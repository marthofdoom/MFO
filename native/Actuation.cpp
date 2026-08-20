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
#include "Confidence.h"  // AUTO cast: ChaseRadius bounds the "nearby enemies" fan-out

#include <optional>      // ForceCast's tri-state return -- not in the PCH
#include <random>        // fix #3/#6: jittered beneficial-recast window (worker-serial RNG)

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

        // ResolveCastTarget moved OUT of this anon namespace to public
        // MFO::Actuation scope (just below, after this block closes) so the
        // logistics OOC cast_target path (Logistics.cpp) can share the exact
        // same resolution ladder as the combat Fire path. It still calls
        // NearestAlly (anon, above) -- an internal-linkage helper is visible
        // throughout this TU.

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

        // ── CONCENTRATION: the BOUNDED DIRECT-FORCE STREAM ───────────────────
        // A concentration spell has no "one cast" for the fire-and-forget
        // machinery to observe: force-YESing it to the AI made a PERMANENT
        // held stream (Lucien's Flames through Xelzaz -- the v1.0.53 freeze),
        // and skipping it was rejected -- exact bounding must cover EVERY
        // spell class, no AI escape hatch (marth). So MFO streams it ITSELF.
        // DELIVERY IS THE DIRECT FORCE (CastSelfDirect / CastTargetDirect =
        // CastSpellImmediate straight onto the target + hand magicka deduct),
        // NEVER the AI package: the package route (v1.0.58-65, Packages::
        // CastAt + CastHold) §4.6-DECLINED every tick for a package-locked
        // custom follower (Lucien 2F00591F -- his own quest owns the package
        // alias at prio 80, MFO claims at 60), which with the consent denies
        // below meant a TOTAL cast lockout. The bound is stated UP FRONT and
        // enforced by Self/TargetCastReconcile each tick:
        //   hostile  -- 1-4 s by temperament (each mage's breath is
        //               consistently their own), LoS + line-of-fire re-checked
        //               on EVERY apply (the ffWatch analog);
        //   heal     -- until the target tops off, capped;
        //   utility  -- a capped window, ward dispelled on release.
        // Bounding is RELEASE + RE-STREAM while the rule keeps winning, never
        // a stop. The AI-first grace is deliberately NOT offered here: the
        // CheckStartCast/CheckCast hooks deny the AI's own attempt at a wanted
        // concentration spell (an AI channel cannot be bounded), so MFO's
        // direct stream is the ONLY open channel -- and it always delivers,
        // because a direct apply passes through neither hook (see
        // ConcentrationCast below). Every exit is bounded: an applied beat, a
        // transparent pace-out, or a legible failure (§5.3) -- never "leave it
        // to their AI".
        constexpr float kConcHealCap     = 6.0f;   // heal stream hard cap
        constexpr float kConcUtilityHold = 4.0f;   // non-self utility/ward window
        // Self ward/utility HARD cap (marth). Deliberately NOT the 4s package
        // utility hold: a PACKAGE roots the caster, so its hold must be short to
        // un-root him often; CastSelfDirect does NOT root (it is a paced direct-
        // apply) and re-streams the very next winning tick, so a short cap only
        // makes a self ward dispel/re-apply FLICKER -- a real defensive gap. 15s
        // bounds a stuck self ward while keeping that re-stream flicker negligible.
        constexpr float kConcSelfUtilityCap = 15.0f;

        // THE CADENCE CONTRACT (critical -- "heals feel broken" regression).
        // A concentration spell's cost is authored PER SECOND, and the ENGINE
        // channels its magnitude continuously through the sustained real effect
        // (SustainConcentrationEffect). The ~1 s beat is the stream's
        // heartbeat: each beat DEDUCTS one second's cost (CalculateMagickaCost
        // on a concentration spell returns the per-second cost, so the 1 s
        // beat is the authored drain) and RE-ARMS the sustained effect's
        // rolling window. Pacing the beat by fCastCooldown (default 4 s) would
        // under-charge the channel 4x and let the effect lapse between re-arms
        // (the original "heals feel broken" shape). Fire-and-forget spells
        // keep the fCastCooldown beat: their magnitude is per CAST, and a 1 s
        // beat would multiply it. Sticky concentration wards beat at 1 s too,
        // but the already-active guard in Apply{Self,Target}Effect keeps a
        // still-up ward from re-stacking.
        constexpr float kConcApplyPeriod = 1.0f;

        // ONE source of truth for the concentration TIME-LIMITS, so EVERY
        // concentration cast -- self, player, ally, foe -- is bounded by the
        // identical "same time limit based casting" durations (marth):
        //   hostile  -- 1-4 s by per-follower Temperament, ffWatch cuts it the
        //               moment a teammate crosses the line of fire;
        //   heal     -- kConcHealCap (6 s) "until the target tops off";
        //   utility  -- a capped kConcUtilityHold (4 s) hold.
        // Consumed by TargetCastReconcile (the non-self direct-force stream's
        // caps) AND by SelfCastReconcile's self-heal cap, so the two paths can
        // never drift apart on the numbers. (Returns a Packages::CastHold purely
        // as a numbers carrier -- no package is dispatched with it any more.)
        Packages::CastHold ConcentrationHold(RE::FormID a_casterID, RE::FormID a_targetID,
                                             CasterConsent::SpellKind a_kind) {
            Packages::CastHold hold;
            if (a_kind == CasterConsent::SpellKind::Offense) {
                hold.holdSeconds = 1.0f + 3.0f * Temperament(a_casterID);   // 1-4 s, flair #1
                hold.ffWatch     = true;
            } else if (a_kind == CasterConsent::SpellKind::Heal) {
                hold.holdSeconds = kConcHealCap;
                hold.healWatch   = a_targetID;
            } else {
                hold.holdSeconds = kConcUtilityHold;
            }
            return hold;
        }

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
                // F3 tri-state (mirrors Logistics.cpp:3987-3996 + CastOn's self
                // fork): only a real Applied is this tick's action; a Refreshed
                // (channel paced, nothing applied) is TRANSPARENT so it does not
                // starve the rules below it, and the fired log never lies.
                if (Config::g_castSelf.load()) {
                    switch (CastSelfDirect(a_follower, a_spell)) {
                    case SelfCast::Applied:
                        return { Result::Fired, "self-cast (direct trigger)" };
                    case SelfCast::Refreshed:
                        return { Result::NoOp, "self-cast refresh (paced)", true };
                    case SelfCast::Declined:
                    default:
                        break;
                    }
                }
                return { Result::FailedOther, "self-cast could not fire", true };
            }

            // ── DIRECT FORCE is THE delivery, combat included (marth 2026-08-18:
            // "always use the known working force; avoid the package route") ──
            // v1.0.58-65 delivered combat concentration through the AI PACKAGE
            // (Packages::CastAt + CastHold). For a package-locked custom
            // follower (Lucien 2F00591F: his own quest owns the package alias
            // at prio 80, MFO claims at 60) the engine §4.6-DECLINED that claim
            // EVERY tick -- and because the consent latch denies the AI's own
            // attempt at a wanted concentration spell (an AI channel cannot be
            // bounded), the dead package was "the ONLY open channel": a TOTAL
            // combat cast lockout. CastTargetDirect (CastSpellImmediate straight
            // onto the target + hand magicka deduct) beats BOTH halves of that
            // trap:
            //   * no package -> no §4.6 alias arbitration to lose;
            //   * no AI deliberation -> the CheckStartCast (0x06, combat-AI
            //     advisory) and CheckCast (0x0A, pre-charge) consent hooks
            //     never see it. Both sit on the AI's own casting pipeline
            //     (RequestCastImpl -> ... -> FinishCastImpl, whose precondition
            //     is CheckCast -- ENGINE_NOTES §0.13/§0.14); CastSpellImmediate
            //     skips that state machine entirely (the same reason it never
            //     animates). Field proof, deck 2026-08-18: Lucien's AI cast of
            //     Healing (0005AD5C) is "[consent] HARD-ABORTED" while MFO's
            //     own direct SELF-CAST applies land on the same actor, same
            //     minute, hooks live.
            // So the consent hooks may keep DENYING the follower's own AI
            // stream while MFO's direct stream delivers -- coherent, never a
            // lockout. The package delivery is REMOVED here, not demoted: two
            // live delivery paths split the cadence/magicka semantics (the
            // package stream spends no magicka and ROOTS the caster mid-fight,
            // and its ~per-hold beat cannot meet the kConcApplyPeriod heal
            // contract), and the animation it bought is deferred anyway. The
            // old bForceCastOnMiss+bUsePackages gate went with it -- the direct
            // force needs neither (OOC already ships without them), so combat
            // and OOC concentration now run the IDENTICAL delivery. No
            // Loadout::CoolingDown gate / StartCooldown either: the channel
            // self-paces (kConcApplyPeriod / fCastCooldown by kind), and a 4 s
            // cooldown hole would let TargetCastReconcile's ~2 s stale window
            // tear down a still-winning stream. LoS + line-of-fire for hostile
            // offense are re-checked on EVERY apply inside CastTargetDirect
            // (the ffWatch analog); bounding lives in TargetCastReconcile
            // (hostile 1-4 s Temperament, heal 6 s, utility 4 s + ward dispel)
            // -- release + re-stream while the rule wins, never a stop.
            switch (CastTargetDirect(a_follower, a_spell, a_target)) {
            case SelfCast::Applied:
                // Exclusive control while the rule governs: keep the consent
                // latch armed so the slider denies COMPETING AI spells and the
                // concentration deny keeps the AI's own unbounded attempt at
                // this spell off. The direct stream is unaffected (above).
                CasterConsent::Want(id, a_spell->GetFormID());
                return { Result::Fired, "concentration (direct force)" };
            case SelfCast::Refreshed:
                // Live stream, paced out this tick -- the wait IS the action;
                // transparent like the FF form.
                return { Result::NoOp, "concentration direct refresh (paced)", true };
            case SelfCast::Declined:
            default:
                // Unaffordable (§5.3) / LoS or line-of-fire held (offense) /
                // off-AE: transparent + legible, the rules below run.
                return { Result::FailedOther, "concentration direct-force declined", true };
            }
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
            // refreshes the channel + applies the effect on the channel's own
            // beat -- ~1 s kConcApplyPeriod for concentration, fCastCooldown
            // for FF); the reconcile releases it when the rule goes false.
            if (a_target == a_follower && Config::g_castSelf.load()) {
                // F3 tri-state (mirrors Logistics.cpp:3987-3996): only a real
                // Applied is THIS tick's action. A Refreshed tick (the channel is
                // winning but paced out -- nothing applied) is TRANSPARENT so a
                // persistently-true combat cast_self does not starve attack/drink/
                // heal below it, and `lastFired`/`[eval] fired` never lies on a
                // no-op tick.
                switch (CastSelfDirect(a_follower, spell)) {
                case SelfCast::Applied:
                    return { Result::Fired, "self-cast channel (direct trigger)" };
                case SelfCast::Refreshed:
                    // Channel kept alive, no effect/magicka this tick -- fall past.
                    return { Result::NoOp, "self-cast refresh (paced)", true };
                case SelfCast::Declined:
                default:
                    // Unaffordable / off-AE / no caster: transparent, the rules
                    // below run (the follower is not stuck on a cast that can't go).
                    return { Result::FailedOther, "self-cast could not fire", true };
                }
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

            // THREADING (#14): CastOn runs on the AddTask job worker
            // (Scheduler::Tick -> Fire), and this silent cast used to call
            // CastSpellImmediate INLINE here -- the same off-main pattern the
            // queued 1.5.x act.cast_target AV reports point at. Post the engine
            // apply + deduct to the MAIN thread, re-resolving by FormID inside
            // (never carry Actor* across threads). Fired is returned
            // optimistically, the same contract as ApplySelfEffect/
            // ApplyTargetEffect. `src` is captured by value (an enum).
            //
            // CHARGE THEM (inside the post). Measured: package casts and
            // CastSpellImmediate both spend NOTHING (ENGINE_NOTES §0.22), so
            // without this §5.3's competence gate is decorative. The cost is
            // CalculateMagickaCost(a_follower), the ACTOR overload (skill +
            // perks -- marth, 2026-07-22). INVARIANTS #16 forbids hand-writing
            // state a flow PRODUCES; this flow produces no deduction at all,
            // so this fills a gap rather than duplicating one -- the same call
            // DAC makes. Clamped to the live pool (#6) so a deduct can never
            // drive magicka negative.
            {
                auto doCast = [fid = a_follower->GetFormID(),
                               tid = a_target ? a_target->GetFormID() : 0,
                               spid = spell->GetFormID(), src] {
                    auto* f  = RE::TESForm::LookupByID<RE::Actor>(fid);
                    auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(spid);
                    // A null target stays null (the original inline call passed
                    // a_target through as-is; CastSpellImmediate accepts null).
                    auto* t  = tid ? RE::TESForm::LookupByID<RE::Actor>(tid) : nullptr;
                    if (!f || !sp || (tid && !t)) return;
                    auto* caster = f->GetMagicCaster(src);
                    if (!caster) caster = f->GetMagicCaster(CS::kInstant);
                    if (!caster) return;   // F4: no caster -> no cast, no deduct
                    auto* mavo = f->AsActorValueOwner();
                    const float pool = mavo ? mavo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                    caster->CastSpellImmediate(sp, false, t, 1.0f, false, 0.0f, f);
                    const float c     = sp->CalculateMagickaCost(f);
                    const float spend = mavo ? std::min(c, pool) : 0.0f;
                    if (mavo && spend > 0.0f)
                        mavo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                                RE::ActorValue::kMagicka, -spend);
                };
                // VR has no pump (Post is a documented no-op): fall back inline.
                if (MainThread::IsInstalled()) MainThread::Post(doCast);
                else                           doCast();
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

    // ── FORCED SELF-CAST: the UNIVERSAL direct trigger (SPEC-self-cast-forced) ──
    // The MFO_CastPackageSelf alias route EQUIPS the spell but never TRIGGERS the
    // cast, and is declined outright on package-locked custom followers (Lucien).
    // So self-cast bypasses packages and applies the effect DIRECTLY -- follower-
    // agnostic, effect + magicka only, NO equip and NO channel:
    //
    //   * NEVER EQUIP THE SPELL. CastSpellImmediate (kInstant) applies the effect
    //     without the spell in hand. Leaving a light spell (Candlelight/Magelight)
    //     equipped let the follower's OWN AI spam-cast it -- 55+ non-MFO lights
    //     piled up to a ShadowSceneNode light-limit CTD (deck 2026-08-19). The
    //     cast animation is DEFERRED (polish pass), so the equip/HoldStow/caster-
    //     drive scaffolding is gone entirely.
    //   * FIRE (CastSelfDirect, every service/combat tick the rule wins): register
    //     the entry, refresh lastFired, and -- once per beat (kConcApplyPeriod ~1 s
    //     for a CONCENTRATION spell, per-second authored magnitude/cost; once per
    //     fCastCooldown for FF) -- apply the
    //     effect + spend magicka (§5.3) via ApplySelfEffect. The ALREADY-ACTIVE
    //     guard there (the foe cast's own HasMagicEffect check) blocks re-applying
    //     a duration self-buff/light while it is still up, so exactly ONE light
    //     per effect-duration cycle. An instant heal re-fires when HP drops again.
    //   * RELEASE (SelfCastReconcile, each tick): when the rule stops re-firing
    //     (goes stale, or the follower unloads) DISPEL a lingering ward/effect so
    //     it cannot persist as a stuck gameplay buff. NO time cap -- a long buff
    //     lives its authored duration while the rule wins. Covers rule-disabled /
    //     condition-false (the entry simply goes stale). Nothing to unequip.
    namespace {
        using SelfClock = std::chrono::steady_clock;
        struct SelfCastState {
            RE::FormID            spell = 0;
            SelfClock::time_point started{};
            SelfClock::time_point lastFired{};   // last time the rule re-fired (release clock)
            SelfClock::time_point lastApply{};   // last effect/magicka application (apply pacing)
        };
        std::unordered_map<RE::FormID, SelfCastState> g_selfCast;   // worker-serial

        // ── ON-TARGET DIRECT FORCE (package-lock-proof; g_selfCast generalized) ──
        // The SAME known-working force (CastSpellImmediate straight onto the actor +
        // magicka deduct) applied to a NON-self target -- player / ally / foe. It
        // touches NO package, so it beats a package-locked custom follower's §4.6
        // alias lock: Lucien (2F00591F) has a prio-80 quest owning the cast alias,
        // so the package route [pkg]-DECLINED every tick and his on-PLAYER heal never
        // landed (deck, build 5f8e873). Direct force lands it. One stream per follower
        // (single channel, like the package). Worker-serial, no lock (#4 discipline);
        // cleared with g_selfCast on revert. `kind` (cached at start) decides the
        // time cap AND when release DISPELS: a ward/buff (Buff) on any release; a
        // momentary heal/damage's SUSTAINED real effect only at end-of-stream
        // (stale/gone/switch -- it genuinely channels and must die with the
        // stream), never on a cap-only re-stream (the re-arm continues it).
        struct TargetCastState {
            RE::FormID               spell  = 0;
            RE::FormID               target = 0;
            CasterConsent::SpellKind kind   = CasterConsent::SpellKind::Buff;
            SelfClock::time_point    started{};
            SelfClock::time_point    lastFired{};
            SelfClock::time_point    lastApply{};
        };
        std::unordered_map<RE::FormID, TargetCastState> g_targetCast;

        // Stop a spell's lingering effect VFX -- the concentration hit-shader
        // that never terminates when the spell is applied one-shot (deck
        // 2026-08-17: the healing glow ran on after the pose ended). Main thread.
        void DispelSpellEffectsOn(RE::Actor* a_actor, RE::FormID a_spellID) {
            auto* mt = a_actor ? a_actor->AsMagicTarget() : nullptr;
            if (!mt) return;
            auto* list = mt->GetActiveEffectList();
            if (!list) return;
            // Collect FIRST, then dispel -- Dispel can mutate the active-effect
            // list, so calling it mid-iteration is unsafe (F5 hardening).
            std::vector<RE::ActiveEffect*> hits;
            for (auto* ae : *list)
                if (ae && ae->spell && ae->spell->GetFormID() == a_spellID)
                    hits.push_back(ae);
            for (auto* ae : hits) ae->Dispel(true);
        }

        // ── THE REAL EFFECT, WITH A SYNTHESIZED DURATION (marth's ruling) ────
        // "Shouldn't you be using the ACTUAL spell effect? It seems like you
        // are trying to recreate it instead." -- correct, and it supersedes two
        // earlier attempts: the per-beat CastSpellImmediate re-cast (dc856ea:
        // HUD churn of duration-0 momentaries, no sustained shader) AND the
        // ApplyConcentrationBeat RestoreActorValue recreation (REMOVED: it only
        // covered value-modifier AVs and could never generalize -- a forced
        // waterbreathing/invisibility/ward concentration is not an AV write).
        //
        // THE PREMISE (field, b63beb9 A/B): a one-shot CastSpellImmediate of a
        // concentration spell creates its REAL ActiveEffect but with duration
        // 0 and no sustaining channel, so it dies within a frame -- a
        // per-second Restore Health accumulates ~0, the shader never sustains,
        // and re-casting per beat just stacks short-lived effects. FF spells
        // are untouched by all of this: a duration-0 FF instant applies its
        // per-CAST magnitude in full through the plain call (field-proven).
        //
        // THE FIX: attach the spell's REAL effect(s) ONCE per stream (plain
        // CastSpellImmediate -- correct effect, shader, HUD entry, resists,
        // hostility, every archetype), then SUSTAIN that single ActiveEffect
        // by pinning a real `duration` (the stream's window) and re-arming
        // `elapsedSeconds` on every beat. Given a real duration the engine
        // runs the effect NORMALLY: a per-second value-modifier accumulates
        // its authored magnitude the ordinary way (a duration'd Restore Health
        // heals magnitude-per-second, exactly like a regen potion), a duration
        // archetype (waterbreathing, invisibility, muffle) simply LASTS, a
        // ward wards. No recreation, no manual math, ALL archetypes. The
        // writes are instance-local on the live AE (the same `duration`/
        // `elapsedSeconds` fields the DoT-recast logic already reads) -- NEVER
        // a shared-form (MGEF/SpellItem) mutation.
        //
        // Returns TRUE if a live effect for this spell was found + re-armed
        // (the caller must NOT re-cast); FALSE -> the caller attaches once via
        // CastSpellImmediate and calls this again to pin it. EVIDENCE
        // COLLECTOR: the caller logs "conc effect ATTACHED" on every attach --
        // if the engine honors the pinned duration that line appears ONCE per
        // stream and the HUD shows one continuous effect; if the engine
        // expires the effect regardless (e.g. a no-duration-flagged MGEF, the
        // open premise CI cannot test), the line repeats every beat and the
        // presentation degrades to the previous per-beat re-attach -- loud in
        // the log, and the fix would be an FF-variant runtime spell, NOT a
        // return to RestoreActorValue. MAIN THREAD only (live AE list).
        bool SustainConcentrationEffect(RE::Actor* a_target, RE::SpellItem* a_spell,
                                        float a_window) {
            auto* mt = a_target ? a_target->AsMagicTarget() : nullptr;
            if (!mt || !a_spell) return false;
            auto* list = mt->GetActiveEffectList();
            if (!list) return false;
            bool found = false;
            for (auto* ae : *list) {
                if (!ae || ae->spell != a_spell) continue;
                ae->duration       = a_window;   // real duration -> the engine channels it
                ae->elapsedSeconds = 0.0f;       // rolling re-arm, one beat at a time
                found = true;
            }
            return found;
        }

        // ── CONCENTRATION + SELF-DELIVERY PROXY (the ONLY delivery fix on top of the
        // baseline). A fire-and-forget Self spell force-cast at another actor lands on
        // that actor (baseline, field-proven -- Candlelight/flesh work). A
        // CONCENTRATION Self spell does NOT: CastSpellImmediate sets up a channeled
        // cast whose target is resolved by the spell's DELIVERY, and kSelf binds the
        // sustained AE to the caster's OWNER (the follower), so a player/ally
        // concentration heal collapses onto the follower and silently fails. FIX:
        // cast a transient COPY of the source with its casting style PRESERVED and ONLY
        // data.delivery flipped kSelf -> kTargetActor, so the channel resolves to the
        // passed target. The follower is still the caster (his rate + magicka); the
        // player never casts / pays. Used ONLY for concentration+Self+off-self -- FF /
        // self-cast / non-Self are untouched baseline.
        //
        // TWO transient dynamic (0xFF__) slots, FILL-CAST-REUSE-FREELY (marth): once
        // the AE is applied it captures its own effect/magnitude/caster and no longer
        // needs the SPEL form, so the slots carry NO lifetime bookkeeping -- reuse a
        // slot for the same source (so a stream's ~1 s beats re-arm the SAME AE via
        // SustainConcentrationEffect, no stacking), and evict round-robin when a THIRD
        // source needs one. Never serialized (dynamic forms are not). MAIN THREAD only
        // (form table); returns nullptr off-main (VR) so no form is created there.
        namespace ConcProxy {
            RE::SpellItem* g_form[2] = { nullptr, nullptr };
            RE::FormID     g_src[2]  = { 0, 0 };
            int            g_next    = 0;   // round-robin victim when both slots are taken

            void Configure(RE::SpellItem* a_p, RE::SpellItem* a_src) {
                a_p->data          = a_src->data;                                 // castingType/cost/etc.
                a_p->data.delivery = RE::MagicSystem::Delivery::kTargetActor;     // the ONLY change
                a_p->effects.clear();
                for (auto* e : a_src->effects) a_p->effects.push_back(e);         // shared effect ptrs
            }
            RE::SpellItem* Get(RE::SpellItem* a_src) {
                if (!a_src || !MainThread::IsInstalled()) return nullptr;         // no off-main create (VR)
                const auto sid = a_src->GetFormID();
                for (int i = 0; i < 2; ++i)
                    if (g_form[i] && g_src[i] == sid) return g_form[i];           // reuse same-source
                int slot = -1;
                for (int i = 0; i < 2; ++i) if (!g_form[i]) { slot = i; break; } // prefer an empty slot
                if (slot < 0) { slot = g_next; g_next ^= 1; }                    // else evict round-robin
                if (!g_form[slot]) {
                    auto* f = RE::IFormFactory::GetConcreteFormFactoryByType<RE::SpellItem>();
                    g_form[slot] = f ? static_cast<RE::SpellItem*>(f->Create()) : nullptr;
                    if (!g_form[slot]) return nullptr;
                }
                Configure(g_form[slot], a_src);
                g_src[slot] = sid;
                return g_form[slot];
            }
            // The proxy FormID currently backing a_srcID (or 0) -- so a stream's dispel
            // can also clear the proxy-keyed AE off the target.
            RE::FormID FormFor(RE::FormID a_srcID) {
                for (int i = 0; i < 2; ++i)
                    if (g_form[i] && g_src[i] == a_srcID) return g_form[i]->GetFormID();
                return 0;
            }
            // Revert/load reset (ClearSelfCasts, kPreLoadGame BEFORE any post-load
            // cast + BEFORE the old game's forms are torn down). The dynamic 0xFF
            // proxy forms do NOT survive a load, but the SOURCE spell FormIDs in
            // g_src[] are static -- so without this, ConcProxy::Get would match a
            // stale g_src and return a FREED g_form pointer next session (UAF). Null
            // the slots so the next cast re-mints. AND drop the borrowed source
            // Effect* the proxy holds (Configure copied a_src->effects BY POINTER):
            // clearing effects here, while the form is still alive, means the engine
            // frees an EMPTY array at teardown -- it can never double-free an Effect*
            // still owned by the live source spell. Main thread (StopPump drained).
            void Reset() {
                for (int i = 0; i < 2; ++i) {
                    if (g_form[i]) g_form[i]->effects.clear();   // drop borrowed source Effect*
                    g_form[i] = nullptr;
                    g_src[i]  = 0;
                }
                g_next = 0;
            }
        }

        // The spell to CAST for a follower delivering a_src at a_tgt: a concentration+
        // Self spell aimed off-self returns its delivery-flipped proxy (or a_src if the
        // proxy is unavailable -- VR / factory fail, a safe no-crash fallback); every
        // other spell (FF, self-cast, non-Self) returns a_src unchanged. MAIN THREAD.
        RE::SpellItem* DeliverySpell(RE::SpellItem* a_src, RE::Actor* a_follower, RE::Actor* a_tgt) {
            if (a_src && a_follower && a_tgt && a_tgt != a_follower &&
                a_src->GetDelivery()    == RE::MagicSystem::Delivery::kSelf &&
                a_src->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
                if (auto* p = ConcProxy::Get(a_src)) return p;
            return a_src;
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
                // A MOMENTARY concentration effect (value-modifier: heal/drain)
                // BYPASSES the guard: its SUSTAINED real effect (the synthesized-
                // duration channel) is by design ACTIVE on every subsequent beat,
                // and the guard must not block the beat that re-arms it -- block
                // it and the channel expires at the window instead of rolling.
                // The guard's CTD case (lights/duration buffs) is not value-
                // modifier and stays guarded.
                const bool concMomentary =
                    sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration &&
                    mgef &&
                    (mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kValueModifier ||
                     mgef->data.archetype == RE::EffectArchetypes::ArchetypeID::kDualValueModifier);
                if (auto* mt = a->AsMagicTarget();
                    !concMomentary && mgef && mt && mt->HasMagicEffect(mgef)) {
                    spdlog::info("[cast] {:08X} cast_self skipped -- {} ({:08X}) already active",
                                 a_id, sp->GetName() ? sp->GetName() : "?", a_spellID);
                    return;
                }
                auto* avo  = a->AsActorValueOwner();
                auto* inst = a->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // THE REAL EFFECT with a synthesized duration (marth's
                    // ruling -- see SustainConcentrationEffect): attach once,
                    // then re-arm the same ActiveEffect each beat; the ENGINE
                    // channels the authored magnitude itself. Window = the self
                    // stream's cap (heal 6 s / ward-utility 15 s) so the single
                    // entry bridges even a magicka-starved caster's sparse beats.
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcSelfUtilityCap;
                    if (!SustainConcentrationEffect(a, sp, window)) {
                        inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);   // attach ONCE
                        SustainConcentrationEffect(a, sp, window);                      // then pin it
                        // Evidence line: ONCE per stream if the engine honors the
                        // pinned duration; repeating every beat = sustain refused.
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on self "
                                     "(spell {:08X}, window {:.0f}s)", a_id, a_spellID, window);
                    }
                } else {
                    inst->CastSpellImmediate(sp, false, a, 1.0f, false, 0.0f, a);
                }
                const float cost  = sp->CalculateMagickaCost(a);
                // #6: clamp to the current pool so a deduct never drives magicka
                // negative (AUTO/self validate cost against ONE worker snapshot).
                const float spend = avo ? std::min(cost, before) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} SELF-CAST {} ({:08X}) -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_id, a->GetName() ? a->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, before, after, cost);
            });
        }

        // RELEASE a self-cast (main thread): dispel the applied ward/effect so it
        // does not persist as a stuck gameplay buff after the rule goes false.
        // The self-cast NEVER equips or channels, so there is nothing to unequip,
        // interrupt, or sheathe -- and doing any of those would touch the
        // follower's OWN combat draw/equip state, which is not ours to change.
        void SelfCastEndActor(RE::FormID a_id, RE::FormID a_spellID) {
            MainThread::Post([a_id, a_spellID] {
                if (auto* a = RE::TESForm::LookupByID<RE::Actor>(a_id))
                    DispelSpellEffectsOn(a, a_spellID);
            });
        }

        // ApplySelfEffect generalized to a NON-self target (main thread). SAME
        // direct call the public build's CastOn force-half used -- the known-
        // working force, no package. Deducts the CASTER's real magicka (§5.3).
        // MAGNITUDE: an FF spell's effect applies in full through the plain call;
        // a CONCENTRATION spell's effect is attached ONCE and SUSTAINED with a
        // synthesized duration (SustainConcentrationEffect -- marth's real-effect
        // ruling) so the ENGINE channels the authored magnitude itself; a bare
        // one-shot applies ~0 (b63beb9 field A/B, the revoked "plain call heals
        // fine" ruling).
        //   a_guard TRUE  (sticky ward/buff): skip if the buff is already active on
        //                 the target, so a duration buff is not re-stacked.
        //   a_guard FALSE (heal / damage -- MOMENTARY): every paced beat deducts a
        //                 second's cost and re-arms the sustained effect, so the
        //                 target is topped up steadily while the rule wins.
        void ApplyTargetEffect(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_guard) {
            MainThread::Post([a_casterID, a_targetID, a_spellID, a_guard] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
                auto* tgt    = RE::TESForm::LookupByID<RE::Actor>(a_targetID);
                auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!caster || !tgt || !sp) return;
                if (a_guard) {
                    auto* ei   = sp->GetCostliestEffectItem();
                    auto* mgef = ei ? ei->baseEffect : nullptr;
                    if (auto* mt = tgt->AsMagicTarget(); mgef && mt && mt->HasMagicEffect(mgef)) {
                        spdlog::info("[cast] {:08X} force-cast skipped -- {} ({:08X}) already "
                                     "active on {:08X}", a_casterID,
                                     sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID);
                        return;
                    }
                }
                auto* avo  = caster->AsActorValueOwner();
                auto* inst = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // FORCED CONCENTRATION = THE REAL EFFECT with a synthesized
                    // duration (marth's ruling -- see SustainConcentrationEffect):
                    // attach ONCE, then re-arm the one ActiveEffect each beat; the
                    // ENGINE channels the authored magnitude itself (resists,
                    // hostility, every archetype). Window by kind: heal 6 s
                    // bridges sparse beats; offense/utility 4 s covers their caps.
                    // A CONCENTRATION + SELF spell aimed off-self collapses onto the
                    // FOLLOWER (delivery binds the channel to the caster's owner), so
                    // cast the delivery-flipped PROXY (kTargetActor) instead -- the
                    // channel then resolves to tgt. Sustain keys on the SAME spell we
                    // cast, so a proxy stream re-arms its own AE cleanly.
                    RE::SpellItem* castSp = DeliverySpell(sp, caster, tgt);
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcUtilityHold;
                    if (!SustainConcentrationEffect(tgt, castSp, window)) {
                        // The KNOWN-WORKING FORCE -- caster casts (proxy of) sp AT tgt.
                        inst->CastSpellImmediate(castSp, false, tgt, 1.0f, false, 0.0f, caster);
                        SustainConcentrationEffect(tgt, castSp, window);
                        // Evidence line: ONCE per stream if the engine honors the
                        // pinned duration; repeating every beat = sustain refused.
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on {:08X} "
                                     "(spell {:08X}{}, window {:.0f}s)",
                                     a_casterID, a_targetID, a_spellID,
                                     castSp != sp ? " self->target proxy" : "", window);
                    }
                } else {
                    // The KNOWN-WORKING FORCE -- caster casts sp AT tgt, package-free.
                    // (Baseline: an FF Self spell force-cast here lands on tgt.)
                    inst->CastSpellImmediate(sp, false, tgt, 1.0f, false, 0.0f, caster);
                }
                const float cost  = sp->CalculateMagickaCost(caster);
                const float spend = avo ? std::min(cost, before) : 0.0f;   // #6: never negative
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} FORCE-CAST {} ({:08X}) at {:08X} -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_casterID, caster->GetName() ? caster->GetName() : "?",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                             before, after, cost);
            });
        }

        // RELEASE a target stream (main thread): dispel our lingering ward/buff --
        // and the SUSTAINED real effect a momentary stream leaves -- off the
        // TARGET. Since the real-effect sustain, this dispel is LOAD-BEARING for
        // momentary kinds: the sustained effect genuinely channels (heals/damages)
        // until its pinned window elapses, so the stream's end must cut it rather
        // than let it run unpaid. Callers gate WHEN (Buff: any release; momentary:
        // end-of-stream / switch only, never a cap-only re-stream -- the re-stream
        // re-arms the same effect seamlessly). Mirrors SelfCastEndActor.
        void TargetCastEndActor(RE::FormID a_targetID, RE::FormID a_spellID) {
            MainThread::Post([a_targetID, a_spellID] {
                if (auto* t = RE::TESForm::LookupByID<RE::Actor>(a_targetID)) {
                    DispelSpellEffectsOn(t, a_spellID);
                    // A concentration+Self stream channels through a delivery-flipped
                    // PROXY, so the live AE carries the PROXY's spellID -- dispel it
                    // too so a heal/ward cannot linger past the stream.
                    if (auto proxyID = ConcProxy::FormFor(a_spellID))
                        DispelSpellEffectsOn(t, proxyID);
                }
            });
        }

        // AUTO fan-out pacing: last broadcast per follower, so a rule that keeps
        // winning every ~133 ms tick re-fans only once per fCastCooldown instead
        // of every tick (no thrash, no magicka spike). Worker-serial, no lock
        // (the g_followers discipline #4); cleared with g_selfCast on revert.
        std::unordered_map<RE::FormID, SelfClock::time_point> g_autoCast;

        // fix #3/#6: BENEFICIAL DURATION recast suppression. Keyed on
        // (casterFormID<<32 | spellID): the last time this caster fired this
        // beneficial buff, plus the JITTERED window it must wait before re-firing.
        // A light applied hands-free (Magelight/Candlelight) never registers as
        // "already active", so the already-active gate never caught it and MFO
        // respammed it as fast as magicka regenerated (deck 2026-08-18). This map
        // is the second layer: even an undetectable light is held off until near
        // its own authored expiry, and the per-fire jitter keeps the recast beat
        // human (never a fixed cadence). Worker-serial, no lock (same #4 discipline
        // as g_autoCast/g_selfCast); cleared with them on revert. INSTANT
        // beneficial spells (authoredDuration 0) and concentration streams are
        // never entered here -- they must re-fire on demand.
        struct BeneficialRecast {
            SelfClock::time_point lastCast{};
            float                 windowSec = 0.0f;
        };
        std::unordered_map<std::uint64_t, BeneficialRecast> g_beneficialRecast;

        std::uint64_t RecastKey(RE::FormID a_caster, RE::FormID a_spell) {
            return (static_cast<std::uint64_t>(a_caster) << 32) | a_spell;
        }

        // The spell's own authored duration = the longest effectItem.duration over
        // its effects (0 = an instant spell -> exempt from recast suppression).
        float AuthoredDuration(RE::SpellItem* a_spell) {
            float d = 0.0f;
            if (!a_spell) return d;
            for (auto* eff : a_spell->effects) {
                if (!eff) continue;
                const float dur = static_cast<float>(eff->effectItem.duration);
                if (dur > d) d = dur;
            }
            return d;
        }

        // A jittered fraction of the authored duration, recomputed per fire so the
        // recast beat is never a robotic constant (marth: "needs a larger more
        // variable delay to look more human"). Worker-serial context -> a
        // function-local static RNG is safe (single-threaded access). The window is
        //   authoredDuration * fBeneficialRecastFrac * (1 +/- fBeneficialRecastJitter).
        float JitteredRecastWindow(float a_authoredDuration) {
            static std::mt19937 rng{ std::random_device{}() };
            const float frac   = Config::g_beneficialRecastFrac.load();
            const float jitter = std::clamp(Config::g_beneficialRecastJitter.load(), 0.0f, 0.95f);
            std::uniform_real_distribution<float> dist(-jitter, jitter);
            return a_authoredDuration * frac * (1.0f + dist(rng));
        }

        // Apply a_spell's effect FROM a caster TO an arbitrary target, hands-free
        // (CastSpellImmediate, kInstant), and deduct the caster's REAL magicka
        // cost (§56: CastSpellImmediate spends nothing, so we deduct). This is
        // ApplySelfEffect generalised to a non-self target so AUTO can heal an
        // ally / damage a foe with the SAME proven mechanism -- effect + magicka
        // only, NO equip and NO channel (animation deferred, ENGINE_NOTES §0.13).
        // Runs on the main thread. The already-active / DoT-recast decision is
        // made WORKER-SIDE by ShouldApplyTo before the post (F2/F3/F4) -- it owns
        // the "is this target worth a cast?" gate for BOTH allies and enemies, so
        // this must NOT re-apply the old blanket already-active skip here: that
        // would defeat the hostile DoT burst-vs-tail recast the worker approved.
        // a_hostile only tunes the log label. (Sole caller: CastAuto.)
        void ApplyEffectFromTo(RE::FormID a_casterID, RE::FormID a_targetID,
                               RE::FormID a_spellID, bool a_hostile) {
            MainThread::Post([a_casterID, a_targetID, a_spellID, a_hostile] {
                auto* caster = RE::TESForm::LookupByID<RE::Actor>(a_casterID);
                auto* target = RE::TESForm::LookupByID<RE::Actor>(a_targetID);
                auto* sp     = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
                if (!caster || !target || !sp) return;
                auto* avo  = caster->AsActorValueOwner();
                auto* inst = caster->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
                if (!inst) return;   // F4: no caster -> no cast, so do NOT deduct magicka
                const float before = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                    // FORCED CONCENTRATION under AUTO: same real-effect contract as
                    // the manual streams -- ONE sustained REAL effect per fanned
                    // target (the 6s/4s window bridges the fCastCooldown re-fan, so
                    // the HUD shows a continuous entry, not a pulse stack); the
                    // ENGINE channels the authored magnitude itself.
                    // A CONCENTRATION + SELF spell fanned to an ally collapses onto
                    // the FOLLOWER -- cast the delivery-flipped PROXY so it lands on
                    // the ally (same one-mechanism fix as the manual streams).
                    RE::SpellItem* castSp = DeliverySpell(sp, caster, target);
                    const float window =
                        CasterConsent::ClassifySpell(sp) == CasterConsent::SpellKind::Heal
                            ? kConcHealCap : kConcUtilityHold;
                    if (!SustainConcentrationEffect(target, castSp, window)) {
                        inst->CastSpellImmediate(castSp, false, target, 1.0f, false, 0.0f, caster);
                        SustainConcentrationEffect(target, castSp, window);
                        spdlog::info("[cast] {:08X} conc effect ATTACHED on {:08X} "
                                     "(AUTO, spell {:08X}{}, window {:.0f}s)",
                                     a_casterID, a_targetID, a_spellID,
                                     castSp != sp ? " self->target proxy" : "", window);
                    }
                } else {
                    inst->CastSpellImmediate(sp, false, target, 1.0f, false, 0.0f, caster);
                }
                const float cost  = sp->CalculateMagickaCost(caster);
                // #6: clamp to the current pool so a deduct never drives magicka
                // negative (AUTO validates N casts against ONE worker snapshot).
                const float spend = avo ? std::min(cost, before) : 0.0f;
                if (avo && spend > 0.0f)
                    avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                           RE::ActorValue::kMagicka, -spend);
                const float after = avo ? avo->GetActorValue(RE::ActorValue::kMagicka) : 0.0f;
                spdlog::info("[cast] {:08X} {} AUTO {} {} ({:08X}) -> {:08X} -- effect applied, "
                             "magicka {:.0f}->{:.0f} (cost {:.0f})",
                             a_casterID, caster->GetName() ? caster->GetName() : "?",
                             a_hostile ? "HOSTILE" : "BENEFICIAL",
                             sp->GetName() ? sp->GetName() : "?", a_spellID, a_targetID,
                             before, after, cost);
            });
        }

        // A detrimental Health effect = damage (an instant hit OR a per-second DoT).
        bool IsDamageEffect(RE::EffectSetting* a_mgef) {
            if (!a_mgef) return false;
            if (a_mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
            return a_mgef->IsDetrimental() || a_mgef->IsHostile();
        }

        // A BENEFICIAL Health effect = a heal (restore/fortify Health, instant or
        // over time) -- the mirror of IsDamageEffect: primaryAV Health but NOT
        // detrimental/hostile. Field #2: the per-target need gate must key off the
        // spell's EFFECTS, not solely CasterConsent::ClassifySpell -- a restore-
        // health spell the classifier does NOT tag kHeal was falling through to the
        // generic whole-party beneficial fan, so a full-HP player got healed merely
        // because a DIFFERENT ally was hurt. Any spell carrying a heal effect is now
        // gated by each target's own HP.
        bool IsHealEffect(RE::EffectSetting* a_mgef) {
            if (!a_mgef) return false;
            if (a_mgef->data.primaryAV != RE::ActorValue::kHealth) return false;
            return !a_mgef->IsDetrimental() && !a_mgef->IsHostile();
        }

        // Does a_spell restore Health to its target? (Any heal effect at all.)
        bool SpellHealsHealth(RE::SpellItem* a_spell) {
            if (!a_spell) return false;
            for (auto* eff : a_spell->effects)
                if (eff && IsHealEffect(eff->baseEffect)) return true;
            return false;
        }

        // WORKER-SIDE per-target gate for the AUTO fan-out (F2/F3/F4). Decides,
        // BEFORE any main-thread post, whether this cast should actually land on
        // the target -- so an all-covered fan-out returns a transparent NoOp
        // instead of burning the tick + suppression window on posts that every
        // one of them would skip. Mirrors ApplyEffectFromTo's own already-active
        // guard, applied to allies AND enemies (F3): an INSTANT spell leaves no
        // lingering effect, never matches HasMagicEffect, and so ALWAYS fires; a
        // duration buff/DoT/debuff already on the target is skipped.
        //
        // HOSTILE DURATION refinement (F4): do not blanket-skip an active DoT.
        // Decompose the spell -- burst = Sigma magnitude of INSTANT (duration==0)
        // damage effects, dotRate = Sigma magnitude of DURATION damage effects
        // (per second) -- read the target's remaining DoT time, and recast when
        //   burst >= dotRate * timeRemaining * fDotRecastBurstRatio.
        // Pure-burst always fires (dotRate==0 -> RHS 0), pure-DoT waits, a
        // big-burst+small-tail spell re-lands as the tail decays. Reads the
        // target's effect list on the worker, same discipline as the enumeration
        // that calls it (PickFoe/PickAlly context).
        bool ShouldApplyTo(RE::Actor* a_target, RE::SpellItem* a_spell, bool a_hostile) {
            if (!a_target || !a_spell) return false;
            // #5: CONCENTRATION spells (Healing Hands, damage streams) are meant to
            // re-apply continuously while the need holds -- the already-active skip
            // AND the burst-vs-tail DoT math both misfire on a stream, which is the
            // inconsistent healing/concentration behaviour marth reported. Never
            // let this gate block a concentration cast; the caller's own need gate
            // (the heal HP-threshold in CastAuto) still decides WHETHER it is wanted.
            if (a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
                return true;

            auto* mt   = a_target->AsMagicTarget();
            auto* ei   = a_spell->GetCostliestEffectItem();
            auto* mgef = ei ? ei->baseEffect : nullptr;
            // #6a: ROBUST already-active detection. HasMagicEffect(costliest
            // baseEffect) MISSES light-archetype spells (Magelight/Candlelight
            // applied via CastSpellImmediate never register that effect), so it
            // returned true every cooldown and MFO respammed the light as fast as
            // magicka regenerated (deck 2026-08-18). So ALSO scan the target's
            // active-effect list for an effect cast by THIS spell that still has
            // remaining duration -- a light that never registers via HasMagicEffect
            // is caught here and treated as active, same as HasMagicEffect true.
            bool active = (mgef && mt && mt->HasMagicEffect(mgef));
            if (!active && mt) {
                if (auto* list = mt->GetActiveEffectList()) {
                    for (auto* ae : *list) {
                        if (!ae || ae->spell != a_spell) continue;
                        if ((ae->duration - ae->elapsedSeconds) > 0.0f) { active = true; break; }
                    }
                }
            }
            // Not currently affected (covers EVERY instant spell) -> apply.
            if (!active) return true;
            // Already affected. Beneficial / allies: do not re-stack -> skip.
            // Only a hostile DURATION spell gets the burst-vs-tail recast test.
            if (!a_hostile) return false;

            float burst = 0.0f, dotRate = 0.0f;
            for (auto* eff : a_spell->effects) {
                if (!eff || !IsDamageEffect(eff->baseEffect)) continue;
                const float mag = eff->effectItem.magnitude;
                if (eff->effectItem.duration == 0) burst   += mag;   // instant hit
                else                               dotRate += mag;   // per-second DoT
            }
            // No DoT component -> nothing to wait out, always recast.
            if (dotRate <= 0.0f) return true;

            // DoT time still to be delivered = max (duration - elapsed) across the
            // spell's active effects on this target.
            float timeRemaining = 0.0f;
            if (auto* list = mt->GetActiveEffectList()) {
                for (auto* ae : *list) {
                    if (!ae || ae->spell != a_spell) continue;
                    const float rem = ae->duration - ae->elapsedSeconds;
                    if (rem > timeRemaining) timeRemaining = rem;
                }
            }
            const float ratio = Config::g_dotRecastBurstRatio.load();
            return burst >= dotRate * timeRemaining * ratio;
        }
    }

    SelfCast CastSelfDirect(RE::Actor* a_follower, RE::SpellItem* a_spell) {
        // AE-only, mirroring CastOn (the SE crash path #67). Off AE -> transparent.
        if (!REL::Module::IsAE())    return SelfCast::Declined;
        if (!a_follower || !a_spell) return SelfCast::Declined;
        const auto id      = a_follower->GetFormID();
        const auto spellID = a_spell->GetFormID();

        // §5.3 COMPETENCE: real cost gates the cast; the reserve floor keeps a
        // self-heal from emptying the pool. Unaffordable -> transparent decline.
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return SelfCast::Declined;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return SelfCast::Declined;
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
            // FIRST fire. DELIBERATELY DO NOT EQUIP THE SPELL. CastSpellImmediate
            // (kInstant, in ApplySelfEffect) applies the effect without the spell
            // in hand, so the follower is NEVER left holding it. Leaving a light
            // spell (Candlelight/Magelight) equipped let the follower's OWN AI
            // spam-cast it -- 55 non-MFO lights piled up to a ShadowSceneNode
            // light-limit CTD (deck 2026-08-19). The animation is deferred anyway,
            // so the equip/HoldStow/caster-drive scaffolding is dropped: MFO casts
            // the chosen self-spell ONCE, it applies, and the already-active guard
            // (ApplySelfEffect) blocks re-casting until the effect expires.
            auto& sc = g_selfCast[id];
            sc.spell = spellID; sc.started = now;
            sc.lastFired = now; sc.lastApply = {};   // epoch -> apply the effect immediately
            it = g_selfCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. Callers refresh this every service/combat tick
        // while the rule wins. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts one second's cost and re-arms that effect's rolling
        // window (fCastCooldown pacing would under-charge 4x and let the
        // channel lapse between re-arms -- "heals feel broken"). A
        // FIRE-AND-FORGET spell keeps the configured fCastCooldown beat (its
        // magnitude is per CAST). A duration buff is additionally capped to
        // once per effect-duration by the already-active guard in
        // ApplySelfEffect, so the 1 s beat can never stack a still-up ward.
        const float interval =
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                ? kConcApplyPeriod
                : std::max(1.0f, Config::g_castCooldown.load());
        if (std::chrono::duration<float>(now - it->second.lastApply).count() >= interval) {
            it->second.lastApply = now;
            ApplySelfEffect(id, spellID);
            return SelfCast::Applied;   // effect + magicka landed THIS tick
        }
        // Rule winning, channel kept alive, but paced out this tick -- a
        // refresh, NOT an action (F3: the caller must not let it suppress the
        // rules below it, or an "always -> cast_self" starves loot/drink).
        return SelfCast::Refreshed;
    }

    void SelfCastReconcile() {
        if (g_selfCast.empty()) return;
        const auto  now = SelfClock::now();
        // Release when the rule stops re-firing. The callers refresh lastFired
        // every service/combat tick while the rule wins, so this only needs to
        // out-wait the round-robin gap (one follower serviced per ~133 ms tick),
        // not the cast cooldown. 2 s covers a large party and still releases
        // promptly when the rule goes false / is disabled.
        //
        // DURATION FIX (field-confirmed): there is NO time cap here. An earlier
        // `capped = started > 30 s` force-released the channel -- and thus
        // DISPELLED the buff -- 30 s after the FIRST cast even while the rule was
        // still winning. That tore a 300 s Candlelight down at 30 s, so the
        // already-active guard saw it expire and re-cast on a rock-steady ~30 s
        // beat (deck 2026: 22:44:49 -> :45:19 -> :45:46 ...). CastSpellImmediate
        // already applies the spell's FULL authored duration/magnitude
        // (magnitudeOverride 0 = use the effect's own); the cap was the only
        // thing shortening it. Release now hinges solely on the rule going stale
        // (or the follower unloading), so a long buff lives its authored life and
        // re-casts at its real expiry.
        // #5 DISPEL-BEAT: a fixed 2 s window is too short in COMBAT. There the
        // rule only re-fires when the follower is BOTH serviced (round-robin,
        // ~133 ms x party) AND past his suppression window (fSuppressWindow x
        // temperament, up to ~1.12x). For party >= 3 that gap exceeds 2 s, so the
        // channel goes "stale", we DISPEL the live self-buff, and the next fire
        // re-applies it -- the exact re-cast beat the 300 s-cap fix removed, back
        // at window cadence. Size the release to out-wait the worst-case gap:
        //   fSuppressWindow*1.12 (max temperament) + 0.133*partySize + margin,
        // floored at the old 2 s so it never releases SLOWER-to-react than before
        // when the party is small / the window short.
        const float suppress   = std::max(0.0f, Config::g_suppressWindow.load());
        const float partySize  = static_cast<float>(Followers::g_active.size() + 1);  // + player
        const float releaseSec = std::max(2.0f,
                                          suppress * 1.12f + 0.133f * partySize + 0.5f);
        std::vector<RE::FormID> done;
        for (auto& [id, sc] : g_selfCast) {
            auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
            const bool gone   = !a || !a->Is3DLoaded();
            const bool stale  = std::chrono::duration<float>(now - sc.lastFired).count() > releaseSec;
            // SAME-TIME-LIMIT (marth) + HEAL-MUST-FLOW (coordinator): classify the
            // channel once, for BOTH the concentration cap AND the sticky-dispel gate.
            //   * The CAP applies to CONCENTRATION only (an FF buff lives its authored
            //     duration -- the no-time-cap fix): HEAL -> kConcHealCap (6s), WARD/
            //     UTILITY(Buff) -> kConcSelfUtilityCap (15s, marth -- not the 4s
            //     package hold, which would only FLICKER a non-rooting self ward).
            //   * The cap RELEASES + re-streams; whether release DISPELS is the
            //     sticky gate below, NOT the cap. A momentary HEAL/DAMAGE is capped
            //     but NEVER dispelled, so the entry just re-creates and re-applies
            //     next tick -- the heal FLOWS UNINTERRUPTED (this is why marth's
            //     "not healing himself either" cannot be this cap: dispel is skipped
            //     for Heal, and an instant restore-Health has no active effect to rip).
            CasterConsent::SpellKind kind = CasterConsent::SpellKind::Buff;   // default sticky
            bool concCapped = false;
            if (!gone) {
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(sc.spell)) {
                    kind = CasterConsent::ClassifySpell(sp);
                    if (sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                        const float cap = (kind == CasterConsent::SpellKind::Heal)
                                              ? kConcHealCap : kConcSelfUtilityCap;
                        if (std::chrono::duration<float>(now - sc.started).count() >= cap)
                            concCapped = true;
                    }
                }
            }
            if (gone || stale || concCapped) {
                // RELEASE. DISPEL a lingering STICKY buff (Buff = ward/fortify,
                // any release) so it cannot persist as a stuck gameplay effect --
                // AND a momentary stream's SUSTAINED real effect when the stream
                // truly ENDED (stale/rule-false): since the real-effect sustain
                // that effect genuinely channels, so the stream's end must cut
                // it rather than let it heal unpaid to its window's end. A
                // CAP-only release re-streams next tick and keeps the ONE
                // sustained HUD entry alive (no flicker at cap boundaries; the
                // re-arm continues the same effect). There is NO equip to undo
                // (the self-cast never holds the spell -- the AI-spam CTD).
                if (a && (kind == CasterConsent::SpellKind::Buff || stale))
                    SelfCastEndActor(id, sc.spell);
                done.push_back(id);
            }
        }
        for (const auto id : done) g_selfCast.erase(id);
    }

    void ClearSelfCasts() {
        // Revert/load: drop the channels. No engine call -- the world is being
        // replaced, and the self-cast holds no equip/debt to undo.
        g_selfCast.clear();
        g_targetCast.clear();         // on-target direct-force streams, likewise
        g_autoCast.clear();           // AUTO fan-out pacing is session-scoped too
        g_beneficialRecast.clear();   // fix #3/#6: per-buff recast windows likewise
        ConcProxy::Reset();           // null dangling 0xFF proxy forms + drop borrowed
                                      // source Effect* (cross-load UAF / double-free)
    }

    // ON-TARGET DIRECT FORCE = CastSelfDirect generalized to a NON-self target.
    // The known-working, package-lock-proof delivery for a concentration cast at a
    // player / ally / foe: no package -> no §4.6 alias-lock decline, so a package-
    // locked custom follower (Lucien) actually heals the player and damages the foe.
    // Registers a single per-follower stream, paces the apply at kConcApplyPeriod
    // (~1 s -- a concentration magnitude is authored per second; FF spells keep
    // fCastCooldown), and is time-bounded + released by TargetCastReconcile.
    // Returns Applied/Refreshed/Declined with the SAME semantics as CastSelfDirect
    // (a paced REFRESH is NOT an action). LoS + line-of-fire GATE hostile offense
    // (never direct-apply damage into a wall or through a teammate). Callers:
    // Logistics OOC cast dispatch AND combat's ConcentrationCast -- BOTH primary,
    // no package anywhere on the concentration delivery. Worker-serial state;
    // the engine apply itself is posted to the MAIN thread (ApplyTargetEffect).
    SelfCast CastTargetDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                              RE::Actor* a_target) {
        if (!REL::Module::IsAE())            return SelfCast::Declined;   // AE-only (#67)
        if (!a_follower || !a_spell || !a_target) return SelfCast::Declined;
        if (a_target == a_follower)          return SelfCast::Declined;   // self -> CastSelfDirect
        const auto id       = a_follower->GetFormID();
        const auto spellID  = a_spell->GetFormID();
        const auto targetID = a_target->GetFormID();
        const auto kind     = CasterConsent::ClassifySpell(a_spell);

        // §5.3 COMPETENCE: real cost gates the cast; the reserve floor keeps it from
        // emptying the pool. Unaffordable -> transparent decline (mirrors CastSelfDirect).
        if (auto* avo = a_follower->AsActorValueOwner()) {
            const float cost = a_spell->CalculateMagickaCost(a_follower);
            const float have = avo->GetActorValue(RE::ActorValue::kMagicka);
            if (cost > have) return SelfCast::Declined;
            const float reserve = Config::g_magickaReserve.load();
            if (reserve > 0.0f) {
                const float mx = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                    a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                      RE::ActorValue::kMagicka);
                if (mx > 0.0f && (have - cost) < reserve * mx) return SelfCast::Declined;
            }
        }

        // HOSTILE offense: LoS + line-of-fire gates on the direct path too (the
        // package's ffWatch has no analog here, so re-check every tick). Held ->
        // Declined (transparent): don't apply this tick, and the un-refreshed entry
        // goes stale so TargetCastReconcile CUTS the beam, exactly like ffWatch.
        if (kind == CasterConsent::SpellKind::Offense) {
            if (Sightline::Check(id, targetID) == Sightline::Verdict::Occluded)
                return SelfCast::Declined;
            if (Sightline::TeammateInFireLine(id, targetID))
                return SelfCast::Declined;
        }

        const auto now = SelfClock::now();
        auto it = g_targetCast.find(id);
        // Spell OR target switched -> end the old stream first, ALWAYS: a ward
        // must not linger, and the SUSTAINED real effect on the OLD target
        // genuinely channels until its window elapses -- it must die with its
        // stream, not keep healing/damaging the old target unpaid. Shaders and
        // buffs never stack or stray.
        if (it != g_targetCast.end() &&
            (it->second.spell != spellID || it->second.target != targetID)) {
            TargetCastEndActor(it->second.target, it->second.spell);
            g_targetCast.erase(it);
            it = g_targetCast.end();
        }
        if (it == g_targetCast.end()) {
            auto& tc = g_targetCast[id];
            tc.spell = spellID; tc.target = targetID; tc.kind = kind;
            tc.started = now; tc.lastFired = now; tc.lastApply = {};   // epoch -> apply now
            it = g_targetCast.find(id);
        } else {
            it->second.lastFired = now;   // rule still winning -> keep the channel open
        }

        // SELF-PACE the beat. CADENCE CONTRACT (kConcApplyPeriod): a
        // CONCENTRATION spell's cost is authored PER SECOND and the engine
        // channels its magnitude through the SUSTAINED real effect, so the ~1 s
        // beat deducts one second's cost and re-arms that effect's rolling
        // window (fCastCooldown pacing would under-charge 4x and let the
        // channel lapse between re-arms -- "heals feel broken"). An FF spell
        // (this function can be handed one) keeps the fCastCooldown beat; a
        // sticky concentration ward's 1 s beat is de-duplicated by the
        // already-active guard in ApplyTargetEffect.
        const float interval =
            a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration
                ? kConcApplyPeriod
                : std::max(1.0f, Config::g_castCooldown.load());
        if (std::chrono::duration<float>(now - it->second.lastApply).count() >= interval) {
            it->second.lastApply = now;
            // GUARD only sticky (Buff) buffs; heal/damage re-apply freely (momentary).
            ApplyTargetEffect(id, targetID, spellID, kind == CasterConsent::SpellKind::Buff);
            return SelfCast::Applied;
        }
        return SelfCast::Refreshed;   // winning but paced out this tick (transparent)
    }

    void TargetCastReconcile() {
        if (g_targetCast.empty()) return;
        const auto now = SelfClock::now();
        // Same release window as SelfCastReconcile: out-wait the worst-case
        // round-robin + suppression gap so a still-winning rule is never torn down.
        // (KNOWN SEV-1, shared with SelfCastReconcile: g_active is main-thread/
        // serial-task-only (#4) and this runs on the job worker -- tracked in
        // REVIEW-2026-08-18-comprehensive; the whole cluster is being fixed in
        // one wave, do not half-fix it here.)
        const float suppress   = std::max(0.0f, Config::g_suppressWindow.load());
        const float partySize  = static_cast<float>(Followers::g_active.size() + 1);
        const float releaseSec = std::max(2.0f, suppress * 1.12f + 0.133f * partySize + 0.5f);
        std::vector<RE::FormID> done;
        for (auto& [id, tc] : g_targetCast) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(id);
            auto* t = RE::TESForm::LookupByID<RE::Actor>(tc.target);
            const bool gone  = !f || !f->Is3DLoaded() || !t || !t->Is3DLoaded() || t->IsDead();
            const bool stale = std::chrono::duration<float>(now - tc.lastFired).count() > releaseSec;
            // TIME CAP by nature (shared ConcentrationHold numbers): hostile 1-4s,
            // heal 6s, utility 4s. The cap RELEASES + re-streams; it only DISPELS for
            // a sticky Buff (ward). A HEAL/DAMAGE cap is release-only -- the next
            // winning tick re-creates the entry and re-applies immediately, so a
            // still-needed heal FLOWS UNINTERRUPTED (marth's critical constraint).
            float cap;
            switch (tc.kind) {
            case CasterConsent::SpellKind::Offense:
                cap = ConcentrationHold(id, tc.target, tc.kind).holdSeconds; break;   // 1-4s
            case CasterConsent::SpellKind::Heal:
                cap = kConcHealCap;     break;                                         // 6s
            default:
                cap = kConcUtilityHold; break;                                        // Buff: 4s
            }
            const bool capped = std::chrono::duration<float>(now - tc.started).count() >= cap;
            if (gone || stale || capped) {
                // DISPEL a sticky ward on ANY release; a momentary stream's
                // SUSTAINED real effect only when the stream truly ENDED
                // (gone/stale) -- since the real-effect sustain it genuinely
                // channels, so the stream's end must cut it rather than let it
                // run unpaid to its window's end. A CAP-only release re-streams
                // next tick: keeping the effect alive keeps ONE continuous HUD
                // entry across cap boundaries, and the next beat re-arms it.
                const bool endOfStream = gone || stale;
                if (t && (tc.kind == CasterConsent::SpellKind::Buff || endOfStream))
                    TargetCastEndActor(tc.target, tc.spell);
                done.push_back(id);
            }
        }
        for (const auto id : done) g_targetCast.erase(id);
    }

    // AUTO TARGET INFERENCE for act.cast_target (marth). The board's default
    // target pick ("Auto", Subject::Self on a cast-target row) no longer just
    // falls back to the player -- it INFERS the set from the spell's nature
    // and fans the cast out, one direct effect-application per member, each
    // paying the spell's full magicka cost (N targets = N x cost), reserve-
    // floored and paced by fCastCooldown so it neither thrashes nor spikes.
    // A MANUAL pick (Player / Nearest ally / a specific follower) or a
    // selector-chosen target still takes the single-target CastOn path
    // unchanged -- AUTO only fills the "nobody obvious" default.
    //
    // PUBLIC: called from BOTH Fire (combat) and Logistics::ServiceFollower (out
    // of combat), so an authored "always -> Candlelight (Auto)" lights the
    // follower while exploring, and OOC heals/buffs reach the party. Out of
    // combat the hostile branch finds no combat group and NoOps cleanly.
    //
    // ROUTING (classification via CasterConsent::ClassifySpell; delivery is NOT
    // consulted -- MFO applies effects directly, so no spell is "self-only"):
    //   hostile (Offense) -> every nearby enemy (own combat group, chase radius)
    //   beneficial        -> the WHOLE PARTY within shared radius who NEEDS it --
    //                        every active follower + the player, the CASTER
    //                        INCLUDED as one of N (a self-delivery Candlelight thus
    //                        lights everyone). Heals filtered to HP<full; the
    //                        already-active guard skips anyone already covered.
    //
    // The fan-out uses the DIRECT effect applier (ApplyEffectFromTo), the same
    // proven hands-free mechanism as the self-cast, rather than the foe cast
    // PACKAGE: the package is a single-holder (one MFO_CastPackage on alias 0,
    // MAP §2) and cannot address N targets at once, and it is declined outright
    // on package-locked custom followers. Direct application costs magicka per
    // cast, touches only actors (follower-agnostic), and needs no animation
    // (deferred project-wide). Friendly fire is structurally impossible: the
    // effect is placed on the CHOSEN actor, never launched as a projectile.
    Outcome CastAuto(RE::Actor* a_follower, RE::FormID a_spellID, float a_healThreshold) {
            // AE-only, mirroring CastOn / CastSelfDirect (the SE crash path #67).
            if (!REL::Module::IsAE())
                return { Result::FailedOther,
                         "cast control is AE-only (SE/VR use the follower's own AI casting)", true };
            auto* spell = RE::TESForm::LookupByID<RE::SpellItem>(a_spellID);
            if (!spell)
                return { Result::FailedOther,
                         std::format("spell {:08X} not in load order", a_spellID), true };

            const auto id      = a_follower->GetFormID();
            const auto kind    = CasterConsent::ClassifySpell(spell);
            const bool hostile = (kind == CasterConsent::SpellKind::Offense);

            // NOTE (marth, verified in the field): AUTO does NOT collapse a
            // self-delivery spell to the caster. MFO applies effects DIRECTLY to
            // the target actor (ApplyEffectFromTo -> CastSpellImmediate on that
            // actor), bypassing the engine's delivery system entirely -- the same
            // way act.cast_player already lands a self-delivery Candlelight on the
            // PLAYER. So the spell's authored delivery does NOT limit who MFO can
            // place the effect on: a beneficial spell fans to the WHOLE PARTY
            // (every teammate + the player who needs it, the caster included as
            // one of N), each via the direct applier. There is no delivery gate.

            // PACING: one broadcast per fCastCooldown per follower. The rule keeps
            // winning every tick; between cooldowns AUTO is a TRANSPARENT NoOp so
            // lower rules can still run.
            const auto  now      = SelfClock::now();
            const float interval = std::max(1.0f, Config::g_castCooldown.load());
            if (auto it = g_autoCast.find(id); it != g_autoCast.end() &&
                std::chrono::duration<float>(now - it->second).count() < interval)
                return { Result::NoOp, "auto-cast cooling down", true };

            // fix #3/#6: BENEFICIAL DURATION recast suppression -- ADDITIVE to the
            // fCastCooldown pace above, NOT a replacement. A beneficial buff with an
            // authored duration (a light, ward, fortify) must not re-fire until it
            // is near its own real expiry, so even a light that never registers as
            // "already active" (the Magelight spam) is held off, on a jittered/human
            // beat. HOSTILE spells, INSTANT beneficial spells (authoredDuration 0,
            // e.g. instant heals) and CONCENTRATION streams (Healing Hands) are NOT
            // suppressed here -- they re-fire on demand.
            const bool concentration =
                spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration;
            const float authoredDur  = AuthoredDuration(spell);
            const bool  suppressible = !hostile && !concentration && authoredDur > 0.0f;
            const auto  recastKey    = RecastKey(id, a_spellID);
            if (suppressible) {
                if (auto it = g_beneficialRecast.find(recastKey); it != g_beneficialRecast.end() &&
                    std::chrono::duration<float>(now - it->second.lastCast).count() < it->second.windowSec)
                    return { Result::NoOp, "beneficial recast suppressed (buff still up)", true };
            }

            // Running magicka budget on the WORKER (the real deducts are posted to
            // main and have not run yet): read once, subtract each planned cast,
            // and STOP the moment the next cast would breach the reserve floor or
            // empty the pool. §5.3 -- competence is not permission.
            auto* avo = a_follower->AsActorValueOwner();
            if (!avo) return { Result::FailedOther, "no magicka pool", true };
            float       budget  = avo->GetActorValue(RE::ActorValue::kMagicka);
            const float cost    = spell->CalculateMagickaCost(a_follower);
            const float reserve = Config::g_magickaReserve.load();
            const float mx      = avo->GetPermanentActorValue(RE::ActorValue::kMagicka) +
                a_follower->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                                  RE::ActorValue::kMagicka);
            const float floor   = (reserve > 0.0f && mx > 0.0f) ? reserve * mx : 0.0f;
            auto affordable = [&] { return cost <= budget && (budget - cost) >= floor; };

            // ENUMERATE the inferred set (worker-safe reads, same context/precedent
            // as Evaluator::PickFoe / PickAlly which run on this same tick). F1:
            // collect FormIDs, NEVER raw Actor* -- the fan-out below runs AFTER the
            // combat-group read-lock releases, and a stored pointer could dangle if
            // the main thread tears the actor down in that window (UAF). Every
            // post-lock use is FormID-only (LookupByID at apply time).
            std::vector<RE::FormID> targets;
            float radius = 0.0f;
            if (hostile) {
                radius = Confidence::ChaseRadius(a_follower);
                auto& rt = a_follower->GetActorRuntimeData();
                if (auto* cc = rt.combatController; cc && cc->combatGroup) {
                    const auto selfPos = a_follower->GetPosition();
                    RE::BSReadLockGuard lk(cc->combatGroup->lock);
                    for (const auto& t : cc->combatGroup->targets) {
                        auto  ptr = t.targetHandle.get();
                        auto* foe = ptr.get();
                        if (!foe || foe == a_follower) continue;
                        if (foe->IsDead() || foe->IsDisabled() || !foe->Is3DLoaded()) continue;
                        if (t.flags.any(RE::CombatTarget::Flags::kTargetLost)) continue;
                        if (!foe->IsHostileToActor(a_follower)) continue;   // brawl gate (#34)
                        if (selfPos.GetDistance(foe->GetPosition()) > radius) continue;
                        // F2/F3/F4: only fan to a foe this cast will actually
                        // affect -- an instant spell always, a duration DoT only
                        // when it is not already covered (burst-vs-tail test).
                        // Filtering here (worker, foe ptr still valid) means an
                        // all-covered fan-out NoOps transparently below.
                        if (ShouldApplyTo(foe, spell, true))
                            targets.push_back(foe->GetFormID());
                    }
                }
                // F7: warm the LoS cache for the foes we will try to hit, so the
                // apply loop's Sightline::Check stops being fail-open Unknown
                // forever (the verdict lands a frame later; walls do not move).
                if (!targets.empty()) Sightline::Want(id, targets);
            } else {
                // WHOLE PARTY: every active follower + the player within range who
                // NEEDS it -- the CASTER INCLUDED (he is one of N, so a self-buff
                // like Candlelight still lights him too). The already-active guard
                // in ApplyEffectFromTo skips anyone who already has the effect; a
                // health-restoring spell is filtered PER TARGET to the firing rule's
                // HP threshold below (#2). No delivery gate.
                radius = Config::g_sharedRadius.load();
                const auto selfPos = a_follower->GetPosition();
                // Field #2: drive the per-target need gate off the SPELL'S EFFECTS,
                // not solely ClassifySpell. A restore-health spell the classifier
                // does not tag kHeal was bypassing the gate and fanning to the whole
                // party -- a full-HP player got healed because a DIFFERENT ally was
                // hurt. Treat the spell as a heal if EITHER classified Heal OR it
                // carries any heal effect, so ANY health-restoring spell heals only
                // those who actually need it (player and followers alike, below).
                const bool heal = (kind == CasterConsent::SpellKind::Heal) ||
                                  SpellHealsHealth(spell);
                auto consider = [&](RE::Actor* ally) {
                    if (!ally) return;
                    if (ally->IsDead() || ally->IsDisabled() || !ally->Is3DLoaded()) return;
                    if (selfPos.GetDistance(ally->GetPosition()) > radius) return;
                    // #2: heal only allies whose status matches the FIRING gambit's
                    // own condition, not everyone below full. a_healThreshold is the
                    // firing rule's health-below fraction (1.0 = the old blanket
                    // "anyone below full" when the rule carries no health gate, e.g.
                    // "Always -> Heal (Auto)" or the Logistics caller's default). A
                    // "Self: Health < 30% -> Heal (Auto)" rule thus heals only allies
                    // under 30%. Non-heal buffs skip this entirely (heal==false), so
                    // Candlelight still fans to the whole party.
                    if (heal && Vocab::HealthPct(ally) >= a_healThreshold) return;
                    // F2/F3: skip anyone already carrying a duration buff from
                    // this spell (an instant heal leaves no effect and re-fires),
                    // so an all-covered party fans to nobody -> transparent NoOp.
                    if (!ShouldApplyTo(ally, spell, false)) return;
                    targets.push_back(ally->GetFormID());
                };
                for (const auto& h : Followers::g_active) { auto p = h.get(); consider(p.get()); }
                consider(RE::PlayerCharacter::GetSingleton());
            }

            if (targets.empty())
                return { Result::NoOp,
                         hostile ? "auto-cast: no enemies in range" : "auto-cast: nobody needs it", true };

            int fired = 0, skipped = 0;
            for (const auto tgtID : targets) {
                if (!affordable()) { ++skipped; break; }   // insufficient magicka -> stop the fan-out
                if (hostile && Sightline::Check(id, tgtID) == Sightline::Verdict::Occluded) {
                    ++skipped; continue;                    // no line of sight -- fail-open on Unknown
                }
                ApplyEffectFromTo(id, tgtID, a_spellID, hostile);
                budget -= cost;
                ++fired;
            }

            if (fired == 0)
                return { Result::NoOp, "auto-cast: all targets skipped (magicka/LoS)", true };
            g_autoCast[id] = now;
            // fix #3/#6: a beneficial duration buff just landed -- arm its jittered
            // recast window so it is not re-fired until near real expiry.
            if (suppressible)
                g_beneficialRecast[recastKey] = { now, JitteredRecastWindow(authoredDur) };
            spdlog::info("[cast] {:08X} {} AUTO {} {} ({:08X}) -- fanned to {} target(s), {} skipped "
                         "(radius {:.0f}, cost {:.0f} each)",
                         id, a_follower->GetName() ? a_follower->GetName() : "?",
                         hostile ? "HOSTILE" : "BENEFICIAL",
                         spell->GetName() ? spell->GetName() : "?", a_spellID,
                         fired, skipped, radius, cost);
            return { Result::Fired, "auto-cast fan-out" };
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
                    // Unknown weapon (created/enchanted): force-unequip the held
                    // WEAPON in either hand to clear the lock. #8: the force-hold
                    // only ever locks a weapon, but GetEquippedObject also returns
                    // spells, shields and torches -- unequipping one of those would
                    // strip an unrelated off-hand item on load. Weapons only.
                    for (bool leftHand : { false, true }) {
                        if (auto* held = actor->GetEquippedObject(leftHand))
                            if (auto* wep = held->As<RE::TESObjectWEAP>())
                                mgr->UnequipObject(actor, wep, nullptr, 1, nullptr, true, true);
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
