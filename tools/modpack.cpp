// Сборщик модов CellFap: папка мода -> один файл .cfmod, и обратно.
//
//   modpack <папка> [файл.cfmod]      собрать (по умолчанию <папка>.cfmod рядом с папкой)
//   modpack --list <файл.cfmod>        показать, что внутри
//   modpack --unpack <файл.cfmod> <папка>   распаковать (чтобы поправить чужой мод)
//   modpack --check <папка>            проверить mod.txt и mod.dpt, ничего не записывая
//
// Тот же код собирает моды в игре (окно «Моды» -> «Собрать .cfmod»).
#include "sim/modpack.hpp"
#include "sim/mods.hpp"
#include "sim/signal_cells.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

int usage() {
    std::printf(
        "modpack <папка> [файл.cfmod]           собрать мод в один файл\n"
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
    auto script = files.find("mod.dpt");
    if (script == files.end()) {
        std::printf("ошибка: нет mod.dpt\n");
        return false;
    }
    if (std::string bad = signalCellSourceProblem(script->second); !bad.empty()) {
        std::printf("ошибка в mod.dpt: %s\n", bad.c_str());
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    if (argc < 2) return usage();
    const std::string a1 = argv[1];
    if (a1 == "--list" && argc == 3) {
        ModFiles files;
        std::string err;
        if (!readModPack(argv[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        describe(files);
        for (const auto& [path, data] : files) std::printf("  %8zu  %s\n", data.size(), path.c_str());
        return 0;
    }
    if (a1 == "--unpack" && argc == 4) {
        ModFiles files;
        std::string err;
        if (!readModPack(argv[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        namespace fs = std::filesystem;
        const std::string root = argv[3];
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
    if (a1 == "--check" && argc == 3) {
        ModFiles files;
        std::string err;
        if (!readModFolder(argv[2], files, err)) {
            std::printf("ошибка: %s\n", err.c_str());
            return 1;
        }
        if (!describe(files)) return 1;
        std::printf("в порядке: %zu файлов\n", files.size());
        return 0;
    }
    if (a1.rfind("--", 0) == 0 || argc > 3) return usage();
    ModFiles files;
    std::string err;
    if (!readModFolder(a1, files, err)) {
        std::printf("ошибка: %s\n", err.c_str());
        return 1;
    }
    if (!describe(files)) return 1;
    std::string out = argc == 3 ? argv[2] : "";
    if (!buildModPack(a1, out, err)) {
        std::printf("ошибка: %s\n", err.c_str());
        return 1;
    }
    std::printf("собрано: %s (%zu файлов)\n", out.c_str(), files.size());
    return 0;
}
