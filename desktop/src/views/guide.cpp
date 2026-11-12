#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>

namespace upix::views {
namespace {

float calcGuideRowH(ImFont* titleFont, float titlePx, const char* title,
                    ImFont* subFont, float subPx, const char* sub, float minH = 44.0f) {
    using namespace upix::ui;
    const ImVec2 tsz = calcText(titleFont, titlePx, title ? title : "");
    const bool hasSub = sub && sub[0];
    const ImVec2 ssz = hasSub ? calcText(subFont, subPx, sub) : ImVec2(0.0f, 0.0f);
    const float contentH = hasSub ? (tsz.y + 4.0f + ssz.y) : tsz.y;
    return std::max(minH, contentH + ctl::geom::ROW_PY * 2.0f);
}

} // namespace

void renderGuidePage(ImDrawList* /*dl*/, ImVec2 winSize, UiViewState& ui,
                     const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    (void)snap;

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

    ImGui::BeginChild("##guide_pane", ImVec2(paneW, winSize.y), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    const float scrollY = ImGui::GetScrollY();
    float y = snapPx(kPadY - scrollY);

    // Page header
    {
        const ImVec2 hsz = calcText(ui.fontSans, geist::type::SUBHEADING, "使用指南");
        drawText(cdl, ui.fontSans, geist::type::SUBHEADING, snapPt(colX, y), t.ink, "使用指南");
        y += hsz.y + 4.0f;
        const ImVec2 ssz = calcText(ui.fontSans, geist::type::BODY_SMALL,
                                    "Upix 本地超分辨率工作流、模型选型与快捷键参考。");
        drawText(cdl, ui.fontSans, geist::type::BODY_SMALL, snapPt(colX, y), t.mute,
                 "Upix 本地超分辨率工作流、模型选型与快捷键参考。");
        y += ssz.y + geist::space::XL;
    }

    // 1. 三步上手
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "三步上手");
    y += 20.0f;
    {
        struct StepRow {
            const char* num;
            const char* title;
            const char* sub;
        };
        const StepRow steps[3] = {
            {"1", "放入图片或文件夹", "直接拖拽文件或目录到工作台，或按 Ctrl+V 粘贴剪贴板图片"},
            {"2", "选择模型与输出格式", "支持跟随原格式或转换为 PNG / JPG / WebP / AVIF，也可开启覆盖原图"},
            {"3", "开始超分并对比细节", "完成后拖动舞台中间的对比竖线，使用滚轮缩放或切换 1:1 像素视图"},
        };
        float rowHeights[3];
        float totalH = 0.0f;
        for (int i = 0; i < 3; ++i) {
            rowHeights[i] = calcGuideRowH(ui.fontSans, geist::type::BODY, steps[i].title,
                                          ui.fontSans, ctl::geom::ROW_SUB, steps[i].sub, 48.0f);
            totalH += rowHeights[i];
        }

        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + totalH);
        ctl::groupCard(cdl, card0, card1);

        float ry = card0.y;
        for (int i = 0; i < 3; ++i) {
            if (i > 0) ctl::cardDivider(cdl, card0.x, card1.x, ry);
            const float rh = rowHeights[i];
            const ImVec2 tsz = calcText(ui.fontSans, geist::type::BODY, steps[i].title);
            const ImVec2 ssz = calcText(ui.fontSans, ctl::geom::ROW_SUB, steps[i].sub);
            const float contentH = tsz.y + 4.0f + ssz.y;
            const float textY = snapPx(ry + (rh - contentH) * 0.5f);
            const float titleMidY = textY + tsz.y * 0.5f;

            // Crisp 22x22 r6 recessed mono step badge aligned with the step title line
            constexpr float badgeSz = 22.0f;
            const ImVec2 b0 = snapPt(card0.x + ctl::geom::ROW_PX, titleMidY - badgeSz * 0.5f);
            const ImVec2 b1 = snapPt(b0.x + badgeSz, b0.y + badgeSz);
            ctl::finish(cdl, b0, b1, ctl::Finish::Inset, geist::radius::MD);
            cdl->AddRect(b0, b1, t.outline, geist::radius::MD, 0, hairlinePx());
            drawTextCentered(cdl, ui.fontMono, ctl::geom::LABEL, b0, b1, t.accent, steps[i].num);

            const float textX = b1.x + 12.0f;
            const float maxTextW = std::max(40.0f, card1.x - ctl::geom::ROW_PX - textX);
            ellipsisText(cdl, ui.fontSans, geist::type::BODY, ImVec2(textX, textY), maxTextW, t.ink, steps[i].title);
            ellipsisText(cdl, ui.fontSans, ctl::geom::ROW_SUB, ImVec2(textX, textY + tsz.y + 4.0f),
                         maxTextW, t.mute, steps[i].sub);

            ry += rh;
        }
        y = card1.y + geist::space::SECTION;
    }

