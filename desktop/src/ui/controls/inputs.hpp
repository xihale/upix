// ui/controls/inputs.hpp — Segmented controls, underline tabs, sliders,
// numeric inputs, toggle switches, and search boxes.
#pragma once

#include "ui/controls/surfaces.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

namespace upix::ui::ctl {

// ---------------------------------------------------------------------------
// Segmented track: flat t.segTrack + 1px t.segBorder, radius 8,
// 4px pad (NO inner shadow, NO vertical dividers). Thumb (t.segThumb + 1px
// t.segThumbRim, + Finish::Whisper in light mode, radius 4) spring-slides.
// `compact` selects 38px height (15px font) instead of 42px (18px font).
// Returns true on change.
// ---------------------------------------------------------------------------
inline bool segmented(ImDrawList* dl, ImGuiID id, ImVec2 pos, ImVec2 size,
                      const char* const* labels, int count, int& current, ImFont* font,
                      float fontPx = 0.0f, bool locked = false, bool compact = false) {
    if (!labels || count <= 0 || size.x <= 1.0f) return false;
    if (current < 0) current = 0;
    if (current >= count) current = count - 1;
    pos = snapPt(pos);
    const bool dark = geist::mode() == geist::Mode::Dark;
    float useFontPx = (fontPx > 0.0f) ? fontPx : (compact ? geist::type::BODY_SMALL : geist::type::BODY);
    const float trackH = (size.y <= 0.0f)
        ? (compact ? geom::SEG_H_COMPACT : geom::SEG_H)
        : (compact ? std::min(size.y, geom::SEG_H_COMPACT) : size.y);
    const ImVec2 p1 = snapPt(pos.x + size.x, pos.y + trackH);
    const float trackW = std::max(1.0f, p1.x - pos.x);
    const float snappedH = std::max(1.0f, p1.y - pos.y);
    const auto& t = geist::tokens();
    if (dl) {
        dl->AddRectFilled(pos, p1, t.segTrack, geom::SEG_TRACK_R);
        dl->AddRect(pos, p1, t.segBorder, geom::SEG_TRACK_R, 0, hairlinePx());
    }

    constexpr int kMaxSeg = 16;
    const int n = std::min(count, kMaxSeg);
    static flex::Context segCtx;
    flex::Layout fx(segCtx);
    auto row = fx.row({
        .padding = flex::Edges::all(geom::SEG_PAD),
        .gap = 0.0f,
        .align = flex::Align::Stretch,
    });
    flex::Slot segSlots[kMaxSeg];
    for (int i = 0; i < n; ++i) {
        segSlots[i] = row.slot(flex::Grow(1.0f), flex::Fraction(1.0f));
    }
    fx.solve(flex::Rect{pos.x, pos.y, trackW, snappedH});

    // Keep at least ~9px horizontal breathing room per side inside tight cells.
    if (font) {
        const float cellW = std::max(0.0f, trackW - geom::SEG_PAD * 2.0f) / static_cast<float>(n);
        auto maxWAt = [&](float px) {
            float m = 0.0f;
            for (int i = 0; i < n; ++i) {
                if (labels[i]) m = std::max(m, calcText(font, px, labels[i]).x);
            }
            return m;
        };
        if (useFontPx > geist::type::BODY_SMALL && maxWAt(useFontPx) + 20.0f > cellW) {
            useFontPx = geist::type::BODY_SMALL;
        }
        if (useFontPx > geist::type::EYEBROW && maxWAt(useFontPx) + 18.0f > cellW) {
            useFontPx = geist::type::EYEBROW;
        }
    }

    detail::Hit hits[kMaxSeg];
    float hovTs[kMaxSeg] = {};
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        const flex::Rect sr = fx.rect(segSlots[i]);
        const ImVec2 a = snapPt(sr.p0());
        const ImVec2 b = snapPt(sr.p1());
        hits[i] = detail::hit(detail::salt(id, 40 + i), a, b, !locked);
        if (hits[i].clicked && current != i) {
            current = i;
            changed = true;
        }
    }
    for (int i = 0; i < n; ++i) {
        const bool on = (i == current);
        hovTs[i] = motion::hoverFactor(detail::salt(id, 80 + i), !locked && !on && hits[i].hovered);
        if (dl && !on && hovTs[i] > 0.01f) {
            const flex::Rect sr = fx.rect(segSlots[i]);
            hoverWashAlpha(dl, snapPt(sr.p0()), snapPt(sr.p1()), geom::SEG_PILL_R, hovTs[i], 0.08f);
        }
    }

