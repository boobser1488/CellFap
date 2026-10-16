#include "mods.hpp"

#include "serialize.hpp"
#include "signal_cells.hpp"  // signalCellSourceProblem
#include "world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- what one call of a mod may see and change

// The parameters of the dish a mod may read (узнать) and change (задать), with their ranges.
struct WorldParam {
    const char* name;
    float Settings::*field;
    float lo, hi;
};
const WorldParam kParams[] = {
    {"свет", &Settings::lightAmount, 0.0f, 2.0f},
    {"питание", &Settings::nutrientRate, 0.0f, 15.0f},
    {"радиация", &Settings::radiation, 0.0f, 0.09f},
    {"вязкость", &Settings::viscosity, 0.0f, 150.0f},
    {"гравитация", &Settings::gravity, 0.0f, 1.0f},
    {"солёность", &Settings::salinity, 0.0f, 1.0f},
    {"нитраты", &Settings::nitrates, 0.0f, 1.0f},
    {"день", &Settings::dayLength, 0.0f, 48.0f},
    {"направление_света", &Settings::lightAngle, -1000.0f, 1000.0f},
};
const WorldParam* findParam(const std::string& name) {
    for (const WorldParam& p : kParams)
        if (name == p.name) return &p;
    if (name == "соленость") return findParam("солёность");
    return nullptr;
}

constexpr size_t kMaxLogLines = 200;
constexpr int kMaxFoodPerCall = 500;
constexpr int kMaxMessagesPerCall = 10;
constexpr int kMaxAdded = 64;  // genomes and programs per mod

const std::vector<std::string> kHostNames = {"журнал",     "сообщение", "настройка", "добавить_геном", "добавить_программу",
                                             "добавить_пищу", "задать", "узнать", "файл_мода", "случайное"};

struct CallCtx {
    std::string modId;
    bool loading = false;  // загрузка(): the only time genomes and programs can be added
    std::vector<ModSetting> settings;
    std::shared_ptr<const ModFiles> files;
    std::map<std::string, double> world;  // узнать
    bool haveWorld = false;
    float dishRadius = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    // what it did
    std::vector<std::string> log, messages;
    struct Food {
        float x, y, m;
    };
    std::vector<Food> food;
    std::vector<std::pair<const WorldParam*, float>> sets;
    std::vector<ModGenome> genomes;
    std::vector<ModProgram> programs;
};
thread_local CallCtx* t_ctx = nullptr;  // the call running on this executor thread

std::string joinArgs(const std::vector<dypt::Value>& args) {
    std::string line;
    for (const dypt::Value& a : args) {
        if (a.kind == DY_STRING) {
            line += a.s;
        } else if (a.kind == DY_NULL) {
            line += "null";
        } else if (a.kind == DY_BOOL) {
            line += a.b ? "true" : "false";
        } else {
            char b[32];
            std::snprintf(b, sizeof(b), "%g", a.asFloat());
            line += b;
        }
    }
    return line;
}

int fail(dypt::Value& out, const std::string& why) {
    out = dypt::Value::string(why);
    return 1;
}

