#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>

namespace upix::views {

void renderGuidePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                     const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float railW = 76.0f;
    const float pad = 28.0f;
    const float colW = std::min(760.0f, std::max(360.0f, winSize.x - railW - pad * 2.0f));
    const float x0 = snapPx(railW + (winSize.x - railW - colW) * 0.5f);
    float y = 28.0f;

    drawText(dl, ui.fontSans, geist::type::SUBHEADING, snapPt(x0, y), t.ink, "使用指南");
    y += 28.0f;
    drawText(dl, ui.fontSans, geist::type::BODY_SMALL, snapPt(x0, y), t.mute,
             "Upix 本地超分辨率工作流、模型选型建议与键盘快捷键。");
    y += 28.0f;

    // 1. 快速上手
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "快速上手");
    y += 22.0f;
    {
        const ImVec2 card0 = snapPt(x0, y);
        const ImVec2 card1 = snapPt(x0 + colW, y + 170.0f);
        ctl::groupCard(dl, card0, card1);

        auto r1 = ctl::cardRow(dl, ImGui::GetID("g-1"), card0, colW,
                               "1. 放入图片或文件夹", "直接拖拽文件/目录到工作台，或按 Ctrl+V 粘贴剪贴板图片", ui.fontSans);
        ctl::cardDivider(dl, card0.x, card1.x, r1.bottomY);
        auto r2 = ctl::cardRow(dl, ImGui::GetID("g-2"), ImVec2(card0.x, r1.bottomY), colW,
                               "2. 选择模型与输出格式", "支持跟随原格式或转换为 PNG / JPG / WebP / AVIF，也可开启覆盖原图", ui.fontSans);
        ctl::cardDivider(dl, card0.x, card1.x, r2.bottomY);
        ctl::cardRow(dl, ImGui::GetID("g-3"), ImVec2(card0.x, r2.bottomY), colW,
                     "3. 拖拽对比与查看细节", "完成后拖动舞台中间的对比竖线，使用滚轮缩放或切换 1:1 像素视图", ui.fontSans);

        y = card1.y + geist::space::SECTION;
    }

    // 2. 模型选型速查
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "模型场景对照");
    y += 22.0f;
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
        const ImVec2 card0 = snapPt(x0, y);
        const float rowH = 52.0f;
        const ImVec2 card1 = snapPt(x0 + colW, y + rowH * 4.0f);
        ctl::groupCard(dl, card0, card1);

        float ry = y;
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ctl::cardDivider(dl, card0.x, card1.x, ry);
            const float cy = ry + rowH * 0.5f;
            dl->AddCircleFilled(snapPt(card0.x + 20.0f, cy), 4.0f, ctl::sceneDotColor(rows[i].scene));
            drawText(dl, ui.fontMono, geist::type::BODY, snapPt(card0.x + 34.0f, ry + 8.0f), t.ink, rows[i].name);
            drawText(dl, ui.fontSans, geist::type::LABEL, snapPt(card0.x + 34.0f, ry + 28.0f), t.mute, rows[i].note);
            ctl::badge(dl, ui.fontSans, snapPt(card1.x - 52.0f, cy - 10.0f), rows[i].scene, t.body, t.tagBg);
            ry += rowH;
        }
        y = card1.y + geist::space::SECTION;
    }

    // 3. 快捷键
    ctl::sectionHeader(dl, ui.fontSans, snapPt(x0, y), "快捷键");
    y += 22.0f;
    {
        struct KeyRow {
            const char* desc;
            const char* keys[2];
            int count;
        };
        const KeyRow krows[4] = {
            {"切换页面（放大 / 模型 / 远程 / 指南 / 设置）", {"Ctrl", "1..5"}, 2},
            {"从剪贴板粘贴图片或文件路径", {"Ctrl", "V"}, 2},
            {"立即中止当前超分任务", {"Ctrl", "."}, 2},
            {"关闭弹层或取消当前等待操作", {"Esc", nullptr}, 1},
        };
        const float rowH = 44.0f;
        const ImVec2 card0 = snapPt(x0, y);
        const ImVec2 card1 = snapPt(x0 + colW, y + rowH * 4.0f);
        ctl::groupCard(dl, card0, card1);

        float ry = y;
        for (int i = 0; i < 4; ++i) {
            if (i > 0) ctl::cardDivider(dl, card0.x, card1.x, ry);
            auto r = ctl::cardRow(dl, ImGui::GetID(1000 + i), ImVec2(card0.x, ry), colW,
                                  krows[i].desc, nullptr, ui.fontSans, false, 140.0f);
            float kx = r.rightAnchorX;
            const float ky = r.centerY - ctl::geom::KBD_H * 0.5f;
            for (int k = krows[i].count - 1; k >= 0; --k) {
                const ImVec2 ksz = calcText(ui.fontMono, ctl::geom::LABEL, krows[i].keys[k]);
                const float kw = ksz.x + ctl::geom::KBD_PX * 2.0f;
                kx -= kw;
                ctl::kbd(dl, ui.fontMono, krows[i].keys[k], ImVec2(kx, ky));
                kx -= 6.0f;
            }
            ry += rowH;
        }
        (void)snap;
    }
}

} // namespace upix::views
