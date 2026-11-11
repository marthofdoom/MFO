#pragma once
// THE RANK SEAM'S ARITHMETIC (IAUS card I1, design 1.3). PURE: standard headers and iaus/Core.h only, so
// tools/rank_equiv builds it on the host and checks it against the expressions it replaced.
//
// Eval::Choice::rank is the ORDER and the IDENTITY of a choice: lower outranks, -1 = no choice. A table
// choice's rank IS its row index (OfTableRow is the identity), so every comparison the scan made on
// `ruleIndex` before I1 is the same comparison on `rank` now. An IAUS choice carries Iaus::RankOf(epoch),
// which is never below Iaus::kRankFloor and so never collides with a row; the two never mix on one follower.
//
// Rank is CARRIED, never inferred (no tenure): the preemption tests compare the stored rank of the
// standing decision with the rank of the asking one, nothing else.

#include <cstdint>

#include "iaus/Core.h"

namespace MFO::Scheduler::Rank {

    inline constexpr int kNone = -1;   // "no choice" (Evaluate matched nothing): the old ruleIndex == -1

    // FLWR stores a table's row count in one byte (Serialization.cpp gambitCount), so a row index is <= 254.
    inline constexpr int kMaxTableRow = 254;
    // Iaus::RankOf clamps the epoch to kRankBase - 1000 (iaus/Decide.cpp), so an IAUS rank is >= 1000.
    inline constexpr int kIausRankFloor = 1000;
    static_assert(kMaxTableRow < kIausRankFloor && kIausRankFloor < Iaus::kRankBase,
                  "a table rank must never reach the IAUS rank range");

    [[nodiscard]] constexpr int  OfTableRow(int a_row) { return a_row; }
    [[nodiscard]] constexpr bool IsChoice(int a_rank) { return a_rank >= 0; }

    // The table walk resumes past a passed-over row (Evaluate's a_startIndex).
    [[nodiscard]] constexpr int NextTableRow(int a_rank) { return a_rank + 1; }

    // The rows evaluated this lap are [0, stop): the stopping row, or the whole table when the walk ran out
    // (rank < 0). The reconciliation's "evaluated above the stop" reads use it.
    [[nodiscard]] constexpr int TableStop(int a_stopRank, int a_rows) { return a_stopRank < 0 ? a_rows : a_stopRank; }

    // POSITIONAL SUPPRESSION (INVARIANTS #26): a window is live only with an anchor, and the walk stops cold at
    // the first candidate that does not outrank the decision that bought it.
    [[nodiscard]] constexpr bool WindowActive(bool a_inWindow, int a_anchor) { return a_inWindow && a_anchor >= 0; }
    [[nodiscard]] constexpr bool StopsAtAnchor(bool a_windowActive, int a_rank, int a_anchor) {
        return a_windowActive && a_rank >= a_anchor;
    }

}
