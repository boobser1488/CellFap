#include "mod_api.hpp"

#include "mods.hpp"
#include "serialize.hpp"
#include "signal_cells.hpp"  // signalCellSourceProblem
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

// The SDK repeats the game's enums as plain numbers: they must not drift apart.
static_assert(CELLFAP_ELECTROCYTE == (int)CellType::Electrocyte && CELLFAP_GRAPPLER == (int)CellType::Grappler &&
              CELLFAP_SIGNAL_CELL == (int)CellType::SignalCell && CELLFAP_CILIOCYTE == (int)CellType::Ciliocyte);
static_assert(kCellTypeCount <= 64, "CellFapMod.cell_types is a 64-bit mask");
static_assert(CELLFAP_INJECT_MUTAGEN == (int)World::Substance::Mutagen &&
              CELLFAP_INJECT_ATRACTYLOSIDE == (int)World::Substance::Atractyloside &&
              CELLFAP_INJECT_ATRACTYLOSIDE + 1 == (int)World::Substance::Count);
static_assert(PF_Count == 11 && kSignalCount == 8 && (int)ProgInput::Injury == 15);
static_assert(CELLFAP_EVENT_BORN == (int)World::CellEvent::Born && CELLFAP_EVENT_DIVIDED == (int)World::CellEvent::Divided &&
              CELLFAP_EVENT_DIED == (int)World::CellEvent::Died);

// ---------------------------------------------------------------- dish parameters

namespace {

#define P_F(name, ru, field, lo, hi) {name, ru, DishParam::Float, offsetof(Settings, field), lo, hi, true}
#define P_I(name, ru, field, lo, hi) {name, ru, DishParam::Int, offsetof(Settings, field), lo, hi, true}
#define P_B(name, ru, field) {name, ru, DishParam::Bool, offsetof(Settings, field), 0, 1, true}
const DishParam kDishParams[] = {
    // The first nine are the dypt mods' original names (узнать / задать).
    P_F("light", "свет", lightAmount, 0.0, 2.0),
    P_F("nutrient_rate", "питание", nutrientRate, 0.0, 15.0),
    P_F("radiation", "радиация", radiation, 0.0, 0.09),
    P_F("viscosity", "вязкость", viscosity, 0.0, 150.0),
    P_F("gravity", "гравитация", gravity, 0.0, 1.0),
    P_F("salinity", "солёность", salinity, 0.0, 1.0),
    P_F("nitrates", "нитраты", nitrates, 0.0, 1.0),
    P_F("day_length", "день", dayLength, 0.0, 48.0),
    P_F("light_angle", "направление_света", lightAngle, -1000.0, 1000.0),
    {"dish_radius", "радиус", DishParam::Float, offsetof(Settings, dishRadius), 50.0, 20000.0, false},
    P_I("max_cells", "предел_клеток", maxCells, 1, 200000),
    P_I("max_food", "предел_пищи", maxFood, 0, 200000),
    P_B("lid", "крышка", lid),
    P_B("contaminate", "заражение", contaminate),
    P_B("kill_at_edge", "смерть_у_края", killAtEdge),
    P_B("only_point_mutations", "точечные_мутации", onlyPointMutations),
    P_B("cell_aging", "старение", cellAging),
    P_B("mobile_food", "подвижная_пища", mobileFood),
    P_F("dyn_friction", "трение", dynFriction, 0.0, 8.0),
    P_F("static_friction", "трение_покоя", staticFriction, 0.0, 8.0),
    P_F("chunk_size", "порция_пищи", chunkSize, 0.022, 1.152),
    P_F("lumpiness", "комковатость", lumpiness, 0.0, 100.0),
    P_F("lump_size", "размер_комков", lumpSize, 0.01, 0.2),
    P_F("coating", "оболочка_пищи", coating, 0.0, 1.0),
    P_F("light_range", "разброс_света", lightRange, 0.0, 1.0),
    P_F("light_dir_change", "вращение_света", lightDirChange, 0.0, 1.0),
    P_F("density", "плотность", density, -3.3, 3.3),
    P_F("density_gradient", "градиент_плотности", densityGradient, -2.0, 0.0),
    P_F("swim_anisotropy", "анизотропия", swimAnisotropy, 1.0, 6.0),
    P_F("mechanical_damage", "травмы", mechanicalDamage, 0.0, 1.0),
    P_F("sap_leak", "сок", sapLeak, 0.0, 2.0),
    P_F("light_capacity", "ёмкость_света", lightCapacity, 0.02, 1.0),
    P_B("cell_lab_physics", "физика_cell_lab", cellLabPhysics),
    P_I("hyaluronic_mode", "режим_гиалуроновой", hyaluronicMode, 0, 2),
};
#undef P_F
#undef P_I
#undef P_B

}  // namespace

