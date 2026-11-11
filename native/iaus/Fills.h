#pragma once
#include "PCH.h"

#include "iaus/Snapshot.h"

#include <string>
#include <vector>

// iaus/Fills.h -- THE ENGINE SIDE OF THE PHASE-2 ESTIMATES (card "IAUS engine fills"). LandingCrew's idea for MFO, as
// the rest of iaus/. Record: Docs/research/2026-11/iaus-engine-fills.md. Formulas: iaus-i8-damage-research.md 2.1-2.4.
//
// The phase-2 considerations (iaus/Estimate.h) read measured inputs: how hard a foe hits, how much armor stops, what a
// spell does, who is mid-swing or mid-cast, who dies for good. This module MEASURES them from the engine.
//
// PROBE FIRST (principle 5: the disassembly proves a path exists, not that it runs). Every input is measured and LOGGED
// next to what really happened before it may move a score:
//   [Debug] bIausFillProbe = 1    -> `[iaus-fill]` lines (rate-limited). Nothing is written into the Snapshot.
//   [Debug] bIausFill<Input> = 1  -> that one input is written. All default OFF (Config.h).
//   [Debug] bIausReachLearn = 1   -> Snapshot::meleeReach is the swing reach LEARNED from his own hits and misses
//                                    (ruling Q68, iaus/Reach.h); fMeleeReach while the learner knows nothing (MFO-B687).
// With every key OFF this module does nothing: no read, no map entry, no line.
//
// NO NEW ENGINE ADDRESS. Everything here is an actor-value read, a record field, a GMST, an ActorState bit field or the
// actor's own magicCasters[] (the plain loads cast/Hands.cpp CastInFlightOnHand already makes on this worker). The one
// id call is BGSEntryPoint::HandleEntryPoint, through its EXISTING verified row
// (`TradePerks.BGSEntryPoint.HandleEntryPoint`), refused when the row does not verify, MAIN THREAD only.
//
// WHAT IS LEFT OUT, and said so in the probe lines (never fabricated):
//   * the weapon's tempering, the ammunition's damage, the attack data's damage multiplier (a power attack) and the
//     attacker's perk entry points on a weapon hit (they need the main thread and, for the core formula, a new id row);
//   * the armor base factor sum (Actor vtable 0xE7: a lazy cache WRITE, main thread only) and the target's perks;
//   * the element resist of a foe's spell, a spell's duration perks (EP 0x1E), dual casting.
// The `[iaus-fill] hit` line is what shows how much those cost: it prints the estimate next to the health the hit took.
//
// THREADING.
//   Pass (Unit / Finish)  the g_followers WORKER (#4), inside Candidates::Gather. The tracker maps are that domain.
//   MeasureSpell          MAIN THREAD (SpellBook::Measure).
//   the hit sink          whatever thread the engine sends TESHitEvent on. It copies ids and one health read into a
//                         bounded list under a LEAF mutex and returns. It never touches a follower list (#4).
//   StartupCheck          MAIN, kDataLoaded.   Clear   ResetAllState after StopPump (through Candidates::Clear).
namespace MFO::Iaus::Fills {

    enum class Role : std::uint8_t { Self = 0, Foe, Ally };

    // MAIN, kDataLoaded (after Config::Read and Forms::Resolve): resolve the GMSTs, check the entry-point row, log the
    // `[iaus-fill] gmst` line (the Requiem check) and the switch states.
    void StartupCheck();

    // MAIN, Diagnostics::Install: the hit sink (inert while bIausFillProbe = 0).
    void RegisterHitSink();

    // WORKER. One Gather's worth of fills. Construct it, call Unit on every Combatant right after its plain reads, and
    // Finish before Gather returns.
    class Pass {
    public:
        Pass();
        [[nodiscard]] bool Armed() const { return m_armed; }
        // Measure a_actor and write the switched-on inputs into a_u. a_u's id / pools / position are already filled.
        void Unit(Combatant& a_u, RE::Actor* a_actor, Role a_role);
        // Snapshot::mortal, the learned swing reach, then the probe lines of this lap (unit lines, the hits whose
        // target this lap sampled).
        void Finish(Snapshot& a_s, RE::Actor* a_self);

        struct Est {   // what was measured for one actor this lap (kept for the probe whatever the switches say)
            std::uint32_t id = 0;
            Role  role = Role::Foe;
            float hit = 0.0f, rate = 0.0f;
            bool  magic = false, ranged = false, swinging = false, casting = false;
            float dr = 0.0f, armor = 0.0f, magicResist = 0.0f, loss = 0.0f;
            std::uint32_t src = 0;      // the weapon / spell / staff the hit estimate is for
            int   skillAv = -1;
            float skill = 0.0f;
            const char* kind = "none";  // weapon | spell | staff | unarmed | none
            int   atkState = 0, castState = 0;
            float hp = 0.0f;            // this lap's health
            double prevT = 0.0;         // the tracker's sample BEFORE this lap (0 = none)
            float  prevHp = 0.0f;
        };

    private:
        bool m_armed = false, m_probe = false, m_armor = false, m_hit = false, m_loss = false, m_swing = false,
             m_casting = false, m_mortal = false, m_reach = false;
        double m_now = 0.0;
        std::vector<Est> m_est;
        void LearnReach(Snapshot& a_s, RE::Actor* a_self);
    };

    // MAIN THREAD. One spell or scroll: its health damage, healing and control seconds from the effect records, the
    // magnitude through EP 0x1D (kModSpellMagnitude) when the row verified and a target exists. Writes a_e.damage /
    // heal / controlSec / measured only with bIausFillSpells = 1. a_probe gets the `[iaus-fill] spell` line while the
    // probe is on (empty otherwise), a_probeKey a value that changes when the line is worth printing again.
    void MeasureSpell(RE::Actor* a_caster, RE::SpellItem* a_spell, SpellEntry& a_e, std::string& a_probe,
                      std::uint32_t& a_probeKey);

    // ResetAllState, after StopPump.
    void Clear();

}
