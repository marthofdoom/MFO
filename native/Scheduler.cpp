#include "PCH.h"
#include "Scheduler.h"
#include "Evaluator.h"
#include "Actuation.h"
#include "Followers.h"
#include "Config.h"
#include "Loadout.h"
#include "CasterConsent.h"
#include "Packages.h"
#include "Vocabulary.h"
#include "Logistics.h"
#include "State.h"
#include "Confidence.h"   // retreat probe: the fill gate reads Of()
#include "Forms.h"        // retreat probe: g_retreatPackage for the onPkg readout

namespace MFO::Scheduler {

    namespace {

        // §4.1: 133 ms is the human simple-reaction floor and the response
        // deadline. The frame-clock form -- max(4 frames, 133 ms) -- is a later
        // slice; chrono alone is correct at >= 30 fps and errs toward polling
        // LESS often below it, which is the safe direction.
        constexpr auto kTickInterval = std::chrono::milliseconds(133);

        std::chrono::steady_clock::time_point g_lastTick{};

        // Round-robin position, remembered by IDENTITY not index (#31).
        // Followers::Refresh() rebuilds g_active every diagnostics wake and
        // re-appends held-miss entries at the BACK, so a bare index silently
        // services one follower twice and skips another whenever the list
        // reorders -- worst in exactly the large parties where response time
        // is already at its ceiling.
        RE::FormID g_lastServiced = 0;

        // Actuation state, not evaluator state: a record of what has already
        // been done to the world. The evaluator itself stays stateless between
        // ticks (INVARIANTS #22).
        struct Recent {
            std::chrono::steady_clock::time_point until{};
            int         firedRule = -1;     // which rule bought the quiet
            int         failRule  = -1;     // last failure reported, for #22j
            std::string failReason;
        };
        std::unordered_map<RE::FormID, Recent> g_recent;

        std::atomic<double>        g_lastTickMs{ 0.0 };
        std::atomic<std::uint32_t> g_ticks{ 0 };

        // ── AUTO-RETREAT bookkeeping ────────────────────────────────────────
        // One fall-back per combat per follower: `tried` arms once when the
        // fill gate first passes and is erased when the follower leaves combat,
        // so the next fight can fall back again. `took` records whether the
        // travel package was ever his current package -- lets the timeout line
        // tell "controller never yielded" from "took but too slow".
        struct RetreatNote {
            bool tried = false;
            bool took  = false;
        };
        std::unordered_map<RE::FormID, RetreatNote> g_retreatNotes;

        // The dials. Fill: confidence below 0.25 (a follower who by the
        // leash tenet WANTS to be at the player's side) while >400u away from
        // the player -- far enough that arrival is an unambiguous pull, not
        // drift. Arrival: 200u (package radius 150 + engine stop slack).
        // Timeout: 30 s -- past any plausible walk time at Run speed.
        constexpr float kRetreatConfidence = 0.25f;
        constexpr float kRetreatMinDist    = 400.0f;
        constexpr float kRetreatArriveDist = 200.0f;
        constexpr float kRetreatTimeout    = 30.0f;

    }

    void ClearTransientState() {
        g_retreatNotes.clear();
        g_recent.clear();
        g_lastServiced = 0;
        g_lastTick = {};
        g_lastTickMs = 0.0;
        g_ticks = 0;
    }

    double        LastTickMs()       { return g_lastTickMs.load(); }
    std::uint32_t TicksThisSession() { return g_ticks.load(); }

