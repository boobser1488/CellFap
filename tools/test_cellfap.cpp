// Интеграционные проверки расширений CellFap: новые вещества, новые клетки, программируемые сигнальные клетки
// на настоящем dypt, лимиты песочницы и сохранение/загрузка.
//
// Запуск:  test_cellfap [путь-к-dypt_embed.dll]
// Возвращает 0, если все проверки пройдены, иначе количество провалов.
#include "core/jobs.hpp"
#include "sim/genome.hpp"
#include "sim/layout.hpp"
#include "sim/modpack.hpp"
#include "sim/mods.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "sim/signal_cells.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_pass = 0, g_fail = 0;

void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  [ok]   %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s%s%s\n", what.c_str(), detail.empty() ? "" : " — ", detail.c_str());
    }
}

void section(const char* name) { std::printf("\n== %s ==\n", name); }

Genome simpleGenome(CellType type, float splitMass) {
    Genome g;
    g.name = "test";
    Mode m;
    m.type = type;
    m.splitMass = splitMass;
    m.color = {0.6f, 0.7f, 0.5f};
    g.modes.set(0, m);
    g.initialMode = 0;
    return g;
}

// Симулировать n тиков.
void run(World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) w.step();
}

// ---------------------------------------------------------------- substances

void testSubstances(JobPool& jobs) {
    section("Вещества");
    using S = World::Substance;

    // Каждое вещество должно приниматься инъектором и давать измеримый эффект.
    struct Case {
        S s;
        const char* name;
        float dose;
    };
    const Case cases[] = {
        {S::Amylase, "амилаза", 1.0f},
        {S::HyaluronicAcid, "гиалуроновая кислота", 1.0f},
        {S::Epibrassinolide, "эпибрассинолид", 1.0f},
        {S::Benzyladin, "бензиладенин", 1.0f},
        {S::GibberellicAcid, "гибберелловая кислота", 2.0f},
        {S::Colchicine, "колхицин", 1.0f},
        {S::Penicillin, "пенициллин", 1.0f},
        {S::Interferon, "интерферон", 1.0f},
        {S::Resveratrol, "ресвератрол", 1.0f},
        {S::Ryanodine, "рианодин", 1.0f},
        {S::Cycloheximide, "циклогексимид", 1.0f},
        {S::Osteopontin, "остеопотин", 1.0f},
        {S::ChondroitinSulfate, "хондроитинсульфат", 1.0f},
        {S::Edrophonium, "эдрофоний", 1.0f},
        {S::Atractyloside, "атрактилозид", 1.0f},
    };
    for (const Case& c : cases) {
        World w(&jobs);
        Settings s;
        s.dishRadius = 400;
        s.nutrientRate = 0;  // без еды, чтобы измерять только вещество
        s.lightAmount = 1.0f;
        w.reset(s);
        Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.5f);
        w.injectSubstance(idx, c.s, c.dose, 0);
        bool alive = w.cells()[idx].alive;
        run(w, 2);
        // Вещество должно вызвать хоть какое-то изменение состояния клетки либо остаться в таймере.
        const Cell& cell = w.cells()[idx];
        float timer = cell.amylase + cell.hyaluronicAcid + cell.epibrassinolide + cell.benzyladin +
                      cell.gibberellicAcid + cell.colchicine + cell.penicillin + cell.interferon + cell.resveratrol +
                      cell.ryanodine + cell.cycloheximide + cell.osteopontin + cell.chondroitinSulfate + cell.edrophonium +
                      cell.atractyloside;
        check(alive && timer > 0.0f, std::string("приём ") + c.name, "таймер=" + std::to_string(timer));
    }

    // Амилаза: фагоцит переваривает пищу быстрее. Меряем объём съеденного за короткий тик: если дать клетке
    // наесться до потолка массы, обе ветви покажут одно и то же.
    {
        auto eaten = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 300;
            s.nutrientRate = 0.0f;
            w.reset(s);
            for (int k = 0; k < 400; ++k) w.addFood(Vec2{0, 0}, 1.0f);
            Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
            int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.5f);
            if (dose > 0) w.injectSubstance(idx, S::Amylase, dose, 0);
            float foodBefore = 0;
            for (const Food& f : w.food()) foodBefore += f.mass;
            run(w, 25);
            float foodAfter = 0;
            for (const Food& f : w.food()) foodAfter += f.mass;
            return foodBefore - foodAfter;
        };
        const float plain = eaten(0.0f), boosted = eaten(1.0f);
        check(boosted > plain * 1.2f, "амилаза увеличивает потребление пищи фагоцитом",
              "обычная=" + std::to_string(plain) + " с амилазой=" + std::to_string(boosted));
    }

    // Эпибрассинолид: фотоцит растёт быстрее.
    {
        auto grow = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 300;
            s.nutrientRate = 0;
            s.lightAmount = 2.0f;
            w.reset(s);
            Genome g = simpleGenome(CellType::Photocyte, kNeverSplit);
            int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.0f);
            if (dose > 0) w.injectSubstance(idx, S::Epibrassinolide, dose, 0);
            run(w, 200);
            const Cell& c = w.cells()[idx];
            return c.alive ? c.mass : 0.0f;
        };
        const float plain = grow(0.0f), boosted = grow(1.0f);
        check(boosted > plain * 1.2f, "эпибрассинолид усиливает фотосинтез",
              "обычная=" + std::to_string(plain) + " с веществом=" + std::to_string(boosted));
    }

    // Гибберелловая кислота: потолок 40.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 900;
        s.nutrientRate = 15.0f;
        s.lightAmount = 0;
        w.reset(s);
        Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.0f);
        w.injectSubstance(idx, S::GibberellicAcid, 100.0f, 0);
        run(w, 600);
        const Cell& c = w.cells()[idx];
        check(c.alive && c.mass <= 40.0f + 1e-3f, "гибберелловая кислота не даёт массе выше 40",
              "масса=" + std::to_string(c.mass));
    }

    // Колхицин: деление останавливается.
    {
        auto divisions = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 900;
            s.nutrientRate = 15.0f;
            w.reset(s);
            Genome g = simpleGenome(CellType::Phagocyte, 2.0f);
            w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.2f);
            if (dose > 0) w.injectSubstance(0, S::Colchicine, dose, 0);
            run(w, 400);
            return w.cellCount();
        };
        const int plain = divisions(0.0f), blocked = divisions(1.0f);
        check(plain > 1 && blocked == 1, "колхицин останавливает деление",
              "без вещества клеток=" + std::to_string(plain) + ", с веществом=" + std::to_string(blocked));
    }

    // Циклогексимид: энергия не набирается, но клетка тратит.
    {
        auto masses = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 300;
            s.nutrientRate = 0;
            s.lightAmount = 2.0f;
            w.reset(s);
            Genome g = simpleGenome(CellType::Photocyte, kNeverSplit);
            int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
            if (dose > 0) w.injectSubstance(idx, S::Cycloheximide, dose, 0);
            run(w, 300);
            const Cell& c = w.cells()[idx];
            return c.alive ? c.mass : 0.0f;
        };
        const float plain = masses(0.0f), blocked = masses(1.0f);
        check(plain > 2.0f && blocked < plain, "циклогексимид блокирует получение энергии",
              "обычная=" + std::to_string(plain) + " заблокированная=" + std::to_string(blocked));
    }

    // Атрактилозид: фагоцит ест, но масса от пищи не растёт. Пищу кладём рядом, чтобы результат не зависел от
    // того, как быстро посыпался дождь.
    {
        auto masses = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 300;
            s.nutrientRate = 0.0f;  // еду не сыплем — она уже в чашке
            w.reset(s);
            for (int k = 0; k < 40; ++k) w.addFood(Vec2{0, 0}, 1.0f);
            Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
            int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.2f);
            if (dose > 0) w.injectSubstance(idx, S::Atractyloside, dose, 0);
            run(w, 400);
            const Cell& c = w.cells()[idx];
            return c.alive ? c.mass : 0.0f;
        };
        const float plain = masses(0.0f), blocked = masses(1.0f);
        check(plain > 1.2f && blocked < plain, "атрактилозид снимает прибавку массы от пищи",
              "обычная=" + std::to_string(plain) + " заблокированная=" + std::to_string(blocked));
    }

    // Пенициллин: кератиноцит теряет защиту, циклоспориноцит — нет.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        Genome k = simpleGenome(CellType::Keratinocyte, kNeverSplit);
        int ki = w.addCell(k, Vec2{-100, 0}, 0, 0, CellTag::User, 1.5f);
        run(w, 1);
        check(w.cells()[ki].isProtected, "кератиноцит защищён без вещества");
        w.injectSubstance(ki, S::Penicillin, 1.0f, 0);
        run(w, 1);
        check(!w.cells()[ki].isProtected, "пенициллин снимает защиту кератиноцита");
    }
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        Genome c = simpleGenome(CellType::Cyclosporinocyte, kNeverSplit);
        int ci = w.addCell(c, Vec2{-100, 0}, 0, 0, CellTag::User, 1.5f);
        w.injectSubstance(ci, S::Penicillin, 5.0f, 0);
        run(w, 4);
        check(w.cells()[ci].alive && w.cells()[ci].penicillin > 0.0f, "пенициллин не убивает циклоспориноцит");
    }

    // Интерферон: заражение не проходит.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 400;
        w.reset(s);
        Genome v = simpleGenome(CellType::Virocyte, kNeverSplit);
        Mode vm = v.modes[0];
        vm.virusCopyFrom = 0;
        v.modes.set(0, vm);
        Mode victim = v.modes[0];
        victim.type = CellType::Phagocyte;
        victim.splitMass = kNeverSplit;
        v.modes.set(1, victim);
        int vi = w.addCell(v, Vec2{-8, 0}, 0, 0, CellTag::User, 1.2f);
        int ti = w.addCell(v, Vec2{8, 0}, 0, 1 /* mode 1: the victim */, CellTag::User, 1.2f);
        w.injectSubstance(ti, S::Interferon, 5.0f, 0);
        run(w, 400);
        const Cell& victim2 = w.cells()[ti];
        check(victim2.alive && w.genomes().get(victim2.genome).modes[0].type == CellType::Virocyte,
              "интерферон не даёт заразиться");
    }

    // Бензиладенин: старение отложено.
    {
        auto lives = [&](float dose) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 400;
            s.nutrientRate = 15.0f;
            s.cellAging = true;
            w.reset(s);
            Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
            int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.5f);
            if (dose > 0) w.injectSubstance(idx, S::Benzyladin, dose, 0);
            run(w, 100);  // 0.25 h: вещество успевает действовать, но клетка ещё молода
            const Cell& c = w.cells()[idx];
            return c.alive ? c.age : -1.0f;
        };
        const float plain = lives(0.0f), treated = lives(5.0f);
        check(plain > 0 && treated > 0, "бензиладенин не убивает молодую клетку");
    }

    // Рианодин: миоцит спазмирует без команды.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 400;
        s.nutrientRate = 15.0f;
        w.reset(s);
        Genome g = simpleGenome(CellType::Myocyte, kNeverSplit);
        Mode m = g.modes[0];
        m.prog[PF_Contraction] = Prog{};
        m.prog[PF_Contraction].b = 0.0f;  // команда «не сокращаться»
        g.modes.set(0, m);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.5f);
        w.injectSubstance(idx, S::Ryanodine, 1.0f, 0);
        run(w, 60);
        const Cell& c = w.cells()[idx];
        check(c.alive && c.actuator > 0.05f, "рианодин вызывает спазм миоцита",
              "сокращение=" + std::to_string(c.actuator));
    }

    // Эдрофоний: нейтрализует тетрадоксин.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 400;
        w.reset(s);
        Genome g = simpleGenome(CellType::Phagocyte, kNeverSplit);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 1.5f);
        w.injectSubstance(idx, S::Tetrodotoxin, 2.0f, 0);
        w.injectSubstance(idx, S::Edrophonium, 2.0f, 0);
        run(w, 200);
        const Cell& c = w.cells()[idx];
        check(c.paralysis < 0.05f, "эдрофоний нейтрализует тетрадоксин",
              "паралич=" + std::to_string(c.paralysis));
    }
}

