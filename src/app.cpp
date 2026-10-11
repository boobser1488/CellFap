#include "app.hpp"
#include "audio.hpp"

#include "core/png.hpp"
#include "sim/cellab_import.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <commdlg.h>
#endif

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "ui_widgets.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <thread>

namespace {
// Cells are drawn this much larger than their physical radius: neighbours overlap a little and their outlines meet in
// a straight seam (Cell Lab look) instead of touching at a point with gaps around it.
constexpr float kDrawScale = 1.07f;
constexpr float kBg[4] = {0.045f, 0.052f, 0.066f, 1.0f};  // only visible before the dish pass

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void checkVk(VkResult r) {
    if (r < 0) vkCheck(r, "imgui_impl_vulkan");
}

void glfwError(int code, const char* msg) { std::fprintf(stderr, "GLFW %d: %s\n", code, msg); }
}  // namespace

// ---------------------------------------------------------------- init / shutdown
bool App::init(const AppOptions& opt) {
    opt_ = opt;
    const bool automated = !opt.screenshotPath.empty();
    cfgPath_ = dataDir("settings") + "/settings.ini";
    layoutPath_ = dataDir("settings") + "/layout.ini";
    if (!automated) loadConfig(cfg_, cfgPath_);  // automated captures always use the defaults
    glfwSetErrorCallback(glfwError);
    if (!glfwInit()) fatalError("Не удалось инициализировать GLFW");
    if (!glfwVulkanSupported()) fatalError("Vulkan не поддерживается этой системой (нужен драйвер с Vulkan 1.1).");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    if (automated) glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);  // don't steal focus from the user
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
    int w = vm ? (int)(vm->width * 0.8) : 1600;
    int h = vm ? (int)(vm->height * 0.8) : 900;
    if (opt.mobileW > 0 && opt.mobileH > 0) {
        w = opt.mobileW;
        h = opt.mobileH;
    }
#ifdef __ANDROID__
    mobile_ = true;
#else
    mobile_ = opt.mobileW > 0;
#endif
    window_ = glfwCreateWindow(w, h, "CellFap — песочница эволюции клеток", nullptr, nullptr);
    if (!window_) fatalError("Не удалось создать окно");
    if (vm) glfwSetWindowPos(window_, (vm->width - w) / 2, (vm->height - h) / 2);
    // A launcher may request a hidden/minimised start (STARTUPINFO); automated captures need a real frame.
    if (!opt.screenshotPath.empty()) {
        glfwRestoreWindow(window_);
        glfwShowWindow(window_);
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* wnd, int, int) {
        static_cast<App*>(glfwGetWindowUserPointer(wnd))->ctx_.requestResize();
    });
    // Drag & drop .substrate / .genome files (Cell Lab, the 127-mode mod, or our own) onto the window.
    glfwSetDropCallback(window_, [](GLFWwindow* wnd, int count, const char** paths) {
        App* app = static_cast<App*>(glfwGetWindowUserPointer(wnd));
        for (int i = 0; i < count; ++i) app->pendingDrops_.push_back(paths[i]);
    });

    if (!ctx_.init(window_, opt.validation)) fatalError("Не удалось инициализировать Vulkan: подходящий GPU не найден.");
#ifdef __ANDROID__
    // The app went to the background (window gone) and came back: rebuild the Vulkan surface and swapchain.
    glfwAndroidSetSurfaceCallbacks([](void* u) { static_cast<App*>(u)->ctx_.releaseSurface(); },
                                   [](void* u) { static_cast<App*>(u)->ctx_.restoreSurface(); }, this);
#endif
    if (ctx_.vsync() != cfg_.vsync) ctx_.setVsync(cfg_.vsync);
    renderer_.init(ctx_);
    initImGui();
    if (automated) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    if (cfg_.fullscreen && !automated && !mobile_) setFullscreen(true);
    applyOrientation();

    jobs_ = std::make_unique<JobPool>((unsigned)std::max(0, cfg_.threads));
    world_ = std::make_unique<World>(jobs_.get());
    preview_ = std::make_unique<World>(jobs_.get());
    preview_->previewMode = true;

    ui_.showPerf = opt.showPerf;
    ui_.editGenome = genomePresets()[3].genome;  // swimmer: shows off adhesion + flagellum
    loadExperiment(opt.experiment >= 0 ? opt.experiment : 3);
    if (opt.preset >= 0 && opt.preset < (int)genomePresets().size()) {
        ui_.editGenome = genomePresets()[opt.preset].genome;
        editorTick_ = opt.tick >= 0 ? opt.tick : 1 << 20;  // clamped to the grown organism
    }
    if (!opt.importFile.empty()) importPath(opt.importFile);
    for (float h = 0; h < opt.preHours; h += kHoursPerTick) world_->step();
    if (opt.tab >= 0 && opt.tab <= 3) ui_.screen = (Screen)opt.tab;
    if (opt.challenge >= 0) startChallenge(opt.challenge);
    if (opt.tool >= 0 && opt.tool < (int)Tool::Count) ui_.tool = (Tool)opt.tool;
    if (opt.screen >= 0) ui_.screen = (Screen)opt.screen;
    if (opt.trackType >= 0) {
        for (size_t i = 0; i < world_->cells().size(); ++i)
            if (world_->cells()[i].alive && (int)world_->cells()[i].type == opt.trackType) {
                track((int)i);
                ui_.follow = true;
                dockMicro_ = {1, true};  // phone: show the tracked cell
                break;
            }
    }
    fitCamera(true);
    if (opt.zoom > 0) zoom_ = zoomTarget_ = opt.zoom;
    if (opt.camSet) cam_ = camTarget_ = {opt.camX, opt.camY};
    if (opt.selectCell >= 0) {
        selCell_ = opt.selectCell;
        editorPopup_ = true;
        editorPopupPos_ = {w * 0.5f, h * 0.45f};
    }
    tickCounterStart_ = nowSeconds();
    lastAutosave_ = nowSeconds();
    if (opt.speed >= 0 && opt.speed <= 3) ui_.speed = (Speed)opt.speed;
    if (opt.genomeBrowser) openGenomeBrowser();
    if (opt.expBrowser) openExperimentBrowser();
    uiw::gTestOpenChooser = opt.colorChooser;
    if (opt.toolSide >= 0) cfg_.toolSide = std::min(opt.toolSide, 2);
    toolDrawerOpen_ = opt.toolsOpen;
    if (opt.dock >= 0) {
        dockGenome_.tab = dockMicro_.tab = opt.dock;
        dockGenome_.open = dockMicro_.open = true;
    }
    simThread_ = std::thread([this] { simThreadLoop(); });  // incubation (idle until Speed::Incubate)
    return true;
}

// Full screen on the monitor the window is on (F11); the windowed size and place are restored.
void App::setFullscreen(bool on) {
    if (on == fullscreen_) return;
    if (on) {
        glfwGetWindowPos(window_, &windowedX_, &windowedY_);
        glfwGetWindowSize(window_, &windowedW_, &windowedH_);
        int count = 0;
        GLFWmonitor** mons = glfwGetMonitors(&count);
        GLFWmonitor* best = glfwGetPrimaryMonitor();
        int cx = windowedX_ + windowedW_ / 2, cy = windowedY_ + windowedH_ / 2;
        for (int i = 0; i < count; ++i) {
            int mx, my;
            glfwGetMonitorPos(mons[i], &mx, &my);
            const GLFWvidmode* m = glfwGetVideoMode(mons[i]);
            if (m && cx >= mx && cx < mx + m->width && cy >= my && cy < my + m->height) best = mons[i];
        }
        const GLFWvidmode* m = glfwGetVideoMode(best);
        if (!m) return;
        glfwSetWindowMonitor(window_, best, 0, 0, m->width, m->height, m->refreshRate);
    } else {
        glfwSetWindowMonitor(window_, nullptr, windowedX_, windowedY_, windowedW_, windowedH_, 0);
    }
    fullscreen_ = on;
    cfg_.fullscreen = on;
    ctx_.requestResize();
}

void App::buildFonts() {
    ImGuiIO& io = ImGui::GetIO();
    cfg_.uiScale = std::clamp(cfg_.uiScale, 0.6f, 2.0f);
    // A phone held sideways has little height: the same sizes would leave the dish a strip between big bars.
    uiScale_ = dpiScale_ * cfg_.uiScale * (landscapeScaled_ ? 0.82f : 1.0f);
    io.Fonts->Clear();
    // Segoe UI covers Cyrillic; fall back to ImGui's built-in font when unavailable.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0400, 0x052F, 0x2010, 0x205E, 0x2190, 0x21FF, 0x2200, 0x22FF,
                                     0x25A0, 0x25FF, 0};
    // A list of candidate files ('|'-separated): the first one that exists is used.
    auto load = [&](const char* files, float size) -> ImFont* {
#ifdef __ANDROID__
        const std::string dir = "/system/fonts/";
#else
        const std::string dir = "C:/Windows/Fonts/";
#endif
        std::string list = files;
        for (size_t a = 0; a <= list.size();) {
            size_t b = list.find('|', a);
            if (b == std::string::npos) b = list.size();
            std::string path = dir + list.substr(a, b - a);
            a = b + 1;
            if (!std::filesystem::exists(path)) continue;
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.PixelSnapH = false;
            return io.Fonts->AddFontFromFileTTF(path.c_str(), size * uiScale_, &cfg, ranges);
        }
        return nullptr;
    };
#ifdef __ANDROID__
    const char* regular = "Roboto-Regular.ttf|NotoSans-Regular.ttf|DroidSans.ttf";
    const char* bold = "Roboto-Medium.ttf|Roboto-Bold.ttf|NotoSans-Bold.ttf|DroidSans-Bold.ttf";
#else
    const char* regular = "segoeui.ttf";
    const char* bold = "seguisb.ttf";
#endif
    fontRegular_ = load(regular, 16.0f);
    if (!fontRegular_) fontRegular_ = io.Fonts->AddFontDefault();
    fontBold_ = load(bold, 16.0f);
    if (!fontBold_) fontBold_ = fontRegular_;
    fontSmall_ = load(regular, mobile_ ? 14.5f : 13.5f);  // phone: hints must stay readable at arm's length
    if (!fontSmall_) fontSmall_ = fontRegular_;
}

void App::applyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    st.WindowRounding = 12;
    st.ChildRounding = 8;
    st.FrameRounding = 6;
    st.PopupRounding = 8;
    st.GrabRounding = 6;
    st.TabRounding = 6;
    st.ScrollbarRounding = 6;
    st.WindowBorderSize = 1;
    st.FrameBorderSize = 0;
    st.PopupBorderSize = 1;
    st.WindowPadding = {14, 12};
    st.FramePadding = {8, 5};
    st.ItemSpacing = {8, 7};
    st.ItemInnerSpacing = {6, 5};
    st.ScrollbarSize = 10;
    st.GrabMinSize = 10;
    st.SeparatorTextBorderSize = 1;
    st.WindowTitleAlign = {0.5f, 0.5f};
    if (mobile_) {
        // Finger-sized targets; thin scrollbars (lists scroll by dragging the content).
        st.WindowRounding = 10;
        st.WindowPadding = {12, 10};
        st.FramePadding = {10, 10};
        st.ItemSpacing = {10, 9};
        st.ItemInnerSpacing = {8, 6};
        st.ScrollbarSize = 5;
        st.GrabMinSize = 26;
        st.TouchExtraPadding = {3, 3};
    }

    // Palette of the original game: charcoal panels, green dividers, orange accents, white text.
    ImVec4* c = st.Colors;
    const ImVec4 accent(1.00f, 0.62f, 0.20f, 1.0f);
    const ImVec4 green(0.43f, 0.75f, 0.47f, 1.0f);
    c[ImGuiCol_Text] = {0.93f, 0.93f, 0.94f, 1};
    c[ImGuiCol_TextDisabled] = {0.58f, 0.59f, 0.62f, 1};
    c[ImGuiCol_WindowBg] = {0.118f, 0.120f, 0.128f, cfg_.panelOpacity};
    c[ImGuiCol_ChildBg] = {0, 0, 0, 0};
    c[ImGuiCol_PopupBg] = {0.135f, 0.137f, 0.146f, 0.98f};
    c[ImGuiCol_Border] = {green.x, green.y, green.z, 0.22f};
    c[ImGuiCol_FrameBg] = {1, 1, 1, 0.06f};
    c[ImGuiCol_FrameBgHovered] = {1, 1, 1, 0.10f};
    c[ImGuiCol_FrameBgActive] = {1, 1, 1, 0.13f};
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = {0.14f, 0.145f, 0.155f, cfg_.panelOpacity};
    c[ImGuiCol_TitleBgActive] = {0.19f, 0.17f, 0.15f, std::min(1.0f, cfg_.panelOpacity + 0.04f)};
    c[ImGuiCol_ScrollbarBg] = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab] = {1, 1, 1, 0.12f};
    c[ImGuiCol_ScrollbarGrabHovered] = {1, 1, 1, 0.18f};
    c[ImGuiCol_ScrollbarGrabActive] = {accent.x, accent.y, accent.z, 0.6f};
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = {accent.x, accent.y, accent.z, 0.9f};
    c[ImGuiCol_SliderGrabActive] = accent;
    c[ImGuiCol_Button] = {1, 1, 1, 0.07f};
    c[ImGuiCol_ButtonHovered] = {1, 1, 1, 0.13f};
    c[ImGuiCol_ButtonActive] = {accent.x, accent.y, accent.z, 0.40f};
    c[ImGuiCol_Header] = {1, 1, 1, 0.05f};
    c[ImGuiCol_HeaderHovered] = {1, 1, 1, 0.10f};
    c[ImGuiCol_HeaderActive] = {accent.x, accent.y, accent.z, 0.30f};
    c[ImGuiCol_Separator] = {green.x, green.y, green.z, 0.35f};
    c[ImGuiCol_SeparatorHovered] = {accent.x, accent.y, accent.z, 0.5f};
    c[ImGuiCol_SeparatorActive] = accent;
    c[ImGuiCol_ResizeGrip] = {1, 1, 1, 0.06f};
    c[ImGuiCol_ResizeGripHovered] = {accent.x, accent.y, accent.z, 0.45f};
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = {1, 1, 1, 0.03f};
    c[ImGuiCol_TabHovered] = {1, 1, 1, 0.10f};
    c[ImGuiCol_TabSelected] = {accent.x, accent.y, accent.z, 0.22f};
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_PlotLines] = accent;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TextSelectedBg] = {accent.x, accent.y, accent.z, 0.30f};
    c[ImGuiCol_NavCursor] = {accent.x, accent.y, accent.z, 0.6f};
    st.ScaleAllSizes(uiScale_);
}

