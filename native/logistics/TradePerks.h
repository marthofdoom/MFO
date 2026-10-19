#pragma once
// logistics/TradePerks.h -- GUARDED ENGINE QUERIES for town phase 2 (round S, ClickUp 86e3tambj).
// Design: _evidence/2026-10-18-designs/TOWN-PHASE2-DESIGN.md, round S and "R0 RESULTS (2026-10-19)".
//
// Thin wrappers over three engine functions that phase 2 calls by Address Library id. Each one
// checks its self-check row first (native/VerifiedAddresses.h, rows "TradePerks.*", the mit-3.7
// F1 gate Runtime::SeatVerified) and REFUSES when the row did not verify: the refusal is logged
// loudly ONCE, by seat name, and every later call returns std::nullopt. There is no fallback
// value: a refused query is "no answer", never the seed and never "false" (CLAUDE.md principle 7).
//
// No price logic, no cache and no behaviour live here. Round R2 owns the prices and the cache.
//
// THREADING CONTRACT (read before calling):
//   EntryPointValue  MAIN THREAD ONLY. The entry-point conditions read live actor and world state
//                    with no lock (MFO's existing lockpick query has the same rule, Progression.h).
//                    The owner must be LOADED with a middle-high process: on an actor with no
//                    process, no middle-high data or no perk data the engine silently leaves the
//                    seed (R0-4, disassembly on both runtimes), so the wrapper returns nullopt
//                    there instead of the seed. The caller evaluates while the follower is loaded
//                    (plan time) and CACHES the result for off-screen and VM-thread use.
//   ConditionTrue    MAIN THREAD ONLY. The evaluator swaps its context in thread-local storage,
//                    but the condition functions read live state with no lock.
//   AliasRefHandle   Any thread that may touch the quest. The engine takes the quest's
//                    aliasAccessLock (read) itself. NEVER call it while holding aliasAccessLock:
//                    BSReadWriteLock is not recursive, so that deadlocks. Today's callers
//                    (Packages.cpp) are on the main thread.
// The seat checks themselves are thread-safe (a function-local static over the self-check result).
#include "PCH.h"

#include <cstdint>
#include <optional>

namespace MFO::Logistics::TradePerks {

    // BGSEntryPoint::HandleEntryPoint (1.6.1170 id 23526, 1.5.97 id 23073). The engine signature is
    // C-variadic: (u32 ep, Actor* owner, condArg..., float* value). The number of condition
    // arguments is fixed per entry point by the engine's own table, so the wrapper accepts only
    // the entry points whose shape is PROVEN (R0-1, plus the lockpick shapes LP-M1 ships):
    //   one condition argument:  8 kModBuyPrices (Target = the vendor REFR),
    //                           60 kModSellPrices (Target = the vendor REFR),
    //                           76 kModTemperingHealth (Item = the TESBoundObject),
    //                           59 kModLockpickSweetSpot (Locked Ref)
    //   no condition argument:  63 kSetLockpickStartingArc, 65 kMakeLockpicksUnbreakable
    // Any other entry point, or the wrong overload for one of these, is refused (nullopt + an
    // error line): a wrong argument count makes the engine read a condition pointer as the value
    // pointer. A null condition argument is refused too: the engine never null-checks condition
    // arguments and no caller of it passes null (probe P-PRICE-1 has not run).
    // a_seed is the value the engine starts from. It is the CALLER's: the engine's barter seeds
    // the Speech factor, its smithing menu seeds the improvement delta, never a blanket 1.0.
    // Returns the value after every applicable perk entry ran, or nullopt (refused, null owner,
    // owner not evaluable).
    std::optional<float> EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner, float a_seed);
    std::optional<float> EntryPointValue(RE::BGSEntryPoint::ENTRY_POINT a_ep, RE::Actor* a_owner,
                                         RE::TESForm* a_arg, float a_seed);

    // TESCondition::IsTrue(action, target) (1.6.1170 id 29888, 1.5.97 id 29074). An empty
    // condition list is true, as in the engine. nullopt = the seat was refused.
    std::optional<bool> ConditionTrue(const RE::TESCondition& a_cond, RE::TESObjectREFR* a_action,
                                      RE::TESObjectREFR* a_target);

    // TESQuest::CreateRefHandleByAliasID (1.6.1170 id 25066, 1.5.97 id 24537), the engine's own
    // reference-alias reader. Returns the alias's handle (empty when the alias is empty), or
    // nullopt when the seat was refused or a_quest is null. A refused reader means "occupancy
    // unknown", which the caller must not treat as "empty" without saying so.
    std::optional<RE::ObjectRefHandle> AliasRefHandle(RE::TESQuest* a_quest, std::uint32_t a_aliasID);

    // True when the AliasRefHandle seat verified (no log of its own beyond the seat's one refusal
    // line). For a caller's diagnostic text.
    bool AliasReaderVerified();
}
