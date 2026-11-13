#pragma once
// iaus/PotionBuff.h -- THE BUFF-POTION KINDS AND THE RULES THAT NEED NO ENGINE (ClickUp 1dfcuabd18f, branch feat/buff-potions).
//
// marth: "use mfo_items.json to add all valid potions to the weighting". Today IAUS saw three potion counts (health / stamina /
// magicka restores). This header names every other BENEFICIAL effect kind a potion can carry, the plain data the snapshot keeps for
// each (counts, best magnitude and duration), and the pure rules Generate.cpp's buff-potion block uses.
//
// PURE: standard headers only, no RE:: type (the same rule as iaus/Core.h), so tools/iaus_harness and the sim build it with the host
// compiler. An actor value is a plain int whose numbers ARE RE::ActorValue's (and Mutagen's ActorValue enum's: both match, checked on
// 2026-11-12 against the fork's RE/A/ActorValues.h); the engine side (logistics/PotionFx.cpp) fills them, nothing here reads one.
//
// THE KIND is read from the effect's MGEF archetype and actor value (never a name): the patcher (installer/MFO.Synthesis/Catalog.cs
// ClassifyFx) and the runtime fallback (logistics/PotionFx.cpp ClassifyRuntime) apply ONE rule, written twice and kept in step:
//   a potion with a Hostile or Detrimental effect (a drug), a poison, or a food = no kinds at all;
//   archetype CurePoison / CureDisease / Invisibility = that kind;
//   archetype ValueModifier / DualValueModifier / PeakValueModifier on actor value:
//     ResistFire / Frost / Shock / PoisonResist / ResistMagic       -> resist kinds
//     Health / Stamina / Magicka WITH the Recover flag or the Peak archetype -> fortify (a plain restore is NOT a kind: the restore
//                                                                      potions keep their own fields and families)
//     HealRate / StaminaRate / MagickaRate (and the Mult actor values) -> regen kinds
//     the 18 skills: base actor values 6..23, the PowerModifier values 135..152 (-129) and the *Modifier values 96..113 (-90)
//                                                                      -> FortifySkill (av = the BASE skill number 6..23)
//     AttackDamageMult / MeleeDamage / UnarmedDamage                   -> FortifyAttack
//     WaterBreathing                                                   -> WaterBreathing
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace MFO::Iaus {

    // APPEND ONLY: the value indexes Kit::buff and is in log lines. The catalog stores the NAME (BuffName), never the number.
    enum class BuffKind : std::uint8_t {
        ResistFire = 0, ResistFrost, ResistShock, ResistPoison, ResistMagic,
        FortifyHealth, FortifyStamina, FortifyMagicka, FortifySkill, FortifyAttack,
        RegenHealth, RegenStamina, RegenMagicka,
        Invisibility, WaterBreathing,
        CurePoison, CureDisease,   // catalogued and counted; no IAUS family reads them yet (nothing in the snapshot says he is poisoned or diseased)
        kCount
    };
    inline constexpr std::size_t kBuffKinds = static_cast<std::size_t>(BuffKind::kCount);

    [[nodiscard]] inline std::string_view BuffName(BuffKind a_k) {
        constexpr std::string_view n[] = {
            "resist_fire", "resist_frost", "resist_shock", "resist_poison", "resist_magic",
            "fortify_health", "fortify_stamina", "fortify_magicka", "fortify_skill", "fortify_attack",
            "regen_health", "regen_stamina", "regen_magicka",
            "invisibility", "waterbreathing",
            "cure_poison", "cure_disease",
        };
        static_assert(std::size(n) == kBuffKinds, "BuffName out of step with BuffKind");
        const auto i = static_cast<std::size_t>(a_k);
        return i < kBuffKinds ? n[i] : "?";
    }
    // The catalog's kind string -> the kind. false = not a kind this DLL knows (a newer patcher): the row's effect is skipped.
    [[nodiscard]] inline bool BuffFromName(std::string_view a_name, BuffKind& a_out) {
        for (std::size_t i = 0; i < kBuffKinds; ++i)
            if (BuffName(static_cast<BuffKind>(i)) == a_name) { a_out = static_cast<BuffKind>(i); return true; }
        return false;
    }

    // One beneficial effect of one potion (the catalog row, or the runtime fallback's read of the record).
    struct PotionFx {
        BuffKind kind = BuffKind::kCount;
        float    magnitude = 0.0f;
        float    duration = 0.0f;   // seconds
        int      av = -1;           // FortifySkill: the skill's actor value; else -1
    };

    // The element a RESIST kind answers, as the resist actor value an incoming spell's effect names (Prune.h kAvResistFire ...).
    inline constexpr int kAvPoisonResistBuff = 40;
    [[nodiscard]] inline int ResistAvOf(BuffKind a_k) {
        switch (a_k) {
        case BuffKind::ResistFire:   return 41;
        case BuffKind::ResistShock:  return 42;
        case BuffKind::ResistFrost:  return 43;
        case BuffKind::ResistMagic:  return 44;
        case BuffKind::ResistPoison: return kAvPoisonResistBuff;
        default: return -1;
        }
    }
    [[nodiscard]] inline bool IsResist(BuffKind a_k) { return ResistAvOf(a_k) >= 0; }
    [[nodiscard]] inline bool IsFortify(BuffKind a_k) {
        return a_k == BuffKind::FortifyHealth || a_k == BuffKind::FortifyStamina || a_k == BuffKind::FortifyMagicka ||
               a_k == BuffKind::FortifySkill || a_k == BuffKind::FortifyAttack;
    }
    [[nodiscard]] inline bool IsRegen(BuffKind a_k) {
        return a_k == BuffKind::RegenHealth || a_k == BuffKind::RegenStamina || a_k == BuffKind::RegenMagicka;
    }

    // The BASE skill number 6..23 of a skill actor value as a potion's effect names it: the base value itself, the PowerModifier value (135..152, base + 129,
    // what vanilla fortify-skill potions use) or the *Modifier value (96..113, base + 90). -1 = not a skill. The patcher (Catalog.cs) and KindOf write the same rule.
    [[nodiscard]] inline int BaseSkillAv(int a_av) {
        int n = a_av;
        if (n >= 135 && n <= 152) n -= 129; else if (n >= 96 && n <= 113) n -= 90;
        return (n >= 6 && n <= 23) ? n : -1;
    }

    // What a FortifySkill potion's skill is worth to the follower in a fight (the skill actor values 6..23).
    enum class SkillGroup : std::uint8_t { Other = 0, Melee, Ranged, Magic, Stealth };
    [[nodiscard]] inline SkillGroup SkillGroupOf(int a_av) {
        switch (a_av) {
        case 6: case 7: case 9: case 11: case 12: return SkillGroup::Melee;   // one-handed, two-handed, block, heavy and light armor
        case 8:                                   return SkillGroup::Ranged;  // archery
        case 18: case 19: case 20: case 21: case 22: return SkillGroup::Magic;   // alteration, conjuration, destruction, illusion, restoration
        case 13: case 14: case 15:                return SkillGroup::Stealth; // pickpocket, lockpicking, sneak
        default:                                  return SkillGroup::Other;   // smithing, alchemy, speech, enchanting: nothing a fight uses
        }
    }
    // The magic skill's School identity (iaus/Core.h: Destruction 0, Alteration 1, Restoration 2, Illusion 3, Conjuration 4); -1 = not a school.
    [[nodiscard]] inline int SkillSchoolOf(int a_av) {
        switch (a_av) {
        case 20: return 0;
        case 18: return 1;
        case 22: return 2;
        case 21: return 3;
        case 19: return 4;
        default: return -1;
        }
    }

    // Per kind, what the follower carries (Kit::buff). count = potions carried that hold the kind; form / magnitude / duration / av =
    // the BEST such potion (largest magnitude, then longest); active = ANY live potion effect of the kind is on him now (read on the main
    // thread from his active effect list, whatever potion it came from), so a second potion of a live kind is not offered.
    struct PotionBuff {
        std::uint32_t form = 0;
        std::int32_t  count = 0;
        float         magnitude = 0.0f;
        float         duration = 0.0f;
        int           av = -1;
        bool          active = false;
        float         value = 0.0f;           // feat/consumables: the BEST potion's gold value (the worth-the-gold rule, iaus/ConsumableWorth.h)
        std::uint32_t kindMask = 0;           // ... and every BuffKind that potion holds (bit = the kind's number): a mixed item counts each effect
        std::uint8_t  restoreMask = 0;        // ... and every restore family it also restores (bit 0 health, 1 stamina, 2 magicka)
        std::string   name;
    };
    using BuffStock = std::array<PotionBuff, kBuffKinds>;

}
