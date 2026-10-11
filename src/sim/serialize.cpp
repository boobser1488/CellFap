#include "serialize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

// UTF-8 path; text that is not valid UTF-8 (e.g. an ANSI-codepage name from an old tool) is taken as is instead of
// throwing out of the game.
static fs::path toPath(const std::string& utf8) {
    try {
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
    } catch (const std::exception&) {
        return fs::path(utf8);
    }
}
static std::string fromPath(const fs::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string dataDir(const char* sub) {
    fs::path base;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    base = fs::path(std::wstring(buf, n)).parent_path();
#else
    base = fs::current_path();
#endif
    fs::path dir = base / sub;
    std::error_code ec;
    fs::create_directories(dir, ec);
    return fromPath(dir);
}

std::string safeFileName(const std::string& s, const char* fallback) {
    std::string out;
    for (char c : s) out += ((unsigned char)c < 32 || std::strchr("/\\:*?\"<>|", c)) ? '_' : c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? fallback : out;
}

FILE* openFileUtf8(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring wmode(mode, mode + std::strlen(mode));
    return _wfopen(toPath(path).c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::vector<std::string> listFiles(const std::string& dir, const std::string& suffix) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(toPath(dir), ec)) {
        if (!e.is_regular_file()) continue;
        std::string name = fromPath(e.path().filename());
        if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}


// Files are written next to the target first and moved over it only when complete: a full disk or a crash in the
// middle must not destroy the previous save.
static bool commitTemp(const std::string& tmp, const std::string& path) {
    std::error_code ec;
    fs::rename(toPath(tmp), toPath(path), ec);
    if (ec) {  // some platforms refuse to replace: remove, then move
        fs::remove(toPath(path), ec);
        ec.clear();
        fs::rename(toPath(tmp), toPath(path), ec);
    }
    if (ec) fs::remove(toPath(tmp), ec);
    return !ec;
}

// ---------------------------------------------------------------- genome text
bool saveGenome(const Genome& g, const std::string& path) {
    const std::string tmp = path + ".tmp";
    std::ofstream f(toPath(tmp));
    if (!f) return false;
    f << "CellFapGenome 1\n";
    f.precision(9);  // floats survive save -> load exactly
    auto oneLine = [](std::string s) {  // a newline in a name would start a new record
        for (char& c : s)
            if (c == '\n' || c == '\r') c = ' ';
        return s;
    };
    f << "name " << oneLine(g.name) << "\n";
    f << "initial " << g.initialMode << "\n";
    for (int i = 0; i < kMaxModes; ++i) {
        // Untouched modes are left out: the reader starts from the same defaults.
        if (g.modes.isDefault(i) && g.labels[i].empty()) continue;
        const Mode& m = g.modes[i];
        f << "mode " << i << "\n";
        if (!g.labels[i].empty()) f << " label " << oneLine(g.labels[i]) << "\n";
        f << " type " << (int)m.type << "\n";
        f << " color " << m.color.r << " " << m.color.g << " " << m.color.b << "\n";
        f << " makeAdhesin " << m.makeAdhesin << "\n";
        f << " prioritize " << m.prioritize << "\n";
        f << " splitMass " << m.splitMass << "\n";
        f << " splitRatio " << m.splitRatio << "\n";
        f << " priority " << m.nutrientPriority << "\n";
        f << " splitAngle " << rad2deg(m.splitAngle) << "\n";
        for (int c = 0; c < 2; ++c)
            f << " child " << c << " " << (int)m.childMode[c] << " " << rad2deg(m.childAngle[c]) << " " << m.keepAdhesin[c]
              << " " << m.mirror[c] << "\n";
        f << " stiffness " << m.adhesinStiffness << "\n";
        f << " cytoskeleton " << m.cytoskeleton << "\n";
        f << " adhesinLength " << m.adhesinLength << "\n";
        f << " maxConnections " << m.maxConnections << "\n";
        for (int p = 0; p < PF_Count; ++p) {
            const Prog& pr = m.prog[p];
            f << " prog " << p << " " << (int)pr.input << " " << (int)pr.kind << " " << pr.a << " " << pr.b << " " << pr.c << "\n";
        }
        f << " stem " << (int)m.stemMode[0] << " " << (int)m.stemMode[1] << "\n";
        f << " neuro " << (int)m.neuroChannel[0] << " " << (int)m.neuroChannel[1] << " " << (int)m.neuroChannel[2] << " "
          << (int)m.neuroChannel[3] << "\n";
        f << " virus " << (int)m.virusCopyFrom << "\n";
        f << " sense " << (int)m.senseTarget << " " << (int)m.senseChannel << " " << m.senseOutput << " " << m.senseColor.r << " "
          << m.senseColor.g << " " << m.senseColor.b << " " << m.senseThreshold << "\n";
        f << " secretion " << (int)m.secretion << "\n";
        f << " gamete " << (int)m.gameteCompatibleMode << " " << (int)m.gameteModeAfter << " " << (int)m.gameteTelomeres << "\n";
    }
    f << "end\n";
    f.close();
    return !f.fail() && commitTemp(tmp, path);
}

static bool parseGenome(Genome& out, std::istream& f) {
    std::string header;
    int version = 0;
    // A UTF-8 byte-order mark (added by some editors and messengers) precedes the header.
    if (f.peek() == 0xEF) {
        char bom[3];
        f.read(bom, 3);
        if (!((unsigned char)bom[0] == 0xEF && (unsigned char)bom[1] == 0xBB && (unsigned char)bom[2] == 0xBF)) f.seekg(0);
    }
    f >> header >> version;
    if (header != "CellFapGenome" || version != 1) return false;
    Genome g;
    Mode* m = nullptr;
    int modeIndex = 0;
    std::string line;
    std::getline(f, line);
    auto u8 = [](int v) { return (uint8_t)clampv(v, 0, kMaxModes - 1); };
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // Windows line ends
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "name") {
            std::getline(ss, g.name);
            if (!g.name.empty() && g.name[0] == ' ') g.name.erase(0, 1);
        } else if (key == "initial") {
            ss >> g.initialMode;
            g.initialMode = clampv(g.initialMode, 0, kMaxModes - 1);
        } else if (key == "mode") {
            int i = 0;
            ss >> i;
            modeIndex = clampv(i, 0, kMaxModes - 1);
            m = &g.modes.edit(modeIndex);
        } else if (key == "end") {
            break;
        } else if (m) {
            int a = 0, b = 0, c = 0, d = 0;
            float x = 0, y = 0;
            if (key == "label") {
                std::string rest;
                std::getline(ss, rest);
                size_t a = rest.find_first_not_of(" \t"), b = rest.find_last_not_of(" \t\r");
                g.labels.set(modeIndex, a == std::string::npos ? "" : rest.substr(a, b - a + 1));
            } else if (key == "type") { ss >> a; m->type = (CellType)clampv(a, 0, kCellTypeCount - 1); }
            else if (key == "color") ss >> m->color.r >> m->color.g >> m->color.b;
            else if (key == "makeAdhesin") ss >> m->makeAdhesin;
            else if (key == "prioritize") ss >> m->prioritize;
            else if (key == "splitMass") ss >> m->splitMass;
            else if (key == "splitRatio") { ss >> m->splitRatio; m->splitRatio = clampv(m->splitRatio, 0.1f, 0.9f); }
            else if (key == "priority") { ss >> m->nutrientPriority; m->nutrientPriority = clampv(m->nutrientPriority, 0.1f, 10.0f); }
            else if (key == "splitAngle") { ss >> x; m->splitAngle = deg2rad(x); }
            else if (key == "child") {
                ss >> a >> b >> y >> c >> d;
                a = clampv(a, 0, 1);
                m->childMode[a] = u8(b);
                m->childAngle[a] = deg2rad(y);
                m->keepAdhesin[a] = c != 0;
                m->mirror[a] = d != 0;
            } else if (key == "stiffness") ss >> m->adhesinStiffness;
            else if (key == "cytoskeleton") ss >> m->cytoskeleton;
            else if (key == "adhesinLength") ss >> m->adhesinLength;
            else if (key == "maxConnections") { ss >> m->maxConnections; m->maxConnections = clampv(m->maxConnections, 1, kMaxLinks); }
            else if (key == "prog") {
                ss >> a >> b >> c;
                if (a >= 0 && a < PF_Count) {
                    Prog& p = m->prog[a];
                    p.input = (ProgInput)clampv(b, 0, (int)ProgInput::Count - 1);
                    p.kind = (ProgKind)clampv(c, 0, (int)ProgKind::Count - 1);
                    ss >> p.a >> p.b >> p.c;
                }
            } else if (key == "stem") { ss >> a >> b; m->stemMode[0] = u8(a); m->stemMode[1] = u8(b); }
            else if (key == "neuro") {
                int ch[4] = {};
                ss >> ch[0] >> ch[1] >> ch[2] >> ch[3];
                for (int k = 0; k < 4; ++k) m->neuroChannel[k] = (uint8_t)clampv(ch[k], 0, kSignalCount - 1);
            } else if (key == "virus") { ss >> a; m->virusCopyFrom = u8(a); }
            else if (key == "sense") {
                ss >> a >> b >> m->senseOutput >> m->senseColor.r >> m->senseColor.g >> m->senseColor.b >> m->senseThreshold;
                m->senseTarget = (SenseTarget)clampv(a, 0, (int)SenseTarget::Count - 1);
                m->senseChannel = (uint8_t)clampv(b, 0, kSignalCount - 1);
            } else if (key == "secretion") { ss >> a; m->secretion = (Secretion)clampv(a, 0, (int)Secretion::Count - 1); }
            else if (key == "gamete") {
                ss >> a >> b >> c;
                m->gameteCompatibleMode = u8(a);
                m->gameteModeAfter = u8(b);
                m->gameteTelomeres = (uint8_t)clampv(c, 0, 5);
            }
        }
    }
    out = g;
    return true;
}

