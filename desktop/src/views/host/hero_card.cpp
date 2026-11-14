// views/host/hero_card.cpp — QR & Endpoint Hero Card flex builder and renderer.
#include "views/host/internal.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cmath>

namespace upix::views::host_detail {
namespace {

double gCopiedUntil = -1.0;

} // namespace

HeroSlots buildHeroCard(upix::ui::flex::Container& page, float colW,
                        const upix::gui::Snapshot& snap,
                        ImFont* sansMed, ImFont* sans, ImFont* mono) {
    using namespace upix::ui;
    HeroSlots s;
    s.running = snap.host.running;
    s.curIp = snap.host.ips.empty()
                  ? std::string()
                  : snap.host.ips[static_cast<size_t>(
                        std::clamp(snap.host.selected_ip_idx, 0,
                                   static_cast<int>(snap.host.ips.size()) - 1))];
    const std::string portText = std::to_string(snap.host.port);
    s.groups = fpGroups(snap.host.fingerprint);

    const float pad = geist::space::XL;
    const float qrSz = 196.0f;
    const bool wide = (colW >= 560.0f);

    const ImVec2 fpLblSz = calcText(sans, geist::type::BODY_SMALL, upix::i18n::tr(upix::i18n::Key::HostFingerprint));
    const ImVec2 ipLblSz = calcText(sans, geist::type::BODY_SMALL, upix::i18n::tr(upix::i18n::Key::HostAddress));
    const ImVec2 portLblSz = calcText(sans, geist::type::BODY_SMALL, upix::i18n::tr(upix::i18n::Key::HostPort));

    s.stateText = s.running ? "LIVE" : "IDLE";
    const ImVec2 stateSz = calcText(mono, geist::type::MONO, s.stateText);
    s.subStatus = s.running ? (s.curIp.empty() ? portText : (s.curIp + ":" + portText))
                            : upix::i18n::tr(upix::i18n::Key::HostStopped);
    s.subFont = s.running ? mono : sans;
    s.subPx = s.running ? geist::type::MONO : geist::type::BODY_SMALL;
    const ImVec2 subSz = calcText(s.subFont, s.subPx, s.subStatus.c_str());

    s.btnLabel = upix::i18n::tr(s.running ? upix::i18n::Key::HostStop : upix::i18n::Key::HostStart);
    const float btnW = ctaWidth(sansMed, s.btnLabel, true);
    const float btnH = std::max(40.0f, std::ceil(calcText(sansMed, geist::type::BODY, s.btnLabel).y) + 18.0f);
    const float ctrlH = std::max(34.0f, ctl::geom::META_CHIP_H + 8.0f);
    const float chipH = std::max(ctl::geom::META_CHIP_H + 2.0f,
                                 std::ceil(calcText(mono, geist::type::BODY_SMALL, "0000").y) + 8.0f);
    const float portW = std::max(88.0f, std::ceil(calcText(mono, geist::type::MONO, ": 65535").x) + 28.0f);
    const float monoH = std::ceil(calcText(mono, geist::type::MONO, "0").y);

    const flex::ContainerStyle cardStyle{
        .padding = flex::Edges::all(pad),
        .gap = wide ? geist::space::XXL : geist::space::LG,
        .align = wide ? flex::Align::Center : flex::Align::Stretch,
        .width = flex::Grow(1.0f),
    };
    s.card = wide ? page.row(cardStyle) : page.col(cardStyle);
    s.qr = s.card.slot(flex::Fixed(qrSz), flex::Fixed(qrSz), flex::AlignSelf::Center);

    auto infoCol = s.card.col({
        .align = flex::Align::Stretch,
        .width = wide ? flex::Grow(1.0f) : flex::Auto(),
    });

    // Row 1: 指纹
    auto fpRow = infoCol.row({
        .padding = flex::Edges::xy(0.0f, 8.0f),
        .gap = geist::space::SM,
        .rowGap = geist::space::XS,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
        .height = flex::Auto().min(44.0f),
    });
    s.fpLbl = fpRow.slot(flex::Fixed(std::ceil(fpLblSz.x) + 2.0f), flex::Fixed(fpLblSz.y));
    auto fpRight = fpRow.row({
        .gap = 6.0f,
        .rowGap = 6.0f,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
    });
    for (size_t i = 0; i < 4; ++i) {
        const float cw = std::max(52.0f, ctl::metaChipWidth(mono, s.groups[i].c_str(), false, 8.0f));
        s.chips[i] = fpRight.slot(flex::Fixed(cw), flex::Fixed(chipH));
    }
    s.copy = fpRight.slot(flex::Fixed(28.0f), flex::Fixed(28.0f), flex::AlignSelf::Center,
                          flex::Edges::only(2.0f, 0.0f, 0.0f, 0.0f));

    s.div1 = infoCol.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));

    // Row 2: 地址
    auto ipRow = infoCol.row({
        .padding = flex::Edges::xy(0.0f, 8.0f),
        .gap = geist::space::MD,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .height = flex::Auto().min(46.0f),
    });
    s.ipLbl = ipRow.slot(flex::Fixed(std::ceil(ipLblSz.x) + 2.0f), flex::Fixed(ipLblSz.y));

    const char* kind = ipKind(s.curIp);
    const float kindW = kind[0] ? ctl::metaChipWidth(mono, kind, false, 5.0f) : 0.0f;
    s.ipTrig = ipRow.row({
        .padding = flex::Edges::xy(10.0f, 0.0f),
        .gap = 6.0f,
        .align = flex::Align::Center,
        .width = flex::Flex(256.0f, 0.0f, 1.0f).min(140.0f).max(256.0f),
        .height = flex::Fixed(ctrlH),
    });
    s.ipTrigDot = s.ipTrig.slot(flex::Fixed(6.0f), flex::Fixed(6.0f));
    s.ipTrigText = s.ipTrig.slot(flex::Grow(1.0f).min(32.0f), flex::Fixed(monoH));
    s.ipTrigKind = s.ipTrig.slot(flex::Fixed(kindW), flex::Fixed(ctl::geom::META_CHIP_H),
                                 flex::AlignSelf::Center, {}, kind[0] != '\0');
    s.ipTrigChev = s.ipTrig.slot(flex::Fixed(12.0f), flex::Fixed(12.0f));

    s.div2 = infoCol.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));

    // Row 3: 端口
    auto portRow = infoCol.row({
        .padding = flex::Edges::xy(0.0f, 8.0f),
        .gap = geist::space::MD,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .height = flex::Auto().min(46.0f),
    });
    s.portLbl = portRow.slot(flex::Fixed(std::ceil(portLblSz.x) + 2.0f), flex::Fixed(portLblSz.y));
    s.port = portRow.slot(flex::Fixed(portW), flex::Fixed(ctrlH));

    // Row 4: Footer status + CTA
    auto footerRow = infoCol.row({
        .margin = flex::Edges::only(0.0f, wide ? geist::space::XL : geist::space::LG, 0.0f, 0.0f),
        .gap = geist::space::MD,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
    });
    auto statusGroup = footerRow.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .width = flex::Flex(100.0f, 1.0f, 1.0f).min(60.0f),
    });
    s.statusDot = statusGroup.slot(flex::Fixed(7.0f), flex::Fixed(7.0f));
    s.state = statusGroup.slot(flex::Fixed(std::ceil(stateSz.x) + 2.0f), flex::Fixed(stateSz.y));
    s.sub = statusGroup.slot(flex::Grow(1.0f, 20.0f).min(20.0f), flex::Fixed(subSz.y));
    s.cta = footerRow.slot(flex::Fixed(btnW), flex::Fixed(btnH));

    return s;
}

