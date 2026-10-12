// apmf/OffenseApproach.cpp -- the ch.24 OFFENSE APPROACH (ABI v19, kIntent_CombatApproach), ClickUp 86e3m5ff8
// (marth: LoreRim round). The offensive twin of apmf/CombatApproach.cpp's held-heal approach: when EVERY
// candidate foe of an offensive cast rule is measured OCCLUDED, the follower is moved toward the pinned foe
// so he gets sight, instead of standing where he is with the cast held.
//
// WHO CALLS IT: Evaluator.cpp's foe selector (PickFoe), in its all-occluded fallback, for a rule whose action
// needs sight (a_ownRay: a spell cast / ranged swing). That call is the refresh: the claim lives only while the
// evaluator keeps saying "all occluded". It is released when a sighted candidate returns (the same selector),
// when the pinned foe dies / is disabled, when the rule stops asking (no refresh for kStaleMs), on retreat /
// combat end / dismissal (Scheduler, Followers), and on load / revert (ClearTransientState).
// LINE OF SIGHT IS NEVER REUSED: the trigger is the selector's own fresh Sightline verdict each evaluation; this
// file keeps no sight state of its own.
//
// BOUNDS (the held-heal approach's, verbatim): X = the pinned foe, R = TightRadius (fHealApproachRadius, tightened
// toward kApproachMinRadius while Holding and still occluded), Repointed every kTightenGap; Harbinger's ch.23 leash
// clamp ends the claim as Leashed (logged, never fought, foe blocked this fight); at the floor with sight still lost
// for kFloorGiveUp the approach is BLOCKED (released, foe not re-filed this fight).
// PRECEDENCE: a held-heal approach on the same follower wins (ONE ch.24 claim per follower): this file never files
// while apmf/CombatApproach.cpp's table holds one, and ServiceHealApproach releases ours first.
// Threads: the pump's AddTask worker; its OWN mutex, never g_mx and never nested with g_approachMx.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Packages.h"   // IsRetreating

