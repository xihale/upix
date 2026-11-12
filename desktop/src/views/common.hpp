// views/common.hpp — shared view-layer state and small draw helpers.
// Mechanically extracted from main.cpp; see docs/gui porting notes.
#pragma once

#include "app_state.hpp"
#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <string>
#include <vector>

// Desktop view-layer contract: every page renders into one ImDrawList over the
// whole window. UiViewState carries per-frame UI state; AppState (upix::gui)
// carries the domain store. Pages own no Vulkan and no SDL event access beyond
// the DialogBridge handles handed to them.
namespace upix::views {

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;

    bool valid() const { return desc != VK_NULL_HANDLE; }
};

struct MmapFontRegion {
    void* ptr = nullptr;
    size_t size = 0;
};

/// Handle to one fire-and-forget native file dialog (SDL3).
struct DialogBridge {
    upix::gui::AppState* app = nullptr;
    int mode = 0; // 0 = input files, 1 = input folder, 2 = output folder, 3 = import models
};

inline void SDLCALL onFileDialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* bridge = static_cast<DialogBridge*>(userdata);
    if (!bridge || !bridge->app || !filelist) return;
    std::vector<std::string> paths;
    for (int i = 0; filelist[i] != nullptr; ++i) {
        if (filelist[i][0] != '\0') paths.emplace_back(filelist[i]);
    }
    if (paths.empty()) return;
    if (bridge->mode == 0 || bridge->mode == 1) {
        bridge->app->addPaths(paths);
    } else if (bridge->mode == 2) {
        bridge->app->setCustomOutDir(paths[0]);
    } else if (bridge->mode == 3) {
        bridge->app->importModels(paths);
    }
}

struct UiViewState {
    int activeTab = 0; // 0 = 放大 (Stage), 1 = 模型 (Models), 2 = 远程 (Host)
    float splitRatio = 0.545f; // Figma 71:1129 uses 480 / 880 = 0.545f
    bool showModelPicker = false;
    std::string modelsSceneFilter = "all";
    upix::ui::Palette pal{true};
    ImFont* fontSans = nullptr;
    ImFont* fontMono = nullptr;
    VulkanTexture stageTex;
    std::string stageTexPath;
    std::vector<MmapFontRegion> mappedFonts;
};


inline void drawStageArtwork(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool highRes, const UiViewState& ui) {
    using namespace upix::ui;
    if (ui.stageTex.valid()) {
        ImU32 tint = highRes ? rgba(255, 255, 255, 255) : rgba(228, 228, 232, 242);
        dl->AddImage((ImTextureID)(intptr_t)ui.stageTex.desc, p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }
    dl->AddRectFilledMultiColor(p0, p1, hex(0x0E1B1E), hex(0x13262A), hex(0x161E24), hex(0x0D1217));
}

inline void drawMiniThumb(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const UiViewState& ui, float radius = 4.0f, size_t cropIdx = 0) {
    using namespace upix::ui;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (ui.stageTex.valid()) {
        // Slightly vary crop window per index so filmstrip slots look like distinct shots when using a single preview texture
        float ox = (cropIdx % 3) * 0.08f;
        float oy = ((cropIdx / 2) % 2) * 0.08f;
        ImVec2 uv0(0.10f + ox, 0.05f + oy);
        ImVec2 uv1(0.74f + ox, 0.85f + oy);
        dl->AddImageRounded((ImTextureID)(intptr_t)ui.stageTex.desc, p0, p1, uv0, uv1, rgba(255, 255, 255, 255), radius);
    } else {
        dl->AddRectFilled(p0, p1, hex(0x121216), radius);
    }
}

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap);
void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                     upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     SDL_Window* window, DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir);
void renderModelsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                      SDL_Window* window, DialogBridge* dlgImport);
void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap);
void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap);

} // namespace upix::views
