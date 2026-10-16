#include "modpack.hpp"

#include "mods.hpp"          // parseModInfo
#include "signal_cells.hpp"  // signalCellSourceProblem: a pack never carries a script the game would refuse
#include "mod_shaders.hpp"
#include "zip.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

const char kMagic[] = "CFMOD1\n";  // 7 bytes

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

bool get32(const std::string& s, size_t& at, uint32_t& v) {
    if (at + 4 > s.size()) return false;
    v = 0;
    for (int k = 0; k < 4; ++k) v |= (uint32_t)(uint8_t)s[at + k] << (8 * k);
    at += 4;
    return true;
}
bool get16(const std::string& s, size_t& at, uint32_t& v) {
    if (at + 2 > s.size()) return false;
    v = (uint32_t)(uint8_t)s[at] | ((uint32_t)(uint8_t)s[at + 1] << 8);
    at += 2;
    return true;
}

}  // namespace

std::string modRelativePath(const std::string& path) {
    std::string p = path;
    for (char& c : p)
        if (c == '\\') c = '/';
    if (p.empty() || p[0] == '/' || p.find(':') != std::string::npos) return "";
    std::string out, part;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (part == "..") return "";
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        } else {
            part.push_back(p[i]);
        }
    }
    return out;
}

const std::string* findModFile(const ModFiles& files, const std::string& path) {
    const std::string p = modRelativePath(path);
    if (p.empty()) return nullptr;
    if (auto it = files.find(p); it != files.end()) return &it->second;
    if (auto it = files.find("assets/" + p); it != files.end()) return &it->second;
    return nullptr;
}

