#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_set>
#include <vector>

namespace upix::views {
namespace {

// View-local stand-ins. controls.hpp has Finish / selectionSpring /
// progressSpring / press springs, but no tooltip bubble, no icon-button,
// and no spinner. Same rules: no entrance motion, springs only for press.

constexpr float kPagePad = 24.0f;
constexpr float kColMax = 760.0f;
constexpr float kToolbarH = 34.0f;
constexpr float kAfterToolbar = 16.0f;
constexpr float kSectionGap = 24.0f;
constexpr float kSectionHeadH = 18.0f;
constexpr float kHeadToCard = 12.0f;
constexpr float kCardGap = 24.0f;
constexpr float kCardHeadH = 36.0f;
constexpr float kRowH = 44.0f;
constexpr float kRowPadX = 16.0f;
constexpr float kEmptyH = 120.0f;
constexpr float kTipDelay = 0.5f; // pointer tooltip; keyboard focus is immediate

struct TipState {
    ImGuiID hot = 0;
    double since = 0.0;
    bool dismissed = false;
    bool show = false;
    ImVec2 a0{};
    ImVec2 a1{};
    const char* text = nullptr;
};

TipState gTip;
bool gTipSeen = false;
float gScroll = 0.0f;
float gContentH = 0.0f;

bool sceneMatches(const std::string& filter, const std::string& scene) {
    // rust SceneFilter::matches. Photo excludes 修复; 综合 also covers 通用 / empty.
    if (filter == "all" || filter.empty()) return true;
    if (filter == "动漫") return scene.find("动漫") != std::string::npos;
    if (filter == "照片") {
        return scene.find("照片") != std::string::npos && scene.find("修复") == std::string::npos;
    }
    if (filter == "综合") {
        return scene.find("综合") != std::string::npos || scene.find("通用") != std::string::npos || scene.empty();
    }
    if (filter == "修复") return scene.find("修复") != std::string::npos;
    return scene.find(filter) != std::string::npos;
}

const char* sceneLabel(const std::string& scene) {
    if (scene.empty() || scene == "通用") return "综合";
    return scene.c_str();
}

ImU32 sceneDot(const std::string& scene) {
    const auto& t = upix::ui::geist::tokens();
    if (scene.find("动漫") != std::string::npos) return t.sceneAnime;
    if (scene.find("修复") != std::string::npos) return t.sceneRestore;
    if (scene.find("照片") != std::string::npos) return t.scenePhoto;
    return t.sceneGeneral;
}

void scaleAbout(ImVec2 p0, ImVec2 p1, float s, ImVec2& q0, ImVec2& q1) {
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    const float hw = (p1.x - p0.x) * 0.5f * s;
    const float hh = (p1.y - p0.y) * 0.5f * s;
    q0 = ImVec2(c.x - hw, c.y - hh);
    q1 = ImVec2(c.x + hw, c.y + hh);
}

void considerTip(ImGuiID id, bool hovered, bool keyFocus, ImVec2 a0, ImVec2 a1, const char* label) {
    if (!hovered && !keyFocus) return;
    gTipSeen = true;
    const double now = ImGui::GetTime();
    if (gTip.hot != id) {
        gTip.hot = id;
        gTip.since = now;
        gTip.dismissed = false;
    }
    const bool due = keyFocus || now - gTip.since >= kTipDelay;
    if (!gTip.dismissed && due) {
        gTip.show = true;
        gTip.a0 = a0;
        gTip.a1 = a1;
        gTip.text = label;
    }
}

// Pure-icon control: 0.94 press spring, hover RaisedHover, tooltip bubble.
// quietError: delete — no raised face; mute icon turns error on hover.
bool iconAction(ImDrawList* dl, const char* id, ImVec2 pos, float box, float iconSz, float radius,
                const char* icon, const char* label, bool quietError) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImVec2 p0 = snapPt(pos);
    ImVec2 p1 = snapPt(pos.x + box, pos.y + box);
    ImGui::SetCursorScreenPos(p0);
    const bool released = ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
    const bool hovered = ImGui::IsItemHovered();
    const bool down = ImGui::IsItemActive();
    const bool keyFocus = ImGui::GetIO().NavVisible && ImGui::IsItemFocused() && !hovered;
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    const float sc = motion::pressScale(ImGui::GetItemID(), down, motion::PRESS_SCALE_ICON);
    ImVec2 q0, q1;
    scaleAbout(p0, p1, sc, q0, q1);
    if (quietError) {
        if (hovered || down) {
            dl->AddRectFilled(q0, q1, t.errorSoft, radius);
        }
    } else {
        ctl::finish(dl, q0, q1, (hovered || down) ? ctl::Finish::RaisedHover : ctl::Finish::Raised, radius);
        dl->AddRect(q0, q1, t.outline, radius, 0, hairlinePx());
    }
    const ImU32 ink = quietError && (hovered || down) ? t.error : t.mute;
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    drawIcon(dl, icon, c, iconSz * sc, ink);
    considerTip(ImGui::GetItemID(), hovered, keyFocus, p0, p1, label);
    return released;
}

void drawSpinner(ImDrawList* dl, ImVec2 c, float radius, ImU32 col) {
    // Continuous "working" indicator (§2.3), not an entrance.
    const float a = static_cast<float>(ImGui::GetTime()) * 6.0f;
    dl->PathArcTo(c, radius, a, a + 4.2f, 18);
    dl->PathStroke(col, 0, 1.6f);
}

void drawTip(ImDrawList* dl, ImFont* font, ImVec2 win) {
    using namespace upix::ui;
    if (!gTip.show || !gTip.text) return;
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(font, geist::type::LABEL, gTip.text);
    const float w = ts.x + 14.0f;
    const float h = std::max(22.0f, ts.y + 8.0f);
    float x = (gTip.a0.x + gTip.a1.x) * 0.5f - w * 0.5f;
    float y = gTip.a0.y - 6.0f - h;
    if (y < 8.0f) y = gTip.a1.y + 6.0f;
    x = std::clamp(x, 8.0f, std::max(8.0f, win.x - w - 8.0f));
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + h);
    ctl::finish(dl, p0, p1, ctl::Finish::Menu, geist::radius::MD);
    drawTextCentered(dl, font, geist::type::LABEL, p0, p1, t.ink, gTip.text);
}

