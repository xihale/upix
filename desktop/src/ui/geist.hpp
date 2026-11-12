// ui/geist.hpp — Geist design tokens (Vercel), ported from rust-rewrite
// crates/upix-gui/src/theme.rs + docs/gui/DESIGN.md + web/vercel/DESIGN.md.
//
// Light: near-white canvas #fafafa carrying near-black ink #171717 — headings,
// primary buttons and hairlines all walk one ink ladder (ink → body → mute →
// faint). Cards are white with a 1px #ebebeb hairline. The only chroma is
// Vercel blue #0070f3, reserved for selection, focus, progress and live
// signals. Dark: pure-black canvas, same ladder inverted.
#pragma once

#include "imgui.h"

#include <cstdint>

namespace upix::ui {
namespace geist {

namespace detail {
inline constexpr ImU32 hx(uint32_t rgb, uint8_t a = 255) {
    return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a);
}
} // namespace detail


enum class Mode { Light, Dark };

inline Mode& mode() { static Mode m = Mode::Dark; return m; }

// Geist radius ladder: 4px chips, 6px functional chrome, 12px cards, 16px panels.
namespace radius {
inline constexpr float SM = 4.0f;
inline constexpr float MD = 6.0f;
inline constexpr float LG = 12.0f;
inline constexpr float XL = 16.0f;
} // namespace radius

// Spacing scale — 4px base grid. Keep whitespace generous: sections breathe at
// 24px, cards pad 16px, related controls sit 8px apart, labels 4px from content.
namespace space {
inline constexpr float XS = 4.0f;
inline constexpr float SM = 8.0f;
inline constexpr float MD = 12.0f;
inline constexpr float LG = 16.0f;
inline constexpr float XL = 20.0f;
inline constexpr float XXL = 24.0f;
inline constexpr float XXXL = 32.0f;
inline constexpr float SECTION = 24.0f; // section gap (breathing room)
inline constexpr float CARD = 16.0f;    // card inner padding
} // namespace space

// Type scale (px). Desktop UI mostly speaks body/label; display sizes exist
// for empty states and page headers.
namespace type {
inline constexpr float DISPLAY = 48.0f;
inline constexpr float HEADING = 32.0f;
inline constexpr float SUBHEADING = 20.0f;
inline constexpr float BODY = 13.0f;
inline constexpr float BODY_SMALL = 12.0f;
inline constexpr float LABEL = 11.0f;   // eyebrows, meta, kbd
inline constexpr float MONO = 12.0f;
inline constexpr float TRACKING_HEADING = -0.4f; // px, apply sparingly
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
    // extras from palette (theme.rs) — same ladder, named so views don't hex them
    ImU32 segBorder;      // segmented track rim
    ImU32 errorHover;     // destructive wash, one step deeper than errorSoft
    ImU32 errorBadgeFg;   // badge ink (dark lifts to #ff6169)
};

inline const Tokens& tokens() {
    static const Tokens dark = {
        /*bg*/            detail::hx(0x000000),
        /*recessed*/      detail::hx(0x111113),
        /*lowest*/        detail::hx(0x0A0A0A),
        /*surface*/       detail::hx(0x0A0A0A),
        /*container*/     detail::hx(0x141414),
        /*high*/          detail::hx(0x1C1C1C),
        /*highest*/       detail::hx(0x242424),
        /*segThumb*/      detail::hx(0x27272A),
        /*outline*/       detail::hx(0x262626),
        /*outlineInput*/  detail::hx(0x404040),
        /*ink*/           detail::hx(0xEDEDED),
        /*body*/          detail::hx(0xA1A1A1),
        /*mute*/          detail::hx(0x8F8F8F),
        /*faint*/         detail::hx(0x6F6F6F),
        /*primary*/       detail::hx(0xEDEDED),
        /*primaryHover*/  detail::hx(0xC7C7C7),
        /*primaryPress*/  detail::hx(0x999999),
        /*onPrimary*/     detail::hx(0x000000),
        /*accent*/        detail::hx(0x3291FF),
        /*accentHover*/   detail::hx(0x66B2FF),
        /*accentSoft*/    detail::hx(0x3291FF, 0x38),
        /*accentBorder*/  detail::hx(0x3291FF, 0x59),
        /*error*/         detail::hx(0xE5484D),
        /*errorSoft*/     detail::hx(0xE5484D, 0x24),
        /*errorBorder*/   detail::hx(0xE5484D, 0x61),
        /*warning*/       detail::hx(0xF5A623),
        /*sceneAnime*/    detail::hx(0xA579E8),
        /*scenePhoto*/    detail::hx(0xF7B955),
        /*sceneGeneral*/  detail::hx(0x3291FF),
        /*sceneRestore*/  detail::hx(0xF06FA3),
        /*scrim*/         detail::hx(0x000000),
        /*popupBg*/       detail::hx(0x141414),
        /*wellBg*/        detail::hx(0x050505),
        /*qrPaper*/       detail::hx(0xFFFFFF),
        /*qrInk*/         detail::hx(0x171717),
        /*segBorder*/     detail::hx(0x1F1F22),
        /*errorHover*/    detail::hx(0xE5484D, 0x33),
        /*errorBadgeFg*/  detail::hx(0xFF6169),
    };
    static const Tokens light = {
        /*bg*/            detail::hx(0xFAFAFA),
        /*recessed*/      detail::hx(0xECECEC),
        /*lowest*/        detail::hx(0xF2F2F2),
        /*surface*/       detail::hx(0xFFFFFF),
        /*container*/     detail::hx(0xF2F2F2),
        /*high*/          detail::hx(0xEBEBEB),
        /*highest*/       detail::hx(0xE0E0E0),
        /*segThumb*/      detail::hx(0xFFFFFF),
        /*outline*/       detail::hx(0xEBEBEB),
        /*outlineInput*/  detail::hx(0xD4D4D4),
        /*ink*/           detail::hx(0x171717),
        /*body*/          detail::hx(0x4D4D4D),
        /*mute*/          detail::hx(0x8F8F8F),
        /*faint*/         detail::hx(0xA1A1A1),
        /*primary*/       detail::hx(0x171717),
        /*primaryHover*/  detail::hx(0x383838),
        /*primaryPress*/  detail::hx(0x666666),
        /*onPrimary*/     detail::hx(0xFFFFFF),
        /*accent*/        detail::hx(0x0070F3),
        /*accentHover*/   detail::hx(0x0761D1),
        /*accentSoft*/    detail::hx(0xD3E5FF),
        /*accentBorder*/  detail::hx(0x0070F3, 0x59),
        /*error*/         detail::hx(0xEE0000),
        /*errorSoft*/     detail::hx(0xEE0000, 0x0F),
        /*errorBorder*/   detail::hx(0xEE0000, 0x4D),
        /*warning*/       detail::hx(0xF5A623),
        /*sceneAnime*/    detail::hx(0x7928CA),
        /*scenePhoto*/    detail::hx(0xF5A623),
        /*sceneGeneral*/  detail::hx(0x0070F3),
        /*sceneRestore*/  detail::hx(0xEB367F),
        /*scrim*/         detail::hx(0x000000),
        /*popupBg*/       detail::hx(0xFFFFFF),
        /*wellBg*/        detail::hx(0xF2F2F2),
        /*qrPaper*/       detail::hx(0xFFFFFF),
        /*qrInk*/         detail::hx(0x171717),
        /*segBorder*/     detail::hx(0x000000, 0x0F),
        /*errorHover*/    detail::hx(0xEE0000, 0x1A),
        /*errorBadgeFg*/  detail::hx(0xEE0000),
    };
    return mode() == Mode::Dark ? dark : light;
}

} // namespace geist
} // namespace upix::ui
