// views/workbench/filmstrip_dock.cpp — Bottom image queue dock.
#include "views/workbench/internal.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace upix::views::detail {
inline namespace wb {

void renderFilmstripDock(ImDrawList* dl, const upix::ui::flex::Rect& dockRect, UiViewState& ui,
                         upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                         SDL_Window* window, DialogBridge* dlgFiles, bool isRunning, bool modal,
                         FilmstripState& state) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    dl->AddRectFilled(dockRect.p0(), dockRect.p1(), t.panelBg);
    ctl::hHairline(dl, dockRect.x, dockRect.right(), dockRect.y, t.outline);

    size_t doneCnt = 0;
    for (const auto& it : snap.inputs) {
        if (it.status_kind == 2) ++doneCnt;
    }
    char cntBuf[32] = {};
    const char* cntText = formatInto(cntBuf, "{}/{}", doneCnt, snap.inputs.size());
    const ImVec2 cntSz = calcText(monoFont, geist::type::MONO, cntText);

    using upix::i18n::Key;
    using upix::i18n::tr;
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiID clearId = ImGui::GetID("##dock_clear");
    const bool clearArmed = ctl::isConfirmArmed(clearId);
    const char* clearLabel = tr(clearArmed ? Key::CommonConfirmClearQ : Key::CommonClear);
    const float clearFs = clearArmed ? geist::type::BODY : geist::type::BODY_SMALL;
    const float clearW = std::max(52.0f, std::ceil(calcText(sansFont, clearFs, clearLabel).x) + 20.0f);
    const float actBtnH = std::max(32.0f, std::ceil(calcText(sansFont, geist::type::BODY, tr(Key::CommonConfirmQ)).y) + 10.0f);

    constexpr float kCardW = 76.0f;
    constexpr float kCardH = 54.0f;
    constexpr float kCardGap = 8.0f;
    constexpr float kAddTileW = 48.0f;

    // 1. Measure intrinsic filmstrip width via stripFx first (hysteresis-free overflow check).
    static flex::Context stripCtx;
    flex::Layout stripFx(stripCtx);
    auto stripRow = stripFx.row({
        .gap = kCardGap,
        .align = flex::Align::Center,
    });
    static std::vector<flex::Slot> cardSlots;
    cardSlots.clear();
    cardSlots.reserve(snap.inputs.size());
    for (size_t i = 0; i < snap.inputs.size(); ++i) {
        cardSlots.push_back(stripRow.slot(flex::Fixed(kCardW), flex::Fixed(kCardH)));
    }
    const flex::Slot addTileSlot = stripRow.slot(flex::Fixed(kAddTileW), flex::Fixed(kCardH));
    const float stripContentW = stripFx.solveAuto(ImVec2(0.0f, 0.0f), false).w;

    // 2. Solve outer dock row (first check strip width without persistent '+' to prevent frame oscillation).
    static flex::Context dockBarCtx;
    auto solveDockBar = [&](bool withPersistentAdd, flex::Rect& outStripR, flex::Rect& outAddR,
                            bool& outAddVis, flex::Rect& outCntR, flex::Rect& outClearR, bool snapDpi) {
        flex::Layout fx(dockBarCtx);
        auto row = fx.row({
            .padding = flex::Edges::xy(geist::space::LG, 8.0f),
            .gap = geist::space::MD,
            .align = flex::Align::Center,
        });
        const flex::Slot stripSlot = row.slot({
            .width = flex::Grow(1.0f),
            .height = flex::Fraction(1.0f),
            .margin = flex::Edges::only(0.0f, 0.0f, 12.0f, 0.0f),
        });
        auto acts = row.row({
            .gap = geist::space::SM,
            .align = flex::Align::Center,
        });
        const flex::Slot addActSlot = acts.slot({
            .width = flex::Fixed(actBtnH),
            .height = flex::Fixed(actBtnH),
            .visible = withPersistentAdd,
        });
        const flex::Slot cntSlot = acts.slot(flex::Fixed(textBasis(cntSz.x)), flex::Fixed(cntSz.y));
        const flex::Slot clearSlot = acts.slot(flex::Fixed(clearW), flex::Fixed(actBtnH));

        fx.solve(dockRect, snapDpi);
        outStripR = fx.rect(stripSlot);
        outAddR = fx.rect(addActSlot);
        outAddVis = fx.isVisible(addActSlot);
        outCntR = fx.rect(cntSlot);
        outClearR = fx.rect(clearSlot);
    };

    flex::Rect stripR, addActR, cntR, clearR;
    bool addActVis = false;
    solveDockBar(false, stripR, addActR, addActVis, cntR, clearR, false);
    const bool showPersistentAdd = (stripContentW > stripR.w);
    solveDockBar(showPersistentAdd, stripR, addActR, addActVis, cntR, clearR, true);

    if (addActVis) {
        if (ctl::iconAction(dl, ImGui::GetID("##dock_add_persistent"), addActR.p0(), addActR.w, "plus",
                            tr(Key::DockAddImage), sansFont, 15.0f, geist::radius::MD, !isRunning, false)) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    }
    drawTextCenteredY(dl, monoFont, geist::type::MONO, cntR.x, cntR.y, cntR.bottom(), t.mute, cntText);
    if (ctl::confirmButton(dl, clearId, tr(Key::CommonClear), tr(Key::CommonConfirmClearQ), sansFont,
                           clearR.p0(), clearR.p1(), !isRunning,
                           nullptr, nullptr, 15.0f, 3.0f, true, false)) {
        app.clearInputs();
    }

    const float maxDockScroll = std::max(0.0f, stripContentW - stripR.w);
    if (!modal && ctl::mouseIn(stripR.p0(), stripR.p1()) && std::fabs(io.MouseWheel) > 0.001f && maxDockScroll > 0.0f) {
        state.scrollTarget -= io.MouseWheel * 48.0f;
    }
    state.scrollTarget = std::clamp(state.scrollTarget, 0.0f, maxDockScroll);
    state.scroll = std::clamp(motion::smoothScroll(ImGui::GetID("wb.dock_scroll"), state.scrollTarget), 0.0f, maxDockScroll);

    stripFx.solve(flex::Rect{stripR.x - state.scroll, stripR.y, flex::kUnbounded, stripR.h});

    dl->PushClipRect(stripR.p0(), stripR.p1(), true);
    const bool dark = (geist::mode() == geist::Mode::Dark);
    static flex::Context cardCtx;
    for (size_t i = 0; i < snap.inputs.size(); ++i) {
        const auto& item = snap.inputs[i];
        const flex::Rect cardR = stripFx.rect(cardSlots[i]);
        const ImVec2 c0 = snapPt(cardR.p0());
        const ImVec2 c1 = snapPt(cardR.p1());
        if (c1.x < stripR.x || c0.x > stripR.right()) continue;

        const bool isActive = (static_cast<int>(i) == snap.active_input);

        flex::Layout cardFx(cardCtx);
        auto card = cardFx.row({
            .padding = flex::Edges::only(6.0f, 6.0f, 4.0f, 6.0f),
            .gap = 4.0f,
            .align = flex::Align::Center,
            .width = flex::Fixed(kCardW),
            .height = flex::Fixed(kCardH),
        });
        const flex::Slot thumbSlot = card.slot(flex::Fixed(42.0f), flex::Fixed(42.0f));
        auto actionCol = card.col({
            .justify = flex::Justify::SpaceBetween,
            .align = flex::Align::Center,
            .width = flex::Fixed(20.0f),
            .height = flex::Fixed(42.0f),
        });
        const flex::Slot rmSlot = actionCol.slot(flex::Fixed(20.0f), flex::Fixed(20.0f));
        const flex::Slot dotSlot = actionCol.slot(flex::Fixed(12.0f), flex::Fixed(14.0f));
        cardFx.solve(cardR);

        const flex::Rect thumbR = cardFx.rect(thumbSlot);
        const flex::Rect rmR = cardFx.rect(rmSlot);
        const flex::Rect dotR = cardFx.rect(dotSlot);

        ImU32 dotCol = t.mute;
        char pctTextBuf[16] = {};
        const char* stStr = tr(Key::DockQueued);
        if (item.status_kind == 1) {
            dotCol = t.accent;
            stStr = formatInto(pctTextBuf, "{}%", snap.job.percent);
        } else if (item.status_kind == 2) {
            dotCol = t.accent;
            stStr = tr(Key::DockDone);
        } else if (item.status_kind == 3) {
            dotCol = t.error;
            stStr = item.status_text.empty() ? tr(Key::DockFailed) : item.status_text.c_str();
        }
        char tipBuf[256] = {};
        const char* tipText = item.name.empty()
            ? stStr
            : formatInto(tipBuf, "{} · {}", item.name, stStr);

        const bool inStripClip = ctl::mouseIn(stripR.p0(), stripR.p1());
        const bool overRm = !modal && !isRunning && inStripClip && ctl::mouseIn(rmR.p0(), rmR.p1());

        bool hov = false;
        bool clicked = false;
        bool focused = false;
        bool rmClicked = false;
        ImGuiID cardHovId = 0;
        if (!modal) {
            ImGui::PushID(static_cast<int>(i));
            cardHovId = ImGui::GetID("##film_card_hov");
            ImGui::SetCursorScreenPos(c0);
            ImGui::SetNextItemAllowOverlap();
            clicked = ImGui::InvisibleButton("##film_card", cardR.size());
            hov = inStripClip && ImGui::IsItemHovered() && !overRm;
            focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ctl::tooltip(dl, ImGui::GetID("##film_tip"), c0, c1, tipText,
                         sansFont, hov, false);
            ImGui::PopID();
        } else {
            cardHovId = ctl::detail::salt(ImGui::GetID("##film_card_hov_m"), static_cast<int>(i));
        }

        const float cardHovT = motion::hoverFactor(cardHovId, hov);
        const ImU32 cardRestBg = (isActive && dark) ? t.container : t.surface;
        const ImU32 cardHovBg = dark ? t.high : t.container;
        dl->AddRectFilled(c0, c1, motion::mix(cardRestBg, cardHovBg, cardHovT), geist::radius::MD);

        drawMiniThumb(dl, snapPt(thumbR.p0()), snapPt(thumbR.p1()), ui, geist::radius::SM, i, item.path);

        const ImVec2 dotC = snapPt(dotR.center());
        if (item.status_kind == 1) {
            dl->AddCircleFilled(dotC, 6.0f, t.accentSoft, 16);
        }
        dl->AddCircleFilled(dotC, 3.5f, dotCol, 16);

        const ImU32 cardBorder = isActive ? t.ink : motion::mix(t.outline, t.outlineStrong, cardHovT);
        dl->AddRect(c0, c1, cardBorder, geist::radius::MD, 0, hairlinePx());
        ctl::detail::focusRing(dl, c0, c1, geist::radius::MD, focused);

        if (!modal) {
            ImGui::PushID(static_cast<int>(i));
            const ImVec2 savedPos = ImGui::GetIO().MousePos;
            if (!inStripClip) ImGui::GetIO().MousePos = ImVec2(-1.0e6f, -1.0e6f);
            rmClicked = ctl::iconAction(dl, ImGui::GetID("##film_rm"), rmR.p0(), rmR.w, "x",
                                        tr(Key::DockRemoveImage), sansFont, 12.5f, geist::radius::SM, !isRunning, false);
            if (!inStripClip) ImGui::GetIO().MousePos = savedPos;
            ImGui::PopID();
        }

        if (rmClicked) {
            app.removeInput(item.path);
        } else if (clicked && !isActive) {
            app.setActiveInput(static_cast<int>(i));
        }
    }

    // Inline dashed .q-add tile
    {
        const flex::Rect addTileR = stripFx.rect(addTileSlot);
        const ImVec2 a0 = snapPt(addTileR.p0());
        const ImVec2 a1 = snapPt(addTileR.p1());
        if (a1.x >= stripR.x && a0.x <= stripR.right()) {
            const bool inStripClip = ctl::mouseIn(stripR.p0(), stripR.p1());
            bool addHov = false;
            bool addHeld = false;
            bool addClicked = false;
            bool addFocused = false;
            if (!modal) {
                ImGui::SetCursorScreenPos(a0);
                addClicked = ImGui::InvisibleButton("##dock_add_tile", addTileR.size()) && inStripClip;
                addHov = !isRunning && inStripClip && ImGui::IsItemHovered();
                addHeld = !isRunning && inStripClip && ImGui::IsItemActive();
                addFocused = !isRunning && ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
                if (addHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (!isRunning) {
                    ctl::tooltip(dl, ImGui::GetID("##dock_add_tip"), a0, a1, tr(Key::DockAddImage), sansFont, addHov, false);
                }
            }
            const float hovT = motion::hoverFactor(ImGui::GetID("##dock_add_hov"), addHov);
            const float press = motion::pressScale(ImGui::GetID("##dock_add_press"), addHeld, motion::PRESS_SCALE);
            ImVec2 qa0 = a0, qa1 = a1;
            ctl::detail::scaled(a0, a1, press, qa0, qa1);
            const float liftT = addHeld ? 1.0f : hovT;
            if (!isRunning && liftT > 0.01f) {
                dl->AddRectFilled(qa0, qa1, withAlpha(dark ? t.high : t.container, liftT), geist::radius::MD);
            }
            const ImU32 dashCol = isRunning ? t.outline : motion::mix(t.outlineStrong, t.ink, liftT * 0.65f);
            const ImU32 iconCol = isRunning ? t.faint : motion::mix(t.mute, t.ink, liftT);
            drawDashedRoundedRect(dl, qa0, qa1, geist::radius::MD, dashCol, hairlinePx(), 5.0f, 3.0f);
            drawIcon(dl, "plus", ImVec2((qa0.x + qa1.x) * 0.5f, (qa0.y + qa1.y) * 0.5f), 16.0f, iconCol);
            ctl::detail::focusRing(dl, a0, a1, geist::radius::MD, addFocused);
            if (addClicked && !isRunning) {
                SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
            }
        }
    }

    // Subtle 14px horizontal fade gradients into t.panelBg when filmstrip overflows
    {
        const ImU32 fadeOpaque = t.panelBg;
        const ImU32 fadeClear = withAlpha(t.panelBg, 0.0f);
        const float fadeW = std::min(14.0f, stripR.w * 0.25f);
        if (fadeW > 1.0f) {
            if (state.scroll > 1.0f) {
                dl->AddRectFilledMultiColor(
                    snapPt(stripR.x, stripR.y),
                    snapPt(stripR.x + fadeW, stripR.bottom()),
                    fadeOpaque, fadeClear, fadeClear, fadeOpaque);
            }
            if (state.scroll < maxDockScroll - 1.0f) {
                dl->AddRectFilledMultiColor(
                    snapPt(stripR.right() - fadeW, stripR.y),
                    snapPt(stripR.right(), stripR.bottom()),
                    fadeClear, fadeOpaque, fadeOpaque, fadeClear);
            }
        }
    }

    dl->PopClipRect();
}

} // namespace wb
} // namespace upix::views::detail