float textTop(float boxY, float boxH, float textH) {
    return boxY + (boxH - textH) * 0.5f;
}

float drawSceneChip(ImDrawList* dl, ImFont* font, float x, float rowY, const std::string& scene) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const char* label = sceneLabel(scene);
    const ImVec2 ts = calcText(font, geist::type::LABEL, label);
    const float w = 24.0f + ts.x;
    const float h = 20.0f;
    const float y = rowY + (kRowH - h) * 0.5f;
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + h);
    dl->AddRectFilled(p0, p1, t.container, geist::radius::SM);
    dl->AddRect(p0, p1, t.outline, geist::radius::SM, 0, hairlinePx());
    const float cy = (p0.y + p1.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(p0.x + 9.5f, cy), 2.5f, sceneDot(scene));
    drawText(dl, font, geist::type::LABEL, ImVec2(p0.x + 17.0f, textTop(p0.y, h, ts.y)), t.body, label);
    return w;
}

float drawScaleChip(ImDrawList* dl, ImFont* font, float x, float rowY, int scale) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const std::string label = std::format("{}×", scale);
    const ImVec2 ts = calcText(font, geist::type::LABEL, label.c_str());
    const float w = 12.0f + ts.x;
    const float h = 20.0f;
    const float y = rowY + (kRowH - h) * 0.5f;
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + h);
    dl->AddRectFilled(p0, p1, t.container, geist::radius::SM);
    dl->AddRect(p0, p1, t.outline, geist::radius::SM, 0, hairlinePx());
    drawTextCentered(dl, font, geist::type::LABEL, p0, p1, t.mute, label.c_str());
    return w;
}

void drawSectionHead(ImDrawList* dl, ImFont* sans, ImFont* mono, float x, float y, float w,
                     const char* title, size_t count) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const ImVec2 ts = calcText(sans, geist::type::BODY_SMALL, title);
    drawText(dl, sans, geist::type::BODY_SMALL, ImVec2(x, textTop(y, kSectionHeadH, ts.y)), t.mute, title);
    const std::string cnt = std::format("{}", count);
    const ImVec2 cs = calcText(mono, geist::type::BODY_SMALL, cnt.c_str());
    const float cx = x + ts.x + 10.0f;
    drawText(dl, mono, geist::type::BODY_SMALL, ImVec2(cx, textTop(y, kSectionHeadH, cs.y)), t.faint, cnt.c_str());
    const float lineX = cx + cs.x + 10.0f;
    const float lineY = std::round(y + kSectionHeadH * 0.5f);
    if (lineX < x + w - 8.0f) {
        dl->AddRectFilled(snapPt(lineX, lineY), snapPt(x + w, lineY + hairlinePx()), t.outline);
    }
}

