// apmf/CombatApproach.cpp -- the ch.24 HELD-HEAL APPROACH (ABI v19, kIntent_CombatApproach):
// MFO's client side of Harbinger's "Moving an NPC toward someone in a fight"
// (Harbinger Docs/INTEGRATION.md). ClickUp 86e3h871c. marth 2026-09-30: "If anything, instead
// of a cap it should do loots moveto towards the actor." The shape mirrors apmf/PursuitLeash.cpp.
//
// WHAT MFO DECLARES: X = the heal's RECIPIENT, R = Config::g_healApproachRadius
// (fHealApproachRadius), ival = kApproach_Hold (stay close until MFO releases). Harbinger owns ONLY
// the in-combat movement facet: the follower's combat area is bounded to (X, R), so the engine's own
// Return-To-Combat-Area behaviour walks him toward X while attacks, casts on both hands, target
// choice and blocking keep running. The held heal itself is unchanged: it stays held (no cap), the
// claim on the hand stands and the engine fires it when sight returns.
//
// WHO CALLS IT: cast/HealRoad.cpp NoteLosHold files the claim when the held heal's own-ray verdict is
// Occluded on two agreeing reads; it is released when sight returns, when the heal on that hand ends
// (HealHandEnded), on retreat start / combat end / dismissal / MFO off (Scheduler, Followers), and on
// load / revert (ClearTransientState). ONE claim per follower: a second hand's occluded recipient does
// not file a second claim; the same hand's NEW recipient Repoints.
//
// LOOP GUARD: a claim Harbinger ENDS with EngineDropped / TargetLost / Refused, one it reports
// Leashed (MFO's ch.23 leash excludes every point within R of X -- logged, never fought) or one that
// never goes live is released and its recipient is blocked for the rest of the fight (the sweep reads
// GetCombatApproachState for the reason; the block clears when the fight ends). Harbinger absent /
// older than v19 / the seat refused = no approach: exactly as before.
// Threads: the pump's AddTask worker for everything but ClearHealApproaches (the drained-pump load
// window); its OWN mutex, never g_mx (same lock-order note as apmf/PursuitLeash.cpp).
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Packages.h"   // IsRetreating -- the sweep's retreat backstop

