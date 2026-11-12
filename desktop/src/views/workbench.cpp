#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <string>
#include <unordered_map>

namespace upix::views {
namespace {

// Same workaround as shell.cpp: motion::springTo's Spring1D boots at 1.0f and
// only honors defaultValue after 120 dormant frames, so a pill or a progress
// bar would animate in from the wrong place (an entrance). Seed at `target`.
// Params match ctl::selectionSpring {360,30}, ctl::progressSpring {180,26},
// and motion::PLACE for the switch knob.
float followSpring(ImGuiID id, float target, upix::ui::motion::SpringParams params) {
    struct Slot {
        upix::ui::motion::Spring1D s;
        int frame = -1;
        bool live = false;
    };
    static std::unordered_map<ImGuiID, Slot> store;
    Slot& slot = store[id];
    const int frame = ImGui::GetFrameCount();
    if (!slot.live || slot.frame + 120 < frame) {
        slot.s.value = target;
        slot.s.velocity = 0.0f;
        slot.live = true;
    }
    slot.frame = frame;
    return upix::ui::motion::advance(slot.s, target, params, ImGui::GetIO().DeltaTime);
}

float easeTo(ImGuiID id, float target, float durMs) {
    struct Slot {
        float v = 0.0f;
        int frame = -1;
        bool live = false;
    };
    static std::unordered_map<ImGuiID, Slot> store;
    Slot& slot = store[id];
    const int frame = ImGui::GetFrameCount();
    if (!slot.live || slot.frame + 120 < frame) {
        slot.v = target;
        slot.live = true;
    }
    slot.frame = frame;
    const float step = ImGui::GetIO().DeltaTime / (durMs / 1000.0f);
    if (slot.v < target) slot.v = std::min(target, slot.v + step);
    else slot.v = std::max(target, slot.v - step);
    return slot.v;
}

// Finish::Inset strokes its inner shadow and then fills the same rect, so the
// shadow never shows. Fill wellBg, then a 1px black rim inside the clip, then
// the hairline the spec asks for.
void drawInsetWell(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float radius) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    dl->AddRectFilled(p0, p1, t.wellBg, radius);
    dl->PushClipRect(p0, p1, true);
    dl->AddRect(ImVec2(p0.x + 1.0f, p0.y + 1.0f), ImVec2(p1.x - 1.0f, p1.y - 1.0f),
                hex(0x000000, 0x73), std::max(0.0f, radius - 1.0f), 0, 1.0f);
    dl->PopClipRect();
    dl->AddRect(p0, p1, t.outline, radius, 0, hairlinePx());
}

struct SegOpt {
    const char* label;
    const char* value;
};

// Track = recessed + hairline. Selected block = Pill that spring-slides.
// Item press 0.97 + 6% wash. Selected label promotes mute → ink over FAST.
bool segmented(ImDrawList* dl, const char* id, ImFont* font, ImVec2 pos, ImVec2 size,
               const SegOpt* opts, int count, std::string& current, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    pos = snapPt(pos);
    const ImVec2 track1(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, track1, t.recessed, 8.0f);
    dl->AddRect(pos, track1, t.outline, 8.0f, 0, hairlinePx());

    const float pad = 2.0f;
    const float segW = (size.x - pad * 2.0f) / static_cast<float>(count);
    const float segH = size.y - pad * 2.0f;

    int active = 0;
    for (int i = 0; i < count; ++i) {
        if (current == opts[i].value) active = i;
    }

    ImGui::PushID(id);
    const float targetX = pos.x + pad + static_cast<float>(active) * segW;
    const float pillX = followSpring(ImGui::GetID("pill"), targetX, motion::SpringParams{360.0f, 30.0f});
    ctl::finish(dl, snapPt(pillX, pos.y + pad), snapPt(pillX + segW, pos.y + pad + segH), ctl::Finish::Pill, 6.0f);

    bool changed = false;
    for (int i = 0; i < count; ++i) {
        const ImVec2 s0 = snapPt(pos.x + pad + static_cast<float>(i) * segW, pos.y + pad);
        const ImVec2 s1 = snapPt(pos.x + pad + static_cast<float>(i + 1) * segW, pos.y + pad + segH);
        ImGui::PushID(i);
        bool hov = false;
        bool held = false;
        if (!modal) {
            ImGui::SetCursorScreenPos(s0);
            ImGui::InvisibleButton("##seg", ImVec2(std::max(1.0f, s1.x - s0.x), std::max(1.0f, segH)));
            hov = ImGui::IsItemHovered();
            held = ImGui::IsItemActive();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked() && current != opts[i].value) {
                current = opts[i].value;
                active = i;
                changed = true;
            }
        }
        float sc = motion::pressScale(ImGui::GetID("press"), held, motion::PRESS_SCALE);
        if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
        if (hov || held) dl->AddRectFilled(s0, s1, hex(0xFFFFFF, 0x0F), 6.0f);
        const float promo = easeTo(ImGui::GetID("ink"), (i == active) ? 1.0f : 0.0f, motion::FAST);
        const ImU32 col = motion::mix(t.mute, t.ink, promo);
        const ImVec2 c((s0.x + s1.x) * 0.5f, (s0.y + s1.y) * 0.5f);
        const ImVec2 q0(c.x + (s0.x - c.x) * sc, c.y + (s0.y - c.y) * sc);
        const ImVec2 q1(c.x + (s1.x - c.x) * sc, c.y + (s1.y - c.y) * sc);
        drawTextCentered(dl, font, 12.0f * sc, q0, q1, col, opts[i].label);
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

// Thumb and fill follow the pointer 1:1. No spring, no throw on release.
// Quantiles 0/25/50/75/100 catch the pointer within 2.2% of the track
// (rust quality_slider). Track is a 2px hairline; fill is ink.
bool qualitySlider(ImDrawList* dl, const char* id, ImVec2 pos, float width, int& value, int minV, int maxV, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    pos = snapPt(pos);
    const float hitH = 18.0f;
    bool changed = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton(id, ImVec2(width, hitH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemActive()) {
            float frac = (ImGui::GetIO().MousePos.x - pos.x) / std::max(1.0f, width);
            frac = std::clamp(frac, 0.0f, 1.0f);
            const float qs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
            for (float q : qs) {
                if (std::fabs(frac - q) <= 0.022f) frac = q;
            }
            int next = static_cast<int>(std::lround(minV + frac * static_cast<float>(maxV - minV)));
            next = std::clamp(next, minV, maxV);
            if (next != value) {
                value = next;
                changed = true;
            }
        }
    }

    const float span = static_cast<float>(std::max(1, maxV - minV));
    const float tNorm = std::clamp(static_cast<float>(value - minV) / span, 0.0f, 1.0f);
    const float trackY = snapPx(pos.y + 8.0f);
    const float fillW = snapPx(tNorm * width);
    dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + width, trackY + 2.0f), t.outline, 1.0f);
    if (fillW > 0.5f) {
        dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + fillW, trackY + 2.0f), t.ink, 1.0f);
    }
    for (int i = 0; i < 5; ++i) {
        const float tx = snapPx(pos.x + width * (static_cast<float>(i) / 4.0f));
        dl->AddRectFilled(ImVec2(tx, snapPx(pos.y + 13.0f)), ImVec2(tx + hairlinePx(), snapPx(pos.y + 16.0f)), t.faint);
    }
    const float thumbX = std::clamp(pos.x + tNorm * width, pos.x + 6.5f, pos.x + width - 6.5f);
    const ImVec2 c = snapPt(thumbX, trackY + 1.0f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + 1.0f), 6.5f, hex(0x000000, 0x55), 24);
    dl->AddCircleFilled(c, 6.5f, t.bg, 24);
    dl->AddCircle(c, 6.5f, t.ink, 24, 1.5f);
    return changed;
}