const DishParam* findDishParam(const std::string& name) {
    const std::string n = name == "соленость" ? std::string("солёность") : name;
    for (const DishParam& p : kDishParams)
        if (n == p.name || (p.ru && n == p.ru)) return &p;
    return nullptr;
}
const DishParam* dishParamAt(int i) {
    return i >= 0 && i < (int)(sizeof(kDishParams) / sizeof(kDishParams[0])) ? &kDishParams[i] : nullptr;
}
double getDishParam(const Settings& s, const DishParam& p) {
    const char* base = reinterpret_cast<const char*>(&s) + p.offset;
    switch (p.kind) {
        case DishParam::Float: return *reinterpret_cast<const float*>(base);
        case DishParam::Int: return *reinterpret_cast<const int*>(base);
        default: return *reinterpret_cast<const bool*>(base) ? 1.0 : 0.0;
    }
}
void setDishParam(Settings& s, const DishParam& p, double v) {
    char* base = reinterpret_cast<char*>(&s) + p.offset;
    v = std::clamp(v, p.lo, p.hi);
    switch (p.kind) {
        case DishParam::Float: *reinterpret_cast<float*>(base) = (float)v; break;
        case DishParam::Int: *reinterpret_cast<int*>(base) = (int)std::lround(v); break;
        default: *reinterpret_cast<bool*>(base) = v >= 0.5; break;
    }
}

// ---------------------------------------------------------------- conversions

void cellToApi(const World& w, int i, CellFapCell& o) {
    const Cell& c = w.cells()[i];
    o = CellFapCell{};
    o.id = c.id;
    o.alive = c.alive ? 1 : 0;
    o.type = (int32_t)c.type;
    o.mode = c.mode;
    o.genome = c.genome;
    o.link_count = c.linkCount;
    o.radius = c.radius;
    o.actuator = c.actuator;
    o.x = c.pos.x;
    o.y = c.pos.y;
    o.vx = c.vel.x;
    o.vy = c.vel.y;
    o.angle = c.angle;
    o.angular_velocity = c.angVel;
    o.mass = c.mass;
    o.age = c.age;
    o.nitrogen = c.nitrogen;
    o.injury = c.injury;
    o.tag = (int32_t)c.tag;
    o.mirrored = c.mirrored ? 1 : 0;
    o.telomeres = c.telomeres;
    o.mutations = c.mutations;
    for (int k = 0; k < kSignalCount; ++k) o.signals[k] = c.signals[k];
    o.charge = c.charge;
    o.stun = c.stun;
    o.paralysis = c.paralysis;
    o.stimulant = c.stimulant;
    const float subs[CELLFAP_SUB_COUNT] = {c.amylase,     c.hyaluronicAcid, c.epibrassinolide, c.benzyladin,  c.gibberellicAcid,
                                           c.colchicine,  c.penicillin,     c.interferon,      c.resveratrol, c.ryanodine,
                                           c.cycloheximide, c.osteopontin,  c.chondroitinSulfate, c.edrophonium, c.atractyloside};
    std::memcpy(o.substances, subs, sizeof subs);
    for (int k = 0; k < 4; ++k) o.memory[k] = c.progMem[k];
}

namespace {

bool finite(float v) { return std::isfinite(v); }

// The writable fields of `in` into cell i; false (nothing changed) when one of them is not a number.
bool cellFromApi(World& w, int i, const CellFapCell& in) {
    const float vals[] = {in.x, in.y, in.vx, in.vy, in.angle, in.angular_velocity, in.mass, in.age, in.nitrogen, in.injury,
                          in.charge, in.stun, in.paralysis, in.stimulant};
    for (float v : vals)
        if (!finite(v)) return false;
    for (float v : in.signals)
        if (!finite(v)) return false;
    for (float v : in.substances)
        if (!finite(v)) return false;
    for (float v : in.memory)
        if (!finite(v)) return false;
    Cell& c = w.cellsMut()[i];
    const Vec2 np{in.x, in.y};
    // A jump (not a nudge): no streak drawn from the old place to the new one.
    if (length(np - c.pos) > c.radius * 2.0f) c.prevPos = np;
    c.pos = np;
    c.vel = {in.vx, in.vy};
    c.angle = in.angle;
    c.angVel = in.angular_velocity;
    const float mass = std::clamp(in.mass, 0.0f, 40.0f);
    if (mass != c.mass) {
        c.mass = mass;
        c.radius = radiusForMass(mass, c.type);
    }
    c.age = std::max(0.0f, in.age);
    c.nitrogen = std::clamp(in.nitrogen, 0.0f, 1.0f);
    c.injury = std::clamp(in.injury, 0.0f, 1.0f);
    c.tag = (CellTag)std::clamp(in.tag, 0, (int)CellTag::Hybrid);
    c.mirrored = in.mirrored != 0;
    c.telomeres = (int8_t)std::clamp(in.telomeres, -1, 127);
    c.mutations = (uint16_t)std::clamp(in.mutations, 0, 65535);
    for (int k = 0; k < kSignalCount; ++k) c.signals[k] = std::clamp(in.signals[k], -1e6f, 1e6f);
    c.charge = std::clamp(in.charge, 0.0f, 1.0f);
    c.stun = std::max(0.0f, in.stun);
    c.paralysis = std::max(0.0f, in.paralysis);
    c.stimulant = std::max(0.0f, in.stimulant);
    float* subs[CELLFAP_SUB_COUNT] = {&c.amylase,     &c.hyaluronicAcid, &c.epibrassinolide, &c.benzyladin,  &c.gibberellicAcid,
                                      &c.colchicine,  &c.penicillin,     &c.interferon,      &c.resveratrol, &c.ryanodine,
                                      &c.cycloheximide, &c.osteopontin,  &c.chondroitinSulfate, &c.edrophonium, &c.atractyloside};
    for (int k = 0; k < CELLFAP_SUB_COUNT; ++k) *subs[k] = std::max(0.0f, in.substances[k]);
    for (int k = 0; k < 4; ++k) c.progMem[k] = in.memory[k];
    return true;
}

}  // namespace

