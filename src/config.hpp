#pragma once
#include <string>

// Player settings (the "НАСТРОЙКИ" screen), kept in settings.ini next to the executable.
struct AppConfig {
    // Graphics
    bool fullscreen = false;
    bool vsync = true;
    int fpsLimit = 0;           // frames per second when vsync is off, 0 = unlimited
    float uiScale = 1.0f;       // interface size on top of the monitor's DPI scale
    int theme = 1;              // microscope: 1 = classic lavender, 0 = dark
    bool softBodies = true;     // squash-and-stretch outlines
    bool smoothMotion = true;   // interpolate between simulation ticks
    bool showSlime = true;      // adhesin slime in the microscope
    bool showFood = true;
    bool showGrid = true;
    float panelOpacity = 0.94f;

    // Controls
    float zoomSpeed = 1.0f;
    bool invertWheel = false;
    float panSpeed = 1.0f;       // WASD / arrow keys
    float cameraSmoothing = 0.5f;  // 0 = instant, 1 = very soft
    bool tooltips = true;
    bool debugStats = false;     // show timings (frame parts, simulation phases), FPS and counts over the dish

    // Simulation
    float slowTicksPerSecond = 30.0f;
    float observeTicksPerSecond = 120.0f;
    float simBudgetMs = 22.0f;   // per frame for catching up at the fixed speeds
    int incubateFps = 60;        // frames per second while incubating (the simulation thread gets the rest), 0 = no cap
    bool speedX2 = true;         // settings already carry the doubled speeds (older files are doubled once on load)
    int threads = 0;             // worker threads, 0 = automatic (applied on restart)
    bool pauseWhenMinimized = true;
    int autosaveMinutes = 0;     // 0 = off; writes substrates/autosave.substrate
    int challengesDone = 0;      // bit i = level i completed

    // Interface
    bool showHints = true;       // tool hint at the bottom of the microscope
    bool editorSimple = false;   // genome editor: beginner layout (hides programs and fine-tuning)
    bool editorTips = false;     // genome editor: the "how it works" card
    bool windowsLocked = false;  // panels cannot be dragged or resized
    int orientation = 0;         // Android: 0 = follow the device (auto-rotate), 1 = landscape, 2 = portrait
    int toolSide = 0;            // phone, portrait: the tool strip at the bottom (0), on the left (1) or on the right (2)
    // Window visibility
    bool winTools = true, winToolOptions = true, winTime = true, winCell = true, winStats = false;
    bool winGenomeBar = true, winModes = true, winMode = true, winTimeline = true;
};

bool loadConfig(AppConfig& c, const std::string& path);
bool saveConfig(const AppConfig& c, const std::string& path);