int hostCall(const std::string& name, const std::vector<dypt::Value>& args, dypt::Value& out) {
    CallCtx* c = t_ctx;
    if (!c) return 1;
    out = dypt::Value::null();
    auto str = [&](size_t k) { return k < args.size() && args[k].kind == DY_STRING ? args[k].s : std::string(); };
    auto num = [&](size_t k) { return k < args.size() ? args[k].asFloat() : 0.0; };
    if (name == "журнал") {
        if (c->log.size() < kMaxLogLines) c->log.push_back(joinArgs(args));
        return 0;
    }
    if (name == "сообщение") {
        if ((int)c->messages.size() < kMaxMessagesPerCall) c->messages.push_back(joinArgs(args).substr(0, 300));
        return 0;
    }
    if (name == "настройка") {
        const std::string key = str(0);
        for (const ModSetting& s : c->settings)
            if (s.key == key) {
                if (s.kind == ModSetting::Flag) out = dypt::Value::boolean(s.value != 0);
                else if (s.kind == ModSetting::Choice)
                    out = dypt::Value::string(s.options.empty() ? std::string()
                                                                 : s.options[std::clamp((size_t)std::max(0.0, s.value), (size_t)0, s.options.size() - 1)]);
                else if (s.value == std::floor(s.value) && std::fabs(s.value) < 1e15) out = dypt::Value::integer((int64_t)s.value);  // for ... to
                else out = dypt::Value::floating(s.value);
                return 0;
            }
        return fail(out, "нет настройки «" + key + "» в mod.txt");
    }
    if (name == "файл_мода") {
        auto it = c->files->find(modRelativePath(str(0)));
        if (it == c->files->end()) return fail(out, "в моде нет файла «" + str(0) + "»");
        out = dypt::Value::string(it->second);
        return 0;
    }
    if (name == "случайное") {
        c->rng ^= c->rng << 13;
        c->rng ^= c->rng >> 7;
        c->rng ^= c->rng << 17;
        out = dypt::Value::floating((double)(c->rng >> 11) / 9007199254740992.0);
        return 0;
    }
    if (name == "добавить_геном") {
        if (!c->loading) return fail(out, "геномы добавляются только в загрузка()");
        if ((int)c->genomes.size() >= kMaxAdded) return fail(out, "не больше 64 геномов на мод");
        const std::string title = str(0), src = str(1);
        auto it = c->files->find(modRelativePath(src));
        const std::string text = it != c->files->end() ? it->second : src;
        ModGenome mg;
        mg.mod = c->modId;
        if (!genomeFromText(mg.genome, text)) return fail(out, "«" + src.substr(0, 60) + "» — не файл мода и не текст генома");
        mg.name = title.empty() ? mg.genome.name : title;
        if (!title.empty()) mg.genome.name = title;
        c->genomes.push_back(std::move(mg));
        return 0;
    }
    if (name == "добавить_программу") {
        if (!c->loading) return fail(out, "программы добавляются только в загрузка()");
        if ((int)c->programs.size() >= kMaxAdded) return fail(out, "не больше 64 программ на мод");
        const std::string title = str(0), src = str(1);
        auto it = c->files->find(modRelativePath(src));
        ModProgram mp{c->modId, title, it != c->files->end() ? it->second : src};
        if (std::string bad = signalCellSourceProblem(mp.source); !bad.empty()) return fail(out, "«" + title + "»: " + bad);
        c->programs.push_back(std::move(mp));
        return 0;
    }
    if (name == "добавить_пищу") {
        if (!c->haveWorld) return fail(out, "пища добавляется в тик()");
        if ((int)c->food.size() >= kMaxFoodPerCall) return fail(out, "не больше 500 порций за вызов");
        const float x = (float)num(0), y = (float)num(1), m = (float)std::clamp(args.size() > 2 ? num(2) : 0.3, 0.01, 2.0);
        if (!std::isfinite(x) || !std::isfinite(y)) return fail(out, "координаты — не числа");
        c->food.push_back({x, y, m});
        return 0;
    }
    if (name == "задать") {
        if (!c->haveWorld) return fail(out, "чашка доступна в тик() и настройки_изменены()");
        const WorldParam* p = findParam(str(0));
        if (!p) return fail(out, "нет параметра «" + str(0) + "» (свет, питание, радиация, вязкость, гравитация, солёность, нитраты, день, направление_света)");
        const double v = num(1);
        if (!std::isfinite(v)) return fail(out, "значение — не число");
        c->sets.push_back({p, (float)std::clamp(v, (double)p->lo, (double)p->hi)});
        c->world[p->name] = std::clamp(v, (double)p->lo, (double)p->hi);
        return 0;
    }
    if (name == "узнать") {
        auto it = c->world.find(str(0) == "соленость" ? std::string("солёность") : str(0));
        if (it == c->world.end()) return fail(out, c->haveWorld ? "нет величины «" + str(0) + "»" : "чашка доступна в тик()");
        out = dypt::Value::floating(it->second);
        return 0;
    }
    return 1;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
// First word of `s` (moved out of it).
std::string word(std::string& s) {
    s = trim(s);
    size_t e = s.find_first_of(" \t");
    std::string w = s.substr(0, e);
    s = e == std::string::npos ? std::string() : trim(s.substr(e));
    return w;
}

fs::path toPath(const std::string& utf8) {
    try {
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
    } catch (const std::exception&) {
        return fs::path(utf8);
    }
}
std::string fromPath(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

}  // namespace

// ---------------------------------------------------------------- mod.txt

bool parseModInfo(const std::string& text, ModInfo& m, std::string& error) {
    m.settings.clear();
    m.description.clear();
    m.tickEvery = 0;
    size_t s = 0;
    int lineNo = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(s, e - s));
        s = e + 1;
        ++lineNo;
        if (line.empty() || line[0] == '#') continue;
        std::string key = word(line);
        if (key == "id") {
            m.id.clear();
            for (char ch : line)  // letters of any alphabet, digits, _ and -: it names the mod in settings/mods.ini
                if ((unsigned char)ch >= 0x80 || std::isalnum((unsigned char)ch) || ch == '_' || ch == '-') m.id.push_back(ch);
        } else if (key == "name") {
            m.name = line;
        } else if (key == "version") {
            m.version = line;
        } else if (key == "author") {
            m.author = line;
        } else if (key == "description") {
            m.description += (m.description.empty() ? "" : "\n") + line;
        } else if (key == "tick") {
            m.tickEvery = std::clamp(std::atoi(line.c_str()), 0, 100000);
        } else if (key == "setting") {
            ModSetting st;
            st.key = word(line);
            std::string kind = word(line);
            if (kind == "число" || kind == "number") {
                st.kind = ModSetting::Number;
                st.def = std::atof(word(line).c_str());
                st.min = std::atof(word(line).c_str());
                st.max = std::atof(word(line).c_str());
                if (st.max <= st.min) st.max = st.min + 1;
                st.def = std::clamp(st.def, st.min, st.max);
            } else if (kind == "флаг" || kind == "flag") {
                st.kind = ModSetting::Flag;
                std::string v = word(line);
                st.def = (v == "1" || v == "да" || v == "yes" || v == "true") ? 1 : 0;
            } else if (kind == "выбор" || kind == "choice") {
                st.kind = ModSetting::Choice;
                std::string def = word(line), opts = word(line);
                size_t a = 0;
                while (a <= opts.size()) {
                    size_t b = opts.find(',', a);
                    if (b == std::string::npos) b = opts.size();
                    if (b > a) st.options.push_back(opts.substr(a, b - a));
                    a = b + 1;
                }
                if (st.options.empty()) st.options.push_back(def);
                auto it = std::find(st.options.begin(), st.options.end(), def);
                st.def = it == st.options.end() ? 0 : (double)(it - st.options.begin());
            } else {
                error = "mod.txt, строка " + std::to_string(lineNo) + ": тип настройки «" + kind + "» — нужен число, флаг или выбор";
                return false;
            }
            st.label = line.empty() ? st.key : line;
            st.value = st.def;
            if (!st.key.empty()) m.settings.push_back(std::move(st));
        }
    }
    if (m.id.empty() || m.name.empty()) {
        error = "в mod.txt нужны строки «id …» и «name …»";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- manager

struct ModManager::CallResult {
    bool ok = false, hung = false;
    std::string error;
    double ms = 0;
};

ModManager::~ModManager() {
    for (size_t i = 0; i < mods.size(); ++i) stop((int)i, false);
    exec_.stop();
    if (exec_.everAbandoned()) {  // a thread still runs inside the library
        lib_.release();
        (void)host_.release();
    }
}

bool ModManager::initialize(const std::string& libraryPath, std::string& error) {
    if (ready_) return true;
    if (!lib_.load(libraryPath)) {
        error = "не удалось загрузить dypt_embed (" + libraryPath + ")";
        return false;
    }
    if (lib_.abiVersion() != 2) {
        error = "несовместимая версия dypt ABI";
        lib_.unload();
        return false;
    }
    host_ = std::make_unique<dypt::HostContext>(&lib_);
    host_->setHostNames(kHostNames);
    host_->setHandler(hostCall);
    host_->setPrinter([](const std::string& text) {
        if (CallCtx* c = t_ctx; c && c->log.size() < kMaxLogLines) c->log.push_back(text);
    });
    host_->init();
    ready_ = true;
    return true;
}

void ModManager::loadState(const std::string& iniPath) {
    std::string text;
    if (!readWholeFile(iniPath, text, 1 << 20)) return;
    saved_.clear();
    Saved* cur = nullptr;
    size_t s = 0;
    while (s <= text.size()) {
        size_t e = text.find('\n', s);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(s, e - s));
        s = e + 1;
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            cur = &saved_[line.substr(1, line.size() - 2)];
            continue;
        }
        size_t eq = line.find('=');
        if (!cur || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "enabled") cur->enabled = v == "1";
        else cur->values[k] = std::atof(v.c_str());
    }
}

