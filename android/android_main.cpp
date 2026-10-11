// Android entry point and the GLFW subset (android/shim/GLFW/glfw3.h) on top of android_native_app_glue.
//
// Touch: one finger is the left mouse button (tools, UI); two fingers pan the view (right button held at their
// midpoint) and pinch to zoom (steps of 1.18x handed to the game by glfwAndroidTakePinch). A new touch becomes a
// press only after ~70 ms or once it moves: a second finger arriving in that time turns it into a pinch without
// clicking anything. After the finger lifts, ImGui gets "no mouse" so nothing stays hovered (no stuck tooltips).
// The hardware Back key is Escape.
// Java side (android/java/.../CellFapActivity.java): orientation, the on-screen keyboard, file picker and export.
// Lifecycle: when the app goes to the background Android takes the window away; the renderer drops its Vulkan
// surface (callback) and the main loop sees the window as iconified until a new window arrives.
#include <GLFW/glfw3.h>

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_internal.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <unistd.h>

#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

int cellfap_main(int argc, char** argv);  // src/main.cpp (renamed on Android)

namespace {

struct GLFWwindowState {
    void* user = nullptr;
    GLFWframebuffersizefun sizeCb = nullptr;
    GLFWdropfun dropCb = nullptr;
    bool shouldClose = false;
};

android_app* gApp = nullptr;
ANativeWindow* gWindow = nullptr;
GLFWwindowState gState;
bool gHadSurface = false;
int gLastW = 0, gLastH = 0;
void (*gSurfaceLost)(void*) = nullptr;
void (*gSurfaceRestored)(void*) = nullptr;
void* gSurfaceUser = nullptr;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void log(const char* msg) { __android_log_print(ANDROID_LOG_INFO, "CellFap", "%s", msg); }
GLFWwindow* windowHandle() { return reinterpret_cast<GLFWwindow*>(&gState); }

// ---------------------------------------------------------------- Java bridge
JNIEnv* gEnv = nullptr;  // the game thread's
jobject gActivity = nullptr;
jmethodID gSetOrientation = nullptr, gShowKeyboard = nullptr, gPickFile = nullptr, gExportFile = nullptr, gShareFile = nullptr,
          gOpenInApp = nullptr, gPlayClick = nullptr, gGestureExclusion = nullptr;

// Events from the Java UI thread, applied on the game thread in pump().
struct JavaEvent {
    enum Kind { Text, Backspace, Enter, File, Message, Ime, Safe } kind;
    std::string s;
    int n = 0;
    int q[4] = {};
};
std::mutex gJavaMutex;
std::vector<JavaEvent> gJavaEvents;
int gImeInset = 0;  // pixels of the window covered by the on-screen keyboard
int gSafe[4] = {};  // left, top, right, bottom: pixels taken by the camera cutout
std::vector<int> gCutout;  // the cutout's parts: left, top, right, bottom each (guarded by gJavaMutex)

void pushJava(JavaEvent e) {
    std::lock_guard<std::mutex> lock(gJavaMutex);
    gJavaEvents.push_back(std::move(e));
}
std::string str(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}
void JNICALL jniText(JNIEnv* env, jclass, jstring s) { pushJava({JavaEvent::Text, str(env, s)}); }
void JNICALL jniBackspace(JNIEnv*, jclass, jint n) { pushJava({JavaEvent::Backspace, {}, (int)n}); }
void JNICALL jniEnter(JNIEnv*, jclass) { pushJava({JavaEvent::Enter}); }
void JNICALL jniFilePicked(JNIEnv* env, jclass, jstring s) { pushJava({JavaEvent::File, str(env, s)}); }
void JNICALL jniMessage(JNIEnv* env, jclass, jstring s) { pushJava({JavaEvent::Message, str(env, s)}); }
void JNICALL jniImeInset(JNIEnv*, jclass, jint px) { pushJava({JavaEvent::Ime, {}, (int)px}); }
void JNICALL jniCutout(JNIEnv* env, jclass, jintArray a) {
    std::vector<int> v(a ? env->GetArrayLength(a) : 0);
    if (!v.empty()) env->GetIntArrayRegion(a, 0, (jsize)v.size(), v.data());
    std::lock_guard<std::mutex> lock(gJavaMutex);
    gCutout = std::move(v);
}
void JNICALL jniSafeInsets(JNIEnv*, jclass, jint l, jint t, jint r, jint b) {
    JavaEvent e{JavaEvent::Safe};
    e.q[0] = l, e.q[1] = t, e.q[2] = r, e.q[3] = b;
    pushJava(e);
}

void initJava(android_app* app) {
    if (app->activity->vm->AttachCurrentThread(&gEnv, nullptr) != JNI_OK) {
        gEnv = nullptr;
        return;
    }
    gActivity = app->activity->clazz;
    jclass cls = gEnv->GetObjectClass(gActivity);
    auto method = [&](const char* name, const char* sig) {
        jmethodID m = gEnv->GetMethodID(cls, name, sig);
        if (gEnv->ExceptionCheck()) {  // an activity without our Java class: that feature stays off
            gEnv->ExceptionClear();
            m = nullptr;
        }
        return m;
    };
    gSetOrientation = method("setOrientation", "(I)V");
    gShowKeyboard = method("showKeyboard", "(Z)V");
    gPickFile = method("pickFile", "()V");
    gExportFile = method("exportFile", "(Ljava/lang/String;Ljava/lang/String;)V");
    gShareFile = method("shareFile", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
    gOpenInApp = method("openInApp", "(Ljava/lang/String;Ljava/lang/String;Z)V");
    gPlayClick = method("playClick", "(F)V");
    gGestureExclusion = method("setGestureExclusion", "(IIII)V");
    jmethodID ready = method("onNativeReady", "()V");
    static const JNINativeMethod natives[] = {
        {"nativeText", "(Ljava/lang/String;)V", (void*)jniText},
        {"nativeBackspace", "(I)V", (void*)jniBackspace},
        {"nativeEnter", "()V", (void*)jniEnter},
        {"nativeFilePicked", "(Ljava/lang/String;)V", (void*)jniFilePicked},
        {"nativeMessage", "(Ljava/lang/String;)V", (void*)jniMessage},
        {"nativeImeInset", "(I)V", (void*)jniImeInset},
        {"nativeSafeInsets", "(IIII)V", (void*)jniSafeInsets},
        {"nativeCutout", "([I)V", (void*)jniCutout},
    };
    if (gEnv->RegisterNatives(cls, natives, sizeof(natives) / sizeof(natives[0])) != 0 || gEnv->ExceptionCheck()) {
        gEnv->ExceptionClear();
        log("RegisterNatives failed: keyboard and files are unavailable");
    }
    gEnv->DeleteLocalRef(cls);
    // Files opened with the app before the game was listening (from a messenger, a file manager) come now.
    if (ready) {
        gEnv->CallVoidMethod(gActivity, ready);
        if (gEnv->ExceptionCheck()) gEnv->ExceptionClear();
    }
}

void callJava(jmethodID m, ...) {
    if (!gEnv || !m) return;
    va_list args;
    va_start(args, m);
    gEnv->CallVoidMethodV(gActivity, m, args);
    va_end(args);
    if (gEnv->ExceptionCheck()) gEnv->ExceptionClear();
}

// Short messages from Java for the player; the app shows them as toasts (glfwAndroidTakeMessage).
std::vector<std::string> gMessages;

// ---------------------------------------------------------------- touch
double gCursorX = 0, gCursorY = 0;
bool gButtons[3] = {};
int gFingers = 0;
float gPinchDist = 0;
float gPinchAccum = 0;  // zoom steps (1.18x) since the game last asked
// A finger that has landed but is not a press yet (it may become a pinch).
bool gPending = false;
double gPendingT = 0;
float gPendingX = 0, gPendingY = 0;
int gFrame = 0, gPressFrame = -1;
bool gReleaseDeferred = false;

float density() {
    int32_t dpi = gApp && gApp->config ? AConfiguration_getDensity(gApp->config) : 160;
    return (dpi > 0 ? dpi : 160) / 160.0f;
}

void imguiPos(float x, float y) {
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
    io.AddMousePosEvent(x, y);
}

void setButton(int b, bool down) {
    if (gButtons[b] == down) return;
    gButtons[b] = down;
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMouseButtonEvent(b, down);
    }
    if (b == 0 && down) gPressFrame = gFrame;
}

void setCursor(double x, double y) {
    gCursorX = x;
    gCursorY = y;
    imguiPos((float)x, (float)y);
}

// The pending touch becomes a real press (at the point where it landed).
void commitPress() {
    if (!gPending) return;
    gPending = false;
    double x = gCursorX, y = gCursorY;
    setCursor(gPendingX, gPendingY);
    setButton(0, true);
    if (x != gPendingX || y != gPendingY) setCursor(x, y);
}

// Lift the left button; a press made in this very frame is released in the next one, so code that polls the button
// state (the dish tools) sees it down at least once.
void releaseLeft() {
    if (!gButtons[0]) return;
    if (gPressFrame == gFrame) {
        gReleaseDeferred = true;
        return;
    }
    setButton(0, false);
    imguiPos(-FLT_MAX, -FLT_MAX);  // nothing stays hovered
}

int32_t onInput(android_app*, AInputEvent* ev) {
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        if (AKeyEvent_getKeyCode(ev) == AKEYCODE_BACK) {
            if (ImGui::GetCurrentContext())
                ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN);
            return 1;  // never let Back kill the dish
        }
        return ImGui_ImplAndroid_HandleInputEvent(ev);
    }
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
    int n = (int)AMotionEvent_getPointerCount(ev);
    if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) n = 0;
    else if (action == AMOTION_EVENT_ACTION_POINTER_UP) n -= 1;

    if (n >= 2) {
        float x0 = AMotionEvent_getX(ev, 0), y0 = AMotionEvent_getY(ev, 0);
        float x1 = AMotionEvent_getX(ev, 1), y1 = AMotionEvent_getY(ev, 1);
        float dist = std::hypot(x1 - x0, y1 - y0);
        if (gFingers < 2) {  // a second finger landed: the first one was not a tap on a tool
            gPending = false;
            if (gButtons[0]) {
                gButtons[0] = false;  // a press already made is withdrawn quietly
                imguiPos(-FLT_MAX, -FLT_MAX);  // released away from any widget: no click
                if (ImGui::GetCurrentContext()) ImGui::GetIO().AddMouseButtonEvent(0, false);
            }
            gReleaseDeferred = false;
            gPinchDist = dist;
        }
        setCursor((x0 + x1) * 0.5, (y0 + y1) * 0.5);
        setButton(1, true);  // pan with the "right button" at the fingers' midpoint
        // The pinch goes to the game directly (glfwAndroidTakePinch), not as ImGui wheel events: ImGui spreads
        // interleaved move + wheel events over separate frames, and the zoom used to lag behind the fingers.
        if (gPinchDist > 1 && dist > 1) {
            gPinchAccum += std::log(dist / gPinchDist) / std::log(1.18f);
            gPinchDist = dist;
        }
        gFingers = n;
        return 1;
    }
    if (n == 1) {
        if (gFingers >= 2) {  // lifting one of two fingers ends the gesture without a click
            setButton(1, false);
            imguiPos(-FLT_MAX, -FLT_MAX);
            gFingers = 1;
            gPending = false;
            return 1;
        }
        int32_t idx = 0;
        if (action == AMOTION_EVENT_ACTION_POINTER_UP) {
            int up = (AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
            idx = up == 0 ? 1 : 0;
        }
        float x = AMotionEvent_getX(ev, idx), y = AMotionEvent_getY(ev, idx);
        if (gFingers == 0) {  // a new touch: wait a moment before it becomes a press
            gPending = true;
            gPendingT = now();
            gPendingX = x;
            gPendingY = y;
            setCursor(x, y);
            // On a panel there is nothing to pinch: press at once (the wait made every button feel slow).
            if (ImGui::GetCurrentContext()) {
                ImGuiWindow* hovered = nullptr;
                ImGuiWindow* under = nullptr;
                ImGui::FindHoveredWindowEx({x, y}, true, &hovered, &under);
                if (hovered) commitPress();
            }
        } else {
            setCursor(x, y);
            if (gPending && std::hypot(x - gPendingX, y - gPendingY) > 10.0f * density()) commitPress();
        }
        gFingers = 1;
        return 1;
    }
    // all fingers up
    if (gFingers == 1) {
        commitPress();  // a quick tap
        releaseLeft();
    }
    if (gButtons[1]) {
        setButton(1, false);
        imguiPos(-FLT_MAX, -FLT_MAX);
    }
    gFingers = 0;
    return 1;
}

