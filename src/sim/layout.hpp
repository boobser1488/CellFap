#pragma once
// Physics-free developmental layout of a genome for the editor (Genomeia-style).
//
// The organism is unfolded generation by generation ("stages"): at each stage every cell whose mode
// divides splits exactly like in the simulation (split plane, child angles, adhesin inheritance), then a
// short deterministic relaxation removes overlaps. Because nothing is integrated over time the editor view
// is perfectly stable — editing a value changes the picture only where the genome actually changed.
#include "genome.hpp"

#include <vector>

struct LayoutCell {
    Vec2 pos;
    float angle = 0;
    float mass = 1;       // mass shown (grown to the split mass if the cell divides next stage)
    float birthMass = 1;
    uint8_t mode = 0;
    bool mirrored = false;
    int parent = -1;      // index in the previous stage (-1 for the zygote)
    int childSlot = 0;    // 0 = child 1, 1 = child 2 (meaningless for the zygote / unchanged cells)
    bool born = false;    // created at this stage (false: carried over unchanged)
};

struct LayoutLink {
    int a, b;
    float extra;
    float stiffness;
    float angA = 0, angB = 0;  // anchor directions in each cell's frame (as in the simulation)
};

struct LayoutStage {
    std::vector<LayoutCell> cells;
    std::vector<LayoutLink> links;
};

struct GenomeLayout {
    std::vector<LayoutStage> stages;  // stages[0] = zygote
};

GenomeLayout buildGenomeLayout(const Genome& g, int maxStages = 72, int maxCells = 1200);

// Materialises the last stage of a layout in the world (cells, modes, adhesins) centred at `pos` and
// rotated so the organism's front points along `angle`. Returns the number of cells created.
class World;
int spawnGrownOrganism(World& w, const Genome& g, const GenomeLayout& L, Vec2 pos, float angle);
// Like spawnGrownOrganism, but searches a place (distance minR..maxR from the centre, any heading) where the whole
// grown body fits inside the dish without touching other cells or walls. Falls back to the least crowded try.
int spawnGrownOrganismFree(World& w, const Genome& g, const GenomeLayout& L, Rng& rng, float minR, float maxR);
