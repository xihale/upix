#include "views/common.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace upix::views {
namespace {

using upix::ui::ctl::Finish;

constexpr float kRailW = 76.0f;
constexpr float kPagePad = 32.0f;
constexpr float kColMax = 720.0f;

float gPageScroll = 0.0f;
float gPageContentH = 0.0f;
float gLogScroll = 0.0f;
double gCopiedUntil = -1.0;

std::string hexDigits(const std::string& fp) {
    std::string out;
    out.reserve(fp.size());
    for (size_t i = 0; i < fp.size();) {
        const unsigned char c = static_cast<unsigned char>(fp[i]);
        if (c < 0x80) {
            if (std::isxdigit(c)) out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < fp.size()) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < fp.size()) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < fp.size()) {
            i += 4;
        } else {
            ++i;
        }
    }
    return out;
}

std::array<std::string, 4> fpGroups(const std::string& fp) {
    const std::string raw = hexDigits(fp);
    if (raw.size() >= 16) {
        return {raw.substr(0, 4), raw.substr(4, 4), raw.substr(8, 4), raw.substr(12, 4)};
    }
    const std::string dot = "\u00B7\u00B7\u00B7\u00B7";
    return {dot, dot, dot, dot};
}

std::string dottedFp(const std::string& fp) {
    const auto g = fpGroups(fp);
    return g[0] + " \u00B7 " + g[1] + " \u00B7 " + g[2] + " \u00B7 " + g[3];
}

const char* ipKind(const std::string& ip) {
    if (ip.rfind("100.", 0) == 0) return "TS";
    if (ip.rfind("10.", 0) == 0 || ip.rfind("172.", 0) == 0) return "VPN";
    if (ip.rfind("127.", 0) == 0) return "LO";
    if (ip.empty()) return "";
    return "LAN";
}

float ctaWidth(ImFont* font, const char* label, bool withIcon) {
    using namespace upix::ui;
    const float text = calcText(font, 14.0f, label).x;
    const float icon = withIcon ? (15.0f + geist::space::SM) : 0.0f;
    const float w = std::ceil((text + icon + geist::space::LG * 2.0f) / 4.0f) * 4.0f;
    return std::max(104.0f, w);
}

void drawQr(ImDrawList* dl, ImVec2 well0, ImVec2 well1, const std::string& payload, bool live) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const float radius = 10.0f;
    dl->AddRectFilled(well0, well1, t.recessed, radius);
    dl->AddRect(well0, well1, t.outline, radius, 0, hairlinePx());

    const ImU32 mark = live ? t.accentBorder : t.faint;
    drawRegistrationMarks(dl, well0, well1, 14.0f, 14.0f, 1.5f, mark);

    if (live && !payload.empty()) {
        try {
            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            const int qsz = qr.getSize();
            constexpr int kQuietModules = 4;
            const int totalModules = qsz + kQuietModules * 2;
            const float scale = currentDpiScale() > 0.0f ? currentDpiScale() : 1.0f;

            // Integer physical pixels per QR module + explicit 4-module quiet zone -> zero subpixel blur.
            int modPx = static_cast<int>(std::floor(200.0f * scale / static_cast<float>(std::max(totalModules, 1))));
            if (modPx < 1) modPx = 1;
            const float mod = static_cast<float>(modPx) / scale;
            const float plateSz = mod * static_cast<float>(totalModules);
            const float wellSz = well1.x - well0.x;
            const ImVec2 plate0 = snapPt(well0.x + (wellSz - plateSz) * 0.5f, well0.y + (wellSz - plateSz) * 0.5f);
            const ImVec2 plate1 = snapPt(plate0.x + plateSz, plate0.y + plateSz);

            dl->AddRectFilled(plate0, plate1, t.qrPaper, geist::radius::MD);
            dl->AddRect(plate0, plate1, IM_COL32(0, 0, 0, 28), geist::radius::MD, 0, hairlinePx());

            const float ox = plate0.x + mod * static_cast<float>(kQuietModules);
            const float oy = plate0.y + mod * static_cast<float>(kQuietModules);
            for (int qy = 0; qy < qsz; ++qy) {
                for (int qx = 0; qx < qsz; ++qx) {
                    if (!qr.getModule(qx, qy)) continue;
                    const ImVec2 m0 = snapPt(ox + qx * mod, oy + qy * mod);
                    const ImVec2 m1 = snapPt(ox + (qx + 1) * mod, oy + (qy + 1) * mod);
                    dl->AddRectFilled(m0, m1, t.qrInk);
                }
            }
        } catch (...) {
        }
        return;
    }

    // Calm muted dark dormant QR matrix (never a harsh white plate when idle).
    const ImU32 ghost = t.faint;
    auto finder = [&](ImVec2 tl) {
        dl->AddRect(tl, ImVec2(tl.x + 32.0f, tl.y + 32.0f), ghost, 4.0f, 0, 2.5f);
        dl->AddRectFilled(ImVec2(tl.x + 9.0f, tl.y + 9.0f), ImVec2(tl.x + 23.0f, tl.y + 23.0f), ghost, 2.0f);
    };
    finder(ImVec2(well0.x + 48.0f, well0.y + 48.0f));
    finder(ImVec2(well1.x - 48.0f - 32.0f, well0.y + 48.0f));
    finder(ImVec2(well0.x + 48.0f, well1.y - 48.0f - 32.0f));
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < 7; ++gx) {
            const bool finderCell = (gx < 2 && gy < 2) || (gx > 4 && gy < 2) || (gx < 2 && gy > 4);
            const bool center = gx >= 2 && gx <= 4 && gy >= 2 && gy <= 4;
            if (finderCell || center) continue;
            if ((gx * 3 + gy * 5) % 2 != 0) continue;
            const ImVec2 d(well0.x + 60.0f + gx * 19.0f, well0.y + 60.0f + gy * 19.0f);
            dl->AddRectFilled(ImVec2(d.x, d.y), ImVec2(d.x + 4.5f, d.y + 4.5f), ghost, 1.5f);
        }
    }
    const ImVec2 mid((well0.x + well1.x) * 0.5f, (well0.y + well1.y) * 0.5f);
    const ImVec2 b0 = snapPt(mid.x - 24.0f, mid.y - 24.0f);
    const ImVec2 b1 = snapPt(mid.x + 24.0f, mid.y + 24.0f);
    dl->AddRectFilled(b0, b1, t.container, 12.0f);
    dl->AddRect(b0, b1, t.outline, 12.0f, 0, hairlinePx());
    drawIcon(dl, "qr-code", mid, 20.0f, t.mute, 1.6f);
}

