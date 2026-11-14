// ui/controls/chips.hpp — Headers, meta chips, badges, keycaps, tooltips,
// and progress bars.
#pragma once

#include "ui/controls/surfaces.hpp"

#include <algorithm>
#include <cmath>

namespace upix::ui::ctl {

// Mono uppercase eyebrow header (11px t.mute, 0.06em tracking for ASCII; 12px
// untracked for CJK/non-ASCII) with optional right-aligned tabular mono meta label.
inline void eyebrowHeader(ImDrawList* dl, ImFont* monoFont, ImVec2 pos, float width,
                          const char* label, const char* rightText = nullptr,
                          ImU32 labelCol = 0, ImU32 rightCol = 0,
                          float fontSize = geist::type::EYEBROW) {
    const auto& t = geist::tokens();
    const char* safeLabel = label ? label : "";
    const float baseFs = fontSize > 0.0f ? fontSize : geist::type::EYEBROW; // 11px
    const bool nonAscii = hasNonAsciiUtf8(safeLabel);
    const float fs = nonAscii ? std::max(baseFs, geist::type::BODY_SMALL) : baseFs;
    const ImU32 col = labelCol ? labelCol : t.mute;
    const bool hasRight = rightText && rightText[0] != '\0' && width > 0.0f;

    char upper[256] = {};
    float tracking = 0.0f;
    if (!nonAscii) {
        detail::asciiUpperCopy(safeLabel, upper, sizeof(upper));
        tracking = fs * 0.06f; // 0.06em
    }
    const char* measureText = nonAscii ? safeLabel : upper;
    const ImVec2 lsz = calcText(monoFont, fs, measureText);
    const ImVec2 rsz = hasRight ? calcText(monoFont, fs, rightText) : ImVec2(0.0f, 0.0f);
    const float rowH = std::max({fs, lsz.y, rsz.y});
    pos = snapPt(pos);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .gap = hasRight ? 8.0f : 0.0f,
        .align = flex::Align::Center,
        .width = (width > 0.0f) ? flex::Fixed(width) : flex::Auto(),
        .height = flex::Fixed(rowH),
    });
    const flex::Len leftW = (width > 0.0f)
        ? flex::Grow(1.0f).min(16.0f)
        : flex::Fixed(std::ceil(lsz.x));
    const flex::Slot leftSlot = row.slot(leftW, flex::Fixed(rowH));
    const flex::Slot rightSlot = row.slot(flex::Fixed(std::ceil(rsz.x)), flex::Fixed(rowH),
                                          flex::AlignSelf::Center, {}, hasRight);
    fx.solve(flex::Rect{pos.x, pos.y, (width > 0.0f) ? width : flex::kUnbounded, rowH});

    const flex::Rect lr = fx.rect(leftSlot);
    if (nonAscii) {
        if (width > 0.0f) {
            ellipsisTextCentered(dl, monoFont, fs, lr.x, lr.y, lr.bottom(), lr.w, col, safeLabel);
        } else {
            drawTextCenteredY(dl, monoFont, fs, lr.x, lr.y, lr.bottom(), col, safeLabel);
        }
    } else {
        const float y = drawTextPosYCentered(monoFont, fs, lr.y, lr.bottom(), upper);
        detail::drawTrackedText(dl, monoFont, fs, ImVec2(lr.x, y), col, upper, tracking);
    }

    if (fx.isVisible(rightSlot)) {
        const flex::Rect rr = fx.rect(rightSlot);
        drawTextCenteredY(dl, monoFont, fs, rr.x, rr.y, rr.bottom(),
                          rightCol ? rightCol : t.faint, rightText);
    }
}

inline void eyebrowHeader(ImDrawList* dl, ImFont* monoFont, ImVec2 pos, const char* label,
                          ImU32 labelCol = 0) {
    eyebrowHeader(dl, monoFont, pos, 0.0f, label, nullptr, labelCol, 0);
}

