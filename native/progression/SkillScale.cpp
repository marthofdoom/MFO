// progression/SkillScale.cpp -- SKILL POINTS: the §4.2 skill reconcile, the §6
// class weights (WeightsFor + the dominant-sibling picks) and the §15 auto-scale
// (RecomputeSkills).
// Split out of the old native/ProgAllocator.cpp by the wave-1 subsystem-folder
// split (2026-09-24): a pure move, proven function by function with tools/splitcheck.
// The allocator's contract and design references are at the top of
// progression/Allocator.cpp.
#include "PCH.h"
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "ProgAllocator_internal.h"   // the split family's shared substrate
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

namespace MFO::ProgAllocator {

    namespace {

        // The follower's TRUE natural for this skill, captured at enrollment
        // (§8 enrollBaseline). <0 = not captured (no floor available).
        float BaselineFloor(const ProgState& a_st, RE::ActorValue a_av) {
            for (const auto& b : a_st.baseline)
                if (b.av == a_av) return b.value;
            return -1.0f;
        }

        // ── the §4.2 skill reconcile (P2-proven) ────────────────────────────
        //
        //   cur     = GetBaseActorValue(av)
        //   natural = cancelEngineAwards ? enrollmentBaseline(a_floor)   // REVERT
        //                                : (cur==lastWrittenBase ? lastWrittenBase-points  // ADOPT
        //                                                        : cur)
        //   natural = max(natural, baselineFloor)            // hard floor
        //   desired = clamp(natural + newPoints, natural, cap)
        //   desired = max(desired, baselineFloor)            // belt-and-braces
        //   points  = desired - natural                      // APPLIED delta
        //
        // TWO SKILL MODELS, one call site (marth #74 revert-engine-awards):
        //  • cancelEngineAwards ON (DEFAULT): natural is FROZEN at the enrollment
        //    baseline (a_floor IS the serialized enrollBaseline, §8) — engine
        //    autocalc/level-up drift in `cur` is IGNORED and CLOBBERED by the
        //    single SetBaseActorValue below, so the base skill is pure MFO
        //    (enrollBaseline + MFOaward). Because MFO writes the BASE AV the value
        //    is PERMANENT if MFO is removed; the ~2s drift-watch re-applies each
        //    cycle so per-level engine gains are cancelled continuously — no new
        //    event hook. If a_floor<0 (baseline uncaptured, old save) it falls
        //    back to the ADOPT path so nothing regresses.
        //  • cancelEngineAwards OFF (compat, byte-identical to the shipped path):
        //    an engine recompute (autocalc, Requiem, level-up) is ADOPTED as the
        //    new natural rather than fought — engine leveling + MFO stack.
        //
        // TWO GUARANTEES (adversarial review of 9f45f3b, SEV-1):
        //  1. `points` records the delta actually WRITTEN (desired − natural),
        //     never the requested amount. Under cap saturation the requested
        //     and applied deltas differ; storing the request made the next
        //     recovery (`lastWrittenBase − points`) under-shoot natural, and a
        //     later SHRINK (class change, drift-watch dominance re-pick) then
        //     wrote the base BELOW the follower's true natural — permanently,
        //     into the save. Applied-delta storage makes recovery exact.
        //  2. The enrollment BASELINE is a hard floor on both the natural
        //     estimate and the written value: this is the ONLY call site of
        //     SetBaseActorValue in the module, so no path can ever leave a
        //     base skill below the captured natural — and a save corrupted by
        //     the pre-fix build self-heals here on its first post-load
        //     reconcile (fix-forward: stop writing it AND sweep it).
        void ReconcileSkill(RE::ActorValueOwner* a_avo, SkillAlloc& a_e,
                            float a_newPoints, float a_floor,
                            RE::FormID a_who, bool a_log) {
            const float cur = a_avo->GetBaseActorValue(a_e.av);
            float natural;
            if (g_econ.cancelEngineAwards && a_floor >= 0.0f) {
                // REVERT mode: freeze natural at the enrollment baseline and
                // discard engine drift in `cur`. The write below clobbers the
                // drift; the drift-watch re-runs this each cycle to cancel it.
                natural = a_floor;
            } else {
                // ADOPT mode (compat, or baseline uncaptured): the shipped path.
                natural = (a_e.lastWrittenBase < 0.0f)
                              ? cur
                              : (cur == a_e.lastWrittenBase ? a_e.lastWrittenBase - a_e.points
                                                            : cur);
            }
            if (a_floor >= 0.0f && natural < a_floor) natural = a_floor;
            float desired = std::max(natural,
                                     std::min(natural + a_newPoints, g_econ.skillCap));
            if (a_floor >= 0.0f && desired < a_floor) desired = a_floor;   // provably redundant
                                                                           // after the natural
                                                                           // clamp; kept as the
                                                                           // stated invariant
            if (desired != cur) {
                a_avo->SetBaseActorValue(a_e.av, desired);
                if (a_log)
                    spdlog::info("[prog] {:08X} skill {}: base {:.1f} -> {:.1f} "
                                 "(natural {:.1f} + alloc {:.0f}, floor {:.1f}, cap {:.0f})",
                                 a_who, AvName(a_e.av), cur, desired, natural,
                                 desired - natural, a_floor, g_econ.skillCap);
            }
            a_e.points = desired - natural;   // APPLIED delta — exact recovery
            a_e.lastWrittenBase = desired;
        }

