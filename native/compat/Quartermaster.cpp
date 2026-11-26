#include "compat/Quartermaster.h"

#include "Config.h"

// <windows.h> is banned outside Board.cpp (it #defines GetObject, ENGINE_NOTES §9): the two Win32 calls, by hand,
// as APMFBridge and Diagnostics do. void* is ABI-correct for HMODULE on Win64.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_procName);

namespace MFO::QuartermasterCompat {

    namespace {
        using VersionFn = std::uint32_t (*)();
        using ItemFn    = bool (*)(std::uint32_t, std::uint32_t);
        using ActorFn   = bool (*)(std::uint32_t);

        struct Api {
            ItemFn  keepsItem    = nullptr;
            ActorFn dressesArmor = nullptr;
        };

        // Resolved once, on first use (every SKSE plugin is loaded by then: the first query comes from a worker
        // scan, after kDataLoaded). Thread-safe by the function-local static rule.
        const Api& Get() {
            static const Api api = [] {
                Api a;
                void* const qm = ::GetModuleHandleA("Quartermaster.dll");
                if (!qm) {
                    spdlog::info("[qm-compat] Quartermaster not loaded -- no Quartermaster awareness");
                    return a;
                }
                const auto version = reinterpret_cast<VersionFn>(::GetProcAddress(qm, "QM_ApiVersion"));
                if (!version || version() < 1) {
                    spdlog::info("[qm-compat] Quartermaster loaded without its API (needs 1.1.2+) -- no Quartermaster awareness");
                    return a;
                }
                a.keepsItem    = reinterpret_cast<ItemFn>(::GetProcAddress(qm, "QM_KeepsItem"));
                a.dressesArmor = reinterpret_cast<ActorFn>(::GetProcAddress(qm, "QM_DressesArmor"));
                spdlog::info("[qm-compat] Quartermaster API v{}: its set pieces are never sold, and a follower it dresses "
                             "is Fashionrim for armor", version());
                return a;
            }();
            return api;
        }
    }

    bool Present() { return Get().keepsItem != nullptr; }

    bool KeepsItem(RE::FormID a_follower, RE::FormID a_item) {
        const auto& api = Get();
        return api.keepsItem && a_follower && a_item && api.keepsItem(a_follower, a_item);
    }

    bool DressesArmor(RE::FormID a_follower) {
        const auto& api = Get();
        return api.dressesArmor && a_follower && api.dressesArmor(a_follower);
    }

    bool DollsFor(RE::FormID a_follower) {
        return Config::g_dollsMode.load() || DressesArmor(a_follower);
    }
}
