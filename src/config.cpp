#include "config.hpp"

#include "sim/serialize.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// One table drives both reading and writing: key, type and address of every field.
enum class Kind { Bool, Int, Float, String };
struct Field {
    const char* key;
    Kind kind;
    void* ptr;
};

template <class F>
void forEachField(AppConfig& c, F&& f) {
    const Field fields[] = {
        {"fullscreen", Kind::Bool, &c.fullscreen},
        {"vsync", Kind::Bool, &c.vsync},
        {"fps_limit", Kind::Int, &c.fpsLimit},
        {"ui_scale", Kind::Float, &c.uiScale},
        {"theme", Kind::Int, &c.theme},
        {"soft_bodies", Kind::Bool, &c.softBodies},
        {"smooth_motion", Kind::Bool, &c.smoothMotion},
        {"show_slime", Kind::Bool, &c.showSlime},
        {"show_food", Kind::Bool, &c.showFood},
        {"show_grid", Kind::Bool, &c.showGrid},
        {"panel_opacity", Kind::Float, &c.panelOpacity},
        {"zoom_speed", Kind::Float, &c.zoomSpeed},
        {"invert_wheel", Kind::Bool, &c.invertWheel},
        {"pan_speed", Kind::Float, &c.panSpeed},
        {"camera_smoothing", Kind::Float, &c.cameraSmoothing},
        {"tooltips", Kind::Bool, &c.tooltips},
        {"sound", Kind::Bool, &c.sound},
        {"sound_volume", Kind::Float, &c.soundVolume},
        {"tool_sounds", Kind::Bool, &c.toolSounds},
        {"debug_stats", Kind::Bool, &c.debugStats},
        {"slow_tps", Kind::Float, &c.slowTicksPerSecond},
        {"observe_tps", Kind::Float, &c.observeTicksPerSecond},
        {"sim_budget_ms", Kind::Float, &c.simBudgetMs},
        {"incubate_fps", Kind::Int, &c.incubateFps},
        {"speed_x2", Kind::Bool, &c.speedX2},
        {"threads", Kind::Int, &c.threads},
        {"pause_minimized", Kind::Bool, &c.pauseWhenMinimized},
        {"autosave_minutes", Kind::Int, &c.autosaveMinutes},
        {"challenges_done", Kind::Int, &c.challengesDone},
        {"news_seen", Kind::Int, &c.newsSeen},
        {"glslc_path", Kind::String, &c.glslcPath},
        {"show_hints", Kind::Bool, &c.showHints},
        {"editor_simple_mode", Kind::Bool, &c.editorSimple},
        {"editor_tips_card", Kind::Bool, &c.editorTips},
        {"windows_locked", Kind::Bool, &c.windowsLocked},
        {"orientation", Kind::Int, &c.orientation},
        {"tool_side", Kind::Int, &c.toolSide},
        {"time_bar_scale", Kind::Float, &c.timeBarScale},
        {"time_bar_free", Kind::Bool, &c.timeBarFree},
        {"time_bar_x", Kind::Float, &c.timeBarX},
        {"time_bar_y", Kind::Float, &c.timeBarY},
        {"win_tools", Kind::Bool, &c.winTools},
        {"win_tool_options", Kind::Bool, &c.winToolOptions},
        {"win_time2", Kind::Bool, &c.winTime},  // (2: off by default since the speed buttons moved into the top bar)
        {"win_cell", Kind::Bool, &c.winCell},
        {"win_stats", Kind::Bool, &c.winStats},
        {"win_genome_bar", Kind::Bool, &c.winGenomeBar},
        {"win_modes", Kind::Bool, &c.winModes},
        {"win_mode", Kind::Bool, &c.winMode},
        {"win_timeline", Kind::Bool, &c.winTimeline},
    };
    for (const Field& fl : fields) f(fl);
}

}  // namespace

bool setConfigValue(AppConfig& c, const std::string& key, const std::string& val) {
    bool found = false;
    forEachField(c, [&](const Field& fl) {
        if (key != fl.key) return;
        found = true;
        switch (fl.kind) {
            case Kind::Bool: *(bool*)fl.ptr = std::atoi(val.c_str()) != 0; break;
            case Kind::Int: *(int*)fl.ptr = std::atoi(val.c_str()); break;
            case Kind::Float: *(float*)fl.ptr = (float)std::atof(val.c_str()); break;
            case Kind::String: *(std::string*)fl.ptr = val; break;
        }
    });
    return found;
}

bool loadConfig(AppConfig& c, const std::string& path) {
    FILE* f = openFileUtf8(path, "rb");
    if (!f) return false;
    char line[256];
    bool sawSpeedX2 = false;
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        char* key = line;
        while (*key == ' ') ++key;
        for (char* e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); --e) *e = 0;
        const char* val = eq + 1;
        if (std::strcmp(key, "speed_x2") == 0) sawSpeedX2 = true;
        forEachField(c, [&](const Field& fl) {
            if (std::strcmp(fl.key, key) != 0) return;
            switch (fl.kind) {
                case Kind::Bool: *(bool*)fl.ptr = std::atoi(val) != 0; break;
                case Kind::Int: *(int*)fl.ptr = std::atoi(val); break;
                case Kind::Float: *(float*)fl.ptr = (float)std::atof(val); break;
                case Kind::String: *(std::string*)fl.ptr = val; break;
            }
        });
    }
    std::fclose(f);
    if (!sawSpeedX2) {  // settings from before the speed-up: every speed runs twice as fast now
        c.slowTicksPerSecond *= 2.0f;
        c.observeTicksPerSecond *= 2.0f;
        c.simBudgetMs = std::min(c.simBudgetMs * 2.0f, 40.0f);
        c.speedX2 = true;
    }
    return true;
}

bool saveConfig(const AppConfig& cIn, const std::string& path) {
    AppConfig c = cIn;
    FILE* f = openFileUtf8(path, "wb");
    if (!f) return false;
    std::fputs("# CellFap settings (edited from the НАСТРОЙКИ screen)\n", f);
    forEachField(c, [&](const Field& fl) {
        switch (fl.kind) {
            case Kind::Bool: std::fprintf(f, "%s = %d\n", fl.key, *(bool*)fl.ptr ? 1 : 0); break;
            case Kind::Int: std::fprintf(f, "%s = %d\n", fl.key, *(int*)fl.ptr); break;
            case Kind::Float: std::fprintf(f, "%s = %g\n", fl.key, *(float*)fl.ptr); break;
            // Пустую строку не пишем: ключ с пустым значением значит «использовать путь из PATH».
            case Kind::String: {
                const std::string* s = (const std::string*)fl.ptr;
                if (s && !s->empty()) std::fprintf(f, "%s = %s\n", fl.key, s->c_str());
                break;
            }
        }
    });
    return std::fclose(f) == 0;
}
