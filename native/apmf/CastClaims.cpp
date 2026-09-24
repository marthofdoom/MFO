// apmf/CastClaims.cpp -- the kIntent_Cast CLAIMS: the per-hand offense-cast
// claim (ClaimOffenseCast, refresh-in-place, per-hand release) and the heal-cast
// claim (ClaimHealCast, RefreshHealCastClaim's never-observed hold cap).
// Split out of the old native/APMFBridge.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The bridge's shared claim state lives in apmf/APMFBridge_internal.h.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // wave-1 split: the bridge's shared claim state
#include "APMF_API.h"
#include "ComposedCast.h"   // ObservedFiring (the idle-hand floor's unobserved gate) + ClearWatchHand
#include "cast/Actuation.h"     // CastInFlightOnHand -- THE one in-flight definition; the idle-hand
                           // floor's gate is armed on CHARGE, not on claim age alone (2026-09-22)
                            // (the expiry sweep drops the swept claim's [cfc] watch) -- both called
                            // from Tick() ONLY, which runs inside the AddTask job-worker body
                            // (Diagnostics.cpp) = ComposedCast's own serialized context; ComposedCast
                            // never takes g_mx, so calling it under g_mx cannot deadlock.
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
#include "MainThread.h"
#include "Packages.h"   // kMaxLootSlots -- the ch.19 loot-travel leg table is keyed by loot SLOT
#include "Rapport.h"