void drawEmptyCard(ImDrawList* dl, ImFont* sans, float x, float y, float w, const char* title, const char* hint) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + kEmptyH);
    ctl::finish(dl, p0, p1, ctl::Finish::Card, geist::radius::LG);
    const float cx = (p0.x + p1.x) * 0.5f;
    drawIcon(dl, "layers", ImVec2(cx, p0.y + 36.0f), 20.0f, t.mute);
    const ImVec2 ts = calcText(sans, geist::type::BODY, title);
    drawText(dl, sans, geist::type::BODY, ImVec2(cx - ts.x * 0.5f, p0.y + 52.0f), t.ink, title);
    const ImVec2 hs = calcText(sans, geist::type::BODY_SMALL, hint);
    drawText(dl, sans, geist::type::BODY_SMALL, ImVec2(cx - hs.x * 0.5f, p0.y + 74.0f), t.mute, hint);
}

struct RowRef {
    upix::gui::ModelItem item;
    bool remote = false;
};

struct GroupRef {
    std::string title;
    std::vector<RowRef> rows;
};

std::vector<GroupRef> filterGroups(const std::vector<upix::gui::ModelFamily>& groups, const std::string& sceneFilter,
                                   bool remote) {
    std::vector<GroupRef> out;
    for (const auto& g : groups) {
        GroupRef fg;
        fg.title = g.label;
        for (const auto& it : g.items) {
            if (!sceneMatches(sceneFilter, it.scene)) continue;
            fg.rows.push_back(RowRef{it, remote});
        }
        if (!fg.rows.empty()) out.push_back(std::move(fg));
    }
    return out;
}

size_t countRows(const std::vector<GroupRef>& groups) {
    size_t n = 0;
    for (const auto& g : groups) n += g.rows.size();
    return n;
}

void drawSceneFilter(ImDrawList* dl, ImFont* font, ImVec2 pos, std::string& current) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const float trackW = 210.0f;
    const float trackH = kToolbarH;
    ImVec2 t0 = snapPt(pos);
    ImVec2 t1 = snapPt(pos.x + trackW, pos.y + trackH);
    dl->AddRectFilled(t0, t1, t.recessed, 8.0f);
    dl->AddRect(t0, t1, t.outline, 8.0f, 0, hairlinePx());

    const SegmentOption opts[5] = {
        {"全部", "all"},
        {"动漫", "动漫"},
        {"照片", "照片"},
        {"综合", "综合"},
        {"修复", "修复"},
    };
    int active = 0;
    for (int i = 0; i < 5; ++i) {
        if (current == opts[i].value) {
            active = i;
            break;
        }
    }
    constexpr float pad = 2.0f;
    const float segW = (trackW - pad * 2.0f) / 5.0f;
    const float segH = trackH - pad * 2.0f;
    const float pillX = ctl::selectionSpring(ImGui::GetID("##scene_pill"), pad + static_cast<float>(active) * segW);
    ImVec2 pill0 = snapPt(t0.x + pillX, t0.y + pad);
    ImVec2 pill1 = snapPt(t0.x + pillX + segW, t0.y + pad + segH);
    ctl::finish(dl, pill0, pill1, ctl::Finish::Pill, geist::radius::MD);

    for (int i = 0; i < 5; ++i) {
        ImVec2 s0 = snapPt(t0.x + pad + static_cast<float>(i) * segW, t0.y + pad);
        ImVec2 s1 = snapPt(s0.x + segW, s0.y + segH);
        ImGui::SetCursorScreenPos(s0);
        ImGui::PushID(i);
        const bool released = ImGui::InvisibleButton("##seg", ImVec2(s1.x - s0.x, segH));
        const bool hovered = ImGui::IsItemHovered();
        const bool down = ImGui::IsItemActive();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (released && current != opts[i].value) current = opts[i].value;
        const float sc = motion::pressScale(ImGui::GetItemID(), down, motion::PRESS_SCALE);
        const float fontPx = 12.0f * sc;
        const bool selected = i == active;
        drawTextCentered(dl, font, fontPx, s0, s1, selected ? t.ink : t.body, opts[i].label);
        ImGui::PopID();
    }
}

