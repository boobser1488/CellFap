// Phone / tablet interface (Android, or --mobile W H scale on the desktop for testing).
//
// The desktop's floating windows do not fit a phone: here every panel is docked. Microscope: the tools in a strip
// (bottom in portrait, left in landscape), the clock and the dish menu on top, and one sheet with tabs (tool options,
// cell, statistics, level) that folds into its tab strip. Genome editor: the genome bar on top, the timeline above
// the sheet, the sheet with the modes, the selected mode and the actions for the tapped cell.
// Lists scroll by dragging the content (touchScroll), with inertia.
#include "app.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#include "imgui_internal.h"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cmath>

using namespace uiw;

namespace {
constexpr float kNavH = 46.0f;  // as in ui.cpp
const ImGuiWindowFlags kDocked = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

// Button that keeps the accent colour while `on`.
// Divider on the inner edge of a docked panel (they have no frame).
void edgeLine(ImVec2 a, ImVec2 b) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRectFullScreen();
    dl->AddLine(a, b, IM_COL32(110, 190, 120, 70));
    dl->PopClipRect();
}

bool toggleButton(const char* label, bool on, ImVec2 size = {0, 0}) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.38f));
    bool r = ImGui::Button(label, size);
    if (on) ImGui::PopStyleColor();
    return r;
}

// Popup menu with finger-sized rows (a menu row is as tall as its text; the item spacing widens the tap area).
bool beginTouchMenu(const char* id, float u) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16 * u, 12 * u});
    bool open = ImGui::BeginPopup(id);
    ImGui::PopStyleVar();
    if (open) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10 * u, 19 * u});
    return open;
}
void endTouchMenu() {
    ImGui::PopStyleVar();
    ImGui::EndPopup();
}
}  // namespace

int App::fitColumns(float avail, float minColW, int maxCols) const {
    const float gap = 14 * uiScale_;
    return std::clamp((int)((avail + gap) / (minColW + gap)), 1, maxCols);
}

void App::applyOrientation() {
#ifdef __ANDROID__
    glfwAndroidSetOrientation(opt_.orient >= 0 ? opt_.orient : cfg_.orientation);
#endif
}

void App::exportToDevice(const std::string& path, const std::string& name) {
#ifdef __ANDROID__
    glfwAndroidExportFile(path.c_str(), name.c_str());
#else
    toast("Сохранено: " + path);
    (void)name;
#endif
}

// ---------------------------------------------------------------------------- touch scrolling
// A finger dragged over a scrollable window scrolls it. A press that landed on a button or a list row is cancelled
// once the finger clearly moves; sliders, text fields and widgets marked with keepTouchDrag() keep the finger.
void App::touchScroll() {
    ImGuiContext& g = *GImGui;
    ImGuiIO& io = g.IO;
    static ImGuiWindow* target = nullptr;
    static bool scrolling = false, owned = false;
    static ImVec2 vel{0, 0};
    // Sideways only where a window asks for it (HorizontalScrollbar): an item a little too wide must not let the
    // whole panel slide left and right under the finger.
    auto scrollsX = [](ImGuiWindow* w) { return w->ScrollMax.x > 0.5f && (w->Flags & ImGuiWindowFlags_HorizontalScrollbar); };
    auto scrollable = [&](ImGuiWindow* w) -> ImGuiWindow* {
        while (w) {
            if (w->ScrollMax.y > 0.5f || scrollsX(w)) return w;
            if (!(w->Flags & ImGuiWindowFlags_ChildWindow)) return nullptr;
            w = w->ParentWindow;
        }
        return nullptr;
    };
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        target = scrollable(g.HoveredWindow);
        scrolling = false;
        vel = {0, 0};
        owned = g.ActiveId != 0 &&
                (g.ActiveIdUsingNavDirMask != 0 || g.ActiveId == gNoTouchScrollId ||
                 (target && (g.ActiveId == ImGui::GetWindowScrollbarID(target, ImGuiAxis_Y) ||
                             g.ActiveId == ImGui::GetWindowScrollbarID(target, ImGuiAxis_X))));
    }
    if (target && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (!scrolling && !owned) {
            ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            const float th = io.MouseDragThreshold;
            bool vertical = target->ScrollMax.y > 0.5f && std::fabs(d.y) > th && std::fabs(d.y) > std::fabs(d.x);
            bool horizontal = scrollsX(target) && std::fabs(d.x) > th && std::fabs(d.x) > std::fabs(d.y);
            if (vertical || horizontal) {
                scrolling = true;
                if (g.ActiveId != 0) ImGui::ClearActiveID();  // it was a scroll, not a tap on that button
            }
        }
        if (scrolling) {
            ImVec2 md = io.MouseDelta;
            ImGui::SetScrollY(target, target->Scroll.y - md.y);
            if (scrollsX(target)) ImGui::SetScrollX(target, target->Scroll.x - md.x);
            float dt = std::max(io.DeltaTime, 1e-3f);
            vel = {vel.x * 0.6f + 0.4f * (-md.x / dt), vel.y * 0.6f + 0.4f * (-md.y / dt)};
        }
    } else if (target && scrolling) {
        // Released: glide on and slow down.
        float dt = io.DeltaTime;
        ImGui::SetScrollY(target, target->Scroll.y + vel.y * dt);
        if (scrollsX(target)) ImGui::SetScrollX(target, target->Scroll.x + vel.x * dt);
        float k = std::exp(-dt * 4.0f);
        vel = {vel.x * k, vel.y * k};
        if (std::fabs(vel.x) + std::fabs(vel.y) < 20.0f) scrolling = false;
    }
    gNoTouchScrollId = 0;
}

