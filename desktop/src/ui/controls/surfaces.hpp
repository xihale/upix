// ui/controls/surfaces.hpp — Geist Finish surfaces, layered shadows, cards,
// card rows, and scrollbar indicators.
#pragma once

#include "ui/controls/primitives.hpp"

#include <algorithm>
#include <cmath>

namespace upix::ui::ctl {

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
    Whisper,
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
    Capsule,
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

// Soft outer shadow. Rings are drawn large-to-small so the fringe fades to
// exactly 0 at `blur` (no hard cutoff) and the umbra holds the spec alpha.
inline void drawOuter(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    if (!dl) return;
    const int a8 = static_cast<int>((s.col >> IM_COL32_A_SHIFT) & 0xFF);
    if (a8 <= 0) return;
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    const float minDim = std::min(w, h);
    if (minDim <= 0.0f) return;

    const float alpha = static_cast<float>(a8) / 255.0f;
    const float effSpread = std::max(s.spread, -0.18f * minDim);
    const float yScale = (minDim < 56.0f) ? std::clamp(minDim / 56.0f, 0.35f, 1.0f) : 1.0f;
    const float effY = s.y * yScale;
    const ImVec2 a(p0.x - effSpread, p0.y - effSpread + effY);
    const ImVec2 b(p1.x + effSpread, p1.y + effSpread + effY);
    const float rad = std::max(0.0f, radius + effSpread);
    const ImU32 rgb = s.col & ~IM_COL32_A_MASK;

    float painted = 0.0f;
    auto paintTo = [&](ImVec2 q0, ImVec2 q1, float rr, float target) {
        if (target <= painted + 1.0e-4f) return;
        const float layer = (target - painted) / std::max(1.0e-4f, 1.0f - painted);
        const int la = static_cast<int>(layer * 255.0f + 0.5f);
        if (la <= 0) return;
        const float actualLayer = static_cast<float>(la) / 255.0f;
        painted = painted + (1.0f - painted) * actualLayer;
        dl->AddRectFilled(q0, q1, rgb | (static_cast<ImU32>(la) << IM_COL32_A_SHIFT), rr);
    };
    if (s.blur < 0.35f) {
        paintTo(a, b, rad, alpha);
        return;
    }
    constexpr int kSteps = 6;
    for (int i = kSteps - 1; i >= 1; --i) {
        const float u = static_cast<float>(i) / static_cast<float>(kSteps);
        const float d = s.blur * u;
        const float falloff = (1.0f - u) * (1.0f - u); // quadratic falloff, 0 at outer edge
        paintTo(ImVec2(a.x - d, a.y - d), ImVec2(b.x + d, b.y + d), rad + d, alpha * falloff);
    }
    paintTo(a, b, rad, alpha);
}

// Inner shadow that stays inside the rounded rect (concentric strokes).
// Positive y biases a lip along the top — light comes from above.
inline void drawInner(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, const ShadowSpec& s) {
    if (!dl) return;
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
    if (s.blur <= 0.0f && s.y == 0.0f) {
        const float th = hairlinePx();
        const float d = th * 0.5f;
        const ImU32 c = colA(alpha);
        if (c) dl->AddRect(ImVec2(p0.x + d, p0.y + d), ImVec2(p1.x - d, p1.y - d), c, std::max(0.0f, radius - d), 0, th);
        dl->PopClipRect();
        return;
    }
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
    if (!dl) return ImVec4(p0.x, p0.y, p1.x, p1.y);
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
    case Finish::Whisper:
        fill = t.surface;
        if (dark) {
            black(1.0f, 2.0f, 0.0f, 0.50f);
        } else {
            white(1.0f, 0.0f, 0.0f, 0.60f, true);
            black(1.0f, 1.0f, 0.0f, 0.04f);
        }
        break;
    case Finish::Primary:
        fill = t.ink;
        black(1.0f, 1.0f, 0.0f, dark ? 0.30f : 0.18f);
        break;
    case Finish::PrimaryHover:
        fill = t.inkHover;
        black(1.0f, 1.0f, 0.0f, dark ? 0.30f : 0.18f);
        break;
    case Finish::PrimaryPress:
        fill = t.inkPress; // no shadow
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
        if (dark) {
            white(0.0f, 0.0f, 0.0f, 0.08f, true);
            black(4.0f, 12.0f, 0.0f, 0.50f);
            black(12.0f, 32.0f, -4.0f, 0.80f);
        } else {
            white(1.0f, 0.0f, 0.0f, 0.60f, true);
            black(2.0f, 2.0f, 0.0f, 0.02f);
            black(8.0f, 16.0f, -4.0f, 0.12f);
        }
        break;
    case Finish::Capsule:
        fill = t.popupBg;
        border = t.outline;
        hasBorder = true;
        if (dark) {
            white(0.0f, 0.0f, 0.0f, 0.08f, true);
            black(2.0f, 6.0f, 0.0f, 0.45f);
            black(6.0f, 16.0f, -2.0f, 0.60f);
        } else {
            white(1.0f, 0.0f, 0.0f, 0.65f, true);
            black(1.0f, 2.0f, 0.0f, 0.03f);
            black(3.0f, 10.0f, -1.0f, 0.08f);
        }
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

namespace detail {

inline void drawRaisedFace(ImDrawList* dl, ImVec2 q0, ImVec2 q1, float radius,
                           bool enabled, float hovT, bool flat = false, bool held = false) {
    if (!dl) return;
    const auto& t = geist::tokens();
    const bool dark = (geist::mode() == geist::Mode::Dark);
    if (flat) {
        // Crisp flat secondary face (no drop shadow):
        // Dark: #111111 -> #1C1C1C hover -> #242424 press;
        // Light: #FFFFFF -> #F4F4F4 hover -> #EBEBEB press.
        ImU32 fill = t.surface;
        ImU32 bd = t.outline;
        if (!enabled) {
            fill = t.container;
            bd = t.outline;
        } else if (held) {
            fill = dark ? t.highest : t.high;
            bd = t.outlineStrong;
        } else if (hovT > 0.01f) {
            const ImU32 hoverFill = dark ? t.high : t.container;
            fill = motion::mix(t.surface, hoverFill, hovT);
            bd = motion::mix(t.outline, t.outlineStrong, hovT);
        }
        dl->AddRectFilled(q0, q1, fill, radius);
        dl->AddRect(q0, q1, bd, radius, 0, hairlinePx());
        return;
    }
    finish(dl, q0, q1, Finish::Raised, radius);
    if (enabled) {
        const ImU32 baseFill = dark ? t.container : t.surface;
        const ImU32 hoverFill = dark ? t.high : t.container;
        if (held) {
            dl->AddRectFilled(q0, q1, dark ? t.highest : t.high, radius);
        } else if (hovT > 0.01f) {
            dl->AddRectFilled(q0, q1, motion::mix(baseFill, hoverFill, hovT), radius);
        }
    }
    const ImU32 bd = !enabled
        ? t.outline
        : (held ? t.outlineStrong : motion::mix(t.outline, t.outlineStrong, hovT));
    dl->AddRect(q0, q1, bd, radius, 0, hairlinePx());
}

} // namespace detail

// Flat hairline surface for sections (no drop shadow).
inline ImVec4 flatSection(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                          float radius = geom::CARD_R, ImU32 fill = 0, ImU32 border = 0) {
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (!dl) return ImVec4(p0.x, p0.y, p1.x, p1.y);
    const auto& t = geist::tokens();
    const ImU32 f = fill ? fill : t.surface;
    const ImU32 b = border ? border : t.outline;
    if ((f >> IM_COL32_A_SHIFT) > 0) dl->AddRectFilled(p0, p1, f, radius);
    if ((b >> IM_COL32_A_SHIFT) > 0) dl->AddRect(p0, p1, b, radius, 0, hairlinePx());
    return ImVec4(p0.x, p0.y, p1.x, p1.y);
}

// Grouping card: radius 12, Card finish (fill + hairline + whisper).
inline ImVec4 groupCard(ImDrawList* dl, ImVec2 p0, ImVec2 p1) {
    return finish(dl, p0, p1, Finish::Card, geom::CARD_R);
}

// Horizontal hairline divider inside a card with optional horizontal inset.
inline void cardDivider(ImDrawList* dl, float x0, float x1, float y, float inset = geom::ROW_PX) {
    hHairline(dl, x0 + inset, x1 - inset, y);
}

// Row chrome inside a groupCard (44px+ min height, title + optional subtitle).
// Returns {rowBottomY, rightAnchorX, centerY, hovered, clicked} via CardRowResult.
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
    const char* safeTitle = title ? title : "";
    const bool hasSub = subtitle && subtitle[0] != '\0';
    const ImVec2 tsz = calcText(font, geist::type::BODY, safeTitle);
    const ImVec2 ssz = hasSub ? calcText(font, geom::ROW_SUB, subtitle) : ImVec2(0.0f, 0.0f);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(geom::ROW_PX, geom::ROW_PY),
        .gap = 0.0f,
        .align = flex::Align::Center,
        .width = flex::Fixed(std::max(0.0f, width)),
        .height = flex::Auto().min(geom::ROW_MIN_H),
    });
    auto textCol = row.col({
        .gap = 3.0f,
        .width = flex::Grow(1.0f).min(40.0f),
        .height = flex::Auto(),
    });
    const flex::Slot titleSlot = textCol.slot(flex::Grow(1.0f), flex::Fixed(tsz.y));
    const flex::Slot subSlot = textCol.slot(flex::Grow(1.0f), flex::Fixed(ssz.y),
                                            flex::AlignSelf::Auto, {}, hasSub);
    const flex::Slot rightSlot = row.slot(flex::Fixed(std::max(0.0f, rightReserveW)),
                                          flex::Fixed(0.0f), flex::AlignSelf::Center, {},
                                          rightReserveW > 0.0f);
    const flex::Rect rowR = fx.solveWidth(p0, width);
    (void)rightSlot;

    CardRowResult res;
    res.bottomY = rowR.bottom();
    res.rightAnchorX = rowR.right() - geom::ROW_PX;
    res.centerY = rowR.midY();

    if (interactive) {
        const detail::Hit hitRes = detail::hit(id, rowR.p0(), rowR.p1(), true);
        res.hovered = hitRes.hovered;
        res.clicked = hitRes.clicked;
        const float hovT = motion::hoverFactor(detail::salt(id, 1), hitRes.hovered);
        if (hitRes.held || hovT > 0.01f) {
            const flex::Rect washR = rowR.inset(2.0f);
            hoverWashAlpha(dl, washR.p0(), washR.p1(), geist::radius::MD,
                           hitRes.held ? 1.0f : hovT,
                           hitRes.held ? (geom::HOVER_WASH * 1.4f) : geom::HOVER_WASH);
        }
        detail::focusRing(dl, rowR.p0(), rowR.p1(), geist::radius::MD, hitRes.focused);
    }

    const flex::Rect tr = fx.rect(titleSlot);
    ellipsisTextCentered(dl, font, geist::type::BODY, tr.x, tr.y, tr.bottom(), tr.w, t.ink, safeTitle);
    if (fx.isVisible(subSlot)) {
        const flex::Rect sr = fx.rect(subSlot);
        ellipsisTextCentered(dl, font, geom::ROW_SUB, sr.x, sr.y, sr.bottom(), sr.w, t.mute, subtitle);
    }
    return res;
}

