#pragma once
#include "PCH.h"

// LINE-OF-SIGHT, off the worker thread. Task: "he casts through walls."
//
// PickFoe gates on dead/disabled/kTargetLost/brawl/chase-cap and NOTHING
// geometric, so a hostile behind a wall within chase range gets selected and a
// forced package cast fires a bolt into the masonry. The engine's own check is
// `Actor::HasLineOfSight` (RELOCATION_ID 53029/53829, verified present at the
// pinned CommonLibSSE-NG v3.7.0, include/RE/A/Actor.h:563) -- but that is a
// physics/pick query, and the evaluator runs on a BSJobs JOB WORKER
// (INVARIANTS #72, [[skse-addtask-runs-on-job-worker]]). A worker-thread
// raycast is the §0.30 crash class, full stop.
//
// So this module SPLITS the query across the threads that can each half of it:
//
//   worker  --Want()-->  MainThread::Post  --HasLineOfSight-->  cache
//   worker  --Check()------------------------------------------^ (read)
//
// The raycast runs ONLY inside the player's per-frame Update (MainThread.cpp,
// §0.37 -- the one proven road to main), writes a verdict into a mutex'd
// cache, and the worker reads the cache. The verdict the evaluator sees is
// therefore up to ~a tick stale, which is fine: walls do not move, and a foe
// crossing a doorway flips the verdict on the next pump.
//
// TWO BASES (2026-09-30, marth: "any spell cast using harbinger gets our los
// check ... should also apply to bows"). The engine's HasLineOfSight, for an NPC
// viewer, does NOT raycast: it returns a per-(viewer, target) bool cached in the
// AIProcess, refreshed on the engine's own schedule, so it can be stale or false
// whatever the geometry (it flickered a heal target OCCLUDED/VISIBLE about once a
// second, and never served heals at the player). So the CALLER names the basis:
//   Basis::Engine (the default, today's behaviour): engine HasLineOfSight first;
//       MFO's own bhkWorld ray (kCharController, feet/torso/head, any clear
//       sample => visible) only when the engine says CLEAR, to catch tent cloth
//       etc. It can only turn VISIBLE into OCCLUDED and fails open.
//       Melee-only foe checks and every non-spell, non-ranged use.
//   Basis::Own: EVERY measurement serving an MFO SPELL decision (any target, any
//       road) or a BOW/ranged decision. Our ray ALONE is the verdict; the engine
//       cache does not decide. If the ray cannot run (no cell / bhkWorld /
//       controller) that one measurement uses the engine's answer and logs
//       "[los] ... own ray unavailable (<why>) -- engine cache used".
// Each (viewer, target) pair keeps a SEPARATE slot per basis (verdict, stamp,
// occRun), and the Want() throttle is per (pair, basis), so a melee reader never
// sees an own-ray verdict and a spell reader never sees the engine's bool.
// Enemy NPCs' own sight is never touched.
//
// FAIL-OPEN BY DESIGN. Unknown (cold cache, unloaded 3D, VR where the
// main-thread pump refuses to install) must degrade to TODAY'S behaviour --
// LoS is a preference/hold, never a wall that silently disables targeting or
// makes the forced cast inert on a runtime where the raycast cannot run.

namespace MFO::Sightline {

    // Which verdict a call reads/measures. Chosen by the CALLER, never inferred.
    enum class Basis : std::uint8_t {
        Engine,   // engine HasLineOfSight first, own ray only to catch an engine-CLEAR (today)
        Own,      // spells and bows: our ray alone (engine only if the ray cannot run)
    };

    enum class Verdict : std::uint8_t {
        Unknown,    // never measured, stale, or unmeasurable -- treat as today
        Visible,    // the raycast said yes, recently
        Occluded,   // the raycast said no, recently
    };

    const char* VerdictName(Verdict a_v);

    // The cached verdict for viewer -> target. WORKER-SAFE: a locked map read,
    // no engine call. Stale entries (older than the freshness window) read as
    // Unknown rather than lying about a wall that may have stopped mattering.
    Verdict Check(RE::FormID a_viewer, RE::FormID a_target, Basis a_basis = Basis::Engine);

    // Ask the main thread to (re)measure viewer -> each target. Callable from
    // any thread; rate-limited per (viewer, target) pair (0.3 s, per pair since
    // 2026-09-29 so one caller's batch never drops another's) so the evaluator's
    // per-rule calls at 7.5 Hz cannot flood the frame queue. The results land in the cache a
    // frame later -- callers read Check(), never a return value.
    void Want(RE::FormID a_viewer, std::vector<RE::FormID> a_targets, Basis a_basis = Basis::Engine);

    // Check() with a caller-chosen trust window instead of kFreshSeconds: the
    // last measured verdict if it is younger than a_maxAgeSeconds, else Unknown.
    // For a caller that must not read an OLD Occluded as Unknown-passes merely
    // because its own re-measure is one service period out (PickAlly's heal
    // pick, field 2026-09-29). Any thread, same leaf lock as Check().
    Verdict CheckWithin(RE::FormID a_viewer, RE::FormID a_target, float a_maxAgeSeconds,
                        Basis a_basis = Basis::Engine);

    // How many Occluded measurements in a ROW the last verdict is, 0 when the last
    // verdict is Visible, absent, or older than a_maxAgeSeconds. Any thread, same
    // leaf lock. For a caller that must not act on a single Occluded reading of a
    // borderline line (the heal recipient check: two agreeing readings).
    int OccludedRun(RE::FormID a_viewer, RE::FormID a_target, float a_maxAgeSeconds,
                    Basis a_basis = Basis::Engine);

    // MAIN THREAD ONLY. Measure viewer -> target NOW -- the same Measure
    // that Want() posts (per a_basis; see the file header) --
    // write the cache, and return the verdict (Check() right after). For a caller
    // that is ALREADY running on the main thread inside a MainThread::Post (the
    // engage-on-sight probe, EngageOnSight.cpp), where a Want() would only queue
    // the measurement another frame out. Not throttled: the caller bounds its own
    // cost. Returns Unknown on VR (HasLineOfSight has no VR id; the pump never
    // runs there anyway) and when either actor is unresolvable, dead or unloaded.
    Verdict MeasureNow(RE::FormID a_viewer, RE::FormID a_target, Basis a_basis = Basis::Engine);

    // LINE OF FIRE (concentration streams). True when the PLAYER or any
    // active teammate -- other than the caster and the intended target --
    // stands within ~a body's width of the caster->target segment. Pure
    // GEOMETRY over the maintained party list (Followers::g_active + the
    // player singleton): no raycast, no main-thread hop, so unlike Check()
    // it answers synchronously and is callable straight from the worker
    // tick -- both as the pre-dispatch gate on a hostile stream and as the
    // mid-stream watch that cuts the beam when an ally walks into it.
    // Unresolvable ids fail OPEN (false): this is a hold on friendly fire,
    // never a wall that silently disables the cast rule.
    bool TeammateInFireLine(RE::FormID a_caster, RE::FormID a_target);

    // Session teardown, same shape as every sibling module: the cache keys are
    // this-session FormID pairs and must not survive a revert.
    void ClearTransientState();

}