bool ModManager::saveState(const std::string& iniPath) const {
    std::string out = "# Модификации CellFap: какие включены и их настройки (меняются в окне «Модификации»).\n";
    for (const auto& [id, sv] : saved_) {
        out += "\n[" + id + "]\nenabled=" + (sv.enabled ? "1" : "0") + "\n";
        for (const auto& [k, v] : sv.values) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.9g", v);
            out += k + "=" + b + "\n";
        }
    }
    return writeWholeFile(iniPath, out);
}

void ModManager::remember(const ModInfo& m) {
    if (m.id.empty()) return;
    stateDirty_ = true;
    Saved& sv = saved_[m.id];
    sv.enabled = m.enabled;
    for (const ModSetting& st : m.settings) {
        if (st.value != st.def) sv.values[st.key] = st.value;
        else sv.values.erase(st.key);
    }
}

bool ModManager::loadFiles(ModInfo& m) {
    ModFiles files;
    std::string err;
    if (!(m.packed ? readModPack(m.path, files, err) : readModFolder(m.path, files, err))) {
        m.error = err;
        return false;
    }
    auto info = files.find("mod.txt");
    if (info == files.end()) {
        m.error = "нет mod.txt";
        return false;
    }
    // Keep the values the player set across a reload.
    std::map<std::string, double> values;
    for (const ModSetting& st : m.settings) values[st.key] = st.value;
    if (!parseModInfo(info->second, m, err)) {
        m.error = err;
        return false;
    }
    for (ModSetting& st : m.settings)
        if (auto it = values.find(st.key); it != values.end()) st.value = it->second;
    m.files = std::make_shared<const ModFiles>(std::move(files));
    return true;
}

