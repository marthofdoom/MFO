#include "PCH.h"
#include "Evaluator.h"
#include "Vocabulary.h"

namespace MFO::Eval {

    namespace {

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

            // An unknown opcode is never true. It is likely a rule authored by
            // a newer vocabulary and loaded by this build; it must fail closed,
            // never fire something else.
            return false;
        }

    }

    Choice Evaluate(RE::Actor* a_follower, const FollowerState& a_state) {
        Choice out;
        if (!a_follower) return out;

        auto* player = RE::PlayerCharacter::GetSingleton();

        const auto& list = a_state.combat();
        for (int i = 0; i < static_cast<int>(list.size()); ++i) {
            const auto& g = list[i];
            if (!g.enabled) continue;
            if (!ConditionTrue(g, a_follower, player)) continue;

            // First true condition wins; nothing below is evaluated (§4.3).
            out.ruleIndex    = i;
            out.actionOpcode = g.actionOpcode;
            out.actionParam  = g.actionParamForm;
            out.subject      = g.subjectSelector;
            return out;
        }
        return out;   // ruleIndex == -1 -> caller does nothing
    }

}
