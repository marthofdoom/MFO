#include "PCH.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Logistics.h"   // supply-condition reads (counts of potions / arrows)
#include "Followers.h"   // g_active -- the maintained teammate list (ally selector)
#include "Config.h"      // g_sharedRadius -- "ally" locality
#include "Confidence.h"  // ChaseRadius -- the combat chase cap (#22)

namespace MFO::Eval {

    namespace {

        // Is this opcode a FOE SELECTOR -- i.e. does it choose a target as well
        // as answer true/false?
        bool IsFoeSelector(const std::string& a_op) {
            return a_op == Vocab::kCondFoeAny        || a_op == Vocab::kCondFoeHpBelow  ||
                   a_op == Vocab::kCondFoeLowestHp   || a_op == Vocab::kCondFoeHighestHp ||
                   a_op == Vocab::kCondFoeWithinRange|| a_op == Vocab::kCondFoeBeyondRange ||
                   a_op == Vocab::kCondFoeAttackingPlayer || a_op == Vocab::kCondFoeAttackingMe ||
                   a_op == Vocab::kCondFoeIsUndead   || a_op == Vocab::kCondFoeIsDragon ||
                   a_op == Vocab::kCondFoeIsCaster   || a_op == Vocab::kCondFoeIsRanged ||
                   a_op == Vocab::kCondFoeWeakerThanMe || a_op == Vocab::kCondFoeBlocking ||
                   a_op == Vocab::kCondFoeFleeing    ||
                   a_op == Vocab::kCondFoeWeakFire   || a_op == Vocab::kCondFoeWeakFrost ||
                   a_op == Vocab::kCondFoeWeakShock;
        }

        // Weak to an element = its resist actor-value is negative (a race trait
        // or an active -resist effect). Reads the foe's own AV, never a name.
        bool IsWeakTo(RE::Actor* a_foe, RE::ActorValue a_resist) {
            auto* avo = a_foe ? a_foe->AsActorValueOwner() : nullptr;
            return avo && avo->GetActorValue(a_resist) < 0.0f;
        }
        // The ALLY selector chooses a teammate target the same way.
        bool IsAllySelector(const std::string& a_op) {
            return a_op == Vocab::kCondAllyHpBelow;
        }

        // Does this foe's current combat target resolve to a_who?
        bool FoeTargets(RE::Actor* a_foe, RE::Actor* a_who) {
            if (!a_foe || !a_who) return false;
            auto p = a_foe->GetActorRuntimeData().currentCombatTarget.get();
            return p.get() == a_who;
        }
        // Does the foe's RACE carry this keyword (ActorTypeUndead / ...Dragon)?
        bool RaceHasKeyword(RE::Actor* a_foe, const char* a_kw) {
            auto* race = a_foe ? a_foe->GetRace() : nullptr;
            return race && race->HasKeywordString(a_kw);
        }
        // Is the foe a spellcaster RIGHT NOW -- a SpellItem equipped in either
        // hand? Reads what they are actually fighting with, not their spell
        // list (a warrior who happens to know Flames is not "a caster").
        bool FoeIsCaster(RE::Actor* a_foe) {
            if (!a_foe) return false;
            auto* r = a_foe->GetEquippedObject(false);
            auto* l = a_foe->GetEquippedObject(true);
            return (r && r->As<RE::SpellItem>()) || (l && l->As<RE::SpellItem>());
        }
        // Is the foe wielding a bow or crossbow? Weapon type, not distance.
        bool FoeIsRanged(RE::Actor* a_foe) {
            auto* obj  = a_foe ? a_foe->GetEquippedObject(false) : nullptr;
            auto* weap = obj ? obj->As<RE::TESObjectWEAP>() : nullptr;
            if (!weap) return false;
            const auto wt = weap->GetWeaponType();
            return wt == RE::WEAPON_TYPE::kBow || wt == RE::WEAPON_TYPE::kCrossbow;
        }
        // Is the foe's combat AI in its flee state? Reads the foe's OWN
        // CombatState (guarded -- the header's IsFleeing() dereferences state
        // unchecked, so spell out the null chain here).
        bool FoeIsFleeing(RE::Actor* a_foe) {
            auto* cc = a_foe ? a_foe->GetActorRuntimeData().combatController : nullptr;
            return cc && cc->state && cc->state->isFleeing;
        }
        // Count live, engaged foes in the follower's own combat group (no target).
        int FoeCount(RE::Actor* a_self) {
            if (!a_self) return 0;
            auto* cc = a_self->GetActorRuntimeData().combatController;
            if (!cc || !cc->combatGroup) return 0;
            int n = 0;
            RE::BSReadLockGuard lk(cc->combatGroup->lock);
            for (const auto& t : cc->combatGroup->targets) {
                auto ptr = t.targetHandle.get();   // HOLD the NiPointer (Targeting rule)
                auto* foe = ptr.get();
                if (!foe || foe == a_self) continue;
                if (foe->IsDead() || foe->IsDisabled()) continue;
                if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
                ++n;
            }
            return n;
        }

