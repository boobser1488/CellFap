// Draws the CellFap app icon (a microscope dish with dividing cells) and writes every size the builds need:
//   android/res/mipmap-*/ic_launcher.png, ic_launcher_round.png  (legacy launcher icons)
//   android/res/mipmap-*/ic_launcher_fg.png                      (adaptive icon foreground, 108 dp)
//   resources/cellfap.ico                                         (Windows executable icon, PNG entries)
// Run from the project root after changing the drawing:  build/release/make_icon.exe
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

struct RGBA {
    float r = 0, g = 0, b = 0, a = 0;  // premultiplied
};
RGBA over(RGBA dst, float r, float g, float b, float a) {
    return {r * a + dst.r * (1 - a), g * a + dst.g * (1 - a), b * a + dst.b * (1 - a), a + dst.a * (1 - a)};
}
float cover(float d, float aa) { return std::clamp(0.5f - d / aa, 0.0f, 1.0f); }  // d < 0 inside
float len(float x, float y) { return std::sqrt(x * x + y * y); }
float hexc(int v, int sh) { return ((v >> sh) & 255) / 255.0f; }
RGBA fill(RGBA dst, int rgb, float a) { return over(dst, hexc(rgb, 16), hexc(rgb, 8), hexc(rgb, 0), a); }

// One cell: membrane ring, body, nucleus. (x, y, r) in icon units.
RGBA cell(RGBA c, float px, float py, float x, float y, float r, int body, int membrane, int nucleus, float aa) {
    float d = len(px - x, py - y) - r;
    c = fill(c, membrane, cover(d, aa));
    c = fill(c, body, cover(d + r * 0.16f, aa));
    c = fill(c, nucleus, cover(len(px - x + r * 0.18f, py - y + r * 0.12f) - r * 0.33f, aa));
    c = fill(c, 0xE8F5E9, 0.55f * cover(len(px - x - r * 0.38f, py - y + r * 0.42f) - r * 0.14f, aa));  // highlight
    return c;
}

enum class Kind { Legacy, Round, Foreground };

// Colour of the icon at (px, py) in [0,1]^2. `k` = the content scale around the centre.
RGBA shade(float px, float py, Kind kind, float aa) {
    RGBA c;
    // Transparent icon: only the dish and its cells, no backing plate (the launcher or the desktop shows through).
    // Content in "design units": the dish (radius 0.445 with the rim) nearly fills a legacy icon; the adaptive
    // foreground keeps it inside the safe zone.
    const float k = kind == Kind::Foreground ? 0.64f / 0.89f : 1.06f;
    float x = (px - 0.5f) / k + 0.5f, y = (py - 0.5f) / k + 0.5f;
    float a2 = aa / k;
    // Dish: lavender disc with a dark rim.
    float dd = len(x - 0.5f, y - 0.5f) - 0.42f;
    c = fill(c, 0x2E2F45, cover(dd, a2));
    c = fill(c, 0xCBC8EE, cover(dd + 0.025f, a2));
    c = fill(c, 0xDAD8F6, 0.6f * cover(len(x - 0.42f, y - 0.40f) - 0.22f, a2 * 6));  // soft light
    // Food specks.
    const float specks[][2] = {{0.27f, 0.36f}, {0.70f, 0.30f}, {0.33f, 0.70f}, {0.62f, 0.76f}, {0.24f, 0.55f}, {0.76f, 0.56f}};
    for (auto& s : specks) c = fill(c, 0x7A5A3C, cover(len(x - s[0], y - s[1]) - 0.018f, a2));
    // Orange flagellate with a wavy tail (lower right).
    for (float t = 0; t <= 1.0f; t += 0.02f) {
        float tx = 0.66f + 0.16f * t, ty = 0.66f + 0.10f * t + 0.025f * std::sin(t * 12.0f);
        c = fill(c, 0xC7741F, cover(len(x - tx, y - ty) - 0.012f * (1.0f - 0.6f * t), a2));
    }
    c = cell(c, x, y, 0.635f, 0.635f, 0.075f, 0xFF9E33, 0xC7741F, 0x8A4E12, a2);
    // Two green cells just after division.
    c = cell(c, x, y, 0.42f, 0.52f, 0.17f, 0x74C779, 0x3E8A46, 0x2F6A35, a2);
    c = cell(c, x, y, 0.62f, 0.38f, 0.125f, 0x74C779, 0x3E8A46, 0x2F6A35, a2);
    return c;
}

