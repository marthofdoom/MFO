// apmf/DevHold.cpp -- DEV BUILDS ONLY (CMake MFO_DEV_SELFTEST): the claim road of the G0b hold exerciser (movement-plan.md package G0b).
// ONE ch.24 kApproach_Point | kApproach_Hold claim per follower, filed exactly as apmf/Disengage.cpp files its own (same API, same
// kOwnBasis, no deny), and an optional ch.33 ACTOR-WIDE style claim above MFO's own class style (kOwnBasis + 10) for the quiet-circling
// rows. The caller (selftest/TestsHold.cpp) decides where, how big and how long; this file only files, re-points, reads and releases.
// STRICT (ABI v48, kApproach_Strict; selftest/TestsStrict.cpp): the same one claim with the Strict bit, only where DevStrictServe says the
// install serves it (abiVersion >= 48 AND kMoveCaps_Strict, by CanServe of exactly that call). Basis above MFO's own ch.24 clients, so the
// dev test measures the order and not the arbitration. SetMoveOrder is main-thread only: posted, its answer kept for the worker.
// NOTHING SHIPPED CALLS THIS. A release DLL does not compile it.
// Threads: the pump worker (the exerciser), and ClearTransientState / SelfTest::ClearAll at load boundaries. Own LEAF mutex g_devHoldMx
// (order g_mx -> g_devHoldMx from ClearTransientState, as the siblings; this file never takes g_mx). Nothing is saved.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "MainThread.h"

