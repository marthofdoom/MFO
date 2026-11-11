#pragma once
// Concealment.h -- card G3: DISENGAGE TO OUT OF SIGHT, THEN SNEAK (class plan G3; Docs/research/2026-11/g3-disengage.md).
// A sneak class that a foe has found runs to a point no foe can see and crouches there. The engine then loses him by
// its own rules (G2 Layer 1): MFO writes no detection state and no actor value.
//   1. THE PICK (main thread, spread over frames). Candidate points around the follower from Harbinger's FindEmptySpace
//      (standable, empty, in a straight walk from him), each tested with MFO's OWN rays from every listed foe's eye
//      (Sightline::PointSeenNow, marth 2026-11-11: "MFO's own rays", not a new Harbinger query). A point is taken only
//      when EVERY foe's rays to it are stopped by geometry. Unknown is never read as hidden.
//   2. THE RUN. apmf/Disengage.cpp (card H3): Harbinger ch.24 on that point with the hold bit, plus a ch.7 Offense deny.
//   3. THE CROUCH. On arrival the G1 stance claim (apmf/SneakClaims.cpp) and a hold at the point for a bounded time.
// While a run or its hold stands the follower's rule scan is skipped (Service returns true), as a retreat's is.
// Road switch: [Debug] bIausHideRoad (INI-only, default OFF until a field run proves the drive, principle 5).
// Threads: Prepare / Begin / Service / Release on the pump's AddTask worker; the pick on the main thread through
// MainThread::Post. Own leaf mutex, never held across an engine call or another module's lock. Nothing is saved.

namespace MFO::Concealment {

    // The road exists: the switch is on, Harbinger serves the disengage drive (ABI >= 39) and the main-thread pump runs.
    bool RoadOpen();

    // WORKER (the IAUS gather, once per lap of a sneak-class follower who is found). True = a fresh concealment point is
    // known for him. Asks the main thread for a new pick when the last answer is stale (rate-limited per follower).
    // a_foes: the foes to hide from, the gather's own list (the first 6 are read).
    // The leash radius is read HERE, on the worker (Confidence::LeashRadius), never on the main thread.
    bool Prepare(RE::Actor* a_follower, const std::vector<RE::FormID>& a_foes);

    // WORKER (cast/Fire.cpp, act.disengage). Starts the run to the fresh point. nullptr = started (a_desc says where);
    // otherwise why not (nothing was changed).
    const char* Begin(RE::Actor* a_follower, std::string& a_desc);

    // WORKER, once per combat service of a follower, before his rule scan. Drives a standing run: arrival, the crouch,
    // the bounded hold, every failure said out loud. True = the run holds him this lap (the caller runs no scan).
    bool Service(RE::Actor* a_follower);

    // Any worker road: combat ended, a retreat, dismissal, the source toggle. Idempotent.
    void Release(RE::FormID a_follower, const char* a_why);

    // revert / load (pump drained).
    void ClearTransientState();
}
