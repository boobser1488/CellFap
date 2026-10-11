#include "cellab_import.hpp"

#include "serialize.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

// The world's internals the importer needs (World declares it a friend).
struct CellLabAccess {
    static void setTime(World& w, double t) {
        w.time_ = t;
        w.nextStatTime_ = t;
    }
    static Cell& cell(World& w, int i) { return w.cells_[i]; }
    // A link with the anchor angles of the file (on a and on b, each in its cell's frame).
    static void link(World& w, int a, int b, float extraLen, float stiffness, float angA, float angB) {
        int li = w.createLink(a, b, extraLen, stiffness, true);
        if (li < 0) return;
        w.links_[li].angA = wrapAngle(angA);
        w.links_[li].angB = wrapAngle(angB);
    }
};

namespace {

constexpr int kVersionOriginal = 95;
constexpr int kVersionMod = 3055;
constexpr int kNumValues = 11, kNumInts = 11, kNumFloats = 7;
constexpr int kGeneCore = 67 + kNumValues * 16 + kNumInts * 4 + kNumFloats * 4;  // 315 bytes (format 95)
// One length unit of Cell Lab in µm. Its cell radius is sqrt(mass/400) units (mass in 10 ng) = 0.03 at 3.6 ng; ours is
// 15 µm, so 1 unit = 500 µm. (The game labels the unit "mm"; positions, the dish radius and food use it. Adhesin
// lengths are added at both ends of a link, so they convert with 1000.)
constexpr float kUmPerUnit = 500.0f;

struct Fail {
    std::string msg;
};

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = openFileUtf8(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1L << 30)) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)n);
    bool ok = n == 0 || std::fread(out.data(), 1, (size_t)n, f) == (size_t)n;
    std::fclose(f);
    return ok;
}

// Concatenates the TC_BLOCKDATA / TC_BLOCKDATALONG payloads of a Java ObjectOutputStream starting at `pos`
// (just after the stream magic). Stops at the first other byte and returns its offset.
size_t javaBlocks(const std::vector<uint8_t>& d, size_t pos, std::vector<uint8_t>& out) {
    while (pos < d.size()) {
        uint8_t tc = d[pos];
        size_t n;
        if (tc == 0x77) {
            if (pos + 2 > d.size()) break;
            n = d[pos + 1];
            pos += 2;
        } else if (tc == 0x7A) {
            if (pos + 5 > d.size()) break;
            n = ((size_t)d[pos + 1] << 24) | ((size_t)d[pos + 2] << 16) | ((size_t)d[pos + 3] << 8) | d[pos + 4];
            pos += 5;
        } else {
            break;
        }
        if (pos + n > d.size()) throw Fail{"Повреждённый блок данных"};
        out.insert(out.end(), d.begin() + (long)pos, d.begin() + (long)(pos + n));
        pos += n;
    }
    return pos;
}

bool hasMagic(const std::vector<uint8_t>& d, size_t pos = 0) {
    return d.size() >= pos + 4 && d[pos] == 0xAC && d[pos + 1] == 0xED && d[pos + 2] == 0x00 && d[pos + 3] == 0x05;
}

std::vector<uint8_t> gunzip(const uint8_t* src, size_t n) {
    std::vector<uint8_t> out;
    z_stream zs{};
    if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK) throw Fail{"zlib: init"};
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = (uInt)n;
    std::vector<uint8_t> buf(1 << 20);
    int r;
    do {
        zs.next_out = buf.data();
        zs.avail_out = (uInt)buf.size();
        r = inflate(&zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            inflateEnd(&zs);
            throw Fail{"Не удалось распаковать данные (gzip)"};
        }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
        if (out.size() > (1u << 30)) {
            inflateEnd(&zs);
            throw Fail{"Слишком большой файл"};
        }
    } while (r != Z_STREAM_END && zs.avail_in > 0);
    inflateEnd(&zs);
    return out;
}

// Current Cell Lab saves a genome as a ZIP: save.bin (the Java stream), mode_names.json, sometimes thumbnail.png.
// Returns the named entry, or false when the archive has no such file. Stored and deflated entries are supported.
bool unzipEntry(const std::vector<uint8_t>& z, const char* wanted, std::vector<uint8_t>& out) {
    auto le16 = [&](size_t p) { return (uint32_t)z[p] | ((uint32_t)z[p + 1] << 8); };
    auto le32 = [&](size_t p) { return le16(p) | (le16(p + 2) << 16); };
    if (z.size() < 22) return false;
    // End of central directory: the last 0x06054b50 within the final 64 KiB (an archive comment may follow it).
    size_t eocd = std::string::npos;
    for (size_t p = z.size() - 22;; --p) {
        if (le32(p) == 0x06054b50u) {
            eocd = p;
            break;
        }
        if (p == 0 || z.size() - p > 65557) break;
    }
    if (eocd == std::string::npos) return false;
    size_t cd = le32(eocd + 16), entries = le16(eocd + 10);
    for (size_t e = 0; e < entries; ++e) {
        if (cd + 46 > z.size() || le32(cd) != 0x02014b50u) return false;
        uint32_t method = le16(cd + 10), csize = le32(cd + 20), usize = le32(cd + 24);
        size_t nameLen = le16(cd + 28), extraLen = le16(cd + 30), commentLen = le16(cd + 32), local = le32(cd + 42);
        if (cd + 46 + nameLen > z.size()) return false;
        std::string name((const char*)&z[cd + 46], nameLen);
        cd += 46 + nameLen + extraLen + commentLen;
        if (name != wanted) continue;
        if (local + 30 > z.size() || le32(local) != 0x04034b50u) return false;
        size_t data = local + 30 + le16(local + 26) + le16(local + 28);
        if (data + csize > z.size() || usize > (1u << 30)) return false;
        if (method == 0) {
            out.assign(z.begin() + (long)data, z.begin() + (long)(data + csize));
            return true;
        }
        if (method != 8) return false;
        out.resize(usize);
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in = const_cast<Bytef*>(&z[data]);
        zs.avail_in = csize;
        zs.next_out = out.data();
        zs.avail_out = usize;
        int r = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        return r == Z_STREAM_END;
    }
    return false;
}

// mode_names.json: {"version":1,"names":["", "Голова", ...]} — strings with JSON escapes (\uXXXX included).
std::vector<std::string> parseModeNames(const std::vector<uint8_t>& j) {
    std::vector<std::string> names;
    std::string s(j.begin(), j.end());
    size_t p = s.find("\"names\"");
    if (p == std::string::npos || (p = s.find('[', p)) == std::string::npos) return names;
    auto putUtf8 = [](std::string& o, uint32_t c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) o += (char)(0xC0 | (c >> 6)), o += (char)(0x80 | (c & 63));
        else o += (char)(0xE0 | (c >> 12)), o += (char)(0x80 | ((c >> 6) & 63)), o += (char)(0x80 | (c & 63));
    };
    for (++p; p < s.size() && s[p] != ']'; ++p) {
        if (s[p] != '"') continue;
        std::string v;
        for (++p; p < s.size() && s[p] != '"'; ++p) {
            if (s[p] != '\\' || p + 1 >= s.size()) {
                v += s[p];
                continue;
            }
            char e = s[++p];
            if (e == 'u' && p + 4 < s.size()) {
                putUtf8(v, (uint32_t)std::strtoul(s.substr(p + 1, 4).c_str(), nullptr, 16));
                p += 4;
            } else {
                v += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
        }
        names.push_back(v);
    }
    return names;
}

// Big-endian reader with bounds checks (Java DataOutput).
struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    void need(size_t n) const {
        if (p + n > d.size()) throw Fail{"Неожиданный конец файла"};
    }
    uint32_t u32() {
        need(4);
        uint32_t v = ((uint32_t)d[p] << 24) | ((uint32_t)d[p + 1] << 16) | ((uint32_t)d[p + 2] << 8) | d[p + 3];
        p += 4;
        return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    int16_t i16() {
        need(2);
        int16_t v = (int16_t)(((uint16_t)d[p] << 8) | d[p + 1]);
        p += 2;
        return v;
    }
    bool b8() {
        need(1);
        return d[p++] != 0;
    }
    float f32() {
        uint32_t u = u32();
        float f;
        std::memcpy(&f, &u, 4);
        return std::isfinite(f) ? f : 0.0f;
    }
    double f64() {
        uint64_t hi = u32(), lo = u32();
        uint64_t u = (hi << 32) | lo;
        double v;
        std::memcpy(&v, &u, 8);
        return std::isfinite(v) ? v : 0.0;
    }
    void skip(size_t n) {
        need(n);
        p += n;
    }
};

// Big-endian writer (Java DataOutput).
struct Writer {
    std::vector<uint8_t> d;
    void u32(uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) d.push_back((uint8_t)(v >> s));
    }
    void i32(int32_t v) { u32((uint32_t)v); }
    void i16(int16_t v) {
        d.push_back((uint8_t)((uint16_t)v >> 8));
        d.push_back((uint8_t)v);
    }
    void b8(bool b) { d.push_back(b ? 1 : 0); }
    void f32(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        u32(u);
    }
    void f64(double v) {
        uint64_t u;
        std::memcpy(&u, &v, 8);
        u32((uint32_t)(u >> 32));
        u32((uint32_t)u);
    }
    void zeros(size_t n) { d.insert(d.end(), n, 0); }
};

int geneSizeFor(int version) { return version == kVersionMod ? 331 : kGeneCore; }

// Cell Lab's 18 cell types have the same indices as ours. Anything else (the mod's extra type, a newer game's)
// has no equivalent: it becomes a phagocyte and is counted, never one of CellFap's own new types.
constexpr int kCellLabTypes = 18;
CellType mapType(int t, int& unknown) {
    if (t >= 0 && t < kCellLabTypes) return (CellType)t;
    ++unknown;
    return CellType::Phagocyte;
}

