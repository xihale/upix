// ui/controls.hpp — Geist Finish system + wired control primitives.
// Ported from rust-rewrite crates/upix-gui/src/{theme.rs (Finish), ui.rs}.
// Working reference implementations; the foundation agent refines feel.
#pragma once

#include "ui/geist.hpp"
#include "ui/motion.hpp"

#include "imgui.h"

#include <cstdint>

namespace upix::ui {
namespace ctl {

// ---------------------------------------------------------------------------
// Finish — the light and depth of a surface. Geist: hairline first, shadow
// second. Flat fills; a `0 1px 1px` whisper shadow under lifted faces;
// layered low-opacity stacks ONLY for floating panels. No gradients, no
// top highlights, no colored shadows.
// ---------------------------------------------------------------------------
enum class Finish {
    Card,          // a card on the page: white/near-black + hairline + whisper
    Inset,         // sunk into a card: well, track, inspector body (inner shadow)
    Raised,        // a control on a card
    RaisedHover,   // same, under the pointer
    Primary,       // main action fill: solid ink
    PrimaryHover,
    PrimaryPress,
    Solid,         // secondary action: surface fill + hairline
    Pill,          // segmented thumb / nav capsule: brightest face + thumb shadow
    Track,         // switch / slider track
    Knob,          // knob / handle over a track
    Chip,          // tinted capsule rim (badges); fill set by caller
    Menu,          // floating panel: popover / modal / menu (layered stack)
};

struct ShadowSpec {
    float y;
    float spread;   // negative pulls in: lift without edge haze
    ImU32 col;
};

namespace detail {
inline void drawShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    // Blur-free approximation of a soft shadow: 2 stacked low-alpha rects.
    const float sp = s.spread;
    ImVec2 a(p0.x - sp, p0.y - sp + s.y), b(p1.x + sp, p1.y + sp + s.y);
    dl->ChannelsSetCurrent(0);
    dl->AddRectFilled(a, b, s.col, radius + sp);
}
} // namespace detail

// Draw a finished surface. Returns the inset rect to draw content in
// (Primary press may sink the face by 1 whole pixel: no resampling).
inline ImVec4 finish(ImDrawList* dl, ImVec2 p0, ImVec2 p1, Finish f, float radius = geist::radius::MD,
                     bool pressed = false) {
    const auto& t = geist::tokens();
    ImU32 fill = 0, border = 0;
    bool hasBorder = false;
    float sink = 0.0f;
    switch (f) {
    case Finish::Card:
        fill = t.surface; border = t.outline; hasBorder = true;
        detail::drawShadow(dl, p0, p1, radius, {1.0f, 2.0f, geist::mode() == geist::Mode::Dark
                                                     ? hex(0x000000, 0x66) : hex(0x000000, 0x0A)});
        break;
    case Finish::Inset:
        fill = t.wellBg;
        // inner shadow approximated by a slightly darker rim
        dl->AddRect(p0, p1, geist::mode() == geist::Mode::Dark ? hex(0x000000, 0x73) : hex(0x000000, 0x10), radius, 0, 1.0f);
        break;
    case Finish::Raised:
    case Finish::RaisedHover:
        fill = f == Finish::Raised ? t.container : t.high;
        detail::drawShadow(dl, p0, p1, radius, {1.0f, 2.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x66 : 0x0A)});
        break;
    case Finish::Primary:
        fill = pressed ? t.primaryPress : t.primary;
        sink = pressed ? 1.0f : 0.0f;
        detail::drawShadow(dl, p0, p1, radius, {1.0f, 2.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x4D : 0x2E)});
        break;
    case Finish::PrimaryHover: fill = t.primaryHover; break;
    case Finish::PrimaryPress: fill = t.primaryPress; break;
    case Finish::Solid: fill = t.container; border = t.outline; hasBorder = true; break;
    case Finish::Pill:
        fill = t.segThumb;
        detail::drawShadow(dl, p0, p1, radius, {1.0f, 3.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x66 : 0x14)});
        break;
    case Finish::Track: fill = t.recessed; break;
    case Finish::Knob: fill = t.ink; break;
    case Finish::Chip: fill = 0; border = t.outline; hasBorder = true; break;
    case Finish::Menu:
        fill = t.popupBg; border = t.outline; hasBorder = true;
        detail::drawShadow(dl, p0, p1, radius, {2.0f, 3.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x59 : 0x08)});
        detail::drawShadow(dl, p0, p1, radius, {8.0f, 9.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0xB3 : 0x1E)});
        if (geist::mode() == geist::Mode::Dark) dl->AddRect(p0, p1, hex(0xFFFFFF, 0x0F), radius);
        break;
    }
    if (sink > 0.0f) { p0.y += sink; p1.y += sink; }
    if (fill) dl->AddRectFilled(p0, p1, fill, radius);
    if (hasBorder) dl->AddRect(p0, p1, border, radius, 0, 1.0f);
    return ImVec4(p0.x, p0.y, p1.x, p1.y);
}

