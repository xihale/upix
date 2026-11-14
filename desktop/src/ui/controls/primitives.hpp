// ui/controls/primitives.hpp — Low-level drawing, optical text alignment,
// geometry tokens, hit testing, hover washes, hairlines, and motion springs.
#pragma once

#include "ui/flex.hpp"
#include "ui/geist.hpp"
#include "ui/icons.hpp"
#include "ui/motion.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace upix::ui {

inline void drawBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 fill, ImU32 border,
                    float radius = 8.0f, ImDrawFlags flags = 0) {
    if (!dl) return;
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
    if (!dl) return;
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

inline unsigned int decodeUtf8(const char*& p) {
    if (!p || !*p) return 0;
    const unsigned char c0 = static_cast<unsigned char>(*p++);
    if (c0 < 0x80u) return c0;
    unsigned int cp = 0;
    int extra = 0;
    if ((c0 & 0xE0u) == 0xC0u) { cp = c0 & 0x1Fu; extra = 1; }
    else if ((c0 & 0xF0u) == 0xE0u) { cp = c0 & 0x0Fu; extra = 2; }
    else if ((c0 & 0xF8u) == 0xF0u) { cp = c0 & 0x07u; extra = 3; }
    else return 0xFFFDu;
    for (int i = 0; i < extra; ++i) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ((c & 0xC0u) != 0x80u) return 0xFFFDu;
        ++p;
        cp = (cp << 6) | (c & 0x3Fu);
    }
    return cp;
}

// Distance from DrawText origin.y to the optical vertical center of `text` at `size`.
// Uses baked glyph ink bounds [Y0..Y1] so Latin caps, digits, and CJK full-width glyphs center
// identically inside pills, chips, buttons, and mixed-font flex rows.
inline float textOpticalMidOffset(ImFont* font, float size, const char* text) {
    if (!font || size <= 0.0f || !font->IsLoaded()) return size * 0.5f;
    ImFontBaked* baked = font->GetFontBaked(size);
    if (!baked) return size * 0.5f;

    auto glyphBox = [&](unsigned int cp, float& minY, float& maxY) -> bool {
        const ImFontGlyph* g = baked->FindGlyphNoFallback(static_cast<ImWchar>(cp));
        if (!g) g = baked->FindGlyph(static_cast<ImWchar>(cp));
        if (!g || !g->Visible || g->Y1 <= g->Y0) return false;
        minY = std::min(minY, g->Y0);
        maxY = std::max(maxY, g->Y1);
        return true;
    };

    float cjkMinY = 1e9f, cjkMaxY = -1e9f;
    bool hasCjk = false;
    bool hasAscii = false;
    if (text) {
        const char* p = text;
        while (*p) {
            const unsigned int cp = decodeUtf8(p);
            if (cp <= 0x20u) continue;
            if (cp >= 0x2E80u) {
                if (glyphBox(cp, cjkMinY, cjkMaxY)) hasCjk = true;
            } else {
                hasAscii = true;
            }
        }
    }
    float capMinY = 1e9f, capMaxY = -1e9f;
    if (hasAscii) {
        if (!glyphBox('H', capMinY, capMaxY)) glyphBox('0', capMinY, capMaxY);
    }
    if (hasCjk && capMinY <= capMaxY) {
        return (std::min(cjkMinY, capMinY) + std::max(cjkMaxY, capMaxY)) * 0.5f;
    }
    if (hasCjk) return (cjkMinY + cjkMaxY) * 0.5f;
    if (capMinY <= capMaxY) return (capMinY + capMaxY) * 0.5f;

    float minY = 1e9f, maxY = -1e9f;
    if (text) {
        const char* p = text;
        while (*p) {
            const unsigned int cp = decodeUtf8(p);
            if (cp > 0x20u) glyphBox(cp, minY, maxY);
        }
    }
    return (minY <= maxY) ? (minY + maxY) * 0.5f : (size * 0.5f);
}

inline float drawTextPosYCentered(ImFont* font, float size, float rectMinY, float rectMaxY, const char* text) {
    return snapPx((rectMinY + rectMaxY) * 0.5f - textOpticalMidOffset(font, size, text));
}

inline void drawText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text) {
    if (!dl || !font || !text) return;
    dl->AddText(font, size, snapPt(pos), col, text);
}

inline void drawTextCenteredY(ImDrawList* dl, ImFont* font, float size,
                              float x, float rectMinY, float rectMaxY, ImU32 col, const char* text) {
    if (!dl || !font || !text) return;
    dl->AddText(font, size, ImVec2(snapPx(x), drawTextPosYCentered(font, size, rectMinY, rectMaxY, text)), col, text);
}

