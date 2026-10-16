#pragma once
// Shared ImGui widgets and styling for all screens (Cell Lab palette: charcoal panels, orange accent).
#include "sim/genome.hpp"
#include "sim/world.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace uiw {


inline const ImVec4 kAccent(1.00f, 0.62f, 0.20f, 1.0f);  // original game orange
inline const ImU32 kAccentU32 = IM_COL32(255, 158, 51, 255);
inline const ImU32 kGreenU32 = IM_COL32(110, 190, 120, 255);  // divider green of the original
inline const char* kTypeAbbrev[kCellTypeCount] = {"Pg", "Fl", "Ph", "Dv", "Lp", "Kr", "By", "Gl", "Vr",
                                           "Nt", "St", "Sn", "My", "Nr", "Sc", "Sm", "Gm", "Cl", "El", "Cn", "Ch"};

inline ImU32 colU32(Color c, float a = 1.0f) {
    return IM_COL32((int)(clampv(c.r, 0.f, 1.f) * 255), (int)(clampv(c.g, 0.f, 1.f) * 255), (int)(clampv(c.b, 0.f, 1.f) * 255),
                    (int)(a * 255));
}

enum Icon { IcFreeze, IcSlow, IcPlay, IcFast, IcStep, IcInspect, IcSynth, IcTweezers, IcBoost, IcRemove, IcPanel, IcHelp,
            IcShock, IcGeneNeedle, IcDrugNeedle, IcMap, IcAdhesin, IcUndo, IcRedo, IcFit, IcMenu, IcToStart, IcToEnd, IcPrev,
            IcNext, IcCell, IcStats, IcFlag, IcFlashlight };