void modeToApi(const Mode& m, CellFapMode& o) {
    o = CellFapMode{};
    o.type = (int32_t)m.type;
    o.color[0] = m.color.r;
    o.color[1] = m.color.g;
    o.color[2] = m.color.b;
    o.make_adhesin = m.makeAdhesin;
    o.prioritize = m.prioritize;
    o.split_mass = m.splitMass;
    o.split_ratio = m.splitRatio;
    o.nutrient_priority = m.nutrientPriority;
    o.split_angle = m.splitAngle;
    for (int k = 0; k < 2; ++k) {
        o.child_angle[k] = m.childAngle[k];
        o.child_mode[k] = m.childMode[k];
        o.keep_adhesin[k] = m.keepAdhesin[k];
        o.mirror[k] = m.mirror[k];
        o.stem_mode[k] = m.stemMode[k];
    }
    o.adhesin_stiffness = m.adhesinStiffness;
    o.cytoskeleton = m.cytoskeleton;
    o.adhesin_length = m.adhesinLength;
    o.max_connections = m.maxConnections;
    for (int k = 0; k < PF_Count; ++k) o.prog[k] = {(int32_t)m.prog[k].input, (int32_t)m.prog[k].kind, m.prog[k].a, m.prog[k].b, m.prog[k].c};
    for (int k = 0; k < 4; ++k) o.neuro_channel[k] = m.neuroChannel[k];
    o.virus_copy_from = m.virusCopyFrom;
    o.sense_target = (int32_t)m.senseTarget;
    o.sense_channel = m.senseChannel;
    o.sense_output = m.senseOutput;
    o.sense_threshold = m.senseThreshold;
    o.sense_color[0] = m.senseColor.r;
    o.sense_color[1] = m.senseColor.g;
    o.sense_color[2] = m.senseColor.b;
    o.secretion = (int32_t)m.secretion;
    o.gamete_compatible_mode = m.gameteCompatibleMode;
    o.gamete_mode_after = m.gameteModeAfter;
    o.gamete_telomeres = m.gameteTelomeres;
}

