#pragma once
// Mods: dypt scripts that extend the game (docs/mods.md).
//
// A mod (modpack.hpp) brings mod.txt — who it is, how often it ticks, its settings — and mod.dpt, a script with up to
// four entry points:
//   загрузка()               once when it is switched on: add genomes and programs, prepare its state
//   тик(состояние)           every `tick` simulation ticks: look at the dish, feed it, change its conditions
//   настройки_изменены()     after the player changed one of its settings
//   новая_чашка()            the dish was replaced (a new experiment, a loaded file)
//   выгрузка()               when it is switched off
// The script calls the game through a set of functions (журнал, сообщение, настройка, добавить_геном,
// добавить_программу, добавить_пищу, задать, узнать, файл_мода, случайное, and the whole dish: клетка, ячеек,
// клетки_рядом, соседи, пища, изменить_клетку, убить_клетку, добавить_клетку, убрать_пищу, сохранить_чашку,
// открыть_чашку). It reads the dish while it runs, but everything it changes is collected and applied by the game
// afterwards; a script that hangs is cut off from the dish before the game goes on.
//
// Safety is the program cells': the same blocklist of functions (signalCellSourceProblem) and a watchdog executor of
// its own (dypt_executor.hpp). A mod that does not return in time, or keeps failing, is switched off with the reason.
//
// A mod may also bring native code: shared libraries under native/<platform>/ speaking the C ABI of
// sdk/cellfap_mod.h (mod_api.cpp), written in any language. They get the whole dish, hooks before and after every
// tick, per-cell behaviour, births and deaths, a panel and drawing over the dish. Native code can do anything the
// game can, so it is never loaded on sight: the player allows it per mod, and the consent is remembered together with
// the SHA-256 of exactly those files (settings/mods.ini) — different files ask again. Libraries are loaded from a
// copy in mods/.cache/<id>/<hash>/, never from the mod itself.
#include "dypt.hpp"
#include "dypt_executor.hpp"
#include "genome.hpp"
#include "mod_api.hpp"
#include "mod_shaders.hpp"
#include "modpack.hpp"
#include "native_mods.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

class World;

struct ModSetting {
    enum Kind { Number, Flag, Choice };
    std::string key, label;
    Kind kind = Number;
    double value = 0, def = 0, min = 0, max = 1;
    std::vector<std::string> options;  // Choice: the names; value is the index
};

struct ModGenome {
    std::string mod, name;
    Genome genome;
};
struct ModProgram {
    std::string mod, name, source;
};

struct ModInfo {
    std::string id, name, version, author, description;
    std::string path;       // the folder or the .cfmod file
    bool packed = false;
    int tickEvery = 0;      // 0: no тик
    std::vector<ModSetting> settings;
    bool enabled = false;   // the player's choice (kept in settings/mods.ini)
    bool running = false;   // loaded and загрузка() went through
    std::string error;      // why it does not run (or what went wrong last)
    std::vector<std::string> log;  // the last lines it wrote
    uint64_t ticks = 0;     // тик calls
    double lastMs = 0, worstMs = 0;
    int runtimeErrors = 0;
    std::shared_ptr<const ModFiles> files;
    dypt::ScriptedCell* script = nullptr;  // belongs to the executor thread
    bool hasTick = false, hasSettingsHook = false, hasUnload = false, hasDishHook = false;
    bool tickDue = false;   // its тик is waiting for room in a tick's budget
    bool hasScript = false; // mod.dpt is there
    std::string duplicate;  // another mod in mods/ has the same id: this one never starts (why)

    // Native code (native/<platform>/).
    NativeFiles native;
    std::string nativeLib;        // mod.txt `native <file>`: the library with the entry points
    bool nativeAllowed = false;   // the player allowed exactly these files (their hash is in settings/mods.ini)
    bool nativeChanged = false;   // allowed before, but the files are different now
    std::shared_ptr<NativeMod> nat;  // the loaded library while it runs
    double nativeMs = 0;          // its callbacks per tick (smoothed)
    // preview.png (RGBA, decoded when the files are read) and why it could not be shown.
    std::vector<uint8_t> preview;
    int previewW = 0, previewH = 0;
    std::string previewError;
    // shaders/*.spv: replacements for the game's pipelines while the mod runs (the game applies them).
    std::shared_ptr<const ModShaders> shaders;
    std::string shaderError;  // a file that is not SPIR-V, or Vulkan refused it / another mod owns the slot (the game)
    // The dish it knows: a different one (or the same one reset) calls world_reset / новая_чашка.
    const World* dishWorld = nullptr;
    uint64_t dishReset = 0;
};

// Parses mod.txt into `m` (id, name, ..., tick, settings). Returns false with the reason when id or name is missing.
bool parseModInfo(const std::string& text, ModInfo& m, std::string& error);

