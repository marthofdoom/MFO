#include "PCH.h"
#include <unordered_set>   // g_otherCast -- NOT in the PCH (the v1.0.8/9 CI lesson)
#include <thread>          // g_mainThread: std::this_thread::get_id (FF world-walk gate)
#include "CasterConsent.h"
#include "Config.h"
#include "Followers.h"
#include "Forms.h"         // P1 probe: the MFO caster-forward combat style
#include "Packages.h"      // StreamLive -- the concentration bound is latch-independent
#include "APMFBridge.h"    // IsOwnedCastActive -- stand down the exclusivity deny where
                            // APMF's T2 allowance hooks now own it (Phase 2)

namespace MFO::CasterConsent {

    // CAST-CONTROL SLIDER classification -- BY THE SPELL'S EFFECTS, not the
    // deliberating caster's vtable (v1.0.35). The vtable category mislabelled
    // dual-purpose spells: Absorb Health deliberates through the Restore
    // caster (it heals the caster) so it read as a Heal and slipped past
    // "ignore buffs & heals", even though it is an ATTACK. The rule marth
    // wants is "does it harm a foe": ANY hostile/detrimental effect -> Offense;
    // else a beneficial Health effect -> Heal (self via Delivery); else Buff.
    // PUBLIC since the concentration hold: Actuation picks the bounded-stream
    // policy (hostile 1-4s / heal until-topped / utility capped) off the same
    // taxonomy the slider's exemptions use, so the two can never disagree.
    SpellKind ClassifySpell(RE::MagicItem* a_mi) {
        if (!a_mi) return SpellKind::Offense;
        bool restoresHealth = false;
        for (auto* eff : a_mi->effects) {
            auto* base = eff ? eff->baseEffect : nullptr;
            if (!base) continue;
            if (base->IsHostile() || base->IsDetrimental())
                return SpellKind::Offense;   // harms a target -> offense, whatever else it does
            if (base->data.primaryAV == RE::ActorValue::kHealth)
                restoresHealth = true;       // beneficial health effect -> a heal
        }
        return restoresHealth ? SpellKind::Heal : SpellKind::Buff;
    }

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

        // ── #59: CONTINUOUS CAST CONTROL -- the persistent gambit reference ──
        // The latch above is TRANSIENT: Want() arms it when a cast rule wins,
        // Clear() drops it on any IsInCombat() flap (the Scheduler's non-combat
        // branch), and it only re-arms when a cast rule wins the scan AGAIN.
        // Deck-proven leak: Serana on EXACT force-casts Sunfire, but between
        // forced casts -- while act.equip_melee wins, or across a combat-state
        // flicker -- she is un-latched, both deny hooks fast-out, and her AI
        // fires its own Ice Spike / conjuration ("their own spell, not ours").
        // The filter must not share the latch's lifetime: this map is the
        // PERSISTENT per-follower record "under continuous cast control", the
        // follower's configured combat cast-gambit spells. The Scheduler
        // repopulates it every combat service tick (cast control on, follower
        // tracked + in combat + at least one cast gambit configured -- an
        // empty set ERASES, so unconfigured followers never appear); it dies
        // in Clear (combat end, dismissal) and ClearAll (revert/load). The
        // deny half (CtrlUnlatchedDeny) reads the SLIDER LIVE at deny time:
        // exact (>=4) allows only the gambit spells, partial (1-3) applies
        // the same CastExempt kind-filter the latched deny uses -- both now
        // continuous in combat, latched or not (marth: "the partial filter
        // will also need to be active constant"). Same g_mx leaf lock as
        // g_want (written from the scheduler's task-queue tick, read from
        // the combat thread); the count mirror keeps both hooks' fast-outs
        // lock-free while no follower is under control.
        std::unordered_map<RE::FormID, std::vector<RE::FormID>> g_ctrl;
        std::atomic<std::size_t> g_ctrlCount{ 0 };

        // Followers whose AI cast a DIFFERENT spell while latched -- the miss
        // signal the hybrid forced-cast consumes (see NoteCast in the header).
        // Same lock as g_want: the two are read and erased together.
        std::unordered_set<RE::FormID> g_otherCast;

        std::atomic<bool> g_hooked{ false };
        std::atomic<std::uint32_t> g_seen{ 0 }, g_vetoed{ 0 }, g_forced{ 0 };

        // Each concrete caster vtable is a separate function pointer, so the
        // originals are keyed by the vtable pointer we read off `this`.
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_orig;

