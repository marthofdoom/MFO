#include "PCH.h"
#include "Scheduler.h"
#include "scheduler/Scheduler_internal.h"   // the state shared with scheduler/CombatScan.cpp (IAUS card I0)
#include "Evaluator.h"
#include "cast/Actuation.h"
#include "Followers.h"
#include "Config.h"
#include "Loadout.h"
#include "apmf/APMFBridge.h"   // crisp release of owned-cast claims when no cast rule holds
#include "ComposedCast.h" // ClearWatch -- drop the shared [cfc] silent-claim watch alongside it
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "Packages.h"
#include "Vocabulary.h"
#include "logistics/Logistics.h"
#include "logistics/Errand.h"   // engage in a party fight: not while he is away on a town errand
#include "State.h"
#include "Cooldowns.h"      // card G9: the per-rule cooldown last-fired store (runtime-only)
#include "Confidence.h"   // retreat probe: the fill gate reads Of()
#include "CombatSense.h"  // party combat: the [sense] foe tally (FoeCount), reused, not re-scanned
#include "Forms.h"        // retreat probe: g_retreatPackage for the onPkg readout
#include "Targeting.h"    // flair #5: retarget hesitation reads the current latch
#include "Temperament.h"  // flair #1: per-follower timing seed
#include "Rapport.h"      // #63 quash backstop routes through QuashAllyPair
#include "TauntProbe.h"    // taunt S0 passive probes (bTauntProbe)
#include "StealthProbe.h"  // G2 stealth probe P1-P3 (bStealthProbe)
#include "Taunt.h"         // taunt S2: Taunt::Tick (every pump) / Clear
#include "EngageOnSight.h" // the hidden OOC gambit "nearest visible enemy" (party-OOC branch)
#include "progression/ProgAllocator.h" // §HMS: publish fired combat action pool for the level-up skew
#include "selftest/SelfTest.h" // dev builds only: the self-test runner tick (an empty inline in release)
#include <cstring>          // [deadtgt] strcmp on the CannotActReason strings
#include <unordered_set>   // T#78: MFO-OFF one-time-release latch (g_mfoDisabledSwept)

namespace MFO::Scheduler {


        void StallProbe(RE::Actor* a_f, RE::FormID a_id) {
            const auto now = std::chrono::steady_clock::now();
            const auto pos = a_f->GetPosition();
            auto [it, fresh] = g_stallProbe.try_emplace(a_id, StallSample{ now, pos });
            if (fresh) return;   // the first sample is the baseline
            const float dt = std::chrono::duration<float>(now - it->second.at).count();
            if (dt < kStallProbeS) return;
            const float moved = pos.GetDistance(it->second.pos);
            it->second = StallSample{ now, pos };
            const auto* pkg  = a_f->GetCurrentPackage();
            auto*       pc   = RE::PlayerCharacter::GetSingleton();
            // The player's target: a plain handle member (Actor runtime data 0xFC),
            // resolved the same way Targeting / Rapport resolve a follower's.
            RE::NiPointer<RE::Actor> tgt;
            if (pc) tgt = pc->GetActorRuntimeData().currentCombatTarget.get();
            const float dPlayer = pc ? pos.GetDistance(pc->GetPosition()) : -1.0f;
            // His own down state (field 2026-10-27: stall windows that fell inside a bleedout read as a stall).
            // The life-state member, a plain load, the same read Actuation::CannotActReason makes on this worker.
            const char* down = "no";
            if (const auto* st = a_f->AsActorState()) {
                if (st->GetLifeState() == RE::ACTOR_LIFE_STATE::kBleedout) down = "bleedout";
                else if (st->GetLifeState() == RE::ACTOR_LIFE_STATE::kEssentialDown) down = "essential down";
            }
            spdlog::info("[stall-probe] {:08X} '{}': party combat ON, own combat OFF -- moved {:.0f} u in {:.1f}s "
                         "({:.0f} u/s) dPlayer={:.0f} pkg={:08X} proc={} down={} | player inCombat={} target={:08X} '{}'{}",
                         a_id, a_f->GetName() ? a_f->GetName() : "?", moved, dt, moved / dt, dPlayer,
                         pkg ? pkg->GetFormID() : 0u,
                         pkg ? static_cast<int>(pkg->packData.packType.underlying()) : -1, down,
                         pc && pc->IsInCombat() ? 1 : 0, tgt ? tgt->GetFormID() : 0u,
                         tgt && tgt->GetName() ? tgt->GetName() : "",
                         tgt ? (tgt->IsDead() ? " (dead)" : "") : "");
        }
    namespace {
        // [deadtgt] (PASSIVE, ClickUp 86e3eb078, diag/mfo-dead-target): a follower in combat whose
        // ENGINE current target (currentCombatTarget, the same worker read as the quash backstop)
        // resolves to an actor that is not alive -- dead / dying / recycle / bleedout / essential-down /
        // unconscious / restrained / kill move. One line at first sighting, then at most one per
        // kDeadTgtLogS while it persists, and a closing line with the total duration when it ends
        // (target alive, changed, gone, or combat over). Worker-serial, keyed by follower; cleared on
        // revert (ResetState) and on the party-OOC teardown. Reads only; writes no target, no pin.
        struct DeadTgtState {
            std::chrono::steady_clock::time_point since{};
            std::chrono::steady_clock::time_point lastLog{};
            RE::FormID                            tgt = 0;
        };
        std::unordered_map<RE::FormID, DeadTgtState> g_deadTgt;
        constexpr double kDeadTgtLogS = 1.0;

