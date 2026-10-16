#include "presets.hpp"

#include "layout.hpp"
#include "scripted.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

Mode& setup(Genome& g, int i, CellType t, Color col) {
    Mode& m = g.modes.edit(i);
    m.type = t;
    m.color = col;
    return m;
}

Genome algae() {
    Genome g;
    g.name = "Водоросль";
    Mode& m = setup(g, 0, CellType::Photocyte, {0.45f, 0.78f, 0.36f});
    m.splitMass = 2.538f;
    return g;
}

Genome filament() {
    Genome g;
    g.name = "Нить";
    Mode& m = setup(g, 0, CellType::Photocyte, {0.36f, 0.72f, 0.50f});
    m.makeAdhesin = true;
    m.adhesinStiffness = 0.7f;
    m.splitMass = 2.79f;
    return g;
}

Genome colony() {
    Genome g;
    g.name = "Колония фагоцитов";
    Mode& m = setup(g, 0, CellType::Phagocyte, {0.86f, 0.70f, 0.44f});
    m.makeAdhesin = true;
    m.adhesinStiffness = 0.3f;
    m.splitAngle = deg2rad(90);
    m.childAngle[0] = deg2rad(90);
    m.childAngle[1] = deg2rad(90);
    m.maxConnections = 6;
    return g;
}

// Zygote -> (front) body + (back) flagellocyte; the body buds new zygotes forward.
Genome swimmer(CellType bodyType, const char* name, Color bodyColor) {
    Genome g;
    g.name = name;
    Mode& z = setup(g, 0, bodyType, bodyColor);
    z.makeAdhesin = true;
    z.adhesinStiffness = 0.8f;
    z.splitMass = 2.034f;
    z.childMode[0] = 2;  // front: body
    z.childMode[1] = 1;  // back: flagellocyte
    Mode& f = setup(g, 1, CellType::Flagellocyte, {0.44f, 0.66f, 0.92f});
    f.splitMass = kNeverSplit;
    f.prog[PF_Swim].b = 0.6f;
    f.nutrientPriority = 0.6f;
    Mode& b = setup(g, 2, bodyType, Color{bodyColor.r * 0.9f, bodyColor.g * 0.9f, bodyColor.b * 0.9f});
    b.splitMass = 3.42f;
    b.splitAngle = deg2rad(180);  // child 1 stays at the back with the tail, child 2 leaves forward
    b.childAngle[0] = deg2rad(180);
    b.childAngle[1] = deg2rad(180);
    b.childMode[0] = 2;
    b.childMode[1] = 0;
    b.keepAdhesin[1] = false;
    b.splitRatio = 0.6f;
    b.nutrientPriority = 1.5f;
    return g;
}

Genome buoyAlgae() {
    Genome g;
    g.name = "Плавучая водоросль";
    // Zygote -> photocyte + float; the photocyte keeps its float and buds free zygotes.
    Mode& z = setup(g, 0, CellType::Photocyte, {0.40f, 0.80f, 0.55f});
    z.makeAdhesin = true;
    z.adhesinStiffness = 0.5f;
    z.childMode[0] = 2;
    z.childMode[1] = 1;
    z.splitMass = 2.538f;
    Mode& b = setup(g, 1, CellType::Buoyocyte, {0.70f, 0.88f, 0.94f});
    b.splitMass = kNeverSplit;
    b.prog[PF_Density].b = -1.5f;
    b.nutrientPriority = 0.5f;
    Mode& p = setup(g, 2, CellType::Photocyte, {0.36f, 0.74f, 0.50f});
    p.splitMass = 2.79f;
    p.splitAngle = deg2rad(180);
    p.childAngle[0] = deg2rad(180);  // child angles are relative to the split direction:
    p.childAngle[1] = deg2rad(180);  // 180° keeps both children facing the parent's way
    p.childMode[0] = 2;
    p.childMode[1] = 0;
    p.keepAdhesin[1] = false;
    return g;
}

Genome plague() {
    Genome g;
    g.name = "Чума";
    Mode& m = setup(g, 0, CellType::Virocyte, {0.70f, 0.40f, 0.86f});
    m.splitMass = 2.79f;
    m.virusCopyFrom = 0;
    return g;
}

Genome armored() {
    Genome g;
    g.name = "Броненосец";
    Mode& m = setup(g, 0, CellType::Phagocyte, {0.80f, 0.74f, 0.50f});
    m.makeAdhesin = true;
    m.adhesinStiffness = 0.4f;
    m.splitAngle = deg2rad(90);
    m.childAngle[0] = deg2rad(30);  // the core turns 120° per division: plates spread around it
    m.childMode[1] = 1;
    m.maxConnections = 4;
    Mode& k = setup(g, 1, CellType::Keratinocyte, {0.74f, 0.70f, 0.60f});
    k.splitMass = kNeverSplit;
    k.nutrientPriority = 0.4f;
    return g;
}

Genome glueMat() {
    Genome g;
    g.name = "Клейкий мат";
    // Free-living photocytes; every division also leaves a sticky glueocyte behind.
    Mode& p = setup(g, 0, CellType::Photocyte, {0.50f, 0.80f, 0.42f});
    p.splitMass = 2.538f;
    p.childMode[1] = 1;
    Mode& m = setup(g, 1, CellType::Glueocyte, {0.86f, 0.82f, 0.46f});
    m.splitMass = kNeverSplit;
    m.nutrientPriority = 0.3f;
    return g;
}

