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

    // THE CAST-TARGET RESOLUTION LADDER (#68). Resolves WHO a cast_target row
    // aims at: a live selector target -> a named specific follower -> Subject
    // Player/NearestAlly -> the PLAYER fallback (a_outIsFallbackPlayer marks that
    // last rung so a caller's range check can WAIVE it). Public so BOTH the
    // combat Fire path and the out-of-combat Logistics cast_target path resolve a
    // manual target identically -- a logistics "Cast at foe/ally, Target=Nearest
    // ally" rule fires instead of being silently dropped. Main-thread / worker.
    RE::Actor* ResolveCastTarget(RE::Actor* a_follower, const Eval::Choice& a_choice,
                                 bool& a_outIsFallbackPlayer);

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

    // ON-TARGET DIRECT FORCE — CastSelfDirect generalized to a NON-self target
    // (player / ally / foe). The KNOWN-WORKING, package-lock-proof delivery for a
    // concentration cast: CastSpellImmediate straight onto the target + magicka
    // deduct, NO package, so it beats a package-locked custom follower's §4.6 alias
    // lock (Lucien 2F00591F's on-player heal that the package route [pkg]-DECLINED
    // every tick). Registers one per-follower stream; a CONCENTRATION spell
    // attaches its REAL effect once and SUSTAINS it with a synthesized duration
    // (the engine channels the authored magnitude itself -- all archetypes),
    // with a ~1 s kConcApplyPeriod beat deducting the per-second cost and
    // re-arming the effect's window; an FF spell keeps the fCastCooldown beat.
    // Time-bounded + released by TargetCastReconcile. Returns
    // Applied/Refreshed/Declined (same semantics as CastSelfDirect). LoS +
    // line-of-fire gate hostile offense on EVERY apply. Callers: Logistics OOC
    // cast dispatch AND combat ConcentrationCast -- BOTH primary; concentration
    // delivery touches no package anywhere. A self target is refused (use
    // CastSelfDirect). Worker-serial state; the engine apply posts to main.
    SelfCast CastTargetDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                              RE::Actor* a_target);

    // Per-tick reconcile for the forced self-cast channels: RELEASES a channel
    // when its rule goes stale (or the follower unloads) by dispelling any
    // lingering ward/buff effect so it cannot persist as a stuck gameplay effect.
    // NO time cap -- a long self-buff lives its full authored duration while the
    // rule keeps winning. There is NO equip/stow/animation to undo (never holds
    // the spell). Worker context; marshals the dispel to the main thread. Call
    // once per tick, right before Loadout::Tick.
    void SelfCastReconcile();

    // Per-tick reconcile for the ON-TARGET direct-force streams (CastTargetDirect):
    // RELEASES a stream when its rule goes stale / the follower or target unloads /
    // the per-kind time cap elapses (hostile 1-4s, heal 6s, utility 4s). DISPELS only
    // a lingering STICKY buff (ward/fortify) off the target -- a heal/damage is
    // release-only and re-streams uninterrupted. Call once per tick, beside
    // SelfCastReconcile. Worker context; marshals any dispel to the main thread.
    void TargetCastReconcile();

    // Drop every self-cast (and AUTO fan-out pacing) channel on revert/load. No
    // engine call -- the world is being replaced, and the self-cast holds no
    // equip/debt to undo (mirrors ClearForcedWeapons).
    void ClearSelfCasts();

    // DELIVERY-AWARE EFFECT CASTER (marth: "when a gambit is set, always read the
    // SPEL for this data"). A SELF-delivery spell's effect ALWAYS lands on the
    // magic-caster's OWNER, never the TESObjectREFR passed to CastSpellImmediate --
    // so casting a Self spell FROM the follower heals the FOLLOWER, and a "heal the
    // player/ally" gambit drains magicka while the intended target's HP stays flat
    // (deck 2026-08-19: Mysticism's Fast Healing 0002F3B8 is Concentration + SELF
    // delivery; force-cast "at" the player it healed Lucien, never the player, and
    // the concentration sustain then searched the PLAYER's effect list, never found
    // the effect -- it was on Lucien -- and re-attached every beat). To place a
    // Self-delivery effect on a DIFFERENT actor, THAT actor must be the magic caster
    // (it self-casts); the follower stays the blame actor and still pays the real
    // magicka. Non-Self deliveries (Aimed/TargetActor/Touch) place the effect on the
    // passed target from the follower's own caster, unchanged. Returns the actor
    // whose GetMagicCaster(kInstant) must perform the cast so the effect lands on
    // a_target. Reads only the SPEL's Delivery; no engine mutation -- any thread.
    RE::Actor* EffectCasterFor(RE::Actor* a_follower, RE::Actor* a_target,
                               RE::SpellItem* a_spell);

    // RE-ATTRIBUTE a freshly-applied effect's CASTER to the follower. When a
    // SELF-delivery spell is routed through the TARGET's own magic caster (so the
    // effect lands on the intended target, EffectCasterFor), the engine created
    // the ActiveEffect(s) with the TARGET as caster -- so it would channel the
    // TARGET's effective magnitude/rate (the target's skill/perks), not the
    // follower's. The follower is CONCEPTUALLY the caster, so re-point every fresh
    // ActiveEffect of a_spell on a_target back to a_follower: the engine then
    // applies the FOLLOWER's perks/effectiveness at each application, for EVERY
    // archetype (heal, ward, flesh/armor, waterbreathing, invisibility, muffle,
    // fear/frenzy, damage/DoT, any buff) and EVERY effect of a multi-effect spell.
    // NO magnitude math, NO per-effect override (which would collapse a multi-
    // effect spell) -- pure caster attribution, archetype-agnostic. Reuses the
    // sustain machinery unchanged: SustainConcentrationEffect re-arms the SAME
    // re-attributed AE each beat, so the follower's rate persists for the whole
    // stream, not just the first beat. MAIN THREAD only (walks the live AE list).
    // Idempotent + a no-op for a normal cast (follower already the caster) and a
    // genuine self-cast (target == follower), so callers may gate on the Self-
    // delivery re-route or call unconditionally.
    void ReattributeEffectCaster(RE::Actor* a_target, RE::SpellItem* a_spell,
                                 RE::Actor* a_follower);

    // AUTO TARGET INFERENCE for act.cast_target. The board's default "Auto" pick
    // (Subject::Self, no subject actor, no selector target) infers WHO from the
    // spell and fans the cast out: hostile -> every nearby enemy; beneficial ->
    // the WHOLE PARTY who needs it (every active follower + the player, the caster
    // included). MFO chooses WHO receives each cast by spell nature, not the SPEL's
    // Delivery -- but HOW the effect is placed still honours Delivery: a SELF-
    // delivery buff is applied by having each recipient self-cast it (EffectCasterFor),
    // so it lands on allies too instead of only the follower.
    // Per-cast magicka (reserve-floored, insufficient-skip), already-active guard,
    // one broadcast per fCastCooldown. Called from BOTH Fire (combat) and Logistics
    // (out of combat); out of combat the hostile branch finds no enemies and NoOps.
    // Returns Fired when at least one cast landed, else a transparent NoOp/decline.
    //
    // #2: a_healThreshold narrows the BENEFICIAL HEAL fan to only allies whose
    // health matches the FIRING gambit's own condition, instead of the old blanket
    // "everyone below full". Fire passes the firing rule's health-below threshold
    // (a fraction in [0,1]) when its condition carries one; otherwise it stays 1.0
    // (heal anyone below full -- the prior behaviour), which the Logistics caller
    // also keeps via the default. It is a HEAL-only narrowing: buffs with no status
    // condition (Candlelight) fan to the whole party regardless.
    Outcome CastAuto(RE::Actor* a_follower, RE::FormID a_spellID, float a_healThreshold = 1.0f);

    // (CONCENTRATION delivery is DIRECT FORCE everywhere -- Docs/CAST-DELIVERY.md.
    // COMBAT: CastOn's concentration fork -> ConcentrationCast -> CastTargetDirect
    // (self -> CastSelfDirect); the v1.0.58-65 package stream is REMOVED -- it
    // §4.6-declined every tick for package-locked custom followers while the
    // consent hooks denied their own AI, a total lockout. OUT OF COMBAT: the
    // Logistics dispatch calls CastTargetDirect directly. Same channel registry,
    // same 1 s concentration beat, same bounds, both contexts.)

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
