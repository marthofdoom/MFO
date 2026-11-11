#pragma once
// iaus/Disengage.h -- card G3's questions to the snapshot: HAS A FOE FOUND HIM, and (for a class that flees rather than hides)
// IS HE HURT UNDER THREAT? PURE (std types only), so the off-line simulator and the harness read the same answer as the game.
//
// "Has a foe found him" is G2 Layer 1's verdict: each foe's own combat-group record of the follower, read into
// `Combatant::detectsMe` (iaus/Hide.h). Yes = the engine says that foe has him. Unknown and No both mean it does not.
#include "iaus/Snapshot.h"

#include <algorithm>

namespace MFO::Iaus::Disengage {

    // marth 2026-11-11 (the "healed past" line of the non-sneak flee): the retreat's own health threshold, the one
    // Cons::RetreatHp (VetoAbove 0.30) and RetreatDanger's health term (iaus/Catalogue.cpp) already use. Not a new number.
    inline constexpr float kFleeHealthLine = 0.30f;

    [[nodiscard]] inline bool FoeHasFound(const Combatant& a_foe) {
        return a_foe.detectsMe == Tri::Yes;
    }

    // A foe has found him: the hide run has a reason. Fleeing and paralysed foes do not count (they are not hunting).
    [[nodiscard]] inline bool Found(const Snapshot& a_s) {
        for (const Combatant& f : a_s.foes)
            if (!f.paralysed && !f.fleeing && FoeHasFound(f)) return true;
        return false;
    }

    // The share of the counted foes (not fleeing, not paralysed) that have found him: the hide run's need. 0 with no foe.
    [[nodiscard]] inline float DetectShare(const Snapshot& a_s) {
        int counted = 0, found = 0;
        for (const Combatant& f : a_s.foes) {
            if (f.paralysed || f.fleeing) continue;
            ++counted;
            if (FoeHasFound(f)) ++found;
        }
        return counted > 0 ? static_cast<float>(found) / static_cast<float>(counted) : 0.0f;
    }

    // Some foe that hunts him now targets him (the "threat" half of the non-sneak flee: low health AND a foe on him).
    [[nodiscard]] inline bool Threatened(const Snapshot& a_s) {
        for (const Combatant& f : a_s.foes)
            if (!f.paralysed && !f.fleeing && f.targetId != 0 && f.targetId == a_s.self.id) return true;
        return false;
    }
}
