// iaus/PathSearch.cpp -- FindPath: the ONE route search, game and simulator (contract in iaus/EnvQuery.h; design in
// Docs/research/2026-11/iaus-sim-v4.md 2.2). Standard headers only. NOT CALLED BY THE DLL YET (sim v4 batch W).
//
// A* over triangles. A node is a triangle with the point he entered it at (the middle of the edge he came through), the
// heuristic is the straight line to the goal times the cheapest cost multiplier (so it never overestimates). The corners
// come from the funnel over the corridor's portals, each portal's ends pulled in by a share of his radius.
//
// Edges: Walk both ways. Drop one way, only for a walker whose abilities allow it and no deeper than his maxDrop. LedgeUp
// never. Portal only when the view says the portals are open. A closed door he can open costs its open time. One he cannot
// open costs PathTuning::blockPenalty: a path that still needs it is reported BLOCKED and ends in front of that door,
// which is what the engine's Movement Blocked looks like from outside.
#include "iaus/EnvQuery.h"

#include <algorithm>
#include <cmath>

namespace MFO::Iaus::Env {

    namespace {

        [[nodiscard]] float Dist(const Vec3& a, const Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
        [[nodiscard]] float Cross2(const Vec3& o, const Vec3& a, const Vec3& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }   // > 0: b is left of o -> a
        [[nodiscard]] bool  Same2(const Vec3& a, const Vec3& b) { return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f; }
        [[nodiscard]] float TriMult(const Tri& t, const PathTuning& pt) { float m = 1.0f; if (t.flags & kTriPreferred) m *= pt.preferredMult; if (t.flags & kTriWater) m *= pt.waterMult; return m; }

        constexpr std::uint8_t kOpen = 1, kClosed = 2, kPenal = 4;

        void Push(std::vector<Scratch::Node>& h, Scratch::Node n) {
            h.push_back(n); std::push_heap(h.begin(), h.end(), [](const Scratch::Node& a, const Scratch::Node& b) { return a.f > b.f || (a.f == b.f && a.tri > b.tri); });
        }
        Scratch::Node Pop(std::vector<Scratch::Node>& h) {
            std::pop_heap(h.begin(), h.end(), [](const Scratch::Node& a, const Scratch::Node& b) { return a.f > b.f || (a.f == b.f && a.tri > b.tri); });
            const Scratch::Node n = h.back(); h.pop_back(); return n;
        }

        // The closest point of a triangle to p in x, y (the landing of a drop).
        [[nodiscard]] Vec3 ClosestIn(const View& v, std::int32_t tri, const Vec3& p) {
            Vec3 on; const Vec3 c = Centroid(v, tri);
            const Tri& T = v.tris[static_cast<std::size_t>(tri)]; const Vec3* q[4] = { &v.verts[T.v[0]], &v.verts[T.v[1]], &v.verts[T.v[2]], &v.verts[T.v[0]] };
            const float s = Cross2(*q[0], *q[1], *q[2]) >= 0.0f ? 1.0f : -1.0f; bool in = true;
            for (int k = 0; k < 3; ++k) if (s * Cross2(*q[k], *q[k + 1], p) < 0.0f) { in = false; break; }
            if (in) { on = { p.x, p.y, HeightAt(v, tri, p.x, p.y) }; return on; }
            float bb = 1e30f; float bx = c.x, by = c.y;
            for (int k = 0; k < 3; ++k) { const float ex = q[k + 1]->x - q[k]->x, ey = q[k + 1]->y - q[k]->y, l2 = ex * ex + ey * ey;
                const float t = l2 > 1e-9f ? std::clamp(((p.x - q[k]->x) * ex + (p.y - q[k]->y) * ey) / l2, 0.0f, 1.0f) : 0.0f, ox = q[k]->x + t * ex, oy = q[k]->y + t * ey;
                const float d2 = (ox - p.x) * (ox - p.x) + (oy - p.y) * (oy - p.y); if (d2 < bb) { bb = d2; bx = ox; by = oy; } }
            const float gl = std::max(1.0f, std::hypot(c.x - bx, c.y - by)), inl = std::min(8.0f, 0.5f * gl);   // a step inside, so he lands ON the triangle
            on.x = bx + (c.x - bx) / gl * inl; on.y = by + (c.y - by) / gl * inl; on.z = HeightAt(v, tri, on.x, on.y);
            return on;
        }

        // The funnel over portals[first .. last) from a_from to a_to. Appends the corners after a_from, a_to included.
        void Funnel(const std::vector<Scratch::Portal>& P, std::size_t first, std::size_t last, const Vec3& a_from, const Vec3& a_to, std::int32_t toTri, Corner endKind, std::vector<PathPoint>& out) {
            const std::size_t n = last - first;   // portal i (1 .. n) is P[first + i - 1], portal n + 1 is the goal
            auto L = [&](std::size_t i) -> const Vec3& { return i == 0 ? a_from : i > n ? a_to : P[first + i - 1].l; };
            auto R = [&](std::size_t i) -> const Vec3& { return i == 0 ? a_from : i > n ? a_to : P[first + i - 1].r; };
            auto triOf = [&](std::size_t i) { return i == 0 || i > n ? toTri : P[first + i - 1].tri; };
            Vec3 apex = a_from, left = a_from, right = a_from; std::size_t ai = 0, li = 0, ri = 0;
            for (std::size_t i = 1, guard = 0; i <= n + 1 && guard < 8 * (n + 2); ++i, ++guard) {
                const Vec3& nl = L(i); const Vec3& nr = R(i);
                if (Cross2(apex, right, nr) >= 0.0f) {                       // the new right end narrows the funnel
                    if (Same2(apex, right) || Cross2(apex, left, nr) < 0.0f) { right = nr; ri = i; }
                    else { out.push_back({ left, triOf(li), Corner::Walk }); apex = left; ai = li; left = right = apex; li = ri = ai; i = ai; continue; }   // it crossed the left side: the left end is a corner
                }
                if (Cross2(apex, left, nl) <= 0.0f) {
                    if (Same2(apex, left) || Cross2(apex, right, nl) > 0.0f) { left = nl; li = i; }
                    else { out.push_back({ right, triOf(ri), Corner::Walk }); apex = right; ai = ri; left = right = apex; li = ri = ai; i = ai; continue; }
                }
            }
            out.push_back({ a_to, toTri, endKind });
        }

    }

