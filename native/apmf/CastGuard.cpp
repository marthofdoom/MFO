// apmf/CastGuard.cpp -- the ch.25 CAST GUARD claim (ABI v21, kIntent_CastGuard):
// MFO's friendly-fire hold and its Exact concentration bound, expressed as ONE
// Harbinger claim per follower instead of MFO's own CheckCast / CheckStartCast
// hooks (fix/mfo-gates-client-only, marth 2026-10-12: "Add to Harbinger's API").
// Recipe: APMF Docs/INTEGRATION.md "Cast guards". APMFBridge.h carries the contract.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kGuardMinAbi = 21;   // the ABI that first serves intent 25

        // The claim's window. Harbinger bounds it like a cast claim and every Repoint
        // renews it; the service lap repoints far more often than this (one combat
        // round-robin lap), so the window only matters once the laps stop (combat
        // end, a stalled pump): then the guard dies on its own within it.
        constexpr float kGuardWindowSecs = 6.0f;

        // NO DECAY (review of 162c1dd). MFO's own hook let every 4th held cast through
        // (CasterConsent kMaxFFHolds). Harbinger holds for as long as a friendly stands
        // in the line and re-measures at the release, which is its documented stance, so
        // the LineOfFire bit is kept steady and only Repoint'd. A held-count decay inside
        // ch.25 would be a Harbinger item (no per-cast readback reaches the client).

        struct Guard {
            APMF_API::Handle handle   = APMF_API::kInvalidHandle;
            std::uint32_t    bits     = 0;   // what the handle carries now
            bool             announced = false;   // CLAIMED logged (once per stretch of wanting a guard)
            std::chrono::steady_clock::time_point mintedAt{};
        };
        std::mutex                            g_guardMx;
        std::unordered_map<RE::FormID, Guard> g_guards;          // guarded by g_guardMx
        std::uint32_t                         g_refusedBits = 0; // guard bits Harbinger refused this session (g_guardMx)

        const APMF_API::APMF_API_v6* GuardApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kGuardMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v6*>(api);   // v21 >= v6: Repoint + IsClaimLive
        }

        std::string BitsName(std::uint32_t a_bits) {
            std::string s;
            if (a_bits & APMF_API::kCastGuard_LineOfFire) s += "LineOfFire";
            if (a_bits & (APMF_API::kCastGuard_ChannelRight | APMF_API::kCastGuard_ChannelLeft))
                s += s.empty() ? "Channel(R+L)" : "+Channel(R+L)";
            return s.empty() ? "none" : s;
        }
    }

    bool CastGuardSupported() { return GuardApi() != nullptr; }

    void ServiceCastGuard(RE::FormID a_follower, bool a_lineOfFire, bool a_channelBound) {
        const auto* api = GuardApi();
        if (!api || a_follower == 0 || a_follower == 0x14) return;   // Harbinger refuses the player
        constexpr std::uint32_t kChannel = APMF_API::kCastGuard_ChannelRight | APMF_API::kCastGuard_ChannelLeft;
        std::uint32_t want = 0;
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        std::uint32_t have = 0;
        bool stale = false;
        {
            std::scoped_lock lock(g_guardMx);
            auto& g = g_guards[a_follower];
            if (a_lineOfFire) want |= APMF_API::kCastGuard_LineOfFire;
            if (a_channelBound) want |= kChannel;
            want &= ~g_refusedBits;   // a guard Harbinger refused this session is not asked again
            h    = g.handle;
            have = g.bits;
            if (want == 0) {
                // No guard applies this lap: no claim (a Repoint with no bit is refused
                // by Harbinger, so the handle is released instead).
                g.handle = APMF_API::kInvalidHandle;
                g.bits = 0;
                g_guards.erase(a_follower);
            } else if (h != APMF_API::kInvalidHandle) {
                // A handle Harbinger no longer knows (its window lapsed, a release-all
                // hotkey) would make Repoint a silent no-op: re-request.
                stale = std::chrono::steady_clock::now() - g.mintedAt >= kEquipAuthValidateAfter &&
                        !api->IsClaimLive(h);
            }
        }
        if (want == 0) {
            if (h != APMF_API::kInvalidHandle) api->Release(h);
            return;
        }
        if (h != APMF_API::kInvalidHandle && !stale) {
            // Heartbeat: Repoint renews the window and re-reads ival (an MCM change
            // rides it).
            APMF_API::APMF_Param p{};
            p.ival = static_cast<std::int32_t>(want);
            p.fval = kGuardWindowSecs;
            api->Repoint(h, &p);
            if (have != want) {
                std::scoped_lock lock(g_guardMx);
                if (auto it = g_guards.find(a_follower); it != g_guards.end() && it->second.handle == h)
                    it->second.bits = want;
            }
            return;
        }
        if (stale) api->Release(h);   // harmless on a handle Harbinger already dropped
        APMF_API::APMF_Param p{};
        p.ival = static_cast<std::int32_t>(want);
        p.fval = kGuardWindowSecs;
        const APMF_API::Handle nh = api->RequestEx(a_follower, APMF_API::kIntent_CastGuard, kOwnBasis, &p);
        std::scoped_lock lock(g_guardMx);
        auto& g = g_guards[a_follower];
        if (nh == APMF_API::kInvalidHandle) {
            g.handle = APMF_API::kInvalidHandle;
            g.bits = 0;
            // NEVER MASKED, NEVER WORKED AROUND: no MFO hook stands in (client mode).
            // Harbinger refuses on configuration (CheckCast gate not installed, the
            // line-of-fire measurement not armed), which a load does not change, so
            // the refused guard is struck for the session and said once. LineOfFire
            // is struck first when both were asked (it alone has the extra arming
            // rule), so the channel bound is asked again alone next lap.
            const std::uint32_t strike = (want & APMF_API::kCastGuard_LineOfFire) ? APMF_API::kCastGuard_LineOfFire : want;
            g_refusedBits |= strike;
            spdlog::error("[cast-guard] Harbinger REFUSED the ch.25 cast guard {} (asked {} for {:08X}) -- {} is "
                          "INACTIVE for this session (APMF's log names why). No MFO hook stands in.",
                          BitsName(strike), BitsName(want), a_follower, BitsName(strike));
            return;
        }
        g.handle = nh;
        g.bits = want;
        g.mintedAt = std::chrono::steady_clock::now();
        if (std::exchange(g.announced, true)) return;
        spdlog::info("[cast-guard] {:08X}: ch.25 guard CLAIMED {} (h={}, window {:.0f} s)", a_follower,
                     BitsName(want), nh, kGuardWindowSecs);
    }

    void ReleaseCastGuard(RE::FormID a_follower) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_guardMx);
            const auto it = g_guards.find(a_follower);
            if (it == g_guards.end()) return;
            h = it->second.handle;
            g_guards.erase(it);
        }
        if (h == APMF_API::kInvalidHandle) return;
        if (auto* api = g_apmf.load(std::memory_order_relaxed)) api->Release(h);
        spdlog::info("[cast-guard] {:08X}: ch.25 guard released", a_follower);
    }

    void ClearCastGuards() {
        std::scoped_lock lock(g_guardMx);
        auto* api = g_apmf.load(std::memory_order_relaxed);
        for (auto& [fid, g] : g_guards)
            if (api && g.handle != APMF_API::kInvalidHandle) api->Release(g.handle);
        g_guards.clear();
    }

}
