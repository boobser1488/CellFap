#pragma once
#include "core/jobs.hpp"
#include "core/math.hpp"
#include "genome.hpp"
#include "signal_cells.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

// Units: length µm, mass ng, simulation time in hours (one tick = kHoursPerTick).
constexpr float kHoursPerTick = 1.0f / 400.0f;
constexpr float kMaxCellMass = 3.6f;
constexpr float kLipocyteCapacity = 21.6f;  // 3.6 base + 18 fat
constexpr float kDeathMass = 0.72f;
constexpr float kMaxCellRadius = 15.0f;  // 30 µm diameter at 3.6 ng
constexpr float kMinSplitAge = 0.5f;     // h
constexpr float kMaxAge = 240.0f;        // h, with "cell aging"
constexpr float kFoodUnit = 10.0f;       // ng per food unit
constexpr float kPlaneShare = 0.26f;     // |cos| below which a link counts as lying on the split plane (~15°)

// Ёмкость по типу клетки: липоцит запасает много, эритроцит — по принципу адипоцита, но заметно меньше.
constexpr float kErythrocyteCapacity = 8.0f;  // 8 ng против 21.6 у липоцита
inline float cellCapacity(CellType t) {
    if (t == CellType::Lipocyte) return kLipocyteCapacity;
    if (t == CellType::Erythrocyte) return kErythrocyteCapacity;
    return kMaxCellMass;
}

inline float radiusForMass(float m, CellType t) {
    float base = kMaxCellRadius * std::sqrt(std::fmax(std::fmin(m, kMaxCellMass), 0.05f) / kMaxCellMass);
    if (t == CellType::Lipocyte && m > kMaxCellMass) base *= 1.0f + 0.6f * (m - kMaxCellMass) / (kLipocyteCapacity - kMaxCellMass);
    // Эритроцит: та же кривая, но до меньшего потолка массы, поэтому растёт только до 8 нг.
    else if (t == CellType::Erythrocyte && m > kMaxCellMass) base *= 1.0f + 0.6f * (m - kMaxCellMass) / (8.0f - kMaxCellMass);
    return base;
}
inline float foodRadius(float mass) { return 3.5f * std::sqrt(std::fmax(mass, 0.01f) / 11.52f); }

enum class CellTag : uint8_t { User, Challenge, Contamination, InfectedUser, InfectedChallenge, InfectedContamination, Hybrid };

inline bool isPlayerTag(CellTag t) { return t == CellTag::User || t == CellTag::InfectedUser || t == CellTag::Hybrid; }

constexpr int kGenePoolSlots = 40;
static_assert(kCellTypeCount <= kGenePoolSlots);

