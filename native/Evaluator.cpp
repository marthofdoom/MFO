#include "PCH.h"
#include <chrono>          // brawl-gate log throttle (#34)
#include <unordered_map>   // brawl-gate log throttle (#34)
#include "Evaluator.h"
#include "Vocabulary.h"
#include "Logistics.h"   // supply-condition reads (counts of potions / arrows)
#include "Followers.h"   // g_active -- the maintained teammate list (ally selector)
#include "Config.h"      // g_sharedRadius -- "ally" locality
#include "Confidence.h"  // ChaseRadius -- the combat chase cap (#22)
#include "Sightline.h"   // LoS preference in PickFoe (worker-safe cached read)

namespace MFO::Eval {

    namespace {

        // Is this opcode a FOE SELECTOR -- i.e. does it choose a target as well
        // as answer true/false?
        bool IsFoeSelector(const std::string& a_op) {
            return a_op == Vocab::kCondFoeAny        || a_op == Vocab::kCondFoeHpBelow  ||
                   a_op == Vocab::kCondFoeLowestHp   || a_op == Vocab::kCondFoeHighestHp ||
                   a_op == Vocab::kCondFoeWithinRange|| a_op == Vocab::kCondFoeBeyondRange ||
                   a_op == Vocab::kCondFoeAttackingPlayer || a_op == Vocab::kCondFoeAttackingMe ||
                   a_op == Vocab::kCondFoeAttackingMeMelee || a_op == Vocab::kCondFoeAttackingMeRanged ||
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
        // Count live, engaged foes in the follower's own combat group. The
        // traversal now lives in CombatSense.h so Confidence (#23) and the
        // cond.foe_count_at_least gambit read the exact same tally.
        int FoeCount(RE::Actor* a_self) { return CombatSense::FoeCount(a_self); }

        // Surface the ALL-OCCLUDED fallback (throttled, outside the lock): every
        // qualified foe failed the LoS preference, so the selector fell back to
        // the old distance/HP pick rather than going passive. One line per ~5 s
        // per follower -- enough for the deck log to show the gate deciding,
        // quiet enough not to drown it.
        void LogOccludedFallback(RE::Actor* a_self, int a_count) {
            static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> s_next;
            const auto now = std::chrono::steady_clock::now();
            auto& nxt = s_next[a_self->GetFormID()];
            if (nxt.time_since_epoch().count() != 0 && now < nxt) return;
            nxt = now + std::chrono::seconds(5);
            spdlog::info("[los] {:08X}: all {} candidate foe(s) occluded -- falling back to "
                         "the unsighted pick (he still engages; the FORCED cast stays held)",
                         a_self->GetFormID(), a_count);
        }

        // Surface the brawl gate ONLY when it suppressed the entire candidate
        // set (a fist-fight opponent was the follower's only "foe"), so a field
        // test can tell "gambit did nothing" from "correctly held fire." Throttled
        // per follower, and called OUTSIDE the combat-group lock.
        void LogBrawlSkip(RE::Actor* a_self, RE::FormID a_foe, int a_count) {
            static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> s_next;
            const auto now = std::chrono::steady_clock::now();
            auto& nxt = s_next[a_self->GetFormID()];
            if (nxt.time_since_epoch().count() != 0 && now < nxt) return;
            nxt = now + std::chrono::seconds(5);
            spdlog::info("[brawl] {:08X}: held fire -- {} non-hostile foe(s) in group "
                         "(e.g. {:08X}), no real target. Brawl / proving fight?",
                         a_self->GetFormID(), a_count, a_foe);
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

            // TARGET-RELATIVE RANGE (marth): "foe targeted within/beyond range"
            // reads the distance to the follower's CURRENT combat target -- the foe
            // they are actually engaged with -- NOT the nearest foe of that range.
            // This is what makes an equip-melee "targeted within" / equip-ranged
            // "targeted beyond" PAIR key off the SAME foe. The old any-foe scan let
            // a DISTANT foe satisfy "beyond" while the follower was toe-to-toe with
            // a close one, so a bow gambit fought his own AI for the melee he was
            // already in -- the observed equip thrash (D5/H6). No current target ->
            // the condition is false and the rule falls through. Reads only the
            // handle (no combat-group lock); brawl gate still applies.
            if (a_op == Vocab::kCondFoeWithinRange || a_op == Vocab::kCondFoeBeyondRange) {
                auto tp   = a_self->GetActorRuntimeData().currentCombatTarget.get();
                auto* tgt = tp.get();
                if (!tgt || tgt->IsDead() || tgt->IsDisabled()) return best;
                if (!tgt->IsHostileToActor(a_self)) return best;   // don't act in a brawl
                const float d  = a_self->GetPosition().GetDistance(tgt->GetPosition());
                const bool  ok = (a_op == Vocab::kCondFoeWithinRange) ? (d <= a_param)
                                                                      : (d > a_param);
                if (ok) best = tgt->GetHandle();
                return best;
            }

            auto& rt = a_self->GetActorRuntimeData();
            auto* cc = rt.combatController;
            if (!cc || !cc->combatGroup) return best;

            // Lowest `score` wins. Distance for the nearest-style selectors, HP
            // for the HP ones (negated for "highest"). A gate `continue`s a foe
            // that does not qualify at all.
            //
            // LoS PREFERENCE (soft, fail-open): two parallel bests -- the best
            // SIGHTED candidate (verdict Visible or Unknown) and the best
            // overall. The sighted one wins when it exists; when EVERY
            // candidate is measured Occluded, fall back to the old pick so the
            // follower still engages (repositioning is the AI's job; only the
            // FORCED cast requires sight, in Actuation). Unknown counts as
            // sighted on purpose: a cold cache or a runtime that cannot
            // raycast must behave exactly like today, not go passive.
            float bestScore    = std::numeric_limits<float>::max();
            float bestVisScore = std::numeric_limits<float>::max();
            RE::ActorHandle bestVis;
            int   qualified    = 0;       // candidates that passed every gate
            std::vector<RE::FormID> wantLoS;   // measured NEXT tick, on main
            const auto selfID  = a_self->GetFormID();
            const auto selfPos = a_self->GetPosition();
            int         nonHostile = 0;   // brawl gate: foes skipped as non-hostile
            RE::FormID  lastNH     = 0;

            // Compute the chase cap ONCE, and BEFORE taking the combat-group lock.
            // ChaseRadius -> Confidence::Of -> CombatSense::FoeCount ALSO reads
            // cc->combatGroup under that same lock (#23). Calling it inside the
            // loop below (as this used to) would nest a read-lock inside the read
            // lock we hold here -- benign only until the main thread's combat AI
            // takes the WRITE lock between the two acquisitions, at which point
            // both threads spin forever (worker holds read#1, waits on read#2;
            // main holds the write bit, waits for readers to drain). Hoisting it
            // out removes the nesting entirely -- and the cap is per-follower, not
            // per-candidate, so this is also strictly less work.
            const float chaseCap = Confidence::ChaseRadius(a_self);

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

                    // BRAWL / PROVING-FIGHT GATE (#34): the engine keeps a
                    // NON-HOSTILE sparring partner in the combat group -- a tavern
                    // brawl, or the Companions' proving fight vs Vilkas. Latching a
                    // real attack or offensive spell onto them turns a fist-fight
                    // into an assault (a bounty, a broken quest, a friend made an
                    // enemy). A foe the follower is not actually hostile to is never
                    // a valid gambit target. IsHostileToActor is the engine's own
                    // relationship+combat read (proven available in Probe). Skips
                    // are counted and surfaced after the lock (see LogBrawlSkip).
                    //
                    // TRADE-OFF (kept deliberately): a real foe attacking only the
                    // PLAYER can read non-hostile-to-follower for the brief window
                    // before aggro propagates, so the follower may not pre-empt it.
                    // That window self-corrects, and the alternative -- exempting
                    // the "attacking player" selector from this gate -- would let a
                    // follower join the player's BRAWL (the opponent is swinging at
                    // the player too), which is the exact bug this closes. The
                    // [brawl] log surfaces real passivity if the soak shows any.
                    if (!foe->IsHostileToActor(a_self)) {
                        ++nonHostile; lastNH = foe->GetFormID();
                        continue;
                    }

                    const float dist = selfPos.GetDistance(foe->GetPosition());

                    // CONFIDENCE CHASE CAP (#22): never auto-select a foe beyond
                    // the follower's confidence-scaled chase radius, so a distance-
                    // blind selector ("attack the weakest / nearest / undead ...")
                    // can't march him across a pack to a far target (Erik's Falmer
                    // charge). Hurt/mobbed shrinks it; healthy widens it. (The
                    // target-relative range conditions returned above, so they
                    // never reach this scan.)
                    if (dist > chaseCap) continue;

                    float score = dist;   // default for the gate-style selectors

                    if (a_op == Vocab::kCondFoeAny) {
                        score = dist;
                    } else if (a_op == Vocab::kCondFoeLowestHp || a_op == Vocab::kCondFoeHpBelow) {
                        const float hp = Vocab::HealthPct(foe);
                        if (a_op == Vocab::kCondFoeHpBelow && hp >= a_param) continue;
                        score = hp;
                    } else if (a_op == Vocab::kCondFoeHighestHp) {
                        score = -Vocab::HealthPct(foe);
                    } else if (a_op == Vocab::kCondFoeAttackingPlayer) {
                        if (!FoeTargets(foe, a_player)) continue;
                    } else if (a_op == Vocab::kCondFoeAttackingMe) {
                        if (!FoeTargets(foe, a_self)) continue;
                    } else if (a_op == Vocab::kCondFoeAttackingMeMelee) {
                        // Targeting the follower AND swinging steel (not a bow, not a
                        // spell) -- the "peel this one off me" rule for a mixed pack.
                        if (!FoeTargets(foe, a_self) || FoeIsRanged(foe) || FoeIsCaster(foe)) continue;
                        // A staff is a ranged caster tool, not melee (Fable).
                        if (auto* rw = foe->GetEquippedObject(false); rw)
                            if (auto* w = rw->As<RE::TESObjectWEAP>();
                                w && w->GetWeaponType() == RE::WEAPON_TYPE::kStaff) continue;
                    } else if (a_op == Vocab::kCondFoeAttackingMeRanged) {
                        if (!FoeTargets(foe, a_self) || !FoeIsRanged(foe)) continue;
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

                    // Every gate passed -- a real candidate. Queue its LoS
                    // measurement (runs on the MAIN thread next frame, #72:
                    // collect ids here, act after the lock) and read the
                    // CACHED verdict for this tick's preference.
                    ++qualified;
                    wantLoS.push_back(foe->GetFormID());
                    if (Sightline::Check(selfID, foe->GetFormID()) !=
                        Sightline::Verdict::Occluded) {
                        if (score < bestVisScore) { bestVisScore = score; bestVis = t.targetHandle; }
                    }

                    if (score < bestScore) { bestScore = score; best = t.targetHandle; }
                }
            }
            // OUTSIDE the group lock: ask the main thread to (re)measure LoS
            // for this tick's candidates so the next tick reads warm verdicts.
            if (!wantLoS.empty()) Sightline::Want(selfID, std::move(wantLoS));

            // The sighted best wins; the unsighted overall best is the
            // fallback so an all-occluded pack never reads as "no foe".
            if (bestVis) {
                best = bestVis;
            } else if (best && qualified > 0) {
                LogOccludedFallback(a_self, qualified);
            }
            // Held fire in a brawl: the only "foes" were non-hostile and nothing
            // real was selected. Log outside the group lock (throttled).
            if (nonHostile > 0 && !best) LogBrawlSkip(a_self, lastNH, nonHostile);
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

            // A PLAYER-targeting condition names the PLAYER as the target, so
            // "Player HP % below -> Cast on target" heals YOU, not the follower
            // (Fable RC#6: subjectSelector was never authored, so Subject::Player was
            // dead and the cast fell back to Self). Self/other conditions leave the
            // target empty and the action resolves its own subject as before.
            // Gated to Cast-on-target ONLY: Attack/PowerAttack also read the target,
            // and "Player HP% below -> Attack" must NOT turn the follower on you
            // (Opus RC-review: un-gated player targeting was friendly-fire).
            if (!chosen && g.conditionOpcode == Vocab::kCondPlayerHpBelow &&
                g.actionOpcode == Vocab::kActCastTarget && player)
                chosen = player->GetHandle();

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
