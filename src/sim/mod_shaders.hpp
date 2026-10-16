#pragma once
// Shaders in mods (docs/mods.md, «Шейдеры»): a mod replaces stages of the game's four scene pipelines with its own
// SPIR-V, shaders/<slot>.vert.spv and shaders/<slot>.frag.spv (slot = dish, cell, line, particle). A stage it does not
// ship stays the game's own. GLSL sources (<slot>.vert / <slot>.frag) may lie beside them: the packer compiles them
// with glslc when it can find one; the game itself only ever loads .spv (a phone has no compiler).
#include "modpack.hpp"

#include <cstdint>

#include <string>
#include <vector>

constexpr int kModShaderSlots = 4;  // the order of Renderer::ShaderSlot
const char* modShaderSlotName(int slot);  // "dish", "cell", "line", "particle"

struct ModShaders {
    std::vector<uint32_t> vert[kModShaderSlots], frag[kModShaderSlots];  // empty: the game's own stage
    bool any() const {
        for (int s = 0; s < kModShaderSlots; ++s)
            if (!vert[s].empty() || !frag[s].empty()) return true;
        return false;
    }
    bool has(int s) const { return !vert[s].empty() || !frag[s].empty(); }
};

// The SPIR-V a mod ships. false: a file is not SPIR-V (the reason in error); files of unknown slots are reported too.
bool modShadersOf(const ModFiles& files, ModShaders& out, std::string& error);

// The compiler: the configured path, else glslc of the Vulkan SDK ($VULKAN_SDK), else "glslc" on PATH. "" on Android.
std::string findGlslc(const std::string& configured);

// Compiles every shaders/<slot>.vert|.frag of a mod folder whose .spv is missing or older, next to the source.
// includeDirs: where #include "common.glsl" may come from besides the mod's own shaders/. log: what was done.
// false: a source did not compile, or there is no compiler and a .spv is missing.
bool compileModShaders(const std::string& modDir, const std::string& glslc, const std::vector<std::string>& includeDirs,
                       std::string& log);