struct Settings {
    // Dish
    float dishRadius = 900.0f;  // µm
    int maxCells = 20000;
    int maxFood = 20000;
    bool lid = true;
    // Switches
    bool contaminate = false;
    bool killAtEdge = false;
    bool onlyPointMutations = false;
    bool cellAging = false;
    bool mobileFood = false;
    // Room for future cell types: the array size is part of the save layout, so it must not follow kCellTypeCount.
    std::array<bool, kGenePoolSlots> genePool;
    // Sliders
    float radiation = 0.0f;  // 0..0.09
    float viscosity = 30.0f;  // 0..150
    float dynFriction = 0.5f;  // 0..8
    float staticFriction = 0.2f;  // 0..8
    float nutrientRate = 3.0f;  // 0..15
    float chunkSize = 0.3f;  // 0.022..1.152 food units
    float lumpiness = 0.0f;  // 0..100
    float lumpSize = 0.05f;  // 0.01..0.2
    float coating = 0.0f;  // 0..1
    float salinity = 0.4f;  // 0..1
    float lightAmount = 1.0f;  // 0..2
    float lightRange = 0.6f;  // 0..1
    float lightDirChange = 0.0f;  // 0..1
    float gravity = 0.0f;  // 0..1
    float density = 0.0f;  // -3.3..3.3
    float densityGradient = 0.0f;  // 0..-2
    float nitrates = 1.0f;  // 0..1
    // Resistive-force swimming: a cell inside a body (with adhesins) is this many times harder to drag
    // sideways than along its axis. Undulating bodies then push off the water, like real worms. 1 = off.
    float swimAnisotropy = 3.0f;  // 1..6
    // State that is saved with the substrate
    float lightAngle = kPi * 0.5f;  // direction the light comes from (up)
    // Mechanical damage: crushing, hard impacts and torn adhesins injure cells. 0 = off, 1 = full.
    // (Appended last: older substrates simply keep the default.)
    float mechanicalDamage = 0.5f;
    // Wounded cells leak cytoplasm ("cell sap") into the water; lysed cells burst. 0 = off.
    float sapLeak = 1.0f;  // 0..2
    // Light is shared: photosynthesising cells may cover at most this fraction of the dish before each of them gets
    // less (algae stop at a carrying capacity instead of overgrowing everything).
    float lightCapacity = 0.25f;  // 0.02..1
    // Day and night: the light fades out and back in with this period (h). 0 = eternal day. (Format 7.)
    float dayLength = 0.0f;  // 0..48
    // Cell Lab's own physics and biology (soft contacts, anchor-to-anchor adhesins, coasting cells, its costs, food
    // and signals in ±1) for dishes from Cell Lab / MinLab: their organisms are built for it. (Format 8: appended.)
    bool cellLabPhysics = false;
    // CellFap: how hyaluronic acid behaves. Both variants existed in the original design notes, so the player picks.
    // 0 = strengthens adhesins and widens their reach, 1 = hinders transport of every substance through adhesins.
    // 2 = both at once (strengthen the bond, slow what travels through it).
    int hyaluronicMode = 0;

    Settings() { genePool.fill(true); }
};

struct Cell {
    Vec2 pos, vel;
    Vec2 prevPos;  // position at the start of the last tick (render interpolation)
    float angle = 0, angVel = 0;
    float prevAngle = 0;
    float mass = 1, radius = 1;
    float age = 0;
    float nitrogen = 1;
    float injury = 0;
    float boost = 0;  // h remaining of the magenta boost tint
    float signals[kSignalCount] = {};
    float actuator = 0;  // last programmable value used for visuals (swim force, contraction...)
    float lastSense = 0;
    float virusCooldown = 0;
    // Electrophysiology and drugs (h remaining, format 6).
    float charge = 0;     // electrocyte / cnidocyte: 0..1 stored charge or loaded thread
    float stun = 0;       // after an electric pulse: neurons refractory, muscles in tetanic cramp, flagella stalled
    float paralysis = 0;  // neurotoxin: nerves and senses silent, muscles slack
    float stimulant = 0;  // adrenaline-like: stronger flagella, cilia and muscles, higher metabolism
    float leak = 0;       // cell sap waiting to leave the cell (ng)
    float lift = 0;       // Cell Lab physics: height off the substrate (lifted segments slide with less friction)
    // CellFap extension: substance timers (h remaining)
    float amylase = 0;          // улучшает потребление пищи фагоцитами
    float hyaluronicAcid = 0;   // усиливает адгезины
    float epibrassinolide = 0;  // усиливает фотосинтез
    float benzyladin = 0;       // увеличивает продолжительность жизни
    float gibberellicAcid = 0;  // увеличивает размер клетки (до 40)
    float colchicine = 0;       // останавливает деление
    float penicillin = 0;       // разрушает оболочки кератиноцитов
    float interferon = 0;       // предотвращает вирусное заражение
    float resveratrol = 0;      // повышает устойчивость к радиации
    float ryanodine = 0;        // вызывает спазмы миоцитов
    float cycloheximide = 0;    // блокирует получение энергии
    float osteopontin = 0;      // делает остеоциты мягкими
    float chondroitinSulfate = 0; // затрудняет передачу через адгезины
    float edrophonium = 0;      // нейтрализует тетрадоксин
    float atractyloside = 0;    // нарушает передачу энергии из митохондрий
    // Signal cell state
    float chronocyteTimer = 0;  // таймер хроноцита
    float granulocyteToxicity = 0; // накопленная токсичность гранулоцита
    float griffocyteGrip = 0;   // сила сцепления гриффоцита
    float spirocyteAngle = 0;   // угол вращения спироцита
    float chromatocyteParam = 0; // параметр хромоцита (-1..1)
    uint8_t chromatocytePreset = 0; // пресет хромоцита (0=градиент, 1=радуга, 2=по сигналу)
    float progMem[4] = {0, 0, 0, 0};  // a program cell's own memory between calls (kSignalCellMemory)
    uint32_t id = 0;
    int32_t genome = -1;
    uint8_t mode = 0;
    CellType type = CellType::Phagocyte;
    CellTag tag = CellTag::User;
    bool alive = false;
    bool mirrored = false;
    bool isProtected = false;  // keratinocyte or linked to one (recomputed each tick)
    int8_t telomeres = -1;    // -1 = infinite
    uint16_t mutations = 0;
    uint8_t linkCount = 0;
    uint8_t glueWallLinks = 0;
    uint8_t editorFlags = 0;  // genome editor only: 1 = phantom (born next stage), 2 = selected mode
    int32_t links[kMaxLinks];
};

