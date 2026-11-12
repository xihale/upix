#include "views/common.hpp"

#include "ui_theme.hpp"
#include "ui_widgets.hpp"

#include "imgui.h"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace upix::views {

void renderRemotePage(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                             upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    using namespace upix::ui;
    const auto& pal = ui.pal;

    float railW = 76.0f;
    float availW = winSize.x - railW;
    float colW = std::min(712.0f, availW - 48.0f);
    float colX = snapPx(railW + (availW - colW) * 0.5f);

    bool isIdle = !snap.host.running && snap.host.trusted_devices.empty() && snap.host.logs.empty();
    float totalActiveH = 288.0f + 14.0f + 110.0f + 14.0f + 122.0f; // 548px
    float y = isIdle ? snapPx((winSize.y - 288.0f) * 0.5f) : snapPx((winSize.y - totalActiveH) * 0.5f);

    // --- Card 1: Pairing Panel (712x288, Figma 73:2672: #1c1c21, border #2d2d34, radius 12px, padding 24px, gap 28px) ---
    ImVec2 p0 = snapPt(colX, y);
    ImVec2 p1 = snapPt(colX + colW, y + 288.0f);
    drawBox(dl, p0, p1, pal.containerLow(), pal.outlineInput(), 12.0f);

    // Left 240x240 QR Well
    ImVec2 qr0 = snapPt(p0.x + 24.0f, p0.y + 24.0f);
    ImVec2 qr1 = snapPt(qr0.x + 240.0f, qr0.y + 240.0f);
    drawBox(dl, qr0, qr1, pal.recessed(), pal.outline(), 10.0f);

    ImU32 regCol = snap.host.running ? pal.primaryDimBorder() : pal.regMark();
    drawRegistrationMarks(dl, qr0, qr1, 14.0f, 12.0f, 2.0f, regCol);

    if (snap.host.running && !snap.host.qr_payload.empty()) {
        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(snap.host.qr_payload.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int qsz = qr.getSize();
            float boxSz = 192.0f;
            float cell = std::floor(boxSz / (qsz + 4));
            float realSz = cell * (qsz + 4);
            ImVec2 qStart = snapPt(qr0.x + (240.0f - realSz) * 0.5f, qr0.y + (240.0f - realSz) * 0.5f);
            dl->AddRectFilled(qStart, ImVec2(qStart.x + realSz, qStart.y + realSz), hex(0xF4F3EF), 8.0f);
            for (int qy = 0; qy < qsz; ++qy) {
                for (int qx = 0; qx < qsz; ++qx) {
                    if (qr.getModule(qx, qy)) {
                        ImVec2 m0(qStart.x + (qx + 2) * cell, qStart.y + (qy + 2) * cell);
                        dl->AddRectFilled(m0, ImVec2(m0.x + cell, m0.y + cell), hex(0x131316));
                    }
                }
            }
        } catch (...) {}
    } else {
        ImVec2 qCenter = snapPt((qr0.x + qr1.x) * 0.5f, (qr0.y + qr1.y) * 0.5f);
        auto drawFinder = [&](ImVec2 tl) {
            dl->AddRect(tl, ImVec2(tl.x + 28.0f, tl.y + 28.0f), pal.outlineStrong(), 3.0f, 0, 2.0f);
            dl->AddRectFilled(ImVec2(tl.x + 8.0f, tl.y + 8.0f), ImVec2(tl.x + 20.0f, tl.y + 20.0f), pal.outlineStrong(), 1.5f);
        };
        drawFinder(ImVec2(qr0.x + 48.0f, qr0.y + 48.0f));
        drawFinder(ImVec2(qr1.x - 76.0f, qr0.y + 48.0f));
        drawFinder(ImVec2(qr0.x + 48.0f, qr1.y - 76.0f));

        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 7; ++gx) {
                if ((gx < 2 && gy < 2) || (gx > 4 && gy < 2) || (gx < 2 && gy > 4) || (gx >= 2 && gx <= 4 && gy >= 2 && gy <= 4))
                    continue;
                if ((gx * 3 + gy * 5) % 2 == 0) {
                    dl->AddCircleFilled(ImVec2(qr0.x + 62.0f + gx * 19.0f, qr0.y + 62.0f + gy * 19.0f), 2.2f, pal.outlineStrong());
                }
            }
        }

        ImVec2 badge0(qCenter.x - 24.0f, qCenter.y - 24.0f);
        ImVec2 badge1(qCenter.x + 24.0f, qCenter.y + 24.0f);
        drawBox(dl, badge0, badge1, pal.surface(), pal.outlineStrong(), 10.0f);
        drawIcon(dl, "qr-code", qCenter, 18.0f, pal.muted());
    }

    // Right Instrument Column (396px wide, gap 28px from QR)
    float rx = snapPx(qr1.x + 28.0f);
    float rw = 396.0f;
    float ry = p0.y + 24.0f;

    // Row 1: 指纹 (44px row, label 12px #7e7e8a, 44x24 pills #16161b border #26262e)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "指纹");
    float fx = rx + 48.0f;
    std::string fp = snap.host.fingerprint.empty() ? "EC61F2B66D4C25C4" : snap.host.fingerprint;
    size_t pos = 0;
    int groupIdx = 0;
    while (pos < fp.size() && groupIdx < 4) {
        while (pos < fp.size() && (fp[pos] == ' ' || fp[pos] == ':')) ++pos;
        std::string token = fp.substr(pos, std::min<size_t>(4, fp.size() - pos));
        pos += token.size();
        ImVec2 c0 = snapPt(fx, ry + 10.0f);
        ImVec2 c1 = snapPt(fx + 44.0f, ry + 34.0f);
        drawBox(dl, c0, c1, hex(0x16161B), hex(0x26262E), 4.0f);
        drawTextCentered(dl, ui.fontMono, 11.5f, c0, c1, hex(0xD8D8E0), token.c_str());
        fx += 50.0f;
        ++groupIdx;
    }
    ImGui::SetCursorScreenPos(ImVec2(fx + 2.0f, ry + 10.0f));
    if (ImGui::InvisibleButton("##copy_fp", ImVec2(24.0f, 24.0f))) {
        SDL_SetClipboardText(fp.c_str());
    }
    drawIcon(dl, "copy", ImVec2(fx + 14.0f, ry + 22.0f), 13.0f, hex(0x9A9AA6));
    ry += 44.0f;

    dl->AddRectFilled(snapPt(rx, ry), snapPt(rx + rw, ry + hairlinePx()), hex(0x26262E));
    ry += 1.0f;

    // Row 2: 地址 (44px row, 216x30 input #22222a border #282830)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "地址");
    std::string curIp = snap.host.ips.empty() ? "192.168.0.126" : snap.host.ips[snap.host.selected_ip_idx % snap.host.ips.size()];
    ImVec2 ip0 = snapPt(rx + 48.0f, ry + 7.0f);
    ImVec2 ip1 = snapPt(ip0.x + 216.0f, ry + 37.0f);
    drawBox(dl, ip0, ip1, hex(0x22222A), pal.outline(), 6.0f);
    dl->AddCircleFilled(ImVec2(ip0.x + 12.5f, ry + 22.0f), 2.5f, pal.primary());
    drawText(dl, ui.fontMono, 12.0f, ImVec2(ip0.x + 23.0f, ry + 14.0f), pal.onSurface(), curIp.c_str());
    ImVec2 lan0 = snapPt(ip1.x - 50.0f, ry + 14.0f);
    drawBox(dl, lan0, ImVec2(lan0.x + 28.0f, lan0.y + 16.0f), hex(0x23232B), 0, 4.0f);
    drawTextCentered(dl, ui.fontMono, 9.5f, lan0, ImVec2(lan0.x + 28.0f, lan0.y + 16.0f), hex(0x8E8E9A), "LAN");
    drawIcon(dl, "chevron-down", ImVec2(ip1.x - 13.0f, ry + 22.0f), 10.0f, hex(0x7E7E8A));
    ry += 44.0f;

    dl->AddRectFilled(snapPt(rx, ry), snapPt(rx + rw, ry + hairlinePx()), hex(0x26262E));
    ry += 1.0f;

    // Row 3: 端口 (44px row, 84x30 input #16161b border #2a2a32)
    drawText(dl, ui.fontSans, 12.0f, ImVec2(rx, ry + 13.0f), hex(0x7E7E8A), "端口");
    ImVec2 pt0 = snapPt(rx + 48.0f, ry + 7.0f);
    ImVec2 pt1 = snapPt(pt0.x + 84.0f, ry + 37.0f);
    drawBox(dl, pt0, pt1, hex(0x16161B), hex(0x2A2A32), 6.0f);
    drawText(dl, ui.fontMono, 12.0f, ImVec2(pt0.x + 10.0f, ry + 14.0f), pal.dim(), ":");
    std::string portNum = std::format("{}", snap.host.port);
    drawText(dl, ui.fontMono, 12.0f, ImVec2(pt0.x + 20.0f, ry + 14.0f), pal.onSurface(), portNum.c_str());

    // Bottom of card: Left [● LIVE] (64x24) + Right [停止服务] (98x36, #26262e border #383844)
    float botY = p1.y - 60.0f;
    ImVec2 st0 = snapPt(rx, botY + 6.0f);
    ImVec2 st1 = snapPt(rx + 64.0f, botY + 30.0f);
    if (snap.host.running) {
        drawBox(dl, st0, st1, hex(0x162624), hex(0x24423E), 12.0f);
        dl->AddCircleFilled(ImVec2(st0.x + 13.0f, st0.y + 12.0f), 3.0f, pal.primary());
        drawText(dl, ui.fontMono, 10.5f, ImVec2(st0.x + 22.0f, st0.y + 5.0f), pal.primary(), "LIVE");

        ImVec2 stop0 = snapPt(rx + rw - 98.0f, botY);
        ImGui::SetCursorScreenPos(stop0);
        if (ImGui::InvisibleButton("##host_stop", ImVec2(98.0f, 36.0f))) {
            app.hostStop();
        }
        drawBox(dl, stop0, ImVec2(stop0.x + 98.0f, stop0.y + 36.0f), hex(0x26262E), hex(0x383844), 6.0f);
        dl->AddRectFilled(snapPt(stop0.x + 15.0f, stop0.y + 14.0f), snapPt(stop0.x + 23.0f, stop0.y + 22.0f), pal.onSurface(), 2.0f);
        drawText(dl, ui.fontSans, 13.0f, ImVec2(stop0.x + 30.0f, stop0.y + 9.0f), pal.onSurface(), "停止服务");
    } else {
        drawBox(dl, st0, st1, pal.recessed(), pal.outline(), 12.0f);
        dl->AddCircleFilled(ImVec2(st0.x + 13.0f, st0.y + 12.0f), 3.0f, pal.dim());
        drawText(dl, ui.fontMono, 10.5f, ImVec2(st0.x + 22.0f, st0.y + 5.0f), pal.dim(), "IDLE");

        if (pillButton(dl, "##host_start", ImVec2(rx + rw - 98.0f, botY), ImVec2(98.0f, 36.0f), "启动服务", "play-outline", pal, ui.fontSans, true, 6.0f, 13.0f)) {
            app.hostStart();
        }
    }

    if (isIdle) return;
    y += 288.0f + 14.0f;

    // --- Card 2: 已信任设备 (712x110, Figma 83:3669: #18181c, border #282830, header 34px #202026e5, rows 38px) ---
    float tdH = 34.0f + std::max<size_t>(1, snap.host.trusted_devices.size()) * 38.0f;
    ImVec2 td0 = snapPt(colX, y);
    ImVec2 td1 = snapPt(colX + colW, y + tdH);
    drawBox(dl, td0, td1, pal.surface(), pal.outline(), 10.0f);
    dl->AddRectFilled(snapPt(td0.x + 1.0f, td0.y + 1.0f), snapPt(td1.x - 1.0f, td0.y + 34.0f), pal.container(), 9.0f, ImDrawFlags_RoundCornersTop);
    drawText(dl, ui.fontSans, 12.5f, ImVec2(td0.x + 16.0f, td0.y + 8.5f), hex(0xE2E2E8), "已信任设备");
    std::string tdCnt = std::format("{}", snap.host.trusted_devices.size());
    drawText(dl, ui.fontMono, 11.0f, ImVec2(td1.x - 24.0f, td0.y + 9.5f), hex(0x7E7E8A), tdCnt.c_str());

    float dy = td0.y + 34.0f;
    for (size_t i = 0; i < snap.host.trusted_devices.size(); ++i) {
        const auto& d = snap.host.trusted_devices[i];
        dl->AddCircleFilled(ImVec2(td0.x + 19.0f, dy + 19.0f), 3.0f, d.online ? pal.primary() : hex(0x4A4A56));
        drawText(dl, ui.fontMono, 12.5f, ImVec2(td0.x + 32.0f, dy + 10.5f), d.online ? pal.onSurface() : hex(0x9E9EA8), d.name.c_str());
        ImVec2 dsz = calcText(ui.fontMono, 12.5f, d.name.c_str());

        ImVec2 b0 = snapPt(td0.x + 42.0f + dsz.x, dy + 9.5f);
        ImVec2 b1 = snapPt(b0.x + 40.0f, dy + 28.5f);
        drawBox(dl, b0, b1, d.online ? pal.primaryDimBg() : pal.container(), d.online ? hex(0x264A43) : hex(0x2C2C35), 4.0f);
        drawTextCentered(dl, ui.fontSans, 11.0f, b0, b1, d.online ? pal.primary() : hex(0x7E7E8A), d.online ? "在线" : "昨天");

        ImVec2 fpsz = calcText(ui.fontMono, 11.5f, d.fingerprint.c_str());
        drawText(dl, ui.fontMono, 11.5f, ImVec2(td1.x - 56.0f - fpsz.x, dy + 11.0f), pal.dim(), d.fingerprint.c_str());
        drawIcon(dl, "trash", ImVec2(td1.x - 27.0f, dy + 19.0f), 13.5f, hex(0x5E5E6A));
        dy += 38.0f;
    }
    y += tdH + 14.0f;

    // --- Card 3: Active Telemetry Console (712x122, Figma 83:3730: #151519, border #282830, header 38px #1d1d23) ---
    ImVec2 lg0 = snapPt(colX, y);
    ImVec2 lg1 = snapPt(colX + colW, y + 122.0f);
    drawBox(dl, lg0, lg1, pal.recessed(), pal.outline(), 10.0f);

    dl->AddRectFilled(snapPt(lg0.x + 1.0f, lg0.y + 1.0f), snapPt(lg1.x - 1.0f, lg0.y + 38.0f), hex(0x1D1D23), 9.0f, ImDrawFlags_RoundCornersTop);
    dl->AddCircleFilled(ImVec2(lg0.x + 19.0f, lg0.y + 19.0f), 3.0f, pal.primary());
    std::string actDev = snap.host.active_device.empty() ? "Pixel 9 Pro" : snap.host.active_device;
    drawText(dl, ui.fontMono, 12.0f, ImVec2(lg0.x + 30.0f, lg0.y + 11.0f), pal.onSurface(), actDev.c_str());

    ImVec2 mb0 = snapPt(lg0.x + 124.0f, lg0.y + 10.0f);
    ImVec2 mb1 = snapPt(mb0.x + 118.0f, lg0.y + 28.0f);
    drawBox(dl, mb0, mb1, hex(0x16161B), hex(0x2A2A34), 4.0f);
    drawTextCentered(dl, ui.fontMono, 10.0f, mb0, mb1, hex(0x8E8E9A), "AnimeSharp-x4 · 4×");

    int tPct = snap.host.task_percent >= 0 ? snap.host.task_percent : 78;
    std::string tPctStr = std::format("{}%", tPct);
    drawText(dl, ui.fontMono, 11.0f, ImVec2(lg1.x - 40.0f, lg0.y + 11.5f), pal.primary(), tPctStr.c_str());
    float pBarW = 136.0f;
    float pBarX = snapPx(lg1.x - 52.0f - pBarW);
    dl->AddRectFilled(ImVec2(pBarX, lg0.y + 17.0f), ImVec2(pBarX + pBarW, lg0.y + 21.0f), hex(0x121216), 2.0f);
    dl->AddRectFilled(ImVec2(pBarX, lg0.y + 17.0f), ImVec2(pBarX + pBarW * (tPct / 100.0f), lg0.y + 21.0f), pal.primary(), 2.0f);

    float ly = lg0.y + 46.0f;
    for (size_t i = 0; i < snap.host.logs.size() && i < 3; ++i) {
        const std::string& raw = snap.host.logs[i];
        auto p1Pos = raw.find('|');
        if (p1Pos != std::string::npos) {
            auto p2Pos = raw.find('|', p1Pos + 1);
            auto p3Pos = raw.find('|', p2Pos + 1);
            std::string tm = raw.substr(0, p1Pos);
            std::string tag = raw.substr(p1Pos + 1, p2Pos - p1Pos - 1);
            std::string msg = raw.substr(p2Pos + 1, p3Pos - p2Pos - 1);
            std::string rNote = (p3Pos != std::string::npos) ? raw.substr(p3Pos + 1) : "";

            drawText(dl, ui.fontMono, 11.0f, ImVec2(lg0.x + 16.0f, ly), hex(0x565662), tm.c_str());

            bool isTask = (tag == "TASK");
            ImVec2 tg0 = snapPt(lg0.x + 76.0f, ly - 1.0f);
            ImVec2 tg1 = snapPt(tg0.x + 42.0f, ly + 15.0f);
            drawBox(dl, tg0, tg1, isTask ? pal.primaryDimBg() : hex(0x1D1E24), isTask ? pal.primaryDimBorder() : hex(0x2D2E38), 4.0f);
            drawTextCentered(dl, ui.fontMono, 10.0f, tg0, tg1, isTask ? pal.primary() : hex(0x8E8E9A), tag.c_str());

            drawText(dl, ui.fontSans, 11.0f, ImVec2(tg1.x + 8.0f, ly - 0.5f), isTask ? hex(0xC8C8D2) : hex(0x8E8E9A), msg.c_str());

            if (!rNote.empty()) {
                ImVec2 nsz = calcText(ui.fontMono, 11.0f, rNote.c_str());
                drawText(dl, ui.fontMono, 11.0f, ImVec2(lg1.x - 16.0f - nsz.x, ly), isTask ? pal.primary() : hex(0x565662), rNote.c_str());
            }
        } else {
            drawText(dl, ui.fontMono, 11.0f, ImVec2(lg0.x + 16.0f, ly), pal.muted(), raw.c_str());
        }
        ly += 24.0f;
    }
}


