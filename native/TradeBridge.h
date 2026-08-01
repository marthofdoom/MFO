#pragma once
#include "PCH.h"

// #21 ECON BRIDGE -- native side (Fable's ECON_PAPYRUS_PLAN).
//
// The merchant read + transaction cannot run in C++: native
// TESObjectREFR::GetInventory() on an unpopulated merchant chest CTDs on any
// thread (memory economy-vendor-detection-excludes-teammates, ENGINE_NOTES
// §0.37). So the merchant work moves to Papyrus (MFO_Trade.psc), the path the
// game's own barter menu uses -- and native keeps the DECISION (vendor
// resolution, sellables, buy candidates, quality ranking).
//
// FLOW (Phase 1, read-only probe): native resolves the vendor + builds the SELL
// list (follower's own inventory -- safe) and the ranked BUY candidates (catalog
// + gambit quotas -- safe), stores them as a token'd TradeOrder, and dispatches
// RunTrade(token) at MFO_TradeQuest. MFO_Trade pulls the order through the
// registered natives, does the crash-prone reads in Papyrus
// (chest.GetItemCount(Gold001) + per-candidate stock), and reports back via
// ReportProbe -- native then logs the WOULD SELL / WOULD BUY plan. Zero
// transactions in Phase 1; the mutation loops flip on in Phase 2/3.
namespace MFO::TradeBridge {

    // One sellable line -- the follower's own, un-worn, un-excluded, vendor-
    // tradeable gear. `obj`/`value`/`count` are all native-safe reads.
    struct SellRow {
        RE::TESBoundObject* obj     = nullptr;
        std::int32_t        count   = 0;
        std::int32_t        value   = 0;    // per-unit gold value
        bool                jewelry = false;
    };

    // One buy candidate -- a catalog supply item (potion/ammo) for a gambit that
    // is below its threshold. `label` names the category (potH/arrows/...),
    // `have`/`want` the follower's current vs target count. Papyrus fills in the
    // vendor STOCK for `obj`; native already knows `value`.
    struct BuyRow {
        RE::TESBoundObject* obj   = nullptr;
        std::int32_t        value = 0;
        std::int32_t        have  = 0;
        std::int32_t        want  = 0;
        std::string         label;
    };

    // Register MFO_Trade's Papyrus natives on the VM. Wired once at plugin load
    // via SKSE::GetPapyrusInterface()->Register(RegisterFuncs).
    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

    // Store a read-only probe order and dispatch RunTrade at MFO_TradeQuest.
    // Called from the (worker) econ scan once a vendor is resolved; the vectors
    // are moved in. No-op (logs) if the trade quest is unresolved/not running.
    void VendorProbe(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<BuyRow> a_buy,
                     std::int32_t a_budget);

    // Drop any pending orders (revert/quit). Token map is transient state.
    void ClearTransientState();

}
