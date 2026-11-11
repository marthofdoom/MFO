#pragma once
// scheduler/Scheduler_internal.h -- the state native/Scheduler.cpp and
// native/scheduler/CombatScan.cpp share: the tick / round-robin cursors, the
// per-follower latches and notes, the retreat dials, and the prototypes of the
// service helpers Scheduler::Tick calls. The variables are `inline` (ONE shared
// instance across the two TUs, never per-TU copies). The move changes no
// threading: each item keeps the rule its own comment states (MAP.md, #4).
// NOT a public API: only those two TUs and scheduler/IausSource.cpp (IAUS card
// I6: the scan's IAUS source reads g_recent / g_proposedTarget / g_retreatHealLog)
// may include this; everything else includes Scheduler.h.
// Split out of native/Scheduler.cpp by IAUS card I0 (2026-11-10): a pure move,
// proven function by function with tools/splitcheck.

#include "PCH.h"
#include "Scheduler.h"
#include <unordered_set>   // T#78: MFO-OFF one-time-release latch (g_mfoDisabledSwept)

namespace MFO::Scheduler {


        // §4.1: 133 ms is the human simple-reaction floor and the response
        // deadline. The frame-clock form -- max(4 frames, 133 ms) -- is a later
        // slice; chrono alone is correct at >= 30 fps and errs toward polling
        // LESS often below it, which is the safe direction.
        constexpr auto kTickInterval = std::chrono::milliseconds(133);

        inline std::chrono::steady_clock::time_point g_lastTick{};

        // Round-robin position, remembered by IDENTITY not index (#31).
        // Followers::Refresh() rebuilds g_active every diagnostics wake and
        // re-appends held-miss entries at the BACK, so a bare index silently
        // services one follower twice and skips another whenever the list
        // reorders -- worst in exactly the large parties where response time
        // is already at its ceiling.
        inline RE::FormID g_lastServiced = 0;

        // Actuation state, not evaluator state: a record of what has already
        // been done to the world. The evaluator itself stays stateless between
        // ticks (INVARIANTS #22).
        struct Recent {
            std::chrono::steady_clock::time_point until{};
            int         firedRule = -1;     // which rule INDEX bought the quiet
            // #11: the fired rule's IDENTITY (runtime uid), so a board Del/Move
            // during the window re-anchors the positional suppress to the rule's
            // NEW index instead of suppressing whatever slid into the old slot.
            // 0 = unassigned -> fall back to firedRule's index.
            std::uint32_t firedUID = 0;
            int         failRule  = -1;     // last failure reported, for #22j
            std::string failReason;
            std::string lastChain;          // last skip-chain line logged (1.4 dedup)
        };
        inline std::unordered_map<RE::FormID, Recent> g_recent;

        // FLAIR #3 -- combat-entry READY BEAT. First combat service stamps the
        // entry; the gambit table holds for ~200-350 ms (temperament-jittered)
        // so no follower acts with inhuman instantaneity, and the party stops
        // opening in unison. Cleared where retreat notes are cleared (combat
        // end), so it re-arms per fight. Wall-clock, not ticks: large parties
        // (already serviced every N x 133 ms) never stack extra delay.
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_combatEnteredAt;
        // T#76: consecutive out-of-combat services, to debounce the force-hold
        // release against an IsInCombat flap (SEV-2, Fable 2026-08-17).
        inline std::unordered_map<RE::FormID, int> g_outOfCombatTicks;
        // T#76 HYSTERESIS (marth: "hysteresis makes it seem human"): the last tick
        // the equip clamp was actively TRUE. The clamp releases only after a
        // per-follower dwell of SUSTAINED known-false, so a foe bobbing at the
        // gambit's range threshold does not re-sheathe the follower every tick.
        // Entry (cast->melee when a foe closes) stays prompt -- this gates only
        // the RELEASE. Cleared on combat end / death / revert.
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_meleeClampTrueAt;

        // FLAIR #5 -- RETARGET HESITATION. A target SWITCH of an existing latch
        // must win twice before it commits: the first winning service stores
        // the proposal and reports NoOp "sizing up <name>"; the second service
        // that proposes the SAME foe commits. First engagement of a fight is
        // exempt (the latch map is empty -- cleared on combat end by Rapport's
        // CombatSink), and so are the foe_attacking_me selectors: self-defense
        // stays snappy. Keyed like g_recent; cleared on combat end.
        inline std::unordered_map<RE::FormID, RE::ActorHandle> g_proposedTarget;