void App::initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Window layout persists between runs (not for automated captures, which must look the same every time).
    io.IniFilename = opt_.screenshotPath.empty() ? layoutPath_.c_str() : nullptr;
    io.ConfigWindowsMoveFromTitleBarOnly = true;  // dragging a slider must not drag its panel

    float xs = 1, ys = 1;
    glfwGetWindowContentScale(window_, &xs, &ys);
    dpiScale_ = std::clamp(xs, 1.0f, 3.0f);
    if (opt_.mobileW > 0) dpiScale_ = opt_.mobileScale;  // desktop preview of a phone screen
    if (mobile_) {
        io.ConfigWindowsResizeFromEdges = false;
        io.ConfigDragClickToInputText = false;
        io.MouseDragThreshold = 10.0f * dpiScale_;  // a finger never lands perfectly still
    }
    buildFonts();
    // Sounds (silent in automated captures and without a sound device).
    if (opt_.screenshotPath.empty()) audio::init();
    audio::setVolume(cfg_.sound ? cfg_.soundVolume : 0.0f);
    audio::setToolSounds(cfg_.toolSounds);
    applyStyle();

    ImGui_ImplGlfw_InitForVulkan(window_, true);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.Instance = ctx_.instance();
    ii.PhysicalDevice = ctx_.physicalDevice();
    ii.Device = ctx_.device();
    ii.QueueFamily = ctx_.queueFamily();
    ii.Queue = ctx_.queue();
    ii.DescriptorPool = ctx_.descriptorPool();
    ii.RenderPass = ctx_.renderPass();
    ii.MinImageCount = ctx_.minImageCount();
    ii.ImageCount = std::max(ctx_.imageCount(), ctx_.minImageCount());
    ii.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = checkVk;
    ImGui_ImplVulkan_Init(&ii);
}

void App::shutdown() {
    if (!window_) return;
    simQuit_ = true;
    if (simThread_.joinable()) simThread_.join();
    audio::shutdown();
    if (opt_.screenshotPath.empty()) saveConfig(cfg_, cfgPath_);
    vkDeviceWaitIdle(ctx_.device());
    renderer_.shutdown();  // removes the preview texture: needs ImGui alive
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    world_.reset();
    preview_.reset();
    jobs_.reset();
    ctx_.shutdown();
    glfwDestroyWindow(window_);
    glfwTerminate();
    window_ = nullptr;
}

// ---------------------------------------------------------------- helpers
std::string App::pickFile(const wchar_t* filter, const wchar_t* title) {
#ifdef _WIN32
    wchar_t buf[4096] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = glfwGetWin32Window(window_);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(n - 1, 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
#elif defined(__ANDROID__)
    (void)filter;
    (void)title;
    glfwAndroidPickFile();  // asynchronous: the file comes back through the drop callback
    return "";
#else
    (void)filter;
    (void)title;
    return "";
#endif
}

void App::reloadSubstrate() {
    if (ui_.challenge >= 0) {
        startChallenge(ui_.challenge);
        toast("Задание начато заново");
    } else if (!lastSubstratePath_.empty()) {
        std::string p = lastSubstratePath_;  // importPath overwrites it
        if (importPath(p)) toast("Чашка перезагружена из файла");
    } else if (lastExperiment_ >= 0) {
        loadExperiment(lastExperiment_);
        toast(std::string("Эксперимент перезапущен: ") + experiments()[lastExperiment_].name);
    } else {
        toast("Перезагружать нечего: сначала загрузите чашку или эксперимент");
    }
}

void App::toast(const std::string& msg) {
    ui_.toast = msg;
    ui_.toastUntil = time_ + 2.5;
}

bool App::importPath(const std::string& pathIn) {
    std::string path = pathIn;
    // Strip quotes pasted from Explorer ("Copy as path").
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
    auto endsWith = [&](const char* suf) {
        size_t n = std::strlen(suf);
        if (path.size() < n) return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char)path[path.size() - n + i]) != suf[i]) return false;
        return true;
    };
    std::string err;
    // The content decides first: files from messengers often arrive renamed. Our own formats carry a signature;
    // anything else is a Cell Lab file, told apart by the extension (or tried as both when there is none).
    char head[16] = {};
    if (FILE* f = openFileUtf8(path, "rb")) {
        std::fread(head, 1, sizeof(head) - 1, f);
        std::fclose(f);
    } else {
        toast("Не удалось открыть файл");
        return false;
    }
    const bool ownGenome = std::strncmp(head, "CellFapGenome", 13) == 0;
    const bool ownDish = std::memcmp(head, "CFAPSUB", 7) == 0;
    if (!ownGenome && !ownDish && !endsWith(".genome") && !endsWith(".genome.txt") && !endsWith(".substrate")) {
        Genome g;
        if (importCellLabGenome(g, path, err)) {
            commitGenome();
            ui_.editGenome = g;
            std::snprintf(ui_.genomeFile, sizeof(ui_.genomeFile), "%s", safeFileName(g.name, "геном").c_str());  // Save keeps this name
            ui_.editMode = g.initialMode;
            editorTick_ = 1 << 20;
            previewAutoFrame_ = true;
            ui_.previewDirty = true;
            ui_.screen = Screen::Genome;
            toast("Геном Cell Lab импортирован: " + g.name + (err.empty() ? "" : " (" + err + ")"));
            return true;
        }
        CellLabImportInfo info;
        if (!importCellLabSubstrate(*world_, path, info, err)) {
            toast("Файл не похож ни на чашку, ни на геном");
            return false;
        }
        ui_.challenge = -1;
        track(-1);
        fitCamera(false);
        ui_.screen = Screen::Microscope;
        lastSubstratePath_ = path;
        lastExperiment_ = -1;
        toast("Чашка Cell Lab загружена");
        return true;
    }
    if (ownGenome || (endsWith(".genome.txt") && !ownDish)) {
        Genome g;
        if (!loadGenome(g, path)) {
            toast("Не удалось прочитать геном");
            return false;
        }
        commitGenome();
        ui_.editGenome = g;
        std::snprintf(ui_.genomeFile, sizeof(ui_.genomeFile), "%s", safeFileName(g.name, "геном").c_str());  // Save keeps this name
        ui_.editMode = g.initialMode;
        editorTick_ = 1 << 20;
        ui_.previewDirty = true;
        ui_.screen = Screen::Genome;
        toast("Геном загружен: " + g.name);
        return true;
    }
    if (endsWith(".genome") && !ownDish) {
        Genome g;
        if (!importCellLabGenome(g, path, err)) {
            toast("Геном Cell Lab: " + err);
            return false;
        }
        commitGenome();
        ui_.editGenome = g;
        std::snprintf(ui_.genomeFile, sizeof(ui_.genomeFile), "%s", safeFileName(g.name, "геном").c_str());  // Save keeps this name
        ui_.editMode = g.initialMode;
        editorTick_ = 1 << 20;
        previewAutoFrame_ = true;
        ui_.previewDirty = true;
        ui_.screen = Screen::Genome;
        toast("Геном Cell Lab импортирован: " + g.name + (err.empty() ? "" : " (" + err + ")"));
        return true;
    }
    if (ownDish || endsWith(".substrate")) {
        // Our own snapshots start with "CFAPSUB"; anything else is treated as a Cell Lab file.
        const bool ours = ownDish;
        if (ours) {
            if (!loadWorld(*world_, path.c_str())) {
                toast("Чашка CellFap повреждена или сохранена несовместимой версией игры");
                return false;
            }
            track(-1);
            fitCamera(false);
            ui_.screen = Screen::Microscope;
            ui_.challenge = -1;  // a loaded dish is free play
            lastSubstratePath_ = path;
            lastExperiment_ = -1;
            toast("Чашка загружена");
            return true;
        }
        CellLabImportInfo info;
        if (!importCellLabSubstrate(*world_, path, info, err)) {
            toast("Субстрат Cell Lab: " + err);
            return false;
        }
        ui_.challenge = -1;
        track(-1);
        fitCamera(false);
        ui_.screen = Screen::Microscope;
        if (ui_.speed == Speed::Freeze) ui_.speed = Speed::Observe;  // the imported dish comes alive right away
        lastSubstratePath_ = path;
        lastExperiment_ = -1;
        char buf[256];
        std::snprintf(buf, sizeof(buf), "Импорт Cell Lab (v%d%s%s): %d клеток, %d пищи, %d геномов", info.version,
                      info.note.empty() ? "" : ", ", info.note.c_str(), info.cells, info.food, info.genomes);
        toast(buf);
        ui_.toastUntil = time_ + 5.0;
        return true;
    }
    toast("Неизвестный тип файла (нужен .substrate или .genome)");
    return false;
}

void App::saveCapture() {
    std::vector<uint8_t> rgb;
    uint32_t w, h;
    if (!ctx_.takeCapture(rgb, w, h)) return;
    std::string path = opt_.screenshotPath;
    bool automated = !path.empty();
    if (automated && opt_.shotFrames > 1) {  // a series: name_0.png, name_1.png, ... of consecutive frames
        // Kept in memory and written at the end, so the frames follow each other at the real frame rate.
        static std::vector<std::vector<uint8_t>> series;
        series.push_back(std::move(rgb));
        if ((int)series.size() < opt_.shotFrames) {
            ctx_.requestCapture();
            return;
        }
        const size_t dot = path.rfind('.');
        for (size_t k = 0; k < series.size(); ++k) {
            std::string p = path.substr(0, dot) + "_" + std::to_string(k) + (dot == std::string::npos ? "" : path.substr(dot));
            if (FILE* f = openFileUtf8(p, "wb")) {
                writePngRgb(f, w, h, series[k].data());
                std::fclose(f);
            }
        }
        glfwSetWindowShouldClose(window_, 1);
        return;
    }
    if (!automated) {
        char name[64];
        std::time_t t = std::time(nullptr);
        std::strftime(name, sizeof(name), "/cellfap_%Y%m%d_%H%M%S.png", std::localtime(&t));
        path = dataDir("screenshots") + name;
    }
    FILE* f = openFileUtf8(path, "wb");
    bool ok = f && writePngRgb(f, w, h, rgb.data());
    if (f) std::fclose(f);
    if (automated) glfwSetWindowShouldClose(window_, 1);
    else toast(ok ? "Снимок сохранён в папку screenshots" : "Не удалось сохранить снимок");
}

std::vector<int> App::cellsInCircle(Vec2 p, float r) const {
    std::vector<int> out;
    world_->forEachCellNear(p, r + 20.0f, [&](int j) {
        const Cell& c = world_->cells()[j];
        if (length(c.pos - p) < r + c.radius * 0.5f) out.push_back(j);
    });
    return out;
}

int App::trackedCell() const {
    int i = ui_.trackedIdx;
    if (i < 0 || i >= (int)world_->cells().size()) return -1;
    const Cell& c = world_->cells()[i];
    return (c.alive && c.id == ui_.trackedId) ? i : -1;
}

void App::track(int idx) {
    ui_.trackedIdx = idx;
    ui_.trackedId = idx >= 0 ? world_->cells()[idx].id : 0;
    if (idx < 0) ui_.follow = false;
}

void App::loadExperiment(int index) {
    const auto& exps = experiments();
    index = std::clamp(index, 0, (int)exps.size() - 1);
    if (ui_.challenge >= 0) {
        ui_.challenge = -1;  // a new dish ends the level
        toast("Задание прервано: загружена новая чашка.");
    }
    Rng rng((uint64_t)(nowSeconds() * 1000.0) | 1);
    exps[index].setup(*world_, rng);
    lastExperiment_ = index;
    lastSubstratePath_.clear();
    track(-1);
    dragCell_ = -1;
    fitCamera(false);
}

Vec2 App::screenToWorld(double sx, double sy) const {
    VkExtent2D e = ctx_.extent();
    int ww, wh;
    glfwGetWindowSize(window_, &ww, &wh);
    // Mouse coordinates are in window units; the framebuffer may be scaled.
    double fx = sx * (double)e.width / std::max(ww, 1);
    double fy = sy * (double)e.height / std::max(wh, 1);
    return {cam_.x + (float)((fx - e.width * 0.5) / zoom_), cam_.y - (float)((fy - e.height * 0.5) / zoom_)};
}

void App::fitCamera(bool instant) {
    VkExtent2D e = ctx_.extent();
    float R = world_->settings.dishRadius;
    if (mobile_ && freeMax_.x > freeMin_.x + 50 && freeMax_.y > freeMin_.y + 50) {
        // Phone: the dish fills the free area between the docked panels.
        float fw = freeMax_.x - freeMin_.x, fh = freeMax_.y - freeMin_.y;
        zoomTarget_ = std::min(fw, fh) / (2.1f * R);
        float cx = (freeMin_.x + freeMax_.x) * 0.5f, cy = (freeMin_.y + freeMax_.y) * 0.5f;
        camTarget_ = {-(cx - e.width * 0.5f) / zoomTarget_, (cy - e.height * 0.5f) / zoomTarget_};
        if (instant) {
            zoom_ = zoomTarget_;
            cam_ = camTarget_;
        }
        return;
    }
    float panel = (cfg_.winCell && ui_.screen == Screen::Microscope) ? 380.0f * uiScale_ : 0.0f;  // default cell window
    float usable = std::max(200.0f, (float)e.width - panel);
    float usableH = std::max(200.0f, (float)e.height - 70.0f * uiScale_);  // nav bar
    zoomTarget_ = std::min(usable, usableH) / (2.15f * R);
    camTarget_ = {panel * 0.5f / zoomTarget_, 35.0f * uiScale_ / zoomTarget_};
    if (instant) {
        zoom_ = zoomTarget_;
        cam_ = camTarget_;
    }
}