    const flex::Rect curR = fx.rect(segSlots[std::min(current, n - 1)]);
    const float pillX = selectionSpring(detail::salt(id, 10), curR.x);
    const float pillW = selectionSpring(detail::salt(id, 11), curR.w);
    const ImVec2 s0 = snapPt(pillX, curR.y);
    const ImVec2 s1 = snapPt(pillX + pillW, curR.bottom());
    if (dl) {
        if (!dark) {
            finish(dl, s0, s1, Finish::Whisper, geom::SEG_PILL_R);
        }
        dl->AddRectFilled(s0, s1, t.segThumb, geom::SEG_PILL_R);
        dl->AddRect(s0, s1, t.segThumbRim, geom::SEG_PILL_R, 0, hairlinePx());
    }

    if (dl) {
        for (int i = 0; i < n; ++i) {
            const flex::Rect sr = fx.rect(segSlots[i]);
            const ImVec2 a = snapPt(sr.p0());
            const ImVec2 b = snapPt(sr.p1());
            const bool on = (i == current);
            const float press = motion::pressScale(detail::salt(id, 60 + i), hits[i].held, motion::PRESS_SCALE);
            ImVec2 t0, t1;
            detail::scaled(a, b, locked ? 1.0f : press, t0, t1);
            ImU32 ink = t.faint;
            if (locked) ink = t.faint;
            else if (on) ink = t.segFg;
            else ink = motion::mix(t.segFgMuted, t.segFg, hovTs[i]);
            const char* lbl = labels[i] ? labels[i] : "";
            const float maxLblW = std::max(8.0f, (t1.x - t0.x) - 12.0f);
            if (font && calcText(font, useFontPx, lbl).x > maxLblW) {
                const std::string fitted = fitTextEllipsis(font, useFontPx, lbl, maxLblW);
                drawTextCentered(dl, font, useFontPx, t0, t1, ink, fitted.c_str());
            } else {
                drawTextCentered(dl, font, useFontPx, t0, t1, ink, lbl);
            }
            detail::focusRing(dl, a, b, geom::SEG_PILL_R, hits[i].focused);
        }
    }
    return changed;
}

// Locked segmented variant (dim, non-interactive).
inline bool segmentedLocked(ImDrawList* dl, ImGuiID id, ImVec2 pos, ImVec2 size,
                            const char* const* labels, int count, int& current, ImFont* font,
                            float fontPx = 0.0f, bool compact = false) {
    return segmented(dl, id, pos, size, labels, count, current, font, fontPx, true, compact);
}

