#pragma once
#include "PCH.h"

// The gambit vocabulary. DESIGN.md §3.
//
// HAND-WRITTEN for now. ARCHITECTURE.md §1 plans this as codegen from a design
// JSON (gen_vocab_header.py) -- correct once there are dozens of entries, but a
// six-entry table does not need a code generator. The opcode STRINGS are a
// frozen serialization contract (INVARIANTS #10): they land in co-saves, so
// their spelling may never change once shipped. Codegen, when it lands, must
// emit exactly these strings.
//
// M5 FIRST SLICE: cheap conditions only (one actor-value read on a known
// actor -- no world snapshot, so no cost-tier machinery yet), and cast/wait
// actions. No foe/ally-scan conditions, no standing orders, no drink/equip.

namespace MFO::Vocab {

    // WHAT COUNTS AS A REAL, PICKABLE SPELL. marth, 2026-07-22: the picker was
    // listing "non-spell spells" -- a follower's spell list (VisitSpells) is
    // full of PASSIVE entries typed as spells: racial/perk constant-effect
    // abilities, granted always-on effects. They are kSpell by type but a
    // follower can never volitionally cast them. The distinguishing mark is the
    // casting type: kConstantEffect is applied-and-held, never a deliberate
    // cast. A castable spell fires-and-forgets or is concentrated. This is the
    // one gate for both the board picker and the seed's auto-pick, so they can
    // never disagree about what a spell is.
    [[nodiscard]] inline bool IsCastableSpell(const RE::SpellItem* a_sp) {
        if (!a_sp) return false;
        if (a_sp->GetSpellType() != RE::MagicSystem::SpellType::kSpell) return false;
        if (a_sp->GetCastingType() == RE::MagicSystem::CastingType::kConstantEffect) return false;
        return true;
    }


    // Who a condition reads / an action targets.
    enum class Subject : std::uint8_t {
        Self   = 0,
        Player = 1,
        // Ally/Foe selectors arrive with the world snapshot in a later slice.
    };

    // ── conditions (stable opcode strings) ──────────────────────────────────
    inline constexpr const char* kCondAlways        = "cond.always";
    inline constexpr const char* kCondSelfHpBelow   = "cond.self_hp_pct_below";
    inline constexpr const char* kCondSelfMpBelow   = "cond.self_mp_pct_below";
    inline constexpr const char* kCondSelfSpBelow   = "cond.self_sp_pct_below";
    inline constexpr const char* kCondPlayerHpBelow = "cond.player_hp_pct_below";

    // FOE SELECTORS. These are conditions that also CHOOSE A TARGET -- the FFXII
    // shape, where "Foe: lowest HP" is one clause, not two. Candidates come from
    // the follower's own combat group, never a world sweep: the engine already
    // knows who is in this fight, and a swept list could name someone the
    // follower is not engaged with.
    inline constexpr const char* kCondFoeAny      = "cond.foe_any";            // nearest
    inline constexpr const char* kCondFoeHpBelow  = "cond.foe_hp_pct_below";   // lowest under X%
    inline constexpr const char* kCondFoeLowestHp = "cond.foe_lowest_hp";      // lowest, any %

