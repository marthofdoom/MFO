// apmf/CombatMove.cpp -- Harbinger's COMBAT MOVE (ABI v38, kIntent_CombatMove): a thin wrapper.
// MFO only DECLARES the move (a bash or a power bash, by whom, meant for which foe); Harbinger starts it ONCE on the game
// thread by the engine's own start-of-attack action (TESActionData::Process, the road the combat AI's Bash leaf takes) and
// reports Started / Refused / Failed by reading the actor's attack data and attack state back. A one-shot, never a claim:
// nothing to release or heartbeat, nothing saved.
// Docs: Harbinger Docs/FACETS.md "ch.31 Combat move", Docs/APMF_API-REFERENCE.md "ABI v38 combat move", INVARIANTS #0 (l);
// MFO Docs/research/2026-11/iaus-sim-v2.md sections 26 and 29.
// WHY NOT NotifyAnimationGraph("bashStart"): the engine sets an actor's attack data and attack state only on the action
// road. A bare animation event plays the swing with whatever attack data he had before.
// Threading: RequestEx is an enqueue (any thread); GetCombatMoveState is a copy under Harbinger's leaf lock (any thread).
// The small table below (the last move per follower, so its outcome is logged once) has its own leaf mutex, never g_mx.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kCombatMoveMinAbi = 38;   // the ABI that first serves intent 31 and carries GetCombatMoveState

        struct Last {
            std::uint32_t handle = 0;
            RE::FormID    foe = 0;
            bool          power = false;
            bool          finalLogged = false;   // the Started / Refused / Failed line was written
            bool          hitLogged = false;     // the hit (or "no hit inside the window") line was written
            std::chrono::steady_clock::time_point at{};
            std::chrono::steady_clock::time_point probeNext{};   // the dev probe's own spacing
        };
        std::mutex g_moveMx;
        std::unordered_map<RE::FormID, Last> g_last;   // follower -> his last requested move

        const char* WhyName(std::uint32_t a_why) {
            switch (a_why) {
            case APMF_API::kCombatMoveWhy_ActorGone:      return "actor gone";
            case APMF_API::kCombatMoveWhy_NotReady:       return "not ready (not loaded, dead, kill move, attacking disabled)";
            case APMF_API::kCombatMoveWhy_Busy:           return "an attack is already in progress";
            case APMF_API::kCombatMoveWhy_NotAttackReady: return "the behaviour graph is not attack-ready";
            case APMF_API::kCombatMoveWhy_TargetGone:     return "the foe is gone";
            case APMF_API::kCombatMoveWhy_NoAttackData:   return "he has no bash of that kind";
            case APMF_API::kCombatMoveWhy_OffenseDenied:  return "a ch.7 claim denies his offense";
            case APMF_API::kCombatMoveWhy_SubstrateHeld:  return "a combat substrate holds him";
            case APMF_API::kCombatMoveWhy_Outranked:      return "MFO does not outrank Engine on CombatAction (Harbinger priorities)";
            case APMF_API::kCombatMoveWhy_EngineRefused:  return "the engine refused the action";
            case APMF_API::kCombatMoveWhy_NotApplied:     return "the engine accepted it but his attack data / state are not the bash";
            case APMF_API::kCombatMoveWhy_Error:          return "an error inside Harbinger";
            default:                                      return "no reason given";
            }
        }
    }

    bool CombatMoveSupported() {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        return api && api->abiVersion >= kCombatMoveMinAbi;
    }

    std::uint32_t RequestBash(RE::FormID a_follower, RE::FormID a_foe, bool a_power) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kCombatMoveMinAbi) return APMF_API::kInvalidHandle;   // no road: the caller makes no move
        PollCombatMove(a_follower);   // the previous move's outcome is written before its record is replaced
        APMF_API::APMF_Param p{};
        p.ival   = static_cast<std::int32_t>(a_power ? APMF_API::kCombatMove_PowerBash : APMF_API::kCombatMove_Bash);
        p.target = a_foe;
        const std::uint32_t h = api->RequestEx(a_follower, APMF_API::kIntent_CombatMove, 0.0f, &p);   // basis is not read
        if (h == APMF_API::kInvalidHandle) {
            spdlog::warn("[combat-move] {:08X}: Harbinger refused the {} request at the call (its log says why)", a_follower,
                         a_power ? "power bash" : "bash");
            return h;
        }
        std::scoped_lock lk(g_moveMx);
        auto& l = g_last[a_follower];
        const auto probeNext = l.probeNext;
        l = Last{};
        l.handle = h;
        l.foe = a_foe;
        l.power = a_power;
        l.at = std::chrono::steady_clock::now();
        l.probeNext = probeNext;
        return h;
    }

    void PollCombatMove(RE::FormID a_follower) {
        const auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || api->abiVersion < kCombatMoveMinAbi) return;
        Last l;
        {
            std::scoped_lock lk(g_moveMx);
            const auto it = g_last.find(a_follower);
            if (it == g_last.end() || it->second.handle == 0 || (it->second.finalLogged && it->second.hitLogged)) return;
            l = it->second;
        }
        APMF_API::APMF_CombatMoveStateInfo info{};
        info.size = sizeof(info);
        const std::uint32_t st = reinterpret_cast<const APMF_API::APMF_API_v38*>(api)->GetCombatMoveState(l.handle, &info);
        if (st == APMF_API::kCombatMoveState_Queued) return;   // the game thread has not run it yet
        bool finalNow = false, hitNow = false;
        const char* kind = l.power ? "power bash" : "bash";
        if (!l.finalLogged) {
            finalNow = true;
            if (st == APMF_API::kCombatMoveState_Started)
                spdlog::info("[combat-move] {:08X}: {} h={} STARTED on {:08X} ({:.0f}u, {:.0f} deg off his heading, stamina spent {:.0f})",
                             a_follower, kind, l.handle, l.foe, info.distance, info.bearingDeg, info.staminaSpent);
            else if (st == APMF_API::kCombatMoveState_Refused)
                spdlog::info("[combat-move] {:08X}: {} h={} REFUSED: {}", a_follower, kind, l.handle, WhyName(info.why));
            else if (st == APMF_API::kCombatMoveState_Failed)
                spdlog::warn("[combat-move] {:08X}: {} h={} FAILED: {}", a_follower, kind, l.handle, WhyName(info.why));
            else
                spdlog::warn("[combat-move] {:08X}: {} h={} has no record in Harbinger any more (a load, or it was read too late)",
                             a_follower, kind, l.handle);
        }
        if (st != APMF_API::kCombatMoveState_Started) {
            hitNow = true;   // no hit line belongs to a move that never started
        } else if (info.info & APMF_API::kCombatMoveInfo_HitSeen) {
            hitNow = true;
            spdlog::info("[combat-move] {:08X}: {} h={} HIT {:08X}{}{}", a_follower, kind, l.handle, info.hitTarget,
                         (info.info & APMF_API::kCombatMoveInfo_HitOnTarget) ? " (the foe it was meant for)" : " (NOT the foe it was meant for)",
                         (info.info & APMF_API::kCombatMoveInfo_HitBlocked) ? ", blocked" : "");
        } else if (std::chrono::steady_clock::now() - l.at >
                   std::chrono::milliseconds(APMF_API::kCombatMoveHitWindowMs + 500)) {
            hitNow = true;
            spdlog::info("[combat-move] {:08X}: {} h={} started but NO bash hit arrived inside {} ms (out of reach, facing away, or the "
                         "swing was interrupted)", a_follower, kind, l.handle, APMF_API::kCombatMoveHitWindowMs);
        }
        if (!finalNow && !hitNow) return;
        std::scoped_lock lk(g_moveMx);
        const auto it = g_last.find(a_follower);
        if (it == g_last.end() || it->second.handle != l.handle) return;
        it->second.finalLogged = it->second.finalLogged || finalNow;
        it->second.hitLogged = it->second.hitLogged || hitNow;
    }

    // DEV PROBE ([Debug] bCombatMoveProbe, INI-only, default OFF). The first field test of the road: a follower in combat
    // whose combat target stands inside melee reach asks for a bash every 6 s, whatever his gambits or the IAUS would do.
    // It exists to PROVE the road (does the engine accept the action, does a bash hit follow) before anything relies on it.
    void CombatMoveProbeTick(RE::Actor* a_follower) {
        if (!a_follower || !Config::g_combatMoveProbe.load(std::memory_order_relaxed)) return;
        const RE::FormID id = a_follower->GetFormID();
        PollCombatMove(id);
        if (!CombatMoveSupported()) {
            static std::atomic<bool> s_said{ false };
            if (!s_said.exchange(true))
                spdlog::warn("[combat-move] bCombatMoveProbe=1 but Harbinger is absent or older than ABI {}: no probe bash is sent",
                             kCombatMoveMinAbi);
            return;
        }
        if (!a_follower->IsInCombat()) return;
        auto tp = a_follower->GetActorRuntimeData().currentCombatTarget.get();   // HOLD
        auto* foe = tp.get();
        if (!foe || foe->IsDead()) return;
        if (a_follower->GetPosition().GetDistance(foe->GetPosition()) > Config::g_meleeReach.load(std::memory_order_relaxed)) return;
        const auto now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lk(g_moveMx);
            auto& l = g_last[id];
            if (now < l.probeNext) return;
            l.probeNext = now + std::chrono::seconds(6);
        }
        const std::uint32_t h = RequestBash(id, foe->GetFormID(), /*a_power=*/false);
        if (h != APMF_API::kInvalidHandle)
            spdlog::info("[combat-move] {:08X} '{}': PROBE bash requested on {:08X} '{}' (h={}, bCombatMoveProbe)", id,
                         a_follower->GetName() ? a_follower->GetName() : "?", foe->GetFormID(), foe->GetName() ? foe->GetName() : "?", h);
    }
}
