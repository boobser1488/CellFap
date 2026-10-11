// Screens of the original game: SUBSTRATE / MICROSCOPE / GENOME (genome editor lives in ui_genome.cpp).
#include "app.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <thread>

using namespace uiw;

namespace {
constexpr float kNavH = 46.0f;
constexpr float kSideW = 360.0f;

const ImGuiWindowFlags kFixed = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;

// Text tab with an orange underline when active (the original's top tab bar).
bool navTab(const char* label, bool active, float h) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ts.x + 36.0f;
    bool pressed = ImGui::InvisibleButton(label, {w, h});
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 txt = active ? IM_COL32(255, 255, 255, 255) : (hov ? IM_COL32(230, 230, 230, 255) : IM_COL32(170, 172, 176, 255));
    dl->AddText({p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f - 1}, txt, label);
    if (active) dl->AddRectFilled({p.x + 10, p.y + h - 3}, {p.x + w - 10, p.y + h}, kAccentU32, 1.5f);
    else if (hov) dl->AddRectFilled({p.x + 14, p.y + h - 2}, {p.x + w - 14, p.y + h}, IM_COL32(255, 255, 255, 40));
    return pressed;
}
}  // namespace

// ============================================================================ frame
void App::drawUi() {
    gLabelW = 150.0f * uiScale_;
    gTooltips = cfg_.tooltips && !mobile_;  // no hover on a touch screen: explanations would pop up on every drag
    if (static int frames = 0; opt_.openCellLab && ++frames == 30) openInCellLab(opt_.openCellLab == 1 ? &ui_.editGenome : nullptr);  // test hook
    ImGuiIO& io = ImGui::GetIO();
    if (mobile_) {
        portrait_ = io.DisplaySize.y > io.DisplaySize.x;
        freeMin_ = {0, navBottom()};
        freeMax_ = io.DisplaySize;
        // The Back key (Escape) closes the open popup, else folds the sheet of the current screen.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) ImGui::ClosePopupToLevel(0, true);
            else if (genomeBrowser_) genomeBrowser_ = false;
            else if (expBrowser_) expBrowser_ = false;
            else if (ui_.screen == Screen::Microscope) dockMicro_.open = false;
            else if (ui_.screen == Screen::Genome) dockGenome_.open = false;
        }
        // Docked panels have no frame: a border along a screen edge reads as a stray line. Inner edges get a
        // divider drawn by the panel itself.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        drawMobileNav();
    } else {
        drawNavBar();
    }
    switch (ui_.screen) {
        case Screen::Microscope:
            if (mobile_) drawMobileMicroscope();
            else drawMicroscopeUi();
            break;
        case Screen::Substrate: drawSubstrateScreen(); break;
        case Screen::Genome: drawGenomeScreen(); break;
        case Screen::Settings: drawSettingsScreen(); break;
        case Screen::Challenges: drawChallengesScreen(); break;
    }
    drawGenomeBrowser();
    drawExperimentBrowser();
    drawOverlays();
    if (ui_.showHelp) drawHelp();
    if (mobile_) {
        touchScroll();
        ImGui::PopStyleVar();  // WindowBorderSize
    }
    resetLayout_ = false;
}

// ---------------------------------------------------------------------------- panel windows
bool App::beginPanel(const char* title, bool* open, ImVec2 pos, ImVec2 size, ImVec2 pivot, int extraFlags) {
    ImGuiCond cond = resetLayout_ ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    ImGui::SetNextWindowPos(pos, cond, pivot);
    if (size.x > 0 || size.y > 0) ImGui::SetNextWindowSize(size, cond);
    ImGuiWindowFlags f = ImGuiWindowFlags_NoFocusOnAppearing | extraFlags;
    if (cfg_.windowsLocked) f |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    bool wasOpen = open ? *open : true;
    bool visible = ImGui::Begin(title, open, f);
    if (open && wasOpen && !*open) markConfigDirty();
    // Keep the title bar reachable after a resolution change (e.g. leaving full screen).
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    float top = navBottom();
    ImVec2 np(std::clamp(wp.x, 60.0f - ws.x, std::max(0.0f, io.DisplaySize.x - 60.0f)),
              std::clamp(wp.y, top, std::max(top, io.DisplaySize.y - 30.0f)));
    if (np.x != wp.x || np.y != wp.y) ImGui::SetWindowPos(np);
    return visible;
}

void App::drawWindowsMenu() {
    auto item = [&](const char* name, bool* v, const char* key = nullptr) {
        if (ImGui::MenuItem(name, key, v)) markConfigDirty();
    };
    ImGui::TextDisabled("Микроскоп");
    item("Инструменты", &cfg_.winTools);
    item("Настройки инструмента", &cfg_.winToolOptions);
    item("Время и эксперименты", &cfg_.winTime);
    item("Клетка", &cfg_.winCell, "Tab");
    item("Статистика", &cfg_.winStats);
    ImGui::Separator();
    ImGui::TextDisabled("Редактор генома");
    item("Панель генома", &cfg_.winGenomeBar);
    item("Моды", &cfg_.winModes);
    item("Настройки моды", &cfg_.winMode);
    item("Развитие", &cfg_.winTimeline);
    ImGui::Separator();
    item("Закрепить окна", &cfg_.windowsLocked);
    if (ImGui::MenuItem("Сбросить расположение")) resetLayout_ = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Полный экран", "F11", fullscreen_)) {
        setFullscreen(!fullscreen_);
        markConfigDirty();
    }
}

// ---------------------------------------------------------------------------- navigation bar
void App::drawNavBar() {
    ImGuiIO& io = ImGui::GetIO();
    float h = kNavH * uiScale_;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({io.DisplaySize.x, h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14 * uiScale_, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.105f, 0.108f, 0.115f, 0.97f));
    ImGui::Begin("##nav", nullptr, kFixed);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine({0, h - 1}, {io.DisplaySize.x, h - 1}, IM_COL32(110, 190, 120, 70));

    // Logo
    ImGui::SetCursorPos({14 * uiScale_, 0});
    {
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled({p.x + 9 * uiScale_, p.y + h * 0.5f}, 7 * uiScale_, kAccentU32);
        dl->AddCircleFilled({p.x + 9 * uiScale_, p.y + h * 0.5f}, 3 * uiScale_, IM_COL32(40, 30, 20, 255));
        ImGui::Dummy({22 * uiScale_, h});
        ImGui::SameLine();
        ImGui::PushFont(fontBold_);
        ImGui::SetCursorPosY((h - ImGui::GetFontSize()) * 0.5f);
        ImGui::TextUnformatted("CellFap");
        ImGui::PopFont();
    }

    // Centered section tabs
    ImGui::PushFont(fontBold_);
    const char* names[] = {"СУБСТРАТ", "МИКРОСКОП", "ГЕНОМ", "НАСТРОЙКИ", "ЗАДАНИЯ"};
    float total = 0;
    for (const char* n : names) total += ImGui::CalcTextSize(n).x + 36.0f;
    ImGui::SameLine();
    ImGui::SetCursorPos({(io.DisplaySize.x - total) * 0.5f, 0});
    for (int i = 0; i < 5; ++i) {
        if (i) ImGui::SameLine(0, 0);
        if (navTab(names[i], (int)ui_.screen == i, h)) ui_.screen = (Screen)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("F%d", i + 2);
    }
    ImGui::PopFont();

    // Left (after the logo): window menu. Right: dish readouts + help.
    ImGui::SameLine();
    ImGui::SetCursorPos({150 * uiScale_, (h - ImGui::GetFrameHeight()) * 0.5f});
    if (ImGui::Button("Окна")) ImGui::OpenPopup("windows");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Показать или скрыть панели, сбросить их расположение");
    if (ImGui::BeginPopup("windows")) {
        drawWindowsMenu();
        ImGui::EndPopup();
    }
    char buf[128];
    {
        // With a day cycle the status line also says what time of day it is in the dish.
        const float dl = world_->daylight();
        const char* tod = world_->settings.dayLength <= 0.01f ? "" : dl > 0.85f ? "   ·   день" : dl < 0.05f ? "   ·   ночь" : "   ·   сумерки";
        std::snprintf(buf, sizeof(buf), "%.2f ч%s   ·   %d клеток   ·   %zu пищи", world_->timeHours(), tod, world_->cellCount(),
                      world_->food().size());
    }
    float tw = ImGui::CalcTextSize(buf).x;
    float bs = 28 * uiScale_;
    ImGui::SameLine();
    ImGui::SetCursorPos({io.DisplaySize.x - tw - bs - 30 * uiScale_, (h - ImGui::GetFontSize()) * 0.5f});
    ImGui::TextDisabled("%s", buf);
    ImGui::SameLine();
    ImGui::SetCursorPos({io.DisplaySize.x - bs - 12 * uiScale_, (h - bs) * 0.5f});
    if (iconButton("help", IcHelp, ui_.showHelp, "Управление (F1)", bs)) ui_.showHelp = !ui_.showHelp;
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
}

// ---------------------------------------------------------------------------- microscope
void App::drawMicroscopeUi() {
    if (cfg_.winTools) drawToolPalette();
    if (cfg_.winTime) drawTimeControls();
    drawToolOptions();
    drawSidePanel();
    drawChallengePanel();
}

