// Сборщик модов CellFap: папка мода -> один файл .cfmod, и обратно.
//
//   modpack <папка> [файл.cfmod]      собрать (по умолчанию <имя папки>.cfmod в текущей папке)
//   modpack --list <файл.cfmod>        показать, что внутри
//   modpack --unpack <файл.cfmod> <папка>   распаковать (чтобы поправить чужой мод)
//   modpack --check <папка>            проверить mod.txt, mod.dpt и native/, ничего не записывая
//
// Тот же код собирает моды в игре (окно «Модификации» -> «Собрать .cfmod»).
#include "sim/modpack.hpp"
#include "sim/mods.hpp"
#include "sim/native_mods.hpp"
#include "sim/signal_cells.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

int usage() {
    std::printf(
        "modpack <папка> [файл.cfmod]           собрать мод в один файл (по умолчанию <имя папки>.cfmod здесь)\n"
        "modpack --list <файл.cfmod>             показать содержимое\n"
        "modpack --unpack <файл.cfmod> <папка>   распаковать\n"
        "modpack --check <папка>                 проверить, ничего не записывая\n");
    return 2;
}

bool describe(const ModFiles& files) {
    auto info = files.find("mod.txt");
    if (info == files.end()) {
        std::printf("ошибка: нет mod.txt\n");
        return false;
    }
    ModInfo m;
    std::string err;
    if (!parseModInfo(info->second, m, err)) {
        std::printf("ошибка: %s\n", err.c_str());
        return false;
    }
    std::printf("мод:       %s (%s) %s\n", m.name.c_str(), m.id.c_str(), m.version.c_str());
    if (!m.author.empty()) std::printf("автор:     %s\n", m.author.c_str());
    std::printf("тик:       %s\n", m.tickEvery > 0 ? ("каждые " + std::to_string(m.tickEvery) + " тиков").c_str() : "нет");
    for (const ModSetting& s : m.settings) std::printf("настройка: %s — %s\n", s.key.c_str(), s.label.c_str());
    // Native code: every platform it ships, and for each the library with the entry points.
    bool native = false;
    std::vector<std::string> platforms;
    for (const auto& [path, data] : files) {
        if (path.rfind("native/", 0) != 0) continue;
        native = true;
        const size_t slash = path.find('/', 7);
        const std::string plat = slash == std::string::npos ? std::string() : path.substr(7, slash - 7);
        if (!plat.empty() && (platforms.empty() || platforms.back() != plat)) platforms.push_back(plat);
    }
    bool ok = true;
    if (native) {
        std::printf("нативный код (полный доступ к компьютеру; игра спросит разрешения):\n");
        for (const std::string& plat : platforms) {
            const std::string suffix = plat.rfind("windows", 0) == 0 ? ".dll" : plat.rfind("macos", 0) == 0 ? ".dylib" : ".so";
            std::vector<std::string> libs;
            ModFiles platFiles;
            for (const auto& [path, data] : files)
                if (path.rfind("native/" + plat + "/", 0) == 0) {
                    platFiles[path] = data;
                    if (path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
                        libs.push_back(path);
                }
            std::string entry = m.nativeLib.empty() ? (libs.size() == 1 ? libs[0] : "") : "native/" + plat + "/" + m.nativeLib;
            const bool found = !entry.empty() && platFiles.count(entry);
            std::printf("  %-20s %zu файлов, главная библиотека: %s\n", plat.c_str(), platFiles.size(),
                        found ? entry.c_str() : (libs.empty() ? "нет" : "не выбрана (строка «native <файл>» в mod.txt)"));
            ok = ok && found;
        }
        const NativeFiles here = nativeFilesOf(files);
        if (!here.hash.empty()) std::printf("  SHA-256 (%s): %s\n", nativePlatform(), here.hash.c_str());
        if (!ok) std::printf("ошибка: не для каждой платформы ясно, какую библиотеку загружать\n");
    }
    auto script = files.find("mod.dpt");
    if (script == files.end()) {
        if (!native) {
            std::printf("ошибка: нет ни mod.dpt, ни native/\n");
            return false;
        }
        return ok;
    }
    if (std::string bad = signalCellSourceProblem(script->second); !bad.empty()) {
        std::printf("ошибка в mod.dpt: %s\n", bad.c_str());
        return false;
    }
    return ok;
}

// The arguments as UTF-8 (on Windows argv is in the ANSI code page: Cyrillic folder names would not survive).
std::vector<std::string> utf8Args(int argc, char** argv) {
    std::vector<std::string> out;
#ifdef _WIN32
    int n = 0;
    if (wchar_t** w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) {
            const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(len > 0 ? (size_t)len - 1 : 0, '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
            out.push_back(std::move(s));
        }
        LocalFree(w);
        return out;
    }
#endif
    for (int i = 0; i < argc; ++i) out.push_back(argv[i]);
    return out;
}

std::string fileNameOf(const std::string& dir) {
    namespace fs = std::filesystem;
    fs::path d(std::u8string(dir.begin(), dir.end()));
    while (!d.empty() && !d.has_filename()) d = d.parent_path();  // "mods/x/" -> "mods/x"
    const std::u8string n = d.filename().u8string();
    return std::string(n.begin(), n.end());
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    const std::vector<std::string> args = utf8Args(argc, argv);
    const int n = (int)args.size();
    if (n < 2) return usage();
    const std::string& a1 = args[1];
    if (a1 == "--list" && n == 3) {
        ModFiles files;
        std::string err;
        if (!readModPack(args[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        describe(files);
        for (const auto& [path, data] : files) std::printf("  %8zu  %s\n", data.size(), path.c_str());
        return 0;
    }
    if (a1 == "--unpack" && n == 4) {
        ModFiles files;
        std::string err;
        if (!readModPack(args[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        namespace fs = std::filesystem;
        const std::string& root = args[3];
        for (const auto& [path, data] : files) {
            const std::string out = root + "/" + path;  // paths inside a pack are already checked (no "..", relative)
            std::error_code ec;
            fs::create_directories(fs::path(std::u8string(out.begin(), out.end())).parent_path(), ec);
            if (!writeWholeFile(out, data)) {
                std::printf("ошибка: не удалось записать %s\n", out.c_str());
                return 1;
            }
        }
        std::printf("распаковано файлов: %zu -> %s\n", files.size(), root.c_str());
        return 0;
    }
    if (a1 == "--check" && n == 3) {
        ModFiles files;
        std::string err;
        if (!readModFolder(args[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        if (!describe(files)) return 1;
        std::printf("в порядке: %zu файлов\n", files.size());
        return 0;
    }
    if (a1.rfind("--", 0) == 0 || n > 3) return usage();
    ModFiles files;
    std::string err;
    if (!readModFolder(a1, files, err)) {
        std::printf("ошибка: %s\n", err.c_str());
        return 1;
    }
    if (!describe(files)) return 1;
    // Not next to the folder by default: that is usually mods/, where the pack would be a second mod with its id.
    std::string out = n == 3 ? args[2] : fileNameOf(a1) + ".cfmod";
    if (!buildModPack(a1, out, err)) {
        std::printf("ошибка: %s\n", err.c_str());
        return 1;
    }
    std::printf("собрано: %s (%zu файлов)\n", out.c_str(), files.size());
    return 0;
}