// Off = recessed track + ink knob. On = ink track + onPrimary knob.
// Knob travels with motion::PLACE (ctl::switchKnob's spring). Track color eases BASE.
bool inkSwitch(ImDrawList* dl, const char* id, ImVec2 pos, bool& on, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    pos = snapPt(pos);
    const ImVec2 sz(44.0f, 26.0f);
    bool toggled = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(pos);
        if (ImGui::InvisibleButton(id, sz)) {
            on = !on;
            toggled = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PushID(id);
    const float k = followSpring(ImGui::GetID("knob"), on ? 1.0f : 0.0f, motion::PLACE);
    const float c = easeTo(ImGui::GetID("col"), on ? 1.0f : 0.0f, motion::BASE);
    ImGui::PopID();

    const ImU32 track = motion::mix(t.recessed, t.ink, c);
    const ImVec2 p1(pos.x + sz.x, pos.y + sz.y);
    dl->AddRectFilled(pos, p1, track, 13.0f);
    dl->AddRect(pos, p1, motion::mix(t.outline, t.ink, c), 13.0f, 0, hairlinePx());
    const float knob = 20.0f;
    const float kx = pos.x + 3.0f + k * (sz.x - 6.0f - knob);
    const ImVec2 kc = snapPt(kx + knob * 0.5f, pos.y + sz.y * 0.5f);
    dl->AddCircleFilled(ImVec2(kc.x, kc.y + 1.0f), 10.0f, hex(0x000000, 0x40), 20);
    dl->AddCircleFilled(kc, 10.0f, motion::mix(t.ink, t.onPrimary, c), 24);
    return toggled;
}

// ctl::primaryCta draws PrimaryHover and then paints Primary over it, because
// `f` stays Primary while hovered — the hover never shows. Same contract,
// fixed: solid ink, hover PrimaryHover INSTANT, press PrimaryPress + sink of
// 1 logical px snapped onto the pixel grid (a whole physical pixel). Never scales.
bool inkCta(ImDrawList* dl, const char* id, ImFont* font, const char* label, ImVec2 p0, ImVec2 p1, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    bool fired = false;
    bool held = false;
    bool hov = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(p0);
        fired = ImGui::InvisibleButton(id, ImVec2(p1.x - p0.x, p1.y - p0.y));
        hov = ImGui::IsItemHovered();
        held = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImVec2 a = p0;
    ImVec2 b = p1;
    if (held) {
        a.y += 1.0f;
        b.y += 1.0f;
    }
    a = snapPt(a);
    b = snapPt(b);
    const ImU32 fill = held ? t.primaryPress : (hov ? t.primaryHover : t.primary);
    dl->AddRectFilled(ImVec2(a.x, a.y + 1.0f), ImVec2(b.x, b.y + 1.0f), hex(0x000000, 0x4D), geist::radius::MD);
    dl->AddRectFilled(a, b, fill, geist::radius::MD);
    drawTextCentered(dl, font, geist::type::BODY, a, b, t.onPrimary, label);
    return fired;
}

bool quietButton(ImDrawList* dl, const char* id, ImFont* font, ImVec2 p0, ImVec2 sz, const char* label, const char* icon, bool modal) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    bool fired = false;
    bool hov = false;
    bool held = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(p0);
        fired = ImGui::InvisibleButton(id, sz);
        hov = ImGui::IsItemHovered();
        held = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PushID(id);
    float sc = motion::pressScale(ImGui::GetID("press"), held, motion::PRESS_SCALE);
    ImGui::PopID();
    if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
    const ImVec2 c(p0.x + sz.x * 0.5f, p0.y + sz.y * 0.5f);
    const ImVec2 a(c.x + (p0.x - c.x) * sc, c.y + (p0.y - c.y) * sc);
    const ImVec2 b(c.x + (p0.x + sz.x - c.x) * sc, c.y + (p0.y + sz.y - c.y) * sc);
    dl->AddRectFilled(a, b, hov ? t.high : t.container, 8.0f);
    dl->AddRect(a, b, t.outline, 8.0f, 0, hairlinePx());

    const bool hasIcon = icon && icon[0] != '\0';
    const float iconPx = hasIcon ? 14.0f : 0.0f;
    const float textPx = 12.0f;
    const ImVec2 ts = calcText(font, textPx, label);
    const float gap = hasIcon ? 8.0f : 0.0f;
    const float total = iconPx + gap + ts.x;
    const float start = c.x - total * 0.5f * sc;
    if (hasIcon) drawIcon(dl, icon, ImVec2(start + iconPx * 0.5f * sc, c.y), iconPx * sc, t.mute);
    drawText(dl, font, textPx * sc, ImVec2(start + (iconPx + gap) * sc, c.y - ts.y * 0.5f * sc), t.mute, label);
    return fired;
}

