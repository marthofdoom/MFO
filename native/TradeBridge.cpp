#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"
#include "Config.h"
#include <mutex>

namespace MFO::TradeBridge {

    namespace {

        // A stored order. Actors/refs are held by HANDLE (collect-then-act #2):
        // the dispatch fires this frame but MFO_Trade calls back a frame or more
        // later, by which point a raw pointer could dangle. Base objects (sell
        // forms) are persistent, so raw is fine there.
        struct TradeOrder {
            RE::ActorHandle      follower;
            RE::ActorHandle      vendor;
            RE::ObjectRefHandle  chest;
            std::vector<SellRow> sell;
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
            if (auto* o = Find(a_token)) {
                out.reserve(o->sell.size());
                for (auto& s : o->sell) out.push_back(s.obj);
            }
            return out;
        }

        std::vector<std::int32_t> GetSellCounts(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) {
                out.reserve(o->sell.size());
                for (auto& s : o->sell) out.push_back(s.count);
            }
            return out;
        }

        std::vector<std::int32_t> GetSellValues(RE::StaticFunctionTag*, std::int32_t a_token) {
            std::scoped_lock lk(g_mtx);
            std::vector<std::int32_t> out;
            if (auto* o = Find(a_token)) {
                out.reserve(o->sell.size());
                for (auto& s : o->sell) out.push_back(s.value);
            }
            return out;
        }

        // Papyrus reports the result and native logs it + frees the token.
        // a_soldValue/a_soldCount are what the SELL loop actually moved (or WOULD
        // move on a dry run); a_vendorGold is the chest's barter gold it read.
        void ReportTrade(RE::StaticFunctionTag*, std::int32_t a_token,
                         std::int32_t a_soldValue, std::int32_t a_soldCount,
                         std::int32_t a_vendorGold) {
            TradeOrder o;
            {
                std::scoped_lock lk(g_mtx);
                auto it = g_orders.find(a_token);
                if (it == g_orders.end()) {
                    spdlog::warn("[econ] ReportTrade: unknown token {}", a_token);
                    return;
                }
                o = std::move(it->second);
                g_orders.erase(it);
            }

            if (a_vendorGold < 0) {
                spdlog::info("[econ] token {} -- Papyrus found no usable chest (aborted)", a_token);
                return;
            }

            auto* fol = o.follower.get().get();
            auto* ven = o.vendor.get().get();
            spdlog::info("[econ] {:08X} '{}' @ '{}': chest gold={} | {} {} item(s) for {}g "
                         "(sell list n={}, purse={})",
                         fol ? fol->GetFormID() : 0u,
                         fol && fol->GetName() ? fol->GetName() : "?",
                         ven && ven->GetName() ? ven->GetName() : "?",
                         a_vendorGold,
                         o.probeOnly ? "WOULD sell" : "SOLD",
                         a_soldCount, a_soldValue, o.sell.size(), o.budget);
        }

    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            spdlog::error("[trade] RegisterFuncs: no VM -- econ bridge disabled");
            return false;
        }
        a_vm->RegisterFunction("GetTradeFollower", "MFO_Trade", GetTradeFollower);
        a_vm->RegisterFunction("GetVendorChest",   "MFO_Trade", GetVendorChest);
        a_vm->RegisterFunction("GetVendorActor",   "MFO_Trade", GetVendorActor);
        a_vm->RegisterFunction("GetProbeOnly",     "MFO_Trade", GetProbeOnly);
        a_vm->RegisterFunction("GetSellForms",     "MFO_Trade", GetSellForms);
        a_vm->RegisterFunction("GetSellCounts",    "MFO_Trade", GetSellCounts);
        a_vm->RegisterFunction("GetSellValues",    "MFO_Trade", GetSellValues);
        a_vm->RegisterFunction("ReportTrade",      "MFO_Trade", ReportTrade);
        spdlog::info("[trade] registered MFO_Trade natives (sell bridge)");
        return true;
    }

    void VendorTrade(RE::Actor* a_follower, RE::Actor* a_vendor,
                     RE::TESObjectREFR* a_chest,
                     std::vector<SellRow> a_sell, std::int32_t a_budget) {
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
            o.budget    = a_budget;
            o.probeOnly = !Config::g_economy.load();   // bEconomy off -> dry run
            g_orders[token] = std::move(o);
        }
        if (!Papyrus::DispatchTradeRun(q, token)) {
            std::scoped_lock lk(g_mtx);
            g_orders.erase(token);   // dispatch failed -> don't leak the order
            spdlog::warn("[econ] dispatch failed for token {}", token);
        }
    }

    void ClearTransientState() {
        std::scoped_lock lk(g_mtx);
        g_orders.clear();
    }

}
