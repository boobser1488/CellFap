#pragma once
#include "genome.hpp"

#include <string>
#include <vector>

// Organisms written in dypt (scripts/*.dpt), compiled to organisms.dll with `dypt --lib --native` and loaded
// next to the executable on first use. The library exports count(), name(i), description(i) and genome(i);
// genomes come back as "CellFapGenome 1" text.
struct ScriptedOrganism {
    std::string name;
    std::string description;
    Genome genome;
};

// Loaded once (call from the main thread: dypt libraries are single-threaded). Empty if the DLL is missing.
const std::vector<ScriptedOrganism>& scriptedOrganisms();
// Human-readable load result for the UI / logs, e.g. "organisms.dll: 2 организма".
const std::string& scriptedStatus();
// Organism by name, or nullptr.
const Genome* scriptedGenome(const char* name);