// ---------------------------------------------------------------- input
void App::handleInput() {
    ImGuiIO& io = ImGui::GetIO();
    if (!opt_.screenshotPath.empty()) return;  // automated capture: ignore whatever the real mouse does
    // Global shortcuts
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) ui_.showHelp = !ui_.showHelp;
        if (ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) ui_.showPerf = !ui_.showPerf;
        if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) ui_.screen = Screen::Substrate;
        if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) ui_.screen = Screen::Microscope;
        if (ImGui::IsKeyPressed(ImGuiKey_F4, false)) ui_.screen = Screen::Genome;
        if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) ui_.screen = Screen::Settings;
        if (ImGui::IsKeyPressed(ImGuiKey_F6, false)) ui_.screen = Screen::Challenges;
        if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) {
            setFullscreen(!fullscreen_);
            markConfigDirty();
        }
    }
    if (ui_.screen == Screen::Genome) {
        ui_.hoveredIdx = -1;
        handleGenomeInput();
        return;
    }
    if (ui_.screen != Screen::Microscope) {
        ui_.hoveredIdx = -1;
        panning_ = false;
        dragCell_ = -1;
        world_->dragCell(-1, {});
        return;
    }

    double mx, my;
    glfwGetCursorPos(window_, &mx, &my);
    Vec2 mouseWorld = screenToWorld(mx, my);
    bool overUi = io.WantCaptureMouse;

    // Hover
    ui_.hoveredIdx = overUi || mobile_ ? -1 : world_->pickCell(mouseWorld);  // a finger does not hover

    // Keyboard (only when no text field is active)
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
            if (ui_.speed == Speed::Freeze) ui_.speed = ui_.speedBeforeFreeze;
            else {
                ui_.speedBeforeFreeze = ui_.speed;
                ui_.speed = Speed::Freeze;
            }
        }
        const ImGuiKey toolKeys[] = {ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
                                     ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9, ImGuiKey_0};
        for (int i = 0; i < (int)Tool::Count; ++i)
            if (ImGui::IsKeyPressed(toolKeys[i], false) && toolAllowed((Tool)i)) ui_.tool = (Tool)i;
        // Brush radius
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) ui_.brushRadius = std::max(5.0f, ui_.brushRadius / 1.15f);
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) ui_.brushRadius = std::min(500.0f, ui_.brushRadius * 1.15f);
        if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) cfg_.winCell = !cfg_.winCell;
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) && trackedCell() >= 0) ui_.follow = !ui_.follow;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) fitCamera(false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            track(-1);
            ui_.showHelp = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && trackedCell() >= 0 && toolAllowed(Tool::Remove)) {
            world_->removeCell(trackedCell(), false);
            track(-1);
        }
        // Keyboard panning
        float pan = 600.0f * cfg_.panSpeed / zoom_ * io.DeltaTime;
        if (ImGui::IsKeyDown(ImGuiKey_W) || ImGui::IsKeyDown(ImGuiKey_UpArrow)) camTarget_.y += pan;
        if (ImGui::IsKeyDown(ImGuiKey_S) || ImGui::IsKeyDown(ImGuiKey_DownArrow)) camTarget_.y -= pan;
        if (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_LeftArrow)) camTarget_.x -= pan;
        if (ImGui::IsKeyDown(ImGuiKey_D) || ImGui::IsKeyDown(ImGuiKey_RightArrow)) camTarget_.x += pan;
    }

    // Ctrl + wheel over the dish: brush radius instead of zoom.
    if (!overUi && io.MouseWheel != 0 && io.KeyCtrl) {
        ui_.brushRadius = std::clamp(ui_.brushRadius * std::pow(1.15f, io.MouseWheel), 5.0f, 500.0f);
    } else if (!overUi && (io.MouseWheel != 0 || pinchSteps_ != 0)) {  // zoom around the cursor (or the pinch centre)
        float wheel = (cfg_.invertWheel && !mobile_ ? -io.MouseWheel : io.MouseWheel) + pinchSteps_;
        // A pinch comes as steps of exactly 1.18x (glfwAndroidTakePinch): the dish follows the fingers.
        float factor = std::pow(mobile_ ? 1.18f : 1.0f + 0.18f * cfg_.zoomSpeed, wheel);
        float R = world_->settings.dishRadius;
        VkExtent2D e = ctx_.extent();
        float minZoom = std::min(e.width, e.height) / (6.0f * R);
        float newZoom = std::clamp(zoomTarget_ * factor, minZoom, 60.0f);
        // Keep the world point under the cursor fixed with respect to the target camera.
        int ww, wh;
        glfwGetWindowSize(window_, &ww, &wh);
        float sx = (float)e.width / std::max(ww, 1);
        Vec2 off{((float)mx * sx - e.width * 0.5f), -((float)my * sx - e.height * 0.5f)};  // pixels from centre
        Vec2 pw = camTarget_ + off / zoomTarget_;
        camTarget_ = pw - off / newZoom;
        zoomTarget_ = newZoom;
        if (mobile_) {  // pinching: no camera smoothing, the dish sticks to the fingers
            zoom_ = zoomTarget_;
            cam_ = camTarget_;
        }
        ui_.follow = false;
    }

    // Pan with right / middle button.
    bool panBtn = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                  glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    if (panBtn && (panning_ || !overUi)) {
        if (panning_) {
            VkExtent2D e = ctx_.extent();
            int ww, wh;
            glfwGetWindowSize(window_, &ww, &wh);
            float sx = (float)e.width / std::max(ww, 1);
            Vec2 d{(float)(mx - lastMouseX_) * sx / zoom_, -(float)(my - lastMouseY_) * sx / zoom_};
            camTarget_ -= d;
            cam_ -= d;
            ui_.follow = false;
        }
        panning_ = true;
    } else {
        panning_ = false;
    }
    // Phone, inspector: one finger drags the dish; a tap without dragging selects the cell under it (or close to it).
    const bool lmbDownNow = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    if (mobile_ && ui_.tool == Tool::Inspect) {
        if (panBtn) touchMoved_ = true;  // a second finger turned the touch into a pinch
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overUi) {
            touchPan_ = true;
            touchMoved_ = false;
        }
        if (touchPan_ && lmbDownNow) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) touchMoved_ = true;
            if (touchMoved_) {
                VkExtent2D e = ctx_.extent();
                int ww, wh;
                glfwGetWindowSize(window_, &ww, &wh);
                float sx = (float)e.width / std::max(ww, 1);
                Vec2 d{(float)(mx - lastMouseX_) * sx / zoom_, -(float)(my - lastMouseY_) * sx / zoom_};
                camTarget_ -= d;
                cam_ -= d;
                ui_.follow = false;
            }
        } else if (touchPan_) {
            if (!touchMoved_) {
                int hit = world_->pickCell(mouseWorld);
                if (hit < 0) {  // fingers are blunt: take the nearest cell within ~5 mm of screen
                    float best = 1e30f;
                    for (int k : cellsInCircle(mouseWorld, 16.0f * uiScale_ / zoom_)) {
                        float d2 = length2(world_->cells()[k].pos - mouseWorld);
                        if (d2 < best) {
                            best = d2;
                            hit = k;
                        }
                    }
                }
                track(hit);
                if (hit >= 0) {
                    dockMicro_.tab = 1;
                    dockMicro_.open = true;
                }
            }
            touchPan_ = false;
        }
    } else {
        touchPan_ = false;
    }
    lastMouseX_ = mx;
    lastMouseY_ = my;

    // Tools (left button)
    bool lmbDown = lmbDownNow;
    if (lmbDownNow && !overUi) relaxFrames_ = 30;  // a tool at work on a frozen dish: let it settle afterwards
    bool lmbClicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overUi && !(mobile_ && ui_.tool == Tool::Inspect);
    if (lmbClicked && !toolAllowed(ui_.tool)) {
        toast("Этот инструмент в задании запрещён.");
    } else if (lmbClicked) {
        int hit = world_->pickCell(mouseWorld);
        // The tool's sound.
        switch (ui_.tool) {
            case Tool::Synth: audio::play(Sfx::Insert); break;
            case Tool::Boost: audio::play(Sfx::Boost); break;
            case Tool::Remove: audio::play(Sfx::Remove); break;
            case Tool::Shock: audio::play(Sfx::Shock); break;
            case Tool::GeneNeedle:
            case Tool::DrugNeedle:
            case Tool::Adhesin: audio::play(Sfx::Needle, 0.8f); break;
            case Tool::Map:
                if (ui_.mapMode < MapMode::Eraser) audio::play(Sfx::Wall);
                else if (ui_.mapMode == MapMode::Current || ui_.mapMode == MapMode::Vortex) audio::play(Sfx::Water);
                break;
            default: break;
        }
        switch (ui_.tool) {
            case Tool::Inspect:
                track(hit);
                if (hit >= 0) cfg_.winCell = true;
                break;
            case Tool::Synth:
                if (length(mouseWorld) >= world_->settings.dishRadius) break;
                if (ui_.challenge >= 0 && ui_.chState.startR > 0 && length(mouseWorld - ui_.chState.start) > ui_.chState.startR) {
                    toast("Вставлять клетки можно только в синем круге.");
                    break;
                }
                if (ui_.challenge >= 0) {
                    const Challenge& ch = challenges()[ui_.challenge];
                    if (ui_.chState.insertsLeft == 0) {
                        toast(ch.inserts == 0 ? "В этом задании синтезатор выключен."
                                              : "Вставки закончились. Чтобы начать сначала, нажмите «Заново».");
                        break;
                    }
                    uint32_t bad = genomeTypes(ui_.editGenome) & ch.forbidden;
                    if (bad) {
                        std::string names;
                        for (int t = 0; t < kCellTypeCount; ++t)
                            if (bad >> t & 1u) names += (names.empty() ? "" : ", ") + std::string(cellTypeInfo(t).name);
                        toast("В геноме есть запрещённые в задании типы клеток: " + names + ".");
                        break;
                    }
                }
                if (world_->addCell(ui_.editGenome, mouseWorld, kPi * 0.5f) < 0) toast("Достигнут лимит клеток.");
                else if (ui_.challenge >= 0 && ui_.chState.insertsLeft > 0) --ui_.chState.insertsLeft;
                break;
            case Tool::Tweezers: dragCell_ = hit; break;
            case Tool::Boost:
                if (ui_.brushArea) {
                    std::vector<int> in = cellsInCircle(mouseWorld, ui_.brushRadius);
                    if (!in.empty()) {
                        for (int k : in) world_->boostCell(k);
                    } else {
                        // Spray of food over the brush circle.
                        Rng rng((uint64_t)(time_ * 1e6) | 1);
                        int count = std::clamp((int)(ui_.brushRadius * ui_.brushRadius / 300.0f), 3, 400);
                        for (int k = 0; k < count; ++k) {
                            Vec2 p = mouseWorld + fromAngle(rng.range(0, kTau)) * (ui_.brushRadius * std::sqrt(rng.uniform()));
                            if (length(p) < world_->settings.dishRadius && !world_->insideBarrier(p, 2.0f))
                                world_->addFood(p, 0.3f * kFoodUnit);
                        }
                    }
                } else if (hit >= 0) {
                    world_->boostCell(hit);
                } else if (length(mouseWorld) < world_->settings.dishRadius) {
                    world_->addFood(mouseWorld, 1.152f * kFoodUnit);
                }
                break;
            case Tool::Remove:
                removing_ = true;
                break;
            case Tool::Shock: {
                World::ShockReport r = world_->shock(mouseWorld, ui_.brushRadius, ui_.shockVolts, ui_.shockMs);
                shocks_.push_back({mouseWorld, ui_.brushRadius, time_});
                if (r.hit) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "Импульс: до %.0f мВ на мембране · возбуждено %d · пробито %d · лопнуло %d",
                                  r.maxDeltaPsi * 1000.0f, r.excited, r.porated, r.lysed);
                    toast(msg);
                }
                break;
            }
            case Tool::Adhesin:
                adhesinDrag_ = true;
                adhesinStart_ = adhesinLast_ = mouseWorld;
                adhesinFrom_ = hit;
                adhesinFromId_ = hit >= 0 ? world_->cells()[hit].id : 0;
                if (ui_.adhesinMode == AdhesinMode::Glue) {
                    int n = world_->glueTouching(mouseWorld, ui_.brushRadius, ui_.adhesinStiffness);
                    if (n) toast("Склеено связей: " + std::to_string(n));
                }
                break;
            case Tool::GeneNeedle:
                if (hit >= 0) {
                    int mode = ui_.geneModeChoice == 1 ? ui_.editMode : (ui_.geneModeChoice == 2 ? ui_.editGenome.initialMode : -1);
                    int n = world_->injectGenome(hit, ui_.editGenome, mode, ui_.geneWholeOrganism);
                    toast("Геном «" + ui_.editGenome.name + "» введён: " + std::to_string(n) + (n == 1 ? " клетка" : " клеток"));
                    track(hit);
                }
                break;
            case Tool::DrugNeedle: {
                auto s = (World::Substance)ui_.drug;
                float dose = ui_.drugDose[ui_.drug];
                if (ui_.drugIntoWater) {
                    if (length(mouseWorld) < world_->settings.dishRadius) world_->releaseSubstance(mouseWorld, s, dose, ui_.drugChannel);
                    lastInject_ = time_;
                } else if (ui_.brushArea) {
                    for (int k : cellsInCircle(mouseWorld, ui_.brushRadius)) world_->injectSubstance(k, s, dose, ui_.drugChannel);
                    lastInject_ = time_;
                } else if (hit >= 0) {
                    world_->injectSubstance(hit, s, dose, ui_.drugChannel);
                    lastInject_ = time_;
                    track(hit);
                }
                break;
            }
            case Tool::Map:
                mapDrawing_ = true;
                mapStart_ = mapLast_ = mouseWorld;
                if (ui_.mapMode == MapMode::Brush) world_->addBarrier(mouseWorld, mouseWorld, ui_.mapThickness);
                if (ui_.mapMode == MapMode::Vortex)
                    world_->addVortex(mouseWorld, ui_.brushRadius, ui_.vortexClockwise ? -ui_.flowSpeed : ui_.flowSpeed);
                break;
            default: break;
        }
    }
    // Holding the drug needle keeps injecting (5 doses a second, except the mutagen).
    if (ui_.tool == Tool::DrugNeedle && lmbDown && !overUi && ui_.drug != (int)World::Substance::Mutagen && time_ - lastInject_ > 0.2) {
        auto s = (World::Substance)ui_.drug;
        float dose = ui_.drugDose[ui_.drug];
        if (ui_.drugIntoWater) {
            if (length(mouseWorld) < world_->settings.dishRadius) world_->releaseSubstance(mouseWorld, s, dose, ui_.drugChannel);
        } else if (ui_.brushArea) {
            for (int k : cellsInCircle(mouseWorld, ui_.brushRadius)) world_->injectSubstance(k, s, dose, ui_.drugChannel);
        } else {
            int hit = world_->pickCell(mouseWorld);
            if (hit >= 0) world_->injectSubstance(hit, s, dose, ui_.drugChannel);
        }
        lastInject_ = time_;
    }
    // Adhesin tool strokes.
    if (ui_.tool == Tool::Adhesin && adhesinDrag_) {
        if (lmbDown) {
            if (ui_.adhesinMode == AdhesinMode::Glue && length(mouseWorld - adhesinLast_) > ui_.brushRadius * 0.5f) {
                world_->glueTouching(mouseWorld, ui_.brushRadius, ui_.adhesinStiffness);
                adhesinLast_ = mouseWorld;
            } else if (ui_.adhesinMode == AdhesinMode::Cut && length(mouseWorld - adhesinLast_) > 2.0f) {
                world_->cutLinks(adhesinLast_, mouseWorld);
                adhesinLast_ = mouseWorld;
            }
        } else {
            if (ui_.adhesinMode == AdhesinMode::Link) {
                int from = adhesinFrom_;
                bool ok = from >= 0 && from < (int)world_->cells().size() && world_->cells()[from].alive &&
                          world_->cells()[from].id == adhesinFromId_;
                int to = world_->pickCell(mouseWorld);
                if (ok && to >= 0 && to != from) {
                    if (world_->linkCells(from, to, ui_.adhesinSlack, ui_.adhesinStiffness) >= 0) toast("Адгезин создан");
                    else toast("Связь не создана: она уже есть или у клетки 20 связей");
                }
            }
            adhesinDrag_ = false;
            adhesinFrom_ = -1;
        }
    }
    // Map editor strokes.
    if (ui_.tool == Tool::Map && mapDrawing_) {
        const float r = ui_.mapThickness;
        if (lmbDown) {
            if (ui_.mapMode == MapMode::Brush && length(mouseWorld - mapLast_) > std::max(2.0f, r * 0.7f)) {
                world_->addBarrier(mapLast_, mouseWorld, r);
                mapLast_ = mouseWorld;
            } else if (ui_.mapMode == MapMode::Eraser) {
                world_->eraseBarriers(mouseWorld, ui_.brushRadius);
            } else if (ui_.mapMode == MapMode::Current && length(mouseWorld - mapLast_) > std::max(3.0f, ui_.brushRadius * 0.2f)) {
                world_->paintFlow(mapLast_, mouseWorld, ui_.brushRadius, ui_.flowSpeed);  // the water follows the finger
                audio::play(Sfx::Water, 0.5f);  // (one at a time: a stroke does not pile them up)
                mapLast_ = mouseWorld;
            } else if (ui_.mapMode == MapMode::Vortex) {
                world_->addVortex(mouseWorld, ui_.brushRadius, ui_.vortexClockwise ? -ui_.flowSpeed : ui_.flowSpeed);
            } else if (ui_.mapMode == MapMode::FlowEraser) {
                world_->eraseFlow(mouseWorld, ui_.brushRadius * 0.5f);
            }
        } else {
            Vec2 a = mapStart_, b = mouseWorld;
            if (ui_.mapMode == MapMode::Line && length(b - a) > 1.0f) {
                world_->addBarrier(a, b, r);
            } else if (ui_.mapMode == MapMode::Rect) {
                Vec2 c1{a.x, b.y}, c2{b.x, a.y};
                world_->addBarrier(a, c1, r);
                world_->addBarrier(c1, b, r);
                world_->addBarrier(b, c2, r);
                world_->addBarrier(c2, a, r);
            } else if (ui_.mapMode == MapMode::Ring) {
                float rad = length(b - a);
                int segs = std::clamp((int)(rad / 12.0f), 12, 96);
                for (int k = 0; k < segs; ++k)
                    world_->addBarrier(a + fromAngle(k * kTau / segs) * rad, a + fromAngle((k + 1) * kTau / segs) * rad, r);
            }
            mapDrawing_ = false;
        }
    }
    if (ui_.tool == Tool::Remove && removing_ && lmbDown && !overUi) {
        std::vector<int> victims;
        if (ui_.brushArea) victims = cellsInCircle(mouseWorld, ui_.brushRadius);
        else if (int hit = world_->pickCell(mouseWorld); hit >= 0) victims.push_back(hit);
        for (int k : victims) {
            if (k == trackedCell()) track(-1);
            world_->removeCell(k, false);
        }
    }
    if (!lmbDown) {
        removing_ = false;
        dragCell_ = -1;
    }
    if (dragCell_ >= 0 && (dragCell_ >= (int)world_->cells().size() || !world_->cells()[dragCell_].alive)) dragCell_ = -1;
    world_->dragCell(dragCell_, mouseWorld);
    if (ui_.speed == Speed::Freeze && dragCell_ >= 0) world_->dragFrozen(dragCell_, mouseWorld, io.DeltaTime);
}

