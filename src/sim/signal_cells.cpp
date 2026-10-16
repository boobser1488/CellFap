#include "signal_cells.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <sstream>

// ---------------------------------------------------------------- names of the inputs

const char* SignalCellInputs::name(int i) {
    static const char* kNames[SignalCellInputs::kCount] = {
        "S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "масса", "возраст", "азот", "связи", "свет", "пища", "травма",
        "память1", "память2", "память3", "память4",
    };
    return kNames[i >= 0 && i < SignalCellInputs::kCount ? i : 0];
}

// ---------------------------------------------------------------- example programs
//
// Written in real dypt and compiled by the same compiler as the player's code. Inputs: S1..S8 at places 0..7, the
// cell's memory at 15..18. Outputs: places 0..7 set S1..S8, 8..11 the memory; null leaves a place as it was.

namespace {

const char* kHeader =
    "// входы: [S1..S8, масса, возраст, азот, связи, свет, пища, травма, память1..память4]\n"
    "// ответ: [S1..S8, память1..память4]; null — оставить как было\n";

std::string num(float v) {
    std::ostringstream o;
    o << v;
    std::string s = o.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos) s += ".0";
    return s;
}

std::string sourceLogicGate(int op) {
    // Two inputs S1 and S2 (1 above 0.5), the result in S3.
    std::ostringstream o;
    o << "// Логический элемент: входы S1 и S2 (единица — выше 0.5), результат в S3.\n"
      << kHeader << "function on_tick(входы) {\n"
      << "    a = 0\n    b = 0\n"
      << "    if входы[0] > 0.5 { a = 1 }\n"
      << "    if входы[1] > 0.5 { b = 1 }\n";
    switch (op) {
        case 1: o << "    r = 0\n    if a + b > 0 { r = 1 }\n"; break;               // OR
        case 2: o << "    r = 0\n    if a + b == 1 { r = 1 }\n"; break;              // XOR
        case 3: o << "    r = 1 - a\n"; break;                                         // NOT (of S1)
        default: o << "    r = a * b\n"; break;                                         // AND
    }
    o << "    return [null, null, r, null, null, null, null, null]\n}\n";
    return o.str();
}

std::string sourceSwitch(float threshold) {
    // A flip-flop: every rising edge of S1 (it crosses the threshold) toggles S3 on or off.
    std::ostringstream o;
    o << "// Переключатель: каждый раз, когда S1 поднимается выше порога, S3 включается или выключается.\n"
      << kHeader << "function on_tick(входы) {\n"
      << "    состояние = входы[15]\n"
      << "    было = входы[16]\n"
      << "    сейчас = 0\n"
      << "    if входы[0] > " << num(threshold) << " { сейчас = 1 }\n"
      << "    if сейчас == 1 and было == 0 { состояние = 1 - состояние }\n"
      << "    return [null, null, состояние, null, null, null, null, null, состояние, сейчас, null, null]\n}\n";
    return o.str();
}

std::string sourceTimer(float periodTicks) {
    // A pulse on S3 every N ticks; each cell counts in its own memory.
    std::ostringstream o;
    o << "// Таймер: импульс на S3 каждые N тиков (тик — 1/400 часа). Счёт у каждой клетки свой, в памяти.\n"
      << kHeader << "function on_tick(входы) {\n"
      << "    счёт = входы[15] + 1\n"
      << "    импульс = 0\n"
      << "    if счёт >= " << num(periodTicks) << " {\n"
      << "        счёт = 0\n"
      << "        импульс = 1\n"
      << "    }\n"
      << "    return [null, null, импульс, null, null, null, null, null, счёт, null, null, null]\n}\n";
    return o.str();
}

std::string sourceConditional(float threshold) {
    // While S1 is above the threshold, S2 is passed on to S3; otherwise S3 is silent.
    std::ostringstream o;
    o << "// Передача по условию: пока S1 выше порога, S2 передаётся в S3, иначе S3 молчит.\n"
      << kHeader << "function on_tick(входы) {\n"
      << "    r = 0\n"
      << "    if входы[0] > " << num(threshold) << " { r = входы[1] }\n"
      << "    return [null, null, r, null, null, null, null, null]\n}\n";
    return o.str();
}

}  // namespace

