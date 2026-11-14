// ui/controls/buttons.hpp — Geist action buttons: ghost, chip, primary CTA,
// secondary, quiet, icon-only, and two-step destructive confirm buttons.
#pragma once

#include "ui/controls/chips.hpp"
#include "ui/controls/surfaces.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

namespace upix::ui::ctl {

namespace detail {

struct ConfirmState {
    bool armed = false;
    float remainingSec = 0.0f;
};

inline std::unordered_map<ImGuiID, ConfirmState>& confirmStore() {
    static std::unordered_map<ImGuiID, ConfirmState> store;
    return store;
}

} // namespace detail

inline bool isConfirmArmed(ImGuiID id) {
    const auto& store = detail::confirmStore();
    const auto it = store.find(id);
    return it != store.end() && it->second.armed;
}

inline void disarmConfirm(ImGuiID id) {
    auto& store = detail::confirmStore();
    const auto it = store.find(id);
    if (it != store.end()) {
        it->second.armed = false;
        it->second.remainingSec = 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Quiet ghost chip (h24, px8, radius 6): no fill until hover (container),
// muted 11px label. Press spring 0.97. Disabled: faint text, no press.
// ---------------------------------------------------------------------------
inline bool ghostButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                        ImVec2 p0, float alignW = 0.0f, bool enabled = true) {
    const auto& t = geist::tokens();
    const char* safeLabel = label ? label : "";
    const ImVec2 ts = calcText(font, geom::LABEL, safeLabel);
    p0 = snapPt(p0);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(geom::GHOST_PX, 0.0f),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .width = (alignW > 0.0f) ? flex::Fixed(alignW) : flex::Auto(),
        .height = flex::Fixed(geom::GHOST_H),
    });
    const flex::Slot lblSlot = row.slot(flex::Shrink(std::ceil(ts.x), 1.0f).min(0.0f), flex::Fixed(ts.y));
    const flex::Rect box = fx.solveAuto(p0);
    const flex::Rect lr = fx.rect(lblSlot);

    const detail::Hit h = detail::hit(id, box.p0(), box.p1(), enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float press = motion::pressScale(id, h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(box.p0(), box.p1(), enabled ? press : 1.0f, q0, q1);
    if (dl) {
        if (enabled && (h.held || hovT > 0.01f)) {
            hoverWashAlpha(dl, q0, q1, geist::radius::MD, h.held ? 1.0f : hovT, h.held ? 0.13f : 0.09f);
        }
        const ImU32 ink = enabled ? motion::mix(t.mute, t.ink, h.held ? 1.0f : hovT) : t.faint;
        if (ts.x > lr.w) {
            const std::string fitted = fitTextEllipsis(font, geom::LABEL, safeLabel, lr.w);
            drawTextCentered(dl, font, geom::LABEL, q0, q1, ink, fitted.c_str());
        } else {
            drawTextCentered(dl, font, geom::LABEL, q0, q1, ink, safeLabel);
        }
        detail::focusRing(dl, box.p0(), box.p1(), geist::radius::MD, h.focused);
    }
    return h.clicked;
}

// Chip button (h24, px8, radius 6): container fill + hairline, 11px ink.
// Hover eases toward `high`. Press spring 0.97.
inline bool chipButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                       ImVec2 p0, float alignW = 0.0f, bool enabled = true) {
    const auto& t = geist::tokens();
    const char* safeLabel = label ? label : "";
    const ImVec2 ts = calcText(font, geom::LABEL, safeLabel);
    p0 = snapPt(p0);

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .padding = flex::Edges::xy(geom::GHOST_PX, 0.0f),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .width = (alignW > 0.0f) ? flex::Fixed(alignW) : flex::Auto(),
        .height = flex::Fixed(geom::GHOST_H),
    });
    const flex::Slot lblSlot = row.slot(flex::Shrink(std::ceil(ts.x), 1.0f).min(0.0f), flex::Fixed(ts.y));
    const flex::Rect box = fx.solveAuto(p0);
    const flex::Rect lr = fx.rect(lblSlot);

    const detail::Hit h = detail::hit(id, box.p0(), box.p1(), enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float press = motion::pressScale(detail::salt(id, 1), h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(box.p0(), box.p1(), enabled ? press : 1.0f, q0, q1);
    if (dl) {
        if (!enabled) {
            finish(dl, q0, q1, Finish::Raised, geist::radius::MD);
            dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, hairlinePx());
        } else {
            const ImU32 activeBg = (geist::mode() == geist::Mode::Dark) ? t.highest : t.high;
            const ImU32 bg = h.held ? activeBg : motion::mix(t.container, t.high, hovT);
            dl->AddRectFilled(q0, q1, bg, geist::radius::MD);
            dl->AddRect(q0, q1, motion::mix(t.outline, t.outlineStrong, h.held ? 1.0f : hovT), geist::radius::MD, 0, hairlinePx());
        }
        const ImU32 ink = enabled ? t.ink : t.mute;
        if (ts.x > lr.w) {
            const std::string fitted = fitTextEllipsis(font, geom::LABEL, safeLabel, lr.w);
            drawTextCentered(dl, font, geom::LABEL, q0, q1, ink, fitted.c_str());
        } else {
            drawTextCentered(dl, font, geom::LABEL, q0, q1, ink, safeLabel);
        }
        detail::focusRing(dl, box.p0(), box.p1(), geist::radius::MD, h.focused);
    }
    return h.clicked;
}

