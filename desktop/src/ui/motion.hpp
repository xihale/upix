// ui/motion.hpp — spring & easing primitives for the Geist motion system.
// Ported from rust-rewrite crates/upix-gui (theme::motion + nsh AnimatedValue).
//
// HARD RULE (docs/gui/DESIGN.md §1.3): no entrance animations. Page switches,
// overlays, and newly mounted controls start at their target on frame 1.
#pragma once

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace upix::ui {
namespace motion {

// Duration tokens (ms).
inline constexpr float INSTANT = 120.0f;
inline constexpr float FAST = 180.0f;
inline constexpr float BASE = 240.0f;
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

inline bool& reduceMotion() {
    static bool r = false;
    return r;
}

// Deterministic headless clock override for unit tests and --audit-motion.
// When headlessMode() is false (default), currentFrame() and currentDt() query ImGui.
namespace detail {

inline bool& headlessMode() {
    static bool h = false;
    return h;
}
inline int& headlessFrame() {
    static int f = 0;
    return f;
}
inline float& headlessDt() {
    static float dt = 1.0f / 60.0f;
    return dt;
}
inline std::uint64_t& epoch() {
    static std::uint64_t e = 1;
    return e;
}

inline int currentFrame() {
    if (headlessMode()) return headlessFrame();
    return ImGui::GetCurrentContext() ? ImGui::GetFrameCount() : headlessFrame();
}

inline float currentDt() {
    if (headlessMode()) return headlessDt();
    return ImGui::GetCurrentContext() ? ImGui::GetIO().DeltaTime : headlessDt();
}

template <typename Map>
inline void pruneStore(Map& store, int frame, int& lastPruneFrame, std::uint64_t& storeEpoch) {
    const std::uint64_t curEpoch = epoch();
    if (storeEpoch != curEpoch) {
        store.clear();
        storeEpoch = curEpoch;
        lastPruneFrame = frame;
        return;
    }
    if (frame - lastPruneFrame < 120) return;
    lastPruneFrame = frame;
    for (auto it = store.begin(); it != store.end();) {
        if (it->second.lastFrame + 600 < frame) {
            it = store.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace detail

inline void beginHeadlessMotion(int startFrame = 0) {
    detail::headlessMode() = true;
    detail::headlessFrame() = startFrame;
    detail::headlessDt() = 1.0f / 60.0f;
    ++detail::epoch();
}

inline void endHeadlessMotion() {
    detail::headlessMode() = false;
}

inline void stepMotionFrame(float dt = 1.0f / 60.0f) {
    ++detail::headlessFrame();
    detail::headlessDt() = dt;
}

inline void resetMotionState() {
    ++detail::epoch();
}

// Soft landing curve for press release: cubic-bezier(.22, 1, .36, 1).
inline float settle(float t) {
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    const float cx = 3.0f * 0.22f, bx = 3.0f * (0.36f - 0.22f) - cx, ax = 1.0f - cx - bx;
    const float cy = 3.0f * 1.0f, by = 3.0f * (1.0f - 1.0f) - cy, ay = 1.0f - cy - by;
    float x = t;
    for (int i = 0; i < 6; ++i) {
        const float xf = ((ax * x + bx) * x + cx) * x;
        const float deriv = (3.0f * ax * x + 2.0f * bx) * x + cx;
        if (std::fabs(deriv) < 1.0e-5f) break;
        x = std::clamp(x - (xf - t) / deriv, 0.0f, 1.0f);
    }
    return ((ay * x + by) * x + cy) * x;
}

// One-dimensional damped spring with per-ID storage for immediate mode.
struct Spring1D {
    float value = 0.0f;
    float velocity = 0.0f;
    int lastFrame = -1;
};

// Closed-form step of x'' = -k x - c v (mass 1).
inline float advance(Spring1D& s, float target, const SpringParams& p, float dt) {
    if (reduceMotion() || !(dt > 0.0f) || !(p.stiffness > 0.0f)) {
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

inline SpringParams fitted(float responseMs, float zeta) {
    const float dur = responseMs < 1.0f ? 0.001f : responseMs / 1000.0f;
    const float z = zeta < 0.05f ? 0.05f : zeta > 1.0f ? 1.0f : zeta;
    constexpr float kSettle = 6.907755f; // ln(1000)
    const float omega = kSettle / (z * dur);
    return SpringParams{omega * omega, 2.0f * z * omega};
}

inline SpringParams layoutSpring() { return fitted(260.0f, 0.9f); }
inline SpringParams selectionParams(float responseMs = 250.0f) { return fitted(responseMs, 0.86f); }

namespace detail {

inline auto& approachStore() {
    static std::unordered_map<ImGuiID, Spring1D> store;
    return store;
}
inline auto& springStore() {
    static std::unordered_map<ImGuiID, Spring1D> store;
    return store;
}

} // namespace detail

inline size_t activeMotionSlots() {
    return detail::approachStore().size() + detail::springStore().size();
}

// Duration ease (color, divider fade). First sight uses `defaultValue`.
inline float approach(ImGuiID id, float target, float durationMs, float defaultValue) {
    static int lastPrune = -1;
    static std::uint64_t storeEpoch = 0;
    const int frame = detail::currentFrame();
    auto& store = detail::approachStore();
    detail::pruneStore(store, frame, lastPrune, storeEpoch);

    Spring1D& s = store[id];
    if (s.lastFrame < 0 || s.lastFrame + 120 < frame) {
        s.value = defaultValue;
        s.velocity = 0.0f;
    }
    s.lastFrame = frame;
    if (reduceMotion() || !(durationMs > 0.0f)) {
        s.value = target;
        return s.value;
    }
    float dt = detail::currentDt();
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.05f) dt = 0.05f;
    const float tau = (durationMs / 1000.0f) / 3.0f;
    const float k = 1.0f - std::exp(-dt / tau);
    s.value += (target - s.value) * k;
    if (std::fabs(s.value - target) < 1.0e-3f) s.value = target;
    return s.value;
}

// Generic per-ID spring toward `target`. First sight seeds at `defaultValue`
// (always pass `target` when zero entrance motion is required).
inline float springTo(ImGuiID id, float target, const SpringParams& p, float defaultValue) {
    static int lastPrune = -1;
    static std::uint64_t storeEpoch = 0;
    const int frame = detail::currentFrame();
    auto& store = detail::springStore();
    detail::pruneStore(store, frame, lastPrune, storeEpoch);

    Spring1D& s = store[id];
    if (s.lastFrame < 0 || s.lastFrame + 120 < frame) {
        s.value = defaultValue;
        s.velocity = 0.0f;
    }
    s.lastFrame = frame;
    advance(s, target, p, detail::currentDt());
    return s.value;
}

// Target-seeded spring helper convenience (never animates on mount).
inline float followSpring(ImGuiID id, float target, const SpringParams& p) {
    return springTo(id, target, p, target);
}

// Press rebound for a control. `pressed` = active this frame.
inline float pressScale(ImGuiID id, bool pressed, float amount = PRESS_SCALE) {
    return springTo(id, pressed ? amount : 1.0f, PRESS, 1.0f);
}

inline ImU32 wash(ImU32 base, ImU32 over, bool hovered) {
    return hovered ? over : base;
}

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
