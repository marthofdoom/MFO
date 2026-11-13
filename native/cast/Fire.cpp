// cast/Fire.cpp -- the combat-rule DISPATCH: Fire and its verbs, and the #68
// resolution ladder (ResolveCastTarget + its NearestAlly helper).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "Runtime.h"      // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "Scheduler.h"    // ReachHoldSlack -- the reach gates read the hold's line (review U3)
#include "Taunt.h"        // act.taunt (taunt S2): Taunt::Fire
#include "apmf/APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "APMF_API.h"          // kCombatMove_Block / kCombatMove_PowerAttack: the "served on this game" question (review F3)
#include "Concealment.h"       // card G3: act.disengage starts the hide run
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
#include "logistics/PotionFx.h"   // act.drink_buff_potion: Logistics::DrinkPotionForm
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps
#include <limits>         // rank preemption: kNoRule sentinel (numeric_limits<int>::max)

namespace MFO::Actuation {

    namespace {
        // Attack verb: since when a follower's unchanged pin has stood while he is NOT in combat (worker-only).
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> s_notInCombatSince;
    }

    namespace {

        // #68: the nearest living player-teammate that is not a_follower
        // himself. Walks the maintained g_active list (not a world sweep) --
        // same precedent as Evaluator.cpp's PickAlly, just by DISTANCE
        // instead of lowest HP, because "nearest ally" as a cast SUBJECT is
        // a positional pick, not a triage one.
        RE::Actor* NearestAlly(RE::Actor* a_follower) {
            if (!a_follower) return nullptr;
            const auto selfPos = a_follower->GetPosition();
            RE::Actor* best = nullptr;
            float bestDist = std::numeric_limits<float>::max();
            for (const auto& h : Followers::g_active) {
                auto ptr = h.get();   // HOLD the NiPointer (Targeting rule)
                auto* ally = ptr.get();
                if (!ally || ally == a_follower) continue;
                if (ally->IsDead() || !ally->Is3DLoaded()) continue;
                const float d = selfPos.GetDistance(ally->GetPosition());
                if (d < bestDist) { bestDist = d; best = ally; }
            }
            return best;
        }

        // ResolveCastTarget moved OUT of this anon namespace to public
        // MFO::Actuation scope (just below, after this block closes) so the
        // logistics OOC cast_target path (Logistics.cpp) can share the exact
        // same resolution ladder as the combat Fire path. It still calls
        // NearestAlly (anon, above) -- an internal-linkage helper is visible
        // throughout this TU.

    }

    namespace {
        // The NoOp text for a Suppressed pin (Targeting::CommandOutcome::Suppressed). Two causes: the foe
        // was GIVEN UP after a not-engaged end (Harbinger ABI v31, apmf/CombatEntry.cpp; it lifts on state,
        // not when the gambit picks another foe), or its pin ended without ever going live.
        const char* SuppressedText(RE::Actor* a_follower, RE::ActorHandle a_target) {
            const auto ptr = a_target.get();
            if (a_follower && ptr && APMFBridge::EngageGivenUp(a_follower->GetFormID(), ptr->GetFormID()))
                return "target given up (Harbinger ended its engagement as not engaged); engaged again when it "
                       "moves, turns on the party or the party closes in";
            return "target pin ended by Harbinger; not re-pinned until the gambit picks another foe";
        }
    }

    // THE "ENEMY" TARGET (marth 2026-10-06: "that enemy must be the highest gambits target"; the
    // TARGET picker's "Enemy (gambit target)"). The foe MFO's own targeting gambits commanded:
    // Targeting::Current, the Harbinger ch.20 pin (pin route) or the latch (degrade), which the
    // highest-ranked winning Attack / foe-selector rule set. Never another selector's pick, never a
    // guess. nullptr when nothing is commanded, or the commanded actor is dead / disabled / no longer
    // hostile to the follower. THREADING: Targeting::Current is internally locked on both routes
    // (the latch's shared_mutex; APMFBridge's pin-table mutex) and this TU already writes it from the
    // job worker (Targeting::CommandEx in the Attack verb), so the worker read is the existing
    // contract, the same read Scheduler's [deadtgt] makes. The handle's actor is read like every
    // other selector target on this tick.
    RE::Actor* CommandedEnemy(RE::Actor* a_follower) {
        if (!a_follower) return nullptr;
        const auto ptr = Targeting::Current(a_follower->GetFormID()).get();
        auto* foe = ptr.get();
        if (!foe || foe == a_follower || foe->IsDead() || foe->IsDisabled() || !foe->IsHostileToActor(a_follower))
            return nullptr;
        return foe;
    }