void drawModelRow(ImDrawList* dl, ImFont* sans, ImFont* mono, ImVec2 c0, ImVec2 c1, float ry, const RowRef& row,
                  bool last, const upix::gui::Snapshot& snap, upix::gui::AppState& app) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const auto& m = row.item;
    const bool current = !row.remote && m.name == snap.model;
    const bool downloading = row.remote && snap.downloading == m.name;
    const ImVec2 row0(c0.x, ry);
    const ImVec2 row1(c1.x, ry + kRowH);
    const bool hovered = ctl::mouseIn(row0, row1);

    ImU32 wash = 0;
    if (current) wash = t.accentSoft;
    else if (hovered) wash = t.high;
    if (wash) {
        const ImDrawFlags corners = last ? ImDrawFlags_RoundCornersBottom : 0;
        dl->AddRectFilled(row0, row1, wash, last ? geist::radius::LG : 0.0f, corners);
    }
    dl->AddRectFilled(snapPt(c0.x, ry), snapPt(c1.x, ry + hairlinePx()), t.outline);

    const float left = c0.x + kRowPadX;
    const float right = c1.x - kRowPadX;
    const float cy = ry + kRowH * 0.5f;

    // Fixed 16px mark slot so a zap appearing does not shove the name.
    if (current && snap.daemon.running) {
        drawIcon(dl, "zap", ImVec2(left + 8.0f, cy), 11.5f, t.accent);
    }

    const char* name = m.short_name.empty() ? m.name.c_str() : m.short_name.c_str();
    const ImVec2 nameSz = calcText(mono, geist::type::BODY, name);
    float nameX = left + 16.0f + 8.0f;

    const float sceneW = 24.0f + calcText(sans, geist::type::LABEL, sceneLabel(m.scene)).x;
    const std::string scaleLabel = std::format("{}×", m.scale);
    const float scaleW = 12.0f + calcText(mono, geist::type::LABEL, scaleLabel.c_str()).x;
    const float starW = m.recommended ? 12.0f + 6.0f : 0.0f;
    const ImVec2 sizeSz = calcText(mono, geist::type::MONO, m.size.c_str());

    // sizeRight is the right edge of the size text. A download replaces the
    // 26px action with spinner + bar + percent, still ending on the row pad.
    std::string pct;
    float sizeRight = right - 26.0f - 12.0f;
    if (downloading) {
        pct = std::format("{}%", std::clamp(snap.download_pct, 0, 100));
        const float pctW = calcText(mono, geist::type::LABEL, pct.c_str()).x;
        sizeRight = right - (12.0f + 14.0f + 8.0f + 72.0f + 8.0f + pctW);
    }

    const float chipsW = sceneW + 8.0f + scaleW;
    float nameMax = sizeRight - sizeSz.x - 12.0f - chipsW - 12.0f - starW - nameX;
    if (nameMax < 24.0f) nameMax = 24.0f;
    const float nameDrawW = std::min(nameSz.x, nameMax);

    const float nameY = textTop(ry, kRowH, nameSz.y);
    dl->PushClipRect(ImVec2(nameX, ry), ImVec2(nameX + nameDrawW, ry + kRowH), true);
    drawText(dl, mono, geist::type::BODY, ImVec2(nameX, nameY), t.ink, name);
    dl->PopClipRect();

    float cursor = nameX + nameDrawW;
    if (m.recommended) {
        cursor += 6.0f;
        drawIcon(dl, "star", ImVec2(cursor + 6.0f, cy), 11.0f, t.warning);
        cursor += 12.0f;
    }
    cursor += 12.0f;
    cursor += drawSceneChip(dl, sans, cursor, ry, m.scene);
    cursor += 8.0f;
    drawScaleChip(dl, mono, cursor, ry, m.scale);

    const float sizeX = sizeRight - sizeSz.x;
    drawText(dl, mono, geist::type::MONO, ImVec2(sizeX, textTop(ry, kRowH, sizeSz.y)), t.mute, m.size.c_str());

    ImGui::PushID(row.remote ? "r" : "i");
    ImGui::PushID(m.name.c_str());
    if (!row.remote) {
        const ImVec2 bpos(right - 26.0f, ry + (kRowH - 26.0f) * 0.5f);
        if (iconAction(dl, "##del", bpos, 26.0f, 13.5f, geist::radius::MD, "trash", "删除这款模型", true)) {
            app.removeModel(m.name);
        }
    } else if (downloading) {
        float x = sizeRight + 12.0f;
        drawSpinner(dl, ImVec2(x + 7.0f, cy), 6.0f, t.accent);
        x += 14.0f + 8.0f;
        const float barW = 72.0f;
        const float barH = 2.0f;
        const float barY = cy - barH * 0.5f;
        const float frac = snap.download_pct < 0 ? 0.0f : std::clamp(snap.download_pct / 100.0f, 0.0f, 1.0f);
        // The shared spring is born near 1px, so the first paint of a download
        // that is already underway would grow in from empty — an entrance.
        // Catch the follower up once; later changes still take one step a frame.
        static std::unordered_set<ImGuiID> barBorn;
        const ImGuiID barId = ImGui::GetID("##dlbar");
        float fillW = ctl::progressSpring(barId, frac, barW);
        if (barBorn.insert(barId).second) {
            for (int i = 0; i < 48 && fillW + 0.75f < frac * barW; ++i) {
                fillW = ctl::progressSpring(barId, frac, barW);
            }
        }
        fillW = std::clamp(fillW, 0.0f, barW);
        dl->AddRectFilled(snapPt(x, barY), snapPt(x + barW, barY + barH), t.outline, 1.0f);
        if (fillW > 0.5f) {
            dl->AddRectFilled(snapPt(x, barY), snapPt(x + fillW, barY + barH), t.accent, 1.0f);
        }
        x += barW + 8.0f;
        const ImVec2 pctSz = calcText(mono, geist::type::LABEL, pct.c_str());
        drawText(dl, mono, geist::type::LABEL, ImVec2(x, textTop(ry, kRowH, pctSz.y)), t.accent, pct.c_str());
    } else {
        const ImVec2 bpos(right - 26.0f, ry + (kRowH - 26.0f) * 0.5f);
        if (iconAction(dl, "##dl", bpos, 26.0f, 13.0f, geist::radius::MD, "download", "下载这款模型", false)) {
            app.downloadModel(m.name);
        }
    }
    ImGui::PopID();
    ImGui::PopID();
}

