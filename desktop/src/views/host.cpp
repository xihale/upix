#include "views/common.hpp"

#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace upix::views {
namespace {

using upix::ui::ctl::Finish;

// Local stand-ins for primitives the foundation headers don't expose yet.
// Rules match ui/controls.hpp + DESIGN.md §1.3 / §2.2 / §2.4.

// ctl::primaryCta centers a label only, and its hover wash is painted under
// the resting ink fill (so hover never shows). This draws the same ink CTA —
// Primary → PrimaryHover → PrimaryPress, 1px sink, never a scale — with an
// optional leading icon.
bool inkCta(ImDrawList* dl, ImGuiID /*id*/, const char* label, const char* icon,
            ImFont* font, ImVec2 p0, ImVec2 p1) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const bool hov = ctl::mouseIn(p0, p1);
    const bool pressed = hov && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const ImVec4 face = ctl::finish(dl, p0, p1, Finish::Primary, geist::radius::MD, pressed);
    if (!pressed && hov) {
        dl->AddRectFilled(ImVec2(face.x, face.y), ImVec2(face.z, face.w), t.primaryHover, geist::radius::MD);
    }
    const float labelPx = geist::type::BODY;
    const ImVec2 ts = calcText(font, labelPx, label);
    const float iconSz = (icon && icon[0]) ? 14.0f : 0.0f;
    const float gap = iconSz > 0.0f ? geist::space::SM : 0.0f;
    const float total = iconSz + gap + ts.x;
    const float x = face.x + ((face.z - face.x) - total) * 0.5f;
    const float midY = (face.y + face.w) * 0.5f;
    if (iconSz > 0.0f) {
        // CJK ink sits low in the em box; nudge the glyph so it doesn't float.
        drawIcon(dl, icon, ImVec2(x + iconSz * 0.5f, midY + 1.0f), iconSz, t.onPrimary, 1.7f);
    }
    drawText(dl, font, labelPx, snapPt(x + iconSz + gap, midY - ts.y * 0.5f), t.onPrimary, label);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Destructive quiet: errorSoft fill + errorBorder hairline; error ink on hover.
// Text label → press spring 0.97 (§1.4). No such primitive in controls.hpp.
bool quietStop(ImDrawList* dl, ImGuiID id, const char* label, ImFont* font, ImVec2 p0, ImVec2 p1) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const bool hov = ctl::mouseIn(p0, p1);
    const bool pressed = hov && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float sc = motion::pressScale(id, pressed, motion::PRESS_SCALE);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    auto xf = [&](ImVec2 p) {
        return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
    };
    const ImVec2 a = xf(p0);
    const ImVec2 b = xf(p1);
    dl->AddRectFilled(a, b, t.errorSoft, geist::radius::MD);
    dl->AddRect(a, b, t.errorBorder, geist::radius::MD, 0, 1.0f);
    const ImU32 ink = hov ? t.error : t.body;
    const float labelPx = geist::type::BODY;
    const ImVec2 ts = calcText(font, labelPx, label);
    const float iconSz = 12.0f;
    const float gap = geist::space::SM;
    const float total = iconSz + gap + ts.x;
    const float x = a.x + ((b.x - a.x) - total) * 0.5f;
    const float midY = (a.y + b.y) * 0.5f;
    drawIcon(dl, "stop", ImVec2(x + iconSz * 0.5f, midY), iconSz, ink, 1.6f);
    drawText(dl, font, labelPx, snapPt(x + iconSz + gap, midY - ts.y * 0.5f), ink, label);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Pure-icon control: 0.94 spring, hover wash INSTANT (§2.3 icon buttons).
// `danger` swaps the wash for errorSoft + error glyph (quiet remove).
bool iconPress(ImDrawList* dl, ImGuiID id, const char* icon, ImVec2 p0, ImVec2 p1,
               float iconSz, bool danger) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const bool hov = ctl::mouseIn(p0, p1);
    const bool pressed = hov && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float sc = motion::pressScale(id, pressed, motion::PRESS_SCALE_ICON);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    auto xf = [&](ImVec2 p) {
        return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
    };
    const ImVec2 a = xf(p0);
    const ImVec2 b = xf(p1);
    if (hov) {
        if (danger) {
            dl->AddRectFilled(a, b, t.errorSoft, geist::radius::MD);
            dl->AddRect(a, b, t.errorBorder, geist::radius::MD, 0, 1.0f);
        } else {
            ctl::finish(dl, a, b, Finish::RaisedHover, geist::radius::MD, false);
        }
    }
    const ImU32 ink = (danger && hov) ? t.error : (hov ? t.ink : t.mute);
    drawIcon(dl, icon, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), iconSz, ink, 1.6f);
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

