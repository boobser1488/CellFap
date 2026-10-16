#pragma once
// Mod files and packs.
//
// A mod is a folder (mods/<name>/) or one packed file (mods/<name>.cfmod) holding:
//   mod.txt   what it is (id, name, version, author, description), how often it ticks and its settings
//   mod.dpt   the dypt script (entry points загрузка / тик / выгрузка, see docs/mods.md)
//   anything else it uses: genomes (*.genome.txt), programs (*.dpt), text.
// A pack is the folder in one file: "CFMOD1\n", the sizes, then the files, compressed with zlib. Packs are what players
// share; the folder is what an author edits. The game and tools/modpack build packs with buildModPack.
#include <map>
#include <string>

using ModFiles = std::map<std::string, std::string>;  // path inside the mod ("genomes/a.genome.txt") -> contents

struct ModPackLimits {
    static constexpr size_t kMaxFiles = 512;
    static constexpr size_t kMaxBytes = 16u << 20;  // all files together, unpacked
};

// A path inside a mod: relative, '/' separators, no "..", no drive or root. Returns the clean form or "".
std::string modRelativePath(const std::string& path);

bool readModFolder(const std::string& dir, ModFiles& out, std::string& error);
bool packModFiles(const ModFiles& files, std::string& bytes, std::string& error);
bool unpackModFiles(const std::string& bytes, ModFiles& out, std::string& error);
bool readModPack(const std::string& path, ModFiles& out, std::string& error);

// Checks a mod (mod.txt with an id and a name, mod.dpt present) and writes the folder as a pack. outPath empty:
// <folder>.cfmod next to the folder. On success outPath holds the file written.
bool buildModPack(const std::string& dir, std::string& outPath, std::string& error);

// Whole files with UTF-8 paths.
bool readWholeFile(const std::string& path, std::string& out, size_t maxBytes = ModPackLimits::kMaxBytes);
bool writeWholeFile(const std::string& path, const std::string& data);
