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

    }

    void ClearTransientState() {
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
            Logistics::ServiceFollower(f, it->second);
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
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
