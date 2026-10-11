#pragma once
// Import of the original Cell Lab file formats (Java serialization) and of the 127-mode / 19-type mod.
//
//  .substrate : ObjectOutputStream header block (environment) + gzip'ed stream with cells and food.
//  .genome    : ObjectOutputStream with an int version followed by the modes.
//
// Supported: format 95 (Cell Lab v103: 40 modes x 315 bytes) and 3055 (mod: 127 modes x 331 bytes, 19 cell
// types). Field layout follows Gene.java (Cell Lab open utilities, P. Säterskog) and substrate.py.
#include "world.hpp"

#include <string>

struct CellLabImportInfo {
    int version = 0;
    int cells = 0;
    int skippedCells = 0;
    int food = 0;
    int genomes = 0;  // distinct genomes after de-duplication
    int unknownTypes = 0;  // modes whose cell type has no equivalent here (mapped to phagocyte)
    std::string note;
};

bool importCellLabSubstrate(World& w, const std::string& path, CellLabImportInfo& info, std::string& error);
bool importCellLabGenome(Genome& g, const std::string& path, std::string& error);
bool exportCellLabGenome(const Genome& g, const std::string& path, std::string& error);  // format 95, first 40 modes