// Main CTA. Solid ink, NO scale (§2.2) — PRESS spring sinks 1 physical pixel
// and the fill steps Primary → PrimaryHover → PrimaryPress. Disabled: flat
// ctaDisabledBg + 1px outline (NO shadow), ctaDisabledFg label, no press.
inline bool primaryCta(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                       ImVec2 p0, ImVec2 p1, bool enabled = true, const char* icon = nullptr) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float sinkT = motion::springTo(detail::salt(id, 2), (enabled && h.held) ? 1.0f : 0.0f, motion::PRESS, 0.0f);
    const float dy = snapPx(sinkT);
    ImVec2 q0(p0.x, p0.y + dy);
    ImVec2 q1(p1.x, p1.y + dy);
    if (dl) {
        if (!enabled) {
            dl->AddRectFilled(q0, q1, t.ctaDisabledBg, geist::radius::MD);
            dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, hairlinePx());
        } else if (h.held) {
            finish(dl, q0, q1, Finish::PrimaryPress, geist::radius::MD);
        } else {
            finish(dl, q0, q1, Finish::Primary, geist::radius::MD);
            if (hovT > 0.01f) {
                dl->AddRectFilled(q0, q1, motion::mix(t.ink, t.inkHover, hovT), geist::radius::MD);
            }
        }
        const ImU32 ink = enabled ? t.onInk : t.ctaDisabledFg;
        drawIconLabelCentered(dl, q0, q1, font, geist::type::BODY, icon, 15.0f, 8.0f, ink, label);
        detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    }
    return h.clicked;
}

inline bool primaryButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                          ImVec2 p0, ImVec2 p1, bool enabled = true, const char* icon = nullptr) {
    return primaryCta(dl, id, label, font, p0, p1, enabled, icon);
}

