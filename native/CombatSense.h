#pragma once
#include "PCH.h"

// COMBAT SENSE -- one canonical read of "how many foes is this follower fighting
// right now," shared by Confidence (foe count knocks confidence down, tightening
// the leash and arming auto-retreat) and Evaluator (the cond.foe_count_at_least
// gambit). The engine already tracks the fight in the follower's own combat
// group, so we read THAT rather than sweeping the whole cell -- cheaper and more
// correct (a foe the engine dropped from the group is no longer our problem).
// The group's targets list is guarded by its own read lock; HOLD the NiPointer
// before touching the actor (the Targeting rule -- a bare .get().get() can race
// a teardown).
namespace MFO::CombatSense {

    inline int FoeCount(RE::Actor* a_self) {
        if (!a_self) return 0;
        auto* cc = a_self->GetActorRuntimeData().combatController;
        if (!cc || !cc->combatGroup) return 0;
        int n = 0;
        RE::BSReadLockGuard lk(cc->combatGroup->lock);
        for (const auto& t : cc->combatGroup->targets) {
            auto ptr = t.targetHandle.get();          // HOLD the NiPointer
            auto* foe = ptr.get();
            if (!foe || foe == a_self) continue;
            if (foe->IsDead() || foe->IsDisabled()) continue;
            if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
            ++n;
        }
        return n;
    }
}
