// Панель разработчика: шейдеры и параметры симуляции.
//
// Шейдеры показываются из встроенных в программу исходников. Правка применяется к этой копии игры: файлы проекта
// не меняются никогда, «Вернуть исходный» возвращает встроенную версину. Скомпилированные SPIR-V проверяются
// компилятором во внешнем процессе, а конвейер заменяется только если Vulkan его принял: сломанный шейдер не может
// закрыть игру.
#include "app.hpp"
#include "render/renderer.hpp"
#include "sim/serialize.hpp"  // dataDir(): рабочая папка программы
#include "sim/signal_cells.hpp"
#include "ui_widgets.hpp"

#include <cstdio>
#include <cstring>

#include "shader_sources.h"

namespace {

constexpr int kShaderBuf = 65536;

// Что редактируем: слой (пара «вершинный + фрагментный») и в какой из двух стадий вносим правку.
enum class Slot { Dish, Cell, Line, Particle, Count };
const char* kSlotNames[(int)Slot::Count] = {"Дно чашки", "Клетки", "Линии", "Частицы"};
const char* kSlotVert[(int)Slot::Count] = {"dish.vert", "cell.vert", "line.vert", "particle.vert"};
const char* kSlotFrag[(int)Slot::Count] = {"dish.frag", "cell.frag", "line.frag", "particle.frag"};

struct EditorState {
    int slot = 1;          // по умолчанию клетки: там интереснее всего
    bool editingFrag = true;
    bool loaded = false;
    bool dirty = false;
    char buf[kShaderBuf] = "";
    std::string log;
    bool compiled = false;
    bool applied = false;
};
EditorState g_ed;

const EmbeddedShader* findShader(const char* name) {
    const EmbeddedShader* list = embeddedShaders();
    for (int i = 0; i < embeddedShaderCount(); ++i)
        if (std::strcmp(list[i].name, name) == 0) return &list[i];
    return nullptr;
}

const char* currentOriginal() {
    const char* want = g_ed.editingFrag ? kSlotFrag[g_ed.slot] : kSlotVert[g_ed.slot];
    const EmbeddedShader* s = findShader(want);
    return s ? s->source : "";
}

// Компиляция во внешний процесс. Ошибка в шейдере остаётся ошибкой glslc, а не состоянием игры.
bool compileToSpirv(const std::string& tool, const std::string& source, const char* stage, bool isFrag,
                    std::vector<uint32_t>& out, std::string& log) {
#ifdef __ANDROID__
    // На телефоне нет компилятора шейдеров: встроить его в APK — это отдельный проект (glslang/shaderc под
    // Android NDK), и он весит столько же, сколько сама игра. Поэтому редактор шейдера на Android показывает и
    // сохраняет правки, а применение честно отказывает.
    (void)tool;
    (void)source;
    (void)stage;
    (void)isFrag;
    (void)out;
    log = "На Android компилятора GLSL в приложении нет: правку можно написать и сохранить, но применить её можно "
          "только на настольной сборке (там вызывается glslc из Vulkan SDK).";
    return false;
#else
    const std::string dir = dataDir("shaders") + "/";
    const std::string srcPath = dir + "edit_" + stage + (isFrag ? ".frag" : ".vert");
    const std::string binPath = dir + "edit_" + stage + ".spv";
    FILE* f = std::fopen(srcPath.c_str(), "wb");
    if (!f) {
        log = "не удалось создать временный файл шейдера в " + dir;
        return false;
    }
    std::fwrite(source.data(), 1, source.size(), f);
    std::fclose(f);

    // common.glsl лежит рядом, потому что шейдеры включают его через #include — путь включения указываем туда же.
    char cmd[1400];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" -O -I \"%s\" -o \"%s\" \"%s\" 2>&1", tool.c_str(), dir.c_str(),
                  binPath.c_str(), srcPath.c_str());
    log.clear();
#ifdef _WIN32
    FILE* pipe = _popen(cmd, "r");
#else
    FILE* pipe = popen(cmd, "r");
#endif
    if (pipe) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe)) log += buf;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    }
    out.clear();
    FILE* sf = std::fopen(binPath.c_str(), "rb");
    if (!sf) {
        if (log.empty()) log = "компилятор не выдал результат (" + tool + ")";
        return false;
    }
    std::fseek(sf, 0, SEEK_END);
    const long bytes = std::ftell(sf);
    std::fseek(sf, 0, SEEK_SET);
    if (bytes > 0) {
        out.resize((size_t)bytes / 4);
        if (std::fread(out.data(), 1, (size_t)bytes, sf) != (size_t)bytes) {
            std::fclose(sf);
            log = "не удалось прочитать скомпилированный шейдер";
            return false;
        }
    }
    std::fclose(sf);
    if (out.empty()) {
        log = "пустой результат компиляции";
        return false;
    }
    return true;  // log может содержать предупреждения — они не мешают
#endif
}

}  // namespace