        // Why a target counts as "not alive", or nullptr. Knockdown / paralysis are NOT down states.
        const char* DeadTgtReason(const RE::Actor* a_t) {
            const char* why = Actuation::CannotActReason(a_t, false);
            if (why && (!std::strcmp(why, "knocked down") || !std::strcmp(why, "paralysed") ||
                        !std::strcmp(why, "knock queued")))
                why = nullptr;
            if (!why && a_t->IsDead()) why = "dead";
            return why;
        }

        // Non-idle casters in the actor's own magicCasters[] (plain loads, as CastInFlightOnHand).
        std::string DeadTgtCasting(RE::Actor* a_f) {
            static constexpr const char* kSlot[RE::Actor::SlotTypes::kTotal] = { "L", "R", "?", "V" };
            std::string out;
            for (std::size_t i = 0; i < RE::Actor::SlotTypes::kTotal; ++i) {
                RE::MagicCaster* c = a_f->GetActorRuntimeData().magicCasters[i];
                if (!c) continue;
                const auto st = c->state.get();
                if (st == RE::MagicCaster::State::kNone || st == RE::MagicCaster::State::kUnk08 ||
                    st == RE::MagicCaster::State::kUnk09)
                    continue;
                auto* sp = c->currentSpell;
                if (!out.empty()) out += ",";
                out += std::format("{}:{:08X} '{}' st={}", kSlot[i], sp ? sp->GetFormID() : 0u,
                                   sp && sp->GetName() ? sp->GetName() : "", static_cast<int>(st));
            }
            return out.empty() ? std::string("none") : out;
        }

    }

