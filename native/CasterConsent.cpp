#include "PCH.h"
#include <unordered_set>   // g_otherCast -- NOT in the PCH (the v1.0.8/9 CI lesson)
#include "CasterConsent.h"
#include "Config.h"
#include "Followers.h"
#include "Forms.h"         // P1 probe: the MFO caster-forward combat style

namespace MFO::CasterConsent {

    namespace {

        using Clock = std::chrono::steady_clock;

        // THE LATCH -- read from the engine's combat-update thread (the hook
        // runs there), written from the main thread. Cross-thread (#4), so a
        // real lock, exactly like Targeting's. shared_mutex because the hook
        // only reads.
        //
        // v1.0.32: the latch carries a PACE alongside the spell. v1.0.30 kept
        // the latch alive through the cast cooldown so the DENY of competing
        // spells stands -- but the standing latch also kept the PERMIT up, and
        // the AI machine-gunned the gambit spell whenever it was in the magic
        // branch (deck 2026-08-05: 4 casts in 2.2 s, 15:32:13-15). permitAfter
        // is stamped by NoteCooldown (+fCastCooldown) -- Loadout::StartCooldown
        // mirrors it on every cast path: the [cast] sink (AI-fired gambit
        // cast), the forced package cast, and the silent fallback. Until it
        // expires the thunk answers NO for the gambit spell TOO. Deny and
        // permit are now separate dials on one latch: deny spans the rule's
        // whole lifetime, permit opens once per cooldown. One store, under the
        // existing g_mx -- the thunk never calls Loadout's non-atomic maps
        // from the combat thread.
        std::shared_mutex g_mx;   // guards g_want + g_otherCast; the hook reads
                                  // (shared), the main thread writes (unique).
        struct Want {
            RE::FormID        spell = 0;
            Clock::time_point permitAfter{};   // epoch (default) = permit now
        };
        std::unordered_map<RE::FormID, Want> g_want;         // follower -> latch
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

        // Same throttle idea for the PACING deny (v1.0.32): one info line per
        // cooldown WINDOW, keyed on the window's expiry -- a window is denied
        // at caster-tick rate, and the log must show the pacing at cast
        // cadence, never tick cadence. Same leaf mutex, same rare path.
        std::unordered_map<RE::FormID, Clock::time_point> g_lastPacedWindow;

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

        // ── P1 PROBE: caster-forward combat-style swap (bProbeCastStyle) ────
        //
        // §0.28's real finding was MELEE BIAS: with the veto removed, a
        // sword-inclined follower rarely ENTERS the magic branch, so the open
        // permission goes unused. The unmeasured axis full cast control
        // depends on: does swapping the LIVE combat instance's style to a
        // caster-forward one (high magicScoreMult) make his own AI cast more
        // -- and mobile? This probe measures exactly that, and NOTHING runs
        // unless bProbeCastStyle=1 (default 0; a dev opt-in, CTD-risk noted).
        //
        // WHY THIS IS LAYOUT-SAFE (§0.29): combatStyle sits at 0x38, BELOW
        // the 0x68 AE divergence (the pinned header's AE-only BSSpinLock
        // inserts at 0x68), so its offset is identical on SE and AE. The
        // asserts below break the build the day that stops being true.
        //
        // WHY THIS IS LIFETIME-SAFE: the swap targets the PER-COMBAT
        // CombatController instance -- never the actor's base TESCombatStyle
        // record -- and every write goes through the a_cc the engine JUST
        // handed this thread's CheckStartCast call, i.e. a controller that is
        // alive by construction on its own combat thread. The stored cc
        // pointer is used for IDENTITY COMPARISON ONLY, never dereferenced:
        // if the live a_cc differs (the old fight ended; its controller died
        // and took the swap with it), the stale entry is dropped, because the
        // engine re-derives a fresh controller's style from the base record
        // anyway. Restore runs through the live a_cc on the first caster tick
        // after the latch releases; a fight that ends first restores
        // implicitly by controller destruction. No path ever touches a
        // controller the engine did not just hand us.
        static_assert(offsetof(RE::CombatController, combatStyle) == 0x38,
                      "CombatController::combatStyle moved -- re-verify against "
                      "the pinned header (ENGINE_NOTES §0.29) before shipping");
        static_assert(offsetof(RE::CombatController, combatStyle) < 0x68,
                      "combatStyle is past the AE layout divergence point (0x68) "
                      "-- its compiled offset is WRONG on AE runtimes");

