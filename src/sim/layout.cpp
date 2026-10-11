#include "layout.hpp"

#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {

// Development only: a mode that divides into itself and lets the other child go (no adhesin kept) is budding —
// reproduction (gonads, budding swimmers), not growth, so the layout of the organism stops there.
bool divides(const Mode& m, int links, int self) {
    if (m.type == CellType::Gamete) return false;
    if (m.splitMass >= kNeverSplit) return false;
    for (int k = 0; k < 2; ++k)
        if (m.childMode[k] == self && !m.keepAdhesin[1 - k]) return false;
    return links < m.maxConnections;
}

// Mass a cell is shown with: the mass it reaches before dividing (or a grown-up size if it never divides).
float displayMass(const Mode& m, float birth) {
    if (m.splitMass >= kNeverSplit || m.type == CellType::Gamete) return std::min(kMaxCellMass, std::max(birth, birth * 1.4f));
    if (m.splitMass <= kSplitAtAge) return birth;
    return std::max(birth, m.splitMass);
}

// Deterministic position-based relaxation: adhesins hold their partners at contact distance, every other
// pair is pushed apart until it no longer overlaps. Candidate pairs are found once with a sweep along x.
void relax(LayoutStage& st) {
    const int n = (int)st.cells.size();
    if (n < 2) return;
    std::vector<float> r(n), w(n);
    float maxR = 1;
    for (int i = 0; i < n; ++i) {
        r[i] = radiusForMass(st.cells[i].mass, CellType::Phagocyte);
        w[i] = 1.0f / std::max(st.cells[i].mass, 0.3f);
        maxR = std::max(maxR, r[i]);
    }
    std::vector<std::vector<int>> adj(n);
    for (const LayoutLink& l : st.links) {
        adj[l.a].push_back(l.b);
        adj[l.b].push_back(l.a);
    }
    std::vector<int> order(n);
    std::vector<std::pair<int, int>> pairs;
    const float reach = maxR * 2.0f * 1.5f;
    // Candidate pairs via a sweep along x; refreshed periodically because crowds expand while relaxing.
    auto findPairs = [&] {
        for (int i = 0; i < n; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return st.cells[a].pos.x < st.cells[b].pos.x; });
        pairs.clear();
        for (int a = 0; a < n; ++a) {
            int i = order[a];
            for (int b = a + 1; b < n; ++b) {
                int j = order[b];
                if (st.cells[j].pos.x - st.cells[i].pos.x > reach) break;
                if (std::fabs(st.cells[j].pos.y - st.cells[i].pos.y) > reach) continue;
                bool linked = false;
                for (int o : adj[i])
                    if (o == j) linked = true;
                if (!linked) pairs.push_back({i, j});
            }
        }
    };
    const int iterations = n > 400 ? 30 : (n > 64 ? 50 : 40);
    // Free cells (no adhesin to anyone) drift toward the middle: a colony of separate cells packs into a disc instead
    // of lining up along their division axis.
    // The same goes for small separate groups (a budded zygote with its flagellum).
    std::vector<int> loose;
    {
        std::vector<int> comp(n, -1), stack;
        for (int s = 0; s < n; ++s) {
            if (comp[s] >= 0) continue;
            std::vector<int> members;
            stack.assign(1, s);
            comp[s] = s;
            while (!stack.empty()) {
                int i = stack.back();
                stack.pop_back();
                members.push_back(i);
                for (int o : adj[i])
                    if (comp[o] < 0) {
                        comp[o] = s;
                        stack.push_back(o);
                    }
            }
            if (members.size() <= 3) loose.insert(loose.end(), members.begin(), members.end());
        }
    }
    for (int it = 0; it < iterations; ++it) {
        if (it % 12 == 0) findPairs();
        if (!loose.empty()) {
            Vec2 c{};
            for (const LayoutCell& lc : st.cells) c += lc.pos;
            c = c / (float)n;
            for (int i : loose) {
                Vec2 d = c - st.cells[i].pos;
                float l = length(d);
                if (l > 1e-3f) st.cells[i].pos += d / l * std::min(l * 0.05f, r[i] * 0.5f);
            }
        }
        for (const LayoutLink& l : st.links) {
            Vec2& pa = st.cells[l.a].pos;
            Vec2& pb = st.cells[l.b].pos;
            Vec2 d = pb - pa;
            float dist = std::max(length(d), 1e-4f);
            float target = (r[l.a] + r[l.b]) * 0.95f + l.extra;
            float diff = (dist - target) / (w[l.a] + w[l.b]);
            Vec2 corr = d / dist * (diff * 0.6f);
            pa += corr * w[l.a];
            pb -= corr * w[l.b];
            // Angular stiffness: pull each partner toward its anchor direction (straight chains stay straight).
            float k = 0.04f + 0.16f * l.stiffness;
            Vec2 wantB = pa + fromAngle(st.cells[l.a].angle + l.angA) * target;
            Vec2 wantA = pb + fromAngle(st.cells[l.b].angle + l.angB) * target;
            float wsum = w[l.a] + w[l.b];
            pb += (wantB - pb) * (k * w[l.b] / wsum);
            pa += (wantA - pa) * (k * w[l.a] / wsum);
        }
        for (const auto& pr : pairs) {
            int i = pr.first, j = pr.second;
            Vec2 d = st.cells[j].pos - st.cells[i].pos;
            float minD = (r[i] + r[j]) * 0.97f;
            float d2 = length2(d);
            if (d2 >= minD * minD) continue;
            float dist = std::sqrt(d2);
            Vec2 nrm = dist > 1e-4f ? d / dist : fromAngle((float)(i * 3 + j));
            float diff = (minD - dist) / (w[i] + w[j]);
            st.cells[i].pos -= nrm * (diff * w[i]);
            st.cells[j].pos += nrm * (diff * w[j]);
        }
    }
}

}  // namespace

