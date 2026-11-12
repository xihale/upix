#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace upix::views {
namespace {

float calcRowH(ImFont* font, const char* title, const char* subtitle) {
    using namespace upix::ui;
    const ImVec2 tsz = calcText(font, geist::type::BODY, title ? title : "");
    const bool hasSub = subtitle && subtitle[0];
    const ImVec2 ssz = hasSub ? calcText(font, ctl::geom::ROW_SUB, subtitle) : ImVec2(0.0f, 0.0f);
    const float contentH = hasSub ? (tsz.y + 3.0f + ssz.y) : tsz.y;
    return std::max(ctl::geom::ROW_MIN_H, contentH + ctl::geom::ROW_PY * 2.0f);
}

} // namespace

void renderSettingsPage(ImDrawList* /*dl*/, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    constexpr float kRailW = 76.0f;
    constexpr float kPadY = 36.0f;
    const float paneX = kRailW;
    const float paneW = std::max(0.0f, winSize.x - kRailW);
    const float colW = std::min(660.0f, std::max(320.0f, paneW - 64.0f));
    const float colX = snapPx(paneX + (paneW - colW) * 0.5f);

    ImGui::SetCursorScreenPos(ImVec2(paneX, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, t.outlineInput);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, t.mute);
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, t.body);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f);

    ImGui::BeginChild("##settings_pane", ImVec2(paneW, winSize.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float scrollY = ImGui::GetScrollY();
    float y = snapPx(kPadY - scrollY);

    // Page header
    {
        const ImVec2 hsz = calcText(ui.fontSans, geist::type::SUBHEADING, "设置");
        drawText(cdl, ui.fontSans, geist::type::SUBHEADING, snapPt(colX, y), t.ink, "设置");
        y += hsz.y + 4.0f;
        const ImVec2 ssz = calcText(ui.fontSans, geist::type::BODY_SMALL,
                                    "推理引擎、默认编码与批处理策略会自动保存。");
        drawText(cdl, ui.fontSans, geist::type::BODY_SMALL, snapPt(colX, y), t.mute,
                 "推理引擎、默认编码与批处理策略会自动保存。");
        y += ssz.y + geist::space::XL;
    }

    // 1. 推理引擎
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "推理引擎");
    y += 20.0f;
    {
        const char* gpuTitle = "计算设备";
        const char* gpuSub = "GPU 更快，CPU 更稳";
        const char* tileTitle = "分块尺寸";
        const char* tileSub = "显存紧张时选小一些";
        const char* daemonTitle = "常驻热引擎";
        const char* idleTitle = "常驻时间";
        const char* idleSub = "空闲多久后自动退出引擎";

        const float h1 = calcRowH(ui.fontSans, gpuTitle, gpuSub);
        const float h2 = calcRowH(ui.fontSans, tileTitle, tileSub);
        const float h3 = calcRowH(ui.fontSans, daemonTitle, "已就绪");
        const float h4 = calcRowH(ui.fontSans, idleTitle, idleSub);

        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + h1 + h2 + h3 + h4);
        ctl::groupCard(cdl, card0, card1);

        // 计算设备
        auto r1 = ctl::cardRow(cdl, ImGui::GetID("set-gpu"), card0, colW, gpuTitle, gpuSub,
                               ui.fontSans, false, 152.0f);
        {
            const char* devLabels[2] = {"GPU", "CPU"};
            int devIdx = snap.gpu ? 0 : 1;
            const float segW = 136.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-gpu"),
                               snapPt(r1.rightAnchorX - segW, r1.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), devLabels, 2, devIdx, ui.fontSans, 12.0f)) {
                app.setGpu(devIdx == 0);
            }
        }

        // 分块尺寸
        ctl::cardDivider(cdl, card0.x, card1.x, r1.bottomY);
        auto r2 = ctl::cardRow(cdl, ImGui::GetID("set-tile"), ImVec2(card0.x, r1.bottomY), colW,
                               tileTitle, tileSub, ui.fontSans, false, 228.0f);
        {
            const char* tileLabels[4] = {"自动", "512", "256", "128"};
            const int tileVals[4] = {0, 512, 256, 128};
            int tileIdx = 0;
            for (int i = 0; i < 4; ++i) {
                if (snap.tile_size == tileVals[i]) tileIdx = i;
            }
            const float segW = 212.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-tile"),
                               snapPt(r2.rightAnchorX - segW, r2.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), tileLabels, 4, tileIdx, ui.fontSans, 12.0f)) {
                app.setTileSize(tileVals[tileIdx]);
            }
        }

        // 常驻热引擎 (live status subtitle with Vercel blue dot or busyArc)
        ctl::cardDivider(cdl, card0.x, card1.x, r2.bottomY);
        {
            const ImVec2 row0 = snapPt(card0.x, r2.bottomY);
            const ImVec2 row1 = snapPt(card1.x, r2.bottomY + h3);
            const float rowCenterY = (row0.y + row1.y) * 0.5f;
            const float rightAnchorX = row1.x - ctl::geom::ROW_PX;

            const ImVec2 tsz = calcText(ui.fontSans, geist::type::BODY, daemonTitle);
            const char* statusText = snap.daemon.loading
                ? "正在启动…"
                : (snap.daemon.running ? "已就绪 · 模型常驻显存" : "未开启 · 任务完成后释放显存");
            std::string statusBuf = statusText;
            if (!snap.daemon.loading && snap.daemon.running && snap.daemon.pid > 0) {
                char pidBuf[64];
                std::snprintf(pidBuf, sizeof(pidBuf), "已就绪 · 模型常驻显存 · PID %d", snap.daemon.pid);
                statusBuf = pidBuf;
            }
            const ImVec2 ssz = calcText(ui.fontSans, ctl::geom::ROW_SUB, statusBuf.c_str());
            const float contentH = tsz.y + 3.0f + ssz.y;
            const float textX = row0.x + ctl::geom::ROW_PX;
            const float textY = row0.y + (h3 - contentH) * 0.5f;
            const float subY = textY + tsz.y + 3.0f;
            const float subMidY = subY + ssz.y * 0.5f;
            const float maxTextW = std::max(40.0f, colW - ctl::geom::ROW_PX * 2.0f - 64.0f);

            ellipsisText(cdl, ui.fontSans, geist::type::BODY, ImVec2(textX, textY), maxTextW, t.ink, daemonTitle);
            if (snap.daemon.loading) {
                ctl::busyArc(cdl, snapPt(textX + 5.0f, subMidY), 4.2f, t.accent, 1.5f);
                ellipsisText(cdl, ui.fontSans, ctl::geom::ROW_SUB, ImVec2(textX + 15.0f, subY),
                             maxTextW - 15.0f, t.accent, statusBuf.c_str());
            } else {
                const ImU32 dotCol = snap.daemon.running ? t.accent : t.faint;
                cdl->AddCircleFilled(snapPt(textX + 3.5f, subMidY), 3.0f, dotCol);
                ellipsisText(cdl, ui.fontSans, ctl::geom::ROW_SUB, ImVec2(textX + 12.0f, subY),
                             maxTextW - 12.0f, snap.daemon.running ? t.body : t.mute, statusBuf.c_str());
            }

            bool daemonOn = snap.daemon.running || snap.daemon.loading;
            if (ctl::toggleSwitch(cdl, ImGui::GetID("sw-daemon"),
                                  snapPt(rightAnchorX - ctl::geom::TOGGLE_W,
                                         rowCenterY - ctl::geom::TOGGLE_H * 0.5f),
                                  daemonOn, !snap.daemon.loading)) {
                app.toggleDaemon();
            }
        }

        // 常驻时间
        const float r3Bottom = r2.bottomY + h3;
        ctl::cardDivider(cdl, card0.x, card1.x, r3Bottom);
        auto r4 = ctl::cardRow(cdl, ImGui::GetID("set-idle"), ImVec2(card0.x, r3Bottom), colW,
                               idleTitle, idleSub, ui.fontSans, false, 260.0f);
        {
            const char* idleLabels[4] = {"5分钟", "15分钟", "30分钟", "不退出"};
            const int idleVals[4] = {300, 900, 1800, 0};
            int idleIdx = 0;
            for (int i = 0; i < 4; ++i) {
                if (snap.daemon.idle_secs == idleVals[i]) idleIdx = i;
            }
            const float segW = 244.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-idle"),
                               snapPt(r4.rightAnchorX - segW, r4.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), idleLabels, 4, idleIdx, ui.fontSans, 12.0f)) {
                app.setDaemonIdleSecs(idleVals[idleIdx]);
            }
        }

        y = card1.y + geist::space::SECTION;
    }

    // 2. 默认编码与批处理
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "默认编码与批处理");
    y += 20.0f;
    {
        const bool isPng = (snap.format == "png");
        const bool isAvif = (snap.format == "avif");
        const bool showQuality = !isPng;
        const bool showChroma = (snap.format == "jpg" || snap.format == "avif");
        const bool showSpeed = isAvif;

        const char* fmtTitle = "默认输出格式";
        const char* fmtSub = "跟随原图格式或统一转换导出容器";
        const char* qTitle = "默认质量";
        const char* qSub = "作用于 JPG / WebP / AVIF 有损编码，可直接输入";
        const char* chromaTitle = "色度抽样";
        const char* chromaSub = "4:4:4 保留更多色彩边缘细节";
        const char* speedTitle = "编码速度";
        const char* speedSub = "越慢越精细，越快越省时";
        const char* pngTitle = "PNG 采用无损压缩";
        const char* pngSub = "不涉及质量与色度抽样参数";
        const char* recTitle = "文件夹包含子目录";
        const char* recSub = "递归扫描下级图片，自动跳过 upix_* 输出目录";
        const char* forceTitle = "忽略超分标记强制重跑";
        const char* forceSub = "重新处理已包含 Upix 超分标记的图片";

        const float hFmt = calcRowH(ui.fontSans, fmtTitle, fmtSub);
        const float hQ = showQuality ? (calcRowH(ui.fontSans, qTitle, qSub) + ctl::geom::SLIDER_H + 12.0f) : 0.0f;
        const float hChroma = showChroma ? calcRowH(ui.fontSans, chromaTitle, chromaSub) : 0.0f;
        const float hSpeed = showSpeed ? calcRowH(ui.fontSans, speedTitle, speedSub) : 0.0f;
        const float hPng = isPng ? calcRowH(ui.fontSans, pngTitle, pngSub) : 0.0f;
        const float hRec = calcRowH(ui.fontSans, recTitle, recSub);
        const float hForce = calcRowH(ui.fontSans, forceTitle, forceSub);

        const float totalH = hFmt + hQ + hChroma + hSpeed + hPng + hRec + hForce;
        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + totalH);
        ctl::groupCard(cdl, card0, card1);

        float curY = card0.y;

        // 默认输出格式
        {
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-fmt"), ImVec2(card0.x, curY), colW,
                                  fmtTitle, fmtSub, ui.fontSans, false, 260.0f);
            const char* fmtLabels[5] = {"跟随", "PNG", "JPG", "WebP", "AVIF"};
            const char* fmtVals[5] = {"", "png", "jpg", "webp", "avif"};
            int fmtIdx = 0;
            for (int i = 0; i < 5; ++i) {
                if (snap.format == fmtVals[i]) fmtIdx = i;
            }
            const float segW = 244.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-fmt"),
                               snapPt(r.rightAnchorX - segW, r.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), fmtLabels, 5, fmtIdx, ui.fontSans, 12.0f)) {
                app.setFormat(fmtVals[fmtIdx]);
            }
            curY = r.bottomY;
        }

        // Format-gated 默认质量 (qualityInput + magnetic 5-tick qualitySlider)
        if (showQuality) {
            ctl::cardDivider(cdl, card0.x, card1.x, curY);
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-quality"), ImVec2(card0.x, curY), colW,
                                  qTitle, qSub, ui.fontSans, false, 68.0f);
            int q = snap.quality;
            if (ctl::qualityInput(cdl, ImGui::GetID("qi-quality"),
                                  snapPt(r.rightAnchorX - 52.0f, r.centerY - 11.0f),
                                  q, 1, 100, ui.fontMono)) {
                app.setQuality(q);
            }
            const float sliderW = std::max(80.0f, colW - ctl::geom::ROW_PX * 2.0f);
            if (ctl::qualitySlider(cdl, ImGui::GetID("qs-quality"),
                                   snapPt(card0.x + ctl::geom::ROW_PX, r.bottomY),
                                   sliderW, q, 1, 100)) {
                app.setQuality(q);
            }
            curY += hQ;
        }

        // Format-gated 色度抽样
        if (showChroma) {
            ctl::cardDivider(cdl, card0.x, card1.x, curY);
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-chroma"), ImVec2(card0.x, curY), colW,
                                  chromaTitle, chromaSub, ui.fontSans, false, 152.0f);
            const char* cLabels[2] = {"4:4:4", "4:2:0"};
            const int cVals[2] = {444, 420};
            int cIdx = (snap.chroma == 444) ? 0 : 1;
            const float segW = 136.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-chroma"),
                               snapPt(r.rightAnchorX - segW, r.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), cLabels, 2, cIdx, ui.fontMono, 12.0f)) {
                app.setChroma(cVals[cIdx]);
            }
            curY = r.bottomY;
        }

        // Format-gated 编码速度
        if (showSpeed) {
            ctl::cardDivider(cdl, card0.x, card1.x, curY);
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-speed"), ImVec2(card0.x, curY), colW,
                                  speedTitle, speedSub, ui.fontSans, false, 190.0f);
            const char* sLabels[3] = {"精细", "均衡", "快速"};
            const int sVals[3] = {4, 6, 8};
            int sIdx = 1;
            for (int i = 0; i < 3; ++i) {
                if (snap.avif_speed == sVals[i]) sIdx = i;
            }
            const float segW = 174.0f;
            if (ctl::segmented(cdl, ImGui::GetID("seg-speed"),
                               snapPt(r.rightAnchorX - segW, r.centerY - ctl::geom::SEG_H * 0.5f),
                               ImVec2(segW, ctl::geom::SEG_H), sLabels, 3, sIdx, ui.fontSans, 12.0f)) {
                app.setAvifSpeed(sVals[sIdx]);
            }
            curY = r.bottomY;
        }

        // Calm PNG lossless row
        if (isPng) {
            ctl::cardDivider(cdl, card0.x, card1.x, curY);
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-png"), ImVec2(card0.x, curY), colW,
                                  pngTitle, pngSub, ui.fontSans);
            curY = r.bottomY;
        }

        // 文件夹包含子目录
        ctl::cardDivider(cdl, card0.x, card1.x, curY);
        {
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-rec"), ImVec2(card0.x, curY), colW,
                                  recTitle, recSub, ui.fontSans, false, 64.0f);
            bool rec = snap.recursive;
            if (ctl::toggleSwitch(cdl, ImGui::GetID("sw-rec"),
                                  snapPt(r.rightAnchorX - ctl::geom::TOGGLE_W,
                                         r.centerY - ctl::geom::TOGGLE_H * 0.5f),
                                  rec)) {
                app.setRecursive(rec);
            }
            curY = r.bottomY;
        }

        // 忽略超分标记强制重跑
        ctl::cardDivider(cdl, card0.x, card1.x, curY);
        {
            auto r = ctl::cardRow(cdl, ImGui::GetID("set-force"), ImVec2(card0.x, curY), colW,
                                  forceTitle, forceSub, ui.fontSans, false, 64.0f);
            bool force = snap.force;
            if (ctl::toggleSwitch(cdl, ImGui::GetID("sw-force"),
                                  snapPt(r.rightAnchorX - ctl::geom::TOGGLE_W,
                                         r.centerY - ctl::geom::TOGGLE_H * 0.5f),
                                  force)) {
                app.setForce(force);
            }
            curY = r.bottomY;
        }

        y = card1.y + geist::space::SECTION;
    }

    // 3. 交互与辅助
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "交互与辅助");
    y += 20.0f;
    {
        const char* rmTitle = "减少动态效果";
        const char* rmSub = "禁用弹簧缓动与滑块过渡，仅保留瞬时状态切换";
        const float h = calcRowH(ui.fontSans, rmTitle, rmSub);
        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + h);
        ctl::groupCard(cdl, card0, card1);

        auto r = ctl::cardRow(cdl, ImGui::GetID("set-motion"), card0, colW,
                              rmTitle, rmSub, ui.fontSans, false, 64.0f);
        bool rm = snap.reduce_motion;
        if (ctl::toggleSwitch(cdl, ImGui::GetID("sw-rm"),
                              snapPt(r.rightAnchorX - ctl::geom::TOGGLE_W,
                                     r.centerY - ctl::geom::TOGGLE_H * 0.5f),
                              rm)) {
            app.setReduceMotion(rm);
            motion::reduceMotion() = rm;
        }
        y = card1.y + geist::space::SECTION;
    }

    // 4. 目录与关于
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "目录与关于");
    y += 20.0f;
    {
        const char* dirTitle = "模型存储目录";
        const char* dirSub = "超分模型（.param / .bin）存放位置";
        const char* aboutTitle = "关于";

        const float hDirTop = calcRowH(ui.fontSans, dirTitle, dirSub);
        constexpr float kWellH = 30.0f;
        constexpr float kWellPadBottom = 14.0f;
        const float hDir = hDirTop + kWellH + kWellPadBottom;
        const float hAbout = calcRowH(ui.fontSans, aboutTitle, nullptr);

        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + hDir + hAbout);
        ctl::groupCard(cdl, card0, card1);

        auto rDir = ctl::cardRow(cdl, ImGui::GetID("set-dir"), card0, colW,
                                 dirTitle, dirSub, ui.fontSans, false, 108.0f);
        const float btnW = 88.0f;
        const float btnH = 30.0f;
        if (ctl::secondaryButton(cdl, ImGui::GetID("btn-open-mdir"), "打开目录", ui.fontSans,
                                 snapPt(rDir.rightAnchorX - btnW, rDir.centerY - btnH * 0.5f),
                                 snapPt(rDir.rightAnchorX, rDir.centerY + btnH * 0.5f))) {
            std::error_code ec;
            if (!snap.models_dir.empty()) {
                std::filesystem::create_directories(snap.models_dir, ec);
                SDL_OpenURL(("file://" + snap.models_dir).c_str());
            }
        }

        // Recessed mono path well spanning the card inner width
        {
            const ImVec2 w0 = snapPt(card0.x + ctl::geom::ROW_PX, rDir.bottomY);
            const ImVec2 w1 = snapPt(card1.x - ctl::geom::ROW_PX, rDir.bottomY + kWellH);
            ctl::finish(cdl, w0, w1, ctl::Finish::Inset, 8.0f);
            cdl->AddRect(w0, w1, t.outline, 8.0f, 0, hairlinePx());
            const float midY = (w0.y + w1.y) * 0.5f;
            drawIcon(cdl, "folder", ImVec2(w0.x + 16.0f, midY), 13.0f, t.mute, 1.5f);
            const char* dirPath = snap.models_dir.empty() ? "—" : snap.models_dir.c_str();
            const ImVec2 psz = calcText(ui.fontMono, 11.5f, dirPath);
            const float textX = w0.x + 28.0f;
            const float maxPathW = std::max(24.0f, w1.x - 10.0f - textX);
            ellipsisText(cdl, ui.fontMono, 11.5f, snapPt(textX, midY - psz.y * 0.5f),
                         maxPathW, t.body, dirPath);
        }

        const float aboutY = card0.y + hDir;
        ctl::cardDivider(cdl, card0.x, card1.x, aboutY);
        auto rAbout = ctl::cardRow(cdl, ImGui::GetID("set-about"), ImVec2(card0.x, aboutY), colW,
                                   "Upix", nullptr, ui.fontSans, false, 200.0f);
        {
            std::string verStr = "v";
            verStr += snap.version.empty() ? "0.2.0" : snap.version;
            verStr += " · ncnn · Vulkan";
            const ImVec2 vsz = calcText(ui.fontMono, geist::type::BODY_SMALL, verStr.c_str());
            drawText(cdl, ui.fontMono, geist::type::BODY_SMALL,
                     snapPt(rAbout.rightAnchorX - vsz.x, rAbout.centerY - vsz.y * 0.5f),
                     t.mute, verStr.c_str());
        }

        y = card1.y + kPadY;
    }

    ImGui::SetCursorScreenPos(ImVec2(paneX, y));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
}

} // namespace upix::views