Genome lipidStore() {
    Genome g;
    g.name = "Жировой запас";
    Mode& m = setup(g, 0, CellType::Photocyte, {0.50f, 0.78f, 0.40f});
    m.makeAdhesin = true;
    m.adhesinStiffness = 0.5f;
    m.splitMass = 2.538f;
    m.childMode[1] = 1;
    m.maxConnections = 3;
    Mode& l = setup(g, 1, CellType::Lipocyte, {0.96f, 0.88f, 0.58f});
    l.splitMass = kNeverSplit;
    l.nutrientPriority = 0.15f;  // low priority: stores only the surplus
    return g;
}

// A photocyte zygote grows, then becomes neurocyte head - myocyte - flagellocyte tail.
// The neurocyte is a relaxation oscillator on S1/S2; S1 diffuses into the myocyte and bends it.
Genome worm() {
    Genome g;
    g.name = "Червь";
    Mode& z = setup(g, 0, CellType::Photocyte, {0.50f, 0.78f, 0.44f});
    z.makeAdhesin = true;
    z.adhesinStiffness = 0.9f;
    z.splitMass = 3.42f;
    z.splitRatio = 0.35f;
    z.childMode[0] = 1;  // front: head
    z.childMode[1] = 2;  // back: body
    Mode& n = setup(g, 1, CellType::Neurocyte, {0.96f, 0.86f, 0.62f});
    n.splitMass = kNeverSplit;
    n.prog[PF_Neuro1] = {ProgInput::S2, ProgKind::Step, 12.0f, 0.0f, 4.0f};
    n.prog[PF_Neuro2] = {ProgInput::S1, ProgKind::Step, -6.0f, 10.0f, 6.0f};
    n.neuroChannel[0] = 0;
    n.neuroChannel[1] = 1;
    Mode& b = setup(g, 2, CellType::Photocyte, {0.46f, 0.74f, 0.42f});
    b.makeAdhesin = true;
    b.adhesinStiffness = 0.9f;
    b.splitMass = 2.034f;
    b.childMode[0] = 4;  // front: myocyte (keeps the head link)
    b.childMode[1] = 3;  // back: tail
    Mode& t = setup(g, 3, CellType::Flagellocyte, {0.44f, 0.66f, 0.92f});
    t.splitMass = kNeverSplit;
    t.prog[PF_Swim].b = 0.5f;
    Mode& m = setup(g, 4, CellType::Myocyte, {0.86f, 0.46f, 0.52f});
    m.splitMass = kNeverSplit;
    m.prog[PF_Bend] = {ProgInput::S1, ProgKind::Linear, 0.12f, -0.5f, 0};
    return g;
}

// ------------------------------------------------------------------ complex organisms
// Giant worm: a long segmented body 21 cells thick that swims by undulation (no flagella).
//  - Eyes: two senseocytes on the snout looking for food; the left one adds +S2, the right one -S2, so S2
//    says "food is to the left / right".
//  - Brain: the first 4 segments have neurocytes that amplify the visual S2, which drives their muscles,
//    so the head turns toward food and the body follows.
//  - Nerve cord: every body segment has a ganglion (relaxation pacemaker on S3/S4) that drives the muscle
//    signal S1; neighbouring ganglia are coupled and start with a phase gradient, so a bending wave runs
//    down the body. Left and right muscles contract in antiphase.
//  - Motor neurons next to each ganglion turn the nerve-cord rhythm S1 into the motor command S2.
//  - Cross-section from the spine outward: motor neuron, chlorenchyma x3, muscle x3, fat, skin, cuticle.
//  With "swimming by undulation" (anisotropic drag) the travelling wave pushes against the water.
// Hunter: devorocyte jaw and a stereocyte "eye" in front; two flagellocytes steer toward prey because the
// eye's signed signal (S1 > 0: prey on the left) throttles one side; armour plates bud offspring.
Genome hunter() {
    Genome g;
    g.name = "Хищник-охотник";
    enum { Z = 0, Front = 1, Back = 2, Jaw = 3, EyeC = 4, Left = 5, Right = 6, FlL = 7, PlL = 8, FlR = 9, PlR = 10 };
    auto body = [&](int i, CellType t, Color c) -> Mode& {
        Mode& m = setup(g, i, t, c);
        m.makeAdhesin = true;
        m.adhesinStiffness = 0.9f;
        m.splitMass = 2.034f;
        return m;
    };
    Mode& z = body(Z, CellType::Phagocyte, {0.86f, 0.46f, 0.40f});
    z.childMode[0] = Front;
    z.childMode[1] = Back;
    Mode& fr = body(Front, CellType::Phagocyte, {0.86f, 0.46f, 0.40f});
    fr.childMode[0] = Jaw;
    fr.childMode[1] = EyeC;
    Mode& jaw = setup(g, Jaw, CellType::Devorocyte, {0.92f, 0.30f, 0.30f});
    jaw.splitMass = kNeverSplit;
    jaw.nutrientPriority = 0.4f;
    Mode& eye = setup(g, EyeC, CellType::Stereocyte, {0.96f, 0.70f, 0.40f});
    eye.splitMass = kNeverSplit;
    eye.senseTarget = SenseTarget::Cell;
    eye.senseColor = {0.45f, 0.78f, 0.38f};  // green prey (photocytes, algae)
    eye.senseThreshold = 0.5f;
    eye.senseChannel = 0;
    eye.senseOutput = 40;
    Mode& bk = body(Back, CellType::Phagocyte, {0.80f, 0.42f, 0.38f});
    bk.splitAngle = deg2rad(90);
    bk.childAngle[0] = bk.childAngle[1] = deg2rad(-90);
    bk.childMode[0] = Left;
    bk.childMode[1] = Right;
    for (int side = 0; side < 2; ++side) {
        Mode& s = body(side == 0 ? Left : Right, CellType::Phagocyte, {0.80f, 0.42f, 0.38f});
        s.splitAngle = deg2rad(180);  // child 1 behind (flagellum), child 2 in front (plate)
        s.childAngle[0] = s.childAngle[1] = deg2rad(180);
        s.childMode[0] = side == 0 ? FlL : FlR;
        s.childMode[1] = side == 0 ? PlL : PlR;
        Mode& fl = setup(g, side == 0 ? FlL : FlR, CellType::Flagellocyte, {0.46f, 0.62f, 0.92f});
        fl.splitMass = kNeverSplit;
        fl.nutrientPriority = 0.6f;
        // Prey on the left (S1 > 0) slows the left flagellum and speeds the right one: the hunter turns left.
        fl.prog[PF_Swim] = {ProgInput::S1, ProgKind::Linear, side == 0 ? -0.012f : 0.012f, 0.55f, 0};
        Mode& pl = setup(g, side == 0 ? PlL : PlR, CellType::Keratinocyte, {0.82f, 0.62f, 0.50f});
        pl.splitMass = 3.42f;  // a well-fed plate buds a new hunter
        pl.splitRatio = 0.6f;
        pl.splitAngle = deg2rad(side == 0 ? -90 : 90);
        pl.childAngle[0] = deg2rad(side == 0 ? 90 : -90);
        pl.childMode[0] = side == 0 ? PlL : PlR;
        pl.childMode[1] = Z;
        pl.keepAdhesin[1] = false;
    }
    return g;
}

