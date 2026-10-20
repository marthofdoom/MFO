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
// Session-only: nothing here is saved (like the trip-done mark). THREADING: the AddTask job worker. Own mutex (a leaf; Poll calls
// Trip::ChestCrowd, which takes Errand g_mx then Sim g_mx, with NO lock of this file held).
#include "PCH.h"
#include "Errand_internal.h"

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
        };
        std::mutex                                g_mx;
        std::unordered_map<RE::FormID, Watch>     g_watch;   // follower -> watch; guarded by g_mx

        long long HourIndex() {
            auto* cal = RE::Calendar::GetSingleton();
            return cal ? static_cast<long long>(std::floor(static_cast<double>(cal->GetDaysPassed()) * 24.0)) : 0;
        }
    }

    void Record(RE::FormID a_follower, RE::FormID a_town, std::vector<Trip::Cand>&& a_closedCands, std::vector<ErrandPlanned>&& a_closedOrders,
                const Trip::Inputs& a_in, const Stop* a_innStop) {
        std::string names;
        std::size_t count = 0;
        {
            std::scoped_lock lk(g_mx);
            if (a_closedCands.empty()) { g_watch.erase(a_follower); return; }
            count = a_closedCands.size();
            Watch w;
            w.town = a_town;
            w.cands = std::move(a_closedCands);
            w.orders = std::move(a_closedOrders);
            w.in = a_in;
            if (a_innStop) w.inn = *a_innStop;
            w.lastHour = HourIndex();   // the first read is at the NEXT game hour
            for (const auto& c : w.cands) names += (names.empty() ? "'" : ", '") + c.name + "'";
            g_watch[a_follower] = std::move(w);
        }
        spdlog::info("[errand] {:08X}: shop watch recorded -- {} shop(s) closed for hours at the plan ({}); re-read once per game hour while his trip is done or he stays at the inn",
                     a_follower, count, names);
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
        if (open.empty()) return false;
        spdlog::info("[errand] {:08X}: shop(s) OPEN now (hour {:.1f}): {} -- closed -> open, re-planning over the recorded needs", a_follower, hour, names);

        Trip::Inputs in = w.in;
        in.follower = a_follower;
        if (a_from) in.from = *a_from;
        std::vector<Stop> shops;
        std::string line;
        const bool ok = Trip::Plan(open, openOrders, in, shops, line);
        auto consume = [&](const std::vector<RE::FormID>& a_keys) {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end()) return;
            auto& cs = it->second.cands;
            std::erase_if(cs, [&](const Trip::Cand& c) { return std::find(a_keys.begin(), a_keys.end(), c.key) != a_keys.end(); });
            std::erase_if(it->second.orders, [&](const ErrandPlanned& p) {
                return std::none_of(cs.begin(), cs.end(), [&](const Trip::Cand& c) { return c.key == p.vendorKey; });
            });
            if (cs.empty()) g_watch.erase(it);
        };
        std::vector<RE::FormID> keys;
        for (const auto& c : open) keys.push_back(c.key);
        if (!ok || shops.empty()) {
            consume(keys);   // no stop: nothing to do there, those factions leave the watch
            spdlog::info("[errand] {:08X}: the opened shop(s) {} yield NO stop over the recorded needs (nothing to sell / buy there) -- dropped from the watch, he stays", a_follower, names);
            return false;
        }
        a_out.keys = keys;
        const std::size_t room = kMaxTripStops - (w.inn ? 1u : 0u);
        if (shops.size() > room) shops.resize(room);
        a_out.stops = std::move(shops);
        if (w.inn) {
            Stop inn = *w.inn;
            inn.state = StopState::Pending;
            inn.order = nullptr;   // the food was bought on the first trip's inn stop
            a_out.stops.push_back(std::move(inn));
        }
        for (auto& s : a_out.stops) s.state = StopState::Pending;
        a_out.stops.front().state = StopState::Active;
        a_out.line = std::format("REOPEN {} | {}", names, line);
        return true;
    }

    void Consume(RE::FormID a_follower, const Plan& a_plan) {
        std::scoped_lock lk(g_mx);
        const auto it = g_watch.find(a_follower);
        if (it == g_watch.end()) return;
        auto& cs = it->second.cands;
        std::erase_if(cs, [&](const Trip::Cand& c) { return std::find(a_plan.keys.begin(), a_plan.keys.end(), c.key) != a_plan.keys.end(); });
        std::erase_if(it->second.orders, [&](const ErrandPlanned& p) {
            return std::none_of(cs.begin(), cs.end(), [&](const Trip::Cand& c) { return c.key == p.vendorKey; });
        });
        if (cs.empty()) g_watch.erase(it);
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
