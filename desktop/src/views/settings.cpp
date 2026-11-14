#include "views/common.hpp"

#include "ui/flex.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <string>

namespace upix::views {
namespace {

constexpr float kPagePad = 32.0f;
constexpr float kColMax = 720.0f;
constexpr float kSectionGap = 28.0f;
constexpr float kHeadToRows = 10.0f;
constexpr float kRowPadY = 12.0f;
constexpr float kRowMinH = 56.0f;

float gScroll = 0.0f;
float gScrollTarget = 0.0f;
float gContentH = 0.0f;

float calcSegmentedW(ImFont* font, const char* const* labels, int count, float minCellW = 48.0f) {
    using namespace upix::ui;
    float maxLabelW = 0.0f;
    for (int i = 0; i < count; ++i) {
        maxLabelW = std::max(maxLabelW, calcText(font, geist::type::LABEL, labels[i]).x);
    }
    const float cellW = std::max(minCellW, std::ceil(maxLabelW + 28.0f));
    return cellW * static_cast<float>(count) + ctl::geom::SEG_PAD * 2.0f;
}

struct TitleSubSlots {
    upix::ui::flex::Container col;
    upix::ui::flex::Slot title;
    upix::ui::flex::Slot sub;
};

TitleSubSlots addTitleSubCol(upix::ui::flex::Container& parent, ImFont* titleFont, ImFont* subFont,
                             const char* title, const char* sub,
                             upix::ui::flex::Len widthSpec = upix::ui::flex::Grow(1.0f).min(48.0f)) {
    using namespace upix::ui;
    const ImVec2 tsz = calcText(titleFont, geist::type::BODY, title ? title : "");
    const bool hasSub = sub && sub[0] != '\0';
    const ImVec2 ssz = hasSub ? calcText(subFont, ctl::geom::ROW_SUB, sub) : ImVec2(0.0f, 0.0f);
    auto c = parent.col({.gap = 2.0f, .width = widthSpec, .height = flex::Auto()});
    auto tSlot = c.slot(flex::Grow(1.0f), flex::Fixed(tsz.y));
    flex::Slot sSlot;
    if (hasSub) {
        sSlot = c.slot(flex::Grow(1.0f), flex::Fixed(ssz.y));
    }
    return {c, tSlot, sSlot};
}

void drawTitleSub(ImDrawList* dl, ImFont* titleFont, ImFont* subFont, const upix::ui::flex::Layout& fx,
                  const TitleSubSlots& slots, const char* title, const char* sub) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ellipsisText(dl, titleFont, geist::type::BODY, fx.p0(slots.title), fx.size(slots.title).x,
                 t.ink, title ? title : "");
    if (slots.sub.valid() && sub && sub[0] != '\0') {
        ellipsisText(dl, subFont, ctl::geom::ROW_SUB, fx.p0(slots.sub), fx.size(slots.sub).x,
                     t.mute, sub);
    }
}

struct ControlRowSlots {
    upix::ui::flex::Container row;
    TitleSubSlots text;
    upix::ui::flex::Slot control;
};

ControlRowSlots addControlRow(upix::ui::flex::Container& list, ImFont* titleFont, ImFont* subFont,
                              const char* title, const char* sub,
                              upix::ui::flex::Len ctrlW, upix::ui::flex::Len ctrlH, bool visible = true) {
    using namespace upix::ui;
    auto r = list.row({
        .padding = flex::Edges::xy(0.0f, kRowPadY),
        .gap = ctl::geom::ROW_GAP,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto().min(kRowMinH),
        .visible = visible,
    });
    auto text = addTitleSubCol(r, titleFont, subFont, title, sub);
    auto ctrl = r.slot(ctrlW, ctrlH, flex::AlignSelf::Center);
    return {r, text, ctrl};
}

struct SectionSlots {
    upix::ui::flex::Slot header;
    upix::ui::flex::Container list;
};

SectionSlots addSection(upix::ui::flex::Container& page, ImFont* headFont, const char* label) {
    using namespace upix::ui;
    auto sec = page.col({.gap = kHeadToRows, .width = flex::Grow(1.0f), .height = flex::Auto()});
    const ImVec2 hsz = calcText(headFont, geist::type::BODY_SMALL, label ? label : "");
    auto hdr = sec.slot(flex::Grow(1.0f), flex::Fixed(hsz.y));
    auto list = sec.col({.width = flex::Grow(1.0f), .height = flex::Auto()});
    return {hdr, list};
}

void drawSectionHairlines(ImDrawList* dl, const upix::ui::flex::Layout& fx,
                          const upix::ui::flex::Container& list,
                          const upix::ui::flex::Slot* rows, size_t count) {
    const ImVec2 lp0 = list.p0();
    const ImVec2 lp1 = list.p1();
    upix::ui::ctl::hairlineRule(dl, lp0.x, lp1.x, lp0.y);
    for (size_t i = 0; i < count; ++i) {
        if (!fx.isVisible(rows[i])) continue;
        upix::ui::ctl::hairlineRule(dl, lp0.x, lp1.x, fx.p1(rows[i]).y);
    }
}

template <typename T, size_t N>
int findOptionIndex(const T (&vals)[N], const T& current, int fallback = 0) {
    for (size_t i = 0; i < N; ++i) {
        if (current == vals[i]) return static_cast<int>(i);
    }
    return fallback;
}

struct ScrollCtx {
    float paneY = 0.0f;
    float paneH = 0.0f;
    float maxScroll = 0.0f;
    bool interactive = true;
};

void drawRowHover(ImDrawList* dl, const upix::ui::flex::Layout& fx, upix::ui::flex::Slot row,
                  const char* idKey, const ScrollCtx& sc) {
    using namespace upix::ui;
    const flex::Rect r = fx.rect(row);
    const ImVec2 p0 = snapPt(r.x - 10.0f, r.y + 1.0f);
    const ImVec2 p1 = snapPt(r.right() + 10.0f, r.bottom() - 1.0f);
    const bool hovered = sc.interactive && ctl::mouseIn(p0, p1);
    const float hovT = motion::hoverFactor(ImGui::GetID(idKey), hovered);
    if (hovT > 0.01f) {
        ctl::hoverWashAlpha(dl, p0, p1, geist::radius::MD, hovT, 0.04f);
    }
}

void keepNavFocusInView(const upix::ui::flex::Rect& r, const ScrollCtx& sc) {
    if (!(ImGui::IsItemFocused() && ImGui::GetIO().NavVisible)) return;
    const float top = (r.y - sc.paneY) + gScroll;
    const float bot = (r.bottom() - sc.paneY) + gScroll;
    if (top - gScrollTarget < kPagePad) {
        gScrollTarget = std::clamp(top - kPagePad, 0.0f, sc.maxScroll);
    } else if (bot - gScrollTarget > sc.paneH - kPagePad) {
        gScrollTarget = std::clamp(bot - (sc.paneH - kPagePad), 0.0f, sc.maxScroll);
    }
}

struct EngineSection {
    SectionSlots sec;
    ControlRowSlots gpu;
    ControlRowSlots tile;
    upix::ui::flex::Container daemonRow;
    upix::ui::flex::Slot daemonTitle;
    upix::ui::flex::Slot daemonDot;
    upix::ui::flex::Slot daemonStatusText;
    upix::ui::flex::Slot daemonToggle;
    ControlRowSlots idle;
    float devSegW = 0.0f;
    float tileSegW = 0.0f;
    float idleSegW = 0.0f;
    std::string statusBuf;
};

EngineSection buildEngineSection(upix::ui::flex::Container& page, const UiViewState& ui,
                                 const upix::gui::Snapshot& snap, ImFont* sansMed) {
    using namespace upix::ui;
    EngineSection out;
    out.sec = addSection(page, sansMed, "推理引擎");

    static constexpr const char* kDevLabels[2] = {"GPU", "CPU"};
    static constexpr const char* kTileLabels[4] = {"自动", "512", "256", "128"};
    static constexpr const char* kIdleLabels[4] = {"5分钟", "15分钟", "30分钟", "不退出"};
    out.devSegW = calcSegmentedW(ui.fontSans, kDevLabels, 2, 64.0f);
    out.tileSegW = calcSegmentedW(ui.fontSans, kTileLabels, 4, 54.0f);
    out.idleSegW = calcSegmentedW(ui.fontSans, kIdleLabels, 4, 60.0f);

    out.gpu = addControlRow(out.sec.list, sansMed, ui.fontSans, "计算设备", nullptr,
                            flex::Shrink(out.devSegW, 1.0f).min(72.0f), flex::Fixed(ctl::geom::SEG_H_COMPACT));
    out.tile = addControlRow(out.sec.list, sansMed, ui.fontSans, "分块尺寸", nullptr,
                             flex::Shrink(out.tileSegW, 1.0f).min(144.0f), flex::Fixed(ctl::geom::SEG_H_COMPACT));

    out.statusBuf = snap.daemon.loading
        ? "正在启动…"
        : (snap.daemon.running ? "已就绪" : "未开启");
    if (!snap.daemon.loading && snap.daemon.running && snap.daemon.pid > 0) {
        char pidBuf[64];
        std::snprintf(pidBuf, sizeof(pidBuf), "已就绪 · PID %d", snap.daemon.pid);
        out.statusBuf = pidBuf;
    }
    const ImVec2 daemonTsz = calcText(sansMed, geist::type::BODY, "常驻热引擎");
    const ImVec2 statusSsz = calcText(ui.fontSans, ctl::geom::ROW_SUB, out.statusBuf.c_str());

    out.daemonRow = out.sec.list.row({
        .padding = flex::Edges::xy(0.0f, kRowPadY),
        .gap = ctl::geom::ROW_GAP,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto().min(kRowMinH),
    });
    auto leftCol = out.daemonRow.col({.gap = 3.0f, .width = flex::Grow(1.0f).min(48.0f), .height = flex::Auto()});
    out.daemonTitle = leftCol.slot(flex::Grow(1.0f), flex::Fixed(daemonTsz.y));
    auto statusRow = leftCol.row({
        .gap = 6.0f,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto(),
    });
    const float dotBoxW = snap.daemon.loading ? 10.0f : 7.0f;
    out.daemonDot = statusRow.slot(flex::Fixed(dotBoxW), flex::Fixed(statusSsz.y), flex::AlignSelf::Center);
    out.daemonStatusText = statusRow.slot(flex::Grow(1.0f), flex::Fixed(statusSsz.y));
    out.daemonToggle = out.daemonRow.slot(flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H),
                                          flex::AlignSelf::Center);