bool iconPlate(ImDrawList* dl, const char* id, ImVec2 pos, float box, const char* icon, float iconPx, bool modal, bool raised) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    bool fired = false;
    bool hov = false;
    bool held = false;
    if (!modal) {
        ImGui::SetCursorScreenPos(pos);
        fired = ImGui::InvisibleButton(id, ImVec2(box, box));
        hov = ImGui::IsItemHovered();
        held = ImGui::IsItemActive();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PushID(id);
    float sc = motion::pressScale(ImGui::GetID("press"), held, motion::PRESS_SCALE_ICON);
    ImGui::PopID();
    if (std::fabs(sc - 1.0f) < 0.001f) sc = 1.0f;
    const ImVec2 c(pos.x + box * 0.5f, pos.y + box * 0.5f);
    const ImVec2 a(c.x - box * 0.5f * sc, c.y - box * 0.5f * sc);
    const ImVec2 b(c.x + box * 0.5f * sc, c.y + box * 0.5f * sc);
    if (raised) {
        ctl::finish(dl, a, b, hov ? ctl::Finish::RaisedHover : ctl::Finish::Raised, 6.0f);
        dl->AddRect(a, b, t.outline, 6.0f, 0, hairlinePx());
    } else {
        dl->AddRectFilled(a, b, hov ? t.high : hex(0x141414, 210), 6.0f);
        dl->AddRect(a, b, t.outline, 6.0f, 0, hairlinePx());
    }
    drawIcon(dl, icon, c, iconPx * sc, t.mute);
    return fired;
}

ImU32 sceneDot(const std::string& scene) {
    const auto& t = upix::ui::geist::tokens();
    if (scene.find("修复") != std::string::npos) return t.sceneRestore;
    if (scene.find("照片") != std::string::npos) return t.scenePhoto;
    if (scene.find("动漫") != std::string::npos) return t.sceneAnime;
    return t.sceneGeneral;
}

bool nameHasScale(const std::string& name, int scale) {
    const std::string n = std::to_string(scale);
    return name.find("x" + n) != std::string::npos || name.find("X" + n) != std::string::npos ||
           name.find(n + "×") != std::string::npos;
}

void drawHudChip(ImDrawList* dl, ImFont* font, ImVec2 p0, float w, const char* text, ImU32 col) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const ImVec2 p1(p0.x + w, p0.y + 26.0f);
    dl->AddRectFilled(p0, p1, hex(0x141414, 210), 6.0f);
    dl->AddRect(p0, p1, t.outline, 6.0f, 0, hairlinePx());
    drawTextCentered(dl, font, 11.0f, p0, p1, col, text);
}

} // namespace

