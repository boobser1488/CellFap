#include "zip.hpp"

#include <zlib.h>

#include <cstdint>
#include <cstring>

namespace zip {

namespace {

constexpr uint32_t kLocalSig = 0x04034b50, kCentralSig = 0x02014b50, kEndSig = 0x06054b50;
constexpr uint16_t kUtf8Flag = 0x0800;
constexpr uint16_t kDosDate = (0 << 9) | (1 << 5) | 1;  // 1980-01-01: a fixed date keeps packs byte-for-byte reproducible

void put16(std::string& s, uint32_t v) {
    s.push_back((char)(v & 255));
    s.push_back((char)((v >> 8) & 255));
}
void put32(std::string& s, uint32_t v) {
    for (int k = 0; k < 4; ++k) s.push_back((char)((v >> (8 * k)) & 255));
}
uint32_t get16(const std::string& s, size_t at) { return (uint32_t)(uint8_t)s[at] | (uint32_t)(uint8_t)s[at + 1] << 8; }
uint32_t get32(const std::string& s, size_t at) { return get16(s, at) | get16(s, at + 2) << 16; }

// Raw deflate (no zlib header): what ZIP method 8 holds.
bool deflateRaw(const std::string& in, std::string& out) {
    z_stream z{};
    if (deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    out.resize(deflateBound(&z, (uLong)in.size()) + 16);
    z.next_in = (Bytef*)in.data();
    z.avail_in = (uInt)in.size();
    z.next_out = (Bytef*)out.data();
    z.avail_out = (uInt)out.size();
    const int rc = deflate(&z, Z_FINISH);
    out.resize(z.total_out);
    deflateEnd(&z);
    return rc == Z_STREAM_END;
}

bool inflateRaw(const char* in, size_t inSize, std::string& out, size_t expect) {
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return false;
    out.assign(expect, '\0');
    z.next_in = (Bytef*)in;
    z.avail_in = (uInt)inSize;
    z.next_out = (Bytef*)out.data();
    z.avail_out = (uInt)out.size();
    int rc = inflate(&z, Z_FINISH);
    if (expect == 0 && rc == Z_BUF_ERROR) rc = Z_STREAM_END;  // (an empty file: nothing to write into)
    const bool ok = rc == Z_STREAM_END && z.total_out == expect;
    inflateEnd(&z);
    return ok;
}

bool litter(const std::string& name) {
    if (name.empty() || name.back() == '/' || name.back() == '\\') return true;  // a folder entry
    if (name.rfind("__MACOSX/", 0) == 0) return true;
    const size_t slash = name.find_last_of("/\\");
    const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    return base == ".DS_Store" || base == "Thumbs.db" || base == "desktop.ini";
}

}  // namespace

bool looksLikeZip(const std::string& b) {
    return b.size() >= 4 && b[0] == 'P' && b[1] == 'K' && ((b[2] == 3 && b[3] == 4) || (b[2] == 5 && b[3] == 6));
}

bool write(const std::map<std::string, std::string>& files, std::string& out, std::string& error) {
    out.clear();
    std::string central;
    uint32_t count = 0;
    for (const auto& [name, data] : files) {
        if (name.size() > 0xFFFF || data.size() > 0xFFFFFFF0u || out.size() > 0xFFFFFFF0u) {
            error = "слишком большой файл для архива: " + name;
            return false;
        }
        const uint32_t crc = (uint32_t)crc32(0L, (const Bytef*)data.data(), (uInt)data.size());
        std::string packed;
        const bool deflated = !data.empty() && deflateRaw(data, packed) && packed.size() < data.size();
        const std::string& body = deflated ? packed : data;
        const uint32_t offset = (uint32_t)out.size();
        put32(out, kLocalSig);
        put16(out, 20);  // version needed: 2.0 (deflate)
        put16(out, kUtf8Flag);
        put16(out, deflated ? 8 : 0);
        put16(out, 0);  // time
        put16(out, kDosDate);
        put32(out, crc);
        put32(out, (uint32_t)body.size());
        put32(out, (uint32_t)data.size());
        put16(out, (uint32_t)name.size());
        put16(out, 0);  // extra
        out += name;
        out += body;

        put32(central, kCentralSig);
        put16(central, 20);  // made by: 2.0, MS-DOS attributes
        put16(central, 20);
        put16(central, kUtf8Flag);
        put16(central, deflated ? 8 : 0);
        put16(central, 0);
        put16(central, kDosDate);
        put32(central, crc);
        put32(central, (uint32_t)body.size());
        put32(central, (uint32_t)data.size());
        put16(central, (uint32_t)name.size());
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, 0);  // external attributes
        put32(central, offset);
        central += name;
        ++count;
    }
    if (count > 0xFFFF) {
        error = "слишком много файлов для архива";
        return false;
    }
    const uint32_t cdOffset = (uint32_t)out.size();
    out += central;
    put32(out, kEndSig);
    put16(out, 0);
    put16(out, 0);
    put16(out, count);
    put16(out, count);
    put32(out, (uint32_t)central.size());
    put32(out, cdOffset);
    put16(out, 0);  // comment
    return true;
}

bool read(const std::string& b, std::map<std::string, std::string>& out, std::string& error, const Limits& lim,
          std::string (*clean)(const std::string&)) {
    out.clear();
    error = "архив повреждён";
    if (b.size() < 22) return false;
    // The end record sits in the last 22 + 65535 bytes (after it, at most a comment).
    size_t end = std::string::npos;
    const size_t lowest = b.size() > 22 + 0xFFFF ? b.size() - 22 - 0xFFFF : 0;
    for (size_t p = b.size() - 22 + 1; p-- > lowest;)
        if (get32(b, p) == kEndSig) {
            end = p;
            break;
        }
    if (end == std::string::npos) return false;
    const uint32_t disk = get16(b, end + 4), cdDisk = get16(b, end + 6), entries = get16(b, end + 10);
    const uint32_t cdSize = get32(b, end + 12), cdOffset = get32(b, end + 16);
    if (disk != 0 || cdDisk != 0 || entries != get16(b, end + 8)) {
        error = "многотомные архивы не поддерживаются";
        return false;
    }
    if (cdOffset == 0xFFFFFFFFu || entries == 0xFFFF) {
        error = "архивы ZIP64 не поддерживаются";
        return false;
    }
    if ((uint64_t)cdOffset + cdSize > end) return false;
    if (entries > lim.maxFiles) {
        error = "в архиве больше " + std::to_string(lim.maxFiles) + " файлов";
        return false;
    }
    size_t at = cdOffset, total = 0;
    for (uint32_t k = 0; k < entries; ++k) {
        if (at + 46 > end || get32(b, at) != kCentralSig) return false;
        const uint32_t flags = get16(b, at + 8), method = get16(b, at + 10), crc = get32(b, at + 16);
        const uint32_t csize = get32(b, at + 20), usize = get32(b, at + 24);
        const uint32_t nlen = get16(b, at + 28), xlen = get16(b, at + 30), clen = get16(b, at + 32);
        const uint32_t local = get32(b, at + 42);
        if (at + 46 + nlen > end) return false;
        const std::string name = b.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        if (litter(name)) continue;
        if (flags & 1) {
            error = "зашифрованные архивы не поддерживаются (" + name + ")";
            return false;
        }
        if (method != 0 && method != 8) {
            error = "метод сжатия " + std::to_string(method) + " не поддерживается (" + name + "): нужен deflate или без сжатия";
            return false;
        }
        if (csize == 0xFFFFFFFFu || usize == 0xFFFFFFFFu || local == 0xFFFFFFFFu) {
            error = "архивы ZIP64 не поддерживаются";
            return false;
        }
        total += usize;
        if (usize > lim.maxTotal || total > lim.maxTotal) {
            error = "мод больше " + std::to_string(lim.maxTotal >> 20) + " МБ";
            return false;
        }
        const std::string path = clean(name);
        if (path.empty()) {
            error = "недопустимый путь в архиве: " + name;
            return false;
        }
        if ((uint64_t)local + 30 > b.size() || get32(b, local) != kLocalSig) return false;
        const size_t data = (size_t)local + 30 + get16(b, local + 26) + get16(b, local + 28);
        if ((uint64_t)data + csize > b.size()) return false;
        std::string content;
        if (method == 0) {
            if (csize != usize) return false;
            content = b.substr(data, usize);
        } else if (!inflateRaw(b.data() + data, csize, content, usize)) {
            return false;
        }
        if ((uint32_t)crc32(0L, (const Bytef*)content.data(), (uInt)content.size()) != crc) {
            error = "архив повреждён (контрольная сумма " + name + ")";
            return false;
        }
        out[path] = std::move(content);
    }
    error.clear();
    return true;
}

}  // namespace zip