const char* signalCellPresetName(SignalCellPreset p) {
    switch (p) {
        case SignalCellPreset::LogicGate: return "Логический элемент";
        case SignalCellPreset::Switch: return "Переключатель";
        case SignalCellPreset::Timer: return "Таймер";
        case SignalCellPreset::Conditional: return "Передача по условию";
        default: return "Свой код";
    }
}

std::string signalCellPresetSource(SignalCellPreset p) {
    switch (p) {
        case SignalCellPreset::LogicGate: return sourceLogicGate(0);
        case SignalCellPreset::Switch: return sourceSwitch(0.5f);
        case SignalCellPreset::Timer: return sourceTimer(200.0f);  // 200 ticks = half an hour
        case SignalCellPreset::Conditional: return sourceConditional(0.5f);
        default: return sourceConditional(0.5f);
    }
}

std::string signalCellPresetApply(SignalCellPreset p, const std::vector<float>& params) {
    switch (p) {
        case SignalCellPreset::LogicGate: return sourceLogicGate(params.empty() ? 0 : (int)std::lround(params[0]));
        case SignalCellPreset::Switch: return sourceSwitch(params.size() > 1 ? params[1] : 0.5f);
        case SignalCellPreset::Timer: return sourceTimer(params.size() > 2 ? std::max(1.0f, params[2]) : 200.0f);
        case SignalCellPreset::Conditional: return sourceConditional(params.size() > 1 ? params[1] : 0.5f);
        default: return signalCellPresetSource(p);
    }
}

// ---------------------------------------------------------------- what a program may not use

