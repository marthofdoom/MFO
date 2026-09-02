#pragma once
#include <RE/Skyrim.h>

// ─────────────────────────────────────────────────────────────────────────────
// APMF integration — MFO as an APMF client (Phase 3: the OWNED cast gambit). APMF
// (AI Package Management Framework) is a SEPARATE SKSE DLL; MFO talks to it ONLY
// through the byte-shared, append-only header `APMF_API.h` (the same discipline MFO
// applies to `MEO_API.h`) plus APMF's runtime interface query. APMF holds ZERO
// MFO-specific code; this bridge is the ONLY MFO code that knows APMF exists.
//
// WHAT THIS WIRES — the OWNED CAST MODEL (marth 2026-09-02). For a HOSTILE cast
// gambit, MFO asks APMF to OWN both halves of the shot on the follower:
//   * cast-select  (kIntent_SelectSpell)  := the gambit's spell   -> the AI selects it
//   * combat-target(kIntent_CombatTarget) := the gambit's target  -> APMF HOLDs it
// With the spell selected, the target held, and MFO's CasterConsent granting the
// follower's own AI consent to cast it, the follower fires the RIGHT spell at the
// RIGHT target through the engine's NORMAL cast flow — i.e. it plays the FULL cast
// ANIMATION/pose ("the animated path"). This is the ANIMATED-FIRST path and the
// point of the owned model: it resolves MFO's long-deferred cast-animation gap
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
// (worker touches OwnHostileCast/Tick; kDataLoaded/kPreLoadGame touch
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

    // Worker-safe. Own a follower's HOSTILE cast: select `a_spell` (right hand) AND
    // HOLD combat-target on `a_target` so the follower's own AI fires the selected
    // spell at the held target — animated. `a_target` 0 means "no target claim"
    // (e.g. a self/no-foe cast — spell only). Idempotent while spell/target are
    // unchanged (just refreshes the keep-alive); switches a claim when the gambit's
    // spell or target changes. No-op when APMF is absent or Config::g_apmfCast is off.
    // Call it every tick the owned cast gambit wins — claims are kept alive by these
    // refreshes and auto-released by Tick() once they stop (gambit ended / target lost).
    void OwnHostileCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target);

    // Worker-safe. Release `a_follower`'s owned-cast claims now (explicit end).
    void ReleaseCast(RE::FormID a_follower);

    // Worker-safe. Once-per-pump sweep: release any follower's claims not refreshed by
    // a recent OwnHostileCast (the gambit stopped choosing an owned cast). Call from
    // the pump body.
    void Tick();

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
