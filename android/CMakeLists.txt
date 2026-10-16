# Android build of CellFap: one native library (libcellfap.so) for a NativeActivity, no Java code.
# Configured by android/build_apk.sh with the NDK toolchain file; the APK is packed by the same script.
cmake_minimum_required(VERSION 3.18)
project(CellFapAndroid LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(IMGUI ${ROOT}/third_party/imgui)

# ---- shaders -> embedded SPIR-V (glslc ships with the NDK)
find_program(GLSLC glslc HINTS ${ANDROID_NDK}/shader-tools/windows-x86_64 ${ANDROID_NDK}/shader-tools/linux-x86_64 REQUIRED)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_INCS)
foreach(s dish.vert dish.frag cell.vert cell.frag line.vert line.frag particle.vert particle.frag)
    set(out ${SHADER_OUT}/${s}.inc)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${GLSLC} -O -mfmt=c -I ${ROOT}/shaders ${ROOT}/shaders/${s} -o ${out}
        DEPENDS ${ROOT}/shaders/${s} ${ROOT}/shaders/common.glsl)
    list(APPEND SHADER_INCS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_INCS})

# The same sources are embedded as text so the in-game shader editor works on the phone too (it can still not
# compile there: that needs the SDK's glslc, which is not on a phone — the panel says so).
set(SHADER_SRC_HEADER ${CMAKE_BINARY_DIR}/gen/shader_sources.h)
add_custom_command(
    OUTPUT ${SHADER_SRC_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${ROOT}/shaders -DOUTPUT=${SHADER_SRC_HEADER} -P ${ROOT}/scripts/embed_shaders.cmake
    DEPENDS ${ROOT}/shaders/cell.frag ${ROOT}/shaders/cell.vert ${ROOT}/shaders/dish.frag ${ROOT}/shaders/dish.vert
            ${ROOT}/shaders/line.frag ${ROOT}/shaders/line.vert ${ROOT}/shaders/particle.frag ${ROOT}/shaders/particle.vert
            ${ROOT}/shaders/common.glsl
            ${ROOT}/scripts/embed_shaders.cmake
    COMMENT "embed shader sources for the in-game editor")
add_custom_target(shader_sources DEPENDS ${SHADER_SRC_HEADER})
include(${ROOT}/cmake/example_mod.cmake)
cellfap_example_mod(${ROOT}/mods/example ${CMAKE_BINARY_DIR}/gen/example_mod.inc)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(cellfap SHARED
    ${ROOT}/src/sim/genome.cpp ${ROOT}/src/sim/world.cpp ${ROOT}/src/sim/serialize.cpp ${ROOT}/src/sim/presets.cpp
    ${ROOT}/src/sim/layout.cpp ${ROOT}/src/sim/cellab_import.cpp ${ROOT}/src/sim/challenges.cpp ${ROOT}/src/sim/scripted.cpp
    ${ROOT}/src/sim/dypt.cpp ${ROOT}/src/sim/signal_cells.cpp ${ROOT}/src/sim/modpack.cpp ${ROOT}/src/sim/mods.cpp
    ${ROOT}/src/main.cpp ${ROOT}/src/app.cpp ${ROOT}/src/ui.cpp ${ROOT}/src/ui_genome.cpp ${ROOT}/src/ui_challenges.cpp ${ROOT}/src/ui_mobile.cpp ${ROOT}/src/ui_files.cpp ${ROOT}/src/audio.cpp
    ${ROOT}/src/ui_script.cpp ${ROOT}/src/ui_dev.cpp ${ROOT}/src/ui_mods.cpp
    ${ROOT}/src/config.cpp ${ROOT}/src/render/vk_context.cpp ${ROOT}/src/render/renderer.cpp
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp ${IMGUI}/imgui_demo.cpp
    ${IMGUI}/backends/imgui_impl_vulkan.cpp ${IMGUI}/backends/imgui_impl_android.cpp
    ${ROOT}/android/android_main.cpp)
add_dependencies(cellfap shaders shader_sources)
# The shim directory comes first: its GLFW/glfw3.h and imgui_impl_glfw.h replace the desktop ones.
target_include_directories(cellfap PRIVATE ${ROOT}/android/shim ${ROOT}/src ${SHADER_OUT} ${ROOT}/android/generated
                                           ${CMAKE_BINARY_DIR}/gen ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(cellfap PRIVATE VK_USE_PLATFORM_ANDROID_KHR IMGUI_DISABLE_OBSOLETE_FUNCTIONS CELLFAP_EMBEDDED_ORGANISMS)
target_compile_options(cellfap PRIVATE -O3 -fno-math-errno -Wno-unused-parameter)
target_link_libraries(cellfap PRIVATE native_app_glue android log vulkan z aaudio)
# Keep the NativeActivity entry point that the glue library defines.
target_link_options(cellfap PRIVATE -u ANativeActivity_onCreate)