GenomeLayout buildGenomeLayout(const Genome& g, int maxStages, int maxCells) {
    GenomeLayout L;
    {
        LayoutStage s0;
        LayoutCell z;
        z.pos = {0, 0};
        z.angle = kPi * 0.5f;
        z.mode = (uint8_t)clampv(g.initialMode, 0, kMaxModes - 1);
        z.birthMass = 2.6f;  // what the synthesizer inserts
        z.mass = displayMass(g.modes[z.mode], z.birthMass);
        s0.cells.push_back(z);
        L.stages.push_back(std::move(s0));
    }

    for (int stage = 1; stage <= maxStages; ++stage) {
        const LayoutStage& prev = L.stages.back();
        const int n = (int)prev.cells.size();
        std::vector<int> degree(n, 0);
        for (const LayoutLink& l : prev.links) {
            degree[l.a]++;
            degree[l.b]++;
        }
        // Count divisions first and inflate the previous layout by the area growth, so children appear
        // roughly where they will end up and the relaxation only has local work to do.
        // Growth is anisotropic: each division lengthens the body along its split direction. The transform
        // T = I + (1/n) * sum(d d^T) doubles a chain along its axis and scales a blob by ~1.5 in all directions.
        int splitting = 0;
        bool anyChange = false;
        float mxx = 0, mxy = 0, myy = 0;
        for (int i = 0; i < n; ++i) {
            const LayoutCell& c = prev.cells[i];
            const Mode& m = g.modes[c.mode];
            if (!divides(m, degree[i], c.mode)) {
                if (m.type == CellType::Stemocyte && m.stemMode[0] != c.mode) anyChange = true;
                continue;
            }
            ++splitting;
            if (!m.makeAdhesin) {
                // The children part ways (separate organisms): they make room in every direction, like a colony
                // spreading as a disc. (Counting their split axis lined free-living algae up into a kilometre-long
                // queue that the editor had to zoom out for.)
                mxx += 0.41f;
                myy += 0.41f;
                continue;
            }
            Vec2 d = fromAngle(c.angle + (c.mirrored ? -1.0f : 1.0f) * m.splitAngle);
            mxx += d.x * d.x;
            mxy += d.x * d.y;
            myy += d.y * d.y;
        }
        if (splitting == 0 && !anyChange) break;
        mxx /= n;
        mxy /= n;
        myy /= n;
        Vec2 centroid{};
        for (const LayoutCell& c : prev.cells) centroid += c.pos;
        centroid = centroid / (float)n;
        auto inflated = [&](Vec2 p) {
            Vec2 r = p - centroid;
            return centroid + Vec2{r.x + mxx * r.x + mxy * r.y, r.y + mxy * r.x + myy * r.y};
        };

        LayoutStage next;
        std::vector<int> heir1(n, -1), heir2(n, -1);
        std::vector<char> didSplit(n, 0);
        std::vector<Vec2> splitDirOf(n);
        bool any = false;
        for (int i = 0; i < n; ++i) {
            const LayoutCell& c = prev.cells[i];
            const Mode& m = g.modes[c.mode];
            if (!divides(m, degree[i], c.mode) || (int)next.cells.size() + 2 > maxCells) {
                LayoutCell k = c;
                k.pos = inflated(c.pos);
                k.parent = i;
                k.born = false;
                if (m.type == CellType::Stemocyte && m.stemMode[0] != c.mode) {
                    k.mode = m.stemMode[0];
                    k.mass = displayMass(g.modes[k.mode], k.birthMass);
                    anyChange = true;
                }
                heir1[i] = (int)next.cells.size();
                next.cells.push_back(k);
                continue;
            }
            any = true;
            didSplit[i] = 1;
            float sgn = c.mirrored ? -1.0f : 1.0f;
            float splitDir = c.angle + sgn * m.splitAngle;
            Vec2 dir = fromAngle(splitDir);
            splitDirOf[i] = dir;
            float masses[2] = {c.mass * m.splitRatio, c.mass * (1.0f - m.splitRatio)};
            float rr[2] = {radiusForMass(masses[0], CellType::Phagocyte), radiusForMass(masses[1], CellType::Phagocyte)};
            float sep = (rr[0] + rr[1]) * 0.98f;
            float tot = masses[0] + masses[1];
            Vec2 base = inflated(c.pos);
            Vec2 pos[2] = {base + dir * (sep * masses[1] / tot), base - dir * (sep * masses[0] / tot)};
            for (int k = 0; k < 2; ++k) {
                if (masses[k] < kDeathMass) continue;  // would starve at birth
                LayoutCell ch;
                ch.pos = pos[k];
                ch.mode = m.childMode[k];
                ch.mirrored = c.mirrored != m.mirror[k];
                ch.angle = wrapAngle(splitDir + sgn * m.childAngle[k]);
                ch.birthMass = masses[k];
                ch.mass = displayMass(g.modes[ch.mode], masses[k]);
                ch.parent = i;
                ch.childSlot = k;
                ch.born = true;
                (k == 0 ? heir1 : heir2)[i] = (int)next.cells.size();
                next.cells.push_back(ch);
            }
        }
        if (!any && !anyChange) break;
        const bool full = (int)next.cells.size() >= maxCells;

        // Adhesin inheritance: each existing link goes to the child on its side of the split plane
        // (both when it lies on the plane), subject to "keep adhesin".
        std::vector<int> linkCount(next.cells.size(), 0);
        // The side of a link is decided by its stored anchor in the parent's frame, like World::splitCell.
        auto heirsToward = [&](int cell, float anchor, int out[2]) {
            int cnt = 0;
            if (!didSplit[cell]) {
                if (heir1[cell] >= 0) out[cnt++] = heir1[cell];
                return cnt;
            }
            const Mode& m = g.modes[prev.cells[cell].mode];
            float s = dot(fromAngle(prev.cells[cell].angle + anchor), splitDirOf[cell]);
            bool to1 = s > kPlaneShare, to2 = s < -kPlaneShare;
            if (!to1 && !to2) to1 = to2 = true;
            if (to1 && m.keepAdhesin[0] && heir1[cell] >= 0) out[cnt++] = heir1[cell];
            if (to2 && m.keepAdhesin[1] && heir2[cell] >= 0) out[cnt++] = heir2[cell];
            return cnt;
        };
        for (const LayoutLink& l : prev.links) {
            int ha[2], hb[2];
            int na = heirsToward(l.a, l.angA, ha);
            int nb = heirsToward(l.b, l.angB, hb);
            // Anchors follow the genome, not the momentary geometry: a child inherits its parent's anchor
            // direction expressed in its own frame (geometry-based anchors drift as chains bend).
            for (int x = 0; x < na; ++x)
                for (int y = 0; y < nb; ++y) {
                    if (linkCount[ha[x]] >= kMaxLinks || linkCount[hb[y]] >= kMaxLinks) continue;
                    linkCount[ha[x]]++;
                    linkCount[hb[y]]++;
                    LayoutLink nl{ha[x], hb[y], l.extra, l.stiffness, l.angA, l.angB};
                    if (didSplit[l.a]) nl.angA = wrapAngle(prev.cells[l.a].angle + l.angA - next.cells[ha[x]].angle);
                    if (didSplit[l.b]) nl.angB = wrapAngle(prev.cells[l.b].angle + l.angB - next.cells[hb[y]].angle);
                    // Like the simulation, re-aim anchors at the real geometry — but keep the genome's direction
                    // when they agree, so straight chains stay exact. Diagonals thus get diagonal anchors and are
                    // inherited by one child only instead of multiplying on every division.
                    {
                        const LayoutCell& ca = next.cells[ha[x]];
                        const LayoutCell& cb = next.cells[hb[y]];
                        Vec2 d = cb.pos - ca.pos;
                        if (length2(d) > 1e-6f) {
                            float phi = std::atan2(d.y, d.x);
                            float geoA = wrapAngle(phi - ca.angle), geoB = wrapAngle(phi + kPi - cb.angle);
                            const float tol = deg2rad(20.0f);
                            if (std::fabs(wrapAngle(geoA - nl.angA)) > tol) nl.angA = geoA;
                            if (std::fabs(wrapAngle(geoB - nl.angB)) > tol) nl.angB = geoB;
                        }
                    }
                    next.links.push_back(nl);
                }
        }
        for (int i = 0; i < n; ++i) {
            if (!didSplit[i]) continue;
            const Mode& m = g.modes[prev.cells[i].mode];
            if (m.makeAdhesin && heir1[i] >= 0 && heir2[i] >= 0 && linkCount[heir1[i]] < kMaxLinks &&
                linkCount[heir2[i]] < kMaxLinks) {
                // Child 1 sits along the split direction, child 2 opposite.
                float splitAng = std::atan2(splitDirOf[i].y, splitDirOf[i].x);
                LayoutLink nl{heir1[i], heir2[i], m.adhesinLength * 10.0f, m.adhesinStiffness};
                nl.angA = wrapAngle(splitAng + kPi - next.cells[heir1[i]].angle);
                nl.angB = wrapAngle(splitAng - next.cells[heir2[i]].angle);
                next.links.push_back(nl);
            }
        }
        relax(next);
        L.stages.push_back(std::move(next));
        if (full) break;  // stop unfolding at the editor's cell budget
    }
    // Big organisms (long worms) need more passes than one stage gives for the last insertions to settle.
    if (L.stages.back().cells.size() > 1200)
        for (int k = 0; k < 6; ++k) relax(L.stages.back());
    return L;
}