void renderWorkbench(ImDrawList* dl, ImVec2 winSize, UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     SDL_Window* window, DialogBridge* dlgFiles, DialogBridge* dlgFolder, DialogBridge* dlgOutDir) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool modal = ui.showModelPicker;

    const float railW = 76.0f;
    const float inspCol = 332.0f;
    dl->AddRectFilled(ImVec2(railW, 0), winSize, t.bg);

    const float stageX = railW;
    const float stageW = std::max(0.0f, winSize.x - railW - inspCol);
    const bool hasInputs = !snap.inputs.empty() || snap.job.phase != upix::gui::JobPhase::Idle;
    const bool hasDock = snap.inputs.size() > 1 || snap.job.phase == upix::gui::JobPhase::Running ||
                         snap.job.phase == upix::gui::JobPhase::Error;
    const float dockBlock = hasDock ? (54.0f + 10.0f) : 0.0f;
    const ImVec2 stageMin = snapPt(stageX + geist::space::SM, geist::space::LG);
    const ImVec2 stageMax = snapPt(stageX + stageW - geist::space::SM, winSize.y - geist::space::LG - dockBlock);

    // ---------------------------------------------------------------- stage
    drawInsetWell(dl, stageMin, stageMax, geist::radius::LG);

    if (!hasInputs) {
        // Empty content is instant — no fade, no rise. Marks stay at the
        // hairline color so they read as crop registration, not chrome.
        drawRegistrationMarks(dl, stageMin, stageMax, 28.0f, 16.0f, 2.0f, t.outline);

        const ImVec2 center((stageMin.x + stageMax.x) * 0.5f, (stageMin.y + stageMax.y) * 0.5f);
        drawIcon(dl, "zoom-in", ImVec2(center.x, center.y - 22.0f), 32.0f, t.mute, 1.8f);
        drawTextCentered(dl, ui.fontSans, 15.0f, ImVec2(stageMin.x, center.y + 4.0f), ImVec2(stageMax.x, center.y + 28.0f), t.mute,
                         "拖入图片，或按 Ctrl+V 粘贴");

        const ImVec2 btnSz(162.0f, 32.0f);
        const ImVec2 btnPos = snapPt(center.x - btnSz.x * 0.5f, stageMax.y - 36.0f - btnSz.y);
        if (quietButton(dl, "##pick_folder", ui.fontSans, btnPos, btnSz, "选择文件夹批量处理", "folder-open", modal)) {
            SDL_ShowOpenFolderDialog(onFileDialog, dlgFolder, window, nullptr, false);
        }
        if (!modal) {
            ImGui::SetCursorScreenPos(stageMin);
            const float hitH = std::max(1.0f, btnPos.y - stageMin.y - 8.0f);
            if (ImGui::InvisibleButton("##stage_empty", ImVec2(stageMax.x - stageMin.x, hitH))) {
                SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
            }
            if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    } else {
        const float telemH = 40.0f;
        const ImVec2 viewMin = snapPt(stageMin.x + 1.0f, stageMin.y + 1.0f);
        const ImVec2 viewMax = snapPt(stageMax.x - 1.0f, stageMax.y - telemH);
        dl->PushClipRect(viewMin, viewMax, true);

        const bool isCompare = snap.job.phase == upix::gui::JobPhase::Done;
        if (isCompare) {
            const float splitX = snapPx(viewMin.x + (viewMax.x - viewMin.x) * ui.splitRatio);
            dl->PushClipRect(viewMin, ImVec2(splitX, viewMax.y), true);
            drawStageArtwork(dl, viewMin, viewMax, false, ui);
            dl->PopClipRect();
            dl->PushClipRect(ImVec2(splitX, viewMin.y), viewMax, true);
            drawStageArtwork(dl, viewMin, viewMax, true, ui);
            dl->PopClipRect();

            dl->AddRectFilled(ImVec2(splitX, viewMin.y), ImVec2(splitX + hairlinePx(), viewMax.y), t.ink);
            const ImVec2 hC = snapPt(splitX, (viewMin.y + viewMax.y) * 0.5f);
            const ImVec2 h0(hC.x - 14.0f, hC.y - 14.0f);
            const ImVec2 h1(hC.x + 14.0f, hC.y + 14.0f);
            dl->AddRectFilled(h0, h1, hex(0x141414, 220), 6.0f);
            dl->AddRect(h0, h1, t.outline, 6.0f, 0, hairlinePx());
            ImVec2 cL[3] = {ImVec2(hC.x - 3.0f, hC.y - 3.5f), ImVec2(hC.x - 6.5f, hC.y), ImVec2(hC.x - 3.0f, hC.y + 3.5f)};
            ImVec2 cR[3] = {ImVec2(hC.x + 3.0f, hC.y - 3.5f), ImVec2(hC.x + 6.5f, hC.y), ImVec2(hC.x + 3.0f, hC.y + 3.5f)};
            dl->AddPolyline(cL, 3, t.mute, ImDrawFlags_None, 1.4f);
            dl->AddPolyline(cR, 3, t.mute, ImDrawFlags_None, 1.4f);

            if (!modal) {
                ImGui::SetCursorScreenPos(ImVec2(splitX - 16.0f, viewMin.y));
                ImGui::InvisibleButton("##split_drag", ImVec2(32.0f, std::max(1.0f, viewMax.y - viewMin.y)));
                if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                if (ImGui::IsItemActive()) {
                    ui.splitRatio = std::clamp((ImGui::GetIO().MousePos.x - viewMin.x) / std::max(1.0f, viewMax.x - viewMin.x), 0.05f, 0.95f);
                }
            }

            drawHudChip(dl, ui.fontMono, snapPt(viewMin.x + 14.0f, viewMin.y + 14.0f), 77.0f, "原图 · 1×", t.mute);
            int scale = 4;
            for (const auto& m : snap.installed) {
                if (m.name == snap.model) scale = m.scale;
            }
            const std::string after = std::format("超分 · {}×", scale);
            const ImVec2 close0 = snapPt(viewMax.x - 14.0f - 28.0f, viewMin.y + 14.0f);
            drawHudChip(dl, ui.fontMono, snapPt(close0.x - 10.0f - 77.0f, viewMin.y + 14.0f), 77.0f, after.c_str(), t.ink);
            if (iconPlate(dl, "##close_stage_done", close0, 28.0f, "x", 13.0f, modal, false)) app.clearInputs();
        } else {
            drawStageArtwork(dl, viewMin, viewMax, false, ui);
            if (snap.job.phase == upix::gui::JobPhase::Running) {
                const float scanY = snapPx(viewMin.y + (viewMax.y - viewMin.y) * (snap.job.percent / 100.0f));
                dl->AddRectFilled(ImVec2(viewMin.x, scanY), ImVec2(viewMax.x, scanY + hairlinePx()), t.accent);
            }
            const ImVec2 close0 = snapPt(viewMax.x - 14.0f - 28.0f, viewMin.y + 14.0f);
            if (iconPlate(dl, "##close_stage_run", close0, 28.0f, "x", 13.0f, modal, false)) app.clearInputs();
        }
        dl->PopClipRect();

        const ImVec2 bar0 = snapPt(stageMin.x + 1.0f, stageMax.y - telemH);
        const ImVec2 bar1 = snapPt(stageMax.x - 1.0f, stageMax.y - 1.0f);
        dl->AddRectFilled(bar0, bar1, t.surface, 11.0f, ImDrawFlags_RoundCornersBottom);
        dl->AddRectFilled(ImVec2(bar0.x, bar0.y), ImVec2(bar1.x, bar0.y + hairlinePx()), t.outline);
        const float cy = snapPx((bar0.y + bar1.y) * 0.5f);

        if (snap.job.phase == upix::gui::JobPhase::Running) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.accent);
            const float textX = bar0.x + 28.0f;
            drawText(dl, ui.fontMono, 12.0f, ImVec2(textX, cy - 7.5f), t.ink, snap.job.current_file.c_str());
            const ImVec2 csz = calcText(ui.fontMono, 12.0f, snap.job.current_file.c_str());
            std::string tileMeta;
            if (snap.job.tiles_total > 0) {
                tileMeta = std::format("分块 {}/{}", snap.job.tiles_done, snap.job.tiles_total);
            }
            if (snap.job.eta_sec > 0) {
                if (!tileMeta.empty()) tileMeta += " · ";
                tileMeta += std::format("剩余 ~{}s", snap.job.eta_sec);
            }
            if (!tileMeta.empty()) {
                drawText(dl, ui.fontMono, 11.0f, ImVec2(textX + csz.x + 10.0f, cy - 7.0f), t.mute, tileMeta.c_str());
            }

            const std::string pctStr = std::format("{}%", snap.job.percent);
            const ImVec2 psz = calcText(ui.fontMono, 11.0f, pctStr.c_str());
            const float pctX = bar1.x - 14.0f - psz.x;
            drawText(dl, ui.fontMono, 11.0f, ImVec2(pctX, cy - 7.0f), t.ink, pctStr.c_str());
            const float trkW = 148.0f;
            const float trkX = snapPx(pctX - 10.0f - trkW);
            const float frac = std::clamp(snap.job.percent / 100.0f, 0.0f, 1.0f);
            const float fillW = followSpring(ImGui::GetID("job-progress"), frac * trkW, motion::SpringParams{180.0f, 26.0f});
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + trkW, cy + 2.0f), t.recessed, 2.0f);
            dl->AddRectFilled(ImVec2(trkX, cy - 2.0f), ImVec2(trkX + std::clamp(fillW, 0.0f, trkW), cy + 2.0f), t.accent, 2.0f);
        } else if (snap.job.phase == upix::gui::JobPhase::Done) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.accent);
            int scale = 4;
            for (const auto& m : snap.installed) {
                if (m.name == snap.model) scale = m.scale;
            }
            const std::string fmtLabel = snap.format.empty() ? "PNG" : snap.format;
            const std::string badge = std::format("{}× · {}", scale, fmtLabel == "png" ? "PNG" : (fmtLabel == "jpg" ? "JPG" : (fmtLabel == "webp" ? "WebP" : (fmtLabel == "avif" ? "AVIF" : "PNG"))));
            const ImVec2 bsz = calcText(ui.fontMono, 10.0f, badge.c_str());
            const ImVec2 b0 = snapPt(bar0.x + 28.0f, cy - 10.0f);
            const ImVec2 b1 = snapPt(b0.x + bsz.x + 12.0f, cy + 10.0f);
            dl->AddRectFilled(b0, b1, t.accentSoft, 4.0f);
            dl->AddRect(b0, b1, t.accentBorder, 4.0f, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.0f, b0, b1, t.accent, badge.c_str());

            const std::string resStr = std::format("{}×{} → {}×{}", snap.job.in_w, snap.job.in_h, snap.job.out_w, snap.job.out_h);
            drawText(dl, ui.fontMono, 12.0f, ImVec2(b1.x + 10.0f, cy - 7.5f), t.ink, resStr.c_str());
            const ImVec2 rsz = calcText(ui.fontMono, 12.0f, resStr.c_str());
            const std::string metaStr = std::format("{:.2f}s · {} · {}", snap.job.elapsed_sec, snap.job.out_size_str, snap.model);
            drawText(dl, ui.fontMono, 11.0f, ImVec2(b1.x + 18.0f + rsz.x, cy - 7.0f), t.mute, metaStr.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(bar1.x - 148.0f, cy - 7.0f), t.mute, "定位文件");
            const ImVec2 open0 = snapPt(bar1.x - 86.0f, cy - 12.0f);
            dl->AddRectFilled(open0, ImVec2(open0.x + 72.0f, open0.y + 24.0f), t.container, 6.0f);
            dl->AddRect(open0, ImVec2(open0.x + 72.0f, open0.y + 24.0f), t.outline, 6.0f, 0, hairlinePx());
            drawTextCentered(dl, ui.fontSans, 11.0f, open0, ImVec2(open0.x + 72.0f, open0.y + 24.0f), t.body, "打开图像");
        } else if (snap.job.phase == upix::gui::JobPhase::Error) {
            dl->AddCircleFilled(ImVec2(bar0.x + 16.0f, cy), 3.0f, t.error);
            const ImVec2 e0 = snapPt(bar0.x + 28.0f, cy - 10.0f);
            const ImVec2 e1 = snapPt(e0.x + 36.0f, cy + 10.0f);
            dl->AddRectFilled(e0, e1, t.errorSoft, 4.0f);
            dl->AddRect(e0, e1, t.errorBorder, 4.0f, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.0f, e0, e1, t.error, "ERR");
            drawText(dl, ui.fontSans, 12.0f, ImVec2(e1.x + 10.0f, cy - 8.0f), t.ink, snap.job.message.c_str());
            const ImVec2 msz = calcText(ui.fontSans, 12.0f, snap.job.message.c_str());
            drawText(dl, ui.fontMono, 11.0f, ImVec2(e1.x + 18.0f + msz.x, cy - 7.0f), t.mute, snap.job.diag_detail.c_str());
            const ImVec2 retry0 = snapPt(bar1.x - 60.0f, cy - 12.0f);
            if (quietButton(dl, "##retry_job", ui.fontSans, retry0, ImVec2(48.0f, 24.0f), "重试", "", modal)) app.startJob();
        } else {
            const std::string ready = std::format("已就绪 · {} 项输入待处理", snap.inputs.size());
            drawText(dl, ui.fontSans, 12.0f, ImVec2(bar0.x + 16.0f, cy - 8.0f), t.mute, ready.c_str());
        }
    }

    if (hasDock) {
        const ImVec2 dock0 = snapPt(stageMin.x, stageMax.y + 10.0f);
        const ImVec2 dock1 = snapPt(stageMax.x, stageMax.y + 10.0f + 54.0f);
        ctl::finish(dl, dock0, dock1, ctl::Finish::Card, geist::radius::LG);

        size_t doneCnt = 0;
        for (const auto& it : snap.inputs) {
            if (it.status_kind == 2) ++doneCnt;
        }
        const std::string cntText = std::format("{} / {}", doneCnt, snap.inputs.size());
        const ImVec2 cntSz = calcText(ui.fontMono, 11.0f, cntText.c_str());
        const ImVec2 add0 = snapPt(dock1.x - 8.0f - 28.0f, dock0.y + 13.0f);
        const float divX = snapPx(add0.x - 10.0f);
        drawText(dl, ui.fontMono, 11.0f, ImVec2(divX - 10.0f - cntSz.x, dock0.y + 19.5f), t.mute, cntText.c_str());
        dl->AddRectFilled(ImVec2(divX, dock0.y + 18.0f), ImVec2(divX + hairlinePx(), dock0.y + 36.0f), t.outline);

        const float thumbsRight = divX - 10.0f - cntSz.x - 8.0f;
        dl->PushClipRect(ImVec2(dock0.x + 6.0f, dock0.y), ImVec2(thumbsRight, dock1.y), true);
        float cx = dock0.x + 8.0f;
        const float ty = dock0.y + 7.0f;
        for (size_t i = 0; i < snap.inputs.size(); ++i) {
            const auto& item = snap.inputs[i];
            const ImVec2 th0 = snapPt(cx, ty);
            const ImVec2 th1 = snapPt(cx + 40.0f, ty + 40.0f);
            const bool hot = ctl::mouseIn(th0, th1);
            drawMiniThumb(dl, th0, th1, ui, 6.0f, i);
            if (item.status_kind == 0) dl->AddRectFilled(th0, th1, hex(0x000000, 90), 6.0f);
            if (item.status_kind == 3) dl->AddRectFilled(th0, th1, t.errorSoft, 6.0f);
            if (hot) dl->AddRectFilled(th0, th1, hex(0xFFFFFF, 0x14), 6.0f);
            ImU32 stroke = t.outline;
            if (item.status_kind == 1) stroke = t.accent;
            else if (item.status_kind == 3) stroke = t.error;
            dl->AddRect(th0, th1, stroke, 6.0f, 0, hairlinePx());
            if (item.status_kind == 1) {
                const float progW = std::clamp(snap.job.percent / 100.0f, 0.08f, 1.0f) * 32.0f;
                dl->AddRectFilled(snapPt(th0.x + 4.0f, th1.y - 3.0f), snapPt(th0.x + 4.0f + progW, th1.y - 1.5f), t.accent, 1.0f);
            }
            cx += 46.0f;
        }
        dl->PopClipRect();
        if (iconPlate(dl, "##dock_add", add0, 28.0f, "plus", 12.0f, modal, true)) {
            SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
        }
    }

    // ------------------------------------------------------------- inspector
    const float inspX = winSize.x - inspCol;
    const ImVec2 card0 = snapPt(inspX + geist::space::SM, geist::space::LG);
    const ImVec2 card1 = snapPt(winSize.x - geist::space::LG, winSize.y - geist::space::LG);
    ctl::finish(dl, card0, card1, ctl::Finish::Card, geist::radius::LG);

    const float ix = card0.x + geist::space::LG;
    const float iw = std::max(80.0f, (card1.x - card0.x) - geist::space::LG * 2.0f);
    const float seam = snapPx(card1.y - geist::space::LG - 44.0f - geist::space::MD);
    float y = card0.y + geist::space::LG;

    auto overline = [&](const char* s) {
        drawText(dl, ui.fontSans, geist::type::LABEL, snapPt(ix, y), t.mute, s);
        y += calcText(ui.fontSans, geist::type::LABEL, s).y + geist::space::XS;
    };
    auto fieldLabel = [&](const char* s) {
        drawText(dl, ui.fontSans, geist::type::LABEL, snapPt(ix, y), t.mute, s);
        y += calcText(ui.fontSans, geist::type::LABEL, s).y + geist::space::XS;
    };

    dl->PushClipRect(ImVec2(card0.x + 1.0f, card0.y + 1.0f), ImVec2(card1.x - 1.0f, seam), true);

    std::string shortMod = snap.model;
    int modScale = 4;
    bool modRec = false;
    for (const auto& m : snap.installed) {
        if (m.name == snap.model) {
            shortMod = m.short_name.empty() ? m.name : m.short_name;
            modScale = m.scale;
            modRec = m.recommended;
            break;
        }
    }

    overline("模型");
    const ImVec2 mp0 = snapPt(ix, y);
    const ImVec2 mp1 = snapPt(ix + iw, y + 36.0f);
    auto paintTrigger = [&]() {
        const bool hov = ctl::mouseIn(mp0, mp1);
        ctl::finish(dl, mp0, mp1, hov ? ctl::Finish::RaisedHover : ctl::Finish::Raised, 8.0f);
        dl->AddRect(mp0, mp1, ui.showModelPicker ? t.accent : t.outlineInput, 8.0f, 0, hairlinePx());
        const float textMax = mp1.x - 36.0f - (nameHasScale(shortMod, modScale) ? 0.0f : 36.0f);
        dl->PushClipRect(ImVec2(mp0.x + 10.0f, mp0.y), ImVec2(textMax, mp1.y), true);
        drawText(dl, ui.fontMono, geist::type::BODY, ImVec2(mp0.x + 12.0f, mp0.y + 10.0f), t.ink, shortMod.c_str());
        dl->PopClipRect();
        const ImVec2 nsz = calcText(ui.fontMono, geist::type::BODY, shortMod.c_str());
        float cursor = mp0.x + 12.0f + nsz.x + 6.0f;
        if (modRec && cursor < textMax) {
            drawIcon(dl, "star", ImVec2(cursor + 6.0f, mp0.y + 18.0f), 11.0f, t.warning);
        }
        if (!nameHasScale(shortMod, modScale)) {
            const std::string badge = std::format("{}×", modScale);
            const ImVec2 b0 = snapPt(mp1.x - 56.0f, mp0.y + 8.0f);
            const ImVec2 b1 = snapPt(mp1.x - 28.0f, mp0.y + 28.0f);
            dl->AddRectFilled(b0, b1, t.container, 4.0f);
            dl->AddRect(b0, b1, t.outline, 4.0f, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 11.0f, b0, b1, t.mute, badge.c_str());
        }
        drawIcon(dl, ui.showModelPicker ? "chevron-up" : "chevron-down", ImVec2(mp1.x - 16.0f, mp0.y + 18.0f), 14.0f, t.mute);
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    };
    paintTrigger();
    ImGui::SetCursorScreenPos(mp0);
    if (ImGui::InvisibleButton("##model_picker_btn", ImVec2(iw, 36.0f))) ui.showModelPicker = !ui.showModelPicker;
    y += 36.0f;

    y += geist::space::SECTION;
    overline("输出");
    const SegOpt fmtOpts[5] = {{"跟随", ""}, {"PNG", "png"}, {"JPG", "jpg"}, {"WebP", "webp"}, {"AVIF", "avif"}};
    std::string curFmt = snap.format;
    if (segmented(dl, "##fmt", ui.fontSans, ImVec2(ix, y), ImVec2(iw, 32.0f), fmtOpts, 5, curFmt, modal)) app.setFormat(curFmt);
    y += 32.0f;

    if (snap.format != "png") {
        y += geist::space::SM;
        const std::string qVal = std::format("{}", snap.quality);
        const ImVec2 qsz = calcText(ui.fontMono, geist::type::BODY, qVal.c_str());
        drawText(dl, ui.fontSans, geist::type::LABEL, snapPt(ix, y), t.mute, "质量");
        drawText(dl, ui.fontMono, geist::type::BODY, ImVec2(ix + iw - qsz.x, y - 1.0f), t.ink, qVal.c_str());
        y += calcText(ui.fontSans, geist::type::LABEL, "质量").y + geist::space::XS;
        int q = snap.quality;
        if (qualitySlider(dl, "##q_slider", ImVec2(ix, y), iw, q, 1, 100, modal)) app.setQuality(q);
        y += 18.0f;
    }
    if (snap.format == "avif" || snap.format == "jpg") {
        y += geist::space::SM;
        fieldLabel("色度抽样");
        const SegOpt chromaOpts[2] = {{"4:2:0", "420"}, {"4:4:4", "444"}};
        std::string cVal = std::to_string(snap.chroma);
        if (segmented(dl, "##chroma", ui.fontMono, ImVec2(ix, y), ImVec2(iw, 28.0f), chromaOpts, 2, cVal, modal)) {
            app.setChroma(std::atoi(cVal.c_str()));
        }
        y += 28.0f;
    }
    if (snap.format == "avif") {
        y += geist::space::SM;
        fieldLabel("编码速度");
        const SegOpt spdOpts[3] = {{"精细", "4"}, {"均衡", "6"}, {"快速", "8"}};
        std::string sVal = std::to_string(snap.avif_speed);
        if (segmented(dl, "##spd", ui.fontSans, ImVec2(ix, y), ImVec2(iw, 28.0f), spdOpts, 3, sVal, modal)) {
            app.setAvifSpeed(std::atoi(sVal.c_str()));
        }
        y += 28.0f;
    }

    y += geist::space::SM;
    fieldLabel("导出位置");
    const ImVec2 dir0 = snapPt(ix, y);
    const ImVec2 dir1 = snapPt(ix + iw, y + 34.0f);
    dl->AddRectFilled(dir0, dir1, t.wellBg, 8.0f);
    dl->AddRect(dir0, dir1, t.outline, 8.0f, 0, hairlinePx());
    drawIcon(dl, "folder", ImVec2(dir0.x + 16.0f, dir0.y + 17.0f), 13.0f, t.mute);
    const bool hasCustom = !snap.custom_out_dir.empty();
    const std::string outLabel = hasCustom ? snap.custom_out_dir : "跟随原图目录";
    const char* dirBtn = hasCustom ? "更改" : "指定…";
    const float dirBtnW = hasCustom ? 44.0f : 56.0f;
    const ImVec2 db0 = snapPt(dir1.x - dirBtnW - 5.0f, dir0.y + 5.0f);
    dl->PushClipRect(ImVec2(dir0.x + 28.0f, dir0.y), ImVec2(db0.x - 6.0f, dir1.y), true);
    drawText(dl, hasCustom ? ui.fontMono : ui.fontSans, 12.0f, ImVec2(dir0.x + 28.0f, dir0.y + 9.0f), t.body, outLabel.c_str());
    dl->PopClipRect();
    if (quietButton(dl, "##pick_outdir", ui.fontSans, db0, ImVec2(dirBtnW, 24.0f), dirBtn, "", modal)) {
        SDL_ShowOpenFolderDialog(onFileDialog, dlgOutDir, window, nullptr, false);
    }
    y += 34.0f;

    y += geist::space::SECTION;
    overline("引擎与处理");
    fieldLabel("计算设备");
    const SegOpt devOpts[2] = {{"GPU", "gpu"}, {"CPU", "cpu"}};
    std::string curDev = snap.gpu ? "gpu" : "cpu";
    if (segmented(dl, "##dev", ui.fontSans, ImVec2(ix, y), ImVec2(iw, 28.0f), devOpts, 2, curDev, modal)) app.setGpu(curDev == "gpu");
    y += 28.0f;

    y += geist::space::SM;
    fieldLabel("分块尺寸");
    const SegOpt tileOpts[4] = {{"自动", "0"}, {"512", "512"}, {"256", "256"}, {"128", "128"}};
    std::string curTile = std::to_string(snap.tile_size);
    if (segmented(dl, "##tile", ui.fontSans, ImVec2(ix, y), ImVec2(iw, 28.0f), tileOpts, 4, curTile, modal)) {
        app.setTileSize(std::atoi(curTile.c_str()));
    }
    y += 28.0f;

    y += geist::space::SM;
    {
        const char* lab = "常驻热引擎";
        const ImVec2 ls = calcText(ui.fontSans, geist::type::BODY, lab);
        drawText(dl, ui.fontSans, geist::type::BODY, snapPt(ix, y + (26.0f - ls.y) * 0.5f), t.ink, lab);
        bool daemonOn = snap.daemon.running;
        if (inkSwitch(dl, "##daemon", ImVec2(ix + iw - 44.0f, y), daemonOn, modal)) app.toggleDaemon();
        y += 26.0f;
    }
    y += geist::space::SM;
    {
        const char* lab = "覆盖已有结果";
        const ImVec2 ls = calcText(ui.fontSans, geist::type::BODY, lab);
        drawText(dl, ui.fontSans, geist::type::BODY, snapPt(ix, y + (26.0f - ls.y) * 0.5f), t.ink, lab);
        bool forceOn = snap.force;
        if (inkSwitch(dl, "##force", ImVec2(ix + iw - 44.0f, y), forceOn, modal)) app.setForce(forceOn);
        y += 26.0f;
    }

    dl->PopClipRect();

    dl->AddRectFilled(ImVec2(card0.x, seam), ImVec2(card1.x, seam + hairlinePx()), t.outline);
    const ImVec2 cta0 = snapPt(ix, seam + geist::space::MD);
    const ImVec2 cta1 = snapPt(ix + iw, cta0.y + 44.0f);
    if (snap.job.phase == upix::gui::JobPhase::Running) {
        bool fired = false;
        bool hov = false;
        if (!modal) {
            ImGui::SetCursorScreenPos(cta0);
            fired = ImGui::InvisibleButton("##cancel_job", ImVec2(cta1.x - cta0.x, cta1.y - cta0.y));
            hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        dl->AddRectFilled(cta0, cta1, hov ? hex(0xE5484D, 0x40) : t.errorSoft, geist::radius::MD);
        dl->AddRect(cta0, cta1, t.errorBorder, geist::radius::MD, 0, hairlinePx());
        const char* cancel = "取消超分";
        const ImVec2 tsz = calcText(ui.fontSans, 14.0f, cancel);
        const float kbdW = 48.0f;
        const float group = 9.0f + 8.0f + tsz.x + 8.0f + kbdW;
        float x = cta0.x + ((cta1.x - cta0.x) - group) * 0.5f;
        const float mid = (cta0.y + cta1.y) * 0.5f;
        dl->AddRectFilled(snapPt(x, mid - 4.5f), snapPt(x + 9.0f, mid + 4.5f), t.error, 2.0f);
        x += 17.0f;
        drawText(dl, ui.fontSans, 14.0f, ImVec2(x, mid - tsz.y * 0.5f), t.error, cancel);
        x += tsz.x + 8.0f;
        const ImVec2 k0 = snapPt(x, mid - 9.0f);
        const ImVec2 k1 = snapPt(x + kbdW, mid + 9.0f);
        dl->AddRectFilled(k0, k1, hex(0x2A181A), 4.0f);
        dl->AddRect(k0, k1, t.errorBorder, 4.0f, 0, hairlinePx());
        drawTextCentered(dl, ui.fontMono, 10.0f, k0, k1, t.error, "Ctrl+.");
        if (fired) app.cancelJob();
    } else {
        const char* label = "开始超分";
        if (snap.job.phase == upix::gui::JobPhase::Error) label = "继续超分";
        else if (snap.job.phase == upix::gui::JobPhase::Done) label = "重新超分";
        if (inkCta(dl, "##start_job", ui.fontSans, label, cta0, cta1, modal)) {
            if (snap.job.phase == upix::gui::JobPhase::Idle && snap.inputs.empty()) {
                SDL_ShowOpenFileDialog(onFileDialog, dlgFiles, window, nullptr, 0, nullptr, true);
            } else {
                app.startJob();
            }
        }
    }

    // Model picker: instant scrim + Menu panel. No fade, no scale.
    // Hits are geometric so they don't depend on ImGui item order.
    if (ui.showModelPicker) {
        const float rowH = 32.0f;
        const float rowGap = 2.0f;
        const float pad = 6.0f;
        const int n = static_cast<int>(snap.installed.size());
        const float listH = n == 0 ? 64.0f : static_cast<float>(n) * rowH + static_cast<float>(std::max(0, n - 1)) * rowGap;
        const float panelH = std::min(320.0f, pad * 2.0f + listH);
        float py = mp1.y + 6.0f;
        if (py + panelH > winSize.y - 8.0f) py = std::max(8.0f, mp0.y - 6.0f - panelH);
        const ImVec2 pop0 = snapPt(mp0.x, py);
        const ImVec2 pop1 = snapPt(mp1.x, py + panelH);

        const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool ate = ctl::mouseIn(mp0, mp1);
        if (click && !ate) {
            bool hitRow = false;
            for (int i = 0; i < n; ++i) {
                const float ry = pop0.y + pad + static_cast<float>(i) * (rowH + rowGap);
                if (ry + rowH > pop1.y - pad + 0.5f) break;
                const ImVec2 r0 = snapPt(pop0.x + pad, ry);
                const ImVec2 r1 = snapPt(pop1.x - pad, ry + rowH);
                if (ctl::mouseIn(r0, r1)) {
                    app.setModel(snap.installed[static_cast<size_t>(i)].name);
                    ui.showModelPicker = false;
                    hitRow = true;
                    break;
                }
            }
            if (!hitRow && !ctl::mouseIn(pop0, pop1)) ui.showModelPicker = false;
        }

        dl->AddRectFilled(ImVec2(0, 0), winSize, hex(0x000000, 102));
        paintTrigger();
        ctl::finish(dl, pop0, pop1, ctl::Finish::Menu, 10.0f);

        if (n == 0) {
            drawTextCentered(dl, ui.fontSans, 12.0f, pop0, pop1, t.mute, "尚未安装模型");
        }
        dl->PushClipRect(pop0, pop1, true);
        for (int i = 0; i < n; ++i) {
            const auto& m = snap.installed[static_cast<size_t>(i)];
            const float ry = pop0.y + pad + static_cast<float>(i) * (rowH + rowGap);
            if (ry + rowH > pop1.y - pad + 0.5f) break;
            const ImVec2 r0 = snapPt(pop0.x + pad, ry);
            const ImVec2 r1 = snapPt(pop1.x - pad, ry + rowH);
            const bool sel = m.name == snap.model;
            const bool hov = ctl::mouseIn(r0, r1);
            if (sel) dl->AddRectFilled(r0, r1, t.accentSoft, 6.0f);
            else if (hov) dl->AddRectFilled(r0, r1, t.high, 6.0f);
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            const char* title = m.short_name.empty() ? m.name.c_str() : m.short_name.c_str();
            const std::string meta = std::format("{} · {}×", m.scene.empty() ? "综合" : m.scene, m.scale);
            const ImVec2 msz = calcText(ui.fontSans, 11.0f, meta.c_str());
            dl->PushClipRect(ImVec2(r0.x + 8.0f, r0.y), ImVec2(r1.x - 16.0f - msz.x - (m.recommended ? 16.0f : 0.0f), r1.y), true);
            drawText(dl, ui.fontMono, 12.5f, ImVec2(r0.x + 10.0f, r0.y + 8.0f), t.ink, title);
            dl->PopClipRect();
            if (m.recommended) {
                const ImVec2 tsz = calcText(ui.fontMono, 12.5f, title);
                drawIcon(dl, "star", ImVec2(r0.x + 10.0f + std::min(tsz.x, r1.x - r0.x - 80.0f) + 8.0f, r0.y + 16.0f), 10.0f, t.warning);
            }
            dl->AddCircleFilled(ImVec2(r1.x - msz.x - 16.0f, (r0.y + r1.y) * 0.5f), 2.5f, sceneDot(m.scene));
            drawText(dl, ui.fontSans, 11.0f, ImVec2(r1.x - 8.0f - msz.x, r0.y + 9.0f), t.mute, meta.c_str());
        }
        dl->PopClipRect();
    }
}

} // namespace upix::views