// ---------------------------------------------------------------------------- one mode ("gene")
// A mode as the Cell Lab family stores it. Two byte layouts carry the same fields: the .genome record (format
// 95/96, 315 bytes) and the native record inside MinLab's "CLSB" substrates (307 bytes, other field order).
struct RawGene {
    float color[3] = {0.5f, 0.5f, 0.5f};
    float splitMass = 0.252f;  // units of 10 ng
    float splitRatio = 0.5f, splitAngle = 0, childAngle[2] = {0, 0}, priority = 1;
    float stiffness = 35;  // 0..35
    int child[2] = {0, 0};
    bool makeAdhesin = false, keep[2] = {true, true}, prioritize = true, initial = false, mirror[2] = {false, false};
    int type = 0;
    struct Value {
        int input = 0, mode = 0;  // input 0..3 = S1..S4, 4 mass, 5 age, 6 nitrogen, 7 links; mode 0 fixed, 1 linear, 2 step
        float a = 0, b = 0, c = 0;
    } v[kNumValues];
    int ints[kNumInts] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 19, 0};
    float fl[kNumFloats] = {0, 0, 0, 0, 0, 0, 1};
    bool hasTail = true;  // false: only the first 67 bytes were understood (ZIP genomes of a newer modded game)
};

// Header of the .genome record (bytes 0..66): version, colour, split, children, flags, stiffness.
void readGeneHead(Reader& r, RawGene& g, bool withStiffness = true) {  // Macro: flags come before the stiffness
    r.i32();  // per-mode version
    for (float& c : g.color) c = r.f32();
    g.splitMass = r.f32();
    g.splitRatio = r.f32();
    g.splitAngle = r.f32();
    g.childAngle[0] = r.f32();
    g.childAngle[1] = r.f32();
    g.priority = r.f32();
    g.child[0] = r.i32();
    g.child[1] = r.i32();
    g.makeAdhesin = r.b8();
    g.keep[0] = r.b8();
    g.keep[1] = r.b8();
    g.type = r.i32();
    r.i32();  // "lucky number", unused
    g.prioritize = r.b8();
    g.initial = r.b8();
    g.mirror[0] = r.b8();
    g.mirror[1] = r.b8();
    if (withStiffness) g.stiffness = r.f32();
}

void readGeneTail(Reader& r, RawGene& g, bool nativeOrder) {
    for (RawGene::Value& v : g.v) {
        if (nativeOrder) {  // a, b, c, input, mode
            v.a = r.f32(), v.b = r.f32(), v.c = r.f32();
            v.input = r.i16(), v.mode = r.i16();
        } else {  // input, mode, a, b, c
            v.input = r.i16(), v.mode = r.i16();
            v.a = r.f32(), v.b = r.f32(), v.c = r.f32();
        }
        v.input = clampv(v.input, 0, 7);  // Cell Lab: 0..3 S1..S4, 4..7 mass, age, nitrogen, links
    }
    for (int& i : g.ints) i = r.i32();
    for (float& f : g.fl) f = r.f32();
}

// `geneSize` > 315: extensions of modded formats, skipped. `headOnly`: the rest of the gene is in an unknown layout.
RawGene readFileGene(Reader& r, int geneSize, bool headOnly) {
    size_t start = r.p;
    RawGene g;
    readGeneHead(r, g);
    if (headOnly) g.hasTail = false;
    else readGeneTail(r, g, false);
    size_t used = r.p - start;
    if (used < (size_t)geneSize) r.skip((size_t)geneSize - used);
    return g;
}

// MinLab's native record (CLSB substrates): 307 bytes.
RawGene readNativeGene(Reader& r) {
    RawGene g;
    for (float& c : g.color) c = r.f32();
    g.splitMass = r.f32();
    g.splitRatio = r.f32();
    g.splitAngle = r.f32();
    g.childAngle[0] = r.f32();
    g.childAngle[1] = r.f32();
    g.priority = r.f32();
    g.stiffness = r.f32();
    g.child[0] = r.i32();
    g.child[1] = r.i32();
    g.makeAdhesin = r.b8();
    g.keep[0] = r.b8();
    g.keep[1] = r.b8();
    g.prioritize = r.b8();
    g.initial = r.b8();
    g.mirror[0] = r.b8();
    g.mirror[1] = r.b8();
    g.type = r.i32();
    readGeneTail(r, g, true);
    return g;
}

void writeGeneTail(Writer& w, const RawGene& g, bool nativeOrder) {
    for (const RawGene::Value& v : g.v) {
        if (nativeOrder) {
            w.f32(v.a), w.f32(v.b), w.f32(v.c);
            w.i16((int16_t)v.input), w.i16((int16_t)v.mode);
        } else {
            w.i16((int16_t)v.input), w.i16((int16_t)v.mode);
            w.f32(v.a), w.f32(v.b), w.f32(v.c);
        }
    }
    for (int i : g.ints) w.i32(i);
    for (float f : g.fl) w.f32(f);
}

void writeFileGene(Writer& w, const RawGene& g, int version) {
    w.i32(version);
    for (float c : g.color) w.f32(c);
    w.f32(g.splitMass);
    w.f32(g.splitRatio);
    w.f32(g.splitAngle);
    w.f32(g.childAngle[0]);
    w.f32(g.childAngle[1]);
    w.f32(g.priority);
    w.i32(g.child[0]);
    w.i32(g.child[1]);
    w.b8(g.makeAdhesin);
    w.b8(g.keep[0]);
    w.b8(g.keep[1]);
    w.i32(g.type);
    w.i32(0);  // "lucky number"
    w.b8(g.prioritize);
    w.b8(g.initial);
    w.b8(g.mirror[0]);
    w.b8(g.mirror[1]);
    w.f32(g.stiffness);
    writeGeneTail(w, g, false);
}

const int kValueField[kNumValues] = {PF_Swim, PF_Density, PF_Bend, PF_Contraction, PF_Lift, PF_Stem1, PF_Stem2,
                                     PF_Neuro1, PF_Neuro2, PF_Neuro3, PF_Neuro4};

// ---------------------------------------------------------------------------- Cell Lab: Macro genes
// Macro (com.massivemistakes.hydralab) writes file versions 3057+ with a mode count; each gene grows by version
// (fields are appended to their group, never reordered). Read with the *file* version, as the game does.
struct MacroLoss {
    int signals = 0;  // inputs, secretions or channels on signals past S8 (Macro has 100)
    int types = 0;    // Macro-only cell types replaced by the closest of ours
};

int macroGeneSize(int fv) {
    const int nK = fv >= 3089 ? 15 : fv >= 3081 ? 14 : fv >= 3056 ? 13 : 11;
    const int nL = fv >= 3056 ? 16 : 11;
    const int nM = fv >= 3077 ? 21 : fv == 3076 ? 19 : fv >= 3067 ? 18 : fv >= 3056 ? 8 : 7;
    int gate = 0;
    if (fv >= 3062) gate = 1 + (fv >= 3063) + 12 + 12 + (fv >= 3064 ? 26 : 0) + (fv >= 3065 ? 4 : 0) + (fv >= 3074 ? 8 : 0) + (fv >= 3080);
    return 63 + (fv >= 3066) + (fv >= 3068) + (fv >= 3078 ? 9 : 0) + (fv >= 3079 ? 4 : 0) + (fv >= 3080) + (fv >= 3090) + (fv >= 3091) +
           (fv >= 3087 ? 6 : 0) + 4 + 16 * nK + 4 * nL + 4 * nM + (fv >= 3059 ? 4 : 0) + 6 * gate + (fv >= 3086 ? 8 : 0) +
           (fv >= 3093 ? 4 : 0) + (fv >= 3099 ? 4 : 0) + (fv >= 3088);
}

// Macro's own cell types (18..25) as the closest of ours.
int macroType(int t, MacroLoss& loss) {
    if (t < 18) return t;
    ++loss.types;
    switch (t) {
        case 18: return (int)CellType::Keratinocyte;  // osteocyte (bone)
        case 19: return (int)CellType::Lipocyte;      // adipocyte
        case 20: return (int)CellType::Neurocyte;     // axon
        case 21: return (int)CellType::Neurocyte;     // gangliocyte (logic)
        case 22: return (int)CellType::Senseocyte;    // scopocyte (eye with three lanes)
        case 23: return (int)CellType::Grappler;      // rhizocyte (grips)
        case 24: return (int)CellType::Stemocyte;     // progenocyte
        case 25: return (int)CellType::Gamete;        // zygocyte
        default: return (int)CellType::Phagocyte;
    }
}

// Signal slot of Macro (0..99) as our channel code: 0..3 S1..S4, 8..11 S5..S8 (program-input codes), -1 beyond S8.
int macroSignalInput(int s, MacroLoss& loss) {
    if (s >= 0 && s < 4) return s;
    if (s >= 4 && s < 8) return 8 + (s - 4);
    ++loss.signals;
    return -1;
}

