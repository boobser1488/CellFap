#include "modpack.hpp"

#include "signal_cells.hpp"  // signalCellSourceProblem: a pack never carries a script the game would refuse

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

void put32(std::string& s, uint32_t v) {
    for (int k = 0; k < 4; ++k) s.push_back((char)((v >> (8 * k)) & 255));
}
void put16(std::string& s, uint32_t v) {
    s.push_back((char)(v & 255));
    s.push_back((char)((v >> 8) & 255));
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

bool packModFiles(const ModFiles& files, std::string& bytes, std::string& error) {
    std::string raw;
    put32(raw, (uint32_t)files.size());
    for (const auto& [path, data] : files) {
        if (path.size() > 1024) {
            error = "слишком длинный путь: " + path;
            return false;
        }
        put16(raw, (uint32_t)path.size());
        raw += path;
        put32(raw, (uint32_t)data.size());
        raw += data;
    }
    uLongf cap = compressBound((uLong)raw.size());
    std::string z(cap, '\0');
    if (compress2((Bytef*)z.data(), &cap, (const Bytef*)raw.data(), (uLong)raw.size(), Z_BEST_COMPRESSION) != Z_OK) {
        error = "сжатие не удалось";
        return false;
    }
    z.resize(cap);
    bytes.assign(kMagic, 7);
    put32(bytes, (uint32_t)raw.size());
    put32(bytes, (uint32_t)z.size());
    bytes += z;
    return true;
}

bool unpackModFiles(const std::string& bytes, ModFiles& out, std::string& error) {
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

bool readModPack(const std::string& path, ModFiles& out, std::string& error) {
    std::string bytes;
    if (!readWholeFile(path, bytes, ModPackLimits::kMaxBytes + (1u << 20))) {
        error = "не удалось прочитать " + path;
        return false;
    }
    return unpackModFiles(bytes, out, error);
}

bool buildModPack(const std::string& dir, std::string& outPath, std::string& error) {
    ModFiles files;
    if (!readModFolder(dir, files, error)) return false;
    auto info = files.find("mod.txt");
    if (info == files.end()) {
        error = "в папке нет mod.txt";
        return false;
    }
    bool hasId = false, hasName = false;
    size_t s = 0;
    const std::string& t = info->second;
    while (s < t.size()) {
        size_t e = t.find('\n', s);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(s, e - s);
        if (line.rfind("id ", 0) == 0 && line.size() > 3) hasId = true;
        if (line.rfind("name ", 0) == 0 && line.size() > 5) hasName = true;
        s = e + 1;
    }
    if (!hasId || !hasName) {
        error = "в mod.txt нужны строки «id …» и «name …»";
        return false;
    }
    auto script = files.find("mod.dpt");
    if (script == files.end()) {
        error = "в папке нет mod.dpt";
        return false;
    }
    if (std::string bad = signalCellSourceProblem(script->second); !bad.empty()) {
        error = "mod.dpt: " + bad;
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
