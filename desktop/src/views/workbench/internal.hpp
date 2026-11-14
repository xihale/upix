// views/workbench/internal.hpp — shared state, text helpers, and sub-view declarations for Workbench.
#pragma once

#include "views/common.hpp"
#include "ui/flex.hpp"

#include "imgui.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace upix::views::detail {
inline namespace wb {

inline constexpr float kZoomCapsuleH = 34.0f;
inline constexpr float kZoomCapsuleMargin = 12.0f;

inline float textBasis(float w) {
    return std::ceil(w) + 2.0f;
}

template <size_t N, typename... Args>
inline const char* formatInto(char (&buf)[N], std::format_string<Args...> fmt, Args&&... args) {
    const auto res = std::format_to_n(buf, N - 1, fmt, std::forward<Args>(args)...);
    *res.out = '\0';
    return buf;
}

struct StaticTextCache {
    const ImFont* font = nullptr;
    float size = 0.0f;
    float fbScale = 0.0f;
    const char* ptr = nullptr;
    ImVec2 val{};

    ImVec2 get(ImFont* f, float sz, const char* text) {
        if (!f) f = ImGui::GetFont();
        const float fb = ImGui::GetIO().DisplayFramebufferScale.x;
        if (font != f || size != sz || fbScale != fb || ptr != text) {
            font = f;
            size = sz;
            fbScale = fb;
            ptr = text;
            val = upix::ui::calcText(f, sz, text);
        }
        return val;
    }
};

inline void drawTextFit(ImDrawList* dl, ImFont* font, float size, const upix::ui::flex::Rect& r,
                        ImU32 col, const char* text, float measuredW) {
    if (measuredW <= r.w) {
        upix::ui::drawTextCenteredY(dl, font, size, r.x, r.y, r.bottom(), col, text);
    } else {
        upix::ui::ellipsisTextCentered(dl, font, size, r.x, r.y, r.bottom(), r.w, col, text);
    }
}

inline void drawTextFitCentered(ImDrawList* dl, ImFont* font, float size, const upix::ui::flex::Rect& r,
                                ImU32 col, const char* text, float measuredW) {
    if (measuredW <= r.w) {
        upix::ui::drawTextCentered(dl, font, size, r.p0(), r.p1(), col, text);
    } else {
        upix::ui::ellipsisTextCentered(dl, font, size, r.x, r.y, r.bottom(), r.w, col, text);
    }
}

struct FilmstripState {
    float scroll = 0.0f;
    float scrollTarget = 0.0f;
};

struct ModelPickerState {
    char search[128] = {};
    float scroll = 0.0f;
    float scrollTarget = 0.0f;

    void reset() {
        search[0] = '\0';
        scroll = 0.0f;
        scrollTarget = 0.0f;
    }
};

struct InspectorState {
    float scroll = 0.0f;
    float scrollTarget = 0.0f;
    std::string doneParamSig;
    upix::gui::JobPhase prevJobPhase = upix::gui::JobPhase::Idle;
    std::vector<std::string> prevDoneOutputs;
};

void drawDashedRoundedRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius,
                           ImU32 col, float thickness, float dashLen = 6.0f, float gapLen = 4.0f);

const char* inferInputFormatLabel(const upix::gui::InputItem* active);
int modelScaleFor(const upix::gui::Snapshot& snap);
void openFileInSystem(const std::string& path);
void revealFileParentInSystem(const std::string& path);

void renderEmptyDropzone(ImDrawList* dl, const upix::ui::flex::Rect& stageRect,
                         const UiViewState& ui, upix::gui::AppState& app,
                         const upix::gui::Snapshot& snap, SDL_Window* window,
                         DialogBridge* dlgFiles, DialogBridge* dlgFolder, bool modal);

void renderFloatingZoomCapsule(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                               UiViewState& ui, float fitScale, float effScale,
                               bool& stagePixelated, upix::ui::flex::Rect& outCapsuleR);

void renderRunningTelemetryPill(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                                const UiViewState& ui, const upix::gui::Snapshot& snap);

void renderErrorVeilCard(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                         UiViewState& ui, upix::gui::AppState& app,
                         const upix::gui::Snapshot& snap, const upix::gui::InputItem* activeItem);

void renderFilmstripDock(ImDrawList* dl, const upix::ui::flex::Rect& dockRect, UiViewState& ui,
                         upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                         SDL_Window* window, DialogBridge* dlgFiles, bool isRunning, bool modal,
                         FilmstripState& state);

void renderInspectorPane(ImDrawList* dl, const upix::ui::flex::Rect& inspRect,
                         UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                         const upix::gui::InputItem* activeItem, SDL_Window* window,
                         DialogBridge* dlgOutDir, bool isRunning, bool isCompare, bool isError,
                         bool modal, ImVec2 savedMousePos,
                         InspectorState& state, ModelPickerState& pickerState);

void renderModelPickerModal(ImVec2 winSize, UiViewState& ui, upix::gui::AppState& app,
                            const upix::gui::Snapshot& snap, ModelPickerState& state,
                            bool justOpened);

} // namespace wb
} // namespace upix::views::detail
