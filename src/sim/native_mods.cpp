#include "native_mods.hpp"

#include "sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path toPath(const std::string& utf8) {
    try {
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
    } catch (const std::exception&) {
        return fs::path(utf8);
    }
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

#ifdef _WIN32
std::string lastErrorText() {
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   (wchar_t*)&buf, 0, nullptr);
    std::string out = "код " + std::to_string(code);
    if (buf) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string text((size_t)std::max(n - 1, 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, text.data(), n, nullptr, nullptr);
        LocalFree(buf);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        out += ": " + text;
    }
    return out;
}
#endif

}  // namespace

const char* nativePlatform() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    return "windows-x64";
#elif defined(_WIN32) && (defined(_M_ARM64) || defined(__aarch64__))
    return "windows-arm64";
#elif defined(__ANDROID__) && defined(__aarch64__)
    return "android-arm64-v8a";
#elif defined(__ANDROID__) && defined(__x86_64__)
    return "android-x86_64";
#elif defined(__ANDROID__) && defined(__arm__)
    return "android-armeabi-v7a";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__linux__)
    return "linux-x64";
#else
    return "unknown";
#endif
}

const char* nativeLibrarySuffix() {
#if defined(_WIN32)
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

NativeFiles nativeFilesOf(const ModFiles& files) {
    NativeFiles nf;
    const std::string mine = std::string("native/") + nativePlatform() + "/";
    for (const auto& [path, data] : files) {
        if (path.rfind("native/", 0) != 0) continue;
        nf.any = true;
        const size_t slash = path.find('/', 7);
        if (slash != std::string::npos) {
            const std::string plat = path.substr(7, slash - 7);
            if (std::find(nf.platforms.begin(), nf.platforms.end(), plat) == nf.platforms.end()) nf.platforms.push_back(plat);
        }
        if (path.rfind(mine, 0) == 0) {
            nf.here.push_back(path);
            if (endsWith(path, nativeLibrarySuffix())) nf.libraries.push_back(path);
        }
    }
    // (ModFiles is a sorted map: `here` is sorted already.)
    if (!nf.here.empty()) {
        Sha256 h;
        for (const std::string& p : nf.here) {
            const std::string& data = files.at(p);
            const uint64_t size = data.size();
            h.update(p.c_str(), p.size() + 1);  // the name with its 0: "a" + "bc" differs from "ab" + "c"
            h.update(&size, sizeof size);
            h.update(data);
        }
        nf.hash = h.hex();
    }
    return nf;
}

bool nativeEntry(const NativeFiles& nf, const std::string& wanted, std::string& entry, std::string& error) {
    const std::string dir = std::string("native/") + nativePlatform() + "/";
    if (!wanted.empty()) {
        const std::string p = modRelativePath(dir + wanted);
        if (std::find(nf.here.begin(), nf.here.end(), p) == nf.here.end()) {
            error = "в mod.txt указана библиотека «" + wanted + "», но в " + dir + " её нет";
            return false;
        }
        entry = p;
        return true;
    }
    if (nf.libraries.size() == 1) {
        entry = nf.libraries[0];
        return true;
    }
    for (const std::string& p : nf.here)
        if (nf.libraries.empty() && (endsWith(p, ".a") || endsWith(p, ".lib"))) {
            error = p + ": статическую библиотеку загрузить нельзя — нужна динамическая (" + nativeLibrarySuffix() + ")";
            return false;
        }
    error = nf.libraries.empty() ? "в " + dir + " нет библиотеки (" + nativeLibrarySuffix() + ")"
                                 : "в " + dir + " несколько библиотек: укажите главную строкой «native <файл>» в mod.txt";
    return false;
}

bool extractNative(const ModFiles& files, const NativeFiles& nf, const std::string& cacheRoot, const std::string& id,
                   std::string& outDir, std::string& error) {
    std::error_code ec;
    const fs::path base = toPath(cacheRoot) / toPath(id);
    const std::string version = nf.hash.substr(0, 16);
    const fs::path dir = base / version;
    // Older versions of this mod: gone when nothing holds them (a library still loaded on Windows keeps its folder).
    for (auto& e : fs::directory_iterator(base, ec))
        if (e.path().filename() != toPath(version)) fs::remove_all(e.path(), ec);
    ec.clear();
    const std::string prefix = std::string("native/") + nativePlatform() + "/";
    for (const std::string& p : nf.here) {
        const fs::path out = dir / toPath(p.substr(prefix.size()));
        const std::string& data = files.at(p);
        // Already there: the folder is named after the content, and a library in use cannot be rewritten anyway.
        if (fs::exists(out, ec) && fs::file_size(out, ec) == data.size()) continue;
        fs::create_directories(out.parent_path(), ec);
#ifndef _WIN32
        ::chmod(out.c_str(), 0600);  // (a read-only leftover of an interrupted copy)
#endif
        fs::path tmp = out;
        tmp += ".part";
        std::u8string tu = tmp.u8string();
        if (!writeWholeFile(std::string(tu.begin(), tu.end()), data)) {
            std::u8string u = out.u8string();
            error = "не удалось записать " + std::string(u.begin(), u.end());
            return false;
        }
        fs::rename(tmp, out, ec);
        if (ec) {
            error = "не удалось записать библиотеку в кэш: " + ec.message();
            return false;
        }
#ifndef _WIN32
        // Android 14+ refuses to load dynamically written code that stays writable: read-only (and executable).
        ::chmod(out.c_str(), 0500);
#endif
    }
    std::u8string u = dir.u8string();
    outDir = std::string(u.begin(), u.end());
    return true;
}

bool SharedLibrary::open(const std::string& path, std::string& error) {
    close();
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), nullptr, 0);
    std::wstring w((size_t)std::max(n, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), w.data(), n);
    for (wchar_t& c : w)
        if (c == L'/') c = L'\\';  // LOAD_WITH_ALTERED_SEARCH_PATH wants a Windows path
    // The library's dependencies are looked for next to it first (a Rust or Go library may come with others).
    h_ = (void*)LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h_) error = "LoadLibrary: " + lastErrorText();
#else
    h_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h_) {
        const char* e = dlerror();
        error = std::string("dlopen: ") + (e ? e : "?");
    }
#endif
    return h_ != nullptr;
}

void* SharedLibrary::symbol(const char* name) const {
    if (!h_) return nullptr;
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)h_, name);
#else
    return dlsym(h_, name);
#endif
}

void SharedLibrary::close() {
    if (!h_) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)h_);
#else
    dlclose(h_);
#endif
    h_ = nullptr;
}
