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
}  // namespace

int App::fitColumns(float avail, float minColW, int maxCols) const {
    const float gap = 14 * uiScale_;
    return std::clamp((int)((avail + gap) / (minColW + gap)), 1, maxCols);
}

void App::applyOrientation() {
#ifdef __ANDROID__
    glfwAndroidSetOrientation(cfg_.orientation);
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
    auto scrollable = [](ImGuiWindow* w) -> ImGuiWindow* {
        while (w) {
            if (w->ScrollMax.y > 0.5f || w->ScrollMax.x > 0.5f) return w;
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
            bool horizontal = target->ScrollMax.x > 0.5f && std::fabs(d.x) > th && std::fabs(d.x) > std::fabs(d.y);
            if (vertical || horizontal) {
                scrolling = true;
                if (g.ActiveId != 0) ImGui::ClearActiveID();  // it was a scroll, not a tap on that button
            }
        }
        if (scrolling) {
            ImVec2 md = io.MouseDelta;
            ImGui::SetScrollY(target, target->Scroll.y - md.y);
            ImGui::SetScrollX(target, target->Scroll.x - md.x);
            float dt = std::max(io.DeltaTime, 1e-3f);
            vel = {vel.x * 0.6f + 0.4f * (-md.x / dt), vel.y * 0.6f + 0.4f * (-md.y / dt)};
        }
    } else if (target && scrolling) {
        // Released: glide on and slow down.
        float dt = io.DeltaTime;
        ImGui::SetScrollY(target, target->Scroll.y + vel.y * dt);
        ImGui::SetScrollX(target, target->Scroll.x + vel.x * dt);
        float k = std::exp(-dt * 4.0f);
        vel = {vel.x * k, vel.y * k};
        if (std::fabs(vel.x) + std::fabs(vel.y) < 20.0f) scrolling = false;
    }
    gNoTouchScrollId = 0;
}

// ---------------------------------------------------------------------------- docked sheet
int App::beginDock(const char* id, Dock& d, const char* const* labels, int count, float bottomReserve) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    d.tab = std::clamp(d.tab, 0, count - 1);
    // A denser style than the rest of the phone UI: the sheet shares the screen with the dish.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {9 * u, 8 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8 * u, 6 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, {6 * u, 4 * u});
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 22 * u);
    const float gripH = portrait_ ? 16 * u : 0;
    const float stripH = ImGui::GetFrameHeight() + 10 * u + gripH;
    const float room = io.DisplaySize.y - top - bottomReserve;  // height the sheet may take in portrait
    ImVec2 pos, size;
    if (portrait_) {
        float h = d.open ? std::round(std::max(stripH + 60 * u, room * d.frac)) : std::ceil(stripH);
        pos = {0, std::round(io.DisplaySize.y - bottomReserve - h)};
        size = {io.DisplaySize.x, h};
    } else {
        float w = std::round(std::clamp(io.DisplaySize.x * d.wfrac, 200 * u, io.DisplaySize.x * 0.75f));
        float h = d.open ? io.DisplaySize.y - top + 4 : std::ceil(stripH);  // open: reaches past the bottom edge
        pos = {std::floor(io.DisplaySize.x - std::ceil(safe_.z) - w), top};  // clear of a right camera cutout
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
    if (portrait_) {
        // Handle: drag to change the height (down to the bottom folds the sheet), tap to fold / unfold.
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##grip", {w, gripH});
        keepTouchDrag();
        bool active = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            float h = (d.open ? size.y : stripH) - io.MouseDelta.y;
            d.open = true;
            d.frac = std::clamp(h / room, 0.12f, 0.85f);
        }
        if (ImGui::IsItemDeactivated()) {
            if (io.MouseDragMaxDistanceSqr[0] < 100.0f * u * u) d.open = !d.open;  // a tap
            else if (d.frac < 0.2f) {                                            // dragged nearly shut
                d.open = false;
                d.frac = 0.36f;
            }
        }
        ImGui::GetWindowDrawList()->AddRectFilled({p.x + w * 0.5f - 22 * u, p.y + gripH * 0.5f - 2 * u},
                                                  {p.x + w * 0.5f + 22 * u, p.y + gripH * 0.5f + 2 * u},
                                                  active ? kAccentU32 : IM_COL32(255, 255, 255, 90), 2 * u);
    } else if (d.open) {
        // Handle on the left edge: drag sideways to make the side panel wider or narrower.
        ImVec2 back = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##gripw", {12 * u, size.y});
        keepTouchDrag();
        bool active = ImGui::IsItemActive();
        if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f))
            d.wfrac = std::clamp(d.wfrac - io.MouseDelta.x / io.DisplaySize.x, 0.22f, 0.75f);
        const float cy = pos.y + std::min(size.y, io.DisplaySize.y - pos.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled({pos.x + 4 * u, cy - 22 * u}, {pos.x + 8 * u, cy + 22 * u},
                                                  active ? kAccentU32 : IM_COL32(255, 255, 255, 90), 2 * u);
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
        ImGui::PopID();
        if (small) ImGui::PopFont();
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
void App::drawMobilePalette(float& reservedBottom, float& reservedLeft) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom(), pad = 6 * u, gap = 4 * u;
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
    const float avail = portrait_ ? io.DisplaySize.x - 2 * pad : io.DisplaySize.y - top - 2 * pad;
    int lines = 1;
    float s = (avail - (n - 1) * gap) / n;
    if (s < 34 * u) {
        lines = 2;
        s = (avail - (n / 2 - 1) * gap) / (n / 2);
    }
    s = std::floor(std::min(s, 58 * u));
    const int perLine = n / lines;
    const float thick = std::ceil(lines * s + (lines - 1) * gap + 2 * pad);  // strip thickness
    const float along = perLine * s + (perLine - 1) * gap;                   // length of one line of buttons
    // Whole pixels, and the window reaches a few pixels past the screen edge: a fractional edge left a one-pixel
    // row of the dish showing along the bottom of the screen.
    ImVec2 pos, size, origin;
    float off;
    if (portrait_) {
        pos = {-4, std::floor(io.DisplaySize.y - thick - std::ceil(safe_.w))};
        size = {io.DisplaySize.x + 8, io.DisplaySize.y - pos.y + 4};
        reservedBottom = io.DisplaySize.y - pos.y;
        origin = {pad, pos.y + pad};
        off = std::max(0.0f, (io.DisplaySize.x - 2 * pad - along) * 0.5f);
    } else {
        const float left = std::ceil(safe_.x);  // a camera cutout on the left edge
        pos = {-4, top};
        size = {thick + left + 4, io.DisplaySize.y - top + 4};
        reservedLeft = thick + left;
        origin = {left + pad, top + pad};
        off = std::max(0.0f, (io.DisplaySize.y - top - 2 * pad - along) * 0.5f);
    }
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, gap});
    ImGui::Begin("##mpalette", nullptr, kDocked | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    for (int i = 0; i < n; ++i) {
        int line = i / perLine, k = i % perLine;
        ImVec2 p = portrait_ ? ImVec2(origin.x + off + k * (s + gap), origin.y + line * (s + gap))
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
        }
        if (!allowed) ImGui::EndDisabled();
    }
    if (portrait_) edgeLine({pos.x, pos.y}, {pos.x + size.x, pos.y});
    else edgeLine({pos.x + size.x - 1, pos.y}, {pos.x + size.x - 1, pos.y + size.y});
    ImGui::End();
    ImGui::PopStyleVar(3);
}