        struct StyleSwap {
            RE::CombatController* cc       = nullptr;  // identity only -- NEVER dereferenced
            RE::TESCombatStyle*   saved    = nullptr;  // what the engine had derived
            std::uint32_t         rederives = 0;       // engine re-derived under us, times
            std::uint32_t         seenAtSwap = 0;      // g_seen at swap -- ask-rate baseline
            Clock::time_point     swappedAt{};
            Clock::time_point     lastLog{};
        };
        std::mutex g_styleMx;   // leaf; taken only when the probe is live
        std::unordered_map<RE::FormID, StyleSwap> g_styleSwaps;
        // Relaxed-atomic mirror of g_styleSwaps.size() so the fast-out can
        // stay lock-free: 0 whenever the probe is off, so the only cost to a
        // non-probe install is one relaxed load.
        std::atomic<std::size_t> g_styleSwapCount{ 0 };

        void ProbeStyleTick(RE::FormID a_fid, RE::Actor* a_actor,
                            RE::CombatController* a_cc, bool a_latched) {
            auto* style = Forms::g_probeCastStyle;
            const bool wantSwap = a_latched && style &&
                                  Config::g_probeCastStyle.load(std::memory_order_relaxed);

            std::lock_guard<std::mutex> lk(g_styleMx);
            auto it = g_styleSwaps.find(a_fid);

            if (wantSwap) {
                if (!a_cc) return;
                if (it != g_styleSwaps.end() && it->second.cc != a_cc) {
                    // The fight this swap lived in is over -- its controller
                    // (and the swap) died with it. Drop; a fresh swap follows.
                    spdlog::info("[probe cstyle] {:08X}: combat instance changed -- old swap "
                                 "dropped (controller destroyed; engine re-derived)", a_fid);
                    g_styleSwaps.erase(it);
                    it = g_styleSwaps.end();
                }
                if (it == g_styleSwaps.end()) {
                    StyleSwap s;
                    s.cc         = a_cc;
                    s.saved      = a_cc->combatStyle;
                    s.seenAtSwap = g_seen.load(std::memory_order_relaxed);
                    s.swappedAt  = Clock::now();
                    s.lastLog    = s.swappedAt;
                    a_cc->combatStyle = style;
                    g_styleSwaps[a_fid] = s;
                    g_styleSwapCount.store(g_styleSwaps.size(), std::memory_order_relaxed);
                    spdlog::info("[probe cstyle] {:08X} {}: combat style SWAPPED {:08X} -> "
                                 "{:08X} (MFO_CastStyle); seen={} at swap",
                                 a_fid, a_actor && a_actor->GetName() ? a_actor->GetName() : "?",
                                 s.saved ? s.saved->GetFormID() : 0,
                                 style->GetFormID(), s.seenAtSwap);
                    return;
                }
                auto& s = it->second;
                // Did the pointer STAY swapped? The engine re-deriving it
                // mid-combat is itself a probe answer -- log each detection,
                // then re-assert so the measurement continues.
                if (a_cc->combatStyle != style) {
                    ++s.rederives;
                    a_cc->combatStyle = style;
                    spdlog::info("[probe cstyle] {:08X}: engine RE-DERIVED the style under the "
                                 "swap (n={}) -- re-asserted", a_fid, s.rederives);
                }
                // The ask-rate line, throttled ~5s: g_seen delta since the
                // swap is the before/after CheckStartCast frequency marth's
                // soak reads to answer "does he cast more?".
                const auto now = Clock::now();
                if (now - s.lastLog > std::chrono::seconds(5)) {
                    s.lastLog = now;
                    spdlog::info("[probe cstyle] {:08X}: swap held {:.0f}s; CheckStartCast "
                                 "seen={} (+{} since swap), rederives={}",
                                 a_fid,
                                 std::chrono::duration<float>(now - s.swappedAt).count(),
                                 g_seen.load(std::memory_order_relaxed),
                                 g_seen.load(std::memory_order_relaxed) - s.seenAtSwap,
                                 s.rederives);
                }
                return;
            }

            // Not (or no longer) wanted: restore an outstanding swap -- but
            // ONLY through the live controller the engine just handed us, and
            // only if it is the very instance we swapped. Anything else means
            // the swapped controller is gone; drop the entry, nothing to touch.
            if (it == g_styleSwaps.end()) return;
            if (a_cc && it->second.cc == a_cc) {
                a_cc->combatStyle = it->second.saved;
                spdlog::info("[probe cstyle] {:08X}: combat style RESTORED to {:08X} "
                             "(latch released)", a_fid,
                             it->second.saved ? it->second.saved->GetFormID() : 0);
            } else {
                spdlog::info("[probe cstyle] {:08X}: swap entry dropped -- swapped combat "
                             "instance no longer live (restored by destruction)", a_fid);
            }
            g_styleSwaps.erase(it);
            g_styleSwapCount.store(g_styleSwaps.size(), std::memory_order_relaxed);
        }

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
            // every caster tick) and it must be cheap and safe. The second
            // load is the P1 probe's outstanding-swap mirror -- a relaxed
            // atomic, permanently 0 unless bProbeCastStyle was armed, so the
            // fast-out stays lock-free (a swap must still be restorable after
            // its follower's latch drops, which is exactly a wantCount==0
            // moment).
            if (g_wantCount.load(std::memory_order_relaxed) == 0 &&
                g_styleSwapCount.load(std::memory_order_relaxed) == 0) return aiSaysYes;
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
            // The permit deadline rides the same latch entry and the same
            // shared lock (v1.0.32, the "existing locks" rule): the thunk must
            // not call Loadout's non-atomic maps from the combat thread.
            RE::FormID        wantSpell = 0;
            bool              latched   = false;
            bool              cooling   = false;
            Clock::time_point coolUntil{};
            {
                std::shared_lock lk(g_mx);
                if (const auto w = g_want.find(fid); w != g_want.end()) {
                    latched   = true;
                    wantSpell = w->second.spell;
                    if (Clock::now() < w->second.permitAfter) {
                        cooling   = true;
                        coolUntil = w->second.permitAfter;
                    }
                }
            }

