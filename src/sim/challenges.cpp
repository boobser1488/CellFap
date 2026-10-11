#include "challenges.hpp"

#include "layout.hpp"
#include "presets.hpp"
#include "scripted.hpp"

#include <cstdio>
#include <cstring>

bool isUserCell(const Cell& c) {
    return c.alive && (c.tag == CellTag::User || c.tag == CellTag::InfectedUser || c.tag == CellTag::Hybrid);
}

int largestUserOrganism(const World& w) {
    const auto& cells = w.cells();
    const auto& links = w.links();
    std::vector<char> seen(cells.size(), 0);
    std::vector<int> stack;
    int best = 0;
    for (size_t s = 0; s < cells.size(); ++s) {
        if (seen[s] || !isUserCell(cells[s])) continue;
        int size = 0;
        stack.assign(1, (int)s);
        seen[s] = 1;
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            ++size;
            const Cell& c = cells[i];
            for (int k = 0; k < c.linkCount; ++k) {
                const Link& l = links[c.links[k]];
                int o = l.a == i ? l.b : l.a;
                if (o >= 0 && !seen[o] && isUserCell(cells[o])) {
                    seen[o] = 1;
                    stack.push_back(o);
                }
            }
        }
        best = std::max(best, size);
    }
    return best;
}

namespace {

constexpr uint32_t kInspect = kToolBit(0), kSynth = kToolBit(1), kTweezers = kToolBit(2), kBoost = kToolBit(3),
                   kRemove = kToolBit(4), kShock = kToolBit(5), kGene = kToolBit(6), kDrug = kToolBit(7), kMap = kToolBit(8),
                   kAdhesin = kToolBit(9);

const Genome& preset(const char* name) {
    for (const GenomePreset& p : genomePresets())
        if (std::strcmp(p.name, name) == 0) return p.genome;
    static const Genome fallback;
    return fallback;
}

void scatterChallenge(World& w, Rng& rng, const Genome& g, int count, float radiusFrac, Vec2 avoid = {}, float avoidR = 0) {
    float R = w.settings.dishRadius * radiusFrac;
    for (int i = 0; i < count; ++i) {
        Vec2 p;
        for (int t = 0; t < 30; ++t) {
            p = fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * R);
            if (avoidR <= 0 || length(p - avoid) > avoidR) break;
        }
        w.addCell(g, p, rng.range(0, kTau), -1, CellTag::Challenge);
    }
}

void tagAll(World& w, CellTag tag) {
    for (Cell& c : w.cellsMut())
        if (c.alive) c.tag = tag;
}

struct Counts {
    int user = 0, challenge = 0, infectedChallenge = 0;
};
Counts count(const World& w) {
    Counts n;
    for (const Cell& c : w.cells()) {
        if (!c.alive) continue;
        if (isUserCell(c)) ++n.user;
        if (c.tag == CellTag::Challenge || c.tag == CellTag::InfectedChallenge) ++n.challenge;
        if (c.tag == CellTag::InfectedChallenge) ++n.infectedChallenge;
    }
    return n;
}

ChallengeProgress reach(int have, int need, const char* what) {
    ChallengeProgress p;
    p.progress = std::min(1.0f, have / (float)need);
    p.won = have >= need;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s: %d из %d", what, have, need);
    p.status = buf;
    return p;
}

// Holding a condition for `hours` of simulated time.
ChallengeProgress hold(const World& w, ChallengeState& st, bool ok, float hours, const char* what, int have, int need) {
    ChallengeProgress p;
    double t = w.timeHours();
    if (!ok) st.holdSince = -1;
    else if (st.holdSince < 0) st.holdSince = t;
    float held = ok ? (float)(t - st.holdSince) : 0.0f;
    p.progress = std::min(1.0f, held / hours);
    p.won = ok && held >= hours;
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s: %d (нужно %d) · продержаться %.1f из %.0f ч", what, have, need, held, hours);
    p.status = buf;
    return p;
}

bool anyUserIn(const World& w, Vec2 c, float r) {
    for (const Cell& cell : w.cells())
        if (isUserCell(cell) && length(cell.pos - c) < r) return true;
    return false;
}

float nearestUser(const World& w, Vec2 c) {
    float best = 1e9f;
    for (const Cell& cell : w.cells())
        if (isUserCell(cell)) best = std::min(best, length(cell.pos - c));
    return best;
}

