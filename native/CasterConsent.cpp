#include "PCH.h"
#include <unordered_set>   // g_otherCast -- NOT in the PCH (the v1.0.8/9 CI lesson)
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

        // Followers whose AI cast a DIFFERENT spell while latched -- the miss
        // signal the hybrid forced-cast consumes (see NoteCast in the header).
        // Same lock as g_want: the two are read and erased together.
        std::unordered_set<RE::FormID> g_otherCast;

        std::atomic<bool> g_hooked{ false };
        std::atomic<std::uint32_t> g_seen{ 0 }, g_vetoed{ 0 }, g_forced{ 0 };

        // Each concrete caster vtable is a separate function pointer, so the
        // originals are keyed by the vtable pointer we read off `this`.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;

        // Throttle for the EXCLUSIVE-CONTROL deny log. The thunk runs at
        // caster-tick frequency, so a suppressed own-spell is logged only when
        // the (follower, denied-spell) pair CHANGES -- not every tick. Its own
        // leaf mutex, taken only on the rare deny path (the one latched
        // follower whose AI wants a different spell), never on the hot fast-out.
        std::mutex g_denyLogMx;
        std::unordered_map<RE::FormID, RE::FormID> g_lastDenied;

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

        // Log a suppressed own-spell cast, deduped per (follower, denied spell)
        // so the combat-thread thunk cannot spam it. Leaf mutex, deny-path only.
        void DenyLog(RE::FormID a_fid, RE::Actor* a_actor,
                     RE::FormID a_denied, RE::FormID a_wanted) {
            {
                std::lock_guard<std::mutex> lk(g_denyLogMx);
                auto it = g_lastDenied.find(a_fid);
                if (it != g_lastDenied.end() && it->second == a_denied) return;
                g_lastDenied[a_fid] = a_denied;
            }
            spdlog::info("[consent] {:08X} {} DENIED own spell {:08X} while latched for {:08X}",
                         a_fid, a_actor->GetName() ? a_actor->GetName() : "?", a_denied, a_wanted);
        }

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

            // Only now, for the one latched follower, read the spell in hand.
            auto* mi = a_this->magicItem;
            const bool isWanted = mi && mi->GetFormID() == wantSpell;

            // EXCLUSIVE CONTROL (marth: the follower cast BOTH his own spell AND
            // the forced one -- "an improvement, but not the control the mod's
            // premise dictates"). While a follower is latched, the gambit's
            // spell is the ONLY spell he may cast. If the AI is about to cast a
            // DIFFERENT one (his own Chain Lightning), DENY it -- so nothing but
            // the dictated spell ever leaves his hands; the hybrid then supplies
            // the gambit spell on the grace timeout (Actuation's force-cast).
            // This is the twin of Want()'s FORCE-YES: that adds the gambit spell,
            // this removes every competing one, and together they make casting
            // fully gambit-driven. Cast-only: a follower out of magicka still
            // falls back to melee (that path never reaches CheckStartCast). LOG
            // mode still only observes -- suppression is a FORCE-mode act.
            if (!isWanted) {
                if (Config::g_casterMode.load() == 0) return aiSaysYes;   // observe-only
                DenyLog(fid, actor, mi ? mi->GetFormID() : 0, wantSpell);
                return false;
            }

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
        // VR GUARD, mirroring Targeting::InstallHook. These are SE/AE
        // CombatMagicCaster vtables at a fixed index; on VR the layout is
        // unverified and slot 0x06 could be an unrelated virtual, so writing it
        // would call garbage on every combat caster -- an instant CTD nowhere
        // near MFO. Now that bCasterHook defaults ON, this guard is what keeps a
        // VR player from that crash out of the box.
        if (REL::Module::IsVR()) {
            spdlog::warn("[consent] VR runtime detected -- CheckStartCast vtable indices are not "
                         "verified for VR; hook NOT installed.");
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
        g_otherCast.erase(a_follower);   // the miss flag dies with the latch
        g_wantCount.store(g_want.size(), std::memory_order_relaxed);
        { std::lock_guard<std::mutex> dl(g_denyLogMx); g_lastDenied.erase(a_follower); }
    }
    void ClearAll() {
        std::unique_lock lk(g_mx);
        g_want.clear();
        g_otherCast.clear();
        g_wantCount.store(0, std::memory_order_relaxed);
        { std::lock_guard<std::mutex> dl(g_denyLogMx); g_lastDenied.clear(); }
    }

    void NoteCast(RE::FormID a_follower, RE::FormID a_spell) {
        std::unique_lock lk(g_mx);
        const auto w = g_want.find(a_follower);
        if (w == g_want.end()) return;       // not latched -- not our business
        if (w->second == a_spell) return;    // OUR spell -- that is the success
                                             // path, handled by the sink's
                                             // NoteOurCast (latch KEPT, v1.0.30)
        g_otherCast.insert(a_follower);
    }

    bool NoteOurCast(RE::FormID a_follower) {
        std::unique_lock lk(g_mx);
        if (!g_want.contains(a_follower)) return false;   // rule already released
        // KEEP g_want -- that is the whole point (see the header). Retire only
        // the per-cast transients: the miss flag is consumed by this cast, and
        // the deny-log dedup entry resets so the next cooldown's first denied
        // own-spell logs once more. Same nested lock order as Clear().
        g_otherCast.erase(a_follower);
        { std::lock_guard<std::mutex> dl(g_denyLogMx); g_lastDenied.erase(a_follower); }
        return true;
    }

    bool OtherCastSeen(RE::FormID a_follower) {
        std::shared_lock lk(g_mx);
        return g_otherCast.contains(a_follower);
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
