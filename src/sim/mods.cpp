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
#include <mutex>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------- what one call of a mod may see and change

constexpr size_t kMaxLogLines = 200;
constexpr int kMaxFoodPerCall = 500;
constexpr int kMaxMessagesPerCall = 10;
constexpr int kMaxAdded = 64;      // genomes and programs per mod
constexpr int kMaxOpsPerCall = 5000;  // cell changes per call

const std::vector<std::string> kHostNames = {
    "журнал", "сообщение", "настройка", "добавить_геном", "добавить_программу", "добавить_пищу", "задать", "узнать",
    "файл_мода", "случайное",
    // the whole dish (docs/mods.md, «Чашка целиком»)
    "ячеек", "клетка", "клетки_рядом", "соседи", "пища", "изменить_клетку", "убить_клетку", "добавить_клетку",
    "убрать_пищу", "сохранить_чашку", "открыть_чашку"};

// Fields of a cell a script may change (изменить_клетку).
const char* const kCellFields[] = {"x", "y", "vx", "vy", "угол", "масса", "возраст", "азот", "повреждение", "режим",
                                   "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8"};

struct CallCtx {
    std::string modId;
    bool loading = false;  // загрузка(): the only time genomes and programs can be added
    std::vector<ModSetting> settings;
    std::shared_ptr<const ModFiles> files;
    std::map<std::string, double> world;  // узнать: the values that are not dish parameters
    bool haveWorld = false;
    float dishRadius = 0;
    uint64_t rng = 0x9E3779B97F4A7C15ull;
    // Reading the dish while the call runs. The caller waits (holding the world) until the call returns; a call that
    // hangs is abandoned, and from then on `alive` is false: the abandoned thread never reads a dish that moves on.
    std::mutex m;
    bool alive = true;
    World* w = nullptr;
    // what it did
    std::vector<std::string> log, messages;
    struct Food {
        float x, y, m;
    };
    std::vector<Food> food;
    std::vector<std::pair<const DishParam*, double>> sets;
    std::vector<ModGenome> genomes;
    std::vector<ModProgram> programs;
    struct Op {
        enum Kind { Set, Kill, Add, RemoveFood } kind;
        int index = -1;
        uint32_t id = 0;
        int field = -1;
        double v = 0;
        std::string genome;  // Add: a genome the mod added, a file of the mod or genome text
        float x = 0, y = 0, angle = 0;
        int mode = -1;
    };
    std::vector<Op> ops;
    std::vector<ModManager::DishRequest> dish;
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

dypt::Value dnum(double v) { return dypt::Value::floating(v); }
dypt::Value whole(int64_t v) { return dypt::Value::integer(v); }

// клетка(i): a list in a fixed order (docs/mods.md):
//   0 id, 1 x, 2 y, 3 vx, 4 vy, 5 угол, 6 масса, 7 радиус, 8 возраст, 9 азот, 10 повреждение, 11 тип, 12 режим,
//   13 связей, 14 геном, 15 [сигналы S1..S8]
dypt::Value cellValue(const World& w, int i) {
    const Cell& c = w.cells()[i];
    std::vector<dypt::Value> sig;
    for (float s : c.signals) sig.push_back(dnum(s));
    return dypt::Value::list({whole(c.id), dnum(c.pos.x), dnum(c.pos.y), dnum(c.vel.x), dnum(c.vel.y), dnum(c.angle), dnum(c.mass),
                              dnum(c.radius), dnum(c.age), dnum(c.nitrogen), dnum(c.injury), whole((int)c.type), whole(c.mode),
                              whole(c.linkCount), whole(c.genome), dypt::Value::list(sig)});
}

int hostCall(const std::string& name, const std::vector<dypt::Value>& args, dypt::Value& out) {
    CallCtx* c = t_ctx;
    if (!c) return 1;
    out = dypt::Value::null();
    auto str = [&](size_t k) { return k < args.size() && args[k].kind == DY_STRING ? args[k].s : std::string(); };
    auto num = [&](size_t k) { return k < args.size() ? args[k].asFloat() : 0.0; };
    auto idx = [&](size_t k) { return k < args.size() ? (int)args[k].asInt() : -1; };
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
    if (name == "сохранить_чашку" || name == "открыть_чашку") {
        const std::string file = str(0);
        if (file.empty()) return fail(out, "нужно имя файла чашки (без .substrate)");
        if (c->dish.size() < 4) c->dish.push_back({c->modId, name == "сохранить_чашку", safeFileName(file, "mod")});
        return 0;
    }
    // Everything below needs the dish.
    if (!c->haveWorld) return fail(out, "чашка доступна в тик(), настройки_изменены() и новая_чашка()");
    if (name == "добавить_пищу") {
        if ((int)c->food.size() >= kMaxFoodPerCall) return fail(out, "не больше 500 порций за вызов");
        const double mass = args.size() > 2 ? num(2) : 0.3;
        // (std::clamp lets NaN through: every number is checked before it is clamped)
        if (!std::isfinite(mass)) return fail(out, "масса — не число");
        const float x = (float)num(0), y = (float)num(1), m = (float)std::clamp(mass, 0.01, 2.0);
        if (!std::isfinite(x) || !std::isfinite(y)) return fail(out, "координаты — не числа");
        c->food.push_back({x, y, m});
        return 0;
    }
    if (name == "задать") {
        const DishParam* p = findDishParam(str(0));
        if (!p) return fail(out, "нет параметра «" + str(0) + "» (свет, питание, радиация, вязкость, гравитация, солёность, нитраты, день, направление_света…)");
        if (!p->writable) return fail(out, "«" + str(0) + "» можно только узнать");
        const double v = num(1);
        if (!std::isfinite(v)) return fail(out, "значение — не число");
        c->sets.push_back({p, std::clamp(v, p->lo, p->hi)});
        return 0;
    }
    std::lock_guard<std::mutex> lk(c->m);
    if (!c->alive || !c->w) return fail(out, "вызов уже закончился");
    const World& w = *c->w;
    if (name == "узнать") {
        const std::string what = str(0);
        if (auto it = c->world.find(what); it != c->world.end()) {
            out = dypt::Value::floating(it->second);
            return 0;
        }
        const DishParam* p = findDishParam(what);
        if (!p) return fail(out, "нет величины «" + what + "»");
        double v = getDishParam(w.settings, *p);
        for (const auto& [sp, sv] : c->sets)  // what this call has set already
            if (sp == p) v = sv;
        out = dypt::Value::floating(v);
        return 0;
    }
    if (name == "ячеек") {
        out = dypt::Value::integer((int64_t)w.cells().size());
        return 0;
    }
    if (name == "клетка") {
        const int i = idx(0);
        if (i >= 0 && i < (int)w.cells().size() && w.cells()[i].alive) out = cellValue(w, i);
        return 0;
    }
    if (name == "пища") {
        const int i = idx(0);
        if (i >= 0 && i < (int)w.food().size()) {
            const Food& f = w.food()[i];
            out = dypt::Value::list({dnum(f.pos.x), dnum(f.pos.y), dnum(f.mass)});  // [x, y, масса]
        }
        return 0;
    }
    if (name == "клетки_рядом") {
        const Vec2 p{(float)num(0), (float)num(1)};
        const float r = (float)num(2);
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(r) || r < 0) return fail(out, "клетки_рядом(x, y, радиус)");
        std::vector<dypt::Value> found;
        w.forEachCellNear(p, r, [&](int j) {
            if (found.size() < 4096 && length(w.cells()[j].pos - p) <= r) found.push_back(dypt::Value::integer(j));
        });
        out = dypt::Value::list(found);
        return 0;
    }
    if (name == "соседи") {
        const int i = idx(0);
        std::vector<dypt::Value> found;
        if (i >= 0 && i < (int)w.cells().size() && w.cells()[i].alive) {
            const Cell& cell = w.cells()[i];
            for (int k = 0; k < cell.linkCount; ++k) {
                const Link& l = w.links()[cell.links[k]];
                const int o = l.a == i ? l.b : l.a;
                if (o >= 0) found.push_back(dypt::Value::integer(o));
            }
        }
        out = dypt::Value::list(found);
        return 0;
    }
    if ((int)c->ops.size() >= kMaxOpsPerCall) return fail(out, "не больше 5000 изменений клеток за вызов");
    auto liveCell = [&](int i) { return i >= 0 && i < (int)w.cells().size() && w.cells()[i].alive; };
    if (name == "изменить_клетку") {
        const int i = idx(0);
        if (!liveCell(i)) return fail(out, "нет живой клетки " + std::to_string(i));
        const std::string f = str(1);
        int field = -1;
        for (int k = 0; k < (int)(sizeof(kCellFields) / sizeof(kCellFields[0])); ++k)
            if (f == kCellFields[k]) field = k;
        if (field < 0) return fail(out, "поле «" + f + "»: x, y, vx, vy, угол, масса, возраст, азот, повреждение, режим, s1…s8");
        const double v = num(2);
        if (!std::isfinite(v)) return fail(out, "значение — не число");
        CallCtx::Op op{CallCtx::Op::Set};
        op.index = i;
        op.id = w.cells()[i].id;
        op.field = field;
        op.v = v;
        c->ops.push_back(std::move(op));
        return 0;
    }
    if (name == "убить_клетку") {
        const int i = idx(0);
        if (!liveCell(i)) return fail(out, "нет живой клетки " + std::to_string(i));
        CallCtx::Op op{CallCtx::Op::Kill};
        op.index = i;
        op.id = w.cells()[i].id;
        op.v = args.size() > 1 && !args[1].asBool() ? 0 : 1;  // leave food (by default)
        c->ops.push_back(std::move(op));
        return 0;
    }
    if (name == "добавить_клетку") {
        CallCtx::Op op{CallCtx::Op::Add};
        op.genome = str(0);
        op.x = (float)num(1);
        op.y = (float)num(2);
        op.angle = (float)num(3);
        op.mode = args.size() > 4 ? idx(4) : -1;
        if (op.genome.empty()) return fail(out, "добавить_клетку(геном, x, y, угол?, режим?)");
        if (!std::isfinite(op.x) || !std::isfinite(op.y) || !std::isfinite(op.angle)) return fail(out, "координаты — не числа");
        if (op.mode < -1 || op.mode >= kMaxModes) return fail(out, "режим 0…127");
        c->ops.push_back(std::move(op));
        return 0;
    }
    if (name == "убрать_пищу") {
        const int i = idx(0);
        if (i < 0 || i >= (int)w.food().size()) return fail(out, "нет пищи " + std::to_string(i));
        CallCtx::Op op{CallCtx::Op::RemoveFood};
        op.index = i;
        c->ops.push_back(std::move(op));
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
    m.nativeLib.clear();
    m.tickEvery = 0;
    // A UTF-8 byte-order mark (Notepad, some messengers) is not part of the first key.
    size_t s = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
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
        } else if (key == "native") {
            m.nativeLib = line;  // the library with the entry points, when native/<platform>/ holds several
        } else if (key == "setting") {
            ModSetting st;
            st.key = word(line);
            // The key is a line "key=value" in settings/mods.ini, beside the mod's own "enabled" and "native_sha256".
            if (st.key.find_first_of("=[]#;") != std::string::npos || st.key == "enabled" || st.key == "native_sha256") {
                error = "mod.txt, строка " + std::to_string(lineNo) + ": ключ настройки «" + st.key +
                        "» не подходит (без = [ ] # ; и не enabled / native_sha256)";
                return false;
            }
            std::string kind = word(line);
            if (kind == "число" || kind == "number") {
                st.kind = ModSetting::Number;
                st.def = std::atof(word(line).c_str());
                st.min = std::atof(word(line).c_str());
                st.max = std::atof(word(line).c_str());
                if (!std::isfinite(st.def) || !std::isfinite(st.min) || !std::isfinite(st.max)) {
                    error = "mod.txt, строка " + std::to_string(lineNo) + ": числа настройки «" + st.key + "» — не числа";
                    return false;
                }
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
    shuttingDown_ = true;
    for (size_t i = 0; i < mods.size(); ++i) stop((int)i, false);  // (native libraries still get their unload())
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
        else if (k == "native_sha256") cur->nativeHash = v;
        else if (const double d = std::atof(v.c_str()); std::isfinite(d)) cur->values[k] = d;
    }
}

bool ModManager::saveState(const std::string& iniPath) const {
    std::string out = "# Модификации CellFap: какие включены и их настройки (меняются в окне «Модификации»).\n"
                      "# native_sha256 — отпечаток нативного кода, который вы разрешили запускать.\n";
    for (const auto& [id, sv] : saved_) {
        out += "\n[" + id + "]\nenabled=" + (sv.enabled ? "1" : "0") + "\n";
        if (!sv.nativeHash.empty()) out += "native_sha256=" + sv.nativeHash + "\n";
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
    m.hasScript = files.count("mod.dpt") != 0;
    m.native = nativeFilesOf(files);
    // The consent holds for exactly the files it was given for.
    auto sv = saved_.find(m.id);
    const std::string allowed = sv != saved_.end() ? sv->second.nativeHash : std::string();
    m.nativeAllowed = !m.native.hash.empty() && allowed == m.native.hash;
    m.nativeChanged = !m.native.hash.empty() && !allowed.empty() && allowed != m.native.hash;
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
            if (!fs::exists(p / "mod.txt", ec)) continue;  // (also mods/.cache and mods/.data)
        } else if (p.extension() == ".cfmod") {
            m.packed = true;
        } else {
            continue;
        }
        if (loadFiles(m)) {
            for (const ModInfo& o : mods)
                if (o.id == m.id) {
                    m.duplicate = "мод с id «" + m.id + "» уже есть (" + fromPath(toPath(o.path).filename()) +
                                  "): уберите один из них из папки mods";
                    m.error = m.duplicate;
                }
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
    if (!m.duplicate.empty()) {  // (its settings and consent would be the other one's)
        m.error = m.duplicate;
        return false;
    }
    m.error.clear();
    if (!m.hasScript && !m.native.any) {
        m.error = "нет mod.dpt (и нет нативного кода в native/)";
        return false;
    }
    // The dish the mod starts with: it hears of a different one (world_reset / новая_чашка) when that one goes.
    m.dishWorld = world_;
    m.dishReset = world_ ? world_->resetCount() : 0;
    if (m.native.any && !startNative(i)) return false;
    if (!m.hasScript) {
        m.running = true;
        return true;
    }
    if (!ready_) {
        m.error = "dypt_embed не подключён: mod.dpt не может работать";
        stopNative(i, true);
        return false;
    }
    auto src = m.files->find("mod.dpt");
    if (std::string bad = signalCellSourceProblem(src->second); !bad.empty()) {
        m.error = "mod.dpt: " + bad;
        stopNative(i, true);
        return false;
    }
    struct Job {
        std::string source, err;
        dypt::HostContext* host;
        dypt::ScriptedCell* script = nullptr;
        bool load = false, tick = false, hook = false, unload = false, dish = false;
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
        job->dish = s->has("новая_чашка");
        job->script = s.release();
    }, hangMs * 4);
    if (!finished) {
        executorLost(i, "компиляция mod.dpt не уложилась во время: мод выключен");
        return false;
    }
    if (!job->script) {
        m.error = job->err.empty() ? "mod.dpt не компилируется" : job->err;
        stopNative(i, true);
        return false;
    }
    m.script = job->script;
    m.hasTick = job->tick && m.tickEvery > 0;
    m.hasSettingsHook = job->hook;
    m.hasUnload = job->unload;
    m.hasDishHook = job->dish;
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
    if (!job->load && !job->tick && !m.nat) m.error = "в mod.dpt нет ни загрузка(), ни тик(): мод ничего не делает";
    return true;
}

void ModManager::stop(int i, bool callUnload) {
    ModInfo& m = mods[i];
    if (m.running && callUnload && m.hasUnload && m.script) {
        CallResult r;
        callMod(i, "выгрузка", {}, nullptr, r);
    }
    bool lost = false;
    if (m.script && exec_.running()) {
        dypt::ScriptedCell* s = m.script;
        m.script = nullptr;
        lost = !exec_.run([s] { delete s; }, hangMs);
    }
    m.script = nullptr;
    stopNative(i, true);  // (its unload() runs even after an error: the library frees what it holds)
    m.running = false;
    m.tickDue = false;
    dropContributions(m.id);
    // Closing the script hung: the executor and every other script on it are gone.
    if (lost) executorLost(-1, "закрытие скрипта «" + m.name + "» зависло: модификации перезапущены");
}

void ModManager::executorLost(int culprit, const std::string& why) {
    // Every script lived on the abandoned thread. Mods with a script lose it and what they added and start again on
    // a fresh executor (their загрузка runs anew); `culprit` stays off. Mods with only native code never used it.
    std::vector<int> others;
    for (size_t k = 0; k < mods.size(); ++k) {
        ModInfo& o = mods[k];
        o.script = nullptr;
        if (!o.hasScript || (!o.running && (int)k != culprit)) continue;
        const bool was = o.running;
        o.running = false;
        o.tickDue = false;
        stopNative((int)k, true);
        dropContributions(o.id);
        if (was && (int)k != culprit) others.push_back((int)k);
    }
    if (culprit >= 0) {
        ModInfo& m = mods[culprit];
        m.error = why;
        m.enabled = false;
        remember(m);
        if (messages_.size() < 50) messages_.push_back(m.name + ": " + why);
    } else if (messages_.size() < 50) {
        messages_.push_back("Модификации: " + why);
    }
    if (shuttingDown_) return;
    for (int k : others) {
        if (start(k)) continue;
        if (messages_.size() < 50) messages_.push_back(mods[k].name + ": не перезапустилась — " + mods[k].error);
    }
}

// ---------------------------------------------------------------- native code

bool ModManager::startNative(int i) {
    ModInfo& m = mods[i];
    if (m.nat) return true;
    if (m.native.here.empty()) {
        std::string have;
        for (const std::string& p : m.native.platforms) have += (have.empty() ? "" : ", ") + p;
        m.error = std::string("нет нативного кода для этой платформы (native/") + nativePlatform() + "/)" +
                  (have.empty() ? "" : "; в моде есть: " + have);
        return false;
    }
    if (!m.nativeAllowed) {
        m.error = m.nativeChanged ? "Нативный код изменился с тех пор, как вы его разрешили: проверьте мод и разрешите снова."
                                  : "Нативный код не разрешён: разрешите его в окне «Модификации».";
        return false;
    }
    std::string entry, dir, err;
    if (!nativeEntry(m.native, m.nativeLib, entry, err) || !extractNative(*m.files, m.native, cacheDir(), m.id, dir, err)) {
        m.error = err;
        return false;
    }
    auto nm = std::make_shared<NativeMod>();
    nm->mgr = this;
    nm->index = i;
    nm->id = m.id;
    nm->libDir = dir;
    nm->dataDir = dataDirOf(m.id);
    std::error_code ec;
    fs::create_directories(toPath(nm->dataDir), ec);
    const std::string prefix = std::string("native/") + nativePlatform() + "/";
    auto open = [&](const std::string& path) -> bool {
        auto lib = std::make_unique<SharedLibrary>();
        if (!lib->open(dir + "/" + path.substr(prefix.size()), err)) return false;
        nm->libs.push_back(std::move(lib));
        return true;
    };
#ifndef _WIN32
    // dlopen does not look next to a library for the libraries it needs: those that are in the mod are opened first
    // (in rounds, so their order does not matter), then they are found by name.
    std::vector<std::string> deps;
    for (const std::string& l : m.native.libraries)
        if (l != entry) deps.push_back(l);
    for (size_t round = 0; round < deps.size() && !deps.empty(); ++round)
        deps.erase(std::remove_if(deps.begin(), deps.end(), [&](const std::string& l) { return open(l); }), deps.end());
#endif
    if (!open(entry)) {
        m.error = "библиотека не загрузилась: " + err;
        return false;
    }
    SharedLibrary& lib = *nm->libs.back();
    auto version = (CellFapModApiVersionFn)lib.symbol("cellfap_mod_api_version");
    auto load = (CellFapModLoadFn)lib.symbol("cellfap_mod_load");
    if (!version || !load) {
        m.error = "библиотека не экспортирует cellfap_mod_api_version и cellfap_mod_load (sdk/cellfap_mod.h)";
        return false;
    }
    const uint32_t v = version();
    if (v == 0 || v > CELLFAP_MOD_API_VERSION) {
        m.error = "мод собран для API " + std::to_string(v) + ", игра понимает до " + std::to_string(CELLFAP_MOD_API_VERSION) +
                  ": нужна более новая игра";
        return false;
    }
    CellFapMod& md = nm->mod;
    md.struct_size = (uint32_t)sizeof(CellFapMod);
    md.api_version = CELLFAP_MOD_API_VERSION;
    md.id = nm->id.c_str();
    md.lib_dir = nm->libDir.c_str();
    md.data_dir = nm->dataDir.c_str();
    md.platform = nativePlatform();
    md.host = nm.get();
    m.nat = nm;
    int rc;
    {
        ModCallScope<CellFapWorld> scope(wctx_);
        wctx_.world = nullptr;  // (no dish while it loads)
        wctx_.mod = nm.get();
        rc = load(modApi(), &md);
    }
    if (rc != CELLFAP_OK) {
        m.error = "cellfap_mod_load вернула " + std::to_string(rc) + (nm->error.empty() ? "" : ": " + nm->error);
        m.nat.reset();
        dropContributions(m.id);
        return false;
    }
    nm->loaded = true;
    // The dish it finds.
    if (world_ && md.world_reset &&
        !callNative(i, "world_reset", world_, [](CellFapMod* x, CellFapWorld* w) { return x->world_reset(x, w); }))
        return false;
    return true;
}

void ModManager::stopNative(int i, bool callUnload) {
    ModInfo& m = mods[i];
    if (!m.nat) return;
    std::shared_ptr<NativeMod> nm = std::move(m.nat);
    if (nm->loaded && callUnload && nm->mod.unload) {
        ModCallScope<CellFapWorld> scope(wctx_);
        wctx_.world = nullptr;
        wctx_.mod = nm.get();
        nm->error.clear();
        if (const int rc = nm->mod.unload(&nm->mod); rc != CELLFAP_OK)
            addLog(i, "unload() вернула " + std::to_string(rc) + (nm->error.empty() ? "" : ": " + nm->error));
    }
    nm->loaded = false;
    while (!nm->libs.empty()) nm->libs.pop_back();  // the entry library first, then what it needed
    m.nativeMs = 0;
}

template <class F>
bool ModManager::callNative(int i, const char* what, World* w, F&& f) {
    NativeMod* nm = mods[i].nat.get();
    if (!nm || !nm->loaded) return false;
    int rc;
    {
        ModCallScope<CellFapWorld> scope(wctx_);
        wctx_.world = w;
        wctx_.mod = nm;
        nm->error.clear();
        rc = f(&nm->mod, w ? &wctx_ : nullptr);
    }
    if (rc == CELLFAP_OK) return true;
    nativeFailed(i, std::string(what) + "() вернула " + std::to_string(rc) + (nm->error.empty() ? "" : ": " + nm->error));
    return false;
}

void ModManager::nativeFailed(int i, const std::string& why) {
    ModInfo& m = mods[i];
    stop(i, true);
    m.error = why + " — мод выключен";
    m.enabled = false;
    remember(m);
    if (messages_.size() < 50) messages_.push_back(m.name + ": " + m.error);
}

void ModManager::allowNative(int i, bool allow) {
    if (i < 0 || i >= (int)mods.size()) return;
    ModInfo& m = mods[i];
    if (allow) {
        if (m.native.hash.empty() || !m.duplicate.empty()) return;
        saved_[m.id].nativeHash = m.native.hash;
        m.nativeAllowed = true;
        m.nativeChanged = false;
        stateDirty_ = true;
        remember(m);
        if (m.enabled && !m.running) start(i);
    } else {
        stop(i, true);
        saved_[m.id].nativeHash.clear();
        m.nativeAllowed = false;
        stateDirty_ = true;
        if (m.enabled) m.error = "Нативный код не разрешён: разрешите его в окне «Модификации».";
    }
}

void ModManager::checkReset(World& w) {
    for (size_t i = 0; i < mods.size(); ++i) {
        ModInfo& m = mods[i];
        if (!m.running || (m.dishWorld == &w && m.dishReset == w.resetCount())) continue;
        m.dishWorld = &w;
        m.dishReset = w.resetCount();
        if (m.nat && m.nat->mod.world_reset &&
            !callNative((int)i, "world_reset", &w, [](CellFapMod* x, CellFapWorld* cw) { return x->world_reset(x, cw); }))
            continue;
        if (m.running && m.hasDishHook && m.script) {
            CallResult r;
            callMod((int)i, "новая_чашка", {}, &w, r);
        }
    }
}

bool ModManager::beforeStep(World& w) {
    if (mods.empty()) return true;
    checkReset(w);
    bool skip = false, events = false;
    for (size_t i = 0; i < mods.size(); ++i) {
        NativeMod* nm = mods[i].nat.get();
        if (!nm || !nm->loaded) continue;
        if (nm->mod.before_step) {
            int s = 0;
            if (callNative((int)i, "before_step", &w, [&s](CellFapMod* x, CellFapWorld* cw) { return x->before_step(x, cw, &s); }) && s)
                skip = true;
        }
        if (mods[i].nat && nm->mod.cell_event && nm->mod.events) events = true;
    }
    w.recordEvents = events;
    return !skip;
}

void ModManager::dispatchNative(World& w) {
    using Clock = std::chrono::steady_clock;
    std::vector<World::CellEvent> evs;
    evs.swap(w.cellEvents);
    bool events = false;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (!mods[i].nat || !mods[i].nat->loaded) continue;
        const auto t0 = Clock::now();
        CellFapMod& md = mods[i].nat->mod;
        // 1. births, divisions and deaths of the tick
        if (md.cell_event && md.events)
            for (const World::CellEvent& e : evs) {
                if (!(md.events & CELLFAP_EVENT_BIT(e.kind))) continue;
                const CellFapEvent ce{e.kind, e.cell, e.other, e.id, e.otherId, (int32_t)e.type, e.pos.x, e.pos.y};
                if (!callNative((int)i, "cell_event", &w, [&ce](CellFapMod* x, CellFapWorld* cw) { return x->cell_event(x, cw, &ce); }))
                    break;
            }
        // 2. its cell types, one cell at a time (the cells it adds now are not visited before the next tick)
        if (mods[i].nat && md.cell_update && md.cell_types) {
            const uint64_t types = md.cell_types;
            const size_t n = w.cells().size();
            for (size_t c = 0; c < n; ++c) {
                const Cell& cell = w.cells()[c];
                if (!cell.alive || !((types >> (unsigned)cell.type) & 1u)) continue;
                const int32_t ci = (int32_t)c;
                if (!callNative((int)i, "cell_update", &w, [ci](CellFapMod* x, CellFapWorld* cw) { return x->cell_update(x, cw, ci); }))
                    break;
            }
        }
        // 3. the whole dish
        if (mods[i].nat && md.after_step)
            callNative((int)i, "after_step", &w, [](CellFapMod* x, CellFapWorld* cw) { return x->after_step(x, cw); });
        if (mods[i].nat) {
            mods[i].nativeMs = mods[i].nativeMs * 0.9 + 0.1 * std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            if (md.cell_event && md.events) events = true;
        }
    }
    w.recordEvents = events;
}