void onCmd(android_app* app, int32_t cmd) {
    switch (cmd) {
        case APP_CMD_INIT_WINDOW:
            gWindow = app->window;
            if (gHadSurface && gSurfaceRestored) gSurfaceRestored(gSurfaceUser);
            break;
        case APP_CMD_TERM_WINDOW:
            // The window is about to go: the swapchain and surface must be released before we return.
            if (gWindow && gSurfaceLost) gSurfaceLost(gSurfaceUser);
            gWindow = nullptr;
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONFIG_CHANGED:
        case APP_CMD_CONTENT_RECT_CHANGED:
            if (gState.sizeCb && gWindow) gState.sizeCb(windowHandle(), ANativeWindow_getWidth(gWindow), ANativeWindow_getHeight(gWindow));
            break;
        default: break;
    }
}

void applyJavaEvents() {
    std::vector<JavaEvent> evs;
    {
        std::lock_guard<std::mutex> lock(gJavaMutex);
        evs.swap(gJavaEvents);
    }
    if (evs.empty()) return;
    std::vector<JavaEvent> later;
    ImGuiIO* io = ImGui::GetCurrentContext() ? &ImGui::GetIO() : nullptr;
    for (JavaEvent& e : evs) {
        switch (e.kind) {
            case JavaEvent::Text:
                if (io) io->AddInputCharactersUTF8(e.s.c_str());
                break;
            case JavaEvent::Backspace:
                for (int i = 0; io && i < e.n; ++i) {
                    io->AddKeyEvent(ImGuiKey_Backspace, true);
                    io->AddKeyEvent(ImGuiKey_Backspace, false);
                }
                break;
            case JavaEvent::Enter:
                if (io) {
                    io->AddKeyEvent(ImGuiKey_Enter, true);
                    io->AddKeyEvent(ImGuiKey_Enter, false);
                }
                break;
            case JavaEvent::File: {
                const char* p = e.s.c_str();
                if (gState.dropCb) gState.dropCb(windowHandle(), 1, &p);
                else later.push_back(e);  // the game has not set its handler yet
                break;
            }
            case JavaEvent::Message: gMessages.push_back(e.s); break;
            case JavaEvent::Ime: gImeInset = std::max(0, e.n); break;
            case JavaEvent::Safe:
                for (int k = 0; k < 4; ++k) gSafe[k] = std::max(0, e.q[k]);
                break;
        }
    }
    if (!later.empty()) {
        std::lock_guard<std::mutex> lock(gJavaMutex);
        gJavaEvents.insert(gJavaEvents.begin(), later.begin(), later.end());
    }
}