    void Tick() {
        const auto now = std::chrono::steady_clock::now();

        // NEVER CATCH UP (INVARIANTS #24). Fire at most once per wake and reset
        // the anchor to now -- a `last += interval` loop turns a load screen
        // into a burst of queued evaluations on the most loaded frame there is.
        // Tolerance: the pump paces at kTickInterval, but this anchor is set at
        // TASK-EXECUTION time and AddTask latency jitters. Without slack, a wake
        // landing a few ms early is skipped entirely and the gap silently
        // doubles to 266 ms -- safe in direction, but it halves response speed
        // at random. The pump is the only caller; it does the real pacing.
        constexpr auto kSlack = std::chrono::milliseconds(10);
        if (g_lastTick.time_since_epoch().count() != 0 && (now - g_lastTick) < (kTickInterval - kSlack)) return;
        g_lastTick = now;

        // OBSERVE THE PACKAGE STATE MACHINE FIRST, and unconditionally.
        //
        // It must run BEFORE every early return below, because the conditions
        // those returns test are exactly the conditions under which a
        // commanded action needs releasing: the party emptied, the follower
        // left combat, the game paused, the record vanished. Pump() is the only
        // thing that advances Requested -> Filled -> Running -> Done, and the
        // fill it is watching is engine state that OUTLIVES the session -- so a
        // tick that returns early without pumping is a tick that can strand a
        // latch in the save.
        Packages::Pump();

        auto& active = Followers::g_active;
        if (active.empty()) return;

        const auto t0 = std::chrono::steady_clock::now();
        ++g_ticks;

        // ROUND-ROBIN, ONE PER TICK. Cost is O(1) in party size; a large party
        // pays in response time, not framerate (§4.1a). Resume AFTER whoever
        // was serviced last, by identity, so a reordered list cannot double-
        // service or starve anyone.
        size_t start = 0;
        if (g_lastServiced != 0) {
            for (size_t i = 0; i < active.size(); ++i) {
                auto* c = active[i].get().get();
                if (c && c->GetFormID() == g_lastServiced) { start = i + 1; break; }
            }
        }
        const size_t idx = start % active.size();

        auto* f = active[idx].get().get();
        if (!f) {
            // ADVANCE ANYWAY. Followers::Refresh deliberately re-pushes handles
            // that fail to resolve, for up to kMissesBeforeDrop sweeps -- the
            // hold exists because handles transiently fail (a 117 ms flicker
            // was eating kills before it). But a held null sits at a fixed
            // position, so a cursor that does not move past it lands on the
            // same entry every tick and NOBODY in the party is evaluated for up
            // to ~1.6 s. In combat, which is the only time it matters.
            // g_activeIds is maintained in lockstep for exactly this: it still
            // knows who this slot is when the handle cannot say.
            if (idx < Followers::g_activeIds.size()) g_lastServiced = Followers::g_activeIds[idx];
            return;
        }
        const auto id = f->GetFormID();
        g_lastServiced = id;

        // Cheap disqualifiers before any evaluation.
        if (f->IsDead() || f->IsDisabled()) return;

        // A cast/drink/loot issued into a paused game resolves strangely on
        // unpause, and menus are exactly when the player is editing the list
        // that drives it. Applies to BOTH tables, so it gates before the branch.
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GameIsPaused()) return;

        const auto it = g_followers.find(id);
        if (it == g_followers.end()) return;          // no record -> nothing to run

        // THE TWO TABLES NEVER INTERLEAVE (§4.8). Combat runs in combat;
        // logistics -- upkeep -- runs out of it. Without the split a seeded heal
        // rule fires while shopping in Whiterun. Logistics is cadence-gated to
        // the ~1 s idle rate INSIDE ServiceFollower, so calling it every service
        // is cheap; it acts at most once per idle tick and is off by default.
        if (!f->IsInCombat()) {
            // RETREAT PROBE teardown on combat end: release the claim (evict to
            // player -- never a VM Clear, never a priority flip) and re-arm the
            // once-per-combat latch for the next fight.
            if (Packages::RetreatHolder() == id) {
                Packages::RetreatClear("combat ended", f);
            }
            g_retreatNotes.erase(id);

            Logistics::ServiceFollower(f, it->second);
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
        }

