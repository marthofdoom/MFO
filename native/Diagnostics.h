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

}
