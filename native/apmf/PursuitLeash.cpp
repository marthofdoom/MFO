// apmf/PursuitLeash.cpp -- the ch.23 in-combat PURSUIT LEASH table (ABI v16,
// kIntent_PursuitLeash): MFO's client side of Harbinger's "Leashing an NPC in combat"
// (Harbinger Docs/INTEGRATION.md). ClickUp 86e3ex5ve (the MFO half) / 86e3erv94 (the
// confidence leash in combat). A new claim family, so its own file in apmf/; the shape
// mirrors apmf/CombatEntry.cpp.
//
// WHAT MFO DECLARES: anchor = the PLAYER (0x14), radius = Confidence::LeashRadius(follower),
// passed in by the Scheduler's combat table. LeashRadius is the confidence tenet's "how far
// from the PLAYER he is willing to be" (Confidence.h), which is exactly ch.23's question (a
// radius around the anchor). ChaseRadius is NOT used: it is measured from the FOLLOWER (how
// far out PickFoe may choose a foe) and answers a different question.
//
// STABLE, NOT PER-TICK: the confidence read moves with every hit, so the claim is Repointed
// only when the wanted radius leaves a band around the CLAIMED one (kRepointFrac of it, at
// least kRepointFloor units) -- no per-service Repoint churn.
//
// LIFETIME: filed on the first combat-table service that is not a retreat; released by the
// Scheduler on retreat start (the retreat has its own road) and on the party-OOC teardown
// (combat end), by Followers::ReleaseHeldState (dismissal / MFO-off), by
// ClearTransientState (load / revert), and by the sweep whenever the follower is retreating.
// A claim Harbinger ENDS (was live, now not: an unload or an outranking leash) or that never
// goes live is logged and kept as ENDED until the fight ends, so it is never re-filed in a
// loop within one fight. Harbinger absent / older than v16 / the seat refused = no leash,
// exactly as before (MFO had none in combat).
// Threads: the pump's AddTask worker for everything but ClearTransientState (the drained-pump
// load window); its OWN mutex, never g_mx (same lock-order note as apmf/ReentryDeny.cpp).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Packages.h"   // IsRetreating -- the sweep's retreat backstop

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kLeashMinAbi = 16;   // the ABI that first serves intent 23
        constexpr RE::FormID    kPlayerId    = 0x14;

        // Repoint band: 20 % of the claimed radius, at least 256 u. With the shipped dials
        // (fLeashMin 512 .. fLeashMax 4000) one band is a confidence change of >= ~0.07 at
        // the floor and ~0.2 at the top -- a real change in how the fight is going, not the
        // hit-by-hit wobble of Of().
        constexpr float kRepointFrac  = 0.20f;
        constexpr float kRepointFloor = 256.0f;

        // Same never-live floor as the sibling tables (see apmf/ReentryDeny.cpp).
        constexpr std::uint32_t kLeashNeverLiveSweeps = 15;

        struct LeashClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;   // invalid once ENDED
            float            radius         = 0.0f;                       // the radius last sent
            std::uint32_t    unpausedSweeps = 0;
            bool             everLive       = false;
            bool             ended          = false;   // ended by Harbinger / never live: not re-filed this fight
        };
        std::mutex                                 g_leashMx;
        std::unordered_map<RE::FormID, LeashClaim> g_leashes;   // guarded by g_leashMx

        std::atomic<bool> g_leashSeatRefused{ false };   // session-stable, like the ch.21 / ch.22 tables

        const APMF_API::APMF_API_v6* LeashApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kLeashMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v6*>(api);   // v16 >= v6: Repoint + IsClaimLive
        }

        bool LeashGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        bool OutsideBand(float a_claimed, float a_want) {
            return std::fabs(a_want - a_claimed) >= std::max(kRepointFloor, kRepointFrac * a_claimed);
        }
    }

    void ServicePursuitLeash(RE::FormID a_follower, float a_radius) {
        const auto* api = LeashApi();
        if (!api || g_leashSeatRefused.load(std::memory_order_relaxed)) return;
        if (a_follower == 0 || a_follower == kPlayerId) return;
        if (!(a_radius > 0.0f) || !std::isfinite(a_radius)) return;   // Harbinger refuses these; never send one

        std::scoped_lock lock(g_leashMx);
        const auto it = g_leashes.find(a_follower);
        if (it == g_leashes.end()) {
            APMF_API::APMF_Param prm{};
            prm.target = kPlayerId;   // the ANCHOR
            prm.fval   = a_radius;    // the radius, game units
            const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_PursuitLeash, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_leashSeatRefused.exchange(true)) {
                    spdlog::warn("[leash] Harbinger REFUSED the ch.23 pursuit leash for {:08X} at the call: the "
                                 "leash is not armed (VR, a runtime other than 1.6.1170 / 1.5.97, [PursuitLeash] "
                                 "bPursuitLeash=0 in APMF.ini, or its address self-check; APMF's "
                                 "'[apmf][pursuit-leash] claim refused' line names it). No in-combat leash this "
                                 "session.", a_follower);
                }
                return;
            }
            LeashClaim c{};
            c.handle = h;
            c.radius = a_radius;
            g_leashes.emplace(a_follower, c);
            spdlog::info("[leash] {:08X}: ch.23 pursuit leash CLAIMED (h={}, anchor the player, radius {:.0f})",
                         a_follower, h, a_radius);
            return;
        }
        auto& c = it->second;
        if (c.ended || c.handle == APMF_API::kInvalidHandle) return;   // no re-file within this fight
        if (!OutsideBand(c.radius, a_radius)) return;
        APMF_API::APMF_Param prm{};
        prm.target = kPlayerId;
        prm.fval   = a_radius;
        api->Repoint(c.handle, &prm);
        spdlog::info("[leash] {:08X}: ch.23 pursuit leash radius {:.0f} -> {:.0f} (Repoint, h={})",
                     a_follower, c.radius, a_radius, c.handle);
        c.radius = a_radius;
    }

    void ReleasePursuitLeash(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_leashMx);
            const auto it = g_leashes.find(a_follower);
            if (it == g_leashes.end()) return;
            h = it->second.handle;
            g_leashes.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle)
            api->Release(h);   // no-op on an ended claim; FIFO-cancels a pending one
        spdlog::info("[leash] {:08X}: ch.23 pursuit leash released ({})", a_follower, a_why);
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_leashMx itself; g_mx NOT held.
    void SweepPursuitLeashes() {
        const auto* api = LeashApi();
        if (!api) return;
        std::vector<RE::FormID> retreating;
        {
            std::scoped_lock lock(g_leashMx);
            if (g_leashes.empty()) return;
            const bool paused = LeashGamePaused();
            for (auto& [fid, c] : g_leashes) {
                if (Packages::IsRetreating(fid)) { retreating.push_back(fid); continue; }
                if (c.ended) continue;
                if (api->IsClaimLive(c.handle)) { c.everLive = true; continue; }
                if (!c.everLive) {
                    if (!paused) ++c.unpausedSweeps;
                    if (c.unpausedSweeps <= kLeashNeverLiveSweeps) continue;
                    spdlog::warn("[leash] {:08X}: ch.23 pursuit leash NEVER WENT LIVE (h={}, {} unpaused sweeps) "
                                 "-- released; no leash for him until this fight ends. APMF's log should say why.",
                                 fid, c.handle, c.unpausedSweeps);
                } else {
                    spdlog::info("[leash] {:08X}: ch.23 pursuit leash ENDED BY HARBINGER (h={}: he unloaded, or "
                                 "an outranking leash took him) -- not re-filed until this fight ends.",
                                 fid, c.handle);
                }
                api->Release(c.handle);
                c.handle = APMF_API::kInvalidHandle;
                c.ended  = true;
            }
        }
        for (const auto fid : retreating) ReleasePursuitLeash(fid, "retreating");
    }

    void ClearPursuitLeashes() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_leashMx);
        for (auto& [fid, c] : g_leashes)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_leashes.clear();
    }

}