struct Link {
    int32_t a = -1, b = -1;  // b == -1: glued to the dish wall at wallPoint
    float angA = 0, angB = 0;  // anchor angles in each cell's local frame
    float extraLength = 0;    // µm added to ra + rb
    float stiffness = 0;      // 0..1
    Vec2 wallPoint;
    bool alive = false;
    bool fromDivision = false;
    // per-tick scratch (written in the link pass, gathered by the cells)
    Vec2 fA;  // force on a (b gets -fA)
    float tA = 0, tB = 0;
    float massFlow = 0;  // ng moving a -> b this tick
    float nitroFlow = 0;
    float maxLen = 0;    // longest the bond may get this tick (0 = no limit): the rope constraint after integration
    float fzA = 0;      // Cell Lab physics: vertical (lift) coupling on a (b gets -fzA)
    float sigFlow[kSignalCount] = {};
};

struct Food {
    Vec2 pos, vel;
    float mass;
    float coating;  // 0..1
};

// Substances dissolved in the water: clouds that spread (diffuse), fade and act on the cells they touch.
enum class FluidKind : uint8_t { Sap, Broth, Toxin, Neurotoxin, Stimulant, Enzyme, Signal, Count };
const char* fluidKindName(FluidKind k);

struct Fluid {
    Vec2 pos, vel;
    float amount;     // ng
    float age = 0;    // h
    float radius = 3; // µm, grows as it diffuses
    uint32_t color = 0;  // RGBA8 (cell sap keeps the colour of its cell)
    FluidKind kind = FluidKind::Sap;
    uint8_t channel = 0;  // Signal: channel
    uint16_t pad = 0;
};

struct Stats {
    float time;
    int cells;
    int food;
    float biomass;
    int perType[kCellTypeCount];
};

struct GenomePool {
    std::vector<Genome> genomes;
    std::vector<int> refs;
    std::vector<int> freeList;

    int add(const Genome& g);
    void retain(int id) { if (id >= 0) refs[id]++; }
    void release(int id);
    Genome& get(int id) { return genomes[id]; }
    const Genome& get(int id) const { return genomes[id]; }
    int liveCount() const { return (int)genomes.size() - (int)freeList.size(); }
    void clear() { genomes.clear(); refs.clear(); freeList.clear(); }
};

class World {
public:
    explicit World(JobPool* jobs);

    void reset(const Settings& s);
    void step();  // one tick

