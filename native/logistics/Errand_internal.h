#pragma once
// logistics/Errand_internal.h -- the state the town-errand files share (ClickUp 86e3t1mx7). NOT a public API:
// only logistics/Errand.cpp (the planner + the PHYSICAL errand), logistics/ErrandSim.cpp (R1b: the OFF-SCREEN
// simulation, the hand-offs and the recall) and logistics/Economy.cpp (the plan entry) include it. The public
// surface is logistics/Errand.h.
#include "Errand.h"
#include "TradeBridge.h"
#include "Runtime.h"   // PlvdPlaceVerified: the exact-build gates

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace MFO::Logistics {

    // ── the faction PLVD place (vendorLocation), per runtime (ENGINE_NOTES §0.52) ──────────────────────────────
    // PackageLocation::Data is a union of TESForm* and a 32-bit ObjectRefHandle; WHICH member a file-loaded PLVD holds is set by the
    // engine's loader, so it is read only on a build whose loader was disassembled (principle 11: each build on its own proof).
    // On every build below: TESFaction::Load (vtable slot 6) allocates 0x18 bytes, the PackageLocation ctor writes the type byte at +0x08,
    // the radius at +0x0C, the raw form id at +0x10, and the pointer is stored at faction+0xB8 (vendorData +0xA8, vendorLocation +0x10).
    // TESFaction::InitItemImpl (slot 0x13) then passes [faction+0xB8] to the resolver, which turns kInCell into a
    // dynamic_cast<TESObjectCELL*> in data.object (type stays 1) and kNearReference into a CreateRefHandle'd 32-bit handle in
    // data.refHandle (type stays 0):
    //   1.6.1170: Load 0x3AC9C0 (store 0x3ACDAB), ctor 0x49BE80, InitItemImpl 0x3AD2E0 (mov rcx,[rdi+0xB8] 0x3AD56E), resolver 0x49C630
    //   1.5.97:   Load 0x354670 (store 0x354996), ctor 0x441160, InitItemImpl 0x354DE0 (mov rcx,[rdi+0xB8] 0x355067), resolver 0x441850
    //   1.7.104:  Load 0x3B3A80 (store 0x3B3E6B), ctor 0x4A33A0, InitItemImpl 0x3B43A0 (mov rcx,[rdi+0xB8] 0x3B462E), resolver 0x4A3B50
    // The three builds share the layout, and CommonLib's struct compiles to the same offsets (asserted below), so one read serves all
    // three; any other build refuses the read (shops are then the loaded-vendor ones only), logged once.
    static_assert(offsetof(RE::TESFaction, vendorData) == 0xA8, "TESFaction::vendorData moved: re-verify PlvdPlaceVerified");
    static_assert(offsetof(RE::FACTION_VENDOR_DATA, vendorLocation) == 0x10, "vendorLocation moved: re-verify PlvdPlaceVerified");
    static_assert(offsetof(RE::PackageLocation, locType) == 0x08, "PackageLocation::locType moved: re-verify PlvdPlaceVerified");
    static_assert(offsetof(RE::PackageLocation, rad) == 0x0C, "PackageLocation::rad moved: re-verify PlvdPlaceVerified");
    static_assert(offsetof(RE::PackageLocation, data) == 0x10, "PackageLocation::data moved: re-verify PlvdPlaceVerified");
    inline bool PlvdPlaceVerified() {
        if (Runtime::IsVerified1_6_1170() || Runtime::IsVerified1_5_97() || Runtime::IsVerified1_7_104()) return true;
        static std::atomic_bool noted{ false };
        if (!noted.exchange(true))
            spdlog::warn("[errand] the vendor factions' PLVD place is NOT read on runtime {} (layout disassembly-verified on 1.6.1170, "
                         "1.5.97 and 1.7.104 only): interior shops whose vendor is not loaded, and the inn/temple data resolve, are skipped",
                         REL::Module::get().version().string("."));
        return false;
    }

    // F4 (town run 3): a faction-only shop (no loaded vendor) usually has an EMPTY faction FULL name, so every log/toast/watch line said '?'.
    // Its name: the faction's own FULL, else its PLVD place (the way BuildTownShops reads it: kInCell = the cell's FULL; kNearReference = the
    // ref's parent cell FULL, else the ref's display name; any other type = the merchant chest's cell FULL). Empty string = nothing named it
    // (callers keep their own "?").
    inline std::string ShopPlaceName(const RE::TESFaction* a_fac) {
        if (!a_fac) return {};
        if (const char* n = a_fac->GetName(); n && *n) return n;
        if (!PlvdPlaceVerified()) return {};   // the PLVD union layout, per runtime (above)
        auto* loc = a_fac->vendorData.vendorLocation;
        if (!loc) return {};
        switch (loc->locType.get()) {
        case RE::PackageLocation::Type::kInCell: {
            auto* cell = loc->data.object ? loc->data.object->As<RE::TESObjectCELL>() : nullptr;
            if (const char* n = cell ? cell->GetName() : nullptr; n && *n) return n;
            break;
        }
        case RE::PackageLocation::Type::kNearReference: {
            auto ref = loc->data.refHandle.get();
            if (!ref) break;
            if (auto* pc = ref->GetParentCell()) if (const char* n = pc->GetName(); n && *n) return n;
            if (const char* n = ref->GetDisplayFullName(); n && *n) return n;
            break;
        }
        default: {   // no place in the PLVD: BuildTownShops places the shop by its merchant chest's cell, so name that cell
            auto* chest = a_fac->vendorData.merchantContainer;
            if (auto* cc = chest ? chest->GetParentCell() : nullptr) if (const char* n = cc->GetName(); n && *n) return n;
            break;
        }
        }
        return {};
    }

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
                           const std::vector<ErrandShop>& a_shops, std::vector<ErrandPlanned>& a_out,
                           int* a_deferred = nullptr);   // F1: count of shops dropped this pass for a pending price refresh
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
        // Run 4 (marth 2026-10-20: routing must not be brittle): session-only, never saved. retryLater = this stop was SKIPPED for a failed walk
        // and is on the retry-later list (the inn stage retries it); laterRetries = how many of those later retries it has had (max 3).
        bool          retryLater = false;
        std::uint8_t  laterRetries = 0;
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
            bool          carriesForge = false;       // R8: a smithy: the VEND trades ore / ingots (its place has the forge furniture)
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
        // diseased or has no ACTIVE blessing: an effect whose MGEF carries MagicBlessing, read from the world) and the inn stop (LAST of
        // every trip) to a_stops; with NO shop work and no temple reason the trip is the INN ALONE (marth 2026-10-19). TRUE = a_stops
        // describes a trip. a_line gets the text appended.
        bool Augment(RE::Actor* a_follower, const FollowerState& a_state, std::chrono::steady_clock::time_point a_now,
                     const RE::BGSLocation* a_town, bool a_haveStops, std::vector<Stop>& a_stops, std::string& a_line);
        // ONE TRIP PER TOWN VISIT (marth 2026-10-19): once a follower's trip ends no new errand is planned for him until the player
        // leaves the town. Session-only; NoteTown clears every mark when the player's town changes (TryStart calls it on every tick).
        void NoteTown(RE::FormID a_townId);
        // F2 (marth 2026-10-20: ONE TRIP AT A TIME): the planner reports the player's combat state on every tick (before any early return). On the
        // combat -> no-combat edge every trip-done mark is cleared (session-only, never saved), so a FRESH plan from the current needs (inn last)
        // runs once the existing combat cooldown has passed. A combat end does not set the mark itself (End a_byCombat; the sim no longer marks).
        void NoteCombat(bool a_inCombat);
        // The blessing / disease READ (a main-thread mirror, MagicView): TRUE = this LOADED follower's mirror is missing or older than 30 s,
        // so the plan must WAIT (the probe is posted here; TryStart returns and re-plans on the next cadence, like R6's Pending). Without it
        // an inn-only trip planned before the read lands would skip the temple for the whole visit (one trip per visit). Logged once per
        // follower per town visit. FALSE for an unloaded follower (today's behaviour; logged) or when inn errands are off.
        bool MagicReadPending(RE::Actor* a_follower);
        // F4: TRUE = a data-resolved temple's entry door (a load door in the town's EXTERIOR cells, not in his own cell) is being read on the main thread
        // for a follower who would go to the temple: the plan must WAIT (like MagicReadPending; capped, then loud and planned without the temple). Augment
        // applies the read (the inn's LOCKED gate; no door found = the temple is skipped with a log line, never walked into BLOCKED).
        bool PlacesPending(RE::Actor* a_follower, const RE::BGSLocation* a_town);
        bool TripDone(RE::FormID a_follower, RE::FormID a_townId);
        // Run 5 (R3): TRUE = this town (BGSLocation id) has NO innkeeper vendor faction whose place is in one of its inns (the innkeeper catalog is empty, which
        // is also what an unverified runtime reads). Such a town has no inn stop; the planner makes the LAST SHOP the trip's rest stage (Augment) and
        // reaching it counts the trip. FALSE for a town with an innkeeper inn, or an id that is not a location.
        bool TownHasNoInnkeeper(RE::FormID a_townId);
        void UnmarkTripDone(RE::FormID a_follower, const char* a_why);   // F3: a needed shop opened -- the visit's mark goes, a new trip starts
        void MarkTripDone(RE::FormID a_follower, RE::FormID a_townId, const char* a_why);
        // Run 4 review F1: a trip that ended with its INN stop Skipped (unreachable inn). Bumps a per-(follower, visit) counter and backs the inn off
        // (2 min, then 4). Run 5 (R4): the inn is NEVER omitted for the visit any more (an unreachable inn is an error, the unstick places him). NO trip-done mark. Cleared by NoteTown / ClearTransientState.
        void NoteInnTripFailed(RE::FormID a_follower, std::chrono::steady_clock::time_point a_now);
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
        // a_noMark (F2 / F3): the end does NOT mark the visit done -- combat ended it (NoteCombat's combat-end edge lets a fresh plan run) or a needed
        // shop opened and a new trip starts (ErrandReopen.cpp).
        void End(RE::FormID a_follower, const char* a_why, bool a_tripOver, bool a_noMark = false);
        void ClearTransientState();   // revert / load (pump drained): every ledger, mark and work
        bool Working(RE::FormID a_follower);
        // The Board text: "Resting at X" / "Praying at X" ("" = no work).
        std::string Describe(RE::FormID a_follower);
        // Is he standing in the cell of this inn / temple stop? (a loaded follower, for the simulation's pick-up)
        bool InStopCell(RE::Actor* a_follower, const Stop& a_stop);
        // A cell walk ended BLOCKED: log (passive, never a decision) the load door(s) to the destination cell within his scan, with the lock level of
        // each side. Any door-open state is NOT read (BGSOpenCloseForm::GetOpenState has no VerifiedAddresses row).
        void LogBlockedDoors(RE::Actor* a_follower, RE::FormID a_destCell, const std::string& a_label);
        // Sim: a temple visit ran off screen; the blessing is OWED (logged; paid only by a physical visit).
        void NoteOwed(RE::FormID a_follower, const std::string& a_followerName, const std::string& a_temple);

        // The SHOP catalog's door read (Errand.cpp BuildCatalog), beside the temple door read it mirrors.
        // ShopDoorFor: the EXTERIOR load door nearest the player that leads into the interior cell a_cell, directly or through one interior
        // (two cells deep), read on the MAIN thread over the loaded grid and cached 5 real minutes. Found = a_pos is that door's position;
        // None = no such door (or the player is indoors); Pending = the read was posted, ask again on the next plan.
        enum class DoorAnswer { Found, None, Pending };
        DoorAnswer ShopDoorFor(RE::FormID a_cell, RE::NiPoint3& a_pos);
    }

    // ── TOWN RUN 5 (marth 2026-10-20 R1 / R4): the UNSTICK -- logistics/ErrandPlace.cpp ─────────────────────────────────────────────
    // "ALWAYS teleport a stuck follower to his destination, even in the player's view." A follower the walk retries cannot free (two BLOCKED ends
    // within 100 u, the step-out leg Blocked or moved < kStepOutMoveU) is PLACED at his current stop by Harbinger's kIntent_Place (ABI v28, ONE
    // engine move, apmf/Place.cpp): beside the ref (a vendor / a PLVD ref), or at the interior side of the cell stop's load door (the exterior
    // door's DoorTeleportData position + rotation, else beside the linked door; else beside a loaded member of the stop's faction). MFO keeps the
    // trip's ch.19 claim (Place never touches it). Own mutex (a leaf: never held across an engine or Harbinger call, never nested with Errand /
    // Sim / Rest g_mx). Session-only, nothing saved. Begin and Poll run on the worker; the cell stop's door read runs on the MAIN thread.
    namespace Unstick {
        enum class State { Idle, Pending, Placed, Failed };
        struct Result { State state = State::Idle; std::string text; };   // text: where / why (Failed: the reason, for the ERROR line)
        // Declare the placement for a_stop (the CURRENT stop). TRUE = in flight (poll it); FALSE = nothing could be requested (a_err says why).
        bool Begin(RE::FormID a_follower, const Stop& a_stop, std::string& a_err);
        Result Poll(RE::FormID a_follower);   // Pending until Harbinger's game thread answered; Placed / Failed are returned ONCE then forgotten
        void Clear(RE::FormID a_follower);
        void ClearAll();                      // revert / load
    }

    // ── F3 (marth 2026-10-20: ONE TRIP AT A TIME): the SHOP-OPENS re-plan -- logistics/ErrandReopen.cpp ───────────────────────────────────
    // BuildCatalog drops a vendor faction that is closed for HOURS only (OpenAt false; never a fence / spouse / conditioned drop). The planner
    // records those shops (candidates + the orders EconomyErrandPlan built for them NOW, while the follower was loaded) with the trip it commits:
    // the WATCH. While the visit's mark stands or he is on his inn stay (on screen or REST_SIM), the watch is re-read at most once per game hour;
    // on closed -> open the recorded candidates that opened go through Trip::Plan again (no inventory is read: the same recorded needs, so the
    // loaded and the off-screen follower decide identically) and, if that yields a stop, the caller ends the stay (NO trip mark) and starts a
    // new trip: those shops, the inn LAST. No stop = those factions leave the watch. Session-only (never saved). Own mutex (a leaf).
    namespace Reopen {
        struct Plan {
            std::vector<Stop> stops;   // the opened shops in visit order, then the inn stop (state Pending, no order); stops.front() is Active
            std::string       line;    // the plan line's body
            std::vector<RE::FormID> keys;   // the opened shops' plan keys: Consume() takes them out of the watch once the trip really started
            std::vector<RE::FormID> lateForms;   // a LATE-SELL plan: the freed items it serves (re-queued by StartReopenedTrip when the start fails)
        };
        // Planner (worker, TryStart), at the commit of a trip. a_closedCands / a_closedOrders = the hours-closed shops and their orders (may be empty).
        // a_in = the plan's inputs; a_innStop = the committed trip's inn stop (null = the trip has none). MISSED SELLS (marth 2026-10-21): a_openCands = the
        // OPEN shops of the plan (a late-sell re-plan evaluates them again), a_tripFactions = the factions the committed trip visits (a gold-blocked
        // entry of a shop the new trip covers is dropped; the others survive the new commit).
        void Record(RE::FormID a_follower, RE::FormID a_town, std::vector<Trip::Cand>&& a_closedCands, std::vector<ErrandPlanned>&& a_closedOrders,
                    const Trip::Inputs& a_in, const Stop* a_innStop, std::vector<Trip::Cand>&& a_openCands, const std::vector<RE::FormID>& a_tripFactions);
        // Worker. TRUE = a_out is a new trip (the opened shops stay in the watch until Consume; a "no stop" verdict drops them here). Hour-gated (once per game hour per follower). a_from (null = the recorded one) is where he is now.
        // Also re-checks the GOLD WATCH (below) on the same hourly poll. Takes Errand g_mx then Sim g_mx (Trip::ChestCrowd): the caller holds neither.
        bool Poll(RE::FormID a_follower, RE::FormID a_town, const RE::NiPoint3* a_from, Plan& a_out);
        // MISSED SELLS (a), marth 2026-10-21: a REPORTED trade whose offered sell rows the vendor's chest gold could not pay (TradeOutcome::unsold) files the
        // shop + the unsold rows here (session-only). The hourly Poll plans a new trip for it once the chest's last-seen gold (TradeBridge::LastSeenChestGold, the
        // planner's own safe read: a native chest read is forbidden, §0.37) covers a row or the vendor's restock interval has passed. a_innStop = the trip's inn stop.
        void NoteUnsold(RE::FormID a_follower, RE::FormID a_town, const Stop& a_stop, const ErrandPlanned* a_order,
                        const TradeBridge::TradeOutcome& a_out, const Stop* a_innStop);
        // MISSED SELLS (b): items that became sell candidates mid-trip and appear in NO stop's order. a_requeue = a failed start puts them back.
        void NoteLate(RE::FormID a_follower, RE::FormID a_town, const std::vector<RE::FormID>& a_forms, bool a_requeue = false);
        // F3 (review): a late-sell trip started for these forms, so the gold entries must not send him back with rows he no longer holds.
        void DropGoldForms(RE::FormID a_follower, const std::vector<RE::FormID>& a_forms);
        struct LateJob {
            std::vector<Trip::Cand> shops;   // the plan's OPEN shops
            std::vector<RE::FormID> forms;   // the late items
            Trip::Inputs            in;
            std::optional<Stop>     inn;
        };
        // Worker, loaded follower only (the caller evaluates his inventory): TRUE = a job (the late items leave the watch; throttled to one per 5 s).
        bool TakeLate(RE::FormID a_follower, RE::FormID a_town, std::chrono::steady_clock::time_point a_now, LateJob& a_out);
        // The late items were evaluated against the open shops (a_evaluated: EconomyErrandPlan over a_job.shops, rows already cut to the late items).
        bool PlanLate(const LateJob& a_job, const std::vector<ErrandPlanned>& a_evaluated, Plan& a_out);
        // The trip started: the opened shops leave the watch. Not consumed on a failed start (queued at kMaxWalking / the ch.19 claim refused): Retry.
        void Consume(RE::FormID a_follower, const Plan& a_plan);
        void Retry(RE::FormID a_follower);   // the start failed: lift the once-per-game-hour gate so the next Poll tries again
        void ClearAll();   // the player's town changed / revert / load
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
        bool  OpenAtHour(const RE::TESFaction* a_fac, float a_hour);   // F3: the vendor faction's VENV hours (the catalog's OpenAt)
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
    // SpouseGate: a conditioned NON-fence vendor. The spouse's own store (spouse a member, a loaded vendor is the spouse): its conditions with the
    // spouse as subject. Any other (2026-10-21 F5): its conditions with the loaded vendor, else the player, as subject.
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
    // The errand ends / the stop ends: release the work claim and forget the work. Idempotent. a_releaseWork false = the HAND-OFF to the
    // simulation: the work record goes but the ch.9 work claim STANDS (it holds the unloaded follower; the simulation / Finish releases it).
    void End(RE::FormID a_follower, const char* a_why, bool a_releaseWork = true);
    bool Working(RE::FormID a_follower);
    std::string Describe(RE::FormID a_follower);   // "Working at X" ("" = no work). Any thread.
    // Off screen: the game-day cost of the SMITH_SIM stage, and its end (worker). REAL stock only: the materials carried at the plan plus
    // what the trades of this trip reported bought (NoteBought); a line is applied only when fully paid. Returns the one-line summary.
    double SimWorkDays();
    std::string SimFinish(RE::FormID a_follower, const std::string& a_followerName);
    void ClearTransientState();   // revert / load (pump drained)
    // A trade's report said these materials were bought (TradeOutcome::smithBought, physical or simulated): the off-screen stage's
    // ledger of REAL stock (it never reads an unloaded inventory). Worker.
    void NoteBought(RE::FormID a_follower, const std::vector<TradeBridge::SmithMat>& a_bought);
}
