#pragma once
#include "cell_types.hpp"
#include "core/math.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>

constexpr int kMaxModes = 128;  // 40 in Cell Lab v103; the popular mod uses 127
constexpr int kSignalCount = 8;  // signal substances S1..S8 (S5..S8 appended in save format 6)
constexpr int kMaxLinks = 20;

// Split-mass sentinels (the original slider has these as its two end snaps).
constexpr float kSplitAtAge = 0.0f;   // split as soon as the cell is 0.5 h old
constexpr float kNeverSplit = 99.0f;  // never split
constexpr float kSplitMassMin = 0.774f;
constexpr float kSplitMassMax = 3.546f;
constexpr float kSplitMassStep = 0.126f;

// ---------------------------------------------------------------- programmable values
// Order is part of the genome format: S5..S8 were appended after Connections, then Light, Food (food within reach)
// and Injury.
enum class ProgInput : uint8_t { Fixed, S1, S2, S3, S4, Mass, Age, Nitrogen, Connections, S5, S6, S7, S8, Light, Food, Injury, Count };
// Program input that reads signal channel s (0..kSignalCount-1), and the reverse (-1 if not a signal).
inline ProgInput signalInput(int s) { return s < 4 ? (ProgInput)((int)ProgInput::S1 + s) : (ProgInput)((int)ProgInput::S5 + s - 4); }
inline int inputSignal(ProgInput p) {
    if (p >= ProgInput::S1 && p <= ProgInput::S4) return (int)p - (int)ProgInput::S1;
    if (p >= ProgInput::S5 && p <= ProgInput::S8) return 4 + (int)p - (int)ProgInput::S5;
    return -1;
}
enum class ProgKind : uint8_t { Linear, Step, Wave, Count };

// value = Linear: a*x + b    Step: x < c ? a : b    Wave: a*sin(2pi*x/c) + b    (Fixed input: always b)
// For Wave the age input is in hours, so c is the period of a neural rhythm (a pacemaker).
struct Prog {
    ProgInput input = ProgInput::Fixed;
    ProgKind kind = ProgKind::Linear;
    float a = 0, b = 0, c = 0;
};

// Range metadata for each programmable field (scale/offset follow the original editor).
struct ProgRange {
    const char* name;
    float lo, hi;
    float scale, offset;
};
enum ProgField : int {
    PF_Swim,
    PF_Density,
    PF_Contraction,
    PF_Bend,  // also ciliocyte left speed
    PF_Lift,  // also ciliocyte right speed
    PF_Stem1,
    PF_Stem2,
    PF_Neuro1,
    PF_Neuro2,
    PF_Neuro3,
    PF_Neuro4,
    PF_Count
};
const ProgRange& progRange(int field);

struct ProgInputs {
    float v[(int)ProgInput::Count];
    float ageHours = 0;
};
float evalProg(const Prog& p, const ProgInputs& in, int field);

// ---------------------------------------------------------------- senses & secretions
// Blood: dissolved cell sap — wounded or burst cells (sharks smell blood). Light also sees glowing luminocytes.
enum class SenseTarget : uint8_t { Cell, Food, CoatedFood, Light, Velocity, Wall, Blood, Count };
enum class Secretion : uint8_t {
    HCN,
    Lipase,
    Protease,
    FoodSmell,
    CoatedFoodSmell,
    LightActivator,
    WallActivator,
    MinusS1,
    MinusS2,
    MinusS3,
    MinusS4,
    PlusS1,
    PlusS2,
    PlusS3,
    PlusS4,
    MinusS5,
    MinusS6,
    MinusS7,
    MinusS8,
    PlusS5,
    PlusS6,
    PlusS7,
    PlusS8,
    Count
};
// Signal channel a secretion adds to (+1) or removes from (-1); returns -1 for non-signal secretions.
inline int secretionSignal(Secretion s, float* sign) {
    int v = (int)s;
    if (v >= (int)Secretion::MinusS1 && v <= (int)Secretion::MinusS4) { *sign = -1; return v - (int)Secretion::MinusS1; }
    if (v >= (int)Secretion::PlusS1 && v <= (int)Secretion::PlusS4) { *sign = 1; return v - (int)Secretion::PlusS1; }
    if (v >= (int)Secretion::MinusS5 && v <= (int)Secretion::MinusS8) { *sign = -1; return 4 + v - (int)Secretion::MinusS5; }
    if (v >= (int)Secretion::PlusS5 && v <= (int)Secretion::PlusS8) { *sign = 1; return 4 + v - (int)Secretion::PlusS5; }
    return -1;
}
inline const char* signalName(int s) {
    static const char* n[] = {"S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8"};
    return n[s < 0 ? 0 : (s > 7 ? 7 : s)];
}
const char* senseTargetName(SenseTarget s);
const char* secretionName(Secretion s);
const char* progInputName(ProgInput p);