        // #11 (#23 combat-sense throttle): last-per-follower next-sample time.
        // Hoisted out of Tick's body so ClearTransientState can drop it on
        // revert -- a function-local static would leak stale FormIDs across a
        // load into a fresh world (the escaping-static pattern).
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_nextSense;

        inline std::atomic<double>        g_lastTickMs{ 0.0 };
        inline std::atomic<std::uint32_t> g_ticks{ 0 };

        // ── AUTO-RETREAT bookkeeping (ClickUp 86e3erv94, batch L) ───────────
        // The driver's per-follower state. A retreat runs TRAVEL (walk to the
        // player) -> STAY (held at the player's side while the fight's
        // confidence is still under the floor) -> released; after a release a
        // COOLDOWN gates the next auto-fill (replacing the old one-per-fight
        // `tried`, which also burned on a DECLINED fill). The note is NOT erased
        // by the party-OOC teardown: the cooldown must survive the fight edge
        // the retreat's own StopCombat can cause, and the map holds at most one
        // small entry per follower seen. Cleared on revert (ClearTransientState).
        // Worker-only, no lock (#4).
        enum class RetreatPhase : std::uint8_t { None, Travel, Stay };
        struct RetreatNote {
            RetreatPhase phase = RetreatPhase::None;
            // Whether the retreat package was ever his current package -- lets
            // the timeout line tell "controller never yielded" from "took but
            // too slow".
            bool took = false;
            // Out of combat observed since the last posted StopCombat (his own
            // flag read false on a service, or the main thread reported the
            // StopCombat landed). The NEXT in-combat read is then a RE-ENTRY and
            // gets exactly one more StopCombat (#22a: never per tick).
            bool outOfCombatSeen = false;
            // Confidence::Of() as last read while he WAS in combat. Out of
            // combat Of() drops the combat/foe multiplier -- and MFO's own
            // StopCombat is what took him out -- so an out-of-combat read is not
            // the fight's confidence. Read only at ARRIVAL: at or above the
            // floor he is released there instead of entering STAY.
            float fightConf = 1.0f;
            // An act.flee hold adopted while he was OUT of combat: release on
            // arrival, no STAY (the shipped flee behaviour).
            bool skipStay = false;
            std::uint32_t reentries = 0;
            std::chrono::steady_clock::time_point stayAt{};
            // STAY start on the UNPAUSED service clock (g_serviceClock).
            double stayClockAt = 0.0;
            // COOLDOWN: the next auto-fill waits until BOTH floors pass.
            std::chrono::steady_clock::time_point rearmAt{};
            std::uint32_t rearmLaps = 0;   // own services still to wait
            // P2: when the retreat trigger first held off for the follower's own cast in
            // flight (epoch == not holding). Bounds the delay, see kRetreatCastDeferMs.
            std::chrono::steady_clock::time_point castDeferSince{};
            // Retreat trigger persistence: the unpaused service clock when confidence first read
            // below the floor on a run of consecutive own services (< 0 = not below), and the
            // number of such samples. The fill needs kRetreatBelowSecs of it AND >= 2 samples.
            double        belowSince   = -1.0;
            std::uint32_t belowSamples = 0;
            // FLEE HOLD (marth 2026-11-09, Q20): the unpaused service clock when the full-health
            // release first found the engine's own Flee package still current (< 0 = not holding).
            double fleeHoldAt = -1.0;
        };
        inline std::unordered_map<RE::FormID, RetreatNote> g_retreatNotes;
        // [retreat-heal] line dedup (3 s per follower).
        inline std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_retreatHealLog;

        // UNPAUSED SERVICE CLOCK (seconds). Advanced at the per-follower pause
        // gate in Tick by the time since the previous advance, ONLY when the
        // game is not paused, and each step capped at kServiceClockMaxStep: a
        // gap longer than that means the pump did not run (a menu, a load, the
        // pause itself), not gameplay. 4 x 133 ms covers AddTask jitter and a
        // dead / null round-robin slot skipping a service. Worker-only (#4);
        // reset on revert. STAY is timed on it (marth: "about ten seconds").
        inline double g_serviceClock = 0.0;
        inline std::chrono::steady_clock::time_point g_serviceClockAt{};
        constexpr double kServiceClockMaxStep = 4 * 0.133;

