#pragma once
// logistics/ErrandCounter.h -- TOWN PHASE 2 R4 "found at the counter" (ClickUp 86e3t1mx7). Internal to logistics/ (Errand.cpp,
// ErrandSim.cpp). The learned VENDOR MAP (where each shop's vendor actually stands) and the BROWSING-hold marker.
// Nothing here is saved (session-only, cleared by Errand::ClearTransientState); nothing here calls the engine except reading
// an already-loaded vendor's position and asking Harbinger (R4b) to place an unloaded follower. Threading: the AddTask job worker (BuildCatalog / Tick / Sim::Sweep); one own mutex,
// held for map access only, never across an engine / Harbinger call.
#include "Errand.h"

#include <string>

namespace MFO::Logistics::Errand::Counter {

    struct Spot {
        RE::FormID   vendor = 0;
        RE::FormID   cell = 0;
        RE::NiPoint3 pos{};
        float        yaw = 0.0f;
    };

    // A LOADED, awake vendor of `a_faction` (the catalog's vendor scan). A spot is CONFIRMED only when two samples at least
    // kSettleSec apart agree within kStillRadius (a vendor in mid-walk is not at his counter); a sample that disagrees with the
    // confirmed spot (another actor of the faction, another cell, a move) DROPS it (confirmed=false) until a new pair agrees: a stale
    // counter is worse than none. NOTE: "confirmed" means stationary for ~2 s+ at ANY sandbox marker, not proven "at the counter".
    void Learn(const RE::Actor* a_vendor, RE::FormID a_faction, Clock::time_point a_now);
    bool Lookup(RE::FormID a_faction, Spot& a_out);

    // The follower is LOADED again at his errand's shop (ResumePhysical): one log line saying whether the counter is known and
    // how far he stands from it. `a_vendor` null = the vendor is not in his cell.
    void NoteResume(RE::FormID a_follower, const std::string& a_name, const RE::Actor* a_follower3d, const RE::Actor* a_vendor,
                    RE::FormID a_faction);
    // The OUT_SIM -> TRADE_SIM arrival (off screen): one log line, and (counter CONFIRMED + INTERIOR + Harbinger ABI >= 28) a Harbinger
    // placement request beside the vendor (see the .cpp). AddTask worker.
    void NoteSimArrival(RE::FormID a_follower, const std::string& a_name, const std::string& a_vendorName, RE::FormID a_faction);
    // Poll the follower's pending placement (a no-op when none): logs Placed / Refused(why) / Failed(why) ONCE, then forgets it.
    // Called each Sim::Sweep pass for the record. AddTask worker.
    // a_final = the errand record is ending (Drop / loaded again): a still-Queued placement is logged and forgotten, so a stale entry
    // never outlives its errand or reports under the next one.
    void PollPlacement(RE::FormID a_follower, const std::string& a_name, bool a_final = false);

    void Clear();
}
