#pragma once
#include "PCH.h"
#include "logistics/Pricing.h"   // town phase 2 R2: the dispatch quote rides the order

// #21 ECON BRIDGE -- native side (Fable's ECON_PAPYRUS_PLAN).
//
// The merchant read + transaction cannot run in C++: native GetInventory /
// GetGoldAmount CTD on a merchant chest / a follower whose InventoryChanges the
// worker tick is mutating (§0.37, memory getgoldamount-ctds-count-gold-from-
// getinventory). So the merchant work moves to Papyrus (MFO_Trade.psc) -- the
// path the game's own barter menu uses -- and native keeps the DECISION.
//
// FLOW: native resolves the vendor + builds the SELL list (follower's own
// inventory) and the BUY NEEDS (which supply gambits are below N, by how much),
// stores them token'd, and dispatches RunTrade at MFO_TradeQuest. MFO_Trade reads
// the chest's barter gold, runs the SELL loop, ENUMERATES the vendor's stock
// (po3 AddAllItemsToArray -- native can't guess which of hundreds of arrow/potion
// types a vendor carries), hands it to native PlanBuy (which classifies + ranks
// the ACTUAL stock, best-affordable-first, capped at quota and purse), executes
// the buys, and reports. bEconomy OFF = a dry run (enumerate + plan, no mutation)
// -- so the enumeration read is exercised safely before any purchase.
namespace MFO::TradeBridge {

    // One sellable line -- the follower's own, un-worn, un-excluded, vendor-
    // tradeable gear. Native pre-sorts highest-value-first.
    struct SellRow {
        RE::TESBoundObject* obj     = nullptr;
        std::int32_t        count   = 0;
        std::int32_t        value   = 0;    // per-unit gold value
        bool                jewelry = false;
    };

    // One buy need -- a supply category the follower is below threshold on. quota
    // is how many MORE to acquire. PlanBuy matches enumerated stock to these.
    struct NeedCat {
        // APPEND-ONLY (parallel classifier value, not serialized -- but keep the
        // order stable so PlanBuy's switch never re-maps). kPotHealth..kBolts are
        // the SUPPLY kinds filled by quota; kWeaponMelee..kSpellTome are the GEAR/
        // TOME kinds ClassifyBuy tags for the single-best-upgrade passes (they do
        // NOT ride the quota mechanism -- see PlanBuy).
        enum Kind : std::int32_t {
            kPotHealth = 0, kPotStamina, kPotMagicka, kArrows, kBolts,
            kWeaponMelee, kWeaponRanged, kArmor, kMageApparel, kSpellTome,
            kWeaponSilver,  // APPENDED (86e3p2gp9): the silver slot's restock, single best pick
            kAmmoSilver,    // APPENDED (86e3p2gp9, marth 2026-10-12): a ranged follower's silver ammo restock
            kFood,          // APPENDED (TOWN PHASE 2 R7): the inn's food stock -- quota-filled in PlanBuy's own FOOD pass, never by the supply loop
            kSmithMaterial  // APPENDED (town phase 2 R8): a smithing/tempering material on BuyThresholds::smithMats (NOT a quota kind)
        };
        std::int32_t kind  = 0;
        std::int32_t quota = 0;
    };

    // One smithing material to acquire (town phase 2 R8): the base form and how many MORE he needs.
    struct SmithMat {
        RE::FormID    form = 0;
        std::int32_t  qty  = 0;
    };

