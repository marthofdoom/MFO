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

        // The 16 concrete CombatMagicCaster vtables. All share the
        // CheckStartCast(CombatController*) signature at index 0x06.
        constexpr std::size_t kCheckStartCast = 0x06;

        using CheckStartCast_t = bool (*)(RE::CombatMagicCaster*, RE::CombatController*);

        bool thunk(RE::CombatMagicCaster* a_this, RE::CombatController* a_cc) {
            // Recover the original for THIS vtable and call it -- MFO never
            // suppresses a cast the AI wanted, it only permits one it did not.
            const auto vt = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto it = g_orig.find(vt);
            const bool aiSaysYes = (it != g_orig.end())
                ? reinterpret_cast<CheckStartCast_t>(it->second)(a_this, a_cc)
                : false;

            if (g_wantCount.load(std::memory_order_relaxed) == 0) return aiSaysYes;
            if (!a_cc || !a_this->magicItem) return aiSaysYes;

            // Whose caster is this? The combat controller's own actor.
            auto* actor = a_cc->cachedAttacker.get();
            if (!actor) return aiSaysYes;
            const auto fid = actor->GetFormID();

            RE::FormID wantSpell = 0;
            {
                std::shared_lock lk(g_mx);
                const auto w = g_want.find(fid);
                if (w == g_want.end()) return aiSaysYes;
                wantSpell = w->second;
            }
            if (a_this->magicItem->GetFormID() != wantSpell) return aiSaysYes;

            ++g_seen;
            if (!aiSaysYes) ++g_vetoed;

            // LOG MODE (iCasterMode 0): observe only, never change the answer.
            // This is the read-only experiment that proves the veto is here
            // before a single bool is flipped.
            if (Config::g_casterMode.load() == 0) {
                spdlog::info("[consent] {:08X} {} CheckStartCast for {:08X} -> AI says {} "
                             "(mode=log, not overriding)",
                             fid, actor->GetName() ? actor->GetName() : "?",
                             wantSpell, aiSaysYes ? "YES" : "NO");
                return aiSaysYes;
            }

            // FORCE MODE: permit the cast the rule demands. The AI still owns
            // when-exactly, aim and movement -- this only removes its veto.
            if (!aiSaysYes) {
                ++g_forced;
                spdlog::info("[consent] {:08X} {} -> FORCED cast of {:08X} (AI said no)",
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

        // VTABLE INDICES, not sourced offsets -- version-resilient. All 16
        // concrete casters, because a follower's spell could land in any
        // category (Restore for a heal, Offensive for a bolt, Ward, ...).
        const REL::VariantID kVtables[] = {
            RE::VTABLE_CombatMagicCasterOffensive[0],  RE::VTABLE_CombatMagicCasterRestore[0],
            RE::VTABLE_CombatMagicCasterWard[0],       RE::VTABLE_CombatMagicCasterSummon[0],
            RE::VTABLE_CombatMagicCasterStagger[0],    RE::VTABLE_CombatMagicCasterDisarm[0],
            RE::VTABLE_CombatMagicCasterCloak[0],      RE::VTABLE_CombatMagicCasterLight[0],
            RE::VTABLE_CombatMagicCasterInvisibility[0],RE::VTABLE_CombatMagicCasterBoundItem[0],
            RE::VTABLE_CombatMagicCasterArmor[0],      RE::VTABLE_CombatMagicCasterTargetEffect[0],
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
