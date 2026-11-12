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

#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace upix::ui {
namespace motion {

// Duration tokens (ms). Never write raw numbers at call sites.
inline constexpr float INSTANT = 120.0f; // hover wash, color changes
inline constexpr float FAST = 180.0f;    // presses, control state
inline constexpr float BASE = 240.0f;    // selection settle, state-color moves

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

// Advance `spring` toward `target` by dt; frame-rate independent semi-implicit Euler.
inline float advance(Spring1D& s, float target, const SpringParams& p, float dt) {
    if (dt <= 0.0f) { s.value = target; return s.value; }
    // clamp large frame gaps (pause, resize stalls) so springs don't explode
    dt = dt > 0.05f ? 0.05f : dt;
    const float a = p.stiffness * (target - s.value) - p.damping * s.velocity;
    s.velocity += a * dt;
    s.value += s.velocity * dt;
    return s.value;
}

// Generic per-ID spring toward `target`. Keep the SAME id for the SAME role;
// the spring remembers its state across frames.
inline float springTo(ImGuiID id, float target, const SpringParams& p, float defaultValue) {
    static std::unordered_map<ImGuiID, Spring1D> store;
    Spring1D& s = store[id];
    const int frame = ImGui::GetFrameCount();
    if (s.lastFrame + 120 < frame) { // dormant for 2s: reset
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