bool loadGenome(Genome& out, const std::string& path) {
    std::ifstream f(toPath(path));
    if (!f) return false;
    return parseGenome(out, f);
}

bool genomeFromText(Genome& out, const std::string& text) {
    std::istringstream f(text);
    return parseGenome(out, f);
}

// ---------------------------------------------------------------- world binary
static_assert(std::is_trivially_copyable_v<Cell>);
static_assert(std::is_trivially_copyable_v<Link>);
static_assert(std::is_trivially_copyable_v<Food>);
static_assert(std::is_trivially_copyable_v<Fluid>);
static_assert(std::is_trivially_copyable_v<Mode>);
static_assert(std::is_trivially_copyable_v<Settings>);

namespace {
constexpr char kMagic[8] = {'C', 'F', 'A', 'P', 'S', 'U', 'B', '1'};
// 2: mode labels; 3: map barriers; 4: (removed) envelope; 6: 8 signals, 21 cell types, living food, fluids;
// 7: gene pool with room for 32 cell types, day length; 8: water currents (a velocity grid after the fluids).
// Settings may grow at the end within one layout generation.
constexpr uint32_t kVersion = 8;

// Layouts of format versions 1..5 (18 cell types, 4 signals, plain food), converted on load.
namespace v5 {
struct Settings {
    float dishRadius;
    int maxCells, maxFood;
    bool lid, contaminate, killAtEdge, onlyPointMutations, cellAging, mobileFood;
    std::array<bool, 18> genePool;
    float radiation, viscosity, dynFriction, staticFriction, nutrientRate, chunkSize, lumpiness, lumpSize, coating, salinity,
        lightAmount, lightRange, lightDirChange, gravity, density, densityGradient, nitrates, swimAnisotropy, lightAngle,
        mechanicalDamage;
};
struct Cell {
    Vec2 pos, vel, prevPos;
    float angle, angVel, prevAngle, mass, radius, age, nitrogen, injury, boost;
    float signals[4];
    float actuator, lastSense, virusCooldown;
    uint32_t id;
    int32_t genome;
    uint8_t mode;
    CellType type;
    CellTag tag;
    bool alive, mirrored, isProtected;
    int8_t telomeres;
    uint16_t mutations;
    uint8_t linkCount, glueWallLinks, editorFlags;
    int32_t links[kMaxLinks];
};
struct Link {
    int32_t a, b;
    float angA, angB, extraLength, stiffness;
    Vec2 wallPoint;
    bool alive, fromDivision;
    Vec2 fA;
    float tA, tB, massFlow, nitroFlow;
    float sigFlow[4];
};
struct Food {
    Vec2 pos, vel;
    float mass, coating;
};

#define V5_SETTINGS(X)                                                                                                     \
    X(dishRadius) X(maxCells) X(maxFood) X(lid) X(contaminate) X(killAtEdge) X(onlyPointMutations) X(cellAging)            \
    X(mobileFood) X(radiation) X(viscosity) X(dynFriction) X(staticFriction) X(nutrientRate) X(chunkSize) X(lumpiness)    \
    X(lumpSize) X(coating) X(salinity) X(lightAmount) X(lightRange) X(lightDirChange) X(gravity) X(density)               \
    X(densityGradient) X(nitrates) X(swimAnisotropy) X(lightAngle) X(mechanicalDamage)

::Settings convert(const char* bytes, uint32_t size) {
    // Start from today's defaults expressed in the old layout, so fields an older file lacks keep them.
    ::Settings s;
    Settings d;
#define X(f) d.f = s.f;
    V5_SETTINGS(X)
#undef X
    for (int t = 0; t < 18; ++t) d.genePool[t] = s.genePool[t];
    std::memcpy((void*)&d, bytes, std::min<size_t>(size, sizeof d));
#define X(f) s.f = d.f;
    V5_SETTINGS(X)
#undef X
    for (int t = 0; t < 18; ++t) s.genePool[t] = d.genePool[t];
    return s;
}
#undef V5_SETTINGS
::Cell convert(const Cell& o) {
    ::Cell c;
    c.pos = o.pos;
    c.vel = o.vel;
    c.prevPos = o.prevPos;
    c.angle = o.angle;
    c.angVel = o.angVel;
    c.prevAngle = o.prevAngle;
    c.mass = o.mass;
    c.radius = o.radius;
    c.age = o.age;
    c.nitrogen = o.nitrogen;
    c.injury = o.injury;
    c.boost = o.boost;
    for (int s = 0; s < 4; ++s) c.signals[s] = o.signals[s];
    c.actuator = o.actuator;
    c.lastSense = o.lastSense;
    c.virusCooldown = o.virusCooldown;
    c.id = o.id;
    c.genome = o.genome;
    c.mode = o.mode;
    c.type = o.type;
    c.tag = o.tag;
    c.alive = o.alive;
    c.mirrored = o.mirrored;
    c.isProtected = o.isProtected;
    c.telomeres = o.telomeres;
    c.mutations = o.mutations;
    c.linkCount = o.linkCount;
    c.glueWallLinks = o.glueWallLinks;
    c.editorFlags = o.editorFlags;
    std::memcpy(c.links, o.links, sizeof c.links);
    return c;
}
::Link convert(const Link& o) {
    ::Link l;
    l.a = o.a;
    l.b = o.b;
    l.angA = o.angA;
    l.angB = o.angB;
    l.extraLength = o.extraLength;
    l.stiffness = o.stiffness;
    l.wallPoint = o.wallPoint;
    l.alive = o.alive;
    l.fromDivision = o.fromDivision;
    return l;
}
::Food convert(const Food& o) {
    return ::Food{o.pos, o.vel, o.mass, o.coating};
}
}  // namespace v5

// Version 6: the gene pool had exactly 21 entries (one per cell type of the time), which shifts every field after it.
namespace v6 {
struct Settings {
    float dishRadius;
    int maxCells, maxFood;
    bool lid, contaminate, killAtEdge, onlyPointMutations, cellAging, mobileFood;
    std::array<bool, 21> genePool;
    float radiation, viscosity, dynFriction, staticFriction, nutrientRate, chunkSize, lumpiness, lumpSize, coating, salinity,
        lightAmount, lightRange, lightDirChange, gravity, density, densityGradient, nitrates, swimAnisotropy, lightAngle,
        mechanicalDamage, sapLeak, lightCapacity;
};
#define V6_SETTINGS(X)                                                                                                     \
    X(dishRadius) X(maxCells) X(maxFood) X(lid) X(contaminate) X(killAtEdge) X(onlyPointMutations) X(cellAging)            \
    X(mobileFood) X(radiation) X(viscosity) X(dynFriction) X(staticFriction) X(nutrientRate) X(chunkSize) X(lumpiness)    \
    X(lumpSize) X(coating) X(salinity) X(lightAmount) X(lightRange) X(lightDirChange) X(gravity) X(density)               \
    X(densityGradient) X(nitrates) X(swimAnisotropy) X(lightAngle) X(mechanicalDamage) X(sapLeak) X(lightCapacity)

::Settings convert(const char* bytes, uint32_t size) {
    ::Settings s;
    Settings d;
#define X(f) d.f = s.f;
    V6_SETTINGS(X)
#undef X
    for (int t = 0; t < 21; ++t) d.genePool[t] = s.genePool[t];
    std::memcpy((void*)&d, bytes, std::min<size_t>(size, sizeof d));
#define X(f) s.f = d.f;
    V6_SETTINGS(X)
#undef X
    for (int t = 0; t < 21; ++t) s.genePool[t] = d.genePool[t];
    return s;
}
#undef V6_SETTINGS
}  // namespace v6

struct Writer {
    std::ofstream f;
    template <class T>
    void pod(const T& v) { f.write((const char*)&v, sizeof(T)); }
    template <class T>
    void vec(const std::vector<T>& v) {
        uint64_t n = v.size();
        pod(n);
        if (n) f.write((const char*)v.data(), sizeof(T) * n);
    }
    void str(const std::string& s) {
        uint32_t n = (uint32_t)s.size();
        pod(n);
        f.write(s.data(), n);
    }
};

struct Reader {
    std::ifstream f;
    template <class T>
    bool pod(T& v) { return (bool)f.read((char*)&v, sizeof(T)); }
    template <class T>
    bool vec(std::vector<T>& v, uint64_t maxN = 1ull << 26) {
        uint64_t n = 0;
        if (!pod(n) || n > maxN) return false;
        v.resize(n);
        return n == 0 || (bool)f.read((char*)v.data(), sizeof(T) * n);
    }
    bool str(std::string& s) {
        uint32_t n = 0;
        if (!pod(n) || n > 4096) return false;
        s.resize(n);
        return n == 0 || (bool)f.read(s.data(), n);
    }
};
// Version 6 stores the dish compactly: only live cells and links (without per-tick scratch), only the genomes that
// live cells use, and of each genome only the modes that differ from the defaults.
struct CellDisk {
    Vec2 pos, vel;
    float angle, angVel, mass, age, nitrogen, injury, boost;
    float signals[kSignalCount];
    float actuator, lastSense, virusCooldown, charge, stun, paralysis, stimulant, leak;
    uint32_t id;
    int32_t genome;
    uint8_t mode, type, tag, mirrored;
    int8_t telomeres;
    uint8_t pad;
    uint16_t mutations;
};
struct LinkDisk {
    int32_t a, b;  // b = -1: glued to the dish wall
    float angA, angB, extraLength, stiffness;
    Vec2 wallPoint;
    uint8_t fromDivision, pad[3];
};
static_assert(std::is_trivially_copyable_v<CellDisk> && std::is_trivially_copyable_v<LinkDisk>);

void writeGenome(Writer& wr, const Genome& g) {
    wr.pod(g.initialMode);
    wr.str(g.name);
    uint16_t n = 0;
    for (int i = 0; i < kMaxModes; ++i) n += !g.modes.isDefault(i);
    wr.pod(n);
    for (int i = 0; i < kMaxModes; ++i)
        if (!g.modes.isDefault(i)) {
            uint8_t k = (uint8_t)i;
            wr.pod(k);
            wr.pod(g.modes[i]);
        }
    uint16_t nl = 0;
    for (int i = 0; i < kMaxModes; ++i) nl += !g.labels[i].empty();
    wr.pod(nl);
    for (int i = 0; i < kMaxModes; ++i)
        if (!g.labels[i].empty()) {
            uint8_t k = (uint8_t)i;
            wr.pod(k);
            wr.str(g.labels[i]);
        }
}

bool readGenome(Reader& rd, Genome& g) {
    uint16_t n = 0, nl = 0;
    if (!rd.pod(g.initialMode) || !rd.str(g.name) || !rd.pod(n) || n > kMaxModes) return false;
    g.initialMode = clampv(g.initialMode, 0, kMaxModes - 1);
    for (int j = 0; j < n; ++j) {
        uint8_t k = 0;
        Mode m;
        if (!rd.pod(k) || !rd.pod(m) || k >= kMaxModes) return false;
        m.type = (CellType)clampv((int)m.type, 0, kCellTypeCount - 1);
        // A damaged file must not index past the 128 modes or the enums.
        for (uint8_t* idx : {&m.childMode[0], &m.childMode[1], &m.virusCopyFrom, &m.stemMode[0], &m.stemMode[1],
                             &m.gameteCompatibleMode, &m.gameteModeAfter})
            if (*idx >= kMaxModes) *idx = 0;
        m.senseTarget = (SenseTarget)clampv((int)m.senseTarget, 0, (int)SenseTarget::Count - 1);
        m.secretion = (Secretion)clampv((int)m.secretion, 0, (int)Secretion::Count - 1);
        for (Prog& p : m.prog) {
            p.kind = (ProgKind)clampv((int)p.kind, 0, (int)ProgKind::Count - 1);
            p.input = (ProgInput)clampv((int)p.input, 0, (int)ProgInput::Count - 1);
        }
        g.modes.set(k, m);
    }
    if (!rd.pod(nl) || nl > kMaxModes) return false;
    for (int j = 0; j < nl; ++j) {
        uint8_t k = 0;
        std::string s;
        if (!rd.pod(k) || !rd.str(s) || k >= kMaxModes) return false;
        g.labels.set(k, s);
    }
    return true;
}
}  // namespace