// Underline tabs (.seg-tabs): shared bottom 1px t.outline rule across full row
// width, active tab has 2px t.ink bottom indicator + t.ink text, inactive t.mute
// -> t.ink on hover. Returns true when `current` changes.
inline bool underlineTabs(ImDrawList* dl, ImGuiID id, ImVec2 pos, ImVec2 size,
                          const char* const* labels, int count, int& current, ImFont* font,
                          float fontPx = geist::type::BODY, float gap = geist::space::LG,
                          bool locked = false, bool bottomRuleFlush = true) {
    if (!labels || count <= 0 || size.x <= 1.0f || size.y <= 1.0f) return false;
    if (current < 0) current = 0;
    if (current >= count) current = count - 1;
    pos = snapPt(pos);
    const ImVec2 p1 = snapPt(pos.x + size.x, pos.y + size.y);
    const auto& t = geist::tokens();

    // Shared bottom 1px hairline across full row width.
    const float lineT = hairlinePx();
    if (dl) hHairline(dl, pos.x, p1.x, p1.y - lineT, t.outline);

    // Measure tab widths up front (cap stack array at 16, fall back cleanly).
    constexpr int kMaxInline = 16;
    float widths[kMaxInline] = {};
    float totalW = 0.0f;
    const int n = std::min(count, kMaxInline);
    for (int i = 0; i < n; ++i) {
        const ImVec2 ts = calcText(font, fontPx, labels[i] ? labels[i] : "");
        widths[i] = std::ceil(ts.x) + 4.0f;
        totalW += widths[i];
    }
    float useGap = gap;
    if (n > 1 && totalW + useGap * static_cast<float>(n - 1) > size.x) {
        useGap = std::max(4.0f, (size.x - totalW) / static_cast<float>(n - 1));
    }

    static flex::Context tabCtx;
    flex::Layout fx(tabCtx);
    auto row = fx.row({
        .gap = useGap,
        .align = flex::Align::Stretch,
    });
    flex::Slot tabSlots[kMaxInline];
    for (int i = 0; i < n; ++i) {
        tabSlots[i] = row.slot(flex::Shrink(widths[i], 1.0f).min(12.0f), flex::Fraction(1.0f));
    }
    fx.solve(flex::Rect{pos.x, pos.y, size.x, size.y});

    const float barH = std::max(lineT, snapPx(2.0f));
    const float barBottom = bottomRuleFlush ? p1.y : (p1.y - lineT);

    bool changed = false;
    for (int i = 0; i < n; ++i) {
        const flex::Rect tr = fx.rect(tabSlots[i]);
        const ImVec2 a = snapPt(tr.p0());
        const ImVec2 b = snapPt(tr.p1());
        const detail::Hit h = detail::hit(detail::salt(id, 40 + i), a, b, !locked);
        if (h.clicked && current != i) {
            current = i;
            changed = true;
        }
        const bool on = (i == current);
        const float hovT = motion::hoverFactor(detail::salt(id, 80 + i), !locked && !on && h.hovered);
        if (dl) {
            if (!on && hovT > 0.01f) {
                const float pillTop = snapPx(a.y + 3.0f);
                const float pillBot = snapPx(barBottom - barH - 4.0f);
                if (pillBot > pillTop + 4.0f) {
                    hoverWashAlpha(dl, ImVec2(a.x, pillTop), ImVec2(b.x, pillBot), geist::radius::MD, hovT, 0.07f);
                }
            }
            ImU32 ink = t.mute;
            if (locked) ink = t.faint;
            else if (on) ink = t.ink;
            else ink = motion::mix(t.mute, t.ink, hovT);
            const char* lbl = labels[i] ? labels[i] : "";
            if (font && calcText(font, fontPx, lbl).x > tr.w) {
                const std::string fitted = fitTextEllipsis(font, fontPx, lbl, tr.w);
                drawTextCentered(dl, font, fontPx, a, b, ink, fitted.c_str());
            } else {
                drawTextCentered(dl, font, fontPx, a, b, ink, lbl);
            }
            detail::focusRing(dl, a, b, geist::radius::SM, h.focused);
        }
    }

    // Active 2px t.ink bottom indicator.
    const flex::Rect curTabR = fx.rect(tabSlots[std::min(current, n - 1)]);
    const float barX = selectionSpring(detail::salt(id, 10), curTabR.x);
    const float barW = selectionSpring(detail::salt(id, 11), curTabR.w);
    if (dl) dl->AddRectFilled(snapPt(barX, barBottom - barH), snapPt(barX + barW, barBottom), t.ink);
    return changed;
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
// Slider (Web .range): 18px hit, 4px track (t.outline rest, t.ink active fill),
// 14×14px circular thumb (t.surface + 1px t.outlineInput + Finish::Knob shadow).
// No tick marks below track. Quantiles 0/25/50/75/100% magnetize within 2.2%.
// ---------------------------------------------------------------------------
inline bool qualitySlider(ImDrawList* dl, ImGuiID id, ImVec2 pos, float width,
                          int& value, int minVal, int maxVal, bool enabled = true) {
    if (!(width > 1.0f) || maxVal <= minVal) return false;
    pos = snapPt(pos);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .align = flex::Align::Center,
    });
    const flex::Slot trackSlot = row.slot(flex::Grow(1.0f), flex::Fixed(geom::SLIDER_TRACK));
    const flex::Rect box = fx.solve(flex::Rect{pos.x, pos.y, width, geom::SLIDER_H});
    const flex::Rect tr = fx.rect(trackSlot);

    const detail::Hit h = detail::hit(id, box.p0(), box.p1(), enabled);
    bool changed = false;
    if (enabled && h.held) {
        const float tpos = (ImGui::GetIO().MousePos.x - tr.x) / std::max(1.0f, tr.w);
        const int next = quantizeSliderValue(tpos, minVal, maxVal, geom::SNAP_FRAC);
        if (next != value) {
            value = next;
            changed = true;
        }
    }
    if (enabled && h.focused) {
        int dir = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dir = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dir = 1;
        if (dir != 0) {
            value = std::clamp(value + dir, minVal, maxVal);
            changed = true;
        }
    }

    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float actT = motion::hoverFactor(detail::salt(id, 6), enabled && h.held);
    if (dl) {
        const float frac = std::clamp(static_cast<float>(value - minVal) / static_cast<float>(maxVal - minVal), 0.0f, 1.0f);
        const auto& t = geist::tokens();
        const float trackR = geom::SLIDER_TRACK * 0.5f;
        const float fillW = snapPx(frac * tr.w);
        const ImU32 trackBg = enabled ? motion::mix(t.outline, t.outlineStrong, hovT * 0.5f) : t.outline;
        dl->AddRectFilled(tr.p0(), tr.p1(), trackBg, trackR);
        if (fillW > 0.0f) {
            dl->AddRectFilled(tr.p0(), ImVec2(tr.x + fillW, tr.bottom()),
                              enabled ? t.ink : t.mute, trackR);
        }
        const float baseKnobR = geom::SLIDER_KNOB * 0.5f;
        const float knobR = baseKnobR + (enabled ? (hovT * 0.75f + actT * 0.5f) : 0.0f);
        const float cx = std::clamp(tr.x + fillW, tr.x + baseKnobR, tr.right() - baseKnobR);
        const ImVec2 c = snapPt(cx, tr.midY());
        const ImVec2 k0(c.x - knobR, c.y - knobR);
        const ImVec2 k1(c.x + knobR, c.y + knobR);
        finish(dl, k0, k1, Finish::Knob, knobR);
        dl->AddCircleFilled(c, knobR, t.surface, 28);
        const ImU32 knobBorder = motion::mix(t.outlineInput, t.ink, enabled ? std::max(hovT * 0.55f, actT) : 0.0f);
        dl->AddCircle(c, knobR - hairlinePx() * 0.5f, knobBorder, 28, hairlinePx());
        detail::focusRing(dl, box.p0(), box.p1(), geist::radius::MD, h.focused);
    }
    return changed;
}

