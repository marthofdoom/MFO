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

    // Drop every queued closure without running it. Called on revert: a fn posted
    // before a load re-resolves its captured handle against the NEXT session's
    // handle table (handles are reused), so draining it post-load could run on the
    // wrong actor. The hook stays installed; only the pending work is discarded.
    void Clear();

}
