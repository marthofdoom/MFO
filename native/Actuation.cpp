// Actuation.cpp -- the combat-rule ACTUATION dispatch (split mechanically,
// no logic change; the direct-delivery streams + apply substrate live in
// Actuation_Direct.cpp): Fire and its verbs, ForceCast, the bounded
// concentration stream entry (ConcentrationCast), CastOn, EquipWeapon +
// the #76 force-hold bookkeeping and its FWPN co-save, NearestAlly and
// ResolveCastTarget. Shared concentration numbers: Actuation_internal.h.
#include "Actuation_internal.h"
#include "APMFBridge.h"   // Phase 3: APMF cast-selection assist (additive, guarded)

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

    // OWNED-CAST DEDUPE (fix, marth 2026-09-02). The owned-cast branch below
    // (CastOn) re-evaluates every ~133ms tick while the follower's AI is still
    // deciding/casting. Re-issuing the APMF facet claims + Targeting::Command
    // every tick is wasted work on the combat/APMF side; keyed per follower on
    // {target, spell} so a steady-state latch is one claim + one command, not
    // one every tick. NOT an engine hold (no equip/stow to undo) -- cleared
    // per-follower on teardown (Followers::ReleaseHeldState) and globally on
    // revert (ClearOwnedCastState, mirrors ClearForcedWeapons/ClearSelfCasts).
    std::unordered_map<RE::FormID, std::pair<RE::FormID, RE::FormID>> g_ownedCastLast;

    void ReleaseOwnedCastLatch(RE::FormID a_actorID) { g_ownedCastLast.erase(a_actorID); }
    void ClearOwnedCastState() { g_ownedCastLast.clear(); }

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

                    // ── OWNED CAST MODEL (default when APMF present; MODERATOR redesign,
                    // marth 2026-09-02). A REAL, AI-DECIDED, MOBILE, ANIMATED cast, made
                    // by the FOLLOWER'S OWN combat AI -- MFO does NOT force it. This is the
                    // same real cast MFO could already produce (equip + consent + a cast-
                    // biased combat style => the AI's own vanilla animated cast, ENGINE_NOTES
                    // §0.15a/§0.27/§0.28), but WITHOUT its old movement cost: the animated
                    // cast never needed the rooting UseMagic package (that was only the
                    // legacy force route) -- so we drop the package entirely and the follower
                    // keeps kiting while it casts.
                    //
                    // DIVISION OF LABOUR (the whole point of routing through APMF):
                    //   * APMF ARBITRATES the facets -- MFO CLAIMS the cast + combat-target
                    //     facets so APMF is the single arbiter (and can suppress competitors).
                    //     APMF executes NOTHING; it makes no cast/combat call.
                    //   * MFO EXECUTES the behaviour with its OWN proven mechanisms:
                    //       - SELECT our spell: Loadout::Prepare's EquipSpell into the LEFT
                    //         hand slot (above, §0.28) -- selection comes from the equip, NOT a
                    //         hand-written selectedSpells/currentSpell (ENGINE_NOTES §585: never
                    //         write currentSpell by hand, it desyncs the engine's own select/
                    //         deselect bookkeeping -- the per-tick raw write that regressed this
                    //         model was doing exactly that, on the WRONG hand, resetting the
                    //         charge before it could release; fixed 2026-09-02);
                    //       - COMMAND our target: Targeting::Command -> currentCombatTarget
                    //         (its UpdateCombat hook re-asserts it);
                    //       - CONSENT + deny competing spells: CasterConsent::Want (granted
                    //         just above) -- the AI is permitted to cast OUR spell and denied
                    //         its others;
                    //       - DECIDE to cast: the Cast-biased combat style (Scheduler applies
                    //         MFO_CastStyle each combat tick a cast is wanted -- raises the
                    //         magic score so the AI CHOOSES to cast; the INVERSE of a deny,
                    //         never a force).
                    // GRANULAR: we claim ONLY the cast + combat-target facets -- we do NOT
                    // touch the movement facet (no SetDontMove, no block), so the follower
                    // keeps moving under its own control WHILE its AI casts. That granular
                    // non-interruption is exactly APMF's value for casting.
                    //
                    // NO forced cast on this path -- CastSpellImmediate NEVER runs here; force
                    // survives only in the LEGACY hybrid below (bLegacyCastHybrid / APMF
                    // absent). If a follower's combat style still will not DECIDE to cast,
                    // that is a magic-score bias question (raise it), NOT a reason to force.
                    // Concentration never reaches this branch (its bounded direct fork
                    // returned above -- exact-bounding intact).
                    const bool ownedCast =
                        APMFBridge::Available() && Config::g_apmfCast.load() &&
                        !Config::g_legacyCastHybrid.load() &&
                        a_target && a_target != a_follower &&
                        a_target != RE::PlayerCharacter::GetSingleton() &&
                        CasterConsent::ClassifySpell(spell) == CasterConsent::SpellKind::Offense;

                    if (ownedCast) {
                        // IDEMPOTENCY GUARD (fix, marth 2026-09-02): the AI-deciding hold below
                        // means this branch re-fires every ~133ms tick while the same target and
                        // spell stay latched. Re-issuing the APMF claims + Targeting::Command
                        // every tick is redundant (the claim/command already stands) and was
                        // masking the real regression above. Only re-issue when {target, spell}
                        // actually changed since the last tick for this follower.
                        const RE::FormID followerId = a_follower->GetFormID();
                        const RE::FormID targetId   = a_target->GetFormID();
                        const RE::FormID spellId    = spell->GetFormID();
                        auto& last = g_ownedCastLast[followerId];
                        const bool unchanged = last.first == targetId && last.second == spellId;

                        if (!unchanged) {
                            // ARBITRATION: claim the two facets via APMF (it records the owner +
                            // suppresses competitors; it executes nothing). Movement is NOT claimed.
                            APMFBridge::ClaimCasting(followerId, spellId);
                            APMFBridge::ClaimCombatTarget(followerId, targetId, /*create=*/true);

                            // EXECUTION (MFO's own): our spell is already selected via the equip
                            // above (Loadout::Prepare); command our target. Consent was granted
                            // above; the Cast combat style (Scheduler) supplies the AI's DECISION.
                            // The follower's own AI then casts our spell at our target -- full
                            // animation, still mobile.
                            Targeting::Command(followerId, a_target->GetHandle());

                            last = { targetId, spellId };
                        }

                        // OPAQUE hold: the AI is deciding+casting; firing lower rules now risks
                        // disturbing that decision (the §0.6 confound). No force, ever, here.
                        return { Result::NoOp, unchanged ? "owned cast: latched (unchanged)"
                                                          : "owned cast: AI deciding (animated, mobile)" };
                    }

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
            // v1.1: actor-keyed g_followers find + combatClassOverride read == the
            // general GetBaseClass primitive (byte-identical; same serial-worker path).
            const std::uint8_t baseClass = Followers::GetBaseClass(a_follower);
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

            // OWNED MODEL: if this follower is a caster with a LIVE APMF combat-target
            // CLAIM (created by a cast directive earlier this fight), keep that facet-claim
            // pointed at the melee foe too, so the arbitration record follows a cast->melee
            // transition. create=false => it NEVER creates a claim for a pure-melee follower.
            // (MFO's own Targeting::Command below is what actually commands the target;
            // APMF only arbitrates the facet, it executes nothing.) No-op without APMF.
            if (APMFBridge::Available() && Config::g_apmfCast.load() &&
                !Config::g_legacyCastHybrid.load())
                APMFBridge::ClaimCombatTarget(a_follower->GetFormID(), foe->GetFormID(), /*create=*/false);

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
            // the retreat driver. #35.
            if (Packages::RetreatFill(a_follower))
                return { Result::Fired, "flee -> retreat to player" };
            return { Result::FailedOther, "retreat alias unavailable", true };
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