        // Does the AI KEEP this spell at slider level `lvl` (i.e. MFO exempts
        // it)? "ignore X" = leave category X to the follower's AI; tighter
        // toward exact. lvl 1 ignore buffs+heals, 2 ignore heals (default),
        // 3 ignore self-heals; lvl>=4 (exact) exempts nothing; lvl<=0 handled
        // before this is ever called.
        bool CastExempt(SpellKind a_kind, bool a_selfHeal, int a_lvl) {
            switch (a_lvl) {
                case 1:  return a_kind != SpellKind::Offense;            // keep buffs + all heals
                case 2:  return a_kind == SpellKind::Heal;               // keep all heals
                case 3:  return a_kind == SpellKind::Heal && a_selfHeal; // keep self-heals only
                default: return false;                                   // exact: exempt nothing
            }
        }

        // Throttle for the EXCLUSIVE-CONTROL deny log. The thunk runs at
        // caster-tick frequency, so a suppressed own-spell is logged only when
        // the (follower, denied-spell) pair CHANGES -- not every tick. Its own
        // leaf mutex, taken only on the rare deny path (the one latched
        // follower whose AI wants a different spell), never on the hot fast-out.
        std::mutex g_denyLogMx;
        std::unordered_map<RE::FormID, RE::FormID> g_lastDenied;

        // Same dedup for the CONCENTRATION denies below (one line per
        // follower+spell, not per caster tick). Shares g_denyLogMx and the
        // same clear points as g_lastDenied; shared by the latched and the
        // latch-independent deny so a latch flap does not double-log.
        std::unordered_map<RE::FormID, RE::FormID> g_lastConcDeny;

        // ── THE CONCENTRATION BOUND, latch-independent (v1.0.53 follow-up) ──
        // "Exact spell bounding must affect all spells, or it may as well not
        // exist" (marth). Actuation's ConcentrationCast early bails --
        // packages off, self route barred, cooling, LoS occluded, teammate in
        // the line of fire -- all return UNLATCHED, and an unlatched
        // follower's own AI would otherwise stream the very spell MFO just
        // refused to (the teammate-in-line bail would come straight back as
        // AI friendly fire). So this deny keys on the SPELL and the SLIDER,
        // never the latch: a tracked follower may run a concentration spell
        // of a BOUNDED category on the AI channel ONLY while MFO's own
        // bounded stream for exactly that follower+spell is live (the
        // package cast reads live and passes these same gates). Exempt
        // categories stay the AI's to keep; castControl<=0 and log mode stay
        // fully hands-off; non-concentration spells fall straight through.
        // Ordered cheap-first: two atomics, then immutable form reads, and
        // the membership probe (Followers::IsTrackedFast -- the g_mx mirror,
        // combat-thread-safe) only ever runs for a concentration spell.
        bool ConcUnboundedDeny(RE::FormID a_fid, RE::MagicItem* a_mi) {
            const int lvl = Config::g_castControl.load();
            // EXACT-ONLY (marth). The latch-independent AI-channel bound fires
            // ONLY at Exact (4). At levels 1-3 a follower's OWN AI concentration
            // is vanilla-safe (self-bounded -- the freeze was MFO force-HOLDING
            // a stream, never the AI casting one), so it is left alone; a gambit
            // follower stays bounded there via the latched deny + package stream.
            if (lvl < 4 || Config::g_casterMode.load() == 0) return false;
            if (!a_mi || !a_mi->Is(RE::FormType::Spell)) return false;
            auto* sp = a_mi->As<RE::SpellItem>();
            if (!sp || sp->GetCastingType() != RE::MagicSystem::CastingType::kConcentration)
                return false;
            // #9 SEV-1: this runs on the COMBAT thread (CheckStartCast/CheckCast
            // thunks). IsTracked walks the UNLOCKED g_active vector, which the job
            // worker reallocates in Refresh -> UAF. IsTrackedFast probes the
            // g_mx-guarded FormID mirror instead (membership only, any thread).
            if (!Followers::IsTrackedFast(a_fid)) return false;   // world casters: not ours
            if (Packages::StreamLive(a_fid, a_mi->GetFormID()))
                return false;                                 // OUR bounded stream -> pass
            const SpellKind k = ClassifySpell(a_mi);
            const bool selfHeal = k == SpellKind::Heal &&
                a_mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            return !CastExempt(k, selfHeal, lvl);             // bounded category -> deny
        }

