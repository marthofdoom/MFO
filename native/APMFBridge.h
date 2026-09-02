#pragma once
#include <RE/Skyrim.h>

// ─────────────────────────────────────────────────────────────────────────────
// APMF integration — MFO as an APMF client (Phase 3 vertical slice: casting
// selection). APMF (AI Package Management Framework) is a SEPARATE SKSE DLL; MFO
// talks to it ONLY through the byte-shared, append-only header `APMF_API.h` (the
// same discipline MFO applies to `MEO_API.h`) plus APMF's runtime interface query.
// APMF holds ZERO MFO-specific code; this bridge is the ONLY MFO code that knows
// APMF exists.
//
// WHAT THIS WIRES. When an MFO cast gambit wants follower F to cast spell S, MFO
// already FORCE-delivers the cast (CastSpellImmediate — see Docs/CAST-DELIVERY.md).
// This additive layer ALSO asks APMF's cast-select channel to set F's OWN right-
// hand spell SELECTION to S, so when F's own combat AI casts, it casts the RIGHT
// spell (the long-standing "cast X gambit but the AI casts its own spell" gap).
// It is a SELECTION layer on TOP of MFO's delivery, never a replacement.
//
// DEGRADE-WHEN-ABSENT. APMF is a declared prerequisite, but MFO must never hard-
// fail without it: Acquire() logs once and leaves the interface null; every call
// here no-ops when APMF is absent (or when Config::g_apmfCast is off), so MFO's
// current casting is byte-identical without APMF.
//
// THREADING (APMF's contract, APMF_API.h). Request/RequestEx/Release are safe from
// ANY thread — they copy POD and enqueue; APMF applies on the game thread. MFO's
// cast dispatch (Fire) and the per-pump sweep (Tick) run on the AddTask job worker,
// so they may call these directly. This bridge's own claim map is guarded by a
// mutex (worker touches SelectSpell/Tick; kDataLoaded/kPreLoadGame touch
// Acquire/ClearTransientState).
//
// NO SAVE/CO-SAVE STATE. Cast selection is runtime-only; nothing here is
// serialized. On kPreLoadGame ClearTransientState() drops every claim (APMF wipes
// its own control map there too, so a stale handle Release is a harmless no-op).
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::APMFBridge {

    // Fetch the APMF C-ABI interface. Call ONCE at kDataLoaded (APMF.dll is loaded
    // by then). Null / logs "absent" if APMF is not in the load order or too old.
    void Acquire();

    // Is the APMF interface live (present + ABI >= 2, so RequestEx is available)?
    bool Available();

    // Worker-safe. Ask APMF to make follower `a_follower`'s OWN AI select `a_spell`
    // (right hand) so an AI-first cast casts the right spell. Idempotent while the
    // spell is unchanged (just refreshes the keep-alive); switches the claim if the
    // gambit's spell changes. No-op when APMF is absent or Config::g_apmfCast is off.
    // Call it every tick a cast gambit picks a spell — the claim is kept alive by
    // these refreshes and auto-released by Tick() once they stop (gambit ended /
    // target lost).
    void SelectSpell(RE::FormID a_follower, RE::FormID a_spell);

    // Worker-safe. Release `a_follower`'s cast-select claim now (explicit end).
    void ReleaseSpell(RE::FormID a_follower);

    // Worker-safe. Once-per-pump sweep: release any claim not refreshed by a recent
    // SelectSpell (the gambit stopped choosing a cast). Call from the pump body.
    void Tick();

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump
    // is drained (so no worker tick races the map).
    void ClearTransientState();
}