inline bool slider(ImDrawList* dl, ImGuiID id, ImVec2 pos, float width,
                   int& value, int minVal, int maxVal, bool enabled = true) {
    return qualitySlider(dl, id, pos, width, value, minVal, maxVal, enabled);
}

// Labeled slider row with right-aligned mono readout. Returns true when value changes.
inline bool sliderRow(ImDrawList* dl, ImGuiID id, const char* label, ImFont* sans, ImFont* mono,
                      ImVec2 pos, float width, int& value, int minVal, int maxVal,
                      const char* suffix = nullptr, bool enabled = true) {
    if (!(width > 1.0f)) return false;
    pos = snapPt(pos);
    const auto& t = geist::tokens();
    const char* safeLabel = label ? label : "";
    char valBuf[32];
    std::snprintf(valBuf, sizeof(valBuf), "%d%s", value, suffix ? suffix : "");
    const ImVec2 lsz = calcText(sans, geist::type::BODY, safeLabel);
    const ImVec2 vsz = calcText(mono, geist::type::BODY_SMALL, valBuf);
    const float headH = std::max(lsz.y, vsz.y);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto col = fx.col({
        .gap = 6.0f,
        .width = flex::Fixed(width),
        .height = flex::Auto(),
    });
    auto headRow = col.row({
        .gap = 8.0f,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Fixed(headH),
    });
    const flex::Slot lblSlot = headRow.slot(flex::Grow(1.0f).min(16.0f), flex::Fixed(lsz.y));
    const flex::Slot valSlot = headRow.slot(flex::Fixed(std::ceil(vsz.x)), flex::Fixed(vsz.y));
    const flex::Slot sliderSlot = col.slot(flex::Grow(1.0f), flex::Fixed(geom::SLIDER_H));
    fx.solveWidth(pos, width);

    const flex::Rect lr = fx.rect(lblSlot);
    const flex::Rect vr = fx.rect(valSlot);
    const flex::Rect sr = fx.rect(sliderSlot);

    ellipsisTextCentered(dl, sans, geist::type::BODY, lr.x, lr.y, lr.bottom(), lr.w,
                         enabled ? t.body : t.faint, safeLabel);
    drawTextCenteredY(dl, mono, geist::type::BODY_SMALL, vr.x, vr.y, vr.bottom(),
                      enabled ? t.ink : t.faint, valBuf);
    return qualitySlider(dl, id, sr.p0(), sr.w, value, minVal, maxVal, enabled);
}