void ModManager::scan(const std::string& dir) {
    for (size_t i = 0; i < mods.size(); ++i) stop((int)i, true);
    mods.clear();
    genomes.clear();
    programs.clear();
    ++generation;
    turn_ = 0;
    dir_ = dir;
    std::error_code ec;
    std::vector<fs::path> entries;
    for (auto& e : fs::directory_iterator(toPath(dir), ec)) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    for (const fs::path& p : entries) {
        ModInfo m;
        m.path = fromPath(p);
        if (fs::is_directory(p, ec)) {
            if (!fs::exists(p / "mod.txt", ec)) continue;
        } else if (p.extension() == ".cfmod") {
            m.packed = true;
        } else {
            continue;
        }
        if (loadFiles(m)) {
            for (const ModInfo& o : mods)
                if (o.id == m.id) m.error = "мод с id «" + m.id + "» уже есть (" + fromPath(toPath(o.path).filename()) + ")";
        } else if (m.name.empty()) {
            m.name = fromPath(p.filename());
        }
        if (auto it = saved_.find(m.id); it != saved_.end() && m.error.empty()) {
            m.enabled = it->second.enabled;
            for (ModSetting& st : m.settings)
                if (auto v = it->second.values.find(st.key); v != it->second.values.end()) {
                    st.value = v->second;
                    if (st.kind == ModSetting::Number) st.value = std::clamp(st.value, st.min, st.max);
                    if (st.kind == ModSetting::Choice) st.value = std::clamp(std::floor(st.value), 0.0, (double)st.options.size() - 1);
                }
        }
        mods.push_back(std::move(m));
    }
    for (size_t i = 0; i < mods.size(); ++i)
        if (mods[i].enabled) start((int)i);
}

void ModManager::dropContributions(const std::string& id) {
    auto g = std::remove_if(genomes.begin(), genomes.end(), [&](const ModGenome& x) { return x.mod == id; });
    auto p = std::remove_if(programs.begin(), programs.end(), [&](const ModProgram& x) { return x.mod == id; });
    if (g != genomes.end() || p != programs.end()) ++generation;
    genomes.erase(g, genomes.end());
    programs.erase(p, programs.end());
}