bool modeFromApi(const CellFapMode& in, Mode& m) {
    auto inRange = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
    const float fl[] = {in.color[0], in.color[1], in.color[2], in.split_mass, in.split_ratio, in.nutrient_priority, in.split_angle,
                        in.child_angle[0], in.child_angle[1], in.adhesin_stiffness, in.cytoskeleton, in.adhesin_length,
                        in.sense_output, in.sense_threshold, in.sense_color[0], in.sense_color[1], in.sense_color[2]};
    for (float v : fl)
        if (!finite(v)) return false;
    if (!inRange(in.type, 0, kCellTypeCount - 1) || !inRange(in.sense_target, 0, (int)SenseTarget::Count - 1) ||
        !inRange(in.secretion, 0, (int)Secretion::Count - 1) || !inRange(in.sense_channel, 0, kSignalCount - 1) ||
        !inRange(in.virus_copy_from, 0, kMaxModes - 1) || !inRange(in.gamete_compatible_mode, 0, kMaxModes - 1) ||
        !inRange(in.gamete_mode_after, 0, kMaxModes - 1) || !inRange(in.gamete_telomeres, 0, 5) ||
        !inRange(in.max_connections, 1, kMaxLinks))
        return false;
    for (int k = 0; k < 2; ++k)
        if (!inRange(in.child_mode[k], 0, kMaxModes - 1) || !inRange(in.stem_mode[k], 0, kMaxModes - 1)) return false;
    for (int k = 0; k < 4; ++k)
        if (!inRange(in.neuro_channel[k], 0, kSignalCount - 1)) return false;
    for (const CellFapProg& p : in.prog)
        if (!inRange(p.input, 0, (int)ProgInput::Count - 1) || !inRange(p.kind, 0, (int)ProgKind::Count - 1) || !finite(p.a) ||
            !finite(p.b) || !finite(p.c))
            return false;
    m.type = (CellType)in.type;
    m.color = {std::clamp(in.color[0], 0.0f, 1.0f), std::clamp(in.color[1], 0.0f, 1.0f), std::clamp(in.color[2], 0.0f, 1.0f)};
    m.makeAdhesin = in.make_adhesin != 0;
    m.prioritize = in.prioritize != 0;
    m.splitMass = in.split_mass;
    m.splitRatio = std::clamp(in.split_ratio, 0.05f, 0.95f);
    m.nutrientPriority = std::clamp(in.nutrient_priority, 0.0f, 10.0f);
    m.splitAngle = in.split_angle;
    for (int k = 0; k < 2; ++k) {
        m.childAngle[k] = in.child_angle[k];
        m.childMode[k] = (uint8_t)in.child_mode[k];
        m.keepAdhesin[k] = in.keep_adhesin[k] != 0;
        m.mirror[k] = in.mirror[k] != 0;
        m.stemMode[k] = (uint8_t)in.stem_mode[k];
    }
    m.adhesinStiffness = std::clamp(in.adhesin_stiffness, 0.0f, 1.0f);
    m.cytoskeleton = std::clamp(in.cytoskeleton, 0.1f, 4.0f);
    m.adhesinLength = std::clamp(in.adhesin_length, 0.0f, 1.0f);
    m.maxConnections = in.max_connections;
    for (int k = 0; k < PF_Count; ++k)
        m.prog[k] = {(ProgInput)in.prog[k].input, (ProgKind)in.prog[k].kind, in.prog[k].a, in.prog[k].b, in.prog[k].c};
    for (int k = 0; k < 4; ++k) m.neuroChannel[k] = (uint8_t)in.neuro_channel[k];
    m.virusCopyFrom = (uint8_t)in.virus_copy_from;
    m.senseTarget = (SenseTarget)in.sense_target;
    m.senseChannel = (uint8_t)in.sense_channel;
    m.senseOutput = std::clamp(in.sense_output, -60.0f, 60.0f);
    m.senseThreshold = std::clamp(in.sense_threshold, 0.0f, 1.7320508f);
    m.senseColor = {std::clamp(in.sense_color[0], 0.0f, 1.0f), std::clamp(in.sense_color[1], 0.0f, 1.0f),
                    std::clamp(in.sense_color[2], 0.0f, 1.0f)};
    m.secretion = (Secretion)in.secretion;
    m.gameteCompatibleMode = (uint8_t)in.gamete_compatible_mode;
    m.gameteModeAfter = (uint8_t)in.gamete_mode_after;
    m.gameteTelomeres = (uint8_t)in.gamete_telomeres;
    return true;
}

