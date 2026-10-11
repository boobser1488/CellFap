#pragma once
#include "world.hpp"

#include <vector>

struct GenomePreset {
    const char* name;
    const char* description;
    Genome genome;
};
const std::vector<GenomePreset>& genomePresets();

struct Experiment {
    const char* name;
    const char* description;
    void (*setup)(World& w, Rng& rng);
};
const std::vector<Experiment>& experiments();