void App::drawTimeControls() {
    ImGuiIO& io = ImGui::GetIO();
    float sideW = (kSideW + 24) * uiScale_;
    bool vis = beginPanel("Время###time", &cfg_.winTime, {io.DisplaySize.x - sideW - 12 * uiScale_, (kNavH + 12) * uiScale_}, {0, 0},
                          {1.0f, 0.0f}, ImGuiWindowFlags_AlwaysAutoResize);
    if (!vis) {
        ImGui::End();
        return;
    }
    float s = 32.0f * uiScale_;
    // The original's "substrate temperature": freeze / slow observe / observe / incubate.
    struct B {
        Speed sp;
        Icon ic;
        const char* tip;
    } btns[] = {{Speed::Freeze, IcFreeze, "Заморозка (Пробел)"},
                {Speed::Slow, IcSlow, "Медленное наблюдение"},
                {Speed::Observe, IcPlay, "Наблюдение"},
                {Speed::Incubate, IcFast, "Инкубация — максимальная скорость"}};
    for (auto& b : btns) {
        if (iconButton(b.tip, b.ic, ui_.speed == b.sp, b.tip, s)) {
            if (b.sp == Speed::Freeze && ui_.speed != Speed::Freeze) ui_.speedBeforeFreeze = ui_.speed;
            ui_.speed = b.sp;
        }
        ImGui::SameLine(0, 2 * uiScale_);
    }
    if (iconButton("step", IcStep, false, "Шаг: +0.05 ч", s)) {
        ui_.speed = Speed::Freeze;
        for (int i = 0; i < 20; ++i) world_->step();
    }
    ImGui::SameLine(0, 10 * uiScale_);
    if (ImGui::Button("Эксперименты", {0, s})) openExperimentBrowser();
    ImGui::SameLine();
    if (ImGui::Button("Стерилизовать", {0, s})) ImGui::OpenPopup("sterilize");
    ImGui::SameLine();
    if (ImGui::Button("Перезагрузить", {0, s})) reloadSubstrate();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Начать эту чашку заново: снова открыть загруженный файл, перезапустить эксперимент или задание");

    if (ui_.challenge >= 0 && ImGui::BeginPopup("sterilize")) {
        // Sterilising during a level would wipe the opponents too: here it means starting over.
        ImGui::TextUnformatted("В задании стерилизация перезапускает задание.\nКлетки соперников тоже появятся заново.");
        if (ImGui::Button("Начать задание заново", {240 * uiScale_, 0})) {
            startChallenge(ui_.challenge);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ui_.challenge < 0 && ImGui::BeginPopup("sterilize")) {
        ImGui::TextUnformatted("Удалить все клетки и пищу?");
        if (ImGui::Button("Стерилизовать", {200 * uiScale_, 0})) {
            world_->sterilize();
            track(-1);
            toast("Чашка стерилизована");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // Speed readout under the buttons
    ImGui::PushFont(fontSmall_);
    ImGui::TextDisabled("%.2f ч/с  ·  %.0f тиков/с", hoursPerSecond_, ticksPerSecond_);
    ImGui::PopFont();
    ImGui::End();
}

void App::drawToolPalette() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {7 * uiScale_, 8 * uiScale_});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4 * uiScale_, 4 * uiScale_});
    bool vis = beginPanel("Инструменты###tools", &cfg_.winTools, {12.0f * uiScale_, (io.DisplaySize.y + kNavH * uiScale_) * 0.5f},
                          {0, 0}, {0.0f, 0.5f}, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar);
    if (vis) {
        float s = 40.0f * uiScale_;
        struct T {
            Tool t;
            Icon ic;
            const char* tip;
        } tools[] = {
            {Tool::Inspect, IcInspect, "Диагностика (1)\nВыбор и отслеживание клетки"},
            {Tool::Synth, IcSynth, "Синтезатор клеток (2)\nВставить начальную клетку редактируемого генома"},
            {Tool::Tweezers, IcTweezers, "Оптический пинцет (3)\nПеретаскивание клеток"},
            {Tool::Boost, IcBoost, "Бустер (4)\nМакс. масса и азот; на пустом месте — частица пищи"},
            {Tool::Remove, IcRemove, "Удаление (5)\nУбивает клетку без остатка пищи"},
            {Tool::Shock, IcShock, "Электрод (6)\nИмпульс тока: возбуждение нервов и мышц, электропорация, лизис"},
            {Tool::GeneNeedle, IcGeneNeedle, "Генная микроигла (7)\nВводит в клетку (или весь организм) геном из редактора"},
            {Tool::DrugNeedle, IcDrugNeedle, "Микроигла для веществ (8)\nГлюкоза, нитраты, сигналы, яды, адреналин, протеаза, мутаген"},
            {Tool::Map, IcMap, "Редактор карты (9)\nРисовать стены и препятствия, о которые ударяются клетки"},
            {Tool::Adhesin, IcAdhesin, "Адгезин (0)\nСвязать клетки адгезином, склеить ткань или разрезать связи"},
        };
        // A grip to move the palette (it has no title bar).
        ImVec2 gp = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("grip", {s, 8 * uiScale_});
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f) && !cfg_.windowsLocked)
            ImGui::SetWindowPos({ImGui::GetWindowPos().x + io.MouseDelta.x, ImGui::GetWindowPos().y + io.MouseDelta.y});
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Перетащите, чтобы переместить панель");
        ImGui::GetWindowDrawList()->AddLine({gp.x + s * 0.3f, gp.y + 4 * uiScale_}, {gp.x + s * 0.7f, gp.y + 4 * uiScale_},
                                            IM_COL32(255, 255, 255, 60), 2.0f * uiScale_);
        for (auto& t : tools) {
            bool allowed = toolAllowed(t.t);
            if (!allowed) ImGui::BeginDisabled();
            if (iconButton(t.tip, t.ic, ui_.tool == t.t, allowed ? t.tip : "Недоступно в этом задании", s)) ui_.tool = t.t;
            if (!allowed) ImGui::EndDisabled();
        }
        ImGui::Separator();
        auto toggle = [&](const char* label, bool* v, const char* tip) {
            ImGui::PushStyleColor(ImGuiCol_Button, *v ? ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.25f) : ImVec4(1, 1, 1, 0.04f));
            ImGui::PushFont(fontSmall_);
            if (ImGui::Button(label, {s, 0})) {
                *v = !*v;
                markConfigDirty();
            }
            ImGui::PopFont();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        toggle("Связи", &cfg_.showSlime, "Показывать адгезины");
        toggle("Пища", &cfg_.showFood, "Показывать частицы пищи");
        toggle("Сетка", &cfg_.showGrid, "Сетка микроскопа");
        bool classic = cfg_.theme != 0;
        toggle("Тема", &classic, "Классический (лавандовый) или тёмный микроскоп");
        cfg_.theme = classic ? 1 : 0;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void App::drawToolOptions() {
    if (!cfg_.winToolOptions) return;
    float w = 330 * uiScale_;
    // Resizable in both directions; on first use it fits its content.
    ImGui::SetNextWindowSizeConstraints({240 * uiScale_, 0}, {900 * uiScale_, FLT_MAX});
    bool vis = beginPanel("Инструмент###toolopt", &cfg_.winToolOptions, {76 * uiScale_, (kNavH + 12) * uiScale_}, {w, 0}, {0, 0});
    if (vis) drawToolOptionsBody(ImGui::GetContentRegionAvail().x);
    ImGui::End();
}