// Parasitic beetle. Free-living stage: a dormant two-cell egg (stemocyte core + senseocyte antenna) that
// costs almost nothing. When the antenna smells a green host the core differentiates into a larva that
// drains the host, sticks to it with glue legs, cuts its adhesins with a protease proboscis (burrowing in)
// and grows an ovary that lays new eggs.
Genome parasiteBeetle() {
    Genome g;
    g.name = "Паразитический жук";
    enum { Egg = 0, Core = 1, Antenna = 2, Larva = 3, Jaw = 4, Shell = 5, Leg = 6, Abdomen = 7, Proboscis = 8, Ovary = 9 };
    // All parasite colours are far from the host green, so the antenna never "smells" its own body.
    Mode& egg = setup(g, Egg, CellType::Keratinocyte, {0.62f, 0.42f, 0.36f});
    egg.splitMass = kSplitAtAge;
    egg.makeAdhesin = true;
    egg.adhesinStiffness = 0.8f;
    egg.childMode[0] = Core;
    egg.childMode[1] = Antenna;
    Mode& core = setup(g, Core, CellType::Stemocyte, {0.80f, 0.52f, 0.46f});
    core.splitMass = kNeverSplit;
    core.stemMode[0] = core.stemMode[1] = Larva;
    core.prog[PF_Stem1] = {ProgInput::S1, ProgKind::Linear, 0.12f, 0.0f, 0};  // hatch when S1 > ~4
    core.prog[PF_Stem2] = {ProgInput::Fixed, ProgKind::Linear, 0, -1.0f, 0};
    Mode& ant = setup(g, Antenna, CellType::Senseocyte, {0.90f, 0.50f, 0.36f});
    ant.splitMass = kNeverSplit;
    ant.senseTarget = SenseTarget::Cell;
    ant.senseColor = {0.45f, 0.78f, 0.38f};
    ant.senseThreshold = 0.35f;
    ant.senseChannel = 0;
    ant.senseOutput = 40;
    ant.nutrientPriority = 0.3f;
    auto part = [&](int i, CellType t, Color c) -> Mode& {
        Mode& m = setup(g, i, t, c);
        m.makeAdhesin = true;
        m.adhesinStiffness = 0.8f;
        m.splitMass = 2.034f;
        return m;
    };
    Mode& larva = part(Larva, CellType::Devorocyte, {0.70f, 0.30f, 0.34f});
    larva.childMode[0] = Jaw;
    larva.childMode[1] = Shell;
    Mode& jaw = setup(g, Jaw, CellType::Devorocyte, {0.74f, 0.26f, 0.30f});
    jaw.splitMass = kNeverSplit;
    jaw.nutrientPriority = 0.3f;
    Mode& shell = part(Shell, CellType::Keratinocyte, {0.42f, 0.32f, 0.26f});
    shell.splitAngle = deg2rad(90);
    shell.childAngle[0] = shell.childAngle[1] = deg2rad(-90);
    shell.childMode[0] = Leg;
    shell.childMode[1] = Abdomen;
    Mode& leg = setup(g, Leg, CellType::Glueocyte, {0.66f, 0.44f, 0.30f});
    leg.splitMass = kNeverSplit;
    Mode& ab = part(Abdomen, CellType::Keratinocyte, {0.36f, 0.28f, 0.24f});
    ab.childMode[0] = Proboscis;  // forward
    ab.childMode[1] = Ovary;      // back
    Mode& pr = setup(g, Proboscis, CellType::Secrocyte, {0.80f, 0.40f, 0.60f});
    pr.splitMass = kNeverSplit;
    pr.secretion = Secretion::Protease;
    Mode& ov = setup(g, Ovary, CellType::Lipocyte, {0.92f, 0.62f, 0.52f});
    ov.nutrientPriority = 3.0f;  // hoards what the jaw steals
    ov.splitMass = 3.42f;
    ov.splitRatio = 0.4f;        // the egg gets 60%: enough for its two cells
    ov.childMode[0] = Ovary;     // stays attached, in front
    ov.childMode[1] = Egg;       // released behind
    ov.keepAdhesin[1] = false;
    return g;
}

