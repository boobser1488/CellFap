// Genome editor screen: the organism unfolds full-screen by stage (Genomeia-style direct manipulation),
// with the original game's per-mode settings list on the right.
#include "app.hpp"
#include "sim/cellab_import.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#include <utility>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

using namespace uiw;

namespace {
constexpr float kNavH = 46.0f;
constexpr float kListW = 270.0f;
constexpr float kPanelW = 360.0f;
const ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing;

bool genomeEquals(const Genome& a, const Genome& b) {
    return a.initialMode == b.initialMode && a.sameModes(b);
}

// Toggle-able pill button (checked = orange tint), as in the reference editor's top bar.
bool pill(const char* label, bool checked = false, ImVec2 size = {0, 0}) {
    if (checked) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.62f, 0.20f, 0.42f));
    }
    bool r = ImGui::Button(label, size);
    if (checked) ImGui::PopStyleColor(2);
    return r;
}

// Grid of the 18 cell types: colour dot + short name; returns true when the choice changed.
// Visual grid of every cell type: a picture of the cell with its name; hover explains (with a joke).
bool typePicker(const char* id, CellType* t, float width) {
    bool changed = false;
    ImGui::PushID(id);
    const int cols = width > 300 ? 6 : 5;  // compact: a picture with a short caption, details on hover
    float gap = 3.0f;
    float bw = (width - gap * (cols - 1)) / cols;
    float bh = ImGui::GetFrameHeight() * 1.75f;
    float fs = ImGui::GetFontSize() * 0.68f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kCellTypeCount; ++i) {
        if (i % cols) ImGui::SameLine(0, gap);
        const CellTypeInfo& info = cellTypeInfo(i);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("t", {bw, bh})) {
            *t = (CellType)i;
            changed = true;
        }
        bool hov = ImGui::IsItemHovered();
        bool sel = (int)*t == i;
        dl->AddRectFilled(p, {p.x + bw, p.y + bh}, sel ? IM_COL32(255, 158, 51, 70) : (hov ? IM_COL32(255, 255, 255, 22) : IM_COL32(255, 255, 255, 8)), 6);
        if (sel) dl->AddRect(p, {p.x + bw, p.y + bh}, IM_COL32(255, 158, 51, 220), 6, 0, 1.5f);
        float ir = bh * 0.24f;
        drawCellIcon(dl, {p.x + bw * 0.5f, p.y + ir + bh * 0.1f}, ir, i, colU32({info.r, info.g, info.b}));
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0, info.name);
        dl->PushClipRect(p, {p.x + bw, p.y + bh}, true);
        dl->AddText(ImGui::GetFont(), fs, {p.x + std::max(2.0f, (bw - ts.x) * 0.5f), p.y + bh - ts.y - 4},
                    sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 208, 212, 255), info.name);
        dl->PopClipRect();
        if (hov) ImGui::SetTooltip("%s (%s)\n%s\n\n%s", info.name, info.latin, info.summary, cellTypeQuip((CellType)i));
        ImGui::PopID();
    }
    ImGui::PopID();
    return changed;
}

const Color kPalette[] = {{0.62f, 0.80f, 0.44f}, {0.36f, 0.72f, 0.50f}, {0.45f, 0.70f, 0.90f}, {0.35f, 0.45f, 0.90f},
                          {0.70f, 0.42f, 0.86f}, {0.90f, 0.45f, 0.70f}, {0.88f, 0.36f, 0.38f}, {0.95f, 0.62f, 0.30f},
                          {0.96f, 0.86f, 0.45f}, {0.80f, 0.74f, 0.60f}, {0.55f, 0.55f, 0.60f}, {0.95f, 0.95f, 0.95f}};
constexpr int kPaletteN = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

bool sameColor(Color a, Color b) { return std::fabs(a.r - b.r) + std::fabs(a.g - b.g) + std::fabs(a.b - b.b) < 0.02f; }

ImU32 hsvU32(float h, float s, float v) {
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImGui::ColorConvertFloat4ToU32({r, g, b, 1.0f});
}

// A strip that the finger slides along: *v in [0, 1]; paint(t) is the colour at t.
template <class F>
bool gradientStrip(const char* id, float* v, float w, float h, F paint) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {w, h});
    keepTouchDrag();
    bool changed = false;
    if (ImGui::IsItemActive()) {
        float nv = std::clamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.0f, 1.0f);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int n = 24;
    for (int k = 0; k < n; ++k) {
        float x0 = p.x + w * k / n, x1 = p.x + w * (k + 1) / n;
        ImU32 a = paint((float)k / n), b = paint((float)(k + 1) / n);
        dl->AddRectFilledMultiColor({x0, p.y}, {k + 1 < n ? x1 + 0.5f : x1, p.y + h}, a, b, b, a);
    }
    dl->AddRect(p, {p.x + w, p.y + h}, IM_COL32(255, 255, 255, 40));
    const float x = p.x + *v * w, t = std::max(1.5f, h * 0.06f);
    dl->AddRectFilled({x - 2 * t, p.y - 2 * t}, {x + 2 * t, p.y + h + 2 * t}, IM_COL32(20, 20, 24, 255), t);
    dl->AddRectFilled({x - t, p.y - t}, {x + t, p.y + h + t}, IM_COL32(255, 255, 255, 255), t * 0.5f);
    return changed;
}

// The colour chooser for a touch screen: ready-made colours in big buttons, then hue, saturation and brightness
// strips, and the result with its hex code.
bool colorChooserBody(Color* c, float w) {
    static Color shown{-1, -1, -1};  // the colour the strips below were set up for
    static float H = 0, S = 0, V = 0;
    if (std::fabs(shown.r - c->r) + std::fabs(shown.g - c->g) + std::fabs(shown.b - c->b) > 1e-5f)
        ImGui::ColorConvertRGBtoHSV(c->r, c->g, c->b, H, S, V);  // (an unchanged colour keeps the hue of a grey)
    const float u = ImGui::GetFontSize() / 16.0f, gap = 6 * u;
    bool changed = false;
    bool picked = false;  // a ready-made colour was tapped
    // Ready-made colours: two rows of six.
    const float s = std::floor((w - 5 * gap) / 6);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < kPaletteN; ++i) {
        if (i % 6) ImGui::SameLine(0, gap);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("c", {s, s * 0.72f})) {
            *c = kPalette[i];
            picked = true;
            ImGui::ColorConvertRGBtoHSV(c->r, c->g, c->b, H, S, V);
            changed = true;
        }
        dl->AddRectFilled(p, {p.x + s, p.y + s * 0.72f}, colU32(kPalette[i]), 6 * u);
        if (sameColor(*c, kPalette[i])) dl->AddRect({p.x - 2 * u, p.y - 2 * u}, {p.x + s + 2 * u, p.y + s * 0.72f + 2 * u}, kAccentU32, 7 * u, 0, 2.5f * u);
        ImGui::PopID();
    }
    ImGui::Dummy({0, 4 * u});
    const float sh = 34 * u;
    auto strip = [&](const char* label, const char* id, float* v, auto paint) {
        ImGui::TextDisabled("%s", label);
        if (gradientStrip(id, v, w, sh, paint)) changed = true;
        ImGui::Dummy({0, 2 * u});
    };
    strip("Оттенок", "##h", &H, [](float t) { return hsvU32(t, 1, 1); });
    strip("Насыщенность", "##s", &S, [&](float t) { return hsvU32(H, t, std::max(V, 0.35f)); });
    strip("Яркость", "##v", &V, [&](float t) { return hsvU32(H, S, t); });
    if (changed && !picked) {  // a strip moved: the colour follows H, S, V
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(H, S, V, r, g, b);
        *c = {r, g, b};
    }
    shown = *c;
    // Result with the hex code.
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float rh = 36 * u;
    ImGui::Dummy({w, rh});
    dl->AddRectFilled(p, {p.x + w, p.y + rh}, colU32(*c), 6 * u);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", (int)std::lround(c->r * 255), (int)std::lround(c->g * 255), (int)std::lround(c->b * 255));
    const bool dark = 0.3f * c->r + 0.59f * c->g + 0.11f * c->b < 0.55f;
    ImVec2 ts = ImGui::CalcTextSize(hex);
    dl->AddText({p.x + (w - ts.x) * 0.5f, p.y + (rh - ts.y) * 0.5f}, dark ? IM_COL32(255, 255, 255, 235) : IM_COL32(20, 20, 24, 235), hex);
    return changed;
}

// Opens the chooser above (centred on the screen, finger-sized) as popup `popupId` of the current ID scope.
bool colorChooserPopup(const char* popupId, Color* c) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = ImGui::GetFontSize() / 16.0f;
    const float pw = std::min(400 * u, io.DisplaySize.x - 16 * u);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({pw, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14 * u, 12 * u});
    bool changed = false;
    if (ImGui::BeginPopup(popupId, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::PopStyleVar();
        const float w = ImGui::GetContentRegionAvail().x;
        changed = colorChooserBody(c, w);
        ImGui::Dummy({0, 4 * u});
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12 * u, 12 * u});
        if (ImGui::Button("Готово", {w, 0})) ImGui::CloseCurrentPopup();
        ImGui::PopStyleVar();
        ImGui::EndPopup();
    } else {
        ImGui::PopStyleVar();
    }
    return changed;
}

