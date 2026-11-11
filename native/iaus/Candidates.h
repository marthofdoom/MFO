#pragma once
#include "PCH.h"

#include "iaus/Core.h"
#include "iaus/Generate.h"
#include "iaus/Snapshot.h"

// iaus/Candidates.h -- the ENGINE SIDE of IAUS candidate generation (card I4). LandingCrew's idea (iaus/Core.h).
//
// Two layers (iaus/Snapshot.h):
//   GATHER (here): the worker's reads + the SpellBook main-thread mirror -> a plain Snapshot. It reads, it never
//   decides: every "is this a candidate" rule is in the pure iaus/Generate.cpp, which the harness and the combat
//   simulator run too.
//   GENERATE (iaus/Generate.h, pure): Snapshot + ClassGate + Tuning -> the candidates.
// Build() is the two in a row for one follower.
//
// CALLED BY the scan's IAUS source (scheduler/IausSource.cpp, card I6): Build, then Engine::FillCooldowns /
// Engine::Decide over set.gen.actions (DecidePosture over set.gen.postures), then NoteDecision. Clear() runs in
// ResetAllState (beside SpellBook::Clear, after StopPump).
//
// THREADING: everything here is the g_followers worker domain (#4) -- the AddTask job worker inside the scan,
// no lock of its own, exactly like iaus/Engine's Minds. Gather makes only reads the Evaluator already makes on
// that worker (AV pools, levels, positions, the combat group walk under its read lock with NOTHING else inside
// it, g_active, FoeTargets, race keywords, resist AVs, the Sightline cache, Targeting::Current, CombatSense,
// Logistics::IsUnholy). Whatever needs the main thread (spell list, inventory, active effects, magicka cost)
// arrives through SpellBook::Read; G10 answers arrive through cast/WillWork's own mirror. Clear(): ResetAllState
// with the pump drained.
namespace MFO::Iaus::Candidates {

    // One follower's tick. The candidates' names are string_views into `snap`, so a Set is filled in place and is
    // neither copied nor moved.
    struct Set {
        Snapshot  snap;
        ClassGate gate;
        Generated gen;
        Set() = default;
        Set(const Set&) = delete;
        Set& operator=(const Set&) = delete;
    };

    // WORKER. Fill a_out from the live fight. False (a_out untouched beyond a reset) when a_follower is null,
    // dead, disabled or not 3D loaded.
    bool Gather(RE::Actor* a_follower, Snapshot& a_out);

    // WORKER. Gather + SpellBook::GateFor + Generate. False when Gather refused; a_out.gen is then empty.
    bool Build(RE::Actor* a_follower, Set& a_out, const Tuning& a_t = DefaultTuning());

    // WORKER, after Decide: remember the top-6 cast-offense / cast-control (spell, target) pairs, so the NEXT
    // Gather asks cast/WillWork for those (design 1.5; a call there is also the prefetch). Unknown passes. A pair
    // answered No stays on the list while the refusal stands (Generate.h NextAskList, sim finding D1).
    void NoteDecision(RE::FormID a_follower, const Decision& a_d, std::span<const Candidate> a_cands);

    // WORKER. The fight ended for a_follower (his combat clock restarts at the next Gather) / he was dismissed
    // or his IAUS toggle went off.
    void CombatEnded(RE::FormID a_follower);
    void Forget(RE::FormID a_follower);

    // ResetAllState, after StopPump (card I6 wires it).
    void Clear();

    // "foes 5/8 | attack 5 power 0 ... | dropped cost 3 class 0 hand 0 target 2 | book yes" for a log line.
    [[nodiscard]] std::string Describe(const Set& a_set);

}
