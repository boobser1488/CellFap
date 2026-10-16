#pragma once
// Programmable signal cells: the player writes a dypt program, the cell runs it as part of the simulation step.
//
// How it works and why:
//   * The program's source lives in the genome (Genome::scripts, one per mode): it travels with the genome through
//     division, mutation and saved dishes.
//   * It runs through dypt_open / dypt_call, the C ABI of the embeddable dypt (dypt.hpp): the real language, its own
//     compiler and virtual machine, not a stand-in.
//   * One compiled script per (genome, mode); the cells of that mode share its code but each keeps its own memory
//     (four numbers in Cell::progMem) that the program reads and writes.
//   * Every call to dypt happens on one executor thread with a watchdog. dypt has no instruction limit of its own:
//     a program that does not return in time is switched off with a message and the game goes on; the executor it
//     hung is abandoned (it keeps one CPU core busy until the game is restarted — a documented limitation).
//   * The step gathers every program cell's inputs from the state of tick N; all programs run; their outputs are
//     applied to the cells together at the end of the tick. The order of the cells therefore does not matter.
#include "dypt.hpp"
#include "dypt_executor.hpp"
#include "genome.hpp"

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr int kSignalCellMemory = 4;  // numbers of memory each program cell keeps between calls

// What the program gets in `входы`: the signals S1..S8, some state of the cell, and its memory.
struct SignalCellInputs {
    static constexpr int kSignals = kSignalCount;   // 0..7   S1..S8
    static constexpr int kMass = kSignals;          // 8      mass / maximum
    static constexpr int kAge = kSignals + 1;       // 9      age / limit
    static constexpr int kNitrogen = kSignals + 2;  // 10     nitrogen 0..1
    static constexpr int kLinks = kSignals + 3;     // 11     links / maximum
    static constexpr int kLight = kSignals + 4;     // 12     light 0..1
    static constexpr int kFood = kSignals + 5;      // 13     food within reach 0..1
    static constexpr int kInjury = kSignals + 6;    // 14     injury 0..1
    static constexpr int kMemory = kSignals + 7;    // 15..18 the cell's memory
    static constexpr int kCount = kMemory + kSignalCellMemory;
    static const char* name(int i);
};

// What it returns: a list; places 0..7 set the levels of S1..S8, places 8..11 the new memory. null (or a shorter
// list) leaves that place as it was. Stored as NaN = "leave it".
constexpr int kSignalCellOutputs = kSignalCount;
constexpr int kSignalCellResults = kSignalCount + kSignalCellMemory;
using SignalCellResult = std::array<float, kSignalCellResults>;

// The example cells: a logic gate, a switch (flip-flop), a timer and a conditional relay.
enum class SignalCellPreset : uint8_t { LogicGate, Switch, Timer, Conditional, Custom, Count };
const char* signalCellPresetName(SignalCellPreset p);
std::string signalCellPresetSource(SignalCellPreset p);
// The preset with parameters: 0 = kind of logic (0 AND, 1 OR, 2 XOR, 3 NOT), 1 = threshold, 2 = period in ticks.
std::string signalCellPresetApply(SignalCellPreset p, const std::vector<float>& params);

// Limits on the player's code, checked while it runs (not only when it is saved).
struct SandboxLimits {
    size_t maxSourceBytes = 16384;  // the source
    int maxOutputs = kSignalCellResults;  // places of the returned list that are read (the rest is ignored)
    int maxProgramsPerWorld = 64;   // different programs alive at once
    int everyTicks = 1;             // how often the programs run: every N-th simulation tick
    double tickBudgetMs = 4.0;      // time for all program cells in one tick; the rest wait for the next tick (in turn)
    double hangMs = 250.0;          // one call longer than this: the program is switched off (watchdog)
    int maxErrorsPerProgram = 8;    // runtime errors after which a program is switched off until edited
};

// Checks a source for things a cell program may not use (files, native code, raw memory, threads, quitting the
// game...). Returns an empty string when it is acceptable, otherwise the reason. The check reads identifiers and
// keywords outside strings and comments; dypt offers no sandbox of its own, so this is a blocklist — see the docs.
std::string signalCellSourceProblem(const std::string& source);