// A colour field: on a touch screen the swatch with its hex code opens the chooser above; on the desktop it is
// ImGui's colour editor with the hex input.
bool colorEditor(const char* id, Color* c, bool touch) {
    if (!touch) return ImGui::ColorEdit3(id, &c->r, ImGuiColorEditFlags_DisplayHex);
    ImGui::PushID(id);
    const float w = ImGui::CalcItemWidth(), h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##sw", {w, h}) || std::exchange(gTestOpenChooser, false)) ImGui::OpenPopup("chooser");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = h * 0.18f;
    dl->AddRectFilled({p.x + pad, p.y + pad}, {p.x + h * 1.8f, p.y + h - pad}, colU32(*c), h * 0.15f);
    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", (int)std::lround(c->r * 255), (int)std::lround(c->g * 255), (int)std::lround(c->b * 255));
    dl->AddText({p.x + h * 1.8f + pad * 2, p.y + (h - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_Text), hex);
    dl->AddText({p.x + w - ImGui::CalcTextSize("›").x - pad * 2, p.y + (h - ImGui::GetFontSize()) * 0.5f}, ImGui::GetColorU32(ImGuiCol_TextDisabled), "›");
    bool changed = colorChooserPopup("chooser", c);
    ImGui::PopID();
    return changed;
}

// Row of ready-made colours (plus the full chooser): beginners pick a colour in one click. `touch`: the full chooser
// is the finger-sized one, else ImGui's picker.
bool colorPalette(const char* id, Color* c, float width, bool touch = false) {
    bool changed = false;
    ImGui::PushID(id);
    const int n = kPaletteN;
    float s = std::min(ImGui::GetFrameHeight(), (width - 30) / (n + 1));
    if (touch) s = std::floor((width - (n / 2) * 4) / (n / 2 + 1));  // two rows, finger-sized
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < n; ++i) {
        if (i && !(touch && i == n / 2)) ImGui::SameLine(0, touch ? 4 : 2);
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("c", {s, s})) {
            *c = kPalette[i];
            changed = true;
        }
        bool sel = sameColor(*c, kPalette[i]);
        dl->AddCircleFilled({p.x + s * 0.5f, p.y + s * 0.5f}, s * 0.42f, colU32(kPalette[i]));
        if (sel || ImGui::IsItemHovered()) dl->AddCircle({p.x + s * 0.5f, p.y + s * 0.5f}, s * 0.48f, sel ? kAccentU32 : IM_COL32(255, 255, 255, 120), 0, 2);
        ImGui::PopID();
        if (touch && i == n - 1) {
            // "Any colour": a rainbow ring that opens the chooser.
            ImGui::SameLine(0, 4);
            ImVec2 q = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("more", {s, s}) || std::exchange(gTestOpenChooser, false)) ImGui::OpenPopup("chooser");
            ImVec2 cc{q.x + s * 0.5f, q.y + s * 0.5f};
            for (int k = 0; k < 24; ++k) {
                float a0 = k * kTau / 24, a1 = (k + 1) * kTau / 24;
                dl->PathArcTo(cc, s * 0.36f, a0, a1 + 0.02f, 3);
                dl->PathStroke(hsvU32(k / 24.0f, 0.75f, 0.95f), 0, s * 0.12f);
            }
            dl->AddCircleFilled(cc, s * 0.22f, colU32(*c));
            if (!sel) {
                bool any = false;
                for (const Color& pc : kPalette) any |= sameColor(pc, *c);
                if (!any) dl->AddCircle(cc, s * 0.48f, kAccentU32, 0, 2);  // the colour is a custom one
            }
        }
    }
    if (touch) {
        if (colorChooserPopup("chooser", c)) changed = true;
    } else {
        ImGui::SameLine(0, 6);
        if (ImGui::ColorEdit3("##more", &c->r, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Любой цвет");
    }
    ImGui::PopID();
    return changed;
}

// Split direction as a dial: the cell in the middle (front = up arrow), drag the orange handle around it.
// Child 1 goes the handle's way, child 2 the opposite way.
bool splitDial(const char* id, float* rad, int type, Color col, bool snap, float size) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c{p.x + size * 0.5f, p.y + size * 0.5f};
    ImGui::InvisibleButton("dial", {size, size});
    keepTouchDrag();  // on a phone dragging the dial turns it, it does not scroll the panel
    bool changed = false;
    if (ImGui::IsItemActive()) {
        ImVec2 m = ImGui::GetIO().MousePos;
        // the cell's front points up on screen: angle 0 = up, counter-clockwise positive (as in the genome)
        float a = std::atan2(-(m.x - c.x), -(m.y - c.y));
        if (snap) a = std::round(a / deg2rad(15.0f)) * deg2rad(15.0f);
        a = wrapAngle(a);
        if (std::fabs(wrapAngle(a - *rad)) > 1e-4f) {
            *rad = a;
            changed = true;
        }
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float R = size * 0.5f - 4;
    dl->AddCircleFilled(c, R, IM_COL32(255, 255, 255, 10), 48);
    dl->AddCircle(c, R, IM_COL32(255, 255, 255, 40), 48, 1.0f);
    for (int k = 0; k < 24; ++k) {
        float a = k * kTau / 24;
        float l = k % 6 == 0 ? 0.86f : 0.93f;
        dl->AddLine({c.x + std::sin(a) * R * l, c.y - std::cos(a) * R * l}, {c.x + std::sin(a) * R, c.y - std::cos(a) * R},
                    IM_COL32(255, 255, 255, 60), 1.0f);
    }
    float cr = R * 0.32f;
    drawCellIcon(dl, c, cr, type, colU32(col));
    // front arrow
    const float ar = std::max(5.0f, R * 0.06f);
    dl->AddTriangleFilled({c.x, c.y - cr - 2 * ar}, {c.x - ar, c.y - cr - 0.4f * ar}, {c.x + ar, c.y - cr - 0.4f * ar}, IM_COL32(235, 238, 242, 220));
    // split direction: handle for child 1, ghost for child 2
    ImVec2 d{-std::sin(*rad), -std::cos(*rad)};
    ImVec2 h1{c.x + d.x * R * 0.72f, c.y + d.y * R * 0.72f}, h2{c.x - d.x * R * 0.72f, c.y - d.y * R * 0.72f};
    ImVec2 pl{-d.y, d.x};
    for (int k = -4; k < 4; ++k) {
        float a0 = (k + 0.2f) / 4.0f * R * 0.8f, a1 = (k + 0.7f) / 4.0f * R * 0.8f;
        dl->AddLine({c.x + pl.x * a0, c.y + pl.y * a0}, {c.x + pl.x * a1, c.y + pl.y * a1}, IM_COL32(255, 158, 51, 200), 1.5f);
    }
    const float hr = std::max(10.0f, R * 0.13f);  // handle radius
    dl->AddCircleFilled(h2, hr * 0.9f, IM_COL32(255, 158, 51, 90));
    dl->AddCircleFilled(h1, hr, kAccentU32);
    for (int k = 0; k < 2; ++k) {
        const char* n = k ? "2" : "1";
        ImVec2 ts = ImGui::CalcTextSize(n), hc = k ? h2 : h1;
        dl->AddText({hc.x - ts.x * 0.5f, hc.y - ts.y * 0.5f}, IM_COL32(30, 20, 10, 255), n);
    }
    char deg[16];
    std::snprintf(deg, sizeof(deg), "%.0f°", rad2deg(*rad));
    dl->AddText({p.x + 2, p.y + size - ImGui::GetFontSize()}, IM_COL32(200, 204, 210, 255), deg);
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
        ImGui::SetTooltip("Тяните оранжевую точку: так ляжет плоскость деления.\n1 — первый потомок, 2 — второй.");
    ImGui::PopID();
    return changed;
}

bool divides(const Mode& m) { return m.type != CellType::Gamete && m.splitMass < kNeverSplit; }

// Compact cell-type chooser: colour dot + name, the list shows each type's role on hover.
bool typeCombo(const char* id, CellType* t) {
    bool changed = false;
    const CellTypeInfo& cur = cellTypeInfo(*t);
    ImGui::PushID(id);
    float h = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    char preview[96];
    std::snprintf(preview, sizeof(preview), "      %s", cur.name);
    if (ImGui::BeginCombo("##type", preview, ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < kCellTypeCount; ++i) {
            const CellTypeInfo& info = cellTypeInfo(i);
            ImGui::PushID(i);
            ImVec2 q = ImGui::GetCursorScreenPos();
            char item[96];
            std::snprintf(item, sizeof(item), "      %s", info.name);
            if (ImGui::Selectable(item, (int)*t == i)) {
                *t = (CellType)i;
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)\n%s", info.name, info.latin, info.summary);
            ImGui::GetWindowDrawList()->AddCircleFilled({q.x + h * 0.35f, q.y + ImGui::GetTextLineHeight() * 0.5f}, h * 0.2f,
                                                        colU32({info.r, info.g, info.b}));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::GetWindowDrawList()->AddCircleFilled({p.x + h * 0.5f, p.y + h * 0.5f}, h * 0.22f, colU32({cur.r, cur.g, cur.b}));
    ImGui::PopID();
    return changed;
}

}  // namespace

// ============================================================================ coordinates
Vec2 App::previewToScreen(Vec2 w) const {
    ImGuiIO& io = ImGui::GetIO();
    VkExtent2D e = ctx_.extent();
    float fbs = (float)e.width / std::max(io.DisplaySize.x, 1.0f);
    float fx = (w.x - previewCam_.x) * previewZoom_ + e.width * 0.5f;
    float fy = -(w.y - previewCam_.y) * previewZoom_ + e.height * 0.5f;
    return {fx / fbs, fy / fbs};
}

Vec2 App::screenToPreview(Vec2 s) const {
    ImGuiIO& io = ImGui::GetIO();
    VkExtent2D e = ctx_.extent();
    float fbs = (float)e.width / std::max(io.DisplaySize.x, 1.0f);
    return {previewCam_.x + (s.x * fbs - e.width * 0.5f) / previewZoom_, previewCam_.y - (s.y * fbs - e.height * 0.5f) / previewZoom_};
}

// ============================================================================ undo / redo
void App::commitGenome() {
    if (!ui_.committedValid) {
        ui_.committed = ui_.editGenome;
        ui_.committedValid = true;
        return;
    }
    if (genomeEquals(ui_.committed, ui_.editGenome)) return;
    ui_.undo.push_back(ui_.committed);
    if (ui_.undo.size() > 64) ui_.undo.erase(ui_.undo.begin());
    ui_.redo.clear();
    ui_.committed = ui_.editGenome;
}

void App::undoGenome() {
    commitGenome();
    if (ui_.undo.empty()) return;
    ui_.redo.push_back(ui_.editGenome);
    ui_.editGenome = ui_.undo.back();
    ui_.undo.pop_back();
    ui_.committed = ui_.editGenome;
    ui_.previewDirty = true;
    toast("Отменено");
}

void App::redoGenome() {
    if (ui_.redo.empty()) return;
    ui_.undo.push_back(ui_.editGenome);
    ui_.editGenome = ui_.redo.back();
    ui_.redo.pop_back();
    ui_.committed = ui_.editGenome;
    ui_.previewDirty = true;
    toast("Повторено");
}

// ============================================================================ editor actions
int App::allocFreeMode() const {
    bool reach[kMaxModes];
    reachableModes(ui_.editGenome, reach);
    for (int i = 0; i < kMaxModes; ++i)
        if (!reach[i]) return i;
    return -1;
}

int App::selectedPreviewCell() const {
    int st = editorStage();
    if (selCell_ < 0 || layout_.stages.empty()) return -1;
    const LayoutStage& s = layout_.stages[std::min(selPhantom_ ? st + 1 : st, (int)layout_.stages.size() - 1)];
    return selCell_ < (int)s.cells.size() ? selCell_ : -1;
}

void App::selectPreviewCell(int idx) {
    selCell_ = idx;
    selPhantom_ = false;
}

// Make the selected cell's mode divide into itself-as-a-terminal-cell (child 1) and a new cell (child 2),
// placing the new cell where the organism has room (compact growth, like the reference editor).
void App::splitSelected() {
    int st = editorStage();
    if (selCell_ < 0 || selPhantom_ || st >= (int)layout_.stages.size()) return;
    const LayoutStage& stage = layout_.stages[st];
    if (selCell_ >= (int)stage.cells.size()) return;
    const LayoutCell c = stage.cells[selCell_];
    Genome& g = ui_.editGenome;
    Mode& m = g.modes.edit(c.mode);
    if (divides(m)) return;

    // Two free mode slots.
    bool reach[kMaxModes];
    reachableModes(g, reach);
    int freeModes[2] = {-1, -1}, nf = 0;
    for (int i = 0; i < kMaxModes && nf < 2; ++i)
        if (!reach[i]) freeModes[nf++] = i;
    if (nf < 2) {
        toast("Нет свободных мод (максимум 128)");
        return;
    }

    float relAngle;
    if (lastAction_.kind == ActionKind::Divide && lastAction_.keepType == false) {
        relAngle = lastAction_.relAngle;  // right-click repeat: same direction relative to the cell
    } else {
        // Score 24 directions: the new cell (child 2) should not overlap others and stay close to the body.
        Vec2 centroid{};
        for (const LayoutCell& o : stage.cells) centroid += o.pos;
        centroid = centroid / (float)stage.cells.size();
        float r = radiusForMass(c.mass * 0.5f, CellType::Phagocyte);
        float best = -1e30f, bestAng = 0;
        for (int k = 0; k < 24; ++k) {
            float a = c.angle + k * kTau / 24.0f;
            Vec2 child2 = c.pos - fromAngle(a) * r;
            Vec2 child1 = c.pos + fromAngle(a) * r;
            float clear = 1e9f;
            for (size_t j = 0; j < stage.cells.size(); ++j) {
                if ((int)j == selCell_) continue;
                const LayoutCell& o = stage.cells[j];
                float ro = radiusForMass(o.mass, CellType::Phagocyte);
                clear = std::min(clear, length(o.pos - child2) - ro - r);
                clear = std::min(clear, length(o.pos - child1) - ro - r);
            }
            float score = std::min(clear, 0.0f) * 10.0f - length(child2 - centroid) * 0.05f + (k == 0 ? 0.01f : 0.0f);
            if (stage.cells.size() == 1) score = (k == 0) ? 1.0f : 0.0f;  // zygote: split along its axis
            if (score > best) {
                best = score;
                bestAng = k * kTau / 24.0f;
            }
        }
        relAngle = bestAng;
    }
    float sgn = c.mirrored ? -1.0f : 1.0f;
    Mode keep = m;  // terminal copy of the current cell
    keep.splitMass = kNeverSplit;
    Mode born = keep;
    born.type = lastAction_.type;
    born.color = lastAction_.color;
    born.childMode[0] = born.childMode[1] = (uint8_t)freeModes[1];
    keep.childMode[0] = keep.childMode[1] = (uint8_t)freeModes[0];
    g.modes.set(freeModes[0], keep);
    g.modes.set(freeModes[1], born);
    m.splitMass = 2.034f;
    m.makeAdhesin = true;
    m.adhesinStiffness = std::max(m.adhesinStiffness, 0.5f);
    m.splitAngle = wrapAngle(sgn * relAngle);
    if (ui_.snap) m.splitAngle = wrapAngle(std::round(m.splitAngle / deg2rad(15.0f)) * deg2rad(15.0f));
    m.childAngle[0] = m.childAngle[1] = 0;
    m.childMode[0] = (uint8_t)freeModes[0];
    m.childMode[1] = (uint8_t)freeModes[1];
    if (ui_.symmetry) m.mirror[1] = true;  // the new branch develops as a mirror image
    lastAction_.kind = ActionKind::Divide;
    lastAction_.relAngle = relAngle;
    ui_.editMode = c.mode;
    ui_.previewDirty = true;
    commitGenome();
}

void App::stopSplitSelected() {
    Mode& m = ui_.editGenome.modes.edit(ui_.editMode);
    if (!divides(m)) return;
    m.splitMass = kNeverSplit;
    ui_.previewDirty = true;
    selPhantom_ = false;
    selCell_ = -1;
    editorPopup_ = false;
    commitGenome();
    toast("Деление убрано");
}

// ============================================================================ input (called from handleInput)
void App::handleGenomeInput() {
    ImGuiIO& io = ImGui::GetIO();
    if (layout_.stages.empty()) return;
    const int maxTick = editorMaxTick();

    if (!io.WantTextInput) {
        bool ctrl = io.KeyCtrl;
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) undoGenome();
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) {
            std::string path = dataDir("genomes") + "/" + safeFileName(ui_.genomeFile, "геном") + ".genome.txt";
            toast(saveGenome(ui_.editGenome, path) ? std::string("Геном сохранён: ") + ui_.genomeFile : "Ошибка сохранения");
        }
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_D, false)) {
            int j = allocFreeMode();
            if (j >= 0) {
                commitGenome();
                ui_.editGenome.modes.set(j, ui_.editGenome.modes[ui_.editMode]);
                ui_.editGenome.labels.set(j, ui_.editGenome.labels[ui_.editMode]);
                toast("Копия моды M" + std::to_string(ui_.editMode + 1) + " — M" + std::to_string(j + 1));
                ui_.editMode = j;
            } else {
                toast("Нет свободных мод");
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || ImGui::IsKeyPressed(ImGuiKey_PageUp)) {
            // Step through the modes the organism uses.
            bool reach[kMaxModes];
            reachableModes(ui_.editGenome, reach);
            int dir = ImGui::IsKeyPressed(ImGuiKey_PageDown) ? 1 : -1;
            for (int k = 1; k < kMaxModes; ++k) {
                int j = (ui_.editMode + dir * k + kMaxModes) % kMaxModes;
                if (reach[j]) {
                    ui_.editMode = j;
                    selCell_ = -1;
                    builtTick_ = -1;
                    break;
                }
            }
        }
        if (ctrl && (ImGui::IsKeyPressed(ImGuiKey_Y) || (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) redoGenome();
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            editorTick_ = io.KeyShift ? std::min(maxTick, (editorTick_ / kTicksPerStage + 1) * kTicksPerStage) : std::min(maxTick, editorTick_ + 1);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            editorTick_ = io.KeyShift ? std::max(0, ((editorTick_ - 1) / kTicksPerStage) * kTicksPerStage) : std::max(0, editorTick_ - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) editorTick_ = maxTick;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) previewAutoFrame_ = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            editorPopup_ = false;
            selCell_ = -1;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && selCell_ >= 0) stopSplitSelected();
    }

    Vec2 mouse{io.MousePos.x, io.MousePos.y};
    if (!ImGui::IsMousePosValid()) {
        // A lifted finger leaves ImGui with "no mouse" (-FLT_MAX); a pinch step arriving then zoomed about that point
        // and threw the camera to 1e38 µm — a black editor. The last touch point is always valid.
        double x, y;
        glfwGetCursorPos(window_, &x, &y);
        mouse = {(float)x, (float)y};
    }
    bool overUi = io.WantCaptureMouse;

    // Zoom about the cursor.
    if (!overUi && (io.MouseWheel != 0 || pinchSteps_ != 0)) {
        Vec2 before = screenToPreview(mouse);
        previewZoom_ = std::clamp(previewZoom_ * std::pow(mobile_ ? 1.18f : 1.15f, io.MouseWheel + pinchSteps_), 0.2f, 40.0f);
        Vec2 after = screenToPreview(mouse);
        previewCam_ = previewCam_ + (before - after);
        previewAutoFrame_ = false;
    }

    // Left button: select / open actions, drag phantoms, or pan on empty space.
    if (!overUi && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool phantom = false;
        int hit = pickPreview(mouse, &phantom);
        if (hit >= 0) {
            int st = editorStage();
            if (editorPopup_ && hit == selCell_ && phantom == selPhantom_) editorRetap_ = true;
            selCell_ = hit;
            selPhantom_ = phantom;
            if (phantom) {
                const LayoutCell& ph = layout_.stages[st + 1].cells[hit];
                ui_.editMode = layout_.stages[st].cells[ph.parent].mode;
                dragPhantom_ = hit;
            } else {
                const LayoutStage& sv = layout_.stages[editorTick_ % kTicksPerStage == 0 ? st : std::min(st + 1, (int)layout_.stages.size() - 1)];
                if (hit < (int)sv.cells.size()) ui_.editMode = sv.cells[hit].mode;
                // Clicking mid-animation snaps the timeline to the stage the cell belongs to.
                if (editorTick_ % kTicksPerStage != 0) editorTick_ = std::min(maxTick, (st + 1) * kTicksPerStage);
            }
            editorPopup_ = true;
            editorPopupPos_ = mouse;
            builtTick_ = -1;  // refresh mode highlight / phantoms
        } else {
            editorPopup_ = false;
            selCell_ = -1;
            previewPanning_ = true;
            builtTick_ = -1;
        }
    }
    bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    if (!lmb) {
        if (dragPhantom_ >= 0) {
            commitGenome();
            ui_.previewDirty = true;  // full rebuild (the drag only built up to the edited stage)
        }
        dragPhantom_ = -1;
        previewPanning_ = false;
    }

    if (dragPhantom_ >= 0 && lmb && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
        // Dragging a phantom around its parent rotates the parent's split direction.
        int st = editorStage();
        if (st + 1 < (int)layout_.stages.size() && dragPhantom_ < (int)layout_.stages[st + 1].cells.size()) {
            const LayoutCell& ph = layout_.stages[st + 1].cells[dragPhantom_];
            const LayoutCell& par = layout_.stages[st].cells[ph.parent];
            Mode& pm = ui_.editGenome.modes.edit(par.mode);
            Vec2 w = screenToPreview(mouse);
            Vec2 dir = normalize(w - par.pos);
            if (ph.childSlot == 1) dir = -dir;
            float sgn = par.mirrored ? -1.0f : 1.0f;
            float a = wrapAngle(sgn * (std::atan2(dir.y, dir.x) - par.angle));
            if (ui_.snap) a = wrapAngle(std::round(a / deg2rad(15.0f)) * deg2rad(15.0f));
            if (std::fabs(wrapAngle(a - pm.splitAngle)) > 1e-4f) {
                pm.splitAngle = a;
                lastAction_.relAngle = a * sgn;
                ui_.previewDirty = true;
            }
        }
    }
    if (previewPanning_ || (!overUi && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)))) {
        ImVec2 d = io.MouseDelta;
        VkExtent2D e = ctx_.extent();
        float fbs = (float)e.width / std::max(io.DisplaySize.x, 1.0f);
        if (d.x != 0 || d.y != 0) {
            previewCam_ = previewCam_ + Vec2{-d.x * fbs / previewZoom_, d.y * fbs / previewZoom_};
            previewAutoFrame_ = false;
        }
    }

    // Right click: repeat the last action on the clicked cell (no dialog).
    if (!overUi && !mobile_ && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f) &&
        io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] < 16.0f) {
        bool phantom = false;
        int hit = pickPreview(mouse, &phantom);
        if (hit >= 0 && !phantom && editorTick_ % kTicksPerStage == 0 && lastAction_.kind != ActionKind::None) {
            selCell_ = hit;
            selPhantom_ = false;
            const LayoutCell& c = layout_.stages[editorStage()].cells[hit];
            ui_.editMode = c.mode;
            if (lastAction_.kind == ActionKind::Divide) {
                bool saved = lastAction_.keepType;
                lastAction_.keepType = false;  // reuse the remembered direction
                splitSelected();
                lastAction_.keepType = saved;
            } else {
                Mode& m = ui_.editGenome.modes.edit(c.mode);
                m.type = lastAction_.type;
                m.color = lastAction_.color;
                ui_.previewDirty = true;
                commitGenome();
            }
            builtTick_ = -1;
        }
    }
}

