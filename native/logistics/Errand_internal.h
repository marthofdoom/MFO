#pragma once
// logistics/Errand_internal.h -- the state the town-errand files share (ClickUp 86e3t1mx7). NOT a public API:
// only logistics/Errand.cpp (the planner + the PHYSICAL errand), logistics/ErrandSim.cpp (R1b: the OFF-SCREEN
// simulation, the hand-offs and the recall) and logistics/Economy.cpp (the plan entry) include it. The public
// surface is logistics/Errand.h.
#include "Errand.h"
#include "TradeBridge.h"

#include <memory>
#include <string>
#include <unordered_map>
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
        bool          wishMaterials = false;   // R8: BuyThresholds::smithMats is non-empty (his planned tempers need materials this stop may sell)
        int           purse = 0;
        std::int32_t  chestGold = -1;    // the chest's last-seen barter gold (-1 unknown)
        // the order (valid when !skipped)
        std::vector<TradeBridge::SellRow> sell;
        std::vector<TradeBridge::NeedCat> needs;
        TradeBridge::BuyThresholds        buy{};
        std::uint64_t                     sig = 0;
        float                             daysNow = 0.0f;
        Pricing::Quote                    quote;   // R2: the plan-time price quote; the sim replay trades with THIS one (session-only, never saved)
    };
    void EconomyErrandPlan(RE::Actor* a_follower, const FollowerState& a_state,
                           std::chrono::steady_clock::time_point a_now,
                           const std::vector<ErrandShop>& a_shops, std::vector<ErrandPlanned>& a_out);
    // R5: the physical give beat of ONE STOP of a trip: EconomyProbe(kExecute) for a_probe.vendors, with the trip's PARTITION
    // applied. a_share (the stop's planned order, null = no partition: a restored stop re-plans in full) limits the sell rows
    // to the objects it lists, the needs to its kinds and the gear / tome wishes to its flags; a_purseCap (< 0 = none) caps
    // the purse at what the trip purse has left. Everything else (live counts, prices, SkipTrade, VendorTrade) is the
    // unchanged probe.
    struct ErrandProbe;
    void EconomyErrandExecute(RE::Actor* a_follower, const FollowerState& a_state,
                              std::chrono::steady_clock::time_point a_now, ErrandProbe& a_probe,
                              const ErrandPlanned* a_share, int a_purseCap);
}

namespace MFO::Logistics::Errand {

    // What the ch.19 leg walks to.
    enum class DestKind { VendorActor, Ref, Cell };

    // ── R5: the ITINERARY (design 1.b) ────────────────────────────────────────────────────────────────
    // A trip is an ordered list of stops with ONE purse. Phase-1 errands are one-stop trips of kind Shop. The single-shop
    // fields of the records below (faction, chest, vendor, dest*, order) are the CURRENT stop's working copy, stops[cur]
    // (Errand.cpp / ErrandSim.cpp keep the two in step), so the phase-1 code paths keep their shape.
    enum class StopKind : std::uint8_t { Shop, Temple, Inn, Smith };   // R7 appended Temple and Inn (the last stop), R8 Smith (the forge / sharpening wheel / armor table); later rounds APPEND Fence, Spouse, Home
    enum class StopState : std::uint8_t { Pending, Active, Done, Skipped };
    struct Stop {
        StopKind      kind = StopKind::Shop;
        RE::FormID    faction = 0, chest = 0;
        RE::FormID    vendorActor = 0;           // 0 = not known (a faction-only shop until he reaches it)
        std::string   vendorName;
        DestKind      destKind = DestKind::Cell;
        RE::FormID    destForm = 0;
        float         destRad = 0.0f;
        RE::NiPoint3  pos{};                     // where the place is (runtime only; a CELL shop: its door), for the walk estimates
        bool          havePos = false;
        std::shared_ptr<const ErrandPlanned> order;   // this stop's SHARE of the trip (null after a load: never saved)
        double        tArrive = 0.0, tWorkEnd = 0.0;  // GAME DAYS, the simulation's (0 = not reached yet)
        StopState     state = StopState::Pending;
    };
    // The bound on the wire (#11) and the planner's cap. R7 appends an Inn stop after the shops, so the shops leave room.
    inline constexpr std::size_t kMaxTripStops = kMaxTripStopsOnWire;
    // R7 (marth: one trip covers EVERYTHING): 5 shops + the temple + R8's smith stop + the inn = the 8 stops of the ERND wire. R5 had 4.
    inline constexpr std::size_t kMaxShopStops = 5;

