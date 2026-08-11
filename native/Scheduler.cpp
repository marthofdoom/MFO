#include "PCH.h"
#include "Scheduler.h"
#include "Evaluator.h"
#include "Actuation.h"
#include "Followers.h"
#include "Config.h"
#include "Loadout.h"
#include "CasterConsent.h"
#include "CombatStyle.h"
#include "Packages.h"
#include "Vocabulary.h"
#include "Logistics.h"
#include "State.h"
#include "Confidence.h"   // retreat probe: the fill gate reads Of()
#include "Forms.h"        // retreat probe: g_retreatPackage for the onPkg readout
#include "Targeting.h"    // flair #5: retarget hesitation reads the current latch
#include "Temperament.h"  // flair #1: per-follower timing seed

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
            std::string lastChain;          // last skip-chain line logged (1.4 dedup)
        };
        std::unordered_map<RE::FormID, Recent> g_recent;

        // FLAIR #3 -- combat-entry READY BEAT. First combat service stamps the
        // entry; the gambit table holds for ~200-350 ms (temperament-jittered)
        // so no follower acts with inhuman instantaneity, and the party stops
        // opening in unison. Cleared where retreat notes are cleared (combat
        // end), so it re-arms per fight. Wall-clock, not ticks: large parties
        // (already serviced every N x 133 ms) never stack extra delay.
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_combatEnteredAt;

        // FLAIR #5 -- RETARGET HESITATION. A target SWITCH of an existing latch
        // must win twice before it commits: the first winning service stores
        // the proposal and reports NoOp "sizing up <name>"; the second service
        // that proposes the SAME foe commits. First engagement of a fight is
        // exempt (the latch map is empty -- cleared on combat end by Rapport's
        // CombatSink), and so are the foe_attacking_me selectors: self-defense
        // stays snappy. Keyed like g_recent; cleared on combat end.
        std::unordered_map<RE::FormID, RE::ActorHandle> g_proposedTarget;

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
        g_combatEnteredAt.clear();
        g_proposedTarget.clear();
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
        if (f->IsDead() || f->IsDisabled()) {
            // A follower who dies/leaves mid-fight never reaches the combat-exit
            // teardown below, so drop weapon-stance ownership here too: otherwise
            // AnyActive() stays true (every combatant pays the per-tick lookup)
            // and a resurrection re-owns the stale stance on the fresh
            // controller without an equip win. The live CSTY already died with
            // the controller. Idempotent erase-miss when unowned.
            CombatStyle::Clear(id);
            return;
        }

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
            g_combatEnteredAt.erase(id);   // flair #3: re-arm the ready beat
            g_proposedTarget.erase(id);    // flair #5: no proposal outlives a fight

            // v1.0.30: the cast-control latch dies with the fight. The [cast]
            // sink no longer clears it on a successful cast (the latch must
            // span cast cooldowns -- the between-casts leak), so combat end is
            // now an explicit release point. Without it, a latch from the last
            // fight lingers and denies the follower's own casting at the start
            // of the NEXT fight before his first service, wanting a spell no
            // rule may still name. Cheap and idempotent out of combat: no
            // combat caster runs the hook, and Clear on an unlatched id is an
            // uncontended erase-miss.
            CasterConsent::Clear(id);
            // Weapon-stance ownership dies with the fight too. The live CSTY
            // already reverted when the per-combat controller was destroyed;
            // this drops the stale bookkeeping so the next fight re-baselines.
            // Idempotent out of combat (uncontended erase-miss when unowned).
            CombatStyle::Clear(id);

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

        // COMBAT SENSE sample (#23): confidence now folds foe count in, so a
        // field test can watch a growing pack tighten the leash and, past a few
        // foes, arm auto-retreat. Purely observational; throttled ~3 s / follower.
        {
            static std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> s_nextSense;
            auto& nxt = s_nextSense[id];
            if (nxt.time_since_epoch().count() == 0 || now >= nxt) {
                nxt = now + std::chrono::seconds(3);
                spdlog::info("[sense] {:08X}: foes={} confidence={:.2f} leash={:.0f} chase={:.0f}",
                             id, CombatSense::FoeCount(f), Confidence::Of(f),
                             Confidence::LeashRadius(f), Confidence::ChaseRadius(f));
            }
        }

        // #63 QUASH ALLY COMBAT -- tick backstop. The TESCombatEvent quash
        // (Rapport) catches a follower ENTERING combat against a teammate; but if
        // BOTH were already fighting foes when friendly fire crossed them, no
        // enter-event fires for the crossfire. So each combat tick, if the serviced
        // follower is actively targeting a player TEAMMATE, end it on both sides.
        // StopCombat here matches the auto-retreat's worker-side StopCombat below.
        if (Config::g_quashAllyCombat.load()) {
            auto tp = f->GetActorRuntimeData().currentCombatTarget.get();
            if (auto* tgt = tp.get(); tgt && tgt != f && tgt->IsPlayerTeammate()) {
                f->StopCombat();
                tgt->StopCombat();
                spdlog::info("[peace] {:08X} targeting teammate {:08X} -- quashed (StopCombat both)",
                             id, tgt->GetFormID());
            }
        }

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
                // Re-disengage each tick: hostiles re-initiate combat, and while she
                // holds a live target the combat behaviour out-competes the
                // kIgnoreCombat travel (Auri: took=true but never moved). StopCombat
                // every tick keeps the travel winning until she reaches you. Cheap;
                // a no-op when she is not in combat.
                f->StopCombat();
                // Measure HER movement, not dPlayer -- dPlayer also closes when the
                // PLAYER walks toward her (the Auri false-positive that made a
                // stuck follower look like a successful retreat).
                const float moved = f->GetPosition().GetDistance(Packages::RetreatStartPos());

                if (pc && dPlayer <= kRetreatArriveDist) {
                    spdlog::info("[retreat] {:08X}: reached player after {:.1f}s (moved {:.0f})", id, secs, moved);
                    Packages::RetreatClear("arrived", f);
                } else if (secs > kRetreatTimeout) {
                    // Transition-only: one line when the fall-back gives up. moved
                    // tells "she walked but too slow" (large) from "controller never
                    // yielded" (near 0).
                    spdlog::info("[retreat] {:08X}: gave up after {:.1f}s (took={}, moved={:.0f}, dPlayer={:.0f})",
                                  id, secs, note.took, moved, dPlayer);
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
                    spdlog::info("[retreat] {:08X}: falling back -- confidence={:.2f} dPlayer={:.0f}",
                                  id, Confidence::Of(f), dPlayer);
                }
            }
        }

        if (it->second.combat().empty()) {
            // #65: no combat gambits, but a class override is still a valid STYLE
            // pick -- apply it (same master-toggle gating as the main stance path
            // below) before this early-out, so "just make this follower Ranged/
            // Mage" works even with an empty rule table. Auto (0) does nothing --
            // the #64 custom-follower guarantee holds on the empty path too.
            if (const std::uint8_t ov = it->second.combatClassOverride; ov != 0) {
                const auto forced = static_cast<CombatStyle::Stance>(ov);
                if (forced == CombatStyle::Stance::Cast) {
                    if (Config::g_castControl.load() > 0) CombatStyle::Want(id, forced);
                } else if (Config::g_weaponStyleControl.load()) {
                    CombatStyle::Want(id, forced);
                }
            }
            return;      // no rules -> nothing else to run
        }

        // ── FLAIR #3: COMBAT-ENTRY READY BEAT ────────────────────────────────
        // The first serviced tick of a fight stamps the entry; the table holds
        // for one human beat (~200-350 ms by temperament) before the opening
        // action. Once per combat entry -- never delays mid-fight reactions --
        // and silent: no engine call, no log, exactly a slightly-later opening.
        {
            const auto beat = g_combatEnteredAt.try_emplace(id, now).first;
            const auto hold = std::chrono::milliseconds(
                200 + static_cast<int>(150.0f * Temperament(id)));
            if (now < beat->second + hold) {
                g_lastTickMs = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - t0).count();
                return;
            }
        }

        // ── THE COMBAT SCAN (GAMBIT_FLOWS H1 + H2 + H3) ──────────────────────
        // Mirrors the logistics fall-through (Logistics.cpp): try matching
        // rules in order until one produces a REAL outcome. A rule whose
        // outcome is TRANSPARENT -- satisfied equip, unaffordable cast, empty
        // potion stack, cast cooldown, 2H debounce -- must not shadow the
        // rules below it: that shadow is why a spellsword never fell back to
        // steel and a follower died holding a heal below an empty drink rule
        // (D1/D2). Still AT MOST ONE real action per tick (§4.3): transparent
        // outcomes are by definition non-mutating, and the scan stops cold on
        // the first Fired or opaque hold (act.wait, a latched attack, the
        // cast-grace wait -- those legitimately occupy the tick).
        auto& recent = g_recent[id];
        const bool inWindow = now < recent.until;

        bool castSeen   = false;   // H3: some cast rule's condition held this tick
        int  handClaim  = 0;       // H2: 0 = none, 1 = melee, 2 = ranged
        int  wantStance = 0;       // combat-style ownership: the equip stance that won
        bool stopped    = false;   // scan ended on a Fired / opaque hold
        bool suppressed = false;   // scan stopped at the suppression window
        std::string chain;         // 1.4: skip-chain, e.g. "0(no potion),2(satisfied)"
        Eval::Choice choice;
        Actuation::Outcome outcome;

        for (int start = 0; ; ) {
            // Re-find the record each pass (INVARIANTS #2): a transparent
            // outcome may still have dispatched engine events (a failing cast
            // self-releases the spell), so never hold an iterator across one.
            const auto rec = g_followers.find(id);
            if (rec == g_followers.end()) break;

            choice = Eval::Evaluate(f, rec->second, Table::Combat, start);
            if (choice.ruleIndex < 0) break;          // nothing (more) matched

            const auto& op = choice.actionOpcode;
            const bool isCast  = op == Vocab::kActCastSelf || op == Vocab::kActCastTarget ||
                                 op == Vocab::kActCastPlayer;
            const bool isEquip = op == Vocab::kActEquipMelee || op == Vocab::kActEquipRanged;

            // SUPPRESSION IS POSITIONAL, NEVER ABSOLUTE (INVARIANTS #26) --
            // and it survives the scan unchanged: during a window the scan
            // stops COLD when the next candidate reaches the fired rule's
            // index; it never scans past it. Rules ABOVE the fired rule are
            // still tried first (transparent ones fall through as usual), so a
            // dying follower's rule-1 heal preempts exactly as before.
            if (inWindow && choice.ruleIndex >= recent.firedRule) {
                if (isCast) castSeen = true;   // its condition held: keep the loan (H3)
                suppressed = true;
                break;
            }

            // H2 -- THE HAND CLAIM. A satisfied equip rule above has already
            // decided this tick's weapon category; a lower CONTRADICTORY equip
            // is skipped WITHOUT firing, whatever its condition says. This is
            // first-match-wins applied to the hand as a resource -- without it
            // the fall-through would re-manufacture the melee<->ranged thrash
            // in mixed packs (D4). Per marth (§7.2) the claim binds only
            // equip-vs-equip: a cast rule below still fires (its off-hand
            // borrow is a loan, not a contradiction -- spellsword lists work).
            // Per-scan state only; nothing persists across ticks (#22).
            if (isEquip && handClaim != 0) {
                const int cat = (op == Vocab::kActEquipRanged) ? 2 : 1;
                if (cat != handClaim) {
                    chain += std::format("{}{}(hand claimed)",
                                         chain.empty() ? "" : ",", choice.ruleIndex);
                    start = choice.ruleIndex + 1;
                    continue;
                }
            }

            // H3: the condition of a cast rule held this tick -- whether it
            // now fires, waits out its grace, sits debounced, or fails (the
            // failure paths self-release internally). Only a tick where NO
            // cast condition held releases the loan, after the loop.
            if (isCast) castSeen = true;

            // ── FLAIR #5: RETARGET HESITATION ────────────────────────────────
            // A switch of an existing latch must win twice: first winning
            // service proposes and holds ("sizing up <name>" -- an opaque
            // beat, like the attack latch itself); the second service that
            // proposes the same foe commits. First engagement is exempt (no
            // latch yet), and so is self-defense (foe_attacking_me*).
            if (op == Vocab::kActAttack && choice.target) {
                const auto cur = Targeting::Current(id);
                // Only a LIVE current target earns the hesitation beat. A stale
                // latch to a DEAD/gone foe must not stall a switch (Opus review:
                // an oscillating winner + a dead latch would leave the follower
                // stuck on a corpse, never re-engaging) -- resolve it, and if it
                // is not a living actor, commit the switch immediately below.
                auto curPtr = cur.get();
                auto* curActor = curPtr.get();
                const bool curLive = curActor && !curActor->IsDead() && !curActor->IsDisabled();
                if (curLive && !(cur == choice.target)) {
                    bool exempt = false;
                    if (choice.ruleIndex < static_cast<int>(rec->second.combat().size())) {
                        const auto& cop = rec->second.combat()[choice.ruleIndex].conditionOpcode;
                        exempt = cop == Vocab::kCondFoeAttackingMe ||
                                 cop == Vocab::kCondFoeAttackingMeMelee ||
                                 cop == Vocab::kCondFoeAttackingMeRanged;
                    }
                    if (!exempt) {
                        auto& prop = g_proposedTarget[id];
                        if (!(prop == choice.target)) {
                            prop = choice.target;
                            auto tp = choice.target.get();
                            outcome = { Actuation::Result::NoOp,
                                        std::format("sizing up {}",
                                                    tp && tp->GetName() ? tp->GetName() : "?") };
                            stopped = true;   // opaque: consumes the tick, once
                            break;
                        }
                        g_proposedTarget.erase(id);   // won twice -> commit below
                    }
                } else {
                    g_proposedTarget.erase(id);       // first latch / same foe
                }
            }

            outcome = Actuation::Fire(f, choice);

            if (outcome.transparent) {
                // A satisfied equip (transparent NoOp on an equip action) is
                // the H2 claim; every other transparent outcome just records
                // its reason in the chain and the scan moves on.
                const bool satisfiedEquip =
                    isEquip && outcome.result == Actuation::Result::NoOp;
                if (satisfiedEquip) handClaim = wantStance = (op == Vocab::kActEquipRanged) ? 2 : 1;
                chain += std::format("{}{}({})", chain.empty() ? "" : ",",
                                     choice.ruleIndex,
                                     satisfiedEquip ? "satisfied" : outcome.reason.c_str());
                start = choice.ruleIndex + 1;
                continue;
            }

            // A FIRED equip is the winning stance too (the satisfied case above
            // handles an already-correct hand; this is the real swap).
            if (isEquip && outcome.result == Actuation::Result::Fired)
                wantStance = (op == Vocab::kActEquipRanged) ? 2 : 1;

            stopped = true;                    // Fired or opaque hold -> done
            break;
        }

        // COMBAT-STYLE OWNERSHIP. The actual combatStyle write happens on the
        // combat thread (CombatStyle::ApplyTick from the UpdateCombat hook); this
        // job-worker tick only records the desired stance. PRIORITY:
        //   1. An equip gambit that claimed the hand this tick -> weapon stance
        //      (v1.0.33), gated by bWeaponStyleControl. Held until battle end or
        //      the next equip flips it.
        //   2. Otherwise, a follower a CAST gambit owns (latched) -> the CASTER
        //      stance (mage update), gated by iCastControl>0 -- swapped to pure
        //      MELEE when he cannot afford the gambit spell (magicka dry), back
        //      to caster when it regenerates (marth: cast-till-dry-then-melee).
        // Only UPDATE on a real signal; a one-tick heal above must not drop the
        // stance (the "hold until it changes" contract), so no Want otherwise.
        // #65: the per-follower COMBAT-CLASS OVERRIDE, if the user set one,
        // SUPERSEDES the gambit-inferred stance below entirely -- an explicit
        // "make him Melee" wins over whatever the equip/cast gambits decided
        // this tick. Re-find the record (same INVARIANTS #2 discipline as
        // `rec` above: g_followers could have shifted under the scan). Auto
        // (0) changes NOTHING -- falls straight through to the unchanged
        // gambit-driven logic, the #64 custom-follower guarantee.
        std::uint8_t classOverride = 0;
        if (const auto fit = g_followers.find(id); fit != g_followers.end())
            classOverride = fit->second.combatClassOverride;

        CombatStyle::Stance stance = CombatStyle::Stance::None;
        if (classOverride != 0) {
            const auto forced = static_cast<CombatStyle::Stance>(classOverride);
            // Same master-toggle gating the inferred paths use below: the
            // override is a forced PICK, not a bypass of the kill-switches --
            // bWeaponStyleControl off still means MFO never touches weapon
            // stances, iCastControl==0 still means it never touches the
            // caster stance, override or not.
            if (forced == CombatStyle::Stance::Cast) {
                if (Config::g_castControl.load() > 0) stance = forced;
            } else if (Config::g_weaponStyleControl.load()) {
                stance = forced;
            }
        } else if (wantStance) {
            if (Config::g_weaponStyleControl.load())
                stance = (wantStance == 2) ? CombatStyle::Stance::Ranged
                                           : CombatStyle::Stance::Melee;
        } else if (Config::g_castControl.load() > 0) {
            if (const RE::FormID cs = CasterConsent::WantedSpell(id)) {
                float cost = 0.0f;
                if (auto* sp = RE::TESForm::LookupByID<RE::SpellItem>(cs))
                    cost = sp->CalculateMagickaCost(f);   // marth: cast till DRY, no reserve floor
                const float mag = f->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
                if (mag >= cost) {
                    stance = CombatStyle::Stance::Cast;
                } else if (Config::g_weaponStyleControl.load()) {
                    // Magicka dry -> pure melee until it regenerates. This writes
                    // MFO_MeleeStyle, so it honours the weapon-style kill-switch:
                    // with it off, leave the stance to the AI (None -> no update).
                    stance = CombatStyle::Stance::Melee;
                }
            }
        }
        if (stance != CombatStyle::Stance::None)
            CombatStyle::Want(id, stance);

        // H3 -- SCAN-AWARE SPELL RELEASE (the frequency limiter, fall-through
        // aware). A gambit spell stays in the follower's hand only while some
        // cast rule still wants it -- because their AI casts what they hold,
        // and MFO controls what they hold. Under the scan, "wants it" means
        // the rule's CONDITION held this tick (fired, in grace, debounced, or
        // suppressed-in-window) even when a lower rule ended up acting --
        // releasing on "the final winner is not a cast" would yank the spell
        // mid-grace every time a skipped cast let a lower rule run (D6).
        //
        // v1.0.32: the CONSENT latch is deliberately NOT released here any
        // more. H3 fires on a SINGLE service tick where no cast condition
        // held -- but the engine's combat casters tick many times inside one
        // service interval, so a momentary condition flicker (foe HP hovering
        // on a threshold, a target swap) dropped the deny for a whole
        // service-to-service gap and the AI slipped its OWN spell in through
        // it -- the sub-tick leak v1.0.30's cooldown-spanning latch still
        // had. Once a cast rule has been winning, exclusive control now holds
        // for the COMBAT'S DURATION; the release points are combat end (the
        // non-combat branch above, so it can never outlive the fight),
        // dismissal (Followers::Refresh), and revert/load (ClearTransient-
        // State). The SPELL still releases on H3 -- hand occupancy is pacing,
        // and their AI cannot cast what they are not holding either way.
        if (!castSeen) Loadout::ReleaseSpell(id);

        const char* name = f->GetName() ? f->GetName() : "?";   // flair #11

        if (!stopped) {
            // §4.4: nothing acted -> NO ENGINE CALL was made. Either no rule
            // matched, every match was transparent, or the scan reached the
            // suppression window. 1.4: keep the fall-through legible with ONE
            // skip-chain line, dedup'd on the whole chain (#22j) so a stable
            // situation logs once, not at 7.5 Hz.
            if (!chain.empty()) {
                std::string line = std::format("rules {} skipped -> {}", chain,
                                               suppressed ? "suppressed (window)"
                                                          : "no action");
                if (recent.lastChain != line) {
                    recent.lastChain = std::move(line);
                    spdlog::info("[eval] {} ({:08X}) {}", name, id, recent.lastChain);
                }
            }
            g_lastTickMs = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - t0).count();
            return;
        }

        // Re-find rather than reuse an iterator: Fire() dispatches engine
        // events synchronously, and INVARIANTS #2 says re-find live records by
        // key at act time instead of holding one across an engine call.
        if (auto rec = g_followers.find(id); rec != g_followers.end()) {
            if (choice.ruleIndex < static_cast<int>(rec->second.combat().size())) {
                // Display only -- the evaluator never reads these back (#22).
                auto& rule = rec->second.combat()[choice.ruleIndex];
                rule.lastFired = (outcome.result == Actuation::Result::Fired);
                rule.lastFailReason = outcome.reason;
                if (rule.lastFired) rule.lastFiredAt = now;   // flair #9: board pulse
            }
        }

        switch (outcome.result) {
        case Actuation::Result::Fired:
            // Suppress only on a REAL action. A wait or a failed cast must not
            // buy quiet time, or a follower who cannot afford a heal would go
            // silent instead of falling through on the next tick.
            //
            // FLAIR #2 -- the window is TEMPO'd per follower (+-12% by the
            // temperament seed) so a shared trigger no longer opens and closes
            // the whole party's windows in lockstep. The MCM knob stays the
            // center; the [eval] timestamps keep the actual cadence readable.
            recent.until = now + std::chrono::milliseconds(static_cast<int>(
                Config::g_suppressWindow.load() * 1000.0f *
                (0.88f + 0.24f * Temperament(id))));
            recent.firedRule = choice.ruleIndex;
            recent.failRule  = -1;
            recent.failReason.clear();
            recent.lastChain.clear();
            // 1.4 / Decision 3: when the fire came through a skip chain, ONE
            // line carries the whole story; otherwise the classic fired line.
            if (chain.empty()) {
                spdlog::info("[eval] {} ({:08X}) fired rule {} ({})",
                             name, id, choice.ruleIndex, choice.actionOpcode);
            } else {
                spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} fired ({})",
                             name, id, chain, choice.ruleIndex, choice.actionOpcode);
            }
            break;

        case Actuation::Result::FailedSkill:
        case Actuation::Result::FailedOther:
            // §5.3: say WHY -- but ON TRANSITION ONLY. A permanently failing
            // rule is the WINNING rule every tick (failures correctly do not
            // suppress), so logging it unconditionally means ~7.5 lines/sec
            // per follower, each with a synchronous flush on the main thread:
            // both a frame cost and a flood that drowns the signal (#22j).
            // (Post-H1, failures are transparent and rarely terminate the
            // scan; this case remains for any opaque wall left by design.)
            if (recent.failRule != choice.ruleIndex || recent.failReason != outcome.reason) {
                recent.failRule   = choice.ruleIndex;
                recent.failReason = outcome.reason;
                if (chain.empty()) {
                    spdlog::info("[eval] {} ({:08X}) rule {} ({}) did NOT fire: {}",
                                 name, id, choice.ruleIndex, choice.actionOpcode, outcome.reason);
                } else {
                    spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} ({}) did NOT fire: {}",
                                 name, id, chain, choice.ruleIndex, choice.actionOpcode, outcome.reason);
                }
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
                if (chain.empty()) {
                    spdlog::info("[eval] {} ({:08X}) rule {} ({}) held off: {}",
                                 name, id, choice.ruleIndex, choice.actionOpcode, outcome.reason);
                } else {
                    spdlog::info("[eval] {} ({:08X}) rules {} skipped -> rule {} ({}) held off: {}",
                                 name, id, chain, choice.ruleIndex, choice.actionOpcode, outcome.reason);
                }
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
