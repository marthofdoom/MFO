#pragma once
#include "PCH.h"

#include "iaus/Core.h"

// iaus/Engine.h -- the THIN engine-facing adapter around the pure IAUS core (card I2). Everything here that is
// not a one-line bridge belongs in iaus/Core.h instead. The idea is LandingCrew's (credit in Core.h).
//
// WHAT IT ADDS TO THE CORE: the clock (steady_clock, the same one Cooldowns uses), the per-follower seed and
// commitment window (Temperament, Config::g_suppressWindow), the SHARED Cooldowns store (Kind::IausAction, so a
// "10 s power-attack cooldown" is one store in both modes, card G9), the per-follower Mind map, the [iaus] /
// [iaus-why] log lines, the fIausBudgetMs warning, and the board readout under a leaf mutex.
//
// NOBODY CALLS IT YET. Card I6 wires it into the combat scan and wires Clear() into ResetAllState.
//
// THREADING:
//   - Minds (MindFor, NoteFired, EndDecision, Forget, Decide, Log): the g_followers domain (#4) -- the AddTask job
//     worker inside the scan. No lock, like every other per-follower scan map.
//   - Readout (Publish / ReadReadout): its own leaf mutex. The worker publishes, the render thread copies. No
//     engine call is made under it.
//   - Clear(): ResetAllState, AFTER Diagnostics::StopPump (the worker drained). Cooldowns::ClearAll, already
//     called there for G9, also clears the Kind::IausAction entries.
namespace MFO::Iaus::Engine {

    struct Tick {
        TickIn                                in;   // what the core reads
        std::chrono::steady_clock::time_point tp;   // the same instant, for Cooldowns
    };

    // One tick's inputs for a_actor: now, its rapport rank (1..5, Rapport::RankFor), the commitment window
    // g_suppressWindow x (0.88 + 0.24 Temperament(id)) (the #26 window's length, design 4.5) and the seed.
    [[nodiscard]] Tick MakeTick(RE::FormID a_actor, std::uint8_t a_rapportRank);

    // Fill Facts::hardCooldownLeft and Facts::sinceFired of every candidate from the shared Cooldowns store.
    void FillCooldowns(RE::FormID a_actor, std::span<Candidate> a_cands, const Tick& a_tick, const Tuning& a_t);

    // The follower's runtime memory (created empty on first use). WORKER ONLY.
    [[nodiscard]] Mind& MindFor(RE::FormID a_actor);

    // Core Decide, timed. A tick over a_t.budgetMs logs one WARN (rate-limited per follower), never a silent cut;
    // offering more than a_t.maxCandidates logs one WARN per follower per session with the family counts.
    [[nodiscard]] Decision Decide(RE::FormID a_actor, std::span<const Candidate> a_cands, const Profile& a_p,
                                  const Tuning& a_t, const Tick& a_tick, double* a_ms = nullptr);

    // Actuation::Fire returned FIRED for a_step: the Mind (OnFired) and the Cooldowns store.
    void NoteFired(RE::FormID a_actor, const Candidate& a_c, const Step& a_step, const Tick& a_tick);
    void NotePostureFired(RE::FormID a_actor, const Candidate& a_c, const Tick& a_tick);

    // The [iaus] line (on change, plus every logRepeatSec while it runs) and, when a_explain (bIausExplain), one
    // [iaus-why] line per top-3 candidate. a_pickedStep = the index into a_d.order that fired, -1 for none.
    void Log(RE::FormID a_actor, std::string_view a_name, const Decision& a_d, std::span<const Candidate> a_cands,
             const Profile& a_p, const Tuning& a_t, const Tick& a_tick, int a_pickedStep,
             std::span<const WalkNote> a_walk, double a_ms, bool a_explain);

    // The board's "Thinking" panel (card I7).
    void Publish(RE::FormID a_actor, Readout a_r);
    [[nodiscard]] bool ReadReadout(RE::FormID a_actor, Readout& a_out);

    // The running decision ended (combat end) / the IAUS toggle edge or dismissal (design 1.3: no rank from the
    // old source survives into the new one).
    void EndDecision(RE::FormID a_actor);
    void Forget(RE::FormID a_actor);

    // ResetAllState, after StopPump: every Mind, every readout, the once-per-session warning flags.
    void Clear();

}
