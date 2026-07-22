#pragma once
#include "PCH.h"
#include "State.h"

// The evaluator. DESIGN.md §4.3. PURE READS -- no engine mutation, ever
// (INVARIANTS #23). Given a follower and their combat table, returns the first
// rule whose condition is true, top-down. That is the whole contract.

namespace MFO::Eval {

    struct Choice {
        int         ruleIndex = -1;    // -1 = no rule matched
        std::string actionOpcode;
        RE::FormID  actionParam = 0;
        std::uint8_t subject = 0;

        // The foe a selector condition chose. Only set by foe selectors; empty
        // for self/player rules. Carried as a HANDLE, never a pointer -- the
        // scheduler acts on it after the evaluator returns (#2).
        RE::ActorHandle target;
    };

    // Scan a_follower's combat table top-down; first true condition wins.
    // Reads a_follower's and (for player conditions) the player's state -- both
    // named explicitly, neither defaulted (INVARIANTS #14).
    //
    // Returns ruleIndex == -1 when nothing matched: the caller then makes NO
    // engine call at all (§4.4's do-nothing guarantee).
    Choice Evaluate(RE::Actor* a_follower, const FollowerState& a_state);

}
