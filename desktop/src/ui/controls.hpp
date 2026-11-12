// ui/controls.hpp — Geist Finish system + wired control primitives.
// Spec: rust-rewrite crates/upix-gui/src/theme.rs (Finish Look tables) and
// ui.rs (geometry). Shadows copy y / blur / spread / alpha from those tables.
//
// Immediate mode: helpers take an ImDrawList and rects, return true on
// click-release, and keep per-ID spring state in motion.hpp.
// No entrance motion. Hover wash is a snap, not a tween.
#pragma once

#include "ui/geist.hpp"
#include "ui/icons.hpp"
#include "ui/motion.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>

namespace upix::ui {

inline void drawBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 fill, ImU32 border,
                    float radius = 8.0f, ImDrawFlags flags = 0) {
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if ((fill >> IM_COL32_A_SHIFT) > 0) {
        dl->AddRectFilled(p0, p1, fill, radius, flags);
    }
    if ((border >> IM_COL32_A_SHIFT) > 0) {
        dl->AddRect(p0, p1, border, radius, flags, hairlinePx());
    }
}

inline void drawRegistrationMarks(ImDrawList* dl, ImVec2 boxMin, ImVec2 boxMax,
                                  float inset, float armLen, float thickness, ImU32 color) {
    boxMin = snapPt(boxMin);
    boxMax = snapPt(boxMax);
    inset = snapPx(inset);
    armLen = snapPx(armLen);
    thickness = std::max(hairlinePx(), snapPx(thickness));
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMin.y + inset),
                      ImVec2(boxMin.x + inset + armLen, boxMin.y + inset + thickness), color);
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMin.y + inset),
                      ImVec2(boxMin.x + inset + thickness, boxMin.y + inset + armLen), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - armLen, boxMin.y + inset),
                      ImVec2(boxMax.x - inset, boxMin.y + inset + thickness), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - thickness, boxMin.y + inset),
                      ImVec2(boxMax.x - inset, boxMin.y + inset + armLen), color);
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMax.y - inset - thickness),
                      ImVec2(boxMin.x + inset + armLen, boxMax.y - inset), color);
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMax.y - inset - armLen),
                      ImVec2(boxMin.x + inset + thickness, boxMax.y - inset), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - armLen, boxMax.y - inset - thickness),
                      ImVec2(boxMax.x - inset, boxMax.y - inset), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - thickness, boxMax.y - inset - armLen),
                      ImVec2(boxMax.x - inset, boxMax.y - inset), color);
}

inline ImVec2 calcText(ImFont* font, float size, const char* text) {
    if (!font || !text) return ImVec2(0, 0);
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

inline void drawText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text) {
    if (!dl || !font || !text) return;
    dl->AddText(font, size, snapPt(pos), col, text);
}

inline void drawTextCentered(ImDrawList* dl, ImFont* font, float size,
                             ImVec2 rectMin, ImVec2 rectMax, ImU32 col, const char* text) {
    if (!dl || !font || !text) return;
    ImVec2 sz = calcText(font, size, text);
    ImVec2 pos = snapPt(rectMin.x + (rectMax.x - rectMin.x - sz.x) * 0.5f,
                        rectMin.y + (rectMax.y - rectMin.y - sz.y) * 0.5f);
    dl->AddText(font, size, pos, col, text);
}

// Clip text gracefully with "..." when width exceeds `maxWidth`.
inline std::string fitTextEllipsis(ImFont* font, float size, const char* text, float maxWidth) {
    if (!text || !text[0]) return {};
    std::string s(text);
    if (!font || maxWidth <= 8.0f) return s;
    if (calcText(font, size, s.c_str()).x <= maxWidth) return s;
    const float dotsW = calcText(font, size, "...").x;
    if (dotsW >= maxWidth) return "...";
    // Step back by UTF-8 codepoint boundaries.
    while (!s.empty()) {
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
        if (!s.empty()) s.pop_back();
        if (s.empty()) break;
        std::string cand = s + "...";
        if (calcText(font, size, cand.c_str()).x <= maxWidth) return cand;
    }
    return "...";
}

inline void ellipsisText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                         float maxWidth, ImU32 col, const char* text) {
    std::string fitted = fitTextEllipsis(font, size, text, maxWidth);
    drawText(dl, font, size, pos, col, fitted.c_str());
}

struct SegmentOption {
    const char* label;
    const char* value;
};