// ---------------------------------------------------------------- simulation pacing
void App::advanceSimulation(double frameDt) {
    double start = nowSeconds();
    int ticks = 0;
    double threadAlpha = -1;  // slow / observe on the simulation thread: the interpolation (see there)
    const double budgetMs = std::clamp((double)cfg_.simBudgetMs, 2.0, 40.0);
    auto budgetLeft = [&] { return (nowSeconds() - start) * 1000.0 < budgetMs; };
    switch (ui_.speed) {
        case Speed::Freeze:
            simAccumulator_ = 0;
            // Without ticks nothing resolves overlaps or refreshes the neighbour grid: do it here, so
            // dragged or inserted cells still push their neighbours aside and outlines stay correct. Only for a while
            // after a change: run every frame, the relaxation kept nudging a packed colony back and forth and the
            // frozen dish shimmered.
            {
                static size_t lastCount = 0;
                if (world_->cells().size() != lastCount) {
                    lastCount = world_->cells().size();
                    relaxFrames_ = std::max(relaxFrames_, 30);
                }
            }
            if (dragCell_ >= 0) relaxFrames_ = 30;
            if (relaxFrames_ > 0) {
                --relaxFrames_;
                world_->relaxStatic(dragCell_ >= 0 ? 3 : 1);
            }
            break;
        case Speed::Slow:
        case Speed::Observe: {
            double rate = ui_.speed == Speed::Slow ? cfg_.slowTicksPerSecond : cfg_.observeTicksPerSecond;
            if (simThread_.joinable()) {
                // The frame keeps the rate: it orders the ticks that are due and the thread runs them while the frame
                // is drawn. (Pacing the thread by the clock let ticks pile up while a frame held the world and run in
                // bursts in between; the clock-based interpolation then jumped ahead and back: shimmering cells.)
                // The frame shows the dish one order behind, between the last two ticks, by the time left over.
                ticks = simTicks_.exchange(0);
                // The dish holds the ticks ordered up to the last frame; the time left over then (in ticks) is how far
                // to show between its last two: the picture runs one frame behind and advances evenly.
                threadAlpha = simAccumulator_;
                simAccumulator_ += std::min(frameDt, 0.1) * rate;
                const int due = (int)simAccumulator_;
                simAccumulator_ -= due;
                const int maxPending = std::max(2, (int)(rate * 0.1));  // an overloaded machine falls behind, no backlog
                simQuota_ = std::min(simQuota_.load() + due, maxPending);
                break;
            }
            simAccumulator_ += std::min(frameDt, 0.1) * rate;
            while (simAccumulator_ >= 1.0 && budgetLeft()) {
                world_->step();
                simAccumulator_ -= 1.0;
                ++ticks;
            }
            simAccumulator_ = std::min(simAccumulator_, 2.0);  // don't spiral when the machine can't keep up
            break;
        }
        case Speed::Incubate:
            if (simThread_.joinable()) {
                ticks = simTicks_.exchange(0);  // the incubation thread did the stepping since the last frame
            } else {
                do {
                    world_->step();
                    ++ticks;
                } while (budgetLeft());
            }
            break;
    }
    if (ui_.speed != Speed::Freeze) relaxFrames_ = 30;  // paused next: settle once, then hold still
    // Render between the last two ticks so motion stays smooth when frames outnumber ticks.
    bool fixedRate = ui_.speed == Speed::Slow || ui_.speed == Speed::Observe;
    renderAlpha_ = fixedRate && cfg_.smoothMotion ? (float)std::clamp(threadAlpha >= 0 ? threadAlpha : simAccumulator_, 0.0, 1.0) : 1.0f;
    // No tick since the last frame (an overloaded machine): hold where the picture was rather than slide back.
    if (fixedRate && ticks == 0) renderAlpha_ = std::max(renderAlpha_, lastAlpha_);
    lastAlpha_ = renderAlpha_;
    if (!fixedRate) simQuota_ = 0;
    double ms = (nowSeconds() - start) * 1000.0;
    simMsPerFrame_ = simMsPerFrame_ * 0.9 + ms * 0.1;
    ticksThisSecond_ += ticks;
    lastFrameTicks_ = ticks;  // the current tracers move with the water by this much
    double t = nowSeconds();
    if (t - tickCounterStart_ >= 1.0) {
        ticksPerSecond_ = ticksThisSecond_ / (t - tickCounterStart_);
        hoursPerSecond_ = (world_->timeHours() - lastHours_) / (t - tickCounterStart_);
        lastHours_ = world_->timeHours();
        ticksThisSecond_ = 0;
        tickCounterStart_ = t;
    }
}

// Genome editor: unfold the genome by stage (physics-free) and show the current tick as a static organism.
void App::updatePreview() {
    ui_.previewVisible = ui_.screen == Screen::Genome;
    if (!ui_.previewVisible) return;
    VkExtent2D ext = ctx_.extent();
    previewW_ = ext.width;
    previewH_ = ext.height;

    if (ui_.previewDirty) {
        int oldStages = (int)layout_.stages.size();
        // While a phantom is dragged only the stages up to the edited one are needed: cheap, smooth drags.
        layout_ = dragPhantom_ >= 0 ? buildGenomeLayout(ui_.editGenome, editorStage() + 1) : buildGenomeLayout(ui_.editGenome);
        // A genome that grows further than before: follow it to the newest stage the user was looking at.
        (void)oldStages;
        editorTick_ = std::min(editorTick_, editorMaxTick());
        ui_.previewDirty = false;
        builtTick_ = -1;
    }
    if (builtTick_ != editorTick_) rebuildPreviewWorld();

    if (previewAutoFrame_) {
        // Frame the fully grown organism so the camera does not move while scrubbing the timeline.
        const LayoutStage& last = layout_.stages.back();
        Vec2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
        for (const LayoutCell& c : last.cells) {
            float r = radiusForMass(c.mass, CellType::Phagocyte);
            lo = {std::min(lo.x, c.pos.x - r), std::min(lo.y, c.pos.y - r)};
            hi = {std::max(hi.x, c.pos.x + r), std::max(hi.y, c.pos.y + r)};
        }
        Vec2 center = (lo + hi) * 0.5f;
        float spanX = std::max(hi.x - lo.x, 80.0f), spanY = std::max(hi.y - lo.y, 80.0f);
        float usableW = std::max(200.0f, previewW_ - 2.0f * 360.0f * uiScale_);
        float usableH = std::max(200.0f, previewH_ - 230.0f * uiScale_);
        Vec2 shift{0.0f, -25.0f * uiScale_};  // screen pixels: the free area's centre relative to the screen centre
        if (mobile_ && freeMax_.x > freeMin_.x + 50 && freeMax_.y > freeMin_.y + 50) {
            usableW = freeMax_.x - freeMin_.x;
            usableH = freeMax_.y - freeMin_.y;
            shift = {(freeMin_.x + freeMax_.x - previewW_) * 0.5f, (freeMin_.y + freeMax_.y - previewH_) * 0.5f};
        }
        float fit = std::min({usableW / (spanX * 1.2f), usableH / (spanY * 1.2f), 9.0f});
        float k = 1.0f - std::exp(-frameDt_ * 6.0f);
        previewZoom_ += (fit - previewZoom_) * k;
        // Shift so the organism sits in the free area between the side panels, above the timeline.
        Vec2 offset{-shift.x / previewZoom_, shift.y / previewZoom_};
        previewCam_ = previewCam_ + (center + offset - previewCam_) * k;
    }
}

