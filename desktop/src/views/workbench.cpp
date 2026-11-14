// views/workbench.cpp — Main Workbench shell & interactive compare stage viewport.
#include "views/workbench/internal.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace upix::views {
namespace {

void renderStageCanvas(ImDrawList* dl, const upix::ui::flex::Rect& stageWellR,
                       UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                       const upix::gui::InputItem* activeItem,
                       bool isRunning, bool isCompare, bool isError, bool modal,
                       bool& stagePixelated) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const ImGuiIO& io = ImGui::GetIO();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    const ImVec2 viewMin = snapPt(stageWellR.p0());
    const ImVec2 viewMax = snapPt(stageWellR.p1());
    const float viewW = std::max(1.0f, viewMax.x - viewMin.x);
    const float viewH = std::max(1.0f, viewMax.y - viewMin.y);

    float refW = viewW;
    float refH = viewH;
    if (isCompare && snap.job.out_w > 0 && snap.job.out_h > 0) {
        refW = static_cast<float>(snap.job.out_w);
        refH = static_cast<float>(snap.job.out_h);
    } else if (ui.beforeTex.valid() && ui.beforeTex.width > 0 && ui.beforeTex.height > 0) {
        refW = static_cast<float>(ui.beforeTex.width);
        refH = static_cast<float>(ui.beforeTex.height);
    } else if (activeItem && activeItem->width > 0 && activeItem->height > 0) {
        refW = static_cast<float>(activeItem->width);
        refH = static_cast<float>(activeItem->height);
    }
    constexpr float kFitPad = 24.0f;
    const float fitW = std::max(1.0f, viewW - kFitPad * 2.0f);
    const float fitH = std::max(1.0f, viewH - kFitPad * 2.0f);
    const float fitScale = std::min(fitW / std::max(1.0f, refW), fitH / std::max(1.0f, refH));

    const bool mouseInView = ctl::mouseIn(viewMin, viewMax);
    if (!modal && mouseInView && std::fabs(io.MouseWheel) > 0.001f) {
        const float oldZoom = ui.fitMode ? 1.0f : ui.zoom;
        const float oldEff = fitScale * oldZoom;
        const float nextZoom = std::clamp(oldZoom * std::pow(1.15f, io.MouseWheel), 0.1f, 32.0f);
        const float nextEff = fitScale * nextZoom;
        const ImVec2 viewCenter(viewMin.x + viewW * 0.5f, viewMin.y + viewH * 0.5f);
        const ImVec2 rel(io.MousePos.x - (viewCenter.x + ui.pan.x),
                         io.MousePos.y - (viewCenter.y + ui.pan.y));
        const float ratio = nextEff / std::max(1.0e-5f, oldEff);
        ui.pan.x += rel.x * (1.0f - ratio);
        ui.pan.y += rel.y * (1.0f - ratio);
        ui.fitMode = false;
        ui.zoom = nextZoom;
    }

