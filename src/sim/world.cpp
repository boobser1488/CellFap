#include "world.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------- tuning constants
// Physics runs in µm / tick / ng. Biology rates are given per hour and scaled by kHoursPerTick.
namespace {
constexpr float kContactK = 0.22f;       // elastic repulsion per µm of overlap
constexpr float kMaxSquash = 0.07f;      // overlap the position solver tolerates (fraction of rA+rB) at cytoskeleton 1
constexpr int kContactIterations = 2;
constexpr int kMaxNbr = 16;
constexpr float kMaxSpeed = 25.0f;       // µm per tick
constexpr float kMaxPush = 6.0f;         // µm per solver iteration
constexpr float kContactDamp = 0.06f;    // normal relative-velocity damping
constexpr float kWallK = 0.6f;
constexpr float kLinkK = 0.09f;          // adhesin distance spring
constexpr float kLinkDamp = 0.06f;
constexpr float kBondRest = 0.93f;       // adhesin rest length as a fraction of the touching distance
constexpr float kRopeLen = 1.3f;         // a bond never opens a gap wider than 30 % of the touching distance
constexpr float kMuscleGain = 2.5f;       // extra spring stiffness of an active myocyte fibre (x kLinkK)
constexpr float kMuscleRate = 8.0f;       // 1/h: muscles reach ~63% of a new command in 0.125 h (50 ticks)
constexpr float kAngBase = 0.004f;       // angular spring at stiffness 0 (angular accel per rad)
constexpr float kAngStiff = 0.35f;       // extra at stiffness 1 (strong enough for tissues to hold shape and bend)
constexpr float kAngDamp = 0.08f;
constexpr float kSwimForce = 0.28f;      // at swim force 1
constexpr float kCiliaForce = 0.09f;
constexpr float kCiliaWater = 0.06f;    // thrust of the cilia on a free side, per unit of beat speed
constexpr float kGravity = 0.03f;
constexpr float kGameteAttract = 0.06f;
constexpr float kGameteSmell = 160.0f;   // µm: gametes find each other from this far
constexpr float kSenseRange = 300.0f;    // µm: eyes see food, prey and walls from afar
constexpr float kGlueMaxLinks = 5;

constexpr float kBaseCost = 0.26f;       // ng/h for coefficient 1 at salinity 0
constexpr float kPhotoRate = 2.0f;       // ng/h at full size and light 1
constexpr float kEatRate = 10.0f;        // ng/h per touching piece
constexpr float kDevourRate = 7.2f;      // ng/h per target
constexpr float kFlowRate = 20.0f;       // ng/h nutrient flow coefficient (6 left the tails of long worms starving)
constexpr float kNitroFlow = 2.0f;       // 1/h
constexpr float kNitroAbsorb = 1.5f;     // 1/h at nitrates 1
constexpr float kSignalDiffusion = 1.5f; // 1/h
constexpr float kSignalDecay = 1.0f;     // 1/h
// The nervous system runs this many times faster than metabolism (rates above and neurocyte / sensor outputs are
// per "signal hour"), so a nerve impulse crosses a 40-segment worm in ~0.5 h instead of half a day.
constexpr float kSignalSpeed = 10.0f;
constexpr float kInjuryRecovery = 0.05f; // fraction/h
constexpr float kPreviewGrowth = 1.1f;   // ng/h in the genome preview
constexpr float kStatInterval = 0.05f;   // h
// Electrophysiology (induced membrane voltage thresholds, V)
constexpr float kExciteV = 0.015f;       // depolarisation that fires nerves and muscles
constexpr float kPoreV = 0.25f;          // reversible electroporation
constexpr float kLysisV = 1.0f;          // irreversible breakdown
constexpr float kElectrocyteVolts = 0.5f;  // one electrocyte's discharge (the organ adds them up)
constexpr float kElectrocyteReach = 40.0f; // µm
constexpr float kElectrocyteCharge = 2.5f; // 1/h recharge
constexpr float kCnidoRecharge = 1.0f;   // 1/h: a new thread is coiled in an hour
constexpr int kGripMaxLinks = 3;         // a grappler holds at most this many foreign cells (or the wall)
constexpr float kFoodReach = 60.0f;      // µm: the "food nearby" program input
constexpr float kBloodGrid = 64.0f;      // µm: buckets of the sap-cloud grid
// Dissolved substances
constexpr float kFluidDiffusion = 2500.0f;  // µm²/h: a cloud widens to ~50 µm in an hour
constexpr int kMaxFluids = 3000;
constexpr float kSapLeakRate = 0.4f;     // fraction of the mass per hour at full injury

// ---- Cell Lab physics (Settings::cellLabPhysics): constants read from MinLab's native simulation, converted to our
// units (the game: length 500 µm, mass 10 ng, hours; its step is 0.02 h = 8 of our ticks).
constexpr float kClContact = 6.25e-5f;    // per µm² of the squared overlap chord, × cytoskeleton
constexpr float kClSwim = 0.03125f;       // flagellum thrust at swim force 1
constexpr float kClGravity = 0.003125f;
constexpr float kClDrag = 5e-5f;          // × viscosity × radius
constexpr float kClRotDrag = 2.5e-5f;     // × viscosity × radius³
constexpr float kClFriction = 0.003125f;  // µm/tick² per unit of the friction sliders
constexpr float kClLinkK = 1.875e-3f;     // anchor-to-anchor spring, softened by the adhesin length
constexpr float kClLinkG = 6.708f;        // µm: the game's smallest cell radius, in that softening
constexpr float kClLinkDamp = 0.0625f;
constexpr float kClAngular = 6.25e-5f;    // × stiffness 0..35 × smaller radius × sqrt|anchor cross product|
constexpr float kClWallLinkK = 5.625e-3f;
constexpr float kClCiliaSlip = 0.09375f;  // µm/tick of sliding per unit of cilia speed (37.5 µm/h)

// A random number in [0, 1) from three integers (the parallel passes cannot share the world's generator).
inline float hash01(uint32_t a, uint32_t b, uint64_t c) {
    uint64_t x = a * 0x9E3779B97F4A7C15ull ^ (uint64_t)b * 0xC2B2AE3D27D4EB4Full ^ c * 0x165667B19E3779F9ull;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return (float)(x >> 40) / 16777216.0f;
}

// Squared chord of the lens where two circles (radii ri, rj, centres d apart) overlap; 0 when they do not touch.
inline float overlapChord2(float ri, float rj, float d) {
    if (d >= ri + rj) return 0;
    const float rmin = std::min(ri, rj);
    if (d <= std::fabs(ri - rj) + 1e-4f) return 4 * rmin * rmin;  // one inside the other
    float a2 = (ri - d - rj) * (-d - ri + rj) * (ri - d + rj) * (d + ri + rj) / (d * d);
    return clampv(a2, 0.0f, 4 * rmin * rmin);
}

// Modified Bessel functions K0, K1 (Abramowitz & Stegun 9.8; the Android C++ library has no std::cyl_bessel_k), as
// Cell Lab tabulates them: 500 steps of 0.01, so they stop falling at x = 5.
struct BesselTables {
    float k0[500], k1[500];
    BesselTables() {
        for (int i = 0; i < 500; ++i) {
            const double x = 0.01 * (i + 1);
            if (x <= 2.0) {
                const double y = (x / 3.75) * (x / 3.75), t = (x / 2) * (x / 2);
                const double i0 = 1 + y * (3.5156229 + y * (3.0899424 + y * (1.2067492 + y * (0.2659732 + y * (0.0360768 + y * 0.0045813)))));
                const double i1 = x * (0.5 + y * (0.87890594 + y * (0.51498869 + y * (0.15084934 + y * (0.02658733 + y * (0.00301532 + y * 0.00032411))))));
                k0[i] = (float)(-std::log(x / 2) * i0 +
                                (-0.57721566 + t * (0.42278420 + t * (0.23069756 + t * (0.03488590 + t * (0.00262698 + t * (0.00010750 + t * 0.0000074)))))));
                k1[i] = (float)((x * std::log(x / 2) * i1 +
                                 (1 + t * (0.15443144 + t * (-0.67278579 + t * (-0.18156897 + t * (-0.01919402 + t * (-0.00110404 + t * -0.00004686))))))) /
                                x);
            } else {
                const double z = 2 / x, e = std::exp(-x) / std::sqrt(x);
                k0[i] = (float)(e * (1.25331414 + z * (-0.07832358 + z * (0.02189568 + z * (-0.01062446 + z * (0.00587872 + z * (-0.00251540 + z * 0.00053208)))))));
                k1[i] = (float)(e * (1.25331414 + z * (0.23498619 + z * (-0.03655620 + z * (0.01504268 + z * (-0.00780353 + z * (0.00325614 + z * -0.00068245)))))));
            }
        }
    }
};
const BesselTables& bessel() {
    static const BesselTables t;
    return t;
}
inline int besselIndex(float x) { return clampv((int)std::lround(x / 0.01f), 0, 499); }

// Cell Lab's basal cost per type (the metabolism scales it by 10 ng/h and the cell's size); CellFap's own types as
// their closest kin.
inline float clTypeCost(CellType t) {
    switch (t) {
        case CellType::Phagocyte:
        case CellType::Flagellocyte:
        case CellType::Devorocyte:
        case CellType::Cnidocyte: return 0.1f;
        case CellType::Photocyte: return 0.25f;
        case CellType::Virocyte: return 0.2f;
        case CellType::Secrocyte: return 0.03f;
        case CellType::Lipocyte:
        case CellType::Gamete:
        case CellType::Ciliocyte:
        case CellType::Cystocyte: return 0.005f;
        case CellType::Keratinocyte:
        case CellType::Glueocyte:
        case CellType::Stereocyte:
        case CellType::Senseocyte:
        case CellType::Chromatocyte:
        case CellType::Luminocyte:
        case CellType::Grappler: return 0.01f;
        default: return 0.02f;  // buoyocyte, nitrocyte, myocyte, neurocyte, stemocyte, electrocyte
    }
}

inline float fluidDecay(FluidKind k) {  // 1/h
    switch (k) {
        case FluidKind::Sap: return 0.6f;     // bacteria and the water dilute it
        case FluidKind::Broth: return 0.35f;
        case FluidKind::Signal: return 1.5f;
        default: return 0.4f;
    }
}

// Эритроцит работает по принципу адипоцита, но запасает заметно меньше энергии (kErythrocyteCapacity в world.hpp).
// Гибберелловая кислота: потолок массы (ng), выше 40 игровых единиц клетка не растёт.
constexpr float kGibberellicMassCap = 40.0f;

// Ёмкость с учётом вещества: гибберелловая кислота растит клетку, но не выше 40 игровых единиц.
inline float capacityFor(const Cell& c) {
    if (c.gibberellicAcid <= 0.0f) return cellCapacity(c.type);
    return std::max(cellCapacity(c.type), std::min(kGibberellicMassCap, cellCapacity(c.type) * (1.0f + c.gibberellicAcid)));
}
inline float typeCost(CellType t, float swim) {
    if (t == CellType::Flagellocyte) return 0.1f + 0.15f * swim;
    if (t == CellType::Neurocyte) return 0.1f;
    if (t == CellType::Luminocyte) return 0.02f + 0.3f * std::max(swim, 0.0f);  // light costs energy (luciferin)
    if (t == CellType::Myocyte) return 0.2f + 0.8f * std::fabs(swim);  // swim = contraction: working muscle burns more
    return cellTypeInfo(t).costPerSec;
}
inline float smoothstep(float a, float b, float x) {
    float t = clampv((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
inline Vec2 rotate(Vec2 v, float a) {
    float c = std::cos(a), s = std::sin(a);
    return {v.x * c - v.y * s, v.x * s + v.y * c};
}

// ---- CellFap: substances added on top of the original set ----
// Timers on a cell are "hours remaining"; injectSubstance adds them, and every tick they run down.
constexpr float kGranulocyteLethal = 1.0f;     // гранулоцит: порог токсичности, после которого он рвётся и гибнет
constexpr float kBenzyladinLife = 1.5f;        // 6-БА: во сколько раз дольше живёт клетка
constexpr float kHyaluronicRange = 1.5f;       // во сколько раз больше радиус действия адгезинов
constexpr float kChondroitinBlock = 0.75f;     // хондроитинсульфат: насколько сильно режет передачу по адгезинам
constexpr float kEdrophoniumDecay = 4.0f;      // 1/h: взаимное разрушение эдрофония и тетрадоксина

// All CellFap substance timers run down by one tick. The effects themselves live in the phases that own the
// mechanic they touch (eating, links, splitting, photosynthesis, metabolism), so that a dose has exactly one
// meaning and there is no second, parallel implementation of it.
inline void decaySubstance(Cell& c, float h) {
    // Эдрофоний нейтрализует тетрадоксин прямо в клетке: находясь рядом, оба вещества быстро разрушают друг друга.
    // Без этого тетрадоксин, введённый иглой, держался бы весь срок действия.
    if (c.edrophonium > 0.0f && c.paralysis > 0.0f) {
        const float burn = std::min(c.edrophonium, kEdrophoniumDecay * h);
        c.paralysis = std::max(0.0f, c.paralysis - burn * 2.0f);
        c.edrophonium = std::max(0.0f, c.edrophonium - burn * 0.5f);
    }
    c.amylase = std::max(0.0f, c.amylase - h);
    c.hyaluronicAcid = std::max(0.0f, c.hyaluronicAcid - h);
    c.epibrassinolide = std::max(0.0f, c.epibrassinolide - h);
    c.benzyladin = std::max(0.0f, c.benzyladin - h);
    c.gibberellicAcid = std::max(0.0f, c.gibberellicAcid - h);
    c.colchicine = std::max(0.0f, c.colchicine - h);
    c.penicillin = std::max(0.0f, c.penicillin - h);
    c.interferon = std::max(0.0f, c.interferon - h);
    c.resveratrol = std::max(0.0f, c.resveratrol - h);
    c.ryanodine = std::max(0.0f, c.ryanodine - h);
    c.cycloheximide = std::max(0.0f, c.cycloheximide - h);
    c.osteopontin = std::max(0.0f, c.osteopontin - h);
    c.chondroitinSulfate = std::max(0.0f, c.chondroitinSulfate - h);
    c.edrophonium = std::max(0.0f, c.edrophonium - h);
    c.atractyloside = std::max(0.0f, c.atractyloside - h);
}
}  // namespace

// ---------------------------------------------------------------- genome pool
int GenomePool::add(const Genome& g) {
    int id;
    if (!freeList.empty()) {
        id = freeList.back();
        freeList.pop_back();
        genomes[id] = g;
        refs[id] = 1;
    } else {
        id = (int)genomes.size();
        genomes.push_back(g);
        refs.push_back(1);
    }
    return id;
}

void GenomePool::release(int id) {
    if (id < 0) return;
    if (--refs[id] == 0) freeList.push_back(id);
}

// ---------------------------------------------------------------- scratch
enum class EventKind : uint8_t { Infect, Glue, GlueWall, Fuse, Kill, Remove, Sting, Discharge, Grip, GripWall };
struct Event {
    EventKind kind;
    int a, b;
    Vec2 p;
};
struct EatRequest {
    int cell, food;
};

struct World::Scratch {
    std::vector<Vec2> force;
    std::vector<Vec2> push;  // position-solver corrections
    std::vector<float> hard;  // cytoskeleton per cell (cached: avoids genome lookups in hot loops)
    std::vector<float> squeeze;  // contact-radius factor (< 1 for a contracting myocyte)
    std::vector<int> nbr;     // contact candidates per cell, kMaxNbr slots each
    std::vector<uint8_t> nbrCount;
    std::vector<float> torque;
    std::vector<float> dMass;     // ng/h from contacts (devouring)
    std::vector<float> dNitro;    // 1/h
    std::vector<float> dInjury;   // 1/h
    std::vector<std::array<float, kSignalCount>> dSignal;  // per hour
    std::vector<float> eaten;     // ng this tick
    std::vector<std::array<float, 8>> act;  // swim, density, contraction, bend, lift, stem1, stem2, sense
    std::vector<float> dToxic;             // granulocyte: toxin drawn out of linked cells this tick
    // Programmable signal cells: the run list for this tick (cell index, program key, its inputs) plus the flat
    // input storage the entries point into. Filled in the prepass, consumed by runTick before integration.
    std::vector<SignalCellManager::Run> signalRuns;
    std::vector<float> signalInputs;
    std::vector<std::pair<int, SignalCellResult>> signalResults;  // applied at the end of the tick
    std::vector<uint8_t> dead, wantsSplit, discharge;
    std::vector<int16_t> newMode;
    std::vector<uint8_t> brokenLink;
    std::vector<Event> events;
    std::vector<EatRequest> eats;
    std::vector<int> secrocytes;
    std::mutex m;
};

enum ActIdx { A_Swim, A_Density, A_Contract, A_Bend, A_Lift, A_Stem1, A_Stem2, A_Sense };

// ---------------------------------------------------------------- world
World::World(JobPool* jobs) : jobs_(jobs), scratch_(std::make_unique<Scratch>()) {}
World::~World() = default;

void World::reset(const Settings& s) {
    settings = s;
    // Every new dish gets its own randomness (food rain, mutations...): the generator used to start from the same
    // constant, so all dishes replayed one sequence.
    rng_ = Rng((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ (rng_.next() << 1) ^ 0x9E3779B97F4A7C15ull);
    cells_.clear();
    freeCells_.clear();
    links_.clear();
    freeLinks_.clear();
    food_.clear();
    fluids_.clear();
    barriers_.clear();
    flow_.clear();
    barrierGridDirty_ = true;
    genomes_.clear();
    lumps_.clear();
    history_.clear();
    cellCount_ = linkCount_ = 0;
    nextId_ = 1;
    time_ = 0;
    ticks_ = 0;
    foodAccumulator_ = 0;
    nextStatTime_ = 0;
    tornLinks = fusions = lysedChallenge = 0;
    daylight_ = daylightAt(settings, 0.0);
    fluidGW_ = 0;
    dragIdx_ = -1;
    std::fill(std::begin(typeCounts_), std::end(typeCounts_), 0);
    cells_.reserve(1024);
    gridW_ = gridH_ = 0;
    cellGridStart_.clear();
    cellGridItems_.clear();
    foodGridStart_.clear();
    foodGridItems_.clear();
    cellEvents.clear();  // (indices of the old dish)
    ++resets_;
}

void World::noteEvent(CellEvent::Kind k, int cell, int other) {
    // A runaway consumer (nobody takes them) must not eat the memory.
    if (cellEvents.size() >= (1u << 18)) return;
    const Cell& c = cells_[cell];
    cellEvents.push_back({k, c.type, cell, other, c.id, other >= 0 ? cells_[other].id : 0u, c.pos});
}

void World::relaxStatic(int iterations) {
    buildGrids();
    const int n = (int)cells_.size();
    if (!scratch_) return;
    std::vector<Vec2>& push = scratch_->push;
    push.assign(n, Vec2{});
    const float pad = gridCell_ * 0.5f;
    for (int it = 0; it < iterations; ++it) {
        jobs_->parallelFor(n, 256, [&](int b, int e) {
            for (int i = b; i < e; ++i) {
                const Cell& c = cells_[i];
                Vec2 corr{};
                if (c.alive) {
                    const float hardC = modeOf(c).cytoskeleton;
                    forNeighbors(c.pos, c.radius + pad, [&](int j) {
                        if (j == i || !cells_[j].alive) return;
                        const Cell& o = cells_[j];
                        Vec2 d = c.pos - o.pos;
                        float hard = 0.5f * (hardC + modeOf(o).cytoskeleton);
                        float minDist = (c.radius + o.radius) * (1.0f - kMaxSquash / hard);
                        float d2 = length2(d);
                        if (d2 >= minDist * minDist) return;
                        float dist = std::sqrt(d2);
                        Vec2 nrm = dist > 1e-4f ? d / dist : fromAngle((float)(i * 7 + j));
                        corr += nrm * ((minDist - dist) * (o.mass / (c.mass + o.mass)) * 0.5f);
                    });
                }
                push[i] = corr;
            }
        });
        for (int i = 0; i < n; ++i)
            if (cells_[i].alive) {
                cells_[i].pos += push[i];
                clampIntoDish(cells_[i]);  // a packed colony must not spread out through the glass while frozen
                cells_[i].prevPos = cells_[i].pos;
            }
    }
    collideBarriers();
    for (Cell& c : cells_)
        if (c.alive) c.prevPos = c.pos;
    buildGrids();
}

// Closed dish: keep the cell inside the glass (it may squash against it as against a neighbour) and stop it moving out.
void World::clampIntoDish(Cell& c) const {
    if (!settings.lid && !previewMode) return;
    float maxR = std::max(settings.dishRadius - c.radius * (1.0f - kMaxSquash), 0.0f);
    float pd = length(c.pos);
    if (pd <= maxR || pd < 1e-4f) return;
    Vec2 u = c.pos / pd;
    c.pos = u * maxR;
    float vn = dot(c.vel, u);
    if (vn > 0) c.vel -= u * vn;
}

void World::dragFrozen(int idx, Vec2 target, float dt) {
    if (idx < 0 || idx >= (int)cells_.size() || !cells_[idx].alive) return;
    Cell& c = cells_[idx];
    float k = 1.0f - std::exp(-dt * 18.0f);
    c.pos = c.pos + (target - c.pos) * k;
    float R = settings.dishRadius - c.radius;
    float l = length(c.pos);
    if (l > R) c.pos = c.pos * (R / l);
    c.prevPos = c.pos;
    c.vel = {};
}

void World::sterilize() {
    // Cells and food go, the drawn map stays.
    Settings s = settings;
    std::vector<Barrier> keep = barriers_;
    reset(s);
    barriers_ = std::move(keep);
    barrierGridDirty_ = true;
}

// ---------------------------------------------------------------- barriers (map editor)
namespace {
Vec2 closestOnSegment(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    float l2 = length2(ab);
    float t = l2 > 1e-8f ? clampv(dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return a + ab * t;
}
}  // namespace

void World::addBarrier(Vec2 a, Vec2 b, float r) {
    // Walls exist only inside the dish: clip the segment to the circle (pieces entirely outside are dropped).
    {
        const float R = settings.dishRadius;
        Vec2 d = b - a;
        float A = length2(d), B = 2.0f * dot(a, d), C = length2(a) - R * R;
        float t0 = 0, t1 = 1;
        if (A < 1e-8f) {
            if (C > 0) return;
        } else {
            float disc = B * B - 4 * A * C;
            if (disc <= 0) return;
            float sq = std::sqrt(disc);
            t0 = std::max(0.0f, (-B - sq) / (2 * A));
            t1 = std::min(1.0f, (-B + sq) / (2 * A));
            if (t0 > t1) return;
        }
        Vec2 a2 = a + d * t0, b2 = a + d * t1;
        a = a2;
        b = b2;
    }
    barriers_.push_back({a, b, std::max(r, 1.0f)});
    barrierGridDirty_ = true;
    // Food buried by the new barrier disappears.
    for (size_t i = 0; i < food_.size();) {
        if (length(food_[i].pos - closestOnSegment(food_[i].pos, a, b)) < r) {
            food_[i] = food_.back();
            food_.pop_back();
        } else {
            ++i;
        }
    }
}

int World::eraseBarriers(Vec2 p, float radius) {
    int removed = 0;
    for (size_t i = 0; i < barriers_.size();) {
        const Barrier& w = barriers_[i];
        if (length(p - closestOnSegment(p, w.a, w.b)) < radius + w.r) {
            barriers_[i] = barriers_.back();
            barriers_.pop_back();
            ++removed;
        } else {
            ++i;
        }
    }
    if (removed) barrierGridDirty_ = true;
    return removed;
}

void World::clearBarriers() {
    barriers_.clear();
    barrierGridDirty_ = true;
}

// ---- water currents
Vec2 World::flowNode(int ix, int iy) const {
    const float R = settings.dishRadius, step = 2.0f * R / (kFlowN - 1);
    return {-R + ix * step, -R + iy * step};
}

Vec2 World::flowAt(Vec2 p) const {
    if (flow_.empty()) return {};
    const float R = settings.dishRadius, step = 2.0f * R / (kFlowN - 1);
    const float fx = (p.x + R) / step, fy = (p.y + R) / step;
    if (fx < 0 || fy < 0 || fx >= kFlowN - 1 || fy >= kFlowN - 1) return {};
    const int x = (int)fx, y = (int)fy;
    const float tx = fx - x, ty = fy - y;
    auto at = [&](int i, int j) { return flow_[(size_t)j * kFlowN + i]; };
    Vec2 a = at(x, y) * (1 - tx) + at(x + 1, y) * tx, b = at(x, y + 1) * (1 - tx) + at(x + 1, y + 1) * tx;
    return a * (1 - ty) + b * ty;
}

// Brush weight of grid nodes within `radius` of the segment a-b (1 in the middle, smoothly 0 at the edge).
template <class F>
static void forFlowNodes(const World& w, Vec2 a, Vec2 b, float radius, F&& f) {
    const float R = w.settings.dishRadius, step = 2.0f * R / (World::kFlowN - 1);
    const int x0 = std::max(0, (int)std::floor((std::min(a.x, b.x) - radius + R) / step));
    const int x1 = std::min(World::kFlowN - 1, (int)std::ceil((std::max(a.x, b.x) + radius + R) / step));
    const int y0 = std::max(0, (int)std::floor((std::min(a.y, b.y) - radius + R) / step));
    const int y1 = std::min(World::kFlowN - 1, (int)std::ceil((std::max(a.y, b.y) + radius + R) / step));
    for (int j = y0; j <= y1; ++j)
        for (int i = x0; i <= x1; ++i) {
            const Vec2 p = w.flowNode(i, j);
            const float d = length(p - closestOnSegment(p, a, b));
            if (d >= radius) continue;
            const float k = 1.0f - d / radius;
            f((size_t)j * World::kFlowN + i, p, k * k * (3.0f - 2.0f * k));
        }
}

void World::paintFlow(Vec2 from, Vec2 to, float radius, float speed) {
    const Vec2 d = to - from;
    if (length2(d) < 1e-6f) return;
    if (flow_.empty()) flow_.assign((size_t)kFlowN * kFlowN, Vec2{});
    const Vec2 target = normalize(d) * speed;
    // Each pass turns the water under the brush toward the stroke: a few strokes make a steady current.
    forFlowNodes(*this, from, to, radius, [&](size_t k, Vec2, float wgt) { flow_[k] += (target - flow_[k]) * (0.6f * wgt); });
}

void World::addVortex(Vec2 centre, float radius, float speed) {
    if (flow_.empty()) flow_.assign((size_t)kFlowN * kFlowN, Vec2{});
    forFlowNodes(*this, centre, centre, radius, [&](size_t k, Vec2 p, float wgt) {
        const Vec2 r = p - centre;
        const float dist = length(r);
        if (dist < 1e-3f) return;
        // Fastest a third of the way out, still at the centre and at the rim.
        const float prof = std::min(1.0f, dist / (radius * 0.33f));
        flow_[k] += (perp(r / dist) * (speed * prof) - flow_[k]) * (0.8f * wgt);
    });
}

void World::eraseFlow(Vec2 p, float radius) {
    if (flow_.empty()) return;
    forFlowNodes(*this, p, p, radius, [&](size_t k, Vec2, float wgt) { flow_[k] *= 1.0f - wgt; });
    bool any = false;
    for (const Vec2& v : flow_) any = any || length2(v) > 1e-6f;
    if (!any) flow_.clear();  // all still again: no cost
}

bool World::insideBarrier(Vec2 p, float margin) const {
    for (const Barrier& w : barriers_)
        if (length(p - closestOnSegment(p, w.a, w.b)) < w.r + margin) return true;
    return false;
}

void World::buildBarrierGrid() {
    barrierGridDirty_ = false;
    float R = settings.dishRadius + 200.0f;
    barrierOrigin_ = {-R, -R};
    barrierGW_ = barrierGH_ = std::max(1, (int)std::ceil(2.0f * R / barrierCell_));
    barrierGrid_.assign((size_t)barrierGW_ * barrierGH_, {});
    const float reach = 24.0f;  // largest cell radius
    for (int i = 0; i < (int)barriers_.size(); ++i) {
        const Barrier& w = barriers_[i];
        float e = w.r + reach;
        int x0 = clampv((int)((std::min(w.a.x, w.b.x) - e - barrierOrigin_.x) / barrierCell_), 0, barrierGW_ - 1);
        int x1 = clampv((int)((std::max(w.a.x, w.b.x) + e - barrierOrigin_.x) / barrierCell_), 0, barrierGW_ - 1);
        int y0 = clampv((int)((std::min(w.a.y, w.b.y) - e - barrierOrigin_.y) / barrierCell_), 0, barrierGH_ - 1);
        int y1 = clampv((int)((std::max(w.a.y, w.b.y) + e - barrierOrigin_.y) / barrierCell_), 0, barrierGH_ - 1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) barrierGrid_[(size_t)y * barrierGW_ + x].push_back(i);
    }
}

// Cells are projected out of barriers after the contact solver; the inward velocity is removed with a little
// bounce, and a hard hit hurts (mechanical damage).
void World::collideBarriers() {
    if (barriers_.empty() || previewMode) return;
    if (barrierGridDirty_) buildBarrierGrid();
    const int n = (int)cells_.size();
    jobs_->parallelFor(n, 256, [&](int b, int e) {
        for (int i = b; i < e; ++i) {
            Cell& c = cells_[i];
            if (!c.alive) continue;
            int gx = (int)((c.pos.x - barrierOrigin_.x) / barrierCell_), gy = (int)((c.pos.y - barrierOrigin_.y) / barrierCell_);
            if (gx < 0 || gy < 0 || gx >= barrierGW_ || gy >= barrierGH_) continue;
            for (int wi : barrierGrid_[(size_t)gy * barrierGW_ + gx]) {
                const Barrier& w = barriers_[wi];
                Vec2 q = closestOnSegment(c.pos, w.a, w.b);
                Vec2 d = c.pos - q;
                float dist = length(d);
                float minD = w.r + c.radius * (1.0f - kMaxSquash);
                if (dist >= minD) continue;
                Vec2 nrm = dist > 1e-4f ? d / dist : perp(normalize(w.b - w.a + Vec2{1e-4f, 0}));
                c.pos = q + nrm * minD;
                float vn = dot(c.vel, nrm);
                if (vn < 0) {
                    if (settings.mechanicalDamage > 0 && -vn > 3.0f)
                        c.injury += (-vn - 3.0f) * 0.04f * settings.mechanicalDamage * (c.isProtected ? 0.25f : 1.0f);
                    c.vel -= nrm * (vn * 1.2f);
                }
            }
        }
    });
}

int World::allocCell() {
    int idx;
    if (!freeCells_.empty()) {
        idx = freeCells_.back();
        freeCells_.pop_back();
    } else {
        idx = (int)cells_.size();
        cells_.emplace_back();
    }
    Cell& c = cells_[idx];
    c = Cell{};
    c.alive = true;
    c.id = nextId_++;
    cellCount_++;
    return idx;
}

int World::allocLink() {
    int idx;
    if (!freeLinks_.empty()) {
        idx = freeLinks_.back();
        freeLinks_.pop_back();
    } else {
        idx = (int)links_.size();
        links_.emplace_back();
    }
    links_[idx] = Link{};
    links_[idx].alive = true;
    linkCount_++;
    return idx;
}

void World::setMode(Cell& c, int mode) {
    c.mode = (uint8_t)clampv(mode, 0, kMaxModes - 1);
    c.type = genomes_.get(c.genome).modes[c.mode].type;
    c.radius = radiusForMass(c.mass, c.type);
}

int World::addCell(const Genome& g, Vec2 pos, float angle, int mode, CellTag tag, float mass) {
    if (cellCount_ >= settings.maxCells) return -1;
    int idx = allocCell();
    Cell& c = cells_[idx];
    c.genome = genomes_.add(g);
    c.pos = pos;
    c.angle = angle;
    c.prevPos = pos;
    c.prevAngle = angle;
    c.mass = mass > 0 ? mass : 2.6f;
    c.age = previewMode ? 0.5f : 0.1f;
    c.nitrogen = 1.0f;
    c.tag = tag;
    setMode(c, mode >= 0 ? mode : g.initialMode);
    if (recordEvents) noteEvent(CellEvent::Born, idx, -1);
    return idx;
}

// ---------------------------------------------------------------- electrophysiology
float World::shockField(float volts, float radius, float d) {
    // A microelectrode tip in saline: the field is strongest at the tip and falls off ~1/d² past its reach.
    float R = std::max(radius, 1.0f);
    float x = std::max(d, 0.0f) / R;
    return volts / R / (1.0f + 3.0f * x * x);
}

int World::applyElectric(int i, float dPsi, float pulseMs) {
    Cell& c = cells_[i];
    if (!c.alive || previewMode) return 0;
    if (c.isProtected) dPsi *= 0.35f;                 // keratin armour insulates
    if (c.type == CellType::Lipocyte) dPsi *= 0.5f;   // so does fat
    if (dPsi < kExciteV) return 0;
    // The delivered charge grows with the pulse length (roughly like its square root for excitation and damage).
    float dur = std::sqrt(clampv(pulseMs, 0.05f, 50.0f));
    float ex = clampv((dPsi - kExciteV) / 0.05f, 0.0f, 1.0f);
    int result = 1;
    switch (c.type) {
        case CellType::Neurocyte:
            // The neuron fires a burst on every channel, then stays refractory (silent) for a while.
            for (float& s : c.signals) s = clampv(s + 40.0f * ex, -100.0f, 100.0f);
            c.stun = std::max(c.stun, 0.01f + 0.012f * ex * dur);
            break;
        case CellType::Myocyte: c.stun = std::max(c.stun, (0.03f + 0.02f * dur) * ex); break;  // tetanic cramp
        case CellType::Flagellocyte:
        case CellType::Ciliocyte: c.stun = std::max(c.stun, 0.04f * ex * dur); break;          // beating arrested
        case CellType::Senseocyte:
        case CellType::Stereocyte:
            for (float& s : c.signals) s = clampv(s + 10.0f * ex, -100.0f, 100.0f);
            break;
        case CellType::Cnidocyte: c.charge = 0; break;  // the thread fires off harmlessly
        case CellType::Luminocyte: c.actuator = 1.0f; break;  // a startled light organ flashes
        default:
            for (float& s : c.signals) s = clampv(s + 3.0f * ex, -100.0f, 100.0f);
            break;
    }
    if (dPsi > kPoreV) {
        // Electroporation: pores open, cytoplasm and nitrogen leak out, the membrane is wounded.
        result = 2;
        float por = (dPsi - kPoreV) / (kLysisV - kPoreV) * dur;
        c.injury += 0.35f * por;
        c.nitrogen = std::max(0.0f, c.nitrogen - 0.15f * por);
        float l = c.mass * clampv(0.12f * por, 0.0f, 0.5f) * std::min(settings.sapLeak, 1.0f);
        c.mass -= l;
        c.leak += l;
        if (dPsi > kLysisV && pulseMs >= 0.2f) {
            c.injury = std::max(c.injury, 1.0f);  // irreversible breakdown: the cell bursts next tick
            result = 3;
        }
    }
    return result;
}

World::ShockReport World::shock(Vec2 p, float radius, float volts, float pulseMs) {
    ShockReport rep;
    if (gridW_ <= 0) buildGrids();
    float reach = radius * 2.5f;
    std::vector<int> near;
    forEachCellNear(p, reach + 20.0f, [&](int j) { near.push_back(j); });
    for (int j : near) {
        Cell& c = cells_[j];
        float d = std::max(0.0f, length(c.pos - p) - c.radius);
        if (d > reach) continue;
        float E = shockField(volts, radius, d);
        float dPsi = 1.5f * E * c.radius;  // Schwan: induced transmembrane voltage of a sphere
        ++rep.hit;
        rep.maxDeltaPsi = std::max(rep.maxDeltaPsi, dPsi);
        int r = applyElectric(j, dPsi, pulseMs);
        if (r >= 1) ++rep.excited;
        if (r >= 2) ++rep.porated;
        if (r == 3) ++rep.lysed;
        // Electrophoresis: the negatively charged cells drift a little in the field.
        Vec2 away = c.pos - p;
        float l = length(away);
        if (l > 1e-3f) c.vel += away / l * std::min(3.0f, E * 30.0f * std::sqrt(clampv(pulseMs, 0.05f, 50.0f)));
    }
    return rep;
}

// ---------------------------------------------------------------- adhesin tool
int World::linkCells(int a, int b, float extraLength, float stiffness) {
    int n = (int)cells_.size();
    if (a < 0 || b < 0 || a >= n || b >= n || a == b || !cells_[a].alive || !cells_[b].alive) return -1;
    Vec2 d = cells_[b].pos - cells_[a].pos;
    // A bridge spans the gap the cells are apart now (plus the requested slack), so they are not yanked together.
    float gap = std::max(0.0f, length(d) - cells_[a].radius - cells_[b].radius);
    return createLink(a, b, std::min(gap + extraLength, 60.0f), clampv(stiffness, 0.0f, 1.0f), false);
}

int World::cutLinks(Vec2 a, Vec2 b) {
    auto segHit = [](Vec2 p1, Vec2 p2, Vec2 q1, Vec2 q2) {
        Vec2 r = p2 - p1, s = q2 - q1;
        float den = cross(r, s);
        if (std::fabs(den) < 1e-6f) return false;
        float t = cross(q1 - p1, s) / den, u = cross(q1 - p1, r) / den;
        return t >= 0 && t <= 1 && u >= 0 && u <= 1;
    };
    int cut = 0;
    for (size_t li = 0; li < links_.size(); ++li) {
        const Link& l = links_[li];
        if (!l.alive) continue;
        Vec2 pa = cells_[l.a].pos, pb = l.b >= 0 ? cells_[l.b].pos : l.wallPoint;
        if (segHit(a, b, pa, pb)) {
            removeLink((int)li);
            ++cut;
        }
    }
    return cut;
}

int World::glueTouching(Vec2 p, float radius, float stiffness) {
    if (gridW_ <= 0) buildGrids();
    std::vector<int> in;
    forEachCellNear(p, radius + 20.0f, [&](int j) {
        if (length(cells_[j].pos - p) < radius + cells_[j].radius * 0.5f) in.push_back(j);
    });
    int made = 0;
    for (size_t x = 0; x < in.size(); ++x)
        for (size_t y = x + 1; y < in.size(); ++y) {
            const Cell& A = cells_[in[x]];
            const Cell& B = cells_[in[y]];
            if (length(A.pos - B.pos) > (A.radius + B.radius) * 1.15f) continue;
            if (createLink(in[x], in[y], 0.0f, clampv(stiffness, 0.0f, 1.0f), false) >= 0) ++made;
        }
    return made;
}

// ---------------------------------------------------------------- dissolved substances
const char* fluidKindName(FluidKind k) {
    static const char* n[] = {"клеточный сок", "питательный бульон", "цианид", "тетродотоксин", "адреналин", "протеаза", "сигнал"};
    return n[clampv((int)k, 0, (int)FluidKind::Count - 1)];
}

void World::addFluid(Vec2 pos, FluidKind kind, float amount, uint32_t color, int channel, Vec2 vel) {
    if (amount <= 0.001f || previewMode) return;
    // A new drop joins a recent cloud of the same substance it lands in (a leaking cell feeds one cloud instead
    // of spawning hundreds): fewer clouds to simulate and to draw.
    const bool full = (int)fluids_.size() >= kMaxFluids;
    for (int k = (int)fluids_.size() - 1, seen = 0; k >= 0 && seen < 48; --k, ++seen) {
        Fluid& f = fluids_[k];
        if (f.kind != kind || f.channel != channel) continue;
        float reach = full ? 40.0f : std::max(6.0f, f.radius * 0.6f);
        if (length2(f.pos - pos) < reach * reach && (full || f.age < 0.5f)) {
            f.amount += amount;
            return;
        }
    }
    if (full) return;
    Fluid f;
    f.pos = pos;
    f.vel = vel;
    f.amount = amount;
    f.kind = kind;
    f.channel = (uint8_t)clampv(channel, 0, kSignalCount - 1);
    f.color = color;
    fluids_.push_back(f);
}

void World::spillSap(Cell& c, float amount, Vec2 where) {
    if (amount <= 0.001f) return;
    Vec2 out = where - c.pos;
    float l = length(out);
    out = l > 1e-3f ? out / l : fromAngle(rng_.range(0, kTau));
    addFluid(where, FluidKind::Sap, amount, packColor(cellColor(c), 1.0f), 0, out * 0.6f + c.vel * 0.5f);
}

void World::injectSubstance(int idx, Substance s, float amount, int channel) {
    if (idx < 0 || idx >= (int)cells_.size() || !cells_[idx].alive) return;
    Cell& c = cells_[idx];
    switch (s) {
        case Substance::Glucose: c.mass = std::min(c.mass + amount, cellCapacity(c.type)); break;           // ng
        case Substance::Nitrate: c.nitrogen = clampv(c.nitrogen + amount * 0.01f, 0.0f, 1.0f); break;      // %
        case Substance::Signal: {
            float& v = c.signals[clampv(channel, 0, kSignalCount - 1)];
            v = clampv(v + amount, -100.0f, 100.0f);
            break;
        }
        case Substance::Cyanide: c.injury += amount * (c.isProtected ? 0.4f : 1.0f); break;  // ~1 ng kills a cell
        case Substance::Tetrodotoxin: c.paralysis += amount * 2.0f; break;                    // 2 h per ng
        case Substance::Adrenaline: c.stimulant += amount * 2.0f; break;                      // 2 h per ng
        case Substance::Protease: {
            // Digests adhesins: a dose of 0.2 ng frees the cell completely, less cuts some of its bonds.
            float p = clampv(amount / 0.2f, 0.0f, 1.0f);
            for (int k = c.linkCount - 1; k >= 0; --k)
                if (rng_.chance(p)) removeLink(c.links[k]);
            c.injury += 0.1f * amount;
            break;
        }
        case Substance::Regenerant: c.injury = std::max(0.0f, c.injury - amount); break;
        case Substance::Mutagen: {
            // The cell gets its own mutated copy of the genome; its offspring inherit the change.
            Genome g = genomes_.get(c.genome);
            MutationOptions opt;
            opt.allowedTypes = settings.genePool.data();
            int n = std::max(1, (int)std::lround(amount));
            for (int k = 0; k < n; ++k) mutateGenome(g, c.mode, rng_, opt);
            int id = genomes_.add(g);
            genomes_.release(c.genome);
            c.genome = id;
            c.mutations += (uint16_t)n;
            setMode(c, c.mode);
            break;
        }
        // ---- CellFap substances: 4 h of effect per ng ----
        case Substance::Amylase: c.amylase += amount * 4.0f; break;                 // enzymes keep working while the timer runs
        case Substance::HyaluronicAcid: c.hyaluronicAcid += amount * 4.0f; break;     // mode chosen in Settings::hyaluronicMode
        case Substance::Epibrassinolide: c.epibrassinolide += amount * 4.0f; break;   // photosynthesis +100 % at 1 ng
        case Substance::Benzyladin: c.benzyladin += amount * 4.0f; break;            // longer life
        case Substance::GibberellicAcid: c.gibberellicAcid += amount * 4.0f; break;  // grows the cell, up to 40 ng
        case Substance::Colchicine: c.colchicine += amount * 4.0f; break;            // no division while present
        case Substance::Penicillin: c.penicillin += amount * 4.0f; break;            // eats keratin armour
        case Substance::Interferon: c.interferon += amount * 4.0f; break;            // blocks infection
        case Substance::Resveratrol: c.resveratrol += amount * 4.0f; break;          // radiation resistance
        case Substance::Ryanodine: c.ryanodine += amount * 4.0f; break;              // myocyte spasm
        case Substance::Cycloheximide: c.cycloheximide += amount * 4.0f; break;      // no energy uptake
        case Substance::Osteopontin: c.osteopontin += amount * 4.0f; break;          // soft but well-bonded
        case Substance::ChondroitinSulfate: c.chondroitinSulfate += amount * 4.0f; break;  // adhesin transport blocked
        case Substance::Edrophonium: c.edrophonium += amount * 4.0f; break;          // eats tetrodotoxin
        case Substance::Atractyloside: c.atractyloside += amount * 4.0f; break;      // blocks the energy yield
        default: break;
    }
}

bool World::releaseSubstance(Vec2 p, Substance s, float amount, int channel) {
    FluidKind k;
    switch (s) {
        case Substance::Glucose: k = FluidKind::Broth; break;
        case Substance::Signal: k = FluidKind::Signal; amount = std::fabs(amount) * 0.02f; break;  // units -> ng of messenger
        case Substance::Cyanide: k = FluidKind::Toxin; break;
        case Substance::Tetrodotoxin: k = FluidKind::Neurotoxin; break;
        case Substance::Adrenaline: k = FluidKind::Stimulant; break;
        case Substance::Protease: k = FluidKind::Enzyme; break;
        default: return false;
    }
    addFluid(p, k, amount, 0, channel);
    return true;
}

int World::injectGenome(int idx, const Genome& g, int mode, bool wholeOrganism) {
    if (idx < 0 || idx >= (int)cells_.size() || !cells_[idx].alive) return 0;
    std::vector<int> targets{idx};
    if (wholeOrganism) {
        std::vector<char> seen(cells_.size(), 0);
        seen[idx] = 1;
        for (size_t q = 0; q < targets.size(); ++q) {
            const Cell& c = cells_[targets[q]];
            for (int k = 0; k < c.linkCount; ++k) {
                const Link& l = links_[c.links[k]];
                int o = l.a == targets[q] ? l.b : l.a;
                if (o >= 0 && !seen[o] && cells_[o].alive) {
                    seen[o] = 1;
                    targets.push_back(o);
                }
            }
        }
    }
    int id = genomes_.add(g);  // one shared copy for the whole injection
    for (int t : targets) {
        Cell& c = cells_[t];
        genomes_.retain(id);
        genomes_.release(c.genome);
        c.genome = id;
        int m = (t == idx && mode >= 0) ? mode : c.mode;
        setMode(c, m);
        c.tag = CellTag::User;
    }
    genomes_.release(id);  // drop the reference add() took
    return (int)targets.size();
}

int World::addCellLike(int src, Vec2 pos, float angle, int mode, float mass, bool mirrored) {
    if (src < 0 || src >= (int)cells_.size() || !cells_[src].alive || cellCount_ >= settings.maxCells) return -1;
    int genome = cells_[src].genome;
    int idx = allocCell();
    Cell& c = cells_[idx];
    c.genome = genome;
    genomes_.retain(genome);
    c.pos = c.prevPos = pos;
    c.angle = c.prevAngle = angle;
    c.mass = mass;
    c.age = 0.5f;
    c.mirrored = mirrored;
    c.nitrogen = 1.0f;
    setMode(c, mode);
    if (recordEvents) noteEvent(CellEvent::Born, idx, -1);
    return idx;
}

bool World::hasLink(int a, int b) const {
    const Cell& c = cells_[a];
    for (int i = 0; i < c.linkCount; ++i) {
        const Link& l = links_[c.links[i]];
        if ((l.a == a && l.b == b) || (l.b == a && l.a == b)) return true;
    }
    return false;
}

int World::createLink(int a, int b, float extraLen, float stiffness, bool fromDivision) {
    Cell& A = cells_[a];
    Cell& B = cells_[b];
    if (a == b || A.linkCount >= kMaxLinks || B.linkCount >= kMaxLinks || hasLink(a, b)) return -1;
    int li = allocLink();
    Link& l = links_[li];
    l.a = a;
    l.b = b;
    Vec2 d = B.pos - A.pos;
    float phi = std::atan2(d.y, d.x);
    l.angA = wrapAngle(phi - A.angle);
    l.angB = wrapAngle(phi + kPi - B.angle);
    l.extraLength = extraLen;
    l.stiffness = stiffness;
    l.fromDivision = fromDivision;
    A.links[A.linkCount++] = li;
    B.links[B.linkCount++] = li;
    return li;
}

int World::createWallLink(int a, Vec2 wallPoint) {
    Cell& A = cells_[a];
    if (A.linkCount >= kMaxLinks) return -1;
    int li = allocLink();
    Link& l = links_[li];
    l.a = a;
    l.b = -1;
    l.wallPoint = wallPoint;
    Vec2 d = wallPoint - A.pos;
    l.angA = wrapAngle(std::atan2(d.y, d.x) - A.angle);
    A.links[A.linkCount++] = li;
    A.glueWallLinks++;
    return li;
}

static void detachFrom(Cell& c, int li) {
    for (int i = 0; i < c.linkCount; ++i)
        if (c.links[i] == li) {
            c.links[i] = c.links[--c.linkCount];
            return;
        }
}

void World::removeLink(int li) {
    Link& l = links_[li];
    if (!l.alive) return;
    detachFrom(cells_[l.a], li);
    if (l.b >= 0) detachFrom(cells_[l.b], li);
    else if (cells_[l.a].glueWallLinks) cells_[l.a].glueWallLinks--;
    l.alive = false;
    freeLinks_.push_back(li);
    linkCount_--;
}

void World::killCell(int idx, bool leaveFood, bool lysis) {
    Cell& c = cells_[idx];
    if (!c.alive) return;
    if (recordEvents) noteEvent(CellEvent::Died, idx, -1);
    while (c.linkCount) removeLink(c.links[c.linkCount - 1]);
    if (leaveFood && !previewMode && c.mass > 0.05f) {
        float remains = c.mass * 0.9f;
        if (c.leak > 0) spillSap(c, c.leak, c.pos + fromAngle(rng_.range(0, kTau)) * c.radius);
        if (lysis && (c.tag == CellTag::Challenge || c.tag == CellTag::InfectedChallenge)) ++lysedChallenge;
        if (lysis && settings.sapLeak > 0) {
            // A burst cell spills its cytoplasm in a few gushes; the torn membrane and organelles stay as detritus.
            float sap = c.mass * 0.55f * std::min(settings.sapLeak, 1.0f);
            remains = c.mass * 0.9f - sap;
            int n = 3 + (int)(c.radius / 5.0f);
            uint32_t col = packColor(cellColor(c), 1.0f);
            for (int k = 0; k < n; ++k) {
                Vec2 dir = fromAngle(rng_.range(0, kTau));
                addFluid(c.pos + dir * (c.radius * 0.6f), FluidKind::Sap, sap / n, col, 0, dir * rng_.range(0.5f, 1.2f) + c.vel * 0.5f);
            }
        }
        // A dead cell always leaves its remains as food (up to twice the dish limit): the cap is for the food rain.
        if ((int)food_.size() < settings.maxFood * 2 && remains > 0.05f) {
            Food f;
            f.pos = c.pos;
            f.vel = c.vel * 0.3f;
            f.mass = remains;
            f.coating = c.type == CellType::Lipocyte ? 0.5f : 0.0f;
            food_.push_back(f);
        }
    }
    genomes_.release(c.genome);
    c.genome = -1;
    c.alive = false;
    freeCells_.push_back(idx);
    cellCount_--;
    if (dragIdx_ == idx) dragIdx_ = -1;
}

void World::removeCell(int idx, bool leaveFood) {
    if (idx >= 0 && idx < (int)cells_.size()) killCell(idx, leaveFood);
}

void World::boostCell(int idx) {
    if (idx < 0 || idx >= (int)cells_.size() || !cells_[idx].alive) return;
    Cell& c = cells_[idx];
    c.mass = kMaxCellMass;
    c.nitrogen = 1.0f;
    c.injury = 0.0f;
    c.boost = 1.0f;
    c.radius = radiusForMass(c.mass, c.type);
}

void World::addFood(Vec2 pos, float mass, float coating) {
    if ((int)food_.size() >= settings.maxFood) return;
    Food f{pos, {}, mass, coating};
    food_.push_back(f);
}

void World::dragCell(int idx, Vec2 target) {
    dragIdx_ = idx;
    dragTarget_ = target;
}

float World::lightAt(Vec2 p) const {
    // The flashlight: a bright disc with a soft edge, added to whatever light the dish has (night included).
    float spot = 0;
    if (spotRadius_ > 0) {
        const float q = length(p - spotPos_) / spotRadius_;
        if (q < 1.0f) spot = spotPower_ * (1.0f - smoothstep(0.55f, 1.0f, q));
    }
    if (settings.lightAmount <= 0 || daylight_ <= 0) return spot;
    return spot + dishLightAt(p);
}

float World::dishLightAt(Vec2 p) const {
    float R = settings.dishRadius;
    float s = dot(p / R, fromAngle(settings.lightAngle));
    if (settings.cellLabPhysics && !previewMode) {
        // Cell Lab's light field: brightest at the lit rim, falling off across the dish and toward the centre; the
        // light range widens it.
        float rho = std::max(settings.lightRange, 0.01f);
        float q = std::min(length(p) / R, 1.0f);
        float den = 1.0f + (1.0f - rho) * (1.0f - q) / rho;
        return std::max(0.0f, settings.lightAmount * daylight_ * (s * (1.0f - rho) + rho) / (den * den));
    }
    float edge = 1.0f - 2.5f * settings.lightRange;
    return settings.lightAmount * daylight_ * smoothstep(edge - 0.25f, edge + 0.25f, s);
}

// Cell Lab's senses. Smells are sums of mass·K(d/λ) over nearby sources — K0 for a plain eye, K1 times the side
// (cross product with the heading) for a stereo eye: cells of the sense colour (its own body too), food pieces (coated
// or not), and secrocytes giving off the matching smell, which carries 4× farther. Light reads the game's light field
// (stereo: its gradient across the axis), velocity the cell's own speed, the wall a 135 µm ramp toward the rim. Sources
// are searched up to 1.5 of the game's smell-grid square (≥ 180 µm). No limit on the stimulus itself.
float World::clSense(const Cell& c, int i, const Mode& m, bool stereo) const {
    const float R = settings.dishRadius;
    const Vec2 fwd = fromAngle(c.angle);
    const int buckets = std::max(1, std::min((int)(2.0f * R / 180.0f), 207));
    const float range = 1.5f * (2.0f * R / (float)buckets);
    const BesselTables& K = bessel();
    float st = 0;
    auto smell = [&](Vec2 p, float mass, float lambda) {
        const Vec2 d = p - c.pos;
        const float dist = length(d);
        if (dist <= 0) return;
        const int k = besselIndex(dist / lambda);
        st += stereo ? mass * K.k1[k] * cross(fwd, d / dist) : mass * K.k0[k];
    };
    const SenseTarget t = m.senseTarget;
    const bool wantsCells = t == SenseTarget::Cell;
    const int smellOf = t == SenseTarget::Food ? (int)Secretion::FoodSmell : t == SenseTarget::CoatedFood ? (int)Secretion::CoatedFoodSmell
                        : t == SenseTarget::Light ? (int)Secretion::LightActivator : t == SenseTarget::Wall ? (int)Secretion::WallActivator : -1;
    if (wantsCells || smellOf >= 0)
        forNeighbors(c.pos, range, [&](int j) {
            if (j == i) return;
            const Cell& o = cells_[j];
            if (!o.alive) return;
            if (wantsCells) {
                Color oc = cellColor(o);
                float dr = oc.r - m.senseColor.r, dg = oc.g - m.senseColor.g, db = oc.b - m.senseColor.b;
                if (dr * dr + dg * dg + db * db < m.senseThreshold * m.senseThreshold) smell(o.pos, o.mass, 45.0f);
            } else if (o.type == CellType::Secrocyte && o.nitrogen >= 0.2f && (int)modeOf(o).secretion == smellOf) {
                smell(o.pos, o.mass, 180.0f);
            }
        });
    if (t == SenseTarget::Food || t == SenseTarget::CoatedFood) {
        const bool coatedWanted = t == SenseTarget::CoatedFood;
        forFoodNear(c.pos, range, [&](int fi) {
            const Food& f = food_[fi];
            if ((f.coating > 0) == coatedWanted) smell(f.pos, f.mass, 45.0f);
        });
    }
    switch (t) {
        case SenseTarget::Light: {
            const float L = lightAt(c.pos);
            if (!stereo) {
                st += 0.2f * L;
            } else if (L > 0) {  // the analytic gradient of the game's light field, per µm
                const float I = settings.lightAmount * daylight_, rho = std::max(settings.lightRange, 0.01f), rp = 1.0f - rho;
                const Vec2 ld = fromAngle(settings.lightAngle);
                const float pl = length(c.pos), s = dot(c.pos, ld) / R;
                const float D = 1.0f + rp * (1.0f - std::min(pl / R, 1.0f)) / rho;
                Vec2 grad = ld * (I * rp / (R * D * D));
                if (pl > 1e-3f) grad += c.pos / pl * (2.0f * I * (s * rp + rho) * rp / (rho * R * D * D * D));
                st += 100.0f * cross(fwd, grad);
            }
            break;
        }
        case SenseTarget::Velocity: st += stereo ? -2.4f * cross(fwd, c.vel) : 2.4f * length(c.vel); break;
        case SenseTarget::Wall: {
            const float pl = length(c.pos);
            const float w = clampv(1.0f - (R - pl) / 135.0f, 0.0f, 1.0f);
            st += stereo ? (pl > 0 ? w * cross(fwd, c.pos / pl) : 0.0f) : w;
            break;
        }
        default: break;
    }
    return st;
}

float World::daylightAt(const Settings& s, double hours) {
    if (s.dayLength <= 0.01f) return 1.0f;
    // The dish starts at sunrise; day and night each last half the period, with an hour-ish of dusk and dawn.
    float x = (float)std::sin(kTau * std::fmod(hours, (double)s.dayLength) / s.dayLength);
    return smoothstep(-0.2f, 0.35f, x);
}

// Sap clouds bucketed on a coarse grid (counting sort), for the "Blood" sense. Only when somebody smells.
void World::buildFluidGrid() {
    const float ext = settings.dishRadius + kSenseRange;
    fluidCell_ = kBloodGrid;
    fluidGW_ = std::max(1, (int)std::ceil(2.0f * ext / fluidCell_));
    const int nb = fluidGW_ * fluidGW_;
    fluidGridStart_.assign(nb + 1, 0);
    auto bucket = [&](Vec2 p) {
        int x = clampv((int)((p.x + ext) / fluidCell_), 0, fluidGW_ - 1);
        int y = clampv((int)((p.y + ext) / fluidCell_), 0, fluidGW_ - 1);
        return y * fluidGW_ + x;
    };
    for (const Fluid& f : fluids_)
        if (f.kind == FluidKind::Sap) fluidGridStart_[bucket(f.pos) + 1]++;
    for (int b = 0; b < nb; ++b) fluidGridStart_[b + 1] += fluidGridStart_[b];
    fluidGridItems_.resize(fluidGridStart_[nb]);
    std::vector<int> fill(fluidGridStart_.begin(), fluidGridStart_.end() - 1);
    for (int k = 0; k < (int)fluids_.size(); ++k)
        if (fluids_[k].kind == FluidKind::Sap) fluidGridItems_[fill[bucket(fluids_[k].pos)]++] = k;
}

// ---------------------------------------------------------------- spatial grids
void World::buildGrids() {
    float maxR = 4.0f;
    for (const Cell& c : cells_)
        if (c.alive) maxR = std::max(maxR, c.radius);
    gridCell_ = 2.0f * maxR + 8.0f;
    float ext = settings.dishRadius + 2.0f * gridCell_;
    gridOrigin_ = {-ext, -ext};
    int n = (int)std::ceil(2.0f * ext / gridCell_);
    gridW_ = gridH_ = std::max(1, n);
    size_t buckets = (size_t)gridW_ * gridH_;

    auto bucketOf = [&](Vec2 p) {
        int x = clampv((int)((p.x - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
        int y = clampv((int)((p.y - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
        return y * gridW_ + x;
    };

    // Cells
    cellGridStart_.assign(buckets + 1, 0);
    static thread_local std::vector<int> bucketIdx;
    bucketIdx.resize(cells_.size());
    for (size_t i = 0; i < cells_.size(); ++i) {
        if (!cells_[i].alive) continue;
        int b = bucketOf(cells_[i].pos);
        bucketIdx[i] = b;
        cellGridStart_[b + 1]++;
    }
    for (size_t b = 0; b < buckets; ++b) cellGridStart_[b + 1] += cellGridStart_[b];
    cellGridItems_.resize(cellCount_);
    {
        std::vector<int> cursor(cellGridStart_.begin(), cellGridStart_.end() - 1);
        for (size_t i = 0; i < cells_.size(); ++i)
            if (cells_[i].alive) cellGridItems_[cursor[bucketIdx[i]]++] = (int)i;
    }

    // Food
    foodGridStart_.assign(buckets + 1, 0);
    bucketIdx.resize(food_.size());
    for (size_t i = 0; i < food_.size(); ++i) {
        int b = bucketOf(food_[i].pos);
        bucketIdx[i] = b;
        foodGridStart_[b + 1]++;
    }
    for (size_t b = 0; b < buckets; ++b) foodGridStart_[b + 1] += foodGridStart_[b];
    foodGridItems_.resize(food_.size());
    {
        std::vector<int> cursor(foodGridStart_.begin(), foodGridStart_.end() - 1);
        for (size_t i = 0; i < food_.size(); ++i) foodGridItems_[cursor[bucketIdx[i]]++] = (int)i;
    }
}

template <class F>
void World::forNeighbors(Vec2 p, float range, F&& f) const {
    if (gridW_ <= 0) return;
    int x0 = clampv((int)((p.x - range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
    int x1 = clampv((int)((p.x + range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
    int y0 = clampv((int)((p.y - range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
    int y1 = clampv((int)((p.y + range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int b = y * gridW_ + x;
            for (int k = cellGridStart_[b]; k < cellGridStart_[b + 1]; ++k) f(cellGridItems_[k]);
        }
}

template <class F>
void World::forFoodNear(Vec2 p, float range, F&& f) const {
    if (gridW_ <= 0 || foodGridStart_.empty()) return;
    int x0 = clampv((int)((p.x - range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
    int x1 = clampv((int)((p.x + range - gridOrigin_.x) / gridCell_), 0, gridW_ - 1);
    int y0 = clampv((int)((p.y - range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
    int y1 = clampv((int)((p.y + range - gridOrigin_.y) / gridCell_), 0, gridH_ - 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int b = y * gridW_ + x;
            for (int k = foodGridStart_[b]; k < foodGridStart_[b + 1]; ++k) f(foodGridItems_[k]);
        }
}

int World::pickCell(Vec2 p) const {
    int best = -1;
    float bestD = 1e30f;
    // The grid may be stale between ticks; a linear fallback keeps picking exact.
    for (size_t i = 0; i < cells_.size(); ++i) {
        const Cell& c = cells_[i];
        if (!c.alive) continue;
        float d = length2(c.pos - p);
        float r = c.radius * 1.15f;
        if (d < r * r && d < bestD) {
            bestD = d;
            best = (int)i;
        }
    }
    return best;
}

// ---------------------------------------------------------------- simulation step
void World::computeProtection() {
    jobs_->parallelFor((int)cells_.size(), 512, [&](int b, int e) {
        for (int i = b; i < e; ++i) {
            Cell& c = cells_[i];
            if (!c.alive) continue;
            // Пенициллин разрушает кератиновую оболочку, поэтому страдает только кератиноцит: у остальных типов
            // такой оболочки нет, поэтому они (включая будущие остеоциты, которых в CellFap пока нет как типа)
            // пенициллину не подвержены по определению. Циклоспориноцит защищён и закрывает только себя —
            // соседей по адгезину он не прикрывает, как и циста.
            auto penicillinBreaks = [](const Cell& x) {
                return x.type == CellType::Keratinocyte && x.penicillin > 0.0f;
            };
            // A cyst and a cyclosporinocyte armour only themselves (the latter: a shell penicillin cannot break).
            bool prot = c.type == CellType::Keratinocyte || c.type == CellType::Cystocyte || c.type == CellType::Cyclosporinocyte;
            if (prot && penicillinBreaks(c)) prot = false;
            for (int k = 0; k < c.linkCount && !prot; ++k) {
                const Link& l = links_[c.links[k]];
                int o = l.a == i ? l.b : l.a;
                if (o >= 0 && cells_[o].type == CellType::Keratinocyte && !penicillinBreaks(cells_[o])) prot = true;
            }
            c.isProtected = prot;
        }
    });
}

void World::step() {
    if (beforeStep && !beforeStep(*this)) {
        // A mod moves the dish itself this tick: only the clock runs (and the mods after the step).
        for (Cell& c : cells_) {
            c.prevPos = c.pos;
            c.prevAngle = c.angle;
        }
        time_ += kHoursPerTick;
        ticks_++;
        daylight_ = daylightAt(settings, time_);
        if (afterStep) afterStep(*this);
        return;
    }
    Scratch& S = *scratch_;
    const float h = kHoursPerTick;
    const float R = settings.dishRadius;
    const int n = (int)cells_.size();
    const float salinityFactor = std::pow(14.0f, -settings.salinity);
    // Cell Lab's physics and biology instead of ours (a dish from Cell Lab / MinLab). The genome preview keeps ours.
    const bool cl = settings.cellLabPhysics && !previewMode;
    // Shared light: how many photosynthesising cells the dish can feed (a cell covers ~450 µm²).
    const float lightShare = [&] {
        float capacity = std::max(settings.lightCapacity, 0.01f) * kPi * R * R / 450.0f;
        int photo = typeCounts_[(int)CellType::Photocyte];
        // Up to the capacity everyone gets full light; beyond it the same light is shared by more cells. (It used to
        // fade to nothing within another 25%: every alga starved at once and the whole population crashed.)
        return photo <= capacity ? 1.0f : capacity / (float)photo;
    }();

    using Clock = std::chrono::steady_clock;
    auto tPrev = Clock::now();
    auto mark = [&](int k) {
        auto now = Clock::now();
        // Exponential moving average so the overlay is readable.
        phaseMs[k] = phaseMs[k] * 0.9 + 0.1 * std::chrono::duration<double, std::milli>(now - tPrev).count();
        tPrev = now;
    };
    daylight_ = daylightAt(settings, time_);
    buildGrids();
    if (barrierGridDirty_ && !barriers_.empty()) buildBarrierGrid();  // read by the parallel passes below
    computeProtection();
    // Somebody smells blood: bucket the sap clouds (read by the parallel sense pass).
    bool smellBlood = false;
    if (!fluids_.empty())
        for (const Cell& c : cells_)
            if (c.alive && (c.type == CellType::Senseocyte || c.type == CellType::Stereocyte) &&
                modeOf(c).senseTarget == SenseTarget::Blood) {
                smellBlood = true;
                break;
            }
    if (smellBlood) buildFluidGrid();
    else fluidGW_ = 0;
    mark(0);

    S.force.assign(n, Vec2{});
    S.torque.assign(n, 0.0f);
    S.dMass.assign(n, 0.0f);
    S.dNitro.assign(n, 0.0f);
    S.dInjury.assign(n, 0.0f);
    S.dSignal.assign(n, {});
    S.dToxic.assign(n, 0.0f);
    S.signalRuns.clear();
    S.signalInputs.clear();
    S.signalResults.clear();
    S.eaten.assign(n, 0.0f);
    S.act.resize(n);
    S.hard.resize(n);
    S.squeeze.assign(n, 1.0f);
    S.nbr.resize((size_t)n * kMaxNbr);
    S.nbrCount.assign(n, 0);
    S.dead.assign(n, 0);
    S.wantsSplit.assign(n, 0);
    S.discharge.assign(n, 0);
    S.newMode.assign(n, -1);
    S.brokenLink.assign(links_.size(), 0);
    S.events.clear();
    S.eats.clear();
    S.secrocytes.clear();

    // ---- 1. programmable values + senses -----------------------------------------
    jobs_->parallelFor(n, 256, [&](int b, int e) {
        for (int i = b; i < e; ++i) {
            const Cell& c = cells_[i];
            if (!c.alive) continue;
            const Mode& m = modeOf(c);
            S.hard[i] = m.cytoskeleton;
            ProgInputs in;
            in.v[(int)ProgInput::Fixed] = 0;
            for (int s = 0; s < kSignalCount; ++s) in.v[(int)signalInput(s)] = c.signals[s];
            in.v[(int)ProgInput::Mass] = c.mass / kMaxCellMass;
            in.v[(int)ProgInput::Age] = c.age / kMaxAge;
            in.ageHours = c.age;
            in.v[(int)ProgInput::Nitrogen] = c.nitrogen;
            in.v[(int)ProgInput::Connections] = c.linkCount / (float)kMaxLinks;
            in.v[(int)ProgInput::Light] = lightAt(c.pos);
            in.v[(int)ProgInput::Injury] = c.injury;
            in.v[(int)ProgInput::Food] = 0;
            {
                // "Food nearby" (0..1, ~3 ng of food close by reads as 1) costs a search: only for cells that use it.
                bool wantsFood = false;
                for (int k = 0; k < PF_Count && !wantsFood; ++k) wantsFood = m.prog[k].input == ProgInput::Food;
                if (wantsFood && !previewMode) {
                    float sum = 0;
                    forFoodNear(c.pos, kFoodReach, [&](int fi) {
                        float d = length(food_[fi].pos - c.pos);
                        if (d < kFoodReach) sum += food_[fi].mass * (1.0f - d / kFoodReach);
                    });
                    in.v[(int)ProgInput::Food] = std::min(1.0f, sum / 3.0f);
                }
            }
            if (c.type == CellType::Cystocyte) S.hard[i] = std::max(m.cytoskeleton, 1.8f);  // a hard shell
            // Остеопотин: клетка мягче и гибче; связи остаются жёсткими (жёсткость связи — это adhesinStiffness,
            // её остеопотин не трогает).
            if (c.osteopontin > 0.0f) S.hard[i] = m.cytoskeleton / (1.0f + c.osteopontin);
            auto& a = S.act[i];
            a.fill(0.0f);
            // Drugs and electric after-effects: stun = refractory nerves / cramped muscles / stalled flagella,
            // paralysis = neurotoxin (nerves, senses and muscles silent), stimulant = adrenaline-like boost.
            const bool stunned = c.stun > 0, paralysed = c.paralysis > 0;
            const float boost = c.stimulant > 0 ? 1.5f : 1.0f;
            switch (c.type) {
                case CellType::Flagellocyte:
                    a[A_Swim] = stunned ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim) * boost;
                    break;
                case CellType::Buoyocyte: a[A_Density] = evalProg(m.prog[PF_Density], in, PF_Density); break;
                case CellType::Myocyte: {
                    // Muscle fibres ramp toward their command instead of twitching (a step in the signal would
                    // otherwise yank the tissue apart): first-order response with time constant 1/kMuscleRate.
                    // The current state lives in actuator (contraction) and lastSense (bend).
                    float rate = std::min(1.0f, h * kMuscleRate * (c.stimulant > 0 ? 2.0f : 1.0f));
                    float con = evalProg(m.prog[PF_Contraction], in, PF_Contraction);
                    float bend = evalProg(m.prog[PF_Bend], in, PF_Bend);
                    if (paralysed) con = bend = 0;  // slack
                    if (stunned) {                  // tetanus: every fibre contracts at once
                        con = 0.5f;
                        rate = std::min(1.0f, rate * 4.0f);
                    }
                    // Рианодин: принудительные спазмы — волокно сокращается само, без команды программы.
                    if (c.ryanodine > 0.0f) {
                        const float spasm = 0.35f + 0.15f * std::sin(c.age * 18.0f);  // 0.2..0.5
                        con = std::max(con, spasm);
                        rate = std::min(1.0f, rate * 6.0f);
                    }
                    if (cl) rate = 1.0f;  // Cell Lab applies the muscle values at once
                    a[A_Contract] = c.actuator + (con - c.actuator) * rate;
                    a[A_Bend] = c.lastSense + (bend - c.lastSense) * rate;
                    S.squeeze[i] = 1.0f - 0.8f * std::max(0.0f, a[A_Contract]);
                    a[A_Lift] = paralysed ? 0.0f : evalProg(m.prog[PF_Lift], in, PF_Lift);
                    break;
                }
                case CellType::Ciliocyte:
                    a[A_Bend] = stunned ? 0.0f : evalProg(m.prog[PF_Bend], in, PF_Bend) * boost;
                    a[A_Lift] = stunned ? 0.0f : evalProg(m.prog[PF_Lift], in, PF_Lift) * boost;
                    break;
                case CellType::Electrocyte:
                case CellType::Cnidocyte:
                    a[A_Swim] = (stunned || paralysed) ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim);  // trigger / ready
                    break;
                case CellType::Chromatocyte: {
                    // Pigment granules spread or gather over a few minutes.
                    float target = paralysed ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim);
                    a[A_Swim] = c.actuator + (target - c.actuator) * std::min(1.0f, h * 6.0f);
                    break;
                }
                case CellType::Luminocyte: {
                    // Luciferin lights up within a few minutes and fades as fast; nerve toxins put the light out.
                    float target = paralysed ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim);
                    a[A_Swim] = c.actuator + (target - c.actuator) * std::min(1.0f, h * 12.0f);
                    break;
                }
                case CellType::Grappler: a[A_Swim] = (stunned || paralysed) ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim); break;
                case CellType::Cystocyte: a[A_Stem1] = evalProg(m.prog[PF_Stem1], in, PF_Stem1); break;
                case CellType::Stemocyte:
                    a[A_Stem1] = evalProg(m.prog[PF_Stem1], in, PF_Stem1);
                    a[A_Stem2] = evalProg(m.prog[PF_Stem2], in, PF_Stem2);
                    break;
                case CellType::Neurocyte: {
                    float net[kSignalCount] = {};
                    if (!stunned && !paralysed)  // refractory after a shock / blocked by the toxin
                        for (int k = 0; k < 4; ++k)
                            net[m.neuroChannel[k] % kSignalCount] += evalProg(m.prog[PF_Neuro1 + k], in, PF_Neuro1 + k);
                    // (Cell Lab: the outputs simply add up; the signal itself is limited to ±1.)
                    for (int s = 0; s < kSignalCount; ++s) S.dSignal[i][s] += cl ? net[s] : clampv(net[s], -20.0f, 20.0f);
                    {  // activity shown by the renderer: the strongest output channel
                        float act = 0;
                        for (int k = 0; k < kSignalCount; ++k) act = std::max(act, std::fabs(net[k]));
                        a[A_Sense] = clampv(act / 20.0f, 0.0f, 1.0f);
                    }
                    break;
                }
                case CellType::Senseocyte:
                case CellType::Stereocyte: {
                    bool stereo = c.type == CellType::Stereocyte;
                    if (cl) {  // Cell Lab's senses (see clSense); a mirrored stereo eye has its sides swapped
                        float st = clSense(c, i, m, stereo);
                        if (stereo && c.mirrored) st = -st;
                        if (paralysed) st = 0;
                        // (its ±20 limit on a sensor's production, in our signal scale ×4)
                        S.dSignal[i][m.senseChannel % kSignalCount] += clampv(m.senseOutput * st, -80.0f, 80.0f);
                        a[A_Sense] = clampv(std::fabs(st), 0.0f, 1.0f);
                        break;
                    }
                    Vec2 fwd = fromAngle(c.angle);
                    float stim = 0;
                    // Targets (cells, food): a stereo eye reports WHICH SIDE the targets are on, weighted toward the
                    // nearest ones, with a strength that grows as they come closer — even one far crumb gives a clear
                    // direction, and food on both sides no longer cancels out. A plain eye reports how much it sees.
                    float tgtSigned = 0, tgtSum = 0, tgtMax = 0;
                    auto see = [&](Vec2 p, float k) {
                        Vec2 d = p - c.pos;
                        float dist = length(d);
                        if (dist > kSenseRange) return;
                        float w = 1.0f - dist / kSenseRange;
                        w *= w * k;
                        tgtSum += w;
                        tgtMax = std::max(tgtMax, w);
                        // Signed by the sine of the bearing: dead ahead 0, abeam ±1 — smooth proportional steering. A
                        // target behind counts fully on its side, so the cell turns around instead of swimming away.
                        if (dist > 1e-3f) {
                            Vec2 dir = d / dist;
                            float sn = cross(fwd, dir);
                            tgtSigned += w * (dot(fwd, dir) >= 0.0f ? sn : (sn >= 0.0f ? 1.0f : -1.0f));
                        }
                    };
                    switch (m.senseTarget) {
                        case SenseTarget::Cell:
                        case SenseTarget::Food:
                        case SenseTarget::CoatedFood:
                        case SenseTarget::Light:
                        case SenseTarget::Wall: {
                            forNeighbors(c.pos, kSenseRange, [&](int j) {
                                if (j == i) return;
                                const Cell& o = cells_[j];
                                const Mode& om = modeOf(o);
                                bool smelly = o.type == CellType::Secrocyte;
                                Secretion sec = om.secretion;
                                if (m.senseTarget == SenseTarget::Cell) {
                                    // An eye never looks at its own organism (same genome: its body and its brood) —
                                    // otherwise a green worm hunting green prey chases its own tail.
                                    if (o.genome == c.genome) return;
                                    if (smelly && (sec == Secretion::FoodSmell || sec == Secretion::CoatedFoodSmell ||
                                                   sec == Secretion::LightActivator || sec == Secretion::WallActivator))
                                        return;  // camouflaged
                                    Color oc = cellColor(o);
                                    float dr = oc.r - m.senseColor.r;
                                    float dg = oc.g - m.senseColor.g, db = oc.b - m.senseColor.b;
                                    if (std::sqrt(dr * dr + dg * dg + db * db) > m.senseThreshold) return;
                                    see(o.pos, 1.0f);
                                } else if (m.senseTarget == SenseTarget::Light && o.type == CellType::Luminocyte) {
                                    // A glowing light organ of another organism looks like a bright spot: light-seeking
                                    // eyes steer to it (the anglerfish's lure). Its own lights do not dazzle the eye.
                                    if (o.genome != c.genome && o.actuator > 0.05f) see(o.pos, 1.5f * o.actuator);
                                } else if (smelly) {
                                    if ((m.senseTarget == SenseTarget::Food && sec == Secretion::FoodSmell) ||
                                        (m.senseTarget == SenseTarget::CoatedFood && sec == Secretion::CoatedFoodSmell) ||
                                        (m.senseTarget == SenseTarget::Light && sec == Secretion::LightActivator) ||
                                        (m.senseTarget == SenseTarget::Wall && sec == Secretion::WallActivator))
                                        see(o.pos, 1.0f);
                                }
                            });
                            if (m.senseTarget == SenseTarget::Food || m.senseTarget == SenseTarget::CoatedFood) {
                                bool wantCoated = m.senseTarget == SenseTarget::CoatedFood;
                                forFoodNear(c.pos, kSenseRange, [&](int fi) {
                                    const Food& f = food_[fi];
                                    if ((f.coating >= 0.5f) == wantCoated) see(f.pos, 0.5f);
                                });
                            } else if (m.senseTarget == SenseTarget::Light) {
                                if (stereo) {
                                    Vec2 side = perp(fwd) * (c.radius * 2.0f);
                                    stim += (lightAt(c.pos + side) - lightAt(c.pos - side)) * 0.5f;
                                } else {
                                    stim += lightAt(c.pos) * 0.5f;
                                }
                            } else if (m.senseTarget == SenseTarget::Wall) {
                                // The dish rim and the map's barriers; walls straight ahead count most.
                                float dist = length(c.pos);
                                float gap = R - dist - c.radius;
                                if (gap < kSenseRange) {
                                    Vec2 out = dist > 1e-3f ? c.pos / dist : fwd;
                                    float w = (1.0f - std::max(gap, 0.0f) / kSenseRange) * (0.4f + 0.6f * std::max(0.0f, dot(fwd, out)));
                                    // a wall straight ahead counts as "left", so the cell turns decisively
                                    if (stereo) w *= cross(fwd, out) > -0.3f ? 1.0f : -1.0f;
                                    stim += w;
                                }
                                // Barriers within sensing range, found through the barrier grid (each counted once).
                                if (!barriers_.empty()) {
                                    int seen[64];
                                    int ns = 0;
                                    int gx0 = clampv((int)((c.pos.x - kSenseRange - barrierOrigin_.x) / barrierCell_), 0, barrierGW_ - 1);
                                    int gx1 = clampv((int)((c.pos.x + kSenseRange - barrierOrigin_.x) / barrierCell_), 0, barrierGW_ - 1);
                                    int gy0 = clampv((int)((c.pos.y - kSenseRange - barrierOrigin_.y) / barrierCell_), 0, barrierGH_ - 1);
                                    int gy1 = clampv((int)((c.pos.y + kSenseRange - barrierOrigin_.y) / barrierCell_), 0, barrierGH_ - 1);
                                    for (int gy = gy0; gy <= gy1; ++gy)
                                        for (int gx = gx0; gx <= gx1; ++gx)
                                            for (int bi : barrierGrid_[(size_t)gy * barrierGW_ + gx]) {
                                                bool dup = false;
                                                for (int q = 0; q < ns && !dup; ++q) dup = seen[q] == bi;
                                                if (dup) continue;
                                                if (ns < 64) seen[ns++] = bi;
                                                const Barrier& b = barriers_[bi];
                                                Vec2 d = closestOnSegment(c.pos, b.a, b.b) - c.pos;
                                                float l = length(d);
                                                float bg = l - b.r - c.radius;
                                                if (bg >= kSenseRange) continue;
                                                Vec2 dn = l > 1e-3f ? d / l : fwd;
                                                float w = (1.0f - std::max(bg, 0.0f) / kSenseRange) *
                                                          (0.4f + 0.6f * std::max(0.0f, dot(fwd, dn)));
                                                if (stereo) w *= cross(fwd, dn) > -0.3f ? 1.0f : -1.0f;
                                                stim += w;
                                            }
                                    stim = clampv(stim, -1.5f, 1.5f);  // a long wall is one wall, not hundreds
                                }
                            }
                            break;
                        }
                        case SenseTarget::Velocity:
                            stim = stereo ? clampv(cross(fwd, c.vel), -1.0f, 1.0f) : clampv(length(c.vel), 0.0f, 1.0f);
                            break;
                        case SenseTarget::Blood:
                            // Dissolved cell sap: a wounded or burst cell nearby. The more sap, the stronger the scent.
                            if (fluidGW_ > 0) {
                                const float ext = settings.dishRadius + kSenseRange;
                                int x0 = clampv((int)((c.pos.x - kSenseRange + ext) / fluidCell_), 0, fluidGW_ - 1);
                                int x1 = clampv((int)((c.pos.x + kSenseRange + ext) / fluidCell_), 0, fluidGW_ - 1);
                                int y0 = clampv((int)((c.pos.y - kSenseRange + ext) / fluidCell_), 0, fluidGW_ - 1);
                                int y1 = clampv((int)((c.pos.y + kSenseRange + ext) / fluidCell_), 0, fluidGW_ - 1);
                                for (int y = y0; y <= y1; ++y)
                                    for (int x = x0; x <= x1; ++x) {
                                        int bk = y * fluidGW_ + x;
                                        for (int k = fluidGridStart_[bk]; k < fluidGridStart_[bk + 1]; ++k) {
                                            const Fluid& f = fluids_[fluidGridItems_[k]];
                                            see(f.pos, std::min(1.0f, f.amount * 2.0f));
                                        }
                                    }
                            }
                            break;
                        default: break;
                    }
                    if (tgtSum > 0) {
                        if (stereo) stim += clampv(tgtSigned / tgtSum * 1.5f, -1.0f, 1.0f) * std::min(1.0f, 0.3f + 2.0f * tgtMax);
                        else stim += std::min(1.0f, tgtSum * 2.0f);
                    }
                    stim = stereo ? clampv(stim, -1.0f, 1.0f) : clampv(stim, 0.0f, 1.0f);
                    if (paralysed) stim = 0;
                    S.dSignal[i][m.senseChannel % kSignalCount] += m.senseOutput * stim;
                    a[A_Sense] = std::fabs(stim);
                    break;
                }
                case CellType::Fibroblast:
                    // Фибробласт: прочные длинные связи — обрабатывается в link pass
                    break;
                case CellType::Erythrocyte: {
                    // Эритроцит: как липоцит, но меньше запасает и имеет низкий метаболизм
                    // Обработка метаболизма в integrate pass
                    break;
                }
                case CellType::Chronocyte: {
                    // Хроноцит: таймер и импульс считаются в последовательной фазе (состояние клетки изменяется там).
                    a[A_Sense] = 1.0f;  // хроноцит светится в момент импульса (см. последовательную фазу)
                    break;
                }
                case CellType::Chromocyte: {
                    // Хромоцит: настраиваемый параметр цвета -1..1 (применяется в последовательной фазе)
                    a[A_Swim] = (std::max(-1.0f, std::min(1.0f, evalProg(m.prog[PF_Swim], in, PF_Swim))) + 1.0f) * 0.5f;
                    break;
                }
                case CellType::SignalCell:
                    // Программируемая клетка: входы собираются в последовательной фазе 4b (этот проход идёт по
                    // пулу потоков и не должен трогать общие векторы). Здесь только отмечаем её для рендера.
                    if (!scriptOf(c).empty()) a[A_Sense] = 0.35f;
                    break;
                case CellType::Granulocyte: {
                    // Гранулоцит: накопление токсичности — обрабатывается в contacts/fluids
                    break;
                }
                case CellType::Griffocyte: {
                    // Гриффоцит: «Сигнал пути 1» задаёт силу сцепления за дно.
                    a[A_Stem1] = (stunned || paralysed) ? 0.0f : std::fabs(evalProg(m.prog[PF_Stem1], in, PF_Stem1));
                    break;
                }
                case CellType::Spirocyte: {
                    // Спироцит: «Сила плавания» — команда вращения; знак задаёт направление, модуль — скорость.
                    a[A_Swim] = (stunned || paralysed) ? 0.0f : evalProg(m.prog[PF_Swim], in, PF_Swim);
                    a[A_Sense] = std::fabs(a[A_Swim]);
                    break;
                }
                case CellType::Baroreceptor: {
                    // Барорецептор: чувствует стенку чашки и выделяет сигнальное вещество (сила тем больше, чем
                    // ближе клетка к стеклу). Касание клеток обрабатывается в фазе контактов.
                    const float gap = R - length(c.pos) - c.radius;
                    if (gap < c.radius) {
                        const float press = clampv(1.0f - gap / std::max(c.radius, 1e-3f), 0.0f, 1.0f);
                        S.dSignal[i][m.senseChannel % kSignalCount] += 0.5f + press;
                        a[A_Sense] = std::max(a[A_Sense], press);
                    }
                    break;
                }
                case CellType::Cyclosporinocyte: {
                    // Циклоспориноцит: защита от пенициллина — обрабатывается в computeProtection
                    break;
                }
                default: break;
            }
        }
    });

    mark(1);
    // ---- 2. contacts ---------------------------------------------------------------
    const float searchPad = gridCell_ * 0.5f;
    jobs_->parallelFor(n, 128, [&](int b, int e) {
        std::vector<Event> localEvents;
        std::vector<EatRequest> localEats;
        std::vector<int> localSecro;
        for (int i = b; i < e; ++i) {
            const Cell& c = cells_[i];
            if (!c.alive) continue;
            const Mode& m = modeOf(c);
            Vec2 F{};
            float T = 0, dm = 0, dN = 0, dI = 0;
            auto& dS = S.dSignal[i];
            Vec2 fwd = fromAngle(c.angle);
            bool cMature = c.age >= kMinSplitAge;
            if (c.type == CellType::Secrocyte) localSecro.push_back(i);
            // Gamete chemotaxis: a mature gamete drifts toward compatible gametes it can smell nearby (sperm finds the
            // egg); the touching pair then fuses in the contact loop below.
            if (c.type == CellType::Gamete && cMature && !previewMode && !cl) {  // (Cell Lab: attraction only at contact)
                forNeighbors(c.pos, kGameteSmell, [&](int j) {
                    if (j == i) return;
                    const Cell& o = cells_[j];
                    if (!o.alive || o.type != CellType::Gamete || o.age < kMinSplitAge) return;
                    if (m.gameteCompatibleMode != o.mode && modeOf(o).gameteCompatibleMode != c.mode) return;
                    Vec2 d = o.pos - c.pos;
                    float dist = length(d);
                    if (dist < 1e-3f || dist > kGameteSmell) return;
                    F += d / dist * (kGameteAttract * c.mass * (1.0f - dist / kGameteSmell));
                });
            }

            bool ciliaCoveredL = false, ciliaCoveredR = false;  // a ciliocyte side touching another cell
            forNeighbors(c.pos, c.radius + searchPad, [&](int j) {
                if (j == i) return;
                const Cell& o = cells_[j];
                Vec2 d = o.pos - c.pos;
                // Contracting myocytes are soft: they may be squeezed by their own contraction.
                float rs = cl ? c.radius + o.radius : c.radius * S.squeeze[i] + o.radius * S.squeeze[j];
                float d2 = length2(d);
                bool gametes = c.type == CellType::Gamete && o.type == CellType::Gamete;
                // Slightly beyond contact: pairs that close in during this tick still reach the position solver.
                float reach = gametes ? std::max(rs + 8.0f, rs * 1.15f) : rs * 1.15f;
                if (d2 >= reach * reach) return;
                float dist = std::sqrt(d2);
                Vec2 nrm = dist > 1e-4f ? d / dist : fromAngle((float)i);
                {
                    uint8_t& nc = S.nbrCount[i];
                    if (nc < kMaxNbr) S.nbr[(size_t)i * kMaxNbr + nc++] = j;
                }

                if (!cl && gametes && cMature && o.age >= kMinSplitAge) {
                    bool compat = m.gameteCompatibleMode == o.mode || modeOf(o).gameteCompatibleMode == c.mode;
                    if (compat) {
                        F += nrm * (kGameteAttract * c.mass);
                        // Fuse on contact. (Requiring 45% of the radii never happened: the contact solver keeps every pair
                        // at ~93%, so gametes only piled up next to each other.)
                        if (dist < rs * 1.02f && i < j) localEvents.push_back({EventKind::Fuse, i, j, {}});
                    } else if (dist < 5.0f && i < j) {
                        localEvents.push_back({EventKind::Fuse, i, j, {}});
                    }
                }
                if (dist >= rs) return;

                // Soft-body repulsion, harder for stiffer cytoskeletons.
                float hard = 2.0f * S.hard[i] * S.hard[j] / (S.hard[i] + S.hard[j]);
                float overlap = rs - dist;
                if (cl) {
                    // Cell Lab: the force grows with the squared chord of the overlap lens (soft: cells squash 15-40 %
                    // and bounce); no damping. A small cell whose centre sinks deep into a bigger one dies (swallowed).
                    // Gametes ready to fuse: mutually compatible ones pull together at contact (-0.6), a one-way match
                    // passes through (0); any two whose centres meet fuse (equal sizes: within ~0.7 µm).
                    float gf = 1.0f;
                    if (gametes && c.age > kMinSplitAge && o.age > kMinSplitAge) {
                        const bool ab = m.gameteCompatibleMode == o.mode, ba = modeOf(o).gameteCompatibleMode == c.mode;
                        gf = ab && ba ? -0.6f : (ab || ba ? 0.0f : 1.0f);
                        if (i < j && d2 < std::fabs(c.radius * c.radius - o.radius * o.radius) + 0.45f)
                            localEvents.push_back({EventKind::Fuse, i, j, {}});
                    }
                    F -= nrm * (kClContact * overlapChord2(c.radius, o.radius, dist) * hard * gf);
                    if (o.radius > c.radius && d2 < o.radius * o.radius - c.radius * c.radius) S.dead[i] = 1;
                } else {
                    F -= nrm * (kContactK * hard * overlap);
                    F += nrm * (dot(o.vel - c.vel, nrm) * kContactDamp);
                }
                if (settings.mechanicalDamage > 0 && !previewMode) {
                    // Crushing (deep overlap) and hard impacts injure the cell; keratin armour and a stiff
                    // cytoskeleton protect. Rates are per hour, like every other injury source.
                    float crush = overlap / rs;
                    float impact = dot(c.vel - o.vel, nrm);  // closing speed, µm per tick
                    float dmg = 0;
                    // the organism's own tissue (muscles squeezing their neighbours) does not crush itself
                    if (crush > 0.3f && o.genome != c.genome) dmg += (crush - 0.3f) * 20.0f;
                    // (the organism's own tissues rubbing as it bends do not bruise each other either)
                    if (impact > 3.0f && o.genome != c.genome) dmg += (impact - 3.0f) * 0.04f / h;
                    if (dmg > 0) {
                        float armour = (c.isProtected ? 0.25f : 1.0f) / std::max(0.5f, S.hard[i]);
                        dI += dmg * armour * settings.mechanicalDamage;
                    }
                }

                // Only predators, viruses, glue, stinging cells and granulocytes care whether the pair is bonded:
                // look it up lazily.
                const bool needLink = c.type == CellType::Devorocyte || o.type == CellType::Devorocyte || c.type == CellType::Virocyte ||
                                      c.type == CellType::Glueocyte || c.type == CellType::Cnidocyte || c.type == CellType::Grappler ||
                                      c.type == CellType::Granulocyte ||
                                      (cl && (c.type == CellType::Ciliocyte || o.type == CellType::Ciliocyte));
                bool linked = false;
                if (needLink)
                    for (int k = 0; k < c.linkCount; ++k) {
                        const Link& l = links_[c.links[k]];
                        if (l.a == j || l.b == j) {
                            linked = true;
                            break;
                        }
                    }

                if (cl && !linked && (c.type == CellType::Ciliocyte || o.type == CellType::Ciliocyte)) {
                    // Cell Lab cilia work only where they touch a foreign surface: they make the two cells slide past
                    // each other at the beat speed (along the ciliocyte's axis, on the contact tangent), and damp the
                    // pressing between them.
                    Vec2 t = perp(nrm);
                    auto beat = [&](const Cell& x, int xi, Vec2 toOther) {
                        Vec2 local = rotate(toOther, -x.angle);
                        bool left = (local.y > 0) != x.mirrored;
                        return left ? S.act[xi][A_Bend] : S.act[xi][A_Lift];
                    };
                    float want = 0;  // c's speed relative to o along t
                    if (o.type == CellType::Ciliocyte) want += beat(o, j, c.pos - o.pos) * kClCiliaSlip * dot(fromAngle(o.angle), t);
                    if (c.type == CellType::Ciliocyte) want -= beat(c, i, o.pos - c.pos) * kClCiliaSlip * dot(fwd, t);
                    float mu = c.mass * o.mass / std::max(c.mass + o.mass, 1e-3f);
                    F += t * (0.25f * mu * (want - dot(c.vel - o.vel, t)));
                    F += nrm * (0.025f * mu * dot(o.vel - c.vel, nrm));
                }
                // Cilia: o's cilia drag c along o's axis; c's cilia push back on c.
                if (!cl && o.type == CellType::Ciliocyte) {
                    Vec2 local = rotate(c.pos - o.pos, -o.angle);
                    bool left = (local.y > 0) != o.mirrored;
                    float sp = left ? S.act[j][A_Bend] : S.act[j][A_Lift];
                    F += fromAngle(o.angle) * (sp * kCiliaForce);
                }
                if (!cl && c.type == CellType::Ciliocyte) {
                    Vec2 local = rotate(o.pos - c.pos, -c.angle);
                    bool left = (local.y > 0) != c.mirrored;
                    float sp = left ? S.act[i][A_Bend] : S.act[i][A_Lift];
                    F -= fwd * (sp * kCiliaForce * 0.5f);
                    // (cells ahead or behind, in the same file, do not cover a flank)
                    if (std::fabs(local.y) > 0.4f * length(local)) (left ? ciliaCoveredL : ciliaCoveredR) = true;
                }

                // Devorocytes (two devorocytes cancel; linked cells and cells of the same genome — the organism's
                // own body and its clones — are spared).
                if (cl) {
                    // Cell Lab: a devorocyte bites every unprotected cell it touches, its own body and other
                    // devorocytes included (keratinocytes are what keeps it off its neighbours).
                    if (c.type == CellType::Devorocyte && !o.isProtected) {
                        dm += kDevourRate;
                        dN += 0.5f * o.nitrogen;
                    }
                    if (o.type == CellType::Devorocyte && !c.isProtected) {
                        dm -= kDevourRate;
                        dN -= 0.5f * c.nitrogen;
                    }
                } else if (!linked && o.genome != c.genome) {
                    if (c.type == CellType::Devorocyte && o.type != CellType::Devorocyte && !o.isProtected) {
                        dm += kDevourRate;
                        dN += 0.5f * o.nitrogen;
                    }
                    if (o.type == CellType::Devorocyte && c.type != CellType::Devorocyte && !c.isProtected) {
                        dm -= kDevourRate;
                        dN -= 0.5f * c.nitrogen;
                        dI += 0.4f;  // the bite wounds: a bitten cell bleeds sap (blood that predators smell)
                        if (c.type == CellType::Virocyte) dI += 20.0f;
                    }
                }

                // Secretions from a touching secrocyte.
                if (o.type == CellType::Secrocyte && (cl ? o.nitrogen >= 0.2f : o.nitrogen > 0.22f)) {
                    Secretion sec = modeOf(o).secretion;
                    bool sameKind = c.type == CellType::Secrocyte && m.secretion == sec;
                    switch (sec) {
                        case Secretion::HCN:
                            if (!c.isProtected && !sameKind) dI += 0.8f;
                            break;
                        case Secretion::Lipase:
                            if (c.type != CellType::Keratinocyte && !sameKind) dI += cl ? 0.1f : 0.25f;
                            break;
                        default: {
                            float sign = 0;
                            int ch = secretionSignal(sec, &sign);
                            // (Cell Lab: saturates the signal at once; lipocytes take no signal)
                            if (ch >= 0 && !(cl && c.type == CellType::Lipocyte)) dS[ch] += (cl ? 240.0f : 8.0f) * sign;
                            break;
                        }
                    }
                }

                // Барорецептор: почувствовал касание (клетка или стенка) — выделяет сигнальное вещество.
                if (c.type == CellType::Baroreceptor)
                    dS[m.senseChannel % kSignalCount] += 0.5f + 0.5f * (1.0f - dist / std::max(rs, 1e-3f));

                // Гранулоцит: вытягивает яд из соседей, с которыми связан адгезином, в себя. Накопление идёт
                // через S.dToxic — состояние клетки правится в последовательной фазе.
                if (c.type == CellType::Granulocyte && linked && o.injury > 0.1f)
                    S.dToxic[i] += o.injury * 0.01f * h * kSignalSpeed;

                // Cnidocyte: a loaded, ready stinging cell fires its thread into a foreign cell it touches.
                if (c.type == CellType::Cnidocyte && c.charge >= 1.0f && S.act[i][A_Swim] >= 0.3f && !linked && !previewMode &&
                    o.genome != c.genome && o.type != CellType::Cnidocyte)
                    localEvents.push_back({EventKind::Sting, i, j, {}});

                // Cell Lab virocyte: infects at random, 5 times per hour per contact, any cell not under keratin (its own
                // body and linked cells too), with no pause after an infection.
                if (cl && c.type == CellType::Virocyte && !o.isProtected && hash01((uint32_t)i, (uint32_t)j, ticks_) < 5.0f * h)
                    localEvents.push_back({EventKind::Infect, i, j, {}});
                if (!linked && !previewMode) {
                    if (!cl && c.type == CellType::Virocyte && c.virusCooldown <= 0 && !o.isProtected &&
                        o.type != CellType::Devorocyte && o.genome != c.genome)
                        localEvents.push_back({EventKind::Infect, i, j, {}});
                    if (c.type == CellType::Glueocyte && c.linkCount < kGlueMaxLinks && !o.isProtected &&
                        (!cl || (c.age >= 0.1f && o.age >= 0.1f)) &&  // (Cell Lab: both at least 0.1 h old)
                        o.linkCount < kMaxLinks && i < j + (o.type == CellType::Glueocyte ? 0 : n))
                        localEvents.push_back({EventKind::Glue, i, j, {}});
                    // Grappler: a closed grip catches any foreign cell it touches (keratin is too smooth to hold).
                    if (c.type == CellType::Grappler && S.act[i][A_Swim] >= 0.3f && o.genome != c.genome &&
                        o.type != CellType::Keratinocyte && o.linkCount < kMaxLinks)
                        localEvents.push_back({EventKind::Grip, i, j, {}});
                }
            });

            // Cilia on a free side (no cell to push) row against the water instead: the ciliocyte itself is driven
            // along its axis. The outer cilia of a ciliate are its oars — beating harder on one flank turns the body.
            if (c.type == CellType::Ciliocyte && !previewMode && !cl) {  // (Cell Lab cilia do not row in open water)
                if (!ciliaCoveredL) F += fwd * (S.act[i][A_Bend] * kCiliaWater);
                if (!ciliaCoveredR) F += fwd * (S.act[i][A_Lift] * kCiliaWater);
            }

            // Dish wall.
            float pd = length(c.pos);
            if (cl) {
                // Cell Lab: a stiff wall pushing on the chord of the cell's part beyond it, no damping; with "kill at
                // edge" no wall at all. A cell whose centre leaves the dish is gone, without leaving food.
                if (pd + c.radius > R) {
                    Vec2 nrm = c.pos / std::max(pd, 1e-4f);
                    if (!settings.killAtEdge) F -= nrm * (2.0f * kClContact * overlapChord2(c.radius, R, std::min(pd, R + c.radius - 1e-3f)) * S.hard[i]);
                    if (c.type == CellType::Glueocyte && c.glueWallLinks == 0 && c.linkCount < kGlueMaxLinks && !settings.killAtEdge)
                        localEvents.push_back({EventKind::GlueWall, i, -1, nrm * (R + 2.683f)});  // (just beyond the glass)
                    if (c.type == CellType::Ciliocyte && !settings.killAtEdge) {  // cilia slide the cell along the glass
                        Vec2 t = perp(nrm);
                        Vec2 local = rotate(nrm, -c.angle);
                        bool left = (local.y > 0) != c.mirrored;
                        float want = -(left ? S.act[i][A_Bend] : S.act[i][A_Lift]) * kClCiliaSlip * dot(fwd, t);
                        F += t * (0.25f * c.mass * (want - dot(c.vel, t)));
                    }
                }
                if (pd > R) localEvents.push_back({EventKind::Remove, i, -1, {}});
            } else if (settings.lid || previewMode) {
                float over = pd + c.radius - R;
                if (over > 0) {
                    Vec2 nrm = c.pos / std::max(pd, 1e-4f);
                    F -= nrm * (kWallK * over);
                    F -= nrm * (std::max(dot(c.vel, nrm), 0.0f) * 0.2f);
                    if (!previewMode) {
                        if (settings.killAtEdge) localEvents.push_back({EventKind::Kill, i, -1, {}});
                        else if (c.type == CellType::Glueocyte && c.glueWallLinks == 0 && c.linkCount < kGlueMaxLinks)
                            localEvents.push_back({EventKind::GlueWall, i, -1, nrm * R});
                        else if (c.type == CellType::Grappler && c.glueWallLinks == 0 && S.act[i][A_Swim] >= 0.8f)  // strong grip
                            localEvents.push_back({EventKind::GripWall, i, -1, nrm * R});
                    }
                }
            } else if (pd > R + c.radius) {
                localEvents.push_back({EventKind::Remove, i, -1, {}});
            }

            // Phagocytes eat uncoated food they touch.
            if (c.type == CellType::Phagocyte && !previewMode) {
                forFoodNear(c.pos, c.radius + 4.0f, [&](int fi) {
                    const Food& f = food_[fi];
                    if (f.coating > 0.01f) return;
                    float rr = c.radius + (cl ? 3.5f : foodRadius(f.mass));  // Cell Lab: a fixed reach
                    if (length2(f.pos - c.pos) < rr * rr) localEats.push_back({i, fi});
                });
            }

            S.force[i] = F;
            S.torque[i] = T;
            S.dMass[i] = dm;
            S.dNitro[i] = dN;
            S.dInjury[i] = dI;
        }
        if (!localEvents.empty() || !localEats.empty() || !localSecro.empty()) {
            std::lock_guard<std::mutex> lk(S.m);
            S.events.insert(S.events.end(), localEvents.begin(), localEvents.end());
            S.eats.insert(S.eats.end(), localEats.begin(), localEats.end());
            S.secrocytes.insert(S.secrocytes.end(), localSecro.begin(), localSecro.end());
        }
    });

    mark(2);
    // ---- 3. feeding (serial, cheap) ------------------------------------------------
    {
        const float bite = kEatRate * h;
        for (const EatRequest& er : S.eats) {
            Food& f = food_[er.food];
            // A crumb is swallowed gradually, not in a tick or two: each bite takes at most 6 % of what is left (and
            // a small floor), so food visibly shrinks against the cell before it is gone. Cell Lab mode keeps its rate.
            // Амилаза: фагоцит переваривает пищу быстрее (bite × (1 + amylase)).
            float cellBite = bite;
            if (!cl) {
                const Cell& cc = cells_[er.cell];
                if (cc.amylase > 0.0f && cc.type == CellType::Phagocyte) cellBite = bite * (1.0f + cc.amylase);
            }
            float take = std::min(cl ? cellBite : std::min(cellBite, std::max(0.06f * f.mass, 0.002f)), f.mass);
            if (take <= 0) continue;
            f.mass -= take;
            S.eaten[er.cell] += take;
        }
    }

    mark(3);
    // Cell Lab adhesin: a spring between anchor points on the two membranes (not between the centres) that pulls at
    // the anchors' midpoint, so it also turns the cells; an angular term from the stiffness (0..35 in the game); and
    // nutrient, nitrogen and signal exchange at the game's rates. Each cell gets the mirror image of the other's force.
    auto clLink = [&](Link& l, int li) {
        const Cell& A = cells_[l.a];
        l.maxLen = 0;
        l.massFlow = l.nitroFlow = 0;
        l.fzA = 0;
        std::fill(std::begin(l.sigFlow), std::end(l.sigFlow), 0.0f);
        if (l.b < 0) {  // glued to the wall: anchor at 0.6 r
            Vec2 o = fromAngle(A.angle + l.angA) * (0.6f * A.radius);
            Vec2 d = l.wallPoint - (A.pos + o);
            Vec2 va = A.vel + perp(o) * A.angVel;
            l.fA = d * kClWallLinkK - va * (0.0025f * A.radius);
            l.tA = cross(o, l.fA);
            l.tB = 0;
            if (length2(d) > 4.0f * A.radius * A.radius) S.brokenLink[li] = 1;
            return;
        }
        const Cell& B = cells_[l.b];
        const float minr = std::min(A.radius, B.radius);
        // Anchors sit 0.4 of the smaller radius inside the membranes; a contracting muscle pulls its front and rear
        // anchors deeper (a shorter bond), a bending one swings them.
        auto anchor = [&](const Cell& M, int idx, float angle) {
            float depth = 0.4f * minr, turn = 0;
            if (M.type == CellType::Myocyte) {
                const auto& a = S.act[idx];
                float ca = std::cos(angle);
                depth += M.radius * ca * ca * a[A_Contract];
                turn = ca * a[A_Bend] * (M.mirrored ? -1.0f : 1.0f);
            }
            depth = clampv(depth, 0.0f, minr);
            return fromAngle(M.angle + angle + turn) * (M.radius - depth + 0.5f * l.extraLength);  // half the adhesin each side
        };
        const Vec2 oA = anchor(A, l.a, l.angA), oB = anchor(B, l.b, l.angB);
        const Vec2 d = (B.pos + oB) - (A.pos + oA);
        const float d2 = length2(d);
        const float k = kClLinkK * kClLinkG / (l.extraLength + kClLinkG) * (1.0f + 2.0f * d2 / 900.0f);
        const Vec2 vA = A.vel + perp(oA) * A.angVel, vB = B.vel + perp(oB) * B.angVel;
        const float mu = A.mass * B.mass / std::max(A.mass + B.mass, 1e-3f);
        const int nmax = std::max(1, (int)std::max(A.linkCount, B.linkCount));
        const Vec2 fA = d * k + (vB - vA) * (kClLinkDamp * mu / nmax);
        const Vec2 mid = (A.pos + oA + B.pos + oB) * 0.5f;
        const float cr = oA.y * oB.x - oA.x * oB.y;  // 0 when the anchors face each other
        const float ang = kClAngular * (35.0f * l.stiffness) * minr * (cr >= 0 ? 1.0f : -1.0f) * std::sqrt(std::fabs(cr));
        l.fA = fA;
        l.tA = cross(mid - A.pos, fA) + ang;
        l.tB = cross(mid - B.pos, -fA) - ang;
        // Height: linked cells pull each other to the same lift; a muscle's lift value raises its front and lowers its back.
        l.fzA = (B.lift - A.lift) - (A.type == CellType::Myocyte ? std::cos(l.angA) * S.act[l.a][A_Lift] : 0.0f) +
                (B.type == CellType::Myocyte ? std::cos(l.angB) * S.act[l.b][A_Lift] : 0.0f);
        if (d2 > 4.0f * minr * minr) {  // the anchors drifted apart: the bond snaps
            S.brokenLink[li] = 1;
            l.fzA = 0;
        }

        // Exchange. Mass (in the game's 10 ng unit) flows toward the hungrier side by priority, at most 1 ng/h; a grown
        // end cell gives everything; "prioritize" holds on to a starving cell's mass; a lipocyte counts its fat as a sixth.
        const Mode& mA = modeOf(A);
        const Mode& mB = modeOf(B);
        auto eff = [](const Cell& c) { return c.type == CellType::Lipocyte ? c.mass / 60.0f : c.mass / 10.0f; };
        auto prio = [&](const Cell& c, const Mode& m) {
            float p = m.nutrientPriority;
            if (m.splitMass >= kNeverSplit && eff(c) > 0.3456f) p = 0;
            if (m.prioritize && eff(c) < 0.0936f) p += (0.0936f - eff(c)) * 462.96f;
            return p;
        };
        l.massFlow = 10.0f * clampv(eff(A) * prio(B, mB) - eff(B) * prio(A, mA), -0.1f, 0.1f) * h;
        const float nA = A.type == CellType::Nitrocyte ? 1.0f : A.nitrogen, nB = B.type == CellType::Nitrocyte ? 1.0f : B.nitrogen;
        l.nitroFlow = clampv(nA - nB, -0.6f, 0.6f) * h;
        if (A.type != CellType::Lipocyte && B.type != CellType::Lipocyte)
            for (int s = 0; s < kSignalCount; ++s) l.sigFlow[s] = 5.0f * (A.signals[s] - B.signals[s]) * h;
    };

    // ---- 4. links --------------------------------------------------------------------
    jobs_->parallelFor((int)links_.size(), 256, [&](int b, int e) {
        for (int li = b; li < e; ++li) {
            Link& l = links_[li];
            if (!l.alive) continue;
            if (cl) {
                clLink(l, li);
                continue;
            }
            const Cell& A = cells_[l.a];
            float IA = 0.5f * A.mass * A.radius * A.radius;
            float kTheta = kAngBase + kAngStiff * l.stiffness;
            if (l.b < 0) {  // wall glue
                Vec2 d = l.wallPoint - A.pos;
                float dist = std::max(length(d), 1e-4f);
                Vec2 nrm = d / dist;
                l.fA = nrm * (kLinkK * (dist - A.radius)) - A.vel * 0.05f;
                float errA = wrapAngle(std::atan2(d.y, d.x) - (A.angle + l.angA));
                l.tA = (kTheta * errA - kAngDamp * A.angVel) * IA;
                l.tB = 0;
                l.massFlow = l.nitroFlow = 0;
                std::fill(std::begin(l.sigFlow), std::end(l.sigFlow), 0.0f);
                if (dist > A.radius * 3.0f + 20.0f) S.brokenLink[li] = 1;
                l.maxLen = 0;
                continue;
            }
            const Cell& B = cells_[l.b];
            float IB = 0.5f * B.mass * B.radius * B.radius;
            Vec2 d = B.pos - A.pos;
            float dist = std::max(length(d), 1e-4f);
            Vec2 nrm = d / dist;
            // Эритроцит: связи ослабленные — пружина мягче, а угловой сцеп слабее, поэтому он легко отрывается.
            float linkScale = (A.type == CellType::Erythrocyte || B.type == CellType::Erythrocyte) ? 0.35f : 1.0f;
            float phi = std::atan2(nrm.y, nrm.x);

            // Myocyte modulation of rest length and anchor targets.
            // Bonded cells press into each other a little (tissue looks packed, membranes flatten at the seams).
            float rest = (A.radius + B.radius) * kBondRest + l.extraLength;
            float offA = 0, offB = 0;
            float springK = kLinkK;
            auto myo = [&](const Cell& M, int idx, float anchor, float& off) {
                if (M.type != CellType::Myocyte) return;
                float ca = std::cos(anchor);
                if (std::fabs(ca) < 0.5f) return;  // only front / rear links
                const auto& a = S.act[idx];
                // Muscle fibres pull hard but push weakly: a negative command lengthens the fibre by at most ~20 %
                // (it used to stretch it by half, tearing gaps into the body); 0.45 of a negative command still lets
                // the bending wave swim.
                const float con = a[A_Contract];
                constexpr float extK = 0.45f;
                rest *= 1.0f - (con > 0.0f ? con : extK * con);
                // An active muscle fibre is much stiffer than passive adhesin, so it can bend a whole body.
                springK += kLinkK * kMuscleGain * std::min(1.0f, 4.0f * std::fabs(a[A_Contract]));
                float sgn = M.mirrored ? -1.0f : 1.0f;
                off += sgn * (ca > 0 ? 0.5f * (a[A_Bend] + a[A_Lift]) : 0.5f * (-a[A_Bend] + a[A_Lift]));
            };
            myo(A, l.a, l.angA, offA);
            myo(B, l.b, l.angB, offB);
            // Tendon: a bending myocyte also bends the non-muscle cell it pulls on along the body, so a chain that
            // alternates muscles and nerve cells curves as a whole instead of zigzagging around stiff nerve cells.
            if (A.type == CellType::Myocyte && B.type != CellType::Myocyte && std::fabs(std::cos(l.angA)) >= 0.5f &&
                std::fabs(std::cos(l.angB)) >= 0.5f)
                offB -= offA * (A.mirrored != B.mirrored ? -1.0f : 1.0f);
            else if (B.type == CellType::Myocyte && A.type != CellType::Myocyte && std::fabs(std::cos(l.angB)) >= 0.5f &&
                     std::fabs(std::cos(l.angA)) >= 0.5f)
                offA -= offB * (A.mirrored != B.mirrored ? -1.0f : 1.0f);
            // Two contracting muscles in a row multiply their shortening: a bond could ask for a quarter of the touching
            // distance while the contact solver pushed the cells apart again, and muscular tissue buzzed. A bond never
            // pulls its cells closer than 60 % of touching.
            rest = std::max(rest, (A.radius + B.radius) * 0.6f);
            // Фибробласт: волокна очень прочные и длинные — жёстче пружина и больше длина покоя.
            const bool fibroA = A.type == CellType::Fibroblast, fibroB = B.type == CellType::Fibroblast;
            if (fibroA || fibroB) {
                springK *= 2.5f;
                rest += (A.radius + B.radius) * 0.25f;
            }
            // Гиалуроновая кислота (режимы 0 и 2) усиливает связь и увеличивает радиус её действия.
            if (settings.hyaluronicMode == 0 || settings.hyaluronicMode == 2) {
                const float hy = std::max(A.hyaluronicAcid, B.hyaluronicAcid);
                if (hy > 0.0f) {
                    springK *= 1.0f + hy * 1.5f;
                    rest += (A.radius + B.radius) * 0.2f * kHyaluronicRange * std::min(hy, 1.0f);
                }
            }
            springK = std::min(springK, kLinkK + 0.25f * std::min(A.mass, B.mass));  // explicit-step stability

            float stretch = dist - rest;
            // Beyond this the bond acts as a rope (see 5c): shear from cilia or muscles can no longer pull visible gaps
            // into a body.
            l.maxLen = std::max(rest * 1.12f, (A.radius + B.radius) * kRopeLen);
            Vec2 fA = nrm * ((springK * stretch + dot(B.vel - A.vel, nrm) * kLinkDamp) * linkScale);

            float omegaBond = cross(nrm, B.vel - A.vel) / dist;
            float errA = wrapAngle(phi - (A.angle + l.angA + offA));
            float errB = wrapAngle(phi + kPi - (B.angle + l.angB + offB));
            // A cell shares its angular hold among its bonds: summed over 8-9 bonds of a stiff tissue the angular springs
            // passed the stability limit of the explicit step, and those cells shook back and forth every tick.
            const float shareA = 3.0f / std::max(3, (int)A.linkCount), shareB = 3.0f / std::max(3, (int)B.linkCount);
            float tA = (kTheta * shareA * errA - kAngDamp * shareA * (A.angVel - omegaBond)) * IA * linkScale;
            float tB = (kTheta * shareB * errB - kAngDamp * shareB * (B.angVel - omegaBond)) * IB * linkScale;
            // Reaction couple: the bond rotates opposite to the torques applied to the cells.
            fA += perp(nrm) * ((tA + tB) / dist);
            l.fA = fA;
            l.tA = tA;
            l.tB = tB;
            // Эритроцит рвётся легче остальных: его ослабленную связь достаточно немного растянуть.
            const float breakLimit = (A.radius + B.radius) * (linkScale < 1.0f ? 1.0f : 1.2f) + (linkScale < 1.0f ? 2.0f : 12.0f);
            if (stretch > breakLimit) S.brokenLink[li] = 1;

            // Nutrient flow equalises (fill / priority).
            const Mode& mA = modeOf(A);
            const Mode& mB = modeOf(B);
            auto prio = [](const Cell& c, const Mode& m) {
                float p = m.nutrientPriority;
                if (m.prioritize && c.mass < kDeathMass * 1.5f) p *= 10.0f;
                return p;
            };
            float qA = A.mass / capacityFor(A) / prio(A, mA);
            float qB = B.mass / capacityFor(B) / prio(B, mB);
            float flow = kFlowRate * (qA - qB) * h;
            flow = clampv(flow, -0.2f * B.mass, 0.2f * A.mass);
            // Хондроитинсульфат всегда затрудняет передачу любых веществ через адгезин. Гиалуроновая кислота
            // по режиму (Settings::hyaluronicMode): 0 — только усиливает связь (см. rest/springK выше),
            // 1 — только затрудняет перенос, 2 — и то и другое сразу.
            const float hyaluron = std::max(A.hyaluronicAcid, B.hyaluronicAcid);
            const float chondro = std::max(A.chondroitinSulfate, B.chondroitinSulfate);
            const float blocking = chondro + ((settings.hyaluronicMode == 1 || settings.hyaluronicMode == 2) ? hyaluron : 0.0f);
            const float transportScale = 1.0f / (1.0f + kChondroitinBlock * blocking);
            l.massFlow = previewMode ? 0.0f : flow * transportScale;
            float nA = A.type == CellType::Nitrocyte ? 1.0f : A.nitrogen;
            float nB = B.type == CellType::Nitrocyte ? 1.0f : B.nitrogen;
            l.nitroFlow = kNitroFlow * (nA - nB) * h * transportScale;
            // Липоцит сигналы не принимает. Эритроцит, наоборот, переносит все вещества, кроме сигналов:
            // массу и азот он проводит, сигнальные каналы заблокированы.
            bool blocked = A.type == CellType::Lipocyte || B.type == CellType::Lipocyte ||
                           A.type == CellType::Erythrocyte || B.type == CellType::Erythrocyte;
            for (int s = 0; s < kSignalCount; ++s)
                l.sigFlow[s] = blocked ? 0.0f : kSignalDiffusion * (A.signals[s] - B.signals[s]) * h * kSignalSpeed * transportScale;
        }
    });

    mark(4);
    // Cell Lab motion: flagellum thrust, gravity on buoyocytes only, linear drag on the radius (cells coast, accelerate
    // slowly and swing on turns), Coulomb friction, no speed limit (a numerical guard only); moment of inertia 0.7 m r².
    auto clMove = [&](Cell& c, int i, Vec2 F, float T, float fz) {
        const auto& a = S.act[i];
        if (c.type == CellType::Flagellocyte) F += fromAngle(c.angle) * (std::max(0.0f, a[A_Swim]) * kClSwim);
        if (settings.gravity > 0) {
            float rho = c.type == CellType::Buoyocyte ? a[A_Density] : 0.0f;
            float rhoSub = settings.density + settings.densityGradient * (c.pos.y / R);
            F.y -= settings.gravity * kClGravity * c.mass * (rho - rhoSub);
        }
        const float m = std::max(c.mass, 0.05f);
        if (i == dragIdx_) {  // the tweezers, as in our physics
            Vec2 desired = (dragTarget_ - c.pos) * 0.25f;
            float dl = length(desired);
            if (dl > 25.0f) desired = desired * (25.0f / dl);
            F += (desired - c.vel) * (0.9f * m);
        }
        const float gamma = kClDrag * settings.viscosity * c.radius;
        Vec2 v = (c.vel + F / m) / (1.0f + gamma / m);
        // Height off the substrate (overdamped, pulled toward the linked cells' and the muscles' lift): a lifted segment
        // presses less on the glass, so the friction drops — worms crawl by lifting parts of the body.
        c.lift += 10.0f * h * (-0.3f * c.lift + fz);
        const float N = clampv(1.0f - 3.0f * c.lift, 0.0f, 4.0f);
        const float dec = settings.dynFriction * kClFriction * N;
        const float stick = settings.staticFriction * kClFriction * (c.mass / 10.0f) * N;
        const float speed = length(v);
        if (c.vel.x == 0 && c.vel.y == 0 && length(F) / m <= stick) v = {};  // stuck stays stuck
        else if (speed > 0) v *= std::max(0.0f, speed - dec) / speed;
        const float sp2 = length2(v);
        if (sp2 > 3600.0f) v = v * (60.0f / std::sqrt(sp2));  // numerical guard only (60 µm per tick)
        if (!(v.x == v.x) || !(v.y == v.y)) v = {};
        c.vel = v;
        c.prevPos = c.pos;
        c.prevAngle = c.angle;
        c.pos += v;
        const float I = std::max(0.7f * m * c.radius * c.radius, 1e-3f);
        const float gRot = kClRotDrag * settings.viscosity * c.radius * c.radius * c.radius;
        c.angVel = (c.angVel + T / I) / (1.0f + gRot / I);
        if (!(c.angVel == c.angVel)) c.angVel = 0;
        c.angVel = clampv(c.angVel, -1.0f, 1.0f);
        c.angle = wrapAngle(c.angle + c.angVel);
    };

    // ---- 4b. programmable signal cells (dypt) ---------------------------------------
    // The player's programs run here: after the sensors have written this tick's intents and before the signals are
    // distributed, so a program sees one consistent snapshot and its outputs land in the tick they were computed in.
    // Serial on purpose: the program registry and the dypt scripts are not thread safe (a dypt environment lives in
    // Rc and belongs to the thread that opened it). Because every cell reads the previous tick's signals and writes
    // into dSignal, the order cells are visited in cannot change the result.
    if (signalCellsEnabled && signalCells_.ready()) {
        bool anySignalCell = false;
        for (int i = 0; i < n; ++i) {
            if (!cells_[i].alive || cells_[i].type != CellType::SignalCell) continue;
            if (scriptOf(cells_[i]).empty()) continue;
            anySignalCell = true;
            break;
        }
        if (anySignalCell) {
            S.signalInputs.clear();
            S.signalRuns.clear();
            S.signalInputs.reserve((size_t)n * SignalCellInputs::kCount);
            std::vector<std::pair<int, int>> liveKeys;
            for (int i = 0; i < n; ++i) {
                const Cell& c = cells_[i];
                if (!c.alive || c.type != CellType::SignalCell) continue;
                if (scriptOf(c).empty()) continue;
                const int key = signalCells_.programFor(c.genome, c.mode, scriptOf(c));
                if (key < 0) continue;  // лимит числа живых программ
                const size_t base = S.signalInputs.size();
                S.signalInputs.resize(base + SignalCellInputs::kCount);
                float* dst = S.signalInputs.data() + base;
                for (int s = 0; s < kSignalCount; ++s) dst[s] = c.signals[s];
                dst[SignalCellInputs::kMass] = c.mass / kMaxCellMass;
                dst[SignalCellInputs::kAge] = c.age / kMaxAge;
                dst[SignalCellInputs::kNitrogen] = c.nitrogen;
                dst[SignalCellInputs::kLinks] = c.linkCount / (float)kMaxLinks;
                dst[SignalCellInputs::kLight] = lightAt(c.pos);
                {  // food nearby, as the programmable inputs of other cells read it (it was always 0 here)
                    float sum = 0;
                    forFoodNear(c.pos, kFoodReach, [&](int fi) {
                        float d = length(food_[fi].pos - c.pos);
                        if (d < kFoodReach) sum += food_[fi].mass * (1.0f - d / kFoodReach);
                    });
                    dst[SignalCellInputs::kFood] = std::min(1.0f, sum / 3.0f);
                }
                dst[SignalCellInputs::kClock] = (float)(ticks_ % 5040u);  // (5040: divisible by most periods one picks)
                dst[SignalCellInputs::kInjury] = c.injury;
                for (int k = 0; k < kSignalCellMemory; ++k) dst[SignalCellInputs::kMemory + k] = c.progMem[k];
                S.signalRuns.push_back({i, key, dst, cl});
                liveKeys.emplace_back(c.genome, c.mode);
            }
            signalCells_.dropUnused(liveKeys);
            // The answers set levels (and memory) at the end of the tick, after the signals have spread: every
            // program saw tick N, all of them act together.
            if (signalCells_.dueThisTick()) {
                signalCellErrors = signalCells_.runTick(S.signalRuns, S.signalResults);
                signalCellOps += S.signalResults.size();
            }
        }
    }

    // ---- 5. integrate + metabolism ---------------------------------------------------
    jobs_->parallelFor(n, 256, [&](int b, int e) {
        for (int i = b; i < e; ++i) {
            Cell& c = cells_[i];
            if (!c.alive) continue;
            const Mode& m = modeOf(c);
            const auto& a = S.act[i];
            Vec2 F = S.force[i];
            float T = S.torque[i];
            float linkMass = 0, linkN = 0, fzLinks = 0;
            float linkSig[kSignalCount] = {};
            for (int k = 0; k < c.linkCount; ++k) {
                const Link& l = links_[c.links[k]];
                if (l.a == i) {
                    F += l.fA;
                    T += l.tA;
                    fzLinks += l.fzA;
                    linkMass -= l.massFlow;
                    linkN -= l.nitroFlow;
                    for (int s = 0; s < kSignalCount; ++s) linkSig[s] -= l.sigFlow[s];
                } else {
                    F -= l.fA;
                    T += l.tB;
                    fzLinks -= l.fzA;
                    linkMass += l.massFlow;
                    linkN += l.nitroFlow;
                    for (int s = 0; s < kSignalCount; ++s) linkSig[s] += l.sigFlow[s];
                }
            }

            Vec2 fwd = fromAngle(c.angle);
            if (cl) {
                clMove(c, i, F, T, fzLinks);
            } else {
                // The genome preview develops the organism in place, so it stays in frame.
                if (c.type == CellType::Flagellocyte && !previewMode) F += fwd * (a[A_Swim] * kSwimForce);
                if (settings.gravity > 0 && !previewMode) {
                    float rho = c.type == CellType::Buoyocyte ? a[A_Density] : 1.0f;
                    float rhoSub = settings.density + settings.densityGradient * (c.pos.y / R);
                    F.y -= settings.gravity * kGravity * c.mass * (rho - rhoSub);
                }
                if (i == dragIdx_) {
                    // Critically damped pull toward the cursor: the cell glides instead of oscillating.
                    Vec2 desired = (dragTarget_ - c.pos) * 0.25f;
                    float maxStep = 25.0f;
                    float dl = length(desired);
                    if (dl > maxStep) desired = desired * (maxStep / dl);
                    F += (desired - c.vel) * (0.9f * std::max(c.mass, 0.3f));
                }
    
                // Implicit Stokes drag + Coulomb-style friction against the substrate.
                float mass = std::max(c.mass, 0.3f);
                float gamma = (0.05f + settings.viscosity * 0.004f) * (c.radius / 10.0f);
                float fricMul = c.type == CellType::Ciliocyte ? 4.0f : 1.0f;
                // Currents: drag and friction act on the motion relative to the water, so a cell drifts with it.
                const Vec2 water = hasFlow() && !previewMode ? flowAt(c.pos) : Vec2{};
                Vec2 v;
                float dec = settings.dynFriction * 0.006f * fricMul;
                if (c.linkCount >= 2 && settings.swimAnisotropy > 1.01f) {
                    // Anisotropic Stokes drag in the cell's frame (resistive force theory): sliding along the body
                    // axis is cheap, moving sideways is expensive. A backward-travelling bending wave therefore
                    // produces forward thrust. Friction against the substrate only resists sideways motion (a nematode
                    // crawling on agar); axial dry friction would cancel the small forward drift of each stroke.
                    Vec2 ax = fromAngle(c.angle), sd = perp(ax);
                    Vec2 p = c.vel - water + F / mass;
                    float gPar = gamma * 0.7f, gPerp = gamma * 0.7f * settings.swimAnisotropy;
                    float vPar = dot(p, ax) / (1.0f + gPar / mass);
                    float vPerp = dot(p, sd) / (1.0f + gPerp / mass);
                    auto coulomb = [](float x, float d) { return x > d ? x - d : (x < -d ? x + d : 0.0f); };
                    vPerp = coulomb(vPerp, dec * 0.5f * settings.swimAnisotropy);  // nothing resists sliding along
                    v = ax * vPar + sd * vPerp + water;
                } else {
                    v = (c.vel - water + F / mass) / (1.0f + gamma / mass);
                    float speed = length(v);
                    float fStatic = settings.staticFriction * 0.004f * fricMul;
                    if (speed < 0.02f && length(F) / mass < fStatic) {
                        v = {};
                    } else if (speed > 0) {
                        v *= std::max(0.0f, speed - dec) / speed;
                    }
                    v += water;
                }
                // Hard speed limit: a crowded or badly configured dish can never blow up numerically.
                float sp2 = length2(v);
                if (sp2 > kMaxSpeed * kMaxSpeed) v = v * (kMaxSpeed / std::sqrt(sp2));
                if (!(v.x == v.x) || !(v.y == v.y)) v = {};
                c.vel = v;
                c.prevPos = c.pos;
                c.prevAngle = c.angle;
                c.pos += v;
    
                float I = 0.5f * mass * c.radius * c.radius;
                float gammaRot = gamma * c.radius * c.radius * 0.5f;
                c.angVel = (c.angVel + T / I) / (1.0f + gammaRot / I);
                c.angVel = clampv(c.angVel, -0.5f, 0.5f);
                c.angle = wrapAngle(c.angle + c.angVel);
            }

            // Biology -----------------------------------------------------------------
            float cap = capacityFor(c);
            if (previewMode) {
                c.mass = std::min(cap, c.mass + kPreviewGrowth * h);
                c.nitrogen = 1.0f;
            } else {
                float gain = S.eaten[i];
                if (cl) {
                    // Cell Lab: photosynthesis 1 ng/h per ng at light 1 (its light field, no shading); a basal cost that
                    // grows with the cell's size, per type; flagella and cilia pay for their beat.
                    if (c.type == CellType::Photocyte) gain += lightAt(c.pos) * std::min(c.mass, kMaxCellMass) * h;
                    const float salF = std::max(0.0f, 1.3857f - 1.2857f * settings.salinity);
                    float cost = 10.0f * clTypeCost(c.type) * salF * (std::min(c.mass, kMaxCellMass) / 10.0f + 3.0f * c.radius / 500.0f);
                    if (c.type == CellType::Flagellocyte) {
                        float sw = std::max(0.0f, a[A_Swim]);
                        cost += 10.0f * sw * (0.008f + 0.0409756f * sw);
                    }
                    if (c.type == CellType::Ciliocyte) cost += 0.01f * (std::fabs(a[A_Bend]) + std::fabs(a[A_Lift]));
                    if ((c.type == CellType::Electrocyte || c.type == CellType::Cnidocyte) && c.charge < 1.0f)
                        c.charge = std::min(1.0f, c.charge + (c.type == CellType::Electrocyte ? kElectrocyteCharge : kCnidoRecharge) * h);
                    c.mass += gain - cost * h + S.dMass[i] * h + linkMass;
                    if (c.mass > cap) c.mass = cap;
                    if (c.type == CellType::Electrocyte && c.charge >= 1.0f && a[A_Swim] > 0.5f) S.discharge[i] = 1;
                    // Nitrogen is taken up only up to the nitrate level of the water.
                    if (c.type == CellType::Nitrocyte) c.nitrogen = 1.0f;
                    else {
                        if (c.nitrogen < settings.nitrates) c.nitrogen += 1.2f * (settings.nitrates - c.nitrogen) * h;
                        c.nitrogen = clampv(c.nitrogen + linkN + S.dNitro[i] * h, 0.0f, 1.0f);
                        // A secrocyte spends nitrogen while it secretes, down to a quarter.
                        if (c.type == CellType::Secrocyte && c.nitrogen > 0.25f) c.nitrogen = std::max(0.25f, c.nitrogen - 0.45f * h);
                    }
                    c.injury = std::max(0.0f, c.injury + S.dInjury[i] * h - kInjuryRecovery * c.injury * h);
                } else {
                if (c.type == CellType::Photocyte) {
                    // Self-shading: inside a dense mat of photosynthesising cells each one gets less light, so algae
                    // carpets stop growing instead of covering the whole dish (filaments and loose cells are unaffected).
                    int greens = 0;
                    const int* nb = &S.nbr[(size_t)i * kMaxNbr];
                    for (int k = 0, nc = S.nbrCount[i]; k < nc; ++k) greens += cells_[nb[k]].type == CellType::Photocyte;
                    float shade = lightShare / (1.0f + 0.3f * std::max(0, greens - 2));
                    // Эпибрассинолид усиливает фотосинтез (до +100 %).
                    const float photoBoost = 1.0f + c.epibrassinolide;
                    gain += kPhotoRate * photoBoost * lightAt(c.pos) * shade * std::min(c.mass, kMaxCellMass) / kMaxCellMass * h;
                }
                float cost = kBaseCost * typeCost(c.type, c.type == CellType::Myocyte ? a[A_Contract] : a[A_Swim]) * salinityFactor * h;
                // In the dark the photosynthetic machinery idles: a photocyte then only pays its dark respiration
                // (about a third), so algae get through a night instead of all starving at once.
                if (c.type == CellType::Photocyte) cost *= 0.35f + 0.65f * std::min(1.0f, lightAt(c.pos));
                // Адреналин: потребление энергии +100 % (было 1.6). Скорость жгутиков и ресничек +50 % — это
                // множитель boost = 1.5 в предпроходе (строка выше).
                if (c.stimulant > 0) cost *= 2.0f;
                cost += settings.radiation * 2.0f * h;
                // Electrocytes charge their membranes, cnidocytes coil a new thread: both cost energy.
                if ((c.type == CellType::Electrocyte || c.type == CellType::Cnidocyte) && c.charge < 1.0f) {
                    c.charge = std::min(1.0f, c.charge + (c.type == CellType::Electrocyte ? kElectrocyteCharge : kCnidoRecharge) * h);
                    cost += 0.25f * h;
                }
                // Циклогексимид блокирует получение энергии: клетка продолжает потреблять ресурсы (cost), но
                // не набирает массы. Атрактилозид разрывает связь митохондрий с цитоплазмой: фотосинтез и рост
                // не дают массы, а фагоцит/девороцит по-прежнему едят и поглощают — без прибавки массы.
                const bool noEnergy = c.cycloheximide > 0.0f;
                const bool noYield = c.atractyloside > 0.0f;
                if (noEnergy || noYield) gain = 0.0f;
                c.mass += gain - cost + S.dMass[i] * h + linkMass;
                if (c.mass > cap) c.mass = cap;
                // A wounded membrane lets cytoplasm seep out (released into the water serially).
                if (settings.sapLeak > 0 && c.injury > 0.15f) {
                    float l = kSapLeakRate * (c.injury - 0.15f) * settings.sapLeak * c.mass * h;
                    c.mass -= l;
                    c.leak += l;
                }
                if (c.type == CellType::Electrocyte && c.charge >= 1.0f && a[A_Swim] > 0.5f) S.discharge[i] = 1;

                if (c.type == CellType::Nitrocyte) c.nitrogen = 1.0f;
                else {
                    c.nitrogen += settings.nitrates * kNitroAbsorb * (1.0f - c.nitrogen) * h + linkN + S.dNitro[i] * h;
                    if (c.type == CellType::Secrocyte && c.nitrogen > 0.22f) c.nitrogen -= 0.45f * h;
                    c.nitrogen = clampv(c.nitrogen, 0.0f, 1.0f);
                }
                c.injury = std::max(0.0f, c.injury + S.dInjury[i] * h - kInjuryRecovery * c.injury * h);
                }  // (our metabolism)
            }
            for (int s = 0; s < kSignalCount; ++s) {
                if (cl) {  // Cell Lab: settling at 5/h toward the production; its ±1 limit is ±20 in our signal scale
                    c.signals[s] = clampv(c.signals[s] + 5.0f * (S.dSignal[i][s] - c.signals[s]) * h + linkSig[s], -20.0f, 20.0f);
                    continue;
                }
                float v2 = c.signals[s] + (S.dSignal[i][s] - c.signals[s] * kSignalDecay) * h * kSignalSpeed + linkSig[s];
                c.signals[s] = clampv(v2, -100.0f, 100.0f);
            }
            c.age += h;
            c.boost = std::max(0.0f, c.boost - h);
            c.virusCooldown = std::max(0.0f, c.virusCooldown - h);
            c.stun = std::max(0.0f, c.stun - h);
            c.paralysis = std::max(0.0f, c.paralysis - h);
            c.stimulant = std::max(0.0f, c.stimulant - h);
            // CellFap: таймеры новых веществ (h remaining) и их эффекты
            decaySubstance(c, h);
            if (c.type == CellType::Chromocyte) {
                c.chromatocyteParam = a[A_Swim] * 2.0f - 1.0f;  // -1..1: where on its gradient
                c.chromatocytePreset = (uint8_t)std::min<int>(m.senseChannel, 2);  // the editor keeps the preset there
            }
            // Хроноцит: импульс каждые N часов. Сигнал кладём прямо в клетку, а не в dSignal: эта фаза идёт уже
            // после того, как сигналы тика разошлись, и запись в dSignal ушла бы в никуда (обнуляется на тике).
            if (c.type == CellType::Chronocyte) {
                // Период задаёт параметр b программы «Сигнал пути 1». Нижняя граница — 0.0025 ч (один тик),
                // выше разумного интервала в 48 ч нет смысла: это сутки игрового времени.
                float period = clampv(m.prog[PF_Stem1].b, kHoursPerTick, 48.0f);
                c.chronocyteTimer += h;
                if (c.chronocyteTimer >= period) {
                    c.chronocyteTimer = 0.0f;
                    const int ch = m.senseChannel % kSignalCount;
                    c.signals[ch] = clampv(c.signals[ch] + 1.0f, -100.0f, 100.0f);
                }
            }
            // Спироцит: вращение вокруг своей оси (программа «Сила плавания» = скорость, знак = направление)
            if (c.type == CellType::Spirocyte) {
                float spin = a[A_Swim] * 4.0f * (c.paralysis > 0 ? 0.0f : 1.0f);  // рад/тик
                c.angVel += (spin - c.angVel) * std::min(1.0f, h * 200.0f);
                c.spirocyteAngle += c.angVel;
            }
            // Гриффоцит: сила сцепления с дном зависит от сигнала (программа «Сигнал пути 1»)
            if (c.type == CellType::Griffocyte) {
                float sig = std::fabs(a[A_Stem1]);
                c.griffocyteGrip += (sig - c.griffocyteGrip) * std::min(1.0f, h * 20.0f);
                if (c.griffocyteGrip > 0.05f) {
                    // Holding on to the bottom of the dish (the substrate under the cell, seen from above): sliding
                    // is damped by the grip. (Pulling the cell toward the lower rim of the picture was not "the
                    // bottom": the dish is seen from above.)
                    c.vel *= (1.0f - std::min(0.9f, c.griffocyteGrip * h * 60.0f));
                }
            }
            // Гранулоцит: токсичность растёт от ранений и от яда, вытянутого из соседей по адгезину; при пороге он
            // рвёт все связи и погибает, оставляя отравленную точку, из которой любая клетка впитывает вещество.
            if (c.type == CellType::Granulocyte) {
                c.granulocyteToxicity += c.injury * h * 0.5f + S.dToxic[i];
                if (c.granulocyteToxicity > kGranulocyteLethal) {
                    // Обрываем все связи и оставляем отравленную точку на месте гибели
                    while (c.linkCount > 0) removeLink(c.links[0]);
                    addFluid(c.pos, FluidKind::Toxin, c.mass * 0.5f + c.granulocyteToxicity * 0.2f, 0xffa0d8a0);
                    c.granulocyteToxicity = 0;
                    S.dead[i] = 1;
                    continue;
                }
            }
            c.actuator = c.type == CellType::Flagellocyte   ? a[A_Swim]
                         : c.type == CellType::Myocyte     ? a[A_Contract]  // smoothed muscle state (see prepass)
                         : c.type == CellType::Ciliocyte   ? std::fabs(a[A_Bend]) + std::fabs(a[A_Lift])
                         : c.type == CellType::Chromatocyte || c.type == CellType::Luminocyte ? a[A_Swim]  // pigment / glow
                         : c.type == CellType::Grappler    ? (a[A_Swim] >= 0.3f ? 1.0f : 0.0f)            // grip closed
                         : c.type == CellType::Electrocyte || c.type == CellType::Cnidocyte ? c.charge
                                                           : a[A_Sense];
            c.lastSense = c.type == CellType::Myocyte ? a[A_Bend] : c.lastSense;
            if (cl) {
                // Cell Lab: the radius follows the mass with a lag (newborns keep their parent's size for 0.1 h and burst
                // apart), and fat does not swell a cell.
                const float rt = radiusForMass(std::min(c.mass, kMaxCellMass), CellType::Phagocyte);
                if (c.age > 0.1f) c.radius += h * 5.0f * (rt * rt / std::max(c.radius, 0.1f) - c.radius);
                c.radius = clampv(c.radius, 1.0f, kMaxCellRadius);
            } else {
                c.radius = radiusForMass(c.mass, c.type);
            }

            // Fate decisions (applied serially).
            const bool cyst = c.type == CellType::Cystocyte;
            // Бензиладенин (6-БА): клетка живёт дольше — предел возраста выше в kBenzyladinLife раз.
            const float ageLimit = kMaxAge * (1.0f + kBenzyladinLife * c.benzyladin);
            if (!previewMode) {
                // A cyst sleeps through nitrogen hunger and old age; only starvation and wounds kill it.
                if (c.mass <= kDeathMass || (c.nitrogen < 0.1f && !cyst) || c.injury >= 1.0f ||
                    (settings.cellAging && c.age > ageLimit && !cyst)) {
                    S.dead[i] = 1;
                    continue;
                }
            }
            if (c.type == CellType::Stemocyte && c.age >= kMinSplitAge) {
                if (a[A_Stem1] > 0.5f) { S.newMode[i] = m.stemMode[0]; continue; }
                if (a[A_Stem2] > 0.5f) { S.newMode[i] = m.stemMode[1]; continue; }
            }
            if (cyst) {
                if (c.age >= kMinSplitAge && a[A_Stem1] > 0.5f) S.newMode[i] = m.stemMode[0];  // wakes up
                continue;                                                                      // a cyst never divides
            }
            // Колхицин останавливает деление: клетка растёт и живёт, но не делится, пока вещество внутри.
            if (c.type != CellType::Gamete && c.age >= kMinSplitAge && c.nitrogen >= 0.2f &&
                c.linkCount < m.maxConnections && c.telomeres != 0 && c.colchicine <= 0.0f) {
                bool ready = m.splitMass <= kSplitAtAge ? true : (m.splitMass < kNeverSplit && c.mass >= m.splitMass);
                if (ready) S.wantsSplit[i] = 1;
            }
        }
    });

    // ---- 5a. what the program cells answered: levels of their signals and their memory, all at once ----------
    for (const auto& [idx, res] : S.signalResults) {
        if (idx < 0 || idx >= n || !cells_[idx].alive) continue;
        Cell& c = cells_[idx];
        const float lim = cl ? 1.0f : 20.0f;  // the signal range of the dish
        for (int s = 0; s < kSignalCount; ++s)
            if (std::isfinite(res[s])) c.signals[s] = clampv(res[s], -lim, lim);
        for (int k = 0; k < kSignalCellMemory; ++k)
            if (std::isfinite(res[kSignalCount + k])) c.progMem[k] = res[kSignalCount + k];
    }

    // ---- 5b. contact position solver --------------------------------------------------
    // Springs alone let crowded cells sink deep into each other. A few Jacobi iterations project
    // overlapping pairs apart until only a small, cytoskeleton-dependent squash remains (heavier cells
    // move less). The correction is fed back into the velocity so cells do not re-penetrate next tick.
    S.push.resize(n);
    for (int it = 0; it < (cl ? 0 : kContactIterations); ++it) {  // (Cell Lab: forces only)
        jobs_->parallelFor(n, 256, [&](int b, int e) {
            for (int i = b; i < e; ++i) {
                const Cell& c = cells_[i];
                Vec2 corr{};
                if (c.alive) {
                    const float hardC = S.hard[i];
                    const int* nb = &S.nbr[(size_t)i * kMaxNbr];
                    for (int k = 0, nc = S.nbrCount[i]; k < nc; ++k) {
                        int j = nb[k];
                        const Cell& o = cells_[j];
                        if (!o.alive) continue;
                        Vec2 d = c.pos - o.pos;
                        float d2 = length2(d);
                        float hard = 0.5f * (hardC + S.hard[j]);
                        float minDist = (c.radius * S.squeeze[i] + o.radius * S.squeeze[j]) * (1.0f - kMaxSquash / hard);
                        if (d2 >= minDist * minDist) continue;
                        float dist = std::sqrt(d2);
                        Vec2 nrm = dist > 1e-4f ? d / dist : fromAngle((float)(i * 7 + j));
                        float share = o.mass / (c.mass + o.mass);
                        corr += nrm * ((minDist - dist) * share * 0.5f);
                    }
                    // Dish wall.
                    if (settings.lid || previewMode) {
                        float pd = length(c.pos + corr);
                        float maxR = settings.dishRadius - c.radius * (1.0f - kMaxSquash);
                        if (pd > maxR) corr += (c.pos + corr) * ((maxR - pd) / pd);
                    }
                }
                float cl = length(corr);
                if (cl > kMaxPush) corr = corr * (kMaxPush / cl);
                S.push[i] = corr;
            }
        });
        jobs_->parallelFor(n, 512, [&](int b, int e) {
            for (int i = b; i < e; ++i) {
                Cell& c = cells_[i];
                if (!c.alive) continue;
                c.pos += S.push[i];
                c.vel += S.push[i] * 0.35f;
            }
        });
    }

    collideBarriers();

    // ---- 5c. bond ropes ---------------------------------------------------------------------
    // A bond stretched past its maxLen is pulled back to it (the lighter cell moves more) and the two cells stop
    // drifting apart along it: cilia and muscles shear a body without ripping gaps into it. A violent yank, far past
    // the limit within one tick, tears the bond instead.
    for (int pass = 0; pass < (cl ? 0 : 2); ++pass) {  // (Cell Lab: no ropes)
        for (size_t li = 0; li < links_.size(); ++li) {
            Link& l = links_[li];
            if (!l.alive || l.b < 0 || l.maxLen <= 0 || S.brokenLink[li]) continue;
            Cell& A = cells_[l.a];
            Cell& B = cells_[l.b];
            if (!A.alive || !B.alive) continue;
            Vec2 d = B.pos - A.pos;
            float dist = length(d);
            float excess = dist - l.maxLen;
            if (excess <= 0 || dist < 1e-4f) continue;
            if (excess > (A.radius + B.radius) * 1.2f + 12.0f) {  // as the spring pass: yanked apart
                S.brokenLink[li] = 1;
                continue;
            }
            Vec2 nrm = d / dist;
            float wA = 1.0f / std::max(A.mass, 0.05f), wB = 1.0f / std::max(B.mass, 0.05f);
            float kA = wA / (wA + wB), kB = wB / (wA + wB);
            A.pos += nrm * (excess * kA);
            B.pos -= nrm * (excess * kB);
            float vrel = dot(B.vel - A.vel, nrm);
            if (vrel > 0) {
                A.vel += nrm * (vrel * kA);
                B.vel -= nrm * (vrel * kB);
            }
        }
    }

    // ---- 5d. closed dish: a hard wall --------------------------------------------------------
    // The soft wall force alone gave way under a packed colony and squeezed cells out through the glass.
    if ((settings.lid || previewMode) && !cl)  // (Cell Lab: the wall force only)
        jobs_->parallelFor(n, 512, [&](int b, int e) {
            for (int i = b; i < e; ++i)
                if (cells_[i].alive) clampIntoDish(cells_[i]);
        });

    mark(5);
    // ---- 6. serial bookkeeping ---------------------------------------------------------
    for (size_t li = 0; li < links_.size(); ++li)
        if (S.brokenLink[li] && links_[li].alive) {
            ++tornLinks;
            // A torn adhesin rips the membranes it held.
            if (settings.mechanicalDamage > 0 && !previewMode) {
                const Link& l = links_[li];
                float tear = 0.12f * settings.mechanicalDamage;
                cells_[l.a].injury += cells_[l.a].isProtected ? tear * 0.25f : tear;
                if (l.b >= 0) cells_[l.b].injury += cells_[l.b].isProtected ? tear * 0.25f : tear;
            }
            removeLink((int)li);
        }

    // Protease cuts links between two cells it touches at once; lipase uncoats touched food.
    for (int si : S.secrocytes) {
        Cell& s = cells_[si];
        if (!s.alive || (cl ? s.nitrogen < 0.2f : s.nitrogen <= 0.22f)) continue;
        Secretion sec = modeOf(s).secretion;
        if (sec == Secretion::Protease && s.age >= kMinSplitAge) {
            int touching[32];
            int nt = 0;
            forNeighbors(s.pos, s.radius + searchPad, [&](int j) {
                if (j == si || nt >= 32) return;
                float rs = s.radius + cells_[j].radius;
                if (length2(cells_[j].pos - s.pos) < rs * rs) touching[nt++] = j;
            });
            for (int x = 0; x < nt; ++x)
                for (int y = x + 1; y < nt; ++y) {
                    Cell& cx = cells_[touching[x]];
                    for (int k = 0; k < cx.linkCount; ++k) {
                        Link& l = links_[cx.links[k]];
                        if (l.a == touching[y] || l.b == touching[y]) {
                            removeLink(cx.links[k]);
                            break;
                        }
                    }
                }
        } else if (sec == Secretion::Lipase) {
            forFoodNear(s.pos, s.radius + 4.0f, [&](int fi) {
                Food& f = food_[fi];
                float rr = s.radius + foodRadius(f.mass);
                if (length2(f.pos - s.pos) < rr * rr) f.coating = std::max(0.0f, f.coating - 1.0f * h);
            });
        }
    }

    for (const Event& ev : S.events) {
        switch (ev.kind) {
            case EventKind::Infect: {
                Cell& v = cells_[ev.a];
                Cell& t = cells_[ev.b];
                // Интерферон: клетка, внутри которой он есть, не заражается. Он не даёт защиты соседям — это
                // противовирусная защита самой клетки, а не брони, поэтому с кератиноцитами не путаем.
                if (t.interferon > 0.0f) break;
                if (cl) {
                    // Cell Lab: the virus mode and two generations of its children are written into the victim, shifted
                    // by (second int - virus mode) within the game's 40 modes, child numbers untouched. Only user and
                    // contamination cells change their tag.
                    if (!v.alive || !t.alive) break;
                    const Genome& vg = genomes_.get(v.genome);
                    const Mode& vm = vg.modes[v.mode];
                    // (the second int is 0 in CellFap's own virocytes: the same mode numbers)
                    const int s = vm.virusCopyFrom, off = vm.stemMode[1] == 0 ? 40 : (int)vm.stemMode[1] - s + 40;
                    int src[7] = {s, vg.modes[s].childMode[0], vg.modes[s].childMode[1], 0, 0, 0, 0};
                    for (int k = 0; k < 2; ++k)
                        for (int q = 0; q < 2; ++q) src[3 + 2 * k + q] = vg.modes[src[1 + k]].childMode[q];
                    Genome ng = genomes_.get(t.genome);
                    bool changed = false;
                    for (int k = 0; k < 7; ++k) {
                        const int to = (src[k] + off) % 40;
                        if (!modeEquals(ng.modes[to], vg.modes[src[k]])) {
                            ng.modes.share(to, vg.modes, src[k]);
                            changed = true;
                        }
                    }
                    if (!changed) break;
                    int newId = genomes_.add(ng);
                    genomes_.release(t.genome);
                    t.genome = newId;
                    const float keepR = t.radius;
                    setMode(t, t.mode);
                    t.radius = keepR;
                    if (t.tag == CellTag::User) t.tag = CellTag::InfectedUser;
                    else if (t.tag == CellTag::Contamination) t.tag = CellTag::InfectedContamination;
                    break;
                }
                if (!v.alive || !t.alive || v.virusCooldown > 0) break;
                const Genome& vg = genomes_.get(v.genome);
                const Mode& vm = vg.modes[v.mode];
                bool copy[kMaxModes] = {};
                int from = vm.virusCopyFrom;
                copy[from] = true;
                for (int c1 = 0; c1 < 2; ++c1) {
                    int m1 = vg.modes[from].childMode[c1];
                    copy[m1] = true;
                    for (int c2 = 0; c2 < 2; ++c2) copy[vg.modes[m1].childMode[c2]] = true;
                }
                Genome ng = genomes_.get(t.genome);
                bool changed = false;
                for (int k = 0; k < kMaxModes; ++k)
                    if (copy[k] && !modeEquals(ng.modes[k], vg.modes[k])) {
                        ng.modes.share(k, vg.modes, k);
                        changed = true;
                    }
                v.virusCooldown = 0.25f;
                if (!changed) break;
                int newId = genomes_.add(ng);
                genomes_.release(t.genome);
                t.genome = newId;
                setMode(t, t.mode);
                if (t.tag == CellTag::User) t.tag = CellTag::InfectedUser;
                else if (t.tag == CellTag::Challenge) t.tag = CellTag::InfectedChallenge;
                else if (t.tag == CellTag::Contamination) t.tag = CellTag::InfectedContamination;
                break;
            }
            case EventKind::Glue: {
                Cell& g = cells_[ev.a];
                if (!g.alive || !cells_[ev.b].alive || g.linkCount >= kGlueMaxLinks) break;
                createLink(ev.a, ev.b, 0.0f, cl ? 0.5f / 35.0f : modeOf(g).adhesinStiffness, false);  // (Cell Lab: always 0.5 of 35)
                break;
            }
            case EventKind::GlueWall: {
                Cell& g = cells_[ev.a];
                if (!g.alive || g.glueWallLinks || g.linkCount >= kGlueMaxLinks) break;
                createWallLink(ev.a, ev.p);
                break;
            }
            case EventKind::Grip:
            case EventKind::GripWall: {
                Cell& g = cells_[ev.a];
                if (!g.alive || (ev.b >= 0 && !cells_[ev.b].alive) || g.linkCount >= kMaxLinks) break;
                int held = 0;  // grip links are the grappler's links that did not come from division
                for (int k = 0; k < g.linkCount; ++k) held += !links_[g.links[k]].fromDivision;
                if (held >= kGripMaxLinks) break;
                if (ev.kind == EventKind::GripWall) {
                    if (!g.glueWallLinks) createWallLink(ev.a, ev.p);
                } else {
                    createLink(ev.a, ev.b, 0.0f, 0.15f, false);
                }
                break;
            }
            case EventKind::Fuse: {
                Cell& A = cells_[ev.a];
                Cell& B = cells_[ev.b];
                if (!A.alive || !B.alive || A.type != CellType::Gamete || B.type != CellType::Gamete) break;
                if (cl) {
                    // Cell Lab: the bigger gamete stays in place (position, motion, links) and becomes the zygote; each
                    // of its modes is the other's with even odds; mode and telomeres from its own gene.
                    if (S.dead[ev.a] || S.dead[ev.b]) break;  // swallowed instead
                    const int zi = A.radius > B.radius || (A.radius == B.radius && ev.a > ev.b) ? ev.a : ev.b;
                    const int oi = zi == ev.a ? ev.b : ev.a;
                    Cell& z = cells_[zi];
                    Cell& o = cells_[oi];
                    const Mode zm = modeOf(z);
                    Genome child = genomes_.get(z.genome);
                    const Genome& og = genomes_.get(o.genome);
                    for (int k = 0; k < kMaxModes; ++k)
                        if (rng_.chance(0.5f)) child.modes.share(k, og.modes, k);
                    if (isPlayerTag(z.tag) || isPlayerTag(o.tag)) ++fusions;
                    if (z.tag != o.tag) z.tag = CellTag::Hybrid;
                    z.mass = std::min(z.mass + o.mass, kMaxCellMass);
                    z.nitrogen = std::min(1.0f, z.nitrogen + o.nitrogen);
                    z.age = 0;
                    z.telomeres = (int8_t)kTelomereChoices[clampv((int)zm.gameteTelomeres, 0, 5)];
                    int ng = genomes_.add(child);
                    genomes_.release(z.genome);
                    z.genome = ng;
                    const float keepR = z.radius;
                    setMode(z, zm.gameteModeAfter);
                    z.radius = keepR;
                    killCell(oi, false);
                    break;
                }
                Cell& big = A.mass >= B.mass ? A : B;
                const Genome& ga = genomes_.get(A.genome);
                const Genome& gb = genomes_.get(B.genome);
                Genome child = ga;
                for (int k = 0; k < kMaxModes; ++k)
                    if (rng_.chance(0.5f)) child.modes.share(k, gb.modes, k);
                const Mode& bm = modeOf(big);
                int afterMode = bm.gameteModeAfter;
                int telo = kTelomereChoices[clampv((int)bm.gameteTelomeres, 0, 5)];
                float totalMass = std::min(A.mass + B.mass, kMaxCellMass);
                Vec2 pos = (A.pos * A.mass + B.pos * B.mass) / (A.mass + B.mass);
                Vec2 vel = (A.vel * A.mass + B.vel * B.mass) / (A.mass + B.mass);
                float angle = big.angle;
                CellTag tag = A.tag == B.tag ? A.tag : CellTag::Hybrid;
                float nitro = std::min(1.0f, A.nitrogen + B.nitrogen);
                int ia = ev.a, ib = ev.b;
                if (isPlayerTag(A.tag) || isPlayerTag(B.tag)) ++fusions;
                killCell(ia, false);
                killCell(ib, false);
                int idx = addCell(child, pos, angle, afterMode, tag, totalMass);
                if (idx >= 0) {
                    Cell& z = cells_[idx];
                    z.vel = vel;
                    z.age = 0;
                    z.nitrogen = nitro;
                    z.telomeres = (int8_t)telo;
                }
                break;
            }
            case EventKind::Kill:
                if (cells_[ev.a].alive) killCell(ev.a, true);
                break;
            case EventKind::Remove:
                if (cells_[ev.a].alive) killCell(ev.a, false);
                break;
            case EventKind::Sting: {
                Cell& s = cells_[ev.a];
                Cell& t = cells_[ev.b];
                if (!s.alive || !t.alive || s.charge < 1.0f) break;
                s.charge = 0;  // the thread is spent; a new one coils in about an hour
                // The barbed thread pierces the membrane (armour mostly stops it) and injects a paralysing venom.
                float armour = t.isProtected ? 0.25f : 1.0f;
                t.injury += 0.35f * armour;
                t.paralysis += 0.4f * armour;
                float l = t.mass * 0.04f * armour * std::min(settings.sapLeak, 1.0f);
                t.mass -= l;
                t.leak += l;
                break;
            }
            default: break;
        }
    }

    // Electric organs: every electrocyte that fires adds its field; the sum acts on the cells around (the
    // organism's own cells are insulated). Many electrocytes firing together stun or kill.
    {
        bool any = false;
        for (int i = 0; i < n; ++i)
            if (S.discharge[i] && cells_[i].alive) {
                any = true;
                break;
            }
        if (any) {
            fieldAcc_.assign(n, 0.0f);
            for (int i = 0; i < n; ++i) {
                if (!S.discharge[i] || !cells_[i].alive) continue;
                Cell& e = cells_[i];
                e.charge = 0;
                float volts = kElectrocyteVolts * std::sqrt(std::max(e.mass, 0.3f) / 2.0f);
                sparks_.push_back({e.pos, kElectrocyteReach, (float)time_});
                forNeighbors(e.pos, kElectrocyteReach * 2.5f, [&](int j) {
                    if (j == i || !cells_[j].alive || cells_[j].genome == e.genome) return;
                    float d = std::max(0.0f, length(cells_[j].pos - e.pos) - cells_[j].radius);
                    if (d > kElectrocyteReach * 2.5f) return;
                    fieldAcc_[j] += 1.5f * shockField(volts, kElectrocyteReach, d) * cells_[j].radius;
                });
            }
            for (int j = 0; j < n; ++j)
                if (fieldAcc_[j] > 0) applyElectric(j, fieldAcc_[j], 2.0f);
        }
        // Flash markers live for a short while (render only).
        sparks_.erase(std::remove_if(sparks_.begin(), sparks_.end(), [&](const Spark& s) { return time_ - s.t > 0.05; }),
                      sparks_.end());
    }

    // Deaths, differentiation, splits.
    for (int i = 0; i < n; ++i) {
        if (!cells_[i].alive) continue;
        if (S.dead[i]) {
            killCell(i, true, cells_[i].injury >= 1.0f);  // killed by injury: the membrane bursts
            continue;
        }
        Cell& lc = cells_[i];
        if (lc.type == CellType::Grappler && lc.linkCount && S.act[i][A_Swim] < 0.3f)  // the grip opens: let go
            for (int k = lc.linkCount - 1; k >= 0; --k)
                if (!links_[lc.links[k]].fromDivision) removeLink(lc.links[k]);
        if (lc.leak > 0.03f) {  // seeping cell sap leaves through the wound in small droplets
            spillSap(lc, lc.leak, lc.pos + fromAngle(rng_.range(0, kTau)) * (lc.radius * 0.95f));
            lc.leak = 0;
        }
        if (S.newMode[i] >= 0) {
            Cell& c = cells_[i];
            const float keepR = c.radius;
            setMode(c, S.newMode[i]);
            if (cl) c.radius = keepR;  // (Cell Lab: the size relaxes, it does not jump)
            c.age = 0;
            continue;
        }
    }
    // Cell Lab: radiation mutates living cells (every 0.3 h, with odds 4 × radiation, three changes to the cell's own
    // genome), not their children at division.
    if (cl && settings.radiation > 0)
        for (int i = 0; i < n; ++i) {
            Cell& c = cells_[i];
            // Ресвератрол: клетка устойчива к радиации — шанс мутации падает в 1/(1+resveratrol) раз.
            if (!c.alive || (ticks_ + c.id) % 120 != 0 ||
                !rng_.chance(4.0f * settings.radiation / (1.0f + c.resveratrol))) continue;
            Genome g = genomes_.get(c.genome);
            MutationOptions opt;
            opt.pointOnly = settings.onlyPointMutations;
            opt.allowedTypes = settings.genePool.data();
            for (int k = 0; k < 3; ++k) mutateGenome(g, rng_.irange(0, 39), rng_, opt);
            const int ng = genomes_.add(g);
            genomes_.release(c.genome);
            c.genome = ng;
            c.mutations = (uint16_t)std::min(65535, c.mutations + 3);
            const float keepR = c.radius;
            setMode(c, c.mode);
            c.radius = keepR;
        }
    // The age check guards against a slot freed by an event and reused by a newborn this tick.
    // Divisions stop when the dish is physically full (packing capacity), not only at the user's limit:
    // squeezing more cells in than fit makes every contact a deep overlap.
    const float Rd = settings.dishRadius;
    const int capacity = std::max(50, (int)(0.8f * Rd * Rd / (11.0f * 11.0f)));
    const int splitLimit = std::min(settings.maxCells, capacity);
    for (int i = 0; i < n; ++i)
        if (S.wantsSplit[i] && cells_[i].alive && cells_[i].age >= kMinSplitAge && cellCount_ < splitLimit)
            splitCell(i);

    // Food bookkeeping: drop eaten-out pieces, mobile food, spawning.
    if (cl)  // Cell Lab: food rots away (0.06 ng/h per piece)
        for (Food& f : food_) f.mass -= 0.06f * h;
    food_.erase(std::remove_if(food_.begin(), food_.end(), [](const Food& f) { return f.mass < 0.005f; }), food_.end());
    if (!previewMode) updateFood();
    if (!previewMode) spawnFood();
    updateFluids();
    if (hasFlow() && !previewMode) {
        // Currents carry food crumbs (light: nearly at the water speed) and dissolved clouds.
        for (Food& f : food_) {
            const Vec2 u = flowAt(f.pos);
            if (length2(u) < 1e-8f) continue;
            const Vec2 np = f.pos + u * 0.9f;
            const float fr = foodRadius(f.mass);
            if (length(np) < R - fr && !barrierAt(np, fr)) f.pos = np;
        }
        for (Fluid& fl : fluids_) fl.vel += flowAt(fl.pos) * 0.15f;
    }

    // Contamination: occasional random cells from the gene pool.
    if (settings.contaminate && !previewMode && rng_.chance((cl ? 3.3f : 0.5f) * h)) {
        float a = rng_.range(0, kTau), r = std::sqrt(rng_.uniform()) * R * 0.9f;
        addCell(randomGenome(rng_, settings.genePool.data()), fromAngle(a) * r, rng_.range(0, kTau), -1, CellTag::Contamination);
    }

    mark(6);
    settings.lightAngle = wrapAngle(settings.lightAngle + settings.lightDirChange * 0.6f * h);
    time_ += h;
    ticks_++;
    if (time_ >= nextStatTime_) {
        recordStats();
        nextStatTime_ = time_ + kStatInterval;
    }
    if (afterStep) afterStep(*this);  // the mods (set by the game on its dish)
}

void World::splitCell(int pi) {
    const bool cl = settings.cellLabPhysics && !previewMode;
    const Mode m = modeOf(cells_[pi]);  // copy: genome pool may reallocate below
    Cell parent = cells_[pi];
    float sgn = parent.mirrored ? -1.0f : 1.0f;
    float splitDir = parent.angle + sgn * m.splitAngle;
    Vec2 dir = fromAngle(splitDir);

    // Child genomes (mutations happen independently in each child).
    int genome[2] = {parent.genome, parent.genome};
    int childModeIdx[2] = {m.childMode[0], m.childMode[1]};
    uint16_t muts[2] = {parent.mutations, parent.mutations};
    // Ресвератрол в родителе передаётся детям и снижает частоту радиационных мутаций.
    float p = cl ? 0.0f : settings.radiation * 6.0f / (1.0f + parent.resveratrol);  // (Cell Lab: mutations strike living cells instead, see step)
    for (int k = 0; k < 2; ++k) {
        if (p > 0 && rng_.chance(p)) {
            Genome g = genomes_.get(parent.genome);
            MutationOptions opt;
            opt.pointOnly = settings.onlyPointMutations;
            opt.allowedTypes = settings.genePool.data();
            int sw = mutateGenome(g, childModeIdx[k], rng_, opt);
            if (sw >= 0) childModeIdx[k] = sw;
            genome[k] = genomes_.add(g);
            muts[k]++;
        } else {
            genomes_.retain(parent.genome);
        }
    }

    int idx[2];
    idx[0] = pi;  // child 1 reuses the parent's slot (and id, so trackers follow it)
    idx[1] = allocCell();
    float masses[2] = {parent.mass * m.splitRatio, parent.mass * (1.0f - m.splitRatio)};

    for (int k = 0; k < 2; ++k) {
        Cell& c = cells_[idx[k]];
        if (k == 1) {
            uint32_t id = c.id;
            c = parent;
            c.id = id;
            c.linkCount = 0;
            c.glueWallLinks = 0;
        }
        c.genome = genome[k];
        c.mutations = muts[k];
        c.mass = masses[k];
        c.nitrogen = parent.nitrogen * 0.5f;
        c.age = 0;
        c.boost = 0;
        c.mirrored = parent.mirrored != m.mirror[k];
        c.angle = wrapAngle(splitDir + sgn * m.childAngle[k]);
        c.angVel = parent.angVel;
        if (c.telomeres > 0) c.telomeres--;
        setMode(c, childModeIdx[k]);
        if (cl) {
            // Cell Lab: both start with the parent's size (they burst apart) and pay for a reinforced cytoskeleton.
            c.radius = parent.radius;
            c.mass = std::max(0.05f, c.mass - std::max(0.0f, modeOf(c).cytoskeleton - 1.0f) * 0.54f);
        }
    }
    genomes_.release(parent.genome);  // the parent's own reference

    if (cl) {
        cells_[idx[0]].pos = parent.pos + dir * (0.1f * parent.radius);  // 0.2 r apart
        cells_[idx[1]].pos = parent.pos - dir * (0.1f * parent.radius);
        clInheritLinks(pi, idx[1], parent, dir, m);
    } else {
    // Place the children touching, keeping the centre of mass.
    {
        Cell& c1 = cells_[idx[0]];
        Cell& c2 = cells_[idx[1]];
        float sep = (c1.radius + c2.radius) * 0.98f;
        float tot = c1.mass + c2.mass;
        c1.pos = parent.pos + dir * (sep * c2.mass / tot);
        c2.pos = parent.pos - dir * (sep * c1.mass / tot);
    }

    // Inherit the parent's links by side of the split plane (currently all attached to slot pi).
    int parentLinks[kMaxLinks];
    int pl = cells_[pi].linkCount;
    std::memcpy(parentLinks, cells_[pi].links, sizeof(int) * pl);
    for (int k = 0; k < pl; ++k) {
        int li = parentLinks[k];
        Link& l = links_[li];
        if (!l.alive) continue;
        if (l.b < 0) {  // glue to the wall is lost on division
            removeLink(li);
            continue;
        }
        bool aSide = l.a == pi;
        int other = aSide ? l.b : l.a;
        float anchor = aSide ? l.angA : l.angB;
        float s = dot(fromAngle(parent.angle + anchor), dir);
        // Links within ~15° of the split plane are shared by both children (keeps tissues cohesive).
        bool to1 = s > kPlaneShare, to2 = s < -kPlaneShare;
        if (!to1 && !to2) to1 = to2 = true;  // on the plane: both
        to1 = to1 && m.keepAdhesin[0];
        to2 = to2 && m.keepAdhesin[1];
        float extra = l.extraLength, stiff = l.stiffness;
        bool fromDiv = l.fromDivision;
        if (to1) {
            // Re-aim the anchors at the new geometry.
            Cell& c = cells_[pi];
            Cell& o = cells_[other];
            Vec2 d = o.pos - c.pos;
            float phi = std::atan2(d.y, d.x);
            if (aSide) {
                l.angA = wrapAngle(phi - c.angle);
                l.angB = wrapAngle(phi + kPi - o.angle);
            } else {
                l.angB = wrapAngle(phi - c.angle);
                l.angA = wrapAngle(phi + kPi - o.angle);
            }
        } else {
            removeLink(li);
        }
        if (to2) createLink(idx[1], other, extra, stiff, fromDiv);
    }
    }  // (our placement and inheritance)

    if (m.makeAdhesin) createLink(idx[0], idx[1], m.adhesinLength * 10.0f, m.adhesinStiffness, true);
    if (recordEvents) noteEvent(CellEvent::Divided, idx[0], idx[1]);  // (before the deaths at birth below)

    if (cl) {
        // Cell Lab: a newborn flagellocyte with any adhesin anchored behind it (into its flagellum) dies.
        for (int k = 0; k < 2; ++k) {
            Cell& c = cells_[idx[k]];
            if (!c.alive || c.type != CellType::Flagellocyte) continue;
            for (int q = 0; q < c.linkCount; ++q) {
                const Link& l = links_[c.links[q]];
                if (std::cos(l.a == idx[k] ? l.angA : l.angB) < -0.8f) {
                    killCell(idx[k], true);
                    break;
                }
            }
        }
    } else if (m.makeAdhesin) {
        // A flagellocyte whose flagellum points into its glued sibling dies at birth.
        for (int k = 0; k < 2; ++k) {
            Cell& c = cells_[idx[k]];
            if (c.type != CellType::Flagellocyte) continue;
            Vec2 toSib = normalize(cells_[idx[1 - k]].pos - c.pos);
            if (dot(-fromAngle(c.angle), toSib) > 0.8f) {
                killCell(idx[k], true);
                break;
            }
        }
    }
    for (int k = 0; k < 2; ++k)
        if (cells_[idx[k]].alive && cells_[idx[k]].telomeres == 0) killCell(idx[k], true);

    // Weld bands while the body grows (as spawnGrownOrganism does for a grown one): the order of divisions leaves cells of
    // one band (a muscle or cilia strip of a worm) touching their neighbour from the next pair of segments without an
    // adhesin, and an unlinked band shears into steps when the body bends. A child links to touching cells of the same
    // organism 2-4 links away (the same tissue, not another organism or a fold of a long chain).
    if (!cl && !previewMode)
        for (int k = 0; k < 2; ++k) {
            const int ci = idx[k];
            if (ci < 0 || !cells_[ci].alive || !m.makeAdhesin || cells_[ci].linkCount >= kMaxLinks) continue;
            int near[64], depth[64], nn = 0;
            near[nn] = ci, depth[nn++] = 0;
            for (int q = 0; q < nn; ++q) {
                if (depth[q] >= 4) continue;
                const Cell& u = cells_[near[q]];
                for (int e = 0; e < u.linkCount && nn < 64; ++e) {
                    const Link& l = links_[u.links[e]];
                    if (!l.alive || l.b < 0) continue;
                    const int v = l.a == near[q] ? l.b : l.a;
                    bool seen = false;
                    for (int s = 0; s < nn && !seen; ++s) seen = near[s] == v;
                    if (!seen) near[nn] = v, depth[nn++] = depth[q] + 1;
                }
            }
            const Cell& c = cells_[ci];
            for (int s = 0; s < nn; ++s) {
                const int j = near[s];
                if (depth[s] < 2) continue;
                const Cell& o = cells_[j];
                if (!o.alive || o.genome != c.genome || o.linkCount >= kMaxLinks) continue;
                // A band of one mode is welded across a small gap; any other cell of the same tissue only when it really
                // touches (that also grows a neck across the whole width of the body, not one link to one side).
                const float reach = o.mode == c.mode ? 1.25f : 1.05f;
                if (length(o.pos - c.pos) > (c.radius + o.radius) * reach || hasLink(ci, j)) continue;
                createLink(ci, j, 0.0f, m.adhesinStiffness, true);
                if (cells_[ci].linkCount >= kMaxLinks) break;
            }
        }
}

// Cell Lab: each of the parent's links goes to the child on its side of a band around the split plane, and the band
// moves with the size ratio; of the links inside the band only the outermost on each flank survive (kept by both
// children when one link is the outermost both ways). Anchors keep their direction in space instead of being re-aimed;
// glue to the wall is lost.
void World::clInheritLinks(int c1, int c2, const Cell& parent, Vec2 dir, const Mode& m) {
    const Vec2 toChild2 = -dir;  // child 2 lies on -dir (child 1 on +dir)
    const float share2 = 1.0f - m.splitRatio;
    struct Old {
        int li, other;
        float absAngle, q, partnerAnchor, extra, stiffness;
        bool fromDivision, flank, to1, to2;
    };
    Old old[kMaxLinks];
    int n = 0;
    const int pl = cells_[c1].linkCount;
    int ids[kMaxLinks];
    std::memcpy(ids, cells_[c1].links, sizeof(int) * pl);
    for (int k = 0; k < pl; ++k) {
        const Link& l = links_[ids[k]];
        if (!l.alive) continue;
        if (l.b < 0) {
            removeLink(ids[k]);
            continue;
        }
        const bool aSide = l.a == c1;
        Old o;
        o.li = ids[k];
        o.other = aSide ? l.b : l.a;
        o.absAngle = parent.angle + (aSide ? l.angA : l.angB);
        o.partnerAnchor = aSide ? l.angB : l.angA;
        o.extra = l.extraLength;
        o.stiffness = l.stiffness;
        o.fromDivision = l.fromDivision;
        const Vec2 ad = fromAngle(o.absAngle);
        o.q = dot(ad, toChild2) - (1.0f - 2.0f * share2);
        o.flank = cross(toChild2, ad) >= 0;
        o.to2 = o.q > 0.2f;
        o.to1 = o.q < -0.2f;
        old[n++] = o;
    }
    for (int f = 0; f < 2; ++f) {  // inside the band: the outermost link on each flank, toward each child
        int hi = -1, lo = -1;
        for (int k = 0; k < n; ++k) {
            if (std::fabs(old[k].q) > 0.2f || old[k].flank != (f == 1)) continue;
            if (hi < 0 || old[k].q > old[hi].q) hi = k;
            if (lo < 0 || old[k].q < old[lo].q) lo = k;
        }
        if (hi >= 0) old[hi].to2 = true;
        if (lo >= 0) old[lo].to1 = true;
    }
    for (int k = 0; k < n; ++k) {
        Old& o = old[k];
        const bool to1 = o.to1 && m.keepAdhesin[0], to2 = o.to2 && m.keepAdhesin[1];
        if (to1) {
            Link& l = links_[o.li];
            float& own = l.a == c1 ? l.angA : l.angB;
            own = wrapAngle(o.absAngle - cells_[c1].angle);
        } else {
            removeLink(o.li);
        }
        if (to2) {
            int nl = createLink(c2, o.other, o.extra, o.stiffness, o.fromDivision);
            if (nl >= 0) {
                links_[nl].angA = wrapAngle(o.absAngle - cells_[c2].angle);
                links_[nl].angB = o.partnerAnchor;
            }
        }
    }
}

bool World::barrierAt(Vec2 p, float margin) {
    if (barriers_.empty()) return false;
    if (barrierGridDirty_) buildBarrierGrid();
    int gx = (int)((p.x - barrierOrigin_.x) / barrierCell_), gy = (int)((p.y - barrierOrigin_.y) / barrierCell_);
    if (gx < 0 || gy < 0 || gx >= barrierGW_ || gy >= barrierGH_) return false;
    for (int wi : barrierGrid_[(size_t)gy * barrierGW_ + gx]) {
        const Barrier& w = barriers_[wi];
        if (length(p - closestOnSegment(p, w.a, w.b)) < w.r + margin) return true;
    }
    return false;
}

void World::updateFood() {
    // Mobile food: cells shove the pieces they touch.
    if (!settings.mobileFood || food_.empty()) return;
    const float R = settings.dishRadius;
    for (Food& f : food_) {
        float fr = foodRadius(f.mass);
        Vec2 push{};
        forNeighbors(f.pos, fr + gridCell_ * 0.5f, [&](int j) {
            const Cell& c = cells_[j];
            if (!c.alive) return;
            Vec2 d = f.pos - c.pos;
            float rs = c.radius + fr;
            float d2 = length2(d);
            if (d2 < rs * rs && d2 > 1e-6f) {
                float dist = std::sqrt(d2);
                push += d / dist * (rs - dist) * 0.3f + c.vel * 0.3f;
            }
        });
        f.vel = (f.vel + push) * 0.8f;
        Vec2 np = f.pos + f.vel;
        float pl = length(np);
        if (pl > R - fr) np = np * ((R - fr) / pl);
        if (barrierAt(np, fr)) {
            f.vel = {};
            continue;
        }
        f.pos = np;
    }
}

void World::updateFluids() {
    if (fluids_.empty()) return;
    const float h = kHoursPerTick;
    const float R = settings.dishRadius;
    for (Fluid& f : fluids_) {
        f.age += h;
        f.vel *= 0.85f;
        Vec2 np = f.pos + f.vel;
        if (length(np) < R && !barrierAt(np, 0.0f)) f.pos = np;
        else f.vel = {};
        f.radius = 3.0f + std::sqrt(kFluidDiffusion * f.age);
        f.amount *= std::exp(-fluidDecay(f.kind) * h);
        if (previewMode) continue;
        // Cells inside the cloud take some of it up: phagocytes drink nutrients greedily, others by pinocytosis.
        forNeighbors(f.pos, f.radius + gridCell_ * 0.5f, [&](int j) {
            Cell& c = cells_[j];
            if (!c.alive || f.amount <= 0) return;
            float d = length(c.pos - f.pos);
            float reach = f.radius + c.radius;
            if (d >= reach) return;
            float overlap = 1.0f - d / reach;
            float area = std::min(1.0f, c.radius * c.radius / (f.radius * f.radius));  // share of the cloud it covers
            float rate;
            switch (f.kind) {
                case FluidKind::Sap:
                case FluidKind::Broth: rate = c.type == CellType::Phagocyte ? 8.0f : 1.0f; break;
                case FluidKind::Toxin:
                case FluidKind::Neurotoxin: rate = c.isProtected ? 0.6f : 3.0f; break;
                default: rate = 3.0f; break;
            }
            float take = std::min(f.amount, f.amount * overlap * area * rate * h * 4.0f);
            if (take <= 0) return;
            // Эдрофоний нейтрализует тетрадоксин: находясь в одной клетке, оба вещества быстро разрушают друг друга.
            if (f.kind == FluidKind::Neurotoxin && c.edrophonium > 0.0f) {
                const float eaten = std::min(take, kEdrophoniumDecay * c.edrophonium * h);
                c.edrophonium = std::max(0.0f, c.edrophonium - eaten * 0.5f);
                take -= eaten;
                if (take <= 0) return;
            }
            switch (f.kind) {
                case FluidKind::Sap:
                case FluidKind::Broth: c.mass = std::min(c.mass + take, capacityFor(c)); break;
                case FluidKind::Toxin:
                    // Гранулоцит собирает яд из облака вместо того, чтобы его переносить.
                    if (c.type == CellType::Granulocyte) c.granulocyteToxicity += take;
                    else c.injury += take;  // ~1 ng is lethal
                    break;
                case FluidKind::Neurotoxin: c.paralysis += take * 2.0f; break;
                case FluidKind::Stimulant: c.stimulant += take * 2.0f; break;
                case FluidKind::Enzyme:
                    for (int k = c.linkCount - 1; k >= 0; --k)
                        if (rng_.chance(std::min(1.0f, take * 5.0f))) removeLink(c.links[k]);
                    c.injury += take * 0.1f;
                    break;
                case FluidKind::Signal: c.signals[f.channel] = clampv(c.signals[f.channel] + take * 50.0f, -100.0f, 100.0f); break;
                default: break;
            }
            f.amount -= take;
        });
    }
    fluids_.erase(std::remove_if(fluids_.begin(), fluids_.end(), [](const Fluid& f) { return f.amount < 0.002f || f.radius > 220.0f; }),
                  fluids_.end());
}

void World::spawnFood() {
    const float R = settings.dishRadius;
    if (settings.cellLabPhysics) {
        // Cell Lab: nutrient rate × (R / 500 µm)² × 10 ng per hour, in lumps of Poisson(lumpiness + 1) chunks scattered
        // around a random point (lump size × 500 µm); every chunk has the coating of the setting.
        const float chunk = std::max(settings.chunkSize, 0.0216f);
        const float lumpN = settings.lumpiness + 1.0f;
        foodAccumulator_ += settings.nutrientRate * (R / 500.0f) * (R / 500.0f) / chunk / lumpN * kHoursPerTick;
        auto poisson = [&](float lambda) {
            if (lambda > 30.0f) return std::max(0, (int)std::lround(lambda + rng_.normal() * std::sqrt(lambda)));
            float L = std::exp(-lambda), p = 1.0f;
            int k = 0;
            do {
                ++k;
                p *= rng_.uniform();
            } while (p > L && k < 200);
            return k - 1;
        };
        while (foodAccumulator_ >= 1.0) {
            foodAccumulator_ -= 1.0;
            float a = rng_.range(0, kTau), r = std::sqrt(rng_.uniform()) * R;
            const Vec2 centre = fromAngle(a) * r;
            const int pieces = settings.lumpiness > 0 ? poisson(lumpN) : 1;
            const float mass = chunk * kFoodUnit, fr = foodRadius(mass);
            for (int k = 0; k < pieces; ++k) {
                if ((int)food_.size() >= settings.maxFood) {
                    foodAccumulator_ = 0;
                    return;
                }
                Vec2 p = centre + Vec2{rng_.normal(), rng_.normal()} * (settings.lumpSize * 500.0f);
                float pl = length(p);
                if (pl > R - fr) p = p * ((R - fr) / std::max(pl, 1e-3f));
                if (barrierAt(p, fr)) continue;
                food_.push_back(Food{p, {}, mass, settings.coating});
            }
        }
        return;
    }
    float areaScale = (R * R) / (900.0f * 900.0f);
    foodAccumulator_ += settings.nutrientRate * 300.0f * areaScale * kHoursPerTick;
    if (lumps_.empty() || rng_.chance(0.02f * kHoursPerTick * 60)) {
        lumps_.clear();
        for (int i = 0; i < 4; ++i) {
            float a = rng_.range(0, kTau), r = std::sqrt(rng_.uniform()) * R * 0.8f;
            lumps_.push_back(fromAngle(a) * r);
        }
    }
    while (foodAccumulator_ >= 1.0) {
        foodAccumulator_ -= 1.0;
        if ((int)food_.size() >= settings.maxFood) {
            foodAccumulator_ = 0;
            break;
        }
        Vec2 p;
        if (rng_.uniform() * 100.0f < settings.lumpiness) {
            Vec2 c = lumps_[rng_.irange(0, (int)lumps_.size() - 1)];
            p = c + Vec2{rng_.normal(), rng_.normal()} * (settings.lumpSize * R);
        } else {
            float a = rng_.range(0, kTau), r = std::sqrt(rng_.uniform()) * R;
            p = fromAngle(a) * r;
        }
        float mass = settings.chunkSize * kFoodUnit;
        float pl = length(p);
        float fr = foodRadius(mass);
        if (pl > R - fr) p = p * ((R - fr) / std::max(pl, 1e-3f));
        float coat = rng_.uniform() < settings.coating ? 1.0f : 0.0f;
        if (barrierAt(p, fr)) continue;
        Food f{p, {}, mass, coat};
        food_.push_back(f);
    }
}

void World::recordStats() {
    Stats s{};
    s.time = (float)time_;
    std::fill(std::begin(typeCounts_), std::end(typeCounts_), 0);
    for (const Cell& c : cells_) {
        if (!c.alive) continue;
        s.cells++;
        s.biomass += c.mass;
        typeCounts_[(int)c.type]++;
    }
    std::memcpy(s.perType, typeCounts_, sizeof(typeCounts_));
    s.food = (int)food_.size();
    history_.push_back(s);
    if (history_.size() > 4000) {
        // Halve resolution of the older half to keep the full timeline.
        std::vector<Stats> h;
        h.reserve(3000);
        for (size_t i = 0; i < 2000; i += 2) h.push_back(history_[i]);
        for (size_t i = 2000; i < history_.size(); ++i) h.push_back(history_[i]);
        history_.swap(h);
    }
}
