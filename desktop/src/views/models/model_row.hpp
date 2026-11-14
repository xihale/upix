// views/models/model_row.hpp — Flex-laid-out model row widget and accent icon action.
#pragma once

#include "ui/flex.hpp"
#include "views/common.hpp"
#include "views/models/grouping.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace upix::views::models_detail {

inline constexpr float kActionIconBox = 28.0f;

inline float modelRowHeight(ImFont* sansMed) {
    using namespace upix::ui;
    const float textH = std::ceil(calcText(sansMed, geist::type::BODY, upix::i18n::tr(upix::i18n::Key::ModelsProbeText)).y);
    return std::max(ctl::geom::ROW_MIN_H, std::max(textH, ctl::geom::META_CHIP_H) + 22.0f);
}

inline bool accentIconButton(ImDrawList* dl, ImGuiID id, ImVec2 p0, float box, const char* icon,
                             const char* tip, ImFont* font, bool enabled) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    const ImVec2 p1 = snapPt(p0.x + box, p0.y + box);
    const ctl::detail::Hit h = ctl::detail::hit(id, p0, p1, enabled);
    const float hovT = motion::hoverFactor(ctl::detail::salt(id, 5), enabled && h.hovered);
    const float press = motion::pressScale(ctl::detail::salt(id, 3), h.held, motion::PRESS_SCALE_ICON);
    ImVec2 q0, q1;
    ctl::detail::scaled(p0, p1, enabled ? press : 1.0f, q0, q1);

    if (enabled && (h.held || hovT > 0.01f)) {
        const float fillA = h.held ? 0.20f : 0.12f * hovT;
        const float borderA = h.held ? 0.40f : 0.26f * hovT;
        dl->AddRectFilled(q0, q1, ctl::detail::withAlpha(t.accent, fillA), geist::radius::MD);
        dl->AddRect(q0, q1, ctl::detail::withAlpha(t.accent, borderA), geist::radius::MD, 0, hairlinePx());
    }
    const ImU32 ink = !enabled
        ? t.faint
        : motion::mix(t.accent, t.accentHover, h.held ? 1.0f : hovT);
    static flex::Context btnCtx;
    flex::Layout fx(btnCtx);
    auto row = fx.row({.justify = flex::Justify::Center, .align = flex::Align::Center});
    const auto iconSlot = row.slot(flex::Fixed(14.0f), flex::Fixed(14.0f));
    fx.solve(q0, q1);
    drawIcon(dl, icon, fx.rect(iconSlot).center(), 14.0f, ink, 1.5f);
    ctl::detail::focusRing(dl, p0, p1, geist::radius::MD, h.focused);
    if (tip && tip[0]) {
        ctl::tooltip(dl, ctl::detail::salt(id, 4), p0, p1, tip, font, h.hovered, h.focused);
    }
    return h.clicked;
}

