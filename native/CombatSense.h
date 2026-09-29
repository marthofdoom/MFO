#pragma once
#include "PCH.h"
#include <cmath>   // std::sqrt -- FoeWeight; std::fabs / std::isfinite -- the melee-reach read
#include "Config.h" // g_meleeReach -- the melee-reach read

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

    // ── MELEE REACH vs AN AIRBORNE FOE (fix/mfo-unreachable-flyer, field 0928c) ──────
    // Cicero (melee by class, his bow correctly denied) kept engaging an airborne Alduin
    // 5-11k away; the engine's ranged plan walked him off a mountain. A MELEE-ONLY
    // follower (Actuation::MeleeOnly) must not latch or chase what a swing cannot reach.
    //
    // UNREACHABLE = AIRBORNE AND THE VERTICAL GAP BETWEEN THE TWO BODIES EXCEEDS THE
    // MELEE REACH. Never IsFlying() alone (marth: "flying enemy edge cases where they are
    // always technically flying"):
    //  * AIRBORNE = the foe's ActorState FLY_STATE is TakeOff / Cruising / Hovering /
    //    Action. None (every ground actor, and every creature that FLOATS by its skeleton:
    //    netch, wispmothers, hovering mod creatures), Perching (a dragon on a word wall) and
    //    Landing (it is coming down: go meet it) are NOT airborne. FLY_STATE is the 3-bit
    //    field at ActorState1 bits 18-20 (fork fde0f3ae include/RE/A/ActorState.h:113,
    //    reached through AsActorState(), which relocates the ActorState base 0xB8 SE /
    //    0xC0 1.6.629+). Verified in BOTH unpacked binaries: 1.6.1170 inlines the engine's
    //    own IsFlying as `mov eax,[actor+0xC8]; and eax,0x1C0000; je; cmp eax,0x140000`
    //    (5<<18 = Perching; e.g. 0x2EB9D9, 0x335C3D, 0x362794), 1.5.97 as
    //    `mov eax,[actor+0xC0]; shr eax,0x12; and eax,7; je; cmp eax,5` (80+ sites, e.g.
    //    0x3BC612). Same bits on both layouts, so one path (principle 11: nothing differs).
    //  * VERTICAL GAP: a foe ABOVE is out of reach when its origin (the lowest point we
    //    count) is more than `reach` over the top of his head: dz > selfHeight + reach. A
    //    foe BELOW (he stands on a ledge over it) is out of reach when its top is more than
    //    `reach` under his feet: -dz > foeHeight + reach. `reach` = Config::g_meleeReach
    //    (fMeleeReach, 200 u) -- the SAME number the power-attack gate calls "adjacent"
    //    (cast/Fire.cpp). Heights = BodyHeight below (field reads only, never the
    //    engine's bound virtuals), with the nominal-humanoid fallback (120) Sightline uses. A human follower (~128) gets
    //    a line ~330 u over his feet: a dragon hovering low enough to be hit is in reach,
    //    one circling overhead is not.
    //  * Horizontal distance is deliberately NOT part of it: running closes that.
    // a_slack moves the line: negative = the foe must come that much LOWER to count as in
    // reach (the leash hold's hysteresis, kReachHoldBand), so a flyer bobbing on the line
    // cannot flip the ch.23 radius every service. Pure reads of the two actors; worker-
    // safe the way the Evaluator's other foe reads are (no lock taken here).
    inline constexpr float kReachHoldBand     = 64.0f;
    inline constexpr float kNominalBodyHeight = 120.0f;

    inline bool IsAirborne(RE::Actor* a_actor) {
        const auto* st = a_actor ? a_actor->AsActorState() : nullptr;
        if (!st) return false;
        switch (st->GetFlyState()) {
            case RE::FLY_STATE::kNone:
            case RE::FLY_STATE::kPerching:
            case RE::FLY_STATE::kLanding:
                return false;
            default:
                return true;
        }
    }

    // Body height from FIELD READS ONLY -- no engine call, safe on the job worker and
    // inside the combat group's read lock. NOT Actor::GetHeight(): its bound virtuals
    // (Actor vtable 0x73 / 0x74, 1.6.1170 0x14065EF30 / 0x14065F010) walk currentProcess
    // and WRITE process-global statics (0x1431994F0..F8) before copying out, which races
    // the main thread (review U1 on 0f9ac10); CommonLib's GetHeight also writes the cache.
    //   1. the high process's cachedActorHeight when the engine has set it
    //      (AIProcess::GetCachedHeight / InHighProcess: member reads of high / processLevel);
    //   2. else the base form's OBND z-extent (TESBoundObject::boundData, static record
    //      data) x TESObjectREFR::GetBaseHeight (refScale x the NPC's race height: member
    //      reads, fork src/RE/T/TESObjectREFR.cpp:135, TESNPC.cpp:125);
    //   3. else kNominalBodyHeight (an NPC_ record whose OBND is zero).
    inline float BodyHeight(RE::Actor* a_actor) {
        if (!a_actor) return kNominalBodyHeight;
        float h = 0.0f;
        if (auto* proc = a_actor->GetActorRuntimeData().currentProcess; proc && proc->InHighProcess())
            h = proc->GetCachedHeight();
        if (!(std::isfinite(h) && h > 1.0f)) {
            if (const auto* base = a_actor->GetObjectReference()) {
                const auto& bd = base->boundData;
                h = static_cast<float>(bd.boundMax.z - bd.boundMin.z) * a_actor->GetBaseHeight();
            }
        }
        return (std::isfinite(h) && h > 1.0f) ? h : kNominalBodyHeight;
    }

    struct ReachRead {
        float dz    = 0.0f;   // foe origin z - his origin z (+ = the foe is above him)
        float line  = 0.0f;   // |dz| past which the foe is out of reach (height + reach + slack)
    };

    // The [reach] lines' numbers: "dz 2400 > reach 330" (above) / "dz -900 < -reach 420" (below).
    inline std::string ReachText(const ReachRead& a_rr) {
        return a_rr.dz >= 0.0f ? std::format("dz {:.0f} > reach {:.0f}", a_rr.dz, a_rr.line)
                               : std::format("dz {:.0f} < -reach {:.0f}", a_rr.dz, a_rr.line);
    }

    // True = a_foe is airborne AND out of a_self's melee reach vertically. a_out (optional)
    // receives the numbers for the [reach] line. a_selfHeight: pass BodyHeight(a_self)
    // when checking many foes (computed once), or <= 0 to read it here.
    inline bool OutOfMeleeReach(RE::Actor* a_self, RE::Actor* a_foe, float a_slack = 0.0f,
                                ReachRead* a_out = nullptr, float a_selfHeight = 0.0f) {
        if (!a_self || !a_foe || !IsAirborne(a_foe)) return false;
        const float reach = Config::g_meleeReach.load(std::memory_order_relaxed);
        const float dz    = a_foe->GetPosition().z - a_self->GetPosition().z;
        const float h     = dz >= 0.0f ? (a_selfHeight > 0.0f ? a_selfHeight : BodyHeight(a_self))
                                       : BodyHeight(a_foe);
        const float line  = h + reach + a_slack;
        if (a_out) { a_out->dz = dz; a_out->line = line; }
        return std::fabs(dz) > line;
    }

    // THE HOLD QUESTION (the Scheduler's ch.23 leash): over a_self's combat group, how many
    // foes the picker could give him -- PickFoe's filters (live, not kTargetLost, 3D
    // loaded, hostile to him), within a_chaseCap, AND in reach -- and how many live
    // hostile foes are airborne out of reach. The flyer count takes NO cap (a flyer
    // circling 5 k out is exactly what drags him) and ignores kTargetLost / unloaded 3D:
    // a flyer flickering out of perception is still his fight, and counting it keeps the
    // hold (and the ch.23 radius) from flipping on every flicker (MFO-B101's shape).
    // a_slack as above. `worst` receives the first out-of-reach flyer and its numbers.
    // Pass a_chaseCap computed BEFORE this call (Confidence::ChaseRadius takes this same
    // lock; calling it inside would nest the read lock, #23).
    struct ReachScan {
        int              reachable   = 0;
        int              unreachable = 0;
        RE::ActorHandle  worst;
        ReachRead        worstRead;
    };
    inline ReachScan ScanMeleeReach(RE::Actor* a_self, float a_chaseCap, float a_slack) {
        ReachScan out;
        if (!a_self) return out;
        auto* cc = a_self->GetActorRuntimeData().combatController;
        if (!cc || !cc->combatGroup) return out;
        const float selfH   = BodyHeight(a_self);
        const auto  selfPos = a_self->GetPosition();
        RE::BSReadLockGuard lk(cc->combatGroup->lock);
        for (const auto& t : cc->combatGroup->targets) {
            auto ptr = t.targetHandle.get();          // HOLD the NiPointer
            auto* foe = ptr.get();
            if (!foe || foe == a_self) continue;
            if (foe->IsDead() || foe->IsDisabled()) continue;
            if (!foe->IsHostileToActor(a_self)) continue;
            ReachRead rr;
            if (OutOfMeleeReach(a_self, foe, a_slack, &rr, selfH)) {
                if (out.unreachable++ == 0) { out.worst = foe->GetHandle(); out.worstRead = rr; }
                continue;
            }
            if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
            if (!foe->Is3DLoaded()) continue;
            if (selfPos.GetDistance(foe->GetPosition()) <= a_chaseCap) ++out.reachable;
        }
        return out;
    }
}