// ============================================================================ screen
void App::drawGenomeScreen() {
    if (!ui_.committedValid) commitGenome();
    Genome before = ui_.editGenome;
    if (mobile_) {
        drawMobileGenome();
    } else {
        if (cfg_.winGenomeBar) drawGenomeToolbar();
        if (cfg_.winModes) drawModeList();
        if (cfg_.winMode) drawModePanel();
        if (cfg_.winTimeline) drawTimeline();
        drawEditorTips();
    }
    drawEditorHandles();
    if (!genomeEquals(before, ui_.editGenome)) {
        ui_.previewDirty = true;
        builtTick_ = -1;
    }
    // Slider drags become one undo step when released.
    if (!ImGui::IsAnyItemActive() && dragPhantom_ < 0) commitGenome();
}

void App::drawGenomeToolbar() {
    ImGuiIO& io = ImGui::GetIO();
    Genome& g = ui_.editGenome;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10 * uiScale_, 8 * uiScale_});
    bool vis = beginPanel("Геном###gbar", &cfg_.winGenomeBar, {io.DisplaySize.x * 0.5f, (kNavH + 10) * uiScale_}, {0, 0}, {0.5f, 0.0f},
                          ImGuiWindowFlags_AlwaysAutoResize);
    if (!vis) {
        ImGui::End();
        ImGui::PopStyleVar();
        return;
    }
    ImGui::SetNextItemWidth(200 * uiScale_);
    char name[128];
    std::snprintf(name, sizeof(name), "%s", g.name.c_str());
    if (ImGui::InputTextWithHint("##name", "Название генома", name, sizeof(name))) g.name = name;
    ImGui::SameLine();
    if (pill("Новый")) genomeNew();
    ImGui::SameLine();
    if (pill("Загрузить")) openGenomeBrowser();
    ImGui::SameLine();
    if (pill("Сохранить")) ImGui::OpenPopup("savegen");
    ImGui::SameLine();
    if (dropButton("Поделиться")) ImGui::OpenPopup("gshare");
    if (ImGui::BeginPopup("gshare")) {
        genomeShareItems(g);
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Файл генома и сообщение для мессенджера (на ПК файл откроется в проводнике)");
    ImGui::SameLine();
    int tc = trackedCell();
    ImGui::BeginDisabled(tc < 0);
    if (pill("Из клетки") && tc >= 0) genomeFromCell();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Геном клетки, выбранной в микроскопе");
    ImGui::SameLine(0, 16 * uiScale_);
    if (pill("Связи", ui_.showLinks)) ui_.showLinks = !ui_.showLinks;
    ImGui::SameLine();
    if (pill("Сетка", cfg_.showGrid)) cfg_.showGrid = !cfg_.showGrid;
    ImGui::SameLine();
    if (pill("Симметрия", ui_.symmetry)) ui_.symmetry = !ui_.symmetry;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Новые ветви при делении развиваются зеркально");
    ImGui::SameLine();
    if (pill("Привязка 15°", ui_.snap)) ui_.snap = !ui_.snap;
    ImGui::SameLine(0, 16 * uiScale_);
    if (pill(cfg_.editorSimple ? "Простой режим" : "Полный режим", cfg_.editorSimple)) {
        cfg_.editorSimple = !cfg_.editorSimple;
        markConfigDirty();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Простой: тип, цвет, деление и потомки — всё картинками.\nПолный: программы, сигналы и тонкая настройка.");
    ImGui::SameLine();
    if (pill("?", cfg_.editorTips)) {
        cfg_.editorTips = !cfg_.editorTips;
        markConfigDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Как пользоваться редактором");

    ImGui::SameLine(0, 16 * uiScale_);
    ImGui::BeginDisabled(ui_.undo.empty());
    if (pill("Отменить")) undoGenome();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Z");
    ImGui::SameLine();
    ImGui::BeginDisabled(ui_.redo.empty());
    if (pill("Повторить")) redoGenome();
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Ctrl+Y");
    ImGui::SameLine(0, 16 * uiScale_);
    if (pill("Взрослый в чашку")) genomeAdultToDish();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Вставить организм сразу выросшим (последняя стадия развития)");
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.62f, 0.20f, 0.75f));
    if (ImGui::Button("В чашку ›")) {
        ui_.screen = Screen::Microscope;
        ui_.tool = Tool::Synth;
        toast("Синтезатор: щёлкните в чашке, чтобы вставить клетку");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Перейти в микроскоп с синтезатором этого генома");

    drawGenomeFilePopups();
    ImGui::End();
    ImGui::PopStyleVar();
}

void App::genomeNew() {
    commitGenome();
    ui_.editGenome = Genome();
    ui_.editMode = 0;
    selCell_ = -1;
    editorTick_ = 0;
    ui_.previewDirty = true;
    commitGenome();
}

void App::genomeFromCell() {
    int tc = trackedCell();
    if (tc < 0) return;
    commitGenome();
    const Cell& c = world_->cells()[tc];
    Genome& g = ui_.editGenome;
    g = world_->genomes().get(c.genome);
    g.initialMode = c.mode;
    ui_.editMode = c.mode;
    editorTick_ = 0;
    ui_.previewDirty = true;
    commitGenome();
}

void App::genomeAdultToDish() {
    GenomeLayout full = buildGenomeLayout(ui_.editGenome, 160, 4000);
    float a = world_->settings.dishRadius;
    Rng rng((uint64_t)(time_ * 1000.0) | 1);
    Vec2 at = fromAngle(rng.range(0, kTau)) * (a * 0.3f * std::sqrt(rng.uniform()));
    int n = spawnGrownOrganism(*world_, ui_.editGenome, full, at, rng.range(0, kTau));
    ui_.screen = Screen::Microscope;
    toast(n > 0 ? "Вставлен взрослый организм: " + std::to_string(n) + " клеток" : "Нет места в чашке");
}

// "Save" popup of the genome bar (opened with OpenPopup("savegen") in the same window). Loading is the genome browser.
void App::drawGenomeFilePopups() {
    Genome& g = ui_.editGenome;
    ImGuiIO& io = ImGui::GetIO();
    if (mobile_) ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopup("savegen")) {
        ImGui::TextUnformatted("Сохранить в банк генов");
        ImGui::SetNextItemWidth(260 * uiScale_);
        ImGui::InputText("##gf", ui_.genomeFile, sizeof(ui_.genomeFile));
        if (ImGui::Button("Сохранить", {260 * uiScale_, 0})) {
            std::string path = dataDir("genomes") + "/" + safeFileName(ui_.genomeFile, "геном") + ".genome.txt";
            toast(saveGenome(g, path) ? "Геном сохранён" : "Ошибка сохранения");
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::Button("Экспорт для Cell Lab (.genome)", {260 * uiScale_, 0})) {
            std::string path = dataDir("genomes") + "/" + safeFileName(ui_.genomeFile, "геном") + ".genome";
            std::string err;
            if (exportCellLabGenome(g, path, err)) {
                toast(err.empty() ? "Экспортировано для Cell Lab без потерь" : "Экспортировано для Cell Lab. Потери: " + err);
                ui_.toastUntil = time_ + (err.empty() ? 2.5 : 7.0);
            } else {
                toast("Ошибка: " + err);
            }
            ImGui::CloseCurrentPopup();
        }
        if (mobile_) {  // the bank lives in the app's private storage: these copy a file out to where the user picks
            ImGui::Separator();
            if (ImGui::Button("В файл на телефоне…", {260 * uiScale_, 0})) {
                std::string path = dataDir("genomes") + "/" + safeFileName(ui_.genomeFile, "геном") + ".genome.txt";
                if (saveGenome(g, path)) exportToDevice(path, safeFileName(ui_.genomeFile, "геном") + ".genome.txt");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::Button("Для Cell Lab в файл…", {260 * uiScale_, 0})) {
                std::string path = dataDir("genomes") + "/" + safeFileName(ui_.genomeFile, "геном") + ".genome";
                std::string err;
                if (exportCellLabGenome(g, path, err)) exportToDevice(path, safeFileName(ui_.genomeFile, "геном") + ".genome");
                else toast("Ошибка: " + err);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------- left: modes of the organism
void App::drawModeList() {
    ImGuiIO& io = ImGui::GetIO();
    float top = (kNavH + 80) * uiScale_, bottom = 130 * uiScale_;
    if (beginPanel("Моды###modes", &cfg_.winModes, {12 * uiScale_, top}, {kListW * uiScale_, io.DisplaySize.y - top - bottom}, {0, 0}))
        drawModeListBody();
    ImGui::End();
}

void App::drawModeListBody() {
    Genome& g = ui_.editGenome;
    const float u = uiScale_;
    static bool showAll = false;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("все").x - ImGui::GetFrameHeight() -
                            2 * ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::InputTextWithHint("##filter", "поиск: номер, имя, тип", ui_.modeFilter, sizeof(ui_.modeFilter));
    ImGui::SameLine();
    ImGui::Checkbox("все", &showAll);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Показывать и моды, которые организм не использует");
    ImGui::Separator();
    bool reach[kMaxModes];
    reachableModes(g, reach);
    const std::string filter = lowerUtf8(ui_.modeFilter);
    // Phone: the actions on the selected mode are buttons under the list (a long press is hard to discover).
    const float footer = mobile_ ? ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().ItemSpacing.y : ImGui::GetTextLineHeightWithSpacing() * 1.6f;
    ImGui::BeginChild("modelist", {0, -footer});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = (mobile_ ? 54 : 38) * u, iconX = (mobile_ ? 24 : 18) * u, iconR = (mobile_ ? 14 : 11) * u;
    const float textX = iconX * 2;
    const float smallH = fontSmall_->FontSize, textY = std::round((rowH - ImGui::GetFontSize() - smallH - 1 * u) * 0.5f);
    for (int i = 0; i < kMaxModes; ++i) {
        if (!showAll && !reach[i]) continue;
        const Mode& m = g.modes[i];
        if (!filter.empty()) {
            std::string hay = lowerUtf8("m" + std::to_string(i + 1) + " " + g.labels[i] + " " + cellTypeInfo(m.type).name);
            bool numeric = filter.find_first_not_of("m0123456789") == std::string::npos;
            bool hit = numeric ? lowerUtf8("m" + std::to_string(i + 1)) == (filter[0] == 'm' ? filter : "m" + filter)
                               : hay.find(filter) != std::string::npos;
            if (!hit) continue;
        }
        ImGui::PushID(i);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        bool sel = ui_.editMode == i;
        if (ImGui::InvisibleButton("row", {w, rowH})) {
            if (mobile_ && ui_.editMode == i) dockGenome_.tab = 0;  // phone: a second tap opens the mode
            ui_.editMode = i;
            selCell_ = -1;
            builtTick_ = -1;
        }
        bool hov = ImGui::IsItemHovered();
        if (sel || hov) dl->AddRectFilled(p, {p.x + w, p.y + rowH}, sel ? IM_COL32(255, 158, 51, 45) : IM_COL32(255, 255, 255, 12), 6);
        if (sel) dl->AddRectFilled(p, {p.x + 3, p.y + rowH}, kAccentU32, 2);
        float a = reach[i] ? 1.0f : 0.45f;
        drawCellIcon(dl, {p.x + iconX, p.y + rowH * 0.5f}, iconR, (int)m.type, colU32(m.color), a);
        if (i == g.initialMode) dl->AddCircle({p.x + iconX, p.y + rowH * 0.5f}, iconR + 1.5f * u, kAccentU32, 0, 1.5f * u);
        char l1[96], l2[96];
        const bool named = !g.labels[i].empty();
        std::snprintf(l1, sizeof(l1), "M%d  %s%s", i + 1, named ? g.labels[i].c_str() : cellTypeInfo(m.type).name,
                      i == g.initialMode ? "  · начальная" : "");
        if (!divides(m)) std::snprintf(l2, sizeof(l2), "%s%sне делится", named ? cellTypeInfo(m.type).name : "", named ? " · " : "");
        else
            std::snprintf(l2, sizeof(l2), "%s%s→ M%d + M%d%s", named ? cellTypeInfo(m.type).name : "", named ? " · " : "",
                          m.childMode[0] + 1, m.childMode[1] + 1, m.makeAdhesin ? " · вместе" : "");
        dl->PushClipRect(p, {p.x + w, p.y + rowH}, true);
        dl->AddText({p.x + textX, p.y + textY}, IM_COL32(235, 237, 240, (int)(255 * a)), l1);
        dl->AddText(fontSmall_, smallH, {p.x + textX, p.y + textY + ImGui::GetFontSize() + 1 * u}, IM_COL32(160, 164, 170, (int)(255 * a)), l2);
        dl->PopClipRect();
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Сделать начальной")) g.initialMode = i;
            if (ImGui::MenuItem("Дублировать", "Ctrl+D")) {
                int j = allocFreeMode();
                if (j >= 0) {
                    g.modes.set(j, g.modes[i]);
                    g.labels.set(j, g.labels[i]);
                    ui_.editMode = j;
                }
            }
            if (ImGui::BeginMenu("Копировать в")) {
                for (int j = 0; j < kMaxModes; ++j) {
                    char it[16];
                    std::snprintf(it, sizeof(it), "M%d", j + 1);
                    if (j != i && ImGui::MenuItem(it)) {
                        g.modes.set(j, g.modes[i]);
                        g.labels.set(j, g.labels[i]);
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Сбросить")) {
                g.modes.set(i, defaultModeFor(i));
                g.labels.set(i, "");
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (mobile_) {
        // Actions on the selected mode.
        const int i = ui_.editMode;
        const float sp = ImGui::GetStyle().ItemSpacing.x, bw = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3;
        ImGui::BeginDisabled(g.initialMode == i);
        if (ImGui::Button("Начальная", {bw, 0})) g.initialMode = i;
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Копия", {bw, 0})) {
            int j = allocFreeMode();
            if (j >= 0) {
                g.modes.set(j, g.modes[i]);
                g.labels.set(j, g.labels[i]);
                ui_.editMode = j;
                toast("Копия: M" + std::to_string(j + 1));
            } else {
                toast("Свободных мод не осталось (максимум 128).");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Открыть", {bw, 0})) dockGenome_.tab = 0;
    } else {
        ImGui::PushFont(fontSmall_);
        ImGui::TextDisabled("Кружок — начальная мода · ПКМ — действия · PgUp/PgDn");
        ImGui::PopFont();
    }
}

// ---------------------------------------------------------------------------- right: settings of the mode
void App::drawModePanel() {
    ImGuiIO& io = ImGui::GetIO();
    float top = (kNavH + 80) * uiScale_, bottom = 130 * uiScale_;
    float w = kPanelW * uiScale_;
    char title[96];
    std::snprintf(title, sizeof(title), "Мода M%d###mode", ui_.editMode + 1);
    if (beginPanel(title, &cfg_.winMode, {io.DisplaySize.x - w - 12 * uiScale_, top}, {w, io.DisplaySize.y - top - bottom}, {0, 0}))
        drawModePanelBody();
    ImGui::End();
}

void App::drawModePanelBody() {
    Genome& g = ui_.editGenome;
    Mode& m = g.modes.edit(ui_.editMode);
    const float u = uiScale_;
    const float contentRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;  // window-local x of the right edge
    colorSwatch(m.color, (mobile_ ? 24 : 20) * u);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(fontBold_);
    ImGui::Text("M%d", ui_.editMode + 1);
    ImGui::PopFont();
    ImGui::SameLine();
    {
        char label[96];
        std::snprintf(label, sizeof(label), "%s", g.labels[ui_.editMode].c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (mobile_ ? 0 : 100 * u));
        if (ImGui::InputTextWithHint("##label", cellTypeInfo(m.type).name, label, sizeof(label))) g.labels.set(ui_.editMode, label);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Имя моды (необязательно) — показывается в списке и в выборе потомков");
    }
    if (!mobile_) ImGui::SameLine();
    bool initial = g.initialMode == ui_.editMode;
    if (ImGui::Checkbox("начальная", &initial) && initial) g.initialMode = ui_.editMode;
    if (mobile_) {
        // Phone: the simple / full switch is here, in sight, not in a menu.
        const char* names[] = {"Простой", "Полный"};
        const float sp = 3 * u;
        float bw = 0;
        for (const char* n : names) bw = std::max(bw, ImGui::CalcTextSize(n).x + 2 * ImGui::GetStyle().FramePadding.x);
        ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetStyle().ItemSpacing.x, contentRight - 2 * bw - sp));
        for (int k = 0; k < 2; ++k) {
            if (k) ImGui::SameLine(0, sp);
            const bool on = cfg_.editorSimple == (k == 0);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.42f));
            if (ImGui::Button(names[k], {bw, 0}) && !on) {
                cfg_.editorSimple = k == 0;
                markConfigDirty();
            }
            if (on) ImGui::PopStyleColor();
        }
    }
    // Where this mode comes from and where it leads: one tap jumps there.
    {
        const float right = contentRight;
        auto label = [&](const char* text) {
            ImGui::PushFont(fontSmall_);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", text);
            ImGui::PopFont();
        };
        auto jump = [&](int j, const char* prefix) {
            char b[96];
            std::snprintf(b, sizeof(b), "%sM%d %s##j%d%s", prefix, j + 1, g.labels[j].empty() ? cellTypeInfo(g.modes[j].type).name : g.labels[j].c_str(),
                          j, prefix);
            bool r;
            if (mobile_) {  // finger-sized, wrapping onto the next line when the row is full
                const float w = ImGui::CalcTextSize(b, nullptr, true).x + 2 * ImGui::GetStyle().FramePadding.x;
                ImGui::SameLine();
                if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10 * u, 8 * u});
                r = ImGui::Button(b);
                ImGui::PopStyleVar();
            } else {
                ImGui::SameLine();
                ImGui::PushFont(fontSmall_);
                r = ImGui::SmallButton(b);
                ImGui::PopFont();
            }
            if (r) {
                ui_.editMode = j;
                selCell_ = -1;
                builtTick_ = -1;
            }
            return r;
        };
        int shown = 0;
        for (int j = 0; j < kMaxModes && shown < 4; ++j) {
            const Mode& o = g.modes[j];
            bool parent = divides(o) && (o.childMode[0] == ui_.editMode || o.childMode[1] == ui_.editMode);
            if (o.type == CellType::Stemocyte && (o.stemMode[0] == ui_.editMode || o.stemMode[1] == ui_.editMode)) parent = true;
            if (o.type == CellType::Cystocyte && o.stemMode[0] == ui_.editMode) parent = true;
            if (!parent || j == ui_.editMode) continue;
            if (shown == 0) label(mobile_ ? "Родитель:" : "из:");
            jump(j, "← ");
            ++shown;
        }
        if (divides(m)) {
            if (mobile_ || shown == 0) label(mobile_ ? "Потомки:" : "в:");
            for (int c = 0; c < 2; ++c) {
                if (c == 1 && m.childMode[1] == m.childMode[0]) break;
                if (m.childMode[c] != ui_.editMode) jump(m.childMode[c], "→ ");
            }
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("modescroll", {0, 0});
    gLabelW = mobile_ ? std::min(140 * u, ImGui::GetContentRegionAvail().x * 0.4f) : 140 * u;
    drawModeEditor(m);
    ImGui::EndChild();
}

// Beginner layout of a mode: what the cell is, its colour, when and how it divides and what the children become.
// Programs, signals and fine-tuning live in the full layout.
void App::drawModeEditorSimple(Mode& m) {
    Genome& g = ui_.editGenome;
    const float aw = ImGui::GetContentRegionAvail().x;
    auto segmented = [&](const char* id, const char* const* labels, int n, int cur) {
        int res = cur;
        ImGui::PushID(id);
        float bw = (aw - (n - 1) * 3.0f) / n;
        for (int k = 0; k < n; ++k) {
            if (k) ImGui::SameLine(0, 3);
            bool on = k == cur;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.42f));
            ImGui::PushID(k);
            ImGui::PushFont(fontSmall_);
            if (ImGui::Button(labels[k], {bw, ImGui::GetFrameHeight() * 1.1f})) res = k;
            ImGui::PopFont();
            ImGui::PopID();
            if (on) ImGui::PopStyleColor();
        }
        ImGui::PopID();
        return res;
    };
    auto note = [&](const char* text) {
        ImGui::PushFont(fontSmall_);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    };

    sectionHeader("Какая это клетка");
    {
        CellType old = m.type;
        CellType t = m.type;
        if (typePicker("stype", &t, aw) && t != old) {
            m.color = {cellTypeInfo(t).r, cellTypeInfo(t).g, cellTypeInfo(t).b};  // the new type's own colour
            m.type = t;
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(cellTypeInfo(m.type).summary);
        ImGui::TextColored(ImVec4(0.75f, 0.70f, 0.60f, 1.0f), "%s", cellTypeQuip(m.type));
        ImGui::PopTextWrapPos();
    }

    sectionHeader("Цвет");
    colorPalette("spal", &m.color, aw, mobile_);

    // What this type does, in plain words with one knob.
    switch (m.type) {
        case CellType::Flagellocyte:
            sectionHeader("Жгутик");
            if (m.prog[PF_Swim].input == ProgInput::Fixed) {
                float v = m.prog[PF_Swim].b * 100.0f;
                if (sliderF("Сила", &v, 0.0f, 100.0f, "%.0f %%")) m.prog[PF_Swim].b = v / 100.0f;
                note("Жгутик толкает клетку вперёд, туда, куда смотрит её нос (стрелка). Чем сильнее, тем больше ест энергии.");
            } else {
                note("Сила жгутика управляется сигналом — настройка в полном режиме.");
            }
            break;
        case CellType::Buoyocyte:
            sectionHeader("Пузырь");
            if (m.prog[PF_Density].input == ProgInput::Fixed) {
                sliderF("Плотность", &m.prog[PF_Density].b, -3.0f, 3.0f, "%.1f");
                note("Меньше нуля — всплывает, больше — тонет. Работает, когда в чашке включена гравитация.");
            }
            break;
        case CellType::Ciliocyte:
            sectionHeader("Реснички");
            if (m.prog[PF_Bend].input == ProgInput::Fixed && m.prog[PF_Lift].input == ProgInput::Fixed) {
                float v = m.prog[PF_Bend].b;
                if (sliderF("Скорость", &v, -1.0f, 1.0f, "%.2f")) m.prog[PF_Bend].b = m.prog[PF_Lift].b = v;
                note("Реснички тянут соседние клетки вдоль своей оси. Положительная скорость — вперёд.");
            }
            break;
        case CellType::Senseocyte:
        case CellType::Stereocyte:
            sectionHeader("Глаз");
            rowLabel("Видит");
            if (ImGui::BeginCombo("##st", senseTargetName(m.senseTarget))) {
                for (int i = 0; i < (int)SenseTarget::Count; ++i)
                    if (ImGui::Selectable(senseTargetName((SenseTarget)i), (int)m.senseTarget == i)) m.senseTarget = (SenseTarget)i;
                ImGui::EndCombo();
            }
            note(m.type == CellType::Stereocyte ? "Стереоцит говорит, слева цель (+) или справа (−). Чтобы на это реагировать, "
                                                  "нужны нейроны и мышцы — полный режим."
                                                : "Сенсоцит выдаёт сигнал, когда видит цель. Кто его услышит — решается в полном режиме.");
            break;
        case CellType::Secrocyte:
            sectionHeader("Выделяет");
            rowLabel("Вещество");
            if (ImGui::BeginCombo("##sec", secretionName(m.secretion))) {
                for (int i = 0; i < (int)Secretion::Count; ++i)
                    if (ImGui::Selectable(secretionName((Secretion)i), (int)m.secretion == i)) m.secretion = (Secretion)i;
                ImGui::EndCombo();
            }
            break;
        case CellType::Chromatocyte:
            sectionHeader("Пигмент");
            colorPalette("pig", &m.senseColor, aw, mobile_);
            if (m.prog[PF_Swim].input == ProgInput::Fixed) {
                float v = m.prog[PF_Swim].b * 100.0f;
                if (sliderF("Сколько пигмента", &v, 0.0f, 100.0f, "%.0f %%")) m.prog[PF_Swim].b = v / 100.0f;
            }
            note("Клетка перекрашивается в цвет пигмента. Глаза хищников видят новый цвет — так прячутся.");
            break;
        case CellType::Luminocyte:
            sectionHeader("Свечение");
            colorPalette("glow", &m.senseColor, aw, mobile_);
            if (m.prog[PF_Swim].input == ProgInput::Fixed) {
                float v = m.prog[PF_Swim].b * 100.0f;
                if (sliderF("Яркость", &v, 0.0f, 100.0f, "%.0f %%")) m.prog[PF_Swim].b = v / 100.0f;
            }
            note("Светится в темноте. Чужие глаза «на свет» плывут к огоньку — так работает приманка удильщика. "
                 "Чем ярче, тем дороже.");
            break;
        case CellType::Grappler: {
            sectionHeader("Хватка");
            if (m.prog[PF_Swim].input == ProgInput::Fixed) {
                static const char* grips[] = {"Разжата", "Клетки", "И стенку"};
                float b = m.prog[PF_Swim].b;
                int cur = b >= 0.8f ? 2 : (b >= 0.3f ? 1 : 0);
                int nw = segmented("grip", grips, 3, cur);
                if (nw != cur) m.prog[PF_Swim].b = nw == 2 ? 1.0f : (nw == 1 ? 0.5f : 0.0f);
            }
            note("Хватает чужие клетки, к которым прикоснётся (до трёх сразу), а при сильной хватке — и стенку чашки. "
                 "Отпускать по сигналу — в полном режиме. Кератиноциты захватить нельзя.");
            break;
        }
        case CellType::Cystocyte:
            sectionHeader("Циста");
            note("Спит под бронёй и почти не тратит еды: так переживают голод, хищников и яды. Когда проснуться и в "
                 "кого — настраивается в полном режиме (например, вход «Еда рядом»).");
            if (ImGui::Button("Открыть полный режим", {aw, 0})) {
                cfg_.editorSimple = false;
                markConfigDirty();
            }
            break;
        case CellType::Myocyte:
        case CellType::Neurocyte:
        case CellType::Stemocyte:
        case CellType::Electrocyte:
        case CellType::Cnidocyte:
        case CellType::Virocyte:
        case CellType::Gamete:
            sectionHeader("Настройка");
            note("У этой клетки есть программа (сигналы, пороги, моды) — она в полном режиме редактора.");
            if (ImGui::Button("Открыть полный режим", {aw, 0})) {
                cfg_.editorSimple = false;
                markConfigDirty();
            }
            break;
        default: break;
    }

    sectionHeader("Деление");
    {
        static const char* when[] = {"Сразу", "Маленькой", "Средней", "Крупной", "Никогда"};
        static const float mass[] = {kSplitAtAge, kSplitMassMin + 6 * kSplitMassStep, kSplitMassMin + 12 * kSplitMassStep,
                                     kSplitMassMin + 18 * kSplitMassStep, kNeverSplit};
        int cur = 2;
        if (m.splitMass <= kSplitAtAge) cur = 0;
        else if (m.splitMass >= kNeverSplit) cur = 4;
        else if (m.splitMass < 1.9f) cur = 1;
        else if (m.splitMass > 2.7f) cur = 3;
        ImGui::TextUnformatted("Когда делиться");
        int nw = segmented("when", when, 5, cur);
        if (nw != cur) m.splitMass = mass[nw];
    }
    if (m.type == CellType::Gamete) {
        note("Гаметы не делятся: они ищут пару, чтобы слиться.");
        return;
    }
    if (m.splitMass < kNeverSplit) {
        float dial = std::min(aw * 0.55f, 170 * uiScale_);
        if (splitDial("dir", &m.splitAngle, (int)m.type, m.color, ui_.snap, dial)) {}
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + aw - dial - 10);
        ImGui::PushFont(fontSmall_);
        ImGui::TextWrapped("Направление деления. Клетка смотрит вверх (стрелка). Потомок 1 рождается со стороны точки 1.");
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
        ImGui::Checkbox("Держаться вместе", &m.makeAdhesin);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Потомки соединятся адгезином — так получается многоклеточный организм.");
        ImGui::EndGroup();

        // The two children as cards: picture, what they are, and a jump.
        for (int c = 0; c < 2; ++c) {
            ImGui::PushID(c);
            const int cm = m.childMode[c];
            const Mode& child = g.modes[cm];
            ImVec2 p = ImGui::GetCursorScreenPos();
            const float ic = (mobile_ ? 52 : 46) * uiScale_;  // icon column
            const float btnY = (mobile_ ? 10 : 6) * uiScale_ + ImGui::GetFontSize() + 4 * uiScale_;
            float h = mobile_ ? btnY + ImGui::GetFrameHeight() + 8 * uiScale_ : 46 * uiScale_;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(p, {p.x + aw, p.y + h}, IM_COL32(255, 255, 255, 12), 8);
            drawCellIcon(dl, {p.x + ic * 0.5f, p.y + h * 0.5f}, ic * 0.32f, (int)child.type, colU32(child.color));
            char l1[96];
            std::snprintf(l1, sizeof(l1), "Потомок %d: M%d %s", c + 1, cm + 1,
                          g.labels[cm].empty() ? cellTypeInfo(child.type).name : g.labels[cm].c_str());
            dl->AddText({p.x + ic, p.y + (mobile_ ? 10 : 6) * uiScale_}, IM_COL32(235, 238, 242, 255), l1);
            ImGui::SetCursorScreenPos({p.x + ic, p.y + (mobile_ ? btnY : 24 * uiScale_)});
            // Phone: three ordinary buttons across the card; desktop: small links.
            const float bw = mobile_ ? (aw - ic - 8 * uiScale_ - 2 * ImGui::GetStyle().ItemSpacing.x) / 3 : 0;
            auto cardButton = [&](const char* phone, const char* desk) {
                return mobile_ ? ImGui::Button(phone, {bw, 0}) : ImGui::SmallButton(desk);
            };
            if (!mobile_) ImGui::PushFont(fontSmall_);
            ImGui::BeginDisabled(mobile_ && cm == ui_.editMode);
            if (cardButton(cm == ui_.editMode ? "Эта же" : "Открыть", cm == ui_.editMode ? "это эта же мода" : "открыть")) {
                ui_.editMode = cm;
                selCell_ = -1;
                builtTick_ = -1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (cardButton("Новая", "сделать новой модой")) {
                int j = allocFreeMode();
                if (j >= 0) {
                    g.modes.set(j, g.modes[cm]);
                    m.childMode[c] = (uint8_t)j;
                } else {
                    toast("Свободных мод не осталось (максимум 128).");
                }
            }
            if (mobile_ && ImGui::IsItemHovered()) ImGui::SetTooltip("Сделать потомка новой модой (копией этой)");
            ImGui::SameLine();
            if (cardButton("Выбрать…", "выбрать…")) ImGui::OpenPopup("pick");
            if (!mobile_) ImGui::PopFont();
            if (ImGui::BeginPopup("pick")) {
                bool reach[kMaxModes];
                reachableModes(g, reach);
                if (mobile_) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * uiScale_, 18 * uiScale_});  // finger-sized rows
                for (int j = 0; j < kMaxModes; ++j) {
                    if (!reach[j] && j != cm) continue;
                    char it[96];
                    std::snprintf(it, sizeof(it), "M%d %s", j + 1, g.labels[j].empty() ? cellTypeInfo(g.modes[j].type).name : g.labels[j].c_str());
                    if (ImGui::Selectable(it, j == cm)) m.childMode[c] = (uint8_t)j;
                }
                if (mobile_) ImGui::PopStyleVar();
                ImGui::EndPopup();
            }
            ImGui::SetCursorScreenPos({p.x, p.y + h + 4});
            ImGui::Dummy({aw, 0});
            ImGui::PopID();
        }
    } else {
        note("Клетка больше не делится — это конечная клетка организма.");
    }
    ImGui::Dummy({0, 8});
    if (ImGui::Button("Полный режим (программы, сигналы, тонкая настройка)", {aw, 0})) {
        cfg_.editorSimple = false;
        markConfigDirty();
    }
}

// A dismissible card explaining the editor in three steps.
void App::drawEditorTips() {
    if (!cfg_.editorTips) return;
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y - 130 * uiScale_}, ImGuiCond_Always, {0.5f, 1.0f});
    ImGui::SetNextWindowBgAlpha(0.93f);
    ImGui::Begin("##tips", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PushFont(fontBold_);
    ImGui::TextUnformatted("Как собрать организм");
    ImGui::PopFont();
    ImGui::TextUnformatted("1. Нажмите на клетку в центре — слева внизу появится панель «Клетка».");
    ImGui::TextUnformatted("2. Выберите, какой будет новая клетка, и нажмите «Разделить».");
    ImGui::TextUnformatted("3. Потяните полупрозрачного «призрака», чтобы повернуть деление. Шкала внизу — время развития.");
    ImGui::TextDisabled("Справа — настройки выбранной моды. Мода — это «профессия» клетки: все клетки одной моды одинаковы.");
    if (ImGui::Button("Понятно")) {
        cfg_.editorTips = false;
        markConfigDirty();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(вернуть: кнопка «?» на панели генома)");
    ImGui::End();
}

void App::drawModeEditor(Mode& m) {
    if (cfg_.editorSimple) {
        drawModeEditorSimple(m);
        return;
    }
    const Genome& g = ui_.editGenome;
    bool snap = ui_.snap;
    auto header = [&](const char* name, bool openByDefault) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        bool open = ImGui::CollapsingHeader(name, openByDefault ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleColor();
        return open;
    };

    if (header("Клетка", true)) {
        CellType t = m.type;
        rowLabel("Тип");
        if (typeCombo("type", &t)) m.type = t;
        ImGui::PushFont(fontSmall_);
        ImGui::TextWrapped("%s", cellTypeInfo(m.type).summary);
        ImGui::TextColored(ImVec4(0.75f, 0.70f, 0.60f, 1.0f), "%s", cellTypeQuip(m.type));
        ImGui::PopFont();
        rowLabel("Цвет");
        colorEditor("##col", &m.color, mobile_);
    }

    if (header("Деление", true)) {
        int k;
        if (m.splitMass <= kSplitAtAge) k = 0;
        else if (m.splitMass >= kNeverSplit) k = 24;
        else k = 1 + (int)std::lround((m.splitMass - kSplitMassMin) / kSplitMassStep);
        char fmt[48];
        if (k == 0) std::snprintf(fmt, sizeof(fmt), "в возрасте 0.5 ч");
        else if (k == 24) std::snprintf(fmt, sizeof(fmt), "никогда");
        else std::snprintf(fmt, sizeof(fmt), "%.3f нг", kSplitMassMin + (k - 1) * kSplitMassStep);
        rowLabel("Масса деления");
        if (ImGui::SliderInt("##sm", &k, 0, 24, fmt))
            m.splitMass = k == 0 ? kSplitAtAge : (k == 24 ? kNeverSplit : kSplitMassMin + (k - 1) * kSplitMassStep);
        char rfmt[32];
        std::snprintf(rfmt, sizeof(rfmt), "%.2f : %.2f", m.splitRatio, 1.0f - m.splitRatio);
        sliderF("Соотношение", &m.splitRatio, 0.1f, 0.9f, rfmt, 1.0f / 30.0f, snap);
        sliderAngle("Угол деления", &m.splitAngle, snap);
        checkbox("Создать адгезин", &m.makeAdhesin);
        rowLabel("Макс. связей");
        ImGui::SliderInt("##maxl", &m.maxConnections, 1, kMaxLinks);
    }

    if (header("Потомки", true)) {
        for (int c = 0; c < 2; ++c) {
            ImGui::PushID(c);
            ImGui::TextDisabled(c == 0 ? "Потомок 1 (по направлению деления)" : "Потомок 2 (напротив)");
            modeCombo("  Мода", &m.childMode[c], g);
            sliderAngle("  Угол", &m.childAngle[c], snap);
            checkbox("  Сохранить адгезины", &m.keepAdhesin[c]);
            checkbox("  Зеркально", &m.mirror[c]);
            ImGui::PopID();
        }
    }

    if (header("Питание", false)) {
        int ki = (int)std::round(std::log10(m.nutrientPriority) * 12.0f);
        char fmt[32];
        std::snprintf(fmt, sizeof(fmt), "%.2f", std::pow(10.0f, ki / 12.0f));
        rowLabel("Приоритет");
        if (ImGui::SliderInt("##prio", &ki, -12, 12, fmt)) m.nutrientPriority = std::pow(10.0f, ki / 12.0f);
        checkbox("Повышать при голоде", &m.prioritize);
    }

    if (header("Адгезин и цитоскелет", false)) {
        sliderF("Жёсткость", &m.adhesinStiffness, 0.0f, 1.0f, "%.2f", 0.05f, snap);
        sliderF("Длина адгезина", &m.adhesinLength, 0.0f, 1.0f, "%.2f", 0.05f, snap);
        sliderF("Цитоскелет", &m.cytoskeleton, 0.5f, 2.0f, "%.3f", 0.0625f, snap);
    }

    // Type-specific settings
    switch (m.type) {
        case CellType::Flagellocyte:
            if (header("Флагеллоцит", true)) progEditor("Сила плавания", m.prog[PF_Swim], PF_Swim, snap);
            break;
        case CellType::Buoyocyte:
            if (header("Буйоцит", true)) progEditor("Плотность", m.prog[PF_Density], PF_Density, snap);
            break;
        case CellType::Myocyte:
            if (header("Миоцит", true)) {
                progEditor("Сокращение", m.prog[PF_Contraction], PF_Contraction, snap);
                progEditor("Изгиб", m.prog[PF_Bend], PF_Bend, snap);
                progEditor("Подъём", m.prog[PF_Lift], PF_Lift, snap);
            }
            break;
        case CellType::Ciliocyte:
            if (header("Цилиоцит", true)) {
                progEditor("Левые реснички", m.prog[PF_Bend], PF_Bend, snap);
                progEditor("Правые реснички", m.prog[PF_Lift], PF_Lift, snap);
            }
            break;
        case CellType::Stemocyte:
            if (header("Стемоцит", true))
                for (int k = 0; k < 2; ++k) {
                    ImGui::PushID(k);
                    modeCombo(k == 0 ? "Путь 1: мода" : "Путь 2: мода", &m.stemMode[k], g);
                    progEditor(k == 0 ? "Сигнал пути 1 (> 0.5)" : "Сигнал пути 2 (> 0.5)", m.prog[PF_Stem1 + k], PF_Stem1 + k, snap);
                    ImGui::PopID();
                }
            break;
        case CellType::Neurocyte:
            if (header("Нейроцит", true))
                for (int k = 0; k < 4; ++k) {
                    ImGui::PushID(k);
                    char lbl[32];
                    std::snprintf(lbl, sizeof(lbl), "Выход %d → канал", k + 1);
                    rowLabel(lbl);
                    int ch = m.neuroChannel[k];
                    if (ImGui::SliderInt("##ch", &ch, 0, kSignalCount - 1, signalName(ch))) m.neuroChannel[k] = (uint8_t)ch;
                    progEditor("Производство", m.prog[PF_Neuro1 + k], PF_Neuro1 + k, snap);
                    ImGui::PopID();
                }
            break;
        case CellType::Senseocyte:
        case CellType::Stereocyte:
            if (header(m.type == CellType::Senseocyte ? "Сенсоцит" : "Стереоцит", true)) {
                rowLabel("Ощущает");
                if (ImGui::BeginCombo("##st", senseTargetName(m.senseTarget))) {
                    for (int i = 0; i < (int)SenseTarget::Count; ++i)
                        if (ImGui::Selectable(senseTargetName((SenseTarget)i), (int)m.senseTarget == i)) m.senseTarget = (SenseTarget)i;
                    ImGui::EndCombo();
                }
                rowLabel("Канал");
                int ch = m.senseChannel;
                if (ImGui::SliderInt("##sc", &ch, 0, kSignalCount - 1, signalName(ch))) m.senseChannel = (uint8_t)ch;
                sliderF("Выход", &m.senseOutput, -60.0f, 60.0f, "%.0f", 5.0f, snap);
                if (m.senseTarget == SenseTarget::Cell) {
                    rowLabel("Цвет цели");
                    colorEditor("##sc3", &m.senseColor, mobile_);
                    sliderF("Порог цвета", &m.senseThreshold, 0.0f, 1.7320508f, "%.3f", 1.7320508f / 24.0f, snap);
                }
            }
            break;
        case CellType::Secrocyte:
            if (header("Секроцит", true)) {
                rowLabel("Выделение");
                if (ImGui::BeginCombo("##sec", secretionName(m.secretion))) {
                    for (int i = 0; i < (int)Secretion::Count; ++i)
                        if (ImGui::Selectable(secretionName((Secretion)i), (int)m.secretion == i)) m.secretion = (Secretion)i;
                    ImGui::EndCombo();
                }
            }
            break;
        case CellType::Virocyte:
            if (header("Вироцит", true)) {
                modeCombo("Копировать моду", &m.virusCopyFrom, g);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Копируются эта мода, её дочерние моды и их дочерние моды — в те же номера генома жертвы.");
                ImGui::PopFont();
            }
            break;
        case CellType::Electrocyte:
            if (header("Электроцит", true)) {
                progEditor("Разряд (> 0.5)", m.prog[PF_Swim], PF_Swim, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Заряжается за 0.4 ч. Разряд — 0.5 В на ~40 мкм; клетки своего организма изолированы. "
                                   "Несколько электроцитов, разряжаясь вместе, оглушают (возбуждение > 15 мВ) или убивают "
                                   "(электропорация > 0.25 В). Пример: вход S1, функция x < c ? a : b, c = 5, a = 0, b = 1.");
                ImGui::PopFont();
            }
            break;
        case CellType::Cnidocyte:
            if (header("Книдоцит", true)) {
                progEditor("Готовность (≥ 0.3)", m.prog[PF_Swim], PF_Swim, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Взведённая клетка стреляет нитью в первую коснувшуюся чужую клетку: повреждение 35%%, "
                                   "паралич на 24 мин, вытекает сок. Новая нить сворачивается за час. Кератин защищает.");
                ImGui::PopFont();
            }
            break;
        case CellType::Luminocyte:
            if (header("Люминоцит", true)) {
                rowLabel("Цвет свечения");
                colorEditor("##glow", &m.senseColor, mobile_);
                progEditor("Яркость (0..1)", m.prog[PF_Swim], PF_Swim, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Стереоциты и сенсоциты «на свет» других организмов видят огонёк как яркое пятно и "
                                   "плывут к нему. Свечение стоит 0.3 расхода фагоцита при полной яркости. "
                                   "Пример ночника: вход «Свет», x < 0.2 ? 1 : 0 (ступенька, c = 0.2, a = 1, b = 0).");
                ImGui::PopFont();
            }
            break;
        case CellType::Cystocyte:
            if (header("Цистоцит", true)) {
                modeCombo("Проснуться в моду", &m.stemMode[0], g);
                progEditor("Пробуждение (> 0.5)", m.prog[PF_Stem1], PF_Stem1, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Циста не делится, не ест, не стареет и не боится голода по азоту; броня как у "
                                   "кератиноцита (только у себя). Расход — 1/250 фагоцита. Пример: вход «Еда рядом», "
                                   "x < 0.3 ? 0 : 1 — проснуться, когда рядом появится пища.");
                ImGui::PopFont();
            }
            break;
        case CellType::Grappler:
            if (header("Хватоцит", true)) {
                progEditor("Хватка (≥ 0.3 — держать)", m.prog[PF_Swim], PF_Swim, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Хватка ≥ 0.3: клетка цепляет адгезином любую чужую клетку, которой касается (кроме "
                                   "кератиноцитов), — до трёх сразу. Хватка ≥ 0.8 держит ещё и стенку чашки. Меньше 0.3 — "
                                   "отпускает. Удар током тоже разжимает хватку. Пример пиявки: вход «Масса», x < 0.75 ? 0.5 : 0 — "
                                   "держит, пока не наестся.");
                ImGui::PopFont();
            }
            break;
        case CellType::Chromatocyte:
            if (header("Хроматоцит", true)) {
                rowLabel("Цвет пигмента");
                colorEditor("##pig", &m.senseColor, mobile_);
                progEditor("Пигмент (0 — свой цвет, 1 — пигмент)", m.prog[PF_Swim], PF_Swim, snap);
                ImGui::PushFont(fontSmall_);
                ImGui::TextWrapped("Глаза других клеток видят текущий цвет: так можно прятаться от хищников или подавать сигналы.");
                ImGui::PopFont();
            }
            break;
        case CellType::Gamete:
            if (header("Гамета", true)) {
                modeCombo("Совместима с", &m.gameteCompatibleMode, g);
                modeCombo("Мода после слияния", &m.gameteModeAfter, g);
                rowLabel("Теломеры");
                const char* names[] = {"∞", "5", "10", "20", "40", "80"};
                int t = m.gameteTelomeres;
                if (ImGui::SliderInt("##tel", &t, 0, 5, names[clampv(t, 0, 5)])) m.gameteTelomeres = (uint8_t)t;
            }
            break;
        default: break;
    }
    ImGui::Dummy({0, 8});
}

// ---------------------------------------------------------------------------- bottom: timeline
void App::drawTimeline() {
    ImGuiIO& io = ImGui::GetIO();
    float w = std::min(760.0f * uiScale_, io.DisplaySize.x - 2 * (kListW + 24) * uiScale_);
    w = std::max(w, 420.0f * uiScale_);
    if (beginPanel("Развитие###timeline", &cfg_.winTimeline, {io.DisplaySize.x * 0.5f, io.DisplaySize.y - 14 * uiScale_}, {w, 0},
                   {0.5f, 1.0f}))
        drawTimelineBody(false);
    ImGui::End();
}

// Developmental time of the preview organism: stage readout, jumps and the tick slider. `compact`: the phone bar.
void App::drawTimelineBody(bool compact) {
    const int maxTick = editorMaxTick();
    const int stages = (int)layout_.stages.size() - 1;
    int st = editorStage();
    int cells = preview_->cellCount();
    int ghosts = 0;
    for (const PreviewRef& r : previewMap_) ghosts += r.phantom;
    int tick = editorTick_;
    const ImGuiStyle& sty = ImGui::GetStyle();
    ImGui::PushFont(fontSmall_);
    if (compact && portrait_) {  // phone, portrait: readout and buttons centred on their own lines
        char rd[96];
        std::snprintf(rd, sizeof(rd), "Стадия %d / %d · клеток %d", st, stages, cells - ghosts);
        ImGui::SetCursorPosX(std::max(sty.WindowPadding.x, (ImGui::GetWindowWidth() - ImGui::CalcTextSize(rd).x) * 0.5f));
        ImGui::TextDisabled("%s", rd);
    } else {
        ImGui::TextDisabled("Тик: %d    Стадия: %d / %d    Клеток: %d", editorTick_, st, stages, cells - ghosts);
    }
    ImGui::PopFont();
    {
        const char* labels[] = {compact ? "Вписать" : "Вписать (Home)", "Начало", "Взрослый"};
        if (!compact) ImGui::PushFont(fontSmall_);
        float total = 0;
        for (const char* l : labels) total += ImGui::CalcTextSize(l).x + 2 * sty.FramePadding.x + sty.ItemSpacing.x;
        if (compact && portrait_) ImGui::SetCursorPosX(std::max(sty.WindowPadding.x, (ImGui::GetWindowWidth() - total + sty.ItemSpacing.x) * 0.5f));
        else ImGui::SameLine(std::max(ImGui::GetCursorPosX(), (ImGui::GetWindowWidth() - sty.WindowPadding.x) - total + sty.ItemSpacing.x));
        auto btn = [&](const char* l) { return compact ? ImGui::Button(l) : ImGui::SmallButton(l); };
        if (btn(labels[0])) previewAutoFrame_ = true;
        ImGui::SameLine();
        if (btn(labels[1])) tick = 0;
        ImGui::SameLine();
        if (btn(labels[2])) tick = maxTick;
        if (!compact) ImGui::PopFont();
    }
    float bs = ImGui::GetFrameHeight() * (compact ? 1.0f : 1.25f);
    if (ImGui::Button("<<", {bs, 0})) tick = std::max(0, ((tick - 1) / kTicksPerStage) * kTicksPerStage);
    ImGui::SameLine();
    if (ImGui::Button("<", {bs, 0}) || (ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] > 0.4f)) tick = std::max(0, tick - 1);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 2 * bs - 2 * ImGui::GetStyle().ItemSpacing.x);
    // Stage boundaries as tick marks under the slider.
    ImVec2 sp = ImGui::GetCursorScreenPos();
    float sw = ImGui::CalcItemWidth();
    ImGui::SliderInt("##tick", &tick, 0, std::max(1, maxTick), "");
    if (maxTick > 0) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float y = sp.y + ImGui::GetFrameHeight() + 2;
        for (int s = 0; s <= stages; ++s) {
            float x = sp.x + 6 + (sw - 12) * (float)(s * kTicksPerStage) / maxTick;
            dl->AddLine({x, y}, {x, y + 4 * uiScale_}, s == st ? kAccentU32 : IM_COL32(150, 150, 160, 160), 1.5f);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(">", {bs, 0}) || (ImGui::IsItemActive() && ImGui::GetIO().MouseDownDuration[0] > 0.4f)) tick = std::min(maxTick, tick + 1);
    ImGui::SameLine();
    if (ImGui::Button(">>", {bs, 0})) tick = std::min(maxTick, (tick / kTicksPerStage + 1) * kTicksPerStage);
    if (tick != editorTick_) {
        editorTick_ = tick;
        if (selPhantom_) selCell_ = -1;
    }
    ImGui::Dummy({0, 2});
}

// ---------------------------------------------------------------------------- handles + action popup
void App::drawEditorHandles() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const bool classic = cfg_.theme != 0;
    const ImU32 ink = classic ? IM_COL32(35, 35, 55, 230) : IM_COL32(235, 238, 242, 230);
    int st = editorStage();
    bool atBoundary = editorTick_ % kTicksPerStage == 0 || st == (int)layout_.stages.size() - 1;

    // Hover hint
    if (!io.WantCaptureMouse && !mobile_ && !(io.ConfigFlags & ImGuiConfigFlags_NoMouse)) {
        bool ph = false;
        int h = pickPreview({io.MousePos.x, io.MousePos.y}, &ph);
        if (h >= 0) {
            const LayoutStage& sv = layout_.stages[std::min(ph ? st + 1 : (atBoundary ? st : st + 1), (int)layout_.stages.size() - 1)];
            if (h < (int)sv.cells.size()) {
                const LayoutCell& c = sv.cells[h];
                ImGui::SetTooltip("M%d · %s%s", c.mode + 1, cellTypeInfo(ui_.editGenome.modes[c.mode].type).name,
                                  ph ? "\nпризрак: перетащите, чтобы изменить направление деления" : "\nЛКМ — выбрать (действия — в панели «Клетка») · ПКМ — повторить последнее действие");
            }
        }
    }

    // Split guides for the selected cell: dashed split plane + numbered child arrows (as the original preview).
    if (selCell_ >= 0 && !selPhantom_ && atBoundary && st < (int)layout_.stages.size() && selCell_ < (int)layout_.stages[st].cells.size()) {
        const LayoutCell& c = layout_.stages[st].cells[selCell_];
        const Mode& m = ui_.editGenome.modes[c.mode];
        float r = radiusForMass(c.mass, CellType::Phagocyte);
        Vec2 cs = previewToScreen(c.pos);
        float rs = r * previewZoom_ / ((float)ctx_.extent().width / std::max(io.DisplaySize.x, 1.0f));
        // Cell front arrow.
        Vec2 f = fromAngle(c.angle);
        ImVec2 tip(cs.x + f.x * rs * 1.45f, cs.y - f.y * rs * 1.45f);
        dl->AddLine({cs.x, cs.y}, tip, ink, 2.0f);
        Vec2 pl = perp(f);
        dl->AddTriangleFilled(tip, {tip.x - f.x * 8 + pl.x * 5, tip.y + f.y * 8 - pl.y * 5}, {tip.x - f.x * 8 - pl.x * 5, tip.y + f.y * 8 + pl.y * 5}, ink);
        if (divides(m)) {
            float sgn = c.mirrored ? -1.0f : 1.0f;
            Vec2 d = fromAngle(c.angle + sgn * m.splitAngle);
            Vec2 pp = perp(d);
            // Dashed split plane
            for (int k = -6; k < 6; ++k) {
                float a0 = (k + 0.15f) / 6.0f * rs * 1.5f, a1 = (k + 0.65f) / 6.0f * rs * 1.5f;
                dl->AddLine({cs.x + pp.x * a0, cs.y - pp.y * a0}, {cs.x + pp.x * a1, cs.y - pp.y * a1}, IM_COL32(255, 158, 51, 255), 2.0f);
            }
            for (int k = 0; k < 2; ++k) {
                Vec2 dd = k == 0 ? d : -d;
                ImVec2 lp(cs.x + dd.x * rs * 0.9f, cs.y - dd.y * rs * 0.9f);
                dl->AddCircleFilled(lp, 9 * uiScale_, IM_COL32(255, 158, 51, 230));
                const char* n = k == 0 ? "1" : "2";
                ImVec2 ts = ImGui::CalcTextSize(n);
                dl->AddText({lp.x - ts.x * 0.5f, lp.y - ts.y * 0.5f}, IM_COL32(30, 20, 10, 255), n);
            }
        }
    }

    // Action popup (the reference editor's "Divide / Mutate" dialog). The phone shows it in a dock tab instead.
    if (mobile_ || !editorPopup_ || selCell_ < 0) return;
    int stIdx = selPhantom_ ? st + 1 : st;
    if (stIdx >= (int)layout_.stages.size() || selCell_ >= (int)layout_.stages[stIdx].cells.size()) {
        editorPopup_ = false;
        return;
    }
    const LayoutCell cell = layout_.stages[stIdx].cells[selCell_];
    // A docked panel (not a popup at the cursor): selecting a cell never covers the organism, and the panel stays put
    // while you work with it.
    float pw = 340 * uiScale_;
    ImGui::SetNextWindowPos({(kListW + 24) * uiScale_, io.DisplaySize.y - 12 * uiScale_}, resetLayout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver,
                            {0.0f, 1.0f});
    ImGui::SetNextWindowSize({pw, 0});
    ImGui::SetNextWindowBgAlpha(0.97f);
    bool open = true;
    char title[96];
    if (selPhantom_) std::snprintf(title, sizeof(title), "Потомок %d → M%d###cellpop2", cell.childSlot + 1, cell.mode + 1);
    else std::snprintf(title, sizeof(title), "Клетка · M%d###cellpop2", cell.mode + 1);
    ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);
    drawCellActionsBody();
    ImGui::End();
    if (!open) {
        editorPopup_ = false;
        selCell_ = -1;
    }
}

