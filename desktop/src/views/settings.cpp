#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>
#include <cstdio>

namespace upix::views {

void renderSettingsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float railW = 76.0f;
    const float pad = 28.0f;
    const float colW = std::min(760.0f, std::max(360.0f, winSize.x - railW - pad * 2.0f));
    const float x0 = snapPx(railW + (winSize.x - railW - colW) * 0.5f);
    float y = 28.0f;

    drawText(dl, ui.fontSans, geist::type::SUBHEADING, snapPt(x0, y), t.ink, "设置");
    y += 28.0f;
    drawText(dl, ui.fontSans, geist::type::BODY_SMALL, snapPt(x0, y), t.mute,
             "外观、GPU 推理与常驻热引擎配置会自动持久化保存。");
    y += 28.0f;

    // 1. 外观与交互
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "外观与交互");
    y += 22.0f;
    {
        const ImVec2 card0 = snapPt(x0, y);
        const float cardH = 114.0f;
        const ImVec2 card1 = snapPt(x0 + colW, y + cardH);
        ctl::groupCard(dl, card0, card1);

        auto r1 = ctl::cardRow(dl, ImGui::GetID("set-theme"), card0, colW,
                               "深色外观", "切换 Geist Pure-Dark 与 Light 画布主题", ui.fontSans, false, 64.0f);
        bool dark = snap.dark_mode;
        if (ctl::toggleSwitch(dl, ImGui::GetID("sw-dark"),
                              snapPt(r1.rightAnchorX - ctl::geom::TOGGLE_W, r1.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              dark)) {
            app.setDarkMode(dark);
        }

        ctl::cardDivider(dl, card0.x, card1.x, r1.bottomY);
        auto r2 = ctl::cardRow(dl, ImGui::GetID("set-motion"), ImVec2(card0.x, r1.bottomY), colW,
                               "减少动态效果", "禁用弹簧缓动与滑块过渡，仅保留瞬时状态切换", ui.fontSans, false, 64.0f);
        bool rm = snap.reduce_motion;
        if (ctl::toggleSwitch(dl, ImGui::GetID("sw-rm"),
                              snapPt(r2.rightAnchorX - ctl::geom::TOGGLE_W, r2.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              rm)) {
            app.setReduceMotion(rm);
        }
        y = card1.y + geist::space::SECTION;
    }

    // 2. 推理引擎与热引擎
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "推理引擎与热引擎");
    y += 22.0f;
    {
        const ImVec2 card0 = snapPt(x0, y);
        const float cardH = 228.0f;
        const ImVec2 card1 = snapPt(x0 + colW, y + cardH);
        ctl::groupCard(dl, card0, card1);

        auto r1 = ctl::cardRow(dl, ImGui::GetID("set-gpu"), card0, colW,
                               "Vulkan GPU 加速", "关闭时强制回退至 CPU 多线程推理", ui.fontSans, false, 64.0f);
        bool gpu = snap.gpu;
        if (ctl::toggleSwitch(dl, ImGui::GetID("sw-gpu"),
                              snapPt(r1.rightAnchorX - ctl::geom::TOGGLE_W, r1.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              gpu)) {
            app.setGpu(gpu);
        }

        ctl::cardDivider(dl, card0.x, card1.x, r1.bottomY);
        auto r2 = ctl::cardRow(dl, ImGui::GetID("set-tile"), ImVec2(card0.x, r1.bottomY), colW,
                               "推理分块大小 (Tile)", "显存紧张时建议降低至 256 或 128", ui.fontSans, false, 240.0f);
        const char* tileLabels[4] = {"自动", "512", "256", "128"};
        const int tileVals[4] = {0, 512, 256, 128};
        int tileIdx = 0;
        for (int i = 0; i < 4; ++i) {
            if (snap.tile_size == tileVals[i]) tileIdx = i;
        }
        if (ctl::segmented(dl, ImGui::GetID("seg-tile"),
                           snapPt(r2.rightAnchorX - 220.0f, r2.centerY - ctl::geom::SEG_H * 0.5f),
                           ImVec2(220.0f, ctl::geom::SEG_H), tileLabels, 4, tileIdx, ui.fontSans, 12.0f)) {
            app.setTileSize(tileVals[tileIdx]);
        }

        ctl::cardDivider(dl, card0.x, card1.x, r2.bottomY);
        char daemonSub[128];
        if (snap.daemon.running && snap.daemon.pid > 0) {
            std::snprintf(daemonSub, sizeof(daemonSub), "%s · PID %d", snap.daemon.label.c_str(), snap.daemon.pid);
        } else {
            std::snprintf(daemonSub, sizeof(daemonSub), "%s · 后台缓存模型避免重复加载", snap.daemon.label.c_str());
        }
        auto r3 = ctl::cardRow(dl, ImGui::GetID("set-daemon"), ImVec2(card0.x, r2.bottomY), colW,
                               "常驻热引擎", daemonSub, ui.fontSans, false, 110.0f);
        if (ctl::secondaryButton(dl, ImGui::GetID("btn-daemon"),
                                 snap.daemon.running ? "停止引擎" : "启动引擎", ui.fontSans,
                                 snapPt(r3.rightAnchorX - 96.0f, r3.centerY - 15.0f),
                                 snapPt(r3.rightAnchorX, r3.centerY + 15.0f),
                                 nullptr, !snap.daemon.loading, snap.daemon.running)) {
            app.toggleDaemon();
        }

        ctl::cardDivider(dl, card0.x, card1.x, r3.bottomY);
        auto r4 = ctl::cardRow(dl, ImGui::GetID("set-idle"), ImVec2(card0.x, r3.bottomY), colW,
                               "热引擎空闲休眠", "无任务超过该时长自动释放显存", ui.fontSans, false, 240.0f);
        const char* idleLabels[4] = {"1分", "5分", "15分", "常驻"};
        const int idleVals[4] = {60, 300, 900, 0};
        int idleIdx = 1;
        for (int i = 0; i < 4; ++i) {
            if (snap.daemon.idle_secs == idleVals[i]) idleIdx = i;
        }
        if (ctl::segmented(dl, ImGui::GetID("seg-idle"),
                           snapPt(r4.rightAnchorX - 220.0f, r4.centerY - ctl::geom::SEG_H * 0.5f),
                           ImVec2(220.0f, ctl::geom::SEG_H), idleLabels, 4, idleIdx, ui.fontSans, 12.0f)) {
            app.setDaemonIdleSecs(idleVals[idleIdx]);
        }

        y = card1.y + geist::space::SECTION;
    }

    // 3. 文件与批量策略
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "文件与批量策略");
    y += 22.0f;
    {
        const ImVec2 card0 = snapPt(x0, y);
        const float cardH = 114.0f;
        const ImVec2 card1 = snapPt(x0 + colW, y + cardH);
        ctl::groupCard(dl, card0, card1);

        auto r1 = ctl::cardRow(dl, ImGui::GetID("set-rec"), card0, colW,
                               "包含子目录", "添加文件夹时递归扫描下级图片（自动跳过 upix_* 输出目录）", ui.fontSans, false, 64.0f);
        bool rec = snap.recursive;
        if (ctl::toggleSwitch(dl, ImGui::GetID("sw-rec"),
                              snapPt(r1.rightAnchorX - ctl::geom::TOGGLE_W, r1.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              rec)) {
            app.setRecursive(rec);
        }

        ctl::cardDivider(dl, card0.x, card1.x, r1.bottomY);
        auto r2 = ctl::cardRow(dl, ImGui::GetID("set-force"), ImVec2(card0.x, r1.bottomY), colW,
                               "允许重绘已放大图像", "忽略文件内嵌的 Upix 超分标记强制重新推理", ui.fontSans, false, 64.0f);
        bool force = snap.force;
        if (ctl::toggleSwitch(dl, ImGui::GetID("sw-force"),
                              snapPt(r2.rightAnchorX - ctl::geom::TOGGLE_W, r2.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              force)) {
            app.setForce(force);
        }
    }
}

} // namespace upix::views