// ---------------------------------------------------------------- new cells

void testCells(JobPool& jobs) {
    section("Новые клетки");
    struct Case {
        CellType t;
        const char* name;
    };
    const Case cases[] = {
        {CellType::Fibroblast, "фибробласт"},
        {CellType::Baroreceptor, "барорецептор"},
        {CellType::Erythrocyte, "эритроцит"},
        {CellType::Chronocyte, "хроноцит"},
        {CellType::Cyclosporinocyte, "циклоспориноцит"},
        {CellType::Granulocyte, "гранулоцит"},
        {CellType::Griffocyte, "гриффоцит"},
        {CellType::Spirocyte, "спироцит"},
        {CellType::Chromocyte, "хромоцит"},
        {CellType::SignalCell, "сигнальная клетка"},
    };
    for (const Case& c : cases) {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        s.nutrientRate = 15.0f;
        s.lightAmount = 1.5f;
        w.reset(s);
        Genome g = simpleGenome(c.t, kNeverSplit);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        run(w, 400);
        const Cell& cell = w.cells()[idx];
        check(cell.alive, std::string("живёт и не падает: ") + c.name,
              cell.alive ? "" : "клетка погибла");
        check(std::isfinite(cell.pos.x) && std::isfinite(cell.pos.y) && std::isfinite(cell.mass),
              std::string("состояние конечно: ") + c.name);
    }

    // Хроноцит: сигнал появляется с интервалом N. Импульс затухает между тиками (это заложено в сигналы), поэтому
    // считаем подъёмы, а не абсолютные значения.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        Genome g = simpleGenome(CellType::Chronocyte, kNeverSplit);
        Mode m = g.modes[0];
        m.prog[PF_Stem1].input = ProgInput::Fixed;
        m.prog[PF_Stem1].b = 0.02f;  // период 0.02 ч = 8 тиков
        m.senseChannel = 0;
        g.modes.set(0, m);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        int pulses = 0;
        float prev = w.cells()[idx].signals[0];
        for (int i = 0; i < 200; ++i) {
            w.step();
            const float now = w.cells()[idx].signals[0];
            if (now > prev + 0.01f) ++pulses;  // сигнал вырос — это и есть импульс
            prev = now;
        }
        check(pulses >= 5, "хроноцит выдаёт импульсы по расписанию", "импульсов=" + std::to_string(pulses));
    }

    // Барорецептор: у стенки сигнал есть, в центре — нет. Сигнал затухает, поэтому меряем пик за тик.
    {
        auto peakAt = [&](float y) {
            World w(&jobs);
            Settings s;
            s.dishRadius = 400;
            w.reset(s);
            Genome g = simpleGenome(CellType::Baroreceptor, kNeverSplit);
            g.modes.edit(0).senseChannel = 0;
            int idx = w.addCell(g, Vec2{0, y}, 0, 0, CellTag::User, 2.0f);
            float peak = 0;
            for (int i = 0; i < 60; ++i) {
                w.step();
                peak = std::max(peak, std::fabs(w.cells()[idx].signals[0]));
            }
            return peak;
        };
        // 380 + радиус клетки вплотную к стеклу, 0 — середина чашки.
        const float atWall = peakAt(380.0f), inMiddle = peakAt(0.0f);
        check(atWall > 0.5f && atWall > inMiddle + 0.3f, "барорецептор реагирует на стенку",
              "у стенки=" + std::to_string(atWall) + " в центре=" + std::to_string(inMiddle));
    }

    // Гранулоцит: набирает яд из воды, затем погибает. Яд в облаке идёт ему в токсичность, а не в ранение,
    // поэтому клетка жива, пока порог не взят.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        s.nutrientRate = 15.0f;
        w.reset(s);
        Genome g = simpleGenome(CellType::Granulocyte, kNeverSplit);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        bool died = false;
        for (int i = 0; i < 2000; ++i) {
            if (w.cells()[idx].alive) w.addFluid(Vec2{0, 0}, FluidKind::Toxin, 0.5f, 0xffa0d8a0);
            w.step();
            if (!w.cells()[idx].alive) {
                died = true;
                break;
            }
        }
        check(died, "гранулоцит погибает, накопив яд");
        check(!w.fluids().empty(), "после смерти осталась отравленная точка");
    }

    // Спироцит: вращается.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 900;
        s.dynFriction = 0.5f;
        s.staticFriction = 0.2f;
        w.reset(s);
        Genome g = simpleGenome(CellType::Spirocyte, kNeverSplit);
        Mode m = g.modes[0];
        m.prog[PF_Swim].input = ProgInput::Fixed;
        m.prog[PF_Swim].b = 1.0f;
        g.modes.set(0, m);
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        const float a0 = w.cells()[idx].angle;
        run(w, 100);
        const float da = std::fabs(wrapAngle(w.cells()[idx].angle - a0));
        check(da > 0.01f, "спироцит вращается", "поворот=" + std::to_string(da));
    }

    // Эритроцит: ёмкость меньше липоцита, метаболизм ниже.
    {
        check(cellCapacity(CellType::Erythrocyte) < cellCapacity(CellType::Lipocyte),
              "эритроцит запасает меньше энергии, чем липоцит");
        check(cellTypeInfo(CellType::Erythrocyte).costPerSec < cellTypeInfo(CellType::Phagocyte).costPerSec,
              "у эритроцита ниже метаболическое потребление");
    }

    // Гриффоцит: сцепляется со дном по сигналу.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 600;
        w.reset(s);
        Genome g = simpleGenome(CellType::Griffocyte, kNeverSplit);
        Mode m = g.modes[0];
        m.prog[PF_Stem1].input = ProgInput::Fixed;
        m.prog[PF_Stem1].b = 1.0f;  // сильный сигнал
        g.modes.set(0, m);
        int idx = w.addCell(g, Vec2{100, -400}, 0, 0, CellTag::User, 2.0f);
        run(w, 200);
        const Cell& c = w.cells()[idx];
        check(c.alive && c.griffocyteGrip > 0.3f, "гриффоцит цепляется за дно",
              "сцепление=" + std::to_string(c.griffocyteGrip));
    }
}