namespace ctl {

// Geometry from ui.rs. Draw hairlines with hairlinePx(), not the literal 1.
namespace geom {
inline constexpr float GHOST_H = 24.0f;
inline constexpr float GHOST_PX = 8.0f;
inline constexpr float CHIP_R = 6.0f;
inline constexpr float LABEL = 11.0f;
inline constexpr float ROW_MIN_H = 44.0f;
inline constexpr float ROW_PX = 16.0f;
inline constexpr float ROW_PY = 10.0f;
inline constexpr float ROW_GAP = 12.0f;
inline constexpr float ROW_SUB = 11.0f;
inline constexpr float ICON = 20.0f;
inline constexpr float KBD_H = 22.0f;
inline constexpr float KBD_PX = 7.0f;
inline constexpr float KBD_R = 4.0f;
inline constexpr float TOGGLE_W = 44.0f;
inline constexpr float TOGGLE_H = 26.0f;
inline constexpr float TOGGLE_KNOB = 20.0f;
inline constexpr float TOGGLE_TRAVEL = 18.0f; // PLACE spring, px
inline constexpr float SEG_H = 32.0f;
inline constexpr float SEG_PAD = 2.0f;
inline constexpr float SEG_TRACK_R = 8.0f;
inline constexpr float SEG_PILL_R = 6.0f;
inline constexpr float SEG_DIV_H = 12.0f;
inline constexpr float SLIDER_H = 18.0f;
inline constexpr float SLIDER_TRACK = 2.0f;
inline constexpr float SLIDER_KNOB = 13.0f;
inline constexpr float CTA_H = 44.0f;
inline constexpr float PROGRESS_H = 4.0f;
inline constexpr float CARD_R = 12.0f;
inline constexpr float HOVER_WASH = 0.08f;    // nav / rows (§2.1)
inline constexpr float SEG_WASH = 0.06f;      // segmented (§2.2)
inline constexpr float SNAP_FRAC = 0.022f;    // quality slider magnet
} // namespace geom

// ---------------------------------------------------------------------------
// Finish — light and depth of a surface. Values are the Look tables in
// theme.rs: drop(y, blur, alpha), lift(y, blur, spread, alpha), inner(...).
// Hairline first, shadow second. No gradients, no top highlights, no
// colored shadows.
// ---------------------------------------------------------------------------
enum class Finish {
    Card,
    Inset,
    Raised,
    RaisedHover,
    Primary,
    PrimaryHover,
    PrimaryPress,
    Solid,
    Pill,
    Track,
    Knob,
    Chip,
    Glass,
    Menu,
};

// One box-shadow layer. spread < 0 pulls the shadow in (lift without edge haze).
struct ShadowSpec {
    float y = 0.0f;
    float blur = 0.0f;
    float spread = 0.0f;
    ImU32 col = 0;
    bool inset = false;
};

namespace detail {

inline ImU32 withAlpha(ImU32 c, float a) {
    int ai = static_cast<int>(a * 255.0f + 0.5f);
    if (ai < 0) ai = 0;
    if (ai > 255) ai = 255;
    return (c & ~IM_COL32_A_MASK) | (static_cast<ImU32>(ai) << IM_COL32_A_SHIFT);
}

inline ImU32 shade(bool white, float a) {
    return withAlpha(white ? IM_COL32_WHITE : IM_COL32_BLACK, a);
}

// Overlay wash: white on dark, black on light. `coverage` is 0..1 (0.08 = 8%).
inline ImU32 washOverlay(float coverage) {
    return shade(geist::mode() == geist::Mode::Dark, coverage);
}

inline ImGuiID salt(ImGuiID id, int n) {
    uint32_t x = static_cast<uint32_t>(id) + 0x9E3779B9u * static_cast<uint32_t>(n + 1);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Soft outer shadow. Rings are drawn large-to-small so the fringe fades to
// exactly 0 at `blur` (no hard cutoff) and the umbra holds the spec alpha.
inline void drawOuter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    const int a8 = static_cast<int>((s.col >> IM_COL32_A_SHIFT) & 0xFF);
    if (a8 <= 0) return;
    const float alpha = static_cast<float>(a8) / 255.0f;
    const float sp = s.spread;
    ImVec2 a(p0.x - sp, p0.y - sp + s.y);
    ImVec2 b(p1.x + sp, p1.y + sp + s.y);
    const float rad = std::max(0.0f, radius + sp);
    const ImU32 rgb = s.col & ~IM_COL32_A_MASK;
    auto paint = [&](ImVec2 q0, ImVec2 q1, float rr, float layer) {
        int la = static_cast<int>(layer * 255.0f + 0.5f);
        if (la <= 0) return;
        dl->AddRectFilled(q0, q1, rgb | (static_cast<ImU32>(la) << IM_COL32_A_SHIFT), rr);
    };
    if (s.blur < 0.35f) {
        paint(a, b, rad, alpha);
        return;
    }
    int N = static_cast<int>(std::ceil(s.blur * 2.0f));
    if (N < 2) N = 2;
    if (N > 14) N = 14;
    float painted = 0.0f;
    auto layerFor = [&](float target) -> float {
        if (target <= painted + 1.0e-4f) return 0.0f;
        const float layer = (target - painted) / std::max(1.0e-4f, 1.0f - painted);
        painted = target;
        return layer;
    };
    for (int i = N; i >= 1; --i) {
        const float d = s.blur * static_cast<float>(i) / static_cast<float>(N);
        const float u = d / s.blur;
        const float sm = u * u * (3.0f - 2.0f * u); // smoothstep, 0 at the umbra edge
        const float target = alpha * (1.0f - sm);
        const float layer = layerFor(target);
        if (layer <= 0.0f) continue;
        paint(ImVec2(a.x - d, a.y - d), ImVec2(b.x + d, b.y + d), rad + d, layer);
    }
    const float core = layerFor(alpha);
    if (core > 0.0f) paint(a, b, rad, core);
}

// Inner shadow that stays inside the rounded rect (concentric strokes).
// Positive y biases a lip along the top — light comes from above.
inline void drawInner(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    const int a8 = static_cast<int>((s.col >> IM_COL32_A_SHIFT) & 0xFF);
    if (a8 <= 0) return;
    const float alpha = static_cast<float>(a8) / 255.0f;
    const ImU32 rgb = s.col & ~IM_COL32_A_MASK;
    auto colA = [&](float a) -> ImU32 {
        int la = static_cast<int>(a * 255.0f + 0.5f);
        if (la <= 0) return 0;
        return rgb | (static_cast<ImU32>(la) << IM_COL32_A_SHIFT);
    };
    dl->PushClipRect(p0, p1, true);
    const float reach = std::max(s.blur, hairlinePx());
    int N = s.blur < 0.75f ? 1 : static_cast<int>(std::ceil(s.blur));
    if (N > 6) N = 6;
    const float th = std::max(reach / static_cast<float>(N), hairlinePx());
    for (int i = 0; i < N; ++i) {
        const float d = (static_cast<float>(i) + 0.5f) * th;
        if (d >= (p1.x - p0.x) * 0.5f) break;
        const float u = d / std::max(reach, 0.001f);
        const float a = alpha * (1.0f - std::min(1.0f, u * u));
        const ImU32 c = colA(a);
        if (!c) continue;
        const float rr = std::max(0.0f, radius - d);
        dl->AddRect(ImVec2(p0.x + d, p0.y + d), ImVec2(p1.x - d, p1.y - d), c, rr, 0, th);
    }
    // Top lip: the +y offset of an inset shadow lands on the top edge.
    if (s.y > 0.0f) {
        const float lip = std::max(hairlinePx(), std::min(s.y + s.blur * 0.5f, 3.0f));
        const float insetX = std::min(radius, (p1.x - p0.x) * 0.5f);
        const ImU32 c = colA(alpha);
        if (c) {
            dl->AddRectFilled(ImVec2(p0.x + insetX * 0.35f, p0.y),
                              ImVec2(p1.x - insetX * 0.35f, p0.y + lip), c);
        }
    }
    dl->PopClipRect();
}

inline void drawShadow(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    if (s.inset) drawInner(dl, p0, p1, radius, s);
    else drawOuter(dl, p0, p1, radius, s);
}

inline void focusRing(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, bool focused) {
    if (!focused) return;
    dl->AddRect(p0, p1, geist::tokens().accent, radius, 0, hairlinePx());
}

struct Hit {
    bool hovered = false;
    bool held = false;
    bool clicked = false;
    bool focused = false;
};

// Item-sized hit target so keyboard focus, hand cursor and click-release
// match ImGui buttons. `id` is mixed into the item id.
inline Hit hit(ImGuiID id, ImVec2 p0, ImVec2 p1, bool enabled) {
    Hit h;
    const ImVec2 sz(std::max(0.0f, p1.x - p0.x), std::max(0.0f, p1.y - p0.y));
    ImGui::SetCursorScreenPos(p0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "##ctl%08x", static_cast<unsigned>(id));
    if (!enabled) ImGui::BeginDisabled();
    const bool released = ImGui::InvisibleButton(buf, sz);
    if (!enabled) ImGui::EndDisabled();
    h.hovered = enabled && ImGui::IsItemHovered();
    h.held = enabled && ImGui::IsItemActive();
    h.clicked = enabled && released;
    h.focused = enabled && ImGui::IsItemFocused();
    if (h.hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return h;
}

inline void scaled(ImVec2 p0, ImVec2 p1, float scale, ImVec2& o0, ImVec2& o1) {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float hx = (p1.x - p0.x) * 0.5f * scale;
    const float hy = (p1.y - p0.y) * 0.5f * scale;
    o0 = snapPt(c.x - hx, c.y - hy);
    o1 = snapPt(c.x + hx, c.y + hy);
}

} // namespace detail

// One shadow layer, appearance-independent fields filled by the caller.
// Prefer finish() — these exist so a view can stack an extra shadow.
inline ShadowSpec shadowBlack(float y, float blur, float spread, float alpha, bool inset = false) {
    return ShadowSpec{y, blur, spread, detail::shade(false, alpha), inset};
}
inline ShadowSpec shadowWhite(float y, float blur, float spread, float alpha, bool inset = false) {
    return ShadowSpec{y, blur, spread, detail::shade(true, alpha), inset};
}

// Draw a finished surface. Returns the rect content was (or should be) drawn
// in. `pressed` on Primary sinks the face 1 physical pixel and swaps in the
// PrimaryPress look (no shadow). Glass / Menu / Card / … ignore it.
inline ImVec4 finish(ImDrawList* dl, ImVec2 p0, ImVec2 p1, Finish f, float radius = geist::radius::MD,
                     bool pressed = false) {
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const bool dark = geist::mode() == geist::Mode::Dark;
    const auto& t = geist::tokens();

    if (f == Finish::Primary && pressed) f = Finish::PrimaryPress;

    ImU32 fill = 0;
    ImU32 border = 0;
    bool hasBorder = false;
    // Spec alphas: rgb_a() rounds alpha*255+0.5. Listed light then dark.
    ShadowSpec layers[3];
    int n = 0;
    auto black = [&](float y, float blur, float spread, float a, bool inset = false) {
        layers[n++] = shadowBlack(y, blur, spread, a, inset);
    };
    auto white = [&](float y, float blur, float spread, float a, bool inset = false) {
        layers[n++] = shadowWhite(y, blur, spread, a, inset);
    };

    switch (f) {
    case Finish::Card:
        fill = t.surface;
        border = t.outline;
        hasBorder = true;
        black(1.0f, 1.0f, 0.0f, dark ? 0.40f : 0.04f);
        break;
    case Finish::Inset:
        fill = t.wellBg;
        black(1.0f, 2.0f, 0.0f, dark ? 0.45f : 0.06f, true);
        break;
    case Finish::Raised:
        fill = dark ? t.container : t.surface; // #141414 / #ffffff
        black(1.0f, 1.0f, 0.0f, dark ? 0.40f : 0.04f);
        break;
    case Finish::RaisedHover:
        fill = dark ? t.high : t.bg; // #1c1c1c / #fafafa
        black(1.0f, 1.0f, 0.0f, dark ? 0.40f : 0.04f);
        break;
    case Finish::Primary:
        fill = t.primary;
        black(1.0f, 1.0f, 0.0f, dark ? 0.30f : 0.18f);
        break;
    case Finish::PrimaryHover:
        fill = t.primaryHover;
        black(1.0f, 1.0f, 0.0f, dark ? 0.30f : 0.18f);
        break;
    case Finish::PrimaryPress:
        fill = t.primaryPress; // no shadow
        break;
    case Finish::Solid:
        fill = dark ? t.container : t.surface;
        border = t.outline;
        hasBorder = true;
        black(1.0f, 1.0f, 0.0f, dark ? 0.40f : 0.04f);
        break;
    case Finish::Pill:
        fill = t.segThumb;
        black(1.0f, 2.0f, 0.0f, dark ? 0.40f : 0.08f);
        break;
    case Finish::Track: // fill is the caller's; inner shadow only
        black(1.0f, 2.0f, 0.0f, dark ? 0.35f : 0.04f, true);
        break;
    case Finish::Knob:
        black(1.0f, 3.0f, 0.0f, dark ? 0.50f : 0.12f);
        break;
    case Finish::Chip:
        break; // no fill, no shadow — caller tints the capsule
    case Finish::Glass:
        fill = detail::withAlpha(dark ? t.popupBg : t.surface, 0.92f);
        border = t.outline;
        hasBorder = true;
        black(2.0f, 2.0f, 0.0f, dark ? 0.40f : 0.02f);
        black(8.0f, 16.0f, -4.0f, dark ? 0.80f : 0.12f);
        break;
    case Finish::Menu:
        fill = t.popupBg;
        border = t.outline;
        hasBorder = true;
        if (dark) white(1.0f, 0.0f, 0.0f, 0.06f, true);
        black(2.0f, 2.0f, 0.0f, dark ? 0.40f : 0.02f);
        black(8.0f, 16.0f, -4.0f, dark ? 0.80f : 0.12f);
        break;
    }

    float sink = 0.0f;
    if (f == Finish::PrimaryPress && pressed) sink = snapPx(1.0f);
    if (sink != 0.0f) {
        p0.y += sink;
        p1.y += sink;
    }

    for (int i = 0; i < n; ++i) {
        if (!layers[i].inset) detail::drawShadow(dl, p0, p1, radius, layers[i]);
    }
    if (fill) dl->AddRectFilled(p0, p1, fill, radius);
    for (int i = 0; i < n; ++i) {
        if (layers[i].inset) detail::drawShadow(dl, p0, p1, radius, layers[i]);
    }
    if (hasBorder) dl->AddRect(p0, p1, border, radius, 0, hairlinePx());
    return ImVec4(p0.x, p0.y, p1.x, p1.y);
}

inline bool mouseIn(ImVec2 p0, ImVec2 p1) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    return m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y;
}
inline bool clicked(ImVec2 p0, ImVec2 p1) {
    return mouseIn(p0, p1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

// Instant hover wash (no tween). coverage 0.08 is the nav/row wash; segmented
// uses geom::SEG_WASH (0.06).
inline void hoverWash(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, bool hovered,
                      float coverage = geom::HOVER_WASH) {
    if (!hovered || coverage <= 0.0f) return;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    dl->AddRectFilled(p0, p1, detail::washOverlay(coverage), radius);
}

// 1 physical-pixel horizontal hairline.
inline void hHairline(ImDrawList* dl, float x0, float x1, float y, ImU32 col = 0) {
    const float t = hairlinePx();
    const float yy = snapPx(y);
    if (!col) col = geist::tokens().outline;
    dl->AddRectFilled(ImVec2(snapPx(x0), yy), ImVec2(snapPx(x1), yy + t), col);
}
inline void vHairline(ImDrawList* dl, float x, float y0, float y1, ImU32 col = 0) {
    const float t = hairlinePx();
    const float xx = snapPx(x);
    if (!col) col = geist::tokens().outline;
    dl->AddRectFilled(ImVec2(xx, snapPx(y0)), ImVec2(xx + t, snapPx(y1)), col);
}

// Grouping card: radius 12, Card finish (fill + hairline + whisper).
inline ImVec4 groupCard(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    return finish(dl, p0, p1, Finish::Card, geom::CARD_R);
}

// 12px section label, muted ink.
inline void sectionHeader(ImDrawList* dl, ImFont* font, ImVec2 pos, const char* label) {
    drawText(dl, font, geist::type::BODY_SMALL, pos, geist::tokens().body, label);
}

// Technical badge: px6 py2, radius 4, 10px label. Returns the drawn size.
inline ImVec2 badge(ImDrawList* dl, ImFont* font, ImVec2 p0, const char* label, ImU32 fg, ImU32 bg) {
    const ImVec2 ts = calcText(font, 10.0f, label);
    const ImVec2 p1 = snapPt(p0.x + ts.x + 12.0f, p0.y + ts.y + 4.0f);
    p0 = snapPt(p0);
    dl->AddRectFilled(p0, p1, bg, geist::radius::SM);
    drawTextCentered(dl, font, 10.0f, p0, p1, fg, label);
    return ImVec2(p1.x - p0.x, p1.y - p0.y);
}

// ---------------------------------------------------------------------------
// Tooltip bubble — Menu finish, 11px label, px7 py4, radius 6.
// Hover: 500ms delay. Keyboard focus: immediate. Esc closes.
// Call once per control per frame while that control is on screen.
// ---------------------------------------------------------------------------
inline void tooltip(ImDrawList* /*dl*/, ImGuiID id, ImVec2 anchor0, ImVec2 anchor1, const char* label,
                    ImFont* font, bool hovered, bool focused) {
    if (!label || !label[0] || !font) return;
    struct Tip {
        ImGuiID id = 0;
        float ms = 0.0f;
        bool show = false;
    };
    static Tip tip;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        tip = Tip{};
        return;
    }
    if (focused) {
        tip.id = id;
        tip.show = true;
        tip.ms = motion::TOOLTIP_DELAY;
    } else if (hovered) {
        if (tip.id != id) {
            tip.id = id;
            tip.ms = 0.0f;
            tip.show = false;
        }
        tip.ms += ImGui::GetIO().DeltaTime * 1000.0f;
        if (tip.ms >= motion::TOOLTIP_DELAY) tip.show = true;
    } else if (tip.id == id) {
        tip = Tip{};
        return;
    }
    if (!tip.show || tip.id != id) return;

    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geom::LABEL, label);
    const float w = ts.x + 14.0f;
    const float h = ts.y + 8.0f;
    ImVec2 q0(anchor0.x, anchor0.y - 6.0f - h);
    ImVec2 q1(q0.x + w, q0.y + h);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (q0.y < 4.0f) {
        q0.y = anchor1.y + 6.0f;
        q1.y = q0.y + h;
    }
    if (q1.x > disp.x - 4.0f) {
        const float dx = q1.x - (disp.x - 4.0f);
        q0.x -= dx;
        q1.x -= dx;
    }
    if (q0.x < 4.0f) {
        q1.x += 4.0f - q0.x;
        q0.x = 4.0f;
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    finish(fg, q0, q1, Finish::Menu, geist::radius::MD);
    drawTextCentered(fg, font, geom::LABEL, q0, q1, t.ink, label);
}

// ---------------------------------------------------------------------------
// Quiet ghost chip (h24, px8, radius 6): no fill until hover (container),
// muted 11px label. Press spring 0.97. Disabled: faint text, no press.
// ---------------------------------------------------------------------------
inline bool ghostButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                        ImVec2 p0, float alignW = 0.0f, bool enabled = true) {
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geom::LABEL, label);
    const float w = alignW > 0.0f ? alignW : ts.x + 2.0f * geom::GHOST_PX;
    ImVec2 p1(p0.x + w, p0.y + geom::GHOST_H);
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float press = motion::pressScale(id, h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);
    if (enabled && h.hovered) dl->AddRectFilled(q0, q1, t.container, geist::radius::MD);
    drawTextCentered(dl, font, geom::LABEL, q0, q1, enabled ? t.mute : t.faint, label);
    detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    return h.clicked;
}