// Options of the current tool (floating window on the desktop, a dock tab on the phone).
void App::drawToolOptionsBody(float w) {
    struct Info {
        const char* name;
        Icon ic;
        const char* what;
    };
    static const Info kInfo[(int)Tool::Count] = {
        {"Диагностика", IcInspect, "Выбор клетки: её состояние, сигналы и геном — в окне «Клетка»."},
        {"Синтезатор клеток", IcSynth, "Вставляет начальную клетку генома из редактора."},
        {"Оптический пинцет", IcTweezers, "Перетаскивание клеток лучом лазера."},
        {"Бустер", IcBoost, "Клеткам — максимум массы и азота; на пустом месте — бактерии (пища)."},
        {"Удаление", IcRemove, "Убирает клетки без остатка."},
        {"Электрод", IcShock, "Импульс тока из микроэлектрода. Действие зависит от наведённого напряжения на мембране."},
        {"Генная микроигла", IcGeneNeedle, "Вводит в клетку или весь организм геном из редактора."},
        {"Микроигла для веществ", IcDrugNeedle, "Вводит вещество в клетку или выпускает его в воду."},
        {"Редактор карты", IcMap, "Стены и препятствия, о которые ударяются клетки."},
        {"Адгезин", IcAdhesin, "Создаёт адгезиновые связи между клетками или разрезает их."},
    };
    const Info& inf = kInfo[clampv((int)ui_.tool, 0, (int)Tool::Count - 1)];
    gLabelW = std::min(118 * uiScale_, w * 0.42f);
    ImGui::Dummy({w, 0});  // fixed width: the window never jumps
    {
        float s = 30 * uiScale_;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x + s, p.y + s}, IM_COL32(255, 158, 51, 40), s * 0.25f);
        drawIcon(ImGui::GetWindowDrawList(), inf.ic, {p.x + s * 0.5f, p.y + s * 0.5f}, s * 0.62f, kAccentU32);
        ImGui::Dummy({s, s});
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(fontBold_);
        ImGui::TextUnformatted(inf.name);
        ImGui::PopFont();
        ImGui::PushFont(fontSmall_);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - s - 40 * uiScale_);
        ImGui::TextDisabled("%s", inf.what);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::EndGroup();
    }
    ImGui::Separator();

    auto segmented = [&](const char* const* labels, int n, int* v) {
        float bw = (ImGui::GetContentRegionAvail().x - (n - 1) * 4 * uiScale_) / n;
        for (int k = 0; k < n; ++k) {
            if (k) ImGui::SameLine(0, 4 * uiScale_);
            bool on = *v == k;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.38f));
            ImGui::PushID(labels[k]);
            if (ImGui::Button(labels[k], {bw, 0})) *v = k;
            ImGui::PopID();
            if (on) ImGui::PopStyleColor();
        }
    };
    // Phone: a grid of finger-sized buttons instead of a drop-down list (a list opened under the finger, scrolled
    // away from it and needed two taps).
    auto chips = [&](const char* const* labels, int n, int* v, int cols, const char* const* tips) {
        const float g = 4 * uiScale_;
        const float bw = (ImGui::GetContentRegionAvail().x - (cols - 1) * g) / cols;
        for (int k = 0; k < n; ++k) {
            if (k % cols) ImGui::SameLine(0, g);
            const bool on = *v == k;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.38f));
            ImGui::PushID(k);
            const bool small = ImGui::CalcTextSize(labels[k]).x + 2 * ImGui::GetStyle().FramePadding.x > bw;
            if (small) ImGui::PushFont(fontSmall_);
            if (ImGui::Button(labels[k], {bw, 0})) *v = k;
            if (small) ImGui::PopFont();
            if (tips && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tips[k]);
            ImGui::PopID();
            if (on) ImGui::PopStyleColor();
        }
    };
    auto radius = [&](const char* label) {
        sliderF(label, &ui_.brushRadius, 5.0f, 500.0f, "%.0f мкм", 0, false, ImGuiSliderFlags_Logarithmic);
        helpMarker("Ctrl + колесо мыши или клавиши [ и ] меняют радиус прямо над чашкой.");
    };
    auto areaToggle = [&] {
        const char* labels[] = {"Одна клетка", "Все в круге"};
        int v = ui_.brushArea ? 1 : 0;
        segmented(labels, 2, &v);
        ui_.brushArea = v == 1;
        if (ui_.brushArea) radius("Радиус");
    };
    auto note = [&](const char* text) {
        ImGui::PushFont(fontSmall_);
        ImGui::TextWrapped("%s", text);
        ImGui::PopFont();
    };

    switch (ui_.tool) {
        case Tool::Inspect:
            note(mobile_ ? "Коснитесь клетки, чтобы выбрать её. Одним пальцем — двигать чашку, двумя — двигать и масштабировать."
                         : "ЛКМ — выбрать клетку, F — следить, Del — удалить, Tab — окно «Клетка».");
            break;
        case Tool::Synth: {
            const Mode& m = ui_.editGenome.modes[ui_.editGenome.initialMode];
            rowLabel("Геном");
            ImGui::Text("«%s»", ui_.editGenome.name.c_str());
            rowLabel("Начальная клетка");
            ImGui::Text("M%d · %s", ui_.editGenome.initialMode + 1, cellTypeInfo(m.type).name);
            note(mobile_ ? "Коснитесь чашки — вставить клетку (2.6 нг, диаметр около 26 мкм)."
                         : "ЛКМ по чашке — вставить клетку (2.6 нг, диаметр около 26 мкм).");
            if (ImGui::Button("Выбрать геном…", {-FLT_MIN, 0})) {
                openGenomeBrowser();
                gbAfterOpen_ = Screen::Microscope;  // choosing a genome here keeps the synthesiser in hand
            }
            break;
        }
        case Tool::Tweezers: note("Удерживайте ЛКМ на клетке и ведите. Работает и при заморозке."); break;
        case Tool::Boost: areaToggle(); break;
        case Tool::Remove: areaToggle(); break;
        case Tool::Shock: {
            sliderF("Напряжение", &ui_.shockVolts, 0.02f, 100.0f, ui_.shockVolts < 1 ? "%.2f В" : "%.1f В", 0, false,
                    ImGuiSliderFlags_Logarithmic);
            sliderF("Длительность", &ui_.shockMs, 0.05f, 20.0f, "%.2f мс", 0, false, ImGuiSliderFlags_Logarithmic);
            radius("Зона поля");
            {
                const char* presets[] = {"Стимуляция", "Порация", "Лизис"};
                int p = -1;
                segmented(presets, 3, &p);
                if (p == 0) ui_.shockVolts = 0.12f, ui_.shockMs = 0.5f;
                if (p == 1) ui_.shockVolts = 1.2f, ui_.shockMs = 1.0f;
                if (p == 2) ui_.shockVolts = 12.0f, ui_.shockMs = 2.0f;
            }
            // Readouts in real units: field at the tip and the voltage induced across a typical membrane.
            float E0 = World::shockField(ui_.shockVolts, ui_.brushRadius, 0.0f);
            float eR = World::shockField(ui_.shockVolts, ui_.brushRadius, ui_.brushRadius);
            float psi0 = 1.5f * E0 * 13.0f, psiR = 1.5f * eR * 13.0f;
            auto verdict = [](float v, ImVec4* col) {
                if (v > 1.0f) { *col = {1.0f, 0.35f, 0.35f, 1}; return "лизис"; }
                if (v > 0.25f) { *col = {1.0f, 0.65f, 0.25f, 1}; return "электропорация"; }
                if (v > 0.015f) { *col = {0.55f, 0.85f, 1.0f, 1}; return "возбуждение"; }
                *col = {0.6f, 0.6f, 0.6f, 1};
                return "без эффекта";
            };
            rowLabel("Поле у кончика");
            ImGui::Text("%.0f В/см", E0 * 1e4f);
            ImVec4 c0, c1;
            const char* v0 = verdict(psi0, &c0);
            const char* v1 = verdict(psiR, &c1);
            rowLabel("Мембрана, центр");
            ImGui::Text("%.0f мВ", psi0 * 1000.0f);
            ImGui::SameLine();
            ImGui::TextColored(c0, "%s", v0);
            rowLabel("Мембрана, край");
            ImGui::Text("%.0f мВ", psiR * 1000.0f);
            ImGui::SameLine();
            ImGui::TextColored(c1, "%s", v1);
            note("Напряжение на мембране = 1.5·E·r (клетка диаметром 26 мкм). > 15 мВ: нейроны дают залп и замолкают, мышцы сводит судорогой, "
                 "жгутики встают. > 0.25 В: поры в мембране, вытекает клеточный сок. > 1 В: клетка лопается. "
                 "Кератин и жир изолируют.");
            break;
        }
        case Tool::GeneNeedle: {
            const char* modes[] = {"не менять", "выбранная в редакторе", "начальная"};
            rowLabel("Мода клетки");
            if (mobile_) {
                ImGui::NewLine();
                chips(modes, 3, &ui_.geneModeChoice, 3, nullptr);
            } else {
                ImGui::SetNextItemWidth(-FLT_MIN);
                ImGui::Combo("##gm", &ui_.geneModeChoice, modes, 3);
            }
            checkbox("Весь организм", &ui_.geneWholeOrganism);
            helpMarker("Геном получат все клетки, связанные адгезинами с уколотой. Иначе — только она и её потомки.");
            rowLabel("Геном");
            ImGui::Text("«%s» (M%d)", ui_.editGenome.name.c_str(), ui_.editMode + 1);
            break;
        }
        case Tool::DrugNeedle: {
            struct D {
                const char* name;
                const char* unit;
                float lo, hi;
                const char* desc;
            };
            static const D kDrugs[(int)World::Substance::Count] = {
                {"Глюкоза", "нг", 0.05f, 4.0f, "Питание: прибавляет массу клетке. В воде — питательный бульон."},
                {"Нитраты", "%", 1.0f, 100.0f, "Азот клетки (нужен для деления)."},
                {"Сигнальное вещество", "у.е.", -50.0f, 50.0f, "Добавляет (или убирает) сигнал выбранного канала."},
                {"Цианид", "нг", 0.02f, 2.0f, "Яд: ~1 нг убивает клетку. Кератин защищает."},
                {"Тетродотоксин", "нг", 0.02f, 2.0f, "Нейротоксин: 2 ч на нг. Нервы и органы чувств молчат, мышцы расслаблены."},
                {"Адреналин", "нг", 0.02f, 2.0f, "Стимулятор: 2 ч на нг. Жгутики, реснички и мышцы сильнее, обмен быстрее."},
                {"Протеаза", "нг", 0.02f, 1.0f, "Фермент: переваривает адгезины (0.2 нг освобождает клетку полностью)."},
                {"Регенерант", "нг", 0.05f, 2.0f, "Залечивает повреждения клетки."},
                {"Мутаген", "мутаций", 1.0f, 5.0f, "Мутирует геном клетки; потомки наследуют изменения."},
            };
            ui_.drug = clampv(ui_.drug, 0, (int)World::Substance::Count - 1);
            rowLabel("Вещество");
            if (mobile_) {
                ImGui::NewLine();
                const char* names[(int)World::Substance::Count];
                const char* tips[(int)World::Substance::Count];
                for (int i = 0; i < (int)World::Substance::Count; ++i) names[i] = kDrugs[i].name, tips[i] = kDrugs[i].desc;
                chips(names, (int)World::Substance::Count, &ui_.drug, 3, tips);
            } else {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##drug", kDrugs[ui_.drug].name)) {
                for (int i = 0; i < (int)World::Substance::Count; ++i) {
                    if (ImGui::Selectable(kDrugs[i].name, ui_.drug == i)) ui_.drug = i;
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kDrugs[i].desc);
                }
                ImGui::EndCombo();
            }
            }
            const D& d = kDrugs[ui_.drug];
            float& dose = ui_.drugDose[ui_.drug];
            char fmt[32];
            if (ui_.drug == (int)World::Substance::Mutagen) {
                dose = std::round(clampv(dose, 1.0f, 5.0f));
                std::snprintf(fmt, sizeof(fmt), "%%.0f %s", d.unit);
                sliderF("Доза", &dose, d.lo, d.hi, fmt, 1.0f, true);
            } else {
                std::snprintf(fmt, sizeof(fmt), d.hi <= 4.0f ? "%%.2f %s" : "%%.0f %s", d.unit);
                bool logScale = d.lo > 0;
                sliderF("Доза", &dose, d.lo, d.hi, fmt, 0, false, logScale ? ImGuiSliderFlags_Logarithmic : 0);
            }
            note(d.desc);
            if (ui_.drug == (int)World::Substance::Signal) {
                rowLabel("Канал");
                int ch = ui_.drugChannel;
                if (ImGui::SliderInt("##dch", &ch, 0, kSignalCount - 1, signalName(ch))) ui_.drugChannel = ch;
            }
            bool water = ui_.drug == (int)World::Substance::Glucose || ui_.drug == (int)World::Substance::Signal ||
                         ui_.drug == (int)World::Substance::Cyanide || ui_.drug == (int)World::Substance::Tetrodotoxin ||
                         ui_.drug == (int)World::Substance::Adrenaline || ui_.drug == (int)World::Substance::Protease;
            if (water) {
                const char* tl[] = {"В клетку", "В воду"};
                int v = ui_.drugIntoWater ? 1 : 0;
                segmented(tl, 2, &v);
                ui_.drugIntoWater = v == 1;
            } else {
                ui_.drugIntoWater = false;
            }
            if (ui_.drugIntoWater) note("ЛКМ выпускает облако вещества: оно расплывается, слабеет и действует на клетки, которых касается.");
            else {
                areaToggle();
                note("Удерживайте ЛКМ, чтобы вводить непрерывно (5 доз в секунду).");
            }
            break;
        }
        case Tool::Map: {
            const char* modes[] = {"Кисть", "Линия", "Рамка", "Кольцо", "Ластик"};
            int mm = (int)ui_.mapMode;
            segmented(modes, (int)MapMode::Count, &mm);
            ui_.mapMode = (MapMode)mm;
            if (ui_.mapMode == MapMode::Eraser) radius("Радиус ластика");
            else {
                float thick = ui_.mapThickness * 2.0f;
                if (sliderF("Толщина стены", &thick, 2.0f, 160.0f, "%.0f мкм", 0, false, ImGuiSliderFlags_Logarithmic))
                    ui_.mapThickness = thick * 0.5f;
            }
            rowLabel("Стен");
            ImGui::Text("%zu", world_->barriers().size());
            if (ImGui::Button("Убрать все стены", {-FLT_MIN, 0})) world_->clearBarriers();
            break;
        }
        case Tool::Adhesin: {
            const char* modes[] = {"Связать", "Склеить", "Разрезать"};
            int mm = (int)ui_.adhesinMode;
            segmented(modes, 3, &mm);
            ui_.adhesinMode = (AdhesinMode)mm;
            if (ui_.adhesinMode != AdhesinMode::Cut) {
                char f[32];
                std::snprintf(f, sizeof(f), "%.0f %%", ui_.adhesinStiffness * 100.0f);
                sliderF("Жёсткость", &ui_.adhesinStiffness, 0.0f, 1.0f, f);
                helpMarker("Угловая жёсткость: 0 — шарнир, 100% — связь держит угол между клетками.");
            }
            if (ui_.adhesinMode == AdhesinMode::Link) {
                sliderF("Запас длины", &ui_.adhesinSlack, 0.0f, 30.0f, "%.1f мкм");
                note("Нажмите на клетку и протяните к другой. Адгезин перекинется через текущий зазор (+ запас).");
            } else if (ui_.adhesinMode == AdhesinMode::Glue) {
                radius("Радиус");
                note("ЛКМ — соединить адгезинами все касающиеся клетки в круге (можно вести кистью).");
            } else {
                note("Проведите ЛКМ поперёк адгезинов — пересечённые связи разрываются.");
            }
            rowLabel("Адгезинов");
            ImGui::Text("%d", (int)std::count_if(world_->links().begin(), world_->links().end(), [](const Link& l) { return l.alive; }));
            break;
        }
        default: break;
    }
    gLabelW = 150.0f * uiScale_;
}