    // ── COMBAT VOCABULARY EXPANSION (GAMBIT_LIBRARY.md, marth-approved) ──────
    // Frozen serialization strings (#10), same contract as everything above.
    // Self-value ABOVE gates -- the mirror of the _below trio; param is pct [0,1].
    inline constexpr const char* kCondSelfHpAbove = "cond.self_hp_pct_above";
    inline constexpr const char* kCondSelfMpAbove = "cond.self_mp_pct_above";
    inline constexpr const char* kCondSelfSpAbove = "cond.self_sp_pct_above";
    // World gates -- no param.
    inline constexpr const char* kCondIsInterior  = "cond.is_interior";
    inline constexpr const char* kCondIsNight     = "cond.is_night";
    // Foe-COUNT gate -- true when at least `param` live foes are in the group.
    // Reads the combat group but chooses NO target (not a selector).
    inline constexpr const char* kCondFoeCountAtLeast = "cond.foe_count_at_least";
    // Foe SELECTORS -- choose a target AND gate, resolved in PickFoe.
    inline constexpr const char* kCondFoeHighestHp       = "cond.foe_highest_hp";
    inline constexpr const char* kCondFoeWithinRange     = "cond.foe_within_range";   // param = units
    inline constexpr const char* kCondFoeBeyondRange     = "cond.foe_beyond_range";   // param = units
    inline constexpr const char* kCondFoeAttackingPlayer = "cond.foe_attacking_player";
    inline constexpr const char* kCondFoeAttackingMe     = "cond.foe_attacking_me";
    inline constexpr const char* kCondFoeIsUndead        = "cond.foe_is_undead";
    inline constexpr const char* kCondFoeIsDragon        = "cond.foe_is_dragon";
    // ELEMENTAL WEAKNESS selectors -- choose the nearest foe whose resistance to
    // this element is NEGATIVE (an active weakness: race trait, ability, or a
    // -resist effect). Read from the actor's own resist actor-value, never a
    // name (§4.8.2). "Foe: weak to fire -> Cast Flames" is the FFXII play.
    inline constexpr const char* kCondFoeWeakFire        = "cond.foe_weak_fire";
    inline constexpr const char* kCondFoeWeakFrost       = "cond.foe_weak_frost";
    inline constexpr const char* kCondFoeWeakShock       = "cond.foe_weak_shock";
    // ALLY SELECTOR -- the lowest-HP teammate under `param` pct; chooses that
    // ally as the target (for Cast at ally / Heal Other). Walks the maintained
    // teammate list, not a world sweep.
    inline constexpr const char* kCondAllyHpBelow = "cond.ally_hp_pct_below";         // param = pct

    // ── actions ─────────────────────────────────────────────────────────────
    inline constexpr const char* kActWait      = "act.wait";
    inline constexpr const char* kActCastSelf  = "act.cast_self";    // param = SpellItem
    inline constexpr const char* kActCastTarget= "act.cast_target";  // param = SpellItem, subject = target
    inline constexpr const char* kActAttack    = "act.attack";       // target = the chosen foe
    // Tier-A equip actions: equip best-in-category from the follower's OWN
    // inventory (ActorEquipManager, §4.5 Tier A). Idempotent -- no-op if already
    // holding that category.
    inline constexpr const char* kActEquipRanged = "act.equip_ranged";
    inline constexpr const char* kActEquipMelee  = "act.equip_melee";
    inline constexpr const char* kActEquipTorch  = "act.equip_torch";

    // ── LOGISTICS TABLE (DESIGN §4.8) ───────────────────────────────────────
    // A SEPARATE non-combat table (State::logistics() == tables[1]). It runs on
    // the out-of-combat idle tick and NEVER interleaves with the combat table
    // (§4.8). These opcode strings are the same frozen serialization contract
    // (#10) as the combat ones -- once shipped in a co-save they are permanent.
    //
    // NAMING: cond.self_* mirrors the existing self-vitals conditions; the
    // supply conditions read the follower's OWN inventory/ammo (INVARIANTS #14
    // -- the subject is always the named follower, never the player). act.drink_*
    // and act.loot_* mirror the act.cast_* shape.

    // Supply conditions. conditionParam is a COUNT (a whole number carried in the
    // float), not a percentage -- "fewer than N of this potion", "fewer than N
    // arrows". Cheap in isolation but each walks the follower's inventory, so
    // they are ONLY legible in the logistics table, which ticks at ~1 s out of
    // combat (§4.8.1) where an inventory walk is affordable -- unlike the 133 ms
    // combat tick, whose #23 no-allocation rule these would violate.
    inline constexpr const char* kCondSelfLowHealthPotion  = "cond.self_low_health_potion";
    inline constexpr const char* kCondSelfLowStaminaPotion = "cond.self_low_stamina_potion";
    inline constexpr const char* kCondSelfLowMagickaPotion = "cond.self_low_magicka_potion";
    inline constexpr const char* kCondSelfOutOfArrows      = "cond.self_out_of_arrows";  // param = count of ARROWS carried
    inline constexpr const char* kCondSelfOutOfBolts       = "cond.self_out_of_bolts";   // param = count of BOLTS carried