bool readWholeFile(const std::string& path, std::string& out, size_t maxBytes) {
    std::error_code ec;
    const auto size = fs::file_size(toPath(path), ec);
    if (ec || size > maxBytes) return false;
#ifdef _WIN32
    FILE* f = _wfopen(toPath(path).c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    out.resize((size_t)size);
    const size_t got = size ? std::fread(out.data(), 1, (size_t)size, f) : 0;
    std::fclose(f);
    out.resize(got);
    return got == (size_t)size;
}

bool writeWholeFile(const std::string& path, const std::string& data) {
#ifdef _WIN32
    FILE* f = _wfopen(toPath(path).c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

bool readModFolder(const std::string& dir, ModFiles& out, std::string& error) {
    out.clear();
    std::error_code ec;
    const fs::path root = toPath(dir);
    if (!fs::is_directory(root, ec)) {
        error = "нет папки " + dir;
        return false;
    }
    size_t total = 0;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = modRelativePath(fromPath(fs::relative(it->path(), root, ec)));
        if (rel.empty() || rel[0] == '.') continue;  // hidden files (.git...) are not part of the mod
        if (out.size() >= ModPackLimits::kMaxFiles) {
            error = "в моде больше " + std::to_string(ModPackLimits::kMaxFiles) + " файлов";
            return false;
        }
        std::string data;
        if (!readWholeFile(fromPath(it->path()), data)) {
            error = "не удалось прочитать " + rel;
            return false;
        }
        total += data.size();
        if (total > ModPackLimits::kMaxBytes) {
            error = "мод больше " + std::to_string(ModPackLimits::kMaxBytes >> 20) + " МБ";
            return false;
        }
        out[rel] = std::move(data);
    }
    return true;
}

bool packModFiles(const ModFiles& files, std::string& bytes, std::string& error) { return zip::write(files, bytes, error); }

namespace {

// A player who zips the folder itself gets everything under "<folder>/": the mod is that folder.
void stripTopFolder(ModFiles& files) {
    if (files.empty() || files.count("mod.txt")) return;
    std::string top;
    for (const auto& [path, data] : files) {
        const size_t slash = path.find('/');
        if (slash == std::string::npos) return;  // a file at the top: no single folder
        const std::string t = path.substr(0, slash + 1);
        if (top.empty()) top = t;
        else if (t != top) return;
    }
    if (!files.count(top + "mod.txt")) return;
    ModFiles inner;
    for (auto& [path, data] : files) inner[path.substr(top.size())] = std::move(data);
    files.swap(inner);
}

// The first packs (they lived for one day): "CFMOD1\n", sizes, one zlib stream of the file table. Still read.
bool unpackLegacy(const std::string& bytes, ModFiles& out, std::string& error) {
    out.clear();
    error = "файл повреждён или это не мод CellFap";
    if (bytes.size() < 15 || bytes.compare(0, 7, kMagic) != 0) return false;
    size_t at = 7;
    uint32_t rawSize = 0, zSize = 0;
    if (!get32(bytes, at, rawSize) || !get32(bytes, at, zSize) || at + zSize != bytes.size()) return false;
    if (rawSize > ModPackLimits::kMaxBytes + ModPackLimits::kMaxFiles * 1100) {
        error = "мод больше " + std::to_string(ModPackLimits::kMaxBytes >> 20) + " МБ";
        return false;
    }
    std::string raw(rawSize, '\0');
    uLongf got = rawSize;
    if (uncompress((Bytef*)raw.data(), &got, (const Bytef*)bytes.data() + at, zSize) != Z_OK || got != rawSize) return false;
    size_t p = 0;
    uint32_t count = 0;
    if (!get32(raw, p, count) || count > ModPackLimits::kMaxFiles) return false;
    for (uint32_t k = 0; k < count; ++k) {
        uint32_t plen = 0, dlen = 0;
        if (!get16(raw, p, plen) || p + plen > raw.size()) return false;
        const std::string path = modRelativePath(raw.substr(p, plen));
        p += plen;
        if (!get32(raw, p, dlen) || p + dlen > raw.size() || path.empty()) return false;
        out[path] = raw.substr(p, dlen);
        p += dlen;
    }
    if (p != raw.size()) return false;
    error.clear();
    return true;
}

}  // namespace

bool unpackModFiles(const std::string& bytes, ModFiles& out, std::string& error) {
    out.clear();
    if (bytes.compare(0, 7, kMagic) == 0) return unpackLegacy(bytes, out, error);
    if (!zip::looksLikeZip(bytes)) {
        error = "это не мод CellFap (.cfmod — архив ZIP)";
        return false;
    }
    zip::Limits lim;
    lim.maxFiles = ModPackLimits::kMaxFiles;
    lim.maxTotal = ModPackLimits::kMaxBytes;
    if (!zip::read(bytes, out, error, lim, modRelativePath)) {
        out.clear();
        return false;
    }
    stripTopFolder(out);
    if (!out.count("mod.txt")) {
        error = "в архиве нет mod.txt (он должен лежать в корне архива)";
        out.clear();
        return false;
    }
    return true;
}

bool readModPack(const std::string& path, ModFiles& out, std::string& error) {
    std::string bytes;
    if (!readWholeFile(path, bytes, ModPackLimits::kMaxBytes + (1u << 20))) {
        error = "не удалось прочитать " + path;
        return false;
    }
    return unpackModFiles(bytes, out, error);
}

std::string modPackProblem(const ModFiles& files, std::vector<std::string>* notes) {
    auto info = files.find("mod.txt");
    if (info == files.end()) return "нет mod.txt";
    // The game's own reader (a byte-order mark, the id, the name, the settings): a pack it would refuse is not built.
    ModInfo m;
    std::string error;
    if (!parseModInfo(info->second, m, error)) return error;
    if (auto script = files.find("mod.dpt"); script != files.end())
        if (std::string bad = signalCellSourceProblem(script->second); !bad.empty()) return "mod.dpt: " + bad;
    for (const auto& [path, data] : files) {
        if (path.rfind("native/", 0) != 0) continue;
        // A static library is code to link, not to load: nothing at run time can use it.
        const bool isStatic = (path.size() > 2 && path.compare(path.size() - 2, 2, ".a") == 0) ||
                              (path.size() > 4 && path.compare(path.size() - 4, 4, ".lib") == 0);
        if (isStatic)
            return path + ": статическую библиотеку игра загрузить не может — слинкуйте её в свою .dll / .so "
                          "(crate-type cdylib, gcc -shared, go -buildmode=c-shared) и положите в native/ её";
    }
    ModShaders shaders;
    if (!modShadersOf(files, shaders, error)) return error;
    if (notes) {
        for (const auto& [path, data] : files) {
            if (path.rfind("shaders/", 0) != 0 || path.size() < 5) continue;
            const std::string ext = path.substr(path.size() - 5);
            if ((ext == ".vert" || ext == ".frag") && !files.count(path + ".spv"))
                notes->push_back(path + ": нет " + path.substr(8) + ".spv — игра загружает только скомпилированный SPIR-V");
        }
        bool native = false;
        for (const auto& [path, data] : files) native = native || path.rfind("native/", 0) == 0;
        if (!files.count("mod.dpt") && !shaders.any() && !native) notes->push_back("нет ни mod.dpt, ни native/, ни shaders/: мод только с файлами");
    }
    return {};
}

bool buildModPack(const std::string& dir, std::string& outPath, std::string& error, const ModPackOptions* opt) {
    // GLSL sources first: their .spv are what goes into the pack.
    if (opt && opt->compileShaders) {
        std::string log;
        const bool ok = compileModShaders(dir, opt->glslc, opt->includeDirs, log);
        if (opt->log) *opt->log += log;
        if (!ok) {
            error = "шейдеры не скомпилированы:\n" + log;
            return false;
        }
    }
    ModFiles files;
    if (!readModFolder(dir, files, error)) return false;
    if (std::string bad = modPackProblem(files, opt ? opt->notes : nullptr); !bad.empty()) {
        error = bad;
        return false;
    }
    std::string bytes;
    if (!packModFiles(files, bytes, error)) return false;
    if (outPath.empty()) {
        fs::path d = toPath(dir);
        while (!d.empty() && !d.has_filename()) d = d.parent_path();  // "mods/x/" -> "mods/x"
        outPath = fromPath(d) + ".cfmod";
    }
    if (!writeWholeFile(outPath, bytes)) {
        error = "не удалось записать " + outPath;
        return false;
    }
    return true;
}