#include <cmath>
#include <format>
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
        constexpr std::uint32_t kStrictMinAbi = 48;  // kApproach_Strict, GetMoveOrderState
        constexpr float         kStrictBasis = kOwnBasis + 20.0f;
        constexpr float         kQuietBasis  = kOwnBasis + 10.0f;   // above StyleClaims.cpp's class style on the same actor

        struct Held {
            APMF_API::Handle hold  = APMF_API::kInvalidHandle;
            APMF_API::Handle style = APMF_API::kInvalidHandle;
            bool             strict = false;
            std::int32_t     setFree = -1;   // SetMoveOrder(kMove_Free)'s answer, -1 = not asked
        };
        std::mutex                             g_devHoldMx;
        std::unordered_map<RE::FormID, Held>   g_devHolds;

        const APMF_API::APMF_API_v19* Api(std::uint32_t a_min) {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < a_min) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        APMF_API::APMF_Param PointParam(const RE::NiPoint3& a_pt, float a_radius, bool a_strict) {
            APMF_API::APMF_Param p{};
            p.ival = static_cast<std::int32_t>(APMF_API::kApproach_Point | APMF_API::kApproach_Hold |
                                               (a_strict ? static_cast<std::uint32_t>(APMF_API::kApproach_Strict) : 0u));   // target stays 0
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
            case APMF_API::kApproachState_Unreachable:   return "Unreachable";
            default:                                     return "?";
            }
        }
    }

    const char* DevHoldFile(RE::FormID a_follower, const RE::NiPoint3& a_point, float a_radius, bool a_strict) {
        const auto* api = Api(a_strict ? kStrictMinAbi : kHoldMinAbi);
        if (!api) return a_strict ? "Harbinger absent or older than ABI 48" : "Harbinger absent or older than ABI 39";
        if (!std::isfinite(a_point.x) || !std::isfinite(a_point.y) || !std::isfinite(a_point.z) || !(a_radius > 0.0f)) return "bad point or radius";
        {
            std::scoped_lock lk(g_devHoldMx);
            if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end() && it->second.hold != APMF_API::kInvalidHandle)
                return "a dev hold already stands";
        }
        auto p = PointParam(a_point, a_radius, a_strict);
        const auto h = api->RequestEx(a_follower, APMF_API::kIntent_CombatApproach, a_strict ? kStrictBasis : kOwnBasis, &p);
        if (h == APMF_API::kInvalidHandle) return "Harbinger refused the point hold at the call (its log says why)";
        std::scoped_lock lk(g_devHoldMx);
        auto& e   = g_devHolds[a_follower];
        e.hold    = h;
        e.strict  = a_strict;
        e.setFree = -1;
        return nullptr;
    }

    std::string DevStrictServe(RE::FormID a_follower, const RE::NiPoint3& a_point, float a_radius) {
        const auto* base = g_apmf.load(std::memory_order_relaxed);
        if (!base) return "Harbinger is absent";
        if (base->abiVersion < kStrictMinAbi) return std::format("Harbinger ABI {} (strict orders need {})", base->abiVersion, kStrictMinAbi);
        auto p = PointParam(a_point, a_radius, true);
        APMF_API::APMF_ServeQuery q{};
        q.size   = sizeof(q);
        q.call   = APMF_API::kServeCall_RequestEx;
        q.actor  = a_follower;
        q.intent = APMF_API::kIntent_CombatApproach;
        q.basis  = kStrictBasis;
        q.param  = &p;
        APMF_API::APMF_ServeInfo info{};
        info.size = sizeof(info);
        const auto v = reinterpret_cast<const APMF_API::APMF_API_v48*>(base)->CanServe(&q, &info);
        info.why[sizeof(info.why) - 1] = '\0';
        if ((info.moveCaps & APMF_API::kMoveCaps_Strict) == 0) return std::format("Harbinger does not serve strict orders here ({})", info.why);
        if (v == APMF_API::kServe_Refused) return std::format("Harbinger refuses this strict claim: {}", info.why);
        return {};
    }

    DevStrictInfo DevStrictRead(RE::FormID a_follower) {
        DevStrictInfo o;
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kStrictMinAbi) return o;
        APMF_API::APMF_MoveOrderInfo info{};
        info.size = sizeof(info);
        o.api     = true;
        o.phase   = reinterpret_cast<const APMF_API::APMF_API_v48*>(api)->GetMoveOrderState(a_follower, &info);
        o.dist = info.dist; o.sinceMs = info.sinceMs; o.passed = info.passedThrough; o.displaced = info.displacedCount;
        o.gen = info.gen; o.stopError = info.stopError;
        std::scoped_lock lk(g_devHoldMx);
        if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end()) {
            o.ours    = it->second.hold != APMF_API::kInvalidHandle && info.ownerHandle == it->second.hold;
            o.setFree = it->second.setFree;
        }
        return o;
    }
    const char* DevStrictPhaseName(std::uint32_t a_p) {
        constexpr const char* n[] = { "none", "PENDING", "TRAVELLING", "STANDING", "RETURNING", "BLOCKED", "YIELDED", "NOT CARRIED", "UNREACHABLE" };
        static_assert(APMF_API::kMovePhase_Unreachable == 8);
        return a_p < std::size(n) ? n[a_p] : "?";
    }

    void DevStrictAskFree(RE::FormID a_follower) {
        MainThread::Post([a_follower]() {   // SetMoveOrder: the game's main thread only
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kStrictMinAbi) return;
            APMF_API::Handle h = APMF_API::kInvalidHandle;
            {
                std::scoped_lock lk(g_devHoldMx);
                const auto it = g_devHolds.find(a_follower);
                if (it == g_devHolds.end() || !it->second.strict) return;
                h = it->second.hold;
            }
            APMF_API::APMF_MoveOrder ord{};
            ord.size = sizeof(ord);
            ord.mode = APMF_API::kMove_Free;
            const auto r = reinterpret_cast<const APMF_API::APMF_API_v48*>(api)->SetMoveOrder(h, &ord);
            std::scoped_lock lk(g_devHoldMx);
            if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end() && it->second.hold == h) it->second.setFree = static_cast<std::int32_t>(r);
        });
    }

    bool DevHoldRepoint(RE::FormID a_follower, const RE::NiPoint3& a_point, float a_radius) {
        const auto* api = Api(kHoldMinAbi);
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        bool             strict = false;
        {
            std::scoped_lock lk(g_devHoldMx);
            if (const auto it = g_devHolds.find(a_follower); it != g_devHolds.end()) { h = it->second.hold; strict = it->second.strict; }
        }
        if (!api || h == APMF_API::kInvalidHandle) return false;
        auto p = PointParam(a_point, a_radius, strict);
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
        o.ended    = o.ours && info.state >= APMF_API::kApproachState_Arrived && info.state <= APMF_API::kApproachState_Unreachable;
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
