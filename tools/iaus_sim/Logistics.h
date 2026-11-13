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
//              the 4 default logistics rows (Followers.cpp:48-51): loot_potions while < 2 restore-health potions, loot_arrows while < 10
//              arrows, loot_equipment always (strict upgrades only, LootEquipment.cpp); no other category (no default row takes gold,
//              gems, ingredients or junk). Carry check per WHOLE stack, no reserve (LootTake.cpp:40-46). Potion floor: the weakest tier is
//              ignored (Upkeep.cpp ComputeWeakPotionFloor: first magnitude > 1.25 x the minimum). Town: an errand runs when its sell rows
//              pay >= 100 g or carry > 80% (ErrandTrip.cpp, iErrandMinSellGold); sells unkept gear, MISC with value/weight >= 20 and
//              obsolete ammo, highest value first, capped by the chest's gold (MFO_Trade.psc RunTrade); buys the rows' needs, potions
//              capped at half the purse (TradeBridge.cpp PlanBuy). Prices: Pricing.cpp, Speech only (no perks in the fixture).
//   Oracle   = a rollout search over TAKE / LEAVE per item and a SELL RUN at a stop's start (a trip to town and back that costs the stop):
//              K sampled futures shared by both options, rolled forward with the baseline, scored by Value(). Any lootable item may be
//              taken (the decision surface an IAUS scorer will have); stolen goods do not exist in the fixture (unowned dungeon).
//   IAUS     = not built (the post-T2 loot scorer). --logistics refuses `--logi-policy iaus` until it exists.
// VALUE (MODEL, every term an open decision for marth, 34.6): J of the potions in the bag at the episode's end, from the COMBAT world's
// potion worth for his class (PotionWorth.h: the k-th potion's worth over a chain of fights) + kGoldJ x gold-equivalent, where
// gold-equivalent = purse + the BUY price of his worn gear and of his arrows up to the keep target (what it would cost to have them),
// and kGoldJ = J per gold where gold buys the most J: max over potion kinds of worth(first potion) / buy price of that kind's cheapest
// restore potion. Unsold junk at the end is worth 0. Everything the fixture item table says is a Skyrim.esm record value read from the
// Vanilla extract (value, damage, armor rating, magnitude) unless it is marked MODEL.
#include "iaus_sim/Setup.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace Sim::Logi {

    enum class Kind : std::uint8_t { Gold, PotionH, PotionM, PotionS, Ammo, Weapon1H, Weapon2H, Bow, ArmorBody, ArmorHead, Gem, Jewelry, Ingredient, Junk, kCount };
    inline const char* Name(Kind k) {
        static const char* n[] = { "gold", "potion_h", "potion_m", "potion_s", "ammo", "weapon_1h", "weapon_2h", "bow", "armor_body", "armor_head", "gem", "jewelry", "ingredient", "junk" };
        return n[static_cast<std::size_t>(k)];
    }
    struct ItemDef { const char* edid; Kind kind; int value; float weight; float power; bool weightModel; };   // power: weapon damage, armor rating, potion magnitude, arrow damage
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
        int bestWeapon = -1, bestBow = -1, body = -1, head = -1;   // worn / in use (the equip authority equips an upgrade it takes)
        [[nodiscard]] float Weight() const { float w = 0; for (const Stack& x : s) w += Items()[static_cast<std::size_t>(x.item)].weight * float(x.count); return w; }
        [[nodiscard]] int Count(Kind k) const { int n = 0; for (const Stack& x : s) if (Items()[static_cast<std::size_t>(x.item)].kind == k) n += x.count; return n; }
        [[nodiscard]] float WorstAmmo() const { float w = 1e9f; for (const Stack& x : s) if (Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo && x.count > 0) w = std::min(w, Items()[static_cast<std::size_t>(x.item)].power); return w; }
        void Add(int item, int n) { if (Items()[static_cast<std::size_t>(item)].kind == Kind::Gold) { purse += n; return; } for (Stack& x : s) if (x.item == item) { x.count += n; return; } s.push_back({ item, n }); }
    };
    struct Who { std::string cls; Cls fam = Cls::Melee; const PotWorth* worth = nullptr; [[nodiscard]] bool Bow() const { return fam == Cls::Ranged; } };

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
            out.push_back(std::move(s)); }
        Source c; c.items.push_back({ ItemIdx("Gold001"), R.I(20, 80) });
        for (int k = R.I(1, 2); k > 0; --k) c.items.push_back({ pick(bp, 7), 1 });
        if (R.P(0.3f)) c.items.push_back({ ItemIdx("GemGarnet (MODEL)"), 1 });
        if (R.P(0.2f)) c.items.push_back({ ItemIdx("SilverRing (MODEL)"), 1 });
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
    // Does today's rule set take this stack now? (the rows are evaluated at the moment of the take, LootScan.cpp:1040-1110)
    [[nodiscard]] inline bool BaselineTakes(const Inv& v, const Who& w, const Stack& st) {
        const ItemDef& d = Items()[static_cast<std::size_t>(st.item)];
        if (!Fits(v, d, st.count)) return false;
        switch (d.kind) {
        case Kind::PotionH: case Kind::PotionM: case Kind::PotionS: return v.Count(Kind::PotionH) < kRowPotions && d.power >= PotionFloor();   // loot_potions is the catch-all drinkable take
        case Kind::Ammo: return w.Bow() && v.Count(Kind::Ammo) < kRowArrows && d.power >= (v.Count(Kind::Ammo) > 0 ? v.WorstAmmo() : 0.0f);   // restock: not worse than his worst held rank
        case Kind::Weapon1H: case Kind::Weapon2H: case Kind::Bow: case Kind::ArmorBody: case Kind::ArmorHead: return IsUpgrade(v, w, d);
        default: return false;   // gold, gems, jewelry, ingredients, junk: no default row takes them
        }
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
    inline Errand Town(Inv& v, const Who& w, bool force) {   // the errand: TryStart's trigger, then BUY first, then SELL (RunTrade)
        const Quote q = QuoteFor(kSpeech); Errand e; int sellTotal = 0;
        std::vector<std::size_t> rows; for (std::size_t i = 0; i < v.s.size(); ++i) { const ItemDef& d = Items()[static_cast<std::size_t>(v.s[i].item)]; if (v.s[i].count > 0 && (Sellable(v, v.s[i]) || SellableGem(d))) { rows.push_back(i); sellTotal += SellUnit(d, q) * v.s[i].count; } }
        const bool needs = v.Count(Kind::PotionH) < kRowPotions || (w.Bow() && v.Count(Kind::Ammo) < kRowArrows);
        if (!force && !(std::min(sellTotal, kVendorGold) >= kMinSellGold || v.Weight() > kCarryRatioErrand * kCarryCap || needs)) return e;
        e.ran = true; int chest = kVendorGold;
        {   // BUY: the rows' needs, potions capped at half the purse (PlanBuy), the dearest supply line first
            int potLeft = v.purse / 2;
            for (const char* id : { "RestoreHealth02", "RestoreHealth03", "RestoreHealth01" }) { const int it = ItemIdx(id); const int u = BuyUnit(Items()[static_cast<std::size_t>(it)], q);   // the dearest line first
                while (v.Count(Kind::PotionH) < kRowPotions && u <= potLeft && u <= v.purse) { v.Add(it, 1); v.purse -= u; potLeft -= u; chest += u; e.bought += u; } }
            if (w.Bow()) { const int it = ItemIdx("IronArrow"); const int u = BuyUnit(Items()[static_cast<std::size_t>(it)], q);
                while (v.Count(Kind::Ammo) < kRowArrows && u <= v.purse) { v.Add(it, 1); v.purse -= u; chest += u; e.bought += u; } }
        }
        std::sort(rows.begin(), rows.end(), [&](std::size_t a, std::size_t b) { return Items()[static_cast<std::size_t>(v.s[a].item)].value > Items()[static_cast<std::size_t>(v.s[b].item)].value; });
        for (std::size_t i : rows) { Stack& x = v.s[i]; const int u = SellUnit(Items()[static_cast<std::size_t>(x.item)], q); if (u <= 0 || u > chest) continue;
            const int n = std::min(x.count, chest / u); x.count -= n; v.purse += u * n; chest -= u * n; e.sold += u * n; }
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
        const Quote q = QuoteFor(kSpeech); double j = 0;
        if (w.worth) { const int n[3] = { v.Count(Kind::PotionH), v.Count(Kind::PotionM), v.Count(Kind::PotionS) };
            for (int t = 0; t < 3; ++t) for (int k = 0; k < std::min(n[t], kPotWorthMax); ++k) j += w.worth->m[static_cast<std::size_t>(t)][static_cast<std::size_t>(k)]; }
        double g = v.purse; for (int it : { v.bestWeapon, v.bestBow, v.body, v.head }) if (it >= 0) g += BuyUnit(Items()[static_cast<std::size_t>(it)], q);
        if (w.Bow()) { int left = kAmmoKeepFloor; std::vector<std::pair<float, int>> am; for (const Stack& x : v.s) if (Items()[static_cast<std::size_t>(x.item)].kind == Kind::Ammo) am.push_back({ Items()[static_cast<std::size_t>(x.item)].power, x.item });
            std::sort(am.rbegin(), am.rend()); for (auto& [pw, it] : am) { (void)pw; for (const Stack& x : v.s) if (x.item == it) { const int n = std::min(left, x.count); g += n * BuyUnit(Items()[static_cast<std::size_t>(it)], q); left -= n; } } }
        return j + GoldJ(w) * g;
    }

    // ── an episode under a policy ────────────────────────────────────────────────────────────────────────────────────────────
    struct Div { long n = 0; double dU = 0; };
    struct Tally { long takes = 0, carryBreaks = 0, offRowTakes = 0, sellRuns = 0; std::map<std::string, Div> div; Inv end; };
    struct Ep { std::vector<std::vector<Source>> stops; };
    inline Ep MakeEpisode(Rng& R) { Ep e; for (int s = 0; s < kStops; ++s) e.stops.push_back(MakeStop(R)); return e; }

    inline void TakeStack(Inv& v, const Who& w, const Stack& st, Tally* t, bool baseline) {
        const ItemDef& d = Items()[static_cast<std::size_t>(st.item)];
        if (t) { ++t->takes; if (!Fits(v, d, st.count)) ++t->carryBreaks; if (baseline && !BaselineTakes(v, w, st)) ++t->offRowTakes; }
        const bool up = IsUpgrade(v, w, d); v.Add(st.item, st.count); if (up) Equip(v, st.item);
    }
    // The baseline over stops [s0, kStops) of an episode, from item i0 of stop s0; then the town. Returns Value().
    inline double RunBaseline(Inv v, const Who& w, const Ep& e, int s0, std::size_t src0, std::size_t i0, Tally* t) {
        for (int s = s0; s < kStops; ++s) { const auto& st = e.stops[static_cast<std::size_t>(s)];
            for (std::size_t a = (s == s0 ? src0 : 0); a < st.size(); ++a) for (std::size_t i = (s == s0 && a == src0 ? i0 : 0); i < st[a].items.size(); ++i)
                if (BaselineTakes(v, w, st[a].items[i])) TakeStack(v, w, st[a].items[i], t, true); }
        Town(v, w, false); if (t) t->end = v; return Value(v, w);
    }
    // The oracle: per item TAKE / LEAVE, per stop start SELL RUN / GO ON, each scored by K rollouts of the baseline over futures resampled
    // after the current stop (the current stop is known, as the combat oracle knows the fight it is in). Shared streams across options.
    inline double RunOracle(const Who& w, const Ep& e, std::uint64_t seed, int K, Tally* t) {
        Inv v = StartKit(w);
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
                const bool b = BaselineTakes(v, w, x), o = std::fabs(take - leave) < 1e-9 ? b : take > leave;   // a tie keeps today's choice (no divergence without a difference)
                if (t && b != o) { Div& dv = t->div[std::string(Name(d.kind)) + "|base:" + (b ? "take" : "leave") + "|orc:" + (o ? "take" : "leave")]; ++dv.n; dv.dU += std::fabs(take - leave) / K; }
                if (o) TakeStack(v, w, x, t, false);
            }
        }
        Town(v, w, false); if (t) t->end = v; return Value(v, w);
    }

    // ── the run: --logistics N (episodes per seed and class) ─────────────────────────────────────────────────────────────────
    inline int Run(const std::vector<SimClass>& all, const std::string& only, const std::vector<std::uint64_t>& seeds, int n, int K, bool checks) {
        long bad = 0;
        {   // the model's own checks (LOGI-CHECKS)
            const Quote q = QuoteFor(kSpeech); const ItemDef& rh = Items()[static_cast<std::size_t>(ItemIdx("RestoreHealth01"))];
            const bool pin = BuyUnit(rh, q) == 38 && SellUnit(rh, q) == 8 && std::fabs(q.buy - 2.25f) < 1e-4f;   // 17 x 2.25 = 38.25 -> 38; 17 / 2.25 = 7.56 -> 8
            bool sellUnderBuy = true; for (const ItemDef& d : Items()) if (d.kind != Kind::Gold) sellUnderBuy = sellUnderBuy && SellUnit(d, q) <= BuyUnit(d, q);
            const bool floorOk = std::fabs(PotionFloor() - 50.0f) < 1e-4f;   // magnitudes 25 / 50 / 75: the weakest tier (25) is ignored
            if (checks) std::printf("LOGI-CHECK prices pinned (RestoreHealth01 buy 38 sell 8 at Speech 15): %s\nLOGI-CHECK every sell unit <= its buy unit: %s\nLOGI-CHECK potion floor = 50: %s\n", pin ? "PASS" : "FAIL", sellUnderBuy ? "PASS" : "FAIL", floorOk ? "PASS" : "FAIL");
            bad += !pin + !sellUnderBuy + !floorOk;
        }
        std::printf("LOGISTICS WORLD (built-in fixture, %d stops, K=%d): J_log = potion worth of the bag + %s x gold-equivalent (Logistics.h VALUE)\n", kStops, K, "kGoldJ");
        for (const SimClass& sc : all) {
            if (!only.empty() && ("," + only + ",").find("," + sc.name + ",") == std::string::npos) continue;
            Who w; w.cls = sc.name; w.fam = sc.fam; w.worth = PotWorthFor(sc.name);
            if (!w.worth) { std::fprintf(stderr, "logistics: no potion-worth row for %s (PotionWorth.h / --potion-worth): refused\n", sc.name.c_str()); return 2; }
            Tally tb, to; std::vector<double> d, dp, dg;
            for (std::uint64_t sd : seeds) for (int k = 0; k < n; ++k) {
                Rng R; R.g.seed(sd * 2654435761ULL + std::uint64_t(k) * 40503ULL + 17); const Ep e = MakeEpisode(R);
                const double jb = RunBaseline(StartKit(w), w, e, 0, 0, 0, &tb), jo = RunOracle(w, e, sd * 1000003ULL + std::uint64_t(k), K, &to);
                const double jb2 = RunBaseline(StartKit(w), w, e, 0, 0, 0, nullptr); if (jb2 != jb) ++bad;   // deterministic
                d.push_back(jo - jb); dp.push_back(PotionJ(to.end, w) - PotionJ(tb.end, w)); dg.push_back(GoldJ(w) > 0 ? (jo - jb - dp.back()) / GoldJ(w) : 0.0);
                std::printf("LOGI %s %llu %d %.4f %.4f\n", sc.name.c_str(), (unsigned long long)sd, k, jb, jo);
            }
            bad += tb.carryBreaks + to.carryBreaks + tb.offRowTakes;
            double m = 0; for (double x : d) m += x; m /= std::max<std::size_t>(1, d.size()); double var = 0; for (double x : d) var += (x - m) * (x - m);
            const double se = d.size() > 1 ? std::sqrt(var / double(d.size() - 1) / double(d.size())) : 0.0;
            auto mean = [](const std::vector<double>& x) { double a = 0; for (double y : x) a += y; return x.empty() ? 0.0 : a / double(x.size()); };
            std::printf("LOGIGAP %s %.4f %.4f %zu goldJ=%.5f potionJ_gap=%.4f gold_equivalent_gap=%.1f baseline_takes=%ld oracle_takes=%ld sell_runs=%ld\n", sc.name.c_str(), m, se, d.size(), GoldJ(w), mean(dp), mean(dg), tb.takes, to.takes, to.sellRuns);
            for (const auto& [k, v] : to.div) std::printf("LOGIDIS %s %s %ld %.4f\n", sc.name.c_str(), k.c_str(), v.n, v.dU);
        }
        std::printf("LOGI-CHECKS: carry never exceeded, the baseline takes only what its rows name, deterministic, prices and floor pinned  => %s\n", bad == 0 ? "PASS" : "FAIL");
        return bad == 0 ? 0 : 1;
    }
}
