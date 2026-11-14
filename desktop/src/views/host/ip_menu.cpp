// views/host/ip_menu.cpp — floating host IP selector popup laid out via flex::Layout.
#include "views/host/internal.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace upix::views::host_detail {

void drawIpMenuPopup(UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     ImVec2 trig0, ImVec2 trig1, ImVec2 winSize) {
    using namespace upix::ui;
    if (!ui.showIpPicker || snap.host.ips.empty()) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showIpPicker = false;
        return;
    }

    struct IpRowSlots {
        flex::Container row;
        flex::Slot dot;
        flex::Slot ip;
        flex::Slot kind;
        flex::Slot check;
    };

    const auto& t = geist::tokens();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float pad = 4.0f;
    const float monoH = std::ceil(calcText(ui.fontMono, geist::type::MONO, "0").y);
    const float rowH = std::max(36.0f, std::max(monoH, ctl::geom::META_CHIP_H) + 10.0f);
    const float gap = 2.0f;
    const size_t count = snap.host.ips.size();
    const float menuW = std::min(std::max(trig1.x - trig0.x, 248.0f), std::max(180.0f, winSize.x - 16.0f));

    static flex::Context menuCtx;
    flex::Layout menuFx(menuCtx);
    auto menuCol = menuFx.col({
        .padding = flex::Edges::all(pad),
        .gap = gap,
        .align = flex::Align::Stretch,
        .width = flex::Fixed(menuW),
    });

    std::vector<IpRowSlots> rows;
    rows.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const std::string& ip = snap.host.ips[i];
        const char* kind = ipKind(ip);
        const float kindW = kind[0] ? ctl::metaChipWidth(ui.fontMono, kind, false, 5.0f) : 0.0f;
        const ImVec2 ipSz = calcText(ui.fontMono, geist::type::MONO, ip.c_str());

        auto r = menuCol.row({
            .padding = flex::Edges::xy(8.0f, 0.0f),
            .gap = 8.0f,
            .align = flex::Align::Center,
            .width = flex::Grow(1.0f),
            .height = flex::Fixed(rowH),
        });
        const flex::Slot dotSlot = r.slot(flex::Fixed(8.0f), flex::Fixed(8.0f), flex::AlignSelf::Center);
        const flex::Slot ipSlot = r.slot(flex::Grow(1.0f).min(40.0f), flex::Fixed(ipSz.y), flex::AlignSelf::Center);
        const flex::Slot kindSlot = r.slot(flex::Fixed(kindW), flex::Fixed(ctl::geom::META_CHIP_H),
                                           flex::AlignSelf::Center, {}, kind[0] != '\0');
        const flex::Slot checkSlot = r.slot(flex::Fixed(14.0f), flex::Fixed(14.0f), flex::AlignSelf::Center);
        rows.push_back({r, dotSlot, ipSlot, kindSlot, checkSlot});
    }

    const flex::Rect measured = menuFx.solveWidth(ImVec2(0.0f, 0.0f), menuW);
    const float menuH = measured.h;

    const float mx0 = std::clamp(trig1.x - menuW, 8.0f, std::max(8.0f, winSize.x - menuW - 8.0f));
    float my0 = trig1.y + 6.0f;
    if (my0 + menuH > winSize.y - 12.0f && trig0.y - 6.0f - menuH >= kTopbarH + 12.0f) {
        my0 = trig0.y - 6.0f - menuH;
    }

    const flex::Rect menuRect = menuFx.solveWidth(snapPt(mx0, my0), menuW);
    const ImVec2 m0 = menuRect.p0();
    const ImVec2 m1 = menuRect.p1();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ctl::mouseIn(m0, m1) && !ctl::mouseIn(trig0, trig1)) {
        ui.showIpPicker = false;
        return;
    }

    ctl::finish(fg, m0, m1, ctl::Finish::Menu, 8.0f, false);

    for (size_t i = 0; i < count; ++i) {
        const std::string& ip = snap.host.ips[i];
        const bool isSel = (static_cast<int>(i) == snap.host.selected_ip_idx);
        const IpRowSlots& rs = rows[i];
        const flex::Rect rr = rs.row.rect();

        const bool hov = ctl::mouseIn(rr.p0(), rr.p1());
        const float hovT = motion::hoverFactor(
            ctl::detail::salt(ImGui::GetID("host.ip_item"), static_cast<int>(i)), hov);
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        const ImU32 rowBg = isSel
            ? motion::mix(t.container, t.high, hovT)
            : upix::ui::withAlpha(t.high, hovT);
        if ((rowBg >> IM_COL32_A_SHIFT) > 0) {
            fg->AddRectFilled(rr.p0(), rr.p1(), rowBg, geist::radius::MD);
        }

        if (isSel) {
            fg->AddCircleFilled(snapPt(menuFx.rect(rs.dot).center()), 3.0f, t.accent);
        }

        const flex::Rect ir = menuFx.rect(rs.ip);
        ellipsisTextCentered(fg, ui.fontMono, geist::type::MONO, ir.x, rr.y, rr.bottom(), ir.w,
                             isSel ? t.ink : motion::mix(t.body, t.ink, hovT), ip.c_str());

        if (menuFx.isVisible(rs.kind)) {
            ctl::metaChip(fg, ui.fontMono, menuFx.p0(rs.kind), ipKind(ip), 0, t.mute, t.tagBg,
                          ctl::geom::META_CHIP_H, 5.0f);
        }

        if (isSel) {
            drawIcon(fg, "check", menuFx.rect(rs.check).center(), 12.0f, t.accent, 1.8f);
        }

        if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            app.selectHostIp(static_cast<int>(i));
            ui.showIpPicker = false;
            break;
        }
    }
}

} // namespace upix::views::host_detail