        // Deduped, legible log for the latch-independent suppression. The
        // packages-off case is named explicitly -- at a bounding slider level
        // that spell is UNBOUNDABLE and never casts, which must read as a
        // decision, not a mystery.
        void ConcSuppressLog(RE::FormID a_fid, RE::Actor* a_actor, RE::FormID a_spell) {
            std::lock_guard<std::mutex> dl(g_denyLogMx);
            auto& last = g_lastConcDeny[a_fid];
            if (last == a_spell) return;
            last = a_spell;
            spdlog::info("[cast] {:08X} {} concentration {:08X} suppressed -- no live bounded "
                         "stream to ride{}",
                         a_fid, a_actor->GetName() ? a_actor->GetName() : "?", a_spell,
                         Config::g_usePackages.load()
                             ? "" : " (packages OFF: unboundable at this slider level)");
        }

        // ── #59: the latch-independent CAST-CONTROL deny ────────────────────
        // The un-latched half of the slider's filter (see g_ctrl above). NORMAL
        // SPELLS ONLY, mirroring CheckCastThunk's own gate: potions (the
        // Restore caster drinks through CheckStartCast too, §0.29), scrolls,
        // staves, shouts, powers and abilities are never ours to veto -- the
        // latched deny exempts them the same way. A gambit spell passes (the
        // takeover's own cast must still fire, forced or AI-driven); past
        // that, exact (>=4) denies everything and partial (1-3) denies the
        // non-exempt kinds -- exactly the latched filter, read off the LIVE
        // slider so an MCM change applies mid-combat. Ordered cheap-first:
        // one relaxed atomic, two config atomics, the form reads, and only
        // then the shared-locked map probe. a_gambitOut carries a configured
        // gambit spell for the deny log (0 = allow / not under control).
        bool CtrlUnlatchedDeny(RE::FormID a_fid, RE::MagicItem* a_mi, RE::FormID& a_gambitOut) {
            a_gambitOut = 0;
            if (g_ctrlCount.load(std::memory_order_relaxed) == 0) return false;
            const int lvl = Config::g_castControl.load();
            if (lvl <= 0 || Config::g_casterMode.load() == 0) return false;
            if (!a_mi || !a_mi->Is(RE::FormType::Spell)) return false;
            auto* sp = a_mi->As<RE::SpellItem>();
            if (!sp || sp->GetSpellType() != RE::MagicSystem::SpellType::kSpell) return false;
            const auto spellID = a_mi->GetFormID();
            {
                std::shared_lock lk(g_mx);
                const auto it = g_ctrl.find(a_fid);
                if (it == g_ctrl.end() || it->second.empty()) return false;
                for (const auto s : it->second) {
                    if (s == spellID) { a_gambitOut = 0; return false; }   // a gambit spell -> the takeover's own
                    if (a_gambitOut == 0) a_gambitOut = s;
                }
            }
            if (lvl >= 4) return true;                    // exact: only the gambit spells cast
            const SpellKind k = ClassifySpell(a_mi);
            const bool selfHeal = k == SpellKind::Heal &&
                a_mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            return !CastExempt(k, selfHeal, lvl);         // partial: the kind-filter, continuous
        }

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
                     RE::FormID a_denied, RE::FormID a_wanted, bool a_latched) {
            {
                std::lock_guard<std::mutex> lk(g_denyLogMx);
                auto it = g_lastDenied.find(a_fid);
                if (it != g_lastDenied.end() && it->second == a_denied) return;
                g_lastDenied[a_fid] = a_denied;
            }
            if (a_latched) {
                spdlog::info("[consent] {:08X} {} DENIED own spell {:08X} while latched for {:08X}",
                             a_fid, a_actor->GetName() ? a_actor->GetName() : "?", a_denied, a_wanted);
            } else {
                // #59: the continuous filter caught a between-casts leak -- the
                // exact line whose ABSENCE the field test reads.
                spdlog::info("[consent] {:08X} {} DENIED own spell {:08X} -- continuous cast "
                             "control (unlatched; gambit {:08X})",
                             a_fid, a_actor->GetName() ? a_actor->GetName() : "?", a_denied, a_wanted);
            }
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
            // moment). The third is #59's continuous-control mirror: while ANY
            // follower is under the slider's filter the thunk must reach the
            // un-latched deny below -- un-latched is exactly the leak it closes
            // -- so the fast-out may only take all three at zero.
            if (g_wantCount.load(std::memory_order_relaxed) == 0 &&
                g_styleSwapCount.load(std::memory_order_relaxed) == 0 &&
                g_ctrlCount.load(std::memory_order_relaxed) == 0) return aiSaysYes;
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
            // spell -- for a non-latched actor, magicItem is only ever read
            // behind ConcUnboundedDeny's / CtrlUnlatchedDeny's atomic gates
            // (the latch-independent bounds), never on the plain fast-out.
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
            if (!latched) {
                // LATCH-INDEPENDENT concentration bound (see ConcUnboundedDeny):
                // ConcentrationCast's early bails return unlatched, and the AI
                // channel must not reopen there. The helper is ordered cheap-
                // first (magicItem is only read past two atomic gates), so the
                // worldwide unlatched fast-out stays effectively free.
                if (ConcUnboundedDeny(fid, a_this->magicItem)) {
                    ConcSuppressLog(fid, actor, a_this->magicItem->GetFormID());
                    return false;
                }
                // #59: CONTINUOUS CAST CONTROL. The un-latched gap -- between
                // forced casts, while an equip rule wins the scan, across a
                // combat-state flicker's Clear -- used to hand the AI channel
                // back wholesale (Serana's own Ice Spike between forced
                // Sunfires). The persistent filter runs the SAME slider policy
                // the latched deny below runs, minus the latch: gambit spells
                // pass, exact denies the rest, partial denies the non-exempt
                // kinds. In-combat by construction here -- CheckStartCast only
                // ticks under a live CombatController -- and g_ctrl only ever
                // names tracked, cast-gambit-configured followers.
                if (RE::FormID gambit = 0;
                    CtrlUnlatchedDeny(fid, a_this->magicItem, gambit)) {
                    DenyLog(fid, actor, a_this->magicItem->GetFormID(), gambit, false);
                    return false;
                }
                return aiSaysYes;
            }

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

            // CAST-CONTROL SLIDER (mage update, iCastControl). Level 0 = OFF:
            // do nothing -- never deny, never force, no instrumentation -- so a
            // player who wants none of this keeps fully vanilla casting. Levels
            // 1..4 tighten control; the FORCE-YES of the gambit spell below runs
            // at every level >= 1.
            const int castLvl = Config::g_castControl.load();
            if (castLvl <= 0) return aiSaysYes;

            // GRADUATED EXCLUSIVITY (marth's slider). While a follower is
            // latched, a spell that is NOT the gambit's is denied ONLY if its
            // category is not exempt at the current level: exact (4) denies
            // every competing spell (the old exclusive-control behaviour);
            // level 3 leaves self-heals to the AI; level 2 (default) leaves all
            // heals; level 1 leaves buffs+heals and only forces offense. "ignore
            // X" = leave category X to the follower's own AI. The category comes
            // from the SPELL'S EFFECTS (v1.0.35 ClassifySpell -- hostile => offense,
            // so Absorb Health counts as offense); heal is split self/other by
            // Delivery. Denies are NOT cooldown-gated: exclusivity is separate
            // from pacing. LOG mode still only observes. NOTE: this is only the
            // ADVISORY half -- the SpellCast hook hard-aborts what slips through.
            if (!isWanted) {
                // Phase 2 (APMF ALLOWANCE-TEMPLATE.md §7): the owned-cast model
                // claims this facet via APMF ch.8, and APMF's OWN CheckCast (0x0A,
                // hard gate) hook now enforces this exact exclusivity -- deny any
                // spell that isn't the claimed one. MFO's advisory CheckStartCast
                // deny is redundant there and would only fight it (two
                // independently-configured deny paths, different slider level vs.
                // hard claim); stand down and let APMF own it for this follower.
                // Consent (the isWanted==true path below) is untouched -- that is
                // MFO's own mechanism, not a facet APMF ever provides.
                if (APMFBridge::IsOwnedCastActive(fid)) return aiSaysYes;
                if (Config::g_casterMode.load() == 0) return aiSaysYes;   // dev observe-only
                if (castLvl < 4) {
                    const SpellKind kind = ClassifySpell(mi);
                    const bool selfHeal = kind == SpellKind::Heal &&
                        mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
                    if (CastExempt(kind, selfHeal, castLvl))
                        return aiSaysYes;   // this category is the AI's to keep
                }
                DenyLog(fid, actor, mi->GetFormID(), wantSpell, true);
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

            // CONCENTRATION IS NEVER FORCE-YESED (v1.0.53 deck freeze). The
            // force-YES below means "cast the spell in your hand NOW" -- and
            // for a concentration spell the combat AI then HOLDS the channel,
            // with the standing latch re-permitting it every cooldown. That
            // was the permanent Flames stream: Lucien streaming through
            // Xelzaz, friendly fire re-aggroing the pair every frame. The
            // bounded stream is PACKAGE-driven (Actuation's concentration
            // hold; a package cast never deliberates through this hook), so
            // the AI's own attempt at the wanted spell is denied outright --
            // exact-mode bounding with no unbounded channel left. AFTER the
            // log-mode return above: LOG mode still only observes. mi passed
            // the Is(Spell) gate above, so As<SpellItem>() resolves.
            if (auto* sp = mi->As<RE::SpellItem>();
                sp && sp->GetCastingType() == RE::MagicSystem::CastingType::kConcentration) {
                {
                    std::lock_guard<std::mutex> dl(g_denyLogMx);
                    auto& last = g_lastConcDeny[fid];
                    if (last != wantSpell) {
                        last = wantSpell;
                        spdlog::info("[consent] {:08X} {} concentration {:08X} -- AI cast "
                                     "denied (stream is package-bounded)", fid,
                                     actor->GetName() ? actor->GetName() : "?", wantSpell);
                    }
                }
                return false;
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

        // ── v1.0.36: pre-charge deny (CheckCast) + friendly-fire ────────────
        //
        // CheckStartCast (0x06) is ADVISORY -- returning false makes the AI skip
        // a cast attempt, but the deck proved a denied spell still fires (Marcurio
        // cast Icy Shard / Mage Armor while latched in "exact"). v1.0.35 hooked
        // MagicCaster::SpellCast (0x09, the release) and aborted with InterruptCast
        // -- functionally correct but the follower PLAYED the whole cast animation
        // + sound before it fizzled, and stood there holding the wrong spell
        // (marth: "wrong spell equipped, doing nothing"). v1.0.36 instead hooks
        // MagicCaster::CheckCast (0x0A) -- the "can I cast this?" gate consulted
        // BEFORE the charge -- and returns false, so the wrong spell never
        // animates. The real fix for the wrong spell in hand is keeping the
        // GAMBIT spell equipped (Loadout::Prepare's spell->spell swap); this is
        // the clean backstop. Caster-local; only the friendly-fire WORLD walk
        // needs the main thread, which it gates on explicitly (below).
        std::unordered_map<std::uintptr_t, std::uintptr_t> g_castOrig;
        std::atomic<bool> g_castHooked{ false };
        constexpr std::size_t kCheckCast = 0x0A;   // the "can I cast this?" gate, PRE-charge
        // Captured at install (kDataLoaded = main thread). The friendly-fire world
        // walk only runs when SpellCast fires on this thread; off-main it is
        // skipped (fail-open) so a jobified anim-graph release can never iterate
        // highActorHandles concurrently with a main-thread resize (§0.30).
        std::thread::id g_mainThread;

        // Shared deny decision (SpellCast side): should this LATCHED follower be
        // denied from casting this NON-gambit spell right now, per the slider?
        // Mirrors the CheckStartCast thunk's logic; outputs the gambit spell for
        // the log. Wanted spell / pacing / forcing stay in CheckStartCast.
        bool ShouldDeny(RE::FormID a_fid, RE::MagicItem* a_mi, RE::FormID& a_wantOut) {
            a_wantOut = 0;
            if (!a_mi) return false;
            const int lvl = Config::g_castControl.load();
            if (lvl <= 0 || Config::g_casterMode.load() == 0) return false;
            {
                std::shared_lock lk(g_mx);
                const auto w = g_want.find(a_fid);
                if (w == g_want.end()) return false;   // not latched -> never our deny
                a_wantOut = w->second.spell;
            }
            if (a_mi->GetFormID() == a_wantOut) return false;   // the gambit spell -> allow
            if (lvl >= 4) return true;                          // exact -> deny all non-gambit
            const SpellKind k = ClassifySpell(a_mi);
            const bool selfHeal = k == SpellKind::Heal &&
                a_mi->GetDelivery() == RE::MagicSystem::Delivery::kSelf;
            return !CastExempt(k, selfHeal, lvl);
        }

        // Distance from point p to segment [a,b] -- but only when p projects
        // BETWEEN the endpoints (an ally behind the caster or past the target is
        // not in the line of fire). Returns +inf otherwise.
        float SegDist(const RE::NiPoint3& p, const RE::NiPoint3& a, const RE::NiPoint3& b) {
            const RE::NiPoint3 ab = b - a, ap = p - a;
            const float len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
            if (len2 <= 1.0f) return ap.Length();
            const float t = (ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2;
            if (t < 0.0f || t > 1.0f) return std::numeric_limits<float>::max();
            const RE::NiPoint3 proj{ a.x + ab.x * t, a.y + ab.y * t, a.z + ab.z * t };
            return (p - proj).Length();
        }

        // Would an OFFENSIVE cast by this follower catch one of the player's OWN
        // teammates -- in the AoE blast at the target, or on the projectile line
        // between caster and target? The hallway case: Auri stood between
        // Marcurio and the foe and ate his Fireballs. Aim point = the caster's
        // current combat target. Main-thread read of highActorHandles.
        bool WouldHitTeammate(RE::Actor* a_caster, RE::MagicItem* a_mi) {
            auto tp = a_caster->GetActorRuntimeData().currentCombatTarget.get();
            auto* tgt = tp.get();
            if (!tgt) return false;                       // no aim -> can't judge, allow
            const auto cpos = a_caster->GetPosition();
            const auto tpos = tgt->GetPosition();
            const float area = static_cast<float>(a_mi->GetLargestArea());
            const float aoe  = area > 0.0f ? area + 64.0f : 0.0f;   // blast + a body
            constexpr float kLinePad = 80.0f;                        // ~a body off the line
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!pl) return false;
            for (auto& h : pl->highActorHandles) {
                auto ap = h.get();
                auto* a = ap.get();
                if (!a || a == a_caster || a == tgt) continue;
                if (a->IsDead() || a->IsDisabled()) continue;
                if (!a->IsPlayerTeammate()) continue;     // only our OWN allies matter
                const auto p = a->GetPosition();
                if (aoe > 0.0f && p.GetDistance(tpos) <= aoe) return true;   // in the blast
                if (SegDist(p, cpos, tpos) <= kLinePad)      return true;    // on the firing line
            }
            return false;
        }

        // Deduped log for the hard aborts (per follower+spell), same idea as
        // DenyLog. Cleared with the latch (Clear/ClearAll) so a later fight's
        // first abort logs again.
        std::mutex g_abortLogMx;
        std::unordered_map<RE::FormID, RE::FormID> g_lastAbort;
        void AbortLog(RE::FormID a_fid, RE::Actor* a_actor, RE::FormID a_spell, const char* a_why) {
            {
                std::lock_guard<std::mutex> lk(g_abortLogMx);
                auto it = g_lastAbort.find(a_fid);
                if (it != g_lastAbort.end() && it->second == a_spell) return;
                g_lastAbort[a_fid] = a_spell;
            }
            spdlog::info("[consent] {:08X} {} HARD-ABORTED cast of {:08X} ({})",
                         a_fid, a_actor->GetName() ? a_actor->GetName() : "?", a_spell, a_why);
        }

        // Friendly-fire DECAY. Holding a follower's every offensive cast in a
        // tight formation would soft-mute them; after kMaxFFHolds consecutive
        // holds, let one through (accept a hit) so they never freeze. Reset on
        // any cast that passes.
        constexpr int kMaxFFHolds = 3;
        std::mutex g_ffMx;
        std::unordered_map<RE::FormID, int> g_ffHold;
        bool FFHold(RE::FormID a_fid) {   // true = hold this cast, false = decay-release
            std::lock_guard<std::mutex> lk(g_ffMx);
            int& c = g_ffHold[a_fid];
            if (++c > kMaxFFHolds) { c = 0; return false; }
            return true;
        }
        void FFReset(RE::FormID a_fid) {
            std::lock_guard<std::mutex> lk(g_ffMx);
            g_ffHold.erase(a_fid);
        }

        using CheckCast_t = bool (*)(RE::MagicCaster*, RE::MagicItem*, bool, float*,
                                     RE::MagicSystem::CannotCastReason*, bool);

        // CheckCast (0x0A) is the AI's "can I cast this?" gate, consulted BEFORE
        // the charge -- returning false here means the wrong spell NEVER animates
        // (unlike the release-level SpellCast abort, which played the whole cast
        // animation + sound before fizzling). The primary fix for "wrong spell
        // equipped" is keeping the gambit spell in hand (Loadout::Prepare); this
        // is the clean backstop for any wrong spell the AI still tries to charge.
        bool CheckCastThunk(RE::MagicCaster* a_this, RE::MagicItem* a_spell, bool a_dual,
                            float* a_alch, RE::MagicSystem::CannotCastReason* a_reason,
                            bool a_useBase) {
            const auto vt  = *reinterpret_cast<std::uintptr_t*>(a_this);
            const auto oit = g_castOrig.find(vt);
            if (oit == g_castOrig.end()) return true;   // not ours -> allow (benign)
            const auto orig = reinterpret_cast<CheckCast_t>(oit->second);
            const bool aiOK = orig(a_this, a_spell, a_dual, a_alch, a_reason, a_useBase);
            if (!aiOK) return false;   // the AI itself already can't cast -> nothing to do

            static std::atomic<bool> s_threadLogged{ false };
            if (!s_threadLogged.exchange(true))
                spdlog::info("[consent] CheckCast fires on {} thread",
                             std::this_thread::get_id() == g_mainThread ? "the MAIN" : "a NON-main");

            const bool ff = Config::g_friendlyFireHold.load(std::memory_order_relaxed);
            // The fast-out stays open when cast control is at EXACT (4) -- the
            // latch-independent concentration bound (below) must still catch
            // ConcentrationCast's zero-latch early bails there -- and (#59)
            // whenever ANY follower is under the continuous cast-control
            // filter (g_ctrl non-empty): the un-latched deny below is exactly
            // the between-casts leak it closes, at partial levels too. With
            // ff defaulted ON this condition was already almost never taken.
            if (g_wantCount.load(std::memory_order_relaxed) == 0 && !ff &&
                g_ctrlCount.load(std::memory_order_relaxed) == 0 &&
                Config::g_castControl.load(std::memory_order_relaxed) < 4) return aiOK;
            // NORMAL SPELLS ONLY -- not staves/scrolls (non-Spell forms) nor
            // shouts/powers/abilities (SpellItems, but not GetSpellType()==kSpell).
            if (!a_spell || !a_spell->Is(RE::FormType::Spell) ||
                a_spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell) return aiOK;
            auto* actor = a_this->GetCasterAsActor();
            if (!actor) return aiOK;
            const auto fid = actor->GetFormID();

            // CONCENTRATION, latch-independent -- the HARD half of the bound.
            // The CheckStartCast suppression is advisory and advisory denies
            // LEAK (deck-proven, the reason this hook exists); this is the
            // pre-charge gate that actually stops a leaked stream. BEFORE
            // ShouldDeny's wanted-allow, so a latched follower's own attempt
            // at the gambit concentration spell is caught too. MFO's package
            // stream reads StreamLive and passes untouched.
            if (ConcUnboundedDeny(fid, a_spell)) {
                AbortLog(fid, actor, a_spell->GetFormID(), "concentration unbounded");
                return false;
            }

            // HARD EXCLUSIVE CONTROL: refuse a latched follower's non-gambit spell
            // at the gate -> no charge, no animation, the engine fizzles cleanly.
            // Still CALL ShouldDeny unconditionally (it populates `want`, which
            // the friendly-fire exemption and the #59 gate below both need) --
            // but on the APMF owned-cast path, do NOT ACT on the verdict: APMF's
            // own CheckCast (0x0A) hook already enforces this exact exclusivity
            // via the ch.8 claim (Phase 2, ALLOWANCE-TEMPLATE.md §7). Running
            // MFO's OWN deny here too would be a second, independently-configured
            // (iCastControl slider) enforcement of the SAME decision -- redundant,
            // and it fights APMF rather than deferring to it.
            RE::FormID want = 0;
            const bool exclusivityDeny = ShouldDeny(fid, a_spell, want);
            if (exclusivityDeny && !APMFBridge::IsOwnedCastActive(fid)) {
                AbortLog(fid, actor, a_spell->GetFormID(), "exclusive");
                return false;
            }

            // #59: CONTINUOUS CAST CONTROL, the HARD half -- the advisory
            // CheckStartCast deny above leaks (deck-proven; the reason this
            // hook exists), so the un-latched filter must stand HERE too or
            // Ice Spike still fires between forced Sunfires. want != 0 means
            // the latched logic above already governed this spell (allowed
            // gambit / exempt kind) -- never second-guess it. The explicit
            // IsInCombat gate is what CheckStartCast gets by construction:
            // CheckCast also fires OUT of combat (a follower topping up a buff
            // in town), where MFO must stay completely hands-off -- g_ctrl
            // entries can outlive the fight by up to a service round before
            // the Scheduler's non-combat Clear sweeps them.
            if (want == 0 && actor->IsInCombat()) {
                if (RE::FormID gambit = 0; CtrlUnlatchedDeny(fid, a_spell, gambit)) {
                    AbortLog(fid, actor, a_spell->GetFormID(), "continuous cast control");
                    return false;
                }
            }

            // FRIENDLY FIRE: refuse an OFFENSIVE, non-gambit, non-self cast that
            // would catch a teammate. Gambit-exempt (no force loop); the world
            // walk is main-thread-only; decay counter touched only on evaluated
            // offensive casts (a heal between shots must not reset it).
            if (ff && actor->IsPlayerTeammate() &&
                a_spell->GetFormID() != want &&
                a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf &&
                std::this_thread::get_id() == g_mainThread &&
                ClassifySpell(a_spell) == SpellKind::Offense) {
                if (WouldHitTeammate(actor, a_spell)) {
                    if (FFHold(fid)) {
                        AbortLog(fid, actor, a_spell->GetFormID(), "friendly fire");
                        return false;
                    }
                    // decay released this one (FFHold reset the count) -> allow
                } else {
                    FFReset(fid);
                }
            }
            return aiOK;
        }

        void InstallCheckCastHook() {
            if (g_castHooked.exchange(true)) return;
            if (REL::Module::IsVR()) return;   // same VR guard as InstallHook
            g_mainThread = std::this_thread::get_id();   // InstallHook runs at kDataLoaded (main)
            // ONLY vtable [0]. VTABLE_ActorMagicCaster's three entries are the three
            // BASE-SUBOBJECT vtables of ONE class (MagicCaster @0, the anim-graph
            // holder @0x48, the BSTEventSink @0x60) -- NOT separate casters. Every
            // caster dispatches CheckCast through [0]; patching [1]/[2] would clobber
            // unrelated engine vtables (Fable, 2026-08-06).
            REL::Relocation<std::uintptr_t> vt{ RE::VTABLE_ActorMagicCaster[0] };
            g_castOrig[vt.address()] = vt.write_vfunc(kCheckCast, &CheckCastThunk);
            spdlog::info("[consent] CheckCast deny hooked (ActorMagicCaster vtable[0], pre-charge)");
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
        // The slider's classifier is now spell-effect-based (ClassifySpell), so
        // the vtable list no longer carries a category -- it is just the set of
        // caster vtables whose CheckStartCast we advise on.
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
        InstallCheckCastHook();   // v1.0.36: pre-charge deny (no wrong-spell animation)
        spdlog::info("[consent] CheckStartCast hooked on {} caster vtable(s), mode={}",
                     n, Config::g_casterMode.load() == 0 ? "LOG" : "FORCE");
    }

    bool IsHooked() { return g_hooked.load(); }

    RE::FormID WantedSpell(RE::FormID a_follower) {
        std::shared_lock lk(g_mx);
        const auto it = g_want.find(a_follower);
        return it != g_want.end() ? it->second.spell : 0;
    }

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
        // #59: the continuous-control reference dies here too -- Clear's call
        // sites (combat end, dismissal) are exactly its release points; the
        // Scheduler re-populates on the next combat service tick.
        g_ctrl.erase(a_follower);
        g_ctrlCount.store(g_ctrl.size(), std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> dl(g_denyLogMx);
            g_lastDenied.erase(a_follower);
            g_lastPacedWindow.erase(a_follower);
            g_lastConcDeny.erase(a_follower);
        }
        { std::lock_guard<std::mutex> al(g_abortLogMx); g_lastAbort.erase(a_follower); }
        FFReset(a_follower);   // v1.0.35: drop the friendly-fire decay counter
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
        g_ctrl.clear();                      // #59: no filter survives revert/load
        g_ctrlCount.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> dl(g_denyLogMx);
            g_lastDenied.clear();
            g_lastPacedWindow.clear();
            g_lastConcDeny.clear();
        }
        { std::lock_guard<std::mutex> al(g_abortLogMx); g_lastAbort.clear(); }
        { std::lock_guard<std::mutex> fl(g_ffMx); g_ffHold.clear(); }
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

    void NoteGambits(RE::FormID a_follower, std::vector<RE::FormID> a_spells) {
        std::unique_lock lk(g_mx);
        if (a_spells.empty()) {
            // Cast control off / log mode / no cast gambits configured -> this
            // follower is NOT under the continuous filter. Idempotent erase.
            g_ctrl.erase(a_follower);
        } else {
            g_ctrl.insert_or_assign(a_follower, std::move(a_spells));
        }
        g_ctrlCount.store(g_ctrl.size(), std::memory_order_relaxed);
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
