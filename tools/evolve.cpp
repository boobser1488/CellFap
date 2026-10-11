// Overnight evolution: several "islands" (dishes with different environments) evolve side by side without a window.
//
// Usage: evolve [real-hours=10] [threads=3]
// Stop early by creating a file named STOP in <exe dir>/evolution.
//
// Every island starts from random genomes plus a few simple viable cells. Radiation mutates the offspring, cell aging
// keeps the population turning over, contamination brings in fresh random genomes. An island that dies out is
// re-seeded. Once a simulated day the most complex organism of each island migrates to the next island (ring).
//
// Output (next to the game, so it can open everything):
//   evolution/journal.txt                     — a readable journal (Russian), one line per island per 24 sim hours
//   evolution/stats.csv                       — numbers for charts
//   genomes/Эволюция — <остров> — рекорд.genome.txt   — the most complex organism seen on the island
//   genomes/Эволюция — <остров> — лидер.genome.txt    — the most numerous genome right now
//   substrates/Эволюция — <остров>.substrate          — the whole dish (overwritten every save)
#include "sim/presets.hpp"
#include "sim/serialize.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

struct Island {
    const char* name;
    const char* about;
    Settings settings;
    std::unique_ptr<World> world;
    Rng rng{1};
    // records
    float bestScore = 0;
    int bestSize = 0, bestTypes = 0;
    double bestAt = 0;
    std::string bestDesc;
    int reseeds = 0;
    int migrantsIn = 0;
};

FILE* gJournal = nullptr;
FILE* gCsv = nullptr;

void journal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fputs(buf, stdout);
    std::fflush(stdout);
    if (gJournal) {
        std::fputs(buf, gJournal);
        std::fflush(gJournal);
    }
}

// Simple genomes that certainly live: a phagocyte that divides, a photocyte that divides. They give the island a
// starting population for the mutations to work on.
Genome seedEater() {
    Genome g;
    g.name = "Затравка: фагоцит";
    Mode& m = g.modes.edit(0);
    m.type = CellType::Phagocyte;
    m.color = {0.62f, 0.78f, 0.42f};
    return g;
}
Genome seedAlga() {
    Genome g;
    g.name = "Затравка: водоросль";
    Mode& m = g.modes.edit(0);
    m.type = CellType::Photocyte;
    m.color = {0.40f, 0.78f, 0.38f};
    m.makeAdhesin = true;
    m.splitAngle = deg2rad(30);
    Mode& f = g.modes.edit(1);  // every other split leaves the filament: fragments float away
    f = m;
    f.makeAdhesin = false;
    m.childMode[1] = 1;
    return g;
}

void seed(Island& is, int randoms) {
    World& w = *is.world;
    float R = w.settings.dishRadius * 0.85f;
    auto at = [&] { return fromAngle(is.rng.range(0, kTau)) * (std::sqrt(is.rng.uniform()) * R); };
    for (int k = 0; k < randoms; ++k) w.addCell(randomGenome(is.rng, w.settings.genePool.data()), at(), is.rng.range(0, kTau));
    for (int k = 0; k < 4; ++k) w.addCell(seedEater(), at(), is.rng.range(0, kTau));
    for (const GenomePreset& p : genomePresets())  // a swimming eater reaches food patches the sessile ones never see
        if (std::string(p.name) == "Пловец")
            for (int k = 0; k < 4; ++k) w.addCell(p.genome, at(), is.rng.range(0, kTau));
    if (w.settings.lightAmount >= 0.3f)
        for (int k = 0; k < 4; ++k) w.addCell(seedAlga(), at(), is.rng.range(0, kTau));
    // a little food so heterotrophs get going
    for (int k = 0; k < 300; ++k) w.addFood(at(), w.settings.chunkSize * kFoodUnit);
}

// Organisms = groups of cells connected by adhesins.
struct Organism {
    int first = -1;  // a cell of it
    int size = 0;
    int genome = -1;  // genome of most cells
    uint32_t types = 0;
    int typeCount = 0;
    float score = 0;
};