RawGene readMacroGene(Reader& r, int fv, MacroLoss& loss) {
    RawGene g;
    readGeneHead(r, g, false);
    r.skip((fv >= 3066) + (fv >= 3068) + (fv >= 3078 ? 9 : 0) + (fv >= 3079 ? 4 : 0) + (fv >= 3080) + (fv >= 3090) + (fv >= 3091) +
           (fv >= 3087 ? 6 : 0));  // axon, signal gate, on-contact, split lineage, sensor and stem flags
    g.stiffness = r.f32();  // 0..35
    const int nK = fv >= 3089 ? 15 : fv >= 3081 ? 14 : fv >= 3056 ? 13 : 11;
    const int nL = fv >= 3056 ? 16 : 11;
    const int nM = fv >= 3077 ? 21 : fv == 3076 ? 19 : fv >= 3067 ? 18 : fv >= 3056 ? 8 : 7;
    const int nSignals = fv >= 3097 ? 100 : 16;  // signal slots, then mass, age, nitrogen, links, random
    for (int k = 0; k < nK; ++k) {
        int input = r.i16(), mode = r.i16();
        float a = r.f32(), b = r.f32(), c = r.f32();
        if (k >= kNumValues) continue;  // neuron outputs 5..6, axon, rhizocyte: no equivalent
        RawGene::Value& v = g.v[k];
        v.mode = clampv(mode, 0, 3);
        v.a = a, v.b = b, v.c = c;
        if (input < nSignals) v.input = v.mode ? macroSignalInput(input, loss) : 0;
        else if (input < nSignals + 4) v.input = 4 + (input - nSignals);  // mass, age, nitrogen, links
        else v.input = -1;                                                // per-cell random: none
    }
    int L[16] = {};
    for (int k = 0; k < nL; ++k) L[k] = r.i32();
    for (int k = 0; k < kNumInts; ++k) g.ints[k] = L[k];
    auto channel = [&](int s) {
        int code = macroSignalInput(s, loss);
        return code < 0 ? 0 : code < 4 ? code : 4 + (code - 8);  // channel 0..7
    };
    g.ints[2] = channel(L[2]);
    for (int k = 5; k <= 8; ++k) g.ints[k] = channel(L[k]);
    {  // secretion: 0..6 substances, then "remove signal i" and "produce signal i"
        const int s = L[4];
        const int remove0 = 7, produce0 = 7 + nSignals;
        if (s >= remove0 && s < produce0) {
            int ch = channel(s - remove0);
            g.ints[4] = ch < 4 ? 7 + ch : 15 + (ch - 4);  // -S1..-S4, -S5..-S8
        } else if (s >= produce0) {
            int ch = channel(s - produce0);
            g.ints[4] = ch < 4 ? 11 + ch : 19 + (ch - 4);  // +S1..+S4, +S5..+S8
        }
    }
    float M[21] = {};
    for (int k = 0; k < nM; ++k) M[k] = r.f32();
    for (int k = 0; k < kNumFloats; ++k) g.fl[k] = M[k];
    if (nM <= 6) g.fl[6] = 1.0f;
    g.type = macroType(g.type, loss);
    // Macro numbers the daughters the other way round: its child 1 (offset 40) is Cell Lab's (and our) child 2.
    std::swap(g.child[0], g.child[1]);
    std::swap(g.childAngle[0], g.childAngle[1]);
    std::swap(g.keep[0], g.keep[1]);
    std::swap(g.mirror[0], g.mirror[1]);
    return g;
}

// Secretion int: 0 food smell, 1 cyanide, 2 lipase, 3 protease, 4 coated-food smell, 5 light activator,
// 6 wall activator, 7..10 = -S1..-S4, 11..14 = +S1..+S4; read from Macro also 15..18 = -S5..-S8, 19..22 = +S5..+S8.
const Secretion kSecretionBase[7] = {Secretion::FoodSmell, Secretion::HCN, Secretion::Lipase, Secretion::Protease,
                                     Secretion::CoatedFoodSmell, Secretion::LightActivator, Secretion::WallActivator};

Mode toMode(const RawGene& g, int& unknownTypes) {
    Mode m;
    m.color = {clampv(g.color[0], 0.f, 1.f), clampv(g.color[1], 0.f, 1.f), clampv(g.color[2], 0.f, 1.f)};
    // Split-mass slider: below the first snap = "split at age", above the last = "never" (3.672 ng).
    float splitM = g.splitMass * 10.0f;  // stored in units of 10 ng
    if (splitM < kSplitMassMin - 0.06f) m.splitMass = kSplitAtAge;
    else if (splitM > kSplitMassMax + 0.06f && splitM < 3.9f) m.splitMass = kNeverSplit;
    else if (splitM >= 3.9f) m.splitMass = kSplitMassMax;  // mods allow far bigger cells: split at our largest
    else m.splitMass = splitM;
    // The stored ratio is the share of the *new* cell, which is child 2; ours is child 1's share.
    m.splitRatio = clampv(1.0f - g.splitRatio, 0.1f, 0.9f);
    // The game puts child 1 on the far side of the split direction (we put it on the near side) with the same
    // headings: turning the split by half a turn and the children back by half a turn gives the same organism.
    m.splitAngle = wrapAngle(g.splitAngle + kPi);
    m.childAngle[0] = wrapAngle(g.childAngle[0] - kPi);
    m.childAngle[1] = wrapAngle(g.childAngle[1] - kPi);
    m.nutrientPriority = clampv(g.priority, 0.1f, 10.0f);
    m.childMode[0] = (uint8_t)clampv(g.child[0], 0, kMaxModes - 1);
    m.childMode[1] = (uint8_t)clampv(g.child[1], 0, kMaxModes - 1);
    m.makeAdhesin = g.makeAdhesin;
    m.keepAdhesin[0] = g.keep[0];
    m.keepAdhesin[1] = g.keep[1];
    m.type = mapType(g.type, unknownTypes);
    m.prioritize = g.prioritize;
    m.mirror[0] = g.mirror[0];
    m.mirror[1] = g.mirror[1];
    // Stiffness is stored 0..35 since mode version 16. The ZIP genomes of the newer modded game are not documented:
    // keep the old guess for them.
    m.adhesinStiffness = clampv(g.hasTail || g.stiffness > 1.0f ? g.stiffness / 35.0f : g.stiffness, 0.0f, 1.0f);
    if (!g.hasTail) return m;

    // Programmable values. a, b and the step levels are in normalised output units [-1, 1]; c is a threshold in
    // input units. Our scale/offset per field equal (max-min)/2 and (max+min)/2 of the game's ranges.
    // Input codes: 0..3 S1..S4 and 4..7 mass, age, nitrogen, links (Cell Lab); 8..11 S5..S8 (from Macro); -1 none.
    // Modes: 0 fixed, 1 linear, 2 step, 3 wave a*sin(c*x - b) (Macro).
    static const ProgInput kInput[12] = {ProgInput::S1,   ProgInput::S2,  ProgInput::S3,       ProgInput::S4,
                                         ProgInput::Mass, ProgInput::Age, ProgInput::Nitrogen, ProgInput::Connections,
                                         ProgInput::S5,   ProgInput::S6,  ProgInput::S7,       ProgInput::S8};
    for (int k = 0; k < kNumValues; ++k) {
        const RawGene::Value& v = g.v[k];
        const int field = kValueField[k];
        const ProgRange& pr = progRange(field);
        Prog& p = m.prog[field];
        const float a = clampv(v.a, -8.0f, 8.0f), b = clampv(v.b, -8.0f, 8.0f), c = clampv(v.c, -8.0f, 8.0f);
        if (v.mode <= 0 || v.mode > 3 || v.input < 0 || v.input >= 12) {
            p.input = ProgInput::Fixed;
            p.kind = ProgKind::Linear;
            p.a = 0;
            p.b = (v.mode == 1 ? b : v.mode == 2 ? a : v.mode == 3 ? 0.0f : b) * pr.scale + pr.offset;  // an unknown input reads 0
            p.c = 0;
            continue;
        }
        p.input = kInput[v.input];
        if (v.mode == 1) {
            p.kind = ProgKind::Linear;
            p.a = a * pr.scale;
            p.b = b * pr.scale + pr.offset;
            p.c = c;
        } else if (v.mode == 2) {  // step: below c -> a, from c on -> b (both output levels)
            p.kind = ProgKind::Step;
            p.a = a * pr.scale + pr.offset;
            p.b = b * pr.scale + pr.offset;
            p.c = c;
        } else {  // wave: same amplitude and period around the middle of the range (the phase is not kept)
            p.kind = ProgKind::Wave;
            p.a = a * pr.scale;
            p.b = pr.offset;
            p.c = std::fabs(c) > 1e-4f ? 6.2831853f / std::fabs(c) : 1.0f;
        }
    }
    const int* ints = g.ints;
    m.virusCopyFrom = (uint8_t)clampv(ints[0], 0, kMaxModes - 1);
    // ints[0]: virus source / stem path 1 / gamete mode after fertilisation; ints[1]: stem path 2 / compatible gamete mode.
    m.stemMode[0] = m.gameteModeAfter = (uint8_t)clampv(ints[0], 0, kMaxModes - 1);
    m.stemMode[1] = m.gameteCompatibleMode = (uint8_t)clampv(ints[1], 0, kMaxModes - 1);
    m.senseChannel = (uint8_t)clampv(ints[2], 0, kSignalCount - 1);  // Cell Lab: 0..3; Macro: up to S8 kept
    m.senseTarget = (SenseTarget)clampv(ints[3], 0, (int)SenseTarget::Wall);  // 0..5 = cell, food, coated, light, velocity, wall
    {
        int s = ints[4];
        if (s >= 0 && s < 7) m.secretion = kSecretionBase[s];
        else if (s >= 7 && s < 11) m.secretion = (Secretion)((int)Secretion::MinusS1 + (s - 7));
        else if (s >= 11 && s < 15) m.secretion = (Secretion)((int)Secretion::PlusS1 + (s - 11));
        else if (s >= 15 && s < 19) m.secretion = (Secretion)((int)Secretion::MinusS5 + (s - 15));  // S5..S8 (from Macro)
        else if (s >= 19 && s < 23) m.secretion = (Secretion)((int)Secretion::PlusS5 + (s - 19));
    }
    for (int k = 0; k < 4; ++k) m.neuroChannel[k] = (uint8_t)clampv(ints[5 + k], 0, kSignalCount - 1);
    m.maxConnections = clampv(ints[9] + 1, 1, kMaxLinks);
    m.gameteTelomeres = (uint8_t)clampv(ints[10], 0, 5);
    const float* fl = g.fl;
    m.senseOutput = clampv(fl[0], -60.0f, 60.0f);
    m.senseColor = {clampv(fl[1], 0.f, 1.f), clampv(fl[2], 0.f, 1.f), clampv(fl[3], 0.f, 1.f)};
    m.senseThreshold = clampv(fl[4], 0.0f, 1.7320508f);
    m.adhesinLength = clampv(fl[5] * 100.0f, 0.0f, 1.0f);
    m.cytoskeleton = fl[6] > 0.0f ? clampv(fl[6], 0.5f, 2.0f) : 1.0f;
    return m;
}

