// views/host/pairing_modal.cpp — incoming device pairing approval modal laid out via flex::Layout.
#include "views/host/internal.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace upix::views {
namespace {

struct PairingModalSlots {
    upix::ui::flex::Container modal;
    upix::ui::flex::Slot icon;
    upix::ui::flex::Slot heading;
    upix::ui::flex::Slot device;
    upix::ui::flex::Slot pairTag;
    upix::ui::flex::Slot body;
    std::array<upix::ui::flex::Slot, 4> pills;
    upix::ui::flex::Slot deny;
    upix::ui::flex::Slot allow;
};

} // namespace

void renderPairingModal(ImDrawList* /*dl*/, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    if (snap.host.pending_device.empty()) return;
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansMed = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(winSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##pairing_modal_layer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImDrawList* mdl = ImGui::GetWindowDrawList();

    // Instant scrim using t.scrim and t.scrimAlpha.
    mdl->AddRectFilled(ImVec2(0.0f, 0.0f), winSize, withAlpha(t.scrim, t.scrimAlpha));

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.hostDecide(false);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        app.hostDecide(true);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    constexpr float kMarginX = 16.0f;
    constexpr float kMarginY = 16.0f;
    const float mw = std::min(432.0f, std::max(280.0f, winSize.x - kMarginX * 2.0f));

    const char* heading = "新设备请求连接";
    const char* body = "核对设备指纹";
    const char* pairTag = "PAIR";
    const char* denyLabel = "拒绝";
    const char* allowLabel = "允许配对";
    const auto groups = host_detail::fpGroups(snap.host.pending_fp);

    const ImVec2 hsz = calcText(sansMed, geist::type::BODY, heading);
    const ImVec2 dsz = calcText(ui.fontMono, geist::type::MONO, snap.host.pending_device.c_str());
    const float tagW = ctl::metaChipWidth(ui.fontMono, pairTag, false, 6.0f);
    const float denyW = std::max(
        88.0f, std::ceil((calcText(sansMed, geist::type::BODY, denyLabel).x + geist::space::LG * 2.0f) / 4.0f) * 4.0f);
    const float allowW = host_detail::ctaWidth(sansMed, allowLabel, true);
    const float btnH = std::max(40.0f, std::ceil(calcText(sansMed, geist::type::BODY, allowLabel).y) + 18.0f);

    auto measureWrapped = [](ImFont* font, float fontPx, const char* text) {
        return [=](float availW, float /*availH*/) -> flex::IntrinsicSize {
            if (!text || !text[0]) return {0.0f, 0.0f};
            if (!font) return {0.0f, fontPx};
            const float wrapW = flex::detail::isDefinite(availW) ? std::max(1.0f, availW) : FLT_MAX;
            const ImVec2 sz = font->CalcTextSizeA(fontPx, FLT_MAX, wrapW, text);
            return {std::min(wrapW, std::ceil(sz.x) + 2.0f), std::ceil(sz.y) + 1.0f};
        };
    };

    static flex::Context modalCtx;
    flex::Layout fx(modalCtx);
    auto overlay = fx.col({
        .padding = flex::Edges::xy(kMarginX, kMarginY),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .width = flex::Fixed(winSize.x),
        .height = flex::Fixed(winSize.y),
    });

    PairingModalSlots s;
    s.modal = overlay.col({
        .padding = flex::Edges::xy(geist::space::XXL, geist::space::XXL),
        .gap = geist::space::LG,
        .align = flex::Align::Stretch,
        .width = flex::Fixed(mw),
        .height = flex::Auto(),
    });

    auto hdr = s.modal.row({
        .gap = geist::space::MD,
        .align = flex::Align::Center,
    });
    s.icon = hdr.slot(flex::Fixed(38.0f), flex::Fixed(38.0f));
    auto titleCol = hdr.col({
        .gap = 3.0f,
        .width = flex::Grow(1.0f, 40.0f).min(40.0f),
    });
    s.heading = titleCol.slot(flex::Grow(1.0f), flex::Fixed(hsz.y));
    s.device = titleCol.slot(flex::Grow(1.0f), flex::Fixed(dsz.y));
    s.pairTag = hdr.slot(flex::Fixed(tagW), flex::Fixed(ctl::geom::META_CHIP_H), flex::AlignSelf::Start);

    s.body = s.modal.measured(measureWrapped(ui.fontSans, geist::type::BODY_SMALL, body),
                              flex::Grow(1.0f), flex::Auto());

    // Single clean row of 4 borderless fingerprint pills (no nested box).
    auto pillsRow = s.modal.row({
        .padding = flex::Edges::xy(0.0f, geist::space::XS),
        .gap = geist::space::SM,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
    });
    const float pillH = std::max(32.0f, std::ceil(calcText(ui.fontMono, geist::type::BODY, "0000").y) + 12.0f);
    for (size_t i = 0; i < 4; ++i) {
        const float pw = std::max(64.0f, std::ceil(calcText(ui.fontMono, geist::type::BODY, groups[i].c_str()).x) + 24.0f);
        s.pills[i] = pillsRow.slot(flex::Fixed(pw), flex::Fixed(pillH));
    }

    auto btnRow = s.modal.row({
        .gap = geist::space::SM,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
    });
    s.deny = btnRow.slot(flex::Fixed(denyW), flex::Fixed(btnH));
    s.allow = btnRow.slot(flex::Fixed(allowW), flex::Fixed(btnH));

    fx.solve(flex::Rect{0.0f, 0.0f, winSize.x, winSize.y});
    const flex::Rect modalRect = s.modal.rect();

    if (!ctl::mouseIn(modalRect.p0(), modalRect.p1()) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        app.hostDecide(false);
        ImGui::End();
        ImGui::PopStyleVar(2);
        return;
    }

    // Single Finish::Menu modal (radius 12px LG, 1px t.outlineInput border, no double-nested card).
    ctl::finish(mdl, modalRect.p0(), modalRect.p1(), ctl::Finish::Menu, geist::radius::LG, false);
    mdl->AddRect(modalRect.p0(), modalRect.p1(), t.outlineInput, geist::radius::LG, 0, hairlinePx());

    const flex::Rect iconR = fx.rect(s.icon);
    mdl->AddRectFilled(iconR.p0(), iconR.p1(), t.accentSoft, geist::radius::MD);
    drawIcon(mdl, "smartphone", iconR.center(), 16.0f, t.accent, 1.6f);

    const flex::Rect hdrR = fx.rect(s.heading);
    ellipsisText(mdl, sansMed, geist::type::BODY, hdrR.p0(), hdrR.w, t.ink, heading);
    const flex::Rect devR = fx.rect(s.device);
    ellipsisText(mdl, ui.fontMono, geist::type::MONO, devR.p0(), devR.w, t.body,
                 snap.host.pending_device.c_str());

    ctl::metaChip(mdl, ui.fontMono, fx.p0(s.pairTag), pairTag, 0, t.mute, t.tagBg);

    const flex::Rect bodyR = fx.rect(s.body);
    if (ui.fontSans) {
        mdl->AddText(ui.fontSans, geist::type::BODY_SMALL, snapPt(bodyR.p0()), t.mute, body, nullptr,
                     std::max(1.0f, bodyR.w));
    }

    for (size_t i = 0; i < 4; ++i) {
        const flex::Rect pr = fx.rect(s.pills[i]);
        mdl->AddRectFilled(pr.p0(), pr.p1(), t.tagBg, geist::radius::SM);
        drawTextCentered(mdl, ui.fontMono, geist::type::BODY, pr.p0(), pr.p1(), t.ink, groups[i].c_str());
    }

    if (ctl::secondaryButton(mdl, ImGui::GetID("pair.deny"), denyLabel, sansMed,
                             fx.p0(s.deny), fx.p1(s.deny), nullptr, true, false)) {
        app.hostDecide(false);
    }
    if (ctl::primaryCta(mdl, ImGui::GetID("pair.allow"), allowLabel, sansMed,
                        fx.p0(s.allow), fx.p1(s.allow), true, "check")) {
        app.hostDecide(true);
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace upix::views