// Signal worm: a chain grown from zones like the filament worm, but its rhythm comes from programmable signal cells
// instead of a nervous system. Every segment is [signal cell, muscle, cilia]: the signal cell runs a dypt oscillator
// that drives S1 along a sine; its muscle neighbour bends by S1. The phase of each segment lags the one before it, so a
// bending wave runs from the head to the tail while the cilia and the flagellum push the body. A thin short chain does
// not swim by undulation (measured: a strong wave only turns it round), so the rhythm is its foraging behaviour: a weak
// wave on the way, a strong one where there is food within reach — it circles there and eats (+13 % mass in 6 h).
std::string signalWormProgram(int segment, int segments) {
    char head[2048];  // (Cyrillic: two bytes a letter)
    std::snprintf(head, sizeof head,
                  "// Сегмент %d из %d: генератор ритма сигнального червя.\n"
                  "// Каждый сегмент качает свой S1 по синусоиде, а мышца рядом изгибается по S1. Сегмент отстаёт от\n"
                  "// предыдущего на СДВИГ периода, поэтому волна изгиба бежит от головы к хвосту.\n"
                  "// Память1 — свой счёт тиков (у каждой клетки свой).\n"
                  "// входы: [S1..S8, масса, возраст, азот, связи, свет, пища, травма, память1..память4]\n"
                  "// ответ: [S1..S8, память1..память4]; null — оставить как было\n"
                  "ПЕРИОД = 120.0\n"
                  "СДВИГ = %.3f\n"
                  "СИЛА = 5          // в пути: слабая волна, червь плывёт почти прямо\n"
                  "СИЛА_У_ЕДЫ = 14   // рядом пища: сильная волна, червь кружит и остаётся у еды\n\n",
                  segment + 1, segments, (double)segment / segments);
    std::string s = head;
    s += "function on_tick(входы) {\n"
         "    t = входы[15] + 1\n"
         "    if t >= ПЕРИОД { t = t - ПЕРИОД }\n"
         "    сила = СИЛА\n"
         "    if входы[13] > 0.2 { сила = СИЛА_У_ЕДЫ }\n"
         "    // фаза 0..1 и синус параболами (без модуля math)\n"
         "    p = t / ПЕРИОД - СДВИГ\n"
         "    if p < 0 { p = p + 1 }\n"
         "    u = p * 2\n"
         "    s = 0\n"
         "    if u < 1 {\n"
         "        s = 4 * u * (1 - u)\n"
         "    } else {\n"
         "        u = u - 1\n"
         "        s = -4 * u * (1 - u)\n"
         "    }\n"
         "    return [сила * s, null, null, null, null, null, null, null, t, null, null, null]\n"
         "}\n";
    return s;
}

