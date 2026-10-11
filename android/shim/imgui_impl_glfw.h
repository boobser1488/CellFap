// Android build: the game calls the GLFW backend; route it to Dear ImGui's Android backend. Touch input is fed by
// android/android_main.cpp (it turns fingers into mouse, pan and pinch events).
#pragma once
#include "imgui.h"

struct GLFWwindow;
bool ImGui_ImplGlfw_InitForVulkan(GLFWwindow* window, bool install_callbacks);
void ImGui_ImplGlfw_Shutdown();
void ImGui_ImplGlfw_NewFrame();
