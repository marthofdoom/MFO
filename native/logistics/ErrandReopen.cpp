// logistics/ErrandReopen.cpp -- F3 (marth 2026-10-20, town run 2): the SHOP-OPENS re-plan. ONE TRIP AT A TIME: a follower whose trip ended (or who
// is staying at the inn) for lack of an OPEN shop does not wait for the player to leave town when that shop opens.
//
//   * RECORD (Errand.cpp TryStart, at the commit of a trip): BuildCatalog drops a vendor faction that is closed for HOURS only (OpenAt false --
//     never a fence / spouse / conditioned drop). TryStart evaluates those shops with the same EconomyErrandPlan as the open ones (the orders are
//     built NOW, while the follower is loaded; MFO never reads an unloaded follower's inventory) and files them here with the trip's inputs and its
//     inn stop: the WATCH.
//   * POLL (Errand.cpp Tick for the on-screen stay / TryStart for a standing trip-done mark; ErrandSim.cpp Sweep for REST_SIM): at most once per
//     GAME HOUR the watched factions' hours are read again. A faction that is open now goes through Trip::Plan over its RECORDED candidate and
//     order -- the loaded and the off-screen follower decide from the same recorded needs, and the order is replayed as recorded (the on-screen
//     execute re-reads live counts for the stop's share, as for any trip). A stop = a new trip (the opened shops, the inn LAST); no stop = those
//     factions leave the watch. Either way the opened factions are consumed.
//
// Why the RECORDED plan on screen too (not a fresh BuildCatalog): a follower staying in the inn is loaded in the INN's cell, where BuildCatalog
// sees the innkeeper and no load door to any other shop; a fresh catalog from there would find nothing and the watch would be dropped silently.
//
// MISSED SELLS (marth 2026-10-21, ClickUp 1dfcuabb1ww), two more kinds of watched work on the SAME hourly poll (no new poller):
//   * GOLD (a): a reported trade whose offered sell rows the vendor's chest gold could not pay (Dragon Bone x2 at a vendor with 1 gold). NoteUnsold files
//     the shop + the unsold rows; Poll re-checks the chest's gold through TradeBridge::LastSeenChestGold (the planner's own safe read; a native chest read
//     is forbidden, ENGINE_NOTES 0.37, so a player's own sale to that vendor is invisible here) and, when it covers a row or the vendor's restock interval
//     (iDaysToRespawnVendor) has passed, plans the shop again through the same path as a reopened one.
//   * LATE (b): a sell candidate that appeared mid-trip and is in NO stop's order (a gem hold released after the plan). NoteLate files it; at the inn stage /
//     the trip-done mark the caller evaluates the recorded OPEN shops again (TakeLate / PlanLate) for a shop whose VEND buys it. On-screen only: the
//     evaluation reads his inventory, which MFO never does for an unloaded follower.
//
// Session-only: nothing here is saved (like the trip-done mark). THREADING: the AddTask job worker. Own mutex (a leaf; Poll calls
// Trip::ChestCrowd, which takes Errand g_mx then Sim g_mx, with NO lock of this file held).
#include "PCH.h"
#include "Errand_internal.h"
#include "Config.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Reopen {

    namespace {
        struct Watch {
            RE::FormID                 town = 0;
            std::vector<Trip::Cand>    cands;    // the hours-closed shops
            std::vector<ErrandPlanned> orders;   // their orders as planned (one per cand by key; may be skipped)
            Trip::Inputs               in;
            std::optional<Stop>        inn;      // the committed trip's inn stop (the new trip's LAST stop)
            long long                  lastHour = -1;   // the game-hour index of the last read
            // MISSED SELLS (a): one entry per shop whose chest gold ran out on a sell. cand/order = what Trip::Plan needs (the order holds ONLY the unsold rows).
            struct Gold {
                Trip::Cand    cand;
                ErrandPlanned order;
                RE::FormID    chest = 0;
                std::int32_t  blockedGold = 0;   // the chest's gold after the blocked trade
                std::int32_t  minUnit = 0;       // the cheapest unsold row's unit price: the gold that unblocks one row
                float         day = 0.0f;        // the game day of that trade
            };
            std::vector<Gold>          gold;
            // MISSED SELLS (b): the plan's OPEN shops (re-evaluated for a late item) and the late items.
            std::vector<Trip::Cand>    openShops;
            std::vector<RE::FormID>    late;
            std::vector<RE::FormID>    lateSeen;   // already evaluated once this commit: not queued again (junk nobody buys must not re-run the evaluation at every stop)
            int                        lateTries = 0;
            std::chrono::steady_clock::time_point lateNext{};
        };
        std::mutex                                g_mx;
        std::unordered_map<RE::FormID, Watch>     g_watch;   // follower -> watch; guarded by g_mx

        long long HourIndex() {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? static_cast<long long>(std::floor(static_cast<double>(cal->GetDaysPassed()) * 24.0)) : 0;
        }
        // The shops with these plan keys leave the watch (hours-closed and gold alike). The Watch itself stays (its open shops / late items live on).
        void EraseKeys(Watch& a_w, const std::vector<RE::FormID>& a_keys) {
            const auto has = [&](RE::FormID a_k) { return std::find(a_keys.begin(), a_keys.end(), a_k) != a_keys.end(); };
            auto& cs = a_w.cands;
            std::erase_if(cs, [&](const Trip::Cand& c) { return has(c.key); });
            std::erase_if(a_w.orders, [&](const ErrandPlanned& p) {
                return std::none_of(cs.begin(), cs.end(), [&](const Trip::Cand& c) { return c.key == p.vendorKey; });
            });
            std::erase_if(a_w.gold, [&](const Watch::Gold& g) { return has(g.cand.key); });
        }
        // The new trip: the stops capped to leave room for the inn, the inn LAST (state Pending, no order), the first stop Active.
        void Assemble(std::vector<Stop>&& a_shops, const std::optional<Stop>& a_inn, Plan& a_out) {
            const std::size_t room = kMaxTripStops - (a_inn ? 1u : 0u);
            if (a_shops.size() > room) a_shops.resize(room);
            a_out.stops = std::move(a_shops);
            if (a_inn) {
                Stop inn = *a_inn;
                inn.state = StopState::Pending;
                inn.order = nullptr;   // the food was bought on the first trip's inn stop
                a_out.stops.push_back(std::move(inn));
            }
            for (auto& s : a_out.stops) s.state = StopState::Pending;
            a_out.stops.front().state = StopState::Active;
        }
        std::string RowNames(const std::vector<TradeBridge::SellRow>& a_rows) {
            std::string out;
            for (std::size_t i = 0; i < a_rows.size(); ++i) {
                if (i >= 4) { out += std::format(", +{} more", a_rows.size() - 4); break; }
                auto* n = a_rows[i].obj ? a_rows[i].obj->As<RE::TESFullName>() : nullptr;
                out += std::format("{}'{}' x{} ({}g)", out.empty() ? "" : ", ", n && n->GetFullName() && *n->GetFullName() ? n->GetFullName() : "?", a_rows[i].count, a_rows[i].value);
            }
            return out;
        }
    }

    void Record(RE::FormID a_follower, RE::FormID a_town, std::vector<Trip::Cand>&& a_closedCands, std::vector<ErrandPlanned>&& a_closedOrders,
                const Trip::Inputs& a_in, const Stop* a_innStop, std::vector<Trip::Cand>&& a_openCands, const std::vector<RE::FormID>& a_tripFactions) {
        std::string names;
        std::size_t count = 0;
        std::size_t goldKept = 0;
        {
            std::scoped_lock lk(g_mx);
            count = a_closedCands.size();
            Watch w;
            w.town = a_town;
            w.cands = std::move(a_closedCands);
            w.orders = std::move(a_closedOrders);
            w.in = a_in;
            if (a_innStop) w.inn = *a_innStop;
            w.openShops = std::move(a_openCands);
            w.lastHour = HourIndex();   // the first read is at the NEXT game hour
            // MISSED SELLS (a): a gold-blocked shop outlives the new commit unless the new trip visits that shop (its own trade covers the rows).
            if (const auto old = g_watch.find(a_follower); old != g_watch.end() && old->second.town == a_town) {
                for (auto& g : old->second.gold) {
                    if (std::find(a_tripFactions.begin(), a_tripFactions.end(), g.cand.faction) != a_tripFactions.end()) continue;
                    w.gold.push_back(std::move(g));
                    ++goldKept;
                }
            }
            for (const auto& c : w.cands) names += (names.empty() ? "'" : ", '") + c.name + "'";
            g_watch[a_follower] = std::move(w);
        }
        if (count > 0)
            spdlog::info("[errand] {:08X}: shop watch recorded -- {} shop(s) closed for hours at the plan ({}); re-read once per game hour while his trip is done or he stays at the inn",
                         a_follower, count, names);
        if (goldKept > 0)
            spdlog::info("[errand] {:08X}: gold watch kept across the new plan -- {} shop(s) still owe him an unsold row", a_follower, goldKept);
    }

    void NoteUnsold(RE::FormID a_follower, RE::FormID a_town, const Stop& a_stop, const ErrandPlanned* a_order,
                    const TradeBridge::TradeOutcome& a_out, const Stop* a_innStop) {
        if (a_out.aborted || a_out.unsold.empty() || a_stop.kind != StopKind::Shop || !a_town) return;
        Watch::Gold g;
        g.cand.key = a_order ? a_order->vendorKey : (a_stop.vendorActor ? a_stop.vendorActor : a_stop.faction);
        g.cand.actorId = a_stop.vendorActor; g.cand.faction = a_stop.faction; g.cand.chest = a_stop.chest;
        g.cand.name = a_stop.vendorName;
        g.cand.pos = a_stop.pos; g.cand.havePos = a_stop.havePos;
        g.cand.kind = a_stop.destKind; g.cand.destForm = a_stop.destForm; g.cand.destRad = a_stop.destRad;
        if (a_order) g.order = *a_order;   // (the plan-time quote and ids; the rows below replace the order's)
        g.order.vendorKey = g.cand.key; g.order.faction = a_stop.faction; g.order.chest = a_stop.chest;
        g.order.skipped = false;
        g.order.sell = a_out.unsold;
        g.order.sellRows = static_cast<int>(a_out.unsold.size());
        g.order.sellValue = 0;
        g.minUnit = INT32_MAX;
        for (const auto& r : a_out.unsold) {
            g.order.sellValue += static_cast<std::int64_t>(r.value) * std::max<std::int32_t>(r.count, 0);
            g.minUnit = std::min(g.minUnit, r.value);
        }
        g.order.needKinds = 0; g.order.needs.clear();   // sell-only: the needs / wishes were this trip's business
        g.order.wishGear = g.order.wishApparel = g.order.wishTome = g.order.wishMaterials = false;
        g.order.buy = TradeBridge::BuyThresholds{};
        g.order.purse = 0;
        g.chest = a_stop.chest; g.blockedGold = std::max(0, a_out.chestGoldNow); g.day = a_out.day;
        const std::string rows = RowNames(a_out.unsold);
        const std::int64_t value = g.order.sellValue;
        const int need = g.minUnit;
        {
            std::scoped_lock lk(g_mx);
            auto it = g_watch.find(a_follower);
            if (it != g_watch.end() && it->second.town != a_town) {
                spdlog::info("[errand] {:08X}: MISSED SELL at '{}' not recorded -- the watch belongs to another town", a_follower, g.cand.name);
                return;
            }
            if (it == g_watch.end()) {   // no commit-time record (a trip restored from the save): a gold-only watch
                Watch w;
                w.town = a_town;
                w.in.follower = a_follower; w.in.from = a_stop.pos;
                w.in.minSell = Config::g_errandMinSellGold.load(); w.in.minPurse = Config::g_errandMinPurse.load();
                if (a_innStop) w.inn = *a_innStop;
                w.lastHour = HourIndex();
                it = g_watch.emplace(a_follower, std::move(w)).first;
            }
            std::erase_if(it->second.gold, [&](const Watch::Gold& o) { return o.cand.key == g.cand.key; });
            it->second.gold.push_back(std::move(g));
        }
        spdlog::info("[errand] {:08X}: MISSED SELL recorded -- '{}' could not pay for {} offered row(s) worth {}g [{}] (chest gold now {}g, the cheapest row needs {}g); "
                     "the shop is watched: re-checked once per game hour, re-planned when its last-seen gold covers a row or its restock ({:.0f} game days) has passed",
                     a_follower, a_stop.vendorName, a_out.unsold.size(), value, rows, a_out.chestGoldNow, need, TradeBridge::VendorRestockDays());
    }

    void NoteLate(RE::FormID a_follower, RE::FormID a_town, const std::vector<RE::FormID>& a_forms, bool a_requeue) {
        std::vector<RE::FormID> fresh;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end() || it->second.town != a_town) return;
            for (const auto f : a_forms) {
                if (!a_requeue && std::find(it->second.lateSeen.begin(), it->second.lateSeen.end(), f) != it->second.lateSeen.end()) continue;
                if (std::find(it->second.late.begin(), it->second.late.end(), f) == it->second.late.end()) { it->second.late.push_back(f); fresh.push_back(f); }
            }
            if (a_requeue) ++it->second.lateTries;
        }
        if (fresh.empty()) return;
        std::string names;
        for (const auto f : fresh) {
            auto* o = RE::TESForm::LookupByID<RE::TESBoundObject>(f);
            names += std::format("{}'{}'", names.empty() ? "" : ", ", o && o->GetName() && *o->GetName() ? o->GetName() : "?");
        }
        spdlog::info("[errand] {:08X}: LATE SELL candidate(s) {}: {} sellable now but in NO stop's order of this trip -- re-evaluated against the open shops at the inn stage / after the trip",
                     a_follower, a_requeue ? "re-queued (the new trip could not start)" : "recorded", names);
    }

    bool TakeLate(RE::FormID a_follower, RE::FormID a_town, std::chrono::steady_clock::time_point a_now, LateJob& a_out) {
        std::scoped_lock lk(g_mx);
        const auto it = g_watch.find(a_follower);
        if (it == g_watch.end() || it->second.town != a_town || it->second.late.empty() || a_now < it->second.lateNext) return false;
        auto& w = it->second;
        if (w.openShops.empty() || w.lateTries > 8) {
            spdlog::warn("[errand] {:08X}: {} late sell candidate(s) DROPPED -- {}", a_follower, w.late.size(),
                         w.openShops.empty() ? "the plan recorded no open shop to sell them to" : "the re-plan kept deferring (price quote pending)");
            w.late.clear(); w.lateTries = 0;
            return false;
        }
        a_out.shops = w.openShops;
        a_out.forms = std::move(w.late);
        for (const auto f : a_out.forms) if (std::find(w.lateSeen.begin(), w.lateSeen.end(), f) == w.lateSeen.end()) w.lateSeen.push_back(f);
        a_out.in = w.in;
        a_out.inn = w.inn;
        w.late.clear();
        w.lateNext = a_now + std::chrono::seconds(5);
        return true;
    }

    bool PlanLate(const LateJob& a_job, const std::vector<ErrandPlanned>& a_evaluated, Plan& a_out) {
        std::vector<Stop> shops;
        std::string line;
        if (!Trip::Plan(a_job.shops, a_evaluated, a_job.in, shops, line) || shops.empty()) return false;
        Assemble(std::move(shops), a_job.inn, a_out);
        a_out.lateForms = a_job.forms;
        a_out.line = std::format("LATE-SELL | {}", line);
        return true;
    }

    bool Poll(RE::FormID a_follower, RE::FormID a_town, const RE::NiPoint3* a_from, Plan& a_out) {
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal) return false;
        Watch w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end() || it->second.town != a_town) return false;
            const long long h = HourIndex();
            if (h == it->second.lastHour) return false;   // at most once per game hour
            it->second.lastHour = h;
            w = it->second;
        }
        const float hour = cal->GetHour();
        std::vector<Trip::Cand>    open;
        std::vector<ErrandPlanned> openOrders;
        std::string                names;
        for (const auto& c : w.cands) {
            auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(c.faction);
            if (!fac || !detail::OpenAtHour(fac, hour)) continue;
            open.push_back(c);
            names += (names.empty() ? "'" : ", '") + c.name + "'";
            for (const auto& o : w.orders) if (o.vendorKey == c.key) openOrders.push_back(o);
        }
        const bool hoursOpened = !open.empty();
        // MISSED SELLS (a): a gold-blocked shop that is open now and whose chest can pay a row again (the last-seen gold, or a vendor restock since).
        const float days = cal->GetDaysPassed();
        for (const auto& g : w.gold) {
            auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(g.cand.faction);
            if (!fac || !detail::OpenAtHour(fac, hour)) continue;
            const std::int32_t seen = TradeBridge::LastSeenChestGold(g.chest);
            const bool goldUp = seen >= g.minUnit && seen > g.blockedGold;
            const bool restocked = days >= g.day + TradeBridge::VendorRestockDays();
            if (!goldUp && !restocked) continue;
            ErrandPlanned o = g.order;
            o.chestGold = goldUp ? seen : -1;   // a restock: the gold is unknown until the trade reads it (unknown never caps the plan)
            open.push_back(g.cand);
            openOrders.push_back(std::move(o));
            names += (names.empty() ? "'" : ", '") + g.cand.name + "'";
            spdlog::info("[errand] {:08X}: MISSED SELL ready -- '{}' ({}): {} -- planning a trip for the {} unsold row(s) ({}g)",
                         a_follower, g.cand.name, g.cand.key,
                         goldUp ? std::format("its chest gold is now {}g (was {}g after the blocked trade), a row needs {}g", seen, g.blockedGold, g.minUnit)
                                : std::format("{:.1f} game days since the blocked trade, past the vendor restock ({:.0f})", days - g.day, TradeBridge::VendorRestockDays()),
                         g.order.sellRows, g.order.sellValue);
        }
        if (open.empty()) return false;
        spdlog::info("[errand] {:08X}: shop(s) OPEN now (hour {:.1f}): {} -- {}, re-planning over the recorded needs", a_follower, hour, names,
                     hoursOpened ? "closed -> open" : "gold watch");

        Trip::Inputs in = w.in;
        in.follower = a_follower;
        if (a_from) in.from = *a_from;
        std::vector<Stop> shops;
        std::string line;
        const bool ok = Trip::Plan(open, openOrders, in, shops, line);
        std::vector<RE::FormID> keys;
        for (const auto& c : open) keys.push_back(c.key);
        if (!ok || shops.empty()) {
            {
                std::scoped_lock lk(g_mx);
                if (const auto it = g_watch.find(a_follower); it != g_watch.end()) EraseKeys(it->second, keys);   // no stop: nothing to do there, those shops leave the watch
            }
            spdlog::info("[errand] {:08X}: the opened shop(s) {} yield NO stop over the recorded needs (nothing to sell / buy there) -- dropped from the watch, he stays", a_follower, names);
            return false;
        }
        a_out.keys = keys;
        Assemble(std::move(shops), w.inn, a_out);
        a_out.line = std::format("{} {} | {}", hoursOpened ? "REOPEN" : "GOLD", names, line);
        return true;
    }

    void Consume(RE::FormID a_follower, const Plan& a_plan) {
        std::scoped_lock lk(g_mx);
        const auto it = g_watch.find(a_follower);
        if (it == g_watch.end()) return;
        EraseKeys(it->second, a_plan.keys);
    }

    void Retry(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        if (const auto it = g_watch.find(a_follower); it != g_watch.end()) it->second.lastHour = -1;
    }

    void ClearAll() {
        std::scoped_lock lk(g_mx);
        g_watch.clear();
    }
}
