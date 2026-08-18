#include "PCH.h"
#include "Actuation.h"
#include "Vocabulary.h"
#include "Config.h"
#include "Loadout.h"
#include "Papyrus.h"
#include "CasterConsent.h"
#include "Targeting.h"
#include "Logistics.h"
#include "Followers.h"   // #68: g_active -- NearestAlly walks the maintained teammate list
#include "Serialization.h" // #76: FWPN record ids for the force-hold co-save
#include "MainThread.h"   // #76: defer the load-time force-lock release to the main thread
#include <limits>        // #68: std::numeric_limits for NearestAlly's distance seed
#include "Packages.h"    // #35: act.flee reuses the retreat package; hybrid forced cast
#include "Sightline.h"   // LoS gate on the forced cast -- no firebolts into walls
#include "Temperament.h" // flair #1: per-follower timing seed (grace offset)

#include <optional>      // ForceCast's tri-state return -- not in the PCH

namespace MFO::Actuation {

    // #76: the weapon a follower is being force-held on, keyed by FormID. Written
    // by EquipWeapon (the fire) and cleared by ReleaseForcedWeapon/Reconcile/
    // ClearForcedWeapons -- ALL on the job-worker serial tick, the same thread
    // the scheduler already drives Fire on, so no lock (the g_followers
    // discipline: #4). Persists ACROSS ticks by design -- it is engine-hold
    // bookkeeping like CombatStyle's owned-stance map, not per-scan evaluator
    // state (#22), and is never read back by the evaluator. Session-scoped:
    // cleared on revert (ClearForcedWeapons). The pointer is a weapon form, which
    // is stable for the session.
    std::unordered_map<RE::FormID, RE::TESBoundObject*> g_forcedWeapon;
    // SEV-1 (Fable 2026-08-17): the worker tick mutates this map, but the SKSE
    // SAVE callback reads it (CoSaveForcedWeapons) off that thread with no pump
    // barrier -- a concurrent insert would invalidate its iterator mid-save
    // (CTD / truncated save). Guard every access; NEVER hold it across an
    // Equip/UnequipObject engine call (the MSTK "copy then act" discipline).
    std::mutex g_forcedMx;

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

        // THE RESOLUTION LADDER (#68). Given a cast-target choice, decides WHO
        // gets cast at, first rung to match wins:
        //   1 (+3) a selector (or Evaluate()'s player-HP-condition special
        //     case) already named someone THIS TICK -- a_choice.target.
        //   2   an explicit subject the player picked on the row: a SPECIFIC
        //       follower (subjectActorForm) takes precedence over the enum
        //       when both are somehow set; Self/Player always resolve;
        //       NearestAlly falls on down the ladder if nobody is in range.
        //   4   otherwise -- THE PLAYER. The catch-all for actor-agnostic
        //       rules ("when dark -> cast Magelight") that would otherwise
        //       resolve to nobody. a_outIsFallbackPlayer marks this rung so
        //       CastOn's out-of-range skip (below) knows to WAIVE it -- a
        //       fallback must always fire, never quietly vanish because the
        //       player happens to be far away.
        RE::Actor* ResolveCastTarget(RE::Actor* a_follower, const Eval::Choice& a_choice,
                                     bool& a_outIsFallbackPlayer) {
            a_outIsFallbackPlayer = false;
            if (auto ptr = a_choice.target.get(); ptr.get()) return ptr.get();

            auto* player = RE::PlayerCharacter::GetSingleton();

            // A SPECIFIC follower, gone/dead/unloaded falls on down the
            // ladder rather than bailing -- a "questionable" target must
            // never wall off the whole rule (the #68 problem statement).
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
                // #68 (marth): on a CAST-AT-TARGET row the default subject
                // (Self=0 -- what every pre-#68 row and every freshly-added row
                // carries) means AUTO: run the ladder to the PLAYER fallback, so
                // an actor-agnostic rule ("when dark -> cast Magelight") lights
                // up around the player instead of resolving to nobody (the #68
                // bug) or to the caster. Deliberate self-casting is the separate
                // "Cast on self" action (kActCastSelf), which never routes
                // through this ladder. A stale specific-follower sentinel (0xFF)
                // lands here too and correctly falls through rather than
                // disarming the rule.
                break;
            }