Genome signalWorm() {
    Genome g;
    g.name = "Сигнальный червь";
    const float stiff = 0.6f;
    const Color body{0.62f, 0.72f, 0.90f};
    auto growing = [&](int i, const char* label) -> Mode& {  // photosynthesises while the body develops
        Mode& m = setup(g, i, CellType::Photocyte, {0.58f, 0.74f, 0.50f});
        m.makeAdhesin = true;
        m.adhesinStiffness = stiff;
        m.splitMass = 1.78f;
        m.nutrientPriority = 3;
        g.labels.set(i, label);
        return m;
    };
    auto finalCell = [&](int i, CellType t, Color c, const char* label) -> Mode& {
        Mode& m = setup(g, i, t, c);
        m.splitMass = kNeverSplit;
        m.adhesinStiffness = stiff;
        g.labels.set(i, label);
        return m;
    };
    auto forward = [&](int i, int front, int back) {
        Mode& m = g.modes.edit(i);
        m.childMode[0] = (uint8_t)front;
        m.childMode[1] = (uint8_t)back;
    };

    constexpr int kSegments = 4;
    int next = 1;
    const int mouth = next++;
    finalCell(mouth, CellType::Phagocyte, {0.90f, 0.66f, 0.50f}, "Рот");
    const int muscle = next++;
    {
        Mode& m = finalCell(muscle, CellType::Myocyte, {0.88f, 0.42f, 0.50f}, "Мышца (изгиб по S1)");
        m.prog[PF_Bend] = {ProgInput::S1, ProgKind::Linear, 0.07f, 0.0f, 0.0f};
        m.nutrientPriority = 2;
    }
    const int cilia = next++;
    {
        Mode& m = finalCell(cilia, CellType::Ciliocyte, {0.60f, 0.66f, 0.95f}, "Реснички");
        m.prog[PF_Bend] = {ProgInput::Fixed, ProgKind::Linear, 0.0f, 0.8f, 0.0f};
        m.prog[PF_Lift] = {ProgInput::Fixed, ProgKind::Linear, 0.0f, 0.8f, 0.0f};
    }
    int rhythm[kSegments];
    for (int k = 0; k < kSegments; ++k) {
        rhythm[k] = next++;
        std::string label = "Ритм " + std::to_string(k + 1) + (k == 0 ? " (голова)" : "");
        finalCell(rhythm[k], CellType::SignalCell, {0.45f, 0.60f, 0.95f}, label.c_str());
        g.scripts.set(rhythm[k], signalWormProgram(k, kSegments));
    }
    // Tail: fat for hungry times and a flagellum that pushes.
    const int fat = next++;
    finalCell(fat, CellType::Lipocyte, {0.95f, 0.86f, 0.56f}, "Жир").nutrientPriority = 0.4f;
    const int flag = next++;
    finalCell(flag, CellType::Flagellocyte, {0.44f, 0.66f, 0.92f}, "Жгутик").prog[PF_Swim].b = 0.5f;
    const int tail = next++;
    growing(tail, "Хвост");
    forward(tail, fat, flag);

    // The body front to back, grown by a chain of zones: zone k makes segment k in front and zone k+1 behind it.
    std::vector<int> parts = {mouth};
    for (int k = 0; k < kSegments; ++k) {
        parts.push_back(rhythm[k]);
        parts.push_back(muscle);
        parts.push_back(cilia);
    }
    const int firstZone = next;
    next += (int)parts.size() - 1;
    for (size_t k = 1; k < parts.size(); ++k) {
        const int z = firstZone + (int)k - 1;
        std::string label = "Зона роста " + std::to_string(k);
        growing(z, label.c_str());
        forward(z, parts[k], k + 1 < parts.size() ? z + 1 : tail);
    }
    growing(0, "Зигота");
    forward(0, parts[0], firstZone);
    g.modes.edit(0).color = body;
    g.initialMode = 0;
    assert(next <= kMaxModes);
    return g;
}

std::vector<GenomePreset> buildPresets() {
    std::vector<GenomePreset> v;
    v.push_back({"Водоросль", "Одиночный фотоцит. Делится при свете.", algae()});
    v.push_back({"Нить", "Фотоциты в жёсткой цепочке.", filament()});
    v.push_back({"Колония фагоцитов", "Плотная колония, питается пищей.", colony()});
    v.push_back({"Пловец", "Фагоцит со жгутиком; отпочковывает потомков.",
                 swimmer(CellType::Phagocyte, "Пловец", {0.62f, 0.80f, 0.44f})});
    v.push_back({"Хищник", "Деворoцит со жгутиком — охотится на клетки.",
                 swimmer(CellType::Devorocyte, "Хищник", {0.90f, 0.40f, 0.40f})});
    v.push_back({"Плавучая водоросль", "Фотоциты с буйоцитом — всплывают при гравитации.", buoyAlgae()});
    v.push_back({"Броненосец", "Фагоциты под защитой кератиноцитов.", armored()});
    v.push_back({"Жировой запас", "Фотоциты, запасающие массу в липоцитах.", lipidStore()});
    v.push_back({"Клейкий мат", "Глюеоциты и фотоциты слипаются в мат.", glueMat()});
    v.push_back({"Червь", "Нейроцит-осциллятор изгибает миоцитное тело.", worm()});
    v.push_back({"Чума", "Вироцит, превращающий жертв в вироциты.", plague()});
    v.push_back({"Хищник-охотник", "Челюсть и глаз-стереоцит; поворачивает к зелёной добыче, почкуется.", hunter()});
    v.push_back({"Паразитический жук", "Яйцо ждёт хозяина, личинка присасывается, клеится и прогрызает ткань, откладывает яйца.",
                 parasiteBeetle()});
    // Organisms written in dypt (scripts/organisms.dpt -> organisms.dll): the worms with a nervous system.
    for (const ScriptedOrganism& o : scriptedOrganisms()) v.push_back({o.name.c_str(), o.description.c_str(), o.genome});
    // Last, so the indices of the others stay as they were.
    v.push_back({"Сигнальный червь", "Ритм задают сигнальные клетки с программой на dypt: в пути слабая волна изгиба, у еды сильная — червь кружит и ест. Плывёт на ресничках и жгутике.",
                 signalWorm()});
    return v;
}

void scatter(World& w, Rng& rng, const Genome& g, int count, float radiusFrac) {
    float R = w.settings.dishRadius * radiusFrac;
    for (int i = 0; i < count; ++i) {
        float a = rng.range(0, kTau), r = std::sqrt(rng.uniform()) * R;
        w.addCell(g, fromAngle(a) * r, rng.range(0, kTau));
    }
}

void expEmpty(World& w, Rng&) { w.reset(Settings{}); }

void expAlgae(World& w, Rng& rng) {
    Settings s;
    s.nutrientRate = 0;
    s.lightAmount = 1.2f;
    s.lightRange = 0.7f;
    w.reset(s);
    scatter(w, rng, algae(), 12, 0.6f);
    scatter(w, rng, filament(), 4, 0.5f);
}