// Process pending events; timeoutMs < 0 blocks until something happens.
void pump(int timeoutMs) {
    int events = 0;
    android_poll_source* source = nullptr;
    while (ALooper_pollOnce(timeoutMs, nullptr, &events, (void**)&source) >= 0) {
        if (source) source->process(gApp, source);
        if (gApp->destroyRequested) return;
        timeoutMs = 0;  // after the first event just drain the queue
    }
    applyJavaEvents();
    // Touch timing (once per frame).
    ++gFrame;
    if (gReleaseDeferred && gPressFrame != gFrame) {
        gReleaseDeferred = false;
        setButton(0, false);
        imguiPos(-FLT_MAX, -FLT_MAX);
    }
    if (gPending && now() - gPendingT > 0.07) commitPress();
    if (gWindow) {
        int w = ANativeWindow_getWidth(gWindow), h = ANativeWindow_getHeight(gWindow);
        if ((w != gLastW || h != gLastH) && gState.sizeCb) gState.sizeCb(windowHandle(), w, h);
        gLastW = w;
        gLastH = h;
    }
}

}  // namespace

// ---------------------------------------------------------------- GLFW subset
extern "C" {

int glfwInit(void) { return 1; }
void glfwTerminate(void) {}
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun) { return nullptr; }
int glfwVulkanSupported(void) { return 1; }
const char** glfwGetRequiredInstanceExtensions(uint32_t* count) {
    static const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    *count = 2;
    return exts;
}
VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow*, const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface) {
    while (!gWindow && !gApp->destroyRequested) pump(-1);
    if (!gWindow) return VK_ERROR_INITIALIZATION_FAILED;
    VkAndroidSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    ci.window = gWindow;
    gHadSurface = true;
    return vkCreateAndroidSurfaceKHR(instance, &ci, alloc, surface);
}