// Secondary / destructive action button (flat hairline surface, NO drop shadow, press scale 0.97).
// When destructive=true: borderless t.dangerBg + t.error text (hover t.dangerBgHover + t.errorHover, press t.dangerBgPress).
inline bool secondaryButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                            ImVec2 p0, ImVec2 p1, const char* icon = nullptr,
                            bool enabled = true, bool destructive = false) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 8), enabled && h.hovered);
    const float press = motion::pressScale(detail::salt(id, 7), h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);

    if (dl) {
        ImU32 ink = t.ink;
        if (destructive) {
            if (!enabled) {
                dl->AddRectFilled(q0, q1, t.ctaDisabledBg, geist::radius::MD);
                dl->AddRect(q0, q1, t.outline, geist::radius::MD, 0, hairlinePx());
                ink = t.faint;
            } else {
                const ImU32 bg = h.held ? t.dangerBgPress : motion::mix(t.dangerBg, t.dangerBgHover, hovT);
                dl->AddRectFilled(q0, q1, bg, geist::radius::MD);
                ink = motion::mix(t.error, t.errorHover, h.held ? 1.0f : hovT);
            }
        } else {
            detail::drawRaisedFace(dl, q0, q1, geist::radius::MD, enabled, hovT, true, h.held);
            ink = enabled ? t.ink : t.faint;
        }

        drawIconLabelCentered(dl, q0, q1, font, geist::type::BODY, icon, 14.0f, 8.0f, ink, label);
        detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    }
    return h.clicked;
}

// Quiet text/icon button: transparent & borderless at rest.
// When destructive=true: t.error text at rest, t.dangerBgHover + t.errorHover on hover, t.dangerBgPress on press.
inline bool quietButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                        ImVec2 p0, ImVec2 p1, const char* icon = nullptr,
                        bool enabled = true, bool destructive = false,
                        float fontSize = geist::type::BODY_SMALL) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float press = motion::pressScale(detail::salt(id, 7), h.held, motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);

    if (dl) {
        if (enabled) {
            if (destructive) {
                if (h.held) dl->AddRectFilled(q0, q1, t.dangerBgPress, geist::radius::MD);
                else if (hovT > 0.01f) dl->AddRectFilled(q0, q1, upix::ui::withAlpha(t.dangerBgHover, hovT), geist::radius::MD);
            } else if (h.held || hovT > 0.01f) {
                hoverWashAlpha(dl, q0, q1, geist::radius::MD, h.held ? 1.0f : hovT, h.held ? 0.13f : 0.09f);
            }
        }

        const ImU32 ink = !enabled
            ? t.faint
            : (destructive
                ? motion::mix(t.error, t.errorHover, h.held ? 1.0f : hovT)
                : motion::mix(t.body, t.ink, h.held ? 1.0f : hovT));

        drawIconLabelCentered(dl, q0, q1, font, fontSize, icon, 14.0f, 6.0f, ink, label ? label : "");
        detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    }
    return h.clicked;
}

inline bool dangerQuietButton(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font,
                              ImVec2 p0, ImVec2 p1, const char* icon = nullptr,
                              bool enabled = true, float fontSize = geist::type::BODY_SMALL) {
    return quietButton(dl, id, label, font, p0, p1, icon, enabled, true, fontSize);
}

