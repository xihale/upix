#pragma once
#include "ui_theme.hpp"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace upix::ui {

inline float easeOutExpo(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return 1.0f - std::pow(2.0f, -10.0f * t);
}

// Draw crisp physical-pixel-aligned rounded box with fill and 1-physical-px hairline stroke
inline void drawBox(ImDrawList* dl, ImVec2 p0, ImVec2 p1, ImU32 fill, ImU32 border, float radius = 8.0f, ImDrawFlags flags = 0) {
    p0 = snapPt(p0);
    p1 = snapPt(p1);
    if ((fill >> IM_COL32_A_SHIFT) > 0) {
        dl->AddRectFilled(p0, p1, fill, radius, flags);
    }
    if ((border >> IM_COL32_A_SHIFT) > 0) {
        dl->AddRect(p0, p1, border, radius, flags, hairlinePx());
    }
}

// Draw corner registration marks (L-shaped film-carrier marks — Figma 4:19..4:26: 16x2px, #343440)
inline void drawRegistrationMarks(ImDrawList* dl, ImVec2 boxMin, ImVec2 boxMax, float inset, float armLen, float thickness, ImU32 color) {
    boxMin = snapPt(boxMin);
    boxMax = snapPt(boxMax);
    inset = snapPx(inset);
    armLen = snapPx(armLen);
    thickness = std::max(hairlinePx(), snapPx(thickness));
    // Top-left
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMin.y + inset),
                      ImVec2(boxMin.x + inset + armLen, boxMin.y + inset + thickness), color);
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMin.y + inset),
                      ImVec2(boxMin.x + inset + thickness, boxMin.y + inset + armLen), color);
    // Top-right
    dl->AddRectFilled(ImVec2(boxMax.x - inset - armLen, boxMin.y + inset),
                      ImVec2(boxMax.x - inset, boxMin.y + inset + thickness), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - thickness, boxMin.y + inset),
                      ImVec2(boxMax.x - inset, boxMin.y + inset + armLen), color);
    // Bottom-left
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMax.y - inset - thickness),
                      ImVec2(boxMin.x + inset + armLen, boxMax.y - inset), color);
    dl->AddRectFilled(ImVec2(boxMin.x + inset, boxMax.y - inset - armLen),
                      ImVec2(boxMin.x + inset + thickness, boxMax.y - inset), color);
    // Bottom-right
    dl->AddRectFilled(ImVec2(boxMax.x - inset - armLen, boxMax.y - inset - thickness),
                      ImVec2(boxMax.x - inset, boxMax.y - inset), color);
    dl->AddRectFilled(ImVec2(boxMax.x - inset - thickness, boxMax.y - inset - armLen),
                      ImVec2(boxMax.x - inset, boxMax.y - inset), color);
}

// Vector icons drawn with ImDrawList (24x24 Lucide coordinates scaled to rect)
inline void drawIcon(ImDrawList* dl, const char* kind, ImVec2 center, float size, ImU32 col, float stroke = 1.6f) {
    auto pt = [&](float vx, float vy) -> ImVec2 {
        return ImVec2(center.x + (vx - 12.0f) / 24.0f * size,
                      center.y + (vy - 12.0f) / 24.0f * size);
    };
    if (std::strcmp(kind, "zoom-in") == 0) {
        dl->AddCircle(pt(11, 11), 7.5f / 24.0f * size, col, 28, stroke);
        dl->AddLine(pt(21, 21), pt(16.4f, 16.4f), col, stroke);
        dl->AddLine(pt(11, 8), pt(11, 14), col, stroke);
        dl->AddLine(pt(8, 11), pt(14, 11), col, stroke);
    } else if (std::strcmp(kind, "layers") == 0) {
        ImVec2 top[4] = {pt(12, 3), pt(21, 7.5f), pt(12, 12), pt(3, 7.5f)};
        dl->AddPolyline(top, 4, col, ImDrawFlags_Closed, stroke);
        ImVec2 mid[3] = {pt(3, 12.5f), pt(12, 17), pt(21, 12.5f)};
        dl->AddPolyline(mid, 3, col, ImDrawFlags_None, stroke);
        ImVec2 bot[3] = {pt(3, 17), pt(12, 21.5f), pt(21, 17)};
        dl->AddPolyline(bot, 3, col, ImDrawFlags_None, stroke);
    } else if (std::strcmp(kind, "smartphone") == 0) {
        dl->AddRect(pt(6.5f, 2.5f), pt(17.5f, 21.5f), col, 2.2f / 24.0f * size, 0, stroke);
        dl->AddCircleFilled(pt(12, 18.2f), 1.15f, col);
    } else if (std::strcmp(kind, "folder-open") == 0 || std::strcmp(kind, "folder") == 0) {
        ImVec2 p[6] = {pt(3, 6), pt(9, 6), pt(11, 8.5f), pt(21, 8.5f), pt(21, 19), pt(3, 19)};
        dl->AddPolyline(p, 6, col, ImDrawFlags_Closed, stroke);
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
        dl->AddCircle(pt(11, 11), 6.8f / 24.0f * size, col, 20, stroke);
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
        dl->AddCircle(pt(12, 12), 7.2f / 24.0f * size, col, 20, stroke);
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
        ImVec2 outer[5], inner[5];
        float rOut = size * 0.44f, rIn = size * 0.19f;
        for (int i = 0; i < 5; ++i) {
            float aOut = -1.570796f + i * 1.256637f;
            float aIn  = aOut + 0.628318f;
            outer[i] = ImVec2(center.x + std::cos(aOut) * rOut, center.y + std::sin(aOut) * rOut);
            inner[i] = ImVec2(center.x + std::cos(aIn) * rIn, center.y + std::sin(aIn) * rIn);
        }
        for (int i = 0; i < 5; ++i) {
            dl->AddTriangleFilled(center, outer[i], inner[i], col);
            dl->AddTriangleFilled(center, inner[i], outer[(i + 1) % 5], col);
        }
    } else if (std::strcmp(kind, "zap") == 0) {
        ImVec2 p1[3] = {pt(13.5f, 2.5f), pt(4.5f, 13.5f), pt(12.0f, 13.5f)};
        ImVec2 p2[3] = {pt(10.5f, 21.5f), pt(19.5f, 10.5f), pt(12.0f, 10.5f)};
        dl->AddTriangleFilled(p1[0], p1[1], p1[2], col);
        dl->AddTriangleFilled(p2[0], p2[1], p2[2], col);
    }
}

