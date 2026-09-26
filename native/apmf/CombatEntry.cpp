// apmf/CombatEntry.cpp -- the ch.21 combat-entry claim table (ABI v14,
// kIntent_CombatEntry): MFO's client side of Harbinger's "Starting a fight".
// Its one caller is the engage-on-sight gambit (native/EngageOnSight.cpp,
// ClickUp 86e3errnu). A new claim family, so its own file in apmf/ (CLAUDE.md
// "a new mechanism is a new file in its subsystem"); the shape mirrors the
// ch.20 pin table in apmf/Excursion.cpp. Tick() / ClearTransientState()
// (apmf/Bridge.cpp) reach it through APMFBridge_internal.h.
//
// Contract: APMF_API.h kIntent_CombatEntry; Harbinger Docs/INTEGRATION.md "Starting a
// fight" / "How combat entry ends". One claim per follower. MFO never Repoints it: a new
// target is a NEW request (INTEGRATION.md: "To try again, send a NEW RequestEx. An ended
// handle is dead"), and an ended entry is only ever forgotten here, never re-filed.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"   // g_engageOnSight -- the sweep's kill switch

#include <atomic>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kEntryMinAbi = 14;   // the ABI that first serves intent 21

        // IsClaimLive reads APMF's PUBLISHED map, which a RequestEx reaches only at APMF's
        // next writer Drain (its PlayerCharacter::Update seat -- not ticking under a
        // game-pausing menu). A never-seen-live entry is judged ended only after this many
        // UNPAUSED sweeps (~2 s of running game at the pump's 133 ms). A FLOOR, not an
        // expiry (principle 9): it only delays noticing an end. Same value and reasoning
        // as the ch.20 pin table's kPinNeverLiveSweeps (apmf/Excursion.cpp).
        constexpr std::uint32_t kEntryNeverLiveSweeps = 15;

        struct CombatEntryClaim {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;   // invalid once ENDED
            RE::FormID       target   = 0;
            std::uint32_t    unpausedSweeps = 0;   // sweeps with the game running since filing
            bool             everLive = false;     // seen IsClaimLive at least once
            bool             ended    = false;     // Harbinger ended it (IsClaimLive went false)
        };
        std::unordered_map<RE::FormID, CombatEntryClaim> g_entries;   // guarded by g_mx

        // A synchronous refusal with valid params = the ch.21 seat is not installed (VR,
        // runtime, [CombatEntry] bCombatEntry=0, self-check). Session-stable: the seat
        // installs once at APMF's kDataLoaded, so this is never reset (a load does not
        // install it). Atomic so CombatEntryOffered() reads it without g_mx.
        std::atomic<bool> g_entrySeatRefused{ false };

        bool EntryGamePaused() {
            auto* ui = RE::UI::GetSingleton();   // the same read the pin table / Scheduler make on this worker
            return ui && ui->GameIsPaused();
        }

        // Liveness of one filed entry; marks it ENDED (and says so, once). g_mx HELD.
        bool EntryEndedLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower, CombatEntryClaim& e) {
            if (e.ended) return true;
            if (e.handle == APMF_API::kInvalidHandle) return false;
            // v6 IsClaimLive: this table only ever holds handles from an ABI >= 14 APMF.
            if (reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(e.handle)) {
                e.everLive = true;
                return false;
            }
            if (!e.everLive) {
                if (EntryGamePaused() || e.unpausedSweeps < kEntryNeverLiveSweeps) return false;
            }
            spdlog::info("[engage-on-sight] {:08X}: combat entry against {:08X} ENDED (h={}, {}). Harbinger "
                         "ends it on engine refusal, entry not attempted, combat ended, or a dead / disabled / "
                         "unloaded / unresolvable follower or target; APMF's '[ch.21] entry ended' line names "
                         "the reason. MFO does not ask again for this target while it stays a candidate.",
                         follower, e.target, e.handle, e.everLive ? "was live" : "never seen live");
            // ALWAYS Release before dropping the handle: a no-op on a claim Harbinger already
            // released, and it FIFO-cancels one still pending, so a false "never live"
            // judgement can never leave an orphan claim.
            api->Release(e.handle);
            e.handle = APMF_API::kInvalidHandle;
            e.ended  = true;
            return true;
        }
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). g_mx HELD.
    void SweepCombatEntriesLocked(const APMF_API::APMF_API_v2* api) {
        if (g_entries.empty() || !api || api->abiVersion < kEntryMinAbi) return;
        // KILL SWITCH: bEngageOnSight flipped OFF mid-session forgets every entry.
        // Release STOPS NOTHING (Harbinger's contract), so a fight already begun goes on.
        if (!Config::g_engageOnSight.load()) {
            spdlog::info("[engage-on-sight] bEngageOnSight off -- releasing {} entry claim(s) "
                         "(no fight is stopped)", g_entries.size());
            ClearCombatEntriesLocked();
            return;
        }
        const bool paused = EntryGamePaused();
        for (auto& [fid, e] : g_entries) {
            if (!paused && !e.ended && !e.everLive) ++e.unpausedSweeps;
            EntryEndedLocked(api, fid, e);
        }
    }

    // ClearTransientState's teardown (declared in APMFBridge_internal.h). g_mx HELD.
    void ClearCombatEntriesLocked() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        for (auto& [fid, e] : g_entries)
            if (api && e.handle != APMF_API::kInvalidHandle) api->Release(e.handle);
        g_entries.clear();
    }

    bool CombatEntryOffered() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kEntryMinAbi && !g_entrySeatRefused.load(std::memory_order_relaxed);
    }

    EntryResult RequestCombatEntry(RE::FormID a_follower, RE::FormID a_target, std::uint32_t* a_outHandle) {
        if (a_outHandle) *a_outHandle = APMF_API::kInvalidHandle;
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kEntryMinAbi || g_entrySeatRefused.load(std::memory_order_relaxed))
            return EntryResult::SeatAbsent;
        // APMF refuses these synchronously too; filtering them here keeps a synchronous
        // refusal meaning exactly ONE thing (the seat is not installed). 0x14 = the player.
        if (a_follower == 0 || a_target == 0 || a_target == a_follower || a_follower == 0x14)
            return EntryResult::Invalid;

        std::scoped_lock lock(g_mx);
        if (g_entries.contains(a_follower)) return EntryResult::Standing;

        APMF_API::APMF_Param prm{};
        prm.form = a_target;   // REQUIRED: the target ACTOR. Nothing else is read.
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_CombatEntry, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            if (!g_entrySeatRefused.exchange(true)) {
                spdlog::warn("[engage-on-sight] Harbinger REFUSED the ch.21 combat-entry claim ({:08X} -> "
                             "{:08X}) at the call: the ch.21 seat is not installed (VR, a runtime other than "
                             "1.6.1170 / 1.5.97, [CombatEntry] bCombatEntry=0 in APMF.ini, or its address "
                             "self-check; APMF's log names it). The engage-on-sight gambit is INERT for this "
                             "session. MFO has no direct road.",
                             a_follower, a_target);
            }
            return EntryResult::SeatAbsent;
        }
        CombatEntryClaim e{};
        e.handle = h;
        e.target = a_target;
        g_entries.emplace(a_follower, e);
        if (a_outHandle) *a_outHandle = h;
        return EntryResult::Filed;
    }

    EntryState CombatEntryStateOf(RE::FormID a_follower, RE::FormID* a_outTarget) {
        if (a_outTarget) *a_outTarget = 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_entries.find(a_follower);
        if (it == g_entries.end()) return EntryState::None;
        if (a_outTarget) *a_outTarget = it->second.target;
        return it->second.ended ? EntryState::Ended : EntryState::Standing;
    }

    void ForgetCombatEntry(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mx);
        const auto it = g_entries.find(a_follower);
        if (it == g_entries.end()) return;
        if (api && it->second.handle != APMF_API::kInvalidHandle) api->Release(it->second.handle);
        g_entries.erase(it);
    }

}