#include <array>   // F10: the two-slot (driving / idle-hand floor) log throttles
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>   // ch.17 claim-refusal throttle
#include <algorithm>       // ch.8 allow-list: sort + unique over the published form set

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        // Left -> slot 0; everything else (kApmfHandRight, bare 0, or an
        // unrecognized value) -> slot 1. Dual is handled separately by its own
        // caller (ClaimOffenseCast mirrors into BOTH slots; a single-slot
        // lookup is meaningless for it, so nothing here maps kApmfHandDualCast
        // specially -- callers never pass it to this helper).
        inline std::size_t OffenseSlot(std::int32_t a_hand) {
            return a_hand == kApmfHandLeft ? 0 : 1;
        }
    }

    bool IsOwnedCastActive(RE::FormID a_follower) {
        // Fast-out before the lock: APMF absent -> never active (mirrors every
        // other accessor's g_apmf check).
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() &&
               (it->second.offense[0].handle != APMF_API::kInvalidHandle ||
                it->second.offense[1].handle != APMF_API::kInvalidHandle);
    }

    bool IsOwnedCastActiveOnHand(RE::FormID a_follower, std::int32_t a_hand) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() &&
               it->second.offense[OffenseSlot(a_hand)].handle != APMF_API::kInvalidHandle;
    }

    RE::FormID GetOffenseCastProxy(RE::FormID a_follower, std::int32_t a_hand) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return 0;
        const auto& c = it->second.offense[OffenseSlot(a_hand)];
        return c.handle != APMF_API::kInvalidHandle ? c.proxy : 0;
    }

    // ── offense-cast (per-cast, TTL-bounded, PER-HAND, ch.8b, kIntent_Cast/RequestCast) ──
    // PORTED feat/offense-cast-seats (2026-09-05) off the retired ch.8
    // kIntent_SelectSpell gate-only claim (ClaimCasting/ReleaseCasting, removed)
    // this call site used to make; REKEYED per-hand (feat/per-hand-cast-slots,
    // 2026-09-06) -- see APMFBridge.h's ClaimOffenseCast doc for the full
    // rationale. Claim plumbing, mirroring ClaimHealCast's shape via the shared
    // EnsureCastClaimLocked helper, just applied to ONE of the two `offense[]`
    // slots (or BOTH, mirrored, for a DualCast ask).
    bool ClaimOffenseCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                         std::int32_t a_hand, bool a_concentration, std::uint32_t a_stopPct) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_apmfCast.load())
            return false;
        // RequestCast is a v5 slot; an APMF.dll built before it (ABI < 5) has no
        // such function pointer to call at all -- degrade cleanly to the legacy
        // AI-first-grace + force-on-miss hybrid rather than reading past the end
        // of an older, shorter interface struct. Logged ONCE (own flag, separate
        // from ClaimHealCast's -- a session may exercise only one of the two
        // paths and both deserve their own visibility).
        if (api->abiVersion < 5) {
            static std::atomic<bool> s_warnedOffense{ false };
            if (!s_warnedOffense.exchange(true))
                spdlog::warn("[apmf] ABI v{} has no RequestCast (need >= 5) -- offense-cast claim "
                             "OFF; the legacy AI-first-grace hybrid runs instead (degrade).", api->abiVersion);
            return false;
        }
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];

        if (a_hand == kApmfHandDualCast) {
            // Defensive: if slot 1 (right) currently holds an INDEPENDENT
            // (non-mirrored) single-hand claim, release it BEFORE mirroring
            // the dual claim over it below -- overwriting a live handle
            // without releasing it would leak an APMF-side claim. Callers are
            // expected to have already verified BOTH hands are free via the
            // per-hand cast-lock juggle (Actuation.cpp's ResolveCastHand)
            // before ever requesting Dual, so this should be a no-op in
            // practice; it exists so a caller mistake corrupts nothing.
            if (o.offense[1].handle != APMF_API::kInvalidHandle &&
                o.offense[1].handle != o.offense[0].handle)
                ReleaseClaimLocked(o.offense[1]);
            // ONE underlying APMF claim (a single RequestCast with
            // kCastFlag_DualCast) occupies BOTH hands -- mirror it into both
            // slots rather than asking APMF to arbitrate the same dual claim
            // twice. Ensure against slot 0 (the "primary"), then copy its
            // result into slot 1 verbatim (same handle -- see ReleaseOffenseCast
            // for the matching dedup-on-release).
            EnsureCastClaimLocked(v5, a_follower, o.offense[0], a_spell, a_target, a_hand,
                                  a_concentration, a_stopPct);
            o.offense[0].refreshed = std::chrono::steady_clock::now();
            o.offense[1] = o.offense[0];
        } else {
            const std::size_t idx   = OffenseSlot(a_hand);
            const std::size_t other = 1 - idx;
            // If the OTHER slot currently mirrors a live DUAL claim (same
            // handle this single-hand ask is about to replace), un-mirror it
            // FIRST -- Ensure below will Release/replace slot `idx`'s copy of
            // that shared handle, and the other slot's copy must not go on
            // pointing at a handle that no longer exists.
            if (o.offense[idx].handle != APMF_API::kInvalidHandle &&
                o.offense[idx].handle == o.offense[other].handle)
                o.offense[other] = CastClaim{};
            EnsureCastClaimLocked(v5, a_follower, o.offense[idx], a_spell, a_target, a_hand,
                                  a_concentration, a_stopPct);
            o.offense[idx].refreshed = std::chrono::steady_clock::now();
        }

        const bool live = a_hand == kApmfHandDualCast
            ? o.offense[0].handle != APMF_API::kInvalidHandle
            : o.offense[OffenseSlot(a_hand)].handle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    // See APMFBridge.h. Kept RIGHT BESIDE ClaimOffenseCast on purpose: this is
    // that function's own non-arbitration early-return set, and nothing else --
    // if one ever gains a condition, the other is one screen away.
    bool OffenseCastClaimSupported() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= 5 && Config::g_apmfCast.load();
    }

    // ── REFRESH A STANDING CAST CLAIM IN PLACE (F9, 2026-09-08) ─────────────────
    // See APMFBridge.h for the contract. The implementation is deliberately a
    // REPLAY of the claim's OWN stored tuple rather than a second refresh path:
    // handing EnsureCastClaimLocked exactly what it already stored guarantees the
    // identity compare MATCHES, so this can never change a hand mode, never
    // re-point a target, and never tear down a claim that is still in force -- the
    // things F8 exists to stop. Everything the fast path does (the ABI-6 liveness
    // check, the lazy proxy read, the TTL heartbeat, the never-published
    // fail-closed split) happens here too, for free and identically, because it is
    // the same code.
    //
    // "THE FAST PATH IS THE ONLY PATH" WOULD BE FALSE, so it is not claimed
    // (review fix, 2026-09-08). The identity compare matching only decides that the
    // request is UNCHANGED; what happens next still depends on APMF. If the stored
    // handle reads NOT live and had once been live, the aged-out branch runs and
    // RE-REQUESTS -- a genuinely fresh handle is minted from here. That is correct
    // and is the point (a claim APMF expired should come back while its rule still
    // wants it), and it is NOT a concurrent second claim: the dead handle is
    // dropped first, exactly one claim exists at every instant, and no charge is
    // interrupted because there was no live claim left to interrupt. The two
    // outcomes a caller must distinguish stay as documented: `true` = a live claim
    // stands on that hand afterwards (renewed or re-minted), `false` = APMF refused
    // it and nothing stands.
    //
    // IT CAN NOW BE CALLED FROM A HOLD, AND THAT CHANGED THE RELATIONSHIP WITH
    // RefreshHealCastClaim (corrected 2026-09-08 -- the paragraph here used to say
    // the two could not collide, which was true only while the in-flight match was
    // this function's only caller). RefreshHealCastClaim refuses to Repoint on
    // purpose, because it feeds a claim whose own rule may have gone SILENT, and
    // it additionally caps a NEVER-OBSERVED claim at kHealHoldNeverObservedMs --
    // both because an incumbent the engine never casts can otherwise re-arm
    // forever. Actuation's cast-hand lock now also calls this while an incumbent's
    // own rule is held off from re-aiming, and its LEFT branch replays `o.heal`,
    // so an unbounded caller here would renew exactly the claim that cap exists to
    // bound. THE CAP THEREFORE LIVES AT THAT CALL SITE: the hold heartbeats only
    // while the claim has been OBSERVED firing or is younger than the same
    // kHealHoldNeverObservedMs window. Nothing here re-derives it -- but do not
    // add a second unbounded caller without one.
    //
    // IT CAN MOVE `created` -- rarely, and the paragraph that used to stand here
    // said it could not ("stamped once per handle in the mint block, which this
    // cannot reach"). It CAN reach it: a stored handle APMF has already expired
    // takes EnsureCastClaimLocked's aged-out branch, which resets the claim and
    // RequestCasts a fresh one, and the mint block stamps `created` on that new
    // handle. Practically unreachable while MFO's own ~2.45 s FacetExpiry sweep
    // beats APMF's 6 s TTL -- the claim is released here long before it can age
    // out there -- but "cannot" was the wrong word, and the never-observed hold
    // cap that reads `created` deserves the accurate version.
    //
    // `a_holdSpell` (2026-09-08): non-zero marks this a HOLD refresh -- see the
    // header -- AND names the ONE spell being held for, so only claims naming it
    // are replayed. That second half is load-bearing on the LEFT hand, where an
    // offense claim and a heal claim can stand together: without it a held
    // offense re-aim would renew a coexisting HEAL claim it knows nothing about,
    // and the heal's own never-observed cap could not stop it (that cap gates
    // RefreshHealCastClaim's answer and reads `created`, while the claim's LIFE
    // is `refreshed`, which this call bumps).
    bool RefreshOwnedCastOnHand(RE::FormID a_follower, std::int32_t a_hand,
                                RE::FormID a_holdSpell) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || api->abiVersion < 5) return false;
        // ABI < 6 PARITY WITH RefreshHealCastClaim (review, 2026-09-08). On a HOLD
        // refresh only. Below ABI 6 there is no IsClaimLive to ask, so
        // EnsureCastClaimLocked's fast path ASSUMES a stored handle is live and its
        // TTL heartbeat is compiled out of reach -- meaning a hold refresh there
        // would bump MFO's own `refreshed` stamp, and nothing else, for a claim
        // APMF may have expired long ago. Tick()'s sweep is then the only thing
        // that could ever end the hold, and this call is precisely what stops it
        // firing: a hold nobody can break (#7). RefreshHealCastClaim refuses on the
        // same ABI for the same reason; refusing here degrades honestly to the
        // pre-hold behaviour (the sweep releases, the rule re-aims) instead.
        // The IN-FLIGHT caller is unaffected and stays byte-identical: it is fed by
        // a rule whose (spell,target) still matches, so nothing is being held open
        // on its behalf. Inert in practice -- the DLL pair ships at kABIVersion 6.
        if (a_holdSpell != 0 && api->abiVersion < 6) return false;
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return false;
        auto& o = it->second;
        const auto now = std::chrono::steady_clock::now();
        bool any = false;

        auto replay = [&](CastClaim& c) {
            if (c.handle == APMF_API::kInvalidHandle) return;
            // A hold names its spell; anything else on this hand is not its to feed.
            if (a_holdSpell != 0 && c.spell != a_holdSpell) return;
            EnsureCastClaimLocked(v5, a_follower, c, c.spell, c.target, c.hand,
                                  c.conc, c.stopPct, c.denyOnly);
            c.refreshed = now;
            if (c.handle != APMF_API::kInvalidHandle) any = true;
        };
        // A mirrored DualCast claim is ONE handle living in BOTH slots, and BOTH
        // copies' `refreshed` stamps have to move together: Tick()'s sweep reads
        // each slot independently, so a stale mirror would release the shared
        // handle out from under the live slot (and then clear it, via that
        // sweep's own dual dedup). Re-mirror after the replay, exactly as
        // ClaimOffenseCast does.
        auto replayOffense = [&](std::size_t idx) {
            auto& c = o.offense[idx];
            if (c.handle == APMF_API::kInvalidHandle) return;
            const std::size_t other = 1 - idx;
            const bool sharedDual = o.offense[other].handle == c.handle;
            replay(c);
            if (sharedDual) o.offense[other] = c;
        };

        if (a_hand == kApmfHandDualCast) {
            replayOffense(0);
            // "Both hands" is USUALLY one mirrored dual claim, which replayOffense
            // has already re-mirrored -- but the caller names HANDS, not claims,
            // and two INDEPENDENT single-hand claims can also occupy both. Refresh
            // the second one too when it is a genuinely different handle, or its
            // `refreshed` stamp would stand still and Tick()'s sweep would release
            // a claim this call was asked to keep alive.
            if (o.offense[1].handle != APMF_API::kInvalidHandle &&
                o.offense[1].handle != o.offense[0].handle)
                replayOffense(1);
        } else if (a_hand == kApmfHandLeft) {
            replayOffense(0);
            // Heals are LEFT always (ClaimHealCast's hard rule), and a heal claim
            // and an offense claim CAN both stand on the left hand at once, so
            // both are refreshed -- the caller named the hand, not the facet.
            replay(o.heal);
        } else {
            replayOffense(1);
        }

        EraseIfEmpty(g_owned.find(a_follower));
        return any;
    }

    // ── PER-HAND CAST-CLAIM RELEASE (rank preemption, marth 2026-09-08) ─────────
    // See APMFBridge.h. Exists because ReleaseOffenseCast is whole-follower by
    // design (its two callers both mean "nothing wanted on either hand anymore")
    // and rank preemption means exactly the opposite: a higher-ranked rule takes
    // ONE hand, and the other hand's independent claim must not even notice.
    //
    // A MIRRORED DUALCAST CLAIM IS ONE CLAIM ON TWO HANDS, so releasing "its left
    // hand" releases the whole thing and clears both slots. That is the honest
    // semantics, not a shortcut: there is one APMF handle, APMF arbitrates it as
    // one claim occupying both hands, and there is no half-release to make. A
    // caller preempting one hand of a dual cast is ending that dual cast.
    //
    // THE OFFENSE SLOT ONLY -- THE HEAL SLOT IS NOT THIS FUNCTION'S TO TOUCH
    // (review finding, 2026-09-08). It used to release `heal` too whenever the hand
    // was LEFT, which was wrong twice over: it was UNCONDITIONAL (an offense claim
    // being displaced from the left dragged an unrelated coexisting heal claim down
    // with it), and it went behind ComposedCast's back -- the heal claim is
    // bookkept THERE (its [cfc] watch slot, its CastBounds arm, its hold record),
    // and dropping the APMF claim alone leaves all three armed for a claim that no
    // longer exists. `ComposedCast::End` is the seam that takes them down together,
    // and the caller routes a heal-backed release through it.
    void ReleaseCastClaimOnHand(RE::FormID a_follower, std::int32_t a_hand) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        auto& o = it->second;
        const std::size_t idx   = OffenseSlot(a_hand);
        const std::size_t other = 1 - idx;
        if (o.offense[idx].handle != APMF_API::kInvalidHandle) {
            const bool sharedDual = o.offense[other].handle == o.offense[idx].handle;
            ReleaseClaimLocked(o.offense[idx]);
            if (sharedDual) o.offense[other] = CastClaim{};   // one handle, released once
        }
        EraseIfEmpty(it);
    }

    void ReleaseOffenseCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        auto& o = it->second;
        // A mirrored DualCast claim shares ONE handle across both slots --
        // release it exactly once (Release on an already-cleared/invalid
        // handle would be a no-op anyway, but this avoids depending on that
        // and keeps the intent explicit: one claim, one Release call).
        const bool sharedDual = o.offense[0].handle != APMF_API::kInvalidHandle &&
                                 o.offense[0].handle == o.offense[1].handle;
        ReleaseClaimLocked(o.offense[0]);
        if (sharedDual) o.offense[1] = CastClaim{};
        else            ReleaseClaimLocked(o.offense[1]);
        EraseIfEmpty(it);
    }

    // ── heal-cast (per-cast, TTL-bounded, ch.8b, kIntent_Cast/RequestCast) ──────
    // Ported feat/mfo-cast-port (2026-09-05): APMF's feat/ai-cast-seats-impl
    // retired the kIntent_SelectSpell +ACT drive this used to ride (`ival`'s
    // hand-mode bits and kActFlag_Drive opt-in are now accepted-and-ignored on
    // that channel -- APMF_API.h) and replaced it with five engine vfunc seats
    // answered while a kIntent_Cast claim stands, so the NPC's OWN AI drives the
    // animated cast natively. See APMFBridge.h's doc comment above for the full
    // shape; this is just the claim plumbing.
    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand, bool a_concentration, std::uint32_t a_stopPct) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        // Executor toggle = the (repurposed) bHealAnimPackage key. APMF absent /
        // toggle off / no spell -> OFF, degrade to kInstant.
        if (!api || a_follower == 0 || a_spell == 0 || !Config::g_healAnimPackage.load())
            return false;
        // RequestCast is a v5 slot; an APMF.dll built before it (ABI < 5) has no
        // such function pointer to call at all -- degrade cleanly to kInstant
        // rather than reading past the end of an older, shorter interface struct
        // (the SAME guard shape as SetSpellAllowList's `>= 4` check above).
        // Logged ONCE (not every tick) so a stale APMF.dll is legible without
        // spamming the log every heal attempt.
        if (api->abiVersion < 5) {
            static std::atomic<bool> s_warned{ false };
            if (!s_warned.exchange(true))
                spdlog::warn("[apmf] ABI v{} has no RequestCast (need >= 5) -- heal-cast claim "
                             "OFF; heals apply via kInstant (degrade).", api->abiVersion);
            return false;
        }
        auto* v5 = reinterpret_cast<const APMF_API::APMF_API_v5*>(api);
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureCastClaimLocked(v5, a_follower, o.heal, a_spell, a_target, a_hand,
                              a_concentration, a_stopPct);
        o.heal.refreshed = std::chrono::steady_clock::now();
        const bool live = o.heal.handle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    // See APMFBridge.h. The heal twin of OffenseCastClaimSupported, kept beside
    // ClaimHealCast for the same reason -- it mirrors THAT function's own
    // non-arbitration early returns (toggle = bHealAnimPackage, not bApmfCast).
    bool HealCastClaimSupported() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= 5 && Config::g_healAnimPackage.load();
    }

    void ReleaseHealCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseClaimLocked(it->second.heal);
        EraseIfEmpty(it);
    }

    bool IsHealCastActive(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.heal.handle != APMF_API::kInvalidHandle;
    }

    // Which spell the heal slot's live claim names (0 when none stands). Added for
    // the preempt path (2026-09-08), which must tell "the lock I am displacing IS
    // the heal claim" from "an offense claim on the LEFT hand, with an unrelated
    // heal claim coexisting" -- releasing the second case's heal would drop a claim
    // nothing asked about. Same g_mx / no-live-claim-answers-0 contract as
    // GetHealCastProxy below.
    RE::FormID GetHealCastSpell(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.heal.handle == APMF_API::kInvalidHandle) return 0;
        return it->second.heal.spell;
    }

    RE::FormID GetHealCastProxy(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return 0;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.heal.handle == APMF_API::kInvalidHandle) return 0;
        return it->second.heal.proxy;
    }

    // See the header for the full rationale (why the stamp, why the age cap, and
    // why the ABI < 6 branch refuses outright). Deliberately does NOT release a
    // dead claim: the SAME shape EnsureCastClaimLocked uses for a handle APMF has
    // already auto-expired -- stop treating it as live and let the normal path
    // (here: Tick()'s sweep, which then sees a stamp that stopped moving) clear
    // it, rather than issuing a Release against a handle APMF no longer knows.
    bool RefreshHealCastClaim(RE::FormID a_follower) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end() || it->second.heal.handle == APMF_API::kInvalidHandle) return false;
        // ── THE NEVER-OBSERVED HOLD CAP (Fable SEV-1 2026-09-06; MECHANISM AND
        //    SIZING BOTH CORRECTED by the Fable diff review, same day) ─────────
        // The ONLY caller is ComposedCast::Try's incumbent hold, and it calls
        // this only while the incumbent has NOT been observed firing.
        //
        // THE RENEWAL MECHANISM IS REAL NOW -- AND THIS FUNCTION STILL MUST NOT
        // USE IT (F5-1, 2026-09-08). The note that stood here said the F2 renewal
        // could not happen at all ("MFO never Repoints a kIntent_Cast claim, and
        // APMF main's ApplyRepoint never touches expiresMs; F2 is on an unmerged
        // branch"). Both halves are now false: APMF's TTL RENEWAL is merged
        // (core/ControlMap.cpp -- a Repoint on a LIVE cast claim moves its
        // deadline to now + its own granted ttlMs), and EnsureCastClaimLocked's
        // fast path Repoints a live cast claim on a heartbeat. Rewritten rather
        // than deleted, because a comment teaching a false mechanism is worse
        // than no comment.
        //
        // WHAT THAT MEANS HERE. The old note's fear -- a renewal keeping
        // IsClaimLive true forever and deadlocking this hold -- is exactly why
        // the heartbeat lives in EnsureCastClaimLocked and NOT in this function.
        // A renewal there is sent only by a rule that WON its lap and is asking
        // for the identical claim it already holds; this function is the opposite
        // case, a hold kept alive for an incumbent whose own rule may have gone
        // silent. So this function still only bumps MFO's local `refreshed` and
        // NEVER Repoints: the moment the incumbent's rule stops asking, nothing
        // renews APMF's window and the claim dies at its TTL, bound 1 intact.
        // Bound 2 (below) actually gets STRONGER, because `created` is no longer
        // reset by a 6s auto-expiry + re-request cycle that no longer happens.
        //
        // WHAT THE CAP REALLY DOES, and why it is still needed. It stops the
        // HOLD'S HEARTBEAT from outliving the incumbent RULE's interest. Every
        // other exit the hold has is conditional on something that may never
        // happen: `observed` needs the engine to really cast; an incumbent CHANGE
        // needs a successful ClaimHealCast the hold itself prevents (and
        // ComposedCast::End() has no caller at all); Tick()'s FacetExpiry sweep
        // cannot fire while this very function keeps bumping `refreshed`. And the
        // incumbent's own rule keeps ClaimHealCast-ing for as long as its
        // condition holds, re-requesting a fresh handle whenever APMF expires the
        // old one. So an incumbent the engine NEVER casts can re-arm indefinitely
        // while a competing rule starves behind it.
        //
        // SIZED FROM THE MEASURED HEAL LATENCY, NOT FROM THE CLAIM TTL AND NOT
        // FROM AN OFFENSE FIRE (#9). kHealHoldNeverObservedMs is 4000ms, sized
        // from the claim-to-OBSERVED time of the one heal that landed on the deck
        // (2.95s: minted 19:59:11.158, MFO `[cast]` 19:59:14.110) plus the
        // measured tail. It is NOT kHealCastTtlMs's 6000ms (what this line read
        // before the Fable diff review), and it is NOT the 2.3-2.5s equip+charge
        // number that briefly sized it at 3000ms -- that figure is an OFFENSE
        // Firebolt, and lifting at 3000ms would have released a heal that was
        // already CASTING. See kHealHoldNeverObservedMs's own comment in
        // APMFBridge.h for the full working, the lap-granularity slack, and the
        // trade-off it is chosen on. Note there is no "provably dead" point in
        // this evidence, and the reachable-hold cases are TWO, not one (a held
        // rule usually outranks the incumbent, but a lower-ranked rule is held
        // whenever the incumbent's own condition has gone false).
        //
        // `created` is stamped once per handle, in EnsureCastClaimLocked's mint
        // block right after its RequestCast, and is moved by no refresh, no
        // APMF-side liveness check, not by this heartbeat and not by F5-1's TTL
        // heartbeat (which moves `renewed` instead, precisely so this clock stays
        // still) -- but it IS moved when the incumbent's own rule re-requests
        // after an APMF auto-expiry (that function's `everLive` aged-out branch
        // falls through to the same mint, giving a new handle a new stamp; F5-1
        // makes that path RARE while a rule keeps winning, since the window is
        // renewed instead of being allowed to lapse).
        // That is harmless HERE, and only here, because that path cannot run
        // underneath a live hold: the incumbent's re-request returns Claimed,
        // which stops the rule scan before any held rule's Try() is reached. It
        // is not the blanket immunity this comment used to assert.
        //
        // NOT a cap on an OBSERVED claim: a live channelled heal never routes
        // through here at all (the caller's `!observed` guard), so a real cast
        // keeps running for as long as its rule and APMF agree it should.
        const auto now = std::chrono::steady_clock::now();
        if (now - it->second.heal.created >= std::chrono::milliseconds(kHealHoldNeverObservedMs))
            return false;
        // ABI < 6 has no IsClaimLive, so there is NO bound available here at all:
        // the unchanged fast path in EnsureCastClaimLocked trusts a stored handle
        // forever on ABI < 6, so the incumbent's own rule keeps `refreshed`
        // moving and Tick()'s sweep never fires either. Reporting the stored
        // handle as live would therefore hold a newcomer off behind a handle APMF
        // may have silently expired, with nothing to end it (#7 -- that is a mask,
        // and the header used to claim a sweep bound it does not have). Refuse:
        // an honest degrade to the pre-F1 behaviour (both rules thrash the slot,
        // visibly), never a hold nobody can break. Inert in practice -- the pair
        // ships together at kABIVersion 6.
        if (api->abiVersion < 6) return false;
        if (!reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(it->second.heal.handle))
            return false;                       // dead APMF-side: no heartbeat, Tick() sweeps it
        it->second.heal.refreshed = now;
        // LATCH THE OBSERVATION TOO (SEV-4, pre-merge review 2026-09-07). This IS a
        // genuine `IsClaimLive == true` sighting of this exact handle, and everLive's
        // own doc says it is set "the first time IsClaimLive(handle) answers true".
        // Without it, a heal whose only liveness sightings came through the hold path
        // would still look never-published to EnsureCastClaimLocked and be reported as
        // an outright refusal it never suffered.
        it->second.heal.everLive = true;
        return true;
    }

}
