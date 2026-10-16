#include "mod_shaders.hpp"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

const char* const kSlots[kModShaderSlots] = {"dish", "cell", "line", "particle"};

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

bool toSpirv(const std::string& bytes, std::vector<uint32_t>& out) {
    if (bytes.size() < 20 || bytes.size() % 4 != 0) return false;
    out.resize(bytes.size() / 4);
    std::memcpy(out.data(), bytes.data(), bytes.size());
    return out[0] == 0x07230203u;  // the SPIR-V magic number
}

#ifndef __ANDROID__
// Runs a command line, collects what it prints; the exit code (-1: it did not start).
int runTool(const std::string& cmdUtf8, std::string& output) {
    output.clear();
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, cmdUtf8.c_str(), -1, nullptr, 0);
    std::wstring cmd((size_t)std::max(n, 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmdUtf8.c_str(), -1, cmd.data(), n);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    // No console window flashing up for every shader.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr);
        CloseHandle(rd);
        return -1;
    }
    CloseHandle(wr);
    char buf[512];
    DWORD got = 0;
    while (ReadFile(rd, buf, sizeof(buf), &got, nullptr) && got > 0) output.append(buf, got);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int)code;
#else
    FILE* p = popen((cmdUtf8 + " 2>&1").c_str(), "r");
    if (!p) return -1;
    char buf[512];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), p)) > 0) output.append(buf, got);
    const int st = pclose(p);
    return st;
#endif
}
#endif

}  // namespace

const char* modShaderSlotName(int slot) { return slot >= 0 && slot < kModShaderSlots ? kSlots[slot] : "?"; }

bool modShadersOf(const ModFiles& files, ModShaders& out, std::string& error) {
    out = ModShaders{};
    for (const auto& [path, data] : files) {
        if (path.rfind("shaders/", 0) != 0 || path.size() < 4 || path.compare(path.size() - 4, 4, ".spv") != 0) continue;
        const std::string name = path.substr(8, path.size() - 8 - 4);  // "cell.frag"
        const size_t dot = name.rfind('.');
        const std::string slot = dot == std::string::npos ? name : name.substr(0, dot);
        const std::string stage = dot == std::string::npos ? std::string() : name.substr(dot + 1);
        int s = -1;
        for (int k = 0; k < kModShaderSlots; ++k)
            if (slot == kSlots[k]) s = k;
        if (s < 0 || (stage != "vert" && stage != "frag")) {
            error = path + ": нужно shaders/<dish|cell|line|particle>.<vert|frag>.spv";
            return false;
        }
        std::vector<uint32_t>& dst = stage == "vert" ? out.vert[s] : out.frag[s];
        if (!toSpirv(data, dst)) {
            error = path + " — не SPIR-V (скомпилируйте glslc)";
            out = ModShaders{};
            return false;
        }
    }
    return true;
}

std::string findGlslc(const std::string& configured) {
#ifdef __ANDROID__
    (void)configured;
    return "";
#else
    std::error_code ec;
    if (!configured.empty()) return configured;
#ifdef _WIN32
    const char* exe = "glslc.exe";
#else
    const char* exe = "glslc";
#endif
    if (const char* sdk = std::getenv("VULKAN_SDK")) {
        for (const char* bin : {"Bin", "bin"}) {
            const fs::path p = toPath(sdk) / bin / exe;
            if (fs::exists(p, ec)) return fromPath(p);
        }
    }
    return "glslc";  // (on PATH, if anywhere)
#endif
}

bool compileModShaders(const std::string& modDir, const std::string& glslc, const std::vector<std::string>& includeDirs,
                       std::string& log) {
    log.clear();
    std::error_code ec;
    const fs::path dir = toPath(modDir) / "shaders";
    if (!fs::is_directory(dir, ec)) return true;
    bool ok = true;
    for (int s = 0; s < kModShaderSlots; ++s)
        for (const char* stage : {"vert", "frag"}) {
            const fs::path src = dir / (std::string(kSlots[s]) + "." + stage);
            const fs::path spv = dir / (std::string(kSlots[s]) + "." + stage + ".spv");
            if (!fs::exists(src, ec)) continue;
            const bool fresh = fs::exists(spv, ec) && fs::last_write_time(spv, ec) >= fs::last_write_time(src, ec);
            if (fresh) continue;
#ifdef __ANDROID__
            log += "shaders/" + std::string(kSlots[s]) + "." + stage + ": на телефоне нет компилятора, нужен готовый .spv\n";
            ok = false;
#else
            if (glslc.empty()) {
                log += "shaders/" + std::string(kSlots[s]) + "." + stage + ": нет glslc, а .spv нет или он старше исходника\n";
                ok = false;
                continue;
            }
            std::string cmd = "\"" + glslc + "\" -O -I \"" + fromPath(dir) + "\"";
            for (const std::string& inc : includeDirs) cmd += " -I \"" + inc + "\"";
            cmd += " -o \"" + fromPath(spv) + "\" \"" + fromPath(src) + "\"";
            std::string out;
            const int code = runTool(cmd, out);
            if (code != 0 || !fs::exists(spv, ec)) {
                log += "shaders/" + std::string(kSlots[s]) + "." + stage + ": " +
                       (code == -1 ? "не удалось запустить " + glslc + " (укажите путь к glslc)" : out) + "\n";
                fs::remove(spv, ec);  // (no stale result that looks fresh)
                ok = false;
            } else {
                log += "shaders/" + std::string(kSlots[s]) + "." + stage + " -> .spv\n";
            }
#endif
        }
    return ok;
}
