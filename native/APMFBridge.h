#pragma once
#include <RE/Skyrim.h>

// ─────────────────────────────────────────────────────────────────────────────
// APMF integration — MFO as an APMF client (Phase 3: the OWNED cast gambit). APMF
// (AI Package Management Framework) is a SEPARATE SKSE DLL; MFO talks to it ONLY
// through the byte-shared, append-only header `APMF_API.h` (the same discipline MFO
// applies to `MEO_API.h`) plus APMF's runtime interface query. APMF holds ZERO
// MFO-specific code; this bridge is the ONLY MFO code that knows APMF exists.
//
// WHAT THIS WIRES — the OWNED CAST MODEL (marth 2026-09-02) with TWO DECOUPLED
// LIFECYCLES. MFO asks APMF to OWN both halves of a follower's combat:
//   * cast-select  (kIntent_SelectSpell)  := the gambit's spell  -> the AI selects it.
//       PER-CAST: refreshed each winning cast tick; released CRISPLY the instant no
//       cast rule holds (ReleaseCastSpell).
//   * combat-target(kIntent_CombatTarget) := the follower's foe  -> APMF HOLDs it.
//       PER-COMBAT: created by a cast directive, RE-POINTED (same handle, no
//       release/re-request) whenever the target changes, kept alive every in-combat
//       tick, and released ONLY when the fight ENDS (refreshing stops -> expiry). A
//       cast->melee rule transition mid-battle RE-POINTS it, it does NOT release it.
// With the spell selected, the target held, and MFO's CasterConsent granting the
// follower's own AI consent to cast it, the follower fires the RIGHT spell at the
// RIGHT target through the engine's NORMAL cast flow — i.e. it plays the FULL cast
// ANIMATION/pose ("the animated path"). This is the ANIMATED-FIRST path and the point
// of the owned model: it resolves MFO's long-deferred cast-animation gap
// ([[cast-animations-deferred-to-post-town-polish]]) — casts animate because the AI
// fires them naturally, not because MFO force-injects them. MFO's own unanimated
// CastSpellImmediate force is demoted to a RARE last-resort (see Actuation CastOn).
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

    // ── cast-SELECT: PER-CAST ────────────────────────────────────────────────────
    // Worker-safe. Select `a_spell` in the follower's right hand (APMF cast-select) so
    // his own AI casts THAT spell. Call every winning cast tick; switches the selection
    // when the gambit's spell changes (re-pointed in place on a v3 APMF). No-op when
    // APMF is absent or Config::g_apmfCast is off.
    void SelectCastSpell(RE::FormID a_follower, RE::FormID a_spell);

    // Worker-safe. Release ONLY the cast-select claim now (the combat-target claim, if
    // any, is left alone). Call the instant no cast rule holds (Scheduler !castSeen) so
    // the spell selection releases crisply, not after the 500 ms expiry backstop.
    void ReleaseCastSpell(RE::FormID a_follower);

    // ── combat-TARGET: PER-COMBAT ────────────────────────────────────────────────
    // Worker-safe. HOLD combat-target on `a_target` for the follower and RE-POINT it in
    // place when the target changes (same claim/handle — never a release+re-request, so
    // a mid-battle retarget is not a release). `a_create`: true from a CAST directive
    // (creates the claim if none — the caster's fight); false from a non-cast combat
    // directive (attack) — refreshes/re-points ONLY an existing claim, never creating
    // one, so a pure-melee follower keeps MFO's own targeting. No-op when APMF is absent
    // or Config::g_apmfCast is off.
    void HoldCombatTarget(RE::FormID a_follower, RE::FormID a_target, bool a_create);

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

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