// 12px section label, muted ink (optionally rendered as Web mono uppercase eyebrow).
inline void sectionHeader(ImDrawList* dl, ImFont* font, ImVec2 pos, const char* label,
                          bool eyebrow = false, float width = 0.0f,
                          const char* rightText = nullptr) {
    if (eyebrow) {
        eyebrowHeader(dl, font, pos, width, label, rightText);
        return;
    }
    const auto& t = geist::tokens();
    const char* safeLabel = label ? label : "";
    const bool hasRight = rightText && rightText[0] != '\0' && width > 0.0f;
    const ImVec2 lsz = calcText(font, geist::type::BODY_SMALL, safeLabel);
    const ImVec2 rsz = hasRight ? calcText(font, geist::type::BODY_SMALL, rightText) : ImVec2(0.0f, 0.0f);
    const float rowH = std::max({geist::type::BODY_SMALL, lsz.y, rsz.y});
    pos = snapPt(pos);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .gap = hasRight ? 8.0f : 0.0f,
        .align = flex::Align::Center,
        .width = (width > 0.0f) ? flex::Fixed(width) : flex::Auto(),
        .height = flex::Fixed(rowH),
    });
    const flex::Len leftW = (width > 0.0f)
        ? flex::Grow(1.0f).min(16.0f)
        : flex::Fixed(std::ceil(lsz.x));
    const flex::Slot leftSlot = row.slot(leftW, flex::Fixed(rowH));
    const flex::Slot rightSlot = row.slot(flex::Fixed(std::ceil(rsz.x)), flex::Fixed(rowH),
                                          flex::AlignSelf::Center, {}, hasRight);
    fx.solve(flex::Rect{pos.x, pos.y, (width > 0.0f) ? width : flex::kUnbounded, rowH});

    const flex::Rect lr = fx.rect(leftSlot);
    if (width > 0.0f) {
        ellipsisTextCentered(dl, font, geist::type::BODY_SMALL, lr.x, lr.y, lr.bottom(), lr.w, t.body, safeLabel);
    } else {
        drawTextCenteredY(dl, font, geist::type::BODY_SMALL, lr.x, lr.y, lr.bottom(), t.body, safeLabel);
    }
    if (fx.isVisible(rightSlot)) {
        const flex::Rect rr = fx.rect(rightSlot);
        drawTextCenteredY(dl, font, geist::type::BODY_SMALL, rr.x, rr.y, rr.bottom(), t.mute, rightText);
    }
}

inline float metaChipWidth(ImFont* font, const char* label, bool hasDot = false, float padX = 5.0f) {
    const ImVec2 ts = calcText(font, geist::type::BODY_SMALL, label ? label : "");
    const float dotW = hasDot ? (6.0f + 5.0f) : 0.0f;
    return std::ceil(ts.x + dotW + padX * 2.0f);
}

// Borderless subtle pill (18px, radius 4px, t.container, optional leading 6px dot, 12px text).
// Pass dotColor == 0 for a dot-free fill-only chip. Returns the drawn {width, height}.
inline ImVec2 metaChip(ImDrawList* dl, ImFont* font, ImVec2 p0, const char* label,
                       ImU32 dotColor = 0, ImU32 textColor = 0, ImU32 bgColor = 0,
                       float height = geom::META_CHIP_H, float padX = 5.0f) {
    const auto& t = geist::tokens();
    const bool hasDot = (dotColor >> IM_COL32_A_SHIFT) > 0;
    const float w = metaChipWidth(font, label, hasDot, padX);
    p0 = snapPt(p0);
    const ImU32 fg = textColor ? textColor : t.body;
    const char* safeLabel = label ? label : "";

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(padX, 0.0f),
        .gap = 5.0f,
        .align = flex::Align::Center,
        .width = flex::Fixed(w),
        .height = flex::Fixed(height),
    });
    const flex::Slot dotSlot = row.slot(flex::Fixed(6.0f), flex::Fixed(6.0f), flex::AlignSelf::Center, {}, hasDot);
    const flex::Slot lblSlot = row.slot(flex::Grow(1.0f), flex::Fixed(geist::type::BODY_SMALL));
    const flex::Rect box = fx.solveAuto(p0);

    if (dl) {
        dl->AddRectFilled(box.p0(), box.p1(), bgColor ? bgColor : t.container, geist::radius::SM);
        if (fx.isVisible(dotSlot)) {
            dl->AddCircleFilled(snapPt(fx.rect(dotSlot).center()), 3.0f, dotColor);
        }
        const flex::Rect lr = fx.rect(lblSlot);
        drawTextCenteredY(dl, font, geist::type::BODY_SMALL, lr.x, box.y, box.bottom(), fg, safeLabel);
    }
    return box.size();
}

inline float recChipWidth(ImFont* font, const char* label = "推荐", float padX = 5.0f) {
    return metaChipWidth(font, label, false, padX);
}

// Blue recommendation pill ("推荐"): t.accentSoft fill + t.accent text, borderless, radius 4px.
inline ImVec2 recChip(ImDrawList* dl, ImFont* font, ImVec2 p0, const char* label = "推荐",
                      float height = geom::META_CHIP_H, float padX = 5.0f) {
    const auto& t = geist::tokens();
    return metaChip(dl, font, p0, label, 0, t.accent, t.accentSoft, height, padX);
}