std::vector<uint8_t> render(int n, Kind kind) {
    const int ss = 4;
    std::vector<uint8_t> px((size_t)n * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            RGBA acc;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    float u = (x + (sx + 0.5f) / ss) / n, v = (y + (sy + 0.5f) / ss) / n;
                    RGBA s = shade(u, v, kind, 1.0f / n);
                    acc.r += s.r;
                    acc.g += s.g;
                    acc.b += s.b;
                    acc.a += s.a;
                }
            float inv = 1.0f / (ss * ss);
            float a = acc.a * inv;
            uint8_t* p = &px[((size_t)y * n + x) * 4];
            for (int ch = 0; ch < 3; ++ch) {
                float v = (&acc.r)[ch] * inv;
                p[ch] = (uint8_t)std::lround(std::clamp(a > 1e-4f ? v / a : 0.0f, 0.0f, 1.0f) * 255.0f);
            }
            p[3] = (uint8_t)std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        }
    return px;
}

void be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) o.push_back((uint8_t)(v >> s));
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
    be32(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, (uint32_t)crc32(0, out.data() + start, (uInt)(out.size() - start)));
}
std::vector<uint8_t> png(int n, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < n; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + (size_t)y * n * 4, rgba.begin() + (size_t)(y + 1) * n * 4);
    }
    uLongf zn = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(zn);
    compress2(z.data(), &zn, raw.data(), (uLong)raw.size(), 9);
    z.resize(zn);
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    be32(ihdr, n);
    be32(ihdr, n);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}
bool save(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    std::fclose(f);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    return ok;
}

}  // namespace

int main() {
    struct D {
        const char* dir;
        int legacy, fg;
    } dens[] = {{"mdpi", 48, 108}, {"hdpi", 72, 162}, {"xhdpi", 96, 216}, {"xxhdpi", 144, 324}, {"xxxhdpi", 192, 432}};
    for (const D& d : dens) {
        std::string base = std::string("android/res/mipmap-") + d.dir + "/";
        save(base + "ic_launcher.png", png(d.legacy, render(d.legacy, Kind::Legacy)));
        save(base + "ic_launcher_round.png", png(d.legacy, render(d.legacy, Kind::Round)));
        save(base + "ic_launcher_fg.png", png(d.fg, render(d.fg, Kind::Foreground)));
    }
    // Windows .ico with PNG entries (Vista+).
    const int sizes[] = {256, 64, 48, 32, 16};
    std::vector<std::vector<uint8_t>> imgs;
    for (int s : sizes) imgs.push_back(png(s, render(s, Kind::Legacy)));
    std::vector<uint8_t> ico = {0, 0, 1, 0, (uint8_t)std::size(sizes), 0};
    uint32_t offset = 6 + 16 * (uint32_t)std::size(sizes);
    auto le16 = [&](uint32_t v) { ico.push_back((uint8_t)v), ico.push_back((uint8_t)(v >> 8)); };
    auto le32 = [&](uint32_t v) { le16(v & 0xFFFF), le16(v >> 16); };
    for (size_t i = 0; i < imgs.size(); ++i) {
        ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.push_back(0);
        ico.push_back(0);
        le16(1);
        le16(32);
        le32((uint32_t)imgs[i].size());
        le32(offset);
        offset += (uint32_t)imgs[i].size();
    }
    for (auto& im : imgs) ico.insert(ico.end(), im.begin(), im.end());
    save("resources/cellfap.ico", ico);
    save("resources/icon_preview.png", png(512, render(512, Kind::Legacy)));
    return 0;
}
