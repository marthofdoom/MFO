#pragma once
#include "PCH.h"

// #21 ECON BRIDGE -- native side (Fable's ECON_PAPYRUS_PLAN).
//
// The merchant read + transaction cannot run in C++: native
// TESObjectREFR::GetInventory() / Actor::GetGoldAmount() CTD on a merchant chest
// / a follower whose InventoryChanges the worker tick is mutating (§0.37, memory
// getgoldamount-ctds-count-gold-from-getinventory). So the merchant work moves to
// Papyrus (MFO_Trade.psc) -- the path the game's own barter menu uses -- and
// native keeps the DECISION (vendor resolution, sellables, catalog).
//
// FLOW: native resolves the vendor + builds the SELL list from the follower's own
// inventory (safe), stores it as a token'd TradeOrder, and dispatches
// RunTrade(token) at MFO_TradeQuest. MFO_Trade pulls the order through the
// registered natives, reads the chest's barter gold (crash-free GetItemCount),
// and -- when bEconomy is on -- runs the SELL loop (RemoveItem follower->chest,
// pay the follower, deduct the chest's gold), capped at the chest's gold. It then
// reports what actually moved. bEconomy OFF = a dry run: it computes and reports
// the plan without mutating (the read-only probe).
namespace MFO::TradeBridge {

    // One sellable line -- the follower's own, un-worn, un-excluded, vendor-
    // tradeable gear. All native-safe reads. Native pre-sorts highest-value-first.
    struct SellRow {
        RE::TESBoundObject* obj     = nullptr;
        std::int32_t        count   = 0;
        std::int32_t        value   = 0;    // per-unit gold value
        bool                jewelry = false;
    };

    // Register MFO_Trade's Papyrus natives on the VM. Wired once at plugin load
    // via SKSE::GetPapyrusInterface()->Register(RegisterFuncs).
    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

    // Store a sell order and dispatch RunTrade at MFO_TradeQuest. Called from the
    // (main-thread) econ scan once a vendor is resolved; the sell vector is moved
    // in. probeOnly is set from bEconomy (off -> dry run). No-op if the trade
    // quest is unresolved/not running.
    void VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::int32_t a_budget);

    // Drop any pending orders (revert/quit). Token map is transient state.
    void ClearTransientState();

}