void App::rebuildPreviewWorld() {
    builtTick_ = editorTick_;
    World& p = *preview_;
    Settings s;
    s.dishRadius = 6000.0f;  // open fluid: the dish rim stays far outside the view
    s.maxCells = 4000;
    s.lightAmount = 0;
    s.nutrientRate = 0;
    p.reset(s);
    previewMap_.clear();
    if (layout_.stages.empty()) return;

    const int stage = editorStage();
    const int lastStage = (int)layout_.stages.size() - 1;
    float frac = stage < lastStage ? (float)(editorTick_ - stage * kTicksPerStage) / kTicksPerStage : 0.0f;
    float t = frac * frac * (3.0f - 2.0f * frac);  // ease the division animation
    const Genome& g = ui_.editGenome;

    // Between two stages, show the next stage with children sliding out of their parents.
    const bool animating = frac > 0.0f;
    const LayoutStage& cur = layout_.stages[animating ? stage + 1 : stage];
    const LayoutStage* prevStage = animating ? &layout_.stages[stage] : nullptr;
    int first = -1;
    auto add = [&](const LayoutCell& c, Vec2 pos, float mass, int layoutIdx, bool phantom) {
        int idx;
        if (first < 0) {
            idx = p.addCell(g, pos, c.angle, c.mode, CellTag::User, mass);
            first = idx;
            if (idx >= 0) p.cellsMut()[idx].mirrored = c.mirrored;
        } else {
            idx = p.addCellLike(first, pos, c.angle, c.mode, mass, c.mirrored);
        }
        if (idx < 0) return -1;
        Cell& wc = p.cellsMut()[idx];
        wc.editorFlags = (phantom ? 1 : 0) | (c.mode == ui_.editMode ? 2 : 0);
        if ((int)previewMap_.size() <= idx) previewMap_.resize(idx + 1);
        previewMap_[idx] = {layoutIdx, phantom};
        return idx;
    };
    std::vector<int> worldIdx(cur.cells.size(), -1);
    for (size_t i = 0; i < cur.cells.size(); ++i) {
        const LayoutCell& c = cur.cells[i];
        Vec2 pos = c.pos;
        float mass = c.mass;
        if (prevStage && c.parent >= 0) {
            const LayoutCell& par = prevStage->cells[c.parent];
            pos = par.pos + (c.pos - par.pos) * t;
            mass = mixf(c.born ? c.birthMass : par.mass, c.mass, t);
        }
        worldIdx[i] = add(c, pos, mass, (int)i, false);
    }
    for (const LayoutLink& l : cur.links)
        if (worldIdx[l.a] >= 0 && worldIdx[l.b] >= 0) p.connect(worldIdx[l.a], worldIdx[l.b], l.extra, l.stiffness);

    // At a stage boundary, ghost the children the selected mode will produce next.
    if (!animating && stage < lastStage) {
        const LayoutStage& nx = layout_.stages[stage + 1];
        for (size_t i = 0; i < nx.cells.size(); ++i) {
            const LayoutCell& c = nx.cells[i];
            if (!c.born || c.parent < 0) continue;
            if (cur.cells[c.parent].mode != ui_.editMode) continue;
            add(c, c.pos, c.birthMass, (int)i, true);
        }
    }
    p.rebuildGrid();
}

int App::pickPreview(Vec2 sp, bool* phantom) const {
    Vec2 w = screenToPreview(sp);
    const auto& cells = preview_->cells();
    int best = -1;
    float bestD = 1e30f;
    bool bestPh = false;
    for (size_t i = 0; i < cells.size() && i < previewMap_.size(); ++i) {
        const Cell& c = cells[i];
        if (!c.alive) continue;
        float d = length2(c.pos - w);
        float r = c.radius;
        bool ph = previewMap_[i].phantom;
        // Phantoms sit on top of their parent: prefer them when the click is inside both.
        float score = ph ? d * 0.5f : d;
        if (d < r * r && score < bestD) {
            bestD = score;
            best = previewMap_[i].layoutIdx;
            bestPh = ph;
        }
    }
    if (phantom) *phantom = bestPh;
    return best;
}

// ---------------------------------------------------------------- soft-body outlines
// Each visible cell gets a target outline: flat faces against every touching neighbour (the radical plane,
// like soap bubbles), bulges where a stretched adhesin pulls, and area conservation so a squeezed cell
// swells sideways. The drawn outline follows the target through per-spoke damped springs, which gives the
// membrane its elasticity (wobble when released, smooth squash when pressed).
void App::updateShapes(const World& w, std::vector<ShapeState>& shapes, const std::vector<int>& list, float alpha, float dt,
                       float pxPerUnit) {
    const auto& cells = w.cells();
    const auto& links = w.links();
    if (shapes.size() < cells.size()) shapes.resize(cells.size());
    static float cosT[kShapeSpokes], sinT[kShapeSpokes];
    static bool tablesReady = false;
    if (!tablesReady) {
        for (int k = 0; k < kShapeSpokes; ++k) {
            cosT[k] = std::cos(k * kTau / kShapeSpokes);
            sinT[k] = std::sin(k * kTau / kShapeSpokes);
        }
        tablesReady = true;
    }
    auto ipos = [&](const Cell& c) { return c.prevPos + (c.pos - c.prevPos) * alpha; };
    auto iang = [&](const Cell& c) { return c.prevAngle + wrapAngle(c.angle - c.prevAngle) * alpha; };
    const float searchR = w.maxCellRadius() + 2.0f;
    dt = std::clamp(dt, 0.0f, 1.0f / 20.0f);

    jobs_->parallelFor((int)list.size(), 64, [&](int b, int e) {
        for (int li = b; li < e; ++li) {
            int i = list[li];
            const Cell& c = cells[i];
            if (!cfg_.softBodies || c.radius * pxPerUnit < 3.0f || (c.editorFlags & 1)) {  // round outline
                ShapeState& st = shapes[i];
                st.id = 0;
                for (int k = 0; k < kShapeSpokes; ++k) st.r[k] = 1.0f;
                continue;
            }
            const Vec2 pi = ipos(c);
            const float ai = iang(c);
            // Outlines are drawn a little larger than the physical cells (kDrawScale), so neighbours press into each
            // other and meet in one straight seam, as in Cell Lab, instead of touching at a point.
            const float Ri = c.radius * kDrawScale;
            float t[kShapeSpokes];
            bool con[kShapeSpokes];
            float cap[kShapeSpokes];  // hard limit: the outline never crosses a neighbour's contact plane
            for (int k = 0; k < kShapeSpokes; ++k) {
                t[k] = 1.0f;
                con[k] = false;
                cap[k] = 1.4f;
            }
            const float ca = std::cos(-ai), sa = std::sin(-ai);
            w.forEachCellNear(pi, Ri + searchR * 1.4f, [&](int j) {
                if (j == i) return;
                const Cell& o = cells[j];
                if (o.editorFlags & 1) return;  // phantoms do not press on real cells
                Vec2 d = ipos(o) - pi;
                float d2 = length2(d);
                const float Ro = o.radius * kDrawScale;
                float rs = Ri + Ro;
                if (d2 >= rs * rs * 1.96f || d2 < 1e-6f) return;
                float dist = std::sqrt(d2);
                // Radical plane of the two circles (where both outlines meet), from this cell's centre.
                float h = (d2 + Ri * Ri - Ro * Ro) / (2.0f * dist * Ri);
                Vec2 n = d / dist;
                float lx = n.x * ca - n.y * sa, ly = n.x * sa + n.y * ca;
                // Touching (or nearly): flatten into a face exactly on the shared plane (one seam line, no sliver).
                bool touching = dist < rs + 0.08f * std::min(Ri, Ro);
                float hf = clampv(h + 0.004f, 0.45f, 1.0f);
                float hc = std::max(h + 0.006f, 0.45f);
                for (int k = 0; k < kShapeSpokes; ++k) {
                    float cs = cosT[k] * lx + sinT[k] * ly;
                    if (cs <= 0.02f) continue;
                    cap[k] = std::min(cap[k], hc / cs);
                    if (touching) {
                        float rk = hf / cs;
                        if (rk < t[k]) {
                            t[k] = rk;
                            con[k] = true;
                        }
                    }
                }
            });
            // A strongly stretched adhesin pulls the membrane into a bulge toward its partner. (Pressed partners need
            // nothing extra: the outlines are drawn kDrawScale larger, so they already meet in one seam.)
            for (int k = 0; k < c.linkCount; ++k) {
                const Link& l = links[c.links[k]];
                if (l.b < 0) continue;
                const Cell& o = cells[l.a == i ? l.b : l.a];
                Vec2 d = ipos(o) - pi;
                float dist = length(d);
                if (dist < 1e-4f) continue;
                float rest = c.radius + o.radius + l.extraLength;
                float st = std::clamp((dist - rest) / c.radius, 0.0f, 0.6f);
                if (st <= 0.02f) continue;
                Vec2 n = d / dist;
                float lx = n.x * ca - n.y * sa, ly = n.x * sa + n.y * ca;
                for (int s = 0; s < kShapeSpokes; ++s) {
                    float cs = cosT[s] * lx + sinT[s] * ly;
                    if (cs <= 0) continue;
                    float c2 = cs * cs;
                    t[s] = std::max(t[s], 1.0f + st * 0.6f * c2 * c2);
                }
            }
            // Flat obstacles: the closed dish wall and the map's walls press the membrane flat like a neighbour does.
            auto pressFlat = [&](Vec2 nWorld, float hdist) {  // nWorld: unit vector toward the wall; hdist: in radii
                float lx = nWorld.x * ca - nWorld.y * sa, ly = nWorld.x * sa + nWorld.y * ca;
                for (int k = 0; k < kShapeSpokes; ++k) {
                    float cs = cosT[k] * lx + sinT[k] * ly;
                    if (cs <= 0.02f) continue;
                    float rk = std::max(hdist, 0.45f) / cs;
                    cap[k] = std::min(cap[k], rk);
                    if (rk < t[k]) {
                        t[k] = rk;
                        con[k] = true;
                    }
                }
            };
            if (w.settings.lid || w.previewMode) {
                float pd = length(pi);
                if (pd > 1e-3f && w.settings.dishRadius - pd < Ri * 1.4f) pressFlat(pi / pd, (w.settings.dishRadius - pd) / Ri);
            }
            w.forBarriersNear(pi, [&](const World::Barrier& b) {
                Vec2 ab = b.b - b.a;
                float ll = length2(ab);
                float u = ll > 1e-6f ? clampv(dot(pi - b.a, ab) / ll, 0.0f, 1.0f) : 0.0f;
                Vec2 v = pi - (b.a + ab * u);
                float dv = length(v);
                if (dv < 1e-3f || dv - b.r > Ri * 1.4f) return;
                pressFlat(-v / dv, (dv - b.r) / Ri);
            });
            // Area conservation: free spokes swell to make up for the flattened ones.
            float conA = 0, freeA = 0;
            int nFree = 0;
            for (int k = 0; k < kShapeSpokes; ++k) {
                if (con[k]) conA += t[k] * t[k];
                else {
                    freeA += t[k] * t[k];
                    ++nFree;
                }
            }
            if (nFree > 0 && nFree < kShapeSpokes && freeA > 0) {
                float need = (float)kShapeSpokes - conA;
                float sc = std::sqrt(std::max(need, 0.0f) / freeA);
                sc = clampv(sc, 1.0f, 1.08f);  // modest swelling: cells keep their size
                for (int k = 0; k < kShapeSpokes; ++k)
                    if (!con[k]) t[k] *= sc;
            }
            // Round the corners between faces, then enforce the contact planes again.
            for (int pass = 0; pass < 2; ++pass) {
                float tmp[kShapeSpokes];
                for (int k = 0; k < kShapeSpokes; ++k)
                    tmp[k] = 0.25f * t[(k + kShapeSpokes - 1) % kShapeSpokes] + 0.5f * t[k] + 0.25f * t[(k + 1) % kShapeSpokes];
                for (int k = 0; k < kShapeSpokes; ++k) t[k] = clampv(std::min(tmp[k], cap[k]), 0.45f, 1.38f);
            }

            ShapeState& st = shapes[i];
            if (st.id != c.id) {
                st.id = c.id;
                for (int k = 0; k < kShapeSpokes; ++k) {
                    st.r[k] = t[k];
                    st.v[k] = 0;
                }
                continue;
            }
            // Damped springs (stiffness ~3.5 Hz, damping ratio ~0.7: a soft give, no lasting jelly wobble), sub-stepped.
            const float K = 480.0f, C = 30.0f;
            int sub = std::max(1, (int)std::ceil(dt / (1.0f / 240.0f)));
            float hdt = dt / sub;
            for (int s = 0; s < sub; ++s)
                for (int k = 0; k < kShapeSpokes; ++k) {
                    st.v[k] += (K * (t[k] - st.r[k]) - C * st.v[k]) * hdt;
                    float r = st.r[k] + st.v[k] * hdt;
                    if (r > cap[k]) {  // wobble may never push the membrane into a neighbour
                        r = cap[k];
                        st.v[k] = std::min(st.v[k], 0.0f);
                    }
                    st.r[k] = clampv(r, 0.42f, 1.4f);
                }
        }
    });
}

