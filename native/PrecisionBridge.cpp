// PrecisionBridge.cpp -- the minimal read-only bridge to Precision (contract, evidence and threading in the header).
#include "PCH.h"
#include "PrecisionBridge.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// Declared by hand, like Targeting.cpp / Diagnostics.cpp do (the PCH never pulls <Windows.h>). void* is
// ABI-correct for HMODULE and FARPROC on Win64.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);
extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* a_module, const char* a_name);

namespace MFO::PrecisionBridge {

    namespace {

        // Precision's InterfaceVersion::V2 and RequestedAttackCollisionType::RightWeapon, as plain numbers (see
        // the header: verified against the published PrecisionAPI.h and the 2.0.4 binary).
        constexpr std::uint8_t kInterfaceV2 = 1;
        constexpr std::uint8_t kRightWeapon = 2;
        constexpr std::size_t  kSlotCapsuleLength = 8;

        using RequestFn = void* (*)(std::uint8_t);
        // slot 8: float (this, ActorHandle by value, uint8). The handle is its 32-bit native value in edx.
        using LengthFn = float (*)(void*, std::uint32_t, std::uint8_t);

        // MAIN THREAD only (resolved on first use, read by RightWeaponLength).
        bool        g_resolved = false;
        void*       g_api = nullptr;
        LengthFn    g_length = nullptr;
        const char* g_state = "absent";

        // [base, base + SizeOfImage) of a loaded module, from its own PE headers.
        bool ImageRange(void* a_module, std::uintptr_t& a_lo, std::uintptr_t& a_hi) {
            const auto base = reinterpret_cast<std::uintptr_t>(a_module);
            if (base == 0) return false;
            std::uint16_t mz = 0;
            std::memcpy(&mz, reinterpret_cast<const void*>(base), sizeof(mz));
            if (mz != 0x5A4D) return false;                                   // "MZ"
            std::int32_t lfanew = 0;
            std::memcpy(&lfanew, reinterpret_cast<const void*>(base + 0x3C), sizeof(lfanew));
            if (lfanew <= 0 || lfanew > 0x1000) return false;
            std::uint32_t sig = 0, size = 0;
            std::memcpy(&sig, reinterpret_cast<const void*>(base + static_cast<std::uintptr_t>(lfanew)), sizeof(sig));
            if (sig != 0x00004550) return false;                              // "PE\0\0"
            // IMAGE_NT_HEADERS64: Signature (4) + FileHeader (20) + OptionalHeader.SizeOfImage at +0x38.
            std::memcpy(&size, reinterpret_cast<const void*>(base + static_cast<std::uintptr_t>(lfanew) + 0x18 + 0x38),
                        sizeof(size));
            if (size == 0) return false;
            a_lo = base;
            a_hi = base + size;
            return true;
        }

        void Resolve() {
            g_resolved = true;
            void* mod = ::GetModuleHandleA("Precision.dll");
            if (!mod) {
                g_state = "absent";
                spdlog::info("[precision] Precision.dll is not loaded -- melee reach stays fMeleeReach");
                return;
            }
            const auto request = reinterpret_cast<RequestFn>(::GetProcAddress(mod, "RequestPluginAPI"));
            if (!request) {
                g_state = "refused: no RequestPluginAPI export";
                spdlog::warn("[precision] Precision.dll is loaded but exports no RequestPluginAPI -- bridge REFUSED");
                return;
            }
            void* api = request(kInterfaceV2);
            if (!api) {
                g_state = "refused: interface V2 not granted";
                spdlog::warn("[precision] Precision.dll did not grant interface V2 (an older or newer API) -- bridge REFUSED");
                return;
            }
            // SELF-CHECK: the object's vtable and the slot we call must both be inside Precision's own image.
            std::uintptr_t lo = 0, hi = 0;
            if (!ImageRange(mod, lo, hi)) {
                g_state = "refused: module image unreadable";
                spdlog::warn("[precision] could not read Precision.dll's image range -- bridge REFUSED");
                return;
            }
            std::uintptr_t vtbl = 0;
            std::memcpy(&vtbl, api, sizeof(vtbl));
            const std::uintptr_t slotAddr = vtbl + kSlotCapsuleLength * sizeof(void*);
            if (vtbl < lo || slotAddr + sizeof(void*) > hi) {
                g_state = "refused: vtable outside the module";
                spdlog::warn("[precision] the API object's vtable ({:#x}) is outside Precision.dll [{:#x}, {:#x}) -- bridge REFUSED",
                             vtbl, lo, hi);
                return;
            }
            std::uintptr_t fn = 0;
            std::memcpy(&fn, reinterpret_cast<const void*>(slotAddr), sizeof(fn));
            if (fn < lo || fn >= hi) {
                g_state = "refused: slot 8 outside the module";
                spdlog::warn("[precision] vtable slot {} ({:#x}) is outside Precision.dll [{:#x}, {:#x}) -- bridge REFUSED",
                             kSlotCapsuleLength, fn, lo, hi);
                return;
            }
            g_api    = api;
            g_length = reinterpret_cast<LengthFn>(fn);
            g_state  = "live";
            spdlog::info("[precision] bridge live: Precision.dll at {:#x}, API V2, GetAttackCollisionCapsuleLength = +{:#x} "
                         "(module-relative; 2.0.4 has +0x117fe0). Measured only, melee reach is still fMeleeReach.",
                         lo, fn - lo);
        }

    }

    float RightWeaponLength(RE::Actor* a_actor) {
        if (!g_resolved) Resolve();
        if (!g_length || !a_actor) return 0.0f;
        const std::uint32_t handle = a_actor->GetHandle().native_handle();
        if (handle == 0) return 0.0f;
        const float len = g_length(g_api, handle, kRightWeapon);
        return (std::isfinite(len) && len > 0.0f) ? len : 0.0f;
    }

    const char* State() {
        if (!g_resolved) Resolve();
        return g_state;
    }

}
