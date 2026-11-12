#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace upix::views {
namespace {

bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

bool nameHasScale(const std::string& name, int scale) {
    const std::string n = std::to_string(scale);
    return name.find("x" + n) != std::string::npos || name.find("X" + n) != std::string::npos ||
           name.find(n + "×") != std::string::npos;
}

std::string formatDisplayLabel(const upix::gui::Snapshot& snap, const upix::gui::InputItem* active,
                               const std::string& outPath) {
    auto mapExt = [](std::string ext) -> std::string {
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
        if (ext == "jpg" || ext == "jpeg") return "JPG";
        if (ext == "webp") return "WebP";
        if (ext == "avif") return "AVIF";
        if (ext == "png") return "PNG";
        return {};
    };
    if (!snap.format.empty()) {
        std::string f = mapExt(snap.format);
        if (!f.empty()) return f;
    }
    if (!outPath.empty() && outPath != "__fixture_after__") {
        std::string f = mapExt(std::filesystem::path(outPath).extension().string());
        if (!f.empty()) return f;
    }
    if (active && !active->path.empty() && active->path != "__fixture_before__") {
        std::string f = mapExt(std::filesystem::path(active->path).extension().string());
        if (!f.empty()) return f;
    }
    return "PNG";
}

int modelScaleFor(const upix::gui::Snapshot& snap) {
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) return std::max(1, m.scale);
    }
    return 4;
}

void drawHudPill(ImDrawList* dl, ImFont* font, ImVec2 p0, float w, const char* text, ImU32 col) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    const ImVec2 p1 = snapPt(p0.x + w, p0.y + 26.0f);
    dl->AddRectFilled(p0, p1, t.hudBg, geist::radius::MD);
    dl->AddRect(p0, p1, t.hudBorder, geist::radius::MD, 0, hairlinePx());
    drawTextCentered(dl, font, geist::type::LABEL, p0, p1, col, text);
}

bool hudPillButton(ImDrawList* dl, ImGuiID id, ImFont* font, ImVec2 p0, float w,
                   const char* label, bool active, bool enabled = true) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    const ImVec2 p1 = snapPt(p0.x + w, p0.y + 26.0f);

    ImGui::SetCursorScreenPos(p0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "##hp%08x", static_cast<unsigned>(id));
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton(buf, ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (!enabled) ImGui::EndDisabled();
    const bool hov = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float sc = motion::pressScale(id, held, motion::PRESS_SCALE);
    ImVec2 q0 = p0, q1 = p1;
    if (enabled && std::fabs(sc - 1.0f) > 0.001f) {
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const float hx = (p1.x - p0.x) * 0.5f * sc;
        const float hy = (p1.y - p0.y) * 0.5f * sc;
        q0 = snapPt(c.x - hx, c.y - hy);
        q1 = snapPt(c.x + hx, c.y + hy);
    }

    const ImU32 fill = active ? t.accentSoft : (hov ? t.high : t.hudBg);
    const ImU32 rim = active ? t.accentBorder : (hov ? t.outlineInput : t.hudBorder);
    const ImU32 fg = active ? t.accent : (hov ? t.ink : t.body);
    dl->AddRectFilled(q0, q1, fill, geist::radius::MD);
    dl->AddRect(q0, q1, rim, geist::radius::MD, 0, hairlinePx());
    drawTextCentered(dl, font, geist::type::LABEL, q0, q1, fg, label);
    return enabled && clicked;
}

bool hudIconButton(ImDrawList* dl, ImGuiID id, ImVec2 p0, float box, const char* icon,
                   const char* tip, ImFont* font, bool enabled) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    const ImVec2 p1 = snapPt(p0.x + box, p0.y + box);

    ImGui::SetCursorScreenPos(p0);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "##hi%08x", static_cast<unsigned>(id));
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::InvisibleButton(buf, ImVec2(p1.x - p0.x, p1.y - p0.y));
    if (!enabled) ImGui::EndDisabled();
    const bool hov = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    const bool focused = enabled && ImGui::IsItemFocused();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float sc = motion::pressScale(id, held, motion::PRESS_SCALE_ICON);
    ImVec2 q0 = p0, q1 = p1;
    if (enabled && std::fabs(sc - 1.0f) > 0.001f) {
        const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
        const float h = box * 0.5f * sc;
        q0 = snapPt(c.x - h, c.y - h);
        q1 = snapPt(c.x + h, c.y + h);
    }

    dl->AddRectFilled(q0, q1, (enabled && hov) ? t.high : t.hudBg, geist::radius::MD);
    dl->AddRect(q0, q1, (enabled && hov) ? t.outlineInput : t.hudBorder, geist::radius::MD, 0, hairlinePx());
    drawIcon(dl, icon, ImVec2((q0.x + q1.x) * 0.5f, (q0.y + q1.y) * 0.5f), 13.0f,
             enabled ? (hov ? t.ink : t.mute) : t.faint);
    if (enabled) {
        ctl::tooltip(dl, id + 0x1234u, p0, p1, tip, font, hov, focused);
    }
    return enabled && clicked;
}

void drawStageInsetWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ctl::finish(dl, p0, p1, ctl::Finish::Inset, radius);
    dl->AddRect(snapPt(p0), snapPt(p1), t.outline, radius, 0, hairlinePx());
}

void openFileInSystem(const std::string& path) {
    if (path.empty() || path.rfind("__fixture", 0) == 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    SDL_OpenURL(("file://" + path).c_str());
}

void revealFileParentInSystem(const std::string& path) {
    if (path.empty() || path.rfind("__fixture", 0) == 0) return;
    std::error_code ec;
    std::filesystem::path p(path);
    std::filesystem::path dir = std::filesystem::is_directory(p, ec) ? p : p.parent_path();
    if (!dir.empty() && std::filesystem::exists(dir, ec)) {
        SDL_OpenURL(("file://" + dir.string()).c_str());
    }
}

} // namespace