// Chip button (h24, px8, radius 6): container fill + hairline, 11px ink.
// Hover snaps to `high`. Press spring 0.97.
inline bool chipButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                       ImVec2 p0, float alignW = 0.0f, bool enabled = true) {
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geom::LABEL, label);
    const float w = alignW > 0.0f ? alignW : ts.x + 2.0f * geom::GHOST_PX;
    ImVec2 p1(p0.x + w, p0.y + geom::GHOST_H);
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float press = motion::pressScale(detail::salt(id, 1), h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);
    if (!enabled) {
        finish(dl, q0, q1, Finish::Raised, geist::radius::MD);
        dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, hairlinePx());
    } else {
        dl->AddRectFilled(q0, q1, h.hovered ? t.high : t.container, geist::radius::MD);
        dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, hairlinePx());
    }
    drawTextCentered(dl, font, geom::LABEL, q0, q1, enabled ? t.ink : t.mute, label);
    detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    return h.clicked;
}

// Main CTA. Solid ink, NO scale (§2.2) — PRESS spring sinks 1 physical pixel
// and the fill steps Primary → PrimaryHover → PrimaryPress. Disabled: Raised
// face, dim label, no press. Optional leading icon (15px, nudged 1px down).
inline bool primaryCta(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                       ImVec2 p0, ImVec2 p1, bool enabled = true, const char* icon = nullptr) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float sinkT = motion::springTo(detail::salt(id, 2), (enabled && h.held) ? 1.0f : 0.0f, motion::PRESS, 0.0f);
    const float dy = snapPx(sinkT);
    ImVec2 q0(p0.x, p0.y + dy);
    ImVec2 q1(p1.x, p1.y + dy);
    if (!enabled) {
        finish(dl, q0, q1, Finish::Raised, geist::radius::MD);
    } else if (h.held) {
        finish(dl, q0, q1, Finish::PrimaryPress, geist::radius::MD);
    } else if (h.hovered) {
        finish(dl, q0, q1, Finish::PrimaryHover, geist::radius::MD);
    } else {
        finish(dl, q0, q1, Finish::Primary, geist::radius::MD);
    }
    const ImU32 ink = enabled ? t.onPrimary : t.mute;
    if (icon && icon[0]) {
        const ImVec2 ts = calcText(font, 14.0f, label);
        constexpr float iconW = 15.0f;
        constexpr float gap = 8.0f;
        const float total = iconW + gap + ts.x;
        const float x = q0.x + ((q1.x - q0.x) - total) * 0.5f;
        const float cy = cjkIconCenterY(q0, q1);
        drawIcon(dl, icon, ImVec2(x + iconW * 0.5f, cy), iconW, ink);
        drawText(dl, font, 14.0f, ImVec2(x + iconW + gap, q0.y + ((q1.y - q0.y) - ts.y) * 0.5f), ink, label);
    } else {
        drawTextCentered(dl, font, 14.0f, q0, q1, ink, label);
    }
    detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    return h.clicked;
}

