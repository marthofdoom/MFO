#pragma once
// iaus/LootNeed.h -- IAUS LOOTING: WHAT a follower takes from what LootScan found, scored by his CURRENT INVENTORY (ClickUp 1dfcuabd18f,
// branch feat/iaus-loot).
//
// marth 2026-11-12: "still find loot the same way we currently do, but then iaus what gets looted by current inventory" / "That way they
// would loot potions they may want to use in battle later". marth 2026-11-13: "Potions shoudl also read all effects, in fact food and drink
// as well". Finding stays LootScan's; with Sentient AI on (and bIausLoot), the act.loot_* gambit rows are skipped and these scores decide
// which loot CATEGORIES he goes for and, for consumables (potions AND food / drink), WHICH items and how many.
//
// PURE: standard headers + iaus/PotionBuff.h only, no RE:: type (the iaus/Core.h rule), so tools/iaus_harness builds it with the host
// compiler. The engine side (logistics/LootNeed.cpp) fills LootInv / Consumable from the follower and the source ref.
//
// EVERY NUMBER IN LootTuning IS A LABELLED PLACEHOLDER: the logistics world of tools/iaus_sim searches them later (sim is Fable-only;
// nothing here is sim-derived yet).
#include "iaus/PotionBuff.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace MFO::Iaus {

    // The loot categories IAUS looting decides (logistics Category minus museum / smith mats / steal / pick locks, which keep their own
    // roads). Order = the log's names, nothing else.
    enum class LootKind : std::uint8_t { Arrows = 0, Bolts, Potions, Equipment, Gold, Jewelry, SoulGems, Lockpicks, Ingredients, Valuables, kCount };
    inline constexpr std::size_t kLootKinds = static_cast<std::size_t>(LootKind::kCount);

    [[nodiscard]] inline const char* LootKindName(LootKind a_k) {
        constexpr const char* n[] = { "arrows", "bolts", "potions", "equipment", "gold", "jewelry", "soulgems", "lockpicks", "ingredients", "valuables" };
        static_assert(std::size(n) == kLootKinds, "LootKindName out of step with LootKind");
        const auto i = static_cast<std::size_t>(a_k);
        return i < kLootKinds ? n[i] : "?";
    }

    // Restore families of a consumable, index into LootInv::restore.
    inline constexpr int kRestoreHealth = 0, kRestoreStamina = 1, kRestoreMagicka = 2, kRestoreFamilies = 3;

    // What he carries and is, read on the worker right before a loot decision.
    struct LootInv {
        // Ammo NEED (marth 2026-11-12): his class uses ranged of the kind AND he carries a launcher of that kind (bow -> arrows,
        // crossbow -> bolts). false = that ammo is valued as sale gold only.
        bool  usesArrows = false, usesBolts = false;
        int   arrows = 0, bolts = 0;
        int   arrowTarget = 0, boltTarget = 0;   // SwapUp's AmmoKeepTarget for the kind he uses (0 when he does not)
        int   lockpicks = 0;
        float carried = 0.0f, capacity = 0.0f;   // weight in his inventory, his CarryWeight
        // Role flags: the same split IAUS drink rules read (Generate.cpp buff block: weapon / casts / skill groups).
        bool  melee = false, ranged = false, caster = false, sneaks = false;
        std::array<int, kRestoreFamilies> restore{};   // restore potions carried per family (CountPotions)
        std::array<int, kBuffKinds>       buff{};      // consumables carried that hold each buff kind
        int   poisons = 0;                             // poisons carried (ALCH with the poison flag)
        // SOUL GEMS' USE (marth 2026-11-13: "cap em, but better is a weapon charge check a weapon with a charge below 50% triggers the
        // looting of gems"): the LOWEST charge, as a fraction of its maximum, over every copy of every enchanted weapon he carries (runtime
        // read of each copy's own charge, never a mod or list name; 1 = none carried / all full), and the soul gems he carries (filled or
        // empty). A gem has a use only while weaponChargeFrac < LootTuning::gemChargeTrigger, up to LootTuning::soulGemTarget gems.
        float weaponChargeFrac = 1.0f;
        int   soulGems = 0;
    };

    // One consumable (an ALCH: potion, food or drink) as the scorer sees it: EVERY beneficial effect read (catalog row first, the
    // runtime fallback otherwise), the plain restores separately.
    struct Consumable {
        bool  food = false;
        bool  hostile = false;            // a poison or a drug (any Hostile / Detrimental effect): no drink need
        bool  poison = false;             // an ALCH with the poison flag: carry value through PoisonCarryValue (weapon users)
        float poisonPower = 0.0f;         // poison: the sum over ALL its hostile effects of magnitude x duration (duration 0 counts as 1 s)
        std::array<float, kRestoreFamilies> restoreMag{};   // per family, the restore magnitude (0 = none)
        std::vector<PotionFx> fx;         // every buff kind it carries
        // feat/consumables (the USE side, never read by the loot scorer): a FOOD or drink's restore per family (health / stamina / magicka;
        // the same archetype split as restoreMag, but a food is not excluded). 0 = none. Empty for a potion (its restores are restoreMag).
        std::array<float, kRestoreFamilies> foodRestoreMag{};
        std::array<float, kRestoreFamilies> foodRestoreDur{};   // ...and the duration (seconds) of the effect that set that magnitude (0 = instant)
        float value = 0.0f, weight = 0.0f;
        float maxDuration = 0.0f;         // feat/consumables (the USE side): the longest duration over EVERY effect it carries, restores included; 0 = none
    };

    struct LootTuning {
        // PLACEHOLDER (sim-search later). A score at or above this is a TAKE.
        float takeThreshold = 0.5f;
        // PLACEHOLDER. need(short, target) = needBase + needSlope * short / target (clamped to 1) while short > 0, else 0.
        float needBase = 0.55f, needSlope = 0.45f;
        // PLACEHOLDER. Carry room: full credit at roomKnee of his capacity free, linear to 0 at none free.
        float roomKnee = 0.25f;
        // PLACEHOLDER. Sale value: gold per unit weight that scores 1 (before saleWeight and the room factor).
        float saleGoldPerWeight = 50.0f, saleWeight = 0.7f;
        // PLACEHOLDER. Category bases (times the room factor): gold is weightless coin.
        float gold = 1.0f, equipment = 0.8f, valuables = 0.75f, jewelry = 0.75f;
        // marth 2026-11-13: a soul gem is wanted only while a carried enchanted weapon is below gemChargeTrigger of its max charge (a fraction),
        // and only up to soulGemTarget gems carried (PLACEHOLDER 3, sim-search later). NeedScore does the rest.
        float gemChargeTrigger = 0.5f;
        int   soulGemTarget = 3;
        // A SALE score counts only where MFO has a sell road for the form type (review F2): Economy.cpp sells WEAP, ARMO (jewellery
        // included) and valuable MISC only. No ALCH / INGR / AMMO road exists today, so a consumable's sale value is 0 until one does;
        // flip this when an economy road sells ALCH. Never a sale floor that clears takeThreshold on its own (review F3).
        bool  consumableSellRoad = false;
        // PLACEHOLDER. Carry targets for the consumables IAUS drinks.
        int   healthTarget = 6, staminaTarget = 4, staminaTargetNoWeapon = 2, magickaTarget = 5;
        int   resistTarget = 2, fortifyTarget = 1, regenTarget = 1, invisTarget = 1, lockpickTarget = 10;
        // PLACEHOLDER. A buff-kind need weighs this much against a restore need; food against a potion (IAUS does not eat yet).
        float buffScale = 0.95f, foodScale = 0.9f;
        // PLACEHOLDER. Poisons (weapon users): carry this many; a poison of poisonPowerRef (magnitude x duration summed) or more scores the
        // full need, a weaker one down to poisonWeakScale of it.
        int   poisonTarget = 3;
        float poisonPowerRef = 100.0f, poisonWeakScale = 0.8f;
    };
    [[nodiscard]] inline const LootTuning& DefaultLootTuning() { static const LootTuning t; return t; }

    struct LootVerdict {
        float       score = 0.0f;
        bool        take = false;
        int         takeCount = 0;   // consumables only: how many of the stack (need shortfall, or all for a sale take)
        std::string why;
    };

    [[nodiscard]] inline float LootClamp01(float a_v) { return std::clamp(a_v, 0.0f, 1.0f); }

    // 1 with at least roomKnee of his capacity free, linearly down to 0 when he is full. capacity <= 0 = unknown -> 1.
    [[nodiscard]] inline float CarryRoom(const LootInv& a_inv, const LootTuning& a_t) {
        if (a_inv.capacity <= 0.0f) return 1.0f;
        const float freeFrac = (a_inv.capacity - a_inv.carried) / a_inv.capacity;
        return a_t.roomKnee > 0.0f ? LootClamp01(freeFrac / a_t.roomKnee) : (freeFrac > 0.0f ? 1.0f : 0.0f);
    }

    [[nodiscard]] inline float NeedScore(int a_have, int a_target, const LootTuning& a_t) {
        if (a_target <= 0 || a_have >= a_target) return 0.0f;
        const float shortFrac = static_cast<float>(a_target - a_have) / static_cast<float>(a_target);
        return LootClamp01(a_t.needBase + a_t.needSlope * shortFrac);
    }

    // Sale gold vs carry weight, times the room factor. A weightless item scores the full saleWeight while he has room.
    [[nodiscard]] inline float SaleScore(float a_value, float a_weight, float a_room, const LootTuning& a_t) {
        if (a_value <= 0.0f) return 0.0f;
        const float gpw = a_weight > 0.0f ? a_value / a_weight : a_t.saleGoldPerWeight;
        return LootClamp01(gpw / a_t.saleGoldPerWeight) * a_t.saleWeight * a_room;
    }

    [[nodiscard]] inline int RestoreTarget(int a_family, const LootInv& a_inv, const LootTuning& a_t) {
        switch (a_family) {
        case kRestoreHealth:  return a_t.healthTarget;
        case kRestoreStamina: return (a_inv.melee || a_inv.ranged) ? a_t.staminaTarget : a_t.staminaTargetNoWeapon;
        case kRestoreMagicka: return a_inv.caster ? a_t.magickaTarget : 0;
        default:              return 0;
        }
    }

    // How many of a buff kind he wants carried: the kinds IAUS drinks (Generate.cpp buff block), mirrored by role. Water breathing and the
    // cures are catalogued only (nothing drinks them yet): target 0, so they are worth their sale gold only. a_skillAv: FortifySkill's skill.
    [[nodiscard]] inline int BuffTarget(BuffKind a_k, int a_skillAv, const LootInv& a_inv, const LootTuning& a_t) {
        const bool weapon = a_inv.melee || a_inv.ranged;
        if (IsResist(a_k)) return a_t.resistTarget;
        switch (a_k) {
        case BuffKind::FortifyHealth:  return a_t.fortifyTarget;
        case BuffKind::FortifyStamina: return weapon ? a_t.fortifyTarget : 0;
        case BuffKind::FortifyMagicka: return a_inv.caster ? a_t.fortifyTarget : 0;
        case BuffKind::FortifyAttack:  return weapon ? a_t.fortifyTarget : 0;
        case BuffKind::FortifySkill:
            switch (SkillGroupOf(a_skillAv)) {
            case SkillGroup::Melee:   return a_inv.melee ? a_t.fortifyTarget : 0;
            case SkillGroup::Ranged:  return a_inv.ranged ? a_t.fortifyTarget : 0;
            case SkillGroup::Magic:   return a_inv.caster ? a_t.fortifyTarget : 0;
            case SkillGroup::Stealth: return a_inv.sneaks ? a_t.fortifyTarget : 0;
            case SkillGroup::Other:   return 0;
            }
            return 0;
        case BuffKind::RegenHealth:  return a_t.regenTarget;
        case BuffKind::RegenStamina: return weapon ? a_t.regenTarget : 0;
        case BuffKind::RegenMagicka: return a_inv.caster ? a_t.regenTarget : 0;
        case BuffKind::Invisibility: return a_inv.sneaks ? a_t.invisTarget : 0;
        default:                     return 0;
        }
    }

    // One loot CATEGORY: is it worth a trip / a strip right now? Consumables (Potions) score their best family need here; the item filter
    // (ScoreConsumable) then picks the items. Equipment / valuables / jewellery / soul gems / ingredients keep their own per-item rules
    // (LootEquipment's keep logic, ValuableRatioClears ...); this only weighs the trip against his free carry weight.
    [[nodiscard]] inline LootVerdict ScoreLootKind(LootKind a_k, const LootInv& a_inv, const LootTuning& a_t = DefaultLootTuning()) {
        LootVerdict v;
        const float room = CarryRoom(a_inv, a_t);
        char buf[160];
        switch (a_k) {
        case LootKind::Arrows:
        case LootKind::Bolts: {
            const bool bolt = a_k == LootKind::Bolts;
            const bool uses = bolt ? a_inv.usesBolts : a_inv.usesArrows;
            const int  have = bolt ? a_inv.bolts : a_inv.arrows;
            const int  tgt  = bolt ? a_inv.boltTarget : a_inv.arrowTarget;
            if (uses) {
                v.score = NeedScore(have, tgt, a_t);
                std::snprintf(buf, sizeof buf, "need %d/%d (uses the kind + carries its launcher)", have, tgt);
            } else {
                v.score = 0.0f;   // no sell road for AMMO (review F2): unused ammo is left
                std::snprintf(buf, sizeof buf, "unused (no ranged use of the kind or no launcher), no sell road for ammo");
            }
            break;
        }
        case LootKind::Potions: {
            float best = 0.0f; int bestFam = -1;
            for (int f = 0; f < kRestoreFamilies; ++f) {
                const float s = NeedScore(a_inv.restore[static_cast<std::size_t>(f)], RestoreTarget(f, a_inv, a_t), a_t);
                if (s > best) { best = s; bestFam = f; }
            }
            int bestBuff = -1;
            for (std::size_t i = 0; i < kBuffKinds; ++i) {
                const auto k = static_cast<BuffKind>(i);
                if (k == BuffKind::FortifySkill) continue;   // per-skill: judged per item only
                const float s = NeedScore(a_inv.buff[i], BuffTarget(k, -1, a_inv, a_t), a_t) * a_t.buffScale;
                if (s > best) { best = s; bestFam = -1; bestBuff = static_cast<int>(i); }
            }
            // NEED only: no ALCH sell road exists (review F2), so a stocked follower leaves potions, food and drink alone.
            v.score = best;
            constexpr const char* fam[] = { "health", "stamina", "magicka" };
            if (bestFam >= 0)
                std::snprintf(buf, sizeof buf, "need restore %s %d/%d", fam[bestFam], a_inv.restore[static_cast<std::size_t>(bestFam)],
                              RestoreTarget(bestFam, a_inv, a_t));
            else if (bestBuff >= 0)
                std::snprintf(buf, sizeof buf, "need buff %s %d", std::string(BuffName(static_cast<BuffKind>(bestBuff))).c_str(),
                              a_inv.buff[static_cast<std::size_t>(bestBuff)]);
            else
                std::snprintf(buf, sizeof buf, "stocked, no sell road for consumables");
            break;
        }
        case LootKind::Lockpicks:
            v.score = NeedScore(a_inv.lockpicks, a_t.lockpickTarget, a_t);
            std::snprintf(buf, sizeof buf, "need %d/%d", a_inv.lockpicks, a_t.lockpickTarget);
            break;
        case LootKind::Gold:        v.score = a_t.gold;              std::snprintf(buf, sizeof buf, "coin (weightless)"); break;
        case LootKind::Equipment:   v.score = a_t.equipment * room;  std::snprintf(buf, sizeof buf, "keep-logic upgrades, room=%.2f", static_cast<double>(room)); break;
        case LootKind::Valuables:   v.score = a_t.valuables * room;  std::snprintf(buf, sizeof buf, "sale gold vs weight, room=%.2f", static_cast<double>(room)); break;
        case LootKind::Jewelry:     v.score = a_t.jewelry * room;    std::snprintf(buf, sizeof buf, "sale gold vs weight, room=%.2f", static_cast<double>(room)); break;
        case LootKind::SoulGems:
            // marth 2026-11-13: a gem is for charging a weapon. NEED (up to soulGemTarget carried) only while he carries an enchanted weapon
            // below gemChargeTrigger of its max charge; otherwise 0 (Economy's valuable-MISC road excludes soul gems: IsValuableMisc, so
            // there is no sale value to keep).
            if (a_inv.weaponChargeFrac < a_t.gemChargeTrigger) {
                v.score = NeedScore(a_inv.soulGems, a_t.soulGemTarget, a_t);
                std::snprintf(buf, sizeof buf, "use: a weapon below %.0f%% charge, gems %d/%d", static_cast<double>(a_t.gemChargeTrigger) * 100.0, a_inv.soulGems, a_t.soulGemTarget);
            } else {
                v.score = 0.0f;
                std::snprintf(buf, sizeof buf, "no weapon below %.0f%% charge, no sell road for gems", static_cast<double>(a_t.gemChargeTrigger) * 100.0);
            }
            break;
        case LootKind::Ingredients: v.score = 0.0f; std::snprintf(buf, sizeof buf, "no use, no sell road for ingredients"); break;
        case LootKind::kCount:      buf[0] = '\0'; break;
        }
        v.take = v.score >= a_t.takeThreshold;
        v.why = buf;
        return v;
    }

    // POISON CARRY VALUE (marth 2026-11-13: "A stub for poisons, followers may be able to apply them in certain lists.", then: "All our
    // current lists allow it. if not via the lust itself(im sure it does) via MAOs perk"). A follower who fights with a weapon (melee or
    // ranged) wants a few poisons carried; the score reads ALL the poison's hostile effects (Consumable::poisonPower = magnitude x duration
    // summed) and is capped by poisonTarget. No list or mod is named or detected here, and no apply logic lives here (filed separately).
    // A non-weapon follower, or a drug (hostile effects without the poison flag): 0, sale gold only. a_wantCount = how many he still wants.
    [[nodiscard]] inline float PoisonCarryValue(const Consumable& a_c, const LootInv& a_inv, const LootTuning& a_t, int& a_wantCount) {
        a_wantCount = 0;
        if (!a_c.poison || !(a_inv.melee || a_inv.ranged)) return 0.0f;
        const float need = NeedScore(a_inv.poisons, a_t.poisonTarget, a_t);
        if (need <= 0.0f) return 0.0f;
        a_wantCount = a_t.poisonTarget - a_inv.poisons;
        const float strength = a_t.poisonPowerRef > 0.0f ? LootClamp01(a_c.poisonPower / a_t.poisonPowerRef) : 1.0f;
        return need * (a_t.poisonWeakScale + (1.0f - a_t.poisonWeakScale) * strength);
    }

    // One consumable stack of a_count in the source: take, how many, and why. Reads EVERY effect (marth 2026-11-13): each restore family
    // and each buff kind scores its need against what he carries; a food scales by foodScale; the sale score (gold vs weight, room) stands
    // beside it. A hostile item (poison / drug) has no carry need: sale gold only.
    [[nodiscard]] inline LootVerdict ScoreConsumable(const Consumable& a_c, int a_count, const LootInv& a_inv,
                                                     const LootTuning& a_t = DefaultLootTuning()) {
        LootVerdict v;
        if (a_count <= 0) { v.why = "empty"; return v; }
        const float room = CarryRoom(a_inv, a_t);
        float need = 0.0f; int needShort = 0;
        std::string needWhy;
        if (!a_c.hostile) {
            constexpr const char* fam[] = { "health", "stamina", "magicka" };
            // A FOOD never creates a restore need (review F1): the stock count (CountPotions / PotionRestores) excludes food, so the need
            // could never be seen filled. Its buff kinds still count (the stock side reads those for every ALCH).
            for (int f = 0; f < kRestoreFamilies && !a_c.food; ++f) {
                if (a_c.restoreMag[static_cast<std::size_t>(f)] <= 0.0f) continue;
                const int have = a_inv.restore[static_cast<std::size_t>(f)], tgt = RestoreTarget(f, a_inv, a_t);
                const float s = NeedScore(have, tgt, a_t);
                if (s > need) { need = s; needWhy = std::string("restore ") + fam[f] + " " + std::to_string(have) + "/" + std::to_string(tgt); }
                if (s > 0.0f) needShort = std::max(needShort, tgt - have);
            }
            for (const auto& fx : a_c.fx) {
                const auto i = static_cast<std::size_t>(fx.kind);
                if (i >= kBuffKinds) continue;
                const int have = a_inv.buff[i], tgt = BuffTarget(fx.kind, fx.av, a_inv, a_t);
                const float s = NeedScore(have, tgt, a_t) * a_t.buffScale;
                if (s > need) { need = s; needWhy = std::string(BuffName(fx.kind)) + " " + std::to_string(have) + "/" + std::to_string(tgt); }
                if (s > 0.0f) needShort = std::max(needShort, tgt - have);
            }
            if (a_c.food) need *= a_t.foodScale;
        } else {
            int want = 0;
            need = PoisonCarryValue(a_c, a_inv, a_t, want);   // weapon users only; a drug or a non-weapon follower: 0 (sale gold only)
            if (need > 0.0f) { needShort = want; needWhy = "poison " + std::to_string(a_inv.poisons) + "/" + std::to_string(a_t.poisonTarget); }
        }
        const float sale = a_t.consumableSellRoad ? SaleScore(a_c.value, a_c.weight, room, a_t) : 0.0f;
        char buf[96];
        if (need >= sale) {
            v.score = need;
            v.takeCount = std::min(a_count, needShort);
            v.why = (a_c.food ? "food need " : "need ") + (needWhy.empty() ? std::string("none") : needWhy);
        } else {
            v.score = sale;
            v.takeCount = a_count;
            std::snprintf(buf, sizeof buf, "%ssale %.0fg/%.1fw room=%.2f", a_c.poison ? "poison, " : (a_c.hostile ? "hostile, " : ""), static_cast<double>(a_c.value),
                          static_cast<double>(a_c.weight), static_cast<double>(room));
            v.why = buf;
        }
        v.take = v.score >= a_t.takeThreshold && v.takeCount > 0;
        return v;
    }

}
