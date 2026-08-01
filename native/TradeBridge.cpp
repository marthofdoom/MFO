#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include "Config.h"
#include "ItemCatalog.h"
#include "Logistics.h"   // Logistics::PotionRestores -- the SAME classifier CountPotions uses
#include <mutex>

namespace MFO::TradeBridge {

    namespace {

        struct TradeOrder {
            RE::ActorHandle      follower;
            RE::ActorHandle      vendor;
            RE::ObjectRefHandle  chest;
            std::vector<SellRow> sell;
            std::vector<NeedCat> needs;      // mutated by PlanBuy as quotas fill
            std::int32_t         budget = 0; // follower purse; PlanBuy spends it down
            std::int32_t         buySpent = 0;
            bool                 probeOnly = true;
        };

        std::mutex                                    g_mtx;
        std::unordered_map<std::int32_t, TradeOrder>  g_orders;
        std::atomic<std::int32_t>                     g_nextToken{ 1 };

        TradeOrder* Find(std::int32_t a_token) {   // caller holds g_mtx
            auto it = g_orders.find(a_token);
            return it == g_orders.end() ? nullptr : &it->second;
        }

        int BaseValue(RE::TESForm* a_form) {
            auto* vf = a_form ? a_form->As<RE::TESValueForm>() : nullptr;
            return vf ? vf->value : 0;
        }

        // Which buy category is this stock form, if any? -1 = not a supply the
        // gambits ask for. Classification is catalog-first (the Synthesis patcher
        // read the real records), so it needs a current mfo_items.json.
        std::int32_t ClassifyBuy(RE::TESForm* a_form) {
            if (!a_form) return -1;
            const auto id = a_form->GetFormID();
            if (auto* alc = a_form->As<RE::AlchemyItem>()) {
                // The SAME classifier the follower counts/drinks with (catalog-first,
                // then the archetype heuristic) -- catalog-only left the vendor's
                // health potions unclassified, so a follower that NEEDED potions (its
                // count uses this classifier) bought none (field: Arcadia, BUY 0).
                switch (Logistics::PotionRestores(alc)) {
                    case RE::ActorValue::kHealth:  return NeedCat::kPotHealth;
                    case RE::ActorValue::kStamina: return NeedCat::kPotStamina;
                    case RE::ActorValue::kMagicka: return NeedCat::kPotMagicka;
                    default: return -1;
                }
            }
            if (a_form->As<RE::TESAmmo>()) {
                switch (Catalog::AmmoKind(id)) {
                    case Catalog::Ammo::kArrow: return NeedCat::kArrows;
                    case Catalog::Ammo::kBolt:  return NeedCat::kBolts;
                    default: return -1;
                }
            }
            return -1;
        }

        // ── Registered natives (called by MFO_Trade, on the VM thread) ────────
        RE::Actor* GetTradeFollower(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->follower.get().get() : nullptr;
        }

        RE::TESObjectREFR* GetVendorChest(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->chest.get().get() : nullptr;
        }

        RE::Actor* GetVendorActor(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->vendor.get().get() : nullptr;
        }

        bool GetProbeOnly(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->probeOnly : true;
        }

