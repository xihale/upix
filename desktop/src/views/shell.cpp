#include "views/common.hpp"

#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace upix::views {

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& pal = ui.pal;
    float railW = 76.0f;

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(railW, winSize.y), pal.bg());

    struct NavItem {
        const char* label;
        const char* icon;
        int idx;
    };
    const NavItem items[3] = {
        {"放大", "zoom-in", 0},
        {"模型", "layers", 1},
        {"远程", "smartphone", 2},
    };

    // Figma: padding-top 12px, item 60x60, gap 2px, active fill #26262c (NO border), radius 10px, icon 20x20, label 11px
    float startY = 12.0f;
    for (int i = 0; i < 3; ++i) {
        ImVec2 pos = snapPt(8.0f, startY + i * (60.0f + 2.0f));
        ImVec2 sz(60.0f, 60.0f);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##nav", sz);
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();

        if (clicked) ui.activeTab = items[i].idx;
        bool active = (ui.activeTab == items[i].idx);

        if (active) {
            drawBox(dl, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), pal.high(), 0, 10.0f);
        } else if (hovered) {
            drawBox(dl, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), pal.containerLow(), 0, 10.0f);
        }

        ImU32 iconCol = active ? pal.primary() : (hovered ? pal.onSurface() : pal.muted());
        ImU32 textCol = active ? pal.onSurface() : (hovered ? pal.onSurface() : pal.muted());
        drawIcon(dl, items[i].icon, ImVec2(pos.x + sz.x * 0.5f, pos.y + 21.0f), 20.0f, iconCol);
        drawTextCentered(dl, ui.fontSans, 11.0f, ImVec2(pos.x, pos.y + 34.0f), ImVec2(pos.x + sz.x, pos.y + 52.0f), textCol, items[i].label);
    }

    // Bottom version label ("0.2.0" — Figma: 10px #a2a2ac, bottom padding 16px)
    drawTextCentered(dl, ui.fontSans, 10.0f, ImVec2(0, winSize.y - 30.0f), ImVec2(railW, winSize.y - 16.0f), pal.muted(), snap.version.c_str());
}



} // namespace upix::views