        // T#78: followers we have already swept on their MFO-OFF transition. The
        // gate below skips every disabled follower each tick, but the one-time
        // release (Followers::ReleaseHeldState) must fire ONCE per OFF edge, not
        // every tick -- membership here is that latch. Erased the moment a
        // follower is MFO-enabled again, so a later OFF re-arms the release.
        // Worker-only, no lock (#4), same as every map above.
        inline std::unordered_set<RE::FormID> g_mfoDisabledSwept;

        // PARTY COMBAT -- the substrate the two combat gates key on (Deck
        // 2026-09-21, Fable): a follower's OWN IsInCombat() is not the truth of
        // whether the party is fighting. In a dragon fight Jesper's flag read
        // false most of the time while Cicero and Adelinda fought, so he sat on
        // the OOC table (only the heal rule could act -- "frozen except heals"),
        // and the same flap ran Cicero through the OOC debounce THREE times
        // mid-fight, force-unequipping his held weapon each time. So:
        //   PARTY COMBAT = the player is in combat
        //              OR any managed active follower is in combat
        //              OR any managed follower's combat-group foe tally
        //                 (CombatSense::FoeCount, the [sense] foes= read) > 0
        //                 -- read only for followers whose own flag is TRUE this
        //                 lap (controller lifetime, see the block in Tick), so
        //                 the third term is implied by the second and survives
        //                 as the transition line's foes= count.
        // Computed ONCE per pump tick at the top of Tick() on the worker, from
        // the same g_active walk the round-robin already makes (#4: worker-only,
        // no lock, no off-road g_followers read), then read by the per-follower
        // tick. Gate 1: the combat table runs while party combat is TRUE, own
        // flag or not (rules that need the follower's own combat state -- the
        // engine's combat group / target -- fall through transparently when he
        // has none: the Evaluator's foe selectors read his own group, so with no
        // controller they simply match nothing). Gate 2: the OOC debounce that
        // releases the forced hold counts party-OOC services, so an own-flag
        // flap inside a party fight never tears the hold down. Not save-scoped
        // (recomputed every tick) but reset on revert so a stale ON cannot log
        // a phantom OFF transition in the next world.
        inline bool g_partyCombat = false;
        // g_serviceClock when party combat last went OFF (-1e300 = never). Out-of-combat casts that open the
        // Harbinger substrate wait kOocCastDebounceSecs after it (field 2026-11: an ooc-auto substrate open
        // 0 ms after "party combat OFF" while foes were still around became an anchor leak). Reset on revert.
        inline double g_partyOffAt = -1.0e300;
        inline bool   g_partyOffDeferLogged = false;   // the deferral is logged once per window
        // 5 s: the service clock advances once per pump lap (kPumpMs 133) and the combat edge is sampled every
        // lap, so the window is ~37 laps, longer than the 1.4 s the field showed party combat to flap back ON.
        inline constexpr double kOocCastDebounceSecs = 5.0;
        // Once-per-fight latch for the "party combat, own combat=0 -- combat
        // table live" line: inserted on the first own-OOC party-combat service,
        // erased with the other per-fight state in the party-OOC branch.
        inline std::unordered_set<RE::FormID> g_partyCombatNoted;
        // COMBAT SUBSTRATE (Harbinger ABI v26, ClickUp 86e3pu939; marth 2026-10-13: "Certainly not. It only exists
        // to facilitate actions"): the followers whose combat is a Harbinger substrate this lap. They are NOT party
        // combat. One line when a follower enters that state and one when he leaves it. Worker-only, like the rest.
        inline std::unordered_set<RE::FormID> g_substrateNoted;
        inline std::unordered_set<RE::FormID> g_anchorLeakNoted;   // one ERROR per anchor-leak episode (worker thread only)
        // POST-FIGHT STALL PROBE (field 2026-09-29), PASSIVE: no behaviour reads it.
        // Followers stood 10-20 s after fights while the PLAYER stayed combat-
        // flagged (a Bandit Chief no follower engaged) and party combat was ON.
        // While party combat is ON and a follower's OWN combat is OFF, one
        // [stall-probe] line per follower every kStallProbeS: how far and how fast
        // they moved since the last sample (position deltas, no engine call), their
        // running package and its procedure type, and the player's combat flag and
        // combat target. For the next field session to name the mechanism. The
        // sample is dropped when they fight again or the party fight ends.
        struct StallSample {
            std::chrono::steady_clock::time_point at{};
            RE::NiPoint3                          pos{};
        };
        inline std::unordered_map<RE::FormID, StallSample> g_stallProbe;
        constexpr float kStallProbeS = 2.0f;
        // Once-per-fight latch for the "[eval] ... combat table: N rules, none
        // matched" line (2026-09-21, Deck/Fable): a follower whose every
        // condition is false has an EMPTY skip-chain, and the no-action chain
        // line prints only a non-empty chain -- so Jesper's table ran a whole
        // dragon fight with no [eval] line at all and STATUS's "[eval] scan
        // lines" promise was wrong. Inserted at the first all-false no-action
        // exit, erased with g_partyCombatNoted in the party-OOC branch.
        inline std::unordered_set<RE::FormID> g_noneMatchedNoted;
        // MFO-B55: followers whose last lap read the equip range rule as
        // UNDECIDABLE (null target, foes remain) -- one log line per fight.
        // Erased in the party-OOC branch with the other per-fight latches;
        // cleared on revert.
        inline std::unordered_set<RE::FormID> g_equipRangeUndecidableNoted;
        // fix/mfo-unreachable-flyer: melee-only followers HOLDING by the player because
        // every foe of theirs is airborne out of reach (ReachHoldRadius below). Worker-
        // only like its neighbours; erased on the fight's end (party-OOC branch) and on
        // revert. No expiry: the hold ends only when the scan says so (principle 9).
        inline std::unordered_set<RE::FormID> g_reachHold;

