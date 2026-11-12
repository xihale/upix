#include "views/common.hpp"

#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace upix::views {

void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                            upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                            SDL_Window* window, DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float inspW = 308.0f;
    float inspX = winSize.x - inspW;
    float stageAreaX = railW;
    float stageAreaW = inspX - railW;

    bool hasInputs = !snap.inputs.empty() || snap.job.phase != upix::gui::JobPhase::Idle;
    bool hasDock = snap.inputs.size() > 1 || snap.job.phase == upix::gui::JobPhase::Running || snap.job.phase == upix::gui::JobPhase::Error;

    float padX = 8.0f;
    float padY = 16.0f;
    float dockBarH = 54.0f;
    float dockH = hasDock ? (dockBarH + 10.0f) : 0.0f;
    ImVec2 stageMin = snapPt(stageAreaX + padX, padY);
    ImVec2 stageMax = snapPt(stageAreaX + stageAreaW - padX, winSize.y - padY - dockH);

    // Stage Well container (Figma 4:18 & 71:1133: background #18181c, border 1px solid #282830, radius 12px)
    ImU32 stageBg = (snap.job.phase == upix::gui::JobPhase::Running) ? pal.recessed() : pal.surface();
    drawBox(dl, stageMin, stageMax, stageBg, pal.outline(), 12.0f);

    if (!hasInputs) {
        // ---- Empty State (Figma Frame 4:2) ----
        // Registration marks: inset 28px, 16x2px, #343440
        drawRegistrationMarks(dl, stageMin, stageMax, 28.0f, 16.0f, 2.0f, pal.regMark());

        ImVec2 center = snapPt((stageMin.x + stageMax.x) * 0.5f, (stageMin.y + stageMax.y) * 0.5f);

        // Figma 7:6: top 352px (center - 52px), 32x32 zoom-in icon, gap 14px, 15px weight 500 text #a2a2ac
        drawIcon(dl, "zoom-in", ImVec2(center.x, center.y - 36.0f), 32.0f, pal.muted(), 1.8f);
        drawTextCentered(dl, ui.fontSans, 15.0f,
                         ImVec2(stageMin.x, center.y - 6.0f), ImVec2(stageMax.x, center.y + 20.0f),
                         pal.muted(), "拖入图片，或按 Ctrl+V 粘贴");

        // Figma 12:366: 162x32px button at top:732px (44px above stage bottom), #1c1c21 fill, #2d2d34 border, radius 8px, 12px #a2a2ac
        ImVec2 folderBtnSz(162.0f, 32.0f);
        ImVec2 btnPos = snapPt(center.x - folderBtnSz.x * 0.5f, stageMax.y - 44.0f - folderBtnSz.y);
        if (pillButton(dl, "##pick_folder", btnPos, folderBtnSz, "选择文件夹批量处理", "folder-open", pal, ui.fontSans, false, 8.0f, 12.0f, true)) {
            SDL_ShowOpenFolderDialog(onFileDialog, dlgFolder, window, nullptr, false);
        }

        // Click on the rest of the stage opens file picker
        ImGui::SetCursorScreenPos(stageMin);
        if (ImGui::InvisibleButton("##stage_empty_click", ImVec2(stageMax.x - stageMin.x, btnPos.y - stageMin.y - 8.0f))) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    } else {
        // ---- Active Stage (Compare / Running / Error) ----
        float telemH = 40.0f;
        ImVec2 viewMin = snapPt(stageMin.x + 1.0f, stageMin.y + 1.0f);
        ImVec2 viewMax = snapPt(stageMax.x - 1.0f, stageMax.y - telemH);

        dl->PushClipRect(viewMin, viewMax, true);

        bool isCompare = (snap.job.phase == upix::gui::JobPhase::Done);
        if (isCompare) {
            float splitX = snapPx(viewMin.x + (viewMax.x - viewMin.x) * ui.splitRatio);

            // Left: Before (1x)
            dl->PushClipRect(viewMin, ImVec2(splitX, viewMax.y), true);
            drawStageArtwork(dl, viewMin, viewMax, false, ui);
            dl->PopClipRect();

            // Right: After (4x)
            dl->PushClipRect(ImVec2(splitX, viewMin.y), viewMax, true);
            drawStageArtwork(dl, viewMin, viewMax, true, ui);
            dl->PopClipRect();

            // 1px vertical split line (#ececeee5) + 28x28 rounded square handle (Figma Frame 71:1129)
            dl->AddRectFilled(ImVec2(splitX, viewMin.y), ImVec2(splitX + hairlinePx(), viewMax.y), pal.withAlpha(pal.onSurface(), 0.90f));
            ImVec2 hCenter = snapPt(splitX, (viewMin.y + viewMax.y) * 0.5f);
            ImVec2 h0(hCenter.x - 14.0f, hCenter.y - 14.0f);
            ImVec2 h1(hCenter.x + 14.0f, hCenter.y + 14.0f);
            drawBox(dl, h0, h1, rgba(20, 20, 24, 215), hex(0x2C2C35), 7.0f);
            ImVec2 cL[3] = {ImVec2(hCenter.x - 3.0f, hCenter.y - 3.5f), ImVec2(hCenter.x - 6.5f, hCenter.y), ImVec2(hCenter.x - 3.0f, hCenter.y + 3.5f)};
            ImVec2 cR[3] = {ImVec2(hCenter.x + 3.0f, hCenter.y - 3.5f), ImVec2(hCenter.x + 6.5f, hCenter.y), ImVec2(hCenter.x + 3.0f, hCenter.y + 3.5f)};
            dl->AddPolyline(cL, 3, pal.muted(), ImDrawFlags_None, 1.4f);
            dl->AddPolyline(cR, 3, pal.muted(), ImDrawFlags_None, 1.4f);

            ImGui::SetCursorScreenPos(ImVec2(splitX - 16.0f, viewMin.y));
            ImGui::InvisibleButton("##split_drag", ImVec2(32.0f, viewMax.y - viewMin.y));
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
            if (ImGui::IsItemActive()) {
                ui.splitRatio = std::clamp((ImGui::GetIO().MousePos.x - viewMin.x) / (viewMax.x - viewMin.x), 0.05f, 0.95f);
            }

            // Optical HUD chips (Figma 75:2993 & 75:2995: 77x26px, #141418c7, border #2c2c35, radius 6px, 11px JetBrains Mono)
            ImVec2 chipL0 = snapPt(viewMin.x + 14.0f, viewMin.y + 14.0f);
            drawBox(dl, chipL0, ImVec2(chipL0.x + 77.0f, chipL0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawTextCentered(dl, ui.fontMono, 11.0f, chipL0, ImVec2(chipL0.x + 77.0f, chipL0.y + 26.0f), pal.muted(), "原图 · 1×");

            ImVec2 close0 = snapPt(viewMax.x - 14.0f - 26.0f, viewMin.y + 14.0f);
            ImVec2 chipR0 = snapPt(close0.x - 10.0f - 77.0f, viewMin.y + 14.0f);
            drawBox(dl, chipR0, ImVec2(chipR0.x + 77.0f, chipR0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawTextCentered(dl, ui.fontMono, 11.0f, chipR0, ImVec2(chipR0.x + 77.0f, chipR0.y + 26.0f), pal.onSurface(), "超分 · 4×");

            ImGui::SetCursorScreenPos(close0);
            if (ImGui::InvisibleButton("##close_stage_done", ImVec2(26.0f, 26.0f))) {
                app.clearInputs();
            }
            drawBox(dl, close0, ImVec2(close0.x + 26.0f, close0.y + 26.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawIcon(dl, "x", ImVec2(close0.x + 13.0f, close0.y + 13.0f), 12.0f, pal.muted());
        } else {
            // Running / Idle / Error plate
            drawStageArtwork(dl, viewMin, viewMax, false, ui);

            if (snap.job.phase == upix::gui::JobPhase::Running) {
                float scanY = snapPx(viewMin.y + (viewMax.y - viewMin.y) * (snap.job.percent / 100.0f));
                dl->AddRectFilled(ImVec2(viewMin.x, scanY), ImVec2(viewMax.x, scanY + hairlinePx()), pal.withAlpha(pal.primary(), 0.55f));
            }

            ImVec2 close0 = snapPt(viewMax.x - 14.0f - 28.0f, viewMin.y + 14.0f);
            ImGui::SetCursorScreenPos(close0);
            if (ImGui::InvisibleButton("##close_stage_run", ImVec2(28.0f, 28.0f))) {
                app.clearInputs();
            }
            drawBox(dl, close0, ImVec2(close0.x + 28.0f, close0.y + 28.0f), rgba(20, 20, 24, 199), hex(0x2C2C35), 6.0f);
            drawIcon(dl, "x", ImVec2(close0.x + 14.0f, close0.y + 14.0f), 13.0f, pal.muted());
        }
        dl->PopClipRect();

        // ---- Bottom 40px Stage Telemetry Bar (Figma 83:3540 & 83:3528: #18181c, border #26262e) ----
        ImVec2 bar0 = snapPt(stageMin.x + 1.0f, stageMax.y - telemH);
        ImVec2 bar1 = snapPt(stageMax.x - 1.0f, stageMax.y - 1.0f);
        dl->AddRectFilled(bar0, bar1, pal.surface(), 11.0f, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(stageMin.x + 1.0f, bar0.y), ImVec2(stageMax.x - 1.0f, bar0.y + hairlinePx()), hex(0x26262E));

        float cy = snapPx((bar0.y + bar1.y) * 0.5f);

        if (snap.job.phase == upix::gui::JobPhase::Running) {
            dl->AddCircleFilled(ImVec2(bar0.x + 18.0f, cy), 3.0f, pal.primary());

            drawText(dl, ui.fontMono, 12.0f, ImVec2(bar0.x + 30.0f, cy - 7.5f), pal.onSurface(), snap.job.current_file.c_str());
            ImVec2 csz = calcText(ui.fontMono, 12.0f, snap.job.current_file.c_str());

            std::string tileMeta = std::format("分块 {}/{} · 剩余 ~0.3s",
                                               snap.job.tiles_done > 0 ? snap.job.tiles_done : 18,
                                               snap.job.tiles_total > 0 ? snap.job.tiles_total : 24);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(bar0.x + 42.0f + csz.x, cy - 7.0f), pal.dim(), tileMeta.c_str());

            // Right: 148x4px inline progress track (#121216) + "75%" (11px #7fdac9)
            std::string pctStr = std::format("{}%", snap.job.percent);
            ImVec2 psz = calcText(ui.fontMono, 11.0f, pctStr.c_str());
            drawText(dl, ui.fontMono, 11.0f, ImVec2(bar1.x - 14.0f - psz.x, cy - 7.0f), pal.primary(), pctStr.c_str());

            float trkW = 148.0f;
            float trkX = snapPx(bar1.x - 24.0f - psz.x - trkW);
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + trkW, cy + 2.0f), hex(0x121216), 2.0f);
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + trkW * (snap.job.percent / 100.0f), cy + 2.0f), pal.primary(), 2.0f);
        } else if (snap.job.phase == upix::gui::JobPhase::Done) {
            dl->AddCircleFilled(ImVec2(bar0.x + 19.0f, cy), 3.0f, pal.primary());

            ImVec2 t0 = snapPt(bar0.x + 32.0f, cy - 10.0f);
            ImVec2 t1 = snapPt(t0.x + 58.0f, cy + 10.0f);
            drawBox(dl, t0, t1, pal.primaryDimBg(), pal.primaryDimBorder(), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, t0, t1, pal.primary(), "4× · PNG");

            std::string resStr = std::format("{}×{} → {}×{}", snap.job.in_w, snap.job.in_h, snap.job.out_w, snap.job.out_h);
            drawText(dl, ui.fontMono, 12.0f, ImVec2(t1.x + 10.0f, cy - 7.5f), pal.onSurface(), resStr.c_str());
            ImVec2 rsz = calcText(ui.fontMono, 12.0f, resStr.c_str());

            std::string metaStr = std::format("{:.2f}s · {} · {}", snap.job.elapsed_sec, snap.job.out_size_str, snap.model);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(t1.x + 20.0f + rsz.x, cy - 7.0f), pal.dim(), metaStr.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(bar1.x - 134.0f, cy - 7.5f), hex(0x8E8E9A), "定位文件");
            ImVec2 open0 = snapPt(bar1.x - 76.0f, cy - 12.0f);
            drawBox(dl, open0, ImVec2(open0.x + 64.0f, open0.y + 24.0f), hex(0x24242C), hex(0x343440), 4.0f);
            drawTextCentered(dl, ui.fontSans, 11.0f, open0, ImVec2(open0.x + 64.0f, open0.y + 24.0f), pal.onSurface(), "打开图像");
        } else if (snap.job.phase == upix::gui::JobPhase::Error) {
            dl->AddCircleFilled(ImVec2(bar0.x + 19.0f, cy), 3.0f, pal.error());

            ImVec2 t0 = snapPt(bar0.x + 32.0f, cy - 10.0f);
            ImVec2 t1 = snapPt(t0.x + 42.0f, cy + 10.0f);
            drawBox(dl, t0, t1, pal.errorDimBg(), pal.errorDimBorder(), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, t0, t1, pal.error(), "VRAM");

            drawText(dl, ui.fontSans, 12.0f, ImVec2(t1.x + 10.0f, cy - 8.0f), pal.onSurface(), snap.job.message.c_str());
            ImVec2 msz = calcText(ui.fontSans, 12.0f, snap.job.message.c_str());

            drawText(dl, ui.fontMono, 11.0f, ImVec2(t1.x + 18.0f + msz.x, cy - 7.0f), pal.dim(), snap.job.diag_detail.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(bar1.x - 94.0f, cy - 7.5f), hex(0x8E8E9A), "跳过");
            if (pillButton(dl, "##retry_job", ImVec2(bar1.x - 56.0f, cy - 12.0f), ImVec2(44.0f, 24.0f), "重试", "", pal, ui.fontSans, false, 4.0f, 11.0f)) {
                app.startJob();
            }
        } else {
            std::string readyStr = std::format("已就绪 · {} 项输入待处理", snap.inputs.size());
            drawText(dl, ui.fontSans, 12.0f, ImVec2(bar0.x + 16.0f, cy - 8.0f), pal.muted(), readyStr.c_str());
        }
    }

    // ---- Minimal Filmstrip Dock (880x54: pure 40x40 thumbnails + bottom status hairline, no text clutter) ----
    if (hasDock) {
        ImVec2 dock0 = snapPt(stageMin.x, stageMax.y + 10.0f);
        ImVec2 dock1 = snapPt(stageMax.x, stageMax.y + 10.0f + dockBarH);
        drawBox(dl, dock0, dock1, pal.surface(), hex(0x26262E), 10.0f);

        float cx = dock0.x + 8.0f;
        float cy = dock0.y + 7.0f;
        size_t doneCnt = 0;
        for (const auto& it : snap.inputs) {
            if (it.status_kind == 2) ++doneCnt;
        }
        for (size_t i = 0; i < snap.inputs.size() && i < 16; ++i) {
            const auto& item = snap.inputs[i];
            ImVec2 th0 = snapPt(cx, cy);
            ImVec2 th1 = snapPt(cx + 40.0f, cy + 40.0f);

            drawMiniThumb(dl, th0, th1, ui, 6.0f, i);

            if (item.status_kind == 0) { // queued
                dl->AddRectFilled(th0, th1, rgba(16, 16, 20, 145), 6.0f);
                dl->AddRect(th0, th1, hex(0x25252D), 6.0f, 0, hairlinePx());
            } else if (item.status_kind == 1) { // running (active glow border + bottom progress bar)
                dl->AddRect(th0, th1, pal.primary(), 6.0f, 0, hairlinePx() * 1.5f);
                float progW = std::clamp(snap.job.percent / 100.0f, 0.08f, 1.0f) * 32.0f;
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 3.5f), snapPt(th0.x + 4.0f + 32.0f, th1.y - 1.5f), rgba(14, 14, 18, 200), 1.0f);
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 3.5f), snapPt(th0.x + 4.0f + progW, th1.y - 1.5f), pal.primary(), 1.0f);
            } else if (item.status_kind == 2) { // done (subtle frame + bottom teal accent line)
                dl->AddRect(th0, th1, hex(0x2E3836), 6.0f, 0, hairlinePx());
                dl->AddRectFilled(snapPt(th0.x + 10.0f, th1.y - 2.5f), snapPt(th1.x - 10.0f, th1.y - 1.0f), pal.primary(), 1.0f);
            } else if (item.status_kind == 3) { // error (coral border + bottom error bar)
                dl->AddRectFilled(th0, th1, rgba(35, 16, 18, 120), 6.0f);
                dl->AddRect(th0, th1, pal.error(), 6.0f, 0, hairlinePx() * 1.5f);
                dl->AddRectFilled(snapPt(th0.x + 8.0f, th1.y - 3.0f), snapPt(th1.x - 8.0f, th1.y - 1.0f), pal.error(), 1.0f);
            }

            cx += 40.0f + 6.0f;
        }

        // Right counter ("2 / 5") + 1x18px divider + 28x28 "+" button
        std::string cntText = std::format("{} / {}", doneCnt, snap.inputs.size());
        ImVec2 cntSz = calcText(ui.fontMono, 11.0f, cntText.c_str());
        drawText(dl, ui.fontMono, 11.0f, ImVec2(dock1.x - 58.0f - cntSz.x, dock0.y + 19.5f), pal.dim(), cntText.c_str());
        dl->AddRectFilled(snapPt(dock1.x - 46.0f, dock0.y + 18.0f), snapPt(dock1.x - 46.0f + hairlinePx(), dock0.y + 36.0f), hex(0x26262E));
        ImVec2 add0 = snapPt(dock1.x - 36.0f, dock0.y + 13.0f);
        ImGui::SetCursorScreenPos(add0);
        if (ImGui::InvisibleButton("##dock_add", ImVec2(28.0f, 28.0f))) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
        drawBox(dl, add0, ImVec2(add0.x + 28.0f, add0.y + 28.0f), hex(0x202027), hex(0x2E2E38), 6.0f);
        drawIcon(dl, "plus", ImVec2(add0.x + 14.0f, add0.y + 14.0f), 12.0f, pal.muted());
    }

    // -----------------------------------------------------------------------
    // 3. Right Inspector (308px fixed width) — Figma "Desktop / Inspector"
    // Note: In Figma 4:2 & 73:1931, Inspector background is #131316 (var(--bg)) with NO left border!
    // -----------------------------------------------------------------------
    dl->AddRectFilled(ImVec2(inspX, 0), ImVec2(winSize.x, winSize.y), pal.bg());

    float ix = snapPx(inspX + 16.0f);
    float iw = 276.0f;
    float iy = 16.0f;

    // --- Section: 模型 (Figma: 12px #a2a2ac, gap 8px/12px) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "模型");
    iy += 24.0f;

    // Model Picker Field (Figma: height 36px, #18181d, border #2d2d36, radius 8px)
    ImVec2 mp0 = snapPt(ix, iy);
    ImVec2 mp1 = snapPt(ix + iw, iy + 36.0f);
    ImGui::SetCursorScreenPos(mp0);
    if (ImGui::InvisibleButton("##model_picker_btn", ImVec2(iw, 36.0f))) {
        ui.showModelPicker = !ui.showModelPicker;
    }
    bool mpHover = ImGui::IsItemHovered();
    if (mpHover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    drawBox(dl, mp0, mp1, mpHover ? pal.container() : hex(0x18181D), ui.showModelPicker ? pal.primaryDimBorder() : hex(0x2D2D36), 8.0f);

    std::string shortMod = snap.model;
    int modScale = 4;
    bool modRec = true;
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) {
            shortMod = m.short_name;
            modScale = m.scale;
            modRec = m.recommended;
            break;
        }
    }
    drawText(dl, ui.fontMono, 13.0f, ImVec2(mp0.x + 12.0f, mp0.y + 9.0f), pal.onSurface(), shortMod.c_str());
    if (modRec) {
        ImVec2 mtsz = calcText(ui.fontMono, 13.0f, shortMod.c_str());
        drawText(dl, ui.fontSans, 11.0f, ImVec2(mp0.x + 18.0f + mtsz.x, mp0.y + 10.5f), pal.onSurface(), "★");
    }

    // [4×] badge (Figma: height 20px, #1c1c22, border #2e2e37, radius 4px, 11px #a2a2ac) + 14x14 chevron
    std::string scaleBadge = std::format("{}×", modScale);
    ImVec2 b0 = snapPt(mp1.x - 58.0f, mp0.y + 8.0f);
    ImVec2 b1 = snapPt(mp1.x - 32.0f, mp0.y + 28.0f);
    drawBox(dl, b0, b1, hex(0x1C1C22), hex(0x2E2E37), 4.0f);
    drawTextCentered(dl, ui.fontMono, 11.0f, b0, b1, pal.muted(), scaleBadge.c_str());
    drawIcon(dl, "chevron-down", ImVec2(mp1.x - 18.0f, mp0.y + 18.0f), 14.0f, pal.muted());
    iy += 48.0f;

    // --- Section: 输出 (Figma: 12px #a2a2ac) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "输出");
    iy += 24.0f;
    const SegmentOption fmtOpts[5] = {
        {"跟随", ""},
        {"PNG", "png"},
        {"JPG", "jpg"},
        {"WebP", "webp"},
        {"AVIF", "avif"},
    };
    std::string curFmt = snap.format;
    if (segmentedControl(dl, "##fmt_seg", ImVec2(ix, iy), ImVec2(iw, 32.0f), fmtOpts, 5, curFmt, pal, ui.fontSans, 12.0f)) {
        app.setFormat(curFmt);
    }
    iy += 44.0f;

    // Quality Slider ("质量" — Figma: label 14px #ececee, value 12.5px/14px JetBrains Mono #ececee)
    if (snap.format != "png") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy), pal.onSurface(), "质量");
        std::string qValStr = std::format("{}", snap.quality);
        ImVec2 qsz = calcText(ui.fontMono, 13.0f, qValStr.c_str());
        drawText(dl, ui.fontMono, 13.0f, ImVec2(ix + iw - qsz.x, iy + 1.0f), pal.onSurface(), qValStr.c_str());
        iy += 26.0f;

        int q = snap.quality;
        if (qualitySlider(dl, "##q_slider", ImVec2(ix, iy), iw, q, 1, 100, pal)) {
            app.setQuality(q);
        }
        iy += 30.0f;
    }

    // Format-linked encoder parameters (AVIF / JPG chroma & speed — Figma Frame 73:1931: 14px label, 164x28 segmented)
    if (snap.format == "avif" || snap.format == "jpg") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "色度抽样");
        const SegmentOption chromaOpts[2] = {{"4:2:0", "420"}, {"4:4:4", "444"}};
        std::string cVal = std::to_string(snap.chroma);
        if (segmentedControl(dl, "##chroma_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), chromaOpts, 2, cVal, pal, ui.fontMono, 12.0f)) {
            app.setChroma(std::atoi(cVal.c_str()));
        }
        iy += 38.0f;
    }
    if (snap.format == "avif") {
        drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "编码速度");
        const SegmentOption spdOpts[3] = {{"精细", "4"}, {"均衡", "6"}, {"快速", "8"}};
        std::string sVal = std::to_string(snap.avif_speed);
        if (segmentedControl(dl, "##spd_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), spdOpts, 3, sVal, pal, ui.fontSans, 12.0f)) {
            app.setAvifSpeed(std::atoi(sVal.c_str()));
        }
        iy += 38.0f;
    }

    // Export Directory ("导出位置" — Figma: 13px #ececee, gap 8px, box 34px #16161a border #282830 radius 8px)
    drawText(dl, ui.fontSans, 13.0f, ImVec2(ix, iy), pal.onSurface(), "导出位置");
    iy += 24.0f;
    ImVec2 dir0 = snapPt(ix, iy);
    ImVec2 dir1 = snapPt(ix + iw, iy + 34.0f);
    drawBox(dl, dir0, dir1, hex(0x16161A), pal.outline(), 8.0f);
    drawIcon(dl, "folder", ImVec2(dir0.x + 16.5f, dir0.y + 17.0f), 13.0f, hex(0x7E7E8A));

    bool hasCustomDir = !snap.custom_out_dir.empty();
    std::string outLabel = hasCustomDir ? snap.custom_out_dir : "跟随原图目录";
    drawText(dl, hasCustomDir ? ui.fontMono : ui.fontSans, 12.0f, ImVec2(dir0.x + 31.0f, dir0.y + 9.0f),
             hex(0xD0D0D8), outLabel.c_str());

    // Pill "指定…" / "更改" (Figma: height 24px, #202026, border #2e2e38, radius 6px, 11px #a2a2ac)
    const char* dirBtnLbl = hasCustomDir ? "更改" : "指定…";
    float dirBtnW = hasCustomDir ? 40.0f : 48.0f;
    ImVec2 db0 = snapPt(dir1.x - dirBtnW - 4.0f, dir0.y + 5.0f);
    ImGui::SetCursorScreenPos(db0);
    if (ImGui::InvisibleButton("##pick_outdir", ImVec2(dirBtnW, 24.0f))) {
        SDL_ShowOpenFolderDialog(onFileDialog, dlgOutDir, window, nullptr, false);
    }
    drawBox(dl, db0, ImVec2(db0.x + dirBtnW, db0.y + 24.0f), pal.container(), hex(0x2E2E38), 6.0f);
    drawTextCentered(dl, ui.fontSans, 11.0f, db0, ImVec2(db0.x + dirBtnW, db0.y + 24.0f), pal.muted(), dirBtnLbl);
    iy += 46.0f;

    // --- Section: 引擎与处理 (Figma: 12px #a2a2ac, gap 12px) ---
    drawText(dl, ui.fontSans, 12.0f, ImVec2(ix, iy), pal.muted(), "引擎与处理");
    iy += 26.0f;

    // 计算设备 (Figma: 14px #ececee, segmented 164x28px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "计算设备");
    const SegmentOption devOpts[2] = {{"GPU", "gpu"}, {"CPU", "cpu"}};
    std::string curDev = snap.gpu ? "gpu" : "cpu";
    if (segmentedControl(dl, "##dev_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), devOpts, 2, curDev, pal, ui.fontSans, 12.0f)) {
        app.setGpu(curDev == "gpu");
    }
    iy += 40.0f;

    // 分块尺寸 (Figma: 14px #ececee, segmented 164x28px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 4.0f), pal.onSurface(), "分块尺寸");
    const SegmentOption tileOpts[4] = {{"自动", "0"}, {"512", "512"}, {"256", "256"}, {"128", "128"}};
    std::string curTile = std::to_string(snap.tile_size);
    if (segmentedControl(dl, "##tile_seg", ImVec2(ix + iw - 164.0f, iy), ImVec2(164.0f, 28.0f), tileOpts, 4, curTile, pal, ui.fontSans, 12.0f)) {
        app.setTileSize(std::atoi(curTile.c_str()));
    }
    iy += 40.0f;

    // 常驻热引擎 (Figma: 14px #ececee, toggle 44x26px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 3.0f), pal.onSurface(), "常驻热引擎");
    bool daemonOn = snap.daemon.running;
    if (toggleSwitch(dl, "##daemon_toggle", ImVec2(ix + iw - 44.0f, iy), daemonOn, pal)) {
        app.toggleDaemon();
    }
    iy += 38.0f;

    // 覆盖已有结果 (Figma: 14px/15px #ececee, toggle 44x26px)
    drawText(dl, ui.fontSans, 14.0f, ImVec2(ix, iy + 3.0f), pal.onSurface(), "覆盖已有结果");
    bool forceOn = snap.force;
    if (toggleSwitch(dl, "##force_toggle", ImVec2(ix + iw - 44.0f, iy), forceOn, pal)) {
        app.setForce(forceOn);
    }

    // --- Bottom Sticky Action Area (Figma: 276x44px, bottom 16px, font 14px weight 500) ---
    ImVec2 cta0 = snapPt(ix, winSize.y - 60.0f);
    ImVec2 ctaSz(iw, 44.0f);
    if (snap.job.phase == upix::gui::JobPhase::Running) {
        // Cancel CTA (Figma Frame 73:1931: #1e1416, border #58282c, stop 9x9px #ff7361, "取消超分" 14px, pill "Ctrl+." #2a181a border #4a2528)
        ImGui::SetCursorScreenPos(cta0);
        if (ImGui::InvisibleButton("##cancel_job", ctaSz)) {
            app.cancelJob();
        }
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        drawBox(dl, cta0, ImVec2(cta0.x + ctaSz.x, cta0.y + ctaSz.y), pal.errorDimBg(), pal.errorDimBorder(), 8.0f);

        float midY = snapPx(cta0.y + ctaSz.y * 0.5f);
        dl->AddRectFilled(snapPt(cta0.x + 74.0f, midY - 4.5f), snapPt(cta0.x + 83.0f, midY + 4.5f), pal.error(), 2.0f);
        drawText(dl, ui.fontSans, 14.0f, ImVec2(cta0.x + 91.0f, midY - 9.0f), pal.error(), "取消超分");
        ImVec2 kbd0 = snapPt(cta0.x + 155.0f, midY - 9.0f);
        ImVec2 kbd1 = snapPt(kbd0.x + 44.0f, midY + 9.0f);
        drawBox(dl, kbd0, kbd1, hex(0x2A181A), hex(0x4A2528), 4.0f);
        drawTextCentered(dl, ui.fontMono, 10.0f, kbd0, kbd1, hex(0xFF897A), "Ctrl+.");
    } else if (snap.job.phase == upix::gui::JobPhase::Error) {
        if (pillButton(dl, "##resume_job", cta0, ctaSz, "继续超分", "play", pal, ui.fontSans, false, 8.0f, 14.0f)) {
            app.startJob();
        }
    } else if (snap.job.phase == upix::gui::JobPhase::Done) {
        if (pillButton(dl, "##rerun_job", cta0, ctaSz, "重新超分", "play", pal, ui.fontSans, true, 8.0f, 14.0f)) {
            app.startJob();
        }
    } else {
        if (pillButton(dl, "##start_job", cta0, ctaSz, "开始超分", "play", pal, ui.fontSans, true, 8.0f, 14.0f)) {
            if (snap.inputs.empty()) {
                SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
            } else {
                app.startJob();
            }
        }
    }

    // --- Floating Model Picker Popup (Figma "Desktop / Input / Model picker", 276px wide) ---
    if (ui.showModelPicker) {
        ImVec2 pop0 = snapPt(ix, mp1.y + 6.0f);
        ImVec2 pop1 = snapPt(ix + iw, mp1.y + 274.0f);
        drawBox(dl, pop0, pop1, pal.surface(), pal.outlineStrong(), 8.0f);

        ImVec2 sb0 = snapPt(pop0.x + 6.0f, pop0.y + 6.0f);
        ImVec2 sb1 = snapPt(pop1.x - 6.0f, pop0.y + 40.0f);
        dl->AddRectFilled(sb0, sb1, pal.recessed(), 6.0f);
        drawIcon(dl, "search", ImVec2(sb0.x + 16.0f, sb0.y + 17.0f), 14.0f, pal.muted());
        dl->AddRectFilled(ImVec2(sb0.x + 30.0f, sb0.y + 10.0f), ImVec2(sb0.x + 31.5f, sb0.y + 24.0f), pal.primary());
        dl->AddRectFilled(ImVec2(sb0.x, sb1.y), ImVec2(sb1.x, sb1.y + 1.5f), pal.primary());

        float ry = sb1.y + 6.0f;
        for (size_t i = 0; i < snap.installed.size() && ry + 42.0f < pop1.y - 4.0f; ++i) {
            const auto& m = snap.installed[i];
            ImVec2 r0 = snapPt(pop0.x + 6.0f, ry);
            ImVec2 r1 = snapPt(pop1.x - 6.0f, ry + 42.0f);
            ImGui::SetCursorScreenPos(r0);
            ImGui::PushID((int)i);
            bool clicked = ImGui::InvisibleButton("##mp_row", ImVec2(r1.x - r0.x, 42.0f));
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();

            bool sel = (m.name == snap.model);
            if (sel || hov) {
                drawBox(dl, r0, r1, sel ? pal.high() : pal.containerLow(), sel ? pal.outlineStrong() : 0, 6.0f);
            }
            drawText(dl, ui.fontMono, 12.5f, ImVec2(r0.x + 12.0f, r0.y + 6.0f), pal.onSurface(), m.name.c_str());
            std::string subMeta = std::format("{} · x{}", m.size, m.scale);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(r0.x + 12.0f, r0.y + 23.0f), pal.dim(), subMeta.c_str());

            if (sel) {
                dl->AddCircleFilled(ImVec2(r1.x - 14.0f, (r0.y + r1.y) * 0.5f), 3.5f, pal.primary());
            }

            if (clicked) {
                app.setModel(m.name);
                ui.showModelPicker = false;
            }
            ry += 44.0f;
        }
    }
}



} // namespace upix::views
