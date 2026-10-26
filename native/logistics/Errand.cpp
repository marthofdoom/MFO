// logistics/Errand.cpp -- TOWN ERRANDS, round R1a (ClickUp 86e3t1mx7). See logistics/Errand.h for the
// contract and the R1b / R2 seams. This file owns the DECISIONS: when a follower goes shopping, which
// vendor, and the state machine of the physical errand
//
//     PLAN --claim ch.19--> WALKING --arrived--> TALKING --> GIVING --trade dispatched--> TRADING
//                                                                                  \--(bought)--> TAKING --> DONE
//     any phase --combat / player left town / follower gone / told to wait / cap / vendor gone--> END (REJOIN)
//
// The Harbinger claims (ch.19 walk, ch.1 hold, ch.12 v2 idle beats) are apmf/ErrandClaims.cpp's; the order
// build and the VEND / SkipTrade / VendorTrade chain are EconomyProbe's own code, driven in errand mode
// (Logistics_internal.h ErrandProbe) so the errand and the legacy proximity trade can never disagree on what
// a trade is.
//
// WHAT "REJOIN" MEANS: every errand claim is released and his own follow package walks him back to the
// player (visible). R1b adds the off-screen half (the simulation, the MoveTo recall); until then a follower
// who unloads simply ends the errand.
//
// THREADING: the AddTask job worker (the per-follower service tick + the pump's Sweep); ClearTransientState
// from the revert with the pump drained. g_mx guards the tables and is NEVER held across a Harbinger,
// TradeBridge or EconomyProbe call (a copy of the record is taken, acted on, and written back if the errand
// still exists).
#include "PCH.h"
#include "Errand_internal.h"
#include "Runtime.h"         // the exact-build gates (the PLVD place: PlvdPlaceVerified, Errand_internal.h)
#include "Logistics.h"          // ForEachLoadedRef (run 4: the unattended-vendor scan of every loaded cell, on the main thread)
#include "Logistics_internal.h"
#include "Config.h"
#include "Followers.h"
#include "MainThread.h"
#include "Forms.h"
#include "LocationTypes.h"
#include "Papyrus.h"         // DispatchMoveTo: the recall of a follower whose errand ended while he was unloaded
#include "Scheduler.h"      // ServiceClock(): the UNPAUSED clock the caps and the stall test run on
#include "TradeBridge.h"
#include "APMF_API.h"
#include "apmf/APMFBridge.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand {

    namespace {

        // ── constants (each sized from a real cadence, principle 9) ───────────────────────────────────
        // The service tick (kLogisticsInterval ~1 s) is the errand's step clock, so every wait below is a
        // handful of ticks. The Harbinger idle window (kIdleConfirmWindowMs 3 s) is judged by Harbinger, not here.
        constexpr float kVendorRadius   = 170.0f;   // ch.19 arrival radius around the vendor (= kArriveDist so both arrival tests agree, town run 6; APMF clamps [50,512])
        constexpr float kArriveDist     = 170.0f;   // distance backstop for an arrival the leg state has not reported yet
        constexpr float kBlockedReach   = 260.0f;   // a BLOCKED end this close to the vendor (same floor) is an arrival
        constexpr float kSameFloorDz    = 120.0f;
        constexpr float kScanRadius     = 4096.0f;  // the catalog's cell walk, around the follower (one cell is ~4096 u)
        constexpr std::size_t kMaxCands = 24;
        constexpr int   kMaxWalking     = 4;        // Harbinger has 8 ch.19 package slots, shared with loot (kMaxLootSlots = 4) and the deposit
        constexpr auto  kPlanInterval   = std::chrono::seconds(6);
        constexpr auto  kQueueRetry     = std::chrono::seconds(6);
        constexpr auto  kTalkHold       = std::chrono::milliseconds(2500);   // the talk beat, once it reads Played
        constexpr auto  kBrowseHold     = std::chrono::milliseconds(6000);   // R4: the browsing beat of a follower FOUND at the counter, once it reads Played
        constexpr auto  kGiveHold       = std::chrono::milliseconds(1500);   // the give beat, once it reads Played, before the trade dispatches
        constexpr auto  kTakeHold       = std::chrono::milliseconds(2500);
        constexpr auto  kBeatPatience   = std::chrono::seconds(8);           // a beat Harbinger has not judged by now is skipped (logged)
        constexpr auto  kDispatchPatience = std::chrono::seconds(25);        // a busy chest / a bridge that is down
        // These three run on Scheduler::ServiceClock() (UNPAUSED seconds), so a barter / map / MCM menu that
        // pauses the world never ages an errand (Lotd::SweepTrip's rule).
        constexpr double kTradeWaitSec  = 45.0;   // dispatch -> ReportTrade (VendorTrade reaps an order at 30 s)
        constexpr double kRealCapSec    = 240.0;
        // Run 4 (marth 2026-10-20: "routing is something that shouldnt be so rigid and brittle"): a blocked / failed walk RETRIES before it skips.
        constexpr int    kWalkRetries   = 3;                                      // per stop, then the stop is skipped onto the retry-later list
        constexpr int    kRetryBackoffS[kWalkRetries] = { 5, 15, 30 };            // real seconds before retry 1 / 2 / 3
        constexpr float  kStepOutMoveU  = 100.0f;                                 // moved less than this since the last block: step out first
        constexpr float  kStepOutRadius = 200.0f;                                 // the step-out walk's arrival radius around the player
        constexpr int    kStepOutSec    = 8;                                      // how long the step-out walk runs before the retry
        constexpr int    kPlaceTries    = 2;                                      // run 5 (R1): placements per stop (a repeat stuck after the first is the last)
        constexpr int    kLaterRetries  = 3;                                      // retry-later attempts per skipped stop, from the inn stage
        constexpr int    kLaterBaseSec  = 60;                                     // inn stay -> first retry-later attempt; doubles per attempt
        constexpr double kStallSec      = 8.0;    // an errand whose follower is not being serviced
        constexpr auto  kLeftTownGrace  = std::chrono::seconds(2);
        constexpr auto  kHandoffPoll    = std::chrono::milliseconds(1000);   // unloaded this long (one more poll) before he is handed to the simulation
        constexpr auto  kParkTime       = std::chrono::minutes(5);
        constexpr float kFailCooldownSec  = 45.0f;
        constexpr float kCombatCooldownSec = 30.0f;
        constexpr std::chrono::seconds kWalkBackoffBase{ 120 };   // a failed walk to a target: skip it this long, doubling per repeat ...
        constexpr std::chrono::seconds kWalkBackoffCap{ 900 };    // ... up to 15 min

        // Skyrim.esm facts, all verified with tools/esp_inspect.py on Skyrim.esm (2026-10-18).
        constexpr RE::FormID kKwPotion    = 0x0008CDEC;   // VendorItemPotion
        constexpr RE::FormID kKwWeapon    = 0x0008F958;   // VendorItemWeapon
        constexpr RE::FormID kKwArmor     = 0x0008F959;   // VendorItemArmor
        constexpr RE::FormID kKwJewelry   = 0x0008F95A;   // VendorItemJewelry
        constexpr RE::FormID kKwClothing  = 0x0008F95B;   // VendorItemClothing
        constexpr RE::FormID kKwArrow     = 0x000917E7;   // VendorItemArrow (bolts carry it too: there is no separate bolt keyword)
        constexpr RE::FormID kKwSpellTome = 0x000937A5;   // VendorItemSpellTome
        constexpr RE::FormID kKwOreIngot  = 0x000914EC;   // VendorItemOreIngot (R8: ore and ingots, smithing materials; verified in Skyrim.esm)
        constexpr RE::FormID kKwAnimalHide = 0x000914EA;  // VendorItemAnimalHide (R8: leather)
        constexpr RE::FormID kKwAnimalPart = 0x000914EB;  // VendorItemAnimalPart (R8: leather strips)
        constexpr RE::FormID kIdleTalk         = 0x000F64DD;   // DialogueGenericTalking
        constexpr RE::FormID kIdleTalkFallback = 0x000E5190;   // DialogueNeutralExpressive
        constexpr RE::FormID kIdleGive         = 0x000B5E20;   // IdleGive (field-confirmed in the museum deposit)
        constexpr RE::FormID kIdleTake         = 0x000B5E1F;   // IdleTake
        constexpr RE::FormID kIdleExamine      = 0x00075C3D;   // IdleExamine (R4 browsing; verified in Skyrim.esm with tools/esp_inspect.py, 2026-10-19)

        enum class Phase { Walking, Talking, Giving, Trading, Taking, Resting, Smithing };   // R7: Resting = the inn stay / the temple prayer (logistics/ErrandRest.cpp owns its steps)

        const char* PhaseName(Phase p) {
            switch (p) {
                case Phase::Walking: return "WALKING";
                case Phase::Talking: return "TALKING";
                case Phase::Giving:  return "GIVING";
                case Phase::Trading: return "TRADING";
                case Phase::Taking:  return "TAKING";
                case Phase::Resting: return "RESTING";
                case Phase::Smithing: return "SMITHING";   // R8: the smith stop's work (logistics/ErrandSmith.cpp owns its steps)
            }
            return "?";
        }

        // One errand per follower. Saved by R2's 'ERND' record (SnapshotForSave below: identities, stage and flags, never the order).
        struct Rec {
            Phase         phase = Phase::Walking;
            RE::FormID    vendor = 0;          // the vendor ACTOR
            RE::FormID    chest = 0;
            RE::FormID    town = 0;            // the BGSLocation the errand belongs to (the player's town when it began)
            std::string   followerName, vendorName;
            Clock::time_point start{}, phaseAt{}, townLostSince{}, confirmedAt{};
            double        startClk = 0.0, lastTickClk = 0.0, dispatchedClk = 0.0;   // Scheduler::ServiceClock() seconds
            float         startDays = 0.0f;
            bool          beatRequested = false;   // the CURRENT beat's idle was requested
            bool          talkFallbackUsed = false;
            bool          dispatched = false;
            bool          bought = false;
            bool          browse = false;          // R4: found at the counter -- the arrival plays the browsing beat (runtime only, ends with the record; R5 LoadStop must reset it)
            bool          announced = false;       // bTownToasts: the "is trading with" line fired (not saved; a resume sets it from the sim stage)
            // R1b: what the SIMULATION needs if he goes off screen, and what a hand-off back to the physical errand needs.
            RE::FormID    faction = 0;                      // the vendor faction of the shop
            DestKind      destKind = DestKind::VendorActor; // what the ch.19 leg walks to NOW
            RE::FormID    destForm = 0;
            float         destRad = 0.0f;
            std::shared_ptr<const ErrandPlanned> order;     // the trade, built at PLAN time while he was loaded
            RE::NiPoint3  lastPos{};                        // the follower's last LOADED position / cell (sampled every tick and lap)
            RE::NiPoint3  planPos{};                        // where he stood at PLAN (log only: the ARRIVED line's distance covered)
            bool          planPosSet = false;
            RE::FormID    lastCell = 0;
            Clock::time_point unloadedSince{};              // first lap he read unloaded (the hand-off waits one more poll)
            // R5: the trip. The single-shop fields above (vendor, chest, faction, vendorName, dest*, order) are stops[cur]'s working
            // copy (LoadStop / SyncCur). tripPurse: the follower's gold at the plan (-1 = none: a resumed restored trip, live gold);
            // tripSpent: what this trip's trades have spent so far (the purse cap of the next stop).
            std::vector<Stop> stops;
            std::uint8_t  cur = 0;
            int           tripPurse = -1, tripSpent = 0;
            // Run 4: the CURRENT stop's walk-retry state (session-only, reset by LoadStop). A failed walk waits retryAt (5/15/30 s), then
            // re-points the same ch.19 claim; needStepOut = he did not move since the last block (first walk toward the player).
            int           walkRetries = 0;
            bool          retryWait = false, retryRepoint = false, stepOut = false, needStepOut = false, haveBlockPos = false;
            Clock::time_point retryAt{}, laterAt{};   // laterAt: the inn stay's next retry-later attempt
            RE::NiPoint3  lastBlockPos{};
            // Run 5 (R1 / R4): the UNSTICK. stepOutOk = the step-out walk was really declared (its result is read when its wait ends); stepOutFrom =
            // where he stood when it was declared; placing = a Harbinger kIntent_Place is in flight (ErrandPlace.cpp); placeTries = placements made for
            // this stop (cap kPlaceTries). Session-only, reset by LoadStop.
            bool          stepOutOk = false, placing = false;
            int           placeTries = 0;
            RE::NiPoint3  stepOutFrom{};
            // mfo-272: times this stop's walk was re-aimed at its vendor NPC's CURRENT spot (Vendors::Locate; cap kMaxRelocations). Session-only, reset by LoadStop.
            int           relocations = 0;
        };

        std::mutex                                           g_mx;
        std::unordered_map<RE::FormID, Rec>                  g_recs;
        std::unordered_map<RE::FormID, Clock::time_point>    g_cool;        // follower -> next errand allowed
        std::unordered_map<RE::FormID, Clock::time_point>    g_nextPlan;    // follower -> next plan evaluation
        std::unordered_map<RE::FormID, Clock::time_point>    g_parked;      // vendor actor -> parked until (a static-blocked walk)
        // (follower, shop chest) -> backoff after a FAILED WALK (session-scoped, not saved; cleared in ClearTransientState). The chest keys
        // the shop for both a loaded vendor actor and a faction-only shop, so the walk's two phases (place, then vendor) share one key.
        struct WalkBackoff { Clock::time_point until{}; int fails = 0; };
        std::unordered_map<std::uint64_t, WalkBackoff>       g_backoff;
        std::unordered_map<RE::FormID, std::string>          g_backoffLogged;   // follower -> the skipped-chest set last logged (the skip line fires on a CHANGE only)
        // Run 4: the main-thread scan of every loaded cell for one vendor faction (PostVendorScan); awakeLoaded = a member who is alive, enabled, loaded and awake.
        struct VendorScan { float hour = -1.0f; int members = 0; RE::FormID awakeLoaded = 0; std::string summary; };
        std::unordered_map<RE::FormID, VendorScan>           g_vendorScan;
        std::unordered_map<RE::FormID, std::string>          g_vendorNote;   // mfo-272: vendor faction -> the NPC-first verdict last logged (said on a CHANGE only)
        // mfo-272: the catalog's NPC-first verdict for one shop, said when it CHANGES (the plan runs every 6 s per follower).
        void NoteVendorOnce(RE::FormID a_faction, std::string a_text) {
            {
                std::scoped_lock lk(g_mx);
                auto& last = g_vendorNote[a_faction];
                if (last == a_text) return;
                last = a_text;
            }
            spdlog::info("[errand] vendor (NPC-first): {}", a_text);
        }
        std::uint64_t BackoffKey(RE::FormID a_fid, RE::FormID a_chest) { return (static_cast<std::uint64_t>(a_fid) << 32) | a_chest; }
        RE::FormID                                           g_townId = 0;  // the town the player has been in ...
        Clock::time_point                                    g_townSince{}; // ... since this instant
        std::atomic<bool>                                    g_latched{ false };   // Harbinger refused the idle seat: errands INERT this session

        // ── small helpers ─────────────────────────────────────────────────────────────────────────────
        const char* LegName(std::uint32_t s) {
            switch (s) {
                case APMF_API::kLeg_None:            return "NONE";
                case APMF_API::kLeg_Pending:         return "PENDING";
                case APMF_API::kLeg_Walking:         return "WALKING";
                case APMF_API::kLeg_Arrived:         return "ARRIVED";
                case APMF_API::kLeg_Blocked:         return "BLOCKED";
                case APMF_API::kLeg_CombatCancelled: return "COMBAT-CANCELLED";
                case APMF_API::kLeg_DestGone:        return "DEST-GONE";
                case APMF_API::kLeg_StuckTimeout:    return "STUCK-TIMEOUT";
                case APMF_API::kLeg_ActorGone:       return "ACTOR-GONE";
                case APMF_API::kLeg_Failed:          return "FAILED";
                case APMF_API::kLeg_Released:        return "RELEASED";
                default:                             return "?";
            }
        }
        // Town run 6: the Harbinger leg state an errand re-point leaves behind (a live WALKING leg is re-pointed in place).
        void LogRepointFrom(RE::FormID a_fid, const char* a_site) {
            APMFBridge::LootLegState l;
            if (APMFBridge::ReadErrandLeg(a_fid, l) && l.ours)
                spdlog::info("[errand] {:08X}: {} -- re-pointing from Harbinger leg {} (dest {:08X}, {} ms in state)", a_fid, a_site, LegName(l.state), l.dest, l.msInState);
            else
                spdlog::info("[errand] {:08X}: {} -- re-pointing with no leg of ours standing", a_fid, a_site);
        }

        const char* NameOf(const RE::Actor* a) {
            const char* n = a ? a->GetName() : nullptr;
            return n && *n ? n : "?";
        }

        std::string TownName(RE::FormID a_id) {
            auto* loc = RE::TESForm::LookupByID<RE::BGSLocation>(a_id);
            const char* n = loc ? loc->GetName() : nullptr;
            return n && *n ? n : std::format("{:08X}", a_id);
        }

        bool InLoadingMenu() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }

        // The vendor faction the legacy scan also picks: the first one that IsVendor && OffersServices with
        // rank >= 0 (a negative rank is a runtime removal).
        RE::TESFaction* VendorFactionOf(RE::Actor* a_actor) {
            RE::TESFaction* fac = nullptr;
            a_actor->VisitFactions([&](RE::TESFaction* a_fac, std::int8_t a_rank) {
                if (!a_fac || a_rank < 0) return false;
                if (a_fac->IsVendor() && a_fac->OffersServices()) { fac = a_fac; return true; }
                return false;
            });
            return fac;
        }

        // VENV hours: start == end, or 0-24, means always open.
        bool OpenAt(const RE::TESFaction* a_fac, float a_hour) {
            const float s = static_cast<float>(a_fac->vendorData.vendorValues.startHour);
            const float e = static_cast<float>(a_fac->vendorData.vendorValues.endHour);
            if (s == e || (s <= 0.0f && e >= 24.0f)) return true;
            return s < e ? (a_hour >= s && a_hour < e) : (a_hour >= s || a_hour < e);
        }

        // Does this vendor's VEND list trade the keyword (same inclusion / exclusion rule as VendorTrades)?
        bool ListTrades(const RE::TESFaction* a_fac, RE::FormID a_kw) {
            auto* vend = a_fac->vendorData.vendorSellBuyList;
            const bool notBuySell = a_fac->vendorData.vendorValues.notBuySell;
            if (!vend) return notBuySell;
            bool match = false;
            for (auto* f : vend->forms)
                if (f && f->GetFormID() == a_kw) { match = true; break; }
            return notBuySell ? !match : match;
        }

        // MISSED BUY (2026-10-26): is this shop an APOTHECARY-class vendor, from engine data at runtime? A VEND list names what a vendor BUYS, so
        // "VendorItemPotion in the list" says nothing about stock (Ri'saad's and Belethor's lists carry it; neither stocked a qualifying potion).
        // Apothecary class = a plain (non-inverted) VEND list that carries VendorItemPotion and whose keywords are at least half alchemy goods
        // (potion, poison, ingredient, food: matched by the keyword's editor id, VendorItemPotion by its verified FormID). Out-param a_known
        // false = no keyword in the list has a readable editor id (stripped by the runtime): the class cannot be judged, the caller keeps the
        // pre-fix behaviour (the VEND keyword alone) rather than refusing every shop.
        bool ApothecaryClass(const RE::TESFaction* a_fac, bool& a_known) {
            a_known = false;
            auto* vend = a_fac ? a_fac->vendorData.vendorSellBuyList : nullptr;
            if (a_fac && a_fac->vendorData.vendorValues.notBuySell) { a_known = true; return false; }   // an inverted list = "buys everything but": judged, NOT apothecary (never "unknown", review F1)
            if (!vend) return false;
            int total = 0, alchemy = 0;
            bool hasPotion = false;
            for (auto* f : vend->forms) {
                if (!f) continue;
                ++total;
                if (f->GetFormID() == kKwPotion) { hasPotion = true; ++alchemy; a_known = true; continue; }
                if (f->GetFormID() == 0x0008CDEAu || f->GetFormID() == 0x000A0E56u) { ++alchemy; a_known = true; continue; }   // VendorItemFood / VendorItemFoodRaw (Skyrim.esm, as ErrandRest.cpp)
                const char* ed = f->GetFormEditorID();
                if (!ed || !*ed) continue;
                a_known = true;
                const std::string_view e{ ed };
                if (e == "VendorItemPoison" || e == "VendorItemIngredient") ++alchemy;
            }
            return hasPotion && total > 0 && alchemy * 2 >= total;
        }

        // ── the catalog (R1a: loaded vendor actors) ───────────────────────────────────────────────────
        // SEAM: `place` (the faction's PLVD cell / reference) is where an UNLOADED shop attaches in R1b. R1a does
        // not read it: PackageLocation::Data is a union of TESForm* and ObjectRefHandle and which member a
        // faction's file-loaded PLVD fills is a per-runtime fact (R1b reads it behind PlvdPlaceVerified), so R1a takes the place
        // from the loaded vendor ACTOR.
        struct Cand {
            RE::ActorHandle  handle;            // empty = a shop whose vendor is not loaded (an interior)
            RE::FormID       id = 0;            // the plan key: the vendor ACTOR's FormID, or the FACTION's for a faction-only shop
            RE::FormID       actorId = 0;       // 0 = no loaded vendor
            RE::TESFaction*  fac = nullptr;
            RE::FormID       chest = 0;
            float            dist = 0.0f;       // player -> vendor / place, straight line (0 for a cell: no position)
            DestKind         kind = DestKind::VendorActor;
            RE::FormID       destForm = 0;      // what ch.19 walks to: the vendor actor, the PLVD ref, or the PLVD cell
            float            destRad = 0.0f;
            RE::NiPoint3     pos{};             // R5: where the place is (the vendor / the ref / a cell's nearest loaded door), for the trip order
            bool             havePos = false;
            // mfo-272 review F2: the shop PLACE (Trip::Cand::placeKind ..., the smith stop's destination). placeForm 0 = none known.
            DestKind         placeKind = DestKind::Cell;
            RE::FormID       placeForm = 0;
            float            placeRad = 0.0f;
            RE::NiPoint3     placePos{};
            bool             havePlacePos = false;
        };

        // R1b: the town's shops from the vendor FACTIONS (design 2.2), cached per town -- immutable once built. The place is the
        // faction's PLVD vendorLocation, read the way the engine's loader leaves it: TESFaction::Load builds the PackageLocation,
        // InitItem's resolver then turns kInCell into a TESObjectCELL* in data.object and kNearReference into a 32-bit
        // ObjectRefHandle in data.refHandle. Other location types are skipped. Disassembly-verified on 1.6.1170, 1.5.97 and
        // 1.7.104, each on its own addresses (PlvdPlaceVerified, Errand_internal.h; ENGINE_NOTES §0.52): any other build gets
        // an empty list and shops are the loaded-vendor ones (R1a).
        // chestAnchor (marth 2026-10-21: the store is where its merchant chest is; the vendor moves about): a faction whose PLVD gives no place
        // (any other type, 'near self' included, or none) is placed by its merchant CHEST's parent cell -- the store cell, walked to like any shop
        // cell; on arrival the vendor search finds wherever the vendor stands. A chest in an exterior cell is walked to as a ref.
        struct ShopStatic { RE::FormID faction = 0, chest = 0, place = 0; DestKind kind = DestKind::Cell; float rad = 0.0f;
                            std::uint8_t cls = 0;   // R6: 0 plain, 1 FENCE (perk + conditions gate), 2 CONDITIONED (a spouse-store candidate)
                            bool chestAnchor = false; };
        std::unordered_map<RE::FormID, std::vector<ShopStatic>> g_townShops;   // town BGSLocation id -> shops; guarded by g_mx

        // Is this vendor faction's shop placed by its merchant chest (BuildTownShops' rule: no PLVD, or a PLVD type that is neither kInCell
        // nor kNearReference)? Recomputed from the faction (nothing saved), so a restored stop reads the same answer.
        bool ChestPlacedFaction(RE::FormID a_fac) {
            auto* fac = a_fac ? RE::TESForm::LookupByID<RE::TESFaction>(a_fac) : nullptr;
            if (!fac || !PlvdPlaceVerified()) return false;
            auto* loc = fac->vendorData.vendorLocation;
            if (!loc) return true;
            const auto t = loc->locType.get();
            return t != RE::PackageLocation::Type::kInCell && t != RE::PackageLocation::Type::kNearReference;
        }

        std::vector<ShopStatic> BuildTownShops(const RE::BGSLocation* a_town) {
            std::vector<ShopStatic> out;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh || !a_town) return out;
            int seen = 0, byChest = 0, byChestHere = 0;
            for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
                if (!fac || !fac->IsVendor() || !fac->OffersServices()) continue;
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;
                // R6: a fence and a conditioned vendor are KEPT, tagged; BuildCatalog's gates (ErrandShops.cpp) decide per follower.
                const bool conditioned = fac->vendorData.vendorConditions && fac->vendorData.vendorConditions->head;
                const std::uint8_t cls = ErrandShops::IsFence(fac) ? 1 : conditioned ? 2 : 0;
                auto* loc = fac->vendorData.vendorLocation;
                ShopStatic sh;
                sh.faction = fac->GetFormID();
                sh.chest   = chest->GetFormID();
                sh.cls     = cls;
                const RE::BGSLocation* placeLoc = nullptr;
                const auto type = loc ? loc->locType.get() : RE::PackageLocation::Type::kNearSelf;   // no PLVD = no place in the record either
                if (type == RE::PackageLocation::Type::kInCell) {
                    auto* cell = loc->data.object ? loc->data.object->As<RE::TESObjectCELL>() : nullptr;
                    if (!cell) continue;
                    sh.kind = DestKind::Cell; sh.place = cell->GetFormID(); placeLoc = cell->GetLocation();
                } else if (type == RE::PackageLocation::Type::kNearReference) {
                    auto ref = loc->data.refHandle.get();
                    if (!ref) continue;
                    sh.kind = DestKind::Ref; sh.place = ref->GetFormID();
                    sh.rad = std::clamp(static_cast<float>(loc->rad), 50.0f, 512.0f);   // ch.19 clamps the same range
                    placeLoc = ref->GetEditorLocation();
                    if (!placeLoc) placeLoc = ref->GetCurrentLocation();
                } else {
                    // THE MERCHANT CHEST (was: skipped -- the College's tome vendors and Farengar are 'near self'). The chest is a persistent ref
                    // (VENC) whose parent cell is kept while the cell is unloaded (the same read ShopPlaceName and the temple door read make).
                    auto* cc = chest->GetParentCell();
                    if (!cc) continue;
                    if (cc->IsInteriorCell()) {
                        sh.kind = DestKind::Cell; sh.place = cc->GetFormID(); placeLoc = cc->GetLocation();
                    } else {   // an exterior chest (a stall): the chest ref is the place
                        sh.kind = DestKind::Ref; sh.place = chest->GetFormID(); sh.rad = 256.0f;
                        placeLoc = chest->GetEditorLocation();
                        if (!placeLoc) placeLoc = chest->GetCurrentLocation();
                    }
                    sh.chestAnchor = true;
                    ++byChest;
                }
                ++seen;
                if (placeLoc && LocationTypes::TownAnchor(placeLoc) == a_town) { out.push_back(sh); if (sh.chestAnchor) ++byChestHere; }
            }
            spdlog::info("[errand] shop catalog for '{}': {} of {} vendor faction(s) with a place sit in this town ({} of them placed by their merchant chest's "
                         "cell, of {} chest-placed faction(s) in the game: no place in the PLVD)",
                         a_town->GetName() ? a_town->GetName() : "?", out.size(), seen, byChestHere, byChest);
            return out;
        }

        // F3: a_closedOut (when given) also collects the shops dropped ONLY for their hours (OpenAt false; the fence / spouse / condition gates ran
        // BEFORE the hours test, so none of those is here) for the shop WATCH (ErrandReopen.cpp). a_dropped counts them as before.
        // a_doorWait (when given) is set when the empty result means "an interior shop's entry-door read is in flight" (Rest::ShopDoorFor), so
        // the caller abandons this plan instead of committing the INN-ONLY trip of a visit that has shops (one trip per visit).
        std::vector<Cand> BuildCatalog(RE::Actor* a_follower, const RE::BGSLocation* a_playerTown,
                                       Clock::time_point a_now, int& a_dropped, std::vector<Cand>* a_closedOut = nullptr, bool* a_doorWait = nullptr) {
            std::vector<Cand> out;
            a_dropped = 0;
            if (a_doorWait) *a_doorWait = false;
            if (a_closedOut) a_closedOut->clear();
            auto* cell = a_follower->GetParentCell();
            auto* pc   = RE::PlayerCharacter::GetSingleton();
            auto* cal  = RE::Calendar::GetSingleton();
            if (!cell || !cell->IsAttached() || !pc || !cal) return out;

            // COLLECT-THEN-ACT (#2): handles only inside the walk, then the reads.
            std::vector<RE::ActorHandle> living;
            cell->ForEachReferenceInRange(a_follower->GetPosition(), kScanRadius,
                [&](RE::TESObjectREFR* a_ref) {
                    if (living.size() >= kMaxCands * 4) return RE::BSContainer::ForEachResult::kStop;
                    auto* actor = a_ref->As<RE::Actor>();
                    if (!actor || actor == a_follower || actor == pc || actor->IsDead() || actor->IsDisabled() ||
                        actor->IsMarkedForDeletion() || actor->IsPlayerTeammate())   // a teammate is never a merchant (the Auri CTD guard)
                        return RE::BSContainer::ForEachResult::kContinue;
                    living.push_back(actor->GetHandle());
                    return RE::BSContainer::ForEachResult::kContinue;
                });

            const float hour = cal->GetHour();
            std::unordered_map<RE::FormID, Clock::time_point> parked;
            {
                std::scoped_lock lk(g_mx);   // a locked copy: Sweep / Tick write g_parked under g_mx (threading carve-out)
                parked = g_parked;
            }
            std::unordered_map<RE::FormID, Clock::time_point> backedOff;   // this follower's failed-walk backoffs: chest -> until
            {
                std::scoped_lock lk(g_mx);
                const auto fid32 = a_follower->GetFormID();
                for (const auto& [k, b] : g_backoff)
                    if (static_cast<RE::FormID>(k >> 32) == fid32 && a_now < b.until) backedOff[static_cast<RE::FormID>(k & 0xFFFFFFFFu)] = b.until;
            }
            std::string backoffSkipped;   // said ONCE per plan
            auto noteBackoff = [&](RE::FormID a_chest, const char* a_name) {
                if (!backoffSkipped.empty()) backoffSkipped += ", ";
                backoffSkipped += std::format("'{}' (chest {:08X})", a_name && *a_name ? a_name : "?", a_chest);
            };
            std::unordered_map<RE::FormID, bool> seenChest;
            bool waiting = false;   // R6: a fence / spouse gate is still waiting for its main-thread condition answer
            bool doorWaiting = false;   // an interior shop's entry-door read (Rest::ShopDoorFor) was just posted to the main thread
            bool vendorWaiting = false;   // mfo-272: the vendor index (Vendors::Locate) is being built on the main thread
            std::unordered_set<RE::FormID> gated;   // R6: factions a LOADED vendor already put through a gate this plan (the faction-only list does not re-ask)
            for (auto& h : living) {
                if (out.size() >= kMaxCands) break;
                auto  ptr = h.get();
                auto* v   = ptr.get();
                if (!v || !v->Is3DLoaded() || APMFBridge::InCombat(v)) continue;
                if (const auto s = v->AsActorState()->GetSitSleepState(); s == RE::SIT_SLEEP_STATE::kIsSleeping ||
                                                           s == RE::SIT_SLEEP_STATE::kWantToSleep ||
                                                           s == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim)
                    continue;   // a sleeper cannot be talked to
                auto* fac = VendorFactionOf(v);
                if (!fac) continue;
                // R6 (was: every conditioned vendor and every fence dropped, marth 2026-10-18 Q10): a FENCE needs the follower's fence
                // perk AND its own conditions (Q1); a conditioned non-fence vendor is the SPOUSE's store when the spouse is a member, else
                // (marth 2026-10-21 F5) its own conditions decide. The gates are ErrandShops.cpp's (conditions are evaluated on the main thread).
                {
                    const bool conditioned = fac->vendorData.vendorConditions && fac->vendorData.vendorConditions->head;
                    const bool fence = ErrandShops::IsFence(fac);
                    if (fence || conditioned) {
                        gated.insert(fac->GetFormID());
                        const auto g = fence ? ErrandShops::FenceGate(a_follower, fac, h) : ErrandShops::SpouseGate(a_follower, fac, h);
                        if (g == ErrandShops::Gate::Closed) { ++a_dropped; continue; }
                        if (g == ErrandShops::Gate::Pending) { waiting = true; continue; }   // the main-thread evaluation is in flight: this plan is abandoned below
                    }
                }
                const bool open = OpenAt(fac, hour);
                if (!open) { ++a_dropped; if (!a_closedOut) continue; }   // F3: a closed shop goes on, to the watch list below
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;   // a malformed merchant link (see EconomyProbe)
                // The same town as the player (a roadside vendor outside any town is not an errand destination).
                const auto* vTown = LocationTypes::TownAnchor(v->GetCurrentLocation());
                if (!vTown || vTown != a_playerTown) { if (open) ++a_dropped; continue; }
                const float dPlayer = v->GetPosition().GetDistance(pc->GetPosition());
                // v2.7.1: no travel-radius gate on a TOWN shop (run 6: Arcadia / Belethor lay beyond fTravelRadius yet are in town). The town test above is the gate.
                if (const auto it = parked.find(v->GetFormID()); it != parked.end() && a_now < it->second) continue;
                if (backedOff.contains(chest->GetFormID())) { noteBackoff(chest->GetFormID(), v->GetName()); continue; }
                if (seenChest.contains(chest->GetFormID())) continue;   // one actor per chest is enough
                // MISSED BUY fix (2026-10-26, the Ahlam hole): a LOADED vendor of an OPEN shop goes through the SAME "would trade now" gate the located
                // (unloaded) path uses (Vendors::Locate: shop-relative JobMerchantFaction + GetOffersServicesNow). It runs BEFORE the chest is marked seen, so a
                // refused stand-in does not hide the real vendor of the same chest behind it. A closed shop is not gated (it only feeds the reopen watch).
                if (open) {
                    const auto offer = Vendors::WouldTrade(v, fac->GetFormID());
                    if (offer == Vendors::Offer::Pending) { vendorWaiting = true; continue; }
                    if (offer == Vendors::Offer::No) { ++a_dropped; continue; }
                }
                seenChest[chest->GetFormID()] = true;
                Cand c;
                c.handle = h;
                c.id     = v->GetFormID();
                c.actorId = c.id;
                c.fac    = fac;
                c.chest  = chest->GetFormID();
                c.dist   = dPlayer;
                c.kind   = DestKind::VendorActor;
                c.destForm = c.id;
                c.destRad = kVendorRadius;
                c.pos = v->GetPosition(); c.havePos = true;
                c.placeKind = DestKind::VendorActor; c.placeForm = c.id; c.placeRad = kVendorRadius; c.placePos = c.pos; c.havePlacePos = true;   // review F2: a loaded vendor is his own place (the smith stop reaches him as a ref, as before mfo-272)
                (open ? out : *a_closedOut).push_back(c);
            }

            // R1b: shops whose vendor is NOT loaded (an interior while the player is outside), from the faction catalog.
            if (a_playerTown && PlvdPlaceVerified()) {
                // A shop that is a CELL has no position: its distance (and the travel-radius gate) is that of the nearest LOADED load
                // door whose destination door sits in that cell. No such door in the follower's loaded cell = the shop is skipped
                // (nothing to estimate the walk from, and no proof the player's area leads there).
                std::unordered_map<RE::FormID, float> cellDoorDist;
                std::unordered_map<RE::FormID, RE::NiPoint3> cellDoorPos;   // R5: that door's position (the trip order)
                {
                    std::vector<RE::ObjectRefHandle> doors;
                    cell->ForEachReferenceInRange(a_follower->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* a_ref) {
                        if (doors.size() < 64 && a_ref->extraList.GetByType<RE::ExtraTeleport>()) doors.push_back(a_ref->GetHandle());
                        return RE::BSContainer::ForEachResult::kContinue;
                    });
                    for (auto& dh : doors) {
                        auto dp = dh.get();
                        auto* d = dp.get();
                        auto* tele = d ? d->extraList.GetByType<RE::ExtraTeleport>() : nullptr;
                        if (!tele || !tele->teleportData) continue;
                        auto lp = tele->teleportData->linkedDoor.get();
                        auto* lc = lp.get() ? lp.get()->GetParentCell() : nullptr;
                        if (!lc) continue;
                        const float dd = d->GetPosition().GetDistance(pc->GetPosition());
                        if (const auto it = cellDoorDist.find(lc->GetFormID()); it == cellDoorDist.end() || dd < it->second) {
                            cellDoorDist[lc->GetFormID()] = dd;
                            cellDoorPos[lc->GetFormID()] = d->GetPosition();
                        }
                    }
                }
                std::vector<Cand> catalogOpen;
                std::vector<ShopStatic> shops;
                bool have = false;
                {
                    std::scoped_lock lk(g_mx);
                    if (const auto it = g_townShops.find(a_playerTown->GetFormID()); it != g_townShops.end()) { shops = it->second; have = true; }
                }
                if (!have) {
                    shops = BuildTownShops(a_playerTown);   // a one-time walk of the faction array, outside the lock
                    std::scoped_lock lk(g_mx);
                    g_townShops[a_playerTown->GetFormID()] = shops;
                }
                for (const auto& sh : shops) {
                    if (seenChest.contains(sh.chest)) continue;   // a loaded vendor already covers this chest
                    auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(sh.faction);
                    if (!fac) continue;
                    if (sh.cls != 0 && !gated.contains(sh.faction)) {   // R6: a fence / conditioned shop with no loaded vendor: subject = the player / the spouse
                        const auto g = sh.cls == 1 ? ErrandShops::FenceGate(a_follower, fac, RE::ActorHandle{})
                                                   : ErrandShops::SpouseGate(a_follower, fac, RE::ActorHandle{});
                        if (g == ErrandShops::Gate::Closed) { ++a_dropped; continue; }
                        if (g == ErrandShops::Gate::Pending) { waiting = true; continue; }
                    } else if (sh.cls != 0) continue;   // its loaded vendor was gated above (and refused or was already listed)
                    const bool open = OpenAt(fac, hour);
                    if (!open) { ++a_dropped; if (!a_closedOut) continue; }   // F3: a closed shop goes on, to the watch list below
                    if (const auto it = parked.find(sh.faction); it != parked.end() && a_now < it->second) continue;
                    if (backedOff.contains(sh.chest)) { noteBackoff(sh.chest, fac->GetName()); continue; }
                    float d = 0.0f;
                    RE::NiPoint3 placePos{};
                    // An INTERIOR place's distance + position: the nearest loaded load door into it in the follower's own cell, else (marth
                    // 2026-10-21: "they check 2 cells deep ... just through doors") the main-thread read of the loaded EXTERIOR grid, one or two
                    // cells deep (Rest::ShopDoorFor). Run 6: Belethor's and Arcadia's doors stand in the market cells NEXT to the follower's, so
                    // the own-cell walk alone never saw them. Pending = the read was just posted: the plan waits (re-planned shortly).
                    auto interiorPlace = [&](RE::FormID a_cell) -> int {   // 1 = d / placePos set, 0 = no door known, -1 = pending
                        if (const auto it = cellDoorDist.find(a_cell); it != cellDoorDist.end()) { d = it->second; placePos = cellDoorPos[a_cell]; return 1; }
                        RE::NiPoint3 dp{};
                        switch (Rest::ShopDoorFor(a_cell, dp)) {
                        case Rest::DoorAnswer::Found:   placePos = dp; d = dp.GetDistance(pc->GetPosition()); return 1;
                        case Rest::DoorAnswer::Pending: return -1;
                        default:                        return 0;
                        }
                    };
                    // The shop's PLACE (the pre-272 destination: the PLVD ref / cell, or the merchant chest's cell), kept as context (review F2):
                    // the SMITH stop walks to it, and so does a shop whose vendor is not in memory (review F1b). 1 = known, 0 = none, -1 = door read pending.
                    int placeOk = 1;
                    RE::NiPoint3 ppos{};
                    float pd = 0.0f;
                    if (sh.kind == DestKind::Ref) {
                        if (auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sh.place); !ref) {
                            placeOk = 0;
                        } else {
                            ppos = ref->GetPosition();
                            pd = ppos.GetDistance(pc->GetPosition());
                            // An INTERIOR ref has no comparable position: reach it through its cell's door like a Cell shop.
                            if (auto* rc = ref->GetParentCell(); rc && rc->IsInteriorCell()) {
                                placeOk = interiorPlace(rc->GetFormID());
                                if (placeOk == 1) { ppos = placePos; pd = d; }
                            }
                        }
                    } else {
                        placeOk = interiorPlace(sh.place);
                        if (placeOk == 1) { ppos = placePos; pd = d; }
                    }
                    if (placeOk < 0) { doorWaiting = true; continue; }
                    // mfo-272 NPC-FIRST (marth 2026-10-25: "who runs this shop, is the shop open, where is that NPC"): the walk goes to the shop's
                    // VENDOR wherever the engine has them now (the place keeps the shop's town membership and the smith stop; the chest is still what
                    // the trade moves goods through). Every member dead or disabled = no shop.
                    Vendors::Where vw;
                    const auto va = Vendors::Locate(sh.faction, vw);
                    if (va == Vendors::Answer::Pending) { vendorWaiting = true; continue; }
                    Cand c;
                    c.id = sh.faction; c.fac = fac; c.chest = sh.chest;
                    if (placeOk == 1) { c.placeKind = sh.kind; c.placeForm = sh.place; c.placeRad = sh.rad; c.placePos = ppos; c.havePlacePos = true; }
                    if (va == Vendors::Answer::NoMembers) {
                        // review F1b (decided): NoMembers = nobody of the faction in MEMORY (a temporary vendor whose cell is not loaded), not a dead
                        // one -- the pre-272 walk to the shop's place, the vendor confirmed in person on arrival (the NPC-first relocate applies there).
                        if (placeOk != 1) { if (open) ++a_dropped; continue; }
                        NoteVendorOnce(sh.faction, std::format("shop '{}' (faction {:08X}): no member in memory -> walk to its PLACE {:08X}, the vendor is found on arrival",
                                                               ShopPlaceName(fac), sh.faction, sh.place));
                        seenChest[sh.chest] = true;
                        c.dist = pd; c.kind = sh.kind; c.destForm = sh.place; c.destRad = sh.rad;
                        c.pos = ppos; c.havePos = true;
                        (open ? catalogOpen : *a_closedOut).push_back(c);
                        continue;
                    }
                    if (va != Vendors::Answer::Located) {
                        NoteVendorOnce(sh.faction, std::format("shop '{}' (faction {:08X}) is NOT a candidate: {} (members {}, dead {}, disabled {}, no cell {})",
                                                               ShopPlaceName(fac), sh.faction, Vendors::AnswerName(va), vw.members, vw.dead, vw.disabled, vw.noCell));
                        if (open) ++a_dropped;
                        continue;
                    }
                    // review F3: "away" / "asleep" drop an OPEN shop only; a closed shop still reaches the reopen watch (a_closedOut).
                    if (open && vw.town != a_playerTown) {
                        NoteVendorOnce(sh.faction, std::format("shop '{}' (faction {:08X}) is NOT a candidate this plan: its vendor {} is not in this town",
                                                               ShopPlaceName(fac), sh.faction, Vendors::Describe(vw)));
                        ++a_dropped;
                        continue;
                    }
                    if (open && vw.asleep) {
                        NoteVendorOnce(sh.faction, std::format("shop '{}' (faction {:08X}) is NOT a candidate this plan: its vendor {} is asleep",
                                                               ShopPlaceName(fac), sh.faction, Vendors::Describe(vw)));
                        ++a_dropped;
                        continue;
                    }
                    DestKind vkind = DestKind::VendorActor;
                    RE::FormID vdest = vw.actor;
                    float vrad = kVendorRadius;
                    const bool sameSpace = vw.loaded && (vw.cell == cell->GetFormID() || (!vw.interior && !cell->IsInteriorCell()));
                    if (sameSpace) {   // loaded where he is (or both in the exterior grid): walk straight to the vendor
                        placePos = vw.pos;
                        d = placePos.GetDistance(pc->GetPosition());
                    } else if (vw.interior && !APMFBridge::ErrandCrossCellsSupported()) {
                        // ANOTHER cell, an interior: the two-leg road the current Harbinger supports -- a CELL leg into the vendor's cell (the
                        // engine paths through the load doors natively; arrival = parent-cell identity), then a ref leg to the vendor inside it.
                        vkind = DestKind::Cell; vdest = vw.cell; vrad = 0.0f;
                        const int ir = interiorPlace(vw.cell);
                        if (ir < 0) { doorWaiting = true; continue; }
                        if (ir == 0) { if (open) ++a_dropped; continue; }
                    } else {
                        // An UNLOADED exterior vendor (an unloaded exterior actor may report its worldspace's persistent cell, which no cell leg
                        // can arrive in), or any vendor once Harbinger carries kTravel_CrossCells: a REF leg to the vendor actor himself (ch.19
                        // does not require a loaded destination). Arrival by distance; the vendor is then confirmed loaded and walked to.
                        vkind = DestKind::Ref;
                        placePos = vw.pos;
                        d = placePos.GetDistance(pc->GetPosition());
                    }
                    NoteVendorOnce(sh.faction, std::format("shop '{}' (faction {:08X}): run by {} -> walk to {} {:08X}", ShopPlaceName(fac), sh.faction,
                                                           Vendors::Describe(vw), vkind == DestKind::Cell ? "the vendor's CELL" : vkind == DestKind::Ref ? "the vendor (ref leg)" : "the vendor",
                                                           vdest));
                    seenChest[sh.chest] = true;
                    c.dist = d;
                    c.actorId = vw.actor;   // the vendor NPC (the stop's vendorActor: confirmed in person on arrival); the plan key stays the faction
                    c.kind = vkind; c.destForm = vdest; c.destRad = vrad;
                    c.pos = placePos; c.havePos = true;
                    (open ? catalogOpen : *a_closedOut).push_back(c);
                }
                // v2.7.1: nearest first, THEN the cap (the radius gate used to thin this list; without it a big town would fill the cap in catalog order)
                std::stable_sort(catalogOpen.begin(), catalogOpen.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
                for (auto& c : catalogOpen) { if (out.size() >= kMaxCands) break; out.push_back(c); }
            }
            if (waiting || doorWaiting || vendorWaiting) {
                if (a_closedOut) a_closedOut->clear();   // never commit a trip that silently lacks the fence / spouse store / a shop whose door is being read: re-plan on the next cadence (6 s)
                spdlog::info("[errand] {:08X}: plan waiting for {}{} -- re-planning shortly", a_follower->GetFormID(),
                             waiting && doorWaiting ? "a vendor-condition answer (fence / spouse gate) and an interior shop's entry-door read"
                             : waiting ? "a vendor-condition answer (fence / spouse gate)"
                             : doorWaiting ? "an interior shop's entry-door read (the loaded exterior grid, two cells deep)"
                                           : "the vendor index (who runs each shop and where they are; built once per load on the main thread)",
                             vendorWaiting && (waiting || doorWaiting) ? " and the vendor index" : "");
                if (a_doorWait && (doorWaiting || vendorWaiting)) *a_doorWait = true;   // mfo-272: a plan without its located vendors is never committed
                out.clear();
                return out;
            }
            {
                // The plan runs every 6 s: say it when this follower's skipped set CHANGES, not on every plan.
                std::scoped_lock lk(g_mx);
                auto& last = g_backoffLogged[a_follower->GetFormID()];
                if (last != backoffSkipped) {
                    last = backoffSkipped;
                    if (!backoffSkipped.empty())
                        spdlog::info("[errand] {:08X}: plan skips backed-off target(s) after a failed walk: {}", a_follower->GetFormID(), backoffSkipped);
                }
            }
            return out;
        }

        // The vendor ACTOR of a faction standing in a LOADED cell: the physical half of a shop reached through its place
        // (design AT_VENDOR_PHYS). Collect-then-act; alive, loaded, not in combat, not a teammate, not asleep.
        RE::NiPointer<RE::Actor> FindVendorActor(RE::Actor* a_near, RE::FormID a_faction, bool a_allCells = false) {
            auto* cell = a_near ? a_near->GetParentCell() : nullptr;
            if (!cell || !cell->IsAttached()) return {};
            std::vector<RE::ActorHandle> cands;
            auto* pc = RE::PlayerCharacter::GetSingleton();
            cell->ForEachReferenceInRange(a_near->GetPosition(), kScanRadius, [&](RE::TESObjectREFR* a_ref) {
                auto* a = a_ref->As<RE::Actor>();
                if (a && a != a_near && a != pc && !a->IsDead() && !a->IsDisabled() && !a->IsMarkedForDeletion() &&
                    !a->IsPlayerTeammate() && cands.size() < 64)
                    cands.push_back(a->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            for (auto& h : cands) {
                auto p = h.get();
                auto* v = p.get();
                if (!v || !v->Is3DLoaded() || APMFBridge::InCombat(v)) continue;
                if (const auto st = v->AsActorState()->GetSitSleepState(); st == RE::SIT_SLEEP_STATE::kIsSleeping ||
                    st == RE::SIT_SLEEP_STATE::kWantToSleep || st == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim) continue;
                if (Vendors::IsMember(v, a_faction)) return p;   // mfo-272: MEMBERSHIP (an actor in two vendor factions runs both shops), not the first vendor faction
            }
            // Run 4: not in his own cell. The main-thread scan of EVERY loaded cell (PostVendorScan, landed by an earlier unattended verdict)
            // may have found a vendor of that faction elsewhere (a stall across a cell border): reuse it while it is loaded and awake.
            if (a_allCells) {   // opt-in: ONLY the unattended re-check (the other callers keep their own-cell meaning)
                RE::FormID seen = 0;
                { std::scoped_lock lk(g_mx); if (const auto it = g_vendorScan.find(a_faction); it != g_vendorScan.end()) seen = it->second.awakeLoaded; }
                if (seen) {
                    if (auto* sv = RE::TESForm::LookupByID<RE::Actor>(seen); sv && !sv->IsDead() && !sv->IsDisabled() && sv->Is3DLoaded() && !APMFBridge::InCombat(sv)) {
                        const auto st = sv->AsActorState()->GetSitSleepState();
                        if (st != RE::SIT_SLEEP_STATE::kIsSleeping && st != RE::SIT_SLEEP_STATE::kWantToSleep && st != RE::SIT_SLEEP_STATE::kWaitingForSleepAnim)
                            return RE::NiPointer<RE::Actor>(sv);
                    }
                }
            }
            return {};
        }

        // Run 4 item 6 (unattended shop): scan EVERY loaded cell for members of the vendor faction, on the MAIN thread (the grid is not safe to
        // walk from the worker, LootScan.cpp crash4), and keep the verdict in g_vendorScan for FindVendorActor + the log line. Returns at once.
        void PostVendorScan(RE::FormID a_faction) {
            if (!a_faction || !MainThread::IsInstalled()) return;
            MainThread::Post([a_faction]() {
                VendorScan sc;
                auto* cal = RE::Calendar::GetSingleton();
                sc.hour = cal ? cal->GetHour() : -1.0f;
                std::string list;
                Logistics::ForEachLoadedRef([&](RE::TESObjectREFR* a_ref) {
                    auto* a = a_ref ? a_ref->As<RE::Actor>() : nullptr;
                    if (!a || a->IsPlayerRef() || a->IsPlayerTeammate()) return RE::BSContainer::ForEachResult::kContinue;
                    if (!Vendors::IsMember(a, a_faction)) return RE::BSContainer::ForEachResult::kContinue;   // mfo-272: membership, not the FIRST vendor faction
                    ++sc.members;
                    const bool asleep = [&] {
                        const auto st = a->AsActorState()->GetSitSleepState();
                        return st == RE::SIT_SLEEP_STATE::kIsSleeping || st == RE::SIT_SLEEP_STATE::kWantToSleep || st == RE::SIT_SLEEP_STATE::kWaitingForSleepAnim;
                    }();
                    auto* c = a->GetParentCell();
                    if (sc.members <= 6)
                        list += std::format("{}{} {:08X} '{}' cell {:08X} {}{}{}{}", list.empty() ? "" : "; ", "member", a->GetFormID(), NameOf(a), c ? c->GetFormID() : 0u,
                                            a->Is3DLoaded() ? "loaded" : "3D-off", a->IsDead() ? ", dead" : "", a->IsDisabled() ? ", disabled" : "", asleep ? ", asleep" : "");
                    if (!sc.awakeLoaded && !a->IsDead() && !a->IsDisabled() && a->Is3DLoaded() && !asleep) sc.awakeLoaded = a->GetFormID();
                    return RE::BSContainer::ForEachResult::kContinue;
                });
                sc.summary = list;
                spdlog::info("[errand] unattended-shop scan of every loaded cell for vendor faction {:08X} at game hour {:.1f}: {} member(s){}{}",
                             a_faction, sc.hour, sc.members, list.empty() ? "" : " -- ", list);
                std::scoped_lock lk(g_mx);
                g_vendorScan[a_faction] = std::move(sc);
            });
        }

        struct Idles { RE::FormID talk = 0, talkFallback = 0, give = 0, take = 0, browse = 0; };
        const Idles& IdleIds() {
            static const Idles s = [] {
                Idles i;
                auto* dh = RE::TESDataHandler::GetSingleton();
                auto id = [&](RE::FormID a_raw) -> RE::FormID {
                    auto* f = dh ? dh->LookupForm<RE::TESIdleForm>(a_raw & 0x00FFFFFF, "Skyrim.esm") : nullptr;
                    return f ? f->GetFormID() : 0u;
                };
                i.talk = id(kIdleTalk); i.talkFallback = id(kIdleTalkFallback); i.give = id(kIdleGive); i.take = id(kIdleTake); i.browse = id(kIdleExamine);
                spdlog::info("[errand] idle forms resolved: talk {:08X} (fallback {:08X}) give {:08X} take {:08X} browse {:08X}",
                             i.talk, i.talkFallback, i.give, i.take, i.browse);
                return i;
            }();
            return s;
        }

        // ── ending an errand ─────────────────────────────────────────────────────────────────────────
        // Every exit funnels here: drop the record, release the claims, stamp the cooldown, say why ONCE.
        void Finish(RE::FormID a_fid, const char* a_why, float a_cooldownSec, bool a_complete = false, bool a_noMark = false) {
            Rec r;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_recs.find(a_fid);
                if (it == g_recs.end()) return;
                r = it->second;
                g_recs.erase(it);
                g_cool[a_fid] = Clock::now() + std::chrono::milliseconds(static_cast<long long>(a_cooldownSec * 1000.0f));
                if (a_complete) g_backoff.erase(BackoffKey(a_fid, r.chest));   // a successful DONE at this target forgives it
            }
            if (r.phase == Phase::Smithing) Smith::End(a_fid, a_why);   // R8: the work claim + the work record
            if (r.phase == Phase::Resting) Rest::End(a_fid, a_why, true, a_noMark || a_cooldownSec == kCombatCooldownSec);   // R7: the stay's release reason is logged; the INN STAGE was reached, the trip is done for this visit
            // Run 4 (marth 2026-10-20): a trip marks the visit done ONLY once it reached the inn stage (the Rest::End above, or NextStopOrFinish after an
            // inn stop DONE). The R7-review marking of "inn stop skipped" / "inn-only trip failed" is gone: a trip that ended earlier (told to wait,
            // failed walks) leaves the visit open, and the retry backoff (not a mark) now paces a re-plan.
            APMFBridge::ReleaseErrand(a_fid);   // idle, hold, walk (in that order) -- and the ch.9 work claim
            Unstick::Clear(a_fid);   // run 5: a placement still in flight is forgotten
            // A shop-place walk has no vendor actor yet (r.vendor == 0): name the faction / destination instead.
            const std::string who = r.vendor ? std::format("vendor '{}' {:08X}", r.vendorName, r.vendor)
                                             : std::format("shop '{}' faction {:08X} dest {:08X}", r.vendorName, r.faction, r.destForm);
            spdlog::info("[errand] {:08X} '{}': {} at {} -- {} ({}; {} s)", a_fid, r.followerName,
                         a_complete ? "DONE" : "ENDED", PhaseName(r.phase), a_why, who,
                         std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - r.start).count());
        }

        // A walk to a target FAILED (the leg ended on something other than arrival): back that (follower, shop) off 120 s, doubling per
        // repeat of the same pair, capped at 15 min, so the planner picks the next candidate instead of the same top-ranked one.
        // Returns the backoff applied, in seconds (for the Finish line).
        long long NoteWalkFailed(RE::FormID a_fid, const Rec& a_r, Clock::time_point a_now) {
            if (!a_r.chest) return 0;
            std::scoped_lock lk(g_mx);
            auto& b = g_backoff[BackoffKey(a_fid, a_r.chest)];
            ++b.fails;
            auto d = kWalkBackoffBase;
            for (int i = 1; i < b.fails && d < kWalkBackoffCap; ++i) d *= 2;
            if (d > kWalkBackoffCap) d = kWalkBackoffCap;
            b.until = a_now + d;
            return d.count();
        }

        // Principle 7: a Harbinger walk cancelled for combat while MFO reads no combat is a disagreement, never masked.
        void WarnCombatDisagree(RE::Actor* a_follower, std::uint32_t a_legState) {
            if (a_legState == APMF_API::kLeg_CombatCancelled && !APMFBridge::InCombat(a_follower))
                spdlog::warn("[errand] {:08X}: Harbinger cancelled the walk for combat but MFO sees no combat (APMFBridge::InCombat false) "
                             "-- the substrate / combat verdicts disagree", a_follower->GetFormID());
        }

        void Store(RE::FormID a_fid, const Rec& a_r) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_recs.find(a_fid); it != g_recs.end()) {
                const auto lost = it->second.townLostSince;   // the Sweep's field: a Tick's stale copy must not reset it
                it->second = a_r;
                it->second.townLostSince = lost;
            }
        }

        // The state of the CURRENT idle beat, with the log line a verdict change deserves.
        APMFBridge::ErrandIdleReading BeatReading(RE::FormID a_fid) { return APMFBridge::ReadErrandIdle(a_fid); }

        // Latch the whole feature off: Harbinger refused the ch.12 idle seat synchronously. No walk-and-refuse
        // loop, no instant trade either (the legacy road is "Harbinger absent" only, never a decline fallback).
        void Latch() {
            if (!g_latched.exchange(true))
                spdlog::warn("[errand] town errands are OFF for this session: Harbinger refused the ch.12 v2 idle "
                             "synchronously (APMF.log names why: [Idle] bIdleV2=0, runtime, self-check). Proper "
                             "animations are required: no idle, no errand, and no instant trade either.");
        }

        std::size_t CountWalking() {
            std::scoped_lock lk(g_mx);
            std::size_t n = 0;
            for (const auto& [id, r] : g_recs) if (r.phase == Phase::Walking) ++n;
            return n;
        }

        // ── R5: the trip's stops ↔ the current-stop working copy ───────────────────────────────────
        // Write the working copy back into stops[cur] (a record with no stops -- never built by this round -- gets one).
        void SyncCur(Rec& r) {
            if (r.stops.empty()) { r.stops.push_back(Stop{}); r.cur = 0; r.stops[0].state = StopState::Active; }
            if (r.cur >= r.stops.size()) r.cur = static_cast<std::uint8_t>(r.stops.size() - 1);
            auto& s = r.stops[r.cur];
            s.faction = r.faction; s.chest = r.chest;
            if (r.vendor) s.vendorActor = r.vendor;
            s.vendorName = r.vendorName;
            s.destKind = r.destKind; s.destForm = r.destForm; s.destRad = r.destRad;
            s.order = r.order;
        }
        // Make stops[a_i] current: a fresh Walking phase toward its place (a vendor actor is walked to directly; a shop place
        // first, and its vendor found on arrival, as phase 1 does).
        void LoadStop(Rec& r, std::size_t a_i) {
            r.cur = static_cast<std::uint8_t>(a_i);
            auto& s = r.stops[a_i];
            s.state = StopState::Active;
            r.phase = Phase::Walking;
            r.vendor = s.destKind == DestKind::VendorActor ? s.destForm : 0u;
            r.chest = s.chest; r.faction = s.faction; r.vendorName = s.vendorName;
            r.destKind = s.destKind; r.destForm = s.destForm; r.destRad = s.destKind == DestKind::VendorActor ? kVendorRadius : s.destRad;   // town run 6: a reloaded vendor stop gets the current radius
            r.order = s.order;
            r.beatRequested = r.talkFallbackUsed = r.dispatched = r.bought = r.announced = false;
            r.browse = false;   // R4: the browsing beat belongs to the stop it was set for (R4 MAP WHAT BREAKS)
            r.confirmedAt = {};
            r.walkRetries = 0; r.retryWait = r.retryRepoint = r.stepOut = r.needStepOut = r.haveBlockPos = false; r.retryAt = {};   // run 4: the retries belong to the stop
            r.stepOutOk = r.placing = false; r.placeTries = 0; r.stepOutFrom = {};   // run 5: so does the unstick
            r.relocations = 0;   // mfo-272: and the vendor re-aims
        }

        // Run 4 review F2: the inn stay's first retry-later attempt kLaterBaseSec in, doubling per attempt already made. Call wherever phase becomes Resting for an Inn stop.
        void ArmLaterRetry(Rec& r, Clock::time_point a_now) {
            int made = 0;
            for (const auto& st : r.stops) if (st.retryLater) made = std::max<int>(made, st.laterRetries);
            r.laterAt = a_now + std::chrono::seconds(kLaterBaseSec << std::min(made, 4));
        }

        // R5: the CURRENT stop is over -- a_ok: its trade ran or there was nothing left to do (Done); else it failed (Skipped, the
        // design's "a failed stop is skipped, logged, and the trip moves on"). TRUE = the trip goes on: r describes the next stop,
        // its walk is declared (ch.19 Repoint of the trip's own claim; MFO declares, Harbinger walks), and the CALLER stores r.
        // FALSE = the trip is over and Finish ran (every claim released, the cooldown stamped, one line).
        // a_cooldown / a_complete are what phase 1 passed to Finish for this end (a one-stop trip ends exactly as phase 1 did).
        bool NextStopOrFinish(RE::FormID a_fid, Rec& r, Clock::time_point a_now, const std::string& a_why, bool a_ok, float a_cooldown,
                              bool a_complete) {
            SyncCur(r);
            bool ok = a_ok, complete = a_complete;
            float cooldown = a_cooldown;
            std::string why = a_why;
            for (;;) {
                r.stops[r.cur].state = ok ? StopState::Done : StopState::Skipped;
                if (complete) { std::scoped_lock lk(g_mx); g_backoff.erase(BackoffKey(a_fid, r.chest)); }   // a trade DONE forgives its target
                const int next = Trip::NextPending(r.stops, r.cur);
                if (next < 0) {
                    const bool multi = r.stops.size() > 1;
                    const bool anyDone = std::any_of(r.stops.begin(), r.stops.end(), [](const Stop& s) { return s.state == StopState::Done; });
                    if (multi && anyDone) cooldown = std::max(cooldown, Config::g_errandCooldownSec.load());   // the trip traded: the errand cooldown
                    // Run 4: the visit is marked done only when the INN STAGE was reached (an inn stop Done), or the trip had no inn stop at all.
                    const bool innDone = std::any_of(r.stops.begin(), r.stops.end(), [](const Stop& s) { return s.kind == StopKind::Inn && s.state == StopState::Done; });
                    const bool hasInn  = std::any_of(r.stops.begin(), r.stops.end(), [](const Stop& s) { return s.kind == StopKind::Inn; });
                    if (Config::g_errandInn.load() && std::any_of(r.stops.begin(), r.stops.end(), [](const Stop& s) { return s.kind == StopKind::Inn && s.state == StopState::Skipped; }))
                        Rest::NoteInnTripFailed(a_fid, a_now);   // run 4 F1: backs the inn off; no mark
                    // Run 5 (R3): a trip with NO inn stop counts ONLY when the town truly has no innkeeper inn (then the last shop was its rest stage,
                    // ErrandRest.cpp Augment, and the shops were visited). A town WITH an innkeeper inn whose stop was not planned or not reached
                    // (an unreachable inn, a door inn without a keeper) is NOT a finished visit.
                    const bool noInnTown = !hasInn && Rest::TownHasNoInnkeeper(r.town);
                    if (anyDone && Config::g_errandInn.load() && (innDone || noInnTown)) Rest::MarkTripDone(a_fid, r.town, innDone ? "the trip ended" : "the trip ended; the town has no innkeeper inn");   // R7: one trip per town visit
                    else if (Config::g_errandInn.load())
                        spdlog::info("[errand] {:08X}: the trip ended WITHOUT reaching the inn stage ({}) -- the visit is NOT marked done; a new trip may plan", a_fid, why);
                    Store(a_fid, r);
                    Finish(a_fid, multi ? std::format("{} (last of a {}-stop trip)", why, r.stops.size()).c_str() : why.c_str(), cooldown, complete);
                    return false;
                }
                const std::string was = Trip::Label(r.stops, r.cur);
                APMFBridge::ReleaseErrandIdle(a_fid);   // the counter's ch.12 beat + ch.1 hold; the ch.19 walk claim stays
                LoadStop(r, static_cast<std::size_t>(next));
                r.phaseAt = a_now;
                r.startClk = r.lastTickClk = Scheduler::ServiceClock();   // the 240 s real-time cap is per stop (fErrandMaxGameMin covers the trip)
                const char* how = "re-pointed";
                LogRepointFrom(a_fid, "errand re-point");
                bool walk = r.destForm != 0 && APMFBridge::RepointErrandTravel(a_fid, r.destForm, r.destRad);
                if (!walk && r.destForm != 0) { walk = APMFBridge::ClaimErrandTravel(a_fid, r.destForm, r.destRad); how = "claimed afresh"; }
                spdlog::info("[errand] {:08X} '{}': TRIP {} {} ({}) -- next {}: walk to {} {:08X} {}", a_fid, r.followerName, was,
                             ok ? "DONE" : "SKIPPED", why, Trip::Label(r.stops, r.cur),
                             r.destKind == DestKind::VendorActor ? "the vendor" : r.destKind == DestKind::Ref ? "the shop ref" : "the shop cell",
                             r.destForm, walk ? how : "could NOT be declared (Harbinger refused the ch.19 Repoint and a new claim)");
                if (walk) return true;
                ok = false;
                complete = false;
                cooldown = kFailCooldownSec;
                why = "the walk to it could not be declared";
            }
        }
    }

    // ── public: lifecycle ───────────────────────────────────────────────────────────────────────────
    bool Active(RE::FormID a_follower) {
        {
            std::scoped_lock lk(g_mx);
            if (g_recs.contains(a_follower)) return true;
        }
        return Sim::Has(a_follower);
    }

    std::string Describe(RE::FormID a_follower) {
        bool resting = false, smithing = false;
        {
            std::scoped_lock lk(g_mx);
            auto it = g_recs.find(a_follower);
            if (it != g_recs.end()) {
                const Rec& r = it->second;
                if (r.phase == Phase::Smithing) smithing = true;   // R8: "Working at X" (the Smith table is read below)
                else if (r.phase == Phase::Resting) resting = true;   // R7: "Resting at X" / "Praying at X" (the Rest table is read below, the lock released)
                else {
                    const std::string shop = (r.vendorName.empty() || r.vendorName == "?") ? std::string("the shop") : r.vendorName;
                    return r.phase == Phase::Walking ? "Walking to " + shop : "Trading with " + shop;
                }
            }
        }
        if (smithing) return Smith::Describe(a_follower);
        if (resting) return Rest::Describe(a_follower);
        return Sim::Describe(a_follower);   // the table mutex is released: the two never nest
    }

    void End(RE::FormID a_follower, const char* a_why) { Finish(a_follower, a_why, kFailCooldownSec); }

    void ClearTransientState() {
        {
            std::scoped_lock lk(g_mx);
            g_recs.clear();
            g_cool.clear();
            g_nextPlan.clear();
            g_parked.clear();
            g_vendorScan.clear();   // run 4
            g_vendorNote.clear();   // mfo-272
            g_backoff.clear();
            g_backoffLogged.clear();
            g_townShops.clear();
            g_townId = 0;
            g_townSince = {};
        }
        Sim::ClearTransientState();
        Rest::ClearTransientState();   // R7
        Unstick::ClearAll();   // run 5
        Vendors::ClearAll();   // mfo-272
        ErrandShops::Clear();   // R6
        Smith::ClearTransientState();   // R8
        APMFBridge::ClearErrandClaims();
    }

    // ── R2: the 'ERND' co-save record (Serialization.cpp owns the bytes) ──────────────────────────────
    // SaveCallback, main thread, the pump PAUSED (no tick mutates a table mid-snapshot; the locks are taken anyway).
    // A physical errand is saved as its phase + flags; the game-day stamps are the simulation's only (a physical
    // errand restored unloaded gets a fresh walk estimate from Sim::Begin).
    std::vector<Saved> SnapshotForSave() {
        std::vector<Saved> out;
        {
            std::scoped_lock lk(g_mx);
            out.reserve(g_recs.size());
            for (const auto& [fid, r] : g_recs) {
                Saved s;
                s.follower = fid;
                switch (r.phase) {
                    case Phase::Walking: s.stage = SavedStage::PhysWalking; break;
                    case Phase::Talking:
                    case Phase::Giving:  s.stage = SavedStage::PhysAtVendor; break;
                    case Phase::Trading: s.stage = SavedStage::PhysTrading; break;
                    case Phase::Taking:
                    case Phase::Smithing:   // R8: saved as the last beat; a restored smith stop is not resumed (its plan is session-only)
                    case Phase::Resting: s.stage = SavedStage::PhysTaking; break;   // R7: a stay / prayer is saved as the last beat (no new stage string)
                }
                if (r.phase != Phase::Walking) s.flags |= Saved::kArrived;
                if (r.dispatched)              s.flags |= Saved::kDispatched;
                if (r.phase == Phase::Taking || (r.phase == Phase::Resting && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn))
                    s.flags |= Saved::kTradeDone;   // (R7: an inn stay is past its trade; a temple prayer is not)
                if (r.order && !r.order->skipped) s.flags |= Saved::kHadOrder;
                s.town = r.town; s.faction = r.faction; s.chest = r.chest; s.vendorActor = r.vendor;
                s.destKind = r.destKind == DestKind::VendorActor ? SavedDest::VendorActor
                           : r.destKind == DestKind::Ref         ? SavedDest::Ref : SavedDest::Cell;
                s.destForm = r.destForm; s.destRad = r.destRad;
                s.lastCell = r.lastCell; s.lastPos = r.lastPos;
                s.startDays = r.startDays;
                Rec rc = r;   // R5: the itinerary, with the working copy written back into stops[cur]
                SyncCur(rc);
                for (const auto& st : rc.stops) s.stops.push_back(Trip::ToSaved(st));
                s.cur = rc.cur;
                out.push_back(std::move(s));
            }
        }
        Sim::SnapshotInto(out);
        return out;
    }

    void RestoreFromSave(std::vector<Saved>&& a_entries) { Sim::StagePending(std::move(a_entries)); }
    void ClearRestorePending() { Sim::ClearPending(); }

    // ── services for the simulation (Errand_internal.h) ────────────────────────────────────────────
    namespace detail {
        void Cooldown(RE::FormID a_follower, float a_seconds) {
            std::scoped_lock lk(g_mx);
            g_cool[a_follower] = Clock::now() + std::chrono::milliseconds(static_cast<long long>(a_seconds * 1000.0f));
        }
        bool InLoadingMenu() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
        }
        bool OpenAtHour(const RE::TESFaction* a_fac, float a_hour) { return a_fac && OpenAt(a_fac, a_hour); }
        bool FactionTradesKeyword(const RE::TESFaction* a_fac, RE::FormID a_kw) { return a_fac && ListTrades(a_fac, a_kw); }
        RE::FormID PlayerTownId() {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const RE::BGSLocation* here = pc ? LocationTypes::TownAnchor(pc->GetCurrentLocation()) : nullptr;
            return here ? here->GetFormID() : 0u;
        }
        const char* NameOf(const RE::Actor* a_actor) {
            const char* n = a_actor ? a_actor->GetName() : nullptr;
            return n && *n ? n : "?";
        }
        void Toast(const std::string& a_text) {
            if (!Config::g_townToasts.load()) return;
            // DebugNotification enqueues a UI message: main thread only (the Tick runs on the AddTask job worker).
            auto fn = [text = a_text]() { RE::DebugNotification(text.c_str()); };
            if (MainThread::IsInstalled()) MainThread::Post(fn);
            else                           fn();   // VR: the pump is a no-op, keep the direct path
        }
        void ToastTrading(const std::string& a_who, const std::string& a_shop, RE::FormID a_faction) {
            if (a_faction && ErrandShops::IsFenceFaction(a_faction)) { ToastFence(a_who, a_shop); return; }   // R6: a fence visit reads differently
            Toast(std::format("{} is trading with {}.", a_who, a_shop));
        }
        // R3 strings table for the phase-2 kinds (no caller yet).
        void ToastSmithing(const std::string& a_who, const std::string& a_place) {
            Toast(std::format("{} is working at {}.", a_who, a_place));
        }
        void ToastResting(const std::string& a_who, const std::string& a_inn) {
            Toast(std::format("{} is resting at {}.", a_who, a_inn));
        }
        void ToastPraying(const std::string& a_who, const std::string& a_temple) {
            Toast(std::format("{} is praying at {}.", a_who, a_temple));
        }
        void ToastFence(const std::string& a_who, const std::string& a_fence) {
            Toast(std::format("{} is visiting {}.", a_who, a_fence));
        }

        void CrowdInto(std::unordered_map<RE::FormID, int>& a_crowd, RE::FormID a_exclude) {
            std::scoped_lock lk(g_mx);
            for (const auto& [fid, r] : g_recs) {
                if (fid == a_exclude) continue;
                if (r.stops.empty()) { if (r.chest) ++a_crowd[r.chest]; continue; }
                for (std::size_t i = r.cur; i < r.stops.size(); ++i) {
                    const auto& st = r.stops[i];
                    if ((st.state == StopState::Active || st.state == StopState::Pending) && st.chest) ++a_crowd[st.chest];
                }
            }
        }

        // RECALL (marth 2026-10-18: a follower always rejoins the player): put an alive, UNLOADED follower beside the player,
        // out of his view, UNLESS he is already in the player's cell. Vanilla Papyrus ObjectReference.MoveTo, ~250 u behind
        // the player's heading (the AFT fBehindOffset precedent): a vanilla native on a persistent actor, no engine-address
        // seat. R1b: the simulation RESOLVES the errand (trade replayed, walk back simulated) before calling this.
        void RecallToPlayer(RE::FormID a_fid, const char* a_why) {
            auto* f  = RE::TESForm::LookupByID<RE::Actor>(a_fid);
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!f || !pc || f->IsDead() || !pc->Is3DLoaded() || InLoadingMenu()) return;
            if (f->Is3DLoaded()) return;
            auto* fc = f->GetParentCell();
            auto* pcell = pc->GetParentCell();
            if (fc && fc == pcell) return;   // already in the player's cell: his own package brings him
            const float az = pc->GetAngleZ();
            constexpr float kBehind = 250.0f;
            const float dx = -kBehind * std::sin(az), dy = -kBehind * std::cos(az);
            const bool ok = Papyrus::DispatchMoveTo(f, pc, dx, dy, 0.0f);
            spdlog::info("[errand] {:08X} '{}': RECALL ({}) -- he is unloaded while the player is elsewhere; MoveTo the "
                         "player (+{:.0f},{:.0f}) {}", a_fid, NameOf(f), a_why, dx, dy, ok ? "dispatched" : "FAILED to dispatch");
        }

        bool VendorPresentNear(RE::Actor* a_follower, RE::FormID a_faction) {
            return static_cast<bool>(FindVendorActor(a_follower, a_faction));
        }

        bool ResumePhysical(RE::Actor* a_follower, const ResumeInfo& a_info) {
            if (!a_follower || g_latched.load() || !APMFBridge::ErrandSupported()) return false;
            // R7: he is loaded AT the inn / temple of the current stop: no walk, no trade -- the stop's work begins (the stay / the prayer).
            if (a_info.work) {
                if (a_info.cur >= a_info.stops.size()) return false;
                const auto fid = a_follower->GetFormID();
                const auto now = Clock::now();
                Rec r;
                r.town = a_info.town;
                r.followerName = a_info.followerName;
                r.stops = a_info.stops; r.cur = a_info.cur;
                r.tripPurse = a_info.tripPurse; r.tripSpent = a_info.tripSpent;
                r.faction = a_info.faction; r.chest = a_info.chest; r.vendorName = a_info.vendorName;
                r.order = a_info.order;
                r.destKind = a_info.destKind; r.destForm = a_info.destForm; r.destRad = a_info.destRad;
                r.start = r.phaseAt = now;
                r.startClk = r.lastTickClk = Scheduler::ServiceClock();
                r.startDays = a_info.startDays;
                r.lastPos = a_follower->GetPosition();
                r.announced = true;
                SyncCur(r);
                r.stops[r.cur].state = StopState::Active;
                if (!Rest::Begin(a_follower, r.stops[r.cur], 0, r.followerName, r.town, now)) return false;
                r.phase = Phase::Resting;
                if (r.stops[r.cur].kind == StopKind::Inn) ArmLaterRetry(r, now);   // run 4 F2
                {
                    std::scoped_lock lk(g_mx);
                    g_recs[fid] = r;
                }
                spdlog::info("[errand] {:08X} '{}': PICKS UP -- he is loaded again at '{}'; the stop's work resumes on screen", fid, r.followerName, r.vendorName);
                return true;
            }
            // R8: a SMITH stop walks to the smithy's PLACE (Ref / Cell) and works there: no vendor, no counter business.
            const bool smithStop = a_info.cur < a_info.stops.size() && a_info.stops[a_info.cur].kind == StopKind::Smith;
            auto vendor = smithStop ? RE::NiPointer<RE::Actor>{} : FindVendorActor(a_follower, a_info.faction);
            // R2: a physical errand restored from the save walks on to its vendor ACTOR even when the vendor is not in his cell (an
            // exterior stall one cell over), provided that actor is loaded and usable.
            if (!smithStop && !vendor && a_info.destKind == DestKind::VendorActor && a_info.destForm) {
                if (auto* va = RE::TESForm::LookupByID<RE::Actor>(a_info.destForm);
                    va && va->Is3DLoaded() && !va->IsDead() && !va->IsDisabled() && !APMFBridge::InCombat(va))
                    vendor = RE::NiPointer<RE::Actor>(va);
            }
            const auto fid = a_follower->GetFormID();
            // No vendor actor in HIS cell: he is not in the shop (he was handed off mid-walk and loaded again elsewhere), so
            // the walk to the shop's place simply resumes. No place either (a vendor actor was the destination): give up.
            if (!vendor && (a_info.destKind == DestKind::VendorActor || a_info.destForm == 0)) return false;
            // A claim that held him through the simulation (the ch.19 cell hold kept across the hand-off) is RE-POINTED to the vendor / place
            // (ClaimErrandTravel on a standing claim of another destination would refuse); no standing claim = a fresh one.
            const RE::FormID resumeDest = vendor ? vendor->GetFormID() : a_info.destForm;
            const float      resumeRad  = vendor ? kVendorRadius : a_info.destRad;
            if (!APMFBridge::RepointErrandTravel(fid, resumeDest, resumeRad) && !APMFBridge::ClaimErrandTravel(fid, resumeDest, resumeRad)) return false;
            // F2 (review of dc0c334): the new walk offer must be the OWNER. A ch.9 work claim / ch.12 idle / ch.1 hold that held him through the simulation
            // (the previous stop's) would tie or outrank it as the earlier claim, so they go now that the walk claim is filed.
            APMFBridge::ReleaseErrandWork(fid);
            APMFBridge::ReleaseErrandIdle(fid);
            const auto now = Clock::now();
            Rec r;
            r.phase = Phase::Walking;
            r.vendor = vendor ? vendor->GetFormID() : 0u;
            r.chest = a_info.chest;
            r.town = a_info.town;
            r.faction = a_info.faction;
            r.followerName = a_info.followerName;
            r.vendorName = vendor ? NameOf(vendor.get()) : a_info.vendorName;
            r.order = a_info.order;
            r.announced = a_info.announced;
            r.stops = a_info.stops; r.cur = a_info.cur;   // R5: the trip goes on from the CURRENT stop
            r.tripPurse = a_info.tripPurse; r.tripSpent = a_info.tripSpent;
            if (vendor) { r.destKind = DestKind::VendorActor; r.destForm = r.vendor; r.destRad = kVendorRadius; }
            else        { r.destKind = a_info.destKind; r.destForm = a_info.destForm; r.destRad = a_info.destRad; }
            r.start = r.phaseAt = now;
            r.startClk = r.lastTickClk = Scheduler::ServiceClock();
            r.startDays = a_info.startDays;
            r.lastPos = a_follower->GetPosition();
            SyncCur(r);
            r.stops[r.cur].state = StopState::Active;
            // R4: found at the counter. Only an errand that really arrived off screen (announced = the sim reached Trade) browses.
            if (vendor) {
                r.browse = a_info.announced && IdleIds().browse != 0;
            }
            {
                std::scoped_lock lk(g_mx);
                g_recs[fid] = r;
            }
            spdlog::info("[errand] {:08X} '{}': PICKS UP -- he is loaded again; {} '{}' ({:08X}) to trade on screen",
                         fid, r.followerName, vendor ? "walking to the vendor" : "resuming the walk to the shop of", r.vendorName,
                         vendor ? r.vendor : r.destForm);
            return true;
        }
    }

    // F3: the commit of a planned trip (TryStart's tail, shared with the shop-opens re-plan): the ch.19 leg budget, the first stop's travel claim,
    // the record, the plan line. TRUE = the trip stands; FALSE = queued / refused (g_nextPlan is set; nothing else changed).
    static bool StartTrip(RE::Actor* a_follower, const RE::BGSLocation* a_town, Clock::time_point a_now, std::vector<Stop>&& stops,
                          const std::string& tripLine, std::size_t a_candCount) {
        const auto fid = a_follower->GetFormID();
        // The ch.19 leg budget (Harbinger's 8 package slots, shared with loot and the deposit): beyond it he queues.
        if (CountWalking() >= static_cast<std::size_t>(kMaxWalking)) {
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kQueueRetry;
            spdlog::info("[errand] {:08X} '{}': QUEUED -- {} errand walks already hold Harbinger's ch.19 legs (shared with "
                         "loot), next try in {} s", fid, NameOf(a_follower), kMaxWalking, kQueueRetry.count());
            return false;
        }
        if (!APMFBridge::ClaimErrandTravel(fid, stops.front().destForm, stops.front().destRad)) {
            // Refused at the call: the leg budget (all 8 slots busy), [Travel] bTravel=0, VR. Retry soon, never a fallback.
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kQueueRetry;
            return false;
        }
        Rec r;
        r.town        = a_town->GetFormID();
        r.followerName = NameOf(a_follower);
        r.stops = std::move(stops);
        r.tripPurse = -1;   // R7: the first stop that carries an order sets the trip purse (a temple stop has none)
        for (const auto& st : r.stops) if (st.order) { r.tripPurse = st.order->purse; break; }
        LoadStop(r, 0);
        r.lastPos = a_follower->GetPosition();
        r.planPos = r.lastPos; r.planPosSet = true;
        if (auto* fc = a_follower->GetParentCell()) r.lastCell = fc->GetFormID();
        r.start = r.phaseAt = a_now;
        r.startClk = r.lastTickClk = Scheduler::ServiceClock();
        if (auto* cal = RE::Calendar::GetSingleton()) r.startDays = cal->GetDaysPassed();
        {
            std::scoped_lock lk(g_mx);
            g_recs[fid] = r;
        }
        // His OWN distance to the destination (the from-player figure above says nothing about where HE stands; field
        // 2026-10-19). A shop CELL has no position here (its door distance is measured from the player): "n/a".
        std::string ownDist = "n/a (shop cell: no position)";
        {
            RE::NiPoint3 dp{};
            bool         have = false;
            const Stop& first = r.stops.front();   // R5: the trip's first stop (the R4 line's `best`)
            if (first.destKind != DestKind::Cell) have = Trip::PlacePos(first, dp);
            else if (first.havePos) { dp = first.pos; have = true; }   // an inn / temple / shop cell: the DOOR position the planner found
            if (have) ownDist = std::format("{:.0f} u{}", a_follower->GetPosition().GetDistance(dp), first.destKind == DestKind::Cell ? " to its door" : "");
        }
        const char* destWhat = r.destKind == DestKind::VendorActor ? "the vendor" : r.destKind == DestKind::Ref ? "the shop ref"
                             : r.stops.front().kind == StopKind::Inn ? "the inn cell" : r.stops.front().kind == StopKind::Temple ? "the temple cell" : "the shop cell";
        spdlog::info("[errand] {:08X} '{}' TRIP PLAN in '{}': {} | walking to {} '{}' ({} from HIM): {} {:08X} | {} candidate(s)",
                     fid, r.followerName, TownName(r.town), tripLine, Trip::Label(r.stops, 0), r.vendorName, ownDist,
                     destWhat, r.destForm, a_candCount);
        return true;
    }

    // F3 (marth 2026-10-20: ONE TRIP AT A TIME): a watched shop opened and the recorded needs yield a stop (ErrandReopen.cpp Poll). Start the new
    // trip -- the opened shops, the inn LAST. The caller has ended whatever held the follower (the stay, the trip-done mark) WITHOUT a mark.
    static bool StartReopenedTrip(RE::Actor* a_follower, RE::FormID a_townId, Clock::time_point a_now, Reopen::Plan&& a_plan) {
        auto* town = RE::TESForm::LookupByID<RE::BGSLocation>(a_townId);
        if (!town) {
            spdlog::warn("[errand] {:08X}: a watched shop opened but the town {:08X} does not resolve -- no new trip", a_follower->GetFormID(), a_townId);
            return false;
        }
        const std::size_t n = a_plan.stops.size();
        const std::string line = a_plan.line;
        std::vector<Stop> stops = a_plan.stops;   // (copy: a failed start leaves the plan, and the watch, intact for the retry)
        if (!StartTrip(a_follower, town, a_now, std::move(stops), line, n)) {
            Reopen::Retry(a_follower->GetFormID());
            if (!a_plan.lateForms.empty()) Reopen::NoteLate(a_follower->GetFormID(), a_townId, a_plan.lateForms, true);   // MISSED SELLS (b): the late items wait for the next try
            spdlog::warn("[errand] {:08X} '{}': a needed shop opened but the new trip could NOT start (queued at the ch.19 walk budget or the travel claim was refused) -- the shop stays in the watch, retried", a_follower->GetFormID(), NameOf(a_follower));
            return false;
        }
        Reopen::Consume(a_follower->GetFormID(), a_plan);
        if (!a_plan.lateForms.empty()) Reopen::DropGoldForms(a_follower->GetFormID(), a_plan.lateForms);   // F3 (review)
        spdlog::info("[errand] {:08X} '{}': a NEEDED SHOP OPENED -- new trip of {} stop(s) STARTED (the inn last): {}", a_follower->GetFormID(), NameOf(a_follower), n, line);
        return true;
    }

    // MISSED SELLS (b), marth 2026-10-21: items that became sellable mid-trip and sit in no stop's order (Reopen::NoteLate). Loaded follower only: the
    // OPEN shops the plan recorded are evaluated again with his CURRENT inventory (EconomyErrandPlan), the orders are cut to the late items, and the
    // normal trip planner decides (minimum sell, payer choice, stop cap). TRUE = a_out is a new trip (the caller ends the stay / the mark and starts it).
    static bool PlanLateSells(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now, RE::FormID a_townId, Reopen::Plan& a_out) {
        const auto fid = a_follower->GetFormID();
        Reopen::LateJob job;
        if (!Reopen::TakeLate(fid, a_townId, a_now, job)) return false;
        std::vector<ErrandShop> shops;
        for (const auto& c : job.shops) {
            auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(c.faction);
            if (!fac) continue;
            RE::ActorHandle h;
            if (auto* va = c.actorId ? RE::TESForm::LookupByID<RE::Actor>(c.actorId) : nullptr; va && va->Is3DLoaded()) h = va->GetHandle();
            shops.push_back(ErrandShop{ fac, h });
        }
        std::vector<ErrandPlanned> planned;
        int deferred = 0;
        if (!shops.empty()) EconomyErrandPlan(a_follower, a_state, a_now, shops, planned, &deferred);
        if (deferred > 0) {   // a price quote is still being computed on the main thread: try again (capped in TakeLate)
            Reopen::NoteLate(fid, a_townId, job.forms, true);
            spdlog::info("[errand] {:08X} '{}': LATE SELL re-plan waits for {} shop price quote(s)", fid, NameOf(a_follower), deferred);
            return false;
        }
        std::string names;
        for (const auto f : job.forms) {
            auto* o = RE::TESForm::LookupByID<RE::TESBoundObject>(f);
            names += std::format("{}'{}'", names.empty() ? "" : ", ", o && o->GetName() && *o->GetName() ? o->GetName() : "?");
        }
        for (auto& p : planned) {
            if (p.skipped) continue;
            std::erase_if(p.sell, [&](const TradeBridge::SellRow& r) {
                return !r.obj || std::find(job.forms.begin(), job.forms.end(), r.obj->GetFormID()) == job.forms.end();
            });
            p.sellRows = static_cast<int>(p.sell.size());
            p.sellValue = 0;
            for (const auto& r : p.sell) p.sellValue += static_cast<std::int64_t>(r.value) * std::max<std::int32_t>(r.count, 0);
            p.needKinds = 0; p.needs.clear();
            p.wishGear = p.wishApparel = p.wishTome = p.wishMaterials = false;
            p.buy = TradeBridge::BuyThresholds{};
            p.purse = 0;
            if (p.sell.empty()) p.skipped = true;
        }
        job.in.follower = fid;
        job.in.from = a_follower->GetPosition();
        if (!Reopen::PlanLate(job, planned, a_out)) {
            spdlog::info("[errand] {:08X} '{}': LATE SELL re-plan dropped -- no open shop of the {} recorded buys {} (or not worth a stop)", fid, NameOf(a_follower), shops.size(), names);
            return false;
        }
        spdlog::info("[errand] {:08X} '{}': LATE SELL re-plan -- {} stop(s) for {}", fid, NameOf(a_follower), a_out.stops.size(), names);
        return true;
    }

    // ── public: the planner (the idle branch of ServiceFollower) ─────────────────────────────────────
    // Town run 6 (never mask): ONE rate-limited line per follower for every silent "not planning" gate of TryStart: logged when the
    // reason changes, else at most every 30 s.
    static void NotPlanning(RE::Actor* a_follower, Clock::time_point a_now, const char* a_reason, float a_remainSec = -1.0f) {
        static std::mutex s_mx;
        static std::unordered_map<RE::FormID, std::pair<const char*, Clock::time_point>> s_last;
        const auto fid = a_follower->GetFormID();
        {
            std::scoped_lock lk(s_mx);
            auto& e = s_last[fid];
            if (e.first == a_reason && a_now < e.second) return;
            e = { a_reason, a_now + std::chrono::seconds(30) };
        }
        if (a_remainSec >= 0.0f)
            spdlog::info("[errand] {:08X} '{}': not planning -- {} ({:.0f} s left)", fid, NameOf(a_follower), a_reason, a_remainSec);
        else
            spdlog::info("[errand] {:08X} '{}': not planning -- {}", fid, NameOf(a_follower), a_reason);
    }

    bool TryStart(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now) {
        if (!a_follower) return false;
        // Legacy proximity trade stays when errands are off, or Harbinger lacks what an errand needs.
        if (!Config::g_townErrands.load()) return false;
        if (!APMFBridge::ErrandSupported()) {
            static std::atomic<bool> s_logged{ false };
            if (!s_logged.exchange(true))
                spdlog::info("[errand] town errands need Harbinger ABI >= 24 (this APMF: {}): the legacy instant "
                             "proximity trade stays. (Logged once.)",
                             APMFBridge::ErrandApiVersion() ? std::format("ABI v{}", APMFBridge::ErrandApiVersion())
                                                            : std::string("absent"));
            return false;
        }
        // From here on errands OWN the economy: whatever happens below, the caller must not run the
        // instant trade (marth 2026-10-18: retired whenever Harbinger is present).
        if (g_latched.load()) { NotPlanning(a_follower, a_now, "errands latched (a fatal error stopped them)"); return true; }

        const auto fid = a_follower->GetFormID();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || !pc->Is3DLoaded()) { NotPlanning(a_follower, a_now, "the player is not loaded"); return true; }

        // The player's town + how long he has been in it (the settle clock). Tracked on every call.
        const RE::BGSLocation* playerTown = LocationTypes::TownAnchor(pc->GetCurrentLocation());
        Clock::time_point townSince{};
        const RE::FormID tidNow = playerTown ? playerTown->GetFormID() : 0u;
        Rest::NoteTown(tidNow);   // R7: the player's town changed (or he left town) -> every "trip done" mark is cleared (before any early return below)
        {
            std::scoped_lock lk(g_mx);
            const RE::FormID tid = playerTown ? playerTown->GetFormID() : 0u;
            if (tid != g_townId) {
                g_townId = tid; g_townSince = a_now;
                // mfo-272 review F1: the vendor index is a snapshot -- a new town is planned from a fresh one. (Vendors' mutex is a LEAF: taken
                // here under Errand g_mx, never the other way round; Invalidate makes no engine call.)
                if (tid) Vendors::Invalidate("the player's town changed");
            }
            townSince = g_townSince;
            if (g_recs.contains(fid)) return true;   // (Tick owns him)
            if (Sim::Has(fid)) return true;          // (the simulation owns him)
            if (const auto it = g_cool.find(fid); it != g_cool.end() && a_now < it->second) {
                NotPlanning(a_follower, a_now, "errand cooldown", std::chrono::duration<float>(it->second - a_now).count());
                return true;
            }
            if (const auto it = g_nextPlan.find(fid); it != g_nextPlan.end() && a_now < it->second) return true;
        }
        if (!playerTown) { NotPlanning(a_follower, a_now, "the player is not in a town"); return true; }
        // COLLEGE GATE (marth 2026-10-21: "quest gated so mage followers dont trespass"): the College of Winterhold is its own town
        // (LocationTypes::TownAnchor); its stops are planned only once the PLAYER is in CollegeofWinterholdFaction (Skyrim.esm 0001F259).
        // Either record absent = no College stops.
        if (auto* dhC = RE::TESDataHandler::GetSingleton(); dhC && playerTown == dhC->LookupForm<RE::BGSLocation>(0x00076F3A, "Skyrim.esm")) {
            auto* colFac = dhC->LookupForm<RE::TESFaction>(0x0001F259, "Skyrim.esm");
            if (!colFac || !pc->IsInFaction(colFac)) {
                NotPlanning(a_follower, a_now, "the College of Winterhold is closed to errands until the player has joined it (CollegeofWinterholdFaction)");
                return true;
            }
            // MAGE-ONLY (marth 2026-10-21: "a mage follower would prefer the college"): every College stop is a College vendor (the town IS the
            // College tree, LocationTypes::TownAnchor), so a follower who is not a mage plans none here: an explicit Mage class pick, a caster
            // (magic loadout + a cast gambit, IsCasterFollower) or a spell-tome wish (bEconomyBuyTomes + a cast gambit, BuildBuyThresholds).
            // The tome gate globals (Economy.cpp UnlockCollegeTomes) still decide which tomes the vendors offer.
            const bool mage = a_state.combatClassOverride == 3 || IsCasterFollower(a_state) ||
                              (Config::g_economyBuyTomes.load() && HasCastGambit(a_state));
            if (!mage) {
                NotPlanning(a_follower, a_now, "the College of Winterhold's vendors serve mage followers only (no Mage class pick, no cast gambit, no spell-tome wish)");
                return true;
            }
        }
        if (Config::g_errandInn.load() && Rest::TripDone(fid, tidNow)) {   // R7: ONE trip AT A TIME (marth 2026-10-19 / 2026-10-20)
            // F3: new-session triggers are the player leaving town (NoteTown), the end of combat (NoteCombat) and a NEEDED SHOP OPENING (here).
            auto* ui0 = RE::UI::GetSingleton();
            auto* avo0 = a_follower->AsActorValueOwner();
            const bool canPlan = !(ui0 && ui0->GameIsPaused()) && !APMFBridge::InCombat(a_follower) && !pc->IsInCombat() && !SlotOf(fid) &&
                              !(avo0 && avo0->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) && !a_follower->IsOnMount() &&
                              a_follower->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal && a_follower->Is3DLoaded() &&
                              LocationTypes::TownAnchor(a_follower->GetCurrentLocation()) == playerTown &&
                              Forms::g_tradeQuest && Forms::g_tradeQuest->IsRunning();
            if (canPlan) {
                Reopen::Plan plan;
                const auto from = a_follower->GetPosition();
                if (Reopen::Poll(fid, tidNow, &from, plan) || PlanLateSells(a_follower, a_state, a_now, tidNow, plan)) {
                    if (StartReopenedTrip(a_follower, tidNow, a_now, std::move(plan))) Rest::UnmarkTripDone(fid, "a needed shop opened / a missed sell");   // a failed start keeps the mark (the watch retries)
                }
            } else {
                NotPlanning(a_follower, a_now, "this visit's trip is done and the reopen watch cannot poll now (paused / combat / looting / waiting / mounted / sitting / not loaded / not in the player's town / no trade quest)");
            }
            return true;
        }
        if (a_now - townSince < std::chrono::milliseconds(static_cast<long long>(Config::g_errandSettleSec.load() * 1000.0f))) {
            NotPlanning(a_follower, a_now, "settling in the town");
            return true;
        }

        // A cheap gate set first; the plan below walks a cell and the follower's inventory.
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GameIsPaused()) { NotPlanning(a_follower, a_now, "game paused"); return true; }
        if (APMFBridge::InCombat(a_follower) || pc->IsInCombat()) { NotPlanning(a_follower, a_now, "combat"); return true; }   // a Harbinger substrate is not combat (v26)
        if (SlotOf(fid)) { NotPlanning(a_follower, a_now, "he holds the looting slot"); return true; }   // looting: loot and errand never overlap
        auto* avo = a_follower->AsActorValueOwner();
        if (avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) { NotPlanning(a_follower, a_now, "told to wait"); return true; }   // told to wait
        if (a_follower->IsOnMount() || a_follower->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) { NotPlanning(a_follower, a_now, "mounted / sitting / sleeping"); return true; }
        if (!a_follower->Is3DLoaded()) { NotPlanning(a_follower, a_now, "he is not loaded"); return true; }
        if (LocationTypes::TownAnchor(a_follower->GetCurrentLocation()) != playerTown) { NotPlanning(a_follower, a_now, "he is not in the player's town"); return true; }   // he is not in the player's town
        if (auto* q = Forms::g_tradeQuest; !q || !q->IsRunning()) { NotPlanning(a_follower, a_now, "the trade quest is missing or not running"); return true; }   // no trade bridge: nothing could be traded

        {
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kPlanInterval;
        }

        // R7: the blessing / disease read decides the temple stop; an inn-only trip planned before it lands would skip the temple for the
        // whole visit (one trip per visit). Pending: the probe is posted, this plan is abandoned and re-run on the next cadence.
        if (Rest::MagicReadPending(a_follower)) { NotPlanning(a_follower, a_now, "waiting for the blessing / disease read"); return true; }
        if (Rest::PlacesPending(a_follower, playerTown)) { NotPlanning(a_follower, a_now, "waiting for the temple / inn door read"); return true; }   // F4: the data-resolved temple's entry door (main-thread read)

        int dropped = 0;
        std::vector<Cand> closedCands;   // F3: dropped ONLY for their hours -> the shop watch
        bool doorWait = false;
        auto cands = BuildCatalog(a_follower, playerTown, a_now, dropped, Config::g_errandInn.load() ? &closedCands : nullptr, &doorWait);
        if (doorWait) { NotPlanning(a_follower, a_now, "waiting for an interior shop's entry-door read or the vendor index"); return true; }   // re-planned on the next cadence
        // R7 (marth 2026-10-19): no open shop is NOT "no errand" -- a follower with nothing to shop for still plans the INN-ONLY trip
        // (Rest::Augment below), so the early return is only for inn errands switched off (MFO-B353).
        if (cands.empty() && !Config::g_errandInn.load()) return true;
        // R8: the smithing want list is built on the MAIN thread (the tempering entry point); the plan reads it, so a missing or old
        // one is queued first and the plan is made on the next cadence.
        bool anySmithy = false;
        for (const auto& c : cands)
            if (ListTrades(c.fac, kKwOreIngot)) { anySmithy = true; break; }   // a real blacksmith trades ore / ingots (a weapon seller is not one)
        if (Smith::Prepare(a_follower, anySmithy) == Smith::Prep::Pending) return true;

        // PLAN: the SAME code a trade uses evaluates every candidate -- loaded vendor actors AND faction-only shops -- and hands
        // back the ORDER for each, built now while the follower is loaded (the simulation replays it; MFO never reads an
        // unloaded follower's inventory).
        std::vector<ErrandShop> shops;
        shops.reserve(cands.size());
        for (auto& c : cands) shops.push_back(ErrandShop{ c.fac, c.handle });
        for (auto& c : closedCands) shops.push_back(ErrandShop{ c.fac, c.handle });   // F3: evaluated NOW (loaded) so the watch holds their orders
        std::vector<ErrandPlanned> planned;
        int deferredShops = 0;
        if (!shops.empty()) EconomyErrandPlan(a_follower, a_state, a_now, shops, planned, &deferredShops);
        // F1 (town run 3): a shop whose price quote is still being computed on the main thread was DROPPED from `planned` (no row at
        // all), and the trip committed without its shop work. Wait ~1 s and re-plan (the refresh is already Posted), like
        // MagicReadPending / PlacesPending; capped, then a loud WARN and the plan proceeds WITHOUT the pending shops (they have no row, so the trip lacks their work).
        {
            static std::unordered_map<RE::FormID, int> s_priceDefers;   // consecutive defers per follower
            if (deferredShops > 0) {
                constexpr int kPriceMaxDefers = 8;   // ~1 s each: a main-thread refresh lands in ~10 ms, 8 s means the Post is stalled
                int& n = s_priceDefers[fid];
                if (++n <= kPriceMaxDefers) {
                    if (n == 1)
                        spdlog::info("[errand] {:08X} '{}': {} shop(s) wait for their price quote (main-thread refresh) -- re-planning in ~1 s", fid, NameOf(a_follower), deferredShops);
                    std::scoped_lock lk(g_mx);
                    g_nextPlan[fid] = a_now + std::chrono::seconds(1);
                    return true;
                }
                spdlog::warn("[errand] {:08X} '{}': {} shop(s) still without a price quote after {} defers (is the main-thread Post running?) -- planning WITHOUT them",
                             fid, NameOf(a_follower), deferredShops, n - 1);
                n = 0;
            } else {
                s_priceDefers.erase(fid);
            }
        }
        std::vector<ErrandPlanned> closedPlanned;   // F3: the hours-closed shops' orders leave the open plan's list (their wishes are not this trip's)
        if (!closedCands.empty()) {
            const auto isClosedKey = [&](RE::FormID a_key) {
                return std::any_of(closedCands.begin(), closedCands.end(), [&](const Cand& c) { return c.id == a_key; });
            };
            for (const auto& p : planned) if (isClosedKey(p.vendorKey)) closedPlanned.push_back(p);
            std::erase_if(planned, [&](const ErrandPlanned& p) { return isClosedKey(p.vendorKey); });
        }

        const int minSell  = Config::g_errandMinSellGold.load();
        const int minPurse = Config::g_errandMinPurse.load();
        float carry = 0.0f;
        if (avo) {
            const float cw = avo->GetActorValue(RE::ActorValue::kCarryWeight);
            const float iw = avo->GetActorValue(RE::ActorValue::kInventoryWeight);
            carry = cw > 0.0f ? iw / cw : 0.0f;
        }
        // R5: the TRIP planner (ErrandTrip.cpp) partitions the work across up to kMaxShopStops stops. The VEND reads stay here: each
        // candidate says which need categories it carries (the phase-1 keyword map) and whether it carries his gear / tome wish.
        bool wantGear = false, wantApparel = false;
        int evaluated = 0, skipped = 0;
        for (const auto& v : planned) {
            ++evaluated;
            if (v.skipped) { ++skipped; continue; }
            wantGear = wantGear || v.wishGear;
            wantApparel = wantApparel || v.wishApparel;
        }
        const auto makeTripCand = [&](const Cand& c, bool a_gear, bool a_apparel) {
            Trip::Cand t;
            t.key = c.id; t.actorId = c.actorId; t.faction = c.fac->GetFormID(); t.chest = c.chest;
            {
                auto* vendor = c.handle.get().get();
                const char* vn = vendor ? vendor->GetName() : nullptr;
                if (vn && *vn) t.name = vn;
                else { t.name = ShopPlaceName(c.fac); if (t.name.empty()) t.name = "?"; }   // F4
            }
            t.dist = c.dist; t.pos = c.pos; t.havePos = c.havePos;
            t.kind = c.kind; t.destForm = c.destForm; t.destRad = c.destRad;
            t.placeKind = c.placeKind; t.placeForm = c.placeForm; t.placeRad = c.placeRad; t.placePos = c.placePos; t.havePlacePos = c.havePlacePos;   // review F2
            for (int k = 0; k < 12; ++k) {
                const RE::FormID kw = (k <= 2) ? kKwPotion : (k == 3 || k == 4 || k == 11) ? kKwArrow : (k == 10) ? kKwWeapon : 0u;
                if (kw && ListTrades(c.fac, kw)) t.needCarry |= 1u << k;
            }
            {   // MISSED BUY: the potion kinds are covered only on EVIDENCE of stock (apothecary class, or this chest's last PlanBuy showed that kind)
                bool known = false;
                const bool apoth = ApothecaryClass(c.fac, known);
                for (int k = 0; k <= 4; ++k) {
                    if (!((t.needCarry >> k) & 1u)) continue;
                    const bool potionKind = k <= 2;
                    if (!potionKind || apoth || !known || TradeBridge::LastSeenStockLines(c.chest, k) > 0) t.needStock |= 1u << k;
                }
                for (int k = 5; k < 12; ++k) if ((t.needCarry >> k) & 1u) t.needStock |= 1u << k;
            }
            t.carriesGear = (a_gear    && (ListTrades(c.fac, kKwWeapon) || ListTrades(c.fac, kKwArmor))) ||
                            (a_apparel && (ListTrades(c.fac, kKwClothing) || ListTrades(c.fac, kKwJewelry) || ListTrades(c.fac, kKwArmor)));
            t.carriesTome = ListTrades(c.fac, kKwSpellTome);
            t.carriesMaterials = ListTrades(c.fac, kKwOreIngot) || ListTrades(c.fac, kKwAnimalHide) || ListTrades(c.fac, kKwAnimalPart);   // R8
            t.carriesForge = ListTrades(c.fac, kKwOreIngot);   // R8: a smithy = an ore / ingot trader (a general store that sells weapons has no forge)
            return t;
        };
        std::vector<Trip::Cand> tcands;
        tcands.reserve(cands.size());
        for (const auto& c : cands) tcands.push_back(makeTripCand(c, wantGear, wantApparel));
        std::vector<Trip::Cand> closedTCands;   // F3
        {
            bool closedGear = false, closedApparel = false;
            for (const auto& v : closedPlanned) { closedGear = closedGear || v.wishGear; closedApparel = closedApparel || v.wishApparel; }
            for (const auto& c : closedCands) closedTCands.push_back(makeTripCand(c, closedGear, closedApparel));
        }
        Trip::Inputs tin;
        tin.follower = fid; tin.from = a_follower->GetPosition(); tin.carry = carry; tin.minSell = minSell; tin.minPurse = minPurse;
        std::vector<Stop> stops;
        std::string tripLine;
        // R7: the temple (when diseased / without an active blessing) and the INN (LAST) are appended to the shop stops; with no shop work
        // and no temple reason the trip is the INN ALONE (marth 2026-10-19).
        bool havePlan = Trip::Plan(tcands, planned, tin, stops, tripLine);
        // R8: the smith stop (his worn gear improved with the materials the shop stops buy) goes AFTER the shops and BEFORE the temple
        // and the inn (marth: "ONE complete trip per town visit"); a smith-only trip is a trip.
        if (Smith::Augment(a_follower, tcands, stops, tripLine)) havePlan = true;
        havePlan = Rest::Augment(a_follower, a_state, a_now, playerTown, havePlan, stops, tripLine);
        if (!havePlan) {
            {
                std::scoped_lock lk(g_mx);
                g_nextPlan[fid] = a_now + std::chrono::seconds(20);   // nothing to do here: the plan is a cell walk + an inventory walk, ask less often
            }
            // Said once per ~minute per follower at most (the plan cadence is 6 s): the reason he is NOT shopping.
            static std::unordered_map<RE::FormID, Clock::time_point> s_nextNoReasonLog;
            if (auto& nx = s_nextNoReasonLog[fid]; nx.time_since_epoch().count() == 0 || a_now >= nx) {
                nx = a_now + std::chrono::seconds(60);
                spdlog::info("[errand] {:08X} '{}': no errand -- {} open shop vendor(s) in '{}' ({} dropped: closed / "
                             "conditioned / fence / other town), {} evaluated, {} skipped (nothing new to trade); no sell/need/"
                             "wish reason clears (minSell {}g, minPurse {}g)", fid, NameOf(a_follower), cands.size(),
                             playerTown->GetName() ? playerTown->GetName() : "?", dropped, evaluated, skipped, minSell,
                             minPurse);
            }
            return true;
        }

        std::optional<Stop> innStopForWatch;   // F3: the committed trip's inn stop is the re-planned trip's LAST stop
        if (!stops.empty() && stops.back().kind == StopKind::Inn) innStopForWatch = stops.back();
        std::vector<Trip::Cand> openTCandsForWatch = tcands;   // MISSED SELLS (b): the late-sell re-plan evaluates these again
        std::vector<RE::FormID> tripFactions;                  // MISSED SELLS (a): a gold-blocked shop this trip visits is covered by its own trade
        for (const auto& st : stops) if (st.kind == StopKind::Shop) tripFactions.push_back(st.faction);
        if (!StartTrip(a_follower, playerTown, a_now, std::move(stops), tripLine, cands.size())) return true;
        // F2: one line per evaluated shop of the committed plan (open shops, then the hours-closed ones the watch holds): the work each
        // carried, or why it carried none. A shop dropped for a pending quote is not listed (F1 re-plans instead).
        {
            const auto logRows = [&](const std::vector<ErrandPlanned>& a_rows, const std::vector<Trip::Cand>& a_cands, const char* a_tag) {
                for (const auto& v : a_rows) {
                    const auto it = std::find_if(a_cands.begin(), a_cands.end(), [&](const Trip::Cand& t) { return t.key == v.vendorKey; });
                    const char* nm = it != a_cands.end() && !it->name.empty() ? it->name.c_str() : "?";
                    if (v.skipped)
                        spdlog::info("[errand] {:08X} shop {} '{}' ({:08X}): SKIPPED (empty-trade memo / nothing to do)", fid, a_tag, nm, v.vendorKey);
                    else
                        spdlog::info("[errand] {:08X} shop {} '{}' ({:08X}): sell {} row(s)/{}g, needs mask {:#x}, carry mask {:#x}, stock-evidence mask {:#x}, wishes gear={} apparel={} tome={} mats={}, purse {}g",
                                     fid, a_tag, nm, v.vendorKey, v.sellRows, v.sellValue, v.needKinds, it != a_cands.end() ? it->needCarry : 0u,
                                     it != a_cands.end() ? it->needStock : 0u, v.wishGear, v.wishApparel, v.wishTome, v.wishMaterials, v.purse);
                }
            };
            logRows(planned, tcands, "open");
            logRows(closedPlanned, closedTCands, "closed");
        }
        // F3 (town run 3): the watch holds only a closed shop that has WORK for him: its row exists, was not skipped, and carries sell rows,
        // a need its VEND carries, or a wish its VEND carries. Anything else would "open" into a trip with no stop (the 17:39 'yield NO stop').
        if (!closedTCands.empty()) {
            const auto hasWork = [&](const Trip::Cand& t) {
                const auto it = std::find_if(closedPlanned.begin(), closedPlanned.end(), [&](const ErrandPlanned& p) { return p.vendorKey == t.key; });
                if (it == closedPlanned.end() || it->skipped) return false;
                return it->sellRows > 0 || (it->needKinds & t.needCarry) != 0 ||
                       (it->wishGear && t.carriesGear) || (it->wishApparel && t.carriesGear) ||
                       (it->wishTome && t.carriesTome) || (it->wishMaterials && t.carriesMaterials);
            };
            std::erase_if(closedTCands, [&](const Trip::Cand& t) { return !hasWork(t); });
            std::erase_if(closedPlanned, [&](const ErrandPlanned& p) {
                return std::none_of(closedTCands.begin(), closedTCands.end(), [&](const Trip::Cand& t) { return t.key == p.vendorKey; });
            });
            if (closedTCands.empty())
                spdlog::info("[errand] {:08X} '{}': no hours-closed shop holds work for him -- no watch", fid, NameOf(a_follower));
        }
        if (Config::g_errandInn.load())   // F3: the hours-closed shops (and their orders) ride the trip: the WATCH
            Reopen::Record(fid, playerTown->GetFormID(), std::move(closedTCands), std::move(closedPlanned), tin, innStopForWatch ? &*innStopForWatch : nullptr,
                           std::move(openTCandsForWatch), std::move(tripFactions));
        return true;
    }

    // ── public: the per-follower step (top of the logistics tick) ───────────────────────────────────
    bool Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now,
              const std::function<bool()>& a_healWants) {
        if (!a_follower) return false;
        const auto fid = a_follower->GetFormID();
        // R1b: a follower the SIMULATION was running and who is LOADED again (the player walked in): "it picks up".
        if (Sim::Has(fid)) return Sim::OnLoadedTick(a_follower, a_state);
        Rec r;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_recs.find(fid);
            if (it == g_recs.end()) return false;
            it->second.lastTickClk = Scheduler::ServiceClock();
            r = it->second;
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!Config::g_townErrands.load() || !Config::g_economy.load()) { Finish(fid, "town errands / economy switched off", kFailCooldownSec); return false; }
        if (a_follower->IsDead()) { Finish(fid, "follower dead", kFailCooldownSec); return false; }
        // An UNLOADED follower is the Sweep's: it hands him to the simulation after one more poll. His own logistics tick can land
        // first (he stays on the roster ~1-1.6 s after the unload and Scheduler::Tick runs before Errand::Sweep), so this tick
        // neither acts nor ends anything -- and does not sample his (now stale) position.
        if (!a_follower->Is3DLoaded()) return true;
        // The last LOADED position / cell: the simulation's start point and the entry-door search origin.
        r.lastPos = a_follower->GetPosition();
        if (auto* fc0 = a_follower->GetParentCell()) r.lastCell = fc0->GetFormID();
        if (APMFBridge::InCombat(a_follower) || (pc && pc->IsInCombat())) { Finish(fid, "combat -- he rejoins", kCombatCooldownSec); return false; }
        if (auto* avo = a_follower->AsActorValueOwner(); avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) {
            Finish(fid, "told to wait", 60.0f);
            return false;
        }
        // R7 review: the INN STAY has exactly two exits (the player leaving town, combat; marth 2026-10-19): a heal request does not end it.
        const bool innStay = r.phase == Phase::Resting && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn;
        if (!innStay && a_healWants && a_healWants()) { Finish(fid, "yields to a heal", 15.0f); return false; }
        // R7: the INN STAY has no cap of its own (marth 2026-10-19: he stays until the player leaves town; combat and "told to wait" above and
        // the Sweep's player-left-town end it). Every other phase, and the temple's prayer, keep both caps.
        if (!innStay && Scheduler::ServiceClock() - r.startClk > kRealCapSec) { Finish(fid, "timed out (240 s real time)", 60.0f); return false; }
        if (auto* cal = RE::Calendar::GetSingleton(); cal && !innStay) {
            const float minutes = (cal->GetDaysPassed() - r.startDays) * 24.0f * 60.0f;
            // R5: fErrandMaxGameMin is the allowance of ONE walk + trade (phase 1); a trip gets it once per PLANNED stop (Trip::CapGameMin).
            if (minutes > Trip::CapGameMin(r.stops.size())) {
                Finish(fid, std::format("older than the trip cap ({:.0f} game min > fErrandMaxGameMin x {} stop(s))", minutes, std::max<std::size_t>(1, r.stops.size())).c_str(), kFailCooldownSec);
                return false;
            }
        }
        // R5: the CURRENT STOP is over (a_ok: done / nothing left; else failed and skipped). The trip moves on to its next stop
        // (TRUE: r now describes it and is stored here) or ends (FALSE: Finish ran). A failure while a trade is IN FLIGHT here ends
        // the whole trip (its outcome would land on the next stop's wait).
        auto stopOver = [&](const std::string& a_why, bool a_ok, float a_cooldown, bool a_complete = false) -> bool {
            if (!a_ok && r.dispatched && r.phase == Phase::Trading) { Finish(fid, a_why.c_str(), a_cooldown); return false; }
            // R7: the INN stop's counter business (food) is over -- he is AT the inn, so he STAYS (rests / sits / sleeps) instead of leaving.
            if (r.phase != Phase::Walking && r.phase != Phase::Resting && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn) {
                SyncCur(r);
                if (Rest::Begin(a_follower, r.stops[r.cur], r.vendor, r.followerName, r.town, a_now)) {
                    APMFBridge::ReleaseErrandIdle(fid);   // the counter's ch.12 beat + ch.1 hold end: the work packages need him free to move
                    r.phase = Phase::Resting;
                    r.phaseAt = a_now;
                    r.dispatched = false;
                    spdlog::info("[errand] {:08X} '{}': the inn's counter business is over ({}) -- he STAYS", fid, r.followerName, a_why);
                    ArmLaterRetry(r, a_now);   // run 4: the skipped stops are retried from the inn stage
                    Store(fid, r);
                    return true;
                }
            }
            if (!NextStopOrFinish(fid, r, a_now, a_why, a_ok, a_cooldown, a_complete)) return false;
            Store(fid, r);
            return true;
        };
        // Run 5 (marth R1 "ALWAYS teleport a stuck follower to his destination, even in the player's view"; R4 "any real verified intentional cell being
        // unreachable is an error"): declare Harbinger's kIntent_Place to the CURRENT stop (ErrandPlace.cpp). TRUE = a placement is in flight (the Walking phase
        // polls it); FALSE = not attempted (he is in dialogue, or the stop's placements are spent) or it could not be declared (logged as an ERROR here, never
        // a mask): the caller falls back to giveUp, whose inn-stop backoff paces the next try. The trip's ch.19 claim is kept (Place never touches it).
        auto tryPlace = [&](const std::string& a_why) -> bool {
            if (const auto* ast = a_follower->AsActorState(); ast && ast->actorState2.talkingToPlayer) return false;   // in dialogue is not stuck (combat already ended the errand above)
            if (r.placeTries >= kPlaceTries || r.cur >= r.stops.size()) return false;
            SyncCur(r);
            const auto here = a_follower->GetPosition();
            std::string err;
            ++r.placeTries;
            if (!Unstick::Begin(fid, r.stops[r.cur], err)) {
                spdlog::error("[errand] {:08X} '{}': OBVIOUSLY STUCK ({}) at {:.0f},{:.0f},{:.0f} -> the placement could NOT be declared: {} -- the stop '{}' is unreachable",
                              fid, r.followerName, a_why, here.x, here.y, here.z, err, Trip::Label(r.stops, r.cur));
                return false;
            }
            r.placing = true;
            r.retryWait = false; r.stepOut = false; r.stepOutOk = false; r.needStepOut = false;
            spdlog::warn("[errand] {:08X} '{}': OBVIOUSLY STUCK ({}) at {:.0f},{:.0f},{:.0f} -> teleporting him to the stop {} via Harbinger Place (placement {}/{})",
                         fid, r.followerName, a_why, here.x, here.y, here.z, Trip::Label(r.stops, r.cur), r.placeTries, kPlaceTries);
            Store(fid, r);
            return true;
        };
        // Run 4: a walk to the CURRENT stop failed (a_why). Under kWalkRetries it is RETRIED after 5/15/30 s: TRUE = a retry is scheduled (the
        // caller returns true); FALSE = out of retries, the caller skips the stop (giveUp). a_repoint false = nothing to re-point (the shop was
        // unattended while he stands in it): the retry just re-checks. A first block never steps out; a repeat block within kStepOutMoveU of the
        // last one first walks him toward the player (kStepOutRadius), then re-points to the stop.
        auto retryWalk = [&](const std::string& a_why, bool a_repoint) -> bool {
            if (r.walkRetries >= kWalkRetries) {
                // mfo-272 (marth: "an obviously stuck follower is ALWAYS teleported to the destination"): the retries are spent, so the walk to this
                // stop is stuck, shop and inn alike, WHEREVER he now stands. Run 5 placed him only when he stood within kStepOutMoveU of the last block
                // (or for the inn), and the 2026-10-25 Winterhold field run showed why that was brittle: Auri's BLOCKED ends were each a few hundred
                // units apart (the engine's Movement Blocked package shuffled her between them), so the "same spot" test never held and 'Birna
                // Services' was SKIPPED with no placement at all. Only a refused / failed placement skips the stop now (its ERROR line, then giveUp).
                if (a_repoint) {
                    const bool sameSpot = r.haveBlockPos && a_follower->GetPosition().GetDistance(r.lastBlockPos) < kStepOutMoveU;
                    if (tryPlace(std::format("{}; {} retries spent ({})", a_why, kWalkRetries,
                                             sameSpot ? "he has not moved since the last block" : "he moved between the blocks but never got there"))) return true;
                    if (r.placeTries >= kPlaceTries)
                        spdlog::error("[errand] {:08X} '{}': {} -- {} retries spent and this stop's {} placement(s) are spent too: it is skipped", fid,
                                      r.followerName, a_why, kWalkRetries, kPlaceTries);
                }
                return false;
            }
            const int secs = kRetryBackoffS[r.walkRetries];
            ++r.walkRetries;
            const auto here = a_follower->GetPosition();
            r.needStepOut = a_repoint && r.haveBlockPos && here.GetDistance(r.lastBlockPos) < kStepOutMoveU;
            r.lastBlockPos = here; r.haveBlockPos = true;
            r.retryWait = true; r.retryRepoint = a_repoint; r.stepOut = false;
            r.retryAt = a_now + std::chrono::seconds(secs);
            spdlog::info("[errand] {:08X} '{}': {} -- RETRY {}/{} in {} s{} ({})", fid, r.followerName, a_why, r.walkRetries, kWalkRetries, secs,
                         r.needStepOut ? ", after a step-out (he has not moved since the last block)" : "", Trip::Label(r.stops, r.cur));
            Store(fid, r);
            return true;
        };
        // Run 4: out of retries -- the stop goes onto the retry-later list (the inn stage retries it) and is skipped.
        auto giveUp = [&](const std::string& a_why, float a_cooldown) -> bool {
            if (r.cur < r.stops.size()) r.stops[r.cur].retryLater = true;
            spdlog::warn("[errand] {:08X} '{}': {} -- {} retries spent, the stop is SKIPPED and kept on the retry-later list (the inn stage retries it)",
                         fid, r.followerName, a_why, kWalkRetries);
            return stopOver(a_why, false, a_cooldown);
        };
        // mfo-272 NPC-FIRST (marth 2026-10-25: "locating the NPC and navigating to them, if the shop is supposed to be open they will have the
        // dialogue"): the shop's vendor is not where this walk ended (or he left the loaded area). Locate him NOW (Vendors::Locate: wherever the
        // engine has him, loaded or not) and re-aim the SAME ch.19 claim at him: straight at the vendor when he is loaded where the follower is
        // (or both in the exterior grid), else a CELL leg into his interior (the engine paths the load doors natively; the ref leg to him follows
        // on arrival), else a REF leg to the vendor actor. TRUE = re-aimed (the walk goes on); FALSE = not located / not reachable / the cap is
        // spent (said here; the caller's unattended retries decide). Never a chest trade (marth: the chest stays the chest-placed rule only).
        auto relocate = [&](const char* a_why) -> bool {
            constexpr int kMaxRelocations = 3;
            if (r.cur >= r.stops.size() || r.stops[r.cur].kind != StopKind::Shop) return false;
            if (r.relocations >= kMaxRelocations) {
                spdlog::warn("[errand] {:08X} '{}': {} -- the vendor of '{}' was already re-located {} time(s); no further re-aim", fid, r.followerName, a_why,
                             Trip::Label(r.stops, r.cur), kMaxRelocations);
                return false;
            }
            Vendors::Where w;
            const auto ans = Vendors::Locate(r.faction, w);
            if (ans == Vendors::Answer::Pending) return false;   // mfo-273: the index / a trading verdict is in flight, not "not located"; the caller's retries ask again
            if (ans != Vendors::Answer::Located) {
                spdlog::warn("[errand] {:08X} '{}': {} -- the vendor of '{}' (faction {:08X}) could NOT be located: {} (members {}, dead {}, disabled {})", fid,
                             r.followerName, a_why, Trip::Label(r.stops, r.cur), r.faction, Vendors::AnswerName(ans), w.members, w.dead, w.disabled);
                return false;
            }
            auto* townLoc = r.town ? RE::TESForm::LookupByID<RE::BGSLocation>(r.town) : nullptr;
            if (w.asleep || (townLoc && w.town != townLoc)) {
                spdlog::warn("[errand] {:08X} '{}': {} -- the vendor of '{}' is {} ({})", fid, r.followerName, a_why, Trip::Label(r.stops, r.cur),
                             w.asleep ? "ASLEEP" : "NOT IN THIS TOWN", Vendors::Describe(w));
                return false;
            }
            auto* fc = a_follower->GetParentCell();
            if (!fc) return false;
            DestKind k = DestKind::VendorActor;
            RE::FormID dest = w.actor;
            float rad = kVendorRadius;
            if (w.loaded && (w.cell == fc->GetFormID() || (!w.interior && !fc->IsInteriorCell()))) {
                // straight to him
            } else if (w.interior && !APMFBridge::ErrandCrossCellsSupported()) {
                if (w.cell == fc->GetFormID()) return false;   // he is in the vendor's cell and the vendor is not loaded there: nothing to re-aim
                k = DestKind::Cell; dest = w.cell; rad = 0.0f;
            } else {
                k = DestKind::Ref;
            }
            if (k == r.destKind && dest == r.destForm) return false;   // already walking there: re-aiming at the same spot is no new route
            ++r.relocations;
            APMFBridge::ReleaseErrandIdle(fid);   // a stand-still hold filed for an unattended wait must not pin the walk
            r.destKind = k; r.destForm = dest; r.destRad = rad;
            r.vendor = k == DestKind::VendorActor ? dest : 0u;
            r.vendorName = w.name;
            r.stops[r.cur].vendorActor = w.actor;
            r.retryWait = r.stepOut = r.stepOutOk = r.needStepOut = false; r.retryAt = {};
            LogRepointFrom(fid, "errand re-point");
            bool walk = APMFBridge::RepointErrandTravel(fid, dest, rad);
            if (!walk) walk = APMFBridge::ClaimErrandTravel(fid, dest, rad);
            r.startClk = r.lastTickClk = Scheduler::ServiceClock();   // the 240 s cap is per attempt (run 4 F3)
            if (!walk) {
                spdlog::error("[errand] {:08X} '{}': VENDOR LOCATED ({}) but the walk to {:08X} could NOT be declared (Harbinger refused the ch.19 Repoint and a new claim)",
                              fid, r.followerName, Vendors::Describe(w), dest);
                return false;
            }
            SyncCur(r);
            spdlog::warn("[errand] {:08X} '{}': {} -- VENDOR LOCATED: {} -> walking to {} {:08X} (re-aim {}/{}, stop {})", fid, r.followerName, a_why,
                         Vendors::Describe(w), k == DestKind::Cell ? "his CELL" : k == DestKind::Ref ? "him (ref leg)" : "him", dest, r.relocations, kMaxRelocations,
                         Trip::Label(r.stops, r.cur));
            Store(fid, r);
            return true;
        };
        // R8: the smith stop's work: the Smith module's steps (its own clocks, the ch.9 station claim, the temper apply).
        if (r.phase == Phase::Smithing) {
            const auto st = Smith::Step(a_follower, a_now);
            if (st.res == Smith::Result::Continue) { Store(fid, r); return true; }
            Smith::End(fid, st.why.c_str());
            return stopOver(st.why, st.res == Smith::Result::Done, st.res == Smith::Result::Done ? Config::g_errandCooldownSec.load() : kFailCooldownSec);
        }
        // R7: the inn stay / the temple prayer: the Rest module's steps (its own claims and clocks), not the shop beats below.
        if (r.phase == Phase::Resting) {
            // Seen LOADED outside the inn / temple while the stay stands = he went in and came back out (the hold did not keep him; the Markarth
            // field bug, 2026-10-20): loud, the stop ends, no retry. (The stay is an inn / temple cell by construction; a smithy may be an exterior ref.)
            if (r.cur < r.stops.size() && !Rest::InStopCell(a_follower, r.stops[r.cur])) {
                auto* oc = a_follower->GetParentCell();
                spdlog::warn("[errand] {:08X} '{}': the stay at '{}' stands but he is LOADED in cell {:08X}, not in its cell {:08X} -- he went in and came back out. "
                             "The stop is ended, no retry", fid, r.followerName, r.stops[r.cur].vendorName, oc ? oc->GetFormID() : 0u, r.stops[r.cur].destForm);
                Rest::End(fid, "he left the inn / temple cell while the stay stood", true);   // the work claim + record end with the stop; one trip per visit
                return stopOver("he left the inn / temple cell while the stay stood", false, kFailCooldownSec);
            }
            // F3: a watched shop opened while he stays at the inn: the stay ends WITHOUT a trip mark and a new trip starts (the opened shops, the inn last).
            if (innStay) {
                Reopen::Plan plan;
                const auto from = a_follower->GetPosition();
                if (Reopen::Poll(fid, r.town, &from, plan) || PlanLateSells(a_follower, a_state, a_now, r.town, plan)) {
                    const RE::FormID townId = r.town;
                    Finish(fid, "a needed shop opened -- the stay ends, a new trip starts", 0.0f, false, true);
                    StartReopenedTrip(a_follower, townId, a_now, std::move(plan));
                    return true;
                }
            }
            // Run 4: from the INN STAGE the stops skipped for a failed walk are retried (up to kLaterRetries each, 60/120/240 s apart). The stay
            // ends WITHOUT a trip mark, the stop is walked to again, and the inn comes back as the last stop (rest only, the food is bought).
            if (innStay && a_now >= r.laterAt) {
                for (std::size_t i = 0; i < r.stops.size(); ++i) {
                    auto& ls = r.stops[i];
                    if (!ls.retryLater || ls.laterRetries >= kLaterRetries || ls.state != StopState::Skipped) continue;
                    ++ls.laterRetries; ls.retryLater = false;
                    const std::size_t innIdx = r.cur;
                    SyncCur(r);
                    spdlog::info("[errand] {:08X} '{}': inn stage -- retrying the skipped stop {} (later retry {}/{}); the stay ends without a trip mark",
                                 fid, r.followerName, Trip::Label(r.stops, i), ls.laterRetries, kLaterRetries);
                    Rest::End(fid, "retrying a skipped stop from the inn stage", true, true);
                    r.stops[innIdx].state = StopState::Pending;
                    r.stops[innIdx].order.reset();   // the food is bought; the return is rest only
                    APMFBridge::ReleaseErrandIdle(fid);
                    LoadStop(r, i);
                    r.phaseAt = a_now;
                    r.startClk = r.lastTickClk = Scheduler::ServiceClock();
                    if (auto* cal1 = RE::Calendar::GetSingleton()) r.startDays = cal1->GetDaysPassed();   // run 4 F3: the trip's game-min cap restarts with the retry
                    LogRepointFrom(fid, "errand re-point");
                    bool walk = r.destForm != 0 && APMFBridge::RepointErrandTravel(fid, r.destForm, r.destRad);
                    if (!walk && r.destForm != 0) walk = APMFBridge::ClaimErrandTravel(fid, r.destForm, r.destRad);
                    if (!walk) return stopOver("the retry walk could not be declared", false, kFailCooldownSec);
                    Store(fid, r);
                    return true;
                }
                r.laterAt = a_now + std::chrono::hours(24);   // nothing (left) to retry
            }
            const auto st = Rest::Step(a_follower, a_now);
            if (st.res == Rest::Result::Continue) { Store(fid, r); return true; }
            return stopOver(st.why, st.res == Rest::Result::Done, st.res == Rest::Result::Done ? Config::g_errandCooldownSec.load() : kFailCooldownSec);
        }
        // r.vendor == 0: still walking to the shop's PLACE (a cell / a ref); the vendor actor is found on arrival.
        RE::Actor* vendor = r.vendor ? RE::TESForm::LookupByID<RE::Actor>(r.vendor) : nullptr;
        // R5: a LATER stop's vendor may be unloaded while he walks there (he is in the previous shop's interior with the player): the
        // ch.19 leg decides that walk (an unreachable one ends DEST-GONE / STUCK and the stop is skipped). Phase 1 / the first stop: unchanged.
        const bool walkingToLaterStop = r.cur > 0 && r.phase == Phase::Walking && vendor && !vendor->IsDead() && !vendor->IsDisabled();
        // mfo-272: the vendor walked out of the loaded area while he walks to him (into his back room, home, a neighbouring cell): he is
        // LOCATED again and the walk re-aimed at where he is now, instead of the stop failing as "gone".
        if (r.vendor && r.phase == Phase::Walking && !walkingToLaterStop && vendor && !vendor->IsDead() && !vendor->IsDisabled() && !vendor->Is3DLoaded() &&
            relocate("the vendor left the loaded area during the walk"))
            return true;
        if (r.vendor && !(walkingToLaterStop && !vendor->Is3DLoaded()) &&
            (!vendor || vendor->IsDead() || vendor->IsDisabled() || !vendor->Is3DLoaded() || APMFBridge::InCombat(vendor)))
            return stopOver("the vendor is gone, dead, unloaded or in combat", false, kFailCooldownSec);
        const bool vendorLoaded = vendor && vendor->Is3DLoaded();
        const Idles& idles = IdleIds();
        if (!idles.give) { Finish(fid, "IdleGive did not resolve", 600.0f); return false; }
        const float dist = vendor ? a_follower->GetPosition().GetDistance(vendor->GetPosition()) : 0.0f;
        const float dz   = vendor ? std::fabs(a_follower->GetPosition().z - vendor->GetPosition().z) : 0.0f;

        // Advance to the next beat: request its idle (the first beat files the ch.1 hold + ch.12 claim, later beats
        // re-point the same claim). A synchronous refusal of the FIRST beat latches the feature.
        auto beginBeat = [&](Phase a_next, RE::FormID a_idle, const char* a_name) -> bool {
            if (!APMFBridge::PlayErrandIdle(fid, a_idle, r.vendor)) {
                Latch();
                Finish(fid, std::format("Harbinger refused the {} idle at the call", a_name).c_str(), 600.0f);
                return false;
            }
            if (a_next != Phase::Talking) r.browse = false;   // R4: the browsing beat is a Talking beat
            r.phase = a_next;
            r.phaseAt = a_now;
            r.beatRequested = true;
            r.confirmedAt = {};
            return true;
        };

        switch (r.phase) {
        case Phase::Walking: {
            if (r.placing) {   // run 5: a Harbinger Place is in flight; the leg is NOT read meanwhile (the move lands on Harbinger's game thread)
                const auto pr = Unstick::Poll(fid);
                if (pr.state == Unstick::State::Pending) { Store(fid, r); return true; }
                r.placing = false;
                if (pr.state != Unstick::State::Placed) {
                    const std::string why = pr.state == Unstick::State::Idle ? "the placement record was lost" : pr.text;
                    spdlog::error("[errand] {:08X} '{}': OBVIOUSLY STUCK -> teleport REFUSED / FAILED: {} -- the stop '{}' is UNREACHABLE and is not dropped silently; it is skipped, kept "
                                  "on the retry-later list, and backed off", fid, r.followerName, why, Trip::Label(r.stops, r.cur));
                    const auto bo = NoteWalkFailed(fid, r, a_now);
                    return giveUp(std::format("the stuck-walk teleport failed ({}; this stop backed off {} s)", why, bo), kFailCooldownSec);
                }
                spdlog::warn("[errand] {:08X} '{}': OBVIOUSLY STUCK -> PLACED {} (placement {}/{}); the walk to the stop carries on from there", fid, r.followerName, pr.text,
                             r.placeTries, kPlaceTries);
                r.walkRetries = 0; r.retryWait = r.stepOut = r.stepOutOk = r.needStepOut = r.haveBlockPos = false; r.retryAt = {};
                LogRepointFrom(fid, "errand re-point");
                bool walk = r.destForm != 0 && APMFBridge::RepointErrandTravel(fid, r.destForm, r.destRad);
                if (!walk && r.destForm != 0) walk = APMFBridge::ClaimErrandTravel(fid, r.destForm, r.destRad);
                r.startClk = r.lastTickClk = Scheduler::ServiceClock();   // the 240 s cap is per attempt (run 4 F3)
                if (!walk) {
                    spdlog::error("[errand] {:08X} '{}': after the placement the walk to {:08X} '{}' could NOT be re-declared (Harbinger refused the ch.19 Repoint and a new claim)",
                                  fid, r.followerName, r.destForm, r.vendorName);
                    return giveUp("the walk could not be re-declared after the placement", kFailCooldownSec);
                }
                Store(fid, r);
                return true;
            }
            if (r.retryWait) {   // run 4: a failed walk is waiting out its backoff; the leg state is NOT read meanwhile (a step-out leg ends Arrived at the player)
                if (a_now < r.retryAt) { Store(fid, r); return true; }
                if (r.retryRepoint && r.needStepOut && !r.stepOut) {
                    const bool so = pc && (APMFBridge::RepointErrandTravel(fid, pc->GetFormID(), kStepOutRadius) ||
                                           APMFBridge::ClaimErrandTravel(fid, pc->GetFormID(), kStepOutRadius));
                    r.stepOut = true;
                    r.stepOutOk = so; r.stepOutFrom = a_follower->GetPosition();   // run 5: the step-out's result is read when its wait ends
                    r.retryAt = a_now + std::chrono::seconds(kStepOutSec);
                    spdlog::info("[errand] {:08X} '{}': STEP-OUT before retry {}/{}: walking toward the player (radius {:.0f}, {} s) {}", fid, r.followerName,
                                 r.walkRetries, kWalkRetries, kStepOutRadius, kStepOutSec, so ? "declared" : "could NOT be declared (retrying straight away)");
                    if (!so) r.retryAt = a_now;
                    Store(fid, r);
                    return true;
                }
                // Run 5: the step-out walk's wait ended. Its leg ending Blocked, or him having moved < kStepOutMoveU (and not ARRIVED at the player) = stuck:
                // two blocked ends at one spot and a step-out that did not free him. Placed at the stop (R1) instead of a fourth blind retry.
                if (r.stepOut && r.stepOutOk) {
                    APMFBridge::LootLegState sl;
                    const bool read = APMFBridge::ReadErrandLeg(fid, sl) && sl.ours;
                    const bool blockedLeg = read && sl.state == APMF_API::kLeg_Blocked;
                    const bool arrivedLeg = read && sl.state == APMF_API::kLeg_Arrived;
                    const float moved = a_follower->GetPosition().GetDistance(r.stepOutFrom);
                    if (blockedLeg || (!arrivedLeg && moved < kStepOutMoveU)) {
                        if (tryPlace(std::format("two blocked ends within {:.0f} u and the step-out walk {} (moved {:.0f} u in {} s)", kStepOutMoveU,
                                                 blockedLeg ? "ended BLOCKED" : "did not free him", moved, kStepOutSec))) return true;
                    }
                }
                r.retryWait = false; r.stepOut = false; r.stepOutOk = false; r.needStepOut = false;
                if (r.retryRepoint) {
                    const char* how = "re-pointed";
                    LogRepointFrom(fid, "errand re-point");
                    bool walk = r.destForm != 0 && APMFBridge::RepointErrandTravel(fid, r.destForm, r.destRad);
                    if (!walk && r.destForm != 0) { walk = APMFBridge::ClaimErrandTravel(fid, r.destForm, r.destRad); how = "claimed afresh"; }
                    r.startClk = r.lastTickClk = Scheduler::ServiceClock();   // run 4 F3: the 240 s cap is per attempt
                    spdlog::info("[errand] {:08X} '{}': RETRY {}/{} -- walk to {:08X} '{}' {}", fid, r.followerName, r.walkRetries, kWalkRetries,
                                 r.destForm, r.vendorName, walk ? how : "could NOT be declared");
                    if (!walk) return giveUp("the retry walk could not be declared", kFailCooldownSec);
                } else {
                    spdlog::info("[errand] {:08X} '{}': RETRY {}/{} -- re-checking the shop's vendor", fid, r.followerName, r.walkRetries, kWalkRetries);
                }
                Store(fid, r);
                return true;
            }
            if (!vendor) {   // walking to the shop's PLACE (a PLVD cell / ref): the leg ends Arrived in the shop, then to the vendor
                APMFBridge::LootLegState pleg;
                if (APMFBridge::ReadErrandLeg(fid, pleg) && pleg.ours) {
                    if (pleg.state == APMF_API::kLeg_Arrived && r.destKind == DestKind::Cell && r.destForm) {
                        // A CELL leg reads Arrived only while he is IN the cell (the hold, ch.19 Key Concept 11a). Seen LOADED in another cell
                        // while the claim stands = he went in and came back out (the Markarth field bug, 2026-10-20): loud, the stop ends, no retry.
                        auto* pc0 = a_follower->GetParentCell();
                        if (pc0 && pc0->GetFormID() != r.destForm) {
                            spdlog::warn("[errand] {:08X} '{}': ARRIVED at cell {:08X} ('{}') but he is LOADED in cell {:08X} -- he went in and came back out "
                                         "(the hold did not keep him). The stop is ended, no retry", fid, r.followerName, r.destForm, r.vendorName, pc0->GetFormID());
                            const auto bo = NoteWalkFailed(fid, r, a_now);
                            return stopOver(std::format("he left the cell the walk arrived in (loaded in {:08X}; this shop backed off {} s)", pc0->GetFormID(), bo),
                                            false, kFailCooldownSec);
                        }
                    }
                    // v2.7.1 (2b, run 6): a REF place-walk that stands within talking reach on the same floor is an arrival even while the leg
                    // still reads WALKING (mirrors the vendor backstop below), so a placement that lands just outside the leg radius cannot stand 120 s.
                    bool refNear = false;
                    if (pleg.state == APMF_API::kLeg_Walking && r.destKind == DestKind::Ref && r.destForm) {
                        if (auto* dref = RE::TESForm::LookupByID<RE::TESObjectREFR>(r.destForm); dref && dref->GetParentCell() && a_follower->GetParentCell() &&
                            (dref->GetParentCell() == a_follower->GetParentCell() ||
                             (!dref->GetParentCell()->IsInteriorCell() && !a_follower->GetParentCell()->IsInteriorCell()))) {
                            const auto fp = a_follower->GetPosition(), dp = dref->GetPosition();
                            refNear = fp.GetDistance(dp) <= kArriveDist && std::abs(fp.z - dp.z) <= kSameFloorDz;
                            if (refNear) spdlog::info("[errand] {:08X} '{}': ARRIVED at the shop place {:08X} by MFO distance backstop while the leg still reads WALKING",
                                                      fid, r.followerName, r.destForm);
                        }
                    }
                    if (pleg.state == APMF_API::kLeg_Arrived || refNear) {
                        // R7: an INN / TEMPLE stop. The temple has no vendor; the inn has the innkeeper only when a food order was planned and he
                        // stands there -- otherwise the stop is REST ONLY (logged) and the work begins at once.
                        const StopKind kind = r.cur < r.stops.size() ? r.stops[r.cur].kind : StopKind::Shop;
                        auto beginWork = [&]() -> bool {
                            SyncCur(r);
                            if (!Rest::Begin(a_follower, r.stops[r.cur], 0, r.followerName, r.town, a_now))
                                return stopOver("the stop's work could not begin", false, kFailCooldownSec);
                            r.phase = Phase::Resting;
                            r.phaseAt = a_now;
                            if (r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn) ArmLaterRetry(r, a_now);   // run 4 F2
                            Store(fid, r);
                            return true;
                        };
                        if (kind == StopKind::Smith) {   // R8: the smithy's place: the work begins at once (no vendor, no counter business)
                            SyncCur(r);
                            if (!Smith::Begin(a_follower, r.stops[r.cur], r.followerName, a_now))
                                return stopOver("the smithing work could not begin", false, kFailCooldownSec);
                            APMFBridge::ReleaseErrandIdle(fid);
                            r.phase = Phase::Smithing;
                            r.phaseAt = a_now;
                            Store(fid, r);
                            return true;
                        }
                        if (kind == StopKind::Temple || (kind == StopKind::Inn && !r.order)) return beginWork();
                        // mfo-272: the vendor NPC the plan located (the stop's vendorActor) first, when he stands loaded and awake where the walk
                        // ended; then any member of the faction here (FindVendorActor, membership-based since mfo-272).
                        RE::NiPointer<RE::Actor> va;
                        if (kind == StopKind::Shop && r.cur < r.stops.size() && r.stops[r.cur].vendorActor)
                            va = Vendors::LoadedVendor(r.stops[r.cur].vendorActor, r.faction, a_follower);
                        if (!va) va = FindVendorActor(a_follower, r.faction, true);   // run 4: the unattended re-check also reads the all-cell scan
                        // mfo-272 NPC-FIRST: no vendor where the walk ended -> where IS he now? Re-aim the walk at him (relocate says why not).
                        if (!va && kind == StopKind::Shop && relocate("no vendor where the walk to the shop ended")) return true;
                        if (!va && kind == StopKind::Inn) {
                            spdlog::info("[errand] {:08X}: the inn has no innkeeper in his cell -- no food bought, the stop is rest only", fid);
                            return beginWork();
                        }
                        // marth 2026-10-21 (F4, "1 now"): a CHEST-PLACED shop (BuildTownShops' merchant-chest anchor) whose vendor is not here trades with
                        // the merchant chest, exactly like the off-screen simulation's TRADE_SIM (ErrandSim.cpp DispatchTrade: TradeBridge::VendorTrade with
                        // no vendor actor, the stored order, the trip purse cap). Every other shop keeps the unattended retries below.
                        if (!va && kind == StopKind::Shop && r.order && !r.order->skipped && ChestPlacedFaction(r.faction)) {
                            auto* chestRef = r.chest ? RE::TESForm::LookupByID<RE::TESObjectREFR>(r.chest) : nullptr;
                            if (chestRef) {
                                // R5: an EARLIER stop's late report is counted against the trip purse first (as the give beat does).
                                if (TradeBridge::TradeOutcome late; r.cur > 0 && !TradeBridge::HasLiveOrder(fid) && TradeBridge::TakeOutcome(fid, late) &&
                                                                    !late.aborted && late.spent > 0) {
                                    r.tripSpent += late.spent;
                                    if (!late.smithBought.empty()) Smith::NoteBought(fid, late.smithBought);
                                    spdlog::info("[errand] {:08X}: a late report from an earlier stop spent {}g -- counted against the trip purse", fid, late.spent);
                                }
                                TradeBridge::DiscardOutcome(fid);
                                const int purse = r.tripPurse >= 0 ? std::min(r.order->purse, std::max(0, r.tripPurse - r.tripSpent)) : r.order->purse;
                                if (TradeBridge::VendorTrade(a_follower, nullptr, chestRef, r.order->sell, r.order->needs, purse, r.order->buy, r.order->sig,
                                                             r.order->daysNow, r.order->vendorKey, &r.order->quote)) {
                                    spdlog::warn("[errand] {:08X} '{}': shop '{}' (faction {:08X}): vendor not present -- traded with the merchant chest {:08X} "
                                                 "(chest-placed shop): {} sell row(s), {} need(s), purse {}", fid, r.followerName, r.vendorName, r.faction, r.chest,
                                                 r.order->sell.size(), r.order->needs.size(), purse);
                                    r.phase = Phase::Trading;
                                    r.dispatched = true;
                                    r.dispatchedClk = Scheduler::ServiceClock();
                                    r.phaseAt = a_now;
                                    Store(fid, r);
                                    return true;
                                }
                                spdlog::warn("[errand] {:08X} '{}': shop '{}': vendor not present and the merchant-chest trade could not be dispatched "
                                             "(chest busy or the trade bridge is down) -- handled as an unattended shop", fid, r.followerName, r.vendorName);
                            }
                        }
                        if (!va) {   // run 4: an unattended shop is a blocked walk (he may be off duty / asleep / across a border): scan, wait, re-check, then retry-later
                            auto* cal0 = RE::Calendar::GetSingleton();
                            int seenN = 0; std::string seenList;
                            { std::scoped_lock lk(g_mx); if (const auto it = g_vendorScan.find(r.faction); it != g_vendorScan.end()) { seenN = it->second.members; seenList = it->second.summary; } }
                            spdlog::info("[errand] {:08X} '{}': shop '{}' (faction {:08X}) UNATTENDED at game hour {:.1f}: no awake loaded vendor in his cell or the last all-cell scan "
                                         "({} faction member(s) seen{}{})", fid, r.followerName, r.vendorName, r.faction, cal0 ? cal0->GetHour() : -1.0f, seenN,
                                         seenList.empty() ? "" : ": ", seenList);
                            PostVendorScan(r.faction);   // lands before the next re-check
                            // mfo-272 (marth: "claims hold for the duration of the mfo event"): a REF leg's arrival ENDS the leg and Harbinger drops its
                            // ch.9 offer, so his framework's own package came back for the whole wait (2026-10-25: Sven's SFF sandbox FE391803 ran for
                            // the 54 s of 'Warmaidens' re-checks). The ch.1 hold keeps him standing at the stop until the walk re-aims or the stop ends.
                            APMFBridge::HoldErrand(fid);
                            if (retryWalk("the shop is unattended (no vendor actor in the loaded shop)", false)) return true;
                            return giveUp("the shop is unattended (no vendor actor in the loaded shop)", kFailCooldownSec);
                        }
                        r.vendor = va->GetFormID();
                        r.vendorName = NameOf(va.get());
                        r.destKind = DestKind::VendorActor; r.destForm = r.vendor; r.destRad = kVendorRadius;
                        if (r.cur < r.stops.size()) r.stops[r.cur].vendorActor = r.vendor;
                        APMFBridge::ReleaseErrandIdle(fid);   // mfo-272: an unattended wait's ch.1 hold must not pin the walk on to the vendor (no-op without one)
                        // review F8: the same Repoint-else-fresh-claim road relocate uses (a claim Harbinger ended cannot be re-pointed).
                        const char* how = "re-pointed";
                        bool walkOn = APMFBridge::RepointErrandTravel(fid, r.vendor, kVendorRadius);
                        if (!walkOn) { walkOn = APMFBridge::ClaimErrandTravel(fid, r.vendor, kVendorRadius); how = "claimed afresh"; }
                        if (!walkOn) {
                            spdlog::error("[errand] {:08X} '{}': found the vendor '{}' ({:08X}) but the walk on to him could NOT be declared (Harbinger refused the ch.19 "
                                          "Repoint and a new claim)", fid, r.followerName, r.vendorName, r.vendor);
                            return giveUp("the walk on to the vendor could not be declared", kFailCooldownSec);
                        }
                        spdlog::info("[errand] {:08X}: found the shop's vendor '{}' ({:08X}) -- walking on to him (walk {})", fid, r.vendorName, r.vendor, how);
                    } else if (pleg.state == APMF_API::kLeg_Blocked) {
                        if (r.destKind == DestKind::Cell) Rest::LogBlockedDoors(a_follower, r.destForm, r.vendorName);   // passive: the door(s), lock level of each side
                        if (retryWalk("the walk to the shop ended BLOCKED", true)) return true;   // run 4: retry before the stop is skipped and the shop parked
                        { std::scoped_lock lk(g_mx); g_parked[r.faction] = a_now + kParkTime; }
                        return giveUp("the walk to the shop ended BLOCKED (a closed door?); shop parked", kFailCooldownSec);
                    } else if (pleg.state == APMF_API::kLeg_Failed) {
                        Finish(fid, "Harbinger could not start the leg (FAILED -- all travel slots busy? APMF.log says); queued", 6.0f);
                        return false;
                    } else if (pleg.state == APMF_API::kLeg_ActorGone) {
                        // Since Harbinger's cell hold (ch.19 Key Concepts 11a/11b) an UNLOADED follower is no longer ActorGone (the hand-off to
                        // the simulation is the Sweep's, off his unloaded 3D): ActorGone means dead or a reference that no longer resolves.
                        // This tick runs only for a LOADED follower, so it is a real loss: never silent (marth: never mask a failure).
                        spdlog::warn("[errand] {:08X} '{}': the walk to '{}' ended ACTOR-GONE while he is loaded and alive (Harbinger reports his reference "
                                     "lost) -- the stop is ended, no retry", fid, r.followerName, r.vendorName);
                        const auto bo = NoteWalkFailed(fid, r, a_now);
                        return stopOver(std::format("the walk to the shop ended: {} (this shop backed off {} s)", LegName(pleg.state), bo), false,
                                        kFailCooldownSec);
                    } else if (pleg.state > APMF_API::kLeg_Blocked && pleg.state <= APMF_API::kLeg_Released) {
                        WarnCombatDisagree(a_follower, pleg.state);
                        if (pleg.state != APMF_API::kLeg_CombatCancelled && retryWalk(std::format("the walk to the shop ended {}", LegName(pleg.state)), true)) return true;
                        const auto bo = NoteWalkFailed(fid, r, a_now);
                        return giveUp(std::format("the walk to the shop ended: {} (this shop backed off {} s)", LegName(pleg.state), bo), kFailCooldownSec);
                    }
                }
                Store(fid, r);
                return true;
            }
            bool arrived = false;
            APMFBridge::LootLegState leg;
            if (APMFBridge::ReadErrandLeg(fid, leg) && leg.ours) {
                if (leg.state == APMF_API::kLeg_Arrived) {
                    arrived = true;
                } else if (leg.state == APMF_API::kLeg_Blocked) {
                    // BLOCKED within talking reach (same floor) is an arrival (the museum deposit's rule); a block that
                    // stopped short FAILS loudly and PARKS that vendor for a while (a static block stays closed).
                    if (vendorLoaded && dist <= kBlockedReach && dz <= kSameFloorDz) {
                        arrived = true;
                        spdlog::info("[errand] {:08X}: walk to '{}' ended BLOCKED at {:.0f} u -- within talking reach, "
                                     "counted as ARRIVED", fid, r.vendorName, dist);
                    } else {
                        if (retryWalk(std::format("the walk ended BLOCKED at {:.0f} u from the vendor (blocker kind {})", dist, leg.blockerKind), true)) return true;   // run 4
                        {
                            std::scoped_lock lk(g_mx);
                            g_parked[r.vendor] = a_now + kParkTime;
                        }
                        return giveUp(std::format("the walk ended BLOCKED at {:.0f} u from the vendor (blocker kind {}); vendor parked for {} min",
                                                  dist, leg.blockerKind, std::chrono::duration_cast<std::chrono::minutes>(kParkTime).count()),
                                      kFailCooldownSec);
                    }
                } else if (leg.state == APMF_API::kLeg_Failed) {
                    // The leg could not start: the usual cause is all 8 ch.19 package slots busy (loot + deposit + errands).
                    // He QUEUES (a short retry), never parks the vendor and never falls back to the instant trade.
                    Finish(fid, "Harbinger could not start the leg (FAILED -- all travel slots busy? APMF.log says); queued", 6.0f);
                    return false;
                } else if (leg.state == APMF_API::kLeg_ActorGone) {
                    // See the place leg above: ActorGone is dead / unresolvable now (an unloaded follower is the Sweep's hand-off). Loud, no retry.
                    spdlog::warn("[errand] {:08X} '{}': the walk to '{}' ended ACTOR-GONE while he is loaded and alive (Harbinger reports his reference "
                                 "lost) -- the stop is ended, no retry", fid, r.followerName, r.vendorName);
                    const auto bo = NoteWalkFailed(fid, r, a_now);
                    return stopOver(std::format("the walk ended: {} (this vendor backed off {} s)", LegName(leg.state), bo), false, kFailCooldownSec);
                } else if (leg.state > APMF_API::kLeg_Blocked && leg.state <= APMF_API::kLeg_Released) {
                    WarnCombatDisagree(a_follower, leg.state);
                    if (leg.state != APMF_API::kLeg_CombatCancelled && retryWalk(std::format("the walk ended {}", LegName(leg.state)), true)) return true;   // run 4
                    const auto bo = NoteWalkFailed(fid, r, a_now);
                    return giveUp(std::format("the walk ended: {} (this vendor backed off {} s)", LegName(leg.state), bo), kFailCooldownSec);
                }
            }
            // Distance backstop: a leg read that is not ours yet, or a vendor who walked up to him (a loaded vendor only: R5's later stop).
            if (!arrived && vendorLoaded && dist <= kArriveDist && dz <= kSameFloorDz) {
                arrived = true;
                if (leg.ours && leg.state == APMF_API::kLeg_Walking)
                    spdlog::info("[errand] {:08X} '{}': ARRIVED by MFO distance backstop ({:.0f} u <= {:.0f} u, dz {:.0f}) while the Harbinger leg still reads WALKING (leg radius {:.0f} u, {} ms in state)",
                                 fid, r.followerName, dist, kArriveDist, dz, r.destRad, leg.msInState);
            }
            if (!arrived) { Store(fid, r); return true; }
            if (!r.announced) {   // one-shot: the Walking phase leaves here for the talk / give beat (a resume that was announced skips it)
                r.announced = true;
                detail::ToastTrading(r.followerName, r.vendorName, r.faction);
            }
            spdlog::info("[errand] {:08X} '{}': ARRIVED at '{}' ({:.0f} u) after {} s{} -- talk beat",
                         fid, r.followerName, r.vendorName, dist,
                         std::chrono::duration_cast<std::chrono::seconds>(a_now - r.start).count(),
                         r.planPosSet ? std::format(", {:.0f} u in a straight line from where the errand began",
                                                    a_follower->GetPosition().GetDistance(r.planPos))
                                      : std::string{});
            if (idles.browse && r.browse) {   // R4: found at the counter -- browse first, then the hand-over
                spdlog::info("[errand] {:08X}: browsing the counter of '{}' (IdleExamine, ch.12 + ch.1 hold)", fid, r.vendorName);
                if (!beginBeat(Phase::Talking, idles.browse, "browse")) return false;
            } else if (!idles.talk) {   // an old master without the dialogue idle: straight to the hand-over
                if (!beginBeat(Phase::Giving, idles.give, "give")) return false;
            } else if (!beginBeat(Phase::Talking, idles.talk, "talk")) {
                return false;
            }
            Store(fid, r);
            return true;
        }

        case Phase::Talking: {
            const auto rd = BeatReading(fid);
            const bool patience = a_now - r.phaseAt > kBeatPatience;
            const bool browsing = r.browse;   // R4: this Talking beat is the browsing idle (longer hold, no talk fallback)
            const auto talkHold = browsing ? kBrowseHold : kTalkHold;
            if (rd.verdict == APMFBridge::ErrandIdleVerdict::Played) {
                if (r.confirmedAt.time_since_epoch().count() == 0) {
                    r.confirmedAt = a_now;
                    spdlog::info("[errand] {:08X}: {} idle PLAYED{} ({})", fid, browsing ? "browse" : "talk", rd.weak ? " (weak)" : "", rd.detail);
                }
                if (a_now - r.confirmedAt >= talkHold && beginBeat(Phase::Giving, idles.give, "give")) {
                    Store(fid, r);
                    return true;
                }
                if (a_now - r.confirmedAt >= talkHold) return false;   // beginBeat finished the errand
            } else if (rd.verdict == APMFBridge::ErrandIdleVerdict::Failed && rd.unconfirmableWhy == APMF_API::kIdleUnconf_NoEntryEvents) {
                // Dialogue idles raise no notify event, so Harbinger can say neither played nor failed. The idle IS running (the claim is
                // live): hold the beat for its normal dwell, and do NOT try the fallback idle (it has the same problem).
                if (r.confirmedAt.time_since_epoch().count() == 0) {
                    r.confirmedAt = a_now;
                    spdlog::info("[errand] {:08X}: talk idle requested -- unconfirmable (Dialogue idles raise no event) ({})", fid, rd.detail);
                }
                if (a_now - r.confirmedAt >= talkHold) {
                    if (!beginBeat(Phase::Giving, idles.give, "give")) return false;
                }
            } else if (rd.verdict == APMFBridge::ErrandIdleVerdict::Failed ||
                       (rd.verdict == APMFBridge::ErrandIdleVerdict::Waiting && patience)) {
                // The dialogue idles may carry conditions that need a live conversation (design 4.2). One fallback idle,
                // then skip the talk: the hand-over is the beat the trade needs.
                if (!browsing && !r.talkFallbackUsed && idles.talkFallback && idles.talkFallback != idles.talk) {
                    r.talkFallbackUsed = true;
                    spdlog::info("[errand] {:08X}: talk idle did not play ({}) -- trying the fallback idle {:08X}",
                                 fid, rd.detail, idles.talkFallback);
                    if (!beginBeat(Phase::Talking, idles.talkFallback, "talk fallback")) return false;
                } else {
                    spdlog::info("[errand] {:08X}: {} beat skipped ({}) -- hand-over next", fid, browsing ? "browse" : "talk", rd.detail);
                    if (!beginBeat(Phase::Giving, idles.give, "give")) return false;
                }
            }
            Store(fid, r);
            return true;
        }

        case Phase::Giving: {
            const auto rd = BeatReading(fid);
            const bool patience = a_now - r.phaseAt > kBeatPatience;
            bool go = false;
            if (rd.verdict == APMFBridge::ErrandIdleVerdict::Played) {
                if (r.confirmedAt.time_since_epoch().count() == 0) {
                    r.confirmedAt = a_now;
                    spdlog::info("[errand] {:08X}: give idle PLAYED{} ({})", fid, rd.weak ? " (weak)" : "", rd.detail);
                }
                go = a_now - r.confirmedAt >= kGiveHold;
            } else if (rd.verdict == APMFBridge::ErrandIdleVerdict::Failed ||
                       (rd.verdict == APMFBridge::ErrandIdleVerdict::Waiting && patience)) {
                // The trade is the follower's decision, the animation is presentation: trade anyway, loudly (principle 7).
                spdlog::warn("[errand] {:08X}: the GIVE idle did not play ({}) -- trading anyway (the animation is "
                             "presentation; APMF.log '[apmf][idle]' names why)", fid, rd.detail);
                go = true;
            }
            if (!go) { Store(fid, r); return true; }

            // R5: an EARLIER stop's report that landed late (after its 45 s wait gave it up) is counted against the trip purse, then
            // cleared, so this stop's wait can only ever read its own report.
            if (TradeBridge::TradeOutcome late; r.cur > 0 && !TradeBridge::HasLiveOrder(fid) && TradeBridge::TakeOutcome(fid, late) &&
                                                !late.aborted && late.spent > 0) {
                r.tripSpent += late.spent;
                if (!late.smithBought.empty()) Smith::NoteBought(fid, late.smithBought);   // R8
                spdlog::info("[errand] {:08X}: a late report from an earlier stop spent {}g -- counted against the trip purse", fid, late.spent);
            }
            TradeBridge::DiscardOutcome(fid);
            ErrandProbe probe;
            probe.mode = ErrandProbe::Mode::kExecute;
            probe.vendors.push_back(vendor->GetHandle());
            // R5: this stop's SHARE of the trip (its rows, needs, wishes) and what the trip purse has left; a restored stop (no order)
            // re-plans in full on his live inventory and gold.
            const int purseCap = r.tripPurse >= 0 ? std::max(0, r.tripPurse - r.tripSpent) : -1;
            if (r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn) {
                // R7: the INN's counter business is its synthetic FOOD order (logistics/ErrandRest.cpp), not the economy probe's rows.
                const auto dr = r.order ? Rest::DispatchOrder(a_follower, vendor, *r.order, purseCap) : Rest::Dispatch::Nothing;
                if (dr == Rest::Dispatch::Nothing)
                    return stopOver("nothing to buy at the inn (no purse / no chest / no order)", true, kFailCooldownSec);
                probe.dispatched = dr == Rest::Dispatch::Sent;
            } else {
                EconomyErrandExecute(a_follower, a_state, a_now, probe, r.order.get(), purseCap);
                if (!probe.unplanned.empty()) {   // MISSED SELLS (b): sellable now, in no OTHER stop's order either -> a late sell (this stop's share was cut out in the probe)
                    std::vector<RE::FormID> late;
                    for (const auto f : probe.unplanned) {
                        bool handled = false;
                        for (std::size_t i = 0; i < r.stops.size() && !handled; ++i) {
                            if (i == r.cur) continue;
                            const auto& o = r.stops[i].order;
                            handled = o && std::any_of(o->sell.begin(), o->sell.end(), [&](const TradeBridge::SellRow& s) { return s.obj && s.obj->GetFormID() == f; });
                        }
                        if (!handled) late.push_back(f);
                    }
                    if (!late.empty()) Reopen::NoteLate(fid, r.town, late);
                }
            }
            if (probe.dispatched) {
                r.phase = Phase::Trading;
                r.dispatched = true;
                r.dispatchedClk = Scheduler::ServiceClock();
                r.phaseAt = a_now;
                spdlog::info("[errand] {:08X} '{}': trade DISPATCHED at '{}' (give beat done)", fid, r.followerName, r.vendorName);
                Store(fid, r);
                return true;
            }
            const bool skipped = std::any_of(probe.verdicts.begin(), probe.verdicts.end(),
                                             [](const ErrandProbe::Verdict& v) { return v.skipped; });
            if (skipped) {   // nothing new to trade with him since the plan (the empty-trade memo): done, no harm
                return stopOver("nothing to trade here any more (the empty-trade memo / nothing to buy or sell)", true, kFailCooldownSec);
            }
            // Not skipped and not dispatched: the chest holds another follower's live order (the second one WAITS,
            // never parks), or the trade bridge is down. Hold the give pose and retry until the patience runs out.
            if (a_now - r.phaseAt > kDispatchPatience + kBeatPatience) {
                return stopOver("the trade could not be dispatched (chest busy or the trade bridge is down)", false, kFailCooldownSec);
            }
            Store(fid, r);
            return true;
        }

        case Phase::Trading: {
            TradeBridge::TradeOutcome out;
            if (!TradeBridge::HasLiveOrder(fid) && TradeBridge::TakeOutcome(fid, out)) {
                if (out.aborted) {
                    r.dispatched = false;   // reported: nothing is in flight any more
                    return stopOver("the trade aborted (Papyrus found no usable chest)", false, kFailCooldownSec);
                }
                spdlog::info("[errand] {:08X} '{}': trade REPORTED at '{}' -- sold {} for {}g, bought {} for {}g",
                             fid, r.followerName, r.vendorName, out.soldCount, out.soldValue, out.boughtCount, out.spent);
                r.tripSpent += std::max(0, out.spent);   // R5: the next stop's purse cap
                if (!out.smithBought.empty()) Smith::NoteBought(fid, out.smithBought);   // R8: real stock for a later hand-off
                if (!out.unsold.empty() && r.cur < r.stops.size()) {   // MISSED SELLS (a): the vendor's gold ran out -- watch the shop, and offer the rows to the other open shops too (b)
                    const Stop* innS = nullptr;
                    for (const auto& st : r.stops) if (st.kind == StopKind::Inn) innS = &st;
                    Reopen::NoteUnsold(fid, r.town, r.stops[r.cur], r.order.get(), out, innS);
                    std::vector<RE::FormID> uf;
                    for (const auto& row : out.unsold) if (row.obj) uf.push_back(row.obj->GetFormID());
                    Reopen::NoteLate(fid, r.town, uf);
                }
                if (r.cur < r.stops.size()) {   // MISSED BUY: a supply quota the vendor's stock could not fill is re-planned to another open shop of the plan
                    Reopen::NoteUnfilled(fid, r.town, r.stops[r.cur], r.order.get(), out);
                }
                r.dispatched = false;
                r.bought = out.boughtCount > 0;
                if (r.bought && idles.take && r.vendor) {   // a merchant-chest trade (no vendor actor) has no one to take from
                    if (!beginBeat(Phase::Taking, idles.take, "take")) return false;
                    Store(fid, r);
                    return true;
                }
                return stopOver("trade done", true, Config::g_errandCooldownSec.load(), true);
            }
            if (Scheduler::ServiceClock() - r.dispatchedClk > kTradeWaitSec) {
                r.dispatched = false;   // VendorTrade reaps an order at 30 s: nothing is in flight after 45 s
                TradeBridge::DiscardOutcome(fid);   // R5 (review SEV-5): the next stop's wait never reads this stop's outcome
                return stopOver("the trade never reported (45 s)", false, kFailCooldownSec);
            }
            Store(fid, r);
            return true;
        }

        case Phase::Taking: {
            const auto rd = BeatReading(fid);
            const bool patience = a_now - r.phaseAt > kBeatPatience;
            if (rd.verdict == APMFBridge::ErrandIdleVerdict::Played) {
                if (r.confirmedAt.time_since_epoch().count() == 0) {
                    r.confirmedAt = a_now;
                    spdlog::info("[errand] {:08X}: take idle PLAYED{} ({})", fid, rd.weak ? " (weak)" : "", rd.detail);
                }
                if (a_now - r.confirmedAt >= kTakeHold) {
                    return stopOver("trade done", true, Config::g_errandCooldownSec.load(), true);
                }
            } else if (rd.verdict == APMFBridge::ErrandIdleVerdict::Failed ||
                       (rd.verdict == APMFBridge::ErrandIdleVerdict::Waiting && patience)) {
                spdlog::info("[errand] {:08X}: take beat skipped ({})", fid, rd.detail);   // the purchase is already made
                return stopOver("trade done", true, Config::g_errandCooldownSec.load(), true);
            }
            Store(fid, r);
            return true;
        }

        case Phase::Smithing:  // R8 (handled above, before the vendor checks)
        case Phase::Resting:   // (handled above, before the vendor checks)
            return true;
        }
        return true;
    }

    // ── public: the owner-independent backstop ───────────────────────────────────────────────────────
    void Sweep() {
        {   // F2: the combat-end edge clears the trip-done marks (a FRESH plan runs after the combat cooldown). Sampled HERE, every pump lap: TryStart never runs during party combat.
            auto* pc0 = RE::PlayerCharacter::GetSingleton();
            Rest::NoteCombat(pc0 && pc0->IsInCombat());
        }
        Sim::Sweep();   // R1b: the off-screen errands (their own table; a lap of the game-time clock)

        std::vector<std::pair<RE::FormID, std::string>> ends;
        std::vector<RE::FormID> handoffs;
        {
            std::scoped_lock lk(g_mx);
            if (g_recs.empty()) return;
            const auto now = Clock::now();
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const bool loading = InLoadingMenu();
            const double clk = Scheduler::ServiceClock();
            const RE::BGSLocation* here = pc ? LocationTypes::TownAnchor(pc->GetCurrentLocation()) : nullptr;
            const RE::FormID hereId = here ? here->GetFormID() : 0u;
            const auto ids = Followers::ActiveSnapshot();
            for (auto& [fid, r] : g_recs) {
                std::string why;
                auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
                if (!Config::g_logistics.load() || !Config::g_townErrands.load()) why = "logistics / town errands off";
                else if (!f || f->IsDead()) why = "follower dead";
                else if (!f->Is3DLoaded()) {
                    // R1b HAND-OFF: the follower left the player's loaded area (the player took a load door, or he walked through
                    // a shop door: Harbinger's ch.19 leg reads ActorGone). Not an end: the errand goes OFF SCREEN and is simulated
                    // against the real chest and gold. He must still be a follower; the roster drop (3 sweeps after the unload)
                    // is NOT a dismissal, so eligibility is read from the actor, not from the roster.
                    if (!Followers::IsEligibleFollower(f)) why = "no longer a follower (dismissed)";
                    else if (loading || !pc || !pc->Is3DLoaded()) continue;   // defer while a load is in progress (the record is what remembers him)
                    else if (r.unloadedSince.time_since_epoch().count() == 0) r.unloadedSince = now;
                    else if (now - r.unloadedSince >= kHandoffPoll) handoffs.push_back(fid);   // unloaded for one more poll: not a 3D hiccup
                }
                else {
                    // F1 (field run 2, the inn stay): a follower who is merely OFF THE ROSTER (a transient drop of the active list while
                    // he is loaded, Followers.cpp) is AWAY, not gone: eligibility read from the actor is the dismissal test (the same rule
                    // as the unloaded branch above). Away = the hand-off to the simulation after kHandoffPoll, the claim kept; he is not
                    // serviced, so the stall check below must not read his stale lastTickClk.
                    const bool offRoster = ids && std::find(ids->begin(), ids->end(), fid) == ids->end();
                    if (!offRoster) r.unloadedSince = {};
                    r.lastPos = f->GetPosition();   // sampled every lap: the simulation starts from the last LOADED position
                    if (auto* fc = f->GetParentCell()) r.lastCell = fc->GetFormID();
                    if (APMFBridge::InCombat(f) || (pc && pc->IsInCombat())) why = "combat -- he rejoins";   // substrate: not combat (v26)
                    else if (offRoster) {
                        if (!Followers::IsEligibleFollower(f)) why = "no longer a follower (dismissed)";
                        else if (loading || !pc || !pc->Is3DLoaded()) continue;
                        else if (r.unloadedSince.time_since_epoch().count() == 0) {
                            r.unloadedSince = now;
                            spdlog::info("[errand] {:08X}: off the active roster while loaded -- treated as away (not a dismissal); handed to the simulation if it lasts", fid);
                        }
                        else if (now - r.unloadedSince >= kHandoffPoll) handoffs.push_back(fid);
                    }
                    else if (clk - r.lastTickClk > kStallSec) why = "the follower is no longer serviced";
                    else if (clk - r.startClk > kRealCapSec &&
                             !(r.phase == Phase::Resting && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn))   // R7: an inn stay has no cap
                        why = "timed out (240 s real time)";
                    else if (!loading && pc && pc->Is3DLoaded()) {
                        // PLAYER LEAVES TOWN (marth 2026-10-18): a LOADED errand ends and his follow package walks him back.
                        // (An unloaded one is the simulation's: it resolves the errand and recalls him.)
                        if (hereId != r.town) {
                            if (r.townLostSince.time_since_epoch().count() == 0) r.townLostSince = now;
                            else if (now - r.townLostSince >= kLeftTownGrace) why = "the player left the town";
                        } else {
                            r.townLostSince = {};
                        }
                    }
                }
                if (!why.empty()) ends.emplace_back(fid, std::move(why));
            }
        }
        for (const auto& [fid, why] : ends)
            Finish(fid, why.c_str(), why.rfind("combat", 0) == 0 ? kCombatCooldownSec : kFailCooldownSec);

        for (const auto fid : handoffs) {
            Rec r;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_recs.find(fid);
                if (it == g_recs.end()) continue;
                r = it->second;
                g_recs.erase(it);
            }
            // HOLD (marth 2026-10-20: "mfo claims need to hold for the duration of the mfo event"; Harbinger ch.19 Key Concept 11a/11b): the claim
            // that holds him (the ch.19 cell hold, or the ch.9 work claim) is NOT released here. Harbinger keeps a claim through the unload
            // (an unloaded actor is not gone); releasing would hand him straight back to his follow package, the field bug (Markarth
            // 2026-10-20). The simulation owns the claim now and releases it when the errand event ends (ErrandSim.cpp Drop / OnLoadedTick).
            if (r.phase == Phase::Smithing) Smith::End(fid, "he unloaded -- handed to the simulation", false);   // R8: SMITH_SIM starts without a plan (Begin erased it): no second apply; a dispatched temper is still charged on his next load; the work claim STANDS
            const bool wasResting = r.phase == Phase::Resting;
            if (wasResting) Rest::End(fid, "he unloaded -- handed to the simulation", false);   // R7: the stay continues off screen (REST_SIM); the work claim STANDS
            SimStart st;
            st.follower = fid; st.followerName = r.followerName; st.vendorName = r.vendorName;
            st.faction = r.faction; st.chest = r.chest; st.town = r.town; st.vendorActor = r.vendor;
            st.destKind = r.destKind; st.destForm = r.destForm; st.destRad = r.destRad;
            st.order = r.order; st.lastPos = r.lastPos; st.lastCell = r.lastCell; st.startDays = r.startDays;
            SyncCur(r);   // R5: the itinerary goes off screen with him
            st.stops = r.stops; st.cur = r.cur; st.tripPurse = r.tripPurse; st.tripSpent = r.tripSpent;
            st.arrived = r.phase != Phase::Walking;
            if (!st.arrived) {   // mfo-273 item C: Harbinger's own verdict on the walk (its leg ARRIVED) counts as arrived too
                APMFBridge::LootLegState sl;
                if (APMFBridge::ReadErrandLeg(fid, sl) && sl.ours && sl.state == APMF_API::kLeg_Arrived) st.arrived = true;
            }
            st.tradeDispatched = r.dispatched;
            st.tradeDone = r.phase == Phase::Taking || (wasResting && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Inn);   // R7: an inn stay is past its trade
            Sim::Begin(std::move(st));
            // Sim::Begin registers nothing when the actor does not resolve: no simulation record will ever release the standing claim.
            if (!Sim::Has(fid)) {
                spdlog::warn("[errand] {:08X} '{}': handed off but the simulation did not take him (actor does not resolve) -- claims released", fid, r.followerName);
                APMFBridge::ReleaseErrand(fid);
            }
        }
    }
}