// What did not fit while converting our modes to the Cell Lab family's.
struct ExportLoss {
    int ownTypes = 0, senses = 0, programs = 0, inputs = 0, channels = 0;  // channels: S5..S8 folded onto S1..S4
};

// `ref(mode index)`: the exported index of a referenced mode.
template <class Ref>
RawGene fromMode(const Mode& m, bool initial, int self, Ref ref, ExportLoss* loss) {
    RawGene g;
    g.color[0] = m.color.r, g.color[1] = m.color.g, g.color[2] = m.color.b;
    float sm = m.splitMass <= kSplitAtAge ? kSplitMassMin - kSplitMassStep
               : m.splitMass >= kNeverSplit ? kSplitMassMax + kSplitMassStep
                                            : m.splitMass;
    g.splitMass = sm / 10.0f;
    g.splitRatio = 1.0f - m.splitRatio;  // the game stores child 2's share
    g.splitAngle = wrapAngle(m.splitAngle - kPi);  // child 1 on the far side in the game (see toMode)
    g.childAngle[0] = wrapAngle(m.childAngle[0] + kPi);
    g.childAngle[1] = wrapAngle(m.childAngle[1] + kPi);
    g.priority = m.nutrientPriority;
    g.child[0] = ref(m.childMode[0]);
    g.child[1] = ref(m.childMode[1]);
    g.makeAdhesin = m.makeAdhesin;
    g.keep[0] = m.keepAdhesin[0];
    g.keep[1] = m.keepAdhesin[1];
    // CellFap-only types have no Cell Lab counterpart: the closest original one. (A type past 17 crashes the game.)
    CellType t = m.type;
    int ct = t == CellType::Electrocyte ? (int)CellType::Neurocyte : t == CellType::Cnidocyte ? (int)CellType::Devorocyte
             : t == CellType::Chromatocyte || t == CellType::Luminocyte ? (int)CellType::Phagocyte
             : t == CellType::Cystocyte ? (int)CellType::Keratinocyte : t == CellType::Grappler ? (int)CellType::Glueocyte
                                                                                       : (int)t;
    if (ct >= kCellLabTypes || ct < 0) ct = (int)CellType::Phagocyte;
    if (loss && ct != (int)t) ++loss->ownTypes;
    g.type = ct;
    g.prioritize = m.prioritize;
    g.initial = initial;
    g.mirror[0] = m.mirror[0];
    g.mirror[1] = m.mirror[1];
    g.stiffness = clampv(m.adhesinStiffness, 0.0f, 1.0f) * 35.0f;
    for (int k = 0; k < kNumValues; ++k) {
        const Prog& p = m.prog[kValueField[k]];
        const ProgRange& pr = progRange(kValueField[k]);
        RawGene::Value& v = g.v[k];
        auto level = [&](float x) { return clampv((x - pr.offset) / pr.scale, -8.0f, 8.0f); };
        int in = -1;  // the game's input index
        if (inputSignal(p.input) >= 0) {
            in = inputSignal(p.input) & 3;  // S5..S8 fold onto S1..S4
            if (loss && inputSignal(p.input) >= 4) ++loss->channels;
        }
        if (p.input >= ProgInput::Mass && p.input <= ProgInput::Connections) in = 4 + ((int)p.input - (int)ProgInput::Mass);
        if (p.input == ProgInput::Fixed || in < 0) {
            if (loss && p.input != ProgInput::Fixed) ++loss->inputs;  // light, food, injury: no such input
            v.input = 0, v.mode = 0;
            v.b = level(p.b);
            continue;
        }
        v.input = in;
        if (p.kind == ProgKind::Step) {
            v.mode = 2;
            v.a = level(p.a);
            v.b = level(p.b);
            v.c = p.c;
        } else {  // linear (a wave has no equivalent: its offset and amplitude as a line)
            if (loss && p.kind != ProgKind::Linear) ++loss->programs;
            v.mode = 1;
            v.a = clampv(p.a / pr.scale, -8.0f, 8.0f);
            v.b = level(p.b);
            v.c = 0;
        }
    }
    int sec = 0;
    switch (m.secretion) {
        case Secretion::FoodSmell: sec = 0; break;
        case Secretion::HCN: sec = 1; break;
        case Secretion::Lipase: sec = 2; break;
        case Secretion::Protease: sec = 3; break;
        case Secretion::CoatedFoodSmell: sec = 4; break;
        case Secretion::LightActivator: sec = 5; break;
        case Secretion::WallActivator: sec = 6; break;
        default: {
            float sign = 1;
            const int s8 = secretionSignal(m.secretion, &sign);
            const int ch = s8 & 3;  // S5..S8 fold onto S1..S4
            if (loss && m.type == CellType::Secrocyte && s8 > 3) ++loss->channels;
            sec = sign < 0 ? 7 + ch : 11 + ch;
        }
    }
    const bool stem = m.type == CellType::Stemocyte, gam = m.type == CellType::Gamete;
    g.ints[0] = ref(stem ? m.stemMode[0] : gam ? m.gameteModeAfter : m.virusCopyFrom);
    g.ints[1] = stem ? ref(m.stemMode[1]) : gam ? ref(m.gameteCompatibleMode) : self;
    g.ints[2] = m.senseChannel & 3;  // S1..S4 only
    const bool senses = m.type == CellType::Senseocyte || m.type == CellType::Stereocyte;
    if (loss && senses && m.senseChannel > 3) ++loss->channels;
    if (m.senseTarget > SenseTarget::Wall) {
        if (loss && senses) ++loss->senses;
        g.ints[3] = 0;
    } else {
        g.ints[3] = (int)m.senseTarget;
    }
    g.ints[4] = sec;
    for (int c = 0; c < 4; ++c) g.ints[5 + c] = m.neuroChannel[c] & 3;
    if (loss && m.type == CellType::Neurocyte)
        for (int c = 0; c < 4; ++c) loss->channels += m.neuroChannel[c] > 3;
    g.ints[9] = clampv(m.maxConnections, 1, 20) - 1;
    g.ints[10] = clampv((int)m.gameteTelomeres, 0, 5);
    g.fl[0] = m.senseOutput;
    g.fl[1] = m.senseColor.r;
    g.fl[2] = m.senseColor.g;
    g.fl[3] = m.senseColor.b;
    g.fl[4] = m.senseThreshold;
    g.fl[5] = m.adhesinLength / 100.0f;
    g.fl[6] = m.cytoskeleton;
    return g;
}

// Genome of up to 40 modes from raw genes: the first gene flagged initial is the initial mode (as the game does).
Genome genomeFromRaw(const std::vector<RawGene>& raw, int& unknownTypes) {
    Genome g;
    const int count = std::min((int)raw.size(), kMaxModes);
    for (int k = 0; k < count; ++k) g.modes.set(k, toMode(raw[k], unknownTypes));
    // References beyond what we keep fall back to mode 1.
    for (int k = 0; k < count; ++k) {
        Mode& m = g.modes.edit(k);
        for (auto* idx : {&m.childMode[0], &m.childMode[1], &m.virusCopyFrom, &m.stemMode[0], &m.stemMode[1],
                          &m.gameteCompatibleMode, &m.gameteModeAfter})
            if (*idx >= count) *idx = 0;
    }
    g.initialMode = 0;
    for (int k = 0; k < count; ++k)
        if (raw[k].initial) {
            g.initialMode = k;
            break;
        }
    return g;
}

// Modes the organism actually uses (reachable from the initial mode), numbered from 0 in the order they appear,
// so a genome that uses M90 or M121 still fits into the game's 40 as long as it needs <= 40.
struct ModeOrder {
    std::vector<int> order;
    int remap[kMaxModes];
    explicit ModeOrder(const Genome& g, const std::vector<int>& alsoUsed = {}) {
        std::fill(std::begin(remap), std::end(remap), -1);
        auto visit = [&](int idx) {
            if (idx < 0 || idx >= kMaxModes || remap[idx] >= 0) return;
            remap[idx] = (int)order.size();
            order.push_back(idx);
        };
        auto expand = [&](size_t from) {
            for (size_t q = from; q < order.size(); ++q) {
                const Mode& m = g.modes[order[q]];
                visit(m.childMode[0]);
                visit(m.childMode[1]);
                if (m.type == CellType::Stemocyte || m.type == CellType::Cystocyte) visit(m.stemMode[0]), visit(m.stemMode[1]);
                if (m.type == CellType::Gamete) visit(m.gameteCompatibleMode), visit(m.gameteModeAfter);
                if (m.type == CellType::Virocyte) visit(m.virusCopyFrom);
            }
        };
        visit(g.initialMode);
        expand(0);
        // Modes cells of a dish are in right now, though the organism no longer reaches them from its initial mode.
        for (int idx : alsoUsed) {
            size_t from = order.size();
            visit(idx);
            expand(from);
        }
    }
    int operator()(int idx) const { return idx >= 0 && idx < kMaxModes && remap[idx] >= 0 && remap[idx] < 40 ? remap[idx] : 0; }
    // The 40 genes of the game, initial first; unused slots are default modes.
    std::vector<RawGene> genes(const Genome& g, ExportLoss* loss) const {
        std::vector<RawGene> out;
        for (int k = 0; k < 40; ++k) {
            if (k < (int)order.size()) {
                out.push_back(fromMode(g.modes[order[k]], k == 0, k, *this, loss));
            } else {
                static const Mode kSpare{};
                RawGene sp = fromMode(kSpare, false, k, [&](int) { return k; }, nullptr);
                out.push_back(sp);
            }
        }
        return out;
    }
};