void glfwWindowHint(int, int) {}
GLFWwindow* glfwCreateWindow(int, int, const char*, GLFWmonitor*, GLFWwindow*) {
    while (!gWindow && !gApp->destroyRequested) pump(-1);
    return gWindow ? windowHandle() : nullptr;
}
void glfwDestroyWindow(GLFWwindow*) {}
int glfwWindowShouldClose(GLFWwindow*) { return gState.shouldClose || gApp->destroyRequested; }
void glfwSetWindowShouldClose(GLFWwindow*, int v) { gState.shouldClose = v != 0; }
void glfwSetWindowUserPointer(GLFWwindow*, void* p) { gState.user = p; }
void* glfwGetWindowUserPointer(GLFWwindow*) { return gState.user; }
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow*, GLFWframebuffersizefun cb) {
    GLFWframebuffersizefun old = gState.sizeCb;
    gState.sizeCb = cb;
    return old;
}
GLFWdropfun glfwSetDropCallback(GLFWwindow*, GLFWdropfun cb) {
    GLFWdropfun old = gState.dropCb;
    gState.dropCb = cb;
    return old;
}
void glfwGetWindowSize(GLFWwindow*, int* w, int* h) {
    *w = gWindow ? ANativeWindow_getWidth(gWindow) : 0;
    *h = gWindow ? ANativeWindow_getHeight(gWindow) : 0;
}
void glfwGetFramebufferSize(GLFWwindow* wnd, int* w, int* h) { glfwGetWindowSize(wnd, w, h); }
void glfwGetWindowPos(GLFWwindow*, int* x, int* y) { *x = *y = 0; }
void glfwSetWindowPos(GLFWwindow*, int, int) {}
void glfwGetWindowContentScale(GLFWwindow*, float* xs, float* ys) {
    // Android density: 160 dpi = 1. Phones are held close, so the UI is a little smaller than "1 dp".
    *xs = *ys = std::max(1.0f, density() * 0.9f);
}
int glfwGetWindowAttrib(GLFWwindow*, int attrib) { return attrib == GLFW_ICONIFIED ? (gWindow == nullptr) : 0; }
void glfwShowWindow(GLFWwindow*) {}
void glfwRestoreWindow(GLFWwindow*) {}
void glfwSetWindowMonitor(GLFWwindow*, GLFWmonitor*, int, int, int, int, int) {}

