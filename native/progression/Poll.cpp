// progression/Poll.cpp -- THE LEVEL POLL: the per-follower activity + economy
// pass (PollWork), its self-chaining MainThread tick (PollTick, which also
// drives the board-view refresh) and the Unmanaged refusal.
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"   // the split family's shared substrate
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

namespace MFO::ProgAllocator {

    namespace {

        constexpr int kViewFrames = 30;   // ~500ms at 60fps while the board is open

        // ── activity + economy (the poll body) ──────────────────────────────

    }

        // UNENROLLED = T#78 toggle unchecked: "unmanage, don't touch" (marth).
        // Every actor-touching path refuses; ProgState stays whole (still
        // leveled, still saved) so re-checking resumes + grants the back debt.
        bool Unmanaged(RE::Actor* a_actor, const char* a_verb) {
            if (!a_actor || Followers::IsMfoEnabled(a_actor->GetFormID())) return false;
            spdlog::info("[prog] {} refused {}: MFO is unchecked for this follower (unmanaged)", a_verb, NameOf(a_actor));
            return true;
        }

    namespace {

        void PollWork() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            const auto pl = player->GetLevel();
            if (g_lastPlayerLevel == 0) g_lastPlayerLevel = pl;   // first observation, no retro-grant
            const int d = static_cast<int>(pl) - static_cast<int>(g_lastPlayerLevel);
            if (d != 0) {
                spdlog::info("[prog] player level {} -> {} ({} enrolled record(s) to advance)",
                             g_lastPlayerLevel, pl, g_prog.size());
                g_lastPlayerLevel = pl;
            }

            // §HMS Phase 3: the player's LIVE per-level HMS gain (modlist-agnostic,
            // "whatever the player actually gains") — the catch-up rate granted to
            // caught-up fixed-stat followers this level. Measured once on a player
            // level-up, off the player's own base H/M/S total.
            const bool playerLeveled = (d > 0);
            float playerTotalNow = 0.0f, playerGain = 0.0f;
            if (playerLeveled) {
                for (int p = 0; p < 3; ++p) playerTotalNow += Followers::GetFollowerHMS(player, p);
                if (g_playerHmsTotalLast <= 0.0f) {
                    g_playerHmsTotalLast = playerTotalNow;   // first observation → no retro grant
                } else {
                    playerGain           = std::max(0.0f, playerTotalNow - g_playerHmsTotalLast);
                    g_playerHmsTotalLast = playerTotalNow;
                }
            } else if (g_playerHmsTotalLast <= 0.0f && !g_prog.empty()) {   // v8 parity: seed BEFORE the 1st level-up
                float t = 0.0f;   // else the 1st level-up credits 0 and withholds a whole award
                for (int p = 0; p < 3; ++p) t += Followers::GetFollowerHMS(player, p);
                g_playerHmsTotalLast = t;
            }
            if (playerGain > 0.0f)
                spdlog::info("[hms-parity] player HMS gain {:.1f} -> credited to every enrolled follower", playerGain);

