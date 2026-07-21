#pragma once
#include "PCH.h"

// M4 — the stick-poking harness. ROADMAP.md.
//
// Fires ONE engine primitive at a chosen follower and records what happened.
// No evaluator, no rules. Its whole purpose is to answer questions that cannot
// be answered by reading source, because they are emergent engine behaviour
// that no mod documents and no header states:
//
//   * Does a commanded combat target STICK, or does the combat controller
//     re-pick within seconds? -- DESIGN.md §4.7's entire standing-order model
//     depends on the answer, and getting it wrong at M9 is far more expensive
//     than getting it now.
//   * Does CastSpellImmediate read as a real combat action, or does
//     DoCombatSpellApply behave differently?
//   * Does KeepOffsetFromActor survive what we think it survives, and does it
//     really fight the combat controller the way Kaidan's code implies?
//   * Does EvaluatePackage need the condition flicker from native code?
//
// Everything here is REVERSIBLE and ledger-free by construction: no package
// overrides, no persisted state, nothing that outlives the session.

namespace MFO::Probe {

    enum class Action : std::uint8_t {
        StartCombatOnNearestFoe,
        StopCombat,
        CastHealOnSelf,
        DoCombatSpellApplyOnTarget,
        KeepOffsetClose,
        KeepOffsetFar,
        ClearKeepOffset,
        SetDontMoveOn,
        SetDontMoveOff,
        EvaluatePackage,
        DrawWeapon,
        SheatheWeapon,
        kCount
    };

    const char* Name(Action a_action);
    const char* Blurb(Action a_action);

    // Fire one primitive at a follower. MAIN THREAD ONLY.
    void Fire(RE::FormID a_followerID, Action a_action);

    // What the last probe did, and what happened afterwards.
    struct Result {
        std::string action;
        std::string subject;
        std::string detail;      // immediate outcome
        bool        ok = false;
        bool        valid = false;
    };
    Result GetLast();

    // TARGET RETENTION WATCH -- the load-bearing measurement.
    //
    // After StartCombat, sample the follower's actual combat target every tick
    // and record how long our chosen target survived. If the controller
    // re-picks quickly, §4.7's "issue once and stay quiet" model needs a
    // refresh cadence instead of pure invalidation.
    struct Retention {
        std::string follower;
        std::string commanded;      // what we told them to attack
        std::string current;        // what they are actually attacking now
        float       heldSeconds = 0.0f;
        int         samples = 0;
        int         changes = 0;    // times the engine picked something else
        bool        active = false;
    };
    Retention GetRetention();

    // Called from the pump each tick. Cheap no-op when no watch is running.
    void Tick();

    void Stop();

}
