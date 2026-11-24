// iaus/EnvQuery.cpp -- point location, the straight walk, geometry rays and the derived tables (contract in iaus/EnvQuery.h).
// Standard headers only: tools/iaus_sim/run.sh and tools/iaus_harness build this file with the host compiler.
// NOT CALLED BY THE DLL YET (sim v4 batch W).
#include "iaus/EnvQuery.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace MFO::Iaus::Env {

    namespace {

        [[nodiscard]] float Cross2(float ax, float ay, float bx, float by) { return ax * by - ay * bx; }
        [[nodiscard]] float Area2(const Vec3& a, const Vec3& b, const Vec3& c) { return Cross2(b.x - a.x, b.y - a.y, c.x - a.x, c.y - a.y); }

        // Inside in x, y, with a tolerance of a twentieth of a unit past each edge. Either winding.
        [[nodiscard]] bool InTri2(const Vec3& a, const Vec3& b, const Vec3& c, float x, float y) {
            const float s = Area2(a, b, c) >= 0.0f ? 1.0f : -1.0f;
            const Vec3* v[4] = { &a, &b, &c, &a };
            for (int k = 0; k < 3; ++k) {
                const float ex = v[k + 1]->x - v[k]->x, ey = v[k + 1]->y - v[k]->y;
                if (s * Cross2(ex, ey, x - v[k]->x, y - v[k]->y) < -0.05f * std::sqrt(ex * ex + ey * ey)) return false;
            }
            return true;
        }

        [[nodiscard]] float PlaneZ(const Vec3& a, const Vec3& b, const Vec3& c, float x, float y) {
            const float d = Area2(a, b, c);
            if (std::fabs(d) < 1e-6f) return (a.z + b.z + c.z) / 3.0f;
            const float wb = Cross2(x - a.x, y - a.y, c.x - a.x, c.y - a.y) / d, wc = Cross2(b.x - a.x, b.y - a.y, x - a.x, y - a.y) / d;
            return a.z + wb * (b.z - a.z) + wc * (c.z - a.z);
        }

        // Closest point of segment a-b to (x, y), in x, y.
        void ClosestOnSeg(const Vec3& a, const Vec3& b, float x, float y, float& ox, float& oy) {
            const float ex = b.x - a.x, ey = b.y - a.y, l2 = ex * ex + ey * ey;
            const float t = l2 > 1e-9f ? std::clamp(((x - a.x) * ex + (y - a.y) * ey) / l2, 0.0f, 1.0f) : 0.0f;
            ox = a.x + t * ex; oy = a.y + t * ey;
        }

        struct CellRange { int x0, y0, x1, y1; };
        [[nodiscard]] CellRange Cells(const Grid& g, float minX, float minY, float maxX, float maxY) {
            CellRange r;
            r.x0 = std::clamp(static_cast<int>(std::floor((minX - g.x0) / g.cell)), 0, g.nx - 1); r.x1 = std::clamp(static_cast<int>(std::floor((maxX - g.x0) / g.cell)), 0, g.nx - 1);
            r.y0 = std::clamp(static_cast<int>(std::floor((minY - g.y0) / g.cell)), 0, g.ny - 1); r.y1 = std::clamp(static_cast<int>(std::floor((maxY - g.y0) / g.cell)), 0, g.ny - 1);
            return r;
        }

        // Rows per cell from a box per item, as start / items (two passes).
        template <class BoxOf>
        void Rows(const Grid& g, std::size_t n, BoxOf boxOf, std::vector<std::uint32_t>& start, std::vector<std::uint32_t>& items) {
            const std::size_t cells = static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny);
            start.assign(cells + 1, 0);
            for (std::size_t i = 0; i < n; ++i) { const CellRange r = boxOf(i); for (int y = r.y0; y <= r.y1; ++y) for (int x = r.x0; x <= r.x1; ++x) ++start[static_cast<std::size_t>(y) * g.nx + x + 1]; }
            for (std::size_t c = 0; c < cells; ++c) start[c + 1] += start[c];
            items.assign(start[cells], 0);
            std::vector<std::uint32_t> at(start.begin(), start.end() - 1);
            for (std::size_t i = 0; i < n; ++i) { const CellRange r = boxOf(i); for (int y = r.y0; y <= r.y1; ++y) for (int x = r.x0; x <= r.x1; ++x) items[at[static_cast<std::size_t>(y) * g.nx + x]++] = static_cast<std::uint32_t>(i); }
        }

        // Segment a-b against triangle p0 p1 p2 in 3D (Moller-Trumbore). t strictly inside (0, 1).
        [[nodiscard]] bool SegTri(const Vec3& a, const Vec3& b, const Vec3& p0, const Vec3& p1, const Vec3& p2, float& tOut) {
            const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
            const float e1x = p1.x - p0.x, e1y = p1.y - p0.y, e1z = p1.z - p0.z, e2x = p2.x - p0.x, e2y = p2.y - p0.y, e2z = p2.z - p0.z;
            const float px = dy * e2z - dz * e2y, py = dz * e2x - dx * e2z, pz = dx * e2y - dy * e2x;
            const float det = e1x * px + e1y * py + e1z * pz;
            if (std::fabs(det) < 1e-7f) return false;
            const float inv = 1.0f / det, tx = a.x - p0.x, ty = a.y - p0.y, tz = a.z - p0.z;
            const float u = (tx * px + ty * py + tz * pz) * inv; if (u < 0.0f || u > 1.0f) return false;
            const float qx = ty * e1z - tz * e1y, qy = tz * e1x - tx * e1z, qz = tx * e1y - ty * e1x;
            const float v = (dx * qx + dy * qy + dz * qz) * inv; if (v < 0.0f || u + v > 1.0f) return false;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
            if (t <= 1e-4f || t >= 1.0f - 1e-4f) return false;
            tOut = t; return true;
        }

        // Every cell the x, y segment passes, in order. fn(cell) returns true to stop.
        template <class Fn>
        void WalkCells(const Grid& g, float ax, float ay, float bx, float by, Fn fn) {
            if (g.nx <= 0 || g.ny <= 0) return;
            int cx = std::clamp(static_cast<int>(std::floor((ax - g.x0) / g.cell)), 0, g.nx - 1), cy = std::clamp(static_cast<int>(std::floor((ay - g.y0) / g.cell)), 0, g.ny - 1);
            const int ex = std::clamp(static_cast<int>(std::floor((bx - g.x0) / g.cell)), 0, g.nx - 1), ey = std::clamp(static_cast<int>(std::floor((by - g.y0) / g.cell)), 0, g.ny - 1);
            const float dx = bx - ax, dy = by - ay;
            const int sx = dx > 0.0f ? 1 : -1, sy = dy > 0.0f ? 1 : -1;
            const float invx = std::fabs(dx) > 1e-9f ? 1.0f / std::fabs(dx) : 1e30f, invy = std::fabs(dy) > 1e-9f ? 1.0f / std::fabs(dy) : 1e30f;
            float tx = (dx > 0.0f ? (g.x0 + static_cast<float>(cx + 1) * g.cell - ax) : (ax - (g.x0 + static_cast<float>(cx) * g.cell))) * invx;
            float ty = (dy > 0.0f ? (g.y0 + static_cast<float>(cy + 1) * g.cell - ay) : (ay - (g.y0 + static_cast<float>(cy) * g.cell))) * invy;
            const float stepx = g.cell * invx, stepy = g.cell * invy;
            for (int guard = g.nx + g.ny + 4; guard > 0; --guard) {
                if (fn(cy * g.nx + cx)) return;
                if (cx == ex && cy == ey) return;
                if (tx < ty) { if (cx == ex) { if (cy == ey) return; cy += sy; ty += stepy; } else { cx += sx; tx += stepx; } }
                else         { if (cy == ey) { if (cx == ex) return; cx += sx; tx += stepx; } else { cy += sy; ty += stepy; } }
                if (cx < 0 || cy < 0 || cx >= g.nx || cy >= g.ny) return;
            }
        }

        // The floors at one x, y other than a_skip, sorted into what the wall builder asks about an edge at height ez.
        struct Floors { bool same = false, lower = false, higher = false; float low = -1e30f, high = 1e30f; };
        void FloorsAt(const View& v, float x, float y, float ez, std::int32_t skip, const SightTuning& t, Floors& f) {
            const Grid& g = *v.grid; const int c = g.CellOf(x, y); if (c < 0) return;
            for (std::uint32_t i = g.triStart[static_cast<std::size_t>(c)]; i < g.triStart[static_cast<std::size_t>(c) + 1]; ++i) {
                const std::int32_t ti = static_cast<std::int32_t>(g.triItems[i]); if (ti == skip) continue;
                const Tri& tr = v.tris[static_cast<std::size_t>(ti)]; const Vec3& a = v.verts[tr.v[0]]; const Vec3& b = v.verts[tr.v[1]]; const Vec3& cc = v.verts[tr.v[2]];
                if (!InTri2(a, b, cc, x, y)) continue;
                const float h = PlaneZ(a, b, cc, x, y);
                if (std::fabs(h - ez) <= t.stepH) f.same = true;
                else if (h < ez) { f.lower = true; f.low = std::max(f.low, h); }
                else if (h - ez <= t.cliffMaxH) { f.higher = true; f.high = std::min(f.high, h); }
            }
        }

    }

    int Grid::CellOf(float a_x, float a_y) const {
        if (nx <= 0 || ny <= 0) return -1;
        const int cx = static_cast<int>(std::floor((a_x - x0) / cell)), cy = static_cast<int>(std::floor((a_y - y0) / cell));
        if (cx < 0 || cy < 0 || cx >= nx || cy >= ny) return -1;
        return cy * nx + cx;
    }

    void BuildGrid(Grid& a_g, std::span<const Vec3> a_verts, std::span<const Tri> a_tris) {
        a_g = Grid{};
        if (a_verts.empty() || a_tris.empty()) return;
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (const Vec3& p : a_verts) { minX = std::min(minX, p.x); minY = std::min(minY, p.y); maxX = std::max(maxX, p.x); maxY = std::max(maxY, p.y); }
        const float ext = std::max(maxX - minX, maxY - minY);
        a_g.cell = std::clamp(ext / 96.0f, 96.0f, 1024.0f); a_g.x0 = minX - 1.0f; a_g.y0 = minY - 1.0f;
        a_g.nx = std::max(1, static_cast<int>(std::ceil((maxX - a_g.x0 + 1.0f) / a_g.cell))); a_g.ny = std::max(1, static_cast<int>(std::ceil((maxY - a_g.y0 + 1.0f) / a_g.cell)));
        const Grid& g = a_g;
        Rows(g, a_tris.size(), [&](std::size_t i) { const Tri& t = a_tris[i]; const Vec3& a = a_verts[t.v[0]]; const Vec3& b = a_verts[t.v[1]]; const Vec3& c = a_verts[t.v[2]];
            return Cells(g, std::min({ a.x, b.x, c.x }) - 0.1f, std::min({ a.y, b.y, c.y }) - 0.1f, std::max({ a.x, b.x, c.x }) + 0.1f, std::max({ a.y, b.y, c.y }) + 0.1f); }, a_g.triStart, a_g.triItems);
        a_g.wallStart.assign(static_cast<std::size_t>(a_g.nx) * static_cast<std::size_t>(a_g.ny) + 1, 0); a_g.wallItems.clear();
    }

    void IndexWalls(Grid& a_g, std::span<const Wall> a_walls) {
        if (a_g.nx <= 0) return;
        const Grid& g = a_g;
        Rows(g, a_walls.size(), [&](std::size_t i) { const Wall& w = a_walls[i]; return Cells(g, std::min(w.ax, w.bx) - 0.1f, std::min(w.ay, w.by) - 0.1f, std::max(w.ax, w.bx) + 0.1f, std::max(w.ay, w.by) + 0.1f); }, a_g.wallStart, a_g.wallItems);
    }

    float HeightAt(const View& a_v, std::int32_t a_tri, float a_x, float a_y) {
        const Tri& t = a_v.tris[static_cast<std::size_t>(a_tri)];
        return PlaneZ(a_v.verts[t.v[0]], a_v.verts[t.v[1]], a_v.verts[t.v[2]], a_x, a_y);
    }

    Vec3 Centroid(const View& a_v, std::int32_t a_tri) {
        const Tri& t = a_v.tris[static_cast<std::size_t>(a_tri)]; const Vec3& a = a_v.verts[t.v[0]]; const Vec3& b = a_v.verts[t.v[1]]; const Vec3& c = a_v.verts[t.v[2]];
        return { (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
    }

    float OutsideBy(const View& a_v, std::int32_t a_tri, float a_x, float a_y) {
        const Tri& t = a_v.tris[static_cast<std::size_t>(a_tri)]; const Vec3& a = a_v.verts[t.v[0]]; const Vec3& b = a_v.verts[t.v[1]]; const Vec3& c = a_v.verts[t.v[2]];
        if (InTri2(a, b, c, a_x, a_y)) return 0.0f;
        const Vec3* q[4] = { &a, &b, &c, &a }; float bb = 1e30f;
        for (int k = 0; k < 3; ++k) { float ox, oy; ClosestOnSeg(*q[k], *q[k + 1], a_x, a_y, ox, oy); bb = std::min(bb, (ox - a_x) * (ox - a_x) + (oy - a_y) * (oy - a_y)); }
        return std::sqrt(bb);
    }

    std::int32_t TriAt(const View& a_v, Vec3 a_p) {
        if (!a_v.grid) return -1;
        const Grid& g = *a_v.grid; const int c = g.CellOf(a_p.x, a_p.y); if (c < 0) return -1;
        std::int32_t best = -1; float bd = 1e30f;
        for (std::uint32_t i = g.triStart[static_cast<std::size_t>(c)]; i < g.triStart[static_cast<std::size_t>(c) + 1]; ++i) {
            const Tri& t = a_v.tris[g.triItems[i]]; const Vec3& a = a_v.verts[t.v[0]]; const Vec3& b = a_v.verts[t.v[1]]; const Vec3& cc = a_v.verts[t.v[2]];
            if (!InTri2(a, b, cc, a_p.x, a_p.y)) continue;
            const float d = std::fabs(PlaneZ(a, b, cc, a_p.x, a_p.y) - a_p.z);
            if (d < bd) { bd = d; best = static_cast<std::int32_t>(g.triItems[i]); }
        }
        return best;
    }

    std::int32_t NearestTri(const View& a_v, Vec3 a_p, float a_maxDist, Vec3& a_on) {
        if (!a_v.grid || a_v.grid->nx <= 0) return -1;
        const Grid& g = *a_v.grid;
        const int cx = std::clamp(static_cast<int>(std::floor((a_p.x - g.x0) / g.cell)), 0, g.nx - 1), cy = std::clamp(static_cast<int>(std::floor((a_p.y - g.y0) / g.cell)), 0, g.ny - 1);
        const int rings = static_cast<int>(std::ceil(a_maxDist / g.cell)) + 1;
        std::int32_t best = -1; float bd = a_maxDist;
        for (int r = 0; r <= rings; ++r) {
            if (best >= 0 && static_cast<float>(r - 1) * g.cell > bd) break;
            for (int y = cy - r; y <= cy + r; ++y) { if (y < 0 || y >= g.ny) continue;
                for (int x = cx - r; x <= cx + r; ++x) { if (x < 0 || x >= g.nx) continue; if (std::max(std::abs(x - cx), std::abs(y - cy)) != r) continue;
                    const std::size_t c = static_cast<std::size_t>(y) * g.nx + x;
                    for (std::uint32_t i = g.triStart[c]; i < g.triStart[c + 1]; ++i) {
                        const Tri& t = a_v.tris[g.triItems[i]]; const Vec3& a = a_v.verts[t.v[0]]; const Vec3& b = a_v.verts[t.v[1]]; const Vec3& cc = a_v.verts[t.v[2]];
                        float px = a_p.x, py = a_p.y;
                        if (!InTri2(a, b, cc, px, py)) { float bx = 0, by = 0, bb = 1e30f; const Vec3* v[4] = { &a, &b, &cc, &a };
                            for (int k = 0; k < 3; ++k) { float ox, oy; ClosestOnSeg(*v[k], *v[k + 1], a_p.x, a_p.y, ox, oy); const float d2 = (ox - a_p.x) * (ox - a_p.x) + (oy - a_p.y) * (oy - a_p.y); if (d2 < bb) { bb = d2; bx = ox; by = oy; } }
                            px = bx; py = by; }
                        const float pz = PlaneZ(a, b, cc, px, py), dz = 2.0f * (pz - a_p.z);
                        const float d = std::sqrt((px - a_p.x) * (px - a_p.x) + (py - a_p.y) * (py - a_p.y) + dz * dz);
                        if (d < bd || (d == bd && best >= 0 && static_cast<std::int32_t>(g.triItems[i]) < best)) { bd = d; best = static_cast<std::int32_t>(g.triItems[i]);
                            const float gx = (a.x + b.x + cc.x) / 3.0f, gy = (a.y + b.y + cc.y) / 3.0f, gl = std::max(1.0f, std::hypot(gx - px, gy - py)), in = std::min(0.5f, gl);   // half a unit inside, so the point is IN the triangle
                            a_on.x = px + (gx - px) / gl * in; a_on.y = py + (gy - py) / gl * in; a_on.z = PlaneZ(a, b, cc, a_on.x, a_on.y); }
                    }
                } }
        }
        return best;
    }

    namespace {
        // Two meshes joined by a link need not meet exactly (the extractor's validator: 2.5 percent of links lie more than 32 units from their
        // target triangle). A walk that crossed such a seam can end a little outside the triangle it is in. Put it on that triangle.
        void OntoTri(const View& v, Walk& w) {
            const Tri& T = v.tris[static_cast<std::size_t>(w.tri)]; const Vec3& a = v.verts[T.v[0]]; const Vec3& b = v.verts[T.v[1]]; const Vec3& c = v.verts[T.v[2]];
            if (InTri2(a, b, c, w.end.x, w.end.y)) return;
            const Vec3* q[4] = { &a, &b, &c, &a }; float bb = 1e30f, bx = w.end.x, by = w.end.y;
            for (int k = 0; k < 3; ++k) { float ox, oy; ClosestOnSeg(*q[k], *q[k + 1], w.end.x, w.end.y, ox, oy); const float d2 = (ox - w.end.x) * (ox - w.end.x) + (oy - w.end.y) * (oy - w.end.y); if (d2 < bb) { bb = d2; bx = ox; by = oy; } }
            const float gx = (a.x + b.x + c.x) / 3.0f, gy = (a.y + b.y + c.y) / 3.0f, gl = std::max(1.0f, std::hypot(gx - bx, gy - by)), in = std::min(0.5f, gl);
            w.end.x = bx + (gx - bx) / gl * in; w.end.y = by + (gy - by) / gl * in; w.end.z = PlaneZ(a, b, c, w.end.x, w.end.y);
        }
    }

    Walk WalkLine(const View& a_v, std::int32_t a_tri, Vec3 a_from, Vec3 a_to, const Walker& a_who) {
        Walk w; w.tri = a_tri; w.end = a_from;
        if (a_tri < 0) return w;
        const float dx = a_to.x - a_from.x, dy = a_to.y - a_from.y, len = std::hypot(dx, dy);
        if (len < 1e-4f) { w.reached = true; return w; }
        std::int32_t cur = a_tri; float tcur = 0.0f; const float back = std::min(0.5f / len, 0.5f); bool seam = false;
        for (int iter = 0; iter < 512; ++iter) {
            const Tri& T = a_v.tris[static_cast<std::size_t>(cur)];
            const Vec3* p[4] = { &a_v.verts[T.v[0]], &a_v.verts[T.v[1]], &a_v.verts[T.v[2]], &a_v.verts[T.v[0]] };
            const float s = Area2(*p[0], *p[1], *p[2]) >= 0.0f ? 1.0f : -1.0f;
            float bt = 1e30f; int bk = -1;
            for (int k = 0; k < 3; ++k) {
                const float nx = s * (p[k + 1]->y - p[k]->y), ny = -s * (p[k + 1]->x - p[k]->x);   // outward
                const float den = dx * nx + dy * ny; if (den <= 1e-9f) continue;
                const float t = ((p[k]->x - a_from.x) * nx + (p[k]->y - a_from.y) * ny) / den;
                if (t < bt) { bt = t; bk = k; }
            }
            if (bk < 0 || bt >= 1.0f) { w.reached = true; w.tri = cur; w.end = { a_to.x, a_to.y, HeightAt(a_v, cur, a_to.x, a_to.y) }; if (seam) OntoTri(a_v, w); return w; }
            bt = std::max(bt, tcur);
            const Edge kind = T.kind[bk]; const std::int32_t n = T.nb[bk];
            bool cross = kind == Edge::Walk && n >= 0;
            if (cross) { const Tri& N = a_v.tris[static_cast<std::size_t>(n)];
                if (a_who.large && (N.flags & kTriNoLarge)) cross = false;
                else if (N.door >= 0 && a_v.doors[static_cast<std::size_t>(N.door)].state != DoorState::Open) { cross = false; w.door = N.door; } }
            if (!cross) { const float te = std::max(tcur, bt - back); w.tri = cur; w.hit = kind; w.hitTri = cur; w.hitEdge = static_cast<std::int8_t>(bk);
                w.end.x = a_from.x + dx * te; w.end.y = a_from.y + dy * te; w.end.z = HeightAt(a_v, cur, w.end.x, w.end.y); if (seam) OntoTri(a_v, w); return w; }
            cur = n; tcur = bt; seam = true;
        }
        w.tri = cur; w.end.x = a_from.x + dx * tcur; w.end.y = a_from.y + dy * tcur; w.end.z = HeightAt(a_v, cur, w.end.x, w.end.y); OntoTri(a_v, w);
        return w;
    }

    Sight CastRay(const View& a_v, Vec3 a_from, Vec3 a_to, Scratch& a_s, const SightTuning& a_t) {
        Sight out;
        if (!a_v.grid || a_v.grid->nx <= 0) return out;
        const Grid& g = *a_v.grid;
        const float dx = a_to.x - a_from.x, dy = a_to.y - a_from.y, dz = a_to.z - a_from.z, len2d = std::hypot(dx, dy);
        auto block = [&](Blocker by, float t) { if (out.clear || t < out.t) { out.clear = false; out.by = by; out.t = t; } };
        // closed doors: the door stands on its triangle, wallH high
        for (const Door& d : a_v.doors) { if (d.state == DoorState::Open || d.tri < 0) continue;
            if (d.leaf) { const float ex = d.bx - d.ax, ey = d.by - d.ay, den = Cross2(dx, dy, ex, ey); if (std::fabs(den) < 1e-9f) continue;
                const float t = Cross2(d.ax - a_from.x, d.ay - a_from.y, ex, ey) / den, u = Cross2(d.ax - a_from.x, d.ay - a_from.y, dx, dy) / den;
                if (t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f) { const float rz = a_from.z + t * dz; if (rz >= d.z0 - a_t.wallBelow && rz <= d.z1) block(Blocker::Door, t); }
                continue; }
            const Tri& T = a_v.tris[static_cast<std::size_t>(d.tri)]; const Vec3* p[4] = { &a_v.verts[T.v[0]], &a_v.verts[T.v[1]], &a_v.verts[T.v[2]], &a_v.verts[T.v[0]] };
            for (int k = 0; k < 3; ++k) { const float ex = p[k + 1]->x - p[k]->x, ey = p[k + 1]->y - p[k]->y, den = Cross2(dx, dy, ex, ey); if (std::fabs(den) < 1e-9f) continue;
                const float t = Cross2(p[k]->x - a_from.x, p[k]->y - a_from.y, ex, ey) / den, u = Cross2(p[k]->x - a_from.x, p[k]->y - a_from.y, dx, dy) / den;
                if (t <= 0.0f || t >= 1.0f || u < 0.0f || u > 1.0f) continue;
                const float ez = p[k]->z + u * (p[k + 1]->z - p[k]->z), rz = a_from.z + t * dz;
                if (rz >= ez - a_t.wallBelow && rz <= ez + a_t.wallH) { block(Blocker::Door, t); break; } } }
        if (!out.clear) return out;
        if (a_t.walls && len2d > 1e-3f && !a_v.walls.empty()) {
            a_s.cross.clear(); bool face = false; float faceT = 1.0f;
            WalkCells(g, a_from.x, a_from.y, a_to.x, a_to.y, [&](int c) {
                for (std::uint32_t i = g.wallStart[static_cast<std::size_t>(c)]; i < g.wallStart[static_cast<std::size_t>(c) + 1]; ++i) {
                    const Wall& w = a_v.walls[g.wallItems[i]]; const float ex = w.bx - w.ax, ey = w.by - w.ay, den = Cross2(dx, dy, ex, ey); if (std::fabs(den) < 1e-9f) continue;
                    const float t = Cross2(w.ax - a_from.x, w.ay - a_from.y, ex, ey) / den, u = Cross2(w.ax - a_from.x, w.ay - a_from.y, dx, dy) / den;
                    if (t <= 0.0f || t >= 1.0f || u < 0.0f || u > 1.0f) continue;
                    const float ez = w.az + u * (w.bz - w.az), rz = a_from.z + t * dz;
                    if (rz < ez + w.lo || rz > ez + w.hi) continue;
                    if (w.kind == WallKind::Face) { face = true; faceT = std::min(faceT, t); continue; }
                    a_s.cross.push_back({ t, Cross2(ex, ey, dx, dy) < 0.0f });   // the interior is on the left of a -> b: he leaves it when he heads to its right
                }
                return false; });
            if (face) block(Blocker::Face, faceT);
            if (!a_s.cross.empty()) {
                std::sort(a_s.cross.begin(), a_s.cross.end(), [](const Scratch::Cross& a, const Scratch::Cross& b) { return a.t < b.t || (a.t == b.t && a.leaving && !b.leaving); });
                const float gap = (a_v.interior ? a_t.gapU : a_t.gapExteriorU) / len2d;
                for (std::size_t i = 0; i < a_s.cross.size(); ++i) { if (!a_s.cross[i].leaving) continue;
                    bool back = false; for (std::size_t j = i + 1; j < a_s.cross.size() && a_s.cross[j].t - a_s.cross[i].t < gap; ++j) if (!a_s.cross[j].leaving) { back = true; break; }
                    if (!back) { block(Blocker::Wall, a_s.cross[i].t); break; } }
            }
        }
        if (!out.clear) return out;
        if (a_t.terrain) {
            const float zlo = std::min(a_from.z, a_to.z) - 1.0f, zhi = std::max(a_from.z, a_to.z) + 1.0f;
            WalkCells(g, a_from.x, a_from.y, a_to.x, a_to.y, [&](int c) {
                for (std::uint32_t i = g.triStart[static_cast<std::size_t>(c)]; i < g.triStart[static_cast<std::size_t>(c) + 1]; ++i) {
                    const Tri& T = a_v.tris[g.triItems[i]]; const Vec3& a = a_v.verts[T.v[0]]; const Vec3& b = a_v.verts[T.v[1]]; const Vec3& cc = a_v.verts[T.v[2]];
                    if ((a.z < zlo && b.z < zlo && cc.z < zlo) || (a.z > zhi && b.z > zhi && cc.z > zhi)) continue;
                    float t; if (SegTri(a_from, a_to, a, b, cc, t)) { block(Blocker::Terrain, t); return true; }
                }
                return false; });
        }
        return out;
    }

    void BuildWalls(std::vector<Wall>& a_out, const View& a_v, const SightTuning& a_t) {
        a_out.clear();
        if (!a_v.grid) return;
        for (std::size_t ti = 0; ti < a_v.tris.size(); ++ti) {
            const Tri& T = a_v.tris[ti]; const Vec3* p[4] = { &a_v.verts[T.v[0]], &a_v.verts[T.v[1]], &a_v.verts[T.v[2]], &a_v.verts[T.v[0]] };
            const float s = Area2(*p[0], *p[1], *p[2]) >= 0.0f ? 1.0f : -1.0f;
            for (int k = 0; k < 3; ++k) {
                if (T.kind[k] == Edge::Walk || T.kind[k] == Edge::Border) continue;
                if (T.kind[k] == Edge::Portal && a_v.portalsOpen) continue;
                const Vec3& a = *p[k]; const Vec3& b = *p[k + 1];
                const float ex = b.x - a.x, ey = b.y - a.y, el = std::hypot(ex, ey); if (el < 1e-3f) continue;
                const float nx = s * ey / el, ny = -s * ex / el, mx = 0.5f * (a.x + b.x), my = 0.5f * (a.y + b.y), ez = 0.5f * (a.z + b.z);
                Floors f; for (const float d : { 0.25f, 0.5f, 1.0f }) FloorsAt(a_v, mx + nx * d * a_t.probeU, my + ny * d * a_t.probeU, ez, static_cast<std::int32_t>(ti), a_t, f);
                Wall w; if (s > 0.0f) { w.ax = a.x; w.ay = a.y; w.az = a.z; w.bx = b.x; w.by = b.y; w.bz = b.z; } else { w.ax = b.x; w.ay = b.y; w.az = b.z; w.bx = a.x; w.by = a.y; w.bz = a.z; }
                if (T.kind[k] == Edge::Drop && T.nb[k] >= 0 && !f.lower) { const float lz = Centroid(a_v, T.nb[k]).z; if (lz < ez - a_t.stepH) { f.lower = true; f.low = lz; } }
                if (f.same) { w.kind = WallKind::Solid; w.lo = -a_t.wallBelow; w.hi = a_t.wallH; }
                else if (f.higher) { w.kind = WallKind::Face; w.lo = -a_t.wallBelow; w.hi = std::max(a_t.wallH, f.high - ez); }   // the foot of a cliff, or a wall with a floor behind and above it
                else if (f.lower) {   // a ledge: he looks down over it. Its face blocks only what passes under the edge, and a balcony (floor under his own) has no face
                    Floors under; FloorsAt(a_v, mx - nx * 8.0f, my - ny * 8.0f, ez, static_cast<std::int32_t>(ti), a_t, under);
                    if (under.lower) continue;
                    w.kind = WallKind::Face; w.lo = f.low - ez; w.hi = -1.0f;
                }
                else { w.kind = WallKind::Solid; w.lo = -a_t.wallBelow; w.hi = a_t.wallH; }
                a_out.push_back(w);
            }
        }
    }

    void LabelComponents(std::span<const Tri> a_tris, std::vector<std::uint32_t>& a_walk, std::vector<std::uint32_t>& a_weak) {
        const std::size_t n = a_tris.size();
        auto label = [&](bool weak, std::vector<std::uint32_t>& out) {
            std::vector<std::uint32_t> par(n); std::iota(par.begin(), par.end(), 0u);
            auto find = [&](std::uint32_t x) { while (par[x] != x) { par[x] = par[par[x]]; x = par[x]; } return x; };
            for (std::size_t i = 0; i < n; ++i) for (int k = 0; k < 3; ++k) { const std::int32_t nb = a_tris[i].nb[k]; if (nb < 0 || static_cast<std::size_t>(nb) >= n) continue;
                if (a_tris[i].kind[k] != Edge::Walk && !(weak && (a_tris[i].kind[k] == Edge::Drop || a_tris[i].kind[k] == Edge::LedgeUp || a_tris[i].kind[k] == Edge::Portal))) continue;
                const std::uint32_t a = find(static_cast<std::uint32_t>(i)), b = find(static_cast<std::uint32_t>(nb)); if (a != b) par[std::max(a, b)] = std::min(a, b); }
            std::vector<std::uint32_t> count(n, 0); for (std::size_t i = 0; i < n; ++i) ++count[find(static_cast<std::uint32_t>(i))];
            std::vector<std::uint32_t> roots; for (std::size_t i = 0; i < n; ++i) if (count[i]) roots.push_back(static_cast<std::uint32_t>(i));
            std::stable_sort(roots.begin(), roots.end(), [&](std::uint32_t a, std::uint32_t b) { return count[a] > count[b]; });   // ties keep the lower root first
            std::vector<std::uint32_t> id(n, 0); for (std::size_t r = 0; r < roots.size(); ++r) id[roots[r]] = static_cast<std::uint32_t>(r);
            out.resize(n); for (std::size_t i = 0; i < n; ++i) out[i] = id[find(static_cast<std::uint32_t>(i))];
        };
        label(false, a_walk); label(true, a_weak);
    }

}
