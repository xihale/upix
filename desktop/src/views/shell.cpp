#include "views/common.hpp"
#include "i18n.hpp"
#include "ui/flex.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace upix::views {

void renderTopBar(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float barW = std::max(0.0f, winSize.x);
    const float barH = kTopbarH;
    const float padX = geist::space::XL; // 20px
    const float gap = geist::space::XL;  // 20px

    dl->AddRectFilled(ImVec2(0.0f, 0.0f), snapPt(barW, barH), t.panelBg);
    ctl::hHairline(dl, 0.0f, barW, barH - hairlinePx(), t.outline);

    dl->PushClipRect(ImVec2(0.0f, 0.0f), snapPt(barW, barH), true);

    ImFont* sansFont = ui.fontSans;
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : ui.fontSans;
    const bool modalOpen = ui.showModelPicker || !snap.host.pending_device.empty();
    const bool interactive = !modalOpen;

    constexpr float kPillPadX = 10.0f;
    constexpr float kDotDia = 6.0f;
    constexpr float kDotGap = 6.0f;
    constexpr float kBrandSize = 18.0f;
    constexpr float kBrandGap = geist::space::SM; // 8px
    constexpr float kWordPx = std::max(18.0f, geist::type::BODY);
    constexpr float kSepH = 18.0f;

    const char* backendLabel = snap.gpu ? "Vulkan" : "CPU";
    const ImVec2 backendSz = calcText(monoFont, geist::type::MONO, backendLabel);
    const float kCtrlH = std::max(34.0f, std::ceil(backendSz.y) + 12.0f);
    const float pillW = kPillPadX * 2.0f + kDotDia + kDotGap + std::ceil(backendSz.x);
    const ImVec2 wordSz = calcText(mediumFont, kWordPx, "Upix");
    const float wordW = std::ceil(wordSz.x);
    const float brandW = kBrandSize + kBrandGap + wordW;
    constexpr float kWinBtnGap = 4.0f;
    const float winCtrlsW = ui.showWindowControls
        ? (geist::space::SM + hairlinePx() + geist::space::SM + kCtrlH * 3.0f + kWinBtnGap * 2.0f)
        : 0.0f;
    const float rightW = pillW + geist::space::SM + kCtrlH + winCtrlsW;

    const char* kTabLabels[4] = {
        upix::i18n::tr(upix::i18n::Key::TabWorkbench),
        upix::i18n::tr(upix::i18n::Key::TabModels),
        upix::i18n::tr(upix::i18n::Key::TabHost),
        upix::i18n::tr(upix::i18n::Key::TabSettings),
    };
    const char* kTabTips[4] = {
        upix::i18n::tr(upix::i18n::Key::TabTipWorkbench),
        upix::i18n::tr(upix::i18n::Key::TabTipModels),
        upix::i18n::tr(upix::i18n::Key::TabTipHost),
        upix::i18n::tr(upix::i18n::Key::TabTipSettings),
    };
    float tabWs[4] = {};
    float tabsTextW = 0.0f;
    for (int i = 0; i < 4; ++i) {
        tabWs[i] = std::ceil(calcText(mediumFont, geist::type::BODY, kTabLabels[i]).x) + 8.0f;
        tabsTextW += tabWs[i];
    }
    const float tabsAvailW = std::max(
        0.0f, barW - padX * 2.0f - brandW - hairlinePx() - gap * 2.0f - rightW - geist::space::LG);
    const float tabsNeedW = tabsTextW + gap * 3.0f;
    const float useGap = (tabsNeedW > tabsAvailW)
                             ? std::max(4.0f, (tabsAvailW - tabsTextW) / 3.0f)
                             : gap;

    static flex::Context sCtx;
    flex::Layout topFx(sCtx);
    auto topRow = topFx.row({.padding = flex::Edges::xy(padX, 0.0f),
                             .gap = gap,
                             .align = flex::Align::Center});

    auto brandRow = topRow.row({.gap = kBrandGap,
                                .align = flex::Align::Center,
                                .width = flex::Auto().shrink(0.0f),
                                .height = flex::Fixed(kCtrlH)});
    const flex::Slot markSlot = brandRow.slot(flex::Fixed(kBrandSize), flex::Fixed(kBrandSize));
    const flex::Slot wordSlot = brandRow.slot(flex::Fixed(wordW), flex::Fixed(kCtrlH));

    const flex::Slot sepSlot = topRow.slot(flex::Fixed(hairlinePx()), flex::Fixed(kSepH));

    auto tabsRow = topRow.row({.gap = useGap,
                               .align = flex::Align::Stretch,
                               .width = flex::Shrink(std::min(tabsAvailW, tabsTextW + useGap * 3.0f), 1.0f),
                               .height = flex::Fixed(barH)});
    flex::Slot tabSlots[4] = {};
    for (int i = 0; i < 4; ++i) {
        tabSlots[i] = tabsRow.slot(flex::Fixed(tabWs[i]), flex::Fixed(barH));
    }

    topRow.spacer(1.0f);

    auto rightRow = topRow.row({.gap = geist::space::SM,
                                .align = flex::Align::Center,
                                .width = flex::Auto().shrink(0.0f)});
    auto pillRow = rightRow.row({.padding = flex::Edges::xy(kPillPadX, 0.0f),
                                 .gap = kDotGap,
                                 .align = flex::Align::Center,
                                 .height = flex::Fixed(kCtrlH)});
    const flex::Slot dotSlot = pillRow.slot(flex::Fixed(kDotDia), flex::Fixed(kDotDia));
    const flex::Slot backendLblSlot = pillRow.slot(flex::Fixed(std::ceil(backendSz.x)), flex::Fixed(kCtrlH));
    const flex::Slot themeSlot = rightRow.slot(flex::Fixed(kCtrlH), flex::Fixed(kCtrlH));
    flex::Slot winSepSlot{};
    flex::Slot minSlot{};
    flex::Slot maxSlot{};
    flex::Slot closeSlot{};
    if (ui.showWindowControls) {
        winSepSlot = rightRow.slot(flex::Fixed(hairlinePx()), flex::Fixed(kSepH));
        auto winBtnsRow = rightRow.row({.gap = kWinBtnGap,
                                        .align = flex::Align::Center,
                                        .width = flex::Auto().shrink(0.0f)});
        minSlot = winBtnsRow.slot(flex::Fixed(kCtrlH), flex::Fixed(kCtrlH));
        maxSlot = winBtnsRow.slot(flex::Fixed(kCtrlH), flex::Fixed(kCtrlH));
        closeSlot = winBtnsRow.slot(flex::Fixed(kCtrlH), flex::Fixed(kCtrlH));
    }

    topFx.solve(flex::Rect{0.0f, 0.0f, barW, barH});

    {
        const flex::Rect brandHitR = topFx.rect(brandRow);
        const flex::Rect tabsHitR = topFx.rect(tabsRow);
        const flex::Rect rightHitR = topFx.rect(rightRow);
        ui.topbarBrandRect = {brandHitR.x, brandHitR.y, brandHitR.right(), brandHitR.bottom()};
        ui.topbarTabsRect = {tabsHitR.x, tabsHitR.y, tabsHitR.right(), tabsHitR.bottom()};
        ui.topbarRightRect = {rightHitR.x, rightHitR.y, rightHitR.right(), rightHitR.bottom()};
        ui.modalBlockingHitTest = modalOpen || ui.showIpPicker;
    }

    // Brand logo + title
    {
        const flex::Rect brandR = topFx.rect(brandRow);
        const flex::Rect markR = topFx.rect(markSlot);
        const flex::Rect wordR = topFx.rect(wordSlot);
        const ImVec2 b0 = snapPt(brandR.x, brandR.y);
        const ImVec2 b1 = snapPt(brandR.right(), brandR.bottom());
        const ImGuiID brandId = ImGui::GetID("topbar.brand");
        const ctl::detail::Hit hit = ctl::detail::hit(brandId, b0, b1, interactive);
        const bool focused = interactive && hit.focused && ImGui::GetIO().NavVisible;
        if (interactive && hit.clicked) {
            ui.activeTab = 0;
        }
        drawBrandMark(dl, snapPt(markR.x, markR.y), kBrandSize);
        drawTextCenteredY(dl, mediumFont, kWordPx, wordR.x, wordR.y, wordR.bottom(), t.ink, "Upix");
        ctl::detail::focusRing(dl, b0, b1, geist::radius::MD, focused);
        ctl::tooltip(dl, ImGui::GetID("topbar.brand.tip"), b0, b1, "Upix", sansFont,
                     interactive && hit.hovered, focused);
    }

    // Vertical separator: 1px x 16px centered vertically
    {
        const flex::Rect sepR = topFx.rect(sepSlot);
        ctl::vHairline(dl, snapPx(sepR.x), snapPx(sepR.y), snapPx(sepR.bottom()), t.outline);
    }

    // Navigation underline tabs
    {
        const flex::Rect tabsR = topFx.rect(tabsRow);
        if (tabsR.w > 1.0f) {
            const ImGuiID tabsId = ImGui::GetID("topbar.tabs");
            const float lineT = hairlinePx();
            ctl::hHairline(dl, tabsR.x, tabsR.right(), barH - lineT, t.outline);

            const bool noModal = interactive && !ui.showIpPicker;
            for (int i = 0; i < 4; ++i) {
                const flex::Rect tr = topFx.rect(tabSlots[i]);
                const ImVec2 a = snapPt(tr.x, tr.y);
                const ImVec2 b = snapPt(std::min(tabsR.right(), tr.right()), tr.bottom());
                const ctl::detail::Hit h = ctl::detail::hit(ctl::detail::salt(tabsId, 40 + i), a, b, interactive);
                if (interactive && h.clicked && ui.activeTab != i) {
                    ui.activeTab = i;
                }
                const bool on = (i == ui.activeTab);
                const float hovT = motion::hoverFactor(ctl::detail::salt(tabsId, 80 + i),
                                                       interactive && !on && h.hovered);
                if (!on && (hovT > 0.01f || (interactive && h.held))) {
                    const float pillPadY = 9.0f;
                    const ImVec2 pillA = snapPt(std::max(tabsR.x, tr.x - 2.0f), tr.y + pillPadY);
                    const ImVec2 pillB = snapPt(std::min(tabsR.right(), tr.right() + 2.0f), tr.bottom() - pillPadY);
                    const float cov = (interactive && h.held) ? ctl::geom::HOVER_WASH * 1.4f : ctl::geom::HOVER_WASH;
                    const float fac = (interactive && h.held) ? 1.0f : hovT;
                    ctl::hoverWashAlpha(dl, pillA, pillB, geist::radius::MD, fac, cov);
                }
                const ImU32 ink = on ? t.ink : motion::mix(t.mute, t.ink, hovT);
                ImFont* tabFont = on ? mediumFont : sansFont;
                drawTextCentered(dl, tabFont, geist::type::BODY, a, b, ink, kTabLabels[i]);
                ctl::detail::focusRing(dl, a, b, geist::radius::SM, interactive && h.focused);
                ctl::tooltip(dl, ImGui::GetID(kTabTips[i]), a, b, kTabTips[i], sansFont,
                             noModal && h.hovered, interactive && h.focused);
            }

            const int activeIdx = std::clamp(ui.activeTab, 0, 3);
            const flex::Rect activeR = topFx.rect(tabSlots[activeIdx]);
            const float barX = ctl::selectionSpring(ctl::detail::salt(tabsId, 10), activeR.x);
            const float barW2 = ctl::selectionSpring(ctl::detail::salt(tabsId, 11), activeR.w);
            const float barH2 = std::max(lineT, snapPx(2.0f));
            dl->AddRectFilled(snapPt(barX, barH - barH2), snapPt(barX + barW2, barH), t.ink);
        }
    }

    // Backend status pill
    {
        const flex::Rect pillR = topFx.rect(pillRow);
        const flex::Rect dotR = topFx.rect(dotSlot);
        const flex::Rect lblR = topFx.rect(backendLblSlot);
        const ImVec2 p0 = snapPt(pillR.x, pillR.y);
        const ImVec2 p1 = snapPt(pillR.right(), pillR.bottom());
        const ImGuiID pillId = ImGui::GetID("topbar.backend");
        const ctl::detail::Hit hit = ctl::detail::hit(pillId, p0, p1, interactive);
        const float hovT = motion::hoverFactor(ctl::detail::salt(pillId, 1), interactive && hit.hovered);
        if (interactive && hit.held) {
            dl->AddRectFilled(p0, p1, t.highest, geist::radius::MD);
            dl->AddRect(p0, p1, t.outlineStrong, geist::radius::MD, 0, hairlinePx());
        } else if (hovT > 0.01f) {
            dl->AddRectFilled(p0, p1, ctl::detail::withAlpha(t.high, hovT), geist::radius::MD);
            dl->AddRect(p0, p1, ctl::detail::withAlpha(t.outlineStrong, hovT * 0.75f),
                        geist::radius::MD, 0, hairlinePx());
        }
        const ImVec2 dotC = snapPt(dotR.center());
        const ImU32 dotCol = snap.gpu ? t.accent : motion::mix(t.mute, t.ink, hovT);
        dl->AddCircleFilled(dotC, kDotDia * 0.5f, dotCol, 16);
        drawTextCenteredY(dl, monoFont, geist::type::MONO, lblR.x, lblR.y, lblR.bottom(),
                          motion::mix(t.body, t.ink, hovT), backendLabel);
        const bool focused = interactive && hit.focused && ImGui::GetIO().NavVisible;
        ctl::detail::focusRing(dl, p0, p1, geist::radius::MD, focused);
        const char* tip = upix::i18n::tr(
            snap.gpu ? upix::i18n::Key::BackendTipGpu : upix::i18n::Key::BackendTipCpu);
        ctl::tooltip(dl, ImGui::GetID("topbar.backend.tip"), p0, p1, tip, sansFont,
                     interactive && hit.hovered, focused);
        if (interactive && hit.clicked) {
            ui.activeTab = 3;
        }
    }

    // Theme toggle icon button: moon in Dark mode, sun in Light mode
    {
        const flex::Rect themeR = topFx.rect(themeSlot);
        const bool isDark = (geist::mode() == geist::Mode::Dark);
        const char* themeIcon = isDark ? "moon" : "sun";
        if (ctl::iconAction(dl, ImGui::GetID("topbar.theme"), snapPt(themeR.x, themeR.y), kCtrlH,
                            themeIcon, upix::i18n::tr(upix::i18n::Key::ThemeToggleTip), sansFont, 16.0f,
                            geist::radius::MD, interactive, false)) {
            ui.requestDarkMode = isDark ? 0 : 1;
        }
    }

    // Custom borderless window controls (minimize / maximize-restore / close)
    if (ui.showWindowControls) {
        const flex::Rect wSepR = topFx.rect(winSepSlot);
        ctl::vHairline(dl, snapPx(wSepR.x), snapPx(wSepR.y), snapPx(wSepR.bottom()), t.outline);

        const flex::Rect minR = topFx.rect(minSlot);
        if (ctl::iconAction(dl, ImGui::GetID("topbar.win_min"), snapPt(minR.x, minR.y), kCtrlH,
                            "win-minimize", upix::i18n::tr(upix::i18n::Key::WinMinimize), sansFont, 14.0f,
                            geist::radius::MD, interactive, false)) {
            ui.requestWindowAction = 1;
        }

        const flex::Rect maxR = topFx.rect(maxSlot);
        const char* maxIcon = ui.windowMaximized ? "win-restore" : "win-maximize";
        const char* maxTip = upix::i18n::tr(
            ui.windowMaximized ? upix::i18n::Key::WinRestore : upix::i18n::Key::WinMaximize);
        if (ctl::iconAction(dl, ImGui::GetID("topbar.win_max"), snapPt(maxR.x, maxR.y), kCtrlH,
                            maxIcon, maxTip, sansFont, 14.0f,
                            geist::radius::MD, interactive, false)) {
            ui.requestWindowAction = 2;
        }

        const flex::Rect closeR = topFx.rect(closeSlot);
        if (ctl::iconAction(dl, ImGui::GetID("topbar.win_close"), snapPt(closeR.x, closeR.y), kCtrlH,
                            "x", upix::i18n::tr(upix::i18n::Key::WinClose), sansFont, 14.0f,
                            geist::radius::MD, interactive, false, true)) {
            ui.requestWindowAction = 3;
        }
    }

    dl->PopClipRect();
}

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    renderTopBar(dl, winSize, ui, snap);
}

} // namespace upix::views
