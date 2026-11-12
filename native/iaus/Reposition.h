#pragma once
// iaus/Reposition.h -- batch I (T2): the PURE questions behind the three short moves IAUS asks Harbinger for besides the hide run:
//   H4   a mage's spell range (the distance he should fight at, from the spell in his hand) and the minimum under which he backs off
//   Q-T2-3  the Mercenary's back-off (two or more foes press him)
//   G18  the hidden step (hidden with no blow ready: close on a foe that does not detect him, quietly)
// Standard types only (iaus/Snapshot.h), so the off-line harness and the simulator read the very same answers as the game.
#include "iaus/Estimate.h"
#include "iaus/Snapshot.h"

#include <algorithm>
#include <cmath>

namespace MFO::Iaus::Reposition {

    // Candidate::form of the two families, so the engine side knows which H3 run to start (scheduler/IausSource.cpp passes it as the
    // act.disengage parameter; cast/Fire.cpp -> Concealment::Begin). 0 stays the hide run / flee of Family::Disengage. NOT FormIDs.
    inline constexpr std::uint32_t kBackOffForm = 1;
    inline constexpr std::uint32_t kStepForm = 2;

    // ── H4: the spell's range (marth: "Near the furthest range an equipped spell will work, but will move to range for touch or
    // cone spells automatically"; Docs/research/2026-11/class-build-plan.md H4) ──────────────────────────────────────────────
    //   touch                      the melee reach x0.8 (a touch spell is a swing's length)
    //   concentration (a cone)     the spell's range x0.8 (a stream that falls off, so inside the range, not at its edge)
    //   Self-delivered blast       its own area x0.8 (he stands in it: the foe must be inside)
    //   aimed / target actor       the furthest range at which it works x0.9
    //   target location            its range (never an IAUS candidate today: Placement is dropped)
    // `desired` = the distance to fight at (0 = the record gives none: no range is kept). `minimum` = the distance under which a foe
    // that has to close is too near for him to keep casting at it: only a spell that is cast from AFAR has one (an aimed or
    // target-actor spell that is not a concentration spell); a touch / cone / self-area spell WANTS the foe near. CHOSEN: 0.4 of the
    // desired distance (the sim tunes it).
    inline constexpr float kCloseSlack = 1.15f;      // the range veto's own slack (Catalogue.cpp RangeSpell): beyond desired x this he closes
    inline constexpr float kMinimumFrac = 0.4f;
    struct Range {
        float desired = 0.0f;
        float minimum = 0.0f;
        [[nodiscard]] bool Known() const { return desired > 0.0f; }
    };
    [[nodiscard]] inline Range SpellRange(const SpellEntry& a_e, float a_meleeReach) {
        Range r;
        const auto fin = [](float v) { return std::isfinite(v) && v > 0.0f ? v : 0.0f; };
        if (a_e.touch) { r.desired = 0.8f * fin(a_meleeReach); return r; }
        if (a_e.selfDelivery) { r.desired = 0.8f * fin(a_e.areaUnits); return r; }   // no area = no range to keep
        const float reach = fin(a_e.reach);
        if (!(reach > 0.0f)) return r;
        if (a_e.concentration) { r.desired = 0.8f * reach; return r; }
        r.desired = 0.9f * reach;
        r.minimum = kMinimumFrac * r.desired;
        return r;
    }

    // The entry of the spell in his hand (Snapshot::equippedSpell), an offense / control spell of the book; nullptr when none.
    [[nodiscard]] inline const SpellEntry* EquippedEntry(const Snapshot& a_s) {
        if (a_s.equippedSpell == 0) return nullptr;
        for (const SpellEntry& e : a_s.book.entries)
            if (e.source == SpellSource::Spell && e.form == a_s.equippedSpell &&
                (e.nature == Prune::Nature::Offense || e.nature == Prune::Nature::Control) && e.drop == Prune::Drop::Keep)
                return &e;
        return nullptr;
    }

    // His hands hold spells or nothing (no weapon drawn, or a staff): the SPELL PHASE of a mage or a hybrid.
    [[nodiscard]] inline bool SpellPhase(const Snapshot& a_s) {
        return a_s.drawn == PostureKind::None || a_s.drawn == PostureKind::Staff;
    }

    // The foe he fights the spell at: his current target if it is a listed foe, else the nearest one that is not fleeing or paralysed.
    [[nodiscard]] inline const Combatant* RangeFoe(const Snapshot& a_s) {
        const Combatant* near = nullptr;
        for (const Combatant& f : a_s.foes) {
            if (f.paralysed || f.fleeing || !std::isfinite(f.dist) || !(f.hpMax > 0.0f)) continue;
            if (a_s.currentTargetId != 0 && f.id == a_s.currentTargetId) return &f;
            if (!near || f.dist < near->dist) near = &f;
        }
        return near;
    }

    // The CLOSING half. RangeToHold: the foe the spell in his hand is cast at and R = the distance to hold it inside (his hands hold spells, a
    // casting class, a spell with a range, a foe). `far` = the foe stands beyond desired x kCloseSlack, which is when a NEW close is filed; a
    // standing close is refreshed on every lap RangeToHold answers true (the claim is a bound, "stay inside R", so inside it costs nothing).
    // Pure: the engine side (apmf/RangeApproach.cpp) files ch.24 with it.
    struct Close {
        std::uint32_t foe = 0;
        float         radius = 0.0f;
        bool          far = false;
    };
    [[nodiscard]] inline bool RangeToHold(const Snapshot& a_s, const ClassGate& a_g, Close& a_out) {
        if (!SpellPhase(a_s) || (a_g.resolved && !a_g.spellsOnClass)) return false;
        const SpellEntry* e = EquippedEntry(a_s);
        if (!e) return false;
        const Range r = SpellRange(*e, a_s.meleeReach);
        if (!r.Known()) return false;
        const Combatant* foe = RangeFoe(a_s);
        if (!foe) return false;
        a_out.foe = foe->id;
        a_out.radius = r.desired;
        a_out.far = foe->dist > r.desired * kCloseSlack;
        return true;
    }

