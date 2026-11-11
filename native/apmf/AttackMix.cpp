// apmf/AttackMix.cpp -- the ch.18 ATTACK SELECTION claim table (Harbinger ABI v34, kIntent_AttackSelection): MFO's client side of
// class combat packages (card C8, marth 2026-11-09 walkthrough: "Knight power share max 0.25 ... Mercenary power cap 15% ...
// Warrior power FLOOR 40%"). A new claim family, so its own file in apmf/; the shape mirrors apmf/AimClaims.cpp.
//
// WHAT MFO DECLARES (policy only; the mechanism is Harbinger's, DENY ONLY): for every managed follower whose PACKAGE class
// (Classes::PackageClass: his equipped TIER 2+ class, Dragonborn = his picked T4) carries a power-share rule in the class table
// (ClassDef::powerShareMax / powerShareMin, classes/ClassTable.cpp), ONE standing claim whose declaration governs the Normal and
// Power attack classes and denies neither outright:
//   powerShareMax  a CEILING on Power / (Power + Normal) over the follower's last 8 started attacks (Knight 0.25, Great Knight 0.20,
//                  Mercenary / Blademaster 0.15, Thief / Master Thief 0.20, Listener 0.25, Ebony Warrior 0.20). While above it, Power is denied.
//   powerShareMin  a FLOOR on the same share (Warrior / Champion / Living Legend 0.40). While below it, Normal is denied; Harbinger lifts
//                  that deny for a roll in which no undenied Power attack would be drawn (marth: a regular attack must fire when
//                  stamina cannot pay for a power attack), and never leaves a roll empty (the floor rule).
// Bash, BashPower, Left and DualWield are NOT governed: bash stays in every class's mix this release (marth Q38). MFO never picks
// a move, a target or a weapon. A class without a rule files nothing (T1 included).
//
// LIFETIME: filed by the pump's sweep (APMFBridge::Tick) when the follower is managed, the class add-on is active (Classes::Active),
// bClassAttackMix is on and his package class has a rule; re-declared (released and re-filed) when the rule changes; released when the
// class is unequipped / classes go OFF / the tree toggles off / bClassAttackMix goes off, on dismissal (Followers::ReleaseHeldState),
// and by ClearTransientState (load / revert, after StopPump). A claim Harbinger ENDS (he unloaded) or that never goes live is released
// and re-tried after a backoff, never in a loop. Harbinger absent / older than v34 / the seat refused = no mix: the engine draws its
// own attacks (the CSTY half still applies; logged once).
// Threads: the pump's AddTask worker for everything but ClearAttackMixClaims (the drained-pump load window); its OWN mutex, never
// g_mx (same lock-order note as apmf/AimClaims.cpp).
// PROBE ([mix], bAttackMixProbe, default OFF): one GetAttackMixState line per claimed follower every 30 s.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"           // ActiveSnapshot(): the managed followers, any thread
#include "classes/Classes.h"    // Classes::Active(), PackageClass(), Get()

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kMixMinAbi = 34;   // the ABI that first serves intent 18
        constexpr RE::FormID    kPlayerId  = 0x14;

        constexpr std::uint32_t kNeverLiveSweeps = 15;
        constexpr std::uint32_t kRetrySweeps     = 225;   // ~30 s of 133 ms sweeps
        constexpr std::uint32_t kProbeEveryMs    = 30000;

        struct MixClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;
            std::uint8_t     cls            = Classes::kNoClass;   // the package class the declaration was made for
            float            max            = Classes::kShareUnset;
            float            min            = Classes::kShareUnset;
            std::uint32_t    unpausedSweeps = 0;
            std::uint32_t    retryIn        = 0;      // > 0: do not re-file yet (sweeps)
            bool             everLive       = false;
            std::uint64_t    lastProbeMs    = 0;
        };
        std::mutex                                g_mixMx;
        std::unordered_map<RE::FormID, MixClaim>  g_mixes;   // guarded by g_mixMx

        std::atomic<bool> g_mixSeatRefused{ false };   // session-stable, like the sibling tables
        std::atomic<bool> g_mixAbiLogged{ false };

        const APMF_API::APMF_API_v34* MixApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api) return nullptr;
            if (api->abiVersion < kMixMinAbi) {
                if (!g_mixAbiLogged.exchange(true) && Classes::Active() && Config::g_classAttackMix.load(std::memory_order_relaxed))
                    spdlog::info("[mix] Harbinger ABI {} is older than {}: the class ch.18 attack mix is not applied (the class CSTY "
                                 "and leash still are)", api->abiVersion, kMixMinAbi);
                return nullptr;
            }
            return reinterpret_cast<const APMF_API::APMF_API_v34*>(api);
        }

        bool MixGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        std::uint64_t NowMs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        bool HasRule(float a_max, float a_min) { return a_max > 0.0f || a_min > 0.0f; }

        std::string ClassName(std::uint8_t a_cls) {
            if (a_cls == Classes::kNoClass) return "none";
            const auto& def = Classes::Get(static_cast<Classes::Id>(a_cls));
            return def.name.empty() ? std::string("?") : std::string(def.name);
        }

        void ReleaseLocked(const APMF_API::APMF_API_v34* a_api, RE::FormID a_fid, MixClaim& a_c, const char* a_why) {
            if (a_api && a_c.handle != APMF_API::kInvalidHandle) a_api->Release(a_c.handle);
            spdlog::info("[mix] {:08X}: ch.18 attack mix released ({}, h={}, class {})", a_fid, a_why, a_c.handle, ClassName(a_c.cls));
            a_c.handle   = APMF_API::kInvalidHandle;
            a_c.cls      = Classes::kNoClass;
            a_c.max      = Classes::kShareUnset;
            a_c.min      = Classes::kShareUnset;
            a_c.everLive = false;
            a_c.unpausedSweeps = 0;
        }

        // g_mixMx HELD. false = the seat refused at the call (session-stable: stop the pass).
        bool FileLocked(const APMF_API::APMF_API_v34* a_api, RE::FormID a_fid, MixClaim& a_c, std::uint8_t a_cls, float a_max, float a_min) {
            const APMF_API::Handle h = a_api->Request(a_fid, APMF_API::kIntent_AttackSelection, kOwnBasis);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_mixSeatRefused.exchange(true))
                    spdlog::warn("[mix] Harbinger REFUSED the ch.18 attack-selection claim for {:08X} at the call: the seat is not "
                                 "installed ([AttackSelect] bAttackSelect=0 in APMF.ini, VR, an unsupported runtime, or its address/byte "
                                 "check; APMF's log names it). No class attack mix this session.", a_fid);
                return false;
            }
            APMF_API::APMF_AttackMix mix{};
            mix.size          = sizeof(mix);
            mix.governed      = APMF_API::kAtkClass_Normal | APMF_API::kAtkClass_Power;   // Bash / Left stay the engine's (marth Q38)
            mix.denied        = 0;                                                        // deny-only through the share rule, nothing outright
            mix.powerShareMax = a_max > 0.0f ? a_max : -1.0f;
            mix.powerShareMin = a_min > 0.0f ? a_min : -1.0f;
            mix.window        = 0;   // Harbinger's default (8)
            a_api->SetAttackMix(h, &mix);
            a_c.handle         = h;
            a_c.cls            = a_cls;
            a_c.max            = a_max;
            a_c.min            = a_min;
            a_c.everLive       = false;
            a_c.unpausedSweeps = 0;
            a_c.retryIn        = 0;
            spdlog::info("[mix] {:08X}: ch.18 attack mix FILED (h={}, class {}, power share ceiling {:.2f} floor {:.2f}; -1 = none)", a_fid, h,
                         ClassName(a_cls), mix.powerShareMax, mix.powerShareMin);
            return true;
        }
    }

    void ReleaseAttackMix(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_mixMx);
            const auto it = g_mixes.find(a_follower);
            if (it == g_mixes.end()) return;
            h = it->second.handle;
            g_mixes.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        if (h != APMF_API::kInvalidHandle) spdlog::info("[mix] {:08X}: ch.18 attack mix released ({})", a_follower, a_why);
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_mixMx itself; g_mx NOT held.
    void SweepAttackMixClaims() {
        const auto* api = MixApi();
        if (!api) return;
        const bool on = Config::g_classAttackMix.load(std::memory_order_relaxed) && Classes::Active() &&
                        !g_mixSeatRefused.load(std::memory_order_relaxed);
        const auto snap   = Followers::ActiveSnapshot();
        const bool paused = MixGamePaused();
        const auto now    = NowMs();
        std::scoped_lock lock(g_mixMx);
        if (!on) {
            for (auto& [fid, c] : g_mixes)
                if (c.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, fid, c, "class attack mix off / class add-on inactive");
            return;
        }
        // 1. The managed followers: file, re-declare on a class change, release when the class carries no rule.
        for (const RE::FormID fid : *snap) {
            if (fid == 0 || fid == kPlayerId) continue;
            const std::uint8_t cls = Classes::PackageClass(fid);   // kNoClass = classes OFF / tier 1 / unequipped
            float wantMax = Classes::kShareUnset, wantMin = Classes::kShareUnset;
            if (cls != Classes::kNoClass) {
                const auto& def = Classes::Get(static_cast<Classes::Id>(cls));
                wantMax = def.powerShareMax;
                wantMin = def.powerShareMin;
            }
            auto& c = g_mixes[fid];
            if (!HasRule(wantMax, wantMin)) {
                if (c.handle != APMF_API::kInvalidHandle)
                    ReleaseLocked(api, fid, c, cls == Classes::kNoClass ? "no package class (unequipped / classes off / tier 1)"
                                                                         : "class carries no power-share rule");
                continue;
            }
            if (c.handle != APMF_API::kInvalidHandle) {
                if (c.cls == cls && c.max == wantMax && c.min == wantMin) continue;
                ReleaseLocked(api, fid, c, "class changed");
            }
            if (c.retryIn > 0) { if (!paused) --c.retryIn; continue; }
            if (!FileLocked(api, fid, c, cls, wantMax, wantMin)) return;   // refused at the call: session-stable, stop here
        }
        // 2. Claims for followers no longer managed (dismissed, dead, gone).
        for (auto it = g_mixes.begin(); it != g_mixes.end();) {
            const bool managed = std::find(snap->begin(), snap->end(), it->first) != snap->end();
            if (!managed) {
                if (it->second.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, it->first, it->second, "not managed");
                it = g_mixes.erase(it);
            } else ++it;
        }
        // 3. Liveness: never-live and ended-by-Harbinger claims are released and re-tried after a backoff.
        for (auto& [fid, c] : g_mixes) {
            if (c.handle == APMF_API::kInvalidHandle) continue;
            if (api->IsClaimLive(c.handle)) {
                c.everLive = true;
                if (Config::g_attackMixProbe.load(std::memory_order_relaxed) && now - c.lastProbeMs >= kProbeEveryMs) {
                    c.lastProbeMs = now;
                    APMF_API::APMF_AttackMixInfo info{};
                    info.size        = sizeof(info);
                    const auto state = api->GetAttackMixState(fid, &info);
                    spdlog::info("[mix] {:08X} ({}) state {} governed {:#x} denied {:#x} activeDeny {:#x} ceiling {:.2f} floor {:.2f} "
                                 "window {}: fill {} power {} normal {} share {:.2f} | zeroed {} floorStood {} rankStood {} wouldDeny {}",
                                 fid, ClassName(c.cls), state, info.governed, info.denied, info.activeDeny, info.powerShareMax,
                                 info.powerShareMin, info.window, info.windowFill, info.windowPower, info.windowNormal, info.powerShare,
                                 info.zeroed, info.floorStood, info.rankStood, info.wouldDeny);
                }
                continue;
            }
            if (!c.everLive) {
                if (!paused) ++c.unpausedSweeps;
                if (c.unpausedSweeps <= kNeverLiveSweeps) continue;
                spdlog::warn("[mix] {:08X}: ch.18 attack mix NEVER WENT LIVE (h={}, {} unpaused sweeps; he is probably not loaded) -- "
                             "released, re-tried in ~30 s. APMF's log says why if it refused.", fid, c.handle, c.unpausedSweeps);
            } else {
                spdlog::info("[mix] {:08X}: ch.18 attack mix ENDED BY HARBINGER (h={}: he unloaded, or an outranking claim took him) -- "
                             "re-tried in ~30 s.", fid, c.handle);
            }
            ReleaseLocked(api, fid, c, "not live");
            c.retryIn = kRetrySweeps;
        }
    }

    void ClearAttackMixClaims() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_mixMx);
        for (auto& [fid, c] : g_mixes)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_mixes.clear();
    }

}