std::vector<Organism> organisms(const World& w) {
    const auto& cells = w.cells();
    const auto& links = w.links();
    std::vector<int> comp(cells.size(), -1);
    std::vector<Organism> out;
    std::vector<int> stack;
    std::map<int, int> votes;
    for (size_t s = 0; s < cells.size(); ++s) {
        if (!cells[s].alive || comp[s] >= 0) continue;
        Organism o;
        o.first = (int)s;
        votes.clear();
        stack.assign(1, (int)s);
        comp[s] = (int)out.size();
        while (!stack.empty()) {
            int i = stack.back();
            stack.pop_back();
            const Cell& c = cells[i];
            ++o.size;
            o.types |= 1u << (int)c.type;
            votes[c.genome]++;
            for (int k = 0; k < c.linkCount; ++k) {
                const Link& l = links[c.links[k]];
                int j = l.a == i ? l.b : l.a;
                // only bonds made by division: cells glued into a heap are not one organism
                if (j >= 0 && l.fromDivision && comp[j] < 0 && cells[j].alive) {
                    comp[j] = comp[s];
                    stack.push_back(j);
                }
            }
        }
        int best = 0;
        int genomeVotes = 0;
        for (auto& [g, n] : votes)
            if (n > best) {
                best = n;
                o.genome = g;
                genomeVotes = n;
            }
        for (int t = 0; t < kCellTypeCount; ++t) o.typeCount += (o.types >> t) & 1;
        // Complexity: size counts up to 60 cells (a huge carpet is not a body), beyond that only division of labour does.
        o.score = std::sqrt((float)std::min(o.size, 60)) * (1.0f + 0.8f * (o.typeCount - 1));
        if (genomeVotes * 10 < o.size * 7) o.score *= 0.3f;  // a chimera of many mutants: not one body plan
        out.push_back(o);
    }
    return out;
}

std::string typesText(uint32_t types) {
    std::string s;
    for (int t = 0; t < kCellTypeCount; ++t)
        if ((types >> t) & 1) {
            if (!s.empty()) s += ", ";
            s += cellTypeInfo(t).name;
        }
    return s;
}

std::string fileSafe(std::string s) {
    for (char& ch : s)
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') ch = '_';
    return s;
}

Island makeIsland(const char* name, const char* about, Settings s, uint64_t seedValue) {
    // Shared evolution rules.
    s.radiation = 0.012f;      // ~7% of daughters carry a mutation
    s.cellAging = true;        // nobody lives forever: the population keeps turning over
    s.contaminate = true;      // a trickle of random newcomers
    s.maxCells = 4500;
    s.maxFood = 6000;
    s.viscosity = 10;
    s.swimAnisotropy = 4.0f;
    Island is;
    is.name = name;
    is.about = about;
    is.settings = s;
    is.rng = Rng(seedValue);
    return is;
}

}  // namespace

