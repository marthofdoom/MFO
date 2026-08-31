// ProgAllocator_Hms.cpp -- the §HMS class-redistribution side of the allocator
// (split mechanically out of ProgAllocator.cpp, no logic change): the class
// H/M/S profile, the F3 off-class fired-pool mirror consumers, battle-edge
// counting (HmsTrackBattle) and the redistribution/grant engine (RecomputeHMS).
// Shared state (kHmsAV, g_hmsFireMx/g_hmsFiredMask, g_classes) lives in
// ProgAllocator_internal.h; NoteCombatFire (the worker-thread producer) is
// defined here next to its consumers.
#include "ProgAllocator_internal.h"
#include <cmath>         // std::floor — not guaranteed via the PCH
#include "Config.h"      // g_hmsRedistribute / g_hmsSkewMaxFrac (main-MFO MCM)
#include "Followers.h"   // GetBaseClass / MeasureEngineVitalAward / SetFollowerHMS

namespace MFO::ProgAllocator {

    namespace {

        static const char* HmsPoolName(int p) {
            switch (p) { case 0: return "Health"; case 1: return "Magicka"; case 2: return "Stamina"; }
            return "?";
        }

        // Class HMS ratio profile keyed by base-class stance ordinal (the general
        // engine fact from GetBaseClass: 1/2/3, 0=none).
        // v1.1 Phase 7: the ratios are ADD-ON DATA (`ClassDef::hmsWeights` +
        // `primaryPool`, declared per class, parsed positionally in ParseClassDef),
        // NOT a DLL-baked switch. This walks the declared classes for the one whose
        // stance matches and returns its NORMALIZED H/M/S weights + primary pool.
        // Per the governing rule there is NO DLL default: a stance with no declared
        // class, or a class that declared no weights, yields false → HMS reshapes
        // nothing (absent add-on = no ratios). The general skew/convergence math
        // downstream is unchanged and consumes this profile exactly as before.
        //
        // Byte-identical to the retired switch for the shipped add-on: the generator
        // emits the same raw weights (Melee 60/5/35 primary Health, Ranged 40/5/55
        // primary Stamina, Mage 15/80/5 primary Magicka) and each sums to 100, so
        // `weight/sum` reproduces the former float literals exactly.
        bool HmsProfile(std::uint8_t a_stance, float a_out[3], int& a_primary) {
            if (a_stance == 0) return false;
            for (const auto& def : g_classes) {
                if (def.stance != a_stance || !def.hmsWeightsSet) continue;
                const float sum = def.hmsWeights[0] + def.hmsWeights[1] + def.hmsWeights[2];
                if (sum <= 0.0f) return false;
                a_out[0] = def.hmsWeights[0] / sum;
                a_out[1] = def.hmsWeights[1] / sum;
                a_out[2] = def.hmsWeights[2] / sum;
                a_primary = static_cast<int>(def.primaryPool);
                return true;
            }
            return false;
        }

        // Which HMS pool is the follower PHYSICALLY exercising right now, read
        // entirely from live engine state on the MAIN thread (race-free — we
        // deliberately do NOT read the worker-written Gambit.lastFired display
        // fields, which would be a #4 cross-thread read of g_followers contents;
        // physical exercise is the same "usage" signal, observed safely):
        //   a readied SPELL in either hand  → Magicka (1)
        //   a bow/crossbow                  → Stamina (2)
        //   a staff                         → Magicka (1)  (magic role)
        //   a melee weapon (1H/2H)          → Health  (0)
        //   nothing decisive                → -1
        // Gated by the caller on IsInCombat(); we additionally require the
        // weapon to be DRAWN so a sheathed idle loadout is not counted.

        // Map a FIRED combat gambit action opcode to the HMS pool it exercises:
        // 0=none/neutral, 1=Health, 2=Magicka, 3=Stamina. Opcode literals are the
        // frozen Vocabulary contract (#10); a cast of ANY delivery -> Magicka.
        std::uint8_t HmsPoolForFire(RE::Actor* a_actor, const std::string& a_op) {
            if (a_op.rfind("act.cast", 0) == 0) return 2;   // cast_self/target/player/spell -> Magicka
            if (a_op == "act.equip_ranged")     return 3;   // ranged role -> Stamina
            if (a_op == "act.equip_melee")      return 1;   // melee role  -> Health
            if (a_op == "act.attack" || a_op == "act.power_attack") {
                // A generic attack is melee OR bow depending on the drawn weapon.
                // Engine read; the Scheduler caller is on the worker (engine reads
                // are fine there, like the rest of its tick).
                if (a_actor) {
                    RE::TESForm* hands[2] = { a_actor->GetEquippedObject(false),
                                             a_actor->GetEquippedObject(true) };
                    for (auto* h : hands)
                        if (auto* w = h ? h->As<RE::TESObjectWEAP>() : nullptr) {
                            const auto t = w->GetWeaponType();
                            if (t == RE::WEAPON_TYPE::kBow || t == RE::WEAPON_TYPE::kCrossbow)
                                return 3;   // ranged attack -> Stamina
                        }
                }
                return 1;   // melee attack -> Health
            }
            return 0;   // wait / flee / equip_torch / loot / drink -> neutral
        }

