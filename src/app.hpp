#pragma once
#include "config.hpp"
#include "core/jobs.hpp"
#include "render/renderer.hpp"
#include "render/vk_context.hpp"
#include "sim/layout.hpp"
#include "sim/world.hpp"
#include "ui.hpp"

#include "imgui.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct GLFWwindow;
struct ImFont;

struct AppOptions {
    bool validation = false;
    bool benchmark = false;
    double exitAfterSeconds = 0;
    bool showPerf = false;
    // Automation (testing / promo shots): set up a scene, capture one frame to a PNG and exit.
    std::string screenshotPath;
    double screenshotAfter = 5.0;
    int shotFrames = 1;          // --frames N: N frames in a row (name_0.png ...), to look for flicker
    std::string tracePath;       // --trace FILE: per frame, where the tracked cell is drawn and the camera (smoothness tests)
    int experiment = -1;
    float preHours = 0;  // simulate this many hours before the first frame
    int tab = -1;        // PanelTab to open
    int preset = -1;     // genome preset to load into the editor
    int trackType = -1;  // auto-track the first cell of this type
    int tool = -1;       // select this tool (Tool)
    int challenge = -1;  // start this level
    int screen = -1;     // open this screen (Screen)
    float zoom = 0;      // pixels per µm, 0 = fit dish
    int speed = -1;      // start at this Speed (0 freeze .. 3 incubate)
    bool genomeBrowser = false;  // open the genome browser
    bool expBrowser = false;     // open the experiment browser
    int tick = -1;               // genome editor: show this developmental tick
    int dock = -1;               // phone: open this tab of the genome sheet (or the microscope sheet)
    int toolSide = -1;           // phone: tool strip side (0 bottom, 1 left, 2 right), for automated captures
    int orient = -1;             // Android: orientation for automated captures (as cfg orientation; not saved)
    bool toolsOpen = false;      // phone, landscape: start with the tool drawer slid out (captures)
    bool colorChooser = false;   // open the first touch colour chooser (screenshots)
    int openCellLab = 0;         // 1 genome, 2 dish: hand it to Cell Lab / MinLab once the app runs (phone tests)
    float camX = 0, camY = 0;    // --cam: look at this point of the dish (µm)
    bool camSet = false;
    int selectCell = -1; // genome editor: open the action popup on this layout cell
    std::string importFile;  // open a .substrate / .genome at start
    // Desktop preview of the phone interface: window of mobileW x mobileH pixels at UI scale mobileScale.
    int mobileW = 0, mobileH = 0;
    float mobileScale = 2.0f;
    float safe[4] = {0, 0, 0, 0};  // --safe L T R B: pretend camera cutout (desktop test of the phone layout)
    float cutout[4] = {0, 0, 0, 0};  // --cutout X0 Y0 X1 Y1: a pretend punch-hole camera (desktop test)
};