// Secondary / destructive action button (press scale 0.97).
inline bool secondaryButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                            ImVec2 p0, ImVec2 p1, const char* icon = nullptr,
                            bool enabled = true, bool destructive = false) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float press = motion::pressScale(detail::salt(id, 7), h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);

    ImU32 ink = t.ink;
    if (destructive) {
        const ImU32 bg = !enabled ? t.container : (h.hovered ? t.errorHover : t.errorSoft);
        const ImU32 bd = !enabled ? t.outline : t.errorBorder;
        dl->AddRectFilled(q0, q1, bg, geist::radius::MD);
        dl->AddRect(q0, q1, bd, geist::radius::MD, 0, hairlinePx());
        ink = enabled ? t.error : t.mute;
    } else {
        finish(dl, q0, q1, (enabled && h.hovered) ? Finish::RaisedHover : Finish::Raised, geist::radius::MD);
        dl->AddRect(q0, q1, (enabled && h.hovered) ? t.outlineInput : t.outline,
                    geist::radius::MD, 0, hairlinePx());
        ink = enabled ? t.ink : t.mute;
    }

    if (icon && icon[0]) {
        const ImVec2 ts = calcText(font, geist::type::BODY, label);
        constexpr float iconW = 14.0f;
        constexpr float gap = 8.0f;
        const float total = iconW + gap + ts.x;
        const float x = q0.x + ((q1.x - q0.x) - total) * 0.5f;
        drawIcon(dl, icon, ImVec2(x + iconW * 0.5f, cjkIconCenterY(q0, q1)), iconW, ink);
        drawText(dl, font, geist::type::BODY,
                 ImVec2(x + iconW + gap, q0.y + ((q1.y - q0.y) - ts.y) * 0.5f), ink, label);
    } else {
        drawTextCentered(dl, font, geist::type::BODY, q0, q1, ink, label);
    }
    detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    return h.clicked;
}