// Technical badge: px6 py2, radius 4, eyebrow label. Returns the drawn size.
inline ImVec2 badge(ImDrawList* dl, ImFont* font, ImVec2 p0, const char* label, ImU32 fg, ImU32 bg) {
    const char* safeLabel = label ? label : "";
    const ImVec2 ts = calcText(font, geist::type::EYEBROW, safeLabel);
    p0 = snapPt(p0);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(7.0f, 3.0f),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    const flex::Slot lblSlot = row.slot(flex::Fixed(std::ceil(ts.x)), flex::Fixed(ts.y));
    const flex::Rect box = fx.solveAuto(p0);

    if (dl) {
        dl->AddRectFilled(box.p0(), box.p1(), bg, geist::radius::SM);
        const flex::Rect lr = fx.rect(lblSlot);
        drawTextCentered(dl, font, geist::type::EYEBROW, lr.p0(), lr.p1(), fg, safeLabel);
    }
    return box.size();
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
    const bool kbdFocused = focused && (!ImGui::GetCurrentContext() || ImGui::GetIO().NavVisible);
    if (kbdFocused) {
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

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto buildTip = [&]() {
        auto row = fx.row({
            .padding = flex::Edges::xy(8.0f, 5.0f),
            .justify = flex::Justify::Center,
            .align = flex::Align::Center,
        });
        return row.slot(flex::Fixed(std::ceil(ts.x)), flex::Fixed(ts.y));
    };
    (void)buildTip();
    const flex::Rect probe = fx.solveAuto(ImVec2(0.0f, 0.0f));

    ImVec2 q0(anchor0.x, anchor0.y - 6.0f - probe.h);
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    if (q0.y < 4.0f) {
        q0.y = anchor1.y + 6.0f;
    }
    if (q0.x + probe.w > disp.x - 4.0f) {
        q0.x -= (q0.x + probe.w) - (disp.x - 4.0f);
    }
    if (q0.x < 4.0f) {
        q0.x = 4.0f;
    }

    const flex::Slot lblSlot = buildTip();
    const flex::Rect box = fx.solveAuto(snapPt(q0));
    const flex::Rect lr = fx.rect(lblSlot);

    ImDrawList* fg = ImGui::GetForegroundDrawList();
    finish(fg, box.p0(), box.p1(), Finish::Menu, geist::radius::MD);
    drawTextCentered(fg, font, geom::LABEL, lr.p0(), lr.p1(), t.ink, label);
}

// kbd keycap: h22, px7, radius 4, Raised + hairline, 11px. Returns width.
inline float kbd(ImDrawList* dl, ImFont* font, const char* label, ImVec2 p0) {
    const char* safeLabel = label ? label : "";
    const ImVec2 ts = calcText(font, geom::LABEL, safeLabel);
    p0 = snapPt(p0);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(geom::KBD_PX, 0.0f),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .height = flex::Fixed(geom::KBD_H),
    });
    const flex::Slot lblSlot = row.slot(flex::Fixed(std::ceil(ts.x)), flex::Fixed(ts.y));
    const flex::Rect box = fx.solveAuto(p0);

    if (dl) {
        finish(dl, box.p0(), box.p1(), Finish::Raised, geom::KBD_R);
        dl->AddRect(box.p0(), box.p1(), geist::tokens().outline, geom::KBD_R, 0, hairlinePx());
        const flex::Rect lr = fx.rect(lblSlot);
        drawTextCentered(dl, font, geom::LABEL, lr.p0(), lr.p1(), geist::tokens().body, safeLabel);
    }
    return box.w;
}

// Thin bar: 4px track (hairline ink), Vercel-blue fill, width on the layout spring.
inline void progressBar(ImDrawList* dl, ImGuiID id, ImVec2 p0, float width, float fraction01,
                        float height = geom::PROGRESS_H) {
    if (!dl) return;
    p0 = snapPt(p0);
    const float h = std::max(height, hairlinePx());
    const float y1 = p0.y + h;
    const auto& t = geist::tokens();
    dl->AddRectFilled(p0, ImVec2(p0.x + width, y1), t.outline, h * 0.5f);
    const float fw = snapPx(std::max(0.0f, progressSpring(id, fraction01, width)));
    if (fw > 0.5f) dl->AddRectFilled(p0, ImVec2(p0.x + fw, y1), t.accent, h * 0.5f);
}

} // namespace upix::ui::ctl
