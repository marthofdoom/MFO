#pragma once
// cast/SeatRows.h -- the PURE half of adopting Harbinger ABI v46 (seated Stagger / Disarm / TargetEffect / Paralyze casters).
// Standard types only (no CommonLib, no engine), so tools/iaus_harness/harness.cpp reads the very same answers as the game.
//
// WHAT THIS HOLDS (the one table and the small readers; "to add or change a row, edit kRows"):
//   * kRows: the four engine caster rows Harbinger v46 can seat, each with its CastSeatCoverage bit (APMF_API.h, mirrored
//     here as literals and static_assert-ed against the header in cast/Archetype.cpp), the engine's reach GMST (name + the
//     vanilla default) and what the engine used to ask of the target (the "need"). Harbinger no longer asks the engine
//     "does this work on this target" for a claim on these rows (APMF_API.h CastSeatCoverage: "THE CLIENT'S PART"), so MFO
//     asks it: cast/WillWork.cpp reads `need`, cast/SpellBook (iaus) reads `reachGmst` as a RANGE NEED on the candidate.
//   * RowSeated: the v46 rule, a row is served when its own bit AND kTypeEquip are set. Below ABI 46 the answer is the old
//     static one (not served).
//   * HoldLedger: the (spell, target) pairs held off for the fight after a cast that fired with no effect (cast/NoEffect.cpp).
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace MFO::SeatRows {

    enum class Row : std::uint8_t { Stagger, Disarm, TargetEffect, Paralyze, kCount };

    // What the engine's equip condition asked of the target before v46 (and the client now asks itself).
    enum class Need : std::uint8_t {
        None,
        Weapon,     // Disarm: the target holds a weapon (drawn or equipped) that can be dropped
        Summoned    // Banish / Command: the target is a summoned (commanded) creature
    };

    struct RowInfo {
        Row           row;
        const char*   name;
        std::uint32_t coverageBit;   // APMF_API::CastSeatCoverage (v46): kCastSeat_Stagger .. kCastSeat_Paralyze
        const char*   reachGmst;     // the engine's distance setting (nullptr = the engine asks no distance)
        float         reachDefault;  // vanilla value of that setting, used when the setting cannot be read
    };

    inline constexpr std::uint32_t kTypeEquipBit = 1u << 16;   // APMF_API::kCastSeat_TypeEquip (v46)
    inline constexpr std::uint32_t kMinAbiCoverage = 46;       // castSeats is valid from this abiVersion

    // THE TABLE. Sources: Harbinger Docs/FACETS.md ch.8b "The client's part" (256 / 256 / 384, the GMST names) and
    // APMF_API.h CastSeatCoverage. The GMST is read live (a list may change it); the default is only the fallback.
    inline constexpr RowInfo kRows[] = {
        { Row::Stagger,      "Stagger",      1u << 10, "fCombatMagicStaggerDistance",  256.0f },
        { Row::Disarm,       "Disarm",       1u << 11, "fCombatMagicDisarmDistance",   256.0f },
        { Row::TargetEffect, "TargetEffect", 1u << 12, nullptr,                          0.0f },
        { Row::Paralyze,     "Paralyze",     1u << 13, "fCombatMagicParalyzeDistance", 384.0f },
    };

    [[nodiscard]] constexpr const RowInfo& Info(Row a_row) { return kRows[static_cast<std::size_t>(a_row)]; }

    // A row is served when Harbinger reports it covered AND the equip-condition admission is installed. abi < 46: no coverage
    // exists, so the answer is "not served" (the old static kUnseatedRows behaviour).
    [[nodiscard]] constexpr bool RowSeated(std::uint32_t a_abi, std::uint32_t a_coverage, Row a_row) {
        return a_abi >= kMinAbiCoverage && (a_coverage & Info(a_row).coverageBit) != 0 && (a_coverage & kTypeEquipBit) != 0;
    }

    // The reach the engine accepts for a row, from the live setting (a_gmst, <= 0 or not finite = unreadable) or the default.
    // 0 = the row has no distance of its own.
    [[nodiscard]] constexpr float RowReach(Row a_row, float a_gmst) {
        const RowInfo& i = Info(a_row);
        if (i.reachGmst == nullptr) return 0.0f;
        return (a_gmst > 0.0f && a_gmst < 1.0e8f) ? a_gmst : i.reachDefault;
    }

    // A RANGE NEED, not a refusal: the candidate's reach is its own, capped to the row's (0 = no reach known stays 0 unless the
    // row has one, then the row's: a spell with no range of its own is still bound by the row's). Reposition::SpellRange then
    // turns the capped reach into the distance he closes to.
    [[nodiscard]] constexpr float CapReach(float a_reach, float a_rowReach) {
        if (!(a_rowReach > 0.0f)) return a_reach;
        if (!(a_reach > 0.0f)) return a_rowReach;
        return a_reach < a_rowReach ? a_reach : a_rowReach;
    }

    // The target-side test of a Need. a_holdsDroppableWeapon: an equipped weapon in either hand that is not flagged cannot-drop.
    [[nodiscard]] constexpr bool NeedMet(Need a_need, bool a_holdsDroppableWeapon, bool a_summoned) {
        switch (a_need) {
        case Need::Weapon:   return a_holdsDroppableWeapon;
        case Need::Summoned: return a_summoned;
        default:             return true;
        }
    }

    // THE ZERO-EFFECT HOLD (cast/NoEffect.cpp): a cast of a hostile-row spell fired and left no effect on that target. Held off
    // for the fight, per (spell, target). No lock here: the owner (NoEffect.cpp) holds one.
    class HoldLedger {
    public:
        [[nodiscard]] static constexpr std::uint64_t Key(std::uint32_t a_spell, std::uint32_t a_target) {
            return (static_cast<std::uint64_t>(a_spell) << 32) | a_target;
        }
        // true = newly held (the caller logs it once).
        bool Hold(std::uint32_t a_follower, std::uint32_t a_spell, std::uint32_t a_target) {
            return m_held[a_follower].insert(Key(a_spell, a_target)).second;
        }
        [[nodiscard]] bool Held(std::uint32_t a_follower, std::uint32_t a_spell, std::uint32_t a_target) const {
            const auto it = m_held.find(a_follower);
            return it != m_held.end() && it->second.count(Key(a_spell, a_target)) != 0;
        }
        void ClearFollower(std::uint32_t a_follower) { m_held.erase(a_follower); }
        void Clear() { m_held.clear(); }
        [[nodiscard]] std::size_t Size(std::uint32_t a_follower) const {
            const auto it = m_held.find(a_follower);
            return it == m_held.end() ? 0u : it->second.size();
        }
    private:
        std::unordered_map<std::uint32_t, std::unordered_set<std::uint64_t>> m_held;
    };

}
