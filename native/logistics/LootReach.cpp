// logistics/LootReach.cpp -- NAVMESH CONNECTIVITY pre-check for a loot walk (LoreRim round 2, item 1).
//
// NavmeshReach (LootTake.cpp) only answers "is there mesh NEAR the ref". A body on a disconnected
// island of mesh (behind a collapsed stair, on a ledge, in a sealed room) passes it, the follower walks
// at it, stalls ("no progress") or is BLOCKED. This is the missing half: do the follower's triangle and
// the ref's triangle sit in ONE connected component of the decoded BSNavmesh graph?
//
// It is read-only over the same decoded data NearestNavmeshDist reads (attached cell + the cell's own
// spinLock to COLLECT the raw mesh pointers, exactly as NearestNavmeshDist does). It is NOT an engine path
// query (none exists in the pinned CommonLib and a real one needs a new engine address -- tier A).
// CONSERVATIVE BY CONSTRUCTION: it answers Island ONLY when the flood fill from the follower's
// triangle is EXHAUSTED inside meshes it could fully read, never met an unreadable/unloaded link,
// stayed under the triangle budget, and did not reach the ref's triangle. Every doubt (a triangle not
// found, a portal to an unloaded mesh, different interior cells, budget hit) is Unknown = proceed
// exactly as before. Closed doors, obstacles and enable/disable portals are treated as PASSABLE.
#include "Logistics_internal.h"

#include <unordered_set>

namespace MFO::Logistics {

    namespace {
        using TriFlag = RE::BSNavmeshTriangle::TriangleFlag;

        // Triangle (xy) containing a_p whose plane z is closest to a_p.z; -1 if none within a_maxDz.
        int FindTriangle(RE::NavMesh* a_mesh, const RE::NiPoint3& a_p, float a_maxDz, float& a_bestDz) {
            int best = -1;
            const auto& vs = a_mesh->vertices;
            const auto  nv = static_cast<std::size_t>(vs.size());
            const auto  nt = static_cast<std::uint32_t>(a_mesh->triangles.size());
            for (std::uint32_t i = 0; i < nt; ++i) {
                const auto& t = a_mesh->triangles[i];
                if (t.triangleFlags.any(TriFlag::kDeleted)) continue;
                if (t.vertices[0] >= nv || t.vertices[1] >= nv || t.vertices[2] >= nv) continue;
                const auto& A = vs[t.vertices[0]].location;
                const auto& B = vs[t.vertices[1]].location;
                const auto& C = vs[t.vertices[2]].location;
                // 2D barycentric
                const float d = (B.y - C.y) * (A.x - C.x) + (C.x - B.x) * (A.y - C.y);
                if (std::fabs(d) < 1e-3f) continue;
                const float l1 = ((B.y - C.y) * (a_p.x - C.x) + (C.x - B.x) * (a_p.y - C.y)) / d;
                const float l2 = ((C.y - A.y) * (a_p.x - C.x) + (A.x - C.x) * (a_p.y - C.y)) / d;
                const float l3 = 1.0f - l1 - l2;
                constexpr float kEps = -0.01f;
                if (l1 < kEps || l2 < kEps || l3 < kEps) continue;
                const float z  = l1 * A.z + l2 * B.z + l3 * C.z;
                const float dz = std::fabs(z - a_p.z);
                if (dz <= a_maxDz && dz < a_bestDz) { a_bestDz = dz; best = static_cast<int>(i); }
            }
            return best;
        }

        void Collect(RE::TESObjectCELL* a_cell, std::vector<RE::NavMesh*>& a_out) {
            if (!a_cell || !a_cell->IsAttached()) return;
            auto& rd = a_cell->GetRuntimeData();
            RE::BSSpinLockGuard lock(rd.spinLock);
            if (!rd.navMeshes) return;
            for (const auto& m : rd.navMeshes->navMeshes)
                if (m) a_out.push_back(m.get());
        }

