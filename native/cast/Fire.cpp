// cast/Fire.cpp -- the combat-rule DISPATCH: Fire and its verbs, and the #68
// resolution ladder (ResolveCastTarget + its NearestAlly helper).
// Split out of the old native/Actuation*.cpp by the wave-1 subsystem-folder split
// (2026-09-24): a pure move, proven function by function with tools/splitcheck.
#include "Actuation_internal.h"
#include "Runtime.h"      // CastPathsVerified(): the ONE exact-version gate the cast paths share
#include "apmf/APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)
#include "ComposedCast.h" // WatchClaim/ClearWatch -- the shared [cfc] silent-claim diagnostic
                          // (feat/offense-cast-seats: reused here, NOT routed through Try())
#include "logistics/Logistics_internal.h" // 2026-09-13: EquipWeapon consumes THE weapon-style decision
                          // (ComputeWeaponRoles / WeaponScore) -- not in Logistics.h, and that
                          // header was outside the change's boundary. First non-Logistics include.
#include <chrono>         // Task 2: the firing-spell gambit lock's own timestamps
#include <limits>         // rank preemption: kNoRule sentinel (numeric_limits<int>::max)

namespace MFO::Actuation {

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
    RE::Actor* ResolveCastTarget(RE::Actor* a_follower, const Eval::Choice& a_choice,
                                 bool& a_outIsFallbackPlayer) {
        a_outIsFallbackPlayer = false;
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
            // (kActCastSelf), which never routes through this ladder. A stale
            // specific-follower sentinel (0xFF) lands here too and correctly
            // falls through rather than disarming the rule.
            break;
        }

        a_outIsFallbackPlayer = true;
        return player;
    }

    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice) {
        // No rule matched -> NO ENGINE CALL AT ALL. Not a no-op command, not a
        // neutral order: nothing (§4.4's do-nothing guarantee, which is what
        // makes an MFO follower byte-identical to a vanilla one when idle).
        if (!a_follower || a_choice.ruleIndex < 0) return { Result::NoOp, {} };

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
        g_firingRule = a_choice.ruleIndex;
        // Only an ALLY selector's param is an ally-HP threshold. Every other
        // condition's param means something else entirely, so it must never be
        // read as one -- Evaluator.cpp's IsAllySelector is this exact test, and
        // kCondAllyHpBelow is its only member today.
        g_firingAllyThreshold = (a_choice.conditionOpcode == Vocab::kCondAllyHpBelow)
                                    ? a_choice.conditionParam
                                    : -1.0f;

        const auto& op = a_choice.actionOpcode;

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
            if (!Targeting::Command(a_follower->GetFormID(), a_choice.target)) {
                return { Result::NoOp, "already on that target" };
            }
            // Log the foe's HP% too, so "Foe: lowest HP -> Attack" is legible in
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
                sp && IsSummonSpell(sp))   // one-shot conjure, any target (fix/mfo-summon-oneshot)
                return CastSummonOnce(a_follower, sp, a_choice.ruleIndex, "combat", op);
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
            // Nearest ally / a specific follower) or a selector that chose a foe
            // keeps the single-target ladder path below, unchanged.
            auto tp = a_choice.target.get();
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
            return CastOn(a_follower, a_choice.actionParam, target, !isFallbackPlayer);
        }

        if (op == Vocab::kActEquipRanged) return EquipWeapon(a_follower, true);
        if (op == Vocab::kActEquipMelee)  return EquipWeapon(a_follower, false);
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
                Targeting::Command(a_follower->GetFormID(), a_choice.target);
                return { Result::Fired, std::format("closing to melee on {}", foeName) };
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
            if (!a_follower->NotifyAnimationGraph("attackPowerStartInPlace"))
                return { Result::FailedOther, "power-attack anim rejected", true };
            Targeting::Command(a_follower->GetFormID(), a_choice.target);
            return { Result::Fired, std::format("power attack -> {}", foeName) };
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

        // Unknown action opcode: fail closed and say so. Likely a rule from a
        // newer vocabulary; it must never fall through to SOMETHING ELSE for
        // this rule -- but the scan may fall PAST it, exactly as the logistics
        // scan skips a stray opcode so it cannot shadow the rules below
        // (Logistics.cpp fall-through precedent).
        return { Result::FailedOther, std::format("unknown action '{}'", op), true };
    }

}
