#include "PCH.h"
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Logistics.h"   // supply-condition reads (counts of potions / arrows)

namespace MFO::Eval {

    namespace {

        // Is this opcode a FOE SELECTOR -- i.e. does it choose a target as well
        // as answer true/false?
        bool IsFoeSelector(const std::string& a_op) {
            return a_op == Vocab::kCondFoeAny ||
                   a_op == Vocab::kCondFoeHpBelow ||
                   a_op == Vocab::kCondFoeLowestHp;
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
        RE::ActorHandle PickFoe(RE::Actor* a_self, const std::string& a_op, float a_param) {
            RE::ActorHandle best;
            if (!a_self) return best;

            auto& rt = a_self->GetActorRuntimeData();
            auto* cc = rt.combatController;
            if (!cc || !cc->combatGroup) return best;

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

                    float score;
                    if (a_op == Vocab::kCondFoeAny) {
                        // Nearest.
                        score = selfPos.GetDistance(foe->GetPosition());
                    } else {
                        // Lowest HP, optionally gated on a threshold.
                        const float hp = Vocab::HealthPct(foe);
                        if (a_op == Vocab::kCondFoeHpBelow && hp >= a_param) continue;
                        score = hp;
                    }

                    if (score < bestScore) { bestScore = score; best = t.targetHandle; }
                }
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
            if (op == Vocab::kCondSelfOutOfArrows) {
                // -1 means "no bow/crossbow equipped": the rule is N/A, not true.
                // Vanilla grants infinite ammo of any type owned, so this rule is
                // harmlessly idle on vanilla and load-bearing on Requiem (§4.8.2).
                const int n = Logistics::ArrowCount(a_self);
                return n >= 0 && n < static_cast<int>(p);
            }

            // Foe selectors are resolved by the caller, which needs the chosen
            // handle. Reaching here means the caller did not ask -- say false
            // rather than silently claiming a target-less foe rule is true.
            if (IsFoeSelector(op)) return false;

            // An unknown opcode is never true. It is likely a rule authored by
            // a newer vocabulary and loaded by this build; it must fail closed,
            // never fire something else.
            return false;
        }

    }

    Choice Evaluate(RE::Actor* a_follower, const FollowerState& a_state, Table a_table) {
        Choice out;
        if (!a_follower) return out;

        auto* player = RE::PlayerCharacter::GetSingleton();

        const auto& list = a_table == Table::Combat ? a_state.combat()
                                                     : a_state.logistics();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const auto& g = list[i];
            if (!g.enabled) continue;

            RE::ActorHandle chosen;
            if (IsFoeSelector(g.conditionOpcode)) {
                // A foe selector is TRUE exactly when it finds someone. No
                // candidate means no target means the rule cannot run, so it
                // falls through to the next one -- which is the whole reason
                // "Foe: lowest HP -> Attack" can sit above "Always -> Wait".
                chosen = PickFoe(a_follower, g.conditionOpcode, g.conditionParam);
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