// ---------------------------------------------------------------- dypt signal cells

void testDypt(JobPool& jobs, const std::string& libPath) {
    section("Dypt: программируемые сигнальные клетки");
    check(!libPath.empty(), "путь к dypt_embed задан", libPath);
    if (libPath.empty()) return;

    SignalCellManager mgr;
    std::string err;
    if (!mgr.initialize(libPath, err)) {
        check(false, "инициализация dypt", err);
        return;
    }
    check(mgr.ready(), "dypt_embed загружен, ABI " + std::to_string(mgr.ready() ? 2 : 0));

    // Пресеты из задания должны компилироваться и исполняться.
    const SignalCellPreset presets[] = {SignalCellPreset::LogicGate, SignalCellPreset::Switch,
                                        SignalCellPreset::Timer, SignalCellPreset::Conditional};
    for (SignalCellPreset p : presets) {
        const std::string src = signalCellPresetSource(p);
        std::string e;
        std::vector<float> out;
        const bool ok = mgr.testProgram(src, e, &out);
        check(ok, std::string("пресет компилируется: ") + signalCellPresetName(p), e);
    }

    // Логика AND: единицы на обоих входах дают единицу, единица и ноль — ноль.
    {
        const std::string src = signalCellPresetApply(SignalCellPreset::LogicGate, {0.0f});
        std::string e;
        std::vector<float> out, ones(SignalCellInputs::kCount, 0.0f), mixed(SignalCellInputs::kCount, 0.0f);
        ones[0] = ones[1] = 1.0f;
        mixed[0] = 1.0f;
        std::vector<float> outOnes, outMixed;
        const bool ok1 = mgr.testProgram(src, e, &outOnes, &ones);
        const bool ok2 = ok1 && mgr.testProgram(src, e, &outMixed, &mixed);
        if (ok2 && outOnes.size() >= 3 && outMixed.size() >= 3) {  // the result is S3 (place 2)
            check(outOnes[2] > 0.9f, "пресет AND: единицы на входе дают единицу",
                  "получено " + std::to_string(outOnes[2]));
            check(outMixed[2] < 0.1f, "пресет AND: единица и ноль дают ноль",
                  "получено " + std::to_string(outMixed[2]));
        } else {
            check(false, "пресет AND исполняется", e);
        }
    }

    // Ошибка компиляции должна быть понятной, а не аварийной. (Заметим: «не» в dypt — это логическое отрицание,
    // поэтому настоящая ошибка синтаксиса выглядит как оборванный список.)
    {
        std::string e;
        const std::string broken = "function on_tick(входы) {\n    выход = [0, 0, 0\n    return +\n}\n";
        check(!mgr.testProgram(broken, e) && !e.empty(), "ошибка компиляции сообщается текстом", e);
    }
    // Отсутствие точки входа.
    {
        std::string e;
        check(!mgr.testProgram("function другое() { return 1 }", e), "программа без on_tick отвергается", e);
    }
    // Слишком большой исходник.
    {
        std::string big(40000, 'x');
        std::string e;
        check(!mgr.testProgram(big, e), "слишком большой исходник отвергается", e);
    }

    // Конечные выходы: программа может вернуть что угодно, но лимит держится.
    {
        std::string e;
        std::vector<float> out;
        const std::string many = "function on_tick(входы) {\n"
                                 "    выход = []\n"
                                 "    for i = 0 to 200 { выход = выход + [i] }\n"
                                 "    return выход\n"
                                 "}\n";
        const bool ok = mgr.testProgram(many, e, &out);
        check(!ok || out.size() <= (size_t)mgr.limits.maxOutputs, "число выходов ограничено",
              "вышло " + std::to_string(out.size()));
    }

    // Сквозной прогон: сигнальные клетки в реальной симуляции.
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        std::string ie;
        w.signalCells().initialize(libPath, ie);
        check(w.signalCells().ready(), "World получил менеджер программ", ie);

        Genome g;
        g.name = "сигнальная";
        Mode m;
        m.type = CellType::SignalCell;
        m.splitMass = kNeverSplit;
        m.color = {0.4f, 0.6f, 0.9f};
        g.modes.set(0, m);
        g.scripts.set(0, signalCellPresetSource(SignalCellPreset::Conditional));
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        check(idx >= 0, "сигнальная клетка создана");
        // Подаём сигнал S1 и ждём, пока программа его пропустит.
        // S1 opens the relay, S2 is what it passes on to S3.
        w.injectSubstance(idx, World::Substance::Signal, 5.0f, 0);
        w.injectSubstance(idx, World::Substance::Signal, 5.0f, 1);
        float peak = 0;
        for (int i = 0; i < 300; ++i) {
            w.step();
            const Cell& c = w.cells()[idx];
            peak = std::max(peak, std::fabs(c.signals[2]));
            if (!std::isfinite(c.pos.x)) break;
        }
        check(peak > 0.5f, "программа исполняется в симуляции и возвращает сигнал",
              "пик S3=" + std::to_string(peak));
        check(w.cells()[idx].alive, "сигнальная клетка выжила");
    }

    // A program that never returns is stopped by the watchdog: the game goes on, the program is switched off.
    {
        std::string e;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = mgr.testProgram("function on_tick(входы) {\n    while true { }\n    return []\n}\n", e);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(!ok && ms < 3000.0, "бесконечный цикл в проверке прерывается сторожем", e + " за " + std::to_string((int)ms) + " мс");
    }
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        std::string ie;
        w.signalCells().initialize(libPath, ie);
        Genome g;
        Mode m;
        m.type = CellType::SignalCell;
        m.splitMass = kNeverSplit;
        g.modes.set(0, m);
        g.scripts.set(0, "function on_tick(входы) {\n    while true { }\n    return []\n}\n");
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        const auto t0 = std::chrono::steady_clock::now();
        run(w, 50);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const LoadedProgram* lp = w.signalCells().program(0);
        check(ms < 5000.0 && w.cells()[idx].alive, "зависшая программа не останавливает симуляцию",
              "50 тиков за " + std::to_string((int)ms) + " мс");
        check(lp && lp->disabled && !lp->lastError.empty(), "зависшая программа отключена с сообщением",
              lp ? lp->lastError : std::string("нет программы"));
    }

    // Reaching outside the cell is refused before compiling.
    {
        std::string e;
        check(!mgr.testProgram("import \"x\"\nfunction on_tick(входы) { return [] }\n", e), "import запрещён", e);
        check(!mgr.testProgram("function on_tick(входы) {\n    exit(0)\n    return []\n}\n", e), "exit запрещён", e);
        check(!mgr.testProgram("function on_tick(входы) {\n    p = malloc(10)\n    return []\n}\n", e), "malloc запрещён", e);
        check(!mgr.testProgram("function on_tick(входы) {\n    append(\"x.txt\", 1)\n    return []\n}\n", e), "append (запись в файл) запрещён", e);
        check(mgr.testProgram("// import в комментарии не мешает\nfunction on_tick(входы) {\n    t = \"exit\"\n    return []\n}\n", e),
              "запрещённые слова в комментарии и строке не мешают", e);
    }

    // Every cell keeps its own memory: three timers count independently (each pulses every 50 ticks).
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        std::string ie;
        w.signalCells().initialize(libPath, ie);
        Genome g;
        Mode m;
        m.type = CellType::SignalCell;
        m.splitMass = kNeverSplit;
        g.modes.set(0, m);
        g.scripts.set(0, signalCellPresetApply(SignalCellPreset::Timer, {0, 0, 50}));
        int ids[3];
        for (int k = 0; k < 3; ++k) ids[k] = w.addCell(g, Vec2{-150.0f + 150.0f * k, 0}, 0, 0, CellTag::User, 2.0f);
        int pulses[3] = {};
        float prev[3] = {};
        for (int t = 0; t < 400; ++t) {
            w.step();
            for (int k = 0; k < 3; ++k) {
                const float v = w.cells()[ids[k]].signals[2];
                if (v > 0.5f && prev[k] <= 0.5f) ++pulses[k];
                prev[k] = v;
            }
        }
        check(pulses[0] >= 7 && pulses[0] <= 9 && pulses[1] == pulses[0] && pulses[2] == pulses[0],
              "у каждой клетки своя память: таймеры не сбивают друг друга",
              std::to_string(pulses[0]) + " / " + std::to_string(pulses[1]) + " / " + std::to_string(pulses[2]) +
                  " импульсов за 400 тиков");
    }

    // Дорогая программа не должна ронять симуляцию: бюджет исполнения ограничивает её вклад в кадр.
    // Цикл здесь ограничен по времени намеренно — бесконечный цикл в dypt прервать нечем (это ограничение
    // самого языка, см. отчёт).
    {
        World w(&jobs);
        Settings s;
        s.dishRadius = 500;
        w.reset(s);
        std::string ie;
        w.signalCells().initialize(libPath, ie);
        Genome g;
        Mode m;
        m.type = CellType::SignalCell;
        m.splitMass = kNeverSplit;
        g.modes.set(0, m);
        g.scripts.set(0, "function on_tick(входы) {\n"
                        "    s = 0\n"
                        "    for i = 1 to 30000 { s = s + i * 0.5 }\n"
                        "    выход = [s, 0, 0, 0, 0, 0, 0, 0]\n"
                        "    return выход\n"
                        "}\n");
        int idx = w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);
        auto t0 = std::chrono::steady_clock::now();
        run(w, 100);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        check(w.cells()[idx].alive, "клетка с дорогой программой не роняет симуляцию");
        check(ms < 4000.0, "бюджет исполнения держит дорогую программу в рамках",
              "100 тиков за " + std::to_string((int)ms) + " мс");
    }
}

