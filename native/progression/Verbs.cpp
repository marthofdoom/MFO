// progression/Verbs.cpp -- THE VERBS the board calls: Enroll, SetClass,
// AllocatePerk / AllocateNextEligible, Respec, RestoreNativePerks, and the §16
// manual skill points (SetManualSkills / ApplyManualSkillPoint).
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

        // Vanilla PotentialFollowerFaction (Skyrim.esm 0x0005C84D) — the §9.5
        // provenance signal, recorded at enroll for the roster component.
        constexpr RE::FormID kPotentialFollowerFaction = 0x0005C84D;

        const Progression::SkillTree* FindTree(RE::ActorValue a_av) {
            for (const auto& tree : Progression::Get().skills)
                if (tree.av == a_av) return &tree;
            return nullptr;
        }

    }

    // ── verbs ───────────────────────────────────────────────────────────────

    bool Enroll(RE::Actor* a_actor) {
        if (!g_ready) { spdlog::info("[prog] enroll refused: addon absent"); return false; }
        if (!a_actor) { spdlog::warn("[prog] enroll refused: null actor"); return false; }
        if (Unmanaged(a_actor, "enroll")) return false;
        const auto id = a_actor->GetFormID();
        auto* base = a_actor->GetActorBase();
        if (!base) { spdlog::warn("[prog] enroll refused {:08X}: no TESNPC base", id); return false; }
        if (!Followers::IsPersistableID(id)) {
            spdlog::info("[prog] enroll refused {}: runtime (0xFF) id — never persistable", NameOf(a_actor));
            return false;
        }
        if (!base->IsUnique()) {
            // v1 scope (§15): base-array perk edits hit every actor sharing
            // the TESNPC — shared templates are out until v2's guard.
            spdlog::info("[prog] enroll refused {}: shared (non-unique) base {:08X} — v1 is unique-base only",
                         NameOf(a_actor), base->GetFormID());
            return false;
        }
        if (!Followers::IsEligibleFollower(a_actor)) {
            spdlog::info("[prog] enroll refused {}: not an eligible follower", NameOf(a_actor));
            return false;
        }
        auto& st = g_prog[id];
        if (st.enrolled) {
            spdlog::info("[prog] {} already enrolled (class {})", NameOf(a_actor), ClsName(st.clsId));
            return false;
        }
        st.enrolled = true;

        // §9.5 provenance: is this follower in the vanilla recruitable
        // faction right now? Logged AND flagged — the roster component
        // distinguishes "away on their own business" from "managed by their
        // own mod" with this bit.
        auto* pff = RE::TESForm::LookupByID<RE::TESFaction>(kPotentialFollowerFaction);
        st.wasInPotentialFollowerFaction = pff && a_actor->IsInFaction(pff);

        // The respec/un-enroll floor: the natural base of every skill at the
        // moment MFO first touched this follower.
        st.baseline.clear();
        if (auto* avo = a_actor->AsActorValueOwner()) {
            for (const auto& s : kSkillNames)
                st.baseline.push_back({ s.av, avo->GetBaseActorValue(s.av) });
            // §HMS: capture the base H/M/S floor at enrollment alongside the
            // skill baseline, so the redistribution never drives a pool below
            // the follower's true natural. hmsCaptured=true suppresses the
            // pre-v5 ADOPT fallback for a freshly enrolled follower.
            for (int p = 0; p < 3; ++p) {
                const float v = avo->GetBaseActorValue(kHmsAV[p]);
                st.hmsBaseline[p]   = v;
                st.hmsTarget[p]     = v;
                st.hmsSkew[p]       = 0.0f;
                st.hmsCumulative[p] = 0.0f;
            }
            st.hmsWithheld = 0.0f; st.hmsParityCredit = 0.0f;   // PRGN v8 parity starts clean
            st.hmsRetroPending = true;   // an engine level jump before the 1st player level-up
            for (int p = 0; p < 3; ++p) st.hmsHeld[p] = st.hmsBaseline[p];   // v8: what he holds now
            st.hmsCaptured = true;
        }

        // B′: strip the list-given catalog perks NOW (engagement); recorded for
        // the uninstall restore, never refunded (not credited, not debited).
        const int strippedN = StripNativePerks(a_actor, base, st);

        // §17: the perk-budget debit — tree ranks the follower STILL holds after
        // the strip (an actor-held rank the strip could not remove). Captured
        // ONCE, before MFO ever grants anything, so the derived pool can never
        // double-pay a pre-trained follower.
        st.nativeTreePerksAtEnroll =
            static_cast<std::uint16_t>(std::min(CountNativeTreeRanks(a_actor, base), 0xFFFF));

        spdlog::info("[prog] ENROLLED {} ({:08X}) — unique base {:08X}, PotentialFollowerFaction={}, "
                     "baseline {} skill(s) captured, {} native tree rank(s) STRIPPED (recorded), {} "
                     "left counted against the perk budget (§17). Progression INACTIVE until a class "
                     "is set (§15).",
                     NameOf(a_actor), id, base->GetFormID(),
                     st.wasInPotentialFollowerFaction ? "yes" : "NO",
                     st.baseline.size(), strippedN, st.nativeTreePerksAtEnroll);
        return true;
    }

    bool SetClass(RE::Actor* a_actor, RE::FormID a_classId) {
        if (!g_ready) { spdlog::info("[prog] set-class refused: addon absent"); return false; }
        if (!a_actor || Unmanaged(a_actor, "set-class")) return false;
        const auto id = a_actor->GetFormID();
        auto it = g_prog.find(id);
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] set-class refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        const ClassDef* def = FindClassDef(a_classId);
        if (!def) {
            // §15 + §18.6: a concrete DECLARED class is required.
            spdlog::info("[prog] set-class refused {}: {:08X} is not a declared class",
                         NameOf(a_actor), a_classId);
            return false;
        }
        auto& st = it->second;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto pl = player ? player->GetLevel() : std::uint16_t{ 1 };

        // First class pick = the on-ramp (§15): level-match to the player,
        // once. No point grant lives here any more — §17 derives the pool
        // from the level directly (floor(level/levelsPerPerkPoint) − spent), so
        // matching the level IS the catch-up.
        if (!st.veteranConsumed) {
            st.progressionLevel = pl;
            st.veteranConsumed = true;
            spdlog::info("[prog] {} level-matched to player level {} — {} perk point(s) "
                         "available (§17: floor(level/{}) − {} spent)",
                         NameOf(a_actor), pl, PerkPointsAvailable(st),
                         g_econ.levelsPerPerkPoint, AllocatedRanks(st));
        }

        const auto beforeName = std::string(ClsName(st.clsId));
        st.clsId = def->id;

        // v1.1 Phase 2: the base class (FollowerState::combatClassOverride) is the
        // stance AUTHORITY, set by the user via the Gambit tab (Board SetClassOverride)
        // and read by HMS/stance machinery through Followers::GetBaseClass. The
        // progression-tab SetClass sets the SKILL class (clsId) ONLY and must NOT
        // overwrite the base class: the old mirror wrote def->stance, which is parsed
        // from a GLOB editor-id suffix the engine discards at runtime (always 0), so it
        // only ever clobbered the user's correct Gambit pick with Auto. Mirror removed.

        RecomputeSkills(a_actor, st, /*log*/ true);
        spdlog::info("[prog] {} class {} -> \"{}\" ({:08X}) — pending auto levels granted to level {} "
                     "({} skill(s) allocated; placed points never move, the new class steers "
                     "future levels), perk allocation unlocked",
                     NameOf(a_actor), beforeName, def->name, def->id,
                     st.progressionLevel, st.skills.size());
        return true;
    }

    bool AllocatePerk(RE::Actor* a_actor, RE::FormID a_nodePerkID) {
        if (!g_ready) { spdlog::info("[prog] allocate refused: addon absent"); return false; }
        if (!a_actor || Unmanaged(a_actor, "allocate")) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] allocate refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (FindClassDef(st.clsId) == nullptr) {
            spdlog::info("[prog] allocate refused {}: no class set (the §15 gate)", NameOf(a_actor));
            return false;
        }
        auto ref = FindNode(a_nodePerkID);
        if (!ref.node) {
            spdlog::info("[prog] allocate refused {}: perk {:08X} not in the catalog "
                         "(filtered as NPC-dead, or not a tree perk)", NameOf(a_actor), a_nodePerkID);
            return false;
        }
        std::string whyNot;
        const int target = GateNextRank(a_actor, base, st, *ref.node, whyNot);
        if (target == 0) {
            // THE §5 BACKEND REJECT — named, no mutation. The UI can be stale
            // or bypassed; this line is the guarantee it cannot over-allocate.
            spdlog::info("[prog] allocate REJECTED {} / {} ({:08X}): {}",
                         NameOf(a_actor), ref.node->name, a_nodePerkID, whyNot);
            return false;
        }
        return GrantRank(a_actor, base, st, *ref.node, target);
    }

    bool AllocateNextEligible(RE::Actor* a_actor) {
        if (!g_ready || !a_actor || Unmanaged(a_actor, "auto-pick")) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        const ClassDef* def = (it != g_prog.end() && it->second.enrolled)
                                  ? FindClassDef(it->second.clsId) : nullptr;
        if (!def) {
            spdlog::info("[prog] auto-pick refused {}: not enrolled or no class", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        std::string whyNot;

        // 1) the addon-declared perk priority for this class, in list order.
        // An entry may be any rank form of a node — match either way.
        for (const auto fid : def->perkPriority) {
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
                    const bool match = node.perkFormID == fid ||
                                       std::find_if(node.ranks.begin(), node.ranks.end(),
                                                    [fid](const auto& r) { return r.perkFormID == fid; })
                                           != node.ranks.end();
                    if (!match) continue;
                    if (const int target = GateNextRank(a_actor, base, st, node, whyNot))
                        return GrantRank(a_actor, base, st, node, target);
                }
            }
        }

        // 2) name-agnostic fallback (§6): walk this class's skills in weight
        // order; within a tree, nodes in catalog (BFS ≈ depth) order —
        // deterministic under any overhaul, no EDIDs anywhere. Effective
        // ranks first; marginal only when nothing effective is takeable.
        const auto weights = WeightsFor(a_actor, *def);
        for (const bool wantEffective : { true, false }) {
            for (const auto& [av, w] : weights) {
                const auto* tree = FindTree(av);
                if (!tree) continue;
                for (const auto& node : tree->nodes) {
                    const int have = [&]() {
                        for (const auto& p : st.perks)
                            if (p.nodePerkID == node.perkFormID) return static_cast<int>(p.rank);
                        return 0;
                    }();
                    if (have >= static_cast<int>(node.ranks.size())) continue;
                    const bool effective =
                        node.ranks[static_cast<std::size_t>(have)].verdict == Progression::Verdict::kEffective;
                    if (effective != wantEffective) continue;
                    if (const int target = GateNextRank(a_actor, base, st, node, whyNot))
                        return GrantRank(a_actor, base, st, node, target);
                }
            }
        }
        spdlog::info("[prog] auto-pick {}: no eligible perk right now ({} point(s) available) — "
                     "gates (skill reqs / prereqs) block everything reachable",
                     NameOf(a_actor), PerkPointsAvailable(st));
        return false;
    }

    bool Respec(RE::Actor* a_actor) {
        if (!g_ready) { spdlog::info("[prog] respec refused: addon absent"); return false; }
        if (!a_actor || Unmanaged(a_actor, "respec")) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        const auto id = a_actor->GetFormID();
        auto it = g_prog.find(id);
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] respec refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        // A″ (marth 2026-09-13): "Respec returns ALL points" — every skill
        // point placed since enrollment, auto AND manual, plus the perks.
        float autoPlaced = 0.0f, manualPlaced = 0.0f;
        for (const auto& e : st.skills) { autoPlaced += e.autoPoints; manualPlaced += e.manualPoints; }
        if (st.perks.empty() && autoPlaced <= 0.0f && manualPlaced <= 0.0f) {
            // Nothing placed, no cost — the rapport hit is FOR the reset, and a
            // no-op reset must not bill anyone.
            spdlog::info("[prog] respec {}: nothing allocated — no perks removed, no rapport spent",
                         NameOf(a_actor));
            return false;
        }

        int removed = 0;
        for (const auto& p : st.perks) {
            auto ref = FindNode(p.nodePerkID);
            RE::BGSPerk* held = nullptr;
            if (ref.node && p.rank >= 1 && p.rank <= ref.node->ranks.size())
                held = PerkByID(ref.node->ranks[p.rank - 1].perkFormID);
            if (!held) held = PerkByID(p.nodePerkID);   // off-catalog: best effort on the node id
            if (held && base->GetPerkIndex(held).has_value()) {
                base->RemovePerk(held);
                ++removed;
            }
        }
        a_actor->ApplyPerksFromBase();   // §4.1: re-settle the actor once
        st.perks.clear();
        // §17: the refund is AUTOMATIC — clearing the allocs removes the
        // debit and the derived pool rises by exactly the ranks returned.

        // SKILLS (A″): every placed point comes off (autoPoints/manualPoints
        // -> 0) and the ledgers restart from enrollment: under AUTO the
        // RecomputeSkills below re-grants every level by the CURRENT weights;
        // under MANUAL the baseline drops to 1 so the whole pool is the
        // player's. Settles through the single ReconcileSkill (baseline floor).
        for (auto& e : st.skills) { e.autoPoints = 0.0f; e.manualPoints = 0.0f; }
        st.autoLevelsGranted    = 0;
        st.manualPointsApplied  = 0;
        st.manualExcludedLevels = 0;
        if (st.manualSkills) st.manualBaselineLevel = 1;
        RecomputeSkills(a_actor, st, /*log*/ true);

        // §15: free of gold, −500 rapport (record default; the follower
        // resents the reset). Reuses the Rapport rank machinery wholesale —
        // EXCEPT the one free post-migration respec (ProgState::freeRespec),
        // which skips the spend and clears the flag; the next one pays.
        const bool wasFree = st.freeRespec;
        if (wasFree) {
            st.freeRespec = false;
            spdlog::info("[prog] RESPEC {} -- FREE (one-time, post-migration)", NameOf(a_actor));
        } else {
            Rapport::Spend(id, g_econ.respecRapportCost, "progression respec");
        }

        spdlog::info("[prog] RESPEC {} ({:08X}): {} perk(s) removed -> {} point(s) available; "
                     "{:.0f} auto + {:.0f} manual skill point(s) returned, manual {} ({} pooled, "
                     "{} auto level(s) re-granted), rapport -{:.0f}",
                     NameOf(a_actor), id, removed, PerkPointsAvailable(st),
                     autoPlaced, manualPlaced, st.manualSkills ? "ON" : "OFF", ManualAvail(st),
                     st.autoLevelsGranted, wasFree ? 0.0f : g_econ.respecRapportCost);
        return true;
    }

    bool HasFreeRespec(RE::FormID a_actorID) {
        const auto it = g_prog.find(a_actorID);
        return it != g_prog.end() && it->second.enrolled && it->second.freeRespec;
    }

    bool RestoreNativePerks(RE::Actor* a_actor) {
        if (!a_actor) return false;
        auto* base = a_actor->GetActorBase();
        auto it = g_prog.find(a_actor->GetFormID());
        if (!base || it == g_prog.end() || !it->second.enrolled) return false;
        const int n = RestoreNativePerksImpl(a_actor, base, it->second);
        spdlog::info("[prog] {}: safe-removal restore -- {} native perk(s) back, record cleared", NameOf(a_actor), n);
        return true;
    }

    // ── §16 manual skill points ─────────────────────────────────────────────

    bool SetManualSkills(RE::Actor* a_actor, bool a_on) {
        if (!g_ready) { spdlog::info("[prog] manual-skills refused: addon absent"); return false; }
        if (!a_actor || Unmanaged(a_actor, "manual-skills")) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled || it->second.clsId == 0) {
            spdlog::info("[prog] manual-skills refused {}: not enrolled or no class (§15 gate)",
                         NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (st.manualSkills == a_on) return true;   // idempotent no-op
        // §16 (round-4 correction): manual is an OVERRIDE, never additive.
        // OFF→ON starts a fresh stint: latch the baseline at the current
        // level (auto growth FREEZES here) and zero the applied counter (the
        // pool accrues from zero). ON→OFF banks the stint's levels into
        // manualExcludedLevels so resumed auto growth can never back-fill
        // levels that were progressed manually — no double-dip in either
        // toggle direction.
        if (a_on) {
            st.manualBaselineLevel = std::max<std::uint16_t>(1, st.progressionLevel);
            st.manualPointsApplied = 0;
        } else {
            const int stint = std::max(0, static_cast<int>(st.progressionLevel) -
                                           static_cast<int>(st.manualBaselineLevel));
            st.manualExcludedLevels = static_cast<std::uint16_t>(
                std::min(st.manualExcludedLevels + stint, 0xFFFF));
        }
        st.manualSkills = a_on;
        // Re-reconcile NOW so the auto share freezes/unfreezes immediately
        // (same single SetBaseActorValue path; the baseline floor rides).
        RecomputeSkills(a_actor, st, /*log*/ true);
        spdlog::info("[prog] {} manual skill points {} (stint baseline {}, {} available, "
                     "flat {}/level, {} level(s) excluded from auto — manual REPLACES "
                     "auto growth while ON)",
                     NameOf(a_actor), a_on ? "ON" : "OFF", st.manualBaselineLevel,
                     ManualAvail(st), g_econ.manualSkillPtsPerLevel, st.manualExcludedLevels);
        return true;
    }

    bool ApplyManualSkillPoint(RE::Actor* a_actor, RE::ActorValue a_av) {
        if (!g_ready) { spdlog::info("[prog] apply-skill refused: addon absent"); return false; }
        if (!a_actor || Unmanaged(a_actor, "apply-skill")) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled || it->second.clsId == 0) {
            spdlog::info("[prog] apply-skill refused {}: not enrolled or no class", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (!st.manualSkills) {
            spdlog::info("[prog] apply-skill refused {}: manual skill points are OFF", NameOf(a_actor));
            return false;
        }
        if (!IsKnownSkillAv(static_cast<std::uint32_t>(a_av))) {
            spdlog::info("[prog] apply-skill refused {}: AV {} is not one of the 18 skills",
                         NameOf(a_actor), static_cast<std::uint32_t>(a_av));
            return false;
        }
        if (ManualAvail(st) < 1) {
            spdlog::info("[prog] apply-skill refused {}: no pooled points "
                         "(level {} - baseline {} at flat {}/level, {} applied)",
                         NameOf(a_actor), st.progressionLevel, st.manualBaselineLevel,
                         g_econ.manualSkillPtsPerLevel, st.manualPointsApplied);
            return false;
        }
        auto* avo = a_actor->AsActorValueOwner();
        if (!avo) return false;
        if (avo->GetBaseActorValue(a_av) + 0.5f >= g_econ.skillCap) {
            spdlog::info("[prog] apply-skill refused {}: {} already at the cap ({:g})",
                         NameOf(a_actor), AvName(a_av), g_econ.skillCap);
            return false;   // refuse instead of silently absorbing at the clamp
        }
        auto* e = [&]() -> SkillAlloc* {
            for (auto& s : st.skills)
                if (s.av == a_av) return &s;
            st.skills.push_back({ a_av, 0.0f, -1.0f });
            return &st.skills.back();
        }();
        e->manualPoints += 1.0f;
        st.manualPointsApplied = static_cast<std::uint16_t>(st.manualPointsApplied + 1);
        // The write goes through the SAME reconcile every other skill write
        // uses (RecomputeSkills → ReconcileSkill, the one SetBaseActorValue
        // call site) — floor discipline and exact recovery hold by
        // construction; manual is just one more term in the target.
        RecomputeSkills(a_actor, st, /*log*/ true);
        spdlog::info("[prog] {} manual +1 {} -> base {:.0f} ({} point(s) left)",
                     NameOf(a_actor), AvName(a_av), avo->GetBaseActorValue(a_av),
                     ManualAvail(st));
        return true;
    }

}