// ---------------------------------------------------------------- the table
namespace {

constexpr int kMaxGenomesPerMod = 64;

bool here(const std::thread::id& t) { return t == std::this_thread::get_id(); }

// The world of the running callback, or null (wrong time, wrong thread, no dish).
World* worldOf(CellFapWorld* h) { return h && h->active && here(h->thread) ? h->world : nullptr; }

// The mod behind a CellFapMod, while one of its calls runs on this thread.
NativeMod* modOf(CellFapMod* m) {
    NativeMod* nm = m ? static_cast<NativeMod*>(m->host) : nullptr;
    if (!nm || !nm->mgr || &nm->mod != m || !nm->mgr->inCallback()) return nullptr;
    return nm;
}

bool cellOk(const World& w, int32_t i) { return i >= 0 && i < (int32_t)w.cells().size() && w.cells()[i].alive; }
bool genomeOk(const World& w, int32_t g) {
    return g >= 0 && g < (int32_t)w.genomes().genomes.size() && w.genomes().refs[g] > 0;
}

#define WORLD(h)                       \
    World* wp = worldOf(h);            \
    if (!wp) return CELLFAP_E_STATE;   \
    World& w = *wp

// -- the mod itself
void apiLog(CellFapMod* m, const char* text) {
    if (NativeMod* nm = modOf(m); nm && text) nm->mgr->addLog(nm->index, text);
}
void apiMessage(CellFapMod* m, const char* text) {
    if (NativeMod* nm = modOf(m); nm && text) nm->mgr->addMessage(nm->index, std::string(text).substr(0, 300));
}
void apiSetError(CellFapMod* m, const char* text) {
    if (NativeMod* nm = modOf(m)) nm->error = text ? std::string(text).substr(0, 500) : std::string();
}
double apiSetting(CellFapMod* m, const char* key, double fallback) {
    NativeMod* nm = modOf(m);
    if (!nm || !key) return fallback;
    for (const ModSetting& s : nm->mgr->mods[nm->index].settings)
        if (s.key == key) return s.value;
    return fallback;
}
int apiSetSetting(CellFapMod* m, const char* key, double v) {
    NativeMod* nm = modOf(m);
    if (!nm) return CELLFAP_E_STATE;
    if (!key || !std::isfinite(v)) return CELLFAP_E_ARG;
    for (ModSetting& s : nm->mgr->mods[nm->index].settings)
        if (s.key == key) {
            if (s.kind == ModSetting::Number) s.value = std::clamp(v, s.min, s.max);
            else if (s.kind == ModSetting::Flag) s.value = v != 0 ? 1 : 0;
            else s.value = std::clamp(std::floor(v), 0.0, (double)s.options.size() - 1);
            nm->mgr->settingsEdited(nm->index);
            return CELLFAP_OK;
        }
    return CELLFAP_E_NOT_FOUND;
}
int apiModFile(CellFapMod* m, const char* path, const void** data, size_t* size) {
    NativeMod* nm = modOf(m);
    if (!nm) return CELLFAP_E_STATE;
    if (!path || !data || !size) return CELLFAP_E_ARG;
    const auto& files = nm->mgr->mods[nm->index].files;
    auto it = files ? files->find(modRelativePath(path)) : ModFiles::const_iterator{};
    if (!files || it == files->end()) return CELLFAP_E_NOT_FOUND;
    *data = it->second.data();
    *size = it->second.size();
    return CELLFAP_OK;
}
int32_t apiRegisterGenome(CellFapMod* m, const char* name, const char* text) {
    NativeMod* nm = modOf(m);
    if (!nm) return CELLFAP_E_STATE;
    if (!text) return CELLFAP_E_ARG;
    if ((int)nm->genomes.size() >= kMaxGenomesPerMod) return CELLFAP_E_FULL;
    ModGenome mg;
    mg.mod = nm->id;
    if (!genomeFromText(mg.genome, text)) return CELLFAP_E_ARG;
    if (name && *name) mg.genome.name = name;
    mg.name = mg.genome.name;
    nm->genomes.push_back(mg.genome);
    nm->mgr->genomes.push_back(std::move(mg));
    nm->mgr->genomesChanged();
    return (int32_t)nm->genomes.size() - 1;
}
int apiRegisterProgram(CellFapMod* m, const char* name, const char* source) {
    NativeMod* nm = modOf(m);
    if (!nm) return CELLFAP_E_STATE;
    if (!name || !source) return CELLFAP_E_ARG;
    int mine = 0;
    for (const ModProgram& p : nm->mgr->programs) mine += p.mod == nm->id;
    if (mine >= kMaxGenomesPerMod) return CELLFAP_E_FULL;
    if (!signalCellSourceProblem(source).empty()) return CELLFAP_E_ARG;
    nm->mgr->programs.push_back({nm->id, name, source});
    nm->mgr->genomesChanged();
    return CELLFAP_OK;
}
int apiRequestDish(CellFapMod* m, int32_t action, const char* file) {
    NativeMod* nm = modOf(m);
    if (!nm) return CELLFAP_E_STATE;
    if ((action != CELLFAP_DISH_SAVE && action != CELLFAP_DISH_LOAD) || !file || !*file) return CELLFAP_E_ARG;
    if (nm->mgr->dishRequests.size() >= 8) return CELLFAP_E_FULL;
    nm->mgr->dishRequests.push_back({nm->id, action == CELLFAP_DISH_SAVE, safeFileName(file, "mod")});
    return CELLFAP_OK;
}
const char* apiCellTypeName(int32_t t) { return t >= 0 && t < kCellTypeCount ? cellTypeInfo(t).latin : nullptr; }
const char* apiParamName(int32_t i) {
    const DishParam* p = dishParamAt(i);
    return p ? p->name : nullptr;
}

// -- the dish
int apiWorldInfo(CellFapWorld* h, CellFapWorldInfo* o) {
    WORLD(h);
    if (!o) return CELLFAP_E_ARG;
    *o = CellFapWorldInfo{};
    o->ticks = w.ticks();
    o->hours = w.timeHours();
    o->cells = w.cellCount();
    o->cell_slots = (int32_t)w.cells().size();
    o->link_slots = (int32_t)w.links().size();
    o->food = (int32_t)w.food().size();
    o->genomes = (int32_t)w.genomes().genomes.size();
    o->cell_type_count = kCellTypeCount;
    o->dish_radius = w.settings.dishRadius;
    o->daylight = w.daylight();
    o->max_cells = w.settings.maxCells;
    o->max_food = w.settings.maxFood;
    return CELLFAP_OK;
}
int apiGetParam(CellFapWorld* h, const char* name, double* v) {
    WORLD(h);
    const DishParam* p = name ? findDishParam(name) : nullptr;
    if (!p || !v) return CELLFAP_E_ARG;
    *v = getDishParam(w.settings, *p);
    return CELLFAP_OK;
}
int apiSetParam(CellFapWorld* h, const char* name, double v) {
    WORLD(h);
    const DishParam* p = name ? findDishParam(name) : nullptr;
    if (!p || !std::isfinite(v)) return CELLFAP_E_ARG;
    if (!p->writable) return CELLFAP_E_STATE;
    setDishParam(w.settings, *p, v);
    return CELLFAP_OK;
}
int apiGetCell(CellFapWorld* h, int32_t i, CellFapCell* o) {
    WORLD(h);
    if (!o) return CELLFAP_E_ARG;
    if (i < 0 || i >= (int32_t)w.cells().size()) return CELLFAP_E_RANGE;
    cellToApi(w, i, *o);  // (a dead slot too: alive == 0)
    return CELLFAP_OK;
}
int apiSetCell(CellFapWorld* h, int32_t i, const CellFapCell* in) {
    WORLD(h);
    if (!in) return CELLFAP_E_ARG;
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    return cellFromApi(w, i, *in) ? CELLFAP_OK : CELLFAP_E_ARG;
}
int32_t apiFindCell(CellFapWorld* h, uint32_t id) {
    WORLD(h);
    const auto& cs = w.cells();
    for (size_t i = 0; i < cs.size(); ++i)
        if (cs[i].alive && cs[i].id == id) return (int32_t)i;
    return -1;
}
int32_t apiPickCell(CellFapWorld* h, float x, float y) {
    WORLD(h);
    if (!finite(x) || !finite(y)) return CELLFAP_E_ARG;
    return w.pickCell({x, y});
}
int32_t apiCellsNear(CellFapWorld* h, float x, float y, float r, int32_t* out, int32_t cap) {
    WORLD(h);
    if (!finite(x) || !finite(y) || !finite(r) || r < 0 || (cap > 0 && !out)) return CELLFAP_E_ARG;
    int32_t n = 0;
    const Vec2 p{x, y};
    w.forEachCellNear(p, r, [&](int j) {
        if (length(w.cells()[j].pos - p) > r) return;
        if (n < cap) out[n] = j;
        ++n;
    });
    return n;
}
int32_t apiAddCell(CellFapWorld* h, int32_t handle, float x, float y, float angle, int32_t mode, float mass) {
    WORLD(h);
    if (!h->mod || handle < 0 || handle >= (int32_t)h->mod->genomes.size()) return CELLFAP_E_NOT_FOUND;
    if (!finite(x) || !finite(y) || !finite(angle) || !finite(mass) || mode < -1 || mode >= kMaxModes) return CELLFAP_E_ARG;
    const int idx = w.addCell(h->mod->genomes[handle], {x, y}, angle, mode, CellTag::User, std::clamp(mass, 0.0f, 40.0f));
    return idx >= 0 ? idx : CELLFAP_E_FULL;
}
int32_t apiAddCellLike(CellFapWorld* h, int32_t src, float x, float y, float angle, int32_t mode, float mass) {
    WORLD(h);
    if (!cellOk(w, src)) return CELLFAP_E_RANGE;
    if (!finite(x) || !finite(y) || !finite(angle) || !finite(mass) || mode < -1 || mode >= kMaxModes) return CELLFAP_E_ARG;
    const int idx = w.addCellLike(src, {x, y}, angle, mode < 0 ? w.cells()[src].mode : mode, mass > 0 ? std::min(mass, 40.0f) : 2.6f,
                                  w.cells()[src].mirrored);
    return idx >= 0 ? idx : CELLFAP_E_FULL;
}
int apiKillCell(CellFapWorld* h, int32_t i, int32_t leaveFood) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    w.removeCell(i, leaveFood != 0);
    return CELLFAP_OK;
}
int apiSetCellMode(CellFapWorld* h, int32_t i, int32_t mode) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    if (mode < 0 || mode >= kMaxModes) return CELLFAP_E_ARG;
    w.setCellMode(i, mode);
    return CELLFAP_OK;
}
int apiInject(CellFapWorld* h, int32_t i, int32_t s, float amount, int32_t channel) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    if (s < 0 || s >= (int)World::Substance::Count || !finite(amount) || channel < 0 || channel >= kSignalCount) return CELLFAP_E_ARG;
    w.injectSubstance(i, (World::Substance)s, amount, channel);
    return CELLFAP_OK;
}
int apiRelease(CellFapWorld* h, float x, float y, int32_t s, float amount, int32_t channel) {
    WORLD(h);
    if (s < 0 || s >= (int)World::Substance::Count || !finite(x) || !finite(y) || !finite(amount) || amount < 0 || channel < 0 ||
        channel >= kSignalCount)
        return CELLFAP_E_ARG;
    return w.releaseSubstance({x, y}, (World::Substance)s, amount, channel) ? CELLFAP_OK : CELLFAP_E_ARG;
}

