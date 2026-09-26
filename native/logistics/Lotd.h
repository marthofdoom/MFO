#pragma once
#include "PCH.h"
#include <chrono>
#include "State.h"   // FollowerState (KeepForDeposit reads his gambit table)

// logistics/Lotd.h -- LOTD AWARENESS (feat/mfo-lotd, ClickUp 86e3edghj, rounds L1-L3).
// Legacy of the Dragonborn has no DLL: its whole interface is data + Papyrus. MFO
// reads it natively and acts on it through its own loot machinery and Harbinger.
//
//   L1  DETECTION + TOGGLE + SNAPSHOT. Detect LegacyoftheDragonborn.esm (6.x) at
//       kDataLoaded; write MFO_LOTDDetected (MCM hiddenToggle); read the museum's
//       slot table ONCE per rebuild from the DBM_MuseumAPI script object on the MAIN
//       thread (post-load / new game / LOTD's own SKSE ModEvents) into an immutable
//       snapshot the worker reads. Passive [lotd] log lines say what it saw.
//   L2  THE GAMBIT "Loot museum items" (act.loot_museum, Category::Museum): takes the
//       relics the museum still needs; a relic a follower is covering never sells.
//   L3  THE DEPOSIT (automatic with the gambit): a follower carrying needed relics
//       near an enabled Museum Shipments crate walks there (Harbinger ch.19), plays
//       IdleGive at it (ch.12 v2, held still by ch.1), and the items move into the
//       crate on the MAIN thread (after the crate was activated, trap (a)).
//
// INERT unless LOTD is detected AND bLootLOTD is on. The gambit (loot + deposit) is
// additionally inert without Harbinger ABI v17 (logged once). NOTHING IS SAVED: the
// snapshot, the needs cache, the in-transit ledger and the trip are session state,
// cleared on revert (Logistics::ClearTransientState).
namespace MFO::Lotd {

    using Clock = std::chrono::steady_clock;

    // ── lifecycle ────────────────────────────────────────────────────────────
    void Detect();                     // kDataLoaded, main thread, once
    void RegisterSinks();              // kDataLoaded, main thread, once (ModCallback sink)
    void OnPostLoad(bool a_newGame);   // kPostLoadGame / kNewGame, main thread
    void OnConfigRead();               // worker, after the MCM-close Config::Read
    void ClearTransientState();        // revert (pump drained)

    // ── state (atomics only: safe from the render thread) ───────────────────
    bool Detected();                   // LOTD 6.x present and its anchors resolved
    bool Enabled();                    // Detected() && bLootLOTD
    bool GambitOffered();              // the board lists "Loot museum items" (== Enabled())

    // ── worker road (the per-follower service tick) ──────────────────────────
    // The act.loot_museum dispatch: start a deposit trip when this follower carries
    // needed relics and a crate is near, else loot museum items. True = it acted.
    bool RunGambit(RE::Actor* a_follower, Clock::time_point a_now);
    // Drive this follower's deposit trip, if he is on one. True = the trip owns this
    // tick (the caller returns). Called at the top of the logistics tick.
    bool DepositTick(RE::Actor* a_follower, Clock::time_point a_now);
    // End this follower's trip now (combat, dismissal). Idempotent, worker road.
    void EndDeposit(RE::FormID a_follower, const char* a_why);
    // The owner-independent backstop (the pump, every lap): ends a trip whose owner is
    // no longer being serviced (logistics off, dead, unloaded, dismissed, stalled) and
    // enforces the trip's maximum. Worker road.
    void SweepTrip();
    // Economy / SwapUp: this base is a relic THIS follower covers for the museum ->
    // never sell it, never drop it (sell list, swap-up drops, the ammo ladder).
    bool HoldFromSale(RE::FormID a_follower, RE::TESBoundObject* a_base);
    // ShedOffRoleWeapon: HoldFromSale AND his table holds an enabled act.loot_museum
    // AND the deposit can run -> keep it for the crate, do not hand it to the player.
    bool KeepForDeposit(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_base);
}
