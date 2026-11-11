#pragma once
#include "PCH.h"
#include "Config.h"   // g_debugUnlockSlots (one-way; Config.h pulls nothing back)

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
    // Monotonic runtime id for the board. Assigned when a rule is created or
    // loaded, NEVER serialized (#10 -- the co-save schema is unchanged). The
    // board keys edits on this, not on row index, so a reordered or shrunk
    // table cannot misapply a command to the wrong rule (#31).
    inline std::uint32_t NextRuleUID() {
        static std::atomic<std::uint32_t> g{ 1 };
        return g.fetch_add(1, std::memory_order_relaxed);
    }

    struct Gambit {
        std::uint32_t uid = 0;   // runtime-only; 0 = unassigned
        std::string conditionOpcode;   // e.g. "cond.ally_hp_below"
        std::string actionOpcode;      // e.g. "act.cast_spell"
        float       conditionParam = 0.0f;
        RE::FormID  actionParamForm = 0;   // spell/item; ResolveFormID on load
        std::uint8_t subjectSelector = 0;
        // #68: a SPECIFIC follower to target (a Cast-on-target row picked a
        // named ally, not the Self/Player/NearestAlly enum). 0 = use
        // subjectSelector instead; ResolveFormID on load, same as
        // actionParamForm -- unresolvable (the follower is gone) just falls
        // back to 0, never disables the rule (Actuation's ladder keeps going).
        RE::FormID  subjectActorForm = 0;
        bool        enabled = true;
        // Card G9: the per-rule cooldown in TENTHS OF A SECOND (u16, exact on the wire; 100 = 10 s). 0 = none.
        // SERIALIZED (FLWR v6, the gambit row, Serialization.h GambitWire), clamped [0, kMaxCooldownTenths] on
        // read and write. Any row may carry it; the board exposes it on power-attack rows only (marth Q7,
        // 2026-11-09). The last-fired time it is measured from is NOT here: it is runtime-only, in Cooldowns.h
        // (the display-only lastFiredAt below is never read by any gate).
        std::uint16_t cooldownTenths = 0;

        // Runtime-only, never serialized (ARCHITECTURE.md §7): display state
        // for the board's per-rule outcome. The evaluator never reads it back
        // (INVARIANTS.md #14 companion -- outcome is display-only).
        bool        lastFired = false;
        std::string lastFailReason;
        // When the rule last FIRED (flair #9: the board pulses the row for ~1 s
        // after this). Display-only like lastFired; steady_clock so the render
        // thread can age it without any cross-thread clock question. A default
        // (epoch) value reads as "long ago" -> no pulse.
        std::chrono::steady_clock::time_point lastFiredAt{};
    };

    // Card G9: the cooldown bound (600 s) and the seconds -> tenths conversion a class default row's
    // DefaultGambit::cooldownSec goes through (card C12 applies it; nothing applies class defaults yet).
    inline constexpr std::uint16_t kMaxCooldownTenths = 6000;
    inline std::uint16_t CooldownTenthsFromSec(float a_sec) {
        if (!(a_sec > 0.0f)) return 0;   // also NaN
        if (a_sec >= kMaxCooldownTenths / 10.0f) return kMaxCooldownTenths;
        return static_cast<std::uint16_t>(a_sec * 10.0f + 0.5f);
    }
    inline float CooldownSec(const Gambit& a_g) { return a_g.cooldownTenths / 10.0f; }

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
        // #65: a user-set per-follower combat class OVERRIDE that forces the
        // follower's CombatStyle::Stance, superseding the gambit-inferred one.
        // Deliberately EQUAL to CombatStyle::Stance's ordinals (0=Auto/None,
        // 1=Melee, 2=Ranged, 3=Cast) so Scheduler can cast it directly. 0 =
        // Auto = NO override -- custom-follower safety (#64): an un-
        // overridden follower is never touched by this feature. Clamped to
        // [0,3] on load (Serialization.cpp).
        std::uint8_t  combatClassOverride = 0;
        // T#78: the per-follower MFO master switch. ON by default. When false MFO
        // leaves this follower completely untouched -- no combat gambits, no
        // logistics/economy, no cast/equip/loot -- and the tick releases every
        // bit of state MFO was holding (forced equip, AI-package aliases, cast /
        // retreat latches) so he reverts to a vanilla/engine-default follower.
        // Serialized in FLWR v5 (Serialization.cpp), appended after
        // combatClassOverride and read gated on version >= 5; a pre-v5 record
        // never wrote it, so it defaults true (this FollowerState{} default) --
        // every existing follower stays enabled.
        bool          mfoEnabled = true;
        // ClickUp 86e3phy41 (marth 2026-10-12): the per-follower "Wear helmets" OVERRIDE.
        // 0 = Default (inherit the MCM bWearHelmets), 1 = Wear, 2 = Never. Read through
        // Logistics::WearsHelmets. NO co-save layout change: it rides bits 1-2 of the FLWR v5
        // mfoEnabled byte, written only while mfoEnabled is true (Serialization.cpp, so an
        // older DLL's `byte != 0` read is still exactly mfoEnabled). Clamped to [0,2] on load.
        std::uint8_t  helmetOverride = 0;
        // Card I5 (design Docs/research/2026-11/iaus-design.md section 7; marth Q31: OFF by default per
        // follower): the per-follower COMBAT IAUS toggle. true = the IAUS scorer (native/iaus/, MAP.md
        // section 11) picks his combat action in place of the combat table. NOBODY READS IT YET: I6 consumes
        // it, I7 gives it a board control. NO co-save layout change: it rides bit 3 of the FLWR v5 mfoEnabled
        // byte under the helmet override's rule (written only while mfoEnabled is true; FlwrEnabledByte.h).
        // Bit 4 of that byte is reserved for the logistics toggle (card 1dfcuabcr0g): it has NO field here,
        // is written 0 and is ignored on load.
        bool          iausCombat = false;
        std::vector<Gambit> tables[static_cast<size_t>(Table::kCount)];
        std::vector<PackageOverride> overrides;

        std::vector<Gambit>&       combat()          { return tables[0]; }
        std::vector<Gambit>&       logistics()       { return tables[1]; }
        const std::vector<Gambit>& combat()    const { return tables[0]; }
        const std::vector<Gambit>& logistics() const { return tables[1]; }
    };

    inline constexpr std::uint8_t kMaxRank = 5;

    // Slots by rank -- DESIGN.md 5.2. Index 0 unused so rank indexes directly.
    //
    // RANK I MUST HOLD THE DEFAULT KIT, or a co-save round-trip truncates it
    // (#11) and the base gambits a follower ships with silently vanish. The kit
    // (Followers::ApplyDefaultKit) is 3 combat (Drink health when hurt, Attack,
    // Wait) and 4 logistics (drink, loot arrows/potions/gear), so both fit
    // Rank I. Logistics starts LARGER and grows SLOWER than combat (marth): a
    // steady utility track, not a combat-power curve.
    inline constexpr std::uint8_t kCombatSlotsByRank[kMaxRank + 1]    = { 0, 3, 5, 7, 9, 12 };
    inline constexpr std::uint8_t kLogisticsSlotsByRank[kMaxRank + 1] = { 0, 4, 5, 6, 7, 8 };

    inline std::uint8_t SlotsForRank(std::uint8_t a_rank, Table a_table) {
        // Debug: every slot open regardless of rank -- the Rank V allowance.
        const auto r = Config::g_debugUnlockSlots.load()
                           ? kMaxRank
                           : std::clamp<std::uint8_t>(a_rank, 1, kMaxRank);
        return a_table == Table::Combat ? kCombatSlotsByRank[r] : kLogisticsSlotsByRank[r];
    }

    // Keyed on the actor's PERSISTENT FormID. Dismissed followers keep their
    // board and Rapport (DESIGN.md 3.1), so membership here is independent of
    // current teammate status.
    inline std::unordered_map<RE::FormID, FollowerState> g_followers;

    void ResetAllState();   // RevertCallback

}