    // GEAR + TOME buy thresholds -- all computed on the WORKER (Logistics reuses
    // its own loot judge), passed opaque to native so PlanBuy stays a pure
    // comparator. Every field is derived from the follower's OWN inventory/skills/
    // gambits before dispatch; native never re-derives them. Zero-value default =
    // "feature off" (no upgrade can be worse than an empty baseline that also
    // has its enable flag false).
    struct BuyThresholds {
        // -- weapon/armor (Feature A), gated by buyGear --
        bool          buyGear      = false;
        std::int32_t  meleeClass   = 3;      // Logistics WepClass int: 0=1H 1=2H 2=Ranged 3=Other(no melee buy)
        // Best in-class melee weapon the follower already owns, as the loot
        // judge's WeaponScore (attack damage x the perk-style bias -- equals the
        // plain damage when preferKinds == 0). PlanBuy scores every melee
        // candidate through Logistics::WeaponBuyScore(w, preferKinds) so buy
        // and loot rank a greatsword vs a warhammer identically.
        float         meleeBaseScore = 0.0f;
        std::int32_t  preferKinds    = 0;    // Progression::WeaponKind bits the perks prefer (0 = none)
        bool          doRanged     = false;
        bool          wantCrossbow = false;  // meaningful only when doRanged
        std::int32_t  rangedBaseDmg= 0;      // best owned bow/crossbow dmg
        bool          buyArmor     = false;  // plain rated armor (NON-mage, not dolls mode)
        // Best owned armor rating PER LOGICAL SLOT (0 head 1 body 2 hands 3 feet
        // 4 shield -- Logistics::ArmorBuySlot order). Per-slot so a warrior with a
        // good chestpiece can still buy a helmet/boots for bare slots (the loot
        // judge is per-slot; buy must match). PlanBuy best-picks per slot.
        std::int32_t  armorBaseRat[5] = {};
        bool          buyMageApparel = false;// clothing/jewelry dress-up (caster + bMageWearRobes, not dolls mode)
        // "Wear helmets" OFF for this follower (ClickUp 86e3phy41; Logistics::WearsHelmets false):
        // PlanBuy skips every head piece (Logistics::IsHelmetPiece) in BOTH the rated-armor and the
        // mage-apparel pass. Town errand buys plan through the same PlanBuy, so they are covered too.
        bool          noHelmets = false;
        // Ranking (marth 2026-10-01): value-driven by default, with or without MEO.
        // schoolPrimary == true ONLY with bMageApparelStrictSchool: ranks by
        // school-enchant tier first; false ranks purely by gold value (most
        // expensive piece per slot). Computed on the worker.
        bool          mageSchoolPrimary = false;
        // Owned baseline the buy must beat, per logical slot (0=head 1=body 2=hands
        // 3=feet 4=ring 5=amulet -- MageClothingSlot order), ranked by (tier,metric):
        // tier 2=top-2-school enchant, 1=plain, 0=off-school enchant; metric = value
        // (+ matching fortify magnitude for tier 2).
        std::int32_t  mageBaseTier[6]   = {};
        std::int32_t  mageBaseMetric[6] = {};
        bool          isNecromancer= false;  // follower has a Reanimate cast gambit -> may buy villain-coded regalia
        // -- spell tomes (Feature B), gated by buyTomes; also the strict-apparel top-2 set --
        bool          buyTomes     = false;  // g_economyBuyTomes && follower is a mage w/ a cast gambit
        std::uint8_t  eligibleSchools = 0;   // bitmask over Logistics school-bit order (top-2 skill schools)
        // -- APPENDED 2026-09-14 (armor class by skill + perks; append-only, never
        //    reorder or remove a field above) --
        // The follower's armor multipliers (Logistics::ArmorPref: kArmorClassBias
        // on the class of his higher BASE armor skill, x kArmorPerkBias on the
        // class his perk ranks vote for). PlanBuy ranks every rated candidate by
        // ArmorScoreOf(rating, type, heavyBias, lightBias) -- the loot judge's
        // own ArmorScore, so buy and loot order a light-vs-heavy piece
        // identically. 1.0/1.0 = no preference (feature off / no skills read).
        float         armorHeavyBias = 1.0f;
        float         armorLightBias = 1.0f;
        // Best OWNED ArmorScore PER LOGICAL SLOT (ArmorBuySlot order, like
        // armorBaseRat), the baseline a buy must beat. A float because the score
        // is one (a x1.25 perk step lands on .5s): truncating it into the int32
        // armorBaseRat would make a candidate tie its own baseline and be re-
        // bought every visit. armorBaseRat above still carries the raw best owned
        // RATING per slot (diagnostic; PlanBuy no longer ranks by it).
        float         armorBaseScore[5] = {};
        // -- APPENDED 2026-09-14 (second one-hander for dual wield by perks;
        //    append-only) --
        // wantOffHand: the follower's perks vote dual wield (WeaponRoles::offHand
        // == 2) and his melee class is one-handed, so he fights with TWO one-
        // handers and the buy planner may add a SECOND in-class one-hander.
        // offHandBaseScore: the SECOND-best owned in-class one-hander's
        // WeaponScore (a count >= 2 stack counts twice; 0 when he owns fewer
        // than two) -- the bar a second buy must beat, exactly as
        // meleeBaseScore is the bar for the first. Loot uses the same pair
        // (EquipmentContext::wantOffHand / offHandBaseScore).
        bool          wantOffHand      = false;
        float         offHandBaseScore = 0.0f;
        // -- APPENDED 2026-09-25 (THE SWAP-UP RULE, ammo; 86e3ebfu3; append-only) --
        // Worker-computed by Logistics::AmmoUpgradeBar (logistics/SwapUp.cpp) under
        // bEconomyBuyGear, for a ranged follower's OWN kind (bolts for a crossbow).
        // ammoBarDmg: the damage of the weakest ammo tier he still relies on; a buy
        // must be STRICTLY above it. ammoUpgradeQty: how many rounds above the bar
        // carry his whole keep target -- the most PlanBuy's AMMO SWAP-UP pass buys.
        bool          ammoUpgrade    = false;
        bool          ammoWantBolt   = false;
        float         ammoBarDmg     = 0.0f;
        std::int32_t  ammoUpgradeQty = 0;
        // Round 1 (review of 4f23c30): the rank is damage, VALUE breaking a tie, so
        // the bar is the pair (ammoBarDmg, ammoBarValue).
        std::int32_t  ammoBarValue   = 0;
        // -- APPENDED 2026-10-12 (THE SILVER SLOT, ClickUp 86e3p2gp9; append-only) --
        // Worker-computed (Logistics::WantsSilverWeapon) under bEconomyBuyGear: the follower has an
        // enabled act.equip_silver rule and owns no silver melee weapon. PlanBuy then buys ONE
        // silver melee weapon (Logistics::IsSilverMeleeWeapon), the best affordable by
        // WeaponBuyScore, under the same comfortably-affordable reserve as every gear buy.
        bool          wantSilver     = false;
        // ... one-handed only: his off-hand is in use (Logistics::SilverNeedsOneHand, review F2).
        bool          silverOneHand  = false;
        // -- APPENDED 2026-10-12 (SILVER AMMO, ClickUp 86e3p2gp9; append-only) --
        // Worker-computed (Logistics::WantsSilverAmmo) under bEconomyBuyGear: a RANGED follower with an
        // enabled act.equip_silver rule owns no silver ammo of his kind (silverAmmoBolt: bolts for a
        // crossbow, arrows for a bow). PlanBuy then buys the best affordable silver ammo of that kind
        // (Logistics::IsSilverAmmo), up to Logistics::kSilverAmmoStock rounds.
        bool          wantSilverAmmo = false;
        bool          silverAmmoBolt = false;
        // -- APPENDED 2026-10-19 (TOWN PHASE 2 R8, smithing errands; append-only) --
        // The materials his planned tempers still need, by base form (the engine's own improve recipe components minus what he
        // carries; logistics/ErrandSmith.cpp FillMaterials). PlanBuy buys each form it finds in the vendor's stock up to its
        // qty, REGARDLESS of any gambit (marth: "regardless of gambits a follower will loot or buy whats needed for town errand
        // smithing"), bounded by the purse (half of what is left, so he never goes broke on ingots). Empty = no smithing wish.
        std::vector<SmithMat> smithMats;
        // -- APPENDED (TOWN PHASE 2 R7; append-only) -- the inn's FOOD order: stock lines that are FOOD worth stocking classify as kFood
        // (the needs vector carries the quota). Only logistics/ErrandRest.cpp sets it.
        bool          wantFood = false;
    };

