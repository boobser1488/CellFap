#pragma once
// Minimal PNG writer for screenshots: zlib deflate (already linked for the game's own files) after per-row PNG filters
// chosen by the usual "smallest sum of absolute values" rule. A 1536×864 capture of the game shrinks from ~4 MB (it
// used to be written uncompressed) to a few hundred KB.
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace png_detail {
inline uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
inline void be32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back((uint8_t)(v >> 24));
    o.push_back((uint8_t)(v >> 16));
    o.push_back((uint8_t)(v >> 8));
    o.push_back((uint8_t)v);
}
inline void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}
inline uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}
}  // namespace png_detail

// rgb: tightly packed 8-bit RGB rows, top to bottom. `file` is an already-opened binary FILE*.
inline bool writePngRgb(FILE* file, uint32_t w, uint32_t h, const uint8_t* rgb) {
    using namespace png_detail;
    const size_t stride = (size_t)w * 3;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * h);
    std::vector<uint8_t> cand[5];
    for (auto& c : cand) c.resize(stride);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* cur = rgb + (size_t)y * stride;
        const uint8_t* up = y ? cur - stride : nullptr;
        // The five filters of the PNG standard; the row goes out with the one whose bytes look smallest.
        int best = 0;
        uint64_t bestSum = UINT64_MAX;
        for (int f = 0; f < 5; ++f) {
            uint64_t sum = 0;
            for (size_t i = 0; i < stride; ++i) {
                const int a = i >= 3 ? cur[i - 3] : 0, b = up ? up[i] : 0, c = (i >= 3 && up) ? up[i - 3] : 0;
                uint8_t v = cur[i];
                if (f == 1) v = (uint8_t)(v - a);
                else if (f == 2) v = (uint8_t)(v - b);
                else if (f == 3) v = (uint8_t)(v - ((a + b) >> 1));
                else if (f == 4) v = (uint8_t)(v - paeth(a, b, c));
                cand[f][i] = v;
                sum += v < 128 ? v : 256 - v;
            }
            if (sum < bestSum) {
                bestSum = sum;
                best = f;
            }
        }
        raw.push_back((uint8_t)best);
        raw.insert(raw.end(), cand[best].begin(), cand[best].end());
    }
    uLongf zn = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zn);
    if (compress2(z.data(), &zn, raw.data(), (uLong)raw.size(), 6) != Z_OK) return false;
    z.resize(zn);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit, truecolour
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return std::fwrite(out.data(), 1, out.size(), file) == out.size();
}
