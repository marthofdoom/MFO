#include "PCH.h"
#include "Actuation.h"
#include "Vocabulary.h"
#include "Config.h"

namespace MFO::Actuation {

    namespace {

        RE::Actor* ResolveTarget(RE::Actor* a_follower, std::uint8_t a_subject) {
            switch (static_cast<Vocab::Subject>(a_subject)) {
            case Vocab::Subject::Player: return RE::PlayerCharacter::GetSingleton();
            case Vocab::Subject::Self:
            default:                     return a_follower;
            }
        }

        Outcome CastOn(RE::Actor* a_follower, RE::FormID a_spellID, RE::Actor* a_target) {
            if (!a_target) return { Result::FailedOther, "no valid target" };

            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!spell) {
                return { Result::FailedOther, std::format("spell {:08X} not in load order", a_spellID) };
            }

            // COMPETENCE IS NOT PERMISSION (DESIGN.md §5.3). MFO does not top up
            // magicka, discount the cost, or substitute a cheaper spell -- if
            // the follower cannot afford it the rule FAILS and the tick falls
            // through, and the board says why. This is the whole point: a list
            // you wrote that your follower cannot run is legible, not silent.
            auto* avo = a_follower->AsActorValueOwner();
            if (avo) {
                const float cost = spell->CalculateMagickaCost(a_follower);
                const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
                if (cost > have) {
                    return { Result::FailedSkill,
                             std::format("insufficient magicka ({:.0f} < {:.0f})", have, cost) };
                }
            }

            // CASTING SOURCE IS CONFIGURABLE, and that is not fussiness.
            // kInstant applies the effect with NO ANIMATION (proven in-game,
            // ENGINE_NOTES §0.8) -- a follower silently healing looks broken.
            // kInstant is literally the no-animation caster, so a HAND source
            // is the obvious candidate for a visible cast, but that is a
            // hypothesis: the M4 probe now fires one variant per source so it
            // can be settled by observation. `iCastSource` selects the winner
            // without a rebuild.
            using CS = RE::MagicSystem::CastingSource;
            CS src = CS::kInstant;
            switch (Config::g_castSource.load()) {
            case 0:  src = CS::kLeftHand;  break;
            case 1:  src = CS::kRightHand; break;
            case 2:  src = CS::kOther;     break;
            default: src = CS::kInstant;   break;
            }

            auto* caster = a_follower->GetMagicCaster(src);
            if (!caster) {
                // Fall back rather than silently doing nothing -- an unanimated
                // heal beats no heal.
                caster = a_follower->GetMagicCaster(CS::kInstant);
                if (!caster) return { Result::FailedOther, "no magic caster" };
            }

            caster->CastSpellImmediate(spell, false, a_target, 1.0f, false, 0.0f, a_follower);
            return { Result::Fired, {} };
        }

    }

    Outcome Fire(RE::Actor* a_follower, const Eval::Choice& a_choice) {
        // No rule matched -> NO ENGINE CALL AT ALL. Not a no-op command, not a
        // neutral order: nothing (§4.4's do-nothing guarantee, which is what
        // makes an MFO follower byte-identical to a vanilla one when idle).
        if (!a_follower || a_choice.ruleIndex < 0) return { Result::NoOp, {} };

        const auto& op = a_choice.actionOpcode;

        if (op == Vocab::kActWait) {
            // Deliberately consumes the tick without acting -- the FFXII
            // "do nothing on purpose" idiom that lets a rule suppress the ones
            // below it (DESIGN.md §3.3).
            return { Result::NoOp, {} };
        }
        if (op == Vocab::kActCastSelf) {
            return CastOn(a_follower, a_choice.actionParam, a_follower);
        }
        if (op == Vocab::kActCastTarget) {
            return CastOn(a_follower, a_choice.actionParam,
                          ResolveTarget(a_follower, a_choice.subject));
        }

        // Unknown action opcode: fail closed and say so. Likely a rule from a
        // newer vocabulary; it must never fall through to something else.
        return { Result::FailedOther, std::format("unknown action '{}'", op) };
    }

}