bool portInputField(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, int port, bool editable,
                    ImFont* mono, int& outPort) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    a = snapPt(a);
    b = snapPt(b);
    const float midY = (a.y + b.y) * 0.5f;

    if (!editable) {
        ctl::finish(dl, a, b, Finish::Inset, geist::radius::MD, false);
        dl->AddRect(a, b, t.outline, geist::radius::MD, 0, hairlinePx());
        const ImVec2 colon = calcText(mono, geist::type::MONO, ":");
        const std::string portStr = std::to_string(port);
        const ImVec2 ps = calcText(mono, geist::type::MONO, portStr.c_str());
        const float contentW = colon.x + 5.0f + ps.x;
        const float startX = a.x + ((b.x - a.x) - contentW) * 0.5f;
        drawText(dl, mono, geist::type::MONO, snapPt(startX, midY - colon.y * 0.5f), t.faint, ":");
        drawText(dl, mono, geist::type::MONO, snapPt(startX + colon.x + 5.0f, midY - ps.y * 0.5f), t.mute,
                 portStr.c_str());
        return false;
    }

    struct PortBuf {
        char text[12] = {};
        int synced = -1;
        bool init = false;
    };
    static std::unordered_map<ImGuiID, PortBuf> bufs;
    PortBuf& buf = bufs[id];
    if (!buf.init || (buf.synced != port && !ImGui::IsAnyItemActive())) {
        std::snprintf(buf.text, sizeof(buf.text), "%d", port);
        buf.synced = port;
        buf.init = true;
    }

    ctl::finish(dl, a, b, Finish::Inset, geist::radius::MD, false);

    const ImVec2 colon = calcText(mono, geist::type::MONO, ":");
    const float colonX = a.x + 12.0f;
    drawText(dl, mono, geist::type::MONO, snapPt(colonX, midY - colon.y * 0.5f), t.faint, ":");

    const float inputX = colonX + colon.x + 5.0f;
    const float inputW = std::max(24.0f, b.x - 10.0f - inputX);

    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    ImGui::SetCursorScreenPos(ImVec2(inputX, a.y + (b.y - a.y - 20.0f) * 0.5f));
    if (mono) ImGui::PushFont(mono, geist::type::MONO);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, t.ink);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(inputW);
    char item[24];
    std::snprintf(item, sizeof(item), "##port%08x", static_cast<unsigned>(id));
    const bool edited = ImGui::InputText(item, buf.text, sizeof(buf.text), ImGuiInputTextFlags_CharsDecimal);
    const bool active = ImGui::IsItemActive();
    const bool focused = active || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (mono) ImGui::PopFont();
    ImGui::PopID();

    dl->AddRect(a, b, focused ? t.accent : t.outlineInput, geist::radius::MD, 0, hairlinePx());

    bool changed = false;
    auto parsePort = [&](long& n) -> bool {
        char* end = nullptr;
        n = std::strtol(buf.text, &end, 10);
        return end != buf.text;
    };
    if (edited) {
        long n = 0;
        if (parsePort(n) && n >= 1024 && n <= 65535) {
            outPort = static_cast<int>(n);
            buf.synced = outPort;
            changed = (outPort != port);
        }
    }
    if (deactivated) {
        long n = 0;
        int clamped = port;
        if (parsePort(n)) {
            clamped = static_cast<int>(std::clamp<long>(n, 1024L, 65535L));
        }
        clamped = std::clamp(clamped, 1024, 65535);
        outPort = clamped;
        changed = (clamped != port);
        std::snprintf(buf.text, sizeof(buf.text), "%d", clamped);
        buf.synced = clamped;
    } else if (!active && buf.synced != port) {
        std::snprintf(buf.text, sizeof(buf.text), "%d", port);
        buf.synced = port;
    }
    return changed;
}

