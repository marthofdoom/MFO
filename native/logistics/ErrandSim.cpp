// logistics/ErrandSim.cpp -- TOWN ERRANDS, round R1b (ClickUp 86e3t1mx7): the OFF-SCREEN half. See logistics/Errand.h for the
// contract and logistics/Errand.cpp for the physical errand this one takes over from.
//
// marth's rulings (2026-10-19): a follower who is not loaded is SIMULATED against the REAL merchant inventory and REAL gold, with
// no movement and no animation outside the player's cell; if the player walks in, the errand PICKS UP from where the simulation
// is; when the player leaves the town (fast travel or otherwise) MFO brings the follower back; the player entering combat makes
// him walk out of the shop and join; every follower may shop at once; the off-screen time is the walk estimate plus
// iErrandTradeGameMin game minutes. House rule (logistics/Lotd.cpp): MFO NEVER reads an unloaded follower's inventory -- the
// trade ORDER was built at PLAN time while he was loaded (ErrandPlanned) and is only REPLAYED here through the unchanged
// TradeBridge::VendorTrade / MFO_Trade.RunTrade, which re-clamps every count against the live follower.
//
//     OUT_SIM --t>=tArrive--> TRADE_SIM (order replayed once) --t>=tTradeEnd--> RET_SIM --t>=tReturn--> REJOIN (Papyrus MoveTo to the
//     door he went in by, or behind the player) --20 s watch--> DONE
//     any stage --leave town / player combat / cap / dismissed / told to wait--> RESOLVE (replay the trade if he had arrived) + RECALL
//
// All clocks are GAME DAYS (Calendar::GetDaysPassed): waiting, sleeping and fast travel advance them, a menu does not.
//
// NOT HERE (R2): nothing is saved. A save made mid-errand loses the record (the follower is simply a follower again; the order is
// dropped, never replayed from an unloaded inventory). The seam is SimStart / ResumeInfo in Errand_internal.h.
//
// THREADING: the AddTask job worker (Errand::Sweep each pump lap, Errand::Tick for a loaded follower); ClearTransientState from
// the revert with the pump drained. g_mx guards the table and is never held across a Papyrus / TradeBridge / Harbinger call.
#include "PCH.h"
#include "Errand_internal.h"
#include "Config.h"
#include "Followers.h"
#include "LocationTypes.h"
#include "Papyrus.h"
#include "Scheduler.h"
#include "TradeBridge.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Sim {

    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr double kWalkSpeed        = 150.0;    // u per real second (a walking follower; the design's estimate)
        constexpr double kMinWalkGameMin   = 2.0;      // the walk is never shorter than this, game minutes
        constexpr double kInteriorWalk     = 500.0;    // u from the door to the counter when the shop is a cell (its size is unknown)
        constexpr float  kEntryDoorReach   = 400.0f;   // a load door within this of his last loaded position is the one he used
        constexpr float  kReturnOffset     = 120.0f;   // u from the door, toward where he went in
        constexpr auto   kPollEvery        = std::chrono::milliseconds(1000);
        constexpr auto   kPosLogEvery      = std::chrono::seconds(5);
        constexpr auto   kRejoinWatch      = std::chrono::seconds(20);
        constexpr auto   kLeftTownGrace    = std::chrono::seconds(2);
        constexpr auto   kLoadedUnserviced = std::chrono::seconds(15);
        constexpr double kDispatchTryClkSec = 20.0;    // a busy chest: retry this long (service-clock seconds), then give the trade up
        constexpr double kTradeWaitClkSec   = 45.0;    // dispatch -> ReportTrade
        constexpr float  kRejoinFarU       = 4096.0f;
        constexpr float  kFailCooldownSec  = 45.0f;
        constexpr float  kCombatCooldownSec = 30.0f;

        enum class Stage { Out, Trade, Return, Rejoin };
        const char* StageName(Stage s) {
            switch (s) {
                case Stage::Out:    return "OUT_SIM";
                case Stage::Trade:  return "TRADE_SIM";
                case Stage::Return: return "RET_SIM";
                case Stage::Rejoin: return "REJOIN";
            }
            return "?";
        }

        struct SimRec {
            RE::FormID    follower = 0;
            std::string   followerName, vendorName;
            RE::FormID    faction = 0, chest = 0, town = 0, vendorActor = 0;
            DestKind      destKind = DestKind::Cell;
            RE::FormID    destForm = 0;
            float         destRad = 0.0f;
            std::shared_ptr<const ErrandPlanned> order;
            RE::NiPoint3  lastPos{};
            RE::FormID    lastCell = 0;
            RE::FormID    entryDoor = 0;
            float         startDays = 0.0f;
            double        tEnter = 0.0, tArrive = 0.0, tTradeEnd = 0.0, tReturn = 0.0;   // GAME DAYS
            Stage         stage = Stage::Out;
            bool          dispatched = false, reported = false, tradeAbandoned = false;
            double        dispatchedClk = 0.0, firstTryClk = -1.0;
            Clock::time_point nextPoll{}, nextPosLog{}, rejoinAt{}, townLostSince{}, loadedSince{};
            // the passive drift line
            RE::NiPoint3  posAtEnter{};
            RE::FormID    cellAtEnter = 0;
            bool          posKnown = false;
        };

        std::mutex                                 g_mx;
        std::unordered_map<RE::FormID, SimRec>     g_sims;   // guarded by g_mx

        double GameDays() {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? static_cast<double>(cal->GetDaysPassed()) : 0.0;
        }
        double WalkDays(double a_units) {
            auto* cal = RE::Calendar::GetSingleton();
            const double ts = cal ? static_cast<double>(cal->GetTimescale()) : 20.0;   // game seconds per real second
            const double realSec = a_units / kWalkSpeed;
            return std::max(kMinWalkGameMin / 1440.0, realSec * ts / 86400.0);
        }

        void Store(const SimRec& a_r) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_sims.find(a_r.follower); it != g_sims.end()) it->second = a_r;
        }

        // The load door he used: the nearest ExtraTeleport door to his last LOADED position within kEntryDoorReach, preferring one
        // whose destination door sits in the cell he is in now. Collect-then-act on the (attached) cell he left.
        RE::FormID FindEntryDoor(RE::Actor* a_follower, RE::FormID a_cell, const RE::NiPoint3& a_pos, bool& a_confirmed) {
            a_confirmed = false;
            auto* cell = a_cell ? RE::TESForm::LookupByID<RE::TESObjectCELL>(a_cell) : nullptr;
            if (!cell || !cell->IsAttached()) return 0;
            std::vector<RE::ObjectRefHandle> doors;
            cell->ForEachReferenceInRange(a_pos, kEntryDoorReach, [&](RE::TESObjectREFR* a_ref) {
                if (doors.size() < 24 && a_ref->extraList.GetByType<RE::ExtraTeleport>())
                    doors.push_back(a_ref->GetHandle());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            auto* nowCell = a_follower ? a_follower->GetParentCell() : nullptr;
            RE::FormID best = 0, bestAny = 0;
            float bestD = 1.0e9f, bestAnyD = 1.0e9f;
            for (auto& h : doors) {
                auto dp = h.get();
                auto* d = dp.get();
                if (!d) continue;
                auto* tele = d->extraList.GetByType<RE::ExtraTeleport>();
                if (!tele || !tele->teleportData) continue;
                const float dist = d->GetPosition().GetDistance(a_pos);
                if (dist < bestAnyD) { bestAnyD = dist; bestAny = d->GetFormID(); }
                auto lp = tele->teleportData->linkedDoor.get();
                auto* linked = lp.get();
                if (nowCell && linked && linked->GetParentCell() == nowCell && dist < bestD) { bestD = dist; best = d->GetFormID(); }
            }
            if (best) { a_confirmed = true; return best; }
            return bestAny;
        }

        // Replay the stored order ONCE against the real chest. The follower is unloaded: nothing here reads his inventory.
        bool DispatchTrade(SimRec& r, RE::Actor* f) {
            if (!r.order || r.order->skipped || r.tradeAbandoned) return false;
            auto* chest = RE::TESForm::LookupByID<RE::TESObjectREFR>(r.chest);
            if (!chest) { r.tradeAbandoned = true; return false; }
            auto* vendor = r.vendorActor ? RE::TESForm::LookupByID<RE::Actor>(r.vendorActor) : nullptr;
            TradeBridge::DiscardOutcome(r.follower);
            const bool ok = TradeBridge::VendorTrade(f, vendor, chest, r.order->sell, r.order->needs, r.order->purse,
                                                     r.order->buy, r.order->sig, r.order->daysNow);
            if (ok) {
                r.dispatched = true;
                r.dispatchedClk = Scheduler::ServiceClock();
                spdlog::info("[errand] {:08X} '{}': TRADE_SIM -- the stored order is REPLAYED against '{}' chest {:08X} (loaded=no): "
                             "{} sell row(s), {} need(s), purse {}", r.follower, r.followerName, r.vendorName, r.chest,
                             r.order->sell.size(), r.order->needs.size(), r.order->purse);
            }
            return ok;
        }

        // "Resolve to now" (design 2.4): if he had ARRIVED and the trade was not dispatched, dispatch it (he got there); else drop it.
        void ResolveNow(SimRec& r, RE::Actor* f, double a_days, const char* a_why) {
            if (r.dispatched || r.tradeAbandoned) return;
            if (a_days >= r.tArrive && r.order && !r.order->skipped) {
                spdlog::info("[errand] {:08X}: resolving to now ({}) -- he had arrived: the trade is replayed", r.follower, a_why);
                DispatchTrade(r, f);
            } else {
                spdlog::info("[errand] {:08X}: resolving to now ({}) -- he had not arrived: the trade is dropped", r.follower, a_why);
            }
        }

        void Drop(RE::FormID a_fid, const char* a_why, float a_cooldownSec, bool a_complete) {
            SimRec r;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_sims.find(a_fid);
                if (it == g_sims.end()) return;
                r = it->second;
                g_sims.erase(it);
            }
            detail::Cooldown(a_fid, a_cooldownSec);
            spdlog::info("[errand] {:08X} '{}': {} (sim) at {} -- {} (vendor '{}')", a_fid, r.followerName,
                         a_complete ? "DONE" : "ENDED", StageName(r.stage), a_why, r.vendorName);
        }

        // Put him outside the door he went in by (toward where he stood before), or -- no door / the door ref is not in memory --
        // behind the player. A vanilla Papyrus MoveTo either way.
        void PlaceReturn(SimRec& r, RE::Actor* f, const char* a_why) {
            auto* door = r.entryDoor ? RE::TESForm::LookupByID<RE::TESObjectREFR>(r.entryDoor) : nullptr;
            if (door && f) {
                float dx = r.lastPos.x - door->GetPosition().x, dy = r.lastPos.y - door->GetPosition().y;
                const float len = std::sqrt(dx * dx + dy * dy);
                if (len > 1.0f) { dx = dx / len * kReturnOffset; dy = dy / len * kReturnOffset; } else { dx = dy = 0.0f; }
                const bool ok = Papyrus::DispatchMoveTo(f, door, dx, dy, 0.0f);
                spdlog::info("[errand] {:08X} '{}': {} -- MoveTo the door he went in by {:08X} (+{:.0f},{:.0f}) {}", r.follower,
                             r.followerName, a_why, r.entryDoor, dx, dy, ok ? "dispatched" : "FAILED to dispatch");
                return;
            }
            detail::RecallToPlayer(r.follower, r.entryDoor ? "the entry door is not in memory" : "no entry door was found");
        }
    }

    // ── public ──────────────────────────────────────────────────────────────────────────────────────
    bool Has(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        return g_sims.contains(a_follower);
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_sims.clear();
    }

    void Begin(SimStart&& a_start) {
        auto* f = RE::TESForm::LookupByID<RE::Actor>(a_start.follower);
        if (!f) return;
        SimRec r;
        r.follower = a_start.follower; r.followerName = a_start.followerName; r.vendorName = a_start.vendorName;
        r.faction = a_start.faction; r.chest = a_start.chest; r.town = a_start.town; r.vendorActor = a_start.vendorActor;
        r.destKind = a_start.destKind; r.destForm = a_start.destForm; r.destRad = a_start.destRad;
        r.order = std::move(a_start.order);
        r.lastPos = a_start.lastPos; r.lastCell = a_start.lastCell; r.startDays = a_start.startDays;
        r.dispatched = a_start.tradeDispatched;
        if (r.dispatched) r.dispatchedClk = Scheduler::ServiceClock();
        r.reported = a_start.tradeDone;

        bool confirmed = false;
        r.entryDoor = FindEntryDoor(f, r.lastCell, r.lastPos, confirmed);
        // The walk estimate: the straight line from his last loaded position to the shop's place. A cell has no position of its own,
        // so it is the door he used plus a typical interior walk.
        double dist = 0.0;
        RE::NiPoint3 destPos = r.lastPos;
        bool havePos = false;
        if (r.destKind == DestKind::VendorActor) {
            if (auto* v = RE::TESForm::LookupByID<RE::Actor>(r.destForm)) { destPos = v->GetPosition(); havePos = true; }
        } else if (r.destKind == DestKind::Ref) {
            if (auto* d = RE::TESForm::LookupByID<RE::TESObjectREFR>(r.destForm)) { destPos = d->GetPosition(); havePos = true; }
        }
        if (havePos) {
            dist = r.lastPos.GetDistance(destPos);
        } else {
            auto* door = r.entryDoor ? RE::TESForm::LookupByID<RE::TESObjectREFR>(r.entryDoor) : nullptr;
            dist = (door ? r.lastPos.GetDistance(door->GetPosition()) : 0.0) + kInteriorWalk;
        }
        const double walk = WalkDays(dist);
        const double now = GameDays();
        r.tEnter = now;
        r.tArrive = a_start.arrived ? now : now + walk;
        r.tTradeEnd = r.tArrive + static_cast<double>(Config::g_errandTradeGameMin.load()) / 1440.0;
        r.tReturn = r.tTradeEnd + walk;
        r.stage = r.reported ? Stage::Return : (now >= r.tArrive ? Stage::Trade : Stage::Out);
        r.nextPoll = Clock::now() + kPollEvery;
        r.nextPosLog = Clock::now() + kPosLogEvery;
        r.posAtEnter = f->GetPosition();
        if (auto* fc = f->GetParentCell()) r.cellAtEnter = fc->GetFormID();
        r.posKnown = true;
        {
            std::scoped_lock lk(g_mx);
            g_sims[r.follower] = r;
        }
        auto toMin = [](double d) { return d * 1440.0; };
        spdlog::info("[errand] {:08X} '{}': OFF SCREEN -> {} at '{}' (vendor actor {:08X}, chest {:08X}): walk estimate {:.1f} game min "
                     "({:.0f} u), arrive +{:.1f}, trade ends +{:.1f}, back +{:.1f} game min; entry door {:08X} ({}); order {}; "
                     "trade {}", r.follower, r.followerName, StageName(r.stage), r.vendorName, r.vendorActor, r.chest, toMin(walk), dist,
                     toMin(r.tArrive - now), toMin(r.tTradeEnd - now), toMin(r.tReturn - now), r.entryDoor,
                     r.entryDoor ? (confirmed ? "its destination is the cell he is in" : "nearest load door, destination unconfirmed") : "none found",
                     r.order && !r.order->skipped ? "stored" : "NONE", r.dispatched ? "already in flight" : "not yet dispatched");
    }

    bool OnLoadedTick(RE::Actor* a_follower, const FollowerState& a_state) {
        (void)a_state;
        if (!a_follower) return false;
        const auto fid = a_follower->GetFormID();
        SimRec r;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_sims.find(fid);
            if (it == g_sims.end()) return false;
            r = it->second;
            g_sims.erase(it);
        }
        // "If the player walks in then it picks up": a trade that has NOT been replayed is done on screen from where the simulation is.
        if (!r.dispatched && !r.reported && !r.tradeAbandoned && r.order && !r.order->skipped &&
            (r.stage == Stage::Out || r.stage == Stage::Trade)) {
            ResumeInfo ri;
            ri.follower = fid; ri.followerName = r.followerName; ri.vendorName = r.vendorName;
            ri.faction = r.faction; ri.chest = r.chest; ri.town = r.town; ri.order = r.order; ri.startDays = r.startDays;
            ri.destKind = r.destKind; ri.destForm = r.destForm; ri.destRad = r.destRad;
            if (detail::ResumePhysical(a_follower, ri)) return true;
            detail::Cooldown(fid, kFailCooldownSec);
            spdlog::info("[errand] {:08X} '{}': loaded again at {} but the shop is unattended / unreachable -- the trade is dropped",
                         fid, r.followerName, StageName(r.stage));
            return false;
        }
        detail::Cooldown(fid, Config::g_errandCooldownSec.load());
        spdlog::info("[errand] {:08X} '{}': loaded again at {} (trade {}) -- he is with the player; DONE", fid, r.followerName,
                     StageName(r.stage), r.dispatched || r.reported ? "done / in flight" : "not replayed");
        return false;
    }

    void Sweep() {
        std::vector<RE::FormID> ids;
        {
            std::scoped_lock lk(g_mx);
            if (g_sims.empty()) return;
            ids.reserve(g_sims.size());
            for (const auto& [id, r] : g_sims) ids.push_back(id);
        }
        auto* pc = RE::PlayerCharacter::GetSingleton();
        const auto now = Clock::now();
        const bool paused = !pc || !pc->Is3DLoaded() || detail::InLoadingMenu();
        for (const auto fid : ids) {
            SimRec r;
            {
                std::scoped_lock lk(g_mx);
                const auto it = g_sims.find(fid);
                if (it == g_sims.end()) continue;
                r = it->second;
            }
            auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!f || f->IsDead()) { Drop(fid, "follower dead", kFailCooldownSec, false); continue; }
            if (!Config::g_logistics.load() || !Config::g_townErrands.load()) { Drop(fid, "logistics / town errands off", kFailCooldownSec, false); continue; }
            if (paused) continue;   // a load screen / no player: nothing moves, nothing decides (the game clock is the only clock)

            if (f->Is3DLoaded()) {   // loaded again: Errand::Tick -> OnLoadedTick resumes him when he is serviced; bounded here
                if (r.loadedSince.time_since_epoch().count() == 0) { r.loadedSince = now; Store(r); }
                else if (now - r.loadedSince > kLoadedUnserviced) Drop(fid, "loaded again but never serviced", kFailCooldownSec, false);
                continue;
            }
            if (r.loadedSince.time_since_epoch().count() != 0) r.loadedSince = {};

            // THE PASSIVE DRIFT LINE (design 4.4, principle 5): where is a simulated follower REALLY? Every 5 s, never a decision.
            if (now >= r.nextPosLog) {
                r.nextPosLog = now + kPosLogEvery;
                const auto p = f->GetPosition();
                auto* fc = f->GetParentCell();
                const float moved = r.posKnown ? p.GetDistance(r.posAtEnter) : 0.0f;
                spdlog::info("[errand-pos] {:08X} '{}': {} cell {:08X} (at hand-off {:08X}) pos ({:.0f},{:.0f},{:.0f}) moved {:.0f} u since going off screen",
                             fid, r.followerName, StageName(r.stage), fc ? fc->GetFormID() : 0u, r.cellAtEnter, p.x, p.y, p.z, moved);
            }
            if (now < r.nextPoll) { Store(r); continue; }
            r.nextPoll = now + kPollEvery;

            const double days = GameDays();
            // ── resolutions: each replays the trade if he had arrived, then drops the record ──
            if (!Followers::IsEligibleFollower(f)) {   // dismissed: vanilla sends him home; no recall
                ResolveNow(r, f, days, "dismissed");
                Drop(fid, "no longer a follower (dismissed)", kFailCooldownSec, false);
                continue;
            }
            if (auto* avo = f->AsActorValueOwner(); avo && avo->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.0f) {
                ResolveNow(r, f, days, "told to wait");
                Drop(fid, "told to wait -- he stays where he is", 60.0f, false);
                continue;
            }
            const RE::FormID here = detail::PlayerTownId();
            if (here != r.town) {
                if (r.townLostSince.time_since_epoch().count() == 0) { r.townLostSince = now; Store(r); continue; }
                if (now - r.townLostSince >= kLeftTownGrace) {   // PLAYER LEFT THE TOWN: finish his business, bring him back
                    ResolveNow(r, f, days, "the player left the town");
                    detail::RecallToPlayer(fid, "the player left the town");
                    Drop(fid, "the player left the town -- recalled", kFailCooldownSec, false);
                    continue;
                }
            } else {
                r.townLostSince = {};
            }
            if (pc->IsInCombat()) {   // he walks out of the shop and joins the fight
                ResolveNow(r, f, days, "player combat");
                PlaceReturn(r, f, "the player is in combat -- he walks out and joins");
                Drop(fid, "player combat -- he joins", kCombatCooldownSec, false);
                continue;
            }
            if ((days - r.startDays) * 1440.0 > Config::g_errandMaxGameMin.load()) {
                ResolveNow(r, f, days, "fErrandMaxGameMin");
                PlaceReturn(r, f, "the errand cap");
                Drop(fid, "older than fErrandMaxGameMin", kFailCooldownSec, false);
                continue;
            }

            // ── the timeline ──
            if (r.stage == Stage::Out && days >= r.tArrive) {
                r.stage = Stage::Trade;
                spdlog::info("[errand] {:08X} '{}': TRADE_SIM -- he has reached '{}' (off screen)", fid, r.followerName, r.vendorName);
            }
            if (r.stage == Stage::Trade) {
                const double clk = Scheduler::ServiceClock();
                if (!r.dispatched && !r.tradeAbandoned) {
                    if (r.firstTryClk < 0.0) r.firstTryClk = clk;
                    if (!r.order || r.order->skipped) {
                        r.tradeAbandoned = true;
                    } else if (!DispatchTrade(r, f) && !r.tradeAbandoned && clk - r.firstTryClk > kDispatchTryClkSec) {
                        r.tradeAbandoned = true;
                        spdlog::warn("[errand] {:08X}: the replayed trade could not be dispatched for {:.0f} s (chest busy or the bridge is down) -- dropped",
                                     fid, kDispatchTryClkSec);
                    }
                }
                if (r.dispatched && !r.reported) {
                    TradeBridge::TradeOutcome out;
                    if (!TradeBridge::HasLiveOrder(fid) && TradeBridge::TakeOutcome(fid, out)) {
                        r.reported = true;
                        if (out.aborted)
                            spdlog::warn("[errand] {:08X}: the replayed trade ABORTED (Papyrus found no usable chest)", fid);
                        else
                            spdlog::info("[errand] {:08X} '{}': trade REPORTED (loaded=no) -- sold {} for {}g, bought {} for {}g", fid,
                                         r.followerName, out.soldCount, out.soldValue, out.boughtCount, out.spent);
                    } else if (clk - r.dispatchedClk > kTradeWaitClkSec) {
                        r.reported = true;
                        spdlog::warn("[errand] {:08X}: the replayed trade never reported ({:.0f} s) -- continuing", fid, kTradeWaitClkSec);
                    }
                }
                const bool tradeSettled = r.reported || r.tradeAbandoned || !r.order || r.order->skipped;
                if (days >= r.tTradeEnd && tradeSettled) {
                    r.stage = Stage::Return;
                    spdlog::info("[errand] {:08X} '{}': RET_SIM -- done at '{}', walking back (off screen)", fid, r.followerName, r.vendorName);
                }
            }
            if (r.stage == Stage::Return && days >= r.tReturn) {
                PlaceReturn(r, f, "RET_SIM complete -- back at the door");
                r.stage = Stage::Rejoin;
                r.rejoinAt = now;
            } else if (r.stage == Stage::Rejoin && now - r.rejoinAt >= kRejoinWatch) {
                // 20 s after the placement: if he is STILL not loaded and far from the player, bring him the rest of the way.
                const float d = f->GetPosition().GetDistance(pc->GetPosition());
                if (d > kRejoinFarU) detail::RecallToPlayer(fid, "not back with the player 20 s after RET_SIM");
                Drop(fid, "returned", Config::g_errandCooldownSec.load(), true);
                continue;
            }
            Store(r);
        }
    }
}