static GLFWmonitor* const kMonitor = reinterpret_cast<GLFWmonitor*>(&gState);
GLFWmonitor* glfwGetPrimaryMonitor(void) { return kMonitor; }
GLFWmonitor** glfwGetMonitors(int* count) {
    static GLFWmonitor* list[1] = {kMonitor};
    *count = 1;
    return list;
}
void glfwGetMonitorPos(GLFWmonitor*, int* x, int* y) { *x = *y = 0; }
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor*) {
    static GLFWvidmode m{};
    glfwGetWindowSize(nullptr, &m.width, &m.height);
    m.redBits = m.greenBits = m.blueBits = 8;
    m.refreshRate = 60;
    return &m;
}

void glfwPollEvents(void) { pump(0); }
void glfwWaitEvents(void) { pump(-1); }
void glfwWaitEventsTimeout(double seconds) { pump((int)(seconds * 1000.0)); }
void glfwGetCursorPos(GLFWwindow*, double* x, double* y) {
    *x = gCursorX;
    *y = gCursorY;
}
int glfwGetMouseButton(GLFWwindow*, int b) { return (b >= 0 && b < 3 && gButtons[b]) ? GLFW_PRESS : GLFW_RELEASE; }

void glfwAndroidSetSurfaceCallbacks(void (*lost)(void*), void (*restored)(void*), void* user) {
    gSurfaceLost = lost;
    gSurfaceRestored = restored;
    gSurfaceUser = user;
}
void glfwAndroidSetOrientation(int mode) { callJava(gSetOrientation, (jint)mode); }
void glfwAndroidPickFile(void) { callJava(gPickFile); }
void glfwAndroidExportFile(const char* path, const char* name) {
    if (!gEnv || !gExportFile) return;
    jstring p = gEnv->NewStringUTF(path), n = gEnv->NewStringUTF(name);
    callJava(gExportFile, p, n);
    gEnv->DeleteLocalRef(p);
    gEnv->DeleteLocalRef(n);
}
void glfwAndroidGetSafeInsets(int* l, int* t, int* r, int* b) {
    *l = gSafe[0];
    *t = gSafe[1];
    *r = gSafe[2];
    *b = gSafe[3];
}
float glfwAndroidTakePinch(void) {
    float s = gPinchAccum;
    gPinchAccum = 0;
    return s;
}
void glfwAndroidShareFile(const char* path, const char* name, const char* text) {
    if (!gEnv || !gShareFile) return;
    jstring p = gEnv->NewStringUTF(path), n = gEnv->NewStringUTF(name), t = gEnv->NewStringUTF(text);
    callJava(gShareFile, p, n, t);
    gEnv->DeleteLocalRef(p);
    gEnv->DeleteLocalRef(n);
    gEnv->DeleteLocalRef(t);
}
void glfwAndroidOpenInApp(const char* path, const char* name, int genome) {
    if (!gEnv || !gOpenInApp) return;
    jstring p = gEnv->NewStringUTF(path), n = gEnv->NewStringUTF(name);
    callJava(gOpenInApp, p, n, (jboolean)(genome != 0));
    gEnv->DeleteLocalRef(p);
    gEnv->DeleteLocalRef(n);
}
int glfwAndroidGetCutout(int* rects, int maxRects) {
    std::lock_guard<std::mutex> lock(gJavaMutex);
    const int n = std::min((int)gCutout.size() / 4, maxRects);
    for (int i = 0; i < n * 4; ++i) rects[i] = gCutout[i];
    return n;
}
void glfwAndroidSetGestureExclusion(int left, int top, int right, int bottom) {
    static int last[4] = {-1, -1, -1, -1};
    if (last[0] == left && last[1] == top && last[2] == right && last[3] == bottom) return;  // only when it moves
    last[0] = left, last[1] = top, last[2] = right, last[3] = bottom;
    callJava(gGestureExclusion, (jint)left, (jint)top, (jint)right, (jint)bottom);
}
void glfwAndroidSystemClick(float volume) { callJava(gPlayClick, (jdouble)volume); }
const char* glfwAndroidTakeMessage(void) {
    static std::string last;
    if (gMessages.empty()) return nullptr;
    last = gMessages.front();
    gMessages.erase(gMessages.begin());
    return last.c_str();
}

}  // extern "C"

