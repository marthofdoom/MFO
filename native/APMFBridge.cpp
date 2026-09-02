#include "APMFBridge.h"
#include "APMF_API.h"
#include "Config.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

#include <spdlog/spdlog.h>

// Two Win32 symbols, declared by hand. <windows.h> is BANNED outside Board.cpp --
// it #defines GetObject and hijacks BGSDefaultObjectManager::GetObject<T>
// (ENGINE_NOTES §9; the same reason Targeting.cpp / Logistics.cpp hand-declare
// GetModuleHandleA). Pointer args/return are pointer-sized on Win64, so void* is
// ABI-correct for HMODULE/FARPROC; the real header is never in this TU to conflict.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_procName);

namespace MFO::APMFBridge {

    namespace {
        // The APMF interface (v2, so RequestEx is present). Written ONCE at kDataLoaded
        // (main) before any worker tick runs; read on the worker (OwnHostileCast/Tick).
        // Correct by ordering already, but atomic (relaxed is enough -- a single
        // pointer, no dependent state) enforces the publish/consume contract cheaply.
        std::atomic<const APMF_API::APMF_API_v2*> g_apmf{ nullptr };

        // MFO's owned-cast claims: per follower, the cast-select (spell) claim + the
        // combat-target (target) claim, and the last-refresh time (drives Tick's
        // auto-release). Guarded by g_mx (worker + main).
        struct Owned {
            APMF_API::Handle spellHandle  = APMF_API::kInvalidHandle;  RE::FormID spell  = 0;
            APMF_API::Handle targetHandle = APMF_API::kInvalidHandle;  RE::FormID target = 0;
            std::chrono::steady_clock::time_point refreshed{};
        };
        std::mutex                             g_mx;
        std::unordered_map<RE::FormID, Owned>  g_owned;

        // MFO outbids APMF's mid-range test hotkeys (basis 100) so a real gambit wins
        // the hand/target over a tester's Numpad keys. Arbitrary among clients; > 100.
        constexpr float kOwnBasis = 200.0f;

        // Claims not refreshed within this window are released (the owned cast gambit
        // stopped winning / lost its target). ~4 pumps at kPumpMs=133 -- rides out a
        // tick or two where another rule wins without thrashing release/re-request.
        constexpr auto kExpiry = std::chrono::milliseconds(500);

        // Ensure ONE channel claim tracks `want` (0 == release it). Caller holds g_mx.
        // APMF Release/RequestEx are thread-safe and no-op a stale handle.
        void EnsureClaimLocked(const APMF_API::APMF_API_v2* api, RE::FormID follower,
                               APMF_API::Intent intent, APMF_API::Handle& handle,
                               RE::FormID& cur, RE::FormID want) {
            if (want == 0) {                                   // no claim wanted
                if (handle != APMF_API::kInvalidHandle) { api->Release(handle); handle = APMF_API::kInvalidHandle; }
                cur = 0;
                return;
            }
            if (handle != APMF_API::kInvalidHandle && cur == want) return;   // already claiming it
            if (handle != APMF_API::kInvalidHandle) api->Release(handle);     // switched -> drop the old
            APMF_API::APMF_Param p{};
            p.form = want;
            handle = api->RequestEx(follower, intent, kOwnBasis, &p);
            cur    = (handle != APMF_API::kInvalidHandle) ? want : 0;
        }

        // Release both of a follower's claims. Caller holds g_mx. Does not erase.
        void ReleaseLocked(Owned& o) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api) {
                if (o.spellHandle  != APMF_API::kInvalidHandle) api->Release(o.spellHandle);
                if (o.targetHandle != APMF_API::kInvalidHandle) api->Release(o.targetHandle);
            }
            o.spellHandle = o.targetHandle = APMF_API::kInvalidHandle;
            o.spell = o.target = 0;
        }
    }

    void Acquire() {
        g_apmf.store(nullptr, std::memory_order_relaxed);
        void* h = GetModuleHandleA("APMF.dll");
        if (!h) {
            spdlog::info("[apmf] interface absent -- APMF.dll not in the load order; "
                         "owned-cast model OFF (MFO falls back to the legacy AI-first cast hybrid).");
            return;
        }
        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) {
            spdlog::warn("[apmf] APMF.dll present but '{}' not exported -- owned-cast model OFF.",
                         APMF_API::kGetInterfaceExport);
            return;
        }
        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) {
            spdlog::warn("[apmf] APMF refused ABI v{} (too old) -- owned-cast model OFF.",
                         APMF_API::kABIVersion);
            return;
        }
        if (base->abiVersion < 2) {
            spdlog::warn("[apmf] APMF ABI v{} has no RequestEx (need >= 2) -- owned-cast model OFF.",
                         base->abiVersion);
            return;
        }
        auto* api = reinterpret_cast<const APMF_API::APMF_API_v2*>(base);
        g_apmf.store(api, std::memory_order_relaxed);
        spdlog::info("[apmf] interface acquired (ABI v{}) -- owned-cast model enabled "
                     "(target+spell owned; AI fires it ANIMATED).", api->abiVersion);
    }

    bool Available() { return g_apmf.load(std::memory_order_relaxed) != nullptr; }

    void OwnHostileCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_spell == 0) return;
        if (!Config::g_apmfCast.load()) return;

        std::scoped_lock lock(g_mx);
        auto& o = g_owned[a_follower];
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_SelectSpell,  o.spellHandle,  o.spell,  a_spell);
        EnsureClaimLocked(api, a_follower, APMF_API::kIntent_CombatTarget, o.targetHandle, o.target, a_target);
        o.refreshed = std::chrono::steady_clock::now();
        if (o.spellHandle == APMF_API::kInvalidHandle && o.targetHandle == APMF_API::kInvalidHandle)
            g_owned.erase(a_follower);   // APMF refused both -- forget it
    }

    void ReleaseCast(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_owned.find(a_follower);
        if (it == g_owned.end()) return;
        ReleaseLocked(it->second);
        g_owned.erase(it);
    }

    void Tick() {
        if (!g_apmf.load(std::memory_order_relaxed)) return;
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        for (auto it = g_owned.begin(); it != g_owned.end();) {
            if (now - it->second.refreshed >= kExpiry) {
                ReleaseLocked(it->second);
                it = g_owned.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ClearTransientState() {
        std::scoped_lock lock(g_mx);
        for (auto& [id, o] : g_owned) ReleaseLocked(o);
        g_owned.clear();
    }
}
