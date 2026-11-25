#pragma once
// cast/SeatRows.h -- the PURE half of adopting Harbinger ABI v46 (seated Stagger / Disarm / TargetEffect / Paralyze casters).
// Standard types only (no CommonLib, no engine), so tools/iaus_harness/harness.cpp reads the very same answers as the game.
//
// WHAT THIS HOLDS (the one table and the small readers; "to add or change a row, edit kRows"):
//   * kRows: the four engine caster rows Harbinger v46 can seat, each with its CastSeatCoverage bit (APMF_API.h, mirrored
//     here as literals and static_assert-ed against the header in cast/Archetype.cpp). Harbinger no longer asks the engine
//     "does this work on this target" for a claim on these rows (APMF_API.h CastSeatCoverage: "THE CLIENT'S PART"), so MFO
//     asks it (cast/WillWork.cpp). NO reach cap: the engine's fCombatMagic*Distance settings are margins added to other
//     distances, not spell ranges (review F1), so the spell keeps its own reach.
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
        Weapon,     // Disarm: the target's right hand holds a weapon the engine's Disarm can take (WillWork DisarmableRightWeapon)
        Summoned    // Command (CommandSummoned): the target is a summoned (commanded) creature. Banish: conditions only
    };

    struct RowInfo {
        Row           row;
        const char*   name;
        std::uint32_t coverageBit;   // APMF_API::CastSeatCoverage (v46): kCastSeat_Stagger .. kCastSeat_Paralyze
    };

    inline constexpr std::uint32_t kTypeEquipBit = 1u << 16;   // APMF_API::kCastSeat_TypeEquip (v46)
    inline constexpr std::uint32_t kMinAbiCoverage = 46;       // castSeats is valid from this abiVersion

    // THE TABLE. Source: APMF_API.h CastSeatCoverage.
    inline constexpr RowInfo kRows[] = {
        { Row::Stagger,      "Stagger",      1u << 10 },
        { Row::Disarm,       "Disarm",       1u << 11 },
        { Row::TargetEffect, "TargetEffect", 1u << 12 },
        { Row::Paralyze,     "Paralyze",     1u << 13 },
    };

    [[nodiscard]] constexpr const RowInfo& Info(Row a_row) { return kRows[static_cast<std::size_t>(a_row)]; }

    // A row is served when Harbinger reports it covered AND the equip-condition admission is installed. abi < 46: no coverage
    // exists, so the answer is "not served" (the old static kUnseatedRows behaviour).
    [[nodiscard]] constexpr bool RowSeated(std::uint32_t a_abi, std::uint32_t a_coverage, Row a_row) {
        return a_abi >= kMinAbiCoverage && (a_coverage & Info(a_row).coverageBit) != 0 && (a_coverage & kTypeEquipBit) != 0;
    }

    // ABI v47 FIRE ONCE (APMF_API.h kCastFlag_FireOnce, "ABI v47: FIRE ONCE"), the pure half of its adoption (anim-residue plan A2).
    // A claim that carries the flag casts AT MOST ONCE on any caster type, so a spell whose effects map to several caster rows
    // (Mystic Wind: Script + Restore) can be claimed although a candidate row (Restore, Offensive) has no bound of its own.
    //   * kFireOnceBit / kMinAbiFireOnce: kCastSeat_FireOnce and the ABI that first reports it (the literals are static_assert-ed
    //     against the header in apmf/CastClaims.cpp).
    //   * kEngineRowBit: the CastSeatCoverage bit of the row's own seat, indexed by cast/Actuation.h EngineRow's ordinal (the order
    //     is static_assert-ed there). 0 = no seat (Unknown). A rowless spell (NoRow) is served as Script by Harbinger.
    //   * FireOnceServed: the whole gate. abi >= 47 AND the coverage carries kCastSeat_FireOnce AND EVERY candidate row of the mask
    //     has its OWN seat bit (the four v46 hostile rows also need kTypeEquip, as RowSeated). A Reanimate candidate needs the claim
    //     to be a corpse claim (kCastFlag_CorpseTarget, a_corpseClaim), else the claim is not fire-once. Harbinger keeps the engine's
    //     own answers on a row whose bit is clear, so a candidate without a bit would be an UNBOUNDED wrong caster: not served.
    // To adopt another row or flag: edit kEngineRowBit; the readers do not change.
    inline constexpr std::uint32_t kFireOnceBit    = 1u << 18;   // APMF_API::kCastSeat_FireOnce (v47)
    inline constexpr std::uint32_t kMinAbiFireOnce = 47;
    inline constexpr std::uint32_t kReanimateRow   = 16;         // EngineRow::Reanimate's ordinal
    inline constexpr std::uint32_t kEngineRowBit[17] = {
        1u << 9,    // NoRow (served as Script)
        0,          // Unknown
        1u << 1,    // Offensive
        1u << 0,    // Restore
        1u << 10,   // Stagger
        1u << 11,   // Disarm
        1u << 12,   // TargetEffect
        1u << 13,   // Paralyze
        1u << 9,    // Script
        1u << 2,    // Ward
        1u << 3,    // Summon
        1u << 4,    // Cloak
        1u << 5,    // Light
        1u << 6,    // Invisibility
        1u << 7,    // BoundItem
        1u << 8,    // Armor
        1u << 14,   // Reanimate (the corpse seat)
    };
    [[nodiscard]] constexpr bool FireOnceServed(std::uint32_t a_abi, std::uint32_t a_coverage, std::uint32_t a_rowMask,
                                                bool a_corpseClaim = false) {
        if (a_abi < kMinAbiFireOnce || (a_coverage & kFireOnceBit) == 0) return false;
        for (std::uint32_t r = 0; r < 17; ++r) {
            if ((a_rowMask & (1u << r)) == 0) continue;
            const std::uint32_t bit = kEngineRowBit[r];
            if (bit == 0 || (a_coverage & bit) == 0) return false;
            if (r >= 4 && r <= 7 && (a_coverage & kTypeEquipBit) == 0) return false;   // the v46 hostile rows
            if (r == kReanimateRow && !a_corpseClaim) return false;
        }
        return (a_rowMask >> 17) == 0;   // a bit past the table is a row this build does not know: not served
    }

    // The target-side test of a Need. a_holdsDroppableWeapon: an equipped weapon in either hand that is not flagged cannot-drop.
    [[nodiscard]] constexpr bool NeedMet(Need a_need, bool a_holdsDroppableWeapon, bool a_summoned) {
        switch (a_need) {
        case Need::Weapon:   return a_holdsDroppableWeapon;
        case Need::Summoned: return a_summoned;
        default:             return true;
        }
    }

    // THE ZERO-EFFECT READING (cast/NoEffect.cpp; review F2 / F3). One cast that fired is READ once, a_ageSec after that fire.
    //   Inconclusive: read too early or too late (the settle window is [kSettleSec, kMaxAgeSec]; later, a slow re-equip or a
    //     re-cast muddies it), a concentration spell, a spell that also carries another hostile instant effect (that one may have
    //     landed), or a lasting keyed effect whose whole duration is not longer than the age plus kMargin (it may simply have ended).
    //   Landed / Zero: the effect is on the target (or Disarm's weapon is gone) / is not.
    // Two ZERO readings from two distinct fires in a row hold the (spell, target) for the fight (kZerosToHold); a Landed reading
    // resets the count, an Inconclusive one leaves it. One miss, ward, absorb or resist is never "never works".
    enum class Reading : std::uint8_t { Inconclusive, Landed, Zero };
    inline constexpr float kSettleSec = 2.0f;
    inline constexpr float kMaxAgeSec = 6.0f;
    inline constexpr float kMarginSec = 1.0f;
    inline constexpr int   kZerosToHold = 2;
    struct ReadInput {
        bool  disarm = false;             // read the right-hand weapon instead of an effect
        bool  concentration = false;
        bool  otherHostileInstant = false;
        bool  hadSubject = true;          // Disarm: a disarmable weapon was seen in the right hand at the fire; else nothing to read
        float keyedDurationSec = 0.0f;    // the keyed effect's duration (0 = instant)
        float ageSec = 0.0f;              // now minus the fire's observed time
        bool  landed = false;             // the keyed effect is on the target / the weapon is no longer in his right hand
    };
    [[nodiscard]] constexpr Reading Judge(const ReadInput& a) {
        if (a.ageSec < kSettleSec || a.ageSec > kMaxAgeSec) return Reading::Inconclusive;
        if (a.concentration || a.otherHostileInstant) return Reading::Inconclusive;
        if (a.disarm) {
            if (!a.hadSubject) return Reading::Inconclusive;
        } else if (!(a.keyedDurationSec > a.ageSec + kMarginSec)) {
            return Reading::Inconclusive;
        }
        return a.landed ? Reading::Landed : Reading::Zero;
    }
    // The running count of consecutive Zero readings after one more reading.
    [[nodiscard]] constexpr int Tally(int a_zeros, Reading a_r) {
        return a_r == Reading::Zero ? a_zeros + 1 : a_r == Reading::Landed ? 0 : a_zeros;
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