        // Pick a foe from the follower's OWN COMBAT GROUP.
        //
        // Not a world sweep. The engine already tracks who is in this fight, so
        // this is both cheaper than walking highActorHandles and more correct --
        // a swept list could name someone the follower is not engaged with, and
        // the targeting hook only redirects among actors the engine considers
        // valid anyway (#59).
        //
        // Returns an empty handle when there is no candidate, which reads as
        // "condition false" and the rule falls through.
        RE::ActorHandle PickFoe(RE::Actor* a_self, RE::Actor* a_player,
                                const std::string& a_op, float a_param) {
            RE::ActorHandle best;
            if (!a_self) return best;

            auto& rt = a_self->GetActorRuntimeData();
            auto* cc = rt.combatController;
            if (!cc || !cc->combatGroup) return best;

            // Lowest `score` wins. Distance for the nearest-style selectors, HP
            // for the HP ones (negated for "highest"). A gate `continue`s a foe
            // that does not qualify at all.
            float bestScore = std::numeric_limits<float>::max();
            const auto selfPos = a_self->GetPosition();

            // The group is shared mutable engine state; read it under its own
            // lock, and do NOTHING but read inside.
            {
                RE::BSReadLockGuard lk(cc->combatGroup->lock);
                for (const auto& t : cc->combatGroup->targets) {
                    auto ptr = t.targetHandle.get();
                    auto* foe = ptr.get();
                    if (!foe || foe == a_self) continue;
                    if (foe->IsDead() || foe->IsDisabled()) continue;

                    // The engine has lost track of this one -- fled, hidden,
                    // undetected. Latching onto it would point the follower at
                    // something they cannot perceive, which is the same failure
                    // #59 guards against, one layer up.
                    if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;

                    const float dist = selfPos.GetDistance(foe->GetPosition());

                    // CONFIDENCE CHASE CAP (#22): never auto-select a foe beyond
                    // the follower's confidence-scaled chase radius, so a distance-
                    // blind selector ("attack the weakest / nearest / undead ...")
                    // can't march him across a pack to a far target (Erik's Falmer
                    // charge). Hurt/mobbed shrinks it; healthy widens it. The
                    // player's OWN explicit range selectors bypass -- they set that
                    // distance on purpose.
                    if (a_op != Vocab::kCondFoeWithinRange &&
                        a_op != Vocab::kCondFoeBeyondRange &&
                        dist > Confidence::ChaseRadius(a_self))
                        continue;

                    float score = dist;   // default for the gate-style selectors

                    if (a_op == Vocab::kCondFoeAny) {
                        score = dist;
                    } else if (a_op == Vocab::kCondFoeLowestHp || a_op == Vocab::kCondFoeHpBelow) {
                        const float hp = Vocab::HealthPct(foe);
                        if (a_op == Vocab::kCondFoeHpBelow && hp >= a_param) continue;
                        score = hp;
                    } else if (a_op == Vocab::kCondFoeHighestHp) {
                        score = -Vocab::HealthPct(foe);
                    } else if (a_op == Vocab::kCondFoeWithinRange) {
                        if (dist > a_param) continue;
                    } else if (a_op == Vocab::kCondFoeBeyondRange) {
                        if (dist <= a_param) continue;
                    } else if (a_op == Vocab::kCondFoeAttackingPlayer) {
                        if (!FoeTargets(foe, a_player)) continue;
                    } else if (a_op == Vocab::kCondFoeAttackingMe) {
                        if (!FoeTargets(foe, a_self)) continue;
                    } else if (a_op == Vocab::kCondFoeIsUndead) {
                        if (!RaceHasKeyword(foe, "ActorTypeUndead")) continue;
                    } else if (a_op == Vocab::kCondFoeIsDragon) {
                        if (!RaceHasKeyword(foe, "ActorTypeDragon")) continue;
                    } else if (a_op == Vocab::kCondFoeIsCaster) {
                        if (!FoeIsCaster(foe)) continue;
                    } else if (a_op == Vocab::kCondFoeIsRanged) {
                        if (!FoeIsRanged(foe)) continue;
                    } else if (a_op == Vocab::kCondFoeWeakerThanMe) {
                        if (foe->GetLevel() >= a_self->GetLevel()) continue;
                    } else if (a_op == Vocab::kCondFoeBlocking) {
                        if (!foe->IsBlocking()) continue;
                    } else if (a_op == Vocab::kCondFoeFleeing) {
                        if (!FoeIsFleeing(foe)) continue;
                    } else if (a_op == Vocab::kCondFoeWeakFire) {
                        if (!IsWeakTo(foe, RE::ActorValue::kResistFire)) continue;
                    } else if (a_op == Vocab::kCondFoeWeakFrost) {
                        if (!IsWeakTo(foe, RE::ActorValue::kResistFrost)) continue;
                    } else if (a_op == Vocab::kCondFoeWeakShock) {
                        if (!IsWeakTo(foe, RE::ActorValue::kResistShock)) continue;
                    }

                    if (score < bestScore) { bestScore = score; best = t.targetHandle; }
                }
            }
            return best;
        }

