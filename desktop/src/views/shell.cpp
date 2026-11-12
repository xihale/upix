#include "views/common.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace upix::views {
namespace {

// ctl::selectionSpring / ctl::switchKnob / ctl::progressSpring all route through
// motion::springTo, whose Spring1D starts at 1.0 and only adopts `defaultValue`
// after 120 dormant frames. A nav pill would therefore boot at y=1 and still
// be sliding in the frame-3 screenshots. This seeds at the target (no entrance)
// and uses the same integrator + the same {360, 30} pair as ctl::selectionSpring.
float followSpring(ImGuiID id, float target, upix::ui::motion::SpringParams params) {
    struct Slot {
        upix::ui::motion::Spring1D s;
        int frame = -1;
        bool live = false;
    };
    static std::unordered_map<ImGuiID, Slot> store;
    Slot& slot = store[id];
    const int frame = ImGui::GetFrameCount();
    if (!slot.live || slot.frame + 120 < frame) {
        slot.s.value = target;
        slot.s.velocity = 0.0f;
        slot.live = true;
    }
    slot.frame = frame;
    return upix::ui::motion::advance(slot.s, target, params, ImGui::GetIO().DeltaTime);
}

// Selected ink promotion over motion::FAST (180ms). Seeded so the resting
// tab is already ink — the fade only runs when the selection changes.
float easeTo(ImGuiID id, float target, float durMs) {
    struct Slot {
        float v = 0.0f;
        int frame = -1;
        bool live = false;
    };
    static std::unordered_map<ImGuiID, Slot> store;
    Slot& slot = store[id];
    const int frame = ImGui::GetFrameCount();
    if (!slot.live || slot.frame + 120 < frame) {
        slot.v = target;
        slot.live = true;
    }
    slot.frame = frame;
    const float step = ImGui::GetIO().DeltaTime / (durMs / 1000.0f);
    if (slot.v < target) slot.v = std::min(target, slot.v + step);
    else slot.v = std::max(target, slot.v - step);
    return slot.v;
}

} // namespace

void renderNavRail(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    // 76px rail. Three destinations only — 放大 / 模型 / 远程 — same clicks
    // as before (ui.activeTab). Guide/settings live on the rust shell but
    // this desktop has no tabs for them.
    const float railW = 76.0f;
    const float item = 60.0f;
    const float gap = 2.0f;
    const float inset = 8.0f; // pill inset from the rail edges
    const float top = 16.0f;
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(railW, winSize.y), t.bg);

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

    int active = 0;
    for (int i = 0; i < 3; ++i) {
        if (ui.activeTab == items[i].idx) active = i;
    }

    // Pill-finish capsule. Springs only after the selection actually moves.
    const float targetY = top + static_cast<float>(active) * (item + gap);
    const float pillY = followSpring(ImGui::GetID("nav-pill"), targetY, motion::SpringParams{360.0f, 30.0f});
    const ImVec2 pill0 = snapPt(inset, pillY);
    const ImVec2 pill1 = snapPt(inset + item, pillY + item);
    ctl::finish(dl, pill0, pill1, ctl::Finish::Pill, 10.0f);

    for (int i = 0; i < 3; ++i) {
        const ImVec2 pos = snapPt(inset, top + static_cast<float>(i) * (item + gap));
        const ImVec2 sz(item, item);
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##nav", sz);
        const bool hov = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) ui.activeTab = items[i].idx;

        // Press: scale 0.95, FAST spring (PRESS 520/34). Hover wash is instant.
        float sc = motion::pressScale(ImGui::GetID("press"), held, 0.95f);
        if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
        const ImVec2 c(pos.x + item * 0.5f, pos.y + item * 0.5f);
        auto S = [&](ImVec2 p) {
            return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
        };

        if (hov) {
            const ImVec2 w0 = S(pos);
            const ImVec2 w1 = S(ImVec2(pos.x + item, pos.y + item));
            // 8% highlight wash. INSTANT — not a fade.
            dl->AddRectFilled(w0, w1, hex(0xFFFFFF, 0x14), 10.0f * sc);
        }

        const bool selected = (ui.activeTab == items[i].idx);
        const float promo = easeTo(ImGui::GetID("ink"), selected ? 1.0f : 0.0f, motion::FAST);
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

    // Version stamp, 10px, bottom padding 16. Not a destination.
    if (!snap.version.empty()) {
        const ImVec2 vs = calcText(ui.fontSans, 10.0f, snap.version.c_str());
        const float ty = winSize.y - 16.0f - vs.y;
        drawText(dl, ui.fontSans, 10.0f, snapPt((railW - vs.x) * 0.5f, ty), t.faint, snap.version.c_str());
    }
}

} // namespace upix::views
