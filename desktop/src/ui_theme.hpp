#pragma once
#include "imgui.h"
#include <cmath>
#include <cstdint>

namespace upix::ui {

inline constexpr ImU32 rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return IM_COL32(r, g, b, a);
}

inline constexpr ImU32 hex(uint32_t rgb, uint8_t a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}

// Physical pixel grid snapping helper for Wayland fractional scaling (e.g. 1.25x, 1.5x)
inline float& currentDpiScale() {
    static float s = 1.0f;
    return s;
}

inline float snapPx(float v) {
    float s = currentDpiScale();
    if (s <= 0.0f) return std::round(v);
    return std::round(v * s) / s;
}

inline ImVec2 snapPt(float x, float y) {
    return ImVec2(snapPx(x), snapPx(y));
}

inline ImVec2 snapPt(ImVec2 p) {
    return ImVec2(snapPx(p.x), snapPx(p.y));
}

// 1 physical pixel thickness in logical coordinates (prevents 1.25px 2-pixel blurry hairlines!)
inline float hairlinePx() {
    float s = currentDpiScale();
    return (s > 0.0f) ? (1.0f / s) : 1.0f;
}

// Figma "Upix · Desktop 当前布局" tokens (Dark / Light)
struct Palette {
    bool dark = true;

    ImU32 bg() const { return dark ? hex(0x131316) : hex(0xF4F3EF); }
    ImU32 recessed() const { return dark ? hex(0x151519) : hex(0xEAE9E4); }
    ImU32 surface() const { return dark ? hex(0x18181C) : hex(0xFFFFFF); }
    ImU32 containerLow() const { return dark ? hex(0x1C1C21) : hex(0xF0EFEB); }
    ImU32 container() const { return dark ? hex(0x202026) : hex(0xECEBE6); }
    ImU32 high() const { return dark ? hex(0x26262C) : hex(0xE3E2DC); }
    ImU32 segSelected() const { return dark ? hex(0x2B2B34) : hex(0xFFFFFF); }
    ImU32 segBorder() const { return dark ? rgba(255, 255, 255, 24) : hex(0xC5C4BC); }
    ImU32 highest() const { return dark ? hex(0x323239) : hex(0xD9D8D1); }
    ImU32 outline() const { return dark ? hex(0x282830) : hex(0xD6D5CD); }
    ImU32 outlineInput() const { return dark ? hex(0x2D2D34) : hex(0xCECDC5); }
    ImU32 outlineStrong() const { return dark ? hex(0x32323C) : hex(0xC5C4BC); }
    ImU32 regMark() const { return dark ? hex(0x343440) : hex(0xBEBDB4); }
    ImU32 onSurface() const { return dark ? hex(0xECECEE) : hex(0x18181B); }
    ImU32 muted() const { return dark ? hex(0xA2A2AC) : hex(0x5E5D57); }
    ImU32 dim() const { return dark ? hex(0x6E6E7A) : hex(0x8A8982); }
    ImU32 primary() const { return dark ? hex(0x7FDAC9) : hex(0x0E6B5F); }
    ImU32 primaryDimBg() const { return dark ? hex(0x152623) : hex(0xDDF2EE); }
    ImU32 primaryDimBorder() const { return dark ? hex(0x244740) : hex(0x9FD5CA); }
    ImU32 toggleOnBg() const { return dark ? hex(0x18302C) : hex(0xC5ECE4); }
    ImU32 toggleOnBorder() const { return dark ? hex(0x2E5E55) : hex(0x85CABB); }
    ImU32 onPrimary() const { return dark ? hex(0x00382F) : hex(0xFFFFFF); }
    ImU32 error() const { return dark ? hex(0xFF7361) : hex(0xC23A2B); }
    ImU32 errorDimBg() const { return dark ? hex(0x1E1416) : hex(0xFCE9E7); }
    ImU32 errorDimBorder() const { return dark ? hex(0x58282C) : hex(0xE8B4AE); }

    ImU32 withAlpha(ImU32 col, float alpha) const {
        uint8_t a = static_cast<uint8_t>(((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
        return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
    }
};

} // namespace upix::ui
