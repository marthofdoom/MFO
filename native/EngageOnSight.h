#pragma once
#include "PCH.h"

// ENGAGE ON SIGHT -- the hidden, built-in out-of-combat gambit "nearest visible
// enemy" (ClickUp 86e3errnu, 2026-09-25; MFO's adoption of Harbinger ch.21).
//
// Not a Gambit record: it is not in the board's list and cannot be edited. The
// MCM toggle bEngageOnSight (default OFF) is its only control. While the PARTY
// is out of combat (looting and other logistics included), a follower who SEES
// an enemy -- MFO's Sightline two-stage measurement says VISIBLE -- inside the
// leash (Confidence::LeashRadius, measured from the PLAYER) enters combat
// against the NEAREST one (nearest to him) through Harbinger ch.21
// (kIntent_CombatEntry) and pins the same target through Targeting (ch.20 when
// Harbinger serves it; it waits until the target is a combat-group member).
// Once he is fighting, his own foe gambits pick targets as they always do.
//
// ENEMY (EngageOnSight.cpp IsEnemy): a loaded, enabled, living actor that is not
// the player or a player teammate, not commanded by the player or a teammate, not
// restrained, bleeding out or on an IgnoreCombat package, and hostile to the player
// OR to him (Actor::IsHostileToActor: the engine's "would attack" read, which alone
// names guards and townsfolk once the player has a bounty). An actor with a CRIME
// FACTION counts only when it is already fighting the party. This is NOT the combat
// foe test (that one reads his combat group's targets); out of combat there is no
// group, so the scan needs its own filters.
//
// NO MFO DIRECT ROAD. Harbinger absent, older than ABI 14, or its ch.21 seat
// refused = the gambit is inert (logged per follower on transition).
// It also stands down while: bAutoRetreat is OFF (a follower who starts a fight
// must be able to break it off), the player sneaks (unless
// bEngageOnSightSneaking), he is told to wait (WaitingForPlayer, or a sandbox
// package), his auto-retreat cooldown runs, or his in-combat confidence estimate
// (Confidence::OfFacing against the candidate count) is under the retreat floor.
//
// NO LOOP. When Harbinger ends an entry (engine refused, combat ended, target
// gone), that target is GIVEN UP for this follower until a probe posted after
// the end reports it GONE: dead, disabled, unloaded, no longer hostile, or past
// the FIXED fLeashMax from the player (never the confidence-scaled leash). The
// engine giving up is final for as long as the foe stays where it was.
//
// THREADS. Service/Forget/ClearTransientState run on the AddTask job worker
// (the Scheduler's per-follower service). The actor scan (highActorHandles) and
// the sightline measurement run on the MAIN thread inside one MainThread::Post
// per follower service (ENGINE_NOTES §0.30 / §0.47; the retreat foe probe's
// pattern), and hand back FormIDs through a mutex'd mirror. No combat-thread
// code reads any of it.
namespace MFO::EngageOnSight {

    // Worker. Called from the Scheduler's party-OOC branch AFTER ServiceRetreat
    // returned false and BEFORE Logistics::ServiceFollower. a_retreatCooling =
    // his auto-retreat cooldown is still running; a_minConfidence = the retreat
    // floor (Scheduler's kRetreatConfidence). Returns true when this lap is the
    // gambit's (an entry was filed now, or one is standing and he is not yet
    // fighting): the caller then skips logistics this lap.
    bool Service(RE::Actor* a_follower, RE::FormID a_id, bool a_retreatCooling, float a_minConfidence);

    // Worker. Dismissal / MFO-off: release his entry claim (Release stops no
    // fight) and forget his per-follower state.
    void Forget(RE::FormID a_id);

    // Worker, inside the StopPump bracket (Scheduler::ClearTransientState).
    void ClearTransientState();

}