void drawIpMenuPopup(UiViewState& ui, upix::gui::AppState& app, const upix::gui::Snapshot& snap,
                     ImVec2 trig0, ImVec2 trig1, ImVec2 winSize) {
    using namespace upix::ui;
    if (!ui.showIpPicker || snap.host.ips.empty()) return;

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ui.showIpPicker = false;
        return;
    }

    const auto& t = geist::tokens();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    const float pad = 4.0f;
    const float rowH = 30.0f;
    const float gap = 2.0f;
    const size_t count = snap.host.ips.size();
    const float menuW = std::max(trig1.x - trig0.x, 216.0f);
    const float menuH = pad * 2.0f + rowH * static_cast<float>(count) + gap * static_cast<float>(count > 0 ? count - 1 : 0);

    float mx0 = trig1.x - menuW;
    float my0 = trig1.y + 6.0f;
    if (my0 + menuH > winSize.y - 12.0f && trig0.y - 6.0f - menuH >= 12.0f) {
        my0 = trig0.y - 6.0f - menuH;
    }
    const ImVec2 m0 = snapPt(mx0, my0);
    const ImVec2 m1 = snapPt(m0.x + menuW, m0.y + menuH);

    // Dismiss on outside click (excluding trigger box; trigger handles its own toggle).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ctl::mouseIn(m0, m1) && !ctl::mouseIn(trig0, trig1)) {
        ui.showIpPicker = false;
        return;
    }

    ctl::finish(fg, m0, m1, Finish::Menu, 8.0f, false);

    float ry = m0.y + pad;
    for (int i = 0; i < static_cast<int>(count); ++i) {
        const std::string& ip = snap.host.ips[static_cast<size_t>(i)];
        const bool isSel = (i == snap.host.selected_ip_idx);
        const ImVec2 r0 = snapPt(m0.x + pad, ry);
        const ImVec2 r1 = snapPt(m1.x - pad, ry + rowH);

        const bool hov = ctl::mouseIn(r0, r1);
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (isSel) {
            fg->AddRectFilled(r0, r1, t.accentSoft, geist::radius::MD);
        } else if (hov) {
            fg->AddRectFilled(r0, r1, t.high, geist::radius::MD);
        }

        const float midY = (r0.y + r1.y) * 0.5f;
        fg->AddCircleFilled(ImVec2(r0.x + 12.0f, midY), 2.5f, isSel ? t.accent : t.faint);

        const char* kind = ipKind(ip);
        const float kindW = kind[0] ? 28.0f : 0.0f;
        const float checkW = 14.0f;
        const float rightArea = checkW + 6.0f + kindW + (kind[0] ? 8.0f : 0.0f);
        const float ipMaxW = std::max(40.0f, (r1.x - 8.0f - rightArea) - (r0.x + 22.0f));
        const ImVec2 ipsz = calcText(ui.fontMono, geist::type::MONO, ip.c_str());
        ellipsisText(fg, ui.fontMono, geist::type::MONO, snapPt(r0.x + 22.0f, midY - ipsz.y * 0.5f),
                     ipMaxW, isSel ? t.ink : t.body, ip.c_str());

        if (isSel) {
            drawIcon(fg, "check", ImVec2(r1.x - 8.0f - checkW * 0.5f, midY), 11.0f, t.accent, 1.8f);
        }
        if (kind[0]) {
            const float kx1 = r1.x - 8.0f - checkW - 6.0f;
            const ImVec2 k0 = snapPt(kx1 - kindW, midY - 8.0f);
            const ImVec2 k1 = snapPt(kx1, midY + 8.0f);
            fg->AddRectFilled(k0, k1, t.tagBg, geist::radius::SM);
            drawTextCentered(fg, ui.fontMono, 10.0f, k0, k1, t.mute, kind);
        }

        if (hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            app.selectHostIp(i);
            ui.showIpPicker = false;
            break;
        }

        ry += rowH + gap;
    }
}

} // namespace

