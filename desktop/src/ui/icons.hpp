// ui/icons.hpp — 24x24 Lucide-style crisp vector icon catalog + CJK baseline alignment.
#pragma once

#include "ui/geist.hpp"
#include "ui/motion.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace upix::ui {

// Text is optically centered at box midpoint via glyph ink bounds, so paired
// inline icons share the exact vertical midpoint without an ad-hoc offset.
inline float cjkIconCenterY(ImVec2 boxMin, ImVec2 boxMax) {
    return (boxMin.y + boxMax.y) * 0.5f;
}

// Unified brand mark matching Android ic_launcher_foreground / ic_launcher_monochrome:
// 24dp glyph bounds [2, 22.75] x [1.25, 22] mapped full-bleed into [0, size]^2 in t.ink.
inline void drawBrandMark(ImDrawList* dl, ImVec2 p0, float size) {
    if (!dl || size <= 0.0f) return;
    const auto& t = geist::tokens();
    const float s = size / 20.75f;
    p0 = snapPt(p0);
    auto pt = [&](float vx, float vy) -> ImVec2 {
        return ImVec2(p0.x + (vx - 2.0f) * s, p0.y + (vy - 1.25f) * s);
    };

    auto pushCubic = [&](auto& pts, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, int steps) {
        for (int i = 1; i <= steps; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(steps);
            const float v = 1.0f - u;
            const float b0 = v * v * v;
            const float b1 = 3.0f * v * v * u;
            const float b2 = 3.0f * v * u * u;
            const float b3 = u * u * u;
            pts.push_back(ImVec2(b0 * a.x + b1 * b.x + b2 * c.x + b3 * d.x,
                                 b0 * a.y + b1 * b.y + b2 * c.y + b3 * d.y));
        }
    };

    ImVector<ImVec2> bodyPts;
    bodyPts.reserve(64);
    bodyPts.push_back(pt(19.7516f, 7.43034f));
    pushCubic(bodyPts, pt(19.7516f, 7.43034f), pt(19.797f, 8.62969f), pt(20.7812f, 9.5887f), pt(21.9904f, 9.59373f), 4);
    pushCubic(bodyPts, pt(21.9904f, 9.59373f), pt(22.0f, 10.3198f), pt(22.0f, 11.1187f), pt(22.0f, 12.0f), 2);
    pushCubic(bodyPts, pt(22.0f, 12.0f), pt(22.0f, 16.714f), pt(22.0f, 19.0711f), pt(20.5355f, 20.5355f), 5);
    pushCubic(bodyPts, pt(20.5355f, 20.5355f), pt(19.0711f, 22.0f), pt(16.714f, 22.0f), pt(12.0f, 22.0f), 5);
    pushCubic(bodyPts, pt(12.0f, 22.0f), pt(7.28595f, 22.0f), pt(4.92893f, 22.0f), pt(3.46447f, 20.5355f), 5);
    pushCubic(bodyPts, pt(3.46447f, 20.5355f), pt(2.0f, 19.0711f), pt(2.0f, 16.714f), pt(2.0f, 12.0f), 5);
    pushCubic(bodyPts, pt(2.0f, 12.0f), pt(2.0f, 7.28595f), pt(2.0f, 4.92893f), pt(3.46447f, 3.46447f), 5);
    pushCubic(bodyPts, pt(3.46447f, 3.46447f), pt(4.92893f, 2.0f), pt(7.28595f, 2.0f), pt(12.0f, 2.0f), 5);
    pushCubic(bodyPts, pt(12.0f, 2.0f), pt(12.8813f, 2.0f), pt(13.6802f, 2.0f), pt(14.4063f, 2.00957f), 2);
    pushCubic(bodyPts, pt(14.4063f, 2.00957f), pt(14.4113f, 3.21884f), pt(15.3703f, 4.20298f), pt(16.5697f, 4.24836f), 4);
    bodyPts.push_back(pt(13.9468f, 6.87122f));
    pushCubic(bodyPts, pt(13.9468f, 6.87122f), pt(13.5572f, 6.20078f), pt(12.8313f, 5.75f), pt(12.0f, 5.75f), 3);
    pushCubic(bodyPts, pt(12.0f, 5.75f), pt(10.7574f, 5.75f), pt(9.75f, 6.75736f), pt(9.75f, 8.0f), 4);
    bodyPts.push_back(pt(9.75f, 12.0f));
    pushCubic(bodyPts, pt(9.75f, 12.0f), pt(9.75f, 13.2426f), pt(10.7574f, 14.25f), pt(12.0f, 14.25f), 4);
    bodyPts.push_back(pt(16.0f, 14.25f));
    pushCubic(bodyPts, pt(16.0f, 14.25f), pt(17.2426f, 14.25f), pt(18.25f, 13.2426f), pt(18.25f, 12.0f), 4);
    pushCubic(bodyPts, pt(18.25f, 12.0f), pt(18.25f, 11.1687f), pt(17.7992f, 10.4428f), pt(17.1288f, 10.0532f), 3);
    dl->AddConcavePolyFilled(bodyPts.Data, bodyPts.Size, t.ink);

    // Double-headed upscale arrow (clockwise in Y-down screen space for ImGui AA fringe)
    ImVector<ImVec2> arrowPts;
    arrowPts.reserve(48);
    arrowPts.push_back(pt(15.9062f, 2.0f));
    pushCubic(arrowPts, pt(15.9062f, 2.0f), pt(15.9062f, 1.58579f), pt(16.242f, 1.25f), pt(16.6562f, 1.25f), 3);
    arrowPts.push_back(pt(22.0f, 1.25f));
    pushCubic(arrowPts, pt(22.0f, 1.25f), pt(22.4142f, 1.25f), pt(22.75f, 1.58579f), pt(22.75f, 2.0f), 3);
    arrowPts.push_back(pt(22.75f, 7.34375f));
    pushCubic(arrowPts, pt(22.75f, 7.34375f), pt(22.75f, 7.75796f), pt(22.4142f, 8.09375f), pt(22.0f, 8.09375f), 3);
    pushCubic(arrowPts, pt(22.0f, 8.09375f), pt(21.5858f, 8.09375f), pt(21.25f, 7.75796f), pt(21.25f, 7.34375f), 3);
    arrowPts.push_back(pt(21.25f, 3.81066f));
    arrowPts.push_back(pt(13.8107f, 11.25f));
    arrowPts.push_back(pt(16.0f, 11.25f));
    pushCubic(arrowPts, pt(16.0f, 11.25f), pt(16.4142f, 11.25f), pt(16.75f, 11.5858f), pt(16.75f, 12.0f), 3);
    pushCubic(arrowPts, pt(16.75f, 12.0f), pt(16.75f, 12.4142f), pt(16.4142f, 12.75f), pt(16.0f, 12.75f), 3);
    arrowPts.push_back(pt(12.0f, 12.75f));
    pushCubic(arrowPts, pt(12.0f, 12.75f), pt(11.5858f, 12.75f), pt(11.25f, 12.4142f), pt(11.25f, 12.0f), 3);
    arrowPts.push_back(pt(11.25f, 8.0f));
    pushCubic(arrowPts, pt(11.25f, 8.0f), pt(11.25f, 7.58579f), pt(11.5858f, 7.25f), pt(12.0f, 7.25f), 3);
    pushCubic(arrowPts, pt(12.0f, 7.25f), pt(12.4142f, 7.25f), pt(12.75f, 7.58579f), pt(12.75f, 8.0f), 3);
    arrowPts.push_back(pt(12.75f, 10.1893f));
    arrowPts.push_back(pt(20.1893f, 2.75f));
    arrowPts.push_back(pt(16.6562f, 2.75f));
    pushCubic(arrowPts, pt(16.6562f, 2.75f), pt(16.242f, 2.75f), pt(15.9062f, 2.41421f), pt(15.9062f, 2.0f), 3);
    dl->AddConcavePolyFilled(arrowPts.Data, arrowPts.Size, t.ink);
}