inline void drawModelRow(ImDrawList* dl, ImFont* sans, ImFont* sansMed, ImFont* mono,
                         const upix::ui::flex::Rect& rowRect, float rowPadX,
                         const std::string& familyTitle,
                         const upix::gui::ModelItem& m, bool mouseInPane,
                         const upix::gui::Snapshot& snap, upix::gui::AppState& app) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool installed = !m.remote;
    const bool current = installed && m.name == snap.model;
    const bool downloading = m.remote && snap.downloading == m.name;

    ImGui::PushID(installed ? "i" : "r");
    ImGui::PushID(m.name.c_str());

    const std::string displayNameStr(stripFamilyPrefixForDisplay(familyTitle, m));
    const char* displayName = displayNameStr.c_str();
    const bool nameShortened = (displayNameStr != m.name);
    const ImVec2 nameSz = calcText(sansMed, geist::type::BODY, displayName);

    char scBuf[32] = {};
    const char* scLbl = ctl::sceneLabel(m.scene.c_str(), scBuf, sizeof(scBuf));
    const float sceneChipW = ctl::metaChipWidth(sans, scLbl, false, 5.0f);
    const std::string scaleLabel = std::format("{}×", m.scale);
    const float scaleChipW = ctl::metaChipWidth(sansMed, scaleLabel.c_str(), false, 5.0f);
    const float specColW = std::max(110.0f, scaleChipW + 5.0f + sceneChipW);
    const char* curLabel = upix::i18n::tr(upix::i18n::Key::CommonCurrent);
    const char* recLabel = upix::i18n::tr(upix::i18n::Key::CommonRecommended);
    const float curW = current ? ctl::metaChipWidth(sansMed, curLabel, true, 5.0f) : 0.0f;
    const float recW = m.recommended ? ctl::recChipWidth(sansMed, recLabel, 5.0f) : 0.0f;

    const std::string sizeStr = isMissingSize(m.size) ? std::string("—") : m.size;
    const ImVec2 sizeSz = calcText(mono, geist::type::MONO, sizeStr.c_str());
    const float sizeColW = std::max(84.0f, std::ceil(calcText(mono, geist::type::MONO, "999.99 MB").x) + 8.0f);

    const bool wideSpecs = (rowRect.w >= 540.0f);
    const bool showSize = (rowRect.w >= 620.0f);

    std::string pctText;
    if (downloading) {
        pctText = std::format("{}%", std::clamp(snap.download_pct, 0, 100));
    }
    const ImVec2 pctSz = calcText(mono, geist::type::MONO, downloading ? pctText.c_str() : "100%");
    const float pctColW = std::ceil(calcText(mono, geist::type::MONO, "100%").x) + 2.0f;
    const float rightClusterW = std::max(56.0f, pctColW + 14.0f);

    static flex::Context rowCtx;
    flex::Layout fx(rowCtx);
    auto rowRoot = fx.col({
        .padding = flex::Edges::xy(rowPadX, 0.0f),
        .width = flex::Fixed(rowRect.w),
        .height = flex::Fixed(rowRect.h),
    });
    auto r = rowRoot.row({
        .gap = geist::space::MD,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Grow(1.0f),
    });
    auto mainRow = r.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f).min(40.0f),
        .height = flex::Auto(),
    });
    const auto nameSlot = mainRow.slot(flex::Shrink(std::ceil(nameSz.x) + 2.0f, 1.0f).min(32.0f),
                                       flex::Fixed(nameSz.y));
    const auto curSlot = mainRow.slot(flex::Fixed(curW), flex::Fixed(ctl::geom::META_CHIP_H),
                                      flex::AlignSelf::Center, {}, current);
    const auto recSlot = mainRow.slot(flex::Fixed(recW), flex::Fixed(ctl::geom::META_CHIP_H),
                                      flex::AlignSelf::Center, {}, m.recommended);

    auto specRow = r.row({
        .gap = 5.0f,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Center,
        .width = flex::Fixed(specColW),
        .height = flex::Fixed(ctl::geom::META_CHIP_H),
        .visible = wideSpecs,
    });
    const auto scaleSlot = specRow.slot(flex::Fixed(scaleChipW), flex::Fixed(ctl::geom::META_CHIP_H),
                                        flex::AlignSelf::Center);
    const auto sceneSlot = specRow.slot(flex::Fixed(sceneChipW), flex::Fixed(ctl::geom::META_CHIP_H),
                                        flex::AlignSelf::Center);

    auto sizeBox = r.row({
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Center,
        .width = flex::Fixed(sizeColW),
        .height = flex::Fixed(sizeSz.y),
        .visible = showSize,
    });
    const auto sizeTextSlot = sizeBox.slot(flex::Fixed(std::ceil(sizeSz.x)), flex::Fixed(sizeSz.y),
                                           flex::AlignSelf::Center);

    auto actionRow = r.row({
        .gap = 6.0f,
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Center,
        .width = flex::Fixed(rightClusterW),
        .height = flex::Fixed(kActionIconBox),
    });
    const auto busySlot = actionRow.slot(flex::Fixed(8.0f), flex::Fixed(8.0f),
                                         flex::AlignSelf::Center, {}, downloading);
    const auto pctSlot = actionRow.slot(flex::Fixed(std::ceil(pctSz.x)), flex::Fixed(pctSz.y),
                                        flex::AlignSelf::Center, {}, downloading);
    const auto btnSlot = actionRow.slot(flex::Fixed(kActionIconBox), flex::Fixed(kActionIconBox),
                                        flex::AlignSelf::Center, {}, !downloading);

    const auto dlBarSlot = rowRoot.slot(flex::Grow(1.0f), flex::Fixed(2.0f),
                                        flex::AlignSelf::Stretch, {}, downloading);
    fx.solve(rowRect);

    const flex::Rect rr = rowRect;
    const flex::Rect actionR = actionRow.rect();

    // Clicking anywhere on a cached row selects that model (outside right action slot).
    bool rowHovered = false;
    bool rowClicked = false;
    bool rowFocused = false;
    if (installed && !downloading) {
        const ImVec2 hitP0(rr.x, rr.y);
        const ImVec2 hitP1(std::max(hitP0.x + 1.0f, actionR.x - 4.0f), rr.bottom());
        ImGui::SetCursorScreenPos(hitP0);
        rowClicked = ImGui::InvisibleButton("##row_sel", ImVec2(hitP1.x - hitP0.x, hitP1.y - hitP0.y));
        rowHovered = ImGui::IsItemHovered();
        rowFocused = ImGui::IsItemFocused();
        if (rowHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    const bool anyHover = rowHovered || (mouseInPane && ctl::mouseIn(rr.p0(), rr.p1()));
    const float rowHovT = motion::hoverFactor(ImGui::GetID("row_hov"), anyHover && !downloading);
    if (current) {
        const ImU32 fill = (rowHovT > 0.01f) ? motion::mix(t.container, t.high, rowHovT) : t.container;
        dl->AddRectFilled(rr.p0(), rr.p1(), fill, geist::radius::MD);
    } else if (installed && rowHovT > 0.01f) {
        dl->AddRectFilled(rr.p0(), rr.p1(), ctl::detail::withAlpha(t.container, rowHovT), geist::radius::MD);
    } else if (!installed && rowHovT > 0.01f) {
        ctl::hoverWashAlpha(dl, rr.p0(), rr.p1(), geist::radius::MD, rowHovT, 0.03f);
    }
    if (rowFocused) {
        ctl::detail::focusRing(dl, rr.p0(), rr.p1(), geist::radius::MD, true);
    }
    if (rowClicked) {
        app.setModel(m.name);
    }

    const flex::Rect nameR = fx.rect(nameSlot);
    ellipsisTextCentered(dl, sansMed, geist::type::BODY, nameR.x, nameR.y, nameR.bottom(), nameR.w, t.ink, displayName);
    if (nameShortened) {
        const bool nameHov = mouseInPane && ctl::mouseIn(nameR.p0(), nameR.p1());
        ctl::tooltip(dl, ImGui::GetID("name_tip"), nameR.p0(), nameR.p1(),
                     m.name.c_str(), sans, nameHov, rowFocused);
    }

    if (fx.isVisible(curSlot)) {
        ctl::metaChip(dl, sansMed, fx.p0(curSlot), curLabel, t.accent, t.accent, t.accentSoft,
                      ctl::geom::META_CHIP_H, 5.0f);
    }
    if (fx.isVisible(recSlot)) {
        ctl::recChip(dl, sansMed, fx.p0(recSlot), recLabel, ctl::geom::META_CHIP_H, 5.0f);
    }
    if (fx.isVisible(scaleSlot)) {
        ctl::metaChip(dl, sansMed, fx.p0(scaleSlot), scaleLabel.c_str(), 0, t.ink, t.tagBg,
                      ctl::geom::META_CHIP_H, 5.0f);
    }
    if (fx.isVisible(sceneSlot)) {
        ctl::metaChip(dl, sans, fx.p0(sceneSlot), scLbl, 0,
                      t.body, t.tagBg, ctl::geom::META_CHIP_H, 5.0f);
    }
    if (fx.isVisible(sizeTextSlot)) {
        const flex::Rect sizeR = fx.rect(sizeTextSlot);
        drawTextCenteredY(dl, mono, geist::type::MONO, sizeR.x,
                          sizeR.y, sizeR.bottom(), t.mute, sizeStr.c_str());
    }

    // Right action column & bottom download progress bar
    if (downloading) {
        if (fx.isVisible(busySlot)) {
            ctl::busyArc(dl, snapPt(fx.rect(busySlot).center()), 3.5f, t.accent, 1.5f);
        }
        if (fx.isVisible(pctSlot)) {
            const flex::Rect pctR = fx.rect(pctSlot);
            drawTextCenteredY(dl, mono, geist::type::MONO, pctR.x, pctR.y, pctR.bottom(),
                              t.accent, pctText.c_str());
        }
        if (fx.isVisible(dlBarSlot)) {
            const flex::Rect barR = fx.rect(dlBarSlot);
            const float frac = snap.download_pct < 0 ? 0.0f : std::clamp(snap.download_pct / 100.0f, 0.0f, 1.0f);
            ctl::progressBar(dl, ImGui::GetID("dlbar"), barR.p0(), barR.w, frac, barR.h);
        }
    } else if (installed) {
        const flex::Rect btnR = fx.rect(btnSlot);
        if (ctl::confirmButton(dl, ImGui::GetID("del"), nullptr,
                               upix::i18n::tr(upix::i18n::Key::CommonConfirmQ), sansMed, btnR.p0(), btnR.p1(),
                               !current, "trash",
                               current ? nullptr : upix::i18n::tr(upix::i18n::Key::ModelsDelete), 14.0f)) {
            app.removeModel(m.name);
        }
        if (current) {
            const bool delHov = mouseInPane && ctl::mouseIn(btnR.p0(), btnR.p1());
            ctl::tooltip(dl, ImGui::GetID("del_cur_tip"), btnR.p0(), btnR.p1(),
                         upix::i18n::tr(upix::i18n::Key::ModelsDeleteCurrentTip), sans, delHov, false);
        }
    } else {
        const bool busyDownloading = !snap.downloading.empty();
        const flex::Rect btnR = fx.rect(btnSlot);
        if (accentIconButton(dl, ImGui::GetID("dl"), btnR.p0(), kActionIconBox, "download",
                             upix::i18n::tr(busyDownloading ? upix::i18n::Key::ModelsBusyDownloadingTip
                                                            : upix::i18n::Key::ModelsDownload),
                             sans, !busyDownloading)) {
            app.downloadModel(m.name);
        }
    }

    ImGui::PopID();
    ImGui::PopID();
}

} // namespace upix::views::models_detail
