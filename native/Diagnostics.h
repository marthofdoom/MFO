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

    // Full state dump to MFO.log. Safe to call from the main thread only.
    void DumpReport(const char* a_trigger);

}
