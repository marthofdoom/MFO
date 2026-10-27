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
//   * MISSED BUY (2026-10-26): a reported trade whose supply quota the vendor's STOCK could not fill (Auri's magicka potions at Ri'saad: a VEND
//     keyword says what a vendor BUYS, not what it sells). NoteUnfilled logs "MISSED BUY <kind> <remaining>" and files the remainder + a template of the
//     stop's order; Poll plans it to the next OPEN candidate of the plan whose VEND carries the kind (stock-evidence ones first, the nearest), never the
//     same shop twice for a kind, at most kMaxBuyReplans times per commit, as an ordinary REOPEN-style trip (the inn last). No new poller.
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
            // MISSED BUY: the remainder still wanted per supply kind (quota = units), a template of the last missing stop's order (purse, quote, ids),
            // the (kind, shop key) pairs already tried, and the re-plans spent this commit.
            std::vector<TradeBridge::NeedCat>                 missed;
            ErrandPlanned                                     missedTmpl;
            std::vector<std::pair<std::int32_t, RE::FormID>>  tried;
            int                                               buyReplans = 0;
            // IN-TRIP RE-QUEUE / CARRIED NEED: kinds the hourly Poll gave up on (nobody else carries them) stay UNMET here, so the leave-town carry sees them.
            std::vector<TradeBridge::NeedCat>                 unmet;
            // MISSED SELLS (b): the plan's OPEN shops (re-evaluated for a late item) and the late items.
            std::vector<Trip::Cand>    openShops;
            std::vector<RE::FormID>    late;
            std::vector<RE::FormID>    lateSeen;   // already evaluated once this commit: not queued again (junk nobody buys must not re-run the evaluation at every stop)
            int                        lateTries = 0;
            std::chrono::steady_clock::time_point lateNext{};
        };
        constexpr int kMaxBuyReplans = 2;   // MISSED BUY re-plans per commit (a third shop for one miss is a second trip too many in one visit)
        const char* BuyKindName(std::int32_t a_k) {
            switch (a_k) {
                case TradeBridge::NeedCat::kPotHealth:  return "health potion";
                case TradeBridge::NeedCat::kPotStamina: return "stamina potion";
                case TradeBridge::NeedCat::kPotMagicka: return "magicka potion";
                case TradeBridge::NeedCat::kArrows:     return "arrows";
                case TradeBridge::NeedCat::kBolts:      return "bolts";
            }
            return "supply";
        }
        std::mutex                                g_mx;
        std::unordered_map<RE::FormID, Watch>     g_watch;   // follower -> watch; guarded by g_mx
        // CARRIED NEED (2026-10-26): the supply kinds a follower still lacked when the player left a town, with the last order template (for building
        // an order at a shop that evaluated as 'skipped'). SESSION-ONLY like the watch: nothing here is saved; guarded by g_mx.
        struct Carried { std::vector<TradeBridge::NeedCat> needs; ErrandPlanned tmpl; };
        std::unordered_map<RE::FormID, Carried>   g_carried;
        // STOCK-ONLY BUY NEEDS (2026-10-26): needs Trip::Plan found NO stockist for in the town (NoteUnserved). Moved to g_carried when the player leaves the town
        // (CarryAndClear). Session-only; guarded by g_mx.
        std::unordered_map<RE::FormID, Carried>   g_unserved;
        // LIVE NEED RE-CHECK: the per-VISIT (kind, shop key) pairs a follower already tried, kept apart from any watch (a restored trip has none). town = the visit.
        struct LiveTried { RE::FormID town = 0; std::vector<std::pair<std::int32_t, RE::FormID>> pairs; };
        std::unordered_map<RE::FormID, LiveTried> g_liveTried;
        void AddLiveTried(RE::FormID a_fid, RE::FormID a_town, std::int32_t a_kind, RE::FormID a_key) {   // caller holds g_mx
            auto& lt = g_liveTried[a_fid];
            if (lt.town != a_town) { lt.town = a_town; lt.pairs.clear(); }
            const std::pair<std::int32_t, RE::FormID> p{ a_kind, a_key };
            if (std::find(lt.pairs.begin(), lt.pairs.end(), p) == lt.pairs.end()) lt.pairs.push_back(p);
        }
        void MergeKind(std::vector<TradeBridge::NeedCat>& a_into, const TradeBridge::NeedCat& a_n) {
            for (auto& m : a_into) if (m.kind == a_n.kind) { m.quota = std::max(m.quota, a_n.quota); return; }
            a_into.push_back(a_n);
        }
        // A buy-only order for the candidate a_c from the template (the same reset Poll's MISSED BUY plan applies): the template's purse / quote / ids,
        // none of its sell rows or wishes, the given needs.
        ErrandPlanned BuyOrderFor(const ErrandPlanned& a_tmpl, const Trip::Cand& a_c, const std::vector<TradeBridge::NeedCat>& a_needs) {
            ErrandPlanned o = a_tmpl;
            o.vendorKey = a_c.key; o.faction = a_c.faction; o.chest = a_c.chest;
            o.skipped = false;
            o.sell.clear(); o.sellRows = 0; o.sellValue = 0;
            o.chestGold = -1;
            o.wishGear = o.wishApparel = o.wishTome = o.wishMaterials = false;
            o.buy = TradeBridge::BuyThresholds{};
            o.needs = a_needs;
            o.needKinds = 0;
            for (const auto& n : o.needs) o.needKinds |= 1u << n.kind;
            return o;
        }

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
                w.tried = old->second.tried;   // the per-VISIT (kind, shop) loop guard outlives the new commit
                w.unmet = old->second.unmet;
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

    void NoteUnfilled(RE::FormID a_follower, RE::FormID a_town, const Stop& a_stop, const ErrandPlanned* a_order,
                      const TradeBridge::TradeOutcome& a_out) {
        if (a_out.aborted || a_stop.kind != StopKind::Shop || !a_town || !a_order) return;
        const RE::FormID key = a_order->vendorKey ? a_order->vendorKey : (a_stop.vendorActor ? a_stop.vendorActor : a_stop.faction);
        std::string said;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end() || it->second.town != a_town) {
                for (const auto& n : a_out.unfilled) {
                    said += std::format("{}{} {}", said.empty() ? "" : ", ", BuyKindName(n.kind), n.quota);
                    if (n.kind >= 0 && n.kind <= TradeBridge::NeedCat::kBolts && n.quota > 0) {   // the live re-check never asks this shop again this visit (under every identity it answers to)
                        AddLiveTried(a_follower, a_town, n.kind, key);
                        if (a_stop.faction) AddLiveTried(a_follower, a_town, n.kind, a_stop.faction);
                        if (a_stop.vendorActor) AddLiveTried(a_follower, a_town, n.kind, a_stop.vendorActor);
                    }
                }
                if (said.empty()) return;
                spdlog::info("[errand] {:08X}: MISSED BUY at '{}': {} -- not re-planned by the watch (no recorded plan for this town visit: a trip restored from the save, or a LIVE NEED trip -- the live re-check handles it, never asking this shop again this visit)", a_follower, a_stop.vendorName, said);
                return;
            }
            auto& w = it->second;
            // A kind this order asked for that did not come back unfilled is settled (filled, or the purse's): it leaves the watch.
            for (const auto& n : a_order->needs) {
                if (n.kind < 0 || n.kind > TradeBridge::NeedCat::kBolts) continue;
                const bool open = std::any_of(a_out.unfilled.begin(), a_out.unfilled.end(), [&](const TradeBridge::NeedCat& u) { return u.kind == n.kind; });
                if (!open) std::erase_if(w.missed, [&](const TradeBridge::NeedCat& m) { return m.kind == n.kind; });
            }
            for (const auto& n : a_out.unfilled) {
                if (n.kind < 0 || n.kind > TradeBridge::NeedCat::kBolts || n.quota <= 0) continue;
                said += std::format("{}{} {}", said.empty() ? "" : ", ", BuyKindName(n.kind), n.quota);
                std::erase_if(w.missed, [&](const TradeBridge::NeedCat& m) { return m.kind == n.kind; });
                w.missed.push_back(n);
                if (std::find(w.tried.begin(), w.tried.end(), std::pair<std::int32_t, RE::FormID>{ n.kind, key }) == w.tried.end()) w.tried.emplace_back(n.kind, key);
            }
            if (said.empty()) return;
            w.missedTmpl = *a_order;
            w.lastHour = -1;   // the next Poll runs now (the hourly throttle is for closed shops)
        }
        spdlog::info("[errand] {:08X}: MISSED BUY at '{}': {} still wanted -- its stock could not fill the quota; the remainder is re-planned to another open shop whose VEND carries it (this visit, at most {} re-plan(s))",
                     a_follower, a_stop.vendorName, said, kMaxBuyReplans);
    }

    void DropGoldForms(RE::FormID a_follower, const std::vector<RE::FormID>& a_forms) {
        std::size_t rows = 0, entries = 0;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end()) return;
            for (auto& g : it->second.gold) {
                auto& sell = g.order.sell;
                const auto before = sell.size();
                std::erase_if(sell, [&](const TradeBridge::SellRow& r) {
                    return r.obj && std::find(a_forms.begin(), a_forms.end(), r.obj->GetFormID()) != a_forms.end();
                });
                rows += before - sell.size();
                g.order.sellRows = static_cast<int>(sell.size());
                g.order.sellValue = 0;
                g.minUnit = INT32_MAX;
                for (const auto& r : sell) {
                    g.order.sellValue += static_cast<std::int64_t>(r.value) * std::max<std::int32_t>(r.count, 0);
                    g.minUnit = std::min(g.minUnit, r.value);
                }
            }
            entries = std::erase_if(it->second.gold, [](const Watch::Gold& g) { return g.order.sell.empty(); });
        }
        if (rows > 0)
            spdlog::info("[errand] {:08X}: late-sell trip started -- {} unsold row(s) dropped from the gold watch ({} shop entr{} emptied)", a_follower, rows, entries, entries == 1 ? "y" : "ies");
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
        // MISSED BUY: the unfilled remainder goes to the nearest OPEN candidate of the plan that shows STOCK EVIDENCE for the kind (Trip::StockNow; a VEND
        // keyword alone is not a claim, marth 2026-10-26), never one already tried for it. A candidate already in `open` keeps the order it has.
        std::vector<std::pair<std::int32_t, RE::FormID>> buyServed;
        bool buyTried = false;
        if (!w.missed.empty() && w.buyReplans < kMaxBuyReplans && !w.openShops.empty()) {
            buyTried = true;
            std::unordered_map<RE::FormID, std::vector<TradeBridge::NeedCat>> byShop;   // candidate key -> the kinds it takes
            for (const auto& m : w.missed) {
                if (m.kind < 0 || m.kind > 31) continue;
                const Trip::Cand* best = nullptr;
                float bestD = 0.0f;
                for (const auto& c : w.openShops) {
                    if (!Trip::StockNow(c, m.kind)) continue;
                    if (std::find(w.tried.begin(), w.tried.end(), std::pair<std::int32_t, RE::FormID>{ m.kind, c.key }) != w.tried.end()) continue;
                    if (std::any_of(open.begin(), open.end(), [&](const Trip::Cand& o) { return o.key == c.key; })) continue;
                    auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(c.faction);
                    if (!fac || !detail::OpenAtHour(fac, hour)) continue;
                    const float d = c.havePos ? w.in.from.GetDistance(c.pos) : c.dist;
                    if (!best || d < bestD) { best = &c; bestD = d; }
                }
                if (best) byShop[best->key].push_back(m);
            }
            for (const auto& c : w.openShops) {
                const auto it = byShop.find(c.key);
                if (it == byShop.end()) continue;
                ErrandPlanned o = w.missedTmpl;
                o.vendorKey = c.key; o.faction = c.faction; o.chest = c.chest;
                o.skipped = false;
                o.sell.clear(); o.sellRows = 0; o.sellValue = 0;
                o.chestGold = -1;
                o.wishGear = o.wishApparel = o.wishTome = o.wishMaterials = false;
                o.buy = TradeBridge::BuyThresholds{};
                o.needs = it->second;
                o.needKinds = 0;
                for (const auto& n : o.needs) { o.needKinds |= 1u << n.kind; buyServed.emplace_back(n.kind, c.key); }
                open.push_back(c);
                openOrders.push_back(std::move(o));
                names += (names.empty() ? "'" : ", '") + c.name + "'";
                std::string kinds;
                for (const auto& n : it->second) kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", BuyKindName(n.kind), n.quota);
                spdlog::info("[errand] {:08X}: MISSED BUY ready -- '{}' ({}) is open and shows stock of it: planning a trip for {}", a_follower, c.name, c.key, kinds);
            }
        }
        if (open.empty()) {
            if (buyTried && buyServed.empty()) {   // nobody left to re-plan to: stop asking every hour
                std::string kinds;
                for (const auto& n : w.missed) kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", BuyKindName(n.kind), n.quota);
                {
                    std::scoped_lock lk(g_mx);
                    if (const auto it = g_watch.find(a_follower); it != g_watch.end()) {
                        for (const auto& n : it->second.missed) MergeKind(it->second.unmet, n);   // still lacking: the leave-town carry takes it
                        it->second.missed.clear();
                    }
                }
                spdlog::info("[errand] {:08X}: MISSED BUY {} dropped -- no other OPEN shop of this plan shows stock of it (or every one was tried); carried to the next town", a_follower, kinds);
            }
            return false;
        }
        spdlog::info("[errand] {:08X}: shop(s) OPEN now (hour {:.1f}): {} -- {}, re-planning over the recorded needs", a_follower, hour, names,
                     hoursOpened ? "closed -> open" : !buyServed.empty() && w.gold.empty() ? "missed buy" : "gold watch");

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
                if (const auto it = g_watch.find(a_follower); it != g_watch.end()) {
                    EraseKeys(it->second, keys);   // no stop: nothing to do there, those shops leave the watch
                    if (!buyServed.empty()) {   // MISSED BUY: the planner found nothing to buy there either (purse / floor): stop asking
                        for (const auto& n : it->second.missed) MergeKind(it->second.unmet, n);
                        it->second.missed.clear();
                    }
                }
            }
            spdlog::info("[errand] {:08X}: the opened shop(s) {} yield NO stop over the recorded needs (nothing to sell / buy there) -- dropped from the watch, he stays", a_follower, names);
            return false;
        }
        a_out.keys = keys;
        a_out.buyServed = std::move(buyServed);
        Assemble(std::move(shops), w.inn, a_out);
        a_out.line = std::format("{} {} | {}", hoursOpened ? "REOPEN" : !a_out.buyServed.empty() && w.gold.empty() ? "MISSED-BUY" : "GOLD", names, line);
        return true;
    }

    void Consume(RE::FormID a_follower, const Plan& a_plan) {
        std::scoped_lock lk(g_mx);
        const auto it = g_watch.find(a_follower);
        if (it == g_watch.end()) return;
        EraseKeys(it->second, a_plan.keys);
        if (!a_plan.buyServed.empty()) {   // MISSED BUY: the trip started -- those (kind, shop) pairs are spent, the kinds leave the watch until a trade reports them unfilled again
            ++it->second.buyReplans;
            for (const auto& p : a_plan.buyServed) {
                if (std::find(it->second.tried.begin(), it->second.tried.end(), p) == it->second.tried.end()) it->second.tried.push_back(p);
                std::erase_if(it->second.missed, [&](const TradeBridge::NeedCat& m) { return m.kind == p.first; });
            }
        }
    }

    void Retry(RE::FormID a_follower) {
        std::scoped_lock lk(g_mx);
        if (const auto it = g_watch.find(a_follower); it != g_watch.end()) it->second.lastHour = -1;
    }

    void ClearAll() {
        std::scoped_lock lk(g_mx);
        g_watch.clear();
        g_carried.clear();
        g_unserved.clear();
        g_liveTried.clear();
    }

    void NoteUnserved(RE::FormID a_follower, const TradeBridge::NeedCat& a_need, const ErrandPlanned& a_tmpl) {
        if (a_need.kind < 0 || a_need.kind > TradeBridge::NeedCat::kBolts || a_need.quota <= 0) return;
        std::scoped_lock lk(g_mx);
        auto& u = g_unserved[a_follower];
        MergeKind(u.needs, a_need);
        u.tmpl = a_tmpl;
    }

    void CarryAndClear() {
        std::vector<std::pair<RE::FormID, std::string>> said;
        {
            std::scoped_lock lk(g_mx);
            for (auto& [fid, w] : g_watch) {
                std::vector<TradeBridge::NeedCat> left = w.missed;
                for (const auto& n : w.unmet) MergeKind(left, n);
                std::erase_if(left, [](const TradeBridge::NeedCat& n) { return n.quota <= 0 || n.kind < 0 || n.kind > TradeBridge::NeedCat::kBolts; });
                if (left.empty()) continue;
                auto& c = g_carried[fid];
                std::string txt;
                for (const auto& n : left) { MergeKind(c.needs, n); txt += std::format("{}{} {}", txt.empty() ? "" : ", ", BuyKindName(n.kind), n.quota); }
                c.tmpl = w.missedTmpl;
                said.emplace_back(fid, txt);
            }
            for (auto& [fid, u] : g_unserved) {   // STOCK-ONLY: needs no shop of the town stocked
                std::vector<TradeBridge::NeedCat> left = u.needs;
                std::erase_if(left, [](const TradeBridge::NeedCat& n) { return n.quota <= 0 || n.kind < 0 || n.kind > TradeBridge::NeedCat::kBolts; });
                if (left.empty()) continue;
                auto& c = g_carried[fid];
                std::string txt;
                for (const auto& n : left) { MergeKind(c.needs, n); txt += std::format("{}{} {}", txt.empty() ? "" : ", ", BuyKindName(n.kind), n.quota); }
                if (c.tmpl.vendorKey == 0 && c.tmpl.purse == 0) c.tmpl = u.tmpl;
                said.emplace_back(fid, txt);
            }
            g_unserved.clear();
            g_watch.clear();
            g_liveTried.clear();   // the tried set belongs to the visit
        }
        for (const auto& [fid, txt] : said)
            spdlog::info("[errand] {:08X}: CARRIED NEED -- the town visit ended with {} still unmet; the next town's plan treats it as a forced need (stock-evidence shops first)", fid, txt);
    }

    int ApplyCarried(RE::FormID a_follower, const std::vector<Trip::Cand>& a_cands, std::vector<ErrandPlanned>& a_planned, const std::vector<TradeBridge::NeedCat>& a_liveNeeds) {
        Carried c;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_carried.find(a_follower);
            if (it == g_carried.end()) return 0;
            c = std::move(it->second);
            g_carried.erase(it);
        }
        int applied = 0;
        int purse = c.tmpl.purse;
        for (const auto& p : a_planned) if (!p.skipped && p.purse > 0) { purse = p.purse; break; }   // the live purse beats the stale template's
        c.tmpl.purse = purse;
        for (const auto& cn : c.needs) {
            const auto lp = std::find_if(a_liveNeeds.begin(), a_liveNeeds.end(), [&](const TradeBridge::NeedCat& l) { return l.kind == cn.kind && l.quota > 0; });
            if (lp == a_liveNeeds.end()) {   // his live threshold is met now: nothing to force (and no stale quota to buy)
                spdlog::info("[errand] {:08X}: carried need {} {} dropped -- his live count no longer shows it unmet", a_follower, BuyKindName(cn.kind), cn.quota);
                continue;
            }
            const TradeBridge::NeedCat n = *lp;   // the LIVE quota
            const bool live = std::any_of(a_planned.begin(), a_planned.end(), [&](const ErrandPlanned& p) {
                return !p.skipped && std::any_of(p.needs.begin(), p.needs.end(), [&](const TradeBridge::NeedCat& m) { return m.kind == n.kind && m.quota > 0; });
            });
            if (live) { spdlog::info("[errand] {:08X}: carried need {} {} is already a live need of this plan", a_follower, BuyKindName(n.kind), n.quota); continue; }
            const Trip::Cand* best = nullptr;
            for (const auto& cd : a_cands) {
                if (!Trip::StockNow(cd, n.kind)) continue;   // stock evidence only (a VEND keyword alone is not a claim)
                if (!best || cd.dist < best->dist) best = &cd;
            }
            if (!best) {   // no stockist in this town either: keep it for the next one (never dropped while his live count still shows it unmet)
                {
                    std::scoped_lock lk(g_mx);
                    auto& u = g_unserved[a_follower];
                    MergeKind(u.needs, n);
                    u.tmpl = c.tmpl;
                }
                spdlog::info("[errand] {:08X}: carried need {} {} -- no open shop of this town shows stock of it: no buy trip, carried on to the next town", a_follower, BuyKindName(n.kind), n.quota);
                continue;
            }
            ErrandPlanned o;
            const auto ex = std::find_if(a_planned.begin(), a_planned.end(), [&](const ErrandPlanned& p) { return p.vendorKey == best->key; });
            if (ex != a_planned.end() && !ex->skipped) {   // an evaluated order: add the kind to it
                o = *ex;
                o.needs.push_back(n);
                o.needKinds |= 1u << n.kind;
            } else {
                o = BuyOrderFor(c.tmpl, *best, { n });
            }
            if (ex != a_planned.end()) *ex = std::move(o); else a_planned.push_back(std::move(o));
            ++applied;
            spdlog::info("[errand] {:08X}: carried need {} {} FORCED into the plan at '{}' (stock evidence)", a_follower, BuyKindName(n.kind), n.quota, best->name);
        }
        return applied;
    }

    bool IsTried(RE::FormID a_follower, RE::FormID a_town, std::int32_t a_kind, RE::FormID a_key) {
        std::scoped_lock lk(g_mx);
        const std::pair<std::int32_t, RE::FormID> p{ a_kind, a_key };
        if (const auto lt = g_liveTried.find(a_follower); lt != g_liveTried.end() && lt->second.town == a_town &&
                                                           std::find(lt->second.pairs.begin(), lt->second.pairs.end(), p) != lt->second.pairs.end()) return true;
        if (const auto it = g_watch.find(a_follower); it != g_watch.end() && it->second.town == a_town &&
                                                       std::find(it->second.tried.begin(), it->second.tried.end(), p) != it->second.tried.end()) return true;
        return false;
    }

    void NoteTried(RE::FormID a_follower, RE::FormID a_town, std::int32_t a_kind, RE::FormID a_key) {
        std::scoped_lock lk(g_mx);
        AddLiveTried(a_follower, a_town, a_kind, a_key);
    }

    int Requeue(RE::FormID a_follower, RE::FormID a_town, std::vector<Stop>& a_stops, std::size_t a_cur, int a_purseLeft, const RE::NiPoint3& a_from) {
        auto* cal = RE::Calendar::GetSingleton();
        if (!cal || a_cur >= a_stops.size()) return 0;
        Watch w;
        {
            std::scoped_lock lk(g_mx);
            const auto it = g_watch.find(a_follower);
            if (it == g_watch.end() || it->second.town != a_town || it->second.missed.empty()) return 0;
            w = it->second;
            if (const auto lt = g_liveTried.find(a_follower); lt != g_liveTried.end() && lt->second.town == a_town)
                for (const auto& p : lt->second.pairs) if (std::find(w.tried.begin(), w.tried.end(), p) == w.tried.end()) w.tried.push_back(p);
        }
        // The gold must be there: the cheapest unit PlanBuy has seen for any missed kind (else the planner's own minimum purse).
        std::int32_t minUnit = INT32_MAX;
        for (const auto& m : w.missed) { const auto u = TradeBridge::MinSeenUnitPrice(m.kind); if (u > 0) minUnit = std::min(minUnit, u); }
        if (minUnit == INT32_MAX) minUnit = std::max(1, Config::g_errandMinPurse.load());
        if (a_purseLeft < 0 || a_purseLeft < minUnit) {
            spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE skipped -- trip purse left {}g < the cheapest unit {}g (a purse-limited miss is the purse's, not the stock's)", a_follower, a_purseLeft, minUnit);
            return 0;
        }
        const float hour = cal->GetHour();
        const auto key_of = [](const Stop& s) { return s.order && s.order->vendorKey ? s.order->vendorKey : (s.vendorActor ? s.vendorActor : s.faction); };
        const auto is_tried = [&](std::int32_t k, RE::FormID key) { return std::find(w.tried.begin(), w.tried.end(), std::pair<std::int32_t, RE::FormID>{ k, key }) != w.tried.end(); };
        const auto open_now = [&](const Trip::Cand& c) { auto* fac = RE::TESForm::LookupByID<RE::TESFaction>(c.faction); return fac && detail::OpenAtHour(fac, hour); };
        const auto cand_for = [&](RE::FormID key, RE::FormID faction) -> const Trip::Cand* {
            for (const auto& c : w.openShops) if (c.key == key) return &c;
            for (const auto& c : w.openShops) if (faction && c.faction == faction) return &c;
            return nullptr;
        };
        std::vector<std::pair<std::int32_t, RE::FormID>> triedNew;
        std::vector<std::int32_t> placed;
        int requeued = 0;
        for (const auto& m : w.missed) {
            if (m.kind < 0 || m.kind > TradeBridge::NeedCat::kBolts || m.quota <= 0) continue;
            // (a) FOLD: a still-pending stop of this trip whose shop SHOWS STOCK of the kind (a VEND keyword alone is not a claim) and has not been tried for it. Trip order.
            int foldAt = -1;
            for (std::size_t i = a_cur + 1; i < a_stops.size(); ++i) {
                const Stop& st = a_stops[i];
                if (st.kind != StopKind::Shop || st.state != StopState::Pending || !st.order) continue;
                const RE::FormID key = key_of(st);
                const Trip::Cand* c = cand_for(key, st.faction);
                if (!c || !Trip::StockNow(*c, m.kind) || is_tried(m.kind, key) || !open_now(*c)) continue;
                if (foldAt < 0) foldAt = static_cast<int>(i);
            }
            if (foldAt >= 0) {
                Stop& st = a_stops[static_cast<std::size_t>(foldAt)];
                const RE::FormID key = key_of(st);
                const Trip::Cand* c = cand_for(key, st.faction);
                ErrandPlanned o = *st.order;
                if (o.skipped) {
                    o = BuyOrderFor(w.missedTmpl, *c, { m });
                } else {
                    const auto ex = std::find_if(o.needs.begin(), o.needs.end(), [&](const TradeBridge::NeedCat& n) { return n.kind == m.kind; });
                    if (ex != o.needs.end()) ex->quota += m.quota; else o.needs.push_back(m);
                    o.needKinds |= 1u << m.kind;
                }
                st.order = std::make_shared<const ErrandPlanned>(std::move(o));
                triedNew.emplace_back(m.kind, key);
                placed.push_back(m.kind);
                ++requeued;
                spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE {} {} -> folded into the pending {} (stock evidence; trip purse left {}g >= the cheapest unit {}g)",
                             a_follower, BuyKindName(m.kind), m.quota, Trip::Label(a_stops, static_cast<std::size_t>(foldAt)), a_purseLeft, minUnit);
                continue;
            }
            // (b) INSERT: the best open untried candidate of the plan WITH STOCK EVIDENCE that is in no stop of this trip yet, before the first non-shop stop (the inn).
            std::size_t shopStops = 0;
            for (const auto& st : a_stops) if (st.kind == StopKind::Shop) ++shopStops;
            if (a_stops.size() >= kMaxTripStops || shopStops >= kMaxShopStops) {
                spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE {} {} -- the trip is full ({} stop(s), {} shop(s)): left for the hourly watch / the carry", a_follower, BuyKindName(m.kind), m.quota, a_stops.size(), shopStops);
                continue;
            }
            std::vector<const Trip::Cand*> order;
            for (const auto& c : w.openShops) {
                if (!Trip::StockNow(c, m.kind) || is_tried(m.kind, c.key) || !open_now(c)) continue;
                if (std::any_of(a_stops.begin(), a_stops.end(), [&](const Stop& st) { return st.kind == StopKind::Shop && (key_of(st) == c.key || st.faction == c.faction); })) continue;
                order.push_back(&c);
            }
            std::stable_sort(order.begin(), order.end(), [&](const Trip::Cand* a, const Trip::Cand* b) {
                const float da = a->havePos ? a_from.GetDistance(a->pos) : a->dist, db = b->havePos ? a_from.GetDistance(b->pos) : b->dist;
                return da < db;
            });
            bool done = false;
            int tries = 0;
            for (const Trip::Cand* c : order) {
                if (++tries > 3) break;
                Trip::Inputs in = w.in;
                in.follower = a_follower; in.from = a_from;
                std::vector<Stop> shops;
                std::string line;
                std::vector<Trip::Cand> one{ *c };
                std::vector<ErrandPlanned> ord{ BuyOrderFor(w.missedTmpl, *c, { m }) };
                triedNew.emplace_back(m.kind, c->key);   // whatever happens, this (kind, shop) is spent
                if (!Trip::Plan(one, ord, in, shops, line) || shops.empty()) {
                    spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE {} {} -- the planner finds no stop at '{}' (purse / floor): next candidate", a_follower, BuyKindName(m.kind), m.quota, c->name);
                    continue;
                }
                std::size_t at = a_cur + 1;
                while (at < a_stops.size() && a_stops[at].kind == StopKind::Shop) ++at;
                shops.front().state = StopState::Pending;
                a_stops.insert(a_stops.begin() + static_cast<std::ptrdiff_t>(at), std::move(shops.front()));
                placed.push_back(m.kind);
                ++requeued;
                done = true;
                spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE {} {} -> INSERTED {} (open, stock evidence; {}; trip purse left {}g >= the cheapest unit {}g)",
                             a_follower, BuyKindName(m.kind), m.quota, Trip::Label(a_stops, at),
                             at + 1 < a_stops.size() ? "before the later stops (the inn last)" : "at the end of the trip", a_purseLeft, minUnit);
                break;
            }
            if (!done)
                spdlog::info("[errand] {:08X}: IN-TRIP RE-QUEUE {} {} -- no other OPEN shop of this plan shows stock of it that was not tried (left for the carry to the next town)", a_follower, BuyKindName(m.kind), m.quota);
        }
        if (requeued > 0 || !triedNew.empty()) {
            std::scoped_lock lk(g_mx);
            if (const auto it = g_watch.find(a_follower); it != g_watch.end()) {
                for (const auto& p : triedNew) if (std::find(it->second.tried.begin(), it->second.tried.end(), p) == it->second.tried.end()) it->second.tried.push_back(p);
                for (const auto k : placed) std::erase_if(it->second.missed, [&](const TradeBridge::NeedCat& n) { return n.kind == k; });
            }
        }
        return requeued;
    }
}
