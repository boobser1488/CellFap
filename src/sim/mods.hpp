#pragma once
// Mods: dypt scripts that extend the game (docs/mods.md).
//
// A mod (modpack.hpp) brings mod.txt — who it is, how often it ticks, its settings — and mod.dpt, a script with up to
// four entry points:
//   загрузка()               once when it is switched on: add genomes and programs, prepare its state
//   тик(состояние)           every `tick` simulation ticks: look at the dish, feed it, change its conditions
//   настройки_изменены()     after the player changed one of its settings
//   выгрузка()               when it is switched off
// The script calls the game through a small set of functions (журнал, сообщение, настройка, добавить_геном,
// добавить_программу, добавить_пищу, задать, узнать, файл_мода, случайное). Everything it changes is collected while it
// runs and applied by the game afterwards, so a script that hangs never touches the dish.
//
// Safety is the program cells': the same blocklist of functions (signalCellSourceProblem) and a watchdog executor of
// its own (dypt_executor.hpp). A mod that does not return in time, or keeps failing, is switched off with the reason.
#include "dypt.hpp"
#include "dypt_executor.hpp"
#include "genome.hpp"
#include "modpack.hpp"

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
    bool hasTick = false, hasSettingsHook = false, hasUnload = false;
    bool tickDue = false;   // its тик is waiting for room in a tick's budget
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
        std::map<std::string, double> values;
    };
    bool start(int i);
    void stop(int i, bool callUnload);
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
