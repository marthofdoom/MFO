#pragma once
#include "PCH.h"
#include <cmath>   // std::sqrt -- FoeWeight

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

    // FOE STRENGTH (Confidence v2, 86e3erv94). How much ONE foe weighs against
    // a_self, bounded to [kFoeWeightMin, kFoeWeightMax]: the geometric mean of the
    // foe's level over his and the foe's CURRENT health over his MAXIMUM health
    // (the same permanent + temporary maximum Vocab::Pct divides by). An even foe
    // reads 1.0, a mudcrab the 0.5 floor, a dragon the 2.0 cap; a foe worn down
    // to a sliver weighs less than a fresh one. His own current health is NOT in
    // the ratio -- Confidence's vitality term already counts it. Pure AV / level
    // reads on the held foe, the same reads Evaluator's foe selectors make on
    // this worker (Vocab::HealthPct(foe), foe->GetLevel()).
    inline constexpr float kFoeWeightMin = 0.5f;
    inline constexpr float kFoeWeightMax = 2.0f;

    inline float FoeWeight(RE::Actor* a_self, RE::Actor* a_foe) {
        if (!a_self || !a_foe) return 1.0f;
        auto* selfAv = a_self->AsActorValueOwner();
        auto* foeAv  = a_foe->AsActorValueOwner();
        if (!selfAv || !foeAv) return 1.0f;
        const float selfMax = selfAv->GetPermanentActorValue(RE::ActorValue::kHealth) +
                              a_self->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kHealth);
        const float foeHp   = foeAv->GetActorValue(RE::ActorValue::kHealth);
        if (selfMax <= 0.0f) return 1.0f;
        const float lvlRatio = static_cast<float>(std::max<std::uint16_t>(1, a_foe->GetLevel())) /
                               static_cast<float>(std::max<std::uint16_t>(1, a_self->GetLevel()));
        const float hpRatio  = std::max(0.0f, foeHp) / selfMax;
        return std::clamp(std::sqrt(lvlRatio * hpRatio), kFoeWeightMin, kFoeWeightMax);
    }

    // The WEIGHTED foe tally: the same foes FoeCount counts (same group, same
    // read lock, same filters), each weighted by FoeWeight. 0 when FoeCount would
    // be 0. a_count (optional) receives that plain count, so one lock pass gives
    // the [sense] line both numbers. FoeCount itself is unchanged: the party gate,
    // the equip-range gate and cond.foe_count_at_least keep reading a plain count.
    inline float FoeLoad(RE::Actor* a_self, int* a_count = nullptr) {
        if (a_count) *a_count = 0;
        if (!a_self) return 0.0f;
        auto* cc = a_self->GetActorRuntimeData().combatController;
        if (!cc || !cc->combatGroup) return 0.0f;
        float load = 0.0f;
        int   n    = 0;
        RE::BSReadLockGuard lk(cc->combatGroup->lock);
        for (const auto& t : cc->combatGroup->targets) {
            auto ptr = t.targetHandle.get();          // HOLD the NiPointer
            auto* foe = ptr.get();
            if (!foe || foe == a_self) continue;
            if (foe->IsDead() || foe->IsDisabled()) continue;
            if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
            load += FoeWeight(a_self, foe);
            ++n;
        }
        if (a_count) *a_count = n;
        return load;
    }
}