        // ── class weights (§6, ESL-authored order) ──────────────────────────
        //
        // The ESL FormList gives the ORDER; the weight is triangular over the
        // post-prune list (N entries: first = N/T, last = 1/T, T = N(N+1)/2).
        // With the shipped 6-entry Melee list pruned to 4 that lands on the
        // design's exact 40/30/20/10.
        //
        // SIBLING PRUNING (the documented convention): when a class list
        // carries BOTH weapon skills (OneHanded/TwoHanded) or BOTH armor
        // skills (Heavy/Light), the DLL keeps only the DOMINANT one for this
        // follower — the HIGHER BASE SKILL, ties to OneHanded / Light (the
        // Logistics ComputeWeaponRoles / ArmorClassSuits rule). That is how
        // "dominant weapon 40%" stays name-agnostic and author-tunable at
        // once: the author lists both siblings, the follower picks theirs.
        //
        // STRICT POINTS (marth 2026-09-13): dominance is read from the SKILLS
        // ONLY — never from the equipped weapon or the worn body armor. The
        // old loadout read re-homed the whole sibling share every ~2 s drift-
        // watch cycle on every equip/loot change (a looted light cuirass
        // moved the armor share from Heavy to Light and back), which is the
        // "fluidly managed" skill drift the player saw under points he had
        // placed himself. A base-skill compare is a fixed point: the sibling
        // holding the share grows further ahead, so the pick is stable until
        // the OTHER sibling genuinely overtakes it — which only the player's
        // own §16 manual points (or a class change / respec) can cause.
        // "Highest skill wins" — the same rule Part B uses for equipment.
        RE::ActorValue DominantWeaponSkill(RE::Actor* a_actor) {
            auto* avo = a_actor->AsActorValueOwner();
            if (avo && avo->GetBaseActorValue(RE::ActorValue::kTwoHanded) >
                           avo->GetBaseActorValue(RE::ActorValue::kOneHanded))
                return RE::ActorValue::kTwoHanded;
            return RE::ActorValue::kOneHanded;
        }

        RE::ActorValue DominantArmorSkill(RE::Actor* a_actor) {
            auto* avo = a_actor->AsActorValueOwner();
            // Ties fall to LIGHT, the Logistics::ArmorClassSuits rule.
            if (avo && avo->GetBaseActorValue(RE::ActorValue::kHeavyArmor) >
                           avo->GetBaseActorValue(RE::ActorValue::kLightArmor))
                return RE::ActorValue::kHeavyArmor;
            return RE::ActorValue::kLightArmor;
        }

    }

        std::vector<std::pair<RE::ActorValue, float>> WeightsFor(RE::Actor* a_actor,
                                                                 const ClassDef& a_def) {
            std::vector<std::pair<RE::ActorValue, float>> out;
            const auto& spec = a_def;
            if (spec.skills.empty()) return out;

            const bool bothWeapon =
                std::find(spec.skills.begin(), spec.skills.end(), RE::ActorValue::kOneHanded) != spec.skills.end() &&
                std::find(spec.skills.begin(), spec.skills.end(), RE::ActorValue::kTwoHanded) != spec.skills.end();
            const bool bothArmor =
                std::find(spec.skills.begin(), spec.skills.end(), RE::ActorValue::kHeavyArmor) != spec.skills.end() &&
                std::find(spec.skills.begin(), spec.skills.end(), RE::ActorValue::kLightArmor) != spec.skills.end();
            const auto weapon = bothWeapon ? DominantWeaponSkill(a_actor) : RE::ActorValue::kNone;
            const auto armor  = bothArmor  ? DominantArmorSkill(a_actor)  : RE::ActorValue::kNone;

            std::vector<RE::ActorValue> kept;
            for (const auto av : spec.skills) {
                if (bothWeapon && (av == RE::ActorValue::kOneHanded || av == RE::ActorValue::kTwoHanded)
                    && av != weapon) continue;
                if (bothArmor && (av == RE::ActorValue::kHeavyArmor || av == RE::ActorValue::kLightArmor)
                    && av != armor) continue;
                if (std::find_if(kept.begin(), kept.end(),
                                 [av](RE::ActorValue k) { return k == av; }) == kept.end())
                    kept.push_back(av);
            }

            const std::size_t n = kept.size();
            const float total = static_cast<float>(n * (n + 1)) / 2.0f;
            for (std::size_t i = 0; i < n; ++i)
                out.emplace_back(kept[i], static_cast<float>(n - i) / total);
            return out;
        }