void App::drawDeveloperPanel() {
    if (!developerOpen_) return;  // (Begin with a closed flag still draws the window)
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize({std::min(560 * uiScale_, io.DisplaySize.x * 0.9f), std::min(720 * uiScale_, io.DisplaySize.y * 0.85f)},
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Разработчик###dev", &developerOpen_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    const float u = uiScale_;

    // ---------------------------------------------------------------- шейдеры
    uiw::sectionHeader("Шейдеры");
    ImGui::TextWrapped("Исходники встроены в программу. Правка применяется к этой копии игры: файлы проекта не "
                      "меняются, «Вернуть исходный» возвращает встроенную версию. Если Vulkan не примет скомпилированный "
                      "шейдер, работающий останется на месте.");
    {
        int slot = g_ed.slot;
        if (ImGui::BeginCombo("Слой", kSlotNames[slot])) {
            for (int i = 0; i < (int)Slot::Count; ++i)
                if (ImGui::Selectable(kSlotNames[i], slot == i)) g_ed.slot = i;
            ImGui::EndCombo();
        }
        // Переключатель стадии: две кнопки рядом, без radio-кнопок — так же, как в других панелях.
        for (int i = 0; i < 2; ++i) {
            const bool isFrag = i == 0;
            if (i) ImGui::SameLine();
            const bool on = g_ed.editingFrag == isFrag;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(uiw::kAccent.x, uiw::kAccent.y, uiw::kAccent.z, 0.42f));
            if (ImGui::Button(isFrag ? "Фрагментный" : "Вершинный", {ImGui::GetContentRegionAvail().x * 0.2f, 0}))
                g_ed.editingFrag = isFrag;
            if (on) ImGui::PopStyleColor();
        }
    }

    const char* orig = currentOriginal();
    if (orig && *orig) {
        if (!g_ed.loaded || (!g_ed.dirty && std::strcmp(g_ed.buf, orig) != 0)) {
            const size_t n = std::min(std::strlen(orig), (size_t)kShaderBuf - 1);
            std::memcpy(g_ed.buf, orig, n);
            g_ed.buf[n] = 0;
            g_ed.loaded = true;
            g_ed.dirty = false;
            g_ed.log.clear();
        }
        const float bw = (ImGui::GetContentRegionAvail().x - 12 * u) / 3.0f;
        if (ImGui::Button("Вернуть исходный", {bw, 0})) {
            const size_t n = std::min(std::strlen(orig), (size_t)kShaderBuf - 1);
            std::memcpy(g_ed.buf, orig, n);
            g_ed.buf[n] = 0;
            g_ed.dirty = false;
            g_ed.log = "Восстановлена встроенная версия. Нажмите «Скомпилировать и применить», чтобы вернуть её на экран.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Скомпилировать и применить", {bw, 0})) {
            g_ed.log.clear();
            g_ed.compiled = g_ed.applied = false;
            const std::string tool = cfg_.glslcPath.empty() ? std::string("glslc") : cfg_.glslcPath;
            std::string edited(g_ed.buf);
            std::vector<uint32_t> vs, fs;
            std::string logV, logF;
            // Собираем пару: правка идёт в одну стадию, вторую берём встроенной.
            const std::string vsSrc = g_ed.editingFrag ? (findShader(kSlotVert[g_ed.slot]) ? findShader(kSlotVert[g_ed.slot])->source : "") : edited;
            const std::string fsSrc = g_ed.editingFrag ? edited : (findShader(kSlotFrag[g_ed.slot]) ? findShader(kSlotFrag[g_ed.slot])->source : "");
            const bool okV = compileToSpirv(tool, vsSrc, "vert", false, vs, logV);
            const bool okF = compileToSpirv(tool, fsSrc, "frag", true, fs, logF);
            if (!okV || !okF) {
                g_ed.log = "Ошибка компиляции:\n" + (okV ? logF : logV);
                if (logV.empty() && logF.empty()) g_ed.log += "\nКомпилятор не найден: укажите путь к нему ниже.";
            } else {
                g_ed.compiled = true;
                Renderer::ShaderSlot s = (Renderer::ShaderSlot)g_ed.slot;
                if (renderer_.replaceShader(s, vs.data(), vs.size() * 4, fs.data(), fs.size() * 4)) {
                    g_ed.applied = true;
                    g_ed.log = "Шейдер применён.";
                } else {
                    g_ed.log = "Vulkan отверг конвейер: оставлен прежний шейдер.\n" + logV + logF;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Проверить синтаксис", {bw, 0})) {
            g_ed.log.clear();
            g_ed.compiled = g_ed.applied = false;
            const std::string tool = cfg_.glslcPath.empty() ? std::string("glslc") : cfg_.glslcPath;
            std::vector<uint32_t> dummy;
            std::string log;
            const bool ok = compileToSpirv(tool, std::string(g_ed.buf), g_ed.editingFrag ? "frag" : "vert",
                                           g_ed.editingFrag, dummy, log);
            g_ed.log = ok ? "Синтаксис в порядке." + (log.empty() ? "" : "\n" + log) : "Ошибка:\n" + log;
        }

        ImGui::Separator();
        ImGui::BeginChild("shadercode", {ImGui::GetContentRegionAvail().x, std::max(160.0f * u, io.DisplaySize.y * 0.32f)},
                          ImGuiChildFlags_Borders);
        ImGui::PushFont(fontSmall_);
        if (ImGui::InputTextMultiline("##sh", g_ed.buf, kShaderBuf, {ImGui::GetContentRegionAvail().x, 0},
                                      ImGuiInputTextFlags_AllowTabInput)) {
            g_ed.dirty = true;
            g_ed.applied = false;
        }
        ImGui::PopFont();
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Исходники шейдеров не встроены в эту сборку.");
    }
    if (!g_ed.log.empty()) {
        const bool good = g_ed.applied || (g_ed.log.find("порядке") != std::string::npos);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(good ? 0.6f : 1.0f, good ? 0.9f : 0.55f, good ? 0.6f : 0.45f, 1.0f));
        ImGui::TextWrapped("%s", g_ed.log.c_str());
        ImGui::PopStyleColor();
    }
    {
        std::string p = cfg_.glslcPath;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", p.c_str());
        if (ImGui::InputText("Путь к glslc", buf, sizeof(buf))) cfg_.glslcPath = buf;
        uiw::helpMarker("Компилятор шейдеров из Vulkan SDK (shaderc). Без него правки можно писать, но не применять. "
                        "На Android компилятора нет — там применяются только встроенные шейдеры.");
    }

    // ---------------------------------------------------------------- сигнальные клетки
    ImGui::Separator();
    uiw::sectionHeader("Программы клеток (dypt)");
    SignalCellManager& mgr = world_->signalCells();
    ImGui::Text("dypt: %s", mgr.ready() ? "подключён" : "не подключён");
    if (!mgr.lastError().empty()) ImGui::TextDisabled("%s", mgr.lastError().c_str());
    ImGui::Text("Живых программ: %d из %d", mgr.programCount(), mgr.limits.maxProgramsPerWorld);
    ImGui::Text("Ошибок за последний тик: %d", world_->signalCellErrors);
    ImGui::Text("Выполнено вызовов: %llu", (unsigned long long)world_->signalCellOps);
    ImGui::TextWrapped("Лимиты проверяются во время работы: исходник до %zu Б, программ до %d, на все клетки с программой "
                       "%.1f мс за тик (остальные ждут своей очереди), вызов дольше %.0f мс — программа отключается.",
                       mgr.limits.maxSourceBytes, mgr.limits.maxProgramsPerWorld, mgr.limits.tickBudgetMs,
                       mgr.limits.hangMs);
    SandboxLimits& lim = mgr.limits;
    {
        int maxSrc = (int)(lim.maxSourceBytes / 1024);
        if (ImGui::SliderInt("Лимит исходника, КБ", &maxSrc, 1, 64)) lim.maxSourceBytes = (size_t)maxSrc * 1024;
        int maxProg = lim.maxProgramsPerWorld;
        if (ImGui::SliderInt("Лимит программ", &maxProg, 1, 128)) lim.maxProgramsPerWorld = maxProg;
        float budget = (float)lim.tickBudgetMs;
        if (ImGui::SliderFloat("Время на тик, мс", &budget, 0.5f, 20.0f, "%.1f")) lim.tickBudgetMs = budget;
        float hang = (float)lim.hangMs;
        if (ImGui::SliderFloat("Сторож, мс", &hang, 50.0f, 2000.0f, "%.0f")) lim.hangMs = hang;
        ImGui::SliderInt("Запуск раз в N тиков", &lim.everyTicks, 1, 40);
        if (ImGui::Button("Сбросить все программы")) {
            mgr.clear();
            g_ed.log = "Программы выгружены; клетки заработают снова после перезапуска или новой правки.";
        }
    }

    // ---------------------------------------------------------------- параметры симуляции
    ImGui::Separator();
    uiw::sectionHeader("Параметры симуляции");
    Settings& s = world_->settings;
    uiw::sliderF("Радиус чашки, мкм", &s.dishRadius, 200.0f, 2000.0f, "%.0f");
    uiw::sliderF("Скорость роста пищи", &s.nutrientRate, 0.0f, 15.0f, "%.1f");
    uiw::sliderF("Вязкость", &s.viscosity, 0.0f, 150.0f, "%.0f");
    uiw::sliderF("Радиация", &s.radiation, 0.0f, 0.09f, "%.3f");
    uiw::sliderF("Свет", &s.lightAmount, 0.0f, 2.0f, "%.2f");
    uiw::sliderF("Гравитация", &s.gravity, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Старение клеток", &s.cellAging);
    ImGui::Checkbox("Заражение извне", &s.contaminate);
    ImGui::TextWrapped("Настройки действуют на текущую чашку. Чтобы они сохранились, нажмите обычное сохранение "
                       "чашки — она пишется вместе с субстратом.");
    ImGui::End();
}
