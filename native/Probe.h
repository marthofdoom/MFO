#pragma once
#include "PCH.h"

// M4 — the stick-poking harness. ROADMAP.md.
//
// Fires ONE engine primitive at a chosen follower and records what happened.
// No evaluator, no rules. It answers questions that cannot be answered by
// reading source, because they are emergent engine behaviour.
//
// ── WHAT M4 ALREADY TAUGHT US, BEFORE RUNNING ────────────────────────────
// A Fable review of the first draft found that FIVE of the twelve primitives
// DESIGN.md §4.5 lists as Tier B have NO C++ BINDING in CommonLibSSE-NG:
//
//     KeepOffsetFromActor / ClearKeepOffsetFromActor   Papyrus-only
//     SetDontMove                                      Papyrus-only
//     DoCombatSpellApply                               Papyrus-only
//     StartCombat                                      po3's fork only
//
// The Papyrus surface named the right engine flows (§4.5a stands), but the
// library does not expose them. Reaching them means either dispatching
// through the Papyrus VM or relocating the engine implementation with a
// SOURCED address — and those are different mechanisms with different risks,
// so they are their own milestone rather than a footnote in this one.
//
// This harness therefore ships ONLY primitives verified present in the pinned
// CommonLibSSE-NG, plus StartCombat via po3's published relocation. The
// unavailable ones appear in the UI marked as such, because a probe that
// silently omits them would hide the most important thing M4 discovered.

namespace MFO::Probe {

    enum class Action : std::uint8_t {
        StartCombatOnNearestFoe,   // via po3's published relocation
        StopCombat,
        CastHealInstant,
        CastHealRightHand,
        CastHealLeftHand,
        CastHealOther,
        CommandTargetAtCrosshair,   // THE trigger for the §0.14 hook
        ClearCommandedTarget,
        EvaluatePackage,
        DrawWeapon,
        SheatheWeapon,
        kCount
    };

    const char* Name(Action a_action);
    const char* Blurb(Action a_action);

    // Primitives DESIGN.md §4.5 wants that the library does not bind. Listed
    // so the gap is visible in game, not just in a doc.
    struct Unavailable {
        const char* name;
        const char* why;
    };
    std::span<const Unavailable> UnavailableActions();

    // Fire one primitive at a follower. MAIN THREAD ONLY.
    void Fire(RE::FormID a_followerID, Action a_action);

    struct Result {
        std::string action, subject, detail;
        bool ok = false;
        bool valid = false;
    };
    Result GetLast();

    // TARGET RETENTION WATCH — the load-bearing measurement.
    //
    // After StartCombat, sample the follower's ACTUAL combat target each tick.
    // If the controller re-picks quickly, DESIGN.md §4.7's "issue once, stay
    // quiet until invalidated" model needs a refresh cadence instead.
    //
    // Compared by HANDLE, never by name: two enemies both called "Bandit" would
    // make a re-pick invisible, and a brief target→none→target blip would read
    // as two changes.
    struct Retention {
        std::string follower, commanded, current;
        float watchSeconds = 0.0f;    // how long the watch has run
        float heldSeconds  = 0.0f;    // how long the COMMANDED target survived
        int   samples = 0;
        int   changes = 0;
        bool  onCommanded = false;    // still attacking who we said?
        bool  everEngaged = false;    // did StartCombat take at all?
        bool  commandedDied = false;  // invalidation, NOT a re-pick
        bool  active = false;
        bool  valid = false;
    };
    Retention GetRetention();

    // What the player is looking at. The commanded-target probe needs a way to
    // say WHO, and the crosshair is the one the player already uses.
    void RegisterCrosshairSink();
    RE::FormID CrosshairTarget();

    void Tick();      // from the pump; cheap no-op when idle
    void Stop();      // ends the watch and releases anything the probe rooted
    void ReleaseAll();// on load/new game — nothing the probe did outlives a session

}