    // THE RESOLUTION LADDER (#68) -- PUBLIC so combat (Fire) AND logistics
    // (out-of-combat cast_target) resolve a manual target the SAME way. Given a
    // cast-target choice, decides WHO gets cast at, first rung to match wins:
    //   1 (+3) a selector (or Evaluate()'s player-HP-condition special case)
    //       already named someone THIS TICK -- a_choice.target.
    //   2   an explicit subject the player picked on the row: a SPECIFIC
    //       follower (subjectActorForm) takes precedence over the enum when both
    //       are somehow set; Self/Player always resolve; NearestAlly falls on
    //       down the ladder if nobody is in range.
    //   4   otherwise -- THE PLAYER. The catch-all for actor-agnostic rules
    //       ("when dark -> cast Magelight") that would otherwise resolve to
    //       nobody. a_outIsFallbackPlayer marks this rung so CastOn's out-of-
    //       range skip knows to WAIVE it -- a fallback must always fire, never
    //       quietly vanish because the player happens to be far away.
    //   0   (before all of these) the two ABSOLUTE subjects (marth 2026-10-06): Caster -> the
    //       follower himself; Enemy -> CommandedEnemy, or nullptr when none is commanded (the
    //       ONE rung that resolves to nobody: callers NoOp / skip the rule, never the player).
    RE::Actor* ResolveCastTarget(RE::Actor* a_follower, const Eval::Choice& a_choice,
                                 bool& a_outIsFallbackPlayer) {
        a_outIsFallbackPlayer = false;
        // Rung 0. SetSubject clears subjectActorForm, so an Enemy / Caster row never names an actor.
        switch (static_cast<Vocab::Subject>(a_choice.subject)) {
        case Vocab::Subject::Caster: return a_follower;
        case Vocab::Subject::Enemy:  return CommandedEnemy(a_follower);
        default: break;
        }
        if (auto ptr = a_choice.target.get(); ptr.get()) return ptr.get();

        auto* player = RE::PlayerCharacter::GetSingleton();

        // A SPECIFIC follower, gone/dead/unloaded falls on down the ladder
        // rather than bailing -- a "questionable" target must never wall off the
        // whole rule (the #68 problem statement).
        if (a_choice.subjectActorForm != 0) {
            if (auto* specific = RE::TESForm::LookupByID<RE::Actor>(a_choice.subjectActorForm)) {
                if (!specific->IsDead() && specific->Is3DLoaded()) return specific;
            }
        }

        switch (static_cast<Vocab::Subject>(a_choice.subject)) {
        case Vocab::Subject::Player:
            return player;
        case Vocab::Subject::NearestAlly:
            if (auto* ally = NearestAlly(a_follower)) return ally;
            break;   // nobody in range -- fall to the player default below
        case Vocab::Subject::Self:
        default:
            // #68 (marth): on a CAST-AT-TARGET row the default subject (Self=0 --
            // what every pre-#68 row and every freshly-added row carries) means
            // AUTO: run the ladder to the PLAYER fallback, so an actor-agnostic
            // rule ("when dark -> cast Magelight") lights up around the player
            // instead of resolving to nobody (the #68 bug) or to the caster.
            // Deliberate self-casting is the separate "Cast on self" action
            // (kActCastSelf), which never routes through this ladder, or the
            // explicit Self pick (Subject::Caster, rung 0 above). A stale
            // specific-follower sentinel (0xFF) lands here too and correctly
            // falls through rather than disarming the rule.
            break;
        }

        a_outIsFallbackPlayer = true;
        return player;
    }

    bool MeleeOnly(RE::Actor* a_follower) {
        if (!a_follower) return false;
        const auto isMeleeWeapon = [](const RE::TESObjectWEAP* a_w) {
            const auto wt = a_w->GetWeaponType();
            return wt != RE::WEAPON_TYPE::kBow && wt != RE::WEAPON_TYPE::kCrossbow &&
                   wt != RE::WEAPON_TYPE::kStaff;
        };
        // Anything in hand that reaches past a swing: he is not melee-only right now.
        for (const bool left : { false, true }) {
            auto* obj = a_follower->GetEquippedObject(left);
            if (!obj) continue;
            if (auto* w = obj->As<RE::TESObjectWEAP>()) {
                if (!isMeleeWeapon(w)) return false;
            } else if (auto* sp = obj->As<RE::SpellItem>()) {
                const auto d = sp->GetDelivery();
                if (d != RE::MagicSystem::Delivery::kSelf && d != RE::MagicSystem::Delivery::kTouch) return false;
            }
        }
        // Declared melee: base class Melee (1 == CombatStyle::Stance::Melee, the #65 ordinal) ...
        const auto id = a_follower->GetFormID();
        if (Followers::GetBaseClass(id) == 1) return true;
        // ... or a standing act.equip_melee force-hold.
        const auto [holdR, holdL] = ForcedHoldFor(id);
        for (const RE::FormID h : { holdR, holdL }) {
            if (auto* w = h ? RE::TESForm::LookupByID<RE::TESObjectWEAP>(h) : nullptr; w && isMeleeWeapon(w))
                return true;
        }
        return false;
    }