void expBroth(World& w, Rng& rng) {
    Settings s;
    s.nutrientRate = 8;
    s.lightAmount = 0.3f;
    w.reset(s);
    for (int i = 0; i < 300; ++i) {
        float a = rng.range(0, kTau), r = std::sqrt(rng.uniform()) * s.dishRadius * 0.95f;
        w.addFood(fromAngle(a) * r, s.chunkSize * kFoodUnit);
    }
    scatter(w, rng, colony(), 3, 0.5f);
    scatter(w, rng, swimmer(CellType::Phagocyte, "Пловец", {0.62f, 0.80f, 0.44f}), 6, 0.6f);
}

void expPredators(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.nutrientRate = 4;
    s.lightAmount = 1.0f;
    w.reset(s);
    scatter(w, rng, algae(), 30, 0.8f);
    scatter(w, rng, swimmer(CellType::Devorocyte, "Хищник", {0.90f, 0.40f, 0.40f}), 4, 0.4f);
    scatter(w, rng, armored(), 3, 0.6f);
}

void expGravity(World& w, Rng& rng) {
    Settings s;
    s.gravity = 0.6f;
    s.densityGradient = -1.0f;
    s.lightAmount = 1.3f;
    s.lightRange = 0.35f;
    s.nutrientRate = 0;
    w.reset(s);
    scatter(w, rng, buoyAlgae(), 6, 0.5f);
    scatter(w, rng, algae(), 10, 0.5f);
}

void expEvolution(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.15f;  // algae must not take over the dish
    s.radiation = 0.03f;
    s.contaminate = true;
    s.nutrientRate = 4;
    s.lightAmount = 1.0f;
    s.lightDirChange = 0.3f;
    s.cellAging = true;
    w.reset(s);
    scatter(w, rng, algae(), 8, 0.6f);
    scatter(w, rng, colony(), 3, 0.6f);
}

// Grown adults of a dypt organism, each placed where it fits without touching anything.
void adults(World& w, Rng& rng, const char* name, int count, float minR, float maxR, int maxCells = 1200) {
    const Genome* g = scriptedGenome(name);
    if (!g) return;
    GenomeLayout L = buildGenomeLayout(*g, 160, maxCells);
    for (int k = 0; k < count; ++k) spawnGrownOrganismFree(w, *g, L, rng, minR, maxR);
}

void expWorms(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.dishRadius = 2600.0f;
    s.lightAmount = 0.9f;
    s.lightRange = 0.6f;
    s.nutrientRate = 1.5f;
    s.maxFood = 5000;  // worms eat little: the dish must not silt up with food
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    // Adult worms (grown from the genome's developmental layout) so they swim right away. Their genomes are
    // written in dypt and come from organisms.dll.
    adults(w, rng, "Червь-гигант", 1, 0, 900, 4000);
    adults(w, rng, "Нематода", 4, 300, 2000);
    adults(w, rng, "Тонкий червь", 5, 300, 2200);
    adults(w, rng, "Нитчатый червь", 5, 300, 2200);
    scatter(w, rng, algae(), 4, 0.9f);
}

// Pond: a school of goldfish, carnivorous trap plants waiting with open jaws, nematodes hunting algae.
void expPond(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.03f;  // algae must not take over the dish
    s.dishRadius = 2400.0f;
    s.lightAmount = 1.1f;
    s.lightRange = 0.7f;
    s.nutrientRate = 1.5f;
    s.maxFood = 5000;  // worms eat little: the dish must not silt up with food
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    adults(w, rng, "Хищная водоросль", 5, 300, 1900);
    adults(w, rng, "Золотая рыбка", 6, 200, 1700);
    adults(w, rng, "Нематода", 3, 400, 1800);
    adults(w, rng, "Разумник", 4, 300, 1700);
    adults(w, rng, "Тонкий червь", 3, 300, 1800);
    adults(w, rng, "Нитчатый червь", 3, 300, 1800);
    scatter(w, rng, algae(), 12, 0.9f);
}

// Reef: hydras with stinging tentacles wait for passers-by, slipper ciliates graze, electric eels hunt the swimmers.
void expReef(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.05f;
    s.dishRadius = 2000.0f;
    s.lightAmount = 1.0f;
    s.lightRange = 0.8f;
    s.nutrientRate = 1.0f;
    s.maxFood = 2500;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    adults(w, rng, "Гидра", 8, 300, 1700);
    adults(w, rng, "Электрический угорь", 3, 300, 1600);
    adults(w, rng, "Инфузория-туфелька", 8, 200, 1700);
    scatter(w, rng, swimmer(CellType::Phagocyte, "Пловец", {0.62f, 0.80f, 0.44f}), 12, 0.85f);
    scatter(w, rng, algae(), 6, 0.9f);
}

// Abyss: 12-hour days. By day euglenas swim to the light; at night the anglerfish light their lures and the noctiluca
// glow — and every euglena heads for the nearest light.
void expAbyss(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.08f;
    s.dishRadius = 1500.0f;
    s.lightAmount = 0.9f;
    s.lightRange = 0.75f;
    s.dayLength = 12.0f;
    s.nutrientRate = 0.12f;  // a few crumbs for the anglers' mouths; euglenas and noctiluca live on light
    s.maxFood = 2500;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    adults(w, rng, "Удильщик", 5, 200, 1300);
    adults(w, rng, "Ночесветка", 10, 200, 1400);
    adults(w, rng, "Эвглена", 30, 200, 1400);
}

