#include "views/common.hpp"

#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace upix::views {

void renderModelsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                             upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                             SDL_Window* window, DialogBridge* dlgImport) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float availW = winSize.x - railW;
    float colW = std::min(760.0f, availW - 48.0f);
    float colX = snapPx(railW + (availW - colW) * 0.5f);
    float y = 28.0f;

    // Top bar (34px): Search (220x34, #151519, border #282830, radius 8px) + Scene Filter (210x34) + Right 3 square icon buttons (32x32)
    ImVec2 s0 = snapPt(colX, y);
    ImVec2 s1 = snapPt(colX + 220.0f, y + 34.0f);
    drawBox(dl, s0, s1, pal.recessed(), pal.outline(), 8.0f);
    drawIcon(dl, "search", ImVec2(s0.x + 17.0f, s0.y + 17.0f), 14.0f, pal.muted());
    drawText(dl, ui.fontSans, 12.0f, ImVec2(s0.x + 32.0f, s0.y + 9.0f), pal.muted(), "搜索…");

    const SegmentOption sceneOpts[5] = {
        {"全部", "all"},
        {"动漫", "动漫"},
        {"照片", "照片"},
        {"综合", "综合"},
        {"修复", "修复"},
    };
    segmentedControl(dl, "##scene_filter", ImVec2(s1.x + 12.0f, y), ImVec2(210.0f, 34.0f),
                     sceneOpts, 5, ui.modelsSceneFilter, pal, ui.fontSans, 12.0f);

    if (iconButton(dl, "##open_mdir", ImVec2(colX + colW - 108.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "folder", pal)) {
        SDL_OpenURL(("file://" + snap.models_dir).c_str());
    }
    if (iconButton(dl, "##import_mod", ImVec2(colX + colW - 70.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "download", pal)) {
        SDL_ShowOpenFileDialog(onFileDialog, dlgImport, window, nullptr, 0, nullptr, true);
    }
    if (iconButton(dl, "##refresh_mod", ImVec2(colX + colW - 32.0f, y + 1.0f), ImVec2(32.0f, 32.0f), "refresh", pal)) {
        app.refreshModels();
    }
    y += 48.0f;

    auto sceneDotColor = [&](const std::string& sc) -> ImU32 {
        if (sc.find("动漫") != std::string::npos) return pal.primary();
        if (sc.find("照片") != std::string::npos) return hex(0xE5A952);
        if (sc.find("修复") != std::string::npos) return hex(0xE07A9A);
        return hex(0x9BA3E8);
    };

    auto renderFamilySection = [&](const char* title, const std::vector<upix::gui::ModelFamily>& groups, bool isRemote) {
        size_t totalCount = 0;
        for (const auto& g : groups) totalCount += g.items.size();

        // Figma 82:2938: "已安装" 12px #a2a2ac + "6" 12px #6e6e7a + 1px rule #25252c
        drawText(dl, ui.fontSans, 12.0f, ImVec2(colX, y), pal.muted(), title);
        std::string cntStr = std::format("{}", totalCount);
        drawText(dl, ui.fontMono, 12.0f, ImVec2(colX + 46.0f, y), pal.dim(), cntStr.c_str());
        dl->AddRectFilled(snapPt(colX + 64.0f, y + 9.0f), snapPt(colX + colW, y + 9.0f + hairlinePx()), hex(0x25252C));
        y += 32.0f;

        for (const auto& grp : groups) {
            std::vector<upix::gui::ModelItem> filtered;
            for (const auto& it : grp.items) {
                if (ui.modelsSceneFilter == "all" || it.scene.find(ui.modelsSceneFilter) != std::string::npos) {
                    filtered.push_back(it);
                }
            }
            if (filtered.empty()) continue;

            // Figma 82:2942: group card #1c1c21, border #282830, radius 10px; header 34px #202026; rows 38px
            float cardH = 34.0f + filtered.size() * 38.0f;
            ImVec2 c0 = snapPt(colX, y);
            ImVec2 c1 = snapPt(colX + colW, y + cardH);
            drawBox(dl, c0, c1, pal.containerLow(), pal.outline(), 10.0f);

            dl->AddRectFilled(snapPt(c0.x + 1.0f, c0.y + 1.0f), snapPt(c1.x - 1.0f, c0.y + 34.0f), pal.container(), 9.0f, ImDrawFlags_RoundCornersTop);
            drawText(dl, ui.fontSans, 12.0f, ImVec2(c0.x + 16.0f, c0.y + 9.0f), hex(0xC7C7D1), grp.label.c_str());
            std::string gCnt = std::format("{} 款", filtered.size());
            ImVec2 gsz = calcText(ui.fontSans, 11.0f, gCnt.c_str());
            drawText(dl, ui.fontSans, 11.0f, ImVec2(c1.x - gsz.x - 16.0f, c0.y + 9.5f), pal.dim(), gCnt.c_str());

            float ry = c0.y + 34.0f;
            for (size_t i = 0; i < filtered.size(); ++i) {
                const auto& m = filtered[i];
                dl->AddRectFilled(snapPt(c0.x + 1.0f, ry), snapPt(c1.x - 1.0f, ry + hairlinePx()), hex(0x23232A));

                bool isCurrent = (!isRemote && m.name == snap.model);
                drawText(dl, ui.fontMono, 13.0f, ImVec2(c0.x + 16.0f, ry + 10.5f), pal.onSurface(), m.short_name.c_str());
                ImVec2 nsz = calcText(ui.fontMono, 13.0f, m.short_name.c_str());
                float iconX = c0.x + 16.0f + nsz.x + 8.0f;
                if (m.recommended) {
                    drawText(dl, ui.fontSans, 11.0f, ImVec2(iconX, ry + 11.5f), pal.muted(), "★");
                    iconX += 16.0f;
                }
                if (isCurrent && snap.daemon.running) {
                    drawIcon(dl, "zap", ImVec2(iconX + 4.0f, ry + 19.0f), 11.5f, pal.primary());
                }

                // Figma 82:2761: Name column width 244px -> badges start at c0.x + 16 + 244 = c0.x + 260
                float bColX = c0.x + 260.0f;
                ImVec2 sc0 = snapPt(bColX, ry + 9.0f);
                ImVec2 sc1 = snapPt(bColX + 46.0f, ry + 29.0f);
                drawBox(dl, sc0, sc1, hex(0x23232A), hex(0x32323C), 4.0f);
                dl->AddCircleFilled(ImVec2(sc0.x + 9.5f, ry + 19.0f), 2.5f, sceneDotColor(m.scene));
                drawText(dl, ui.fontSans, 11.0f, ImVec2(sc0.x + 16.0f, ry + 11.0f), hex(0xC7C7D1), m.scene.c_str());

                std::string scaleStr = std::format("{}×", m.scale);
                ImVec2 sb0 = snapPt(sc1.x + 8.0f, ry + 9.0f);
                ImVec2 sb1 = snapPt(sb0.x + 26.0f, ry + 29.0f);
                drawBox(dl, sb0, sb1, hex(0x1C1C22), hex(0x2E2E37), 4.0f);
                drawTextCentered(dl, ui.fontMono, 11.0f, sb0, sb1, pal.muted(), scaleStr.c_str());

                ImGui::PushID(m.name.c_str());
                if (!isRemote) {
                    ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                    drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 52.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                    ImGui::SetCursorScreenPos(ImVec2(c1.x - 40.0f, ry + 6.0f));
                    if (ImGui::InvisibleButton("##del_m", ImVec2(26.0f, 26.0f))) {
                        app.removeModel(m.name);
                    }
                    drawIcon(dl, "trash", ImVec2(c1.x - 27.0f, ry + 19.0f), 13.5f, hex(0x5E5E6A));
                } else {
                    if (snap.downloading == m.name) {
                        ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                        drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 140.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                        float barX = snapPx(c1.x - 126.0f);
                        float barW = 72.0f;
                        dl->AddRectFilled(ImVec2(barX, ry + 17.5f), ImVec2(barX + barW, ry + 20.5f), hex(0x2A2A33), 1.5f);
                        dl->AddRectFilled(ImVec2(barX, ry + 17.5f), ImVec2(barX + barW * (std::max(0, snap.download_pct) / 100.0f), ry + 20.5f), pal.primary(), 1.5f);
                        std::string pct = std::format("{}%", std::max(0, snap.download_pct));
                        drawText(dl, ui.fontMono, 11.0f, ImVec2(c1.x - 44.0f, ry + 11.5f), pal.primary(), pct.c_str());
                    } else {
                        ImVec2 ssz = calcText(ui.fontMono, 12.0f, m.size.c_str());
                        drawText(dl, ui.fontMono, 12.0f, ImVec2(c1.x - 52.0f - ssz.x, ry + 11.0f), hex(0x8F8F9E), m.size.c_str());
                        if (iconButton(dl, "##dl_m", ImVec2(c1.x - 40.0f, ry + 6.0f), ImVec2(26.0f, 26.0f), "download", pal, 13.0f, 6.0f)) {
                            app.downloadModel(m.name);
                        }
                    }
                }
                ImGui::PopID();

                ry += 38.0f;
            }
            y += cardH + 14.0f;
        }
    };

    renderFamilySection("已安装", snap.installed_groups, false);
    renderFamilySection("可下载", snap.remote_groups, true);
}



} // namespace upix::views
