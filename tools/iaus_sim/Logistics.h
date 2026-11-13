#pragma once
// iaus_sim/Logistics.h -- THE LOGISTICS WORLD: looting and selling decisions on the sim's seeds, worth tables and oracle. HOST-ONLY.
//
// marth 11-12 (Q-iaus-loot, "Loot sim"): "still find loot the same way we currently do, but then iaus what gets looted by current
// inventory"; "use the same sim engine, for looting decisions, just obviously completely different variables". This is that lab, built
// BEFORE the IAUS loot scorer (post-T2 batch, ClickUp 1dfcuabd18f) so the scorer can be measured the moment it exists.
//
// An EPISODE: a dungeon of kStops stops (the potion chain's 8 fights), then the town. At every stop he finds bodies and a chest; each
// found item is TAKE or LEAVE. In town the errand sells and buys by today's rules. Policies:
//   Baseline = TODAY'S RULES, mirrored from native (Docs/research/2026-11/iaus-sim-v2.md 34.6 lists each rule with its file:line):
//              the default logistics rows in their order (Followers.cpp:48-51): loot_arrows while < 10 arrows, loot_potions while < 2
//              restore-health potions, loot_equipment always; no other category (no default row takes gold, gems, ingredients or junk).
//              PER SOURCE (LootScan.cpp:1046-1090): the rows are evaluated once per body / chest, in order, each condition read at that
//              moment, and a row that fires takes EVERYTHING its action wants from that source: LootAmmo (LootTake.cpp:77, SwapUp.cpp
//              SwapUpAmmoTakeSet: at or above his worst held round and not obsolete on landing, best first; NO BOW GATE, Evaluator.cpp:
//              880-885 "No bow gate (marth)": a follower who does not use arrows restocks with keep target 0, nothing obsolete),
//              LootPotions (LootTake.cpp:97: every drinkable at or above the floor, strongest first), loot_equipment drained one piece per
//              call up to 10 (LootScan.cpp guard; LootEquipment.cpp:483 weapon, then ranged, then armor; with no piece left, a bow user
//              takes ammo STRICTLY above the weakest tier he relies on, SwapUpAmmoFrom upgrade road). Carry check per WHOLE stack, no
//              reserve (LootTake.cpp:40-46). Potion floor: the weakest tier is ignored (Upkeep.cpp ComputeWeakPotionFloor: first
//              magnitude > 1.25 x the minimum). Town: an errand runs when its sell rows pay >= 100 g, carry > 80% (ErrandTrip.cpp,
//              iErrandMinSellGold) or a row needs something (potions < 2, arrows < 10: Economy.cpp:1034, no bow check); BUY first (the
//              rows' needs, potions capped at half the purse: TradeBridge.cpp PlanBuy), then SELL what was planned before the buy: unkept
//              gear, MISC with value/weight >= 20, obsolete ammo (a user's: below the cut at keep target 50; a follower with no bow: below
//              the cut at the arrows row's 10, Economy.cpp:967-988), highest value first, capped by the chest's gold (MFO_Trade.psc
//              RunTrade). Prices: Pricing.cpp, Speech only (no perks in the fixture). No worn-ammo pin is modelled: the engine nocks his
//              best round and the best round is never under the cut, so the pin never changes a sale here (MODEL).
//   Oracle   = a rollout search over TAKE / LEAVE per item and a SELL RUN at a stop's start (a trip to town and back that costs the stop):
//              K sampled futures shared by both options, rolled forward with the baseline, scored by Value(). Any lootable item may be
//              taken (the decision surface an IAUS scorer will have); stolen goods do not exist in the fixture (unowned dungeon).
//   IAUS     = not built (the post-T2 loot scorer). --logistics refuses `--logi-policy iaus` until it exists.
// VALUE (MODEL, every term an open decision for marth, 34.6): J of the potions in the bag at the episode's end, from the COMBAT world's
// potion worth for his class (PotionWorth.h: the k-th potion's worth over a chain of fights) + kGoldJ x gold-equivalent, where
// gold-equivalent = purse + the BUY price of his worn gear and of his arrows up to the keep target (what it would cost to have them;
// arrows are a need only for a ranged class that carries a bow, marth 2026-11-12; otherwise they are worth their sale gold),
// and kGoldJ = J per gold where gold buys the most J: max over potion kinds of worth(first potion) / buy price of that kind's cheapest
// restore potion. Unsold junk at the end is worth 0. Everything the fixture item table says is a Skyrim.esm record value read from the
// Vanilla extract (value, damage, armor rating, magnitude) unless it is marked MODEL.
#include "iaus_sim/Setup.h"
#include "iaus_sim/Tune.h"   // tune 2.11.1: Tune().loot, the IAUS arm's LootTuning (loot.<field>)
#include "iaus/LootNeed.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace Sim::Logi {

    enum class Kind : std::uint8_t { Gold, PotionH, PotionM, PotionS, Ammo, Weapon1H, Weapon2H, Bow, ArmorBody, ArmorHead, Gem, Jewelry, Ingredient, Junk,
        Buff, Food, Poison, SoulGem, Bolt, kCount };   // tune 2.11.1 (--logi-consumables): buff potions, food, poisons, soul gems, bolts
    inline const char* Name(Kind k) {
        static const char* n[] = { "gold", "potion_h", "potion_m", "potion_s", "ammo", "weapon_1h", "weapon_2h", "bow", "armor_body", "armor_head", "gem", "jewelry", "ingredient", "junk",
                                    "buff", "food", "poison", "soulgem", "bolt" };
        return n[static_cast<std::size_t>(k)];
    }
    struct ItemDef { const char* edid; Kind kind; int value; float weight; float power; bool weightModel;
        int bk = -1; int av = -1; float dur = 0; int rfam = -1; };   // tune 2.11.1: a buff potion's BuffKind, its skill / resist av, its duration; a food's restore family (0 health, 1 stamina, 2 magicka)   // power: weapon damage, armor rating, potion magnitude, arrow damage
    // THE FIXTURE ITEMS. value / power: Vanilla extract (Skyrim.esm, exe 1.6.1170) unless weightModel / the comment says MODEL.
    // Weights: weapons from the extract; armor, potions, gems, jewelry, ingredients and junk are NOT in any extract (MODEL, marked).
    inline const std::vector<ItemDef>& Items() {
        static const std::vector<ItemDef> v = {
            { "Gold001", Kind::Gold, 1, 0.0f, 0, false },
            { "RestoreHealth01", Kind::PotionH, 17, 0.5f, 25, true }, { "RestoreHealth02", Kind::PotionH, 73, 0.5f, 50, true }, { "RestoreHealth03", Kind::PotionH, 57, 0.5f, 75, true },
            { "RestoreMagicka01", Kind::PotionM, 20, 0.5f, 25, true }, { "RestoreMagicka02", Kind::PotionM, 73, 0.5f, 50, true },
            { "RestoreStamina01", Kind::PotionS, 20, 0.5f, 25, true }, { "RestoreStamina02", Kind::PotionS, 44, 0.5f, 50, true },
            { "IronArrow", Kind::Ammo, 1, 0.0f, 8, true }, { "DraugrArrow", Kind::Ammo, 1, 0.0f, 10, true }, { "SteelArrow", Kind::Ammo, 2, 0.0f, 10, true }, { "OrcishArrow", Kind::Ammo, 3, 0.0f, 12, true },
            { "IronSword", Kind::Weapon1H, 25, 9.0f, 7, false }, { "DraugrSword", Kind::Weapon1H, 13, 12.0f, 8, false }, { "SteelSword", Kind::Weapon1H, 45, 10.0f, 8, false },
            { "IronWarAxe", Kind::Weapon1H, 30, 11.0f, 8, false }, { "DraugrWarAxe", Kind::Weapon1H, 15, 14.0f, 9, false }, { "SteelGreatsword", Kind::Weapon2H, 90, 17.0f, 17, false },
            { "LongBow", Kind::Bow, 30, 5.0f, 6, false }, { "HuntingBow", Kind::Bow, 50, 7.0f, 7, false }, { "ImperialBow", Kind::Bow, 90, 8.0f, 9, false },
            { "ArmorHideCuirass", Kind::ArmorBody, 50, 6.0f, 20, true }, { "ArmorLeatherCuirass", Kind::ArmorBody, 125, 6.0f, 26, true }, { "ArmorIronCuirass", Kind::ArmorBody, 125, 30.0f, 25, true },
            { "ArmorDraugrCuirass", Kind::ArmorBody, 125, 30.0f, 25, true }, { "ArmorSteelCuirassA", Kind::ArmorBody, 275, 35.0f, 31, true }, { "ArmorIronHelmet", Kind::ArmorHead, 60, 5.0f, 15, true },
            { "GemGarnet (MODEL)", Kind::Gem, 100, 0.1f, 0, true }, { "GemAmethyst (MODEL)", Kind::Gem, 120, 0.1f, 0, true }, { "SilverRing (MODEL)", Kind::Jewelry, 30, 0.25f, 0, true },
            { "Ingredient (MODEL)", Kind::Ingredient, 2, 0.1f, 0, true }, { "Clutter (MODEL)", Kind::Junk, 3, 2.0f, 0, true },
            // tune 2.11.1, drawn only with --logi-consumables. Magnitudes / durations: the Vanilla extract (FortifyHealth01 20 / 60 s, FortifyStamina01
            // 20 / 300 s, FortifyMagicka01 20 / 60 s; FoodCheeseWheel01A 15 health, FoodVenisonStew 15 stamina; bolts 10 damage). Potion values: the
            // ALCH auto-calc of those effects (Setup.h AutoCalcValue: 68 / 341 / 58 g; the MODEL resist and skill potions at baseCost 0.5: 97 g). MODEL (no extract has them): the resist and fortify-skill
            // potions, the poison, the soul gem, food values, every weight and the bolt's value.
            { "FortifyHealth01", Kind::Buff, 68, 0.5f, 20, true, 5, 24, 60 }, { "FortifyStamina01", Kind::Buff, 341, 0.5f, 20, true, 6, 26, 300 },
            { "FortifyMagicka01", Kind::Buff, 58, 0.5f, 20, true, 7, 25, 60 }, { "ResistFire (MODEL)", Kind::Buff, 97, 0.5f, 20, true, 0, 41, 60 },
            { "FortifyOneHanded (MODEL)", Kind::Buff, 97, 0.5f, 20, true, 8, 6, 60 }, { "FortifyMarksman (MODEL)", Kind::Buff, 97, 0.5f, 20, true, 8, 8, 60 },
            { "FoodCheeseWheel01A", Kind::Food, 10, 2.0f, 15, true, -1, 24, 0, 0 }, { "FoodVenisonStew", Kind::Food, 8, 1.0f, 15, true, -1, 26, 0, 1 },
            { "DamageHealthPoison (MODEL)", Kind::Poison, 40, 0.5f, 15, true },   // power = magnitude x duration summed (15 x 1 s)
            { "SoulGemPetty (MODEL)", Kind::SoulGem, 10, 0.1f, 0, true }, { "SteelBolt", Kind::Bolt, 2, 0.0f, 10, true },
        };
        return v;
    }
    inline int ItemIdx(const char* edid) { const auto& v = Items(); for (std::size_t i = 0; i < v.size(); ++i) if (!std::strcmp(v[i].edid, edid)) return static_cast<int>(i); return -1; }

    // PRICES (Pricing.cpp, Pricing.h:6-19): Skyrim.esm GMSTs (gmst_defaults.json, exe 1.6.1170). No perks in the fixture follower.
    inline constexpr float kBarterMax = 2.4000001f, kBarterMin = 1.4f, kBarterBuyMin = 1.05f, kBarterSellMax = 0.95f, kMinBuyMult = 1.0f, kMaxSellMult = 1.0f;
    struct Quote { float buy = 1, sell = 1; };
    inline Quote QuoteFor(float speech) {
        const float s = std::clamp(speech, 0.0f, 100.0f); Quote q;
        float b0 = std::max(kBarterMax - (kBarterMax - kBarterMin) * s * 0.01f, kBarterBuyMin); float s0 = b0 > 0 ? 1.0f / b0 : kBarterSellMax; s0 = std::min(s0, kBarterSellMax);
        q.buy = std::max(b0, std::max(kMinBuyMult, 1.0f)); q.sell = std::min(s0, std::min(kMaxSellMult, 1.0f)); return q;
    }
    inline int SellUnit(const ItemDef& d, const Quote& q) { if (d.value <= 0) return d.kind == Kind::Ammo ? 1 : 0; return std::max(d.kind == Kind::Ammo ? 1 : 0, static_cast<int>(std::lround(d.value * q.sell))); }
    inline int BuyUnit(const ItemDef& d, const Quote& q) { return std::max(1, static_cast<int>(std::lround(d.value * q.buy))); }

    // THE RULE CONSTANTS (native defaults, 34.6)
    inline constexpr int   kStops = 8;              // the potion chain's length (PotionWorth.h, chain 8): the dungeon between two towns
    inline constexpr float kValuablesRatio = 20.0f; // fValuablesRatio (Config.h), MISC value / weight to count as a valuable
    inline constexpr int   kMinSellGold = 100;      // iErrandMinSellGold
    inline constexpr float kCarryRatioErrand = 0.8f;// ErrandTrip.cpp [C]
    inline constexpr int   kAmmoKeepFloor = 50;     // SwapUp.cpp kAmmoKeepFloor [C]
    inline constexpr int   kRowPotions = 2, kRowArrows = 10;   // the default rows' params (Followers.cpp:48-51)
    // MODEL fixtures (no extract has them): his carry weight, Speech, the vendor's chest gold and stock.
    inline constexpr float kCarryCap = 300.0f, kSpeech = 15.0f;
    inline constexpr int   kVendorGold = 750;

    struct Stack { int item = -1; int count = 0; };
    struct Inv {
        std::vector<Stack> s; int purse = 0;
        float charge = 1.0f;   // tune 2.11.1: the lowest charge fraction of his enchanted weapon (1 = none or full; Ep::charge)
        int bestWeapon = -1, bestBow = -1, body = -1, head = -1;   // worn / in use (the equip authority equips an upgrade it takes)
        [[nodiscard]] float Weight() const { float w = 0; for (const Stack& x : s) w += Items()[static_cast<std::size_t>(x.item)].weight * float(x.count); return w; }
        [[nodiscard]] int Count(Kind k) const { int n = 0; for (const Stack& x : s) if (Items()[static_cast<std::size_t>(x.item)].kind == k) n += x.count; return n; }
        [[nodiscard]] float WorstAmmo() const { float w = 1e9f; for (const Stack& x : s) if (Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo && x.count > 0) w = std::min(w, Items()[static_cast<std::size_t>(x.item)].power); return w; }
        void Add(int item, int n) { if (Items()[static_cast<std::size_t>(item)].kind == Kind::Gold) { purse += n; return; } for (Stack& x : s) if (x.item == item) { x.count += n; return; } s.push_back({ item, n }); }
    };
    struct Who { std::string cls; Cls fam = Cls::Melee; const PotWorth* worth = nullptr; const std::array<double, MFO::Iaus::kBuffKinds + 3>* uses = nullptr;   // uses: --logi-uses, per fight
        [[nodiscard]] bool Bow() const { return fam == Cls::Ranged; } };

    inline Inv StartKit(const Who& w) {   // MODEL: a T1 kit of his family (the combat world's iron rung)
        Inv v; v.purse = 50; v.Add(ItemIdx("RestoreHealth02"), 1);
        if (w.fam == Cls::Mage) { v.Add(ItemIdx("RestoreMagicka02"), 1); return v; }
        v.Add(ItemIdx("IronSword"), 1); v.bestWeapon = ItemIdx("IronSword");
        if (w.Bow()) { v.Add(ItemIdx("LongBow"), 1); v.bestBow = ItemIdx("LongBow"); v.Add(ItemIdx("IronArrow"), 30); v.Add(ItemIdx("ArmorHideCuirass"), 1); v.body = ItemIdx("ArmorHideCuirass"); }
        else { v.Add(ItemIdx("ArmorIronCuirass"), 1); v.body = ItemIdx("ArmorIronCuirass"); }
        return v;
    }

    // LOOT FOUND AT A STOP (MODEL: the shape of a draugr crypt's bodies and its chest; a list world would read real containers, 34.6)
    struct Source { std::vector<Stack> items; };
    inline bool g_logiConsumables = false;   // --logi-consumables (tune 2.11.1): the stops also hold buff potions, food, poisons, soul gems and bolts
    // MODEL (tune 2.11.1): how often a body / the chest holds the new kinds (drawn after main's draws, so off = main's stops draw for draw)
    inline void ExtraFinds(Rng& R, Source& s, bool chest) {
        static const char* bb[] = { "FortifyHealth01", "FortifyStamina01", "FortifyMagicka01", "ResistFire (MODEL)", "FortifyOneHanded (MODEL)", "FortifyMarksman (MODEL)" };
        if (R.P(chest ? 0.3f : 0.12f)) s.items.push_back({ ItemIdx(bb[R.I(0, 5)]), 1 });
        if (R.P(chest ? 0.1f : 0.1f)) s.items.push_back({ R.P(0.5f) ? ItemIdx("FoodCheeseWheel01A") : ItemIdx("FoodVenisonStew"), R.I(1, 2) });
        if (R.P(chest ? 0.15f : 0.1f)) s.items.push_back({ ItemIdx("DamageHealthPoison (MODEL)"), 1 });
        if (R.P(chest ? 0.2f : 0.1f)) s.items.push_back({ ItemIdx("SoulGemPetty (MODEL)"), 1 });
        if (!chest && R.P(0.1f)) s.items.push_back({ ItemIdx("SteelBolt"), R.I(3, 10) });
    }
    inline std::vector<Source> MakeStop(Rng& R) {
        std::vector<Source> out; const int bodies = R.I(2, 5);
        static const char* bw[] = { "DraugrSword", "DraugrWarAxe", "IronSword", "SteelSword", "IronWarAxe", "SteelGreatsword", "LongBow", "HuntingBow", "ImperialBow" };
        static const char* ba[] = { "ArmorHideCuirass", "ArmorLeatherCuirass", "ArmorIronCuirass", "ArmorDraugrCuirass", "ArmorSteelCuirassA", "ArmorIronHelmet" };
        static const char* bp[] = { "RestoreHealth01", "RestoreHealth02", "RestoreHealth03", "RestoreMagicka01", "RestoreMagicka02", "RestoreStamina01", "RestoreStamina02" };
        static const char* bm[] = { "IronArrow", "DraugrArrow", "SteelArrow", "OrcishArrow" };
        auto pick = [&](const char* const* a, int n) { return ItemIdx(a[R.I(0, n - 1)]); };
        for (int b = 0; b < bodies; ++b) { Source s;
            if (R.P(0.5f)) s.items.push_back({ pick(bw, 9), 1 });
            if (R.P(0.3f)) s.items.push_back({ pick(ba, 6), 1 });
            if (R.P(0.4f)) s.items.push_back({ pick(bm, 4), R.I(3, 10) });
            if (R.P(0.2f)) s.items.push_back({ pick(bp, 7), 1 });
            if (R.P(0.3f)) s.items.push_back({ ItemIdx("Gold001"), R.I(5, 30) });
            if (R.P(0.15f)) s.items.push_back({ R.P(0.5f) ? ItemIdx("GemGarnet (MODEL)") : ItemIdx("GemAmethyst (MODEL)"), 1 });
            if (R.P(0.25f)) s.items.push_back({ ItemIdx("Ingredient (MODEL)"), R.I(1, 3) });
            if (R.P(0.3f)) s.items.push_back({ ItemIdx("Clutter (MODEL)"), 1 });
            if (g_logiConsumables) ExtraFinds(R, s, false);
            out.push_back(std::move(s)); }
        Source c; c.items.push_back({ ItemIdx("Gold001"), R.I(20, 80) });
        for (int k = R.I(1, 2); k > 0; --k) c.items.push_back({ pick(bp, 7), 1 });
        if (R.P(0.3f)) c.items.push_back({ ItemIdx("GemGarnet (MODEL)"), 1 });
        if (R.P(0.2f)) c.items.push_back({ ItemIdx("SilverRing (MODEL)"), 1 });
        if (g_logiConsumables) ExtraFinds(R, c, true);
        out.push_back(std::move(c));
        return out;
    }

    // ── today's rules ──────────────────────────────────────────────────────────────────────────────────────────────────────────
    inline float PotionFloor() {   // Upkeep.cpp ComputeWeakPotionFloor over every restore potion of the load order (here: the fixture's)
        std::vector<float> m; for (const ItemDef& d : Items()) if (d.kind == Kind::PotionH || d.kind == Kind::PotionM || d.kind == Kind::PotionS) m.push_back(d.power);
        std::sort(m.begin(), m.end()); for (float x : m) if (x > m.front() * 1.25f) return x; return m.front();
    }
    inline bool Fits(const Inv& v, const ItemDef& d, int n) { return d.kind == Kind::Gold || v.Weight() + d.weight * float(n) <= kCarryCap; }   // FitsCarryWeight: the whole stack
    [[nodiscard]] inline bool IsUpgrade(const Inv& v, const Who& w, const ItemDef& d) {   // LootEquipment.cpp: a strict upgrade of his own class (one rung of WeaponScore / ArmorScore)
        const auto& I = Items(); auto p = [&](int i) { return i < 0 ? -1.0f : I[static_cast<std::size_t>(i)].power; };
        switch (d.kind) {
        case Kind::Weapon1H: return w.fam != Cls::Mage && d.power > p(v.bestWeapon) && (v.bestWeapon < 0 || I[static_cast<std::size_t>(v.bestWeapon)].kind == Kind::Weapon1H);
        case Kind::Weapon2H: return w.fam != Cls::Mage && v.bestWeapon >= 0 && I[static_cast<std::size_t>(v.bestWeapon)].kind == Kind::Weapon2H && d.power > p(v.bestWeapon);   // his melee class is one-handed in the kit
        case Kind::Bow: return w.Bow() && d.power > p(v.bestBow);
        case Kind::ArmorBody: return w.fam != Cls::Mage && v.body >= 0 && d.power > p(v.body);   // mage apparel mode: robes are judged apart (not in the fixture)
        case Kind::ArmorHead: return w.fam != Cls::Mage && d.power > p(v.head);
        default: return false;
        }
    }
    inline void Equip(Inv& v, int item) { const Kind k = Items()[static_cast<std::size_t>(item)].kind;
        if (k == Kind::Weapon1H || k == Kind::Weapon2H) v.bestWeapon = item; else if (k == Kind::Bow) v.bestBow = item; else if (k == Kind::ArmorBody) v.body = item; else if (k == Kind::ArmorHead) v.head = item; }
    // ── ammo (SwapUp.cpp; rank = damage, then value: Logistics_internal.h AmmoRankAbove) ──────────────────────────────────────
    [[nodiscard]] inline bool RankAbove(int a, int b) { const ItemDef& x = Items()[static_cast<std::size_t>(a)]; const ItemDef& y = Items()[static_cast<std::size_t>(b)]; return x.power > y.power || (x.power == y.power && x.value > y.value); }
    [[nodiscard]] inline int KeepTarget(const Who& w) { return w.Bow() ? std::max(kAmmoKeepFloor, kRowArrows) : 0; }   // AmmoKeepTarget: 0 when he does not use the kind
    [[nodiscard]] inline std::vector<Stack> HeldAmmo(const Inv& v) { std::vector<Stack> h; for (const Stack& x : v.s) if (x.count > 0 && Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo) h.push_back(x); return h; }
    [[nodiscard]] inline int Cutoff(std::vector<Stack> pool, int target) {   // AmmoCutoff: the tier at which the best `target` rounds are reached; -1 = none
        std::stable_sort(pool.begin(), pool.end(), [](const Stack& a, const Stack& b) { return RankAbove(a.item, b.item); });
        if (target <= 0) return -1; long cum = 0; for (const Stack& x : pool) { cum += x.count; if (cum >= target) return x.item; } return -1;
    }
    [[nodiscard]] inline bool Obsolete(int item, int cut) { return cut >= 0 && RankAbove(cut, item); }   // AmmoObsolete (no special / silver round in the fixture)
    // SwapUpAmmoTakeSet: which of the source's ammo stacks the rule takes. Restock (the arrows row): at or above his worst held round.
    // Upgrade (loot_equipment, a user only): strictly above the weakest tier he relies on (AmmoUpgradeBar), nothing held = no upgrade.
    [[nodiscard]] inline std::vector<Stack> AmmoTakeSet(const Inv& v, const Who& w, const std::vector<Stack>& body, bool upgradeOnly) {
        const int target = KeepTarget(w); const std::vector<Stack> held = HeldAmmo(v); int floor = -1; bool strict = false;
        if (upgradeOnly) {
            if (target <= 0 || held.empty()) return {};
            std::vector<Stack> pool = held; std::stable_sort(pool.begin(), pool.end(), [](const Stack& a, const Stack& b) { return RankAbove(a.item, b.item); });
            int bar = pool.back().item; long cum = 0; for (const Stack& x : pool) { cum += x.count; if (cum >= target) { bar = x.item; break; } }
            long above = 0; for (const Stack& x : pool) if (RankAbove(x.item, bar)) above += x.count;
            if (target - above <= 0) return {};
            floor = bar; strict = true;
        } else for (const Stack& h : held) if (floor < 0 || RankAbove(floor, h.item)) floor = h.item;
        std::vector<Stack> pool = held; pool.insert(pool.end(), body.begin(), body.end()); const int cut = Cutoff(pool, target);
        std::vector<Stack> take;
        for (const Stack& b : body) { bool ok = true; if (floor >= 0) ok = strict ? RankAbove(b.item, floor) : !RankAbove(floor, b.item); if (ok && !Obsolete(b.item, cut)) take.push_back(b); }
        std::stable_sort(take.begin(), take.end(), [](const Stack& a, const Stack& b) { return RankAbove(a.item, b.item); });   // best first (the carry gate drops iron)
        return take;
    }

    struct Tally;
    inline void TakeStack(Inv& v, const Who& w, const Stack& st, Tally* t, bool baseline);
    // TODAY'S RULES ON ONE SOURCE (LootScan.cpp:1046-1090), over its stacks from index `from` (a rollout that resumes mid-source applies
    // the rows to what is left of it). Each row's condition is read when the row is reached; a row that fires drains its category.
    // `took` (optional) marks which stacks were taken.
    inline void BaselineSource(Inv& v, const Who& w, const std::vector<Stack>& items, std::size_t from, Tally* t, std::vector<char>* took = nullptr) {
        std::vector<char> gone(items.size(), 0); for (std::size_t i = 0; i < from && i < items.size(); ++i) gone[i] = 1;
        auto take = [&](std::size_t i) { TakeStack(v, w, items[i], t, true); gone[i] = 1; if (took) (*took)[i] = 1; };
        auto ammoRoad = [&](bool upgradeOnly) {   // SwapUpAmmoFrom: the whole take set, best first, each stack carry-checked
            std::vector<Stack> body; std::vector<std::size_t> at;
            for (std::size_t i = 0; i < items.size(); ++i) if (!gone[i] && Items()[static_cast<std::size_t>(items[i].item)].kind == Kind::Ammo) { body.push_back(items[i]); at.push_back(i); }
            if (body.empty()) return false; bool moved = false;
            for (const Stack& s : AmmoTakeSet(v, w, body, upgradeOnly)) for (std::size_t k = 0; k < body.size(); ++k)
                if (!gone[at[k]] && body[k].item == s.item && body[k].count == s.count) { if (Fits(v, Items()[static_cast<std::size_t>(s.item)], s.count)) { take(at[k]); moved = true; } break; }
            return moved;
        };
        if (v.Count(Kind::Ammo) < kRowArrows) ammoRoad(false);   // loot_arrows (row 2), no bow gate
        if (v.Count(Kind::PotionH) < kRowPotions) {             // loot_potions (row 3): every drinkable at or above the floor, strongest first
            std::vector<std::size_t> p; const float fl = PotionFloor();
            for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = Items()[static_cast<std::size_t>(items[i].item)];
                if (!gone[i] && (d.kind == Kind::PotionH || d.kind == Kind::PotionM || d.kind == Kind::PotionS) && !(d.power > 0 && d.power < fl)) p.push_back(i); }
            std::stable_sort(p.begin(), p.end(), [&](std::size_t a, std::size_t b) { return Items()[static_cast<std::size_t>(items[a].item)].power > Items()[static_cast<std::size_t>(items[b].item)].power; });
            for (std::size_t i : p) if (Fits(v, Items()[static_cast<std::size_t>(items[i].item)], items[i].count)) take(i);
        }
        for (int guard = 10; guard > 0; --guard) {               // loot_equipment (row 4, always): one piece per call, drained
            auto bestOf = [&](std::initializer_list<Kind> ks) { int b = -1; for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = Items()[static_cast<std::size_t>(items[i].item)];
                    bool in = false; for (Kind k : ks) in = in || d.kind == k; if (!gone[i] && in && IsUpgrade(v, w, d) && (b < 0 || d.power > Items()[static_cast<std::size_t>(items[static_cast<std::size_t>(b)].item)].power)) b = static_cast<int>(i); } return b; };
            int b = bestOf({ Kind::Weapon1H, Kind::Weapon2H }); if (b < 0) b = bestOf({ Kind::Bow }); if (b < 0) b = bestOf({ Kind::ArmorBody, Kind::ArmorHead });   // LootEquipment.cpp:483
            if (b >= 0 && !Fits(v, Items()[static_cast<std::size_t>(items[static_cast<std::size_t>(b)].item)], items[static_cast<std::size_t>(b)].count)) b = -1;   // no room: skipped, the call goes on to ammo
            if (b >= 0) { take(static_cast<std::size_t>(b)); continue; }
            if (!ammoRoad(true)) break;
        }
    }
    // Would today's rules take stack i of this source, from where he stands now? (the oracle's label of a divergence)
    [[nodiscard]] inline bool BaselineTakes(const Inv& v, const Who& w, const std::vector<Stack>& items, std::size_t i) {
        Inv u = v; std::vector<char> took(items.size(), 0); BaselineSource(u, w, items, i, nullptr, &took); return took[i] != 0;
    }
    struct Errand { int sold = 0, bought = 0; bool ran = false; };
    [[nodiscard]] inline bool Sellable(const Inv& v, const Stack& x) {   // EconomyProbeImpl: unkept gear, valuable MISC, obsolete ammo (gold, potions, ingredients never)
        const ItemDef& d = Items()[static_cast<std::size_t>(x.item)];
        switch (d.kind) {
        case Kind::Weapon1H: case Kind::Weapon2H: case Kind::Bow: case Kind::ArmorBody: case Kind::ArmorHead: case Kind::Jewelry:
            return x.item != v.bestWeapon && x.item != v.bestBow && x.item != v.body && x.item != v.head;
        case Kind::Gem: return false;   // soul gems are never sold; a GEM here is a MISC valuable: see Junk/Gem split below
        case Kind::Junk: return d.weight > 0 ? d.value / d.weight >= kValuablesRatio : d.value > 0;
        default: return false;
        }
    }
    [[nodiscard]] inline bool SellableGem(const ItemDef& d) { return d.kind == Kind::Gem && (d.weight > 0 ? d.value / d.weight >= kValuablesRatio : d.value > 0); }   // a gem is MISC (IsValuableMisc)
    // THE AMMO HE SELLS (Economy.cpp:955-993): a user sells what is obsolete at his keep target; one who carries no bow sells every
    // round below the cut at the arrows row's N (10); one who carries a bow he does not use sells none (target 0).
    [[nodiscard]] inline std::vector<Stack> SellAmmo(const Inv& v, const Who& w) {
        const std::vector<Stack> held = HeldAmmo(v); int target = KeepTarget(w);
        if (!w.Bow() && v.Count(Kind::Bow) == 0) target = kRowArrows;   // off-kind: keep the best N the row asks for (keepN > 0 always here)
        if (target <= 0) return {};
        const int cut = Cutoff(held, target); std::vector<Stack> out; for (const Stack& x : held) if (Obsolete(x.item, cut)) out.push_back(x); return out;
    }
    inline Errand Town(Inv& v, const Who& w, bool force) {   // the errand: TryStart's trigger, then BUY first, then SELL (RunTrade)
        const Quote q = QuoteFor(kSpeech); Errand e; int sellTotal = 0;
        std::vector<Stack> rows;   // planned before the buy (EconomyProbeImpl): item and count; the sale moves min(planned, held)
        for (const Stack& x : v.s) { const ItemDef& d = Items()[static_cast<std::size_t>(x.item)]; if (x.count > 0 && (Sellable(v, x) || SellableGem(d))) rows.push_back(x); }
        for (const Stack& x : SellAmmo(v, w)) rows.push_back(x);
        for (const Stack& x : rows) sellTotal += SellUnit(Items()[static_cast<std::size_t>(x.item)], q) * x.count;
        const bool needs = v.Count(Kind::PotionH) < kRowPotions || v.Count(Kind::Ammo) < kRowArrows;   // Economy.cpp:1034: no bow check
        if (!force && !(std::min(sellTotal, kVendorGold) >= kMinSellGold || v.Weight() > kCarryRatioErrand * kCarryCap || needs)) return e;
        e.ran = true; int chest = kVendorGold;
        {   // BUY: the rows' needs, potions capped at half the purse (PlanBuy), the dearest supply line first
            int potLeft = v.purse / 2;
            for (const char* id : { "RestoreHealth02", "RestoreHealth03", "RestoreHealth01" }) { const int it = ItemIdx(id); const int u = BuyUnit(Items()[static_cast<std::size_t>(it)], q);   // the dearest line first
                while (v.Count(Kind::PotionH) < kRowPotions && u <= potLeft && u <= v.purse) { v.Add(it, 1); v.purse -= u; potLeft -= u; chest += u; e.bought += u; } }
            { const int it = ItemIdx("IronArrow"); const int u = BuyUnit(Items()[static_cast<std::size_t>(it)], q);   // the arrows row's need, whatever he wields
                while (v.Count(Kind::Ammo) < kRowArrows && u <= v.purse) { v.Add(it, 1); v.purse -= u; chest += u; e.bought += u; } }
        }
        std::stable_sort(rows.begin(), rows.end(), [&](const Stack& a, const Stack& b) { return Items()[static_cast<std::size_t>(a.item)].value > Items()[static_cast<std::size_t>(b.item)].value; });
        for (const Stack& r : rows) { Stack* x = nullptr; for (Stack& y : v.s) if (y.item == r.item) x = &y; if (!x) continue;
            const int u = SellUnit(Items()[static_cast<std::size_t>(r.item)], q); if (u <= 0 || u > chest) continue;
            const int n = std::min({ r.count, x->count, chest / u }); x->count -= n; v.purse += u * n; chest -= u * n; e.sold += u * n; }
        return e;
    }

    // ── the value of where he ends (MODEL, header) ───────────────────────────────────────────────────────────────────────────
    inline double GoldJ(const Who& w) {
        if (!w.worth) return 0.0; const Quote q = QuoteFor(kSpeech); double best = 0;
        const char* cheap[3] = { "RestoreHealth01", "RestoreMagicka01", "RestoreStamina01" };
        for (int t = 0; t < 3; ++t) best = std::max(best, double(w.worth->m[static_cast<std::size_t>(t)][0]) / BuyUnit(Items()[static_cast<std::size_t>(ItemIdx(cheap[t]))], q));
        return best;
    }
    inline double PotionJ(const Inv& v, const Who& w) {
        double j = 0; if (!w.worth) return 0; const int n[3] = { v.Count(Kind::PotionH), v.Count(Kind::PotionM), v.Count(Kind::PotionS) };
        for (int t = 0; t < 3; ++t) for (int k = 0; k < std::min(n[t], kPotWorthMax); ++k) j += w.worth->m[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)];
        return j;
    }
    inline double Value(const Inv& v, const Who& w) {
        const Quote q = QuoteFor(kSpeech); const Quote& q0 = q; double j = 0;
        if (w.worth) { const int n[3] = { v.Count(Kind::PotionH), v.Count(Kind::PotionM), v.Count(Kind::PotionS) };
            for (int t = 0; t < 3; ++t) for (int k = 0; k < std::min(n[t], kPotWorthMax); ++k) j += w.worth->m[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)]; }
        double g = v.purse; for (int it : { v.bestWeapon, v.bestBow, v.body, v.head }) if (it >= 0) g += BuyUnit(Items()[static_cast<std::size_t>(it)], q);
        // AMMO'S WORTH (marth 2026-11-12, the ammo need): a NEED only when his class uses ranged AND he carries a launcher of the kind
        // (a bow for arrows): then up to the keep target at the buy price; otherwise ammo is worth its sale gold only. The BASELINE's
        // take rules are native's (no bow gate); this is the value both arms are scored by.
        if (!(w.Bow() && v.Count(Kind::Bow) > 0)) { for (const Stack& x : v.s) if (Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo) g += x.count * SellUnit(Items()[static_cast<std::size_t>(x.item)], q); }
        else { int left = kAmmoKeepFloor; std::vector<std::pair<float, int>> am; for (const Stack& x : v.s) if (Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo) am.push_back({ Items()[static_cast<std::size_t>(x.item)].power, x.item });
            std::sort(am.rbegin(), am.rend()); for (auto& [pw, it] : am) { (void)pw; for (const Stack& x : v.s) if (x.item == it) { const int n = std::min(left, x.count); g += n * BuyUnit(Items()[static_cast<std::size_t>(it)], q); left -= n; } } }
        if (g_logiConsumables && w.uses) {   // tune 2.11.1 (MODEL, open decision as the rest of VALUE): a buff potion or food is worth its BUY price while the combat
            // world would use it (uses per fight x kStops fights, --logi-uses), nothing past that (no sell road for ALCH). Poisons: no apply road, soul gems: no
            // recharge road, bolts: no crossbow and no ammo sale road in the model: 0. The loot table's targets never enter this.
            std::array<double, MFO::Iaus::kBuffKinds + 3> left{}; for (std::size_t i = 0; i < left.size(); ++i) left[i] = (*w.uses)[i] * kStops;
            std::vector<std::pair<int, int>> cs; for (const Stack& x : v.s) { const ItemDef& d = Items()[static_cast<std::size_t>(x.item)]; if (x.count > 0 && (d.kind == Kind::Buff || d.kind == Kind::Food)) cs.push_back({ -d.value, x.item }); }
            std::sort(cs.begin(), cs.end());   // the dearest stack first: the uses go to what it would cost most to replace
            for (auto& [nv, it] : cs) { (void)nv; const ItemDef& d = Items()[static_cast<std::size_t>(it)]; const std::size_t slot = d.kind == Kind::Buff ? static_cast<std::size_t>(d.bk) : MFO::Iaus::kBuffKinds + static_cast<std::size_t>(d.rfam);
                int n = 0; for (const Stack& x : v.s) if (x.item == it) n += x.count;
                for (int q = 0; q < n && left[slot] > 0.0; ++q) { g += std::min(1.0, left[slot]) * BuyUnit(d, q0); left[slot] -= 1.0; } }
        }
        return j + GoldJ(w) * g;
    }

    // ── an episode under a policy ────────────────────────────────────────────────────────────────────────────────────────────
    struct Div { long n = 0; double dU = 0; };
    struct Tally { long takes = 0, carryBreaks = 0, offRowTakes = 0, sellRuns = 0; std::map<std::string, Div> div; Inv end; };
    struct Ep { std::vector<std::vector<Source>> stops; float charge = 1.0f; };
    inline Ep MakeEpisode(Rng& R) { Ep e; for (int s = 0; s < kStops; ++s) e.stops.push_back(MakeStop(R)); if (g_logiConsumables) e.charge = R.U(0.0f, 1.0f); return e; }   // charge: MODEL, his enchanted weapon's lowest charge
    inline Inv Start(const Who& w, const Ep& e) { Inv v = StartKit(w); if (g_logiConsumables && w.fam != Cls::Mage) v.charge = e.charge; return v; }

    inline void TakeStack(Inv& v, const Who& w, const Stack& st, Tally* t, bool baseline) {
        const ItemDef& d = Items()[static_cast<std::size_t>(st.item)];
        if (t) { ++t->takes; if (!Fits(v, d, st.count)) ++t->carryBreaks;   // the baseline takes only what a default row names (ammo, drinkables, his upgrades)
            if (baseline && !(d.kind == Kind::Ammo || d.kind == Kind::PotionH || d.kind == Kind::PotionM || d.kind == Kind::PotionS || IsUpgrade(v, w, d))) ++t->offRowTakes; }
        const bool up = IsUpgrade(v, w, d); v.Add(st.item, st.count); if (up) Equip(v, st.item);
    }
    // The baseline over stops [s0, kStops) of an episode, from item i0 of stop s0; then the town. Returns Value().
    inline double RunBaseline(Inv v, const Who& w, const Ep& e, int s0, std::size_t src0, std::size_t i0, Tally* t) {
        for (int s = s0; s < kStops; ++s) { const auto& st = e.stops[static_cast<std::size_t>(s)];
            for (std::size_t a = (s == s0 ? src0 : 0); a < st.size(); ++a) BaselineSource(v, w, st[a].items, s == s0 && a == src0 ? i0 : 0, t); }
        Town(v, w, false); if (t) t->end = v; return Value(v, w);
    }
    // ── THE IAUS ARM (tune 2.11.1): native's IAUS looting on the same stops, the NATIVE scorer compiled in (iaus/LootNeed.h ScoreLootKind /
    // ScoreConsumable) with the table under search (Tune().loot, loot.<field>). Mirrors native/logistics/LootNeed.cpp + LootScan.cpp:1053-1061:
    // per source, the kinds whose ScoreLootKind is a TAKE, best score first (IausLootPlan, read once per source); each kind's road runs once
    // (Equipment up to 10 calls); the inventory is read again after every take (IausLootInvDirty). Roads: Arrows = LootAmmo (the restock take
    // set), Potions = LootConsumablesNeed (every ALCH in the source scored, best first, re-judged against what he carries now, takeCount of
    // the stack, ApplyLootFloor: a restore below the weak-potion floor carries no restore), Equipment = loot_equipment's road, Gold = all
    // coin, Jewelry = every ring, SoulGems = every soul gem (LootSoulGems), Valuables = MISC at value / weight >= fValuablesRatio. The town
    // errand is today's (its buy list is the rows' needs, 34.6) in every arm. The IAUS arm replaces the rows: they never run on top of it.
    inline MFO::Iaus::LootInv LootInvOf(const Inv& v, const Who& w) {
        MFO::Iaus::LootInv inv; using namespace MFO::Iaus;
        inv.usesArrows = w.Bow() && v.Count(Kind::Bow) > 0; inv.usesBolts = false;   // no crossbow in the fixture
        inv.arrows = v.Count(Kind::Ammo); inv.bolts = v.Count(Kind::Bolt);
        inv.arrowTarget = inv.usesArrows ? KeepTarget(w) : 0; inv.boltTarget = 0;   // AmmoKeepTarget: 0 for a kind he does not use
        inv.carried = v.Weight(); inv.capacity = kCarryCap;
        inv.melee = w.fam != Cls::Mage && v.bestWeapon >= 0; inv.ranged = w.Bow(); inv.caster = w.fam == Cls::Mage; inv.sneaks = false;
        inv.restore[kRestoreHealth] = v.Count(Kind::PotionH); inv.restore[kRestoreStamina] = v.Count(Kind::PotionS); inv.restore[kRestoreMagicka] = v.Count(Kind::PotionM);
        for (const Stack& x : v.s) { const ItemDef& d = Items()[static_cast<std::size_t>(x.item)]; if (d.kind == Kind::Buff && d.bk >= 0) inv.buff[static_cast<std::size_t>(d.bk)] += x.count; }
        inv.poisons = v.Count(Kind::Poison); inv.soulGems = v.Count(Kind::SoulGem); inv.weaponChargeFrac = v.charge;
        return inv;
    }
    [[nodiscard]] inline bool IsAlch(Kind k) { return k == Kind::PotionH || k == Kind::PotionM || k == Kind::PotionS || k == Kind::Buff || k == Kind::Food || k == Kind::Poison; }
    inline MFO::Iaus::Consumable ConsOf(const ItemDef& d) {   // logistics/LootNeed.cpp ReadConsumableImpl + ApplyLootFloor, from the fixture record
        MFO::Iaus::Consumable c; using namespace MFO::Iaus; c.value = float(d.value); c.weight = d.weight;
        if (d.kind == Kind::Poison) { c.poison = true; c.hostile = true; c.poisonPower = d.power; return c; }
        if (d.kind == Kind::Food) { c.food = true; c.foodRestoreMag[static_cast<std::size_t>(d.rfam)] = d.power; return c; }   // the loot side's restoreMag stays 0 for food (review F1)
        const int f = d.kind == Kind::PotionH ? kRestoreHealth : d.kind == Kind::PotionS ? kRestoreStamina : d.kind == Kind::PotionM ? kRestoreMagicka : -1;
        if (f >= 0) c.restoreMag[static_cast<std::size_t>(f)] = (d.power > 1.0f && d.power < PotionFloor()) ? 0.0f : d.power;   // ApplyLootFloor
        if (d.kind == Kind::Buff) { PotionFx fx; fx.kind = static_cast<BuffKind>(d.bk); fx.av = d.bk == static_cast<int>(BuffKind::FortifySkill) ? d.av : -1; fx.magnitude = d.power; fx.duration = d.dur; c.fx.push_back(fx); }
        return c;
    }
    struct IausTally { long takes = 0, carryBreaks = 0; std::array<long, static_cast<std::size_t>(Kind::kCount)> byKind{}; };
    inline void IausSource(Inv& v, const Who& w, std::vector<Stack> items, IausTally* t) {
        using namespace MFO::Iaus; const LootTuning& T = Tune().loot;
        auto moveN = [&](std::size_t i, int n) { const ItemDef& d = Items()[static_cast<std::size_t>(items[i].item)]; if (n <= 0 || !Fits(v, d, n)) return false;
            const bool up = IsUpgrade(v, w, d); v.Add(items[i].item, n); if (up) Equip(v, items[i].item); items[i].count -= n;
            if (t) { ++t->takes; t->byKind[static_cast<std::size_t>(d.kind)] += n; } return true; };
        std::vector<std::pair<float, LootKind>> plan; { const LootInv inv = LootInvOf(v, w);
            for (std::size_t i = 0; i < kLootKinds; ++i) { const auto k = static_cast<LootKind>(i); const LootVerdict r = ScoreLootKind(k, inv, T); if (r.take) plan.push_back({ r.score, k }); } }
        std::stable_sort(plan.begin(), plan.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        auto takeAll = [&](auto pred) { for (std::size_t i = 0; i < items.size(); ++i) if (items[i].count > 0 && pred(Items()[static_cast<std::size_t>(items[i].item)])) moveN(i, items[i].count); };
        auto ammoRoad = [&](bool upgradeOnly) {
            std::vector<Stack> body; std::vector<std::size_t> at;
            for (std::size_t i = 0; i < items.size(); ++i) if (items[i].count > 0 && Items()[static_cast<std::size_t>(items[i].item)].kind == Kind::Ammo) { body.push_back(items[i]); at.push_back(i); }
            if (body.empty()) return false; bool moved = false;
            for (const Stack& st : AmmoTakeSet(v, w, body, upgradeOnly)) for (std::size_t k = 0; k < body.size(); ++k)
                if (items[at[k]].count > 0 && body[k].item == st.item && body[k].count == st.count) { moved = moveN(at[k], st.count) || moved; break; }
            return moved;
        };
        for (const auto& [sc, k] : plan) { (void)sc;
            switch (k) {
            case LootKind::Arrows: ammoRoad(false); break;
            case LootKind::Bolts: case LootKind::Lockpicks: case LootKind::Ingredients: case LootKind::kCount: break;   // no crossbow / lockpicks in the fixture; ingredients score 0
            case LootKind::Gold: takeAll([](const ItemDef& d) { return d.kind == Kind::Gold; }); break;
            case LootKind::Jewelry: takeAll([](const ItemDef& d) { return d.kind == Kind::Jewelry; }); break;
            case LootKind::SoulGems: takeAll([](const ItemDef& d) { return d.kind == Kind::SoulGem; }); break;
            case LootKind::Valuables: takeAll([](const ItemDef& d) { return (d.kind == Kind::Gem || d.kind == Kind::Junk) && (d.weight > 0 ? d.value / d.weight >= kValuablesRatio : d.value > 0); }); break;
            case LootKind::Equipment:
                for (int guard = 10; guard > 0; --guard) {
                    auto bestOf = [&](std::initializer_list<Kind> ks) { int b = -1; for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = Items()[static_cast<std::size_t>(items[i].item)];
                            bool in = false; for (Kind kk : ks) in = in || d.kind == kk; if (items[i].count > 0 && in && IsUpgrade(v, w, d) && (b < 0 || d.power > Items()[static_cast<std::size_t>(items[static_cast<std::size_t>(b)].item)].power)) b = static_cast<int>(i); } return b; };
                    int b = bestOf({ Kind::Weapon1H, Kind::Weapon2H }); if (b < 0) b = bestOf({ Kind::Bow }); if (b < 0) b = bestOf({ Kind::ArmorBody, Kind::ArmorHead });
                    if (b >= 0 && moveN(static_cast<std::size_t>(b), items[static_cast<std::size_t>(b)].count)) continue;
                    if (!ammoRoad(true)) break;
                }
                break;
            case LootKind::Potions: {   // LootConsumablesNeed
                std::vector<std::pair<float, std::size_t>> cand; LootInv inv = LootInvOf(v, w);
                for (std::size_t i = 0; i < items.size(); ++i) { const ItemDef& d = Items()[static_cast<std::size_t>(items[i].item)]; if (items[i].count <= 0 || !IsAlch(d.kind)) continue;
                    const LootVerdict r = ScoreConsumable(ConsOf(d), items[i].count, inv, T); if (r.take) cand.push_back({ r.score, i }); }
                std::stable_sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (const auto& [scr, i] : cand) { (void)scr; const LootVerdict r = ScoreConsumable(ConsOf(Items()[static_cast<std::size_t>(items[i].item)]), items[i].count, inv, T);
                    if (!r.take) continue; if (moveN(i, r.takeCount)) inv = LootInvOf(v, w); }
                break; }
            }
        }
    }
    inline double RunIaus(Inv v, const Who& w, const Ep& e, IausTally* t, Tally* end = nullptr) {
        for (int s = 0; s < kStops; ++s) for (const Source& src : e.stops[static_cast<std::size_t>(s)]) IausSource(v, w, src.items, t);
        Town(v, w, false); if (end) end->end = v; return Value(v, w);
    }

    // The oracle: per item TAKE / LEAVE, per stop start SELL RUN / GO ON, each scored by K rollouts of the baseline over futures resampled
    // after the current stop (the current stop is known, as the combat oracle knows the fight it is in). Shared streams across options.
    inline double RunOracle(const Who& w, const Ep& e, std::uint64_t seed, int K, Tally* t) {
        Inv v = Start(w, e);
        auto futures = [&](int s, std::uint64_t salt) { std::vector<Ep> f; for (int k = 0; k < K; ++k) { Rng R; R.g.seed(seed * 7919ULL + salt * 104729ULL + std::uint64_t(k)); Ep x = e;
                for (int q = s + 1; q < kStops; ++q) x.stops[static_cast<std::size_t>(q)] = MakeStop(R); f.push_back(std::move(x)); } return f; };
        for (int s = 0; s < kStops; ++s) {
            const auto F = futures(s, std::uint64_t(s) * 1000 + 1);
            {   // a sell run at this stop's start: he goes to town and back and this stop's loot is not seen
                double go = 0, run = 0; for (const Ep& f : F) { go += RunBaseline(v, w, f, s, 0, 0, nullptr); Inv u = v; Town(u, w, true); run += s + 1 < kStops ? RunBaseline(u, w, f, s + 1, 0, 0, nullptr) : (Town(u, w, false), Value(u, w)); }
                if (run > go + 1e-9) { const double du = (run - go) / K; if (t) { ++t->sellRuns; Div& d = t->div["sell_run|base:go_on|orc:sell_run"]; ++d.n; d.dU += du; } Town(v, w, true); continue; }
            }
            const auto& st = e.stops[static_cast<std::size_t>(s)];
            for (std::size_t a = 0; a < st.size(); ++a) for (std::size_t i = 0; i < st[a].items.size(); ++i) {
                const Stack& x = st[a].items[i]; const ItemDef& d = Items()[static_cast<std::size_t>(x.item)];
                if (!Fits(v, d, x.count)) continue;   // the engine refuses an over-weight stack whatever the policy
                double take = 0, leave = 0; Inv tv = v; TakeStack(tv, w, x, nullptr, false);
                for (const Ep& f : F) { take += RunBaseline(tv, w, f, s, a, i + 1, nullptr); leave += RunBaseline(v, w, f, s, a, i + 1, nullptr); }
                const bool b = BaselineTakes(v, w, st[a].items, i), o = std::fabs(take - leave) < 1e-9 ? b : take > leave;   // a tie keeps today's choice (no divergence without a difference)
                if (t && b != o) { Div& dv = t->div[std::string(Name(d.kind)) + "|base:" + (b ? "take" : "leave") + "|orc:" + (o ? "take" : "leave")]; ++dv.n; dv.dU += std::fabs(take - leave) / K; }
                if (o) TakeStack(v, w, x, t, false);
            }
        }
        Town(v, w, false); if (t) t->end = v; return Value(v, w);
    }

    // ── the run: --logistics N (episodes per seed and class) ─────────────────────────────────────────────────────────────────
    inline std::map<std::string, std::array<double, MFO::Iaus::kBuffKinds + 3>> g_uses;   // --logi-uses: class -> uses per fight (CONSUSE, the driver's prep)
    inline bool LoadUses(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "r"); if (!f) return false; char name[64];
        while (std::fscanf(f, "%63s", name) == 1) { std::array<double, MFO::Iaus::kBuffKinds + 3> u{}; for (double& x : u) if (std::fscanf(f, "%lf", &x) != 1) { std::fclose(f); return false; } g_uses[name] = u; }
        std::fclose(f); return true;
    }
    // policy: "baseline" (today's rules vs the oracle) or "iaus" (also the IAUS arm). score: the IAUS arm only, its mean J per seed as SEED lines
    // (the tune driver's loot stage reads them like a combat run's), one class per run.
    inline int Run(const std::vector<SimClass>& all, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int K, bool checks,
                   const std::string& policy = "baseline", bool score = false) {
        long bad = 0; const bool iaus = policy == "iaus";
        if (score) {
            for (const SimClass& sc : all) {
                if (only.empty() || ("," + only + ",").find("," + sc.name + ",") == std::string::npos) continue;
                Who w; w.cls = sc.name; w.fam = sc.fam; w.worth = PotWorthFor(sc.name);
                if (!w.worth) { std::fprintf(stderr, "logistics: no potion-worth row for %s: refused\n", sc.name.c_str()); return 2; }
                if (g_logiConsumables) { auto it = g_uses.find(sc.name); if (it == g_uses.end()) { std::fprintf(stderr, "--logi-consumables: no --logi-uses row for %s: refused\n", sc.name.c_str()); return 2; } w.uses = &it->second; }
                for (std::uint64_t sd : seeds) { double a = 0; for (int k = 0; k < n; ++k) { Rng R; R.g.seed(sd * 2654435761ULL + std::uint64_t(k) * 40503ULL + 17); const Ep e = MakeEpisode(R); a += RunIaus(Start(w, e), w, e, nullptr); }
                    std::printf("SEED %llu  J=%.4f (scenarios 0.00, natural 0.00)\nSWITCH %llu 0 1.000\n", (unsigned long long)sd, a / n, (unsigned long long)sd); }
                return 0;
            }
            std::fprintf(stderr, "--logi-score: one class (--classes) is required\n"); return 2;
        }
        {   // the model's own checks (LOGI-CHECKS)
            const Quote q = QuoteFor(kSpeech); const ItemDef& rh = Items()[static_cast<std::size_t>(ItemIdx("RestoreHealth01"))];
            const bool pin = BuyUnit(rh, q) == 38 && SellUnit(rh, q) == 8 && std::fabs(q.buy - 2.25f) < 1e-4f;   // 17 x 2.25 = 38.25 -> 38; 17 / 2.25 = 7.56 -> 8
            bool sellUnderBuy = true; for (const ItemDef& d : Items()) if (d.kind != Kind::Gold) sellUnderBuy = sellUnderBuy && SellUnit(d, q) <= BuyUnit(d, q);
            const bool floorOk = std::fabs(PotionFloor() - 50.0f) < 1e-4f;   // magnitudes 25 / 50 / 75: the weakest tier (25) is ignored
            if (checks) std::printf("LOGI-CHECK prices pinned (RestoreHealth01 buy 38 sell 8 at Speech 15): %s\nLOGI-CHECK every sell unit <= its buy unit: %s\nLOGI-CHECK potion floor = 50: %s\n", pin ? "PASS" : "FAIL", sellUnderBuy ? "PASS" : "FAIL", floorOk ? "PASS" : "FAIL");
            bad += !pin + !sellUnderBuy + !floorOk;
            // the review round's rules (L1 no bow gate, L2 per-source drain, L3 the ammo sale), pinned on hand-made inventories
            Who mel; mel.fam = Cls::Melee; Who bow; bow.fam = Cls::Ranged;
            Inv m = StartKit(mel); const std::vector<Stack> body = { { ItemIdx("SteelArrow"), 7 }, { ItemIdx("RestoreHealth02"), 1 }, { ItemIdx("RestoreHealth03"), 1 } };
            BaselineSource(m, mel, body, 0, nullptr);   // he holds 1 health potion and no arrow: both rows fire, each drains its kind
            const bool l1 = m.Count(Kind::Ammo) == 7, l2 = m.Count(Kind::PotionH) == 3;
            Inv o = StartKit(mel); o.Add(ItemIdx("IronArrow"), 12); o.Add(ItemIdx("SteelArrow"), 10);   // off-kind: keeps the best 10, sells the 12 iron
            std::vector<Stack> so = SellAmmo(o, mel); const bool l3a = so.size() == 1 && so[0].item == ItemIdx("IronArrow") && so[0].count == 12;
            Inv b = StartKit(bow); b.Add(ItemIdx("OrcishArrow"), 50);   // a user at keep target 50: the 30 iron rounds under the cut sell
            std::vector<Stack> sb = SellAmmo(b, bow); const bool l3b = sb.size() == 1 && sb[0].item == ItemIdx("IronArrow") && sb[0].count == 30;
            if (checks) std::printf("LOGI-CHECK no bow gate: a melee follower restocks arrows (7 of 7): %s\nLOGI-CHECK per-source drain: 1 potion held, a body with 2 gives 2: %s\nLOGI-CHECK ammo sale: off-kind below the row's 10 (12 iron), a user's below the cut at 50 (30 iron): %s\n",
                                    l1 ? "PASS" : "FAIL", l2 ? "PASS" : "FAIL", l3a && l3b ? "PASS" : "FAIL");
            bad += !l1 + !l2 + !(l3a && l3b);
        }
        {   // tune 2.11.1: the IAUS arm's rules on hand-made inventories (native's scorer, the table at --set loot.*)
            Who mel; mel.fam = Cls::Melee; Who bow; bow.fam = Cls::Ranged; const bool was = g_logiConsumables;
            Inv m = StartKit(mel); IausSource(m, mel, { { ItemIdx("SteelArrow"), 7 } }, nullptr); const bool a1 = m.Count(Kind::Ammo) == 0;   // ammo by class + launcher: a melee follower leaves arrows
            Inv b = StartKit(bow); for (Stack& x : b.s) if (x.item == ItemIdx("IronArrow")) x.count = 5; IausSource(b, bow, { { ItemIdx("SteelArrow"), 7 }, { ItemIdx("SteelBolt"), 5 } }, nullptr); const bool a2 = b.Count(Kind::Ammo) == 12 && b.Count(Kind::Bolt) == 0;   // a bow user under target takes arrows, never bolts
            Inv g1 = StartKit(mel); g1.charge = 0.2f; IausSource(g1, mel, { { ItemIdx("SoulGemPetty (MODEL)"), 1 } }, nullptr);
            Inv g2 = StartKit(mel); g2.charge = 0.9f; IausSource(g2, mel, { { ItemIdx("SoulGemPetty (MODEL)"), 1 } }, nullptr);
            const bool a3 = g1.Count(Kind::SoulGem) == (Tune().loot.gemChargeTrigger > 0.2f && Tune().loot.soulGemTarget > 0 ? 1 : 0) && g2.Count(Kind::SoulGem) == (Tune().loot.gemChargeTrigger > 0.9f && Tune().loot.soulGemTarget > 0 ? 1 : 0);
            Inv p1 = StartKit(mel); Who mag; mag.fam = Cls::Mage; Inv p2 = StartKit(mag); IausSource(p1, mel, { { ItemIdx("DamageHealthPoison (MODEL)"), 1 } }, nullptr); IausSource(p2, mag, { { ItemIdx("DamageHealthPoison (MODEL)"), 1 } }, nullptr);
            const bool a4 = p2.Count(Kind::Poison) == 0;   // a poison is a weapon user's only; a mage leaves it (no ALCH sell road)
            const bool a5 = std::fabs(GoldJ(Who{ "melee", Cls::Melee, PotWorthFor("melee") }) - GoldJOf(PotWorthFor("melee"))) < 1e-12;   // the combat world charges at the same J per gold
            g_logiConsumables = was; (void)p1;
            if (checks) std::printf("LOGI-CHECK IAUS ammo by class + launcher (melee leaves arrows; a bow user takes arrows, never bolts): %s\nLOGI-CHECK IAUS soul gems only below the charge trigger: %s\nLOGI-CHECK IAUS poison only for a weapon user: %s\nLOGI-CHECK combat J per gold = logistics J per gold: %s\n",
                                    a1 && a2 ? "PASS" : "FAIL", a3 ? "PASS" : "FAIL", a4 ? "PASS" : "FAIL", a5 ? "PASS" : "FAIL");
            if (iaus) bad += !(a1 && a2) + !a3 + !a4 + !a5;
        }
        std::printf("LOGISTICS WORLD (built-in fixture, %d stops, K=%d): J_log = potion worth of the bag + %s x gold-equivalent (Logistics.h VALUE)\n", kStops, K, "kGoldJ");
        for (const SimClass& sc : all) {
            if (!only.empty() && ("," + only + ",").find("," + sc.name + ",") == std::string::npos) continue;
            Who w; w.cls = sc.name; w.fam = sc.fam; w.worth = PotWorthFor(sc.name);
            if (!w.worth) { std::fprintf(stderr, "logistics: no potion-worth row for %s (PotionWorth.h / --potion-worth): refused\n", sc.name.c_str()); return 2; }
            if (g_logiConsumables) { auto it = g_uses.find(sc.name); if (it == g_uses.end()) { std::fprintf(stderr, "--logi-consumables: no --logi-uses row for %s: refused\n", sc.name.c_str()); return 2; } w.uses = &it->second; }
            Tally tb, to; std::vector<double> d, dp, dg, di, dio; IausTally ti;
            for (std::uint64_t sd : seeds) for (int k = 0; k < n; ++k) {
                Rng R; R.g.seed(sd * 2654435761ULL + std::uint64_t(k) * 40503ULL + 17); const Ep e = MakeEpisode(R);
                const double jb = RunBaseline(Start(w, e), w, e, 0, 0, 0, &tb), jo = RunOracle(w, e, sd * 1000003ULL + std::uint64_t(k), K, &to);
                const double jb2 = RunBaseline(Start(w, e), w, e, 0, 0, 0, nullptr); if (jb2 != jb) ++bad;   // deterministic
                if (iaus) { const double ji = RunIaus(Start(w, e), w, e, &ti); di.push_back(ji - jb); dio.push_back(jo - ji); if (RunIaus(Start(w, e), w, e, nullptr) != ji) ++bad; std::printf("LOGIIAUSEP %s %llu %d %.4f\n", sc.name.c_str(), (unsigned long long)sd, k, ji); }
                d.push_back(jo - jb); dp.push_back(PotionJ(to.end, w) - PotionJ(tb.end, w)); dg.push_back(GoldJ(w) > 0 ? (jo - jb - dp.back()) / GoldJ(w) : 0.0);
                std::printf("LOGI %s %llu %d %.4f %.4f\n", sc.name.c_str(), (unsigned long long)sd, k, jb, jo);
            }
            bad += tb.carryBreaks + to.carryBreaks + tb.offRowTakes;
            double m = 0; for (double x : d) m += x; m /= std::max<std::size_t>(1, d.size()); double var = 0; for (double x : d) var += (x - m) * (x - m);
            const double se = d.size() > 1 ? std::sqrt(var / double(d.size() - 1) / double(d.size())) : 0.0;
            auto mean = [](const std::vector<double>& x) { double a = 0; for (double y : x) a += y; return x.empty() ? 0.0 : a / double(x.size()); };
            std::printf("LOGIGAP %s %.4f %.4f %zu goldJ=%.5f potionJ_gap=%.4f gold_equivalent_gap=%.1f baseline_takes=%ld oracle_takes=%ld sell_runs=%ld\n", sc.name.c_str(), m, se, d.size(), GoldJ(w), mean(dp), mean(dg), tb.takes, to.takes, to.sellRuns);
            for (const auto& [k, v] : to.div) std::printf("LOGIDIS %s %s %ld %.4f\n", sc.name.c_str(), k.c_str(), v.n, v.dU);
            if (iaus) { auto ms = [](const std::vector<double>& x) { double a = 0; for (double y : x) a += y; a /= std::max<std::size_t>(1, x.size()); double q = 0; for (double y : x) q += (y - a) * (y - a);
                    return std::pair<double, double>{ a, x.size() > 1 ? std::sqrt(q / double(x.size() - 1) / double(x.size())) : 0.0 }; };
                const auto [a1, s1] = ms(di); const auto [a2, s2] = ms(dio); bad += ti.carryBreaks;
                std::printf("LOGIIAUS %s iaus-baseline %.4f %.4f oracle-iaus %.4f %.4f takes=%ld", sc.name.c_str(), a1, s1, a2, s2, ti.takes);
                for (std::size_t q = 0; q < ti.byKind.size(); ++q) if (ti.byKind[q]) std::printf(" %s=%ld", Name(static_cast<Kind>(q)), ti.byKind[q]); std::printf("\n"); }
        }
        std::printf("LOGI-CHECKS: carry never exceeded, the baseline takes only what its rows name, deterministic, prices and floor pinned  => %s\n", bad == 0 ? "PASS" : "FAIL");
        return bad == 0 ? 0 : 1;
    }
}
