#include "PCH.h"
#include "CasterConsent.h"
#include "Config.h"
#include "Followers.h"

namespace MFO::CasterConsent {

    namespace {

        // THE LATCH -- read from the engine's combat-update thread (the hook
        // runs there), written from the main thread. Cross-thread (#4), so a
        // real lock, exactly like Targeting's. shared_mutex because the hook
        // only reads.
        std::shared_mutex g_mx;
        std::unordered_map<RE::FormID, RE::FormID> g_want;   // follower -> spell it may cast
        std::atomic<std::size_t> g_wantCount{ 0 };           // fast-path: skip the lock when empty

        std::atomic<bool> g_hooked{ false };
        std::atomic<std::uint32_t> g_seen{ 0 }, g_vetoed{ 0 }, g_forced{ 0 };

        // Each concrete caster vtable is a separate function pointer, so the
        // originals are keyed by the vtable pointer we read off `this`.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;

        // The 14 concrete CombatMagicCaster vtables. All share the
        // CheckStartCast(CombatController*) signature at index 0x06.
        // (VTABLE_CombatMagicCasterArmor is a symbol with NO class -- excluded,
        // ENGINE_NOTES §0.28.)
        constexpr std::size_t kCheckStartCast = 0x06;

        // LAYOUT GUARD -- ENGINE_NOTES §0.29. CommonLibSSE-NG's
        // CombatController.h gates its AE-only member (BSSpinLock at 0x68) on
        // SKYRIM_SUPPORT_AE, a macro NG NEVER defines (NG uses
        // ENABLE_SKYRIM_AE). So this struct ALWAYS compiles with the SE
        // layout, and on an AE runtime every member past 0x68 sits +8 from
        // where the header says. Reading cachedAttacker (header 0xC8) on AE
        // actually reads handleCount -- an int that is 1 in a one-enemy fight;
        // it passed a null check and formID at 1+0x14 faulted. That was BOTH
        // recurring CTDs.
        //
        // THE RULE: this hook may only touch CombatController members BELOW
        // 0x68, which are layout-identical on SE and AE. attackerHandle (0x28)
        // is the only member we read; these asserts break the build if that
        // ever stops being true -- an intentional fix, not an accidental one.
        static_assert(offsetof(RE::CombatController, attackerHandle) == 0x28,
                      "CombatController::attackerHandle moved -- re-verify the "
                      "SE/AE layout split (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, attackerHandle) < 0x68,
                      "attackerHandle is past the AE layout divergence point "
                      "(0x68) -- its compiled offset is WRONG on AE runtimes");

        using CheckStartCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);

        bool thunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            // Recover the original for THIS vtable. If the vtable is not one we
            // hooked, this thunk was reached on a foreign object -- do NOT read
            // its members. Return a benign false.
            const auto vt = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_orig.find(vt);
            if (oit == g_orig.end()) return false;
            const auto original = reinterpret_cast<CheckStartCast_t>(oit->second);
            const bool aiSaysYes = original(a_this, a_cc);

            // FAST OUT: nothing latched -> we only ever observe, so leave now
            // and never touch a member. This is the common case (every NPC's
            // every caster tick) and it must be cheap and safe.
            if (g_wantCount.load(std::memory_order_relaxed) == 0) return aiSaysYes;
            if (!a_cc) return aiSaysYes;

            // RESOLVE THE ACTOR VIA attackerHandle (0x28), NEVER cachedAttacker.
            //
            // This is NOT about staleness -- it is a CommonLibSSE-NG HEADER BUG
            // (ENGINE_NOTES §0.29). CombatController.h guards its AE-only member
            // on `SKYRIM_SUPPORT_AE`, a macro NG never defines (it uses
            // ENABLE_SKYRIM_AE). So the struct always compiles with the SE
            // layout, but on the 1.6.1170 AE runtime every member past 0x68 is
            // shifted +8. `cachedAttacker` compiles to 0xC8 -- where the real AE
            // object holds `handleCount`, which is exactly 1 when the follower
            // fights one enemy. Read as a pointer that is the value 1; formID at
            // +0x14 then faults at 0x15. It crashed the game twice, identically.
            //
            // attackerHandle is at 0x28, BEFORE the 0x68 divergence, so its
            // offset is the same in both layouts. NEVER dereference ANY
            // CombatController member at offset >= 0x68 through this header
            // (aimControllers, currentAimController, targetSelectors,
            // cachedAttacker, cachedTarget) -- all are +8 at runtime on AE.
            auto attPtr = a_cc->attackerHandle.get();   // NiPointer<Actor>
            auto* actor = attPtr.get();
            if (!actor) return aiSaysYes;
            const auto fid = actor->GetFormID();

