// logistics/ErrandTrip.cpp -- TOWN ERRANDS phase 2, round R5 (design _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md 1.b):
// the MULTI-STOP TRIP planner and the itinerary helpers. A trip visits up to kMaxShopStops shops in one walk, with ONE purse:
//
//   1. SELL PARTITION. Every sell object goes to the candidate that pays the most for it (unit price x count over the rows that
//      vendor's VEND filter accepts, already priced by EconomyErrandPlan), capped by that chest's last-seen barter gold, minus
//      the B300 convergence penalty (kCrowdPenalty per OTHER follower whose trip is heading to that chest). A candidate whose
//      share does not clear the phase-1 sell reason (>= iErrandMinSellGold, or carry > 80 %) hands its rows to a stop that does;
//      once every stop is chosen, a row no visited stop owns goes to the visited stop that pays the most for it.
//   2. NEEDS (supply gambits) go to the FIRST stop in visit order whose VEND carries the category; a need no chosen stop
//      carries adds the nearest (least crowded) candidate that does.
//   3. WISHES (gear / apparel = BuyThresholds::buyGear, tomes = buyTomes) go to ONE stop each; an extra stop is added for a
//      wish only when the purse clears iErrandMinPurse (the phase-1 wish reason).
//   4. ORDER: nearest-neighbour from the follower, stops WITHOUT a wish first, then the wish stops ("needs first, then wishes":
//      the 50 % reserve of PlanBuy then sees the purse the needs left). Stops with no work are dropped. R7 appends the Inn
//      after the shops. The ammo swap-up (BuyThresholds::ammoUpgrade) rides the first stop that sells ammo, never two.
//   5. PURSE: each stop's order carries the trip purse; the trade is capped at what the trip purse has left (tripPurse -
//      tripSpent, Errand.cpp / ErrandSim.cpp), so PlanBuy's reserve rule applies to the TRIP purse, not per stop.
//
// MFO only PLANS and DECLARES here: the walks are ch.19 claims (apmf/ErrandClaims.cpp), the trades the unchanged TradeBridge.
// THREADING: Plan runs on the AddTask job worker (TryStart, the follower loaded); no lock is held by the caller. ChestCrowd takes
// Errand g_mx, then Sim g_mx, one after the other (never nested). The helpers below touch no shared state.
#include "PCH.h"
#include "Errand_internal.h"
#include "Config.h"   // g_errandMaxGameMin: the trip cap (CapGameMin)

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::Logistics::Errand::Trip {

    namespace {
        // B300: what one OTHER follower already heading to a chest costs that chest in the plan, in gold-equivalent (the same
        // scale as the phase-1 ranker's need / wish weights, 250 / 150). Two followers at one chest wait on the per-chest
        // in-flight guard (TradeBridge) for up to ~33 s; this steers the second one to another vendor of the same kind when
        // there is one, and still sends him when there is not.
        constexpr float  kCrowdPenalty   = 150.0f;
        // A leg whose place has no known position (a CELL shop restored from a save): a typical street walk between two shops.
        constexpr double kUnknownLegUnits = 2000.0;

        struct Use {                       // one usable candidate: a candidate the plan evaluated and SkipTrade let through
            const Cand*          c = nullptr;
            const ErrandPlanned* p = nullptr;
            int                  crowd = 0;
            std::int64_t         goldLeft = -1;   // the chest's last-seen gold not yet promised to a row (-1 unknown)
            bool                 chosen = false;
            bool                 gear = false, tome = false;   // the wish this stop carries in the trip
            std::uint32_t        needs = 0;                    // the need kinds this stop serves in the trip
        };

        std::int64_t RowValue(const ErrandPlanned& a_p, RE::TESBoundObject* a_obj) {
            std::int64_t v = 0;
            for (const auto& r : a_p.sell)
                if (r.obj == a_obj) v += static_cast<std::int64_t>(r.value) * std::max<std::int32_t>(r.count, 0);
            return v;
        }

        float Dist(const RE::NiPoint3& a_from, const Cand& a_c) {
            return a_c.havePos ? a_from.GetDistance(a_c.pos) : a_c.dist;
        }
    }

    const char* StateName(StopState a_s) {
        switch (a_s) {
            case StopState::Pending: return "pending";
            case StopState::Active:  return "active";
            case StopState::Done:    return "done";
            case StopState::Skipped: return "skipped";
        }
        return "?";
    }

    float CapGameMin(std::size_t a_stops) {
        return Config::g_errandMaxGameMin.load() * static_cast<float>(std::max<std::size_t>(1, a_stops));
    }

    int NextPending(const std::vector<Stop>& a_stops, std::size_t a_cur) {
        for (std::size_t i = a_cur + 1; i < a_stops.size(); ++i)
            if (a_stops[i].state == StopState::Pending) return static_cast<int>(i);
        return -1;
    }

    bool PlacePos(const Stop& a_stop, RE::NiPoint3& a_out) {
        if (a_stop.destKind == DestKind::VendorActor && a_stop.destForm) {
            if (auto* v = RE::TESForm::LookupByID<RE::Actor>(a_stop.destForm)) { a_out = v->GetPosition(); return true; }
        } else if (a_stop.destKind == DestKind::Ref && a_stop.destForm) {
            if (auto* r = RE::TESForm::LookupByID<RE::TESObjectREFR>(a_stop.destForm)) { a_out = r->GetPosition(); return true; }
        }
        if (a_stop.havePos) { a_out = a_stop.pos; return true; }
        return false;
    }

    double LegUnits(const Stop& a_from, const Stop& a_to) {
        RE::NiPoint3 a, b;
        if (PlacePos(a_from, a) && PlacePos(a_to, b)) return static_cast<double>(a.GetDistance(b));
        return kUnknownLegUnits;
    }

    std::string Label(const std::vector<Stop>& a_stops, std::size_t a_cur) {
        const std::string name = a_cur < a_stops.size() && !a_stops[a_cur].vendorName.empty() ? a_stops[a_cur].vendorName : "?";
        return std::format("stop {}/{} '{}'", a_cur + 1, a_stops.size(), name);
    }

    SavedStop ToSaved(const Stop& a_stop) {
        SavedStop s;
        s.kind = SavedStopKind::Shop;
        s.faction = a_stop.faction; s.chest = a_stop.chest; s.vendorActor = a_stop.vendorActor;
        s.destKind = a_stop.destKind == DestKind::VendorActor ? SavedDest::VendorActor
                   : a_stop.destKind == DestKind::Ref         ? SavedDest::Ref : SavedDest::Cell;
        s.destForm = a_stop.destForm; s.destRad = a_stop.destRad;
        s.state = a_stop.state == StopState::Pending ? SavedStopState::Pending
                : a_stop.state == StopState::Active  ? SavedStopState::Active
                : a_stop.state == StopState::Done    ? SavedStopState::Done : SavedStopState::Skipped;
        s.tArrive = a_stop.tArrive; s.tWorkEnd = a_stop.tWorkEnd;
        return s;
    }

    Stop FromSaved(const SavedStop& a_saved) {
        Stop s;
        s.kind = StopKind::Shop;
        s.faction = a_saved.faction; s.chest = a_saved.chest; s.vendorActor = a_saved.vendorActor;
        s.destKind = a_saved.destKind == SavedDest::VendorActor ? DestKind::VendorActor
                   : a_saved.destKind == SavedDest::Ref         ? DestKind::Ref : DestKind::Cell;
        s.destForm = a_saved.destForm; s.destRad = a_saved.destRad;
        s.state = a_saved.state == SavedStopState::Pending ? StopState::Pending
                : a_saved.state == SavedStopState::Active  ? StopState::Active
                : a_saved.state == SavedStopState::Done    ? StopState::Done : StopState::Skipped;
        s.tArrive = a_saved.tArrive; s.tWorkEnd = a_saved.tWorkEnd;
        if (auto* va = s.vendorActor ? RE::TESForm::LookupByID<RE::Actor>(s.vendorActor) : nullptr) s.vendorName = detail::NameOf(va);
        else if (auto* fac = s.faction ? RE::TESForm::LookupByID<RE::TESFaction>(s.faction) : nullptr; fac && fac->GetName() && *fac->GetName())
            s.vendorName = fac->GetName();
        else s.vendorName = "?";
        return s;
    }

    std::unordered_map<RE::FormID, int> ChestCrowd(RE::FormID a_exclude) {
        std::unordered_map<RE::FormID, int> crowd;
        detail::CrowdInto(crowd, a_exclude);   // Errand g_mx (released on return)
        Sim::CrowdInto(crowd, a_exclude);      // then Sim g_mx
        return crowd;
    }

    bool Plan(const std::vector<Cand>& a_cands, const std::vector<ErrandPlanned>& a_planned, const Inputs& a_in,
              std::vector<Stop>& a_out, std::string& a_line) {
        a_out.clear();
        a_line.clear();
        const auto crowd = ChestCrowd(a_in.follower);

        std::vector<Use> use;
        for (const auto& p : a_planned) {
            if (p.skipped) continue;
            const Cand* c = nullptr;
            for (const auto& k : a_cands) if (k.key == p.vendorKey) { c = &k; break; }
            if (!c) continue;
            Use u;
            u.c = c; u.p = &p;
            if (const auto it = crowd.find(c->chest); it != crowd.end()) u.crowd = it->second;
            u.goldLeft = p.chestGold >= 0 ? p.chestGold : -1;
            use.push_back(u);
        }
        if (use.empty()) return false;

        // The follower-level facts (identical in every candidate's evaluation: his needs, his wishes, his purse).
        int purse = 0;
        std::uint32_t needKinds = 0;
        bool wishGear = false, wishTome = false;
        for (const auto& u : use) {
            purse = std::max(purse, u.p->purse);
            needKinds |= u.p->needKinds;
            wishGear = wishGear || u.p->wishGear || u.p->wishApparel;
            wishTome = wishTome || u.p->wishTome;
        }
        const bool rich = purse >= a_in.minPurse;

        // ── 1. SELL PARTITION ──────────────────────────────────────────────────────────────────────
        std::vector<RE::TESBoundObject*> objs;
        {
            std::unordered_set<RE::TESBoundObject*> seen;
            for (const auto& u : use)
                for (const auto& r : u.p->sell)
                    if (r.obj && seen.insert(r.obj).second) objs.push_back(r.obj);
        }
        auto bestRow = [&](RE::TESBoundObject* o) {
            std::int64_t b = 0;
            for (const auto& u : use) b = std::max(b, RowValue(*u.p, o));
            return b;
        };
        std::stable_sort(objs.begin(), objs.end(), [&](RE::TESBoundObject* a, RE::TESBoundObject* b) {
            const auto va = bestRow(a), vb = bestRow(b);
            return va != vb ? va > vb : a->GetFormID() < b->GetFormID();
        });
        std::unordered_map<RE::TESBoundObject*, int> owner;   // object -> index into use (-1 = not sold this trip)
        auto pickFor = [&](RE::TESBoundObject* o, auto&& a_allowed) -> int {
            int best = -1;
            float bestEff = 0.0f, bestDist = 0.0f;
            for (int i = 0; i < static_cast<int>(use.size()); ++i) {
                if (!a_allowed(i)) continue;
                const std::int64_t rv = RowValue(*use[i].p, o);
                if (rv <= 0) continue;   // this vendor does not accept it
                const std::int64_t paid = use[i].goldLeft >= 0 ? std::min(rv, use[i].goldLeft) : rv;
                const float eff = static_cast<float>(paid) - kCrowdPenalty * static_cast<float>(use[i].crowd);
                const float d = Dist(a_in.from, *use[i].c);
                if (best < 0 || eff > bestEff || (eff == bestEff && d < bestDist)) { best = i; bestEff = eff; bestDist = d; }
            }
            return best;
        };
        for (auto* o : objs) {
            const int i = pickFor(o, [](int) { return true; });
            owner[o] = i;
            if (i >= 0 && use[i].goldLeft >= 0) use[i].goldLeft = std::max<std::int64_t>(0, use[i].goldLeft - RowValue(*use[i].p, o));
        }
        auto sellGold = [&](int i) {   // what the chest can pay for the rows this stop owns (last-seen gold; unknown = uncapped)
            std::int64_t v = 0;
            int rows = 0;
            for (const auto& [o, w] : owner) if (w == i) { v += RowValue(*use[i].p, o); ++rows; }
            if (use[i].p->chestGold >= 0) v = std::min<std::int64_t>(v, use[i].p->chestGold);
            return std::pair<std::int64_t, int>{ v, rows };
        };
        auto qualifies = [&](int i) {
            const auto [g, rows] = sellGold(i);
            return rows > 0 && (g >= a_in.minSell || a_in.carry > 0.8f);
        };
        // A candidate whose share does not clear the sell reason hands its rows to one that does (or they wait for another trip).
        {
            std::vector<char> ok(use.size(), 0);
            for (int i = 0; i < static_cast<int>(use.size()); ++i) ok[i] = qualifies(i) ? 1 : 0;
            for (auto& [o, w] : owner)
                if (w >= 0 && !ok[w]) w = pickFor(o, [&](int j) { return ok[j] != 0; });
            for (int i = 0; i < static_cast<int>(use.size()); ++i) if (ok[i] && qualifies(i)) use[i].chosen = true;
        }
        // The stop cap: keep the best-paying sell stops (crowd-penalised), hand the rest's rows to a kept one.
        {
            std::vector<int> sellers;
            for (int i = 0; i < static_cast<int>(use.size()); ++i) if (use[i].chosen) sellers.push_back(i);
            if (sellers.size() > kMaxShopStops) {
                std::stable_sort(sellers.begin(), sellers.end(), [&](int a, int b) {
                    return static_cast<float>(sellGold(a).first) - kCrowdPenalty * use[a].crowd >
                           static_cast<float>(sellGold(b).first) - kCrowdPenalty * use[b].crowd;
                });
                for (std::size_t k = kMaxShopStops; k < sellers.size(); ++k) use[sellers[k]].chosen = false;
                for (auto& [o, w] : owner)
                    if (w >= 0 && !use[w].chosen) w = pickFor(o, [&](int j) { return use[j].chosen; });
            }
        }
        auto chosenCount = [&] { return static_cast<std::size_t>(std::count_if(use.begin(), use.end(), [](const Use& u) { return u.chosen; })); };
        // Adding a stop for a need / wish: the nearest, least crowded candidate that carries it.
        auto addNearest = [&](auto&& a_carries) -> int {
            if (chosenCount() >= kMaxShopStops) return -1;
            int best = -1;
            float bestScore = 0.0f;
            for (int i = 0; i < static_cast<int>(use.size()); ++i) {
                if (use[i].chosen || !a_carries(i)) continue;
                const float score = Dist(a_in.from, *use[i].c) / 10.0f + kCrowdPenalty * static_cast<float>(use[i].crowd);
                if (best < 0 || score < bestScore) { best = i; bestScore = score; }
            }
            if (best >= 0) use[best].chosen = true;
            return best;
        };

        // ── 2. NEEDS: make sure a chosen stop carries each one (assigned to the first in visit order below) ──
        std::uint32_t unserved = 0;
        for (int k = 0; k < 32; ++k) {
            if (!((needKinds >> k) & 1u)) continue;
            const bool have = std::any_of(use.begin(), use.end(), [&](const Use& u) { return u.chosen && ((u.c->needCarry >> k) & 1u); });
            if (!have && addNearest([&](int i) { return ((use[i].c->needCarry >> k) & 1u) != 0; }) < 0) unserved |= 1u << k;
        }
        // ── 3. WISHES: one stop each; a new stop only when the purse clears iErrandMinPurse ──
        auto placeWish = [&](bool a_want, bool Cand::* a_carry, bool Use::* a_flag, int a_prefer) -> int {
            if (!a_want) return -1;
            if (a_prefer >= 0 && use[a_prefer].c->*a_carry) { use[a_prefer].*a_flag = true; return a_prefer; }
            for (int i = 0; i < static_cast<int>(use.size()); ++i)
                if (use[i].chosen && use[i].c->*a_carry) { use[i].*a_flag = true; return i; }
            if (!rich) return -1;
            const int i = addNearest([&](int j) { return use[j].c->*a_carry; });
            if (i >= 0) use[i].*a_flag = true;
            return i;
        };
        const int gearAt = placeWish(wishGear, &Cand::carriesGear, &Use::gear, -1);
        placeWish(wishTome, &Cand::carriesTome, &Use::tome, gearAt);

        // Rows no chosen stop owns yet (their best payer is not visited, or alone it missed the sell reason): sold at the visited stop
        // that pays the most for them, if any accepts them -- he is there anyway (phase 1 sold every row the visited vendor accepts).
        for (auto& [o, w] : owner)
            if (w < 0 || !use[w].chosen) w = pickFor(o, [&](int j) { return use[j].chosen; });

        // ── 4. ORDER: nearest-neighbour, the stops with no wish first ──
        std::vector<int> order;
        {
            RE::NiPoint3 at = a_in.from;
            bool atKnown = true;
            for (int tier = 0; tier < 2; ++tier) {
                std::vector<int> left;
                for (int i = 0; i < static_cast<int>(use.size()); ++i)
                    if (use[i].chosen && ((use[i].gear || use[i].tome) ? 1 : 0) == tier) left.push_back(i);
                while (!left.empty()) {
                    std::size_t bi = 0;
                    float bd = 0.0f;
                    for (std::size_t j = 0; j < left.size(); ++j) {
                        const Cand& c = *use[left[j]].c;
                        const float d = (atKnown && c.havePos) ? at.GetDistance(c.pos) : c.dist;
                        if (j == 0 || d < bd) { bi = j; bd = d; }
                    }
                    const int pick = left[bi];
                    left.erase(left.begin() + static_cast<std::ptrdiff_t>(bi));
                    order.push_back(pick);
                    atKnown = use[pick].c->havePos;
                    if (atKnown) at = use[pick].c->pos;
                }
            }
        }
        // Needs to the FIRST stop in visit order that carries them.
        for (int k = 0; k < 32; ++k) {
            if (!((needKinds >> k) & 1u) || ((unserved >> k) & 1u)) continue;
            for (const int i : order)
                if ((use[i].c->needCarry >> k) & 1u) { use[i].needs |= 1u << k; break; }
        }

        // ── 5. the stops, each with its SHARE of the order ──
        std::string body;
        bool ammoPlaced = false;   // the ammo swap-up (BuyThresholds::ammoUpgrade) rides ONE stop: the first that sells ammo
        for (const int i : order) {
            const auto& base = *use[i].p;
            auto pl = std::make_shared<ErrandPlanned>(base);
            pl->sell.clear();
            for (const auto& r : base.sell) if (const auto it = owner.find(r.obj); it != owner.end() && it->second == i) pl->sell.push_back(r);
            pl->needs.clear();
            for (const auto& n : base.needs) if (n.kind >= 0 && n.kind < 32 && ((use[i].needs >> n.kind) & 1u)) pl->needs.push_back(n);
            pl->buy.buyGear  = base.buy.buyGear && use[i].gear;
            pl->buy.buyTomes = base.buy.buyTomes && use[i].tome;
            const bool sellsAmmo = ((use[i].c->needCarry >> TradeBridge::NeedCat::kArrows) & 1u) != 0;
            pl->buy.ammoUpgrade = base.buy.ammoUpgrade && sellsAmmo && !ammoPlaced;
            pl->wishGear = base.wishGear && use[i].gear;
            pl->wishApparel = base.wishApparel && use[i].gear;
            pl->wishTome = base.wishTome && use[i].tome;
            pl->sellRows = static_cast<int>(pl->sell.size());
            pl->sellValue = 0;
            for (const auto& r : pl->sell) pl->sellValue += static_cast<std::int64_t>(r.value) * std::max<std::int32_t>(r.count, 0);
            pl->needKinds = 0;
            for (const auto& n : pl->needs) pl->needKinds |= 1u << n.kind;
            if (pl->sell.empty() && pl->needs.empty() && !pl->buy.buyGear && !pl->buy.buyTomes) continue;   // no work: dropped
            if (pl->buy.ammoUpgrade) ammoPlaced = true;
            pl->sig = TradeBridge::TradeSignature(pl->purse, pl->needs, pl->sell);   // the memo describes what THIS stop offers

            const Cand& c = *use[i].c;
            Stop s;
            s.kind = StopKind::Shop;
            s.faction = c.faction; s.chest = c.chest; s.vendorActor = c.actorId; s.vendorName = c.name;
            s.destKind = c.kind; s.destForm = c.destForm; s.destRad = c.destRad;
            s.pos = c.pos; s.havePos = c.havePos;
            s.order = std::move(pl);
            s.state = a_out.empty() ? StopState::Active : StopState::Pending;
            const auto& o = *s.order;
            body += std::format("{}{} '{}' ({:08X}): sell {} row(s) {}g (chest gold {}), needs {:#x}, wish gear={} tome={}{}",
                                a_out.empty() ? "" : " -> ", a_out.size() + 1, c.name, c.key, o.sellRows, o.sellValue, o.chestGold,
                                o.needKinds, o.buy.buyGear, o.buy.buyTomes,
                                use[i].crowd ? std::format(", crowd {} (-{:.0f}g)", use[i].crowd, kCrowdPenalty * use[i].crowd) : std::string{});
            a_out.push_back(std::move(s));
        }
        if (a_out.empty()) return false;
        a_line = std::format("{} stop(s): {} | purse {} (trip), carry {:.0f}%, needs {:#x}{}, wishes gear={} tome={} (rich={}) | {} usable "
                             "candidate(s)", a_out.size(), body, purse, a_in.carry * 100.0f, needKinds,
                             unserved ? std::format(" (no open shop carries {:#x})", unserved) : std::string{}, wishGear, wishTome,
                             rich, use.size());
        return true;
    }
}
