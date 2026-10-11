#include "cellab_import.hpp"

#include "serialize.hpp"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr int kVersionOriginal = 95;
constexpr int kVersionMod = 3055;
constexpr int kNumValues = 11, kNumInts = 11, kNumFloats = 7;
constexpr int kGeneCore = 67 + kNumValues * 16 + kNumInts * 4 + kNumFloats * 4;  // 315 bytes (format 95)

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

int geneSizeFor(int version) { return version == kVersionMod ? 331 : kGeneCore; }

// Cell Lab's 18 cell types have the same indices as ours. Anything else (the mod's extra type, a newer game's)
// has no equivalent: it becomes a phagocyte and is counted, never one of CellFap's own new types.
constexpr int kCellLabTypes = 18;
CellType mapType(int t, int& unknown) {
    if (t >= 0 && t < kCellLabTypes) return (CellType)t;
    ++unknown;
    return CellType::Phagocyte;
}
// `modern`: genes of format 3118+ (a ZIP; the files seen so far come from a modded game with up to 300 modes, cell
// types numbered past 17 and split masses up to ~90 ng). Their first 67 bytes are laid out as in format 95; the rest
// (programs, senses) is a different, undocumented layout, so it is skipped and those settings keep their defaults.
Mode readMode(Reader& r, int geneSize, bool modern, int& unknownTypes) {
    size_t start = r.p;
    Mode m;
    int ver = r.i32();
    (void)ver;
    m.color = {r.f32(), r.f32(), r.f32()};
    float splitM = r.f32() * 10.0f;  // stored in units of 10 ng (every format)
    m.splitRatio = clampv(r.f32(), 0.1f, 0.9f);
    m.splitAngle = wrapAngle(r.f32());
    m.childAngle[0] = wrapAngle(r.f32());
    m.childAngle[1] = wrapAngle(r.f32());
    m.nutrientPriority = clampv(r.f32(), 0.1f, 10.0f);
    int c1 = r.i32(), c2 = r.i32();
    m.childMode[0] = (uint8_t)clampv(c1, 0, kMaxModes - 1);
    m.childMode[1] = (uint8_t)clampv(c2, 0, kMaxModes - 1);
    m.makeAdhesin = r.b8();
    m.keepAdhesin[0] = r.b8();
    m.keepAdhesin[1] = r.b8();
    m.type = mapType(r.i32(), unknownTypes);
    r.i32();  // "lucky number"
    m.prioritize = r.b8();
    bool initial = r.b8();
    m.mirror[0] = r.b8();
    m.mirror[1] = r.b8();
    float stiff = r.f32();
    m.adhesinStiffness = clampv(stiff > 1.0f ? stiff / 35.0f : stiff, 0.0f, 1.0f);
    // Split-mass slider: below the first snap = "split at age", above the last = "never".
    if (splitM < kSplitMassMin - 0.06f) m.splitMass = kSplitAtAge;
    else if (splitM > kSplitMassMax + 0.06f && splitM < 3.9f) m.splitMass = kNeverSplit;  // the slider's "never" (3.7)
    else if (splitM >= 3.9f) m.splitMass = kSplitMassMax;  // mods allow far bigger cells: split at our largest
    else m.splitMass = splitM;
    if (modern) {  // the rest of a modern gene is not decoded (see above)
        size_t used = r.p - start;
        if (used < (size_t)geneSize) r.skip((size_t)geneSize - used);
        (void)initial;
        return m;
    }

    // Programmable values: (input index, input type) + a, b, c in normalised units.
    static const int fieldOf[kNumValues] = {PF_Swim, PF_Density, PF_Bend, PF_Contraction, PF_Lift, PF_Stem1, PF_Stem2,
                                            PF_Neuro1, PF_Neuro2, PF_Neuro3, PF_Neuro4};
    for (int v = 0; v < kNumValues; ++v) {
        int idx = r.i16(), type = r.i16();
        float a = r.f32(), b = r.f32(), c = r.f32();
        int field = fieldOf[v];
        const ProgRange& pr = progRange(field);
        Prog& p = m.prog[field];
        p.kind = ProgKind::Linear;
        p.a = a * pr.scale;
        p.b = b * pr.scale + pr.offset;
        p.c = c;
        if (type == 1) p.input = (ProgInput)((int)ProgInput::S1 + clampv(idx, 0, 3));
        else if (type == 2) p.input = (ProgInput)((int)ProgInput::Mass + clampv(idx, 0, 3));
        else p.input = ProgInput::Fixed;
    }
    int ints[kNumInts];
    for (int& v : ints) v = r.i32();
    // ints: virus from, virus to (also stem / gamete mode slots), smell channel, smell type, secretion,
    // neuro channels x4, max connections - 1, telomere choice.
    m.virusCopyFrom = (uint8_t)clampv(ints[0], 0, kMaxModes - 1);
    m.stemMode[0] = m.gameteCompatibleMode = (uint8_t)clampv(ints[0], 0, kMaxModes - 1);
    m.stemMode[1] = m.gameteModeAfter = (uint8_t)clampv(ints[1], 0, kMaxModes - 1);
    m.senseChannel = (uint8_t)clampv(ints[2], 0, 3);
    m.senseTarget = (SenseTarget)clampv(ints[3], 0, (int)SenseTarget::Count - 1);
    {
        static const Secretion base[7] = {Secretion::FoodSmell, Secretion::HCN, Secretion::Lipase, Secretion::Protease,
                                          Secretion::CoatedFoodSmell, Secretion::LightActivator, Secretion::WallActivator};
        int s = ints[4];
        if (s >= 0 && s < 7) m.secretion = base[s];
        else if (s >= 7 && s < 11) m.secretion = (Secretion)((int)Secretion::PlusS1 + (s - 7));
        else if (s >= 11 && s < 15) m.secretion = (Secretion)((int)Secretion::MinusS1 + (s - 11));
    }
    for (int k = 0; k < 4; ++k) m.neuroChannel[k] = (uint8_t)clampv(ints[5 + k], 0, 3);
    m.maxConnections = clampv(ints[9] + 1, 1, kMaxLinks);
    m.gameteTelomeres = (uint8_t)clampv(ints[10], 0, 5);
    float fl[kNumFloats];
    for (float& v : fl) v = r.f32();
    m.senseOutput = clampv(fl[0], -60.0f, 60.0f);
    m.senseColor = {clampv(fl[1], 0.f, 1.f), clampv(fl[2], 0.f, 1.f), clampv(fl[3], 0.f, 1.f)};
    m.senseThreshold = clampv(fl[4], 0.0f, 1.7320508f);
    m.adhesinLength = clampv(fl[5] * 100.0f, 0.0f, 1.0f);
    m.cytoskeleton = fl[6] > 0.0f ? clampv(fl[6], 0.5f, 2.0f) : 1.0f;
    // Skip extensions of newer / modded formats (e.g. 4 extra words per mode in format 3055).
    size_t used = r.p - start;
    size_t total = (size_t)geneSize;
    if (used < total) r.skip(total - used);
    (void)initial;  // the initial mode is located by readInitialFlagModeIndex
    return m;
}

