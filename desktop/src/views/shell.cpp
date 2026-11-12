#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace upix::views {

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool dark = geist::mode() == geist::Mode::Dark;

    constexpr float railW = 76.0f;
    constexpr float item = 60.0f;
    constexpr float gap = 2.0f;
    constexpr float inset = (railW - item) * 0.5f; // 8.0f
    constexpr float top = 16.0f;
    constexpr float bottomPad = 16.0f;

    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(railW, winSize.y), t.bg);
    ctl::vHairline(dl, railW - hairlinePx(), 0.0f, winSize.y, t.outline);

    struct NavItem {
        const char* label;
        const char* tip;
        const char* icon;
        int idx;
        bool footer;
    };
    const NavItem items[5] = {
        {"放大", "放大 (Ctrl+1)", "zoom-in", 0, false},
        {"模型", "模型 (Ctrl+2)", "layers", 1, false},
        {"远程", "远程 (Ctrl+3)", "smartphone", 2, false},
        {"指南", "指南 (Ctrl+4)", "book", 3, true},
        {"设置", "设置 (Ctrl+5)", "gear", 4, true},
    };

    const float verHeight = snap.version.empty()
        ? 0.0f
        : (calcText(ui.fontMono, 10.0f, snap.version.c_str()).y + 10.0f);
    const float minFooterY = top + 3.0f * (item + gap) + 12.0f;
    const float footerStart = std::max(minFooterY, winSize.y - bottomPad - verHeight - (item * 2.0f + gap));

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

    // Top 3 primary destinations use selectionSpring; footer utility destinations
    // use a calm static active pill so switching across the gap never jumps.
    const float pillY = (active < 3)
        ? ctl::selectionSpring(ImGui::GetID("nav-pill-y"), itemPosY(active))
        : itemPosY(active);
    {
        const ImVec2 pill0 = snapPt(inset, pillY);
        const ImVec2 pill1 = snapPt(inset + item, pillY + item);
        ctl::finish(dl, pill0, pill1, ctl::Finish::Pill, 10.0f);
        dl->AddRect(pill0, pill1, withAlpha(t.ink, dark ? 0.08f : 0.06f), 10.0f, 0, hairlinePx());
    }

    for (int i = 0; i < 5; ++i) {
        const ImVec2 pos = snapPt(inset, itemPosY(i));
        const ImVec2 sz(item, item);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(pos);
        const bool clicked = ImGui::InvisibleButton("##nav", sz);
        const bool hov = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (clicked) ui.activeTab = items[i].idx;

        float sc = motion::pressScale(ImGui::GetID("press"), held, 0.95f);
        if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
        const ImVec2 c(pos.x + item * 0.5f, pos.y + item * 0.5f);
        auto S = [&](ImVec2 p) {
            return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
        };

        const bool selected = (ui.activeTab == items[i].idx);
        if (hov && !selected) {
            const ImVec2 w0 = S(pos);
            const ImVec2 w1 = S(ImVec2(pos.x + item, pos.y + item));
            ctl::hoverWash(dl, w0, w1, 10.0f * sc, true, ctl::geom::HOVER_WASH);
        }

        const float promo = motion::approach(ImGui::GetID("ink"), selected ? 1.0f : 0.0f, motion::FAST, selected ? 1.0f : 0.0f);
        const ImU32 ink = motion::mix(t.mute, t.ink, promo);

        constexpr float iconSz = 20.0f;
        constexpr float labelPx = 11.0f;
        constexpr float iconGap = 4.0f;
        const ImVec2 ts = calcText(ui.fontSans, labelPx, items[i].label);
        const float stack = iconSz + iconGap + ts.y;
        const float stackTop = (item - stack) * 0.5f;
        const ImVec2 iconC = snapPt(S(ImVec2(pos.x + item * 0.5f, pos.y + stackTop + iconSz * 0.5f)));
        drawIcon(dl, items[i].icon, iconC, iconSz * sc, ink, 1.6f);
        const ImVec2 textP = snapPt(S(ImVec2(pos.x + (item - ts.x) * 0.5f, pos.y + stackTop + iconSz + iconGap)));
        drawText(dl, ui.fontSans, labelPx * sc, textP, ink, items[i].label);

        ctl::tooltip(dl, ImGui::GetID("tip"), pos, ImVec2(pos.x + item, pos.y + item),
                     items[i].tip, ui.fontSans, hov, focused);
        ImGui::PopID();
    }

    if (!snap.version.empty()) {
        const ImVec2 vs = calcText(ui.fontMono, 10.0f, snap.version.c_str());
        const float ty = winSize.y - bottomPad - vs.y;
        drawText(dl, ui.fontMono, 10.0f, snapPt((railW - vs.x) * 0.5f, ty), t.faint, snap.version.c_str());
    }
}

} // namespace upix::views
