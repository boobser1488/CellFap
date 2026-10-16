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
    // CellFap extension: new cell types
    Fibroblast,    // Фибробласт: создаёт очень прочные и длинные адгезионные связи
    Baroreceptor,  // Барорецепторная клетка: обнаруживает контакт со стенками и выделяет сигнал
    Erythrocyte,   // Эритроцит: без ядра, запасает меньше энергии, низкий метаболизм, ослабленные связи
    Chronocyte,    // Хроноцит: выделяет сигнал каждые N секунд
    Cyclosporinocyte, // Циклоспориноцит: защищён от пенициллина
    Granulocyte,   // Гранулоцит: накапливает вредные вещества, при пороге погибает
    Griffocyte,    // Гриффоцит: цепляется за дно, сила зависит от сигнального вещества
    Spirocyte,     // Спироцит: вращается вокруг своей оси
    Chromocyte,    // Хромоцит: настраиваемый цветовой градиент (-1..1) с пресетами
    SignalCell,    // Программируемая сигнальная клетка (скрипт Dypt)
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