std::string signalCellSourceProblem(const std::string& src) {
    // Keywords and built-ins that reach outside the cell: other files and native code (import, extern, from ...
    // import, the __import__ functions, raw calls), raw process memory, threads, the console, sleeping, quitting.
    static const char* kForbidden[] = {
        "import", "extern", "from", "__import__", "__import_dll__", "__make_module__", "call_at", "call_bin",
        "load_bin", "free_bin", "malloc", "free", "mem_read", "mem_write", "peek", "peek32", "poke", "raw_alloc",
        "raw_free", "raw_get32", "raw_get8", "raw_set32", "raw_set8", "raw_setf32", "get_reg", "set_reg",
        "thread_spawn", "thread_join", "thread_yield", "thread_id", "thread_count", "sleep", "exit", "read",
        "readln", "sreadln", "write", "async", "await",
    };
    std::string ident;
    auto check = [&]() -> std::string {
        if (ident.empty()) return {};
        for (const char* f : kForbidden)
            if (ident == f) return "в программе клетки нельзя использовать «" + ident + "»";
        ident.clear();
        return {};
    };
    for (size_t i = 0; i < src.size();) {
        const unsigned char ch = (unsigned char)src[i];
        // comments
        if (ch == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            if (std::string r = check(); !r.empty()) return r;
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (ch == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            if (std::string r = check(); !r.empty()) return r;
            const size_t e = src.find("*/", i + 2);
            i = e == std::string::npos ? src.size() : e + 2;
            continue;
        }
        if (ch == '#') {  // dypt also takes # comments
            if (std::string r = check(); !r.empty()) return r;
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        // strings
        if (ch == '"' || ch == '\'') {
            if (std::string r = check(); !r.empty()) return r;
            const char q = (char)ch;
            ++i;
            while (i < src.size() && src[i] != q) i += src[i] == '\\' ? 2 : 1;
            ++i;
            continue;
        }
        // identifiers: ASCII letters, digits, _ and any UTF-8 byte (Russian names)
        if (std::isalnum(ch) || ch == '_' || ch >= 0x80) {
            ident += (char)ch;
            ++i;
            continue;
        }
        if (std::string r = check(); !r.empty()) return r;
        ++i;
    }
    return check();
}

// ---------------------------------------------------------------- the manager
//
// Every job handed to the executor owns its state (a shared_ptr captured by value) and touches nothing else: a job
// that outlives its watchdog — a slow program that does return in the end — must not write into the manager or the
// caller's stack, which have moved on.

SignalCellManager::~SignalCellManager() {
    clear();
    if (hung_) {
        // A thread still runs inside the library: unloading it would pull the code from under that thread.
        lib_.release();
        (void)host_.release();
    }
}

void SignalCellManager::startExecutor() {
    auto ex = std::make_shared<Executor>();
    ex->thread = std::thread([ex] {
        std::unique_lock<std::mutex> lk(ex->m);
        for (;;) {
            ex->cv.wait(lk, [&] { return ex->quit || ex->hasJob; });
            if (ex->quit) return;
            std::function<void()> job = std::move(ex->job);
            ex->hasJob = false;
            lk.unlock();
            job();
            job = nullptr;  // the job's state goes before the caller is told
            lk.lock();
            ex->done = true;
            ex->cv.notify_all();
        }
    });
    exec_ = ex;
}

bool SignalCellManager::exec(std::function<void()> fn, double timeoutMs) {
    if (!exec_) startExecutor();
    std::shared_ptr<Executor> ex = exec_;
    std::unique_lock<std::mutex> lk(ex->m);
    ex->job = std::move(fn);
    ex->hasJob = true;
    ex->done = false;
    ex->cv.notify_all();
    const bool finished =
        ex->cv.wait_for(lk, std::chrono::duration<double, std::milli>(timeoutMs), [&] { return ex->done; });
    if (finished) return true;
    lk.unlock();
    abandonExecutor();
    return false;
}

// The executor is stuck in a program that does not return. It cannot be stopped from outside: it is left to itself
// (detached, it keeps a core busy until the game ends) and every script it owned is let go without being touched —
// they belong to that thread. A fresh executor recompiles the programs from their sources when they are next used.
void SignalCellManager::abandonExecutor() {
    if (exec_ && exec_->thread.joinable()) exec_->thread.detach();
    exec_.reset();
    for (auto& p : programs_)
        if (p && p->script) (void)p->script.release();
    hung_ = true;
}

bool SignalCellManager::initialize(const std::string& libraryPath, std::string& error) {
    if (ready_) return true;
    if (libraryPath.empty()) {
        error = "путь к dypt_embed не задан";
        return false;
    }
    if (!lib_.load(libraryPath)) {
        error = "не удалось загрузить dypt_embed (" + libraryPath + ")";
        lastError_ = error;
        return false;
    }
    if (lib_.abiVersion() != 2) {
        error = "несовместимая версия dypt ABI: " + std::to_string(lib_.abiVersion()) + " (нужна 2)";
        lastError_ = error;
        lib_.unload();
        return false;
    }
    host_ = std::make_unique<dypt::HostContext>(&lib_);
    // The only name of the game a program may call: a line in the log.
    host_->setHostNames({"журнал"});
    auto logMutex = logMutex_;
    auto log = log_;
    host_->setHandler([logMutex, log](const std::string& name, const std::vector<dypt::Value>& args, dypt::Value& out) {
        if (name != "журнал") return 1;
        std::string line;
        for (const dypt::Value& a : args) {
            if (!line.empty()) line += ' ';
            if (a.kind == DY_STRING) {
                line += a.s;
            } else {
                char b[32];
                std::snprintf(b, sizeof(b), "%g", a.asFloat());
                line += b;
            }
        }
        std::lock_guard<std::mutex> lk(*logMutex);
        if (log->size() < 4096) *log += line + "\n";  // a chatty program does not grow it without end
        out = dypt::Value::null();
        return 0;
    });
    host_->setPrinter([logMutex, log](const std::string& text) {
        std::lock_guard<std::mutex> lk(*logMutex);
        if (log->size() < 4096) *log += text + "\n";
    });
    host_->init();
    ready_ = true;
    lastError_.clear();
    return true;
}

std::string SignalCellManager::takeLog() {
    std::lock_guard<std::mutex> lk(*logMutex_);
    std::string s;
    s.swap(*log_);
    return s;
}

// Closes scripts on the thread they belong to (the job owns them from now on).
void SignalCellManager::closeScripts(std::vector<dypt::ScriptedCell*> scripts) {
    if (scripts.empty()) return;
    if (!exec_) {  // nothing runs: the executor that owned them was abandoned; they stay where they are
        return;
    }
    auto list = std::make_shared<std::vector<dypt::ScriptedCell*>>(std::move(scripts));
    exec([list] {
        for (dypt::ScriptedCell* s : *list) delete s;
    }, limits.hangMs);
}

void SignalCellManager::clear() {
    std::vector<dypt::ScriptedCell*> close;
    for (auto& p : programs_)
        if (p && p->script) close.push_back(p->script.release());
    closeScripts(std::move(close));
    if (exec_) {
        {
            std::lock_guard<std::mutex> lk(exec_->m);
            exec_->quit = true;
        }
        exec_->cv.notify_all();
        if (exec_->thread.joinable()) exec_->thread.join();
        exec_.reset();
    }
    programs_.clear();
    freeSlots_.clear();
    byKey_.clear();
    turn_ = 0;
}

// Compiles lp.source on the executor. On failure lp is disabled with the message.
bool SignalCellManager::loadScript(LoadedProgram& lp) {
    if (lp.script) closeScripts({lp.script.release()});
    lp.errors = 0;
    lp.lastError.clear();
    lp.disabled = false;
    if (lp.source.size() > limits.maxSourceBytes) {
        lp.lastError = "исходник длиннее лимита (" + std::to_string(limits.maxSourceBytes) + " байт)";
        lp.disabled = true;
        return false;
    }
    if (std::string bad = signalCellSourceProblem(lp.source); !bad.empty()) {
        lp.lastError = bad;
        lp.disabled = true;
        return false;
    }
    struct Job {
        std::string source, err;
        dypt::HostContext* host;
        dypt::ScriptedCell* script = nullptr;
    };
    auto job = std::make_shared<Job>();
    job->source = lp.source;
    job->host = host_.get();
    const bool finished = exec([job] {
        auto s = std::make_unique<dypt::ScriptedCell>(job->host);
        if (s->load(job->source, job->err) && s->hasOnTick()) job->script = s.release();
        else if (job->err.empty()) job->err = "в программе нет функции on_tick(входы)";
    }, limits.hangMs * 4);
    if (!finished) job->err = "компиляция не уложилась во время";
    else if (job->script) lp.script.reset(job->script);
    if (!lp.script) {
        lp.lastError = job->err;
        lp.disabled = true;
        return false;
    }
    return true;
}

int SignalCellManager::programFor(int genome, int mode, const std::string& source) {
    if (!ready_) return -1;
    const uint64_t key = ((uint64_t)(uint32_t)genome << 32) | (uint32_t)mode;
    if (auto it = byKey_.find(key); it != byKey_.end()) {
        LoadedProgram& lp = *programs_[it->second];
        // Edited in the editor (or the executor was replaced): compile again.
        if (lp.source != source) {
            lp.source = source;
            loadScript(lp);
        } else if (!lp.script && !lp.disabled) {
            loadScript(lp);
        }
        return it->second;
    }
    if ((int)byKey_.size() >= limits.maxProgramsPerWorld) return -1;
    int slot;
    if (!freeSlots_.empty()) {
        slot = freeSlots_.back();
        freeSlots_.pop_back();
        programs_[slot] = std::make_unique<LoadedProgram>();
    } else {
        slot = (int)programs_.size();
        programs_.push_back(std::make_unique<LoadedProgram>());
    }
    programs_[slot]->source = source;
    loadScript(*programs_[slot]);
    byKey_[key] = slot;
    return slot;
}

void SignalCellManager::dropUnused(const std::vector<std::pair<int, int>>& liveKeys) {
    std::unordered_map<uint64_t, char> keep;
    for (auto& km : liveKeys) keep[((uint64_t)(uint32_t)km.first << 32) | (uint32_t)km.second] = 1;
    std::vector<dypt::ScriptedCell*> close;
    for (auto it = byKey_.begin(); it != byKey_.end();) {
        if (!keep.count(it->first)) {
            std::unique_ptr<LoadedProgram>& p = programs_[it->second];
            if (p && p->script) close.push_back(p->script.release());
            p.reset();
            freeSlots_.push_back(it->second);
            it = byKey_.erase(it);
        } else {
            ++it;
        }
    }
    closeScripts(std::move(close));
}

int SignalCellManager::runTick(const std::vector<Run>& runs, std::vector<std::pair<int, SignalCellResult>>& results) {
    results.clear();
    if (!ready_ || runs.empty()) return 0;
    const size_t n = runs.size();
    if (turn_ >= n) turn_ = 0;
    // The job: the scripts (owned by the executor's thread anyway), the inputs and room for the answers.
    struct Job {
        std::vector<dypt::ScriptedCell*> scripts;
        std::vector<int> keys;
        std::vector<float> inputs;
        std::vector<SignalCellResult> outs;
        std::vector<std::string> errs;
        std::vector<char> done, failed;
        size_t start = 0, ran = 0;
        double budgetMs = 4.0;
        int maxOutputs = kSignalCellResults;
        std::atomic<int> running{-1};
    };
    auto job = std::make_shared<Job>();
    job->scripts.resize(n, nullptr);
    job->keys.resize(n, -1);
    job->inputs.resize(n * SignalCellInputs::kCount);
    job->outs.resize(n);
    job->errs.resize(n);
    job->done.assign(n, 0);
    job->failed.assign(n, 0);
    job->start = turn_;
    job->budgetMs = limits.tickBudgetMs;
    job->maxOutputs = std::min(kSignalCellResults, limits.maxOutputs);
    bool any = false;
    for (size_t r = 0; r < n; ++r) {
        LoadedProgram* lp = program(runs[r].programKey);
        if (!lp || lp->disabled || !lp->script) continue;
        job->scripts[r] = lp->script.get();
        job->keys[r] = runs[r].programKey;
        std::copy(runs[r].inputs, runs[r].inputs + SignalCellInputs::kCount, job->inputs.begin() + r * SignalCellInputs::kCount);
        any = true;
    }
    if (!any) return 0;
    // All the tick's programs in one trip to the executor. It stops when the tick's time is used up; the next tick
    // starts with the cells that did not get their turn.
    const bool finished = exec([job] {
        const auto t0 = std::chrono::steady_clock::now();
        const size_t n = job->scripts.size();
        std::vector<float> in(SignalCellInputs::kCount), out;
        for (size_t k = 0; k < n; ++k) {
            const size_t r = (job->start + k) % n;
            dypt::ScriptedCell* s = job->scripts[r];
            if (!s) continue;
            if (job->ran > 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > job->budgetMs)
                break;
            std::copy(job->inputs.begin() + r * SignalCellInputs::kCount,
                      job->inputs.begin() + (r + 1) * SignalCellInputs::kCount, in.begin());
            job->running = job->keys[r];
            std::string err;
            const bool ok = s->onTick(in, out, err);
            job->running = -1;
            ++job->ran;
            job->done[r] = 1;
            if (!ok) {
                job->failed[r] = 1;
                job->errs[r] = err;
                continue;
            }
            SignalCellResult& res = job->outs[r];
            res.fill(std::nanf(""));
            for (size_t i = 0; i < out.size() && i < (size_t)job->maxOutputs; ++i)
                if (std::isfinite(out[i])) res[i] = std::clamp(out[i], -100.0f, 100.0f);
        }
    }, limits.hangMs);
    if (!finished) {
        // A program hung: the one the executor was in is switched off; the others recompile on the next tick.
        if (LoadedProgram* lp = program(job->running.load())) {
            lp->disabled = true;
            lp->lastError = "программа не уложилась в " + std::to_string((int)limits.hangMs) +
                            " мс (бесконечный цикл?) и отключена";
            lastError_ = lp->lastError;
        }
        return 1;
    }
    turn_ = (turn_ + job->ran) % n;
    int errors = 0;
    for (size_t r = 0; r < n; ++r) {
        if (!job->done[r]) continue;
        LoadedProgram* lp = program(job->keys[r]);
        if (lp) ++lp->calls;
        if (job->failed[r]) {
            ++errors;
            if (lp) {
                lp->lastError = job->errs[r];
                lastError_ = job->errs[r];
                if (++lp->errors >= limits.maxErrorsPerProgram) {
                    lp->disabled = true;
                    lp->lastError = "отключена после " + std::to_string(lp->errors) + " ошибок: " + job->errs[r];
                    lastError_ = lp->lastError;
                }
            }
            continue;
        }
        results.emplace_back(runs[r].cellIndex, job->outs[r]);
    }
    return errors;
}

bool SignalCellManager::testProgram(const std::string& source, std::string& error, std::vector<float>* outputs,
                                    const std::vector<float>* inputs) {
    error.clear();
    if (!ready_) {
        error = "dypt недоступен: " + (lastError_.empty() ? std::string("не инициализирован") : lastError_);
        return false;
    }
    if (source.size() > limits.maxSourceBytes) {
        error = "исходник длиннее лимита (" + std::to_string(limits.maxSourceBytes) + " байт)";
        return false;
    }
    if (std::string bad = signalCellSourceProblem(source); !bad.empty()) {
        error = bad;
        return false;
    }
    struct Job {
        std::string source, err;
        dypt::HostContext* host;
        std::vector<float> in, out;
        bool ok = false;
    };
    auto job = std::make_shared<Job>();
    job->source = source;
    job->host = host_.get();
    job->in.assign(SignalCellInputs::kCount, 0.0f);
    job->in[SignalCellInputs::kLight] = 1.0f;
    if (inputs)
        for (size_t k = 0; k < inputs->size() && k < job->in.size(); ++k) job->in[k] = (*inputs)[k];
    const bool finished = exec([job] {
        dypt::ScriptedCell test(job->host);
        if (!test.load(job->source, job->err)) return;
        if (!test.hasOnTick()) {
            job->err = "в программе нет функции on_tick(входы)";
            return;
        }
        job->ok = test.onTick(job->in, job->out, job->err);
    }, limits.hangMs * 4);
    if (!finished) {
        error = "программа не уложилась в " + std::to_string((int)(limits.hangMs * 4)) + " мс (бесконечный цикл?)";
        return false;
    }
    if (!job->ok) {
        error = job->err;
        return false;
    }
    if (job->out.size() > (size_t)limits.maxOutputs) job->out.resize((size_t)limits.maxOutputs);
    if (outputs) *outputs = job->out;
    return true;
}

bool SignalCellManager::testSequence(const std::string& source, std::vector<float> inputs, int steps,
                                     std::vector<std::vector<float>>& trace, std::string& error, int& failedStep) {
    error.clear();
    trace.clear();
    failedStep = -1;
    if (!ready_) {
        error = "dypt недоступен: " + (lastError_.empty() ? std::string("не инициализирован") : lastError_);
        return false;
    }
    if (source.size() > limits.maxSourceBytes) {
        error = "исходник длиннее лимита (" + std::to_string(limits.maxSourceBytes) + " байт)";
        return false;
    }
    if (std::string bad = signalCellSourceProblem(source); !bad.empty()) {
        error = bad;
        return false;
    }
    inputs.resize(SignalCellInputs::kCount, 0.0f);
    struct Job {
        std::string source, err;
        dypt::HostContext* host;
        std::vector<float> in;
        std::vector<std::vector<float>> trace;
        int steps = 1, failed = -1, maxOutputs = kSignalCellResults;
        bool compiled = false;
    };
    auto job = std::make_shared<Job>();
    job->source = source;
    job->host = host_.get();
    job->in = std::move(inputs);
    job->steps = std::clamp(steps, 1, 4000);
    job->maxOutputs = std::min(kSignalCellResults, limits.maxOutputs);
    const double timeout = limits.hangMs * 4 + job->steps * 2.0;  // a long test may take its time, a hang may not
    const bool finished = exec([job] {
        dypt::ScriptedCell s(job->host);
        if (!s.load(job->source, job->err)) return;
        if (!s.hasOnTick()) {
            job->err = "в программе нет функции on_tick(входы)";
            return;
        }
        job->compiled = true;
        std::vector<float> out;
        for (int k = 0; k < job->steps; ++k) {
            if (!s.onTick(job->in, out, job->err)) {
                job->failed = k;
                return;
            }
            // As in the dish: numbers set the signals (in its range) and the memory; null leaves them.
            for (size_t i = 0; i < out.size() && i < (size_t)job->maxOutputs; ++i) {
                if (!std::isfinite(out[i])) continue;
                if (i < (size_t)kSignalCount) job->in[i] = std::clamp(out[i], -20.0f, 20.0f);
                else job->in[SignalCellInputs::kMemory + (i - kSignalCount)] = out[i];
            }
            job->trace.push_back(job->in);
        }
    }, timeout);
    if (!finished) {
        error = "программа не уложилась во время (бесконечный цикл?) — проверка прервана";
        return false;
    }
    trace = std::move(job->trace);
    failedStep = job->failed;
    error = job->err;
    return job->compiled && job->failed < 0;
}

const LoadedProgram* SignalCellManager::findBySource(const std::string& source) const {
    for (const auto& [key, slot] : byKey_)
        if (programs_[slot] && programs_[slot]->source == source) return programs_[slot].get();
    return nullptr;
}

void SignalCellManager::retry(const std::string& source) {
    for (const auto& [key, slot] : byKey_) {
        LoadedProgram* lp = programs_[slot].get();
        if (!lp || lp->source != source) continue;
        lp->disabled = false;
        lp->errors = 0;
        lp->lastError.clear();
        lp->source.clear();  // differs from the genome's text: programFor recompiles it (closing the old script on its thread)
    }
}