class App {
public:
    bool init(const AppOptions& opt);
    void run();
    void shutdown();

private:
    // app.cpp
    void initImGui();
    void buildFonts();   // font atlas at the current UI scale
    void applyStyle();   // colours and sizes at the current UI scale
    void setFullscreen(bool on);
    void markConfigDirty() { cfgDirtyAt_ = time_; }
    void handleInput();
    void advanceSimulation(double frameDt);
    void updatePreview();
    void buildLists(const World& w, RenderLists& out, Vec2 viewMin, Vec2 viewMax, int selected, int hovered, float time);
    void prepareFrame();  // render lists from the world (under the world lock)
    void renderFrame();   // GPU work only
    // Incubation runs on its own thread: the frame locks the world only for input, UI and render lists.
    std::unique_lock<std::mutex> lockWorld();
    void simThreadLoop();
    std::thread simThread_;
    std::mutex simMutex_;
    enum SimMode { SimIdle, SimFixedRate, SimFlatOut };  // what the simulation thread does: nothing / slow + observe / incubate
    std::atomic<int> simMode_{SimIdle};
    std::atomic<double> simRate_{60.0};       // ticks per second at SimFixedRate
    std::atomic<double> lastTickTime_{0.0};   // nowSeconds() of the newest tick (render interpolation)
    std::atomic<bool> simQuit_{false}, mainWaiting_{false};
    std::atomic<int> simTicks_{0};
    std::atomic<int> simQuota_{0};  // slow / observe: ticks the frame has ordered and the thread has not done yet
    float lastAlpha_ = 0;           // the interpolation of the previous frame (never shown going backwards)
    Vec2 screenToWorld(double sx, double sy) const;
    void fitCamera(bool instant);
    void loadExperiment(int index);
    void toast(const std::string& msg);
    bool importPath(const std::string& path);  // Cell Lab / mod / own files by extension and signature
    // System "Open file" dialog; returns a UTF-8 path or "" when cancelled. filter: pairs "Name\0*.ext;*.ext\0" ending
    // in \0. Android: opens the system file picker and returns ""; the chosen file arrives later as a dropped file.
    std::string pickFile(const wchar_t* filter, const wchar_t* title);
    // Restart the current dish: the last loaded file, else the last experiment or level.
    void reloadSubstrate();
    std::string lastSubstratePath_;
    int lastExperiment_ = -1;
    std::vector<std::string> pendingDrops_;
    char importPathBuf_[512] = "";
    void saveCapture();
    int trackedCell() const;  // validated index of the tracked cell or -1
    void track(int idx);

    // ui.cpp
    void drawUi();
    void drawNavBar();
    void drawMicroscopeUi();
    void drawToolPalette();
    void drawToolOptions();
    void drawTimeControls();
    void drawSidePanel();
    void drawSubstrateScreen();
    void drawGenomeScreen();
    void drawGenomeToolbar();
    void drawModeList();
    void drawModePanel();
    void drawModeEditor(Mode& m);
    void drawModeEditorSimple(Mode& m);  // beginner layout
    void drawEditorTips();
    void drawTimeline();
    void drawEditorHandles();
    void drawCellTab();
    void drawStatsTab();
    void drawSettingsScreen();
    // ui_challenges.cpp
    void drawChallengesScreen();
    void drawChallengePanel();
    void startChallenge(int index);
    void updateChallenge();
    bool toolAllowed(Tool t) const;
    void drawWindowsMenu();
    // A movable, resizable panel window. Default placement applies on first use and after "reset layout".
    bool beginPanel(const char* title, bool* open, ImVec2 pos, ImVec2 size, ImVec2 pivot, int extraFlags = 0);
    void drawOverlays();
    void drawHelp();
    // ui_files.cpp: genome browser and sharing
    struct Thumb {
        float x, y, r;  // in the unit square of the thumbnail
        ImU32 col;
        int type;
        float angle;  // front direction (world radians)
    };
    struct GenomeEntry {
        std::string name, description, path;  // path: empty for built-in organisms
        int source = 0;                         // 0 built-in, 1 the player's (our format), 2 Cell Lab file
        Genome genome;
        std::vector<Thumb> thumb;
        bool previewReady = false;
        int cells = 0;
    };
    std::vector<GenomeEntry> gbEntries_;
    bool genomeBrowser_ = false, gbDirty_ = true, gbFocus_ = false;
    int gbTab_ = 0, gbSel_ = -1;
    Screen gbAfterOpen_ = Screen::Genome;  // where an opened genome takes the player
    char gbFilter_[64] = "";
    void openGenomeBrowser();
    void rebuildGenomeList();
    void buildGenomeThumb(GenomeEntry& e);
    void drawGenomeBrowser();
    void loadGenomeIntoEditor(const Genome& g);
    // Experiment browser: cards like the genome browser, each with a picture of the freshly set-up dish.
    struct ExpCard {
        std::vector<Thumb> dots;  // cells in the unit square of the dish (dish centre = 0.5, 0.5)
        std::vector<ImVec4> walls;  // barriers: (ax, ay, bx, by) in the same square
        std::vector<ImVec2> food;   // a sample of the food particles
        int cells = 0;
        bool ready = false;
    };
    std::vector<ExpCard> ebCards_;
    bool expBrowser_ = false, ebFocus_ = false;
    int ebSel_ = -1;
    char ebFilter_[64] = "";
    void openExperimentBrowser();
    void buildExperimentThumb(int i);
    void drawExperimentBrowser();
    // Hand a saved file to a messenger (Android share sheet) or show it in Explorer (desktop) with a short message.
    void shareFile(const std::string& path, const std::string& fileName, const std::string& what);
    enum class ShareAs { Auto, CellLab, CellFap };
    void shareGenome(const Genome& g, ShareAs how = ShareAs::Auto);  // see ui_files.cpp
    void genomeShareItems(const Genome& g);  // the share choices as menu items
    void dishShareItems();
    void shareDish(bool forCellLab = false);  // forCellLab: Cell Lab format 95 (opens in Cell Lab and MinLab)
    void openInCellLab(const Genome* genome);  // phone: hand the genome (nullptr: the dish) to Cell Lab / MinLab