    out.idle = addControlRow(out.sec.list, sansMed, ui.fontSans, "常驻时间", nullptr,
                             flex::Shrink(out.idleSegW, 1.0f).min(144.0f), flex::Fixed(ctl::geom::SEG_H_COMPACT));
    return out;
}

void drawEngineSection(ImDrawList* dl, const upix::ui::flex::Layout& fx, const EngineSection& s,
                       const UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                       ImFont* sansMed, float colW, const ScrollCtx& sc) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    ctl::eyebrowHeader(dl, sansMed, fx.p0(s.sec.header), colW, "推理引擎");
    const flex::Slot rows[] = {s.gpu.row, s.tile.row, s.daemonRow, s.idle.row};
    drawSectionHairlines(dl, fx, s.sec.list, rows, std::size(rows));

    static constexpr const char* kDevLabels[2] = {"GPU", "CPU"};
    drawRowHover(dl, fx, s.gpu.row, "row-gpu", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.gpu.text, "计算设备", nullptr);
    int devIdx = snap.gpu ? 0 : 1;
    if (ctl::segmented(dl, ImGui::GetID("seg-gpu"), fx.p0(s.gpu.control), fx.size(s.gpu.control),
                       kDevLabels, 2, devIdx, ui.fontSans, geist::type::LABEL, false, true)) {
        app.setGpu(devIdx == 0);
    }

    static constexpr const char* kTileLabels[4] = {"自动", "512", "256", "128"};
    static constexpr int kTileVals[4] = {0, 512, 256, 128};
    drawRowHover(dl, fx, s.tile.row, "row-tile", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.tile.text, "分块尺寸", nullptr);
    int tileIdx = findOptionIndex(kTileVals, snap.tile_size);
    if (ctl::segmented(dl, ImGui::GetID("seg-tile"), fx.p0(s.tile.control), fx.size(s.tile.control),
                       kTileLabels, 4, tileIdx, ui.fontSans, geist::type::LABEL, false, true)) {
        app.setTileSize(kTileVals[tileIdx]);
    }

    drawRowHover(dl, fx, s.daemonRow, "row-daemon", sc);
    ellipsisText(dl, sansMed, geist::type::BODY, fx.p0(s.daemonTitle), fx.size(s.daemonTitle).x,
                 t.ink, "常驻热引擎");
    const ImVec2 dotC = fx.rect(s.daemonDot).center();
    if (snap.daemon.loading) {
        ctl::busyArc(dl, snapPt(dotC), 4.2f, t.accent, 1.5f);
    } else {
        dl->AddCircleFilled(snapPt(dotC), 3.0f, snap.daemon.running ? t.accent : t.faint);
    }
    const flex::Rect dst = fx.rect(s.daemonStatusText);
    ellipsisTextCentered(dl, ui.fontSans, ctl::geom::ROW_SUB, dst.x, dst.y, dst.bottom(), dst.w,
                         snap.daemon.loading ? t.accent : (snap.daemon.running ? t.body : t.mute),
                         s.statusBuf.c_str());
    bool daemonOn = snap.daemon.running || snap.daemon.loading;
    if (ctl::toggleSwitch(dl, ImGui::GetID("sw-daemon"), fx.p0(s.daemonToggle), daemonOn, !snap.daemon.loading)) {
        app.toggleDaemon();
    }
    keepNavFocusInView(fx.rect(s.daemonToggle), sc);

    static constexpr const char* kIdleLabels[4] = {"5分钟", "15分钟", "30分钟", "不退出"};
    static constexpr int kIdleVals[4] = {300, 900, 1800, 0};
    drawRowHover(dl, fx, s.idle.row, "row-idle", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.idle.text, "常驻时间", nullptr);
    int idleIdx = findOptionIndex(kIdleVals, snap.daemon.idle_secs);
    if (ctl::segmented(dl, ImGui::GetID("seg-idle"), fx.p0(s.idle.control), fx.size(s.idle.control),
                       kIdleLabels, 4, idleIdx, ui.fontSans, geist::type::LABEL, false, true)) {
        app.setDaemonIdleSecs(kIdleVals[idleIdx]);
    }
}

