// views/host/activity_log.cpp — Activity/Runtime Log flex section builder and renderer.
#include "views/host/internal.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace upix::views::host_detail {
namespace {

float gLogScroll = 0.0f;
float gLogScrollTarget = 0.0f;

LogEntry parseLogEntry(const std::string& raw) {
    const auto p1 = raw.find('|');
    if (p1 == std::string::npos) {
        if (raw.size() >= 3 && raw[0] == '[') {
            const auto close = raw.find(']', 1);
            if (close != std::string::npos && close > 1 && close <= 12) {
                const std::string tag = raw.substr(1, close - 1);
                if (tag.find_first_of(" \t") == std::string::npos) {
                    size_t pos = close + 1;
                    while (pos < raw.size() && raw[pos] == ' ') ++pos;
                    return {"", tag, raw.substr(pos), "", tag == "TASK"};
                }
            }
        }
        return {"", "HOST", raw, "", false};
    }
    const auto p2 = raw.find('|', p1 + 1);
    const auto p3 = (p2 == std::string::npos) ? std::string::npos : raw.find('|', p2 + 1);
    const std::string ts = raw.substr(0, p1);
    std::string tag = (p2 == std::string::npos) ? raw.substr(p1 + 1) : raw.substr(p1 + 1, p2 - p1 - 1);
    if (tag.empty()) tag = "HOST";
    std::string msg;
    std::string note;
    if (p2 != std::string::npos) {
        msg = (p3 == std::string::npos) ? raw.substr(p2 + 1) : raw.substr(p2 + 1, p3 - p2 - 1);
        if (p3 != std::string::npos) note = raw.substr(p3 + 1);
    }
    return {ts, std::move(tag), std::move(msg), std::move(note), false};
}

} // namespace

