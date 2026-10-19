// apmf/Substrate.cpp -- MFO as the OUT-OF-COMBAT client of Harbinger's ch.27 COMBAT SUBSTRATE (substrate round R3 =
// slice S3; plan _evidence/2026-10-18-designs/SUBSTRATE-TO-OOC-PLAN.md "R3 detail"; Harbinger Docs/FACETS.md "Combat
// substrate"). marth 2026-10-12: "Getting animations for everything is non negotiable." Every ch.8b claim seat is a
// vfunc of a CombatMagicCaster, which the engine builds only inside CombatController::Update, so an out-of-combat
// cast has no animated road. Harbinger forces combat against its own anchor as a SUBSTRATE (CLAUDE.md principle 1,
// "think with portals"), denies every facet it switches on except the cast leaves, and closes it on the claim's
// first fire. MFO's half is this file: the claim PAIR (substrate + cast claim) and its state machine.
//
// THE STATE MACHINE (one pair per follower; each transition logged ONCE per pair, `road=substrate-claim`):
//   OPEN requested  OpenSubstrateCast filed both claims (RequestEx kIntent_CombatSubstrate, then RequestCast).
//   held            GetSubstrateState(follower) == kSubstrateState_Held (Harbinger entered the substrate).
//   cast claimed    IsClaimLive(cast) first answered true (the ch.8b claim is in Harbinger's snapshot).
//   fired           the SpellSink saw the pair's spell (or Harbinger's minted proxy) fire on this follower.
//   closed          the substrate claim is gone (Harbinger's fire close, the claim's end, the TTL, a real fight,
//                   or NOT OPENED), then the cast claim is released (at once once fired or on a real fight, else
//                   after kCloseGrace so a close never cuts a charge in flight, APMF-B93), or MFO's backstop.
//   REFUSED         RequestEx / RequestCast returned no handle: an ERROR, the cast is skipped, never the direct road.
//   NOT OPENED      the substrate claim ended without ever being held or fired: an ERROR (Harbinger's
//                   "[substrate] ... NOT OPENED: <why>" in APMF.log names the reason). Never a fallback.
//
// THREADING. OpenSubstrateCast runs on the AddTask job worker (the OOC table) or on the MAIN thread (the summon's
// main-thread decision, cast/Summon.cpp); NoteSubstrateCastFired on the worker (the SpellSink's queued task);
// SweepSubstrateCasts on the worker (APMFBridge::Tick, the pump body); ClearSubstrateCasts from
// ClearTransientState (kPreLoadGame / kNewGame, the pump drained). One leaf mutex, g_subMx, guards the table. The
// APMF calls made under it are enqueues (RequestEx / RequestCast / Release) or lock-free reads (IsClaimLive,
// GetCastProxy, GetSubstrateState, IsSubstrateAvailable): none of them calls back into MFO. g_mx is never taken
// here, so the order g_mx -> g_subMx (ClearTransientState) cannot invert.
//
// NOTHING IS SAVED (no co-save change): Harbinger forgets every substrate on a load, and so does this table.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr std::uint32_t kSubstrateQueryAbi = 27;   // IsSubstrateAvailable (and GetSubstrateState, v26)

        // A just-filed claim is not in Harbinger's published snapshot until its next per-frame Drain, and
        // IsClaimLive scans only that snapshot (kEquipAuthValidateAfter's note, APMFBridge_internal.h). So a handle
        // that has NEVER read live is not called gone before this floor: the substrate opens on the next frame and
        // enters one frame later (FACETS "Worked example"); 2 s covers a stalled frame many times over. A floor,
        // not an expiry (principle 9): it only delays the NOT OPENED verdict.
        constexpr auto kNeverLiveFloor = kEquipAuthValidateAfter;

        // APMF-B93: Harbinger's fire close may run on NotifyStartCast, before the spell leaves the hand. MFO does
        // not add a second cut: once the substrate is gone the cast claim is kept until the fire is SEEN (the
        // SpellSink) or this grace passes, then released. Sized from the measured claim fire -> TESSpellCastEvent
        // lag (FIRED -> MFO [cast] line 0.33..2.02 s, APMFBridge.h kHoldLastSeenCapMs's note): 2.5 s clears 2.02 s.
        // Bounded either way: the cast claim's own TTL (kHealCastTtlMs) ends it on Harbinger's side.
        constexpr auto kCloseGrace = std::chrono::milliseconds(2500);

        // MFO's backstop for a pair Harbinger never closed: the substrate TTL is clamped to 15 s on Harbinger's
        // side (FACETS Table 27a), so 17 s never cuts a substrate Harbinger still owns. Logged as an ERROR: a pair
        // living this long means a close MFO did not see (never a silent release).
        constexpr auto kBackstop = std::chrono::seconds(17);

        struct Pair {
            APMF_API::Handle sub  = APMF_API::kInvalidHandle;
            APMF_API::Handle cast = APMF_API::kInvalidHandle;
            RE::FormID       spell  = 0;
            RE::FormID       target = 0;
            RE::FormID       proxy  = 0;          // Harbinger's minted delivery-flip proxy (GetCastProxy), 0 = none
            const char*      site   = "?";        // a string literal from the caller (cast/SubstrateRoad.cpp)
            Clock::time_point filed{};
            Clock::time_point closedAt{};        // when the substrate claim was seen gone (closing)
            bool subEverLive = false;
            bool held        = false;
            bool claimed     = false;
            bool fired       = false;
            bool real        = false;            // the real-combat trigger fired (kSubstrateState_Real)
            bool closing     = false;
            const char* closeReason = nullptr;
        };

        std::mutex                           g_subMx;
        std::unordered_map<RE::FormID, Pair> g_pairs;   // guarded by g_subMx

        std::atomic<bool> g_tooOldNoted{ false };
        std::atomic<bool> g_unavailableNoted{ false };

        const APMF_API::APMF_API_v27* SubApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kSubstrateQueryAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v27*>(api);   // v27 >= v26 >= v6 >= v5: every slot used here
        }

        long long MsSince(Clock::time_point a_t, Clock::time_point a_now) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(a_now - a_t).count();
        }

        const char* SpellName(RE::FormID a_spell) {
            auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spell);
            return sp && sp->GetName() && *sp->GetName() ? sp->GetName() : "?";
        }

        // Release both handles of a pair. Caller holds g_subMx.
        void ReleasePairLocked(const APMF_API::APMF_API_v27* a_api, Pair& a_p) {
            if (a_api) {
                if (a_p.cast != APMF_API::kInvalidHandle) a_api->Release(a_p.cast);
                if (a_p.sub  != APMF_API::kInvalidHandle) a_api->Release(a_p.sub);
            }
            a_p.cast = a_p.sub = APMF_API::kInvalidHandle;
        }
    }

    SubstrateAvail SubstrateAvailability() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return SubstrateAvail::Absent;
        if (api->abiVersion < kSubstrateQueryAbi) {
            if (!g_tooOldNoted.exchange(true))
                spdlog::warn("[substrate] bSubstrateCasts is ON but Harbinger is ABI v{} (the availability query is "
                             "v27): out-of-combat casts stay on today's direct road this session (declared degrade: "
                             "update Harbinger)", api->abiVersion);
            return SubstrateAvail::TooOld;
        }
        const auto* v27 = reinterpret_cast<const APMF_API::APMF_API_v27*>(api);
        if (!v27->IsSubstrateAvailable || !v27->IsSubstrateAvailable()) {
            // marth 2026-10-19 (SUBSTRATE-RULINGS Q4): "Shouldnt happen". The substrate must be available wherever
            // Harbinger is. A loud bug, once per session, and today's road (the declared degrade, never silent).
            if (!g_unavailableNoted.exchange(true))
                spdlog::error("[substrate] THIS SHOULD NOT HAPPEN: bSubstrateCasts is ON and Harbinger (ABI v{}) is "
                              "present, but IsSubstrateAvailable() is FALSE (APMF.log '[substrate] combat substrate "
                              "NOT installed' names why: APMF.ini [CombatSubstrate] bCombatSubstrate, the runtime, a "
                              "seat or an address row). Out-of-combat casts stay on today's direct road (unanimated) "
                              "this session. Report it.", api->abiVersion);
            return SubstrateAvail::Unavailable;
        }
        return SubstrateAvail::Available;
    }

    bool SubstrateCastPending(RE::FormID a_follower) {
        std::scoped_lock lock(g_subMx);
        return g_pairs.find(a_follower) != g_pairs.end();
    }

    bool OpenSubstrateCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target, const char* a_site) {
        const auto* api = SubApi();
        if (!api || a_follower == 0 || a_follower == 0x14 || a_spell == 0) return false;   // Harbinger refuses the player
        if (!api->IsSubstrateAvailable || !api->IsSubstrateAvailable()) return false;      // the caller asked first
        const char* site = a_site ? a_site : "?";
        std::scoped_lock lock(g_subMx);
        if (g_pairs.find(a_follower) != g_pairs.end()) return false;   // the latch (the caller asked first; a race)

        // 1. THE SUBSTRATE, filed first ("file it beside your kIntent_Cast claim", FACETS Example 29). fval 0 =
        //    Harbinger's INI TTL ([CombatSubstrate] fSubstrateTtlSec, 6 s; clamped to 15 s).
        APMF_API::APMF_Param p{};
        p.fval = 0.0f;
        const APMF_API::Handle sub = api->RequestEx(a_follower, APMF_API::kIntent_CombatSubstrate, kOwnBasis, &p);
        if (sub == APMF_API::kInvalidHandle) {
            spdlog::error("[substrate] {:08X} OPEN REFUSED at the call (RequestEx kIntent_CombatSubstrate returned no "
                          "handle; APMF.log names why) -- {} ({:08X}) at {:08X} ({}) is NOT cast this lap, never the "
                          "direct road road=substrate-claim", a_follower, SpellName(a_spell), a_spell, a_target, site);
            return false;
        }
        // 2. THE CAST CLAIM: the spell, the recipient, the LEFT hand (MFO's heal / buff claim convention, BuffClaim and
        //    ClaimHealCast). Fire-and-forget only (concentration waits for slice S5). Harbinger's own line of sight
        //    for a recipient that is not the caster (kCastFlag_OwnLineOfSight, as EnsureCastClaimLocked asks it).
        APMF_API::APMF_CastRequest req{};
        req.spell  = a_spell;
        req.proxy  = 0;   // Harbinger mints its own delivery-flip proxy where the shape needs one
        req.target = a_target;
        req.flags  = APMF_API::kCastFlag_LeftHand;
        if (a_target != 0 && a_target != a_follower && LosSupported())
            req.flags |= APMF_API::kCastFlag_OwnLineOfSight;
        req.ttlMs  = kHealCastTtlMs;
        const APMF_API::Handle cast = api->RequestCast(a_follower, kOwnBasis, &req);
        if (cast == APMF_API::kInvalidHandle) {
            api->Release(sub);
            spdlog::error("[substrate] {:08X} OPEN REFUSED at the call (RequestCast returned no handle; the substrate "
                          "claim is released) -- {} ({:08X}) at {:08X} ({}) is NOT cast this lap, never the direct "
                          "road road=substrate-claim", a_follower, SpellName(a_spell), a_spell, a_target, site);
            return false;
        }
        Pair pr;
        pr.sub = sub; pr.cast = cast; pr.spell = a_spell; pr.target = a_target; pr.site = site;
        pr.filed = Clock::now();
        g_pairs[a_follower] = pr;
        spdlog::info("[substrate] {:08X} OPEN requested: {} ({:08X}) at {:08X}, left hand, site {} (substrate handle "
                     "{}, cast handle {}) road=substrate-claim", a_follower, SpellName(a_spell), a_spell, a_target,
                     site, sub, cast);
        return true;
    }

    void NoteSubstrateCastFired(RE::FormID a_follower, RE::FormID a_spell) {
        std::scoped_lock lock(g_subMx);
        const auto it = g_pairs.find(a_follower);
        if (it == g_pairs.end()) return;
        auto& pr = it->second;
        if (pr.fired) return;
        if (a_spell != pr.spell && (pr.proxy == 0 || a_spell != pr.proxy)) return;
        pr.fired = true;
        spdlog::info("[substrate] {:08X} FIRED: {} ({:08X}) {} ms after the open{} road=substrate-claim",
                     a_follower, SpellName(pr.spell), a_spell, MsSince(pr.filed, Clock::now()),
                     pr.held ? "" : " (never seen held: the sweep missed the window)");
    }

    void ReleaseSubstrateCast(RE::FormID a_follower, const char* a_why) {
        std::scoped_lock lock(g_subMx);
        const auto it = g_pairs.find(a_follower);
        if (it == g_pairs.end()) return;
        ReleasePairLocked(SubApi(), it->second);
        spdlog::info("[substrate] {:08X} CLOSED reason=released ({}) after {} ms road=substrate-claim", a_follower,
                     a_why ? a_why : "?", MsSince(it->second.filed, Clock::now()));
        g_pairs.erase(it);
    }

    void SweepSubstrateCasts() {
        std::scoped_lock lock(g_subMx);
        if (g_pairs.empty()) return;
        const auto* api = SubApi();
        const auto  now = Clock::now();
        for (auto it = g_pairs.begin(); it != g_pairs.end();) {
            const RE::FormID fid = it->first;
            auto& pr = it->second;
            if (!api) {   // Harbinger gone mid-session (cannot happen in practice): nothing to ask, forget the pair
                it = g_pairs.erase(it);
                continue;
            }
            const auto age = now - pr.filed;
            const bool subLive  = pr.sub  != APMF_API::kInvalidHandle && api->IsClaimLive(pr.sub);
            const bool castLive = pr.cast != APMF_API::kInvalidHandle && api->IsClaimLive(pr.cast);
            if (subLive) pr.subEverLive = true;
            if (castLive && !pr.claimed) {
                pr.claimed = true;
                pr.proxy   = api->GetCastProxy ? api->GetCastProxy(pr.cast) : 0;
                spdlog::info("[substrate] {:08X} CAST CLAIMED: {} ({:08X}) at {:08X}, proxy {:08X}, {} ms after the "
                             "open road=substrate-claim", fid, SpellName(pr.spell), pr.spell, pr.target, pr.proxy,
                             MsSince(pr.filed, now));
            }
            const std::uint32_t st = api->GetSubstrateState ? api->GetSubstrateState(fid)
                                                            : APMF_API::kSubstrateState_None;
            if (st == APMF_API::kSubstrateState_Held && !pr.held && !pr.closing) {
                pr.held = true;
                spdlog::info("[substrate] {:08X} HELD: Harbinger holds the substrate, {} ms after the open "
                             "road=substrate-claim", fid, MsSince(pr.filed, now));
            }
            if (st == APMF_API::kSubstrateState_Real && !pr.real) {
                // A REAL FIGHT during the window: Harbinger drops the substrate and keeps the fight. The OOC cast is
                // moot there and the combat table owns casting from now on, so the cast claim goes at once (no MFO
                // claim of the out-of-combat table may stand beside the combat table's: the MFO-B173 class).
                pr.real = true;
                ReleasePairLocked(api, pr);
                spdlog::info("[substrate] {:08X} CLOSED reason=real-combat (a real fight dropped the substrate; the "
                             "cast claim is released, the combat table owns casting) after {} ms{} "
                             "road=substrate-claim", fid, MsSince(pr.filed, now), pr.fired ? ", fired" : ", not fired");
                it = g_pairs.erase(it);
                continue;
            }

            // The substrate claim is GONE: seen live and now not, or never live past the snapshot floor.
            const bool subGone = (pr.subEverLive && !subLive) || (!pr.subEverLive && age >= kNeverLiveFloor);
            if (subGone && !pr.closing) {
                pr.closing  = true;
                pr.closedAt = now;
                if (pr.sub != APMF_API::kInvalidHandle) { api->Release(pr.sub); pr.sub = APMF_API::kInvalidHandle; }   // a no-op on a gone claim
                if (pr.fired)                    pr.closeReason = "fired";
                else if (!pr.held)               pr.closeReason = "not-opened";
                else if (pr.claimed && !castLive) pr.closeReason = "cast-claim-ended";
                else                             pr.closeReason = "closed-unfired";
                if (!pr.fired && !pr.held) {
                    spdlog::error("[substrate] {:08X} NOT OPENED: the substrate claim ended without ever being held "
                                  "(APMF.log '[substrate] ... NOT OPENED: <why>' names the reason) -- {} ({:08X}) at "
                                  "{:08X} ({}) was NOT cast, never the direct road road=substrate-claim",
                                  fid, SpellName(pr.spell), pr.spell, pr.target, pr.site);
                }
            }
            if (pr.closing && (pr.fired || !castLive || now - pr.closedAt >= kCloseGrace)) {
                ReleasePairLocked(api, pr);
                spdlog::info("[substrate] {:08X} CLOSED reason={} after {} ms: {} ({:08X}) at {:08X} ({}){}{} "
                             "road=substrate-claim", fid, pr.closeReason ? pr.closeReason : "?",
                             MsSince(pr.filed, now), SpellName(pr.spell), pr.spell, pr.target, pr.site,
                             pr.held ? ", was held" : "", pr.claimed ? ", was claimed" : "");
                it = g_pairs.erase(it);
                continue;
            }
            if (age >= kBackstop) {
                ReleasePairLocked(api, pr);
                spdlog::error("[substrate] {:08X} CLOSED reason=timeout: the pair stood {} ms (past Harbinger's 15 s "
                              "TTL clamp) and MFO never saw it close -- both claims released ({} ({:08X}) at {:08X}, "
                              "{}) road=substrate-claim", fid, MsSince(pr.filed, now), SpellName(pr.spell), pr.spell,
                              pr.target, pr.site);
                it = g_pairs.erase(it);
                continue;
            }
            ++it;
        }
    }

    void ClearSubstrateCasts() {
        std::scoped_lock lock(g_subMx);
        const auto* api = SubApi();
        for (auto& [fid, pr] : g_pairs) ReleasePairLocked(api, pr);   // Harbinger wipes its own at kPreLoadGame: harmless
        g_pairs.clear();
    }
}