// ---------------------------------------------------------------- render lists
void App::buildLists(const World& w, RenderLists& out, Vec2 viewMin, Vec2 viewMax, int selected, int hovered, float time) {
    out.clear();
    const auto& cells = w.cells();
    const auto& links = w.links();
    const float margin = 40.0f;
    auto visible = [&](Vec2 p, float r) {
        return p.x + r + margin > viewMin.x && p.x - r - margin < viewMax.x && p.y + r + margin > viewMin.y &&
               p.y - r - margin < viewMax.y;
    };

    const bool isPreview = &w == preview_.get();
    const float alpha = isPreview ? 1.0f : renderAlpha_;
    auto ipos = [&](const Cell& c) { return c.prevPos + (c.pos - c.prevPos) * alpha; };
    auto iang = [&](const Cell& c) { return c.prevAngle + wrapAngle(c.angle - c.prevAngle) * alpha; };

    // Visible cells, then their soft-body outlines.
    std::vector<int>& vis = visibleScratch_;
    vis.clear();
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        if (c.alive && visible(c.pos, c.radius * 2.6f)) vis.push_back((int)i);
    }
    std::vector<ShapeState>& shapes = isPreview ? previewShapes_ : shapes_;
    updateShapes(w, shapes, vis, alpha, frameDt_, isPreview ? previewZoom_ : zoom_);

    out.cells.reserve(vis.size());
    for (int i : vis) {
        const Cell& c = cells[i];
        Color col = w.cellColor(c);
        // Status tints like the original: injury -> green, boost -> magenta.
        if (c.injury > 0.01f) {
            float k = std::min(c.injury, 1.0f) * 0.6f;
            col = {mixf(col.r, 0.35f, k), mixf(col.g, 0.85f, k), mixf(col.b, 0.30f, k)};
        }
        if (c.boost > 0) {
            float k = std::min(c.boost, 1.0f) * 0.55f;
            col = {mixf(col.r, 0.95f, k), mixf(col.g, 0.35f, k), mixf(col.b, 0.9f, k)};
        }
        uint32_t flags = 0;
        (void)selected;  // the tracked cell gets an overlay ring instead of the shader ring
        if ((int)i == hovered) flags |= kFlagHovered;
        if (c.tag == CellTag::InfectedUser || c.tag == CellTag::InfectedChallenge || c.tag == CellTag::InfectedContamination)
            flags |= kFlagInfected;
        if (c.mass < kDeathMass * 1.25f && !isPreview) flags |= kFlagStarving;
        if (c.editorFlags & 1) flags |= kFlagGhost;
        if (c.editorFlags & 2) flags |= kFlagModeMark;
        float p0 = c.type == CellType::Myocyte ? std::fabs(c.actuator) * 2.0f : c.actuator;
        if (c.type == CellType::Lipocyte) p0 = std::max(0.0f, (c.mass - 1.0f) / (kLipocyteCapacity - 1.0f)) * 1.6f;
        if (c.type == CellType::Buoyocyte) p0 = 0.5f + 0.5f * clampv(w.modeOf(c).prog[PF_Density].b / 3.0f, -1.0f, 1.0f);
        if (c.type == CellType::Devorocyte) {
            // Where the devorocyte is bonded to its own body it has no teeth: a 16-sector mask of bond directions in
            // the cell's frame (sector k centred on k * 22.5°), passed as an exact integer in p0.
            uint32_t mask = 0;
            const Vec2 pc = ipos(c);
            const float ac = iang(c);
            for (int q = 0; q < c.linkCount; ++q) {
                const Link& l = links[c.links[q]];
                if (!l.alive || l.b < 0) continue;
                const Cell& o = cells[l.a == (int)i ? l.b : l.a];
                Vec2 d = ipos(o) - pc;
                float rel = wrapAngle(std::atan2(d.y, d.x) - ac);
                for (int k = 0; k < 16; ++k)
                    if (std::fabs(wrapAngle(rel - k * kTau / 16.0f)) < deg2rad(25.0f)) mask |= 1u << k;
            }
            p0 = (float)mask;
        }
        {
            // Beats run on a per-cell phase advanced by the frame time at the current (smoothed) activity: a phase of
            // time x rate jumped whenever the activity changed, and flagella, cilia and nerve impulses flickered.
            ShapeState& an = shapes[i];
            const float a01 = clampv(p0, 0.0f, 1.0f);
            if (an.animId != c.id) {
                an.animId = c.id;
                an.phase = (float)((c.id * 2654435761u) >> 8) / 16777216.0f;  // each cell starts at its own point
                an.act = a01;
            }
            // A frozen dish holds still, as in Cell Lab: no beating flagella either.
            const float adt = (!isPreview && ui_.speed == Speed::Freeze) ? 0.0f : clampv(frameDt_, 0.0f, 0.1f);
            an.act += (a01 - an.act) * (1.0f - std::exp(-adt / 0.12f));
            auto advance = [&](float cyclesPerSecond) { an.phase = std::fmod(an.phase + adt * cyclesPerSecond, 1.0f); };
            switch (c.type) {
                case CellType::Flagellocyte:
                    advance((5.0f + 14.0f * an.act) / kTau);
                    p0 = an.phase;
                    break;
                case CellType::Ciliocyte:
                    advance(9.0f * (0.6f + 0.8f * an.act) / kTau);
                    p0 = an.phase;
                    break;
                case CellType::Neurocyte:  // phase in the fraction, activity in 1/63 steps in the integer part
                    advance(0.6f + 2.4f * an.act);
                    p0 = std::floor(an.act * 63.0f + 0.5f) + an.phase;
                    break;
                case CellType::Myocyte: p0 = an.act; break;  // fibres follow the contraction smoothly
                default: break;
            }
        }
        CellInstance ci;
        Vec2 p = ipos(c);
        ci.x = p.x;
        ci.y = p.y;
        ci.radius = c.radius * kDrawScale;  // drawn slightly larger: neighbours press into one seam (updateShapes)
        ci.angle = iang(c);
        const ShapeState& sh = shapes[i];
        for (int k = 0; k < kShapeSpokes / 4; ++k) {
            uint32_t word = 0;
            for (int b = 0; b < 4; ++b) {
                float r = sh.r[k * 4 + b];
                uint32_t q = (uint32_t)clampv((r - 0.4f) * 255.0f + 0.5f, 0.0f, 255.0f);
                word |= q << (b * 8);
            }
            ci.shape[k] = word;
        }
        ci.color = packColor(col);
        ci.typeFlags = (uint32_t)c.type | (flags << 8) | ((c.id * 2654435761u) & 0xFFFF0000u);
        ci.p0 = p0;
        ci.p1 = c.mass / kMaxCellMass;
        out.cells.push_back(ci);
    }

    // Map barriers: flat vector strokes in one colour (overlapping pieces merge into clean lines).
    if (!isPreview) {
        const bool classic = cfg_.theme != 0;
        Color wall = classic ? Color{0.33f, 0.33f, 0.44f} : Color{0.60f, 0.64f, 0.70f};
        for (const World::Barrier& b : w.barriers()) {
            if (!visible(b.a, b.r + 60.0f) && !visible(b.b, b.r + 60.0f) && !visible((b.a + b.b) * 0.5f, length(b.b - b.a) + b.r)) continue;
            LineInstance li{};
            li.ax = b.a.x;
            li.ay = b.a.y;
            li.bx = b.b.x;
            li.by = b.b.y;
            li.width = b.r;
            li.pinch = -1.0f;  // flat stroke
            li.color = packColor(wall, 1.0f);
            out.lines.push_back(li);
        }
    }

    if (isPreview ? ui_.showLinks : cfg_.showSlime) {
        for (const Link& l : links) {
            if (!l.alive) continue;
            const Cell& a = cells[l.a];
            Vec2 pa = ipos(a);
            Vec2 pb = l.b >= 0 ? ipos(cells[l.b]) : l.wallPoint;
            if (!visible(pa, 40.0f) && !visible(pb, 40.0f)) continue;
            // Adhesin = slime bridging the two membranes. Invisible while the cells touch; it appears as the
            // gap opens and thins toward the middle the more it is stretched.
            const bool wall = l.b < 0;
            float rb = wall ? 0.0f : cells[l.b].radius;
            Vec2 d = pb - pa;
            float dist = length(d);
            if (dist < 1e-3f) continue;
            // Pressed together: a few adhesin fibres fan across the seam (the Cell Lab look of a bond).
            if (!wall && dist < (a.radius + rb) * kDrawScale) {
                const float minR = std::min(a.radius, rb);
                const float pxScale = isPreview ? previewZoom_ : zoom_;
                if (minR * pxScale > 5.0f) {
                    const float RA = a.radius * kDrawScale, RB = rb * kDrawScale;
                    const Vec2 n = d / dist, pp = perp(n);
                    const float sA = (dist * dist + RA * RA - RB * RB) / (2.0f * dist);  // seam, from a's centre
                    const Color ca = w.modeOf(a).color, cb = w.modeOf(cells[l.b]).color;
                    const Color fc{(ca.r + cb.r) * 0.27f, (ca.g + cb.g) * 0.27f, (ca.b + cb.b) * 0.27f};
                    for (int k = 0; k < 3; ++k) {
                        float lat = (k - 1.0f) * 0.17f * minR;
                        Vec2 s0 = pa + n * (sA - 0.32f * minR) + pp * (lat * 1.35f);
                        Vec2 s1 = pa + n * (sA + 0.32f * minR) + pp * (lat * 0.75f);
                        LineInstance fi{};
                        fi.ax = s0.x;
                        fi.ay = s0.y;
                        fi.bx = s1.x;
                        fi.by = s1.y;
                        fi.width = 0.035f * minR;
                        fi.pinch = -2.0f;  // fibre mode of line.frag
                        fi.color = packColor(fc, 0.5f);
                        out.fibres.push_back(fi);
                    }
                }
            }
            float gap = dist - (a.radius + rb) * 0.97f;
            if (gap < 0.4f) continue;
            Vec2 n = d / dist;
            float minR = wall ? a.radius : std::min(a.radius, rb);
            float stretch = gap / (a.radius + std::max(rb, a.radius * 0.5f));
            float vis = clampv((gap - 0.4f) / 5.0f, 0.0f, 1.0f);
            Vec2 e1 = pa + n * (a.radius * 0.8f);
            Vec2 e2 = wall ? pb : pb - n * (rb * 0.8f);
            Color ca = w.modeOf(a).color;
            Color cb = wall ? ca : w.modeOf(cells[l.b]).color;
            Color mixc{(ca.r + cb.r) * 0.5f, (ca.g + cb.g) * 0.5f, (ca.b + cb.b) * 0.5f};
            float toWhite = cfg_.theme ? 0.55f : 0.35f;
            Color slime{mixf(mixc.r, 1.0f, toWhite), mixf(mixc.g, 1.0f, toWhite), mixf(mixc.b, 1.0f, toWhite)};
            LineInstance li{};
            li.ax = e1.x;
            li.ay = e1.y;
            li.bx = e2.x;
            li.by = e2.y;
            li.width = minR * 0.42f / (1.0f + stretch * 1.5f);
            li.pinch = clampv(stretch * 1.8f, 0.0f, 0.8f);
            li.color = packColor(slime, (cfg_.theme ? 0.78f : 0.62f) * vis);
            out.lines.push_back(li);
        }
    }

    // Dissolved substances (cell sap, broth, drugs): soft clouds under everything else.
    if (!isPreview) {
        for (const Fluid& f : w.fluids()) {
            if (!visible(f.pos, f.radius)) continue;
            Color c;
            switch (f.kind) {
                case FluidKind::Sap: c = {(f.color & 255) / 255.0f, ((f.color >> 8) & 255) / 255.0f, ((f.color >> 16) & 255) / 255.0f}; break;
                case FluidKind::Broth: c = {0.80f, 0.66f, 0.38f}; break;
                case FluidKind::Toxin: c = {0.35f, 0.80f, 0.30f}; break;
                case FluidKind::Neurotoxin: c = {0.62f, 0.38f, 0.85f}; break;
                case FluidKind::Stimulant: c = {0.95f, 0.35f, 0.30f}; break;
                case FluidKind::Enzyme: c = {0.95f, 0.85f, 0.30f}; break;
                default: c = {0.35f, 0.70f, 0.95f}; break;
            }
            // Opacity follows the concentration (ng per µm² of cloud), so a spreading cloud fades out.
            float conc = f.amount / (kPi * f.radius * f.radius);
            float a = clampv(conc * 900.0f, 0.0f, 0.55f);
            // Poisons and drugs stay visible as long as they act (a spread-out cloud of tetrodotoxin still paralyses):
            // a floor of opacity, more contrast at low concentration, and a thin ring in their colour around them.
            const bool drug = f.kind != FluidKind::Sap && f.kind != FluidKind::Broth;
            if (drug) a = 0.14f + 0.41f * std::sqrt(a / 0.55f);
            if (a < 0.03f) continue;  // nearly invisible clouds are not worth their overdraw
            ParticleInstance p{};
            p.x = f.pos.x;
            p.y = f.pos.y;
            p.radius = f.radius;
            p.color = packColor(c, a);
            p.kind = 8u | ((uint32_t)(((size_t)&f - (size_t)w.fluids().data()) / sizeof(Fluid) * 37u & 255u) << 8);
            out.food.push_back(p);
            if (drug) {
                p.radius = f.radius * 0.82f;  // where the cloud is still dense enough to act
                p.color = packColor(c, 0.35f + a * 0.6f);
                p.kind = 2;
                out.food.push_back(p);
            }
        }
    }

    // Water currents: light streaks drifting with the water (born where it moves, carried by it, fading out), and in the
    // current editor a grid of arrows.
    if (!isPreview && w.hasFlow()) {
        const float R = w.settings.dishRadius;
        static uint32_t seed = 0x2545F491u;
        auto rnd = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (float)(seed >> 8) / 16777216.0f;
        };
        const float adv = (float)std::max(0, lastFrameTicks_);
        for (FlowTracer& t : tracers_) {
            t.p += w.flowAt(t.p) * adv;
            t.age += frameDt_;
        }
        tracers_.erase(std::remove_if(tracers_.begin(), tracers_.end(),
                                      [&](const FlowTracer& t) {
                                          return t.age > t.life || length(t.p) > R || length2(w.flowAt(t.p)) < 0.0004f;
                                      }),
                       tracers_.end());
        const size_t want = (size_t)std::clamp(R * R / (160.0f * 160.0f), 150.0f, 1500.0f);
        for (int tries = 0; tracers_.size() < want && tries < 200; ++tries) {
            Vec2 p = fromAngle(rnd() * kTau) * (std::sqrt(rnd()) * R);
            if (length2(w.flowAt(p)) < 0.0004f) continue;  // still water: nothing to show
            tracers_.push_back({p, 0.0f, 1.2f + 1.8f * rnd()});
        }
        const float px = 1.0f / std::max(zoom_, 1e-4f);  // one screen pixel in µm
        for (const FlowTracer& t : tracers_) {
            if (!visible(t.p, 40.0f)) continue;
            const Vec2 u = w.flowAt(t.p);
            const float sp = length(u);
            const float fade = std::min(1.0f, std::min(t.age / 0.3f, (t.life - t.age) / 0.5f));
            const Vec2 tail = t.p - u * 12.0f - (u / std::max(sp, 1e-4f)) * (3.0f * px);  // 12 ticks of water + 3 px
            LineInstance li{};
            li.ax = tail.x;
            li.ay = tail.y;
            li.bx = t.p.x;
            li.by = t.p.y;
            li.width = 0.8f * px;
            li.pinch = -1.0f;
            li.color = packColor(cfg_.theme ? Color{0.25f, 0.35f, 0.65f} : Color{0.80f, 0.92f, 1.0f},
                                 std::clamp(fade, 0.0f, 1.0f) * std::clamp(sp / 0.6f, 0.25f, 0.7f));
            out.lines.push_back(li);
        }
        if (&w == world_.get() && ui_.screen == Screen::Microscope && ui_.tool == Tool::Map && ui_.mapMode >= MapMode::Current) {
            // The editor: an arrow at every third node, as long as the water moves in 25 ticks.
            for (int j = 1; j < World::kFlowN; j += 3)
                for (int i = 1; i < World::kFlowN; i += 3) {
                    const Vec2 p = w.flowNode(i, j), u = w.flowGrid()[(size_t)j * World::kFlowN + i];
                    if (length2(u) < 0.0004f || length(p) > R || !visible(p, 40.0f)) continue;
                    const Vec2 tip = p + u * 25.0f, side = perp(normalize(u)) * (5.0f * std::min(1.0f, length(u)));
                    const Vec2 back = tip - normalize(u) * (8.0f * std::min(1.0f, length(u)));
                    for (auto seg : {std::pair{p, tip}, std::pair{tip, back + side}, std::pair{tip, back - side}}) {
                        LineInstance li{};
                        li.ax = seg.first.x;
                        li.ay = seg.first.y;
                        li.bx = seg.second.x;
                        li.by = seg.second.y;
                        li.width = 1.0f * px;
                        li.pinch = -1.0f;
                        li.color = packColor({0.55f, 0.75f, 1.0f}, 0.6f);
                        out.lines.push_back(li);
                    }
                }
        }
    } else if (!isPreview) {
        tracers_.clear();
    }

    // Light organs: a soft halo in the glow colour around every lit luminocyte (brightest in a dark dish).
    for (int i : vis) {
        const Cell& c = cells[i];
        if (c.type != CellType::Luminocyte || c.actuator < 0.04f) continue;
        float g = std::min(c.actuator, 1.0f);
        Vec2 p = ipos(c);
        ParticleInstance pi{};
        pi.x = p.x;
        pi.y = p.y;
        pi.radius = c.radius * (2.2f + 2.0f * g);
        pi.color = packColor(w.modeOf(c).senseColor, (0.25f + 0.4f * (1.0f - w.daylight())) * g);
        pi.kind = 3;
        out.food.push_back(pi);
    }

    if (cfg_.showFood) {
        for (const Food& f : w.food()) {
            if (!visible(f.pos, 10.0f)) continue;
            ParticleInstance p{};
            p.x = f.pos.x;
            p.y = f.pos.y;
            p.radius = std::max(foodRadius(f.mass), 1.4f);  // remains of small dead cells stay visible
            if (cfg_.theme) p.color = f.coating > 0.01f ? packColor({0.85f, 0.25f, 0.40f}, 0.85f) : packColor({0.36f, 0.24f, 0.14f}, 0.75f);
            else p.color = f.coating > 0.01f ? packColor({0.86f, 0.42f, 0.52f}, 0.85f) : packColor({0.66f, 0.52f, 0.34f}, 0.8f);
            p.kind = 0;
            out.food.push_back(p);
        }
    }

    if (&w == world_.get()) {
        // Level zones: blue start (the synthesiser works only there), green finish.
        if (ui_.challenge >= 0) {
            const ChallengeState& st = ui_.chState;
            float pulse = 0.85f + 0.15f * std::sin(time * 2.5f);
            if (st.startR > 0) {
                out.overlay.push_back({st.start.x, st.start.y, st.startR, packColor({0.40f, 0.65f, 1.0f}, 0.9f), 2, 0});
                out.overlay.push_back({st.start.x, st.start.y, st.startR, packColor({0.40f, 0.65f, 1.0f}, 0.10f), 4, 0});
            }
            if (st.targetR > 0) {
                out.overlay.push_back({st.target.x, st.target.y, st.targetR, packColor({0.45f, 0.90f, 0.50f}, 0.95f * pulse), 2, 0});
                out.overlay.push_back({st.target.x, st.target.y, st.targetR, packColor({0.45f, 0.90f, 0.50f}, 0.14f * pulse), 4, 0});
            }
        }
        // Electric organ discharges become flashes too.
        for (const World::Spark& s : w.sparks())
            if (s.t > lastSparkSeen_) {
                shocks_.push_back({s.pos, s.radius, time_});
                lastSparkSeen_ = s.t;
            }
        if (w.timeHours() < lastSparkSeen_) lastSparkSeen_ = 0;  // a new dish
        // Electroshock flashes: a bright ring expanding past the shock radius and fading, a spark at the source.
        for (size_t k = 0; k < shocks_.size();) {
            float sa = (float)(time_ - shocks_[k].t0) / 0.12f;
            if (sa < 1.0f)
                out.overlay.push_back({shocks_[k].pos.x, shocks_[k].pos.y, shocks_[k].radius * 0.6f,
                                       packColor({0.85f, 0.95f, 1.0f}, 1.0f - sa), 9u | ((uint32_t)(k * 53) & 255u) << 8, 0});
            float a = (float)(time_ - shocks_[k].t0) / 0.35f;
            if (a >= 1.0f) {
                shocks_.erase(shocks_.begin() + (long)k);
                continue;
            }
            const ShockFx& fx = shocks_[k];
            out.overlay.push_back({fx.pos.x, fx.pos.y, fx.radius * (0.3f + 0.9f * a), packColor({0.75f, 0.90f, 1.0f}, 0.9f * (1 - a)), 2, 0});
            out.overlay.push_back({fx.pos.x, fx.pos.y, fx.radius * (0.2f + 0.5f * a), packColor({0.55f, 0.75f, 1.0f}, 0.25f * (1 - a)), 0, 0});
            ++k;
        }
        // Tracked cell marker
        int t = trackedCell();
        if (t >= 0) {
            const Cell& c = cells[t];
            Vec2 cp = ipos(c);
            float pulse = 1.55f + 0.06f * std::sin(time * 4.0f);
            out.overlay.push_back({cp.x, cp.y, c.radius * pulse, packColor({1.0f, 0.60f, 0.16f}, 0.95f), 2, 0});
        }
        // Tool cursor
        double mx, my;
        glfwGetCursorPos(window_, &mx, &my);
        Vec2 mw = screenToWorld(mx, my);
        // On a phone the brush circle shows only while a finger is down (there is no cursor in between).
        if (!ImGui::GetIO().WantCaptureMouse && (!mobile_ || glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS)) {
            auto ring = [&](float r, Color c, float a) { out.overlay.push_back({mw.x, mw.y, r, packColor(c, a), 2, 0}); };
            auto line = [&](Vec2 a, Vec2 b, Color c, float alpha, float widthPx) {
                LineInstance li{};
                li.ax = a.x;
                li.ay = a.y;
                li.bx = b.x;
                li.by = b.y;
                li.width = widthPx / zoom_;
                li.pinch = -1.0f;
                li.color = packColor(c, alpha);
                out.lines.push_back(li);
            };
            const Color red{0.95f, 0.40f, 0.40f}, green{0.55f, 0.90f, 0.70f}, blue{0.55f, 0.75f, 1.0f};
            // Brush circle: a thin ring of constant screen width, whatever its radius.
            if (ui_.toolUsesRadius()) {
                Color rc = ui_.tool == Tool::Map && ui_.mapMode >= MapMode::Current && ui_.mapMode != MapMode::FlowEraser
                               ? blue  // a current brush
                               : ui_.tool == Tool::Remove || ui_.tool == Tool::Map ? red : ui_.tool == Tool::Shock ? blue : green;
                ring(ui_.brushRadius, rc, 0.85f);
                if (ui_.tool == Tool::Shock) ring(ui_.brushRadius * 2.5f, rc, 0.25f);  // where the field fades out
            }
            if (ui_.tool == Tool::Synth) {
                const Mode& m = ui_.editGenome.modes[ui_.editGenome.initialMode];
                float r = radiusForMass(2.6f, m.type);
                out.overlay.push_back({mw.x, mw.y, r, packColor(m.color, 0.7f), 2, 0});
                out.overlay.push_back({mw.x, mw.y, r * 1.6f, packColor(m.color, 0.18f), 3, 0});
            } else if (ui_.tool == Tool::GeneNeedle || (ui_.tool == Tool::DrugNeedle && !ui_.toolUsesRadius())) {
                Color nc = ui_.tool == Tool::GeneNeedle ? Color{0.70f, 0.45f, 0.95f} : Color{0.35f, 0.85f, 0.75f};
                out.overlay.push_back({mw.x, mw.y, 2.5f, packColor(nc, 0.9f), 0, 0});
                if (ui_.tool == Tool::DrugNeedle && ui_.drugIntoWater)
                    out.overlay.push_back({mw.x, mw.y, 14.0f, packColor(nc, 0.35f), 3, 0});
                else
                    out.overlay.push_back({mw.x, mw.y, 9.0f, packColor(nc, 0.5f), 2, 0});
            } else if (ui_.tool == Tool::Map && ui_.mapMode < MapMode::Eraser) {  // a wall being drawn
                Color pc{1.0f, 0.62f, 0.18f};
                float r = ui_.mapThickness;
                auto ghost = [&](Vec2 a, Vec2 b) {
                    LineInstance li{};
                    li.ax = a.x;
                    li.ay = a.y;
                    li.bx = b.x;
                    li.by = b.y;
                    li.width = r;
                    li.pinch = -1.0f;
                    li.color = packColor(pc, 0.45f);
                    out.lines.push_back(li);
                };
                if (mapDrawing_ && ui_.mapMode == MapMode::Line) {
                    ghost(mapStart_, mw);
                } else if (mapDrawing_ && ui_.mapMode == MapMode::Rect) {
                    Vec2 a = mapStart_, b = mw, c1{a.x, b.y}, c2{b.x, a.y};
                    ghost(a, c1);
                    ghost(c1, b);
                    ghost(b, c2);
                    ghost(c2, a);
                } else if (mapDrawing_ && ui_.mapMode == MapMode::Ring) {
                    float rad = length(mw - mapStart_);
                    for (int k = 0; k < 48; ++k)
                        ghost(mapStart_ + fromAngle(k * kTau / 48) * rad, mapStart_ + fromAngle((k + 1) * kTau / 48) * rad);
                } else {
                    ring(r, pc, 0.7f);
                }
            } else if (ui_.tool == Tool::Remove && !ui_.brushArea) {
                ring(6.0f, red, 0.8f);
            } else if (ui_.tool == Tool::Boost && !ui_.brushArea && hovered < 0) {
                out.overlay.push_back({mw.x, mw.y, foodRadius(11.52f), packColor({0.66f, 0.52f, 0.34f}, 0.6f), 0, 0});
            } else if (ui_.tool == Tool::Adhesin) {
                Color ac{1.0f, 0.82f, 0.45f};
                if (ui_.adhesinMode == AdhesinMode::Link) {
                    int from = adhesinFrom_;
                    if (adhesinDrag_ && from >= 0 && from < (int)cells.size() && cells[from].alive) {
                        Vec2 cp = ipos(cells[from]);
                        line(cp, mw, ac, 0.9f, 2.0f);
                        out.overlay.push_back({cp.x, cp.y, cells[from].radius * 1.25f, packColor(ac, 0.9f), 2, 0});
                        if (hovered >= 0 && hovered != from) {
                            Vec2 hp = ipos(cells[hovered]);
                            out.overlay.push_back({hp.x, hp.y, cells[hovered].radius * 1.25f, packColor(ac, 0.9f), 2, 0});
                        }
                    } else {
                        ring(4.0f, ac, 0.9f);
                    }
                } else if (ui_.adhesinMode == AdhesinMode::Cut) {
                    if (adhesinDrag_) line(adhesinStart_, mw, red, 0.55f, 1.5f);
                    line(mw + Vec2{-6, -6} / zoom_ * 1.5f, mw + Vec2{6, 6} / zoom_ * 1.5f, red, 0.9f, 1.2f);
                    line(mw + Vec2{-6, 6} / zoom_ * 1.5f, mw + Vec2{6, -6} / zoom_ * 1.5f, red, 0.9f, 1.2f);
                }
            }
            if (dragCell_ >= 0) {
                Vec2 cp = ipos(cells[dragCell_]);
                LineInstance li{};
                li.ax = cp.x;
                li.ay = cp.y;
                li.bx = mw.x;
                li.by = mw.y;
                li.width = 1.0f / zoom_;
                li.color = packColor({1.0f, 0.60f, 0.16f}, 0.85f);
                out.lines.push_back(li);
            }
        }
    }
}

