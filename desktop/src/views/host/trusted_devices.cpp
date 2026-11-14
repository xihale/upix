// views/host/trusted_devices.cpp — Trusted Devices flex section builder and renderer.
#include "views/host/internal.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace upix::views::host_detail {

TrustedSlots buildTrustedDevices(upix::ui::flex::Container& page, float colW,
                                   const upix::gui::Snapshot& snap,
                                   ImFont* sansMed, ImFont* sans, ImFont* mono) {
    using namespace upix::ui;
    TrustedSlots s;
    const size_t nDev = snap.host.trusted_devices.size();
    s.countStr = std::to_string(nDev);
    s.clearId = ImGui::GetID("host.clear_trusted");

    using upix::i18n::Key;
    using upix::i18n::tr;
    const char* clrLabel = tr(Key::CommonClear);
    const char* clrConfirm = tr(Key::CommonConfirmClearQ);
    const char* activeClrLabel = ctl::isConfirmArmed(s.clearId) ? clrConfirm : clrLabel;
    const ImVec2 clrSz = calcText(sansMed, geist::type::BODY_SMALL, activeClrLabel);
    const float cw = std::ceil(clrSz.x) + 20.0f;
    const float ch = std::max(30.0f, std::ceil(clrSz.y) + 12.0f);

    const ImVec2 titleSz = calcText(sansMed, geist::type::BODY_SMALL, tr(Key::HostTrustedTitle));
    const ImVec2 countSz = calcText(mono, geist::type::MONO, s.countStr.c_str());
    const float headH = std::max(36.0f, std::ceil(std::max(titleSz.y, countSz.y)) + 14.0f);

    auto sec = page.col({.width = flex::Grow(1.0f)});
    auto head = sec.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Fixed(headH),
    });
    s.title = head.slot(flex::Fixed(std::ceil(titleSz.x) + 4.0f), flex::Fixed(titleSz.y));
    s.count = head.slot(flex::Fixed(std::ceil(countSz.x) + 2.0f), flex::Fixed(countSz.y));
    head.spacer(1.0f);
    s.clear = head.slot(flex::Fixed(cw), flex::Fixed(ch), flex::AlignSelf::Center, {}, nDev > 0);
    s.headDiv = sec.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));

    if (nDev == 0) {
        const float emptyH = std::max(54.0f, std::ceil(calcText(sans, geist::type::BODY_SMALL, tr(Key::HostNoTrusted)).y) + 28.0f);
        s.empty = sec.slot(flex::Grow(1.0f), flex::Fixed(emptyH));
        s.emptyDiv = sec.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));
        return s;
    }

    const float badgeW = ctl::metaChipWidth(sans, tr(Key::HostTrustedBadge), false, 6.0f);
    const float btnBox = 30.0f;
    const float rowH = std::max(52.0f, std::ceil(calcText(sansMed, geist::type::BODY, "Ag").y) + 24.0f);
    s.rows.reserve(nDev);

    for (size_t i = 0; i < nDev; ++i) {
        const auto& d = snap.host.trusted_devices[i];
        TrustedRowSlots rs;
        rs.fpText = dottedFp(d.fingerprint);
        const ImVec2 fpSz = calcText(mono, geist::type::MONO, rs.fpText.c_str());
        const ImVec2 nameSz = calcText(sansMed, geist::type::BODY, d.name.c_str());
        const float fpSlotW = std::ceil(fpSz.x) + 2.0f;
        const float minRowForFp = 8.0f + 40.0f + badgeW + fpSlotW + btnBox + geist::space::SM * 4.0f;
        const bool showFp = (colW >= minRowForFp);

        rs.row = sec.row({
            .gap = 10.0f,
            .align = flex::Align::Center,
            .width = flex::Grow(1.0f),
            .height = flex::Fixed(rowH),
        });
        rs.dot = rs.row.slot(flex::Fixed(8.0f), flex::Fixed(8.0f));
        rs.name = rs.row.slot(flex::Shrink(std::ceil(nameSz.x) + 2.0f, 1.0f).min(40.0f),
                              flex::Fixed(nameSz.y));
        rs.badge = rs.row.slot(flex::Fixed(badgeW), flex::Fixed(ctl::geom::META_CHIP_H));
        rs.row.spacer(1.0f);
        rs.fp = rs.row.slot(flex::Fixed(fpSlotW), flex::Fixed(fpSz.y),
                            flex::AlignSelf::Center, {}, showFp);
        rs.del = rs.row.slot(flex::Fixed(btnBox), flex::Fixed(btnBox));

        if (i + 1 < nDev) {
            rs.div = sec.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));
        }
        s.rows.push_back(std::move(rs));
    }
    return s;
}