// 56×26 recessed field, mono-ish digits. Live-clamps into [min,max] on commit
// (deactivate); in-range edits write through immediately. Returns true when
// `value` changes.
inline bool qualityField(ImDrawList* dl, ImGuiID id, ImVec2 pos, int& value, int minVal, int maxVal,
                         ImFont* font) {
    pos = snapPt(pos);
    constexpr float kBoxW = 56.0f;
    constexpr float kBoxH = 26.0f;

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(8.0f, 0.0f),
        .align = flex::Align::Center,
    });
    const flex::Slot inSlot = row.slot(flex::Grow(1.0f), flex::Fraction(1.0f));
    const flex::Rect box = fx.solve(flex::Rect{pos.x, pos.y, kBoxW, kBoxH});
    const flex::Rect inR = fx.rect(inSlot);

    const auto& t = geist::tokens();
    if (dl) {
        dl->AddRectFilled(box.p0(), box.p1(), t.recessed, 5.0f);
    }

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
    const float padY = snapPx(std::max(0.0f, drawTextPosYCentered(font, geist::type::MONO,
                                                                  inR.y, inR.bottom(), b.text) - inR.y));
    ImGui::SetCursorScreenPos(snapPt(inR.p0()));
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    if (font) ImGui::PushFont(font, geist::type::MONO);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, drewNative ? t.ink : IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(std::max(12.0f, inR.w));
    char item[24];
    std::snprintf(item, sizeof(item), "##qf%08x", static_cast<unsigned>(id));
    const bool edited = ImGui::InputText(item, b.text, sizeof(b.text), ImGuiInputTextFlags_CharsDecimal);
    const bool hovered = ImGui::IsItemHovered();
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
    const float hovT = motion::hoverFactor(detail::salt(id, 5), hovered && !focused);
    if (!drewNative && dl) {
        drawTextCentered(dl, font, geist::type::MONO, box.p0(), box.p1(), t.ink, b.text);
    }
    if (focused) {
        detail::inputFocusHalo(dl, box.p0(), box.p1(), 5.0f, true);
    } else if (dl) {
        dl->AddRect(box.p0(), box.p1(), motion::mix(t.outlineInput, t.mute, hovT * 0.45f), 5.0f, 0, hairlinePx());
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (font) ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

// Toggle 32×18, 14px knob, 14px PLACE travel, 2px inset (matching Web .switch).
// Off track: t.outlineStrong (hover t.mute), off knob: t.switchKnobOff.
// On track: t.ink (hover t.inkHover), on knob: t.onInk. Returns true when flipped.
inline bool toggle(ImDrawList* dl, ImGuiID id, ImVec2 pos, bool& checked, bool enabled = true) {
    pos = snapPt(pos);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::all(2.0f),
        .align = flex::Align::Center,
    });
    const flex::Slot trackSlot = row.slot(flex::Grow(1.0f), flex::Fixed(geom::TOGGLE_KNOB));
    const flex::Rect box = fx.solve(flex::Rect{pos.x, pos.y, geom::TOGGLE_W, geom::TOGGLE_H});
    const flex::Rect tr = fx.rect(trackSlot);

    const detail::Hit h = detail::hit(id, box.p0(), box.p1(), enabled);
    if (h.clicked) checked = !checked;
    const float travel = switchKnob(detail::salt(id, 5), checked);
    const float u = motion::approach(detail::salt(id, 6), checked ? 1.0f : 0.0f, motion::BASE, checked ? 1.0f : 0.0f);
    const float hovT = motion::hoverFactor(detail::salt(id, 7), enabled && h.hovered);

    if (dl) {
        const auto& t = geist::tokens();
        const ImU32 offTrack = motion::mix(t.outlineStrong, t.mute, hovT);
        const ImU32 onTrack = motion::mix(t.ink, t.inkHover, hovT);
        const ImU32 track = enabled ? motion::mix(offTrack, onTrack, u) : t.container;
        dl->AddRectFilled(box.p0(), box.p1(), track, geom::TOGGLE_H * 0.5f);
        if (!enabled) {
            dl->AddRect(box.p0(), box.p1(), t.outline, geom::TOGGLE_H * 0.5f, 0, hairlinePx());
        }
        const float kr = geom::TOGGLE_KNOB * 0.5f;
        const float travelRange = std::max(0.0f, tr.w - geom::TOGGLE_KNOB);
        const ImVec2 c = snapPt(tr.x + kr + travel * travelRange, tr.midY());
        const ImVec2 k0(c.x - kr, c.y - kr);
        const ImVec2 k1(c.x + kr, c.y + kr);
        detail::drawOuter(dl, k0, k1, kr, shadowBlack(1.0f, 2.0f, 0.0f, 0.16f));
        const ImU32 knob = motion::mix(t.switchKnobOff, t.onInk, u);
        dl->AddCircleFilled(c, kr, enabled ? knob : motion::mix(knob, t.mute, 0.45f), 28);
        detail::focusRing(dl, box.p0(), box.p1(), geom::TOGGLE_H * 0.5f, h.focused, 2.0f);
    }
    return h.clicked;
}

