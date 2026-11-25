// WALKABLE POINTS AROUND A FOLLOWER (DEV BUILDS ONLY; the g0b hold exerciser's place searches).
// A cover / flank point must be found AROUND obstacles, not along one straight clear walk (Deck 2026-11-24: g0b.cover stayed pending a whole
// session in a room with pillars). This walks the decoded navmesh the way logistics/LootReach.cpp does (same reads, MAIN THREAD ONLY, only the
// meshes of the attached cells named below), as a shortest-path search from his triangle, and samples each reached triangle on a grid.
// MODEL: a step is the line from where a triangle was entered to the next edge's middle. Good enough to sort by distance walked. Ledges and
// enable / disable portals are NOT crossed. Nothing is kept: no pointer outlives the call. To change the density or the reach: kNav.
#include "selftest/SelfTest_internal.h"

#include <cmath>
#include <queue>

namespace MFO::SelfTest {

    namespace {
        using TriFlag = RE::BSNavmeshTriangle::TriangleFlag;
        struct NavTuning {
            float       spacing = 80.0f;    // sample grid, units
            int         maxSplit = 8;       // at most this many samples along an edge
            std::size_t maxTris = 4000;     // triangles closed
            std::size_t maxPoints = 1500;
        };
        constexpr NavTuning kNav{};

        void Collect(RE::TESObjectCELL* a_cell, std::vector<RE::NavMesh*>& a_out) {
            if (!a_cell || !a_cell->IsAttached()) return;
            auto&               rd = a_cell->GetRuntimeData();
            RE::BSSpinLockGuard lock(rd.spinLock);
            if (!rd.navMeshes) return;
            for (const auto& m : rd.navMeshes->navMeshes)
                if (m && std::find(a_out.begin(), a_out.end(), m.get()) == a_out.end()) a_out.push_back(m.get());
        }
        bool Corners(const RE::NavMesh* m, std::uint32_t a_t, RE::NiPoint3 (&o)[3]) {
            if (a_t >= m->triangles.size()) return false;
            const auto& t = m->triangles[a_t];
            if (t.triangleFlags.any(TriFlag::kDeleted)) return false;
            for (int k = 0; k < 3; ++k) {
                if (t.vertices[k] >= m->vertices.size()) return false;
                o[k] = m->vertices[t.vertices[k]].location;
            }
            return true;
        }
        int FindTriangle(const RE::NavMesh* m, const RE::NiPoint3& p, float& a_bestDz) {
            int        best = -1;
            const auto n = static_cast<std::uint32_t>(m->triangles.size());
            for (std::uint32_t i = 0; i < n; ++i) {
                RE::NiPoint3 c[3];
                if (!Corners(m, i, c)) continue;
                const float d = (c[1].y - c[2].y) * (c[0].x - c[2].x) + (c[2].x - c[1].x) * (c[0].y - c[2].y);
                if (std::fabs(d) < 1e-3f) continue;
                const float l1 = ((c[1].y - c[2].y) * (p.x - c[2].x) + (c[2].x - c[1].x) * (p.y - c[2].y)) / d;
                const float l2 = ((c[2].y - c[0].y) * (p.x - c[2].x) + (c[0].x - c[2].x) * (p.y - c[2].y)) / d;
                const float l3 = 1.0f - l1 - l2;
                if (l1 < -0.01f || l2 < -0.01f || l3 < -0.01f) continue;
                const float dz = std::fabs(l1 * c[0].z + l2 * c[1].z + l3 * c[2].z - p.z);
                if (dz <= 150.0f && dz < a_bestDz) { a_bestDz = dz; best = static_cast<int>(i); }
            }
            return best;
        }
    }