void ModManager::frame(World& w) {
    if (!mods.empty()) checkReset(w);
}

bool ModManager::hasPanel(int i) const {
    return i >= 0 && i < (int)mods.size() && mods[i].nat && mods[i].nat->loaded && mods[i].nat->mod.panel;
}

void ModManager::drawPanel(int i, World* w, ModUi& ui) {
    if (!hasPanel(i)) return;
    ModCallScope<CellFapUi> scope(uictx_);
    uictx_.ui = &ui;
    callNative(i, "panel", w, [this](CellFapMod* x, CellFapWorld* cw) { return x->panel(x, cw, &uictx_); });
}

void ModManager::drawOverlay(World& w, ModDraw& d) {
    for (size_t i = 0; i < mods.size(); ++i) {
        if (!mods[i].nat || !mods[i].nat->loaded || !mods[i].nat->mod.draw) continue;
        ModCallScope<CellFapDraw> scope(drawctx_);
        drawctx_.draw = &d;
        callNative((int)i, "draw", &w, [this](CellFapMod* x, CellFapWorld* cw) { return x->draw(x, cw, &drawctx_); });
    }
}

void ModManager::addLog(int i, const std::string& line) {
    if (i < 0 || i >= (int)mods.size()) return;
    std::vector<std::string>& log = mods[i].log;
    log.push_back(line);
    if (log.size() > kMaxLogLines) log.erase(log.begin(), log.end() - kMaxLogLines);
}

