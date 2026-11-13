#pragma once
// iaus/ConsumableWorth.h -- IS THIS CONSUMABLE WORTH ITS GOLD, HERE AND NOW (feat/consumables, ClickUp 1dfcuabd2t0).
//
// marth 2026-11-13: "For potions, it needs ot be calculated if using that potion is worth the gold cost of the potion, needed enough in that
// situation to win". One pure rule for EVERY consumable use (a restore potion, a buff potion, a food or drink), read by Cons::ConsumableWorth:
//     worth = need * (1 - costShare * (1 - need))
//   need      0..1  how much THIS situation asks for the item's effect (the outcome without it): a vital resource in real danger = 1 outright, a
//                   hurt resource under a fight's pressure, a fortify / resist / regen kind = the fight's pressure alone. The item's WORST case
//                   over every effect it carries counts (a mixed item: the max over its effects).
//   costShare 0..1  the item's gold value against his wealth (the gold he carries): value / max(gold * wealthFrac, wealthFloor).
//   So the need DOMINATES: at need 1 the worth is 1 whatever the item costs (a dying follower drinks the dear potion), at need 0 it is 0 whatever
//   it costs, and in between a dear item needs a bigger reason than a cheap one.
// FLAT-SAFE: a candidate whose Facts::consumableWorth is < 0 (every family that is not a consumable use, and an item with no known need such as
// invisibility) is NOT APPLICABLE to the consideration, so it scores as before. A follower whose gold is unknown (Kit::gold < 0) has costShare 0.
//
// PURE (standard headers only), so tools/iaus_harness and the sim build it with the host compiler. EVERY NUMBER is a labelled PLACEHOLDER in
// WorthTable, NOT searched: the sim / oracle search sets them later (tools/iaus_sim is NOT edited by this change). Tuning::worth carries the table.
#include <algorithm>
#include <cmath>

namespace MFO::Iaus {

    struct WorthTable {
        // PRESSURE of the fight (0..1): each kept foe adds min(foe level / his level, ratioCap) / ratioCap; the sum / pressureFull, capped at 1.
        // Two equal-level foes (or one twice his level) = 1; a mudcrab against a veteran ~ 0.1.
        float ratioCap = 2.0f;
        float pressureFull = 1.0f;
        // RESOURCE NEED: below `line` (ClassGate::drinkHealthLine, 0.34) a VITAL resource (health) needs the item at 1 whatever the fight; the
        // need falls to 0 at line + dangerSpan. Any resource ABOVE the band is hurt x pressure x hurtWeight. A non-vital resource (stamina,
        // magicka) takes the low band only as far as the pressure.
        float dangerSpan = 0.34f;
        float hurtWeight = 1.0f;
        // CANNOT-WIN FLOOR (fix round, review F1): a need floor from facts the Snapshot already holds, for the case where the fight cannot be won
        // without the resource whatever its pressure reads. PLACEHOLDERS, not searched.
        //   magicka: a caster the fight still asks casts of (Facts::castsWanted > 0) with no weapon offense and a spent pool (<= magickaWinLine)
        //   stamina: a class whose offense needs stamina (a power-attack row and a melee weapon) at <= staminaWinLine
        //   health : his own time to live (Estimate::Ttl on the damage coming in): need 1 at <= ttlFloorSec, 0 at ttlFloorSec + ttlSpanSec
        // Each ramps linearly from 0 at its line to winNeed at an empty pool.
        float magickaWinLine = 0.15f;
        float staminaWinLine = 0.10f;
        float winNeed = 1.0f;
        float ttlFloorSec = 4.0f;
        float ttlSpanSec = 6.0f;
        // COST SHARE: the item's value / max(gold * wealthFrac, wealthFloor), times costWeight, capped at 1.
        float wealthFrac = 0.25f;
        float wealthFloor = 100.0f;
        float costWeight = 1.0f;
        // SCARCITY (marth 2026-11-13: "scarcity should also factor in"): the cost share is divided by 1 + scarcityRate x (carried - 1), so the last
        // one is dear and a full stack is cheap to use (restore potions are mostly abundant). A RESTORE item (potion, food or drink) also pays only
        // restoreCostScale of its cost share, and its need never falls below (1 - the resource now) x restoreUseWeight, so a hurt follower is not
        // held back from a heal that is useful. The gate's main target is the BUFF potions (invisibility, fortify, resist): drunk when NEEDED
        // (the need of a fortify / resist is the fight's pressure alone), not merely when they would help. PLACEHOLDERS.
        float scarcityRate = 0.5f;
        float restoreCostScale = 0.25f;
        float restoreUseWeight = 0.5f;
        // A worth below this vetoes the use (Cons::ConsumableWorth's curve; Generate's fortify gate, which replaces the old
        // kBuffFortifyFoeRatio level rule, reads the same number).
        float floor = 0.2f;
        // FOOD: a restore food is offered only while he carries no restore POTION of that resource, and only if it restores at least this share
        // of the resource's maximum (an apple is not a heal).
        float foodMinGain = 0.05f;
    };

