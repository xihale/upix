// views/host.cpp — Remote collaboration host page unified into one flex::Layout tree.
#include "views/host/internal.hpp"
#include "i18n.hpp"

#include <algorithm>

namespace upix::views {
namespace {

float gPageScroll = 0.0f;
float gPageScrollTarget = 0.0f;
float gPageContentH = 0.0f;

upix::ui::flex::Slot buildHeader(upix::ui::flex::Container& page, ImFont* sansMed, const char* pageTitle) {
    using namespace upix::ui;
    const ImVec2 tsz = calcText(sansMed, geist::type::PAGE_TITLE, pageTitle);
    return page.slot(flex::Grow(1.0f), flex::Fixed(tsz.y));
}

void drawHeader(ImDrawList* dl, const upix::ui::flex::Layout& fx, upix::ui::flex::Slot titleSlot,
                ImFont* sansMed, const char* pageTitle) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const flex::Rect tr = fx.rect(titleSlot);
    ellipsisText(dl, sansMed, geist::type::PAGE_TITLE, tr.p0(), tr.w, t.ink, pageTitle);
}

} // namespace

void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sans = ui.fontSans;
    ImFont* sansMed = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;
    ImFont* mono = ui.fontMono;

    const bool modal = !snap.host.pending_device.empty();
    if (modal) ui.showIpPicker = false;

    const float paneX = 0.0f;
    const float paneY = kTopbarH;
    const float paneW = std::max(0.0f, winSize.x);
    const float paneH = std::max(0.0f, winSize.y - kTopbarH);
    dl->AddRectFilled(ImVec2(paneX, paneY), winSize, t.bg);

    const float colW = std::min(host_detail::kColMax, std::max(320.0f, paneW - 64.0f));
    const float colX = snapPx(paneX + (paneW - colW) * 0.5f);
    const float rowOutset = std::min(10.0f, (colX - paneX) * 0.5f);

    const char* pageTitle = upix::i18n::tr(upix::i18n::Key::HostTitle);
    const float pageScrollUsed = gPageScroll;

    static flex::Context pageCtx;
    flex::Layout pageFx(pageCtx);
    auto page = pageFx.col({
        .gap = geist::space::XXXL,
        .width = flex::Fixed(colW),
        .height = flex::Auto(),
    });

    const flex::Slot hdrSlot = buildHeader(page, sansMed, pageTitle);
    const host_detail::HeroSlots heroSlots =
        host_detail::buildHeroCard(page, colW, snap, sansMed, sans, mono);
    const host_detail::TrustedSlots trustedSlots =
        host_detail::buildTrustedDevices(page, colW, snap, sansMed, sans, mono);
    const host_detail::LogSlots logSlots =
        host_detail::buildActivityLog(page, colW, snap, sansMed, sans, mono);

    const flex::Rect pageRect =
        pageFx.solveWidth(ImVec2(colX, paneY + host_detail::kPagePad - pageScrollUsed), colW);

    dl->PushClipRect(ImVec2(paneX, paneY), winSize, true);

    drawHeader(dl, pageFx, hdrSlot, sansMed, pageTitle);

    ImVec2 ipTrig0{};
    ImVec2 ipTrig1{};
    bool ipTrigDrawn = false;
    host_detail::drawHeroCard(dl, pageFx, heroSlots, ui, app, snap, sansMed, sans, mono, modal,
                              ipTrig0, ipTrig1, ipTrigDrawn);

    host_detail::drawTrustedDevices(dl, pageFx, trustedSlots, ui, app, snap, sansMed, sans, mono,
                                    rowOutset, modal);

    bool wheelConsumed = false;
    host_detail::drawActivityLog(dl, pageFx, logSlots, ui, snap, sansMed, sans, mono,
                                 modal, wheelConsumed);

    dl->PopClipRect();

    gPageContentH = pageRect.h + host_detail::kPagePad * 2.0f;
    const float maxPageScroll = std::max(0.0f, gPageContentH - paneH);
    const ImGuiIO& io = ImGui::GetIO();
    const bool overPage = io.MousePos.x >= paneX && io.MousePos.x < winSize.x &&
                          io.MousePos.y >= paneY && io.MousePos.y < winSize.y;
    if (!modal && !ui.showIpPicker && !wheelConsumed && overPage && io.MouseWheel != 0.0f) {
        gPageScrollTarget -= io.MouseWheel * 48.0f;
    }
    gPageScrollTarget = std::clamp(gPageScrollTarget, 0.0f, maxPageScroll);
    gPageScroll = std::clamp(motion::smoothScroll(ImGui::GetID("host.page_scroll"), gPageScrollTarget),
                             0.0f, maxPageScroll);

    ctl::scrollThumb(dl, ImVec2(paneX, paneY), ImVec2(paneX + paneW, paneY + paneH),
                     pageScrollUsed, gPageContentH);

    if (ipTrigDrawn && ui.showIpPicker && !modal) {
        host_detail::drawIpMenuPopup(ui, app, snap, ipTrig0, ipTrig1, winSize);
    }
}

} // namespace upix::views
