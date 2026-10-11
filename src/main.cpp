#include "app.hpp"

#ifdef __ANDROID__
#define main cellfap_main  // android/android_main.cpp calls it from android_main()
#endif

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms sleeps: frame pacing during incubation would otherwise round to 15.6 ms
    // argv comes in the ANSI code page: a file opened by double click with a Cyrillic name would arrive garbled.
    // Take the wide command line and pass UTF-8 on.
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Argv;
    if (int n = 0; LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n)) {
        for (int i = 0; i < n; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s((size_t)std::max(len - 1, 0), '\0');
            if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
            utf8Args.push_back(std::move(s));
        }
        LocalFree(w);
        for (std::string& s : utf8Args) utf8Argv.push_back(s.data());
        utf8Argv.push_back(nullptr);
        argc = n;
        argv = utf8Argv.data();
    }
#endif
    AppOptions opt;
    for (int i = 1; i < argc; ++i) {
        auto is = [&](const char* name) { return std::strcmp(argv[i], name) == 0; };
        bool hasValue = i + 1 < argc;
        if (is("--validation")) opt.validation = true;
        else if (is("--benchmark")) opt.benchmark = true;
        else if (is("--perf")) opt.showPerf = true;
        else if (is("--exit-after") && hasValue) opt.exitAfterSeconds = std::atof(argv[++i]);
        else if (is("--screenshot") && hasValue) opt.screenshotPath = argv[++i];
        else if (is("--screenshot-after") && hasValue) opt.screenshotAfter = std::atof(argv[++i]);
        else if (is("--frames") && hasValue) opt.shotFrames = std::max(1, std::atoi(argv[++i]));
        else if (is("--frame-scale") && hasValue) opt.shotScale = std::max(1, std::atoi(argv[++i]));
        else if (is("--trace") && hasValue) opt.tracePath = argv[++i];
        else if (is("--cfg") && hasValue) opt.cfgOverrides.push_back(argv[++i]);
        else if (is("--user-config")) opt.userConfig = true;
        else if (is("--experiment") && hasValue) opt.experiment = std::atoi(argv[++i]);
        else if (is("--hours") && hasValue) opt.preHours = (float)std::atof(argv[++i]);
        else if (is("--tab") && hasValue) opt.tab = std::atoi(argv[++i]);
        else if (is("--preset") && hasValue) opt.preset = std::atoi(argv[++i]);
        else if (is("--track-type") && hasValue) opt.trackType = std::atoi(argv[++i]);
        else if (is("--tool") && hasValue) opt.tool = std::atoi(argv[++i]);
        else if (is("--challenge") && hasValue) opt.challenge = std::atoi(argv[++i]);
        else if (is("--screen") && hasValue) opt.screen = std::atoi(argv[++i]);
        else if (is("--zoom") && hasValue) opt.zoom = (float)std::atof(argv[++i]);
        else if (is("--speed") && hasValue) opt.speed = std::atoi(argv[++i]);
        else if (is("--genomes")) opt.genomeBrowser = true;
        else if (is("--experiments")) opt.expBrowser = true;
        else if (is("--color-chooser")) opt.colorChooser = true;
        else if (is("--open-cellab") && hasValue) opt.openCellLab = std::strcmp(argv[++i], "dish") == 0 ? 2 : 1;
        else if (is("--tick") && hasValue) opt.tick = std::atoi(argv[++i]);
        else if (is("--dock") && hasValue) opt.dock = std::atoi(argv[++i]);
        else if (is("--tool-side") && hasValue) opt.toolSide = std::atoi(argv[++i]);
        else if (is("--orient") && hasValue) opt.orient = std::atoi(argv[++i]);
        else if (is("--tools-open")) opt.toolsOpen = true;
        else if (is("--cam") && i + 2 < argc) {
            opt.camX = (float)std::atof(argv[++i]);
            opt.camY = (float)std::atof(argv[++i]);
            opt.camSet = true;
        }
        else if (is("--select-cell") && hasValue) opt.selectCell = std::atoi(argv[++i]);
        else if (is("--import") && hasValue) opt.importFile = argv[++i];
        else if (is("--mobile") && i + 3 < argc) {  // --mobile W H scale: the phone interface in a W x H window
            opt.mobileW = std::atoi(argv[++i]);
            opt.mobileH = std::atoi(argv[++i]);
            opt.mobileScale = (float)std::atof(argv[++i]);
        } else if (is("--cutout") && i + 4 < argc) {
            for (float& v : opt.cutout) v = (float)std::atof(argv[++i]);
        } else if (is("--safe") && i + 4 < argc) {  // --safe L T R B: a pretend camera cutout for --mobile
            for (float& v : opt.safe) v = (float)std::atof(argv[++i]);
        }
        else if (argv[i][0] != '-') opt.importFile = argv[i];  // file association / drag onto the exe
    }
    App app;
    if (!app.init(opt)) return 1;
    app.run();
    app.shutdown();
    return 0;
}