// Aliases matching DESIGN / blueprint names.
inline bool qualityInput(ImDrawList* dl, ImGuiID id, ImVec2 pos, int& value, int minVal, int maxVal, ImFont* font) {
    return qualityField(dl, id, pos, value, minVal, maxVal, font);
}

inline bool toggleSwitch(ImDrawList* dl, ImGuiID id, ImVec2 pos, bool& checked, bool enabled = true) {
    return toggle(dl, id, pos, checked, enabled);
}

// Search field with leading magnifying glass and clickable clear "x" when non-empty.
// Default mode draws a bordered surface with accent focus halo; `underlineOnly`
// draws a borderless field anchored by a 1px bottom rule (Models toolbar).
inline bool searchBox(ImDrawList* dl, ImGuiID id, ImVec2 pos, float width, float height,
                      char* buf, size_t bufCap, const char* placeholder, ImFont* font,
                      bool underlineOnly = false) {
    if (!buf || bufCap == 0 || width <= 40.0f) return false;
    pos = snapPt(pos);
    const auto& t = geist::tokens();
    const bool hasText = buf[0] != '\0';

    static flex::Context srchCtx;
    flex::Layout fx(srchCtx);
    auto row = fx.row({
        .padding = flex::Edges::only(underlineOnly ? 2.0f : 8.0f, 0.0f, 4.0f, 0.0f),
        .gap = 6.0f,
        .align = flex::Align::Center,
    });
    const flex::Slot iconSlot = row.slot(flex::Fixed(14.0f), flex::Fixed(14.0f));
    const flex::Slot inputSlot = row.slot(flex::Grow(1.0f).min(16.0f), flex::Fraction(1.0f));
    const flex::Slot clearSlot = row.slot(flex::Fixed(20.0f), flex::Fixed(20.0f),
                                          flex::AlignSelf::Center, {}, hasText);
    const flex::Rect box = fx.solve(flex::Rect{pos.x, pos.y, width, height});

    if (dl && !underlineOnly) {
        dl->AddRectFilled(box.p0(), box.p1(), t.surface, geist::radius::MD);
    }

    const flex::Rect iconR = fx.rect(iconSlot);
    const flex::Rect inR = fx.rect(inputSlot);
    if (dl) drawIcon(dl, "search", iconR.center(), 14.0f, t.mute, 1.5f);

    const float textPx = (height < 28.0f) ? geist::type::BODY_SMALL : geist::type::BODY;
    const char* safePlaceholder = placeholder ? placeholder : "";
    const float padY = snapPx(std::max(0.0f, drawTextPosYCentered(font, textPx, inR.y, inR.bottom(),
                                                                  hasText ? buf : safePlaceholder) - inR.y));

    bool changed = false;
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    ImGui::SetCursorScreenPos(snapPt(inR.p0()));
    if (font) ImGui::PushFont(font, textPx);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, t.ink);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, t.faint);
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, t.accentSoft);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(snapPx(2.0f), padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(std::max(16.0f, inR.w));
    char item[24];
    std::snprintf(item, sizeof(item), "##srch%08x", static_cast<unsigned>(id));
    if (ImGui::InputTextWithHint(item, safePlaceholder, buf, bufCap)) {
        changed = true;
    }
    const bool focused = ImGui::IsItemActive() || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    const bool hovered = ImGui::IsItemHovered() || (ImGui::IsWindowHovered() && mouseIn(box.p0(), box.p1()));
    const float hovT = motion::hoverFactor(detail::salt(id, 5), hovered && !focused);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(5);
    if (font) ImGui::PopFont();
    ImGui::PopID();

    if (dl) {
        if (underlineOnly) {
            const float lineH = focused ? std::max(hairlinePx(), snapPx(2.0f)) : hairlinePx();
            const ImU32 lineCol = focused ? t.outlineStrong : motion::mix(t.outline, t.outlineStrong, hovT);
            dl->AddRectFilled(snapPt(box.x, box.bottom() - lineH), box.p1(), lineCol);
        } else if (focused) {
            detail::inputFocusHalo(dl, box.p0(), box.p1(), geist::radius::MD, true);
        } else {
            dl->AddRect(box.p0(), box.p1(), motion::mix(t.outlineInput, t.mute, hovT * 0.45f), geist::radius::MD, 0, hairlinePx());
        }
    }

    if (fx.isVisible(clearSlot)) {
        const flex::Rect clrR = fx.rect(clearSlot);
        const ImVec2 c0 = snapPt(clrR.p0());
        const ImVec2 c1 = snapPt(clrR.p1());
        const detail::Hit clrHit = detail::hit(detail::salt(id, 9), c0, c1, true);
        const float clrHovT = motion::hoverFactor(detail::salt(id, 10), clrHit.hovered);
        if (dl) {
            if (clrHit.held || clrHovT > 0.01f) {
                hoverWashAlpha(dl, c0, c1, geist::radius::SM, clrHit.held ? 1.0f : clrHovT, clrHit.held ? 0.14f : 0.09f);
            }
            drawIcon(dl, "x", clrR.center(), 11.0f,
                     motion::mix(t.mute, t.ink, clrHit.held ? 1.0f : clrHovT), 1.5f);
        }
        if (clrHit.clicked) {
            buf[0] = '\0';
            changed = true;
        }
    }
    return changed;
}

} // namespace upix::ui::ctl
