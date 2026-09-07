#include "APMFBridge.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"   // g_active.size() -- round-robin-aware expiry sizing (FacetExpiry)
#include "Loadout.h"     // WeaponHandActive's live-grip read
#include "MainThread.h"
#include "Rapport.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

// Two Win32 symbols, declared by hand. <windows.h> is BANNED outside Board.cpp --
// it #defines GetObject and hijacks BGSDefaultObjectManager::GetObject<T>
// (ENGINE_NOTES §9; the same reason Targeting.cpp / Logistics.cpp hand-declare
// GetModuleHandleA). Pointer args/return are pointer-sized on Win64, so void* is
// ABI-correct for HMODULE/FARPROC; the real header is never in this TU to conflict.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_procName);

namespace MFO::APMFBridge {

    namespace {
        // The APMF interface (v2+, so RequestEx is present; Repoint needs v3 -- gated
        // per call on api->abiVersion). Written ONCE at kDataLoaded (main) before any
        // worker tick runs; read on the worker. Atomic (relaxed) enforces the
        // publish/consume contract cheaply.
        std::atomic<const APMF_API::APMF_API_v2*> g_apmf{ nullptr };

        // Per-follower owned-cast claims -- TWO INDEPENDENT LIFECYCLES:
        //   * offense-cast (kIntent_Cast, ch.8b): PER-CAST. Refreshed each winning
        //     cast tick; released CRISPLY the moment no cast rule holds
        //     (ReleaseOffenseCast <- Scheduler !castSeen); the expiry is only a
        //     backstop. PORTED feat/offense-cast-seats (2026-09-05) off the retired
        //     ch.8 kIntent_SelectSpell gate-only claim this field used to back --
        //     see Owned::offense below (feat/per-hand-cast-slots, 2026-09-06:
        //     now a 2-slot array, one per hand) and APMFBridge.h's
        //     ClaimOffenseCast doc for the full port rationale. IMPORTANT
        //     (2026-09-05, corrects an
        //     earlier false assumption): "each winning cast tick" is a
        //     Scheduler::Tick ROUND-ROBIN lap for this follower's OWN service, not a
        //     tight combat-thread beat -- ONE follower is serviced per ~133ms pump
        //     (Scheduler.cpp), so the real refresh gap is ~0.133s * partySize. See
        //     FacetExpiry() below, which this claim now uses instead of the flat
        //     kExpiry.
        //   * combat-TARGET: PER-COMBAT. Refreshed every in-combat tick (by the cast/
        //     attack directive that steers it AND by RefreshCombatTarget from the
        //     combat service), RE-POINTED on a target change (same handle), and
        //     released ONLY by the expiry sweep once refreshing STOPS -- i.e. at combat
        //     end. It is NOT tied to the cast gambit's win/lose, so a cast->melee
        //     transition RE-POINTS it, never releases it. Same round-robin caveat as
        //     cast-SELECT above -- "every in-combat tick" means this follower's own
        //     Scheduler::Tick lap, so it too now uses FacetExpiry().
        // Each claim carries its own refresh timestamp. Guarded by g_mx (worker + main
        // + the COMBAT THREAD: Phase 2's IsOwnedCastActive/IsEquipmentClaimActive are
        // called from CasterConsent/CombatStyle thunks). Real mutex, never nested, never
        // held across a Post/form-table walk, so the combat thread can't stall on it.
        // package-offer (ch.9) is a PER-EXCURSION, caller-driven lifecycle wired into
        // Packages.cpp's loot-travel routing: created on the first winning dispatch,
        // refreshed by a repeat call (same form == cheap no-op via EnsureClaimLocked),
        // released the instant the caller says so (arrival/loot-done/abandoned). ITS
        // refresh is genuinely NOT round-robin-bound -- Packages::Pump() refreshes
        // every live package-offer claim UNCONDITIONALLY, for every active slot, on
        // EVERY Scheduler::Tick call (~133ms flat, independent of party size and of
        // whose turn the round-robin cursor is on) -- so the flat kExpiry backstop
        // really is a safe ~3.7x margin here (verified 2026-09-05, not assumed).
        // combat-action-deny (ch.7) shares the SAME claim machinery and was designed
        // to piggyback on that identical Packages.cpp per-excursion cadence, but is
        // currently UNWIRED (no caller anywhere in the tree -- "built but not wired",
        // Docs/STATUS.md) -- actionHandle is dead weight today, so its flat kExpiry is
        // unexercised rather than proven; revisit sizing WHEN it is actually wired,
        // based on whatever actually drives it then.
        // A single kIntent_Cast claim's full state -- ONE handle plus the
        // (spell,target,hand,concentration,stopPct) it was created/refreshed
        // with, plus its own refresh timestamp. Factored out (feat/per-hand-
        // cast-slots, 2026-09-06) so `Owned::offense` can hold TWO of these
        // (one per hand) without duplicating six parallel fields per slot; heal
        // stays a single instance (always LEFT, never per-hand).
        struct CastClaim {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            RE::FormID       spell  = 0;
            RE::FormID       target = 0;
            std::int32_t     hand   = 0;
            bool             conc   = false;
            std::uint32_t    stopPct = 0;
            std::chrono::steady_clock::time_point refreshed{};
            // WHEN THIS HANDLE WAS REQUESTED -- the claim's own AGE clock, set
            // ONCE where `handle` is assigned below and never moved again while
            // the same handle stands (Fable SEV-1, 2026-09-06). Distinct from
            // `refreshed` on purpose: `refreshed` is a heartbeat every interested
            // party bumps (ClaimHealCast, RefreshHealCastClaim), so it can never
            // bound anything a still-interested rule keeps asking for. `created`
            // is bumped by NOTHING -- not a refresh, not APMF-side liveness, not
            // a TTL renewal (APMF's Repoint renewal extends the claim on APMF's
            // side; it does not re-request a handle here) -- so it is the one
            // clock that can cap how long a NEVER-OBSERVED claim is allowed to
            // hold ComposedCast's single heal slot. Reset to the epoch by
            // `c = CastClaim{}` on every release/re-request, so a genuinely new
            // claim always gets a full fresh window. See RefreshHealCastClaim.
            std::chrono::steady_clock::time_point created{};
            // ABI v6 observability (cast-claim observability, 2026-09-06): the
            // delivery-flip proxy FormID APMF minted internally for `handle`
            // (APMF_API::APMF_API_v6::GetCastProxy), fetched once right after a
            // successful RequestCast. 0 on ABI < 6, or when this claim minted no
            // proxy. Read-only bookkeeping -- exposed to callers (GetHealCastProxy/
            // GetOffenseCastProxy below) so ComposedCast's silent-claim watch can
            // recognise a cast of the PROXY, not just the original spell, as our
            // own claim actually firing (see ComposedCast.h/.cpp).
            RE::FormID       proxy  = 0;
        };