// ---------------------------------------------------------------- ImGui backend mapping
bool ImGui_ImplGlfw_InitForVulkan(GLFWwindow*, bool) { return ImGui_ImplAndroid_Init(gWindow); }
void ImGui_ImplGlfw_Shutdown() { ImGui_ImplAndroid_Shutdown(); }
void ImGui_ImplGlfw_NewFrame() {
    // Not ImGui_ImplAndroid_NewFrame: it keeps the window it was initialised with, which dies when the app goes to
    // the background. Size and time come from here instead.
    ImGuiIO& io = ImGui::GetIO();
    if (gWindow) {
        float w = (float)ANativeWindow_getWidth(gWindow), h = (float)ANativeWindow_getHeight(gWindow);
        // With the on-screen keyboard up the interface lays itself out in the part above it.
        if (gImeInset > 0 && gImeInset < h * 0.7f) h -= (float)gImeInset;
        io.DisplaySize = ImVec2(w, h);
    }
    io.DisplayFramebufferScale = ImVec2(1, 1);
    static double last = 0;
    double t = now();
    io.DeltaTime = last > 0 ? (float)std::max(t - last, 1e-4) : 1.0f / 60.0f;
    last = t;
    // The keyboard follows ImGui's text fields.
    static bool keyboard = false;
    if (io.WantTextInput != keyboard) {
        keyboard = io.WantTextInput;
        callJava(gShowKeyboard, (jboolean)keyboard);
        if (!keyboard) gImeInset = 0;
    }
}

