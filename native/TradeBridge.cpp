#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include <mutex>

namespace MFO::TradeBridge {

    namespace {

        // A stored order. Actors/refs are held by HANDLE (collect-then-act #2):
        // the dispatch fires this frame but MFO_Trade calls back a frame or more
        // later, by which point a raw pointer could dangle. Base objects (sell/buy
        // forms) are persistent, so raw is fine there.
        struct TradeOrder {
            RE::ActorHandle     follower;
            RE::ActorHandle     vendor;
            RE::ObjectRefHandle chest;
            std::vector<SellRow> sell;
            std::vector<BuyRow>  buy;
            std::int32_t         budget = 0;
            bool                 probeOnly = true;
        };

        std::mutex                                    g_mtx;
        std::unordered_map<std::int32_t, TradeOrder>  g_orders;
        std::atomic<std::int32_t>                     g_nextToken{ 1 };

        TradeOrder* Find(std::int32_t a_token) {   // caller holds g_mtx
            auto it = g_orders.find(a_token);
            return it == g_orders.end() ? nullptr : &it->second;
        }

        // ── Registered natives (called by MFO_Trade, on the VM thread) ────────
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

        std::vector<RE::TESForm*> GetBuyCandidates(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<RE::TESForm*> out;
            if (auto* o = Find(a_token)) {
                out.reserve(o->buy.size());
                for (auto& b : o->buy) out.push_back(b.obj);
            }
            return out;
        }

        bool GetProbeOnly(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            auto* o = Find(a_token);
            return o ? o->probeOnly : true;
        }

        // Papyrus hands back the barter-safe reads; native logs the full plan and
        // frees the token. `a_stock` is parallel to the buy rows (trailing entries
        // beyond buy.size() are the array's zero padding and ignored).
        void ReportProbe(RE::StaticFunctionTag*, std::int32_t a_token,
                         std::int32_t a_vendorGold, std::vector<std::int32_t> a_stock) {
            TradeOrder o;
            {
                std::scoped_lock lk(g_mtx);
                auto it = g_orders.find(a_token);
                if (it == g_orders.end()) {
                    spdlog::warn("[econprobe] ReportProbe: unknown token {}", a_token);
                    return;
                }
                o = std::move(it->second);
                g_orders.erase(it);
            }

            if (a_vendorGold < 0) {
                spdlog::info("[econprobe] token {} -- Papyrus found no usable chest (aborted)", a_token);
                return;
            }

            auto* fol = o.follower.get().get();
            auto* ven = o.vendor.get().get();
            const char* folN = fol && fol->GetName() ? fol->GetName() : "?";
            const char* venN = ven && ven->GetName() ? ven->GetName() : "?";

            // WOULD SELL.
            std::string sellStr;
            int sellN = 0, saleTotal = 0;
            for (auto& s : o.sell) {
                saleTotal += s.value * s.count;
                if (sellN < 8 && s.obj)
                    sellStr += std::format(" '{}'{} x{} val={}", s.obj->GetName(),
                                           s.jewelry ? " [jewelry]" : "", s.count, s.value);
                ++sellN;
            }
            spdlog::info("[econprobe] {:08X} '{}' @ vendor '{}': gold={} (PAPYRUS read, no crash)",
                         fol ? fol->GetFormID() : 0u, folN, venN, a_vendorGold);
            spdlog::info("[econprobe]   WOULD SELL:{}{} (n={} total={}, vendor can pay: {})",
                         sellStr.empty() ? " (nothing)" : sellStr.c_str(),
                         sellN > 8 ? " ..." : "", sellN, saleTotal,
                         saleTotal == 0 ? "n/a" : (a_vendorGold >= saleTotal ? "yes" : "SHORT"));

            // WOULD BUY -- pair each candidate with the stock Papyrus just read.
            std::string buyStr;
            for (std::size_t i = 0; i < o.buy.size(); ++i) {
                auto& b = o.buy[i];
                const int have = (i < a_stock.size()) ? a_stock[i] : 0;
                const bool ok  = have > 0 && b.value <= o.budget;
                buyStr += std::format(" [{} have={} want={} '{}' val={} stock={} -> {}]",
                                      b.label, b.have, b.want,
                                      b.obj && b.obj->GetName() ? b.obj->GetName() : "?",
                                      b.value, have, ok ? "BUY" : "skip");
            }
            spdlog::info("[econprobe]   WOULD BUY:{} | purse={}",
                         buyStr.empty() ? " (no supply gambits below N)" : buyStr.c_str(), o.budget);
        }

    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            spdlog::error("[trade] RegisterFuncs: no VM -- econ bridge disabled");
            return false;
        }
        a_vm->RegisterFunction("GetVendorChest",   "MFO_Trade", GetVendorChest);
        a_vm->RegisterFunction("GetVendorActor",   "MFO_Trade", GetVendorActor);
        a_vm->RegisterFunction("GetBuyCandidates", "MFO_Trade", GetBuyCandidates);
        a_vm->RegisterFunction("GetProbeOnly",     "MFO_Trade", GetProbeOnly);
        a_vm->RegisterFunction("ReportProbe",      "MFO_Trade", ReportProbe);
        spdlog::info("[trade] registered MFO_Trade natives (probe: chest/actor/candidates/report)");
        return true;
    }

    void VendorProbe(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::vector<BuyRow> a_buy,
                     std::int32_t a_budget) {
        auto* q = Forms::g_tradeQuest;
        if (!q || !q->IsRunning()) return;   // bridge unavailable -> silently skip this scan
        if (!a_follower || !a_chest) return;

        const std::int32_t token = g_nextToken.fetch_add(1);
        {
            std::scoped_lock lk(g_mtx);
            TradeOrder o;
            o.follower  = a_follower->GetHandle();
            o.vendor    = a_vendor ? a_vendor->GetHandle() : RE::ActorHandle{};
            o.chest     = a_chest->GetHandle();
            o.sell      = std::move(a_sell);
            o.buy       = std::move(a_buy);
            o.budget    = a_budget;
            o.probeOnly = true;
            g_orders[token] = std::move(o);
        }
        spdlog::info("[econprobe] {:08X} dispatching probe token {} (vendor {:08X})",
                     a_follower->GetFormID(), token, a_vendor ? a_vendor->GetFormID() : 0u);
        if (!Papyrus::DispatchTradeRun(q, token)) {
            std::scoped_lock lk(g_mtx);
            g_orders.erase(token);   // dispatch failed -> don't leak the order
            spdlog::warn("[econprobe] dispatch failed for token {}", token);
        }
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mtx);
        g_orders.clear();
    }

}