        struct Owned {
            APMF_API::Handle targetHandle = APMF_API::kInvalidHandle;  RE::FormID target = 0;
            std::chrono::steady_clock::time_point targetRefreshed{};
            APMF_API::Handle packageHandle = APMF_API::kInvalidHandle;  RE::FormID package = 0;
            std::chrono::steady_clock::time_point packageRefreshed{};
            APMF_API::Handle actionHandle  = APMF_API::kInvalidHandle;  std::uint32_t actionMask = 0;
            std::chrono::steady_clock::time_point actionRefreshed{};
            // weapon-order equipment (ch.15) -- PER-ORDER: refreshed every tick the
            // force-hold survives (Actuation::ReconcileForcedWeapon), released the
            // instant it releases (Actuation::ReleaseForcedWeapon). PROVEN
            // round-robin-bound, not tight-cadence (deck 2026-09-05, Cicero):
            // ReconcileForcedWeapon is called from the SAME per-follower
            // Scheduler::Tick service as cast-select/combat-target above, so "every
            // tick the force-hold survives" is one round-robin lap, same gap as
            // those two. Uses FacetExpiry() below, not the flat kExpiry.
            APMF_API::Handle equipHandle   = APMF_API::kInvalidHandle;  RE::FormID equip  = 0;
            std::chrono::steady_clock::time_point equipRefreshed{};
            // heal-cast (ch.8b, kIntent_Cast/RequestCast, ported feat/mfo-cast-port)
            // -- PER-CAST, TTL-bounded, single slot (LEFT always -- ClaimHealCast's
            // own hard rule, never per-hand). Held by ComposedCast for the life of
            // a claimed heal; refreshed every tick the gambit still wants it
            // (create-or-refresh on a spell/target/hand/concentration/stopPct
            // change -- RequestCast has no in-place re-point, so a CHANGE releases
            // and re-requests, a new bounded claim), released the instant it stops
            // (ComposedCast::End), and auto-expired by FacetExpiry() (below) AND
            // (on APMF's side) by the claim's own TTL if a caller forgets. Distinct
            // from `offense` below (heal and offense claims never overlap on one
            // follower's SAME hand -- CasterConsent::SpellKind makes them mutually
            // exclusive per tick for a given spell -- but each gets its own state
            // to avoid any cross-talk; a heal on LEFT and an offense claim on RIGHT
            // CAN now be concurrently live, feat/per-hand-cast-slots).
            CastClaim heal;
            // offense-cast (ch.8b, kIntent_Cast/RequestCast, PORTED feat/offense-
            // cast-seats, 2026-09-05, off the retired ch.8 kIntent_SelectSpell
            // gate-only claim this slot used to back -- see APMFBridge.h's
            // ClaimOffenseCast doc) -- PER-CAST, TTL-bounded, and PER-HAND
            // (feat/per-hand-cast-slots, 2026-09-06): [0] = left, [1] = right, so
            // two independent offense claims can stand on one follower at once
            // (the parallel APMF change arbitrates kIntent_Cast per (actor, hand)
            // now). A DualCast claim is ONE underlying handle mirrored into BOTH
            // indices -- see ClaimOffenseCast's own doc for how that mirroring is
            // created and torn down without a double-release.
            CastClaim offense[2];
        };
        std::mutex                             g_mx;
        std::unordered_map<RE::FormID, Owned>  g_owned;