// Tardigrades graze food patches and lay cysts; nematodes and leeches hunt them. Cysts wake up only next to food.
void expTardigrades(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.05f;
    s.dishRadius = 1600.0f;
    s.lightAmount = 0.5f;
    s.nutrientRate = 1.2f;
    s.lumpiness = 60.0f;  // food comes in patches: feast here, famine there
    s.lumpSize = 0.08f;
    s.maxFood = 3000;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    adults(w, rng, "Тихоходка", 10, 200, 1400);
    adults(w, rng, "Пиявка", 3, 300, 1400);
    adults(w, rng, "Нематода", 2, 400, 1400);
    adults(w, rng, "Инфузория-туфелька", 6, 200, 1400);
}

// Blood in the water: piranhas follow the scent of wounded cells, leeches latch on to whoever passes.
// A river: water runs round the dish along its rim, a whirlpool turns in the middle. Algae drift with it, swimmers
// fight it or ride it, food rains in and is carried round. (The map editor draws and erases currents.)
void expRiver(World& w, Rng& rng) {
    Settings s;
    s.dishRadius = 1800.0f;
    s.nutrientRate = 2;
    s.lightAmount = 0.9f;
    s.lightCapacity = 0.06f;
    w.reset(s);
    const float R = s.dishRadius;
    for (int pass = 0; pass < 3; ++pass)  // the river: a ring of strokes, counter-clockwise
        for (int k = 0; k < 48; ++k)
            w.paintFlow(fromAngle(k * kTau / 48) * (R * 0.72f), fromAngle((k + 1) * kTau / 48) * (R * 0.72f), R * 0.22f, 0.9f);
    w.addVortex({0, 0}, R * 0.3f, -0.7f);  // the whirlpool turns the other way
    for (int i = 0; i < 200; ++i) w.addFood(fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * R * 0.95f), s.chunkSize * kFoodUnit);
    scatter(w, rng, algae(), 24, 0.9f);
    scatter(w, rng, swimmer(CellType::Phagocyte, "Пловец", {0.62f, 0.80f, 0.44f}), 6, 0.8f);
    scatter(w, rng, colony(), 3, 0.5f);
}

void expBlood(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.05f;
    s.dishRadius = 2000.0f;
    s.lightAmount = 1.0f;
    s.lightRange = 0.7f;
    s.nutrientRate = 1.5f;
    s.maxFood = 4000;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    adults(w, rng, "Пиранья", 4, 300, 1700);
    adults(w, rng, "Пиявка", 4, 300, 1700);
    adults(w, rng, "Золотая рыбка", 5, 300, 1700);
    adults(w, rng, "Тонкий червь", 4, 300, 1800);
    adults(w, rng, "Инфузория-туфелька", 8, 200, 1800);
    scatter(w, rng, algae(), 8, 0.9f);
}

// Test range for the nematode: the dish is split by walls into four rooms, one behaviour per room.
//   top left  "Стена":    an adult swims straight at a wall — does it turn away instead of ramming it?
//   top right "Пища":     a heap of food ahead-left within eyesight — does it steer to it and eat?
//   bottom left "Добыча": a cluster of green cells that do not divide — does it hunt them with its fangs?
//   bottom right "Развитие": a zygote — does it lie still while growing and wake up once the tail is built?
void expNematodeTest(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.05f;
    s.dishRadius = 2600.0f;
    s.lightAmount = 0.9f;
    s.lightRange = 1.0f;
    // A thin food rain: the tests still turn on what is laid out, but nematodes no longer starve to pieces after a day.
    s.nutrientRate = 0.4f;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    const Genome* g = scriptedGenome("Нематода");
    if (!g) return;
    GenomeLayout L = buildGenomeLayout(*g);
    // rooms
    w.addBarrier({-2700, 0}, {2700, 0}, 10.0f);
    w.addBarrier({0, -2700}, {0, 2700}, 10.0f);
    // "Стена": head toward the vertical wall, 650 µm away
    spawnGrownOrganism(w, *g, L, {-1100, 1100}, 0.0f);
    // "Пища": food heap ahead and a little to the left of the head
    spawnGrownOrganism(w, *g, L, {600, 900}, 0.0f);
    for (int i = 0; i < 120; ++i) w.addFood(Vec2{1300, 1000} + fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * 120.0f), 3.0f);
    // "Добыча": green cells that never divide, ahead of the head
    spawnGrownOrganism(w, *g, L, {-700, -1000}, kPi);
    Genome prey = algae();
    prey.name = "Добыча";
    prey.modes.edit(0).splitMass = kNeverSplit;
    for (int i = 0; i < 16; ++i)
        w.addCell(prey, Vec2{-1500, -900} + fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * 140.0f), rng.range(0, kTau),
                  -1, CellTag::Challenge);
    // "Развитие": the nematode's growing cells eat food, not light, so the zygote lies in a food patch to grow on.
    w.addCell(*g, {1200, -1200}, kPi * 0.5f);
    for (int i = 0; i < 560; ++i)  // a dense core it touches at once and a wide patch the growing body reaches
        w.addFood(Vec2{1200, -1200} + fromAngle(rng.range(0, kTau)) * (std::sqrt(rng.uniform()) * (i < 160 ? 90.0f : 350.0f)), 3.0f);
}