// Text helper with physical-pixel position snapping
inline void drawText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text) {
    dl->AddText(font, size, snapPt(pos), col, text);
}

inline ImVec2 calcText(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

inline void drawTextCentered(ImDrawList* dl, ImFont* font, float size, ImVec2 rectMin, ImVec2 rectMax, ImU32 col, const char* text) {
    ImVec2 sz = calcText(font, size, text);
    ImVec2 pos = snapPt(rectMin.x + (rectMax.x - rectMin.x - sz.x) * 0.5f,
                        rectMin.y + (rectMax.y - rectMin.y - sz.y) * 0.5f);
    dl->AddText(font, size, pos, col, text);
}

// Segmented control matching Figma "Desktop / Segmented" (#151519 track, #25252D border, #2B2B34 active)
struct SegmentOption {
    const char* label;
    const char* value;
};

inline bool segmentedControl(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size,
                             const SegmentOption* opts, int count, std::string& current,
                             const Palette& pal, ImFont* font, float fontSize = 12.0f) {
    bool changed = false;
    pos = snapPt(pos);
    drawBox(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), pal.recessed(), hex(0x25252D), 8.0f);

    float pad = 2.0f;
    float innerW = size.x - pad * 2.0f;
    float segW = innerW / static_cast<float>(count);
    float segH = size.y - pad * 2.0f;

    int activeIdx = 0;
    for (int i = 0; i < count; ++i) {
        if (current == opts[i].value) {
            activeIdx = i;
            break;
        }
    }

    for (int i = 0; i < count; ++i) {
        ImVec2 s0 = snapPt(pos.x + pad + i * segW, pos.y + pad);
        ImVec2 s1 = snapPt(pos.x + pad + (i + 1) * segW, pos.y + pad + segH);

        ImGui::SetCursorScreenPos(s0);
        ImGui::PushID(id);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##seg", ImVec2(s1.x - s0.x, segH));
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        ImGui::PopID();

        if (clicked && current != opts[i].value) {
            current = opts[i].value;
            activeIdx = i;
            changed = true;
        }

        bool isSelected = (i == activeIdx);
        if (isSelected) {
            drawBox(dl, s0, s1, pal.segSelected(), pal.segBorder(), 6.0f);
        } else if (hovered) {
            drawBox(dl, s0, s1, pal.containerLow(), 0, 6.0f);
        }

        // Hairline divider between unselected neighbors (Figma: 1x12px #282832)
        if (i > 0 && i != activeIdx && (i - 1) != activeIdx) {
            float divX = snapPx(s0.x);
            float divY0 = snapPx(pos.y + (size.y - 12.0f) * 0.5f);
            dl->AddRectFilled(ImVec2(divX, divY0), ImVec2(divX + hairlinePx(), divY0 + 12.0f), hex(0x282832));
        }

        ImU32 ink = isSelected ? pal.onSurface() : (hovered ? pal.onSurface() : pal.muted());
        drawTextCentered(dl, font, fontSize, s0, s1, ink, opts[i].label);
    }
    return changed;
}

