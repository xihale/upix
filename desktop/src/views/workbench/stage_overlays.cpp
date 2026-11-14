// views/workbench/stage_overlays.cpp — Stage empty dropzone, zoom capsule, telemetry & error overlays.
#include "views/workbench/internal.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace upix::views::detail {
inline namespace wb {

namespace {

void drawFloatShadowNoBorder(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius, ImU32 fill) {
    using namespace upix::ui;
    const bool dark = geist::mode() == geist::Mode::Dark;
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if (dark) {
        ctl::detail::drawOuter(dl, p0, p1, radius, ctl::shadowBlack(4.0f, 12.0f, 0.0f, 0.50f));
        ctl::detail::drawOuter(dl, p0, p1, radius, ctl::shadowBlack(12.0f, 32.0f, -4.0f, 0.80f));
    } else {
        ctl::detail::drawOuter(dl, p0, p1, radius, ctl::shadowBlack(2.0f, 2.0f, 0.0f, 0.02f));
        ctl::detail::drawOuter(dl, p0, p1, radius, ctl::shadowBlack(8.0f, 16.0f, -4.0f, 0.12f));
    }
    dl->AddRectFilled(p0, p1, fill, radius);
}

} // namespace

void drawDashedRoundedRect(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius,
                           ImU32 col, float thickness, float dashLen, float gapLen) {
    if (!dl || (col >> IM_COL32_A_SHIFT) == 0) return;
    p0 = upix::ui::snapPt(p0);
    p1 = upix::ui::snapPt(p1);
    const float w = p1.x - p0.x;
    const float h = p1.y - p0.y;
    if (w <= 2.0f || h <= 2.0f) return;

    const float r = std::clamp(radius, 0.0f, std::min(w, h) * 0.5f);
    constexpr float kPi = 3.14159265358979323846f;
    constexpr int kArcSteps = 6;

    static std::vector<ImVec2> pts;
    static std::vector<float> cum;
    pts.clear();
    pts.reserve(4 * (kArcSteps + 1) + 1);
    auto addArc = [&](float cx, float cy, float a0, float a1) {
        for (int i = 0; i <= kArcSteps; ++i) {
            const float a = a0 + (a1 - a0) * (static_cast<float>(i) / static_cast<float>(kArcSteps));
            pts.emplace_back(cx + std::cos(a) * r, cy + std::sin(a) * r);
        }
    };
    if (r > 0.5f) {
        addArc(p1.x - r, p0.y + r, -kPi * 0.5f, 0.0f);
        addArc(p1.x - r, p1.y - r, 0.0f, kPi * 0.5f);
        addArc(p0.x + r, p1.y - r, kPi * 0.5f, kPi);
        addArc(p0.x + r, p0.y + r, kPi, kPi * 1.5f);
    } else {
        pts.push_back(ImVec2(p1.x, p0.y));
        pts.push_back(ImVec2(p1.x, p1.y));
        pts.push_back(ImVec2(p0.x, p1.y));
        pts.push_back(ImVec2(p0.x, p0.y));
    }
    pts.push_back(pts.front());

    cum.assign(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
        const float dx = pts[i].x - pts[i - 1].x;
        const float dy = pts[i].y - pts[i - 1].y;
        cum[i] = cum[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    const float totalLen = cum.back();
    if (totalLen <= 1.0f) return;

    auto sampleAt = [&](float dist, size_t& hintIdx) -> ImVec2 {
        while (hintIdx + 1 < cum.size() && cum[hintIdx + 1] < dist) {
            ++hintIdx;
        }
        const float s0 = cum[hintIdx];
        const float s1 = cum[std::min(hintIdx + 1, cum.size() - 1)];
        const float seg = s1 - s0;
        if (seg <= 1.0e-4f) return pts[hintIdx];
        const float u = std::clamp((dist - s0) / seg, 0.0f, 1.0f);
        const ImVec2& a = pts[hintIdx];
        const ImVec2& b = pts[hintIdx + 1];
        return ImVec2(a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u);
    };

    const float step = std::max(2.0f, dashLen + gapLen);
    size_t idx = 0;
    for (float s = 0.0f; s < totalLen; s += step) {
        const float e = std::min(totalLen, s + dashLen);
        if (e - s < 0.5f) break;
        dl->PathClear();
        size_t curIdx = idx;
        dl->PathLineTo(sampleAt(s, curIdx));
        while (curIdx + 1 < cum.size() && cum[curIdx + 1] < e) {
            ++curIdx;
            dl->PathLineTo(pts[curIdx]);
        }
        size_t endIdx = curIdx;
        dl->PathLineTo(sampleAt(e, endIdx));
        dl->PathStroke(col, 0, thickness);
        idx = curIdx;
    }
}

const char* inferInputFormatLabel(const upix::gui::InputItem* active) {
    if (!active || active->is_dir) {
        return "";
    }
    auto extractExtLabel = [](const std::string& src) -> const char* {
        if (src.empty() || src.rfind("__fixture", 0) == 0) return "";
        const size_t slash = src.find_last_of("/\\");
        const std::string_view base = (slash == std::string::npos)
            ? std::string_view(src)
            : std::string_view(src).substr(slash + 1);
        const size_t dot = base.rfind('.');
        if (dot == std::string_view::npos || dot + 1 >= base.size()) return "";
        const std::string_view ext = base.substr(dot + 1);
        auto eqi = [](std::string_view a, std::string_view b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (static_cast<char>(std::tolower(static_cast<unsigned char>(a[i]))) != b[i]) return false;
            }
            return true;
        };
        if (eqi(ext, "png")) return "PNG";
        if (eqi(ext, "jpg") || eqi(ext, "jpeg")) return "JPEG";
        if (eqi(ext, "webp")) return "WebP";
        if (eqi(ext, "avif")) return "AVIF";
        return "";
    };
    if (const char* fromPath = extractExtLabel(active->path); fromPath[0] != '\0') {
        return fromPath;
    }
    return extractExtLabel(active->name);
}

int modelScaleFor(const upix::gui::Snapshot& snap) {
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) return std::max(1, m.scale);
    }
    for (const auto& m : snap.remote) {
        if (m.name == snap.model) return std::max(1, m.scale);
    }
    return 4;
}

