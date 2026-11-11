// apmf/GroundApproach.cpp -- GIVE GROUND: a ch.24 combat approach centred on a POINT (Harbinger ABI v39, kApproach_Point).
// A follower who should step back from a foe (he has seconds left, or he fights at range and a brute is on him) gets his
// combat area bounded to a point behind him for a moment. The engine's own Return To Combat Area walks him there and he
// keeps fighting the whole way: composition, the engine's movement, no package, no forced move.
// MFO DECLARES the point (kStep units straight away from the foe, from where he stands), the radius and the time. Harbinger
// carries the bound and ends the claim on arrival; MFO releases it after kHoldMs if it still stands. A point the engine
// cannot path to ends as EngineDropped in Harbinger (3 s, logged there); MFO releases first at kHoldMs and says so.
// NOT CLAIMED: facing. The engine may turn him away from the foe to walk there (to be looked at in the field).
// PRECEDENCE: one ch.24 claim per follower. A held-heal approach wins: no step is filed while it stands. An offense approach
// (same basis, filed earlier) keeps the channel in Harbinger: our claim then never owns, which Service sees and releases.
// Docs: Harbinger Docs/FACETS.md ch.24 "A point instead of a reference"; MFO Docs/research/2026-11/iaus-sim-v2.md 26.3 / 29.
// Threads: the pump's AddTask worker; its OWN leaf mutex, never g_mx and never nested with another approach mutex.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kGroundMinAbi = 39;   // the ABI that first reads kApproach_Point
        constexpr float         kStep   = 400.0f;     // how far behind him the point lies (the lab's step back: 400 to 500)
        constexpr float         kRadius = 150.0f;     // R: inside it he has arrived
        constexpr std::chrono::milliseconds kHoldMs{ 2500 };   // the lab's order holds 2 s; the claim never outlives this

        struct Step {
            APMF_API::Handle handle = APMF_API::kInvalidHandle;
            RE::FormID       foe = 0;
            std::chrono::steady_clock::time_point at{};
        };
        std::mutex g_groundMx;
        std::unordered_map<RE::FormID, Step> g_steps;   // follower -> his standing step

        const APMF_API::APMF_API_v19* GroundApi() {
            const auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kGroundMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v19*>(api);
        }

        const char* EndName(std::uint32_t a_state) {
            switch (a_state) {
            case APMF_API::kApproachState_Arrived:       return "arrived";
            case APMF_API::kApproachState_TargetLost:    return "he left the point's cell space";
            case APMF_API::kApproachState_CombatEnded:   return "combat ended";
            case APMF_API::kApproachState_EngineDropped: return "the engine dropped the bound (no path to the point)";
            case APMF_API::kApproachState_ActorGone:     return "actor gone";
            case APMF_API::kApproachState_Released:      return "released";
            case APMF_API::kApproachState_Refused:       return "refused";
            default:                                     return "ended";
            }
        }
    }

    bool GroundStepSupported() { return GroundApi() != nullptr; }

    bool RequestGroundStep(RE::Actor* a_follower, RE::Actor* a_foe) {
        const auto* api = GroundApi();
        if (!api || !a_follower || !a_foe) return false;
        const RE::FormID id = a_follower->GetFormID();
        if (HealApproachStands(id)) return false;   // a held heal's walk to its recipient outranks a step back
        {
            std::scoped_lock lk(g_groundMx);
            if (const auto it = g_steps.find(id); it != g_steps.end() && it->second.handle != APMF_API::kInvalidHandle)
                return false;   // one step at a time: the standing one is released by Service
        }
        const RE::NiPoint3 me = a_follower->GetPosition();
        const RE::NiPoint3 fp = a_foe->GetPosition();
        const float dx = me.x - fp.x, dy = me.y - fp.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(len) || len < 1.0f) return false;   // on top of the foe: "away" has no direction
        APMF_API::APMF_Param prm{};
        prm.ival = static_cast<std::int32_t>(APMF_API::kApproach_Point);   // ends on arrival; target stays 0
        prm.fval = kRadius;
        prm.posX = me.x + dx / len * kStep;
        prm.posY = me.y + dy / len * kStep;
        prm.posZ = me.z;
        const APMF_API::Handle h = api->RequestEx(id, APMF_API::kIntent_CombatApproach, kOwnBasis, &prm);
        if (h == APMF_API::kInvalidHandle) {
            spdlog::warn("[ground] {:08X}: Harbinger refused the step back at the call (its log says why)", id);
            return false;
        }
        {
            std::scoped_lock lk(g_groundMx);
            g_steps[id] = Step{ h, a_foe->GetFormID(), std::chrono::steady_clock::now() };
        }
        spdlog::info("[ground] {:08X} '{}': step back from {:08X} '{}' ({:.0f}u away) to {:.0f},{:.0f},{:.0f} (R {:.0f}, h={})", id,
                     a_follower->GetName() ? a_follower->GetName() : "?", a_foe->GetFormID(), a_foe->GetName() ? a_foe->GetName() : "?",
                     len, prm.posX, prm.posY, prm.posZ, kRadius, h);
        return true;
    }

    void ServiceGroundStep(RE::FormID a_follower) {
        Step s;
        {
            std::scoped_lock lk(g_groundMx);
            const auto it = g_steps.find(a_follower);
            if (it == g_steps.end() || it->second.handle == APMF_API::kInvalidHandle) return;
            s = it->second;
        }
        const auto* api = GroundApi();
        const char* why = nullptr;
        std::uint32_t applied = 0;
        float dist = -1.0f;
        const auto age = std::chrono::steady_clock::now() - s.at;
        if (!api) {
            why = "Harbinger is gone";
        } else {
            APMF_API::APMF_CombatApproachInfo info{};
            info.size = sizeof(info);
            if (api->GetCombatApproachState) api->GetCombatApproachState(a_follower, &info);
            applied = info.applied;
            dist = info.distance;
            if (info.ownerHandle == s.handle && info.state >= APMF_API::kApproachState_Arrived &&
                info.state <= APMF_API::kApproachState_Refused)
                why = EndName(info.state);
            else if (age > std::chrono::milliseconds(700) && info.ownerHandle != s.handle)
                why = "another ch.24 claim owns him (ours never applied)";
            else if (age > kHoldMs)
                why = applied != 0 ? "the step's time is up" : "the step's time is up and the engine never carried the bound";
        }
        if (!why) return;
        if (api) api->Release(s.handle);   // a no-op on a claim Harbinger already ended
        {
            std::scoped_lock lk(g_groundMx);
            const auto it = g_steps.find(a_follower);
            if (it != g_steps.end() && it->second.handle == s.handle) g_steps.erase(it);
        }
        spdlog::info("[ground] {:08X}: step back h={} over: {} (the area carried the bound {} time(s), last distance to the point {:.0f})",
                     a_follower, s.handle, why, applied, dist);
    }

    void ReleaseGroundStep(RE::FormID a_follower, const char* a_why) {
        Step s;
        {
            std::scoped_lock lk(g_groundMx);
            const auto it = g_steps.find(a_follower);
            if (it == g_steps.end()) return;
            s = it->second;
            g_steps.erase(it);
        }
        if (s.handle == APMF_API::kInvalidHandle) return;
        if (const auto* api = g_apmf.load(std::memory_order_relaxed)) api->Release(s.handle);
        spdlog::info("[ground] {:08X}: step back h={} released ({})", a_follower, s.handle, a_why ? a_why : "");
    }
}
