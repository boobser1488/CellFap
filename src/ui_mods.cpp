// Modifications: loading at start-up and the «Модификации» window (list, switches, settings, log, packing).
// The mods themselves are src/sim/mods.cpp; the format and the API are described in docs/mods.md.
#include "app.hpp"
#include "sim/modpack.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

using namespace uiw;

namespace {

// The example modification, built in from mods/example (cmake/example_mod.cmake).
struct EmbeddedFile {
    const char* path;
    const char* text;
};
const EmbeddedFile kExampleMod[] = {
#include "example_mod.inc"
};

std::filesystem::path u8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
std::string fileName(const std::string& path) {
    const std::u8string n = u8(path).filename().u8string();
    return std::string(n.begin(), n.end());
}

#ifdef _WIN32
void openFolder(const std::string& dir) {
    std::wstring w = u8(dir).wstring();
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
#endif

}  // namespace

void App::initMods(const std::string& dyptLib, bool useSavedState) {
    std::string err;
    if (!mods_.initialize(dyptLib, err)) std::fprintf(stderr, "CellFap: модификации не могут работать (%s)\n", err.c_str());
    modsIni_ = dataDir("settings") + "/mods.ini";
    const std::string dir = dataDir("mods");
    // First start with modifications (no mods.ini yet): the example is put into mods/ (switched off). Deleted later,
    // it stays deleted.
    std::error_code ec;
    if (useSavedState && !std::filesystem::exists(u8(modsIni_), ec)) {
        for (const EmbeddedFile& f : kExampleMod) {
            const std::string path = dir + "/example/" + f.path;
            std::filesystem::create_directories(u8(path).parent_path(), ec);
            writeWholeFile(path, f.text);
        }
        mods_.saveState(modsIni_);
    }
    if (useSavedState) mods_.loadState(modsIni_);
    mods_.scan(dir);
    world_->afterStep = [this](World& w) { mods_.tick(w); };
    if (opt_.modsWindow) modsOpen_ = true;  // --mods [id] (captures)
    for (int i = 0; i < (int)mods_.mods.size(); ++i)
        if (!opt_.modEnable.empty() && mods_.mods[i].id == opt_.modEnable) {
            mods_.setEnabled(i, true);
            modSel_ = i;
        }
}

void App::drawModsWindow() {
    // Messages of the mods (сообщение): on the screen whatever window is open.
    for (const std::string& m : mods_.takeMessages()) toast(m);
    // Switches, settings and mods the watchdog turned off go to settings/mods.ini (not from automated captures).
    if (mods_.takeStateDirty() && opt_.screenshotPath.empty()) mods_.saveState(modsIni_);
    if (!modsOpen_) return;
    ImGuiIO& io = ImGui::GetIO();
    const float u = uiScale_;
    if (mobile_) {
        ImGui::SetNextWindowPos({safe_.x, navBottom()});
        ImGui::SetNextWindowSize({io.DisplaySize.x - safe_.x - safe_.z, io.DisplaySize.y - navBottom()});
    } else {
        ImGui::SetNextWindowSize({std::min(980 * u, io.DisplaySize.x * 0.9f), std::min(640 * u, io.DisplaySize.y * 0.85f)},
                                 ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos({io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
    }
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    if (mobile_) flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    if (!ImGui::Begin("Модификации###mods", &modsOpen_, flags)) {
        ImGui::End();
        return;
    }
    if (mobile_) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    if (modSel_ >= (int)mods_.mods.size()) modSel_ = -1;
    if (modSel_ < 0 && !mods_.mods.empty()) modSel_ = 0;

    // Narrow (a phone held upright): the list above the details instead of beside them.
    const bool narrow = ImGui::GetContentRegionAvail().x < 640 * u;
    const float listW = narrow ? 0.0f : std::max(260 * u, ImGui::GetContentRegionAvail().x * 0.34f);
    const float listH = narrow ? std::min(ImGui::GetContentRegionAvail().y * 0.38f, (float)mods_.mods.size() * 52 * u + 90 * u) : 0.0f;

    // ---------------------------------------------------------------- list
    ImGui::BeginChild("##modlist", {listW, listH}, ImGuiChildFlags_Borders);
    if (!mods_.ready()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "dypt_embed не подключён: модификации видны, но работать не могут.");
        ImGui::PopTextWrapPos();
    }
    for (int i = 0; i < (int)mods_.mods.size(); ++i) {
        ModInfo& m = mods_.mods[i];
        ImGui::PushID(i);
        bool on = m.enabled;
        if (ImGui::Checkbox("##on", &on)) {
            mods_.setEnabled(i, on);
            toast(m.name + (on ? (m.running ? ": включена" : ": не запустилась — " + m.error) : ": выключена"));
        }
        ImGui::SameLine();
        const ImVec4 dot = m.running ? ImVec4(0.43f, 0.75f, 0.47f, 1) : !m.error.empty() && m.enabled ? ImVec4(0.95f, 0.45f, 0.38f, 1)
                                                                                                      : ImVec4(0.5f, 0.5f, 0.55f, 1);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float fh = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled({c.x + 5 * u, c.y + fh * 0.5f}, 4 * u, ImGui::GetColorU32(dot));
        ImGui::Dummy({12 * u, fh});
        ImGui::SameLine();
        std::string label = m.name + (m.version.empty() ? "" : "  " + m.version);
        if (ImGui::Selectable(label.c_str(), modSel_ == i, 0, {0, fh})) modSel_ = i;
        ImGui::PopID();
    }
    if (mods_.mods.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Модификаций нет. Положите папку мода или файл .cfmod в папку mods и нажмите «Обновить».");
        ImGui::PopTextWrapPos();
    }
    ImGui::Separator();
    if (ImGui::Button("Обновить")) {
        mods_.scan(mods_.directory());
        modSel_ = mods_.mods.empty() ? -1 : 0;
        toast("Модификаций: " + std::to_string(mods_.mods.size()));
    }
    helpMarker("Перечитать папку mods: новые и изменённые модификации, файлы .cfmod. Включённые перезапускаются.");
    ImGui::SameLine();
    if (ImGui::Button("Добавить .cfmod…")) {
        const std::string p = pickFile(L"Модификации CellFap (*.cfmod)\0*.cfmod\0Все файлы\0*.*\0", L"Добавить модификацию");
        if (!p.empty()) importPath(p);  // (on the phone the picker answers later, through importPath as well)
    }
    helpMarker("Скопировать файл модификации в папку mods. Она добавится выключенной.");
#if defined(_WIN32)
    ImGui::SameLine();
    if (ImGui::Button("Папка mods")) openFolder(mods_.directory());
    helpMarker("Открыть папку mods в проводнике.");
#endif
    ImGui::EndChild();
    if (!narrow) ImGui::SameLine();

    // ---------------------------------------------------------------- details
    ImGui::BeginChild("##moddetail", {0, 0}, ImGuiChildFlags_Borders);
    if (modSel_ >= 0) {
        ModInfo& m = mods_.mods[modSel_];
        ImGui::PushFont(fontBold_);
        ImGui::TextUnformatted(m.name.c_str());
        ImGui::PopFont();
        ImGui::TextDisabled("%s%s%s · %s", m.version.empty() ? "" : ("версия " + m.version).c_str(),
                            m.author.empty() ? "" : (m.version.empty() ? "" : " · "), m.author.c_str(),
                            m.packed ? "файл .cfmod" : "папка");
        if (!m.description.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(m.description.c_str());
            ImGui::PopTextWrapPos();
        }
        // State.
        if (!m.error.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "%s", m.error.c_str());
            ImGui::PopTextWrapPos();
        }
        if (m.running) {
            int g = 0, p = 0;
            for (const ModGenome& x : mods_.genomes) g += x.mod == m.id;
            for (const ModProgram& x : mods_.programs) p += x.mod == m.id;
            std::string what = "Работает";
            if (m.tickEvery > 0 && m.hasTick) {
                char b[160];
                std::snprintf(b, sizeof b, " · тик каждые %d тиков, вызовов %llu, %.2f мс (худшее %.1f)", m.tickEvery,
                              (unsigned long long)m.ticks, m.lastMs, m.worstMs);
                what += b;
            }
            ImGui::TextColored({0.55f, 0.85f, 0.6f, 1.0f}, "%s", what.c_str());
            if (g || p) ImGui::TextDisabled("Добавила: геномов %d (браузер геномов, вкладка «Модификации»), программ %d (редактор программ)", g, p);
        } else if (!m.enabled) {
            ImGui::TextDisabled("Выключена.");
        }

        // Actions.
        if (ImGui::Button(m.enabled ? "Выключить" : "Включить")) {
            mods_.setEnabled(modSel_, !m.enabled);
        }
        ImGui::SameLine();
        if (ImGui::Button("Перезапустить")) {
            mods_.restart(modSel_);
            toast(m.running ? m.name + ": перезапущена" : m.name + (m.enabled ? ": " + m.error : ": файлы перечитаны"));
        }
        helpMarker("Перечитать файлы модификации с диска (после правки mod.dpt) и запустить заново.");
        if (!m.packed) {
            ImGui::SameLine();
            if (ImGui::Button("Собрать .cfmod")) {
                std::string out, err;
                if (buildModPack(m.path, out, err)) shareFile(out, fileName(out), "Модификация CellFap «" + m.name + "»");
                else toast("Не собрана: " + err);
            }
            helpMarker("Упаковать папку в один файл, которым удобно делиться. Его достаточно положить в папку mods.");
        }

        // Settings.
        if (!m.settings.empty()) {
            sectionHeader("Настройки");
            const float oldLabel = gLabelW;
            gLabelW = std::min(260 * u, ImGui::GetContentRegionAvail().x * 0.5f);
            bool changed = false;
            for (ModSetting& st : m.settings) {
                ImGui::PushID(st.key.c_str());
                if (st.kind == ModSetting::Flag) {
                    bool v = st.value != 0;
                    if (checkbox(st.label.c_str(), &v)) {
                        st.value = v ? 1 : 0;
                        changed = true;
                    }
                } else if (st.kind == ModSetting::Choice) {
                    rowLabel(st.label.c_str());
                    int v = (int)st.value;
                    if (ImGui::BeginCombo("##c", st.options[std::clamp(v, 0, (int)st.options.size() - 1)].c_str())) {
                        for (int k = 0; k < (int)st.options.size(); ++k)
                            if (ImGui::Selectable(st.options[k].c_str(), k == v)) {
                                st.value = k;
                                changed = true;
                            }
                        ImGui::EndCombo();
                    }
                } else {
                    // Whole numbers when the range and the default are whole: a count, not a fraction.
                    const bool whole = st.min == std::floor(st.min) && st.max == std::floor(st.max) && st.def == std::floor(st.def);
                    rowLabel(st.label.c_str());
                    if (whole) {
                        int v = (int)st.value;
                        if (ImGui::SliderInt("##n", &v, (int)st.min, (int)st.max)) st.value = v;
                    } else {
                        float v = (float)st.value;
                        if (ImGui::SliderFloat("##n", &v, (float)st.min, (float)st.max, "%.3g")) st.value = v;
                    }
                    // The mod hears of it when the slider is let go, not at every pixel of the drag.
                    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
                }
                ImGui::PopID();
            }
            gLabelW = oldLabel;
            if (ImGui::Button("По умолчанию")) {
                mods_.resetSettings(modSel_, world_.get());
            }
            if (changed) {
                mods_.settingChanged(modSel_, world_.get());
            }
        }

        // Log.
        sectionHeader("Журнал");
        ImGui::BeginChild("##modlog", {0, std::max(80 * u, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing())},
                          ImGuiChildFlags_Borders);
        ImGui::PushFont(fontMono_);
        for (const std::string& line : m.log) ImGui::TextUnformatted(line.c_str());
        if (m.log.empty()) ImGui::TextDisabled("Пусто: здесь то, что мод пишет через журнал() и print().");
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        if (ImGui::SmallButton("Очистить журнал")) m.log.clear();
        ImGui::SameLine();
        ImGui::TextDisabled("Как писать модификации — docs/mods.md");
    }
    ImGui::EndChild();
    ImGui::End();
}