// ---------------------------------------------------------------------------- docked sheet
int App::beginDock(const char* id, Dock& d, const char* const* labels, int count, float bottomReserve, bool roomy,
                   float minTop) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    d.tab = std::clamp(d.tab, 0, count - 1);
    // The microscope's sheet is denser than the rest of the phone UI: it shares the screen with the dish. The genome
    // editor's sheet is where the work happens: finger-sized rows.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, roomy ? ImVec2(12 * u, 13 * u) : ImVec2(9 * u, 8 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, roomy ? ImVec2(8 * u, 9 * u) : ImVec2(8 * u, 6 * u));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {6 * u, 4 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, (roomy ? 30 : 22) * u);
    // Landscape with tab icons: folded, the sheet is a slim rail of them along the right edge (a strip of tabs hanging
    // at the top right over the dish looked stray); open, the column slides out over the rail and back in.
    const bool rail = !portrait_ && dockIcons_;
    if (rail) {
        if (d.anim < 0) d.anim = d.open ? 1.0f : 0.0f;
        const float target = d.open ? 1.0f : 0.0f;
        d.anim += (target - d.anim) * (1.0f - std::exp(-io.DeltaTime * 14.0f));
        if (std::fabs(d.anim - target) < 0.01f) d.anim = target;
    }
    auto drawRail = [&](const char* winName) {
        const float s = std::floor(44 * u), padR = 6 * u, right = std::ceil(safe_.z);
        const float railW = s + 2 * padR;
        ImVec2 pos{std::floor(io.DisplaySize.x - right - railW), top}, size{railW + right + 4, io.DisplaySize.y - top + 4};
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padR, padR});
        ImGui::Begin(winName, nullptr, kDocked | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);
        edgeLine(pos, {pos.x, pos.y + size.y});
        for (int i = 0; i < count; ++i) {
            ImGui::PushID(i);
            if (iconButton(labels[i], (Icon)dockIcons_[i], d.open && d.tab == i, labels[i], s)) {
                if (d.open && d.tab == i) d.open = false;
                else {
                    d.tab = i;
                    d.open = true;
                }
            }
            ImGui::PopID();
        }
        return std::pair<ImVec2, ImVec2>{pos, size};
    };
    if (rail && !d.open && d.anim <= 0.0f) {  // folded: only the rail (the window endDock() closes)
        auto [pos, size] = drawRail(id);
        dockPos_ = pos;
        dockSize_ = size;
        dockBodyOpen_ = false;
        return -1;
    }
    if (rail) {  // sliding or open: the rail stays underneath, the column slides over it
        drawRail("##dockrail");
        ImGui::End();
    }
    const float gripH = portrait_ ? (roomy ? 22 : 16) * u : 0;
    const float stripH = ImGui::GetFrameHeight() + 10 * u + gripH;
    const float room = io.DisplaySize.y - top - bottomReserve;  // height the sheet may take in portrait
    ImVec2 pos, size;
    if (portrait_) {
        float h = d.open ? std::round(std::max(stripH + 60 * u, room * d.frac)) : std::ceil(stripH);
        if (minTop > top) h = std::min(h, std::max(std::ceil(stripH), std::floor(io.DisplaySize.y - bottomReserve - minTop)));
        pos = {dockInsetL_, std::round(io.DisplaySize.y - bottomReserve - h)};
        size = {io.DisplaySize.x - dockInsetL_ - dockInsetR_, h};
    } else {
        float w = std::round(std::clamp(io.DisplaySize.x * d.wfrac, 200 * u, io.DisplaySize.x * 0.75f));
        float h = d.open || rail ? io.DisplaySize.y - top + 4 : std::ceil(stripH);  // open: reaches past the bottom edge
        // With the rail: the column stands to the left of it and slides out from under it (the rail stays on top).
        const float railW = rail ? std::floor(44 * u) + 12 * u : 0.0f;
        const float slide = rail ? std::floor(w * (1.0f - d.anim)) : 0.0f;
        pos = {std::floor(io.DisplaySize.x - std::ceil(safe_.z) - railW - w) + slide, top};  // clear of a right camera cutout
        size = {w, h};
    }
    dockPos_ = pos;
    dockSize_ = size;
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {portrait_ ? 6 * u : 14 * u, portrait_ ? 0 : 5 * u});  // landscape: room for the width handle
    ImGui::Begin(id, nullptr, kDocked | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (portrait_) edgeLine(pos, {pos.x + size.x, pos.y});
    else edgeLine(pos, {pos.x, pos.y + size.y});
    // Resizing in portrait follows the finger itself (kept at the same distance from the sheet's top edge as where it
    // grabbed), not per-frame deltas: grab the handle, or slide a tab up or down. It goes on until the finger lifts.
    static ImGuiID resizing = 0;
    static float grabDy = 0;
    const ImGuiID resizeId = ImGui::GetID(id);
    const float maxFrac = roomy ? 0.92f : 0.85f;
    auto follow = [&] {
        float h = (io.DisplaySize.y - bottomReserve) - (io.MousePos.y - grabDy);
        d.open = true;
        d.frac = std::clamp(h / room, 0.12f, maxFrac);
    };
    auto finish = [&] {
        resizing = 0;
        if (d.frac < 0.2f) {  // dragged nearly shut
            d.open = false;
            d.frac = roomy ? 0.62f : 0.36f;
        }
    };
    if (portrait_) {
        // The handle: a tap unfolds the sheet, then toggles between the usual and nearly full height. (Tapping the
        // active tab folds it.)
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##grip", {w, gripH});
        keepTouchDrag();
        if (ImGui::IsItemActivated()) grabDy = io.MousePos.y - pos.y;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) resizing = resizeId;
        if (ImGui::IsItemDeactivated() && resizing != resizeId && io.MouseDragMaxDistanceSqr[0] < 100.0f * u * u) {
            if (!d.open) d.open = true;
            else if (roomy) d.frac = d.frac < 0.75f ? maxFrac : 0.62f;
            else d.frac = d.frac < 0.6f ? 0.85f : 0.46f;
        }
        const bool active = ImGui::IsItemActive() || resizing == resizeId;
        const float hw = (roomy ? 40 : 22) * u, hh = (roomy ? 3.0f : 2) * u;
        const float cx = p.x + w * 0.5f, cy = p.y + gripH * 0.5f;
        const ImU32 col = active ? kAccentU32 : IM_COL32(255, 255, 255, roomy ? 150 : 90);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({cx - hw, cy - hh}, {cx + hw, cy + hh}, col, hh);
        if (roomy) {  // small arrows on both ends: it pulls up and down
            const float s = 5 * u;
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const float ax = cx + sgn * (hw + 14 * u);
                dl->AddTriangleFilled({ax - s, cy - 2 * u}, {ax + s, cy - 2 * u}, {ax, cy - 2 * u - s}, col);
                dl->AddTriangleFilled({ax + s, cy + 2 * u}, {ax - s, cy + 2 * u}, {ax, cy + 2 * u + s}, col);
            }
        }
    } else if (d.open) {
        // Handle on the left edge: drag sideways to make the side panel wider or narrower.
        ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##gripw", {(roomy ? 20 : 12) * u, size.y});
        keepTouchDrag();
        bool active = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            d.wfrac = std::clamp(d.wfrac - io.MouseDelta.x / io.DisplaySize.x, 0.22f, 0.75f);
        if (ImGui::IsItemDeactivated() && io.MouseDragMaxDistanceSqr[0] < 100.0f * u * u) d.wfrac = d.wfrac < 0.5f ? 0.6f : 0.36f;  // a tap: wide / narrow
        // A visible grip: a pill with small arrows both ways (it pulls wider and narrower).
        const float cy = pos.y + std::min(size.y, io.DisplaySize.y - pos.y) * 0.5f;
        const ImU32 gc = active ? kAccentU32 : IM_COL32(255, 255, 255, 140);
        ImDrawList* gdl = ImGui::GetWindowDrawList();
        const float gx = pos.x + 7 * u;
        gdl->AddRectFilled({gx - 2.5f * u, cy - 26 * u}, {gx + 2.5f * u, cy + 26 * u}, gc, 2.5f * u);
        const float ta = 4 * u;
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const float ay = cy + sgn * 36 * u;
            gdl->AddTriangleFilled({gx - ta, ay}, {gx, ay - ta}, {gx, ay + ta}, gc);
            gdl->AddTriangleFilled({gx + ta, ay}, {gx, ay + ta}, {gx, ay - ta}, gc);
        }
        ImGui::SetCursorScreenPos(back);
    }
    // Tab strip: equal buttons; the active one folds the sheet, another one opens it on that tab.
    const float gap = 4 * u;
    const float bw = (ImGui::GetContentRegionAvail().x - (count - 1) * gap) / count;
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, gap);
        bool small = ImGui::CalcTextSize(labels[i]).x + 2 * ImGui::GetStyle().FramePadding.x > bw;
        if (small) ImGui::PushFont(fontSmall_);
        ImGui::PushID(i);
        if (toggleButton(labels[i], d.open && d.tab == i, {bw, ImGui::GetFrameHeight()})) {
            if (d.open && d.tab == i) d.open = false;
            else {
                d.tab = i;
                d.open = true;
            }
        }
        // A tab slid up or down is a handle: the press turns into a resize (and is no longer a tap on the tab).
        if (portrait_ && ImGui::IsItemActive() && resizing == 0) {
            ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            if (std::fabs(dd.y) > 12 * u && std::fabs(dd.y) > std::fabs(dd.x)) {
                grabDy = io.MouseClickedPos[0].y - pos.y;
                resizing = resizeId;
                ImGui::ClearActiveID();
            }
        }
        ImGui::PopID();
        if (small) ImGui::PopFont();
    }
    if (resizing == resizeId) {  // until the finger lifts, wherever it is
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            follow();
            // Keep the finger captured by the interface, so the dish under it does not pan.
            ImGuiContext& g = *GImGui;
            if (g.ActiveId == 0 || g.ActiveId == resizeId) {
                ImGui::SetActiveID(resizeId, ImGui::GetCurrentWindow());
                ImGui::KeepAliveID(resizeId);
            }
        } else {
            if (GImGui->ActiveId == resizeId) ImGui::ClearActiveID();
            finish();
        }
    }
    dockBodyOpen_ = d.open;
    if (!d.open) return -1;
    ImGui::Separator();
    ImGui::BeginChild("##dockbody", {0, 0}, ImGuiChildFlags_None);
    gLabelW = std::min(130 * u, ImGui::GetContentRegionAvail().x * 0.38f);
    return d.tab;
}

