#pragma once
#include "world.hpp"

#include <cstdio>
#include <string>
#include <vector>

// Genomes: human-readable text ("*.genome.txt"), easy to share.
bool saveGenome(const Genome& g, const std::string& path);
bool loadGenome(Genome& g, const std::string& path);
bool genomeFromText(Genome& g, const std::string& text);  // the same format, from memory

// Substrates: binary snapshot of settings, genomes, cells, links and food ("*.substrate").
bool saveWorld(const World& w, const char* path);
bool loadWorld(World& w, const char* path);

// Lists files with the given suffix in a directory (non-recursive), sorted by name.
std::vector<std::string> listFiles(const std::string& dir, const std::string& suffix);
std::string dataDir(const char* sub);  // <exe dir>/<sub>, created on demand
// A file name from free text: no path separators or characters Windows refuses, no trailing dots or spaces.
std::string safeFileName(const std::string& s, const char* fallback);
FILE* openFileUtf8(const std::string& path, const char* mode);  // fopen that accepts UTF-8 paths on Windows