        // MFO outbids APMF's mid-range test hotkeys (basis 100) so a real gambit wins
        // the hand/target over a tester's Numpad keys. Arbitrary among clients; > 100.
        constexpr float kOwnBasis = 200.0f;

        // A claim not refreshed within this window is released. Genuinely flat-safe
        // ONLY for facets proven to refresh on a true ~133ms beat regardless of party
        // size -- verified (2026-09-05) to be package-offer alone (Packages::Pump()
        // refreshes it unconditionally, every Scheduler::Tick, for every active slot --
        // NOT gated by the round-robin cursor). combat-action-deny is unwired (no
        // current refresher at all) and inherits this pending real evidence once it is
        // wired. Every OTHER facet (cast-select, combat-target, weapon-order equipment,
        // heal-cast) is refreshed from INSIDE the per-follower Scheduler::Tick
        // ROUND-ROBIN service -- ONE follower serviced per ~133ms pump -- and uses
        // FacetExpiry() below instead.
        constexpr auto kExpiry = std::chrono::milliseconds(500);
    }   // end anon namespace -- FacetExpiry (below) is now EXTERNAL LINKAGE, declared
        // in APMFBridge.h, so feat/cast-gambit-concentration's cross-TU cast-lock
        // staleness window (Actuation.cpp, Task 2) can reuse the SAME round-robin-
        // aware sizing instead of inventing its own budget (#9). Purely a visibility
        // move -- no formula change; every existing in-TU call site below still
        // resolves it via the header declaration (included at the top of this file).

    // ROUND-ROBIN-AWARE FACET EXPIRY (deck 2026-09-05, found via the heal-cast
    // claim: claim/release every ~530ms, caster stuck at rest forever; then
    // RE-PROVEN on the weapon-order equipment claim, Cicero deck capture: CLAIMED
    // gate-only -> APMF-side RELEASED 919ms later, a full round-robin lap early,
    // while MFO's own force-hold was still standing -- see ReconcileForcedWeapon's
    // call site for the trace). An earlier version of this comment asserted the
    // flat 500ms kExpiry was "a fine backstop" for cast-select/combat-target/
    // equipment because "a live combat controller re-Wants every combat-thread
    // beat" -- THAT WAS WRONG, disproven by the Cicero capture: all three are
    // refreshed from the SAME per-follower Scheduler::Tick ROUND-ROBIN lap the
    // heal claim uses (ClaimOffenseCast/ClaimCombatTarget <- Actuation::CastOn <-
    // Actuation::Fire <- Scheduler.cpp's round-robin service; ClaimEquipment <-
    // Actuation::ReconcileForcedWeapon <- the SAME service) -- ONE follower
    // serviced per ~133ms (Scheduler.cpp), so a given follower's own gambit only
    // re-fires (and re-Claims/Repoints/refreshes) every ~0.133s * partySize. For
    // anything but a 1-2-follower party that gap already exceeds the flat 500ms,
    // so the sweep in Tick() below released a live, still-wanted claim every
    // round-robin lap. Size it the SAME way TargetCastReconcile/SelfCastReconcile
    // already size their own round-robin-aware release windows: out-wait the
    // worst-case suppression + round-robin gap, floored at the old kExpiry so a
    // small party never regresses to a SLOWER release than before. Shared by
    // offense[0]/offense[1]/targetHandle/equipHandle/heal -- one formula, no
    // per-facet copy-paste (none of these need different sizing: they all
    // share the identical round-robin service as their refresh source). NOW ALSO
    // reused by Actuation.cpp's cast-gambit lock (Task 2) as the staleness window
    // for its own un-claimed (direct-force concentration) fallback case.
    std::chrono::milliseconds FacetExpiry() {
        const float suppress  = std::max(0.0f, Config::g_suppressWindow.load());
        const float partySize = static_cast<float>(Followers::g_active.size() + 1);   // + player
        const float sec = std::max(0.5f, suppress * 1.12f + 0.133f * partySize + 0.5f);
        return std::chrono::milliseconds(static_cast<std::uint64_t>(sec * 1000.0f));
    }