            for (auto& [id, st] : g_prog) {
                if (!st.enrolled) continue;
                st.hmsParityCredit += playerGain;   // PRGN v8: the player-rate cap RecomputeHMS spends

                RE::Actor* actor   = RE::TESForm::LookupByID<RE::Actor>(id);
                const bool managed = Followers::IsMfoEnabled(id);

                if (st.clsId != 0) {
                    const bool active = IsActiveFollower(id);
                    const int  lag    = std::max(0, static_cast<int>(pl) - static_cast<int>(st.progressionLevel));
                    int gain = 0;
                    if (!g_econ.sharedGrowthEnabled) {
                        // Shared Growth OFF (§15): everyone matches the
                        // player's level outright — close any lag now.
                        gain = lag;
                        st.sharedGrowthRemainder = 0;
                    } else if (active) {
                        // Active + ON: earn at the player's own RATE (the
                        // delta), never an instant catch-up — a bench-lag is
                        // the PRICE of Shared Growth's half rate, and
                        // re-recruiting must not refund it. The banked
                        // remainder keeps for the next bench spell.
                        gain = std::min(std::max(0, d), lag);
                    } else if (d > 0) {
                        // Benched + Shared Growth ON: bank player levels and
                        // convert at the divisor (§15: half rate by default).
                        st.sharedGrowthRemainder = static_cast<std::uint16_t>(st.sharedGrowthRemainder + d);
                        const int div = std::max(1, g_econ.sharedGrowthDivisor);
                        gain = st.sharedGrowthRemainder / div;
                        st.sharedGrowthRemainder = static_cast<std::uint16_t>(st.sharedGrowthRemainder % div);
                        gain = std::min(gain, std::max(0, static_cast<int>(pl) - static_cast<int>(st.progressionLevel)));
                    }
                    if (gain > 0) {
                        st.progressionLevel = static_cast<std::uint16_t>(st.progressionLevel + gain);
                        // §17: no grant — the derived pool follows the level.
                        spdlog::info("[prog] {:08X} level {} (+{}) — {} perk point(s) available "
                                     "(floor(level/{}) − {} spent)",
                                     id, st.progressionLevel, gain, PerkPointsAvailable(st),
                                     g_econ.levelsPerPerkPoint, AllocatedRanks(st));
                        if (actor && managed) RecomputeSkills(actor, st, /*log*/ true);
                        if (actor && managed) RecomputeHMS(actor, st, /*log*/ true);   // §HMS
                    }
                }

                if (actor && !managed) {
                    // UNMANAGED: levels still accrue (back debt), the actor is not
                    // touched; re-arm reapply + strip for the re-check poll.
                    st.applied = false;
                    st.nativeHeld = false;
                } else if (actor) {
                    // B′: an unstripped follower (v6 save / benched / re-enrolled)
                    // is stripped the first ACTIVE poll, BEFORE the reapply (re-armed
                    // when anything was removed); never restored while enrolled.
                    if (!st.nativeHeld && IsActiveFollower(id))
                        if (auto* base = actor->GetActorBase())
                            if (StripNativePerks(actor, base, st) > 0) st.applied = false;
                    if (!st.applied) {
                        ReapplyFollower(actor, st);   // lazy: runs once the actor resolves
                    } else if (st.clsId != 0 && IsActiveFollower(id)) {
                        // Drift watch on the active party: an engine recompute
                        // (level-up autocalc, another mod's write) is adopted
                        // and re-topped by the reconcile. Writes only on
                        // divergence, so the steady state is pure reads.
                        RecomputeSkills(actor, st, /*log*/ true);
                        // §HMS: the drift-watch is the PRIMARY measure site —
                        // it MEASURES the engine's positive HMS drift (the award)
                        // and redistributes it, then holds target. Between awards
                        // base==target so this is pure reads. Battle counting
                        // (for the skew) runs every poll on the active party.
                        HmsTrackBattle(actor, st);
                        // §HMS Phase 3 fixed-stat DETECTION + GRANT (active party
                        // only — a benched follower isn't measured, so it must not
                        // be judged). Evaluated on a player level-up: the award
                        // tallied over the window that just closed decides 0-award.
                        float grantBudget = 0.0f;
                        if (playerLeveled) {
                            const bool gotAward = (st.hmsAwardAccum > 1e-4f);
                            st.hmsAwardAccum = 0.0f;   // close the window
                            if (gotAward) {
                                st.hmsZeroAwardStreak = 0;
                                st.fixedStat          = false;   // it's a leveling follower
                            } else {
                                if (st.hmsZeroAwardStreak < 2) ++st.hmsZeroAwardStreak;
                                if (st.hmsZeroAwardStreak >= 2) st.fixedStat = true;
                            }
                            if (st.fixedStat) {
                                // GATE: freeze until the player's total HMS catches up
                                // to this follower's baseline total, THEN converge the
                                // follower's TOTAL toward the player's total.
                                // v1.1 Phase-3 BACKFILL (marth 2026-08-26): the grant is
                                // NOT the per-level delta — it is the SHORTFALL to the
                                // target granted-HMS max(0, playerTotal - npcTotal). So
                                // an EXISTING fixed-stat follower jumps its owed HMS in
                                // ONE shot at activation (the backfill: playerTotal ->
                                // follower total), then tracks the player per-level as
                                // the target grows. hmsCumulative already holds the
                                // granted running total (no new co-save field);
                                // RecomputeHMS reshapes the budget by class ratio and
                                // carries the fractional remainder to whole points.
                                float npcTotal = 0.0f, grantedTotal = 0.0f;
                                for (int p = 0; p < 3; ++p) {
                                    npcTotal     += st.hmsBaseline[p];
                                    grantedTotal += st.hmsCumulative[p];
                                }
                                const bool caughtUp = (playerTotalNow >= npcTotal);
                                if (caughtUp)
                                    grantBudget = std::max(0.0f,
                                        (playerTotalNow - npcTotal) - grantedTotal);
                                spdlog::info("[hms] {:08X} fixed-stat grant: streak {} caughtUp {} "
                                             "npcTotal {:.0f} playerTotal {:.0f} playerGain {:.1f} "
                                             "granted {:.0f} -> backfill budget {:.1f}",
                                             id, st.hmsZeroAwardStreak, caughtUp, npcTotal,
                                             playerTotalNow, playerGain, grantedTotal, grantBudget);
                            }
                        }
                        RecomputeHMS(actor, st, /*log*/ true, grantBudget);
                    }
                }
            }
        }

    }

        void PollTick(int a_gen) {
            if (a_gen != g_pollGen) return;   // superseded by revert/reload
            if (--g_pollFrames <= 0) {
                g_pollFrames = kPollFrames;
                PollWork();
            }
            // Board-view refresh (component 3): while the board is open,
            // republish on the open edge, on a follower-focus change (the tab
            // switched who it is looking at — don't make the tree lag half a
            // second behind an L1/R1), and on the ~500ms cadence. Closed =
            // free (one atomic read + a bool).
            if (Board::IsOpen()) {
                const bool focusChanged = g_boardFocus.load() != g_lastPublishedFocus;
                if (!g_boardWasOpen || focusChanged || --g_viewFrames <= 0) {
                    g_viewFrames = kViewFrames;
                    PublishBoardViews();
                }
                g_boardWasOpen = true;
            } else {
                g_boardWasOpen = false;
            }
            MainThread::Post([a_gen]() { PollTick(a_gen); });
        }

}