        void HmsClearFiredMask(RE::FormID a_id) {
            std::scoped_lock lk(g_hmsFireMx);
            g_hmsFiredMask.erase(a_id);
        }
        std::uint8_t HmsConsumeFiredMask(RE::FormID a_id) {
            std::scoped_lock lk(g_hmsFireMx);
            auto it = g_hmsFiredMask.find(a_id);
            if (it == g_hmsFiredMask.end()) return 0;
            const std::uint8_t m = it->second;
            g_hmsFiredMask.erase(it);
            return m;
        }

    }   // anonymous namespace

        // Combat-edge battle counting for the skew usage metric. Runs every poll
        // on the MAIN thread for a resolved, active enrolled follower. Detects
        // the rising edge of a (3s-dwell-smoothed) battle and, once per battle,
        // flags whether the follower exercised an off-class pool. Both counters
        // are reset by RecomputeHMS when it consumes them for a fresh award.
        // Runtime state (hmsInBattle/hmsBattleOffCounted/hmsLastCombat) is never
        // serialized; the two COUNTS are (v5 block).
        void HmsTrackBattle(RE::Actor* a_actor, ProgState& a_st) {
            const ClassDef* def = FindClassDef(a_st.clsId);
            if (!def) return;   // enrollment/MFO-managed gate (stays on clsId)
            float prof[3]; int primary = 0;
            // v1.1 Phase 2: stance from the base Gambit class (GetBaseClass), not
            // ClassDef::stance (GLOB editor-id suffix, discarded at runtime → 0).
            if (!HmsProfile(Followers::GetBaseClass(a_actor), prof, primary)) return;

            constexpr auto kHmsCombatDwell = std::chrono::seconds(3);   // mirror Logistics shed dwell
            const auto now = std::chrono::steady_clock::now();
            const RE::FormID fid = a_actor->GetFormID();
            const bool combatNow = a_actor->IsInCombat();
            if (combatNow) a_st.hmsLastCombat = now;
            const bool recentCombat =
                combatNow || (a_st.hmsInBattle && (now - a_st.hmsLastCombat) < kHmsCombatDwell);

            if (recentCombat && !a_st.hmsInBattle) {
                // rising edge — a new battle. Drop any fires the worker published
                // during the PREVIOUS battle so they never bleed into this one.
                a_st.hmsInBattle = true;
                a_st.hmsBattleOffCounted = false;
                HmsClearFiredMask(fid);
                if (a_st.battlesSinceLevelUp < 0xFFFFFFFFu) ++a_st.battlesSinceLevelUp;
            } else if (!recentCombat && a_st.hmsInBattle) {
                a_st.hmsInBattle = false;
            }

            if (a_st.hmsInBattle && combatNow && !a_st.hmsBattleOffCounted) {
                // REAL off-class signal (F3): did the follower's combat gambit
                // actually FIRE an off-class action this battle? Consume the
                // worker-published fired-pool bitmask (bit p == pool index p).
                const std::uint8_t mask = HmsConsumeFiredMask(fid);
                int offPool = -1;
                for (int p = 0; p < 3; ++p)
                    if (p != primary && (mask & (1u << p))) { offPool = p; break; }
                if (offPool >= 0) {
                    a_st.hmsBattleOffCounted = true;
                    if (a_st.battlesOffClass < 0xFFFFFFFFu) ++a_st.battlesOffClass;
                    // First off-class pool since the last award wins (stable).
                    if (a_st.offClassPool == 0)
                        a_st.offClassPool = static_cast<std::uint8_t>(offPool + 1);
                }
            }
        }

