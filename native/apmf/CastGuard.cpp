// apmf/CastGuard.cpp -- the ch.25 CAST GUARD claim (ABI v21, kIntent_CastGuard):
// MFO's friendly-fire hold and its Exact concentration bound, expressed as ONE
// Harbinger claim per follower instead of MFO's own CheckCast / CheckStartCast
// hooks (fix/mfo-gates-client-only, marth 2026-10-12: "Add to Harbinger's API").
// Recipe: APMF Docs/INTEGRATION.md "Cast guards". APMFBridge.h carries the contract.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "CastGuardBits.h"
#include "Config.h"

#include <atomic>

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kGuardMinAbi = 21;   // the ABI that first serves intent 25
        // kCastGuard_Bystanders (marth 2026-11-02 crime ruling, "Bystanders only"): Harbinger also holds a
        // weapon attack or a Self area spell that would hit a bystander (not the player, not the party).
        // It rides bFriendlyFireHold with LineOfFire. An APMF below ABI 32 REFUSES the whole claim for an
        // unknown bit, so the bit is asked only from ABI 32 up (older: LineOfFire alone, as before).
        // Both gates and the self blast bits live in CastGuardBits.h (pure, harness rows); its literals are the header's:
        static_assert(CastGuardBits::kLineOfFire == APMF_API::kCastGuard_LineOfFire && CastGuardBits::kBystanders == APMF_API::kCastGuard_Bystanders);
        static_assert(CastGuardBits::kChannel == (APMF_API::kCastGuard_ChannelRight | APMF_API::kCastGuard_ChannelLeft));
        static_assert(CastGuardBits::kSelfBlastParty == APMF_API::kCastGuard_SelfBlastParty && CastGuardBits::kSelfBlastSummons == APMF_API::kCastGuard_SelfBlastSummons);

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
            CastGuardBits::In in;            // what the last service (and SetSelfBlastGuard) asked for
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
            if (a_bits & APMF_API::kCastGuard_Bystanders) s += s.empty() ? "Bystanders" : "+Bystanders";
            if (a_bits & (APMF_API::kCastGuard_ChannelRight | APMF_API::kCastGuard_ChannelLeft))
                s += s.empty() ? "Channel(R+L)" : "+Channel(R+L)";
            if (a_bits & CastGuardBits::kSelfBlastParty) s += s.empty() ? "SelfBlastParty" : "+SelfBlastParty";
            if (a_bits & CastGuardBits::kSelfBlastSummons) s += s.empty() ? "SelfBlastSummons" : "+SelfBlastSummons";
            return s.empty() ? "none" : s;
        }
    }

    bool CastGuardSupported() { return GuardApi() != nullptr; }

    namespace {
        // THE DRY RUN (Harbinger ABI v35), ONCE PER SESSION: are the self blast bits served? They came without an ABI bump, and a Repoint that carries an
        // unknown bit is DROPPED, so nothing is repointed with them before this answered. Asked with the two bits ALONE: the answer is about them. What it
        // reads (the header's bits, the measurement armed at kDataLoaded) does not change in a session. 0 = not asked, 1 = served, 2 = not served.
        std::atomic<int> g_selfBlastServed{ 0 };

        bool SelfBlastServed(const APMF_API::APMF_API_v6* a_api, RE::FormID a_follower) {
            if (const int s = g_selfBlastServed.load(std::memory_order_acquire); s != 0) return s == 1;
            std::uint32_t v = APMF_API::kServe_Failed;
            APMF_API::APMF_ServeInfo info{};
            info.size = sizeof(info);
            if (a_api->abiVersion >= CastGuardBits::kServeMinAbi) {
                APMF_API::APMF_Param p{};
                p.ival = static_cast<std::int32_t>(CastGuardBits::kSelfBlast);
                p.fval = kGuardWindowSecs;
                APMF_API::APMF_ServeQuery q{};
                q.size   = sizeof(q);
                q.call   = APMF_API::kServeCall_RequestEx;
                q.actor  = a_follower;
                q.intent = APMF_API::kIntent_CastGuard;
                q.basis  = kOwnBasis;
                q.param  = &p;
                v = reinterpret_cast<const APMF_API::APMF_API_v35*>(a_api)->CanServe(&q, &info);
            }
            const bool served = v == APMF_API::kServe_Accepted;
            int expected = 0;
            if (g_selfBlastServed.compare_exchange_strong(expected, served ? 1 : 2, std::memory_order_acq_rel)) {
                info.why[sizeof(info.why) - 1] = '\0';
                if (served)
                    spdlog::info("[cast-guard] Harbinger serves the self blast party hold (kCastGuard_SelfBlastParty / Summons; asked once per session): it stands "
                                 "with every IAUS self blast claim.");
                else
                    spdlog::warn("[cast-guard] Harbinger does NOT serve the self blast party hold (ABI {}, verdict {}: {}) -- a self blast is guarded by MFO's own "
                                 "census and margin alone, as before. Said once per session.",
                                 a_api->abiVersion, v, a_api->abiVersion < CastGuardBits::kServeMinAbi ? "no CanServe" : info.why);
            }
            return served;
        }

        // The claim of the follower whose inputs are stored in g_guards: file, repoint or release. Worker.
        void Apply(const APMF_API::APMF_API_v6* api, RE::FormID a_follower) {
            CastGuardBits::In in;
            {
                std::scoped_lock lock(g_guardMx);
                const auto it = g_guards.find(a_follower);
                if (it == g_guards.end()) return;
                in = it->second.in;
            }
            in.abi = api->abiVersion;
            if (in.selfBlast && in.lineOfFire) in.selfBlastServed = SelfBlastServed(api, a_follower);   // before any Repoint with the bits
            in.summonsBlock = Config::g_areaSummonsBlock.load();

            for (;;) {   // a second pass only after the self blast bits were struck
                std::uint32_t    want = 0, have = 0;
                APMF_API::Handle h = APMF_API::kInvalidHandle;
                bool stale = false;
                {
                    std::scoped_lock lock(g_guardMx);
                    const auto it = g_guards.find(a_follower);
                    if (it == g_guards.end()) return;
                    auto& g = it->second;
                    in.refused = g_refusedBits;   // a guard Harbinger refused this session is not asked again
                    want = CastGuardBits::Want(in);
                    h    = g.handle;
                    have = g.bits;
                    if (want == 0) {
                        // No guard applies this lap: no claim (a Repoint with no bit is refused
                        // by Harbinger, so the handle is released instead).
                        g_guards.erase(it);
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
                if ((want ^ have) & CastGuardBits::kSelfBlast)
                    spdlog::info("[cast-guard] {:08X}: self blast party hold {} (the guard now asks {})", a_follower,
                                 (want & CastGuardBits::kSelfBlast) ? "ON" : "off", BitsName(want));
                APMF_API::APMF_Param p{};
                p.ival = static_cast<std::int32_t>(want);
                p.fval = kGuardWindowSecs;
                if (h != APMF_API::kInvalidHandle && !stale) {
                    // Heartbeat: Repoint renews the window and re-reads ival (an MCM change
                    // rides it).
                    api->Repoint(h, &p);
                    if (have != want) {
                        std::scoped_lock lock(g_guardMx);
                        if (auto it = g_guards.find(a_follower); it != g_guards.end() && it->second.handle == h)
                            it->second.bits = want;
                    }
                    return;
                }
                if (stale) api->Release(h);   // harmless on a handle Harbinger already dropped
                const APMF_API::Handle nh = api->RequestEx(a_follower, APMF_API::kIntent_CastGuard, kOwnBasis, &p);
                std::scoped_lock lock(g_guardMx);
                auto& g = g_guards[a_follower];
                if (nh != APMF_API::kInvalidHandle) {
                    g.handle = nh;
                    g.bits = want;
                    g.mintedAt = std::chrono::steady_clock::now();
                    if (std::exchange(g.announced, true)) return;
                    spdlog::info("[cast-guard] {:08X}: ch.25 guard CLAIMED {} (h={}, window {:.0f} s)", a_follower,
                                 BitsName(want), nh, kGuardWindowSecs);
                    return;
                }
                g.handle = APMF_API::kInvalidHandle;
                g.bits = 0;
                // NEVER MASKED, NEVER WORKED AROUND: no MFO hook stands in (client mode).
                // Harbinger refuses on configuration (CheckCast gate not installed, the
                // line-of-fire measurement not armed), which a load does not change, so
                // the refused guard is struck for the session and said once. The self blast
                // bits are struck first and ALONE (the rest is asked again at once); else LineOfFire
                // (with Bystanders, which shares its arming rule) is struck first when the
                // channel bound was asked too, so the channel bound is asked again alone
                // next lap.
                const std::uint32_t strike = CastGuardBits::Strike(want);
                g_refusedBits |= strike;
                spdlog::error("[cast-guard] Harbinger REFUSED the ch.25 cast guard {} (asked {} for {:08X}) -- {} is "
                              "INACTIVE for this session (APMF's log names why). No MFO hook stands in.",
                              BitsName(strike), BitsName(want), a_follower, BitsName(strike));
                if ((strike & CastGuardBits::kSelfBlast) == 0) return;
            }
        }
    }

    void ServiceCastGuard(RE::FormID a_follower, bool a_lineOfFire, bool a_channelBound, bool a_selfBlast) {
        const auto* api = GuardApi();
        if (!api || a_follower == 0 || a_follower == 0x14) return;   // Harbinger refuses the player
        {
            std::scoped_lock lock(g_guardMx);
            auto& in = g_guards[a_follower].in;
            in.lineOfFire   = a_lineOfFire;
            in.channelBound = a_channelBound;
            in.selfBlast    = a_selfBlast;
        }
        Apply(api, a_follower);
    }

    void SetSelfBlastGuard(RE::FormID a_follower, bool a_on) {
        const auto* api = GuardApi();
        if (!api) return;
        {
            std::scoped_lock lock(g_guardMx);
            const auto it = g_guards.find(a_follower);
            if (it == g_guards.end() || it->second.in.selfBlast == a_on) return;   // no guard serviced yet: the next lap's service carries it
            it->second.in.selfBlast = a_on;
        }
        Apply(api, a_follower);
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
