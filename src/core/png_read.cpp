#include "png_read.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

uint32_t be32(const std::string& s, size_t at) {
    return (uint32_t)(uint8_t)s[at] << 24 | (uint32_t)(uint8_t)s[at + 1] << 16 | (uint32_t)(uint8_t)s[at + 2] << 8 | (uint8_t)s[at + 3];
}

int paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

}  // namespace

bool decodePng(const std::string& f, std::vector<uint8_t>& rgba, int& w, int& h, std::string& error, int maxSide) {
    static const unsigned char kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    rgba.clear();
    w = h = 0;
    if (f.size() < 8 + 25 || std::memcmp(f.data(), kSig, 8) != 0) {
        error = "это не PNG";
        return false;
    }
    int depth = 0, color = -1;
    std::string idat;
    std::vector<uint8_t> palette, trns;
    bool sawHeader = false, sawEnd = false;
    for (size_t at = 8; at + 12 <= f.size() && !sawEnd;) {
        const uint32_t len = be32(f, at);
        if (len > f.size() - at - 12) {
            error = "PNG обрезан";
            return false;
        }
        const std::string type = f.substr(at + 4, 4);
        const size_t data = at + 8;
        if (type == "IHDR") {
            if (len < 13) break;
            const uint32_t W = be32(f, data), H = be32(f, data + 4);
            depth = (uint8_t)f[data + 8];
            color = (uint8_t)f[data + 9];
            const int interlace = (uint8_t)f[data + 12];
            if (W == 0 || H == 0 || W > (uint32_t)maxSide || H > (uint32_t)maxSide) {
                error = "картинка " + std::to_string(W) + "×" + std::to_string(H) + ": нужно не больше " + std::to_string(maxSide) +
                        "×" + std::to_string(maxSide);
                return false;
            }
            if (interlace != 0) {
                error = "PNG с чересстрочной развёрткой (interlace) не поддерживается";
                return false;
            }
            const bool ok = (color == 3 && (depth == 1 || depth == 2 || depth == 4 || depth == 8)) ||
                            ((color == 0 || color == 2 || color == 4 || color == 6) && depth == 8);
            if (!ok) {
                error = "PNG: поддерживаются 8-битные серые, RGB, RGBA и палитровые картинки (здесь тип " + std::to_string(color) +
                        ", " + std::to_string(depth) + " бит)";
                return false;
            }
            w = (int)W;
            h = (int)H;
            sawHeader = true;
        } else if (type == "PLTE") {
            palette.assign(f.begin() + data, f.begin() + data + len);
        } else if (type == "tRNS") {
            trns.assign(f.begin() + data, f.begin() + data + len);
        } else if (type == "IDAT") {
            idat.append(f, data, len);
        } else if (type == "IEND") {
            sawEnd = true;
        }
        at += 12 + len;
    }
    if (!sawHeader || idat.empty()) {
        error = "PNG без заголовка или без пикселей";
        return false;
    }
    if (color == 3 && palette.size() < 3) {
        error = "PNG с палитрой без палитры";
        return false;
    }
    const int channels = color == 0 ? 1 : color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
    const size_t stride = ((size_t)w * channels * depth + 7) / 8;
    const size_t bpp = std::max<size_t>(1, (size_t)channels * depth / 8);
    std::vector<uint8_t> raw((stride + 1) * (size_t)h);
    uLongf got = (uLongf)raw.size();
    if (uncompress(raw.data(), &got, (const Bytef*)idat.data(), (uLong)idat.size()) != Z_OK || got != raw.size()) {
        error = "PNG повреждён (сжатые данные)";
        return false;
    }
    // Undo the filters row by row.
    std::vector<uint8_t> prev(stride, 0), cur(stride);
    rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = raw.data() + (size_t)y * (stride + 1);
        const int filter = row[0];
        for (size_t i = 0; i < stride; ++i) {
            const int x = row[1 + i], a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
            int v;
            switch (filter) {
                case 0: v = x; break;
                case 1: v = x + a; break;
                case 2: v = x + b; break;
                case 3: v = x + ((a + b) >> 1); break;
                case 4: v = x + paeth(a, b, c); break;
                default:
                    error = "PNG повреждён (фильтр строки)";
                    rgba.clear();
                    return false;
            }
            cur[i] = (uint8_t)v;
        }
        uint8_t* out = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x, out += 4) {
            switch (color) {
                case 0: out[0] = out[1] = out[2] = cur[x]; break;
                case 4: out[0] = out[1] = out[2] = cur[x * 2]; out[3] = cur[x * 2 + 1]; break;
                case 2: std::memcpy(out, &cur[x * 3], 3); break;
                case 6: std::memcpy(out, &cur[x * 4], 4); break;
                default: {  // palette
                    const int perByte = 8 / depth, shift = 8 - depth * (x % perByte + 1);
                    const int idx = (cur[x / perByte] >> shift) & ((1 << depth) - 1);
                    if ((size_t)idx * 3 + 2 < palette.size()) std::memcpy(out, &palette[(size_t)idx * 3], 3);
                    else out[0] = out[1] = out[2] = 0;
                    out[3] = (size_t)idx < trns.size() ? trns[idx] : 255;
                }
            }
        }
        prev.swap(cur);
    }
    return true;
}
