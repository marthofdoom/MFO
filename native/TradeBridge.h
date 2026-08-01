#pragma once
#include "PCH.h"

// #21 ECON BRIDGE -- native side (Fable's ECON_PAPYRUS_PLAN).
//
// The merchant read + transaction cannot run in C++: native
// TESObjectREFR::GetInventory() on an unpopulated merchant chest CTDs on any
// thread (memory economy-vendor-detection-excludes-teammates, ENGINE_NOTES
// §0.37). So the merchant work moves to Papyrus (MFO_Trade.psc), the path the
// game's own barter menu uses -- and native keeps the DECISION (catalog, gambit
// quotas, quality ranking).
//
// This module is the native half of the bridge:
//   * the Papyrus natives MFO_Trade pulls its TradeOrder through (registered on
//     class "MFO_Trade", returned SYNCHRONOUSLY to the calling Papyrus frame --
//     unlike Papyrus::Dispatch*, which is fire-and-forget/async);
//   * (Phase 0) a self-test that proves the whole round trip end to end before
//     any merchant is touched: DLL -> RunTrade(token) -> NativePing(token) -> log.
namespace MFO::TradeBridge {

    // Register MFO_Trade's native functions on the VM. Wired once at load via
    // SKSE::GetPapyrusInterface()->Register(RegisterFuncs). Returns false if the
    // VM is unavailable (feature simply stays dark -- never a crash).
    bool RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm);

    // Phase 0 bridge proof: dispatch RunTrade(token) at MFO_TradeQuest ONCE, the
    // first time the quest is resolved and running. Safe to call every tick; it
    // latches only on a successful dispatch, and no-ops forever after. Removed
    // when Phase 1 wires the real vendor trigger.
    void SelfTest();

}
