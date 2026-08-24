#pragma once
#include "PCH.h"

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
            kWeaponMelee, kWeaponRanged, kArmor, kMageApparel, kSpellTome
        };
        std::int32_t kind  = 0;
        std::int32_t quota = 0;
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
        std::int32_t  meleeBaseDmg = 0;      // best in-class melee weapon dmg the follower already owns
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
        // MEO-aware ranking (marth): value-driven ONLY when MEO is present (gems
        // transfer and supply school relevance). schoolPrimary == true (MEO absent
        // OR bMageApparelStrictSchool) ranks by school-enchant tier first; false
        // ranks purely by gold value. Computed on the worker (MEOBridge::Available
        // is worker-safe).
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
    };

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
    bool VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget, const BuyThresholds& a_buy = {});

    // Drop any pending orders (revert/quit). Token map is transient state.
    void ClearTransientState();

}