        // IN COMBAT: end any loot excursion for this follower FIRST, so he fights
        // instead of staying claimed by the loot quest at priority 60 (batches
        // last up to 60 s and would otherwise run right through the fight, making
        // him look passive). Must be before the combat-rules early-out below, so
        // it yields even for a follower with no combat gambits.
        Logistics::ReleaseTravelOnCombat(f);

        // ── AUTO-RETREAT (leash safety, opt-in via bAutoRetreat) ─────────────
        // The confidence leash taken to its conclusion: a follower who is badly
        // outmatched (confidence below threshold -- by the leash tenet he WANTS
        // to be at the player's side) AND far from the player in combat falls
        // back under a kIgnoreCombat alias Travel package that outranks his
        // combat controller's locomotion. Fires once per fight; teardown on
        // arrival / timeout here and on combat end in the non-combat branch
        // above. OFF by default -- a default install never acts without an
        // authored rule. Measured reliable in §0.36; logging is transition-only.
        {
            auto* pc = RE::PlayerCharacter::GetSingleton();
            const float dPlayer = pc ?
                f->GetPosition().GetDistance(pc->GetPosition()) : 0.0f;

            if (Packages::RetreatHolder() == id) {
                auto& note = g_retreatNotes[id];
                const float secs  = Packages::RetreatSeconds();
                auto*       cur   = f->GetCurrentPackage();
                if (cur == Forms::g_retreatPackage) note.took = true;

                if (pc && dPlayer <= kRetreatArriveDist) {
                    spdlog::debug("[retreat] {:08X}: reached player after {:.1f}s", id, secs);
                    Packages::RetreatClear("arrived", f);
                } else if (secs > kRetreatTimeout) {
                    // Transition-only: one line when the fall-back gives up, with
                    // enough to tell "controller never yielded" from "too slow".
                    spdlog::debug("[retreat] {:08X}: gave up after {:.1f}s (took={}, dPlayer={:.0f})",
                                  id, secs, note.took, dPlayer);
                    Packages::RetreatClear("timeout", f);
                }

                // While falling back, do NOT run the gambit table: a cast rule
                // would fill the COMMAND alias (also priority 60) on the same
                // actor and fight the retreat travel for the alias. A retreating
                // follower is disengaging, not gambitting -- that is the point.
                g_lastTickMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                return;
            }

            auto& note = g_retreatNotes[id];
            if (Config::g_autoRetreat.load() && !note.tried &&
                Confidence::Of(f) < kRetreatConfidence &&
                f->IsInCombat() && pc && dPlayer > kRetreatMinDist) {
                note.tried = true;   // one fall-back per fight, fill failures included
                if (Packages::RetreatFill(f)) {
                    spdlog::debug("[retreat] {:08X}: falling back -- confidence={:.2f} dPlayer={:.0f}",
                                  id, Confidence::Of(f), dPlayer);
                }
            }
        }

        if (it->second.combat().empty()) return;      // no rules -> nothing to run

        const auto choice = Eval::Evaluate(f, it->second);

        // THE FREQUENCY LIMITER. A gambit spell stays in the follower's hand
        // only while a cast rule still wants it -- because their AI casts what
        // they are holding, and MFO controls what they hold. Leaving a heal
        // equipped after the follower is healed is what let one drain ~1000
        // magicka: the condition had gone false and the spell was still there.
        //
        // This is the honest lever. MFO cannot tell the combat AI "cast less";
        // it can decide what is available to cast.
        const bool wantsCast = choice.ruleIndex >= 0 &&
                               (choice.actionOpcode == Vocab::kActCastSelf ||
                                choice.actionOpcode == Vocab::kActCastTarget);
        if (!wantsCast) { Loadout::ReleaseSpell(id); CasterConsent::Clear(id); }

        // §4.4: no match means NO ENGINE CALL. Not a neutral command -- nothing.
        if (choice.ruleIndex < 0) {
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
        }