struct EncodingSection {
    SectionSlots sec;
    ControlRowSlots fmt;
    upix::ui::flex::Container qualityRow;
    TitleSubSlots qualityText;
    upix::ui::flex::Slot qualitySlider;
    upix::ui::flex::Slot qualityInput;
    ControlRowSlots chroma;
    ControlRowSlots speed;
    ControlRowSlots rec;
    ControlRowSlots force;
};

EncodingSection buildEncodingSection(upix::ui::flex::Container& page, const UiViewState& ui,
                                     const upix::gui::Snapshot& snap, ImFont* sansMed) {
    using namespace upix::ui;
    EncodingSection out;
    out.sec = addSection(page, sansMed, "默认编码与批处理");

    const bool isPng = (snap.format == "png");
    const bool isAvif = (snap.format == "avif");
    const bool showQuality = !isPng;
    const bool showChroma = (snap.format == "jpg" || snap.format == "avif");
    const bool showSpeed = isAvif;

    static constexpr const char* kFmtLabels[5] = {"跟随", "PNG", "JPEG", "WebP", "AVIF"};
    static constexpr const char* kChromaLabels[2] = {"4:4:4", "4:2:0"};
    static constexpr const char* kSpeedLabels[3] = {"精细", "均衡", "快速"};
    const float fmtSegW = calcSegmentedW(ui.fontSans, kFmtLabels, 5, 52.0f);
    const float chromaSegW = calcSegmentedW(ui.fontMono, kChromaLabels, 2, 64.0f);
    const float speedSegW = calcSegmentedW(ui.fontSans, kSpeedLabels, 3, 58.0f);

    out.fmt = addControlRow(out.sec.list, sansMed, ui.fontSans, "默认输出格式", nullptr,
                            flex::Shrink(fmtSegW, 1.0f).min(176.0f), flex::Fixed(ctl::geom::SEG_H_COMPACT));

    const float qInputW = std::max(56.0f, std::ceil(calcText(ui.fontMono, geist::type::MONO, "100").x) + 20.0f);
    const float qInputH = std::max(28.0f, std::ceil(calcText(ui.fontMono, geist::type::MONO, "100").y) + 8.0f);
    const float qRightH = std::max(ctl::geom::SLIDER_H, qInputH);

    out.qualityRow = out.sec.list.row({
        .padding = flex::Edges::xy(0.0f, kRowPadY),
        .gap = ctl::geom::ROW_GAP,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto().min(kRowMinH),
        .visible = showQuality,
    });
    out.qualityText = addTitleSubCol(out.qualityRow, sansMed, ui.fontSans, "默认质量", nullptr);
    auto qRight = out.qualityRow.row({
        .gap = 10.0f,
        .align = flex::Align::Center,
        .alignSelf = flex::AlignSelf::Center,
        .width = flex::Shrink(188.0f + qInputW, 1.0f).min(144.0f),
        .height = flex::Fixed(qRightH),
    });
    out.qualitySlider = qRight.slot(flex::Grow(1.0f, 176.0f).min(78.0f),
                                    flex::Fixed(ctl::geom::SLIDER_H), flex::AlignSelf::Center);
    out.qualityInput = qRight.slot(flex::Fixed(qInputW), flex::Fixed(qInputH), flex::AlignSelf::Center);

    out.chroma = addControlRow(out.sec.list, sansMed, ui.fontSans, "色度抽样", nullptr,
                               flex::Shrink(chromaSegW, 1.0f).min(72.0f),
                               flex::Fixed(ctl::geom::SEG_H_COMPACT), showChroma);
    out.speed = addControlRow(out.sec.list, sansMed, ui.fontSans, "编码速度", nullptr,
                              flex::Shrink(speedSegW, 1.0f).min(108.0f),
                              flex::Fixed(ctl::geom::SEG_H_COMPACT), showSpeed);

    out.rec = addControlRow(out.sec.list, sansMed, ui.fontSans, "文件夹包含子目录", nullptr,
                            flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H));
    out.force = addControlRow(out.sec.list, sansMed, ui.fontSans, "忽略超分标记强制重跑", nullptr,
                              flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H));
    return out;
}