float App::drawMobileTimeBar(float x, float y, float w) {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSizeConstraints({w, 0}, {w, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * u, 6 * u});
    ImGui::Begin("##mtime", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    const float s = 44 * u;
    const char* menuLabel = "Чашка…";
    const float mw = ImGui::CalcTextSize(menuLabel).x + 2 * ImGui::GetStyle().FramePadding.x;
    // Speed buttons and the dish menu as one centred row.
    const float rowW = 5 * s + 4 * 2 * u + 14 * u + mw;
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (w - rowW) * 0.5f));
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
        ImGui::SameLine(0, 2 * u);
    }
    if (iconButton("step", IcStep, false, "Шаг: +0.05 ч", s)) {
        ui_.speed = Speed::Freeze;
        for (int i = 0; i < 20; ++i) world_->step();
    }
    ImGui::SameLine(0, 14 * u);
    if (ImGui::Button(menuLabel, {mw, s})) ImGui::OpenPopup("dishmenu");
    ImGui::PushFont(fontSmall_);
    {
        const float dl = world_->daylight();
        const char* tod = world_->settings.dayLength <= 0.01f ? "" : dl > 0.85f ? " · день" : dl < 0.05f ? " · ночь" : " · сумерки";
        char st[160];
        std::snprintf(st, sizeof(st), "%.2f ч%s · %d кл. · %zu пищи · %.2f ч/с", world_->timeHours(), tod, world_->cellCount(),
                      world_->food().size(), hoursPerSecond_);
        ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (w - ImGui::CalcTextSize(st).x) * 0.5f));
        ImGui::TextDisabled("%s", st);
    }
    ImGui::PopFont();

    bool openExp = false, openSter = false;
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({0, 0}, {io.DisplaySize.x - 16 * u, io.DisplaySize.y * 0.8f});
    if (ImGui::BeginPopup("dishmenu")) {
        if (ImGui::MenuItem("Эксперименты…")) openExp = true;
        if (ImGui::MenuItem("Перезагрузить чашку")) reloadSubstrate();
        if (ImGui::MenuItem("Поделиться чашкой…")) shareDish();
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
        ImGui::EndPopup();
    }
    if (openExp) ImGui::OpenPopup("experiments");
    if (openSter) ImGui::OpenPopup("sterilize");

    const float pw = std::min(460 * u, io.DisplaySize.x - 16 * u);
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({pw, 0}, {pw, io.DisplaySize.y * 0.8f});
    if (ImGui::BeginPopup("experiments")) {
        ImGui::PushFont(fontBold_);
        ImGui::TextUnformatted("Новая чашка");
        ImGui::PopFont();
        ImGui::Separator();
        const auto& exps = experiments();
        ImGui::PushTextWrapPos(0);
        for (int i = 0; i < (int)exps.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(exps[i].name)) {
                loadExperiment(i);
                toast(std::string("Эксперимент: ") + exps[i].name);
            }
            ImGui::PushFont(fontSmall_);
            ImGui::TextDisabled("%s", exps[i].description);
            ImGui::PopFont();
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
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
    edgeLine({x, y + h - 1}, {x + w, y + h - 1});
    ImGui::End();
    return h;
}