LogSlots buildActivityLog(upix::ui::flex::Container& page, float colW,
                          const upix::gui::Snapshot& snap,
                          ImFont* sansMed, ImFont* sans, ImFont* mono) {
    using namespace upix::ui;
    LogSlots s;
    const bool running = snap.host.running;
    s.visible = running || !snap.host.logs.empty();
    if (!s.visible) return s;

    const float lineH = std::max(32.0f, std::ceil(std::max(calcText(sans, geist::type::BODY_SMALL, "Ag").y,
                                                           ctl::geom::META_CHIP_H)) + 8.0f);
    const float bodyH = std::max(112.0f, lineH * 3.5f + 8.0f);
    s.scrollUsed = gLogScroll;

    const ImVec2 ts = calcText(sansMed, geist::type::BODY_SMALL, "运行日志");
    const bool hasModel = !snap.model.empty();
    const float modelChipW = hasModel ? ctl::metaChipWidth(mono, snap.model.c_str(), false, 6.0f) : 0.0f;

    const bool havePct = snap.host.task_percent >= 0;
    s.frac = havePct ? std::clamp(snap.host.task_percent, 0, 100) / 100.0f : (running ? 1.0f : 0.0f);
    if (havePct) {
        s.pctLabel = std::to_string(std::clamp(snap.host.task_percent, 0, 100)) + "%";
    } else {
        s.pctLabel = running ? "LIVE" : "IDLE";
    }
    const ImVec2 ps = calcText(mono, geist::type::MONO, s.pctLabel.c_str());
    const float pctBoxW = std::ceil(std::max(ps.x, calcText(mono, geist::type::MONO, "100%").x)) + 2.0f;
    const float headH = std::max(36.0f, std::ceil(std::max(ts.y, ctl::geom::META_CHIP_H)) + 12.0f);

    auto sec = page.col({.width = flex::Grow(1.0f)});
    auto head = sec.row({
        .gap = geist::space::SM,
        .align = flex::Align::Center,
        .width = flex::Grow(1.0f),
        .height = flex::Fixed(headH),
    });
    s.sig = head.slot(flex::Fixed(6.0f), flex::Fixed(6.0f));
    s.title = head.slot(flex::Fixed(std::ceil(ts.x) + 4.0f), flex::Fixed(ts.y));
    s.model = head.slot(flex::Shrink(modelChipW, 1.0f).min(48.0f),
                        flex::Fixed(ctl::geom::META_CHIP_H),
                        flex::AlignSelf::Center, {}, hasModel);
    head.spacer(1.0f);
    s.bar = head.slot(flex::Shrink(136.0f, 1.0f).min(28.0f), flex::Fixed(ctl::geom::PROGRESS_H));
    auto pctBox = head.row({
        .justify = flex::Justify::End,
        .align = flex::Align::Center,
        .width = flex::Fixed(pctBoxW),
        .height = flex::Fixed(ps.y),
    });
    s.pct = pctBox.slot(flex::Fixed(std::ceil(ps.x)), flex::Fixed(ps.y));
    s.headDiv = sec.slot(flex::Grow(1.0f), flex::Fixed(hairlinePx()));

    s.viewport = sec.col({
        .width = flex::Grow(1.0f),
        .height = flex::Fixed(bodyH),
    });
    s.list = s.viewport.col({
        .margin = flex::Edges::only(0.0f, 8.0f - s.scrollUsed, 0.0f, 0.0f),
        .width = flex::Grow(1.0f),
        .height = flex::Auto(),
    });

    std::vector<LogEntry> entries;
    if (snap.host.logs.empty()) {
        entries.push_back({"", "HOST", "等待配对…", "", false});
    } else {
        entries.reserve(snap.host.logs.size());
        for (const std::string& raw : snap.host.logs) {
            LogEntry e = parseLogEntry(raw);
            e.task = (e.tag == "TASK");
            entries.push_back(std::move(e));
        }
    }

    bool hasAnyTime = false;
    float maxBadgeW = 0.0f;
    for (const LogEntry& e : entries) {
        if (!e.ts.empty()) hasAnyTime = true;
        maxBadgeW = std::max(maxBadgeW, ctl::metaChipWidth(mono, e.tag.c_str(), false, 6.0f));
    }

    const bool showTime = (colW >= 460.0f) && hasAnyTime;
    const ImVec2 timeSampleSz = calcText(mono, geist::type::MONO, "00:00:00");
    const float timeColW = std::ceil(timeSampleSz.x) + 2.0f;
    const float badgeColW = std::ceil(maxBadgeW);

    s.lines.reserve(entries.size());
    for (LogEntry& entry : entries) {
        const ImVec2 nsz = entry.note.empty() ? ImVec2(0.0f, 0.0f) : calcText(mono, geist::type::MONO, entry.note.c_str());
        const ImVec2 msz = calcText(sans, geist::type::BODY_SMALL, entry.msg.c_str());
        auto lr = s.list.row({
            .gap = 10.0f,
            .align = flex::Align::Center,
            .width = flex::Grow(1.0f),
            .height = flex::Fixed(lineH),
        });
        LogRowSlots row;
        row.time = lr.slot(flex::Fixed(timeColW), flex::Fixed(std::ceil(timeSampleSz.y)),
                           flex::AlignSelf::Center, {}, showTime);
        // Fixed badge column matched to the widest active tag so messages align cleanly.
        row.badge = lr.slot(flex::Fixed(badgeColW), flex::Fixed(ctl::geom::META_CHIP_H));
        row.msg = lr.slot(flex::Grow(1.0f, 40.0f).min(40.0f), flex::Fixed(msz.y));
        auto noteBox = lr.row({
            .justify = flex::Justify::End,
            .align = flex::Align::Center,
            .alignSelf = flex::AlignSelf::Center,
            .width = flex::Fixed(std::ceil(nsz.x) + 2.0f),
            .height = flex::Fixed(nsz.y),
            .visible = !entry.note.empty(),
        });
        row.note = noteBox.slot(flex::Fixed(std::ceil(nsz.x)), flex::Fixed(nsz.y));
        row.data = std::move(entry);
        s.lines.push_back(std::move(row));
    }
    return s;
}