int readInitialFlagModeIndex(const std::vector<uint8_t>& d, size_t geneStart, int geneSize, int count) {
    // "isInitial" is the 2nd of the four booleans after the type ints: offset 4+36+8+3+8+1.
    for (int k = 0; k < count; ++k) {
        size_t off = geneStart + (size_t)k * geneSize + 60;
        if (off < d.size() && d[off]) return k;
    }
    return 0;
}


}  // namespace

// ============================================================================ genome
bool importCellLabGenome(Genome& g, const std::string& path, std::string& error) {
    try {
        std::vector<uint8_t> raw;
        if (!readFile(path, raw)) throw Fail{"Не удалось открыть файл"};
        std::vector<std::string> labels;
        if (raw.size() >= 4 && raw[0] == 'P' && raw[1] == 'K' && raw[2] == 3 && raw[3] == 4) {
            // The current game: a ZIP with save.bin (the genome stream) and mode_names.json.
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
        // Formats: 95 (original) and 3055 (the 127-mode mod) are a flat list of genes; the current game (3118 and
        // later) writes the number of modes first and longer genes, each starting with the version number.
        const bool modern = version >= 3100;
        int count, geneSize;
        size_t genesAt = 4;
        if (modern) {
            count = r.i32();
            if (count <= 0 || count > 100000) throw Fail{"Неверное число мод: " + std::to_string(count)};
            genesAt = 8;
            switch (version) {
                case 3118: geneSize = 918; break;
                case 3147: geneSize = 1145; break;
                case 3148: geneSize = 1146; break;
                default: {
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
            }
            if (genesAt + (size_t)count * geneSize > data.size()) throw Fail{"Файл генома обрезан"};
        } else {
            geneSize = geneSizeFor(version);
            count = (int)((data.size() - 4) / geneSize);
        }
        const int total = count;
        count = std::min(count, kMaxModes);
        if (count <= 0) throw Fail{"Пустой геном"};
        int unknown = 0;
        Genome out;
        for (int k = 0; k < count; ++k) out.modes.set(k, readMode(r, geneSize, modern, unknown));
        // Child and other mode references beyond what we keep fall back to mode 1.
        for (int k = 0; k < count; ++k) {
            Mode& m = out.modes.edit(k);
            for (auto* idx : {&m.childMode[0], &m.childMode[1], &m.virusCopyFrom, &m.stemMode[0], &m.stemMode[1],
                              &m.gameteCompatibleMode, &m.gameteModeAfter})
                if (*idx >= count) *idx = 0;
        }
        out.initialMode = std::min(readInitialFlagModeIndex(data, genesAt, geneSize, count), count - 1);
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
        if (modern) error += std::string(error.empty() ? "" : "; ") + "программы и сенсоры новой версии не перенесены";
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
        if (!hasMagic(raw)) throw Fail{"Это не файл Cell Lab (нет сигнатуры Java)"};
        std::vector<uint8_t> header;
        size_t gz = javaBlocks(raw, 4, header);
        if (gz + 2 > raw.size() || raw[gz] != 0x1F || raw[gz + 1] != 0x8B) throw Fail{"Не найден сжатый блок клеток"};

        // ---- environment header
        Reader h{header};
        int version = h.i32();
        if (version < kVersionOriginal) throw Fail{"Слишком старая версия субстрата: " + std::to_string(version)};
        info.version = version;
        double age = h.f64();
        int cellCount = h.i32();
        if (cellCount < 0 || cellCount > 2000000) throw Fail{"Неверное число клеток: " + std::to_string(cellCount)};
        h.i32();  // environment version
        Settings s;
        // Settings Cell Lab does not have are set so the dish behaves as it does there.
        s.swimAnisotropy = 1.0f;    // no undulatory swimming
        s.mechanicalDamage = 0.0f;  // cells are not crushed or torn
        s.sapLeak = 0.0f;           // no sap clouds
        s.lightCapacity = 1.0f;     // no self-shading of algae
        s.dayLength = 0.0f;         // eternal day
        for (int t = kCellLabTypes; t < kGenePoolSlots; ++t) s.genePool[t] = false;  // contamination: Cell Lab types only
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
            if (t < kCellTypeCount) s.genePool[t] = on;
        }
        s.contaminate = h.b8();
        s.gravity = clampv((float)h.f64(), 0.0f, 1.0f);
        s.density = clampv((float)h.f64(), -3.3f, 3.3f);
        s.densityGradient = clampv((float)h.f64(), -2.0f, 0.0f);
        s.killAtEdge = h.b8();
        s.nitrates = clampv((float)h.f64(), 0.0f, 1.0f);
        s.maxCells = clampv(h.i32(), 100, 100000);
        s.maxFood = clampv(h.i32(), 0, 100000);
        double diameterMm = h.f64();
        s.dishRadius = clampv((float)(diameterMm * 500.0), 100.0f, 12000.0f);
        s.dynFriction = clampv((float)h.f64(), 0.0f, 8.0f);
        s.staticFriction = clampv((float)h.f64(), 0.0f, 8.0f);
        s.onlyPointMutations = h.b8();
        s.salinity = clampv(h.f32(), 0.0f, 1.0f);
        s.cellAging = h.b8();
        s.lumpiness = clampv((float)h.f64(), 0.0f, 100.0f);
        s.lumpSize = clampv((float)h.f64(), 0.01f, 0.2f);
        s.mobileFood = h.b8();
        s.coating = clampv(h.f32(), 0.0f, 1.0f);
        s.maxCells = std::max(s.maxCells, cellCount + 100);

        // ---- cells + food (gzip'ed Java stream)
        std::vector<uint8_t> unz = gunzip(raw.data() + gz, raw.size() - gz);
        std::vector<uint8_t> body;
        if (hasMagic(unz)) javaBlocks(unz, 4, body);
        else body.swap(unz);
        Reader r{body};
        s.lightAngle = wrapAngle((float)r.f64());

        const float mm = 1000.0f;  // file positions are in millimetres
        // Pre-scan positions: depending on the version the "diameter" field behaves like a radius, so the
        // dish is sized to contain every cell and food piece rather than trusting the field alone.
        {
            Reader q{body};
            q.p = r.p;
            double maxR = 0;
            for (int ci = 0; ci < cellCount; ++ci) {
                int cver = q.i32();
                double x = q.f64(), y = q.f64();
                q.skip(9 * 8);
                int slots = q.i32();
                if (slots < 0 || slots > 64) throw Fail{"Повреждённый список связей"};
                q.skip((size_t)slots * 45 + 4 + 1 + 12);
                int ngenes = q.i32();
                if (ngenes <= 0 || ngenes > 4096) throw Fail{"Неверное число мод в клетке"};
                q.skip((size_t)ngenes * geneSizeFor(cver) + (cver == kVersionMod ? 177 : 117));
                maxR = std::max(maxR, std::hypot(x, y));
            }
            int nfood = q.i32();
            for (int k = 0; k < nfood && k < 1000000; ++k) {
                float fx = q.f32(), fy = q.f32();
                q.skip(16);
                maxR = std::max(maxR, (double)std::hypot(fx, fy));
            }
            float need = (float)(maxR * mm * 1.02 + 20.0);
            if (need > s.dishRadius) s.dishRadius = std::min(need, 12000.0f);
        }
        w.reset(s);
        w.time_ = age;
        w.nextStatTime_ = age;
        const float R = s.dishRadius;
        std::vector<int> worldIdx(cellCount, -1);
        struct PendingLink {
            int a, b;
            float length, stiffness;
        };
        std::vector<PendingLink> links;
        std::unordered_map<std::string, int> genomeCache;
        int unknownTypes = 0;

        for (int ci = 0; ci < cellCount; ++ci) {
            int cver = r.i32();
            if (cver != version && cver < kVersionOriginal) throw Fail{"Неизвестная версия клетки: " + std::to_string(cver)};
            double x = r.f64(), y = r.f64(), angle = r.f64();
            r.f64();                      // unknown
            r.f64(); r.f64(); r.f64();    // velocity x, y, angular
            r.f64();                      // unknown
            r.f64();                      // diameter (recomputed from mass)
            double mass = r.f64() * 10.0; // units of 10 ng
            double cage = r.f64();
            int slots = r.i32();
            if (slots < 0 || slots > 64) throw Fail{"Повреждённый список связей"};
            for (int k = 0; k < slots; ++k) {
                int other = r.i32();
                r.f64();
                r.f64();
                r.b8();
                r.f64();
                r.f64();
                float stiff = r.f32();
                float length = r.f32();
                if (other >= 0 && other < cellCount && ci < other)
                    links.push_back({ci, other, length * mm, stiff > 1.0f ? stiff / 35.0f : stiff});
            }
            r.i32();  // link count
            bool dead = r.b8();
            r.f32(); r.f32(); r.f32();  // display colour
            int ngenes = r.i32();
            if (ngenes <= 0 || ngenes > 4096) throw Fail{"Неверное число мод в клетке"};
            int geneSize = geneSizeFor(cver);
            size_t gstart = r.p;
            r.need((size_t)ngenes * geneSize);
            std::string key(reinterpret_cast<const char*>(body.data() + gstart), (size_t)ngenes * geneSize);
            int count = std::min(ngenes, kMaxModes);
            Genome g;
            for (int k = 0; k < count; ++k) g.modes.set(k, readMode(r, geneSizeFor(cver), false, unknownTypes));
            r.p = gstart + (size_t)ngenes * geneSize;
            g.initialMode = readInitialFlagModeIndex(body, gstart, geneSize, count);
            g.name = "Импорт";
            // Cell tail
            int mode = r.i32();
            int tag = r.i32();
            r.i32();
            r.f64(); r.f64(); r.f64();
            double nitrogen = r.f64();
            bool mirrored = r.b8();
            size_t tailRest = cver == kVersionMod ? 177 - 45 : 117 - 45;
            float injury = 0;
            int mutations = 0, telomeres = 0;
            if (cver == kVersionOriginal) {
                for (int k = 0; k < 8; ++k) r.f32();
                r.f32();           // toxins
                injury = r.f32();
                for (int k = 0; k < 3; ++k) r.f32();
                r.f32();           // lipids
                mutations = r.i32();
                telomeres = r.i32();
                r.f64();           // lift
            } else {
                r.skip(tailRest);
            }
            if (dead) {
                info.skippedCells++;
                continue;
            }
            Vec2 pos{(float)x * mm, (float)y * mm};
            float pl = length(pos);
            if (pl > R * 0.999f) pos = pos * (R * 0.999f / std::max(pl, 1e-3f));
            int idx;
            auto it = genomeCache.find(key);
            if (it != genomeCache.end() && it->second >= 0 && w.cells()[it->second].alive) {
                idx = w.addCellLike(it->second, pos, (float)angle, clampv(mode, 0, count - 1), (float)mass, mirrored);
            } else {
                static const CellTag tags[7] = {CellTag::Challenge, CellTag::User, CellTag::Contamination,
                                                CellTag::InfectedChallenge, CellTag::InfectedUser, CellTag::InfectedContamination,
                                                CellTag::Hybrid};
                idx = w.addCell(g, pos, (float)angle, clampv(mode, 0, count - 1), tags[clampv(tag, 0, 6)], (float)std::max(mass, 0.1));
                if (idx >= 0) {
                    genomeCache[key] = idx;
                    info.genomes++;
                }
            }
            if (idx < 0) continue;
            Cell& c = w.cells_[idx];
            c.mirrored = mirrored;
            c.age = (float)cage;
            c.nitrogen = clampv((float)nitrogen, 0.0f, 1.0f);
            c.injury = clampv(injury, 0.0f, 1.0f);
            c.mutations = (uint16_t)std::max(mutations, 0);
            c.telomeres = (int8_t)(cver == kVersionOriginal && telomeres > 0 ? std::min(telomeres, 127) : -1);
            worldIdx[ci] = idx;
            info.cells++;
        }
        for (const PendingLink& l : links)
            if (worldIdx[l.a] >= 0 && worldIdx[l.b] >= 0)
                w.createLink(worldIdx[l.a], worldIdx[l.b], clampv(l.length, 0.0f, 10.0f), clampv(l.stiffness, 0.0f, 1.0f), true);

        int nfood = r.i32();
        if (nfood < 0 || nfood > 1000000) throw Fail{"Неверное число частиц пищи"};
        for (int k = 0; k < nfood; ++k) {
            float fx = r.f32(), fy = r.f32(), size = r.f32();
            r.f32();
            r.f32();
            float coating = r.f32();
            w.addFood({fx * mm, fy * mm}, std::max(size * kFoodUnit, 0.05f), clampv(coating, 0.0f, 1.0f));
            info.food++;
        }
        info.unknownTypes = unknownTypes;
        if (version == kVersionMod) info.note = "мод: 127 мод, 19 типов";
        w.rebuildGrid();
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
    std::vector<uint8_t> d;
    auto u32 = [&](uint32_t v) {
        d.push_back((uint8_t)(v >> 24));
        d.push_back((uint8_t)(v >> 16));
        d.push_back((uint8_t)(v >> 8));
        d.push_back((uint8_t)v);
    };
    auto i32 = [&](int32_t v) { u32((uint32_t)v); };
    auto i16 = [&](int16_t v) {
        d.push_back((uint8_t)((uint16_t)v >> 8));
        d.push_back((uint8_t)v);
    };
    auto f32 = [&](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        u32(u);
    };
    auto b8 = [&](bool b) { d.push_back(b ? 1 : 0); };

    // Cell Lab has 40 modes. Export the modes the organism actually uses (reachable from the initial mode), numbered
    // from 0 in the order they appear, so a genome that uses M90 or M121 still fits as long as it needs <= 40.
    std::vector<int> order;
    int remap[kMaxModes];
    std::fill(std::begin(remap), std::end(remap), -1);
    auto visit = [&](int idx) {
        if (idx < 0 || idx >= kMaxModes || remap[idx] >= 0) return;
        remap[idx] = (int)order.size();
        order.push_back(idx);
    };
    visit(g.initialMode);
    for (size_t q = 0; q < order.size(); ++q) {
        const Mode& m = g.modes[order[q]];
        visit(m.childMode[0]);
        visit(m.childMode[1]);
        if (m.type == CellType::Stemocyte || m.type == CellType::Cystocyte) visit(m.stemMode[0]), visit(m.stemMode[1]);
        if (m.type == CellType::Gamete) visit(m.gameteCompatibleMode), visit(m.gameteModeAfter);
        if (m.type == CellType::Virocyte) visit(m.virusCopyFrom);
    }
    const int used = (int)order.size();
    auto R = [&](int idx) { return idx >= 0 && idx < kMaxModes && remap[idx] >= 0 && remap[idx] < 40 ? remap[idx] : 0; };
    int ownTypes = 0, newSenses = 0, newPrograms = 0;

    i32(kVersionOriginal);
    for (int k = 0; k < 40; ++k) {
        static const Mode kSpare{};
        const Mode& m = k < used ? g.modes[order[k]] : kSpare;
        if (k < used && (int)m.type >= kCellLabTypes) ++ownTypes;
        if (k < used && m.senseTarget >= SenseTarget::Velocity) ++newSenses;
        i32(kVersionOriginal);
        f32(m.color.r);
        f32(m.color.g);
        f32(m.color.b);
        float sm = m.splitMass <= kSplitAtAge ? kSplitMassMin - kSplitMassStep
                   : m.splitMass >= kNeverSplit ? kSplitMassMax + kSplitMassStep
                                                : m.splitMass;
        f32(sm / 10.0f);
        f32(m.splitRatio);
        f32(m.splitAngle);
        f32(m.childAngle[0]);
        f32(m.childAngle[1]);
        f32(m.nutrientPriority);
        i32(R(m.childMode[0]));
        i32(R(m.childMode[1]));
        b8(m.makeAdhesin);
        b8(m.keepAdhesin[0]);
        b8(m.keepAdhesin[1]);
        // CellFap-only types have no Cell Lab counterpart: export the closest original one.
        i32(m.type == CellType::Electrocyte ? (int)CellType::Neurocyte : m.type == CellType::Cnidocyte ? (int)CellType::Devorocyte
            : m.type == CellType::Chromatocyte || m.type == CellType::Luminocyte ? (int)CellType::Phagocyte
            : m.type == CellType::Cystocyte ? (int)CellType::Keratinocyte : m.type == CellType::Grappler ? (int)CellType::Glueocyte
                                                                                                   : (int)m.type);
        i32(7);
        b8(m.prioritize);
        b8(k == 0);  // the initial mode is the first one exported
        b8(m.mirror[0]);
        b8(m.mirror[1]);
        f32(m.adhesinStiffness);
        static const int fieldOf[kNumValues] = {PF_Swim, PF_Density, PF_Bend, PF_Contraction, PF_Lift, PF_Stem1, PF_Stem2,
                                                PF_Neuro1, PF_Neuro2, PF_Neuro3, PF_Neuro4};
        for (int v = 0; v < kNumValues; ++v) {
            const Prog& p = m.prog[fieldOf[v]];
            if (k < used && p.kind != ProgKind::Linear) ++newPrograms;
            const ProgRange& pr = progRange(fieldOf[v]);
            int in = (int)p.input;
            int idx = 0, type = 0;
            if (inputSignal(p.input) >= 0) {
                type = 1;
                idx = inputSignal(p.input) & 3;  // S5..S8 fold onto S1..S4
            } else if (in >= (int)ProgInput::Mass && in <= (int)ProgInput::Connections) {  // Light, Food, Injury: none
                type = 2;
                idx = in - (int)ProgInput::Mass;
            }
            i16((int16_t)idx);
            i16((int16_t)type);
            f32(p.a / pr.scale);
            f32((p.b - pr.offset) / pr.scale);
            f32(p.c);
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
                int ch = secretionSignal(m.secretion, &sign) & 3;
                sec = sign > 0 ? 7 + ch : 11 + ch;
            }
        }
        bool stem = m.type == CellType::Stemocyte, gam = m.type == CellType::Gamete;
        i32(R(stem ? m.stemMode[0] : gam ? m.gameteCompatibleMode : m.virusCopyFrom));
        i32(stem ? R(m.stemMode[1]) : gam ? R(m.gameteModeAfter) : k);
        i32(m.senseChannel & 3);  // Cell Lab has S1..S4 only
        i32(m.senseTarget >= SenseTarget::Velocity ? 0 : (int)m.senseTarget);  // Cell Lab: cell / food / coated / light
        i32(sec);
        for (int c = 0; c < 4; ++c) i32(m.neuroChannel[c] & 3);
        i32(m.maxConnections - 1);
        i32(m.gameteTelomeres);
        f32(m.senseOutput);
        f32(m.senseColor.r);
        f32(m.senseColor.g);
        f32(m.senseColor.b);
        f32(m.senseThreshold);
        f32(m.adhesinLength / 100.0f);
        f32(m.cytoskeleton);
    }
    // Java ObjectOutputStream: magic + one long data block.
    std::vector<uint8_t> out = {0xAC, 0xED, 0x00, 0x05, 0x7A};
    uint32_t n = (uint32_t)d.size();
    out.push_back((uint8_t)(n >> 24));
    out.push_back((uint8_t)(n >> 16));
    out.push_back((uint8_t)(n >> 8));
    out.push_back((uint8_t)n);
    out.insert(out.end(), d.begin(), d.end());
    FILE* f = openFileUtf8(path, "wb");
    if (!f) {
        error = "Не удалось создать файл";
        return false;
    }
    bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    std::fclose(f);
    if (!ok) error = "Ошибка записи";
    if (ok) {  // what Cell Lab cannot hold, told plainly (the export itself succeeded)
        error.clear();
        auto add = [&](const std::string& s) { error += (error.empty() ? "" : "; ") + s; };
        if (used > 40) add("используется " + std::to_string(used) + " мод, в Cell Lab помещается 40 — остальные потеряны");
        if (ownTypes) add("типов CellFap заменено ближайшими: " + std::to_string(ownTypes));
        if (newSenses) add("сенсоров без аналога в Cell Lab: " + std::to_string(newSenses));
        if (newPrograms) add("ступенчатые и волновые программы стали линейными: " + std::to_string(newPrograms));
    }
    return ok;
}