    namespace Trip {
        // One candidate the planner may visit (Errand.cpp builds these from its catalog; the VEND reads stay there).
        struct Cand {
            RE::FormID    key = 0;            // the plan key: ErrandPlanned::vendorKey (the vendor actor, else the faction)
            RE::FormID    actorId = 0, faction = 0, chest = 0;
            std::string   name;
            float         dist = 0.0f;        // player -> place, straight line (a cell: its nearest loaded door)
            RE::NiPoint3  pos{};
            bool          havePos = false;
            DestKind      kind = DestKind::Cell;
            RE::FormID    destForm = 0;
            float         destRad = 0.0f;
            std::uint32_t needCarry = 0;      // bit n = this VEND carries the category of NeedCat::Kind n
            bool          carriesGear = false, carriesTome = false;   // the gear / apparel wish, the tome wish
            bool          carriesMaterials = false;   // R8: the VEND trades ore / ingots, hides or animal parts (smithing materials)
            bool          carriesForge = false;       // R8: a smithy: the VEND trades ore / ingots or weapons (its place has the forge furniture)
        };
        struct Inputs {
            RE::FormID    follower = 0;
            RE::NiPoint3  from{};             // the follower's position (the nearest-neighbour origin)
            float         carry = 0.0f;       // inventory weight / carry weight
            int           minSell = 0, minPurse = 0;
        };
        // Plan a trip over the evaluated candidates (a_planned: EconomyErrandPlan's output, one per candidate). Fills a_out
        // (stops in visit order, each with its own order) and a_line (the one "[errand] TRIP" plan line's body). False = no
        // stop has work.
        bool Plan(const std::vector<Cand>& a_cands, const std::vector<ErrandPlanned>& a_planned, const Inputs& a_in,
                  std::vector<Stop>& a_out, std::string& a_line);
        // The next Pending stop after a_cur, or -1.
        int  NextPending(const std::vector<Stop>& a_stops, std::size_t a_cur);
        // Straight-line units between two stops' places (a place with no known position: a fixed street-walk estimate).
        double LegUnits(const Stop& a_from, const Stop& a_to);
        // Where a stop's place is NOW (the vendor actor / the ref, else the planned position). False = unknown.
        bool PlacePos(const Stop& a_stop, RE::NiPoint3& a_out);
        const char* StateName(StopState a_s);
        // The game-minute cap of a whole trip: fErrandMaxGameMin (the phase-1 allowance of one walk + trade) once per PLANNED stop
        // (a_stops 0 or 1 = phase 1 exactly). The planned count, not the remaining one, so no stop transition ever extends it.
        float CapGameMin(std::size_t a_stops);
        // "2/3 'Belethor'" for the log lines.
        std::string Label(const std::vector<Stop>& a_stops, std::size_t a_cur);
        // The ERND v2 itinerary <-> the runtime one.
        SavedStop ToSaved(const Stop& a_stop);
        Stop      FromSaved(const SavedStop& a_saved);
        // B300 (convergence): how many OTHER followers' trips are heading to each chest (their current and pending stops).
        // Errand g_mx, then Sim g_mx, never nested.
        std::unordered_map<RE::FormID, int> ChestCrowd(RE::FormID a_exclude);
    }

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
        // R2: a PHYSICAL errand restored from the 'ERND' record. It carries no order (the order is never saved): the
        // physical resume re-plans it on screen, an unloaded one drops the trade (Sim::Sweep's restore decision).
        bool          restored = false, hadOrder = false, tradeAbandoned = false;
        bool          restoredPhysical = false;   // restored from a PHYSICAL stage (OnLoadedTick resumes him anywhere in the town)
        // R5: the itinerary (the fields above are stops[cur]'s) and the trip purse (-1 = none: a restored trip re-plans on live gold).
        std::vector<Stop> stops;
        std::uint8_t  cur = 0;
        int           tripPurse = -1, tripSpent = 0;
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
        bool          announced = false;   // the "is trading with" toast already fired for this errand (sim Trade stage reached)
        bool          work = false;        // R7: he is loaded AT the inn / temple of the current stop: start its work at once (no walk, no trade)
        // R5: the itinerary (the fields above are stops[cur]'s) and the trip purse.
        std::vector<Stop> stops;
        std::uint8_t  cur = 0;
        int           tripPurse = -1, tripSpent = 0;
    };

    // ── TOWN PHASE 2 R7: the INN and TEMPLE stops -- logistics/ErrandRest.cpp ──────────────────────────────────────────
    // The planner appends them (Augment: the temple when owed, the inn LAST), the physical errand runs their WORK (Begin / Step: the
    // ch.9 work packages through apmf/ErrandWork.cpp, the ch.12 kneel idles), the simulation only waits (Errand::Sim). Own mutex (a
    // leaf: never held across an engine, Harbinger, Papyrus or TradeBridge call, and never nested with Errand / Sim g_mx).
    namespace Rest {
        // Planner (worker, TryStart, the follower loaded). a_haveStops = Trip::Plan found shop work. Appends the temple stop (when he is
        // diseased or not yet blessed today) and the inn stop (LAST of every trip) to a_stops; with NO shop work a trip exists only when
        // the temple is owed (then it ends at the inn too). TRUE = a_stops describes a trip. a_line gets the text appended.
        bool Augment(RE::Actor* a_follower, const FollowerState& a_state, std::chrono::steady_clock::time_point a_now,
                     const RE::BGSLocation* a_town, bool a_haveStops, std::vector<Stop>& a_stops, std::string& a_line);
        // ONE TRIP PER TOWN VISIT (marth 2026-10-19): once a follower's trip ends no new errand is planned for him until the player
        // leaves the town. Session-only; NoteTown clears every mark when the player's town changes (TryStart calls it on every tick).
        void NoteTown(RE::FormID a_townId);
        bool TripDone(RE::FormID a_follower, RE::FormID a_townId);
        void MarkTripDone(RE::FormID a_follower, RE::FormID a_townId, const char* a_why);
        // The synthetic food order's dispatch (the inn's physical give beat): VendorTrade of a_order at the innkeeper's chest.
        // Sent = on its way; Busy = the chest holds another follower's live order (retry); Nothing = nothing to buy (no purse / no chest).
        enum class Dispatch { Sent, Busy, Nothing };
        Dispatch DispatchOrder(RE::Actor* a_follower, RE::Actor* a_vendor, const ErrandPlanned& a_order, int a_purseCap);

        // The physical work. Begin: he is AT the stop (the inn after the shop beats / the temple's cell). False = cannot (logged); the stop is skipped.
        bool Begin(RE::Actor* a_follower, const Stop& a_stop, RE::FormID a_vendor, const std::string& a_followerName, RE::FormID a_town, std::chrono::steady_clock::time_point a_now);
        enum class Result { Continue, Done, Failed };
        struct StepOut { Result res = Result::Continue; std::string why; };
        StepOut Step(RE::Actor* a_follower, std::chrono::steady_clock::time_point a_now);
        // The errand ends / the stop ends: release the work claim and forget it. a_why is logged (the release reason of an inn stay).
        // a_tripOver: the stay / visit really ended (marks the trip done for this town visit); false = handed to the simulation.
        void End(RE::FormID a_follower, const char* a_why, bool a_tripOver);
        void ClearTransientState();   // revert / load (pump drained): every ledger, mark and work
        bool Working(RE::FormID a_follower);
        // The Board text: "Resting at X" / "Praying at X" ("" = no work).
        std::string Describe(RE::FormID a_follower);
        // Is he standing in the cell of this inn / temple stop? (a loaded follower, for the simulation's pick-up)
        bool InStopCell(RE::Actor* a_follower, const Stop& a_stop);
        // Sim: a temple visit ran off screen; the blessing is OWED (logged; paid only by a physical visit).
        void NoteOwed(RE::FormID a_follower, const std::string& a_followerName, const std::string& a_temple);
    }

    namespace Sim {
        bool Has(RE::FormID a_follower);
        // R3: the Board status of a simulated errand ("" = none). Any thread, copies under g_mx.
        std::string Describe(RE::FormID a_follower);
        // Worker. Takes over an errand that just went off screen. Logs OUT_SIM / TRADE_SIM / RET_SIM transitions.
        void Begin(SimStart&& a_start);
        // The pump, every lap (called from Errand::Sweep): the game-time clock, the replayed trade, the return,
        // leave-town / combat / dismissal handling, the passive [errand-pos] drift line.
        void Sweep();
        // The service tick of a follower who has a simulation record and is LOADED again. TRUE = the record owns
        // the tick (it resumed the physical errand this tick).
        bool OnLoadedTick(RE::Actor* a_follower, const FollowerState& a_state);
        void ClearTransientState();   // also the staged restore entries
        // R2 ('ERND'): append every simulation record and every staged-not-yet-adopted entry (SaveCallback, pump paused).
        void SnapshotInto(std::vector<Saved>& a_out);
        // R2: stage restored entries (LoadCallback, pump stopped); Sweep adopts them on its first lap in the world.
        void StagePending(std::vector<Saved>&& a_entries);
        void ClearPending();
        // R5 (B300): add every simulated trip's current + pending stop chests to a_crowd (a_exclude's own skipped). Takes Sim g_mx.
        void CrowdInto(std::unordered_map<RE::FormID, int>& a_crowd, RE::FormID a_exclude);
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
        // bTownToasts: one informational HUD line (RE::DebugNotification, posted to the main thread). No-op while the toggle is OFF.
        void  Toast(const std::string& a_text);
        // "<who> is trading with <shop>." via Toast, at the moment the trade begins (arrival, physical or simulated).
        // R6: a_faction (the stop's vendor faction, 0 = unknown) of a FENCE reads "<who> is visiting <fence>." (ToastFence) instead.
        void  ToastTrading(const std::string& a_who, const std::string& a_shop, RE::FormID a_faction = 0);
        // R5 (B300): the physical trips' share of Trip::ChestCrowd. Takes Errand g_mx.
        void  CrowdInto(std::unordered_map<RE::FormID, int>& a_crowd, RE::FormID a_exclude);
        // R3: STRINGS ONLY, no caller yet (the phase-2 rounds that add each errand kind call them at their own begin point).
        // Informational: never a gold amount, never a failure (marth's ruling for town toasts).
        void  ToastSmithing(const std::string& a_who, const std::string& a_place);   // "<who> is working at <place>."
        void  ToastResting(const std::string& a_who, const std::string& a_inn);      // "<who> is resting at <inn>."
        void  ToastPraying(const std::string& a_who, const std::string& a_temple);   // "<who> is praying at <temple>."
        void  ToastFence(const std::string& a_who, const std::string& a_fence);      // "<who> is visiting <fence>."
    }
}

