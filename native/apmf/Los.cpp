// apmf/Los.cpp -- Harbinger's own line of sight and awareness (ABI v18),
// feat/mfo-harbinger-los-adopt. Contract: APMF_API.h "ABI v18: LINE OF SIGHT AND
// AWARENESS". Two thin wrappers over the two v18 slots, nothing else:
//
//   LosOf     -> APMF_API_v18::GetLineOfSight (ANY thread, lock-free, never casts a ray;
//                the ask itself keeps the pair measured). Mapped to Sightline::Verdict:
//                ONLY kLos_Visible is Visible, kLos_Occluded is Occluded, Unknown and
//                Unavailable are Unknown (fail-open exactly like Sightline's cold cache,
//                never "seen"). kLos_Unsupported = the service is not armed: the caller
//                keeps Sightline (LosOf returns false).
//   SenseOf   -> APMF_API_v18::SenseActor (TRUE MAIN THREAD ONLY: it casts rays).
//
// Below ABI v18, with APMF absent, or with Config::g_apmfCast off, both report "not
// usable" and the caller keeps MFO's own Sightline path unchanged (the documented
// degrade). The state is logged once per change, never per call.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"   // g_apmf
#include "APMF_API.h"
#include "Config.h"

#include <atomic>
#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kLosMinAbi = 18;

        // 0 = not yet seen, 1 = ABI < 18 (own Sightline), 2 = v18 usable, 3 = v18 but the
        // service reported kLos_Unsupported. Logged once per change.
        std::atomic<int> g_losState{ 0 };

        void NoteState(int a_state, std::uint32_t a_abi) {
            if (g_losState.exchange(a_state, std::memory_order_relaxed) == a_state) return;
            switch (a_state) {
            case 1:
                spdlog::info("[apmf] ABI v{} < {}: no Harbinger line of sight -- MFO uses its own Sightline "
                             "for spells, bows and engage-on-sight", a_abi, kLosMinAbi);
                break;
            case 2:
                spdlog::info("[apmf] ABI v{}: Harbinger's own line of sight and awareness in use "
                             "(spell/bow LoS reads, own-LoS cast + pin flags, engage-on-sight senses)", a_abi);
                break;
            case 3:
                spdlog::warn("[apmf] ABI v{} but Harbinger reports line of sight UNSUPPORTED (VR, an unverified "
                             "runtime, [Sightline] bOwnLineOfSight=0 or before kDataLoaded) -- MFO uses its "
                             "own Sightline", a_abi);
                break;
            default: break;
            }
        }

        const APMF_API::APMF_API_v18* ApiV18() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api) return nullptr;                        // APMF absent: nothing to log
            if (api->abiVersion < kLosMinAbi) { NoteState(1, api->abiVersion); return nullptr; }
            return reinterpret_cast<const APMF_API::APMF_API_v18*>(api);
        }
    }

    bool LosSupported() {
        if (!Config::g_apmfCast.load()) return false;
        const auto* api = ApiV18();
        if (!api) return false;
        // ARMED PROBE (APMF core/Sightline.cpp GetLineOfSight, verified against APMF main):
        // with the service not armed (VR, an unverified runtime, [Sightline] bOwnLineOfSight=0,
        // before kDataLoaded) the call returns kLos_Unsupported FIRST, before it reads any
        // argument, and with armed it never returns Unsupported. (0, 0, nullptr) is a free,
        // lock-free, no-ask probe. Not latched: a service that arms later is used.
        if (!api->GetLineOfSight) return false;
        if (api->GetLineOfSight(0, 0, nullptr) == APMF_API::kLos_Unsupported) {
            NoteState(3, api->abiVersion);
            return false;
        }
        return true;
    }

    bool LosOf(RE::FormID a_viewer, RE::FormID a_target, LosReading& a_out) {
        if (!Config::g_apmfCast.load() || !a_viewer || !a_target) return false;
        const auto* api = ApiV18();
        if (!api || !api->GetLineOfSight) return false;
        APMF_API::APMF_LosInfo info{};
        info.size = sizeof(info);
        const std::uint32_t v = api->GetLineOfSight(a_viewer, a_target, &info);
        if (v == APMF_API::kLos_Unsupported) { NoteState(3, api->abiVersion); return false; }
        NoteState(2, api->abiVersion);
        a_out.verdict = v == APMF_API::kLos_Visible  ? Sightline::Verdict::Visible
                      : v == APMF_API::kLos_Occluded ? Sightline::Verdict::Occluded
                                                     : Sightline::Verdict::Unknown;
        a_out.ageMs       = info.ageMs;
        a_out.occludedRun = info.occludedRun;
        return true;
    }

    bool SenseOf(RE::FormID a_viewer, RE::FormID a_target, RE::FormID a_anchor, float a_sightRange,
                 SenseReading& a_out) {
        if (!Config::g_apmfCast.load() || !a_viewer || !a_target) return false;
        const auto* api = ApiV18();
        if (!api || !api->SenseActor) return false;
        APMF_API::APMF_AwarenessQuery q{};
        q.size       = sizeof(q);
        q.viewer     = a_viewer;
        q.target     = a_target;
        q.anchor     = a_anchor;
        q.sightRange = a_sightRange;   // hearRadius / proximityRadius / flags stay 0: Harbinger's defaults, all four senses
        APMF_API::APMF_AwarenessResult r{};
        r.size = sizeof(r);
        const std::uint32_t st = api->SenseActor(&q, &r);
        if (st == APMF_API::kQuery_Unsupported) { NoteState(3, api->abiVersion); return false; }
        if (st != APMF_API::kQuery_Ok) return false;   // NotMainThread / BadArgs / NoOrigin / Failed: no reading
        NoteState(2, api->abiVersion);
        a_out.senses       = r.senses;
        a_out.detail       = r.detail;
        a_out.distance     = r.distance;
        a_out.sightVerdict = r.sightVerdict;
        a_out.engagedWith  = r.engagedWith;
        return true;
    }
}
