#pragma once
// Concealment.h -- card G3: DISENGAGE TO OUT OF SIGHT, THEN SNEAK (class plan G3; Docs/research/2026-11/g3-disengage.md).
// A sneak class that a foe has found runs to a point no foe can see and crouches there. The engine then loses him by
// its own rules (G2 Layer 1): MFO writes no detection state and no actor value.
//   1. THE PICK (main thread, spread over frames). Candidate points around the follower from Harbinger's FindEmptySpace
//      (standable, empty, in a straight walk from him), each tested with MFO's OWN rays from every listed foe's eye
//      (Sightline::PointSeenNow, marth 2026-11-11: "MFO's own rays", not a new Harbinger query). A point is taken only
//      when EVERY foe's rays to it are stopped by geometry. Unknown is never read as hidden.
//   2. THE RUN. apmf/Disengage.cpp (card H3): Harbinger ch.24 on that point with the hold bit, plus a ch.7 Offense deny.
//   3. THE CROUCH. On arrival the G1 stance claim, HELD for the hide (apmf/SneakClaims.cpp HoldSneakForHide), and a hold at the
//      point. marth 2026-11-11 ("the main purpose is to become hidden ... it ends when hidden is accomplished and then a sneak
//      attack becomes the next step"): the run / hold ENDS as soon as G2's verdict says no listed foe detects him (the 12 s
//      cap is only an upper bound and is logged when hit). The stance claim then STAYS for G2's hold row until the blow from
//      stealth RESOLVES (NoteBlow arms it on the pick, Service releases it on his hit or the swing's end), a reveal (a foe detects him again), the fight's end, dismissal, death or a load (Release).
//   NON-SNEAK CLASSES (class tree ON, kDisengage, not kSneakAlways): the same drive is a FLEE to a point clearly farther from
//      the foes (no cover needed). The IAUS keeps running during it (Service returns false): offense is not generated and his
//      own heals are boosted (Snapshot::fleeRun). It ends when he is healed past the retreat's health line, on arrival, or on
//      failure.
// While a SNEAK run or its hold stands the follower's rule scan is skipped (Service returns true), as a retreat's is.
// The IAUS hard cooldown of the family counts from the END of the run (Iaus::Engine::NoteDisengageEnded).
// Road switch: [Debug] bIausHideRoad (INI-only, default OFF until a field run proves the drive, principle 5).
// Threads: Prepare / Begin / Service / Release on the pump's AddTask worker; the pick on the main thread through
// MainThread::Post; the blow's hit sink on the engine's event thread (a marked bool under the leaf mutex). Own leaf mutex, never held across an engine call or another module's lock. Nothing is saved.

namespace MFO::Concealment {

    // The road exists: the switch is on, Harbinger serves the disengage drive (ABI >= 39) and the main-thread pump runs.
    bool RoadOpen();

    // WORKER (the IAUS gather, once per lap of a sneak-class follower who is found). True = a fresh concealment point is
    // known for him. Asks the main thread for a new pick when the last answer is stale (rate-limited per follower).
    // a_foes: the foes to hide from, the gather's own list (the first 6 are read).
    // The leash radius is read HERE, on the worker (Confidence::LeashRadius), never on the main thread.
    // a_flee: the non-sneak flee (a point clearly farther from the foes, no cover needed) instead of a hiding place.
    bool Prepare(RE::Actor* a_follower, const std::vector<RE::FormID>& a_foes, bool a_flee);

    // WORKER. A non-sneak flee run stands for him (the IAUS gather reads it: no offense, his heals boosted).
    bool FleeRunning(RE::FormID a_follower);

    // WORKER (scheduler/IausSource.cpp Record). The pick that fired is a blow (attack, power attack, offense or control cast,
    // exploit, bash). The hide hold's stance claim is NOT released here (review P1-G3 of 938f45f: the engine reads "the
    // attacker is sneaking" when the hit LANDS, 0.3 s or more after the pick): the blow is ARMED, and Service releases the
    // stance when it RESOLVES -- his first hit on an actor (the TESHitEvent sink below), or his attack state / own cast seen
    // live after the pick and then back to none (plus a flight time for a shot or a cast), or the cap on one live swing
    // (kBlowCapMs, counted from the lap the swing was first seen, never from the pick). While a swing is live the reveal road
    // is not read; before it starts the hold is exactly what it was. No-op when no stance is held.
    void NoteBlow(RE::FormID a_follower);

    // kDataLoaded, once (Diagnostics::Install, beside the other TESHitEvent sinks). The sink marks "his hit landed" on a held,
    // armed stance: any thread, a FormID compare under the module's leaf mutex, nothing else.
    void RegisterHitSink();

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