// Food already lying around at the start (so immobile beginners' cells do not starve waiting for the first crumb).
void seedFood(World& w, Rng& rng, int n) {
    float R = w.settings.dishRadius * 0.95f;
    for (int i = 0; i < n; ++i) w.addFood(fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * R), w.settings.chunkSize * kFoodUnit);
}

// ---------------------------------------------------------------- levels
void setupFirst(World& w, Rng& rng, ChallengeState&) {
    Settings s;
    s.nutrientRate = 6;
    s.chunkSize = 0.5f;  // generous crumbs: the very first level must work with the default cell
    w.reset(s);
    seedFood(w, rng, 2500);
}
ChallengeProgress checkFirst(const World& w, ChallengeState&) { return reach(count(w).user, 50, "Ваших клеток"); }

void setupSun(World& w, Rng&, ChallengeState&) {
    Settings s;
    s.nutrientRate = 0;
    s.lightAmount = 1.3f;
    s.lightRange = 0.9f;
    w.reset(s);
}
ChallengeProgress checkSun(const World& w, ChallengeState&) { return reach(count(w).user, 200, "Ваших клеток"); }

void setupDelivery(World& w, Rng&, ChallengeState& st) {
    Settings s;
    s.nutrientRate = 4;
    s.lumpiness = 100;
    s.lumpSize = 0.035f;
    s.lightAmount = 0;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 120;
}
ChallengeProgress checkDelivery(const World& w, ChallengeState&) { return reach(count(w).user, 150, "Ваших клеток"); }

void setupArmor(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.nutrientRate = 4;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 150;
    scatterChallenge(w, rng, preset("Хищник"), 6, 0.9f, st.start, 400);
}
ChallengeProgress checkArmor(const World& w, ChallengeState& st) {
    int n = count(w).user;
    return hold(w, st, n >= 80, 12.0f, "Ваших клеток", n, 80);
}

void setupMulti(World& w, Rng& rng, ChallengeState&) {
    Settings s;
    s.nutrientRate = 4;
    s.lightAmount = 0.8f;
    w.reset(s);
    seedFood(w, rng, 900);
}
ChallengeProgress checkMulti(const World& w, ChallengeState&) {
    return reach(largestUserOrganism(w), 30, "Клеток в самом большом организме");
}

void setupSwim(World& w, Rng&, ChallengeState& st) {
    Settings s;
    s.dishRadius = 1400;
    s.nutrientRate = 2;
    s.lightAmount = 0.6f;
    s.lightAngle = kPi;  // light from the start side
    s.lightRange = 0.4f;
    w.reset(s);
    st.start = {-1050, 0};
    st.startR = 200;
    st.target = {1050, 0};
    st.targetR = 170;
}
ChallengeProgress checkTarget(const World& w, ChallengeState& st) {
    ChallengeProgress p;
    float d = nearestUser(w, st.target);
    float span = length(st.target - st.start);
    p.progress = d > 1e8f ? 0.0f : clampv(1.0f - (d - st.targetR) / span, 0.0f, 1.0f);
    p.won = anyUserIn(w, st.target, st.targetR);
    char buf[96];
    if (d > 1e8f) std::snprintf(buf, sizeof(buf), "Клеток пока нет — вставьте их в синий круг");
    else std::snprintf(buf, sizeof(buf), "До финиша: %.0f мкм", std::max(0.0f, d - st.targetR));
    p.status = buf;
    return p;
}

void setupMaze(World& w, Rng&, ChallengeState& st) {
    Settings s;
    s.dishRadius = 1300;
    s.nutrientRate = 3;
    s.lightAmount = 0.5f;
    w.reset(s);
    w.addBarrier({-500, -1400}, {-500, 900}, 8);
    w.addBarrier({0, -900}, {0, 1400}, 8);
    w.addBarrier({500, -1400}, {500, 900}, 8);
    w.addBarrier({-500, 900}, {-250, 900}, 8);
    w.addBarrier({250, -900}, {0, -900}, 8);
    st.start = {-900, 0};
    st.startR = 170;
    st.target = {900, 0};
    st.targetR = 160;
}

