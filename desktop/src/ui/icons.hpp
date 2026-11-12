// ui/icons.hpp — 24x24 Lucide-style crisp vector icon catalog + CJK baseline alignment.
#pragma once

#include "ui/geist.hpp"

#include "imgui.h"

#include <cmath>
#include <cstring>

namespace upix::ui {

// CJK glyphs sit slightly lower in the em box than Latin caps; nudge paired
// inline icons +1px so icon and CJK label share optical center.
inline float cjkIconCenterY(ImVec2 boxMin, ImVec2 boxMax) {
    return (boxMin.y + boxMax.y) * 0.5f + 1.0f;
}

inline void drawIcon(ImDrawList* dl, const char* kind, ImVec2 center, float size, ImU32 col, float stroke = 1.6f) {
    if (!dl || !kind || !kind[0]) return;
    auto pt = [&](float vx, float vy) -> ImVec2 {
        return ImVec2(center.x + (vx - 12.0f) / 24.0f * size,
                      center.y + (vy - 12.0f) / 24.0f * size);
    };
    const float unit = size / 24.0f;

    if (std::strcmp(kind, "zoom-in") == 0) {
        dl->AddCircle(pt(11, 11), 7.5f * unit, col, 28, stroke);
        dl->AddLine(pt(21, 21), pt(16.4f, 16.4f), col, stroke);
        dl->AddLine(pt(11, 8), pt(11, 14), col, stroke);
        dl->AddLine(pt(8, 11), pt(14, 11), col, stroke);
    } else if (std::strcmp(kind, "zoom-1x") == 0) {
        dl->AddRect(pt(3.5f, 5.5f), pt(20.5f, 18.5f), col, 3.0f * unit, 0, stroke);
        dl->AddLine(pt(8.5f, 9.0f), pt(8.5f, 15.0f), col, stroke);
        dl->AddCircleFilled(pt(12.0f, 10.5f), 0.9f * unit, col);
        dl->AddCircleFilled(pt(12.0f, 13.8f), 0.9f * unit, col);
        dl->AddLine(pt(15.5f, 9.0f), pt(15.5f, 15.0f), col, stroke);
    } else if (std::strcmp(kind, "maximize") == 0 || std::strcmp(kind, "fit") == 0) {
        // 4 corner brackets
        ImVec2 tl[3] = {pt(4, 9), pt(4, 4), pt(9, 4)};
        ImVec2 tr[3] = {pt(15, 4), pt(20, 4), pt(20, 9)};
        ImVec2 br[3] = {pt(20, 15), pt(20, 20), pt(15, 20)};
        ImVec2 bl[3] = {pt(9, 20), pt(4, 20), pt(4, 15)};
        dl->AddPolyline(tl, 3, col, ImDrawFlags_None, stroke);
        dl->AddPolyline(tr, 3, col, ImDrawFlags_None, stroke);
        dl->AddPolyline(br, 3, col, ImDrawFlags_None, stroke);
        dl->AddPolyline(bl, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "layers") == 0) {
        ImVec2 top[4] = {pt(12, 3), pt(21, 7.5f), pt(12, 12), pt(3, 7.5f)};
        dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, stroke);
        ImVec2 mid[3] = {pt(3, 12.5f), pt(12, 17), pt(21, 12.5f)};
        dl->AddPolyline(mid, 3, col, ImDrawFlags_None, stroke);
        ImVec2 bot[3] = {pt(3, 17), pt(12, 21.5f), pt(21, 17)};
        dl->AddPolyline(bot, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "smartphone") == 0) {
        dl->AddRect(pt(6.5f, 2.5f), pt(17.5f, 21.5f), col, 2.2f * unit, 0, stroke);
        dl->AddCircleFilled(pt(12, 18.2f), std::max(1.0f, 1.15f * unit), col);
    } else if (std::strcmp(kind, "book") == 0) {
        // Open book for 指南 Guide
        ImVec2 left[4] = {pt(12, 6), pt(3.5f, 4.5f), pt(3.5f, 18.5f), pt(12, 20)};
        ImVec2 right[4] = {pt(12, 6), pt(20.5f, 4.5f), pt(20.5f, 18.5f), pt(12, 20)};
        dl->AddPolyline(left, 4, col, ImDrawFlags_Closed, stroke);
        dl->AddPolyline(right, 4, col, ImDrawFlags_Closed, stroke);
    } else if (std::strcmp(kind, "gear") == 0 || std::strcmp(kind, "settings") == 0) {
        dl->AddCircle(pt(12, 12), 3.3f * unit, col, 20, stroke);
        dl->AddCircle(pt(12, 12), 6.8f * unit, col, 24, stroke * 0.85f);
        for (int i = 0; i < 8; ++i) {
            float a = i * 0.78539816f;
            ImVec2 p0(center.x + std::cos(a) * 6.8f * unit, center.y + std::sin(a) * 6.8f * unit);
            ImVec2 p1(center.x + std::cos(a) * 9.5f * unit, center.y + std::sin(a) * 9.5f * unit);
            dl->AddLine(p0, p1, col, stroke);
        }
    } else if (std::strcmp(kind, "folder-open") == 0 || std::strcmp(kind, "folder") == 0) {
        ImVec2 p[6] = {pt(3, 6), pt(9, 6), pt(11, 8.5f), pt(21, 8.5f), pt(21, 19), pt(3, 19)};
        dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, stroke);
    } else if (std::strcmp(kind, "clipboard") == 0) {
        dl->AddRect(pt(5.5f, 5.0f), pt(18.5f, 21.0f), col, 2.0f * unit, 0, stroke);
        dl->AddRect(pt(9.0f, 3.0f), pt(15.0f, 6.5f), col, 1.2f * unit, 0, stroke);
        dl->AddLine(pt(8.5f, 11.5f), pt(15.5f, 11.5f), col, stroke * 0.9f);
        dl->AddLine(pt(8.5f, 15.5f), pt(13.5f, 15.5f), col, stroke * 0.9f);
    } else if (std::strcmp(kind, "image") == 0) {
        dl->AddRect(pt(3.5f, 4.5f), pt(20.5f, 19.5f), col, 2.0f * unit, 0, stroke);
        dl->AddCircleFilled(pt(8.5f, 9.5f), std::max(1.1f, 1.3f * unit), col);
        ImVec2 ridge[4] = {pt(4.5f, 17.5f), pt(9.5f, 13.0f), pt(12.5f, 15.5f), pt(19.5f, 10.5f)};
        dl->AddPolyline(ridge, 4, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "external-link") == 0) {
        ImVec2 box[4] = {pt(14, 5), pt(5, 5), pt(5, 19), pt(19, 19)};
        dl->AddPolyline(box, 4, col, ImDrawFlags_None, stroke);
        dl->AddLine(pt(19, 19), pt(19, 11), col, stroke);
        dl->AddLine(pt(11, 13), pt(19.5f, 4.5f), col, stroke);
        ImVec2 arr[3] = {pt(15, 4.5f), pt(19.5f, 4.5f), pt(19.5f, 9)};
        dl->AddPolyline(arr, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "alert-circle") == 0) {
        dl->AddCircle(pt(12, 12), 9.0f * unit, col, 24, stroke);
        dl->AddLine(pt(12, 8.0f), pt(12, 12.8f), col, stroke);
        dl->AddCircleFilled(pt(12, 16.0f), std::max(0.9f, 1.0f * unit), col);
    } else if (std::strcmp(kind, "spinner") == 0) {
        const float a0 = ImGui::GetCurrentContext() ? static_cast<float>(ImGui::GetTime()) * 5.0f : 0.0f;
        dl->PathClear();
        dl->PathArcTo(center, 8.0f * unit, a0, a0 + 2.2f, 18);
        dl->PathStroke(col, 0, stroke);
    } else if (std::strcmp(kind, "play") == 0) {
        dl->AddTriangleFilled(pt(6.5f, 4.8f), pt(19.0f, 12.0f), pt(6.5f, 19.2f), col);
    } else if (std::strcmp(kind, "play-outline") == 0) {
        ImVec2 p[3] = {pt(6.5f, 4.8f), pt(19.0f, 12.0f), pt(6.5f, 19.2f)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_Closed, stroke);
    } else if (std::strcmp(kind, "stop") == 0) {
        dl->AddRectFilled(pt(6.5f, 6.5f), pt(17.5f, 17.5f), col, 1.8f);
    } else if (std::strcmp(kind, "chevron-down") == 0) {
        ImVec2 p[3] = {pt(6, 9), pt(12, 15), pt(18, 9)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "chevron-up") == 0) {
        ImVec2 p[3] = {pt(6, 15), pt(12, 9), pt(18, 15)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "chevron-left") == 0) {
        ImVec2 p[3] = {pt(15, 6), pt(9, 12), pt(15, 18)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "chevron-right") == 0) {
        ImVec2 p[3] = {pt(9, 6), pt(15, 12), pt(9, 18)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "check") == 0) {
        ImVec2 p[3] = {pt(20, 6), pt(9, 17), pt(4, 12)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "x") == 0) {
        dl->AddLine(pt(6, 6), pt(18, 18), col, stroke);
        dl->AddLine(pt(6, 18), pt(18, 6), col, stroke);
    } else if (std::strcmp(kind, "plus") == 0) {
        dl->AddLine(pt(5, 12), pt(19, 12), col, stroke);
        dl->AddLine(pt(12, 5), pt(12, 19), col, stroke);
    } else if (std::strcmp(kind, "search") == 0) {
        dl->AddCircle(pt(11, 11), 6.8f * unit, col, 20, stroke);
        dl->AddLine(pt(20, 20), pt(15.8f, 15.8f), col, stroke);
    } else if (std::strcmp(kind, "download") == 0) {
        dl->AddLine(pt(12, 4), pt(12, 15.5f), col, stroke);
        ImVec2 a[3] = {pt(7.5f, 11.0f), pt(12, 15.5f), pt(16.5f, 11.0f)};
        dl->AddPolyline(a, 3, col, ImDrawFlags_None, stroke);
        dl->AddLine(pt(5, 19.5f), pt(19, 19.5f), col, stroke);
    } else if (std::strcmp(kind, "trash") == 0) {
        dl->AddLine(pt(4, 6.5f), pt(20, 6.5f), col, stroke);
        dl->AddLine(pt(9, 4.0f), pt(15, 4.0f), col, stroke);
        ImVec2 body[4] = {pt(6.5f, 6.5f), pt(17.5f, 6.5f), pt(16.5f, 20.0f), pt(7.5f, 20.0f)};
        dl->AddPolyline(body, 4, col, ImDrawFlags_Closed, stroke);
        dl->AddLine(pt(10.2f, 10.5f), pt(10.2f, 16.5f), col, stroke * 0.85f);
        dl->AddLine(pt(13.8f, 10.5f), pt(13.8f, 16.5f), col, stroke * 0.85f);
    } else if (std::strcmp(kind, "refresh") == 0) {
        dl->AddCircle(pt(12, 12), 7.2f * unit, col, 20, stroke);
        dl->AddTriangleFilled(pt(16.5f, 3.8f), pt(21.0f, 8.2f), pt(15.0f, 8.5f), col);
    } else if (std::strcmp(kind, "copy") == 0) {
        dl->AddRect(pt(8.5f, 8.5f), pt(19.5f, 19.5f), col, 1.5f, 0, stroke);
        ImVec2 b[3] = {pt(15.5f, 4.5f), pt(4.5f, 4.5f), pt(4.5f, 15.5f)};
        dl->AddPolyline(b, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "qr-code") == 0) {
        dl->AddRect(pt(3, 3), pt(9.5f, 9.5f), col, 1.0f, 0, stroke);
        dl->AddRect(pt(14.5f, 3), pt(21, 9.5f), col, 1.0f, 0, stroke);
        dl->AddRect(pt(3, 14.5f), pt(9.5f, 21), col, 1.0f, 0, stroke);
        dl->AddRectFilled(pt(14.5f, 14.5f), pt(17.5f, 17.5f), col);
        dl->AddRectFilled(pt(18.0f, 18.0f), pt(21.0f, 21.0f), col);
    } else if (std::strcmp(kind, "star") == 0) {
        constexpr double kPi = 3.14159265358979323846;
        // Exact pentagram inner/outer ratio (cos 72° / cos 36°) keeps upper arms collinear,
        // and vertical bbox offset centers the star visually between top and bottom tips.
        const double rOut = static_cast<double>(size) * 0.44;
        const double rIn  = rOut * 0.38196601125010515;
        const double cx   = static_cast<double>(center.x);
        const double cy   = static_cast<double>(center.y) + rOut * 0.0954915028125263;
        ImVec2 poly[10];
        for (int i = 0; i < 5; ++i) {
            const double aOut = -kPi * 0.5 + static_cast<double>(i) * (2.0 * kPi / 5.0);
            const double aIn  = aOut + (kPi / 5.0);
            poly[2 * i] = ImVec2(static_cast<float>(cx + std::cos(aOut) * rOut),
                                 static_cast<float>(cy + std::sin(aOut) * rOut));
            poly[2 * i + 1] = ImVec2(static_cast<float>(cx + std::cos(aIn) * rIn),
                                     static_cast<float>(cy + std::sin(aIn) * rIn));
        }
        dl->AddConcavePolyFilled(poly, 10, col);
    } else if (std::strcmp(kind, "zap") == 0) {
        ImVec2 p1[3] = {pt(13.5f, 2.5f), pt(4.5f, 13.5f), pt(12.0f, 13.5f)};
        ImVec2 p2[3] = {pt(10.5f, 21.5f), pt(19.5f, 10.5f), pt(12.0f, 10.5f)};
        dl->AddTriangleFilled(p1[0], p1[1], p1[2], col);
        dl->AddTriangleFilled(p2[0], p2[1], p2[2], col);
    }
}

} // namespace upix::ui