        // Pick the lowest-HP TEAMMATE under a_param pct, within the shared-kill
        // radius. Walks the maintained g_active list (not a world sweep), which
        // is updated on the same serial SKSE task this evaluator runs on, so it
        // is stable here. Yields both the truth value and the ally target.
        RE::ActorHandle PickAlly(RE::Actor* a_self, float a_param) {
            RE::ActorHandle best;
            if (!a_self) return best;
            const auto selfPos = a_self->GetPosition();
            const float radius = Config::g_sharedRadius.load();
            float lowest = a_param;   // must be strictly under the threshold
            for (const auto& h : Followers::g_active) {
                auto ptr = h.get();   // HOLD the NiPointer (Targeting rule)
                auto* ally = ptr.get();
                if (!ally || ally == a_self) continue;
                if (ally->IsDead() || ally->IsDisabled()) continue;
                if (selfPos.GetDistance(ally->GetPosition()) > radius) continue;
                const float hp = Vocab::HealthPct(ally);
                if (hp < lowest) { lowest = hp; best = ally->GetHandle(); }
            }
            return best;
        }

        // Evaluate ONE condition. Pure read. a_self is the follower; a_player
        // is the player, passed in rather than fetched so this stays a pure
        // function of its inputs (and so the caller controls whose state is
        // read -- INVARIANTS #14/#15).
        bool ConditionTrue(const Gambit& a_g, RE::Actor* a_self, RE::Actor* a_player) {
            const auto& op = a_g.conditionOpcode;
            const float p  = a_g.conditionParam;   // a percentage in [0,1] for the *below thresholds

            if (op == Vocab::kCondAlways)        return true;
            if (op == Vocab::kCondSelfHpBelow)   return Vocab::HealthPct(a_self)  < p;
            if (op == Vocab::kCondSelfMpBelow)   return Vocab::MagickaPct(a_self) < p;
            if (op == Vocab::kCondSelfSpBelow)   return Vocab::StaminaPct(a_self) < p;
            if (op == Vocab::kCondPlayerHpBelow) return Vocab::HealthPct(a_player) < p;

            // Self ABOVE gates -- the mirror trio.
            if (op == Vocab::kCondSelfHpAbove)   return Vocab::HealthPct(a_self)  > p;
            if (op == Vocab::kCondSelfMpAbove)   return Vocab::MagickaPct(a_self) > p;
            if (op == Vocab::kCondSelfSpAbove)   return Vocab::StaminaPct(a_self) > p;

            // World gates.
            if (op == Vocab::kCondIsInterior) {
                auto* cell = a_self ? a_self->GetParentCell() : nullptr;
                return cell && cell->IsInteriorCell();
            }
            if (op == Vocab::kCondIsNight) {
                auto* cal = RE::Calendar::GetSingleton();
                if (!cal) return false;
                const float h = cal->GetHour();   // [0,24)
                return h >= 20.0f || h < 6.0f;     // dusk to dawn
            }

            // Foe-COUNT gate -- reads the group, picks no target.
            if (op == Vocab::kCondFoeCountAtLeast)
                return FoeCount(a_self) >= static_cast<int>(p);

            // LOGISTICS SUPPLY CONDITIONS (§4.8.1). Pure reads of the NAMED
            // follower's own inventory/ammo (#14). p is a COUNT threshold, not a
            // percentage. These walk the inventory, so they belong only in the
            // logistics table's ~1 s tick (see Vocabulary.h) -- but ConditionTrue
            // does not know which table it is in, so it just answers the read;
            // #28 forbids second-guessing where the player put a rule.
            if (op == Vocab::kCondSelfLowHealthPotion)
                return Logistics::CountPotions(a_self, RE::ActorValue::kHealth)  < static_cast<int>(p);
            if (op == Vocab::kCondSelfLowStaminaPotion)
                return Logistics::CountPotions(a_self, RE::ActorValue::kStamina) < static_cast<int>(p);
            if (op == Vocab::kCondSelfLowMagickaPotion)
                return Logistics::CountPotions(a_self, RE::ActorValue::kMagicka) < static_cast<int>(p);
            // ARROWS / BOLTS carried below N. No bow gate (marth): whether a
            // follower should gather ammo is THIS rule's job to say, not a
            // hidden precondition. Separate opcodes -- arrows and bolts are
            // different gambits. p is a COUNT.
            if (op == Vocab::kCondSelfOutOfArrows)
                return Logistics::ArrowCount(a_self) < static_cast<int>(p);
            if (op == Vocab::kCondSelfOutOfBolts)
                return Logistics::BoltCount(a_self)  < static_cast<int>(p);
            // ENCUMBRANCE GATE. p is a FRACTION of the follower's carry cap
            // ([0,1], the Percent-param convention -- the board shows it as a
            // %). Fails toward FALSE (never over-encumbered) on an unreadable
            // actor, same fail-closed default as everything here.
            if (op == Vocab::kCondSelfCarryWeightAbove) {
                if (!a_self) return false;
                auto* avo = a_self->AsActorValueOwner();
                if (!avo) return false;
                const float cap = avo->GetActorValue(RE::ActorValue::kCarryWeight);
                if (cap <= 0.0f) return false;
                return (a_self->GetWeightInContainer() / cap) >= p;
            }

            // Selectors are resolved by the caller, which needs the chosen
            // handle. Reaching here means the caller did not ask -- say false
            // rather than silently claiming a target-less rule is true.
            if (IsFoeSelector(op) || IsAllySelector(op)) return false;

            // An unknown opcode is never true. It is likely a rule authored by
            // a newer vocabulary and loaded by this build; it must fail closed,
            // never fire something else.
            return false;
        }

    }