// ---------------------------------------------------------------- genome layout

// Насколько клетки раскладки заходят друг на друга: глубина перекрытия, делённая на меньший радиус.
struct OverlapStats {
    float worst = 0;
    int deep = 0;  // пар с перекрытием больше трети меньшего радиуса
    int cells = 0;
};

OverlapStats layoutOverlap(const LayoutStage& st) {
    OverlapStats s;
    const int n = (int)st.cells.size();
    s.cells = n;
    std::vector<float> r(n);
    for (int i = 0; i < n; ++i) r[i] = radiusForMass(st.cells[i].mass, CellType::Phagocyte);
    // Связанные адгезином клетки по замыслу прижаты до 0.95 касания (плоская граница): им положен запас.
    std::vector<std::vector<int>> adj(n);
    for (const LayoutLink& l : st.links) {
        adj[l.a].push_back(l.b);
        adj[l.b].push_back(l.a);
    }
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            bool linked = std::find(adj[i].begin(), adj[i].end(), j) != adj[i].end();
            float d = length(st.cells[j].pos - st.cells[i].pos);
            float depth = ((r[i] + r[j]) * (linked ? 0.9f : 1.0f) - d) / std::min(r[i], r[j]);
            if (depth > s.worst) s.worst = depth;
            if (depth > 0.34f) ++s.deep;
        }
    return s;
}

