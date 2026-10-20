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
#include "ErrandCounter.h"   // R4: the learned vendor map + the browsing hold
#include "Runtime.h"         // IsVerified1_6_1170: the PLVD layout (faction vendorLocation) is verified on that build only
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
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand {

    namespace {

        // ── constants (each sized from a real cadence, principle 9) ───────────────────────────────────
        // The service tick (kLogisticsInterval ~1 s) is the errand's step clock, so every wait below is a
        // handful of ticks. The Harbinger idle window (kIdleConfirmWindowMs 3 s) is judged by Harbinger, not here.
        constexpr float kVendorRadius   = 110.0f;   // ch.19 arrival radius around the vendor (talking distance; APMF clamps [50,512])
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

        enum class Phase { Walking, Talking, Giving, Trading, Taking };

        const char* PhaseName(Phase p) {
            switch (p) {
                case Phase::Walking: return "WALKING";
                case Phase::Talking: return "TALKING";
                case Phase::Giving:  return "GIVING";
                case Phase::Trading: return "TRADING";
                case Phase::Taking:  return "TAKING";
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

        // ── the catalog (R1a: loaded vendor actors) ───────────────────────────────────────────────────
        // SEAM: `place` (the faction's PLVD cell / reference) is where an UNLOADED shop attaches in R1b. R1a does
        // not read it: PackageLocation::Data is a union of TESForm* and ObjectRefHandle and which member a
        // faction's file-loaded PLVD fills is not verified, so R1a takes the place from the loaded vendor ACTOR.
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
        };

        // R1b: the town's shops from the vendor FACTIONS (design 2.2), cached per town -- immutable once built. The place is the
        // faction's PLVD vendorLocation, read the way 1.6.1170's loader leaves it (disassembled, 2026-10-19): TESFaction::Load
        // builds the PackageLocation, InitItem's resolver (rva 0x49c630) then turns kInCell into a TESObjectCELL* in
        // data.object and kNearReference into a 32-bit ObjectRefHandle in data.refHandle. Other location types are skipped.
        // Verified on 1.6.1170 only (per-runtime rule): elsewhere the list is empty and shops are the loaded-vendor ones (R1a).
        struct ShopStatic { RE::FormID faction = 0, chest = 0, place = 0; DestKind kind = DestKind::Cell; float rad = 0.0f;
                            std::uint8_t cls = 0; };   // R6: 0 plain, 1 FENCE (perk + conditions gate), 2 CONDITIONED (a spouse-store candidate)
        std::unordered_map<RE::FormID, std::vector<ShopStatic>> g_townShops;   // town BGSLocation id -> shops; guarded by g_mx

        std::vector<ShopStatic> BuildTownShops(const RE::BGSLocation* a_town) {
            std::vector<ShopStatic> out;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh || !a_town) return out;
            int seen = 0;
            for (auto* fac : dh->GetFormArray<RE::TESFaction>()) {
                if (!fac || !fac->IsVendor() || !fac->OffersServices()) continue;
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;
                // R6: a fence and a conditioned vendor are KEPT, tagged; BuildCatalog's gates (ErrandShops.cpp) decide per follower.
                const bool conditioned = fac->vendorData.vendorConditions && fac->vendorData.vendorConditions->head;
                const std::uint8_t cls = ErrandShops::IsFence(fac) ? 1 : conditioned ? 2 : 0;
                auto* loc = fac->vendorData.vendorLocation;
                if (!loc) continue;
                ShopStatic sh;
                sh.faction = fac->GetFormID();
                sh.chest   = chest->GetFormID();
                sh.cls     = cls;
                const RE::BGSLocation* placeLoc = nullptr;
                switch (loc->locType.get()) {
                case RE::PackageLocation::Type::kInCell: {
                    auto* cell = loc->data.object ? loc->data.object->As<RE::TESObjectCELL>() : nullptr;
                    if (!cell) continue;
                    sh.kind = DestKind::Cell; sh.place = cell->GetFormID(); placeLoc = cell->GetLocation();
                    break;
                }
                case RE::PackageLocation::Type::kNearReference: {
                    auto ref = loc->data.refHandle.get();
                    if (!ref) continue;
                    sh.kind = DestKind::Ref; sh.place = ref->GetFormID();
                    sh.rad = std::clamp(static_cast<float>(loc->rad), 50.0f, 512.0f);   // ch.19 clamps the same range
                    placeLoc = ref->GetEditorLocation();
                    if (!placeLoc) placeLoc = ref->GetCurrentLocation();
                    break;
                }
                default: continue;   // the other PLVD types are not shop PLACES (spouse stores, package-relative): skipped
                }
                ++seen;
                if (placeLoc && LocationTypes::TownAnchor(placeLoc) == a_town) out.push_back(sh);
            }
            spdlog::info("[errand] shop catalog for '{}': {} of {} vendor faction(s) with a PLVD place sit in this town",
                         a_town->GetName() ? a_town->GetName() : "?", out.size(), seen);
            return out;
        }

        std::vector<Cand> BuildCatalog(RE::Actor* a_follower, const RE::BGSLocation* a_playerTown,
                                       Clock::time_point a_now, int& a_dropped) {
            std::vector<Cand> out;
            a_dropped = 0;
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
            const float maxFromPlayer = Config::g_travelRadius.load();
            std::unordered_map<RE::FormID, bool> seenChest;
            bool waiting = false;   // R6: a fence / spouse gate is still waiting for its main-thread condition answer
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
                // perk AND its own conditions (Q1); a conditioned non-fence vendor is offered only as the SPOUSE's store. Every other
                // conditioned vendor is still dropped. The gates are ErrandShops.cpp's (conditions are evaluated on the main thread).
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
                if (!OpenAt(fac, hour)) { ++a_dropped; continue; }
                Counter::Learn(v, fac->GetFormID(), a_now);   // R4: where this vendor actually stands (session-only map)
                auto* chest = fac->vendorData.merchantContainer;
                if (!chest || !chest->GetContainer()) continue;   // a malformed merchant link (see EconomyProbe)
                // The same town as the player (a roadside vendor outside any town is not an errand destination).
                const auto* vTown = LocationTypes::TownAnchor(v->GetCurrentLocation());
                if (!vTown || vTown != a_playerTown) { ++a_dropped; continue; }
                const float dPlayer = v->GetPosition().GetDistance(pc->GetPosition());
                if (dPlayer > maxFromPlayer) { ++a_dropped; continue; }
                if (const auto it = parked.find(v->GetFormID()); it != parked.end() && a_now < it->second) continue;
                if (backedOff.contains(chest->GetFormID())) { noteBackoff(chest->GetFormID(), v->GetName()); continue; }
                if (seenChest.contains(chest->GetFormID())) continue;   // one actor per chest is enough
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
                out.push_back(c);
            }

            // R1b: shops whose vendor is NOT loaded (an interior while the player is outside), from the faction catalog.
            if (Runtime::IsVerified1_6_1170() && a_playerTown) {
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
                    if (out.size() >= kMaxCands) break;
                    if (seenChest.contains(sh.chest)) continue;   // a loaded vendor already covers this chest
                    auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(sh.faction);
                    if (!fac) continue;
                    if (sh.cls != 0 && !gated.contains(sh.faction)) {   // R6: a fence / conditioned shop with no loaded vendor: subject = the player / the spouse
                        const auto g = sh.cls == 1 ? ErrandShops::FenceGate(a_follower, fac, RE::ActorHandle{})
                                                   : ErrandShops::SpouseGate(a_follower, fac, RE::ActorHandle{});
                        if (g == ErrandShops::Gate::Closed) { ++a_dropped; continue; }
                        if (g == ErrandShops::Gate::Pending) { waiting = true; continue; }
                    } else if (sh.cls != 0) continue;   // its loaded vendor was gated above (and refused or was already listed)
                    if (!OpenAt(fac, hour)) { ++a_dropped; continue; }
                    if (const auto it = parked.find(sh.faction); it != parked.end() && a_now < it->second) continue;
                    if (backedOff.contains(sh.chest)) { noteBackoff(sh.chest, fac->GetName()); continue; }
                    float d = 0.0f;
                    RE::NiPoint3 placePos{};
                    if (sh.kind == DestKind::Ref) {
                        auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(sh.place);
                        if (!ref) continue;
                        placePos = ref->GetPosition();
                        d = placePos.GetDistance(pc->GetPosition());
                        if (d > maxFromPlayer) { ++a_dropped; continue; }
                    } else {
                        const auto it = cellDoorDist.find(sh.place);
                        if (it == cellDoorDist.end()) { ++a_dropped; continue; }
                        d = it->second;
                        placePos = cellDoorPos[sh.place];
                        if (d > maxFromPlayer) { ++a_dropped; continue; }
                    }
                    seenChest[sh.chest] = true;
                    Cand c;
                    c.id = sh.faction; c.fac = fac; c.chest = sh.chest; c.dist = d;
                    c.kind = sh.kind; c.destForm = sh.place; c.destRad = sh.rad;
                    c.pos = placePos; c.havePos = true;
                    out.push_back(c);
                }
            }
            if (waiting) {   // never commit a trip that silently lacks the fence / spouse store: re-plan on the next cadence (6 s)
                spdlog::info("[errand] {:08X}: plan waiting for a vendor-condition answer (fence / spouse gate) -- re-planning shortly",
                             a_follower->GetFormID());
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
        RE::NiPointer<RE::Actor> FindVendorActor(RE::Actor* a_near, RE::FormID a_faction) {
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
                auto* fac = VendorFactionOf(v);
                if (fac && fac->GetFormID() == a_faction) return p;
            }
            return {};
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
        void Finish(RE::FormID a_fid, const char* a_why, float a_cooldownSec, bool a_complete = false) {
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
            APMFBridge::ReleaseErrand(a_fid);   // idle, hold, walk (in that order)
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
            r.destKind = s.destKind; r.destForm = s.destForm; r.destRad = s.destRad;
            r.order = s.order;
            r.beatRequested = r.talkFallbackUsed = r.dispatched = r.bought = r.announced = false;
            r.browse = false;   // R4: the browsing beat belongs to the stop it was set for (R4 MAP WHAT BREAKS)
            r.confirmedAt = {};
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
        {
            std::scoped_lock lk(g_mx);
            auto it = g_recs.find(a_follower);
            if (it != g_recs.end()) {
                const Rec& r = it->second;
                const std::string shop = (r.vendorName.empty() || r.vendorName == "?") ? std::string("the shop") : r.vendorName;
                return r.phase == Phase::Walking ? "Walking to " + shop : "Trading with " + shop;
            }
        }
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
            g_backoff.clear();
            g_backoffLogged.clear();
            g_townShops.clear();
            g_townId = 0;
            g_townSince = {};
        }
        Sim::ClearTransientState();
        Counter::Clear();
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
                    case Phase::Taking:  s.stage = SavedStage::PhysTaking; break;
                }
                if (r.phase != Phase::Walking) s.flags |= Saved::kArrived;
                if (r.dispatched)              s.flags |= Saved::kDispatched;
                if (r.phase == Phase::Taking)  s.flags |= Saved::kTradeDone;
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
            auto vendor = FindVendorActor(a_follower, a_info.faction);
            // R2: a physical errand restored from the save walks on to its vendor ACTOR even when the vendor is not in his cell (an
            // exterior stall one cell over), provided that actor is loaded and usable.
            if (!vendor && a_info.destKind == DestKind::VendorActor && a_info.destForm) {
                if (auto* va = RE::TESForm::LookupByID<RE::Actor>(a_info.destForm);
                    va && va->Is3DLoaded() && !va->IsDead() && !va->IsDisabled() && !APMFBridge::InCombat(va))
                    vendor = RE::NiPointer<RE::Actor>(va);
            }
            const auto fid = a_follower->GetFormID();
            // No vendor actor in HIS cell: he is not in the shop (he was handed off mid-walk and loaded again elsewhere), so
            // the walk to the shop's place simply resumes. No place either (a vendor actor was the destination): give up.
            if (!vendor && (a_info.destKind == DestKind::VendorActor || a_info.destForm == 0)) return false;
            if (!APMFBridge::ClaimErrandTravel(fid, vendor ? vendor->GetFormID() : a_info.destForm,
                                               vendor ? kVendorRadius : a_info.destRad)) return false;
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
                Counter::NoteResume(fid, r.followerName, a_follower, vendor.get(), r.faction);
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

    // ── public: the planner (the idle branch of ServiceFollower) ─────────────────────────────────────
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
        if (g_latched.load()) return true;

        const auto fid = a_follower->GetFormID();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc || !pc->Is3DLoaded()) return true;

        // The player's town + how long he has been in it (the settle clock). Tracked on every call.
        const RE::BGSLocation* playerTown = LocationTypes::TownAnchor(pc->GetCurrentLocation());
        Clock::time_point townSince{};
        {
            std::scoped_lock lk(g_mx);
            const RE::FormID tid = playerTown ? playerTown->GetFormID() : 0u;
            if (tid != g_townId) { g_townId = tid; g_townSince = a_now; }
            townSince = g_townSince;
            if (g_recs.contains(fid)) return true;   // (Tick owns him)
            if (Sim::Has(fid)) return true;          // (the simulation owns him)
            if (const auto it = g_cool.find(fid); it != g_cool.end() && a_now < it->second) return true;
            if (const auto it = g_nextPlan.find(fid); it != g_nextPlan.end() && a_now < it->second) return true;
        }
        if (!playerTown) return true;
        if (a_now - townSince < std::chrono::milliseconds(static_cast<long long>(Config::g_errandSettleSec.load() * 1000.0f)))
            return true;

        // A cheap gate set first; the plan below walks a cell and the follower's inventory.
        auto* ui = RE::UI::GetSingleton();
        if (ui && ui->GameIsPaused()) return true;
        if (APMFBridge::InCombat(a_follower) || pc->IsInCombat()) return true;   // a Harbinger substrate is not combat (v26)
        if (SlotOf(fid)) return true;   // looting: loot and errand never overlap
        auto* avo = a_follower->AsActorValueOwner();
        if (avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) return true;   // told to wait
        if (a_follower->IsOnMount() || a_follower->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) return true;
        if (!a_follower->Is3DLoaded()) return true;
        if (LocationTypes::TownAnchor(a_follower->GetCurrentLocation()) != playerTown) return true;   // he is not in the player's town
        if (auto* q = Forms::g_tradeQuest; !q || !q->IsRunning()) return true;   // no trade bridge: nothing could be traded

        {
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kPlanInterval;
        }

        int dropped = 0;
        auto cands = BuildCatalog(a_follower, playerTown, a_now, dropped);
        if (cands.empty()) return true;
        // R8: the smithing want list is built on the MAIN thread (the tempering entry point); the plan reads it, so a missing or old
        // one is queued first and the plan is made on the next cadence. Off (smithing off / not hooked in this build) costs nothing.
        if (Smith::Prepare(a_follower) == Smith::Prep::Pending) return true;

        // PLAN: the SAME code a trade uses evaluates every candidate -- loaded vendor actors AND faction-only shops -- and hands
        // back the ORDER for each, built now while the follower is loaded (the simulation replays it; MFO never reads an
        // unloaded follower's inventory).
        std::vector<ErrandShop> shops;
        shops.reserve(cands.size());
        for (auto& c : cands) shops.push_back(ErrandShop{ c.fac, c.handle });
        std::vector<ErrandPlanned> planned;
        EconomyErrandPlan(a_follower, a_state, a_now, shops, planned);

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
        std::vector<Trip::Cand> tcands;
        tcands.reserve(cands.size());
        for (const auto& c : cands) {
            Trip::Cand t;
            t.key = c.id; t.actorId = c.actorId; t.faction = c.fac->GetFormID(); t.chest = c.chest;
            {
                auto* vendor = c.handle.get().get();
                const char* vn = vendor ? vendor->GetName() : c.fac->GetName();
                t.name = vn && *vn ? vn : "?";
            }
            t.dist = c.dist; t.pos = c.pos; t.havePos = c.havePos;
            t.kind = c.kind; t.destForm = c.destForm; t.destRad = c.destRad;
            for (int k = 0; k < 12; ++k) {
                const RE::FormID kw = (k <= 2) ? kKwPotion : (k == 3 || k == 4 || k == 11) ? kKwArrow : (k == 10) ? kKwWeapon : 0u;
                if (kw && ListTrades(c.fac, kw)) t.needCarry |= 1u << k;
            }
            t.carriesGear = (wantGear    && (ListTrades(c.fac, kKwWeapon) || ListTrades(c.fac, kKwArmor))) ||
                            (wantApparel && (ListTrades(c.fac, kKwClothing) || ListTrades(c.fac, kKwJewelry) || ListTrades(c.fac, kKwArmor)));
            t.carriesTome = ListTrades(c.fac, kKwSpellTome);
            t.carriesMaterials = ListTrades(c.fac, kKwOreIngot) || ListTrades(c.fac, kKwAnimalHide) || ListTrades(c.fac, kKwAnimalPart);   // R8
            t.carriesForge = ListTrades(c.fac, kKwOreIngot) || ListTrades(c.fac, kKwWeapon);                                              // R8: a smithy
            tcands.push_back(std::move(t));
        }
        Trip::Inputs tin;
        tin.follower = fid; tin.from = a_follower->GetPosition(); tin.carry = carry; tin.minSell = minSell; tin.minPurse = minPurse;
        std::vector<Stop> stops;
        std::string tripLine;
        if (!Trip::Plan(tcands, planned, tin, stops, tripLine)) {
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

        // The ch.19 leg budget (Harbinger's 8 package slots, shared with loot and the deposit): beyond it he queues.
        if (CountWalking() >= static_cast<std::size_t>(kMaxWalking)) {
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kQueueRetry;
            spdlog::info("[errand] {:08X} '{}': QUEUED -- {} errand walks already hold Harbinger's ch.19 legs (shared with "
                         "loot), next try in {} s", fid, NameOf(a_follower), kMaxWalking, kQueueRetry.count());
            return true;
        }
        if (!APMFBridge::ClaimErrandTravel(fid, stops.front().destForm, stops.front().destRad)) {
            // Refused at the call: the leg budget (all 8 slots busy), [Travel] bTravel=0, VR. Retry soon, never a fallback.
            std::scoped_lock lk(g_mx);
            g_nextPlan[fid] = a_now + kQueueRetry;
            return true;
        }
        Rec r;
        r.town        = playerTown->GetFormID();
        r.followerName = NameOf(a_follower);
        r.stops = std::move(stops);
        r.tripPurse = r.stops.front().order ? r.stops.front().order->purse : -1;
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
            if (have) ownDist = std::format("{:.0f} u", a_follower->GetPosition().GetDistance(dp));
        }
        spdlog::info("[errand] {:08X} '{}' TRIP PLAN in '{}': {} | walking to {} '{}' ({} from HIM): {} {:08X} | {} candidate(s)",
                     fid, r.followerName, TownName(r.town), tripLine, Trip::Label(r.stops, 0), r.vendorName, ownDist,
                     r.destKind == DestKind::VendorActor ? "the vendor" : r.destKind == DestKind::Ref ? "the shop ref" : "the shop cell",
                     r.destForm, cands.size());
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
        if (a_healWants && a_healWants()) { Finish(fid, "yields to a heal", 15.0f); return false; }
        if (Scheduler::ServiceClock() - r.startClk > kRealCapSec) { Finish(fid, "timed out (240 s real time)", 60.0f); return false; }
        if (auto* cal = RE::Calendar::GetSingleton()) {
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
            if (!NextStopOrFinish(fid, r, a_now, a_why, a_ok, a_cooldown, a_complete)) return false;
            Store(fid, r);
            return true;
        };
        // r.vendor == 0: still walking to the shop's PLACE (a cell / a ref); the vendor actor is found on arrival.
        RE::Actor* vendor = r.vendor ? RE::TESForm::LookupByID<RE::Actor>(r.vendor) : nullptr;
        // R5: a LATER stop's vendor may be unloaded while he walks there (he is in the previous shop's interior with the player): the
        // ch.19 leg decides that walk (an unreachable one ends DEST-GONE / STUCK and the stop is skipped). Phase 1 / the first stop: unchanged.
        const bool walkingToLaterStop = r.cur > 0 && r.phase == Phase::Walking && vendor && !vendor->IsDead() && !vendor->IsDisabled();
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
            if (!vendor) {   // walking to the shop's PLACE (a PLVD cell / ref): the leg ends Arrived in the shop, then to the vendor
                APMFBridge::LootLegState pleg;
                if (APMFBridge::ReadErrandLeg(fid, pleg) && pleg.ours) {
                    if (pleg.state == APMF_API::kLeg_Arrived) {
                        auto va = FindVendorActor(a_follower, r.faction);
                        if (!va) return stopOver("the shop is unattended (no vendor actor in the loaded shop)", false, kFailCooldownSec);
                        r.vendor = va->GetFormID();
                        r.vendorName = NameOf(va.get());
                        r.destKind = DestKind::VendorActor; r.destForm = r.vendor; r.destRad = kVendorRadius;
                        APMFBridge::RepointErrandTravel(fid, r.vendor, kVendorRadius);
                        spdlog::info("[errand] {:08X}: reached the shop; walking on to its vendor '{}' ({:08X})", fid, r.vendorName, r.vendor);
                    } else if (pleg.state == APMF_API::kLeg_Blocked) {
                        { std::scoped_lock lk(g_mx); g_parked[r.faction] = a_now + kParkTime; }
                        return stopOver("the walk to the shop ended BLOCKED (a closed door?); shop parked", false, kFailCooldownSec);
                    } else if (pleg.state == APMF_API::kLeg_Failed) {
                        Finish(fid, "Harbinger could not start the leg (FAILED -- all travel slots busy? APMF.log says); queued", 6.0f);
                        return false;
                    } else if (pleg.state == APMF_API::kLeg_ActorGone) {
                        // He went through a shop door / left the loaded area: the HAND-OFF to the simulation (Sweep), not a failure.
                    } else if (pleg.state > APMF_API::kLeg_Blocked && pleg.state <= APMF_API::kLeg_Released) {
                        WarnCombatDisagree(a_follower, pleg.state);
                        const auto bo = NoteWalkFailed(fid, r, a_now);
                        return stopOver(std::format("the walk to the shop ended: {} (this shop backed off {} s)", LegName(pleg.state), bo), false,
                                        kFailCooldownSec);
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
                        {
                            std::scoped_lock lk(g_mx);
                            g_parked[r.vendor] = a_now + kParkTime;
                        }
                        return stopOver(std::format("the walk ended BLOCKED at {:.0f} u from the vendor (blocker kind {}); vendor parked for {} min",
                                                    dist, leg.blockerKind, std::chrono::duration_cast<std::chrono::minutes>(kParkTime).count()),
                                        false, kFailCooldownSec);
                    }
                } else if (leg.state == APMF_API::kLeg_Failed) {
                    // The leg could not start: the usual cause is all 8 ch.19 package slots busy (loot + deposit + errands).
                    // He QUEUES (a short retry), never parks the vendor and never falls back to the instant trade.
                    Finish(fid, "Harbinger could not start the leg (FAILED -- all travel slots busy? APMF.log says); queued", 6.0f);
                    return false;
                } else if (leg.state == APMF_API::kLeg_ActorGone) {
                    // (the hand-off to the simulation is the Sweep's, not a failure)
                } else if (leg.state > APMF_API::kLeg_Blocked && leg.state <= APMF_API::kLeg_Released) {
                    WarnCombatDisagree(a_follower, leg.state);
                    const auto bo = NoteWalkFailed(fid, r, a_now);
                    return stopOver(std::format("the walk ended: {} (this vendor backed off {} s)", LegName(leg.state), bo), false, kFailCooldownSec);
                }
            }
            // Distance backstop: a leg read that is not ours yet, or a vendor who walked up to him (a loaded vendor only: R5's later stop).
            if (!arrived && vendorLoaded && dist <= kArriveDist && dz <= kSameFloorDz) arrived = true;
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
                spdlog::info("[errand] {:08X}: a late report from an earlier stop spent {}g -- counted against the trip purse", fid, late.spent);
            }
            TradeBridge::DiscardOutcome(fid);
            ErrandProbe probe;
            probe.mode = ErrandProbe::Mode::kExecute;
            probe.vendors.push_back(vendor->GetHandle());
            // R5: this stop's SHARE of the trip (its rows, needs, wishes) and what the trip purse has left; a restored stop (no order)
            // re-plans in full on his live inventory and gold.
            const int purseCap = r.tripPurse >= 0 ? std::max(0, r.tripPurse - r.tripSpent) : -1;
            EconomyErrandExecute(a_follower, a_state, a_now, probe, r.order.get(), purseCap);
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
                r.dispatched = false;
                r.bought = out.boughtCount > 0;
                if (r.bought && idles.take) {
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
        }
        return true;
    }

    // ── public: the owner-independent backstop ───────────────────────────────────────────────────────
    void Sweep() {
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
                    r.unloadedSince = {};
                    r.lastPos = f->GetPosition();   // sampled every lap: the simulation starts from the last LOADED position
                    if (auto* fc = f->GetParentCell()) r.lastCell = fc->GetFormID();
                    if (APMFBridge::InCombat(f) || (pc && pc->IsInCombat())) why = "combat -- he rejoins";   // substrate: not combat (v26)
                    else if (ids && std::find(ids->begin(), ids->end(), fid) == ids->end()) why = "no longer an active follower";
                    else if (clk - r.lastTickClk > kStallSec) why = "the follower is no longer serviced";
                    else if (clk - r.startClk > kRealCapSec) why = "timed out (240 s real time)";
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
            APMFBridge::ReleaseErrand(fid);   // Harbinger dropped the claims with the unload; this forgets our record of them
            SimStart st;
            st.follower = fid; st.followerName = r.followerName; st.vendorName = r.vendorName;
            st.faction = r.faction; st.chest = r.chest; st.town = r.town; st.vendorActor = r.vendor;
            st.destKind = r.destKind; st.destForm = r.destForm; st.destRad = r.destRad;
            st.order = r.order; st.lastPos = r.lastPos; st.lastCell = r.lastCell; st.startDays = r.startDays;
            SyncCur(r);   // R5: the itinerary goes off screen with him
            st.stops = r.stops; st.cur = r.cur; st.tripPurse = r.tripPurse; st.tripSpent = r.tripSpent;
            st.arrived = r.phase != Phase::Walking;
            st.tradeDispatched = r.dispatched;
            st.tradeDone = r.phase == Phase::Taking;
            Sim::Begin(std::move(st));
        }
    }
}