void setupPlague(World& w, Rng& rng, ChallengeState&) {
    Settings s;
    s.nutrientRate = 4;
    w.reset(s);
    scatterChallenge(w, rng, preset("Колония фагоцитов"), 5, 0.7f);
}
ChallengeProgress checkPlague(const World& w, ChallengeState&) {
    return reach(count(w).infectedChallenge, 60, "Заражённых клеток колонии");
}

void setupCleanup(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.nutrientRate = 0;
    s.lightAmount = 1.2f;
    s.lightRange = 0.9f;
    s.lightCapacity = 0.3f;
    w.reset(s);
    scatterChallenge(w, rng, preset("Нить"), 15, 0.85f);
    scatterChallenge(w, rng, preset("Водоросль"), 40, 0.85f);
    st.baseline = 0;
}
ChallengeProgress checkCleanup(const World& w, ChallengeState& st) {
    ChallengeProgress p;
    Counts n = count(w);
    double t = w.timeHours();
    p.won = t > 2.0 && n.challenge < 30;
    p.lost = t > 36.0 && !p.won;
    st.baseline = std::max(st.baseline, n.challenge);  // the peak of the algae bloom
    p.progress = st.baseline > 30 ? clampv((float)(st.baseline - n.challenge) / (float)(st.baseline - 30), 0.0f, 1.0f) : 0.0f;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Водорослей: %d (нужно меньше 30) · прошло %.1f из 36 ч", n.challenge, t);
    p.status = buf;
    return p;
}

void setupThrift(World& w, Rng&, ChallengeState& st) {
    Settings s;
    s.nutrientRate = 0.5f;
    s.lightAmount = 0.2f;
    s.salinity = 0.1f;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 200;
}
ChallengeProgress checkThrift(const World& w, ChallengeState& st) {
    int n = count(w).user;
    return hold(w, st, n >= 40, 24.0f, "Ваших клеток", n, 40);
}

void setupTamer(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.dishRadius = 1200;
    s.nutrientRate = 3;
    s.viscosity = 5;
    s.swimAnisotropy = 5;
    w.reset(s);
    if (const Genome* g = scriptedGenome("Нематода")) {
        GenomeLayout L = buildGenomeLayout(*g);
        spawnGrownOrganismFree(w, *g, L, rng, 0, 300);
    }
    tagAll(w, CellTag::Challenge);
    st.baseline = count(w).challenge;
}
ChallengeProgress checkTamer(const World& w, ChallengeState& st) {
    ChallengeProgress p;
    int total = 0, calm = 0;
    for (const Cell& c : w.cells())
        if (c.alive && c.tag == CellTag::Challenge) {
            ++total;
            if (c.paralysis > 0 || c.stun > 0) ++calm;
        }
    p.progress = total ? calm / (float)total / 0.9f : 0.0f;
    p.progress = std::min(p.progress, 1.0f);
    p.won = total > 0 && calm >= total * 0.9f;
    p.lost = total < st.baseline / 2;
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Успокоено клеток: %d из %d (нужно 90%%), живых %d из %d", calm, total, total, st.baseline);
    p.status = buf;
    return p;
}

void setupKindergarten(World& w, Rng& rng, ChallengeState&) {
    Settings s;
    s.nutrientRate = 3;
    w.reset(s);
    Genome g;
    g.name = "Одиночка";
    Mode& m = g.modes.edit(0);
    m.type = CellType::Phagocyte;
    m.color = {0.95f, 0.72f, 0.45f};
    m.splitMass = kNeverSplit;
    for (int i = 0; i < 40; ++i) {
        Vec2 p = fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * s.dishRadius * 0.85f);
        w.addCell(g, p, rng.range(0, kTau), 0, CellTag::User, 2.4f);
    }
}
ChallengeProgress checkKindergarten(const World& w, ChallengeState&) {
    return reach(largestUserOrganism(w), 25, "Клеток в самом большом организме");
}

void setupEel(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.dishRadius = 1000;
    s.nutrientRate = 2;
    s.lightAmount = 0.4f;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 150;
    scatterChallenge(w, rng, preset("Пловец"), 16, 0.9f, st.start, 350);
}
ChallengeProgress checkEel(const World& w, ChallengeState&) {
    return reach((int)w.lysedChallenge, 20, "Пловцов, лопнувших от разряда");
}

