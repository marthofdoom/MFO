#pragma once
#include <RE/Skyrim.h>

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
//   * ClaimCasting      (kIntent_SelectSpell)  — "MFO owns this follower's cast choice"
//   * ClaimCombatTarget (kIntent_CombatTarget) — "MFO owns this follower's combat target"
// and then, in Actuation::CastOn, EXECUTES the real cast ITSELF: writes the follower's
// own `selectedSpells`, commands the target via `Targeting::Command` (currentCombatTarget),
// grants its own `CasterConsent`, and a Cast-biased combat style makes the follower's own
// AI DECIDE to cast — a real, fully-animated, MOBILE cast. Crucially MFO does NOT claim
// the MOVEMENT facet, so the follower keeps kiting while it casts — that granular
// non-interruption is exactly why the cast routes through APMF. No forced cast on this
// path (see CAST-DELIVERY.md; force lives only in the legacy hybrid).
//
// Claim lifecycles: casting = PER-CAST (refreshed each winning cast tick; released
// crisply by ReleaseCasting the instant no cast rule holds). combat-target = PER-COMBAT
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

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every
    // other accessor here takes). Does `a_follower` currently hold a LIVE
    // cast-select facet claim (i.e. is the owned-cast model actively driving
    // this follower's cast right now)? Phase 2 (Docs/ALLOWANCE-TEMPLATE.md
    // §7, APMF repo): once APMF's own T2 allowance hooks (CastGate/EquipGate)
    // enforce "only the claimed spell" at the engine gate, MFO's OWN
    // cast-exclusivity gates (CasterConsent's competing-spell deny,
    // CombatStyle's equip-gate deny) consult this to STAND DOWN for that one
    // follower -- avoiding two independently-configured deny mechanisms
    // (MFO's iCastControl slider vs. APMF's hard claim) disagreeing on the
    // same decision. Does NOT affect CasterConsent's own consent-GRANT (the
    // veto-removal that lets the AI consider casting at all) -- that stays
    // MFO's, APMF only ever narrows a YES to a NO, never invents one.
    bool IsOwnedCastActive(RE::FormID a_follower);

    // ── casting facet CLAIM: PER-CAST ────────────────────────────────────────────
    // Worker-safe. CLAIM the casting facet for this follower (APMF records MFO as the
    // owner; the chosen `a_spell` rides along for arbitration/observability). This does
    // NOT select or cast anything — MFO writes selectedSpells + grants consent itself in
    // CastOn. Call every winning cast tick (re-pointed in place on a v3 APMF when the
    // spell changes). No-op when APMF is absent or Config::g_apmfCast is off.
    void ClaimCasting(RE::FormID a_follower, RE::FormID a_spell);

    // Worker-safe. Release ONLY the casting facet-claim now (the combat-target claim, if
    // any, is left alone). Call the instant no cast rule holds (Scheduler !castSeen) so
    // the claim releases crisply, not after the 500 ms expiry backstop.
    void ReleaseCasting(RE::FormID a_follower);

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

    // Worker-safe. Once-per-pump sweep: release each claim not refreshed within the
    // expiry window (cast-select backstop; combat-target = combat-end detector). Call
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
    // ClaimCasting uses, so one call both engages a new hold and keeps an existing one
    // alive under the shared kExpiry backstop. RE-POINTS in place (same handle) if the
    // held weapon FormID changes (a melee<->ranged flip re-forces a different weapon).
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

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