void App::endDock() {
    if (dockBodyOpen_) ImGui::EndChild();
    ImGui::End();
    // The rail of tab icons (landscape) stays above the column sliding out from under it.
    // (Not over an open tool drawer's dimmed screen: that one is modal.)
    if (ImGuiWindow* r = ImGui::FindWindowByName("##dockrail"); r && r->WasActive && !(toolDrawerOpen_ && !portrait_))
        ImGui::BringWindowToDisplayFront(r);
    ImGui::PopStyleVar(4);
    gLabelW = 150 * uiScale_;
}

// ---------------------------------------------------------------------------- navigation
void App::drawMobileNav() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, h = kNavH * u, y0 = safe_.y;  // the strip under a top camera cutout is the bar's too
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({io.DisplaySize.x, y0 + h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.105f, 0.108f, 0.115f, 0.98f));
    ImGui::Begin("##mnav", nullptr, kDocked);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({0, y0 + h - 1}, {io.DisplaySize.x, y0 + h - 1}, IM_COL32(110, 190, 120, 70));
    // Same order as the desktop tabs (Screen enum). Bold when the names fit, else the regular font.
    const char* names[] = {"Субстрат", "Микроскоп", "Геном", "Настройки", "Задания"};
    const float maxTab = 150 * u;
    const float x0w = safe_.x, ww = io.DisplaySize.x - safe_.x - safe_.z;  // clear of side cutouts
    const float tabW = std::min(maxTab, ww / 5.0f);
    const float x0 = x0w + (ww - tabW * 5) * 0.5f;
    ImFont* font = fontBold_;
    for (const char* n : names)
        if (ImGui::CalcTextSize(n).x + 8 * u > tabW) font = fontRegular_;
    ImGui::PushFont(font);
    for (int i = 0; i < 5; ++i) {
        ImGui::SetCursorPos({x0 + i * tabW, y0});
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("tab", {tabW, h})) ui_.screen = (Screen)i;
        ImGui::PopID();
        bool active = (int)ui_.screen == i;
        ImVec2 ts = ImGui::CalcTextSize(names[i]);
        ImU32 col = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 172, 176, 255);
        dl->AddText({p.x + (tabW - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f - 1}, col, names[i]);
        if (active) dl->AddRectFilled({p.x + 10 * u, p.y + h - 3 * u}, {p.x + tabW - 10 * u, p.y + h}, kAccentU32, 1.5f * u);
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

// ---------------------------------------------------------------------------- microscope
void App::drawMobilePalette(float& reservedBottom, float& reservedLeft, float& reservedRight, float barH) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, pad = 6 * u, gap = 4 * u;
    // Portrait with the strip on a side: a column like in landscape, below the time bar (which spans the width).
    const bool side = portrait_ && cfg_.toolSide != 0, onRight = side && cfg_.toolSide == 2;
    const bool row = portrait_ && !side;
    const float top = navBottom() + (side ? barH : 0);
    struct T {
        Tool t;
        Icon ic;
        const char* tip;
    } tools[] = {
        {Tool::Inspect, IcInspect, "Диагностика: выбор клетки, сдвиг чашки"},
        {Tool::Synth, IcSynth, "Синтезатор: вставить клетку генома из редактора"},
        {Tool::Tweezers, IcTweezers, "Пинцет: тащить клетку"},
        {Tool::Boost, IcBoost, "Бустер: масса и азот; на пустом месте — пища"},
        {Tool::Remove, IcRemove, "Удаление"},
        {Tool::Shock, IcShock, "Электрод: импульс тока"},
        {Tool::GeneNeedle, IcGeneNeedle, "Генная игла: ввести геном из редактора"},
        {Tool::DrugNeedle, IcDrugNeedle, "Игла для веществ"},
        {Tool::Map, IcMap, "Редактор карты: стены"},
        {Tool::Adhesin, IcAdhesin, "Адгезин: связать, склеить, разрезать"},
    };
    const int n = (int)Tool::Count;
    // Landscape: the strip is a drawer by the left edge (next to a camera cutout, a fixed column took far more width
    // than its icons). Folded it is a slim rail with the tool in hand; it slides out over the dish.
    const bool drawer = !portrait_;
    const float railW = std::floor(28 * u);  // slim: the tool in hand and a "›"
    const int slots = n + (drawer ? 1 : 0);  // the open drawer ends with a "fold" button
    const float avail = row ? io.DisplaySize.x - 2 * pad : io.DisplaySize.y - top - 2 * pad - std::ceil(safe_.w);
    int lines = 1;
    float s = (avail - (slots - 1) * gap) / slots;
    if (s < 34 * u) {
        lines = 2;
        const int half = (slots + 1) / 2;
        s = (avail - (half - 1) * gap) / half;
    }
    s = std::floor(std::min(s, 58 * u));
    const int perLine = (slots + lines - 1) / lines;
    const float thick = std::ceil(lines * s + (lines - 1) * gap + 2 * pad);  // strip thickness
    const float along = perLine * s + (perLine - 1) * gap;                   // length of one line of buttons
    // The drawer slides: 0 = only the rail, 1 = all the way out. A finger pulling it moves it directly; let go, it
    // finishes the way it was going (out past a third, back in past a third of the way back).
    const float travel = thick + std::ceil(safe_.x) + 4;
    if (!toolDrawerDrag_) {
        toolDrawerT_ += ((toolDrawerOpen_ ? 1.0f : 0.0f) - toolDrawerT_) * (1.0f - std::exp(-io.DeltaTime * 14.0f));
        if (std::fabs(toolDrawerT_ - (toolDrawerOpen_ ? 1.0f : 0.0f)) < 0.01f) toolDrawerT_ = toolDrawerOpen_ ? 1.0f : 0.0f;
    }
    if (drawer) {
        // Pulling an open drawer back: from anywhere on it or on the dimmed screen (a tap there still just folds it).
        if (toolDrawerOpen_ && !toolDrawerDrag_ && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x < -8 * u &&
            io.MouseClickedPos[0].x < io.DisplaySize.x * 0.5f) {
            toolDrawerDrag_ = true;
            toolDrawerDragFrom_ = toolDrawerT_;
            ImGui::ClearActiveID();  // no tool picked, no tap on the dimmed screen when the finger lets go
        }
        if (toolDrawerDrag_ && toolDrawerOpen_) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                toolDrawerT_ = std::clamp(toolDrawerDragFrom_ + ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x / travel, 0.0f, 1.0f);
            else {
                toolDrawerOpen_ = toolDrawerT_ > 0.67f;
                toolDrawerDrag_ = false;
            }
        }
    }
    if (drawer) {  // the rail is always there; the strip slides over it
        const float left = std::ceil(safe_.x);
        ImVec2 pos{-4, top}, size{left + railW + 4, io.DisplaySize.y - top + 4};
        reservedLeft = left + railW;
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        // Never raised by a tap: the drawer slides out over it (a raised rail hid the drawer's icons and took their taps).
        ImGui::Begin("##mrail", nullptr,
                     kDocked | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar(2);
        // The whole rail is the handle: a tap opens the drawer, a pull drags it out under the finger.
        ImGui::SetCursorScreenPos({left, top});
        ImGui::InvisibleButton("##opentools", {railW, size.y - 4});
        keepTouchDrag();
        if (ImGui::IsItemActive() && !toolDrawerOpen_) {
            const float dx = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f).x;
            if (!toolDrawerDrag_ && dx > 6 * u) {
                toolDrawerDrag_ = true;
                toolDrawerDragFrom_ = toolDrawerT_;
            }
            if (toolDrawerDrag_) toolDrawerT_ = std::clamp(toolDrawerDragFrom_ + dx / travel, 0.0f, 1.0f);
        }
        if (ImGui::IsItemDeactivated() && !toolDrawerOpen_) {
            toolDrawerOpen_ = toolDrawerDrag_ ? toolDrawerT_ > 0.33f : true;  // a pull past a third, or a tap
            toolDrawerDrag_ = false;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Centred on the whole dark strip from the screen edge (the camera cutout's margin looks like part of it), as
        // large as it allows, and clear of the camera hole (in landscape it sits at the middle of this edge).
        const float stripW = left + railW;
        const float bottomY = io.DisplaySize.y - std::ceil(safe_.w);
        const float b = std::floor(std::min(stripW - 4 * u, 40 * u)), a = 5 * u;
        const float groupH = b + 4 * a;  // the icon, a gap and the "›" under it
        const float cx = stripW * 0.5f;
        float gy = top + (bottomY - top - groupH) * 0.5f;  // top of the group
        for (const ImVec4& cut : cutout_) {
            if (cut.x >= stripW) continue;  // a cutout elsewhere
            const float m = 6 * u;
            if (gy < cut.w + m && gy + groupH > cut.y - m) {
                const float below = cut.w + m, above = cut.y - m - groupH;
                gy = below + groupH <= bottomY ? below : std::max(top + m, above);
            }
        }
        int ti = 0;
        for (int i = 0; i < n; ++i)
            if (tools[i].t == ui_.tool) ti = i;
        // The tool in hand, and under it a "›" — pull this way.
        const float iy = gy + b * 0.5f;
        const int fade = (int)(255 * (1.0f - toolDrawerT_));
        dl->AddRectFilled({cx - b * 0.5f, iy - b * 0.5f}, {cx + b * 0.5f, iy + b * 0.5f}, IM_COL32(255, 158, 51, 60 * fade / 255), 6 * u);
        drawIcon(dl, tools[ti].ic, {cx, iy}, b * 0.8f, (kAccentU32 & 0x00FFFFFFu) | ((ImU32)fade << 24));
        const float ay = gy + b + 2.5f * a;
#ifdef __ANDROID__
        // A pull from the very edge here is the game's, not the system's "back" swipe (at most 200 dp per edge).
        {
            const float half = 95.0f / 0.9f * u;  // the interface scale is 0.9 x the screen density
            const float gc = gy + groupH * 0.5f;
            glfwAndroidSetGestureExclusion(0, (int)std::max(top, gc - half), (int)(stripW + 8 * u), (int)std::min(bottomY, gc + half));
            railExclusion_ = true;
        }
#endif
        dl->AddTriangleFilled({cx - a * 0.5f, ay - a}, {cx - a * 0.5f, ay + a}, {cx + a * 0.6f, ay}, IM_COL32(255, 255, 255, 170 * fade / 255));
        edgeLine({left + railW - 1, top}, {left + railW - 1, io.DisplaySize.y});
        ImGui::End();
        if (!toolDrawerOpen_ && toolDrawerT_ <= 0.0f) return;  // folded: only the rail
    }
    // Whole pixels, and the window reaches a few pixels past the screen edge: a fractional edge left a one-pixel
    // row of the dish showing along the bottom of the screen.
    ImVec2 pos, size, origin;
    float off;
    if (row) {
        pos = {-4, std::floor(io.DisplaySize.y - thick - std::ceil(safe_.w))};
        size = {io.DisplaySize.x + 8, io.DisplaySize.y - pos.y + 4};
        reservedBottom = io.DisplaySize.y - pos.y;
        origin = {pad, pos.y + pad};
        off = std::max(0.0f, (io.DisplaySize.x - 2 * pad - along) * 0.5f);
    } else if (onRight) {
        const float right = std::ceil(safe_.z);  // a camera cutout on the right edge
        pos = {std::floor(io.DisplaySize.x - right - thick), top};
        size = {thick + right + 4, io.DisplaySize.y - top + 4};
        reservedRight = io.DisplaySize.x - pos.x;
        origin = {pos.x + pad, top + pad};
        off = std::max(0.0f, (io.DisplaySize.y - top - 2 * pad - std::ceil(safe_.w) - along) * 0.5f);
    } else {
        const float left = std::ceil(safe_.x);  // a camera cutout on the left edge
        // A drawer slides in from the left edge (and out again).
        const float slide = drawer ? -std::floor((thick + left + 4) * (1.0f - toolDrawerT_)) : 0.0f;
        pos = {-4 + slide, top};
        size = {thick + left + 4, io.DisplaySize.y - top + 4};
        reservedLeft = drawer ? left + railW : thick + left;  // the open drawer lies over the dish
        origin = {left + pad + slide, top + pad};
        off = std::max(0.0f, (io.DisplaySize.y - top - 2 * pad - along) * 0.5f);
    }
    // The open drawer is modal like a phone's navigation drawer: the rest of the screen dims, and a tap there only
    // folds the drawer (it used to fall through and use the tool on the dish as well).
    if (drawer && toolDrawerOpen_) {
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##toolscrim", nullptr,
                     kDocked | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::GetWindowDrawList()->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(0, 0, 0, (int)(70 * toolDrawerT_)));
        ImGui::SetCursorScreenPos({0, 0});
        if (ImGui::InvisibleButton("##scrim", io.DisplaySize)) toolDrawerOpen_ = false;
        ImGui::End();
    }
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, gap});
    ImGui::Begin("##mpalette", nullptr, kDocked | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (drawer) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());  // over the rail and the dimmed screen
    for (int i = 0; i < n; ++i) {
        int line = i / perLine, k = i % perLine;
        ImVec2 p = row ? ImVec2(origin.x + off + k * (s + gap), origin.y + line * (s + gap))
                       : ImVec2(origin.x + line * (s + gap), origin.y + off + k * (s + gap));
        ImGui::SetCursorScreenPos(p);
        bool allowed = toolAllowed(tools[i].t);
        if (!allowed) ImGui::BeginDisabled();
        if (iconButton(tools[i].tip, tools[i].ic, ui_.tool == tools[i].t, allowed ? tools[i].tip : "Не в этом задании", s)) {
            if (ui_.tool == tools[i].t) {  // tapping the current tool again shows (or folds) its options
                if (dockMicro_.open && dockMicro_.tab == 0) dockMicro_.open = false;
                else {
                    dockMicro_.tab = 0;
                    dockMicro_.open = true;
                }
            }
            ui_.tool = tools[i].t;
            if (drawer) toolDrawerOpen_ = false;  // a tool picked: the drawer slides back, the rail shows it
        }
        if (!allowed) ImGui::EndDisabled();
    }
    if (drawer) {  // last slot: fold the drawer back to its rail
        const int line = n / perLine, k = n % perLine;
        ImVec2 p{origin.x + line * (s + gap), origin.y + off + k * (s + gap)};
        ImGui::SetCursorScreenPos(p);
        if (ImGui::InvisibleButton("##foldtools", {s, s})) toolDrawerOpen_ = false;
        const float cx = p.x + s * 0.5f, cy = p.y + s * 0.5f, a = s * 0.16f;
        ImGui::GetWindowDrawList()->AddTriangleFilled({cx + a * 0.5f, cy - a}, {cx + a * 0.5f, cy + a}, {cx - a * 0.6f, cy},
                                                      IM_COL32(255, 255, 255, ImGui::IsItemHovered() ? 230 : 160));
    }
    if (row) edgeLine({pos.x, pos.y}, {pos.x + size.x, pos.y});
    else if (onRight) edgeLine({pos.x, pos.y}, {pos.x, pos.y + size.y});
    else edgeLine({pos.x + size.x - 1, pos.y}, {pos.x + size.x - 1, pos.y + size.y});
    ImGui::End();
    ImGui::PopStyleVar(3);
}

