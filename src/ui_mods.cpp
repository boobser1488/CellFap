// Modifications: loading at start-up and the «Модификации» window (list, switches, settings, log, packing).
// The mods themselves are src/sim/mods.cpp; the format and the API are described in docs/mods.md.
#include "app.hpp"
#include "sim/modpack.hpp"
#include "sim/serialize.hpp"
#include "ui_widgets.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>

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

ImU32 rgbaToImGui(uint32_t c) { return IM_COL32((c >> 24) & 255, (c >> 16) & 255, (c >> 8) & 255, c & 255); }

// A native mod's panel (sdk/cellfap_mod.h, ui_*): its primitives drawn with our widgets. Labels are labels: ImGui IDs
// stay inside the panel's own ID scope.
class ImGuiModUi final : public ModUi {
public:
    explicit ImGuiModUi(ImFont* bold) : bold_(bold) {}
    void text(const char* s) override {
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(s);
        ImGui::PopTextWrapPos();
    }
    void textColored(uint32_t rgba, const char* s) override {
        ImGui::PushStyleColor(ImGuiCol_Text, rgbaToImGui(rgba));
        text(s);
        ImGui::PopStyleColor();
    }
    void heading(const char* s) override {
        if (bold_) ImGui::PushFont(bold_);
        ImGui::TextUnformatted(s);
        if (bold_) ImGui::PopFont();
    }
    void separator() override { ImGui::Separator(); }
    void sameLine() override { ImGui::SameLine(); }
    bool button(const char* label) override { return ImGui::Button(label); }
    bool checkbox(const char* label, int32_t* v) override {
        bool b = *v != 0;
        if (!uiw::checkbox(label, &b)) return false;
        *v = b ? 1 : 0;
        return true;
    }
    bool sliderFloat(const char* label, float* v, float lo, float hi) override { return uiw::sliderF(label, v, lo, hi, "%.3g"); }
    bool sliderInt(const char* label, int32_t* v, int32_t lo, int32_t hi) override {
        int x = *v;
        const bool changed = uiw::sliderI(label, &x, lo, hi);
        *v = x;
        return changed;
    }
    bool inputText(const char* label, char* buf, size_t cap) override {
        uiw::rowLabel(label);
        ImGui::PushID(label);
        const bool changed = ImGui::InputText("##v", buf, cap);
        ImGui::PopID();
        return changed;
    }

private:
    ImFont* bold_;
};

// Drawing over the dish for native mods (draw()): world coordinates to the screen, on ImGui's background list (above
// the dish, under every window).
class ImGuiModDraw final : public ModDraw {
public:
    ImGuiModDraw(Vec2 cam, float zoom, float fbW, float fbH)
        : cam_(cam), zoom_(zoom), fbW_(std::max(fbW, 1.0f)), fbH_(std::max(fbH, 1.0f)), dl_(ImGui::GetBackgroundDrawList()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        sx_ = ds.x / fbW_;
        sy_ = ds.y / fbH_;
    }
    void line(float x0, float y0, float x1, float y1, uint32_t rgba, float width) override {
        dl_->AddLine(screen(x0, y0), screen(x1, y1), rgbaToImGui(rgba), width);
    }
    void circle(float x, float y, float r, uint32_t rgba, float width) override {
        const float rs = r * zoom_ * sx_;
        if (rs < 0.5f || rs > 1e5f) return;
        const int segments = std::clamp((int)(rs * 0.5f), 12, 128);
        if (width <= 0) dl_->AddCircleFilled(screen(x, y), rs, rgbaToImGui(rgba), segments);
        else dl_->AddCircle(screen(x, y), rs, rgbaToImGui(rgba), segments, width);
    }
    void text(float x, float y, uint32_t rgba, const char* s) override {
        const ImVec2 p = screen(x, y), size = ImGui::CalcTextSize(s);
        dl_->AddText({p.x - size.x * 0.5f, p.y - size.y * 0.5f}, rgbaToImGui(rgba), s);
    }

private:
    ImVec2 screen(float x, float y) const {
        return {((x - cam_.x) * zoom_ + fbW_ * 0.5f) * sx_, ((cam_.y - y) * zoom_ + fbH_ * 0.5f) * sy_};
    }
    Vec2 cam_;
    float zoom_, fbW_, fbH_, sx_ = 1, sy_ = 1;
    ImDrawList* dl_;
};

