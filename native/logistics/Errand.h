#pragma once
#include "PCH.h"
#include <chrono>
#include <functional>
#include "State.h"   // FollowerState

// logistics/Errand.h -- TOWN ERRANDS, round R1a (ClickUp 86e3t1mx7; design: _evidence/2026-10-18-designs/
// TOWN-DESIGN.md). With Harbinger (ch.19 travel + ch.12 idle v2 + GetIdleState, ABI >= 24) present, the
// follower no longer trades INSTANTLY with any vendor near him (the old EconomyProbe proximity scan): when the
// PLAYER is in a City / Town / Settlement and the follower has a reason, he WALKS to an open shop's vendor
// (ch.19), plays talk / give / take idles AT him (ch.1 hold + ch.12 v2, the Deposit pattern), trades through
// the unchanged TradeBridge::VendorTrade, and rejoins (every claim released; his follow package walks him
// back). Harbinger absent (or bTownErrands off) = the legacy proximity trade, unchanged.
//
// R1a SCOPE: the vendor is a LOADED actor (exterior stalls, the forge, or the player inside the shop). The
// catalog reads each candidate's vendor FACTION (hours, fence flag, conditions, VEND list, chest) but takes the
// PLACE from the loaded vendor actor, not from the faction's PLVD location (its in-memory union is unverified;
// see the seams below). No off-screen simulation: a follower who unloads ends the errand.
//
// SEAMS for R1b / R2 (do not build here): the state machine's Phase enum + Errand record (Errand.cpp) are
// where OUT_SIM / TRADE_SIM / RET_SIM / RECALL and the entryDoor attach; Sweep() is the owner-independent
// tick the simulation clock rides; CatalogEntry::place is where the faction's PLVD cell / ref goes once its
// layout is verified; a PLAN-time stored TradeOrder replaces the fresh EconomyProbe(kExecute) call; the
// ERND co-save record serialises the Errand record.
//
// THREADING: every entry point runs on the AddTask job worker (the per-follower service tick, the pump's
// Sweep) except ClearTransientState (revert, pump drained). One mutex guards the table; it is never held
// across a Harbinger or TradeBridge call.
namespace MFO::Logistics::Errand {

    using Clock = std::chrono::steady_clock;

    // The idle branch of ServiceFollower, in place of EconomyProbe. TRUE = errands own the economy for this
    // follower (the caller must NOT run the legacy proximity trade); false = legacy applies (bTownErrands
    // off, or Harbinger absent / too old).
    bool TryStart(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now);
    // Top of the logistics tick: drive this follower's errand. TRUE = the errand owns this tick.
    bool Tick(RE::Actor* a_follower, const FollowerState& a_state, Clock::time_point a_now,
              const std::function<bool()>& a_healWants = {});
    // The owner-independent backstop (the pump, every lap): ends an errand whose follower is dead, unloaded,
    // in combat, dismissed, no longer serviced, or whose player left the town; enforces the caps.
    void Sweep();
    // End one follower's errand now (idempotent).
    void End(RE::FormID a_follower, const char* a_why);
    // Is this follower on an errand? (board / logging)
    bool Active(RE::FormID a_follower);
    // Revert / load (pump drained): drop every errand and its claims.
    void ClearTransientState();
}
