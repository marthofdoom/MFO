// IAUS card I1: host-side equivalence check of the rank seam (native/scheduler/Rank.h).
//
// Every order / sentinel test the combat scan made on Eval::Choice::ruleIndex before I1 is written out here
// VERBATIM (the old* functions), and compared with the Rank:: helper the scan calls now, over every pair of
// ranks in the domain: the "no rule" sentinel -1, every table row a save can hold (0..254), the out-of-range
// ints, Actuation's kNoRule (INT_MAX, the unranked lock) and the IAUS ranks Iaus::RankOf hands out.
// It also checks the cross-source facts the seam relies on (a table rank never reaches the IAUS range, every
// rank outranks kNoRule, RankOf is strictly decreasing in the epoch).
//
// Build + run: tools/rank_equiv/run.sh (host g++, no CommonLib). Exit code = number of failed checks.

#include <climits>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "scheduler/Rank.h"

namespace R = MFO::Scheduler::Rank;

namespace {
    // ── The pre-I1 expressions, verbatim (file:line on main 9372a6e) ─────────────────────────────────────────
    // CombatScan.cpp:964 / :755 `if (choice.ruleIndex < 0) break;`, Fire.cpp:193 `a_choice.ruleIndex < 0`
    bool oldNoChoice(int ruleIndex) { return ruleIndex < 0; }
    // CombatScan.cpp:932 `const bool windowActive = inWindow && firedRuleNow >= 0;`
    bool oldWindowActive(bool inWindow, int firedRuleNow) { return inWindow && firedRuleNow >= 0; }
    // CombatScan.cpp:983 `if (windowActive && choice.ruleIndex >= firedRuleNow)`
    bool oldStopsAtAnchor(bool windowActive, int ruleIndex, int firedRuleNow) {
        return windowActive && ruleIndex >= firedRuleNow;
    }
    // CombatScan.cpp:1197-1199 / :1219 / :1234 / :1252
    // `(choice.ruleIndex < 0) ? static_cast<int>(rules.size()) : choice.ruleIndex`
    int oldStopIdx(int ruleIndex, int rows) { return (ruleIndex < 0) ? rows : ruleIndex; }
    // CombatScan.cpp:756 / :1009 / :1033 / :1131 `start = choice.ruleIndex + 1;`
    int oldNextStart(int ruleIndex) { return ruleIndex + 1; }
    // CombatScan.cpp:929 `firedRuleNow = i;` (the uid re-resolve)
    int oldAnchorOfRow(int i) { return i; }

    constexpr int kNoRule = INT_MAX;   // cast/Actuation_internal.h:200 (the unranked lock)

    int g_fail = 0;
    long g_checks = 0;
    void Check(bool ok, const char* what, long a, long b) {
        ++g_checks;
        if (!ok) {
            ++g_fail;
            if (g_fail <= 20) std::printf("FAIL %s (a=%ld b=%ld)\n", what, a, b);
        }
    }
}

int main() {
    std::vector<int> ranks;
    for (int r = -3; r <= R::kMaxTableRow + 2; ++r) ranks.push_back(r);   // sentinel, every row, just past
    ranks.push_back(INT_MIN);
    ranks.push_back(kNoRule);
    const std::uint32_t epochs[] = { 0u, 1u, 2u, 1000u, 123456u, 999'998'999u, 999'999'000u, 999'999'001u,
                                     0x7FFFFFFFu, 0xFFFFFFFFu };
    for (auto e : epochs) ranks.push_back(MFO::Iaus::RankOf(e));

    // 1. Single-rank tests.
    for (int r : ranks) {
        Check(oldNoChoice(r) == !R::IsChoice(r), "IsChoice", r, 0);
        if (r != INT_MAX) Check(oldNextStart(r) == R::NextTableRow(r), "NextTableRow", r, 0);
        for (int rows : { 0, 1, 2, 12, 64, 254, 255 })
            Check(oldStopIdx(r, rows) == R::TableStop(r, rows), "TableStop", r, rows);
    }
    // 2. Every (rank, anchor) pair, both window states.
    for (int a : ranks)
        for (bool inWindow : { false, true }) {
            const bool waOld = oldWindowActive(inWindow, a);
            const bool waNew = R::WindowActive(inWindow, a);
            Check(waOld == waNew, "WindowActive", a, inWindow);
            for (int r : ranks)
                for (bool wa : { false, true })
                    Check(oldStopsAtAnchor(wa, r, a) == R::StopsAtAnchor(wa, r, a), "StopsAtAnchor", r, a);
        }
    // 3. A table row is its rank.
    for (int i = 0; i <= R::kMaxTableRow; ++i) {
        Check(R::OfTableRow(i) == i, "OfTableRow", i, 0);
        Check(oldAnchorOfRow(i) == R::OfTableRow(i), "anchor of row", i, 0);
    }
    // 4. Cross-source facts.
    for (auto e : epochs) {
        const int ir = MFO::Iaus::RankOf(e);
        Check(ir >= R::kIausRankFloor, "IAUS rank >= floor", ir, e);
        Check(ir > R::kMaxTableRow, "IAUS rank above every table row", ir, e);
        Check(ir < kNoRule, "IAUS rank outranks kNoRule", ir, e);
        Check(R::IsChoice(ir), "IAUS rank is a choice", ir, e);
    }
    for (std::uint32_t e = 0; e < 5000; ++e)
        Check(MFO::Iaus::RankOf(e + 1) < MFO::Iaus::RankOf(e), "RankOf strictly decreasing", e, 0);
    for (int i = 0; i <= R::kMaxTableRow; ++i) Check(R::OfTableRow(i) < kNoRule, "row outranks kNoRule", i, 0);

    std::printf("rank_equiv: %ld checks, %d failed\n", g_checks, g_fail);
    return g_fail;
}