void drawEncodingSection(ImDrawList* dl, const upix::ui::flex::Layout& fx, const EncodingSection& s,
                         const UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                         ImFont* sansMed, float colW, const ScrollCtx& sc) {
    using namespace upix::ui;
    ctl::eyebrowHeader(dl, sansMed, fx.p0(s.sec.header), colW, "默认编码与批处理");

    const flex::Slot rows[] = {s.fmt.row, s.qualityRow, s.chroma.row, s.speed.row,
                               s.rec.row, s.force.row};
    drawSectionHairlines(dl, fx, s.sec.list, rows, std::size(rows));

    static constexpr const char* kFmtLabels[5] = {"跟随", "PNG", "JPEG", "WebP", "AVIF"};
    static constexpr const char* kFmtVals[5] = {"", "png", "jpg", "webp", "avif"};
    drawRowHover(dl, fx, s.fmt.row, "row-fmt", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.fmt.text, "默认输出格式", nullptr);
    int fmtIdx = 0;
    for (int i = 0; i < 5; ++i) {
        if (snap.format == kFmtVals[i]) fmtIdx = i;
    }
    if (ctl::segmented(dl, ImGui::GetID("seg-fmt"), fx.p0(s.fmt.control), fx.size(s.fmt.control),
                       kFmtLabels, 5, fmtIdx, ui.fontSans, geist::type::LABEL, false, true)) {
        app.setFormat(kFmtVals[fmtIdx]);
    }

    if (fx.isVisible(s.qualityRow)) {
        drawRowHover(dl, fx, s.qualityRow, "row-quality", sc);
        drawTitleSub(dl, sansMed, ui.fontSans, fx, s.qualityText, "默认质量", nullptr);
        int q = snap.quality;
        if (ctl::slider(dl, ImGui::GetID("qs-quality"), fx.p0(s.qualitySlider),
                        fx.size(s.qualitySlider).x, q, 1, 100)) {
            app.setQuality(q);
        }
        keepNavFocusInView(fx.rect(s.qualitySlider), sc);
        if (ctl::qualityInput(dl, ImGui::GetID("qi-quality"), fx.p0(s.qualityInput), q, 1, 100, ui.fontMono)) {
            app.setQuality(q);
        }
        keepNavFocusInView(fx.rect(s.qualityInput), sc);
    }

    if (fx.isVisible(s.chroma.row)) {
        static constexpr const char* kChromaLabels[2] = {"4:4:4", "4:2:0"};
        static constexpr int kChromaVals[2] = {444, 420};
        drawRowHover(dl, fx, s.chroma.row, "row-chroma", sc);
        drawTitleSub(dl, sansMed, ui.fontSans, fx, s.chroma.text, "色度抽样", nullptr);
        int cIdx = (snap.chroma == 444) ? 0 : 1;
        if (ctl::segmented(dl, ImGui::GetID("seg-chroma"), fx.p0(s.chroma.control), fx.size(s.chroma.control),
                           kChromaLabels, 2, cIdx, ui.fontMono, geist::type::LABEL, false, true)) {
            app.setChroma(kChromaVals[cIdx]);
        }
    }

    if (fx.isVisible(s.speed.row)) {
        static constexpr const char* kSpeedLabels[3] = {"精细", "均衡", "快速"};
        static constexpr int kSpeedVals[3] = {4, 6, 8};
        drawRowHover(dl, fx, s.speed.row, "row-speed", sc);
        drawTitleSub(dl, sansMed, ui.fontSans, fx, s.speed.text, "编码速度", nullptr);
        int sIdx = findOptionIndex(kSpeedVals, snap.avif_speed, 1);
        if (ctl::segmented(dl, ImGui::GetID("seg-speed"), fx.p0(s.speed.control), fx.size(s.speed.control),
                           kSpeedLabels, 3, sIdx, ui.fontSans, geist::type::LABEL, false, true)) {
            app.setAvifSpeed(kSpeedVals[sIdx]);
        }
    }

    drawRowHover(dl, fx, s.rec.row, "row-rec", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.rec.text, "文件夹包含子目录", nullptr);
    bool rec = snap.recursive;
    if (ctl::toggleSwitch(dl, ImGui::GetID("sw-rec"), fx.p0(s.rec.control), rec)) {
        app.setRecursive(rec);
    }
    keepNavFocusInView(fx.rect(s.rec.control), sc);

    drawRowHover(dl, fx, s.force.row, "row-force", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.force.text, "忽略超分标记强制重跑", nullptr);
    bool force = snap.force;
    if (ctl::toggleSwitch(dl, ImGui::GetID("sw-force"), fx.p0(s.force.control), force)) {
        app.setForce(force);
    }
    keepNavFocusInView(fx.rect(s.force.control), sc);
}