void setupStingers(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.dishRadius = 900;
    s.nutrientRate = 3;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 150;
    scatterChallenge(w, rng, preset("Пловец"), 20, 0.9f, st.start, 300);
}
ChallengeProgress checkStingers(const World& w, ChallengeState&) {
    int calm = 0;
    for (const Cell& c : w.cells())
        if (c.alive && c.tag == CellTag::Challenge && c.paralysis > 0) ++calm;
    return reach(calm, 12, "Парализованных пловцов сейчас");
}

void setupFloat(World& w, Rng&, ChallengeState& st) {
    Settings s;
    s.dishRadius = 1000;
    s.nutrientRate = 3;
    s.gravity = 1.0f;
    s.density = 0.0f;
    s.densityGradient = -1.0f;
    s.lightAmount = 0.3f;
    w.reset(s);
    st.start = {0, -750};
    st.startR = 160;
    st.target = {0, 720};
    st.targetR = 220;
}
ChallengeProgress checkFloat(const World& w, ChallengeState& st) {
    int in = 0;
    for (const Cell& c : w.cells())
        if (isUserCell(c) && length(c.pos - st.target) < st.targetR) ++in;
    return reach(in, 40, "Ваших клеток в зелёном круге");
}

void setupGametes(World& w, Rng&, ChallengeState&) {
    Settings s;
    s.nutrientRate = 4;
    s.lightAmount = 0.8f;
    w.reset(s);
}
ChallengeProgress checkGametes(const World& w, ChallengeState&) { return reach((int)w.fusions, 10, "Слияний гамет"); }

void setupColony(World& w, Rng& rng, ChallengeState&) {
    Settings s;
    s.nutrientRate = 3;
    s.lightAmount = 0.9f;
    w.reset(s);
    seedFood(w, rng, 900);
    scatterChallenge(w, rng, preset("Хищник"), 4, 0.9f);
}
ChallengeProgress checkColony(const World& w, ChallengeState& st) {
    int big = largestUserOrganism(w);
    return hold(w, st, big >= 40, 6.0f, "Клеток в самом большом организме", big, 40);
}

// Lure: euglenas wander through a dark dish; a light in the green circle brings them in.
void setupLure(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.dishRadius = 900;
    s.nutrientRate = 1;
    s.lightAmount = 0.6f;  // dim and even (no direction to follow): enough for the euglenas to live on
    s.lightRange = 1.0f;
    s.viscosity = 5;
    s.swimAnisotropy = 5;
    w.reset(s);
    st.start = st.target = {0, 0};
    st.startR = 120;
    st.targetR = 200;
    if (const Genome* g = scriptedGenome("Эвглена")) {
        GenomeLayout L = buildGenomeLayout(*g);
        for (int k = 0; k < 16; ++k) spawnGrownOrganismFree(w, *g, L, rng, 350, 850);
    }
    tagAll(w, CellTag::Challenge);
}
ChallengeProgress checkLure(const World& w, ChallengeState& st) {
    int in = 0;
    for (const Cell& c : w.cells())
        if (c.alive && c.tag == CellTag::Challenge && length(c.pos - st.target) < st.targetR) ++in;
    return reach(in, 16, "Клеток эвглен в зелёном круге");
}

// Grip: fast swimmers dash around; catch them alive.
void setupGrip(World& w, Rng& rng, ChallengeState& st) {
    Settings s;
    s.dishRadius = 800;
    s.nutrientRate = 3;
    w.reset(s);
    st.start = {0, 0};
    st.startR = 150;
    seedFood(w, rng, 300);
    scatterChallenge(w, rng, preset("Пловец"), 14, 0.9f, st.start, 320);
}
ChallengeProgress checkGrip(const World& w, ChallengeState&) {
    // A swimmer counts as caught while an adhesin ties it to one of the player's cells (glue is forbidden here, so only
    // a grappler's grip can do that).
    int caught = 0;
    const auto& cells = w.cells();
    for (const Cell& c : cells) {
        if (!c.alive || c.tag != CellTag::Challenge) continue;
        for (int k = 0; k < c.linkCount; ++k) {
            const Link& l = w.links()[c.links[k]];
            int o = l.a == (int)(&c - cells.data()) ? l.b : l.a;
            if (o >= 0 && isUserCell(cells[o])) {
                ++caught;
                break;
            }
        }
    }
    return reach(caught, 5, "Пойманных пловцов сейчас");
}