// ---------------------------------------------------------------------------- substrate, common part
// Cells, links and food as read from either substrate format, before they are placed into the world.
struct ImpCell {
    double x = 0, y = 0, angle = 0, mass = 1, age = 0, nitrogen = 0;
    bool dead = false, mirrored = false;
    int mode = 0, tag = 1, mutations = 0, telomeres = -1;
    float injury = 0;
    std::string key;  // the raw genome bytes (de-duplication)
    std::vector<RawGene> genes;
};
struct ImpLink {
    int a, b;
    float length, stiffness;  // µm extra length, 0..1
    float angA, angB;         // anchor angles on a and on b (rad, in each cell's frame)
};
struct ImpFood {
    float x, y, mass, coating;
};

// Settings Cell Lab does not have are set so the dish behaves as it does there.
Settings cellLabSettings() {
    Settings s;
    s.swimAnisotropy = 1.0f;    // no undulatory swimming
    s.mechanicalDamage = 0.0f;  // cells are not crushed or torn
    s.sapLeak = 0.0f;           // no sap clouds
    s.lightCapacity = 1.0f;     // no self-shading of algae
    s.dayLength = 0.0f;         // eternal day
    for (int t = kCellLabTypes; t < kGenePoolSlots; ++t) s.genePool[t] = false;  // contamination: Cell Lab types only
    return s;
}

void buildWorld(World& w, Settings s, double age, const std::vector<ImpCell>& cells, const std::vector<ImpLink>& links,
                const std::vector<ImpFood>& food, CellLabImportInfo& info) {
    const float mm = kUmPerUnit;  // one length unit of the game (it calls it mm) in µm
    // The dish is sized to contain every cell and food piece rather than trusting the diameter field alone
    // (depending on the version it behaves like a radius).
    double maxR = 0;
    for (const ImpCell& c : cells) maxR = std::max(maxR, std::hypot(c.x, c.y));
    for (const ImpFood& f : food) maxR = std::max(maxR, (double)std::hypot(f.x, f.y));
    float need = (float)(maxR * mm * 1.02 + 20.0);
    if (need > s.dishRadius) s.dishRadius = std::min(need, 12000.0f);
    s.maxCells = std::max(s.maxCells, (int)cells.size() + 100);
    w.reset(s);
    CellLabAccess::setTime(w, age);
    const float R = s.dishRadius;
    std::vector<int> worldIdx(cells.size(), -1);
    std::unordered_map<std::string, int> genomeCache;
    int unknownTypes = 0;
    for (size_t ci = 0; ci < cells.size(); ++ci) {
        const ImpCell& ic = cells[ci];
        if (ic.dead) {
            info.skippedCells++;
            continue;
        }
        const int count = std::min((int)ic.genes.size(), kMaxModes);
        const int mode = clampv(ic.mode, 0, std::max(count - 1, 0));
        Vec2 pos{(float)ic.x * mm, (float)ic.y * mm};
        float pl = length(pos);
        if (pl > R * 0.999f) pos = pos * (R * 0.999f / std::max(pl, 1e-3f));
        int idx;
        auto it = genomeCache.find(ic.key);
        if (it != genomeCache.end() && it->second >= 0 && w.cells()[it->second].alive) {
            idx = w.addCellLike(it->second, pos, (float)ic.angle, mode, (float)ic.mass, ic.mirrored);
        } else {
            Genome g = genomeFromRaw(ic.genes, unknownTypes);
            g.name = "Импорт";
            static const CellTag tags[7] = {CellTag::Challenge, CellTag::User, CellTag::Contamination, CellTag::InfectedChallenge,
                                            CellTag::InfectedUser, CellTag::InfectedContamination, CellTag::Hybrid};
            idx = w.addCell(g, pos, (float)ic.angle, mode, tags[clampv(ic.tag, 0, 6)], (float)std::max(ic.mass, 0.1));
            if (idx >= 0) {
                genomeCache[ic.key] = idx;
                info.genomes++;
            }
        }
        if (idx < 0) continue;
        Cell& c = CellLabAccess::cell(w, idx);
        c.mirrored = ic.mirrored;
        c.age = (float)ic.age;
        c.nitrogen = clampv((float)ic.nitrogen, 0.0f, 1.0f);
        c.injury = clampv(ic.injury, 0.0f, 1.0f);
        c.mutations = (uint16_t)std::max(ic.mutations, 0);
        c.telomeres = (int8_t)(ic.telomeres > 0 ? std::min(ic.telomeres, 127) : -1);
        worldIdx[ci] = idx;
        info.cells++;
    }
    for (const ImpLink& l : links)
        if (l.a >= 0 && l.b >= 0 && l.a < (int)cells.size() && l.b < (int)cells.size() && worldIdx[l.a] >= 0 && worldIdx[l.b] >= 0)
            CellLabAccess::link(w, worldIdx[l.a], worldIdx[l.b], clampv(l.length, 0.0f, 10.0f), clampv(l.stiffness, 0.0f, 1.0f), l.angA, l.angB);
    for (const ImpFood& f : food) {
        w.addFood({f.x * mm, f.y * mm}, std::max(f.mass * kFoodUnit, 0.05f), clampv(f.coating, 0.0f, 1.0f));
        info.food++;
    }
    info.unknownTypes = unknownTypes;
    w.rebuildGrid();
}

constexpr uint32_t kClsb = 0x434C5342u;  // "CLSB": MinLab's own substrate stream
constexpr uint32_t kClen = 0x434C454Eu;  // "CLEN": its environment block
constexpr int kClsbCellBytes = 13795, kClsbLinkSlots = 20, kClsbGenes = 40, kNativeGeneBytes = 307;
constexpr int kCellLabMaxCells = 5000, kCellLabMaxFood = 10000;  // caps for exported dishes (see the export)