// The mod whose native code the player is being asked about (the consent popup), or -1.
int gConsentAsk = -1;

// preview.png of the mods as textures, by the mod's path; rebuilt when its files are read anew.
struct PreviewTex {
    Renderer::Texture tex;
    const ModFiles* files = nullptr;
};
std::map<std::string, PreviewTex> gPreviews;

// Which mod's shaders each pipeline shows (the game's own when null), and why a mod's shaders are not shown.
std::shared_ptr<const ModShaders> gShaderApplied[kModShaderSlots];
std::string gShaderOwner[kModShaderSlots];
std::map<std::string, std::string> gShaderNotes;         // mod id -> a conflict with another mod (every frame)
std::map<const ModShaders*, std::string> gShaderRefused;  // shaders Vulkan refused (until the mod's files change)

const char* kNativeWarning =
    "Нативный код имеет полный доступ к компьютеру: он может читать, менять и удалять ваши файлы, запускать программы "
    "и выходить в сеть — всё, что может сама игра. Игра не может его ограничить или остановить, а ошибка в нём может "
    "закрыть игру. Разрешайте его только модификациям из источников, которым доверяете.";

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
    mods_.attach(world_.get());  // (native mods get the dish right after they load)
    mods_.scan(dir);
    world_->afterStep = [this](World& w) { mods_.tick(w); };
    world_->beforeStep = [this](World& w) { return mods_.beforeStep(w); };
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
    // A dish replaced while the simulation stands still: the mods hear of it now, not at the next tick.
    mods_.frame(*world_);
    // Dishes the mods asked to save or open (substrates/<name>.substrate).
    std::vector<ModManager::DishRequest> dishes;
    dishes.swap(mods_.dishRequests);
    for (const ModManager::DishRequest& rq : dishes) {
        const std::string path = dataDir("substrates") + "/" + rq.file + ".substrate";
        std::error_code ec;
        if (rq.save) toast(saveWorld(*world_, path.c_str()) ? "Модификация сохранила чашку: substrates/" + rq.file + ".substrate"
                                                             : "Модификация не смогла сохранить чашку " + rq.file);
        else if (std::filesystem::exists(u8(path), ec)) importPath(path);
        else toast("Модификация просит открыть чашку «" + rq.file + "», но её нет в substrates");
    }
    // Native mods draw over the dish.
    if (ui_.screen == Screen::Microscope) {
        const VkExtent2D e = ctx_.extent();
        ImGuiModDraw draw(cam_, zoom_, (float)e.width, (float)e.height);
        mods_.drawOverlay(*world_, draw);
    }
    // Shaders of running mods replace the game's (one mod per pipeline); a mod switched off gives them back.
    {
        std::shared_ptr<const ModShaders> want[kModShaderSlots];
        std::string owner[kModShaderSlots];
        for (const ModInfo& m : mods_.mods) {
            gShaderNotes.erase(m.id);
            if (!m.running || !m.shaders) continue;
            for (int s = 0; s < kModShaderSlots; ++s) {
                if (!m.shaders->has(s)) continue;
                if (want[s]) {
                    gShaderNotes[m.id] += std::string("шейдер «") + modShaderSlotName(s) + "» уже заменяет модификация «" + owner[s] +
                                          "»: выключите одну из них. ";
                    continue;
                }
                want[s] = m.shaders;
                owner[s] = m.name;
            }
        }
        for (int s = 0; s < kModShaderSlots; ++s) {
            if (want[s] == gShaderApplied[s]) {
                if (want[s] && gShaderOwner[s] != owner[s]) gShaderOwner[s] = owner[s];
                continue;
            }
            const auto slot = (Renderer::ShaderSlot)s;
            gShaderApplied[s] = want[s];
            gShaderOwner[s] = owner[s];
            if (!want[s]) {
                renderer_.restoreShader(slot);
                continue;
            }
            // A stage the mod does not bring stays the game's own.
            const uint32_t *vs = want[s]->vert[s].data(), *fs = want[s]->frag[s].data();
            size_t vn = want[s]->vert[s].size() * 4, fn = want[s]->frag[s].size() * 4;
            if (want[s]->vert[s].empty()) Renderer::builtinShader(slot, false, &vs, &vn);
            if (want[s]->frag[s].empty()) Renderer::builtinShader(slot, true, &fs, &fn);
            if (!renderer_.replaceShader(slot, vs, vn, fs, fn)) {
                renderer_.restoreShader(slot);
                // (noted until the mod's files change: it is not tried again every frame)
                gShaderRefused[want[s].get()] += std::string("Vulkan отверг шейдер «") + modShaderSlotName(s) +
                                                 "» (входы, выходы или push-константы не совпадают с игрой): оставлен встроенный. ";
            }
        }
        for (auto it = gShaderRefused.begin(); it != gShaderRefused.end();) {
            bool live = false;
            for (const ModInfo& m : mods_.mods) live = live || m.shaders.get() == it->first;
            it = live ? std::next(it) : gShaderRefused.erase(it);
        }
    }
    // Previews of mods that are gone or were read anew are let go.
    for (auto it = gPreviews.begin(); it != gPreviews.end();) {
        bool keep = false;
        for (const ModInfo& m : mods_.mods) keep = keep || (m.path == it->first && m.files.get() == it->second.files);
        if (keep) {
            ++it;
            continue;
        }
        renderer_.destroyTexture(it->second.tex);
        it = gPreviews.erase(it);
    }
    if (!modsOpen_) gConsentAsk = -1;
    if (!modsOpen_) return;
    ImGuiIO& io = ImGui::GetIO();
    // The preview of a mod as a texture (made the first time it is shown).
    auto preview = [&](const ModInfo& m) -> ImTextureID {
        if (m.preview.empty()) return ImTextureID{};
        auto it = gPreviews.find(m.path);
        if (it != gPreviews.end()) return (ImTextureID)it->second.tex.id;
        if (gPreviews.size() >= 40) return ImTextureID{};  // (descriptor sets are few)
        PreviewTex p;
        p.files = m.files.get();
        p.tex = renderer_.createTexture(m.preview.data(), m.previewW, m.previewH);
        return (ImTextureID)gPreviews.emplace(m.path, p).first->second.tex.id;
    };
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
        if (uiw::boxCheck("##on", &on)) {
            mods_.setEnabled(i, on);
            if (on && !m.running && m.native.any && !m.native.here.empty() && !m.nativeAllowed && m.duplicate.empty()) {
                modSel_ = i;
                gConsentAsk = i;  // switched on, but its native code needs the player's word first
            } else {
                toast(m.name + (on ? (m.running ? ": включена" : ": не запустилась — " + m.error) : ": выключена"));
            }
        }
        ImGui::SameLine();
        const ImVec4 dot = m.running ? ImVec4(0.43f, 0.75f, 0.47f, 1) : !m.error.empty() && m.enabled ? ImVec4(0.95f, 0.45f, 0.38f, 1)
                                                                                                      : ImVec4(0.5f, 0.5f, 0.55f, 1);
        const ImVec2 c = ImGui::GetCursorScreenPos();
        const float fh = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled({c.x + 5 * u, c.y + fh * 0.5f}, 4 * u, ImGui::GetColorU32(dot));
        ImGui::Dummy({12 * u, fh});
        ImGui::SameLine();
        if (const ImTextureID tex = preview(m)) {  // its picture, a frame high
            const float aspect = (float)m.previewW / (float)std::max(m.previewH, 1);
            ImGui::Image(tex, {fh * std::min(aspect, 2.0f), fh});
            ImGui::SameLine();
        }
        std::string label = m.name + (m.version.empty() ? "" : "  " + m.version) + (m.native.any ? "  · нативный код" : "");
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
        // preview.png beside the name and the description (above them when the column is narrow).
        const ImTextureID tex = preview(m);
        const float picH = std::min(160 * u, ImGui::GetContentRegionAvail().x * 0.3f);
        const float picW = tex ? picH * std::min((float)m.previewW / (float)std::max(m.previewH, 1), 2.0f) : 0.0f;
        if (tex) {
            ImGui::Image(tex, {picW, picH});
            ImGui::SameLine();
        }
        ImGui::BeginGroup();
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
        if (!m.previewError.empty()) ImGui::TextDisabled("%s", m.previewError.c_str());
        // What it brings besides a script.
        if (m.shaders) {
            std::string which;
            for (int s = 0; s < kModShaderSlots; ++s)
                if (m.shaders->has(s))
                    which += std::string(which.empty() ? "" : ", ") + modShaderSlotName(s) +
                             (m.shaders->vert[s].empty() ? " (фрагментный)" : m.shaders->frag[s].empty() ? " (вершинный)" : "");
            ImGui::TextDisabled("Шейдеры: %s%s", which.c_str(), m.running ? "" : " — применятся, когда модификация включена");
        }
        ImGui::EndGroup();
        {
            std::string note = m.shaderError;
            if (auto it = gShaderNotes.find(m.id); it != gShaderNotes.end()) note += it->second;
            if (m.shaders)
                if (auto it = gShaderRefused.find(m.shaders.get()); it != gShaderRefused.end()) note += it->second;
            if (!note.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "%s", note.c_str());
                ImGui::PopTextWrapPos();
            }
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
            if (m.nat) {
                char b[96];
                std::snprintf(b, sizeof b, " · нативный код: %.2f мс на тик", m.nativeMs);
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
            if (m.enabled && !m.running && m.native.any && !m.native.here.empty() && !m.nativeAllowed && m.duplicate.empty())
                gConsentAsk = modSel_;
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
                // Into exports/, not beside the folder: in mods/ the pack would be a second mod with the same id.
                std::string out = dataDir("exports") + "/" + fileName(m.path) + ".cfmod", err, log;
                ModPackOptions po;
#ifndef __ANDROID__
                po.compileShaders = true;  // shaders/*.vert|.frag -> .spv (the phone has no compiler: .spv only)
                po.glslc = findGlslc(cfg_.glslcPath);
                po.includeDirs = {dataDir("shaders")};
#endif
                po.log = &log;
                if (buildModPack(m.path, out, err, &po)) {
                    if (!log.empty()) m.log.push_back("сборка: " + log);
                    shareFile(out, fileName(out), "Модификация CellFap «" + m.name + "»");
                } else {
                    toast("Не собрана: " + err);
                    m.log.push_back("сборка не удалась: " + err);
                }
            }
            helpMarker("Упаковать папку в один файл (папка exports), которым удобно делиться. Его достаточно положить в "
                       "папку mods на другом устройстве.");
        }

        // Native code: what it is, the warning and the player's consent (sdk/cellfap_mod.h, docs/mods.md).
        if (m.native.any) {
            sectionHeader("Нативный код");
            std::string plats;
            for (const std::string& p : m.native.platforms) plats += (plats.empty() ? "" : ", ") + p;
            ImGui::TextDisabled("Библиотеки для: %s", plats.empty() ? "—" : plats.c_str());
            ImGui::PushTextWrapPos(0);
            if (m.native.here.empty()) {
                ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "Для этой платформы (%s) библиотеки нет: модификация здесь не работает.",
                                   nativePlatform());
            } else {
                ImGui::TextColored({1.0f, 0.72f, 0.35f, 1.0f}, "%s", kNativeWarning);
                ImGui::TextDisabled("SHA-256: %s", m.native.hash.c_str());
                if (m.nativeChanged)
                    ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f},
                                       "Файлы нативного кода изменились после того, как вы их разрешили. Убедитесь, что "
                                       "модификация пришла из того же источника.");
                if (m.nativeAllowed) {
                    ImGui::TextColored({0.55f, 0.85f, 0.6f, 1.0f}, "Разрешён (именно этот код).");
                    if (ImGui::Button("Запретить нативный код")) {
                        mods_.allowNative(modSel_, false);
                        toast(m.name + ": нативный код запрещён");
                    }
                } else if (m.duplicate.empty()) {
                    if (ImGui::Button(m.nativeChanged ? "Разрешить новую версию…" : "Разрешить нативный код…")) gConsentAsk = modSel_;
                }
            }
            ImGui::PopTextWrapPos();
        }

        // The mod's own panel (a native mod's panel()).
        if (mods_.hasPanel(modSel_)) {
            const char* title = m.nat && m.nat->mod.panel_title ? m.nat->mod.panel_title : "Панель модификации";
            sectionHeader(title);
            ImGui::PushID("modpanel");
            const float oldLabel = gLabelW;
            gLabelW = std::min(260 * u, ImGui::GetContentRegionAvail().x * 0.5f);
            ImGuiModUi ui(fontBold_);
            mods_.drawPanel(modSel_, world_.get(), ui);
            gLabelW = oldLabel;
            ImGui::PopID();
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
                        if (uiw::cellLabSlider("##n", ImGuiDataType_S32, &v, (int)st.min, (int)st.max)) st.value = v;
                    } else {
                        float v = (float)st.value;
                        if (uiw::cellLabSlider("##n", ImGuiDataType_Float, &v, (float)st.min, (float)st.max, "%.3g")) st.value = v;
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

    // Consent to native code: asked every time the player wants it (switching on, the button), never assumed.
    if (gConsentAsk >= (int)mods_.mods.size()) gConsentAsk = -1;
    if (gConsentAsk >= 0 && !ImGui::IsPopupOpen("###nativeconsent")) ImGui::OpenPopup("###nativeconsent");
    ImGui::SetNextWindowSize({std::min(560 * u, io.DisplaySize.x * 0.92f), 0});
    if (ImGui::BeginPopupModal("Разрешить нативный код?###nativeconsent", nullptr, ImGuiWindowFlags_NoResize)) {
        if (gConsentAsk < 0) {
            ImGui::CloseCurrentPopup();
        } else {
            ModInfo& m = mods_.mods[gConsentAsk];
            ImGui::PushFont(fontBold_);
            ImGui::TextUnformatted(m.name.c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s%s", m.author.empty() ? "" : ("автор: " + m.author + " · ").c_str(), m.packed ? "файл .cfmod" : "папка");
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored({1.0f, 0.72f, 0.35f, 1.0f}, "%s", kNativeWarning);
            if (m.nativeChanged) ImGui::TextColored({1.0f, 0.55f, 0.45f, 1.0f}, "Этот код отличается от того, что вы разрешали раньше.");
            std::string libs;
            for (const std::string& l : m.native.here) libs += (libs.empty() ? "" : ", ") + l;
            ImGui::TextDisabled("Файлы: %s", libs.c_str());
            ImGui::TextDisabled("SHA-256: %s", m.native.hash.c_str());
            ImGui::TextDisabled("Разрешение запомнится для этих файлов; если они изменятся, игра спросит снова.");
            ImGui::PopTextWrapPos();
            ImGui::Separator();
            if (ImGui::Button("Разрешить и запустить")) {
                const int i = gConsentAsk;
                mods_.allowNative(i, true);
                if (!mods_.mods[i].enabled) mods_.setEnabled(i, true);
                toast(mods_.mods[i].name + (mods_.mods[i].running ? ": нативный код разрешён, работает" : ": " + mods_.mods[i].error));
                gConsentAsk = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Отмена")) {
                gConsentAsk = -1;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}