inline void drawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float s, ImU32 col) {
    float t = std::max(1.5f, s * 0.11f);  // stroke
    switch (ic) {
        case IcFreeze:
            for (int i = 0; i < 3; ++i) {
                float a = i * kPi / 3.0f;
                ImVec2 d(std::cos(a) * s * 0.42f, std::sin(a) * s * 0.42f);
                dl->AddLine({c.x - d.x, c.y - d.y}, {c.x + d.x, c.y + d.y}, col, t);
            }
            break;
        case IcSlow:
            dl->AddTriangleFilled({c.x - s * 0.22f, c.y - s * 0.3f}, {c.x - s * 0.22f, c.y + s * 0.3f}, {c.x + s * 0.28f, c.y}, col);
            dl->AddLine({c.x - s * 0.42f, c.y - s * 0.3f}, {c.x - s * 0.42f, c.y + s * 0.3f}, col, t);
            break;
        case IcPlay:
            dl->AddTriangleFilled({c.x - s * 0.28f, c.y - s * 0.36f}, {c.x - s * 0.28f, c.y + s * 0.36f}, {c.x + s * 0.36f, c.y}, col);
            break;
        case IcFast:
            for (int i = 0; i < 2; ++i) {
                float o = (i - 0.5f) * s * 0.36f;
                dl->AddTriangleFilled({c.x - s * 0.2f + o, c.y - s * 0.32f}, {c.x - s * 0.2f + o, c.y + s * 0.32f},
                                      {c.x + s * 0.2f + o, c.y}, col);
            }
            break;
        case IcStep:
            dl->AddTriangleFilled({c.x - s * 0.3f, c.y - s * 0.3f}, {c.x - s * 0.3f, c.y + s * 0.3f}, {c.x + s * 0.18f, c.y}, col);
            dl->AddLine({c.x + s * 0.3f, c.y - s * 0.3f}, {c.x + s * 0.3f, c.y + s * 0.3f}, col, t);
            break;
        case IcInspect:
            dl->AddCircle({c.x - s * 0.08f, c.y - s * 0.08f}, s * 0.26f, col, 20, t);
            dl->AddLine({c.x + s * 0.12f, c.y + s * 0.12f}, {c.x + s * 0.38f, c.y + s * 0.38f}, col, t * 1.4f);
            break;
        case IcSynth:
            dl->AddCircle(c, s * 0.36f, col, 24, t);
            dl->AddLine({c.x - s * 0.18f, c.y}, {c.x + s * 0.18f, c.y}, col, t);
            dl->AddLine({c.x, c.y - s * 0.18f}, {c.x, c.y + s * 0.18f}, col, t);
            break;
        case IcTweezers:
            dl->AddLine({c.x - s * 0.3f, c.y - s * 0.38f}, {c.x + s * 0.02f, c.y + s * 0.32f}, col, t);
            dl->AddLine({c.x + s * 0.3f, c.y - s * 0.38f}, {c.x - s * 0.02f, c.y + s * 0.32f}, col, t);
            dl->AddCircleFilled({c.x, c.y + s * 0.36f}, t * 0.9f, col);
            break;
        case IcBoost: {  // a cell with an arrow up: more mass and nitrogen
            dl->AddCircle(c, s * 0.38f, col, 28, t);
            ImVec2 head[3] = {{c.x, c.y - s * 0.24f}, {c.x - s * 0.17f, c.y - s * 0.02f}, {c.x + s * 0.17f, c.y - s * 0.02f}};
            dl->AddTriangleFilled(head[0], head[1], head[2], col);
            dl->AddRectFilled({c.x - s * 0.06f, c.y - s * 0.04f}, {c.x + s * 0.06f, c.y + s * 0.24f}, col);
            break;
        }
        case IcRemove:
            dl->AddCircle(c, s * 0.36f, col, 24, t);
            dl->AddLine({c.x - s * 0.16f, c.y - s * 0.16f}, {c.x + s * 0.16f, c.y + s * 0.16f}, col, t);
            dl->AddLine({c.x + s * 0.16f, c.y - s * 0.16f}, {c.x - s * 0.16f, c.y + s * 0.16f}, col, t);
            break;
        case IcPanel:
            dl->AddRect({c.x - s * 0.36f, c.y - s * 0.3f}, {c.x + s * 0.36f, c.y + s * 0.3f}, col, 3, 0, t);
            dl->AddLine({c.x + s * 0.08f, c.y - s * 0.3f}, {c.x + s * 0.08f, c.y + s * 0.3f}, col, t);
            break;
        case IcShock: {  // lightning bolt
            ImVec2 p[6] = {{c.x + s * 0.06f, c.y - s * 0.42f}, {c.x - s * 0.24f, c.y + s * 0.04f}, {c.x - s * 0.02f, c.y + s * 0.04f},
                           {c.x - s * 0.08f, c.y + s * 0.42f}, {c.x + s * 0.24f, c.y - s * 0.06f}, {c.x + s * 0.02f, c.y - s * 0.06f}};
            dl->AddConvexPolyFilled(p, 3, col);
            ImVec2 q[3] = {p[3], p[4], p[5]};
            dl->AddConvexPolyFilled(q, 3, col);
            dl->AddQuadFilled(p[1], p[2], p[4], p[5], col);
            break;
        }
        case IcGeneNeedle:
        case IcDrugNeedle: {  // syringe; the gene needle carries a DNA helix, the drug needle a drop
            ImVec2 a{c.x - s * 0.36f, c.y + s * 0.36f}, b{c.x + s * 0.12f, c.y - s * 0.12f};
            dl->AddLine(a, {c.x - s * 0.18f, c.y + s * 0.18f}, col, t * 0.8f);                       // needle
            dl->AddLine({c.x - s * 0.18f, c.y + s * 0.18f}, b, col, t * 2.6f);                       // barrel
            dl->AddLine({c.x + s * 0.06f, c.y - s * 0.30f}, {c.x + s * 0.30f, c.y - s * 0.06f}, col, t);  // grip
            if (ic == IcGeneNeedle) {
                for (int k = 0; k < 3; ++k) {
                    float y = c.y - s * 0.40f + k * s * 0.11f;
                    dl->AddLine({c.x + s * 0.22f, y}, {c.x + s * 0.42f, y + s * 0.06f}, col, t * 0.6f);
                }
            } else {
                dl->AddCircleFilled({c.x + s * 0.34f, c.y - s * 0.30f}, s * 0.09f, col);
            }
            break;
        }
        case IcMap: {  // a wall being drawn with a pen
            dl->AddLine({c.x - s * 0.42f, c.y + s * 0.30f}, {c.x + s * 0.05f, c.y + s * 0.30f}, col, t * 2.2f);
            dl->AddLine({c.x - s * 0.42f, c.y + s * 0.30f}, {c.x - s * 0.42f, c.y - s * 0.10f}, col, t * 2.2f);
            dl->AddLine({c.x + s * 0.40f, c.y - s * 0.40f}, {c.x + s * 0.02f, c.y - s * 0.02f}, col, t * 1.2f);
            dl->AddTriangleFilled({c.x + s * 0.02f, c.y - s * 0.02f}, {c.x + s * 0.10f, c.y - s * 0.02f}, {c.x - s * 0.06f, c.y + s * 0.12f}, col);
            break;
        }
        case IcAdhesin: {  // two cells joined by an adhesin bridge
            ImVec2 a{c.x - s * 0.24f, c.y + s * 0.18f}, b{c.x + s * 0.24f, c.y - s * 0.18f};
            dl->AddCircle(a, s * 0.19f, col, 18, t);
            dl->AddCircle(b, s * 0.19f, col, 18, t);
            ImVec2 d{b.x - a.x, b.y - a.y};
            float l = std::sqrt(d.x * d.x + d.y * d.y);
            d = {d.x / l, d.y / l};
            dl->AddLine({a.x + d.x * s * 0.19f, a.y + d.y * s * 0.19f}, {b.x - d.x * s * 0.19f, b.y - d.y * s * 0.19f}, col, t * 1.8f);
            break;
        }
        case IcUndo:
        case IcRedo: {  // a hook arrow: over the top and back (undo) or forward (redo)
            const float m = ic == IcUndo ? 1.0f : -1.0f;
            const int n = 12;
            ImVec2 pts[n + 1];
            const float cx = c.x + m * s * 0.08f, cy = c.y + s * 0.02f, ry = s * 0.24f;
            for (int k = 0; k <= n; ++k) {  // half circle on the far side, from the top down to the bottom
                float a = -kPi * 0.5f + kPi * k / n;
                pts[k] = {cx + m * std::cos(a) * s * 0.26f, cy + std::sin(a) * ry};
            }
            dl->AddPolyline(pts, n + 1, col, 0, t);
            dl->AddLine(pts[n], {c.x - m * s * 0.30f, pts[n].y}, col, t);  // bottom tail
            const float bx = c.x - m * s * 0.14f;
            dl->AddLine(pts[0], {bx, pts[0].y}, col, t);
            dl->AddTriangleFilled({c.x - m * s * 0.42f, pts[0].y}, {bx, pts[0].y - s * 0.17f}, {bx, pts[0].y + s * 0.17f}, col);
            break;
        }
        case IcFit: {  // four corners of a frame
            const float a = s * 0.38f, b = s * 0.16f;
            for (int sx = -1; sx <= 1; sx += 2)
                for (int sy = -1; sy <= 1; sy += 2) {
                    ImVec2 k{c.x + sx * a, c.y + sy * a};
                    dl->AddLine(k, {k.x - sx * b, k.y}, col, t);
                    dl->AddLine(k, {k.x, k.y - sy * b}, col, t);
                }
            dl->AddCircle(c, s * 0.12f, col, 12, t);
            break;
        }
        case IcMenu:
            for (int k = -1; k <= 1; ++k) dl->AddLine({c.x - s * 0.34f, c.y + k * s * 0.24f}, {c.x + s * 0.34f, c.y + k * s * 0.24f}, col, t);
            break;
        case IcToStart:
        case IcToEnd: {  // |◀ and ▶|
            const float m = ic == IcToEnd ? 1.0f : -1.0f;
            dl->AddTriangleFilled({c.x - m * s * 0.26f, c.y - s * 0.30f}, {c.x - m * s * 0.26f, c.y + s * 0.30f}, {c.x + m * s * 0.18f, c.y}, col);
            dl->AddLine({c.x + m * s * 0.28f, c.y - s * 0.30f}, {c.x + m * s * 0.28f, c.y + s * 0.30f}, col, t * 1.3f);
            break;
        }
        case IcPrev:
        case IcNext: {  // a single triangle: one stage back / on
            const float m = ic == IcNext ? 1.0f : -1.0f;
            dl->AddTriangleFilled({c.x - m * s * 0.2f, c.y - s * 0.30f}, {c.x - m * s * 0.2f, c.y + s * 0.30f}, {c.x + m * s * 0.24f, c.y}, col);
            break;
        }
        case IcHelp:
            dl->AddCircle(c, s * 0.36f, col, 24, t);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.9f,
                        {c.x - ImGui::CalcTextSize("?").x * 0.42f, c.y - ImGui::GetFontSize() * 0.45f}, col, "?");
            break;
        case IcCell:  // a cell: membrane and nucleus
            dl->AddCircle(c, s * 0.38f, col, 28, t);
            dl->AddCircleFilled(c, s * 0.13f, col, 16);
            break;
        case IcStats:  // a bar chart
            for (int i = 0; i < 3; ++i) {
                const float x = c.x + (i - 1) * s * 0.26f, hgt = s * (0.28f + 0.2f * (i == 1 ? 2 : i == 0 ? 0.6f : 1.2f));
                dl->AddRectFilled({x - s * 0.08f, c.y + s * 0.36f - hgt}, {x + s * 0.08f, c.y + s * 0.36f}, col, 1.0f);
            }
            break;
        case IcFlag:  // a pennant on a pole: the level goal
            dl->AddLine({c.x - s * 0.26f, c.y - s * 0.38f}, {c.x - s * 0.26f, c.y + s * 0.4f}, col, t);
            dl->AddTriangleFilled({c.x - s * 0.26f, c.y - s * 0.38f}, {c.x + s * 0.34f, c.y - s * 0.18f}, {c.x - s * 0.26f, c.y + s * 0.02f}, col);
            break;
        case IcFlashlight: {  // a torch pointing up-right with its beam
            const ImVec2 d{0.7071f, -0.7071f}, n{0.7071f, 0.7071f};
            auto P = [&](float a, float b) { return ImVec2{c.x + (d.x * a + n.x * b) * s, c.y + (d.y * a + n.y * b) * s}; };
            // handle, then a head widening toward the lens
            dl->AddQuadFilled(P(-0.48f, -0.08f), P(-0.12f, -0.08f), P(-0.12f, 0.08f), P(-0.48f, 0.08f), col);
            dl->AddQuadFilled(P(-0.12f, -0.12f), P(0.08f, -0.2f), P(0.08f, 0.2f), P(-0.12f, 0.12f), col);
            // the beam: rays fanning out
            for (int k = -1; k <= 1; ++k) dl->AddLine(P(0.18f, 0.1f * k), P(0.46f, 0.28f * k), col, t * 0.9f);
            break;
        }
    }
}