// ── R6: fences + spouse stores (logistics/ErrandShops.cpp; design 1.c) ────────────────────────────────────────────────────
namespace MFO::Logistics::ErrandShops {
    // Open = a candidate; Closed = not one; Pending = the main-thread condition evaluation is still being computed (skip this plan).
    enum class Gate { Open, Closed, Pending };
    // A base form's copies in the follower's inventory: none stolen, every copy stolen, or a mix (sold nowhere).
    enum class Stolen : std::uint8_t { None, All, Mixed };
    // A pure fence: buysStolen && !buysNonStolen. The ONLY vendors a stolen row may reach (and they take nothing else).
    bool IsFence(const RE::TESFaction* a_fac);
    bool IsFenceFaction(RE::FormID a_fac);
    // Worker (BuildCatalog). a_vendor = the loaded vendor actor, empty for a faction-only shop. Both log ONE decision line.
    Gate FenceGate(RE::Actor* a_follower, const RE::TESFaction* a_fac, RE::ActorHandle a_vendor);
    Gate SpouseGate(RE::Actor* a_follower, const RE::TESFaction* a_fac, RE::ActorHandle a_vendor);
    // Worker (EconomyProbeImpl, once per scan): every base form in his inventory classified by its copies' ExtraOwnership.
    std::unordered_map<const RE::TESBoundObject*, Stolen> ClassifyStolen(RE::Actor* a_follower);
    void NoteMixed(RE::Actor* a_follower, const RE::TESBoundObject* a_obj);   // one [econ] line per base per session
    void Clear();                                                              // session reset (Errand::ClearTransientState)
}