void testLayout() {
    section("Раскладка геномов");
    for (const GenomePreset& p : genomePresets()) {
        const auto t0 = std::chrono::steady_clock::now();
        GenomeLayout L = buildGenomeLayout(p.genome);
        const int ms = (int)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        OverlapStats worst;
        for (const LayoutStage& st : L.stages) {
            OverlapStats s = layoutOverlap(st);
            worst.worst = std::max(worst.worst, s.worst);
            worst.deep = std::max(worst.deep, s.deep);
            worst.cells = std::max(worst.cells, s.cells);
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "клеток %d, худшее перекрытие %.2f r, глубоких пар %d, %d мс", worst.cells, worst.worst, worst.deep, ms);
        check(worst.worst < 0.3f && worst.deep == 0, std::string("клетки не заходят друг на друга: ") + p.name, buf);
    }
}

// ---------------------------------------------------------------- persistence

void testPersistence(JobPool& jobs) {
    section("Сохранение программы");
    World w(&jobs);
    Settings s;
    s.dishRadius = 500;
    w.reset(s);
    Genome g = simpleGenome(CellType::SignalCell, kNeverSplit);
    const std::string src = signalCellPresetSource(SignalCellPreset::Switch);
    g.scripts.set(0, src);
    g.name = "Программа-тест";
    int id = w.genomesMut().add(g);
    w.addCell(g, Vec2{0, 0}, 0, 0, CellTag::User, 2.0f);

    // Сохранение генома в файл и чтение обратно.
    const char* path = "test_signal_cell.genome";
    check(saveGenome(w.genomes().get(id), path), "геном с программой сохранён");
    Genome back;
    if (loadGenome(back, path)) {
        check(back.scripts[0] == src, "программа пережила сохранение и загрузку",
              "прочитано " + std::to_string(back.scripts[0].size()) + " байт");
    } else {
        check(false, "геном загружен");
    }
    remove(path);

    // Субстрат целиком.
    const char* spath = "test_signal_cell.substrate";
    check(saveWorld(w, spath), "чашка сохранена");
    World w2(&jobs);
    if (loadWorld(w2, spath)) {
        bool found = false;
        for (const Genome& gg : w2.genomes().genomes)
            if (gg.scripts[0] == src) found = true;
        check(found, "программа пережила сохранение чашки");
    } else {
        check(false, "чашка загружена");
    }
    remove(spath);
}