    // The reach gate's reason text (Attack / Power attack): a transparent refusal that
    // latches nothing, so the scan falls to the rules below.
    namespace {
        std::string UnreachableReason(RE::Actor* a_foe, const CombatSense::ReachRead& a_rr) {
            return std::format("'{}' unreachable (flying, {}) -- not latched",
                               a_foe->GetName() ? a_foe->GetName() : "?", CombatSense::ReachText(a_rr));
        }
    }

    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice) {
        // No rule matched -> NO ENGINE CALL AT ALL. Not a no-op command, not a
        // neutral order: nothing (§4.4's do-nothing guarantee, which is what
        // makes an MFO follower byte-identical to a vanilla one when idle).
        if (!a_follower || a_choice.rank < 0) return { Result::NoOp, {} };

        // RANK, CARRIED FROM THE ONE PLACE THAT KNOWS IT. The gambit list order IS
        // the priority order (marth 2026-09-08), and the evaluator has just handed
        // us the winning rule's INDEX in that list. Recording it here -- the single
        // entry point into every actuation in this file -- is what lets the cast
        // hand lock store which gambit owns a hand and lets the preempt test make a
        // real comparison instead of inferring rank from when a rule happened to
        // arrive in a scan. Worker-serial, no lock (#4); see g_firingRule.
        //
        // RESET ON EVERY EXIT (review finding, 2026-09-08). Correct without it
        // TODAY -- every reader lives in this file's anon namespace, CastOn and
        // ConcentrationCast are reachable only from here, Fire has one caller, and
        // Logistics' own service runs inside the same serialized Scheduler::Tick --
        // so nothing can observe a stale value. But that is a fact about today's
        // call graph, not a property of the code: a future public entry into CastOn
        // would silently inherit whatever rule last fired, for whatever follower,
        // and mint a hand lock wearing another gambit's RANK. The guard makes the
        // failure mode of that mistake harmless instead: an unranked (kNoRule) lock,
        // which outranks nothing and is preemptable by anything.
        struct FiringScope {
            ~FiringScope() { g_firingRule = kNoRule; g_firingAllyThreshold = -1.0f; }
        } firingScope;
        g_firingRule = a_choice.rank;
        // Only an ALLY selector's param is an ally-HP threshold. Every other
        // condition's param means something else entirely, so it must never be
        // read as one -- kCondAllyHpBelow is the only ally selector that carries a
        // threshold (the ally STATE selectors, cond.ally_downed and kin, take no param).
        g_firingAllyThreshold = (a_choice.conditionOpcode == Vocab::kCondAllyHpBelow)
                                    ? a_choice.conditionParam
                                    : -1.0f;

        const auto& op = a_choice.actionOpcode;

#ifdef MFO_DEV_SELFTEST
        // The dev self-test marker never reaches here (Evaluator skips it); if a future caller hands it in,
        // it is a transparent no-op, never a wall (native/selftest/Runner.cpp owns the mechanism).
        if (op == Vocab::kActDevSelftest) return { Result::NoOp, "self-test runner marker", true };
#endif

        // HELD GUARD (review F2 of 88777f7; marth 2026-11-12 Q-melee-3: "a held guard denies all his own offense, heals
        // included"). While MFO's own block request is out or his guard is up (apmf/BlockClaims.cpp), MFO asks for nothing
        // that would drop it or that Harbinger's hold denies anyway: no attack latch, power attack, bash, taunt, cast of any
        // kind, or weapon swap. TRANSPARENT: nothing is mutated, no cooldown starts, and the rows below (a potion, Wait,
        // Flee) are still read. The guard ends by its window (at most two back to back, then 1.5 s of rest), never by MFO
        // talking over it.
        if (op != Vocab::kActBlock && APMFBridge::BlockHoldLive(a_follower->GetFormID()) &&
            (op == Vocab::kActAttack || op == Vocab::kActPowerAttack || op == Vocab::kActBash || op == Vocab::kActTaunt ||
             op == Vocab::kActCastSelf || op == Vocab::kActCastTarget || op == Vocab::kActCastPlayer ||
             op == Vocab::kActEquipRanged || op == Vocab::kActEquipMelee || op == Vocab::kActEquipSilver))
            return { Result::FailedOther, "holding his guard: no attack, cast or weapon change until it ends", true };

        if (op == Vocab::kActWait) {
            // Deliberately consumes the tick without acting -- the FFXII
            // "do nothing on purpose" idiom that lets a rule suppress the ones
            // below it (DESIGN.md §3.3). OPAQUE by design (GAMBIT_FLOWS §7.1):
            // Wait is the authored suppress idiom; it must keep walling.
            return { Result::NoOp, {} };
        }
        if (op == Vocab::kActAttack) {
            // THE ATTACK VERB (DESIGN §4.7a). There is no engine call that says
            // "swing at this one" -- the combat controller owns that entirely
            // and exposes nothing. What MFO can do is decide WHO, by latching
            // the choice; the hook then re-asserts it after every combat update,
            // because the engine re-picks continuously (ENGINE_NOTES §0.6).
            //
            // So this is cheap and idempotent by design: a rule that keeps
            // winning simply keeps naming the same foe.
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe) return { Result::FailedOther, "chosen foe no longer resolves", true };

            // REACH GATE (fix/mfo-unreachable-flyer): a melee-only follower never latches
            // a foe that is airborne out of his melee reach (CombatSense::OutOfMeleeReach).
            // PickFoe already skips one for a swing rule; this is the same answer at the
            // action, checked BEFORE the APMF claim and the latch so a refusal mutates
            // nothing. TRANSPARENT: the scan falls to the rules below (the power-attack
            // gate's no-melee-weapon shape).
            if (CombatSense::ReachRead rr; MeleeOnly(a_follower) &&
                                           CombatSense::OutOfMeleeReach(a_follower, foe,
                                               Scheduler::ReachHoldSlack(a_follower->GetFormID()), &rr))
                return { Result::FailedSkill, UnreachableReason(foe, rr), true };

            // A commanded Attack directive gets the SAME APMF combat-target arbitration
            // a cast directive already gets (ch.6 plumbing, no new APMF work): claim (or
            // re-point) the facet at the chosen foe. create=true => a pure-melee follower
            // with no prior cast-directive claim this fight now gets one created here,
            // instead of getting zero arbitration until/unless it later casts.
            // (MFO's own Targeting::Command below is what actually commands the target;
            // APMF only arbitrates the facet, it executes nothing.) No-op without APMF.
            if (APMFBridge::Available() && Config::g_apmfCast.load() &&
                !Config::g_legacyCastHybrid.load())
                APMFBridge::ClaimCombatTarget(a_follower->GetFormID(), foe->GetFormID(), /*create=*/true);

            // Ask the HOOK, not the config. They disagree whenever install was
            // refused with the flag on -- VR, today. Reporting Fired there would
            // buy a suppression window for a latch nothing reads.
            // (Harbinger ch.20 pin route: the pin does not need MFO's hook, so the
            // question there is bCommandTarget -- Targeting::Commandable.)
            if (!Targeting::Commandable()) {
                return { Result::FailedOther, "target command unavailable (hook not installed or bCommandTarget=0)", true };
            }

            // Re-commanding the SAME foe is not an action. The latch persists
            // and the hook re-asserts it every combat update, so a rule that
            // keeps winning would otherwise report Fired every suppression
            // window forever -- nine identical log lines in one fight, and a
            // suppression window burned each time that a higher rule could
            // have used.
            // OPAQUE by design (GAMBIT_FLOWS D3): Attack is an ACTIVITY --
            // FFXII's own semantics put reactive rules ABOVE it, and lines
            // below an active Attack never run. Do NOT make this transparent.
            // Harbinger ch.20 pin route: a SUPPRESSED pin (Harbinger ended it on this
            // foe and it never went live, so it is not re-pinned) holds NOTHING -- say
            // so, instead of "already on that target". Still OPAQUE per D3: the rule is
            // the follower's activity, and a transparent outcome here would let a lower
            // Attack rule pin a different foe, which this rule then re-picks away from
            // on its next pass -- an alternation D3's opacity exists to prevent.
            const auto cmd = Targeting::CommandEx(a_follower->GetFormID(), a_choice.target);
            if (cmd != Targeting::CommandOutcome::Unchanged) s_notInCombatSince.erase(a_follower->GetFormID());
            if (cmd == Targeting::CommandOutcome::Suppressed) {
                return { Result::NoOp, SuppressedText(a_follower, a_choice.target) };
            }
            if (cmd != Targeting::CommandOutcome::Changed) {
                // "Already on that target" is the OPAQUE wall of an ongoing Attack ACTIVITY (D3). A pin held while the
                // follower is NOT in combat (a ch.20 pin on a far foe the engine has not engaged; a Harbinger substrate
                // is not combat, ABI v26) is no activity: it must not read as satisfied and wall the rules below it
                // (ClickUp 1dfcuabc4du: the 13:59 far Reaver pin, 84 denied engine picks). TRANSPARENT then.
                // TRANSPARENT ONLY WHERE THE STALE-PIN FIX NEEDS IT (D3): party combat is ON, he is not in combat, and the
                // pinned foe is NOT itself fighting the party (a stale far pin). A pin on a foe that IS fighting the party
                // stays OPAQUE "already on that target", so no lower Attack rule can pin a second live target while the
                // first lives: there is no alternation between two live targets. Plus a DEBOUNCE (the own flag flaps false
                // mid-fight): the stretch must last 2 x the follower's real service period (roster size x the 133 ms
                // pump, floor 1 s). Worker-only map (the combat scan), erased when he is in combat or the pin changes.
                const auto fid = a_follower->GetFormID();
                bool foeFightsParty = true;
                {
                    auto  tp = foe->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
                    auto* t  = tp.get();
                    foeFightsParty = t && (t->IsPlayerRef() || t->IsPlayerTeammate());
                }
                if (APMFBridge::InCombat(a_follower) || !Scheduler::PartyCombat() || foeFightsParty) {
                    s_notInCombatSince.erase(fid);
                } else {
                    const double kPinNotInCombatSecs = std::max(
                        1.0, 2.0 * 0.133 * static_cast<double>(std::max<std::size_t>(1, Followers::g_activeIds.size())));
                    const auto nowT = std::chrono::steady_clock::now();
                    const auto sit  = s_notInCombatSince.emplace(fid, nowT).first;
                    if (std::chrono::duration<double>(nowT - sit->second).count() >= kPinNotInCombatSecs)
                        return { Result::NoOp, "pinned, but not in combat (not counted as engaged)", true };
                }
                return { Result::NoOp, "already on that target" };
            }
            // Log the foe's HP% too, so "Foe: Lowest HP -> Attack" is legible in
            // the log -- confirms the SELECTOR actually picked the weakest foe.
            return { Result::Fired,
                     std::format("target {} ({}% hp)",
                                 foe->GetName() ? foe->GetName() : "?",
                                 static_cast<int>(Vocab::HealthPct(foe) * 100.0f)) };
        }
        // SUMMON SPAM GUARD (v1.1.1, COMBAT path). Mirrors the out-of-combat skip
        // in Logistics::ServiceFollower. A conjured/reanimated creature is a
        // COMMANDED ACTOR, not a caster-side magic effect, so the AI-first cast
        // grace below re-fires a summon gambit every grace window even while the
        // creature is still alive (marth, field). Suppress the cast while the
        // follower already commands a LIVE summon from THIS spell -- caster-side,
        // per-spell (Twin-Souls-aware via the helper), keyed on the live actor so
        // a killed/expired summon recasts at once. TRANSPARENT, so the combat scan
        // falls PAST it to the next rule exactly like the cast-grace hold. No-op
        // for any non-summon spell (CasterHasLiveSummon returns false), so buffs/
        // heals/candlelight and every non-summon combat cast are byte-identical and
        // the grace/pacing below is untouched. Reads only the active-effect list --
        // the same off-worker read as the existing combat guards, no new lock.
        if (op == Vocab::kActCastSelf || op == Vocab::kActCastPlayer ||
            op == Vocab::kActCastTarget) {
            if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_choice.actionParam);
                sp && IsSummonSpell(sp)) {   // one-shot conjure, any target (fix/mfo-summon-oneshot)
                // THE ANIMATED SUMMON CLAIM (feat/mfo-claim-road-summon-ally-rowless, cast/BuffRoad.cpp): with
                // Harbinger's summon seats and a combat controller the summon is a self claim (target 0), cast
                // by the follower's own AI through CastOn's self fork (the hand lock, the never-fired bound).
                // Every other answer is the direct one-shot below, ChooseBuffRoad naming why on a [buff] line.
                if (Config::g_castSelf.load() && ChooseBuffRoad(a_follower, sp) == BuffRoad::Claim)
                    return CastOn(a_follower, a_choice.actionParam, a_follower);
                return CastSummonOnce(a_follower, sp, a_choice.rank, "combat", op);
            }
            if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_choice.actionParam);
                sp && CasterHasLiveSummon(a_follower, sp)) {
                return { Result::NoOp, "summon still live", true };
            }
            // (APMF cast ownership is engaged inside CastOn's FF-non-self hostile
            // branch, where the RESOLVED target is known -- see the OWNED cast model
            // there. Not here: the target is not resolved for kActCastTarget yet.)
        }
        if (op == Vocab::kActCastSelf) {
            return CastOn(a_follower, a_choice.actionParam, a_follower);
        }
        if (op == Vocab::kActCastPlayer) {
            return CastOn(a_follower, a_choice.actionParam, RE::PlayerCharacter::GetSingleton());
        }
        if (op == Vocab::kActCastTarget) {
            // AUTO (marth): the board's DEFAULT target pick ("Auto", carried as
            // Subject::Self on a cast-target row) infers the set from the spell's
            // nature and fans the cast out (CastAuto). It engages ONLY when
            // nothing more specific named a target -- no explicit subject actor,
            // no selector/condition target this tick. A MANUAL pick (Player /
            // Ally: Nearest / a specific follower) or a selector that chose a foe
            // keeps the single-target ladder path below, unchanged.
            auto tp = a_choice.target.get();
            // An AUTO target on a nature-undeclared spell never casts, selector target or not (marth
            // 2026-10-06: "Auto doesnt even show up for these spells"). Transparent; the board labels it.
            if (a_choice.subjectActorForm == 0 &&
                static_cast<Vocab::Subject>(a_choice.subject) == Vocab::Subject::Self)
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_choice.actionParam);
                    sp && AutoNatureUndeclared(a_follower, sp))
                    return { Result::NoOp, "auto: spell nature undeclared in its record (pick Enemy, Ally or Self)", true };
            const bool autoPick =
                a_choice.subjectActorForm == 0 &&
                static_cast<Vocab::Subject>(a_choice.subject) == Vocab::Subject::Self &&
                !tp.get();
            if (autoPick) {
                // #2: a beneficial HEAL AUTO fan must match the firing gambit's own
                // status requirement, not blanket-heal everyone below full. When the
                // firing rule's condition is a health-below gate, its param IS the
                // threshold (heal only allies under it); otherwise 1.0 keeps the
                // whole-party / anyone-below-full behaviour ("Always -> Heal (Auto)",
                // world-gated buffs). Non-heal spells ignore the threshold entirely.
                float healThreshold = 1.0f;
                if (a_choice.conditionOpcode == Vocab::kCondSelfHpBelow   ||
                    a_choice.conditionOpcode == Vocab::kCondPlayerHpBelow ||
                    a_choice.conditionOpcode == Vocab::kCondAllyHpBelow)
                    healThreshold = a_choice.conditionParam;
                return CastAuto(a_follower, a_choice.actionParam, healThreshold);
            }

            // #68: the full resolution ladder -- a selector/condition target
            // first, then the row's explicit subject, then the player as the
            // last rung. a_rangeGate is OFF only for that last rung (the
            // fallback must fire regardless of distance); every obvious
            // target above it is range-checked inside CastOn.
            bool isFallbackPlayer = false;
            auto* target = ResolveCastTarget(a_follower, a_choice, isFallbackPlayer);
            // Target "Enemy (gambit target)" with no commanded foe: wait, transparently. Never the
            // caster, never the player (marth 2026-10-06). Any spell, not only a nature-undeclared one.
            if (!target && static_cast<Vocab::Subject>(a_choice.subject) == Vocab::Subject::Enemy)
                return { Result::NoOp, "target Enemy: no commanded target", true };
            // G10 (cast/WillWork.cpp): a spell that can never affect this target (level cap, effect conditions)
            // is passed over, transparently. Unknown (no fresh measurement) does NOT skip and never holds.
            if (target && target != a_follower)
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_choice.actionParam)) {
                    const WillWorkResult ww = WillWork(a_follower, sp, target);
                    if (ww.verdict == WillWorkVerdict::No)
                        return { Result::FailedSkill, "will not work: " + ww.reason, true };
                }
            // Target "Self" (Subject::Caster) resolved to the follower: CastOn(follower, spell, follower)
            // takes the same road act.cast_self does above (the self road, the hand lock, the bounds); the one
            // difference is a_rangeGate (true here, act.cast_self passes the default), which CastOn reads only
            // for a target other than the follower, so the two behave the same.
            return CastOn(a_follower, a_choice.actionParam, target, !isFallbackPlayer);
        }

        if (op == Vocab::kActEquipRanged) return EquipWeapon(a_follower, true);
        if (op == Vocab::kActEquipMelee)  return EquipWeapon(a_follower, false);
        if (op == Vocab::kActEquipSilver) return EquipSilver(a_follower);   // cast/EquipSilver.cpp (86e3p2gp9)
        if (op == Vocab::kActTaunt) return Taunt::Fire(a_follower, a_choice);   // Taunt.cpp (taunt S2, 86e3g6duw)
        // SNEAK (card G1, apmf/SneakClaims.cpp): hold the ch.3 stance claim while this rule stands. TRANSPARENT either
        // way: the scan goes on to the rows below, so "Always -> Sneak" can sit on top of a table. NoOp = held,
        // FailedOther = not held (Harbinger absent or refused, or in combat with bSneakInCombat off), reason named.
        if (op == Vocab::kActSneak) {
            if (const char* why = APMFBridge::ServiceSneakRule(a_follower, APMFBridge::SneakTable::Combat))
                return { Result::FailedOther, why, true };
            return { Result::NoOp, "sneaking", true };
        }
        // kActEquipTorch moved to the LOGISTICS table (#35, marth: never needed in
        // combat) -- handled by Logistics::ServiceFollower now, not here.

        if (op == Vocab::kActFlee) {
            // Disengage by reusing the RETREAT package -- travel to the player
            // under kIgnoreCombat, the proven machinery that pulls a follower off
            // a fight (Packages::RetreatFill). Cleared on combat end / arrival by
            // the retreat driver. #35. APMF SHOWPIECE: when APMF is present,
            // RetreatFill routes this through APMF's ch.9 0x49 package-offer
            // channel instead of MFO_RetreatQuest's alias/static-priority-60
            // race (same fix as loot-travel, for a follower package-locked by
            // an outranking custom AI framework); the alias route below is the
            // APMF-ABSENT degrade only. Transparent here -- RetreatFill owns
            // the routing decision.
            if (Packages::RetreatFill(a_follower))
                return { Result::Fired, "flee -> retreat to player" };
            return { Result::FailedOther, "retreat unavailable", true };
        }
        if (op == Vocab::kActPowerAttack) {
            // "Foe blocking -> Power attack" (marth): get in MELEE range of the
            // chosen foe, THEN power-attack that foe. There is no engine verb for
            // "power-attack X" -- MFO owns WHO (the target latch, like Attack) and
            // the range GATE (fire the swing only when actually adjacent); the
            // engine's own combat AI owns the MOVEMENT that closes the distance.
            //
            // The chosen foe is a_choice.target -- for a foe selector (incl.
            // kCondFoeBlocking) PickFoe already picked the specific blocking,
            // hostile, in-chase-cap foe (Evaluator.cpp PickFoe; #22b: that
            // selection is EVALUATOR state, we only ACT on it here). This gate is
            // GENERAL: any gambit whose action is Power attack swings only when in
            // reach of ITS intended target, not "any foe blocked -> swing in air"
            // (marth, field: the follower power-attacked at whatever range the
            // instant any foe blocked).
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe) return { Result::FailedOther, "chosen foe no longer resolves", true };
            if (!Targeting::Commandable())
                return { Result::FailedOther, "target command unavailable (hook not installed or bCommandTarget=0)", true };
            // Only meaningful with a MELEE weapon drawn -- a power attack from a bow
            // or empty hands is nonsense and would just burn the tick (Fable).
            auto* r = a_follower->GetEquippedObject(false);
            auto* w = r ? r->As<RE::TESObjectWEAP>() : nullptr;
            const auto wt = w ? w->GetWeaponType() : RE::WEAPON_TYPE::kHandToHandMelee;
            const bool melee = w && wt != RE::WEAPON_TYPE::kBow && wt != RE::WEAPON_TYPE::kCrossbow &&
                                    wt != RE::WEAPON_TYPE::kStaff;
            // TRANSPARENT: "foe blocking -> power attack" while holding a bow
            // must not starve the attack rule below for as long as the foe
            // blocks (GAMBIT_FLOWS §3.6). Checked BEFORE any latch so a rejected
            // (bow-held) power attack mutates NOTHING and falls through cleanly.
            if (!melee) return { Result::FailedSkill, "no melee weapon drawn for a power attack", true };
            // REACH GATE (fix/mfo-unreachable-flyer), the Attack verb's: no CLOSE on a foe
            // airborne out of a melee-only follower's reach. Before any latch, transparent.
            if (CombatSense::ReachRead rr; MeleeOnly(a_follower) &&
                                           CombatSense::OutOfMeleeReach(a_follower, foe,
                                               Scheduler::ReachHoldSlack(a_follower->GetFormID()), &rr))
                return { Result::FailedSkill, UnreachableReason(foe, rr), true };

            // RANGE GATE. GetDistance is a pure read of already-loaded actor data
            // (the kCondFoeWithinRange path reads distance the same way on this
            // same worker tick), so no MainThread::Post is needed here.
            const float dist = a_follower->GetPosition().GetDistance(foe->GetPosition());
            const char* foeName = foe->GetName() ? foe->GetName() : "?";

            // OUT of melee reach -> CLOSE first. Latch the chosen foe as the combat
            // target: the combat-thread hook re-asserts it every update and the
            // engine's own combat AI walks the follower in (the same "approach"
            // plain Attack relies on -- MFO invents no movement). OPAQUE like Attack
            // (GAMBIT_FLOWS D3): while closing on the blocking foe the tick belongs
            // to this rule; a lower rule must not pull him off it. Latching then
            // reporting Fired is a single consistent outcome (no fall-through), so
            // it does NOT reintroduce the "latch + transparent-reject" two-mutation
            // tick the anim path below still guards against.
            if (dist > Config::g_meleeReach.load()) {
                // Report Fired only while a latch / pin actually HOLDS the foe (a new one,
                // or the standing one). A Suppressed pin holds nothing, so nothing is
                // closing on this foe on our order -- an honest NoOp (opaque, like Attack).
                const auto cmd = Targeting::CommandEx(a_follower->GetFormID(), a_choice.target);
                if (cmd == Targeting::CommandOutcome::Suppressed)
                    return { Result::NoOp, SuppressedText(a_follower, a_choice.target) };
                if (cmd == Targeting::CommandOutcome::Unavailable)
                    return { Result::FailedOther, "target command unavailable", true };
                return { Result::Fired, std::format("closing to melee on {}", foeName), false, /*approach=*/true };
            }

            // IN melee reach -- SWING. Try the anim FIRST and commit the target
            // latch ONLY if it accepted. Latch-then-reject (Opus review) was a
            // two-mutation tick: Command wrote the latch, then the transparent
            // reject fell through and let a LOWER rule also fire -- and a FAILED
            // power attack still committed a target. Ordering the graph check ahead
            // means a rejected power attack mutates NOTHING and falls through
            // cleanly (§4.3, GAMBIT_FLOWS §3.6). `sent` still isn't proof the swing
            // landed; field-verify. Report Fired only on accept so a reject buys no
            // suppression window (Fable: Fired-on-reject went visibly passive).
            //
            // TWO ROADS (T2 batch M item 2, marth Q82; both stay selectable until the field probe decides):
            //  * road=harbinger ([Debug] bPowerAttackEngineRoad = 1 and Harbinger ABI >= 44): the engine's REAL power
            //    attack (ch.31 kCombatMove_PowerAttack, apmf/CombatMove.cpp). Harbinger starts one of his own power
            //    entries by the engine's start-of-attack action on the game thread, so the hit carries the power
            //    attack's damage and stamina cost. The outcome is asynchronous ("[combat-move]" lines by
            //    PollCombatMove), so MFO tests here what it can read: an attack already in progress is the engine's
            //    first refusal and is TRANSPARENT (nothing asked, nothing mutated).
            //  * road=graph: MFO's own animation event, the road before. It plays the swing; whether the hit is a real
            //    power attack is what the probe measures.
            // Review F3: the engine road only where Harbinger SERVES the move on this game (asked once per load). Where it
            // does not (1.7.104 today), road=graph is the documented degrade for an unserved facet; a refusal of a served
            // request never falls back to it.
            const char* const unserved = Config::g_powerAttackEngineRoad.load(std::memory_order_relaxed)
                                             ? APMFBridge::CombatMoveUnservedReason(a_follower->GetFormID(), APMF_API::kCombatMove_PowerAttack)
                                             : "bPowerAttackEngineRoad=0";
            if (!unserved) {
                if (a_follower->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
                    return { Result::FailedOther, "power attack: an attack is already in progress", true };
                const std::uint32_t h = APMFBridge::RequestPowerAttack(a_follower->GetFormID(), foe->GetFormID());
                if (h == 0) return { Result::FailedOther, "power attack refused by Harbinger at the call", true };
                Targeting::Command(a_follower->GetFormID(), a_choice.target);
                spdlog::info("[power-attack] {:08X}: road=harbinger (ch.31 kCombatMove_PowerAttack, h={}) -> {:08X} '{}' at {:.0f}u",
                             a_follower->GetFormID(), h, foe->GetFormID(), foeName, dist);
                return { Result::Fired, std::format("power attack requested -> {} (road=harbinger, h={})", foeName, h) };
            }
            if (!a_follower->NotifyAnimationGraph("attackPowerStartInPlace"))
                return { Result::FailedOther, "power-attack anim rejected", true };
            Targeting::Command(a_follower->GetFormID(), a_choice.target);
            spdlog::info("[power-attack] {:08X}: road=graph (animation event attackPowerStartInPlace; {}) -> {:08X} '{}' at {:.0f}u",
                         a_follower->GetFormID(), unserved, foe->GetFormID(), foeName, dist);
            return { Result::Fired, std::format("power attack -> {} (road=graph)", foeName) };
        }

        if (op == Vocab::kActBash) {
            // BASH (Harbinger ch.31, ABI v38). MFO owns WHO and WHEN: the chosen foe, alive, inside melee reach NOW. There is
            // no closing leg (a bash answers the moment: a foe charging a spell, a foe on him). Harbinger starts the engine's
            // own bash action once on the game thread; the outcome is asynchronous and written by APMFBridge::PollCombatMove.
            // Everything before the request is TRANSPARENT: a bash that cannot be asked for mutates nothing.
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe || foe->IsDead()) return { Result::FailedOther, "chosen foe no longer resolves", true };
            if (!APMFBridge::CombatMoveSupported())
                return { Result::FailedOther, "no bash road (Harbinger absent or older than ABI 38)", true };
            const float dist = a_follower->GetPosition().GetDistance(foe->GetPosition());
            if (dist > Config::g_meleeReach.load())
                return { Result::FailedSkill, "bash: the foe is out of melee reach", true };
            const std::uint32_t h = APMFBridge::RequestBash(a_follower->GetFormID(), foe->GetFormID(), /*a_power=*/false);
            if (h == 0) return { Result::FailedOther, "bash refused by Harbinger at the call", true };
            return { Result::Fired, std::format("bash requested -> {} (h={})", foe->GetName() ? foe->GetName() : "?", h) };
        }

        if (op == Vocab::kActBlock) {
            // BLOCK (Harbinger ch.31 held guard, ABI v43; apmf/BlockClaims.cpp). MFO owns WHEN (the rule's condition) and
            // for HOW LONG ([Evaluator] fBlockHoldSeconds); Harbinger raises the guard by the engine's own block action,
            // denies his own offense and the engine's own release for the window, and lowers it. Everything before the
            // request is TRANSPARENT: a block that cannot be asked for mutates nothing and the rows below run.
            // Review F3: "served on this game" is asked once per load (the ABI v35 dry run), so a runtime or an INI on which
            // Harbinger refuses the block is one log line and a skipped row, never a request every 2 s.
            if (const char* why = APMFBridge::CombatMoveUnservedReason(a_follower->GetFormID(), APMF_API::kCombatMove_Block))
                return { Result::FailedOther, std::format("no block road: {}", why), true };
            if (const char* why = APMFBridge::BlockHandsReason(a_follower))
                return { Result::FailedSkill, std::format("block: {}", why), true };
            // Review F6: the engine's own first refusal (an attack in progress), tested before a NEW raise is asked for, so
            // a Busy refusal does not spend the row's cooldown. A standing guard is renewed whatever his attack state.
            if (!APMFBridge::BlockHoldLive(a_follower->GetFormID()) &&
                a_follower->AsActorState()->GetAttackState() != RE::ATTACK_STATE_ENUM::kNone)
                return { Result::FailedOther, "block: an attack is in progress", true };
            std::uint32_t h = 0;
            switch (APMFBridge::ServiceBlock(a_follower->GetFormID(), &h)) {
            case APMFBridge::BlockAsk::Requested:
                return { Result::Fired, std::format("guard up for {:.1f} s (h={})",
                                                    Config::g_blockHoldSeconds.load(std::memory_order_relaxed), h) };
            case APMFBridge::BlockAsk::Held:
                // The guard is up (or its request is about to run): the tick is this rule's, as with a standing attack.
                return { Result::NoOp, "holding his guard" };
            case APMFBridge::BlockAsk::Backoff:
                return { Result::FailedOther, "block: waiting after a refusal", true };
            case APMFBridge::BlockAsk::Resting:
                return { Result::FailedOther, "block: resting after a guard", true };
            case APMFBridge::BlockAsk::Refused:
                return { Result::FailedOther, "block refused by Harbinger at the call", true };
            case APMFBridge::BlockAsk::NoRoad:
            default:
                return { Result::FailedOther, "no block road", true };
            }
        }

        if (op == Vocab::kActGiveGround) {
            // GIVE GROUND (Harbinger ch.24 on a point, ABI v39). MFO declares the point (straight away from the chosen foe);
            // the engine's own return-to-area movement walks him there while he keeps fighting. Transparent when no step can
            // be filed (no road, a held-heal approach stands, a step already stands): nothing was mutated.
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe || foe->IsDead()) return { Result::FailedOther, "chosen foe no longer resolves", true };
            if (!APMFBridge::GroundStepSupported())
                return { Result::FailedOther, "no give-ground road (Harbinger absent or older than ABI 39)", true };
            if (!APMFBridge::RequestGroundStep(a_follower, foe))
                return { Result::FailedSkill, "give ground: no step filed (a heal approach or a step stands, or refused)", true };
            return { Result::Fired, std::format("giving ground from {}", foe->GetName() ? foe->GetName() : "?") };
        }

        if (op == Vocab::kActDisengage) {
            // DISENGAGE TO OUT OF SIGHT, THEN SNEAK (card G3). Concealment owns the point (picked on the main thread with
            // MFO's own rays) and the run (apmf/Disengage.cpp: Harbinger ch.24 on that point + a ch.7 Offense deny).
            // Transparent when no run can start (no road, no fresh point, a heal approach stands): nothing was mutated.
            std::string desc;
            // The kind (batch I): 0 = the hide run or the flee, 1 = a back-off, 2 = the hidden step (Candidate::form via IausSource).
            if (const char* why = Concealment::Begin(a_follower, desc, Concealment::KindOfForm(a_choice.actionParam)))
                return { Result::FailedOther, why, true };
            return { Result::Fired, desc };
        }

        // IN-COMBAT DRINKING. The same drink action logistics runs OUT of combat,
        // available here so a survival gambit ("Self HP < 30% -> Drink health
        // potion") can fire mid-fight. Logistics::DrinkPotion is cooldown-gated
        // per resource (~the potion's own duration), so a persistently-winning
        // rule cannot chain-drink the whole stack -- and it drinks only what the
        // follower already carries (§5.3: MFO hands out nothing).
        if (op == Vocab::kActDrinkHealthPotion || op == Vocab::kActDrinkStaminaPotion ||
            op == Vocab::kActDrinkMagickaPotion) {
            const auto av = (op == Vocab::kActDrinkHealthPotion)  ? RE::ActorValue::kHealth  :
                            (op == Vocab::kActDrinkStaminaPotion) ? RE::ActorValue::kStamina :
                                                                    RE::ActorValue::kMagicka;
            if (Logistics::DrinkPotion(a_follower, av))
                return { Result::Fired, "drank a potion" };
            // TRANSPARENT: the worst concrete D1 -- a follower must not die
            // holding a heal rule below an out-of-potions drink rule (§3.1).
            return { Result::FailedSkill, "no matching potion, or still on cooldown", true };
        }

        // DRINK A BUFF POTION (IAUS-only, feat/buff-potions): the potion named by actionParam, through the same EquipObject road as the drinks
        // above (Logistics::DrinkPotionForm). TRANSPARENT when nothing was drunk (he cannot act, the potion is gone or not a buff potion, or it is
        // still inside its own duration): no cooldown starts and the scan goes on to the candidates below.
        if (op == Vocab::kActDrinkBuffPotion) {
            if (Logistics::DrinkPotionForm(a_follower, a_choice.actionParam))
                return { Result::Fired, "use issued (potion, food or drink; the consume is confirmed on the next pass)" };
            return { Result::FailedSkill, "no such item carried, nothing beneficial in it, or still inside its own duration", true };
        }

        // Unknown action opcode: fail closed and say so. Likely a rule from a
        // newer vocabulary; it must never fall through to SOMETHING ELSE for
        // this rule -- but the scan may fall PAST it, exactly as the logistics
        // scan skips a stray opcode so it cannot shadow the rules below
        // (Logistics.cpp fall-through precedent).
        return { Result::FailedOther, std::format("unknown action '{}'", op), true };
    }

}
