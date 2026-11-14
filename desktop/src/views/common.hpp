// views/common.hpp — shared view-layer state and small draw helpers.
#pragma once

#include "app_state.hpp"
#include "ui/controls.hpp"
#include "ui/geist.hpp"
#include "ui/icons.hpp"
#include "ui/motion.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace upix::views {

inline constexpr float kTopbarH = 52.0f;

struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkSampler samplerNearest = VK_NULL_HANDLE;
    VkDescriptorSet desc = VK_NULL_HANDLE;
    VkDescriptorSet descNearest = VK_NULL_HANDLE;
    int width = 0;
    int height = 0;

    bool valid() const { return desc != VK_NULL_HANDLE; }
    VkDescriptorSet descriptor(bool nearest) const noexcept {
        return (nearest && descNearest != VK_NULL_HANDLE) ? descNearest : desc;
    }
};

struct MmapFontRegion {
    void* ptr = nullptr;
    size_t size = 0;
};

/// Handle to one native file dialog (SDL3) guarded by lifetime flag.
struct DialogBridge {
    upix::gui::AppState* app = nullptr;
    int mode = 0; // 0 = input files, 1 = input folder, 2 = output folder, 3 = import models
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> recursive{false};
};

inline void SDLCALL onFileDialog(void* userdata, const char* const* filelist, int /*filter*/) {
    auto* bridge = static_cast<DialogBridge*>(userdata);
    if (!bridge || !bridge->alive || !bridge->alive->load(std::memory_order_acquire)) return;
    if (!bridge->app || !filelist) return;
    std::vector<std::string> paths;
    for (int i = 0; filelist[i] != nullptr; ++i) {
        if (filelist[i][0] != '\0') paths.emplace_back(filelist[i]);
    }
    if (paths.empty()) return;
    if (bridge->mode == 0) {
        bridge->app->addPaths(paths);
    } else if (bridge->mode == 1) {
        const bool rec = bridge->recursive.load(std::memory_order_acquire);
        for (const auto& p : paths) {
            bridge->app->addFolder(p, rec);
        }
    } else if (bridge->mode == 2) {
        bridge->app->setCustomOutDir(paths[0]);
    } else if (bridge->mode == 3) {
        bridge->app->importModels(paths);
    }
}

struct TopbarHitRegion {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    bool contains(float x, float y) const noexcept {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    }
};

struct UiViewState {
    int activeTab = 0; // 0 = 工作台, 1 = 模型, 2 = 远程, 3 = 设置
    float splitRatio = 0.5f;
    bool showModelPicker = false;
    bool showIpPicker = false;
    bool requestOpenFiles = false;
    std::string modelsSceneFilter = "all";
    char modelsSearch[128] = {};
    float zoom = 1.0f;
    ImVec2 pan{0.0f, 0.0f};
    bool fitMode = true;
    int requestDarkMode = -1; // -1 none, 0 light, 1 dark

    bool showWindowControls = false;
    bool windowMaximized = false;
    int requestWindowAction = 0; // 0 none, 1 minimize, 2 toggle maximize, 3 close
    TopbarHitRegion topbarBrandRect;
    TopbarHitRegion topbarTabsRect;
    TopbarHitRegion topbarRightRect;
    bool modalBlockingHitTest = false;

    ImFont* fontSans = nullptr;
    ImFont* fontSansMedium = nullptr;
    ImFont* fontMono = nullptr;

    VulkanTexture beforeTex;
    std::string beforeTexPath;
    VulkanTexture afterTex;
    std::string afterTexPath;

    // Small bounded LRU filmstrip thumbnail cache (keyed by input file path)
    std::unordered_map<std::string, VulkanTexture> thumbCache;
    std::vector<std::string> thumbLru;

    std::vector<MmapFontRegion> mappedFonts;
};

inline void drawCheckerboard(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float cellPx = 10.0f) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (p1.x <= p0.x || p1.y <= p0.y || cellPx <= 0.0f) return;
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, t.checkerA);
    const int cols = static_cast<int>(std::ceil((p1.x - p0.x) / cellPx));
    const int rows = static_cast<int>(std::ceil((p1.y - p0.y) / cellPx));
    for (int r = 0; r < rows; ++r) {
        for (int c = (r & 1); c < cols; c += 2) {
            ImVec2 q0 = snapPt(p0.x + c * cellPx, p0.y + r * cellPx);
            ImVec2 q1 = snapPt(std::min(p1.x, p0.x + (c + 1) * cellPx),
                               std::min(p1.y, p0.y + (r + 1) * cellPx));
            if (q1.x > q0.x && q1.y > q0.y) {
                dl->AddRectFilled(q0, q1, t.checkerB);
            }
        }
    }
    dl->PopClipRect();
}

inline void drawStageArtwork(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool highRes, const UiViewState& ui, bool nearest = false) {
    using namespace upix::ui;
    const VulkanTexture& tex = (highRes && ui.afterTex.valid()) ? ui.afterTex : ui.beforeTex;
    if (tex.valid()) {
        const ImU32 beforeTint = IM_COL32_WHITE;
        const ImU32 tint = (highRes || !ui.afterTex.valid()) ? IM_COL32_WHITE : beforeTint;
        dl->AddImage((ImTextureID)(intptr_t)tex.descriptor(nearest), p0, p1, ImVec2(0, 0), ImVec2(1, 1), tint);
        return;
    }
    dl->AddRectFilled(p0, p1, geist::tokens().wellBg);
}

inline void drawMiniThumb(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const UiViewState& ui,
                          float radius = 4.0f, size_t cropIdx = 0, const std::string& path = {}) {
    using namespace upix::ui;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (!path.empty()) {
        auto it = ui.thumbCache.find(path);
        if (it != ui.thumbCache.end() && it->second.valid()) {
            dl->AddImageRounded((ImTextureID)(intptr_t)it->second.desc, p0, p1,
                                ImVec2(0, 0), ImVec2(1, 1), rgba(255, 255, 255, 255), radius);
            return;
        }
    }
    const VulkanTexture& fallback = ui.beforeTex.valid() ? ui.beforeTex : ui.afterTex;
    if (fallback.valid()) {
        float ox = (cropIdx % 3) * 0.08f;
        float oy = ((cropIdx / 2) % 2) * 0.08f;
        ImVec2 uv0(0.10f + ox, 0.05f + oy);
        ImVec2 uv1(0.74f + ox, 0.85f + oy);
        dl->AddImageRounded((ImTextureID)(intptr_t)fallback.desc, p0, p1, uv0, uv1, rgba(255, 255, 255, 255), radius);
    } else {
        dl->AddRectFilled(p0, p1, geist::tokens().container, radius);
    }
}

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap);
void renderTopBar(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap);
void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                     upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     SDL_Window* window, DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir);
void renderModelsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                      SDL_Window* window, DialogBridge* dlgImport);
void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap);
void renderSettingsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap);
void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap);

} // namespace upix::views
