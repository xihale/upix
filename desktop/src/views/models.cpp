#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

namespace upix::views {
namespace {

constexpr float kRailW = 76.0f;
constexpr float kPagePad = 32.0f;
constexpr float kColMax = 720.0f;
constexpr float kToolbarH = 32.0f;
constexpr float kAfterToolbar = 16.0f;
constexpr float kBannerGap = 16.0f;
constexpr float kSectionGap = 24.0f;
constexpr float kSectionHeadH = 18.0f;
constexpr float kHeadToCard = 12.0f;
constexpr float kCardGap = 16.0f;
constexpr float kCardHeadH = 36.0f;
constexpr float kRowH = 44.0f;
constexpr float kRowPadX = 16.0f;
constexpr float kEmptyH = 104.0f;

float gScroll = 0.0f;
float gContentH = 0.0f;

std::string toLowerTrimmed(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    s = s.substr(b, e - b);
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return s;
}

std::string toLowerAscii(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x80) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

bool sceneMatches(const std::string& filter, const std::string& scene) {
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

bool queryMatches(const std::string& qLower, const upix::gui::ModelItem& m, const std::string& family) {
    if (qLower.empty()) return true;
    char scBuf[32] = {};
    const char* scLbl = upix::ui::ctl::sceneLabel(m.scene.c_str(), scBuf, sizeof(scBuf));
    if (toLowerAscii(m.name).find(qLower) != std::string::npos) return true;
    if (!m.short_name.empty() && toLowerAscii(m.short_name).find(qLower) != std::string::npos) return true;
    if (!family.empty() && toLowerAscii(family).find(qLower) != std::string::npos) return true;
    if (!m.scene.empty() && toLowerAscii(m.scene).find(qLower) != std::string::npos) return true;
    if (scLbl && scLbl[0] && toLowerAscii(scLbl).find(qLower) != std::string::npos) return true;
    return false;
}

bool isMissingSize(const std::string& s) {
    return s.empty() || s == "0.00 MB" || s == "0 MB" || s == "0 B";
}

float textTop(float boxY, float boxH, float textH) {
    return boxY + (boxH - textH) * 0.5f;
}

float drawSceneChip(ImDrawList* dl, ImFont* font, float x, float rowY, const std::string& scene) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    char scBuf[32] = {};
    const char* label = ctl::sceneLabel(scene.c_str(), scBuf, sizeof(scBuf));
    const ImVec2 ts = calcText(font, geist::type::LABEL, label);
    const float w = 24.0f + ts.x;
    const float h = 20.0f;
    const float y = rowY + (kRowH - h) * 0.5f;
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + h);
    dl->AddRectFilled(p0, p1, t.tagBg, geist::radius::SM);
    dl->AddRect(p0, p1, t.outline, geist::radius::SM, 0, hairlinePx());
    const float cy = (p0.y + p1.y) * 0.5f;
    dl->AddCircleFilled(ImVec2(p0.x + 9.5f, cy), 2.5f, ctl::sceneDotColor(scene.c_str()));
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
    const float lineY = snapPx(y + kSectionHeadH * 0.5f);
    if (lineX < x + w - 8.0f) {
        ctl::hHairline(dl, lineX, x + w, lineY, t.outline);
    }
}

bool drawEmptyCard(ImDrawList* dl, ImFont* sans, ImGuiID btnId, float x, float y, float w,
                   const char* title, const char* hint, bool withClearFilter) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const float cardH = withClearFilter ? 128.0f : kEmptyH;
    ImVec2 p0 = snapPt(x, y);
    ImVec2 p1 = snapPt(x + w, y + cardH);
    ctl::groupCard(dl, p0, p1);
    const float cx = (p0.x + p1.x) * 0.5f;
    drawIcon(dl, "layers", ImVec2(cx, p0.y + 30.0f), 18.0f, t.mute);
    const ImVec2 ts = calcText(sans, geist::type::BODY, title);
    drawText(dl, sans, geist::type::BODY, ImVec2(cx - ts.x * 0.5f, p0.y + 46.0f), t.ink, title);
    if (hint && hint[0]) {
        const ImVec2 hs = calcText(sans, geist::type::BODY_SMALL, hint);
        drawText(dl, sans, geist::type::BODY_SMALL, ImVec2(cx - hs.x * 0.5f, p0.y + 68.0f), t.mute, hint);
    }
    if (withClearFilter) {
        const char* btnLabel = "清除筛选";
        const ImVec2 bs = calcText(sans, ctl::geom::LABEL, btnLabel);
        const float bw = bs.x + 2.0f * ctl::geom::GHOST_PX;
        return ctl::ghostButton(dl, btnId, btnLabel, sans,
                                snapPt(cx - bw * 0.5f, p0.y + 90.0f), bw, true);
    }
    return false;
}

