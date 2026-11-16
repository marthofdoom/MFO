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
    // IsMainThread: this thread is the main thread (NoteMainThread at SKSEPluginLoad / kDataLoaded, then the thunk every frame).
    // InWorkerTick: this thread is inside MFO's AddTask tick body (WorkerTickScope), whichever OS thread the job system gave it.
    // LiveReadsAllowed: may THIS call read a live engine inventory / effect list? Never inside a worker tick (checked first: the
    // job system may run that task on any thread). Else true in the seat, on the main thread (event sinks, load handlers: as
    // before), and when the pump is not installed (the documented legacy path).
    // logistics/InvSnapshot and iaus/EffectMirror branch on it, so one helper serves both threads.
    bool InSeat();
    bool IsMainThread();
    // Record THIS thread as the main thread. Called at SKSEPluginLoad and at kDataLoaded (Install), both on the main thread, so
    // IsMainThread() / EngineWritesAllowed() answer correctly BEFORE the first player Update (review of ca0465c9, F2: the first
    // kPostLoadGame runs before PlayerCharacter::Update has ever run). The thunk keeps storing it every frame, as before.
    void NoteMainThread();
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

    // ── ENGINE WRITES RUN ON THE MAIN THREAD (INVARIANTS #83; native/EngineOps.h is the one road) ──
    // EngineWritesAllowed: may THIS call write engine actor / inventory / caster / graph state in place? The same answer as
    // LiveReadsAllowed: never inside a worker tick, else in the seat, on the main thread, or with no pump installed (the legacy path).
    // RunOrPost: run a_fn here when EngineWritesAllowed(), else Post it (first in, first out with every other post). TRUE = it ran
    // in place. The closure must hold FormIDs / handles / values only: a frame passes before a posted one runs.
    // NoteEngineWrite: the self-check, called on the line before an engine write. Off the seat (inside the worker tick, or on a
    // thread that is not the main one) it is a VIOLATION: counted, said ONCE per site as an `[thread]` error; the write still runs
    // (passive, nothing masked). Diagnostics.cpp prints the count in the `[overlap]` line. It must stay 0.
    bool EngineWritesAllowed();
    bool RunOrPost(std::function<void()> a_fn);
    void NoteEngineWrite(const char* a_site);
    std::uint64_t EngineWriteViolations();

    // ── THE PUMP'S EXCEPTION GUARD (stability audit M5) ──
    // Each posted closure runs in its own try/catch. A throw is an ERROR (rate-limited: the first 8, then every 256th), the closure
    // that threw is lost, and the drain goes on with the next one: one bad closure never takes the rest of the batch or the frame.
    // Nothing is retried and nothing is hidden: PumpFaults() is printed in the `[overlap]` line.
    std::uint64_t PumpFaults();
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

    // RUN WHAT IS QUEUED NOW, ONCE (review of ca0465c9, F1: no posted engine write may be pending across a save). Called from
    // plugin.cpp's kSaveGame handler, on the main thread, AFTER Diagnostics::HoldPumpForSave() drained the worker tick and
    // BEFORE the engine writes a record. Swaps the queue ONCE and runs that batch in post order (FIFO) under the same per-closure
    // guard as the frame drain. THE BOUND: the batch is fixed at the swap; a closure that posts again (a two-hop re-arm, a
    // self-posting poll) lands in the NEW queue and runs next frame, never in this drain, so it runs exactly the closures that
    // were queued at the call and returns. Refused (an ERROR, nothing run) off the main thread or with no pump installed (then
    // nothing is ever queued). Returns how many closures ran.
    std::size_t DrainForSave();
    // Bumped by every Clear(). A poster that keeps "my closure is queued" state compares it, so a Clear that dropped the closure
    // cannot leave that state stuck (Diagnostics.cpp's snapshot build pass).
    std::uint32_t ClearGeneration();

}