#include <algorithm>
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
        constexpr std::uint32_t kMinAbi = 19;
        constexpr std::uint32_t kNeverLiveSweeps = 15;
        constexpr std::chrono::milliseconds kBeatMs{ 5000 };
        // The evaluator re-asks about once a second while the rule matches; three missed asks = it stopped.
        constexpr std::chrono::milliseconds kStaleMs{ 3500 };
        constexpr float                 kMinRadius = 128.0f;
        constexpr float                 kTightenFrac = 0.6f;
        constexpr std::chrono::seconds  kTightenGap{ 3 };
        constexpr std::chrono::seconds  kFloorGiveUp{ 5 };
        constexpr std::chrono::seconds  kRefileGap{ 4 };

        struct Claim {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            RE::FormID       foe    = 0;
            float            radius = 0.0f;
            std::uint32_t    unpausedSweeps = 0;
            bool             everLive = false;
            std::uint32_t    lastSeq = 0;
            std::chrono::steady_clock::time_point lastAsk{};
            std::chrono::steady_clock::time_point lastTighten{};
            std::chrono::steady_clock::time_point floorSince{};
            std::chrono::steady_clock::time_point lastBeat{};
        };
        std::mutex                                                        g_offMx;
        std::unordered_map<RE::FormID, Claim>                             g_off;          // guarded by g_offMx
        std::unordered_map<RE::FormID, std::unordered_set<RE::FormID>>    g_offBlocked;   // follower -> foes given up on this fight
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_offRefileAfter;
        std::atomic<bool> g_offSeatRefused{ false };

        const APMF_API::APMF_API_v19* OffApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        float Tight(RE::FormID a_follower, RE::FormID a_foe, float a_radius) {
            const auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            const auto* r = RE::TESForm::LookupByID<RE::Actor>(a_foe);
            if (!f || !r) return a_radius;
            const float d = f->GetPosition().GetDistance(r->GetPosition());
            return std::min(a_radius, std::max(kMinRadius, kTightenFrac * d));
        }

        const char* Word(std::uint32_t a_state) {
            switch (a_state) {
            case APMF_API::kApproachState_Approaching:   return "APPROACHING";
            case APMF_API::kApproachState_Holding:       return "HOLDING";
            case APMF_API::kApproachState_Leashed:       return "LEASHED (the ch.23 leash excludes every point in R)";
            case APMF_API::kApproachState_Arrived:       return "ARRIVED";
            case APMF_API::kApproachState_TargetLost:    return "TARGET LOST";
            case APMF_API::kApproachState_CombatEnded:   return "COMBAT ENDED";
            case APMF_API::kApproachState_EngineDropped: return "ENGINE DROPPED (its own path to X failed)";
            case APMF_API::kApproachState_Refused:       return "REFUSED";
            default:                                     return "other";
            }
        }

        APMF_API::APMF_CombatApproachInfo Read(const APMF_API::APMF_API_v19* a_api, RE::FormID a_follower) {
            APMF_API::APMF_CombatApproachInfo info{};
            info.size = sizeof(info);
            if (a_api->GetCombatApproachState) a_api->GetCombatApproachState(a_follower, &info);
            return info;
        }

        bool IsEnd(std::uint32_t a_state) {
            return a_state >= APMF_API::kApproachState_Arrived && a_state <= APMF_API::kApproachState_Refused;
        }
        bool Blocks(std::uint32_t a_state) {
            return a_state == APMF_API::kApproachState_EngineDropped || a_state == APMF_API::kApproachState_TargetLost ||
                   a_state == APMF_API::kApproachState_Leashed || a_state == APMF_API::kApproachState_Refused;
        }
    }

    void ServiceOffenseApproach(RE::FormID a_follower, RE::FormID a_foe) {
        const auto* api = OffApi();
        if (!api || g_offSeatRefused.load(std::memory_order_relaxed)) return;
        if (a_follower == 0 || a_foe == 0 || a_foe == a_follower) return;
        const float radius = Config::g_healApproachRadius.load();
        if (!(radius > 0.0f) || !std::isfinite(radius)) return;
        if (HealApproachStands(a_follower)) return;   // taken BEFORE g_offMx: the held heal owns the one ch.24 claim

        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_offMx);
        auto it = g_off.find(a_follower);
        if (const auto bit = g_offBlocked.find(a_follower); bit != g_offBlocked.end() && bit->second.count(a_foe)) {
            if (it != g_off.end() && it->second.foe != a_foe) {
                const auto h = it->second.handle;
                const RE::FormID old = it->second.foe;
                g_off.erase(it);
                if (h != APMF_API::kInvalidHandle) api->Release(h);
                spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} released (the pinned foe is now {:08X}, "
                             "which Harbinger gave up on this fight)", a_follower, old, a_foe);
            }
            return;
        }
        if (it == g_off.end()) {
            if (const auto rit = g_offRefileAfter.find(a_follower); rit != g_offRefileAfter.end() && now < rit->second) return;
            APMF_API::APMF_Param prm{};
            prm.target = a_foe;
            prm.fval   = Tight(a_follower, a_foe, radius);
            prm.ival   = APMF_API::kApproach_Hold;
            const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_CombatApproach, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_offSeatRefused.exchange(true))
                    spdlog::warn("[offense-approach] Harbinger REFUSED the ch.24 combat approach for {:08X} at the call: "
                                 "no offense approach this session (see the '[heal-approach] ... REFUSED' reasons). "
                                 "The cast rule still waits for sight.", a_follower);
                return;
            }
            Claim c{};
            c.handle = h; c.foe = a_foe; c.radius = prm.fval; c.lastAsk = now; c.lastTighten = now;
            g_off.emplace(a_follower, c);
            spdlog::info("[offense-approach] {:08X}: ch.24 combat approach CLAIMED (h={}, X {:08X}, R {:.0f}, Hold) -- every "
                         "candidate foe is occluded; he walks toward the pinned foe to regain sight",
                         a_follower, h, a_foe, prm.fval);
            return;
        }
        auto& c = it->second;
        c.lastAsk = now;
        if (c.handle == APMF_API::kInvalidHandle) return;
        if (c.foe == a_foe) {
            if (now - c.lastTighten < kTightenGap) return;
            if (Read(api, a_follower).state != APMF_API::kApproachState_Holding) { c.floorSince = {}; return; }
            const float r2 = Tight(a_follower, a_foe, radius);
            if (r2 >= c.radius - 1.0f) {
                if (c.floorSince == std::chrono::steady_clock::time_point{}) { c.floorSince = now; return; }
                if (now - c.floorSince < kFloorGiveUp) return;
                spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} is BLOCKED at the floor (R {:.0f}, Holding, "
                             "sight still lost for {} s) -- released and not re-filed for him this fight",
                             a_follower, a_foe, c.radius,
                             std::chrono::duration_cast<std::chrono::seconds>(now - c.floorSince).count());
                g_offBlocked[a_follower].insert(a_foe);
                const auto h = c.handle;
                g_off.erase(it);
                api->Release(h);
                return;
            }
            c.floorSince = {};
            APMF_API::APMF_Param tp{};
            tp.target = a_foe; tp.fval = r2; tp.ival = APMF_API::kApproach_Hold;
            api->Repoint(c.handle, &tp);
            spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} TIGHTENED R {:.0f} -> {:.0f} (inside the bound "
                         "but still no sight)", a_follower, a_foe, c.radius, r2);
            c.radius = r2; c.lastTighten = now; c.lastSeq = 0;
            return;
        }
        APMF_API::APMF_Param prm{};
        prm.target = a_foe; prm.fval = Tight(a_follower, a_foe, radius); prm.ival = APMF_API::kApproach_Hold;
        api->Repoint(c.handle, &prm);
        spdlog::info("[offense-approach] {:08X}: ch.24 combat approach X {:08X} -> {:08X} (Repoint, h={}, R {:.0f})",
                     a_follower, c.foe, a_foe, c.handle, prm.fval);
        c.foe = a_foe; c.radius = prm.fval; c.lastTighten = now; c.floorSince = {}; c.lastSeq = 0;
    }

    void ReleaseOffenseApproach(RE::FormID a_follower, const char* a_why, bool a_fightOver) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        RE::FormID       x = 0;
        {
            std::scoped_lock lock(g_offMx);
            if (a_fightOver) { g_offBlocked.erase(a_follower); g_offRefileAfter.erase(a_follower); }
            const auto it = g_off.find(a_follower);
            if (it == g_off.end()) return;
            h = it->second.handle;
            x = it->second.foe;
            g_off.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        spdlog::info("[offense-approach] {:08X}: ch.24 combat approach to {:08X} released ({})", a_follower, x, a_why);
    }

    void SweepOffenseApproaches() {
        const auto* api = OffApi();
        if (!api) return;
        std::vector<std::pair<RE::FormID, const char*>> drop;   // follower, why: released below (outside g_offMx)
        {
            std::scoped_lock lock(g_offMx);
            if (g_off.empty()) return;
            auto* ui = RE::UI::GetSingleton();
            const bool paused = ui && ui->GameIsPaused();
            const auto now = std::chrono::steady_clock::now();
            std::vector<RE::FormID> done;
            for (auto& [fid, c] : g_off) {
                if (c.handle == APMF_API::kInvalidHandle) continue;
                if (Packages::IsRetreating(fid)) { drop.emplace_back(fid, "retreating"); continue; }
                if (!paused && now - c.lastAsk > kStaleMs) { drop.emplace_back(fid, "the rule stopped asking (no longer all-occluded)"); continue; }
                const auto* foe = RE::TESForm::LookupByID<RE::Actor>(c.foe);
                if (!foe || foe->IsDead() || foe->IsDisabled()) { drop.emplace_back(fid, "the pinned foe died / is gone"); continue; }
                const bool live = api->IsClaimLive(c.handle);
                if (!live && !c.everLive && (paused || ++c.unpausedSweeps <= kNeverLiveSweeps)) continue;
                const auto info = Read(api, fid);
                if (live) {
                    c.everLive = true;
                    if (info.ownerHandle == c.handle && info.state == APMF_API::kApproachState_Leashed) {
                        spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} is LEASHED by MFO's ch.23 pursuit leash "
                                     "(no point within R {:.0f} of him lies inside it) -- not fought; released and not re-filed "
                                     "for him this fight", fid, c.foe, c.radius);
                        g_offBlocked[fid].insert(c.foe);
                        api->Release(c.handle);
                        c.handle = APMF_API::kInvalidHandle;
                        done.push_back(fid);
                        continue;
                    }
                    if (info.seq != c.lastSeq || now - c.lastBeat >= kBeatMs) {
                        if (info.state == APMF_API::kApproachState_Approaching || info.state == APMF_API::kApproachState_Holding ||
                            info.seq != c.lastSeq)
                            spdlog::info("[offense-approach] {:08X}: ch.24 {} -> {:08X}: distance {:.0f} / R {:.0f}",
                                         fid, Word(info.state), c.foe, info.distance, info.radius);
                        c.lastSeq = info.seq;
                        c.lastBeat = now;
                    }
                    continue;
                }
                const bool ours = info.ownerHandle == c.handle;
                if (!c.everLive && !(ours && IsEnd(info.state))) {
                    spdlog::warn("[offense-approach] {:08X}: ch.24 approach to {:08X} NEVER WENT LIVE (h={}, {} unpaused sweeps) -- "
                                 "released; not re-filed for him this fight", fid, c.foe, c.handle, c.unpausedSweeps);
                    g_offBlocked[fid].insert(c.foe);
                } else {
                    const bool block = ours && Blocks(info.state);
                    if (ours && info.state == APMF_API::kApproachState_CombatEnded) g_offRefileAfter[fid] = now + kRefileGap;
                    spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} ENDED BY HARBINGER: {} (distance {:.0f} / R {:.0f}, "
                                 "h={}){}", fid, c.foe, Word(info.state), info.distance, info.radius, c.handle,
                                 block ? " -- not re-filed for him this fight" : "");
                    if (block) g_offBlocked[fid].insert(c.foe);
                }
                api->Release(c.handle);
                c.handle = APMF_API::kInvalidHandle;
                done.push_back(fid);
            }
            for (const auto fid : done) g_off.erase(fid);
        }
        for (const auto& [fid, why] : drop) ReleaseOffenseApproach(fid, why, false);
    }

    void ClearOffenseApproaches() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_offMx);
        for (auto& [fid, c] : g_off)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_off.clear();
        g_offBlocked.clear();
        g_offRefileAfter.clear();
    }

}
