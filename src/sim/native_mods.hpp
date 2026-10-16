#pragma once
// Native code of mods: shared libraries under native/<platform>/ inside a mod (sdk/cellfap_mod.h, docs/mods.md).
//
// Only what does not depend on the game's state lives here: which files belong to the running platform, their
// fingerprint (the player's consent is tied to it), copying them out of the mod into a cache folder the library is
// loaded from, and opening a library. The API the libraries call is mod_api.cpp; when they are called, mods.cpp.
#include "modpack.hpp"

#include <string>
#include <vector>

// The folder name under native/ for this build ("windows-x64", "android-arm64-v8a", ...) and its library suffix.
const char* nativePlatform();
const char* nativeLibrarySuffix();

struct NativeFiles {
    bool any = false;                    // the mod has a native/ folder at all
    std::vector<std::string> platforms;  // the platforms it ships code for
    std::vector<std::string> here;       // this platform's files (paths inside the mod), sorted
    std::vector<std::string> libraries;  // of those, the shared libraries
    std::string hash;                    // SHA-256 over this platform's files (paths and contents); "" without them
};
NativeFiles nativeFilesOf(const ModFiles& files);

// Picks the library with the entry points: `wanted` (mod.txt `native <file>`) or the only library there is.
bool nativeEntry(const NativeFiles& nf, const std::string& wanted, std::string& entry, std::string& error);

// Writes this platform's files to <cacheRoot>/<id>/<first 16 hex digits of the hash>/ (once: the folder name is the
// content) and removes older versions of the same mod when it can. outDir: the folder written to.
bool extractNative(const ModFiles& files, const NativeFiles& nf, const std::string& cacheRoot, const std::string& id,
                   std::string& outDir, std::string& error);

class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary() { close(); }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    // UTF-8 path. Windows: the library's own folder is searched for its dependencies.
    bool open(const std::string& path, std::string& error);
    void* symbol(const char* name) const;
    void close();
    bool isOpen() const { return h_ != nullptr; }

private:
    void* h_ = nullptr;
};
