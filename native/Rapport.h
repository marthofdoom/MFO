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

    // Save-scoped; called from RevertCallback.
    void ResetSessionCounters();

}