// Hit-test helper for draw-list controls (no ImGui widgets — views draw direct).
inline bool mouseIn(ImVec2 p0, ImVec2 p1) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    return m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y;
}
inline bool clicked(ImVec2 p0, ImVec2 p1) {
    return mouseIn(p0, p1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// ---------------------------------------------------------------------------
// Quiet ghost chip (h24, px8, radius 6): no fill until hover, muted label.
// Spring press 0.97; hover wash INSTANT; label + role mandatory (§3).
// ---------------------------------------------------------------------------
inline bool ghostButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                        ImVec2 p0, float alignW = 0.0f) {
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geist::type::LABEL, label);
    const float w = alignW > 0.0f ? alignW : ts.x + 2 * geist::space::SM;
    ImVec2 p1(p0.x + w, p0.y + 24.0f);
    const bool hov = mouseIn(p0, p1);
    const float press = motion::pressScale(id, ImGui::IsMouseDown(ImGuiMouseButton_Left) && hov);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    ImVec2 q0(c.x - (p1.x - p0.x) * 0.5f * press, c.y - 12.0f * press);
    ImVec2 q1(c.x + (p1.x - p0.x) * 0.5f * press, c.y + 12.0f * press);
    if (hov) dl->AddRectFilled(q0, q1, t.high, geist::radius::MD);
    drawTextCentered(dl, font, geist::type::LABEL, q0, q1, hov ? t.ink : t.body, label);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Chip button (h24, px8, radius 6): container fill + hairline, medium ink.
inline bool chipButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                       ImVec2 p0, float alignW = 0.0f) {
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geist::type::LABEL, label);
    const float w = alignW > 0.0f ? alignW : ts.x + 2 * geist::space::SM;
    ImVec2 p1(p0.x + w, p0.y + 24.0f);
    const bool hov = mouseIn(p0, p1);
    const float press = motion::pressScale(id, ImGui::IsMouseDown(ImGuiMouseButton_Left) && hov);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    ImVec2 q0(c.x - (p1.x - p0.x) * 0.5f * press, c.y - 12.0f * press);
    ImVec2 q1(c.x + (p1.x - p0.x) * 0.5f * press, c.y + 12.0f * press);
    dl->AddRectFilled(q0, q1, hov ? t.high : t.container, geist::radius::MD);
    dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, 1.0f);
    drawTextCentered(dl, font, geist::type::LABEL, q0, q1, t.body, label);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Main CTA: solid ink, NO scale on press (§2.2) — sinks 1 whole pixel and
// deepens one step along the ink ladder. Primary → PrimaryPress.
// height 36 default; caller decides width. Returns true on release inside.
inline bool primaryCta(ImDrawList* dl, ImGuiID /*id*/, const char* label, ImFont* font,
                       ImVec2 p0, ImVec2 p1) {
    const auto& t = geist::tokens();
    const bool hov = mouseIn(p0, p1);
    const bool pressed = hov && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    Finish f = Finish::Primary;
    if (pressed) f = Finish::PrimaryPress;
    else if (hov) { dl->AddRectFilled(p0, p1, t.primaryHover, geist::radius::MD);
                    detail::drawShadow(dl, p0, p1, geist::radius::MD, {1.0f, 2.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x4D : 0x2E)}); }
    if (f == Finish::PrimaryPress) {
        finish(dl, p0, p1, f, geist::radius::MD, true);
    } else if (f == Finish::Primary) {
        dl->AddRectFilled(p0, p1, t.primary, geist::radius::MD);
        detail::drawShadow(dl, p0, p1, geist::radius::MD, {1.0f, 2.0f, hex(0x000000, geist::mode() == geist::Mode::Dark ? 0x4D : 0x2E)});
    }
    drawTextCentered(dl, font, geist::type::BODY, p0, ImVec2(p1.x, p1.y - (pressed ? 1.0f : 0.0f)),
                     t.onPrimary, label);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Selection pill position for rails / segmented tracks: spring-slides to the
// active slot (LayoutTransition analogue). p.x returns animated offset.
inline float selectionSpring(ImGuiID id, float targetX) {
    return motion::springTo(id, targetX, motion::SpringParams{360.0f, 30.0f}, targetX);
}

// Switch knob horizontal offset (0..1 travel): PLACE spring home (§2.5).
inline float switchKnob(ImGuiID id, bool checked) {
    return motion::springTo(id, checked ? 1.0f : 0.0f, motion::PLACE, checked ? 1.0f : 0.0f);
}

// Progress width follower: layout spring 260ms, ~0.9 damping (§2.2).
inline float progressSpring(ImGuiID id, float fraction01, float width) {
    return motion::springTo(id, fraction01 * width, motion::SpringParams{180.0f, 26.0f}, 0.0f);
}

} // namespace ctl
} // namespace upix::ui