// Clever creatures in a maze: walls drawn into the dish, lumps of food, and nematodes hunting them. The clever ones
// steer to food, turn away from walls and flee the red hunters.
void expClever(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.dishRadius = 1600.0f;
    s.lightAmount = 0.8f;
    s.nutrientRate = 4;
    s.lumpiness = 70;
    s.lumpSize = 0.05f;
    s.viscosity = 5;
    s.swimAnisotropy = 5.0f;
    w.reset(s);
    // maze: a broken ring and a few straight walls
    const float ringR = 900.0f;
    for (int k = 0; k < 40; ++k) {
        if (k % 10 >= 8) continue;  // gaps to swim through
        w.addBarrier(fromAngle(k * kTau / 40) * ringR, fromAngle((k + 1) * kTau / 40) * ringR, 6.0f);
    }
    w.addBarrier({-500, -250}, {-100, -250}, 6.0f);
    w.addBarrier({150, 300}, {550, 300}, 6.0f);
    w.addBarrier({-300, 450}, {-300, 750}, 6.0f);
    adults(w, rng, "Разумник", 8, 100, 1450);
    // Nematodes (a millimetre long) hunt in the outer corridor, between the ring and the rim.
    adults(w, rng, "Нематода", 2, 1000, 1450);
}

void expHunt(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.lightAmount = 1.2f;
    s.lightRange = 0.8f;
    s.nutrientRate = 1;
    w.reset(s);
    scatter(w, rng, algae(), 40, 0.85f);
    scatter(w, rng, filament(), 6, 0.7f);
    scatter(w, rng, hunter(), 4, 0.3f);
}

void expParasites(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.lightAmount = 1.3f;
    s.lightRange = 1.0f;
    s.nutrientRate = 0;
    w.reset(s);
    scatter(w, rng, filament(), 10, 0.7f);
    scatter(w, rng, algae(), 20, 0.8f);
    scatter(w, rng, parasiteBeetle(), 12, 0.8f);
}

void expPlague(World& w, Rng& rng) {
    Settings s;
    s.lightCapacity = 0.06f;  // algae must not take over the dish
    s.nutrientRate = 0;
    s.lightAmount = 1.2f;
    w.reset(s);
    scatter(w, rng, filament(), 10, 0.7f);
    scatter(w, rng, armored(), 4, 0.7f);
    scatter(w, rng, plague(), 2, 0.2f);
}

}  // namespace

const std::vector<GenomePreset>& genomePresets() {
    static const std::vector<GenomePreset> p = buildPresets();
    return p;
}

const std::vector<Experiment>& experiments() {
    static const std::vector<Experiment> e = {
        {"Пустая чашка", "Стандартный субстрат без клеток. Чистый лист. Не испортите. Или испортите — это же наука.", expEmpty},
        {"Водоросли", "Только свет: фотоциты и нити. Самый спокойный эксперимент. Пока водоросли не захватят мир.", expAlgae},
        {"Питательный бульон", "Много пищи, колонии и пловцы. Шведский стол для простейших.", expBroth},
        {"Хищник и жертва", "Водоросли, хищники и бронированные колонии. Природа, как она есть: кто-то ест, кого-то едят.", expPredators},
        {"Гравитация", "Свет сверху, плотность растёт ко дну. Всплыть или утонуть — вечный вопрос буйоцита.", expGravity},
        {"Эволюция", "Радиация, заражение, старение — смотрите, кто выживет. Ставки принимаются.", expEvolution},
        {"Эпидемия", "Вироциты среди нитей и броненосцев. Мойте органеллы перед едой.", expPlague},
        {"Разумники", "Умные существа в лабиринте: ищут пищу, обходят стены и убегают от нематод. Умнее, чем кажутся. Глупее, чем думают о себе.", expClever},
        {"Пруд", "Рыбки, водоросли-ловушки, нематоды, разумники и тонкие черви. Желания рыбки не исполняют: они микроскопические.", expPond},
        {"Гигантские черви", "Червь-гигант, нематоды, тонкие и нитчатые черви. Извиваются, ищут пищу и не понимают, зачем вы на них смотрите.", expWorms},
        {"Охота", "Хищники-охотники преследуют водоросли. Водоросли не убегают: они растения. Охотники всё равно гордятся.", expHunt},
        {"Паразиты", "Яйца паразитических жуков среди колоний водорослей. Не смотрите на ночь.", expParasites},
        {"Риф", "Гидры жалят прохожих, туфельки пасутся, электрические угри бьют током всех подряд. Риф — это сложно.", expReef},
        {"Полигон: нематода", "Четыре отсека — четыре экзамена для нематоды: стена прямо по курсу, кучка пищи, зелёная добыча и зигота, которая растёт. Без шпаргалок.", expNematodeTest},
        {"Бездна", "Сутки по 12 часов. Днём эвглены греются на свету, ночью загораются ночесветки и приманки удильщиков — и эвглены плывут на огонёк. Мораль ищите сами.", expAbyss},
        {"Тихоходки", "Пища лежит пятнами, тихоходки бродят от пятна к пятну и откладывают цисты. Пиявки и нематоды охотятся. Цисты ждут лучших времён — они умеют ждать.", expTardigrades},
        {"Кровь в воде", "Пираньи чуют кровь, пиявки цепляются ко всем подряд, рыбки делают вид, что ничего не происходит. Обычный вторник в пруду.", expBlood},
        {"Река", "Вода бежит кругом вдоль края чашки, в середине крутится водоворот. Водоросли плывут по течению, пловцы гребут против. Течения рисуются и стираются в редакторе карты.", expRiver},
    };
    return e;
}
