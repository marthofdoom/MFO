#pragma once
// iaus/Estimate.h -- WHAT A FIGHT IS WORTH IN NUMBERS (the damage-estimate part of card I8; sim v2 phase 2).
// LandingCrew's idea for MFO, as the rest of iaus/.
//
// PURE (standard headers + Snapshot.h, never an RE:: type): the same code runs in the game, the harness and the
// combat simulator. It turns the measured inputs of a Snapshot into the few numbers a skilled player weighs before
// he acts: how hard a foe hits whom, how long someone lives at the present rate, what a spell will actually do to
// THIS target.
//
// INPUTS (Snapshot.h; 0 = not measured, and every function then answers "unknown", never a guess):
//   Combatant::hit          physical damage of ONE of its weapon hits before the target's armor
//   Combatant::hitsPerSec   the engine's own rate model (weapon speed x fCombatDPS*SpeedMult)
//   Combatant::hitIsMagic   the hit is a spell (armor does not apply)
//   Combatant::armor        its physical mitigation, 0..1
//   Combatant::magicResist  kResistMagic, points
//   Combatant::lossPerSec   health it has been LOSING per second (observed: CombatSense)
//   SpellEntry::damage/heal expected health damage / healing of one cast on an unresisting target, after perks
// The formulas are the engine's (Docs/research/2026-11/iaus-i8-damage-research.md 2.1 - 2.4).
//
// UNKNOWN IS NEUTRAL. A time-to-live is +inf when nothing is known to be hitting (the considerations that read it are
// then not applicable). Nothing here divides by a value it has not checked.
#include "iaus/Snapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace MFO::Iaus::Estimate {

    inline constexpr float kUnknown = std::numeric_limits<float>::infinity();

    [[nodiscard]] inline float Pos(float a_v) { return (std::isfinite(a_v) && a_v > 0.0f) ? a_v : 0.0f; }

    // Health per second a_from deals to a_to with its weapon (0 = not measured).
    [[nodiscard]] inline float Dps(const Combatant& a_from, const Combatant& a_to) {
        const float hit = Pos(a_from.hit), rate = Pos(a_from.hitsPerSec);
        if (!(hit > 0.0f) || !(rate > 0.0f)) return 0.0f;
        const float armor = a_from.hitIsMagic ? 0.0f : std::clamp(std::isfinite(a_to.armor) ? a_to.armor : 0.0f, 0.0f, 1.0f);
        return hit * rate * (1.0f - armor);
    }

    // Health per second a_who is losing: what the foes that target him deal, and never less than what was observed.
    [[nodiscard]] inline float IncomingOn(const Snapshot& a_s, const Combatant& a_who) {
        float sum = 0.0f;
        for (const Combatant& f : a_s.foes)
            if (f.targetId != 0 && f.targetId == a_who.id && !f.paralysed && !f.fleeing) sum += Dps(f, a_who);
        return (std::max)(sum, Pos(a_who.lossPerSec));
    }

    // Seconds until a_health is gone at a_incoming per second. kUnknown when nothing is coming in.
    [[nodiscard]] inline float Ttl(float a_health, float a_incoming) {
        const float h = Pos(a_health), in = Pos(a_incoming);
        if (!(in > 0.0f)) return kUnknown;
        return h / in;
    }

    // What one cast of a_e does to a_to's health: damage x (1 - element resist) x (1 - magic resist); 0 at 100 points
    // of either (the engine's own rule). 0 = the spell's damage was not measured.
    [[nodiscard]] inline float SpellOn(const SpellEntry& a_e, const Combatant& a_to) {
        const float dmg = Pos(a_e.damage);
        if (!(dmg > 0.0f)) return 0.0f;
        const float el = a_e.resistAv >= 0 ? a_to.Resist(a_e.resistAv) : 0.0f;
        const float mr = std::isfinite(a_to.magicResist) ? a_to.magicResist : 0.0f;
        if (!std::isfinite(el) || el >= 100.0f || mr >= 100.0f) return 0.0f;
        return dmg * (1.0f - el / 100.0f) * (1.0f - mr / 100.0f);
    }

}