// A small vector picture of a cell type (body in its colour + the type's signature organelle), the same motifs as the
// microscope shader: used by the type picker, the mode list and the genome editor's cards.
inline void drawCellIcon(ImDrawList* dl, ImVec2 c, float r, int type, ImU32 fill, float alpha = 1.0f, float rot = 0.0f) {
    auto rgba = [&](int R, int G, int B, int A = 255) { return IM_COL32(R, G, B, (int)(A * alpha)); };
    ImVec4 f = ImGui::ColorConvertU32ToFloat4(fill);
    ImU32 dark = ImGui::ColorConvertFloat4ToU32({f.x * 0.55f, f.y * 0.55f, f.z * 0.55f, f.w * alpha});
    ImU32 darker = ImGui::ColorConvertFloat4ToU32({f.x * 0.4f, f.y * 0.4f, f.z * 0.4f, f.w * alpha});
    ImU32 light = ImGui::ColorConvertFloat4ToU32({0.5f + f.x * 0.5f, 0.5f + f.y * 0.5f, 0.5f + f.z * 0.5f, f.w * alpha});
    ImU32 body = ImGui::ColorConvertFloat4ToU32({f.x, f.y, f.z, f.w * alpha});
    float t = std::max(1.0f, r * 0.12f);
    const float cr = std::cos(rot), sr = std::sin(rot);
    // Local point (x forward, y to the left, in radii) -> screen.
    auto P = [&](float x, float y) { return ImVec2(c.x + r * (x * cr - y * sr), c.y - r * (x * sr + y * cr)); };
    auto pt = [&](float a, float d) { return P(std::cos(a) * d, std::sin(a) * d); };
    // appendages behind the body
    if (type == (int)CellType::Flagellocyte) {  // thick at the base, tapering into a wave
        const int n = 10;
        for (int k = 0; k < n; ++k) {
            float x0 = 0.85f + k * 0.12f, x1 = x0 + 0.12f;
            float s0 = (float)k / n, s1 = (float)(k + 1) / n;
            ImVec2 a = P(-x0, std::sin(k * 1.1f) * 0.2f * s0), b = P(-x1, std::sin((k + 1) * 1.1f) * 0.2f * s1);
            dl->AddLine(a, b, darker, std::max(1.0f, r * (0.26f - 0.18f * s0)));
        }
    } else if (type == (int)CellType::Ciliocyte) {
        for (int k = 0; k < 14; ++k) {
            float a = k * kTau / 14;
            dl->AddLine(pt(a, 0.95f), pt(a + 0.25f, 1.3f), dark, t * 0.8f);
        }
    } else if (type == (int)CellType::Senseocyte) {
        dl->AddLine(pt(0.35f, 0.9f), pt(0.55f, 1.35f), dark, t);
        dl->AddLine(pt(-0.35f, 0.9f), pt(-0.55f, 1.35f), dark, t);
    } else if (type == (int)CellType::Glueocyte) {
        for (int k = 0; k < 10; ++k) dl->AddCircleFilled(pt(k * kTau / 10, 1.05f), r * 0.12f, rgba(240, 225, 140));
    } else if (type == (int)CellType::Cnidocyte) {
        dl->AddLine(pt(0, 0.9f), pt(0, 1.4f), dark, t);
    } else if (type == (int)CellType::Luminocyte) {
        dl->AddCircleFilled(c, r * 1.45f, rgba(200, 255, 235, 40), 24);  // halo
        dl->AddCircleFilled(c, r * 1.2f, rgba(200, 255, 235, 50), 24);
    } else if (type == (int)CellType::Grappler) {
        for (int s = -1; s <= 1; s += 2) {  // two hooked claws at the front
            dl->AddLine(pt(0.35f * s, 0.9f), pt(0.25f * s, 1.38f), dark, t * 1.3f);
            dl->AddLine(pt(0.25f * s, 1.38f), pt(0.05f * s, 1.3f), dark, t * 1.3f);
        }
    }
    // body + membrane
    if (type == (int)CellType::Devorocyte) {
        ImVec2 p[24];
        for (int k = 0; k < 24; ++k) p[k] = pt(k * kTau / 24, k % 2 ? 0.88f : 1.04f);
        dl->AddConvexPolyFilled(p, 24, body);
        dl->AddPolyline(p, 24, dark, ImDrawFlags_Closed, t);
    } else {
        dl->AddCircleFilled(c, r, body, 24);
        dl->AddCircle(c, r * (type == (int)CellType::Keratinocyte ? 0.9f : 0.96f), dark, 24,
                      type == (int)CellType::Keratinocyte ? t * 2.6f : t);
    }
    auto poly = [&](std::initializer_list<ImVec2> pts, ImU32 col) {
        ImVec2 v[8];
        int n = 0;
        for (ImVec2 p : pts) v[n++] = p;
        dl->AddConvexPolyFilled(v, n, col);
    };
    auto hexagon = [&](float rad, ImU32 col, float thick) {  // thick <= 0: filled
        ImVec2 v[6];
        for (int k = 0; k < 6; ++k) v[k] = pt(k * kTau / 6 + kPi / 6, rad);
        if (thick > 0) dl->AddPolyline(v, 6, col, ImDrawFlags_Closed, thick);
        else dl->AddConvexPolyFilled(v, 6, col);
    };
    // signature organelles
    switch ((CellType)type) {
        case CellType::Phagocyte:
            dl->AddCircle(pt(0.8f, 0.45f), r * 0.16f, dark, 12, t * 0.8f);
            dl->AddCircle(pt(3.6f, 0.45f), r * 0.12f, dark, 12, t * 0.8f);
            break;
        case CellType::Photocyte:
            for (int k = 0; k < 6; ++k) dl->AddCircleFilled(pt(k * kTau / 6, 0.6f), r * 0.14f, rgba(40, 110, 40));
            break;
        case CellType::Devorocyte: dl->AddCircleFilled(pt(0, 0.35f), r * 0.22f, rgba(90, 15, 25)); break;
        case CellType::Lipocyte: dl->AddCircleFilled(pt(0, 0.12f), r * 0.55f, rgba(255, 245, 190)); break;
        case CellType::Buoyocyte: dl->AddCircleFilled(pt(0.3f, 0.15f), r * 0.45f, light); break;
        case CellType::Virocyte:
            for (int k = 0; k < 5; ++k) dl->AddNgonFilled(pt(k * kTau / 5 + 0.3f, 0.55f), r * 0.14f, rgba(120, 55, 160), 6);
            break;
        case CellType::Nitrocyte:
            for (int k = 0; k < 3; ++k) dl->AddLine(pt(k * 2.1f, 0.35f), pt(k * 2.1f + 0.9f, 0.65f), rgba(40, 110, 120), t * 1.6f);
            break;
        case CellType::Stereocyte:
            for (int k = 0; k < 6; ++k) dl->AddLine(c, pt(k * kTau / 6 + 0.2f, 0.85f), dark, t);
            break;
        case CellType::Senseocyte:
            dl->AddCircleFilled(pt(0, 0.55f), r * 0.24f, rgba(235, 100, 40));
            dl->AddCircleFilled(pt(0, 0.58f), r * 0.1f, rgba(20, 10, 10));
            break;
        case CellType::Myocyte:
            for (int k = -2; k <= 2; ++k) dl->AddLine(P(-0.8f, k * 0.22f), P(0.8f, k * 0.22f), dark, t * 0.8f);
            break;
        case CellType::Neurocyte:
            for (int k = 0; k < 6; ++k) dl->AddLine(c, pt(k * kTau / 6, 0.9f), dark, t);
            dl->AddCircleFilled(c, r * 0.22f, light);
            break;
        case CellType::Secrocyte:
            for (int k = 0; k < 5; ++k) {
                dl->AddCircleFilled(pt(k * kTau / 5 + 0.63f, 0.56f), r * 0.12f, light);
                dl->AddCircle(pt(k * kTau / 5 + 0.63f, 0.56f), r * 0.12f, dark, 10, t * 0.6f);
            }
            break;
        case CellType::Stemocyte:
            dl->AddCircle(c, r * 0.62f, dark, 20, t * 0.8f);
            dl->AddCircle(c, r * 0.4f, dark, 16, t * 0.7f);
            break;
        case CellType::Gamete:  // rhombus with a bright centre
            poly({P(0.62f, 0), P(0, 0.42f), P(-0.62f, 0), P(0, -0.42f)}, dark);
            dl->AddCircleFilled(c, r * 0.13f, light);
            break;
        case CellType::Ciliocyte:
            for (int k = 0; k < 8; ++k) dl->AddCircleFilled(pt(k * kTau / 8, 0.75f), r * 0.07f, dark);
            break;
        case CellType::Electrocyte:  // a stack of bowed plates from the - pole (back) to the + pole (front)
            for (int k = 0; k < 5; ++k) {
                float x0 = -0.5f + k * 0.25f;
                ImVec2 pts[7];
                for (int j = 0; j < 7; ++j) {
                    float y = -0.6f + j * 0.2f;
                    pts[j] = P(x0 - 0.35f * y * y, y);
                }
                dl->AddPolyline(pts, 7, darker, 0, std::max(1.0f, r * 0.1f));
            }
            dl->AddLine(P(0.62f, 0), P(0.82f, 0), darker, t);
            dl->AddLine(P(0.72f, -0.1f), P(0.72f, 0.1f), darker, t);
            break;
        case CellType::Cnidocyte:  // capsule with a harpoon
            dl->AddEllipseFilled(P(0.12f, 0), {r * 0.5f, r * 0.3f}, light, -rot);
            poly({P(0.48f, 0), P(-0.18f, 0.17f), P(-0.18f, -0.17f)}, darker);
            break;
        case CellType::Chromatocyte:  // six arms around a hexagon
            for (int k = 0; k < 6; ++k) {
                ImVec2 tip = pt(k * kTau / 6 + kPi / 6, 0.7f);
                dl->AddLine(c, tip, dark, t * 1.2f);
                dl->AddCircleFilled(tip, r * 0.08f, dark);
            }
            hexagon(0.22f, dark, 0);
            break;
        case CellType::Luminocyte:  // a glowing photophore with a lens
            dl->AddCircleFilled(pt(0, 0.15f), r * 0.5f, rgba(230, 255, 240));
            dl->AddCircle(pt(0, 0.15f), r * 0.5f, dark, 18, t * 0.8f);
            dl->AddCircleFilled(pt(0, 0.3f), r * 0.18f, rgba(255, 255, 255));
            break;
        case CellType::Cystocyte:  // hexagonal two-layer wall around a resting core
            hexagon(0.62f, darker, t * 1.2f);
            hexagon(0.5f, dark, t * 0.7f);
            dl->AddCircleFilled(c, r * 0.32f, dark);
            dl->AddCircleFilled(c, r * 0.13f, darker);
            break;
        case CellType::Grappler:  // contractile ring of the grip
            dl->AddCircle(pt(0, 0.4f), r * 0.32f, dark, 16, t * 1.4f);
            dl->AddLine(pt(0, 0.08f), pt(kPi, 0.6f), dark, t);
            break;
        default: break;
    }
    // nucleus: in the middle, as the microscope draws it (at the back where an organ fills the middle)
    if (type != (int)CellType::Neurocyte && type != (int)CellType::Virocyte && type != (int)CellType::Lipocyte &&
        type != (int)CellType::Chromatocyte && type != (int)CellType::Gamete && type != (int)CellType::Cystocyte &&
        type != (int)CellType::Electrocyte && type != (int)CellType::Erythrocyte && type != (int)CellType::SignalCell) {
        const bool back = type == (int)CellType::Cnidocyte || type == (int)CellType::Luminocyte;
        dl->AddCircleFilled(back ? P(-0.6f, 0) : c, r * (back ? 0.17f : 0.22f), dark);
    }
}