    const bool spacePan = ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left);
    const bool midPan = ImGui::IsMouseDragging(ImGuiMouseButton_Middle);
    if (!modal && mouseInView && (spacePan || midPan)) {
        if (ui.fitMode) {
            ui.fitMode = false;
            ui.zoom = 1.0f;
        }
        ui.pan.x += io.MouseDelta.x;
        ui.pan.y += io.MouseDelta.y;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    const float effZoom = ui.fitMode ? 1.0f : std::clamp(ui.zoom, 0.1f, 32.0f);
    const float effScale = fitScale * effZoom;
    const float imgW = std::max(1.0f, refW * effScale);
    const float imgH = std::max(1.0f, refH * effScale);
    const float maxPanX = std::max(0.0f, (imgW + viewW) * 0.5f - 48.0f);
    const float maxPanY = std::max(0.0f, (imgH + viewH) * 0.5f - 48.0f);
    ui.pan.x = std::clamp(ui.pan.x, -maxPanX, maxPanX);
    ui.pan.y = std::clamp(ui.pan.y, -maxPanY, maxPanY);

    const ImVec2 imgCenter(viewMin.x + viewW * 0.5f + ui.pan.x, viewMin.y + viewH * 0.5f + ui.pan.y);
    const ImVec2 img0 = snapPt(imgCenter.x - imgW * 0.5f, imgCenter.y - imgH * 0.5f);
    const ImVec2 img1 = snapPt(imgCenter.x + imgW * 0.5f, imgCenter.y + imgH * 0.5f);

    // Stage keyboard shortcuts (0/1/2 zoom presets + compare curtain controls)
    if (ui.activeTab == 0 && !modal && !io.WantTextInput && !ImGui::IsAnyItemActive() &&
        !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) {
            ui.fitMode = true;
            ui.zoom = 1.0f;
            ui.pan = ImVec2(0.0f, 0.0f);
        } else if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) {
            ui.fitMode = false;
            ui.zoom = std::clamp(1.0f / std::max(0.01f, fitScale), 0.1f, 32.0f);
            ui.pan = ImVec2(0.0f, 0.0f);
        } else if (ImGui::IsKeyPressed(ImGuiKey_2) || ImGui::IsKeyPressed(ImGuiKey_Keypad2)) {
            ui.fitMode = false;
            ui.zoom = std::clamp(2.0f / std::max(0.01f, fitScale), 0.1f, 32.0f);
            ui.pan = ImVec2(0.0f, 0.0f);
        }
        if (isCompare) {
            const float step = io.KeyShift ? 0.10f : 0.02f;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) {
                ui.splitRatio = std::clamp(ui.splitRatio - step, 0.0f, 1.0f);
            } else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
                ui.splitRatio = std::clamp(ui.splitRatio + step, 0.0f, 1.0f);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Home)) {
                ui.splitRatio = 0.0f;
            } else if (ImGui::IsKeyPressed(ImGuiKey_End)) {
                ui.splitRatio = 1.0f;
            }
        }
    }

    const float cTop = std::max(viewMin.y, img0.y);
    const float cBot = std::min(viewMax.y, img1.y);
    const float cLeft = std::max(viewMin.x, img0.x);
    const float cRight = std::min(viewMax.x, img1.x);
    const float splitSpanW = (cRight > cLeft) ? (cRight - cLeft) : viewW;
    const float splitBaseX = (cRight > cLeft) ? cLeft : viewMin.x;
    const float zoomCapTopY = snapPx(stageWellR.bottom() - detail::kZoomCapsuleMargin - detail::kZoomCapsuleH);
    float splitX = snapPx(splitBaseX + splitSpanW * std::clamp(ui.splitRatio, 0.0f, 1.0f));

    bool splitDragHov = false;
    bool splitDragging = false;
    if (isCompare && !isRunning && !isError && !modal) {
        if (ui.fitMode && !ImGui::IsKeyDown(ImGuiKey_Space)) {
            const float stageDragH = std::max(1.0f, zoomCapTopY - 4.0f - viewMin.y);
            ImGui::SetCursorScreenPos(viewMin);
            ImGui::SetNextItemAllowOverlap();
            ImGui::InvisibleButton("##stage_split_anywhere", ImVec2(viewW, stageDragH));
            if (ImGui::IsItemActive()) {
                splitDragging = true;
                ui.splitRatio = std::clamp((io.MousePos.x - splitBaseX) / splitSpanW, 0.0f, 1.0f);
                splitX = snapPx(splitBaseX + splitSpanW * ui.splitRatio);
            }
        }
        if (cBot > cTop) {
            const float dragTop = cTop;
            const float dragBot = std::max(dragTop + 1.0f, std::min(cBot, zoomCapTopY - 4.0f));
            ImGui::SetCursorScreenPos(ImVec2(splitX - 16.0f, dragTop));
            ImGui::InvisibleButton("##split_drag", ImVec2(32.0f, std::max(1.0f, dragBot - dragTop)));
            splitDragHov = ImGui::IsItemHovered();
            if (ImGui::IsItemActive()) {
                splitDragging = true;
                ui.splitRatio = std::clamp((io.MousePos.x - splitBaseX) / splitSpanW, 0.0f, 1.0f);
                splitX = snapPx(splitBaseX + splitSpanW * ui.splitRatio);
            }
        }
        if (splitDragHov || splitDragging) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
    }

    dl->PushClipRect(viewMin, viewMax, true);

    if (isCompare) {
        dl->PushClipRect(viewMin, ImVec2(splitX, viewMax.y), true);
        drawCheckerboard(dl, img0, img1, 10.0f);
        drawStageArtwork(dl, img0, img1, false, ui, stagePixelated);
        dl->PopClipRect();

        dl->PushClipRect(ImVec2(splitX, viewMin.y), viewMax, true);
        drawCheckerboard(dl, img0, img1, 10.0f);
        drawStageArtwork(dl, img0, img1, true, ui, stagePixelated);
        dl->PopClipRect();

        // Vertical curtain line + halo clamped to visible image bounds [cTop, cBot]
        if (cBot > cTop) {
            const float curtainHovT = motion::hoverFactor(
                ImGui::GetID("##curtain_hov"),
                !spacePan && !midPan && (splitDragging || splitDragHov));

            const float lineT = hairlinePx();
            const ImU32 haloCol = withAlpha(t.bg, t.curtainHaloAlpha * (0.6f + 0.3f * curtainHovT));
            dl->AddRectFilled(ImVec2(splitX - lineT, cTop), ImVec2(splitX, cBot), haloCol);
            dl->AddRectFilled(ImVec2(splitX + lineT, cTop), ImVec2(splitX + lineT * 2.0f, cBot), haloCol);
            dl->AddRectFilled(ImVec2(splitX, cTop), ImVec2(splitX + lineT, cBot), t.ink);

            // Floating pill handle with centered 2px t.accent bar inside
            float handleY = std::min((cTop + cBot) * 0.5f, zoomCapTopY - 26.0f);
            if (cBot - cTop >= 40.0f) {
                handleY = std::clamp(handleY, cTop + 20.0f, cBot - 20.0f);
            } else {
                handleY = (cTop + cBot) * 0.5f;
            }
            const ImVec2 hC = snapPt(splitX, handleY);
            const float halfW = snapPx(9.0f + 1.0f * curtainHovT);
            const float halfH = snapPx(18.0f + 1.0f * curtainHovT);
            const float barH = snapPx(16.0f + 2.0f * curtainHovT);
            const flex::Rect handleBounds{hC.x - halfW, hC.y - halfH, halfW * 2.0f, halfH * 2.0f};

            static flex::Context curtainCtx;
            flex::Layout hfx(curtainCtx);
            auto hBox = hfx.row({
                .justify = flex::Justify::Center,
                .align = flex::Align::Center,
            });
            const flex::Slot barSlot = hBox.slot(flex::Fixed(2.0f), flex::Fixed(barH));
            hfx.solve(handleBounds);
            const flex::Rect hR = hBox.rect();
            const flex::Rect barR = hfx.rect(barSlot);

            const bool dark = (geist::mode() == geist::Mode::Dark);
            const ImU32 handleRestBg = dark ? hex(0x18181B) : t.surface;
            const ImU32 handleHovBg = dark ? t.highest : t.container;
            const ImU32 handleBg = motion::mix(handleRestBg, handleHovBg, curtainHovT);
            const ImU32 handleBorder = motion::mix(t.outlineStrong, t.ink, curtainHovT * 0.65f);
            const ImU32 barCol = motion::mix(t.accent, t.accentHover, curtainHovT);

            if (curtainHovT > 0.01f) {
                dl->AddRect(ImVec2(hR.x - 2.0f, hR.y - 2.0f),
                            ImVec2(hR.right() + 2.0f, hR.bottom() + 2.0f),
                            withAlpha(t.accent, 0.24f * curtainHovT),
                            geist::radius::MD + 2.0f, 0, hairlinePx());
            }
            ctl::finish(dl, hR.p0(), hR.p1(), ctl::Finish::Whisper, geist::radius::MD);
            dl->AddRectFilled(hR.p0(), hR.p1(), handleBg, geist::radius::MD);
            dl->AddRect(hR.p0(), hR.p1(), handleBorder, geist::radius::MD, 0, hairlinePx());
            dl->AddRectFilled(barR.p0(), barR.p1(), barCol, 1.0f);
        }
    } else {
        drawCheckerboard(dl, img0, img1, 10.0f);
        drawStageArtwork(dl, img0, img1, false, ui, stagePixelated);
        if (isRunning) {
            dl->AddRectFilled(viewMin, viewMax, withAlpha(t.bg, t.veilAlpha));
        }
    }

    if (isRunning) {
        detail::renderRunningTelemetryPill(dl, stageWellR, ui, snap);
    } else if (isError) {
        detail::renderErrorVeilCard(dl, stageWellR, ui, app, snap, activeItem);
    } else {
        if (isCompare) {
            char beforeTag[48] = {};
            if (snap.job.in_w > 0 && snap.job.in_h > 0) {
                detail::formatKeyInto(beforeTag, upix::i18n::Key::WbCompareOrigDims, snap.job.in_w, snap.job.in_h);
            } else if (activeItem && activeItem->width > 0 && activeItem->height > 0) {
                detail::formatKeyInto(beforeTag, upix::i18n::Key::WbCompareOrigDims, activeItem->width, activeItem->height);
            } else {
                detail::formatInto(beforeTag, "{}", upix::i18n::tr(upix::i18n::Key::WbCompareOrig));
            }

            char afterTag[48] = {};
            const int scale = detail::modelScaleFor(snap);
            if (snap.job.out_w > 0 && snap.job.out_h > 0) {
                detail::formatInto(afterTag, "{}× {}×{}", scale, snap.job.out_w, snap.job.out_h);
            } else {
                detail::formatInto(afterTag, "{}×", scale);
            }

            const ImVec2 bSz = calcText(monoFont, geist::type::MONO, beforeTag);
            const ImVec2 aSz = calcText(monoFont, geist::type::MONO, afterTag);
            constexpr float kTagPadX = 8.0f;
            constexpr float kTagPadY = 3.0f;
            const float bW = std::ceil(bSz.x) + kTagPadX * 2.0f;
            const float bH = std::ceil(bSz.y) + kTagPadY * 2.0f;
            const float aW = std::ceil(aSz.x) + kTagPadX * 2.0f;
            const float aH = std::ceil(aSz.y) + kTagPadY * 2.0f;

            static flex::Context tagCtx;
            flex::Layout tagFx(tagCtx);
            auto tagRow = tagFx.row({
                .padding = flex::Edges::xy(12.0f, 12.0f),
                .justify = flex::Justify::SpaceBetween,
                .align = flex::Align::Start,
            });
            const flex::Slot bSlot = tagRow.slot(flex::Fixed(bW), flex::Fixed(bH));
            const flex::Slot aSlot = tagRow.slot(flex::Fixed(aW), flex::Fixed(aH));
            tagFx.solve(flex::Rect{viewMin.x, viewMin.y, viewW, viewH});

            const flex::Rect bTagR = tagFx.rect(bSlot);
            const flex::Rect aTagR = tagFx.rect(aSlot);

            ctl::finish(dl, bTagR.p0(), bTagR.p1(), ctl::Finish::Whisper, geist::radius::SM);
            drawTextCentered(dl, monoFont, geist::type::MONO, bTagR.p0(), bTagR.p1(), t.body, beforeTag);

            ctl::finish(dl, aTagR.p0(), aTagR.p1(), ctl::Finish::Whisper, geist::radius::SM);
            drawTextCentered(dl, monoFont, geist::type::MONO, aTagR.p0(), aTagR.p1(), t.body, afterTag);
        }

        flex::Rect zoomCapR;
        detail::renderFloatingZoomCapsule(dl, stageWellR, ui, fitScale, effScale, stagePixelated, zoomCapR);
    }

    dl->PopClipRect();
}

} // namespace

