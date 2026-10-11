#pragma once
// Levels ("задания"): a prepared dish, a goal checked while it runs, and the original game's dry sense of humour.
#include "world.hpp"

#include <string>
#include <vector>

// Per-attempt state, filled by the level's setup and used by its check.
struct ChallengeState {
    Vec2 target{};          // finish zone (targetR > 0)
    float targetR = 0;
    Vec2 start{};           // the synthesiser only works inside the start zone (startR > 0)
    float startR = 0;
    double holdSince = -1;  // simulation time since which a "keep it up for N hours" condition holds
    int baseline = 0;       // level-specific number remembered at the start
    int insertsLeft = -1;   // synthesiser uses left (-1 = unlimited)
};

struct ChallengeProgress {
    float progress = 0;  // 0..1 for the progress bar
    bool won = false;
    bool lost = false;
    std::string status;  // e.g. "Ваших клеток: 57 из 100"
};

// Tool bits (same order as Tool in ui.hpp: Inspect, Synth, Tweezers, Boost, Remove, Shock, GeneNeedle, DrugNeedle,
// Map, Adhesin).
constexpr uint32_t kToolBit(int t) { return 1u << t; }

struct Challenge {
    const char* name;
    const char* story;  // the briefing
    const char* goal;   // one line
    const char* hint;
    const char* win;
    const char* lose;
    uint32_t tools;     // allowed tools (bits)
    int inserts;        // how many cells the synthesiser may insert (0 = none, -1 = unlimited)
    uint32_t forbidden; // cell types the inserted genome must not use (bits of CellType)
    void (*setup)(World& w, Rng& rng, ChallengeState& st);
    ChallengeProgress (*check)(const World& w, ChallengeState& st);
};

const std::vector<Challenge>& challenges();

// Helpers shared with the UI.
bool isUserCell(const Cell& c);
// Cell types the genome can actually grow (reachable modes), as bits of CellType.
uint32_t genomeTypes(const Genome& g);
int largestUserOrganism(const World& w);  // cells in the biggest adhesin-connected group of the player's cells