struct RowRef {
    upix::gui::ModelItem item;
    bool remote = false;
};

struct GroupRef {
    std::string title;
    std::vector<RowRef> rows;
};

std::vector<GroupRef> filterGroups(const std::vector<upix::gui::ModelFamily>& groups,
                                   const std::string& sceneFilter,
                                   const std::string& qLower,
                                   bool remote,
                                   const std::unordered_set<std::string>* excludeNames = nullptr) {
    std::vector<GroupRef> out;
    for (const auto& g : groups) {
        GroupRef fg;
        fg.title = g.label;
        for (const auto& it : g.items) {
            if (excludeNames && excludeNames->count(it.name)) continue;
            if (!sceneMatches(sceneFilter, it.scene)) continue;
            if (!queryMatches(qLower, it, g.label)) continue;
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

void drawModelRow(ImDrawList* dl, ImFont* sans, ImFont* mono, ImVec2 c0, ImVec2 c1, float ry, const RowRef& row,
                  bool last, const upix::gui::Snapshot& snap, upix::gui::AppState& app) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const auto& m = row.item;
    const bool current = !row.remote && m.name == snap.model;
    const bool downloading = row.remote && snap.downloading == m.name;
    const ImVec2 row0(c0.x, ry);
    const ImVec2 row1(c1.x, ry + kRowH);

    ImGui::PushID(row.remote ? "r" : "i");
    ImGui::PushID(m.name.c_str());

    const float left = c0.x + kRowPadX;
    const float right = c1.x - kRowPadX;
    const float cy = ry + kRowH * 0.5f;

    // Reserve action rect first so clicking trash/download does not trigger row selection.
    const float btnBox = 26.0f;
    const ImVec2 bpos = snapPt(right - btnBox, ry + (kRowH - btnBox) * 0.5f);
    const bool rowClickable = !row.remote;
    bool rowHovered = false;
    bool rowClicked = false;
    if (rowClickable) {
        const ImVec2 hitP0(row0.x + 1.0f, row0.y + 1.0f);
        const ImVec2 hitP1(bpos.x - 6.0f, row1.y - 1.0f);
        ImGui::SetCursorScreenPos(hitP0);
        rowClicked = ImGui::InvisibleButton("##select_row", ImVec2(std::max(1.0f, hitP1.x - hitP0.x), std::max(1.0f, hitP1.y - hitP0.y)));
        rowHovered = ImGui::IsItemHovered();
        if (rowHovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    } else {
        rowHovered = ctl::mouseIn(row0, row1);
    }

    const bool anyHover = rowHovered || ctl::mouseIn(row0, row1);
    if (current) {
        const ImDrawFlags corners = last ? ImDrawFlags_RoundCornersBottom : 0;
        dl->AddRectFilled(ImVec2(row0.x + 1.0f, row0.y), ImVec2(row1.x - 1.0f, last ? row1.y - 1.0f : row1.y),
                          t.accentSoft, last ? (ctl::geom::CARD_R - 1.0f) : 0.0f, corners);
    } else if (anyHover) {
        const ImDrawFlags corners = last ? ImDrawFlags_RoundCornersBottom : 0;
        dl->AddRectFilled(ImVec2(row0.x + 1.0f, row0.y), ImVec2(row1.x - 1.0f, last ? row1.y - 1.0f : row1.y),
                          t.high, last ? (ctl::geom::CARD_R - 1.0f) : 0.0f, corners);
    }
    ctl::hHairline(dl, c0.x, c1.x, ry, t.outline);

    if (rowClicked) {
        app.setModel(m.name);
    }

    // Fixed 16px warm-engine mark slot on installed active row.
    if (current) {
        const ImVec2 markC(left + 8.0f, cy);
        if (snap.daemon.loading) {
            if (motion::reduceMotion()) {
                dl->PathClear();
                dl->PathArcTo(markC, 5.5f, 0.0f, 1.9f, 16);
                dl->PathStroke(t.accent, 0, 1.6f);
            } else {
                ctl::busyArc(dl, markC, 5.5f, t.accent, 1.6f);
            }
        } else if (snap.daemon.running) {
            drawIcon(dl, "zap", markC, 12.0f, t.accent);
        }
    }

    const char* name = m.short_name.empty() ? m.name.c_str() : m.short_name.c_str();
    const ImVec2 nameSz = calcText(mono, geist::type::BODY, name);
    const float nameX = left + 16.0f + 8.0f;

    char scBuf[32] = {};
    const char* scLbl = ctl::sceneLabel(m.scene.c_str(), scBuf, sizeof(scBuf));
    const float sceneW = 24.0f + calcText(sans, geist::type::LABEL, scLbl).x;
    const std::string scaleLabel = std::format("{}×", m.scale);
    const float scaleW = 12.0f + calcText(mono, geist::type::LABEL, scaleLabel.c_str()).x;
    const float starW = m.recommended ? (6.0f + 12.0f) : 0.0f;

    const std::string sizeStr = isMissingSize(m.size) ? std::string("—") : m.size;
    const ImVec2 sizeSz = calcText(mono, geist::type::MONO, sizeStr.c_str());

    std::string pct;
    float sizeRight = right - btnBox - 12.0f;
    if (downloading) {
        pct = std::format("{}%", std::clamp(snap.download_pct, 0, 100));
        const float pctW = calcText(mono, geist::type::LABEL, pct.c_str()).x;
        sizeRight = right - (12.0f + 14.0f + 8.0f + 72.0f + 8.0f + pctW);
    }

    const float chipsW = sceneW + 8.0f + scaleW;
    const float nameMax = std::max(24.0f, sizeRight - sizeSz.x - 14.0f - chipsW - 12.0f - starW - nameX);
    const std::string fittedName = fitTextEllipsis(mono, geist::type::BODY, name, nameMax);
    const float nameDrawW = calcText(mono, geist::type::BODY, fittedName.c_str()).x;

    drawText(dl, mono, geist::type::BODY, ImVec2(nameX, textTop(ry, kRowH, nameSz.y)),
             current ? t.ink : t.ink, fittedName.c_str());

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
    drawText(dl, mono, geist::type::MONO, ImVec2(sizeX, textTop(ry, kRowH, sizeSz.y)), t.mute, sizeStr.c_str());

    if (!row.remote) {
        if (ctl::iconAction(dl, ImGui::GetID("del"), bpos, btnBox, "trash", "删除模型", sans, 13.5f,
                            geist::radius::MD, true)) {
            app.removeModel(m.name);
        }
    } else if (downloading) {
        float x = sizeRight + 12.0f;
        const ImVec2 spinC(x + 7.0f, cy);
        if (motion::reduceMotion()) {
            dl->PathClear();
            dl->PathArcTo(spinC, 5.5f, 0.0f, 1.9f, 16);
            dl->PathStroke(t.accent, 0, 1.6f);
        } else {
            ctl::busyArc(dl, spinC, 5.5f, t.accent, 1.6f);
        }
        x += 14.0f + 8.0f;
        const float barW = 72.0f;
        const float frac = snap.download_pct < 0 ? 0.0f : std::clamp(snap.download_pct / 100.0f, 0.0f, 1.0f);
        ctl::progressBar(dl, ImGui::GetID("dlbar"), ImVec2(x, cy - ctl::geom::PROGRESS_H * 0.5f), barW, frac,
                         ctl::geom::PROGRESS_H);
        x += barW + 8.0f;
        const ImVec2 pctSz = calcText(mono, geist::type::LABEL, pct.c_str());
        drawText(dl, mono, geist::type::LABEL, ImVec2(x, textTop(ry, kRowH, pctSz.y)), t.accent, pct.c_str());
    } else {
        const bool busyDownloading = !snap.downloading.empty();
        if (ctl::iconAction(dl, ImGui::GetID("dl"), bpos, btnBox, "download", "下载模型", sans, 13.0f,
                            geist::radius::MD, !busyDownloading)) {
            app.downloadModel(m.name);
        }
    }

    ImGui::PopID();
    ImGui::PopID();
}

float drawGroups(ImDrawList* dl, ImFont* sans, ImFont* mono, float x, float y, float w,
                 const std::vector<GroupRef>& groups, const upix::gui::Snapshot& snap, upix::gui::AppState& app) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        if (gi) y += kCardGap;
        const auto& g = groups[gi];
        const float cardH = kCardHeadH + static_cast<float>(g.rows.size()) * kRowH;
        ImVec2 c0 = snapPt(x, y);
        ImVec2 c1 = snapPt(x + w, y + cardH);
        ctl::groupCard(dl, c0, c1);

        const std::string summary = std::format("{} 款", g.rows.size());
        const ImVec2 ss = calcText(sans, geist::type::LABEL, summary.c_str());
        const float countX = c1.x - kRowPadX - ss.x;
        drawText(dl, sans, geist::type::LABEL, ImVec2(countX, textTop(c0.y, kCardHeadH, ss.y)), t.mute, summary.c_str());

        const float titleMax = std::max(24.0f, countX - 12.0f - (c0.x + kRowPadX));
        const ImVec2 ts = calcText(sans, geist::type::BODY, g.title.c_str());
        ellipsisText(dl, sans, geist::type::BODY, ImVec2(c0.x + kRowPadX, textTop(c0.y, kCardHeadH, ts.y)),
                     titleMax, t.ink, g.title.c_str());

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

    const float availW = std::max(0.0f, winSize.x - kRailW);
    const float colW = std::min(kColMax, std::max(320.0f, availW - 64.0f));
    const float colX = snapPx(kRailW + (availW - colW) * 0.5f);

    dl->AddRectFilled(ImVec2(kRailW, 0.0f), winSize, t.bg);

    const ImGuiIO& io = ImGui::GetIO();
    const bool overPage = io.MousePos.x >= kRailW && io.MousePos.x < winSize.x &&
                          io.MousePos.y >= 0.0f && io.MousePos.y < winSize.y;
    const float maxScroll = std::max(0.0f, gContentH - winSize.y);
    if (overPage && io.MouseWheel != 0.0f) {
        gScroll -= io.MouseWheel * 48.0f;
    }
    gScroll = std::clamp(gScroll, 0.0f, maxScroll);

    dl->PushClipRect(ImVec2(kRailW, 0.0f), winSize, true);

    float y = kPagePad - gScroll;

    // 1. Toolbar: searchBox + 5-way scene segmented + 3 right-aligned iconAction buttons.
    {
        const float btn = 32.0f;
        const float btnGap = 6.0f;
        const float rightW = btn * 3.0f + btnGap * 2.0f;
        const float segW = 220.0f;
        const float searchW = std::clamp(colW - segW - rightW - 24.0f, 140.0f, 248.0f);

        ctl::searchBox(dl, ImGui::GetID("models.search"), ImVec2(colX, y), searchW, kToolbarH,
                       ui.modelsSearch, sizeof(ui.modelsSearch), "搜索模型名称或场景…", ui.fontSans);

        const char* sceneLabels[5] = {"全部", "动漫", "照片", "综合", "修复"};
        const char* sceneValues[5] = {"all", "动漫", "照片", "综合", "修复"};
        int sceneIdx = 0;
        for (int i = 0; i < 5; ++i) {
            if (ui.modelsSceneFilter == sceneValues[i]) {
                sceneIdx = i;
                break;
            }
        }
        if (ctl::segmented(dl, ImGui::GetID("models.scene"),
                           ImVec2(colX + searchW + 12.0f, y + (kToolbarH - ctl::geom::SEG_H) * 0.5f),
                           ImVec2(segW, ctl::geom::SEG_H), sceneLabels, 5, sceneIdx, ui.fontSans, 12.0f)) {
            ui.modelsSceneFilter = sceneValues[sceneIdx];
        }

        const float btnY = y + (kToolbarH - btn) * 0.5f;
        const float xRefresh = colX + colW - btn;
        const float xImport = xRefresh - btnGap - btn;
        const float xFolder = xImport - btnGap - btn;

        if (ctl::iconAction(dl, ImGui::GetID("models.open_dir"), ImVec2(xFolder, btnY), btn,
                            "folder", "打开模型目录", ui.fontSans, 15.0f, geist::radius::MD)) {
            std::error_code ec;
            if (!snap.models_dir.empty()) {
                std::filesystem::create_directories(snap.models_dir, ec);
                SDL_OpenURL(("file://" + snap.models_dir).c_str());
            }
        }
        if (ctl::iconAction(dl, ImGui::GetID("models.import"), ImVec2(xImport, btnY), btn,
                            "download", "导入本地模型", ui.fontSans, 15.0f, geist::radius::MD)) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgImport, window, nullptr, 0, nullptr, true);
        }
        if (ctl::iconAction(dl, ImGui::GetID("models.refresh"), ImVec2(xRefresh, btnY), btn,
                            "refresh", "重新扫描", ui.fontSans, 15.0f, geist::radius::MD)) {
            app.refreshModels();
        }
    }
    y += kToolbarH + kAfterToolbar;

    // 2. Dismissible status banner when !snap.models_status.empty()
    if (!snap.models_status.empty()) {
        const float bannerH = 36.0f;
        const ImVec2 b0 = snapPt(colX, y);
        const ImVec2 b1 = snapPt(colX + colW, y + bannerH);
        ctl::finish(dl, b0, b1, ctl::Finish::Raised, 8.0f);
        dl->AddRect(b0, b1, t.outline, 8.0f, 0, hairlinePx());

        const float midY = (b0.y + b1.y) * 0.5f;
        dl->AddCircleFilled(ImVec2(b0.x + 14.0f, midY), 3.0f, t.accent);

        const float closeBox = 24.0f;
        const ImVec2 closePos = snapPt(b1.x - 6.0f - closeBox, midY - closeBox * 0.5f);
        const float msgX = b0.x + 26.0f;
        const float msgMaxW = std::max(40.0f, closePos.x - 10.0f - msgX);
        const ImVec2 msz = calcText(ui.fontSans, geist::type::BODY_SMALL, snap.models_status.c_str());
        ellipsisText(dl, ui.fontSans, geist::type::BODY_SMALL,
                     ImVec2(msgX, midY - msz.y * 0.5f), msgMaxW, t.body, snap.models_status.c_str());

        if (ctl::iconAction(dl, ImGui::GetID("models.clear_status"), closePos, closeBox,
                            "x", "关闭提示", ui.fontSans, 11.0f, geist::radius::MD)) {
            app.clearModelsStatus();
        }
        y += bannerH + kBannerGap;
    }

    // 3. Filter installed & remote groups live by search query & scene filter.
    std::unordered_set<std::string> installedNames;
    for (const auto& g : snap.installed_groups) {
        for (const auto& m : g.items) installedNames.insert(m.name);
    }

    const std::string qLower = toLowerTrimmed(ui.modelsSearch);
    const bool hasFilter = !qLower.empty() || (ui.modelsSceneFilter != "all" && !ui.modelsSceneFilter.empty());

    const auto installed = filterGroups(snap.installed_groups, ui.modelsSceneFilter, qLower, false, nullptr);
    const auto remote = filterGroups(snap.remote_groups, ui.modelsSceneFilter, qLower, true, &installedNames);
    const size_t installedCount = countRows(installed);
    const size_t remoteCount = countRows(remote);

    if (hasFilter && installedCount == 0 && remoteCount == 0) {
        if (drawEmptyCard(dl, ui.fontSans, ImGui::GetID("models.clear_all_empty"), colX, y, colW,
                          "没有匹配的模型", "尝试换个关键词或切换场景筛选", true)) {
            ui.modelsSearch[0] = '\0';
            ui.modelsSceneFilter = "all";
        }
        y += 128.0f;
    } else {
        drawSectionHead(dl, ui.fontSans, ui.fontMono, colX, y, colW, "已安装", installedCount);
        y += kSectionHeadH + kHeadToCard;
        if (installedCount == 0) {
            if (hasFilter) {
                if (drawEmptyCard(dl, ui.fontSans, ImGui::GetID("models.clear_inst_empty"), colX, y, colW,
                                  "没有匹配的模型", "已安装列表中没有符合当前筛选条件的模型", true)) {
                    ui.modelsSearch[0] = '\0';
                    ui.modelsSceneFilter = "all";
                }
                y += 128.0f;
            } else {
                drawEmptyCard(dl, ui.fontSans, ImGui::GetID("models.inst_empty"), colX, y, colW,
                              "尚未安装本地模型", "从下方列表下载推荐模型，或点击右上角导入", false);
                y += kEmptyH;
            }
        } else {
            y = drawGroups(dl, ui.fontSans, ui.fontMono, colX, y, colW, installed, snap, app);
        }

        y += kSectionGap;
        drawSectionHead(dl, ui.fontSans, ui.fontMono, colX, y, colW, "可下载", remoteCount);
        y += kSectionHeadH + kHeadToCard;
        if (remoteCount == 0) {
            if (hasFilter) {
                if (drawEmptyCard(dl, ui.fontSans, ImGui::GetID("models.clear_rem_empty"), colX, y, colW,
                                  "没有匹配的模型", "可下载列表中没有符合当前筛选条件的模型", true)) {
                    ui.modelsSearch[0] = '\0';
                    ui.modelsSceneFilter = "all";
                }
                y += 128.0f;
            } else {
                drawEmptyCard(dl, ui.fontSans, ImGui::GetID("models.rem_empty"), colX, y, colW,
                              "没有可下载的模型", "所有目录模型均已安装，或点击右上角重新扫描", false);
                y += kEmptyH;
            }
        } else {
            y = drawGroups(dl, ui.fontSans, ui.fontMono, colX, y, colW, remote, snap, app);
        }
    }

    dl->PopClipRect();

    gContentH = (y + gScroll) + kPagePad;
    if (gContentH > winSize.y && winSize.y > 24.0f) {
        const float trackH = winSize.y - 16.0f;
        const float thumbH = std::max(28.0f, trackH * (winSize.y / gContentH));
        const float frac = maxScroll > 0.0f ? (gScroll / maxScroll) : 0.0f;
        const float thumbY = 8.0f + frac * (trackH - thumbH);
        dl->AddRectFilled(snapPt(winSize.x - 6.0f, thumbY), snapPt(winSize.x - 2.0f, thumbY + thumbH),
                          withAlpha(t.faint, 0.45f), 2.0f);
    }
}

} // namespace upix::views
