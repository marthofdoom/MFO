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

    // Scan ONE of a_follower's tables top-down; first true condition wins.
    // Reads a_follower's and (for player conditions) the player's state -- both
    // named explicitly, neither defaulted (INVARIANTS #14).
    //
    // a_table selects combat() or logistics() -- the two never interleave
    // (§4.8), so the caller decides which by the follower's combat state, and a
    // single scan function serves both because first-match-wins is identical.
    // Foe selectors only ever appear in the combat table; out of combat they
    // find no combat group and fall through harmlessly, so no special-casing.
    //
    // Returns ruleIndex == -1 when nothing matched: the caller then makes NO
    // engine call at all (§4.4's do-nothing guarantee).
    //
    // a_startIndex lets BOTH callers resume the scan past a rule that matched
    // but whose action did nothing: logistics past a "loot potions" with no
    // potions nearby, and the combat scheduler past any TRANSPARENT outcome
    // (satisfied equip, unaffordable cast, empty potion stack -- GAMBIT_FLOWS
    // §2/H1), so a near-always-true rule cannot SHADOW the useful rules below
    // it. Opaque outcomes (act.wait, a latched attack, the cast-grace hold)
    // still consume the tick -- the caller decides; this function just scans.
    Choice Evaluate(RE::Actor* a_follower, const FollowerState& a_state,
                    Table a_table = Table::Combat, int a_startIndex = 0);

}
