// Headless simulation runner: runs each experiment and prints population and timing.
// Usage: sim_bench [hours=20] [experiment-index|-1 for all]
#include "sim/cellab_import.hpp"
#include "sim/layout.hpp"
#include "sim/presets.hpp"
#include "sim/serialize.hpp"

#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    float hours = argc > 1 ? (float)std::atof(argv[1]) : 20.0f;
    int only = argc > 2 ? std::atoi(argv[2]) : -1;
    JobPool jobs(argc > 3 ? (unsigned)std::atoi(argv[3]) : 0u);
    std::printf("threads: %u\n", jobs.threadCount());
    if (only == -4 && argc > 4) {
        // Import a Cell Lab substrate / genome file and simulate a little.
        std::string path = argv[4];
        if (path.size() > 7 && path.substr(path.size() - 7) == ".genome") {
            Genome g;
            std::string err;
            if (!importCellLabGenome(g, path, err)) {
                std::printf("genome import failed: %s\n", err.c_str());
                return 1;
            }
            std::printf("genome ok: initial M%d type %s\n", g.initialMode + 1, cellTypeInfo(g.modes[g.initialMode].type).latin);
            return 0;
        }
        World w(&jobs);
        CellLabImportInfo info;
        std::string err;
        auto t0 = std::chrono::steady_clock::now();
        if (!importCellLabSubstrate(w, path, info, err)) {
            std::printf("import failed: %s\n", err.c_str());
            return 1;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("import ok in %.0f ms: version %d cells %d (dead skipped %d) food %d genomes %d unknownTypes %d %s\n", ms,
                    info.version, info.cells, info.skippedCells, info.food, info.genomes, info.unknownTypes, info.note.c_str());
        std::printf("dish radius %.0f µm, light %.2f, nutrients %.1f, viscosity %.1f, max cells %d\n", w.settings.dishRadius,
                    w.settings.lightAmount, w.settings.nutrientRate, w.settings.viscosity, w.settings.maxCells);
        int links = 0;
        for (const Link& l : w.links()) links += l.alive;
        std::printf("links %d, age %.1f h\n", links, w.timeHours());
        for (int t = 0; t < (int)(hours / kHoursPerTick); ++t) w.step();
        std::printf("after %.1f h: cells %d [", hours, w.cellCount());
        for (int k = 0; k < kCellTypeCount; ++k)
            if (w.countOfType(k)) std::printf(" %s:%d", cellTypeInfo(k).latin, w.countOfType(k));
        std::printf(" ]\n");
        return 0;
    }
    if (only == -5) {
        // Adult giant worm: does it move? Track centroid and heading.
        const GenomePreset* wp = nullptr;
        std::string want = argc > 4 ? argv[4] : "Червь-гигант";
        for (auto& p : genomePresets())
            if (want == p.name) wp = &p;
        if (!wp && !want.empty() && std::isdigit((unsigned char)want[0])) {
            int k = std::atoi(want.c_str());
            if (k >= 0 && k < (int)genomePresets().size()) wp = &genomePresets()[k];
        }
        if (!wp) { std::printf("no preset %s\n", want.c_str()); return 1; }
        World w(&jobs);
        Settings s;
        s.dishRadius = 3000;
        s.nutrientRate = 0;
        s.lightAmount = 1.4f;
        s.lightRange = 1.0f;
        s.viscosity = 30;
        w.reset(s);
        GenomeLayout L = buildGenomeLayout(wp->genome, 160, 4000);
        int n = spawnGrownOrganism(w, wp->genome, L, {0, 0}, 0.0f);
        std::printf("spawned %d cells (layout %zu stages)\n", n, L.stages.size() - 1);
        auto centroid = [&] {
            Vec2 c{};
            int k = 0;
            for (const Cell& cl : w.cells())
                if (cl.alive) { c += cl.pos; ++k; }
            return k ? c / (float)k : c;
        };
        Vec2 c0 = centroid();
        {
            int myo = 0, axial = 0, lateral = 0;
            for (size_t i = 0; i < w.cells().size(); ++i) {
                const Cell& cl = w.cells()[i];
                if (!cl.alive || cl.type != CellType::Myocyte) continue;
                ++myo;
                for (int k = 0; k < cl.linkCount; ++k) {
                    const Link& l = w.links()[cl.links[k]];
                    float anc = l.a == (int)i ? l.angA : l.angB;
                    if (std::fabs(std::cos(anc)) > 0.5f) ++axial; else ++lateral;
                }
            }
            std::printf("myocytes %d: axial links %d, lateral links %d\n", myo, axial, lateral);
        }
        int probe = -1;
        {
            float best = 1e30f;
            for (size_t i = 0; i < w.cells().size(); ++i) {
                const Cell& cl = w.cells()[i];
                if (!cl.alive || cl.type != CellType::Neurocyte) continue;
                float d = length2(cl.pos - c0);  // the ganglion nearest the middle of the body
                if (d < best) { best = d; probe = (int)i; }
            }
        }
        auto bend = [&] {  // RMS lateral distance of neurocytes from the straight line through the body
            float s2 = 0; int k = 0;
            for (const Cell& cl : w.cells())
                if (cl.alive && cl.type == CellType::Neurocyte) { float dy = cl.pos.y - c0.y; s2 += dy * dy; ++k; }
            return k ? std::sqrt(s2 / k) : 0.0f;
        };
        std::printf("ganglion S1 trace:");
        for (int t = 0; t < 400; ++t) {
            w.step();
            if (t % 20 == 0 && probe >= 0) std::printf(" %.1f/%.1f(b%.0f)", w.cells()[probe].signals[0], w.cells()[probe].signals[2], bend());
        }
        std::printf("\n");
        for (int q = 1; q <= 4; ++q) {
            for (int t = 0; t < (int)(0.25f / kHoursPerTick); ++t) w.step();
            Vec2 c = centroid();
            int links = 0;
            for (const Link& l : w.links()) links += l.alive;
            float s1 = 0, mn = 1e9f, mx = -1e9f, vmax = 0;
            int mus = 0;
            for (const Cell& cl : w.cells()) {
                if (!cl.alive) continue;
                vmax = std::max(vmax, length(cl.vel));
                if (cl.type == CellType::Myocyte) { s1 += cl.signals[0]; ++mus; mn = std::min(mn, cl.signals[0]); mx = std::max(mx, cl.signals[0]); }
            }
            std::printf("   muscle S1 range [%.2f, %.2f], max cell speed %.3f\n", mn, mx, vmax);
            std::printf("t=%.2fh centroid moved %.1f um (%.1f,%.1f) cells %d links %d meanS1(muscle) %.2f\n", q * 0.25f,
                        length(c - c0), c.x - c0.x, c.y - c0.y, w.cellCount(), links, mus ? s1 / mus : 0.f);
        }
        return 0;
    }
    if (only == -3) {
        // Editor layout: stages, cells, build time and worst remaining overlap of the grown organism.
        for (const GenomePreset& p : genomePresets()) {
            auto t0 = std::chrono::steady_clock::now();
            GenomeLayout L = buildGenomeLayout(p.genome);
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const LayoutStage& last = L.stages.back();
            float worst = 0;
            for (size_t i = 0; i < last.cells.size(); ++i)
                for (size_t j = i + 1; j < last.cells.size(); ++j) {
                    float ri = radiusForMass(last.cells[i].mass, CellType::Phagocyte);
                    float rj = radiusForMass(last.cells[j].mass, CellType::Phagocyte);
                    float ov = (ri + rj - length(last.cells[i].pos - last.cells[j].pos)) / (ri + rj);
                    if (ov > worst) {
                        worst = ov;
                        bool linked = false;
                        int dup = 0;
                        for (const LayoutLink& l : last.links) {
                            if ((l.a == (int)i && l.b == (int)j) || (l.a == (int)j && l.b == (int)i)) { linked = true; dup++; }
                        }
                        if (ov > 0.5f) std::printf("    pair %zu-%zu linked=%d dup=%d modes %d/%d d=%.2f ri=%.1f rj=%.1f\n", i, j, linked, dup,
                                                   last.cells[i].mode, last.cells[j].mode, length(last.cells[i].pos - last.cells[j].pos), ri, rj);
                    }
                }
            std::printf("%-22s stages=%2zu cells=%4zu links=%4zu build=%.2f ms worstOverlap=%.0f%%\n", p.name, L.stages.size() - 1,
                        last.cells.size(), last.links.size(), ms, worst * 100);
        }
        return 0;
    }
    if (only == -2) {
        // Grow each preset genome alone in a fed, lit dish and report what it became.
        for (const GenomePreset& p : genomePresets()) {
            World w(&jobs);
            Settings s;
            s.nutrientRate = 6;
            s.lightAmount = 1.0f;
            w.reset(s);
            Rng rng(7);
            for (int i = 0; i < 1500; ++i) {
                float a = rng.range(0, kTau), r = std::sqrt(rng.uniform()) * s.dishRadius;
                w.addFood(fromAngle(a) * r, s.chunkSize * kFoodUnit);
            }
            w.addCell(p.genome, {0, 0}, 0.0f);
            int ticks = (int)(hours / kHoursPerTick);
            for (int t = 0; t < ticks; ++t) w.step();
            int links = 0;
            for (const Link& l : w.links()) links += l.alive;
            std::printf("%-22s cells=%5d links=%5d [", p.name, w.cellCount(), links);
            for (int k = 0; k < kCellTypeCount; ++k)
                if (w.countOfType(k)) std::printf(" %s:%d", cellTypeInfo(k).latin, w.countOfType(k));
            float maxV = 0;
            for (const Cell& c : w.cells())
                if (c.alive) maxV = std::max(maxV, length(c.vel));
            std::printf(" ] maxV=%.2f\n", maxV);
        }
        return 0;
    }
    const auto& exps = experiments();
    for (int e = 0; e < (int)exps.size(); ++e) {
        if (only >= 0 && e != only) continue;
        World w(&jobs);
        Rng rng(42 + e);
        exps[e].setup(w, rng);
        std::printf("\n== %s ==\n", exps[e].name);
        auto t0 = std::chrono::steady_clock::now();
        int ticks = (int)(hours / kHoursPerTick);
        double worst = 0;
        for (int t = 0; t < ticks; ++t) {
            auto a = std::chrono::steady_clock::now();
            w.step();
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
            if (ms > worst) worst = ms;
            if (t % (int)(2.0f / kHoursPerTick) == 0) {
                int links = 0;
                for (const Link& l : w.links()) links += l.alive;
                float mass = 0, maxSpeed = 0;
                for (const Cell& c : w.cells())
                    if (c.alive) {
                        mass += c.mass;
                        maxSpeed = std::max(maxSpeed, length(c.vel));
                    }
                std::printf("  t=%6.1fh cells=%6d links=%6d food=%6zu biomass=%8.1f maxV=%5.2f genomes=%d  [", w.timeHours(),
                            w.cellCount(), links, w.food().size(), mass, maxSpeed, w.genomes().liveCount());
                for (int k = 0; k < kCellTypeCount; ++k)
                    if (w.countOfType(k)) std::printf(" %s:%d", cellTypeInfo(k).latin, w.countOfType(k));
                std::printf(" ]\n    phases ms: grid %.2f pre %.2f contact %.2f feed %.2f link %.2f integ %.2f serial %.2f\n",
                            w.phaseMs[0], w.phaseMs[1], w.phaseMs[2], w.phaseMs[3], w.phaseMs[4], w.phaseMs[5], w.phaseMs[6]);
                std::fflush(stdout);
            }
            // Sanity: no NaNs, nobody escapes the dish.
            if (t % 200 == 0)
                for (const Cell& c : w.cells())
                    if (c.alive && (!(c.pos.x == c.pos.x) || length(c.pos) > w.settings.dishRadius * 1.5f)) {
                        std::printf("  !! cell %u invalid pos (%f,%f) type %s\n", c.id, c.pos.x, c.pos.y, cellTypeInfo(c.type).latin);
                        return 1;
                    }
        }
        double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %d ticks in %.2fs (%.3f ms/tick avg, worst %.2f ms), final cells=%d\n", ticks, total, total * 1000 / ticks,
                    worst, w.cellCount());
    }
}