void renderEmptyDropzone(ImDrawList* dl, const upix::ui::flex::Rect& stageRect,
                         const UiViewState& ui, upix::gui::AppState& app,
                         const upix::gui::Snapshot& snap, SDL_Window* window,
                         DialogBridge* dlgFiles, DialogBridge* dlgFolder, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;

    const float cardW = std::min(460.0f, std::max(240.0f, stageRect.w - geist::space::XXXL * 1.5f));
    const float cardH = std::min(248.0f, std::max(176.0f, stageRect.h - geist::space::XXXL - 36.0f));

    using upix::i18n::Key;
    using upix::i18n::tr;
    const char* dropTitle = tr(Key::WbDropTitle);
    const char* filesLabel = tr(Key::WbSelectImages);
    const char* folderLabel = tr(Key::WbSelectFolder);
    const char* recLabel = tr(Key::WbIncludeSubfolders);

    const ImVec2 titleSz = calcText(mediumFont, geist::type::SUBHEADING, dropTitle);
    const ImVec2 recSz = calcText(sansFont, geist::type::BODY_SMALL, recLabel);
    const ImVec2 filesSz = calcText(mediumFont, geist::type::BODY, filesLabel);
    const ImVec2 folderSz = calcText(sansFont, geist::type::BODY, folderLabel);
    const float btnH = std::max(ctl::geom::BTN_H, std::ceil(filesSz.y) + 14.0f);
    const float innerW = std::max(140.0f, cardW - geist::space::XXXL);
    const float filesBtnW = std::min(innerW, std::max(108.0f, std::ceil(filesSz.x) + 32.0f));
    const float folderBtnW = std::min(innerW, std::max(120.0f, std::ceil(folderSz.x) + 32.0f));

    static flex::Context dzCtx;
    flex::Layout fx(dzCtx);
    auto stageBox = fx.col({
        .gap = 12.0f,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    auto card = stageBox.col({
        .padding = flex::Edges::xy(geist::space::XL, geist::space::LG),
        .gap = 14.0f,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .width = flex::Fixed(cardW),
        .height = flex::Fixed(cardH),
    });
    const flex::Slot iconSlot = card.slot(flex::Fixed(44.0f), flex::Fixed(44.0f));
    const flex::Slot titleSlot = card.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(titleSz.y),
        .alignSelf = flex::AlignSelf::Stretch,
    });
    auto btnRow = card.row({
        .margin = flex::Edges::only(0.0f, 2.0f, 0.0f, 0.0f),
        .gap = geist::space::SM,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Stretch,
        .wrap = flex::Wrap::Wrap,
    });
    const flex::Slot btnFilesSlot = btnRow.slot(flex::Fixed(filesBtnW), flex::Fixed(btnH));
    const flex::Slot btnFolderSlot = btnRow.slot(flex::Fixed(folderBtnW), flex::Fixed(btnH));
    auto recRow = stageBox.row({
        .gap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    const flex::Slot recLblSlot = recRow.slot(flex::Shrink(textBasis(recSz.x), 1.0f).min(40.0f), flex::Fixed(recSz.y));
    const flex::Slot recSwSlot = recRow.slot(flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H));

    fx.solve(stageRect);

    const flex::Rect cardR = fx.rect(card);
    const flex::Rect iconR = fx.rect(iconSlot);
    const flex::Rect titleR = fx.rect(titleSlot);
    const flex::Rect btnFilesR = fx.rect(btnFilesSlot);
    const flex::Rect btnFolderR = fx.rect(btnFolderSlot);
    const flex::Rect recLblR = fx.rect(recLblSlot);
    const flex::Rect recSwR = fx.rect(recSwSlot);

    dl->PushClipRect(stageRect.p0(), stageRect.p1(), true);

    const bool dark = (geist::mode() == geist::Mode::Dark);
    const bool overChildBtn = ctl::mouseIn(btnFilesR.p0(), btnFilesR.p1()) ||
                              ctl::mouseIn(btnFolderR.p0(), btnFolderR.p1());

    bool cardBgClicked = false;
    bool cardBgHov = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(snapPt(cardR.p0()));
        ImGui::SetNextItemAllowOverlap();
        cardBgClicked = ImGui::InvisibleButton("##empty_dz_card", cardR.size());
        cardBgHov = ctl::mouseIn(cardR.p0(), cardR.p1()) && !overChildBtn;
        if (ImGui::IsItemHovered() && !overChildBtn) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const float hovT = motion::hoverFactor(ImGui::GetID("##empty_dz_hov"), cardBgHov);

    const ImVec2 c0 = snapPt(cardR.p0());
    const ImVec2 c1 = snapPt(cardR.p1());
    dl->AddRectFilled(c0, c1, motion::mix(t.dropzoneBg, t.container, hovT), geist::radius::LG);
    const ImU32 dzBorder = motion::mix(t.dropzoneBorder, t.outlineStrong, hovT);
    drawDashedRoundedRect(dl, c0, c1, geist::radius::LG, dzBorder, hairlinePx(), 6.0f, 4.0f);

    // 44x44 rounded r:6 icon badge
    const ImVec2 ib0 = snapPt(iconR.p0());
    const ImVec2 ib1 = snapPt(iconR.p1());
    dl->AddRectFilled(ib0, ib1, motion::mix(t.surface, dark ? t.high : t.surface, hovT), geist::radius::MD);
    dl->AddRect(ib0, ib1, motion::mix(t.outline, t.outlineStrong, hovT), geist::radius::MD, 0, hairlinePx());
    drawIcon(dl, "upload", iconR.center(), 21.0f, motion::mix(t.body, t.ink, hovT), 1.6f);

    drawTextFitCentered(dl, mediumFont, geist::type::SUBHEADING, titleR, t.ink, dropTitle, titleSz.x);

    if (ctl::primaryButton(dl, ImGui::GetID("##empty_pick_files"), filesLabel, mediumFont,
                           btnFilesR.p0(), btnFilesR.p1(), true)) {
        SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
    }
    if (ctl::secondaryButton(dl, ImGui::GetID("##empty_pick_folder"), folderLabel, sansFont,
                             btnFolderR.p0(), btnFolderR.p1())) {
        if (dlgFolder) dlgFolder->recursive = snap.recursive;
        SDL_ShowOpenFolderDialog(onFileDialog, dlgFolder, window, nullptr, false);
    }

    bool recLblClicked = false;
    bool recLblHov = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(snapPt(recLblR.p0()));
        recLblClicked = ImGui::InvisibleButton("##empty_rec_lbl", recLblR.size());
        recLblHov = ImGui::IsItemHovered();
        if (recLblHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    const float recLblHovT = motion::hoverFactor(ImGui::GetID("##empty_rec_lbl_hov"), recLblHov);
    drawTextFit(dl, sansFont, geist::type::BODY_SMALL, recLblR,
                motion::mix(t.mute, t.ink, recLblHovT), recLabel, recSz.x);

    bool rec = snap.recursive;
    if (ctl::toggleSwitch(dl, ImGui::GetID("##empty_recursive"), recSwR.p0(), rec)) {
        app.setRecursive(rec);
    } else if (recLblClicked) {
        app.setRecursive(!snap.recursive);
    }

    if (cardBgClicked) {
        SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
    }

    dl->PopClipRect();
}

void renderFloatingZoomCapsule(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                               UiViewState& ui, float fitScale, float effScale,
                               bool& stagePixelated, upix::ui::flex::Rect& outCapsuleR) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool dark = geist::mode() == geist::Mode::Dark;
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    struct ZoomBtnSpec {
        const char* label;
        bool isMono;
        bool active;
        float targetScale; // > 0 for fixed scale, <= 0 for fit / pixel
        int kind;          // 0=fit, 1=scale, 2=pixel
    };

    const bool is1x = !ui.fitMode && std::fabs(effScale - 1.0f) < 0.02f;
    const bool is2x = !ui.fitMode && std::fabs(effScale - 2.0f) < 0.02f;
    const bool is4x = !ui.fitMode && std::fabs(effScale - 4.0f) < 0.02f;

    const ZoomBtnSpec btns[5] = {
        {upix::i18n::tr(upix::i18n::Key::WbZoomFit), false, ui.fitMode, 0.0f, 0},
        {"1:1", true, is1x, 1.0f, 1},
        {"2:1", true, is2x, 2.0f, 1},
        {"4:1", true, is4x, 4.0f, 1},
        {upix::i18n::tr(upix::i18n::Key::WbZoomPixel), false, stagePixelated, 1.0f, 2},
    };

    char pctBuf[24] = {};
    const float pct = effScale * 100.0f;
    const char* pctStr = (pct >= 99.95f)
        ? formatInto(pctBuf, "{:.0f}%", pct)
        : formatInto(pctBuf, "{:.1f}%", pct);

    constexpr float kCapPadX = 3.0f;
    constexpr float kCapGap = 2.0f;
    float btnWidths[5] = {};
    for (int i = 0; i < 5; ++i) {
        ImFont* bf = btns[i].isMono ? monoFont : sansFont;
        btnWidths[i] = std::ceil(calcText(bf, geist::type::BODY_SMALL, btns[i].label).x) + 16.0f;
    }
    const float pctMaxW = std::ceil(calcText(monoFont, geist::type::MONO, "400%").x) + 14.0f;
    const float pctW = std::max(pctMaxW, std::ceil(calcText(monoFont, geist::type::MONO, pctStr).x) + 14.0f);

    static flex::Context zoomCtx;
    flex::Layout fx(zoomCtx);
    auto host = fx.col({
        .padding = flex::Edges::xy(kZoomCapsuleMargin, kZoomCapsuleMargin),
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
    });
    auto capsule = host.row({
        .padding = flex::Edges::xy(kCapPadX, kCapPadX),
        .gap = kCapGap,
        .align = flex::Align::Center,
        .width = flex::Auto().min(180.0f),
        .height = flex::Fixed(kZoomCapsuleH),
    });
    flex::Slot bSlots[5];
    for (int i = 0; i < 5; ++i) {
        bSlots[i] = capsule.slot(flex::Shrink(btnWidths[i], 1.0f).min(28.0f),
                                 flex::Fixed(kZoomCapsuleH - kCapPadX * 2.0f));
    }
    const flex::Slot sepSlot = capsule.slot({
        .width = flex::Fixed(hairlinePx()),
        .height = flex::Fixed(16.0f),
        .margin = flex::Edges::xy(2.0f, 0.0f),
    });
    const flex::Slot pctSlot = capsule.slot(flex::Shrink(pctW, 0.8f).min(42.0f),
                                            flex::Fixed(kZoomCapsuleH - kCapPadX * 2.0f));

    fx.solve(viewRect);
    outCapsuleR = fx.rect(capsule);
    const flex::Rect sepR = fx.rect(sepSlot);
    const flex::Rect pctR = fx.rect(pctSlot);

    const float capR = kZoomCapsuleH * 0.5f;
    ctl::finish(dl, outCapsuleR.p0(), outCapsuleR.p1(), ctl::Finish::Capsule, capR);
    dl->AddRect(outCapsuleR.p0(), outCapsuleR.p1(), t.outline, capR, 0, hairlinePx());

    for (int i = 0; i < 5; ++i) {
        const flex::Rect r = fx.rect(bSlots[i]);
        if (r.w < 16.0f) continue;
        const ImVec2 p0 = snapPt(r.p0());
        const ImVec2 p1 = snapPt(r.p1());
        const float pillR = (kZoomCapsuleH - kCapPadX * 2.0f) * 0.5f;
        ImGui::PushID(i);
        ImGui::SetCursorScreenPos(p0);
        const bool clicked = ImGui::InvisibleButton("##zbtn", r.size());
        const bool hov = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const bool focused = ImGui::GetIO().NavVisible && ImGui::IsItemFocused();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        const ImGuiID bid = ImGui::GetID("##zbtn_id");
        const float hovT = motion::hoverFactor(ctl::detail::salt(bid, 1), hov);
        const float sc = motion::pressScale(ctl::detail::salt(bid, 2), held, motion::PRESS_SCALE);
        ImVec2 q0 = p0, q1 = p1;
        ctl::detail::scaled(p0, p1, sc, q0, q1);

        if (btns[i].active) {
            ctl::finish(dl, q0, q1, ctl::Finish::Whisper, pillR);
            const ImU32 activeBg = dark
                ? motion::mix(t.segThumb, t.highest, hovT)
                : motion::mix(t.container, t.high, hovT);
            dl->AddRectFilled(q0, q1, activeBg, pillR);
            dl->AddRect(q0, q1, dark ? t.segThumbRim : t.outline, pillR, 0, hairlinePx());
        } else if (held || hovT > 0.01f) {
            ctl::hoverWashAlpha(dl, q0, q1, pillR, held ? 1.0f : hovT, held ? 0.14f : 0.09f);
        }
        const ImU32 fg = btns[i].active ? t.ink : motion::mix(t.mute, t.ink, held ? 1.0f : hovT);
        ImFont* bf = btns[i].isMono ? monoFont : (btns[i].active ? mediumFont : sansFont);
        drawTextCentered(dl, bf, geist::type::BODY_SMALL, q0, q1, fg, btns[i].label);
        ctl::detail::focusRing(dl, p0, p1, pillR, focused);

        if (clicked) {
            if (btns[i].kind == 0) {
                ui.fitMode = true;
                ui.zoom = 1.0f;
                ui.pan = ImVec2(0.0f, 0.0f);
            } else if (btns[i].kind == 1) {
                ui.fitMode = false;
                ui.zoom = std::clamp(btns[i].targetScale / std::max(0.01f, fitScale), 0.1f, 32.0f);
                ui.pan = ImVec2(0.0f, 0.0f);
            } else {
                stagePixelated = !stagePixelated;
                if (stagePixelated && ui.fitMode) {
                    ui.fitMode = false;
                    ui.zoom = std::clamp(1.0f / std::max(0.01f, fitScale), 0.1f, 32.0f);
                    ui.pan = ImVec2(0.0f, 0.0f);
                }
            }
        }
        ImGui::PopID();
    }

    ctl::vHairline(dl, sepR.x, sepR.y, sepR.bottom(), t.outline);
    drawTextCentered(dl, monoFont, geist::type::MONO, pctR.p0(), pctR.p1(), t.mute, pctStr);
}

void renderRunningTelemetryPill(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                                const UiViewState& ui, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    char queueBuf[24] = {};
    const bool showQueue = snap.job.total > 1;
    const char* queueStr = showQueue
        ? formatInto(queueBuf, "{} / {}", std::max(1, snap.job.index), snap.job.total)
        : nullptr;

    using upix::i18n::Key;
    using upix::i18n::tr;
    char phaseBuf[64] = {};
    const char* phaseStr = nullptr;
    if (snap.job.phase == upix::gui::JobPhase::Cancelling) {
        phaseStr = tr(Key::WbPhaseCancelling);
    } else if (snap.job.tiles_total > 0) {
        phaseStr = formatKeyInto(phaseBuf, Key::WbPhaseInferringFmt, snap.job.tiles_done, snap.job.tiles_total);
    } else if (!snap.job.current_file.empty()) {
        phaseStr = snap.job.current_file.c_str();
    } else {
        phaseStr = tr(Key::WbPhasePreparing);
    }

    char pctBuf[16] = {};
    const char* pctStr = formatInto(pctBuf, "{}%", snap.job.percent);

    char statsBuf[160] = {};
    const char* backendStr = snap.gpu ? "Vulkan" : "CPU";
    if (snap.job.tiles_total > 0 && snap.job.eta_sec > 0) {
        formatKeyInto(statsBuf, Key::WbStatsTileEtaFmt, snap.model,
                      snap.job.tiles_done, snap.job.tiles_total, snap.job.eta_sec, backendStr);
    } else if (snap.job.tiles_total > 0) {
        formatKeyInto(statsBuf, Key::WbStatsTileFmt, snap.model,
                      snap.job.tiles_done, snap.job.tiles_total, backendStr);
    } else {
        formatInto(statsBuf, "{} · {}", snap.model, backendStr);
    }

    const ImVec2 queueSz = queueStr ? calcText(monoFont, geist::type::MONO, queueStr) : ImVec2{};
    const ImVec2 phaseSz = calcText(mediumFont, geist::type::BODY, phaseStr);
    const ImVec2 pctSz = calcText(monoFont, geist::type::MONO, pctStr);
    const ImVec2 statsSz = calcText(monoFont, geist::type::MONO, statsBuf);
    const float pillW = std::min(520.0f, std::max(260.0f, viewRect.w - geist::space::XXXL));

    static flex::Context teleCtx;
    flex::Layout fx(teleCtx);
    auto host = fx.col({
        .padding = flex::Edges::xy(geist::space::LG, 16.0f),
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
    });
    auto pill = host.col({
        .padding = flex::Edges::xy(geist::space::LG, 12.0f),
        .gap = geist::space::SM,
        .width = flex::Fixed(pillW),
        .height = flex::Auto(),
    });
    auto topRow = pill.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
    });
    const flex::Slot queueSlot = topRow.slot({
        .width = flex::Fixed(textBasis(queueSz.x)),
        .height = flex::Fixed(queueSz.y),
        .visible = showQueue,
    });
    const flex::Slot phaseSlot = topRow.slot(flex::Shrink(textBasis(phaseSz.x), 1.0f).min(48.0f),
                                             flex::Fixed(phaseSz.y));
    topRow.spacer(1.0f);
    const flex::Slot pctSlot = topRow.slot(flex::Fixed(std::ceil(pctSz.x)), flex::Fixed(pctSz.y));
    const flex::Slot progSlot = pill.slot(flex::Auto(), flex::Fixed(3.0f));
    const flex::Slot statsSlot = pill.slot(flex::Auto(), flex::Fixed(statsSz.y));

    fx.solve(viewRect);
    const flex::Rect pillR = fx.rect(pill);
    const flex::Rect queueR = fx.rect(queueSlot);
    const bool queueVis = fx.isVisible(queueSlot);
    const flex::Rect phaseR = fx.rect(phaseSlot);
    const flex::Rect pctR = fx.rect(pctSlot);
    const flex::Rect progR = fx.rect(progSlot);
    const flex::Rect statsR = fx.rect(statsSlot);

    drawFloatShadowNoBorder(dl, pillR.p0(), pillR.p1(), geist::radius::LG, t.surface);

    if (queueVis && queueStr) {
        drawTextCenteredY(dl, monoFont, geist::type::MONO, queueR.x, queueR.y, queueR.bottom(), t.mute, queueStr);
    }
    drawTextFit(dl, mediumFont, geist::type::BODY, phaseR, t.ink, phaseStr, phaseSz.x);
    drawTextCenteredY(dl, monoFont, geist::type::MONO, pctR.x, pctR.y, pctR.bottom(), t.ink, pctStr);

    const float frac = std::clamp(snap.job.percent / 100.0f, 0.0f, 1.0f);
    ctl::progressBar(dl, ImGui::GetID("##tele_progress"), progR.p0(), progR.w, frac, 3.0f);
    drawTextFit(dl, monoFont, geist::type::MONO, statsR, t.mute, statsBuf, statsSz.x);
}