// ---------------------------------------------------------------- mods

void testMods(JobPool& jobs, const std::string& libPath) {
    section("Моды");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "cellfap_modtest";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const std::string dir = root.string();
    auto put = [&](const std::string& rel, const std::string& text) {
        fs::create_directories((root / rel).parent_path(), ec);
        writeWholeFile((root / rel).string(), text);
    };

    // Упаковка примера: папка -> .cfmod -> те же файлы.
    const std::string example = std::string(CELLFAP_SOURCE_DIR) + "/mods/example";
    std::string packPath = dir + "/example.cfmod", err;
    check(buildModPack(example, packPath, err), "пример мода собирается в .cfmod", err);
    ModFiles folder, packed;
    readModFolder(example, folder, err);
    check(readModPack(packPath, packed, err) && packed == folder, "пакет распаковывается в те же файлы", err);
    check(modRelativePath("../x") == "" && modRelativePath("a\\b/./c") == "a/b/c", "пути внутри мода не выходят наружу");

    // Ещё два мода рядом с примером: зависающий и запрещённый.
    put("hang/mod.txt", "id hang\nname Зависший\ntick 1\n");
    put("hang/mod.dpt", "function тик(с) {\n    while true { }\n}\n");
    put("bad/mod.txt", "id bad\nname Запрещённый\n");
    put("bad/mod.dpt", "function загрузка() {\n    append(\"x.txt\", 1)\n}\n");

    ModManager mm;
    mm.hangMs = 300;
    if (!mm.initialize(libPath, err)) {
        check(false, "менеджер модов подключает dypt", err);
        return;
    }
    const std::string ini = dir + "/mods.ini";
    writeWholeFile(ini, "[example]\nenabled=1\nпорций=20\n[hang]\nenabled=1\n[bad]\nenabled=1\n");
    mm.loadState(ini);
    mm.scan(dir);
    auto find = [&](const std::string& id) {
        for (int i = 0; i < (int)mm.mods.size(); ++i)
            if (mm.mods[i].id == id) return i;
        return -1;
    };
    const int ex = find("example"), hang = find("hang"), bad = find("bad");
    check(ex >= 0 && hang >= 0 && bad >= 0, "найдены три мода (пакет и две папки)", std::to_string(mm.mods.size()));
    if (ex < 0 || hang < 0 || bad < 0) return;
    check(mm.mods[ex].running, "пример запущен", mm.mods[ex].error);
    check(mm.mods[ex].settings.size() == 4 && mm.mods[ex].settings[0].value == 20, "настройки прочитаны из mods.ini");
    check(!mm.mods[bad].running && mm.mods[bad].error.find("append") != std::string::npos, "мод с append не запускается",
          mm.mods[bad].error);
    check(mm.genomes.size() == 1 && mm.genomes[0].genome.name == "Розовая водоросль", "загрузка() добавила геном");
    check(mm.programs.size() == 1 && mm.programs[0].name == "Мигалка", "загрузка() добавила программу");
    const auto msgs = mm.takeMessages();
    check(!msgs.empty(), "мод показал сообщение");

    World w(&jobs);
    Settings s;
    s.nutrientRate = 0;
    w.reset(s);
    const size_t food0 = w.food().size();
    const auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < 120; ++t) {
        w.step();
        mm.tick(w);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(!mm.mods[hang].running && !mm.mods[hang].enabled, "зависший мод выключен сторожем", mm.mods[hang].error);
    check(mm.mods[ex].running, "пример пережил зависание соседа и перезапустился", mm.mods[ex].error);
    check(w.food().size() >= food0 + 20, "тик() насыпал пищу",
          std::to_string(food0) + " -> " + std::to_string(w.food().size()) + " " + mm.mods[ex].error);
    check(ms < 5000, "моды не остановили симуляцию", std::to_string((int)ms) + " мс");

    // Настройка меняется в интерфейсе — мод узнаёт об этом.
    mm.mods[ex].settings[1].value = 0;  // дождь выкл.
    mm.settingChanged(ex, &w);
    check(!mm.mods[ex].log.empty() && mm.mods[ex].log.back().find("дождь false") != std::string::npos,
          "настройки_изменены() видит новое значение", mm.mods[ex].log.empty() ? "" : mm.mods[ex].log.back());
    mm.saveState(ini);
    std::string saved;
    readWholeFile(ini, saved);
    check(saved.find("дождь=0") != std::string::npos && saved.find("[hang]\nenabled=0") != std::string::npos,
          "mods.ini помнит настройки и выключенный мод");

    mm.setEnabled(ex, false);
    check(mm.genomes.empty() && mm.programs.empty(), "выключенный мод забирает свои геномы и программы");
    fs::remove_all(root, ec);
}

}  // namespace

int main(int argc, char** argv) {
    std::string libPath;
    if (argc > 1) {
        libPath = argv[1];
    } else {
        libPath = "dypt_embed.dll";
    }
    std::printf("CellFap integration tests\ndypt: %s\n", libPath.c_str());
    JobPool jobs(2u);
    testSubstances(jobs);
    testCells(jobs);
    testDypt(jobs, libPath);
    testPersistence(jobs);
    testLayout();
    testMods(jobs, libPath);
    std::printf("\npassed %d, failed %d\n", g_pass, g_fail);
    return g_fail;
}