// Pure-icon control. Default borderless (.icon-btn): transparent at rest,
// t.container on hover. Pass bordered=true for Raised face + hairline.
// Press spring 0.94, tooltip from `tip` (500ms / focus immediate / Esc).
inline bool iconAction(ImDrawList* dl, ImGuiID id, ImVec2 p0, float box, const char* icon,
                       const char* tip, ImFont* font, float iconPx = geom::ICON,
                       float radius = geist::radius::MD, bool enabled = true,
                       bool bordered = false, bool destructive = false) {
    p0 = snapPt(p0);
    const ImVec2 p1 = snapPt(p0.x + box, p0.y + box);
    const detail::Hit h = detail::hit(id, p0, p1, enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 5), enabled && h.hovered);
    const float press = motion::pressScale(detail::salt(id, 3), h.held, motion::PRESS_SCALE_ICON);
    ImVec2 q0, q1;
    detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);
    const auto& t = geist::tokens();

    static flex::Context ctx;
    flex::Layout fx(ctx);
    auto row = fx.row({
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    const flex::Slot iconSlot = row.slot(flex::Fixed(iconPx), flex::Fixed(iconPx));
    fx.solve(flex::Rect{q0.x, q0.y, std::max(0.0f, q1.x - q0.x), std::max(0.0f, q1.y - q0.y)});

    if (dl) {
        if (bordered) {
            detail::drawRaisedFace(dl, q0, q1, radius, enabled, hovT, false, h.held);
        } else if (enabled && (h.held || hovT > 0.01f)) {
            if (destructive) {
                dl->AddRectFilled(q0, q1,
                                  h.held ? t.dangerBgPress : detail::withAlpha(t.dangerBgHover, hovT),
                                  radius);
            } else {
                hoverWashAlpha(dl, q0, q1, radius, h.held ? 1.0f : hovT, h.held ? 0.14f : 0.09f);
            }
        }
        const ImU32 hoverInk = destructive ? (h.held ? t.errorHover : t.error) : t.ink;
        const ImU32 ink = enabled ? motion::mix(t.mute, hoverInk, h.held ? 1.0f : hovT) : t.faint;
        drawIcon(dl, icon, fx.rect(iconSlot).center(), iconPx, ink);
        detail::focusRing(dl, p0, p1, radius, h.focused);
    }
    if (enabled) tooltip(dl, detail::salt(id, 4), p0, p1, tip, font, h.hovered, h.focused);
    return h.clicked;
}

inline bool iconButton(ImDrawList* dl, ImGuiID id, ImVec2 p0, float box, const char* icon,
                       const char* tip, ImFont* font, float iconPx = geom::ICON,
                       float radius = geist::radius::MD, bool enabled = true,
                       bool bordered = false) {
    return iconAction(dl, id, p0, box, icon, tip, font, iconPx, radius, enabled, bordered);
}

// Two-step inline confirmation for destructive actions.
// Click 1 arms the control; click 2 fires and returns true. Clicking outside,
// pressing Esc, or waiting `disarmSeconds` disarms automatically.
// Pass `restLabel = nullptr` with an `icon` for an `.icon-btn` that expands into
// a danger pill when armed. Default `quietRest = true, destructiveRest = true`
// renders transparent & borderless at rest with `t.error` text (`t.dangerBgHover`
// on hover) and borderless `t.dangerBg` + `t.error` pill when armed.
inline bool confirmButton(ImDrawList* dl, ImGuiID id, const char* restLabel, const char* confirmLabel,
                          ImFont* font, ImVec2 p0, ImVec2 p1, bool enabled = true,
                          const char* icon = nullptr, const char* tip = nullptr,
                          float iconPx = 14.0f, float disarmSeconds = 3.0f,
                          bool quietRest = true, bool destructiveRest = true) {
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    auto& st = detail::confirmStore()[id];

    if (!enabled) {
        st.armed = false;
        st.remainingSec = 0.0f;
    } else if (st.armed) {
        const float dt = ImGui::GetCurrentContext() ? ImGui::GetIO().DeltaTime : 0.0f;
        st.remainingSec -= dt;
        if (st.remainingSec <= 0.0f || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            st.armed = false;
            st.remainingSec = 0.0f;
        }
    }

    const char* armText = (confirmLabel && confirmLabel[0])
        ? confirmLabel
        : upix::i18n::tr(upix::i18n::Key::CommonConfirmQ);
    ImVec2 r0 = p0;
    ImVec2 r1 = p1;
    const bool iconOnlyRest = (!restLabel || !restLabel[0]) && icon && icon[0];
    if (st.armed && iconOnlyRest) {
        const ImVec2 armTs = calcText(font, geist::type::BODY_SMALL, armText);
        static flex::Context armCtx;
        flex::Layout armFx(armCtx);
        auto armRow = armFx.row({
            .padding = flex::Edges::xy(8.0f, 0.0f),
            .justify = flex::Justify::Center,
            .align = flex::Align::Center,
        });
        armRow.slot(flex::Fixed(std::ceil(armTs.x)), flex::Fixed(armTs.y));
        const flex::Rect armBox = armFx.solveAuto(p0);
        if (armBox.w > (p1.x - p0.x)) {
            r0.x = snapPx(p1.x - armBox.w);
        }
    }

    if (st.armed && !mouseIn(r0, r1) &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        st.armed = false;
        st.remainingSec = 0.0f;
        r0 = p0;
        r1 = p1;
    }

    const detail::Hit h = detail::hit(id, r0, r1, enabled);
    const float hovT = motion::hoverFactor(detail::salt(id, 6), enabled && h.hovered);
    const float press = motion::pressScale(detail::salt(id, 7), h.held,
                                           (iconOnlyRest && !st.armed) ? motion::PRESS_SCALE_ICON : motion::PRESS_SCALE);
    ImVec2 q0, q1;
    detail::scaled(r0, r1, enabled ? press : 1.0f, q0, q1);

    if (dl) {
        if (st.armed) {
            const ImU32 bg = h.held ? t.dangerBgPress : motion::mix(t.dangerBg, t.dangerBgHover, hovT);
            const ImU32 fg = motion::mix(t.error, t.errorHover, h.held ? 1.0f : hovT);
            dl->AddRectFilled(q0, q1, bg, geist::radius::MD);
            const float fs = iconOnlyRest ? geist::type::BODY_SMALL : geist::type::BODY;
            drawIconLabelCentered(dl, q0, q1, font, fs, nullptr, 0.0f, 0.0f, fg, armText);
        } else if (iconOnlyRest) {
            if (enabled) {
                if (h.held) dl->AddRectFilled(q0, q1, t.dangerBgPress, geist::radius::MD);
                else if (hovT > 0.01f) dl->AddRectFilled(q0, q1, upix::ui::withAlpha(t.dangerBgHover, hovT), geist::radius::MD);
            }
            const ImU32 ink = enabled ? motion::mix(t.mute, t.error, h.held ? 1.0f : hovT) : upix::ui::withAlpha(t.faint, 0.55f);
            drawIconLabelCentered(dl, q0, q1, font, geist::type::BODY_SMALL, icon, iconPx, 0.0f, ink, "");
        } else if (quietRest) {
            if (enabled) {
                if (destructiveRest) {
                    if (h.held) dl->AddRectFilled(q0, q1, t.dangerBgPress, geist::radius::MD);
                    else if (hovT > 0.01f) dl->AddRectFilled(q0, q1, upix::ui::withAlpha(t.dangerBgHover, hovT), geist::radius::MD);
                } else if (h.held || hovT > 0.01f) {
                    hoverWashAlpha(dl, q0, q1, geist::radius::MD, h.held ? 1.0f : hovT, h.held ? 0.13f : 0.09f);
                }
            }
            const ImU32 ink = !enabled
                ? t.faint
                : (destructiveRest
                    ? motion::mix(t.error, t.errorHover, h.held ? 1.0f : hovT)
                    : motion::mix(t.body, t.ink, h.held ? 1.0f : hovT));
            drawIconLabelCentered(dl, q0, q1, font, geist::type::BODY_SMALL, icon, iconPx, 6.0f,
                                  ink, restLabel ? restLabel : "");
        } else {
            detail::drawRaisedFace(dl, q0, q1, geist::radius::MD, enabled, hovT, true, h.held);
            const ImU32 ink = enabled ? motion::mix(t.body, t.ink, hovT) : t.faint;
            drawIconLabelCentered(dl, q0, q1, font, geist::type::BODY, icon, iconPx, 6.0f,
                                  ink, restLabel ? restLabel : "");
        }
        detail::focusRing(dl, r0, r1, geist::radius::MD, h.focused);
    }

    if (!st.armed && iconOnlyRest && enabled && tip && tip[0]) {
        tooltip(dl, detail::salt(id, 4), r0, r1, tip, font, h.hovered, h.focused);
    }

    if (h.clicked && enabled) {
        if (!st.armed) {
            st.armed = true;
            st.remainingSec = disarmSeconds;
            return false;
        }
        st.armed = false;
        st.remainingSec = 0.0f;
        return true;
    }
    return false;
}

} // namespace upix::ui::ctl