// ── TOWN PHASE 2 R8: SMITHING / TEMPERING ERRANDS -- logistics/ErrandSmith.cpp ─────────────────────────────────────────────
// The follower's own Smithing (his AV, unclamped) and tempering perks (kModTemperingHealth, entry point 76 with the item) decide
// how far each WORN weapon / armor piece can be improved; the engine's own improve recipe (the temper COBJ) names the materials.
// The planner (Augment) appends a Smith stop AFTER the shops and BEFORE R7's temple / inn; materials are bought on the shop stops
// through BuyThresholds::smithMats (FillMaterials) whatever his gambits say; the physical stop (Begin / Step) finds a sharpening
// wheel / armor table in the loaded cell, offers the station's animation through Harbinger (ch.9 Activate work package, R7's
// apmf/ErrandWork.cpp -- stubbed behind OfferWork until it merges) and applies the temper: WornObject.SetItemHealthPercent, read
// back natively, the materials removed only for the pieces that took. Off screen (SimFinish) only ARMOR is applied (a weapon needs
// a process, R0-5/P-SMITH-W) and the rest waits for the next visit. Session-only: nothing here is saved (the ERND wire only
// carries the string opcode "smith" for the stop's kind).
// THREADING: Prepare / FillMaterials / Augment / Begin / Step / End / SimFinish run on the AddTask job worker (the planner and the
// service tick, like Rest below); the want-list BUILD and the temper APPLY run on the MAIN thread (the entry point evaluates perk
// conditions with no lock, and the actor must be loaded -- TradePerks.h). Own mutex (a leaf: never held across an engine, Harbinger,
// Papyrus or TradeBridge call, never nested with Errand / Sim g_mx).
namespace MFO::Logistics::Errand::Smith {
    enum class Prep { Off, Ready, Pending };
    // Planner gate (worker, TryStart, BEFORE EconomyErrandPlan). Ready = a want list younger than ~30 s exists (it may be empty);
    // Pending = a main-thread build was queued, the caller returns and plans again on its next cadence; Off = smithing is off.
    // a_smithyHere: some open shop of this town trades ore / ingots or weapons (materials are only wished for when it does).
    Prep Prepare(RE::Actor* a_follower, bool a_smithyHere);
    // EconomyProbeImpl (errand mode, worker): the materials he still has to BUY for his planned tempers -> BuyThresholds::smithMats.
    // Cache read only.
    void FillMaterials(RE::FormID a_follower, std::vector<TradeBridge::SmithMat>& a_out);
    // Planner (worker, after Trip::Plan, BEFORE R7's Rest::Augment): appends the Smith stop to a_stops when he has an improvable
    // piece AND (carries the materials OR a planned shop stop buys them). True = a Smith stop was appended (a_line gets its text).
    bool Augment(RE::Actor* a_follower, const std::vector<Trip::Cand>& a_cands, std::vector<Stop>& a_stops, std::string& a_line);

    enum class Result { Continue, Done, Failed };
    struct StepOut { Result res = Result::Continue; std::string why; };
    // The physical work. Begin: he is AT the stop (the smithy's place). False = cannot (logged); the stop is skipped.
    bool Begin(RE::Actor* a_follower, const Stop& a_stop, const std::string& a_followerName, std::chrono::steady_clock::time_point a_now);
    StepOut Step(RE::Actor* a_follower, std::chrono::steady_clock::time_point a_now);
    // The errand ends / the stop ends: release the work claim and forget the work. Idempotent.
    void End(RE::FormID a_follower, const char* a_why);
    bool Working(RE::FormID a_follower);
    std::string Describe(RE::FormID a_follower);   // "Working at X" ("" = no work). Any thread.
    // Off screen: the game-day cost of the SMITH_SIM stage, and its end (worker). a_materialsBought = every earlier shop stop that
    // wished for materials reported (its trade replayed). Returns the one-line summary (also logged).
    double SimWorkDays();
    std::string SimFinish(RE::FormID a_follower, const std::string& a_followerName, bool a_materialsBought);
    void ClearTransientState();   // revert / load (pump drained)
}