float App::drawMobileTimeBar(float x, float y, float w) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    // Speed buttons, a step and the dish menu: six equal buttons. Portrait: spread evenly over a full-width bar.
    // Landscape: a compact pill centred over the dish, only as wide as its buttons (a full-width band ate the height).
    // Dragged by its status line it becomes a free pill left wherever the finger lets go (a double tap puts it back);
    // its size is set in the dish menu or the settings.
    const bool freeBar = cfg_.timeBarFree;
    const bool compact = !portrait_ || freeBar;
    const float sc = std::clamp(cfg_.timeBarScale, 0.6f, 1.5f);
    const float padX = 8 * u;
    const float s = compact ? std::floor(42 * u * sc) : std::floor(std::min(52 * u * sc, (w - 2 * padX) / 6.0f - 4 * u));
    const float gapB = compact ? std::floor(6 * u * sc) : std::floor(std::min(18 * u, (w - 2 * padX - 6 * s) / 5.0f));
    const float rowW = 6 * s + 5 * gapB;
    const float winW = compact ? std::min(freeBar ? io.DisplaySize.x : w, rowW + 2 * padX) : w;
    float winX = compact ? std::floor(x + (w - winW) * 0.5f) : x, winY = compact ? y + 4 * u : y;
    if (freeBar) {
        static float lastH = 60;  // its height (last frame): keeps it on the screen
        // It stays where the dish shows (last frame's area): not over the tool strip, the rails or the sheet.
        const bool known = dishAreaMax_.x > dishAreaMin_.x + winW && dishAreaMax_.y > dishAreaMin_.y + lastH;
        const float x0 = known ? dishAreaMin_.x : 0.0f, x1 = known ? dishAreaMax_.x : io.DisplaySize.x;
        const float y0 = std::max(navBottom(), known ? dishAreaMin_.y : 0.0f), y1 = known ? dishAreaMax_.y : io.DisplaySize.y;
        winX = std::floor(std::clamp(cfg_.timeBarX * io.DisplaySize.x - winW * 0.5f, x0, x1 - winW));
        winY = std::floor(std::clamp(cfg_.timeBarY * io.DisplaySize.y - lastH * 0.5f, y0, std::max(y0, y1 - lastH)));
        lastH = std::max(lastH, 20.0f);
        if (ImGui::FindWindowByName("##mtime")) lastH = ImGui::FindWindowByName("##mtime")->Size.y;
    }
    ImGui::SetNextWindowPos({winX, winY});
    ImGui::SetNextWindowSizeConstraints({winW, 0}, {winW, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, compact ? 12 * u : 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {padX, (compact ? 4 : 6) * u});
    ImGui::Begin("##mtime", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::SetCursorPosX(std::floor((winW - rowW) * 0.5f));
    struct B {
        Speed sp;
        Icon ic;
        const char* tip;
    } btns[] = {{Speed::Freeze, IcFreeze, "Заморозка"},
                {Speed::Slow, IcSlow, "Медленно"},
                {Speed::Observe, IcPlay, "Наблюдение"},
                {Speed::Incubate, IcFast, "Инкубация"}};
    for (auto& b : btns) {
        if (iconButton(b.tip, b.ic, ui_.speed == b.sp, b.tip, s)) {
            if (b.sp == Speed::Freeze && ui_.speed != Speed::Freeze) ui_.speedBeforeFreeze = ui_.speed;
            ui_.speed = b.sp;
        }
        ImGui::SameLine(0, gapB);
    }
    if (iconButton("step", IcStep, false, "Шаг: +0.05 ч", s)) {
        ui_.speed = Speed::Freeze;
        for (int i = 0; i < 20; ++i) world_->step();
    }
    ImGui::SameLine(0, gapB);
    if (iconButton("dish", IcMenu, false, "Чашка: эксперименты, перезагрузка, поделиться, вид", s)) ImGui::OpenPopup("dishmenu");
    ImGui::PushFont(fontSmall_);
    {
        const float dl = world_->daylight();
        const char* tod = world_->settings.dayLength <= 0.01f ? "" : dl > 0.85f ? " · день" : dl < 0.05f ? " · ночь" : " · сумерки";
        char st[160];
        std::snprintf(st, sizeof(st), "%.2f ч%s · %d кл. · %zu пищи · %.2f ч/с", world_->timeHours(), tod, world_->cellCount(),
                      world_->food().size(), hoursPerSecond_);
        // The status line is the bar's handle: drag it to move the bar (it turns into a free pill), double-tap to
        // put it back in its place.
        const ImVec2 ts = ImGui::CalcTextSize(st);
        ImGui::SetCursorPosX(padX);
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##timehandle", {winW - 2 * padX, ts.y + 4 * u});
        keepTouchDrag();
        static bool moving = false;
        static ImVec2 grab{0, 0};  // finger offset from the bar's centre
        if (ImGui::IsItemActivated()) {
            moving = false;
            grab = {io.MousePos.x - (winX + winW * 0.5f), io.MousePos.y - (winY + ImGui::GetWindowHeight() * 0.5f)};
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 6 * u)) moving = true;
        if (moving && ImGui::IsItemActive()) {
            cfg_.timeBarFree = true;
            cfg_.timeBarX = std::clamp((io.MousePos.x - grab.x) / io.DisplaySize.x, 0.0f, 1.0f);
            cfg_.timeBarY = std::clamp((io.MousePos.y - grab.y) / io.DisplaySize.y, 0.0f, 1.0f);
        }
        if (ImGui::IsItemDeactivated() && moving) {
            markConfigDirty();
            previewAutoFrame_ = true;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && cfg_.timeBarFree) {
            cfg_.timeBarFree = false;
            markConfigDirty();
            previewAutoFrame_ = true;
        }
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        dl2->AddText({hp.x + (winW - 2 * padX - ts.x) * 0.5f, hp.y + 2 * u},
                     ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_Text : ImGuiCol_TextDisabled), st);
    }
    ImGui::PopFont();

    bool openSter = false;
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({0, 0}, {io.DisplaySize.x - 16 * u, io.DisplaySize.y * 0.8f});
    if (beginTouchMenu("dishmenu", u)) {
        if (ImGui::MenuItem("Эксперименты…")) openExperimentBrowser();
        if (ImGui::MenuItem("Перезагрузить чашку")) reloadSubstrate();
        ImGui::Separator();
        ImGui::TextDisabled("Поделиться чашкой");
        dishShareItems();
        ImGui::Separator();
        if (ImGui::MenuItem("Выбрать геном для синтезатора…")) {
            openGenomeBrowser();
            gbAfterOpen_ = Screen::Microscope;
            ui_.tool = Tool::Synth;
        }
        if (ImGui::MenuItem(ui_.challenge >= 0 ? "Начать задание заново" : "Стерилизовать…")) openSter = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Вся чашка")) fitCamera(false);
        int t = trackedCell();
        if (ImGui::MenuItem("Следить за клеткой", nullptr, ui_.follow, t >= 0)) ui_.follow = !ui_.follow;
        if (ImGui::MenuItem("Снять выделение", nullptr, false, t >= 0)) track(-1);
        ImGui::Separator();
        auto flag = [&](const char* label, bool* v) {
            if (ImGui::MenuItem(label, nullptr, *v)) {
                *v = !*v;
                markConfigDirty();
            }
        };
        flag("Адгезины", &cfg_.showSlime);
        flag("Пища", &cfg_.showFood);
        flag("Сетка", &cfg_.showGrid);
        bool classic = cfg_.theme != 0;
        flag("Классический микроскоп", &classic);
        cfg_.theme = classic ? 1 : 0;
        ImGui::Separator();
        ImGui::TextDisabled("Панель времени (тяните за строку под кнопками)");
        ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);  // tap a few times and watch the bar change
        if (ImGui::MenuItem("Крупнее", nullptr, false, cfg_.timeBarScale < 1.45f)) {
            cfg_.timeBarScale = std::min(1.5f, cfg_.timeBarScale + 0.1f);
            markConfigDirty();
        }
        if (ImGui::MenuItem("Мельче", nullptr, false, cfg_.timeBarScale > 0.65f)) {
            cfg_.timeBarScale = std::max(0.6f, cfg_.timeBarScale - 0.1f);
            markConfigDirty();
        }
        ImGui::PopItemFlag();
        if (ImGui::MenuItem("На место", nullptr, false, cfg_.timeBarFree)) {
            cfg_.timeBarFree = false;
            markConfigDirty();
            previewAutoFrame_ = true;
        }
        if (portrait_) {
            ImGui::Separator();
            ImGui::TextDisabled("Инструменты");
            const char* sides[] = {"Снизу", "Слева", "Справа"};
            for (int k = 0; k < 3; ++k)
                if (ImGui::MenuItem(sides[k], nullptr, cfg_.toolSide == k)) {
                    cfg_.toolSide = k;
                    markConfigDirty();
                    previewAutoFrame_ = true;
                }
        }
        endTouchMenu();
    }
    if (openSter) ImGui::OpenPopup("sterilize");
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopup("sterilize")) {
        if (ui_.challenge >= 0) {
            ImGui::TextUnformatted("Начать задание заново?");
            if (ImGui::Button("Заново", {200 * u, 0})) {
                startChallenge(ui_.challenge);
                ImGui::CloseCurrentPopup();
            }
        } else {
            ImGui::TextUnformatted("Удалить все клетки и пищу?");
            if (ImGui::Button("Стерилизовать", {200 * u, 0})) {
                world_->sterilize();
                track(-1);
                toast("Чашка стерилизована");
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    float h = ImGui::GetWindowSize().y;
    if (!compact) edgeLine({x, y + h - 1}, {x + w, y + h - 1});
    else h += 8 * u;  // the pill floats 4 u below the tabs, with as much room under it
    if (freeBar) h = 0;  // a free pill floats over the dish and takes no room
    ImGui::End();
    return h;
}

void App::drawMobileMicroscope() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    float bottom = 0, left = 0, right = 0;
    static float lastBarH = 0;  // the time bar (measured last frame): the sheet never covers it
    drawMobilePalette(bottom, left, right, lastBarH);
    if (portrait_) {  // a side strip: the sheet keeps clear of it
        dockInsetL_ = left;
        dockInsetR_ = right;
    }

    // A level that has just started shows its goal.
    static int shownChallenge = -1;
    if (ui_.challenge != shownChallenge) {
        if (ui_.challenge >= 0) {
            dockMicro_.tab = 3;
            dockMicro_.open = true;
        } else if (dockMicro_.tab == 3) {
            dockMicro_.tab = 0;
            dockMicro_.open = false;
        }
        shownChallenge = ui_.challenge;
    }
    const char* labels[] = {"Инструмент", "Клетка", "Статистика", "Задание"};
    // Folded in landscape: a rail of these icons (the tool tab shows the tool in hand).
    static const int kToolIcons[] = {IcInspect, IcSynth, IcTweezers, IcBoost, IcRemove, IcShock, IcGeneNeedle, IcDrugNeedle, IcMap, IcAdhesin};
    const int tabIcons[] = {kToolIcons[std::clamp((int)ui_.tool, 0, 9)], IcCell, IcStats, IcFlag};
    dockIcons_ = tabIcons;
    int tab = beginDock("##mdock", dockMicro_, labels, ui_.challenge >= 0 ? 4 : 3, bottom, false, top + lastBarH + 40 * u);
    const float bodyW = ImGui::GetContentRegionAvail().x - 8 * u;
    switch (tab) {
        case 0: drawToolOptionsBody(bodyW); break;
        case 1:
            if (trackedCell() < 0) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Выберите клетку: инструмент «Диагностика» (лупа) и касание клетки.");
                ImGui::PopTextWrapPos();
            } else {
                drawCellTab();
            }
            break;
        case 2: drawStatsTab(); break;
        case 3:
            if (ui_.challenge >= 0) drawChallengeBody(bodyW);
            break;
        default: break;
    }
    endDock();
    dockInsetL_ = dockInsetR_ = 0;
    dockIcons_ = nullptr;
    const ImVec2 dp = dockPos_;

    const float barX = portrait_ ? 0 : left;
    const float barW = portrait_ ? io.DisplaySize.x : dp.x - left;
    const float barH = drawMobileTimeBar(barX, top, barW);
    lastBarH = barH;
    if (portrait_) {
        freeMin_ = {left, top + barH};
        freeMax_ = {io.DisplaySize.x - right, dp.y};
    } else {
        freeMin_ = {left, top + barH};
        // Open: the column; folded: the rail of tab icons. Either way the dish ends at its left edge.
        freeMax_ = {dp.x, io.DisplaySize.y};
    }
    dishAreaMin_ = freeMin_;  // where the dish shows: a free time bar stays inside it (not over the panels)
    dishAreaMax_ = freeMax_;
    drawChallengePanel();  // the result window (the goal itself is in the dock)

    // First frame and every rotation: frame the dish in the free area (known only now).
    // Auto-sized bars are measured a frame late: frame again for a few frames.
    static ImVec2 framedFor{0, 0};
    static int settle = 0;
    if (framedFor.x != io.DisplaySize.x || framedFor.y != io.DisplaySize.y) {
        framedFor = io.DisplaySize;
        settle = 3;
        previewAutoFrame_ = true;
    }
    if (settle > 0) {
        --settle;
        if (opt_.zoom <= 0) fitCamera(true);  // (following a cell overrides only the centre)
    }
}