    // ui_mobile.cpp: the phone / tablet interface (Android, or --mobile on the desktop). The panels are docked
    // instead of floating; one finger works the tools, drags scroll lists, two fingers pan and pinch.
    bool mobile_ = false;
    // Phone: screen edges covered by the camera cutout (left, top, right, bottom in ImGui units). The dish is drawn
    // under it, the interface keeps out.
    ImVec4 safe_{0, 0, 0, 0};
    std::vector<ImVec4> cutout_;  // the camera cutout itself (x0, y0, x1, y1 each): the tool rail keeps its icon clear
    bool railExclusion_ = false;  // this frame the tool rail claimed the screen edge from the system's back swipe
    float navBottom() const { return 46.0f * uiScale_ + (mobile_ ? safe_.y : 0.0f); }  // kNavH
    bool portrait_ = false;
    // Free part of the screen between the docked panels (dish hints, toasts, camera framing), ImGui coordinates.
    ImVec2 freeMin_{0, 0}, freeMax_{0, 0};
    struct Dock {
        int tab = 0;
        bool open = false;
        float frac = 0.36f;  // portrait: share of the free height the open sheet takes (dragged by its handle)
        float wfrac = 0.36f;  // landscape: share of the screen width (dragged by the left-edge handle)
        float anim = -1;      // landscape: slide of the column, 0 folded .. 1 out (-1: not yet drawn, starts in place)
    } dockMicro_, dockGenome_{1, true, 0.62f};  // the editor opens on the selected mode; its sheet is the main part of the screen
    // Docked sheet with tabs: bottom of the screen in portrait (above `bottomReserve`), right column in landscape.
    // Collapsed it shows only the tab strip; tapping the active tab folds it. Returns the tab to draw, -1 if folded.
    // minTop (portrait): the sheet never rises above this screen y, so it cannot cover the bars over it.
    int beginDock(const char* id, Dock& d, const char* const* labels, int count, float bottomReserve, bool roomy = false,
                  float minTop = 0);
    void endDock();
    void touchScroll();  // drag-to-scroll with inertia for every scrollable window
    void drawMobileNav();
    void drawMobileMicroscope();
    float drawMobileTimeBar(float x, float y, float w);  // returns its height
    ImVec2 dockPos_, dockSize_;  // where the last beginDock() put its window
    float dockInsetL_ = 0, dockInsetR_ = 0;  // portrait: screen edges the sheet leaves free (a side tool strip)
    const int* dockIcons_ = nullptr;          // landscape: tab icons for the folded rail (none: a strip of tabs)
    bool toolDrawerOpen_ = false;             // landscape: the tool drawer slid out over the dish
    float toolDrawerT_ = 0;                   // its slide: 0 folded .. 1 out
    bool toolDrawerDrag_ = false;             // a finger is pulling it (out or back): it follows the finger
    float toolDrawerDragFrom_ = 0;            // its slide when that pull started
    ImVec2 dishAreaMin_{0, 0}, dishAreaMax_{0, 0};  // microscope: the part of the screen the dish shows in (last frame)
    bool dockBodyOpen_ = false;
    void exportToDevice(const std::string& path, const std::string& name);  // Android: "save as" to the device
    void genomeNew();
    void genomeFromCell();
    void genomeAdultToDish();
    void drawGenomeFilePopups();
    // Tool strip: bottom (portrait), left (landscape) or a side chosen in the settings (portrait, cfg toolSide). It
    // reports the screen edges it takes; a side strip starts below the time bar (`barH`, measured last frame).
    void drawMobilePalette(float& reservedBottom, float& reservedLeft, float& reservedRight, float barH);
    void drawMobileGenome();
    void drawMobileTimeline();  // the genome editor's one-row timeline
    void drawToolOptionsBody(float width);
    void drawChallengeBody(float width);
    void drawModeListBody();
    void drawModePanelBody();
    bool drawCellActionsBody();  // genome editor: actions for the selected cell; false when there is nothing to show
    void drawTimelineBody(bool compact);
    void applyOrientation();
    // Columns that fit the width (screens with cards): at least 1, at most maxCols.
    int fitColumns(float avail, float minColW, int maxCols) const;