// ---------------------------------------------------------------- entry point
void android_main(android_app* app) {
    gApp = app;
    app->onAppCmd = onCmd;
    app->onInputEvent = onInput;
    // Settings, saved dishes and genomes live in the app's private storage.
    if (app->activity->internalDataPath) chdir(app->activity->internalDataPath);
    initJava(app);
    log("CellFap starting");
    // Test options passed by adb in the "cellfap_test" extra; only a few harmless ones are kept.
    std::vector<std::string> args{"cellfap"};
    if (gEnv && gActivity) {
        jclass cls = gEnv->GetObjectClass(gActivity);
        jmethodID m = gEnv->GetMethodID(cls, "testArgs", "()Ljava/lang/String;");
        if (gEnv->ExceptionCheck()) {
            gEnv->ExceptionClear();
            m = nullptr;
        }
        if (jstring js = m ? (jstring)gEnv->CallObjectMethod(gActivity, m) : nullptr) {
            const char* s = gEnv->GetStringUTFChars(js, nullptr);
            std::vector<std::string> words;
            for (const char* p = s; *p;) {
                while (*p == ' ') ++p;
                const char* b = p;
                while (*p && *p != ' ') ++p;
                if (p > b) words.emplace_back(b, p);
            }
            gEnv->ReleaseStringUTFChars(js, s);
            static const char* withValue[] = {"--screen", "--preset",   "--dock", "--experiment", "--open-cellab",
                                              "--tick",   "--tool-side", "--hours", "--zoom",      "--track-type", "--speed", "--orient"};
            static const char* flags[] = {"--genomes", "--experiments", "--perf", "--tools-open"};
            for (size_t i = 0; i < words.size(); ++i) {
                bool known = false;
                for (const char* a : flags)
                    if (words[i] == a) {
                        args.push_back(words[i]);
                        known = true;
                    }
                if (known) continue;
                for (const char* a : withValue)
                    if (words[i] == a && i + 1 < words.size()) {
                        args.insert(args.end(), {words[i], words[i + 1]});
                        ++i;
                        break;
                    }
            }
        }
    }
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    cellfap_main((int)args.size(), argv.data());
    log("CellFap finished");
    if (gEnv) app->activity->vm->DetachCurrentThread();
    ANativeActivity_finish(app->activity);
    // Let the activity finish: keep pumping until Android destroys us.
    while (!app->destroyRequested) pump(-1);
    std::exit(0);
}
