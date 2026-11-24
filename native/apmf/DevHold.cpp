// apmf/DevHold.cpp -- DEV BUILDS ONLY (CMake MFO_DEV_SELFTEST): the claim road of the G0b hold exerciser (movement-plan.md package G0b).
// ONE ch.24 kApproach_Point | kApproach_Hold claim per follower, filed exactly as apmf/Disengage.cpp files its own (same API, same
// kOwnBasis, no deny), and an optional ch.33 ACTOR-WIDE style claim above MFO's own class style (kOwnBasis + 10) for the quiet-circling
// rows. The caller (selftest/TestsHold.cpp) decides where, how big and how long; this file only files, re-points, reads and releases.
// NOTHING SHIPPED CALLS THIS. A release DLL does not compile it.
// Threads: the pump worker (the exerciser), and ClearTransientState / SelfTest::ClearAll at load boundaries. Own LEAF mutex g_devHoldMx
// (order g_mx -> g_devHoldMx from ClearTransientState, as the siblings; this file never takes g_mx). Nothing is saved.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

#ifndef MFO_DEV_SELFTEST
#error "apmf/DevHold.cpp is compiled only in a dev build (CMake -DMFO_DEV_SELFTEST=ON)"
#endif

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kHoldMinAbi  = 39;   // kApproach_Point
        constexpr std::uint32_t kStyleMinAbi = 45;   // ch.33
        constexpr float         kQuietBasis  = kOwnBasis + 10.0f;   // above StyleClaims.cpp's class style on the same actor

        struct Held {
            APMF_API::Handle hold  = APMF_API::kInvalidHandle;
            APMF_API::Handle style = APMF_API::kInvalidHandle;
        };
        std::mutex                             g_devHoldMx;
        std::unordered_map<RE::FormID, Held>   g_devHolds;

        const APMF_API::APMF_API_v19* Api(std::uint32_t a_min) {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < a_min) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        APMF_API::APMF_Param PointParam(const RE::NiPoint3& a_pt, float a_radius) {
            APMF_API::APMF_Param p{};
            p.ival = static_cast<std::int32_t>(APMF_API::kApproach_Point | APMF_API::kApproach_Hold);   // target stays 0
            p.fval = a_radius;
            p.posX = a_pt.x;
            p.posY = a_pt.y;
            p.posZ = a_pt.z;
            return p;
        }

        const char* StateName(std::uint32_t a_s) {
            switch (a_s) {
            case APMF_API::kApproachState_None:          return "None";
            case APMF_API::kApproachState_Waiting:       return "Waiting";
            case APMF_API::kApproachState_Approaching:   return "Approaching";
            case APMF_API::kApproachState_Holding:       return "Holding";
            case APMF_API::kApproachState_Yielded:       return "Yielded";
            case APMF_API::kApproachState_Disabled:      return "Disabled";
            case APMF_API::kApproachState_Leashed:       return "Leashed";
            case APMF_API::kApproachState_Arrived:       return "Arrived";
            case APMF_API::kApproachState_TargetLost:    return "TargetLost";
            case APMF_API::kApproachState_CombatEnded:   return "CombatEnded";
            case APMF_API::kApproachState_EngineDropped: return "EngineDropped";
            case APMF_API::kApproachState_ActorGone:     return "ActorGone";
            case APMF_API::kApproachState_Released:      return "Released";
            case APMF_API::kApproachState_Refused:       return "Refused";
            default:                                     return "?";
            }
        }
    }

    const char* DevHoldFile(RE::FormID a_follower, const RE::NiPoint3& a_point, float a_radius) {
        const auto* api = Api(kHoldMinAbi);
        if (!api) return "Harbinger absent or older than ABI 39";
        if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z) || !(a_radius > 0.0f)) return "bad point or radius";
        {
            std::scoped_lock lk(g_devHoldMx);
            if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end() && it->second.hold != APMF_API::kInvalidHandle)
                return "a dev hold already stands";
        }
        auto p = PointParam(a_point, a_radius);
        const auto h = api->RequestEx(a_follower, APMF_API::kIntent_CombatApproach, kOwnBasis, &p);
        if (h == APMF_API::kInvalidHandle) return "Harbinger refused the point hold at the call (its log says why)";
        std::scoped_lock lk(g_devHoldMx);
        g_devHolds[a_follower].hold = h;
        return nullptr;
    }

    bool DevHoldRepoint(RE::FormID a_follower, const RE::NiPoint3& a_point, float a_radius) {
        const auto* api = Api(kHoldMinAbi);
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lk(g_devHoldMx);
            if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end()) h = it->second.hold;
        }
        if (!api || h == APMF_API::kInvalidHandle) return false;
        auto p = PointParam(a_point, a_radius);
        api->Repoint(h, &p);
        return true;
    }

    const char* DevQuietFile(RE::FormID a_follower, RE::FormID a_style) {
        const auto* api = Api(kStyleMinAbi);
        if (!api) return "Harbinger absent or older than ABI 45 (no ch.33)";
        APMF_API::APMF_Param p{};
        p.form = a_style;
        p.ival = 0;   // actor-wide
        const auto h = api->RequestEx(a_follower, APMF_API::kIntent_CombatStyle, kQuietBasis, &p);
        if (h == APMF_API::kInvalidHandle) return "Harbinger refused the ch.33 style at the call (its log says why)";
        APMF_API::Handle old = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lk(g_devHoldMx);
            auto& e = g_devHolds[a_follower];
            old     = e.style;
            e.style = h;
        }
        if (old != APMF_API::kInvalidHandle) api->Release(old);
        return nullptr;
    }

    DevHoldInfo DevHoldRead(RE::FormID a_follower) {
        DevHoldInfo o;
        Held        h;
        {
            std::scoped_lock lk(g_devHoldMx);
            const auto it = g_devHolds.find(a_follower);
            if (it == g_devHolds.end()) return o;
            h = it->second;
        }
        const auto* api = Api(kHoldMinAbi);
        if (!api) return o;
        o.stands    = h.hold != APMF_API::kInvalidHandle;
        o.styleLive = h.style != APMF_API::kInvalidHandle && api->IsClaimLive(h.style);
        if (!o.stands || !api->GetCombatApproachState) return o;
        APMF_API::APMF_CombatApproachInfo info{};
        info.size = sizeof(info);
        api->GetCombatApproachState(a_follower, &info);
        o.ours     = info.ownerHandle == h.hold;
        o.state    = o.ours ? StateName(info.state) : "(another claim owns ch.24)";
        o.waiting  = o.ours && info.state == APMF_API::kApproachState_Waiting;
        o.ended    = o.ours && info.state >= APMF_API::kApproachState_Arrived && info.state <= APMF_API::kApproachState_Refused;
        o.dropped  = o.ours && info.state == APMF_API::kApproachState_EngineDropped;
        o.combatEnded = o.ours && info.state == APMF_API::kApproachState_CombatEnded;
        o.applied  = o.ours ? info.applied : 0;
        o.engineInside = o.ours ? info.engineInside : 0;
        return o;
    }

    void DevHoldRelease(RE::FormID a_follower) {
        Held h;
        {
            std::scoped_lock lk(g_devHoldMx);
            const auto it = g_devHolds.find(a_follower);
            if (it == g_devHolds.end()) return;
            h = it->second;
            g_devHolds.erase(it);
        }
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        if (h.hold != APMF_API::kInvalidHandle) api->Release(h.hold);   // a no-op on a claim Harbinger ended
        if (h.style != APMF_API::kInvalidHandle) api->Release(h.style);
    }

    void DevHoldReleaseAll() {
        std::unordered_map<RE::FormID, Held> all;
        {
            std::scoped_lock lk(g_devHoldMx);
            all.swap(g_devHolds);
        }
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return;
        for (const auto& [id, h] : all) {
            if (h.hold != APMF_API::kInvalidHandle) api->Release(h.hold);
            if (h.style != APMF_API::kInvalidHandle) api->Release(h.style);
        }
    }
}