void App::drawSidePanel() {
    ImGuiIO& io = ImGui::GetIO();
    float w = kSideW * uiScale_, m = 12.0f * uiScale_, top = kNavH * uiScale_ + m;
    float split = (io.DisplaySize.y - top - m) * 0.62f;
    if (cfg_.winCell) {
        if (beginPanel("Клетка###cell", &cfg_.winCell, {io.DisplaySize.x - w - m, top}, {w, split}, {0, 0})) drawCellTab();
        ImGui::End();
    }
    if (cfg_.winStats) {
        float y = top + split + m;
        if (beginPanel("Статистика###stats", &cfg_.winStats, {io.DisplaySize.x - w - m, y}, {w, io.DisplaySize.y - y - m}, {0, 0}))
            drawStatsTab();
        ImGui::End();
    }
}

// ---------------------------------------------------------------------------- substrate
void App::drawSubstrateScreen() {
    ImGuiIO& io = ImGui::GetIO();
    Settings& s = world_->settings;
    bool snap = ui_.snap;
    float top = navBottom();
    // Dim the dish behind the settings (it keeps running, as in the original).
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, top}, io.DisplaySize, IM_COL32(16, 17, 19, mobile_ ? 240 : 150));

    float maxW = std::min(io.DisplaySize.x - (mobile_ ? 12 * uiScale_ + safe_.x + safe_.z : 40 * uiScale_), 1240.0f * uiScale_);
    ImGui::SetNextWindowPos({(io.DisplaySize.x - maxW + (mobile_ ? safe_.x - safe_.z : 0.0f)) * 0.5f, top + (mobile_ ? 6 : 16) * uiScale_});
    ImGui::SetNextWindowSize({maxW, io.DisplaySize.y - top - (mobile_ ? 12 : 32) * uiScale_});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##substrate", nullptr, kFixed);
    ImGui::PopStyleVar();
    // During a level the dish settings are part of the puzzle: look, do not touch.
    const bool locked = ui_.challenge >= 0;
    if (locked) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.35f, 1.0f), "Идёт задание: настройки субстрата заблокированы.");
        ImGui::BeginDisabled();
    }
    const int cols = fitColumns(ImGui::GetContentRegionAvail().x, 330 * uiScale_, 3);
    float colW = (ImGui::GetContentRegionAvail().x - (cols - 1) * 14 * uiScale_) / cols;
    gLabelW = std::min(150.0f * uiScale_, colW * 0.45f);

    auto card = [&](const char* id, float h, auto&& body) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.115f, 0.118f, 0.126f, 0.96f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * uiScale_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14 * uiScale_, 10 * uiScale_});
        ImGui::BeginChild(id, {colW, h}, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        body();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    };

    ImGui::BeginGroup();  // column 1
    card("light", 0, [&] {
        sectionHeader("Свет");
        sliderF("Количество", &s.lightAmount, 0.0f, 2.0f, "%.2f");
        sliderF("Освещённая зона", &s.lightRange, 0.0f, 1.0f, "%.2f");
        sliderF("Вращение света", &s.lightDirChange, 0.0f, 1.0f, "%.2f");
        sliderAngle("Направление", &s.lightAngle, snap);
        {
            float pct = s.lightCapacity * 100.0f;
            if (sliderF("Ёмкость света", &pct, 2.0f, 100.0f, "%.0f %%", 0, false, ImGuiSliderFlags_Logarithmic)) s.lightCapacity = pct / 100.0f;
        }
        helpMarker("Какую долю чашки могут покрыть фотосинтезирующие клетки, прежде чем начнут затенять друг друга. Меньше — водоросли не зарастают всю чашку.");
        sliderF("Сутки", &s.dayLength, 0.0f, 48.0f, s.dayLength <= 0.01f ? "вечный день" : "%.0f ч", 1.0f, true);
        helpMarker("Смена дня и ночи: свет гаснет и загорается с этим периодом. Ночью фотоциты голодают (пригодятся "
                   "липоциты), а светящиеся люминоциты видно издалека. 0 — вечный день.");
    });
    ImGui::Dummy({0, 6 * uiScale_});
    card("food", 0, [&] {
        sectionHeader("Питательные вещества");
        sliderF("Скорость", &s.nutrientRate, 0.0f, 15.0f, "%.1f");
        sliderF("Размер частиц", &s.chunkSize, 0.022f, 1.152f, "%.3f");
        sliderF("Комковатость", &s.lumpiness, 0.0f, 100.0f, "%.0f");
        sliderF("Размер комков", &s.lumpSize, 0.01f, 0.2f, "%.3f");
        sliderF("Покрытие", &s.coating, 0.0f, 1.0f, "%.2f");
        helpMarker("Доля покрытых частиц. Фагоциты едят только непокрытую пищу; покрытие снимает липаза.");
        checkbox("Подвижная пища", &s.mobileFood);
        helpMarker("Клетки расталкивают частицы пищи.");
        sliderF("Утечка сока", &s.sapLeak, 0.0f, 2.0f, "%.2f");
        helpMarker("Раненые клетки теряют цитоплазму в воду, лопнувшие — выплёскивают её. Сок питателен для соседей.");
    });
    ImGui::EndGroup();
    if (1 % cols != 0) ImGui::SameLine(0, 14 * uiScale_);

    ImGui::BeginGroup();  // column 2
    card("env", 0, [&] {
        sectionHeader("Среда");
        checkbox("Физика Cell Lab", &s.cellLabPhysics);
        helpMarker("Физика и биология как в Cell Lab / MinLab: мягкие клетки сплющиваются и отскакивают, плывут по инерции, "
                   "связи крепятся к оболочкам, свои расходы, пища и сигналы. Включается сама для чашек из Cell Lab; "
                   "организмы CellFap рассчитаны на обычную физику.");
        sliderF("Вязкость", &s.viscosity, 0.0f, 150.0f, "%.0f");
        sliderF("Динамич. трение", &s.dynFriction, 0.0f, 8.0f, "%.2f");
        sliderF("Статич. трение", &s.staticFriction, 0.0f, 8.0f, "%.2f");
        sliderF("Солёность", &s.salinity, 0.0f, 1.0f, "%.2f");
        helpMarker("Чем выше солёность, тем медленнее клетки расходуют массу (до 14 раз).");
        sliderF("Нитраты", &s.nitrates, 0.0f, 1.0f, "%.2f");
        helpMarker("Восполнение азота из среды. Без азота клетки не делятся (<20%) и гибнут (<10%).");
        sliderF("Гравитация", &s.gravity, 0.0f, 1.0f, "%.2f");
        sliderF("Плотность", &s.density, -3.3f, 3.3f, "%.2f");
        sliderF("Градиент", &s.densityGradient, -2.0f, 0.0f, "%.2f");
        sliderF("Механический урон", &s.mechanicalDamage, 0.0f, 1.0f, "%.2f");
        helpMarker("Сильное сдавливание, удары и разрыв адгезинов ранят клетки. Кератиноциты защищают, жёсткий цитоскелет тоже. 0 — выключено.");
        sliderF("Плавание извиванием", &s.swimAnisotropy, 1.0f, 6.0f, "%.1f");
        helpMarker("Во сколько раз клетке в составе тела труднее двигаться боком, чем вдоль своей оси. "
                   "Благодаря этому изгибающиеся организмы отталкиваются от воды, как черви. 1 — выключено.");
    });
    ImGui::Dummy({0, 6 * uiScale_});
    card("life", 0, [&] {
        sectionHeader("Мутации и жизнь");
        sliderF("Радиация", &s.radiation, 0.0f, 0.09f, "%.3f");
        helpMarker("Вызывает мутации при делении и понемногу отнимает массу.");
        checkbox("Только точечные", &s.onlyPointMutations);
        checkbox("Старение (240 ч)", &s.cellAging);
        checkbox("Случайные клетки", &s.contaminate);
        checkbox("Гибель у стенки", &s.killAtEdge);
        checkbox("Крышка", &s.lid);
    });
    ImGui::EndGroup();
    if (2 % cols != 0) ImGui::SameLine(0, 14 * uiScale_);

    ImGui::BeginGroup();  // column 3
    card("pool", 0, [&] {
        sectionHeader("Генофонд");
        ImGui::PushFont(fontSmall_);
        if (ImGui::BeginTable("pool", 2)) {
            for (int t = 0; t < kCellTypeCount; ++t) {
                ImGui::TableNextColumn();
                ImGui::PushID(t);
                ImGui::Checkbox(cellTypeInfo(t).name, &s.genePool[t]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::PopFont();
    });
    ImGui::Dummy({0, 6 * uiScale_});
    card("dish", 0, [&] {
        sectionHeader("Чашка");
        float diam = s.dishRadius * 2.0f / 1000.0f;
        if (sliderF("Диаметр, мм", &diam, 0.4f, 12.0f, "%.2f")) s.dishRadius = diam * 500.0f;
        rowLabel("Макс. клеток");
        ImGui::SliderInt("##maxc", &s.maxCells, 100, 100000, "%d", ImGuiSliderFlags_Logarithmic);
        rowLabel("Макс. пищи");
        ImGui::SliderInt("##maxf", &s.maxFood, 0, 100000, "%d", ImGuiSliderFlags_Logarithmic);
        sectionHeader("Сохранение");
        rowLabel("Имя файла");
        ImGui::InputText("##sf", ui_.substrateFile, sizeof(ui_.substrateFile));
        float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Сохранить", {bw, 0})) {
            std::string path = dataDir("substrates") + "/" + safeFileName(ui_.substrateFile, "чашка") + ".substrate";
            toast(saveWorld(*world_, path.c_str()) ? "Чашка сохранена" : "Ошибка сохранения");
        }
        ImGui::SameLine();
        if (ImGui::Button("Загрузить…", {bw, 0})) ImGui::OpenPopup("loadsub");
        if (dropButton("Поделиться чашкой", {-FLT_MIN, 0})) ImGui::OpenPopup("subshare");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Файл чашки и сообщение для мессенджера (на ПК файл откроется в проводнике, текст — в буфере обмена)");
        if (mobile_) ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {10 * uiScale_, 18 * uiScale_});  // finger-sized rows
        if (ImGui::BeginPopup("subshare")) {
            dishShareItems();
            ImGui::EndPopup();
        }
        if (mobile_) ImGui::PopStyleVar();
        if (mobile_ && ImGui::Button("Сохранить в файл на телефоне…", {-FLT_MIN, 0})) {
            std::string path = dataDir("substrates") + "/" + safeFileName(ui_.substrateFile, "чашка") + ".substrate";
            if (saveWorld(*world_, path.c_str())) exportToDevice(path, safeFileName(ui_.substrateFile, "чашка") + ".substrate");
            else toast("Ошибка сохранения");
        }
        if (ImGui::Button("Открыть файл…", {-FLT_MIN, 0})) {
            std::string p = pickFile(L"Чашки CellFap и Cell Lab (*.substrate)\0*.substrate\0Геномы (*.genome, *.genome.txt)\0*.genome;*.genome.txt\0Все файлы\0*.*\0",
                                     L"Открыть чашку или геном");
            if (!p.empty()) importPath(p);
        }
        if (ImGui::BeginPopup("loadsub")) {
            auto files = listFiles(dataDir("substrates"), ".substrate");
            if (files.empty()) ImGui::TextDisabled("Нет сохранённых чашек");
            for (auto& f : files)
                if (ImGui::Selectable(f.c_str())) importPath(dataDir("substrates") + "/" + f);
            ImGui::EndPopup();
        }
        sectionHeader("Импорт Cell Lab");
        ImGui::PushFont(fontSmall_);
        ImGui::TextWrapped("Кнопка «Открыть файл…» выше открывает и чашки, и геномы Cell Lab. Можно и перетащить файл в окно, вставить путь или положить файлы в папку import.");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##imp", "путь к файлу .substrate или .genome", importPathBuf_, sizeof(importPathBuf_));
        if (ImGui::Button("Импортировать", {-FLT_MIN, 0}) && importPathBuf_[0]) importPath(importPathBuf_);
        {
            std::string dir = dataDir("import");
            for (auto& f : listFiles(dir, ".substrate"))
                if (ImGui::Selectable(("чашка: " + f).c_str())) importPath(dir + "/" + f);
            for (auto& f : listFiles(dir, ".genome"))
                if (ImGui::Selectable(("геном: " + f).c_str())) importPath(dir + "/" + f);
        }
        sectionHeader("Эксперименты");
        const auto& exps = experiments();
        for (int i = 0; i < (int)exps.size(); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(exps[i].name)) {
                loadExperiment(i);
                ui_.screen = Screen::Microscope;
                toast(std::string("Эксперимент: ") + exps[i].name);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", exps[i].description);
            ImGui::PopID();
        }
    });
    ImGui::EndGroup();
    if (locked) ImGui::EndDisabled();
    ImGui::End();
    gLabelW = 150.0f * uiScale_;
}