    // Tools ----------------------------------------------------------
    int addCell(const Genome& g, Vec2 pos, float angle, int mode = -1, CellTag tag = CellTag::User, float mass = 0);
    void removeCell(int idx, bool leaveFood);
    void boostCell(int idx);
    void addFood(Vec2 pos, float mass, float coating = 0);
    int pickCell(Vec2 p) const;  // -1 if none
    void dragCell(int idx, Vec2 target);  // optical tweezers (applied next tick)
    void dragFrozen(int idx, Vec2 target, float dt);  // tweezers while time is frozen: moves the cell directly
    // Electric pulse from a microelectrode at p (voltage in V, pulse length in ms; the field reaches ~radius µm).
    // Each cell feels the induced membrane voltage ΔΨ = 1.5·E·r (Schwan): > 15 mV excites nerves and muscles
    // (neurons fire then fall silent, muscles cramp, flagella stall), > 0.25 V opens pores (injury, the cell leaks
    // sap), > 1 V tears the membrane apart (lysis).
    struct ShockReport {
        int hit = 0, excited = 0, porated = 0, lysed = 0;
        float maxDeltaPsi = 0;  // V
    };
    ShockReport shock(Vec2 p, float radius, float volts, float pulseMs);
    // Field strength (V/µm) a pulse of `volts` with reach `radius` produces at distance d from the electrode.
    static float shockField(float volts, float radius, float d);
    // Adhesin tool: connect two cells / cut adhesins crossing a stroke / glue all touching cells in a circle.
    int linkCells(int a, int b, float extraLength, float stiffness);
    int cutLinks(Vec2 a, Vec2 b);
    int glueTouching(Vec2 p, float radius, float stiffness);
    // Dissolved substances (cell sap, drugs poured into the water...).
    void addFluid(Vec2 pos, FluidKind kind, float amount, uint32_t color = 0, int channel = 0, Vec2 vel = {});
    const std::vector<Fluid>& fluids() const { return fluids_; }
    // Recent electrocyte discharges (for the renderer's flashes).
    struct Spark {
        Vec2 pos;
        float radius;
        float t;  // simulation time, h
    };
    const std::vector<Spark>& sparks() const { return sparks_; }
    // Gene micro-needle: the cell (or its whole organism — every cell reachable through adhesins) gets a copy of
    // `g`. mode >= 0 also switches the injected cell to that mode; other cells keep their mode numbers.
    int injectGenome(int idx, const Genome& g, int mode, bool wholeOrganism);
    // Map editor: solid barriers (capsules = thick line segments) drawn into the dish. Cells collide with them,
    // food does not appear inside them.
    struct Barrier {
        Vec2 a, b;
        float r;
    };
    void addBarrier(Vec2 a, Vec2 b, float r);
    int eraseBarriers(Vec2 p, float radius);  // removes barrier pieces touching the circle
    void clearBarriers();
    const std::vector<Barrier>& barriers() const { return barriers_; }

    // Water currents: a coarse grid of water velocities (µm per tick) over the square around the dish, painted in the
    // map editor. Cells drift with the water (their drag pulls them toward its velocity), food and dissolved clouds are
    // carried along. Empty grid = still water (no cost).
    static constexpr int kFlowN = 96;
    bool hasFlow() const { return !flow_.empty(); }
    Vec2 flowAt(Vec2 p) const;
    void paintFlow(Vec2 from, Vec2 to, float radius, float speed);  // the water runs along the stroke
    void addVortex(Vec2 centre, float radius, float speed);         // a whirl (speed < 0: clockwise)
    void eraseFlow(Vec2 p, float radius);
    void clearFlow() { flow_.clear(); }
    const std::vector<Vec2>& flowGrid() const { return flow_; }
    Vec2 flowNode(int ix, int iy) const;  // position of a grid node
    // Barriers that may touch a cell at p (from the collision grid; nothing while the grid is being rebuilt).
    template <class F>
    void forBarriersNear(Vec2 p, F&& f) const {
        if (barriers_.empty() || barrierGridDirty_ || barrierGW_ == 0) return;
        int gx = (int)((p.x - barrierOrigin_.x) / barrierCell_), gy = (int)((p.y - barrierOrigin_.y) / barrierCell_);
        if (gx < 0 || gy < 0 || gx >= barrierGW_ || gy >= barrierGH_) return;
        for (int wi : barrierGrid_[(size_t)gy * barrierGW_ + gx]) f(barriers_[wi]);
    }
    bool insideBarrier(Vec2 p, float margin) const;
    // Substance micro-needle. Doses: ng (glucose, poisons, drugs, enzyme), % (nitrate), signal units, mutations.
    enum class Substance {
        Glucose, Nitrate, Signal, Cyanide, Tetrodotoxin, Adrenaline, Protease, Regenerant, Mutagen,
        // CellFap extension substances
        Amylase, HyaluronicAcid, Epibrassinolide, Benzyladin, GibberellicAcid, Colchicine, Penicillin,
        Interferon, Resveratrol, Ryanodine, Cycloheximide, Osteopontin, ChondroitinSulfate, Edrophonium, Atractyloside,
        Count
    };
    void injectSubstance(int idx, Substance s, float amount, int channel = 0);
    // The same substance poured into the water at p (only those that dissolve: returns false otherwise).
    bool releaseSubstance(Vec2 p, Substance s, float amount, int channel = 0);

