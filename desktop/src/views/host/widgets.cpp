// views/host/widgets.cpp — formatting helpers, QR canvas artwork, and port input control.
#include "views/host/internal.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace upix::views::host_detail {
namespace {

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

} // namespace

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
    const float text = calcText(font, geist::type::BODY, label).x;
    const float icon = withIcon ? (15.0f + geist::space::SM) : 0.0f;
    const float w = std::ceil((text + icon + geist::space::LG * 2.0f) / 4.0f) * 4.0f;
    return std::max(108.0f, w);
}

void drawQr(ImDrawList* dl, ImVec2 well0, ImVec2 well1, const std::string& payload, bool live) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    const float lineT = hairlinePx();

    if (live && !payload.empty()) {
        try {
            const qrcodegen::QrCode qr =
                qrcodegen::QrCode::encodeText(payload.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            const int qsz = qr.getSize();
            const float wellSz = well1.x - well0.x;
            const float scale = currentDpiScale() > 0.0f ? currentDpiScale() : 1.0f;
            constexpr float kFramePad = 12.0f;
            constexpr float kBracketGap = 5.0f;
            const float maxMatrixSz = std::max(64.0f, wellSz - (kFramePad + kBracketGap) * 2.0f);

            int modPx = static_cast<int>(std::floor(maxMatrixSz * scale / static_cast<float>(std::max(qsz, 1))));
            if (modPx < 1) modPx = 1;
            const float mod = static_cast<float>(modPx) / scale;
            const float matrixSz = mod * static_cast<float>(qsz);
            const float plateSz = matrixSz + kFramePad * 2.0f;

            const ImVec2 plate0 = snapPt(well0.x + (wellSz - plateSz) * 0.5f,
                                         well0.y + (wellSz - plateSz) * 0.5f);
            const ImVec2 plate1 = snapPt(plate0.x + plateSz, plate0.y + plateSz);

            // Compact 12px white margin plate tightly hugged by 1px accent corner brackets.
            dl->AddRectFilled(plate0, plate1, t.qrPaper, geist::radius::MD);
            dl->AddRect(plate0, plate1, t.outline, geist::radius::MD, 0, lineT);

            const ImVec2 br0 = snapPt(plate0.x - kBracketGap, plate0.y - kBracketGap);
            const ImVec2 br1 = snapPt(plate1.x + kBracketGap, plate1.y + kBracketGap);
            drawRegistrationMarks(dl, br0, br1, 0.0f, 12.0f, lineT, t.accent);

            const float ox = plate0.x + kFramePad;
            const float oy = plate0.y + kFramePad;
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

    // Idle dormant state: subtle well + 1px regMark corner brackets.
    dl->AddRectFilled(well0, well1, t.wellBg, geist::radius::LG);
    dl->AddRect(well0, well1, t.outline, geist::radius::LG, 0, lineT);
    drawRegistrationMarks(dl, well0, well1, 10.0f, 12.0f, lineT, t.regMark);

    const ImU32 ghost = t.faint;
    auto finder = [&](ImVec2 tl) {
        dl->AddRect(tl, ImVec2(tl.x + 28.0f, tl.y + 28.0f), ghost, 4.0f, 0, 2.0f);
        dl->AddRectFilled(ImVec2(tl.x + 8.0f, tl.y + 8.0f), ImVec2(tl.x + 20.0f, tl.y + 20.0f), ghost, 2.0f);
    };
    finder(ImVec2(well0.x + 36.0f, well0.y + 36.0f));
    finder(ImVec2(well1.x - 36.0f - 28.0f, well0.y + 36.0f));
    finder(ImVec2(well0.x + 36.0f, well1.y - 36.0f - 28.0f));
    for (int gy = 0; gy < 7; ++gy) {
        for (int gx = 0; gx < 7; ++gx) {
            const bool finderCell = (gx < 2 && gy < 2) || (gx > 4 && gy < 2) || (gx < 2 && gy > 4);
            const bool center = gx >= 2 && gx <= 4 && gy >= 2 && gy <= 4;
            if (finderCell || center) continue;
            if ((gx * 3 + gy * 5) % 2 != 0) continue;
            const ImVec2 d(well0.x + 48.0f + gx * 16.0f, well0.y + 48.0f + gy * 16.0f);
            dl->AddRectFilled(d, ImVec2(d.x + 4.0f, d.y + 4.0f), ghost, 1.5f);
        }
    }
    const ImVec2 mid((well0.x + well1.x) * 0.5f, (well0.y + well1.y) * 0.5f);
    const ImVec2 b0 = snapPt(mid.x - 22.0f, mid.y - 22.0f);
    const ImVec2 b1 = snapPt(mid.x + 22.0f, mid.y + 22.0f);
    dl->AddRectFilled(b0, b1, t.container, 10.0f);
    dl->AddRect(b0, b1, t.outline, 10.0f, 0, lineT);
    drawIcon(dl, "qr-code", mid, 18.0f, t.mute, 1.6f);
}

bool portInputField(ImDrawList* dl, ImGuiID id, ImVec2 a, ImVec2 b, int port, bool editable,
                    bool runningLocked, ImFont* mono, ImFont* sans, int& outPort) {
    using namespace upix::ui;
    const auto& t = geist::tokens();
    a = snapPt(a);
    b = snapPt(b);

    const ImVec2 colon = calcText(mono, geist::type::MONO, ":");

    if (!editable) {
        dl->AddRectFilled(a, b, runningLocked ? t.recessed : t.bg, geist::radius::MD);
        dl->AddRect(a, b, t.outline, geist::radius::MD, 0, hairlinePx());

        const std::string portStr = std::to_string(port);
        const ImVec2 ps = calcText(mono, geist::type::MONO, portStr.c_str());

        static flex::Context roCtx;
        flex::Layout fx(roCtx);
        auto row = fx.row({
            .gap = 5.0f,
            .justify = flex::Justify::Center,
            .align = flex::Align::Center,
        });
        const auto colonSlot = row.slot(flex::Fixed(colon.x), flex::Fixed(colon.y));
        const auto portSlot = row.slot(flex::Fixed(ps.x), flex::Fixed(ps.y));
        fx.solve(flex::Rect{a.x, a.y, std::max(0.0f, b.x - a.x), std::max(0.0f, b.y - a.y)});

        const flex::Rect cr = fx.rect(colonSlot);
        const flex::Rect pr = fx.rect(portSlot);
        drawTextCenteredY(dl, mono, geist::type::MONO, cr.x, a.y, b.y, t.faint, ":");
        drawTextCenteredY(dl, mono, geist::type::MONO, pr.x, a.y, b.y,
                          runningLocked ? t.mute : t.ink, portStr.c_str());
        if (runningLocked) {
            const bool hov = ctl::mouseIn(a, b);
            ctl::tooltip(dl, ctl::detail::salt(id, 4), a, b,
                         upix::i18n::tr(upix::i18n::Key::HostPortLockedTip), sans, hov, false);
        }
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

    dl->AddRectFilled(a, b, t.bg, geist::radius::MD);

    static flex::Context edCtx;
    flex::Layout fx(edCtx);
    auto row = fx.row({
        .padding = flex::Edges::only(12.0f, 0.0f, 10.0f, 0.0f),
        .gap = 5.0f,
        .align = flex::Align::Center,
    });
    const auto colonSlot = row.slot(flex::Fixed(colon.x), flex::Fixed(colon.y));
    const auto inputSlot = row.slot(flex::Grow(1.0f).min(24.0f), flex::Fixed(std::max(0.0f, b.y - a.y)));
    fx.solve(flex::Rect{a.x, a.y, std::max(0.0f, b.x - a.x), std::max(0.0f, b.y - a.y)});

    const flex::Rect cr = fx.rect(colonSlot);
    const flex::Rect ir = fx.rect(inputSlot);
    drawTextCenteredY(dl, mono, geist::type::MONO, cr.x, a.y, b.y, t.faint, ":");

    const float inputW = std::max(24.0f, ir.w);
    const float portPadY =
        snapPx(std::max(0.0f, drawTextPosYCentered(mono, geist::type::MONO, a.y, b.y, buf.text) - a.y));
    ImGui::PushID(static_cast<int>(id & 0x7fffffff));
    ImGui::SetCursorScreenPos(snapPt(ir.x, a.y));
    if (mono) ImGui::PushFont(mono, geist::type::MONO);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, t.ink);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, portPadY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::SetNextItemWidth(inputW);
    char item[24];
    std::snprintf(item, sizeof(item), "##port%08x", static_cast<unsigned>(id));
    const bool edited = ImGui::InputText(item, buf.text, sizeof(buf.text), ImGuiInputTextFlags_CharsDecimal);
    const bool active = ImGui::IsItemActive();
    const bool focused = active || (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible);
    const bool itemHovered = ImGui::IsItemHovered();
    const bool deactivated = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    if (mono) ImGui::PopFont();
    ImGui::PopID();

    const bool portHovered = !focused && (ctl::mouseIn(a, b) || itemHovered);
    const float hovT = motion::hoverFactor(ctl::detail::salt(id, 3), portHovered);

    if (focused) {
        ctl::detail::inputFocusHalo(dl, a, b, geist::radius::MD, true);
    } else {
        dl->AddRect(a, b, motion::mix(t.outline, t.outlineInput, hovT), geist::radius::MD, 0, hairlinePx());
    }

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

} // namespace upix::views::host_detail