void ModManager::addMessage(int i, const std::string& text) {
    if (i < 0 || i >= (int)mods.size() || messages_.size() >= 50) return;
    messages_.push_back(mods[i].name + ": " + text);
}

// ---------------------------------------------------------------- dypt calls

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
        ctx->w = w;
        ctx->dishRadius = w->settings.dishRadius;
        ctx->world["часы"] = w->timeHours();
        ctx->world["клеток"] = w->cellCount();
        ctx->world["пищи"] = (double)w->food().size();
        ctx->world["тик"] = (double)w->ticks();
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
    {
        // From here on the script reads no dish (an abandoned thread finds the call over).
        std::lock_guard<std::mutex> lk(ctx->m);
        ctx->alive = false;
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m.lastMs = out.ms;
    m.worstMs = std::max(m.worstMs, out.ms);
    if (!finished) {
        out.hung = true;
        out.error = fn + "() не вернулась за " + std::to_string((int)hangMs) + " мс";
        executorLost(i, out.error + ": мод выключен (зациклился?)");
        return false;
    }
    // What it did.
    for (std::string& line : ctx->log) m.log.push_back(std::move(line));
    if (m.log.size() > kMaxLogLines) m.log.erase(m.log.begin(), m.log.end() - kMaxLogLines);
    for (const std::string& msg : ctx->messages)
        if (messages_.size() < 50) messages_.push_back(m.name + ": " + msg);
    for (DishRequest& d : ctx->dish)
        if (dishRequests.size() < 8) dishRequests.push_back(std::move(d));
    if (w) {
        const float R = w->settings.dishRadius - 5.0f;
        for (const CallCtx::Food& f : ctx->food)
            if (f.x * f.x + f.y * f.y < R * R) w->addFood({f.x, f.y}, f.m);
        for (const auto& [p, v] : ctx->sets) setDishParam(w->settings, *p, v);
        // Cell changes in the order they were asked for; a cell that died meanwhile is skipped (its slot may hold
        // another cell by now: the id tells).
        std::vector<int> foodGone;
        for (const CallCtx::Op& op : ctx->ops) {
            const bool same = op.index >= 0 && op.index < (int)w->cells().size() && w->cells()[op.index].alive &&
                              w->cells()[op.index].id == op.id;
            if (op.kind == CallCtx::Op::Set && same) {
                Cell& c = w->cellsMut()[op.index];
                const float v = (float)op.v;
                switch (op.field) {
                    case 0: c.pos.x = v; break;
                    case 1: c.pos.y = v; break;
                    case 2: c.vel.x = std::clamp(v, -50.0f, 50.0f); break;
                    case 3: c.vel.y = std::clamp(v, -50.0f, 50.0f); break;
                    case 4: c.angle = v; break;
                    case 5:
                        c.mass = std::clamp(v, 0.0f, 40.0f);
                        c.radius = radiusForMass(c.mass, c.type);
                        break;
                    case 6: c.age = std::max(0.0f, v); break;
                    case 7: c.nitrogen = std::clamp(v, 0.0f, 1.0f); break;
                    case 8: c.injury = std::clamp(v, 0.0f, 1.0f); break;
                    case 9: w->setCellMode(op.index, std::clamp((int)op.v, 0, kMaxModes - 1)); break;
                    default: c.signals[op.field - 10] = std::clamp(v, -1e6f, 1e6f); break;
                }
            } else if (op.kind == CallCtx::Op::Kill && same) {
                w->removeCell(op.index, op.v != 0);
            } else if (op.kind == CallCtx::Op::Add) {
                // A genome this mod added, a file of the mod or genome text.
                const Genome* g = nullptr;
                Genome parsed;
                for (const ModGenome& mg : genomes)
                    if (mg.mod == m.id && mg.name == op.genome) g = &mg.genome;
                if (!g) {
                    auto it = m.files->find(modRelativePath(op.genome));
                    if (genomeFromText(parsed, it != m.files->end() ? it->second : op.genome)) g = &parsed;
                }
                if (!g) {
                    m.log.push_back("добавить_клетку: нет генома «" + op.genome.substr(0, 60) + "»");
                    continue;
                }
                if (op.x * op.x + op.y * op.y < R * R) w->addCell(*g, {op.x, op.y}, op.angle, op.mode);
            } else if (op.kind == CallCtx::Op::RemoveFood) {
                foodGone.push_back(op.index);
            }
        }
        if (!foodGone.empty()) {
            // Highest first, so the indices the script saw stay right.
            std::sort(foodGone.begin(), foodGone.end());
            foodGone.erase(std::unique(foodGone.begin(), foodGone.end()), foodGone.end());
            auto& food = w->foodMut();
            for (auto it = foodGone.rbegin(); it != foodGone.rend(); ++it)
                if (*it < (int)food.size()) food.erase(food.begin() + *it);
        }
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
    if (mods.empty()) {
        w.cellEvents.clear();
        return;
    }
    checkReset(w);
    dispatchNative(w);
    if (!ready_) return;
    const uint64_t t = w.ticks();
    for (ModInfo& m : mods)
        if (m.running && m.hasTick && t % (uint64_t)m.tickEvery == 0) m.tickDue = true;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = mods.size();
    for (size_t k = 0; k < n; ++k) {
        const size_t i = (turn_ + k) % n;
        if (i >= mods.size()) break;  // (a hang restarted everything)
        ModInfo& m = mods[i];
        if (!m.running || !m.tickDue || !m.script) continue;
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
        if (!m.duplicate.empty()) {  // never switched on: the switch belongs to the other mod with this id
            m.error = m.duplicate;
            return;
        }
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
    if (m.nat && m.nat->loaded && m.nat->mod.settings_changed &&
        !callNative(i, "settings_changed", w, [](CellFapMod* x, CellFapWorld* cw) { return x->settings_changed(x, cw); }))
        return;
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
