// apmf/AimClaims.cpp -- the ch.29 PROJECTILE AIM claim table (Harbinger ABI v36,
// kIntent_ProjectileAim): MFO's client side of card H5 (marth 2026-11-10, "Ranger at most 3
// misses in 10; Jaqspur at most 1 in 10, never-miss is acceptable"). A new claim family, so its
// own file in apmf/; the shape mirrors apmf/PursuitLeash.cpp.
//
// WHAT MFO DECLARES (policy only; the mechanism is Harbinger's). For every managed follower whose
// EQUIPPED class (classes/State.cpp, GetEquipped) is one of the ranged classes, MFO files ONE
// standing claim whose param.ival is that class's MISS BUDGET over the follower's last 10 shots:
//   Archer   iAimBudgetArcher      (default 5)   -- detection + correction, no fixed guarantee
//   Scout    iAimBudgetScout       (default 4)
//   Witchhunter iAimBudgetWitchhunter (default 4)
//   Ranger   iAimBudgetRanger      (default 3)   -- marth: at most 3 misses in 10
//   Jaqspur  iAimBudgetJaqspur     (default 1)   -- marth: at most 1 in 10 (0 = never misses)
// Harbinger counts the follower's launches and TESHitEvents and, when the window's misses (plus
// shots in flight) reach the budget, re-aims the next launch at the engine's own unscattered
// solution (its lead point and ballistic lift, no random offset, no Archery cone). MFO never
// touches a projectile, an angle or an animation: it only says WHO and HOW MANY.
//
// LIFETIME: filed by the pump's sweep when the follower is managed, the class add-on is active
// (Classes::Active: the ESL present and bClassTree on) and the equipped class is ranged; re-filed
// when the class (and so the budget) changes; released when the class stops being ranged, the
// follower is dismissed (Followers::ReleaseHeldState), the add-on goes inactive, bAimBudget is
// off, and by ClearTransientState (load / revert). A claim Harbinger ENDS (the follower unloaded)
// or that never goes live is released and re-tried after a backoff, never in a loop. Harbinger
// absent / older than v36 / the seat refused = no claim: the engine aims as it always did.
// Threads: the pump's AddTask worker for everything but ClearAimClaims (the drained-pump load
// window); its OWN mutex, never g_mx (same lock-order note as apmf/ReentryDeny.cpp).
// PROBE ([aim], bAimProbe, default OFF): one GetProjectileAimState line per claimed follower every
// 30 s (shots / corrected / hits / misses / window), so a Deck session proves the mechanism.
#include "APMFBridge.h"
#include "APMFBridge_internal.h"
#include "APMF_API.h"
#include "Config.h"
#include "Followers.h"           // ActiveSnapshot(): the managed followers, any thread
#include "classes/Classes.h"    // Classes::Active(), GetEquipped(), Id

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace MFO::APMFBridge {

    namespace {
        constexpr std::uint32_t kAimMinAbi = 36;   // the ABI that first serves intent 29
        constexpr RE::FormID    kPlayerId  = 0x14;
        constexpr std::int32_t  kNotRanged = -1;

        // Same never-live floor as the sibling tables; a longer backoff before a re-file (~30 s of
        // 133 ms sweeps) once Harbinger ended a claim or it never went live.
        constexpr std::uint32_t kNeverLiveSweeps = 15;
        constexpr std::uint32_t kRetrySweeps     = 225;
        constexpr std::uint32_t kProbeEveryMs    = 30000;

        struct AimClaim {
            APMF_API::Handle handle         = APMF_API::kInvalidHandle;
            std::int32_t     budget         = kNotRanged;
            std::uint8_t     cls            = Classes::kNoClass;
            std::uint32_t    unpausedSweeps = 0;
            std::uint32_t    retryIn        = 0;      // > 0: do not re-file yet (sweeps)
            bool             everLive       = false;
            std::uint64_t    lastProbeMs    = 0;
        };
        std::mutex                               g_aimMx;
        std::unordered_map<RE::FormID, AimClaim> g_aims;   // guarded by g_aimMx

        std::atomic<bool> g_aimSeatRefused{ false };   // session-stable, like the sibling tables

        const APMF_API::APMF_API_v36* AimApi() {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (!api || api->abiVersion < kAimMinAbi) return nullptr;
            return reinterpret_cast<const APMF_API::APMF_API_v36*>(api);
        }

        bool AimGamePaused() {
            auto* ui = RE::UI::GetSingleton();
            return ui && ui->GameIsPaused();
        }

        std::uint64_t NowMs() {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        // The class -> budget policy. kNotRanged for any class without a shot budget.
        std::int32_t BudgetFor(std::uint8_t a_cls) {
            if (a_cls == Classes::kNoClass) return kNotRanged;
            const auto id = static_cast<Classes::Id>(a_cls);
            int b = kNotRanged;
            switch (id) {
            case Classes::Id::Archer:      b = Config::g_aimBudgetArcher.load(std::memory_order_relaxed); break;
            case Classes::Id::Scout:       b = Config::g_aimBudgetScout.load(std::memory_order_relaxed); break;
            case Classes::Id::Witchhunter: b = Config::g_aimBudgetWitchhunter.load(std::memory_order_relaxed); break;
            case Classes::Id::Ranger:      b = Config::g_aimBudgetRanger.load(std::memory_order_relaxed); break;
            case Classes::Id::Jaqspur:     b = Config::g_aimBudgetJaqspur.load(std::memory_order_relaxed); break;
            default:                       return kNotRanged;
            }
            return b < 0 ? 0 : (b > 10 ? 10 : b);
        }

        std::string ClassName(std::uint8_t a_cls) {
            if (a_cls == Classes::kNoClass) return "none";
            const auto& def = Classes::Get(static_cast<Classes::Id>(a_cls));
            return def.name.empty() ? std::string("?") : std::string(def.name);
        }

        void ReleaseLocked(const APMF_API::APMF_API_v36* a_api, RE::FormID a_fid, AimClaim& a_c, const char* a_why) {
            if (a_api && a_c.handle != APMF_API::kInvalidHandle) a_api->Release(a_c.handle);
            spdlog::info("[aim] {:08X}: ch.29 aim claim released ({}, h={}, budget {})", a_fid, a_why, a_c.handle, a_c.budget);
            a_c.handle   = APMF_API::kInvalidHandle;
            a_c.budget   = kNotRanged;
            a_c.everLive = false;
            a_c.unpausedSweeps = 0;
        }

        bool FileLocked(const APMF_API::APMF_API_v36* a_api, RE::FormID a_fid, AimClaim& a_c, std::uint8_t a_cls, std::int32_t a_budget) {
            APMF_API::APMF_Param prm{};
            prm.ival                 = a_budget;
            const APMF_API::Handle h = a_api->RequestEx(a_fid, APMF_API::kIntent_ProjectileAim, kOwnBasis, &prm);
            if (h == APMF_API::kInvalidHandle) {
                if (!g_aimSeatRefused.exchange(true))
                    spdlog::warn("[aim] Harbinger REFUSED the ch.29 projectile-aim claim for {:08X} at the call: the seat is not "
                                 "installed (VR, a runtime other than 1.6.1170 / 1.5.97, [ProjectileAim] bProjectileAim=0 in "
                                 "APMF.ini, or its address self-check; APMF's '[ch.29] claim refused' line names it). No miss "
                                 "budget this session.", a_fid);
                return false;
            }
            a_c.handle         = h;
            a_c.budget         = a_budget;
            a_c.cls            = a_cls;
            a_c.everLive       = false;
            a_c.unpausedSweeps = 0;
            a_c.retryIn        = 0;
            spdlog::info("[aim] {:08X}: ch.29 aim claim FILED (h={}, class {}, budget {} miss{} in 10{})", a_fid, h,
                         ClassName(a_cls), a_budget, a_budget == 1 ? "" : "es",
                         a_budget == 0 ? ": every shot corrected" : (a_budget >= 10 ? ": observe only" : ""));
            return true;
        }
    }

    void ReleaseAimClaim(RE::FormID a_follower, const char* a_why) {
        APMF_API::Handle h = APMF_API::kInvalidHandle;
        {
            std::scoped_lock lock(g_aimMx);
            const auto it = g_aims.find(a_follower);
            if (it == g_aims.end()) return;
            h = it->second.handle;
            g_aims.erase(it);
        }
        if (auto* api = g_apmf.load(std::memory_order_relaxed); api && h != APMF_API::kInvalidHandle) api->Release(h);
        if (h != APMF_API::kInvalidHandle) spdlog::info("[aim] {:08X}: ch.29 aim claim released ({})", a_follower, a_why);
    }

    // Tick()'s pass (declared in APMFBridge_internal.h). Takes g_aimMx itself; g_mx NOT held.
    void SweepAimClaims() {
        const auto* api = AimApi();
        if (!api) return;
        const bool on = Config::g_aimBudget.load(std::memory_order_relaxed) && Classes::Active() &&
                        !g_aimSeatRefused.load(std::memory_order_relaxed);
        const auto snap   = Followers::ActiveSnapshot();
        const bool paused = AimGamePaused();
        const auto now    = NowMs();
        std::scoped_lock lock(g_aimMx);
        if (!on) {
            for (auto& [fid, c] : g_aims)
                if (c.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, fid, c, "miss budget off / class add-on inactive");
            return;
        }
        // 1. The managed followers: file, re-file on a class change, release when no longer ranged.
        for (const RE::FormID fid : *snap) {
            if (fid == 0 || fid == kPlayerId) continue;
            const std::uint8_t cls    = Classes::GetEquipped(fid);
            const std::int32_t budget = BudgetFor(cls);
            auto& c                   = g_aims[fid];
            if (budget == kNotRanged) {
                if (c.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, fid, c, "class no longer ranged");
                c.cls = cls;
                continue;
            }
            if (c.handle != APMF_API::kInvalidHandle) {
                if (c.budget == budget) continue;
                ReleaseLocked(api, fid, c, "budget changed (class or INI)");
            }
            if (c.retryIn > 0) { if (!paused) --c.retryIn; continue; }
            if (!FileLocked(api, fid, c, cls, budget)) return;   // refused at the call: session-stable, stop here
        }
        // 2. Claims for followers no longer managed.
        for (auto it = g_aims.begin(); it != g_aims.end();) {
            const bool managed = std::find(snap->begin(), snap->end(), it->first) != snap->end();
            if (!managed) {
                if (it->second.handle != APMF_API::kInvalidHandle) ReleaseLocked(api, it->first, it->second, "not managed");
                it = g_aims.erase(it);
            } else ++it;
        }
        // 3. Liveness: never-live and ended-by-Harbinger claims are released and re-tried after a backoff.
        for (auto& [fid, c] : g_aims) {
            if (c.handle == APMF_API::kInvalidHandle) continue;
            if (api->IsClaimLive(c.handle)) {
                c.everLive = true;
                if (Config::g_aimProbe.load(std::memory_order_relaxed) && now - c.lastProbeMs >= kProbeEveryMs) {
                    c.lastProbeMs = now;
                    APMF_API::APMF_ProjectileAimInfo info{};
                    info.size        = sizeof(info);
                    const auto state = api->GetProjectileAimState(fid, &info);
                    spdlog::info("[aim] {:08X} ({}) state {} budget {}: shots {} corrected {} hits {} hitsOther {} misses {} pending {} "
                                 "window {}/{} misses | uncorrectable {} wouldCorrect {} rankStood {}",
                                 fid, ClassName(c.cls), state, info.budget, info.shots, info.corrected, info.hits, info.hitsOther,
                                 info.misses, info.pending, info.windowMisses, info.windowFill, info.uncorrectable, info.wouldCorrect,
                                 info.rankStood);
                }
                continue;
            }
            if (!c.everLive) {
                if (!paused) ++c.unpausedSweeps;
                if (c.unpausedSweeps <= kNeverLiveSweeps) continue;
                spdlog::warn("[aim] {:08X}: ch.29 aim claim NEVER WENT LIVE (h={}, {} unpaused sweeps; he is probably not loaded) "
                             "-- released, re-tried in ~30 s. APMF's log says why if it refused.",
                             fid, c.handle, c.unpausedSweeps);
            } else {
                spdlog::info("[aim] {:08X}: ch.29 aim claim ENDED BY HARBINGER (h={}: he unloaded, or an outranking claim took him) "
                             "-- re-tried in ~30 s.", fid, c.handle);
            }
            ReleaseLocked(api, fid, c, "not live");
            c.retryIn = kRetrySweeps;
        }
    }

    void ClearAimClaims() {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        std::scoped_lock lock(g_aimMx);
        for (auto& [fid, c] : g_aims)
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
        g_aims.clear();
    }

}