    bool FindPath(const View& a_v, const PathQuery& a_q, Scratch& a_s, Path& a_out, const PathTuning& a_t) {
        a_out.Clear();
        const std::size_t n = a_v.tris.size();
        if (a_q.fromTri < 0 || a_q.toTri < 0 || static_cast<std::size_t>(a_q.fromTri) >= n || static_cast<std::size_t>(a_q.toTri) >= n) return false;
        if (a_q.fromTri == a_q.toTri) { a_out.pts.push_back({ a_q.to, a_q.toTri, Corner::Goal }); a_out.length = Dist(a_q.from, a_q.to); a_out.reachable = true; return true; }
        if (!a_v.weak.empty() && a_v.weak[static_cast<std::size_t>(a_q.fromTri)] != a_v.weak[static_cast<std::size_t>(a_q.toTri)]) return false;   // no link of any kind joins them
        if (a_s.mark.size() < n) { a_s.mark.assign(n, 0); a_s.g.resize(n); a_s.parent.resize(n); a_s.state.resize(n); a_s.entry.resize(n); a_s.gen = 0; }
        if (++a_s.gen == 0) { std::fill(a_s.mark.begin(), a_s.mark.end(), 0u); a_s.gen = 1; }
        const std::uint32_t gen = a_s.gen; const float hMult = std::min(1.0f, a_t.preferredMult);
        auto touch = [&](std::int32_t t) { const std::size_t i = static_cast<std::size_t>(t); if (a_s.mark[i] != gen) { a_s.mark[i] = gen; a_s.g[i] = 1e30f; a_s.parent[i] = -1; a_s.state[i] = 0; } };
        a_s.heap.clear();
        touch(a_q.fromTri); a_s.g[static_cast<std::size_t>(a_q.fromTri)] = 0.0f; a_s.entry[static_cast<std::size_t>(a_q.fromTri)] = a_q.from; a_s.state[static_cast<std::size_t>(a_q.fromTri)] = kOpen;
        Push(a_s.heap, { hMult * Dist(a_q.from, a_q.to), a_q.fromTri });
        std::int32_t best = a_q.fromTri; float bestH = Dist(a_q.from, a_q.to); bool found = false;
        while (!a_s.heap.empty()) {
            const Scratch::Node nd = Pop(a_s.heap); const std::size_t ci = static_cast<std::size_t>(nd.tri);
            if (a_s.state[ci] & kClosed) continue;
            a_s.state[ci] |= kClosed; ++a_out.expanded;
            if (nd.tri == a_q.toTri) { found = true; break; }
            if (a_t.maxExpand > 0 && a_out.expanded >= static_cast<std::uint32_t>(a_t.maxExpand)) break;
            const Tri& T = a_v.tris[ci]; const Vec3 P = a_s.entry[ci]; const float gT = a_s.g[ci], mT = TriMult(T, a_t);
            if (gT < a_t.blockPenalty) { const float h = Dist(P, a_q.to); if (h < bestH) { bestH = h; best = nd.tri; } }
            for (int k = 0; k < 3; ++k) {
                const std::int32_t nb = T.nb[k]; if (nb < 0 || static_cast<std::size_t>(nb) >= n) continue;
                const Edge kind = T.kind[k];
                const Vec3& a = a_v.verts[T.v[k]]; const Vec3& b = a_v.verts[T.v[(k + 1) % 3]];
                const Vec3 mid{ 0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z) };
                Vec3 in = mid; float cost = Dist(P, mid) * mT; bool penal = false;
                if (kind == Edge::Walk) {}
                else if (kind == Edge::Drop) { if (!a_q.who.drops) continue; in = ClosestIn(a_v, nb, mid); const float dz = mid.z - in.z; if (dz > a_q.who.maxDrop) continue;
                    cost += std::hypot(in.x - mid.x, in.y - mid.y) + a_t.dropBaseU + a_t.dropPerU * std::max(0.0f, dz); }
                else if (kind == Edge::Portal) { if (!a_v.portalsOpen) continue; }
                else continue;
                const Tri& N = a_v.tris[static_cast<std::size_t>(nb)];
                if (a_q.who.large && (N.flags & kTriNoLarge)) continue;
                if (N.door >= 0) { const DoorState ds = a_v.doors[static_cast<std::size_t>(N.door)].state;
                    if (ds == DoorState::Closed) { if (a_q.who.opensDoors) cost += a_t.doorOpenU; else { cost += a_t.blockPenalty; penal = true; } }
                    else if (ds == DoorState::Locked) { if (a_q.who.opensDoors && a_q.who.unlocks) cost += a_t.unlockU; else { cost += a_t.blockPenalty; penal = true; } } }
                touch(nb); const std::size_t ni = static_cast<std::size_t>(nb);
                if (a_s.state[ni] & kClosed) continue;
                const float g = gT + cost;
                if (g < a_s.g[ni]) { a_s.g[ni] = g; a_s.parent[ni] = nd.tri; a_s.entry[ni] = in; a_s.state[ni] = static_cast<std::uint8_t>(kOpen | (penal ? kPenal : 0));
                    Push(a_s.heap, { g + hMult * Dist(in, a_q.to), nb }); }
            }
        }
        // the corridor, goal first
        std::int32_t end = found ? a_q.toTri : best; Vec3 goal = found ? a_q.to : a_s.entry[static_cast<std::size_t>(best)]; bool reach = found;
        a_s.corridor.clear();
        for (std::int32_t t = end; t >= 0; t = a_s.parent[static_cast<std::size_t>(t)]) { a_s.corridor.push_back({ t, 0 }); if (t == a_q.fromTri) break; }
        std::reverse(a_s.corridor.begin(), a_s.corridor.end());
        if (a_s.corridor.empty() || a_s.corridor.front().tri != a_q.fromTri) return false;
        for (std::size_t i = 1; i < a_s.corridor.size(); ++i) if (a_s.state[static_cast<std::size_t>(a_s.corridor[i].tri)] & kPenal) {   // BLOCKED: the path ends in front of the first thing he cannot open
            a_out.blocked = true; a_out.blockDoor = a_v.tris[static_cast<std::size_t>(a_s.corridor[i].tri)].door; reach = false;
            const Vec3 at = a_s.entry[static_cast<std::size_t>(a_s.corridor[i].tri)]; const Vec3 c = Centroid(a_v, a_s.corridor[i - 1].tri);
            const float l = std::max(1.0f, std::hypot(c.x - at.x, c.y - at.y)), inl = std::min(4.0f, 0.5f * l);
            goal = { at.x + (c.x - at.x) / l * inl, at.y + (c.y - at.y) / l * inl, 0.0f }; goal.z = HeightAt(a_v, a_s.corridor[i - 1].tri, goal.x, goal.y);
            a_s.corridor.resize(i); break; }
        if (a_s.corridor.size() == 1) { a_out.pts.push_back({ goal, a_q.fromTri, Corner::Goal }); a_out.length = Dist(a_q.from, goal); a_out.reachable = reach; return true; }
        // portals, split into funnel runs at each drop
        const float inset = a_t.cornerInset * a_q.who.radius;
        a_s.portals.clear(); Vec3 runFrom = a_q.from; std::size_t runFirst = 0;
        for (std::size_t i = 0; i + 1 < a_s.corridor.size(); ++i) {
            const std::int32_t ti = a_s.corridor[i].tri, tn = a_s.corridor[i + 1].tri; const Tri& T = a_v.tris[static_cast<std::size_t>(ti)];
            int k = -1; for (int e = 0; e < 3; ++e) if (T.nb[e] == tn && (T.kind[e] == Edge::Walk || T.kind[e] == Edge::Drop || T.kind[e] == Edge::Portal) && (k < 0 || T.kind[e] == Edge::Walk)) k = e;
            if (k < 0) return false;
            const Vec3& a = a_v.verts[T.v[k]]; const Vec3& b = a_v.verts[T.v[(k + 1) % 3]];
            if (T.kind[k] == Edge::Drop) {
                const Vec3 mid{ 0.5f * (a.x + b.x), 0.5f * (a.y + b.y), 0.5f * (a.z + b.z) };
                Funnel(a_s.portals, runFirst, a_s.portals.size(), runFrom, mid, ti, Corner::DropStart, a_out.pts);
                const Vec3 land = ClosestIn(a_v, tn, mid); a_out.pts.push_back({ land, tn, Corner::DropLand }); ++a_out.drops;
                runFrom = land; runFirst = a_s.portals.size(); continue;
            }
            const bool ccw = Cross2(a_v.verts[T.v[0]], a_v.verts[T.v[1]], a_v.verts[T.v[2]]) >= 0.0f;
            Vec3 l = ccw ? b : a, r = ccw ? a : b;   // facing out of a counter-clockwise triangle across a -> b, b is on the left
            const float w = Dist(l, r), pull = std::min(inset, 0.45f * w);
            if (w > 1e-3f) { const Vec3 d{ (r.x - l.x) / w, (r.y - l.y) / w, (r.z - l.z) / w }; l = { l.x + d.x * pull, l.y + d.y * pull, l.z + d.z * pull }; r = { r.x - d.x * pull, r.y - d.y * pull, r.z - d.z * pull }; }
            a_s.portals.push_back({ l, r, tn });
        }
        Funnel(a_s.portals, runFirst, a_s.portals.size(), runFrom, goal, a_s.corridor.back().tri, Corner::Goal, a_out.pts);
        // The corridor A* chose need not hold the straight line (its nodes are edge middles), so the funnel can leave a corner the
        // open floor does not need. Drop every corner the straight walk from the corner before it to the one after it clears.
        { Vec3 from = a_q.from; std::int32_t fromTri = a_q.fromTri; std::size_t w = 0;
          for (std::size_t i = 0; i < a_out.pts.size(); ++i) {
              const PathPoint& c = a_out.pts[i];
              const bool last = i + 1 >= a_out.pts.size(), fixed = c.kind != Corner::Walk || last || a_out.pts[i + 1].kind == Corner::DropLand;
              if (!fixed) { const PathPoint& nx = a_out.pts[i + 1]; const Walk wk = WalkLine(a_v, fromTri, from, nx.p, a_q.who);
                  if (wk.reached && std::fabs(wk.end.z - nx.p.z) < 16.0f) continue; }   // skipped: the next corner is reached without it
              if (w && std::fabs(a_out.pts[w - 1].p.x - c.p.x) < 0.5f && std::fabs(a_out.pts[w - 1].p.y - c.p.y) < 0.5f && std::fabs(a_out.pts[w - 1].p.z - c.p.z) < 8.0f && a_out.pts[w - 1].kind == Corner::Walk) --w;   // the same point twice
              a_out.pts[w++] = c;
              if (c.kind == Corner::DropLand) { from = c.p; fromTri = c.tri; }
              else { const Walk wk = WalkLine(a_v, fromTri, from, c.p, a_q.who); if (wk.reached) { from = c.p; fromTri = wk.tri; } else { from = c.p; fromTri = c.tri; } }
          }
          a_out.pts.resize(w); }
        Vec3 prev = a_q.from;
        for (const PathPoint& p : a_out.pts) { a_out.length += Dist(prev, p.p); prev = p.p; }
        // height gained and lost, along the corridor (a ridge between two corners still counts)
        { float z = a_q.from.z; for (std::size_t i = 1; i < a_s.corridor.size(); ++i) { const Tri& T = a_v.tris[static_cast<std::size_t>(a_s.corridor[i - 1].tri)]; bool drop = false;
              for (int e = 0; e < 3; ++e) if (T.nb[e] == a_s.corridor[i].tri && T.kind[e] == Edge::Drop) drop = true;
              const float nz = a_s.entry[static_cast<std::size_t>(a_s.corridor[i].tri)].z; if (!drop) { if (nz > z) a_out.climb += nz - z; else a_out.fall += z - nz; } z = nz; }
          if (goal.z > z) a_out.climb += goal.z - z; else a_out.fall += z - goal.z; }
        a_out.reachable = reach;
        return true;
    }

}
