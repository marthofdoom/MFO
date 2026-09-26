// apmf/ReentryDeny.cpp -- the ch.22 combat RE-ENTRY deny table (ABI v15,
// kIntent_CombatReentryDeny): MFO's client side of Harbinger's "retreat = StopCombat once +
// deny re-entry" (Harbinger Docs/INTEGRATION.md "Keeping an NPC out of combat"). Its one
// caller is the retreat (Packages::RetreatFill, both roads), driven by the Scheduler's
// ServiceRetreat (ClickUp 86e3ex5v9, the MFO half). A new claim family, so its own file in
// apmf/ (CLAUDE.md "a new mechanism is a new file"); the shape mirrors apmf/CombatEntry.cpp.
//
// THE RECIPE, EXACTLY (INTEGRATION.md): claim first; the handle is PENDING until IsClaimLive
// reads true (the request is applied at Harbinger's next drain); ONLY THEN post the single
// StopCombat -- through the retreat's own main-thread road (Packages::RetreatReengage ->
// PostRetreatStopCombat, generation-checked). A StopCombat before the claim is live leaves the
// gap the recipe exists to close. "Not live yet" is never "ended".
//
// WHAT CHANGES FOR THE RETREAT while the deny holds: the per-re-entry StopCombat of the TRAVEL
// phase is not posted (the engine cannot put him back in; a re-entry seen anyway is either a
// declared ch.21 entry, which passes by contract, or a Harbinger DENY MISSED -- logged, never
// papered over with another StopCombat, principle 7). With no claim (Harbinger absent, older
// than v15, the seat refused, VR) or once the claim has ENDED (window elapsed / owner dead /
// never went live), the Scheduler keeps the shipped per-re-entry StopCombat: that is the
// documented no-ch.22 degrade, not a fallback from a working deny.
//
// THE CLAIM IS RELEASED when the retreat reaches the player (TRAVEL -> STAY: STAY must never
// bench him -- "engaged at your side" needs the engine's combat entry to pass), and on every
// retreat end (Scheduler finish(): no live foes / timeout / arrival release / STAY end), on
// dismissal / MFO-off (Followers::ReleaseHeldState), on load / revert (ClearTransientState),
// and by the sweep below whenever Packages::IsRetreating reads false (any release the driver
// did not make). Threads: every entry point runs on the pump's AddTask worker (Scheduler /
// Packages / Followers / APMFBridge::Tick) except ClearTransientState (the drained-pump load
// window); the table has its OWN mutex and never takes the bridge's g_mx, so the
// g_mx -> g_denyMx order in ClearTransientState cannot invert.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Packages.h"   // IsRetreating / RetreatReengage -- the retreat's own StopCombat road

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kDenyMinAbi = 15;   // the ABI that first serves intent 22

        // THE WINDOW (seconds), sent as param.fval. The deny is released at ARRIVAL (TRAVEL ->
        // STAY), so the window only has to cover TRAVEL, whose own bound is the Scheduler's
        // kRetreatTimeout (30 s, Scheduler.cpp; the retreat clock is steady_clock from
        // RetreatFill, the same moment and the same monotonic kind of clock as Harbinger's
        // window, which runs from the request -- a pause spends both alike). +5 s covers the
        // timeout being SEEN only on his own service (N x 133 ms: N <= 37 followers), so
        // Harbinger's own expiry never ends the deny under a retreat that is still travelling.
        // It is a BACKSTOP: every retreat end releases the claim explicitly. Keep it in step
        // with kRetreatTimeout (a larger timeout needs a larger window; Harbinger clamps >120).
        constexpr float kRetreatDenyWindowSecs = 35.0f;

        // A never-live claim is judged failed after this many UNPAUSED sweeps (~2 s of running
        // game at the pump's 133 ms): Harbinger's drain is its per-frame PlayerCharacter::Update
        // seat, which a game-pausing menu stops. A FLOOR (principle 9), the same value and
        // reasoning as the ch.20 / ch.21 tables (kPinNeverLiveSweeps / kEntryNeverLiveSweeps).
        constexpr std::uint32_t kDenyNeverLiveSweeps = 15;

        struct DenyClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;   // invalid once ENDED
            std::uint32_t    unpausedSweeps = 0;
            bool             everLive       = false;   // IsClaimLive read true (StopCombat posted)
            bool             ended          = false;   // ended by Harbinger or never went live
        };
        std::mutex                                g_denyMx;
        std::unordered_map<RE::FormID, DenyClaim> g_denies;   // guarded by g_denyMx

        // A synchronous refusal with valid params = the ch.22 seat is not installed (VR, a
        // runtime other than 1.6.1170 / 1.5.97, [CombatReentryDeny] bCombatReentryDeny=0, the
        // self-check). Session-stable: Harbinger installs the seat once at its kDataLoaded.
        std::atomic<bool> g_denySeatRefused{ false };

        const APMF_API::APMF_API_v6* DenyApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kDenyMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v6*>(api);   // v15 >= v6: IsClaimLive
        }

        bool DenyGamePaused() {
            auto* ui = RE::UI::GetSingleton();   // the same read the pin / entry tables make on this worker
            return ui && ui->GameIsPaused();
        }
    }

    bool ClaimRetreatReentryDeny(RE::FormID a_follower) {
        const auto* api = DenyApi();
        if (!api || g_denySeatRefused.load(std::memory_order_relaxed)) return false;
        if (a_follower == 0 || a_follower == 0x14) return false;   // Harbinger refuses the player too

        APMF_API::APMF_Param prm{};
        prm.fval = kRetreatDenyWindowSecs;   // the window; nothing else is read
        const APMF_API::Handle h =
            api->RequestEx(a_follower, APMF_API::kIntent_CombatReentryDeny, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            if (!g_denySeatRefused.exchange(true)) {
                spdlog::warn("[retreat] Harbinger REFUSED the ch.22 re-entry deny for {:08X} at the call: the "
                             "ch.22 seat is not installed (VR, a runtime other than 1.6.1170 / 1.5.97, "
                             "[CombatReentryDeny] bCombatReentryDeny=0 in APMF.ini, or its address self-check; "
                             "APMF's log names it). Retreats use the per-re-entry StopCombat for this session.",
                             a_follower);
            }
            return false;
        }
        APMF_API::Handle old = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_denyMx);
            auto& d = g_denies[a_follower];
            old = d.handle;
            d = DenyClaim{};
            d.handle = h;
        }
        if (old != APMF_API::kInvalidHandle) api->Release(old);   // one deny per follower
        spdlog::info("[retreat] {:08X}: ch.22 re-entry deny CLAIMED (h={}, window {:.0f} s) -- PENDING; the "
                     "single StopCombat is posted on the first sweep that reads it LIVE",
                     a_follower, h, kRetreatDenyWindowSecs);
        return true;
    }

    DenyState RetreatReentryDenyStateOf(RE::FormID a_follower) {
        std::scoped_lock lock(g_denyMx);
        const auto it = g_denies.find(a_follower);
        if (it == g_denies.end()) return DenyState::None;
        if (it->second.ended)    return DenyState::Ended;
        return it->second.everLive ? DenyState::Live : DenyState::Pending;
    }

    void ReleaseRetreatReentryDeny(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        bool wasLive = false;
        {
            std::scoped_lock lock(g_denyMx);
            const auto it = g_denies.find(a_follower);
            if (it == g_denies.end()) return;
            h       = it->second.handle;
            wasLive = it->second.everLive && !it->second.ended;
            g_denies.erase(it);
        }
        // ALWAYS Release a held handle: a no-op on one Harbinger already ended, and it
        // FIFO-cancels one still pending, so no orphan claim can outlive the retreat.
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle)
            api->Release(h);
        spdlog::info("[retreat] {:08X}: ch.22 re-entry deny released ({}; {}) -- engine combat entries pass again",
                     a_follower, a_why, wasLive ? "was live" : "was pending or already ended");
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_denyMx itself; called with
    // g_mx NOT held. The StopCombat posts run after the table lock is dropped.
    void SweepReentryDenies() {
        const auto* api = DenyApi();
        if (!api) return;
        enum class Act : std::uint8_t { Stop, StopNeverLive, Release };
        std::vector<std::pair<RE::FormID, Act>> acts;
        {
            std::scoped_lock lock(g_denyMx);
            if (g_denies.empty()) return;
            const bool paused = DenyGamePaused();
            for (auto& [fid, d] : g_denies) {
                if (!Packages::IsRetreating(fid)) { acts.emplace_back(fid, Act::Release); continue; }
                if (d.ended) continue;
                if (api->IsClaimLive(d.handle)) {
                    if (!d.everLive) {
                        d.everLive = true;
                        acts.emplace_back(fid, Act::Stop);
                    }
                    continue;
                }
                if (d.everLive) {
                    spdlog::info("[retreat] {:08X}: ch.22 re-entry deny ENDED BY HARBINGER (h={}; window elapsed "
                                 "or owner dead -- APMF's '[ch.22] deny ended' line names it). A later re-entry "
                                 "gets the per-re-entry StopCombat again.", fid, d.handle);
                    api->Release(d.handle);   // a no-op on a claim Harbinger already released
                    d.handle = APMF_API::kInvalidHandle;
                    d.ended  = true;
                    continue;
                }
                if (!paused) ++d.unpausedSweeps;
                if (d.unpausedSweeps <= kDenyNeverLiveSweeps) continue;
                // PRINCIPLE 7: a pending that never turned live is a FAILURE, said loudly. The
                // retreat then gets the shipped disengage (one StopCombat now), exactly what it
                // would have had with no ch.22 at all.
                spdlog::warn("[retreat] {:08X}: ch.22 re-entry deny NEVER WENT LIVE (h={}, {} unpaused sweeps) -- "
                             "released; posting the retreat's StopCombat WITHOUT the deny (the no-ch.22 road). "
                             "APMF's log should say why the claim was not applied.",
                             fid, d.handle, d.unpausedSweeps);
                api->Release(d.handle);       // FIFO-cancels it if it is still queued
                d.handle = APMF_API::kInvalidHandle;
                d.ended  = true;
                acts.emplace_back(fid, Act::StopNeverLive);
            }
        }
        for (const auto& [fid, act] : acts) {
            switch (act) {
                case Act::Stop:
                    spdlog::info("[retreat] {:08X}: ch.22 re-entry deny LIVE -- posting the single StopCombat", fid);
                    Packages::RetreatReengage(fid, "engage (ch.22 deny live)");
                    break;
                case Act::StopNeverLive:
                    Packages::RetreatReengage(fid, "engage (ch.22 never live)");
                    break;
                case Act::Release:
                    ReleaseRetreatReentryDeny(fid, "retreat ended outside the driver");
                    break;
            }
        }
    }

    // ClearTransientState's teardown (declared in APMFBridge_internal.h). Harbinger drops
    // every claim at its own kPreLoadGame (never saved); the Release here is the harmless
    // stale-handle one the sibling tables make.
    void ClearReentryDenies() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_denyMx);
        for (auto& [fid, d] : g_denies)
            if (api && d.handle != APMF_API::kInvalidHandle) api->Release(d.handle);
        g_denies.clear();
    }

}