        // The §HMS sibling of ReconcileSkill. MEASURE the engine's per-level HMS
        // award (positive drift off the held target, BEFORE re-asserting), sum
        // the three pool deltas into a live per-modlist budget, redistribute that
        // budget by class%+skew, then hold target = baseline + cumulative.
        //
        // Uncaptured baseline (pre-v5 save, or freshly enrolled without capture):
        // ADOPT the follower's current base H/M/S as the baseline once, exactly
        // like the skill ADOPT fallback — existing followers are not retro-slammed.
        // a_grantBudget > 0 → v1.1 Phase 3 fixed-stat CATCH-UP GRANT: skip the
        // (always-0) engine measurement as the budget and instead reshape this
        // INJECTED player-gain amount into the follower's pools, landing whole
        // base-AV points via hmsGrantRemainder. a_grantBudget == 0 (the default)
        // is the NORMAL engine-award path, byte-identical to Phase 2.
        void RecomputeHMS(RE::Actor* a_actor, ProgState& a_st, bool a_log, float a_grantBudget) {
            if (!Config::g_hmsRedistribute.load()) return;   // main-MFO MCM master switch
            const ClassDef* def = FindClassDef(a_st.clsId);  // enrollment/MFO gate + skew/weights
            // v1.1 Phase 2: the stance AUTHORITY is the base Gambit class
            // (FollowerState::combatClassOverride, read via GetBaseClass) — NOT
            // ClassDef::stance, which is parsed from a GLOB editor-id suffix the
            // engine DISCARDS at runtime (→ always 0 → HMS wrongly skipped). Only
            // the stance VALUE moves here; def stays the gate + skew/weights source.
            const std::uint8_t stance = Followers::GetBaseClass(a_actor);
            const auto id = a_actor->GetFormID();

            // [hms-diag] once per call (low-frequency: level-up / ~2s drift). Deferred
            // emit so earlyReturn + measured budget reflect the ACTUAL exit path.
            const char* diagExit = "none";
            float diagBudget = 0.0f;
            auto emitDiag = [&] {
                spdlog::info("[hms-diag] {:08X} clsId={:08X} def=\"{}\" defStance={} "
                             "baseClass={} earlyReturn={} redistribute={:.1f} "
                             "fixedStat={} grantBudget={:.1f}",
                             id, a_st.clsId, def ? def->name : "none",
                             def ? static_cast<int>(def->stance) : -1,
                             static_cast<int>(stance), diagExit, diagBudget,
                             a_st.fixedStat, a_grantBudget);
            };

            if (!def) { diagExit = "nodef"; emitDiag(); return; }   // no class picked, or the addon left
            float prof[3]; int primary = 0;
            if (!HmsProfile(stance, prof, primary)) { diagExit = "noprofile"; emitDiag(); return; }   // stance 0/none → skip
            if (!a_actor->AsActorValueOwner()) { diagExit = "noavo"; emitDiag(); return; }

            // MEASURE the engine's fresh per-level award via the general follower
            // API (v1.1, byte-identical to the old inline read+diff): current base
            // H/M/S into cur, signed drift off the held target into delta, clamped
            // sum into budget. Pure read (capture BEFORE any write erases it). The
            // captured check below uses cur; delta/budget are ignored on the un-
            // captured path. The signed-sum-then-floor telescopes to exactly the
            // engine award even on a pool the class profile starves (points MFO
            // previously shifted OUT re-enter as a negative delta and cancel, so a
            // positive-only sum would re-count and inflate) -- rationale lives in
            // MeasureEngineVitalAward's header.
            float cur[3]; float delta[3];
            const float measured = Followers::MeasureEngineVitalAward(a_actor, a_st.hmsTarget, cur, delta);
            // §HMS Phase 3 fixed-stat DETECTION tally: accumulate the MEASURED
            // engine award (never the injected grant) toward the next player
            // level-up's 0-award check. A grant call (a_grantBudget>0) measures 0
            // for a fixed-stat follower, so adding it is a harmless +0.
            if (measured > 0.0f) a_st.hmsAwardAccum += measured;
            // NORMAL path: budget = the measured engine award (byte-identical to
            // Phase 2). GRANT path: reshape the injected player-gain instead.
            float budget = (a_grantBudget > 0.0f) ? a_grantBudget : measured;
            diagBudget = budget;   // [hms-diag]: budget reported at whichever exit follows

            // First touch on an uncaptured record → adopt current base as the
            // baseline/target, zero the cumulative, and STOP (nothing to measure
            // yet; the next drift edge measures against this target).
            if (!a_st.hmsCaptured) {
                for (int p = 0; p < 3; ++p) {
                    a_st.hmsBaseline[p]   = cur[p];
                    a_st.hmsTarget[p]     = cur[p];
                    a_st.hmsSkew[p]       = 0.0f;
                    a_st.hmsCumulative[p] = 0.0f;
                }
                a_st.hmsCaptured = true;
                spdlog::info("[hms] {:08X} baseline ADOPTED (uncaptured record): "
                             "H {:.0f} / M {:.0f} / S {:.0f} — no retro award",
                             id, cur[0], cur[1], cur[2]);
                emitDiag();
                return;
            }

            // (cur/delta/budget were measured above via MeasureEngineVitalAward.)

            // ── Long-term CONVERGING allocation (marth 2026-08-25) ──────────────
            // The class ratio is a LONG-TERM target for the follower's RUNNING-TOTAL
            // base HMS, not a fixed per-level split. Each award nudges the total toward
            // the (skew-adjusted) target ratio, correcting past deviation incl. the
            // off-ratio pre-enrollment vanilla baseline. Budget is only ever ADDED (a
            // pool is never reduced below its baseline), so it converges over several
            // levels and individual levels differ. The SKEW is a SEMI-PERMANENT ratio
            // shift toward the off-class pool: it grows with off-class usage, HOLDS while
            // the off-class gambit keeps firing (== equipped + enabled), and decays once
            // the firing stops (gambit unequipped/disabled), whereupon the convergence
            // pulls the ratio back to pure class%.
            float award[3]  = { 0.0f, 0.0f, 0.0f };
            int   offPool   = (a_st.offClassPool >= 1 && a_st.offClassPool <= 3)
                                  ? (a_st.offClassPool - 1) : -1;
            float sk = 0.0f, usagePct = 0.0f; int skPool = -1;
            if (budget > 0.0f) {
                // (1) SEMI-PERMANENT skew fraction. offPool from THIS window resets to 0
                //     each award, so recover the currently-held skew DIRECTION from the
                //     stored array — a no-usage window must DECAY it, not wipe it.
                const float capFrac = Config::g_hmsSkewMaxFrac.load();   // MCM ceiling (F4: cap WINS)
                int   heldPool = -1; float heldSk = 0.0f;
                for (int p = 0; p < 3; ++p)
                    if (a_st.hmsSkew[p] > heldSk) { heldSk = a_st.hmsSkew[p]; heldPool = p; }
                if (offPool >= 0 && offPool != primary &&
                    a_st.battlesSinceLevelUp > 0 && a_st.battlesOffClass > 0) {
                    float usageFrac = static_cast<float>(a_st.battlesOffClass) /
                                      static_cast<float>(a_st.battlesSinceLevelUp);
                    if (usageFrac > 1.0f) usageFrac = 1.0f;
                    usagePct = usageFrac * 100.0f;
                    const float base = (heldPool == offPool) ? heldSk : 0.0f;   // same dir grows; new dir starts fresh
                    sk = std::max(base, capFrac * usageFrac);            // ratchet up + hold (semi-permanent)
                    skPool = offPool;
                } else if (heldPool >= 0 && heldPool != primary) {
                    sk = heldSk * 0.5f;                                  // no off-class usage → DECAY toward class%
                    skPool = heldPool;
                }
                if (sk > capFrac) sk = capFrac;
                if (sk < 0.01f) { sk = 0.0f; skPool = -1; }
                for (int p = 0; p < 3; ++p) a_st.hmsSkew[p] = 0.0f;      // re-derive the shape
                if (skPool >= 0 && skPool != primary && sk > 0.0f) {
                    a_st.hmsSkew[skPool]  =  sk;
                    a_st.hmsSkew[primary] = -sk;
                }

                // (2) effective target ratio = class profile shifted by the skew.
                float eprof[3];
                for (int p = 0; p < 3; ++p) eprof[p] = prof[p];
                if (skPool >= 0 && skPool != primary && sk > 0.0f) {
                    eprof[skPool]  += sk;
                    eprof[primary] -= sk;
                    if (eprof[primary] < 0.0f) { eprof[skPool] += eprof[primary]; eprof[primary] = 0.0f; }
                }

                // (3) allocate the budget to CONVERGE current totals toward eprof.
                //     Σ(eprof*total - held) == budget, so after clamping negatives the
                //     deficit sum D >= budget > 0; award splits budget by deficit share
                //     (a pool already at/over its target share gets 0 this level).
                float total = budget;
                for (int p = 0; p < 3; ++p) total += a_st.hmsBaseline[p] + a_st.hmsCumulative[p];
                float deficit[3]; float D = 0.0f;
                for (int p = 0; p < 3; ++p) {
                    const float held = a_st.hmsBaseline[p] + a_st.hmsCumulative[p];
                    deficit[p] = eprof[p] * total - held;
                    if (deficit[p] < 0.0f) deficit[p] = 0.0f;
                    D += deficit[p];
                }
                for (int p = 0; p < 3; ++p)
                    award[p] = (D > 1e-4f) ? budget * (deficit[p] / D) : budget * eprof[p];

                const bool grantMode = (a_grantBudget > 0.0f);   // fixed-stat catch-up grant
                for (int p = 0; p < 3; ++p) {
                    if (award[p] < 0.0f) award[p] = 0.0f;
                    if (grantMode) {
                        // §HMS Phase 3: land WHOLE base-AV points — carry the
                        // fraction so a 15/80/5 split accretes cleanly over levels
                        // instead of truncating (and losing) it each level.
                        const float raw   = award[p] + a_st.hmsGrantRemainder[p];
                        const float whole = std::floor(raw);
                        a_st.hmsGrantRemainder[p] = raw - whole;      // 0 <= frac < 1
                        a_st.hmsCumulative[p]    += whole;
                    } else {
                        a_st.hmsCumulative[p] += award[p];            // byte-identical to Phase 2
                    }
                }
            }

            // HOLD: target = baseline + cumulative (this REVERTS the engine's raw
            // distribution in cur and grants the reshaped total — net per-follower
            // gain == the measured budget, reshaped to the class profile).
            for (int p = 0; p < 3; ++p) {
                float tgt = a_st.hmsBaseline[p] + a_st.hmsCumulative[p];
                if (tgt < a_st.hmsBaseline[p]) tgt = a_st.hmsBaseline[p];   // floor
                a_st.hmsTarget[p] = tgt;
                if (tgt != cur[p]) Followers::SetFollowerHMS(a_actor, p, tgt);   // v1.1 API (byte-identical)
            }

            if (budget > 0.0f) {
                // REQUIRED [hms] probe (INVARIANTS #13): the measured engine
                // award, class profile, per-pool award, skew, participation, and
                // the final targets. Naturally rate-limited to award events.
                if (a_log)
                    spdlog::info("[hms] {:08X} stance {} class {:.0f}/{:.0f}/{:.0f}%: engine award "
                                 "dH {:.1f} dM {:.1f} dS {:.1f} = budget {:.1f} | skew {:.0f}% "
                                 "{}→{} @ {:.0f}% of {} battle(s) off-class | converge award "
                                 "H {:.1f} M {:.1f} S {:.1f} → base H {:.0f} M {:.0f} S {:.0f}",
                                 id, static_cast<int>(stance), prof[0]*100, prof[1]*100, prof[2]*100,
                                 delta[0], delta[1], delta[2], budget,
                                 sk*100, HmsPoolName(primary),
                                 skPool >= 0 ? HmsPoolName(skPool) : "(none)",
                                 usagePct, a_st.battlesSinceLevelUp,
                                 award[0], award[1], award[2],
                                 a_st.hmsTarget[0], a_st.hmsTarget[1], a_st.hmsTarget[2]);
                // Consume the counters: this award closes the window (== level-up
                // reset). Runtime battle-edge state is left as-is (an in-progress
                // battle keeps counting toward the NEXT window).
                // The ongoing battle (if any) belongs to the NEW window: its rising
                // edge was consumed by the window just closed, so seed the counter to 1.
                // Otherwise a re-count of this same battle as off-class gives bOff=1 /
                // bSince=0, which both drops the >=1 floor here (guard needs bSince>0) and
                // is zeroed by CoSaveLoad's min(bOff,bSince) clamp. hmsInBattle unchanged.
                a_st.battlesSinceLevelUp = a_st.hmsInBattle ? 1u : 0u;
                a_st.battlesOffClass     = 0;
                a_st.offClassPool        = 0;
                a_st.hmsBattleOffCounted = false;
            }
            emitDiag();   // [hms-diag]: full path, earlyReturn=none, redistribute=measured budget
        }

    // §HMS off-class usage (F3): the combat scheduler (worker thread) publishes a
    // FIRED combat gambit action's exercised pool here; HmsTrackBattle (main poll)
    // consumes it. Self-gating — a no-op when the feature is off or the action is
    // neutral. Mutex-guarded, race-free, never touches g_followers/g_prog.
    void NoteCombatFire(RE::Actor* a_actor, const std::string& a_actionOpcode) {
        if (!Config::g_hmsRedistribute.load()) return;   // feature off — don't accumulate
        if (!a_actor) return;
        const std::uint8_t pool = HmsPoolForFire(a_actor, a_actionOpcode);
        if (pool == 0) return;                            // neutral action — nothing to record
        std::scoped_lock lk(g_hmsFireMx);
        g_hmsFiredMask[a_actor->GetFormID()] |= static_cast<std::uint8_t>(1u << (pool - 1));
    }

}