void drawHeroCard(ImDrawList* dl, const upix::ui::flex::Layout& fx, const HeroSlots& s,
                  UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                  ImFont* sansMed, ImFont* sans, ImFont* mono, bool modal,
                  ImVec2& ipTrig0, ImVec2& ipTrig1, bool& ipTrigDrawn) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const flex::Rect cardRect = s.card.rect();
    ctl::flatSection(dl, cardRect.p0(), cardRect.p1(), geist::radius::LG, t.surface, t.outline);

    drawQr(dl, fx.p0(s.qr), fx.p1(s.qr), snap.host.qr_payload,
           s.running && !snap.host.qr_payload.empty());

    const flex::Rect fpLblR = fx.rect(s.fpLbl);
    drawTextCenteredY(dl, sans, geist::type::BODY_SMALL, fpLblR.x, fpLblR.y, fpLblR.bottom(),
                      t.mute, upix::i18n::tr(upix::i18n::Key::HostFingerprint));
    for (size_t i = 0; i < 4; ++i) {
        const flex::Rect cr = fx.rect(s.chips[i]);
        ctl::metaChip(dl, mono, cr.p0(), s.groups[i].c_str(), 0, t.ink, t.tagBg, cr.h, 8.0f);
    }

    const flex::Rect copyR = fx.rect(s.copy);
    if (ImGui::GetTime() < gCopiedUntil) {
        dl->AddRectFilled(copyR.p0(), copyR.p1(), t.accentSoft, geist::radius::MD);
        drawIcon(dl, "check", copyR.center(), 13.0f, t.accent, 1.8f);
        const bool hov = ctl::mouseIn(copyR.p0(), copyR.p1());
        ctl::tooltip(dl, ImGui::GetID("host.copyfp.tip"), copyR.p0(), copyR.p1(),
                     upix::i18n::tr(upix::i18n::Key::HostCopied), sans, hov, false);
    } else if (ctl::iconAction(dl, ImGui::GetID("host.copyfp"), copyR.p0(), copyR.w, "copy",
                               upix::i18n::tr(upix::i18n::Key::HostCopyFp),
                               sans, 13.0f, geist::radius::MD, !modal, false)) {
        SDL_SetClipboardText(snap.host.fingerprint.c_str());
        gCopiedUntil = ImGui::GetTime() + 1.5;
    }

    const flex::Rect d1 = fx.rect(s.div1);
    ctl::hairlineRule(dl, d1.x, d1.right(), d1.y);

    // IP row
    const flex::Rect ipLblR = fx.rect(s.ipLbl);
    drawTextCenteredY(dl, sans, geist::type::BODY_SMALL, ipLblR.x, ipLblR.y, ipLblR.bottom(),
                      t.mute, upix::i18n::tr(upix::i18n::Key::HostAddress));
    {
        const flex::Rect trigR = s.ipTrig.rect();
        const ImVec2 a = trigR.p0();
        const ImVec2 b = trigR.p1();
        ipTrig0 = a;
        ipTrig1 = b;
        ipTrigDrawn = true;

        const bool canPick = !modal && !snap.host.ips.empty();
        ImGui::SetCursorScreenPos(a);
        if (!canPick) ImGui::BeginDisabled();
        const bool trigClicked = ImGui::InvisibleButton("##host.ip_trigger", trigR.size());
        const bool trigHov = canPick && ImGui::IsItemHovered();
        const bool trigFocused = canPick && ImGui::IsItemFocused();
        if (!canPick) ImGui::EndDisabled();
        if (trigHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (trigClicked && canPick) {
            ui.showIpPicker = !ui.showIpPicker;
        }

        const float trigHovT = motion::hoverFactor(ImGui::GetID("host.ip_trigger.hov"),
                                                   trigHov && !ui.showIpPicker);
        const float trigActiveT = ui.showIpPicker ? 1.0f : trigHovT;

        dl->AddRectFilled(a, b, motion::mix(t.bg, t.container, trigActiveT), geist::radius::MD);
        dl->AddRect(a, b, motion::mix(t.outline, t.outlineStrong, trigActiveT),
                    geist::radius::MD, 0, hairlinePx());
        if (trigFocused && !ui.showIpPicker) {
            ctl::detail::focusRing(dl, a, b, geist::radius::MD, true);
        }

        dl->AddCircleFilled(snapPt(fx.rect(s.ipTrigDot).center()), 3.0f, s.running ? t.accent : t.mute);

        const char* shown = s.curIp.empty() ? "—" : s.curIp.c_str();
        const flex::Rect itr = fx.rect(s.ipTrigText);
        ellipsisTextCentered(dl, mono, geist::type::MONO, itr.x, a.y, b.y, itr.w,
                             s.curIp.empty() ? t.faint : t.ink, shown);

        if (fx.isVisible(s.ipTrigKind)) {
            ctl::metaChip(dl, mono, fx.p0(s.ipTrigKind), ipKind(s.curIp), 0, t.mute, t.tagBg,
                          ctl::geom::META_CHIP_H, 5.0f);
        }

        drawIcon(dl, ui.showIpPicker ? "chevron-up" : "chevron-down",
                 fx.rect(s.ipTrigChev).center(), 11.0f,
                 motion::mix(t.mute, t.ink, trigActiveT), 1.6f);
    }

    const flex::Rect d2 = fx.rect(s.div2);
    ctl::hairlineRule(dl, d2.x, d2.right(), d2.y);

    // Port row
    const flex::Rect portLblR = fx.rect(s.portLbl);
    drawTextCenteredY(dl, sans, geist::type::BODY_SMALL, portLblR.x, portLblR.y, portLblR.bottom(),
                      t.mute, upix::i18n::tr(upix::i18n::Key::HostPort));
    {
        int nextPort = snap.host.port;
        if (portInputField(dl, ImGui::GetID("host.port"), fx.p0(s.port), fx.p1(s.port),
                           snap.host.port, !s.running && !modal && !ui.showIpPicker,
                           s.running && !modal && !ui.showIpPicker,
                           mono, sans, nextPort)) {
            app.setHostPort(nextPort);
        }
    }

    // Footer
    const ImU32 stateCol = s.running ? t.accent : t.mute;
    const ImVec2 dotC = snapPt(fx.rect(s.statusDot).center());
    if (s.running) {
        dl->AddCircleFilled(dotC, 3.0f, stateCol);
    } else {
        dl->AddCircle(dotC, 3.0f, stateCol, 14, 1.4f);
    }
    const flex::Rect stR = fx.rect(s.state);
    drawTextCenteredY(dl, mono, geist::type::MONO, stR.x, stR.y, stR.bottom(), stateCol, s.stateText);

    const flex::Rect subR = fx.rect(s.sub);
    if (subR.w >= 20.0f) {
        ellipsisTextCentered(dl, s.subFont, s.subPx, subR.x, subR.y, subR.bottom(), subR.w,
                             s.running ? t.body : t.mute, s.subStatus.c_str());
    }

    if (s.running) {
        if (ctl::secondaryButton(dl, ImGui::GetID("host.stop"), s.btnLabel, sansMed,
                                 fx.p0(s.cta), fx.p1(s.cta),
                                 "stop", !modal && !ui.showIpPicker, true)) {
            app.hostStop();
        }
    } else if (ctl::primaryCta(dl, ImGui::GetID("host.start"), s.btnLabel, sansMed,
                               fx.p0(s.cta), fx.p1(s.cta),
                               !modal && !ui.showIpPicker, "play")) {
        app.hostStart();
    }
}

} // namespace upix::views::host_detail
