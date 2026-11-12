// apmf/HideClaims.cpp -- the Harbinger claims behind the G2 Layer 1 HOLD ROW (class plan card G2, Docs/research/2026-11/
// g2-hide-design.md). A new claim family, so its own file in apmf/, shaped like apmf/SneakClaims.cpp and ErrandClaims.cpp.
//
// WHAT MFO DECLARES. While the IAUS picks the hold row (he sneaks and no listed foe's group detects him, no blow from
// stealth ready) he stays still and quiet. marth 2026-11-11: engine effects go through Harbinger. Two claims, filed together:
//   * ch.1 kIntent_MovementBlock (no param): the full stand-still.
//   * ch.7 kIntent_CombatAction, ival = kCombatActionCat_Offense | kCombatActionCat_Cast: the combat behaviour tree may not
//     draw an attack or a cast leaf. NOT APMFBridge::ClaimCombatActionDeny (Excursion.cpp): that one is loot-gated and its slot
//     belongs to the excursion.
// HIS OWN CAST (2026-11-11, with Harbinger ABI v40 "Offense means hostile"). G2 keeps a SILENT caster's cast candidates
// while he holds (iaus/Generate.cpp holdQuiet: marth, "There is a perk for hidden casting"), and MFO's casts are delivered
// by Harbinger through the AI's own cast leaves over several laps. A lap that picks the hold row again while that claim is
// still being delivered would file Offense | Cast over it and force-fail the leaf: the cast MFO itself declared would never
// land (the same shape as the G3 flee's heals). So while MFO holds a live cast claim for him (offense or heal,
// IsOwnedCastActive / IsHealCastActive) the hold's ch.7 deny STANDS DOWN and only ch.1 is held; it is filed again on the
// first hold lap after the claim is gone. What the hold denies is therefore: every attack leaf and every cast the ENGINE
// would choose by itself (loud or not), never a cast MFO claimed. Offense alone is not used here: under v40 it would let
// the engine's own beneficial casts through, and a cast is noise unless MFO measured it silent.
// Whether ch.1 parks a follower who is IN COMBAT (the P1 moving= bits) is UNOBSERVED (principle 5); the field run reads it.
// If it does not, the documented next step is ch.23 kIntent_PursuitLeash anchored on him (card G2). Not built here.
//
// LIFETIME. IausSource calls ServiceHideHold(follower, want) once per lap, after the pick: want = true only when the picked
// step is the hold row, false on every other lap (so the ch.7 deny never eats the blow from stealth, a reveal or another
// family winning). A claim that Harbinger ended (IsClaimLive false after it was live, or never live past the publish grace)
// is re-filed on the next want. Every other end is in the sweep (Tick, every pump): his combat ended, he is not sneaking, G1's
// stance claim is gone (without bSneakInCombat), gone / dead / unloaded, or the hold row was not asked for within kStaleSweeps
// (the IAUS did not run for him: another rule won above it, so a movement block must not outlive its decision). Dismissal / MFO
// off ReleaseHideHold (Followers), a load ClearHideClaims (ClearTransientState; NOTHING IS SAVED, Harbinger drops them).
//
// THREADS. ServiceHideHold, ReleaseHideHold and the sweep run on the pump's AddTask worker. ClearHideClaims runs in the
// drained-pump load window. Own leaf mutex g_hideMx: this file never takes g_mx or g_sneakMx while holding it (the sweep reads
// the sneak claim BEFORE it locks). ClearTransientState takes g_hideMx under g_mx (order g_mx -> g_hideMx, never the reverse).
// APMF's Request / Release / IsClaimLive are thread-safe and never call back into MFO.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf, kOwnBasis
#include "APMF_API.h"
#include "Config.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kHideMinAbi = 6;   // IsClaimLive (v6); RequestEx is v2
        // Sized from the real cadence (principle 9): the round-robin tick refreshes one follower every N x 133 ms (a party of
        // ten is ~1.3 s), so 30 sweeps (~4 s) of silence means the IAUS is not asking for him any more. Sweeps stand still
        // while a menu pauses the game (the scans do not run either).
        constexpr std::uint32_t kStaleSweeps = 30;
        constexpr auto          kPublishGrace = std::chrono::seconds(3);
        constexpr std::uint32_t kHoldCats = APMF_API::kCombatActionCat_Offense | APMF_API::kCombatActionCat_Cast;

        using Clock = std::chrono::steady_clock;

        struct One {
            APMF_API::Handle  h = APMF_API::kInvalidHandle;
            bool              everLive = false;
            Clock::time_point filed{};
        };
        struct Hold {
            One           move;               // ch.1
            One           deny;               // ch.7
            std::uint32_t quietSweeps = 0;    // sweeps since ServiceHideHold(want = true) last ran
            bool          refusedLogged = false;
            bool          denyStoodDown = false;   // the ch.7 deny is released while MFO's own cast claim is being delivered
        };
        std::mutex                              g_hideMx;
        std::unordered_map<RE::FormID, Hold>    g_holds;   // guarded by g_hideMx
        std::atomic<bool>                       g_absentLogged{ false };

        const APMF_API::APMF_API_v2* HideApi() {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            return (api && api->abiVersion >= kHideMinAbi) ? api : nullptr;
        }

        // g_hideMx HELD. True = Harbinger no longer holds this handle.
        bool Dead(const APMF_API::APMF_API_v6* a_api, One& a_o) {
            if (a_o.h == APMF_API::kInvalidHandle) return false;
            if (a_api->IsClaimLive(a_o.h)) { a_o.everLive = true; return false; }
            return a_o.everLive || Clock::now() - a_o.filed > kPublishGrace;
        }

        // g_hideMx HELD. Files a_o if it holds nothing live. False = Harbinger refused.
        bool EnsureLocked(const APMF_API::APMF_API_v2* a_api, RE::FormID a_fid, One& a_o, APMF_API::Intent a_intent,
                          std::int32_t a_ival, const char* a_name) {
            if (Dead(reinterpret_cast<const APMF_API::APMF_API_v6*>(a_api), a_o)) {
                spdlog::info("[hide] {:08X}: the {} claim ended (Harbinger released it) -- filing a fresh one", a_fid, a_name);
                a_api->Release(a_o.h);   // a never-live handle may only be pending: release it so it cannot apply later, orphaned (no-op if dead)
                a_o.h = APMF_API::kInvalidHandle;
            }
            if (a_o.h != APMF_API::kInvalidHandle) return true;
            APMF_API::APMF_Param p{};
            p.ival = a_ival;
            a_o.everLive = false;
            a_o.filed    = Clock::now();
            a_o.h        = a_api->RequestEx(a_fid, a_intent, kOwnBasis, &p);
            return a_o.h != APMF_API::kInvalidHandle;
        }

        // g_hideMx HELD. Releases both (the entry stays until the caller erases it).
        void ReleaseLocked(const APMF_API::APMF_API_v2* a_api, RE::FormID a_fid, Hold& a_h, const char* a_why) {
            const bool any = a_h.move.h != APMF_API::kInvalidHandle || a_h.deny.h != APMF_API::kInvalidHandle;
            for (One* o : { &a_h.deny, &a_h.move }) {
                if (a_api && o->h != APMF_API::kInvalidHandle) a_api->Release(o->h);
                *o = One{};
            }
            if (any) spdlog::info("[hide] {:08X}: ch.1 movement block + ch.7 offense/cast deny RELEASED ({})", a_fid, a_why);
        }
    }

    bool HideHoldSupported() {
        if (HideApi()) return true;
        if (!g_absentLogged.exchange(true))
            spdlog::warn("[hide] Harbinger (ABI >= {}) is absent: the G2 hold row is NOT generated (no MFO-only stand-in for the "
                         "stand-still; legacy = absent only)", kHideMinAbi);
        return false;
    }

    void ServiceHideHold(RE::Actor* a_follower, bool a_want) {
        if (!a_follower) return;
        const RE::FormID fid = a_follower->GetFormID();
        const auto* api = HideApi();
        // Read BEFORE g_hideMx (these take g_mx; this file never takes g_mx under its own lock).
        const bool ownCast = a_want && (IsOwnedCastActive(fid) || IsHealCastActive(fid));
        std::scoped_lock lk(g_hideMx);
        if (!a_want) {
            const auto it = g_holds.find(fid);
            if (it == g_holds.end()) return;
            ReleaseLocked(api, fid, it->second, "the hold row is not the pick this lap");
            g_holds.erase(it);
            return;
        }
        if (!api) return;
        auto& h = g_holds[fid];
        h.quietSweeps = 0;
        const bool hadMove = h.move.h != APMF_API::kInvalidHandle, hadDeny = h.deny.h != APMF_API::kInvalidHandle;
        const bool okMove = EnsureLocked(api, fid, h.move, APMF_API::kIntent_MovementBlock, 0, "ch.1 movement block");
        if (ownCast) {
            // His own claimed cast is being delivered through the cast leaves: the ch.7 deny stands down for it.
            if (h.deny.h != APMF_API::kInvalidHandle) {
                api->Release(h.deny.h);
                h.deny = One{};
            }
            if (!h.denyStoodDown) {
                h.denyStoodDown = true;
                spdlog::info("[hide] {:08X}: ch.7 offense/cast deny STANDS DOWN while his own cast claim is delivered (ch.1 "
                             "movement block {})", fid, okMove ? "held" : "REFUSED");
            }
            return;
        }
        if (h.denyStoodDown) {
            h.denyStoodDown = false;
            spdlog::info("[hide] {:08X}: his cast claim is gone: the ch.7 offense/cast deny is filed again", fid);
        }
        const bool okDeny = EnsureLocked(api, fid, h.deny, APMF_API::kIntent_CombatAction, static_cast<std::int32_t>(kHoldCats),
                                         "ch.7 offense/cast deny");
        if (!okMove || !okDeny) {
            if (!h.refusedLogged) {
                h.refusedLogged = true;
                spdlog::warn("[hide] {:08X}: Harbinger REFUSED the hold claim ({}{}; APMF.log names the reason). He holds without "
                             "it.", fid, okMove ? "" : "ch.1 movement block ", okDeny ? "" : "ch.7 deny");
            }
            return;
        }
        h.refusedLogged = false;
        if (!hadMove || !hadDeny)
            spdlog::info("[hide] {:08X}: ch.1 movement block (h={}) + ch.7 offense/cast deny (h={}) FILED", fid, h.move.h, h.deny.h);
    }

    void ReleaseHideHold(RE::FormID a_follower, const char* a_why) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lk(g_hideMx);
        const auto it = g_holds.find(a_follower);
        if (it == g_holds.end()) return;
        ReleaseLocked(api, a_follower, it->second, a_why);
        g_holds.erase(it);
    }

    void SweepHideHolds() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GameIsPaused()) return;
        // Snapshot the ids, decide without the lock (the game reads and G1's mutex), then act under it.
        std::vector<RE::FormID> fids;
        {
            std::scoped_lock lk(g_hideMx);
            for (const auto& [fid, h] : g_holds) fids.push_back(fid);
        }
        const bool combatRoad = Config::g_sneakInCombat.load(std::memory_order_relaxed);
        for (const RE::FormID fid : fids) {
            const char* why = nullptr;
            auto* a = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!a || a->IsDead() || !a->Is3DLoaded()) why = "follower gone, dead or unloaded";
            else if (!InCombat(a))                     why = "his combat ended";
            else if (!a->IsSneaking())                 why = "he is not sneaking";
            else if (!combatRoad && !SneakClaimHeld(fid)) why = "G1's stance claim is released";
            std::scoped_lock lk(g_hideMx);
            const auto it = g_holds.find(fid);
            if (it == g_holds.end()) continue;
            if (!why && ++it->second.quietSweeps > kStaleSweeps) why = "the IAUS did not ask for the hold row (another rule won)";
            if (!why) continue;
            ReleaseLocked(api, fid, it->second, why);
            g_holds.erase(it);
        }
    }

    void ClearHideClaims() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lk(g_hideMx);
        for (auto& [fid, h] : g_holds)
            for (One* o : { &h.deny, &h.move })
                if (api && o->h != APMF_API::kInvalidHandle) api->Release(o->h);   // stale after a load: a no-op
        g_holds.clear();
    }

}