bool saveWorld(const World& w, const char* path) {
    Writer wr;
    const std::string tmp = std::string(path) + ".tmp";
    wr.f.open(toPath(tmp), std::ios::binary);
    if (!wr.f) return false;
    wr.f.write(kMagic, 8);
    wr.pod(kVersion);
    uint32_t sizes[5] = {(uint32_t)sizeof(Settings), (uint32_t)sizeof(CellDisk), (uint32_t)sizeof(LinkDisk),
                         (uint32_t)sizeof(Food), (uint32_t)sizeof(Mode)};
    wr.pod(sizes);
    wr.pod(w.settings);
    wr.pod(w.time_);
    wr.pod(w.ticks_);
    wr.pod(w.nextId_);

    // Live cells get consecutive numbers; genomes are renumbered in order of first use.
    const auto& cells = w.cells_;
    std::vector<int32_t> cellIdx(cells.size(), -1), genomeIdx(w.genomes_.genomes.size(), -1);
    std::vector<int> usedGenomes;
    int32_t nCells = 0;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (!cells[i].alive) continue;
        cellIdx[i] = nCells++;
        int g = cells[i].genome;
        if (genomeIdx[g] < 0) {
            genomeIdx[g] = (int32_t)usedGenomes.size();
            usedGenomes.push_back(g);
        }
    }
    uint32_t ng = (uint32_t)usedGenomes.size();
    wr.pod(ng);
    for (int g : usedGenomes) writeGenome(wr, w.genomes_.genomes[g]);

    std::vector<CellDisk> cd;
    cd.reserve(nCells);
    for (const Cell& c : cells) {
        if (!c.alive) continue;
        CellDisk d{};
        d.pos = c.pos;
        d.vel = c.vel;
        d.angle = c.angle;
        d.angVel = c.angVel;
        d.mass = c.mass;
        d.age = c.age;
        d.nitrogen = c.nitrogen;
        d.injury = c.injury;
        d.boost = c.boost;
        std::memcpy(d.signals, c.signals, sizeof d.signals);
        d.actuator = c.actuator;
        d.lastSense = c.lastSense;
        d.virusCooldown = c.virusCooldown;
        d.charge = c.charge;
        d.stun = c.stun;
        d.paralysis = c.paralysis;
        d.stimulant = c.stimulant;
        d.leak = c.leak;
        d.id = c.id;
        d.genome = genomeIdx[c.genome];
        d.mode = c.mode;
        d.type = (uint8_t)c.type;
        d.tag = (uint8_t)c.tag;
        d.mirrored = c.mirrored;
        d.telomeres = c.telomeres;
        d.mutations = c.mutations;
        cd.push_back(d);
    }
    wr.vec(cd);
    std::vector<LinkDisk> ld;
    for (const Link& l : w.links_) {
        if (!l.alive || !cells[l.a].alive || (l.b >= 0 && !cells[l.b].alive)) continue;
        LinkDisk d{};
        d.a = cellIdx[l.a];
        d.b = l.b >= 0 ? cellIdx[l.b] : -1;
        d.angA = l.angA;
        d.angB = l.angB;
        d.extraLength = l.extraLength;
        d.stiffness = l.stiffness;
        d.wallPoint = l.wallPoint;
        d.fromDivision = l.fromDivision;
        ld.push_back(d);
    }
    wr.vec(ld);
    wr.vec(w.food_);
    wr.vec(w.barriers_);
    wr.vec(w.fluids_);
    wr.vec(w.flow_);
    wr.f.close();
    return !wr.f.fail() && commitTemp(tmp, path);
}