struct AppearanceSection {
    SectionSlots sec;
    ControlRowSlots theme;
    ControlRowSlots motion;
};

AppearanceSection buildAppearanceSection(upix::ui::flex::Container& page, const UiViewState& ui,
                                         ImFont* sansMed) {
    using namespace upix::ui;
    AppearanceSection out;
    out.sec = addSection(page, sansMed, "交互与外观");
    static constexpr const char* kThemeLabels[2] = {"深色", "浅色"};
    const float themeSegW = calcSegmentedW(ui.fontSans, kThemeLabels, 2, 64.0f);
    out.theme = addControlRow(out.sec.list, sansMed, ui.fontSans, "外观主题", nullptr,
                              flex::Shrink(themeSegW, 1.0f).min(72.0f), flex::Fixed(ctl::geom::SEG_H_COMPACT));
    out.motion = addControlRow(out.sec.list, sansMed, ui.fontSans, "减少动态效果", nullptr,
                               flex::Fixed(ctl::geom::TOGGLE_W), flex::Fixed(ctl::geom::TOGGLE_H));
    return out;
}

void drawAppearanceSection(ImDrawList* dl, const upix::ui::flex::Layout& fx, const AppearanceSection& s,
                           const UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                           ImFont* sansMed, float colW, const ScrollCtx& sc) {
    using namespace upix::ui;
    ctl::eyebrowHeader(dl, sansMed, fx.p0(s.sec.header), colW, "交互与外观");

    const flex::Slot rows[] = {s.theme.row, s.motion.row};
    drawSectionHairlines(dl, fx, s.sec.list, rows, std::size(rows));

    static constexpr const char* kThemeLabels[2] = {"深色", "浅色"};
    drawRowHover(dl, fx, s.theme.row, "row-theme", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.theme.text, "外观主题", nullptr);
    int themeIdx = snap.dark_mode ? 0 : 1;
    if (ctl::segmented(dl, ImGui::GetID("seg-theme"), fx.p0(s.theme.control), fx.size(s.theme.control),
                       kThemeLabels, 2, themeIdx, ui.fontSans, geist::type::LABEL, false, true)) {
        app.setDarkMode(themeIdx == 0);
    }
    keepNavFocusInView(fx.rect(s.theme.control), sc);

    drawRowHover(dl, fx, s.motion.row, "row-motion", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.motion.text, "减少动态效果", nullptr);
    bool rm = snap.reduce_motion;
    if (ctl::toggleSwitch(dl, ImGui::GetID("sw-rm"), fx.p0(s.motion.control), rm)) {
        app.setReduceMotion(rm);
        motion::reduceMotion() = rm;
    }
    keepNavFocusInView(fx.rect(s.motion.control), sc);
}