int spawnGrownOrganism(World& w, const Genome& g, const GenomeLayout& L, Vec2 pos, float angle) {
    if (L.stages.empty()) return 0;
    const LayoutStage& st = L.stages.back();
    Vec2 centroid{};
    for (const LayoutCell& c : st.cells) centroid += c.pos;
    centroid = centroid / (float)std::max<size_t>(1, st.cells.size());
    const float rot = angle - kPi * 0.5f;  // layouts face up (+y)
    const float cr = std::cos(rot), sr = std::sin(rot);
    std::vector<int> idx(st.cells.size(), -1);
    int first = -1, created = 0;
    for (size_t i = 0; i < st.cells.size(); ++i) {
        const LayoutCell& c = st.cells[i];
        Vec2 d = c.pos - centroid;
        Vec2 p = pos + Vec2{d.x * cr - d.y * sr, d.x * sr + d.y * cr};
        float a = c.angle + rot;
        float mass = std::min(c.mass, kMaxCellMass);
        int k = first < 0 ? w.addCell(g, p, a, c.mode, CellTag::User, mass) : w.addCellLike(first, p, a, c.mode, mass, c.mirrored);
        if (k < 0) break;
        if (first < 0) {
            first = k;
            w.cellsMut()[k].mirrored = c.mirrored;
        }
        Cell& wc = w.cellsMut()[k];
        wc.age = 1.0f;
        idx[i] = k;
        ++created;
    }
    for (const LayoutLink& l : st.links)
        if (idx[l.a] >= 0 && idx[l.b] >= 0) w.connect(idx[l.a], idx[l.b], l.extra, l.stiffness);

    // Weld the tissue: the order of divisions leaves some touching neighbours unlinked (e.g. the side split
    // second loses its links to the next segment). Cells that touch and are close in the link graph (2-8 steps,
    // i.e. the same tissue — up to the neighbouring rib of a wide flat body — not separate parts of the organism) get
    // the missing adhesin (see `reach` below for bands).
    {
        const int m = (int)st.cells.size();
        std::vector<std::vector<int>> adj(m);
        for (const LayoutLink& l : st.links) {
            adj[l.a].push_back(l.b);
            adj[l.b].push_back(l.a);
        }
        std::vector<int> depth(m, -1), seen;
        for (int i = 0; i < m; ++i) {
            if (idx[i] < 0 || adj[i].empty()) continue;
            seen.assign(1, i);
            depth[i] = 0;
            for (size_t q = 0; q < seen.size(); ++q) {
                int u = seen[q];
                if (depth[u] >= 8) continue;
                for (int v : adj[u])
                    if (depth[v] < 0) {
                        depth[v] = depth[u] + 1;
                        seen.push_back(v);
                    }
            }
            for (int j : seen) {
                if (j <= i || depth[j] < 2 || idx[j] < 0) continue;
                const Cell& A = w.cells()[idx[i]];
                const Cell& B = w.cells()[idx[j]];
                if (A.linkCount >= kMaxLinks || B.linkCount >= kMaxLinks) continue;
                // Cells of one mode in neighbouring segments (a muscle or cilia band) are welded across a wider gap:
                // relaxation leaves them ~1.3 apart where a pair of segments meets the next, and the unlinked band
                // then shears into steps when the body bends.
                const float reach = st.cells[i].mode == st.cells[j].mode ? 1.5f : 1.2f;
                const float dist = length(B.pos - A.pos), touch = A.radius + B.radius;
                if (dist > touch * reach) continue;
                float stiff = 0.5f * (g.modes[st.cells[i].mode].adhesinStiffness + g.modes[st.cells[j].mode].adhesinStiffness);
                w.connect(idx[i], idx[j], 0.0f, stiff);
            }
            for (int j : seen) depth[j] = -1;
        }
    }
    w.rebuildGrid();
    return created;
}

