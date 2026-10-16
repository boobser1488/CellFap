#pragma once
// Minimal PNG reader (zlib for the pixels): mod previews. 8-bit greyscale, greyscale + alpha, RGB, RGBA and palette
// images (palette also at 1, 2 and 4 bits), not interlaced. Everything else is refused with a reason, never guessed.
#include <cstdint>
#include <string>
#include <vector>

// RGBA8 rows top to bottom. maxSide caps both dimensions (checked before anything is inflated).
bool decodePng(const std::string& file, std::vector<uint8_t>& rgba, int& w, int& h, std::string& error, int maxSide = 1024);