// Pure-icon control. Raised ⇄ RaisedHover, press spring 0.94, tooltip from
// `tip` (500ms / focus immediate / Esc). `iconPx` defaults to 20.
inline bool iconAction(ImDrawList* dl, ImGuiID id, ImVec2 p0, float box, const char* icon,
                       const char* tip, ImFont* font, float iconPx = geom::ICON,
                       float radius = geist::radius::MD, bool enabled = true) {
    ImVec2 p1(p0.x + box, p0.y + box);
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float press = motion::pressScale(detail::salt(id, 3), h.held, motion::PRESS_SCALE_ICON);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);
    const auto& t = geist::tokens();
    if (!enabled) {
        finish(dl, q0, q1, Finish::Raised, radius);
        dl->AddRect(q0, q1, t.outline, radius, 0, hairlinePx());
    } else if (h.hovered) {
        finish(dl, q0, q1, Finish::RaisedHover, radius);
        dl->AddRect(q0, q1, t.outlineInput, radius, 0, hairlinePx());
    } else {
        finish(dl, q0, q1, Finish::Raised, radius);
        dl->AddRect(q0, q1, t.outline, radius, 0, hairlinePx());
    }
    drawIcon(dl, icon, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), iconPx,
             enabled ? t.mute : t.faint);
    detail::focusRing(dl, p0, p1, radius, h.focused);
    if (enabled) tooltip(dl, detail::salt(id, 4), p0, p1, tip, font, h.hovered, h.focused);
    return h.clicked;
}

// kbd keycap: h22, px7, radius 4, Raised + hairline, 11px. Returns width.
inline float kbd(ImDrawList* dl, ImFont* font, const char* label, ImVec2 p0) {
    const ImVec2 ts = calcText(font, geom::LABEL, label);
    const float w = ts.x + 2.0f * geom::KBD_PX;
    ImVec2 p1(p0.x + w, p0.y + geom::KBD_H);
    finish(dl, p0, p1, Finish::Raised, geom::KBD_R);
    dl->AddRect(snapPt(p0), snapPt(p1), geist::tokens().outline, geom::KBD_R, 0, hairlinePx());
    drawTextCentered(dl, font, geom::LABEL, p0, p1, geist::tokens().body, label);
    return w;
}

// Selection pill position. `responseMs` defaults to the segmented 250ms fit
// (damping 0.86). Pass another id to spring the width the same way.
inline float selectionSpring(ImGuiID id, float target, float responseMs = 250.0f) {
    return motion::springTo(id, target, motion::selectionParams(responseMs), target);
}

// Switch knob travel 0..1. Multiply by geom::TOGGLE_TRAVEL (18px).
inline float switchKnob(ImGuiID id, bool checked) {
    return motion::springTo(id, checked ? 1.0f : 0.0f, motion::PLACE, checked ? 1.0f : 0.0f);
}

// Progress width follower: 260ms response, damping 0.9. Seeded at target so
// fixture states / already-running jobs never enter from 0 on first mount.
inline float progressSpring(ImGuiID id, float fraction01, float width) {
    fraction01 = std::clamp(fraction01, 0.0f, 1.0f);
    const float target = fraction01 * width;
    return motion::springTo(id, target, motion::layoutSpring(), target);
}

// Thin bar: 4px track (hairline ink), Vercel-blue fill, width on the layout spring.
inline void progressBar(ImDrawList* dl, ImGuiID id, ImVec2 p0, float width, float fraction01,
                        float height = geom::PROGRESS_H) {
    p0 = snapPt(p0);
    const float h = std::max(height, hairlinePx());
    const float y1 = p0.y + h;
    const auto& t = geist::tokens();
    dl->AddRectFilled(p0, ImVec2(p0.x + width, y1), t.outline, h * 0.5f);
    const float fw = snapPx(std::max(0.0f, progressSpring(id, fraction01, width)));
    if (fw > 0.5f) dl->AddRectFilled(p0, ImVec2(p0.x + fw, y1), t.accent, h * 0.5f);
}