#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kApproachMinAbi = 19;   // the ABI that first serves intent 24

        // Same never-live floor as the sibling tables (see apmf/ReentryDeny.cpp, PursuitLeash.cpp).
        constexpr std::uint32_t kApproachNeverLiveSweeps = 15;
        // Heartbeat while a claim is Approaching / Holding: distance falling is the proof.
        constexpr std::chrono::milliseconds kApproachBeatMs{ 1500 };

        struct ApproachClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;
            std::size_t      hand           = 0;      // the hand whose held heal filed it
            RE::FormID       recipient      = 0;      // X as last sent
            float            radius         = 0.0f;
            std::uint32_t    unpausedSweeps = 0;
            bool             everLive       = false;
            std::uint32_t    lastSeq        = 0;      // GetCombatApproachState's seq at the last log
            std::chrono::steady_clock::time_point lastBeat{};
        };
        std::mutex                                                          g_approachMx;
        std::unordered_map<RE::FormID, ApproachClaim>                       g_approach;   // guarded by g_approachMx
        // follower -> recipients Harbinger gave up on / refused / never served THIS fight.
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>      g_approachBlocked;   // guarded by g_approachMx

        std::atomic<bool> g_approachSeatRefused{ false };   // session-stable, like the ch.21 / ch.22 / ch.23 tables

        const APMF_API::APMF_API_v19* ApproachApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kApproachMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        bool ApproachGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        const char* StateWord(std::uint32_t a_state) {
            switch (a_state) {
            case APMF_API::kApproachState_None:          return "NONE";
            case APMF_API::kApproachState_Waiting:       return "WAITING (not in combat)";
            case APMF_API::kApproachState_Approaching:   return "APPROACHING";
            case APMF_API::kApproachState_Holding:       return "HOLDING";
            case APMF_API::kApproachState_Yielded:       return "YIELDED (a HOLD POSITION package owns the area)";
            case APMF_API::kApproachState_Disabled:      return "DISABLED (fleeing / not high process)";
            case APMF_API::kApproachState_Leashed:       return "LEASHED (the ch.23 leash excludes every point in R)";
            case APMF_API::kApproachState_Arrived:       return "ARRIVED";
            case APMF_API::kApproachState_TargetLost:    return "TARGET LOST";
            case APMF_API::kApproachState_CombatEnded:   return "COMBAT ENDED";
            case APMF_API::kApproachState_EngineDropped: return "ENGINE DROPPED (its own path to X failed)";
            case APMF_API::kApproachState_ActorGone:     return "ACTOR GONE";
            case APMF_API::kApproachState_Released:      return "RELEASED";
            case APMF_API::kApproachState_Refused:       return "REFUSED";
            default:                                     return "?";
            }
        }

        APMF_API::APMF_CombatApproachInfo ReadState(const APMF_API::APMF_API_v19* a_api, RE::FormID a_follower) {
            APMF_API::APMF_CombatApproachInfo info{};
            info.size = sizeof(info);
            if (a_api->GetCombatApproachState) a_api->GetCombatApproachState(a_follower, &info);
            return info;
        }

        // A reason that must not be answered by filing the same recipient again in this fight.
        bool BlocksRecipient(std::uint32_t a_state) {
            return a_state == APMF_API::kApproachState_EngineDropped ||
                   a_state == APMF_API::kApproachState_TargetLost ||
                   a_state == APMF_API::kApproachState_Leashed ||
                   a_state == APMF_API::kApproachState_Refused;
        }
    }

    void ServiceHealApproach(RE::FormID a_follower, std::size_t a_hand, RE::FormID a_recipient) {
        const auto* api = ApproachApi();
        if (!api || g_approachSeatRefused.load(std::memory_order_relaxed)) return;
        if (a_follower == 0 || a_recipient == 0 || a_recipient == a_follower) return;
        const float radius = Config::g_healApproachRadius.load();
        if (!(radius > 0.0f) || !std::isfinite(radius)) return;   // Harbinger refuses these; never send one

        std::scoped_lock lock(g_approachMx);
        if (const auto bit = g_approachBlocked.find(a_follower);
            bit != g_approachBlocked.end() && bit->second.count(a_recipient))
            return;   // the loop guard: Harbinger gave up on this recipient this fight
        const auto it = g_approach.find(a_follower);
        if (it == g_approach.end()) {
            APMF_API::APMF_Param prm{};
            prm.target = a_recipient;                 // X
            prm.fval   = radius;                      // R, game units
            prm.ival   = APMF_API::kApproach_Hold;    // stay close: MFO releases when sight returns
            const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_CombatApproach, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_approachSeatRefused.exchange(true)) {
                    spdlog::warn("[heal-approach] Harbinger REFUSED the ch.24 combat approach for {:08X} at the "
                                 "call: no held-heal approach this session (VR, a runtime other than 1.6.1170 / "
                                 "1.5.97 / 1.7.104, [CombatApproach] bCombatApproach=0 in APMF.ini, or its address "
                                 "self-check; APMF's '[ch.24]' line names it). The held heal still waits.",
                                 a_follower);
                }
                return;
            }
            ApproachClaim c{};
            c.handle    = h;
            c.hand      = a_hand;
            c.recipient = a_recipient;
            c.radius    = radius;
            g_approach.emplace(a_follower, c);
            spdlog::info("[heal-approach] {:08X}: ch.24 combat approach CLAIMED (h={}, X {:08X}, R {:.0f}, Hold) "
                         "-- the held heal on the {} hand stays held; he walks toward the recipient",
                         a_follower, h, a_recipient, radius, a_hand == 0 ? "LEFT" : "RIGHT");
            return;
        }
        auto& c = it->second;
        if (c.handle == APMF_API::kInvalidHandle) return;
        if (c.hand != a_hand) return;                 // ONE claim per follower: the first hand's recipient owns it
        if (c.recipient == a_recipient) return;       // nothing changed: no Repoint churn
        APMF_API::APMF_Param prm{};
        prm.target = a_recipient;
        prm.fval   = radius;
        prm.ival   = APMF_API::kApproach_Hold;
        api->Repoint(c.handle, &prm);
        spdlog::info("[heal-approach] {:08X}: ch.24 combat approach X {:08X} -> {:08X} (Repoint, h={}, R {:.0f})",
                     a_follower, c.recipient, a_recipient, c.handle, radius);
        c.recipient = a_recipient;
        c.radius    = radius;
        c.lastSeq   = 0;
    }

    void ReleaseHealApproach(RE::FormID a_follower, const char* a_why, std::size_t a_hand, bool a_fightOver) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        RE::FormID       x = 0;
        {
            std::scoped_lock lock(g_approachMx);
            if (a_fightOver) g_approachBlocked.erase(a_follower);
            const auto it = g_approach.find(a_follower);
            if (it == g_approach.end()) return;
            if (a_hand != kApproachAnyHand && it->second.hand != a_hand) return;   // another hand's claim stands
            h = it->second.handle;
            x = it->second.recipient;
            g_approach.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle)
            api->Release(h);   // no-op on an ended claim
        spdlog::info("[heal-approach] {:08X}: ch.24 combat approach to {:08X} released ({})", a_follower, x, a_why);
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_approachMx itself; g_mx NOT held.
    void SweepHealApproaches() {
        const auto* api = ApproachApi();
        if (!api) return;
        std::vector<RE::FormID> retreating;
        {
            std::scoped_lock lock(g_approachMx);
            if (g_approach.empty()) return;
            const bool paused = ApproachGamePaused();
            const auto now    = std::chrono::steady_clock::now();
            std::vector<RE::FormID> done;
            for (auto& [fid, c] : g_approach) {
                if (Packages::IsRetreating(fid)) { retreating.push_back(fid); continue; }
                if (c.handle == APMF_API::kInvalidHandle) continue;
                const bool live = api->IsClaimLive(c.handle);
                if (!live && !c.everLive && (paused || ++c.unpausedSweeps <= kApproachNeverLiveSweeps)) continue;
                const auto info = ReadState(api, fid);
                if (live) {
                    c.everLive = true;
                    if (info.state == APMF_API::kApproachState_Leashed) {
                        spdlog::info("[heal-approach] {:08X}: ch.24 approach to {:08X} is LEASHED by MFO's ch.23 "
                                     "pursuit leash (no point within R {:.0f} of him lies inside it) -- not "
                                     "fought; released and not re-filed for him this fight, the held heal waits",
                                     fid, c.recipient, c.radius);
                        g_approachBlocked[fid].insert(c.recipient);
                        api->Release(c.handle);
                        c.handle = APMF_API::kInvalidHandle;
                        done.push_back(fid);
                        continue;
                    }
                    if (info.seq != c.lastSeq || now - c.lastBeat >= kApproachBeatMs) {
                        if (info.state == APMF_API::kApproachState_Approaching ||
                            info.state == APMF_API::kApproachState_Holding ||
                            info.seq != c.lastSeq) {
                            spdlog::info("[heal-approach] {:08X}: ch.24 {} -> {:08X}: distance {:.0f} / R {:.0f} "
                                         "(engine {}, area carried the bound {} time(s))",
                                         fid, StateWord(info.state), c.recipient, info.distance, info.radius,
                                         info.engineInside == 2 ? "INSIDE" : info.engineInside == 1 ? "OUTSIDE" : "unseen",
                                         info.applied);
                        }
                        c.lastSeq  = info.seq;
                        c.lastBeat = now;
                    }
                    continue;
                }
                // Not live: Harbinger ended it (or it never went live).
                if (!c.everLive) {
                    spdlog::warn("[heal-approach] {:08X}: ch.24 approach to {:08X} NEVER WENT LIVE (h={}, {} unpaused "
                                 "sweeps) -- released; not re-filed for him this fight. APMF's log should say why.",
                                 fid, c.recipient, c.handle, c.unpausedSweeps);
                    g_approachBlocked[fid].insert(c.recipient);
                } else {
                    const bool block = BlocksRecipient(info.state);
                    spdlog::info("[heal-approach] {:08X}: ch.24 approach to {:08X} ENDED BY HARBINGER: {} (distance "
                                 "{:.0f} / R {:.0f}, h={}){}",
                                 fid, c.recipient, StateWord(info.state), info.distance, info.radius, c.handle,
                                 block ? " -- not re-filed for him this fight" : "");
                    if (block) g_approachBlocked[fid].insert(c.recipient);
                }
                api->Release(c.handle);
                c.handle = APMF_API::kInvalidHandle;
                done.push_back(fid);
            }
            for (const auto fid : done) g_approach.erase(fid);
        }
        for (const auto fid : retreating) ReleaseHealApproach(fid, "retreating", kApproachAnyHand, false);
    }

    void ClearHealApproaches() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_approachMx);
        for (auto& [fid, c] : g_approach)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_approach.clear();
        g_approachBlocked.clear();
    }

}