bool ModManager::start(int i) {
    ModInfo& m = mods[i];
    if (m.running) return true;
    m.runtimeErrors = 0;
    m.ticks = 0;
    m.worstMs = m.lastMs = 0;
    if (!m.files) {
        if (m.error.empty()) m.error = "файлы мода не прочитаны";
        return false;
    }
    m.error.clear();
    if (!ready_) {
        m.error = "dypt_embed не подключён: моды не могут работать";
        return false;
    }
    auto src = m.files->find("mod.dpt");
    if (src == m.files->end()) {
        m.error = "нет mod.dpt";
        return false;
    }
    if (std::string bad = signalCellSourceProblem(src->second); !bad.empty()) {
        m.error = "mod.dpt: " + bad;
        return false;
    }
    struct Job {
        std::string source, err;
        dypt::HostContext* host;
        dypt::ScriptedCell* script = nullptr;
        bool load = false, tick = false, hook = false, unload = false;
    };
    auto job = std::make_shared<Job>();
    job->source = src->second;
    job->host = host_.get();
    const bool finished = exec_.run([job] {
        auto s = std::make_unique<dypt::ScriptedCell>(job->host);
        if (!s->load(job->source, job->err)) return;
        job->load = s->has("загрузка");
        job->tick = s->has("тик");
        job->hook = s->has("настройки_изменены");
        job->unload = s->has("выгрузка");
        job->script = s.release();
    }, hangMs * 4);
    if (!finished) {
        // Compiling hung: whatever the old thread owned is gone with it.
        for (ModInfo& o : mods) {
            o.script = nullptr;
            if (o.running) {
                o.running = false;
                o.error = "остановлен: другой мод завис при компиляции";
            }
        }
        m.error = "компиляция не уложилась во время";
        m.enabled = false;
        remember(m);
        return false;
    }
    if (!job->script) {
        m.error = job->err.empty() ? "mod.dpt не компилируется" : job->err;
        return false;
    }
    m.script = job->script;
    m.hasTick = job->tick && m.tickEvery > 0;
    m.hasSettingsHook = job->hook;
    m.hasUnload = job->unload;
    m.running = true;
    if (job->load) {
        CallResult r;
        if (!callMod(i, "загрузка", {}, nullptr, r)) {
            if (!r.hung) {
                m.error = "загрузка(): " + r.error;
                stop(i, false);
            }
            return false;
        }
    }
    if (!job->load && !job->tick) m.error = "в mod.dpt нет ни загрузка(), ни тик(): мод ничего не делает";
    return true;
}

void ModManager::stop(int i, bool callUnload) {
    ModInfo& m = mods[i];
    if (m.running && callUnload && m.hasUnload) {
        CallResult r;
        callMod(i, "выгрузка", {}, nullptr, r);
    }
    if (m.script && exec_.running()) {
        dypt::ScriptedCell* s = m.script;
        exec_.run([s] { delete s; }, hangMs);
    }
    m.script = nullptr;
    m.running = false;
    m.tickDue = false;
    dropContributions(m.id);
}