            // P1 PROBE hook (bProbeCastStyle, default OFF -- inert then): swap
            // a latched follower's live combat style / restore an outstanding
            // swap. AFTER the g_mx block (its g_styleMx is a leaf; never
            // nested inside the latch lock), and only when the probe is armed
            // or a swap is outstanding, so the normal path never pays for it.
            if (Config::g_probeCastStyle.load(std::memory_order_relaxed) ||
                g_styleSwapCount.load(std::memory_order_relaxed) != 0) {
                ProbeStyleTick(fid, actor, a_cc, latched);
            }
            if (!latched) return aiSaysYes;

            // Only now, for the one latched follower, read the item in hand.
            //
            // SPELLS ONLY (v1.0.32). This hook also fires on
            // CombatMagicCasterRestore deliberating a POTION -- it is the
            // combat drink-potion caster too (§0.29) -- and the old
            // unconditional deny below was suppressing a latched follower's
            // own combat drinking. A non-spell magicItem (AlchemyItem
            // formType 46, scrolls, or null) is never ours to veto OR to
            // force: return the AI's own answer untouched. MFO's gambit-driven
            // drinking never passes through here either way (Logistics::
            // DrinkPotion is an inventory action, not a combat caster).
            auto* mi = a_this->magicItem;
            if (!mi || !mi->Is(RE::FormType::Spell)) return aiSaysYes;
            const bool isWanted = mi->GetFormID() == wantSpell;

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
            // Deliberately NOT cooldown-gated: exclusivity is separate from
            // pacing, so a competing spell is denied whether or not a gambit
            // cast is currently due.
            if (!isWanted) {
                if (Config::g_casterMode.load() == 0) return aiSaysYes;   // observe-only
                DenyLog(fid, actor, mi->GetFormID(), wantSpell);
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

            // PACING (v1.0.32). The force-YES below used to have no cooldown
            // consult at all: while the spell stayed in the follower's hand,
            // the permit fired on every caster tick and casts BURST (deck: 4
            // in ~2.2s) -- fCastCooldown only ever gated the re-EQUIP. Within
            // the cooldown the wanted spell is DENIED, so no cast fires until
            // due; StartCooldown (stamped by the [cast] sink on every gambit
            // cast, AI-fired or forced) opens the next window. Logged once per
            // window, keyed on the window's expiry -- cast cadence, not tick.
            if (cooling) {
                {
                    std::lock_guard<std::mutex> dl(g_denyLogMx);
                    auto& last = g_lastPacedWindow[fid];
                    if (last != coolUntil) {
                        last = coolUntil;
                        spdlog::info("[consent] {:08X} {} pacing: {:08X} held until cooldown "
                                     "expires ({:.1f}s)", fid,
                                     actor->GetName() ? actor->GetName() : "?", wantSpell,
                                     std::chrono::duration<float>(coolUntil - Clock::now()).count());
                    }
                }
                return false;
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
        // Overwrite the SPELL only. Re-Want() fires every service tick while
        // the rule wins; resetting permitAfter with it would push the permit
        // forever out of reach. The pace belongs to NoteCooldown alone.
        g_want[a_follower].spell = a_spell;
        g_wantCount.store(g_want.size(), std::memory_order_relaxed);
    }
    void Clear(RE::FormID a_follower) {
        std::unique_lock lk(g_mx);
        g_want.erase(a_follower);            // permitAfter dies with the latch --
                                             // a fresh fight's first cast is prompt
        g_otherCast.erase(a_follower);       // the miss flag dies with the latch
        g_wantCount.store(g_want.size(), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> dl(g_denyLogMx);
            g_lastDenied.erase(a_follower);
            g_lastPacedWindow.erase(a_follower);
        }
        // An outstanding P1 style swap is NOT touched here: restore needs the
        // live CombatController, which only the combat thread's thunk holds.
        // The swap-count mirror keeps the thunk past its fast-out until the
        // follower's next caster tick restores (or combat end destroys) it.
    }
    void ClearAll() {
        std::unique_lock lk(g_mx);
        g_want.clear();
        g_otherCast.clear();
        g_wantCount.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> dl(g_denyLogMx);
            g_lastDenied.clear();
            g_lastPacedWindow.clear();
        }
        // P1 probe: on a session reset every combat controller is gone (or
        // about to be); the swaps died with them. Drop the mirror so the
        // fast-out goes fully quiet again -- nothing is dereferenced.
        {
            std::lock_guard<std::mutex> sl(g_styleMx);
            if (!g_styleSwaps.empty()) {
                spdlog::info("[probe cstyle] {} outstanding swap(s) dropped on session reset",
                             g_styleSwaps.size());
            }
            g_styleSwaps.clear();
            g_styleSwapCount.store(0, std::memory_order_relaxed);
        }
    }

    void NoteCooldown(RE::FormID a_follower, float a_seconds) {
        if (a_seconds <= 0.0f) return;
        std::unique_lock lk(g_mx);
        // Only a LATCHED follower carries a permit to pace. If the rule has
        // already released, there is nothing to stamp -- and no stale deadline
        // to greet the next latch (Clear() drops the whole entry).
        const auto w = g_want.find(a_follower);
        if (w == g_want.end()) return;
        w->second.permitAfter = Clock::now() +
            std::chrono::milliseconds(static_cast<int>(a_seconds * 1000.0f));
    }

    void NoteCast(RE::FormID a_follower, RE::FormID a_spell) {
        std::unique_lock lk(g_mx);
        const auto w = g_want.find(a_follower);
        if (w == g_want.end()) return;             // not latched -- not our business
        if (w->second.spell == a_spell) return;    // OUR spell -- that is the success
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