// Night shift: light only by day (12 h days), no food; keep a colony alive through two nights.
void setupNight(World& w, Rng&, ChallengeState&) {
    Settings s;
    s.nutrientRate = 0;
    s.lightAmount = 1.2f;
    s.lightRange = 1.0f;
    s.dayLength = 12;
    w.reset(s);
}
ChallengeProgress checkNight(const World& w, ChallengeState& st) {
    // During the second night (18..24 h) the colony must glow: at least 12 lit luminocytes among 40 cells, 2 h in a row.
    int users = 0, lit = 0;
    for (const Cell& c : w.cells())
        if (isUserCell(c)) {
            ++users;
            lit += c.type == CellType::Luminocyte && c.actuator > 0.5f;
        }
    double t = w.timeHours();
    const bool night = w.daylight() < 0.05f;
    const bool secondNight = night && t > 15.0;
    char what[128];
    std::snprintf(what, sizeof what, "Светящихся люминоцитов (клеток %d, нужно 40)", users);
    ChallengeProgress p = hold(w, st, secondNight && lit >= 12 && users >= 40, 2.0f, what, lit, 12);
    if (!secondNight) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "%s. Ваших клеток: %d, светится: %d · %s", t < 15.0 ? "Ждём вторую ночь" : "Сейчас не ночь", users, lit,
                      night ? "ночь" : (w.daylight() > 0.85f ? "день" : "сумерки"));
        p.status = buf;
    }
    st.baseline = std::max(st.baseline, users);  // the colony existed: dying out now is a loss
    if (st.baseline > 0 && users == 0) p.lost = true;
    if (t >= 24.5 && !p.won) p.lost = true;
    return p;
}

constexpr uint32_t type(CellType t) { return 1u << (int)t; }

}  // namespace

uint32_t genomeTypes(const Genome& g) {
    bool seen[kMaxModes] = {};
    int stack[kMaxModes * 4], sp = 0;
    stack[sp++] = g.initialMode;
    uint32_t bits = 0;
    while (sp) {
        int m = stack[--sp];
        if (seen[m]) continue;
        seen[m] = true;
        const Mode& md = g.modes[m];
        bits |= 1u << (int)md.type;
        if (md.splitMass < kNeverSplit && md.type != CellType::Gamete) {
            stack[sp++] = md.childMode[0];
            stack[sp++] = md.childMode[1];
        }
        if (md.type == CellType::Stemocyte) {
            stack[sp++] = md.stemMode[0];
            stack[sp++] = md.stemMode[1];
        }
        if (md.type == CellType::Cystocyte) stack[sp++] = md.stemMode[0];
        if (md.type == CellType::Gamete) stack[sp++] = md.gameteModeAfter;
    }
    return bits;
}