bool ModManager::callMod(int i, const std::string& fn, const std::vector<dypt::Value>& args, World* w, CallResult& out) {
    ModInfo& m = mods[i];
    if (!m.script) return false;
    auto ctx = std::make_shared<CallCtx>();
    ctx->modId = m.id;
    ctx->loading = fn == "загрузка";
    ctx->settings = m.settings;
    ctx->files = m.files;
    ctx->rng ^= (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() * 0x2545F4914F6CDD1Dull + (uint64_t)i;
    if (w) {
        ctx->haveWorld = true;
        ctx->dishRadius = w->settings.dishRadius;
        for (const WorldParam& p : kParams) ctx->world[p.name] = w->settings.*p.field;
        ctx->world["часы"] = w->timeHours();
        ctx->world["клеток"] = w->cellCount();
        ctx->world["пищи"] = (double)w->food().size();
        ctx->world["тик"] = (double)w->ticks();
        ctx->world["радиус"] = w->settings.dishRadius;
    }
    struct Job {
        std::shared_ptr<CallCtx> ctx;
        dypt::ScriptedCell* script;
        std::string fn, err;
        std::vector<dypt::Value> args;
        bool ok = false;
    };
    auto job = std::make_shared<Job>();
    job->ctx = ctx;
    job->script = m.script;
    job->fn = fn;
    job->args = args;
    const auto t0 = std::chrono::steady_clock::now();
    const bool finished = exec_.run([job] {
        t_ctx = job->ctx.get();
        dypt::Value res;
        job->ok = job->script->call(job->fn, job->args, res, job->err);
        t_ctx = nullptr;
    }, hangMs);
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m.lastMs = out.ms;
    m.worstMs = std::max(m.worstMs, out.ms);
    if (!finished) {
        // The executor is gone with every script it owned: this mod is switched off, the others start again on a
        // fresh one (their загрузка runs anew).
        out.hung = true;
        out.error = fn + "() не вернулась за " + std::to_string((int)hangMs) + " мс";
        std::vector<int> others;
        for (size_t k = 0; k < mods.size(); ++k) {
            mods[k].script = nullptr;
            if (mods[k].running) {
                mods[k].running = false;
                dropContributions(mods[k].id);
                if ((int)k != i) others.push_back((int)k);
            }
        }
        m.error = out.error + ": мод выключен (зациклился?)";
        m.enabled = false;
        remember(m);
        messages_.push_back(m.name + ": " + m.error);
        for (int k : others) start(k);
        return false;
    }
    // What it did.
    for (std::string& line : ctx->log) m.log.push_back(std::move(line));
    if (m.log.size() > kMaxLogLines) m.log.erase(m.log.begin(), m.log.end() - kMaxLogLines);
    for (const std::string& msg : ctx->messages)
        if (messages_.size() < 50) messages_.push_back(m.name + ": " + msg);
    if (w) {
        const float R = w->settings.dishRadius - 5.0f;
        for (const CallCtx::Food& f : ctx->food)
            if (f.x * f.x + f.y * f.y < R * R) w->addFood({f.x, f.y}, f.m);
        for (const auto& [p, v] : ctx->sets) w->settings.*(p->field) = v;
    }
    if (!ctx->genomes.empty() || !ctx->programs.empty()) {
        for (ModGenome& g : ctx->genomes) genomes.push_back(std::move(g));
        for (ModProgram& p : ctx->programs) programs.push_back(std::move(p));
        ++generation;
    }
    out.ok = job->ok;
    if (!job->ok) {
        out.error = job->err;
        m.error = fn + "(): " + job->err;
        if (++m.runtimeErrors >= maxErrors && fn == "тик") {
            m.error += " — мод выключен после " + std::to_string(maxErrors) + " ошибок";
            messages_.push_back(m.name + ": " + m.error);
            stop(i, false);
        }
    }
    return job->ok;
}

void ModManager::tick(World& w) {
    if (!ready_ || mods.empty()) return;
    const uint64_t t = w.ticks();
    for (ModInfo& m : mods)
        if (m.running && m.hasTick && t % (uint64_t)m.tickEvery == 0) m.tickDue = true;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = mods.size();
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (turn_ + k) % n;
        if (i >= mods.size()) break;  // (a hang restarted everything)
        ModInfo& m = mods[i];
        if (!m.running || !m.tickDue) continue;
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() > tickBudgetMs) {
            turn_ = i;  // over budget: the rest go first next tick
            return;
        }
        m.tickDue = false;
        ++m.ticks;
        std::vector<dypt::Value> st = {dypt::Value::floating(w.timeHours()), dypt::Value::integer(w.cellCount()),
                                       dypt::Value::integer((int64_t)w.food().size()), dypt::Value::integer((int64_t)t)};
        CallResult r;
        callMod((int)i, "тик", {dypt::Value::list(st)}, &w, r);
    }
    turn_ = 0;
}

void ModManager::setEnabled(int i, bool on) {
    if (i < 0 || i >= (int)mods.size()) return;
    ModInfo& m = mods[i];
    if (on) {
        m.enabled = true;
        if (!m.files) loadFiles(m);
        start(i);
    } else {
        stop(i, true);
        m.enabled = false;
        m.error.clear();
    }
    remember(m);
}

void ModManager::restart(int i) {
    if (i < 0 || i >= (int)mods.size()) return;
    ModInfo& m = mods[i];
    stop(i, true);
    m.error.clear();
    m.log.clear();
    if (loadFiles(m) && m.enabled) start(i);
}

void ModManager::settingChanged(int i, World* w) {
    if (i < 0 || i >= (int)mods.size()) return;
    ModInfo& m = mods[i];
    remember(m);
    if (m.running && m.hasSettingsHook) {
        CallResult r;
        callMod(i, "настройки_изменены", {}, w, r);
    }
}

void ModManager::resetSettings(int i, World* w) {
    if (i < 0 || i >= (int)mods.size()) return;
    for (ModSetting& st : mods[i].settings) st.value = st.def;
    settingChanged(i, w);
}

std::vector<std::string> ModManager::takeMessages() {
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}
