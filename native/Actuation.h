#pragma once
#include "PCH.h"
#include "Evaluator.h"

// Actuation. DESIGN.md §4.5 Tier A. The ONLY module that mutates actor state.
// Main thread only.

namespace MFO::Actuation {

    enum class Result : std::uint8_t {
        Fired,          // the action executed
        NoOp,           // deliberately nothing. A BLANK reason means truly nothing
                        // (act.wait, no rule matched); a non-blank reason is a
                        // decision MFO made and IS logged on transition.
        FailedSkill,    // the follower could not afford/execute it -> fall through
        FailedOther,
    };

    struct Outcome {
        Result      result = Result::NoOp;
        std::string reason;   // for the board / log when it did not fire
    };

    // Execute a chosen action on a follower. Returns what happened so the
    // scheduler can suppress on a real Fire and record a reason on a failure
    // (§5.3 -- a rule that could not run says why, it is not silent).
    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice);

}