    // Calls f(index) for live cells whose grid bucket lies within `range` of p (uses the grid of the last tick;
    // callers must test actual distances). Safe to call between ticks.
    template <class F>
    void forEachCellNear(Vec2 p, float range, F&& f) const {
        if (gridW_ <= 0 || cellGridStart_.empty()) return;
        int x0 = clampv((int)((p.x - range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
        int x1 = clampv((int)((p.x + range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
        int y0 = clampv((int)((p.y - range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
        int y1 = clampv((int)((p.y + range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
        const int n = (int)cells_.size();
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                int b = y * gridW_ + x;
                for (int k = cellGridStart_[b]; k < cellGridStart_[b + 1]; ++k) {
                    int j = cellGridItems_[k];
                    if (j < n && cells_[j].alive) f(j);
                }
            }
    }
    float maxCellRadius() const { return std::fmax((gridCell_ - 8.0f) * 0.5f, 4.0f); }
    void sterilize();

    // Genome-editor helpers: build a static snapshot of a laid-out organism.
    int addCellLike(int sourceCell, Vec2 pos, float angle, int mode, float mass, bool mirrored);  // shares the genome
    int connect(int a, int b, float extraLength, float stiffness) { return createLink(a, b, extraLength, stiffness, true); }
    void rebuildGrid() { buildGrids(); }
    // Time frozen: refresh the grid and push overlapping cells apart (tweezers, inserted cells).
    void relaxStatic(int iterations);

    // Accessors -----------------------------------------------------
    Settings settings;
    const std::vector<Cell>& cells() const { return cells_; }
    std::vector<Cell>& cellsMut() { return cells_; }
    const std::vector<Link>& links() const { return links_; }
    const std::vector<Food>& food() const { return food_; }
    const GenomePool& genomes() const { return genomes_; }
    GenomePool& genomesMut() { return genomes_; }
    int cellCount() const { return cellCount_; }
    double timeHours() const { return time_; }
    uint64_t ticks() const { return ticks_; }
    const std::vector<Stats>& history() const { return history_; }
    int countOfType(int t) const { return typeCounts_[t]; }
    float lightAt(Vec2 p) const;
    float dishLightAt(Vec2 p) const;  // the dish's own light field (without the flashlight)
    // A flashlight held over the dish (the tool): extra light around a point, by day and by night. Off when radius <= 0.
    void setSpotlight(Vec2 p, float radius, float power) {
        spotPos_ = p;
        spotRadius_ = radius;
        spotPower_ = power;
    }
    bool spotlightOn() const { return spotRadius_ > 0; }
    Vec2 spotlightPos() const { return spotPos_; }
    float spotlightRadius() const { return spotRadius_; }
    // Cell Lab physics: what a senseocyte (or, stereo, a stereocyte) of mode m at cell i perceives — see world.cpp.
    float clSense(const Cell& c, int i, const Mode& m, bool stereo) const;
    // 0 at night .. 1 at noon (always 1 without a day cycle); lightAt already includes it.
    float daylight() const { return daylight_; }
    static float daylightAt(const Settings& s, double hours);
    const Mode& modeOf(const Cell& c) const { return genomes_.get(c.genome).modes[c.mode]; }
    // The dypt program of a signal cell (empty when the mode is not a programmable one).
    const std::string& scriptOf(const Cell& c) const { return genomes_.get(c.genome).scripts[c.mode]; }
    // Colour as seen by eyes and the microscope (a chromatocyte shifts toward its pigment).
    Color cellColor(const Cell& c) const {
        const Mode& m = modeOf(c);
        if (c.type == CellType::Chromatocyte) {
            float k = clampv(c.actuator, 0.0f, 1.0f);
            return {m.color.r + (m.senseColor.r - m.color.r) * k, m.color.g + (m.senseColor.g - m.color.g) * k,
                    m.color.b + (m.senseColor.b - m.color.b) * k};
        }
        // Chromocyte: the whole cell takes the average of its gradient, so eyes and the microscope see one colour.
        // Preset 0 = black toward the chosen colour, 1 = rainbow, 2 = brightness by signal concentration.
        if (c.type == CellType::Chromocyte) return chromocyteColor(c, m);
        return m.color;
    }
    Color chromocyteColor(const Cell& c, const Mode& m) const {
        const float par = clampv(c.chromatocyteParam * 0.5f + 0.5f, 0.0f, 1.0f);  // -1..1 -> 0..1
        switch (c.chromatocytePreset) {
            case 1: {  // rainbow: the average of the hues around the circle is its base colour, brightened
                const float avg = par * 0.5f + 0.5f;
                const float h = avg * 0.33333f;
                auto hue = [&](float t) { return 0.5f + 0.5f * std::cos(kTau * (t + h)); };
                const float bright = 0.55f + 0.45f * par;
                return {hue(0.0f) * bright, hue(0.33333f) * bright, hue(0.66667f) * bright};
            }
            case 2: {  // by signal concentration: dim -> colour -> white as it saturates
                const float g = 0.15f + 0.85f * par;
                Color out{m.color.r * g, m.color.g * g, m.color.b * g};
                if (par > 0.85f) {
                    const float w = (par - 0.85f) / 0.15f * 0.6f;
                    out.r += (1.0f - out.r) * w;
                    out.g += (1.0f - out.g) * w;
                    out.b += (1.0f - out.b) * w;
                }
                return out;
            }
            default: {  // black toward the chosen colour, brightness rising with the parameter
                const float g = 0.1f + 0.9f * par;
                return {m.color.r * g, m.color.g * g, m.color.b * g};
            }
        }
    }
    bool previewMode = false;
    // Smoothed per-phase timings of step(): grid, prepass, contacts, feeding, links, integrate, serial.
    double phaseMs[7] = {};
    uint64_t tornLinks = 0;  // adhesins torn by stretching (statistics)
    uint64_t fusions = 0;    // gamete pairs fused (involving the player's cells)
    uint64_t lysedChallenge = 0;  // level ("challenge") cells that burst from injury

    // ---- programmable signal cells (dypt) -------------------------------------------
    // The manager owns the live dypt scripts. A dypt script belongs to the thread that opened it, and the
    // simulation runs on its own thread, so everything here must be called from that same thread (App does).
    SignalCellManager& signalCells() { return signalCells_; }
    // Whether step() may run player scripts at all (the editor turns this off while a dish is being rebuilt).
    bool signalCellsEnabled = true;
    // Diagnostics for the UI.
    int signalCellErrors = 0;
    uint64_t signalCellOps = 0;  // player-code instructions executed since the last reset (budget guard)

    // Serialisation hooks
    friend bool saveWorld(const World& w, const char* path);
    friend bool loadWorld(World& w, const char* path);
    friend bool importCellLabSubstrate(World& w, const std::string& path, struct CellLabImportInfo& info, std::string& error);
    friend struct CellLabAccess;  // cellab_import.cpp: building a world from a Cell Lab / MinLab dish

private:
    std::vector<Barrier> barriers_;
    std::vector<Vec2> flow_;  // kFlowN x kFlowN water velocities over [-R, R]^2, or empty
    std::vector<std::vector<int>> barrierGrid_;  // buckets of barrier indices (expanded by the largest cell)
    Vec2 barrierOrigin_;
    int barrierGW_ = 0, barrierGH_ = 0;
    float barrierCell_ = 64.0f;
    bool barrierGridDirty_ = true;
    void buildBarrierGrid();
    void collideBarriers();
    void clampIntoDish(Cell& c) const;  // closed dish: back inside the glass, no outward velocity
    int allocCell();
    int allocLink();
    bool hasLink(int a, int b) const;
    int createLink(int a, int b, float extraLen, float stiffness, bool fromDivision);
    int createWallLink(int a, Vec2 wallPoint);
    void removeLink(int li);
    void killCell(int idx, bool leaveFood, bool lysis = false);
    // Electrophysiology: apply an induced membrane voltage (V) for a pulse of pulseMs to cell i.
    // Returns 0 nothing, 1 excited, 2 porated, 3 lysed.
    int applyElectric(int i, float deltaPsi, float pulseMs);
    void spillSap(Cell& c, float amount, Vec2 where);
    void updateFluids();
    bool barrierAt(Vec2 p, float margin);  // fast (grid) test for points
    void updateFood();
    std::vector<Fluid> fluids_;
    std::vector<Spark> sparks_;
    std::vector<float> fieldAcc_;  // per cell: induced voltage from electrocyte discharges this tick
    void splitCell(int idx);
    // Cell Lab physics: how the parent's links are shared between the children (c1 = the parent's slot).
    void clInheritLinks(int c1, int c2, const Cell& parent, Vec2 dir, const Mode& m);
    void setMode(Cell& c, int mode);
    void spawnFood();
    void buildGrids();
    void computeProtection();
    void recordStats();

    template <class F>
    void forNeighbors(Vec2 p, float range, F&& f) const;
    template <class F>
    void forFoodNear(Vec2 p, float range, F&& f) const;

    JobPool* jobs_;
    SignalCellManager signalCells_;
    Rng rng_{0x1234567ull};
    std::vector<Cell> cells_;
    std::vector<int> freeCells_;
    std::vector<Link> links_;
    std::vector<int> freeLinks_;
    std::vector<Food> food_;
    GenomePool genomes_;
    int cellCount_ = 0;
    int linkCount_ = 0;
    uint32_t nextId_ = 1;
    double time_ = 0;
    uint64_t ticks_ = 0;
    double foodAccumulator_ = 0;
    float daylight_ = 1.0f;
    Vec2 spotPos_{};
    float spotRadius_ = 0, spotPower_ = 0;  // the flashlight (setSpotlight)
    std::vector<int> fluidGridStart_, fluidGridItems_;  // Sap clouds, for the "Blood" sense (built when needed)
    float fluidCell_ = 64.0f;
    int fluidGW_ = 0;
    void buildFluidGrid();
    std::vector<Vec2> lumps_;
    int typeCounts_[kCellTypeCount] = {};
    std::vector<Stats> history_;
    double nextStatTime_ = 0;

    // Tweezers
    int dragIdx_ = -1;
    Vec2 dragTarget_;

    // Uniform grids (counting-sort layout)
    float gridCell_ = 32.0f;
    int gridW_ = 0, gridH_ = 0;
    Vec2 gridOrigin_;
    std::vector<int> cellGridStart_, cellGridItems_;
    std::vector<int> foodGridStart_, foodGridItems_;

    // Per-tick scratch
    struct Scratch;
    std::unique_ptr<Scratch> scratch_;

public:
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;
};