// ---------------------------------------------------------------------------- cell diagnostics
void App::drawCellTab() {
    int ti = trackedCell();
    if (ti < 0) {
        ImGui::Dummy({0, 12});
        ImGui::TextWrapped("Выберите клетку инструментом «Диагностика» (1), чтобы увидеть её состояние.");
        return;
    }
    const Cell& c = world_->cells()[ti];
    const Mode& m = world_->modeOf(c);
    const Genome& g = world_->genomes().get(c.genome);

    ImGui::Dummy({0, 4});
    colorSwatch(m.color, 22 * uiScale_);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(fontBold_);
    if (g.labels[c.mode].empty()) ImGui::Text("%s  ·  M%d", cellTypeInfo(c.type).name, c.mode + 1);
    else ImGui::Text("%s  ·  M%d %s", cellTypeInfo(c.type).name, c.mode + 1, g.labels[c.mode].c_str());
    ImGui::PopFont();
    ImGui::PushFont(fontSmall_);
    ImGui::TextDisabled("#%u · %s · «%s»", c.id, tagName(c.tag), g.name.c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(ImVec4(0.75f, 0.70f, 0.60f, 1.0f), "%s", cellTypeQuip(c.type));
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndGroup();

    sectionHeader("Состояние");
    char buf[64];
    float cap = c.type == CellType::Lipocyte ? kLipocyteCapacity : kMaxCellMass;
    std::snprintf(buf, sizeof(buf), "%.3f нг", c.mass);
    valueBar("Масса", c.mass / cap, buf, IM_COL32(255, 158, 51, 150));
    std::snprintf(buf, sizeof(buf), "%.0f %%", c.nitrogen * 100.0f);
    valueBar("Азот", c.nitrogen, buf, IM_COL32(110, 160, 230, 150));
    std::snprintf(buf, sizeof(buf), "%.0f %%", c.injury * 100.0f);
    valueBar("Повреждение", c.injury, buf, IM_COL32(120, 210, 90, 150));
    std::snprintf(buf, sizeof(buf), "%.2f ч", c.age);
    valueBar("Возраст", world_->settings.cellAging ? c.age / kMaxAge : std::min(c.age / 10.0f, 1.0f), buf, IM_COL32(200, 200, 210, 70));
    rowLabel("Диаметр");
    ImGui::Text("%.1f мкм", c.radius * 2.0f);
    rowLabel("Связи");
    ImGui::Text("%d / %d", c.linkCount, m.maxConnections);
    rowLabel("Мутации");
    ImGui::Text("%d", c.mutations);
    if (c.telomeres >= 0) {
        rowLabel("Теломеры");
        ImGui::Text("%d", c.telomeres);
    }
    if (c.type == CellType::Photocyte) {
        rowLabel("Свет");
        ImGui::Text("%.2f", world_->lightAt(c.pos));
    }
    if (c.type == CellType::Electrocyte || c.type == CellType::Cnidocyte) {
        std::snprintf(buf, sizeof(buf), "%.0f %%", c.charge * 100.0f);
        valueBar(c.type == CellType::Electrocyte ? "Заряд" : "Нить", c.charge, buf, IM_COL32(120, 170, 255, 150));
    }
    if (c.type == CellType::Chromatocyte) {
        std::snprintf(buf, sizeof(buf), "%.0f %%", c.actuator * 100.0f);
        valueBar("Пигмент", c.actuator, buf, IM_COL32(200, 120, 80, 150));
    }
    if (c.type == CellType::Luminocyte) {
        std::snprintf(buf, sizeof(buf), "%.0f %%", c.actuator * 100.0f);
        valueBar("Свечение", c.actuator, buf, IM_COL32(200, 255, 220, 150));
    }
    if (c.type == CellType::Grappler) {
        int held = 0;
        for (int k = 0; k < c.linkCount; ++k) held += !world_->links()[c.links[k]].fromDivision;
        rowLabel("Хватка");
        ImGui::Text("%s, держит: %d", c.actuator > 0.5f ? "сжата" : "разжата", held);
    }
    if (c.type == CellType::Cystocyte) {
        rowLabel("Циста");
        ImGui::TextUnformatted("спит");
    }
    // Drug and electric after-effects (hours left).
    auto effect = [&](const char* name, float hLeft, ImU32 col) {
        if (hLeft <= 0) return;
        std::snprintf(buf, sizeof(buf), "%.0f мин", hLeft * 60.0f);
        valueBar(name, std::min(hLeft, 1.0f), buf, col);
    };
    effect("Судорога / рефрактерность", c.stun, IM_COL32(140, 190, 255, 150));
    effect("Паралич", c.paralysis, IM_COL32(170, 120, 220, 150));
    effect("Стимуляция", c.stimulant, IM_COL32(240, 110, 90, 150));

    sectionHeader("Сигналы");
    for (int s = 0; s < kSignalCount; ++s) {
        std::snprintf(buf, sizeof(buf), "%.2f", c.signals[s]);
        valueBar(signalName(s), 0.5f + c.signals[s] / 40.0f, buf, s % 2 ? IM_COL32(110, 160, 230, 120) : IM_COL32(230, 200, 90, 120));
    }

    sectionHeader("Действия");
    float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button(ui_.follow ? "Не следить (F)" : "Следить (F)", {bw, 0})) ui_.follow = !ui_.follow;
    ImGui::SameLine();
    if (ImGui::Button("Геном в редактор", {bw, 0})) {
        commitGenome();
        ui_.editGenome = g;
        ui_.editGenome.initialMode = c.mode;
        ui_.editMode = c.mode;
        ui_.previewDirty = true;
        editorTick_ = 0;
        ui_.screen = Screen::Genome;
    }
    if (ImGui::Button("Буст", {bw, 0})) world_->boostCell(ti);
    ImGui::SameLine();
    if (ImGui::Button("Удалить (Del)", {bw, 0})) {
        world_->removeCell(ti, false);
        track(-1);
    }
}