    [[nodiscard]] inline float WorthUnit(float a_v) { return std::isfinite(a_v) ? std::clamp(a_v, 0.0f, 1.0f) : 0.0f; }

    // One foe's share of the pressure (see WorthTable). a_ratio = the foe's level / his level (already safe).
    [[nodiscard]] inline float FoePressureShare(float a_ratio, const WorthTable& a_t) {
        return std::isfinite(a_ratio) ? std::clamp(a_ratio, 0.0f, a_t.ratioCap) / a_t.ratioCap : 0.0f;
    }
    [[nodiscard]] inline float Pressure(float a_shareSum, const WorthTable& a_t) {
        return WorthUnit(a_t.pressureFull > 0.0f ? a_shareSum / a_t.pressureFull : 0.0f);
    }

    // The cannot-win floor of a spent pool (see WorthTable): ramps from 0 at a_line to a_t.winNeed at 0.
    [[nodiscard]] inline float PoolWinFloor(float a_pct, float a_line, const WorthTable& a_t) {
        return a_line > 0.0f ? WorthUnit((a_line - WorthUnit(a_pct)) / a_line) * WorthUnit(a_t.winNeed) : 0.0f;
    }
    // The cannot-win floor of his health from his time to live in seconds (kInf / non-finite = nothing is hitting him = 0).
    [[nodiscard]] inline float DeathWinFloor(float a_ttl, const WorthTable& a_t) {
        if (!std::isfinite(a_ttl) || !(a_t.ttlSpanSec > 0.0f)) return 0.0f;
        return WorthUnit((a_t.ttlFloorSec + a_t.ttlSpanSec - a_ttl) / a_t.ttlSpanSec) * WorthUnit(a_t.winNeed);
    }

    // The need of a resource's restore: a_pct = the resource now (0..1), a_vital = health. a_winFloor = the cannot-win floor (0 = none).
    [[nodiscard]] inline float ResourceNeed(float a_pct, float a_pressure, float a_line, bool a_vital, const WorthTable& a_t, float a_winFloor = 0.0f) {
        const float pct = WorthUnit(a_pct), pressure = WorthUnit(a_pressure);
        const float span = a_t.dangerSpan > 0.0f ? a_t.dangerSpan : 1.0f;
        const float low = WorthUnit((a_line + span - pct) / span) * (a_vital ? 1.0f : pressure);
        const float hurt = (1.0f - pct) * pressure * a_t.hurtWeight;
        return WorthUnit(std::max({ low, hurt, WorthUnit(a_winFloor) }));
    }

    // The item's gold value against his wealth. a_gold < 0 = unknown: no cost is held against the item.
    // a_count = how many of that item (or kind) he carries (scarcity); a_restore = a restore item (its cost is held lightly, see WorthTable).
    [[nodiscard]] inline float CostShare(float a_value, float a_gold, const WorthTable& a_t, int a_count = 1, bool a_restore = false) {
        if (!(a_value > 0.0f) || !std::isfinite(a_value) || !(a_gold >= 0.0f) || !std::isfinite(a_gold)) return 0.0f;
        const float ref = std::max(a_gold * a_t.wealthFrac, a_t.wealthFloor);
        const float scarcity = 1.0f / (1.0f + std::max(0.0f, a_t.scarcityRate) * static_cast<float>(std::max(a_count, 1) - 1));
        return WorthUnit(a_value / ref * a_t.costWeight * scarcity * (a_restore ? WorthUnit(a_t.restoreCostScale) : 1.0f));
    }

    [[nodiscard]] inline float Worth(float a_need, float a_costShare) {
        const float n = WorthUnit(a_need), c = WorthUnit(a_costShare);
        return n * (1.0f - c * (1.0f - n));
    }

}