// MinLab's CLSB substrate (big-endian, no Java framing).
void readClsb(const std::vector<uint8_t>& raw, World& w, CellLabImportInfo& info) {
    Reader r{raw};
    r.u32();  // magic
    if (r.i32() != 1) throw Fail{"Неизвестная версия субстрата MinLab"};
    info.version = r.i32();
    double age = r.f64();
    int cellCount = r.i32();
    if (cellCount < 0 || cellCount > 2000000) throw Fail{"Неверное число клеток: " + std::to_string(cellCount)};
    int envLen = r.i32();
    if (envLen < 222 || envLen > 1 << 16) throw Fail{"Повреждённые настройки чашки"};
    r.need((size_t)envLen);
    Settings s = cellLabSettings();
    {
        std::vector<uint8_t> envBytes(raw.begin() + (long)r.p, raw.begin() + (long)(r.p + envLen));
        Reader e{envBytes};
        if (e.u32() != kClen) throw Fail{"Повреждённые настройки чашки"};
        e.i32();  // version
        e.b8();   // preview environment
        s.lightAngle = wrapAngle((float)e.f64());
        e.f64(), e.f64();  // cos, sin of it
        s.nutrientRate = clampv((float)e.f64(), 0.0f, 15.0f);
        s.chunkSize = clampv((float)e.f64(), 0.022f, 1.152f);
        s.radiation = clampv((float)e.f64(), 0.0f, 0.09f);
        s.lightAmount = clampv((float)e.f64(), 0.0f, 2.0f);
        s.lightDirChange = clampv((float)e.f64(), 0.0f, 1.0f);
        s.lightRange = clampv((float)e.f64(), 0.0f, 1.0f);
        s.viscosity = clampv((float)e.f64(), 0.0f, 150.0f);
        s.density = clampv((float)e.f64(), -3.3f, 3.3f);
        s.gravity = clampv((float)e.f64(), 0.0f, 1.0f);
        s.densityGradient = clampv((float)e.f64(), -2.0f, 0.0f);
        s.dishRadius = clampv((float)(e.f64() * kUmPerUnit), 100.0f, 12000.0f);  // called the diameter in the game, but the radius
        e.f64(), e.f64(), e.f64();  // MinLab's test coefficients (no equivalent)
        s.staticFriction = clampv((float)e.f64(), 0.0f, 8.0f);
        s.dynFriction = clampv((float)e.f64(), 0.0f, 8.0f);
        s.salinity = clampv(e.f32(), 0.0f, 1.0f);
        s.coating = clampv(e.f32(), 0.0f, 1.0f);
        s.lumpiness = clampv((float)e.f64(), 0.0f, 100.0f);
        s.lumpSize = clampv((float)e.f64(), 0.01f, 0.2f);
        s.nitrates = clampv((float)e.f64(), 0.0f, 1.0f);
        s.maxCells = clampv(e.i32(), 100, 100000);
        s.maxFood = clampv(e.i32(), 0, 100000);
        e.i32();  // grid size
        s.onlyPointMutations = e.b8();
        s.cellAging = e.b8();
        s.contaminate = e.b8();
        s.killAtEdge = e.b8();
        s.mobileFood = e.b8();
        e.skip(8);  // environment seed
        int n = e.i32();
        if (n < 0 || n > 64) throw Fail{"Повреждённые настройки чашки"};
        for (int t = 0; t < n; ++t) {
            bool on = e.b8();
            if (t < kCellLabTypes) s.genePool[t] = on;
        }
    }
    r.skip((size_t)envLen);
    s.lightAngle = wrapAngle((float)r.f64());
    r.skip(8);  // seed
    int n2 = r.i32();
    if (n2 != cellCount) throw Fail{"Повреждённый субстрат: число клеток не совпадает"};
    std::vector<ImpCell> cells((size_t)cellCount);
    std::vector<ImpLink> links;
    for (int ci = 0; ci < cellCount; ++ci) {
        r.need(kClsbCellBytes);
        const size_t c0 = r.p;
        ImpCell& c = cells[(size_t)ci];
        auto at = [&](size_t off) {
            Reader q{raw};
            q.p = c0 + off;
            return q;
        };
        {
            Reader q = at(0);
            c.x = q.f64();
            c.y = q.f64();
        }
        c.angle = at(80).f64();
        c.age = at(112).f64();
        c.nitrogen = at(120).f64();
        c.mass = at(160).f64() * 10.0;  // units of 10 ng
        {
            Reader q = at(168);
            c.dead = q.b8();
            c.mirrored = q.b8();
        }
        int linkCount = clampv(at(175).i32(), 0, kClsbLinkSlots);
        c.mode = at(179).i32();
        c.tag = at(183).i32();
        c.mutations = at(191).i32();
        c.telomeres = at(207).i32();
        for (int k = 0; k < linkCount; ++k) {
            Reader q = at(227 + (size_t)k * 60);
            int other = q.i32();
            const double d1 = q.f64(), d2 = q.f64();
            q.skip(16);  // wall anchor (links to the wall are not imported)
            float stiff = q.f32();
            float len = q.f32();
            if (other >= 0 && other < cellCount && ci < other) links.push_back({ci, other, len * 1000.0f, stiff / 35.0f, (float)d1, (float)d2});  // angular stiffness 0..35
        }
        {
            Reader q = at(1427);
            c.genes.reserve(kClsbGenes);
            for (int k = 0; k < kClsbGenes; ++k) c.genes.push_back(readNativeGene(q));
            c.key.assign(reinterpret_cast<const char*>(raw.data() + c0 + 1427), (size_t)kClsbGenes * kNativeGeneBytes);
        }
        c.injury = at(13775).f32();
        r.p = c0 + kClsbCellBytes;
    }
    int nfood = r.i32();
    if (nfood < 0 || nfood > 1000000) throw Fail{"Неверное число частиц пищи"};
    std::vector<ImpFood> food;
    food.reserve((size_t)nfood);
    for (int k = 0; k < nfood; ++k) {
        ImpFood f;
        f.x = r.f32(), f.y = r.f32(), f.mass = r.f32();
        r.f32(), r.f32();
        f.coating = r.f32();
        food.push_back(f);
    }
    info.note = "MinLab";
    buildWorld(w, s, age, cells, links, food, info);
}

}  // namespace

// ============================================================================ genome
bool importCellLabGenome(Genome& g, const std::string& path, std::string& error) {
    try {
        std::vector<uint8_t> raw;
        if (!readFile(path, raw)) throw Fail{"Не удалось открыть файл"};
        std::vector<std::string> labels;
        if (raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K' && raw[2] == 3 && raw[3] == 4) {
            // A newer modded game: a ZIP with save.bin (the genome stream) and mode_names.json.
            std::vector<uint8_t> names, bin;
            if (unzipEntry(raw, "mode_names.json", names)) labels = parseModeNames(names);
            if (!unzipEntry(raw, "save.bin", bin)) throw Fail{"В архиве нет save.bin — это не геном Cell Lab"};
            raw.swap(bin);
        }
        if (!hasMagic(raw)) throw Fail{"Это не файл Cell Lab (нет сигнатуры Java)"};
        std::vector<uint8_t> data;
        javaBlocks(raw, 4, data);
        Reader r{data};
        int version = r.i32();
        if (version < kVersionOriginal) throw Fail{"Слишком старая версия генома: " + std::to_string(version)};
        // Formats: 95 (Cell Lab), 96 (MinLab; same record), 3055 (the 127-mode mod) are a flat list of genes;
        // Cell Lab: Macro (3057 and later) writes the number of modes first and longer genes (3107 and older are
        // known field by field; 3118 appends 28 bytes; later ones are read with the 3107 layout if it fits).
        const bool macro = version >= 3057;
        int count, geneSize;
        bool headOnly = false;
        if (macro) {
            count = r.i32();
            if (count <= 0 || count > 100000) throw Fail{"Неверное число мод: " + std::to_string(count)};
            const size_t genesAt = 8;
            switch (version) {
                case 3118: geneSize = 918; break;
                case 3147: geneSize = 1145; break;
                case 3148: geneSize = 1146; break;
                default:
                    if (version <= 3107) {
                        geneSize = macroGeneSize(version);
                        break;
                    }
                    // Unknown newer version: the gene length is the distance to the next gene's version number.
                    geneSize = 0;
                    for (size_t p = genesAt + 68; p + 4 <= data.size() && count > 1; ++p)
                        if (((uint32_t)data[p] << 24 | (uint32_t)data[p + 1] << 16 | (uint32_t)data[p + 2] << 8 | data[p + 3]) ==
                            (uint32_t)version) {
                            geneSize = (int)(p - genesAt);
                            break;
                        }
                    if (geneSize < 68) throw Fail{"Неизвестная версия генома Cell Lab: " + std::to_string(version)};
            }
            if (genesAt + (size_t)count * geneSize > data.size()) throw Fail{"Файл генома обрезан"};
            headOnly = geneSize < macroGeneSize(std::min(version, 3107));
        } else {
            geneSize = geneSizeFor(version);
            count = (int)((data.size() - 4) / geneSize);
        }
        const int total = count;
        count = std::min(count, kMaxModes);
        if (count <= 0) throw Fail{"Пустой геном"};
        const size_t genesStart = r.p;
        MacroLoss ml;
        std::vector<RawGene> genes;
        auto readMacroAll = [&](bool head) {
            genes.clear();
            ml = {};
            r.p = genesStart;
            for (int k = 0; k < count; ++k) {
                const size_t st = r.p;
                RawGene g;
                if (head) {  // what is known for sure: the shared head (stiffness and the rest keep their defaults)
                    readGeneHead(r, g, false);
                    g.hasTail = false;
                    g.type = macroType(g.type, ml);
                    std::swap(g.child[0], g.child[1]);
                    std::swap(g.childAngle[0], g.childAngle[1]);
                    std::swap(g.keep[0], g.keep[1]);
                    std::swap(g.mirror[0], g.mirror[1]);
                } else {
                    g = readMacroGene(r, version, ml);
                }
                genes.push_back(g);
                r.p = st + (size_t)geneSize;
            }
        };
        if (macro) {
            readMacroAll(headOnly);
            if (!headOnly && version > 3118) {
                // A layout not seen in the game's code: keep it only if every gene looks sane.
                bool sane = true;
                for (const RawGene& g : genes) {
                    sane = sane && g.stiffness >= 0 && g.stiffness <= 35.5f && g.priority >= 0.09f && g.priority <= 10.5f;
                    for (float c : g.color) sane = sane && c >= 0 && c <= 1;
                    sane = sane && g.fl[6] >= 0.49f && g.fl[6] <= 2.01f;  // cytoskeleton
                }
                if (!sane) readMacroAll(headOnly = true);
            }
        } else {
            for (int k = 0; k < count; ++k) genes.push_back(readFileGene(r, geneSize, false));
        }
        int unknown = 0;
        Genome out = genomeFromRaw(genes, unknown);
        for (int k = 0; k < count && k < (int)labels.size(); ++k) {
            std::string l = labels[k];
            for (char& ch : l)
                if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
            if (!l.empty()) out.labels.set(k, l);
        }
        std::string name = path;
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
        size_t dot = name.rfind(".genome");
        if (dot != std::string::npos) name = name.substr(0, dot);
        out.name = name;
        g = out;
        error.clear();
        if (total > kMaxModes) error = "мод больше 128: взяты первые 128";
        if (unknown) error += std::string(error.empty() ? "" : "; ") + "незнакомых типов клеток: " + std::to_string(unknown);
        auto add = [&](const std::string& s) { error += (error.empty() ? "" : "; ") + s; };
        if (headOnly) add("программы и сенсоры этой версии не перенесены");
        if (ml.types) add("типов Macro заменено ближайшими: " + std::to_string(ml.types));
        if (ml.signals) add("сигналов дальше S8 отброшено: " + std::to_string(ml.signals));
        return true;
    } catch (const Fail& f) {
        error = f.msg;
        return false;
    } catch (const std::exception&) {
        error = "Файл повреждён";
        return false;
    }
}

