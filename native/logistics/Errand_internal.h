#pragma once
// logistics/Errand_internal.h -- the state the town-errand files share (ClickUp 86e3t1mx7). NOT a public API:
// only logistics/Errand.cpp (the planner + the PHYSICAL errand), logistics/ErrandSim.cpp (R1b: the OFF-SCREEN
// simulation, the hand-offs and the recall) and logistics/Economy.cpp (the plan entry) include it. The public
// surface is logistics/Errand.h.
#include "Errand.h"
#include "TradeBridge.h"

#include <memory>
#include <string>
#include <vector>

namespace MFO::Logistics {

    // ── the PLAN entry (defined in Economy.cpp beside EconomyProbe) ──────────────────────────────────────
    // One candidate shop: its vendor FACTION, and the vendor ACTOR when he is loaded (null = an interior shop
    // whose vendor is not in memory; the order is built for the faction's merchant chest all the same).
    struct ErrandShop {
        RE::TESFaction* fac = nullptr;
        RE::ActorHandle vendor;
    };
    // What the plan learned about one candidate AND the order to replay. The order is built NOW, while the
    // follower is loaded (house rule: MFO never reads an unloaded follower's inventory); the simulation hands it
    // to the unchanged TradeBridge::VendorTrade later.
    struct ErrandPlanned {
        RE::FormID    vendorKey = 0;     // the vendor actor's FormID, or the FACTION's for a shop with no loaded vendor
        RE::FormID    faction = 0;
        RE::FormID    chest = 0;
        bool          skipped = false;   // SkipTrade said no (the empty-trade memo / nothing to do here)
        int           sellRows = 0;
        std::int64_t  sellValue = 0;     // unit price x count over the rows THIS vendor accepts
        std::uint32_t needKinds = 0;     // bit n = a TradeBridge::NeedCat::Kind n supply need
        bool          wishGear = false, wishApparel = false, wishTome = false;
        int           purse = 0;
        std::int32_t  chestGold = -1;    // the chest's last-seen barter gold (-1 unknown)
        // the order (valid when !skipped)
        std::vector<TradeBridge::SellRow> sell;
        std::vector<TradeBridge::NeedCat> needs;
        TradeBridge::BuyThresholds        buy{};
        std::uint64_t                     sig = 0;
        float                             daysNow = 0.0f;
    };
    void EconomyErrandPlan(RE::Actor* a_follower, const FollowerState& a_state,
                           std::chrono::steady_clock::time_point a_now,
                           const std::vector<ErrandShop>& a_shops, std::vector<ErrandPlanned>& a_out);
}

namespace MFO::Logistics::Errand {

    // What the ch.19 leg walks to.
    enum class DestKind { VendorActor, Ref, Cell };

    // Errand.cpp -> ErrandSim.cpp: the follower unloaded (or was found unloaded) with an errand in progress.
    // `arrived` = he had already reached the vendor (the trade is due at once); `tradeDispatched` = the order is
    // in flight; `tradeDone` = it reported (only the walk back is left).
    struct SimStart {
        RE::FormID    follower = 0;
        std::string   followerName, vendorName;
        RE::FormID    faction = 0, chest = 0, town = 0;
        RE::FormID    vendorActor = 0;          // 0 = unknown / not loaded
        DestKind      destKind = DestKind::Cell;
        RE::FormID    destForm = 0;
        float         destRad = 0.0f;
        std::shared_ptr<const ErrandPlanned> order;
        RE::NiPoint3  lastPos{};                // the follower's last LOADED position
        RE::FormID    lastCell = 0;
        float         startDays = 0.0f;
        bool          arrived = false, tradeDispatched = false, tradeDone = false;
    };

    // Errand.cpp -> ErrandSim.cpp: a follower the simulation was running is LOADED again (the player walked in).
    struct ResumeInfo {
        RE::FormID    follower = 0;
        std::string   followerName, vendorName;
        RE::FormID    faction = 0, chest = 0, town = 0;
        std::shared_ptr<const ErrandPlanned> order;
        float         startDays = 0.0f;
        DestKind      destKind = DestKind::Cell;   // the errand's original destination (a fallback when no vendor actor is loaded)
        RE::FormID    destForm = 0;
        float         destRad = 0.0f;
    };

    namespace Sim {
        bool Has(RE::FormID a_follower);
        // Worker. Takes over an errand that just went off screen. Logs OUT_SIM / TRADE_SIM / RET_SIM transitions.
        void Begin(SimStart&& a_start);
        // The pump, every lap (called from Errand::Sweep): the game-time clock, the replayed trade, the return,
        // leave-town / combat / dismissal handling, the passive [errand-pos] drift line.
        void Sweep();
        // The service tick of a follower who has a simulation record and is LOADED again. TRUE = the record owns
        // the tick (it resumed the physical errand this tick).
        bool OnLoadedTick(RE::Actor* a_follower, const FollowerState& a_state);
        void ClearTransientState();
    }

    // Errand.cpp services the simulation uses (defined there, outside the anonymous namespace).
    namespace detail {
        void  Cooldown(RE::FormID a_follower, float a_seconds);
        // Rebuild a PHYSICAL errand from a simulation record: the follower is loaded in the shop's cell, so find the
        // vendor actor there and walk to him (the trade is NOT done). False = no vendor actor (shop unattended).
        bool  ResumePhysical(RE::Actor* a_follower, const ResumeInfo& a_info);
        // Put an alive, UNLOADED follower beside the player (~250 u behind his heading) with a Papyrus MoveTo, unless
        // he is already in the player's cell. `a_why` goes to the log. No-op during a loading screen.
        void  RecallToPlayer(RE::FormID a_follower, const char* a_why);
        bool  InLoadingMenu();
        // Is a vendor ACTOR of this faction standing, loaded and awake, in the follower's (loaded) cell?
        bool  VendorPresentNear(RE::Actor* a_follower, RE::FormID a_faction);
        RE::FormID PlayerTownId();   // the BGSLocation id of the player's town, 0 = not in one
        const char* NameOf(const RE::Actor* a_actor);
    }
}
