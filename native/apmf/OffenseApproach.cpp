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
// LINE OF SIGHT IS NEVER REUSED: the trigger is the selector's own fresh Sightline verdict each evaluation. This
// file keeps only the DWELL clocks over those fresh samples (marth/Opus 2026-10-18, Auri vs a flying dragon: terrain
// flips a borderline line OCCLUDED/VISIBLE every 1-2 s, and one sample claimed then released the walk at once):
// a claim needs the all-occluded verdict to have held kSightDwellMs, and a release needs sight to have held
// kSightDwellMs, each clock reset by one opposite sample (NoteOffenseSight is the visible side).
//
// BOUNDS (the held-heal approach's, verbatim): X = the pinned foe, R = TightRadius (fHealApproachRadius, tightened
// toward kApproachMinRadius while Holding and still occluded), Repointed every kTightenGap; Harbinger's ch.23 leash
// clamp ends the claim as Leashed (logged, never fought, foe blocked this fight); at the floor with sight still lost
// for kFloorGiveUp the approach is BLOCKED (released, foe not re-filed this fight). EXCEPT ENGINE DROPPED (11-17f): a timed backoff, Tuning::offenseApproachBackoffSec.
// 11-17 field fixes: (b) the tightening floor is kMinRadius, not the standoff: while Holding and still occluded INSIDE the standoff the
// claim keeps tightening and stops at the first point with sight (the sight dwell releases it); the 15 s regular-movement release is the
// true floor only. (c) A block (g_offBlocked) is scoped: it lapses when the follower's leash radius has grown, or he has moved more than
// kBlockMoveUnits, since it was made (the ch.23 leash that blocked it was a verdict about THAT radius at THAT place).
// PRECEDENCE: a held-heal approach on the same follower wins (ONE ch.24 claim per follower): this file never files
// while apmf/CombatApproach.cpp's table holds one, and ServiceHealApproach releases ours first.
// Threads: the pump's AddTask worker; its OWN mutex, never g_mx and never nested with g_approachMx.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Confidence.h"   // LeashRadius: the block is scoped to the radius it was given up at
#include "iaus/Core.h"    // DefaultTuning: the engine-dropped backoff (offenseApproachBackoffSec)
#include "Packages.h"   // IsRetreating

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <string>
#include <tuple>
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
        // marth 2026-10-12: "Ignore spell range, always 1024" -- the offense approach never takes R below this and stops
        // approaching at it. "Unless its a touch spell": a Touch spell closes to kTouchStandoff, which is the approach's
        // own tightening floor (kMinRadius; no dedicated touch-reach constant exists in the code, NOT FOUND).
        constexpr float                 kOffenseStandoff = 1024.0f;
        constexpr float                 kMinRadius = 128.0f;
        constexpr float                 kTouchStandoff = kMinRadius;
        constexpr float                 kTightenFrac = 0.6f;
        constexpr std::chrono::seconds  kTightenGap{ 3 };
        constexpr std::chrono::seconds  kFloorGiveUp{ 5 };
        constexpr std::chrono::seconds  kRefileGap{ 4 };
        // Dwell over the selector's fresh LoS samples (Sightline re-measures a pair at most every 0.3 s and the
        // evaluator asks about once a lap, ~133 ms x party): 1.5 s is about five agreeing measurements, above one
        // flip of the 1-2 s terrain flicker seen in the field, below the 3.5 s kStaleMs.
        constexpr std::chrono::milliseconds kSightDwellMs{ 1500 };
        // marth 2026-10-12: inside the standoff, still occluded, nothing more the approach can do (no strafe): release
        // the claim so the engine's own combat movement runs, and do not re-claim for THAT follower+foe for this long.
        // NOT a fight-long block: after it, the normal flow may claim again.
        constexpr std::int64_t          kOffenseRegularMoveMs = 15000;
        // PLACEHOLDERS (11-17 A1c, the sim tunes them): a block made by a ch.23 leash verdict lapses (the foe may be filed again) when the
        // follower's leash radius has grown by more than kBlockLeashGrow, or he stands more than kBlockMoveUnits from where it was made.
        constexpr float                 kBlockLeashGrow = 150.0f;
        constexpr float                 kBlockMoveUnits = 300.0f;

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
        // A block and the world it was made in (A1c). armed = stamped (pos / leash filled after the sweep released g_offMx: LeashRadius takes the
        // combat group lock, never taken under g_offMx); an unstamped block stands until the fight ends, as every block did before.
        // leashed: made by the ch.23 LEASHED verdict, which depends on the PLAYER's and the FOE's positions and the leash radius (apmf/PursuitLeash.cpp),
        // not on where the follower stands: such a block lapses when either of them moved; any other block (never went live, Harbinger-ended) lapses
        // on the follower's own move.
        struct Blk { bool armed = false, leashed = false; float x = 0, y = 0, z = 0, leash = 0, px = 0, py = 0, pz = 0, fx = 0, fy = 0, fz = 0; };
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, Blk>> g_offBlocked;   // follower -> foes given up on this fight
        std::unordered_map<RE::FormID, std::chrono::steady_clock::time_point> g_offRefileAfter;
        std::unordered_map<RE::FormID, std::unordered_map<RE::FormID, int>> g_offDropCount;   // follower -> foe -> ENGINE DROPPED endings this fight (g_offMx)
        struct Regular { RE::FormID foe = 0; std::chrono::steady_clock::time_point until{}; };
        std::unordered_map<RE::FormID, Regular> g_offRegular;   // follower -> foe whose re-claim is suppressed; g_offMx
        // Dwell clocks (g_offMx): `since` = first sample of the current run of that verdict, `last` = the most recent one;
        // a run whose last sample is older than kStaleMs (the rule stopped asking) restarts.
        struct Dwell { std::chrono::steady_clock::time_point since{}, last{}; };
        std::unordered_map<RE::FormID, Dwell> g_offOccDwell;   // follower -> all-occluded run
        std::unordered_map<RE::FormID, Dwell> g_offVisDwell;   // follower -> sight run (only while a claim stands)
        std::atomic<bool> g_offSeatRefused{ false };

        // Extend (or restart) a run with a sample at a_now; returns how long the run has held.
        std::chrono::milliseconds NoteRun(std::unordered_map<RE::FormID, Dwell>& a_m, RE::FormID a_fid,
                                          std::chrono::steady_clock::time_point a_now) {
            auto& d = a_m[a_fid];
            if (d.since == std::chrono::steady_clock::time_point{} || a_now - d.last > kStaleMs) d.since = a_now;
            d.last = a_now;
            return std::chrono::duration_cast<std::chrono::milliseconds>(a_now - d.since);
        }

        const APMF_API::APMF_API_v19* OffApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        // a_tightening: a standing claim that is still occluded (A1b): its floor is kMinRadius, so inside the standoff it keeps closing. The FIRST
        // filing (and a repoint to another foe) is bounded by the standoff as before.
        float Tight(RE::FormID a_follower, RE::FormID a_foe, float a_radius, bool a_touch, bool a_tightening = false) {
            const auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            const auto* r = RE::TESForm::LookupByID<RE::Actor>(a_foe);
            if (!f || !r) return a_radius;
            const float d = f->GetPosition().GetDistance(r->GetPosition());
            const float standoff = a_tightening ? kMinRadius : (a_touch ? kTouchStandoff : kOffenseStandoff);
            return std::max(standoff, std::min(a_radius, std::max(kMinRadius, kTightenFrac * d)));
        }

        // Is a_foe still blocked for a_follower? Not called under g_offMx (reads LeashRadius). A stamped block lapses when the leash radius has
        // grown or the follower has moved (see the banner); the entry is erased and the lapse logged.
        bool BlockStands(RE::FormID a_follower, RE::FormID a_foe) {
            {
                std::scoped_lock lock(g_offMx);
                const auto bit = g_offBlocked.find(a_follower);
                if (bit == g_offBlocked.end()) return false;
                const auto fit = bit->second.find(a_foe);
                if (fit == bit->second.end()) return false;
                if (!fit->second.armed) return true;
            }
            auto* f = RE::TESForm::LookupByID<RE::Actor>(a_follower);
            if (!f) return true;
            const auto  pos   = f->GetPosition();
            const float leash = Confidence::LeashRadius(f);
            const auto* pc    = RE::PlayerCharacter::GetSingleton();
            const auto* foeA  = RE::TESForm::LookupByID<RE::Actor>(a_foe);
            const RE::NiPoint3 ppos = pc ? pc->GetPosition() : RE::NiPoint3{};
            const RE::NiPoint3 fpos = foeA ? foeA->GetPosition() : RE::NiPoint3{};
            std::scoped_lock lock(g_offMx);
            const auto bit = g_offBlocked.find(a_follower);
            if (bit == g_offBlocked.end()) return false;
            const auto fit = bit->second.find(a_foe);
            if (fit == bit->second.end()) return false;
            const Blk& b = fit->second;
            const float moved = pos.GetDistance(RE::NiPoint3{ b.x, b.y, b.z });
            const bool grown = leash > b.leash + kBlockLeashGrow;
            float lapseMove = moved;   // a non-leashed block: the follower's own move
            if (b.leashed) {           // a ch.23 verdict: the player or the foe moved
                const float pm = pc ? ppos.GetDistance(RE::NiPoint3{ b.px, b.py, b.pz }) : 0.0f;
                const float fm = foeA ? fpos.GetDistance(RE::NiPoint3{ b.fx, b.fy, b.fz }) : 0.0f;
                lapseMove = std::max(pm, fm);
            }
            if (!(b.leashed && grown) && !(lapseMove > kBlockMoveUnits)) return true;
            spdlog::info("[offense-approach] {:08X}: block on {:08X} lapsed ({}: leash {:.0f} -> {:.0f}, {} moved {:.0f} u) -- he may be filed again",
                         a_follower, a_foe, (b.leashed && grown) ? "the leash radius grew" : "things moved on", b.leash, leash,
                         b.leashed ? "player / foe" : "he", lapseMove);
            bit->second.erase(fit);
            return false;
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

    bool OffenseApproachStands(RE::FormID a_follower) {
        std::scoped_lock lock(g_offMx);
        const auto it = g_off.find(a_follower);
        return it != g_off.end() && it->second.handle != APMF_API::kInvalidHandle;
    }

    bool OffenseApproachCannotFile(RE::FormID a_follower, RE::FormID a_foe) {
        if (!OffApi() || g_offSeatRefused.load(std::memory_order_relaxed)) return true;
        if (BlockStands(a_follower, a_foe)) return true;
        // A pending refile gap (the ENGINE DROPPED backoff, combat ended, sight returned) means ServiceOffenseApproach returns early, so nothing would move
        // him if H4's range close were released for it: H4 keeps the channel until the gap is over (review F2).
        std::scoped_lock lock(g_offMx);
        const auto rit = g_offRefileAfter.find(a_follower);
        return rit != g_offRefileAfter.end() && std::chrono::steady_clock::now() < rit->second;
    }

    void ServiceOffenseApproach(RE::FormID a_follower, RE::FormID a_foe, bool a_touchSpell) {
        const float standoff = a_touchSpell ? kTouchStandoff : kOffenseStandoff;
        const char* standoffWhy = a_touchSpell ? "touch spell" : "ranged";
        const auto* api = OffApi();
        if (!api || g_offSeatRefused.load(std::memory_order_relaxed)) return;
        if (a_follower == 0 || a_foe == 0 || a_foe == a_follower) return;
        const float radius = Config::g_healApproachRadius.load();
        if (!(radius > 0.0f) || !std::isfinite(radius)) return;
        if (HealApproachStands(a_follower)) return;   // taken BEFORE g_offMx: the held heal owns the one ch.24 claim
        const bool blocked = BlockStands(a_follower, a_foe);   // BEFORE g_offMx: a stamped block reads the leash radius

        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_offMx);
        auto it = g_off.find(a_follower);
        g_offVisDwell.erase(a_follower);   // an occluded sample ends any run of sight
        if (const auto rg = g_offRegular.find(a_follower); rg != g_offRegular.end()) {
            if (rg->second.foe == a_foe && now < rg->second.until) return;   // regular movement window
            g_offRegular.erase(rg);
        }
        if (blocked) {
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
            // Not on one occluded sample: the verdict must have held (a visible sample clears the run).
            const auto occludedFor = NoteRun(g_offOccDwell, a_follower, now);
            if (occludedFor < kSightDwellMs) return;
            APMF_API::APMF_Param prm{};
            prm.target = a_foe;
            prm.fval   = Tight(a_follower, a_foe, radius, a_touchSpell);
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
            spdlog::info("[offense-approach] {:08X}: ch.24 combat approach CLAIMED (h={}, X {:08X}, R {:.0f}, Hold, standoff={:.0f} ({})) -- every "
                         "candidate foe has been occluded for {} ms; he walks toward the pinned foe to regain sight",
                         a_follower, h, a_foe, prm.fval, standoff, standoffWhy, occludedFor.count());
            return;
        }
        auto& c = it->second;
        c.lastAsk = now;
        if (c.handle == APMF_API::kInvalidHandle) return;
        if (c.foe == a_foe) {
            if (now - c.lastTighten < kTightenGap) return;
            if (Read(api, a_follower).state != APMF_API::kApproachState_Holding) { c.floorSince = {}; return; }
            const float r2 = Tight(a_follower, a_foe, radius, a_touchSpell, /*a_tightening=*/true);   // A1b: floor kMinRadius, not the standoff
            if (r2 >= c.radius - 1.0f) {
                if (c.floorSince == std::chrono::steady_clock::time_point{}) { c.floorSince = now; return; }
                if (now - c.floorSince < kFloorGiveUp) return;
                spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} at the tightening floor and still occluded: regular "
                             "movement for {} s (R {:.0f}, Holding, no sight for {} s; floor={:.0f}) -- released, not blocked",
                             a_follower, a_foe, kOffenseRegularMoveMs / 1000, c.radius,
                             std::chrono::duration_cast<std::chrono::seconds>(now - c.floorSince).count(), kMinRadius);
                g_offRegular[a_follower] = Regular{ a_foe, now + std::chrono::milliseconds(kOffenseRegularMoveMs) };
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
                         "but still no sight; floor={:.0f})", a_follower, a_foe, c.radius, r2, kMinRadius);
            c.radius = r2; c.lastTighten = now; c.lastSeq = 0;
            return;
        }
        APMF_API::APMF_Param prm{};
        prm.target = a_foe; prm.fval = Tight(a_follower, a_foe, radius, a_touchSpell); prm.ival = APMF_API::kApproach_Hold;
        api->Repoint(c.handle, &prm);
        spdlog::info("[offense-approach] {:08X}: ch.24 combat approach X {:08X} -> {:08X} (Repoint, h={}, R {:.0f}, standoff={:.0f} ({}))",
                     a_follower, c.foe, a_foe, c.handle, prm.fval, standoff, standoffWhy);
        c.foe = a_foe; c.radius = prm.fval; c.lastTighten = now; c.floorSince = {}; c.lastSeq = 0;
    }

    // A fresh VISIBLE (or Unknown, which passes) verdict from the selector. Ends the all-occluded run; with a claim
    // standing, releases it only once sight has held kSightDwellMs (one visible sample of a flickering line is not sight).
    void NoteOffenseSight(RE::FormID a_follower) {
        bool release = false;
        long long heldMs = 0;
        {
            std::scoped_lock lock(g_offMx);
            g_offOccDwell.erase(a_follower);
            if (g_off.find(a_follower) == g_off.end()) { g_offVisDwell.erase(a_follower); return; }
            const auto held = NoteRun(g_offVisDwell, a_follower, std::chrono::steady_clock::now());
            release = held >= kSightDwellMs;
            heldMs = held.count();
        }
        if (release) {
            const std::string why = "sight returned: a candidate foe has been in view for " + std::to_string(heldMs) + " ms";
            ReleaseOffenseApproach(a_follower, why.c_str(), false, false);
        }
    }

    void ReleaseOffenseApproach(RE::FormID a_follower, const char* a_why, bool a_fightOver, bool a_refileGap) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        RE::FormID       x = 0;
        {
            std::scoped_lock lock(g_offMx);
            g_offOccDwell.erase(a_follower);
            g_offVisDwell.erase(a_follower);
            if (a_fightOver) { g_offBlocked.erase(a_follower); g_offRefileAfter.erase(a_follower); g_offRegular.erase(a_follower); g_offDropCount.erase(a_follower); }
            const auto it = g_off.find(a_follower);
            if (it == g_off.end()) return;
            h = it->second.handle;
            x = it->second.foe;
            g_off.erase(it);
            // A "sight returned" release must not be undone next tick by the rule that still sees nothing (churn).
            if (a_refileGap) g_offRefileAfter[a_follower] = std::chrono::steady_clock::now() + kRefileGap;
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        spdlog::info("[offense-approach] {:08X}: ch.24 combat approach to {:08X} released ({})", a_follower, x, a_why);
    }

    void SweepOffenseApproaches() {
        const auto* api = OffApi();
        if (!api) return;
        std::vector<std::pair<RE::FormID, const char*>> drop;   // follower, why: released below (outside g_offMx)
        std::vector<std::tuple<RE::FormID, RE::FormID, bool>> stamp;   // follower, foe: blocks made this sweep, stamped below (A1c)
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
                        g_offBlocked[fid].try_emplace(c.foe); stamp.emplace_back(fid, c.foe, true);
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
                    g_offBlocked[fid].try_emplace(c.foe); stamp.emplace_back(fid, c.foe, false);
                } else {
                    // ENGINE DROPPED (its own path to X failed) is a BACKOFF, not a whole-fight block (field 11-17f, Jesper: one failed path
                    // ended his approach for the rest of a 50 s fight while the claim on the occluded foe held both hands). A path that fails
                    // from here may succeed from another spot or once the foe moves, so the foe is filed again after
                    // Tuning::offenseApproachBackoffSec (`t.offApproachBackoff`, placeholder). The other ended states keep their block.
                    // The backoff repeats at most Tuning::offenseApproachDropCap times per (follower, foe) per fight (review F2): past it the ending is a
                    // block as before, so a foe the engine can never path to does not cycle forever.
                    bool engineDropped = ours && info.state == APMF_API::kApproachState_EngineDropped;
                    if (engineDropped && ++g_offDropCount[fid][c.foe] > static_cast<int>(Iaus::DefaultTuning().offenseApproachDropCap)) engineDropped = false;
                    const bool block = ours && Blocks(info.state) && !engineDropped;
                    const float backoffSec = Iaus::DefaultTuning().offenseApproachBackoffSec;
                    if (ours && info.state == APMF_API::kApproachState_CombatEnded) g_offRefileAfter[fid] = now + kRefileGap;
                    if (engineDropped)
                        g_offRefileAfter[fid] = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                          std::chrono::duration<float>((std::max)(0.0f, backoffSec)));
                    spdlog::info("[offense-approach] {:08X}: ch.24 approach to {:08X} ENDED BY HARBINGER: {} (distance {:.0f} / R {:.0f}, "
                                 "h={}){}", fid, c.foe, Word(info.state), info.distance, info.radius, c.handle,
                                 block ? " -- not re-filed for him this fight"
                                       : engineDropped ? std::format(" -- backoff: filed again after {:.0f} s if it still waits on sight", backoffSec).c_str()
                                                       : "");
                    if (block) { g_offBlocked[fid].try_emplace(c.foe); stamp.emplace_back(fid, c.foe, info.state == APMF_API::kApproachState_Leashed); }
                }
                api->Release(c.handle);
                c.handle = APMF_API::kInvalidHandle;
                done.push_back(fid);
            }
            for (const auto fid : done) g_off.erase(fid);
        }
        for (const auto& [fid, why] : drop) ReleaseOffenseApproach(fid, why, false);
        // A1c: stamp each block with where and at what leash radius it was made. Read here, outside g_offMx (LeashRadius takes the combat group lock).
        for (const auto& [fid, foe, leashed] : stamp) {
            auto* f = RE::TESForm::LookupByID<RE::Actor>(fid);
            if (!f) continue;
            const auto  pos   = f->GetPosition();
            const float leash = Confidence::LeashRadius(f);
            std::scoped_lock lock(g_offMx);
            const auto bit = g_offBlocked.find(fid);
            if (bit == g_offBlocked.end()) continue;
            const auto fit = bit->second.find(foe);
            if (fit == bit->second.end()) continue;
            Blk nb; nb.armed = true; nb.leashed = leashed; nb.x = pos.x; nb.y = pos.y; nb.z = pos.z; nb.leash = leash;
            if (const auto* pc = RE::PlayerCharacter::GetSingleton()) { const auto pp = pc->GetPosition(); nb.px = pp.x; nb.py = pp.y; nb.pz = pp.z; }
            if (const auto* fa = RE::TESForm::LookupByID<RE::Actor>(foe)) { const auto fp = fa->GetPosition(); nb.fx = fp.x; nb.fy = fp.y; nb.fz = fp.z; }
            fit->second = nb;
        }
    }

    void ClearOffenseApproaches() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_offMx);
        for (auto& [fid, c] : g_off)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_off.clear();
        g_offBlocked.clear();
        g_offRefileAfter.clear();
        g_offDropCount.clear();
        g_offRegular.clear();
        g_offOccDwell.clear();
        g_offVisDwell.clear();
    }

}