// ---------------------------------------------------------------------------- genome editor
// Top to bottom: one bar (menu, name, undo / redo, "to the dish"), the organism, one timeline row with the stage
// written on the slider, and the sheet: «Клетка» (the tapped cell: divide, type, colour), «Мода» (every setting of
// the selected mode) and «Все моды» (the list). Landscape: the sheet is the right column.
void App::drawMobileGenome() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    Genome& g = ui_.editGenome;

    // A tapped cell opens its actions.
    static int lastSel = -1;
    static bool lastPhantom = false;
    if (editorPopup_ && selCell_ >= 0 && (selCell_ != lastSel || selPhantom_ != lastPhantom)) {
        dockGenome_.tab = 0;
        dockGenome_.open = true;
    }
    lastSel = editorPopup_ ? selCell_ : -1;
    lastPhantom = selPhantom_;

    // Sheet first: in landscape the bars on the left take the width it leaves.
    char modeLabel[32];
    std::snprintf(modeLabel, sizeof(modeLabel), "Мода M%d", ui_.editMode + 1);
    const char* labels[] = {"Клетка", modeLabel, "Все моды"};
    // The sheet stops below the top bar, the timeline and a strip of the organism (heights from the last frame).
    static float lastBarH = 0, lastTlH = 0;
    int tab = beginDock("##gdock", dockGenome_, labels, 3, 0, true, top + lastBarH + lastTlH + 56 * u);
    switch (tab) {
        case 0:
            if (!drawCellActionsBody()) {
                ImGui::Dummy({0, 6 * u});
                ImGui::PushFont(fontBold_);
                ImGui::TextUnformatted("Коснитесь клетки организма");
                ImGui::PopFont();
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted("Здесь появятся «Разделить», тип и цвет этой клетки.");
                ImGui::TextDisabled("Полупрозрачный «призрак» — будущий потомок: тяните его пальцем, чтобы повернуть деление.");
                ImGui::PopTextWrapPos();
            }
            break;
        case 1: drawModePanelBody(); break;
        case 2: drawModeListBody(); break;
        default: break;
    }
    endDock();
    const ImVec2 dp = dockPos_;
    const float sx = std::ceil(safe_.x);  // clear of a left camera cutout
    const float barW = (portrait_ ? io.DisplaySize.x : dp.x) - sx;

    // ---- top bar: menu, name, undo / redo, "to the dish"
    ImGui::SetNextWindowPos({sx, top});
    ImGui::SetNextWindowSizeConstraints({barW, 0}, {barW, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * u, 7 * u});
    ImGui::Begin("##mgbar", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10 * u, 11 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6 * u, 6 * u});
    {
        const float h = ImGui::GetFrameHeight(), sp = ImGui::GetStyle().ItemSpacing.x;
        const char* toDish = "В чашку ›";
        const float dw = ImGui::CalcTextSize(toDish).x + 2 * ImGui::GetStyle().FramePadding.x;
        if (iconFrameButton("##gmenu", IcMenu, {h, h})) ImGui::OpenPopup("gmenu");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(60 * u, ImGui::GetContentRegionAvail().x - dw - 2 * h - 3 * sp));
        char name[128];
        std::snprintf(name, sizeof(name), "%s", g.name.c_str());
        if (ImGui::InputTextWithHint("##name", "Название генома", name, sizeof(name))) g.name = name;
        ImGui::SameLine();
        ImGui::BeginDisabled(ui_.undo.empty());
        if (iconFrameButton("##undo", IcUndo, {h, h})) undoGenome();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(ui_.redo.empty());
        if (iconFrameButton("##redo", IcRedo, {h, h})) redoGenome();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.55f));
        if (ImGui::Button(toDish, {dw, h})) {
            ui_.screen = Screen::Microscope;
            ui_.tool = Tool::Synth;
            toast("Синтезатор: коснитесь чашки, чтобы вставить клетку");
        }
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar(2);

    bool openSave = false;
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.42f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({std::min(300 * u, io.DisplaySize.x - 16 * u), 0}, {io.DisplaySize.x - 16 * u, io.DisplaySize.y * 0.85f});
    if (beginTouchMenu("gmenu", u)) {
        ImGui::TextDisabled("Геном");
        if (ImGui::MenuItem("Новый")) genomeNew();
        if (ImGui::MenuItem("Открыть…")) openGenomeBrowser();
        if (ImGui::MenuItem("Сохранить…")) openSave = true;
        if (ImGui::MenuItem("Взять у клетки в чашке", nullptr, false, trackedCell() >= 0)) genomeFromCell();
        if (ImGui::MenuItem("Взрослый организм в чашку")) genomeAdultToDish();
        ImGui::Separator();
        ImGui::TextDisabled("Поделиться");
        genomeShareItems(g);
        ImGui::Separator();
        ImGui::TextDisabled("Показывать");
        auto flag = [&](const char* label, bool* v, bool persist) {
            if (ImGui::MenuItem(label, nullptr, *v)) {
                *v = !*v;
                if (persist) markConfigDirty();
            }
        };
        flag("Связи между клетками", &ui_.showLinks, false);
        flag("Сетку", &cfg_.showGrid, true);
        flag("Подсказку «Как собрать»", &cfg_.editorTips, true);
        ImGui::Separator();
        ImGui::TextDisabled("Поворот деления");
        flag("Симметрично", &ui_.symmetry, false);
        flag("Шагами по 15°", &ui_.snap, false);
        endTouchMenu();
    }
    if (openSave) ImGui::OpenPopup("savegen");
    drawGenomeFilePopups();
    const float barH = ImGui::GetWindowSize().y;
    edgeLine({sx, top + barH - 1}, {sx + barW, top + barH - 1});
    ImGui::End();

    // ---- timeline: above the sheet (portrait) or along the bottom of the free area (landscape)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * u, 7 * u});
    const float tlBottom = portrait_ ? dp.y : io.DisplaySize.y + 3;  // landscape: past the bottom edge (no stray row)
    ImGui::SetNextWindowPos({sx, tlBottom}, ImGuiCond_Always, {0.0f, 1.0f});
    ImGui::SetNextWindowSizeConstraints({barW, 0}, {barW, FLT_MAX});
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##mtimeline", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    drawMobileTimeline();
    const float tlH = ImGui::GetWindowSize().y;
    edgeLine({sx, tlBottom - tlH}, {sx + barW, tlBottom - tlH});
    ImGui::End();
    lastBarH = barH;
    lastTlH = tlH;

    freeMin_ = {sx, top + barH};
    freeMax_ = {sx + barW, tlBottom - tlH};
    static ImVec2 framedFor{0, 0};  // rotated: frame the organism again
    if (framedFor.x != io.DisplaySize.x || framedFor.y != io.DisplaySize.y) {
        framedFor = io.DisplaySize;
        previewAutoFrame_ = true;
    }

    // ---- "fit" button in the corner of the organism view
    {
        const float s = 44 * u, m = 8 * u;
        ImGui::SetNextWindowPos({freeMax_.x - s - m, freeMin_.y + m});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, s * 0.5f);
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::Begin("##mgfit", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.13f, 0.15f, 0.72f));
        if (iconFrameButton("##fit", IcFit, {s, s}, 0.46f)) previewAutoFrame_ = true;
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar(2);
    }

    // ---- how-to card at the bottom of the organism view
    if (cfg_.editorTips) {
        float w = std::min(520 * u, freeMax_.x - freeMin_.x - 16 * u);
        ImGui::SetNextWindowPos({(freeMin_.x + freeMax_.x) * 0.5f, freeMax_.y - 8 * u}, ImGuiCond_Always, {0.5f, 1.0f});
        ImGui::SetNextWindowSize({w, 0});
        ImGui::SetNextWindowBgAlpha(0.94f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12 * u, 10 * u});
        ImGui::Begin("##mtips", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PopStyleVar();
        ImGui::PushFont(fontBold_);
        ImGui::TextUnformatted("Как собрать организм");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("1. Коснитесь клетки — внизу откроется вкладка «Клетка».");
        ImGui::TextUnformatted("2. Выберите тип новой клетки и нажмите «Разделить».");
        ImGui::TextUnformatted("3. Тяните полупрозрачного «призрака», чтобы повернуть деление.");
        ImGui::TextDisabled("Шкала под организмом — время развития. «Мода» — все настройки выбранной моды, "
                            "«Все моды» — их список. Двумя пальцами — сдвиг и масштаб.");
        ImGui::PopTextWrapPos();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {16 * u, 10 * u});
        if (ImGui::Button("Понятно")) {
            cfg_.editorTips = false;
            markConfigDirty();
        }
        ImGui::PopStyleVar();
        ImGui::End();
    }
}