void renderErrorVeilCard(ImDrawList* dl, const upix::ui::flex::Rect& viewRect,
                         UiViewState& ui, upix::gui::AppState& app,
                         const upix::gui::Snapshot& snap, const upix::gui::InputItem* activeItem) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImFont* sansFont = ui.fontSans ? ui.fontSans : ImGui::GetFont();
    ImFont* mediumFont = ui.fontSansMedium ? ui.fontSansMedium : sansFont;
    ImFont* monoFont = ui.fontMono ? ui.fontMono : sansFont;

    dl->AddRectFilled(viewRect.p0(), viewRect.p1(), withAlpha(t.bg, t.veilAlpha));

    using upix::i18n::Key;
    using upix::i18n::tr;
    const bool missingModel = !snap.job.missing_model.empty();
    const bool noModel = snap.model.empty();
    const char* titleStr = tr(missingModel ? Key::WbErrMissingModelTitle : Key::WbErrFailedTitle);
    const char* msgStr = snap.job.message.empty() ? tr(Key::WbErrUnknownMsg) : snap.job.message.c_str();
    const bool hasDiag = !snap.job.diag_detail.empty();
    const char* diagStr = snap.job.diag_detail.c_str();
    const char* actLabel = tr(missingModel ? Key::ModelsDownload : (noModel ? Key::WbErrSelectModel : Key::WbErrRetry));
    const char* resetLabel = tr(Key::CommonReset);

    const float cardW = std::min(460.0f, std::max(260.0f, viewRect.w - geist::space::XXXL));
    const ImVec2 titleSz = calcText(mediumFont, geist::type::SUBHEADING, titleStr);
    const ImVec2 msgSz = calcText(sansFont, geist::type::BODY, msgStr);
    const ImVec2 diagSz = hasDiag ? calcText(monoFont, geist::type::MONO, diagStr) : ImVec2(0.0f, 0.0f);
    const ImVec2 actSz = calcText(mediumFont, geist::type::BODY, actLabel);
    const float errBtnH = std::max(ctl::geom::BTN_H, std::ceil(actSz.y) + 14.0f);
    const float actW = std::max(96.0f, std::ceil(actSz.x) + 32.0f);
    const float resetW = std::max(84.0f, std::ceil(calcText(sansFont, geist::type::BODY, resetLabel).x) + 32.0f);

    static flex::Context errCtx;
    flex::Layout fx(errCtx);
    auto host = fx.col({
        .padding = flex::Edges::all(geist::space::LG),
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    auto card = host.col({
        .padding = flex::Edges::all(geist::space::XXL),
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .width = flex::Fixed(cardW),
    });
    auto headRow = card.row({
        .gap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
    });
    const flex::Slot iconSlot = headRow.slot(flex::Fixed(22.0f), flex::Fixed(22.0f));
    const flex::Slot titleSlot = headRow.slot(flex::Shrink(textBasis(titleSz.x), 1.0f).min(48.0f),
                                              flex::Fixed(titleSz.y));
    const flex::Slot msgSlot = card.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(msgSz.y),
        .alignSelf = flex::AlignSelf::Stretch,
    });
    const flex::Slot diagSlot = card.slot({
        .width = flex::Auto(),
        .height = flex::Fixed(diagSz.y),
        .alignSelf = flex::AlignSelf::Stretch,
        .visible = hasDiag,
    });
    auto btnRow = card.row({
        .margin = flex::Edges::only(0.0f, geist::space::SM, 0.0f, 0.0f),
        .gap = geist::space::SM,
        .rowGap = geist::space::SM,
        .justify = flex::Justify::Center,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Stretch,
        .wrap = flex::Wrap::Wrap,
    });
    const flex::Slot actSlot = btnRow.slot(flex::Fixed(actW), flex::Fixed(errBtnH));
    const flex::Slot resetSlot = btnRow.slot(flex::Fixed(resetW), flex::Fixed(errBtnH));

    fx.solve(viewRect);
    const flex::Rect cardR = fx.rect(card);
    const flex::Rect iconR = fx.rect(iconSlot);
    const flex::Rect titleR = fx.rect(titleSlot);
    const flex::Rect msgR = fx.rect(msgSlot);
    const flex::Rect diagR = fx.rect(diagSlot);
    const bool diagVis = fx.isVisible(diagSlot);
    const flex::Rect actR = fx.rect(actSlot);
    const flex::Rect resetR = fx.rect(resetSlot);

    ctl::finish(dl, cardR.p0(), cardR.p1(), ctl::Finish::Menu, geist::radius::LG);
    drawIcon(dl, "alert-circle", iconR.center(), 20.0f, t.error, 1.7f);
    drawTextFit(dl, mediumFont, geist::type::SUBHEADING, titleR, t.error, titleStr, titleSz.x);
    drawTextFitCentered(dl, sansFont, geist::type::BODY, msgR, t.ink, msgStr, msgSz.x);
    if (diagVis) {
        drawTextFitCentered(dl, monoFont, geist::type::MONO, diagR, t.mute, diagStr, diagSz.x);
    }

    if (ctl::secondaryButton(dl, ImGui::GetID("##err_action"), actLabel, mediumFont, actR.p0(), actR.p1())) {
        if (missingModel) {
            app.downloadModel(snap.job.missing_model);
            ui.activeTab = 1;
        } else if (noModel) {
            ui.showModelPicker = true;
        } else {
            app.startJob();
        }
    }
    if (ctl::secondaryButton(dl, ImGui::GetID("##err_reset"), resetLabel, sansFont, resetR.p0(), resetR.p1())) {
        if (snap.inputs.size() <= 1) {
            app.clearInputs();
        } else if (activeItem) {
            app.removeInput(activeItem->path);
        }
    }
}

} // namespace wb
} // namespace upix::views::detail
