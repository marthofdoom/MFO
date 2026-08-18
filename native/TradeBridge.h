#pragma once
#include "PCH.h"
#include <unordered_set>

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
        // APPEND-ONLY (kSpellTome added for the town-update follower-buy-spells
        // feature). This enum is native-internal -- carried in the token'd order
        // between the econ scan and PlanBuy, NEVER crosses the Papyrus wire and is
        // NOT serialized -- so appending a kind is free (no save/script contract).
        enum Kind : std::int32_t { kPotHealth = 0, kPotStamina, kPotMagicka, kArrows, kBolts, kSpellTome };
        std::int32_t kind  = 0;
        std::int32_t quota = 0;
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
    // a_ownedTomeSpells: taught-spell FormIDs the follower ALREADY carries a tome
    // for -- collected worker-side in the econ scan (the follower's own inventory
    // read is safe there), so PlanBuy never re-buys a tome the follower hasn't
    // learned yet. Empty when the buy-spells feature is off.
    bool VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget,
                     std::unordered_set<RE::FormID> a_ownedTomeSpells = {});

    // Drop any pending orders (revert/quit). Token map is transient state.
    void ClearTransientState();

}