// ---------------------------------------------------------------------------
// Segmented track: recessed + Track inner shadow, radius 8, 2px pad.
// Pill (Finish::Pill, radius 6) spring-slides. Unselected neighbors keep a
// 1×12 hairline. Hover wash 6% instant. Press 0.97. Locked: dim, no press.
// `current` is an index. Returns true when it changes.
// ---------------------------------------------------------------------------
inline bool segmented(ImDrawList* dl, ImGuiID id, ImVec2 pos, ImVec2 size,
                      const char* const* labels, int count, int& current, ImFont* font,
                      float fontPx = 12.0f, bool locked = false) {
    if (!labels || count <= 0 || size.x <= 1.0f) return false;
    if (current < 0) current = 0;
    if (current >= count) current = count - 1;
    pos = snapPt(pos);
    const bool dark = geist::mode() == geist::Mode::Dark;
    const ImVec2 p1(pos.x + size.x, pos.y + size.y);
    const auto& t = geist::tokens();
    dl->AddRectFilled(pos, p1, t.recessed, geom::SEG_TRACK_R);
    finish(dl, pos, p1, Finish::Track, geom::SEG_TRACK_R);
    dl->AddRect(pos, p1, t.segBorder, geom::SEG_TRACK_R, 0, hairlinePx());

    const float innerW = std::max(0.0f, size.x - geom::SEG_PAD * 2.0f);
    const float gaps = static_cast<float>(count - 1) * hairlinePx();
    const float segW = (innerW - gaps) / static_cast<float>(count);
    const float segH = std::max(0.0f, size.y - geom::SEG_PAD * 2.0f);
    const float top = pos.y + geom::SEG_PAD;

    auto segX = [&](int i) { return pos.x + geom::SEG_PAD + static_cast<float>(i) * (segW + hairlinePx()); };

    const float pillX = selectionSpring(detail::salt(id, 10), segX(current));
    const float pillW = selectionSpring(detail::salt(id, 11), segW);
    ImVec2 s0 = snapPt(pillX, top);
    ImVec2 s1 = snapPt(pillX + pillW, top + segH);
    finish(dl, s0, s1, Finish::Pill, geom::SEG_PILL_R);
    // Segmented thumb rim: 1px #FFFFFF14 in the dark (ui.rs comment), black 6% in the light.
    const float pillRim = dark ? 0.08f : 0.06f;
    dl->AddRect(s0, s1, detail::washOverlay(pillRim), geom::SEG_PILL_R, 0, hairlinePx());

    bool changed = false;
    for (int i = 0; i < count; ++i) {
        if (i > 0) {
            const bool beside = (i == current || i - 1 == current);
            const float a = motion::approach(detail::salt(id, 20 + i), beside ? 0.0f : 1.0f, motion::FAST,
                                             beside ? 0.0f : 1.0f);
            if (a > 0.02f) {
                const float x = snapPx(segX(i) - hairlinePx());
                const float y0 = snapPx(pos.y + (size.y - geom::SEG_DIV_H) * 0.5f);
                dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + hairlinePx(), y0 + geom::SEG_DIV_H),
                                  detail::withAlpha(t.outlineInput, a));
            }
        }
        ImVec2 a(segX(i), top);
        ImVec2 b(a.x + segW, top + segH);
        a = snapPt(a);
        b = snapPt(b);
        const detail::Hit h = detail::hit(detail::salt(id, 40 + i), a, b, !locked);
        if (h.clicked && current != i) {
            current = i;
            changed = true;
        }
        const bool on = (i == current);
        if (!on && h.hovered) hoverWash(dl, a, b, geom::SEG_PILL_R, true, geom::SEG_WASH);
        const float press = motion::pressScale(detail::salt(id, 60 + i), h.held, motion::PRESS_SCALE);
        ImVec2 t0, t1;
        detail::scaled(a, b, locked ? 1.0f : press, t0, t1);
        ImU32 ink = t.mute;
        if (locked) ink = t.mute;
        else if (on) ink = t.ink;
        else if (h.hovered) ink = t.ink;
        else ink = t.body;
        drawTextCentered(dl, font, fontPx, t0, t1, ink, labels[i] ? labels[i] : "");
        detail::focusRing(dl, a, b, geom::SEG_PILL_R, h.focused);
    }
    return changed;
}

// Locked segmented variant (dim, non-interactive).
inline bool segmentedLocked(ImDrawList* dl, ImGuiID id, ImVec2 pos, ImVec2 size,
                            const char* const* labels, int count, int& current, ImFont* font,
                            float fontPx = 12.0f) {
    return segmented(dl, id, pos, size, labels, count, current, font, fontPx, true);
}

// Pure quality slider snapping helper (unit-testable headless).
inline float snapSliderFrac(float rawFrac, float snapFrac = geom::SNAP_FRAC) {
    rawFrac = std::clamp(rawFrac, 0.0f, 1.0f);
    static constexpr float qs[5] = {0.0f, 0.25f, 0.50f, 0.75f, 1.0f};
    for (float q : qs) {
        if (std::fabs(rawFrac - q) <= snapFrac) return q;
    }
    return rawFrac;
}

inline int quantizeSliderValue(float rawFrac, int minVal, int maxVal, float snapFrac = geom::SNAP_FRAC) {
    if (maxVal <= minVal) return minVal;
    const float tpos = snapSliderFrac(rawFrac, snapFrac);
    return std::clamp(static_cast<int>(std::lround(minVal + tpos * (maxVal - minVal))), minVal, maxVal);
}

// ---------------------------------------------------------------------------
// Quality slider. Thumb and fill track the pointer 1:1 (no spring). Release
// does not fling. Quantiles 0/25/50/75/100% magnetize within 2.2% of the track.
// 18px hit, 2px hairline track, ink fill, 13px surface knob + Knob shadow,
// five 1×3 ticks.
// ---------------------------------------------------------------------------
inline bool qualitySlider(ImDrawList* dl, ImGuiID id, ImVec2 pos, float width,
                          int& value, int minVal, int maxVal) {
    if (!(width > 1.0f) || maxVal <= minVal) return false;
    pos = snapPt(pos);
    const ImVec2 sz(width, geom::SLIDER_H);
    const detail::Hit h = detail::hit(id, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), true);
    bool changed = false;
    if (h.held) {
        const float tpos = (ImGui::GetIO().MousePos.x - pos.x) / width;
        const int next = quantizeSliderValue(tpos, minVal, maxVal, geom::SNAP_FRAC);
        if (next != value) {
            value = next;
            changed = true;
        }
    }
    if (h.focused) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dir = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dir = 1;
        if (dir != 0) {
            value = std::clamp(value + dir, minVal, maxVal);
            changed = true;
        }
    }

    const float frac = std::clamp(static_cast<float>(value - minVal) / static_cast<float>(maxVal - minVal), 0.0f, 1.0f);
    const auto& t = geist::tokens();
    const float trackY = snapPx(pos.y + (geom::SLIDER_H - geom::SLIDER_TRACK) * 0.5f);
    const float fillW = snapPx(frac * width);
    dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + width, trackY + geom::SLIDER_TRACK), t.outline, 1.0f);
    if (fillW > 0.0f) {
        dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + fillW, trackY + geom::SLIDER_TRACK), t.primary, 1.0f);
    }
    for (int i = 0; i < 5; ++i) {
        const float tx = snapPx(pos.x + (width - hairlinePx()) * (static_cast<float>(i) / 4.0f));
        dl->AddRectFilled(ImVec2(tx, snapPx(pos.y + 13.0f)),
                          ImVec2(tx + hairlinePx(), snapPx(pos.y + 16.0f)), t.outlineInput);
    }
    const float knobR = geom::SLIDER_KNOB * 0.5f;
    const float cx = std::clamp(pos.x + fillW, pos.x + knobR, pos.x + width - knobR);
    const ImVec2 c = snapPt(cx, trackY + geom::SLIDER_TRACK * 0.5f);
    ImVec2 k0(c.x - knobR, c.y - knobR);
    ImVec2 k1(c.x + knobR, c.y + knobR);
    finish(dl, k0, k1, Finish::Knob, knobR);
    dl->AddCircleFilled(c, knobR, t.bg, 28);
    dl->AddCircle(c, knobR - 0.75f, t.ink, 28, 1.5f);
    return changed;
}