    // Genome editor actions (ui.cpp)
    void handleGenomeInput();
    void commitGenome();  // push an undo step if the genome changed since the last commit
    void undoGenome();
    void redoGenome();
    void splitSelected();      // make the selected cell's mode divide into two new child modes
    void stopSplitSelected();  // the selected mode never divides
    int allocFreeMode() const; // a mode index not used by the organism, or -1
    int selectedPreviewCell() const;
    void selectPreviewCell(int idx);
    Vec2 previewToScreen(Vec2 w) const;  // window (ImGui) coordinates
    Vec2 screenToPreview(Vec2 s) const;

    AppOptions opt_;
    GLFWwindow* window_ = nullptr;
    VkContext ctx_;
    Renderer renderer_;
    std::unique_ptr<JobPool> jobs_;
    std::unique_ptr<World> world_;
    std::unique_ptr<World> preview_;
    UiState ui_;
    RenderLists lists_, previewLists_;
    SceneParams scene_, previewScene_;
    ImFont* fontRegular_ = nullptr;
    ImFont* fontBold_ = nullptr;
    ImFont* fontSmall_ = nullptr;
    float uiScale_ = 1.0f;   // dpiScale_ * cfg_.uiScale
    float dpiScale_ = 1.0f;

    // Settings and window layout
    AppConfig cfg_;
    std::string cfgPath_, layoutPath_;
    double cfgDirtyAt_ = -1;      // save settings shortly after the last change
    bool fontsDirty_ = false;     // rebuild fonts / style before the next frame (UI scale changed)
    bool landscapeScaled_ = false;  // phone held sideways: the UI is drawn smaller (the screen is only ~400 dp tall)
    bool resetLayout_ = false;    // put every panel back to its default place this frame
    bool fullscreen_ = false;
    int windowedX_ = 100, windowedY_ = 100, windowedW_ = 1600, windowedH_ = 900;
    double lastAutosave_ = 0;

    // Soft-body outlines: per cell slot, 32 spokes with spring dynamics (render-side, frame-rate based).
    struct ShapeState {
        uint32_t id = 0;
        float r[kShapeSpokes];
        float v[kShapeSpokes];
        // Animation state kept per cell, so a change of activity changes the speed of a beat, not its phase (a phase of
        // time x rate jumped at every change: flagella, cilia and nerve impulses flickered).
        uint32_t animId = 0;
        float phase = 0;     // cycles
        float act = 0;       // smoothed activity
    };
    void updateShapes(const World& w, std::vector<ShapeState>& shapes, const std::vector<int>& cellsToShape, float alpha,
                      float dt, float pxPerUnit);
    std::vector<ShapeState> shapes_, previewShapes_;
    std::vector<int> visibleScratch_;
    float renderAlpha_ = 1.0f;  // interpolation factor between the last two ticks
    float frameDt_ = 1.0f / 60.0f;

