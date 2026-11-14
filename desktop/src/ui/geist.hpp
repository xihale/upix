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
inline constexpr float PAGE_TITLE = 28.0f;
inline constexpr float SUBHEADING = 23.0f;
inline constexpr float BODY = 18.0f;
inline constexpr float BODY_SMALL = 15.0f;
inline constexpr float LABEL = 16.0f;
inline constexpr float MONO = 15.0f;
inline constexpr float EYEBROW = 13.0f;
inline constexpr float TRACKING_HEADING = -0.4f;
} // namespace type

struct Tokens {
    // surfaces
    ImU32 bg;             // canvas (--bg)
    ImU32 panelBg;        // docked panel / filmstrip (--bg-panel)
    ImU32 recessed;       // subtle recessed surface (--bg-subtle)
    ImU32 lowest;         // wells sunk into the canvas (--bg-well)
    ImU32 surface;        // elevated face: cards, inputs (--bg-elevated)
    ImU32 container;      // subtle grey face (--bg-subtle)
    ImU32 high;           // row hover / static selection wash
    ImU32 highest;        // hover step of `high`
    ImU32 btnGhostHover;  // quiet button hover wash
    ImU32 segTrack;       // segmented control track
    ImU32 segThumb;       // segmented selected block / nav capsule
    // lines
    ImU32 outline;        // hairline: every card, input, divider (--rule)
    ImU32 outlineStrong;  // stronger hairline (--rule-strong)
    ImU32 outlineInput;   // input / hover border
    // ink
    ImU32 ink;            // headings, high-emphasis text, primary fill
    ImU32 inkHover;       // primary ink hover step
    ImU32 inkPress;       // primary ink active/press step
    ImU32 onInk;          // text/icon on solid ink fill
    ImU32 body;           // secondary text / labels (--ink-secondary)
    ImU32 mute;           // dim text, icons (--ink-muted)
    ImU32 faint;          // placeholder / disabled ink (--ink-disabled)
    ImU32 ctaDisabledBg;  // disabled CTA background
    ImU32 ctaDisabledFg;  // disabled CTA foreground
    // primary action aliases (solid ink, NOT colored)
    ImU32 primary;
    ImU32 primaryHover;
    ImU32 primaryPress;
    ImU32 onPrimary;
    // the single accent
    ImU32 accent;         // Vercel blue
    ImU32 accentHover;
    ImU32 accentSoft;     // selected-row / active-tag fill
    ImU32 accentBorder;
    // signals & danger
    ImU32 error;
    ImU32 errorHover;     // danger text hover (--danger-deep)
    ImU32 errorDeep;      // alias of errorHover
    ImU32 dangerBg;       // quiet danger fill
    ImU32 dangerBgHover;  // quiet danger hover fill
    ImU32 dangerBgPress;  // quiet danger press fill
    ImU32 errorSoft;      // alias of dangerBg
    ImU32 errorBorder;
    ImU32 errorPress;     // alias of dangerBgPress
    ImU32 errorBadgeFg;
    ImU32 warning;        // star gold
    // scene dots (Geist accents, tiny circles only)
    ImU32 sceneAnime;
    ImU32 scenePhoto;
    ImU32 sceneGeneral;
    ImU32 sceneRestore;
    // misc & controls
    ImU32 scrim;
    ImU32 popupBg;
    ImU32 wellBg;
    ImU32 qrPaper;
    ImU32 qrInk;
    ImU32 segBorder;
    ImU32 segFg;
    ImU32 segFgMuted;
    ImU32 segThumbRim;
    ImU32 dropzoneBg;
    ImU32 dropzoneBorder;
    ImU32 switchKnobOff;
    // stage / hud / tags / registration marks
    ImU32 regMark;
    ImU32 checkerA;
    ImU32 checkerB;
    ImU32 hudBg;
    ImU32 hudBorder;
    ImU32 tagBg;
    // overlay alphas
    float veilAlpha;
    float scrimAlpha;
    float curtainHaloAlpha;
};

