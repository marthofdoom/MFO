#pragma once
#include "PCH.h"

namespace MFO::Diagnostics {

    // Register the Field Orders cast sink (kDataLoaded).
    void Install();

    // Begin the periodic detection refresh (kPostLoadGame / kNewGame).
    void StartPump();

    // Stops the sleeper thread. Called on revert; MUST be called before the
    // runtime tears down or the thread can queue into a dead task interface.
    void StopPump();

    // Resumable worker quiesce for SaveCallback (SEV-1). PausePump leaves the
    // pump thread/epoch alone but makes every deferred body bail, then drains any
    // body already mutating g_followers, so the co-save's two passes see a stable
    // map. ResumePump lifts it. MUST be paired (use an RAII guard across the
    // callback's early returns). Not for revert -- that is StopPump.
    void PausePump();
    void ResumePump();

    // Full state dump to MFO.log. Safe to call from the main thread only.
    void DumpReport(const char* a_trigger);

    // ── [atk-obs] passive attack-event probe (feat/mfo-attack-observe) ───────
    // Print + reset this follower's per-fight attack histogram NOW, if a fight is
    // open for him. Idempotent: no open fight -> no line, nothing touched. The
    // probe closes fights ITSELF (IsInCombat low for 1.5 s, in the sleeper tick --
    // flap-tolerant, one line per fight), so nothing calls this today; it is
    // exported for an explicit release point that wants the line early (a
    // dismissal path, a board action). DECIDED (Fable round on 2ac4163): it is NOT
    // wired into the Scheduler's 2-tick OOC debounce -- that ~266 ms debounce would
    // split one fight into two lines on an IsInCombat flap. WORKER / serial-pump
    // domain only (#4): it reads the slot table's worker-owned fight state.
    void DumpAttackHistogram(RE::FormID a_actorID);

    // Revert: forget every slot (fid, counters, open fights). Called from
    // ResetAllState AFTER StopPump drained the worker. The graph sinks stay
    // registered on whatever graphs survive -- they are static objects, and an
    // event on a slot whose fid is 0 is ignored, so nothing dangles.
    void ResetAttackObserve();

    // ── Cross-TU pump-drain gate (SEV-1 concurrency wave) ─────────────────────
    // Event sinks in OTHER translation units (Rapport death, Logistics loot,
    // Board focus-fire) queue AddTask bodies that mutate save-scoped state
    // (Followers::Refresh, the loot/claim maps) exactly like this file's own
    // sinks do, so they must drain with StopPump/PausePump the same way -- or a
    // revert/save can clear those maps mid-body (the #4 hazard). These expose the
    // ONE guard shape used by every AddTask body queued in Diagnostics.cpp:
    // capture the epoch at QUEUE time, construct the gate FIRST in the deferred
    // body, and bail on `!gate`. See the HitSink in Diagnostics.cpp for the
    // canonical use. The pump atomics stay file-local; the gate's ctor/dtor are
    // defined out-of-line in Diagnostics.cpp so they reach them.
    std::uint64_t CurrentPumpEpoch();

    class PumpTickGate {
    public:
        explicit PumpTickGate(std::uint64_t a_queuedEpoch);
        ~PumpTickGate();
        explicit operator bool() const { return m_ok; }
        PumpTickGate(const PumpTickGate&)            = delete;
        PumpTickGate& operator=(const PumpTickGate&) = delete;
    private:
        bool m_ok = false;
    };

}