struct AboutSection {
    SectionSlots sec;
    upix::ui::flex::Container dirRow;
    TitleSubSlots dirText;
    upix::ui::flex::Slot dirBtn;
    upix::ui::flex::Container dirWell;
    upix::ui::flex::Slot dirIcon;
    upix::ui::flex::Slot dirPath;
    upix::ui::flex::Container aboutRow;
    upix::ui::flex::Slot aboutTitle;
    upix::ui::flex::Slot aboutVer;
    std::string verStr;
};

AboutSection buildAboutSection(upix::ui::flex::Container& page, const UiViewState& ui,
                               const upix::gui::Snapshot& snap, ImFont* sansMed) {
    using namespace upix::ui;
    AboutSection out;
    out.sec = addSection(page, sansMed, "目录与关于");

    const ImVec2 btnSz = calcText(ui.fontSans, geist::type::BODY, "打开目录");
    const float dirBtnW = std::max(84.0f, std::ceil(btnSz.x) + 28.0f);
    const float dirBtnH = std::max(32.0f, std::ceil(btnSz.y) + 12.0f);
    const char* dirPath = snap.models_dir.empty() ? "—" : snap.models_dir.c_str();
    const ImVec2 dirPathSz = calcText(ui.fontMono, geist::type::MONO, dirPath);
    const float dirWellH = std::max(32.0f, std::ceil(dirPathSz.y) + 12.0f);

    out.dirRow = out.sec.list.col({
        .padding = flex::Edges::xy(0.0f, kRowPadY),
        .gap = 6.0f,
        .width = flex::Grow(1.0f),
        .height = flex::Auto(),
    });
    auto dirTop = out.dirRow.row({
        .gap = ctl::geom::ROW_GAP,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto(),
    });
    out.dirText = addTitleSubCol(dirTop, sansMed, ui.fontSans, "模型存储目录", nullptr);
    out.dirBtn = dirTop.slot(flex::Fixed(dirBtnW), flex::Fixed(dirBtnH), flex::AlignSelf::Center);
    out.dirWell = out.dirRow.row({
        .padding = flex::Edges::xy(10.0f, 0.0f),
        .gap = 6.0f,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Fixed(dirWellH),
    });
    out.dirIcon = out.dirWell.slot(flex::Fixed(13.0f), flex::Fixed(13.0f), flex::AlignSelf::Center);
    out.dirPath = out.dirWell.slot(flex::Grow(1.0f).min(24.0f), flex::Fixed(dirPathSz.y), flex::AlignSelf::Center);

    out.verStr = "v";
    out.verStr += snap.version.empty() ? "0.2.0" : snap.version;
    out.verStr += " · ncnn · Vulkan";
    const ImVec2 aboutTsz = calcText(sansMed, geist::type::BODY, "Upix");
    const ImVec2 verSz = calcText(ui.fontMono, geist::type::MONO, out.verStr.c_str());

    out.aboutRow = out.sec.list.row({
        .padding = flex::Edges::xy(0.0f, kRowPadY),
        .gap = ctl::geom::ROW_GAP,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Auto().min(44.0f),
    });
    out.aboutTitle = out.aboutRow.slot(flex::Grow(1.0f).min(40.0f), flex::Fixed(aboutTsz.y));
    out.aboutVer = out.aboutRow.slot(flex::Shrink(verSz.x, 1.0f).min(40.0f), flex::Fixed(verSz.y));
    return out;
}