int spawnGrownOrganismFree(World& w, const Genome& g, const GenomeLayout& L, Rng& rng, float minR, float maxR) {
    if (L.stages.empty()) return 0;
    const LayoutStage& st = L.stages.back();
    Vec2 centroid{};
    for (const LayoutCell& c : st.cells) centroid += c.pos;
    centroid = centroid / (float)std::max<size_t>(1, st.cells.size());
    // Coarse occupancy grid of the cells already in the dish.
    const float cell = 40.0f;
    std::unordered_map<int64_t, std::vector<int>> occ;
    auto key = [&](Vec2 p) { return ((int64_t)std::floor(p.x / cell) << 32) ^ (int64_t)(uint32_t)(int32_t)std::floor(p.y / cell); };
    const auto& cells = w.cells();
    for (int i = 0; i < (int)cells.size(); ++i)
        if (cells[i].alive) occ[key(cells[i].pos)].push_back(i);
    const float R = w.settings.dishRadius;
    Vec2 bestPos{};
    float bestAngle = 0;
    int bestHits = 1 << 30;
    for (int attempt = 0; attempt < 80 && bestHits > 0; ++attempt) {
        Vec2 pos = fromAngle(rng.range(0, kTau)) * rng.range(minR, std::max(minR, maxR));
        float angle = rng.range(0, kTau);
        const float rot = angle - kPi * 0.5f;
        const float cr = std::cos(rot), sr = std::sin(rot);
        int hits = 0;
        for (size_t i = 0; i < st.cells.size() && hits < bestHits; i += 2) {  // every other cell is enough
            Vec2 d = st.cells[i].pos - centroid;
            Vec2 p = pos + Vec2{d.x * cr - d.y * sr, d.x * sr + d.y * cr};
            if (length(p) > R - 30.0f || w.insideBarrier(p, 25.0f)) {
                hits += 4;
                continue;
            }
            int64_t kx = (int64_t)std::floor(p.x / cell), ky = (int64_t)std::floor(p.y / cell);
            for (int64_t ox = -1; ox <= 1; ++ox)
                for (int64_t oy = -1; oy <= 1; ++oy) {
                    auto it = occ.find(((kx + ox) << 32) ^ (int64_t)(uint32_t)(int32_t)(ky + oy));
                    if (it == occ.end()) continue;
                    for (int j : it->second)
                        if (length2(cells[j].pos - p) < 45.0f * 45.0f) ++hits;
                }
        }
        if (hits < bestHits) {
            bestHits = hits;
            bestPos = pos;
            bestAngle = angle;
        }
    }
    return spawnGrownOrganism(w, g, L, bestPos, bestAngle);
}