class ModManager {
public:
    ModManager() = default;
    ~ModManager();
    ModManager(const ModManager&) = delete;
    ModManager& operator=(const ModManager&) = delete;

    // Connects to dypt_embed (the library the program cells use). Without it mods are listed but cannot run.
    bool initialize(const std::string& libraryPath, std::string& error);
    bool ready() const { return ready_; }

    // Which mods are on and their setting values, by mod id (settings/mods.ini). Read before scan, written after
    // every change.
    void loadState(const std::string& iniPath);
    bool saveState(const std::string& iniPath) const;

    // Stops every mod, reads mods/ anew (folders and packs) and starts the enabled ones.
    void scan(const std::string& dir);
    const std::string& directory() const { return dir_; }

    void setEnabled(int i, bool on);
    void restart(int i);           // reads the mod from disk again and starts it if it is on
    // A setting was edited in the interface: remembered, and the mod is told (with the dish, when the caller holds it).
    void settingChanged(int i, World* w);
    void resetSettings(int i, World* w);

    // After a world step, on the thread that steps the world (the world is not touched while a script runs).
    void tick(World& w);
    // Before a world step, on the same thread. false: a native mod skips the physics of this tick.
    bool beforeStep(World& w);
    // The dish native mods get right after they load (world_reset). The game attaches its dish once at start-up.
    void attach(World* w) { world_ = w; }

    // ---- native code
    // The player's consent for this mod's native code (exactly these files); the mod starts when it is switched on.
    void allowNative(int i, bool allow);
    // UI thread, every frame (under the world lock): notices a replaced dish when the simulation stands still.
    void frame(World& w);
    bool hasPanel(int i) const;
    void drawPanel(int i, World* w, ModUi& ui);
    void drawOverlay(World& w, ModDraw& d);
    std::string cacheDir() const { return dir_ + "/.cache"; }
    std::string dataDirOf(const std::string& id) const { return dir_ + "/.data/" + id; }

    // Dishes the mods asked to save or open (substrates/<file>.substrate); the game takes and performs them.
    struct DishRequest {
        std::string mod;
        bool save = false;
        std::string file;
    };
    std::vector<DishRequest> dishRequests;

    // For the native API (mod_api.cpp).
    bool inCallback() const { return wctx_.active && wctx_.thread == std::this_thread::get_id(); }
    void addLog(int i, const std::string& line);
    void addMessage(int i, const std::string& text);
    void settingsEdited(int i) { remember(mods[i]); }
    void genomesChanged() { ++generation; }

    // What the running mods added.
    std::vector<ModGenome> genomes;
    std::vector<ModProgram> programs;
    int generation = 0;  // grows when genomes or programs change (the interface rebuilds its lists)

    std::vector<ModInfo> mods;
    // Messages for the screen (сообщение), oldest first.
    std::vector<std::string> takeMessages();
    // Something to write to mods.ini changed (a switch, a setting, a mod the watchdog turned off).
    bool takeStateDirty() {
        const bool d = stateDirty_;
        stateDirty_ = false;
        return d;
    }

    double hangMs = 500.0;      // one call longer than this: the mod is switched off
    double tickBudgetMs = 8.0;  // all mods together in one simulation tick (the rest wait for the next)
    int maxErrors = 5;          // runtime errors after which a mod is switched off

private:
    struct Saved {
        bool enabled = false;
        std::string nativeHash;  // the native code the player allowed
        std::map<std::string, double> values;
    };
    bool start(int i);
    void stop(int i, bool callUnload);
    bool startNative(int i);
    void stopNative(int i, bool callUnload);
    // Runs one callback of a native mod with the dish handle valid; an error return or a broken rule switches the mod
    // off (with what it said through set_error). false: the mod no longer runs.
    template <class F>
    bool callNative(int i, const char* what, World* w, F&& f);
    void nativeFailed(int i, const std::string& why);
    // The executor thread was abandoned (a call, a compile or a close hung): see mods.cpp.
    void executorLost(int culprit, const std::string& why);
    bool shuttingDown_ = false;
    void checkReset(World& w);
    void dispatchNative(World& w);
    World* world_ = nullptr;
    uint64_t seenReset_ = ~0ull;
    const World* seenWorld_ = nullptr;
    CellFapWorld wctx_;
    CellFapUi uictx_;
    CellFapDraw drawctx_;
    bool loadFiles(ModInfo& m);
    void remember(const ModInfo& m);
    struct CallResult;
    bool callMod(int i, const std::string& fn, const std::vector<dypt::Value>& args, World* w, CallResult& out);
    void dropContributions(const std::string& id);

    bool ready_ = false;
    dypt::Library lib_;
    std::unique_ptr<dypt::HostContext> host_;
    DyptExecutor exec_;
    std::string dir_;
    std::map<std::string, Saved> saved_;
    std::vector<std::string> messages_;
    size_t turn_ = 0;
    bool stateDirty_ = false;
};
