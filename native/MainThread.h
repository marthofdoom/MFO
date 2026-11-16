#pragma once
#include "PCH.h"
#include <functional>   // std::function -- not in the PCH

// THE MAIN-THREAD PUMP. ENGINE_NOTES §0.37.
//
// MFO's logistics/diagnostics tick does NOT run on the main thread, and neither
// does anything queued with SKSE::GetTaskInterface()->AddTask: in this runtime
// that queue drains inside Job_Post_process on BSJobs::JobThread (§0.32's
// crash4 proved it first; two vendor-read crash stacks on 2026-07-31 proved it
// again). So AddTask means "run soon on a WORKER", never "run on main"
// [[skse-addtask-runs-on-job-worker]].
//
// This is the real road to main: a queue drained from inside the PLAYER's
// per-frame Update vfunc. The player is updated exactly once per frame, on the
// main thread -- so everything Post()ed here runs on the main thread, next
// frame, in post order. Anything that must touch live engine state a worker
// cannot safely read (a live merchant's inventory, 3D teardown, cell mutation)
// goes through here.

namespace MFO::MainThread {

    // Enqueue fn to run on the MAIN thread during the player's next Update.
    // Callable from any thread. Safe to call before Install() -- fns queue up
    // and drain once the hook is live. A posted fn may Post() again (the drain
    // runs outside the queue lock); the repost lands next frame, not this one.
    void Post(std::function<void()> a_fn);

    // Install the player-Update vfunc hook. Once, at kDataLoaded, exactly like
    // Targeting::InstallHook. Not installed on VR (unverified vtable index) --
    // in that case Post() becomes a documented no-op rather than a leak.
    void Install();

    // True once the pump is LIVE (hook installed and not the VR no-op path), so a
    // Post() will actually run. A caller whose work MUST still happen on VR -- where
    // Post is a documented no-op -- checks this and falls back to a direct call.
    bool IsInstalled();

    // ── WHICH THREAD IS THIS (INVARIANTS #84: the worker never walks an engine inventory or active-effect list) ──
    // InSeat: this thread is inside the player-Update thunk right now (the engine's update or the drain).
    // IsMainThread: this thread is the one the thunk last ran on. False until the first frame has run.
    // InWorkerTick: this thread is inside MFO's AddTask tick body (WorkerTickScope), whichever OS thread the job system gave it.
    // LiveReadsAllowed: may THIS call read a live engine inventory / effect list? True in the seat, on the main thread outside a
    // worker tick (event sinks, load handlers: as before), and when the pump is not installed (the documented legacy path).
    // logistics/InvSnapshot and iaus/EffectMirror branch on it, so one helper serves both threads.
    bool InSeat();
    bool IsMainThread();
    bool InWorkerTick();
    bool LiveReadsAllowed();
    // THE INVARIANT CHECKS ITSELF (#84, principle 5). Every place that still reads a LIVE engine inventory or active-effect list
    // names itself here, on the line before the read:
    //   NoteLiveRead   a read that must run in the seat or on the main thread outside a worker tick. Called anywhere else (inside the
    //                  worker tick, or on a thread that is not the main one) it is a VIOLATION: counted, and said ONCE per site as an
    //                  `[thread]` error. Passive: the read still runs (the engine call is the caller's), nothing is masked.
    //   NoteWriteRead  the read that belongs to an engine WRITE made on the same thread right after it (it needs the live
    //                  ExtraDataList). On the worker that is the known residue stability audit M2 moves to the main thread with its
    //                  write: counted, never an error. Zero of these is M2's finish line.
    // Diagnostics.cpp prints both counts in the `[overlap]` line.
    void NoteLiveRead(const char* a_site);
    void NoteWriteRead(const char* a_site);
    std::uint64_t LiveReadViolations();
    std::uint64_t WorkerWriteReads();
    struct WorkerTickScope { WorkerTickScope(); ~WorkerTickScope(); WorkerTickScope(const WorkerTickScope&) = delete; WorkerTickScope& operator=(const WorkerTickScope&) = delete; };

    // ── P0 OVERLAP PROBE (passive, always on: a few relaxed atomics per frame and per tick; Diagnostics.cpp prints `[overlap]`) ──
    // SeatSeq is odd while the main thread is inside the thunk and moves at its entry and exit; SeatPhase is 1 in the engine's own
    // player Update, 2 in MFO's drain, 0 outside. NpcCombat*: the same for Character::UpdateCombat (Targeting.cpp's hook wraps the
    // original in NpcCombatScope). SeatSawWorker counts thunk entries / exits that found a worker tick in flight.
    std::uint32_t SeatSeq();
    int           SeatPhase();
    std::uint32_t SeatSawWorker();
    std::uint32_t NpcCombatSeq();
    int           NpcCombatIn();
    struct NpcCombatScope { NpcCombatScope(); ~NpcCombatScope(); NpcCombatScope(const NpcCombatScope&) = delete; NpcCombatScope& operator=(const NpcCombatScope&) = delete; };

    // Drop every queued closure without running it. Called on revert: a fn posted
    // before a load re-resolves its captured handle against the NEXT session's
    // handle table (handles are reused), so draining it post-load could run on the
    // wrong actor. The hook stays installed; only the pending work is discarded.
    void Clear();
    // Bumped by every Clear(). A poster that keeps "my closure is queued" state compares it, so a Clear that dropped the closure
    // cannot leave that state stuck (Diagnostics.cpp's snapshot build pass).
    std::uint32_t ClearGeneration();

}