    // Camera (world units are µm; zoom = pixels per µm)
    Vec2 cam_{0, 0}, camTarget_{0, 0};
    float zoom_ = 0.4f, zoomTarget_ = 0.4f;
    bool panning_ = false;
    float pinchSteps_ = 0;  // two-finger pinch this frame, in zoom steps of 1.18x (Android; bypasses ImGui)
    bool touchPan_ = false, touchMoved_ = false;  // phone inspector: one-finger drag in progress, moved past a tap
    double lastMouseX_ = 0, lastMouseY_ = 0;
    int dragCell_ = -1;
    struct ShockFx {
        Vec2 pos;
        float radius;
        double t0;
    };
    std::vector<ShockFx> shocks_;  // expanding flashes of the electroshock tool
    double lastInject_ = 0;
    bool mapDrawing_ = false;   // map editor stroke in progress
    Vec2 mapStart_, mapLast_;
    std::vector<int> cellsInCircle(Vec2 p, float r) const;
    bool removing_ = false;
    // Adhesin tool: a bridge being dragged from a cell / a cutting or gluing stroke.
    bool adhesinDrag_ = false;
    int adhesinFrom_ = -1;
    uint32_t adhesinFromId_ = 0;
    Vec2 adhesinStart_, adhesinLast_;
    double lastSparkSeen_ = 0;  // simulation time of the newest electrocyte flash already turned into an effect

    // Preview camera (genome screen draws the preview organism full-screen)
    Vec2 previewCam_{0, 0};
    float previewZoom_ = 2.0f;
    bool previewAutoFrame_ = true;
    bool previewPanning_ = false;
    bool previewWanted_ = false;
    uint32_t previewW_ = 0, previewH_ = 0;
    // Genome editor timeline: the layout is unfolded by stage; kTicksPerStage animation ticks between stages.
    static constexpr int kTicksPerStage = 24;
    GenomeLayout layout_;
    int editorTick_ = 0;
    int builtTick_ = -1;
    struct PreviewRef {
        int layoutIdx;   // index in layout_.stages[stage] (or stage+1 for phantoms)
        bool phantom;
    };
    std::vector<PreviewRef> previewMap_;  // preview world cell index -> layout cell
    int editorStage() const { return std::min(editorTick_ / kTicksPerStage, (int)layout_.stages.size() - 1); }
    int editorMaxTick() const { return std::max(0, ((int)layout_.stages.size() - 1) * kTicksPerStage); }
    void rebuildPreviewWorld();
    int pickPreview(Vec2 screenPos, bool* phantom) const;  // layout index under the cursor
    // Selected cell: a cell of the current stage, or a phantom (next-stage child) of the selected mode.
    int selCell_ = -1;
    bool selPhantom_ = false;
    bool editorPopup_ = false;
    Vec2 editorPopupPos_;
    int dragPhantom_ = -1;  // layout index (next stage) of the phantom being dragged
    enum class ActionKind { None, Divide, Mutate };
    struct LastAction {
        ActionKind kind = ActionKind::None;
        CellType type = CellType::Phagocyte;
        Color color;
        float relAngle = 0;  // divide: split angle relative to the cell
        bool keepType = true;
    } lastAction_;
    int themeClassic_ = 1;  // 1 = original lavender microscope, 0 = dark

    // Timing
    double simAccumulator_ = 0;
    int relaxFrames_ = 30;  // frozen dish: frames left to settle overlaps after a change (inserts, drags, a load)
    double time_ = 0;
    double fps_ = 60;
    double simMsPerFrame_ = 0;
    // Debug statistics (settings → «Отладка», the ` key): smoothed timings of the parts of a frame and of a tick.
    struct PerfStats {
        double waitMs = 0, uiMs = 0, listsMs = 0, renderMs = 0, frameMs = 0;
        int framesThisSecond = 0;
        double secondStart = 0, fpsCounted = 0;
    } perf_;
    std::atomic<double> stepMs_{0};  // the incubation thread: smoothed time of one world step
    double ticksPerSecond_ = 0;
    int ticksThisSecond_ = 0;
    int lastFrameTicks_ = 0;  // simulation ticks done since the last frame
    struct FlowTracer {
        Vec2 p;
        float age, life;
    };
    std::vector<FlowTracer> tracers_;  // streaks drifting with the water currents (visual only)
    double tickCounterStart_ = 0;
    double hoursPerSecond_ = 0;
    double lastHours_ = 0;
};