// ---------------------------------------------------------------- incubation thread
// The frame takes the world for its input, UI and render lists; between frames (while the GPU work is recorded and
// the swapchain waits for vsync) the incubation thread steps the dish as fast as it can, one tick per lock.
std::unique_lock<std::mutex> App::lockWorld() {
    // Slow / observe: give the thread a moment to finish the ticks the last frame ordered, so every frame moves the
    // dish on by the same amount (a frame that found them undone stood still and the next one leapt).
    if (simMode_ == SimFixedRate) {
        const double until = nowSeconds() + 0.008;
        while (simQuota_.load() > 0 && nowSeconds() < until) std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    mainWaiting_ = true;
    std::unique_lock<std::mutex> lk(simMutex_);
    mainWaiting_ = false;
    return lk;
}

void App::simThreadLoop() {
    while (!simQuit_) {
        const int mode = simMode_;
        if (mode == SimIdle) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (mainWaiting_) {  // the frame wants the world: let it in
            std::this_thread::yield();
            continue;
        }
        if (mode == SimFixedRate && simQuota_.load() <= 0) {
            // Slow / observe: only the ticks the frame has ordered (it keeps the rate; see advanceSimulation).
            std::this_thread::sleep_for(std::chrono::microseconds(300));
            continue;
        }
        std::lock_guard<std::mutex> lk(simMutex_);
        if (simMode_ != mode || simQuit_) continue;
        if (mode == SimFixedRate && simQuota_.fetch_sub(1) <= 0) {  // (cancelled meanwhile)
            simQuota_ = 0;
            continue;
        }
        const double t0 = nowSeconds();
        world_->step();
        stepMs_ = stepMs_.load() * 0.95 + (nowSeconds() - t0) * 1000.0 * 0.05;
        lastTickTime_ = nowSeconds();
        ++simTicks_;
    }
}

// ---------------------------------------------------------------- frame
// Render lists and scene parameters from the dish (needs the world: runs while the frame holds simMutex_).
void App::prepareFrame() {
    VkExtent2D e = ctx_.extent();
    const bool genome = ui_.screen == Screen::Genome;
    if (genome) {
        // Genome screen: the developing organism fills the whole view (Genomeia-style editor).
        float halfW = e.width * 0.5f / previewZoom_, halfH = e.height * 0.5f / previewZoom_;
        buildLists(*preview_, previewLists_, {previewCam_.x - halfW, previewCam_.y - halfH},
                   {previewCam_.x + halfW, previewCam_.y + halfH}, -1, -1, (float)time_);
        previewScene_.camX = previewCam_.x;
        previewScene_.camY = previewCam_.y;
        previewScene_.zoom = previewZoom_;
        previewScene_.time = (float)time_;
        previewScene_.dishRadius = 1.0e7f;  // the editor shows open water: no dish rim, however far the view zooms out
        previewScene_.light = 0.55f;
        previewScene_.lightAngle = kPi * 0.5f;
        previewScene_.lightRange = 1.0f;
        previewScene_.gridAlpha = cfg_.showGrid ? 0.3f : 0.0f;
        previewScene_.theme = (float)cfg_.theme;
    } else {
        float halfW = e.width * 0.5f / zoom_, halfH = e.height * 0.5f / zoom_;
        Vec2 vmin{cam_.x - halfW, cam_.y - halfH}, vmax{cam_.x + halfW, cam_.y + halfH};
        buildLists(*world_, lists_, vmin, vmax, trackedCell(), ui_.hoveredIdx, (float)time_);
        scene_.camX = cam_.x;
        scene_.camY = cam_.y;
        scene_.zoom = zoom_;
        scene_.time = (float)time_;
        scene_.dishRadius = world_->settings.dishRadius;
        scene_.light = std::min(world_->settings.lightAmount, 2.0f) * 0.5f * world_->daylight();  // dark at night
        scene_.night = world_->settings.dayLength > 0.01f ? 1.0f - world_->daylight() : 0.0f;
        scene_.lightAngle = world_->settings.lightAngle;
        scene_.lightRange = world_->settings.lightRange;
        scene_.gridAlpha = cfg_.showGrid ? 0.35f : 0.0f;
        scene_.theme = (float)cfg_.theme;
    }
}

// GPU work only (no access to the world): the incubation thread keeps stepping meanwhile.
void App::renderFrame() {
    if (!ctx_.beginFrame()) {
        ImGui::Render();  // keep ImGui's frame state consistent even when skipping
        return;
    }
    VkExtent2D e = ctx_.extent();
    const bool genome = ui_.screen == Screen::Genome;
    ImGui::Render();

    ctx_.beginMainPass(kBg);
    if (genome) renderer_.draw(previewLists_, previewScene_, 1, e);
    else renderer_.draw(lists_, scene_, 0, e);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), ctx_.cmd());
    ctx_.endFrame();
}

