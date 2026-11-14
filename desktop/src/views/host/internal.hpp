// views/host/internal.hpp — shared internal helpers and sub-renderers for the host view.
#pragma once

#include "views/common.hpp"
#include "ui/flex.hpp"

#include <array>
#include <string>
#include <vector>

namespace upix::views::host_detail {

inline constexpr float kPagePad = upix::ui::geist::space::XXXL;
inline constexpr float kColMax = 760.0f;

struct HeroSlots {
    upix::ui::flex::Container card;
    upix::ui::flex::Slot qr;
    upix::ui::flex::Slot fpLbl;
    std::array<upix::ui::flex::Slot, 4> chips{};
    upix::ui::flex::Slot copy;
    upix::ui::flex::Slot div1;
    upix::ui::flex::Slot ipLbl;
    upix::ui::flex::Container ipTrig;
    upix::ui::flex::Slot ipTrigDot;
    upix::ui::flex::Slot ipTrigText;
    upix::ui::flex::Slot ipTrigKind;
    upix::ui::flex::Slot ipTrigChev;
    upix::ui::flex::Slot div2;
    upix::ui::flex::Slot portLbl;
    upix::ui::flex::Slot port;
    upix::ui::flex::Slot statusDot;
    upix::ui::flex::Slot state;
    upix::ui::flex::Slot sub;
    upix::ui::flex::Slot cta;

    std::array<std::string, 4> groups{};
    std::string curIp;
    std::string subStatus;
    const char* stateText = "IDLE";
    const char* btnLabel = "启动服务";
    ImFont* subFont = nullptr;
    float subPx = 0.0f;
    bool running = false;
};

struct TrustedRowSlots {
    upix::ui::flex::Container row;
    upix::ui::flex::Slot dot;
    upix::ui::flex::Slot name;
    upix::ui::flex::Slot badge;
    upix::ui::flex::Slot fp;
    upix::ui::flex::Slot del;
    upix::ui::flex::Slot div;
    std::string fpText;
};

struct TrustedSlots {
    upix::ui::flex::Slot title;
    upix::ui::flex::Slot count;
    upix::ui::flex::Slot clear;
    upix::ui::flex::Slot headDiv;
    upix::ui::flex::Slot empty;
    upix::ui::flex::Slot emptyDiv;
    std::vector<TrustedRowSlots> rows;
    std::string countStr;
    ImGuiID clearId = 0;
};

struct LogEntry {
    std::string ts;
    std::string tag;
    std::string msg;
    std::string note;
    bool task = false;
};

struct LogRowSlots {
    upix::ui::flex::Slot time;
    upix::ui::flex::Slot badge;
    upix::ui::flex::Slot msg;
    upix::ui::flex::Slot note;
    LogEntry data;
};

struct LogSlots {
    upix::ui::flex::Slot sig;
    upix::ui::flex::Slot title;
    upix::ui::flex::Slot model;
    upix::ui::flex::Slot bar;
    upix::ui::flex::Slot pct;
    upix::ui::flex::Slot headDiv;
    upix::ui::flex::Container viewport;
    upix::ui::flex::Container list;
    std::vector<LogRowSlots> lines;
    std::string pctLabel;
    float frac = 0.0f;
    float scrollUsed = 0.0f;
    bool visible = false;
};

std::array<std::string, 4> fpGroups(const std::string& fp);
std::string dottedFp(const std::string& fp);
const char* ipKind(const std::string& ip);
float ctaWidth(ImFont* font, const char* label, bool withIcon);

void drawQr(ImDrawList* dl, ImVec2 well0, ImVec2 well1, const std::string& payload, bool live);

bool portInputField(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, int port, bool editable,
                    bool runningLocked, ImFont* mono, ImFont* sans, int& outPort);

void drawIpMenuPopup(UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     ImVec2 trig0, ImVec2 trig1, ImVec2 winSize);

HeroSlots buildHeroCard(upix::ui::flex::Container& page, float colW,
                        const upix::gui::Snapshot& snap,
                        ImFont* sansMed, ImFont* sans, ImFont* mono);
void drawHeroCard(ImDrawList* dl, const upix::ui::flex::Layout& fx, const HeroSlots& s,
                  UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                  ImFont* sansMed, ImFont* sans, ImFont* mono, bool modal,
                  ImVec2& ipTrig0, ImVec2& ipTrig1, bool& ipTrigDrawn);

TrustedSlots buildTrustedDevices(upix::ui::flex::Container& page, float colW,
                                   const upix::gui::Snapshot& snap,
                                   ImFont* sansMed, ImFont* sans, ImFont* mono);
void drawTrustedDevices(ImDrawList* dl, const upix::ui::flex::Layout& fx, const TrustedSlots& s,
                        const UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                        ImFont* sansMed, ImFont* sans, ImFont* mono,
                        float rowOutset, bool modal);

LogSlots buildActivityLog(upix::ui::flex::Container& page, float colW,
                          const upix::gui::Snapshot& snap,
                          ImFont* sansMed, ImFont* sans, ImFont* mono);
void drawActivityLog(ImDrawList* dl, const upix::ui::flex::Layout& fx, const LogSlots& s,
                     const UiViewState& ui, const upix::gui::Snapshot& snap,
                     ImFont* sansMed, ImFont* sans, ImFont* mono,
                     bool modal, bool& wheelConsumed);

} // namespace upix::views::host_detail
