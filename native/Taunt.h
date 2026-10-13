#pragma once
#include "PCH.h"
#include "Evaluator.h"
#include "cast/Actuation.h"

// TAUNT (slice S2 of Docs/TAUNT-DESIGN.md, ClickUp 86e3g6duw): cond.foe_attacking_ally + act.taunt and the executor.
//
// act.taunt is MELEE-ONLY (a melee weapon in the right hand). One firing is ONE ATTEMPT: the follower plays the
// combat taunt idle through Harbinger ch.12 (ch.1 holds him still), and only when that idle is CONFIRMED played the
// chosen foe and up to three more nearby (in combat with the party, within fTauntRadius, able to sense him) each
// roll p = clamp(0.5 + 0.05 x (taunter level - foe level), 0.1, 0.9). A hit pins THAT FOE onto the taunter with a
// Harbinger ch.20 pin (plus a ch.21 entry only when the taunter is not in the foe's combat group), held for
// fTauntDuration and released early on the taunter downed or dead, the foe dead or unloaded, combat ending, a
// load, or Harbinger ending the hold itself. No cooldown, no co-save (all state is transient). The claims live in
// apmf/TauntClaims.cpp; this file owns the decisions.
//
// THREADING (INVARIANTS #4). Fire and Tick run on the AddTask job worker (the Scheduler tick). The roll-and-pin step
// runs on the MAIN thread (MainThread::Post), because Harbinger's SenseActor is main-thread only. The two share
// the tables below behind one mutex (g_mx, taken before the bridge's own leaf mutex, never the reverse).
namespace MFO::Taunt {

    // Is a_actor the player, a player teammate, or a summon / reanimated actor one of them commands?
    // cond.foe_attacking_ally's party test and the executor's "in combat with the party" test.
    [[nodiscard]] bool IsPartyMember(RE::Actor* a_actor);

    // The act.taunt offer gate: a melee weapon (one-handed sword, dagger, axe, mace or a two-hander) in the RIGHT
    // hand. Reads the actor; the board calls it on the main thread when it builds a row.
    [[nodiscard]] bool MeleeInRightHand(RE::Actor* a_actor);

    // Actuation::Fire's act.taunt road (cast/Fire.cpp). Worker. Transparent on every refusal so the combat scan falls
    // through; opaque "in progress" while an attempt waits on its idle.
    Actuation::Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice);

    // Once per Scheduler pump, worker, BEFORE the early returns: advances every attempt (idle confirmation) and
    // every active taunt (duration and the early ends). Pauses with the game.
    void Tick();

    // Revert / load: log and drop every attempt and taunt, release every claim.
    void Clear();

}