void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool modal = !snap.host.pending_device.empty();
    if (modal) ui.showIpPicker = false;

    const float availW = std::max(0.0f, winSize.x - kRailW);
    dl->AddRectFilled(ImVec2(kRailW, 0.0f), winSize, t.bg);

    const float colW = std::min(kColMax, std::max(320.0f, availW - 64.0f));
    const float colX = snapPx(kRailW + (availW - colW) * 0.5f);

    const ImGuiIO& io = ImGui::GetIO();
    const bool overPage = io.MousePos.x >= kRailW && io.MousePos.x < winSize.x &&
                          io.MousePos.y >= 0.0f && io.MousePos.y < winSize.y;
    bool wheelConsumed = false;

    dl->PushClipRect(ImVec2(kRailW, 0.0f), winSize, true);

    float y = kPagePad - gPageScroll;

    const bool running = snap.host.running;
    const std::string curIp = snap.host.ips.empty()
                                  ? std::string()
                                  : snap.host.ips[static_cast<size_t>(
                                        std::clamp(snap.host.selected_ip_idx, 0,
                                                   static_cast<int>(snap.host.ips.size()) - 1))];
    const std::string portText = std::to_string(snap.host.port);
    const auto groups = fpGroups(snap.host.fingerprint);

    ImVec2 ipTrig0{}, ipTrig1{};
    bool ipTrigDrawn = false;

    // 1. QR & Endpoint Card
    {
        const float pad = geist::space::LG;
        const float qr = 240.0f;
        const float cardH = pad + qr + pad;
        const ImVec2 c0 = snapPt(colX, y);
        const ImVec2 c1 = snapPt(colX + colW, y + cardH);
        ctl::groupCard(dl, c0, c1);

        const ImVec2 q0 = snapPt(c0.x + pad, c0.y + pad);
        const ImVec2 q1 = snapPt(q0.x + qr, q0.y + qr);
        drawQr(dl, q0, q1, snap.host.qr_payload, running && !snap.host.qr_payload.empty());

        const float infoX = snapPx(q1.x + geist::space::XXL);
        const float infoR = snapPx(c1.x - pad);
        const float infoW = std::max(0.0f, infoR - infoX);
        const float rowsH = 44.0f * 3.0f + hairlinePx() * 2.0f;
        const float actionH = 36.0f;
        const float blockH = rowsH + geist::space::XXL + actionH;
        float ry = snapPx(q0.y + (qr - blockH) * 0.5f);

        auto label = [&](const char* text, float rowY) {
            const ImVec2 ts = calcText(ui.fontSans, geist::type::LABEL, text);
            drawText(dl, ui.fontSans, geist::type::LABEL,
                     snapPt(infoX, rowY + (44.0f - ts.y) * 0.5f), t.mute, text);
        };
        auto divider = [&](float yy) {
            const float h = hairlinePx();
            ctl::hHairline(dl, infoX, infoR, yy, t.outline);
            return h;
        };

        // Row 1: 指纹 (4x4 mono pill badges + copy button with 1.5s check feedback)
        label("指纹", ry);
        {
            const float chipW = 44.0f;
            const float chipH = 24.0f;
            const float gap = 6.0f;
            const float copyBox = 24.0f;
            const float total = chipW * 4.0f + gap * 3.0f + geist::space::SM + copyBox;
            float x = infoR - total;
            const float cy = ry + (44.0f - chipH) * 0.5f;
            for (int i = 0; i < 4; ++i) {
                const ImVec2 a = snapPt(x, cy);
                const ImVec2 b = snapPt(x + chipW, cy + chipH);
                ctl::finish(dl, a, b, Finish::Inset, geist::radius::SM, false);
                dl->AddRect(a, b, t.outline, geist::radius::SM, 0, hairlinePx());
                drawTextCentered(dl, ui.fontMono, geist::type::MONO, a, b, t.ink, groups[static_cast<size_t>(i)].c_str());
                x += chipW + gap;
            }
            const ImVec2 b0 = snapPt(infoR - copyBox, ry + (44.0f - copyBox) * 0.5f);
            const bool copiedRecent = ImGui::GetTime() < gCopiedUntil;
            if (copiedRecent) {
                const ImVec2 b1 = snapPt(b0.x + copyBox, b0.y + copyBox);
                dl->AddRectFilled(b0, b1, t.accentSoft, geist::radius::MD);
                dl->AddRect(b0, b1, t.accentBorder, geist::radius::MD, 0, hairlinePx());
                drawIcon(dl, "check", ImVec2((b0.x + b1.x) * 0.5f, (b0.y + b1.y) * 0.5f), 13.0f, t.accent, 1.8f);
                const bool hov = ctl::mouseIn(b0, b1);
                ctl::tooltip(dl, ImGui::GetID("host.copyfp.tip"), b0, b1, "已复制", ui.fontSans, hov, false);
            } else {
                if (ctl::iconAction(dl, ImGui::GetID("host.copyfp"), b0, copyBox, "copy", "复制指纹",
                                    ui.fontSans, 13.0f, geist::radius::MD, !modal)) {
                    SDL_SetClipboardText(snap.host.fingerprint.c_str());
                    gCopiedUntil = ImGui::GetTime() + 1.5;
                }
            }
        }
        ry += 44.0f;
        ry += divider(ry);

        // Row 2: 地址 (interactive dropdown trigger + chevron-down + Finish::Menu popup)
        label("地址", ry);
        {
            const float boxW = std::min(228.0f, std::max(140.0f, infoW - 48.0f));
            const float boxH = 30.0f;
            const ImVec2 a = snapPt(infoR - boxW, ry + (44.0f - boxH) * 0.5f);
            const ImVec2 b = snapPt(a.x + boxW, a.y + boxH);
            ipTrig0 = a;
            ipTrig1 = b;
            ipTrigDrawn = true;

            const bool canPick = !modal && !snap.host.ips.empty();
            ImGui::SetCursorScreenPos(a);
            if (!canPick) ImGui::BeginDisabled();
            const bool trigClicked = ImGui::InvisibleButton("##host.ip_trigger", ImVec2(b.x - a.x, b.y - a.y));
            const bool trigHov = canPick && ImGui::IsItemHovered();
            const bool trigFocused = canPick && ImGui::IsItemFocused();
            if (!canPick) ImGui::EndDisabled();
            if (trigHov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (trigClicked && canPick) {
                ui.showIpPicker = !ui.showIpPicker;
            }

            ctl::finish(dl, a, b, Finish::Inset, geist::radius::MD, false);
            if (trigHov && !ui.showIpPicker) {
                ctl::hoverWash(dl, a, b, geist::radius::MD, true, 0.05f);
            }
            dl->AddRect(a, b, ui.showIpPicker ? t.accent : (trigHov ? t.outlineInput : t.outline),
                        geist::radius::MD, 0, hairlinePx());
            if (trigFocused && !ui.showIpPicker) {
                dl->AddRect(a, b, t.accent, geist::radius::MD, 0, hairlinePx());
            }

            const float midY = (a.y + b.y) * 0.5f;
            const ImU32 dot = running ? t.accent : t.mute;
            dl->AddCircleFilled(ImVec2(a.x + 12.0f, midY), 2.5f, dot);

            const float chevW = 12.0f;
            const char* kind = ipKind(curIp);
            const float kindW = kind[0] ? 28.0f : 0.0f;
            const float rightReserve = chevW + (kind[0] ? (6.0f + kindW) : 0.0f) + 10.0f;

            drawIcon(dl, ui.showIpPicker ? "chevron-up" : "chevron-down",
                     ImVec2(b.x - 10.0f - chevW * 0.5f, midY), 11.0f,
                     ui.showIpPicker ? t.ink : t.mute, 1.6f);

            if (kind[0]) {
                const float kx1 = b.x - 10.0f - chevW - 6.0f;
                const ImVec2 k0 = snapPt(kx1 - kindW, midY - 8.0f);
                const ImVec2 k1 = snapPt(kx1, midY + 8.0f);
                dl->AddRectFilled(k0, k1, t.container, geist::radius::SM);
                dl->AddRect(k0, k1, t.outline, geist::radius::SM, 0, hairlinePx());
                drawTextCentered(dl, ui.fontMono, 10.0f, k0, k1, t.mute, kind);
            }

            const char* shown = curIp.empty() ? "—" : curIp.c_str();
            const ImVec2 ts = calcText(ui.fontMono, geist::type::MONO, shown);
            const float ipMaxW = std::max(32.0f, (b.x - rightReserve - 6.0f) - (a.x + 22.0f));
            ellipsisText(dl, ui.fontMono, geist::type::MONO, snapPt(a.x + 22.0f, midY - ts.y * 0.5f),
                         ipMaxW, curIp.empty() ? t.faint : t.ink, shown);
        }
        ry += 44.0f;
        ry += divider(ry);

        // Row 3: 端口 (compact interactive InputText when idle, muted read-only when running)
        label("端口", ry);
        {
            const float boxW = 78.0f;
            const float boxH = 30.0f;
            const ImVec2 a = snapPt(infoR - boxW, ry + (44.0f - boxH) * 0.5f);
            const ImVec2 b = snapPt(a.x + boxW, a.y + boxH);
            int nextPort = snap.host.port;
            if (portInputField(dl, ImGui::GetID("host.port"), a, b, snap.host.port, !running && !modal,
                               ui.fontMono, nextPort)) {
                app.setHostPort(nextPort);
            }
        }
        ry += 44.0f;
        ry = snapPx(ry + geist::space::XXL);

        // Row 4: Footer status + Primary CTA / Stop button
        {
            const char* state = running ? "LIVE" : "IDLE";
            const ImU32 stateCol = running ? t.accent : t.mute;
            const float midY = ry + actionH * 0.5f;
            if (running) {
                dl->AddCircleFilled(ImVec2(infoX + 3.5f, midY), 3.0f, stateCol);
            } else {
                dl->AddCircle(ImVec2(infoX + 3.5f, midY), 3.0f, stateCol, 14, 1.4f);
            }
            const ImVec2 ss = calcText(ui.fontMono, geist::type::LABEL, state);
            drawText(dl, ui.fontMono, geist::type::LABEL, snapPt(infoX + 12.0f, midY - ss.y * 0.5f), stateCol, state);

            const char* btnLabel = running ? "停止服务" : "启动服务";
            const float btnW = ctaWidth(ui.fontSans, btnLabel, true);
            const ImVec2 b0 = snapPt(infoR - btnW, ry);
            const ImVec2 b1 = snapPt(infoR, ry + actionH);

            std::string subStatus = running ? (curIp.empty() ? portText : (curIp + ":" + portText)) : "服务未启动";
            const float ex = infoX + 12.0f + ss.x + geist::space::MD;
            const float maxSubW = std::max(0.0f, b0.x - geist::space::MD - ex);
            if (maxSubW > 20.0f) {
                ImFont* subFont = running ? ui.fontMono : ui.fontSans;
                const float subPx = running ? geist::type::MONO : geist::type::BODY_SMALL;
                const ImVec2 es = calcText(subFont, subPx, subStatus.c_str());
                ellipsisText(dl, subFont, subPx, snapPt(ex, midY - es.y * 0.5f),
                             maxSubW, running ? t.body : t.mute, subStatus.c_str());
            }

            if (running) {
                if (ctl::secondaryButton(dl, ImGui::GetID("host.stop"), btnLabel, ui.fontSans, b0, b1,
                                         "stop", !modal && !ui.showIpPicker, true)) {
                    app.hostStop();
                }
            } else {
                if (ctl::primaryCta(dl, ImGui::GetID("host.start"), btnLabel, ui.fontSans, b0, b1,
                                    !modal && !ui.showIpPicker, "play")) {
                    app.hostStart();
                }
            }
        }

        y += cardH + geist::space::SECTION;
    }

    // 2. Trusted devices card (always shown: clear button when populated, calm empty row when 0)
    {
        const float headerH = 40.0f;
        const float rowH = 44.0f;
        const float emptyH = 52.0f;
        const size_t nDev = snap.host.trusted_devices.size();
        const float bodyH = (nDev == 0) ? emptyH : rowH * static_cast<float>(nDev);
        const float tdH = headerH + bodyH;
        const ImVec2 t0 = snapPt(colX, y);
        const ImVec2 t1 = snapPt(colX + colW, y + tdH);
        const float pad = geist::space::LG;

        ctl::groupCard(dl, t0, t1);

        {
            const char* title = "已信任设备";
            const ImVec2 ts = calcText(ui.fontSans, geist::type::BODY, title);
            drawText(dl, ui.fontSans, geist::type::BODY,
                     snapPt(t0.x + pad, t0.y + (headerH - ts.y) * 0.5f), t.ink, title);
            const std::string n = std::to_string(nDev);
            const ImVec2 ns = calcText(ui.fontMono, geist::type::LABEL, n.c_str());
            drawText(dl, ui.fontMono, geist::type::LABEL,
                     snapPt(t0.x + pad + ts.x + geist::space::SM, t0.y + (headerH - ns.y) * 0.5f),
                     t.mute, n.c_str());

            if (nDev > 0) {
                const char* clrLabel = "清空";
                const ImVec2 cs = calcText(ui.fontSans, ctl::geom::LABEL, clrLabel);
                const float cw = cs.x + 2.0f * ctl::geom::GHOST_PX;
                const ImVec2 cp0 = snapPt(t1.x - pad - cw, t0.y + (headerH - ctl::geom::GHOST_H) * 0.5f);
                if (ctl::ghostButton(dl, ImGui::GetID("host.clear_trusted"), clrLabel, ui.fontSans,
                                     cp0, cw, !modal && !ui.showIpPicker)) {
                    app.clearTrustedDevices();
                }
            }
        }
        ctl::hHairline(dl, t0.x, t1.x, t0.y + headerH, t.outline);

        if (nDev == 0) {
            const char* emptyMsg = "暂无已信任设备 — 启动服务后使用手机端扫码配对";
            const ImVec2 es = calcText(ui.fontSans, geist::type::BODY_SMALL, emptyMsg);
            drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
                     snapPt(t0.x + (colW - es.x) * 0.5f, t0.y + headerH + (emptyH - es.y) * 0.5f),
                     t.mute, emptyMsg);
        } else {
            const float line = hairlinePx();
            float dy = t0.y + headerH;
            for (size_t i = 0; i < nDev; ++i) {
                const auto& d = snap.host.trusted_devices[i];
                const bool last = (i + 1 == nDev);
                const ImVec2 r0(t0.x, dy);
                const ImVec2 r1(t1.x, dy + rowH);
                if (ctl::mouseIn(r0, r1) && !modal && !ui.showIpPicker) {
                    const ImVec2 h0(t0.x + 1.0f, dy);
                    const ImVec2 h1(t1.x - 1.0f, last ? r1.y - 1.0f : r1.y - line);
                    if (last) {
                        dl->AddRectFilled(h0, h1, t.high, ctl::geom::CARD_R - 1.0f, ImDrawFlags_RoundCornersBottom);
                    } else {
                        dl->AddRectFilled(h0, h1, t.high);
                    }
                }

                const float midY = dy + rowH * 0.5f;
                const ImU32 dot = d.online ? t.accent : t.mute;
                dl->AddCircleFilled(ImVec2(t0.x + pad + 3.0f, midY), 3.0f, dot);

                const float btnBox = 26.0f;
                const ImVec2 tb0 = snapPt(t1.x - pad - btnBox, midY - btnBox * 0.5f);
                ImGui::PushID(static_cast<int>(i));
                if (ctl::iconAction(dl, ImGui::GetID("host.untrust"), tb0, btnBox, "trash", "移除设备",
                                    ui.fontSans, 13.5f, geist::radius::MD, !modal && !ui.showIpPicker)) {
                    app.removeTrustedDevice(d.fingerprint);
                }
                ImGui::PopID();

                const std::string fp = dottedFp(d.fingerprint);
                const ImVec2 fs = calcText(ui.fontMono, geist::type::MONO, fp.c_str());
                const float fx = tb0.x - geist::space::MD - fs.x;

                const char* badge = "已信任";
                const ImVec2 bs = calcText(ui.fontSans, geist::type::LABEL, badge);
                const float badgeW = bs.x + geist::space::SM * 2.0f;

                const float nameX = t0.x + pad + 16.0f;
                const float maxNameW = std::max(40.0f, fx - geist::space::MD - badgeW - geist::space::SM - nameX);
                const ImVec2 ns = calcText(ui.fontSans, geist::type::BODY, d.name.c_str());
                const std::string fittedName = fitTextEllipsis(ui.fontSans, geist::type::BODY, d.name.c_str(), maxNameW);
                const float drawnNameW = calcText(ui.fontSans, geist::type::BODY, fittedName.c_str()).x;
                drawText(dl, ui.fontSans, geist::type::BODY, snapPt(nameX, midY - ns.y * 0.5f),
                         t.ink, fittedName.c_str());

                const ImVec2 k0 = snapPt(nameX + drawnNameW + geist::space::SM, midY - 9.0f);
                const ImVec2 k1 = snapPt(k0.x + badgeW, midY + 9.0f);
                dl->AddRectFilled(k0, k1, t.container, geist::radius::SM);
                dl->AddRect(k0, k1, t.outline, geist::radius::SM, 0, hairlinePx());
                drawTextCentered(dl, ui.fontSans, geist::type::LABEL, k0, k1, t.mute, badge);

                if (fx > k1.x + geist::space::SM) {
                    drawText(dl, ui.fontMono, geist::type::MONO, snapPt(fx, midY - fs.y * 0.5f), t.mute, fp.c_str());
                }

                dy += rowH;
                if (!last) {
                    ctl::cardDivider(dl, t0.x, t1.x, dy);
                }
            }
        }
        y += tdH + geist::space::SECTION;
    }

    // 3. Remote Session / Service Log Card (shown when running or logs exist)
    if (running || !snap.host.logs.empty()) {
        const float pad = geist::space::LG;
        const float headerH = 40.0f;
        const float lineH = 24.0f;
        const float bodyH = 92.0f;
        const ImVec2 l0 = snapPt(colX, y);
        const ImVec2 l1 = snapPt(colX + colW, y + headerH + bodyH);
        ctl::groupCard(dl, l0, l1);

        const float midY = l0.y + headerH * 0.5f;
        const ImU32 sig = running ? t.accent : t.mute;
        dl->AddCircleFilled(ImVec2(l0.x + pad + 3.0f, midY), 3.0f, sig);
        const char* title = running ? "远程服务就绪" : "服务日志";
        const ImVec2 ts = calcText(ui.fontMono, geist::type::BODY_SMALL, title);
        drawText(dl, ui.fontMono, geist::type::BODY_SMALL,
                 snapPt(l0.x + pad + 14.0f, midY - ts.y * 0.5f), t.ink, title);

        float badgeR = l0.x + pad + 14.0f + ts.x;
        if (!snap.model.empty()) {
            const ImVec2 ms = calcText(ui.fontMono, geist::type::LABEL, snap.model.c_str());
            const ImVec2 m0 = snapPt(badgeR + geist::space::SM, midY - 9.0f);
            const ImVec2 m1 = snapPt(m0.x + ms.x + geist::space::SM * 2.0f, midY + 9.0f);
            dl->AddRectFilled(m0, m1, t.wellBg, geist::radius::SM);
            dl->AddRect(m0, m1, t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, geist::type::LABEL, m0, m1, t.mute, snap.model.c_str());
            badgeR = m1.x;
        }

        const bool havePct = snap.host.task_percent >= 0;
        const float frac = havePct ? std::clamp(snap.host.task_percent, 0, 100) / 100.0f
                                   : (running ? 1.0f : 0.0f);
        std::string pctStore;
        const char* pctLabel = nullptr;
        if (havePct) {
            pctStore = std::to_string(std::clamp(snap.host.task_percent, 0, 100)) + "%";
            pctLabel = pctStore.c_str();
        } else {
            pctLabel = running ? "LIVE" : "IDLE";
        }
        const ImVec2 ps = calcText(ui.fontMono, geist::type::LABEL, pctLabel);
        drawText(dl, ui.fontMono, geist::type::LABEL,
                 snapPt(l1.x - pad - ps.x, midY - ps.y * 0.5f), sig, pctLabel);

        const float barW = 136.0f;
        const float barR = l1.x - pad - ps.x - geist::space::MD;
        const float barX = barR - barW;
        if (barX > badgeR + geist::space::MD) {
            ctl::progressBar(dl, ImGui::GetID("host.taskbar"),
                             ImVec2(barX, midY - ctl::geom::PROGRESS_H * 0.5f), barW, frac, ctl::geom::PROGRESS_H);
        }
        ctl::hHairline(dl, l0.x, l1.x, l0.y + headerH, t.outline);

        const ImVec2 body0(l0.x, l0.y + headerH);
        const ImVec2 body1(l1.x, l1.y);
        const size_t totalLines = snap.host.logs.empty() ? 1 : snap.host.logs.size();
        const float totalLogH = 16.0f + static_cast<float>(totalLines) * lineH;
        const float maxLogScroll = std::max(0.0f, totalLogH - bodyH);
        if (!modal && !ui.showIpPicker && ctl::mouseIn(body0, body1) && io.MouseWheel != 0.0f && maxLogScroll > 0.0f) {
            gLogScroll = std::clamp(gLogScroll - io.MouseWheel * 24.0f, 0.0f, maxLogScroll);
            wheelConsumed = true;
        } else {
            gLogScroll = std::clamp(gLogScroll, 0.0f, maxLogScroll);
        }

        dl->PushClipRect(ImVec2(l0.x + pad, l0.y + headerH + 2.0f), ImVec2(l1.x - pad, l1.y - 2.0f), true);
        float ly = l0.y + headerH + geist::space::SM - gLogScroll;
        auto paintLine = [&](const std::string& tag, const std::string& msg, const std::string& note, bool task) {
            const float tagH = 16.0f;
            const ImVec2 ts2 = calcText(ui.fontMono, 10.0f, tag.c_str());
            const ImVec2 g0 = snapPt(l0.x + pad, ly + 2.0f);
            const ImVec2 g1 = snapPt(g0.x + ts2.x + 10.0f, g0.y + tagH);
            dl->AddRectFilled(g0, g1, task ? t.accentSoft : t.container, geist::radius::SM);
            dl->AddRect(g0, g1, task ? t.accentBorder : t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.0f, g0, g1, task ? t.accent : t.mute, tag.c_str());

            float rightEnd = l1.x - pad;
            if (!note.empty()) {
                const ImVec2 ns = calcText(ui.fontMono, geist::type::LABEL, note.c_str());
                drawText(dl, ui.fontMono, geist::type::LABEL,
                         snapPt(l1.x - pad - ns.x, g0.y + (tagH - ns.y) * 0.5f),
                         task ? t.accent : t.faint, note.c_str());
                rightEnd = l1.x - pad - ns.x - geist::space::MD;
            }
            const float msgX = g1.x + geist::space::SM;
            const float msgMaxW = std::max(24.0f, rightEnd - msgX);
            const ImVec2 ms = calcText(ui.fontSans, geist::type::LABEL, msg.c_str());
            ellipsisText(dl, ui.fontSans, geist::type::LABEL,
                         snapPt(msgX, g0.y + (tagH - ms.y) * 0.5f),
                         msgMaxW, task ? t.body : t.mute, msg.c_str());
            ly += lineH;
        };

        if (snap.host.logs.empty()) {
            paintLine("HOST", "等待手机连接…", "", false);
        } else {
            for (const std::string& raw : snap.host.logs) {
                const auto p1 = raw.find('|');
                if (p1 == std::string::npos) {
                    paintLine("HOST", raw, "", false);
                    continue;
                }
                const auto p2 = raw.find('|', p1 + 1);
                const auto p3 = p2 == std::string::npos ? std::string::npos : raw.find('|', p2 + 1);
                std::string tag = p2 == std::string::npos ? raw.substr(p1 + 1) : raw.substr(p1 + 1, p2 - p1 - 1);
                std::string msg;
                std::string note;
                if (p2 != std::string::npos) {
                    msg = p3 == std::string::npos ? raw.substr(p2 + 1) : raw.substr(p2 + 1, p3 - p2 - 1);
                    if (p3 != std::string::npos) note = raw.substr(p3 + 1);
                }
                paintLine(tag, msg, note, tag == "TASK");
            }
        }
        dl->PopClipRect();

        if (maxLogScroll > 0.0f) {
            const float thumbH = std::max(18.0f, (bodyH - 8.0f) * (bodyH / totalLogH));
            const float thumbY = l0.y + headerH + 4.0f + (gLogScroll / maxLogScroll) * (bodyH - 8.0f - thumbH);
            dl->AddRectFilled(snapPt(l1.x - 5.0f, thumbY), snapPt(l1.x - 2.0f, thumbY + thumbH),
                              withAlpha(t.faint, 0.45f), 1.5f);
        }

        y += headerH + bodyH;
    }

    dl->PopClipRect();

    gPageContentH = (y + gPageScroll) + kPagePad;
    if (!modal && !ui.showIpPicker && !wheelConsumed && overPage && io.MouseWheel != 0.0f) {
        gPageScroll -= io.MouseWheel * 48.0f;
    }
    gPageScroll = std::clamp(gPageScroll, 0.0f, std::max(0.0f, gPageContentH - winSize.y));

    if (gPageContentH > winSize.y && winSize.y > 24.0f) {
        const float trackH = winSize.y - 16.0f;
        const float thumbH = std::max(28.0f, trackH * (winSize.y / gPageContentH));
        const float maxP = std::max(1.0f, gPageContentH - winSize.y);
        const float frac = std::clamp(gPageScroll / maxP, 0.0f, 1.0f);
        const float thumbY = 8.0f + frac * (trackH - thumbH);
        dl->AddRectFilled(snapPt(winSize.x - 6.0f, thumbY), snapPt(winSize.x - 2.0f, thumbY + thumbH),
                          withAlpha(t.faint, 0.45f), 2.0f);
    }

    if (ipTrigDrawn && ui.showIpPicker && !modal) {
        drawIpMenuPopup(ui, app, snap, ipTrig0, ipTrig1, winSize);
    }
}