inline void drawTextCentered(ImDrawList* dl, ImFont* font, float size,
                             ImVec2 rectMin, ImVec2 rectMax, ImU32 col, const char* text) {
    if (!dl || !font || !text) return;
    const ImVec2 sz = calcText(font, size, text);
    const ImVec2 pos(snapPx(rectMin.x + (rectMax.x - rectMin.x - sz.x) * 0.5f),
                     drawTextPosYCentered(font, size, rectMin.y, rectMax.y, text));
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

inline void ellipsisTextCentered(ImDrawList* dl, ImFont* font, float size,
                                 float x, float rectMinY, float rectMaxY,
                                 float maxWidth, ImU32 col, const char* text) {
    std::string fitted = fitTextEllipsis(font, size, text, maxWidth);
    drawTextCenteredY(dl, font, size, x, rectMinY, rectMaxY, col, fitted.c_str());
}

inline void drawIconLabelCentered(ImDrawList* dl, ImVec2 q0, ImVec2 q1, ImFont* font, float fontSize,
                                  const char* icon, float iconSize, float gap, ImU32 col, const char* label) {
    const char* safeLabel = label ? label : "";
    const bool hasIcon = icon && icon[0] != '\0';
    const bool hasLabel = safeLabel[0] != '\0';
    if (!hasIcon && !hasLabel) return;

    const ImVec2 ts = hasLabel ? calcText(font, fontSize, safeLabel) : ImVec2(0.0f, 0.0f);
    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .gap = (hasIcon && hasLabel) ? gap : 0.0f,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    const flex::Slot iconSlot = row.slot(flex::Fixed(iconSize), flex::Fixed(iconSize),
                                         flex::AlignSelf::Center, {}, hasIcon);
    const flex::Slot textSlot = row.slot(flex::Shrink(std::ceil(ts.x), 1.0f).min(0.0f), flex::Fixed(ts.y),
                                         flex::AlignSelf::Center, {}, hasLabel);
    fx.solve(flex::Rect{q0.x, q0.y, std::max(0.0f, q1.x - q0.x), std::max(0.0f, q1.y - q0.y)});

    if (fx.isVisible(iconSlot)) {
        drawIcon(dl, icon, fx.rect(iconSlot).center(), iconSize, col);
    }
    if (fx.isVisible(textSlot)) {
        const flex::Rect tr = fx.rect(textSlot);
        ellipsisTextCentered(dl, font, fontSize, tr.x, q0.y, q1.y, tr.w, col, safeLabel);
    }
}

inline bool hasNonAsciiUtf8(const char* s) {
    if (!s) return false;
    for (const char* p = s; *p; ++p) {
        if ((static_cast<unsigned char>(*p) & 0x80u) != 0) return true;
    }
    return false;
}

struct SegmentOption {
    const char* label;
    const char* value;
};

namespace ctl {

using upix::ui::hasNonAsciiUtf8;

// Geometry from ui.rs / web theme.css. Draw hairlines with hairlinePx(), not the literal 1.
namespace geom {
inline constexpr float GHOST_H = 28.0f;
inline constexpr float GHOST_PX = 10.0f;
inline constexpr float CHIP_R = 6.0f;
inline constexpr float LABEL = geist::type::LABEL;
inline constexpr float ROW_MIN_H = 52.0f;
inline constexpr float ROW_PX = 16.0f;
inline constexpr float ROW_PY = 10.0f;
inline constexpr float ROW_GAP = 12.0f;
inline constexpr float ROW_SUB = geist::type::BODY_SMALL;
inline constexpr float ICON = 20.0f;
inline constexpr float KBD_H = 26.0f;
inline constexpr float KBD_PX = 8.0f;
inline constexpr float KBD_R = 4.0f;
inline constexpr float TOGGLE_W = 32.0f;
inline constexpr float TOGGLE_H = 18.0f;
inline constexpr float TOGGLE_KNOB = 14.0f;
inline constexpr float TOGGLE_TRAVEL = 14.0f; // PLACE spring, px
inline constexpr float SEG_H = 42.0f;
inline constexpr float SEG_H_COMPACT = 38.0f;
inline constexpr float SEG_PAD = 4.0f;
inline constexpr float SEG_TRACK_R = 8.0f;
inline constexpr float SEG_PILL_R = 4.0f;
inline constexpr float SEG_DIV_H = 12.0f;
inline constexpr float TOPBAR_H = 52.0f;
inline constexpr float SLIDER_H = 18.0f;
inline constexpr float SLIDER_TRACK = 4.0f;
inline constexpr float SLIDER_KNOB = 14.0f;
inline constexpr float BTN_H = 36.0f;
inline constexpr float INPUT_H = 34.0f;
inline constexpr float CTA_H = 46.0f;
inline constexpr float PROGRESS_H = 4.0f;
inline constexpr float CARD_R = 12.0f;
inline constexpr float META_CHIP_H = 24.0f;
inline constexpr float HOVER_WASH = 0.08f;    // nav / rows (§2.1)
inline constexpr float SEG_WASH = 0.06f;      // segmented (§2.2)
inline constexpr float SNAP_FRAC = 0.022f;    // quality slider magnet
} // namespace geom

namespace detail {

inline ImU32 withAlpha(ImU32 c, float a) {
    return upix::ui::withAlpha(c, std::clamp(a, 0.0f, 1.0f));
}

inline ImU32 shade(bool white, float a) {
    return upix::ui::withAlpha(white ? IM_COL32_WHITE : IM_COL32_BLACK, std::clamp(a, 0.0f, 1.0f));
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

inline void focusRing(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, bool focused,
                      float offset = 0.0f, float thickness = 2.0f) {
    (void)dl;
    (void)p0;
    (void)p1;
    (void)radius;
    (void)focused;
    (void)offset;
    (void)thickness;
}

inline void inputFocusHalo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius,
                           bool focused, bool invalid = false) {
    if (!dl || !focused) return;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (invalid) {
        dl->AddRect(ImVec2(p0.x - 1.5f, p0.y - 1.5f), ImVec2(p1.x + 1.5f, p1.y + 1.5f),
                    t.errorSoft, radius + 1.5f, 0, 3.0f);
        dl->AddRect(p0, p1, t.error, radius, 0, std::max(hairlinePx(), 1.5f));
    } else {
        dl->AddRect(p0, p1, t.outlineStrong, radius, 0, std::max(hairlinePx(), 1.5f));
    }
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

inline const char* nextUtf8(const char* p) {
    if (!p || !*p) return p;
    const unsigned char c = static_cast<unsigned char>(*p++);
    if ((c & 0x80u) == 0) return p;
    int extra = 0;
    if ((c & 0xE0u) == 0xC0u) extra = 1;
    else if ((c & 0xF0u) == 0xE0u) extra = 2;
    else if ((c & 0xF8u) == 0xF0u) extra = 3;
    while (extra-- > 0 && *p && (static_cast<unsigned char>(*p) & 0xC0u) == 0x80u) ++p;
    return p;
}

inline void asciiUpperCopy(const char* src, char* dst, size_t cap) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        dst[i] = c;
    }
    dst[i] = '\0';
}

inline void drawTrackedText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos,
                            ImU32 col, const char* text, float trackingPx) {
    if (!dl || !font || !text || !text[0]) return;
    if (std::fabs(trackingPx) < 0.01f) {
        drawText(dl, font, size, pos, col, text);
        return;
    }
    float x = snapPx(pos.x);
    const float y = snapPx(pos.y);
    const char* p = text;
    while (*p) {
        const char* n = nextUtf8(p);
        dl->AddText(font, size, snapPt(x, y), col, p, n);
        x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, p, n).x + trackingPx;
        p = n;
    }
}

} // namespace detail