float drawGroups(ImDrawList* dl, ImFont* sans, ImFont* mono, float x, float y, float w,
                 const std::vector<GroupRef>& groups, const char* emptyTitle, const char* emptyHint,
                 const upix::gui::Snapshot& snap, upix::gui::AppState& app) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    if (groups.empty()) {
        drawEmptyCard(dl, sans, x, y, w, emptyTitle, emptyHint);
        return y + kEmptyH;
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (gi) y += kCardGap;
        const auto& g = groups[gi];
        const float cardH = kCardHeadH + static_cast<float>(g.rows.size()) * kRowH;
        ImVec2 c0 = snapPt(x, y);
        ImVec2 c1 = snapPt(x + w, y + cardH);
        ctl::finish(dl, c0, c1, ctl::Finish::Card, geist::radius::LG);

        const std::string summary = std::format("{} 款", g.rows.size());
        const ImVec2 ss = calcText(sans, geist::type::LABEL, summary.c_str());
        const float countX = c1.x - kRowPadX - ss.x;
        drawText(dl, sans, geist::type::LABEL, ImVec2(countX, textTop(c0.y, kCardHeadH, ss.y)), t.mute, summary.c_str());

        const float titleMax = countX - 12.0f - (c0.x + kRowPadX);
        const ImVec2 ts = calcText(sans, geist::type::BODY, g.title.c_str());
        dl->PushClipRect(ImVec2(c0.x + kRowPadX, c0.y), ImVec2(c0.x + kRowPadX + std::max(24.0f, titleMax), c0.y + kCardHeadH), true);
        drawText(dl, sans, geist::type::BODY, ImVec2(c0.x + kRowPadX, textTop(c0.y, kCardHeadH, ts.y)), t.ink, g.title.c_str());
        dl->PopClipRect();

        float ry = c0.y + kCardHeadH;
        for (size_t i = 0; i < g.rows.size(); ++i) {
            drawModelRow(dl, sans, mono, c0, c1, ry, g.rows[i], i + 1 == g.rows.size(), snap, app);
            ry += kRowH;
        }
        y += cardH;
    }
    return y;
}

} // namespace

