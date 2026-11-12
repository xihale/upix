#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace upix::views {

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float railW = 76.0f;
    const float item = 60.0f;
    const float gap = 2.0f;
    const float inset = 8.0f;
    const float top = 16.0f;
    const float bottomPad = 16.0f;

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(railW, winSize.y), t.bg);
    ctl::vHairline(dl, railW - hairlinePx(), 0.0f, winSize.y, t.outline);

    struct NavItem {
        const char* label;
        const char* icon;
        int idx;
        bool footer;
    };
    const NavItem items[5] = {
        {"放大", "zoom-in", 0, false},
        {"模型", "layers", 1, false},
        {"远程", "smartphone", 2, false},
        {"指南", "book", 3, true},
        {"设置", "gear", 4, true},
    };

    const float verHeight = snap.version.empty() ? 0.0f : (calcText(ui.fontSans, 10.0f, snap.version.c_str()).y + 10.0f);
    const float footerStart = winSize.y - bottomPad - verHeight - (item * 2.0f + gap);

    auto itemPosY = [&](int k) -> float {
        if (!items[k].footer) {
            return top + static_cast<float>(k) * (item + gap);
        }
        return footerStart + static_cast<float>(k - 3) * (item + gap);
    };

    int active = 0;
    for (int i = 0; i < 5; ++i) {
        if (ui.activeTab == items[i].idx) active = i;
    }

    const float targetY = itemPosY(active);
    const float pillY = ctl::selectionSpring(ImGui::GetID("nav-pill-y"), targetY);
    const ImVec2 pill0 = snapPt(inset, pillY);
    const ImVec2 pill1 = snapPt(inset + item, pillY + item);
    ctl::finish(dl, pill0, pill1, ctl::Finish::Pill, 10.0f);

    for (int i = 0; i < 5; ++i) {
        const ImVec2 pos = snapPt(inset, itemPosY(i));
        const ImVec2 sz(item, item);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##nav", sz);
        const bool hov = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) ui.activeTab = items[i].idx;

        float sc = motion::pressScale(ImGui::GetID("press"), held, 0.95f);
        if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
        const ImVec2 c(pos.x + item * 0.5f, pos.y + item * 0.5f);
        auto S = [&](ImVec2 p) {
            return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
        };

        if (hov) {
            const ImVec2 w0 = S(pos);
            const ImVec2 w1 = S(ImVec2(pos.x + item, pos.y + item));
            ctl::hoverWash(dl, w0, w1, 10.0f * sc, true, ctl::geom::HOVER_WASH);
        }

        const bool selected = (ui.activeTab == items[i].idx);
        const float promo = motion::approach(ImGui::GetID("ink"), selected ? 1.0f : 0.0f, motion::FAST, selected ? 1.0f : 0.0f);
        const ImU32 ink = motion::mix(t.mute, t.ink, promo);

        const float iconSz = 20.0f;
        const float labelPx = 11.0f;
        const ImVec2 ts = calcText(ui.fontSans, labelPx, items[i].label);
        const float stack = iconSz + 3.0f + ts.y;
        const float stackTop = (item - stack) * 0.5f;
        const ImVec2 iconC = S(ImVec2(pos.x + item * 0.5f, pos.y + stackTop + iconSz * 0.5f));
        drawIcon(dl, items[i].icon, iconC, iconSz * sc, ink, 1.6f);
        const ImVec2 textP = S(ImVec2(pos.x + (item - ts.x) * 0.5f, pos.y + stackTop + iconSz + 3.0f));
        drawText(dl, ui.fontSans, labelPx * sc, textP, ink, items[i].label);
        ImGui::PopID();
    }

    if (!snap.version.empty()) {
        const ImVec2 vs = calcText(ui.fontSans, 10.0f, snap.version.c_str());
        const float ty = winSize.y - bottomPad - vs.y;
        drawText(dl, ui.fontSans, 10.0f, snapPt((railW - vs.x) * 0.5f, ty), t.faint, snap.version.c_str());
    }
}

} // namespace upix::views
