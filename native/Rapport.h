#pragma once
#include "PCH.h"

// Rapport accrual. DESIGN.md §5.1, BALANCE.md §1-2.
//
// Per-follower, never pooled, never transferable. Earned by fighting
// TOGETHER; survives dismissal.

namespace MFO::Rapport {

    void RegisterSinks();

    // Rank from cumulative rapport, using the live BALANCE thresholds.
    std::uint8_t RankFor(std::uint32_t a_rapport);

    // Award to one follower and log any rank change. Main thread only.
    void Award(RE::FormID a_actorID, float a_amount, const char* a_reason);

    // Counters for the diagnostic dump (TEST_GUIDE 2C/2D). Without these the
    // combat-dispatch volume question cannot be answered at all.
    std::uint32_t CombatEventCount();
    std::uint32_t SessionKills();
    std::uint32_t SessionRapport();
    double        SessionMinutes();

    // The last kill MFO credited, so "why was that not a boss?" is answerable
    // in game rather than only in a log nobody can read mid-fight.
    struct LastKill {
        std::string name;
        std::uint16_t victimLevel = 0;
        std::uint16_t playerLevel = 0;
        std::string   kind;        // standard / boss / dragon
        float         awarded = 0.0f;
        int           credited = 0;
        bool          valid = false;
    };
    LastKill GetLastKill();

    // #63 ally-combat quash, the THREAD-SAFE half (v1.0.53 deck freeze).
    // Defers StopCombat on BOTH actors (plus a best-effort combat-target
    // drop) OUT of the caller's stack -- to the main-thread pump, or to the
    // SKSE task queue on VR where the pump is a no-op -- so it never runs
    // inline in a TESCombatEvent dispatch and never on the caller's worker
    // tick: the two call stacks whose collision froze the game. A per-PAIR
    // ~2 s cooldown (canonical key, so (A,B) and (B,A) share one entry) kills
    // the re-quash churn. Returns true when the quash was dispatched, false
    // while the pair is still cooling -- callers log their [peace] line only
    // on true, so field logs keep meaning "one line per actual quash".
    // Callable from any thread.
    bool QuashAllyPair(RE::FormID a_actor, RE::FormID a_target);

    // Save-scoped; called from RevertCallback.
    void ResetSessionCounters();

}