void App::drawMobileMicroscope() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    float bottom = 0, left = 0;
    drawMobilePalette(bottom, left);

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
    int tab = beginDock("##mdock", dockMicro_, labels, ui_.challenge >= 0 ? 4 : 3, bottom);
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
    const ImVec2 dp = dockPos_;

    const float barX = portrait_ ? 0 : left;
    const float barW = portrait_ ? io.DisplaySize.x : dp.x - left;
    const float barH = drawMobileTimeBar(barX, top, barW);
    if (portrait_) {
        freeMin_ = {0, top + barH};
        freeMax_ = {io.DisplaySize.x, dp.y};
    } else {
        freeMin_ = {left, top + barH};
        // A folded sheet is only its tab strip at the top right: the dish may use the full width below it.
        freeMax_ = {dockMicro_.open ? dp.x : io.DisplaySize.x, io.DisplaySize.y};
        freeMin_.y = std::max(freeMin_.y, dp.y + dockSize_.y);
    }
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
void App::drawMobileGenome() {
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_, top = navBottom();
    Genome& g = ui_.editGenome;

    // A tapped cell opens its actions.
    static int lastSel = -1;
    static bool lastPhantom = false;
    if (editorPopup_ && selCell_ >= 0 && (selCell_ != lastSel || selPhantom_ != lastPhantom)) {
        dockGenome_.tab = 2;
        dockGenome_.open = true;
    }
    lastSel = editorPopup_ ? selCell_ : -1;
    lastPhantom = selPhantom_;

    // Sheet first: in landscape the bars on the left take the width it leaves.
    char modeLabel[32];
    std::snprintf(modeLabel, sizeof(modeLabel), "Мода M%d", ui_.editMode + 1);
    const char* labels[] = {"Моды", modeLabel, "Клетка"};
    int tab = beginDock("##gdock", dockGenome_, labels, 3, 0);
    switch (tab) {
        case 0: drawModeListBody(); break;
        case 1: drawModePanelBody(); break;
        case 2:
            if (!drawCellActionsBody()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Коснитесь клетки организма: здесь появятся «Разделить», тип и цвет. "
                                    "Полупрозрачного «призрака» потомка можно тянуть пальцем — это направление деления.");
                ImGui::PopTextWrapPos();
            }
            break;
        default: break;
    }
    endDock();
    const ImVec2 dp = dockPos_;
    const float sx = std::ceil(safe_.x);  // clear of a left camera cutout
    const float barW = (portrait_ ? io.DisplaySize.x : dp.x) - sx;

    // Genome bar: name, file and view menus, undo, "to the dish".
    ImGui::SetNextWindowPos({sx, top});
    ImGui::SetNextWindowSizeConstraints({barW, 0}, {barW, FLT_MAX});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * u, 6 * u});
    ImGui::Begin("##mgbar", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    {
        const char* toDish = "В чашку ›";
        float dw = ImGui::CalcTextSize(toDish).x + 2 * ImGui::GetStyle().FramePadding.x;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - dw - ImGui::GetStyle().ItemSpacing.x);
        char name[128];
        std::snprintf(name, sizeof(name), "%s", g.name.c_str());
        if (ImGui::InputTextWithHint("##name", "Название генома", name, sizeof(name))) g.name = name;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 0.62f, 0.20f, 0.55f));
        if (ImGui::Button(toDish, {dw, 0})) {
            ui_.screen = Screen::Microscope;
            ui_.tool = Tool::Synth;
            toast("Синтезатор: коснитесь чашки, чтобы вставить клетку");
        }
        ImGui::PopStyleColor();
    }
    {
        // Second row, centred.
        const ImGuiStyle& sty = ImGui::GetStyle();
        float rowW = 3 * sty.ItemSpacing.x;
        for (const char* l : {"Файл…", "Вид…", "Отменить", "Повторить"}) rowW += ImGui::CalcTextSize(l).x + 2 * sty.FramePadding.x;
        ImGui::SetCursorPosX(std::max(sty.WindowPadding.x, (barW - rowW) * 0.5f));
    }
    if (ImGui::Button("Файл…")) ImGui::OpenPopup("gfile");
    ImGui::SameLine();
    if (ImGui::Button("Вид…")) ImGui::OpenPopup("gview");
    ImGui::SameLine();
    ImGui::BeginDisabled(ui_.undo.empty());
    if (ImGui::Button("Отменить")) undoGenome();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ui_.redo.empty());
    if (ImGui::Button("Повторить")) redoGenome();
    ImGui::EndDisabled();

    bool openLoad = false, openSave = false;
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopup("gfile")) {
        if (ImGui::MenuItem("Новый")) genomeNew();
        if (ImGui::MenuItem("Выбрать геном…")) openLoad = true;
        if (ImGui::MenuItem("Поделиться…")) shareGenome(g);
        if (ImGui::MenuItem("Сохранить…")) openSave = true;
        if (ImGui::MenuItem("Из выбранной клетки", nullptr, false, trackedCell() >= 0)) genomeFromCell();
        ImGui::Separator();
        if (ImGui::MenuItem("Взрослый в чашку")) genomeAdultToDish();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopup("gview")) {
        auto flag = [&](const char* label, bool* v, bool persist) {
            if (ImGui::MenuItem(label, nullptr, *v)) {
                *v = !*v;
                if (persist) markConfigDirty();
            }
        };
        flag("Связи", &ui_.showLinks, false);
        flag("Сетка", &cfg_.showGrid, true);
        flag("Симметрия", &ui_.symmetry, false);
        flag("Привязка 15°", &ui_.snap, false);
        flag("Простой режим", &cfg_.editorSimple, true);
        flag("Как пользоваться", &cfg_.editorTips, true);
        ImGui::EndPopup();
    }
    if (openLoad) openGenomeBrowser();
    if (openSave) ImGui::OpenPopup("savegen");
    drawGenomeFilePopups();
    const float barH = ImGui::GetWindowSize().y;
    edgeLine({sx, top + barH - 1}, {sx + barW, top + barH - 1});
    ImGui::End();

    // Timeline: above the sheet (portrait) or along the bottom of the free area (landscape).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8 * u, 4 * u});
    const float tlBottom = portrait_ ? dp.y : io.DisplaySize.y + 3;  // landscape: past the bottom edge (no stray row)
    ImGui::SetNextWindowPos({sx, tlBottom}, ImGuiCond_Always, {0.0f, 1.0f});
    ImGui::SetNextWindowSizeConstraints({barW, 0}, {barW, FLT_MAX});
    ImGui::SetNextWindowBgAlpha(0.9f);
    ImGui::Begin("##mtimeline", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar);
    drawTimelineBody(true);
    const float tlH = ImGui::GetWindowSize().y;
    edgeLine({sx, tlBottom - tlH}, {sx + barW, tlBottom - tlH});
    ImGui::End();
    ImGui::PopStyleVar(2);

    freeMin_ = {sx, top + barH};
    freeMax_ = {sx + barW, tlBottom - tlH};
    static ImVec2 framedFor{0, 0};  // rotated: frame the organism again
    if (framedFor.x != io.DisplaySize.x || framedFor.y != io.DisplaySize.y) {
        framedFor = io.DisplaySize;
        previewAutoFrame_ = true;
    }

    // How-to card in the middle of the free area.
    if (cfg_.editorTips) {
        float w = std::min(520 * u, freeMax_.x - freeMin_.x - 16 * u);
        ImGui::SetNextWindowPos({(freeMin_.x + freeMax_.x) * 0.5f, freeMax_.y - 8 * u}, ImGuiCond_Always, {0.5f, 1.0f});
        ImGui::SetNextWindowSize({w, 0});
        ImGui::SetNextWindowBgAlpha(0.94f);
        ImGui::Begin("##mtips", nullptr, kDocked | ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::PushFont(fontBold_);
        ImGui::TextUnformatted("Как собрать организм");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("1. Коснитесь клетки — откроется вкладка «Клетка».");
        ImGui::TextUnformatted("2. Выберите тип новой клетки и нажмите «Разделить».");
        ImGui::TextUnformatted("3. Тяните полупрозрачного «призрака», чтобы повернуть деление. Шкала — время развития.");
        ImGui::TextDisabled("Вкладка «Мода» — все настройки выбранной моды. Двумя пальцами — сдвиг и масштаб.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Понятно")) {
            cfg_.editorTips = false;
            markConfigDirty();
        }
        ImGui::End();
    }
}
