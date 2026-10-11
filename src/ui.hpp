#pragma once
#include "sim/challenges.hpp"
#include "sim/genome.hpp"

#include <cstdint>
#include <string>
#include <vector>

enum class Tool { Inspect, Synth, Tweezers, Boost, Remove, Shock, GeneNeedle, DrugNeedle, Map, Adhesin, Count };
enum class AdhesinMode { Link, Glue, Cut, Count };
// Walls (brush .. eraser), then water currents: a stroke the water follows, a whirl, an eraser of currents.
enum class MapMode { Brush, Line, Rect, Ring, Eraser, Current, Vortex, FlowEraser, Count };
enum class Speed { Freeze, Slow, Observe, Incubate };
// The three full-screen sections of the original game, plus the settings.
enum class Screen { Substrate, Microscope, Genome, Settings, Challenges };

struct UiState {
    Screen screen = Screen::Microscope;
    Tool tool = Tool::Inspect;
    Speed speed = Speed::Observe;
    Speed speedBeforeFreeze = Speed::Observe;

    // Genome editor -------------------------------------------------------
    Genome editGenome;
    int editMode = 0;
    int editCell = -1;          // selected cell in the preview organism (index), -1 = none
    uint32_t editCellId = 0;
    bool snap = true;
    bool symmetry = false;      // mirror split/angle edits onto the sibling mode
    float previewHours = 6.0f;  // developmental time shown in the editor
    bool previewDirty = true;
    bool previewVisible = false;
    bool showLinks = true;
    bool showSplitGuides = true;
    std::vector<Genome> undo, redo;
    Genome committed;           // last state pushed to the undo stack
    bool committedValid = false;

    // Microscope ---------------------------------------------------------
    int trackedIdx = -1;
    uint32_t trackedId = 0;
    bool follow = false;
    int hoveredIdx = -1;
    bool showHelp = false;
    bool showPerf = false;
    char modeFilter[64] = "";   // genome editor: mode list search

    // Tool options (real units) -------------------------------------------
    // One brush radius for every tool that works on an area (Ctrl + wheel or [ ] change it over the dish).
    float brushRadius = 40.0f;     // µm
    bool brushArea = false;        // remove / boost / drug needle: false = the cell under the cursor, true = the circle
    float shockVolts = 0.5f;       // electrode voltage, V
    float shockMs = 1.0f;          // pulse length, ms
    int geneModeChoice = 1;        // 0 = keep the cell's mode, 1 = mode selected in the editor, 2 = initial mode
    bool geneWholeOrganism = true;
    int drug = 0;                  // World::Substance
    // Dose per substance: glucose ng, nitrate %, signal units, cyanide ng, tetrodotoxin ng, adrenaline ng,
    // protease ng, regenerant ng, mutagen mutations.
    float drugDose[9] = {1.0f, 30.0f, 20.0f, 0.3f, 0.25f, 0.5f, 0.2f, 0.5f, 1.0f};
    int drugChannel = 0;           // signal channel S1..S8
    bool drugIntoWater = false;    // inject into the cell, or release into the water
    MapMode mapMode = MapMode::Brush;
    float mapThickness = 4.0f;     // barrier half-width, µm
    float flowSpeed = 0.6f;        // currents: water speed the brush sets, µm per tick
    bool vortexClockwise = false;
    AdhesinMode adhesinMode = AdhesinMode::Link;
    float adhesinStiffness = 0.5f; // 0..1
    float adhesinSlack = 0.0f;     // µm of extra length
    // Levels ("задания") -------------------------------------------------
    int challenge = -1;         // active level, -1 = free play
    ChallengeState chState;
    ChallengeProgress chProgress;
    bool chFinished = false;    // the win / lose window was shown for this attempt
    bool chShowHint = false;
    int chSelected = 0;         // level highlighted on the levels screen
    // Messages ------------------------------------------------------------
    std::string toast;
    double toastUntil = 0;

    // File name buffers
    char genomeFile[128] = "мой_геном";
    char substrateFile[128] = "моя_чашка";

    // Does the current tool act on the brush circle (so the circle and its radius are shown)?
    bool toolUsesRadius() const {
        switch (tool) {
            case Tool::Shock: return true;
            case Tool::Remove:
            case Tool::Boost: return brushArea;
            case Tool::DrugNeedle: return brushArea && !drugIntoWater;
            case Tool::Map: return mapMode == MapMode::Eraser || mapMode >= MapMode::Current;  // currents: the brush
            case Tool::Adhesin: return adhesinMode == AdhesinMode::Glue;
            default: return false;
        }
    }
};
