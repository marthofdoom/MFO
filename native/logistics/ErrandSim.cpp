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
// R2 (the 'ERND' co-save record, Serialization.cpp): every errand is saved as identities + stage + flags + game-day stamps, NEVER
// the order. A load STAGES the entries (StagePending); the first lap with the player in the world ADOPTS them here (AdoptPending):
// gone / dead / dismissed / errands off -> dropped (logged); otherwise a SimRec flagged `restored`. A restored follower who is
// LOADED at the shop picks up on screen (OnLoadedTick -> ResumePhysical; the physical errand re-plans the trade from his live
// inventory at the give beat); one who stays UNLOADED for kRestoreSettle DROPS the trade (marth 2026-10-19: a save before the
// off-screen trade ran drops it; the order was never saved and MFO never reads an unloaded inventory) and walks back on the game
// clock. A trade in flight at the save is never replayed: its Papyrus stack resumes from the .ess with a stale token
// (TradeBridge::ClearTransientState) and finishes or aborts on its own.
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
        // R2: a restored follower gets this long (real seconds, the world running) to show up LOADED before an unrun trade is
        // dropped. The roster refresh + the first service tick after a load are each ~1 s (kLogisticsInterval), so 3 s is
        // two polls of margin, and a follower who reloads after it still resumes his walk back (never re-sent to the counter).
        constexpr auto   kRestoreSettle    = std::chrono::seconds(3);
        // R5: the walk back from a stop whose place has no known position (a CELL shop restored from a save), in units.
        constexpr double kUnknownBackUnits = 2000.0;

        enum class Stage { Out, Trade, Return, Rejoin, Rest };   // R7: Rest = the INN stay off screen (no return leg until the player leaves town)
        const char* StageName(Stage s) {
            switch (s) {
                case Stage::Out:    return "OUT_SIM";
                case Stage::Trade:  return "TRADE_SIM";
                case Stage::Return: return "RET_SIM";
                case Stage::Rejoin: return "REJOIN";
                case Stage::Rest:   return "REST_SIM";
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
            // R2: restored from the 'ERND' record. No order (never saved); hadOrder = there was a non-empty one, so a resume on
            // screen may re-plan it. decideBy: the unloaded restore decision (the trade is dropped) is due at this instant.
            bool          restored = false, hadOrder = false;
            bool          restoredPhysical = false;   // restored from a PHYSICAL stage: loaded in the town, he resumes even away from the shop
            Clock::time_point decideBy{};
            // R5: the trip. The single-shop fields above (faction, chest, vendorActor, vendorName, dest*, order, tArrive, tTradeEnd) are
            // stops[cur]'s working copy (SyncCur / AdvanceStop); dispatched / reported / tradeAbandoned are the CURRENT stop's.
            std::vector<Stop> stops;
            std::uint8_t  cur = 0;
            int           tripPurse = -1, tripSpent = 0;
        };

        std::mutex                                 g_mx;
        std::unordered_map<RE::FormID, SimRec>     g_sims;      // guarded by g_mx
        std::vector<Saved>                         g_pending;   // R2: loaded from the save, not yet adopted; guarded by g_mx

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

        // R7: how long the work of a stop takes off screen, game minutes: a temple visit iTempleGameMin, anything else iErrandTradeGameMin.
        double WorkDays(const std::vector<Stop>& a_stops, std::size_t a_cur) {
            if (a_cur < a_stops.size() && a_stops[a_cur].kind == StopKind::Smith) return Smith::SimWorkDays();   // R8: iSmithWorkGameMin
            const bool temple = a_cur < a_stops.size() && a_stops[a_cur].kind == StopKind::Temple;
            return static_cast<double>(temple ? Config::g_templeGameMin.load() : Config::g_errandTradeGameMin.load()) / 1440.0;
        }
        StopKind CurKind(const SimRec& r) { return r.cur < r.stops.size() ? r.stops[r.cur].kind : StopKind::Shop; }

        void Store(const SimRec& a_r) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_sims.find(a_r.follower); it != g_sims.end()) it->second = a_r;
        }

        // R5: write the current-stop working copy back into stops[cur] (a record with no stops gets one: the v1 shape).
        void SyncCur(SimRec& r) {
            if (r.stops.empty()) { r.stops.push_back(Stop{}); r.cur = 0; r.stops[0].state = StopState::Active; }
            if (r.cur >= r.stops.size()) r.cur = static_cast<std::uint8_t>(r.stops.size() - 1);
            auto& s = r.stops[r.cur];
            s.faction = r.faction; s.chest = r.chest;
            if (r.vendorActor) s.vendorActor = r.vendorActor;
            s.vendorName = r.vendorName;
            s.destKind = r.destKind; s.destForm = r.destForm; s.destRad = r.destRad;
            s.order = r.order;
            s.tArrive = r.tArrive; s.tWorkEnd = r.tTradeEnd;
        }

        // R5: the current stop's business is settled off screen (traded, given up, or nothing to do). Move on to the next stop that
        // has an order (OUT_SIM again, tArrive = now + the walk between the two places; design 1.b) and return TRUE, or return FALSE
        // when none is left (the caller walks him back). A stop with no order (restored from a save: orders are never saved) is
        // skipped: MFO never rebuilds an order from an unloaded inventory.
        bool AdvanceStop(SimRec& r, double a_days) {
            SyncCur(r);
            r.stops[r.cur].state = (r.reported && !r.tradeAbandoned) ? StopState::Done : StopState::Skipped;
            int next = Trip::NextPending(r.stops, r.cur);
            // (R7: a TEMPLE or INN stop is never skipped for lacking an order -- a temple has none, a rest-only inn has none; a SHOP is.)
            while (next >= 0 && !r.stops[static_cast<std::size_t>(next)].order && r.stops[static_cast<std::size_t>(next)].kind == StopKind::Shop) {
                r.stops[static_cast<std::size_t>(next)].state = StopState::Skipped;
                spdlog::info("[errand] {:08X} '{}': TRIP {} SKIPPED (sim) -- its order is not in the save (never rebuilt off screen)",
                             r.follower, r.followerName, Trip::Label(r.stops, static_cast<std::size_t>(next)));
                next = Trip::NextPending(r.stops, static_cast<std::size_t>(next));
            }
            if (next < 0) return false;
            const Stop prev = r.stops[r.cur];
            const std::string was = Trip::Label(r.stops, r.cur);
            r.cur = static_cast<std::uint8_t>(next);
            auto& s = r.stops[r.cur];
            s.state = StopState::Active;
            r.faction = s.faction; r.chest = s.chest; r.vendorActor = s.vendorActor; r.vendorName = s.vendorName;
            r.destKind = s.destKind; r.destForm = s.destForm; r.destRad = s.destRad;
            r.order = s.order;
            r.dispatched = r.reported = r.tradeAbandoned = false;
            r.hadOrder = true;
            r.firstTryClk = -1.0;
            const double leg = WalkDays(Trip::LegUnits(prev, s));
            RE::NiPoint3 np;
            const double back = WalkDays(Trip::PlacePos(s, np) ? static_cast<double>(np.GetDistance(r.lastPos)) : kUnknownBackUnits);
            r.tArrive = a_days + leg;
            r.tTradeEnd = r.tArrive + WorkDays(r.stops, r.cur);
            r.tReturn = r.tTradeEnd + back;
            s.tArrive = r.tArrive; s.tWorkEnd = r.tTradeEnd;
            r.entryDoor = 0;   // the door he went in by belongs to the first shop: the return places him behind the player instead
            r.stage = Stage::Out;
            spdlog::info("[errand] {:08X} '{}': TRIP {} {} (sim) -- OUT_SIM to {}: walk {:.1f} game min, trade ends +{:.1f}, back +{:.1f} game min",
                         r.follower, r.followerName, was, prev.state == StopState::Done ? "DONE" : "SKIPPED", Trip::Label(r.stops, r.cur),
                         leg * 1440.0, (r.tTradeEnd - a_days) * 1440.0, (r.tReturn - a_days) * 1440.0);
            return true;
        }

        // R5: a restored trip whose current trade is DROPPED drops every later stop with it (none has an order).
        void SkipRemaining(SimRec& r) {
            for (std::size_t i = r.cur + 1; i < r.stops.size(); ++i)
                if (r.stops[i].state == StopState::Pending) r.stops[i].state = StopState::Skipped;
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
            // R5: the trip purse -- this stop spends at most what the earlier stops left of it (PlanBuy's reserve then applies to the trip).
            const int purse = r.tripPurse >= 0 ? std::min(r.order->purse, std::max(0, r.tripPurse - r.tripSpent)) : r.order->purse;
            const bool ok = TradeBridge::VendorTrade(f, vendor, chest, r.order->sell, r.order->needs, purse,
                                                     r.order->buy, r.order->sig, r.order->daysNow, r.order->vendorKey,
                                                     &r.order->quote);
            if (ok) {
                r.dispatched = true;
                r.dispatchedClk = Scheduler::ServiceClock();
                spdlog::info("[errand] {:08X} '{}': TRADE_SIM -- the stored order is REPLAYED against '{}' chest {:08X} (loaded=no): "
                             "{} sell row(s), {} need(s), purse {}{}", r.follower, r.followerName, r.vendorName, r.chest,
                             r.order->sell.size(), r.order->needs.size(), purse,
                             r.stops.size() > 1 ? std::format(" ({})", Trip::Label(r.stops, r.cur)) : std::string{});
            }
            return ok;
        }

        // "Resolve to now" (design 2.4): if he had ARRIVED and the trade was not dispatched, dispatch it (he got there); else drop it.
        void ResolveNow(SimRec& r, RE::Actor* f, double a_days, const char* a_why) {
            if (r.dispatched || r.tradeAbandoned || r.reported) return;
            if (!r.order && r.restored) {
                spdlog::info("[errand] {:08X}: resolving to now ({}) -- restored from the save, whose order is never saved: the trade is "
                             "dropped", r.follower, a_why);
                return;
            }
            if (a_days >= r.tArrive && r.order && !r.order->skipped) {
                spdlog::info("[errand] {:08X}: resolving to now ({}) -- he had arrived: the trade is replayed", r.follower, a_why);
                if (!DispatchTrade(r, f))
                    spdlog::warn("[errand] {:08X}: the trade could NOT be replayed while resolving ({}): the chest holds another order, the "
                                 "bridge is down or the order is gone -- the errand ends without it", r.follower, a_why);
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
    std::string Describe(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        auto it = g_sims.find(a_follower);
        if (it == g_sims.end()) return {};
        const SimRec& r = it->second;
        const std::string shop = (r.vendorName.empty() || r.vendorName == "?") ? std::string("the shop") : r.vendorName;
        switch (r.stage) {
            case Stage::Out:    return "Off-screen: walking to " + shop;
            case Stage::Trade:  return CurKind(r) == StopKind::Temple ? "Praying at " + shop : CurKind(r) == StopKind::Smith ? "Working at " + shop : "Off-screen: trading at " + shop;
            case Stage::Rest:   return "Resting at " + shop;   // the whole stay (marth 2026-10-19), physical and simulated alike
            case Stage::Return:
            case Stage::Rejoin: return "Heading back";
        }
        return {};
    }

    bool Has(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        if (g_sims.contains(a_follower)) return true;
        // R2: a staged restore entry owns him too (no new errand is planned before it is adopted or dropped).
        return std::any_of(g_pending.begin(), g_pending.end(), [&](const Saved& s) { return s.follower == a_follower; });
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mx);
        g_sims.clear();
        g_pending.clear();
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
        r.restored = a_start.restored;
        r.hadOrder = a_start.hadOrder;
        r.tradeAbandoned = a_start.tradeAbandoned;
        r.restoredPhysical = a_start.restoredPhysical;
        if (r.restored && !r.reported && !r.tradeAbandoned) r.decideBy = Clock::now() + kRestoreSettle;
        r.stops = std::move(a_start.stops); r.cur = a_start.cur;   // R5: the trip goes off screen at its CURRENT stop
        r.tripPurse = a_start.tripPurse; r.tripSpent = a_start.tripSpent;

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
        r.tTradeEnd = r.tArrive + WorkDays(r.stops, r.cur);
        r.tReturn = r.tTradeEnd + walk;
        r.stage = r.reported ? Stage::Return : (now >= r.tArrive ? Stage::Trade : Stage::Out);
        SyncCur(r);   // R5: the current stop carries the stamps just estimated
        if (r.reported && Trip::NextPending(r.stops, r.cur) >= 0) r.stage = Stage::Trade;   // traded here, more stops: the Trade stage moves on
        if (r.reported && CurKind(r) == StopKind::Inn) { r.stage = Stage::Rest; r.tReturn = r.tTradeEnd; }   // R7: an inn stay stays (no walk back)
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
        spdlog::info("[errand] {:08X} '{}': {} -> {} at '{}' (vendor actor {:08X}, chest {:08X}): walk estimate {:.1f} game min "
                     "({:.0f} u), arrive +{:.1f}, trade ends +{:.1f}, back +{:.1f} game min; entry door {:08X} ({}); order {}; "
                     "trade {}", r.follower, r.followerName, r.restored ? "RESTORED from the save (fresh walk estimate)" : "OFF SCREEN",
                     StageName(r.stage), r.vendorName, r.vendorActor, r.chest, toMin(walk), dist,
                     toMin(r.tArrive - now), toMin(r.tTradeEnd - now), toMin(r.tReturn - now), r.entryDoor,
                     r.entryDoor ? (confirmed ? "its destination is the cell he is in" : "nearest load door, destination unconfirmed") : "none found",
                     r.order && !r.order->skipped ? "stored" : (r.restored && r.hadOrder ? "not in the save (re-planned only on screen)" : "NONE"),
                     r.reported ? "done" : r.dispatched ? "already in flight"
                     : r.tradeAbandoned ? (r.restored ? "not MFO's to run (given up, or in flight at the save: the save's own Papyrus trade owns it)" : "given up")
                     : "not yet dispatched");
        if (r.stops.size() > 1)
            spdlog::info("[errand] {:08X} '{}': TRIP off screen at {} of {} stop(s); trip purse {}, spent {}", r.follower, r.followerName,
                         Trip::Label(r.stops, r.cur), r.stops.size(), r.tripPurse, r.tripSpent);
    }

    bool OnLoadedTick(RE::Actor* a_follower, const FollowerState& a_state) {
        (void)a_state;
        if (!a_follower) return false;
        // Not loaded = still the Sweep's (a stale tick of an unloaded follower must not resume or end anything).
        if (!a_follower->Is3DLoaded()) return true;
        const auto fid = a_follower->GetFormID();
        SimRec r;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_sims.find(fid);
            if (it == g_sims.end()) return false;
            r = it->second;
            g_sims.erase(it);
        }
        const double days = GameDays();
        // "If the player walks in then it picks up" -- but ONLY where he really is in the shop: the player is still in the errand's
        // town AND either the shop's vendor stands in the cell he loaded into, or that cell IS the shop's place. A follower who
        // reloads anywhere else (the player came back out, a teleport, fast travel) is found near the player: resolve to now, DONE
        // (design 2.4), never sent back across the town.
        auto* nowCell = a_follower->GetParentCell();
        const bool inTown = detail::PlayerTownId() == r.town;
        const bool atShop = inTown && (detail::VendorPresentNear(a_follower, r.faction) ||
                                       (r.destKind == DestKind::Cell && nowCell && nowCell->GetFormID() == r.destForm));
        // R2: a restored record has no order; it resumes on screen when it HAD one (the physical give beat re-plans the trade from
        // his live inventory), unless the unloaded restore decision already dropped the trade (tradeAbandoned).
        const bool tradeLeft = r.order ? !r.order->skipped : (r.restored && r.hadOrder);
        // R2 (design 2.7, "if the player walks in then it picks up"): a PHYSICAL errand restored from the save with him loaded
        // resumes wherever he is in the errand's town (the walk to the shop / vendor goes on). The away-from-the-shop end stays for
        // a record that was SIMULATED (R1b: he reloaded near the player) and for a player who is no longer in the town.
        // R5: a trip walking BETWEEN two stops (OUT_SIM to a later stop) is in the town's streets by construction: loaded again in the
        // town, he walks on to the current stop on screen (design 1.b: "a follower who loads mid-trip resumes the CURRENT stop").
        // R7: he is loaded AT the inn / temple of the current stop. The INN: he STAYS (the stay resumes on screen) -- entering the inn does
        // NOT make him rejoin (marth 2026-10-19) -- unless the food trade is still due, in which case the shop path below walks him to the
        // innkeeper first. The TEMPLE: the visit's work (the prayer) happens now, "applied when he loads".
        // R8: loaded again during SMITH_SIM, in the SAME session (a restored record has no plan): the trip goes on on screen -- he walks to the
        // smithy's place and Smith::Begin reads a fresh want list there. After a reload (r.restored) it falls through to DONE, as every stop does.
        if (inTown && !r.restored && r.cur < r.stops.size() && r.stops[r.cur].kind == StopKind::Smith &&
            (r.stage == Stage::Out || r.stage == Stage::Trade)) {
            ResumeInfo si;
            si.follower = fid; si.followerName = r.followerName; si.vendorName = r.vendorName;
            si.faction = r.faction; si.chest = r.chest; si.town = r.town; si.order = r.order; si.startDays = r.startDays;
            si.destKind = r.destKind; si.destForm = r.destForm; si.destRad = r.destRad;
            si.announced = false;
            SyncCur(r);
            si.stops = r.stops; si.cur = r.cur; si.tripPurse = r.tripPurse; si.tripSpent = r.tripSpent;
            if (detail::ResumePhysical(a_follower, si)) return true;
            detail::Cooldown(fid, kFailCooldownSec);
            spdlog::info("[errand] {:08X} '{}': loaded again during SMITH_SIM but the walk to the smithy could not be resumed -- he is with the player; DONE", fid, r.followerName);
            return false;
        }
        if (inTown && r.cur < r.stops.size() && r.stops[r.cur].kind != StopKind::Shop && r.stops[r.cur].kind != StopKind::Smith &&
            Rest::InStopCell(a_follower, r.stops[r.cur])) {
            const StopKind kind = r.stops[r.cur].kind;
            const bool pendingTrade = kind == StopKind::Inn && tradeLeft && !r.dispatched && !r.reported && !r.tradeAbandoned && r.stage != Stage::Rest;
            const bool atWork = kind == StopKind::Inn ? !pendingTrade : (r.stage == Stage::Out || r.stage == Stage::Trade);
            if (atWork) {
                ResumeInfo wi;
                wi.follower = fid; wi.followerName = r.followerName; wi.vendorName = r.vendorName;
                wi.faction = r.faction; wi.chest = r.chest; wi.town = r.town; wi.order = r.order; wi.startDays = r.startDays;
                wi.destKind = r.destKind; wi.destForm = r.destForm; wi.destRad = r.destRad;
                wi.announced = true; wi.work = true;
                SyncCur(r);
                wi.stops = r.stops; wi.cur = r.cur; wi.tripPurse = r.tripPurse; wi.tripSpent = r.tripSpent;
                if (detail::ResumePhysical(a_follower, wi)) return true;
                detail::Cooldown(fid, kFailCooldownSec);
                spdlog::info("[errand] {:08X} '{}': loaded again at the {} but its work could not be resumed -- he is with the player; DONE", fid, r.followerName,
                             kind == StopKind::Inn ? "inn" : "temple");
                return false;
            }
        }
        const bool betweenStops = inTown && r.cur > 0 && r.stage == Stage::Out;
        const bool resumeHere = atShop || (r.restoredPhysical && inTown) || betweenStops;
        if (resumeHere && !r.dispatched && !r.reported && !r.tradeAbandoned && tradeLeft &&
            (r.stage == Stage::Out || r.stage == Stage::Trade)) {
            ResumeInfo ri;
            ri.follower = fid; ri.followerName = r.followerName; ri.vendorName = r.vendorName;
            ri.faction = r.faction; ri.chest = r.chest; ri.town = r.town; ri.order = r.order; ri.startDays = r.startDays;
            ri.destKind = r.destKind; ri.destForm = r.destForm; ri.destRad = r.destRad;
            ri.announced = r.stage == Stage::Trade;   // the Trade stage was entered (toast fired) or restored as arrived: do not toast again
            SyncCur(r);
            ri.stops = r.stops; ri.cur = r.cur; ri.tripPurse = r.tripPurse; ri.tripSpent = r.tripSpent;   // R5
            if (detail::ResumePhysical(a_follower, ri)) return true;
            detail::Cooldown(fid, kFailCooldownSec);
            spdlog::info("[errand] {:08X} '{}': loaded again at {} {} but the walk could not be resumed -- the trade is dropped",
                         fid, r.followerName, StageName(r.stage), atShop ? "in the shop" : "in the town (restored physical errand)");
            return false;
        }
        ResolveNow(r, a_follower, days, atShop ? "loaded again in the shop" : "loaded again away from the shop");
        detail::Cooldown(fid, Config::g_errandCooldownSec.load());
        spdlog::info("[errand] {:08X} '{}': loaded again at {} ({}) -- he is with the player; DONE", fid, r.followerName,
                     StageName(r.stage), atShop ? "trade already done / in flight" : "away from the shop, resolved to now");
        return false;
    }

    // ── R2: the 'ERND' record ──────────────────────────────────────────────────────────────────────
    void SnapshotInto(std::vector<Saved>& a_out) {
        const double days = GameDays();
        std::scoped_lock lk(g_mx);
        for (const auto& [fid, r] : g_sims) {
            Saved s;
            s.follower = fid;
            switch (r.stage) {
                case Stage::Out:    s.stage = SavedStage::SimOut; break;
                case Stage::Trade:  s.stage = SavedStage::SimTrade; break;
                case Stage::Return: s.stage = SavedStage::SimReturn; break;
                case Stage::Rejoin: s.stage = SavedStage::SimRejoin; break;
                case Stage::Rest:   s.stage = SavedStage::SimTrade; break;   // R7: saved as a settled Trade (no new stage string); the load re-enters REST_SIM
            }
            if (r.stage == Stage::Rest) s.flags |= Saved::kTradeDone;
            if (r.stage != Stage::Out || days >= r.tArrive) s.flags |= Saved::kArrived;
            if (r.dispatched)     s.flags |= Saved::kDispatched;
            if (r.reported)       s.flags |= Saved::kTradeDone;
            if (r.order ? !r.order->skipped : r.hadOrder) s.flags |= Saved::kHadOrder;
            if (r.tradeAbandoned) s.flags |= Saved::kAbandoned;
            s.town = r.town; s.faction = r.faction; s.chest = r.chest; s.vendorActor = r.vendorActor;
            s.destKind = r.destKind == DestKind::VendorActor ? SavedDest::VendorActor
                       : r.destKind == DestKind::Ref         ? SavedDest::Ref : SavedDest::Cell;
            s.destForm = r.destForm; s.destRad = r.destRad;
            s.entryDoor = r.entryDoor; s.lastCell = r.lastCell; s.lastPos = r.lastPos;
            s.startDays = r.startDays;
            s.tEnter = r.tEnter; s.tArrive = r.tArrive; s.tTradeEnd = r.tTradeEnd; s.tReturn = r.tReturn;
            SimRec rc = r;   // R5: the itinerary, with the working copy written back into stops[cur]
            SyncCur(rc);
            for (const auto& st : rc.stops) s.stops.push_back(Trip::ToSaved(st));
            s.cur = rc.cur;
            a_out.push_back(std::move(s));
        }
        for (const auto& s : g_pending) a_out.push_back(s);   // staged, not yet adopted: saved back verbatim
    }

    void StagePending(std::vector<Saved>&& a_entries) {
        std::scoped_lock lk(g_mx);
        for (auto& s : a_entries) g_pending.push_back(s);
    }

    void ClearPending() {
        std::scoped_lock lk(g_mx);
        g_pending.clear();
    }

    void CrowdInto(std::unordered_map<RE::FormID, int>& a_crowd, RE::FormID a_exclude) {
        std::scoped_lock lk(g_mx);
        for (const auto& [fid, r] : g_sims) {
            if (fid == a_exclude || r.stage == Stage::Return || r.stage == Stage::Rejoin || r.stage == Stage::Rest) continue;
            if (r.stops.empty()) { if (r.chest) ++a_crowd[r.chest]; continue; }
            for (std::size_t i = r.cur; i < r.stops.size(); ++i) {
                const auto& st = r.stops[i];
                if ((i == r.cur || st.state == StopState::Pending) && st.chest) ++a_crowd[st.chest];
            }
        }
        for (const auto& p : g_pending)   // a staged restore entry is still heading there until it is adopted or dropped
            if (p.follower != a_exclude && p.chest) ++a_crowd[p.chest];
    }

    namespace {
        DestKind ToDest(SavedDest a_d) {
            return a_d == SavedDest::VendorActor ? DestKind::VendorActor : a_d == SavedDest::Ref ? DestKind::Ref : DestKind::Cell;
        }
        bool IsPhysical(SavedStage a_s) {
            return a_s == SavedStage::PhysWalking || a_s == SavedStage::PhysAtVendor || a_s == SavedStage::PhysTrading ||
                   a_s == SavedStage::PhysTaking;
        }

        // The first lap with the player in the world after a load (worker): turn each staged entry into a simulation record.
        void AdoptPending() {
            std::vector<Saved> pend;
            {
                std::scoped_lock lk(g_mx);
                if (g_pending.empty()) return;
            }
            auto* pc = RE::PlayerCharacter::GetSingleton();
            if (!pc || !pc->Is3DLoaded() || detail::InLoadingMenu()) return;   // the world is not there yet: keep them staged
            {
                std::scoped_lock lk(g_mx);
                pend.swap(g_pending);
            }
            const auto now = Clock::now();
            for (const auto& s : pend) {
                auto* f = RE::TESForm::LookupByID<RE::Actor>(s.follower);
                const char* why = nullptr;
                if (!f)                                                            why = "the follower is not in this game";
                else if (f->IsDead())                                              why = "the follower is dead";
                else if (!Config::g_logistics.load() || !Config::g_townErrands.load()) why = "logistics / town errands are off";
                else if (!Followers::IsEligibleFollower(f))                        why = "no longer a follower (dismissed)";
                else if (Errand::Active(s.follower))                              why = "he is already on an errand";
                if (why) {
                    spdlog::info("[errand] {:08X}: saved errand DROPPED on load -- {}", s.follower, why);
                    // Switched off with him still out (an unloaded follower may be standing in a shop): bring him back. A dismissed
                    // follower is not recalled (vanilla sends him home); RecallToPlayer itself skips a loaded one or one in the player's cell.
                    if (f && !f->IsDead() && (!Config::g_logistics.load() || !Config::g_townErrands.load()) &&
                        Followers::IsEligibleFollower(f) && !f->Is3DLoaded())
                        detail::RecallToPlayer(s.follower, "town errands are off; his saved errand was dropped");
                    continue;
                }
                std::string vendorName = "?";
                if (auto* va = s.vendorActor ? RE::TESForm::LookupByID<RE::Actor>(s.vendorActor) : nullptr) vendorName = detail::NameOf(va);
                else if (auto* fac = s.faction ? RE::TESForm::LookupByID<RE::TESFaction>(s.faction) : nullptr; fac && fac->GetName() && *fac->GetName())
                    vendorName = fac->GetName();
                const bool done = (s.flags & Saved::kTradeDone) != 0;
                // A trade IN FLIGHT at the save is the save's own: its Papyrus RunTrade stack resumes from the .ess and runs its
                // token-free sell / buy loops. MFO must not trade again (on screen or off), so it is given up here and he walks back.
                const bool inFlight = (s.flags & Saved::kDispatched) != 0 && !done;
                const bool abandoned = (s.flags & Saved::kAbandoned) != 0 || inFlight;
                const bool hadOrder = (s.flags & Saved::kHadOrder) != 0;
                // A physical errand (or a simulated one whose clock did not survive the reader's checks) gets a FRESH walk estimate
                // from Begin, which is also what a live hand-off does.
                if (IsPhysical(s.stage) || s.tReturn <= 0.0) {
                    SimStart st;
                    st.follower = s.follower; st.followerName = detail::NameOf(f); st.vendorName = vendorName;
                    st.faction = s.faction; st.chest = s.chest; st.town = s.town; st.vendorActor = s.vendorActor;
                    st.destKind = ToDest(s.destKind); st.destForm = s.destForm; st.destRad = s.destRad;
                    st.lastPos = s.lastPos; st.lastCell = s.lastCell; st.startDays = s.startDays;
                    st.arrived = (s.flags & Saved::kArrived) != 0 || s.stage == SavedStage::SimTrade ||
                                 s.stage == SavedStage::SimReturn || s.stage == SavedStage::SimRejoin;
                    st.tradeDispatched = false;   // a trade in flight at the save is never replayed (its Papyrus stack finishes it)
                    st.tradeDone = done;
                    st.restored = true; st.hadOrder = hadOrder; st.tradeAbandoned = abandoned;
                    st.restoredPhysical = IsPhysical(s.stage);
                    for (const auto& ss : s.stops) st.stops.push_back(Trip::FromSaved(ss));   // R5 (ERND v2; a v1 entry: one stop)
                    st.cur = s.cur;
                    Begin(std::move(st));
                    continue;
                }
                SimRec r;
                r.follower = s.follower; r.followerName = detail::NameOf(f); r.vendorName = vendorName;
                r.faction = s.faction; r.chest = s.chest; r.town = s.town; r.vendorActor = s.vendorActor;
                r.destKind = ToDest(s.destKind); r.destForm = s.destForm; r.destRad = s.destRad;
                r.lastPos = s.lastPos; r.lastCell = s.lastCell; r.entryDoor = s.entryDoor; r.startDays = s.startDays;
                r.tEnter = s.tEnter; r.tArrive = s.tArrive; r.tTradeEnd = s.tTradeEnd; r.tReturn = s.tReturn;
                r.stage = s.stage == SavedStage::SimOut ? Stage::Out : s.stage == SavedStage::SimTrade ? Stage::Trade
                        : s.stage == SavedStage::SimReturn ? Stage::Return : Stage::Rejoin;
                r.dispatched = false;   // never replayed (see the file comment)
                r.reported = done;
                r.tradeAbandoned = abandoned;
                r.restored = true;
                r.hadOrder = hadOrder;
                for (const auto& ss : s.stops) r.stops.push_back(Trip::FromSaved(ss));   // R5 (ERND v2; a v1 entry: one stop)
                r.cur = s.cur;
                SyncCur(r);   // the head fields (resolved by the reader) are the current stop's
                // R7 review SEV-2: a saved REST_SIM (SimTrade + kTradeDone on an INN stop) re-enters REST_SIM, not Trade -- the Trade stage would meet the cap
                // check before the Trade->Rest transition and recall him out of the inn. (No wire change: the same flags the writer already sets.)
                if (s.stage == SavedStage::SimTrade && done && CurKind(r) == StopKind::Inn) r.stage = Stage::Rest;
                if (!r.reported && !r.tradeAbandoned) r.decideBy = now + kRestoreSettle;
                r.nextPoll = now;
                r.nextPosLog = now + kPosLogEvery;
                r.rejoinAt = now;   // a REJOIN stage restarts its 20 s watch
                r.posAtEnter = f->GetPosition();
                if (auto* fc = f->GetParentCell()) r.cellAtEnter = fc->GetFormID();
                r.posKnown = true;
                {
                    std::scoped_lock lk(g_mx);
                    g_sims[r.follower] = r;
                }
                const double days = GameDays();
                spdlog::info("[errand] {:08X} '{}': RESTORED from the save at {} ('{}', chest {:08X}, town {:08X}); arrive {:+.1f}, trade ends "
                             "{:+.1f}, back {:+.1f} game min; entry door {:08X}; trade {}; loaded={}", r.follower, r.followerName,
                             StageName(r.stage), r.vendorName, r.chest, r.town, (r.tArrive - days) * 1440.0, (r.tTradeEnd - days) * 1440.0,
                             (r.tReturn - days) * 1440.0, r.entryDoor,
                             r.reported ? "done" : inFlight ? "in flight at the save (the save's own Papyrus trade owns it; MFO does not trade again)"
                             : r.tradeAbandoned ? "given up" : hadOrder ? "not run (re-planned only if he is loaded at the shop, else dropped)" : "none",
                             f->Is3DLoaded());
            }
        }
    }

    void Sweep() {
        AdoptPending();   // R2: entries restored from the save (a no-op once adopted)
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
            if (!Config::g_logistics.load() || !Config::g_townErrands.load()) {
                ResolveNow(r, f, GameDays(), "logistics / town errands switched off");
                Drop(fid, "logistics / town errands off", kFailCooldownSec, false);
                continue;
            }
            if (paused) continue;   // a load screen / no player: nothing moves, nothing decides (the game clock is the only clock)

            if (f->Is3DLoaded()) {   // loaded again: Errand::Tick -> OnLoadedTick resumes him when he is serviced; bounded here
                if (r.loadedSince.time_since_epoch().count() == 0) { r.loadedSince = now; Store(r); }
                else if (now - r.loadedSince > kLoadedUnserviced) Drop(fid, "loaded again but never serviced", kFailCooldownSec, false);
                continue;
            }
            if (r.loadedSince.time_since_epoch().count() != 0) r.loadedSince = {};

            // R2 RESTORE DECISION: a restored errand whose trade had not run, and whose follower has not shown up LOADED (at the shop,
            // with the player) within kRestoreSettle, DROPS the trade -- the order is not in the save and is never rebuilt from an
            // unloaded inventory (marth 2026-10-19). The game clock goes on: he walks back, no longer than the saved walk.
            if (r.restored && !r.reported && !r.tradeAbandoned && !r.dispatched) {
                if (now < r.decideBy) { Store(r); continue; }
                r.tradeAbandoned = true;
                SkipRemaining(r);   // R5: no later stop has an order either (orders are never saved)
                const double days = GameDays();
                if (r.stage == Stage::Out || r.stage == Stage::Trade) {
                    const double walk = std::max(0.0, r.tReturn - r.tTradeEnd);
                    r.tReturn = std::min(r.tReturn, days + walk);
                    r.stage = Stage::Return;
                }
                spdlog::info("[errand] {:08X} '{}': restored UNLOADED with the trade not run -- the trade is DROPPED (saved before it ran; "
                             "the order is never saved); {}, back in {:.1f} game min", fid, r.followerName, StageName(r.stage),
                             std::max(0.0, r.tReturn - days) * 1440.0);
            }

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
                if (Config::g_errandInn.load() && CurKind(r) == StopKind::Inn) Rest::MarkTripDone(fid, r.town, "REST_SIM ended by player combat");   // R7 review: one trip per visit, off screen too
                Drop(fid, "player combat -- he joins", kCombatCooldownSec, false);
                continue;
            }
            if (r.stage != Stage::Rest && (days - r.startDays) * 1440.0 > Trip::CapGameMin(r.stops.size())) {   // R5: once per planned stop (R7: an inn stay has no cap)
                ResolveNow(r, f, days, "fErrandMaxGameMin");
                detail::RecallToPlayer(fid, "the errand cap");
                Drop(fid, "older than the trip cap (fErrandMaxGameMin x stops) -- recalled", kFailCooldownSec, false);
                continue;
            }

            // ── the timeline ──
            if (r.stage == Stage::Out && days >= r.tArrive) {
                r.stage = Stage::Trade;
                spdlog::info("[errand] {:08X} '{}': TRADE_SIM -- he has reached '{}' (off screen)", fid, r.followerName, r.vendorName);
                if (CurKind(r) == StopKind::Temple) detail::ToastPraying(r.followerName, r.vendorName);   // R7
                else if (CurKind(r) == StopKind::Smith) detail::ToastSmithing(r.followerName, r.vendorName);   // R8: SMITH_SIM
                else if (!r.tradeAbandoned && r.order && !r.order->skipped) detail::ToastTrading(r.followerName, r.vendorName, r.faction);   // not for a trade that will never run
            }
            if (r.stage == Stage::Trade) {
                const double clk = Scheduler::ServiceClock();
                if (!r.dispatched && !r.tradeAbandoned && !r.reported) {   // (R5: a stop that already traded -- handed off at TAKING -- never re-trades)
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
                        r.tripSpent += std::max(0, out.spent);   // R5: the next stop's purse cap
                        if (!out.smithBought.empty()) Smith::NoteBought(fid, out.smithBought);   // R8: what this trade really bought
                    } else if (clk - r.dispatchedClk > kTradeWaitClkSec) {
                        r.reported = true;
                        spdlog::warn("[errand] {:08X}: the replayed trade never reported ({:.0f} s) -- continuing", fid, kTradeWaitClkSec);
                    }
                }
                const bool tradeSettled = r.reported || r.tradeAbandoned || !r.order || r.order->skipped;
                if (days >= r.tTradeEnd && tradeSettled && CurKind(r) == StopKind::Inn) {
                    // R7: the INN is the last stop and he STAYS: no return leg. Only the player leaving town (RECALL), combat (he walks out and
                    // joins), a dismissal or "told to wait" ends it -- the resolutions above -- and the player entering the inn picks him up on screen.
                    r.stage = Stage::Rest;
                    r.tReturn = r.tTradeEnd;
                    detail::ToastResting(r.followerName, r.vendorName);
                    spdlog::info("[errand] {:08X} '{}': REST_SIM -- he is resting at '{}' (off screen) until the player leaves town; no return leg", fid, r.followerName, r.vendorName);
                } else if (days >= r.tTradeEnd && tradeSettled) {
                    if (CurKind(r) == StopKind::Temple) Rest::NoteOwed(fid, r.followerName, r.vendorName);   // R7: TEMPLE_SIM -- the blessing is OWED
                    if (CurKind(r) == StopKind::Smith) {   // R8: SMITH_SIM -- the result is applied at the END of the stage (armor only; weapons wait)
                        Smith::SimFinish(fid, r.followerName);
                    }
                    // R5: the next stop of the trip (OUT_SIM again), else the walk back.
                    if (!AdvanceStop(r, days)) {
                        r.stage = Stage::Return;
                        spdlog::info("[errand] {:08X} '{}': RET_SIM -- done at '{}', walking back (off screen)", fid, r.followerName, r.vendorName);
                    }
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
                if (Config::g_errandInn.load()) Rest::MarkTripDone(fid, r.town, "RET_SIM returned");   // R7 review: an inn-less trip ends here; one trip per visit
                Drop(fid, "returned", Config::g_errandCooldownSec.load(), true);
                continue;
            }
            Store(r);
        }
    }
}