inline bool mouseIn(ImVec2 p0, ImVec2 p1) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    return m.x >= p0.x && m.x < p1.x && m.y >= p0.y && m.y < p1.y;
}

inline bool clicked(ImVec2 p0, ImVec2 p1) {
    return mouseIn(p0, p1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
}

inline void hoverWashAlpha(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, float factor,
                           float coverage = geom::HOVER_WASH) {
    if (!dl || factor <= 0.01f || coverage <= 0.0f) return;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    dl->AddRectFilled(p0, p1, detail::washOverlay(coverage * std::clamp(factor, 0.0f, 1.0f)), radius);
}

// Instant or factor-driven hover wash. coverage 0.08 is the nav/row wash; segmented
// uses geom::SEG_WASH (0.06).
inline void hoverWash(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, bool hovered,
                      float coverage = geom::HOVER_WASH) {
    if (!hovered) return;
    hoverWashAlpha(dl, p0, p1, radius, 1.0f, coverage);
}

// 1 physical-pixel horizontal hairline.
inline void hHairline(ImDrawList* dl, float x0, float x1, float y, ImU32 col = 0) {
    if (!dl) return;
    const float t = hairlinePx();
    const float yy = snapPx(y);
    if (!col) col = geist::tokens().outline;
    dl->AddRectFilled(ImVec2(snapPx(x0), yy), ImVec2(snapPx(x1), yy + t), col);
}

inline void vHairline(ImDrawList* dl, float x, float y0, float y1, ImU32 col = 0) {
    if (!dl) return;
    const float t = hairlinePx();
    const float xx = snapPx(x);
    if (!col) col = geist::tokens().outline;
    dl->AddRectFilled(ImVec2(xx, snapPx(y0)), ImVec2(xx + t, snapPx(y1)), col);
}

// Flush 1px hairline rule across a section boundary.
inline void hairlineRule(ImDrawList* dl, float x0, float x1, float y, ImU32 col = 0) {
    hHairline(dl, x0, x1, y, col);
}

// Selection pill position. `responseMs` defaults to the segmented 250ms fit
// (damping 0.86). Pass another id to spring the width the same way.
inline float selectionSpring(ImGuiID id, float target, float responseMs = 250.0f) {
    return motion::springTo(id, target, motion::selectionParams(responseMs), target);
}

// Switch knob travel 0..1. Multiply by geom::TOGGLE_TRAVEL (14px).
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

// Continuous "working" arc — not an entrance. `turnsPerSec` ~ 0.8.
inline void busyArc(ImDrawList* dl, ImVec2 center, float radius, ImU32 col, float stroke = 1.8f) {
    if (!dl) return;
    if (!motion::reduceMotion() && ImGui::GetCurrentContext()) motion::markMotionActive();
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

} // namespace ctl
} // namespace upix::ui
