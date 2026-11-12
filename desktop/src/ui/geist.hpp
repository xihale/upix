// ui/geist.hpp — Geist design tokens (Vercel) + DPI pixel-snapping helpers.
// Ported from rust-rewrite crates/upix-gui/src/theme.rs + docs/gui/DESIGN.md.
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

inline ImU32 withAlpha(ImU32 col, float alpha) {
    int a = static_cast<int>(static_cast<float>((col >> IM_COL32_A_SHIFT) & 0xFF) * alpha + 0.5f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
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

// 1 physical pixel thickness in logical coordinates (prevents blurry 1.25x hairlines).
inline float hairlinePx() {
    float s = currentDpiScale();
    return (s > 0.0f) ? (1.0f / s) : 1.0f;
}

inline float easeOutExpo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - std::pow(2.0f, -10.0f * t);
}

namespace geist {

enum class Mode { Light, Dark };

inline Mode& mode() {
    static Mode m = Mode::Dark;
    return m;
}

// Geist radius ladder: 4px chips, 6px functional chrome, 12px cards, 16px panels.
namespace radius {
inline constexpr float SM = 4.0f;
inline constexpr float MD = 6.0f;
inline constexpr float LG = 12.0f;
inline constexpr float XL = 16.0f;
} // namespace radius

// Spacing scale — 4px base grid.
namespace space {
inline constexpr float XS = 4.0f;
inline constexpr float SM = 8.0f;
inline constexpr float MD = 12.0f;
inline constexpr float LG = 16.0f;
inline constexpr float XL = 20.0f;
inline constexpr float XXL = 24.0f;
inline constexpr float XXXL = 32.0f;
inline constexpr float SECTION = 24.0f;
inline constexpr float CARD = 16.0f;
} // namespace space

// Type scale (px).
namespace type {
inline constexpr float DISPLAY = 48.0f;
inline constexpr float HEADING = 32.0f;
inline constexpr float SUBHEADING = 20.0f;
inline constexpr float BODY = 13.0f;
inline constexpr float BODY_SMALL = 12.0f;
inline constexpr float LABEL = 11.0f;
inline constexpr float MONO = 12.0f;
inline constexpr float TRACKING_HEADING = -0.4f;
} // namespace type

struct Tokens {
    // surfaces
    ImU32 bg;             // canvas
    ImU32 recessed;       // segmented track, one step deeper than canvas
    ImU32 lowest;         // wells sunk into the canvas (stage well)
    ImU32 surface;        // elevated face: cards, inputs
    ImU32 container;      // subtle grey face (bg-subtle)
    ImU32 high;           // row hover / static selection wash
    ImU32 highest;        // hover step of `high`
    ImU32 segThumb;       // segmented selected block / nav capsule
    // lines
    ImU32 outline;        // hairline: every card, input, divider
    ImU32 outlineInput;   // input / hover border
    // ink
    ImU32 ink;            // headings, high-emphasis text, primary fill
    ImU32 body;           // secondary text / labels
    ImU32 mute;           // dim text, icons
    ImU32 faint;          // placeholder / registration marks
    // primary action (solid ink, NOT colored)
    ImU32 primary;
    ImU32 primaryHover;
    ImU32 primaryPress;
    ImU32 onPrimary;
    // the single accent
    ImU32 accent;         // Vercel blue
    ImU32 accentHover;
    ImU32 accentSoft;     // selected-row / active-tag fill
    ImU32 accentBorder;
    // signals
    ImU32 error;
    ImU32 errorSoft;
    ImU32 errorBorder;
    ImU32 warning;        // star gold
    // scene dots (Geist accents, tiny circles only)
    ImU32 sceneAnime;
    ImU32 scenePhoto;
    ImU32 sceneGeneral;
    ImU32 sceneRestore;
    // misc
    ImU32 scrim;
    ImU32 popupBg;
    ImU32 wellBg;
    ImU32 qrPaper;
    ImU32 qrInk;
    // extras
    ImU32 segBorder;
    ImU32 errorHover;
    ImU32 errorBadgeFg;
    // stage / hud / tags / registration marks
    ImU32 regMark;
    ImU32 checkerA;
    ImU32 checkerB;
    ImU32 hudBg;
    ImU32 hudBorder;
    ImU32 tagBg;
};

inline const Tokens& tokens() {
    static const Tokens dark = {
        /*bg*/            hex(0x000000),
        /*recessed*/      hex(0x111113),
        /*lowest*/        hex(0x0A0A0A),
        /*surface*/       hex(0x0A0A0A),
        /*container*/     hex(0x141414),
        /*high*/          hex(0x1C1C1C),
        /*highest*/       hex(0x242424),
        /*segThumb*/      hex(0x27272A),
        /*outline*/       hex(0x262626),
        /*outlineInput*/  hex(0x404040),
        /*ink*/           hex(0xEDEDED),
        /*body*/          hex(0xA1A1A1),
        /*mute*/          hex(0x8F8F8F),
        /*faint*/         hex(0x6F6F6F),
        /*primary*/       hex(0xEDEDED),
        /*primaryHover*/  hex(0xC7C7C7),
        /*primaryPress*/  hex(0x999999),
        /*onPrimary*/     hex(0x000000),
        /*accent*/        hex(0x3291FF),
        /*accentHover*/   hex(0x66B2FF),
        /*accentSoft*/    hex(0x3291FF, 0x38),
        /*accentBorder*/  hex(0x3291FF, 0x59),
        /*error*/         hex(0xE5484D),
        /*errorSoft*/     hex(0xE5484D, 0x24),
        /*errorBorder*/   hex(0xE5484D, 0x61),
        /*warning*/       hex(0xF5A623),
        /*sceneAnime*/    hex(0xA579E8),
        /*scenePhoto*/    hex(0xF7B955),
        /*sceneGeneral*/  hex(0x3291FF),
        /*sceneRestore*/  hex(0xF06FA3),
        /*scrim*/         hex(0x000000),
        /*popupBg*/       hex(0x141414),
        /*wellBg*/        hex(0x050505),
        /*qrPaper*/       hex(0xFFFFFF),
        /*qrInk*/         hex(0x171717),
        /*segBorder*/     hex(0x1F1F22),
        /*errorHover*/    hex(0xE5484D, 0x33),
        /*errorBadgeFg*/  hex(0xFF6169),
        /*regMark*/       hex(0x343440),
        /*checkerA*/      hex(0x161618),
        /*checkerB*/      hex(0x1E1E21),
        /*hudBg*/         hex(0x141414, 0xEB),
        /*hudBorder*/     hex(0x2E2E32),
        /*tagBg*/         hex(0x1E1E22),
    };
    static const Tokens light = {
        /*bg*/            hex(0xFAFAFA),
        /*recessed*/      hex(0xECECEC),
        /*lowest*/        hex(0xF2F2F2),
        /*surface*/       hex(0xFFFFFF),
        /*container*/     hex(0xF2F2F2),
        /*high*/          hex(0xEBEBEB),
        /*highest*/       hex(0xE0E0E0),
        /*segThumb*/      hex(0xFFFFFF),
        /*outline*/       hex(0xEBEBEB),
        /*outlineInput*/  hex(0xD4D4D4),
        /*ink*/           hex(0x171717),
        /*body*/          hex(0x4D4D4D),
        /*mute*/          hex(0x8F8F8F),
        /*faint*/         hex(0xA1A1A1),
        /*primary*/       hex(0x171717),
        /*primaryHover*/  hex(0x383838),
        /*primaryPress*/  hex(0x666666),
        /*onPrimary*/     hex(0xFFFFFF),
        /*accent*/        hex(0x0070F3),
        /*accentHover*/   hex(0x0761D1),
        /*accentSoft*/    hex(0xD3E5FF),
        /*accentBorder*/  hex(0x0070F3, 0x59),
        /*error*/         hex(0xEE0000),
        /*errorSoft*/     hex(0xEE0000, 0x0F),
        /*errorBorder*/   hex(0xEE0000, 0x4D),
        /*warning*/       hex(0xF5A623),
        /*sceneAnime*/    hex(0x7928CA),
        /*scenePhoto*/    hex(0xF5A623),
        /*sceneGeneral*/  hex(0x0070F3),
        /*sceneRestore*/  hex(0xEB367F),
        /*scrim*/         hex(0x000000),
        /*popupBg*/       hex(0xFFFFFF),
        /*wellBg*/        hex(0xF2F2F2),
        /*qrPaper*/       hex(0xFFFFFF),
        /*qrInk*/         hex(0x171717),
        /*segBorder*/     hex(0x000000, 0x0F),
        /*errorHover*/    hex(0xEE0000, 0x1A),
        /*errorBadgeFg*/  hex(0xEE0000),
        /*regMark*/       hex(0xCCCCCC),
        /*checkerA*/      hex(0xEAEAEA),
        /*checkerB*/      hex(0xF6F6F6),
        /*hudBg*/         hex(0xFFFFFF, 0xEB),
        /*hudBorder*/     hex(0xD4D4D4),
        /*tagBg*/         hex(0xEBEBEB),
    };
    return mode() == Mode::Dark ? dark : light;
}

} // namespace geist
} // namespace upix::ui
