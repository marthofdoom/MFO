#pragma once
#include "PCH.h"
#include <chrono>
#include <functional>
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
    // The act.loot_museum dispatch: loot museum items. True = it acted. (The deposit
    // is PriorityDeposit's since the museum-priority round, 2026-09-28.)
    bool RunGambit(RE::Actor* a_follower, Clock::time_point a_now);
    // MUSEUM DEPOSIT = TOP PRIORITY (marth 2026-09-28). The two things only the service
    // tick knows, asked LAZILY (after the cheap checks pass): does a heal rule want the
    // tick, and may a running loot excursion yield (the callback ends it through the
    // loot road's own clear path and returns true; false = it is fetching a museum item,
    // the deposit waits for it to land). Empty = "no" / "nothing to yield".
    struct DepositGate {
        std::function<bool()> healWants;
        std::function<bool()> yieldExcursion;
    };
    // Checked on EVERY service tick right after DepositTick, ahead of the excursion
    // driver and the rule loop: a follower with an enabled act.loot_museum rule who
    // carries needed relics (Shippable) and has an outgoing crate inside his leash starts
    // a deposit trip, whatever the board order or the dibs deferral. Yields only to
    // combat (his or the player's) and heals. True = a trip started (the caller returns).
    bool PriorityDeposit(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now,
                         const DepositGate& a_gate);
    // Drive this follower's deposit trip, if he is on one. True = the trip owns this
    // tick (the caller returns). Called at the top of the logistics tick. a_healWants:
    // while he is still walking, a heal rule that wants to fire ends the trip.
    bool DepositTick(RE::Actor* a_follower, Clock::time_point a_now,
                     const std::function<bool()>& a_healWants = {});
    // End this follower's trip now (combat, dismissal). Idempotent, worker road.
    void EndDeposit(RE::FormID a_follower, const char* a_why);
    // The owner-independent backstop (the pump, every lap): ends a trip whose owner is
    // no longer being serviced (logistics off, dead, unloaded, dismissed, stalled) and
    // enforces the trip's maximum. Worker road.
    void SweepTrip();
    // Economy / SwapUp: this base is a relic THIS follower covers for the museum ->
    // never sell it, never drop it (sell list, swap-up drops, the ammo ladder).
    bool HoldFromSale(RE::FormID a_follower, RE::TESBoundObject* a_base);
    // Loose-loot bar (86e3f9pkg): a_ref is a museum DISPLAY ref named by the slot table
    // (the current snapshot; false before the first snapshot or without LOTD). Any thread.
    bool IsDisplayRef(RE::FormID a_ref);
    // PlayerGiven (ClickUp 86e3faccn): may a_base be a museum RELIC -- any base the
    // slot table accepts, needed now or not (the museum's needs change; a gift is
    // protected for good)? False without LOTD. TRUE while LOTD is detected but no
    // snapshot is read yet (unknown: fail closed -- the gift is recorded). Any thread.
    bool MayBeRelic(RE::FormID a_base);
    // ShedOffRoleWeapon: HoldFromSale AND his table holds an enabled act.loot_museum
    // AND the deposit can run -> keep it for the crate, do not hand it to the player.
    bool KeepForDeposit(RE::Actor* a_follower, const FollowerState& a_state, RE::TESBoundObject* a_base);
    // ShedOffRoleWeapon (MFO-B126): the off-role relics KeepForDeposit kept in the pack
    // THIS pass and the in-role weapons he carried, REPLACING the follower's record
    // (empty kept = cleared: the shed clears it on entry, so its early returns leave
    // nothing stale). Such a relic ships even when WORN -- his own AI equipped it in an
    // unowned hand, which the shed would have dropped anyway -- but only when it is NOT
    // player-given (PlayerGiven), no MFO hold names it, and one of those in-role weapons
    // is still carried at ship time. Worker road.
    // a_keptUnworn = the kept relics seen UNWORN this pass (a subset of a_kept): added
    // to a STICKY per-follower set (pruned to what is still kept on each full pass) --
    // the positive proof PlayerGiven's AI-equip mark needs (MFO saw it unworn in his
    // pack as a kept relic before anything equipped it).
    void NoteKeptForDeposit(RE::FormID a_follower, std::vector<RE::FormID> a_kept,
                            std::vector<RE::FormID> a_inRole, std::vector<RE::FormID> a_keptUnworn = {});
    // Was this base seen UNWORN in his pack as a kept relic (since it was last not
    // kept)? Worker road (PlayerGiven's queued equip body).
    bool SeenUnwornKept(RE::FormID a_follower, RE::FormID a_base);
}