void renderPairingModal(ImDrawList* dl, ImVec2 winSize, UiViewState& ui,
                               upix::gui::AppState& app, const upix::gui::Snapshot& snap) {
    if (snap.host.pending_device.empty()) return;
    using namespace upix::ui;
    const auto& pal = ui.pal;

    dl->AddRectFilled(ImVec2(0, 0), winSize, rgba(8, 8, 11, 185));

    ImVec2 sz(380.0f, 240.0f);
    ImVec2 m0 = snapPt((winSize.x - sz.x) * 0.5f, (winSize.y - sz.y) * 0.5f);
    ImVec2 m1 = snapPt(m0.x + sz.x, m0.y + sz.y);
    drawBox(dl, m0, m1, pal.surface(), pal.outlineStrong(), 14.0f);

    float headY = m0.y + 24.0f;
    ImVec2 ic0 = snapPt(m0.x + 100.0f, headY);
    ImVec2 ic1 = snapPt(ic0.x + 28.0f, headY + 28.0f);
    drawBox(dl, ic0, ic1, pal.primaryDimBg(), pal.primaryDimBorder(), 7.0f);
    drawIcon(dl, "smartphone", ImVec2(ic0.x + 14.0f, ic0.y + 14.0f), 15.0f, pal.primary());

    drawText(dl, ui.fontMono, 15.0f, ImVec2(ic1.x + 10.0f, headY + 4.5f), pal.onSurface(), snap.host.pending_device.c_str());
    ImVec2 dsz = calcText(ui.fontMono, 15.0f, snap.host.pending_device.c_str());

    ImVec2 pb0 = snapPt(ic1.x + 20.0f + dsz.x, headY + 4.0f);
    ImVec2 pb1 = snapPt(pb0.x + 38.0f, headY + 24.0f);
    drawBox(dl, pb0, pb1, pal.high(), pal.outlineStrong(), 4.0f);
    drawTextCentered(dl, ui.fontMono, 10.5f, pb0, pb1, pal.muted(), "PAIR");

    ImVec2 fw0 = snapPt(m0.x + 22.0f, m0.y + 72.0f);
    ImVec2 fw1 = snapPt(m1.x - 22.0f, m0.y + 156.0f);
    drawBox(dl, fw0, fw1, pal.recessed(), pal.outline(), 10.0f);
    drawTextCentered(dl, ui.fontSans, 11.5f, ImVec2(fw0.x, fw0.y + 12.0f), ImVec2(fw1.x, fw0.y + 28.0f), pal.muted(), "核对设备指纹一致");

    std::string fp = snap.host.pending_fp.empty() ? "EC61F2B66D4C25C4" : snap.host.pending_fp;
    float pillW = 60.0f;
    float pillGap = 8.0f;
    float startX = snapPx(fw0.x + (336.0f - (pillW * 4 + pillGap * 3)) * 0.5f);
    size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        while (pos < fp.size() && (fp[pos] == ' ' || fp[pos] == ':')) ++pos;
        std::string tok = fp.substr(pos, std::min<size_t>(4, fp.size() - pos));
        pos += tok.size();
        ImVec2 c0 = snapPt(startX + i * (pillW + pillGap), fw0.y + 38.0f);
        ImVec2 c1 = snapPt(c0.x + pillW, c0.y + 32.0f);
        drawBox(dl, c0, c1, pal.surface(), pal.outlineStrong(), 6.0f);
        drawTextCentered(dl, ui.fontMono, 13.0f, c0, c1, pal.onSurface(), tok.c_str());
    }

    ImVec2 deny0 = snapPt(m0.x + 22.0f, m1.y - 56.0f);
    ImVec2 denySz(116.0f, 38.0f);
    ImGui::SetCursorScreenPos(deny0);
    if (ImGui::InvisibleButton("##pair_deny", denySz)) {
        app.hostDecide(false);
    }
    drawBox(dl, deny0, ImVec2(deny0.x + denySz.x, deny0.y + denySz.y), pal.container(), pal.outlineStrong(), 8.0f);
    drawText(dl, ui.fontSans, 13.0f, ImVec2(deny0.x + 30.0f, deny0.y + 10.0f), pal.onSurface(), "拒绝");
    drawText(dl, ui.fontMono, 11.5f, ImVec2(deny0.x + 64.0f, deny0.y + 11.0f), pal.dim(), "Esc");

    if (pillButton(dl, "##pair_allow", ImVec2(deny0.x + denySz.x + 10.0f, m1.y - 56.0f), ImVec2(210.0f, 38.0f), "允许连接", "", pal, ui.fontSans, true, 8.0f, 13.0f)) {
        app.hostDecide(true);
    }
}



} // namespace upix::views