// ---------------------------------------------------------------------------- statistics
void App::drawStatsTab() {
    const auto& hist = world_->history();
    sectionHeader("Популяция");
    float w = ImGui::GetContentRegionAvail().x, h = 160.0f * uiScale_;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, IM_COL32(255, 255, 255, 8), 6);
    if (hist.size() >= 2) {
        int order[kCellTypeCount];
        for (int i = 0; i < kCellTypeCount; ++i) order[i] = i;
        const Stats& last = hist.back();
        std::sort(order, order + kCellTypeCount, [&](int a, int b) { return last.perType[a] > last.perType[b]; });
        float maxV = 1;
        for (const Stats& s : hist) maxV = std::max(maxV, (float)s.cells);
        float t0 = hist.front().time, t1 = std::max(hist.back().time, t0 + 1e-3f);
        auto pt = [&](float t, float v) { return ImVec2(p.x + 6 + (w - 12) * (t - t0) / (t1 - t0), p.y + h - 6 - (h - 18) * v / maxV); };
        for (int k = 1; k < 4; ++k) {
            float y = p.y + 6 + (h - 18) * k / 4.0f;
            dl->AddLine({p.x + 6, y}, {p.x + w - 6, y}, IM_COL32(255, 255, 255, 10));
        }
        std::vector<ImVec2> pts;
        for (int k = 0; k < 5; ++k) {
            int t = order[k];
            if (last.perType[t] == 0) break;
            pts.clear();
            for (const Stats& s : hist) pts.push_back(pt(s.time, (float)s.perType[t]));
            const CellTypeInfo& info = cellTypeInfo(t);
            dl->AddPolyline(pts.data(), (int)pts.size(), colU32({info.r, info.g, info.b}, 0.85f), 0, 1.5f * uiScale_);
        }
        pts.clear();
        for (const Stats& s : hist) pts.push_back(pt(s.time, (float)s.cells));
        dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(240, 245, 248, 230), 0, 2.0f * uiScale_);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.0f", maxV);
        ImGui::PushFont(fontSmall_);
        dl->AddText({p.x + 8, p.y + 4}, IM_COL32(200, 205, 210, 160), buf);
        std::snprintf(buf, sizeof(buf), "%.1f ч", t1);
        dl->AddText({p.x + w - ImGui::CalcTextSize(buf).x - 8, p.y + h - 18 * uiScale_}, IM_COL32(200, 205, 210, 160), buf);
        ImGui::PopFont();
    } else {
        dl->AddText({p.x + 10, p.y + 10}, IM_COL32(200, 205, 210, 120), "Сбор данных…");
    }
    ImGui::Dummy({w, h});

    sectionHeader("Типы клеток");
    int total = std::max(1, world_->cellCount());
    for (int t = 0; t < kCellTypeCount; ++t) {
        int n = world_->countOfType(t);
        if (!n) continue;
        const CellTypeInfo& info = cellTypeInfo(t);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", n);
        valueBar(info.name, (float)n / total, buf, colU32({info.r, info.g, info.b}, 0.55f));
    }
    rowLabel("Живых геномов");
    ImGui::Text("%d", world_->genomes().liveCount());

    sectionHeader("Производительность");
    rowLabel("Кадры");
    ImGui::Text("%.0f FPS", fps_);
    rowLabel("Симуляция");
    ImGui::Text("%.1f мс/кадр", simMsPerFrame_);
    rowLabel("GPU");
    ImGui::TextUnformatted(ctx_.gpuName());
    rowLabel("Потоки");
    ImGui::Text("%u", jobs_->threadCount());
    bool vs = ctx_.vsync();
    if (checkbox("Вертик. синхр.", &vs)) ctx_.setVsync(vs);
}