int apiGetLink(CellFapWorld* h, int32_t li, CellFapLink* o) {
    WORLD(h);
    if (!o) return CELLFAP_E_ARG;
    if (li < 0 || li >= (int32_t)w.links().size()) return CELLFAP_E_RANGE;
    const Link& l = w.links()[li];
    *o = CellFapLink{};
    o->alive = l.alive;
    o->a = l.a;
    o->b = l.b;
    o->from_division = l.fromDivision;
    o->extra_length = l.extraLength;
    o->stiffness = l.stiffness;
    o->angle_a = l.angA;
    o->angle_b = l.angB;
    o->wall_x = l.wallPoint.x;
    o->wall_y = l.wallPoint.y;
    return CELLFAP_OK;
}
int32_t apiCellLinks(CellFapWorld* h, int32_t i, int32_t* out, int32_t cap) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    if (cap > 0 && !out) return CELLFAP_E_ARG;
    const Cell& c = w.cells()[i];
    for (int k = 0; k < c.linkCount && k < cap; ++k) out[k] = c.links[k];
    return c.linkCount;
}
int32_t apiAddLink(CellFapWorld* h, int32_t a, int32_t b, float extra, float stiffness) {
    WORLD(h);
    if (!cellOk(w, a) || !cellOk(w, b)) return CELLFAP_E_RANGE;
    if (a == b || !finite(extra) || !finite(stiffness)) return CELLFAP_E_ARG;
    const int li = w.linkCells(a, b, std::clamp(extra, 0.0f, 60.0f), stiffness);
    return li >= 0 ? li : CELLFAP_E_FULL;
}
int apiSetLink(CellFapWorld* h, int32_t li, float extra, float stiffness) {
    WORLD(h);
    if (li < 0 || li >= (int32_t)w.links().size() || !w.links()[li].alive) return CELLFAP_E_RANGE;
    if (!finite(extra) || !finite(stiffness)) return CELLFAP_E_ARG;
    Link& l = w.linksMut()[li];
    l.extraLength = std::clamp(extra, 0.0f, 200.0f);
    l.stiffness = std::clamp(stiffness, 0.0f, 1.0f);
    return CELLFAP_OK;
}
int apiRemoveLink(CellFapWorld* h, int32_t li) {
    WORLD(h);
    if (li < 0 || li >= (int32_t)w.links().size() || !w.links()[li].alive) return CELLFAP_E_RANGE;
    w.unlink(li);
    return CELLFAP_OK;
}

