#pragma once
// Mod files and packs.
//
// A mod is a folder (mods/<name>/) or one packed file (mods/<name>.cfmod) holding:
//   mod.txt              what it is (id, name, version, author, description), how often it ticks and its settings
//   mod.dpt              optional: the dypt script (entry points загрузка / тик / выгрузка, see docs/mods.md)
//   preview.png          optional: its picture in the «Модификации» window
//   native/<platform>/   optional: shared libraries speaking sdk/cellfap_mod.h
//   shaders/             optional: SPIR-V replacing stages of the game's pipelines (mod_shaders.hpp)
//   assets/ and anything else it uses: genomes (*.genome.txt), programs (*.dpt), textures, sounds, data.
// A pack (.cfmod) is the folder as a standard ZIP archive (zip.hpp): any archiver makes one and shows what is inside.
// The first packs ("CFMOD1\n" + one zlib stream) are still read. Packs are what players share; the folder is what an
// author edits. The game and tools/modpack build packs with buildModPack.
#include <map>
#include <string>
#include <vector>

using ModFiles = std::map<std::string, std::string>;  // path inside the mod ("genomes/a.genome.txt") -> contents

struct ModPackLimits {
    static constexpr size_t kMaxFiles = 512;
    static constexpr size_t kMaxBytes = 64u << 20;  // all files together, unpacked (native libraries are big)
};

// A path inside a mod: relative, '/' separators, no "..", no drive or root. Returns the clean form or "".
std::string modRelativePath(const std::string& path);

// A file of a mod by the path a script or a library names: as given, else under assets/ ("g.genome.txt" finds
// "assets/g.genome.txt"). Null when there is none (or the path tries to leave the mod).
const std::string* findModFile(const ModFiles& files, const std::string& path);

bool readModFolder(const std::string& dir, ModFiles& out, std::string& error);
bool packModFiles(const ModFiles& files, std::string& bytes, std::string& error);  // a ZIP archive
bool unpackModFiles(const std::string& bytes, ModFiles& out, std::string& error);  // ZIP (or the first format)
bool readModPack(const std::string& path, ModFiles& out, std::string& error);

// Why the game would refuse these files as a mod ("" when it would take them): mod.txt, mod.dpt's forbidden
// functions, static libraries in native/, shader files that are not SPIR-V. notes: things worth knowing that are not
// errors (a GLSL source without its .spv...).
std::string modPackProblem(const ModFiles& files, std::vector<std::string>* notes = nullptr);

struct ModPackOptions {
    bool compileShaders = false;            // shaders/*.vert|.frag -> .spv first (glslc)
    std::string glslc;                      // findGlslc()
    std::vector<std::string> includeDirs;   // where the game's common.glsl lies
    std::string* log = nullptr;             // what the compiler did
    std::vector<std::string>* notes = nullptr;
};

// Checks a mod (modPackProblem) and writes the folder as a pack. outPath empty: <folder>.cfmod next to the folder —
// callers pass a path outside mods/ (a pack beside its folder in mods/ would be a second mod with the same id).
// On success outPath holds the file written.
bool buildModPack(const std::string& dir, std::string& outPath, std::string& error, const ModPackOptions* opt = nullptr);

// Whole files with UTF-8 paths.
bool readWholeFile(const std::string& path, std::string& out, size_t maxBytes = ModPackLimits::kMaxBytes);
bool writeWholeFile(const std::string& path, const std::string& data);