        // ── the auto-scale (§15) — PRGN v7 ACCUMULATOR (marth: "auto placed
        // skill points are equally as permanent, there is no shift"). 1) GRANT
        // the not-yet-granted auto levels × skillPointsPerLevel by the weights
        // AT THIS MOMENT (largest remainder) into autoPoints — a placed point is
        // never read or moved. 2) HOLD natural + auto + manual through the single
        // ReconcileSkill. Respec zeroes both ledgers; this step re-grants (A″).
        void RecomputeSkills(RE::Actor* a_actor, ProgState& a_st, bool a_log) {
            const ClassDef* def = FindClassDef(a_st.clsId);
            if (!def) return;   // no class picked, or the declaring addon left
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) {
                spdlog::warn("[prog] {:08X}: no ActorValueOwner — skill auto-scale skipped",
                             a_actor->GetFormID());
                return;
            }
            const auto id = a_actor->GetFormID();

            const int effAutoLvl = std::max(0,
                (a_st.manualSkills ? static_cast<int>(a_st.manualBaselineLevel)
                                   : static_cast<int>(a_st.progressionLevel)) -
                static_cast<int>(a_st.manualExcludedLevels));
            const int targetGranted = std::max(0, effAutoLvl - 1);
            const int pending = targetGranted - static_cast<int>(a_st.autoLevelsGranted);

            // 1) GRANT the pending levels by the weights of this moment. The
            // EXACT float share goes into autoPoints (Fable F3 on 4a62688: a
            // per-level largest-remainder split starved the 10 % skill of a
            // 40/30/20/10 class FOREVER at the shipped 5 pts/level — 2/2/1/0 every
            // level); the whole-point floor is taken at HOLD time below, so the
            // fractions carry across levels and every skill converges on its
            // exact share (20 levels × 5 → 40/30/20/10, not 40/40/20/0).
            if (pending > 0) {
                const float pts = g_econ.skillPointsPerLevel * static_cast<float>(pending);
                const auto weights = WeightsFor(a_actor, *def);
                if (pts > 0.0f && !weights.empty()) {
                    std::string diag;
                    for (const auto& [av, w] : weights) {
                        const float share = pts * w;
                        if (share <= 0.0f) continue;
                        auto* e = [&]() -> SkillAlloc* {
                            for (auto& x : a_st.skills)
                                if (x.av == av) return &x;
                            a_st.skills.push_back({ av, 0.0f, -1.0f });
                            return &a_st.skills.back();
                        }();
                        e->autoPoints += share;
                        diag += std::format("{}{}+{:.2f}={:.2f}", diag.empty() ? "" : " ", AvName(av), share, e->autoPoints);
                    }
                    if (a_log)
                        spdlog::info("[prog] {:08X}: GRANT {} auto level(s) = {:g} point(s) by class \"{}\": {} "
                                     "(granted levels {} -> {}; placed points never move)",
                                     id, pending, pts, def->name, diag, a_st.autoLevelsGranted, targetGranted);
                }
                a_st.autoLevelsGranted = static_cast<std::uint16_t>(std::min(targetGranted, 0xFFFF));
            }

            // 2) HOLD. Entries with nothing placed settle back to natural and
            // drop (the baseline floor rides: a shrink can never land below the
            // captured natural — SEV-1 guarantee, single choke point).
            for (auto it = a_st.skills.begin(); it != a_st.skills.end();) {
                if (it->autoPoints <= 0.0f && it->manualPoints <= 0.0f) {
                    ReconcileSkill(avo, *it, 0.0f, BaselineFloor(a_st, it->av), id, a_log);
                    it = a_st.skills.erase(it);
                } else {
                    ++it;
                }
            }
            // Whole points only reach the actor: floor the exact auto ledger
            // (+1e-3 so an accumulated 39.99999f reads as 40); the fraction
            // stays in autoPoints for the next grant to complete.
            for (auto& e : a_st.skills)
                ReconcileSkill(avo, e, std::floor(e.autoPoints + 1.0e-3f) + e.manualPoints,
                               BaselineFloor(a_st, e.av), id, a_log);
        }

}