void drawActivityLog(ImDrawList* dl, const upix::ui::flex::Layout& fx, const LogSlots& s,
                     const UiViewState& ui, const upix::gui::Snapshot& snap,
                     ImFont* sansMed, ImFont* sans, ImFont* mono,
                     bool modal, bool& wheelConsumed) {
    if (!s.visible) return;
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const ImU32 sig = snap.host.running ? t.accent : t.mute;

    dl->AddCircleFilled(snapPt(fx.rect(s.sig).center()), 3.0f, sig);
    {
        const flex::Rect tr = fx.rect(s.title);
        drawTextCenteredY(dl, sansMed, geist::type::BODY_SMALL, tr.x, tr.y, tr.bottom(), t.mute, "运行日志");
    }
    if (fx.isVisible(s.model)) {
        const flex::Rect mr = fx.rect(s.model);
        const std::string fitted = fitTextEllipsis(mono, geist::type::BODY_SMALL, snap.model.c_str(),
                                                   std::max(16.0f, mr.w - 12.0f));
        ctl::metaChip(dl, mono, mr.p0(), fitted.c_str(), 0, t.body, t.tagBg, ctl::geom::META_CHIP_H, 6.0f);
    }
    const flex::Rect barR = fx.rect(s.bar);
    if (barR.w >= 16.0f) {
        ctl::progressBar(dl, ImGui::GetID("host.taskbar"), barR.p0(), barR.w, s.frac, ctl::geom::PROGRESS_H);
    }
    const flex::Rect pctR = fx.rect(s.pct);
    drawTextCenteredY(dl, mono, geist::type::MONO, pctR.x, pctR.y, pctR.bottom(), sig,
                      s.pctLabel.c_str());

    const flex::Rect hd = fx.rect(s.headDiv);
    ctl::hairlineRule(dl, hd.x, hd.right(), hd.y);

    const flex::Rect bodyR = s.viewport.rect();
    const float totalLogH = 12.0f + s.list.rect().h;
    const float maxLogScroll = std::max(0.0f, totalLogH - bodyR.h);

    dl->PushClipRect(ImVec2(bodyR.x, bodyR.y + 2.0f), ImVec2(bodyR.right(), bodyR.bottom() - 2.0f), true);
    for (const LogRowSlots& lr : s.lines) {
        if (fx.isVisible(lr.time) && !lr.data.ts.empty()) {
            const flex::Rect tr = fx.rect(lr.time);
            drawTextCenteredY(dl, mono, geist::type::MONO, tr.x, tr.y, tr.bottom(), t.faint, lr.data.ts.c_str());
        }

        const bool isErr = (lr.data.tag == "ERR");
        const ImU32 tagFg = lr.data.task ? t.accent : (isErr ? t.error : t.mute);
        const ImU32 tagBg = lr.data.task ? t.accentSoft : (isErr ? t.errorSoft : t.tagBg);
        ctl::metaChip(dl, mono, fx.p0(lr.badge), lr.data.tag.c_str(), 0,
                      tagFg, tagBg, ctl::geom::META_CHIP_H, 6.0f);

        const flex::Rect mr = fx.rect(lr.msg);
        ellipsisTextCentered(dl, sans, geist::type::BODY_SMALL, mr.x, mr.y, mr.bottom(), mr.w,
                             lr.data.task ? t.ink : t.body, lr.data.msg.c_str());

        if (fx.isVisible(lr.note)) {
            const flex::Rect nr = fx.rect(lr.note);
            drawTextCenteredY(dl, mono, geist::type::MONO, nr.x, nr.y, nr.bottom(),
                              lr.data.task ? t.accent : t.mute, lr.data.note.c_str());
        }
    }
    dl->PopClipRect();

    ctl::scrollThumb(dl, bodyR.p0(), bodyR.p1(), s.scrollUsed, totalLogH);

    const ImGuiIO& io = ImGui::GetIO();
    if (!modal && !ui.showIpPicker && ctl::mouseIn(bodyR.p0(), bodyR.p1()) && io.MouseWheel != 0.0f &&
        maxLogScroll > 0.0f) {
        gLogScrollTarget = std::clamp(gLogScrollTarget - io.MouseWheel * 28.0f, 0.0f, maxLogScroll);
        wheelConsumed = true;
    } else {
        gLogScrollTarget = std::clamp(gLogScrollTarget, 0.0f, maxLogScroll);
    }
    gLogScroll = std::clamp(motion::smoothScroll(ImGui::GetID("host.log_scroll"), gLogScrollTarget),
                            0.0f, maxLogScroll);
}

} // namespace upix::views::host_detail