// One row: to the start, a stage back, the slider (with the stage written on it), a stage on, the grown organism.
// Holding a stage button steps tick by tick.
void App::drawMobileTimeline() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    const int maxTick = std::max(1, editorMaxTick());
    const int stages = (int)layout_.stages.size() - 1;
    const int st = editorStage();
    int ghosts = 0;
    for (const PreviewRef& r : previewMap_) ghosts += r.phantom;
    int tick = editorTick_;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8 * u, 11 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {5 * u, 5 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 26 * u);
    const float h = ImGui::GetFrameHeight(), bw = h * 1.05f;
    static ImGuiID repeating = 0;  // a stage button held down: steps by ticks, and its release is not a click
    auto stepButton = [&](const char* id, Icon ic, int dir) {
        bool clicked = iconFrameButton(id, ic, {bw, h});
        ImGuiID me = ImGui::GetItemID();
        if (ImGui::IsItemActive() && io.MouseDownDuration[0] > 0.4f) {
            tick = std::clamp(tick + dir, 0, maxTick);
            repeating = me;
        }
        if (clicked) {
            if (repeating == me) repeating = 0;
            else if (dir < 0) tick = std::max(0, ((tick - 1) / kTicksPerStage) * kTicksPerStage);
            else tick = std::min(maxTick, (tick / kTicksPerStage + 1) * kTicksPerStage);
        }
    };
    if (iconFrameButton("##tstart", IcToStart, {bw, h})) tick = 0;
    ImGui::SameLine();
    stepButton("##tprev", IcPrev, -1);
    ImGui::SameLine();
    char rd[96];
    std::snprintf(rd, sizeof(rd), "Стадия %d из %d · клеток %d", st, stages, preview_->cellCount() - ghosts);
    const float sw = ImGui::GetContentRegionAvail().x - 2 * bw - 2 * ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::CalcTextSize(rd).x > sw - 50 * u) std::snprintf(rd, sizeof(rd), "%d / %d", st, stages);
    ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(sw);
    ImGui::SliderInt("##tick", &tick, 0, maxTick, rd);
    keepTouchDrag();
    // Stage boundaries as marks along the bottom of the slider.
    if (stages > 0) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float y1 = sp.y + h - 2 * u, y0 = y1 - 5 * u, pad = ImGui::GetStyle().GrabMinSize * 0.5f;
        for (int s = 0; s <= stages; ++s) {
            float x = sp.x + pad + (sw - 2 * pad) * (float)(s * kTicksPerStage) / maxTick;
            dl->AddLine({x, y0}, {x, y1}, s == st ? kAccentU32 : IM_COL32(150, 150, 160, 140), 1.5f * u);
        }
    }
    ImGui::SameLine();
    stepButton("##tnext", IcNext, +1);
    ImGui::SameLine();
    if (iconFrameButton("##tend", IcToEnd, {bw, h})) tick = maxTick;
    ImGui::PopStyleVar(3);
    tick = std::min(tick, editorMaxTick());
    if (tick != editorTick_) {
        editorTick_ = tick;
        if (selPhantom_) selCell_ = -1;
    }
}