    // ── The back-off need (Cons::BackOffNeed's input, 0..1; 0 = no reason) ───────────────────────────────────────────────────
    // A MAGE (spell phase, a casting class, a spell with a minimum in hand): a foe that has to close and has him as its target stands
    // inside the minimum. The need is how far inside (at the foe's feet = 1).
    [[nodiscard]] inline float MageBackOffNeed(const Snapshot& a_s, const ClassGate& a_g) {
        if (!SpellPhase(a_s) || (a_g.resolved && !a_g.spellsOnClass)) return 0.0f;
        const SpellEntry* e = EquippedEntry(a_s);
        if (!e) return 0.0f;
        const Range r = SpellRange(*e, a_s.meleeReach);
        if (!(r.minimum > 0.0f)) return 0.0f;
        float need = 0.0f;
        for (const Combatant& f : a_s.foes) {
            if (f.paralysed || f.fleeing || f.targetId == 0 || f.targetId != a_s.self.id || !Estimate::MustClose(f)) continue;
            if (!std::isfinite(f.dist) || !(f.dist < r.minimum)) continue;
            need = (std::max)(need, std::clamp(1.0f - f.dist / r.minimum, 0.0f, 1.0f));
        }
        return need;
    }

    // The MERCENARY (Q-T2-3: "backs off when two foes press, then darts in again"): foes that have him as their target and stand inside
    // striking distance. Two press him = 0.6, three 0.8, four or more 1.0 (CHOSEN, tuned in the sim). One foe is a duel: no back-off.
    inline constexpr int   kPressCount = 2;
    [[nodiscard]] inline int PressingFoes(const Snapshot& a_s) {
        const float strike = Estimate::StrikeDistance(a_s);
        int n = 0;
        for (const Combatant& f : a_s.foes) {
            if (f.paralysed || f.fleeing || f.targetId == 0 || f.targetId != a_s.self.id) continue;
            if (std::isfinite(f.dist) && f.dist <= strike) ++n;
        }
        return n;
    }
    [[nodiscard]] inline float PressedBackOffNeed(const Snapshot& a_s, const ClassGate& a_g) {
        if (!a_g.backOffPressed) return 0.0f;
        const int n = PressingFoes(a_s);
        if (n < kPressCount) return 0.0f;
        return (std::min)(1.0f, 0.6f + 0.2f * static_cast<float>(n - kPressCount));
    }
    // The larger of the two reasons (a Mercenary is never in the spell phase, a mage is never a Mercenary; the max is for a class
    // that is both one day).
    [[nodiscard]] inline float BackOffNeed(const Snapshot& a_s, const ClassGate& a_g) {
        return (std::max)(MageBackOffNeed(a_s, a_g), PressedBackOffNeed(a_s, a_g));
    }

    // ── G18: the hidden step ─────────────────────────────────────────────────────────────────────────────────────────────────
    // An unaware foe he could strike NOW: melee, inside the swing reach and not airborne beyond it; bow, in his own ray and inside the
    // chase cap. A caster's posture (None / Staff) never strikes. (Moved here from Generate.cpp so the gather and the generator ask the
    // same question; Generate's Ctx::StrikeReady calls it.)
    [[nodiscard]] inline bool StrikeReady(const Snapshot& a_s, const Combatant& a_foe) {
        if (a_foe.detectsMe != Tri::No || !std::isfinite(a_foe.dist)) return false;
        if (a_s.drawn == PostureKind::Melee || a_s.drawn == PostureKind::Silver)
            return !a_foe.outOfMeleeReach && a_foe.dist <= (std::max)(0.0f, Prune::Finite(a_s.meleeReach, 0.0f));
        if (a_s.drawn == PostureKind::Bow)
            return a_foe.sight == Sight::Visible && a_foe.dist <= (std::max)(0.0f, Prune::Finite(a_s.chaseCap, 0.0f));
        return false;
    }
    // The foe the step is for: the nearest foe the engine says does not detect him, while every listed foe is unaware and none of them
    // can be struck now (the hold row's own condition), with a weapon drawn that strikes (melee / silver / bow). nullptr = no step wanted.
    // The caller has already established that he SNEAKS and that the hold road exists (Snapshot::hideRoad, selfSneaking).
    [[nodiscard]] inline const Combatant* StepTarget(const Snapshot& a_s) {
        if (!(a_s.drawn == PostureKind::Melee || a_s.drawn == PostureKind::Silver || a_s.drawn == PostureKind::Bow)) return nullptr;
        if (!a_s.selfSneaking || !a_s.hideRoad) return nullptr;
        const Combatant* best = nullptr;
        for (const Combatant& f : a_s.foes) {
            if (!(f.hpMax > 0.0f)) continue;
            if (f.detectsMe != Tri::No) return nullptr;      // a foe has him, or the engine has no verdict: not hidden from every foe
            if (StrikeReady(a_s, f)) return nullptr;         // a blow is ready: the blow, not a step
            if (f.paralysed || f.fleeing || !std::isfinite(f.dist)) continue;
            if (!best || f.dist < best->dist) best = &f;
        }
        return best;
    }
}