// Quality slider with 5 tick marks matching Figma "Desktop / Slider / Quality" (height 18px)
inline bool qualitySlider(ImDrawList* dl, const char* id, ImVec2 pos, float width,
                          int& value, int minVal, int maxVal, const Palette& pal) {
    bool changed = false;
    pos = snapPt(pos);
    ImVec2 sz(width, 18.0f);
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton(id, sz);
    bool active = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    if (active) {
        float mx = ImGui::GetIO().MousePos.x;
        float t = (mx - pos.x) / width;
        t = std::clamp(t, 0.0f, 1.0f);
        int next = static_cast<int>(std::round(minVal + t * (maxVal - minVal)));
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    float t = std::clamp(static_cast<float>(value - minVal) / static_cast<float>(maxVal - minVal), 0.0f, 1.0f);
    float trackY = snapPx(pos.y + 8.0f);
    float fillW = snapPx(t * width);
    // Track background (2px, #2D2D34)
    dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + width, trackY + 2.0f), pal.outlineInput(), 1.0f);
    // Track progress (2px primary)
    if (fillW > 0.0f) {
        dl->AddRectFilled(ImVec2(pos.x, trackY), ImVec2(pos.x + fillW, trackY + 2.0f), pal.primary(), 1.0f);
    }
    // 5 Tick marks underneath (1x3px at top:13px, #3A3A44)
    for (int i = 0; i < 5; ++i) {
        float tx = snapPx(pos.x + 1.0f + (width - 2.0f) * (i / 4.0f));
        dl->AddRectFilled(ImVec2(tx, snapPx(pos.y + 13.0f)), ImVec2(tx + hairlinePx(), snapPx(pos.y + 16.0f)), hex(0x3A3A44));
    }
    // Thumb (12x12 circle, #131316 fill + 2.5px #7FDAC9 stroke)
    float thumbX = std::clamp(pos.x + fillW, pos.x + 6.0f, pos.x + width - 6.0f);
    ImVec2 center = snapPt(thumbX, trackY + 1.0f);
    dl->AddCircleFilled(center, 6.0f, pal.bg(), 24);
    dl->AddCircle(center, 5.2f, pal.primary(), 24, 2.2f);

    return changed;
}

// Toggle switch matching Figma "Desktop / Toggle" (44x26, 20x20 thumb, 3px padding)
inline bool toggleSwitch(ImDrawList* dl, const char* id, ImVec2 pos, bool& checked, const Palette& pal) {
    pos = snapPt(pos);
    ImVec2 sz(44.0f, 26.0f);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton(id, sz);
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (clicked) checked = !checked;

    ImU32 fill = checked ? pal.toggleOnBg() : pal.high();
    ImU32 border = checked ? pal.toggleOnBorder() : pal.outlineInput();
    drawBox(dl, pos, ImVec2(pos.x + sz.x, pos.y + sz.y), fill, border, 13.0f);

    float cx = checked ? (pos.x + sz.x - 13.0f) : (pos.x + 13.0f);
    float cy = pos.y + 13.0f;
    ImU32 thumbCol = checked ? pal.primary() : pal.onSurface();
    dl->AddCircleFilled(snapPt(cx, cy), 9.5f, thumbCol, 24);

    return clicked;
}

// Square icon button (32x32 or custom size)
inline bool iconButton(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size,
                       const char* icon, const Palette& pal, float iconSz = 14.0f, float radius = 7.0f) {
    pos = snapPt(pos);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImU32 fill = hovered ? pal.high() : pal.containerLow();
    ImU32 border = hovered ? pal.outlineStrong() : pal.outline();
    ImU32 ink = hovered ? pal.onSurface() : pal.muted();
    drawBox(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), fill, border, radius);
    drawIcon(dl, icon, ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f), iconSz, ink);
    return clicked;
}

// Pill button
inline bool pillButton(ImDrawList* dl, const char* id, ImVec2 pos, ImVec2 size,
                       const char* label, const char* icon, const Palette& pal,
                       ImFont* font, bool primaryStyle = false, float radius = 8.0f, float fontSize = 13.0f, bool mutedText = false) {
    pos = snapPt(pos);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    ImU32 fill = primaryStyle ? pal.primary() : (hovered ? pal.high() : pal.containerLow());
    ImU32 border = primaryStyle ? 0 : (hovered ? pal.outlineStrong() : pal.outlineInput());
    ImU32 ink = primaryStyle ? pal.onPrimary() : (hovered ? pal.onSurface() : (mutedText ? pal.muted() : pal.onSurface()));

    drawBox(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), fill, border, radius);

    if (icon && icon[0] != '\0') {
        ImVec2 tsz = calcText(font, fontSize, label);
        float iconW = 15.0f;
        float gap = 8.0f;
        float totalW = iconW + gap + tsz.x;
        float startX = snapPx(pos.x + (size.x - totalW) * 0.5f);
        drawIcon(dl, icon, ImVec2(startX + iconW * 0.5f, pos.y + size.y * 0.5f), iconW, ink);
        drawText(dl, font, fontSize, ImVec2(startX + iconW + gap, pos.y + (size.y - tsz.y) * 0.5f), ink, label);
    } else {
        drawTextCentered(dl, font, fontSize, pos, ImVec2(pos.x + size.x, pos.y + size.y), ink, label);
    }
    return clicked;
}

} // namespace upix::ui