    // 2. 模型场景对照
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "模型场景对照");
    y += 20.0f;
    {
        struct ModelGuideRow {
            const char* name;
            const char* scene;
            const char* note;
        };
        const ModelGuideRow rows[4] = {
            {"AnimeSharp-x4 / Real-ESRGANv3-anime", "动漫", "二次元插画、赛璐珞线条锐化与纯净色块放大"},
            {"UltraSharp-x4 / Remacri-x4", "照片", "摄影纹理、建筑与自然风光的高频细节重建"},
            {"BSRGAN-x4 / DeJPG-RealPLKSR", "修复", "重度压缩、模糊噪点图与老旧截图降噪修复"},
            {"Real-ESRGANv3-general-x4", "综合", "混合图文、UI 截图与通用日常素材均衡输出"},
        };

        float rowHeights[4];
        float totalH = 0.0f;
        for (int i = 0; i < 4; ++i) {
            rowHeights[i] = calcGuideRowH(ui.fontMono, geist::type::BODY, rows[i].name,
                                          ui.fontSans, ctl::geom::ROW_SUB, rows[i].note, 48.0f);
            totalH += rowHeights[i];
        }

        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + totalH);
        ctl::groupCard(cdl, card0, card1);

        float ry = card0.y;
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ctl::cardDivider(cdl, card0.x, card1.x, ry);
            const float rh = rowHeights[i];

            const ImVec2 nsz = calcText(ui.fontMono, geist::type::BODY, rows[i].name);
            const ImVec2 dsz = calcText(ui.fontSans, ctl::geom::ROW_SUB, rows[i].note);
            const float contentH = nsz.y + 4.0f + dsz.y;
            const float titleY = snapPx(ry + (rh - contentH) * 0.5f);
            const float noteY = snapPx(titleY + nsz.y + 4.0f);

            // Right-aligned scene badge chip
            const ImVec2 chipTs = calcText(ui.fontSans, ctl::geom::LABEL, rows[i].scene);
            const float chipW = chipTs.x + 16.0f;
            constexpr float chipH = 20.0f;
            const ImVec2 c0 = snapPt(card1.x - ctl::geom::ROW_PX - chipW, ry + (rh - chipH) * 0.5f);
            const ImVec2 c1 = snapPt(c0.x + chipW, c0.y + chipH);
            cdl->AddRectFilled(c0, c1, t.container, geist::radius::SM);
            cdl->AddRect(c0, c1, t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(cdl, ui.fontSans, ctl::geom::LABEL, c0, c1, t.body, rows[i].scene);

            // 7px colored scene dot aligned directly with the mono model-family title line
            constexpr float dotDia = 7.0f;
            const float dotX = card0.x + ctl::geom::ROW_PX + dotDia * 0.5f;
            const float dotY = titleY + nsz.y * 0.5f;
            cdl->AddCircleFilled(snapPt(dotX, dotY), dotDia * 0.5f, ctl::sceneDotColor(rows[i].scene));

            const float textX = card0.x + ctl::geom::ROW_PX + dotDia + 9.0f;
            const float maxTextW = std::max(40.0f, c0.x - 12.0f - textX);
            ellipsisText(cdl, ui.fontMono, geist::type::BODY, ImVec2(textX, titleY), maxTextW, t.ink, rows[i].name);
            ellipsisText(cdl, ui.fontSans, ctl::geom::ROW_SUB, ImVec2(textX, noteY), maxTextW, t.mute, rows[i].note);

            ry += rh;
        }
        y = card1.y + geist::space::SECTION;
    }

    // 3. 快捷键
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "快捷键");
    y += 20.0f;
    {
        struct KeyRow {
            const char* desc;
            const char* keys[2];
            int count;
            bool sansKey;
        };
        const KeyRow krows[5] = {
            {"切换页面（放大 / 模型 / 远程 / 指南 / 设置）", {"Ctrl", "1…5"}, 2, false},
            {"从剪贴板粘贴图片或文件路径", {"Ctrl", "V"}, 2, false},
            {"立即中止当前超分任务", {"Ctrl", "."}, 2, false},
            {"关闭弹层或拒绝配对请求", {"Esc", nullptr}, 1, false},
            {"舞台图像缩放（配合拖拽平移）", {"滚轮", nullptr}, 1, true},
        };
        constexpr float rowH = 44.0f;
        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + rowH * 5.0f);
        ctl::groupCard(cdl, card0, card1);

        const ImVec2 plusSz = calcText(ui.fontMono, ctl::geom::LABEL, "+");
        float ry = card0.y;
        for (int i = 0; i < 5; ++i) {
            if (i > 0) ctl::cardDivider(cdl, card0.x, card1.x, ry);
            auto r = ctl::cardRow(cdl, ImGui::GetID(1000 + i), ImVec2(card0.x, ry), colW,
                                  krows[i].desc, nullptr, ui.fontSans, false, 148.0f);
            float kx = r.rightAnchorX;
            const float ky = r.centerY - ctl::geom::KBD_H * 0.5f;
            for (int k = krows[i].count - 1; k >= 0; --k) {
                ImFont* kFont = krows[i].sansKey ? ui.fontSans : ui.fontMono;
                const ImVec2 ksz = calcText(kFont, ctl::geom::LABEL, krows[i].keys[k]);
                const float kw = ksz.x + ctl::geom::KBD_PX * 2.0f;
                kx -= kw;
                ctl::kbd(cdl, kFont, krows[i].keys[k], ImVec2(kx, ky));
                if (k > 0) {
                    kx -= 4.0f + plusSz.x;
                    drawText(cdl, ui.fontMono, ctl::geom::LABEL,
                             snapPt(kx, r.centerY - plusSz.y * 0.5f), t.faint, "+");
                    kx -= 4.0f;
                }
            }
            ry += rowH;
        }
        y = card1.y + geist::space::SECTION;
    }

    // 4. 还没有模型？
    ctl::sectionHeader(cdl, ui.fontSans, snapPt(colX, y), "还没有模型？");
    y += 20.0f;
    {
        const char* title = "前往模型页挑选或导入";
        const char* sub = "首次使用可前往模型页在线下载推荐模型，或导入本地 ncnn 权重文件";
        const float rh = calcGuideRowH(ui.fontSans, geist::type::BODY, title,
                                       ui.fontSans, ctl::geom::ROW_SUB, sub, 56.0f);
        const ImVec2 card0 = snapPt(colX, y);
        const ImVec2 card1 = snapPt(colX + colW, y + rh);
        ctl::groupCard(cdl, card0, card1);

        constexpr float btnW = 100.0f;
        constexpr float btnH = 30.0f;
        auto r = ctl::cardRow(cdl, ImGui::GetID("guide-models"), card0, colW,
                              title, sub, ui.fontSans, false, btnW + 16.0f);
        if (ctl::secondaryButton(cdl, ImGui::GetID("guide-open-models"), "打开模型页", ui.fontSans,
                                 snapPt(r.rightAnchorX - btnW, r.centerY - btnH * 0.5f),
                                 snapPt(r.rightAnchorX, r.centerY + btnH * 0.5f))) {
            ui.activeTab = 1;
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