        // The dials. Fill: confidence below 0.25 (a follower who by the
        // leash tenet WANTS to be at the player's side) while >400u away from
        // the player -- far enough that arrival is an unambiguous pull, not
        // drift. Arrival: 200u (package radius 150 + engine stop slack).
        // Timeout: 30 s -- past any plausible walk time at Run speed.
        constexpr float kRetreatConfidence = 0.25f;
        // P2: the longest the fill waits for the follower's OWN cast in flight, from the first
        // delay. OwnCastInFlight counts caster states 1-7: a heal took about 560 ms from state 1 to 6
        // (E5 458 ms, E6 568 ms to 4), an offense charge about 0.7-1.0 s from state 1 to 4. 1500 ms
        // covers both with margin over one 133 ms pump. A floor on the cast, not an expiry on the
        // retreat: past it the fill goes ahead, loudly (WARN).
        constexpr std::uint32_t kRetreatCastDeferMs = 1500;
        // ENGAGE-ON-SIGHT BAR (Confidence v2 review, SEV-3): starting a fight needs a
        // margin OVER the retreat floor, so a follower never starts a fight he would
        // flee at the first damage (the v2 HP trend drops Of() as soon as he is hit).
        // With the v2 multiplier, at full health this admits ~5 even joiners (v1's
        // 0.25 floor admitted 4; v2 at 0.25 would admit ~10). ONE definition, passed
        // to EngageOnSight::Service like the retreat floor was.
        constexpr float kEngageConfidence = kRetreatConfidence + 0.15f;
        // The retreat fires only after confidence has stayed under the floor for this long on
        // consecutive own services (not one sample). Samples come once per OWN service, every
        // N x 133 ms (N = active roster), so at N > 11 two samples already span more than this;
        // hence also >= 2 samples.
        constexpr double kRetreatBelowSecs = 1.5;
        constexpr float kRetreatMinDist    = 400.0f;
        constexpr float kRetreatArriveDist = 200.0f;
        constexpr float kRetreatTimeout    = 30.0f;
        // STAY (marth 2026-09-25: "about ten seconds, or until fully healed",
        // whichever comes FIRST). After a follower has fallen back to the player,
        // STAY holds him there so he does not walk straight back out to the
        // fight. It ends on the FIRST of:
        //   - kRetreatStaySecs (10 s) of UNPAUSED service time (g_serviceClock,
        //     advanced only on unpaused Ticks -- a pause or menu never spends
        //     it; this is also the pause fix for STAY, MFO-B108);
        //   - his health reaching full: Vocab::HealthPct >= Vocab::kHealFull
        //     (99.95%), the codebase's one "full HP" mark -- HealthPct is
        //     current / (permanent + temporary max) and rarely equals exactly
        //     1.0, so a strict >= 1.0 could never end it;
        //   - ANY in-combat read ("engaged at your side"): he is released at once
        //     with NO StopCombat -- STAY never leaves him defenceless; if the
        //     fight comes to him he fights beside the player (review of 7580bea,
        //     SEV-2).
        // A follower whose last in-combat confidence (fightConf) is already at
        // the floor at arrival gets no STAY at all (released on arrival).
        constexpr double kRetreatStaySecs = 10.0;
        // FLEE HOLD (marth 2026-11-09, Q20): the full-health release waits while the engine's OWN
        // Flee package (PACKAGE_PROCEDURE_TYPE::kFlee) is still his current package, then releases.
        // Bound on the UNPAUSED service clock: he is sampled once per OWN service (N x 133 ms,
        // N = active roster; ~1.3 s at N = 10), so 20 s is >= 15 samples at N = 10 and still
        // several at large rosters. Hitting it is logged as a WARN (never a silent release).
        constexpr double kRetreatFleeHoldSecs = 20.0;
        // NO LIVE FOES (review of 7580bea, SEV-3 threading carve-out): judged
        // from the MAIN-THREAD foe probe (Packages::RetreatPostFoeProbe), posted
        // at most once per own service while he retreats, so its cadence is his
        // service period N x 133 ms (N = active roster). The retreat ends when
        // the last kRetreatNoFoeProbes LANDED probes all saw no engaged hostile
        // AND none has been seen for kRetreatNoFoeSecs. 3 probes = the same
        // two-consecutive-reads-plus-one rule as the party-OOC debounce and the
        // cooldown; 3 s is a wall floor over it (the probe count alone is 0.8 s
        // at N=2) -- ~20x the measured 145 ms engine combat-controller restart
        // (21:57:34.884/35.029), during which a foe's currentCombatTarget can
        // read empty, and one [sense] sampling period. The laps floor dominates
        // from N = 3 s / (3 x 133 ms) ~ 8 followers up.
        constexpr std::uint32_t kRetreatNoFoeProbes = 3;
        constexpr float         kRetreatNoFoeSecs   = 3.0f;
        // COOLDOWN (principle 9 -- a FLOOR, sized from the real cadences, not
        // an expiry): after a release, or a DECLINED fill, the next auto-fill
        // waits until BOTH
        //   (a) kRetreatCooldownLaps of HIS OWN services have passed. The
        //       round-robin services one follower per 133 ms tick, so his own
        //       refresh period is N x 133 ms (N = active roster, summons
        //       included). 3 laps = the releasing lap + one lap on which the
        //       release is first observable (the APMF Release nudge / legacy
        //       evaluate and the engine's re-engage land between laps) + one
        //       lap confirming it, the same two-consecutive-reads rule as the
        //       party-OOC debounce. At N=4 that is 1.6 s, at N=8 3.2 s -- it
        //       scales with the party instead of silently shrinking to one
        //       sample for a big one; and
        //   (b) kRetreatCooldownSecs of wall time. The measured field retreats
        //       arrived in 2.0-8.7 s (7 auto-retreats, deck logs 2026-09-21..24,
        //       scratchpad assess-confidence-leash.md); 10 s is just over the
        //       longest, so a released follower gets at least one measured
        //       retreat-walk's worth of time back in the fight before the next
        //       pull -- the fill cannot yo-yo him faster than one walk each way.
        //       (b) dominates up to N = 10 s / (3 x 133 ms) ~ 25 followers.
        // A declined fill pays the same cooldown so a refusing road (APMF
        // refusal, legacy alias busy) retries within the fight at a bounded log
        // rate instead of every service.
        constexpr std::uint32_t kRetreatCooldownLaps = 3;
        constexpr float         kRetreatCooldownSecs = 10.0f;

    // Defined in native/Scheduler.cpp, called by Scheduler::Tick (scheduler/CombatScan.cpp).
    void StallProbe(RE::Actor* a_f, RE::FormID a_id);
    void DeadTgtEnd(RE::FormID a_id, const char* a_why, RE::FormID a_now);
    void DeadTgtService(RE::Actor* a_f, RE::FormID a_id, bool a_inCombat);
    void StartRetreatCooldown(RetreatNote& a_note, std::chrono::steady_clock::time_point a_now);
    bool ServiceRetreat(RE::Actor* a_f, RE::FormID a_id, float a_dPlayer, bool a_havePlayer);
    void ServiceLeashWithReachHold(RE::Actor* a_f, RE::FormID a_id);

}
