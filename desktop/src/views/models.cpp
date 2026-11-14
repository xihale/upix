#include "views/common.hpp"
#include "views/models/grouping.hpp"
#include "views/models/model_row.hpp"

#include "ui/flex.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace upix::views {
namespace {

constexpr float kPagePad = upix::ui::geist::space::XXXL;
constexpr float kColMax = 980.0f;

float gScroll = 0.0f;
float gScrollTarget = 0.0f;
float gContentH = 0.0f;
std::string gDismissedBannerStatus;
bool gMirrorOpen = false;

struct EmptyStateSlots {
    upix::ui::flex::Slot icon;
    upix::ui::flex::Slot title;
    upix::ui::flex::Slot button;
};

EmptyStateSlots addEmptyState(upix::ui::flex::Container& parent, ImFont* sans, ImFont* sansMed,
                              float colW, const char* title, bool withClearFilter, bool visible) {
    using namespace upix::ui;
    const ImVec2 tsz = calcText(sansMed, geist::type::BODY, title);
    const ImVec2 bsz = calcText(sans, geist::type::BODY_SMALL, "清除筛选");
    const float btnW = std::ceil(bsz.x) + 28.0f;
    const float btnH = std::max(32.0f, std::ceil(bsz.y) + 14.0f);

    auto box = parent.col({
        .padding = flex::Edges::xy(geist::space::LG, geist::space::XXXL),
        .gap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .width = flex::Fixed(colW),
        .visible = visible,
    });
    const auto iconSlot = box.slot(flex::Fixed(20.0f), flex::Fixed(20.0f));
    const auto titleSlot = box.slot(flex::Auto(std::ceil(tsz.x) + 4.0f),
                                    flex::Fixed(std::ceil(tsz.y) + 2.0f));
    const auto btnSlot = box.slot(flex::Fixed(btnW), flex::Fixed(btnH), flex::AlignSelf::Center,
                                  flex::Edges::only(0.0f, geist::space::XS, 0.0f, 0.0f), withClearFilter);
    return {iconSlot, titleSlot, btnSlot};
}

bool drawEmptyState(ImDrawList* dl, const upix::ui::flex::Layout& fx, const EmptyStateSlots& slots,
                    ImFont* sans, ImFont* sansMed, ImGuiID btnId,
                    const char* title, bool withClearFilter) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    drawIcon(dl, "layers", fx.rect(slots.icon).center(), 18.0f, t.mute);
    drawTextCentered(dl, sansMed, geist::type::BODY, fx.p0(slots.title), fx.p1(slots.title), t.ink, title);
    if (withClearFilter && fx.isVisible(slots.button)) {
        return ctl::secondaryButton(dl, btnId, "清除筛选", sans, fx.p0(slots.button), fx.p1(slots.button));
    }
    return false;
}

struct GroupSlots {
    upix::ui::flex::Slot head;
    std::vector<upix::ui::flex::Slot> rows;
};

} // namespace