inline const Tokens& tokens() {
    static const Tokens dark = {
        /*bg*/              hex(0x000000),
        /*panelBg*/         hex(0x0A0A0A),
        /*recessed*/        hex(0x141414),
        /*lowest*/          hex(0x000000),
        /*surface*/         hex(0x111111),
        /*container*/       hex(0x141414),
        /*high*/            hex(0x1C1C1C),
        /*highest*/         hex(0x242424),
        /*btnGhostHover*/   hex(0x141414),
        /*segTrack*/        hex(0x000000),
        /*segThumb*/        hex(0x1F1F1F),
        /*outline*/         hex(0x1F1F1F),
        /*outlineStrong*/   hex(0x333333),
        /*outlineInput*/    hex(0x333333),
        /*ink*/             hex(0xEDEDED),
        /*inkHover*/        hex(0xD4D4D4),
        /*inkPress*/        hex(0xA3A3A3),
        /*onInk*/           hex(0x000000),
        /*body*/            hex(0xA1A1A1),
        /*mute*/            hex(0x737373),
        /*faint*/           hex(0x525252),
        /*ctaDisabledBg*/   hex(0x141414),
        /*ctaDisabledFg*/   hex(0x666666),
        /*primary*/         hex(0xEDEDED),
        /*primaryHover*/    hex(0xD4D4D4),
        /*primaryPress*/    hex(0xA3A3A3),
        /*onPrimary*/       hex(0x000000),
        /*accent*/          hex(0x3291FF),
        /*accentHover*/     hex(0x66B2FF),
        /*accentSoft*/      rgba(50, 145, 255, 51),
        /*accentBorder*/    hex(0x3291FF, 0x59),
        /*error*/           hex(0xE5484D),
        /*errorHover*/      hex(0xFF6169),
        /*errorDeep*/       hex(0xFF6169),
        /*dangerBg*/        rgba(229, 72, 77, 36),
        /*dangerBgHover*/   rgba(229, 72, 77, 61),
        /*dangerBgPress*/   rgba(229, 72, 77, 82),
        /*errorSoft*/       rgba(229, 72, 77, 36),
        /*errorBorder*/     hex(0xE5484D, 0x61),
        /*errorPress*/      rgba(229, 72, 77, 82),
        /*errorBadgeFg*/    hex(0xFF6169),
        /*warning*/         hex(0xF5A623),
        /*sceneAnime*/      hex(0xA579E8),
        /*scenePhoto*/      hex(0xF7B955),
        /*sceneGeneral*/    hex(0x3291FF),
        /*sceneRestore*/    hex(0xF06FA3),
        /*scrim*/           hex(0x000000),
        /*popupBg*/         hex(0x111111),
        /*wellBg*/          hex(0x000000),
        /*qrPaper*/         hex(0xFFFFFF),
        /*qrInk*/           hex(0x171717),
        /*segBorder*/       hex(0x242424),
        /*segFg*/           hex(0xFFFFFF),
        /*segFgMuted*/      hex(0x888888),
        /*segThumbRim*/     rgba(255, 255, 255, 31),
        /*dropzoneBg*/      hex(0x070707),
        /*dropzoneBorder*/  hex(0x262626),
        /*switchKnobOff*/   hex(0xD4D4D4),
        /*regMark*/         hex(0x404040),
        /*checkerA*/        hex(0x161618),
        /*checkerB*/        hex(0x1E1E21),
        /*hudBg*/           hex(0x141414, 0xEB),
        /*hudBorder*/       hex(0x2E2E32),
        /*tagBg*/           hex(0x1F1F1F),
        /*veilAlpha*/       0.82f,
        /*scrimAlpha*/      0.72f,
        /*curtainHaloAlpha*/0.72f,
    };
    static const Tokens light = {
        /*bg*/              hex(0xFAFAFA),
        /*panelBg*/         hex(0xFFFFFF),
        /*recessed*/        hex(0xF4F4F4),
        /*lowest*/          hex(0xFAFAFA),
        /*surface*/         hex(0xFFFFFF),
        /*container*/       hex(0xF4F4F4),
        /*high*/            hex(0xEBEBEB),
        /*highest*/         hex(0xE0E0E0),
        /*btnGhostHover*/   hex(0xF4F4F4),
        /*segTrack*/        hex(0xF5F5F5),
        /*segThumb*/        hex(0xFFFFFF),
        /*outline*/         hex(0xEAEAEA),
        /*outlineStrong*/   hex(0xD4D4D4),
        /*outlineInput*/    hex(0xD4D4D4),
        /*ink*/             hex(0x171717),
        /*inkHover*/        hex(0x383838),
        /*inkPress*/        hex(0x666666),
        /*onInk*/           hex(0xFFFFFF),
        /*body*/            hex(0x525252),
        /*mute*/            hex(0x737373),
        /*faint*/           hex(0xA3A3A3),
        /*ctaDisabledBg*/   hex(0xF3F3F3),
        /*ctaDisabledFg*/   hex(0x999999),
        /*primary*/         hex(0x171717),
        /*primaryHover*/    hex(0x383838),
        /*primaryPress*/    hex(0x666666),
        /*onPrimary*/       hex(0xFFFFFF),
        /*accent*/          hex(0x0070F3),
        /*accentHover*/     hex(0x0761D1),
        /*accentSoft*/      hex(0xD3E5FF),
        /*accentBorder*/    hex(0x0070F3, 0x59),
        /*error*/           hex(0xEE0000),
        /*errorHover*/      hex(0xC50000),
        /*errorDeep*/       hex(0xC50000),
        /*dangerBg*/        rgba(238, 0, 0, 15),
        /*dangerBgHover*/   rgba(238, 0, 0, 31),
        /*dangerBgPress*/   rgba(238, 0, 0, 46),
        /*errorSoft*/       rgba(238, 0, 0, 15),
        /*errorBorder*/     hex(0xEE0000, 0x4D),
        /*errorPress*/      rgba(238, 0, 0, 46),
        /*errorBadgeFg*/    hex(0xEE0000),
        /*warning*/         hex(0xF5A623),
        /*sceneAnime*/      hex(0x7928CA),
        /*scenePhoto*/      hex(0xF5A623),
        /*sceneGeneral*/    hex(0x0070F3),
        /*sceneRestore*/    hex(0xEB367F),
        /*scrim*/           hex(0x000000),
        /*popupBg*/         hex(0xFFFFFF),
        /*wellBg*/          hex(0xFAFAFA),
        /*qrPaper*/         hex(0xFFFFFF),
        /*qrInk*/           hex(0x171717),
        /*segBorder*/       hex(0xE6E6E6),
        /*segFg*/           hex(0x171717),
        /*segFgMuted*/      hex(0x666666),
        /*segThumbRim*/     rgba(0, 0, 0, 15),
        /*dropzoneBg*/      hex(0xFFFFFF),
        /*dropzoneBorder*/  hex(0xDCDCDC),
        /*switchKnobOff*/   hex(0xFFFFFF),
        /*regMark*/         hex(0xD4D4D4),
        /*checkerA*/        hex(0xEAEAEA),
        /*checkerB*/        hex(0xF6F6F6),
        /*hudBg*/           hex(0xFFFFFF, 0xEB),
        /*hudBorder*/       hex(0xD4D4D4),
        /*tagBg*/           hex(0xEBEBEB),
        /*veilAlpha*/       0.82f,
        /*scrimAlpha*/      0.48f,
        /*curtainHaloAlpha*/0.72f,
    };
    return mode() == Mode::Dark ? dark : light;
}

} // namespace geist
} // namespace upix::ui
