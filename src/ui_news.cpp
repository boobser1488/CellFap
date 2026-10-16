// «Новое»: what this version brought, each with a button that takes the player straight to it. Opens by itself the
// first time a new version starts (AppConfig::newsSeen < App::kNewsVersion); afterwards it is the first tab of the menu
// behind «‹» (the desktop) or a button in the settings (the phone).
#include "app.hpp"
#include "sim/presets.hpp"
#include "ui_widgets.hpp"

#include <cstring>
#include <functional>

using namespace uiw;

void App::drawNewsScreen() {
    const bool screen = ui_.screen == Screen::News;
    if (mobile_ && screen) {  // (--tab 7 on the phone layout)
        newsOpen_ = true;
        ui_.screen = Screen::Microscope;
    }
    if (!(mobile_ ? newsOpen_ : screen)) return;
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    ImGui::SetNextWindowPos({safe_.x, navBottom()});
    ImGui::SetNextWindowSize({io.DisplaySize.x - safe_.x - safe_.z, io.DisplaySize.y - navBottom()});
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("Новое###news", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    if (mobile_) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());

    // Where the buttons lead.
    auto leave = [&](Screen s) {
        newsOpen_ = false;
        ui_.screen = s;
    };
    auto openPreset = [&](const char* name) {
        for (const GenomePreset& p : genomePresets())
            if (std::strcmp(p.name, name) == 0) {
                loadGenomeIntoEditor(p.genome);
                leave(Screen::Genome);
                return true;
            }
        return false;
    };
    auto openWormProgram = [&] {
        if (!openPreset("Сигнальный червь")) return;
        for (int i = 0; i < kMaxModes; ++i)
            if (ui_.editGenome.modes[i].type == CellType::SignalCell) {
                ui_.editMode = i;
                break;
            }
        if (mobile_) {
            dockGenome_.open = true;
            dockGenome_.tab = 1;  // the program
        } else {
            programWinOpen_ = true;
        }
    };
    auto openMods = [&](const char* select) {
        for (int i = 0; i < (int)mods_.mods.size(); ++i)
            if (select && mods_.mods[i].id == select) modSel_ = i;
        if (mobile_) {
            newsOpen_ = false;
            modsOpen_ = true;
        } else {
            leave(Screen::Mods);
        }
    };

    struct Item {
        const char* title;
        const char* text;
        const char* button;
        std::function<void()> action;
    };
    const Item items[] = {
        {"Интерфейс как в Cell Lab",
         mobile_ ? "Оформление оригинала: тёмные панели с зелёными рамками, ползунки — зелёная дорожка с оранжевым бегунком, "
                   "флажки — оранжевые квадраты, вкладки с разделителями. В «Субстрате» сверху — сводка чашки и кнопки "
                   "«Эксперименты», «Перезагрузить», «Стерилизовать»."
                 : "Сверху стрелка «назад», логотип и название чашки, под ними вкладки СУБСТРАТ · МИКРОСКОП · ГЕНОМ. За "
                   "стрелкой — меню: новое, задания, эксперименты, модификации, настройки. Скорость времени — в верхней "
                   "панели. Ползунки, флажки и панели — как в оригинале. Окна по-прежнему двигаются, закрываются и "
                   "закрепляются: кнопка «Окна».",
         "Открыть настройки", [&] { leave(Screen::Settings); }},
        {"Модификации",
         "Моды — файлы .cfmod (обычный ZIP) или папки в mods: скрипт на dypt, картинка-превью, ресурсы, свои шейдеры и "
         "нативные библиотеки на любом языке. Включение, настройки, журнал и сборка .cfmod — на экране «Модификации». "
         "Нативный код запускается только после вашего разрешения. Как писать моды — docs/mods.md и папка sdk.",
         "Открыть «Модификации»", [&] { openMods(nullptr); }},
        {"Свои шейдеры",
         "Мод может заменить, как рисуются клетки, чашка, связи или частицы. Пример — «Сиреневые клетки»: включите его, "
         "и клетки в чашке станут сиреневыми; выключите — вернутся прежние.",
         "Показать пример", [&] { openMods("shader_tint"); }},
        {"Сигнальный червь",
         "Новый организм: волну изгиба ведут сигнальные клетки с программой на dypt, по одной в каждом сегменте, в ногу "
         "по общим часам чашки. Плывёт на ресничках и жгутике.",
         "Открыть в редакторе", [&] { openPreset("Сигнальный червь"); }},
        {"Программы клеток",
         "Редактор программ: поле кода растягивается за ручку и выносится в отдельное окно. У программ новые входы: "
         "«часы» (общий счёт тиков чашки) и «пища» рядом с клеткой. Ошибки объясняются по-русски, с номером строки.",
         "Открыть программу червя", openWormProgram},
        {"Геномы и клетки",
         "Клетки в редакторе и в превью больше не налезают друг на друга. Превью в браузере геномов рисуются тем же "
         "шейдером, что и чашка. Ядро стоит в центре клетки, даже когда её сжимают соседи. Электроцит перерисован: "
         "стопка пластин от «−» к «+».",
         "Браузер геномов", [&] {
             newsOpen_ = false;
             if (!mobile_) ui_.screen = Screen::Microscope;
             openGenomeBrowser();
         }},
        {"Анимация и мелочи",
         mobile_ ? "Окна, меню и страницы плавно появляются. Временные файлы скриптов больше не ломаются из-за "
                   "кириллицы в пути."
                 : "Окна, меню и экраны плавно появляются, подчёркивание вкладок переезжает. Панель «Клетка» появляется, "
                   "только когда клетка выбрана. Настройки — кнопкой в верхней панели (или F5).",
         nullptr, nullptr},
    };

    // Header.
    ImGui::PushFont(fontBold_);
    ImGui::TextUnformatted("Что нового в CellFap");
    ImGui::PopFont();
    ImGui::SameLine();
    const char* closeLabel = mobile_ ? "Закрыть" : "К чашке";
    const ImGuiStyle& st = ImGui::GetStyle();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - st.WindowPadding.x -
                                                              ImGui::CalcTextSize(closeLabel).x - 2 * st.FramePadding.x));
    if (ImGui::Button(closeLabel)) leave(Screen::Microscope);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(mobile_ ? "Эта страница открылась сама при первом запуске новой версии. Потом — «Настройки» → «Что нового»."
                                   : "Эта вкладка открылась сама при первом запуске новой версии. Потом она в меню за стрелкой «назад» (вкладка НОВОЕ) и в настройках.");
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 4 * u});

    // Cards: two columns when there is room.
    ImGui::BeginChild("##newsgrid", {0, 0});
    const float gap = 12 * u;
    const float avail = ImGui::GetContentRegionAvail().x;
    const int cols = avail > 900 * u ? 2 : 1;
    const float cardW = (avail - (cols - 1) * gap) / cols;
    int k = 0;
    for (const Item& it : items) {
        if (k % cols) ImGui::SameLine(0, gap);
        ImGui::PushID(k++);
        ImGui::BeginGroup();
        panelCard("##card", cardW, 0, [&] {
            ImGui::PushFont(fontBold_);
            ImGui::TextColored(kAccent, "%s", it.title);
            ImGui::PopFont();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(it.text);
            ImGui::PopTextWrapPos();
            if (it.button && ImGui::Button(it.button)) it.action();
        });
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}
