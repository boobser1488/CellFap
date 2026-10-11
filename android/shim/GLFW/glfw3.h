// The subset of GLFW that CellFap uses, implemented on Android with android_native_app_glue
// (android/android_main.cpp). The desktop build uses the real GLFW; this header only exists in the Android build.
#pragma once

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLFWwindow GLFWwindow;
typedef struct GLFWmonitor GLFWmonitor;
typedef struct GLFWvidmode {
    int width, height, redBits, greenBits, blueBits, refreshRate;
} GLFWvidmode;

typedef void (*GLFWerrorfun)(int, const char*);
typedef void (*GLFWframebuffersizefun)(GLFWwindow*, int, int);
typedef void (*GLFWdropfun)(GLFWwindow*, int, const char**);

#define GLFW_FALSE 0
#define GLFW_TRUE 1
#define GLFW_RELEASE 0
#define GLFW_PRESS 1
#define GLFW_MOUSE_BUTTON_LEFT 0
#define GLFW_MOUSE_BUTTON_RIGHT 1
#define GLFW_MOUSE_BUTTON_MIDDLE 2
#define GLFW_ICONIFIED 0x00020002
#define GLFW_FOCUS_ON_SHOW 0x0002000C
#define GLFW_CLIENT_API 0x00022001
#define GLFW_NO_API 0

int glfwInit(void);
void glfwTerminate(void);
GLFWerrorfun glfwSetErrorCallback(GLFWerrorfun cb);
int glfwVulkanSupported(void);
const char** glfwGetRequiredInstanceExtensions(uint32_t* count);
VkResult glfwCreateWindowSurface(VkInstance instance, GLFWwindow* window, const VkAllocationCallbacks* alloc, VkSurfaceKHR* surface);

void glfwWindowHint(int hint, int value);
GLFWwindow* glfwCreateWindow(int width, int height, const char* title, GLFWmonitor* monitor, GLFWwindow* share);
void glfwDestroyWindow(GLFWwindow* window);
int glfwWindowShouldClose(GLFWwindow* window);
void glfwSetWindowShouldClose(GLFWwindow* window, int value);
void glfwSetWindowUserPointer(GLFWwindow* window, void* p);
void* glfwGetWindowUserPointer(GLFWwindow* window);
GLFWframebuffersizefun glfwSetFramebufferSizeCallback(GLFWwindow* window, GLFWframebuffersizefun cb);
GLFWdropfun glfwSetDropCallback(GLFWwindow* window, GLFWdropfun cb);
void glfwGetWindowSize(GLFWwindow* window, int* w, int* h);
void glfwGetFramebufferSize(GLFWwindow* window, int* w, int* h);
void glfwGetWindowPos(GLFWwindow* window, int* x, int* y);
void glfwSetWindowPos(GLFWwindow* window, int x, int y);
void glfwGetWindowContentScale(GLFWwindow* window, float* xs, float* ys);
int glfwGetWindowAttrib(GLFWwindow* window, int attrib);
void glfwShowWindow(GLFWwindow* window);
void glfwRestoreWindow(GLFWwindow* window);
void glfwSetWindowMonitor(GLFWwindow* window, GLFWmonitor* monitor, int x, int y, int w, int h, int refresh);

GLFWmonitor* glfwGetPrimaryMonitor(void);
GLFWmonitor** glfwGetMonitors(int* count);
void glfwGetMonitorPos(GLFWmonitor* monitor, int* x, int* y);
const GLFWvidmode* glfwGetVideoMode(GLFWmonitor* monitor);

void glfwPollEvents(void);
void glfwWaitEvents(void);
void glfwWaitEventsTimeout(double seconds);
void glfwGetCursorPos(GLFWwindow* window, double* x, double* y);
int glfwGetMouseButton(GLFWwindow* window, int button);

// Android only: the native window can disappear (app in the background) and come back. The renderer releases its
// Vulkan surface in `lost` and creates a new one in `restored`.
void glfwAndroidSetSurfaceCallbacks(void (*lost)(void*), void (*restored)(void*), void* user);
// Screen orientation: 0 = follow the device, 1 = landscape, 2 = portrait.
void glfwAndroidSetOrientation(int mode);
// System file picker ("open"): the chosen file is copied into the app's storage and delivered through the drop callback.
void glfwAndroidPickFile(void);
// System "save as" dialog: copies the file at `path` to a place the user picks (suggested name `name`).
void glfwAndroidExportFile(const char* path, const char* name);
// System share sheet (messengers): the file at `path` as `name`, with `text` as the message.
void glfwAndroidShareFile(const char* path, const char* name, const char* text);
// Next message from the Java side for the player (file saved, errors), or NULL.
const char* glfwAndroidTakeMessage(void);
// Two-finger pinch since the last call, in zoom steps of 1.18x (> 0 = spread the fingers).
float glfwAndroidTakePinch(void);
// Screen area covered by the camera cutout (pixels from each edge); the interface keeps out of it.
void glfwAndroidGetSafeInsets(int* left, int* top, int* right, int* bottom);

#ifdef __cplusplus
}
#endif
