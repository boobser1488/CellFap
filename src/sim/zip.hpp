#pragma once
// ZIP archives, just enough for mods (.cfmod): any archiver makes one and shows what is inside.
//
// Writing: stored or deflated entries (whichever is smaller), names flagged UTF-8, no ZIP64 (mods stay far below 4 GB).
// Reading: stored and deflated entries, CRC checked, sizes and counts capped before anything is inflated; encrypted,
// ZIP64 and multi-disk archives are refused. Names go through the caller's check (relative, no "..").
#include <cstddef>
#include <map>
#include <string>

namespace zip {

struct Limits {
    size_t maxFiles = 512;
    size_t maxTotal = 64u << 20;  // all entries together, unpacked
};

// path -> contents. Directories are implied by the paths.
bool write(const std::map<std::string, std::string>& files, std::string& out, std::string& error);

// Entries whose name the `clean` function maps to "" are refused (the whole archive is: a ".." in a mod is not an
// accident). Entries that are folders or system litter (__MACOSX/, .DS_Store) are skipped.
bool read(const std::string& bytes, std::map<std::string, std::string>& out, std::string& error, const Limits& lim,
          std::string (*clean)(const std::string&));

bool looksLikeZip(const std::string& bytes);

}  // namespace zip