// ---------------------------------------------------------------------------- overlays
void App::drawOverlays() {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    ImGui::PushFont(fontSmall_);
    const bool classic = cfg_.theme != 0;
    const ImU32 ink = classic ? IM_COL32(40, 40, 60, 230) : IM_COL32(220, 228, 232, 220);

    if (ui_.screen == Screen::Microscope && !genomeBrowser_ && !expBrowser_) {  // nothing over a full-screen browser
        const char* hint = "";
        switch (ui_.tool) {
            case Tool::Inspect: hint = "ЛКМ — выбрать клетку · ПКМ/колесо — перемещение и зум · F — следить"; break;
            case Tool::Synth: hint = "ЛКМ — вставить клетку из редактора генома"; break;
            case Tool::Tweezers: hint = "Удерживайте ЛКМ на клетке и тяните"; break;
            case Tool::Boost: hint = "ЛКМ по клетке — буст · ЛКМ по пустому месту — частица пищи"; break;
            case Tool::Remove: hint = "ЛКМ (можно с протяжкой) — удалить клетки"; break;
            case Tool::Shock: hint = "ЛКМ — импульс тока · Ctrl+колесо или [ ] — зона поля"; break;
            case Tool::GeneNeedle: hint = "ЛКМ по клетке — ввести геном из редактора"; break;
            case Tool::DrugNeedle: hint = "ЛКМ по клетке — ввести вещество (удерживайте для непрерывного ввода)"; break;
            case Tool::Map: hint = "ЛКМ — рисовать стену (режим и толщина — в окне инструмента) · ПКМ — перемещение"; break;
            case Tool::Adhesin: hint = "Связать: протяните от клетки к клетке · Склеить: ЛКМ по ткани · Разрезать: проведите поперёк связей"; break;
            default: break;
        }
        if (cfg_.showHints && !mobile_) {  // the phone shows the tool description in its sheet tab
            ImVec2 ts = ImGui::CalcTextSize(hint);
            ImVec2 hp(16 * uiScale_, io.DisplaySize.y - ts.y - 14 * uiScale_);
            fg->AddRectFilled({hp.x - 8, hp.y - 5}, {hp.x + ts.x + 8, hp.y + ts.y + 5}, IM_COL32(24, 25, 28, 210), 8);
            fg->AddText(hp, IM_COL32(200, 204, 210, 235), hint);
        }

        // Scale bar
        float targetPx = 120.0f * uiScale_;
        float worldLen = targetPx / zoom_;
        float p10 = std::pow(10.0f, std::floor(std::log10(worldLen)));
        float nice = worldLen / p10 >= 5 ? 5 * p10 : (worldLen / p10 >= 2 ? 2 * p10 : p10);
        float px = nice * zoom_;
        const float barCx = mobile_ ? (freeMin_.x + freeMax_.x) * 0.5f : io.DisplaySize.x * 0.5f;
        const float barY = (mobile_ ? freeMax_.y : io.DisplaySize.y) - 22 * uiScale_;
        ImVec2 a(barCx - px * 0.5f, barY);
        fg->AddLine(a, {a.x + px, a.y}, ink, 2.0f);
        fg->AddLine({a.x, a.y - 4}, {a.x, a.y + 4}, ink, 2.0f);
        fg->AddLine({a.x + px, a.y - 4}, {a.x + px, a.y + 4}, ink, 2.0f);
        char buf[32];
        if (nice >= 1000) std::snprintf(buf, sizeof(buf), "%.0f мм", nice / 1000);
        else std::snprintf(buf, sizeof(buf), "%.0f мкм", nice);
        ImVec2 bs = ImGui::CalcTextSize(buf);
        fg->AddText({a.x + (px - bs.x) * 0.5f, a.y - bs.y - 4}, ink, buf);

        if (ui_.hoveredIdx >= 0 && !io.WantCaptureMouse) {
            const Cell& c = world_->cells()[ui_.hoveredIdx];
            char lb[96];
            std::snprintf(lb, sizeof(lb), "%s · M%d · %.2f нг", cellTypeInfo(c.type).name, c.mode + 1, c.mass);
            ImVec2 mp = io.MousePos;
            ImVec2 ls = ImGui::CalcTextSize(lb);
            fg->AddRectFilled({mp.x + 14, mp.y + 14}, {mp.x + 26 + ls.x, mp.y + 22 + ls.y}, IM_COL32(24, 25, 28, 225), 6);
            fg->AddText({mp.x + 20, mp.y + 18}, IM_COL32(235, 238, 242, 255), lb);
        }
    }
    ImGui::PopFont();

    if (time_ < ui_.toastUntil && !ui_.toast.empty()) {
        float a = std::min(1.0f, (float)(ui_.toastUntil - time_) * 2.0f);
        // Long messages wrap on narrow screens; on the phone the message sits above the docked panels.
        const float wrap = std::min(io.DisplaySize.x - 48 * uiScale_, 900 * uiScale_);
        ImVec2 s = ImGui::CalcTextSize(ui_.toast.c_str(), nullptr, false, wrap);
        const float cx = mobile_ ? (freeMin_.x + freeMax_.x) * 0.5f : io.DisplaySize.x * 0.5f;
        const float by = mobile_ ? freeMax_.y - 60 * uiScale_ - s.y : io.DisplaySize.y - 140 * uiScale_;
        ImVec2 p(cx - s.x * 0.5f, by);
        fg->AddRectFilled({p.x - 16, p.y - 9}, {p.x + s.x + 16, p.y + s.y + 9}, IM_COL32(28, 29, 32, (int)(235 * a)), 10);
        fg->AddRect({p.x - 16, p.y - 9}, {p.x + s.x + 16, p.y + s.y + 9}, IM_COL32(255, 158, 51, (int)(160 * a)), 10);
        fg->AddText(ImGui::GetFont(), ImGui::GetFontSize(), p, IM_COL32(240, 242, 245, (int)(255 * a)), ui_.toast.c_str(), nullptr, wrap);
    }

    if (ui_.showPerf) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%.0f FPS · sim %.1f мс · %.0f тиков/с · %d клеток · фазы %.2f/%.2f/%.2f/%.2f мс", fps_,
                      simMsPerFrame_, ticksPerSecond_, world_->cellCount(), world_->phaseMs[2], world_->phaseMs[4],
                      world_->phaseMs[5], world_->phaseMs[0]);
        fg->AddText({16 * uiScale_, (kNavH + 10) * uiScale_}, classic ? IM_COL32(40, 40, 70, 255) : IM_COL32(200, 230, 220, 255), buf);
    }
}