    Choice Evaluate(RE::Actor* a_follower, const FollowerState& a_state, Table a_table,
                    int a_startIndex) {
        Choice out;
        if (!a_follower) return out;

        auto* player = RE::PlayerCharacter::GetSingleton();

        const auto& list = a_table == Table::Combat ? a_state.combat()
                                                     : a_state.logistics();
        for (int i = std::max(0, a_startIndex); i < static_cast<int>(list.size()); ++i) {
            const auto& g = list[i];
            if (!g.enabled) continue;

            RE::ActorHandle chosen;
            if (IsFoeSelector(g.conditionOpcode)) {
                // A foe selector is TRUE exactly when it finds someone. No
                // candidate means no target means the rule cannot run, so it
                // falls through to the next one -- which is the whole reason
                // "Foe: lowest HP -> Attack" can sit above "Always -> Wait".
                chosen = PickFoe(a_follower, player, g.conditionOpcode, g.conditionParam);
                if (!chosen) continue;
            } else if (IsAllySelector(g.conditionOpcode)) {
                // Same shape, ally side: true iff a wounded teammate is found,
                // and that teammate becomes the target (Cast at ally / Heal Other).
                chosen = PickAlly(a_follower, g.conditionParam);
                if (!chosen) continue;
            } else if (!ConditionTrue(g, a_follower, player)) {
                continue;
            }

            // First true condition wins; nothing below is evaluated (§4.3).
            out.ruleIndex    = i;
            out.actionOpcode = g.actionOpcode;
            out.actionParam  = g.actionParamForm;
            out.subject      = g.subjectSelector;
            out.target       = chosen;
            return out;
        }
        return out;   // ruleIndex == -1 -> caller does nothing
    }

}