    // THE rated-armor score, shared with Logistics::ArmorScore (loot / keep /
    // owned-equip / sell) so the buy planner -- which has no Actor on the VM
    // thread, only the worker-built thresholds above -- ranks armor by the
    // exact same arithmetic. rating <= 0 (clothing, jewelry) scores 0; an
    // ArmorType that is neither heavy nor light scores the raw rating.
    inline float ArmorScoreOf(float a_rating, RE::BGSBipedObjectForm::ArmorType a_type,
                              float a_heavyBias, float a_lightBias) {
        using AT = RE::BGSBipedObjectForm::ArmorType;
        if (a_rating <= 0.0f) return 0.0f;
        switch (a_type) {
        case AT::kHeavyArmor: return a_rating * a_heavyBias;
        case AT::kLightArmor: return a_rating * a_lightBias;
        default:              return a_rating;
        }
    }

    // Register MFO_Trade's Papyrus natives on the VM. Wired once at plugin load
    // via SKSE::GetPapyrusInterface()->Register(RegisterFuncs).
    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

    // Store a trade order and dispatch RunTrade at MFO_TradeQuest. Called from the
    // (main-thread) econ scan once a vendor is resolved; vectors are moved in.
    // probeOnly is set from bEconomy (off -> dry run). No-op if the trade quest is
    // unresolved/not running.
    // Returns true only if an order was actually dispatched. False = the bridge is
    // unavailable, the chest already has a live order (per-chest guard), or the
    // dispatch failed -- so the caller must NOT burn its trade cooldown.
    // a_sig / a_daysNow: the NO-EMPTY-TRADES memo's inputs (TradeSignature below and
    // RE::Calendar days passed, both read by the caller on the worker); carried in the
    // order so ReportTrade can file an empty result against exactly what was offered.
    bool VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget, const BuyThresholds& a_buy = {},
                     std::uint64_t a_sig = 0, float a_daysNow = 0.0f,
                     RE::FormID a_memoKey = 0,
                     const Logistics::Pricing::Quote* a_quote = nullptr);
    // a_memoKey (town errands, R1b): the vendor key the empty-trade memo is filed under when a_vendor is null or unloaded -- the
    // SAME key the plan's SkipTrade used (the vendor actor's FormID, or the FACTION's for a shop with no loaded vendor).

    // ── NO EMPTY TRADES (marth 2026-09-28; field: 159 dispatches, 156 moved nothing --
    // Jesper at Brand-Shei's chest, gold 1, three sell rows, every ~20 s for 5 min) ──
    // Per (follower, vendor), an order whose report moved NOTHING (sold 0, bought 0) is
    // remembered with its SIGNATURE: the purse, the needs, the offered sell rows (form,
    // count, unit price) and the vendor chest's gold as last seen by ANY report there.
    // TradeSignature hashes the first three; the chest's last-seen gold is folded in by
    // SkipTrade, so another follower's trade that changes the chest's gold is a change.
    std::uint64_t TradeSignature(std::int32_t a_purse, const std::vector<NeedCat>& a_needs,
                                 const std::vector<SellRow>& a_sell, const std::vector<SmithMat>& a_mats = {});
    // (R8: a_mats, the smithing materials wanted, is part of what is offered: a newly wanted ingot must not hit the memo of an
    // earlier empty trade.)
    // Worker, before VendorTrade. True = do NOT dispatch (a_why says why, for the log):
    //  (a) the last result for this pair was empty and the signature is unchanged, until
    //      the vendor's restock -- iDaysToRespawnVendor game days (Skyrim.esm GMST
    //      0x0123C00E, 2) after that empty trade, read by name at runtime;
    //  (b) there is nothing to buy (a_buyWishes false and a_needs empty) and no sell row
    //      the chest's last-seen gold can pay for (unknown gold never skips).
    // A skipped dispatch sets nothing (the [T] glyph's IsTrading window is only stamped
    // by a real dispatch).
    bool SkipTrade(RE::FormID a_follower, RE::FormID a_vendor, RE::FormID a_chest, std::uint64_t a_sig,
                   float a_daysNow, bool a_buyWishes, const std::vector<SellRow>& a_sell,
                   const std::vector<NeedCat>& a_needs, std::string& a_why);

    // ── TOWN ERRAND seam (logistics/Errand.cpp, ClickUp 86e3t1mx7 R1a) ───────────────
    // The errand dispatches ONE VendorTrade per follower and needs to see that it FINISHED:
    // HasLiveOrder = an order for this follower is stored and not yet reported (or reaped);
    // TakeOutcome = the last ReportTrade for this follower (consumed; false = none yet).
    // DiscardOutcome before a dispatch so a stale report is never mistaken for the new one.
    // `aborted` = Papyrus found no usable chest. Native only: no Papyrus signature changes.
    struct TradeOutcome {
        bool         aborted    = false;
        std::int32_t soldCount  = 0;
        std::int32_t soldValue  = 0;
        std::int32_t boughtCount= 0;
        std::int32_t spent      = 0;
        // R8: the smithing materials this trade ACTUALLY bought, by form. Papyrus reports only totals, so a line is credited only when
        // the report shows EVERY planned unit was bought (boughtCount >= the plan's total); otherwise nothing is credited (never a guess).
        std::vector<SmithMat> smithBought;
    };
    // The vendor chest's barter gold as the LAST report there read it (-1 = never reported). Worker.
    std::int32_t LastSeenChestGold(RE::FormID a_chest);
    bool HasLiveOrder(RE::FormID a_follower);
    bool TakeOutcome(RE::FormID a_follower, TradeOutcome& a_out);
    void DiscardOutcome(RE::FormID a_follower);

    // Drop any pending orders (revert/quit). Token map is transient state.
    void ClearTransientState();

}
