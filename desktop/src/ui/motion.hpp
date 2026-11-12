// ui/motion.hpp — spring & easing primitives for the Geist motion system.
// Ported from rust-rewrite crates/upix-gui (theme::motion + nsh AnimatedValue).
//
// HARD RULE (docs/gui/DESIGN.md §1.3, user ruling 2026-11-10): no entrance
// animations. Page switches, overlays, list appearance are INSTANT. The only
// motion allowed is direct-manipulation feedback with physical cause:
// press rebound, hover wash, selection slide, switch knob travel, progress.
// If a motion cannot name the gesture that caused it, delete it.
#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace upix::ui {
namespace motion {

// Duration tokens (ms). Never write raw numbers at call sites.
inline constexpr float INSTANT = 120.0f; // hover wash, color changes
inline constexpr float FAST = 180.0f;    // presses, control state
inline constexpr float BASE = 240.0f;    // selection settle, state-color moves
// Hint overlay delay (nsh tooltip). Keyboard focus shows it at once.
inline constexpr float TOOLTIP_DELAY = 500.0f;

// Damped-spring parameter pairs (stiffness / damping).
struct SpringParams {
    float stiffness;
    float damping;
};
inline constexpr SpringParams PRESS{520.0f, 34.0f}; // press rebound: crisp, no wobble
inline constexpr SpringParams PLACE{320.0f, 30.0f}; // switch knob home

// Press scale tokens. Text controls 0.97; pure-icon controls 0.94.
// The main CTA never scales — it sinks 1 whole pixel instead (§2.2).
inline constexpr float PRESS_SCALE = 0.97f;
inline constexpr float PRESS_SCALE_ICON = 0.94f;

// Soft landing curve for press release: cubic-bezier(.22, 1, .36, 1).
inline float settle(float t) {
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    // Bezier(.22,1,.36,1) solved by Newton iterations on x.
    const float cx = 3.0f * 0.22f, bx = 3.0f * (0.36f - 0.22f) - cx, ax = 1.0f - cx - bx;
    const float cy = 3.0f * 1.0f, by = 3.0f * (1.0f - 1.0f) - cy, ay = 1.0f - cy - by;
    float x = t;
    for (int i = 0; i < 6; ++i) {
        const float xf = ((ax * x + bx) * x + cx) * x;
        const float dx = 1.0f / ((3.0f * ax * x + 2.0f * bx) * x + cx);
        x = (x - (xf - t) * dx) * (x >= 0.0f && x <= 1.0f ? 1.0f : 1.0f);
    }
    return ((ay * x + by) * x + cy) * x;
}

// One-dimensional damped spring with per-ID storage for immediate mode.
struct Spring1D {
    float value = 1.0f;
    float velocity = 0.0f;
    int lastFrame = -1;
};

// Closed-form step of x'' = -k x - c v (mass 1). Semi-implicit Euler rings
// on large dt; the analytic under/critical/over-damped step settles cleanly.
inline float advance(Spring1D& s, float target, const SpringParams& p, float dt) {
    if (!(dt > 0.0f) || !(p.stiffness > 0.0f)) {
        s.value = target;
        s.velocity = 0.0f;
        return s.value;
    }
    dt = dt > 0.05f ? 0.05f : dt;
    const float omega = std::sqrt(p.stiffness);
    const float zeta = p.damping / (2.0f * omega);
    float x = s.value - target;
    float v = s.velocity;
    if (zeta > 0.999f && zeta < 1.001f) {
        const float e = std::exp(-omega * dt);
        const float A = x;
        const float B = v + omega * x;
        const float xt = (A + B * dt) * e;
        v = (B - omega * (A + B * dt)) * e;
        x = xt;
    } else if (zeta < 1.0f) {
        const float beta = zeta * omega;
        const float wd = omega * std::sqrt(std::max(0.0f, 1.0f - zeta * zeta));
        const float e = std::exp(-beta * dt);
        const float A = x;
        const float B = (wd > 1.0e-4f) ? (v + beta * x) / wd : 0.0f;
        const float c = std::cos(wd * dt);
        const float sn = std::sin(wd * dt);
        x = e * (A * c + B * sn);
        v = -beta * x + e * (-A * wd * sn + B * wd * c);
    } else {
        const float disc = std::sqrt(std::max(0.0f, zeta * zeta - 1.0f));
        const float r1 = -omega * (zeta - disc);
        const float r2 = -omega * (zeta + disc);
        const float denom = r1 - r2;
        const float C1 = std::fabs(denom) > 1.0e-6f ? (v - r2 * x) / denom : x;
        const float C2 = x - C1;
        const float e1 = std::exp(r1 * dt);
        const float e2 = std::exp(r2 * dt);
        x = C1 * e1 + C2 * e2;
        v = C1 * r1 * e1 + C2 * r2 * e2;
    }
    if (!std::isfinite(x) || !std::isfinite(v) || (std::fabs(x) < 1.0e-3f && std::fabs(v) < 1.0e-2f)) {
        s.value = target;
        s.velocity = 0.0f;
        return s.value;
    }
    s.value = target + x;
    s.velocity = v;
    return s.value;
}

// Duration-fitted spring: settles to ~1/1000 by `responseMs` at damping ratio
// `zeta` (UIKit / nsh `Spring::new(ms).damping_ratio`). Mass 1.
// layout follower: fitted(260, 0.9). Selection pill: fitted(ms, 0.86).
inline SpringParams fitted(float responseMs, float zeta) {
    const float dur = responseMs < 1.0f ? 0.001f : responseMs / 1000.0f;
    const float z = zeta < 0.05f ? 0.05f : zeta > 1.0f ? 1.0f : zeta;
    constexpr float kSettle = 6.907755f; // ln(1000)
    const float omega = kSettle / (z * dur);
    return SpringParams{omega * omega, 2.0f * z * omega};
}

inline SpringParams layoutSpring() { return fitted(260.0f, 0.9f); }
inline SpringParams selectionParams(float responseMs = 250.0f) { return fitted(responseMs, 0.86f); }

// Duration ease (color, divider fade). Not a spring — springs are press & knob.
// Reaches ~95% of the way in `durationMs`. First sight uses `defaultValue`
// (pass the target to avoid an entrance).
inline float approach(ImGuiID id, float target, float durationMs, float defaultValue) {
    static std::unordered_map<ImGuiID, Spring1D> store;
    Spring1D& s = store[id];
    const int frame = ImGui::GetFrameCount();
    if (s.lastFrame < 0 || s.lastFrame + 120 < frame) {
        s.value = defaultValue;
        s.velocity = 0.0f;
    }
    s.lastFrame = frame;
    if (!(durationMs > 0.0f)) {
        s.value = target;
        return s.value;
    }
    float dt = ImGui::GetIO().DeltaTime;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    const float tau = (durationMs / 1000.0f) / 3.0f;
    const float k = 1.0f - std::exp(-dt / tau);
    s.value += (target - s.value) * k;
    if (std::fabs(s.value - target) < 1.0e-3f) s.value = target;
    return s.value;
}

// Generic per-ID spring toward `target`. Keep the SAME id for the SAME role;
// the spring remembers its state across frames.
inline float springTo(ImGuiID id, float target, const SpringParams& p, float defaultValue) {
    static std::unordered_map<ImGuiID, Spring1D> store;
    Spring1D& s = store[id];
    const int frame = ImGui::GetFrameCount();
    // First sight and anything dormant ~2s snaps to defaultValue so a newly
    // mounted control does not travel in from Spring1D's 1.0f seed.
    if (s.lastFrame < 0 || s.lastFrame + 120 < frame) {
        s.value = defaultValue;
        s.velocity = 0.0f;
    }
    s.lastFrame = frame;
    advance(s, target, p, ImGui::GetIO().DeltaTime);
    return s.value;
}

// Press rebound for a control. `pressed` = active this frame. Returns the
// scale to draw with. Give each control its own id (ImGui::GetID).
inline float pressScale(ImGuiID id, bool pressed, float amount = PRESS_SCALE) {
    return springTo(id, pressed ? amount : 1.0f, PRESS, 1.0f);
}

// Hover wash with an INSTANT color move (§2 tables: hover is a wash, not a slide).
inline ImU32 wash(ImU32 base, ImU32 over, bool hovered) {
    return hovered ? over : base;
}

// Linear color mix (for BASE-length state-color transitions).
inline ImU32 mix(ImU32 a, ImU32 b, float t) {
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const auto ca = ImGui::ColorConvertU32ToFloat4(a);
    const auto cb = ImGui::ColorConvertU32ToFloat4(b);
    ImVec4 r{ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
             ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t};
    return ImGui::ColorConvertFloat4ToU32(r);
}

} // namespace motion
} // namespace upix::ui
