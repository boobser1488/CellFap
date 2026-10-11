cmake_minimum_required(VERSION 3.20)
project(CellFap LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Vulkan REQUIRED)
find_program(GLSLC glslc HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin" REQUIRED)

# Prefer static GLFW so the executable only needs the system Vulkan loader.
if(MINGW)
    find_library(GLFW_STATIC NAMES libglfw3.a)
endif()
if(GLFW_STATIC)
    # Static GLFW pulls in Win32 GDI symbols.
    set(GLFW_LIB ${GLFW_STATIC} gdi32)
else()
    find_package(glfw3 REQUIRED)
    set(GLFW_LIB glfw)
endif()
message(STATUS "GLFW: ${GLFW_LIB}")

# ---------------------------------------------------------------- shaders -> embedded SPIR-V
set(SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADERS dish.vert dish.frag cell.vert cell.frag line.vert line.frag particle.vert particle.frag)
set(SHADER_INCS)
foreach(s ${SHADERS})
    set(out ${SHADER_OUT}/${s}.inc)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${GLSLC} -O -mfmt=c -I ${SHADER_DIR} ${SHADER_DIR}/${s} -o ${out}
        DEPENDS ${SHADER_DIR}/${s} ${SHADER_DIR}/common.glsl
        COMMENT "glslc ${s}")
    list(APPEND SHADER_INCS ${out})
endforeach()
add_custom_target(shaders DEPENDS ${SHADER_INCS})

# ---------------------------------------------------------------- imgui
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(imgui PUBLIC Vulkan::Vulkan ${GLFW_LIB})
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# ---------------------------------------------------------------- simulation core (no graphics)
add_library(cellsim STATIC src/sim/genome.cpp src/sim/world.cpp src/sim/serialize.cpp src/sim/presets.cpp src/sim/layout.cpp src/sim/cellab_import.cpp
            src/sim/challenges.cpp)
target_include_directories(cellsim PUBLIC src)
find_package(ZLIB REQUIRED)
if(MINGW)
    find_library(ZLIB_STATIC NAMES libz.a)
endif()
target_link_libraries(cellsim PUBLIC $<IF:$<BOOL:${ZLIB_STATIC}>,${ZLIB_STATIC},ZLIB::ZLIB>)
if(MINGW)
    target_compile_options(cellsim PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
                                           $<$<CONFIG:Release>:-O3 -march=x86-64-v2 -fno-math-errno>)
endif()

target_sources(cellsim PRIVATE src/sim/scripted.cpp)

# ---------------------------------------------------------------- organisms written in dypt -> organisms.dll
# scripts/organisms.dpt is compiled by the dypt compiler into a native DLL that the game loads at start-up
# (see src/sim/scripted.cpp). Without dypt the game still builds; the dypt organisms are then missing.
find_program(DYPT NAMES dypt_interpreter dypt HINTS "$ENV{DYPT_HOME}/bin" "C:/projects/rust/dypt/target/release")
if(DYPT)
    set(ORGANISMS_DLL ${CMAKE_BINARY_DIR}/organisms.dll)
    add_custom_command(
        OUTPUT ${ORGANISMS_DLL}
        COMMAND ${DYPT} ${CMAKE_SOURCE_DIR}/scripts/organisms.dpt --lib --native --cc ${CMAKE_C_COMPILER} -o ${ORGANISMS_DLL}
        DEPENDS ${CMAKE_SOURCE_DIR}/scripts/organisms.dpt
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "dypt organisms.dpt -> organisms.dll")
    add_custom_target(organisms ALL DEPENDS ${ORGANISMS_DLL})
    message(STATUS "dypt: ${DYPT}")
else()
    message(WARNING "dypt not found: organisms.dll (worms written in dypt) will not be built")
endif()

add_executable(sim_bench tools/sim_bench.cpp)
target_link_libraries(sim_bench PRIVATE cellsim)
if(DYPT)
    add_dependencies(sim_bench organisms)
endif()
if(MINGW)
    target_link_options(sim_bench PRIVATE -static)
endif()

add_executable(export_organisms tools/export_organisms.cpp)
target_link_libraries(export_organisms PRIVATE cellsim)
if(MINGW)
    target_link_options(export_organisms PRIVATE -static)
endif()

add_executable(evolve tools/evolve.cpp)
target_link_libraries(evolve PRIVATE cellsim)
if(MINGW)
    target_link_options(evolve PRIVATE -static)
endif()

# ---------------------------------------------------------------- game
add_executable(CellFap
    src/main.cpp
    src/app.cpp
    src/ui.cpp
    src/ui_genome.cpp
    src/ui_challenges.cpp
    src/ui_mobile.cpp
    src/audio.cpp
    src/ui_files.cpp
    src/config.cpp
    src/render/vk_context.cpp
    src/render/renderer.cpp)
target_link_libraries(CellFap PRIVATE cellsim)
add_dependencies(CellFap shaders)
if(DYPT)
    add_dependencies(CellFap organisms)
endif()
target_include_directories(CellFap PRIVATE src ${SHADER_OUT})
target_link_libraries(CellFap PRIVATE imgui Vulkan::Vulkan ${GLFW_LIB})
if(WIN32)
    target_link_libraries(CellFap PRIVATE comdlg32 winmm shell32)
    enable_language(RC)
    target_sources(CellFap PRIVATE resources/cellfap.rc)  # app icon
endif()

# App icon for Android and Windows (run from the project root after changing the drawing).
add_executable(make_icon tools/make_icon.cpp)
target_link_libraries(make_icon PRIVATE cellsim)  # for zlib
if(MINGW)
    target_link_options(make_icon PRIVATE -static)
endif()

if(MINGW)
    target_link_options(CellFap PRIVATE -static -static-libgcc -static-libstdc++ -mwindows)
    target_compile_options(CellFap PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    target_compile_options(CellFap PRIVATE $<$<CONFIG:Release>:-O3 -march=x86-64-v2 -fno-math-errno>)
    target_compile_options(imgui PRIVATE $<$<CONFIG:Release>:-O2>)
elseif(MSVC)
    target_compile_options(CellFap PRIVATE /W3 /utf-8 $<$<CONFIG:Release>:/O2 /fp:fast>)
    set_target_properties(CellFap PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(CellFap PRIVATE /ENTRY:mainCRTStartup)
endif()
