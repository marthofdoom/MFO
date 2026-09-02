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
        // The APMF interface (v2, so RequestEx is present). Written ONCE at
        // kDataLoaded (main) before any worker tick runs; read on the worker
        // (SelectSpell/Tick). Correct by ordering already, but atomic (relaxed is
        // enough -- it is a single pointer with no dependent state) enforces the
        // cross-thread publish/consume contract cheaply.
        std::atomic<const APMF_API::APMF_API_v2*> g_apmf{ nullptr };

        // MFO's cast-select claims: one per follower. Guarded by g_mx (worker +
        // main). `spell` is the FormID currently selected; `handle` the APMF claim;
        // `refreshed` the last SelectSpell time (drives Tick's auto-release).
        struct Claim {
            APMF_API::Handle                      handle = APMF_API::kInvalidHandle;
            RE::FormID                            spell  = 0;
            std::chrono::steady_clock::time_point refreshed{};
        };
        std::mutex                                  g_mx;
        std::unordered_map<RE::FormID, Claim>       g_claims;

        // MFO outbids APMF's mid-range test hotkey (basis 100) so a real gambit wins
        // the hand over a tester's Numpad4. Arbitrary among clients; > the test basis.
        constexpr float kCastSelectBasis = 200.0f;

        // A cast claim not refreshed within this window is released (the gambit
        // stopped choosing a cast). ~4 pumps at kPumpMs=133 -- rides out a tick or
        // two where another rule wins without thrashing release/re-request.
        constexpr auto kExpiry = std::chrono::milliseconds(500);

        // Release one claim's handle (APMF Release is thread-safe + no-ops a stale
        // handle). Caller holds g_mx. Does not erase the map entry.
        void ReleaseLocked(Claim& c) {
            auto* api = g_apmf.load(std::memory_order_relaxed);
            if (api && c.handle != APMF_API::kInvalidHandle) api->Release(c.handle);
            c.handle = APMF_API::kInvalidHandle;
        }
    }

    void Acquire() {
        g_apmf.store(nullptr, std::memory_order_relaxed);
        void* h = GetModuleHandleA("APMF.dll");
        if (!h) {
            spdlog::info("[apmf] interface absent -- APMF.dll not in the load order; "
                         "cast-selection assist OFF (MFO casting unchanged).");
            return;
        }
        auto fn = reinterpret_cast<APMF_API::GetInterface_t>(
            GetProcAddress(h, APMF_API::kGetInterfaceExport));
        if (!fn) {
            spdlog::warn("[apmf] APMF.dll present but '{}' not exported -- cast-selection assist OFF.",
                         APMF_API::kGetInterfaceExport);
            return;
        }
        const APMF_API::APMF_API_v1* base = fn(APMF_API::kABIVersion);
        if (!base) {
            spdlog::warn("[apmf] APMF refused ABI v{} (too old) -- cast-selection assist OFF.",
                         APMF_API::kABIVersion);
            return;
        }
        if (base->abiVersion < 2) {
            spdlog::warn("[apmf] APMF ABI v{} has no RequestEx (need >= 2) -- cast-selection assist OFF.",
                         base->abiVersion);
            return;
        }
        auto* api = reinterpret_cast<const APMF_API::APMF_API_v2*>(base);
        g_apmf.store(api, std::memory_order_relaxed);
        spdlog::info("[apmf] interface acquired (ABI v{}) -- cast-selection assist enabled.",
                     api->abiVersion);
    }

    bool Available() { return g_apmf.load(std::memory_order_relaxed) != nullptr; }

    void SelectSpell(RE::FormID a_follower, RE::FormID a_spell) {
        auto* api = g_apmf.load(std::memory_order_relaxed);
        if (!api || a_follower == 0 || a_spell == 0) return;
        if (!Config::g_apmfCast.load()) return;

        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        auto& c = g_claims[a_follower];

        if (c.handle != APMF_API::kInvalidHandle && c.spell == a_spell) {
            c.refreshed = now;   // same spell still wanted: just keep it alive
            return;
        }
        if (c.handle != APMF_API::kInvalidHandle) {
            ReleaseLocked(c);    // gambit switched spells: drop the old selection first
        }

        APMF_API::APMF_Param param{};
        param.form = a_spell;
        c.handle    = api->RequestEx(a_follower, APMF_API::kIntent_SelectSpell,
                                     kCastSelectBasis, &param);
        c.spell     = a_spell;
        c.refreshed = now;
        if (c.handle == APMF_API::kInvalidHandle) {
            g_claims.erase(a_follower);   // APMF refused (no cast-select channel?) -- forget it
        }
    }

    void ReleaseSpell(RE::FormID a_follower) {
        std::scoped_lock lock(g_mx);
        auto it = g_claims.find(a_follower);
        if (it == g_claims.end()) return;
        ReleaseLocked(it->second);
        g_claims.erase(it);
    }

    void Tick() {
        if (!g_apmf.load(std::memory_order_relaxed)) return;
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock(g_mx);
        for (auto it = g_claims.begin(); it != g_claims.end();) {
            if (now - it->second.refreshed >= kExpiry) {
                ReleaseLocked(it->second);
                it = g_claims.erase(it);
            } else {
                ++it;
            }
        }
    }

    void ClearTransientState() {
        std::scoped_lock lock(g_mx);
        for (auto& [id, c] : g_claims) ReleaseLocked(c);
        g_claims.clear();
    }
}