inline void drawIcon(ImDrawList* dl, const char* kind, ImVec2 center, float size, ImU32 col, float stroke = 1.5f) {
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
    } else if (std::strcmp(kind, "alert-circle") == 0 || std::strcmp(kind, "alert") == 0) {
        dl->AddCircle(pt(12, 12), 9.0f * unit, col, 24, stroke);
        dl->AddLine(pt(12, 8.0f), pt(12, 13.0f), col, stroke);
        dl->AddCircleFilled(pt(12, 16.2f), std::max(0.9f, 1.0f * unit), col);
    } else if (std::strcmp(kind, "spinner") == 0) {
        const bool animating = ImGui::GetCurrentContext() && !motion::reduceMotion();
        const float a0 = animating ? static_cast<float>(ImGui::GetTime()) * 5.0f : 0.0f;
        if (animating) motion::markMotionActive();
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
        ImVec2 p[3] = {pt(5, 12.5f), pt(9.5f, 17.0f), pt(19, 7.5f)};
        dl->AddPolyline(p, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "x") == 0 || std::strcmp(kind, "close") == 0) {
        dl->AddLine(pt(6, 6), pt(18, 18), col, stroke);
        dl->AddLine(pt(6, 18), pt(18, 6), col, stroke);
    } else if (std::strcmp(kind, "win-minimize") == 0) {
        dl->AddLine(pt(6, 12), pt(18, 12), col, stroke);
    } else if (std::strcmp(kind, "win-maximize") == 0) {
        dl->AddRect(pt(6.0f, 6.0f), pt(18.0f, 18.0f), col, 1.5f * unit, 0, stroke);
    } else if (std::strcmp(kind, "win-restore") == 0) {
        dl->AddRect(pt(5.5f, 8.5f), pt(15.5f, 18.5f), col, 1.2f * unit, 0, stroke);
        ImVec2 back[4] = {pt(8.5f, 8.5f), pt(8.5f, 5.5f), pt(18.5f, 5.5f), pt(18.5f, 15.5f)};
        dl->AddPolyline(back, 4, col, ImDrawFlags_None, stroke);
        dl->AddLine(pt(15.5f, 15.5f), pt(18.5f, 15.5f), col, stroke);
    } else if (std::strcmp(kind, "plus") == 0) {
        dl->AddLine(pt(5, 12), pt(19, 12), col, stroke);
        dl->AddLine(pt(12, 5), pt(12, 19), col, stroke);
    } else if (std::strcmp(kind, "search") == 0) {
        dl->AddCircle(pt(11, 11), 6.5f * unit, col, 24, stroke);
        dl->AddLine(pt(16, 16), pt(20.5f, 20.5f), col, stroke);
    } else if (std::strcmp(kind, "download") == 0) {
        dl->AddLine(pt(12, 4), pt(12, 16), col, stroke);
        ImVec2 a[3] = {pt(7, 11), pt(12, 16), pt(17, 11)};
        dl->AddPolyline(a, 3, col, ImDrawFlags_None, stroke);
        dl->AddLine(pt(4, 20), pt(20, 20), col, stroke);
    } else if (std::strcmp(kind, "trash") == 0) {
        dl->AddLine(pt(4, 7), pt(20, 7), col, stroke);
        ImVec2 handle[4] = {pt(9, 7), pt(9, 4), pt(15, 4), pt(15, 7)};
        dl->AddPolyline(handle, 4, col, ImDrawFlags_None, stroke);
        ImVec2 body[4] = {pt(6.5f, 7), pt(7.5f, 20), pt(16.5f, 20), pt(17.5f, 7)};
        dl->AddPolyline(body, 4, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "refresh") == 0 || std::strcmp(kind, "rotate-cw") == 0) {
        dl->PathClear();
        dl->PathArcTo(center, 7.8f * unit, 0.22f, 5.45f, 20);
        dl->PathLineTo(pt(19.5f, 8.3f));
        dl->PathStroke(col, ImDrawFlags_None, stroke);
        ImVec2 arr[3] = {pt(19.5f, 4.0f), pt(19.5f, 8.3f), pt(15.2f, 8.3f)};
        dl->AddPolyline(arr, 3, col, ImDrawFlags_None, stroke);
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
    } else if (std::strcmp(kind, "sun") == 0) {
        constexpr float kPi = 3.14159265358979323846f;
        dl->AddCircle(pt(12, 12), 4.0f * unit, col, 24, stroke);
        for (int i = 0; i < 8; ++i) {
            const float a = static_cast<float>(i) * (kPi * 0.25f);
            const ImVec2 p0(center.x + std::cos(a) * 7.0f * unit, center.y + std::sin(a) * 7.0f * unit);
            const ImVec2 p1(center.x + std::cos(a) * 9.5f * unit, center.y + std::sin(a) * 9.5f * unit);
            dl->AddLine(p0, p1, col, stroke);
        }
    } else if (std::strcmp(kind, "contrast") == 0) {
        constexpr float kPi = 3.14159265358979323846f;
        dl->AddCircle(pt(12, 12), 8.5f * unit, col, 24, stroke);
        dl->PathClear();
        dl->PathArcTo(pt(12, 12), 8.5f * unit, -kPi * 0.5f, kPi * 0.5f, 16);
        dl->PathFillConvex(col);
    } else if (std::strcmp(kind, "moon") == 0) {
        constexpr float kTwoPi = 6.28318530717958647692f;
        constexpr float R = 8.5f;
        constexpr float r = 6.25f;
        constexpr float ox = 16.5f;
        constexpr float oy = 7.5f;
        const float dx = ox - 12.0f;
        const float dy = oy - 12.0f;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float a = (R * R - r * r + d * d) / (2.0f * d);
        const float theta = std::atan2(dy, dx);
        const float delta = std::acos(std::clamp(a / R, -1.0f, 1.0f));
        const float a1Top = theta - delta;
        const float a1BotCw = theta + delta;
        const float a1Bot = a1BotCw - kTwoPi;
        const float pTopX = 12.0f + R * std::cos(a1Top);
        const float pTopY = 12.0f + R * std::sin(a1Top);
        const float pBotX = 12.0f + R * std::cos(a1BotCw);
        const float pBotY = 12.0f + R * std::sin(a1BotCw);
        const float a2Bot = std::atan2(pBotY - oy, pBotX - ox);
        float a2Top = std::atan2(pTopY - oy, pTopX - ox);
        if (a2Top < a2Bot) a2Top += kTwoPi;
        dl->PathClear();
        dl->PathArcTo(pt(12, 12), R * unit, a1Top, a1Bot, 22);
        dl->PathArcTo(pt(ox, oy), r * unit, a2Bot, a2Top, 14);
        dl->PathStroke(col, ImDrawFlags_Closed, stroke);
    } else if (std::strcmp(kind, "arrow-right") == 0) {
        dl->AddLine(pt(5, 12), pt(19, 12), col, stroke);
        ImVec2 h[3] = {pt(12.5f, 5.5f), pt(19, 12), pt(12.5f, 18.5f)};
        dl->AddPolyline(h, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "arrow-left") == 0) {
        dl->AddLine(pt(19, 12), pt(5, 12), col, stroke);
        ImVec2 h[3] = {pt(11.5f, 5.5f), pt(5, 12), pt(11.5f, 18.5f)};
        dl->AddPolyline(h, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "arrow-up") == 0) {
        dl->AddLine(pt(12, 19), pt(12, 5), col, stroke);
        ImVec2 h[3] = {pt(5.5f, 11.5f), pt(12, 5), pt(18.5f, 11.5f)};
        dl->AddPolyline(h, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "arrow-down") == 0) {
        dl->AddLine(pt(12, 5), pt(12, 19), col, stroke);
        ImVec2 h[3] = {pt(5.5f, 12.5f), pt(12, 19), pt(18.5f, 12.5f)};
        dl->AddPolyline(h, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "upload") == 0) {
        dl->AddLine(pt(12, 16), pt(12, 4), col, stroke);
        ImVec2 a[3] = {pt(7, 9), pt(12, 4), pt(17, 9)};
        dl->AddPolyline(a, 3, col, ImDrawFlags_None, stroke);
        dl->AddLine(pt(4, 18), pt(20, 18), col, stroke);
    } else if (std::strcmp(kind, "info") == 0) {
        dl->AddCircle(pt(12, 12), 9.0f * unit, col, 24, stroke);
        dl->AddCircleFilled(pt(12, 8.2f), std::max(0.9f, 1.0f * unit), col);
        dl->AddLine(pt(12, 11.5f), pt(12, 16.5f), col, stroke);
    } else if (std::strcmp(kind, "sliders") == 0) {
        dl->AddLine(pt(4, 7), pt(10, 7), col, stroke);
        dl->AddLine(pt(14, 7), pt(20, 7), col, stroke);
        dl->AddCircle(pt(12, 7), 2.0f * unit, col, 14, stroke);
        dl->AddLine(pt(4, 17), pt(8, 17), col, stroke);
        dl->AddLine(pt(12, 17), pt(20, 17), col, stroke);
        dl->AddCircle(pt(10, 17), 2.0f * unit, col, 14, stroke);
    }
}

} // namespace upix::ui