int main(int argc, char** argv) {
    double realHours = argc > 1 ? std::atof(argv[1]) : 10.0;
    unsigned threads = argc > 2 ? (unsigned)std::atoi(argv[2]) : 3u;
#ifdef _WIN32
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);  // the PC stays responsive overnight
    // Keep the computer awake while evolving (the screen may still turn off). The first overnight run was lost: the PC
    // went to sleep after a few minutes. Released automatically when the process exits.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#endif
    const std::string dir = dataDir("evolution");
    const std::string stopFile = dir + "/STOP";
    std::filesystem::remove(std::filesystem::path(std::u8string(stopFile.begin(), stopFile.end())));
    gJournal = openFileUtf8(dir + "/journal.txt", "a");
    gCsv = openFileUtf8(dir + "/stats.csv", "a");
    if (gCsv) std::fprintf(gCsv, "real_h,island,sim_h,cells,organisms,genomes,biggest,mean_size,best_types,best_score,food\n");
    JobPool jobs(threads);

    std::vector<Island> islands;
    {
        // 1. Sunny shallows: light with day and night — storage, filaments, phototaxis.
        Settings s;
        s.dishRadius = 1300;
        s.lightAmount = 1.2f;
        s.lightRange = 0.5f;
        s.dayLength = 16;
        s.lightCapacity = 0.12f;
        s.nutrientRate = 1.0f;
        s.nitrates = 0.8f;
        islands.push_back(makeIsland("Солнечная отмель",
                                     "свет полчашки, смена дня и ночи (16 ч), немного пищи", s, 0xA11CEull));
    }
    {
        // 2. Food patches: little light, food comes in rich patches — motion and eyes pay off.
        Settings s;
        s.dishRadius = 1300;
        s.lightAmount = 0.05f;  // almost dark: only eaters and hunters
        s.nutrientRate = 3.0f;
        s.lumpiness = 25;
        s.lumpSize = 0.06f;
        s.mobileFood = true;
        islands.push_back(makeIsland("Пищевые пятна", "темнота, пища падает пятнами и дрейфует", s, 0xB0B0ull));
    }
    {
        // 3. Predator reef: light and food, cells crush and bite each other — armour and size.
        Settings s;
        s.dishRadius = 1500;
        s.lightAmount = 0.9f;
        s.lightRange = 0.8f;
        s.lightCapacity = 0.04f;  // algae leave room for the animals
        s.nutrientRate = 2.0f;
        s.mechanicalDamage = 1.0f;
        islands.push_back(makeIsland("Хищный риф", "свет и пища для всех, сильные удары и раны — гонка брони и зубов", s,
                                     0xC0FFEEull));
    }
    {
        // 4. Deep water: gravity, light only at the top, food sinks to the bottom — floating and diving.
        Settings s;
        s.dishRadius = 1300;
        s.gravity = 1.0f;
        s.density = 0.0f;
        s.densityGradient = -1.0f;
        s.lightAmount = 1.0f;
        s.lightRange = 0.6f;
        s.lightAngle = kPi * 0.5f;
        s.nutrientRate = 3.0f;
        s.salinity = 0.6f;
        islands.push_back(makeIsland("Глубина", "гравитация, свет только сверху, солёная вода", s, 0xDEE9ull));
    }

    for (Island& is : islands) {
        is.world = std::make_unique<World>(&jobs);
        is.world->reset(is.settings);
        seed(is, 40);
    }

    auto now = [] { return Clock::now(); };
    // Real time actually spent evolving: a round that took suspiciously long (the PC slept anyway) counts as one minute.
    double activeSec = 0;
    auto roundStart = now();
    auto realElapsedH = [&] { return activeSec / 3600.0; };
    journal("\n=== Ночная эволюция: %zu острова, %.1f ч реального времени, потоков %u ===\n", islands.size(), realHours,
            jobs.threadCount());
    for (Island& is : islands) journal("  • %s — %s\n", is.name, is.about);

    double lastSave = 0, lastArchive = 0;
    int round = 0;
    bool stop = false;
    while (!stop) {
        ++round;
        {
            auto t = now();
            activeSec += std::min(60.0, std::chrono::duration<double>(t - roundStart).count());
            roundStart = t;
        }
        // One simulated hour per island per round.
        for (Island& is : islands) {
            World& w = *is.world;
            for (int t = 0; t < 400; ++t) w.step();
            if (w.cellCount() < 25) {  // extinction (or nearly): new settlers
                seed(is, 30);
                ++is.reseeds;
                journal("[%5.2f ч] %s: жизнь почти угасла (%d клеток) — новые поселенцы, %d-й раз\n", realElapsedH(), is.name,
                        w.cellCount(), is.reseeds);
            }
        }

        bool daily = round % 24 == 0;
        // Records: the most complex organism (checked every 4 simulated hours).
        if (round % 4 == 0) {
            for (Island& is : islands) {
                World& w = *is.world;
                std::vector<Organism> org = organisms(w);
                for (const Organism& o : org) {
                    if (o.size < 4 || o.score <= is.bestScore * 1.15f) continue;
                    is.bestScore = o.score;
                    is.bestSize = o.size;
                    is.bestTypes = o.typeCount;
                    is.bestAt = w.timeHours();
                    is.bestDesc = typesText(o.types);
                    Genome g = w.genomes().get(o.genome);
                    char nm[160];
                    std::snprintf(nm, sizeof nm, "Эво %s, %.0f ч, %d кл.", is.name, w.timeHours(), o.size);
                    g.name = nm;
                    saveGenome(g, dataDir("genomes") + "/" + fileSafe(std::string("Эволюция — ") + is.name + " — рекорд") +
                                      ".genome.txt");
                    journal("[%5.2f ч] %s, %.0f ч: новый рекорд сложности — организм из %d клеток, %d типов (%s)\n",
                            realElapsedH(), is.name, w.timeHours(), o.size, o.typeCount, is.bestDesc.c_str());
                }
            }
        }

        if (daily) {
            // Daily report + migration of each island's most complex current organism to the next island.
            std::vector<Genome> migrants(islands.size());
            std::vector<bool> has(islands.size(), false);
            for (size_t k = 0; k < islands.size(); ++k) {
                Island& is = islands[k];
                World& w = *is.world;
                std::vector<Organism> org = organisms(w);
                int biggest = 0, multi = 0;
                double sumSize = 0;
                const Organism* top = nullptr;
                for (const Organism& o : org) {
                    biggest = std::max(biggest, o.size);
                    sumSize += o.size;
                    if (o.size >= 3) ++multi;
                    if (!top || o.score > top->score) top = &o;
                }
                // most numerous genome
                std::map<int, int> perGenome;
                for (const Cell& c : w.cells())
                    if (c.alive) perGenome[c.genome]++;
                int leader = -1, leaderN = 0;
                for (auto& [g, n] : perGenome)
                    if (n > leaderN) {
                        leaderN = n;
                        leader = g;
                    }
                if (leader >= 0) {
                    Genome g = w.genomes().get(leader);
                    char nm[160];
                    std::snprintf(nm, sizeof nm, "Эво %s, лидер %.0f ч", is.name, w.timeHours());
                    g.name = nm;
                    saveGenome(g, dataDir("genomes") + "/" + fileSafe(std::string("Эволюция — ") + is.name + " — лидер") +
                                      ".genome.txt");
                }
                int types[kCellTypeCount] = {};
                for (const Cell& c : w.cells())
                    if (c.alive) types[(int)c.type]++;
                std::string mix;
                std::vector<std::pair<int, int>> order;
                for (int t = 0; t < kCellTypeCount; ++t)
                    if (types[t]) order.push_back({types[t], t});
                std::sort(order.rbegin(), order.rend());
                for (size_t q = 0; q < order.size() && q < 5; ++q) {
                    char b[64];
                    std::snprintf(b, sizeof b, "%s%s %d", q ? ", " : "", cellTypeInfo(order[q].second).name, order[q].first);
                    mix += b;
                }
                journal("[%5.2f ч] %-17s сутки %3d: %4d клеток, %4d организмов (многоклеточных %d, крупнейший %d), геномов %d, "
                        "лидер %d кл. | %s\n",
                        realElapsedH(), is.name, (int)(w.timeHours() / 24), w.cellCount(), (int)org.size(), multi, biggest,
                        w.genomes().liveCount(), leaderN, mix.c_str());
                if (gCsv) {
                    std::fprintf(gCsv, "%.3f,%s,%.1f,%d,%d,%d,%d,%.2f,%d,%.2f,%zu\n", realElapsedH(), is.name, w.timeHours(),
                                 w.cellCount(), (int)org.size(), w.genomes().liveCount(), biggest,
                                 org.empty() ? 0.0 : sumSize / org.size(), top ? top->typeCount : 0, top ? top->score : 0.0f,
                                 w.food().size());
                    std::fflush(gCsv);
                }
                if (top && top->size >= 3) {
                    migrants[k] = w.genomes().get(top->genome);
                    has[k] = true;
                }
            }
            for (size_t k = 0; k < islands.size(); ++k) {
                if (!has[k]) continue;
                Island& dst = islands[(k + 1) % islands.size()];
                World& w = *dst.world;
                for (int q = 0; q < 3; ++q) {
                    Vec2 p = fromAngle(dst.rng.range(0, kTau)) * (std::sqrt(dst.rng.uniform()) * w.settings.dishRadius * 0.8f);
                    w.addCell(migrants[k], p, dst.rng.range(0, kTau));
                }
                dst.migrantsIn++;
            }
        }

        double el = realElapsedH();
        // Whole dishes: every 30 real minutes (overwritten), so the game can open the latest state any time.
        if (el - lastSave >= 0.5 || el >= realHours) {
            lastSave = el;
            for (Island& is : islands)
                saveWorld(*is.world, (dataDir("substrates") + "/" + fileSafe(std::string("Эволюция — ") + is.name) + ".substrate").c_str());
        }
        // Record genomes archived every 3 real hours (a few files, to see how they changed during the night).
        if (el - lastArchive >= 3.0) {
            lastArchive = el;
            for (Island& is : islands) {
                std::string src = dataDir("genomes") + "/" + fileSafe(std::string("Эволюция — ") + is.name + " — рекорд") + ".genome.txt";
                char suffix[32];
                std::snprintf(suffix, sizeof suffix, " — %02.0f ч", el);
                std::string dst = dataDir("genomes") + "/" + fileSafe(std::string("Эволюция — ") + is.name + " — рекорд") + suffix + ".genome.txt";
                Genome g;
                if (loadGenome(g, src)) saveGenome(g, dst);
            }
        }
        if (el >= realHours) stop = true;
        if (FILE* f = openFileUtf8(stopFile, "rb")) {
            std::fclose(f);
            stop = true;
            journal("[%5.2f ч] найден файл STOP — останавливаюсь\n", el);
        }
    }

    journal("\n=== Итоги (%.2f ч реального времени) ===\n", realElapsedH());
    for (Island& is : islands) {
        World& w = *is.world;
        journal("  • %s: %.0f ч симуляции, %d клеток; рекорд — %d клеток, %d типов (%s) на %.0f ч; переселений сюда %d, "
                "перезаселений %d\n",
                is.name, w.timeHours(), w.cellCount(), is.bestSize, is.bestTypes, is.bestDesc.c_str(), is.bestAt, is.migrantsIn,
                is.reseeds);
    }
    journal("Чашки: substrates/Эволюция — <остров>.substrate; геномы: genomes/Эволюция — …\n");
    if (gJournal) std::fclose(gJournal);
    if (gCsv) std::fclose(gCsv);
    return 0;
}
