#pragma once
#include "PCH.h"

// The evaluator's tick. DESIGN.md §4.1 / §4.1a.
//
// M5 FIRST SLICE — deliberately the simple form:
//   * a ~133 ms chrono tick (the §4.1 response deadline), no frame clock yet
//   * round-robin ONE follower per tick, so cost is O(1) in party size
//   * POSITIONAL suppression after a real action -- a higher rule always
//     preempts the window (INVARIANTS #26); it is never an absolute mute
//   * combat table only, gated on IsInCombat (§4.8: the tables never interleave)
//   * NO jitter, NO urgency tiers, NO distance LOD, NO logistics table
//
// Every one of those omissions is designed in §4.1b/§4.2/§4.4 and is a later
// slice. They are refinements to a loop that must first be shown to work at
// all; shipping them together would make a misbehaviour impossible to bisect.

namespace MFO::Scheduler {

    // Called from the pump on the main thread. Cheap no-op until the tick is due.
    void Tick();

    // Save-scoped: suppression state and the round-robin cursor. Cleared on
    // revert like every other save-scoped map (INVARIANTS #22i's lesson --
    // a stale streak carried across saves re-opened a fixed bug).
    void ClearTransientState();

    // Last measured tick cost. Surfaced via bProfileEvaluator in the log; the
    // Field Kit does not read these until the rule board lands (M7), so for
    // M5 the log is the only place per-rule outcomes are visible.
    double LastTickMs();
    std::uint32_t TicksThisSession();

}
