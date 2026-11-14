// views/workbench/inspector.cpp — Right-hand scrollable parameter inspector and sticky CTA dock.
#include "views/workbench/internal.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace upix::views::detail {
inline namespace wb {

namespace {

struct ActiveModelInfo {
    const char* displayShort = nullptr;
    const char* scene = "";
    int scale = 4;
    bool recommended = false;
    bool installed = false;
    bool downloading = false;
    std::string size;
};

ActiveModelInfo inspectActiveModel(const upix::gui::Snapshot& snap) {
    ActiveModelInfo info;
    info.displayShort = snap.model.empty()
        ? upix::i18n::tr(upix::i18n::Key::WbErrSelectModel)
        : snap.model.c_str();
    auto scan = [&](const std::vector<upix::gui::ModelItem>& list, bool installedFlag) {
        for (const auto& m : list) {
            if (m.name == snap.model) {
                if (!m.short_name.empty()) info.displayShort = m.short_name.c_str();
                info.scene = m.scene.c_str();
                info.scale = std::max(1, m.scale);
                info.recommended = m.recommended;
                info.installed = installedFlag;
                info.size = m.size;
                return true;
            }
        }
        return false;
    };
    if (!scan(snap.installed, true)) {
        scan(snap.remote, false);
    }
    info.downloading = (!snap.model.empty() && snap.downloading == snap.model && snap.download_pct >= 0);
    return info;
}

struct OutputFormatSpec {
    std::string_view effectiveFmt;
    bool showQuality = false;
    bool showChroma = false;
    bool showSpeed = false;
};

OutputFormatSpec resolveOutputFormat(const upix::gui::Snapshot& snap,
                                     const upix::gui::InputItem* activeItem) {
    OutputFormatSpec spec;
    const bool isFollow = (!snap.overwrite_original && snap.format.empty());
    const char* inferredFmt = (isFollow && !snap.inputs.empty()) ? inferInputFormatLabel(activeItem) : "";
    if (!snap.overwrite_original) {
        if (!isFollow) {
            spec.effectiveFmt = snap.format;
        } else if (std::string_view(inferredFmt) == "PNG") {
            spec.effectiveFmt = "png";
        } else if (std::string_view(inferredFmt) == "JPEG") {
            spec.effectiveFmt = "jpg";
        } else if (std::string_view(inferredFmt) == "WebP") {
            spec.effectiveFmt = "webp";
        } else if (std::string_view(inferredFmt) == "AVIF") {
            spec.effectiveFmt = "avif";
        }
    }

    spec.showQuality = (spec.effectiveFmt == "jpg") ||
                       (spec.effectiveFmt == "webp" && !snap.webp_lossless) ||
                       (spec.effectiveFmt == "avif");
    spec.showChroma = (spec.effectiveFmt == "jpg" || spec.effectiveFmt == "avif");
    spec.showSpeed = (spec.effectiveFmt == "avif");
    return spec;
}

enum class CtaMode {
    CancelRunning,
    DownloadMissingModel,
    DisabledPrereq,
    CompareChanged,
    CompareReady,
    StartOrResume,
};

} // namespace

void openFileInSystem(const std::string& path) {
    if (path.empty() || path.rfind("__fixture", 0) == 0) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return;
    openLocalPathInSystem(path);
}

void revealFileParentInSystem(const std::string& path) {
    if (path.empty() || path.rfind("__fixture", 0) == 0) return;
    std::error_code ec;
    const std::filesystem::path p(path);
    const std::filesystem::path dir = std::filesystem::is_directory(p, ec) ? p : p.parent_path();
    if (!dir.empty() && std::filesystem::exists(dir, ec)) {
        openLocalPathInSystem(dir);
    }
}

void renderInspectorPane(ImDrawList* dl, const upix::ui::flex::Rect& inspRect,
                         UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                         const upix::gui::InputItem* activeItem, SDL_Window* window,
                         DialogBridge* dlgOutDir, bool isRunning, bool isCompare, bool isError,
                         bool modal, ImVec2 savedMousePos,
                         InspectorState& state, ModelPickerState& pickerState) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImGuiIO& io = ImGui::GetIO();

    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    dl->AddRectFilled(inspRect.p0(), inspRect.p1(), t.panelBg);

    // Track parameter signature while in Done/Compare phase to flip CTA to "重新超分" on edits.
    const std::string curParamSig = std::format(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        snap.model, snap.format, snap.quality, snap.chroma, snap.avif_speed,
        snap.webp_lossless ? 1 : 0, snap.tile_size,
        snap.gpu ? 1 : 0, snap.force ? 1 : 0,
        snap.overwrite_original ? 1 : 0);
    if (isCompare) {
        if (state.prevJobPhase != upix::gui::JobPhase::Done ||
            state.prevDoneOutputs != snap.job.outputs ||
            state.doneParamSig.empty()) {
            state.doneParamSig = curParamSig;
            state.prevDoneOutputs = snap.job.outputs;
        }
    } else {
        state.doneParamSig.clear();
        state.prevDoneOutputs.clear();
    }
    state.prevJobPhase = snap.job.phase;
    const bool doneParamsChanged = isCompare && (curParamSig != state.doneParamSig);

    static const std::string kEmptyStr;
    const std::string& outPath = (activeItem && !activeItem->output_path.empty())
        ? activeItem->output_path
        : (snap.job.outputs.empty() ? kEmptyStr : snap.job.outputs.back());
    const bool hasOut = !outPath.empty();
    const bool hasActiveDone = !isCompare && activeItem && activeItem->status_kind == 2 && hasOut;

    CtaMode ctaMode = CtaMode::StartOrResume;
    if (isRunning) {
        ctaMode = CtaMode::CancelRunning;
    } else if (isError && !snap.job.missing_model.empty()) {
        ctaMode = CtaMode::DownloadMissingModel;
    } else if (snap.inputs.empty() || snap.model.empty()) {
        ctaMode = CtaMode::DisabledPrereq;
    } else if (isCompare) {
        ctaMode = doneParamsChanged ? CtaMode::CompareChanged : CtaMode::CompareReady;
    }

    const float dockInnerW = std::max(120.0f, inspRect.w - 40.0f);
    const float ctaPriH = ctl::geom::CTA_H;
    const float ctaSecH = std::max(ctl::geom::BTN_H + 4.0f, ctaPriH - 4.0f);
    using upix::i18n::Key;
    using upix::i18n::tr;
    const float openImgBtnW = std::max(84.0f, std::ceil(calcText(sansFont, geist::type::BODY, tr(Key::CommonOpenImage)).x) + 24.0f);
    const float revealBtnW = std::max(84.0f, std::ceil(calcText(sansFont, geist::type::BODY, tr(Key::CommonRevealFile)).x) + 24.0f);
    const float rerunPriMinW = std::ceil(calcText(mediumFont, geist::type::BODY, tr(Key::InspectorCtaRerun)).x) + 28.0f;
    const char* cmpOpenLabel = (activeItem && activeItem->is_dir) ? tr(Key::CommonOpenDir) : tr(Key::CommonOpenImage);
    const float cmpOpenMinW = std::ceil(calcText(mediumFont, geist::type::BODY, cmpOpenLabel).x) + 28.0f;
    const bool doneNeedsTwoRows = isCompare && (doneParamsChanged
        ? (dockInnerW < rerunPriMinW + geist::space::SM + openImgBtnW)
        : (dockInnerW < cmpOpenMinW + revealBtnW + ctaPriH + geist::space::SM * 2.0f));

    // 1. Solve outer Inspector Pane (Scroll Viewport + Hairline Seam + Sticky CTA Dock) in one tree.
    static flex::Context paneCtx;
    flex::Layout paneFx(paneCtx);
    auto paneCol = paneFx.col({
        .align = flex::Align::Stretch,
    });
    const flex::Slot scrollSlot = paneCol.slot(flex::Auto(), flex::Grow(1.0f).min(64.0f));
    const flex::Slot seamSlot = paneCol.slot(flex::Auto(), flex::Fixed(hairlinePx()));
    auto ctaDock = paneCol.col({
        .padding = flex::Edges::only(20.0f, 16.0f, 20.0f, 20.0f),
        .gap = geist::space::SM,
        .align = flex::Align::Stretch,
        .height = flex::Auto(),
    });

    flex::Slot ctaPrimarySlot, ctaSecondarySlot, ctaIconSlot;
    if (ctaMode == CtaMode::CompareChanged) {
        if (!hasOut) {
            ctaPrimarySlot = ctaDock.slot(flex::Auto(), flex::Fixed(ctaPriH));
        } else if (doneNeedsTwoRows) {
            ctaPrimarySlot = ctaDock.slot(flex::Auto(), flex::Fixed(ctaPriH));
            ctaSecondarySlot = ctaDock.slot(flex::Auto(), flex::Fixed(ctaSecH));
        } else {
            auto row = ctaDock.row({.gap = geist::space::SM, .align = flex::Align::Center});
            ctaPrimarySlot = row.slot(flex::Grow(1.0f), flex::Fixed(ctaPriH));
            ctaSecondarySlot = row.slot(flex::Fixed(openImgBtnW), flex::Fixed(ctaPriH));
        }
    } else if (ctaMode == CtaMode::CompareReady) {
        if (doneNeedsTwoRows) {
            ctaPrimarySlot = ctaDock.slot(flex::Auto(), flex::Fixed(ctaPriH));
            auto subRow = ctaDock.row({.gap = geist::space::SM, .align = flex::Align::Center});
            ctaSecondarySlot = subRow.slot(flex::Grow(1.0f), flex::Fixed(ctaSecH));
            ctaIconSlot = subRow.slot(flex::Fixed(ctaSecH), flex::Fixed(ctaSecH));
        } else {
            auto row = ctaDock.row({.gap = geist::space::SM, .align = flex::Align::Center});
            ctaPrimarySlot = row.slot(flex::Grow(1.0f), flex::Fixed(ctaPriH));
            ctaSecondarySlot = row.slot(flex::Fixed(revealBtnW), flex::Fixed(ctaPriH));
            ctaIconSlot = row.slot(flex::Fixed(ctaPriH), flex::Fixed(ctaPriH));
        }
    } else if (ctaMode == CtaMode::StartOrResume && hasActiveDone) {
        auto row = ctaDock.row({.gap = geist::space::SM, .align = flex::Align::Center});
        ctaPrimarySlot = row.slot(flex::Grow(1.0f), flex::Fixed(ctaPriH));
        ctaSecondarySlot = row.slot(flex::Fixed(openImgBtnW), flex::Fixed(ctaPriH));
    } else {
        ctaPrimarySlot = ctaDock.slot(flex::Auto(), flex::Fixed(ctaPriH));
    }

    paneFx.solve(inspRect);

    const flex::Rect scrollViewport{
        paneFx.rect(scrollSlot).x,
        paneFx.rect(scrollSlot).y,
        std::max(1.0f, paneFx.rect(scrollSlot).w),
        std::max(1.0f, paneFx.rect(scrollSlot).h),
    };
    const float inspSeamY = paneFx.rect(seamSlot).y;

    // 2. Prepare Inspector Body Model & Output Metadata.
    const ActiveModelInfo mod = inspectActiveModel(snap);
    char modHeadRightBuf[32] = {};
    const char* modHeadRight = nullptr;
    if (mod.downloading) {
        modHeadRight = formatInto(modHeadRightBuf, "{}%", snap.download_pct);
    } else if (!snap.model.empty() && !mod.installed && !mod.size.empty()) {
        modHeadRight = mod.size.c_str();
    }

    const OutputFormatSpec fmtSpec = resolveOutputFormat(snap, activeItem);

    const bool dirLocked = snap.overwrite_original;
    const bool hasCustom = !snap.custom_out_dir.empty();

    char modScaleBuf[16] = {};
    formatInto(modScaleBuf, "{}×", mod.scale);
    char modSceneBuf[32] = {};
    const char* modSceneLbl = ctl::sceneLabel(mod.scene, modSceneBuf, sizeof(modSceneBuf));
    const float scaleChipW = ctl::metaChipWidth(monoFont, modScaleBuf, false, 6.0f);
    const float sceneChipW = ctl::metaChipWidth(sansFont, modSceneLbl, false, 6.0f);
    const float recChipW = mod.recommended ? ctl::recChipWidth(sansFont, tr(Key::CommonRecommended), 6.0f) : 0.0f;

    const ImVec2 modNameSz = calcText(mediumFont, geist::type::BODY, mod.displayShort);

    static StaticTextCache hdr1Cache, hdr2Cache, hdr3Cache, qlCache, clCache, slCache;
    static StaticTextCache dirBtnCache, resetCache, ovTCache, bkTCache, outStaticCache;
    const ImVec2 hdr1Sz = hdr1Cache.get(mediumFont, geist::type::BODY_SMALL, tr(Key::InspectorSecModel));
    const ImVec2 hdr2Sz = hdr2Cache.get(mediumFont, geist::type::BODY_SMALL, tr(Key::InspectorSecOutput));
    const ImVec2 hdr3Sz = hdr3Cache.get(mediumFont, geist::type::BODY_SMALL, tr(Key::InspectorSecExport));
    const float hdr1RightH = modHeadRight ? calcText(mediumFont, geist::type::BODY_SMALL, modHeadRight).y : 0.0f;
    const float hdr1H = std::max({geist::type::BODY_SMALL, hdr1Sz.y, hdr1RightH});
    const float hdr2H = std::max(geist::type::BODY_SMALL, hdr2Sz.y);
    const float hdr3H = std::max(geist::type::BODY_SMALL, hdr3Sz.y);

    const ImVec2 qlSz = fmtSpec.showQuality ? qlCache.get(sansFont, geist::type::BODY, tr(Key::InspectorQuality)) : ImVec2{};
    const ImVec2 clSz = fmtSpec.showChroma ? clCache.get(sansFont, geist::type::BODY, tr(Key::SettingsChroma)) : ImVec2{};
    const ImVec2 slSz = fmtSpec.showSpeed ? slCache.get(sansFont, geist::type::BODY, tr(Key::SettingsEncodeSpeed)) : ImVec2{};

    auto calcSegW = [&](ImFont* font, float fontPx, const char* const* labels, int count, float minCellW) {
        float maxW = 0.0f;
        for (int i = 0; i < count; ++i) {
            maxW = std::max(maxW, calcText(font, fontPx, labels[i]).x);
        }
        const float cellW = std::max(minCellW, std::ceil(maxW + 28.0f));
        return cellW * static_cast<float>(count) + ctl::geom::SEG_PAD * 2.0f;
    };
    static constexpr const char* kChromaLabels[2] = {"4:4:4", "4:2:0"};
    const char* kSpeedLabels[3] = {
        tr(Key::SettingsSpeedFine),
        tr(Key::SettingsSpeedBalanced),
        tr(Key::SettingsSpeedFast),
    };
    const float chromaSegW = calcSegW(monoFont, geist::type::BODY_SMALL, kChromaLabels, 2, 64.0f);
    const float speedSegW = calcSegW(sansFont, geist::type::BODY_SMALL, kSpeedLabels, 3, 58.0f);

    const char* dirBtnLabel = hasCustom ? tr(Key::InspectorChangeDir) : tr(Key::InspectorChooseDir);
    const ImVec2 dirBtnSz = dirBtnCache.get(sansFont, geist::type::BODY, dirBtnLabel);
    const float dirBtnW = textBasis(dirBtnSz.x) + 20.0f;
    const float dirBtnH = std::max(32.0f, std::ceil(dirBtnSz.y) + 8.0f);
    const float dirBoxH = std::max(44.0f, dirBtnH + 10.0f);
    const float resetW = (hasCustom && !dirLocked)
        ? (textBasis(resetCache.get(sansFont, geist::type::BODY, tr(Key::CommonReset)).x) + 18.0f)
        : 0.0f;

    char outBuf[256] = {};
    const char* outLabel = tr(Key::InspectorFollowOrigDir);
    ImFont* outFont = sansFont;
    float outFontPx = geist::type::BODY_SMALL;
    bool outIsStatic = false;
    if (dirLocked) {
        outLabel = tr(Key::InspectorOverwriteInPlace);
        outIsStatic = true;
    } else if (hasCustom) {
        outLabel = snap.custom_out_dir.c_str();
        outFont = monoFont;
        outFontPx = geist::type::MONO;
    } else {
        bool hasBatchRoot = false;
        for (const auto& it : snap.inputs) {
            if (!it.batch_root.empty()) {
                hasBatchRoot = true;
                break;
            }
        }
        if (hasBatchRoot) {
            outLabel = formatInto(outBuf, "upix_{}/", snap.model.empty() ? "out" : snap.model);
            outFont = monoFont;
            outFontPx = geist::type::MONO;
        } else {
            outIsStatic = true;
        }
    }
    const ImVec2 outSz = outIsStatic
        ? outStaticCache.get(outFont, outFontPx, outLabel)
        : calcText(outFont, outFontPx, outLabel);

    const char* ovTitle = tr(Key::InspectorOverwriteOrig);
    const char* bkTitle = tr(Key::InspectorBackupOrig);
    const ImVec2 ovTSz = ovTCache.get(sansFont, geist::type::BODY, ovTitle);
    const ImVec2 bkTSz = snap.overwrite_original ? bkTCache.get(sansFont, geist::type::BODY, bkTitle) : ImVec2{};

    // 3. Build unified Inspector Scrollable Body Flexbox tree.
    static flex::Context bodyCtx;
    flex::Layout fx(bodyCtx);
    auto body = fx.col({
        .padding = flex::Edges::xy(20.0f, 24.0f),
        .gap = 26.0f,
    });

    // Section 1: 模型
    auto sec1 = body.col({.gap = 12.0f});
    const flex::Slot hdr1Slot = sec1.slot(flex::Auto(), flex::Fixed(hdr1H));
    auto trigCol = sec1.col({
        .padding = flex::Edges::xy(12.0f, 12.0f),
        .gap = 8.0f,
    });
    auto trigTop = trigCol.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
    });
    const flex::Slot mpNameSlot = trigTop.slot(flex::Grow(1.0f).min(40.0f), flex::Fixed(modNameSz.y));
    const flex::Slot mpChevSlot = trigTop.slot(flex::Fixed(16.0f), flex::Fixed(16.0f));
    auto trigTags = trigCol.row({
        .gap = 6.0f,
        .rowGap = 6.0f,
        .align = flex::Align::Center,
        .wrap = flex::Wrap::Wrap,
    });
    const flex::Slot mpScaleSlot = trigTags.slot(flex::Fixed(scaleChipW), flex::Fixed(ctl::geom::META_CHIP_H));
    const flex::Slot mpSceneSlot = trigTags.slot(flex::Fixed(sceneChipW), flex::Fixed(ctl::geom::META_CHIP_H));
    const flex::Slot mpRecSlot = trigTags.slot({
        .width = flex::Fixed(recChipW),
        .height = flex::Fixed(ctl::geom::META_CHIP_H),
        .visible = mod.recommended,
    });
    const flex::Slot mpBarSlot = sec1.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(3.0f),
        .visible = mod.downloading,
    });

    // Section 2: 输出
    auto sec2 = body.col({.gap = 14.0f});
    const flex::Slot hdr2Slot = sec2.slot(flex::Auto(), flex::Fixed(hdr2H));
    const flex::Slot fmtSegSlot = sec2.slot(flex::Auto(), flex::Fixed(ctl::geom::SEG_H_COMPACT));

    auto qGroup = sec2.col({
        .gap = 8.0f,
        .visible = fmtSpec.showQuality,
    });
    auto qRow = qGroup.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
    });
    const flex::Slot qLblSlot = qRow.slot(flex::Grow(1.0f), flex::Fixed(qlSz.y));
    const flex::Slot qInSlot = qRow.slot(flex::Fixed(56.0f), flex::Fixed(26.0f));
    const flex::Slot qSliderSlot = qGroup.slot(flex::Auto(), flex::Fixed(ctl::geom::SLIDER_H));

    auto chromaRow = sec2.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .visible = fmtSpec.showChroma,
    });
    const flex::Slot chromaLblSlot = chromaRow.slot(flex::Grow(1.0f), flex::Fixed(clSz.y));
    const flex::Slot chromaSegSlot = chromaRow.slot(flex::Shrink(chromaSegW, 1.0f).min(116.0f),
                                                    flex::Fixed(ctl::geom::SEG_H_COMPACT));

    auto speedRow = sec2.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .visible = fmtSpec.showSpeed,
    });
    const flex::Slot speedLblSlot = speedRow.slot(flex::Grow(1.0f), flex::Fixed(slSz.y));
    const flex::Slot speedSegSlot = speedRow.slot(flex::Shrink(speedSegW, 1.0f).min(144.0f),
                                                  flex::Fixed(ctl::geom::SEG_H_COMPACT));

    // Section 3: 导出设置 (always expanded)
    auto sec3 = body.col({.gap = 14.0f});
    const flex::Slot accRuleSlot = sec3.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(hairlinePx()),
        .margin = flex::Edges::only(0.0f, 0.0f, 0.0f, 2.0f),
    });
    const flex::Slot hdr3Slot = sec3.slot(flex::Auto(), flex::Fixed(hdr3H));

    auto dirBox = sec3.row({
        .padding = flex::Edges::xy(geist::space::SM, 0.0f),
        .gap = 6.0f,
        .align = flex::Align::Center,
        .height = flex::Fixed(dirBoxH),
    });
    const flex::Slot dirIconSlot = dirBox.slot(flex::Fixed(16.0f), flex::Fixed(16.0f));
    const flex::Slot dirPathSlot = dirBox.slot(flex::Grow(1.0f).min(28.0f), flex::Fixed(outSz.y));
    const flex::Slot dirResetSlot = dirBox.slot({
        .width = flex::Fixed(resetW),
        .height = flex::Fixed(dirBtnH),
        .visible = hasCustom && !dirLocked,
    });
    const flex::Slot dirPickSlot = dirBox.slot(flex::Fixed(dirBtnW), flex::Fixed(dirBtnH));

    struct SwitchSlots {
        flex::Slot tSlot, swSlot;
    };
    auto addSwitchRow = [&](ImVec2 tSz, bool vis) -> SwitchSlots {
        const float rowH = std::max(tSz.y, ctl::geom::TOGGLE_H) + 4.0f;
        auto swRow = sec3.row({
            .gap = geist::space::SM,
            .align = flex::Align::Center,
            .height = flex::Fixed(rowH),
            .visible = vis,
        });
        const flex::Slot ts = swRow.slot(flex::Grow(1.0f).min(48.0f), flex::Fixed(tSz.y));
        const flex::Slot sw = swRow.slot(flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H));
        return {ts, sw};
    };

    const SwitchSlots ovSlots = addSwitchRow(ovTSz, true);
    const SwitchSlots bkSlots = addSwitchRow(bkTSz, snap.overwrite_original);

    const float contentH = fx.solveWidth(scrollViewport.p0(), scrollViewport.w, false).h;
    const float maxInspScroll = std::max(0.0f, contentH - scrollViewport.h);
    if (!modal && ctl::mouseIn(scrollViewport.p0(), scrollViewport.p1()) &&
        std::fabs(io.MouseWheel) > 0.001f && maxInspScroll > 0.0f) {
        state.scrollTarget -= io.MouseWheel * 36.0f;
    }
    state.scrollTarget = std::clamp(state.scrollTarget, 0.0f, maxInspScroll);
    state.scroll = std::clamp(motion::smoothScroll(ImGui::GetID("wb.insp_scroll"), state.scrollTarget),
                              0.0f, maxInspScroll);

    // Solve at scrolled origin (hits per-node measurement cache; snaps final coordinates directly).
    fx.solveWidth(ImVec2(scrollViewport.x, scrollViewport.y - snapPx(state.scroll)), scrollViewport.w, true);

    const flex::Rect hdr1R = fx.rect(hdr1Slot);
    const flex::Rect mpR = fx.rect(trigCol);
    const flex::Rect mpNameR = fx.rect(mpNameSlot);
    const flex::Rect mpChevR = fx.rect(mpChevSlot);
    const flex::Rect mpScaleR = fx.rect(mpScaleSlot);
    const flex::Rect mpSceneR = fx.rect(mpSceneSlot);
    const flex::Rect mpRecR = fx.rect(mpRecSlot);
    const flex::Rect mpBarR = fx.rect(mpBarSlot);

    const flex::Rect hdr2R = fx.rect(hdr2Slot);
    const flex::Rect fmtSegR = fx.rect(fmtSegSlot);
    const flex::Rect qLblR = fx.rect(qLblSlot);
    const flex::Rect qInR = fx.rect(qInSlot);
    const flex::Rect qSliderR = fx.rect(qSliderSlot);
    const flex::Rect chromaLblR = fx.rect(chromaLblSlot);
    const flex::Rect chromaSegR = fx.rect(chromaSegSlot);
    const flex::Rect speedLblR = fx.rect(speedLblSlot);
    const flex::Rect speedSegR = fx.rect(speedSegSlot);

    const flex::Rect accRuleR = fx.rect(accRuleSlot);
    const flex::Rect hdr3R = fx.rect(hdr3Slot);
    const flex::Rect dirBoxR = fx.rect(dirBox);
    const flex::Rect dirIconR = fx.rect(dirIconSlot);
    const flex::Rect dirPathR = fx.rect(dirPathSlot);
    const flex::Rect dirResetR = fx.rect(dirResetSlot);
    const flex::Rect dirPickR = fx.rect(dirPickSlot);
    const flex::Rect ovTitleR = fx.rect(ovSlots.tSlot);
    const flex::Rect ovSwR = fx.rect(ovSlots.swSlot);
    const flex::Rect bkTitleR = fx.rect(bkSlots.tSlot);
    const flex::Rect bkSwR = fx.rect(bkSlots.swSlot);

    const bool mouseOutsideScroll = !modal && !ctl::mouseIn(scrollViewport.p0(), scrollViewport.p1());
    if (mouseOutsideScroll) {
        io.MousePos = ImVec2(-1.0e6f, -1.0e6f);
    }

    dl->PushClipRect(scrollViewport.p0(), scrollViewport.p1(), true);

    auto ensureVisibleOnFocus = [&](const flex::Rect& r) {
        if (ImGui::IsItemFocused() && maxInspScroll > 0.0f) {
            if (r.y < scrollViewport.y) {
                state.scrollTarget = std::max(0.0f, state.scrollTarget - (scrollViewport.y - r.y) - geist::space::MD);
            } else if (r.bottom() > scrollViewport.bottom()) {
                state.scrollTarget = std::min(maxInspScroll, state.scrollTarget + (r.bottom() - scrollViewport.bottom()) + geist::space::MD);
            }
        }
    };

    const bool dark = (geist::mode() == geist::Mode::Dark);

    // ── Section 1: 模型 ──
    ctl::eyebrowHeader(dl, mediumFont, hdr1R.p0(), hdr1R.w, tr(Key::InspectorSecModel), modHeadRight,
                       t.mute, mod.downloading ? t.ink : t.mute);
    {
        ImGui::SetCursorScreenPos(mpR.p0());
        const bool trigClicked = ImGui::InvisibleButton("##model_picker_btn", mpR.size());
        const bool trigHov = ImGui::IsItemHovered();
        const bool trigHeld = ImGui::IsItemActive();
        const bool trigFocused = ImGui::IsItemFocused();
        ensureVisibleOnFocus(mpR);
        if (trigHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        const float hovT = motion::hoverFactor(ImGui::GetID("##mp_hov"), trigHov || ui.showModelPicker);
        const float liftT = trigHeld ? 1.0f : hovT;
        const ImU32 trigHovBg = dark ? (trigHeld ? t.highest : t.high) : (trigHeld ? t.high : t.container);
        const ImU32 trigFill = motion::mix(t.surface, trigHovBg, liftT);
        const ImU32 trigBorder = motion::mix(t.outline, t.outlineStrong, liftT);
        dl->AddRectFilled(mpR.p0(), mpR.p1(), trigFill, geist::radius::MD);
        dl->AddRect(mpR.p0(), mpR.p1(), trigBorder, geist::radius::MD, 0, hairlinePx());
        ctl::detail::focusRing(dl, mpR.p0(), mpR.p1(), geist::radius::MD, trigFocused);

        const ImU32 nameCol = (mod.installed || snap.model.empty())
            ? t.ink
            : motion::mix(t.mute, t.ink, liftT * 0.6f);
        drawTextFit(dl, mediumFont, geist::type::BODY, mpNameR, nameCol, mod.displayShort, modNameSz.x);
        drawIcon(dl, ui.showModelPicker ? "chevron-up" : "chevron-down",
                 mpChevR.center(), 15.0f, motion::mix(t.mute, t.ink, liftT));

        ctl::metaChip(dl, monoFont, mpScaleR.p0(), modScaleBuf, 0, t.ink, t.tagBg, ctl::geom::META_CHIP_H, 6.0f);
        ctl::metaChip(dl, sansFont, mpSceneR.p0(), modSceneLbl, 0, t.body, t.tagBg, ctl::geom::META_CHIP_H, 6.0f);
        if (mod.recommended) {
            ctl::recChip(dl, sansFont, mpRecR.p0(), tr(Key::CommonRecommended), ctl::geom::META_CHIP_H, 6.0f);
        }

        if (trigClicked) {
            ui.showModelPicker = !ui.showModelPicker;
            if (ui.showModelPicker) {
                pickerState.reset();
            }
        }
    }
    if (fx.isVisible(mpBarSlot)) {
        ctl::progressBar(dl, ImGui::GetID("##mp_dl_bar"), mpBarR.p0(), mpBarR.w,
                         static_cast<float>(snap.download_pct) / 100.0f, 3.0f);
    }

    // ── Section 2: 输出 ──
    ctl::eyebrowHeader(dl, mediumFont, hdr2R.p0(), tr(Key::InspectorSecOutput));

    const char* fmtLabels[5] = {tr(Key::CommonFollow), "PNG", "JPEG", "WebP", "AVIF"};
    const char* fmtVals[5] = {"", "png", "jpg", "webp", "avif"};
    int fmtIdx = 0;
    if (!snap.overwrite_original) {
        for (int i = 0; i < 5; ++i) {
            if (snap.format == fmtVals[i]) {
                fmtIdx = i;
                break;
            }
        }
    }
    float maxFmtItemW = 0.0f;
    for (int i = 0; i < 5; ++i) {
        maxFmtItemW = std::max(maxFmtItemW, calcText(sansFont, geist::type::LABEL, fmtLabels[i]).x);
    }
    const float fmtCellW = (fmtSegR.w - ctl::geom::SEG_PAD * 2.0f) / 5.0f;
    const float fmtFontPx = (maxFmtItemW + 24.0f > fmtCellW) ? geist::type::BODY_SMALL : geist::type::LABEL;
    const bool fmtChanged = snap.overwrite_original
        ? ctl::segmentedLocked(dl, ImGui::GetID("##fmt"), fmtSegR.p0(), fmtSegR.size(),
                               fmtLabels, 5, fmtIdx, sansFont, fmtFontPx, true)
        : ctl::segmented(dl, ImGui::GetID("##fmt"), fmtSegR.p0(), fmtSegR.size(),
                         fmtLabels, 5, fmtIdx, sansFont, fmtFontPx, false, true);
    if (fmtChanged && !snap.overwrite_original) {
        app.setFormat(fmtVals[fmtIdx]);
    }

    if (fx.isVisible(qGroup.handle())) {
        drawTextCenteredY(dl, sansFont, geist::type::BODY, qLblR.x, qLblR.y, qLblR.bottom(), t.body,
                          tr(Key::InspectorQuality));
        int q = snap.quality;
        if (ctl::qualityInput(dl, ImGui::GetID("##q_input"), qInR.p0(), q, 1, 100, monoFont)) {
            app.setQuality(q);
        }
        ensureVisibleOnFocus(qInR);
        if (ctl::qualitySlider(dl, ImGui::GetID("##q_slider"), qSliderR.p0(), qSliderR.w, q, 1, 100)) {
            app.setQuality(q);
        }
        ensureVisibleOnFocus(qSliderR);
    }

    if (fx.isVisible(chromaRow.handle())) {
        drawTextCenteredY(dl, sansFont, geist::type::BODY, chromaLblR.x, chromaLblR.y, chromaLblR.bottom(),
                          t.body, tr(Key::SettingsChroma));
        const char* chromaLabels[2] = {"4:4:4", "4:2:0"};
        const int chromaVals[2] = {444, 420};
        int chromaIdx = (snap.chroma == 444) ? 0 : 1;
        if (ctl::segmented(dl, ImGui::GetID("##chroma"), chromaSegR.p0(), chromaSegR.size(),
                           chromaLabels, 2, chromaIdx, monoFont, geist::type::BODY_SMALL, false, true)) {
            app.setChroma(chromaVals[chromaIdx]);
        }
    }

    if (fx.isVisible(speedRow.handle())) {
        drawTextCenteredY(dl, sansFont, geist::type::BODY, speedLblR.x, speedLblR.y, speedLblR.bottom(),
                          t.body, tr(Key::SettingsEncodeSpeed));
        const char* spdLabels[3] = {
            tr(Key::SettingsSpeedFine),
            tr(Key::SettingsSpeedBalanced),
            tr(Key::SettingsSpeedFast),
        };
        const int spdVals[3] = {4, 6, 8};
        int spdIdx = 1;
        for (int i = 0; i < 3; ++i) {
            if (snap.avif_speed == spdVals[i]) {
                spdIdx = i;
                break;
            }
        }
        if (ctl::segmented(dl, ImGui::GetID("##spd"), speedSegR.p0(), speedSegR.size(),
                           spdLabels, 3, spdIdx, sansFont, geist::type::BODY_SMALL, false, true)) {
            app.setAvifSpeed(spdVals[spdIdx]);
        }
    }

    // ── Section 3: 导出设置 (always expanded) ──
    ctl::hairlineRule(dl, accRuleR.x, accRuleR.right(), accRuleR.y, t.outline);
    ctl::eyebrowHeader(dl, mediumFont, hdr3R.p0(), tr(Key::InspectorSecExport));

    {
        const bool resetVis = fx.isVisible(dirResetSlot);
        const float hitRight = resetVis ? (dirResetR.x - 4.0f) : (dirPickR.x - 4.0f);
        const ImVec2 hitP0 = snapPt(dirBoxR.p0());
        const ImVec2 hitP1 = snapPt(std::max(hitP0.x + 8.0f, hitRight), dirBoxR.bottom());

        bool dirPathClicked = false;
        bool dirPathHov = false;
        if (!dirLocked) {
            ImGui::SetCursorScreenPos(hitP0);
            dirPathClicked = ImGui::InvisibleButton("##dir_box_hit", ImVec2(hitP1.x - hitP0.x, hitP1.y - hitP0.y));
            dirPathHov = ImGui::IsItemHovered();
            if (dirPathHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const bool dirAnyHov = !dirLocked && ctl::mouseIn(dirBoxR.p0(), dirBoxR.p1());
        const float dirPathHovT = motion::hoverFactor(ImGui::GetID("##dir_path_hov"), dirPathHov);
        const float dirBorderHovT = motion::hoverFactor(ImGui::GetID("##dir_border_hov"), dirAnyHov);

        const ImU32 dirFill = motion::mix(t.surface, dark ? t.high : t.container, dirPathHovT);
        const ImU32 dirBorder = motion::mix(t.outline, t.outlineStrong, dirBorderHovT);
        dl->AddRectFilled(dirBoxR.p0(), dirBoxR.p1(), dirFill, geist::radius::MD);
        dl->AddRect(dirBoxR.p0(), dirBoxR.p1(), dirBorder, geist::radius::MD, 0, hairlinePx());
        drawIcon(dl, "folder", dirIconR.center(), 15.0f,
                 dirLocked ? t.faint : motion::mix(t.mute, t.ink, dirPathHovT));
        drawTextFit(dl, outFont, outFontPx, dirPathR,
                    dirLocked ? t.faint : motion::mix(t.body, t.ink, dirPathHovT), outLabel, outSz.x);
        if (!dirLocked && (hasCustom || outSz.x > dirPathR.w)) {
            ctl::tooltip(dl, ImGui::GetID("##dir_path_tip"), hitP0, hitP1, outLabel,
                         sansFont, dirPathHov, false);
        }

        if (resetVis) {
            if (ctl::secondaryButton(dl, ImGui::GetID("##reset_outdir"), tr(Key::CommonReset), sansFont,
                                     dirResetR.p0(), dirResetR.p1())) {
                app.setCustomOutDir("");
            }
            ensureVisibleOnFocus(dirResetR);
        }
        if (ctl::secondaryButton(dl, ImGui::GetID("##pick_outdir"), dirBtnLabel, sansFont,
                                 dirPickR.p0(), dirPickR.p1(), nullptr, !dirLocked) || dirPathClicked) {
            SDL_ShowOpenFolderDialog(onFileDialog, dlgOutDir, window, nullptr, false);
        }
        ensureVisibleOnFocus(dirPickR);
    }

    {
        bool ovLblClicked = false;
        bool ovLblHov = false;
        if (!isRunning) {
            ImGui::SetCursorScreenPos(snapPt(ovTitleR.p0()));
            ovLblClicked = ImGui::InvisibleButton("##ov_lbl_hit", ovTitleR.size());
            ovLblHov = ImGui::IsItemHovered();
            if (ovLblHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const float ovHovT = motion::hoverFactor(ImGui::GetID("##ov_lbl_hov"),
                                                 !isRunning && (ovLblHov || ctl::mouseIn(ovSwR.p0(), ovSwR.p1())));
        if (ovHovT > 0.01f) {
            const ImVec2 r0 = snapPt(ovTitleR.x - 6.0f, std::min(ovTitleR.y, ovSwR.y) - 4.0f);
            const ImVec2 r1 = snapPt(ovSwR.right() + 6.0f, std::max(ovTitleR.bottom(), ovSwR.bottom()) + 4.0f);
            ctl::hoverWashAlpha(dl, r0, r1, geist::radius::MD, ovHovT, 0.045f);
        }
        drawTextFit(dl, sansFont, geist::type::BODY, ovTitleR,
                    !isRunning ? t.ink : t.mute, ovTitle, ovTSz.x);
        bool nextOv = snap.overwrite_original;
        const bool toggled = ctl::toggleSwitch(dl, ImGui::GetID("##sw_overwrite"), ovSwR.p0(), nextOv, !isRunning);
        ensureVisibleOnFocus(ovSwR);
        if (ovLblClicked) {
            nextOv = !snap.overwrite_original;
        }
        if ((toggled || ovLblClicked) && nextOv != snap.overwrite_original) {
            app.setOverwriteOriginal(nextOv);
            if (nextOv) app.setBackupOriginal(true);
        }
    }

    if (fx.isVisible(bkSlots.swSlot)) {
        bool bkLblClicked = false;
        bool bkLblHov = false;
        if (!isRunning) {
            ImGui::SetCursorScreenPos(snapPt(bkTitleR.p0()));
            bkLblClicked = ImGui::InvisibleButton("##bk_lbl_hit", bkTitleR.size());
            bkLblHov = ImGui::IsItemHovered();
            if (bkLblHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        const float bkHovT = motion::hoverFactor(ImGui::GetID("##bk_lbl_hov"),
                                                 !isRunning && (bkLblHov || ctl::mouseIn(bkSwR.p0(), bkSwR.p1())));
        if (bkHovT > 0.01f) {
            const ImVec2 r0 = snapPt(bkTitleR.x - 6.0f, std::min(bkTitleR.y, bkSwR.y) - 4.0f);
            const ImVec2 r1 = snapPt(bkSwR.right() + 6.0f, std::max(bkTitleR.bottom(), bkSwR.bottom()) + 4.0f);
            ctl::hoverWashAlpha(dl, r0, r1, geist::radius::MD, bkHovT, 0.045f);
        }
        drawTextFit(dl, sansFont, geist::type::BODY, bkTitleR,
                    !isRunning ? t.ink : t.mute, bkTitle, bkTSz.x);
        bool nextBk = snap.backup_original;
        const bool toggled = ctl::toggleSwitch(dl, ImGui::GetID("##sw_backup"), bkSwR.p0(), nextBk, !isRunning);
        ensureVisibleOnFocus(bkSwR);
        if (bkLblClicked) {
            nextBk = !snap.backup_original;
        }
        if ((toggled || bkLblClicked) && nextBk != snap.backup_original) {
            app.setBackupOriginal(nextBk);
        }
    }

    ctl::scrollThumb(dl, scrollViewport.p0(), scrollViewport.p1(), state.scroll, contentH);
    dl->PopClipRect();

    if (mouseOutsideScroll && !modal) {
        io.MousePos = savedMousePos;
    }

    // 4. Render Sticky CTA Dock using rects already solved by paneFx.
    ctl::hairlineRule(dl, inspRect.x, inspRect.right(), inspSeamY, t.outline);

    const flex::Rect priR = paneFx.rect(ctaPrimarySlot);
    const flex::Rect secR = paneFx.rect(ctaSecondarySlot);
    const flex::Rect icoR = paneFx.rect(ctaIconSlot);

    switch (ctaMode) {
    case CtaMode::CancelRunning: {
        const bool canCancel = (snap.job.phase == upix::gui::JobPhase::Running);
        if (ctl::secondaryButton(dl, ImGui::GetID("##cancel_job"),
                                 canCancel ? tr(Key::InspectorCtaCancel) : tr(Key::WbPhaseCancelling), mediumFont,
                                 priR.p0(), priR.p1(), nullptr, canCancel, true)) {
            app.cancelJob();
        }
        break;
    }
    case CtaMode::DownloadMissingModel: {
        if (ctl::primaryButton(dl, ImGui::GetID("##cta_dl_model"), tr(Key::ModelsDownload), mediumFont,
                               priR.p0(), priR.p1(), true, "download")) {
            app.downloadModel(snap.job.missing_model);
            ui.activeTab = 1;
        }
        break;
    }
    case CtaMode::DisabledPrereq: {
        const char* emptyLabel = snap.inputs.empty()
            ? tr(Key::InspectorCtaAddImageFirst)
            : tr(Key::InspectorCtaSelectModelFirst);
        ctl::primaryButton(dl, ImGui::GetID("##cta_disabled"), emptyLabel, mediumFont,
                           priR.p0(), priR.p1(), false);
        break;
    }
    case CtaMode::CompareChanged: {
        if (ctl::primaryButton(dl, ImGui::GetID("##cta_rerun"), tr(Key::InspectorCtaRerun), mediumFont,
                               priR.p0(), priR.p1(), true)) {
            state.doneParamSig.clear();
            state.prevDoneOutputs.clear();
            app.startJob();
        }
        if (hasOut && ctl::secondaryButton(dl, ImGui::GetID("##cta_open_img"), tr(Key::CommonOpenImage), sansFont,
                                           secR.p0(), secR.p1(), nullptr, true)) {
            openFileInSystem(outPath);
        }
        break;
    }
    case CtaMode::CompareReady: {
        if (ctl::primaryButton(dl, ImGui::GetID("##cta_open_img"), cmpOpenLabel, mediumFont,
                               priR.p0(), priR.p1(), hasOut)) {
            openFileInSystem(outPath);
        }
        if (ctl::secondaryButton(dl, ImGui::GetID("##cta_reveal_file"), tr(Key::CommonRevealFile), sansFont,
                                 secR.p0(), secR.p1(), nullptr, hasOut)) {
            revealFileParentInSystem(outPath);
        }
        if (ctl::iconAction(dl, ImGui::GetID("##cta_rerun_icon"), icoR.p0(), icoR.w,
                            "refresh", tr(Key::InspectorCtaRerun), sansFont, 17.0f, geist::radius::MD, true, true)) {
            state.doneParamSig.clear();
            state.prevDoneOutputs.clear();
            app.startJob();
        }
        break;
    }
    case CtaMode::StartOrResume: {
        size_t doneCnt = 0;
        for (const auto& it : snap.inputs) {
            if (it.status_kind == 2) ++doneCnt;
        }
        const bool resuming = (doneCnt > 0) ||
                              (snap.job.phase == upix::gui::JobPhase::Cancelled) ||
                              isError;
        const char* label = resuming ? tr(Key::InspectorCtaResume) : tr(Key::InspectorCtaStart);
        if (ctl::primaryButton(dl, ImGui::GetID("##cta_start"), label, mediumFont,
                               priR.p0(), priR.p1(), true)) {
            app.startJob();
        }
        if (hasActiveDone && ctl::secondaryButton(dl, ImGui::GetID("##cta_open_done"), tr(Key::CommonOpenImage), sansFont,
                                                  secR.p0(), secR.p1(), nullptr, true)) {
            openFileInSystem(outPath);
        }
        break;
    }
    }
}

} // namespace wb
} // namespace upix::views::detail
