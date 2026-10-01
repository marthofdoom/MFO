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

    // ── HAND INDICES ────────────────────────────────────────────────────────
    // 0 = left, 1 = right, everywhere in the Actuation family (the per-hand cast
    // lock's two slots, CastInFlightOnHand's `a_hand`). MOVED here from
    // Actuation_internal.h (2026-09-09) with its enumerators and values unchanged; named HandIndex (MFO-B10) --
    // it now has to be nameable from ComposedCast.cpp, which is NOT one of the
    // three Actuation TUs and so may not include that internal header. The same
    // numbering as RE::Actor::SlotTypes::kLeftHand/kRightHand, deliberately: the
    // array index is what CastInFlightOnHand ultimately reads.
    enum HandIndex : std::size_t { kHandLeft = 0, kHandRight = 1, kHandCount = 2 };

    // ── "THE ENGINE STARTED THIS CAST AND HAS NOT FINISHED IT" ───────────────
    // THE one definition of in-flight in this codebase (F8, 2026-09-08). True
    // when the follower's own MagicCaster for `a_hand` is in a live cast state
    // AND the spell it currently has selected is `a_spell` -- or `a_proxy`, the
    // delivery-flip form APMF minted for that claim, since a proxied claim never
    // has the original spell selected. Read straight off the actor's already-built
    // caster array: no virtual call, no allocation, safe from the job worker.
    // Full reasoning, symbol verification and the state table live on the
    // DEFINITION in Actuation_Hands.cpp -- read it there, it was not copied here.
    //
    // PUBLIC since 2026-09-09 (it was file-local to Actuation_Hands.cpp): the heal
    // claim path lives in ComposedCast.cpp and needs exactly this question
    // answered before it releases an incumbent claim. A SECOND liveness test was
    // the alternative and is explicitly refused -- one definition, one answer.
    bool CastInFlightOnHand(RE::Actor* a_follower, std::size_t a_hand,
                            RE::FormID a_spell, RE::FormID a_proxy);

    // CHARGED AND WAITING (field 2026-09-30): CastInFlightOnHand's own reads (spell or
    // proxy selected, caster built) AND the caster is in MagicCaster::State::kReady,
    // the fork's state 3, which Harbinger's census names "Charged" (CastObserve.cpp
    // CasterStateName). A spell the engine has fully charged and is HOLDING, not yet
    // casting and not concluding. An offense charge held on an unsighted foe sits
    // here for as long as the claim stands, and CastInFlightOnHand alone calls that
    // "in flight". Racy plain loads, same as CastInFlightOnHand (definition in
    // cast/Hands.cpp). Worker-safe.
    bool CastChargedWaitingOnHand(RE::Actor* a_follower, std::size_t a_hand,
                                  RE::FormID a_spell, RE::FormID a_proxy);

    // WHEN THE CAST LOCK ON `a_hand` CLAIMED `a_spell` (MFO-B190): the lock's
    // `lastSeen`, which a refresh in flight and a heal repair lap never re-stamp, so
    // it is the claim's start. Returns the clock epoch (time_point{}) when the hand
    // has no lock for that spell (the direct roads hold none). Worker-serial, no
    // lock, like every cast-lock reader (#4); ComposedCast's [cfc] watch is the
    // caller and runs on the same worker.
    std::chrono::steady_clock::time_point CastLockClaimStamp(RE::FormID a_follower, std::size_t a_hand,
                                                             RE::FormID a_spell);

    // Drop the LEFT cast lock IF it names `a_spell` (MFO-B179): the twin of
    // ReleaseOwnHealClaim's own lock clear for a caller that has already ended the
    // claim through ComposedCast::End and cannot reach the ledger. Idempotent. The
    // right hand's lock is never touched. Worker-serial (#4).
    void ClearLeftCastLockIf(RE::FormID a_follower, RE::FormID a_spell);

    // End a_follower's direct HEAL streams (MFO-B176): the self stream and the
    // on-target stream, each only when its spell is Heal-kind. ComposedCast::
    // Try calls it when a heal claim is minted (Claimed return, MFO-B192), so one actor never
    // runs a direct heal stream beside a heal claim ("ONE ROAD PER ACTOR"; the
    // claim -> direct half is ChooseHealRoad's own End). It is the streams' own
    // switch end: settle, then the dispel + kInstant interrupt posted to the MAIN
    // thread (FormIDs only, re-resolved there), then the registry entry is dropped.
    // a_why names the switch in the [heal] line. Worker-serial (#4). A no-op when
    // no heal stream stands.
    void EndDirectHealStreams(RE::FormID a_follower, const char* a_why);

    // Is a_follower's LEFT heal lock in its stream-cap re-stream gap (MFO-B177)? The
    // cap released the heal claim and the rule re-claims next lap; the lock keeps
    // the heal's rank until then (CastLock::restreamAt). ComposedCast::HealTakesLeft
    // reads it so MFO's non-gambit left automation keeps deferring across that one
    // lap. Worker-serial (#4).
    bool HealRestreamGap(RE::FormID a_follower);

    // THE CAST-TARGET RESOLUTION LADDER (#68). Resolves WHO a cast_target row
    // aims at: a live selector target -> a named specific follower -> Subject
    // Player/NearestAlly -> the PLAYER fallback (a_outIsFallbackPlayer marks that
    // last rung so a caller's range check can WAIVE it). Public so BOTH the
    // combat Fire path and the out-of-combat Logistics cast_target path resolve a
    // manual target identically -- a logistics "Cast on target, Target=Ally: Nearest
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
    //   Held      — the APMF heal-claim path (ComposedCast::Try) HELD this spell
    //               off: a DIFFERENT spell's live claim owns the follower's
    //               single heal slot, so nothing was claimed, nothing was
    //               applied, and no magicka was spent. TRANSPARENT, and it must
    //               never be reported as Fired or re-stamp the Task-2 hand lock
    //               with this spell (Fable SEV-2, 2026-09-06 -- it used to come
    //               back as Applied and did exactly both). See
    //               ComposedCast::TryResult / ComposedCast::HeldOffBy.
    // a_stopPct (default 0 = unused): a whole percent 1..100 of the target's
    // (here, the follower's own) PERMANENT actor value at which a claimed
    // APMF-driven concentration heal's native channel should stop -- forwarded
    // to ComposedCast::Try -> APMFBridge::ClaimHealCast (APMF_API::MakeStopPct).
    // ONLY meaningful on the ComposedCast (APMF-claimed) path; the kInstant
    // fallback below is unaffected -- its own re-selection each tick already
    // stops targeting an ally once above the calling rule's threshold. Callers
    // with no numeric threshold in scope pass the default (matches the prior
    // full-restoration behaviour byte-for-byte).
    enum class SelfCast : std::uint8_t { Declined, Refreshed, Applied, Held };
    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                            std::uint32_t a_stopPct = 0);

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
    // Applied/Refreshed/Declined/Held (same semantics as CastSelfDirect). LoS +
    // line-of-fire gate hostile offense on EVERY apply. Callers: Logistics OOC
    // cast dispatch AND combat ConcentrationCast -- BOTH primary; concentration
    // delivery touches no package anywhere. A self target is refused (use
    // CastSelfDirect). Worker-serial state; the engine apply posts to main.
    // a_stopPct: see CastSelfDirect's doc above -- same forwarding, same
    // default, but here it is a percent of a_target's permanent actor value
    // (the ally being healed), not the follower's own.
    SelfCast CastTargetDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                              RE::Actor* a_target, std::uint32_t a_stopPct = 0);

    // CAST-ROAD SELECTION: IS THIS A RESTORATION CAST? (fix/mfo-combat-restoration-
    // direct, 2026-09-21 -- Deck 2026-09-21, Jesper 750012C6.) A RESTORATION cast is
    // any spell that does NOT harm a foe (CasterConsent::ClassifySpell != Offense)
    // AND either restores Health (a beneficial Health effect -- the same read
    // ClassifySpell's Heal kind and CastAuto's SpellHealsHealth make) OR belongs to
    // the Restoration school (any effect whose associatedSkill is kRestoration:
    // wards, Turn-Undead-free utility). A Restoration-school spell that harms a foe
    // (Sun Fire, Turn Undead) is an OFFENSE cast and is NOT restoration here --
    // offensive spells keep the AI-fired road.
    //
    // WHY A ROAD IS CHOSEN BY THIS: on the COMBAT table a restoration cast takes
    // the DIRECT road (CastSelfDirect / CastTargetDirect: CastSpellImmediate,
    // MainThread::Post'd, the bounded delivery the OOC logistics heal has always
    // used -- 22/22 delivered that session) and NEVER the ch.8b AI-fired heal claim
    // (ComposedCast::Try). The AI-fired road holds one hand with a claim naming
    // the gambit's spell while the engine's kMultipleCast re-deliberation keeps
    // selecting a DIFFERENT heal (Healing Hands against a Fast Healing claim ->
    // `t2c CheckCast DENIED`, 3x InterruptCast, `[cfc] claim live 7607 ms`), and
    // the idle-hand floor closes the OTHER hand while that claim stands -- so the
    // dagger cannot be armed either and the follower stands frozen through the
    // fight. Read by CastOn (Actuation.cpp), CastSelfDirect and CastTargetDirect.
    // Effect-list read only, no vfunc, worker-safe.
    bool IsRestorationSpell(RE::SpellItem* a_spell);

    // Does the ON-TARGET direct-force registry (CastTargetDirect's own stream
    // table) hold a LIVE stream for exactly this (follower, spell, target)? This is
    // the honest "which road delivered it" read for a CastTargetDirect Applied:
    // the direct road always creates/refreshes this entry, an APMF claim never
    // touches it. Logistics.cpp's OOC cast label reads it instead of inferring
    // the road from heal-claim liveness (which mislabelled a direct Healing Hands
    // as "APMF claimed" because ANOTHER rule's claim was live on the follower).
    // Worker-serial, same registry discipline as CastTargetDirect.
    bool TargetStreamLive(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target);

    // ── CAN THIS ACTOR ACT? (fix/mfo-can-act, field 0928c, 2026-09-29) ──────────
    // Field 2026-09-28 21:06-21:07 (MFO.log.1): Jesper, BLEEDING OUT (HP ~0, stamina
    // ~0, magicka full), fired rule 0's instant Close Greater Wounds on himself,
    // Serana and the player, and streamed Fast Healing at a downed Serana, reviving
    // both within seconds. No cast or drink road asked whether the CASTER could act.
    // marth: healing a downed ally is fine, from a caster who CAN act.
    //
    // Returns nullptr when a_actor can act, else a short static reason. Every read is
    // a PLAIN MEMBER LOAD (no vfunc, no relocated call), so it is safe on the job
    // worker, inside the combat group's lock and on the main thread alike. Verified
    // against the fork at fde0f3ae (registry baseline 038638a) and BOTH unpacked
    // executables:
    //  * LIFE STATE = ActorState1 bits 21-24 (include/RE/A/ActorState.h:114, reached
    //    through AsActorState(): the ActorState base 0xB8 SE / 0xC0 1.6.629+). The
    //    engine reads the same field: 1.5.97 `[actor+0xC0] >> 21 & 0xF` compared to
    //    2 / 3 / 6 / 8 (26 sites), 1.6.1170 `[actor+0xC8] & 0x1E00000` compared to
    //    0x400000 / 0x600000 / 0xC00000 / 0xE00000 / 0x1000000 (144 sites) -- dead,
    //    unconscious, restrained, essential-down, bleedout. Only the named
    //    down states are "cannot act" (an ALLOWLIST, fix/mfo-lifestate; kAlive and
    //    kReanimate (a raised thrall acts) act, and so does an unnamed value): dying, dead, unconscious,
    //    restrained, recycle, essential-down, bleedout (ActorState::IsBleedingOut is
    //    exactly the last two).
    //  * KNOCK STATE = ActorState1 bits 25-27 (:115). Anything but kNormal (queued,
    //    explode / out + their lead-ins, down, get-up, wait-for-task-queue) is a
    //    knockdown or ragdoll in progress. 1.5.97 `>> 25 & 7` (4 sites), 1.6.1170
    //    `& 0xE000000` (6 sites). Actor::IsInRagdollState is NOT used: it is a
    //    relocated engine call (RELOCATION_ID 36492 / 37491, Actor.cpp:813), not a
    //    field read, and would need its own self-check row.
    //  * PARALYSED = Actor boolBits bit 31 kParalyzed (Actor.h:186; runtime data
    //    0xE0 SE / 0xE8 AE, Actor.h:699). Both executables read bit 31 of exactly
    //    that dword (30 sites each: `mov r,[r+0xE0|0xE8]; shr r,0x1f`).
    //  * KILL MOVE = Actor boolFlags bit 14 kIsInKillMove through the fork's own
    //    inline Actor::IsInKillMove() (Actor.h:585; boolFlags 0x1FC SE / 0x204 AE,
    //    7 `shr r,0xe` sites each).
    // Not in it: stagger (a stagger does not stop a cast in the engine either) and
    // disabled / not loaded (every caller already handles those).
    // a_countPendingKnock (review C3 on 91b17a3): false = a knock that is only
    // QUEUED (kQueued / kWaitForTaskQueue, momentary, may never become a knockdown)
    // does not count. The stream reconciles pass false, so a queued knock may skip a
    // lap (the scan / apply gates use the default) but never dispels, interrupts or
    // frees a live heal stream.
    // fix/mfo-lifestate: an UNNAMED life state (raw value outside kAlive..kBleedout, 0-8)
    // is NOT a down state. The give idle and the lockpick idle flip a follower to one
    // (hp 100%), and 1b3af00's `default: "not alive"` stopped deposits and picks. Logs
    // the raw value ONCE per actor per value (cast/CanAct.cpp), thread-safe.
    void NoteUnknownLifeState(RE::FormID a_actor, std::uint32_t a_raw);
    // The ALLOWLIST: only the named down states below block. kRecycle (5) is the engine's
    // dead-body recycle, so it blocks too. Unknown values pass (see above).
    inline const char* CannotActReason(const RE::Actor* a_actor, bool a_countPendingKnock = true) {
        if (!a_actor) return "gone";
        if (const auto* st = a_actor->AsActorState()) {
            switch (st->GetLifeState()) {
            case RE::ACTOR_LIFE_STATE::kAlive:
            case RE::ACTOR_LIFE_STATE::kReanimate:      break;
            case RE::ACTOR_LIFE_STATE::kBleedout:       return "bleedout";
            case RE::ACTOR_LIFE_STATE::kEssentialDown:  return "essential down";
            case RE::ACTOR_LIFE_STATE::kUnconcious:     return "unconscious";
            case RE::ACTOR_LIFE_STATE::kRestrained:     return "restrained";
            case RE::ACTOR_LIFE_STATE::kDying:          return "dying";
            case RE::ACTOR_LIFE_STATE::kDead:           return "dead";
            case RE::ACTOR_LIFE_STATE::kRecycle:        return "recycle";
            default:
                NoteUnknownLifeState(a_actor->GetFormID(), static_cast<std::uint32_t>(st->GetLifeState()));
                break;
            }
            switch (st->GetKnockState()) {
            case RE::KNOCK_STATE_ENUM::kNormal:
                break;
            case RE::KNOCK_STATE_ENUM::kQueued:
            case RE::KNOCK_STATE_ENUM::kWaitForTaskQueue:
                if (a_countPendingKnock) return "knock queued";
                break;
            default:
                return "knocked down";
            }
        }
        if (a_actor->GetActorRuntimeData().boolBits.all(RE::Actor::BOOL_BITS::kParalyzed))
            return "paralysed";
        if (a_actor->IsInKillMove()) return "kill move";
        return nullptr;
    }
    inline bool CanAct(const RE::Actor* a_actor) { return CannotActReason(a_actor) == nullptr; }

    // MAIN-THREAD REFUSAL LOG. A cast / drink apply that re-checked CanAct on the main
    // thread (the state can change between the worker's decision and the post) and
    // found the caster down logs ONCE per caster until he can act again (NoteLifeState
    // re-arms it). No magicka is spent on a refused apply. Any thread (mutex).
    void NoteRefusedApply(RE::FormID a_caster, const char* a_road, const char* a_why,
                          RE::FormID a_spell, RE::FormID a_target);

    // [bleed] -- a passive, transition-only log of a follower's LIFE STATE (down /
    // up), with HP, and on the way up which MFO heal (or MFO potion) landed on him
    // last. Worker, called once per own service from the Scheduler. Re-arms the
    // NoteRefusedApply line when he can act again. NoteHealLanded records an MFO
    // heal the moment it is applied (main thread, or the worker for a potion; mutex).
    void NoteLifeState(RE::Actor* a_follower);
    void NoteHealLanded(RE::FormID a_target, RE::FormID a_caster, RE::FormID a_spell);

    // ── NORMAL REACH FOR A HEAL ON ANOTHER ACTOR (marth 2026-09-29) ─────────────
    // A heal on an ally reaches only as far as the player's own cast of that spell
    // would, with line of sight -- not the flat fSharedRadius (3000 u) with none.
    // The reach, from the spell's own record (plain field reads, worker-safe):
    //  * Aimed / Target Actor / Target Location: the spell's own Range (SPIT) when
    //    set; else the projectile range of its effects (the longest
    //    EffectSetting::data.projectileBase->data.range) -- vanilla and Mysticism
    //    Healing Hands / Heal Other carry Range 0 and projectile 0x12FDC, range 10000;
    //    else no cap of its own (the candidate radius still applies).
    //  * Self / Touch (marth 2026-09-29, option B): the reach of HEAL OTHER
    //    (Skyrim.esm 0x00012FD2), the aimed ally heal, by the same rule -- Range 0,
    //    projectile range 10000 in Skyrim.esm and in Mysticism's override. A Self
    //    spell reaches nobody but its caster when the player casts it; MFO places Self
    //    heals on allies (ConcProxy, AUTO) and they reach as far as Heal Other would.
    //    A self-heal on the caster himself is never reach-checked.
    // HealReach returns the reach in units (a very large number = no cap of its own).
    float HealReach(RE::SpellItem* a_spell);
    // WORKER: in reach = distance <= HealReach AND the Sightline cache does not say
    // Occluded (Unknown passes: the fail-open every other LoS gate here takes; the
    // caller Want()s the pair). Always true for a_target == a_caster.
    bool HealInReach(RE::Actor* a_caster, RE::Actor* a_target, RE::SpellItem* a_spell);
    // Is the heal recipient beyond the spell's reach (HealReach)? REACH ONLY: line of sight
    // never drops a standing claim's recipient (marth 2026-09-30b). Declared here so
    // Evaluator's PickAlly applies the same incumbent test; defined in cast/Hands.cpp.
    bool HealRecipientUnreachable(RE::Actor* a_follower, RE::Actor* a_victim, RE::SpellItem* a_spell);
    // MAIN THREAD, right before an apply: true = REFUSE this heal on a_target (the
    // caster is beyond HealReach or Sightline::MeasureNow, a synchronous LoS measure,
    // says Occluded); logs the refusal (deduped per caster/target, 5 s). False for a
    // spell that does not restore Health (HealsHealth) and for a_target == a_caster.
    bool RefuseHealApplyOnMain(RE::Actor* a_caster, RE::Actor* a_target,
                               RE::SpellItem* a_spell, const char* a_road);
    // Does a_spell restore Health (any beneficial Health effect)? The same read
    // CastAuto's heal gate makes (SpellHealsHealth), public for logistics.
    bool HealsHealth(RE::SpellItem* a_spell);

    // ── [heal-obs] (feat/mfo-animheal-p0, phase 0a) ─────────────────────────────
    // A passive, rate-limited observation of ONE heal apply, so "denied but logged as
    // applied" cannot hide (principle 7). Called on the MAIN thread by every direct
    // heal apply right after its CastSpellImmediate: it records the road, caster,
    // recipient, distance, the Sightline verdict, the recipient's HP BEFORE the cast
    // (a_hpBefore, read by the caller before casting), whether the effect was present
    // right after the cast (a_attach), and whether MFO held a ch.8b cast claim on the
    // caster (Harbinger's pre-d41ed43 actor-wide fallback). About 1 s later the worker
    // sweep (HealObsSweep) posts a main-thread read of the recipient's HP, whether the
    // effect is STILL present, and -- for a concentration stream -- whether the
    // caster's instant channel is still running, then logs ONE line. At most one
    // observation per (caster, recipient) every 3 s. No behaviour change.
    enum class HealAttach : std::uint8_t { Present, Absent, Instant };
    void HealObsNote(RE::Actor* a_caster, RE::Actor* a_target, RE::SpellItem* a_spell,
                     RE::SpellItem* a_castForm, const char* a_road, float a_hpBefore,
                     HealAttach a_attach);

    // [heal-obs] ON THE CLAIM ROAD (animheal phase 2). There is no MFO apply to
    // observe there: the follower's own AI casts. So the observation starts at the
    // engine's own FIRE of the claimed heal (Diagnostics.cpp's TESSpellCastEvent sink,
    // on the job worker, right after ComposedCast::NoteObservedCast): when the fired
    // form is the standing heal claim's spell or its delivery-flip proxy, one
    // HealObsNote with road=claim is posted to the main thread, recipient = the
    // claim's own target (0 = the caster). The "HP before" is the recipient's health
    // on the claim's last lap before the fire (HealObsClaimLap, stamped by CastOn's
    // per-lap recipient check), because by the time the event reaches MFO an instant
    // heal has already landed. Worker-serial callers; the shared state is under the
    // [heal-obs] mutex.
    void HealObsClaimLap(RE::FormID a_caster, RE::FormID a_recipient, float a_hp);
    void HealObsNoteClaimFire(RE::FormID a_caster, RE::FormID a_firedForm);

    // MFO's own ConcProxy forms (the delivery-flipped 0xFF copies, minted at runtime,
    // kept for the session, re-minted after a load): every one minted so far. ANY
    // thread (an atomic mirror). Published on the follower's ch.8 allow-list so
    // Harbinger's instant-caster gate admits MFO's own direct concentration heals
    // (feat/mfo-animheal-p0, 1m(c)).
    std::vector<RE::FormID> ConcProxyForms();

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
    //
    // #2: a_healThreshold narrows the BENEFICIAL HEAL fan to only allies whose
    // health matches the FIRING gambit's own condition, instead of the old blanket
    // "everyone below full". Fire passes the firing rule's health-below threshold
    // (a fraction in [0,1]) when its condition carries one; otherwise it stays 1.0
    // (heal anyone below full -- the prior behaviour), which the Logistics caller
    // also keeps via the default. It is a HEAL-only narrowing: buffs with no status
    // condition (Candlelight) fan to the whole party regardless.
    Outcome CastAuto(RE::Actor* a_follower, RE::FormID a_spellID, float a_healThreshold = 1.0f);

    // SUMMON LIVENESS (v1.1.1). A conjured familiar/atronach or a reanimated corpse
    // is a separate COMMANDED ACTOR, not a caster-side magic effect, so the OOC
    // cast routes' already-active guards (which read a magic effect on the cast
    // TARGET) never see it: the recast suppression never arms and a "cast [summon]"
    // gambit re-summons every cadence (marth, field). Returns true when a_caster
    // already commands a LIVE summon created by a_spell -- scanned caster-side via
    // the SummonCreatureEffect/ReanimateEffect commandedActor, PER-SPELL and keyed
    // on the live actor (a killed/expired/despawned summon frees an immediate
    // recast). False for every non-summon spell (no summon/reanimate archetype), so
    // candlelight/buff/heal pacing is untouched. Worker/main context, list read only.
    bool CasterHasLiveSummon(RE::Actor* a_caster, RE::SpellItem* a_spell);

    // SUMMON = ONE-SHOT CONJURE (fix/mfo-summon-oneshot, deck 2026-09-24). True
    // when any effect of a_spell has the kSummonCreature archetype (bound weapons
    // and Reanimate never match).
    bool IsSummonSpell(RE::SpellItem* a_spell);
    // THE single summon path for every target setting (self / player / foe /
    // AUTO) on both tables: the combat Fire dispatch and the Logistics OOC
    // service call it before any other cast road. WORKER side: competence +
    // magicka gates, the main thread's last verdict (fresh 1 s), a 0.5 s post
    // throttle, then ONE MainThread::Post. MAIN side (every engine read lives
    // there): skip while THIS spell's creature is alive (per spell) or still
    // APPEARING (engine effect younger than fMagicSummonMaxAppearTime with no
    // resolved handle; fallback floor = that + 1 s after our own cast), skip
    // when listed commanded actors (raw count, as the engine) + other summons
    // appearing >= the caster's summon limit (iMaxSummonedCreatures +
    // kModCommandedActorLimit perks, round half-up, exactly as the engine; not
    // when the engine's skip-cap flag is set, read behind its self-check rows), else cast ONCE by the
    // caster on the direct road (CastSpellImmediate kInstant, magicka deducted).
    // Never a self/target stream, no hand lock, so no MFO reconcile, combat-end
    // or cleanup path ends the summon. ALWAYS returns a transparent result: the
    // main thread's [summon] line says whether it cast. WORKER only.
    Outcome CastSummonOnce(RE::Actor* a_follower, RE::SpellItem* a_spell, int a_rule,
                           const char* a_table, std::string_view a_target);

    // (CONCENTRATION delivery is DIRECT FORCE everywhere -- Docs/CAST-DELIVERY.md.
    // COMBAT: CastOn's concentration fork -> ConcentrationCast -> CastTargetDirect
    // (self -> CastSelfDirect); the v1.0.58-65 package stream is REMOVED -- it
    // §4.6-declined every tick for package-locked custom followers while the
    // consent hooks denied their own AI, a total lockout. OUT OF COMBAT: the
    // Logistics dispatch calls CastTargetDirect directly. Same channel registry,
    // same 1 s concentration beat, same bounds, both contexts.)

    // ── T#76: EQUIP FORCE-HOLD lifecycle ──────────────────────────────────────
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
    //
    // a_standDown -- "NOBODY EVER SEES AN UNARMED FOLLOWER" (marth 2026-09-22:
    // *"a fix so that no one ever sees an unarmed follower, they sheathe weapons.
    // Not unequip."*). The force-unequip is KEPT and is not removed, but NOT for the
    // reason this comment first gave: "a plain unequip is REFUSED against a forced
    // item" is FALSE (corrected 2026-09-22 from disassembly -- `UnequipObject`'s
    // dispatcher clears `ExtraCannotWear` unconditionally before it reads the force
    // byte; see the call site in Actuation.cpp and Docs/ENGINE_NOTES.md). It is kept
    // because it is harmless, because every release path in this file passes the same
    // shape, and because the lock must be gone before the AI may re-arm. What an
    // UNEQUIPPED weapon costs is VISIBILITY: it stops being drawn
    // on the body at all, where a SHEATHED one is still worn on the hip/back. That is
    // what the field saw ("weapons vanishing and returning", Cicero, three times in
    // one dragon fight). So on a STAND-DOWN release -- the fight is over, the hold is
    // simply finished, and no specific item has to leave the hand -- the same
    // weapon(s) are RE-EQUIPPED non-forced (the AI is free to swap them from then on)
    // and the follower is sheathed. The lock is still cleared; only the empty-handed
    // end state is not.
    //
    // Pass false (the default, and every pre-existing call site's behaviour) when the
    // item genuinely has to leave the hand: death/disable teardown, a category flip
    // mid-fight, the kill switch, dismissal.
    void ReleaseForcedWeapon(RE::Actor* a_follower, bool a_standDown = false);

    // The ledger as it stands NOW for a_follower: {right, left} hold FormIDs, 0 =
    // no hold in that hand (ABI v9, Fable F1 on 7857446). Read under g_forcedMx,
    // any worker-serial reader. THE OOC declaration road's source for the
    // scope: Logistics::ServiceFollower runs on ANY out-of-combat service (the
    // T#76 two-tick debounce guards only ReleaseForcedWeapon), so a one-tick
    // IsInCombat flap with holds passed as 0/0 would declare `owned=Armor`, drop
    // the hand entries, and leave a STANDING hold unowned for the rest of the
    // fight (nothing re-declares until the next equip event). Passing the ledger
    // on every road keeps the scope following the ledger; ReleaseForcedWeapon
    // erasing it then makes the next OOC tick declare Armor-only on its own.
    std::pair<RE::FormID, RE::FormID> ForcedHoldFor(RE::FormID a_follower);

    // MELEE-ONLY (fix/mfo-unreachable-flyer, cast/Fire.cpp): MFO has DECLARED him melee --
    // base class Melee (#65 combatClassOverride 1, the resting stance), or an
    // act.equip_melee force-hold standing in either hand (ForcedHoldFor: the hold the ch.17
    // hand claim is declared from) -- AND nothing in his hands reaches past a swing (no
    // bow / crossbow / staff, no spell whose delivery is not Self or Touch). Read by the
    // reach gates: Evaluator PickFoe (swing rules), Fire's Attack / Power attack, and the
    // Scheduler's ch.23 leash hold. Worker-serial (GetBaseClass reads g_followers, #4).
    bool MeleeOnly(RE::Actor* a_follower);

    // Per-tick reconcile from the combat scan: KEEP the force-hold iff the
    // feature is on AND an equip gambit of the forced weapon's OWN category held
    // this tick (a_wantStance: 0=none/condition-false, 1=melee, 2=ranged);
    // otherwise RELEASE it. This is the gambit true->false lifecycle: a==0
    // (condition went false) or a category flip both release.
    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance, bool a_condKnownFalse);

    // Revert/load: drop the session-scoped force-hold records. No engine call --
    // the world is being replaced (mirrors CombatStyle::ClearAll).
    void ClearForcedWeapons();

    // T#76 force-hold co-save (kRecForcedWeapon/'FWPN'): persist the force-equip
    // locks so a load can clear stale ones. CoLoad releases (never repopulates) —
    // a session starts with no force-hold; the gambit re-forces if still true.
    void CoSaveForcedWeapons(SKSE::SerializationInterface* a_intfc);
    void CoLoadForcedWeapons(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);

    // ── TASK 2 (feat/cast-gambit-concentration): firing-spell gambit lock ──────
    // While a spell gambit is actively firing (an owned-cast claim mid-decision,
    // or a concentration stream), CastOn/ConcentrationCast hold off a DIFFERENT
    // cast rule from re-pointing it -- see the lock's own doc comment in
    // Actuation.cpp (above ConcentrationCast) for the full rationale and its
    // release conditions. These two are its EXTERNAL release points, for
    // callers outside this TU:

    // Drop a_follower's lock NOW. Idempotent (no lock -> no-op). Call the
    // instant the follower is dismissed (Followers::ReleaseHeldState) or when
    // no cast rule's condition holds this tick / combat ends (Scheduler,
    // alongside ReleaseOffenseCast/ComposedCast::ClearWatch -- the same release
    // points that claim already uses).
    void ClearCastLock(RE::FormID a_follower);

    // Revert/load: drop every follower's lock. No engine call -- the world is
    // being replaced (mirrors ClearForcedWeapons/ClearSelfCasts). Called from
    // ClearSelfCasts(), not wired as a separate Serialization.cpp call site.
    void ClearCastLocks();

    // F3-7 (deploy-gate review 2026-09-07). Drop ONE follower's APMF-refusal log
    // dedup entries in Actuation_Direct.cpp's anon namespace. Declared here ONLY
    // because ClearCastLock (Actuation.cpp) has to reach across the TU boundary to
    // keep that map and its Actuation.cpp twin -- documented as identical -- on the
    // SAME per-follower release point. Not an outside-caller entry point.
    void ClearApmfRefusalLog(RE::FormID a_follower);

}