int apiGetFood(CellFapWorld* h, int32_t i, CellFapFood* o) {
    WORLD(h);
    if (!o) return CELLFAP_E_ARG;
    if (i < 0 || i >= (int32_t)w.food().size()) return CELLFAP_E_RANGE;
    const Food& f = w.food()[i];
    *o = {f.pos.x, f.pos.y, f.vel.x, f.vel.y, f.mass, f.coating};
    return CELLFAP_OK;
}
int apiSetFood(CellFapWorld* h, int32_t i, const CellFapFood* in) {
    WORLD(h);
    if (!in) return CELLFAP_E_ARG;
    if (i < 0 || i >= (int32_t)w.food().size()) return CELLFAP_E_RANGE;
    if (!finite(in->x) || !finite(in->y) || !finite(in->vx) || !finite(in->vy) || !finite(in->mass) || !finite(in->coating))
        return CELLFAP_E_ARG;
    Food& f = w.foodMut()[i];
    f.pos = {in->x, in->y};
    f.vel = {in->vx, in->vy};
    f.mass = std::clamp(in->mass, 0.0f, 50.0f);
    f.coating = std::clamp(in->coating, 0.0f, 1.0f);
    return CELLFAP_OK;
}
int apiAddFood(CellFapWorld* h, float x, float y, float mass) {
    WORLD(h);
    if (!finite(x) || !finite(y) || !finite(mass) || mass <= 0) return CELLFAP_E_ARG;
    const float R = w.settings.dishRadius - 5.0f;
    if (x * x + y * y > R * R) return CELLFAP_E_ARG;
    if ((int)w.food().size() >= w.settings.maxFood) return CELLFAP_E_FULL;
    w.addFood({x, y}, std::min(mass, 50.0f));
    return CELLFAP_OK;
}
int apiRemoveFood(CellFapWorld* h, int32_t i) {
    WORLD(h);
    auto& food = w.foodMut();
    if (i < 0 || i >= (int32_t)food.size()) return CELLFAP_E_RANGE;
    food[i] = food.back();
    food.pop_back();
    return CELLFAP_OK;
}

int apiGenomeText(CellFapWorld* h, int32_t g, char* buf, size_t cap, size_t* needed) {
    WORLD(h);
    if (!genomeOk(w, g)) return CELLFAP_E_RANGE;
    if (cap > 0 && !buf) return CELLFAP_E_ARG;
    const std::string text = genomeToText(w.genomes().get(g));
    if (needed) *needed = text.size();
    if (cap > 0) {
        const size_t n = std::min(cap - 1, text.size());
        std::memcpy(buf, text.data(), n);
        buf[n] = 0;
    }
    return CELLFAP_OK;
}
int apiGetMode(CellFapWorld* h, int32_t g, int32_t mode, CellFapMode* o) {
    WORLD(h);
    if (!genomeOk(w, g)) return CELLFAP_E_RANGE;
    if (!o || mode < 0 || mode >= kMaxModes) return CELLFAP_E_ARG;
    modeToApi(w.genomes().get(g).modes[mode], *o);
    return CELLFAP_OK;
}
int apiSetCellGenome(CellFapWorld* h, int32_t i, const char* text, int32_t whole) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    Genome g;
    if (!text || !genomeFromText(g, text)) return CELLFAP_E_ARG;
    w.injectGenome(i, g, -1, whole != 0);
    return CELLFAP_OK;
}
int apiEditCellMode(CellFapWorld* h, int32_t i, int32_t mode, const CellFapMode* in, int32_t whole) {
    WORLD(h);
    if (!cellOk(w, i)) return CELLFAP_E_RANGE;
    if (!in || mode < 0 || mode >= kMaxModes) return CELLFAP_E_ARG;
    Genome g = w.genomes().get(w.cells()[i].genome);  // (copy-on-write: only the edited mode is new)
    Mode m = g.modes[mode];
    if (!modeFromApi(*in, m)) return CELLFAP_E_ARG;
    g.modes.set(mode, m);
    w.injectGenome(i, g, -1, whole != 0);
    return CELLFAP_OK;
}