void drawTrustedDevices(ImDrawList* dl, const upix::ui::flex::Layout& fx, const TrustedSlots& s,
                        const UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                        ImFont* sansMed, ImFont* sans, ImFont* mono,
                        float rowOutset, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    using upix::i18n::Key;
    using upix::i18n::tr;
    const flex::Rect trR = fx.rect(s.title);
    const flex::Rect cr = fx.rect(s.count);
    drawTextCenteredY(dl, sansMed, geist::type::BODY_SMALL, trR.x, trR.y, trR.bottom(), t.mute, tr(Key::HostTrustedTitle));
    drawTextCenteredY(dl, mono, geist::type::MONO, cr.x, cr.y, cr.bottom(), t.mute, s.countStr.c_str());

    if (fx.isVisible(s.clear)) {
        if (ctl::confirmButton(dl, s.clearId, tr(Key::CommonClear), tr(Key::CommonConfirmClearQ),
                               sansMed, fx.p0(s.clear), fx.p1(s.clear),
                               !modal && !ui.showIpPicker)) {
            app.clearTrustedDevices();
        }
    }
    const flex::Rect hd = fx.rect(s.headDiv);
    ctl::hairlineRule(dl, hd.x, hd.right(), hd.y);

    const size_t nDev = snap.host.trusted_devices.size();
    if (nDev == 0) {
        const flex::Rect er = fx.rect(s.empty);
        const std::string fitted = fitTextEllipsis(
            sans, geist::type::BODY_SMALL, tr(Key::HostNoTrusted), er.w - 24.0f);
        drawTextCentered(dl, sans, geist::type::BODY_SMALL, er.p0(), er.p1(), t.mute, fitted.c_str());
        const flex::Rect ed = fx.rect(s.emptyDiv);
        ctl::hairlineRule(dl, ed.x, ed.right(), ed.y);
        return;
    }

    for (size_t i = 0; i < nDev; ++i) {
        const auto& d = snap.host.trusted_devices[i];
        const TrustedRowSlots& rs = s.rows[i];
        const flex::Rect rr = rs.row.rect();

        const bool rowHov = !modal && !ui.showIpPicker && ctl::mouseIn(rr.p0(), rr.p1());
        const float rowHovT = motion::hoverFactor(
            ctl::detail::salt(ImGui::GetID("host.dev_row"), static_cast<int>(i)), rowHov);
        if (rowHovT > 0.005f) {
            dl->AddRectFilled(snapPt(rr.x - rowOutset, rr.y),
                              snapPt(rr.right() + rowOutset, rr.bottom()),
                              upix::ui::withAlpha(t.container, rowHovT), geist::radius::MD);
        }

        dl->AddCircleFilled(snapPt(fx.rect(rs.dot).center()), 3.0f, d.online ? t.accent : t.mute);

        const flex::Rect nr = fx.rect(rs.name);
        ellipsisTextCentered(dl, sansMed, geist::type::BODY, nr.x, nr.y, nr.bottom(), nr.w, t.ink, d.name.c_str());

        ctl::metaChip(dl, sans, fx.p0(rs.badge), tr(Key::HostTrustedBadge), 0, t.body, t.tagBg);

        if (fx.isVisible(rs.fp)) {
            const flex::Rect fpr = fx.rect(rs.fp);
            drawTextCenteredY(dl, mono, geist::type::MONO, fpr.x, fpr.y, fpr.bottom(), t.mute, rs.fpText.c_str());
        }

        ImGui::PushID(static_cast<int>(i));
        if (ctl::confirmButton(dl, ImGui::GetID("host.untrust"), nullptr, tr(Key::CommonConfirmQ), sansMed,
                               fx.p0(rs.del), fx.p1(rs.del),
                               !modal && !ui.showIpPicker, "trash", tr(Key::HostRemoveDevice), 13.5f)) {
            app.removeTrustedDevice(d.fingerprint);
        }
        ImGui::PopID();

        if (rs.div.valid()) {
            const flex::Rect rd = fx.rect(rs.div);
            ctl::hairlineRule(dl, rd.x, rd.right(), rd.y);
        }
    }
}

} // namespace upix::views::host_detail