void drawAboutSection(ImDrawList* dl, const upix::ui::flex::Layout& fx, const AboutSection& s,
                      const UiViewState& ui, const upix::gui::Snapshot& snap,
                      ImFont* sansMed, float colW, const ScrollCtx& sc) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ctl::eyebrowHeader(dl, sansMed, fx.p0(s.sec.header), colW, "目录与关于");

    const flex::Slot rows[] = {s.dirRow, s.aboutRow};
    drawSectionHairlines(dl, fx, s.sec.list, rows, std::size(rows));

    drawRowHover(dl, fx, s.dirRow, "row-dir", sc);
    drawTitleSub(dl, sansMed, ui.fontSans, fx, s.dirText, "模型存储目录", nullptr);
    if (ctl::secondaryButton(dl, ImGui::GetID("btn-open-mdir"), "打开目录", ui.fontSans,
                             fx.p0(s.dirBtn), fx.p1(s.dirBtn))) {
        std::error_code ec;
        if (!snap.models_dir.empty()) {
            std::filesystem::create_directories(snap.models_dir, ec);
            SDL_OpenURL(("file://" + snap.models_dir).c_str());
        }
    }
    keepNavFocusInView(fx.rect(s.dirBtn), sc);

    ctl::flatSection(dl, s.dirWell.p0(), s.dirWell.p1(), geist::radius::MD, t.surface, t.outline);
    drawIcon(dl, "folder", fx.rect(s.dirIcon).center(), 13.0f, t.mute, 1.5f);
    const flex::Rect dr = fx.rect(s.dirPath);
    const char* dirPath = snap.models_dir.empty() ? "—" : snap.models_dir.c_str();
    ellipsisTextCentered(dl, ui.fontMono, geist::type::MONO, dr.x, dr.y, dr.bottom(), dr.w, t.body, dirPath);

    const flex::Rect atr = fx.rect(s.aboutTitle);
    ellipsisTextCentered(dl, sansMed, geist::type::BODY, atr.x, atr.y, atr.bottom(), atr.w, t.ink, "Upix");
    const flex::Rect avr = fx.rect(s.aboutVer);
    ellipsisTextCentered(dl, ui.fontMono, geist::type::MONO, avr.x, avr.y, avr.bottom(), avr.w,
                         t.mute, s.verStr.c_str());
}

} // namespace

