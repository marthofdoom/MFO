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
// FAIL-OPEN BY DESIGN. Unknown (cold cache, unloaded 3D, VR where the
// main-thread pump refuses to install) must degrade to TODAY'S behaviour --
// LoS is a preference/hold, never a wall that silently disables targeting or
// makes the forced cast inert on a runtime where the raycast cannot run.

namespace MFO::Sightline {

    enum class Verdict : std::uint8_t {
        Unknown,    // never measured, stale, or unmeasurable -- treat as today
        Visible,    // the raycast said yes, recently
        Occluded,   // the raycast said no, recently
    };

    const char* VerdictName(Verdict a_v);

    // The cached verdict for viewer -> target. WORKER-SAFE: a locked map read,
    // no engine call. Stale entries (older than the freshness window) read as
    // Unknown rather than lying about a wall that may have stopped mattering.
    Verdict Check(RE::FormID a_viewer, RE::FormID a_target);

    // Ask the main thread to (re)measure viewer -> each target. Callable from
    // any thread; rate-limited per viewer so the evaluator's per-rule calls at
    // 7.5 Hz cannot flood the frame queue. The results land in the cache a
    // frame later -- callers read Check(), never a return value.
    void Want(RE::FormID a_viewer, std::vector<RE::FormID> a_targets);

    // Session teardown, same shape as every sibling module: the cache keys are
    // this-session FormID pairs and must not survive a revert.
    void ClearTransientState();

}
