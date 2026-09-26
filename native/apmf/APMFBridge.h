#pragma once
#include <RE/Skyrim.h>
#include <chrono>   // FacetExpiry()'s std::chrono::milliseconds return type
#include <vector>   // DeclareEquipSet's declared worn set
#include "APMF_API.h"   // APMF_EquipEntry -- DeclareEquipSet's entry type (ABI v8); EquipCategory bits (v9)
#include "Loadout.h"   // Loadout::HandPick -- HandFor's argument type below

// ─────────────────────────────────────────────────────────────────────────────
// APMF integration — MFO as an APMF client (Phase 3: the OWNED cast gambit). APMF
// (AI Package Management Framework) is a SEPARATE SKSE DLL; MFO talks to it ONLY
// through the byte-shared, append-only header `APMF_API.h` (the same discipline MFO
// applies to `MEO_API.h`) plus APMF's runtime interface query. APMF holds ZERO
// MFO-specific code; this bridge is the ONLY MFO code that knows APMF exists.
//
// THE MODERATOR MODEL (marth 2026-09-02). APMF ARBITRATES facets; it NEVER generates
// behaviour. MFO makes the behaviour with its OWN proven mechanisms and EXECUTES it;
// APMF only makes it WIN (single arbiter of the facet + suppresses competitors). So
// this bridge only ever CLAIMS facets — it never asks APMF to cast, command a target,
// or move a body (APMF would refuse; its channels are arbitration-only).
//
// The OWNED CAST, granularly: for a hostile cast gambit MFO CLAIMS two facets here —
//   * ClaimOffenseCast  (kIntent_Cast)         — "the AI's own seats cast THIS spell at
//                                                THIS target" (PORTED feat/offense-cast-
//                                                seats, 2026-09-05, off the retired ch.8
//                                                kIntent_SelectSpell gate-only claim this
//                                                site used to make — see the offense-cast
//                                                section below for why)
//   * ClaimCombatTarget (kIntent_CombatTarget) — "MFO owns this follower's combat target"
// and then, in Actuation::CastOn, EXECUTES the equip half itself: Loadout::Prepare puts
// the spell in the follower's LEFT hand, commands the target via `Targeting::Command`
// (currentCombatTarget), grants its own `CasterConsent`, and — while the kIntent_Cast
// claim stands — APMF's five engine vfunc seats drive the follower's OWN combat AI to
// select/equip/charge/aim/fire/channel EXACTLY that spell at EXACTLY that target: a real,
// fully-animated, MOBILE cast, and (unlike the retired gate-only claim) the gambit's named
// spell is the one that actually fires, not whatever the AI would have picked on its own.
// Crucially MFO does NOT claim the MOVEMENT facet, so the follower keeps kiting while it
// casts — that granular non-interruption is exactly why the cast routes through APMF. No
// forced cast on this path (see CAST-DELIVERY.md; force lives only in the legacy hybrid).
//
// THE IDLE-HAND FLOOR (F10, marth's design ruling 2026-09-08) — internal to
// APMFBridge.cpp, no entry point of its own, listed here because it is a THIRD claim
// this bridge makes and nothing else in the header would say so. Whenever MFO's driving
// cast claim(s) occupy exactly ONE of a follower's hands, MFO also claims the OTHER hand
// DENY-ONLY (APMF_API::kCastFlag_DenyHandOnly): a claim that drives nothing and admits
// nothing, so no un-gambited spell can arm there. APMF leaving an unclaimed hand
// permissive is correct for a general framework; MFO's design is that ONLY gambited
// spells occur, so "the other hand goes idle" is the specification, not a trade. It is
// ONLY ever a companion to a driving claim — MFO holding no cast claim floors NEITHER
// hand — and it is requested at the SAME kOwnBasis as every other claim here, because
// APMF's comparator makes a deny-only claim lose to a driving one at an equal basis, so
// MFO's own next gambit takes the hand with no release/re-request gap. Derived once per
// pump in Tick(); released with the other claims in ClearTransientState(). Full working
// (including why "neither" and not "both") at ReconcileHandFloorLocked in the .cpp.
//
// Claim lifecycles: casting = PER-CAST (refreshed each winning cast tick; released
// crisply by ReleaseOffenseCast the instant no cast rule holds). combat-target = PER-COMBAT
// (created by a cast directive, re-pointed via APMF Repoint when the foe changes, kept
// alive every in-combat tick by RefreshCombatTarget, released only at combat end via the
// expiry sweep) — so a cast->melee transition keeps the claim, it does not drop it.
//
// DEGRADE-WHEN-ABSENT. APMF is a declared prerequisite, but MFO must never hard-fail
// without it: Acquire() logs once and leaves the interface null; every call here
// no-ops when APMF is absent (or Config::g_apmfCast is off), and CastOn falls back to
// the LEGACY AI-first-wait + force-on-miss hybrid (also selectable via the MCM
// bLegacyCastHybrid toggle), so MFO's casting is byte-identical without APMF.
//
// THREADING (APMF's contract, APMF_API.h). Request/RequestEx/Release are safe from
// ANY thread — they copy POD and enqueue; APMF applies on the game thread. MFO's
// cast dispatch (CastOn) and the per-pump sweep (Tick) run on the AddTask job worker,
// so they may call these directly. This bridge's own claim map is guarded by a mutex
// (worker touches Select/Hold/Refresh/Release/Tick; kDataLoaded/kPreLoadGame touch
// Acquire/ClearTransientState).
//
// NO SAVE/CO-SAVE STATE. Cast ownership is runtime-only; nothing here is serialized.
// On kPreLoadGame ClearTransientState() drops every claim (APMF wipes its own control
// map there too, so a stale handle Release is a harmless no-op).
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::APMFBridge {

    // Fetch the APMF C-ABI interface. Call ONCE at kDataLoaded (APMF.dll is loaded
    // by then). Null / logs "absent" if APMF is not in the load order or too old.
    void Acquire();

    // Is the APMF interface live (present + ABI >= 2, so RequestEx is available)?
    bool Available();

    // ROUND-ROBIN-AWARE STALENESS WINDOW (APMFBridge.cpp, 2026-09-05; moved to
    // external linkage feat/cast-gambit-concentration so it has ONE definition
    // shared cross-TU). Every claim in this file that is refreshed from INSIDE
    // the per-follower Scheduler::Tick round-robin service (cast-select,
    // combat-target, weapon-order equipment, heal-cast) is only re-touched every
    // ~0.133s * partySize, not on a flat beat -- this returns the release window
    // sized for that gap (worst-case suppression + round-robin lap, floored at
    // 500ms), NOT the flat package-offer kExpiry. Reused verbatim by
    // Actuation.cpp's firing-spell-gambit lock (Task 2, #9: a floor is safe, a
    // guessed budget is not) as the staleness bound for the un-claimed direct-
    // force concentration case, so both budgets can never drift apart.
    std::chrono::milliseconds FacetExpiry();

    // Worker-safe (called from the pump, Diagnostics.cpp's SleeperLoop, beside
    // Tick()). No-ops instantly when Available() is true -- ZERO cost beyond
    // that one check whenever APMF is present, and the toast can NEVER fire in
    // that case. When APMF is absent AND Config::g_warnNoApmf is on, posts one
    // unobtrusive RE::DebugNotification corner toast (never a modal) reminding
    // the player MFO is running its legacy fallback: once ~30-60s after load,
    // then roughly every 10 minutes for as long as APMF stays absent. Cadence
    // state is plain session-relative timing (Rapport::SessionMinutes(), which
    // itself resets on load) -- no new co-save state, nothing serialized. The
    // actual DebugNotification call is routed through MainThread::Post since
    // this runs on the job worker, not the main thread.
    void MaybeWarnAbsence();

    // Hand-mode constant for the a_hand parameter both ClaimOffenseCast and
    // ClaimHealCast below take (2 = left, per APMF's ival encoding -- see
    // each function's own a_hand doc for the weapon-hand-exclusion rule this
    // implements). Declared HERE (moved up from beside ClaimHealCast) so it
    // is in scope for ClaimOffenseCast's default argument too -- named so a
    // call site reads as policy, not a bare magic 2.
    inline constexpr std::int32_t kApmfHandLeft = 2;

    // Explicit RIGHT-hand hint (feat/per-hand-cast-slots, 2026-09-06) -- MFO's
    // OWN encoding for a_hand, distinct from kApmfHandLeft/kApmfHandDualCast
    // and from the implicit "either/auto" of 0 (HandFor's EitherFree default,
    // for a caller that has not itself resolved a concrete hand). A caller
    // that HAS resolved "this request uses the right hand" (Actuation.cpp's
    // per-hand cast-lock juggle -- see ResolveCastHand) should pass this
    // constant rather than bare 0, so ClaimOffenseCast's slot selection below
    // is explicit rather than "whatever falls out of the default." Translates
    // to the SAME CastFlags encoding 0 already produced (no kCastFlag_LeftHand
    // -> APMF's own "hand hint (default right)"), so this is a bookkeeping
    // distinction on MFO's side, not a new engine-facing bit.
    inline constexpr std::int32_t kApmfHandRight = 1;

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every
    // other accessor here takes). Does `a_follower` currently hold a LIVE
    // offense kIntent_Cast claim on ANY hand (i.e. is APMF's engine-seat drive
    // actively casting this follower's gambit spell right now, left or
    // right)? REPOINTED (feat/offense-cast-seats, 2026-09-05) from the retired
    // ch.8 kIntent_SelectSpell gate-only claim this accessor used to back --
    // same name, same call sites, now backed by ClaimOffenseCast's kIntent_Cast
    // handle(s) instead (Owned::offense[2], APMFBridge.cpp -- feat/per-hand-
    // cast-slots made this PER-HAND internally, but this accessor's own
    // per-actor "is ANY offense claim live" contract is UNCHANGED, since its
    // two consumers below only ever cared about the actor, never the hand).
    // Two independent consumers: (1) CasterConsent's own exclusivity/hard-abort
    // standdowns (folded into `ClientCastClaimed` too, feat/offense-cast-seats)
    // and (2) CombatStyle's equip-gate standdown -- both still correct under
    // kIntent_Cast, since a live claim ALSO answers the 0x0F CheckShouldEquip
    // seat (APMF_API.h), covering exactly what those two standdowns used to
    // rely on ch.8 for. Does NOT affect CasterConsent's own consent-GRANT (the
    // veto-removal that lets the AI consider casting at all) -- that stays
    // MFO's, APMF only ever narrows/drives, never invents unrequested consent.
    bool IsOwnedCastActive(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (SAME g_mx). Does `a_follower` currently
    // hold a LIVE offense kIntent_Cast claim on THIS SPECIFIC hand
    // (a_hand == kApmfHandLeft -> the left slot, anything else -> the right
    // slot -- matches ClaimOffenseCast's own slot-selection rule below; a
    // DualCast claim mirrors into BOTH slots, so it answers true for either
    // hand queried)? NEW (feat/per-hand-cast-slots, 2026-09-06) for
    // Actuation.cpp's per-hand cast-gambit lock (CastLockLive), which needs to
    // know whether ONE hand specifically is occupied, not just "the actor
    // has some offense claim somewhere" (IsOwnedCastActive's job, unchanged
    // above). Heals are LEFT always (ClaimHealCast's own hard rule) and are
    // NOT reflected here -- a caller that also cares about the heal facet
    // ORs this with IsHealCastActive itself (Actuation.cpp does, for the left
    // hand only).
    bool IsOwnedCastActiveOnHand(RE::FormID a_follower, std::int32_t a_hand);

    // Worker- AND combat-thread-safe (SAME g_mx). The delivery-flip proxy FormID
    // APMF minted internally for a_follower's LIVE offense kIntent_Cast claim on
    // THIS hand (a_hand selects the slot exactly like IsOwnedCastActiveOnHand
    // above), or 0 if there is no live claim on that hand, its handle minted no
    // proxy, or the resolved APMF interface is ABI < 6 (APMF_API::GetCastProxy is
    // a v6 slot). Cast-claim observability (2026-09-06): lets a caller that just
    // claimed the offense facet (Actuation.cpp) hand this proxy to
    // ComposedCast::WatchClaim so the silent-claim diagnostic recognises a cast
    // of the proxy, not only the original spell, as this claim actually firing.
    RE::FormID GetOffenseCastProxy(RE::FormID a_follower, std::int32_t a_hand);

    // ── offense-cast facet CLAIM: PER-CAST, TTL-bounded, PER-HAND (ch.8b, APMF v5) ──
    // PORTED (feat/offense-cast-seats, 2026-09-05) off the retired ch.8
    // kIntent_SelectSpell gate-only claim (`ClaimCasting`/`ReleaseCasting`,
    // removed) this call site used to make: that channel only ARBITRATEs +
    // DENIES a competing framework's own spell selection -- the follower's
    // OWN AI still picked whichever spell IT wanted, the long-standing
    // "target right, spell wrong" defect (see the module doc block's cast
    // gambit history). This claim instead rides the SAME kIntent_Cast facet
    // `ClaimHealCast` uses (full engine-seat shape documented on that
    // function below) -- while it stands, APMF's five seats drive the
    // follower's OWN combat AI to select/equip/charge/aim/fire/channel THIS
    // spell at THIS target natively, closing the defect: the gambit's named
    // spell is now the one that actually fires.
    //
    // PER-HAND (feat/per-hand-cast-slots, 2026-09-06). The parallel APMF
    // change makes kIntent_Cast arbitrate PER (actor, hand), so TWO
    // independent offense claims -- one LEFT, one RIGHT -- can now stand on
    // ONE follower at once: `Owned::offense[2]` (APMFBridge.cpp), index 0 =
    // left, 1 = right, selected by a_hand below. No API change on APMF's
    // side -- this is just calling RequestCast twice and holding two handles.
    // A DualCast claim (a_hand == kApmfHandDualCast) is ONE underlying APMF
    // claim (a single RequestCast with kCastFlag_DualCast) that MIRRORS its
    // handle/metadata into BOTH slots, so a hand-availability query on either
    // index sees it occupied; releasing/changing either slot tears the
    // mirrored pair down together (never a stray half-release of a shared
    // handle). Still distinct from `Owned::heal` (Task 3's "heal and offense
    // stay two distinct claims" invariant, preserved) -- heal and offense
    // casts are mutually exclusive per tick by `CasterConsent::SpellKind`
    // (never concurrent on one follower) but never share state, so neither
    // claim's release/refresh cross-talks the other.
    //
    // a_hand: kApmfHandLeft -> the left slot; kApmfHandDualCast -> both slots
    // (mirrored, ONE handle); anything else (kApmfHandRight, or bare 0) ->
    // the right slot. The caller is expected to have ALREADY resolved a
    // concrete hand (Actuation.cpp's per-hand cast-lock juggle, via
    // `Loadout::PlanCastHand` + its own `ResolveCastHand`) -- this function
    // does not itself arbitrate "which hand is free," it only routes to the
    // named slot. The weapon-hand-exclusion hard rule (a weapon owning the
    // right hand -> spells claim LEFT, unconditionally, dual-cast never
    // requested) is enforced UPSTREAM inside `Loadout::PlanCastHand` itself,
    // before a_hand is even chosen -- this function has no weapon-state
    // awareness of its own and trusts the caller's resolved value, same as
    // it always has for the single-hand-LEFT case.
    // a_concentration: pass true only for a held-stream offense cast that
    // reaches this claim -- today's call sites (`Actuation::CastOn`'s
    // owned-cast branch, always LEFT/RIGHT/DualCast per the juggle; and
    // Actuation_Direct.cpp's self/target concentration-offense streams,
    // always LEFT) either always pass false or always pass true per their own
    // delivery model (concentration forks off before ownedCast, so the two
    // never mix at one call site). a_stopPct: a HEAL-ONLY concept (a client
    // restore threshold read by seat 0x07); offense has no such threshold in
    // scope -- always 0.
    //
    // Worker-safe. CREATE-OR-REFRESH: call every tick the gambit still wins --
    // a repeat call with the SAME (spell, target, hand, concentration,
    // stopPct) is a cheap refresh; a CHANGE releases and re-requests (no
    // in-place re-point on RequestCast's rich payload). Returns whether
    // a_follower now holds a LIVE claim on the SLOT this call resolved to
    // (false -> caller falls back to the legacy AI-first-grace + force-on-miss
    // hybrid). No-op (returns false) when APMF is absent, its resolved interface
    // has no RequestCast slot (ABI < 5), or Config::g_apmfCast is off.
    //
    // A `false` IS AMBIGUOUS ON ITS OWN and callers must NOT treat it as one
    // condition (fix/mfo-no-decline-fallback, marth 2026-09-07: "without APMF,
    // it needs to just work, whatever unpolished way works. With APMF theres no
    // fallback for it. APMF shoudl work"). Pair it with
    // `OffenseCastClaimSupported()` below to split the two halves of that rule:
    //   * false + NOT supported  -> APMF (or the facet, or the toggle) is ABSENT.
    //     The legacy AI-first-grace + force-on-miss hybrid is the SUPPORTED
    //     degrade contract -- run it, byte-identical to a world with no APMF.
    //   * false + supported      -> APMF is PRESENT and CAPABLE and said NO.
    //     That is a BUG TO FIX IN APMF, never a condition to degrade through:
    //     FAIL CLOSED, log loudly, do NOT route around it into the legacy path.
    bool ClaimOffenseCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                          std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                          std::uint32_t a_stopPct = 0);

    // Worker-safe. Is the offense-cast CLAIM CHANNEL itself available right now --
    // i.e. would a `false` from ClaimOffenseCast above mean "APMF REFUSED" rather
    // than "there was nothing to ask"? True iff APMF is present AND its resolved
    // interface actually carries the v5 RequestCast slot AND Config::g_apmfCast is
    // on. Deliberately mirrors ClaimOffenseCast's own three non-arbitration early
    // returns (they sit beside each other in APMFBridge.cpp and read the SAME
    // `g_apmf`/`abiVersion`/toggle, so the two cannot drift); it does NOT re-check
    // the caller-controlled argument guards (a_follower/a_spell != 0), which every
    // call site already satisfies by construction.
    bool OffenseCastClaimSupported();

    // Worker-safe. Release the offense-cast claim(s) now (the combat-target claim, if
    // any, is left alone) on BOTH hands -- releasing one hand must never disturb the
    // other's independent claim, so a caller that wants only ONE hand released again
    // resolves and re-Claims that hand alone rather than calling this (today's two
    // call sites -- Scheduler's !castSeen and Followers' dismissal teardown -- both
    // mean "nothing is wanted at all anymore", so releasing everything is correct for
    // them). A mirrored DualCast claim is released exactly once (its shared handle is
    // torn down as the single claim it is, not double-released). Call the instant no
    // cast rule holds (Scheduler !castSeen) so the claim releases crisply, not after
    // the round-robin-aware expiry backstop.
    // Worker-safe. REFRESH the cast claim(s) MFO already holds on `a_hand` --
    // without re-requesting anything (F9, 2026-09-08). This is the "my cast is
    // already running, I only need to keep it alive" call, made by Actuation's
    // satisfied-in-flight gate instead of re-entering the whole claim/equip/
    // consent path for a rule that has nothing new to ask for.
    //
    // WHAT IT DOES: replays each live claim's OWN stored (spell, target, hand,
    // concentration, stopPct, deny-only) tuple through the same create-or-refresh
    // helper every claim call uses. Because the tuple is by construction identical
    // to what is stored, the identity compare matches: the TTL is renewed in place
    // (the ABI-6 liveness check, the lazy delivery-flip proxy read, the heartbeat
    // Repoint, and the `refreshed` stamp Tick()'s FacetExpiry sweep reads). It can
    // never change a hand mode, never re-point a target, and never tear down a
    // claim that is still in force.
    //
    // IT CAN STILL MINT A HANDLE, and saying otherwise would be false: a stored
    // handle APMF has already auto-expired takes the aged-out branch and is
    // RE-REQUESTED from here -- correct (the rule still wants it), and not a
    // concurrent second claim (the dead handle is dropped first; one claim exists
    // at every instant, and there is no live charge left to interrupt).
    //
    // a_hand: kApmfHandLeft -> the left offense slot AND the heal slot (heals are
    // LEFT always, and the two can stand together); kApmfHandDualCast -> the
    // mirrored dual claim, re-mirrored so both slots' stamps move together;
    // anything else -> the right offense slot.
    //
    // RETURNS true when at least one claim on that hand is still live afterwards.
    // A `false` means the claim really is gone (APMF aged it out, or the
    // fail-closed never-published split dropped it) -- the caller must then fall
    // through to its normal claim path, NOT treat the rule as satisfied. It is not
    // an APMF refusal and must never be logged as one.
    //
    // `a_holdSpell` (2026-09-08): NON-ZERO marks this a HOLD refresh -- feeding a
    // claim whose own rule is being HELD OFF (Actuation's cast-hand lock holding a
    // re-aim) rather than one whose (spell,target) still matches -- and names the
    // ONE spell being held for. Three consequences, all the caller's to respect:
    //   * it REFUSES on ABI < 6, for the same reason RefreshHealCastClaim does (no
    //     IsClaimLive to ask, so the refresh would bump MFO's stamp for a claim
    //     APMF may have expired and stop the only sweep that could end the hold);
    //   * only claims NAMING that spell are replayed. On the LEFT hand an offense
    //     claim and a heal claim can stand together, and a held offense re-aim
    //     renewing a coexisting heal claim would slip that heal past its own
    //     never-observed cap -- that cap gates RefreshHealCastClaim's answer and
    //     reads `created`, while the claim's LIFE is `refreshed`, which this bumps;
    //   * the CALLER must bound how long it keeps calling. A claim that can never
    //     fire must not be renewed forever, which is what kHealHoldNeverObservedMs
    //     bounds on the heal side, and the caller's evidence for "still firing"
    //     must be RECENT (ComposedCast::ObservedFiring's window) -- the raw
    //     `observed` latch is never cleared by any offense release path.
    // 0 (the default) = the in-flight caller: every claim on the hand, no refusal.
    bool RefreshOwnedCastOnHand(RE::FormID a_follower, std::int32_t a_hand,
                                RE::FormID a_holdSpell = 0);

    void ReleaseOffenseCast(RE::FormID a_follower);

    // Worker-safe. Release ONLY the cast claim(s) standing on ONE hand -- the other
    // hand's independent claim, and every non-cast facet, are untouched. Added for
    // RANK PREEMPTION (marth 2026-09-08: the gambit list order IS the priority
    // order, and a higher-ranked rule reaching the cast path must be able to TAKE a
    // hand from a lower-ranked incumbent rather than wait for its condition to go
    // false). ReleaseOffenseCast above cannot serve that: it is whole-follower by
    // design, and using it would drop the OTHER hand's unrelated claim as
    // collateral.
    //
    // a_hand: kApmfHandLeft -> the left offense slot; anything else -> the right
    // offense slot. **The HEAL slot is deliberately NOT touched, on either hand.**
    // A heal claim is bookkept by ComposedCast (its [cfc] watch slot, its CastBounds
    // arm, its hold record), so releasing the APMF claim from under it would leave
    // all three armed for a claim that no longer exists -- `ComposedCast::End` is
    // the seam that takes them down together, and a caller displacing a heal must
    // go through it. (This function used to drop `heal` unconditionally for the LEFT
    // hand, which also meant an offense claim being displaced took an unrelated
    // coexisting heal with it.) `GetHealCastSpell` below is how a caller tells the
    // two apart.
    //
    // A MIRRORED DUALCAST CLAIM IS RELEASED WHOLE from either hand -- one APMF
    // handle occupying both, arbitrated as one claim, with no half-release to make.
    // Preempting one hand of a dual cast ends that dual cast, deliberately.
    void ReleaseCastClaimOnHand(RE::FormID a_follower, std::int32_t a_hand);

    // ── combat-target facet CLAIM: PER-COMBAT ────────────────────────────────────
    // Worker-safe. CLAIM the combat-target facet for this follower (APMF records the
    // owner; the intended `a_target` rides along). This does NOT command the target —
    // MFO commands it via Targeting::Command itself. RE-POINTS the claim in place when
    // the target changes (same handle, via APMF Repoint — a mid-battle retarget is not a
    // release). `a_create`: true creates the claim if none exists yet — both a CAST
    // directive and an Attack directive pass true, so a pure-melee follower gets the
    // same arbitration a caster does (Actuation.cpp's kActAttack handler); pass false to
    // re-point ONLY an existing claim without ever creating one. No-op when APMF is
    // absent or Config::g_apmfCast is off.
    void ClaimCombatTarget(RE::FormID a_follower, RE::FormID a_target, bool a_create);

    // Worker-safe. Keep the follower's EXISTING combat-target claim alive (refresh its
    // expiry timestamp only — no create, no re-point). Call every in-combat service tick
    // so the claim persists across non-targeting rules (potion/flee/wait) for the WHOLE
    // fight; it is released by the expiry sweep only once this STOPS being called (i.e.
    // combat ended). No-op if the follower holds no combat-target claim.
    void RefreshCombatTarget(RE::FormID a_follower);

    // ── ch.20 TARGET PIN (ABI v13, kIntent_TargetPin): MFO's foe choice, HELD ──────
    // Harbinger answers the engine's own combat-target selector (vtable slot 6) with the
    // pinned foe, so the engine's pick never reaches the follower. This is the EXECUTING
    // twin of ClaimCombatTarget above (ch.6 stays arbitration-only and unchanged).
    // Only Targeting.cpp calls these: Targeting::Command / Clear / ClearAll / Current
    // route here when Targeting's pin route is on (see Targeting.h), so every foe-target
    // caller is converted at that one choke point. Worker-safe (the tick's AddTask
    // worker, the same thread every other claim in this file is filed from); takes g_mx.
    //
    // THE PIN ENDS ON HARBINGER'S SIDE (target lost / dead / disabled / unloaded /
    // unresolvable, or the follower dead): IsClaimLive turns false. (An OUTRANKED claim
    // is NOT ended -- Harbinger's Poll ends only the winner -- so it stays live.)
    // Tick()'s sweep marks it ENDED and Releases the handle (a no-op on a dead claim;
    // FIFO-cancels a still-pending one, so no orphan can survive a false judgement).
    // THE NO-LOOP RULE: when the gambit re-chooses an ended pin's foe, MFO re-pins it only
    // if that pin had gone LIVE -- MFO's foe selector skips dead / disabled / lost /
    // unloaded foes, so being re-chosen means MFO sees it trackable again. A pin that
    // never went live returns Suppressed until the gambit chooses a DIFFERENT foe (a fresh
    // choice) or the pin is released (INTEGRATION.md "How the pin ends").
    enum class PinResult {
        Pinned,       // a NEW pin was filed (first pin, a changed target, or a re-pin)
        Unchanged,    // already pinned (or pending) on this target: not an action
        Suppressed,   // an ended pin that never went live, on this same target: not re-pinned
        Invalid,      // no follower / no target / self-target: nothing filed
        SeatAbsent,   // APMF absent, ABI < 13, or a synchronous refusal (= ch.20 seat not
                      // installed -- the only synchronous refusal MFO's valid params can hit)
    };
    // True when APMF is present at ABI >= 13 -- two of ch.20's three availability gates.
    // The third (the seat is installed) is only observable as a synchronous refusal of
    // the first pin (APMF ControlMap::EnqueueRequest), reported as PinResult::SeatAbsent.
    bool TargetPinOffered();
    PinResult PinTarget(RE::FormID a_follower, RE::FormID a_target, RE::ActorHandle a_targetHandle);
    // Release this follower's pin (live or ended) and forget it. No-op when none.
    void ReleaseTargetPin(RE::FormID a_follower);
    void ReleaseAllTargetPins();
    // The pinned foe while the pin stands (live, or filed and not yet drained); an
    // EMPTY handle when there is none or Harbinger ended it.
    RE::ActorHandle PinnedTarget(RE::FormID a_follower);
    // Pins currently standing (not ended) -- the Diagnostics targeting line.
    std::size_t TargetPinCount();

    // ── ch.21 COMBAT ENTRY (ABI v14, kIntent_CombatEntry): the engage-on-sight gambit ──
    // Harbinger calls the engine's own Actor::StartCombat(target) ONCE, on the game thread,
    // one frame after the claim is applied (Harbinger INTEGRATION.md "Starting a fight").
    // MFO's only client is the hidden "nearest visible enemy" out-of-combat gambit
    // (EngageOnSight.cpp, called from the Scheduler's party-OOC branch; ClickUp 86e3errnu); there is NO MFO direct road:
    // Harbinger absent or older than v14 = the gambit is inert.
    // THE CLAIM ENDS ON HARBINGER'S SIDE: engine refused / entry not attempted / combat
    // ended / owner or target dead, disabled, unloaded, unresolvable. IsClaimLive turns
    // false; Tick()'s sweep marks the entry ENDED and Releases the handle (a no-op on a dead
    // claim; FIFO-cancels a still-pending one). MFO never Repoints it: a new target is a
    // NEW request, and an ended entry is only ever forgotten, never re-filed by this table
    // (EngageOnSight's per-follower given-up set is the no-loop rule).
    // Worker-safe (the tick's AddTask worker); takes g_mx.
    enum class EntryResult {
        Filed,        // a NEW entry claim was filed; *a_outHandle carries it
        Standing,     // this follower already holds an entry (live, pending or ended-unconsumed)
        Invalid,      // no follower / no target / self / the player: nothing filed
        SeatAbsent,   // APMF absent, ABI < 14, or a synchronous refusal (VR, runtime,
                      // [CombatEntry] bCombatEntry=0, self-check) -- session-stable
    };
    enum class EntryState {
        None,         // no entry held for this follower
        Standing,     // filed and not ended (live, or not yet drained by Harbinger)
        Ended,        // Harbinger ended it (the sweep saw IsClaimLive false); not yet forgotten
    };
    // True when APMF is present at ABI >= 14 AND no synchronous refusal has been seen this
    // session (the ch.21 seat is installed as far as MFO can tell).
    bool CombatEntryOffered();
    EntryResult RequestCombatEntry(RE::FormID a_follower, RE::FormID a_target, std::uint32_t* a_outHandle);
    // The follower's entry state; *a_outTarget (may be null) = the entry's target.
    EntryState CombatEntryStateOf(RE::FormID a_follower, RE::FormID* a_outTarget);
    // Release (if still standing) and forget this follower's entry. No-op when none.
    // Release STOPS NOTHING (Harbinger's contract): the fight is the engine's.
    void ForgetCombatEntry(RE::FormID a_follower);

    // ── ch.22 COMBAT RE-ENTRY DENY (ABI v15, kIntent_CombatReentryDeny): the retreat ──────
    // apmf/ReentryDeny.cpp. Harbinger's recipe "retreat = StopCombat once + deny re-entry":
    // the retreat claims the deny, the handle stays PENDING until IsClaimLive reads true, and
    // ONLY THEN is the single StopCombat posted (Packages::RetreatReengage, the retreat's own
    // main-thread road) by Tick()'s sweep. Worker-safe; its own mutex (never g_mx).
    // ClaimRetreatReentryDeny: true = filed (the caller must NOT post its own StopCombat --
    // the sweep does, on live); false = Harbinger absent, ABI < 15, or the seat refused
    // (session-stable) -> the caller keeps the shipped immediate StopCombat.
    enum class DenyState {
        None,      // no deny for this follower (never claimed, or released)
        Pending,   // filed, not yet read live (StopCombat not posted yet)
        Live,      // read live at least once and not ended (StopCombat posted)
        Ended,     // Harbinger ended it (window elapsed / owner dead) after it was live
        Degraded,  // it NEVER went live (a Harbinger apply failure): this retreat is DEGRADED to
                   // the per-re-entry StopCombat (review of 497b6cd SEV-4 #2; marth's policy call pending)
    };
    bool      ClaimRetreatReentryDeny(RE::FormID a_follower);
    DenyState RetreatReentryDenyStateOf(RE::FormID a_follower);
    // Release (if held) and forget. No-op when none, so every retreat end may call it.
    void      ReleaseRetreatReentryDeny(RE::FormID a_follower, const char* a_why);

    // ── ch.23 PURSUIT LEASH (ABI v16, kIntent_PursuitLeash): the in-combat leash ──────────
    // apmf/PursuitLeash.cpp. Anchor = the player, radius = what the Scheduler passes
    // (Confidence::LeashRadius). ServicePursuitLeash files the claim on the first call and
    // Repoints it ONLY when the radius leaves a band around the claimed one (no per-tick
    // churn); an ended / never-live claim is not re-filed until ReleasePursuitLeash (the
    // fight's end). No-op when Harbinger is absent, ABI < 16, or the seat refused.
    // Worker-safe; its own mutex (never g_mx).
    void ServicePursuitLeash(RE::FormID a_follower, float a_radius);
    void ReleasePursuitLeash(RE::FormID a_follower, const char* a_why);

    // Worker-safe. Once-per-pump sweep: release each claim not refreshed within its
    // expiry window (offense-cast backstop; combat-target = combat-end detector).
    // offense-cast/combat-target/weapon-order-equipment/heal-cast all use the
    // round-robin-aware FacetExpiry() (APMFBridge.cpp, anon ns) -- proven
    // round-robin-bound, not flat-cadence (2026-09-05: first the heal claim, then
    // the equipment claim, deck-captured); package-offer alone stays on the flat
    // kExpiry (proven genuinely flat-refreshed, Packages::Pump()); combat-action-
    // deny is unwired and inherits the flat backstop pending real evidence. Call
    // from the pump body.
    void Tick();

    // ── weapon-order EQUIPMENT facet CLAIM: PER-ORDER (ch.15) ───────────────────
    // Retires MFO's native weapon-order equip gate (CombatStyle.cpp's EquipGateThunk)
    // onto APMF's T2a CheckShouldEquip hook, mirroring the cast-select/CasterConsent
    // retirement above -- MFO still EXECUTES the force-equip itself (ActorEquipManager
    // forceEquip, Actuation.cpp's EquipWeapon/g_forcedWeapon); this claim only makes
    // APMF's OWN gate enforce "no spell/staff re-arm while the order holds the hands"
    // instead of MFO's own gate doing it natively. GATE-ONLY: APMF makes no engine
    // write for a param'd Equipment claim (APMF_API.h's kIntent_Equipment comment).
    //
    // Worker-safe. CLAIM (or refresh-in-place) the equipment facet for a_follower,
    // naming a_weaponForm (kIntent_Equipment, param.form = the forced weapon's
    // FormID). Call every tick the force-hold survives (Actuation::ReconcileForcedWeapon
    // when it decides to KEEP the hold) -- same "the claim call also refreshes" idiom
    // ClaimOffenseCast uses, so one call both engages a new hold and keeps an existing one
    // alive under the round-robin-aware FacetExpiry() backstop (2026-09-05: this
    // facet used to share the flat kExpiry and a deck capture on Cicero proved that
    // dropped a still-wanted claim ~919ms in -- ReconcileForcedWeapon runs from the
    // SAME per-follower Scheduler::Tick round-robin lap as cast-select, not a tight
    // beat, so it needed the same fix). RE-POINTS in place (same handle) if the held
    // weapon FormID changes (a melee<->ranged flip re-forces a different weapon).
    // No-op when APMF is absent or Config::g_weaponStyleControl is off.
    void ClaimEquipment(RE::FormID a_follower, RE::FormID a_weaponForm);

    // Worker-safe. Release the equipment claim. Call the instant the force-hold
    // actually releases (Actuation::ReleaseForcedWeapon, its single choke point --
    // reached from ReconcileForcedWeapon's release branch AND every teardown site
    // that force-unequips directly) so MFO's native gate re-enforces immediately if
    // APMF is absent, with no gap between "hands freed" and "gate re-armed".
    void ReleaseEquipment(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every other
    // accessor here takes, matching IsOwnedCastActive's shape exactly). Does
    // `a_follower` currently hold a LIVE equipment facet claim? CombatStyle.cpp's
    // EquipGateThunk consults this to stand its OWN weapon-order deny down (APMF's
    // T2a now owns that enforcement) -- independent of, and checked alongside,
    // IsOwnedCastActive's existing cast stand-down; either, both, or neither may be
    // true for a given follower on a given tick. Returns false (native gate keeps
    // enforcing, byte-identical to pre-APMF) whenever APMF is absent, the claim was
    // never made, or it already expired.
    bool IsEquipmentClaimActive(RE::FormID a_follower);

    // Worker-safe (no lock of its own beyond IsEquipmentClaimActive's;
    // Loadout::Read is a plain form/equipped-object read, the same
    // discipline every other Loadout call already runs under on this tick).
    // Does a WEAPON currently own a_follower's RIGHT hand -- either because
    // Loadout::Read sees one equipped there RIGHT NOW (Grip::OneHanded/
    // TwoHanded), or because IsEquipmentClaimActive says an equip gambit holds a live
    // force-equip claim on one and will reassert it shortly even if the hand
    // reads momentarily empty (the exact race ClaimHealCast's hand rule, and
    // Loadout::PlanCastHand's a_weaponHandActive parameter, exist to close --
    // see both docs). The canonical "does a weapon own this hand" signal for
    // ANY cast hand-selection on the APMF-owned path; a_follower == nullptr
    // returns false.
    bool WeaponHandActive(RE::Actor* a_follower);

    // ── package-offer facet CLAIM: PER-EXCURSION (ch.9, T3 0x49) ────────────────
    // Worker-safe. CLAIM the package-offer facet for a_follower, naming a_packageForm
    // (kIntent_OfferPackage, param.form). While this claim is held, APMF's 0x49 hook
    // (Actor::CheckForCurrentAliasPackage) hands a_follower this package DIRECTLY,
    // unconditionally -- no alias fill, no quest-priority race, so a follower who is
    // package-locked by an outranking custom AI framework (the Cicero case) still
    // gets it. The CALLER owns the package's own runtime target (Packages.cpp writes
    // a targType-0 runtime handle into it BEFORE calling this) -- this bridge only
    // ever names the FormID. RE-POINTS in place (same handle) on a repeat call with a
    // DIFFERENT form; a repeat call with the SAME form is a cheap refresh (matches
    // EnsureClaimLocked's existing unchanged-claim fast path). No-op when APMF is
    // absent or Config::g_apmfLootTravel is off.
    //
    // Returns whether a_follower now holds a LIVE package-offer claim (false on a
    // no-op no-APMF call, OR on a live APMF that REFUSED the claim -- e.g. it lost
    // arbitration to a higher-basis client, or no channel serves the intent on an
    // older ABI). MFO IS AN APMF SHOWPIECE (Docs/STATUS.md): the caller uses this
    // to LOG a refusal loudly, never to silently fall back to a pre-APMF route --
    // with APMF present, the APMF path is committed, not a first-try-then-decline.
    bool OfferPackage(RE::FormID a_follower, RE::FormID a_packageForm);

    // Worker-safe. Release ONLY the package-offer claim (any combat-action-deny claim
    // held for the same follower, if any, is left alone -- release it separately).
    // Call the instant the excursion ends (arrival / loot done / abandoned) so the
    // follower's framework package resumes immediately, not after the expiry backstop.
    void ReleaseOfferPackage(RE::FormID a_follower);

    // ── ch.19 TRAVEL facet: LOOT-TRAVEL ROAD 2 (APMF ABI v10, A/B ONLY) ─────────
    // test/mfo-loot-travel-via-ch19 (2026-09-22), gated by
    // Config::g_lootTravelViaApmfTravel (bLootTravelViaApmfTravel, DEFAULT OFF).
    //
    // WHAT THIS IS. The ch.9 road above needs MFO to ship its OWN travel package
    // records (MFO_APMFLootTravelPackage0..3) and to write their Near-Reference
    // location itself. APMF v0.9.5's ch.19 kIntent_Travel does that whole job with
    // APMF's own records (Data/APMF.esl, EIGHT slots): the client names the
    // DESTINATION and APMF points its package, writes the location, files an
    // internal ch.9 offer at the client's basis, and ENDS the leg on arrival, on the
    // actor entering combat, or on the destination dying/being disabled/deleted.
    // So this is not a second claim beside the ch.9 one -- it REPLACES it for the
    // excursion that took this road. THE TWO ROADS ARE NEVER BOTH LIVE FOR ONE
    // FOLLOWER: on this road Logistics_Loot.cpp does not call
    // Packages::LootTravelFill/Retarget at all, so OfferPackage above is never
    // called for that excursion and MFO holds no ch.9 claim of its own.
    //
    // WHY IT IS KEYED BY LOOT SLOT, NOT BY FOLLOWER. The release edges all funnel
    // through Packages::LootTravelClear, and Logistics.cpp's sweeps call it with
    // a_follower == nullptr and only the slot index. The slot is therefore the only
    // key every release edge actually has.
    //
    // ABI. kIntent_Travel is ABI v10; MFO's byte-shared APMF_API.h mirror is at v10
    // (byte-identical to APMF's). The wrapper still gates on the LIVE
    // api->abiVersion >= 10 as defence.

    // Worker-safe. CLAIM (or RE-POINT, same handle) the ch.19 travel facet for
    // a_follower with a_destRef as the destination and a_radius as the arrival
    // radius in game units (APMF clamps to [50, 512] and logs any clamp; the value
    // in force is also written into the package's own stop radius, so the engine's
    // idea of "arrived" and APMF's cannot drift). a_slot is MFO's loot slot, which
    // owns the leg record. A repeat call on the same slot with a DIFFERENT
    // destination RE-POINTS in place (no release/re-claim churn) -- that is the
    // retarget road; a repeat call with the SAME destination is a cheap no-op.
    //
    // Returns whether the slot now holds a LIVE ch.19 claim. FALSE means APMF
    // REFUSED (absent, below ABI v10, Data/APMF.esl missing or disabled,
    // [Travel] bTravel=0, VR, a zero destination, or all eight APMF travel slots
    // busy) -- the ONE case where the caller falls back to the ch.9 control road,
    // because a refusal means APMF will do nothing whatsoever and "no looting" is
    // not an acceptable answer to it. Logged once per refusal reason class.
    bool ClaimLootTravel(RE::FormID a_follower, RE::FormID a_destRef, int a_slot, float a_radius);

    // Worker-safe. TRUE while a_slot holds a live ch.19 loot-travel claim. This is
    // what makes the A/B switch safe to flip mid-session: the road is chosen only at
    // a fresh DISPATCH, and a leg already in flight is retargeted (and released) on
    // whichever road started it, whatever the switch now says.
    bool HasLootTravelLeg(int a_slot);

    // Worker-safe. Release a_slot's ch.19 claim if it holds one; returns whether it
    // did. A no-op (false) on the ch.9 control road, which is what lets
    // Packages::LootTravelClear call it unconditionally at every release edge.
    // APMF never revokes a claim the client still holds -- it only ends the work it
    // started (Travel.cpp EndLeg) -- so this call is MANDATORY on every end of an
    // excursion that took this road, including the ends APMF reached first
    // (arrival, combat, destination gone, its own 120 s abandon).
    bool ReleaseLootTravelSlot(int a_slot);

    // Worker-safe. Release every ch.19 loot-travel claim held for a_follower,
    // whatever slot it sits in (the dismissal / retreat-preempt edge, which knows
    // the actor and not the slot). Returns how many it released.
    int ReleaseLootTravelFor(RE::FormID a_follower);

    // ── ch.19 LEG STATE (loot M2, APMF ABI v12 GetTravelLegState) ───────────────
    // Worker-safe (APMF's read is a mutex snapshot, "safe from any thread"; the
    // slot's handle/dest/follower are copied under g_mx and the call is made
    // OUTSIDE it). Returns FALSE when there is nothing to read: no APMF, an APMF
    // below ABI v12 (MFO requests 10; APMF hands back its newest struct and
    // abiVersion says how far it goes), or a_slot holds no ch.19 leg. The caller
    // then keeps its own observation (the Movement Blocked timer): that is the
    // documented degrade, not a mask.
    //   `ours`  = the state is about THIS slot's claim and destination
    //             (ownerHandle == our handle, or 0 while APMF has not composed it
    //             yet; destForm == our destination) AND it is not the state that
    //             stood before our own latest RequestEx/Repoint (its seq differs
    //             from the one read just before that call), so an ended leg's end
    //             is never re-read as the end of the leg that replaced it.
    //   `state` = an APMF_API::TravelLegState. Only meaningful when `ours`.
    struct LootLegState {
        bool          ours        = false;
        std::uint32_t state       = 0;
        std::uint32_t seq         = 0;
        std::uint32_t msInState   = 0;
        std::uint32_t blockedMs   = 0;
        std::uint32_t speed       = 0xFFFFFFFFu;
        std::uint32_t blockerKind = 0;
        RE::FormID    blocker     = 0;
        RE::FormID    dest        = 0;
        RE::NiPoint3  stall{};
    };
    bool ReadLootTravelLeg(int a_slot, LootLegState& a_out);

    // ── combat-action DENY facet CLAIM: PER-EXCURSION (ch.7, T1) ────────────────
    // Worker-safe. CLAIM the combat-action-deny facet for a_follower, naming
    // a_categoryMask (kIntent_CombatAction, param.ival -- an OR of
    // APMF_API::CombatActionCategory bits). While held, APMF denies the combat
    // behavior-tree leaves classified under the claimed categories (e.g.
    // kCombatActionCat_Offense denies Attack/Bash/RangedAttack/Cast*/etc). NOT
    // wired into the loot-travel dispatch by default (Logistics_Loot.cpp) --
    // MFO's own PACKAGE-THEFT guard already concedes loot-travel to a live combat
    // package on purpose (a follower who is actually fighting should keep
    // fighting); this helper exists for a caller that has a narrower, considered
    // need. No-op when APMF is absent or Config::g_apmfLootTravel is off. Returns
    // whether a_follower now holds a LIVE claim -- same showpiece-logging contract
    // as OfferPackage's return, for whichever caller eventually wires this in.
    bool ClaimCombatActionDeny(RE::FormID a_follower, std::uint32_t a_categoryMask);

    // Worker-safe. Release the combat-action-deny claim.
    void ReleaseCombatActionDeny(RE::FormID a_follower);

    // ── EQUIP AUTHORITY: a STANDING claim + the DECLARED WORN SET (ch.17, APMF v7, SCOPED v9) ──
    // feat/mfo-equip-authority (2026-09-15): port #1 of an MFO engine mechanism
    // into APMF. MFO used to hold a follower's gear in place by force-equipping it
    // (the T#76 prevent-removal lock, Actuation.cpp) and still lost the LEFT hand to
    // the follower's own combat AI within a second (Fable 2026-09-15: the AI
    // re-equips the shield over the dual-wield off-hand and the engine's forced
    // displacement strips the hold). Under the authority MFO DECLARES the worn set
    // instead -- APMF equips every declared item the actor is not wearing and
    // REFUSES every other engine equip on that actor at its #17a call-site seat
    // (outfit re-apply, the AI's own weapon/armor choice, combat re-arm, RemoveItem
    // re-equip, another plugin's equip). The declaration is built by
    // Logistics_Economy.cpp's RefreshEquipDeclaration (worker) and only SENT when
    // it changes ("declare, do not tick" -- APMF_API.h's SetEquipSet doc).
    //
    // LIFECYCLE. The claim is STANDING: no TTL, no refresh, NEVER swept by Tick()
    // -- it ends only with ReleaseEquipAuthority (unmanage / dismiss / the T#78
    // MFO-OFF edge, all via Logistics::OnFollowerRemoved), ClearTransientState
    // (kPreLoadGame AND kNewGame -- SKSE sends no kPreLoadGame for a New Game
    // started from a running session, and the new session reuses the same base
    // FormIDs), or the kill switch (Tick() releases a standing claim the
    // instant bApmfEquipAuthority reads OFF, so a flipped toggle cannot leave a
    // set enforced behind it). Re-made on the first service after a load. A KEPT
    // handle is re-validated against APMF (IsClaimLive, once ≥ 2 s old) on every
    // claim call and re-minted if APMF has forgotten it (F2) -- so APMF's own
    // release-all / unload sweep cannot leave MFO declaring into the void.
    //
    // THREAD ROAD: the worker (Scheduler tick -> Logistics::ServiceFollower and
    // Actuation::EquipWeapon), exactly ClaimEquipment's road -- APMF's
    // RequestEx/Release/SetEquipSet are thread-safe (copy + enqueue, applied on
    // the game thread at APMF's next Drain), and this bridge's own map is under
    // g_mx. No g_followers access here (#4/#74); callers pass FormIDs.
    //
    // NOT MIXED with ch.15: ClaimEquipment's kIntent_Equipment claim is the PARAM
    // form (gate only, no engine write), which APMF's INTEGRATION.md states is
    // unaffected by a ch.17 claim on the same actor. Both stand together.
    //
    // SCOPED (ABI v9, feat/mfo-equip-authority-v9, 2026-09-16). v7/v8 took the
    // facet WHOLE: every off-set engine equip of a governed type was refused, so
    // a declaration built from "the weapon currently in the hand" froze a
    // follower with no equip gambit out of his own melee<->ranged switch (STATUS
    // F6: 117 CombatNode would-denies in one deck run, none against a hold). v9
    // SCOPES the claim: MFO DECLARES what it OWNS and ACTS DIRECTLY in what it
    // does not. The scope (DeclareEquipScope, sent before the set) is
    //   owned  = Armor | Right (a right hold) | Left (a left hold)
    //          | Right+Left+Ammo (a bow/crossbow hold) | Right+Left (a 2H hold)
    //   denied = Shield when the perks vote dual wield (offHand==2) under
    //            bWeaponStyleControl, else 0
    // Light is never owned, Shield never owned (offHand==1 keeps the direct
    // EquipShieldOnMain), a hand is owned only while a ForcedHold stands in it
    // -- so with no hold the AI re-arms freely (`owned=0 verdict=allow` at the
    // seat) and a hold WINS over range (a dual-wield hold under an enemy at
    // range stays: the only exits are the gambit's own condition, combat end,
    // or a spell taking the left). Computed by RefreshEquipDeclaration.

    // Worker-safe. Is the equip-authority channel available right now -- APMF
    // present AND its resolved interface carries the v9 SetEquipScope slot (the
    // SCOPE: v8's whole-facet default would own the hands MFO's v9 declaration
    // deliberately omits and freeze them, so v9 is the floor -- a too-old APMF
    // degrades to NO authority, never to a blanket lock, logged once) AND
    // Config::g_apmfEquipAuthority is on? The exact three non-arbitration early
    // returns ClaimEquipAuthority/DeclareEquipScope/DeclareEquipSet take. UNLIKE the cast facets'
    // OffenseCastClaimSupported split, a refused CLAIM here does NOT fail closed
    // (F1/F5, Fable round 2): APMF refuses a ch.17 claim exactly when its #17a
    // equip seat is not installed, and then nothing on APMF's side can perform or
    // deny an equip -- so the correct behaviour on refusal is "MFO keeps its own
    // equips" (IsEquipAuthorityClaimed false -> every direct path runs as without
    // APMF), and that is what every site does. What DOES fail closed is a
    // DECLARATION on a standing claim (DeclareEquipSet false with a claim live):
    // the caller logs and does not equip around it.
    bool EquipAuthoritySupported();

    // Any thread. APMF's v8 IsEquipAuthorityEnforced: true only when its #17a
    // seat is INSTALLED and APMF.ini's [EquipAuthority] bEquipObserveOnly is 0 --
    // an off-set engine equip on a claimed actor is actually REFUSED. False in
    // observe mode (APMF still equips the declared set, the engine may take it
    // back off), before kDataLoaded, or when the channel is unsupported. Logged
    // as `mode=` in the `[equip-auth] <id>: claim` line so a Deck log states
    // which of the two a session ran under.
    bool IsEquipAuthorityEnforced();

    // Worker-safe. Ensure a_follower holds the STANDING kIntent_EquipAuthority
    // claim (param.ival = kEquipAuth_None: scripts/console pass, unequips pass,
    // observe-only is APMF's own INI decision this cycle). Idempotent: a live claim
    // is kept, not re-requested -- but a KEPT handle is re-validated with APMF's
    // v6 IsClaimLive once it is ≥ 2 s old (young claims are not yet in APMF's
    // published snapshot) and re-minted when APMF no longer knows it (F2: APMF's
    // hotkey release-all / unload sweep, a New Game from a running session, the
    // toggle's OFF->ON). Returns true iff a claim handle stands after the call;
    // `a_outFresh` (optional) is set true when the handle standing now was minted
    // since the last DeclareEquipSet on it -- RefreshEquipDeclaration drops its
    // change detector on that, so the new handle gets a declaration at once
    // instead of "unchanged, nothing sent" against a handle APMF has forgotten.
    // false + EquipAuthoritySupported() -> APMF REFUSED the claim (APMF refuses
    // exactly when its equip seat is not installed, so nothing on its side could
    // perform or deny an equip): the caller keeps MFO's OWN equips for that
    // follower -- the direct paths run as without APMF. Logged once per streak.
    // Logs `[equip-auth] <id>: claim` when a handle is minted.
    bool ClaimEquipAuthority(RE::FormID a_follower, bool* a_outFresh = nullptr);

    // Worker-safe. Release the standing claim (APMF: the engine owns the worn set
    // again; nothing is unequipped). No-op without one. Logs
    // `[equip-auth] <id>: release` when a live handle was released.
    void ReleaseEquipAuthority(RE::FormID a_follower);

    // Worker-safe (mutex-guarded read). Does a_follower hold a live equip-
    // authority claim handle? THE per-follower switch the direct equip sites
    // consult (AcquireEquip's armor hop, Actuation's weapon/shield equips):
    // supported AND claimed -> the declaration is the equip path. Says nothing
    // about whether the claim currently OWNS the channel (a claim that lost
    // arbitration keeps its handle and its stored set, which takes effect on the
    // win) -- that is APMF's business, and MFO must not equip around it either way.
    bool IsEquipAuthorityClaimed(RE::FormID a_follower);

    // Worker-safe. SCOPE the standing claim (ABI v9 SetEquipScope): `a_owned` /
    // `a_denied` are APMF_API::EquipCategory masks (see the block doc above for
    // what MFO computes). COPIED inside APMF's call (a stack temporary), applied
    // at its next Drain; the ONE caller (RefreshEquipDeclaration) sends it
    // immediately BEFORE DeclareEquipSet so both normally land in the same Drain
    // (a Drain between the two enqueues pairs the new scope with the old set for
    // one self-healing frame, MFO-B45 -- never a stale scope over a new set).
    // Bits outside kEquipCat_All are
    // MFO's error (logged; APMF masks them). Returns false when the channel is
    // unsupported OR no claim handle stands -- the same discipline as
    // DeclareEquipSet, and the caller treats it the same (fail closed, retry on
    // the next refresh). Does NOT clear the claim's `fresh` flag: the scope alone
    // is not a declaration; DeclareEquipSet, which always follows, does.
    bool DeclareEquipScope(RE::FormID a_follower, std::uint32_t a_owned, std::uint32_t a_denied);

    // Worker-safe (mutex-guarded read). Does the scope last SENT on a_follower's
    // standing claim own ANY of `a_categories` (an EquipCategory mask)? False
    // without a claim, and false on a claim no declaration has gone out on yet
    // (APMF answers "no declaration -> allow" there, so nothing is owned in
    // effect). THE gate a direct equip path reads before writing into a hand the
    // authority holds: Logistics::EquipTorch skips while Left is owned (a torch
    // competes for Light+Left; equipping it under a left or bow hold would only
    // draw an `External(MFO.dll) ... verdict=deny` at APMF's seat).
    bool EquipAuthorityOwns(RE::FormID a_follower, std::uint32_t a_categories);

    // Worker-safe (mutex-guarded read), the exact MIRROR of EquipAuthorityOwns above
    // and with the same two guards: false without a standing claim, and false on a
    // claim no declaration has gone out on yet (nothing is denied in effect until a
    // scope is SENT). Reads the `denied` half of the scope last sent, so it answers
    // "MFO's OWN declaration currently refuses this category for this follower".
    //
    // WHY IT EXISTS (fix/mfo-spell-authority-0922): the economy's sell scan tests
    // `worn` BEFORE anything else and reads a worn item as "keep". For a category
    // MFO's own declaration DENIES -- today only Shield, denied when the perks vote
    // dual-wield under bWeaponStyleControl -- that read is wrong in one direction
    // only: nothing can ever re-equip the item (APMF refuses the category) and
    // nothing ever UNEQUIPS an already-worn one (APMF ch.17 never unequips), so it
    // is worn forever and never sold. Cicero's shield, field 2026-09-22. The sell
    // path routes such an item through its existing FORCE-SELL branch, whose
    // RemoveItem unequips on sale.
    bool EquipAuthorityDenies(RE::FormID a_follower, std::uint32_t a_categories);

    // Worker-safe. DECLARE the worn set for a_follower through the v8 SetEquipSetEx
    // slot on the standing claim. `a_forms` are APMF_EquipEntry {BASE FormID, hand}:
    // kEquipSlot_Right / kEquipSlot_Left for the hand-held items MFO holds (the
    // ForcedHold ledger -- under v9 NOTHING else names a hand: a hand without a
    // hold is not owned and carries no entry), kEquipSlot_Default (the engine
    // picks, v7 verbatim) for armor, ammo, a held bow, a held two-hander; the
    // same form may appear once per hand. Shields and torches are never declared
    // (their categories are never owned). The array is COPIED inside APMF's
    // call. An empty set CLEARS the declaration (pass-through) without
    // releasing the claim.
    // More than APMF_API::kMaxEquipSet (32) entries is MFO's error: logged as an
    // error and truncated (APMF treats the excess as off-set). Returns false when
    // the channel is unsupported OR no claim handle stands (a declaration needs the
    // claim: call ClaimEquipAuthority first) -- false + EquipAuthoritySupported()
    // is FAIL CLOSED for the caller (log, do not equip directly). SetEquipSet
    // itself returns nothing and no-ops silently on a stale handle, so a `true`
    // means "handed to APMF", not "worn" -- the [apmf][equip-auth] pass line and
    // the [apmf][equip-obs] verdicts are the readback.
    bool DeclareEquipSet(RE::FormID a_follower, const std::vector<APMF_API::APMF_EquipEntry>& a_forms);

    // ── heal-cast facet CLAIM: PER-CAST, TTL-bounded (ch.8b, APMF v5) ───────────
    // MFO's Composed Forced Cast (Docs/SPEC-FORCED-CAST.md) makes a follower cast
    // a spell his AI would not choose -- the canonical case a heal at an ally.
    // PORTED (feat/mfo-cast-port, 2026-09-05) from the kIntent_SelectSpell +ACT
    // drive back to kIntent_Cast/RequestCast: APMF's feat/ai-cast-seats-impl
    // RETIRED the +ACT drive entirely (`ival`/`target`/`pos` on kIntent_SelectSpell
    // are now accepted-and-ignored -- see APMF_API.h) and replaced it with FIVE
    // engine vfunc seats answered on the Restore caster vtable: while a kIntent_Cast
    // claim stands, the NPC's OWN combat AI selects/equips/charges/aims/fires/
    // channels `spell` at `target` -- a real native animated cast. APMF fires
    // NOTHING itself (no EquipSpell/CastSpell/CastSpellImmediate/anim-graph write);
    // every engine call is the AI's own, from its own behavior tree. `target` is
    // now LOAD-BEARING (seat 0x0A hands it to the engine as the magic target, seat
    // 0x0D as the aim override) -- it was RECORD ONLY under the retired +ACT drive.
    //
    //     RequestCast(actor, basis, {spell, proxy, target, flags, ttlMs})
    //     (repeat call, same values)   -- cheap refresh, no release/re-engage churn
    //     (repeat call, changed values) -- a NEW bounded claim (release + re-
    //                                      request; the rich payload has no
    //                                      in-place re-point, unlike SelectSpell's
    //                                      Repoint)
    //     Release(handle)              -- ends the claim, restores the AI's own
    //                                      cast deliberation
    //
    // A kSelf-delivery spell aimed at a non-self target needs `proxy` (or APMF
    // mints one itself -- core/CastProxy.h), because the engine's Self branch
    // always lands on the caster; MFO forwards `proxy = 0` and lets APMF mint it
    // (Try() does not inspect delivery, does not proxy, and does not substitute --
    // ComposedCast.cpp's own comment on this still applies verbatim).
    //
    // Rides its OWN kIntent_Cast channel (ch.8b) -- a DISTINCT claim slot from
    // offense's ClaimOffenseCast (also kIntent_Cast, ch.8b, above, PORTED
    // feat/offense-cast-seats off the retired ch.8 kIntent_SelectSpell claim);
    // heal and hostile casts are mutually exclusive per tick by
    // CasterConsent::SpellKind, never concurrent on one follower, but keeping
    // distinct facets avoids any accidental cross-talk.
    //
    // a_hand: LEFT, ALWAYS (THE RULE, marth 2026-09-05, deck-proven): whenever an
    // equip gambit is actively force-holding a weapon (APMFBridge::
    // IsEquipmentClaimActive), that weapon owns the RIGHT hand, so a spell must
    // claim LEFT (kApmfHandLeft above) rather than auto -- auto prefers the right
    // hand when it reads free, and a follower briefly unarmed (e.g. the
    // facet-expiry bug this same file fixed 2026-09-05) let auto grab the right
    // hand, only for the equip gambit's own re-equip to shove the spell right back
    // out ~500ms later (deck: "driving right hand" -> re-equipped 'Elven Dagger'
    // -> "never left rest -- degrading"). LEFT is also the correct fallback with
    // no weapon held: heals are left-hand almost always regardless, UNCONDITIONALLY
    // -- a heal never routes through the dual-cast/juggle policy below, so
    // ClaimHealCast always passes kApmfHandLeft (unchanged, 2026-09-06).
    //
    // THE GENERAL HAND POLICY (2026-09-06, WIRED feat/per-hand-cast-slots), for
    // OFFENSE only (heal's own rule above always overrides it -- a heal never
    // routes through this). `Loadout::PlanCastHand` (native/Loadout.h/.cpp)
    // decides, from the follower's REAL live loadout + perks + magicka:
    // weapon-hand-active (see WeaponHandActive below) -> Left; both hands free
    // + one spell wanted + the follower's own dual-casting perk
    // (`Loadout::CanDualCast`, HasPerk + CalculateMagickaCost x the vanilla
    // fMagicDualCastingCostMult, 2.8) can afford it -> DualCast; otherwise ->
    // EitherFree. Actuation.cpp's `ResolveCastHand` (the per-hand cast-gambit
    // lock, `CastLockLive`'s neighbour) is the live caller that turns
    // EitherFree into a CONCRETE Left or Right by checking which hand is
    // actually free against the lock's own per-hand state -- "the juggle":
    // when a second, DIFFERENT spell is also wanted (by a later rule in the
    // SAME scan, or a later tick), it lands on whichever hand the first
    // spell did NOT take. `ClaimOffenseCast`'s `a_hand` parameter above
    // receives that already-resolved value directly (never bare `HandFor`
    // output for the EitherFree case -- see ResolveCastHand's own doc).
    //
    // DualCast IS EXPRESSIBLE through this claim shape (2026-09-06, APMF_API::
    // kCastFlag_DualCast, bit 3, append-only mirror of APMF's own header):
    // `EnsureCastClaimLocked`'s flags build translates a_hand ==
    // kApmfHandDualCast into kCastFlag_DualCast INSTEAD OF kCastFlag_LeftHand
    // (never both -- see APMF_API.h's own "do NOT set kCastFlag_LeftHand
    // alongside it" rule) -- see `HandFor` below, which does the
    // `Loadout::HandPick` -> a_hand translation for the Left/DualCast cases
    // (EitherFree needs ResolveCastHand's own lock-aware juggle, not a static
    // mapping -- see its doc). It is still just a HINT: the engine seats
    // decide whether both hands actually arm, and a follower who can't (or
    // the engine otherwise won't) simply casts single-hand -- no retry, no
    // re-claim, no fallback that would make that degrade look like a real
    // dual-cast (CLAUDE.md principle 7). ResolveCastHand itself additionally
    // refuses to even ATTEMPT a DualCast claim unless BOTH hands are free (or
    // already refreshing the SAME spell) at the lock layer -- a half-landed
    // dual-cast is worse than a held-off one, never a silent downgrade.
    // a_target: 0 = self; an ally/player FormID for heal-other.
    //
    // a_concentration: pass true when a_spell->GetCastingType() ==
    // kConcentration -- sets APMF_API::kCastFlag_Concentration so the claim's TTL
    // floor applies (a held stream is never cut mid-channel by an early TTL).
    //
    // a_stopPct: 0 = no client threshold (the seat stops the channel at FULL
    // restoration, same as before this claim carried the bit); 1..100 = a whole
    // percent of the TARGET's PERMANENT actor value at which seat 0x07
    // CheckStopCast ends the channel -- lets a "Health < N% -> Heal" gambit's own
    // configured threshold stop the native channel exactly where the gambit says,
    // not always at 100%. Built into the claim via APMF_API::MakeStopPct(pct);
    // see that header for the exact semantics. Ignored (harmless) on an instant
    // (non-concentration) heal -- there is no channel to stop early.
    //
    // Worker-safe. CREATE-OR-REFRESH: call every tick the gambit still wants the
    // heal -- a repeat call with the SAME (spell, target, hand, concentration,
    // stopPct) is a cheap refresh; a CHANGE in any of them releases and
    // re-requests (a new bounded claim -- RequestCast has no in-place re-point).
    // Returns whether a_follower now holds a LIVE claim (false -> caller degrades
    // to kInstant, a heal must never vanish). No-op (returns false) when APMF is
    // absent, its resolved interface has no RequestCast slot (ABI < 5 -- an older
    // APMF.dll degrades cleanly rather than crashing), or Config::g_healAnimPackage
    // is off.

    // a_hand value meaning "claim BOTH hands for one spell, empowered" --
    // EnsureHealClaimLocked (APMFBridge.cpp) maps this to APMF_API::
    // kCastFlag_DualCast rather than kCastFlag_LeftHand. This is MFO's OWN
    // encoding for a_hand (a plain std::int32_t, distinct from kApmfHandLeft
    // and the implicit "right/auto" of 0) -- it has no meaning to APMF
    // itself, which only ever sees the resulting CastFlags bit. Produce this
    // value via `HandFor` below (from a real `Loadout::PlanCastHand` result),
    // never by hand -- PlanCastHand alone holds the weapon-hand-exclusion
    // guard (Loadout.cpp: `a_weaponHandActive -> HandPick::Left`, unconditional,
    // checked before anything else) that keeps this from ever firing over a
    // held weapon.
    inline constexpr std::int32_t kApmfHandDualCast = 3;

    // Translate a `Loadout::PlanCastHand` decision into this bridge's a_hand
    // encoding (kApmfHandLeft / kApmfHandDualCast / 0 for "either hand" --
    // EnsureHealClaimLocked treats 0 as no hand hint, engine auto-prefers
    // right). Pure -- no engine writes, no re-checking of PlanCastHand's own
    // weapon-hand-exclusion guard (it already ran inside PlanCastHand; this
    // function only carries its answer forward). A caller still owns getting
    // `a_weaponHandActive` right when it calls PlanCastHand in the first
    // place (see WeaponHandActive below) -- this function cannot fix a wrong
    // input, it only forwards a correct one faithfully.
    inline std::int32_t HandFor(Loadout::HandPick a_pick) {
        switch (a_pick) {
        case Loadout::HandPick::Left:      return kApmfHandLeft;
        case Loadout::HandPick::DualCast:  return kApmfHandDualCast;
        case Loadout::HandPick::EitherFree:
        default:                           return 0;
        }
    }

    // The heal-cast claim's TTL, ms (APMF_CastRequest::ttlMs). A SINGLE named
    // constant so ComposedCast.cpp's CastBounds::Arm window (the MFO-side consent
    // standdown, native/CastBounds.h) and APMF's own claim TTL (the auto-release
    // backstop if MFO ever forgets to release) never drift apart -- both windows
    // exist to bound the SAME thing (a heal claim MFO already vetted), so one
    // value serves both call sites. 6s: long enough to outlast one HoT tick + the
    // round-robin refresh gap (FacetExpiry() below sizes the SAME margin for the
    // local backstop), short enough a crashed/forgetful caller's claim
    // self-releases quickly (SPEC-FORCED-CAST.md §1.4's Heal case).
    inline constexpr std::uint32_t kHealCastTtlMs = 6000;

    // THE NEVER-OBSERVED HOLD CAP, ms -- the ceiling on how long ComposedCast::
    // Try's F1 incumbent hold may keep a competing heal rule OFF the follower's
    // single heal slot while the engine has NEVER been observed firing the
    // incumbent's claim. Read RefreshHealCastClaim's doc below for the mechanism;
    // this is only the sizing.
    //
    // DELIBERATELY NOT kHealCastTtlMs (Fable diff review, 2026-09-06 -- it WAS
    // that constant, whose 6000ms is the wrong quantity for this job). The two
    // numbers bound different things and must not be aliased: kHealCastTtlMs is
    // the lifetime of a claim MFO has already vetted, while THIS one must outlast
    // the engine's own claim -> OBSERVED-CAST latency, since `observed` (set from
    // Diagnostics.cpp's SpellSink) is the signal that takes the incumbent off
    // this cap entirely. #9: size it from the real cadence.
    //
    // SIZED FROM THE HEAL DATUM, NOT THE OFFENSE ONE (round-3 review, 2026-09-07
    // -- the 3000ms this held first was sized from an offense fire and was too
    // tight). Deck log 2026-09-06 (Docs/DIAG-2026-09-06-deny-heal-failures.md,
    // on `main`; this branch predates that file):
    //   * The ONE heal that landed all session: claim minted 19:59:11.158 ->
    //     castobs Casting 19:59:13.643 -> MFO `[cast]` 19:59:14.110. That is
    //     **2.95s claim-to-OBSERVED**, and `observed` is what this cap races.
    //   * The 2.3-2.5s figure quoted elsewhere in that DIAG is an **OFFENSE**
    //     Firebolt (claim 19:56:56.873 -> fire 19:56:59.155). It is NOT a heal
    //     latency and must not be used to size a heal budget.
    //   * The offense evidence also shows fires trailing their 2.0-2.4s [cfc]
    //     warns by up to 1.5s -- ~3.9s claim-to-fire at the tail.
    //   * The DIAG's own F6 recommends >= 4000ms for the mere WARNING; a LIFT is
    //     a far more destructive action than a log line, so it cannot be tighter.
    // 4000ms clears the measured 2.95s heal observation by ~1.05s and sits at or
    // above the worst measured latency of ANY claimed cast in that session, while
    // staying well inside kHealCastTtlMs (6000ms) so the cap still bites before
    // the claim it guards dies on its own.
    //
    // AND THE CHECK IS LAP-GRANULAR: it runs on the OOC service lap (~1.1-1.3s,
    // DIAG RC2), so the actual lift lands anywhere in [cap, cap + ~1.3s]. That
    // slack is on the SAFE side and is deliberately not compensated for.
    //
    // NO "PROVABLY NOT GOING TO FIRE" CLAIM. The note that stood here said an
    // unobserved incumbent past the cap was provably dead. On this DIAG's own
    // evidence that is FALSE -- a real heal was still 2.95s from `observed`, and
    // offense fires arrived 3.9s after their claim. The honest framing is a
    // TRADE-OFF, not a proof:
    //   * Cost of a LARGER cap: a competing rule (usually higher-ranked -- see
    //     ComposedCast.cpp's REACHABILITY note) waits cap + one lap before it can
    //     claim. Bounded, visible in the log, and it waits either way.
    //   * Cost of a SMALLER cap: the lift makes the newcomer's ClaimHealCast
    //     RELEASE the incumbent's handle mid-charge -- cutting the only heal path
    //     ever observed to land, at the instant it fires. That is RC1, the exact
    //     field failure F1 exists to fix.
    // The asymmetry is why this is sized long rather than short.
    //
    // Sits ABOVE ComposedCast.cpp's kSilentWarnAfter (2000ms) -- static_asserted
    // at that constant's definition -- so the "[cfc] NO observed cast" warning
    // always PRECEDES the lift in the log instead of trailing it. The claim TTL
    // is untouched: this caps only the HEARTBEAT, and only on the unobserved
    // path, so an OBSERVED heal still runs its full kHealCastTtlMs window.
    inline constexpr std::uint32_t kHealHoldNeverObservedMs = 4000;

    // THE IDLE-HAND FLOOR'S UNOBSERVED GATE (fix/mfo-combat-restoration-direct,
    // 2026-09-21). How long a DRIVING cast claim may stand with NO observed cast
    // before the floor it earns on the other hand is RELEASED. Deck 2026-09-21
    // (Jesper 750012C6): a left-hand claim the engine never fired kept the
    // right-hand floor up ("nothing un-gambited may arm on this one"), so the
    // dagger could not be armed either -- a claim that does not fire must not
    // close the other hand. Past this age with no observed cast within the
    // claim's lifetime (ComposedCast::ObservedFiring over `created`), the floor
    // is dropped; the driving claim itself is untouched (its own TTL / sweep /
    // hold caps bound it), and the floor returns the moment the claim is
    // observed firing.
    //
    // SIZED FROM THE SAME DATUM AS kHealHoldNeverObservedMs, DELIBERATELY THE
    // SAME NUMBER, AND THAT DATUM IS CONTESTED -- read Docs/REVIEW-BACKLOG.md
    // MFO-B7 and MFO-B8 before touching this. Both constants bound one physical
    // quantity, the engine's claim -> OBSERVED-CAST latency. The 0906 heal
    // measured 2.95 s claim-to-observed (Docs/DIAG-2026-09-06-deny-heal-
    // failures.md; the same session's offense Firebolt 2.3-2.5 s claim-to-fire,
    // ~3.9 s at the tail). The 0908 heal EXCEEDED it: 4.5-6.1 s claim-to-fire
    // (Docs/DIAG-2026-09-08-field.md:267, "2.95 s plus one extra equip cycle" --
    // the hand was mid-unequip from a Firebolt churn), so 4000 ms does NOT clear
    // every measured heal latency, and B7's ruling stands: MEASURE, do not
    // resize from n=2. What this gate actually consumes is the OFFENSE claim's
    // latency WITH an equip cycle in front of it (heals no longer claim -- they
    // take the direct road), and that quantity is UNMEASURED; the next Deck log
    // sizes it (the [cfc] fire timestamps against the "FLOOR released" lines).
    // Until then the cost of the gate firing early is the pre-F10 state -- the
    // other hand opens to the AI while a genuine cast is still charging --
    // never a freeze. This is consumer (d) of the constant under B8's scheme.
    // Aliased rather than copied so a re-measurement moves both. Lap-granular:
    // the check runs on the pump (~133 ms), so the lift lands within
    // [gate, gate + 133 ms].
    // ── RE-SIZED AND DE-ALIASED FROM THE 2026-09-22 DECK LOG ──────────────────
    // This constant's own note (above) said "that quantity is UNMEASURED; the next
    // Deck log sizes it (the [cfc] fire timestamps against the 'FLOOR released'
    // lines)". THIS IS THAT LOG, and it came back against the 4000 ms alias:
    //   * An OFFENSE claim reaches an observed SpellFire in 5.8 s with an equip
    //     cycle in front of it (Jesper 750012C6, claim 08:26:59.202 -> SpellFire
    //     08:27:04.997) -- so a 4000 ms cap fired EARLY on every offense claim,
    //     which is every claim now that heals take the direct road.
    //   * The floor lifted 60 ms AFTER the engine began CHARGING the claimed
    //     Firebolt, and the AI charged its OWN spell in the freed hand 0.78 s
    //     later. The gate did not catch a dead claim; it opened a hand out from
    //     under a live one.
    // So the alias is BROKEN (the two constants no longer bound the same physical
    // quantity: kHealHoldNeverObservedMs governs a HEAL claim's heartbeat, and
    // heals no longer claim at all), and this one is sized from the offense
    // measurement it actually consumes: 8000 ms clears the measured 5.8 s by
    // 2.2 s and the worst heal claim-to-fire ever measured (6.1 s,
    // Docs/DIAG-2026-09-08-field.md:267) by 1.9 s. Lap-granular on the pump, so
    // the lift lands in [8000, 8000 + 133] ms.
    //
    // AND THE AGE IS NO LONGER THE WHOLE TEST. A timer alone cannot tell a dead
    // claim from a slow one -- that is what this log proved. `silentPastGate`
    // (APMFBridge.cpp) now also refuses to call a claim silent while the ENGINE
    // IS ACTUALLY CHARGING IT, read through Actuation::CastInFlightOnHand, THE one
    // definition of in-flight in this codebase (the follower's own MagicCaster for
    // that hand is in a live cast state with the claim's spell-or-proxy selected).
    // That is the "arm on first Charging" half of the fix: the age bounds a claim
    // the engine never picked up, and the in-flight read protects one it did.
    // MFO-B47 predicted exactly this failure -- see Docs/REVIEW-BACKLOG.md.
    inline constexpr std::uint32_t kIdleFloorUnobservedMs = 8000;

    // Returns whether a_follower now holds a LIVE heal-cast claim. As with
    // ClaimOffenseCast above, a `false` IS AMBIGUOUS on its own -- pair it with
    // `HealCastClaimSupported()` to tell "APMF refused" (FAIL CLOSED, never
    // degrade to kInstant) from "APMF/the facet/the toggle is absent" (kInstant
    // is the supported contract). ComposedCast::Try is the only caller and does
    // exactly that, surfacing the split to ITS callers as
    // ComposedCast::TryResult::ApmfRefused vs ::NotApplicable.
    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                       std::uint32_t a_stopPct = 0);

    // Worker-safe. The heal twin of OffenseCastClaimSupported() above: true iff
    // APMF is present AND its interface carries the v5 RequestCast slot AND
    // Config::g_healAnimPackage (the heal-cast executor toggle) is on -- exactly
    // ClaimHealCast's own three non-arbitration early returns.
    bool HealCastClaimSupported();

    // Worker-safe. Release ONLY the heal-cast claim now (every other facet left
    // alone). Call the instant the gambit stops wanting the heal (target lost /
    // spell/rule no longer wins) so APMF restores the AI's own cast deliberation
    // immediately -- the round-robin-aware FacetExpiry() backstop (Tick()) AND the
    // claim's own TTL (on APMF's side) cover a caller that forgets.
    void ReleaseHealCast(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every
    // other accessor here takes). Does a_follower currently hold a LIVE heal-cast
    // claim? NOTE: MFO's OWN CasterConsent hook is installed globally, so it also
    // intercepts APMF's seat-answered CheckStartCast/CheckCast on the SAME
    // Restore caster vtable -- that hook stands down via CastBounds
    // (native/CastBounds.h) OR'd with this very accessor (CasterConsent.cpp's
    // ClientCastClaimed), NOT this accessor alone. This also exists for parity/
    // observability with the other IsXActive queries above.
    bool IsHealCastActive(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (SAME g_mx). The delivery-flip proxy FormID
    // APMF minted internally for a_follower's LIVE heal-cast claim (see
    // GetOffenseCastProxy's doc above -- same ABI v6/no-live-claim/no-proxy 0
    // contract, heal's own always-LEFT slot). Cast-claim observability
    // (2026-09-06): ComposedCast::Try hands this to WatchArmed so the silent-
    // claim diagnostic recognises a cast of the proxy, not only the original
    // spell, as the claimed heal actually firing.
    // Worker-safe. The spell the heal slot's LIVE claim names, or 0 when no heal
    // claim stands. Exists for the preempt path, which must distinguish "the lock I
    // am displacing IS the heal claim" (route the release through
    // ComposedCast::End) from "an offense claim on the LEFT hand with a heal claim
    // coexisting" (leave the heal alone). Same contract as GetHealCastProxy.
    RE::FormID GetHealCastSpell(RE::FormID a_follower);

    RE::FormID GetHealCastProxy(RE::FormID a_follower);

    // Worker-safe (the SAME g_mx as every accessor above). HEARTBEAT for a
    // heal-cast claim a client is deliberately HOLDING instead of re-requesting
    // (ComposedCast::Try's incumbent lock, Fable amendment (a) 2026-09-06).
    // Returns whether a_follower's heal claim is still genuinely live, and when
    // it is, bumps the SAME `refreshed` stamp ClaimHealCast bumps.
    //
    // WHY THE STAMP. The hold path never reaches ClaimHealCast by design (it
    // must not release/re-request the incumbent), so without a heartbeat the
    // round-robin FacetExpiry() sweep in Tick() released the very claim the lock
    // was protecting -- after ~2.45s at the default fSuppressWindow, and after
    // ~0.77s at a legal fSuppressWindow=0, i.e. the lock did nothing at all at a
    // small suppression window. This is a refresh of an EXISTING claim, exactly
    // what one more ClaimHealCast lap would have done, never a new lifetime.
    //
    // WHY IT IS STILL BOUNDED -- TWO BOUNDS, and the second one is the real one
    // (Fable SEV-1 2026-09-06, sizing + rationale CORRECTED by the Fable diff
    // review the same day).
    //  1. LIVENESS, asked of APMF itself on ABI >= 6 (IsClaimLive -- APMF
    //     auto-expires a claim at its own TTL with no notice, so a stored handle
    //     is not proof). A claim that is NOT live is never heartbeaten, so it
    //     goes stale and Tick() collects it: the hold lifts. This bound is real
    //     but it is not the interesting one, because the INCUMBENT'S OWN RULE
    //     keeps re-arming: while its condition holds it calls ClaimHealCast every
    //     lap, which re-requests a fresh handle the moment APMF expires the old
    //     one. (BOTH HALVES OF THIS PARENTHESIS USED TO BE WRONG, and are
    //     rewritten here rather than deleted -- a comment teaching a false
    //     mechanism is worse than no comment. It read "MFO never Repoints a
    //     kIntent_Cast claim, and APMF main's ApplyRepoint does not touch
    //     expiresMs; the APMF-side renewal lives on an unmerged branch." APMF's
    //     renewal MERGED (core/ControlMap.cpp's TTL RENEWAL: a Repoint on a live
    //     cast claim moves its deadline to now + the claim's own granted ttlMs),
    //     and as of F5-1 MFO DOES Repoint a live cast claim -- see the CAST-CLAIM
    //     HEARTBEAT in EnsureCastClaimLocked. THE BOUND SURVIVES ANYWAY, and is
    //     in fact tighter now. That heartbeat fires ONLY from ClaimHealCast /
    //     ClaimOffenseCast, i.e. only from a rule still winning its lap -- never
    //     from THIS function, deliberately, because a hold that renewed APMF's
    //     TTL would be exactly the deadlock the old note imagined. So bound 1 is
    //     unchanged for a claim whose rule has gone quiet, and for a claim whose
    //     rule keeps re-arming the heartbeat merely replaces the expire-and-
    //     re-request cycle that was already keeping it alive -- with one real
    //     difference in this function's favour: `created` no longer moves on
    //     every 6s auto-expiry, so bound 2 below now measures the claim's TRUE
    //     age instead of being reset by a re-request.)
    //  2. CLAIM AGE. Past kHealHoldNeverObservedMs (4000ms -- sized from the
    //     MEASURED claim-to-OBSERVED latency of the one heal that landed on the
    //     deck, 2.95s, plus the tail; see that constant above for the full
    //     working and for why it is NOT the 6000ms claim TTL and NOT the offense
    //     2.3-2.5s figure) measured from when the handle was REQUESTED
    //     (CastClaim::created, stamped once inside EnsureCastClaimLocked), this
    //     refuses to heartbeat regardless of liveness. That caps how long a claim
    //     the engine has NEVER been observed firing may hold ComposedCast's
    //     single heal slot against a competing rule -- USUALLY one that OUTRANKS
    //     it, but not always: a LOWER-ranked rule is also held whenever the
    //     incumbent's own condition has gone false since it claimed, because the
    //     hold's heartbeat keeps that abandoned claim alive until this cap
    //     (ComposedCast.cpp's REACHABILITY note has both cases).
    //     `created` DOES move when the incumbent's own rule re-requests after an
    //     APMF auto-expiry (a genuinely new handle earns a fresh window, by
    //     design); that cannot happen underneath a live hold, because the
    //     incumbent's re-request returns Claimed and stops the rule scan before
    //     any held rule's Try() is reached. The cap never applies to an OBSERVED
    //     claim either, because the only caller (ComposedCast::Try's incumbent
    //     hold) calls this only while the incumbent is unobserved -- a live
    //     channelled heal never routes through here at all.
    //
    // ABI < 6 RETURNS FALSE. There is no IsClaimLive to ask AND no other bound in
    // play (EnsureCastClaimLocked's unchanged fast path trusts a stored handle
    // forever on ABI < 6, and the incumbent's own rule keeps `refreshed` moving,
    // so Tick()'s sweep never fires either). Reporting the stored handle would
    // hold a newcomer off behind a handle APMF may have silently expired, with
    // nothing able to end it -- a mask (#7). Refusing degrades honestly to the
    // pre-F1 behaviour (the two rules visibly thrash the slot) instead. Inert in
    // practice: kABIVersion is 6 and the DLL pair ships together.
    bool RefreshHealCastClaim(RE::FormID a_follower);

    // ── CAST-SELECT CANDIDATE REFUSAL: a GATE-ONLY ch.8 claim + the ALLOW-LIST ──
    //    (kIntent_SelectSpell + APMF_API_v4 SetSpellAllowList; fix/mfo-spell-
    //    authority-0922)
    //
    // THE HOLE THIS CLOSES. MFO governs CASTING (CasterConsent's CheckStartCast /
    // CheckCast denies) but governed EQUIP/SCORE only on a hand carrying a LIVE
    // cast claim: the score steer only ever ADDS +1000 to the claimed spell, it
    // never lowers anything, and a zero would not work either (a zero-scored item
    // stays selectable -- only CheckShouldEquip's 0x0F answer REMOVES a candidate).
    // So on 2026-09-22 the AI equipped an UNGAMBITED spell (Vampiric Bolt
    // 841B8A15) in BOTH hands and stood there charging it while MFO's consent
    // denied every cast: ~133 s of 221 s of party combat visibly inactive.
    // Deny-complete, nothing activated -- CLAUDE.md principle 2's exact failure.
    // marth's rule, in his own words (2026-09-07): "MFO by design ONLY is to
    // allow the gambited spells to occur."
    //
    // WHAT IT IS. While a follower is under CONTINUOUS CAST CONTROL, MFO holds an
    // ACTOR-WIDE, GATE-ONLY kIntent_SelectSpell claim (`param.form == 0`: it
    // names no spell and drives nothing) and attaches the follower's allowed
    // forms with SetSpellAllowList. APMF's allowance then reads
    // (core/Allowance.cpp:60-79, verified against the APMF tree):
    //     no claim                       -> ALLOW (uncontrolled)
    //     claim.form==0 && allowCount==0  -> ALLOW (channel default)
    //     subjectForm == claim.form       -> ALLOW
    //     subjectForm in the allow-set    -> ALLOW
    //     otherwise                       -> DENY
    // and consults it at BOTH seats, ACTOR-WIDE and independent of per-hand cast
    // claims and of the idle-hand floor:
    //   * t2a CheckShouldEquip (APMF core/EquipGate.cpp:609) -- engine-answer-
    //     first, so it only ever flips the engine's YES to NO. THIS is the
    //     candidate refusal; it is the only surface that can stop an ungambited
    //     spell reaching a hand at all.
    //   * t2c CheckCast (APMF core/CastGate.cpp:160) -- a second, redundant-by-
    //     design deny of the charge itself.
    // APMF has shipped this since ABI v4 and MFO had NEVER called it
    // (`git log -S"SetSpellAllowList("` was empty; shelved 2026-09-04).
    //
    // ONE SOURCE OF TRUTH, BY CONSTRUCTION. The list is published from inside
    // CasterConsent::NoteGambits -- the SINGLE writer of the `g_ctrl` set the
    // continuous deny reads -- so the deny and the allow-list cannot disagree
    // about "what this follower is allowed to use". That disagreement IS the
    // principle-2 violation this exists to fix, so it is closed structurally
    // rather than by two call sites agreeing to stay in step.
    //
    // WHAT MUST BE ON THE LIST OR OUR OWN DENY BITES US (it has, twice):
    //   * every configured cast-gambit spell (bound-weapon and summon gambits are
    //     just act.cast_self with a Bound/Conjure spell as the param -- there is
    //     no separate opcode, so the g_ctrl enumeration already covers them);
    //   * the delivery-flip PROXY forms APMF mints for MFO's live cast claims
    //     (both seats already exempt a ch.8b claim's own spell-or-proxy BEFORE
    //     they consult this channel, so this is belt-and-braces against a proxy
    //     on a hand the seat cannot resolve);
    //   * EVERYTHING CasterConsent's own deny DELIBERATELY EXEMPTS. That deny is
    //     NORMAL SPELLS ONLY (CasterConsent.cpp CtrlUnlatchedDeny: `Is(FormType::
    //     Spell)` + `GetSpellType() == kSpell`), so scrolls, STAVES, shouts,
    //     powers and abilities are never MFO's to veto. APMF's allowance is a
    //     pure FormID-set test and cannot be told "spells only", so the exempt
    //     items must be ENUMERATED onto the list. t2a is installed on 15
    //     CombatInventoryItemMagic AND 15 CombatInventoryItemStaff templates
    //     (APMF core/EquipGate.cpp), so a spells-only list would refuse every
    //     staff the follower carries -- a deny MFO does not make today.
    //
    // OVERFLOW FAILS OPEN AND LOUD, NEVER MUTES. kMaxSpellAllowList is 32 and
    // APMF's documented degrade for a longer list is "the excess are treated as
    // non-exempt", i.e. DENIED -- which on this channel would silently disarm a
    // follower. So a list that does not FIT is not truncated: no claim is held at
    // all for that follower, an error names the counts, and the cast-time deny
    // carries the load exactly as it did before this existed (#7: an unmasked
    // failure diagnoses in one field cycle).
    //
    // LIFETIME = the deny's lifetime, IN COMBAT ONLY. `g_ctrl` is populated only
    // from the Scheduler's PARTY-COMBAT service and erased by
    // CasterConsent::Clear (the 2-tick party-out-of-combat teardown, dismissal)
    // and ClearAll (revert/load) -- so the claim is released on exactly those
    // edges. AT FIGHT END THE GATE LIFTS COMPLETELY (the decision, recorded):
    // out of combat MFO deliberately leaves a follower's own casting alone
    // (Candlelight, a self-heal, a mage's utility), the cast-time deny is gone
    // there too, and a gate that outlived the fight would be a mute nothing in
    // combat asked for. Backstopped by Tick()'s FacetExpiry() sweep on the same
    // round-robin-aware window every other per-combat facet in this file uses, so
    // a follower who simply stops being serviced (cell change, pump stall) loses
    // the gate rather than keeping it forever -- expiry is the SAFE direction
    // here, since no claim means ALLOW.
    //
    // ABI FLOOR 4 (SetSpellAllowList). Below it there is no allow-set to attach,
    // and a bare gate-only claim would then be `form==0 && allowCount==0` ->
    // APMF's "channel default -> ALLOW", i.e. a claim that denies nothing: no
    // claim is made at all. Kill switch: `bApmfSpellAllowList` (default ON), and
    // it is additionally inert whenever the cast facet is off (`bApmfCast`) or
    // cast control is off (which empties `g_ctrl`).
    //
    // `a_spells` is MFO's gambit set; the proxies and the exempt items are added
    // here. Returns true when a claim stands and the list it carries is current.
    // An empty `a_spells` RELEASES (idempotent). Worker road; takes g_mx and must
    // NOT be called while CasterConsent's own lock is held (the combat thread
    // reads that lock in both deny hooks).
    bool PublishSpellAllowList(RE::FormID a_follower, const std::vector<RE::FormID>& a_spells);

    // Drop the gate-only ch.8 claim for one follower. Idempotent; logs only when a
    // claim actually stood. Called from CasterConsent::Clear (combat end,
    // dismissal) and by Tick()'s expiry sweep.
    void ReleaseSpellAllowList(RE::FormID a_follower);

    // Does a gate-only ch.8 claim stand for this follower right now? Diagnostic /
    // caller-side gating only; nothing in the deny path reads it.
    bool IsSpellAllowListClaimed(RE::FormID a_follower);

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
