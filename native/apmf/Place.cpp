// apmf/Place.cpp -- Harbinger's PLACE facet (ABI v28, kIntent_Place): a thin wrapper, TOWN PHASE 2 R4b (ClickUp 86e3t1mx7).
// MFO only DECLARES the spot (a destination reference, an offset, a yaw, the flags); Harbinger runs the ONE engine move
// (TESObjectREFR::MoveTo_Impl) on the game thread and reports Placed / Refused / Failed by read-back. A one-shot, never a
// claim: nothing to release or heartbeat, no MFO state here, nothing saved.
// Docs: APMF Docs/FACETS.md "Place" (tables 20b / 20c), Docs/APMF_API-REFERENCE.md#abi-v28-place, INVARIANTS #0 (k).
// Threading: RequestEx is an enqueue (any thread); GetPlaceState is a copy under APMF's leaf lock (any thread). No lock here.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <atomic>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kPlaceMinAbi = 28;   // the ABI that first serves intent 28 and carries GetPlaceState
    }

    bool PlaceSupported() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kPlaceMinAbi;
    }

    std::uint32_t RequestPlace(RE::FormID a_actor, RE::FormID a_dest, const RE::NiPoint3& a_offset, float a_yaw, std::uint32_t a_flags) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api) return APMF_API::kInvalidHandle;   // Harbinger absent: a documented no-op (errands do not run without it)
        if (api->abiVersion < kPlaceMinAbi) {
            static std::atomic<bool> s_said{false};
            if (!s_said.exchange(true))
                spdlog::warn("[place] Harbinger ABI {} < {}: no placement facet -- an off-screen errand follower is NOT placed at the "
                             "counter (he stays where the engine has him)", api->abiVersion, kPlaceMinAbi);
            return APMF_API::kInvalidHandle;
        }
        APMF_API::APMF_Param p{};
        p.form = a_dest;
        p.posX = a_offset.x; p.posY = a_offset.y; p.posZ = a_offset.z;
        p.ival = static_cast<std::int32_t>(a_flags);
        p.fval = a_yaw;
        return api->RequestEx(a_actor, APMF_API::kIntent_Place, 0.0f, &p);   // basis is not read for a placement
    }

    PlaceRead ReadPlace(std::uint32_t a_handle) {
        PlaceRead r;
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kPlaceMinAbi || a_handle == APMF_API::kInvalidHandle) return r;   // state 0 = Unknown
        APMF_API::APMF_PlaceStateInfo info{};
        info.size = sizeof(info);
        r.state = reinterpret_cast<const APMF_API::APMF_API_v28*>(api)->GetPlaceState(a_handle, &info);
        r.why = info.why;
        r.blocker = info.blocker;
        r.cell = info.cell;
        r.pos = RE::NiPoint3(info.x, info.y, info.z);
        return r;
    }
}
