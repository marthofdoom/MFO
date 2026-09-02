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
    // release). `a_create`: true from a CAST directive (creates the claim if none); false
    // from a non-cast directive (attack) — re-points ONLY an existing claim, never
    // creating one, so a pure-melee follower is not claimed. No-op when APMF is absent or
    // Config::g_apmfCast is off.
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

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
