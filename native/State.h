#pragma once
#include "PCH.h"

// MFO authoritative state.
//
// THREADING (ARCHITECTURE.md §3.1, INVARIANTS.md #4):
//   g_followers is MAIN-THREAD-ONLY BY CONSTRUCTION and takes NO LOCK.
//   Event sinks queue to main via AddTask; serialization callbacks run on
//   main; board edits queue to main. If something needs this off-thread the
//   answer is a SNAPSHOT, never a lock -- a lock here would end up held
//   across engine calls and reintroduce MAO's re-entrant self-deadlock.

namespace MFO {

    // Serialized as a stable STRING opcode, never an enum ordinal
    // (INVARIANTS.md #10). These strings land in co-saves; renaming one is a
    // schema migration, not an edit.
    struct Gambit {
        std::string conditionOpcode;   // e.g. "cond.ally_hp_below"
        std::string actionOpcode;      // e.g. "act.cast_spell"
        float       conditionParam = 0.0f;
        RE::FormID  actionParamForm = 0;   // spell/item; ResolveFormID on load
        std::uint8_t subjectSelector = 0;
        bool        enabled = true;

        // Runtime-only, never serialized (ARCHITECTURE.md §7): display state
        // for the board's per-rule outcome. The evaluator never reads it back
        // (INVARIANTS.md #14 companion -- outcome is display-only).
        bool        lastFired = false;
        std::string lastFailReason;
    };

    // Package overrides PERSIST THROUGH SAVES (ENGINE_NOTES.md §2.1).
    // An override with no ledger entry is a bug and is logged as one.
    struct PackageOverride {
        RE::FormID   package = 0;
        std::uint8_t priority = 0;
    };

    // DESIGN.md 4.8: two independent tables per follower. They never
    // interleave -- combat runs in combat, logistics runs out of it -- and
    // they have separate slot ladders so upkeep rules never compete with
    // combat rules for the Rapport reward.
    enum class Table : std::uint8_t {
        Combat    = 0,
        Logistics = 1,
        kCount    = 2
    };

    struct FollowerState {
        std::uint32_t rapport = 0;
        std::uint8_t  rank = 1;                 // clamped [1,5] on load
        std::vector<Gambit> tables[static_cast<size_t>(Table::kCount)];
        std::vector<PackageOverride> overrides;

        std::vector<Gambit>&       combat()          { return tables[0]; }
        std::vector<Gambit>&       logistics()       { return tables[1]; }
        const std::vector<Gambit>& combat()    const { return tables[0]; }
        const std::vector<Gambit>& logistics() const { return tables[1]; }
    };

    inline constexpr std::uint8_t kMaxRank = 5;

    // Slots by rank -- DESIGN.md 5.2. Index 0 unused so rank indexes directly.
    // Logistics is deliberately shallow: 5 slots covers the whole 4.8
    // vocabulary (3 potions + ammo + equipment), so Rank V is fully
    // self-sufficient rather than merely closer to it.
    inline constexpr std::uint8_t kCombatSlotsByRank[kMaxRank + 1]    = { 0, 2, 4, 6, 8, 12 };
    inline constexpr std::uint8_t kLogisticsSlotsByRank[kMaxRank + 1] = { 0, 1, 2, 3, 4, 5 };

    inline std::uint8_t SlotsForRank(std::uint8_t a_rank, Table a_table) {
        const auto r = std::clamp<std::uint8_t>(a_rank, 1, kMaxRank);
        return a_table == Table::Combat ? kCombatSlotsByRank[r] : kLogisticsSlotsByRank[r];
    }

    // Keyed on the actor's PERSISTENT FormID. Dismissed followers keep their
    // board and Rapport (DESIGN.md 3.1), so membership here is independent of
    // current teammate status.
    inline std::unordered_map<RE::FormID, FollowerState> g_followers;

    void ResetAllState();   // RevertCallback

}