void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, upix::gui::AppState& app,
                     const upix::gui::Snapshot& snap, SDL_Window* window,
                     DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool modal = ui.showModelPicker;

    static detail::FilmstripState filmstripState;
    static detail::ModelPickerState pickerState;
    static detail::InspectorState inspectorState;
    static int lastActiveIdx = -1;
    static std::string lastActivePath;
    static bool stagePixelated = false;
    static bool wasPickerOpen = false;

    const bool pickerJustOpened = modal && !wasPickerOpen;
    wasPickerOpen = modal;

    if (snap.active_input != lastActiveIdx ||
        (!snap.inputs.empty() &&
         snap.inputs[static_cast<size_t>(std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1))].path != lastActivePath)) {
        lastActiveIdx = snap.active_input;
        lastActivePath = snap.inputs.empty()
            ? std::string{}
            : snap.inputs[static_cast<size_t>(std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1))].path;
        ui.fitMode = true;
        ui.zoom = 1.0f;
        ui.pan = ImVec2(0.0f, 0.0f);
    }

    const ImVec2 wsMin(0.0f, kTopbarH);
    const ImVec2 wsMax = winSize;
    const float wsW = std::max(0.0f, wsMax.x - wsMin.x);
    const float wsH = std::max(0.0f, wsMax.y - wsMin.y);
    dl->AddRectFilled(wsMin, wsMax, t.wellBg);

    const bool hasInputs = !snap.inputs.empty() || snap.job.phase != upix::gui::JobPhase::Idle;
    const bool isRunning = (snap.job.phase == upix::gui::JobPhase::Running ||
                            snap.job.phase == upix::gui::JobPhase::Cancelling);
    const bool isCompare = (snap.job.phase == upix::gui::JobPhase::Done);
    const bool isError = (snap.job.phase == upix::gui::JobPhase::Error);
    const bool hasDock = hasInputs;

    if (ui.requestOpenFiles) {
        ui.requestOpenFiles = false;
        if (!modal && !isRunning) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    }

    const upix::gui::InputItem* activeItem = nullptr;
    if (!snap.inputs.empty()) {
        const int idx = std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1);
        activeItem = &snap.inputs[static_cast<size_t>(idx)];
    }

    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;
    const float dockH = std::max(70.0f, std::ceil(calcText(monoFont, geist::type::MONO, "0/0").y) + 28.0f);
    const float targetInspW = (wsW < 920.0f) ? std::clamp(wsW * 0.38f, 288.0f, 368.0f) : 368.0f;

    static flex::Context shellCtx;
    flex::Layout fx(shellCtx);
    auto root = fx.row({
        .padding = flex::Edges::all(0.0f),
        .gap = 0.0f,
        .align = flex::Align::Stretch,
    });
    auto stageCol = root.col({
        .gap = 0.0f,
        .align = flex::Align::Stretch,
        .width = flex::Grow(1.0f).min(240.0f),
    });
    const flex::Slot stageWellSlot = stageCol.slot(flex::Auto(), flex::Grow(1.0f).min(120.0f));
    const flex::Slot dockSlot = stageCol.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(dockH),
        .visible = hasDock,
    });
    const flex::Slot vDivSlot = root.slot(flex::Fixed(hairlinePx()), flex::Fraction(1.0f));
    const flex::Slot inspSlot = root.slot(flex::Shrink(targetInspW, 1.0f).min(280.0f), flex::Fraction(1.0f));

    fx.solve(flex::Rect{wsMin.x, wsMin.y, wsW, wsH});

    const flex::Rect stageWellR = fx.rect(stageWellSlot);
    const flex::Rect dockRect = fx.rect(dockSlot);
    const bool dockVisible = fx.isVisible(dockSlot);
    const flex::Rect vDivR = fx.rect(vDivSlot);
    const flex::Rect inspRect = fx.rect(inspSlot);

    // Suppress background hits while centered model picker modal is open.
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 savedMousePos = io.MousePos;
    if (modal) {
        io.MousePos = ImVec2(-1.0e6f, -1.0e6f);
    }

    dl->AddRectFilled(stageWellR.p0(), stageWellR.p1(), t.wellBg);
    if (!hasInputs) {
        detail::renderEmptyDropzone(dl, stageWellR, ui, app, snap, window, dlgFiles, dlgFolder, modal);
    } else {
        renderStageCanvas(dl, stageWellR, ui, app, snap, activeItem,
                          isRunning, isCompare, isError, modal, stagePixelated);
    }

    if (dockVisible) {
        detail::renderFilmstripDock(dl, dockRect, ui, app, snap, window, dlgFiles, isRunning, modal,
                                    filmstripState);
    }

    ctl::vHairline(dl, vDivR.x, vDivR.y, vDivR.bottom(), t.outline);

    detail::renderInspectorPane(dl, inspRect, ui, app, snap, activeItem, window, dlgOutDir,
                                isRunning, isCompare, isError, modal, savedMousePos,
                                inspectorState, pickerState);

    if (modal) {
        io.MousePos = savedMousePos;
        detail::renderModelPickerModal(winSize, ui, app, snap, pickerState, pickerJustOpened);
    }
}

} // namespace upix::views