void renderModelsPage(ImDrawList* /*dl*/, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                      SDL_Window* window, DialogBridge* dlgImport) {
    using namespace upix::ui;
    using namespace upix::views::models_detail;
    const auto& t = geist::tokens();
    ImFont* sans = ui.fontSans;
    ImFont* sansMed = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;
    ImFont* mono = ui.fontMono;

    const float paneX = 0.0f;
    const float paneY = kTopbarH;
    const float paneW = std::max(0.0f, winSize.x);
    const float paneH = std::max(0.0f, winSize.y - paneY);

    const bool narrow = (paneW <= 860.0f);
    const float padX = narrow ? 16.0f : 24.0f;
    const float topPad = kPagePad;
    const float bottomPad = narrow ? 48.0f : 64.0f;
    const float secGap = narrow ? 20.0f : geist::space::XXL;
    const float colW = std::min(kColMax, std::max(280.0f, paneW - padX * 2.0f));
    const float rowOutset = std::min(12.0f, padX * 0.5f);
    const float pageW = colW + rowOutset * 2.0f;
    const float pageX = snapPx(paneX + (paneW - pageW) * 0.5f);

    ImGui::SetCursorScreenPos(ImVec2(paneX, paneY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##models_pane", ImVec2(paneW, paneH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetScrollY(0.0f);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    cdl->AddRectFilled(ImVec2(paneX, paneY), winSize, t.bg);

    const ImGuiIO& io = ImGui::GetIO();
    const bool modal = !snap.host.pending_device.empty();
    const bool overPage = !modal && io.MousePos.x >= paneX && io.MousePos.x < winSize.x &&
                          io.MousePos.y >= paneY && io.MousePos.y < winSize.y;
    const float maxScroll = std::max(0.0f, gContentH - paneH);
    if (overPage && io.MouseWheel != 0.0f) {
        gScrollTarget -= io.MouseWheel * 48.0f;
    }
    gScrollTarget = std::clamp(gScrollTarget, 0.0f, maxScroll);
    gScroll = std::clamp(motion::smoothScroll(ImGui::GetID("models.scroll"), gScrollTarget), 0.0f, maxScroll);

    // Prepare catalog summary, filter tabs, banner state, and grouped models before unified flex solve.
    std::unordered_set<std::string> catalogNames;
    double totalInstalledMB = 0.0;
    for (const auto& m : snap.installed) {
        catalogNames.insert(m.name);
        totalInstalledMB += parseSizeMB(m.size);
    }
    for (const auto& m : snap.remote) {
        catalogNames.insert(m.name);
    }
    const std::string subText = std::format(
        "已缓存 {} / {} · {}",
        snap.installed.size(), catalogNames.size(), formatTotalMB(totalInstalledMB));

    const ImVec2 titleSz = calcText(sansMed, geist::type::PAGE_TITLE, "模型库");
    const ImVec2 subSz = calcText(mono, geist::type::MONO, subText.c_str());
    const float hdrBtnH = std::max(34.0f, std::ceil(calcText(sansMed, geist::type::BODY, "导入模型").y) + 14.0f);
    const float importW = std::ceil(calcText(sansMed, geist::type::BODY, "导入模型").x) + 22.0f;
    const float mirrorW = std::ceil(calcText(sansMed, geist::type::BODY, "镜像源").x) + 20.0f;
    const float refreshW = std::ceil(calcText(sansMed, geist::type::BODY, "刷新列表").x) + 20.0f;
    const float dirW = std::ceil(calcText(sansMed, geist::type::BODY, "打开目录").x) + 20.0f;

    static constexpr const char* kSourceLabels[3] = {"HF-Mirror", "Hugging Face", "GitHub"};
    static constexpr const char* kSourceValues[3] = {"hf-mirror", "hf", "github"};
    int sourceIdx = 0;
    for (int i = 0; i < 3; ++i) {
        if (snap.model_source == kSourceValues[i]) {
            sourceIdx = i;
            break;
        }
    }
    float maxSrcLblW = 0.0f;
    for (int i = 0; i < 3; ++i) {
        maxSrcLblW = std::max(maxSrcLblW, calcText(sans, geist::type::LABEL, kSourceLabels[i]).x);
    }
    const float sourceSegW = std::max(48.0f, std::ceil(maxSrcLblW + 24.0f)) * 3.0f + ctl::geom::SEG_PAD * 2.0f;
    const ImVec2 srcLblSz = calcText(sansMed, geist::type::BODY, "下载源");
    const bool mirrorCompact = (colW < sourceSegW + srcLblSz.x + 60.0f);

    static const char* const kTabLabels[6] = {"全部", "推荐", "动漫", "照片", "综合", "修复"};
    static const char* const kTabValues[6] = {"all", "rec", "动漫", "照片", "综合", "修复"};
    int tabIdx = 0;
    for (int i = 0; i < 6; ++i) {
        if (ui.modelsSceneFilter == kTabValues[i]) {
            tabIdx = i;
            break;
        }
    }
    constexpr float tabGap = geist::space::XL;
    float tabsNeedW = 0.0f;
    for (int i = 0; i < 6; ++i) {
        tabsNeedW += std::ceil(calcText(sansMed, geist::type::BODY, kTabLabels[i]).x) + 4.0f;
    }
    tabsNeedW += tabGap * 5.0f;
    const float toolbarH = std::max(36.0f, std::ceil(calcText(sansMed, geist::type::BODY, "全部").y) + 16.0f);
    const float searchW = std::max(220.0f, std::ceil(calcText(sans, geist::type::BODY, "搜索模型...").x) + 64.0f);
    const bool singleRow = (colW >= tabsNeedW + searchW + geist::space::LG);

    const std::string qLower = toLowerTrimmed(ui.modelsSearch);
    const bool hasFilter = !qLower.empty() || (ui.modelsSceneFilter != "all" && !ui.modelsSceneFilter.empty());
    const auto groups = buildUnifiedFilteredGroups(snap, ui.modelsSceneFilter, qLower);

    const bool hasDownload = !snap.downloading.empty();
    bool downloadingRowVisible = false;
    if (hasDownload) {
        for (const auto& g : groups) {
            for (const auto& item : g.items) {
                if (item.remote && item.name == snap.downloading) {
                    downloadingRowVisible = true;
                    break;
                }
            }
            if (downloadingRowVisible) break;
        }
    }
    const bool hasStatus = !snap.models_status.empty() && snap.models_status != gDismissedBannerStatus;
    if (snap.models_status.empty()) gDismissedBannerStatus.clear();
    const bool showBanner = (!hasDownload && hasStatus) || (hasDownload && !downloadingRowVisible);

    const std::string bannerMsg = hasDownload
        ? std::format("正在下载 {}", snap.downloading)
        : snap.models_status;
    const ImVec2 msz = showBanner ? calcText(sans, geist::type::BODY_SMALL, bannerMsg.c_str()) : ImVec2(0.0f, 0.0f);
    const std::string pctStr = hasDownload
        ? std::format("{}%", std::clamp(snap.download_pct, 0, 100))
        : std::string();
    const ImVec2 psz = (showBanner && hasDownload) ? calcText(mono, geist::type::MONO, pctStr.c_str()) : ImVec2(0.0f, 0.0f);
    const float pctW = (showBanner && hasDownload) ? (std::ceil(calcText(mono, geist::type::MONO, "100%").x) + 2.0f) : 0.0f;

    const char* emptyTitle = hasFilter ? "无匹配模型" : "暂无可用模型";
    const float bannerH = std::max(40.0f, std::ceil(std::max(msz.y, psz.y)) + 16.0f);

    // Pass 1: Build ONE unified page flex tree.
    static flex::Context pageCtx;
    flex::Layout pageFx(pageCtx);
    auto page = pageFx.col({
        .gap = secGap,
        .align = flex::Align::Center,
        .width = flex::Fixed(pageW),
    });

    // 1. Page Header
    auto hdr = page.row({
        .gap = geist::space::LG,
        .rowGap = geist::space::MD,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
        .width = flex::Fixed(colW),
    });
    auto titles = hdr.col({
        .gap = 4.0f,
        .width = flex::Flex(260.0f, 1.0f, 1.0f).min(200.0f),
    });
    const auto titleSlot = titles.slot(flex::Grow(1.0f), flex::Fixed(titleSz.y));
    const auto subSlot = titles.slot(flex::Grow(1.0f), flex::Fixed(subSz.y));

    auto actions = hdr.row({.gap = geist::space::SM, .align = flex::Align::Center, .wrap = flex::Wrap::Wrap});
    const auto impSlot = actions.slot(flex::Fixed(importW), flex::Fixed(hdrBtnH));
    const auto mirSlot = actions.slot(flex::Fixed(mirrorW), flex::Fixed(hdrBtnH));
    const auto refSlot = actions.slot(flex::Fixed(refreshW), flex::Fixed(hdrBtnH));
    const auto dirSlot = actions.slot(flex::Fixed(dirW), flex::Fixed(hdrBtnH));

    // 1b. Collapsible Mirror Source Bar
    auto mirrorBar = page.row({
        .padding = flex::Edges::xy(14.0f, 10.0f),
        .gap = geist::space::MD,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
        .width = flex::Fixed(colW),
        .visible = gMirrorOpen,
    });
    const auto mirTitleSlot = mirrorBar.slot(
        mirrorCompact ? flex::Fixed(std::max(80.0f, colW - 28.0f)) : flex::Grow(1.0f, 80.0f).min(80.0f),
        flex::Fixed(srcLblSz.y),
        flex::AlignSelf::Center);
    const auto mirSegSlot = mirrorBar.slot(
        flex::Fixed(std::min(sourceSegW, std::max(180.0f, colW - 28.0f))),
        flex::Fixed(ctl::geom::SEG_H_COMPACT),
        flex::AlignSelf::Center);

    // 2. Shared-Rule Filter & Search Toolbar
    auto tb = page.row({
        .gap = geist::space::LG,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::SpaceBetween,
        .align = flex::Align::End,
        .wrap = flex::Wrap::Wrap,
        .width = flex::Fixed(colW),
    });
    const auto tabsSlot = tb.slot(flex::Fixed(singleRow ? tabsNeedW : colW), flex::Fixed(toolbarH));
    const auto searchSlot = tb.slot(flex::Fixed(singleRow ? searchW : colW), flex::Fixed(toolbarH));

    // 3. Conditional Download / Status Banner
    auto banner = page.row({
        .padding = flex::Edges::only(14.0f, 6.0f, 8.0f, 6.0f),
        .gap = 10.0f,
        .align = flex::Align::Center,
        .width = flex::Fixed(colW),
        .height = flex::Fixed(bannerH),
        .visible = showBanner,
    });
    const auto dotSlot = banner.slot(flex::Fixed(6.0f), flex::Fixed(6.0f));
    const auto msgSlot = banner.slot(flex::Grow(1.0f, 40.0f).min(40.0f), flex::Fixed(msz.y));
    const auto barSlot = banner.slot(flex::Shrink(140.0f, 1.0f).min(60.0f),
                                     flex::Fixed(ctl::geom::PROGRESS_H), flex::AlignSelf::Center, {}, hasDownload);
    auto pctBox = banner.row({
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Center,
        .width = flex::Fixed(pctW),
        .height = flex::Fixed(psz.y),
        .visible = hasDownload,
    });
    const auto pctTextSlot = pctBox.slot(flex::Fixed(std::ceil(psz.x)), flex::Fixed(psz.y));
    const auto closeSlot = banner.slot(flex::Fixed(28.0f), flex::Fixed(28.0f));

    // 4. EmptyState or Model Family Groups & Rows
    const EmptyStateSlots emptySlots = addEmptyState(page, sans, sansMed, colW, emptyTitle,
                                                     hasFilter, groups.empty());
    std::vector<GroupSlots> groupSlots;
    if (!groups.empty()) {
        const float groupHeadH = std::ceil(std::max(calcText(mono, geist::type::BODY_SMALL, "A").y,
                                                    calcText(sansMed, geist::type::BODY_SMALL, "模型").y)) + 8.0f;
        const float rowH = modelRowHeight(sansMed);
        auto groupsCol = page.col({
            .gap = geist::space::XXL,
            .align = flex::Align::Center,
            .width = flex::Fixed(pageW),
        });
        groupSlots.reserve(groups.size());
        for (const auto& g : groups) {
            auto grpCol = groupsCol.col({
                .align = flex::Align::Center,
                .width = flex::Fixed(pageW),
            });
            GroupSlots gs;
            gs.head = grpCol.slot(flex::Fixed(colW), flex::Fixed(groupHeadH));
            gs.rows.reserve(g.items.size());
            for (size_t ri = 0; ri < g.items.size(); ++ri) {
                gs.rows.push_back(grpCol.slot(flex::Fixed(pageW), flex::Fixed(rowH)));
            }
            groupSlots.push_back(std::move(gs));
        }
    }

    // Pass 2: Single unified solve.
    const flex::Rect pageRect = pageFx.solveWidth(ImVec2(pageX, paneY + topPad - gScroll), pageW);
    gContentH = pageRect.h + topPad + bottomPad;

    cdl->PushClipRect(ImVec2(paneX, paneY), winSize, true);

    // Draw Page Header
    drawText(cdl, sansMed, geist::type::PAGE_TITLE, pageFx.p0(titleSlot), t.ink, "模型库");
    ellipsisText(cdl, mono, geist::type::MONO, pageFx.p0(subSlot), pageFx.rect(subSlot).w, t.mute, subText.c_str());

    if (ctl::secondaryButton(cdl, ImGui::GetID("models.import"), "导入模型", sansMed,
                             pageFx.p0(impSlot), pageFx.p1(impSlot), nullptr, true, false)) {
        SDL_ShowOpenFileDialog(onFileDialog, dlgImport, window, nullptr, 0, nullptr, true);
    }
    if (ctl::quietButton(cdl, ImGui::GetID("models.mirror"), "镜像源", sansMed,
                         pageFx.p0(mirSlot), pageFx.p1(mirSlot), nullptr, true, false, geist::type::BODY)) {
        gMirrorOpen = !gMirrorOpen;
    }
    if (ctl::quietButton(cdl, ImGui::GetID("models.refresh"), "刷新列表", sansMed,
                         pageFx.p0(refSlot), pageFx.p1(refSlot), nullptr, true, false, geist::type::BODY)) {
        app.refreshModels();
    }
    if (ctl::quietButton(cdl, ImGui::GetID("models.open_dir"), "打开目录", sansMed,
                         pageFx.p0(dirSlot), pageFx.p1(dirSlot), nullptr, true, false, geist::type::BODY)) {
        std::error_code ec;
        if (!snap.models_dir.empty()) {
            std::filesystem::create_directories(snap.models_dir, ec);
            SDL_OpenURL(("file://" + snap.models_dir).c_str());
        }
    }

    if (gMirrorOpen) {
        const flex::Rect mRect = mirrorBar.rect();
        ctl::flatSection(cdl, mRect.p0(), mRect.p1(), geist::radius::MD, t.surface, t.outline);
        const flex::Rect mtR = pageFx.rect(mirTitleSlot);
        ellipsisTextCentered(cdl, sansMed, geist::type::BODY, mtR.x, mtR.y, mtR.bottom(), mtR.w, t.ink, "下载源");
        const flex::Rect segR = pageFx.rect(mirSegSlot);
        if (ctl::segmented(cdl, ImGui::GetID("models.source"), segR.p0(), segR.size(),
                           kSourceLabels, 3, sourceIdx, sans, geist::type::LABEL, false, true)) {
            app.setModelSource(kSourceValues[sourceIdx]);
        }
    }

    // Draw Filter & Search Toolbar
    if (singleRow) {
        const flex::Rect tbR = tb.rect();
        ctl::hairlineRule(cdl, tbR.x, tbR.right(), tbR.bottom() - hairlinePx(), t.outline);
    }
    const flex::Rect tr = pageFx.rect(tabsSlot);
    if (ctl::underlineTabs(cdl, ImGui::GetID("models.scene"), tr.p0(), tr.size(),
                           kTabLabels, 6, tabIdx, sansMed, geist::type::BODY, tabGap, false, true)) {
        ui.modelsSceneFilter = kTabValues[tabIdx];
    }
    const flex::Rect sr = pageFx.rect(searchSlot);
    ctl::searchBox(cdl, ImGui::GetID("models.search"), sr.p0(), sr.w, sr.h,
                   ui.modelsSearch, sizeof(ui.modelsSearch), "搜索模型...", sans, true);

    // Draw Banner
    if (showBanner) {
        const flex::Rect bRect = banner.rect();
        ctl::flatSection(cdl, bRect.p0(), bRect.p1(), geist::radius::MD, t.surface, t.outline);
        cdl->AddCircleFilled(snapPt(pageFx.rect(dotSlot).center()), 3.0f, t.accent);

        const flex::Rect msgR = pageFx.rect(msgSlot);
        ellipsisTextCentered(cdl, sans, geist::type::BODY_SMALL, msgR.x, msgR.y, msgR.bottom(),
                             msgR.w, t.ink, bannerMsg.c_str());

        if (hasDownload && pageFx.isVisible(barSlot)) {
            const flex::Rect barR = pageFx.rect(barSlot);
            const float frac = snap.download_pct < 0 ? 0.0f : std::clamp(snap.download_pct / 100.0f, 0.0f, 1.0f);
            ctl::progressBar(cdl, ImGui::GetID("models.banner_bar"), barR.p0(), barR.w, frac, ctl::geom::PROGRESS_H);
            if (pageFx.isVisible(pctTextSlot)) {
                const flex::Rect pctR = pageFx.rect(pctTextSlot);
                drawTextCenteredY(cdl, mono, geist::type::MONO, pctR.x, pctR.y, pctR.bottom(),
                                  t.accent, pctStr.c_str());
            }
        }

        if (ctl::iconAction(cdl, ImGui::GetID("models.clear_status"), pageFx.p0(closeSlot), 28.0f,
                            "x", "关闭提示", sans, 11.5f, geist::radius::MD, true, false)) {
            gDismissedBannerStatus = snap.models_status;
            app.clearModelsStatus();
        }
    }

    // Draw EmptyState or Model Family Groups & Rows
    if (groups.empty()) {
        if (drawEmptyState(cdl, pageFx, emptySlots, sans, sansMed, ImGui::GetID("models.clear_empty"),
                           emptyTitle, hasFilter)) {
            ui.modelsSearch[0] = '\0';
            ui.modelsSceneFilter = "all";
        }
    } else {
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            const auto& gs = groupSlots[gi];
            const std::string cnt = std::format("{}", g.items.size());
            const bool isAsciiGroup = std::all_of(g.title.begin(), g.title.end(),
                                                  [](unsigned char c) { return c < 0x80; });
            ImFont* groupHeadFont = isAsciiGroup ? mono : sansMed;
            const flex::Rect headR = pageFx.rect(gs.head);
            ctl::eyebrowHeader(cdl, groupHeadFont, headR.p0(), headR.w,
                               g.title.c_str(), cnt.c_str(), t.mute, t.mute, geist::type::BODY_SMALL);

            for (size_t ri = 0; ri < g.items.size(); ++ri) {
                drawModelRow(cdl, sans, sansMed, mono, pageFx.rect(gs.rows[ri]), rowOutset,
                             g.title, g.items[ri], overPage, snap, app);
            }
        }
    }

    cdl->PopClipRect();

    ctl::scrollThumb(cdl, ImVec2(paneX, paneY), ImVec2(paneX + paneW, paneY + paneH), gScroll, gContentH);

    ImGui::EndChild();
    ImGui::PopStyleVar(1);
}

} // namespace upix::views