// Solid ⇄ RaisedHover, press 0.97. Two labels (word + kbd) centered as a group.
bool solidButton(ImDrawList* dl, ImGuiID id, const char* label, const char* kbd,
                 ImFont* font, ImFont* mono, ImVec2 p0, ImVec2 p1) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    const bool hov = ctl::mouseIn(p0, p1);
    const bool pressed = hov && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    const float sc = motion::pressScale(id, pressed, motion::PRESS_SCALE);
    const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    auto xf = [&](ImVec2 p) {
        return ImVec2(c.x + (p.x - c.x) * sc, c.y + (p.y - c.y) * sc);
    };
    const ImVec2 a = xf(p0);
    const ImVec2 b = xf(p1);
    ctl::finish(dl, a, b, hov ? Finish::RaisedHover : Finish::Solid, geist::radius::MD, false);
    const ImVec2 ls = calcText(font, geist::type::BODY, label);
    const ImVec2 ks = kbd ? calcText(mono, geist::type::LABEL, kbd) : ImVec2(0, 0);
    const float gap = kbd ? 6.0f : 0.0f;
    const float total = ls.x + gap + ks.x;
    const float x = a.x + ((b.x - a.x) - total) * 0.5f;
    const float midY = (a.y + b.y) * 0.5f;
    drawText(dl, font, geist::type::BODY, snapPt(x, midY - ls.y * 0.5f), t.ink, label);
    if (kbd) {
        drawText(dl, mono, geist::type::LABEL, snapPt(x + ls.x + gap, midY - ks.y * 0.5f), t.faint, kbd);
    }
    return hov && ImGui::IsMouseReleased(ImGuiMouseButton_Left);
}

std::string hexDigits(const std::string& fp) {
    std::string out;
    out.reserve(fp.size());
    for (size_t i = 0; i < fp.size();) {
        const unsigned char c = static_cast<unsigned char>(fp[i]);
        if (c < 0x80) {
            if (std::isxdigit(c)) out.push_back(static_cast<char>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < fp.size()) {
            i += 2; // separators such as U+00B7 MIDDLE DOT
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

// Four scan groups. Never invent a fingerprint when the key isn't ready
// (rust split_fp_groups): placeholders cannot be "verified".
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
    return g[0] + "  \u00B7  " + g[1] + "  \u00B7  " + g[2] + "  \u00B7  " + g[3];
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
    const float text = calcText(font, geist::type::BODY, label).x;
    const float icon = withIcon ? 14.0f + geist::space::SM : 0.0f;
    const float w = std::ceil((text + icon + geist::space::LG * 2.0f) / 4.0f) * 4.0f;
    return std::max(96.0f, w);
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
            const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::LOW);
            const int qsz = qr.getSize();
            const float scale = currentDpiScale() > 0.0f ? currentDpiScale() : 1.0f;
            float cellPx = std::floor(176.0f * scale / static_cast<float>(std::max(qsz, 1)));
            if (cellPx < 1.0f) cellPx = 1.0f;
            const float cell = cellPx / scale;
            const float real = cell * static_cast<float>(qsz);
            const float paper = std::max(real + 16.0f, 192.0f);
            const ImVec2 paper0 = snapPt(well0.x + (240.0f - paper) * 0.5f, well0.y + (240.0f - paper) * 0.5f);
            const ImVec2 paper1 = snapPt(paper0.x + paper, paper0.y + paper);
            dl->AddRectFilled(paper0, paper1, t.qrPaper, geist::radius::SM + 4.0f);
            // Quiet frame: a hairline, not a heavy stroke. Code itself is pure paper / ink.
            dl->AddRect(paper0, paper1, IM_COL32(0, 0, 0, 28), geist::radius::SM + 4.0f, 0, hairlinePx());
            const float ox = paper0.x + (paper - real) * 0.5f;
            const float oy = paper0.y + (paper - real) * 0.5f;
            for (int qy = 0; qy < qsz; ++qy) {
                for (int qx = 0; qx < qsz; ++qx) {
                    if (!qr.getModule(qx, qy)) continue;
                    const ImVec2 m0 = snapPt(ox + qx * cell, oy + qy * cell);
                    const ImVec2 m1 = snapPt(ox + (qx + 1) * cell, oy + (qy + 1) * cell);
                    dl->AddRectFilled(m0, m1, t.qrInk);
                }
            }
        } catch (...) {
        }
        return;
    }

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

} // namespace

