#pragma once
#include <RE/Skyrim.h>
#include <chrono>   // FacetExpiry()'s std::chrono::milliseconds return type
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
    // hybrid, byte-identical to the APMF-absent path -- a refused claim must
    // never silently drop the cast). No-op (returns false) when APMF is
    // absent, its resolved interface has no RequestCast slot (ABI < 5), or
    // Config::g_apmfCast is off.
    bool ClaimOffenseCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                          std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                          std::uint32_t a_stopPct = 0);

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
    void ReleaseOffenseCast(RE::FormID a_follower);

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

    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                       std::uint32_t a_stopPct = 0);

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
    //     one. (It is NOT defeated by a renewing Repoint: MFO never Repoints a
    //     kIntent_Cast claim -- see EnsureCastClaimLocked -- and APMF main's
    //     ApplyRepoint does not touch expiresMs. The APMF-side renewal this doc
    //     used to warn about lives on an unmerged branch.)
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

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