bool loadWorld(World& w, const char* path) {
    Reader rd;
    rd.f.open(toPath(path), std::ios::binary);
    if (!rd.f) return false;
    char magic[8];
    uint32_t version = 0, sizes[5];
    if (!rd.f.read(magic, 8) || std::memcmp(magic, kMagic, 8) != 0) return false;
    if (!rd.pod(version) || version < 1 || version > kVersion || !rd.pod(sizes)) return false;
    const bool legacy = version <= 5;
    uint32_t expect[5] = {(uint32_t)sizeof(Settings), (uint32_t)sizeof(CellDisk), (uint32_t)sizeof(LinkDisk),
                          (uint32_t)sizeof(Food), (uint32_t)sizeof(Mode)};
    if (legacy) {
        expect[0] = (uint32_t)sizeof(v5::Settings);
        expect[1] = (uint32_t)sizeof(v5::Cell);
        expect[2] = (uint32_t)sizeof(v5::Link);
        expect[3] = (uint32_t)sizeof(v5::Food);
    } else if (version == 6) {
        expect[0] = (uint32_t)sizeof(v6::Settings);
    }
    // Settings only ever grow at the end: a shorter block from an older version keeps the new fields' defaults.
    if (sizes[0] > expect[0] || std::memcmp(sizes + 1, expect + 1, sizeof(expect) - sizeof(uint32_t)) != 0) return false;

    Settings s;
    double time = 0;
    uint64_t ticks = 0;
    uint32_t nextId = 1;
    {
        std::vector<char> raw(sizes[0]);
        if (!rd.f.read(raw.data(), sizes[0])) return false;
        if (legacy) s = v5::convert(raw.data(), sizes[0]);
        else if (version == 6) s = v6::convert(raw.data(), sizes[0]);
        else std::memcpy((void*)&s, raw.data(), sizes[0]);
    }
    if (!rd.pod(time) || !rd.pod(ticks) || !rd.pod(nextId)) return false;

    GenomePool pool;
    std::vector<Cell> cells;
    std::vector<int> freeCells, freeLinks;
    std::vector<Link> links;
    std::vector<Food> food;
    std::vector<World::Barrier> barriers;
    std::vector<Fluid> fluids;
    std::vector<Vec2> flow;  // version 8: water currents
    int cellCount = 0, linkCount = 0;

    if (legacy) {
        uint64_t ng = 0;
        if (!rd.pod(ng) || ng > (1u << 22)) return false;
        pool.genomes.resize(ng);
        pool.refs.resize(ng);
        for (uint64_t i = 0; i < ng; ++i) {
            Genome& g = pool.genomes[i];
            if (!rd.pod(pool.refs[i]) || !rd.pod(g.initialMode) || !rd.str(g.name)) return false;
            std::vector<Mode> modes(kMaxModes);
            if (!rd.f.read((char*)modes.data(), sizeof(Mode) * kMaxModes)) return false;
            for (int k = 0; k < kMaxModes; ++k)
                if (!modeEquals(modes[k], g.modes[k])) g.modes.set(k, modes[k]);
            if (version >= 2)
                for (int k = 0; k < kMaxModes; ++k) {
                    std::string l;
                    if (!rd.str(l)) return false;
                    g.labels.set(k, l);
                }
            float envelopeOfV4 = 0;  // version 4 stored an organism envelope, since removed
            if (version == 4 && !rd.pod(envelopeOfV4)) return false;
        }
        std::vector<v5::Cell> oc;
        std::vector<v5::Link> ol;
        std::vector<v5::Food> of;
        if (!rd.vec(pool.freeList) || !rd.vec(oc) || !rd.vec(freeCells) || !rd.vec(ol) || !rd.vec(freeLinks) || !rd.vec(of) ||
            !rd.pod(cellCount) || !rd.pod(linkCount))
            return false;
        cells.reserve(oc.size());
        for (const v5::Cell& c : oc) cells.push_back(v5::convert(c));
        links.reserve(ol.size());
        for (const v5::Link& l : ol) links.push_back(v5::convert(l));
        food.reserve(of.size());
        for (const v5::Food& f : of) food.push_back(v5::convert(f));
        if (version >= 3 && !rd.vec(barriers)) return false;
        // Validate references so a corrupt file cannot index out of range.
        for (const Cell& c : cells) {
            if (!c.alive) continue;
            if (c.genome < 0 || (uint64_t)c.genome >= ng || c.mode >= kMaxModes || c.linkCount > kMaxLinks) return false;
            for (int k = 0; k < c.linkCount; ++k)
                if (c.links[k] < 0 || (size_t)c.links[k] >= links.size()) return false;
        }
        for (const Link& l : links) {
            if (!l.alive) continue;
            if (l.a < 0 || (size_t)l.a >= cells.size() || l.b >= (int)cells.size()) return false;
        }
    } else {
        uint32_t ng = 0;
        if (!rd.pod(ng) || ng > (1u << 22)) return false;
        pool.genomes.resize(ng);
        pool.refs.assign(ng, 0);
        for (uint32_t i = 0; i < ng; ++i)
            if (!readGenome(rd, pool.genomes[i])) return false;
        std::vector<CellDisk> cd;
        std::vector<LinkDisk> ld;
        if (!rd.vec(cd) || !rd.vec(ld) || !rd.vec(food) || !rd.vec(barriers) || !rd.vec(fluids)) return false;
        if (version >= 8 && (!rd.vec(flow) || (!flow.empty() && flow.size() != (size_t)World::kFlowN * World::kFlowN))) return false;
        cells.resize(cd.size());
        for (size_t i = 0; i < cd.size(); ++i) {
            const CellDisk& d = cd[i];
            if (d.genome < 0 || (uint32_t)d.genome >= ng || d.mode >= kMaxModes || d.type >= kCellTypeCount) return false;
            Cell& c = cells[i];
            c.pos = c.prevPos = d.pos;
            c.vel = d.vel;
            c.angle = c.prevAngle = d.angle;
            c.angVel = d.angVel;
            c.mass = d.mass;
            c.age = d.age;
            c.nitrogen = d.nitrogen;
            c.injury = d.injury;
            c.boost = d.boost;
            std::memcpy(c.signals, d.signals, sizeof c.signals);
            c.actuator = d.actuator;
            c.lastSense = d.lastSense;
            c.virusCooldown = d.virusCooldown;
            c.charge = d.charge;
            c.stun = d.stun;
            c.paralysis = d.paralysis;
            c.stimulant = d.stimulant;
            c.leak = d.leak;
            c.id = d.id;
            c.genome = d.genome;
            c.mode = d.mode;
            c.type = (CellType)d.type;
            c.tag = (CellTag)d.tag;
            c.mirrored = d.mirrored != 0;
            c.telomeres = d.telomeres;
            c.mutations = d.mutations;
            c.alive = true;
            c.radius = radiusForMass(c.mass, c.type);
            pool.refs[d.genome]++;
        }
        cellCount = (int)cells.size();
        for (const LinkDisk& d : ld) {
            if (d.a < 0 || (size_t)d.a >= cells.size() || d.b >= (int)cells.size() || d.b < -1) return false;
            Cell& A = cells[d.a];
            if (A.linkCount >= kMaxLinks || (d.b >= 0 && cells[d.b].linkCount >= kMaxLinks)) continue;
            Link l;
            l.a = d.a;
            l.b = d.b;
            l.angA = d.angA;
            l.angB = d.angB;
            l.extraLength = d.extraLength;
            l.stiffness = d.stiffness;
            l.wallPoint = d.wallPoint;
            l.fromDivision = d.fromDivision != 0;
            l.alive = true;
            int li = (int)links.size();
            links.push_back(l);
            A.links[A.linkCount++] = li;
            if (d.b >= 0) {
                Cell& B = cells[d.b];
                B.links[B.linkCount++] = li;
            } else {
                A.glueWallLinks++;
            }
        }
        linkCount = (int)links.size();
        for (uint32_t i = 0; i < ng; ++i)
            if (pool.refs[i] == 0) pool.freeList.push_back((int)i);
    }

    w.reset(s);
    w.genomes_ = std::move(pool);
    w.cells_ = std::move(cells);
    w.freeCells_ = std::move(freeCells);
    w.links_ = std::move(links);
    w.freeLinks_ = std::move(freeLinks);
    w.food_ = std::move(food);
    w.cellCount_ = cellCount;
    w.linkCount_ = linkCount;
    w.barriers_ = std::move(barriers);
    w.fluids_ = std::move(fluids);
    w.flow_ = std::move(flow);
    w.barrierGridDirty_ = true;
    w.time_ = time;
    w.ticks_ = ticks;
    w.nextId_ = nextId;
    w.nextStatTime_ = time;
    return true;
}