// -- the panel
ModUi* uiOf(CellFapUi* u) { return u && u->active && here(u->thread) ? u->ui : nullptr; }
void apiUiText(CellFapUi* u, const char* s) {
    if (ModUi* ui = uiOf(u); ui && s) ui->text(s);
}
void apiUiTextColored(CellFapUi* u, uint32_t rgba, const char* s) {
    if (ModUi* ui = uiOf(u); ui && s) ui->textColored(rgba, s);
}
void apiUiHeading(CellFapUi* u, const char* s) {
    if (ModUi* ui = uiOf(u); ui && s) ui->heading(s);
}
void apiUiSeparator(CellFapUi* u) {
    if (ModUi* ui = uiOf(u)) ui->separator();
}
void apiUiSameLine(CellFapUi* u) {
    if (ModUi* ui = uiOf(u)) ui->sameLine();
}
int apiUiButton(CellFapUi* u, const char* label) {
    ModUi* ui = uiOf(u);
    return ui && label && ui->button(label) ? 1 : 0;
}
int apiUiCheckbox(CellFapUi* u, const char* label, int32_t* v) {
    ModUi* ui = uiOf(u);
    return ui && label && v && ui->checkbox(label, v) ? 1 : 0;
}
int apiUiSliderFloat(CellFapUi* u, const char* label, float* v, float lo, float hi) {
    ModUi* ui = uiOf(u);
    if (!ui || !label || !v || !finite(lo) || !finite(hi) || !(hi > lo)) return 0;
    if (!finite(*v)) *v = lo;
    return ui->sliderFloat(label, v, lo, hi) ? 1 : 0;
}
int apiUiSliderInt(CellFapUi* u, const char* label, int32_t* v, int32_t lo, int32_t hi) {
    ModUi* ui = uiOf(u);
    return ui && label && v && hi > lo && ui->sliderInt(label, v, lo, hi) ? 1 : 0;
}
int apiUiInputText(CellFapUi* u, const char* label, char* buf, size_t cap) {
    ModUi* ui = uiOf(u);
    return ui && label && buf && cap > 0 && ui->inputText(label, buf, cap) ? 1 : 0;
}

// -- over the dish
ModDraw* drawOf(CellFapDraw* d) { return d && d->active && here(d->thread) ? d->draw : nullptr; }
void apiDrawLine(CellFapDraw* d, float x0, float y0, float x1, float y1, uint32_t rgba, float width) {
    if (ModDraw* dr = drawOf(d); dr && finite(x0) && finite(y0) && finite(x1) && finite(y1) && finite(width))
        dr->line(x0, y0, x1, y1, rgba, std::clamp(width, 0.5f, 20.0f));
}
void apiDrawCircle(CellFapDraw* d, float x, float y, float r, uint32_t rgba, float width) {
    if (ModDraw* dr = drawOf(d); dr && finite(x) && finite(y) && finite(r) && r > 0 && finite(width))
        dr->circle(x, y, r, rgba, std::clamp(width, 0.0f, 20.0f));
}
void apiDrawText(CellFapDraw* d, float x, float y, uint32_t rgba, const char* s) {
    if (ModDraw* dr = drawOf(d); dr && s && finite(x) && finite(y)) dr->text(x, y, rgba, s);
}

#undef WORLD

const CellFapApi kApi = {
    CELLFAP_MOD_API_VERSION,
    (uint32_t)sizeof(CellFapApi),
    apiLog,
    apiMessage,
    apiSetError,
    apiSetting,
    apiSetSetting,
    apiModFile,
    apiRegisterGenome,
    apiRegisterProgram,
    apiRequestDish,
    apiCellTypeName,
    apiParamName,
    apiWorldInfo,
    apiGetParam,
    apiSetParam,
    apiGetCell,
    apiSetCell,
    apiFindCell,
    apiPickCell,
    apiCellsNear,
    apiAddCell,
    apiAddCellLike,
    apiKillCell,
    apiSetCellMode,
    apiInject,
    apiRelease,
    apiGetLink,
    apiCellLinks,
    apiAddLink,
    apiSetLink,
    apiRemoveLink,
    apiGetFood,
    apiSetFood,
    apiAddFood,
    apiRemoveFood,
    apiGenomeText,
    apiGetMode,
    apiSetCellGenome,
    apiEditCellMode,
    apiUiText,
    apiUiTextColored,
    apiUiHeading,
    apiUiSeparator,
    apiUiSameLine,
    apiUiButton,
    apiUiCheckbox,
    apiUiSliderFloat,
    apiUiSliderInt,
    apiUiInputText,
    apiDrawLine,
    apiDrawCircle,
    apiDrawText,
};

}  // namespace

const CellFapApi* modApi() { return &kApi; }