void renderModelsPage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                      SDL_Window* window, DialogBridge* dlgImport) {
    using namespace upix::ui;
    const auto& t = geist::tokens();

    const float railW = 76.0f;
    const float availW = winSize.x - railW;
    const float colW = std::min(kColMax, availW - 48.0f);
    const float colX = railW + (availW - colW) * 0.5f;

    dl->AddRectFilled(ImVec2(railW, 0.0f), winSize, t.bg);

    const ImGuiIO& io = ImGui::GetIO();
    const bool overPage = io.MousePos.x >= railW && io.MousePos.x < winSize.x && io.MousePos.y >= 0.0f &&
                          io.MousePos.y < winSize.y;
    const float maxScroll = std::max(0.0f, gContentH - winSize.y);
    if (overPage && io.MouseWheel != 0.0f) gScroll -= io.MouseWheel * 48.0f;
    gScroll = std::clamp(gScroll, 0.0f, maxScroll);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) gTip.dismissed = true;
    gTip.show = false;
    gTipSeen = false;

    dl->PushClipRect(ImVec2(railW, 0.0f), winSize, true);

    float y = kPagePad - gScroll;

    // Top bar: search (visual only — no query is stored) + scene pill + icon actions.
    {
        ImVec2 s0 = snapPt(colX, y);
        ImVec2 s1 = snapPt(colX + 220.0f, y + kToolbarH);
        dl->AddRectFilled(s0, s1, t.recessed, 8.0f);
        dl->AddRect(s0, s1, t.outline, 8.0f, 0, hairlinePx());
        drawIcon(dl, "search", ImVec2(s0.x + 17.0f, s0.y + kToolbarH * 0.5f), 14.0f, t.mute);
        const char* ph = "搜索…";
        const ImVec2 ps = calcText(ui.fontSans, 12.0f, ph);
        drawText(dl, ui.fontSans, 12.0f, ImVec2(s0.x + 32.0f, textTop(s0.y, kToolbarH, ps.y)), t.faint, ph);

        drawSceneFilter(dl, ui.fontSans, ImVec2(s1.x + 12.0f, y), ui.modelsSceneFilter);

        const float btn = 32.0f;
        const float btnY = y + (kToolbarH - btn) * 0.5f;
        const float xRefresh = colX + colW - btn;
        const float xImport = xRefresh - 6.0f - btn;
        const float xFolder = xImport - 6.0f - btn;
        if (iconAction(dl, "##open_mdir", ImVec2(xFolder, btnY), btn, 14.0f, 7.0f, "folder", "打开模型目录", false)) {
            SDL_OpenURL(("file://" + snap.models_dir).c_str());
        }
        if (iconAction(dl, "##import_mod", ImVec2(xImport, btnY), btn, 14.0f, 7.0f, "download", "导入本地模型", false)) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgImport, window, nullptr, 0, nullptr, true);
        }
        if (iconAction(dl, "##refresh_mod", ImVec2(xRefresh, btnY), btn, 14.0f, 7.0f, "refresh", "重新扫描", false)) {
            app.refreshModels();
        }
    }
    y += kToolbarH + kAfterToolbar;

    // Filter after the segmented control so a click updates the list this frame.
    const auto installed = filterGroups(snap.installed_groups, ui.modelsSceneFilter, false);
    const auto remote = filterGroups(snap.remote_groups, ui.modelsSceneFilter, true);

    drawSectionHead(dl, ui.fontSans, ui.fontMono, colX, y, colW, "已安装", countRows(installed));
    y += kSectionHeadH + kHeadToCard;
    y = drawGroups(dl, ui.fontSans, ui.fontMono, colX, y, colW, installed, "还没有已安装的模型",
                   "到下方「可下载」区一键下载，或用右上角按钮导入本地 .param / .bin", snap, app);

    y += kSectionGap;
    drawSectionHead(dl, ui.fontSans, ui.fontMono, colX, y, colW, "可下载", countRows(remote));
    y += kSectionHeadH + kHeadToCard;
    y = drawGroups(dl, ui.fontSans, ui.fontMono, colX, y, colW, remote, "没有匹配的可下载模型",
                   "换个关键词，或切换场景筛选试试", snap, app);

    dl->PopClipRect();
    drawTip(dl, ui.fontSans, winSize);

    // Content height includes a 24px tail so the last card can scroll clear of the edge.
    gContentH = (y + gScroll) + kPagePad;
    if (!gTipSeen) {
        gTip.hot = 0;
        gTip.dismissed = false;
    }
}

} // namespace upix::views
