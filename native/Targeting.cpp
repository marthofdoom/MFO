#include "PCH.h"
#include "Targeting.h"
#include "Config.h"

// ONE Win32 symbol, declared by hand.
//
// <windows.h> is banned outside Board.cpp -- it #defines GetObject and hijacks
// BGSDefaultObjectManager::GetObject<T> (ENGINE_NOTES §9). REX::W32 would be the
// tidy alternative but does not exist in the pinned CommonLibSSE-NG, which CI
// proved rather than the docs. A direct declaration depends on neither: it
// cannot drift with the library and it drags in nothing.
extern "C" __declspec(dllimport) void* __stdcall GetModuleHandleA(const char* a_name);

namespace MFO::Targeting {

    namespace {

        // THE LATCH IS READ FROM THE ENGINE'S THREAD.
        //
        // Every other map in MFO is main-thread-only and unlocked. This one is
        // not: the hook body runs inside the engine's per-actor combat update,
        // while Command()/Clear() are called from the evaluator tick on the main
        // thread. That is a genuine cross-thread reader (INVARIANTS #4), so it
        // gets a real lock -- a shared_mutex, because the hook only ever reads.
        std::shared_mutex g_latchMx;
        std::unordered_map<RE::FormID, RE::ActorHandle> g_latch;

        // FAST PATH. The hook fires for EVERY Character in combat in the whole
        // world, not just followers. With no latches active -- which is almost
        // always -- this atomic read is the entire cost, and we never touch the
        // lock at all.
        std::atomic<std::size_t> g_latchCount{ 0 };

        std::atomic<bool> g_hooked{ false };
        std::atomic<bool> g_conflict{ false };
        std::atomic<std::uint32_t> g_asserts{ 0 }, g_drifts{ 0 }, g_passes{ 0 };

        struct UpdateCombatHook {
            static void thunk(RE::Actor* a_this) {
                // ALWAYS run the original first. MFO corrects the engine's
                // choice; it never replaces the engine's bookkeeping.
                func(a_this);

                if (g_latchCount.load(std::memory_order_relaxed) == 0) return;
                if (!a_this || !Config::g_commandTarget.load()) return;

                RE::ActorHandle wanted;
                {
                    std::shared_lock lk(g_latchMx);
                    const auto it = g_latch.find(a_this->GetFormID());
                    if (it == g_latch.end()) return;
                    wanted = it->second;
                }

                // Hold the NiPointer. `wanted.get().get()` drops the refcount at
                // the end of the statement and leaves a raw pointer behind. The
                // reference implementation has this flaw; no reason to copy it.
                auto targetPtr = wanted.get();
                auto* target = targetPtr.get();
                if (!target || target->IsDead() || target->IsDisabled()) return;

                auto& rt = a_this->GetActorRuntimeData();

                // ONLY REDIRECT WHEN THE ENGINE ALREADY HAS A TARGET.
                //
                // This guard is load-bearing, and it is the one the reference
                // implementation leads with. If vanilla cleared the target --
                // the foe fled, died, went undetected, combat ended -- it did
                // that for a reason MFO cannot see. Forcing a target back in
                // means fighting the engine's own validity logic, which is how
                // a mod ends up with followers swinging at things they cannot
                // perceive. Commanding WHICH foe is ours; commanding THAT there
                // is a foe is not.
                auto currentPtr = rt.currentCombatTarget.get();
                auto* current = currentPtr.get();
                if (!current) { ++g_passes; return; }

                if (current == target) { ++g_passes; return; }   // already right

                // The engine re-picked away from our choice. That IS the §4.7
                // retention measurement -- counted, not assumed.
                ++g_drifts;

                rt.currentCombatTarget = wanted;
                if (auto* cc = rt.combatController) {
                    cc->previousTargetHandle = cc->targetHandle;
                    cc->targetHandle = wanted;
                }
                ++g_asserts;
            }

            static inline REL::Relocation<decltype(thunk)> func;
            static constexpr std::size_t idx = 0xE4;   // Character::UpdateCombat
        };

    }

    void InstallHook() {
        if (!Config::g_commandTarget.load()) {
            spdlog::info("[target] bCommandTarget=0 -- hook NOT installed");
            return;
        }
        // 0xE4 IS THE SE/AE INDEX. UpdateCombat is SKYRIM_REL_VR_VIRTUAL, so on
        // VR that slot holds a different function and hooking it would call an
        // arbitrary virtual on every actor in combat -- an instant CTD that
        // would not point anywhere near MFO. The reference implementation does
        // not support VR either.
        if (REL::Module::IsVR()) {
            spdlog::warn("[target] VR runtime detected -- UpdateCombat vtable index is not "
                         "verified for VR; hook NOT installed.");
            return;
        }
        if (g_hooked.exchange(true)) return;

        // A VTABLE INDEX, not an AddressLib offset. That is why this is
        // version-resilient where a sourced relocation is not -- and StartCombat
        // via a sourced relocation is exactly what failed in the field (§0.12).
        REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
        UpdateCombatHook::func =
            vtbl.write_vfunc(UpdateCombatHook::idx, UpdateCombatHook::thunk);

        // KNOWN CONTENTION. LoreRim ships Aggro Management in Skyrim, which
        // steers follower targets through its own hate table and hooks
        // detection. Two writers to the same fields is a fact of the terrain,
        // not a reason to avoid the mechanism -- but it must be VISIBLE, or the
        // resulting weirdness gets blamed on whichever mod was installed last.
        if (::GetModuleHandleA("SmartNPCTargetSelector.dll")) {
            g_conflict = true;
            spdlog::warn("[target] SmartNPCTargetSelector.dll IS LOADED -- it also steers "
                         "follower targets and hooks detection. Expect contention; MFO only "
                         "writes for followers with an active gambit latch.");
        }

        spdlog::info("[target] UpdateCombat vfunc hook installed (Character vtbl idx 0x{:X})",
                     UpdateCombatHook::idx);
    }

    bool Command(RE::FormID a_follower, RE::ActorHandle a_target) {
        std::unique_lock lk(g_latchMx);
        const auto it = g_latch.find(a_follower);
        if (it != g_latch.end() && it->second == a_target) return false;   // unchanged
        g_latch[a_follower] = a_target;
        g_latchCount.store(g_latch.size(), std::memory_order_relaxed);
        return true;
    }

    RE::ActorHandle Current(RE::FormID a_follower) {
        std::shared_lock lk(g_latchMx);
        const auto it = g_latch.find(a_follower);
        return it != g_latch.end() ? it->second : RE::ActorHandle{};
    }

    void Clear(RE::FormID a_follower) {
        std::unique_lock lk(g_latchMx);
        g_latch.erase(a_follower);
        g_latchCount.store(g_latch.size(), std::memory_order_relaxed);
    }

    void ClearAll() {
        std::unique_lock lk(g_latchMx);
        g_latch.clear();
        g_latchCount.store(0, std::memory_order_relaxed);
    }

    bool IsHooked() { return g_hooked.load(); }

    Stats GetStats() {
        Stats s;
        s.asserts = g_asserts.load();
        s.drifts  = g_drifts.load();
        s.passes  = g_passes.load();
        s.conflictMod = g_conflict.load();
        s.latched = static_cast<std::uint32_t>(g_latchCount.load());
        return s;
    }

}