    std::vector<NavPoint> WalkPoints(RE::Actor* a_from, RE::Actor* a_foe, float a_maxWalk, std::string& a_why) {
        std::vector<NavPoint>     out;
        std::vector<RE::NavMesh*> pool;
        Collect(a_from->GetParentCell(), pool);
        if (a_foe) Collect(a_foe->GetParentCell(), pool);
        if (auto* pc = RE::PlayerCharacter::GetSingleton()) Collect(pc->GetParentCell(), pool);
        const auto   me = a_from->GetPosition();
        RE::NavMesh* sm = nullptr;
        int          st = -1;
        float        dz = 1e9f;
        for (auto* m : pool)
            if (const int t = FindTriangle(m, me, dz); t >= 0) { sm = m; st = t; }
        if (!sm) { a_why = std::format("he stands on no navmesh triangle ({} mesh(es) read)", pool.size()); return out; }

        struct Node { float g; RE::NavMesh* m; std::uint32_t t; RE::NiPoint3 at; };
        const auto later = [](const Node& a, const Node& b) { return a.g > b.g; };
        std::priority_queue<Node, std::vector<Node>, decltype(later)> open(later);
        std::unordered_set<std::uint64_t>                             closed, cells;
        open.push({ 0.0f, sm, static_cast<std::uint32_t>(st), me });
        while (!open.empty() && closed.size() < kNav.maxTris && out.size() < kNav.maxPoints) {
            const Node n = open.top();
            open.pop();
            if (!closed.insert((static_cast<std::uint64_t>(n.m->GetFormID()) << 16) ^ n.t).second) continue;
            RE::NiPoint3 c[3];
            if (!Corners(n.m, n.t, c)) continue;
            const float edge = std::max({ c[0].GetDistance(c[1]), c[1].GetDistance(c[2]), c[2].GetDistance(c[0]) });
            const int   k = std::clamp(static_cast<int>(std::ceil(edge / kNav.spacing)), 1, kNav.maxSplit);
            for (int i = 0; i < k; ++i)
                for (int j = 0; i + j < k; ++j) {
                    const float  u = (static_cast<float>(i) + 1.0f / 3.0f) / static_cast<float>(k), v = (static_cast<float>(j) + 1.0f / 3.0f) / static_cast<float>(k);
                    const auto   p = c[0] + (c[1] - c[0]) * u + (c[2] - c[0]) * v;
                    const float  w = n.g + n.at.GetDistance(p);
                    if (w > a_maxWalk) continue;
                    const auto   q = [](float f) { return static_cast<std::uint64_t>(static_cast<std::uint32_t>(static_cast<std::int32_t>(std::floor(f / 60.0f))) & 0x1FFFFF); };
                    if (cells.insert((q(p.x) << 42) | (q(p.y) << 21) | q(p.z)).second) out.push_back({ p, w });   // one point per 60 u
                }
            const auto& t = n.m->triangles[n.t];
            for (int e = 0; e < 3; ++e) {
                const std::uint16_t nb = t.triangles[e];
                RE::NavMesh*        nm = n.m;
                std::uint32_t       nt = nb;
                if (t.triangleFlags.any(static_cast<TriFlag>(1 << e))) {
                    if (nb >= n.m->extraEdgeInfo.size()) continue;
                    const auto& x = n.m->extraEdgeInfo[nb];
                    if (x.type.get() != RE::EDGE_EXTRA_INFO_TYPE::kPortal) continue;
                    nm = RE::TESForm::LookupByID<RE::NavMesh>(x.portal.otherMeshID);
                    if (!nm || std::find(pool.begin(), pool.end(), nm) == pool.end()) continue;
                    nt = x.portal.triangle;
                } else if (nb == 0xFFFF) {
                    continue;
                }
                RE::NiPoint3 nc[3];
                if (!Corners(nm, nt, nc)) continue;
                const auto  mid = (c[e] + c[(e + 1) % 3]) * 0.5f;
                const float g = n.g + n.at.GetDistance(mid);
                if (g <= a_maxWalk) open.push({ g, nm, nt, mid });
            }
        }
        std::sort(out.begin(), out.end(), [](const NavPoint& a, const NavPoint& b) { return a.walk < b.walk; });
        a_why = std::format("{} triangle(s) walked, {} point(s)", closed.size(), out.size());
        return out;
    }

}
