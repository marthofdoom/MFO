// logistics/LootReach.cpp -- NAVMESH CONNECTIVITY pre-check for a loot walk (LoreRim round 2, item 1).
//
// NavmeshReach (LootTake.cpp) only answers "is there mesh NEAR the ref". A body on a disconnected
// island of mesh (behind a collapsed stair, on a ledge, in a sealed room) passes it, the follower walks
// at it, stalls ("no progress") or is BLOCKED. This is the missing half: do the follower's triangle and
// the ref's triangle sit in ONE connected component of the decoded BSNavmesh graph?
//
// It is read-only over the same decoded data NearestNavmeshDist reads. It RUNS ON THE MAIN THREAD (posted by
// NavConnected, verdict cached per ref, used on a later scan): a cell unload happens on main, so no visited mesh can
// be freed mid-fill, and every visited mesh must belong to the follower's or the ref's attached cell. It is NOT an engine path
// query (none exists in the pinned CommonLib and a real one needs a new engine address -- tier A).
// CONSERVATIVE BY CONSTRUCTION: it answers Island ONLY when the flood fill from the follower's
// triangle is EXHAUSTED inside meshes it could fully read, never met an unreadable/unloaded link,
// stayed under the triangle budget, and did not reach the ref's triangle. Every doubt (a triangle not
// found, a portal to an unloaded mesh, different interior cells, budget hit) is Unknown = proceed
// exactly as before. Closed doors, obstacles and enable/disable portals are treated as PASSABLE.
#include "Logistics_internal.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "MainThread.h"

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

        struct Verdict { RE::FormID follower = 0; NavConn verdict = NavConn::Unknown; std::chrono::steady_clock::time_point at{}; };
        constexpr std::chrono::seconds kVerdictTtl{ 30 };
        std::mutex                              g_reachMx;       // LEAF: map access only
        std::unordered_map<RE::FormID, Verdict> g_reach;         // guarded by g_reachMx
        std::unordered_set<RE::FormID>          g_reachPending;  // guarded by g_reachMx
        std::unordered_set<RE::FormID> g_unreachLogged;   // worker-only (the loot scan); logged-once set
    }

    // MAIN THREAD ONLY. Pool = the meshes of the follower's and the ref's attached cells; every mesh the fill
    // visits is in that pool (a link out of it reads Unknown), and nothing unloads a cell during the call, so no
    // visited mesh can be freed under it (review of 8c68e4a, SEV-2).
    static NavConn ComputeOnMain(RE::FormID a_followerID, RE::FormID a_refID) {
        auto* a_follower = RE::TESForm::LookupByID<RE::Actor>(a_followerID);
        auto* a_ref      = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_refID);
        if (!a_follower || !a_ref) return NavConn::Unknown;
        auto* fc = a_follower->GetParentCell();
        auto* rc = a_ref->GetParentCell();
        if (!fc || !rc) return NavConn::Unknown;
        if (fc != rc && (fc->IsInteriorCell() || rc->IsInteriorCell())) return NavConn::Unknown;   // via a door: not judged

        std::vector<RE::NavMesh*> pool;
        Collect(fc, pool);
        if (rc != fc) Collect(rc, pool);
        if (pool.empty()) return NavConn::Unknown;
        const std::unordered_set<RE::NavMesh*> inPool(pool.begin(), pool.end());

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
        std::unordered_set<std::uint64_t>                   seen;
        std::vector<std::pair<RE::NavMesh*, std::uint32_t>> stack;
        constexpr std::size_t kBudget = 12000;
        seen.insert(key(fm, static_cast<std::uint32_t>(ft)));
        stack.emplace_back(fm, static_cast<std::uint32_t>(ft));
        const std::uint64_t goal = key(tm, static_cast<std::uint32_t>(tt));

        // Island only when CERTAIN: any one-sided edge, any teleport-door portal triangle reached, any link out of the
        // pool or unreadable mesh poisons the verdict (Unknown), never an Island.
        bool doubt = false;
        std::unordered_set<RE::NavMesh*> doorChecked;
        while (!stack.empty()) {
            auto [m, ti] = stack.back();
            stack.pop_back();
            if (key(m, ti) == goal) return NavConn::Connected;
            if (seen.size() > kBudget) return NavConn::Unknown;
            for (const auto& dp : m->doorPortals)
                if (dp.owningTriangleIndex == ti) doubt = true;   // a teleport door leaves this component
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
                    if (!nm || !inPool.count(nm) || nm->vertices.empty() || x.portal.triangle >= nm->triangles.size())
                        return NavConn::Unknown;   // outside the pool / unloaded: unknown
                    nt = x.portal.triangle;
                    // reciprocal: the far triangle must link back to (m, ti), else a one-sided (unfinalized) link
                    bool back = false;
                    const auto& ft2 = nm->triangles[nt];
                    for (int k = 0; k < 3 && !back; ++k) {
                        if (!ft2.triangleFlags.any(static_cast<TriFlag>(1 << k))) continue;
                        const auto bi = ft2.triangles[k];
                        if (bi < nm->extraEdgeInfo.size() && nm->extraEdgeInfo[bi].portal.otherMeshID == m->GetFormID() &&
                            nm->extraEdgeInfo[bi].portal.triangle == ti) back = true;
                    }
                    if (!back) doubt = true;
                } else {
                    if (nb == 0xFFFF) continue;   // boundary edge
                    if (nb >= m->triangles.size()) return NavConn::Unknown;
                    const auto& n2 = m->triangles[nb];
                    bool back = false;
                    for (int k = 0; k < 3; ++k)
                        if (!n2.triangleFlags.any(static_cast<TriFlag>(1 << k)) && n2.triangles[k] == ti) back = true;
                    if (!back) doubt = true;   // one-sided adjacency
                }
                if (nm->triangles[nt].triangleFlags.any(TriFlag::kDeleted)) continue;
                if (seen.insert(key(nm, nt)).second) stack.emplace_back(nm, nt);
            }
        }
        return doubt ? NavConn::Unknown : NavConn::Island;   // exhausted inside fully-read meshes, nothing doubtful
    }

    // WORKER. Cached verdict for (ref) or Unknown; a miss posts ONE main-thread computation (see ComputeOnMain) and
    // the verdict is used on a later scan. Leaf mutex g_reachMx: held only for map access, never across a call.
    NavConn NavConnected(RE::Actor* a_follower, RE::TESObjectREFR* a_ref) {
        if (!a_follower || !a_ref) return NavConn::Unknown;
        const RE::FormID fid = a_follower->GetFormID();
        const RE::FormID rid = a_ref->GetFormID();
        const auto now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk(g_reachMx);
            if (const auto it = g_reach.find(rid); it != g_reach.end() && it->second.follower == fid &&
                now - it->second.at < kVerdictTtl)
                return it->second.verdict;
            if (g_reachPending.count(rid) || g_reachPending.size() >= 4) return NavConn::Unknown;
            g_reachPending.insert(rid);
        }
        MainThread::Post([fid, rid]() {
            const NavConn v = ComputeOnMain(fid, rid);
            std::scoped_lock lk(g_reachMx);
            g_reachPending.erase(rid);
            if (g_reach.size() >= 512) g_reach.clear();
            g_reach[rid] = Verdict{ fid, v, std::chrono::steady_clock::now() };
        });
        return NavConn::Unknown;
    }

    bool NoteUnreachableOnce(RE::FormID a_ref) {
        if (g_unreachLogged.size() >= 1024) g_unreachLogged.clear();
        return g_unreachLogged.insert(a_ref).second;
    }
    void ClearUnreachableLog() {
        g_unreachLogged.clear();
        std::scoped_lock lk(g_reachMx);
        g_reach.clear();
        g_reachPending.clear();
    }

}
