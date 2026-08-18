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

    // FORCED SELF-CAST — the UNIVERSAL direct trigger (Docs/SPEC-self-cast-forced.md).
    // Fires an authored `act.cast_self` (concentration OR fire-and-forget) as a
    // REAL cast on the follower himself, bypassing the alias/package machinery
    // entirely so it drives BOTH vanilla and package-locked custom-framework
    // followers (Lucien/Inigo, prio-80 quests that decline MFO's alias). NEVER
    // equips the spell: CastSpellImmediate (kInstant) applies the effect hands-
    // free (a held light spell let the follower's AI spam-cast it -> light-limit
    // CTD; the animation/equip scaffolding was removed, deferred to a polish
    // pass). Deducts the follower's real magicka (§5.3, §56). The caller re-fires
    // it every tick while the rule wins; SelfCastReconcile releases when it goes
    // false. Main-thread / worker context. Callers: CastOn (combat) + CastAuto +
    // Logistics (out-of-combat).
    //
    // Returns which happened THIS call, so a caller paced by its own scan can
    // tell a real apply from a mere channel-refresh (F3: a Refreshed tick must
    // NOT count as an action that suppresses the rules below it):
    //   Declined  — did not fire (off-AE / unaffordable / null); transparent.
    //   Refreshed — the rule is winning and the channel is kept alive, but no
    //               effect/magicka was applied this tick (fCastCooldown pacing).
    //   Applied   — the effect landed and magicka was spent this tick.
    enum class SelfCast : std::uint8_t { Declined, Refreshed, Applied };
    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell);

    // Per-tick reconcile for the forced self-cast channels: RELEASES a channel
    // when its rule goes stale (or the follower unloads) by dispelling any
    // lingering ward/buff effect so it cannot persist as a stuck gameplay effect.
    // NO time cap -- a long self-buff lives its full authored duration while the
    // rule keeps winning. There is NO equip/stow/animation to undo (never holds
    // the spell). Worker context; marshals the dispel to the main thread. Call
    // once per tick, right before Loadout::Tick.
    void SelfCastReconcile();

    // Drop every self-cast (and AUTO fan-out pacing) channel on revert/load. No
    // engine call -- the world is being replaced, and the self-cast holds no
    // equip/debt to undo (mirrors ClearForcedWeapons).
    void ClearSelfCasts();

    // AUTO TARGET INFERENCE for act.cast_target. The board's default "Auto" pick
    // (Subject::Self, no subject actor, no selector target) infers WHO from the
    // spell and fans the cast out: hostile -> every nearby enemy; beneficial ->
    // the WHOLE PARTY who needs it (every active follower + the player, the caster
    // included). Delivery type is NOT consulted -- MFO applies effects directly to
    // each target (ApplyEffectFromTo), so a self-delivery buff lands on allies too.
    // Per-cast magicka (reserve-floored, insufficient-skip), already-active guard,
    // one broadcast per fCastCooldown. Called from BOTH Fire (combat) and Logistics
    // (out of combat); out of combat the hostile branch finds no enemies and NoOps.
    // Returns Fired when at least one cast landed, else a transparent NoOp/decline.
    Outcome CastAuto(RE::Actor* a_follower, RE::FormID a_spellID);

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
    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance, bool a_condKnownFalse);

    // Revert/load: drop the session-scoped force-hold records. No engine call --
    // the world is being replaced (mirrors CombatStyle::ClearAll).
    void ClearForcedWeapons();

    // #76 force-hold co-save (kRecForcedWeapon/'FWPN'): persist the force-equip
    // locks so a load can clear stale ones. CoLoad releases (never repopulates) —
    // a session starts with no force-hold; the gambit re-forces if still true.
    void CoSaveForcedWeapons(SKSE::SerializationInterface* a_intfc);
    void CoLoadForcedWeapons(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);

}