    // The "hurt / low-magicka / low-stamina out of combat" drink triggers reuse
    // the existing self-vitals conditions (kCondSelfHpBelow / kCondSelfMpBelow /
    // kCondSelfSpBelow) rather than duplicating them -- a percentage read of the
    // named follower is identical whether the rule sits in the combat or the
    // logistics table, and #28 says MFO does not second-guess which table the
    // player put a rule in.

    // Logistics actions.
    //   drink_*  -- consume the BEST (highest-magnitude) restore potion of that
    //               resource the follower already carries, via the AlchemyItem
    //               equip path (DESIGN §4.5 Tier A). Health/Stamina/Magicka only
    //               (§4.8.2), classified by MGEF archetype, never by name.
    //   loot_*   -- take from a nearby corpse/container, gated by the first-dibs
    //               delay + player-looted waiver (§4.8.3, INVARIANTS #22e/#22g/
    //               #22h). Arrows and the three potions are self-contained;
    //               loot_equipment is generalized by category, never by item.
    inline constexpr const char* kActDrinkHealthPotion  = "act.drink_health_potion";
    inline constexpr const char* kActDrinkStaminaPotion = "act.drink_stamina_potion";
    inline constexpr const char* kActDrinkMagickaPotion = "act.drink_magicka_potion";
    inline constexpr const char* kActLootArrows         = "act.loot_arrows";
    inline constexpr const char* kActLootBolts          = "act.loot_bolts";
    // Potion looting comes in FOUR flavours. loot_potions is the catch-all --
    // grab ANY drinkable (health/stamina/magicka/fortify/resist/cure), the
    // "top up on everything" action. The three per-resource actions loot ONLY
    // that restorative (classified by PotionRestores' MGEF archetype, never by
    // name), so a player who wants a healer to hoard only healing potions can
    // say so EXPLICITLY -- the type is the action, not inferred from the paired
    // condition (marth: the catch-all never replaced per-type selection).
    inline constexpr const char* kActLootPotions        = "act.loot_potions";
    inline constexpr const char* kActLootHealthPotion   = "act.loot_health_potion";
    inline constexpr const char* kActLootStaminaPotion  = "act.loot_stamina_potion";
    inline constexpr const char* kActLootMagickaPotion  = "act.loot_magicka_potion";
    inline constexpr const char* kActLootEquipment      = "act.loot_equipment";
    inline constexpr const char* kActLootGold           = "act.loot_gold";

    // A cheap actor-value-percentage read. Reads THE NAMED actor's state
    // (INVARIANTS #15 -- say whose).
    //
    // Returns 1.0 (full) on any failure, so an unreadable actor never triggers
    // a "below X%" condition. Failing toward NOT acting is the safe default:
    // MFO doing nothing is always compatible, MFO acting on garbage is not.
    //
    // THE MAXIMUM IS NOT GetPermanentActorValue. That call is base + permanent
    // and OMITS the temporary modifier -- which is exactly where fortify-health
    // from gear and potions lands. A follower at 100 permanent wearing +50
    // fortify would read 150/100, clamp to 1.0 while untouched, and still read
    // "full" after losing 50 HP; a "HP < 40%" rule would not fire until 40/150
    // = 27% of true maximum. In a heavily-modded order every follower wears
    // that gear, so this would have shipped as "the heal always comes late".
    inline float Pct(RE::Actor* a_actor, RE::ActorValue a_av) {
        if (!a_actor) return 1.0f;
        auto* avo = a_actor->AsActorValueOwner();
        if (!avo) return 1.0f;
        const float mx = avo->GetPermanentActorValue(a_av) +
                         a_actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, a_av);
        if (mx <= 0.0f) return 1.0f;
        return std::clamp(avo->GetActorValue(a_av) / mx, 0.0f, 1.0f);
    }

    inline float HealthPct(RE::Actor* a)  { return Pct(a, RE::ActorValue::kHealth); }
    inline float MagickaPct(RE::Actor* a) { return Pct(a, RE::ActorValue::kMagicka); }
    inline float StaminaPct(RE::Actor* a) { return Pct(a, RE::ActorValue::kStamina); }

}
