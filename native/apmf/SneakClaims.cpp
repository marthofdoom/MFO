// apmf/SneakClaims.cpp -- the Harbinger ch.3 STANCE claim behind act.sneak (class plan card G1, ClickUp class
// build plan G1). A new claim family, so its own file in apmf/. The table shape mirrors apmf/AimClaims.cpp.
//
// WHAT MFO DECLARES. A rule "<condition> -> Sneak" asks for the follower to crouch and stay crouched while the rule
// stands. MFO files ONE ch.3 kIntent_Stance claim (ABI 1, no param, FACETS ch.3 "Field-proven"): Harbinger sends
// SneakStart if he is not already sneaking and, at Release, SneakStop only if it was the one that started it.
// NOT ch.16 kIntent_Detection: that one rewrites the detection actor values and would erase a thief's own perks.
//
// "WHILE THE RULE STANDS". Every scan lap that reaches the rule with its condition true calls ServiceSneakRule,
// which files the claim if none stands and resets the hold counter. The rule is TRANSPARENT (the scan goes on to
// the rows below), so "Always -> Sneak" sits on top of a thief's table. The pump's sweep (Tick, every 133 ms)
// counts the unpaused sweeps since the last lap. Past kHoldSweeps (sized from the cadence below) the rule has
// stopped standing (a higher opaque rule won, its condition went false, the table stopped running, he was
// dismissed or MFO was turned off for him) and the claim is released.
//
// THE RE-ASSERT IS MFO's. ch.3 does not re-assert: a package or the combat AI that forces a stance can stand him up
// while the claim is live. When the claim has stood kReassertSweeps (Harbinger applies a Request on its next game
// thread drain, and the SneakStart needs its beat) and IsSneaking reads false, MFO releases it and files a new one,
// so a re-file happens at most once per kReassertSweeps.
// Not while he swims or rides (the engine's own IsSneaking is false there by definition, and a SneakStart into
// water is not ours to send).
//
// IN COMBAT IS UNOBSERVED (principle 5). Out of combat ch.3 is field-proven. Whether the combat behaviour tree
// stands an NPC up mid-fight is not, so the in-combat road is the dev switch bSneakInCombat (MFO.ini, default OFF).
// With it off, a follower in combat (APMFBridge::InCombat) gets no claim and a standing one is released at once.
//
// PROBE ([sneak], always on, passive and rate-limited). One line per event: claim FILED, IsSneaking true / false
// (transitions only, while a claim stands), RE-FILE, RELEASED (with the reason). At most kLogBurst lines per follower
// per kLogWindowSec. Past that the lines are counted and one "N lines suppressed" line closes the window. That is
// what a field run reads to prove (or disprove) the in-combat road.
//
// THREADS. ServiceSneakRule and the sweep run on the pump's AddTask worker (Fire, the logistics scan, Tick).
// ClearSneakClaims runs in the drained-pump load window (ClearTransientState). Own mutex g_sneakMx, a leaf: this
// file never takes g_mx, and ClearTransientState takes g_sneakMx under g_mx (order g_mx -> g_sneakMx, never the
// reverse). APMF's Request / Release are thread-safe and never call back into MFO. NOTHING IS SAVED: Harbinger
// drops every claim at a load.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"
#include "Config.h"

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        // Sizes from the real cadence (principle 9). The sweep runs every pump (133 ms). A rule is re-reached once
        // per service lap of its follower: 133 ms x party, ~1.3 s for ten. Hold = 30 sweeps (~4 s, three missed
        // laps at a party of ten). Re-assert = 23 sweeps (~3 s) of claim age: the Request lands on Harbinger's next
        // drain and the SneakStart animation needs its beat before IsSneaking is a fair read, and a re-file resets the
        // age, so this is also the re-file throttle. A refused Request is retried after 75 sweeps (~10 s), never in a
        // loop.
        constexpr std::uint32_t kHoldSweeps     = 30;
        constexpr std::uint32_t kReassertSweeps = 23;
        constexpr std::uint32_t kRetrySweeps    = 75;
        constexpr std::uint32_t kLogBurst       = 12;
        constexpr double        kLogWindowSec   = 30.0;

        using Clock = std::chrono::steady_clock;

        struct SneakClaim {
            APMF_API::Handle handle      = APMF_API::kInvalidHandle;
            std::uint32_t    idleSweeps  = 0;       // unpaused sweeps since the rule was last reached
            std::uint32_t    ageSweeps   = 0;       // unpaused sweeps since this handle was filed
            std::uint32_t    retryIn     = 0;       // > 0: a refused Request, do not re-file yet (sweeps)
            std::uint32_t    refiles     = 0;       // re-files of this hold (for the log)
            bool             wanted      = false;   // the rule stood within kHoldSweeps
            bool             inCombat    = false;   // the last lap that reached the rule was in combat
            int              lastSneak   = -1;      // last IsSneaking logged while claimed (-1 = none yet)
            bool             refusedLogged = false;
            // the rate limiter
            Clock::time_point logWindow{};
            std::uint32_t     logLines   = 0;
            std::uint32_t     logDropped = 0;
        };
        std::mutex                                 g_sneakMx;
        std::unordered_map<RE::FormID, SneakClaim> g_sneaks;   // guarded by g_sneakMx

        // g_sneakMx HELD. True when this follower may log one more line in the current window.
        bool MayLogLocked(RE::FormID a_fid, SneakClaim& a_c) {
            const auto now = Clock::now();
            if (a_c.logWindow.time_since_epoch().count() == 0 ||
                std::chrono::duration<double>(now - a_c.logWindow).count() >= kLogWindowSec) {
                if (a_c.logDropped > 0)
                    spdlog::info("[sneak] {:08X}: {} line(s) suppressed in the last {:.0f} s (rate limit)", a_fid,
                                 a_c.logDropped, kLogWindowSec);
                a_c.logWindow  = now;
                a_c.logLines   = 0;
                a_c.logDropped = 0;
            }
            if (a_c.logLines < kLogBurst) { ++a_c.logLines; return true; }
            ++a_c.logDropped;
            return false;
        }

        // g_sneakMx HELD.
        bool FileLocked(const APMF_API::APMF_API_v2* a_api, RE::FormID a_fid, SneakClaim& a_c, bool a_log) {
            const APMF_API::Handle h = a_api->Request(a_fid, APMF_API::kIntent_Stance, kOwnBasis);
            if (h == APMF_API::kInvalidHandle) {
                a_c.retryIn = kRetrySweeps;
                if (!a_c.refusedLogged) {
                    a_c.refusedLogged = true;
                    spdlog::warn("[sneak] {:08X}: Harbinger REFUSED the ch.3 stance claim at the call (APMF's log names "
                                 "the reason). Retried every ~10 s while the rule stands.", a_fid);
                }
                return false;
            }
            a_c.handle    = h;
            a_c.ageSweeps = 0;
            a_c.lastSneak = -1;
            a_c.refusedLogged = false;
            if (a_log && MayLogLocked(a_fid, a_c))
                spdlog::info("[sneak] {:08X}: ch.3 stance claim FILED (h={}, {})", a_fid, h,
                             a_c.inCombat ? "in combat, bSneakInCombat on" : "out of combat");
            return true;
        }

        // g_sneakMx HELD. Releases the handle (if any). The entry stays so its log window survives a re-file.
        void ReleaseLocked(const APMF_API::APMF_API_v2* a_api, RE::FormID a_fid, SneakClaim& a_c, const char* a_why) {
            if (a_c.handle == APMF_API::kInvalidHandle) return;
            if (a_api) a_api->Release(a_c.handle);
            if (MayLogLocked(a_fid, a_c))
                spdlog::info("[sneak] {:08X}: ch.3 stance claim RELEASED ({}, h={}, re-filed {} time{})", a_fid, a_why,
                             a_c.handle, a_c.refiles, a_c.refiles == 1 ? "" : "s");
            a_c.handle    = APMF_API::kInvalidHandle;
            a_c.ageSweeps = 0;
            a_c.lastSneak = -1;
        }

        bool SneakGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }
    }

    const char* ServiceSneakRule(RE::Actor* a_follower) {
        if (!a_follower) return "no follower";
        const RE::FormID fid = a_follower->GetFormID();
        const bool inCombat = InCombat(a_follower);
        if (inCombat && !Config::g_sneakInCombat.load(std::memory_order_relaxed)) {
            ReleaseSneak(fid, "in combat, bSneakInCombat off");
            return "sneak in combat is off (bSneakInCombat, dev switch)";
        }
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return "sneak needs Harbinger (ch.3)";
        std::scoped_lock lk(g_sneakMx);
        auto& c = g_sneaks[fid];
        c.wanted     = true;
        c.idleSweeps = 0;
        if (c.handle != APMF_API::kInvalidHandle && c.inCombat != inCombat && MayLogLocked(fid, c))
            spdlog::info("[sneak] {:08X}: the held claim crossed {} combat", fid, inCombat ? "INTO" : "OUT OF");
        c.inCombat = inCombat;
        if (c.handle != APMF_API::kInvalidHandle) return nullptr;   // held
        if (c.retryIn > 0) return "Harbinger refused the sneak claim (retrying)";
        c.refiles = 0;
        return FileLocked(api, fid, c, true) ? nullptr : "Harbinger refused the sneak claim";
    }

    void ReleaseSneak(RE::FormID a_follower, const char* a_why) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lk(g_sneakMx);
        const auto it = g_sneaks.find(a_follower);
        if (it == g_sneaks.end()) return;
        ReleaseLocked(api, a_follower, it->second, a_why);
        it->second.wanted = false;
    }

    void SweepSneakClaims() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        if (SneakGamePaused()) return;   // a menu holds every counter still (the scans do not run either)
        const bool combatRoad = Config::g_sneakInCombat.load(std::memory_order_relaxed);
        std::scoped_lock lk(g_sneakMx);
        for (auto it = g_sneaks.begin(); it != g_sneaks.end();) {
            const RE::FormID fid = it->first;
            auto&            c   = it->second;
            if (c.retryIn > 0) --c.retryIn;
            if (c.handle == APMF_API::kInvalidHandle) {
                // Nothing held (released, or a refused Request waiting out its retry): forget the entry once the
                // rule has stopped standing.
                if (!c.wanted || ++c.idleSweeps > kHoldSweeps) { it = g_sneaks.erase(it); continue; }
                ++it;
                continue;
            }
            ++c.ageSweeps;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
            const char* why = nullptr;
            if (++c.idleSweeps > kHoldSweeps)              why = "the rule stopped standing";
            else if (!a || a->IsDead() || !a->Is3DLoaded()) why = "follower gone, dead or unloaded";
            else if (!combatRoad && InCombat(a))            why = "in combat, bSneakInCombat off";
            if (why) {
                ReleaseLocked(api, fid, c, why);
                c.wanted = false;
                ++it;
                continue;
            }
            const bool sneaking = a->IsSneaking();
            if (static_cast<int>(sneaking) != c.lastSneak) {
                if (MayLogLocked(fid, c))
                    spdlog::info("[sneak] {:08X}: IsSneaking {} (claim h={}, {} sweeps old, {})", fid,
                                 sneaking ? "TRUE" : "FALSE", c.handle, c.ageSweeps,
                                 InCombat(a) ? "in combat" : "out of combat");
                c.lastSneak = sneaking ? 1 : 0;
            }
            // THE RE-ASSERT: the claim has had its beat, he is standing, and he is not swimming or riding.
            if (!sneaking && c.ageSweeps >= kReassertSweeps && !a->AsActorState()->IsSwimming() && !a->IsOnMount()) {
                const APMF_API::Handle old = c.handle;
                api->Release(old);   // Release then Request: Harbinger applies both, in order, on its next drain
                c.handle = APMF_API::kInvalidHandle;
                ++c.refiles;
                const bool ok = FileLocked(api, fid, c, false);
                if (MayLogLocked(fid, c))
                    spdlog::info("[sneak] {:08X}: RE-FILE #{} (IsSneaking false under a live claim for {} sweeps, {}): "
                                 "h={} -> {}", fid, c.refiles, kReassertSweeps, InCombat(a) ? "in combat" : "out of combat",
                                 old, ok ? std::to_string(c.handle) : std::string("refused"));
            }
            ++it;
        }
    }

    void ClearSneakClaims() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lk(g_sneakMx);
        for (auto& [fid, c] : g_sneaks)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);   // stale after a load: a no-op
        g_sneaks.clear();
    }

}