void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    if (snap.host.pending_device.empty()) return;
    using namespace upix::ui;
    const auto& t = geist::tokens();

    // Instant scrim + centered Geist Finish::Menu card (zero entrance fade/scale).
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), winSize, IM_COL32(0, 0, 0, 140));

    const float mw = 420.0f;
    const float mh = 256.0f;
    const ImVec2 m0 = snapPt((winSize.x - mw) * 0.5f, (winSize.y - mh) * 0.5f);
    const ImVec2 m1 = snapPt(m0.x + mw, m0.y + mh);

    // Click outside modal card on scrim denies via app.hostDecide(false).
    if (!ctl::mouseIn(m0, m1) && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        app.hostDecide(false);
        return;
    }

    ctl::finish(dl, m0, m1, Finish::Menu, geist::radius::XL, false);

    const float padX = 24.0f;
    const float innerL = m0.x + padX;
    const float innerR = m1.x - padX;
    const float innerW = innerR - innerL;

    const float iconBox = 32.0f;
    const float hy = m0.y + 20.0f;
    const ImVec2 i0 = snapPt(innerL, hy);
    const ImVec2 i1 = snapPt(innerL + iconBox, hy + iconBox);
    dl->AddRectFilled(i0, i1, t.accentSoft, geist::radius::MD);
    dl->AddRect(i0, i1, t.accentBorder, geist::radius::MD, 0, hairlinePx());
    drawIcon(dl, "smartphone", ImVec2((i0.x + i1.x) * 0.5f, (i0.y + i1.y) * 0.5f), 16.0f, t.accent, 1.6f);

    const char* heading = "新设备请求连接";
    const ImVec2 hsz = calcText(ui.fontSans, geist::type::BODY, heading);
    const float textX = i1.x + geist::space::MD;
    drawText(dl, ui.fontSans, geist::type::BODY, snapPt(textX, hy - 1.0f), t.ink, heading);

    const char* pairTag = "PAIR";
    const ImVec2 ptsz = calcText(ui.fontMono, 10.0f, pairTag);
    const ImVec2 p0 = snapPt(innerR - (ptsz.x + 12.0f), hy + 2.0f);
    const ImVec2 p1 = snapPt(innerR, hy + 20.0f);
    dl->AddRectFilled(p0, p1, t.tagBg, geist::radius::SM);
    dl->AddRect(p0, p1, t.outline, geist::radius::SM, 0, hairlinePx());
    drawTextCentered(dl, ui.fontMono, 10.0f, p0, p1, t.mute, pairTag);

    const float devMaxW = std::max(40.0f, p0.x - 8.0f - textX);
    const ImVec2 dsz = calcText(ui.fontMono, geist::type::BODY_SMALL, snap.host.pending_device.c_str());
    ellipsisText(dl, ui.fontMono, geist::type::BODY_SMALL,
                 snapPt(textX, hy + iconBox - dsz.y + 1.0f), devMaxW, t.body, snap.host.pending_device.c_str());
    (void)hsz;

    const char* body = "核对手机端显示的指纹与下方一致，再决定是否允许连接。";
    const ImVec2 bodySz = calcText(ui.fontSans, geist::type::BODY_SMALL, body);
    const float bodyY = hy + iconBox + geist::space::MD;
    ellipsisText(dl, ui.fontSans, geist::type::BODY_SMALL, snapPt(innerL, bodyY), innerW, t.mute, body);

    const float wellY = bodyY + bodySz.y + geist::space::MD;
    const float wellH = 82.0f;
    const ImVec2 w0 = snapPt(innerL, wellY);
    const ImVec2 w1 = snapPt(innerR, wellY + wellH);
    ctl::finish(dl, w0, w1, Finish::Inset, geist::radius::LG, false);
    dl->AddRect(w0, w1, t.outline, geist::radius::LG, 0, hairlinePx());

    const char* hint = "设备指纹核对";
    const ImVec2 hintSz = calcText(ui.fontSans, geist::type::LABEL, hint);
    drawTextCentered(dl, ui.fontSans, geist::type::LABEL,
                     ImVec2(w0.x, w0.y + 12.0f), ImVec2(w1.x, w0.y + 12.0f + hintSz.y), t.mute, hint);

    const auto groups = fpGroups(snap.host.pending_fp);
    const float pillW = 68.0f;
    const float pillH = 28.0f;
    const float pillGap = 10.0f;
    const float totalPillsW = pillW * 4.0f + pillGap * 3.0f;
    float gx = snapPx(w0.x + (innerW - totalPillsW) * 0.5f);
    const float gy = snapPx(w0.y + 38.0f);
    for (int i = 0; i < 4; ++i) {
        const ImVec2 g0 = snapPt(gx, gy);
        const ImVec2 g1 = snapPt(gx + pillW, gy + pillH);
        dl->AddRectFilled(g0, g1, t.container, geist::radius::MD);
        dl->AddRect(g0, g1, t.outline, geist::radius::MD, 0, hairlinePx());
        drawTextCentered(dl, ui.fontMono, 14.0f, g0, g1, t.ink, groups[static_cast<size_t>(i)].c_str());
        gx += pillW + pillGap;
    }

    const float btnH = 38.0f;
    const float btnY = m1.y - 20.0f - btnH;
    const float denyW = 100.0f;
    const float allowW = 116.0f;
    const ImVec2 a0 = snapPt(innerR - allowW, btnY);
    const ImVec2 a1 = snapPt(innerR, btnY + btnH);
    const ImVec2 d1 = snapPt(a0.x - geist::space::SM, btnY + btnH);
    const ImVec2 d0 = snapPt(d1.x - denyW, btnY);

    if (ctl::secondaryButton(dl, ImGui::GetID("pair.deny"), "拒绝 (Esc)", ui.fontSans, d0, d1,
                             nullptr, true, false)) {
        app.hostDecide(false);
    }
    if (ctl::primaryCta(dl, ImGui::GetID("pair.allow"), "允许连接", ui.fontSans, a0, a1,
                        true, "check")) {
        app.hostDecide(true);
    }
}

} // namespace upix::views
