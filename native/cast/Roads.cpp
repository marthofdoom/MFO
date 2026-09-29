// cast/Roads.cpp -- the three delivery ROADS CastOn forks to: the forced
// package cast (ForceCast), the bounded concentration stream entry
// (ConcentrationCast) and restoration-at-a-target (RestorationCastDirect).
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
                // F3 tri-state (mirrors Logistics.cpp's OOC cast dispatch,
                // ~`:1500-1560`, + CastOn's self fork -- the old cite here pointed at
                // Logistics.cpp:3987-3996, which has not existed since that file was
                // split; it is 2065 lines): only a real Applied is this tick's action;
                // a Refreshed
                // (channel paced, nothing applied) is TRANSPARENT so it does not
                // starve the rules below it, and the fired log never lies.
                if (Config::g_castSelf.load()) {
                    switch (CastSelfDirect(a_follower, a_spell)) {
                    case SelfCast::Applied:
                        HoldCastLock(id, kHandLeft, a_spell->GetFormID(), 0);   // TASK 2
                        return { Result::Fired, "self-cast (direct trigger)" };
                    case SelfCast::Refreshed:
                        HoldCastLock(id, kHandLeft, a_spell->GetFormID(), 0);   // TASK 2
                        return { Result::NoOp, "self-cast refresh (paced)", true };
                    // Fable SEV-2 (2026-09-06): a HELD outcome is not a cast.
                    // ComposedCast::Try held this spell off because a DIFFERENT
                    // spell's live claim owns the follower's single heal slot, so
                    // nothing was claimed and nothing was applied. It used to
                    // arrive as Applied (the hold rode home inside Try()'s bare
                    // `true`), which reported a fired cast that never happened AND
                    // re-stamped the Task-2 hand lock with the HELD-OFF spell --
                    // on any lap where no lock was live yet (fresh after
                    // ClearCastLock, the Scheduler's no-cast-rule path, an
                    // OOC->combat transition) that re-label starved BOTH rules
                    // until a TTL. Transparent NoOp, its own label, NO lock.
                    case SelfCast::Held:
                        return { Result::NoOp, "self-cast held off (another heal owns the claim)", true };
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
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::Fired, "concentration (direct force)" };
            case SelfCast::Refreshed:
                // Live stream, paced out this tick -- the wait IS the action;
                // transparent like the FF form.
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::NoOp, "concentration direct refresh (paced)", true };
            // Fable SEV-2 (2026-09-06): a HELD outcome is not a cast.
            // ComposedCast::Try held this spell off because a DIFFERENT
            // spell's live claim owns the follower's single heal slot, so
            // nothing was claimed and nothing was applied. It used to
            // arrive as Applied (the hold rode home inside Try()'s bare
            // `true`), which reported a fired cast that never happened AND
            // re-stamped the Task-2 hand lock with the HELD-OFF spell --
            // on any lap where no lock was live yet (fresh after
            // ClearCastLock, the Scheduler's no-cast-rule path, an
            // OOC->combat transition) that re-label starved BOTH rules
            // until a TTL. Transparent NoOp, its own label, NO lock.
            case SelfCast::Held:
                return { Result::NoOp, "concentration held off (another heal owns the claim)", true };
            case SelfCast::Declined:
            default:
                // Unaffordable (§5.3) / LoS or line-of-fire held (offense) /
                // unverified runtime (Runtime.h): transparent + legible, the rules below run.
                return { Result::FailedOther, "concentration direct-force declined", true };
            }
        }

        // ── RESTORATION AT A TARGET: the DIRECT road, fire-and-forget too ──
        // (fix/mfo-combat-restoration-direct, 2026-09-21.) A restoration cast
        // (IsRestorationSpell, Actuation.h) aimed at an ally/player on the
        // COMBAT table is delivered exactly like the concentration stream
        // above -- CastTargetDirect, CastSpellImmediate straight onto the
        // target, MainThread::Post'd, magicka deducted, bounded by
        // TargetCastReconcile -- and NEVER through the ch.8b AI-fired heal
        // claim (ComposedCast::Try + equip + AI grace + ForceCast). Deck
        // 2026-09-21: that road held Jesper's left hand on a Fast Healing
        // claim the engine never fired (it kept re-selecting Healing Hands
        // and was denied), the idle-hand floor closed his right, and he stood
        // frozen; the OOC logistics heal on this same direct road delivered
        // 22/22. Same outcome mapping and hand lock as the non-self branch of
        // ConcentrationCast (LEFT, the hand every heal/buff plan resolves to);
        // CasterConsent::Want keeps the slider denying the AI's COMPETING
        // spells while the rule governs. Self never reaches here (the self
        // fork intercepts it), and a concentration spell forks first.
        Outcome RestorationCastDirect(RE::Actor* a_follower, RE::SpellItem* a_spell,
                                      RE::Actor* a_target) {
            const auto id = a_follower->GetFormID();
            switch (CastTargetDirect(a_follower, a_spell, a_target)) {
            case SelfCast::Applied:
                CasterConsent::Want(id, a_spell->GetFormID());
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::Fired, "restoration (direct force)" };
            case SelfCast::Refreshed:
                HoldCastLock(id, kHandLeft, a_spell->GetFormID(), a_target->GetFormID());   // TASK 2
                return { Result::NoOp, "restoration direct refresh (paced)", true };
            case SelfCast::Held:
                return { Result::NoOp, "restoration held off (another heal owns the claim)", true };
            case SelfCast::Declined:
            default:
                return { Result::FailedOther, "restoration direct-force declined", true };
            }
        }

}