inline bool iconButton(const char* id, Icon ic, bool active, const char* tip, float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(id, {size, size});
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? IM_COL32(255, 158, 51, 55) : (hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 0));
    dl->AddRectFilled(p, {p.x + size, p.y + size}, bg, size * 0.25f);
    if (active) dl->AddRect(p, {p.x + size, p.y + size}, IM_COL32(255, 158, 51, 150), size * 0.25f, 0, 1.0f);
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImU32 fg = disabled ? IM_COL32(110, 114, 120, 140) : active ? kAccentU32 : (hov ? IM_COL32(235, 240, 245, 255) : IM_COL32(190, 198, 205, 255));
    drawIcon(dl, ic, {p.x + size * 0.5f, p.y + size * 0.5f}, size * 0.62f, fg);
    if (hov && tip) ImGui::SetTooltip("%s", tip);
    return pressed;
}

// A button that opens a drop-down menu: the label and a small triangle (drawn, so no font glyph is needed).
inline bool dropButton(const char* label, ImVec2 size = {0, 0}) {
    const float fs = ImGui::GetFontSize();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s      ##drop", label);  // room for the triangle
    bool pressed = ImGui::Button(buf, size);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const float cx = b.x - ImGui::GetStyle().FramePadding.x - fs * 0.35f, cy = (a.y + b.y) * 0.5f, s = fs * 0.28f;
    ImGui::GetWindowDrawList()->AddTriangleFilled({cx - s, cy - s * 0.5f}, {cx + s, cy - s * 0.5f}, {cx, cy + s * 0.6f},
                                                  ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

// An ordinary button (frame and all) with an icon instead of a label.
inline bool iconFrameButton(const char* id, Icon ic, ImVec2 size, float iconScale = 0.5f) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button(id, size);
    ImVec2 sz = ImGui::GetItemRectSize();
    const bool disabled = (ImGui::GetItemFlags() & ImGuiItemFlags_Disabled) != 0;
    ImU32 fg = disabled ? IM_COL32(110, 114, 120, 160) : IM_COL32(225, 230, 236, 255);
    drawIcon(ImGui::GetWindowDrawList(), ic, {p.x + sz.x * 0.5f, p.y + sz.y * 0.5f}, std::min(sz.x, sz.y) * iconScale, fg);
    return pressed;
}

inline void sectionHeader(const char* text) {
    ImGui::Dummy({0, 2});
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(text);
    ImGui::PopStyleColor();
}

// Touch: the active widget that follows the finger itself (a dial, a picker): the panel must not scroll instead.
inline ImGuiID gNoTouchScrollId = 0;
inline void keepTouchDrag() {
    if (ImGui::IsItemActive()) gNoTouchScrollId = ImGui::GetItemID();
}
// Explanation shown as a tooltip on the widget just drawn (no extra glyph that could overflow the row).
inline bool gTooltips = true;  // settings: explanations on hover
inline bool gTestOpenChooser = false;  // --color-chooser: the first touch colour field opens its chooser (screenshots)
inline void helpMarker(const char* text) {
    if (gTooltips && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Label on the left, widget filling the rest (cleaner than ImGui's right-hand labels).
inline float gLabelW = 150.0f;
inline void rowLabel(const char* label) {
    // A label longer than its column (a narrow phone card) gets a line of its own above the widget instead of
    // running into it.
    if (ImGui::CalcTextSize(label, nullptr, true).x > gLabelW - 2 * ImGui::GetStyle().ItemSpacing.x) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
        ImGui::SameLine(gLabelW);
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
}

inline bool sliderF(const char* label, float* v, float lo, float hi, const char* fmt, float snapStep = 0, bool snap = false,
             ImGuiSliderFlags flags = 0) {
    rowLabel(label);
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##v", v, lo, hi, fmt, flags);
    ImGui::PopID();
    if (ch && snap && snapStep > 0) *v = lo + std::round((*v - lo) / snapStep) * snapStep;
    return ch;
}

inline bool sliderAngle(const char* label, float* rad, bool snap) {
    float deg = rad2deg(*rad);
    if (deg < 0) deg += 360.0f;
    rowLabel(label);
    ImGui::PushID(label);
    bool ch = ImGui::SliderFloat("##a", &deg, 0.0f, 360.0f, "%.0f°");
    ImGui::PopID();
    if (ch) {
        if (snap) deg = std::round(deg / 15.0f) * 15.0f;
        *rad = wrapAngle(deg2rad(deg));
    }
    return ch;
}

inline bool checkbox(const char* label, bool* v) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float box = ImGui::GetFrameHeight();
    // The box stands at the right end of the row; a label longer than the room left of it wraps onto more lines
    // (it used to run under the box on a narrow phone card).
    bool ch = false;
    ImGui::PushID(label);
    if (ImGui::CalcTextSize(label).x <= gLabelW - 2 * st.ItemSpacing.x) {  // fits the label column: box in the column
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(gLabelW);
        ch = ImGui::Checkbox("##c", v);
    } else {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float boxX = std::max(gLabelW, ImGui::GetContentRegionAvail().x - box);
        ImGui::SetCursorScreenPos({start.x + boxX, start.y});
        ch = ImGui::Checkbox("##c", v);
        const ImGuiLastItemData boxItem = ImGui::GetCurrentContext()->LastItemData;  // for helpMarker's tooltip
        ImGui::SetCursorScreenPos({start.x, start.y + st.FramePadding.y});
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(boxX - st.ItemSpacing.x, box));
        ImGui::TextUnformatted(label);
        ImGui::PopTextWrapPos();
        // The next row starts below whichever is lower: the wrapped label or the box.
        const float y = std::max(ImGui::GetCursorScreenPos().y, start.y + box + st.ItemSpacing.y) - st.ItemSpacing.y;  // Dummy adds it back
        ImGui::SetCursorScreenPos({start.x, y});
        ImGui::Dummy({0, 0});
        ImGui::GetCurrentContext()->LastItemData = boxItem;
    }
    ImGui::PopID();
    return ch;
}

inline void colorSwatch(Color c, float size, bool circle = true) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float y = p.y + (ImGui::GetFrameHeight() - size) * 0.5f;
    if (circle) dl->AddCircleFilled({p.x + size * 0.5f, y + size * 0.5f}, size * 0.5f, colU32(c));
    else dl->AddRectFilled({p.x, y}, {p.x + size, y + size}, colU32(c), 3);
    ImGui::Dummy({size, ImGui::GetFrameHeight()});
}

inline bool modeCombo(const char* label, uint8_t* mode, const Genome& g) {
    rowLabel(label);
    ImGui::PushID(label);
    char preview[96];
    const Mode& cur = g.modes[*mode];
    std::snprintf(preview, sizeof(preview), "M%d  %s", *mode + 1,
                  g.labels[*mode].empty() ? cellTypeInfo(cur.type).name : g.labels[*mode].c_str());
    bool ch = false;
    if (ImGui::BeginCombo("##m", preview, ImGuiComboFlags_HeightLarge)) {
        for (int i = 0; i < kMaxModes; ++i) {
            const Mode& m = g.modes[i];
            ImGui::PushID(i);
            colorSwatch(m.color, ImGui::GetFontSize() * 0.7f);
            ImGui::SameLine();
            char item[96];
            if (g.labels[i].empty()) std::snprintf(item, sizeof(item), "M%-3d %s", i + 1, cellTypeInfo(m.type).name);
            else std::snprintf(item, sizeof(item), "M%-3d %s · %s", i + 1, g.labels[i].c_str(), cellTypeInfo(m.type).name);
            if (ImGui::Selectable(item, i == *mode)) {
                *mode = (uint8_t)i;
                ch = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    return ch;
}

inline bool progEditor(const char* title, Prog& p, int field, bool snap) {
    const ProgRange& r = progRange(field);
    bool ch = false;
    ImGui::PushID(field);
    ImGui::PushID(title);
    ImGui::TextDisabled("%s", title);
    rowLabel("  Вход");
    if (ImGui::BeginCombo("##in", progInputName(p.input))) {
        for (int i = 0; i < (int)ProgInput::Count; ++i) {
            if (ImGui::Selectable(progInputName((ProgInput)i), (int)p.input == i)) {
                p.input = (ProgInput)i;
                ch = true;
            }
            const char* tip = nullptr;
            switch ((ProgInput)i) {
                case ProgInput::Light: tip = "Освещённость в точке клетки, 0..2 (ночью 0, если в чашке есть смена дня и ночи)."; break;
                case ProgInput::Food: tip = "Сколько пищи в 60 мкм вокруг: 0 — нет, 1 — несколько крупинок рядом."; break;
                case ProgInput::Injury: tip = "Боль: повреждение клетки, 0 — цела, 1 — гибнет."; break;
                default: break;
            }
            if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        }
        ImGui::EndCombo();
    }
    if (p.input == ProgInput::Fixed) {
        ch |= sliderF("  Значение", &p.b, r.lo, r.hi, "%.2f", (r.hi - r.lo) / 40.0f, snap);
    } else {
        rowLabel("  Функция");
        int kind = (int)p.kind;
        if (ImGui::RadioButton("a·x + b", kind == 0)) kind = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("x < c ? a : b", kind == 1)) kind = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("волна", kind == 2)) kind = 2;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("a·sin(2π·x / c) + b — ритм. Для входа «Возраст» x в часах, c — период в часах.");
        if (kind != (int)p.kind) {
            if (kind == 2 && (p.c <= 0.05f || p.kind != ProgKind::Wave)) p.c = 1.5f;
            p.kind = (ProgKind)kind;
            ch = true;
        }
        if (p.kind == ProgKind::Wave) {
            ch |= sliderF("  a (размах)", &p.a, -(std::fabs(r.lo) + std::fabs(r.hi)), std::fabs(r.lo) + std::fabs(r.hi), "%.2f");
            ch |= sliderF("  b (сдвиг)", &p.b, r.lo, r.hi, "%.2f");
            ch |= sliderF("  c (период)", &p.c, 0.05f, 24.0f, p.input == ProgInput::Age ? "%.2f ч" : "%.2f");
            ImGui::PopID();
            ImGui::PopID();
            return ch;
        }
        float aMax = p.kind == ProgKind::Linear ? 8.0f * r.scale : r.scale + std::fabs(r.offset) + r.scale;
        ch |= sliderF("  a", &p.a, -aMax, aMax, "%.2f");
        ch |= sliderF("  b", &p.b, r.lo - 8.0f * r.scale * (p.kind == ProgKind::Linear), r.hi + 8.0f * r.scale * (p.kind == ProgKind::Linear),
                      "%.2f");
        if (p.kind == ProgKind::Step) {
            bool sig = inputSignal(p.input) >= 0;
            ch |= sliderF("  c (порог)", &p.c, sig ? -20.0f : 0.0f, sig ? 20.0f : 1.0f, "%.2f");
        }
    }
    ImGui::PopID();
    ImGui::PopID();
    return ch;
}

// Modes reachable from the initial mode through children, stem paths and gamete results.
inline void reachableModes(const Genome& g, bool out[kMaxModes]) {
    std::fill(out, out + kMaxModes, false);
    int stack[kMaxModes * 8], sp = 0;
    stack[sp++] = g.initialMode;
    while (sp) {
        int m = stack[--sp];
        if (out[m]) continue;
        out[m] = true;
        const Mode& md = g.modes[m];
        if (md.splitMass < kNeverSplit && md.type != CellType::Gamete) {
            stack[sp++] = md.childMode[0];
            stack[sp++] = md.childMode[1];
        }
        if (md.type == CellType::Stemocyte) {
            stack[sp++] = md.stemMode[0];
            stack[sp++] = md.stemMode[1];
        }
        if (md.type == CellType::Cystocyte) stack[sp++] = md.stemMode[0];
        if (md.type == CellType::Gamete) stack[sp++] = md.gameteModeAfter;
    }
}

inline const char* tagName(CellTag t) {
    switch (t) {
        case CellTag::User: return "пользователь";
        case CellTag::Challenge: return "задание";
        case CellTag::Contamination: return "заражение";
        case CellTag::InfectedUser: return "инфицирована (пользователь)";
        case CellTag::InfectedChallenge: return "инфицирована (задание)";
        case CellTag::InfectedContamination: return "инфицирована (заражение)";
        case CellTag::Hybrid: return "гибрид";
    }
    return "";
}

inline void valueBar(const char* label, float frac, const char* text, ImU32 col) {
    rowLabel(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, IM_COL32(255, 255, 255, 14), 5);
    dl->AddRectFilled(p, {p.x + w * clampv(frac, 0.0f, 1.0f), p.y + h}, col, 5);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText({p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f}, IM_COL32(235, 240, 245, 255), text);
    ImGui::Dummy({w, h});
}


// Lower-case ASCII and Cyrillic (UTF-8) for searches.
inline std::string lowerUtf8(const std::string& in) {
    std::string out = in;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned char c = (unsigned char)out[i];
        if (c >= 'A' && c <= 'Z') out[i] = (char)(c + 32);
        else if (c == 0xD0 && i + 1 < out.size()) {
            unsigned char d = (unsigned char)out[i + 1];
            if (d >= 0x90 && d <= 0x9F) out[i + 1] = (char)(d + 0x20);  // А-П -> а-п
            else if (d >= 0xA0 && d <= 0xAF) {                          // Р-Я -> р-я (second block)
                out[i] = (char)0xD1;
                out[i + 1] = (char)(d - 0x20);
            } else if (d == 0x81) {  // Ё -> ё
                out[i] = (char)0xD1;
                out[i + 1] = (char)0x91;
            }
            ++i;
        }
    }
    return out;
}

}  // namespace uiw
