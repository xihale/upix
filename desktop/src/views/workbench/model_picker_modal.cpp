// views/workbench/model_picker_modal.cpp — Centered searchable model catalog modal.
#include "views/workbench/internal.hpp"
#include "views/models/grouping.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace upix::views::detail {
inline namespace wb {

namespace {

bool containsInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    const auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

} // namespace

void renderModelPickerModal(ImVec2 winSize, UiViewState& ui, upix::gui::AppState& app,
                            const upix::gui::Snapshot& snap, ModelPickerState& state,
                            bool justOpened) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    static int kbCursor = -1;
    static std::string lastQuery;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(winSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##model_picker_modal_layer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground);
    ImDrawList* mdl = ImGui::GetWindowDrawList();

    // Full-window backdrop scrim
    mdl->AddRectFilled(ImVec2(0.0f, 0.0f), winSize, withAlpha(t.scrim, t.scrimAlpha));

    struct FilteredGroup {
        std::string label;
        std::vector<const upix::gui::ModelItem*> items;
    };
    const std::string query(state.search);
    if (justOpened || query != lastQuery) {
        kbCursor = -1;
        if (query != lastQuery) {
            state.scroll = 0.0f;
            state.scrollTarget = 0.0f;
        }
        lastQuery = query;
    }

    std::vector<FilteredGroup> groups;
    std::unordered_map<std::string, size_t> groupIdxByLabel;
    std::vector<const upix::gui::ModelItem*> flatItems;
    size_t totalHits = 0;

    auto addGroupItems = [&](const std::string& grpLabel, const std::vector<upix::gui::ModelItem>& items) {
        for (const auto& m : items) {
            if (!containsInsensitive(m.name, query) &&
                !containsInsensitive(m.short_name, query) &&
                !containsInsensitive(m.scene, query) &&
                !containsInsensitive(grpLabel, query)) {
                continue;
            }
            const auto it = groupIdxByLabel.find(grpLabel);
            if (it == groupIdxByLabel.end()) {
                groupIdxByLabel[grpLabel] = groups.size();
                groups.push_back(FilteredGroup{grpLabel, {&m}});
            } else {
                groups[it->second].items.push_back(&m);
            }
            ++totalHits;
        }
    };

    if (!snap.installed_groups.empty() || !snap.remote_groups.empty()) {
        for (const auto& g : snap.installed_groups) addGroupItems(g.label, g.items);
        for (const auto& g : snap.remote_groups) addGroupItems(g.label, g.items);
    } else {
        if (!snap.installed.empty()) addGroupItems("已安装模型", snap.installed);
        if (!snap.remote.empty()) addGroupItems("可下载模型", snap.remote);
    }

    flatItems.reserve(totalHits);
    for (const auto& g : groups) {
        for (const auto* m : g.items) flatItems.push_back(m);
    }

    const float dlgW = std::min(660.0f, std::max(320.0f, winSize.x - 48.0f));
    const float dlgH = std::min(560.0f, std::max(280.0f, winSize.y - 64.0f));

    const char* manageLabel = "管理模型";
    const ImVec2 msz = calcText(mediumFont, geist::type::BODY_SMALL, manageLabel);
    const float inputH = std::max(30.0f, std::ceil(geist::type::BODY) + 8.0f);
    const float kBarH = std::max(50.0f, inputH + 18.0f);
    const float kFootH = std::max(44.0f, std::ceil(msz.y) + 20.0f);
    const float kHdrH = std::max(32.0f, std::ceil(calcText(mediumFont, geist::type::BODY_SMALL, "模型").y) + 12.0f);
    const float kRowH = std::max({52.0f, ctl::geom::ROW_MIN_H,
                                  std::ceil(calcText(mediumFont, geist::type::BODY, "模型").y) + 28.0f});
    constexpr float kGrpGap = 6.0f;

    static flex::Context modalCtx;
    flex::Layout fx(modalCtx);
    auto screen = fx.col({
        .padding = flex::Edges::xy(24.0f, 32.0f),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    auto dlg = screen.col({
        .width = flex::Fixed(dlgW),
        .height = flex::Fixed(dlgH),
    });
    auto bar = dlg.row({
        .padding = flex::Edges::xy(geist::space::LG, 0.0f),
        .gap = geist::space::MD,
        .align = flex::Align::Center,
        .height = flex::Fixed(kBarH),
    });
    const flex::Slot sIconSlot = bar.slot(flex::Fixed(18.0f), flex::Fixed(18.0f));
    const flex::Slot inputSlot = bar.slot(flex::Grow(1.0f).min(60.0f), flex::Fixed(inputH));
    const flex::Slot clearSlot = bar.slot({
        .width = flex::Fixed(28.0f),
        .height = flex::Fixed(28.0f),
        .visible = (state.search[0] != '\0'),
    });
    const flex::Slot closeSlot = bar.slot(flex::Fixed(28.0f), flex::Fixed(28.0f));

    const flex::Slot listSlot = dlg.slot(flex::Auto(), flex::Grow(1.0f));

    auto foot = dlg.row({
        .padding = flex::Edges::xy(geist::space::LG, 0.0f),
        .gap = geist::space::MD,
        .align = flex::Align::Center,
        .height = flex::Fixed(kFootH),
    });
    foot.spacer(1.0f);
    const flex::Slot mngSlot = foot.slot(
        flex::Fixed(std::ceil(msz.x) + 16.0f),
        flex::Fixed(std::max(28.0f, std::ceil(msz.y) + 10.0f)));

    fx.solve(flex::Rect{0.0f, 0.0f, winSize.x, winSize.y});

    const flex::Rect dlgRect = fx.rect(dlg);
    const flex::Rect barR = fx.rect(bar);
    const flex::Rect searchIconR = fx.rect(sIconSlot);
    const flex::Rect inputR = fx.rect(inputSlot);
    const flex::Rect clearSearchR = fx.rect(clearSlot);
    const flex::Rect closeR = fx.rect(closeSlot);
    const flex::Rect listR = fx.rect(listSlot);
    const flex::Rect footR = fx.rect(foot);
    const flex::Rect mngR = fx.rect(mngSlot);

    ctl::finish(mdl, dlgRect.p0(), dlgRect.p1(), ctl::Finish::Menu, geist::radius::LG);
    mdl->AddRectFilled(dlgRect.p0(), dlgRect.p1(), t.surface, geist::radius::LG);

    // ── Top search bar ──
    drawIcon(mdl, "search", searchIconR.center(), 17.0f, t.mute, 1.5f);
    {
        const float padY = snapPx(std::max(0.0f, (inputR.h - geist::type::BODY) * 0.5f));
        ImGui::PushID("##picker_modal_input");
        ImGui::SetCursorScreenPos(snapPt(inputR.x, inputR.y));
        if (sansFont) ImGui::PushFont(sansFont, geist::type::BODY);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, t.ink);
        ImGui::PushStyleColor(ImGuiCol_TextDisabled, t.faint);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, t.accentSoft);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(snapPx(2.0f), padY));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        ImGui::SetNextItemWidth(inputR.w);
        if (justOpened || ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::InputTextWithHint("##pk_q", "搜索模型…", state.search, sizeof(state.search));
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(5);
        if (sansFont) ImGui::PopFont();
        ImGui::PopID();
    }
    if (state.search[0] != '\0') {
        if (ctl::iconAction(mdl, ImGui::GetID("##picker_clear_q"), clearSearchR.p0(), clearSearchR.w, "x",
                            "清除搜索", sansFont, 13.0f, geist::radius::SM, true, false)) {
            state.search[0] = '\0';
        }
    }
    if (ctl::iconAction(mdl, ImGui::GetID("##picker_close"), closeR.p0(), closeR.w, "x",
                        "关闭 (Esc)", sansFont, 15.0f, geist::radius::SM, true, false)) {
        ui.showModelPicker = false;
    }
    ctl::hHairline(mdl, dlgRect.x + hairlinePx(), dlgRect.right() - hairlinePx(),
                   barR.bottom() - hairlinePx(), t.outline);

    // ── Precompute scrollable catalog layout & item positions via Flexbox ──
    static flex::Context listCtx;
    flex::Layout listFx(listCtx);
    static std::vector<flex::Slot> grpHdrSlots;
    static std::vector<flex::Slot> rowSlots;
    static std::vector<float> flatItemTop;
    grpHdrSlots.clear();
    rowSlots.clear();
    flatItemTop.clear();
    flatItemTop.reserve(totalHits);

    float listContentH = 96.0f;
    if (totalHits > 0) {
        auto listCol = listFx.col({
            .padding = flex::Edges::only(0.0f, 4.0f, 0.0f, 10.0f),
            .gap = kGrpGap,
        });
        grpHdrSlots.reserve(groups.size());
        rowSlots.reserve(totalHits);
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            const bool cjkHdr = ctl::hasNonAsciiUtf8(g.label.c_str());
            ImFont* hdrFont = cjkHdr ? mediumFont : monoFont;
            const float hdrFs = cjkHdr ? geist::type::BODY_SMALL : geist::type::EYEBROW;
            const float hdrBoxH = std::max(hdrFs, calcText(hdrFont, hdrFs, g.label.c_str()).y);

            auto gCol = listCol.col({.gap = 0.0f});
            auto hdrRow = gCol.row({
                .padding = flex::Edges::xy(geist::space::LG, 0.0f),
                .align = flex::Align::Center,
                .height = flex::Fixed(kHdrH),
            });
            grpHdrSlots.push_back(hdrRow.slot(flex::Grow(1.0f), flex::Fixed(hdrBoxH)));
            for (size_t mi = 0; mi < groups[gi].items.size(); ++mi) {
                rowSlots.push_back(gCol.slot(flex::Fraction(1.0f), flex::Fixed(kRowH)));
            }
        }
        listContentH = listFx.solveWidth(ImVec2(0.0f, 0.0f), listR.w, false).h;
        for (const auto& rs : rowSlots) {
            flatItemTop.push_back(listFx.rect(rs).y);
        }
    }

    const float maxScroll = std::max(0.0f, listContentH - listR.h);

    // Keyboard navigation (↑↓ · ↵ · Esc) with auto-scroll
    bool kbMoved = false;
    if (!flatItems.empty()) {
        const int nItems = static_cast<int>(flatItems.size());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            kbCursor = (kbCursor < 0) ? 0 : (kbCursor + 1) % nItems;
            kbMoved = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            kbCursor = (kbCursor <= 0) ? (nItems - 1) : (kbCursor - 1);
            kbMoved = true;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            const int pickIdx = (kbCursor >= 0 && kbCursor < nItems) ? kbCursor : 0;
            app.setModel(flatItems[static_cast<size_t>(pickIdx)]->name);
            ui.showModelPicker = false;
        }
    }
    if (kbMoved && kbCursor >= 0 && static_cast<size_t>(kbCursor) < flatItemTop.size() && maxScroll > 0.0f) {
        const float itemTop = flatItemTop[static_cast<size_t>(kbCursor)];
        const float itemBot = itemTop + kRowH;
        const float pad = 4.0f;
        if (itemTop - pad < state.scrollTarget) {
            state.scrollTarget = std::max(0.0f, itemTop - pad);
        } else if (itemBot + pad > state.scrollTarget + listR.h) {
            state.scrollTarget = std::min(maxScroll, itemBot + pad - listR.h);
        }
    }

    ImGuiIO& io = ImGui::GetIO();
    if (ctl::mouseIn(listR.p0(), listR.p1()) && std::fabs(io.MouseWheel) > 0.001f && maxScroll > 0.0f) {
        state.scrollTarget -= io.MouseWheel * 36.0f;
    }
    state.scrollTarget = std::clamp(state.scrollTarget, 0.0f, maxScroll);
    state.scroll = std::clamp(motion::smoothScroll(ImGui::GetID("wb.picker_scroll"), state.scrollTarget), 0.0f, maxScroll);

    const float frameInset = hairlinePx();
    const float rowW = std::max(1.0f, listR.w - frameInset * 2.0f - (maxScroll > 0.0f ? 10.0f : 0.0f));
    if (totalHits > 0) {
        listFx.solveWidth(ImVec2(listR.x + frameInset, listR.y - state.scroll), rowW);
    }

    const ImVec2 listClipMin(listR.x + frameInset, listR.y);
    const ImVec2 listClipMax(listR.right() - frameInset, std::max(listR.y + 2.0f, listR.bottom() - frameInset));
    mdl->PushClipRect(listClipMin, listClipMax, true);
    if (totalHits == 0) {
        const char* emptyTitle = "无匹配模型";
        drawTextCentered(mdl, sansFont, geist::type::BODY, listR.p0(), listR.p1(), t.mute, emptyTitle);
    } else {
        const bool dark = (geist::mode() == geist::Mode::Dark);
        constexpr float kActionBox = 28.0f;
        const float sizeColW = std::max(78.0f, std::ceil(calcText(monoFont, geist::type::MONO, "999.99 MB").x) + 6.0f);
        const float pctMaxW = std::ceil(calcText(monoFont, geist::type::MONO, "100%").x) + 2.0f;
        bool anyDlInPicker = false;
        for (const auto* item : flatItems) {
            if (item->remote && snap.downloading == item->name && snap.download_pct >= 0) {
                anyDlInPicker = true;
                break;
            }
        }
        const float trailColW = anyDlInPicker ? std::max(54.0f, pctMaxW + 14.0f) : kActionBox;

        int flatIdx = 0;
        static flex::Context rowCtx;
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            const auto& g = groups[gi];
            const flex::Rect hdrR = listFx.rect(grpHdrSlots[gi]);

            if (hdrR.bottom() >= listR.y && hdrR.y <= listR.bottom()) {
                char cntStr[16] = {};
                formatInto(cntStr, "{}", g.items.size());
                const bool cjkHdr = ctl::hasNonAsciiUtf8(g.label.c_str());
                ImFont* hdrFont = cjkHdr ? mediumFont : monoFont;
                ctl::eyebrowHeader(mdl, hdrFont, hdrR.p0(), hdrR.w, g.label.c_str(), cntStr);
            }

            for (size_t mi = 0; mi < g.items.size(); ++mi, ++flatIdx) {
                const auto* m = g.items[mi];
                const flex::Rect rowR = listFx.rect(rowSlots[static_cast<size_t>(flatIdx)]);

                if (rowR.bottom() < listR.y || rowR.y > listR.bottom()) continue;

                ImGui::PushID(m->remote ? "r" : "i");
                ImGui::PushID(m->name.c_str());

                const bool isSel = (m->name == snap.model);
                const bool isDl = (m->remote && snap.downloading == m->name && snap.download_pct >= 0);
                const bool isDlBtn = (m->remote && !isDl);
                const bool showRemCurChip = (isSel && m->remote);

                const std::string displayTitleStr(models_detail::stripFamilyPrefixForDisplay(g.label, *m));
                const char* rowTitle = displayTitleStr.c_str();
                const bool nameShortened = (displayTitleStr != m->name);
                const ImVec2 nameSz = calcText(mediumFont, geist::type::BODY, rowTitle);

                char scaleBuf[16] = {};
                formatInto(scaleBuf, "{}×", std::max(1, m->scale));
                const float scaleChipW = ctl::metaChipWidth(monoFont, scaleBuf, false, 6.0f);

                char sceneBuf[32] = {};
                const char* sLbl = ctl::sceneLabel(m->scene.c_str(), sceneBuf, sizeof(sceneBuf));
                const float sceneChipW = ctl::metaChipWidth(sansFont, sLbl, false, 6.0f);
                const float recChipW = m->recommended ? ctl::recChipWidth(sansFont, "推荐", 6.0f) : 0.0f;
                const float curChipW = showRemCurChip ? ctl::metaChipWidth(mediumFont, "当前", true, 6.0f) : 0.0f;

                const char* sizeStr = m->size.empty() ? "—" : m->size.c_str();
                const ImVec2 sizeSz = calcText(monoFont, geist::type::MONO, sizeStr);
                const bool showSize = (rowR.w >= 420.0f);

                char pctStr[16] = {};
                if (isDl) formatInto(pctStr, "{}%", std::clamp(snap.download_pct, 0, 100));
                const ImVec2 pctSz = isDl ? calcText(monoFont, geist::type::MONO, pctStr) : ImVec2{};

                flex::Layout rfx(rowCtx);
                auto rCol = rfx.col({
                    .padding = flex::Edges::xy(geist::space::LG, 0.0f),
                });
                auto r = rCol.row({
                    .gap = geist::space::SM,
                    .align = flex::Align::Center,
                    .height = flex::Grow(1.0f),
                });
                const flex::Slot nameSlot = r.slot(flex::Shrink(textBasis(nameSz.x), 1.0f).min(52.0f),
                                                   flex::Fixed(nameSz.y));
                const flex::Slot scaleSlot = r.slot(flex::Fixed(scaleChipW), flex::Fixed(ctl::geom::META_CHIP_H));
                const flex::Slot sceneSlot = r.slot(flex::Fixed(sceneChipW), flex::Fixed(ctl::geom::META_CHIP_H));
                const flex::Slot recSlot = r.slot({
                    .width = flex::Fixed(recChipW),
                    .height = flex::Fixed(ctl::geom::META_CHIP_H),
                    .visible = m->recommended,
                });
                const flex::Slot curSlot = r.slot({
                    .width = flex::Fixed(curChipW),
                    .height = flex::Fixed(ctl::geom::META_CHIP_H),
                    .visible = showRemCurChip,
                });
                r.spacer(1.0f);
                auto sizeBox = r.row({
                    .justify = flex::Justify::End,
                    .align = flex::Align::Center,
                    .width = flex::Fixed(sizeColW),
                    .height = flex::Fixed(sizeSz.y),
                    .visible = showSize,
                });
                const flex::Slot sizeSlot = sizeBox.slot(flex::Fixed(textBasis(sizeSz.x)), flex::Fixed(sizeSz.y));
                auto actBox = r.row({
                    .gap = 6.0f,
                    .justify = flex::Justify::End,
                    .align = flex::Align::Center,
                    .width = flex::Fixed(trailColW),
                    .height = flex::Fixed(kActionBox),
                });
                const flex::Slot busySlot = actBox.slot({
                    .width = flex::Fixed(8.0f),
                    .height = flex::Fixed(8.0f),
                    .visible = isDl,
                });
                const flex::Slot pctSlot = actBox.slot({
                    .width = flex::Fixed(textBasis(pctSz.x)),
                    .height = flex::Fixed(pctSz.y),
                    .visible = isDl,
                });
                const flex::Slot btnSlot = actBox.slot({
                    .width = flex::Fixed(kActionBox),
                    .height = flex::Fixed(kActionBox),
                    .visible = !isDl,
                });
                const flex::Slot dlBarSlot = rCol.slot({
                    .width = flex::Auto(),
                    .height = flex::Fixed(2.0f),
                    .visible = isDl,
                });

                rfx.solve(rowR);
                const flex::Rect nameR = rfx.rect(nameSlot);
                const flex::Rect scaleR = rfx.rect(scaleSlot);
                const flex::Rect sceneR = rfx.rect(sceneSlot);
                const flex::Rect recR = rfx.rect(recSlot);
                const flex::Rect curR = rfx.rect(curSlot);
                const flex::Rect sizeR = rfx.rect(sizeSlot);
                const flex::Rect busyR = rfx.rect(busySlot);
                const flex::Rect pctR = rfx.rect(pctSlot);
                const flex::Rect btnR = rfx.rect(btnSlot);
                const flex::Rect dlBarR = rfx.rect(dlBarSlot);

                const bool inListClip = ctl::mouseIn(listClipMin, listClipMax);
                const bool rowHov = inListClip && ctl::mouseIn(rowR.p0(), rowR.p1());
                const bool overDlBtn = isDlBtn && inListClip && ctl::mouseIn(btnR.p0(), btnR.p1());
                const bool isKb = (flatIdx == kbCursor);

                const float rowHovT = motion::hoverFactor(
                    ImGui::GetID("##pk_row_hov"),
                    (rowHov && !overDlBtn && !isDl) || isKb);

                const ImVec2 bg0 = snapPt(rowR.x + 6.0f, rowR.y + 1.0f);
                const ImVec2 bg1 = snapPt(rowR.right() - 6.0f, rowR.bottom() - 1.0f);
                const ImU32 restSelBg = dark ? t.high : t.container;
                const ImU32 hovSelBg = dark ? t.highest : t.high;
                if (isSel) {
                    const ImU32 fill = (rowHovT > 0.01f) ? motion::mix(restSelBg, hovSelBg, rowHovT) : restSelBg;
                    mdl->AddRectFilled(bg0, bg1, fill, geist::radius::MD);
                } else if (rowHovT > 0.01f) {
                    mdl->AddRectFilled(bg0, bg1, withAlpha(restSelBg, rowHovT), geist::radius::MD);
                }

                if (rowHov && !overDlBtn && !isDl) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                }

                drawTextFit(mdl, mediumFont, geist::type::BODY, nameR, t.ink, rowTitle, nameSz.x);
                if (nameShortened) {
                    ctl::tooltip(mdl, ImGui::GetID("##pk_name_tip"), nameR.p0(), nameR.p1(),
                                 m->name.c_str(), sansFont, inListClip && ctl::mouseIn(nameR.p0(), nameR.p1()), false);
                }
                ctl::metaChip(mdl, monoFont, scaleR.p0(), scaleBuf, 0, t.ink, t.tagBg, ctl::geom::META_CHIP_H, 6.0f);
                ctl::metaChip(mdl, sansFont, sceneR.p0(), sLbl, 0, t.body, t.tagBg, ctl::geom::META_CHIP_H, 6.0f);
                if (m->recommended) {
                    ctl::recChip(mdl, sansFont, recR.p0(), "推荐", ctl::geom::META_CHIP_H, 6.0f);
                }
                if (showRemCurChip) {
                    ctl::metaChip(mdl, mediumFont, curR.p0(), "当前", t.accent, t.accent, t.accentSoft,
                                  ctl::geom::META_CHIP_H, 6.0f);
                }

                if (showSize) {
                    drawTextCenteredY(mdl, monoFont, geist::type::MONO, sizeR.x, sizeR.y, sizeR.bottom(),
                                      t.mute, sizeStr);
                }

                bool dlClicked = false;
                if (isDl) {
                    ctl::busyArc(mdl, snapPt(busyR.center()), 3.5f, t.accent, 1.5f);
                    drawTextCenteredY(mdl, monoFont, geist::type::MONO,
                                      pctR.x, pctR.y, pctR.bottom(), t.accent, pctStr);
                    ctl::progressBar(mdl, ImGui::GetID("##pk_dl_bar"), dlBarR.p0(), dlBarR.w,
                                     std::clamp(static_cast<float>(snap.download_pct) / 100.0f, 0.0f, 1.0f),
                                     dlBarR.h);
                } else if (isDlBtn) {
                    const bool canDl = snap.downloading.empty();
                    const ImVec2 savedPos = io.MousePos;
                    if (!inListClip) io.MousePos = ImVec2(-1.0e6f, -1.0e6f);
                    dlClicked = ctl::iconAction(mdl, ImGui::GetID("##pk_dl"), btnR.p0(), kActionBox,
                                                "download", "下载模型", sansFont, 15.0f,
                                                geist::radius::MD, canDl, false);
                    if (!canDl && inListClip) {
                        ctl::tooltip(mdl, ImGui::GetID("##pk_dl_busy_tip"), btnR.p0(), btnR.p1(),
                                     "正在下载其他模型", sansFont, ctl::mouseIn(btnR.p0(), btnR.p1()), false);
                    }
                    if (dlClicked) {
                        app.downloadModel(m->name);
                    }
                    if (!inListClip) io.MousePos = savedPos;
                } else if (isSel) {
                    drawIcon(mdl, "check", snapPt(btnR.center()), 16.0f, t.accent, 1.85f);
                }

                if (rowHov && !dlClicked && !overDlBtn && !isDl &&
                    ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    app.setModel(m->name);
                    ui.showModelPicker = false;
                }

                ImGui::PopID();
                ImGui::PopID();
            }
        }
    }
    if (maxScroll > 0.0f) {
        const ImU32 fadeOpaque = t.surface;
        const ImU32 fadeClear = withAlpha(t.surface, 0.0f);
        const float fadeH = std::min(14.0f, listR.h * 0.15f);
        if (fadeH > 1.0f) {
            if (state.scroll > 1.0f) {
                mdl->AddRectFilledMultiColor(
                    snapPt(listClipMin.x, listClipMin.y),
                    snapPt(listClipMax.x, listClipMin.y + fadeH),
                    fadeOpaque, fadeOpaque, fadeClear, fadeClear);
            }
            if (state.scroll < maxScroll - 1.0f) {
                mdl->AddRectFilledMultiColor(
                    snapPt(listClipMin.x, listClipMax.y - fadeH),
                    snapPt(listClipMax.x, listClipMax.y),
                    fadeClear, fadeClear, fadeOpaque, fadeOpaque);
            }
        }
    }
    mdl->PopClipRect();

    ctl::scrollThumb(mdl, listR.p0(), listR.p1(), state.scroll, listContentH);

    // ── Bottom footer (t.panelBg, 1px t.outline top border, inset from outer border) ──
    mdl->AddRectFilled(
        snapPt(footR.x + frameInset, footR.y),
        snapPt(footR.right() - frameInset, footR.bottom() - frameInset),
        t.panelBg, std::max(0.0f, geist::radius::LG - frameInset), ImDrawFlags_RoundCornersBottom);
    ctl::hHairline(mdl, footR.x + frameInset, footR.right() - frameInset, footR.y, t.outline);

    if (ctl::quietButton(mdl, ImGui::GetID("##picker_manage_models"), manageLabel, mediumFont,
                         mngR.p0(), mngR.p1(), nullptr, true, false, geist::type::BODY_SMALL)) {
        ui.showModelPicker = false;
        ui.activeTab = 1;
    }

    // Outer 1px frame drawn last so scrolled content and footer never clip the border.
    mdl->AddRect(dlgRect.p0(), dlgRect.p1(), t.outline, geist::radius::LG, 0, frameInset);

    // Scrim click closes modal
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ctl::mouseIn(dlgRect.p0(), dlgRect.p1())) {
        ui.showModelPicker = false;
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

} // namespace wb
} // namespace upix::views::detail