void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, upix::gui::AppState& app,
                     const upix::gui::Snapshot& snap, SDL_Window* window,
                     DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool dark = geist::mode() == geist::Mode::Dark;
    const bool modal = ui.showModelPicker;

    static char pickerSearch[128] = {};
    static float pickerScroll = 0.0f;
    static float dockScroll = 0.0f;
    static int lastActiveIdx = -1;
    static std::string lastActivePath;

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

    const float railW = 76.0f;
    const float inspCol = 332.0f;
    dl->AddRectFilled(ImVec2(railW, 0.0f), winSize, t.bg);

    const float stageX = railW;
    const float stageW = std::max(0.0f, winSize.x - railW - inspCol);
    const bool hasInputs = !snap.inputs.empty() || snap.job.phase != upix::gui::JobPhase::Idle;
    const bool isRunning = (snap.job.phase == upix::gui::JobPhase::Running ||
                            snap.job.phase == upix::gui::JobPhase::Cancelling);
    const bool hasDock = snap.inputs.size() > 1 || isRunning || snap.job.phase == upix::gui::JobPhase::Error;
    const float dockBlock = hasDock ? (54.0f + 10.0f) : 0.0f;
    const ImVec2 stageMin = snapPt(stageX + geist::space::SM, geist::space::LG);
    const ImVec2 stageMax = snapPt(stageX + stageW - geist::space::SM, winSize.y - geist::space::LG - dockBlock);

    const upix::gui::InputItem* activeItem = nullptr;
    if (!snap.inputs.empty()) {
        const int idx = std::clamp(snap.active_input, 0, static_cast<int>(snap.inputs.size()) - 1);
        activeItem = &snap.inputs[static_cast<size_t>(idx)];
    }

    // While the model picker popup is open, suppress mouse hits on background
    // widgets without dimming them (the popup draws last with real mouse pos).
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 savedMousePos = io.MousePos;
    if (modal) {
        io.MousePos = ImVec2(-1.0e6f, -1.0e6f);
    }

    // ---------------------------------------------------------------- stage
    drawStageInsetWell(dl, stageMin, stageMax, geist::radius::LG);

    if (!hasInputs) {
        drawRegistrationMarks(dl, stageMin, stageMax, 28.0f, 16.0f, 2.0f, t.regMark);

        const float centerX = (stageMin.x + stageMax.x) * 0.5f;
        const float centerY = (stageMin.y + stageMax.y) * 0.5f;
        const float cardW = std::min(380.0f, std::max(260.0f, (stageMax.x - stageMin.x) - 64.0f));
        const float cardH = 224.0f;
        const ImVec2 dz0 = snapPt(centerX - cardW * 0.5f, centerY - cardH * 0.5f);
        const ImVec2 dz1 = snapPt(centerX + cardW * 0.5f, centerY + cardH * 0.5f);

        ctl::groupCard(dl, dz0, dz1);

        drawIcon(dl, "image", ImVec2(centerX, dz0.y + 44.0f), 30.0f, t.mute, 1.7f);

        const char* dropTitle = "拖入图片，或按 Ctrl+V 粘贴";
        drawTextCentered(dl, ui.fontSans, 15.0f, ImVec2(dz0.x + 16.0f, dz0.y + 68.0f),
                         ImVec2(dz1.x - 16.0f, dz0.y + 92.0f), t.ink, dropTitle);
        const char* dropSub = "支持 PNG · JPG · WebP · AVIF 批量超分";
        drawTextCentered(dl, ui.fontSans, geist::type::BODY_SMALL, ImVec2(dz0.x + 16.0f, dz0.y + 92.0f),
                         ImVec2(dz1.x - 16.0f, dz0.y + 112.0f), t.mute, dropSub);

        const float btnY = dz0.y + 126.0f;
        const float btnGap = 8.0f;
        const float filesW = 112.0f;
        const float folderW = 166.0f;
        const float rowW = filesW + btnGap + folderW;
        const float rowX = snapPx(centerX - rowW * 0.5f);

        if (ctl::secondaryButton(dl, ImGui::GetID("##empty_pick_files"), "选择图片", ui.fontSans,
                                 ImVec2(rowX, btnY), ImVec2(rowX + filesW, btnY + 32.0f), "plus")) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
        if (ctl::secondaryButton(dl, ImGui::GetID("##empty_pick_folder"), "选择文件夹批量处理", ui.fontSans,
                                 ImVec2(rowX + filesW + btnGap, btnY),
                                 ImVec2(rowX + filesW + btnGap + folderW, btnY + 32.0f), "folder-open")) {
            if (dlgFolder) dlgFolder->recursive = snap.recursive;
            SDL_ShowOpenFolderDialog(onFileDialog, dlgFolder, window, nullptr, false);
        }

        // Inline recursive toggle below action buttons
        const char* recLabel = "包含子文件夹";
        const ImVec2 recSz = calcText(ui.fontSans, geist::type::BODY_SMALL, recLabel);
        const float recTotalW = recSz.x + 10.0f + ctl::geom::TOGGLE_W;
        const float recX = snapPx(centerX - recTotalW * 0.5f);
        const float recY = snapPx(btnY + 32.0f + 14.0f);
        drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
                 ImVec2(recX, recY + (ctl::geom::TOGGLE_H - recSz.y) * 0.5f), t.mute, recLabel);
        bool rec = snap.recursive;
        if (ctl::toggleSwitch(dl, ImGui::GetID("##empty_recursive"),
                              ImVec2(recX + recSz.x + 10.0f, recY), rec)) {
            app.setRecursive(rec);
        }
    } else {
        const float telemH = 40.0f;
        const ImVec2 viewMin = snapPt(stageMin.x + 1.0f, stageMin.y + 1.0f);
        const ImVec2 viewMax = snapPt(stageMax.x - 1.0f, stageMax.y - telemH);
        const float viewW = std::max(1.0f, viewMax.x - viewMin.x);
        const float viewH = std::max(1.0f, viewMax.y - viewMin.y);

        const bool isCompare = (snap.job.phase == upix::gui::JobPhase::Done);

        // Compute image natural aspect and fit scale
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
        const float fitScale = std::min(viewW / std::max(1.0f, refW), viewH / std::max(1.0f, refH));

        // Wheel zoom around cursor & pan (middle mouse or Space+Left drag)
        const bool mouseInView = ctl::mouseIn(viewMin, viewMax);
        if (!modal && mouseInView && std::fabs(io.MouseWheel) > 0.001f) {
            const float oldZoom = ui.fitMode ? 1.0f : ui.zoom;
            const float oldEff = fitScale * oldZoom;
            const float nextZoom = std::clamp(oldZoom * std::pow(1.15f, io.MouseWheel), 0.25f, 8.0f);
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

        const float effZoom = ui.fitMode ? 1.0f : std::clamp(ui.zoom, 0.25f, 8.0f);
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

        dl->PushClipRect(viewMin, viewMax, true);

        if (isCompare) {
            const float splitX = snapPx(viewMin.x + viewW * std::clamp(ui.splitRatio, 0.03f, 0.97f));

            dl->PushClipRect(viewMin, ImVec2(splitX, viewMax.y), true);
            drawCheckerboard(dl, img0, img1, 10.0f);
            drawStageArtwork(dl, img0, img1, false, ui);
            dl->PopClipRect();

            dl->PushClipRect(ImVec2(splitX, viewMin.y), viewMax, true);
            drawCheckerboard(dl, img0, img1, 10.0f);
            drawStageArtwork(dl, img0, img1, true, ui);
            dl->PopClipRect();

            // Crisp 1px vertical split line & 28x28 `< >` drag handle
            dl->AddRectFilled(ImVec2(splitX, viewMin.y), ImVec2(splitX + hairlinePx(), viewMax.y), t.ink);
            const ImVec2 hC = snapPt(splitX, (viewMin.y + viewMax.y) * 0.5f);
            const ImVec2 h0(hC.x - 14.0f, hC.y - 14.0f);
            const ImVec2 h1(hC.x + 14.0f, hC.y + 14.0f);
            dl->AddRectFilled(h0, h1, t.hudBg, geist::radius::MD);
            dl->AddRect(h0, h1, t.hudBorder, geist::radius::MD, 0, hairlinePx());
            ImVec2 cL[3] = {ImVec2(hC.x - 3.0f, hC.y - 3.5f), ImVec2(hC.x - 6.5f, hC.y), ImVec2(hC.x - 3.0f, hC.y + 3.5f)};
            ImVec2 cR[3] = {ImVec2(hC.x + 3.0f, hC.y - 3.5f), ImVec2(hC.x + 6.5f, hC.y), ImVec2(hC.x + 3.0f, hC.y + 3.5f)};
            dl->AddPolyline(cL, 3, t.mute, ImDrawFlags_None, 1.4f);
            dl->AddPolyline(cR, 3, t.mute, ImDrawFlags_None, 1.4f);
        } else {
            drawCheckerboard(dl, img0, img1, 10.0f);
            drawStageArtwork(dl, img0, img1, false, ui);
            if (isRunning && !motion::reduceMotion()) {
                const float frac = std::clamp(snap.job.percent / 100.0f, 0.04f, 0.96f);
                const float scanY = snapPx(img0.y + (img1.y - img0.y) * frac);
                dl->AddRectFilled(ImVec2(img0.x, scanY - 6.0f), ImVec2(img1.x, scanY),
                                  withAlpha(t.accent, 0.14f));
                dl->AddRectFilled(ImVec2(img0.x, scanY), ImVec2(img1.x, scanY + std::max(1.5f, hairlinePx())),
                                  t.accent);
            }
        }

        // Floating HUD pills (hit-tested before split drag so pills always win)
        const float closeY = snapPx(viewMin.y + 14.0f);
        const float hudY = snapPx(closeY + 1.0f); // center 26px pills on 28px close button midline
        if (isCompare) {
            const int inW = snap.job.in_w > 0 ? snap.job.in_w : (activeItem ? activeItem->width : 0);
            const int inH = snap.job.in_h > 0 ? snap.job.in_h : (activeItem ? activeItem->height : 0);
            const std::string beforeLabel = (inW > 0 && inH > 0)
                ? std::format("原图 · 1× · {}×{}", inW, inH)
                : std::string("原图 · 1×");
            const float bw = calcText(ui.fontMono, geist::type::LABEL, beforeLabel.c_str()).x + 18.0f;
            drawHudPill(dl, ui.fontMono, ImVec2(viewMin.x + 14.0f, hudY), bw, beforeLabel.c_str(), t.mute);
        } else if (activeItem && activeItem->width > 0 && activeItem->height > 0) {
            const std::string prevLabel = std::format("{}×{}", activeItem->width, activeItem->height);
            const float pw = calcText(ui.fontMono, geist::type::LABEL, prevLabel.c_str()).x + 18.0f;
            drawHudPill(dl, ui.fontMono, ImVec2(viewMin.x + 14.0f, hudY), pw, prevLabel.c_str(), t.mute);
        }

        float rx = viewMax.x - 14.0f;
        const ImVec2 close0 = snapPt(rx - 28.0f, closeY);
        if (hudIconButton(dl, ImGui::GetID("##stage_close"), close0, 28.0f, "x", "移除当前图片", ui.fontSans, !isRunning)) {
            if (snap.inputs.size() <= 1) {
                app.clearInputs();
            } else if (activeItem) {
                app.removeInput(activeItem->path);
            }
        }
        rx = close0.x - 8.0f;

        if (isCompare) {
            const int scale = modelScaleFor(snap);
            const std::string afterLabel = std::format("超分 · {}×", scale);
            const float aw = calcText(ui.fontMono, geist::type::LABEL, afterLabel.c_str()).x + 18.0f;
            rx -= aw;
            drawHudPill(dl, ui.fontMono, ImVec2(rx, hudY), aw, afterLabel.c_str(), t.ink);
            rx -= 8.0f;
        }

        const float oneToOneZoom = std::clamp(1.0f / std::max(0.05f, fitScale), 0.25f, 8.0f);
        const bool isOneToOne = !ui.fitMode && std::fabs(effZoom - oneToOneZoom) < 0.02f;
        const float btn11W = calcText(ui.fontMono, geist::type::LABEL, "1:1").x + 16.0f;
        rx -= btn11W;
        if (hudPillButton(dl, ImGui::GetID("##hud_1x1"), ui.fontMono, ImVec2(rx, hudY), btn11W, "1:1", isOneToOne)) {
            ui.fitMode = false;
            ui.zoom = oneToOneZoom;
            ui.pan = ImVec2(0.0f, 0.0f);
        }
        rx -= 6.0f;

        const float btnFitW = calcText(ui.fontSans, geist::type::LABEL, "适应").x + 16.0f;
        rx -= btnFitW;
        if (hudPillButton(dl, ImGui::GetID("##hud_fit"), ui.fontSans, ImVec2(rx, hudY), btnFitW, "适应", ui.fitMode)) {
            ui.fitMode = true;
            ui.zoom = 1.0f;
            ui.pan = ImVec2(0.0f, 0.0f);
        }

        if (!ui.fitMode) {
            const std::string zStr = std::format("{:.0f}%", effScale * 100.0f);
            const float zw = calcText(ui.fontMono, geist::type::LABEL, zStr.c_str()).x + 16.0f;
            rx -= (zw + 6.0f);
            drawHudPill(dl, ui.fontMono, ImVec2(rx, hudY), zw, zStr.c_str(), t.body);
        }

        // Sticky split drag tracking below top HUD strip
        if (isCompare && !modal) {
            const float splitX = snapPx(viewMin.x + viewW * std::clamp(ui.splitRatio, 0.03f, 0.97f));
            const float dragTop = viewMin.y + 46.0f;
            ImGui::SetCursorScreenPos(ImVec2(splitX - 16.0f, dragTop));
            ImGui::InvisibleButton("##split_drag", ImVec2(32.0f, std::max(1.0f, viewMax.y - dragTop)));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) {
                ui.splitRatio = std::clamp((io.MousePos.x - viewMin.x) / viewW, 0.03f, 0.97f);
            }
        }

        dl->PopClipRect();

        // ----------------------------------------------------- telemetry bar
        const ImVec2 bar0 = snapPt(stageMin.x + 1.0f, stageMax.y - telemH);
        const ImVec2 bar1 = snapPt(stageMax.x - 1.0f, stageMax.y - 1.0f);
        dl->AddRectFilled(bar0, bar1, t.surface, 11.0f, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(bar0.x, bar0.y), ImVec2(bar1.x, bar0.y + hairlinePx()), t.outline);
        const float cy = snapPx((bar0.y + bar1.y) * 0.5f);

        if (isRunning) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.accent);

            const std::string pctStr = std::format("{}%", snap.job.percent);
            const ImVec2 psz = calcText(ui.fontMono, geist::type::LABEL, pctStr.c_str());
            const float pctX = bar1.x - 16.0f - psz.x;
            drawText(dl, ui.fontMono, geist::type::LABEL, ImVec2(pctX, cy - psz.y * 0.5f), t.ink, pctStr.c_str());

            const float trkW = std::min(148.0f, std::max(64.0f, (bar1.x - bar0.x) * 0.25f));
            const float trkX = snapPx(pctX - 10.0f - trkW);
            const float frac = std::clamp(snap.job.percent / 100.0f, 0.0f, 1.0f);
            ctl::progressBar(dl, ImGui::GetID("##job_progress"), ImVec2(trkX, cy - ctl::geom::PROGRESS_H * 0.5f),
                             trkW, frac);

            const float textX = bar0.x + 28.0f;
            const float maxLeft = trkX - 12.0f;
            std::string tileMeta;
            if (snap.job.tiles_total > 0) {
                tileMeta = std::format("分块 {}/{}", snap.job.tiles_done, snap.job.tiles_total);
            }
            if (snap.job.eta_sec > 0) {
                if (!tileMeta.empty()) tileMeta += " · ";
                tileMeta += std::format("剩余 ~{}s", snap.job.eta_sec);
            }
            const ImVec2 fsz = calcText(ui.fontMono, geist::type::MONO, snap.job.current_file.c_str());
            const float fileMaxW = std::max(40.0f, maxLeft - textX - (tileMeta.empty() ? 0.0f : 90.0f));
            const std::string fittedFile = fitTextEllipsis(ui.fontMono, geist::type::MONO, snap.job.current_file.c_str(), fileMaxW);
            const ImVec2 fittedSz = calcText(ui.fontMono, geist::type::MONO, fittedFile.c_str());
            drawText(dl, ui.fontMono, geist::type::MONO, ImVec2(textX, cy - fsz.y * 0.5f), t.ink, fittedFile.c_str());

            if (!tileMeta.empty()) {
                const float metaX = textX + fittedSz.x + 10.0f;
                if (metaX + 24.0f < maxLeft) {
                    const ImVec2 msz = calcText(ui.fontMono, geist::type::LABEL, tileMeta.c_str());
                    ellipsisText(dl, ui.fontMono, geist::type::LABEL, ImVec2(metaX, cy - msz.y * 0.5f),
                                 maxLeft - metaX, t.mute, tileMeta.c_str());
                }
            }
        } else if (snap.job.phase == upix::gui::JobPhase::Done) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.accent);

            const std::string outPath = (activeItem && !activeItem->output_path.empty())
                ? activeItem->output_path
                : (snap.job.outputs.empty() ? std::string{} : snap.job.outputs.back());

            const int scale = modelScaleFor(snap);
            const std::string fmtLbl = formatDisplayLabel(snap, activeItem, outPath);
            const std::string badge = std::format("{}× · {}", scale, fmtLbl);

            // High-contrast badge (#68B5FF / #E6F4FF text on #0D253F fill with #1E4F8A hairline in dark mode)
            const ImVec2 bsz = calcText(ui.fontMono, 10.5f, badge.c_str());
            const ImVec2 b0 = snapPt(bar0.x + 28.0f, cy - 10.0f);
            const ImVec2 b1 = snapPt(b0.x + bsz.x + 14.0f, cy + 10.0f);
            const ImU32 badgeFill = dark ? hex(0x0D253F) : t.accentSoft;
            const ImU32 badgeBorder = dark ? hex(0x1E4F8A) : t.accentBorder;
            const ImU32 badgeFg = dark ? hex(0xE6F4FF) : t.accent;
            dl->AddRectFilled(b0, b1, badgeFill, geist::radius::SM);
            dl->AddRect(b0, b1, badgeBorder, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.5f, b0, b1, badgeFg, badge.c_str());

            // Right interactive buttons laid out first so middle telemetry never overlaps
            const float btnY = snapPx(cy - ctl::geom::GHOST_H * 0.5f);
            const float openW = calcText(ui.fontSans, ctl::geom::LABEL, "打开图像").x + 2.0f * ctl::geom::GHOST_PX;
            const float openX = snapPx(bar1.x - 14.0f - openW);
            if (ctl::chipButton(dl, ImGui::GetID("##btn_open_img"), "打开图像", ui.fontSans,
                                ImVec2(openX, btnY), openW, !outPath.empty())) {
                openFileInSystem(outPath);
            }

            const float revealW = calcText(ui.fontSans, ctl::geom::LABEL, "定位文件").x + 2.0f * ctl::geom::GHOST_PX;
            const float revealX = snapPx(openX - 8.0f - revealW);
            if (ctl::ghostButton(dl, ImGui::GetID("##btn_reveal_file"), "定位文件", ui.fontSans,
                                 ImVec2(revealX, btnY), revealW, !outPath.empty())) {
                revealFileParentInSystem(outPath);
            }

            const float maxMidRight = revealX - 12.0f;
            float cxText = b1.x + 10.0f;

            const std::string resStr = std::format("{}×{} → {}×{}", snap.job.in_w, snap.job.in_h,
                                                   snap.job.out_w, snap.job.out_h);
            const ImVec2 rsz = calcText(ui.fontMono, geist::type::MONO, resStr.c_str());
            if (cxText + 20.0f < maxMidRight) {
                const std::string fittedRes = fitTextEllipsis(ui.fontMono, geist::type::MONO, resStr.c_str(),
                                                              maxMidRight - cxText);
                drawText(dl, ui.fontMono, geist::type::MONO, ImVec2(cxText, cy - rsz.y * 0.5f),
                         t.ink, fittedRes.c_str());
                cxText += calcText(ui.fontMono, geist::type::MONO, fittedRes.c_str()).x + 10.0f;
            }

            if (cxText + 24.0f < maxMidRight) {
                const std::string metaStr = std::format("{:.2f}s · {} · {}", snap.job.elapsed_sec,
                                                        snap.job.out_size_str, snap.model);
                const ImVec2 msz = calcText(ui.fontMono, geist::type::LABEL, metaStr.c_str());
                ellipsisText(dl, ui.fontMono, geist::type::LABEL, ImVec2(cxText, cy - msz.y * 0.5f),
                             maxMidRight - cxText, t.mute, metaStr.c_str());
            }
        } else if (snap.job.phase == upix::gui::JobPhase::Error) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.error);

            const ImVec2 e0 = snapPt(bar0.x + 28.0f, cy - 10.0f);
            const ImVec2 e1 = snapPt(e0.x + 36.0f, cy + 10.0f);
            dl->AddRectFilled(e0, e1, t.errorSoft, geist::radius::SM);
            dl->AddRect(e0, e1, t.errorBorder, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.5f, e0, e1, t.error, "ERR");

            const bool missingModel = !snap.job.missing_model.empty();
            const bool noModel = snap.model.empty();
            const char* actionLabel = missingModel ? "下载模型" : (noModel ? "选择模型" : "重试");
            const float actW = calcText(ui.fontSans, ctl::geom::LABEL, actionLabel).x + 2.0f * ctl::geom::GHOST_PX;
            const float actX = snapPx(bar1.x - 14.0f - actW);
            const float actY = snapPx(cy - ctl::geom::GHOST_H * 0.5f);
            if (ctl::chipButton(dl, ImGui::GetID("##err_action"), actionLabel, ui.fontSans,
                                ImVec2(actX, actY), actW)) {
                if (missingModel) {
                    app.downloadModel(snap.job.missing_model);
                    ui.activeTab = 1;
                } else if (noModel) {
                    ui.activeTab = 1;
                } else {
                    app.startJob();
                }
            }

            const float msgX = e1.x + 10.0f;
            const float maxErrRight = actX - 12.0f;
            if (msgX + 16.0f < maxErrRight) {
                const ImVec2 msz = calcText(ui.fontSans, geist::type::BODY_SMALL, snap.job.message.c_str());
                const std::string fittedMsg = fitTextEllipsis(ui.fontSans, geist::type::BODY_SMALL,
                                                              snap.job.message.c_str(), maxErrRight - msgX);
                drawText(dl, ui.fontSans, geist::type::BODY_SMALL, ImVec2(msgX, cy - msz.y * 0.5f),
                         t.ink, fittedMsg.c_str());
                const float diagX = msgX + calcText(ui.fontSans, geist::type::BODY_SMALL, fittedMsg.c_str()).x + 10.0f;
                if (!snap.job.diag_detail.empty() && diagX + 24.0f < maxErrRight) {
                    const ImVec2 dsz = calcText(ui.fontMono, geist::type::LABEL, snap.job.diag_detail.c_str());
                    ellipsisText(dl, ui.fontMono, geist::type::LABEL, ImVec2(diagX, cy - dsz.y * 0.5f),
                                 maxErrRight - diagX, t.mute, snap.job.diag_detail.c_str());
                }
            }
        } else {
            const char* prefix = (snap.job.phase == upix::gui::JobPhase::Cancelled) ? "已取消" : "已就绪";
            const std::string ready = std::format("{} · {} 项输入待处理", prefix, snap.inputs.size());
            const ImVec2 rsz = calcText(ui.fontSans, geist::type::BODY_SMALL, ready.c_str());
            ellipsisText(dl, ui.fontSans, geist::type::BODY_SMALL,
                         ImVec2(bar0.x + 16.0f, cy - rsz.y * 0.5f),
                         std::max(40.0f, (bar1.x - bar0.x) - 32.0f), t.mute, ready.c_str());
        }
    }

    // -------------------------------------------------------- filmstrip dock
    if (hasDock) {
        const ImVec2 dock0 = snapPt(stageMin.x, stageMax.y + 10.0f);
        const ImVec2 dock1 = snapPt(stageMax.x, stageMax.y + 10.0f + 54.0f);
        ctl::groupCard(dl, dock0, dock1);

        size_t doneCnt = 0;
        for (const auto& it : snap.inputs) {
            if (it.status_kind == 2) ++doneCnt;
        }

        const ImVec2 add0 = snapPt(dock1.x - 8.0f - 28.0f, dock0.y + 13.0f);
        if (ctl::iconAction(dl, ImGui::GetID("##dock_add"), add0, 28.0f, "plus", "添加图片",
                            ui.fontSans, 13.0f, geist::radius::MD, !isRunning)) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }

        const ImVec2 trash0 = snapPt(add0.x - 6.0f - 28.0f, dock0.y + 13.0f);
        if (ctl::iconAction(dl, ImGui::GetID("##dock_clear"), trash0, 28.0f, "trash", "清空队列",
                            ui.fontSans, 13.0f, geist::radius::MD, !isRunning)) {
            app.clearInputs();
        }

        const float divX = snapPx(trash0.x - 10.0f);
        ctl::vHairline(dl, divX, dock0.y + 18.0f, dock0.y + 36.0f);

        const std::string cntText = std::format("{} / {}", doneCnt, snap.inputs.size());
        const ImVec2 cntSz = calcText(ui.fontMono, geist::type::LABEL, cntText.c_str());
        const float cntX = snapPx(divX - 10.0f - cntSz.x);
        drawText(dl, ui.fontMono, geist::type::LABEL,
                 ImVec2(cntX, dock0.y + (54.0f - cntSz.y) * 0.5f), t.mute, cntText.c_str());

        const ImVec2 stripMin = snapPt(dock0.x + 8.0f, dock0.y);
        const ImVec2 stripMax = snapPt(cntX - 10.0f, dock1.y);
        const float stripW = std::max(0.0f, stripMax.x - stripMin.x);
        const float totalThumbsW = static_cast<float>(snap.inputs.size()) * 46.0f - (snap.inputs.empty() ? 0.0f : 6.0f);
        const float maxDockScroll = std::max(0.0f, totalThumbsW - stripW);
        if (!modal && ctl::mouseIn(stripMin, stripMax) && std::fabs(io.MouseWheel) > 0.001f && maxDockScroll > 0.0f) {
            dockScroll -= io.MouseWheel * 46.0f;
        }
        dockScroll = std::clamp(dockScroll, 0.0f, maxDockScroll);

        dl->PushClipRect(stripMin, stripMax, true);
        float cx = stripMin.x - dockScroll;
        const float ty = dock0.y + 7.0f;
        for (size_t i = 0; i < snap.inputs.size(); ++i) {
            const auto& item = snap.inputs[i];
            const ImVec2 th0 = snapPt(cx, ty);
            const ImVec2 th1 = snapPt(cx + 40.0f, ty + 40.0f);
            cx += 46.0f;
            if (th1.x < stripMin.x || th0.x > stripMax.x) continue;

            const bool isActive = (static_cast<int>(i) == snap.active_input);
            bool hov = false;
            bool clicked = false;
            if (!modal) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetCursorScreenPos(th0);
                clicked = ImGui::InvisibleButton("##film_thumb", ImVec2(40.0f, 40.0f));
                hov = ImGui::IsItemHovered();
                const bool focused = ImGui::IsItemFocused();
                if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ctl::tooltip(dl, ImGui::GetID("##film_tip"), th0, th1, item.name.c_str(),
                             ui.fontSans, hov, focused);
                ImGui::PopID();
            }

            drawMiniThumb(dl, th0, th1, ui, geist::radius::MD, i, item.path);
            if (item.status_kind == 0) dl->AddRectFilled(th0, th1, hex(0x000000, 64), geist::radius::MD);
            if (item.status_kind == 3) dl->AddRectFilled(th0, th1, t.errorSoft, geist::radius::MD);
            if (hov) ctl::hoverWash(dl, th0, th1, geist::radius::MD, true, 0.10f);

            if (isActive) {
                dl->AddRect(th0, th1, t.accent, geist::radius::MD, 0, 1.5f);
            } else {
                ImU32 stroke = t.outline;
                if (item.status_kind == 1 || item.status_kind == 2) stroke = t.accentBorder;
                else if (item.status_kind == 3) stroke = t.error;
                dl->AddRect(th0, th1, stroke, geist::radius::MD, 0, hairlinePx());
            }

            // Per-item status indicator
            if (item.status_kind == 1) {
                const float progW = std::clamp(snap.job.percent / 100.0f, 0.08f, 1.0f) * 32.0f;
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 4.0f),
                                  snapPt(th0.x + 4.0f + progW, th1.y - 2.0f), t.accent, 1.0f);
            } else if (item.status_kind == 2) {
                const ImVec2 b0 = snapPt(th1.x - 13.0f, th0.y + 3.0f);
                const ImVec2 b1 = snapPt(th1.x - 3.0f, th0.y + 13.0f);
                dl->AddRectFilled(b0, b1, t.accent, 3.0f);
                drawIcon(dl, "check", ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f),
                         8.0f, IM_COL32_WHITE, 1.6f);
            } else if (item.status_kind == 3) {
                const ImVec2 b0 = snapPt(th1.x - 13.0f, th0.y + 3.0f);
                const ImVec2 b1 = snapPt(th1.x - 3.0f, th0.y + 13.0f);
                dl->AddRectFilled(b0, b1, t.error, 3.0f);
                drawIcon(dl, "x", ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f),
                         7.0f, IM_COL32_WHITE, 1.6f);
            }

            if (clicked && !isActive) {
                app.setActiveInput(static_cast<int>(i));
            }
        }
        dl->PopClipRect();
    }

    // ------------------------------------------------------------- inspector
    const float inspX = winSize.x - inspCol;
    const ImVec2 card0 = snapPt(inspX + geist::space::SM, geist::space::LG);
    const ImVec2 card1 = snapPt(winSize.x - geist::space::LG, winSize.y - geist::space::LG);
    ctl::groupCard(dl, card0, card1);

    const float ix = card0.x + geist::space::LG;
    const float iw = std::max(80.0f, (card1.x - card0.x) - geist::space::LG * 2.0f);
    const float seam = snapPx(card1.y - geist::space::LG - ctl::geom::CTA_H - geist::space::MD);
    float y = card0.y + geist::space::LG;

    auto sectionLabel = [&](const char* s) {
        ctl::sectionHeader(dl, ui.fontSans, snapPt(ix, y), s);
        y += calcText(ui.fontSans, geist::type::BODY_SMALL, s).y + geist::space::XS;
    };

    // Look up active model metadata
    std::string displayMod = snap.model.empty() ? "选择模型" : snap.model;
    std::string modScene;
    int modScale = 4;
    bool modRec = false;
    bool modInstalled = false;
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) {
            modScene = m.scene;
            modScale = std::max(1, m.scale);
            modRec = m.recommended;
            modInstalled = true;
            break;
        }
    }

    dl->PushClipRect(ImVec2(card0.x + 1.0f, card0.y + 1.0f), ImVec2(card1.x - 1.0f, seam), true);

    // ── Section 1: 模型 ──
    sectionLabel("模型");
    const ImVec2 mp0 = snapPt(ix, y);
    const ImVec2 mp1 = snapPt(ix + iw, y + 36.0f);
    {
        // Restore real mouse pos strictly for the trigger button so clicking it
        // toggles the picker closed even while modal is open.
        if (modal) io.MousePos = savedMousePos;
        ImGui::SetCursorScreenPos(mp0);
        const bool trigClicked = ImGui::InvisibleButton("##model_picker_btn", ImVec2(iw, 36.0f));
        const bool trigHov = ImGui::IsItemHovered();
        const bool trigFocused = ImGui::IsItemFocused();
        if (trigHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (modal) io.MousePos = ImVec2(-1.0e6f, -1.0e6f);

        ctl::finish(dl, mp0, mp1, trigHov ? ctl::Finish::RaisedHover : ctl::Finish::Raised, 8.0f);
        dl->AddRect(mp0, mp1, ui.showModelPicker ? t.accent : (trigHov ? t.outlineInput : t.outlineInput),
                    8.0f, 0, hairlinePx());
        if (trigFocused) {
            dl->AddRect(mp0, mp1, t.accent, 8.0f, 0, hairlinePx());
        }

        const bool showScaleBadge = modInstalled && !nameHasScale(displayMod, modScale);
        const float badgeW = showScaleBadge
            ? (calcText(ui.fontMono, 10.5f, std::format("{}×", modScale).c_str()).x + 12.0f + 6.0f)
            : 0.0f;
        const float rightClusterW = 28.0f + 12.0f + badgeW; // chevron + scene dot + optional scale badge
        const float nameMaxX = mp1.x - rightClusterW - (modRec ? 18.0f : 4.0f);

        const std::string fittedMod = fitTextEllipsis(ui.fontMono, geist::type::BODY, displayMod.c_str(),
                                                      std::max(40.0f, nameMaxX - (mp0.x + 12.0f)));
        const ImVec2 msz = calcText(ui.fontMono, geist::type::BODY, fittedMod.c_str());
        drawText(dl, ui.fontMono, geist::type::BODY,
                 ImVec2(mp0.x + 12.0f, mp0.y + (36.0f - msz.y) * 0.5f),
                 (modInstalled || snap.model.empty()) ? t.ink : t.mute, fittedMod.c_str());
        if (modRec) {
            drawIcon(dl, "star", ImVec2(mp0.x + 12.0f + msz.x + 9.0f, mp0.y + 18.0f), 11.0f, t.warning);
        }

        if (showScaleBadge) {
            const std::string scStr = std::format("{}×", modScale);
            const float bw = calcText(ui.fontMono, 10.5f, scStr.c_str()).x + 10.0f;
            const ImVec2 b0 = snapPt(mp1.x - 40.0f - bw, mp0.y + 9.0f);
            const ImVec2 b1 = snapPt(mp1.x - 40.0f, mp0.y + 27.0f);
            dl->AddRectFilled(b0, b1, t.container, geist::radius::SM);
            dl->AddRect(b0, b1, t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.5f, b0, b1, t.mute, scStr.c_str());
        }

        dl->AddCircleFilled(ImVec2(mp1.x - 32.0f, mp0.y + 18.0f), 3.0f,
                            ctl::sceneDotColor(modScene.c_str()));
        drawIcon(dl, ui.showModelPicker ? "chevron-up" : "chevron-down",
                 ImVec2(mp1.x - 16.0f, mp0.y + 18.0f), 14.0f, t.mute);

        if (trigClicked) {
            ui.showModelPicker = !ui.showModelPicker;
            if (ui.showModelPicker) {
                pickerSearch[0] = '\0';
                pickerScroll = 0.0f;
            }
        }
    }
    y += 36.0f;

    // ── Section 2: 输出 ──
    y += geist::space::SECTION;
    sectionLabel("输出");

    const char* fmtLabels[5] = {"跟随", "PNG", "JPG", "WebP", "AVIF"};
    const char* fmtVals[5] = {"", "png", "jpg", "webp", "avif"};
    int fmtIdx = 0;
    if (!snap.overwrite_original) {
        for (int i = 0; i < 5; ++i) {
            if (snap.format == fmtVals[i]) {
                fmtIdx = i;
                break;
            }
        }
    }
    const bool fmtChanged = snap.overwrite_original
        ? ctl::segmentedLocked(dl, ImGui::GetID("##fmt"), ImVec2(ix, y), ImVec2(iw, ctl::geom::SEG_H),
                               fmtLabels, 5, fmtIdx, ui.fontSans, 12.0f)
        : ctl::segmented(dl, ImGui::GetID("##fmt"), ImVec2(ix, y), ImVec2(iw, ctl::geom::SEG_H),
                         fmtLabels, 5, fmtIdx, ui.fontSans, 12.0f, false);
    if (fmtChanged && !snap.overwrite_original) {
        app.setFormat(fmtVals[fmtIdx]);
    }
    y += ctl::geom::SEG_H;

    const std::string effectiveFmt = snap.overwrite_original ? std::string{} : snap.format;
    const bool isPng = (effectiveFmt == "png");
    const bool isJpg = (effectiveFmt == "jpg");
    const bool isAvif = (effectiveFmt == "avif");

    if (!isPng) {
        y += geist::space::SM;
        const ImVec2 qlSz = calcText(ui.fontSans, geist::type::BODY_SMALL, "质量");
        drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
                 snapPt(ix, y + (22.0f - qlSz.y) * 0.5f), t.body, "质量");
        int q = snap.quality;
        const ImVec2 qiPos = snapPt(ix + iw - 52.0f, y);
        if (ctl::qualityInput(dl, ImGui::GetID("##q_input"), qiPos, q, 1, 100, ui.fontMono)) {
            app.setQuality(q);
        }
        // Avoid spurious frame-1 NavInit blue focus ring before user clicks or tabs in.
        if (!ImGui::IsItemActive() && !io.NavVisible) {
            dl->AddRect(qiPos, snapPt(qiPos.x + 52.0f, qiPos.y + 22.0f), t.outlineInput, 5.0f, 0, hairlinePx());
        }
        y += 22.0f + geist::space::XS;
        if (ctl::qualitySlider(dl, ImGui::GetID("##q_slider"), ImVec2(ix, y), iw, q, 1, 100)) {
            app.setQuality(q);
        }
        y += ctl::geom::SLIDER_H;
    } else {
        y += geist::space::SM;
        const char* pngNote = "PNG 无损压缩，完整保留透明通道";
        const ImVec2 nsz = calcText(ui.fontSans, geist::type::LABEL, pngNote);
        drawText(dl, ui.fontSans, geist::type::LABEL, snapPt(ix, y), t.mute, pngNote);
        y += nsz.y;
    }

    if (isJpg || isAvif) {
        y += geist::space::SM;
        const ImVec2 clSz = calcText(ui.fontSans, geist::type::BODY_SMALL, "色度抽样");
        drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
                 snapPt(ix, y + (28.0f - clSz.y) * 0.5f), t.body, "色度抽样");
        const char* chromaLabels[2] = {"4:4:4", "4:2:0"};
        const int chromaVals[2] = {444, 420};
        int chromaIdx = (snap.chroma == 444) ? 0 : 1;
        if (ctl::segmented(dl, ImGui::GetID("##chroma"), ImVec2(ix + iw - 148.0f, y),
                           ImVec2(148.0f, 28.0f), chromaLabels, 2, chromaIdx, ui.fontMono, 12.0f)) {
            app.setChroma(chromaVals[chromaIdx]);
        }
        y += 28.0f;
    }

    if (isAvif) {
        y += geist::space::SM;
        const ImVec2 slSz = calcText(ui.fontSans, geist::type::BODY_SMALL, "编码速度");
        drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
                 snapPt(ix, y + (28.0f - slSz.y) * 0.5f), t.body, "编码速度");
        const char* spdLabels[3] = {"精细", "均衡", "快速"};
        const int spdVals[3] = {4, 6, 8};
        int spdIdx = 1;
        for (int i = 0; i < 3; ++i) {
            if (snap.avif_speed == spdVals[i]) {
                spdIdx = i;
                break;
            }
        }
        if (ctl::segmented(dl, ImGui::GetID("##spd"), ImVec2(ix + iw - 164.0f, y),
                           ImVec2(164.0f, 28.0f), spdLabels, 3, spdIdx, ui.fontSans, 12.0f)) {
            app.setAvifSpeed(spdVals[spdIdx]);
        }
        y += 28.0f;
    }

    // 导出位置
    y += geist::space::SM;
    const bool dirLocked = snap.overwrite_original;
    const ImVec2 dirLblSz = calcText(ui.fontSans, geist::type::BODY_SMALL, "导出位置");
    drawText(dl, ui.fontSans, geist::type::BODY_SMALL, snapPt(ix, y), dirLocked ? t.faint : t.body, "导出位置");
    y += dirLblSz.y + geist::space::XS;

    const ImVec2 dir0 = snapPt(ix, y);
    const ImVec2 dir1 = snapPt(ix + iw, y + 34.0f);
    dl->AddRectFilled(dir0, dir1, t.wellBg, 8.0f);
    dl->AddRect(dir0, dir1, t.outline, 8.0f, 0, hairlinePx());
    drawIcon(dl, "folder", ImVec2(dir0.x + 15.0f, dir0.y + 17.0f), 13.0f, dirLocked ? t.faint : t.mute);

    const bool hasCustom = !snap.custom_out_dir.empty();
    const char* dirBtnLabel = hasCustom ? "更改" : "指定…";
    const float dirBtnW = calcText(ui.fontSans, ctl::geom::LABEL, dirBtnLabel).x + 2.0f * ctl::geom::GHOST_PX;
    const float dirBtnX = snapPx(dir1.x - 5.0f - dirBtnW);
    const float btnY = snapPx(dir0.y + 5.0f);
    if (ctl::chipButton(dl, ImGui::GetID("##pick_outdir"), dirBtnLabel, ui.fontSans,
                        ImVec2(dirBtnX, btnY), dirBtnW, !dirLocked)) {
        SDL_ShowOpenFolderDialog(onFileDialog, dlgOutDir, window, nullptr, false);
    }

    float textRightLimit = dirBtnX - 6.0f;
    if (hasCustom && !dirLocked) {
        const float resetW = calcText(ui.fontSans, ctl::geom::LABEL, "重置").x + 2.0f * ctl::geom::GHOST_PX;
        const float resetX = snapPx(dirBtnX - 4.0f - resetW);
        if (ctl::ghostButton(dl, ImGui::GetID("##reset_outdir"), "重置", ui.fontSans,
                             ImVec2(resetX, btnY), resetW, true)) {
            app.setCustomOutDir("");
        }
        textRightLimit = resetX - 6.0f;
    }

    std::string outLabel = "跟随原图目录";
    ImFont* outFont = ui.fontSans;
    if (dirLocked) {
        outLabel = "原位覆盖原图";
    } else if (hasCustom) {
        outLabel = snap.custom_out_dir;
        outFont = ui.fontMono;
    } else {
        bool hasBatchRoot = false;
        for (const auto& it : snap.inputs) {
            if (!it.batch_root.empty()) {
                hasBatchRoot = true;
                break;
            }
        }
        if (hasBatchRoot) {
            outLabel = std::format("upix_{}/（文件夹内）", snap.model.empty() ? "out" : snap.model);
        }
    }
    const ImVec2 osz = calcText(outFont, geist::type::BODY_SMALL, outLabel.c_str());
    ellipsisText(dl, outFont, geist::type::BODY_SMALL,
                 ImVec2(dir0.x + 28.0f, dir0.y + (34.0f - osz.y) * 0.5f),
                 std::max(24.0f, textRightLimit - (dir0.x + 28.0f)),
                 dirLocked ? t.faint : t.body, outLabel.c_str());
    y += 34.0f;

    // 覆盖原图 + conditional 备份原图
    auto drawSwitchRow = [&](const char* swId, const char* title, const char* sub, bool val, bool enabled) -> bool {
        y += geist::space::SM;
        const float textMaxW = std::max(60.0f, iw - ctl::geom::TOGGLE_W - 10.0f);
        const ImVec2 tsz = calcText(ui.fontSans, geist::type::BODY, title);
        const ImVec2 ssz = calcText(ui.fontSans, geist::type::LABEL, sub);
        const float rowH = std::max(ctl::geom::TOGGLE_H, tsz.y + 2.0f + ssz.y);
        const float textTop = y + (rowH - (tsz.y + 2.0f + ssz.y)) * 0.5f;
        ellipsisText(dl, ui.fontSans, geist::type::BODY, snapPt(ix, textTop), textMaxW,
                     enabled ? t.ink : t.mute, title);
        ellipsisText(dl, ui.fontSans, geist::type::LABEL, snapPt(ix, textTop + tsz.y + 2.0f), textMaxW,
                     t.mute, sub);
        bool next = val;
        const bool toggled = ctl::toggleSwitch(
            dl, ImGui::GetID(swId),
            snapPt(ix + iw - ctl::geom::TOGGLE_W, y + (rowH - ctl::geom::TOGGLE_H) * 0.5f),
            next, enabled);
        y += rowH;
        return toggled && next != val;
    };

    if (drawSwitchRow("##sw_overwrite", "覆盖原图", "结果写回输入文件，不再另存一份",
                      snap.overwrite_original, !isRunning)) {
        const bool nextOv = !snap.overwrite_original;
        app.setOverwriteOriginal(nextOv);
        if (nextOv) app.setBackupOriginal(true);
    }

    if (snap.overwrite_original) {
        if (drawSwitchRow("##sw_backup", "备份原图 (upix_orig/)", "覆盖前存到同目录的 upix_orig/",
                          snap.backup_original, !isRunning)) {
            app.setBackupOriginal(!snap.backup_original);
        }
    }

    dl->PopClipRect();

    // ── Bottom Sticky CTA ──
    ctl::hHairline(dl, card0.x, card1.x, seam);
    const ImVec2 cta0 = snapPt(ix, seam + geist::space::MD);
    const ImVec2 cta1 = snapPt(ix + iw, cta0.y + ctl::geom::CTA_H);

    if (isRunning) {
        const bool canCancel = (snap.job.phase == upix::gui::JobPhase::Running);
        if (ctl::secondaryButton(dl, ImGui::GetID("##cancel_job"),
                                 canCancel ? "取消超分" : "正在取消…", ui.fontSans,
                                 cta0, cta1, "stop", canCancel, true)) {
            app.cancelJob();
        }
    } else if (snap.job.phase == upix::gui::JobPhase::Error && !snap.job.missing_model.empty()) {
        if (ctl::primaryCta(dl, ImGui::GetID("##cta_dl_model"), "下载模型", ui.fontSans, cta0, cta1, true, "download")) {
            app.downloadModel(snap.job.missing_model);
            ui.activeTab = 1;
        }
    } else if (snap.inputs.empty()) {
        if (ctl::primaryCta(dl, ImGui::GetID("##cta_pick_imgs"), "选择图片", ui.fontSans, cta0, cta1, true, "plus")) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    } else {
        const char* label = "开始超分";
        if (snap.job.phase == upix::gui::JobPhase::Done) label = "重新超分";
        else if (snap.job.phase == upix::gui::JobPhase::Cancelled || snap.job.phase == upix::gui::JobPhase::Error) {
            label = "继续超分";
        }
        if (ctl::primaryCta(dl, ImGui::GetID("##cta_start"), label, ui.fontSans, cta0, cta1, true)) {
            app.startJob();
        }
    }

    // Restore real mouse pos before rendering the model picker popup
    if (modal) {
        io.MousePos = savedMousePos;
    }

    // -------------------------------------------------------- model picker popup
    if (ui.showModelPicker) {
        struct FilteredGroup {
            std::string label;
            std::vector<const upix::gui::ModelItem*> items;
        };
        const std::string query(pickerSearch);
        std::vector<FilteredGroup> groups;
        size_t totalHits = 0;

        auto collectFrom = [&](const std::string& grpLabel, const std::vector<upix::gui::ModelItem>& items) {
            FilteredGroup fg;
            fg.label = grpLabel;
            for (const auto& m : items) {
                if (containsInsensitive(m.name, query) ||
                    containsInsensitive(m.short_name, query) ||
                    containsInsensitive(m.scene, query) ||
                    containsInsensitive(grpLabel, query)) {
                    fg.items.push_back(&m);
                }
            }
            if (!fg.items.empty()) {
                totalHits += fg.items.size();
                groups.push_back(std::move(fg));
            }
        };

        if (!snap.installed_groups.empty()) {
            for (const auto& g : snap.installed_groups) {
                collectFrom(g.label, g.items);
            }
        } else if (!snap.installed.empty()) {
            collectFrom("已安装模型", snap.installed);
        }

        const float pad = 6.0f;
        const float searchH = 30.0f;
        const float rowH = 30.0f;
        const float rowGap = 2.0f;
        const float hdrH = 18.0f;
        const float grpGap = 8.0f;

        float listContentH = 0.0f;
        if (totalHits == 0) {
            listContentH = snap.installed.empty() ? 116.0f : 72.0f;
        } else {
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (gi > 0) listContentH += grpGap;
                const float n = static_cast<float>(groups[gi].items.size());
                listContentH += hdrH + 2.0f + n * rowH + std::max(0.0f, n - 1.0f) * rowGap;
            }
        }

        const float maxPanelH = 340.0f;
        const float panelH = std::min(maxPanelH, pad * 2.0f + searchH + 6.0f + listContentH);
        float py = mp1.y + 6.0f;
        if (py + panelH > winSize.y - 8.0f) {
            py = std::max(8.0f, mp0.y - 6.0f - panelH);
        }
        const ImVec2 pop0 = snapPt(mp0.x, py);
        const ImVec2 pop1 = snapPt(mp1.x, py + panelH);
        const float popW = pop1.x - pop0.x;

        // Draw floating Menu surface (NO dark scrim over inspector or CTA!)
        ctl::finish(dl, pop0, pop1, ctl::Finish::Menu, 10.0f);

        // Top functional searchBox
        ctl::searchBox(dl, ImGui::GetID("##picker_search"),
                       snapPt(pop0.x + pad, pop0.y + pad), popW - pad * 2.0f, searchH,
                       pickerSearch, sizeof(pickerSearch), "搜索已安装模型…", ui.fontSans);

        const ImVec2 list0 = snapPt(pop0.x + pad, pop0.y + pad + searchH + 6.0f);
        const ImVec2 list1 = snapPt(pop1.x - pad, pop1.y - pad);
        const float listVisH = std::max(1.0f, list1.y - list0.y);
        const float maxScroll = std::max(0.0f, listContentH - listVisH);

        if (ctl::mouseIn(list0, list1) && std::fabs(io.MouseWheel) > 0.001f && maxScroll > 0.0f) {
            pickerScroll -= io.MouseWheel * 28.0f;
        }
        pickerScroll = std::clamp(pickerScroll, 0.0f, maxScroll);

        if (totalHits == 0) {
            const bool noInstalled = snap.installed.empty();
            const char* emptyTitle = noInstalled ? "尚未安装模型" : "没有匹配的模型";
            const char* emptyHint = noInstalled ? "到「模型」页挑选并下载" : "换个关键词，或到「模型」页下载";
            const float blockTop = list0.y + (listVisH - (noInstalled ? 82.0f : 38.0f)) * 0.5f;
            drawTextCentered(dl, ui.fontSans, geist::type::BODY_SMALL,
                             ImVec2(list0.x, blockTop), ImVec2(list1.x, blockTop + 18.0f),
                             t.ink, emptyTitle);
            drawTextCentered(dl, ui.fontSans, geist::type::LABEL,
                             ImVec2(list0.x, blockTop + 20.0f), ImVec2(list1.x, blockTop + 36.0f),
                             t.mute, emptyHint);
            if (noInstalled) {
                const float btnW = std::min(148.0f, (list1.x - list0.x) - 24.0f);
                const float btnX = snapPx((list0.x + list1.x - btnW) * 0.5f);
                if (ctl::secondaryButton(dl, ImGui::GetID("##picker_open_models"), "打开模型页", ui.fontSans,
                                         ImVec2(btnX, blockTop + 46.0f), ImVec2(btnX + btnW, blockTop + 76.0f))) {
                    ui.showModelPicker = false;
                    ui.activeTab = 1;
                }
            }
        } else {
            dl->PushClipRect(list0, list1, true);
            float ry = list0.y - pickerScroll;
            for (size_t gi = 0; gi < groups.size(); ++gi) {
                if (gi > 0) ry += grpGap;
                const auto& g = groups[gi];
                const ImVec2 hsz = calcText(ui.fontSans, geist::type::LABEL, g.label.c_str());
                drawText(dl, ui.fontSans, geist::type::LABEL,
                         snapPt(list0.x + 8.0f, ry + (hdrH - hsz.y) * 0.5f), t.mute, g.label.c_str());
                ry += hdrH + 2.0f;

                for (size_t mi = 0; mi < g.items.size(); ++mi) {
                    const auto* m = g.items[mi];
                    const ImVec2 r0 = snapPt(list0.x, ry);
                    const ImVec2 r1 = snapPt(list1.x, ry + rowH);
                    ry += rowH + rowGap;

                    if (r1.y < list0.y || r0.y > list1.y) continue;

                    const bool isSel = (m->name == snap.model);
                    const bool hov = ctl::mouseIn(r0, r1);
                    if (isSel) {
                        dl->AddRectFilled(r0, r1, t.accentSoft, geist::radius::MD);
                    } else if (hov) {
                        dl->AddRectFilled(r0, r1, t.high, geist::radius::MD);
                    }
                    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

                    char sceneBuf[32];
                    const char* sLbl = ctl::sceneLabel(m->scene.c_str(), sceneBuf, sizeof(sceneBuf));
                    const std::string meta = std::format("{} · {}×", sLbl, m->scale);
                    const ImVec2 msz = calcText(ui.fontSans, 10.5f, meta.c_str());
                    const float dotX = r1.x - 10.0f - msz.x - 8.0f;

                    const char* rowTitle = m->short_name.empty() ? m->name.c_str() : m->short_name.c_str();
                    const float nameX = r0.x + 18.0f;
                    const float maxNameW = std::max(24.0f, dotX - 8.0f - nameX - (m->recommended ? 16.0f : 0.0f));
                    const std::string fittedName = fitTextEllipsis(ui.fontMono, 12.5f, rowTitle, maxNameW);
                    const ImVec2 nsz = calcText(ui.fontMono, 12.5f, fittedName.c_str());
                    drawText(dl, ui.fontMono, 12.5f,
                             ImVec2(nameX, r0.y + (rowH - nsz.y) * 0.5f),
                             isSel ? t.accent : t.ink, fittedName.c_str());
                    if (m->recommended) {
                        drawIcon(dl, "star", ImVec2(nameX + nsz.x + 8.0f, r0.y + rowH * 0.5f),
                                 10.0f, t.warning);
                    }

                    dl->AddCircleFilled(ImVec2(dotX, (r0.y + r1.y) * 0.5f), 2.5f,
                                        ctl::sceneDotColor(m->scene.c_str()));
                    drawText(dl, ui.fontSans, 10.5f,
                             ImVec2(r1.x - 10.0f - msz.x, r0.y + (rowH - msz.y) * 0.5f),
                             t.mute, meta.c_str());

                    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        app.setModel(m->name);
                        ui.showModelPicker = false;
                    }
                }
            }
            dl->PopClipRect();
        }

        // Dismiss on outside click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ctl::mouseIn(pop0, pop1) && !ctl::mouseIn(mp0, mp1)) {
            ui.showModelPicker = false;
        }
    }
}

} // namespace upix::views
