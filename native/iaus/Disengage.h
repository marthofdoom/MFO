#pragma once
// iaus/Disengage.h -- card G3's ONE question to the snapshot: HAS A FOE FOUND HIM? PURE (std types only), so the
// off-line simulator and the harness read the same answer as the game.
//
// THE SEAM TO G2 LAYER 1 (branch feat/g2-hide-layer1, not on main when this was written). Layer 1 reads each foe's own
// combat-group record of the follower into `Combatant::detectsMe` (Tri). That is the engine's verdict and the right
// answer to this question. Until it is on main this header answers from what main's snapshot has: a foe whose combat
// TARGET is the follower has found him (a foe cannot target what it has not detected). That misses a foe that detects
// him while it targets someone else, so it is the narrower reading, never a wider one.
// WHEN LAYER 1 MERGES: replace the body of FoeHasFound with `return a_foe.detectsMe == Tri::Yes;` and delete this
// paragraph. Nothing else in card G3 changes.
#include "iaus/Snapshot.h"

namespace MFO::Iaus::Disengage {

    [[nodiscard]] inline bool FoeHasFound(const Snapshot& a_s, const Combatant& a_foe) {
        return a_foe.targetId != 0 && a_foe.targetId == a_s.self.id;
    }

    // A foe has found him: the hide run has a reason. Fleeing and paralysed foes do not count (they are not hunting).
    [[nodiscard]] inline bool Found(const Snapshot& a_s) {
        for (const Combatant& f : a_s.foes)
            if (!f.paralysed && !f.fleeing && FoeHasFound(a_s, f)) return true;
        return false;
    }
}