        // SUPPRESSION IS POSITIONAL, NEVER ABSOLUTE (INVARIANTS #26).
        // An absolute per-follower window means a just-fired rule 6 deafens the
        // follower to the player's rule 1 heal for the whole window -- the
        // priority inversion #26 exists to forbid, and 1.5 s of it on a dying
        // follower is player-visible. So: still evaluate, and let a HIGHER
        // rule (lower index) preempt. Only equal-or-lower rules stay quiet.
        if (auto r = g_recent.find(id); r != g_recent.end() && now < r->second.until) {
            if (choice.ruleIndex >= r->second.firedRule) {
                g_lastTickMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                return;
            }
        }

        const auto outcome = Actuation::Fire(f, choice);

        // Re-find rather than reuse the iterator: Fire() dispatches engine
        // events synchronously, and INVARIANTS #2 says re-find live records by
        // key at act time instead of holding one across an engine call.
        auto& recent = g_recent[id];
        if (auto rec = g_followers.find(id); rec != g_followers.end()) {
            if (choice.ruleIndex < static_cast<int>(rec->second.combat().size())) {
                // Display only -- the evaluator never reads these back (#22).
                auto& rule = rec->second.combat()[choice.ruleIndex];
                rule.lastFired = (outcome.result == Actuation::Result::Fired);
                rule.lastFailReason = outcome.reason;
            }
        }

        switch (outcome.result) {
        case Actuation::Result::Fired:
            // Suppress only on a REAL action. A wait or a failed cast must not
            // buy quiet time, or a follower who cannot afford a heal would go
            // silent instead of falling through on the next tick.
            recent.until = now + std::chrono::milliseconds(
                static_cast<int>(Config::g_suppressWindow.load() * 1000.0f));
            recent.firedRule = choice.ruleIndex;
            recent.failRule  = -1;
            recent.failReason.clear();
            spdlog::info("[eval] {:08X} fired rule {} ({})", id, choice.ruleIndex, choice.actionOpcode);
            break;

        case Actuation::Result::FailedSkill:
        case Actuation::Result::FailedOther:
            // §5.3: say WHY -- but ON TRANSITION ONLY. A permanently failing
            // rule is the WINNING rule every tick (failures correctly do not
            // suppress), so logging it unconditionally means ~7.5 lines/sec
            // per follower, each with a synchronous flush on the main thread:
            // both a frame cost and a flood that drowns the signal (#22j).
            if (recent.failRule != choice.ruleIndex || recent.failReason != outcome.reason) {
                recent.failRule   = choice.ruleIndex;
                recent.failReason = outcome.reason;
                spdlog::info("[eval] {:08X} rule {} ({}) did NOT fire: {}",
                             id, choice.ruleIndex, choice.actionOpcode, outcome.reason);
            }
            break;

        case Actuation::Result::NoOp:
            // LOG THE SILENT PATH TOO, on transition.
            //
            // A NoOp with a reason is a decision MFO made -- "giving their AI a
            // chance", "caster has not selected the spell yet", "already on that
            // target". Every one of those was invisible, and a whole field
            // session was spent on a probe whose most likely outcome was a
            // silent NoOp: the log showed no [drive] line and no rule 0, which
            // read as "nothing happened" when it may have been happening every
            // tick. That is #53 in the one place it costs a test.
            //
            // Reasonless NoOps (act.wait, no rule matched) stay quiet -- those
            // really are nothing.
            if (!outcome.reason.empty() &&
                (recent.failRule != choice.ruleIndex || recent.failReason != outcome.reason)) {
                recent.failRule   = choice.ruleIndex;
                recent.failReason = outcome.reason;
                spdlog::info("[eval] {:08X} rule {} ({}) held off: {}",
                             id, choice.ruleIndex, choice.actionOpcode, outcome.reason);
            }
            break;

        default:
            break;
        }

        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        g_lastTickMs = ms;
        if (Config::g_profileEvaluator.load()) {
            spdlog::info("[eval-profile] tick #{} on {:08X}: {:.3f} ms ({} active)",
                         g_ticks.load(), id, ms, active.size());
        }
    }

}