// One compiled script, shared by the cells of one (genome, mode).
struct LoadedProgram {
    std::string source;
    std::unique_ptr<dypt::ScriptedCell> script;  // lives on the executor thread
    int errors = 0;
    std::string lastError;
    bool disabled = false;
    uint64_t calls = 0;
};

class SignalCellManager {
public:
    SignalCellManager() = default;
    ~SignalCellManager();
    SignalCellManager(const SignalCellManager&) = delete;
    SignalCellManager& operator=(const SignalCellManager&) = delete;

    // Connects to the dypt library (dypt_embed.dll / libdypt_embed.so). Without it the game works as always; the
    // program cells simply stay silent.
    bool initialize(const std::string& libraryPath, std::string& error);
    bool ready() const { return ready_; }
    const std::string& lastError() const { return lastError_; }

    // Compiles a source into a scratch script and runs on_tick once with the given inputs (zeros, light 1, when
    // none): returns the compile or run error, or the outputs (NaN = "left as it was").
    bool testProgram(const std::string& source, std::string& error, std::vector<float>* outputs = nullptr,
                     const std::vector<float>* inputs = nullptr);

    struct Run {
        int cellIndex;
        int programKey;
        const float* inputs;  // SignalCellInputs::kCount values
        bool clMode;
    };
    // Runs the program cells of one tick (as many as the tick's budget allows, in turn). Returns the number of
    // errors; results: (cell index, result) for every cell that ran.
    int runTick(const std::vector<Run>& runs, std::vector<std::pair<int, SignalCellResult>>& results);
    // Whether the programs run at this tick (SandboxLimits::everyTicks).
    bool dueThisTick() { return ++tickCounter_ % (uint64_t)std::max(1, limits.everyTicks) == 0; }

    // (genome, mode) -> index of its program (compiled on first use, recompiled when the source changes).
    int programFor(int genome, int mode, const std::string& source);
    LoadedProgram* program(int key) { return key >= 0 && key < (int)programs_.size() ? programs_[key].get() : nullptr; }
    int programCount() const { return (int)byKey_.size(); }
    // Programs no cell runs any more are closed and their slots reused.
    void dropUnused(const std::vector<std::pair<int, int>>& liveKeys);
    void clear();
    // The newest error of any program (for the interface), and what the programs printed.
    std::string takeLog();
    // Runs on_tick `steps` times on one compiled copy, the way the dish feeds it: each answer sets the signals and the
    // memory the next call sees. trace[k] = the inputs after call k (kCount numbers). failedStep: the call that failed.
    bool testSequence(const std::string& source, std::vector<float> inputs, int steps, std::vector<std::vector<float>>& trace,
                      std::string& error, int& failedStep);
    // The program running in the dish with exactly this source (its calls, errors), or nullptr.
    const LoadedProgram* findBySource(const std::string& source) const;
    // A switched-off program gets another chance: it is compiled afresh the next time a cell runs it.
    void retry(const std::string& source);

    SandboxLimits limits;

private:
    // The thread every dypt call runs on, with a watchdog (dypt_executor.hpp).
    // Runs fn on the executor and waits up to timeoutMs. false: it hung — the executor is abandoned.
    bool exec(std::function<void()> fn, double timeoutMs);
    void closeScripts(std::vector<dypt::ScriptedCell*> scripts);
    void abandonExecutor();
    bool loadScript(LoadedProgram& lp);

    bool ready_ = false;
    std::string lastError_;
    dypt::Library lib_;
    std::unique_ptr<dypt::HostContext> host_;
    DyptExecutor exec_;
    std::vector<std::unique_ptr<LoadedProgram>> programs_;
    std::vector<int> freeSlots_;
    std::unordered_map<uint64_t, int> byKey_;
    bool hung_ = false;                // an executor was abandoned inside a program: the library must stay loaded
    size_t turn_ = 0;                  // where the next tick starts when the budget cut the last one short
    uint64_t tickCounter_ = 0;
    // What the programs printed (shared with the host callbacks, which may outlive a hung executor).
    std::shared_ptr<std::mutex> logMutex_ = std::make_shared<std::mutex>();
    std::shared_ptr<std::string> log_ = std::make_shared<std::string>();
};