// ============================================================================ substrate
bool importCellLabSubstrate(World& w, const std::string& path, CellLabImportInfo& info, std::string& error) {
    try {
        std::vector<uint8_t> raw;
        if (!readFile(path, raw)) throw Fail{"Не удалось открыть файл"};
        if (raw.size() >= 4 && Reader{raw}.u32() == kClsb) {
            readClsb(raw, w, info);
            return true;
        }
        if (raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K') {  // a Cell Lab: Macro bundle (save.bin + thumbnail)
            std::vector<uint8_t> bin;
            if (!unzipEntry(raw, "save.bin", bin)) throw Fail{"В архиве нет save.bin — это не чашка Cell Lab"};
            raw.swap(bin);
        }
        if (!hasMagic(raw)) throw Fail{"Это не чашка Cell Lab или MinLab"};
        std::vector<uint8_t> header;
        size_t gz = javaBlocks(raw, 4, header);
        if (gz + 2 > raw.size() || raw[gz] != 0x1F || raw[gz + 1] != 0x8B) throw Fail{"Не найден сжатый блок клеток"};

        // ---- environment header
        Reader h{header};
        int version = h.i32();
        if (version < kVersionOriginal) throw Fail{"Слишком старая версия субстрата: " + std::to_string(version)};
        if (version >= 3057)  // Macro: extra dish settings, longer links and cells — not supported yet
            throw Fail{"Чашки Cell Lab: Macro пока не поддерживаются (геномы из неё открываются)"};
        info.version = version;
        double age = h.f64();
        int cellCount = h.i32();
        if (cellCount < 0 || cellCount > 2000000) throw Fail{"Неверное число клеток: " + std::to_string(cellCount)};
        h.i32();  // environment version
        Settings s = cellLabSettings();
        s.nutrientRate = clampv((float)h.f64(), 0.0f, 15.0f);
        s.chunkSize = clampv((float)h.f64(), 0.022f, 1.152f);
        s.radiation = clampv((float)h.f64(), 0.0f, 0.09f);
        s.lightAmount = clampv((float)h.f64(), 0.0f, 2.0f);
        s.lightDirChange = clampv((float)h.f64(), 0.0f, 1.0f);
        s.lightRange = clampv((float)h.f64(), 0.0f, 1.0f);
        s.viscosity = clampv((float)h.f64(), 0.0f, 150.0f);
        int ntypes = h.i32();
        if (ntypes < 0 || ntypes > 64) throw Fail{"Неверное число типов клеток"};
        for (int t = 0; t < ntypes; ++t) {
            bool on = h.b8();
            if (t < kCellLabTypes) s.genePool[t] = on;
        }
        s.contaminate = h.b8();
        s.gravity = clampv((float)h.f64(), 0.0f, 1.0f);
        s.density = clampv((float)h.f64(), -3.3f, 3.3f);
        s.densityGradient = clampv((float)h.f64(), -2.0f, 0.0f);
        s.killAtEdge = h.b8();
        s.nitrates = clampv((float)h.f64(), 0.0f, 1.0f);
        s.maxCells = clampv(h.i32(), 100, 100000);
        s.maxFood = clampv(h.i32(), 0, 100000);
        s.dishRadius = clampv((float)(h.f64() * kUmPerUnit), 100.0f, 12000.0f);  // called the diameter in the game, but the radius
        s.dynFriction = clampv((float)h.f64(), 0.0f, 8.0f);
        s.staticFriction = clampv((float)h.f64(), 0.0f, 8.0f);
        s.onlyPointMutations = h.b8();
        s.salinity = clampv(h.f32(), 0.0f, 1.0f);
        s.cellAging = h.b8();
        s.lumpiness = clampv((float)h.f64(), 0.0f, 100.0f);
        s.lumpSize = clampv((float)h.f64(), 0.01f, 0.2f);
        s.mobileFood = h.b8();
        s.coating = clampv(h.f32(), 0.0f, 1.0f);

        // ---- cells + food (gzip'ed Java stream)
        std::vector<uint8_t> unz = gunzip(raw.data() + gz, raw.size() - gz);
        std::vector<uint8_t> body;
        if (hasMagic(unz)) javaBlocks(unz, 4, body);
        else body.swap(unz);
        Reader r{body};
        s.lightAngle = wrapAngle((float)r.f64());
        if (version == 96) r.skip(8);  // MinLab: random seed

        std::vector<ImpCell> cells((size_t)cellCount);
        std::vector<ImpLink> links;
        for (int ci = 0; ci < cellCount; ++ci) {
            ImpCell& c = cells[(size_t)ci];
            int cver = r.i32();
            if (cver != version && cver < kVersionOriginal) throw Fail{"Неизвестная версия клетки: " + std::to_string(cver)};
            c.x = r.f64(), c.y = r.f64(), c.angle = r.f64();
            r.f64();                    // unknown
            r.f64(), r.f64(), r.f64();  // velocity x, y, angular
            r.f64();                    // unknown
            r.f64();                    // diameter (recomputed from mass)
            c.mass = r.f64() * 10.0;    // units of 10 ng
            c.age = r.f64();
            int slots = r.i32();
            if (slots < 0 || slots > 64) throw Fail{"Повреждённый список связей"};
            std::vector<ImpLink> slotLinks;
            for (int k = 0; k < slots; ++k) {
                int other = r.i32();
                const double d1 = r.f64(), d2 = r.f64();
                r.b8();
                r.f64(), r.f64();
                float stiff = r.f32();
                float len = r.f32();
                slotLinks.push_back({ci, other, len * 1000.0f, stiff / 35.0f, (float)d1, (float)d2});  // angular stiffness 0..35
            }
            const int used = r.i32();  // link count: only the first `used` slots are links
            for (int k = 0; k < (int)slotLinks.size() && k < used; ++k)
                if (slotLinks[k].b >= 0 && slotLinks[k].b < cellCount && ci < slotLinks[k].b) links.push_back(slotLinks[k]);
            c.dead = r.b8();
            r.f32(), r.f32(), r.f32();  // display colour
            int ngenes = r.i32();
            if (ngenes <= 0 || ngenes > 4096) throw Fail{"Неверное число мод в клетке"};
            const int geneSize = geneSizeFor(cver);
            const size_t gstart = r.p;
            r.need((size_t)ngenes * geneSize);
            c.key.assign(reinterpret_cast<const char*>(body.data() + gstart), (size_t)ngenes * geneSize);
            const int count = std::min(ngenes, kMaxModes);
            for (int k = 0; k < count; ++k) c.genes.push_back(readFileGene(r, geneSize, false));
            r.p = gstart + (size_t)ngenes * geneSize;
            // Cell tail
            c.mode = r.i32();
            c.tag = r.i32();
            r.i32();
            r.f64(), r.f64(), r.f64();
            c.nitrogen = r.f64();
            c.mirrored = r.b8();
            if (cver != kVersionMod) {  // 95 and 96
                for (int k = 0; k < 8; ++k) r.f32();  // signals
                r.f32();                               // toxins
                c.injury = r.f32();
                for (int k = 0; k < 3; ++k) r.f32();
                r.f32();  // lipids
                c.mutations = r.i32();
                c.telomeres = r.i32();
                r.f64();  // lift
            } else {
                r.skip(177 - 45);
            }
        }
        int nfood = r.i32();
        if (nfood < 0 || nfood > 1000000) throw Fail{"Неверное число частиц пищи"};
        std::vector<ImpFood> food;
        food.reserve((size_t)nfood);
        for (int k = 0; k < nfood; ++k) {
            ImpFood f;
            f.x = r.f32(), f.y = r.f32(), f.mass = r.f32();
            r.f32(), r.f32();
            f.coating = r.f32();
            food.push_back(f);
        }
        if (version == kVersionMod) info.note = "мод: 127 мод, 19 типов";
        buildWorld(w, s, age, cells, links, food, info);
        return true;
    } catch (const Fail& f) {
        error = f.msg;
        return false;
    } catch (const std::exception&) {  // a malformed file must never take the game down
        error = "Файл повреждён";
        return false;
    }
}

// ============================================================================ export (format 95)
bool exportCellLabGenome(const Genome& g, const std::string& path, std::string& error) {
    const ModeOrder order(g);
    const int used = (int)order.order.size();
    ExportLoss loss;
    Writer d;
    d.i32(kVersionOriginal);  // 95: read by Cell Lab and by MinLab (which writes 96, same record)
    for (const RawGene& gene : order.genes(g, &loss)) writeFileGene(d, gene, kVersionOriginal);
    // Java ObjectOutputStream: magic + one long data block.
    std::vector<uint8_t> out = {0xAC, 0xED, 0x00, 0x05, 0x7A};
    uint32_t n = (uint32_t)d.d.size();
    for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(n >> s));
    out.insert(out.end(), d.d.begin(), d.d.end());
    FILE* f = openFileUtf8(path, "wb");
    if (!f) {
        error = "Не удалось создать файл";
        return false;
    }
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        error = "Ошибка записи";
        return false;
    }
    // What Cell Lab cannot hold, told plainly (the export itself succeeded).
    error.clear();
    auto add = [&](const std::string& s) { error += (error.empty() ? "" : "; ") + s; };
    if (used > 40) add("используется " + std::to_string(used) + " мод, в Cell Lab помещается 40 — остальные потеряны");
    if (loss.ownTypes) add("типов CellFap заменено ближайшими: " + std::to_string(loss.ownTypes));
    if (loss.senses) add("сенсоров без аналога в Cell Lab: " + std::to_string(loss.senses));
    if (loss.programs) add("волновые программы стали линейными: " + std::to_string(loss.programs));
    if (loss.inputs) add("входов без аналога (свет, еда, травма) заменено постоянными: " + std::to_string(loss.inputs));
    if (loss.channels) add("сигналов S5–S8 сведено к S1–S4: " + std::to_string(loss.channels));
    return true;
}