void App::run() {
    double last = nowSeconds();
    double startTime = last;
    while (!glfwWindowShouldClose(window_)) {
        double frameStart = nowSeconds();
        glfwPollEvents();
        if (opt_.exitAfterSeconds > 0 && nowSeconds() - startTime > opt_.exitAfterSeconds) break;
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
            if (cfg_.pauseWhenMinimized) {
                simMode_ = SimIdle;
                glfwWaitEventsTimeout(0.1);
            } else {
                // Keep the dish alive in the background (no rendering).
                double now = nowSeconds();
                {
                    std::unique_lock<std::mutex> lk = lockWorld();
                    advanceSimulation(now - last);
                }
                last = now;
                glfwWaitEventsTimeout(0.005);
            }
            continue;
        }
        auto ema = [](double& v, double ms) { v = v * 0.9 + ms * 0.1; };
        // Everything up to the render lists works on the dish: the incubation thread waits meanwhile.
        const double tWait = nowSeconds();
        std::unique_lock<std::mutex> worldLock = lockWorld();
        const double tLocked = nowSeconds();
        ema(perf_.waitMs, (tLocked - tWait) * 1000.0);
        if (mobile_) {  // turned sideways or back: rebuild the fonts at that orientation's scale
            const ImVec2 ds = ImGui::GetIO().DisplaySize;
            const bool land = ds.x > ds.y && ds.y > 0;
            if (land != landscapeScaled_) {
                landscapeScaled_ = land;
                fontsDirty_ = true;
            }
        }
        if (fontsDirty_) {
            // UI scale changed in the settings: new font atlas and style sizes.
            vkDeviceWaitIdle(ctx_.device());
            ImGui_ImplVulkan_DestroyFontsTexture();
            buildFonts();
            applyStyle();
            ImGui_ImplVulkan_CreateFontsTexture();
            fontsDirty_ = false;
        }
        double now = nowSeconds();
        double dt = now - last;
        last = now;
        time_ += dt;
        frameDt_ = (float)dt;
        // Frames actually shown in the last second (an average of 1/dt overstates it when frame times vary).
        ++perf_.framesThisSecond;
        if (now - perf_.secondStart >= 1.0) {
            perf_.fpsCounted = perf_.framesThisSecond / (now - perf_.secondStart);
            perf_.framesThisSecond = 0;
            perf_.secondStart = now;
        }
        fps_ = perf_.fpsCounted > 0 ? perf_.fpsCounted : fps_;

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

#ifdef __ANDROID__
        if (const char* msg = glfwAndroidTakeMessage()) toast(msg);  // file saved / picker errors
        pinchSteps_ = glfwAndroidTakePinch();
        {
            int l, t, r, b;
            glfwAndroidGetSafeInsets(&l, &t, &r, &b);
            safe_ = ImVec4((float)l, (float)t, (float)r, (float)b);
            int cr[16];
            const int nc = glfwAndroidGetCutout(cr, 4);
            cutout_.clear();
            for (int i = 0; i < nc; ++i) cutout_.push_back(ImVec4((float)cr[i * 4], (float)cr[i * 4 + 1], (float)cr[i * 4 + 2], (float)cr[i * 4 + 3]));
        }
#else
        if (mobile_) safe_ = ImVec4(opt_.safe[0], opt_.safe[1], opt_.safe[2], opt_.safe[3]);
        cutout_.clear();
        if (opt_.cutout[2] > opt_.cutout[0]) cutout_.push_back(ImVec4(opt_.cutout[0], opt_.cutout[1], opt_.cutout[2], opt_.cutout[3]));
#endif
        if (!pendingDrops_.empty()) {
            std::vector<std::string> drops;
            drops.swap(pendingDrops_);
            for (const std::string& p : drops) importPath(p);
        }
        handleInput();
        advanceSimulation(dt);
        updatePreview();  // before the UI: the editor reads the current layout
        drawUi();
        if (ui_.previewDirty || builtTick_ != editorTick_) updatePreview();  // apply this frame's edits

        // A camera that went non-finite or flew off (a bad zoom centre) is reset instead of leaving a black view.
        {
            auto bad = [](Vec2 v, float lim) { return !std::isfinite(v.x) || !std::isfinite(v.y) || std::fabs(v.x) > lim || std::fabs(v.y) > lim; };
            const float lim = world_->settings.dishRadius * 20.0f + 1e4f;
            if (bad(cam_, lim) || bad(camTarget_, lim) || !std::isfinite(zoom_) || !std::isfinite(zoomTarget_) || zoom_ <= 0) fitCamera(true);
            if (bad(previewCam_, 1e6f) || !std::isfinite(previewZoom_) || previewZoom_ <= 0) {
                previewCam_ = {0, 0};
                previewZoom_ = 2.0f;
                previewAutoFrame_ = true;
            }
        }
        // Camera smoothing and cell following.
        int t = trackedCell();
        if (ui_.follow && t >= 0) {
            // Where the cell is drawn (between the last two ticks), not where the last tick put it: aimed at the tick
            // position, the camera stepped once a tick while the cells glided, and the whole view shimmered.
            const Cell& tc = world_->cells()[t];
            camTarget_ = tc.prevPos + (tc.pos - tc.prevPos) * renderAlpha_;
            if (mobile_ && freeMax_.x > freeMin_.x) {  // keep it in the middle of the part of the dish that is visible
                VkExtent2D e = ctx_.extent();
                camTarget_ += Vec2{-((freeMin_.x + freeMax_.x) * 0.5f - e.width * 0.5f) / zoomTarget_,
                                   ((freeMin_.y + freeMax_.y) * 0.5f - e.height * 0.5f) / zoomTarget_};
            }
        }
        float k = 1.0f - std::exp(-(float)dt * mixf(40.0f, 4.0f, std::clamp(cfg_.cameraSmoothing, 0.0f, 1.0f)));
        zoom_ += (zoomTarget_ - zoom_) * k;
        cam_ = cam_ + (camTarget_ - cam_) * k;
        if (!opt_.tracePath.empty() && t >= 0) {  // smoothness test: where the tracked cell is drawn, frame by frame
            static FILE* tf = openFileUtf8(opt_.tracePath, "w");
            const Cell& tc = world_->cells()[t];
            const Vec2 p = tc.prevPos + (tc.pos - tc.prevPos) * renderAlpha_;
            if (tf) std::fprintf(tf, "%.5f %d %.3f %.4f %.4f %.4f %.4f\n", now, lastFrameTicks_, renderAlpha_, p.x, p.y, cam_.x, cam_.y);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) ctx_.requestCapture();
        static bool autoShotRequested = false;
        if (!opt_.screenshotPath.empty() && !autoShotRequested && now - startTime > opt_.screenshotAfter) {
            ctx_.requestCapture();
            autoShotRequested = true;
        }

        if (opt_.screenshotPath.empty() && cfg_.autosaveMinutes > 0 && now - lastAutosave_ > cfg_.autosaveMinutes * 60.0) {
            lastAutosave_ = now;
            std::string path = dataDir("substrates") + "/autosave.substrate";
            if (saveWorld(*world_, path.c_str())) toast("Автосохранение: substrates/autosave.substrate");
        }
        ema(perf_.uiMs, (nowSeconds() - tLocked) * 1000.0);
        const double tLists = nowSeconds();
        prepareFrame();
        ema(perf_.listsMs, (nowSeconds() - tLists) * 1000.0);
        // The simulation runs on its own thread while this one draws: slow / observe at their tick rate, incubation
        // flat out. The frame never waits for the simulation beyond one tick.
        if (ui_.speed == Speed::Slow || ui_.speed == Speed::Observe) {
            simRate_ = ui_.speed == Speed::Slow ? cfg_.slowTicksPerSecond : cfg_.observeTicksPerSecond;
            simMode_ = SimFixedRate;
        } else {
            simMode_ = ui_.speed == Speed::Incubate ? SimFlatOut : SimIdle;
        }
        worldLock.unlock();

        const double tRender = nowSeconds();
        renderFrame();
        ema(perf_.renderMs, (nowSeconds() - tRender) * 1000.0);
        ema(perf_.frameMs, (nowSeconds() - frameStart) * 1000.0);
        saveCapture();

        if (opt_.exitAfterSeconds > 0 && now - startTime > opt_.exitAfterSeconds) glfwSetWindowShouldClose(window_, 1);

        // Settings are written shortly after the last change, so a crash does not lose them.
        if (opt_.screenshotPath.empty() && cfgDirtyAt_ >= 0 && time_ - cfgDirtyAt_ > 1.0) {
            saveConfig(cfg_, cfgPath_);
            cfgDirtyAt_ = -1;
        }
        // Frame cap (only meaningful without vsync).
        // Incubation: every frame holds the world for its UI and render lists (a few ms), so frames are spaced out
        // to leave the rest of the time to the simulation thread.
        if (cfg_.incubateFps > 0 && ui_.speed == Speed::Incubate) {
            double until = frameStart + 1.0 / cfg_.incubateFps;
            double left = until - nowSeconds();
            if (left > 0.0005) std::this_thread::sleep_for(std::chrono::duration<double>(left));
        } else if (cfg_.fpsLimit > 0 && !ctx_.vsync()) {
            double until = frameStart + 1.0 / cfg_.fpsLimit;
            double left = until - nowSeconds();
            if (left > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(left - 0.0015));
            while (nowSeconds() < until) std::this_thread::yield();
        }
    }
}
