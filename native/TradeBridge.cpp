#include "PCH.h"
#include "TradeBridge.h"
#include "Forms.h"
#include "Papyrus.h"

namespace MFO::TradeBridge {

    namespace {

        // Papyrus native: MFO_Trade.NativePing(token). Its only job in Phase 0 is
        // to prove a registered native returns to the VM SYNCHRONOUSLY and that
        // the dispatched RunTrade actually ran on a bound MFO_Trade instance. In
        // Phase 1 this is replaced by the real GetSellForms/GetVendorChest/...
        // pull accessors keyed by the same token.
        void NativePing(RE::StaticFunctionTag*, std::int32_t a_token) {
            spdlog::info("[trade] NativePing token={} -- bridge round-trip OK "
                         "(DLL -> RunTrade -> native)", a_token);
        }

    }

    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm) {
        if (!a_vm) {
            spdlog::error("[trade] RegisterFuncs: no VM -- econ bridge disabled");
            return false;
        }
        a_vm->RegisterFunction("NativePing", "MFO_Trade", NativePing);
        spdlog::info("[trade] registered MFO_Trade natives (NativePing)");
        return true;
    }

    void SelfTest() {
        static std::atomic<bool> s_done{ false };
        if (s_done.load()) return;

        auto* q = Forms::g_tradeQuest;
        if (!q) return;   // MFO_TradeQuest unresolved -> already logged by Forms; feature dark.

        // The bound MFO_Trade instance only exists once the quest is running.
        // On an existing save that predates the quest, the SEQ starts it -- but
        // not necessarily by the first tick, so retry (don't latch) until then.
        if (!q->IsRunning()) return;

        const std::int32_t token = 1;
        spdlog::info("[trade] self-test: dispatching RunTrade({}) to MFO_TradeQuest {:08X}",
                     token, q->GetFormID());
        if (Papyrus::DispatchTradeRun(q, token)) {
            s_done = true;   // dispatched -- the NativePing line confirms the far side
        }
    }

}