// Divide / change type / colour of the cell selected in the genome editor (or of the phantom child).
bool App::drawCellActionsBody(bool quick) {
    if (!editorPopup_ || selCell_ < 0 || layout_.stages.empty()) return false;
    const int st = editorStage();
    int stIdx = selPhantom_ ? st + 1 : st;
    if (stIdx >= (int)layout_.stages.size() || selCell_ >= (int)layout_.stages[stIdx].cells.size()) {
        editorPopup_ = false;
        return false;
    }
    const LayoutCell cell = layout_.stages[stIdx].cells[selCell_];
    Genome& g = ui_.editGenome;
    if (mobile_) {
        ImGui::PushFont(fontBold_);
        if (selPhantom_) ImGui::Text("Потомок %d → M%d", cell.childSlot + 1, cell.mode + 1);
        else ImGui::Text("Клетка · M%d", cell.mode + 1);
        ImGui::PopFont();
    }
    float aw = ImGui::GetContentRegionAvail().x;
    if (selPhantom_) {
        const LayoutCell& par = layout_.stages[st].cells[cell.parent];
        Mode& child = g.modes.edit(cell.mode);
        ImGui::TextDisabled("Рождается при делении M%d", par.mode + 1);
        ImGui::TextUnformatted("Тип потомка");
        CellType t = child.type;
        if (typePicker("ptype", &t, aw)) {
            child.type = t;
            child.color = {cellTypeInfo(t).r, cellTypeInfo(t).g, cellTypeInfo(t).b};
            lastAction_.kind = ActionKind::Mutate;
            lastAction_.type = t;
            lastAction_.color = child.color;
        }
        rowLabel("Цвет");
        colorEditor("##pc", &child.color, mobile_);
        ImGui::PushFont(fontSmall_);
        ImGui::TextWrapped("Перетащите призрак вокруг родителя, чтобы изменить направление деления.");
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.22f, 0.55f));
        if (ImGui::Button("Удалить деление", {aw, 0})) {
            ui_.editMode = par.mode;
            stopSplitSelected();
        }
        ImGui::PopStyleColor();
    } else {
        Mode& m = g.modes.edit(cell.mode);
        int shared = 0;
        for (const LayoutCell& o : layout_.stages[st].cells) shared += o.mode == cell.mode;
        colorSwatch(m.color, 16 * uiScale_);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(cellTypeInfo(m.type).name);
        if (shared > 1) {
            ImGui::SameLine();
            ImGui::TextDisabled("(мода у %d клеток)", shared);
        }
        if (!divides(m)) {
            sectionHeader("Разделить");
            ImGui::TextUnformatted("Новая клетка");
            if (lastAction_.kind == ActionKind::None) {
                lastAction_.type = m.type;
                lastAction_.color = m.color;
            }
            CellType nt = lastAction_.type;
            if (typePicker("ntype", &nt, aw)) {
                lastAction_.type = nt;
                lastAction_.color = {cellTypeInfo(nt).r, cellTypeInfo(nt).g, cellTypeInfo(nt).b};
            }
            rowLabel("Цвет");
            colorEditor("##nc", &lastAction_.color, mobile_);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.62f, 0.20f, 0.80f));
            if (ImGui::Button("Разделить", {aw, ImGui::GetFrameHeight() * 1.3f})) {
                lastAction_.keepType = true;
                splitSelected();
                lastAction_.kind = ActionKind::Divide;
                editorPopup_ = false;
                selCell_ = -1;
            }
            ImGui::PopStyleColor(2);
        } else {
            sectionHeader("Деление");
            ImGui::Text("Делится на M%d + M%d", m.childMode[0] + 1, m.childMode[1] + 1);
            float bw = (aw - 2 * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
            if (ImGui::Button("+15°", {bw, 0})) m.splitAngle = wrapAngle(m.splitAngle + deg2rad(15));
            ImGui::SameLine();
            if (ImGui::Button("-15°", {bw, 0})) m.splitAngle = wrapAngle(m.splitAngle - deg2rad(15));
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.25f, 0.22f, 0.55f));
            if (ImGui::Button("Убрать", {bw, 0})) {
                ui_.editMode = cell.mode;
                stopSplitSelected();
            }
            ImGui::PopStyleColor();
        }
        if (!editorPopup_) {
            return true;
        }
        if (quick) return true;
        sectionHeader("Изменить клетку");
        CellType t = m.type;
        if (typePicker("mtype", &t, aw)) {
            m.type = t;
            m.color = {cellTypeInfo(t).r, cellTypeInfo(t).g, cellTypeInfo(t).b};
            lastAction_.kind = ActionKind::Mutate;
            lastAction_.type = t;
            lastAction_.color = m.color;
        }
        rowLabel("Цвет");
        colorEditor("##mc", &m.color, mobile_);
        if (cell.mode != g.initialMode && ImGui::Button("Сделать начальной", {aw, 0})) g.initialMode = cell.mode;
        ImGui::PushFont(fontSmall_);
        ImGui::TextDisabled(mobile_ ? "Остальные параметры моды — на вкладке «Мода»." : "Остальные параметры моды — в панели справа.");
        ImGui::PopFont();
    }
    return true;
}