// ============================================================================ substrate export (format 95)
// Java ObjectOutputStream block data: 1024-byte TC_BLOCKDATALONG records and a short last one, as Java writes them.
static void appendJavaBlocks(std::vector<uint8_t>& out, const std::vector<uint8_t>& payload) {
    size_t p = 0;
    while (p < payload.size()) {
        size_t n = std::min<size_t>(1024, payload.size() - p);
        if (n <= 255 && p + n == payload.size()) {
            out.push_back(0x77);
            out.push_back((uint8_t)n);
        } else {
            out.push_back(0x7A);
            for (int s = 24; s >= 0; s -= 8) out.push_back((uint8_t)(n >> s));
        }
        out.insert(out.end(), payload.begin() + (long)p, payload.begin() + (long)(p + n));
        p += n;
    }
}

static bool gzipInto(std::vector<uint8_t>& out, const std::vector<uint8_t>& in) {
    z_stream zs{};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = (uInt)in.size();
    std::vector<uint8_t> buf(1 << 16);
    int r;
    do {
        zs.next_out = buf.data();
        zs.avail_out = (uInt)buf.size();
        r = deflate(&zs, Z_FINISH);
        if (r == Z_STREAM_ERROR) {
            deflateEnd(&zs);
            return false;
        }
        out.insert(out.end(), buf.data(), buf.data() + (buf.size() - zs.avail_out));
    } while (r != Z_STREAM_END);
    deflateEnd(&zs);
    return true;
}

bool exportCellLabSubstrate(const World& w, const std::string& path, CellLabExportInfo& info, std::string& error) {
    const Settings& s = w.settings;
    const float mmPerUm = 1.0f / kUmPerUnit;  // our µm -> the game's length unit
    info = {};
    // Cells that go into the file (the alive ones), in file order.
    std::vector<int> cellsOut;
    std::vector<int> fileIdx(w.cells().size(), -1);
    for (size_t i = 0; i < w.cells().size(); ++i)
        if (w.cells()[i].alive) {
            fileIdx[i] = (int)cellsOut.size();
            cellsOut.push_back((int)i);
        }
    // The modes each genome's cells are in (they go into the 40 too, even when not reachable from the initial mode).
    std::unordered_map<int, std::vector<int>> usedModes;
    for (int ci : cellsOut) {
        const Cell& c = w.cells()[ci];
        std::vector<int>& v = usedModes[c.genome];
        if (std::find(v.begin(), v.end(), (int)c.mode) == v.end()) v.push_back(c.mode);
    }
    // One conversion per genome: its 40 modes and the mode renumbering.
    struct GenomeOut {
        ModeOrder order;
        std::vector<RawGene> genes;
    };
    std::unordered_map<int, GenomeOut> genomes;
    ExportLoss loss;
    auto genomeOut = [&](int id) -> const GenomeOut& {
        auto it = genomes.find(id);
        if (it != genomes.end()) return it->second;
        const Genome& g = w.genomes().get(id);
        GenomeOut go{ModeOrder(g, usedModes[id]), {}};
        go.genes = go.order.genes(g, &loss);
        if (go.order.order.size() > 40) ++info.genomesCut;
        return genomes.emplace(id, std::move(go)).first->second;
    };

    // ---- header block (environment)
    Writer h;
    h.i32(kVersionOriginal);
    h.f64(w.timeHours());
    h.i32((int)cellsOut.size());
    h.i32(kVersionOriginal);  // environment version
    h.f64(s.nutrientRate);
    h.f64(clampv(s.chunkSize, 0.0216f, 1.152f));
    h.f64(s.radiation);
    h.f64(s.lightAmount);
    h.f64(s.lightDirChange);
    h.f64(s.lightRange);
    h.f64(s.viscosity);
    h.i32(kCellLabTypes);
    for (int t = 0; t < kCellLabTypes; ++t) h.b8(s.genePool[t]);
    h.b8(s.contaminate);
    h.f64(s.gravity);
    h.f64(s.density);
    h.f64(s.densityGradient);
    h.b8(s.killAtEdge);
    h.f64(s.nitrates);
    const int foodCount = (int)w.food().size();
    // The game reserves memory for every possible cell (about 14 KB each with its 40 genes): our default of 20 000
    // cells cannot be loaded on a phone ("error reading substrate"). Its own dishes use ~1000 cells / 3500 food.
    // Not less than what the dish holds: the game refuses more cells / food than these.
    h.i32(std::max(std::min(s.maxCells, kCellLabMaxCells), (int)cellsOut.size()));
    h.i32(std::max(std::min(s.maxFood, kCellLabMaxFood), foodCount));
    h.f64(s.dishRadius / kUmPerUnit);  // called the diameter in the game, but it is the radius
    h.f64(s.dynFriction);
    h.f64(s.staticFriction);
    h.b8(s.onlyPointMutations);
    h.f32(s.salinity);
    h.b8(s.cellAging);
    h.f64(s.lumpiness);
    h.f64(s.lumpSize);
    h.b8(s.mobileFood);
    h.f32(s.coating);

    // ---- cells and food (a gzip'ed Java stream)
    Writer b;
    b.f64(s.lightAngle);
    static const CellTag kTags[7] = {CellTag::Challenge, CellTag::User, CellTag::Contamination, CellTag::InfectedChallenge,
                                     CellTag::InfectedUser, CellTag::InfectedContamination, CellTag::Hybrid};
    for (int ci : cellsOut) {
        const Cell& c = w.cells()[ci];
        const GenomeOut& go = genomeOut(c.genome);
        // Mass in units of 10 ng; the game kills cells outside 0.72..3.6 ng, and the diameter must match the mass.
        const double mass = clampv(c.mass / 10.0f, 0.075f, 0.355f);
        b.i32(kVersionOriginal);
        b.f64(c.pos.x * mmPerUm);
        b.f64(c.pos.y * mmPerUm);
        b.f64(c.angle);
        b.f64(0);                      // second (visual) angle
        b.f64(0), b.f64(0), b.f64(0);  // velocities: the dish starts at rest
        b.f64(0);
        b.f64(std::sqrt(mass / 400.0));  // diameter, mm
        b.f64(mass);
        b.f64(c.age);
        // 20 link slots, the used ones first; both cells of a pair hold mirrored slots.
        b.i32(kClsbLinkSlots);
        int used = 0;
        for (int k = 0; k < c.linkCount && used < kClsbLinkSlots; ++k) {
            const Link& l = w.links()[c.links[k]];
            if (!l.alive) continue;
            const bool isA = l.a == ci;
            const int other = l.b < 0 ? -1 : (isA ? fileIdx[l.b] : fileIdx[l.a]);
            if (l.b >= 0 && other < 0) continue;
            b.i32(other);
            b.f64(isA ? l.angA : l.angB);  // anchor on this cell
            b.f64(isA ? l.angB : l.angA);  // anchor on the partner
            b.b8(true);
            b.f64(l.b < 0 ? l.wallPoint.x * mmPerUm : 0.0);  // wall anchor (links to the wall only)
            b.f64(l.b < 0 ? l.wallPoint.y * mmPerUm : 0.0);
            b.f32(clampv(l.stiffness, 0.0f, 1.0f) * 35.0f);
            b.f32(clampv(l.extraLength, 0.0f, 10.0f) / 1000.0f);  // the game adds it at both ends: half per end, ×2 for the unit
            ++used;
        }
        for (int k = used; k < kClsbLinkSlots; ++k) {
            b.i32(-1);
            b.f64(0), b.f64(0);
            b.b8(true);
            b.f64(0), b.f64(0);
            b.f32(0), b.f32(0);
        }
        b.i32(used);
        b.b8(false);  // dead
        const int mode = go.order(c.mode);
        const RawGene& active = go.genes[(size_t)mode];
        b.f32(active.color[0]), b.f32(active.color[1]), b.f32(active.color[2]);  // displayed colour
        b.i32(40);
        for (const RawGene& g : go.genes) writeFileGene(b, g, kVersionOriginal);
        b.i32(mode);
        int tag = 1;
        for (int t = 0; t < 7; ++t)
            if (kTags[t] == c.tag) tag = t;
        b.i32(tag);
        b.i32(0);  // microscope selection tag (above 99 the cell is frozen)
        b.f64(0), b.f64(0), b.f64(0);
        b.f64(clampv(c.nitrogen, 0.0f, 1.0f));
        b.b8(c.mirrored);
        for (int k = 0; k < 4; ++k) b.f32(clampv(c.signals[k], -1.0f, 1.0f)), b.f32(0);
        b.f32(0);  // toxins
        b.f32(clampv(c.injury, 0.0f, 0.99f));
        b.f32(0), b.f32(0), b.f32(0);
        b.f32(0);  // lipids
        b.i32(c.mutations);
        b.i32(c.telomeres < 0 ? -1 : (int)c.telomeres);
        b.f64(0);  // lift
        info.cells++;
    }
    b.i32(foodCount);
    for (const Food& f : w.food()) {
        b.f32(f.pos.x * mmPerUm);
        b.f32(f.pos.y * mmPerUm);
        b.f32(f.mass / kFoodUnit);
        b.f32(0), b.f32(0);
        b.f32(clampv(f.coating, 0.0f, 1.0f));
    }
    info.food = foodCount;
    info.genomes = (int)genomes.size();
    info.ownTypes = loss.ownTypes;
    info.otherLosses = loss.senses + loss.programs + loss.inputs + loss.channels;

    std::vector<uint8_t> out = {0xAC, 0xED, 0x00, 0x05};
    appendJavaBlocks(out, h.d);
    std::vector<uint8_t> inner = {0xAC, 0xED, 0x00, 0x05};
    appendJavaBlocks(inner, b.d);
    if (!gzipInto(out, inner)) {
        error = "Ошибка сжатия";
        return false;
    }
    FILE* f = openFileUtf8(path, "wb");
    if (!f) {
        error = "Не удалось создать файл";
        return false;
    }
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        error = "Ошибка записи";
        return false;
    }
    error.clear();
    return true;
}
