// apmf/RangeApproach.cpp -- H4, the CLOSING half: a ch.24 combat approach to the foe a mage casts at, bounded by the range of the spell in
// his hand (batch I item 1, Docs/research/2026-11/class-build-plan.md H4; Harbinger ABI v19 kIntent_CombatApproach, kApproach_Hold).
// MFO DERIVES the distance (iaus/Reposition.h SpellRange: touch / cone / concentration x0.8, aimed / target-actor the furthest working
// range x0.9, a self-area its own area x0.8) and DECLARES it: X = the foe, R = that distance. Harbinger carries the bound; the engine's own
// Return To Combat Area walks the follower inside R, and he keeps casting the whole way (composition, no package, no forced move).
// KEEPING BACK is not here: that is H3 (Concealment Kind::BackOff).
//
// WHO CALLS IT: scheduler/IausSource.cpp, once per IAUS lap of a follower in the spell phase, with the foe and the radius
// Reposition::RangeToHold derived. The call is the refresh: a claim nobody refreshes for kStaleMs is released by ServiceRangeClose.
// PRECEDENCE (one ch.24 claim per follower, every MFO client files at kOwnBasis, the EARLIER claim keeps the channel): a held-heal approach
// and a standing disengage drive outrank this (never filed while they stand); the offense approach (occlusion) and the give-ground step
// are released by the drive, not by us -- if one owns the channel, ours never applies, which ServiceRangeClose sees and releases.
// Threads: the pump's AddTask worker; its OWN leaf mutex, never g_mx and never nested with another approach mutex. Nothing is saved.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kRangeMinAbi = 19;                       // kIntent_CombatApproach + kApproach_Hold (OffenseApproach.cpp kMinAbi)
        constexpr float         kMinRadius   = 128.0f;                   // never below the approach's own tightening floor (a touch spell's standoff)
        constexpr float         kRepointFrac = 0.10f;                    // a new R this far from the filed one is a Repoint
        constexpr std::chrono::milliseconds kStaleMs{ 3500 };            // the offense approach's: three missed asks (about a second apart) = he stopped asking
        constexpr std::chrono::milliseconds kApplyMs{ 700 };             // a claim that never owned the channel after this is not going to (GroundApproach's)

        struct Claim {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            RE::FormID       foe = 0;
            float            radius = 0.0f;
            std::chrono::steady_clock::time_point filed{}, lastAsk{};
        };
        std::mutex g_rangeMx;
        std::unordered_map<RE::FormID, Claim> g_range;   // follower -> his standing close

        const APMF_API::APMF_API_v19* RangeApi() {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kRangeMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        const char* EndName(std::uint32_t a_state) {
            switch (a_state) {
            case APMF_API::kApproachState_Arrived:       return "arrived";
            case APMF_API::kApproachState_TargetLost:    return "the foe is gone";
            case APMF_API::kApproachState_CombatEnded:   return "combat ended";
            case APMF_API::kApproachState_EngineDropped: return "the engine dropped the bound (no path to the foe)";
            case APMF_API::kApproachState_Leashed:       return "leashed (the ch.23 leash excludes every point in R)";
            case APMF_API::kApproachState_ActorGone:     return "actor gone";
            case APMF_API::kApproachState_Released:      return "released";
            case APMF_API::kApproachState_Refused:       return "refused";
            default:                                     return "ended";
            }
        }
    }

    bool RangeCloseSupported() { return RangeApi() != nullptr; }

    void ServiceRangeClose(RE::FormID a_follower, RE::FormID a_foe, float a_radius, bool a_file) {
        const auto* api = RangeApi();
        if (!api || a_follower == 0 || a_foe == 0 || a_foe == a_follower) return;
        if (!std::isfinite(a_radius) || !(a_radius > 0.0f)) return;
        const float radius = (std::max)(kMinRadius, a_radius);
        const auto  now = std::chrono::steady_clock::now();
        // batch I F1: a held heal's approach is filed LATER than a standing close, and Harbinger breaks a same-basis tie for the earlier claim,
        // so the close would starve it for as long as it is refreshed. Release the close (outside our lock) and ask for nothing this lap.
        if (HealApproachStands(a_follower)) { ReleaseRangeClose(a_follower, "a held heal needs the ch.24 approach"); return; }
        {
            std::scoped_lock lk(g_rangeMx);
            if (const auto it = g_range.find(a_follower); it != g_range.end() && it->second.handle != APMF_API::kInvalidHandle) {
                Claim& c = it->second;
                c.lastAsk = now;
                if (c.foe == a_foe && std::fabs(c.radius - radius) <= kRepointFrac * c.radius) return;   // the claim stands as asked
                APMF_API::APMF_Param prm{};
                prm.target = a_foe; prm.fval = radius; prm.ival = APMF_API::kApproach_Hold;
                api->Repoint(c.handle, &prm);
                spdlog::info("[range] {:08X}: ch.24 close X {:08X} R {:.0f} -> X {:08X} R {:.0f} (Repoint, h={})", a_follower, c.foe, c.radius,
                             a_foe, radius, c.handle);
                c.foe = a_foe; c.radius = radius;
                return;
            }
        }
        if (!a_file) return;                              // nothing stands and the caller does not want one filed yet
        if (DisengageStands(a_follower)) return;          // so does a standing H3 drive
        APMF_API::APMF_Param prm{};
        prm.target = a_foe;
        prm.fval   = radius;
        prm.ival   = APMF_API::kApproach_Hold;
        const APMF_API::Handle h = api->RequestEx(a_follower, APMF_API::kIntent_CombatApproach, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::warn("[range] {:08X}: Harbinger refused the ch.24 close at the call (its log says why)", a_follower);
            return;
        }
        {
            std::scoped_lock lk(g_rangeMx);
            g_range[a_follower] = Claim{ h, a_foe, radius, now, now };
        }
        spdlog::info("[range] {:08X}: ch.24 close CLAIMED (h={}, X {:08X}, R {:.0f}, Hold): the spell in his hand works inside that range", a_follower,
                     h, a_foe, radius);
    }

    bool RangeCloseStands(RE::FormID a_follower) {
        std::scoped_lock lk(g_rangeMx);
        const auto it = g_range.find(a_follower);
        return it != g_range.end() && it->second.handle != APMF_API::kInvalidHandle;
    }

    void TickRangeClose(RE::FormID a_follower) {
        Claim s;
        {
            std::scoped_lock lk(g_rangeMx);
            const auto it = g_range.find(a_follower);
            if (it == g_range.end() || it->second.handle == APMF_API::kInvalidHandle) return;
            s = it->second;
        }
        const auto* api = RangeApi();
        const char* why = nullptr;
        const auto now = std::chrono::steady_clock::now();
        if (!api) {
            why = "Harbinger is gone";
        } else {
            APMF_API::APMF_CombatApproachInfo info{};
            info.size = sizeof(info);
            if (api->GetCombatApproachState) api->GetCombatApproachState(a_follower, &info);
            if (info.ownerHandle == s.handle && ((info.state >= APMF_API::kApproachState_Arrived && info.state <= APMF_API::kApproachState_Refused) ||
                                                 info.state == APMF_API::kApproachState_Leashed))
                why = EndName(info.state);   // batch I F4: Arrived and Leashed are END states (no zombie record); a live Hold reports Holding
            else if (now - s.filed > kApplyMs && info.ownerHandle != s.handle)
                why = "another ch.24 claim owns him (ours never applied)";
            else if (now - s.lastAsk > kStaleMs)
                why = "nobody asked for it for 3.5 s (the spell phase ended, or the foe is no longer the one he casts at)";
        }
        if (!why) return;
        ReleaseRangeClose(a_follower, why);
    }

    void ReleaseRangeClose(RE::FormID a_follower, const char* a_why) {
        Claim s;
        {
            std::scoped_lock lk(g_rangeMx);
            const auto it = g_range.find(a_follower);
            if (it == g_range.end()) return;
            s = it->second;
            g_range.erase(it);
        }
        if (s.handle == APMF_API::kInvalidHandle) return;
        if (const auto* api = g_apmf.load(std::memory_order_relaxed)) api->Release(s.handle);   // a no-op on a claim Harbinger already ended
        spdlog::info("[range] {:08X}: ch.24 close h={} released ({})", a_follower, s.handle, a_why ? a_why : "");
    }

    void ClearRangeCloses() {
        std::scoped_lock lk(g_rangeMx);
        g_range.clear();   // revert / load: Harbinger drops every claim of the outgoing world itself
    }
}