// 52×22 recessed field, mono-ish digits. Live-clamps into [min,max] on commit
// (deactivate); in-range edits write through immediately. Returns true when
// `value` changes.
inline bool qualityField(ImDrawList* dl, ImGuiID id, ImVec2 pos, int& value, int minVal, int maxVal,
                         ImFont* font) {
    pos = snapPt(pos);
    const ImVec2 p1(pos.x + 52.0f, pos.y + 22.0f);
    const auto& t = geist::tokens();
    dl->AddRectFilled(pos, p1, t.recessed, 5.0f);
    dl->AddRect(pos, p1, t.outlineInput, 5.0f, 0, hairlinePx());

    struct Buf {
        char text[16] = {};
        int synced = 0x7fffffff;
        bool init = false;
        bool wasActive = false;
    };
    static std::unordered_map<ImGuiID, Buf> bufs;
    Buf& b = bufs[id];

    if (!b.init || (b.synced != value && !ImGui::IsAnyItemActive())) {
        std::snprintf(b.text, sizeof(b.text), "%d", value);
        b.synced = value;
        b.init = true;
    }

    const bool drewNative = b.wasActive;
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 8.0f, pos.y + 2.0f));
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    if (font) ImGui::PushFont(font, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, drewNative ? t.ink : IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(36.0f);
    char item[24];
    std::snprintf(item, sizeof(item), "##qf%08x", static_cast<unsigned>(id));
    const bool edited = ImGui::InputText(item, b.text, sizeof(b.text), ImGuiInputTextFlags_CharsDecimal);
    bool changed = false;
    auto parseInRange = [&](long& n) -> bool {
        char* end = nullptr;
        n = std::strtol(b.text, &end, 10);
        return end != b.text;
    };
    if (edited) {
        long n = 0;
        if (parseInRange(n) && n >= minVal && n <= maxVal) {
            const int next = static_cast<int>(n);
            if (next != value) {
                value = next;
                changed = true;
            }
            b.synced = value;
        }
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        long n = 0;
        int next = value;
        if (parseInRange(n)) {
            if (n < minVal) n = minVal;
            if (n > maxVal) n = maxVal;
            next = static_cast<int>(n);
        }
        next = std::clamp(next, minVal, maxVal);
        if (next != value) {
            value = next;
            changed = true;
        }
        std::snprintf(b.text, sizeof(b.text), "%d", value);
        b.synced = value;
    } else if (!ImGui::IsItemActive() && b.synced != value) {
        std::snprintf(b.text, sizeof(b.text), "%d", value);
        b.synced = value;
    }
    const bool active = ImGui::IsItemActive();
    b.wasActive = active;
    const bool focused = active || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    if (!drewNative) {
        const ImVec2 tsz = calcText(font, 12.0f, b.text);
        drawText(dl, font, 12.0f,
                 snapPt(pos.x + (52.0f - tsz.x) * 0.5f, pos.y + (22.0f - tsz.y) * 0.5f),
                 t.ink, b.text);
    }
    detail::focusRing(dl, pos, p1, 5.0f, focused);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (font) ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

// Toggle 44×26, 20px knob, 18px PLACE travel. Track color eases over BASE
// (off = high + input hairline, on = ink). Knob: surface off, onPrimary on.
// No track shadow (the web switch is a flat face). Returns true when flipped.
inline bool toggle(ImDrawList* dl, ImGuiID id, ImVec2 pos, bool& checked, bool enabled = true) {
    pos = snapPt(pos);
    const ImVec2 p1(pos.x + geom::TOGGLE_W, pos.y + geom::TOGGLE_H);
    const detail::Hit h = detail::hit(id, pos, p1, enabled);
    if (h.clicked) checked = !checked;
    const float travel = switchKnob(detail::salt(id, 5), checked);
    const float u = motion::approach(detail::salt(id, 6), checked ? 1.0f : 0.0f, motion::BASE, checked ? 1.0f : 0.0f);
    const auto& t = geist::tokens();
    const ImU32 track = motion::mix(t.high, t.ink, u);
    const ImU32 rim = motion::mix(t.outlineInput, t.ink, u);
    dl->AddRectFilled(pos, p1, track, geom::TOGGLE_H * 0.5f);
    dl->AddRect(pos, p1, rim, geom::TOGGLE_H * 0.5f, 0, hairlinePx());
    // Border is inside the 44: pad 2 + border 1 → knob origin +3, travel 18.
    const float left = pos.x + 3.0f + travel * geom::TOGGLE_TRAVEL;
    const float top = pos.y + 3.0f;
    const float kr = geom::TOGGLE_KNOB * 0.5f;
    const ImVec2 c = snapPt(left + kr, top + kr);
    ImVec2 k0(c.x - kr, c.y - kr);
    ImVec2 k1(c.x + kr, c.y + kr);
    finish(dl, k0, k1, Finish::Knob, kr);
    const ImU32 knob = motion::mix(t.surface, t.onPrimary, u);
    dl->AddCircleFilled(c, kr, enabled ? knob : motion::mix(knob, t.mute, 0.35f), 28);
    detail::focusRing(dl, pos, p1, geom::TOGGLE_H * 0.5f, h.focused);
    return h.clicked;
}

// Continuous "working" arc — not an entrance. `turnsPerSec` ~ 0.8.
inline void busyArc(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, float stroke = 1.8f) {
    const float a0 = static_cast<float>(ImGui::GetTime()) * 5.0f;
    dl->PathClear();
    dl->PathArcTo(center, radius, a0, a0 + 1.9f, 18);
    dl->PathStroke(col, 0, stroke);
}

// Scene dot color, shared by model rows and the picker (ui.rs scene_dot_color).
// 动漫 → blue (scene general), 照片 → amber, 修复 → magenta, else violet.
inline ImU32 sceneDotColor(const char* scene) {
    const auto& t = geist::tokens();
    if (!scene || !scene[0]) return t.sceneGeneral; // empty / 通用 reads as 综合
    if (std::strstr(scene, "动漫")) return t.sceneAnime;
    if (std::strstr(scene, "照片") && !std::strstr(scene, "修复")) return t.scenePhoto;
    if (std::strstr(scene, "修复")) return t.sceneRestore;
    return t.sceneGeneral;
}

// Empty / 通用 reads as 综合. Writes into `out` (NUL-terminated, cap includes NUL).
inline const char* sceneLabel(const char* scene, char* out, int cap) {
    if (!out || cap < 2) return scene ? scene : "";
    if (!scene || !scene[0] || std::strcmp(scene, "通用") == 0) {
        std::snprintf(out, static_cast<size_t>(cap), "综合");
        return out;
    }
    return scene;
}

// Aliases matching DESIGN / blueprint names.
inline bool qualityInput(ImDrawList* dl, ImGuiID id, ImVec2 pos, int& value, int minVal, int maxVal, ImFont* font) {
    return qualityField(dl, id, pos, value, minVal, maxVal, font);
}

inline bool toggleSwitch(ImDrawList* dl, ImGuiID id, ImVec2 pos, bool& checked, bool enabled = true) {
    return toggle(dl, id, pos, checked, enabled);
}

// Horizontal hairline divider inside a card with optional horizontal inset.
inline void cardDivider(ImDrawList* dl, float x0, float x1, float y, float inset = geom::ROW_PX) {
    hHairline(dl, x0 + inset, x1 - inset, y);
}

// Row chrome inside a groupCard (44px+ min height, title + optional subtitle).
// Returns {rowBottomY, clicked} via CardRowResult.
struct CardRowResult {
    float bottomY = 0.0f;
    float rightAnchorX = 0.0f;
    float centerY = 0.0f;
    bool hovered = false;
    bool clicked = false;
};

inline CardRowResult cardRow(ImDrawList* dl, ImGuiID id, ImVec2 p0, float width,
                             const char* title, const char* subtitle, ImFont* font,
                             bool interactive = false, float rightReserveW = 0.0f) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    const ImVec2 tsz = calcText(font, geist::type::BODY, title ? title : "");
    const bool hasSub = subtitle && subtitle[0];
    const ImVec2 ssz = hasSub ? calcText(font, geom::ROW_SUB, subtitle) : ImVec2(0.0f, 0.0f);
    const float contentH = hasSub ? (tsz.y + 3.0f + ssz.y) : tsz.y;
    const float h = std::max(geom::ROW_MIN_H, contentH + geom::ROW_PY * 2.0f);
    const ImVec2 p1 = snapPt(p0.x + width, p0.y + h);

    CardRowResult res;
    res.bottomY = p1.y;
    res.rightAnchorX = p1.x - geom::ROW_PX;
    res.centerY = (p0.y + p1.y) * 0.5f;

    if (interactive) {
        const detail::Hit hitRes = detail::hit(id, p0, p1, true);
        res.hovered = hitRes.hovered;
        res.clicked = hitRes.clicked;
        if (hitRes.hovered) {
            hoverWash(dl, ImVec2(p0.x + 2.0f, p0.y + 2.0f), ImVec2(p1.x - 2.0f, p1.y - 2.0f),
                      geist::radius::MD, true, geom::HOVER_WASH);
        }
        detail::focusRing(dl, p0, p1, geist::radius::MD, hitRes.focused);
    }

    const float textMaxW = std::max(40.0f, width - geom::ROW_PX * 2.0f - rightReserveW);
    const float textY = p0.y + (h - contentH) * 0.5f;
    ellipsisText(dl, font, geist::type::BODY, ImVec2(p0.x + geom::ROW_PX, textY), textMaxW, t.ink, title ? title : "");
    if (hasSub) {
        ellipsisText(dl, font, geom::ROW_SUB, ImVec2(p0.x + geom::ROW_PX, textY + tsz.y + 3.0f),
                     textMaxW, t.mute, subtitle);
    }
    return res;
}