        void DeadTgtEnd(RE::FormID a_id, const char* a_why, RE::FormID a_now) {
            const auto it = g_deadTgt.find(a_id);
            if (it == g_deadTgt.end()) return;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - it->second.since).count();
            spdlog::info("[deadtgt] {:08X} engine target {:08X} not-alive window ENDED after {} ms ({}); now={:08X}",
                         a_id, it->second.tgt, ms, a_why, a_now);
            g_deadTgt.erase(it);
        }

        void DeadTgtService(RE::Actor* a_f, RE::FormID a_id, bool a_inCombat) {
            RE::NiPointer<RE::Actor> tp;
            const RE::Actor*         tgt = nullptr;
            const char*              why = nullptr;
            if (a_inCombat) {
                tp  = a_f->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
                tgt = tp.get();
                if (tgt && tgt != a_f) why = DeadTgtReason(tgt);
            }
            if (!why) {
                DeadTgtEnd(a_id, !a_inCombat ? "combat over" : !tgt ? "no target" : "target up",
                           tgt ? tgt->GetFormID() : 0u);
                return;
            }
            const auto tid = tgt->GetFormID();
            auto it = g_deadTgt.find(a_id);
            if (it != g_deadTgt.end() && it->second.tgt != tid) {
                DeadTgtEnd(a_id, "target changed to another not-alive actor", tid);
                it = g_deadTgt.end();
            }
            const auto now = std::chrono::steady_clock::now();
            if (it == g_deadTgt.end()) it = g_deadTgt.emplace(a_id, DeadTgtState{ now, now - std::chrono::hours(1), tid }).first;
            if (std::chrono::duration<double>(now - it->second.lastLog).count() < kDeadTgtLogS) return;
            it->second.lastLog = now;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.since).count();
            const auto latched = Targeting::Current(a_id).get();   // the pin (pin route) / the latch (degrade)
            const auto claim   = APMFBridge::GetCombatTargetClaim(a_id);
            const bool pinned  = Targeting::PinRoute() && latched;
            spdlog::info("[deadtgt] {:08X} '{}' engine target {:08X} '{}' state={} for {} ms; MFO latched={:08X} "
                         "ch6 intended={:08X} pinned={} casting={}",
                         a_id, a_f->GetName() ? a_f->GetName() : "?", tid, tgt->GetName() ? tgt->GetName() : "", why, ms,
                         latched ? latched->GetFormID() : 0u, claim, pinned ? "yes" : "no", DeadTgtCasting(a_f));
        }


        void StartRetreatCooldown(RetreatNote& a_note, std::chrono::steady_clock::time_point a_now) {
            a_note.rearmAt   = a_now + std::chrono::milliseconds(
                                   static_cast<long long>(kRetreatCooldownSecs * 1000.0f));
            a_note.rearmLaps = kRetreatCooldownLaps;
        }

        // THE RETREAT DRIVER for one serviced follower -- called on EVERY service
        // from BOTH tables (party combat and party-OOC), because a retreat's own
        // StopCombat can end the party fight: keying it to the combat table
        // alone is what let the party-OOC teardown cancel a retreat 0.54 s
        // after dispatch (deck-MFO-v9.log:1544-1582, Cicero). Returns true iff he
        // held a retreat at entry -- the retreat is then his action this lap and
        // the caller runs neither table. Ends a retreat ONLY on: no live foes
        // near him (the main-thread probe), the travel timeout, or the end of
        // STAY (engaged at the player's side, sustained out of combat, or the
        // stay cap). Arrival does not end it, it starts STAY -- unless the fight
        // confidence at arrival is already at the floor, or the hold is an
        // out-of-combat act.flee one (skipStay), which release on arrival.
        bool ServiceRetreat(RE::Actor* a_f, RE::FormID a_id, float a_dPlayer, bool a_havePlayer) {
            auto& note = g_retreatNotes[a_id];
            const auto now = std::chrono::steady_clock::now();
            if (note.rearmLaps > 0) --note.rearmLaps;

            if (!Packages::IsRetreating(a_id)) {
                if (note.phase != RetreatPhase::None) {
                    // Released OUTSIDE the driver (dismissal / MFO-off sweep /
                    // load). Not an error; keep the cooldown so the next fill
                    // is not instant.
                    spdlog::info("[retreat] {:08X}: hold released outside the driver -- cooldown", a_id);
                    note.phase = RetreatPhase::None;
                    note.fleeHoldAt = -1.0;
                    StartRetreatCooldown(note, now);
                }
                return false;
            }
            const bool inCombat = APMFBridge::InCombat(a_f);   // a Harbinger substrate is not combat (ABI v26)
            if (note.phase == RetreatPhase::None) {
                // A hold the driver did not dispatch (the act.flee gambit's
                // RetreatFill): adopt it as a fresh TRAVEL. STAY only when he is
                // in combat right now, judged by that in-combat reading; an
                // out-of-combat flee keeps the shipped flee behaviour (release
                // on arrival) -- an out-of-combat Of() is not a fight's
                // confidence (review of 7580bea, SEV-4).
                note.phase           = RetreatPhase::Travel;
                note.took            = false;
                note.outOfCombatSeen = false;
                note.reentries       = 0;
                note.skipStay        = !inCombat;
                note.fleeHoldAt      = -1.0;
                note.fightConf       = inCombat ? Confidence::Of(a_f) : 1.0f;
            }

            if (Forms::IsRetreatPackage(a_f->GetCurrentPackage())) note.took = true;
            if (Packages::RetreatConsumeStopLanded(a_id)) note.outOfCombatSeen = true;
            if (!inCombat) note.outOfCombatSeen = true;

            // One main-thread foe probe per own service; its result is read below
            // on this and later services (it lands next frame).
            Packages::RetreatPostFoeProbe(a_id, Config::g_chaseMax.load());

            const float secs  = Packages::RetreatSeconds(a_id);
            // Measure HER movement, not dPlayer -- dPlayer also closes when the
            // PLAYER walks toward her (the Auri false-positive that made a
            // stuck follower look like a successful retreat).
            const float moved = a_f->GetPosition().GetDistance(Packages::RetreatStartPos(a_id));
            const auto  finish = [&](const char* a_why) {
                APMFBridge::ReleaseRetreatReentryDeny(a_id, a_why);   // ch.22 (no-op when none)
                Packages::RetreatClear(a_why, a_f);
                note.phase = RetreatPhase::None;
                note.fleeHoldAt = -1.0;
                StartRetreatCooldown(note, now);
            };

            const auto foes = Packages::RetreatFoeProbeResult(a_id);
            if (foes.valid && foes.probesSinceFoe >= kRetreatNoFoeProbes &&
                foes.secsSinceFoe >= kRetreatNoFoeSecs) {
                spdlog::info("[retreat] {:08X}: no engaged foes within {:.0f}u for {} probes / {:.1f}s -- "
                             "ending ({} after {:.1f}s, moved {:.0f}, dPlayer={:.0f})",
                             a_id, Config::g_chaseMax.load(), foes.probesSinceFoe, foes.secsSinceFoe,
                             note.phase == RetreatPhase::Stay ? "stay" : "travel", secs, moved, a_dPlayer);
                finish("no live foes");
                return true;
            }

            if (note.phase == RetreatPhase::Travel) {
                if (a_havePlayer && a_dPlayer <= kRetreatArriveDist) {
                    if (note.skipStay || note.fightConf >= kRetreatConfidence) {
                        spdlog::info("[retreat] {:08X}: reached player after {:.1f}s (moved {:.0f}, took={}) -- "
                                     "released ({})", a_id, secs, moved, note.took,
                                     note.skipStay ? "out-of-combat flee, no stay" : "fight confidence at floor");
                        finish("arrived");
                        return true;
                    }
                    spdlog::info("[retreat] {:08X}: reached player after {:.1f}s (moved {:.0f}, took={}) -- "
                                 "STAY (fight confidence={:.2f}, floor {:.2f})",
                                 a_id, secs, moved, note.took, note.fightConf, kRetreatConfidence);
                    note.phase       = RetreatPhase::Stay;
                    note.stayAt      = now;
                    note.stayClockAt = g_serviceClock;
                    // ch.22: the deny covers TRAVEL only. STAY must never bench him --
                    // "engaged at your side" below needs the engine's combat entry to pass
                    // (the SEV-2 of the 7580bea review), so the deny ends at arrival.
                    // A deny still PENDING here never posted its StopCombat (a fast arrival):
                    // post the retreat's own one now, once, as the deny goes (review of
                    // 497b6cd SEV-4 #1) -- otherwise he would never be disengaged.
                    const bool denyPending =
                        APMFBridge::RetreatReentryDenyStateOf(a_id) == APMFBridge::DenyState::Pending;
                    APMFBridge::ReleaseRetreatReentryDeny(a_id, "arrived: STAY");
                    if (denyPending) {
                        spdlog::info("[retreat] {:08X}: arrived with the ch.22 deny still PENDING -- released; "
                                     "posting the retreat's StopCombat once", a_id);
                        Packages::RetreatReengage(a_id, "arrived: STAY (ch.22 deny still pending)");
                    }
                    // fall through to the STAY checks on this same lap
                } else if (inCombat) {
                    // TRAVEL RE-ENTRY: one more main-thread StopCombat, ONLY on
                    // the out -> in edge (#22a: never per tick).
                    note.fightConf = Confidence::Of(a_f);
                    if (note.outOfCombatSeen) {
                        note.outOfCombatSeen = false;
                        ++note.reentries;
                        // ch.22 (apmf/ReentryDeny.cpp): while the deny is PENDING its sweep
                        // posts the StopCombat once live; while it is LIVE the engine cannot
                        // put him back in, so a re-entry is a declared ch.21 entry (passes by
                        // contract) or a Harbinger DENY MISSED -- said loudly, NOT papered over
                        // with another StopCombat (principle 7). No deny / an ENDED one: the
                        // shipped per-re-entry StopCombat, unchanged. A DEGRADED one (never went
                        // live): the same StopCombat, named as a degrade (marth's call pending).
                        const auto deny = APMFBridge::RetreatReentryDenyStateOf(a_id);
                        if (deny == APMFBridge::DenyState::Live) {
                            spdlog::warn("[retreat] {:08X}: re-entered combat mid-retreat (#{}) UNDER A LIVE ch.22 "
                                         "deny -- NOT re-issuing StopCombat. A ch.21 entry passes by contract; "
                                         "otherwise APMF's log shows '[ch.22] ... DENY MISSED' (report it).",
                                         a_id, note.reentries);
                        } else if (deny == APMFBridge::DenyState::Pending) {
                            spdlog::info("[retreat] {:08X}: re-entered combat mid-retreat (#{}) with the ch.22 deny "
                                         "still PENDING -- its StopCombat is posted once it reads live",
                                         a_id, note.reentries);
                        } else {
                            spdlog::info("[retreat] {:08X}: re-entered combat mid-retreat (#{}, confidence={:.2f}) "
                                         "-- StopCombat posted to main{}", a_id, note.reentries, note.fightConf,
                                         deny == APMFBridge::DenyState::Degraded
                                             ? " (DEGRADED: this retreat's ch.22 deny never went live)" : "");
                            Packages::RetreatReengage(a_id, "re-entered combat");
                        }
                    }
                }
                if (note.phase == RetreatPhase::Travel && secs > kRetreatTimeout) {
                    // Transition-only: one line when the fall-back gives up. moved
                    // tells "she walked but too slow" (large) from "controller never
                    // yielded" (near 0).
                    spdlog::warn("[retreat] {:08X}: gave up after {:.1f}s (took={}, moved={:.0f}, dPlayer={:.0f}, "
                                 "re-entries={})", a_id, secs, note.took, moved, a_dPlayer, note.reentries);
                    finish("timeout");
                    return true;
                }
            }

            if (note.phase == RetreatPhase::Stay) {
                const double stayed = g_serviceClock - note.stayClockAt;   // unpaused seconds
                if (inCombat) {
                    // The fight came to him: never hold a follower out of a fight
                    // at the player's side. Release, NO StopCombat.
                    spdlog::info("[retreat] {:08X}: engaged at your side after {:.1f}s of stay -- released "
                                 "(no StopCombat)", a_id, stayed);
                    finish("engaged at your side");
                    return true;
                }
                const float hp = Vocab::HealthPct(a_f);
                // Worker read of his current package (the same read as the `took` line above).
                const auto* curPkg = a_f->GetCurrentPackage();
                const bool fleeCurrent = curPkg && curPkg->packData.packType.get() ==
                                                       RE::PACKAGE_PROCEDURE_TYPE::kFlee;
                if (hp >= Vocab::kHealFull) {
                    if (fleeCurrent) {
                        if (note.fleeHoldAt < 0.0) {
                            note.fleeHoldAt = g_serviceClock;
                            spdlog::info("[retreat] {:08X}: fully healed after {:.1f}s of stay but the engine's "
                                         "Flee package is still current (pkg={:08X}) -- holding until it ends "
                                         "(bound {:.0f}s)", a_id, stayed, curPkg->GetFormID(), kRetreatFleeHoldSecs);
                            return true;
                        }
                        const double held = g_serviceClock - note.fleeHoldAt;
                        if (held < kRetreatFleeHoldSecs) return true;
                        spdlog::warn("[retreat] {:08X}: engine Flee package STILL current after {:.1f}s of flee "
                                     "hold (bound {:.0f}s) -- releasing anyway (report it)", a_id, held,
                                     kRetreatFleeHoldSecs);
                        finish("stay over: healed, flee hold bound");
                        return true;
                    }
                    if (note.fleeHoldAt >= 0.0) {
                        spdlog::info("[retreat] {:08X}: engine Flee ended after {:.1f}s of flee hold -- fully "
                                     "healed, released (dPlayer={:.0f})", a_id, g_serviceClock - note.fleeHoldAt,
                                     a_dPlayer);
                        finish("stay over: healed, flee ended");
                        return true;
                    }
                    spdlog::info("[retreat] {:08X}: stay over after {:.1f}s -- fully healed (dPlayer={:.0f}, "
                                 "engaged foes on last probe={})", a_id, stayed, a_dPlayer, foes.lastCount);
                    finish("stay over: healed");
                } else if (stayed >= kRetreatStaySecs) {
                    // The time release is gated on the engine Flee too (an HP dip mid-hold must not
                    // release him with the Flee still current); same bound from the original fleeHoldAt.
                    if (fleeCurrent) {
                        if (note.fleeHoldAt < 0.0) {
                            note.fleeHoldAt = g_serviceClock;
                            spdlog::info("[retreat] {:08X}: stay time reached after {:.1f}s (health {:.0f}%) but the "
                                         "engine's Flee package is still current (pkg={:08X}) -- holding until it "
                                         "ends (bound {:.0f}s)", a_id, stayed, hp * 100.0f, curPkg->GetFormID(),
                                         kRetreatFleeHoldSecs);
                            return true;
                        }
                        const double held = g_serviceClock - note.fleeHoldAt;
                        if (held < kRetreatFleeHoldSecs) return true;
                        spdlog::warn("[retreat] {:08X}: engine Flee package STILL current after {:.1f}s of flee "
                                     "hold (bound {:.0f}s) -- releasing anyway (report it)", a_id, held,
                                     kRetreatFleeHoldSecs);
                        finish("stay over: time, flee hold bound");
                        return true;
                    }
                    if (note.fleeHoldAt >= 0.0) {
                        spdlog::info("[retreat] {:08X}: engine Flee ended after {:.1f}s of flee hold -- stay time "
                                     "over (health {:.0f}%), released (dPlayer={:.0f})", a_id,
                                     g_serviceClock - note.fleeHoldAt, hp * 100.0f, a_dPlayer);
                        finish("stay over: time, flee ended");
                        return true;
                    }
                    spdlog::info("[retreat] {:08X}: stay over after {:.1f}s (unpaused) -- health {:.0f}% "
                                 "(dPlayer={:.0f}, engaged foes on last probe={})", a_id, stayed, hp * 100.0f,
                                 a_dPlayer, foes.lastCount);
                    finish("stay over: time");
                }
            }
            return true;
        }

        // fix/mfo-unreachable-flyer: the ch.23 leash service with the REACH HOLD. HOLD =
        // Actuation::MeleeOnly AND >= 1 foe airborne out of his melee reach AND no foe the
        // picker could give him in reach (CombatSense::ScanMeleeReach; its chase cap is
        // computed here, before the scan takes the group lock, #23). Holding -> radius
        // fLeashMin around the player (the existing leash, no new movement); otherwise the
        // confidence radius, exactly as before. While holding, a flyer must come
        // kReachHoldBand LOWER than the entry line to end it (hysteresis), so a flyer
        // bobbing on the line cannot Repoint ch.23 every service. Transition lines only;
        // the picker's own [reach] skip line repeats at 5 s while it lasts.
        void ServiceLeashWithReachHold(RE::Actor* a_f, RE::FormID a_id) {
            const bool wasHolding = g_reachHold.contains(a_id);
            const bool meleeOnly  = Actuation::MeleeOnly(a_f);
            CombatSense::ReachScan scan;
            if (meleeOnly) {
                const float cap = Confidence::ChaseRadius(a_f);
                scan = CombatSense::ScanMeleeReach(a_f, cap, wasHolding ? -CombatSense::kReachHoldBand : 0.0f);
            }
            const bool hold = meleeOnly && scan.unreachable > 0 && scan.reachable == 0;
            const float radius = hold ? Config::g_leashMin.load() : Confidence::LeashRadius(a_f);
            APMFBridge::ServicePursuitLeash(a_id, radius);
            if (hold == wasHolding) return;
            if (hold) {
                g_reachHold.insert(a_id);
                auto wp = scan.worst.get();
                const char* wn = (wp && wp->GetName()) ? wp->GetName() : "?";
                if (APMFBridge::PursuitLeashStanding(a_id)) {
                    spdlog::info("[reach] {:08X}: '{}' unreachable (flying, {}), no foe in reach ({} airborne) -- "
                                 "holding by the player (ch.23 leash radius {:.0f})",
                                 a_id, wn, CombatSense::ReachText(scan.worstRead), scan.unreachable, radius);
                } else {
                    spdlog::warn("[reach] {:08X}: '{}' unreachable (flying, {}), no foe in reach ({} airborne) -- "
                                 "should hold by the player, but NO ch.23 pursuit leash stands for him (Harbinger "
                                 "absent / older than v16 / the claim ended): nothing holds him",
                                 a_id, wn, CombatSense::ReachText(scan.worstRead), scan.unreachable);
                }
                // Review U2: a pin / latch made BEFORE the flyer took off is not released by
                // the gates (they only stop NEW latches). On the hold's entry edge, clear it
                // when it names a foe airborne out of his reach, through the one existing
                // release road (Targeting::Clear: the ch.20 pin on either route + the latch).
                if (auto cur = Targeting::Current(a_id).get(); cur) {
                    CombatSense::ReachRead crr;
                    if (CombatSense::OutOfMeleeReach(a_f, cur.get(), 0.0f, &crr)) {
                        Targeting::Clear(a_id);
                        spdlog::info("[reach] {:08X}: target pin on '{}' cleared (flying, {}) -- he holds, "
                                     "not latched on a foe he cannot reach",
                                     a_id, cur->GetName() ? cur->GetName() : "?", CombatSense::ReachText(crr));
                    }
                }
            } else {
                g_reachHold.erase(a_id);
                const char* why = !meleeOnly            ? "no longer melee-only"
                                : scan.reachable > 0    ? "a foe is in reach"
                                                        : "no airborne foe out of reach";
                spdlog::info("[reach] {:08X}: hold ended ({}) -- ch.23 leash back to the confidence radius {:.0f}",
                             a_id, why, radius);
            }
        }


    void ClearTransientState() {
        g_retreatNotes.clear();
        g_retreatHealLog.clear();
        EngageOnSight::ClearTransientState();   // its per-follower notes + the probe mirror
        g_serviceClock   = 0.0;
        g_serviceClockAt = {};
        g_mfoDisabledSwept.clear();   // T#78: revert/load re-arms the OFF-edge release
        g_outOfCombatTicks.clear();
        g_meleeClampTrueAt.clear();   // T#76 hysteresis dwell
        g_partyCombat = false;        // party-combat substrate: no phantom OFF edge in the next world
        g_partyOffAt = -1.0e300; g_partyOffDeferLogged = false;
        g_partyCombatNoted.clear();
        g_substrateNoted.clear();
        g_anchorLeakNoted.clear();
        g_stallProbe.clear();                   // [stall-probe] samples are session state
        g_deadTgt.clear();                      // [deadtgt] windows are session state
        TauntProbe::Reset();                    // [taunt-probe] samples are session state
        StealthProbe::Reset();                  // [stealth-probe] samples, pending hits and the q87 aggregate are session state
        Taunt::Clear();                         // taunt attempts + pinned foes: logged "load", claims released
        g_noneMatchedNoted.clear();
        g_equipRangeUndecidableNoted.clear();   // MFO-B55 once-per-fight note
        g_reachHold.clear();                    // fix/mfo-unreachable-flyer: the leash hold
        g_recent.clear();
        g_combatEnteredAt.clear();
        g_proposedTarget.clear();
        g_nextSense.clear();          // #11: was a Tick-local static that leaked
        Confidence::ClearTrend();     // Confidence v2: the per-follower HP ring is session state
        g_lastServiced = 0;
        g_lastTick = {};
        g_lastTickMs = 0.0;
        g_ticks = 0;
    }

    double        LastTickMs()       { return g_lastTickMs.load(); }
    std::uint32_t TicksThisSession() { return g_ticks.load(); }
    double        ServiceClock()     { return g_serviceClock; }   // worker-only (#4), like its writer
    bool PartyCombat() { return g_partyCombat; }   // worker-only, like g_partyCombat itself
    bool PartyCombatSettling() {   // worker-only, like g_partyCombat itself
        if (g_partyCombat || g_serviceClock - g_partyOffAt >= kOocCastDebounceSecs) return false;
        if (!g_partyOffDeferLogged) {
            g_partyOffDeferLogged = true;
            spdlog::info("[sched] out-of-combat casts held {:.0f} s after party combat went OFF (now {:.1f} s in): "
                         "a substrate open right at the edge leaks Harbinger's anchor while foes are still around",
                         kOocCastDebounceSecs, g_serviceClock - g_partyOffAt);
        }
        return true;
    }

    float ReachHoldSlack(RE::FormID a_follower) {                 // worker-only (#4), like g_reachHold
        return g_reachHold.contains(a_follower) ? -CombatSense::kReachHoldBand : 0.0f;
    }

}