        std::unordered_set<RE::FormID> g_unreachLogged;   // worker-only (the loot scan); logged-once set
    }

    NavConn NavConnected(RE::Actor* a_follower, RE::TESObjectREFR* a_ref) {
        if (!a_follower || !a_ref) return NavConn::Unknown;
        auto* fc = a_follower->GetParentCell();
        auto* rc = a_ref->GetParentCell();
        if (!fc || !rc) return NavConn::Unknown;
        if (fc != rc && (fc->IsInteriorCell() || rc->IsInteriorCell())) return NavConn::Unknown;   // via a door: not judged

        std::vector<RE::NavMesh*> pool;
        Collect(fc, pool);
        if (rc != fc) Collect(rc, pool);
        if (pool.empty()) return NavConn::Unknown;

        const RE::NiPoint3 fp = a_follower->GetPosition();
        const RE::NiPoint3 tp = a_ref->GetPosition();

        RE::NavMesh* fm = nullptr; int ft = -1;
        RE::NavMesh* tm = nullptr; int tt = -1;
        float fdz = 1e9f, tdz = 1e9f;
        for (auto* m : pool) {
            if (!m) continue;
            if (const int t = FindTriangle(m, fp, 150.0f, fdz); t >= 0) { fm = m; ft = t; }
            if (const int t = FindTriangle(m, tp, 150.0f, tdz); t >= 0) { tm = m; tt = t; }
        }
        if (!fm || !tm) return NavConn::Unknown;

        auto key = [](const RE::NavMesh* a_m, std::uint32_t a_t) {
            return (static_cast<std::uint64_t>(a_m->GetFormID()) << 16) ^ a_t;
        };
        std::unordered_set<std::uint64_t>                 seen;
        std::vector<std::pair<RE::NavMesh*, std::uint32_t>> stack;
        constexpr std::size_t kBudget = 40000;
        seen.insert(key(fm, static_cast<std::uint32_t>(ft)));
        stack.emplace_back(fm, static_cast<std::uint32_t>(ft));
        const std::uint64_t goal = key(tm, static_cast<std::uint32_t>(tt));

        while (!stack.empty()) {
            auto [m, ti] = stack.back();
            stack.pop_back();
            if (key(m, ti) == goal) return NavConn::Connected;
            if (seen.size() > kBudget) return NavConn::Unknown;
            const auto& t = m->triangles[ti];
            for (int e = 0; e < 3; ++e) {
                const std::uint16_t nb = t.triangles[e];
                const bool link = t.triangleFlags.any(static_cast<TriFlag>(1 << e));
                RE::NavMesh*  nm = m;
                std::uint32_t nt = nb;
                if (link) {
                    // An edge into ANOTHER mesh: nb indexes extraEdgeInfo (xEdit NVNM external connection).
                    if (nb >= m->extraEdgeInfo.size()) return NavConn::Unknown;
                    const auto& x = m->extraEdgeInfo[nb];
                    nm = RE::TESForm::LookupByID<RE::NavMesh>(x.portal.otherMeshID);
                    if (!nm || nm->vertices.empty() || x.portal.triangle >= nm->triangles.size()) return NavConn::Unknown;   // unloaded: unknown
                    nt = x.portal.triangle;
                } else {
                    if (nb == 0xFFFF) continue;   // boundary edge
                    if (nb >= m->triangles.size()) return NavConn::Unknown;
                }
                if (nm->triangles[nt].triangleFlags.any(TriFlag::kDeleted)) continue;
                if (seen.insert(key(nm, nt)).second) stack.emplace_back(nm, nt);
            }
        }
        return NavConn::Island;   // exhausted inside fully-read meshes without meeting the ref's triangle
    }

    bool NoteUnreachableOnce(RE::FormID a_ref) {
        if (g_unreachLogged.size() >= 1024) g_unreachLogged.clear();
        return g_unreachLogged.insert(a_ref).second;
    }
    void ClearUnreachableLog() { g_unreachLogged.clear(); }

}