// Recessed search field with leading magnifying glass and clickable clear "x" when non-empty.
inline bool searchBox(ImDrawList* dl, ImGuiID id, ImVec2 pos, float width, float height,
                      char* buf, size_t bufCap, const char* placeholder, ImFont* font) {
    if (!buf || bufCap == 0 || width <= 40.0f) return false;
    pos = snapPt(pos);
    const ImVec2 p1 = snapPt(pos.x + width, pos.y + height);
    const auto& t = geist::tokens();

    dl->AddRectFilled(pos, p1, t.recessed, geist::radius::MD);
    const bool hasText = buf[0] != '\0';

    drawIcon(dl, "search", ImVec2(pos.x + 14.0f, cjkIconCenterY(pos, p1)), 13.0f, t.mute, 1.5f);

    const float clearBtnW = hasText ? 22.0f : 0.0f;
    const float inputX = pos.x + 28.0f;
    const float inputW = std::max(16.0f, width - 34.0f - clearBtnW);

    bool changed = false;
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    ImGui::SetCursorScreenPos(ImVec2(inputX, pos.y + (height - 20.0f) * 0.5f));
    if (font) ImGui::PushFont(font, geist::type::BODY_SMALL);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, t.ink);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(inputW);
    char item[24];
    std::snprintf(item, sizeof(item), "##srch%08x", static_cast<unsigned>(id));
    if (ImGui::InputText(item, buf, bufCap)) {
        changed = true;
    }
    const bool focused = ImGui::IsItemActive() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (font) ImGui::PopFont();
    ImGui::PopID();

    if (!hasText && placeholder && placeholder[0]) {
        const ImVec2 psz = calcText(font, geist::type::BODY_SMALL, placeholder);
        drawText(dl, font, geist::type::BODY_SMALL,
                 ImVec2(inputX, pos.y + (height - psz.y) * 0.5f), t.faint, placeholder);
    }

    dl->AddRect(pos, p1, focused ? t.accent : t.outlineInput, geist::radius::MD, 0, hairlinePx());

    if (hasText) {
        const ImVec2 c0 = snapPt(p1.x - 24.0f, pos.y + (height - 20.0f) * 0.5f);
        const ImVec2 c1 = snapPt(c0.x + 20.0f, c0.y + 20.0f);
        const detail::Hit clrHit = detail::hit(detail::salt(id, 9), c0, c1, true);
        if (clrHit.hovered) {
            dl->AddRectFilled(c0, c1, detail::washOverlay(0.08f), geist::radius::SM);
        }
        drawIcon(dl, "x", ImVec2((c0.x + c1.x) * 0.5f, (c0.y + c1.y) * 0.5f), 11.0f,
                 clrHit.hovered ? t.ink : t.mute, 1.5f);
        if (clrHit.clicked) {
            buf[0] = '\0';
            changed = true;
        }
    }
    return changed;
}

} // namespace ctl
} // namespace upix::ui