        std::vector<RE::TESForm*> GetSellForms(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<RE::TESForm*> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.obj); }
            return out;
        }
        std::vector<std::int32_t> GetSellCounts(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.count); }
            return out;
        }
        std::vector<std::int32_t> GetSellValues(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) { out.reserve(o->sell.size()); for (auto& s : o->sell) out.push_back(s.value); }
            return out;
        }

        // BUY PLAN: given the vendor's ACTUAL stock (Papyrus enumerated it), decide
        // how many of each form to buy. "Best they can afford, up to the number
        // needed" (marth): rank the needed-category stock by value DESC and buy the
        // best first, filling each category's quota, bounded by the shared purse.
        // Returns qty[] parallel to a_forms; the spent gold is cached for
        // GetBuySpent. Mutates the order's quotas/budget (single-shot per trade).
        std::vector<std::int32_t> PlanBuy(RE::StaticFunctionTag*, std::int32_t a_token,
                                          std::vector<RE::TESForm*> a_forms,
                                          std::vector<std::int32_t> a_counts) {
            std::vector<std::int32_t> plan(a_forms.size(), 0);
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            if (!o || o->needs.empty()) return plan;

            // Candidate stock lines of a needed category, value-desc.
            struct Cand { std::size_t idx; std::int32_t kind, value, avail; };
            std::vector<Cand> cands;
            for (std::size_t i = 0; i < a_forms.size(); ++i) {
                auto* f = a_forms[i];
                const int avail = (i < a_counts.size()) ? a_counts[i] : 0;
                if (!f || avail <= 0) continue;
                if (Catalog::IsExcluded(f->GetFormID())) continue;
                const int kind = ClassifyBuy(f);
                if (kind < 0) continue;
                const int val = BaseValue(f);
                if (val <= 0) continue;   // free/valueless stock isn't a real buy
                cands.push_back(Cand{ i, kind, val, avail });
            }
            std::sort(cands.begin(), cands.end(),
                      [](const Cand& a, const Cand& b) { return a.value > b.value; });

            for (const auto& c : cands) {
                if (o->budget < c.value) continue;   // can't afford even one
                // Remaining quota for this category.
                NeedCat* need = nullptr;
                for (auto& n : o->needs) if (n.kind == c.kind && n.quota > 0) { need = &n; break; }
                if (!need) continue;
                int qty = c.avail;
                if (qty > need->quota)              qty = need->quota;
                if (qty > o->budget / c.value)      qty = o->budget / c.value;
                if (qty <= 0) continue;
                plan[c.idx] += qty;
                need->quota  -= qty;
                o->budget    -= qty * c.value;
                o->buySpent  += qty * c.value;
            }
            return plan;
        }

        std::int32_t GetBuySpent(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->buySpent : 0;
        }

        // Papyrus reports the result; native logs it + frees the token.
        void ReportTrade(RE::StaticFunctionTag*, std::int32_t a_token,
                         std::int32_t a_soldValue, std::int32_t a_soldCount,
                         std::int32_t a_boughtCount, std::int32_t a_spent,
                         std::int32_t a_vendorGold) {
            TradeOrder o;
            {
                std::scoped_lock lk(g_mtx);
                auto it = g_orders.find(a_token);
                if (it == g_orders.end()) { spdlog::warn("[econ] ReportTrade: unknown token {}", a_token); return; }
                o = std::move(it->second);
                g_orders.erase(it);
            }
            if (a_vendorGold < 0) {
                spdlog::info("[econ] token {} -- Papyrus found no usable chest (aborted)", a_token);
                return;
            }
            auto* fol = o.follower.get().get();
            auto* ven = o.vendor.get().get();
            const char* verb = o.probeOnly ? "WOULD" : "did";
            spdlog::info("[econ] {:08X} '{}' @ '{}': chest gold={} | {} SELL {} for {}g, BUY {} for {}g "
                         "(sell n={}, needs={}, purse={})",
                         fol ? fol->GetFormID() : 0u,
                         fol && fol->GetName() ? fol->GetName() : "?",
                         ven && ven->GetName() ? ven->GetName() : "?",
                         a_vendorGold, verb, a_soldCount, a_soldValue, a_boughtCount, a_spent,
                         o.sell.size(), o.needs.size(), o.budget);
        }

    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) { spdlog::error("[trade] RegisterFuncs: no VM -- econ bridge disabled"); return false; }
        a_vm->RegisterFunction("GetTradeFollower", "MFO_Trade", GetTradeFollower);
        a_vm->RegisterFunction("GetVendorChest",   "MFO_Trade", GetVendorChest);
        a_vm->RegisterFunction("GetVendorActor",   "MFO_Trade", GetVendorActor);
        a_vm->RegisterFunction("GetProbeOnly",     "MFO_Trade", GetProbeOnly);
        a_vm->RegisterFunction("GetSellForms",     "MFO_Trade", GetSellForms);
        a_vm->RegisterFunction("GetSellCounts",    "MFO_Trade", GetSellCounts);
        a_vm->RegisterFunction("GetSellValues",    "MFO_Trade", GetSellValues);
        a_vm->RegisterFunction("PlanBuy",          "MFO_Trade", PlanBuy);
        a_vm->RegisterFunction("GetBuySpent",      "MFO_Trade", GetBuySpent);
        a_vm->RegisterFunction("ReportTrade",      "MFO_Trade", ReportTrade);
        spdlog::info("[trade] registered MFO_Trade natives (sell + buy bridge)");
        return true;
    }

    void VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<NeedCat> a_needs,
                     std::int32_t a_budget) {
        auto* q = Forms::g_tradeQuest;
        if (!q || !q->IsRunning()) return;
        if (!a_follower || !a_chest) return;

        const std::int32_t token = g_nextToken.fetch_add(1);
        {
            std::scoped_lock lk(g_mtx);
            TradeOrder o;
            o.follower  = a_follower->GetHandle();
            o.vendor    = a_vendor ? a_vendor->GetHandle() : RE::ActorHandle{};
            o.chest     = a_chest->GetHandle();
            o.sell      = std::move(a_sell);
            o.needs     = std::move(a_needs);
            o.budget    = a_budget;
            o.probeOnly = !Config::g_economy.load();
            g_orders[token] = std::move(o);
        }
        if (!Papyrus::DispatchTradeRun(q, token)) {
            std::scoped_lock lk(g_mtx);
            g_orders.erase(token);
            spdlog::warn("[econ] dispatch failed for token {}", token);
        }
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mtx);
        g_orders.clear();
    }

}
