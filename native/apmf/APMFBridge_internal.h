#pragma once
// apmf/APMFBridge_internal.h -- the APMF bridge's SHARED claim state. NOT a public
// API: only the native/apmf/*.cpp TUs may include this (the public API is
// apmf/APMFBridge.h). The wave-1 subsystem-folder split (2026-09-24) cut
// native/APMFBridge.cpp by claim family; everything here was in that file's
// anonymous namespace and is now used from more than one apmf/*.cpp TU, so it
// gained external linkage: the types moved here whole, variables are `extern`
// here and defined in apmf/Bridge.cpp, functions are declared here and defined
// in apmf/Bridge.cpp (unchanged), and the two small helpers the compiler INLINED
// across the new cut moved here whole as `inline`. tools/splitcheck proves the
// generated code is unchanged (modulo per-TU inlining drift, which it lists).
// THREADING is unchanged: every map below is guarded by g_mx exactly as before.

#include "PCH.h"
#include "APMFBridge.h"
#include "APMF_API.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MFO::APMFBridge {

        // The APMF interface (defined in apmf/Bridge.cpp; see its comment there).
        extern std::atomic<const APMF_API::APMF_API_v2*> g_apmf;

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
            // IS THIS THE IDLE-HAND FLOOR? (F10, 2026-09-08.) A claim carrying
            // APMF_API::kCastFlag_DenyHandOnly drives NOTHING: APMF forces its
            // spell, proxy and target to 0 when the claim is built
            // (APMF core/ControlMap.cpp ApplyRequest) and pins `param.form` to 0
            // on every Repoint, so it exists only to CLOSE its hand to the AI.
            // Part of this claim's IDENTITY, not a decoration: a floor and a
            // driving claim are different asks even when every other field
            // happens to match (both carry spell 0 only in the floor's case, but
            // the identity compare in EnsureCastClaimLocked must not be able to
            // mistake one for the other), and it selects the DenyHandOnly bit in
            // the request flags plus the `reqParam.form = 0` mirror the heartbeat
            // depends on.
            bool             denyOnly = false;
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
            // WAS THIS HANDLE EVER PUBLISHED AS LIVE? (F3-1, deploy-gate review
            // 2026-09-07.) Set true the first time IsClaimLive(handle) answers true,
            // and NEVER cleared while the same handle stands; reset to false with the
            // rest of the claim by `c = CastClaim{}` on every release/re-request, so
            // it always describes THIS handle and no earlier one.
            //
            // It exists to separate the only two ways a stored handle can read
            // not-live, which look identical at the call site but mean opposite
            // things:
            //   * everLive == true  -> the claim WAS in force and has since AGED OUT
            //     at APMF's own TTL. Normal, expected, and a re-request is right.
            //   * everLive == false -> APMF minted a handle, and it has never once
            //     been published as live. Handles are minted synchronously and
            //     arbitration is decided later in APMF's Drain (ControlMap.cpp), so
            //     this is the signature of an ARBITRATION LOSS: a refusal that
            //     arrives asynchronously instead of as a kInvalidHandle. Re-requesting
            //     it forever is what made the fail-closed path nearly unreachable.
            bool everLive = false;
            // ABI v6 observability (cast-claim observability, 2026-09-06): the
            // delivery-flip proxy FormID APMF minted internally for `handle`
            // (APMF_API::APMF_API_v6::GetCastProxy), fetched once right after a
            // successful RequestCast. 0 on ABI < 6, or when this claim minted no
            // proxy. Read-only bookkeeping -- exposed to callers (GetHealCastProxy/
            // GetOffenseCastProxy below) so ComposedCast's silent-claim watch can
            // recognise a cast of the PROXY, not just the original spell, as our
            // own claim actually firing (see ComposedCast.h/.cpp).
            RE::FormID       proxy  = 0;
            // ── CAST-CLAIM HEARTBEAT STATE (F5-1, 2026-09-08) ──────────────
            // WHEN APMF'S OWN TTL WINDOW LAST STARTED for this handle -- set
            // where `created` is set (the mint), and moved forward by EVERY
            // successful heartbeat Repoint below. This is the ONLY clock that
            // tracks APMF's `expiresMs`, and it is deliberately none of the
            // other three:
            //   * `created` must NOT move (it is the never-observed hold cap's
            //     bound -- see its comment above and RefreshHealCastClaim);
            //   * `refreshed` is bumped by every interested party every lap
            //     (ClaimHealCast/ClaimOffenseCast/RefreshHealCastClaim), so it
            //     measures MFO-side interest, never time-since-renewal;
            //   * APMF's `expiresMs` lives on APMF's side and is not readable.
            // NOT a second expiry (#9 / the sweep must not disagree with it):
            // nothing is ever released on this stamp. It only decides WHEN to
            // send the next renewal; the release decisions stay exactly where
            // they were (FacetExpiry()'s sweep on `refreshed`, the explicit
            // Release* calls, and -- on APMF's side -- the TTL itself).
            std::chrono::steady_clock::time_point renewed{};
            // THE EXACT APMF_Param APMF STORED FOR THIS CLAIM, kept verbatim so
            // a heartbeat Repoint can re-send it byte-for-byte instead of
            // rebuilding one that is merely "the same".
            //
            // WHAT IT MUST MIRROR IS `ApplyRequest`'s **EFFECTIVE** FORM, NOT
            // `req->spell` (review finding, 2026-09-08 -- the sentence that stood
            // here named only EnqueueCast and so recorded a dependency it did not
            // state). TWO functions shape what APMF ends up storing in
            // `self->param`, and only the first is a straight copy:
            //   * core/ControlMap.cpp's EnqueueCast writes
            //     `op.param.form = req->spell` and `op.param.ival = req->flags`
            //     into a zero-initialised APMF_Param, and nothing else.
            //   * core/ControlMap.cpp's ApplyRequest then REWRITES that form
            //     before the claim is stored, in exactly two cases:
            //       - kCastFlag_DenyHandOnly -> forced to 0 (that claim drives
            //         NOTHING for its whole life);
            //       - kCastFlag_FromPackage  -> replaced by the spell APMF
            //         EXTRACTED from the package.
            // MFO sets NEITHER flag today, which is the only reason a plain
            // `form = wantSpell` was byte-exact -- an unrecorded dependency, and
            // MFO's own deny-only hand claim is queued work
            // (memory cast-deny-only-hand-claim / apmf-f3-denyhand-followups).
            // So the mint below mirrors the DenyHandOnly rewrite locally. It
            // CANNOT mirror the FromPackage one: the extracted spell is resolved
            // inside APMF and there is no query that hands it back, so a future
            // caller that sets kCastFlag_FromPackage MUST solve that first (an
            // APMF-side read of the resolved spell) rather than assume this copy
            // is still exact. Stated here so the next reader inherits the
            // dependency instead of re-deriving it from a warn flood.
            //
            // WHAT GOES WRONG IF THE MIRROR DRIFTS. Not a mask -- ApplyRepoint
            // still renews the TTL -- but it REFUSES the form change and says so
            // (`spdlog::warn`), so every heartbeat would emit one warn per claim
            // per interval (~3 s at the defaults) for as long as the claim stands.
            //
            // WHY VERBATIM AND NOT REBUILT. APMF's ApplyRepoint REFUSES (loudly)
            // a Repoint whose `param.form` differs from the claim's current
            // spell, because a cast claim's delivery-flip proxy, resolved target
            // handle and CastFlags were all resolved AGAINST the original spell
            // inside RequestCast and Repoint re-runs none of that -- honouring a
            // form swap would leave the claim NAMED for one spell while DRIVING
            // another's proxy at another's target. A stored copy cannot drift
            // from what was requested; a rebuild can (the flags word is derived
            // from hand/conc/stopPct through three conditionals, and any future
            // edit to that derivation would silently desync the heartbeat).
            APMF_API::APMF_Param reqParam{};
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
            // ── THE IDLE-HAND FLOOR (F10, marth's design ruling 2026-09-08) ──
            // A SECOND, DENY-ONLY kIntent_Cast claim (APMF_API::
            // kCastFlag_DenyHandOnly) standing on whichever ONE hand MFO's own
            // driving claim above does NOT occupy.
            //
            // WHY MFO CLAIMS IT AT ALL. APMF scopes a cast claim's deny to that
            // claim's own hand, and leaving the other hand permissive is CORRECT
            // for a general framework -- an unclaimed hand belongs to the actor's
            // own AI. MFO's design is the narrower one: only GAMBITED spells
            // occur. marth, 2026-09-08: *"The followers other hand is supposed to
            // be claimed by MFO as idle is it not? Its proper APMF for it to
            // behave like it is currently. But its improper MFO. Under MFO it
            // should be idle when unclaimed."* So "the other hand goes idle" is
            // the SPECIFICATION here, not a cost being traded against the casts
            // it forgoes. Field shape it closes (Docs/DIAG-2026-09-08-field.md
            // RC3): every un-gambited cast that session -- 2 Poison Sprays, 5
            // Stone Runes, 3 Raise Zombies -- charged on the RIGHT hand inside a
            // window where MFO held only a LEFT-hand claim; where a claim DID
            // occupy the hand the deny went 3 for 3.
            //
            // ONLY EVER A COMPANION TO A DRIVING CLAIM -- never a standing hold
            // of its own. See ReconcileHandFloorLocked below for the full
            // derivation, including WHY "MFO holds no cast claim at all" floors
            // NEITHER hand rather than both.
            //
            // Reuses CastClaim wholesale so it inherits the same TTL heartbeat,
            // liveness check, never-published fail-closed split and reqParam
            // mirror as every other cast claim in this file -- one mechanism, not
            // a second one (its `spell` stays 0 and its `denyOnly` stays true for
            // the claim's whole life).
            CastClaim floor;
            // ── EQUIP AUTHORITY (ch.17, APMF v7; feat/mfo-equip-authority) ──
            // STANDING: no refresh stamp on purpose -- Tick() never sweeps it
            // (there is no cadence to size a window from; the claim ends only
            // by an explicit Release: OnFollowerRemoved, ClearTransientState,
            // or the bApmfEquipAuthority kill switch in Tick()). The declared
            // set itself is NOT mirrored here: Logistics_Economy.cpp owns the
            // last-sent copy (the change detector), this file only carries the
            // handle the declaration rides on.
            APMF_API::Handle equipAuthHandle = APMF_API::kInvalidHandle;
            // F2 (Fable round 2 on c66dc80): a kept handle is RE-VALIDATED with
            // v6 IsClaimLive once it is old enough to be in APMF's published
            // snapshot (kEquipAuthValidateAfter below) -- APMF's hotkey
            // release-all, its unload sweep, a New Game from a running session
            // and the bApmfEquipAuthority OFF->ON flip all leave MFO holding a
            // handle SetEquipSet silently no-ops on. `equipAuthFresh` = minted
            // since the last declaration went out on it; RefreshEquipDeclaration
            // reads it (through ClaimEquipAuthority's out-param) to drop its
            // change detector so the new handle gets a declaration at once.
            std::chrono::steady_clock::time_point equipAuthMintedAt{};
            bool equipAuthFresh = false;
            // ABI v9 (feat/mfo-equip-authority-v9): the SCOPE last SENT on this
            // handle (DeclareEquipScope), mirrored here ONLY for the direct
            // paths' EquipAuthorityOwns query (EquipTorch's left-hand gate) --
            // the set itself still lives in Logistics_Economy.cpp. Reset to
            // APMF's own default {All, 0} when a handle is minted: a fresh claim
            // carries no declaration, and APMF answers "no declaration -> allow"
            // until one goes out, so the query reads "owned" only from a SENT
            // scope (equipAuthFresh false).
            std::uint32_t equipOwned  = APMF_API::kEquipCat_All;
            std::uint32_t equipDenied = 0;
            // ── ch.8 CAST-SELECT GATE-ONLY CLAIM + ITS ALLOW-LIST (ABI v4;
            // fix/mfo-spell-authority-0922). See APMFBridge.h's block doc.
            // `selectHandle` is a gate-only kIntent_SelectSpell claim (param.form
            // == 0, so it drives nothing and names nothing); `selectList` is the
            // allow-set LAST SENT on it, kept here as the change detector so a
            // per-lap republish of an unchanged list is one map compare and no
            // enqueue. UNLIKE the ch.17 standing claim this one IS expiry-swept
            // (`selectRefreshed`, FacetExpiry()): the gate DENIES, so a follower
            // who stops being serviced must LOSE it, not keep it.
            APMF_API::Handle selectHandle = APMF_API::kInvalidHandle;
            std::vector<RE::FormID> selectList;
            std::chrono::steady_clock::time_point selectRefreshed{};
            // Mint stamp, for the SAME F2 re-validation the ch.17 claim does
            // (kEquipAuthValidateAfter): a handle younger than that is not in
            // APMF's published snapshot yet and IsClaimLive would read it as dead.
            std::chrono::steady_clock::time_point selectMintedAt{};
        };
        // The claim map and its refusal throttles (defined in apmf/Bridge.cpp).
        // *** ALL of them are guarded by g_mx. ***
        extern std::mutex                             g_mx;
        extern std::unordered_map<RE::FormID, Owned>  g_owned;
        extern std::unordered_set<RE::FormID>         g_equipAuthRefused;
        extern std::unordered_set<RE::FormID>         g_selectOverflow;
        extern std::unordered_set<RE::FormID>         g_selectRefused;
        // A just-minted ch.17 claim is not in APMF's published snapshot until
        // its next per-frame Drain, and IsClaimLive scans ONLY that snapshot --
        // so validating a handle younger than this reads a live claim as dead
        // and re-mints a duplicate. A floor sized from the cadence (one Drain
        // per frame; 2 s covers a stalled frame many times over), not a guess.
        inline constexpr auto kEquipAuthValidateAfter = std::chrono::seconds(2);

        // MFO outbids APMF's mid-range test hotkeys (basis 100) so a real gambit wins
        // the hand/target over a tester's Numpad keys. Arbitrary among clients; > 100.
        inline constexpr float kOwnBasis = 200.0f;

        // The shared claim helpers (defined in apmf/Bridge.cpp; see their doc comments
        // there). Caller holds g_mx for all of them.
        void EnsureClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                               APMF_API::Intent intent, APMF_API::Handle& handle,
                               RE::FormID& cur, RE::FormID want);
        void ReleaseClaimLocked(CastClaim& c);
        void EnsureCastClaimLocked(const APMF_API::APMF_API_v5* api, RE::FormID follower,
                                   CastClaim& c, RE::FormID wantSpell, RE::FormID wantTarget,
                                   std::int32_t wantHand, bool wantConc, std::uint32_t wantStopPct,
                                   bool wantDenyOnly = false);

        // Release one handle (thread-safe; no-ops a stale handle). Caller holds g_mx.
        // RE::FormID IS std::uint32_t (a plain alias, not a distinct type), so this
        // ONE overload also serves actionMask -- a second overload on that "different"
        // parameter type would be a duplicate-definition error, not a real overload.
        inline void ReleaseHandleLocked(APMF_API::Handle& handle, RE::FormID& cur) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && handle != APMF_API::kInvalidHandle) api->Release(handle);
            handle = APMF_API::kInvalidHandle;
            cur = 0;
        }

        // Drop the map entry once EVERY claim is gone. Caller holds g_mx.
        // ch.20 TARGET PIN table (apmf/Excursion.cpp, file-local, guarded by g_mx).
        // SweepTargetPinsLocked: Tick()'s once-per-pump liveness pass (marks a pin
        // Harbinger ended; releases every pin when bCommandTarget is flipped off).
        // ClearTargetPinsLocked: ClearTransientState's teardown. Both need g_mx HELD.
        void SweepTargetPinsLocked(const APMF_API::APMF_API_v2* api, std::chrono::steady_clock::time_point now);
        void ClearTargetPinsLocked();

        inline void EraseIfEmpty(std::unordered_map<RE::FormID, Owned>::iterator it) {
            const auto& o = it->second;
            if (o.targetHandle == APMF_API::kInvalidHandle &&
                o.packageHandle == APMF_API::kInvalidHandle && o.actionHandle == APMF_API::kInvalidHandle &&
                o.equipHandle == APMF_API::kInvalidHandle && o.heal.handle == APMF_API::kInvalidHandle &&
                o.offense[0].handle == APMF_API::kInvalidHandle &&
                o.offense[1].handle == APMF_API::kInvalidHandle &&
                // F10: an entry holding ONLY the idle-hand floor must survive --
                // erasing it would drop MFO's record of a claim that is still
                // standing on APMF's side, leaking it until its TTL. It cannot
                // normally happen (the floor is released in the same Tick() pass
                // that sees the last driving claim go), but a caller-driven
                // release ordering must not be able to create it.
                o.floor.handle == APMF_API::kInvalidHandle &&
                o.equipAuthHandle == APMF_API::kInvalidHandle &&   // ch.17 standing claim
                o.selectHandle == APMF_API::kInvalidHandle)       // ch.8 gate-only claim
                g_owned.erase(it);
        }

}