void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                      upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const bool modal = !snap.host.pending_device.empty();

    const float railW = 76.0f;
    const float availW = std::max(0.0f, winSize.x - railW);
    dl->AddRectFilled(ImVec2(railW, 0.0f), winSize, t.bg);

    const float colW = std::min(712.0f, std::max(320.0f, availW - geist::space::XXL * 2.0f));
    const float colX = snapPx(railW + (availW - colW) * 0.5f);
    float y = geist::space::XXL;

    const bool running = snap.host.running;
    const std::string curIp = snap.host.ips.empty()
                                  ? std::string()
                                  : snap.host.ips[static_cast<size_t>(
                                        std::clamp(snap.host.selected_ip_idx, 0,
                                                   static_cast<int>(snap.host.ips.size()) - 1))];
    const std::string portText = std::to_string(snap.host.port);
    const auto groups = fpGroups(snap.host.fingerprint);

    // --- Pairing card. QR well is 240; the info column is content-sized and
    // centered against it (rust pairing_card). Pad 16, sections below at 24.
    const float pad = geist::space::LG;
    const float qr = 240.0f;
    const float cardH = pad + qr + pad;
    const ImVec2 c0 = snapPt(colX, y);
    const ImVec2 c1 = snapPt(colX + colW, y + cardH);
    ctl::finish(dl, c0, c1, Finish::Card, geist::radius::LG, false);

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
        dl->AddRectFilled(snapPt(infoX, yy), snapPt(infoR, yy + h), t.outline);
        return h;
    };

    // 指纹 — inset chips, mono, spaced in 4-char groups.
    label("指纹", ry);
    {
        const float chipW = 44.0f;
        const float chipH = 24.0f;
        const float gap = 6.0f;
        const float copy = 24.0f;
        const float total = chipW * 4.0f + gap * 3.0f + geist::space::SM + copy;
        float x = infoR - total;
        const float cy = ry + (44.0f - chipH) * 0.5f;
        for (int i = 0; i < 4; ++i) {
            const ImVec2 a = snapPt(x, cy);
            const ImVec2 b = snapPt(x + chipW, cy + chipH);
            ctl::finish(dl, a, b, Finish::Inset, geist::radius::SM, false);
            drawTextCentered(dl, ui.fontMono, geist::type::MONO, a, b, t.ink, groups[static_cast<size_t>(i)].c_str());
            x += chipW + gap;
        }
        const ImVec2 b0 = snapPt(infoR - copy, ry + (44.0f - copy) * 0.5f);
        if (!modal && iconPress(dl, ImGui::GetID("host.copyfp"), "copy", b0,
                                ImVec2(b0.x + copy, b0.y + copy), 13.0f, false)) {
            SDL_SetClipboardText(snap.host.fingerprint.c_str());
        }
    }
    ry += 44.0f;
    ry += divider(ry);

    // 地址 — read-only inset. Kind badge is derived from the address, not new state.
    // IP picker isn't wired: the view never called selectHostIp (no open-flag on UiViewState).
    label("地址", ry);
    {
        const float boxW = std::min(216.0f, std::max(120.0f, infoW - 48.0f));
        const float boxH = 30.0f;
        const ImVec2 a = snapPt(infoR - boxW, ry + (44.0f - boxH) * 0.5f);
        const ImVec2 b = snapPt(a.x + boxW, a.y + boxH);
        ctl::finish(dl, a, b, Finish::Inset, geist::radius::MD, false);
        const float midY = (a.y + b.y) * 0.5f;
        const ImU32 dot = running ? t.accent : t.mute;
        dl->AddCircleFilled(ImVec2(a.x + 12.0f, midY), 2.5f, dot);
        const char* kind = ipKind(curIp);
        const float kindW = kind[0] ? 32.0f : 0.0f;
        if (kind[0]) {
            const ImVec2 k0 = snapPt(b.x - 8.0f - kindW, midY - 9.0f);
            const ImVec2 k1 = snapPt(k0.x + kindW, midY + 9.0f);
            dl->AddRectFilled(k0, k1, t.container, geist::radius::SM);
            drawTextCentered(dl, ui.fontMono, geist::type::LABEL, k0, k1, t.mute, kind);
        }
        const char* shown = curIp.empty() ? "—" : curIp.c_str();
        const ImVec2 ts = calcText(ui.fontMono, geist::type::MONO, shown);
        drawText(dl, ui.fontMono, geist::type::MONO,
                 snapPt(a.x + 22.0f, midY - ts.y * 0.5f), curIp.empty() ? t.faint : t.ink, shown);
    }
    ry += 44.0f;
    ry += divider(ry);

    // 端口 — locked while the listener (and the QR) would disagree with a new value.
    label("端口", ry);
    {
        const float boxW = 84.0f;
        const float boxH = 30.0f;
        const ImVec2 a = snapPt(infoR - boxW, ry + (44.0f - boxH) * 0.5f);
        const ImVec2 b = snapPt(a.x + boxW, a.y + boxH);
        ctl::finish(dl, a, b, Finish::Inset, geist::radius::MD, false);
        const ImVec2 colon = calcText(ui.fontMono, geist::type::MONO, ":");
        const float midY = (a.y + b.y) * 0.5f;
        drawText(dl, ui.fontMono, geist::type::MONO, snapPt(a.x + 10.0f, midY - colon.y * 0.5f), t.faint, ":");
        const ImU32 ink = running ? t.mute : t.ink;
        const ImVec2 ts = calcText(ui.fontMono, geist::type::MONO, portText.c_str());
        drawText(dl, ui.fontMono, geist::type::MONO, snapPt(a.x + 22.0f, midY - ts.y * 0.5f), ink, portText.c_str());
    }
    ry += 44.0f;
    ry = snapPx(ry + geist::space::XXL);

    // Status row: dot + state word. Color swaps INSTANT (§2.4, no mix).
    {
        const char* state = running ? "LIVE" : "IDLE";
        const ImU32 stateCol = running ? t.accent : t.mute;
        const float midY = ry + actionH * 0.5f;
        dl->AddCircleFilled(ImVec2(infoX + 3.0f, midY), 3.0f, stateCol);
        const ImVec2 ss = calcText(ui.fontMono, geist::type::LABEL, state);
        drawText(dl, ui.fontMono, geist::type::LABEL, snapPt(infoX + 12.0f, midY - ss.y * 0.5f), stateCol, state);

        std::string endpoint;
        if (!curIp.empty()) endpoint = curIp + ":" + portText;
        const char* btnLabel = running ? "停止服务" : "启动服务";
        const float btnW = ctaWidth(ui.fontSans, btnLabel, true);
        const ImVec2 b0 = snapPt(infoR - btnW, ry);
        const ImVec2 b1 = snapPt(infoR, ry + actionH);
        if (!endpoint.empty()) {
            const ImVec2 es = calcText(ui.fontMono, geist::type::MONO, endpoint.c_str());
            const float ex = infoX + 12.0f + ss.x + geist::space::MD;
            if (ex + es.x < b0.x - geist::space::MD) {
                drawText(dl, ui.fontMono, geist::type::MONO, snapPt(ex, midY - es.y * 0.5f), t.body, endpoint.c_str());
            }
        }
        if (!modal) {
            if (running) {
                if (quietStop(dl, ImGui::GetID("host.stop"), btnLabel, ui.fontSans, b0, b1)) app.hostStop();
            } else if (inkCta(dl, ImGui::GetID("host.start"), btnLabel, "play", ui.fontSans, b0, b1)) {
                app.hostStart();
            }
        } else if (running) {
            quietStop(dl, ImGui::GetID("host.stop"), btnLabel, ui.fontSans, b0, b1);
        } else {
            inkCta(dl, ImGui::GetID("host.start"), btnLabel, "play", ui.fontSans, b0, b1);
        }
    }

    y += cardH + geist::space::SECTION;

    // --- Trusted devices. Absent when the snapshot has none (don't invent rows).
    if (!snap.host.trusted_devices.empty()) {
        const float headerH = 40.0f;
        const float rowH = 44.0f;
        const float tdH = headerH + rowH * static_cast<float>(snap.host.trusted_devices.size());
        const ImVec2 t0 = snapPt(colX, y);
        const ImVec2 t1 = snapPt(colX + colW, y + tdH);
        ctl::finish(dl, t0, t1, Finish::Card, geist::radius::LG, false);
        dl->PushClipRect(t0, t1, true);

        {
            const char* title = "已信任设备";
            const ImVec2 ts = calcText(ui.fontSans, geist::type::BODY, title);
            drawText(dl, ui.fontSans, geist::type::BODY,
                     snapPt(t0.x + pad, t0.y + (headerH - ts.y) * 0.5f), t.ink, title);
            const std::string n = std::to_string(snap.host.trusted_devices.size());
            const ImVec2 ns = calcText(ui.fontMono, geist::type::LABEL, n.c_str());
            drawText(dl, ui.fontMono, geist::type::LABEL,
                     snapPt(t0.x + pad + ts.x + geist::space::SM, t0.y + (headerH - ns.y) * 0.5f),
                     t.mute, n.c_str());
        }
        dl->AddRectFilled(snapPt(t0.x, t0.y + headerH),
                          snapPt(t1.x, t0.y + headerH + hairlinePx()), t.outline);

        const float line = hairlinePx();
        float dy = t0.y + headerH;
        const size_t nDev = snap.host.trusted_devices.size();
        for (size_t i = 0; i < nDev; ++i) {
            const auto& d = snap.host.trusted_devices[i];
            const bool last = i + 1 == nDev;
            const ImVec2 r0(t0.x, dy);
            const ImVec2 r1(t1.x, dy + rowH);
            const bool hov = ctl::mouseIn(r0, r1);
            if (hov) {
                // Inside the card hairline. Last row keeps the bottom radius.
                const ImVec2 h0(t0.x + 1.0f, dy);
                const ImVec2 h1(t1.x - 1.0f, last ? r1.y - 1.0f : r1.y - line);
                if (last) {
                    dl->AddRectFilled(h0, h1, t.high, geist::radius::LG - 1.0f, ImDrawFlags_RoundCornersBottom);
                } else {
                    dl->AddRectFilled(h0, h1, t.high);
                }
            }

            const float midY = dy + rowH * 0.5f;
            const ImU32 dot = d.online ? t.accent : t.mute;
            dl->AddCircleFilled(ImVec2(t0.x + pad + 3.0f, midY), 3.0f, dot);

            const float nameX = t0.x + pad + 16.0f;
            const ImVec2 ns = calcText(ui.fontSans, geist::type::BODY, d.name.c_str());
            drawText(dl, ui.fontSans, geist::type::BODY, snapPt(nameX, midY - ns.y * 0.5f), t.ink, d.name.c_str());

            const char* badge = "已信任";
            const ImVec2 bs = calcText(ui.fontSans, geist::type::LABEL, badge);
            const ImVec2 k0 = snapPt(nameX + ns.x + geist::space::SM, midY - 9.0f);
            const ImVec2 k1 = snapPt(k0.x + bs.x + geist::space::SM * 2.0f, midY + 9.0f);
            dl->AddRectFilled(k0, k1, t.container, geist::radius::SM);
            dl->AddRect(k0, k1, t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontSans, geist::type::LABEL, k0, k1, t.mute, badge);

            const float btn = 28.0f;
            const ImVec2 tb0 = snapPt(t1.x - pad - btn, midY - btn * 0.5f);
            const ImVec2 tb1 = snapPt(tb0.x + btn, tb0.y + btn);
            ImGui::PushID(static_cast<int>(i));
            // No removeTrusted on AppState — the control is drawn, the click is not invented.
            iconPress(dl, ImGui::GetID("host.untrust"), "trash", tb0, tb1, 13.5f, true);
            ImGui::PopID();

            const std::string fp = dottedFp(d.fingerprint);
            const ImVec2 fs = calcText(ui.fontMono, geist::type::MONO, fp.c_str());
            const float fx = tb0.x - geist::space::MD - fs.x;
            if (fx > k1.x + geist::space::SM) {
                drawText(dl, ui.fontMono, geist::type::MONO, snapPt(fx, midY - fs.y * 0.5f), t.mute, fp.c_str());
            }

            dy += rowH;
            if (!last) {
                dl->AddRectFilled(snapPt(t0.x + pad, dy - line), snapPt(t1.x - pad, dy), t.outline);
            }
        }
        dl->PopClipRect();
        y += tdH + geist::space::SECTION;
    }

    // --- Service log. Only once the listener is up or a line exists.
    if (running || !snap.host.logs.empty()) {
        const float headerH = 40.0f;
        const float bodyH = 84.0f;
        const ImVec2 l0 = snapPt(colX, y);
        const ImVec2 l1 = snapPt(colX + colW, y + headerH + bodyH);
        ctl::finish(dl, l0, l1, Finish::Card, geist::radius::LG, false);

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

        // Binary LIVE fill is the current value, not an entrance. progressSpring
        // cold-starts at 0, so a 3-frame --shot would show an empty bar (§1.3).
        const bool havePct = snap.host.task_percent >= 0;
        const float frac = havePct ? std::clamp(snap.host.task_percent, 0, 100) / 100.0f
                                   : (running ? 1.0f : 0.0f);
        const char* pctLabel = nullptr;
        std::string pctStore;
        if (havePct) {
            pctStore = std::to_string(snap.host.task_percent) + "%";
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
            const float by = snapPx(midY - 2.0f);
            dl->AddRectFilled(ImVec2(barX, by), ImVec2(barR, by + 4.0f), t.high, 2.0f);
            if (frac > 0.0f) {
                dl->AddRectFilled(ImVec2(barX, by), ImVec2(barX + barW * frac, by + 4.0f), t.accent, 2.0f);
            }
        }
        dl->AddRectFilled(snapPt(l0.x, l0.y + headerH),
                          snapPt(l1.x, l0.y + headerH + hairlinePx()), t.outline);

        dl->PushClipRect(ImVec2(l0.x + pad, l0.y + headerH), ImVec2(l1.x - pad, l1.y), true);
        float ly = l0.y + headerH + geist::space::SM;
        auto paintLine = [&](const std::string& tag, const std::string& msg, const std::string& note, bool task) {
            const float tagH = 16.0f;
            const ImVec2 ts2 = calcText(ui.fontMono, 10.0f, tag.c_str());
            const ImVec2 g0 = snapPt(l0.x + pad, ly + 2.0f);
            const ImVec2 g1 = snapPt(g0.x + ts2.x + 10.0f, g0.y + tagH);
            dl->AddRectFilled(g0, g1, task ? t.accentSoft : t.container, geist::radius::SM);
            dl->AddRect(g0, g1, task ? t.accentBorder : t.outline, geist::radius::SM, 0, hairlinePx());
            drawTextCentered(dl, ui.fontMono, 10.0f, g0, g1, task ? t.accent : t.mute, tag.c_str());
            const ImVec2 ms = calcText(ui.fontSans, geist::type::LABEL, msg.c_str());
            drawText(dl, ui.fontSans, geist::type::LABEL,
                     snapPt(g1.x + geist::space::SM, g0.y + (tagH - ms.y) * 0.5f),
                     task ? t.body : t.mute, msg.c_str());
            if (!note.empty()) {
                const ImVec2 ns = calcText(ui.fontMono, geist::type::LABEL, note.c_str());
                drawText(dl, ui.fontMono, geist::type::LABEL,
                         snapPt(l1.x - pad - ns.x, g0.y + (tagH - ns.y) * 0.5f),
                         task ? t.accent : t.faint, note.c_str());
            }
            ly += 24.0f;
        };
        if (snap.host.logs.empty()) {
            paintLine("HOST", "等待手机连接…", "", false);
        } else {
            const size_t n = snap.host.logs.size();
            const size_t begin = n > 3 ? n - 3 : 0;
            for (size_t i = begin; i < n; ++i) {
                const std::string& raw = snap.host.logs[i];
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
    }
}

void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                        upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    if (snap.host.pending_device.empty()) return;
    using namespace upix::ui;
    const auto& t = geist::tokens();

    // Instant: scrim + menu card in the same frame. No fade, scale, or slide (§2.1 / §2.4).
    dl->AddRectFilled(ImVec2(0, 0), winSize, IM_COL32(0, 0, 0, 122));

    const float mw = 380.0f;
    const float mh = 240.0f;
    const ImVec2 m0 = snapPt((winSize.x - mw) * 0.5f, (winSize.y - mh) * 0.5f);
    const ImVec2 m1 = snapPt(m0.x + mw, m0.y + mh);
    ctl::finish(dl, m0, m1, Finish::Menu, geist::radius::XL, false);

    const float padX = 20.0f;
    const float innerL = m0.x + padX;
    const float innerR = m1.x - padX;
    const char* name = snap.host.pending_device.c_str();
    const ImVec2 nameSz = calcText(ui.fontSans, geist::type::BODY, name);
    const float pairW = 40.0f;
    const float pairH = 18.0f;
    const float iconBox = 28.0f;
    const float headW = iconBox + geist::space::SM + nameSz.x + geist::space::SM + pairW;
    float hx = snapPx(m0.x + (mw - headW) * 0.5f);
    const float hy = m0.y + 20.0f;

    const ImVec2 i0 = snapPt(hx, hy);
    const ImVec2 i1 = snapPt(hx + iconBox, hy + iconBox);
    dl->AddRectFilled(i0, i1, t.accentSoft, geist::radius::MD);
    dl->AddRect(i0, i1, t.accentBorder, geist::radius::MD, 0, hairlinePx());
    drawIcon(dl, "smartphone", ImVec2(i0.x + iconBox * 0.5f, i0.y + iconBox * 0.5f), 15.0f, t.accent, 1.6f);

    const float nameX = i1.x + geist::space::SM;
    drawText(dl, ui.fontSans, geist::type::BODY,
             snapPt(nameX, hy + (iconBox - nameSz.y) * 0.5f), t.ink, name);

    const ImVec2 p0 = snapPt(nameX + nameSz.x + geist::space::SM, hy + (iconBox - pairH) * 0.5f);
    const ImVec2 p1 = snapPt(p0.x + pairW, p0.y + pairH);
    dl->AddRectFilled(p0, p1, t.high, geist::radius::SM);
    dl->AddRect(p0, p1, t.outline, geist::radius::SM, 0, hairlinePx());
    drawTextCentered(dl, ui.fontMono, geist::type::LABEL, p0, p1, t.mute, "PAIR");

    const char* body = "核对设备指纹与手机屏幕一致，再决定是否允许连接。";
    const ImVec2 bodySz = calcText(ui.fontSans, geist::type::BODY_SMALL, body);
    drawText(dl, ui.fontSans, geist::type::BODY_SMALL,
             snapPt(innerL, hy + iconBox + geist::space::SM + (16.0f - bodySz.y) * 0.5f), t.body, body);

    const float wellY = hy + iconBox + geist::space::SM + 16.0f + geist::space::MD;
    const float wellH = 84.0f;
    const ImVec2 w0 = snapPt(innerL, wellY);
    const ImVec2 w1 = snapPt(innerR, wellY + wellH);
    ctl::finish(dl, w0, w1, Finish::Inset, geist::radius::LG, false);
    const char* hint = "核对设备指纹一致";
    const ImVec2 hintSz = calcText(ui.fontSans, geist::type::LABEL, hint);
    drawTextCentered(dl, ui.fontSans, geist::type::LABEL,
                     ImVec2(w0.x, w0.y + 12.0f), ImVec2(w1.x, w0.y + 12.0f + hintSz.y), t.mute, hint);

    const auto groups = fpGroups(snap.host.pending_fp);
    const float groupGap = 20.0f;
    float groupsW = 0.0f;
    ImVec2 gw[4];
    for (int i = 0; i < 4; ++i) {
        gw[i] = calcText(ui.fontMono, geist::type::BODY, groups[static_cast<size_t>(i)].c_str());
        groupsW += gw[i].x;
    }
    groupsW += groupGap * 3.0f;
    float gx = snapPx(w0.x + (w1.x - w0.x - groupsW) * 0.5f);
    const float gy = w0.y + 46.0f;
    for (int i = 0; i < 4; ++i) {
        drawText(dl, ui.fontMono, geist::type::BODY, snapPt(gx, gy), t.ink, groups[static_cast<size_t>(i)].c_str());
        gx += gw[i].x + groupGap;
    }

    const float btnH = 36.0f;
    const float btnY = m1.y - 16.0f - btnH;
    const float denyW = 128.0f;
    const ImVec2 d0 = snapPt(innerL, btnY);
    const ImVec2 d1 = snapPt(innerL + denyW, btnY + btnH);
    const ImVec2 a0 = snapPt(d1.x + geist::space::SM, btnY);
    const ImVec2 a1 = snapPt(innerR, btnY + btnH);

    // Esc itself is handled in main.cpp (hostDecide(false)); the kbd hint matches that.
    if (solidButton(dl, ImGui::GetID("pair.deny"), "拒绝", "Esc", ui.fontSans, ui.fontMono, d0, d1)) {
        app.hostDecide(false);
    }
    if (inkCta(dl, ImGui::GetID("pair.allow"), "允许连接", nullptr, ui.fontSans, a0, a1)) {
        app.hostDecide(true);
    }
}

} // namespace upix::views