void App::drawHelp() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({std::min(560 * uiScale_, io.DisplaySize.x - 16 * uiScale_), 0});
    if (ImGui::Begin("Управление", &ui_.showHelp, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        auto row = [&](const char* k, const char* v) {
            ImGui::PushFont(fontBold_);
            ImGui::TextUnformatted(k);
            ImGui::PopFont();
            ImGui::SameLine(180 * uiScale_);
            ImGui::TextWrapped("%s", v);
        };
        sectionHeader("Везде");
        row("F2 / F3 / F4 / F5", "Субстрат / Микроскоп / Геном / Настройки");
        row("F1", "Эта справка");
        row("F11", "Полный экран");
        row("F12", "Снимок экрана (папка screenshots)");
        row("Окна", "Меню в верхней панели: показать и скрыть панели. Панели перетаскиваются за заголовок, размер — за край");
        sectionHeader("Микроскоп");
        row("Колесо", "Зум к курсору");
        row("ПКМ / СКМ, WASD", "Перемещение");
        row("Пробел", "Заморозка / продолжение");
        row("1 – 9", "Диагностика, синтезатор, пинцет, бустер, удаление, электрошок, генная игла, игла для веществ, редактор карты");
        row("Tab", "Окно «Клетка»");
        row("F / Del / Home", "Следить · удалить клетку · вся чашка");
        sectionHeader("Редактор генома");
        row("ЛКМ по клетке", "Действия: разделить или изменить");
        row("ЛКМ по призраку", "Изменить / удалить деление; перетаскивание — направление");
        row("ПКМ по клетке", "Повторить последнее действие");
        row("← → / Shift", "Тик / стадия;  Пробел — в конец");
        row("Ctrl+Z / Ctrl+Y", "Отменить / повторить");
        row("Ctrl+S", "Сохранить геном в банк генов");
        row("PgUp / PgDn", "Предыдущая / следующая мода");
        row("Ctrl+D", "Копия выбранной моды в свободный номер");
        row("Колесо, ЛКМ по пустому", "Зум и перемещение; Home — вписать");
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------- settings
void App::drawSettingsScreen() {
    ImGuiIO& io = ImGui::GetIO();
    AppConfig& c = cfg_;
    float top = navBottom();
    ImGui::GetBackgroundDrawList()->AddRectFilled({0, top}, io.DisplaySize, IM_COL32(16, 17, 19, mobile_ ? 240 : 170));

    float maxW = std::min(io.DisplaySize.x - (mobile_ ? 12 * uiScale_ + safe_.x + safe_.z : 40 * uiScale_), 1320.0f * uiScale_);
    ImGui::SetNextWindowPos({(io.DisplaySize.x - maxW + (mobile_ ? safe_.x - safe_.z : 0.0f)) * 0.5f, top + (mobile_ ? 6 : 16) * uiScale_});
    ImGui::SetNextWindowSize({maxW, io.DisplaySize.y - top - (mobile_ ? 12 : 32) * uiScale_});
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##settings", nullptr, kFixed);
    ImGui::PopStyleVar();
    const float gap = 14 * uiScale_;
    const int cols = fitColumns(ImGui::GetContentRegionAvail().x, 300 * uiScale_, 4);
    float colW = (ImGui::GetContentRegionAvail().x - (cols - 1) * gap) / cols;
    gLabelW = std::min(170.0f * uiScale_, colW * 0.5f);
    bool changed = false;
    auto card = [&](const char* id, auto&& body) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.115f, 0.118f, 0.126f, 0.97f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * uiScale_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {14 * uiScale_, 10 * uiScale_});
        ImGui::BeginChild(id, {colW, 0}, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_AutoResizeY);
        body();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    };
    auto note = [&](const char* text) {
        ImGui::PushFont(fontSmall_);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    };

    // ---- graphics
    ImGui::BeginGroup();
    card("gfx", [&] {
        sectionHeader("Графика");
        if (mobile_) {
            rowLabel("Ориентация");
            const char* orient[] = {"как у телефона", "альбомная", "портретная"};
            if (ImGui::Combo("##orient", &c.orientation, orient, 3)) {
                applyOrientation();
                changed = true;
            }
            helpMarker("«Как у телефона» — экран поворачивается по датчику положения, даже если автоповорот в системе выключен. "
                       "Альбомная и портретная тоже переворачиваются на 180°, если перевернуть телефон.");
            rowLabel("Инструменты");
            const char* sides[] = {"снизу", "слева", "справа"};
            if (ImGui::Combo("##toolside", &c.toolSide, sides, 3)) changed = true;
            helpMarker("Где стоит полоса инструментов микроскопа в портретной ориентации. Сбоку она не занимает высоту экрана, "
                       "и до неё удобно дотянуться большим пальцем. Также в меню ☰ микроскопа.");
        } else {
            bool fs = fullscreen_;
            if (checkbox("Полный экран (F11)", &fs)) {
                setFullscreen(fs);
                changed = true;
            }
        }
        bool vs = ctx_.vsync();
        if (checkbox("Вертикальная синхр.", &vs)) {
            ctx_.setVsync(vs);
            c.vsync = vs;
            changed = true;
        }
        helpMarker("Кадры совпадают с частотой монитора: без разрывов изображения и с меньшей нагрузкой.");
        ImGui::BeginDisabled(ctx_.vsync());
        const int caps[] = {0, 30, 60, 90, 120, 144, 165, 240};
        int ci = 0;
        for (int k = 0; k < 8; ++k)
            if (caps[k] == c.fpsLimit) ci = k;
        char fmt[32];
        if (ci == 0) std::snprintf(fmt, sizeof(fmt), "без ограничения");
        else std::snprintf(fmt, sizeof(fmt), "%d кадров/с", caps[ci]);
        rowLabel("Предел кадров");
        if (ImGui::SliderInt("##fps", &ci, 0, 7, fmt)) {
            c.fpsLimit = caps[ci];
            changed = true;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ctx_.vsync())
            ImGui::SetTooltip("Работает при выключенной вертикальной синхронизации");
        sectionHeader("Изображение");
        rowLabel("Микроскоп");
        const char* themes[] = {"тёмный", "классический"};
        int theme = c.theme ? 1 : 0;
        if (ImGui::Combo("##theme", &theme, themes, 2)) {
            c.theme = theme;
            changed = true;
        }
        changed |= checkbox("Мягкие клетки", &c.softBodies);
        helpMarker("Клетки сплющиваются при касании и пружинят. Без этого — простые круги (быстрее на огромных чашках).");
        changed |= checkbox("Плавное движение", &c.smoothMotion);
        helpMarker("Положения клеток сглаживаются между шагами симуляции.");
        changed |= checkbox("Адгезины (слизь)", &c.showSlime);
        changed |= checkbox("Частицы пищи", &c.showFood);
        changed |= checkbox("Сетка", &c.showGrid);
        rowLabel("GPU");
        ImGui::TextWrapped("%s", ctx_.gpuName());
    });
    ImGui::EndGroup();
    if (1 % cols != 0) ImGui::SameLine(0, gap);

    // ---- controls
    ImGui::BeginGroup();
    card("ctl", [&] {
        sectionHeader("Управление");
        auto key = [&](const char* k, const char* v) {
            ImGui::TextUnformatted(k);
            ImGui::SameLine(gLabelW);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", v);
            ImGui::PopTextWrapPos();
        };
        if (mobile_) {
            changed |= sliderF("Плавность камеры", &c.cameraSmoothing, 0.0f, 1.0f, "%.2f");
            sectionHeader("Жесты");
            ImGui::PushFont(fontSmall_);
            key("Касание", "действие инструмента; в «Диагностике» — выбрать клетку");
            key("Один палец", "в «Диагностике» — двигать чашку, в списках — прокрутка");
            key("Два пальца", "двигать и масштабировать чашку или организм");
            key("Удержание", "подсказка к кнопке");
            key("«Назад»", "закрыть окно, снять выделение");
            ImGui::PopFont();
            return;
        }
        changed |= sliderF("Скорость зума", &c.zoomSpeed, 0.3f, 3.0f, "%.1f×");
        changed |= checkbox("Обратное колесо", &c.invertWheel);
        changed |= sliderF("Перемещение WASD", &c.panSpeed, 0.3f, 3.0f, "%.1f×");
        changed |= sliderF("Плавность камеры", &c.cameraSmoothing, 0.0f, 1.0f, "%.2f");
        helpMarker("0 — камера следует мгновенно, 1 — очень мягко.");
        sectionHeader("Клавиши");
        ImGui::PushFont(fontSmall_);
        key("F2 F3 F4 F5", "разделы");
        key("F11 / F12", "полный экран / снимок");
        key("Пробел", "заморозка");
        key("1 – 9", "инструменты");
        key("Колесо, ПКМ", "зум, перемещение");
        key("WASD, стрелки", "перемещение");
        key("Home / F", "вся чашка / следить");
        key("Tab", "окно «Клетка»");
        key("Ctrl+Z / Y / S", "отмена / повтор / сохранить");
        key("PgUp / PgDn", "соседняя мода");
        ImGui::PopFont();
        if (ImGui::Button("Вся справка (F1)", {-FLT_MIN, 0})) ui_.showHelp = true;
    });
    ImGui::EndGroup();
    if (2 % cols != 0) ImGui::SameLine(0, gap);

    // ---- simulation
    ImGui::BeginGroup();
    card("sim", [&] {
        sectionHeader("Симуляция");
        changed |= sliderF("Медленно, тиков/с", &c.slowTicksPerSecond, 2.0f, 120.0f, "%.0f");
        changed |= sliderF("Наблюдение, тиков/с", &c.observeTicksPerSecond, 15.0f, 480.0f, "%.0f");
        helpMarker("1 тик = 9 секунд жизни клеток (1/400 ч). 60 тиков/с — это 9 минут симуляции в секунду.");
        {
            // Incubation runs on its own thread; rarer frames leave it more time.
            const int opts[] = {30, 60, 90, 120, 0};
            int oi = 1;
            for (int k = 0; k < 5; ++k)
                if (opts[k] == c.incubateFps) oi = k;
            char ifmt[48];
            if (opts[oi] == 0) std::snprintf(ifmt, sizeof(ifmt), "как у экрана");
            else std::snprintf(ifmt, sizeof(ifmt), "%d", opts[oi]);
            rowLabel("Кадров/с при инкубации");
            if (ImGui::SliderInt("##incfps", &oi, 0, 4, ifmt)) {
                c.incubateFps = opts[oi];
                changed = true;
            }
            helpMarker("Инкубация считается в отдельном потоке и не снижает частоту кадров. Каждый кадр ненадолго "
                       "останавливает расчёт, поэтому чем реже кадры, тем быстрее идёт инкубация.");
        }
        rowLabel("Потоки");
        unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        char tfmt[48];
        if (c.threads == 0) std::snprintf(tfmt, sizeof(tfmt), "авто (%u)", jobs_->threadCount());
        else std::snprintf(tfmt, sizeof(tfmt), "%d", c.threads);
        if (ImGui::SliderInt("##thr", &c.threads, 0, (int)hw, tfmt)) changed = true;
        note("Число потоков применяется после перезапуска игры.");
        changed |= checkbox("Пауза в свёрнутом окне", &c.pauseWhenMinimized);
        rowLabel("Автосохранение");
        char afmt[32];
        if (c.autosaveMinutes == 0) std::snprintf(afmt, sizeof(afmt), "выключено");
        else std::snprintf(afmt, sizeof(afmt), "каждые %d мин", c.autosaveMinutes);
        if (ImGui::SliderInt("##auto", &c.autosaveMinutes, 0, 30, afmt)) changed = true;
        helpMarker("Сохраняет чашку в substrates/autosave.substrate (загрузка — в разделе «Субстрат»).");
    });
    ImGui::EndGroup();
    if (3 % cols != 0) ImGui::SameLine(0, gap);

    // ---- interface
    ImGui::BeginGroup();
    card("ui", [&] {
        sectionHeader("Интерфейс");
        float scale = c.uiScale;
        if (sliderF("Размер интерфейса", &scale, 0.6f, 2.0f, "%.2f×", 0.05f, true)) c.uiScale = scale;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            fontsDirty_ = true;
            changed = true;
        }
        helpMarker("Применяется, когда отпустите ползунок.");
        float op = c.panelOpacity;
        if (sliderF("Непрозрачность окон", &op, 0.5f, 1.0f, "%.2f")) {
            c.panelOpacity = op;
            applyStyle();
            changed = true;
        }
        changed |= checkbox("Всплывающие пояснения", &c.tooltips);
        changed |= checkbox("Подсказка инструмента", &c.showHints);
        if (!mobile_) {  // the phone layout is docked: nothing to show, hide or move
            sectionHeader("Окна");
            drawWindowsMenu();
        }
        sectionHeader("Сброс");
        if (ImGui::Button("Настройки по умолчанию", {-FLT_MIN, 0})) {
            bool fs = fullscreen_;
            c = AppConfig();
            c.fullscreen = fs;
            ctx_.setVsync(c.vsync);
            applyStyle();
            fontsDirty_ = true;
            changed = true;
            toast("Настройки сброшены");
        }
        note("Настройки сохраняются в папке settings рядом с игрой.");
    });
    ImGui::EndGroup();

    if (changed) markConfigDirty();
    ImGui::End();
    gLabelW = 150.0f * uiScale_;
}
