#pragma once
#include "PCH.h"
#include "Evaluator.h"

// Actuation. DESIGN.md §4.5 Tier A. The ONLY module that mutates actor state.
// Main thread only.

namespace MFO::Actuation {

    enum class Result : std::uint8_t {
        Fired,          // the action executed
        NoOp,           // deliberately nothing. A BLANK reason means truly nothing
                        // (act.wait, no rule matched); a non-blank reason is a
                        // decision MFO made and IS logged on transition.
        FailedSkill,    // the follower could not afford/execute it -> fall through
        FailedOther,
    };

    struct Outcome {
        Result      result = Result::NoOp;
        std::string reason;   // for the board / log when it did not fire

        // THE OUTCOME TAXONOMY (GAMBIT_FLOWS §2). `transparent` marks an outcome
        // the combat scan may fall PAST to try the rules below -- the dividing
        // line is activity vs state:
        //   TRANSPARENT: the rule's goal already holds (equip "already holding
        //     that category"), or it provably cannot run this tick (insufficient
        //     magicka, reserve floor, no weapon carried, no potion, cast
        //     cooldown, two-handed debounce, gear debt). A window, not a wall.
        //   OPAQUE (false): the outcome IS an ongoing activity that legitimately
        //     occupies the tick -- act.wait (the authored suppress idiom),
        //     attack "already on that target" (FFXII: lines below an active
        //     Attack never run), and the cast-grace hold (the wait IS the cast
        //     happening; firing lower rules mid-grace re-opens the §0.6
        //     confound). Fired outcomes are opaque by definition.
        // Defaults false so any path that forgets to tag stays a wall -- the
        // pre-fall-through behaviour, safe in direction.
        bool        transparent = false;
    };

    // Execute a chosen action on a follower. Returns what happened so the
    // scheduler can suppress on a real Fire and record a reason on a failure
    // (§5.3 -- a rule that could not run says why, it is not silent).
    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice);

    // ── #76: EQUIP FORCE-HOLD lifecycle ──────────────────────────────────────
    // While an equip-melee/ranged gambit's condition holds TRUE, the fired
    // weapon is FORCE-equipped (ActorEquipManager forceEquip=true = the engine's
    // prevent-removal lock) so the follower's own combat AI cannot auto-unequip
    // it to re-arm a spell -- the dagger<->spell thrash a both-hands caster still
    // showed after v1.0.62's "both hands satisfy the category" NoOp. The hold is
    // RELEASED when the gambit's condition goes FALSE (Scheduler reconciles every
    // combat tick), on combat end, on death, on dismissal, and on revert -- a
    // weapon left force-locked forever is a WORSE bug (the follower can never cast
    // again), so release is the critical correctness path. Gated by
    // bWeaponStyleControl; with it off, EquipWeapon does a plain EquipObject and
    // records nothing. Worker/main-thread only, same discipline as Fire.

    // Release the force-hold NOW: force-unequip the held weapon (forceEquip=true
    // on the UNequip clears the prevent-removal lock) and drop the record.
    // Idempotent (no record -> no-op). Combat end / death / dismissal.
    void ReleaseForcedWeapon(RE::Actor* a_follower);

    // Per-tick reconcile from the combat scan: KEEP the force-hold iff the
    // feature is on AND an equip gambit of the forced weapon's OWN category held
    // this tick (a_wantStance: 0=none/condition-false, 1=melee, 2=ranged);
    // otherwise RELEASE it. This is the gambit true->false lifecycle: a==0
    // (condition went false) or a category flip both release.
    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance);

    // Revert/load: drop the session-scoped force-hold records. No engine call --
    // the world is being replaced (mirrors CombatStyle::ClearAll).
    void ClearForcedWeapons();

}
