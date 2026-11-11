#pragma once
#include "PCH.h"

// STEALTH PROBE P1-P3 (card G2, Docs/research/2026-11/g2-hide-design.md section 3). FULLY PASSIVE: no claim, no animation,
// no actor-value or engine write, no behaviour reads its output. Gated by [Debug] bStealthProbe (INI-only, default 0) and
// rate-limited (1 line a second per follower / per (foe, follower) pair, at most 20 hit lines a minute per follower, a
// "N suppressed" tail on the next line and a closing line when the window ends).
//   P1 "self":  the follower's detection inputs (stance, moving bits, Sneak / MovementNoiseMult / Invisibility AVs) and the perk
//               entry values 57 / 48 / 47 / 15 / 18 (HandleEntryPoint, MAIN THREAD via Post, row-verified like iaus/Fills.cpp).
//   P2 "seen":  for a sneaking follower, each engaged foe's combat-group record of him (iaus/Hide.h), MFO's SenseActor verdict
//               and the formula-predicted score (calculate-detection-re.md section 4, light and equipped weight unknown).
//   P3 "hit" + "q87": every TESHitEvent a follower causes: the kSneakAttack flag, the expected multiplier (the weapon's
//               fCombatSneak*Mult by name x entry 18) and the target's health before / after, with a per-weapon-type sneak vs plain
//               aggregate for the Q87 acceptance rule. NOT P4 (the light probe needs a new verified address).
// THREADS: Service runs on the worker (the Scheduler tick, beside TauntProbe); the hit sink runs on whatever thread the
// engine posts TESHitEvent from and only copies a POD record into a leaf-mutex ring; entry points, SenseActor and the final
// log lines run on the main thread (MainThread::Post); the q87 aggregate is main-thread work behind its own leaf mutex.
namespace MFO::StealthProbe {

    // One call per serviced follower per lap, from the combat scan (scheduler/CombatScan.cpp). Returns at once when the gate is off.
    void Service(RE::Actor* a_f, RE::FormID a_id, bool a_ownCombat);

    // kDataLoaded: register the passive hit sink (inert while bStealthProbe = 0).
    void RegisterHitSink();

    // Revert / load: drop every sample, pending hit and aggregate (session state).
    void Reset();

}