            a_outIsFallbackPlayer = true;
            return player;
        }

        // THE HYBRID'S FORCE HALF. The AI-first grace (below, in CastOn) stays
        // the preferred path because it is MOBILE -- the follower strafes and
        // closes while casting, which no package cast does (§0.27 roots him).
        // But when the AI MISSED -- it cast a DIFFERENT spell during the grace
        // (Marcurio's Chain-Lightning-over-Firebolt, the swap that also resets
        // the grace clock forever), or the grace elapsed with no cast of ours
        // -- the configured spell is FORCED through the proven-but-unwired
        // package route (§0.21-0.23: animated, targeted, every axis proven).
        //
        // Returns nullopt when the package route is unavailable/declined
        // structurally, so CastOn falls through to the legacy silent apply --
        // never a regression, exactly the old grace-expiry behaviour.
        std::optional<Outcome> ForceCast(RE::Actor* a_follower, RE::SpellItem* a_spell,
                                         RE::Actor* a_target, bool a_aiCastOther) {
            if (!Config::g_forceCastOnMiss.load()) return std::nullopt;
            if (!Packages::Available())            return std::nullopt;

            const auto id   = a_follower->GetFormID();
            const bool self = !a_target || a_target == a_follower;

            // LINE OF SIGHT, required for the forced shot only. The AI path
            // repositions on its own; a package cast fires from where he
            // stands, so an OCCLUDED verdict holds the force and lets the
            // rules below (attack -> the AI closes/repositions) run instead.
            // TRANSPARENT: a wall between him and the foe must not wall off
            // the rest of his list too. Unknown passes -- fail-open, so a
            // runtime where the raycast cannot run (VR: no main-thread pump)
            // degrades to today's behaviour instead of an inert forced cast.
            if (!self) {
                const auto v = Sightline::Check(id, a_target->GetFormID());
                if (v == Sightline::Verdict::Occluded) {
                    spdlog::debug("[cast] {:08X}: forced cast HELD -- no line of sight to {:08X}",
                                  id, a_target->GetFormID());
                    return Outcome{ Result::NoOp, "forced cast held (no line of sight)", true };
                }
            }

            const auto d = self ? Packages::CastSelf(a_follower, a_spell)
                                : Packages::CastAt(a_follower, a_spell, a_target);
            if (d == Packages::Decline::None) {
                // The decision line the deck log needs: WHICH miss branch fired.
                spdlog::info("[cast] {:08X} {} FORCED {} ({:08X}) {} -- {} (LoS {})",
                             id, a_follower->GetName() ? a_follower->GetName() : "?",
                             a_spell->GetName() ? a_spell->GetName() : "?",
                             a_spell->GetFormID(),
                             self ? std::string("on self")
                                  : std::format("at {:08X}", a_target->GetFormID()),
                             a_aiCastOther ? "their AI cast a DIFFERENT spell"
                                           : "grace elapsed, no cast",
                             self ? "n/a"
                                  : Sightline::VerdictName(
                                        Sightline::Check(id, a_target->GetFormID())));
                // The package carries the spell itself, but the latch stays
                // (v1.0.30): clearing consent here re-opened the same
                // between-casts gap the sink's Clear did -- until the next
                // service re-Want()ed, the AI could answer the forced cast
                // with a cast of its OWN. NoteOurCast retires the miss flag
                // (it was just consumed -- kept, it would short-circuit the
                // re-armed grace below into an instant re-force) and keeps
                // the deny standing. Give the hand back on the cooldown, and
                // re-arm the grace so the NEXT firing of this rule offers the
                // AI a fresh window.
                CasterConsent::NoteOurCast(id);
                Loadout::ArmGrace(id);
                Loadout::StartCooldown(id);
                return Outcome{ Result::Fired,
                                a_aiCastOther ? "forced cast (AI cast its own spell)"
                                              : "forced cast (grace elapsed)" };
            }
            if (d == Packages::Decline::Busy) {
                // Another follower holds the single cast alias -- a seconds-
                // long wait during which this one FIGHTS; transparent so the
                // rules below run (same shape as the loadout debounce).
                return Outcome{ Result::NoOp, "cast package busy", true };
            }
            // Contention / no record / quest stopped: structural. Fall through
            // to the silent path rather than dropping the rule -- the decline
            // was already logged with its reason by Packages.
            return std::nullopt;
        }

        // ── CONCENTRATION: the BOUNDED STREAM (v1.0.53 deck freeze) ──────────
        // A concentration spell has no "one cast" for the fire-and-forget
        // machinery to observe: force-YESing it to the AI made a PERMANENT
        // held stream (Lucien's Flames through Xelzaz -- the freeze), and
        // skipping it was rejected -- exact bounding must cover EVERY spell
        // class, no AI escape hatch (marth). So MFO streams it ITSELF through
        // the proven package route (§0.21/0.22: concentration casts animated
        // on every probed axis), with the bound stated UP FRONT and enforced
        // by Packages::Pump each tick:
        //   hostile  -- 1-4 s by temperament (each mage's breath is
        //               consistently their own), cut EARLY the moment a
        //               teammate crosses the line of fire;
        //   heal     -- until the target tops off, capped;
        //   utility  -- a capped hold; "still relevant" is the rule still
        //               WINNING, which re-fires another bounded stream after
        //               the cooldown rather than holding one open forever.
        // Release is eviction + InterruptCast (Packages::ClearAlias), so the
        // beam dies with the package. The AI-first grace is deliberately NOT
        // offered here: the CheckStartCast hook denies the AI's own attempt
        // at a wanted concentration spell (an AI channel cannot be bounded),
        // so the package stream is the ONLY open channel. Every exit below
        // is bounded: a dispatched hold, a transparent wait, or a legible
        // failure (§5.3) -- never "leave it to their AI".
        constexpr float kConcHealCap     = 6.0f;   // heal stream hard cap
        constexpr float kConcUtilityHold = 4.0f;   // utility hold cap

        Outcome ConcentrationCast(RE::Actor* a_follower, RE::SpellItem* a_spell,
                                  RE::Actor* a_target) {
            const auto id   = a_follower->GetFormID();
            const bool self = !a_target || a_target == a_follower;

            // SELF concentration: routed to the UNIVERSAL direct trigger, not the
            // package (SPEC-self-cast-forced). In practice CastOn intercepts self
            // BEFORE this fork, so this is defence-in-depth -- but it must never
            // fall to the inert alias package, which equips but never fires and
            // is declined outright on package-locked custom followers.
            if (self) {
                if (Config::g_castSelf.load() && CastSelfDirect(a_follower, a_spell))
                    return { Result::Fired, "self-cast (direct trigger)" };
                return { Result::FailedOther, "self-cast could not fire", true };
            }

            // No package, no stream (FOE concentration only). The silent fallback
            // is meaningless for a channel (an instant apply of a per-second
            // effect), so this fails LEGIBLY -- transparent, the rules below run.
            if (!Config::g_forceCastOnMiss.load() || !Packages::Available()) {
                return { Result::FailedOther,
                         "concentration needs the cast package (bForceCastOnMiss+bUsePackages)",
                         true };
            }
            // SELF concentration (SPEC-self-cast-forced): self-heal, self-ward,
            // any authored cast_self channel. Served by the dedicated no-QNAM t6
            // self package via Packages::CastSelf, GATED behind bCastSelf. With
            // the gate off, CastSelf returns Decline::SelfRoute and this falls to
            // the transparent structural decline below -- the caller's rules run,
            // exactly the pre-feature behaviour. A self-cast needs NO line-of-
            // fire gate (nothing to friendly-fire) and no target-LoS check.
            // PACING -- the same fCastCooldown every other gambit cast obeys,
            // consulted directly (no equip machinery on this path to consult
            // it for us). This is what spaces one bounded stream from the
            // next. Same reason string as Prepare's, for the transition log.
            if (Loadout::CoolingDown(id)) {
                return { Result::NoOp, "cast cooling down", true };
            }
            // LoS: never stream into a wall (the forced shot's gate). Foe only.
            if (!self && Sightline::Check(id, a_target->GetFormID()) ==
                Sightline::Verdict::Occluded) {
                return { Result::NoOp, "forced cast held (no line of sight)", true };
            }

            const auto kind = CasterConsent::ClassifySpell(a_spell);

            // THE LINE-OF-FIRE GATE, hostile FOE streams, NOT optional: friendly
            // fire from a stream is what triggered the freeze; the #63 quash
            // is a backstop, never a license. The same check re-runs every
            // Pump tick mid-stream (ffWatch) and cuts the beam if someone
            // walks into it. A SELF stream has no line of fire, so it is exempt.
            if (!self && kind == CasterConsent::SpellKind::Offense &&
                Sightline::TeammateInFireLine(id, a_target->GetFormID())) {
                return { Result::NoOp,
                         "concentration held (teammate in the line of fire)", true };
            }

            Packages::CastHold hold;
            const char*        kindName = "utility";
            if (kind == CasterConsent::SpellKind::Offense) {
                hold.holdSeconds = 1.0f + 3.0f * Temperament(id);   // 1-4 s, flair #1
                hold.ffWatch     = !self;   // no line of fire to watch on a self stream
                kindName         = self ? "self-offense" : "hostile";
            } else if (kind == CasterConsent::SpellKind::Heal) {
                hold.holdSeconds = kConcHealCap;
                hold.healWatch   = self ? id : a_target->GetFormID();
                kindName         = self ? "self-heal" : "heal";
            } else {
                hold.holdSeconds = kConcUtilityHold;
                kindName         = self ? "self-utility" : "utility";
            }

            const auto d = self ? Packages::CastSelf(a_follower, a_spell, hold)
                                : Packages::CastAt(a_follower, a_spell, a_target, hold);
            if (d == Packages::Decline::None) {
                // Exclusive control while the rule governs: the latch's DENY
                // of other spells is the exact-mode bounding, and the
                // concentration deny in CheckStartCast keeps the PERMIT half
                // off, so the latch can never re-arm an AI stream. The
                // cooldown paces the next stream.
                CasterConsent::Want(id, a_spell->GetFormID());
                Loadout::StartCooldown(id);
                spdlog::info("[cast] {:08X} {} CONCENTRATION {} ({:08X}) {} -- "
                             "{} stream, hold {:.1f}s{}",
                             id, a_follower->GetName() ? a_follower->GetName() : "?",
                             a_spell->GetName() ? a_spell->GetName() : "?",
                             a_spell->GetFormID(),
                             self ? std::string("on self")
                                  : std::format("at {:08X}", a_target->GetFormID()),
                             kindName, hold.holdSeconds,
                             hold.healWatch ? " (or until healed)" : "");
                return { Result::Fired, "concentration stream (bounded)" };
            }
            if (d == Packages::Decline::Busy) {
                // Our own live stream lands here every tick while it runs --
                // the wait IS the action; transparent like the FF form.
                return { Result::NoOp, "cast package busy", true };
            }
            // Structural decline -- Packages already logged the reason.
            return { Result::FailedOther, "concentration cast declined (see [pkg])", true };
        }

        // a_rangeGate (#68): true only for an OBVIOUS target (ladder rungs
        // 1-3 -- a selector, an explicit subject, a condition-implied actor).
        // The PLAYER FALLBACK (rung 4) always passes false: a rule that
        // exists to cover "nobody obvious applies" must fire regardless of
        // distance, or the fallback itself becomes just another thing that
        // can silently not happen. kActCastSelf/kActCastPlayer never set
        // this (unchanged -- they are not part of the #68 ladder at all).
        Outcome CastOn(RE::Actor* a_follower, RE::FormID a_spellID, RE::Actor* a_target,
                       bool a_rangeGate = false) {
            // #67 SE/VR GUARD (mirrors the CasterConsent hook guards). The mage
            // cast-control path CRASHES on Skyrim SE 1.5.97: a reporter's crash log
            // pinned an EXCEPTION_ACCESS_VIOLATION to Scheduler::Tick -> Actuation::
            // Fire -> CastOn on the SKSE job worker (byte read off a poisoned
            // pointer), an SE-only divergence in the equip/cost work below. The
            // forced-cast PACKAGE route already declines off AE, but CastOn's own
            // Loadout::Prepare (spell equip) + CalculateMagickaCost run FIRST and
            // are what fault. This whole feature is AE-developed and AE-tested, so
            // off AE we decline the cast rule TRANSPARENTLY -- the follower's own
            // vanilla AI keeps casting (mobile, animated), exactly the graceful
            // degradation the VR guards already give. Gate here (not just the
            // package route) so no cast-control code runs at all off AE.
            if (!REL::Module::IsAE())
                return { Result::FailedOther,
                         "cast control is AE-only (SE/VR use the follower's own AI casting)", true };
            // TRANSPARENT (GAMBIT_FLOWS §2): a cast that provably cannot run this
            // tick must not wall off the rules below it -- FFXII skips an
            // unaffordable gambit and runs the next line.
            if (!a_target) return { Result::FailedOther, "no valid target", true };

            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!spell) {
                return { Result::FailedOther,
                         std::format("spell {:08X} not in load order", a_spellID), true };
            }

            // #68 OUT-OF-RANGE SKIP, obvious targets only (a_rangeGate).
            // Self and Touch delivery are CONTACT/self spells -- there is no
            // "aimed" range to exceed, so they are never out of range no
            // matter the distance. For Aimed/TargetActor/TargetLocation,
            // SpellItem::GetRange() is the SPIT record's own Range field
            // (confirmed in the pinned CommonLibSSE-NG headers: RE::MagicItem
            // declares the virtual, RE::SpellItem overrides it to return
            // data.range). A ZERO/unset range FAILS OPEN -- plenty of vanilla
            // spells ship with no declared cap, and reading that as "cannot
            // reach anyone" would silently disable rules on data we cannot
            // read confidently, exactly the failure mode §5.3 exists to avoid.
            // TRANSPARENT (GAMBIT_FLOWS §2): a target merely being far away
            // must not wall off the gambits below this one.
            if (a_rangeGate && a_target && a_target != a_follower) {
                const auto delivery = spell->GetDelivery();
                if (delivery != RE::MagicSystem::Delivery::kSelf &&
                    delivery != RE::MagicSystem::Delivery::kTouch) {
                    const float range = spell->GetRange();
                    if (range > 0.0f) {
                        const float dist = a_follower->GetPosition().GetDistance(a_target->GetPosition());
                        if (dist > range) {
                            return { Result::NoOp,
                                     std::format("target beyond {} range ({:.0f}/{:.0f})",
                                                 spell->GetName() ? spell->GetName() : "spell",
                                                 dist, range),
                                     true };
                        }
                    }
                }
            }

            // COMPETENCE IS NOT PERMISSION (DESIGN.md §5.3). MFO does not top up
            // magicka, discount the cost, or substitute a cheaper spell -- if
            // the follower cannot afford it the rule FAILS and the tick falls
            // through, and the board says why. This is the whole point: a list
            // you wrote that your follower cannot run is legible, not silent.
            auto* avo = a_follower->AsActorValueOwner();
            float cost = 0.0f;
            if (avo) {
                cost = spell->CalculateMagickaCost(a_follower);
                const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
                if (cost > have) {
                    // Take it back. The rule keeps winning while the condition
                    // holds, so wantsCast stays true and the scheduler's release
                    // never runs -- leaving a spell they cannot afford in their
                    // hand for their AI to keep trying.
                    Loadout::ReleaseSpell(a_follower->GetFormID());
                    spdlog::debug("[eval] {:08X} has {:.0f} magicka, needs {:.0f}",
                                  a_follower->GetFormID(), have, cost);
                    // TRANSPARENT: this is the spellsword flow -- reserve/empty
                    // pool falls through to steel (GAMBIT_FLOWS D1, §3.5).
                    return { Result::FailedSkill,
                             std::format("insufficient magicka (needs {:.0f})", cost), true };
                }

                // THE RESERVE. §5.3 says the follower's competence decides what
                // they can run -- but a gambit that empties the pool leaves them
                // unable to do anything ELSE they know, which is not what a
                // player means by "heal yourself when hurt". Keep a floor.
                // M2 from review: the floor is about what is left AFTER the
                // cast. "Do not start below 25%" still lets a 40%-of-pool spell
                // finish at nearly zero, which is not what "keep something for
                // everything else you know" means.
                const float reserve = Config::g_magickaReserve.load();
                if (reserve > 0.0f) {
                    auto* avo2 = a_follower->AsActorValueOwner();
                    const float mx = avo2
                        ? avo2->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                          a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                            RE::ActorValue::kMagicka)
                        : 0.0f;
                    if (mx > 0.0f && (have - cost) < reserve * mx) {
                        Loadout::ReleaseSpell(a_follower->GetFormID());
                        return { Result::FailedSkill,
                                 std::format("magicka reserve (floor {:.0f})", reserve * mx),
                                 true };   // transparent -- fall to steel (§3.5)
                    }
                }
            }

            // SELF-CAST forks off FIRST (SPEC-self-cast-forced): a bCastSelf-
            // armed cast_self -- concentration OR fire-and-forget -- fires
            // through the UNIVERSAL direct trigger (CastSelfDirect), BEFORE the
            // concentration fork and before the equip/grace/package machinery.
            // Self needs neither AI-grace nor an alias package (which package-
            // locked custom followers decline), so it bypasses both. It is a
            // CHANNEL, self-paced by its own registry -- do NOT gate on
            // Loadout::CoolingDown or call StartCooldown: StartCooldown ->
            // ReleaseSpell would rip the spell out of the hand and kill the
            // animation. Re-fire it every combat tick while the rule wins (it
            // refreshes the channel + applies the effect at fCastCooldown
            // cadence); the reconcile releases it when the rule goes false.
            if (a_target == a_follower && Config::g_castSelf.load()) {
                if (CastSelfDirect(a_follower, spell))
                    return { Result::Fired, "self-cast channel (direct trigger)" };
                // Unaffordable / off-AE / no caster: transparent, the rules
                // below run (the follower is not stuck on a cast that can't go).
                return { Result::FailedOther, "self-cast could not fire", true };
            }

            // CONCENTRATION forks off HERE -- after the range and competence
            // gates (a stream obeys §5.3 and #68 like any cast), BEFORE the
            // equip/grace/force machinery, all of which assumes a
            // fire-and-forget release to observe. The bounded stream is its
            // own actuation (see ConcentrationCast above). (Self concentration
            // never reaches here -- the self fork above intercepts it.)
            if (spell->GetCastingType() ==
                RE::MagicSystem::CastingType::kConcentration) {
                return ConcentrationCast(a_follower, spell, a_target);
            }

            // THE FOLLOWER CASTS IT -- MFO does not cast on their behalf.
            //
            // ActorMagicCaster is driven by the ANIMATION GRAPH (it inherits
            // SimpleAnimationGraphManagerHolder and sinks BSAnimationGraphEvent),
            // so CastSpellImmediate can never animate no matter which casting
            // source issues it -- tested across all four, ENGINE_NOTES §0.13.
            // The only animated path is the vanilla one every enemy mage uses:
            // put the spell in a hand and let the actor cast.
            //
            // The off hand is what makes this affordable (DESIGN §4.5b).
            // ONE new mechanism this release (#45): equipping. The cast
            // itself still goes through CastSpellImmediate, deliberately.
            //
            // `Projectile::LaunchSpell` was the obvious partner and is WRONG
            // here: it launches a projectile, and a Self-delivery spell -- the
            // flagship `Self HP < 40% -> Cast Healing` gambit -- has no
            // projectile to launch. That path would have equipped the spell and
            // applied NOTHING, strictly worse than the silent heal it replaced.
            // It also bypasses MagicCaster entirely, so it spends no magicka,
            // which would make §5.3's gate a tautology (ENGINE_NOTES §0.9).
            //
            // So: put the spell in their hand, then cast through THAT HAND's
            // caster. Whether holding the spell is enough to make the graph
            // animate is the open question this build exists to answer -- and
            // if the answer is no, behaviour is exactly what it is today rather
            // than a regression.
            bool equipped = false;
            if (Config::g_equipToCast.load()) {
                std::string why;
                switch (Loadout::Prepare(a_follower, spell, why)) {
                case Loadout::Ready::AlreadyReady:
                case Loadout::Ready::Equipped: {
                    equipped = true;

                    // INFLUENCE (§0.28). The spell is now in their hand; latch
                    // CONSENT so their own combat AI's CheckStartCast returns
                    // true for it. The AI then casts it AS a combat action --
                    // mobile, animated, correctly timed -- rather than vetoing
                    // it (§0.16). This is why the grace wait below now usually
                    // succeeds: MFO removed the veto that made it fail. No-op
                    // if bCasterHook is off; observe-only in log mode.
                    CasterConsent::Want(a_follower->GetFormID(), spell->GetFormID());

                    // GIVE THE FOLLOWER'S OWN AI A CHANCE FIRST.
                    //
                    // This is the whole point and it is easy to destroy. The
                    // animated cast comes from the follower's combat AI firing
                    // a spell we put in their hand -- but if MFO silent-casts
                    // in the same tick it equips, the heal lands instantly, the
                    // low-HP condition disappears, and the AI never has a
                    // reason to cast. The evidence we are trying to collect is
                    // destroyed by the act of collecting it: exactly the §0.6
                    // confound, rebuilt.
                    //
                    // So: hold off for fAiCastGrace. If a [cast] line appears in
                    // that window, the AI did it and it animated. If the window
                    // passes and the condition is still true, fall through to
                    // the silent cast -- an unanimated heal beats no heal.
                    const float held = Loadout::SecondsSinceEquip(a_follower->GetFormID());
                    // FLAIR #6: each caster has their own patience. The MCM knob
                    // stays the CENTER; the temperament seed spreads the band
                    // +-13% so two mages who both fail to animate do not land
                    // twin silent heals on the same beat. Deterministic per
                    // follower; the reason string (the dedup key) is unchanged.
                    const float grace = Config::g_aiCastGrace.load() *
                        (0.87f + 0.26f * Temperament(a_follower->GetFormID()));

                    // THE MISS DETECTOR. "The AI cast a different spell" ends
                    // the grace EARLY: that cast swapped the equipped spell,
                    // MFO re-equipped it this tick, and SecondsSinceEquip
                    // restarted -- so without this flag the grace clock resets
                    // forever and the configured spell is never cast (the
                    // Marcurio/Firebolt field report). A cast of OUR spell is
                    // the success path and never reaches here: the sink starts
                    // the cooldown and retires the miss flag -- keeping the
                    // latch (v1.0.30) -- so Prepare() debounces the next tick
                    // with the deny still standing.
                    const bool aiCastOther =
                        CasterConsent::OtherCastSeen(a_follower->GetFormID());
                    if (held < grace && !aiCastOther) {
                        // A STABLE reason string. The transition logger compares
                        // reasons, so embedding the elapsed time here would make
                        // every tick a "new" reason and log at 7.5 Hz.
                        // OPAQUE (marth, GAMBIT_FLOWS §7.1): the hold IS the cast
                        // happening -- firing lower rules mid-grace risks
                        // disturbing the AI's cast (the §0.6 confound).
                        return { Result::NoOp, "waiting for their AI to cast it" };
                    }

                    // THE AI MISSED -- grace spent with nothing, or spent on
                    // its own spell. Force the CONFIGURED spell through the
                    // package route; only a structural decline falls through
                    // to the silent apply below (never a regression).
                    if (auto forced = ForceCast(a_follower, spell, a_target, aiCastOther)) {
                        return *forced;
                    }
                    break;
                }
                case Loadout::Ready::Debounced:
                    // NOT a rule failure -- the follower is willing and able,
                    // MFO is declining to thrash their gear.
                    //
                    // RE-ASSERT THE LATCH FIRST (v1.0.30). The rule is still
                    // WINNING -- this wait is part of the cast's own pacing --
                    // so exclusive control must hold through it. The sink no
                    // longer drops the latch on a cast, and (v1.0.32) neither
                    // does an H3 condition flicker -- but this branch is still
                    // reachable UNLATCHED: combat end clears the latch while a
                    // cooldown stamped in the LAST fight survives into the
                    // next one, so the first services of a new fight can land
                    // here with no latch standing. Without this line that
                    // left the deny off for the rest of the cooldown -- the
                    // residual leak. Idempotent overwrite (spell only, never
                    // the pace), same call the Ready path makes; observe-only
                    // in log mode, exactly like every other Want.
                    CasterConsent::Want(a_follower->GetFormID(), spell->GetFormID());
                    // TRANSPARENT (marth, GAMBIT_FLOWS §7.1): cast cooldown /
                    // two-handed debounce / gear debt are seconds-long waits
                    // during which the follower FIGHTS -- the rules below run.
                    return { Result::NoOp, why, true };
                case Loadout::Ready::Failed:
                default:
                    return { Result::FailedSkill, why, true };   // transparent (§2)
                }
            }

            // PROBE ONLY, DEFAULT OFF (bCommandCast).
            //
            // This was built believing Actor.DoCombatSpellApply was Bethesda's
            // "cast this spell at THIS target, as a combat action" verb. IT IS
            // NOT. Actor.psc's own comment reads "Apply a spell to a target in
            // combat"; the Papyrus index positions it as the alternative to
            // AddSpell; and every shipped call site in ten modlists uses it as
            // an INSTANT SILENT APPLY -- including Bethesda's own Dawnguard
            // shield script, which uses it to eject the player. It is the
            // Papyrus twin of CastSpellImmediate.
            //
            // It stays behind a default-off flag for ONE measurement: whether a
            // proc-apply verb deducts magicka (if it does not, §0.9's finding
            // that our pre-check is the only gate stands unchanged). It is not
            // the animation answer, and the animation answer is not a spell
            // verb at all -- see ENGINE_NOTES §0.14.
            if (Config::g_commandCast.load() && Papyrus::Available()) {
                if (Papyrus::DoCombatSpellApply(a_follower, spell, a_target)) {
                    // Re-arm here too, or the one-shot bug simply survives on
                    // the bCommandCast + bEquipToCast combination.
                    if (equipped) Loadout::ArmGrace(a_follower->GetFormID());
                    return { Result::Fired, "dispatched" };
                }
                // Fall through to the silent path rather than dropping the
                // rule: an unanimated heal beats no heal.
            }

            // Cast from the hand the spell is actually in when we equipped it;
            // otherwise honour iCastSource. CastSpellImmediate is PROVEN to
            // deduct magicka (§0.9) and to work for self-delivery spells, which
            // is why it stays the verb.
            using CS = RE::MagicSystem::CastingSource;
            CS src = CS::kInstant;
            if (equipped) {
                // AlreadyReady can mean the spell was in the RIGHT hand all
                // along (a caster follower's own choice) -- match the hand the
                // spell is actually in. A freshly-queued equip may not read
                // back yet, so the left hand stays the default.
                src = (a_follower->GetEquippedObject(false) == spell &&
                       a_follower->GetEquippedObject(true)  != spell)
                          ? CS::kRightHand
                          : CS::kLeftHand;
            } else {
                switch (Config::g_castSource.load()) {
                case 0:  src = CS::kLeftHand;  break;
                case 1:  src = CS::kRightHand; break;
                case 2:  src = CS::kOther;     break;
                default: src = CS::kInstant;   break;
                }
            }
            // DRIVE THE CASTER (probe, bDriveCaster, default off).
            //
            // Every path that APPLIES a spell is silent, proven three times.
            // The animation comes from the caster's own state machine
            // advancing, which the combat AI normally kicks off. The animation
            // events cannot do it -- ten modlists use BeginCastLeft and its
            // siblings ONLY with RegisterForAnimationEvent, so the graph EMITS
            // them. That leaves the caster itself.
            //
            // EXPECTATION, stated so the result is falsifiable: this should
            // start the request and charge, and then WEDGE, because the release
            // step comes from the animation graph (MRh_SpellFire_Event ->
            // StartCastImpl) and nothing will play that animation. If so, the
            // shipped answer is a forced casting package instead (§0.17).
            //
            // The follower goes unhealed while this runs. That is deliberate:
            // a same-tick silent fallback would rebuild the §0.6 confound and
            // we could not tell which path produced the effect.
            if (Config::g_driveCaster.load() && equipped) {
                auto* hand = a_follower->GetMagicCaster(src);
                if (hand) {
                    // VERIFY, DO NOT FORCE. There is no SetCurrentSpell at the
                    // pinned CommonLib rev -- only SetCurrentSpellImpl, a no-op
                    // on ActorMagicCaster. Writing `currentSpell` by hand would
                    // desync the engine's own select/deselect bookkeeping, and
                    // the equip is queued so it may not have landed yet.
                    if (hand->currentSpell != spell) {
                        return { Result::NoOp, "caster has not selected the spell yet" };
                    }

                    // ONLY FROM REST. MSCO's shipped hook exists specifically to
                    // DENY RequestCastImpl once the caster is past its early
                    // states; re-requesting mid-sequence is how a follower ends
                    // up with charge-glow hands for a whole fight and a caster
                    // that accepts nothing further.
                    const auto st = hand->state.get();
                    if (st != RE::MagicCaster::State::kNone) {
                        hand->InterruptCast(true);      // refunds magicka
                        // The state number belongs in its OWN line, not in the
                        // reason: the reason is the dedup key, and a number that
                        // moves makes every tick a new message.
                        spdlog::info("[drive] {:08X} caster busy at state {} -- interrupted",
                                     a_follower->GetFormID(), static_cast<std::uint32_t>(st));
                        return { Result::NoOp, "caster busy -- interrupted" };
                    }

                    // Ask FIRST why it might refuse. This line is most of the
                    // probe's evidentiary value: it separates "the engine
                    // rejected the cast" from "the engine accepted it and the
                    // graph never released".
                    float strength = 1.0f;
                    RE::MagicSystem::CannotCastReason reason{};
                    const bool ok = hand->CheckCast(spell, false, &strength, &reason, false);

                    if (a_target) hand->desiredTarget = a_target->CreateRefHandle();
                    hand->RequestCastImpl();

                    // ORDER IS LOAD-BEARING: ArmGrace before StartCooldown, so
                    // the minimum hold stops the cooldown yanking the spell out
                    // of their hand mid-cast.
                    Loadout::ArmGrace(a_follower->GetFormID());
                    Loadout::StartCooldown(a_follower->GetFormID());

                    spdlog::info("[drive] {:08X} {} -- CheckCast={} reason={} state {} -> {}",
                                 a_follower->GetFormID(),
                                 spell->GetName() ? spell->GetName() : "?",
                                 ok ? "OK" : "REFUSED",
                                 static_cast<std::uint32_t>(reason),
                                 static_cast<std::uint32_t>(st),
                                 static_cast<std::uint32_t>(hand->state.get()));
                    return { Result::Fired, "caster driven" };
                }
            }

            auto* caster = a_follower->GetMagicCaster(src);
            if (!caster) {
                caster = a_follower->GetMagicCaster(CS::kInstant);
                if (!caster) return { Result::FailedOther, "no magic caster", true };
            }
            caster->CastSpellImmediate(spell, false, a_target, 1.0f, false, 0.0f, a_follower);

            // CHARGE THEM. Measured: package casts and CastSpellImmediate both
            // spend NOTHING (ENGINE_NOTES §0.22), so without this §5.3's
            // competence gate is decorative -- the pool never falls, so
            // "insufficient magicka" can only ever trigger on a pool drained by
            // vanilla AI.
            //
            // The cost is CalculateMagickaCost(a_follower), the ACTOR overload,
            // which accounts for their skill level and perks: a Destruction-
            // perked follower pays less for the same spell, and a gate that
            // ignored that would be measuring the wrong thing entirely
            // (marth, 2026-07-22).
            //
            // INVARIANTS #16 forbids hand-writing state a flow PRODUCES. This
            // flow produces no deduction at all, so this fills a gap rather
            // than duplicating one -- the same call DAC makes.
            if (avo) {
                // On ActorValueOwner, not Actor -- and `avo` is already in hand.
                avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                       RE::ActorValue::kMagicka, -cost);
            }

            // Restart the AI's window. Without this the grace is a ONE-SHOT:
            // the clock was armed at the first equip and never re-armed, so
            // after MFO's first cast every later one fired instantly and the
            // follower's own AI was never given another opening. The field log
            // showed it as rule 0 firing every 1.6s -- the suppression window,
            // not the 3s grace.
            if (equipped) {
                Loadout::ArmGrace(a_follower->GetFormID());
                Loadout::StartCooldown(a_follower->GetFormID());
            }

            return { Result::Fired, equipped ? "silent (their AI declined)" : "" };
        }

        // ── Tier-A EQUIP actions (§4.5) ──────────────────────────────────────
        // Equip the best weapon of a category from the follower's OWN inventory.
        // IDEMPOTENT: a no-op when already holding that category, so a persistently
        // winning rule does not re-equip every tick. Direct ActorEquipManager,
        // the same path LootEquipment uses; runs in combat, so a weapon it equips
        // can coexist with a left-hand spell but is NOT tracked by Loadout's hand
        // ledger -- acceptable because equip and cast are alternative gambits
        // (first-match-wins fires only one per tick).
        Outcome EquipWeapon(RE::Actor* a_follower, bool a_ranged) {
            // BOTH HANDS decide "already holding" (#75). The old guard read only
            // the RIGHT hand -- but a caster keeps a SPELL there, so the melee
            // weapon her off-hand still held was invisible to it and a
            // persistently-winning equip rule re-equipped the SAME weapon every
            // service tick (deck, Serana: 'Ebony Tanto' at gambit cadence,
            // visible weapon thrash while her own AI fought back for the
            // spell). The rule's goal is a weapon CATEGORY in hand -- EITHER
            // hand satisfies it.
            // marth: a base MAGE's melee sidearm is a DAGGER -- the same base-class
            // rule the loot path uses (#65 combatClassOverride==3/Cast, gated by
            // bMageDaggersOnly). For such a follower a MELEE equip order means a
            // dagger specifically: a looted sword/mace he still owns must NOT count
            // as "already holding the category" (else the satisfied NoOp below keeps
            // him on it forever) and must NOT win the draw. baseClass read on the
            // worker's g_followers-serial path (same as Scheduler / Actuation:25).
            std::uint8_t baseClass = 0;
            if (auto it = g_followers.find(a_follower->GetFormID()); it != g_followers.end())
                baseClass = it->second.combatClassOverride;
            const bool daggerMelee = !a_ranged && baseClass == 3 && Config::g_mageDaggersOnly.load();
            const auto holdsCategory = [a_ranged, daggerMelee](RE::TESForm* a_held) {
                auto* w = a_held ? a_held->As<RE::TESObjectWEAP>() : nullptr;
                if (!w || w->IsStaff()) return false;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) return false;
                return !daggerMelee || w->GetWeaponType() == RE::WEAPON_TYPE::kOneHandDagger;
            };
            // TRANSPARENT (satisfied): the rule's goal already holds, so
            // the scan falls past it -- AND the scheduler reads this
            // exact shape (transparent NoOp on an equip action) as the
            // H2 hand-claim: a lower CONTRADICTORY equip is skipped
            // without firing, so fall-through cannot manufacture the
            // melee<->ranged thrash of GAMBIT_FLOWS D4.
            if (holdsCategory(a_follower->GetEquippedObject(false)) ||
                holdsCategory(a_follower->GetEquippedObject(true)))
                return { Result::NoOp, "already holding that category", true };
            RE::TESObjectWEAP* best = nullptr; std::uint16_t bestDmg = 0;
            for (auto& [obj, data] : a_follower->GetInventory()) {
                if (!obj || data.first <= 0) continue;
                auto* w = obj->As<RE::TESObjectWEAP>();
                if (!w || w->IsStaff()) continue;
                // NON-PLAYABLE (record-header flag bit 2) is creature/automaton
                // gear -- fires but is INVISIBLE on a humanoid (same check as
                // Logistics' IsCreatureWeapon). Loot never stocks these, but a
                // player-given stray must not win the combat equip either.
                if ((w->GetFormFlags() & (1u << 2)) != 0) continue;
                if ((w->IsBow() || w->IsCrossbow()) != a_ranged) continue;
                // base MAGE melee = daggers only (see holdsCategory above).
                if (daggerMelee && w->GetWeaponType() != RE::WEAPON_TYPE::kOneHandDagger) continue;
                if (w->GetAttackDamage() >= bestDmg) { bestDmg = w->GetAttackDamage(); best = w; }
            }
            if (!best) return { Result::FailedSkill, a_ranged ? "no ranged weapon carried"
                                                              : "no melee weapon carried",
                                true };   // transparent -- cannot act, rules below run (§2)
            if (auto* mgr = RE::ActorEquipManager::GetSingleton()) {
                if (Config::g_weaponStyleControl.load()) {
                    // #76 FORCE-HOLD. forceEquip=true (7th arg, after extraData,
                    // count, slot, queueEquip) sets the engine's prevent-removal
                    // lock so the follower's OWN combat AI cannot auto-unequip the
                    // weapon to re-arm a spell -- the both-hands caster thrash
                    // v1.0.62 narrowed but did not close. The both-hands "already
                    // holding that category" NoOp above then keeps the rule
                    // satisfied with no re-equip until the gambit's condition goes
                    // false, when the scheduler releases it (ReconcileForcedWeapon).
                    const auto id = a_follower->GetFormID();
                    // A DIFFERENT weapon may still be locked from before -- a
                    // category flip (melee<->ranged), OR a base-mage daggers-only
                    // SAME-category swap (a looted sword was force-locked, then
                    // holdsCategory rejected it and the loop picked a dagger; the
                    // satisfied NoOp no longer catches this melee->melee case since
                    // c97035f). Force-unequip the old lock first, then lock the new.
                    // Read/write the map UNDER the lock; do the engine calls
                    // OUTSIDE it (SEV-1 discipline).
                    RE::TESBoundObject* oldForced = nullptr;
                    {
                        std::scoped_lock lk(g_forcedMx);
                        if (auto old = g_forcedWeapon.find(id);
                            old != g_forcedWeapon.end() && old->second && old->second != best)
                            oldForced = old->second;
                    }
                    if (oldForced)
                        mgr->UnequipObject(a_follower, oldForced, nullptr, 1, nullptr, true, true);
                    mgr->EquipObject(a_follower, best, nullptr, 1, nullptr, true, true);
                    {
                        std::scoped_lock lk(g_forcedMx);
                        g_forcedWeapon[id] = best;
                    }
                } else {
                    mgr->EquipObject(a_follower, best);   // kill-switch off: today's behaviour exactly
                }
            }
            // FLAIR #4: visibly COMMIT to the new tool -- steel out, squared up
            // -- rather than leaving the swap in whatever draw state the last
            // weapon had. The exact call Loadout.cpp:240 already ships for the
            // cast path; drawing while already drawn is a no-op, and this path
            // only runs on a real equip Fired (which buys a suppression window),
            // so it cannot repeat-fire.
            a_follower->DrawWeaponMagicHands(true);
            spdlog::info("[equip] {:08X}: GAMBIT equip {} '{}' dmg={}", a_follower->GetFormID(),
                         a_ranged ? "ranged" : "melee",
                         best->GetFullName() ? best->GetFullName() : "?", bestDmg);
            return { Result::Fired, a_ranged ? "equipped ranged" : "equipped melee" };
        }

        // (EquipTorch moved to Logistics -- torch is upkeep, not a combat action, #35.)

    }

    // ── FORCED SELF-CAST: the UNIVERSAL direct trigger (SPEC-self-cast-forced) ──
    // The MFO_CastPackageSelf alias route EQUIPS the spell but never TRIGGERS the
    // cast, and is declined outright on package-locked custom followers (Lucien).
    // So self-cast bypasses packages and drives the ACTOR directly -- follower-
    // agnostic. It is a proper CHANNEL, not a one-shot, tracked per follower:
    //
    //   FIRE (CastSelfDirect, paced by the caller's cooldown):
    //     * first fire  -> equip the spell (Loadout stows the weapon) and
    //       HoldStow so Tick can't auto-restore it ~500 ms in (the amputation
    //       marth saw). Register the channel.
    //     * every fire  -> apply the effect + spend magicka (§5.3), refreshing
    //       the VFX (dispel-then-apply so the shader never stacks across fires).
    //   RECONCILE (each tick, SelfCastReconcile):
    //     * DRIVE ONCE, the moment the async equip lands (currentSpell == spell)
    //       -- entering the caster's cast state is what makes the animation play
    //       IN FULL and stops the AI unequipping mid-cast. Driving on the FIRST
    //       fire read currentSpell before the equip settled -> the field's
    //       0->0. No per-tick re-equip -> no thrash / erratic animation.
    //     * RELEASE when the rule stops re-firing (or a safety cap): stop the
    //       VFX (Dispel), InterruptCast the channel, DeselectSpell, sheathe, and
    //       give the weapon back. This is the exact-bounding + no-stuck-VFX
    //       teardown, and it also fires on rule-disabled / condition-false
    //       (the rule simply stops firing -> the entry goes stale).
    namespace {
        using SelfClock = std::chrono::steady_clock;
        struct SelfCastState {
            RE::FormID            spell = 0;
            bool                  driven = false;   // caster driven into the cast state yet?
            SelfClock::time_point started{};
            SelfClock::time_point lastFired{};   // last time the rule re-fired (release clock)
            SelfClock::time_point lastApply{};   // last effect/magicka application (apply pacing)
        };
        std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // worker-serial

        // Stop a spell's lingering effect VFX -- the concentration hit-shader
        // that never terminates when the spell is applied one-shot (deck
        // 2026-08-17: the healing glow ran on after the pose ended). Main thread.
        void DispelSpellEffectsOn(RE::Actor* a_actor, RE::FormID a_spellID) {
            auto* mt = a_actor ? a_actor->AsMagicTarget() : nullptr;
            if (!mt) return;
            auto* list = mt->GetActiveEffectList();
            if (!list) return;
            for (auto* ae : *list)
                if (ae && ae->spell && ae->spell->GetFormID() == a_spellID)
                    ae->Dispel(true);
        }

        // Apply the effect + spend magicka for ONE fire (main thread). §5.3:
        // CastSpellImmediate spends nothing (§0.22), so deduct the real cost.
        void ApplySelfEffect(RE::FormID a_id, RE::FormID a_spellID) {
            MainThread::Post([a_id, a_spellID] {
                auto* a  = RE::TESForm::LookupByID<RE::Actor>(a_id);
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!a || !sp) return;
                // ALREADY-ACTIVE GUARD -- byte-identical to the TARGET one-shot
                // cast's own guard (Logistics OOC cast, "skip re-casting a buff
                // still on the TARGET"): do NOT re-apply a spell whose effect is
                // already active on the caster. A duration self-buff/LIGHT
                // (Candlelight) otherwise spawns a fresh light every re-cast and
                // they accumulate to a ShadowSceneNode null-call CTD (deck
                // 2026-08-18, "Active Lights 57"). Self and foe now behave
                // identically; an instant self-heal leaves no active effect, so it
                // still re-fires when the condition recurs; a ward re-fires after
                // it drops.
                auto* ei   = sp->GetCostliestEffectItem();
                auto* mgef = ei ? ei->baseEffect : nullptr;
                if (auto* mt = a->AsMagicTarget(); mgef && mt && mt->HasMagicEffect(mgef)) {
                    spdlog::info("[cast] {:08X} cast_self skipped -- {} ({:08X}) already active",
                                 a_id, sp->GetName() ? sp->GetName() : "?", a_spellID);
                    return;
                }
                auto*       avo    = a->AsActorValueOwner();
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (auto* inst = a->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant))
                    inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);
                const float cost = sp->CalculateMagickaCost(a);
                if (avo && cost > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -cost);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} SELF-CAST {} ({:08X}) -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_id, a->GetName() ? a->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, before, after, cost);
            });
        }

        // End a channel on the actor (main thread): stop the VFX, cut the
        // channel, take the spell out of the hand, sheathe.
        void SelfCastEndActor(RE::FormID a_id, RE::FormID a_spellID) {
            MainThread::Post([a_id, a_spellID] {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id);
                if (!a) return;
                DispelSpellEffectsOn(a, a_spellID);
                using CS = RE::MagicSystem::CastingSource;
                for (const auto s : { CS::kLeftHand, CS::kRightHand, CS::kInstant })
                    if (auto* mc = a->GetMagicCaster(s)) mc->InterruptCast(false);
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                    sp && a->GetEquippedObject(true) == sp)
                    a->DeselectSpell(sp);
                a->DrawWeaponMagicHands(false);   // sheathe
            });
        }
    }

    bool CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        // AE-only, mirroring CastOn (the SE crash path #67). Off AE -> transparent.
        if (!REL::Module::IsAE())    return false;
        if (!a_follower || !a_spell) return false;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();

        // §5.3 COMPETENCE: real cost gates the cast; the reserve floor keeps a
        // self-heal from emptying the pool. Unaffordable -> transparent decline.
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return false;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return false;
            }
        }

        const auto now = SelfClock::now();
        auto it = g_selfCast.find(id);

        // A DIFFERENT spell was channeling -> end it (stop its VFX) before this
        // one, so shaders never stack.
        if (it != g_selfCast.end() && it->second.spell != spellID) {
            SelfCastEndActor(id, it->second.spell);
            g_selfCast.erase(it);
            it = g_selfCast.end();
        }

        if (it == g_selfCast.end()) {
            // FIRST fire: equip (Loadout stows the weapon + records the debt) and
            // HOLD the restore. The DRIVE waits for the reconcile -- the equip is
            // async, so currentSpell is not the spell yet on this pass.
            std::string why;
            if (Loadout::Prepare(a_follower, a_spell, why) == Loadout::Ready::Failed)
                return false;
            Loadout::HoldStow(id);
            auto& sc = g_selfCast[id];
            sc.spell = spellID; sc.driven = false; sc.started = now;
            sc.lastFired = now; sc.lastApply = {};   // epoch -> apply the effect immediately
            it = g_selfCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the effect application -- this channel does NOT use
        // Loadout::StartCooldown (that calls ReleaseSpell, which would rip the
        // spell out of the hand and kill the animation). Callers refresh this
        // every service/combat tick while the rule wins; we apply the effect +
        // spend magicka only once per fCastCooldown, so a self-heal ticks at the
        // configured cadence rather than every 133 ms.
        const float interval = std::max(1.0f, Config::g_castCooldown.load());
        if (std::chrono::duration<float>(now - it->second.lastApply).count() >= interval) {
            it->second.lastApply = now;
            ApplySelfEffect(id, spellID);
        }
        return true;
    }

    void SelfCastReconcile() {
        if (g_selfCast.empty()) return;
        const auto  now = SelfClock::now();
        // Release when the rule stops re-firing. The callers refresh lastFired
        // every service/combat tick while the rule wins, so this only needs to
        // out-wait the round-robin gap (one follower serviced per ~133 ms tick),
        // not the cast cooldown. 2 s covers a large party and still releases
        // promptly when the rule goes false / is disabled.
        const float releaseSec = 2.0f;
        std::vector<RE::FormID> done;
        for (auto& [id, sc] : g_selfCast) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            const bool gone   = !a || !a->Is3DLoaded();
            const bool stale  = std::chrono::duration<float>(now - sc.lastFired).count() > releaseSec;
            const bool capped = std::chrono::duration<float>(now - sc.started).count()   > 30.0f;
            if (gone || stale || capped) {
                if (a) SelfCastEndActor(id, sc.spell);   // stop VFX + unequip + sheathe
                Loadout::EndStowHold(id);
                Loadout::Restore(id);                    // give the weapon back
                done.push_back(id);
                continue;
            }
            // DRIVE ONCE, the moment the async equip lands. Entering the cast
            // state plays the animation in full AND stops the AI unequipping
            // mid-cast -- no per-tick re-equip, no thrash. currentSpell is read
            // off the worker (the bDriveCaster precedent); the drive itself is
            // marshalled to the main thread where the animation graph ticks.
            if (!sc.driven && a) {
                using CS = RE::MagicSystem::CastingSource;
                auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sc.spell);
                const CS src = (sp && a->GetEquippedObject(false) == sp &&
                                a->GetEquippedObject(true) != sp) ? CS::kRightHand : CS::kLeftHand;
                auto* hand = sp ? a->GetMagicCaster(src) : nullptr;
                if (hand && hand->currentSpell == sp) {
                    sc.driven = true;
                    const auto spellID = sc.spell;
                    MainThread::Post([id, spellID, src] {
                        auto* ac = RE::TESForm::LookupByID<RE::Actor>(id);
                        auto* s2 = RE::TESForm::LookupByID<RE::SpellItem>(spellID);
                        if (!ac || !s2) return;
                        ac->DrawWeaponMagicHands(true);
                        if (auto* h = ac->GetMagicCaster(src)) {
                            h->desiredTarget = ac->CreateRefHandle();   // self
                            float strength = 1.0f;
                            RE::MagicSystem::CannotCastReason reason{};
                            h->CheckCast(s2, false, &strength, &reason, false);
                            h->RequestCastImpl();
                            spdlog::info("[cast] {:08X} self-cast DRIVEN -- caster state {} "
                                         "(CheckCast reason {})", id,
                                         static_cast<std::uint32_t>(h->state.get()),
                                         static_cast<std::uint32_t>(reason));
                        }
                    });
                }
            }
        }
        for (const auto id : done) g_selfCast.erase(id);
    }

    void ClearSelfCasts() {
        for (auto& [id, sc] : g_selfCast) Loadout::EndStowHold(id);
        g_selfCast.clear();
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

            // Ask the HOOK, not the config. They disagree whenever install was
            // refused with the flag on -- VR, today. Reporting Fired there would
            // buy a suppression window for a latch nothing reads.
            if (!Targeting::IsHooked()) {
                return { Result::FailedOther, "targeting hook not installed", true };
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
        if (op == Vocab::kActCastSelf) {
            return CastOn(a_follower, a_choice.actionParam, a_follower);
        }
        if (op == Vocab::kActCastPlayer) {
            return CastOn(a_follower, a_choice.actionParam, RE::PlayerCharacter::GetSingleton());
        }
        if (op == Vocab::kActCastTarget) {
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
            // the retreat driver. #35.
            if (Packages::RetreatFill(a_follower))
                return { Result::Fired, "flee -> retreat to player" };
            return { Result::FailedOther, "retreat alias unavailable", true };
        }
        if (op == Vocab::kActPowerAttack) {
            // EXPERIMENTAL (#35): there is no engine verb for "power-attack X".
            // Aim at the foe via the latch (like Attack), then fire the standing
            // power-attack animation event directly. May no-op if the follower is
            // not in a melee-attack state -- FIELD-VERIFY before trusting it.
            auto ptr = a_choice.target.get();
            auto* foe = ptr.get();
            if (!foe) return { Result::FailedOther, "chosen foe no longer resolves", true };
            if (!Targeting::IsHooked())
                return { Result::FailedOther, "targeting hook not installed", true };
            // Only meaningful with a MELEE weapon drawn -- a power attack from a bow
            // or empty hands is nonsense and would just burn the tick (Fable).
            auto* r = a_follower->GetEquippedObject(false);
            auto* w = r ? r->As<RE::TESObjectWEAP>() : nullptr;
            const auto wt = w ? w->GetWeaponType() : RE::WEAPON_TYPE::kHandToHandMelee;
            const bool melee = w && wt != RE::WEAPON_TYPE::kBow && wt != RE::WEAPON_TYPE::kCrossbow &&
                                    wt != RE::WEAPON_TYPE::kStaff;
            // TRANSPARENT: "foe blocking -> power attack" while holding a bow
            // must not starve the attack rule below for as long as the foe
            // blocks (GAMBIT_FLOWS §3.6).
            if (!melee) return { Result::FailedSkill, "no melee weapon drawn for a power attack", true };
            // Try the anim FIRST and commit the target latch ONLY if it accepted.
            // Latch-then-reject (Opus review) was a two-mutation tick: Command wrote
            // the latch, then the transparent reject fell through and let a LOWER rule
            // also fire -- and a FAILED power attack still committed a target. Ordering
            // the graph check ahead means a rejected power attack mutates NOTHING and
            // falls through cleanly (§4.3, GAMBIT_FLOWS §3.6). `sent` still isn't proof
            // the swing landed; field-verify. Report Fired only on accept so a reject
            // buys no suppression window (Fable: Fired-on-reject went visibly passive).
            if (!a_follower->NotifyAnimationGraph("attackPowerStartInPlace"))
                return { Result::FailedOther, "power-attack anim rejected", true };
            Targeting::Command(a_follower->GetFormID(), a_choice.target);
            return { Result::Fired, "power attack (experimental)" };
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

    // ── #76: EQUIP FORCE-HOLD lifecycle ──────────────────────────────────────
    void ReleaseForcedWeapon(RE::Actor* a_follower) {
        if (!a_follower) return;
        const auto id = a_follower->GetFormID();
        RE::TESBoundObject* obj = nullptr;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(id);
            if (it == g_forcedWeapon.end()) return;
            obj = it->second;
            g_forcedWeapon.erase(it);
        }
        // forceEquip=true on the UNequip clears the prevent-removal lock the
        // force-equip set; a plain unequip would be REFUSED against a forced
        // item and the follower would stay stuck holding the weapon, unable to
        // cast -- the worse-than-oscillation failure this whole feature must not
        // create. Engine call OUTSIDE the lock.
        if (auto* mgr = RE::ActorEquipManager::GetSingleton(); mgr && obj)
            mgr->UnequipObject(a_follower, obj, nullptr, 1, nullptr, true, true);
        spdlog::info("[equip] {:08X}: force-hold released", id);
    }

    void ReconcileForcedWeapon(RE::Actor* a_follower, int a_wantStance, bool a_condKnownFalse) {
        if (!a_follower) return;
        // KEEP the hold while the feature is ON and an equip gambit of the forced
        // weapon's OWN category held THIS tick (a_wantStance, set by a fired-or-
        // satisfied equip). RELEASE only when we actually KNOW the condition is
        // false: a_condKnownFalse (the scan ran to completion — Evaluate returns
        // only MATCHING rules, so a scan STOPPED by a higher rule leaves the
        // equip's truth UNKNOWN; releasing then force-unequips the weapon mid-
        // fight the tick a heal/attack preempts — the SEV-2 churn). A category
        // mismatch under condKnownFalse also releases (a stale/flipped record).
        // Feature OFF -> release immediately, regardless of the scan. Decide
        // UNDER the lock, Release OUTSIDE it (Release re-locks -> no self-deadlock).
        bool release = false;
        {
            std::scoped_lock lk(g_forcedMx);
            auto it = g_forcedWeapon.find(a_follower->GetFormID());
            if (it == g_forcedWeapon.end()) return;   // nothing forced -> nothing to do
            if (!Config::g_weaponStyleControl.load()) {
                release = true;                        // kill-switch: always release
            } else if (a_condKnownFalse) {
                auto* w  = it->second ? it->second->As<RE::TESObjectWEAP>() : nullptr;
                const int cat = (w && (w->IsBow() || w->IsCrossbow())) ? 2 : 1;
                release = (a_wantStance != cat);       // condition confirmed false / flipped
            }
            // else: scan stopped early -> UNKNOWN -> keep the hold this tick.
        }
        if (release) ReleaseForcedWeapon(a_follower);
    }

    void ClearForcedWeapons() {
        std::scoped_lock lk(g_forcedMx);
        g_forcedWeapon.clear();
    }

    // #76 force-hold co-save. Persist the force-equip locks so a load clears the
    // stale ones the .ess carried (the engine's forceEquip serializes; the map
    // does not). INDEPENDENT record — its own version guard + ResolveFormID/DROP.
    constexpr std::uint32_t kMaxForcedWeapons = 64;   // party is tiny; a generous cap

    void CoSaveForcedWeapons(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecForcedWeapon, kForcedWeaponVersion)) {
            spdlog::error("[cosave] OpenRecord('FWPN') failed -- force-holds NOT saved");
            return;
        }
        // SEV-1: SNAPSHOT under the lock, then write from the copy — a lock must
        // never be held across a WriteRecordData (the MSTK/CopyStockGear rule).
        std::vector<std::pair<RE::FormID, RE::FormID>> snap;   // {followerID, weaponID | 0}
        {
            std::scoped_lock lk(g_forcedMx);
            for (const auto& [id, w] : g_forcedWeapon) {
                if (!w || !Followers::IsPersistableID(id)) continue;   // #9: never write a 0xFF follower
                const RE::FormID wid = w->GetFormID();
                // #9 for the WEAPON too: a player-enchanted/created weapon has a
                // 0xFF id that ResolveFormID passes through unresolved -> the load
                // would unequip the WRONG object and the real lock would survive.
                // Persist weapon=0; CoLoad then sweeps BOTH hands to clear it.
                snap.emplace_back(id, Followers::IsPersistableID(wid) ? wid : 0u);
            }
        }
        a_intfc->WriteRecordData(static_cast<std::uint32_t>(snap.size()));
        for (const auto& [id, wid] : snap) {
            a_intfc->WriteRecordData(id);
            a_intfc->WriteRecordData(wid);
        }
        spdlog::info("[cosave] saved {} force-hold(s), schema v{}", snap.size(), kForcedWeaponVersion);
    }

    void CoLoadForcedWeapons(SKSE::SerializationInterface* a_intfc, std::uint32_t /*a_version*/) {
        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) return;
        if (count > kMaxForcedWeapons) {
            spdlog::error("[cosave] implausible force-hold count {} -- ABORTING FWPN load", count);
            return;
        }
        std::uint32_t released = 0, dropped = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID rawFollower = 0, rawWeapon = 0;
            if (!a_intfc->ReadRecordData(rawFollower)) return;
            if (!a_intfc->ReadRecordData(rawWeapon))   return;
            RE::FormID follower = 0, weapon = 0;
            if (!a_intfc->ResolveFormID(rawFollower, follower)) { ++dropped; continue; }   // #8 DROP
            // weapon==0 means "was a created/enchanted (0xFF) weapon at save" —
            // resolve only a real id; anything unresolvable falls to the both-
            // hands sweep so a stale lock still clears.
            if (rawWeapon != 0 && !a_intfc->ResolveFormID(rawWeapon, weapon)) weapon = 0;
            // The lock lives in the actor's inventory extra-data, not the map, so
            // clear it on the follower. Defer to the main thread (the load callback
            // is off it and the actor may not be 3D-loaded yet; the force-unequip
            // is an inventory op). NO map repopulation / erase — the map is empty
            // post-revert, so touching it here would be the only cross-thread
            // access (SEV-1). A session starts hold-free; the equip gambit
            // re-forces next combat if its condition still holds.
            MainThread::Post([follower, weapon]() {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(follower);
                if (!actor) return;
                auto* mgr = RE::ActorEquipManager::GetSingleton();
                if (!mgr) return;
                if (weapon != 0) {
                    if (auto* obj = RE::TESForm::LookupByID<RE::TESBoundObject>(weapon))
                        mgr->UnequipObject(actor, obj, nullptr, 1, nullptr, true, true);
                } else {
                    // Unknown weapon (created/enchanted): force-unequip whatever
                    // bound object is in EITHER hand to clear the lock regardless.
                    for (bool leftHand : { false, true }) {
                        if (auto* held = actor->GetEquippedObject(leftHand))
                            if (auto* obj = held->As<RE::TESBoundObject>())
                                mgr->UnequipObject(actor, obj, nullptr, 1, nullptr, true, true);
                    }
                }
                spdlog::info("[equip] {:08X}: stale force-hold cleared on load", follower);
            });
            ++released;
        }
        spdlog::info("[cosave] loaded {} force-hold(s) to clear on load ({} dropped unresolvable)",
                     released, dropped);
    }

}