void renderSettingsPage(ImDrawList* /*dl*/, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float paneX = 0.0f;
    const float paneY = kTopbarH;
    const float paneW = std::max(0.0f, winSize.x);
    const float paneH = std::max(0.0f, winSize.y - kTopbarH);
    const float colW = std::min(kColMax, std::max(320.0f, paneW - 64.0f));
    const float colX = snapPx(paneX + (paneW - colW) * 0.5f);

    ImGui::SetCursorScreenPos(ImVec2(paneX, paneY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##settings_pane", ImVec2(paneW, paneH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::SetScrollY(0.0f);
    ImDrawList* cdl = ImGui::GetWindowDrawList();
    cdl->AddRectFilled(ImVec2(paneX, paneY), winSize, t.bg);

    const ImGuiIO& io = ImGui::GetIO();
    const bool modal = !snap.host.pending_device.empty();
    const bool overPage = !modal && io.MousePos.x >= paneX && io.MousePos.x < winSize.x &&
                          io.MousePos.y >= paneY && io.MousePos.y < winSize.y;
    const float prevMaxScroll = std::max(0.0f, gContentH - paneH);
    if (overPage && io.MouseWheel != 0.0f) {
        gScrollTarget -= io.MouseWheel * 48.0f;
    }
    gScrollTarget = std::clamp(gScrollTarget, 0.0f, prevMaxScroll);
    gScroll = std::clamp(motion::smoothScroll(ImGui::GetID("settings.scroll"), gScrollTarget), 0.0f, prevMaxScroll);

    ImFont* sansMed = ui.fontSansMedium ? ui.fontSansMedium : ui.fontSans;
    const char* pageTitle = "设置";

    static flex::Context sCtx;
    flex::Layout fx(sCtx);
    auto page = fx.col({.gap = kSectionGap, .width = flex::Fixed(colW), .height = flex::Auto()});

    const ImVec2 hsz = calcText(sansMed, geist::type::PAGE_TITLE, pageTitle);
    auto hdrTitleSlot = page.slot(flex::Grow(1.0f), flex::Fixed(hsz.y));

    const EngineSection secEngine = buildEngineSection(page, ui, snap, sansMed);
    const EncodingSection secEncoding = buildEncodingSection(page, ui, snap, sansMed);
    const AppearanceSection secAppearance = buildAppearanceSection(page, ui, sansMed);
    const AboutSection secAbout = buildAboutSection(page, ui, snap, sansMed);

    const flex::Rect pageRect = fx.solveWidth(ImVec2(colX, paneY + kPagePad - gScroll), colW);
    gContentH = pageRect.h + kPagePad * 2.0f;
    const ScrollCtx sc{paneY, paneH, std::max(0.0f, gContentH - paneH), !modal};

    cdl->PushClipRect(ImVec2(paneX, paneY), winSize, true);

    ellipsisText(cdl, sansMed, geist::type::PAGE_TITLE, fx.p0(hdrTitleSlot), fx.size(hdrTitleSlot).x,
                 t.ink, pageTitle);

    drawEngineSection(cdl, fx, secEngine, ui, app, snap, sansMed, colW, sc);
    drawEncodingSection(cdl, fx, secEncoding, ui, app, snap, sansMed, colW, sc);
    drawAppearanceSection(cdl, fx, secAppearance, ui, app, snap, sansMed, colW, sc);
    drawAboutSection(cdl, fx, secAbout, ui, snap, sansMed, colW, sc);

    cdl->PopClipRect();

    ctl::scrollThumb(cdl, ImVec2(paneX, paneY), ImVec2(paneX + paneW, paneY + paneH), gScroll, gContentH);

    ImGui::EndChild();
    ImGui::PopStyleVar(1);
}

} // namespace upix::views