    namespace {

        // Ensure ONE channel claim tracks `want` (0 == release it). Caller holds g_mx.
        // On a CHANGE of an existing claim, RE-POINTS in place via Repoint (v3, same
        // handle -- no release/re-engage churn); falls back to Release+Request only
        // against an older v2 APMF. APMF Release/RequestEx/Repoint are thread-safe.
        void EnsureClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                               APMF_API::Intent intent, APMF_API::Handle& handle,
                               RE::FormID& cur, RE::FormID want) {
            if (want == 0) {                                   // no claim wanted
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                cur = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && cur == want) return;   // unchanged
            APMF_API::APMF_Param p{};
            p.form = want;
            if (handle == APMF_API::kInvalidHandle) {                        // create
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            } else if (api->abiVersion >= 3) {                               // re-point in place
                reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(handle, &p);
                cur = want;
            } else {                                                        // v2 fallback: release+request
                api->Release(handle);
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            }
        }

        // ival twin of EnsureClaimLocked, for kIntent_CombatAction's param.ival
        // (a category bitmask) rather than a param.form. Same create / re-point-in-
        // place (v3) / release+request (v2 fallback) shape; `want == 0` releases.
        void EnsureIvalClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                                   APMF_API::Intent intent, APMF_API::Handle& handle,
                                   std::uint32_t& cur, std::uint32_t want) {
            if (want == 0) {
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                cur = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && cur == want) return;   // unchanged
            APMF_API::APMF_Param p{};
            p.ival = static_cast<std::int32_t>(want);
            if (handle == APMF_API::kInvalidHandle) {
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            } else if (api->abiVersion >= 3) {
                reinterpret_cast<const APMF_API::APMF_API_v3*>(api)->Repoint(handle, &p);
                cur = want;
            } else {
                api->Release(handle);
                handle = api->RequestEx(follower, intent, kOwnBasis, &p);
                cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
            }
        }

        // Release one handle (thread-safe; no-ops a stale handle). Caller holds g_mx.
        // RE::FormID IS std::uint32_t (a plain alias, not a distinct type), so this
        // ONE overload also serves actionMask -- a second overload on that "different"
        // parameter type would be a duplicate-definition error, not a real overload.
        void ReleaseHandleLocked(APMF_API::Handle& handle, RE::FormID& cur) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && handle != APMF_API::kInvalidHandle) api->Release(handle);
            handle = APMF_API::kInvalidHandle;
            cur = 0;
        }

        // Release ONE CastClaim's handle (if live) and reset it to default.
        // Caller holds g_mx. Safe to call on an already-empty claim (no-op).
        void ReleaseClaimLocked(CastClaim& c) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
            c = CastClaim{};
        }

        // kIntent_Cast create-or-refresh (kIntent_Cast/RequestCast, ch.8b) -- SHARED
        // by both ClaimHealCast and ClaimOffenseCast (renamed from
        // EnsureHealClaimLocked, feat/offense-cast-seats, 2026-09-05: the function
        // was already fully generic over its handle/curX out-params, only the name
        // was heal-specific; feat/per-hand-cast-slots, 2026-09-06: folded the six
        // parallel out-params into one CastClaim& now that offense needs TWO of
        // them). Unlike EnsureClaimLocked's retired kIntent_SelectSpell Repoint,
        // RequestCast's rich payload has NO in-place re-point -- a CHANGE in any of
        // (spell, target, hand, concentration, stopPct) releases the old claim and
        // requests a fresh one (a new bounded window; never two live claims on the
        // same slot at once). Caller holds g_mx AND must have already verified
        // api->abiVersion >= 5 (RequestCast is a v5 slot -- see ClaimHealCast/
        // ClaimOffenseCast). `wantSpell == 0` releases.
        void EnsureCastClaimLocked(const APMF_API::APMF_API_v5* api, RE::FormID follower,
                                   CastClaim& c, RE::FormID wantSpell, RE::FormID wantTarget,
                                   std::int32_t wantHand, bool wantConc, std::uint32_t wantStopPct) {
            if (wantSpell == 0) {
                ReleaseClaimLocked(c);
                return;
            }
            if (c.handle != APMF_API::kInvalidHandle && c.spell == wantSpell &&
                c.target == wantTarget && c.hand == wantHand &&
                c.conc == wantConc && c.stopPct == wantStopPct) {
                // ABI v6 (cast-claim observability, 2026-09-06): do NOT trust a
                // stored handle blindly on the unchanged fast path -- APMF
                // auto-expires a claim at its own TTL with no notice to the
                // client (APMF_API.h's IsClaimLive doc), so MFO could keep
                // believing a long-dead handle is still in force (the exact
                // field failure diagnosed 2026-09-06: the gambit re-fired for
                // 23s while this early-return kept swallowing every re-fire and
                // RequestCast never reached APMF again). ABI < 6 has no way to
                // ask this and keeps today's behaviour byte-identical.
                //
                // LATENT, AND THE CORRECTNESS DEPENDENCY IS DOCUMENTED NOWHERE
                // ELSE (Fable diff review, 2026-09-06). IsClaimLive walks APMF's
                // PUBLISHED snapshot only (core/ControlMap.cpp) -- the same shape
                // as the GetCastProxy bug F4 fixes just below -- so a handle
                // RequestCast minted in THIS frame, before APMF's next per-frame
                // Drain publishes it, reads as NOT live. Called in the same frame
                // as a fresh RequestCast, this branch therefore logs "already
                // auto-expired" and re-requests a claim that is in fact alive.
                // THE INVARIANT IS NARROWER THAN "ONE heal Try() PER TICK", and
                // the broad version stated here was wrong (round-3 review,
                // 2026-09-07): Logistics really does run several heal Try()s per
                // follower per tick (a Held outcome continues the scan, and the
                // `pass < 2 && !acted` wrapper re-runs it). What cannot happen is
                // a second ClaimHealCast on the SAME tick as one that MINTED a
                // handle: a Claimed outcome ends every scan, and a Refused one
                // leaves no incumbent behind for a later Try() to hold. So every
                // call that reaches this branch is looking at a handle minted on
                // an EARLIER tick, which APMF has long since published. THAT is
                // the undocumented dependency this fast path's correctness rests
                // on -- break it and this thrashes for that tick.
                if (api->abiVersion < 6 ||
                    reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->IsClaimLive(c.handle)) {
                    // F4 (RC4, 2026-09-06): the proxy is minted lazily during
                    // APMF's own per-frame Drain, so the read at :417-418
                    // below (right after RequestCast) can still see 0 even
                    // though the SAME handle now has a real proxy published.
                    // Re-read it lazily here, on the unchanged/still-live fast
                    // path this claim takes on every later Try() while nothing
                    // changes, so the watch eventually learns it instead of
                    // caching that 0 forever.
                    if (c.proxy == 0 && api->abiVersion >= 6)
                        c.proxy = reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->GetCastProxy(c.handle);
                    return;   // unchanged AND still live -- cheap no-op, no release/re-request churn
                }
                spdlog::info("[apmf] {:08X} kIntent_Cast claim (spell {:08X}) already auto-expired "
                             "at APMF's own TTL -- re-requesting instead of trusting a dead handle",
                             follower, wantSpell);
                c = CastClaim{};   // treat as empty; fall through to the fresh RequestCast below
            }
            if (c.handle != APMF_API::kInvalidHandle) { api->Release(c.handle); c.handle = APMF_API::kInvalidHandle; }
            APMF_API::APMF_CastRequest req{};
            req.spell  = wantSpell;
            req.proxy  = 0;   // APMF mints its own delivery-flip proxy for kSelf-delivery (core/CastProxy.h)
            req.target = wantTarget;
            // kCastFlag_DualCast and kCastFlag_LeftHand are mutually exclusive (APMF_API.h:
            // "do NOT set kCastFlag_LeftHand alongside it") -- kApmfHandDualCast (Loadout::
            // HandPick::DualCast, via HandFor above) claims BOTH hands and never also sets
            // the single-hand hint. Anything else (kApmfHandRight, or bare 0) is the RIGHT
            // hint -- APMF's own default when kCastFlag_LeftHand is unset (APMF_API.h).
            const bool wantDual = (wantHand == kApmfHandDualCast);
            req.flags  = (wantDual                    ? APMF_API::kCastFlag_DualCast :
                          wantHand == kApmfHandLeft    ? APMF_API::kCastFlag_LeftHand : 0u) |
                         (wantConc ? APMF_API::kCastFlag_Concentration : 0u) |
                         APMF_API::MakeStopPct(wantStopPct);
            req.ttlMs  = kHealCastTtlMs;
            c.handle = api->RequestCast(follower, kOwnBasis, &req);
            // [cfc] dual-cast ask vs. observed outcome (marth 2026-09-06): the flag is a
            // HINT (APMF_API.h) -- APMF may still only arm one hand, and never reports which.
            // This distinguishes "asked for dual, claim granted" (engine may still degrade to
            // one hand silently downstream) from "asked for dual, claim REFUSED outright" from
            // "never asked" (no log at all) -- so the field can tell a refused second hand from
            // a policy that never requested one. Fires only on a claim CREATE/CHANGE (the
            // unchanged-claim fast path above already skips repeat ticks), so this is
            // inherently rate-limited, not spammy.
            if (wantDual) {
                if (c.handle != APMF_API::kInvalidHandle)
                    spdlog::info("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "granted; engine may still arm only one hand (hint only)",
                                 follower, wantSpell);
                else
                    spdlog::warn("[cfc] {:08X} asked for dual-cast (spell {:08X}) -- claim "
                                 "REFUSED outright, not just downgraded to one hand",
                                 follower, wantSpell);
            }
            if (c.handle != APMF_API::kInvalidHandle) {
                c.spell = wantSpell; c.target = wantTarget; c.hand = wantHand;
                c.conc  = wantConc;  c.stopPct = wantStopPct;
                // Fable SEV-1 (2026-09-06): the claim's AGE clock, stamped HERE
                // and only here -- this is the single point where a NEW handle
                // comes into existence. The unchanged-and-still-live fast path
                // above deliberately does NOT touch it, which is exactly what
                // makes it a bound (see the `created` field comment above and
                // RefreshHealCastClaim below).
                c.created = std::chrono::steady_clock::now();
                // ABI v6: fetch the delivery-flip proxy APMF minted for this claim
                // (0 on ABI < 6, or a claim that minted no proxy) -- recorded
                // alongside the claimed spell so a later match on the OBSERVED
                // cast (Diagnostics.cpp's SpellSink, via ComposedCast's watch) can
                // recognise a cast of the proxy as our own claim actually firing,
                // not "their own spell, not ours" (the false-alarm diagnosed
                // 2026-09-06 -- MFO only ever matched the original spell).
                c.proxy = (api->abiVersion >= 6)
                    ? reinterpret_cast<const APMF_API::APMF_API_v6*>(api)->GetCastProxy(c.handle)
                    : 0;
            } else {
                c = CastClaim{};
            }
        }

        // Drop the map entry once EVERY claim is gone. Caller holds g_mx.
        void EraseIfEmpty(std::unordered_map<RE::FormID, Owned>::iterator it) {
            const auto& o = it->second;
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle && o.offense[1].handle == APMF_API::kInvalidHandle)
                g_owned.erase(it);
        }
    }

    void Acquire() {
        g_apmf.store(nullptr, std::memory_order_relaxed);
        void* h = GetModuleHandleA("APMF.dll");
        if (!h) {
            spdlog::info("[apmf] interface absent -- APMF.dll not in the load order; "
                         "owned-cast model OFF (MFO falls back to the legacy AI-first cast hybrid).");
            return;
        }
        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) {
            spdlog::warn("[apmf] APMF.dll present but '{}' not exported -- owned-cast model OFF.",
                         APMF_API::kGetInterfaceExport);
            return;
        }
        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) {
            spdlog::warn("[apmf] APMF refused ABI v{} (too old) -- owned-cast model OFF.",
                         APMF_API::kABIVersion);
            return;
        }
        if (base->abiVersion < 2) {
            spdlog::warn("[apmf] APMF ABI v{} has no RequestEx (need >= 2) -- owned-cast model OFF.",
                         base->abiVersion);
            return;
        }
        auto* api = reinterpret_cast<const APMF_API::APMF_API_v2*>(base);
        g_apmf.store(api, std::memory_order_relaxed);
        spdlog::info("[apmf] interface acquired (ABI v{}) -- owned-cast model enabled "
                     "(target+spell owned; AI fires it ANIMATED; re-point {}).",
                     api->abiVersion, api->abiVersion >= 3 ? "in place" : "via release+request (v2)");
    }

    bool Available() { return g_apmf.load(std::memory_order_relaxed) != nullptr; }

    namespace {
        // The exact toast wording, in ONE place. Plain, one sentence, no em/en-
        // dash, no semicolon (Docs/VOICE.md).
        constexpr const char* kNoApmfToast =
            "MFO works best with Harbinger (APMF) installed, and it's running in fallback mode without it.";

        constexpr double kWarnFirstAtMinutes = 0.6;    // ~36s after load: seen once, early
        constexpr double kWarnEveryMinutes   = 10.0;   // then roughly every 10 minutes

        // -1.0 = never fired yet this session. Rapport::SessionMinutes() resets on
        // every load, so a load that makes it go backwards (mins < last) is this
        // module's own "new session" signal -- no separate reset hook needed, and
        // nothing here touches serialization.
        std::atomic<double> g_warnLastMinutes{ -1.0 };
    }

    void MaybeWarnAbsence() {
        if (Available()) return;                     // APMF present: never warn, one cheap check
        if (!Config::g_warnNoApmf.load()) return;     // silenced

        const double mins = Rapport::SessionMinutes();
        double last = g_warnLastMinutes.load(std::memory_order_relaxed);
        if (mins < last) last = -1.0;                 // a new load happened; treat as a fresh session

        const bool fire = (last < 0.0) ? (mins >= kWarnFirstAtMinutes)
                                        : (mins - last >= kWarnEveryMinutes);
        if (!fire) return;

        g_warnLastMinutes.store(mins, std::memory_order_relaxed);
        MainThread::Post([]() { RE::DebugNotification(kNoApmfToast); });
    }

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

    // ── combat-TARGET (per-combat) ──────────────────────────────────────────────
    void ClaimCombatTarget(RE::FormID a_follower, RE::FormID a_target, bool a_create) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_target == 0 || !Config::g_apmfCast.load()) return;
        std::scoped_lock lock(g_mx);
        Owned* o = nullptr;
        if (a_create) {
            o = &g_owned[a_follower];
        } else {
            // Refresh/re-point ONLY an existing claim -- never CREATE one from a
            // non-cast directive, so a pure-melee follower (who never cast) keeps
            // using MFO's own targeting, not APMF combat-target.
            auto it = g_owned.find(a_follower);
            if (it == g_owned.end() || it->second.targetHandle == APMF_API::kInvalidHandle) return;
            o = &it->second;
        }
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_CombatTarget, o->targetHandle, o->target, a_target);
        o->targetRefreshed = std::chrono::steady_clock::now();
        EraseIfEmpty(g_owned.find(a_follower));
    }

    void RefreshCombatTarget(RE::FormID a_follower) {
        if (!g_apmf.load(std::memory_order_relaxed)) return;
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it != g_owned.end() && it->second.targetHandle != APMF_API::kInvalidHandle)
            it->second.targetRefreshed = std::chrono::steady_clock::now();   // keep-alive: timestamp only
    }

    // ── weapon-order equipment (per-order, ch.15) ───────────────────────────────
    void ClaimEquipment(RE::FormID a_follower, RE::FormID a_weaponForm) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_weaponForm == 0 || !Config::g_weaponStyleControl.load()) return;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_Equipment, o.equipHandle, o.equip, a_weaponForm);
        o.equipRefreshed = std::chrono::steady_clock::now();
        EraseIfEmpty(g_owned.find(a_follower));
    }

    void ReleaseEquipment(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.equipHandle, it->second.equip);
        EraseIfEmpty(it);
    }

    bool IsEquipmentClaimActive(RE::FormID a_follower) {
        // Fast-out before the lock: APMF absent -> never active (mirrors
        // IsOwnedCastActive exactly, so the EquipGateThunk caller's native
        // enforcement path is byte-identical when APMF is not present).
        if (!g_apmf.load(std::memory_order_relaxed) || a_follower == 0) return false;
        std::scoped_lock lock(g_mx);
        const auto it = g_owned.find(a_follower);
        return it != g_owned.end() && it->second.equipHandle != APMF_API::kInvalidHandle;
    }

    bool WeaponHandActive(RE::Actor* a_follower) {
        if (!a_follower) return false;
        // Live read FIRST -- cheap, and correct even when APMF is absent (a
        // legacy-hybrid follower with a sword out is still weapon-active with
        // no equip-gambit claim in the picture at all).
        const auto grip = Loadout::Read(a_follower, nullptr).grip;
        if (grip == Loadout::Grip::OneHanded || grip == Loadout::Grip::TwoHanded) return true;
        // Momentarily-empty-handed race guard: an equip gambit holding a live
        // force-equip claim will reassert the weapon shortly even though the
        // hand reads free THIS tick (the deck-proven failure this exists to
        // close -- see this function's header doc).
        return IsEquipmentClaimActive(a_follower->GetFormID());
    }

    // ── package-offer (per-excursion) ───────────────────────────────────────────
    bool OfferPackage(RE::FormID a_follower, RE::FormID a_packageForm) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_packageForm == 0 || !Config::g_apmfLootTravel.load()) return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_OfferPackage, o.packageHandle, o.package, a_packageForm);
        o.packageRefreshed = std::chrono::steady_clock::now();
        const bool live = o.packageHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseOfferPackage(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.packageHandle, it->second.package);
        EraseIfEmpty(it);
    }

    // ── combat-action deny (per-excursion) ──────────────────────────────────────
    bool ClaimCombatActionDeny(RE::FormID a_follower, std::uint32_t a_categoryMask) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_categoryMask == 0 || !Config::g_apmfLootTravel.load()) return false;
        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureIvalClaimLocked(api, a_follower, APMF_API::kIntent_CombatAction, o.actionHandle, o.actionMask, a_categoryMask);
        o.actionRefreshed = std::chrono::steady_clock::now();
        const bool live = o.actionHandle != APMF_API::kInvalidHandle;
        EraseIfEmpty(g_owned.find(a_follower));
        return live;
    }

    void ReleaseCombatActionDeny(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseHandleLocked(it->second.actionHandle, it->second.actionMask);
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
        // WHAT THE NOTE THAT STOOD HERE GOT WRONG. It claimed APMF's F2 makes
        // Repoint RENEW a cast claim's TTL, so IsClaimLive could stay true
        // forever and the hold could deadlock. Neither shipped binary can produce
        // that: MFO never calls Repoint on a kIntent_Cast claim at all (its only
        // Repoints are the non-cast EnsureClaimLocked/EnsureIvalClaimLocked at
        // :238/:265 -- RequestCast has no in-place re-point, so a CHANGE releases
        // and re-requests), and APMF **main**'s ApplyRepoint (core/ControlMap.cpp)
        // writes `self->param` and never touches `expiresMs`. F2 is on an
        // unmerged APMF branch. Recorded rather than quietly deleted, because a
        // comment teaching a false mechanism is worse than no comment.
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
        // `created` is stamped once per handle (:409, right after the
        // RequestCast at :381) and is moved by no refresh, no APMF-side liveness
        // check and not by this heartbeat -- but it IS moved when the incumbent's
        // own rule re-requests after an APMF auto-expiry (:360-363 falls through
        // to :381, minting a new handle with a new :409 stamp).
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
        return true;
    }

    void Tick() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        // Computed once per sweep, not per-facet-per-follower: same inputs
        // (Config::g_suppressWindow, Followers::g_active.size()) for every claim
        // checked below, and Tick() already holds g_mx for the whole loop.
        const auto facetExpiry = FacetExpiry();
        for (auto it = g_owned.begin(); it != g_owned.end();) {
            auto& o = it->second;
            // offense[0]/[1]: PER-HAND now (feat/per-hand-cast-slots) -- each
            // slot expires independently. A mirrored DualCast claim shares ONE
            // handle across both slots and keeps identical `refreshed` stamps
            // (ClaimOffenseCast stamps both together), so it goes stale on the
            // SAME sweep pass for both -- release the shared handle exactly
            // once (capture the dual-ness BEFORE releasing slot i, since
            // releasing zeroes it).
            for (std::size_t i = 0; i < 2; ++i) {
                auto& c = o.offense[i];
                if (c.handle != APMF_API::kInvalidHandle && now - c.refreshed >= facetExpiry) {
                    const bool sharedDual = o.offense[1 - i].handle == c.handle;
                    ReleaseClaimLocked(c);
                    if (sharedDual) o.offense[1 - i] = CastClaim{};
                }
            }
            if (o.targetHandle != APMF_API::kInvalidHandle && now - o.targetRefreshed >= facetExpiry)
                ReleaseHandleLocked(o.targetHandle, o.target);
            // package-offer: genuinely flat-refreshed every ~133ms regardless of party
            // size (Packages::Pump(), unconditional, every active slot) -- kExpiry stays.
            if (o.packageHandle != APMF_API::kInvalidHandle && now - o.packageRefreshed >= kExpiry)
                ReleaseHandleLocked(o.packageHandle, o.package);
            // combat-action-deny: unwired (no caller anywhere), so kExpiry is inert
            // today -- kept flat pending real evidence once something drives it.
            if (o.actionHandle != APMF_API::kInvalidHandle && now - o.actionRefreshed >= kExpiry)
                ReleaseHandleLocked(o.actionHandle, o.actionMask);
            if (o.equipHandle != APMF_API::kInvalidHandle && now - o.equipRefreshed >= facetExpiry)
                ReleaseHandleLocked(o.equipHandle, o.equip);
            if (o.heal.handle != APMF_API::kInvalidHandle && now - o.heal.refreshed >= facetExpiry)
                ReleaseClaimLocked(o.heal);
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle && o.offense[1].handle == APMF_API::kInvalidHandle)
                it = g_owned.erase(it);
            else
                ++it;
        }
    }

    void ClearTransientState() {
        std::scoped_lock lock(g_mx);
        for (auto& [id, o] : g_owned) {
            ReleaseHandleLocked(o.targetHandle, o.target);
            ReleaseHandleLocked(o.packageHandle, o.package);
            ReleaseHandleLocked(o.actionHandle,  o.actionMask);
            ReleaseHandleLocked(o.equipHandle,   o.equip);
            ReleaseClaimLocked(o.heal);
            // Mirrored DualCast claim: release the shared handle once (see
            // Tick()'s identical dedupe for why -- a stray second Release on
            // the same handle is never safe to assume harmless).
            const bool sharedDual = o.offense[0].handle != APMF_API::kInvalidHandle &&
                                     o.offense[0].handle == o.offense[1].handle;
            ReleaseClaimLocked(o.offense[0]);
            if (sharedDual) o.offense[1] = CastClaim{};
            else            ReleaseClaimLocked(o.offense[1]);
        }
        g_owned.clear();
    }
}