// Subtle 3px inset scrollbar thumb (6px vertical margin, min 28px tall, 1.5px radius).
inline void scrollThumb(ImDrawList* dl, ImVec2 paneMin, ImVec2 paneMax, float scroll, float contentH) {
    if (!dl) return;
    const float paneH = paneMax.y - paneMin.y;
    if (contentH <= paneH + 2.0f || paneH <= 24.0f) return;

    const float trackY0 = paneMin.y + 6.0f;
    const float trackY1 = paneMax.y - 6.0f;
    const float trackH = trackY1 - trackY0;
    if (trackH <= 4.0f) return;

    const float thumbH = std::min(trackH, std::max(28.0f, trackH * (paneH / contentH)));
    const float maxScroll = std::max(0.0f, contentH - paneH);
    const float frac = (maxScroll > 0.0f) ? std::clamp(scroll / maxScroll, 0.0f, 1.0f) : 0.0f;
    const float thumbY = trackY0 + frac * (trackH - thumbH);

    const float x0 = paneMax.x - 7.0f;
    const float x1 = paneMax.x - 4.0f;
    const bool trackHover = ImGui::GetCurrentContext()
        ? mouseIn(ImVec2(paneMax.x - 12.0f, trackY0), ImVec2(paneMax.x, trackY1))
        : false;
    const auto& t = geist::tokens();
    const ImU32 col = upix::ui::withAlpha(trackHover ? t.mute : t.outlineStrong, 0.85f);
    dl->AddRectFilled(snapPt(x0, thumbY), snapPt(x1, thumbY + thumbH), col, 1.5f);
}

} // namespace upix::ui::ctl