            // Is this follower latched? Decide BEFORE touching the caster's
            // spell -- for every non-latched actor we never read magicItem.
            RE::FormID wantSpell = 0;
            {
                std::shared_lock lk(g_mx);
                const auto w = g_want.find(fid);
                if (w == g_want.end()) return aiSaysYes;
                wantSpell = w->second;
            }

            // Only now, for the one latched follower, read the spell.
            auto* mi = a_this->magicItem;
            if (!mi || mi->GetFormID() != wantSpell) return aiSaysYes;

            ++g_seen;
            if (!aiSaysYes) ++g_vetoed;

            if (Config::g_casterMode.load() == 0) {
                spdlog::info("[consent] {:08X} {} CheckStartCast for {:08X} -> AI says {} "
                             "(mode=log)", fid, actor->GetName() ? actor->GetName() : "?",
                             wantSpell, aiSaysYes ? "YES" : "NO");
                return aiSaysYes;
            }
            if (!aiSaysYes) {
                ++g_forced;
                spdlog::info("[consent] {:08X} {} -> FORCED cast of {:08X}",
                             fid, actor->GetName() ? actor->GetName() : "?", wantSpell);
            }
            return true;
        }

    }

    void InstallHook() {
        if (!Config::g_casterHook.load()) {
            spdlog::info("[consent] bCasterHook=0 -- CheckStartCast hook NOT installed");
            return;
        }
        if (g_hooked.exchange(true)) return;

        // VTABLE INDICES, not sourced offsets -- version-resilient. All 14
        // concrete casters, because a follower's spell could land in any
        // category (Restore for a heal, Offensive for a bolt, Ward, ...).
        // NOTE (ENGINE_NOTES §0.29): CombatMagicCasterRestore is ALSO the
        // caster for combat potion-drinking, so this hook fires on potion
        // deliberation too -- expected, and layout-safe since the fix above.
        const REL::VariantID kVtables[] = {
            RE::VTABLE_CombatMagicCasterOffensive[0],  RE::VTABLE_CombatMagicCasterRestore[0],
            RE::VTABLE_CombatMagicCasterWard[0],       RE::VTABLE_CombatMagicCasterSummon[0],
            RE::VTABLE_CombatMagicCasterStagger[0],    RE::VTABLE_CombatMagicCasterDisarm[0],
            RE::VTABLE_CombatMagicCasterCloak[0],      RE::VTABLE_CombatMagicCasterLight[0],
            RE::VTABLE_CombatMagicCasterInvisibility[0],RE::VTABLE_CombatMagicCasterBoundItem[0],
            RE::VTABLE_CombatMagicCasterTargetEffect[0],
            RE::VTABLE_CombatMagicCasterParalyze[0],   RE::VTABLE_CombatMagicCasterScript[0],
            RE::VTABLE_CombatMagicCasterReanimate[0],
        };

        int n = 0;
        for (const auto& id : kVtables) {
            REL::Relocation<std::uintptr_t> vt{ id };
            // write_vfunc returns the previous entry -- store it under this
            // vtable's address so the thunk can dispatch to the right original.
            const std::uintptr_t orig = vt.write_vfunc(kCheckStartCast, &thunk);
            g_orig[vt.address()] = orig;
            ++n;
        }
        spdlog::info("[consent] CheckStartCast hooked on {} caster vtable(s), mode={}",
                     n, Config::g_casterMode.load() == 0 ? "LOG" : "FORCE");
    }

    bool IsHooked() { return g_hooked.load(); }

    void Want(RE::FormID a_follower, RE::FormID a_spell) {
        std::unique_lock lk(g_mx);
        g_want[a_follower] = a_spell;
        g_wantCount.store(g_want.size(), std::memory_order_relaxed);
    }
    void Clear(RE::FormID a_follower) {
        std::unique_lock lk(g_mx);
        g_want.erase(a_follower);
        g_wantCount.store(g_want.size(), std::memory_order_relaxed);
    }
    void ClearAll() {
        std::unique_lock lk(g_mx);
        g_want.clear();
        g_wantCount.store(0, std::memory_order_relaxed);
    }

    Stats GetStats() {
        Stats s;
        s.seen = g_seen.load(); s.vetoed = g_vetoed.load(); s.forced = g_forced.load();
        s.latched = static_cast<std::uint32_t>(g_wantCount.load());
        return s;
    }
    void ClearTransientState() {
        ClearAll();
        g_seen = 0; g_vetoed = 0; g_forced = 0;
    }

}
