#pragma once
#include <cstdint>

// Order is part of the save format and the shader (cell.frag) — append only.
enum class CellType : uint8_t {
    Phagocyte = 0,
    Flagellocyte,
    Photocyte,
    Devorocyte,
    Lipocyte,
    Keratinocyte,
    Buoyocyte,
    Glueocyte,
    Virocyte,
    Nitrocyte,
    Stereocyte,
    Senseocyte,
    Myocyte,
    Neurocyte,
    Secrocyte,
    Stemocyte,
    Gamete,
    Ciliocyte,
    // CellFap additions (not in Cell Lab)
    Electrocyte,   // electric organ cell: stores charge, discharges a current into its surroundings
    Cnidocyte,     // stinging cell: fires a venomous thread into a foreign cell it touches
    Chromatocyte,  // pigment cell: its colour shifts toward a pigment colour under program control
    Luminocyte,    // light organ: glows under program control; light-seeking eyes of other organisms are drawn to it
    Cystocyte,     // dormant cyst: armoured, almost no metabolism, wakes into another mode when its program says so
    Grappler,      // grasping cell: grips foreign cells or the dish wall it touches while its program is "on", lets go after
    Count
};

constexpr int kCellTypeCount = (int)CellType::Count;

struct CellTypeInfo {
    const char* name;     // Russian UI name
    const char* latin;    // original English name
    const char* summary;  // one-line description for tooltips
    float costPerSec;     // extra metabolic cost (mass / s) on top of the base rate
    float r, g, b;        // suggested default colour
};

const CellTypeInfo& cellTypeInfo(CellType t);
const char* cellTypeQuip(CellType t);  // a joke line for the UI
inline const CellTypeInfo& cellTypeInfo(int t) { return cellTypeInfo((CellType)t); }