// Gamete telomere choices: index into {inf, 5, 10, 20, 40, 80}
constexpr int kTelomereChoices[6] = {-1, 5, 10, 20, 40, 80};

// ---------------------------------------------------------------- mode
struct Mode {
    CellType type = CellType::Phagocyte;
    Color color{0.5f, 0.75f, 0.45f};
    bool makeAdhesin = false;
    bool prioritize = true;
    float splitMass = 2.538f;
    float splitRatio = 0.5f;
    float nutrientPriority = 1.0f;
    float splitAngle = 0;  // radians, from the cell's front
    float childAngle[2] = {0, 0};  // radians, relative to the split direction
    uint8_t childMode[2] = {0, 0};
    bool keepAdhesin[2] = {true, true};
    bool mirror[2] = {false, false};
    float adhesinStiffness = 0.0f;  // 0..1, angular stiffness
    float cytoskeleton = 1.0f;      // 0.5..2, cell hardness
    float adhesinLength = 0.0f;     // 0..1
    int maxConnections = 20;        // 1..20

    // Type-specific -------------------------------------------------
    Prog prog[PF_Count];
    uint8_t stemMode[2] = {0, 0};
    uint8_t neuroChannel[4] = {0, 1, 2, 3};  // output k -> signal channel 0..kSignalCount-1
    uint8_t virusCopyFrom = 0;
    SenseTarget senseTarget = SenseTarget::Cell;
    uint8_t senseChannel = 0;
    float senseOutput = 0.0f;  // -60..60
    Color senseColor{1, 1, 1};  // also the pigment colour of a chromatocyte and the glow colour of a luminocyte
    float senseThreshold = 0.0f;  // 0..sqrt(3)
    Secretion secretion = Secretion::FoodSmell;
    uint8_t gameteCompatibleMode = 0;
    uint8_t gameteModeAfter = 0;
    uint8_t gameteTelomeres = 0;  // index into kTelomereChoices
};

Mode defaultModeFor(int index);

// The 128 modes of a genome, copy-on-write per mode: copies of a genome (offspring, mutants, undo steps) share
// every mode they have not changed, so a mutated copy costs a few hundred bytes instead of 36 KB. Read with [],
// change with edit(i) (which un-shares that one mode first).
class ModeTable {
public:
    ModeTable();  // the default modes (shared by every genome)
    ModeTable(const ModeTable& o) { copyFrom(o); }
    ModeTable& operator=(const ModeTable& o) {
        if (this != &o) copyFrom(o);
        return *this;
    }
    const Mode& operator[](int i) const { return *p_[i]; }
    Mode& edit(int i);
    void set(int i, const Mode& m) { edit(i) = m; }
    // Take mode j of another table (shared, no copy).
    void share(int i, const ModeTable& o, int j) {
        p_[i] = o.p_[j];
        leaked_[i] = false;
    }
    bool sameAs(int i, const ModeTable& o, int j) const { return p_[i] == o.p_[j]; }
    bool isDefault(int i) const;

private:
    void copyFrom(const ModeTable& o);
    std::array<std::shared_ptr<Mode>, kMaxModes> p_;
    // Modes handed out for editing: a copy of the table must not share them (the editor may still hold the reference).
    std::array<bool, kMaxModes> leaked_{};
};

// Mode names, copy-on-write for the whole table (most genomes have none, and copies share them).
class LabelTable {
public:
    const std::string& operator[](int i) const;
    void set(int i, const std::string& s);
    bool any() const { return (bool)p_; }

private:
    std::shared_ptr<std::array<std::string, kMaxModes>> p_;
};

struct Genome {
    ModeTable modes;
    int initialMode = 0;
    std::string name = "Новый геном";
    // Optional human names of the modes (editor only; "Мышца левая", "Глаз"...). Not part of the cell program.
    LabelTable labels;

    Genome() = default;  // defaults: every mode a phagocyte splitting into itself, with distinct colours
    bool sameModes(const Genome& o) const;
};

struct MutationOptions {
    bool pointOnly = false;
    const bool* allowedTypes = nullptr;  // kCellTypeCount flags (gene pool), may be null
};
// Mutates one random setting near `focusMode`. Returns a new active mode if the mutation changed it, else -1.
int mutateGenome(Genome& g, int focusMode, Rng& rng, const MutationOptions& opt);
Genome randomGenome(Rng& rng, const bool* allowedTypes);
bool modeEquals(const Mode& a, const Mode& b);