const std::vector<Challenge>& challenges() {
    static const std::vector<Challenge> c = {
        {"Первая клетка",
         "В чашке Петри есть пища, но нет клеток. Вставьте одну клетку синтезатором (2) и подождите: она будет есть и "
         "делиться сама.",
         "Вырастите 50 своих клеток из одной.",
         "Фагоцит по умолчанию уже умеет есть и делиться — геном менять не обязательно.",
         "Из одной клетки выросло 50.",
         "", kInspect | kSynth, 1, 0, setupFirst, checkFirst},
        {"Солнечные ванны",
         "В чашке нет пищи, но много света. Клеткам придётся питаться светом.",
         "Вырастите 200 клеток без пищи — только на свету. Вставка одна.",
         "Фотоциты питаются светом. Держите их в освещённой части чашки.",
         "Колония выросла до 200 клеток только на свету.",
         "", kInspect | kSynth, 1, 0, setupSun, checkSun},
        {"Доставка не работает",
         "Пища лежит кучками в разных местах чашки, света нет. Чтобы добраться до пищи, клеткам нужно плавать. Стартовая "
         "зона — синий круг.",
         "Вырастите 150 клеток. Света нет, вставка одна.",
         "Флагеллоцит толкает клетку вперёд. Организм из фагоцита и флагеллоцита сам доплывает до пищи.",
         "Колония выросла до 150 клеток.",
         "", kInspect | kSynth, 1, 0, setupDelivery, checkDelivery},
        {"Броня крепка",
         "В чашке живут хищники: они питаются чужими клетками. Защитите от них свою колонию.",
         "Удерживайте не меньше 80 своих клеток 12 часов подряд.",
         "Кератиноцит защищает себя и всех, кто держится за него адгезином. Девороциты такую броню не прокусывают.",
         "80 клеток продержались 12 часов.",
         "", kInspect | kSynth, 2, 0, setupArmor, checkArmor},
        {"Многоклеточность",
         "Вырастите организм хотя бы из 30 клеток, связанных друг с другом адгезинами.",
         "Организм из 30 клеток, связанных адгезинами.",
         "Включите «Держаться вместе» при делении: потомки останутся связаны адгезином.",
         "Собран организм из 30 связанных клеток.",
         "", kInspect | kSynth, 1, 0, setupMulti, checkMulti},
        {"Вплавь",
         "Финиш — зелёный круг на другом краю чашки. Старт — в синем круге.",
         "Доведите любую свою клетку до зелёного круга.",
         "Жгутик толкает клетку вперёд. Чем больше жгутиков направлено назад, тем быстрее организм движется вперёд.",
         "Клетка доплыла до финиша.",
         "", kInspect | kSynth, 2, 0, setupSwim, checkTarget},
        {"Лабиринт",
         "В чашке стоят стенки лабиринта. Вход — в синем круге, выход — в зелёном.",
         "Доведите любую свою клетку до зелёного круга.",
         "Глаз на стены (стереоцит, «Стенка») подскажет, куда поворачивать. Другой путь — размножаться, пока колония не "
         "заполнит лабиринт.",
         "Клетка прошла лабиринт.",
         "", kInspect | kSynth, 2, 0, setupMaze, checkTarget},
        {"Эпидемиолог наоборот",
         "В чашке растёт чужая колония фагоцитов. Заразите её своим вирусом.",
         "Заразите 60 клеток колонии своим вирусом.",
         "Вироцит при касании переписывает моды чужой клетки. Чтобы заразить колонию, вироцит должен до неё доплыть.",
         "60 клеток колонии заражены.",
         "", kInspect | kSynth, 2, 0, setupPlague, checkPlague},
        {"Санитарный день",
         "Чашку заполняют водоросли. Удалять их инструментами нельзя — нужен организм, который их съест.",
         "Оставьте меньше 30 водорослей за 36 часов.",
         "Девороцит высасывает чужие клетки. Хищник со жгутиком сам находит водоросли.",
         "Водорослей осталось меньше 30.",
         "Прошло 36 часов, а водорослей по-прежнему 30 или больше.",
         kInspect | kSynth, 3, 0, setupCleanup, checkCleanup},
        {"Режим экономии",
         "Пищи мало, света почти нет. Клеткам нужно расходовать массу экономно и запасать её.",
         "Удерживайте 40 своих клеток 24 часа подряд.",
         "Липоцит хранит массу и почти ничего не тратит. Каждая лишняя клетка расходует пищу.",
         "40 клеток продержались 24 часа.",
         "", kInspect | kSynth, 2, 0, setupThrift, checkThrift},
        {"Укротитель",
         "Нематода быстро движется по чашке. Обездвижьте её, не убивая.",
         "Обездвижьте 90% клеток нематоды одновременно (паралич или судорога).",
         "Тетродотоксин в воде рядом с нематодой парализует её нервы. Электрод действует быстрее, но слишком сильный разряд "
         "убивает клетки.",
         "Нематода обездвижена.",
         "Погибло больше половины клеток нематоды.",
         kInspect | kDrug | kShock, 0, 0, setupTamer, checkTamer},
        {"Детский сад",
         "В чашке 40 отдельных клеток, которые не делятся и не связываются друг с другом. Свяжите их вручную.",
         "Соберите инструментом «Адгезин» организм из 25 клеток.",
         "Режим «Связать»: нажмите на клетку и протяните к соседней. Пинцетом можно подвести далёкие клетки ближе.",
         "Собран организм из 25 клеток.",
         "", kInspect | kAdhesin | kTweezers, 0, 0, setupKindergarten, checkKindergarten},
        {"Электрорыбалка",
         "В чашке плавают пловцы. Девороциты, вироциты и книдоциты в этом задании запрещены: уничтожать пловцов можно только "
         "разрядом электроцитов.",
         "20 пловцов должны лопнуть от разряда.",
         "Электроцит разряжается, когда программа «Разряд» > 0.5. Разряда одного электроцита мало, нескольких вместе — "
         "достаточно. Глаз на клетки подскажет, когда разряжаться.",
         "20 пловцов уничтожены разрядом.",
         "", kInspect | kSynth, 2, type(CellType::Devorocyte) | type(CellType::Virocyte) | type(CellType::Cnidocyte), setupEel,
         checkEel},
        {"Стрекающие",
         "Парализуйте пловцов, не съедая их. Девороциты и вироциты в этом задании запрещены.",
         "Парализуйте 12 пловцов одновременно.",
         "Книдоцит стреляет ядовитой нитью в чужую клетку при касании. Чем больше книдоцитов на подвижном организме, тем "
         "больше попаданий.",
         "12 пловцов парализованы одновременно.",
         "", kInspect | kSynth, 2, type(CellType::Devorocyte) | type(CellType::Virocyte), setupStingers, checkStingers},
        {"Всплыть",
         "В чашке включена гравитация. Стартовая зона — внизу, зелёный круг — наверху.",
         "40 ваших клеток одновременно в зелёном круге наверху.",
         "Буйоцит с отрицательной плотностью тянет вверх. Привяжите к нему остальные клетки — и весь организм всплывёт.",
         "40 клеток всплыли в зелёный круг.",
         "", kInspect | kSynth, 1, 0, setupFloat, checkFloat},
        {"Свидание вслепую",
         "Освойте половое размножение: гаметы вашего вида должны сливаться друг с другом.",
         "10 слияний гамет вашего вида.",
         "Гамета сливается с совместимой гаметой (настройка «Совместима с»). Нужны две моды-гаметы, совместимые друг с другом, "
         "и клетки, которые их производят.",
         "Гаметы слились 10 раз.",
         "", kInspect | kSynth, 2, 0, setupGametes, checkGametes},
        {"Крепость",
         "В чашке хищники. Постройте организм, который они не смогут разрушить.",
         "Организм из 40 клеток должен продержаться 6 часов подряд.",
         "Кератиноциты по краям, адгезины внутри. Хищники держатся рядом долго, поэтому защита нужна всё время.",
         "Организм из 40 клеток продержался 6 часов.",
         "", kInspect | kSynth, 1, 0, setupColony, checkColony},
        {"Приманка",
         "В чашке полутемно. Эвглены плавают по чашке и плывут на свет. Соберите их в зелёном круге.",
         "16 клеток эвглен одновременно в зелёном круге.",
         "Люминоцит светится, а эвглены плывут на любой источник света в пределах 300 мкм. Поставьте светящийся организм в "
         "центре.",
         "16 клеток эвглен собрались в зелёном круге.",
         "", kInspect | kSynth, 2, 0, setupLure, checkLure},
        {"Мёртвая хватка",
         "По чашке быстро плавают пловцы. Поймайте их живыми. Глюеоциты, девороциты и вироциты в этом задании запрещены.",
         "Держите 5 пловцов одновременно.",
         "Хватоцит цепляет чужие клетки, которых касается. Неподвижная колония хватоцитов работает как сеть: пловцы сами в неё "
         "врезаются.",
         "5 пловцов пойманы.",
         "", kInspect | kSynth, 2, type(CellType::Glueocyte) | type(CellType::Devorocyte) | type(CellType::Virocyte), setupGrip,
         checkGrip},
        {"Ночная смена",
         "Здесь сменяются день и ночь, пищи нет. Колония должна светиться ночью.",
         "Во вторую ночь: 12 светящихся люминоцитов в колонии из 40 клеток, 2 часа подряд.",
         "Люминоцит с программой «вход Свет, x < c ? a : b» (c = 0.2, a = 1, b = 0) горит только в темноте. Днём фотоциты "
         "кормят колонию — держите их вместе адгезином.",
         "Колония светилась 2 часа во вторую ночь.",
         "Колония не светилась во вторую ночь или погибла.",
         kInspect | kSynth, 1, 0, setupNight, checkNight},
    };
    return c;
}
