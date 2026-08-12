#include "PCH.h"
#include <cmath>   // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "Progression.h"
#include "Board.h"
#include "Config.h"
#include "Followers.h"
#include "Rapport.h"
#include "MainThread.h"
#include "Serialization.h"
#include "State.h"

// The allocator backend. See ProgAllocator.h for the contract;
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §4/§5/§6/§8/§15 for the design.
//
// Every engine write here is one of the three ProgProbe-proven mechanisms —
// nothing in this file tries an unproven RE:: path:
//   perks  : TESNPC::AddPerk/RemovePerk + Actor::ApplyPerksFromBase (P1)
//   skills : GetBaseActorValue/SetBaseActorValue + the §4.2 reconcile (P2)
//   reapply: GetPerkIndex-guarded, ApplyPerksFromBase only on change (P3)
//
// Every log line carries the [prog] tag. Log the ZERO cases too — "found
// none" and "never ran" must not look the same (INVARIANTS #46).

namespace MFO::ProgAllocator {

    namespace {

        // ── economy — the ESL's GLOB record defaults, latched at Init ───────
        // (§10: GLOB values are save-persisted; only the kDataLoaded read of
        // the record default is trustworthy. The DLL defaults below are what
        // a missing record degrades to, each with its own named log line.)
        struct Economy {
            float perkPointsPerLevel  = 1.0f;    // §15: copies the player
            float skillPointsPerLevel = 3.0f;    // §6 proposal, ESL-tunable
            int   sharedGrowthDivisor = 2;       // §15: benched = half rate
            float respecRapportCost   = 500.0f;  // §15: respec costs rapport
            float veteranCatchupMult  = 1.0f;    // scales the one-shot grant
            float skillCap            = 100.0f;
        };
        Economy g_econ;

        bool           g_ready  = false;    // Detected + economy latched
        RE::TESGlobal* g_devCmd = nullptr;  // harness selector — read LIVE on
                                            // purpose (console `set` writes the
                                            // live value); dev-only, never a
                                            // gameplay input

        // ── class specs (ESL FormLists, DLL fallback) ───────────────────────
        struct ClassSpec {
            std::vector<RE::ActorValue> skills;        // priority order
            std::vector<RE::FormID>     perkPriority;  // may be empty (shipped so)
        };
        ClassSpec g_class[4];   // indexed by Class ordinal; [0] unused

        // ── the poll (level-with-player, §6/§15) ────────────────────────────
        // A MainThread::Post self-chain (the ProgProbe DelayedTick shape).
        // Generation-guarded: revert/reload bumps g_pollGen so a stale chain
        // drops itself even if MainThread::Clear missed it (belt + braces).
        int g_pollGen    = 0;
        int g_pollFrames = 0;
        constexpr int kPollFrames = 120;   // ~2s at 60fps

        // The one global economy anchor: the player level the last poll saw.
        // Serialized in the PRGN header so benched half-rate accrual survives
        // a save/load without re-observing (and never double-grants).
        std::uint16_t g_lastPlayerLevel = 0;

        // ── co-save ingestion bounds (INVARIANTS #11) ───────────────────────
        constexpr std::uint32_t kMaxProgFollowers = 4096;
        constexpr std::uint16_t kMaxPerkAllocs    = 1024;
        constexpr std::uint16_t kMaxSkillAllocs   = 64;

        // ── the 18 skills (display + baseline; same set as the catalog) ─────
        constexpr struct { RE::ActorValue av; const char* name; } kSkillNames[] = {
            { RE::ActorValue::kOneHanded,   "OneHanded" },
            { RE::ActorValue::kTwoHanded,   "TwoHanded" },
            { RE::ActorValue::kArchery,     "Archery" },
            { RE::ActorValue::kBlock,       "Block" },
            { RE::ActorValue::kHeavyArmor,  "HeavyArmor" },
            { RE::ActorValue::kLightArmor,  "LightArmor" },
            { RE::ActorValue::kDestruction, "Destruction" },
            { RE::ActorValue::kRestoration, "Restoration" },
            { RE::ActorValue::kConjuration, "Conjuration" },
            { RE::ActorValue::kAlteration,  "Alteration" },
            { RE::ActorValue::kIllusion,    "Illusion" },
            { RE::ActorValue::kSneak,       "Sneak" },
            { RE::ActorValue::kSmithing,    "Smithing" },
            { RE::ActorValue::kAlchemy,     "Alchemy" },
            { RE::ActorValue::kEnchanting,  "Enchanting" },
            { RE::ActorValue::kLockpicking, "Lockpicking" },
            { RE::ActorValue::kPickpocket,  "Pickpocket" },
            { RE::ActorValue::kSpeech,      "Speech" },
        };

        const char* AvName(RE::ActorValue a_av) {
            for (const auto& s : kSkillNames)
                if (s.av == a_av) return s.name;
            return "?";
        }

        // Is this raw co-save ordinal one of the 18 skill AVs this module
        // ever writes? Ingestion defense (INVARIANTS #11): a garbage av fed
        // to Get/SetBaseActorValue would index the engine's AV array out of
        // bounds — validate the VALUE, not just the count.
        bool IsKnownSkillAv(std::uint32_t a_raw) {
            for (const auto& s : kSkillNames)
                if (static_cast<std::uint32_t>(s.av) == a_raw) return true;
            return false;
        }

        const char* NameOf(RE::TESForm* a_form) {
            if (!a_form) return "<none>";
            const char* n = a_form->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        // Vanilla PotentialFollowerFaction (Skyrim.esm 0x0005C84D) — the §9.5
        // provenance signal, recorded at enroll for the roster component.
        constexpr RE::FormID kPotentialFollowerFaction = 0x0005C84D;

        // ── catalog access (component 1 — consumed, never rewritten) ────────

        struct NodeRef {
            const Progression::SkillTree*    tree = nullptr;
            const Progression::PerkNodeView* node = nullptr;
        };

        // Find a node by its identity (the rank-1 perk FormID). Linear scan —
        // a few hundred nodes, and only on user-driven verbs, never per tick.
        NodeRef FindNode(RE::FormID a_nodePerkID) {
            for (const auto& tree : Progression::Get().skills)
                for (const auto& node : tree.nodes)
                    if (node.perkFormID == a_nodePerkID) return { &tree, &node };
            return {};
        }

        const Progression::SkillTree* FindTree(RE::ActorValue a_av) {
            for (const auto& tree : Progression::Get().skills)
                if (tree.av == a_av) return &tree;
            return nullptr;
        }

        // §15 scarcity: (ranks a follower can use ÷ ranks the player can use).
        // Both numbers come from the frozen catalog; the floor keeps a
        // degenerate load order (0 effective ranks read) from zeroing all
        // earning forever — it would still be visibly broken in the census.
        double ScarcityRatio() {
            const auto& c = Progression::Get();
            if (!c.built || c.totalRanks <= 0) return 1.0;
            return std::clamp(static_cast<double>(c.effectiveRanks) / c.totalRanks, 0.01, 1.0);
        }

        // ── ownership questions ─────────────────────────────────────────────

        PerkAlloc* FindAlloc(ProgState& a_st, RE::FormID a_nodePerkID) {
            for (auto& p : a_st.perks)
                if (p.nodePerkID == a_nodePerkID) return &p;
            return nullptr;
        }

        // Does the follower own ANY rank of this node — through MFO's alloc,
        // or natively (Requiem/SPID granted)? The prereq gate (§5.1) needs
        // "owned at all"; the parent may itself be a FILTERED perk that never
        // reaches the board, so this must work from a bare FormID too.
        bool OwnsAnyRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         const ProgState& a_st, RE::FormID a_nodePerkID) {
            for (const auto& p : a_st.perks)
                if (p.nodePerkID == a_nodePerkID && p.rank > 0) return true;
            // Native ownership: check every rank form when the node is in the
            // catalog, else just the id we were handed.
            if (auto ref = FindNode(a_nodePerkID); ref.node) {
                for (const auto& r : ref.node->ranks) {
                    if (auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(r.perkFormID)) {
                        if (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk))
                            return true;
                    }
                }
                return false;
            }
            auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(a_nodePerkID);
            return perk && (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk));
        }

        // The follower's TRUE natural for this skill, captured at enrollment
        // (§8 enrollBaseline). <0 = not captured (no floor available).
        float BaselineFloor(const ProgState& a_st, RE::ActorValue a_av) {
            for (const auto& b : a_st.baseline)
                if (b.av == a_av) return b.value;
            return -1.0f;
        }

        // §16 manual pool — a PURE FUNCTION of serialized baselines, never an
        // incremental accumulator (the round-2 SEV-1 lesson: accumulators
        // drift under replay; a formula cannot). Self-clamping: an economy
        // GLOB lowered between sessions can make applied exceed accrued —
        // that reads as 0 available, never a negative or a corrupt state.
        int ManualAvail(const ProgState& a_st) {
            if (!a_st.manualSkills || a_st.manualBaselineLevel == 0) return 0;
            const int lvls = std::max(0, static_cast<int>(a_st.progressionLevel) -
                                          static_cast<int>(a_st.manualBaselineLevel));
            const int accrued = static_cast<int>(std::floor(
                static_cast<float>(lvls) * g_econ.skillPointsPerLevel + 1e-4f));
            return std::max(0, accrued - static_cast<int>(a_st.manualPointsApplied));
        }

        // ── the §4.2 skill reconcile (P2-proven) ────────────────────────────
        //
        //   cur     = GetBaseActorValue(av)
        //   natural = (cur == lastWrittenBase) ? lastWrittenBase - points : cur
        //   natural = max(natural, baselineFloor)            // hard floor
        //   desired = clamp(natural + newPoints, natural, cap)
        //   desired = max(desired, baselineFloor)            // belt-and-braces
        //   points  = desired - natural                      // APPLIED delta
        //
        // Idempotent and convergent: an engine recompute (autocalc, Requiem,
        // level-up) is ADOPTED as the new natural rather than fought.
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
            float natural = (a_e.lastWrittenBase < 0.0f)
                                ? cur
                                : (cur == a_e.lastWrittenBase ? a_e.lastWrittenBase - a_e.points
                                                              : cur);
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
        // follower — dominance read from the loadout (equipped weapon type /
        // worn body-armor class), falling back to the higher base AV. That is
        // how "dominant weapon 40%" stays name-agnostic and author-tunable at
        // once: the author lists both siblings, the follower picks theirs.
        RE::ActorValue DominantWeaponSkill(RE::Actor* a_actor) {
            if (auto* obj = a_actor->GetEquippedObject(false)) {
                if (auto* weap = obj->As<RE::TESObjectWEAP>()) {
                    switch (weap->GetWeaponType()) {
                    case RE::WEAPON_TYPE::kTwoHandSword:
                    case RE::WEAPON_TYPE::kTwoHandAxe:
                        return RE::ActorValue::kTwoHanded;
                    case RE::WEAPON_TYPE::kOneHandSword:
                    case RE::WEAPON_TYPE::kOneHandDagger:
                    case RE::WEAPON_TYPE::kOneHandAxe:
                    case RE::WEAPON_TYPE::kOneHandMace:
                        return RE::ActorValue::kOneHanded;
                    default:
                        break;   // bow/staff/fists say nothing about melee dominance
                    }
                }
            }
            auto* avo = a_actor->AsActorValueOwner();
            if (avo && avo->GetBaseActorValue(RE::ActorValue::kTwoHanded) >
                           avo->GetBaseActorValue(RE::ActorValue::kOneHanded))
                return RE::ActorValue::kTwoHanded;
            return RE::ActorValue::kOneHanded;
        }

        RE::ActorValue DominantArmorSkill(RE::Actor* a_actor) {
            using AT = RE::BGSBipedObjectForm::ArmorType;
            if (auto* body = a_actor->GetWornArmor(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
                switch (body->GetArmorType()) {
                case AT::kHeavyArmor: return RE::ActorValue::kHeavyArmor;
                case AT::kLightArmor: return RE::ActorValue::kLightArmor;
                default: break;   // clothing decides nothing
                }
            }
            auto* avo = a_actor->AsActorValueOwner();
            // Ties fall to LIGHT, the Logistics::ArmorClassSuits rule.
            if (avo && avo->GetBaseActorValue(RE::ActorValue::kHeavyArmor) >
                           avo->GetBaseActorValue(RE::ActorValue::kLightArmor))
                return RE::ActorValue::kHeavyArmor;
            return RE::ActorValue::kLightArmor;
        }

        std::vector<std::pair<RE::ActorValue, float>> WeightsFor(RE::Actor* a_actor, Class a_cls) {
            std::vector<std::pair<RE::ActorValue, float>> out;
            const auto& spec = g_class[static_cast<std::size_t>(a_cls)];
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

        // ── the auto-scale (§15: AUTO skills, by class, to level) ───────────
        // total = skillPointsPerLevel × (progressionLevel − 1), split by the
        // class weights, applied through the reconcile. Recomputed whole on
        // every level gain / class change; entries leaving the set settle
        // back to natural and are dropped — UNLESS they carry §16 manual
        // points, which are ADDITIVE on top of the class share and survive
        // any class change (the manual override outranks the auto default).
        void RecomputeSkills(RE::Actor* a_actor, ProgState& a_st, bool a_log) {
            if (a_st.cls == Class::kNone) return;
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) {
                spdlog::warn("[prog] {:08X}: no ActorValueOwner — skill auto-scale skipped",
                             a_actor->GetFormID());
                return;
            }

            const float total = g_econ.skillPointsPerLevel *
                                static_cast<float>(std::max(0, a_st.progressionLevel - 1));
            const auto weights = WeightsFor(a_actor, a_st.cls);

            // Desired AUTO points per skill (deterministic: round-half-up).
            std::vector<std::pair<RE::ActorValue, float>> desired;
            for (const auto& [av, w] : weights)
                desired.emplace_back(av, std::floor(total * w + 0.5f));
            auto autoFor = [&](RE::ActorValue av) -> float {
                for (const auto& d : desired)
                    if (d.first == av) return d.second;
                return 0.0f;
            };

            const auto id = a_actor->GetFormID();

            // 1) skills leaving the allocation entirely (no auto share, no
            // manual points) settle back to natural and drop. The baseline
            // floor rides along: a shrink can never land below the captured
            // natural (SEV-1 guarantee, single choke point).
            for (auto it = a_st.skills.begin(); it != a_st.skills.end();) {
                const bool keep = it->manualPoints > 0.0f ||
                                  autoFor(it->av) > 0.0f;
                if (!keep) {
                    ReconcileSkill(avo, *it, 0.0f, BaselineFloor(a_st, it->av), id, a_log);
                    it = a_st.skills.erase(it);
                } else {
                    ++it;
                }
            }
            // 2) ensure an entry exists for every auto-desired skill, then
            // reconcile EVERY kept entry to auto + manual in one pass — one
            // target, one call site, exact recovery.
            for (const auto& [av, pts] : desired) {
                (void)pts;
                const bool have = std::find_if(a_st.skills.begin(), a_st.skills.end(),
                                               [&](const SkillAlloc& s) { return s.av == av; })
                                  != a_st.skills.end();
                if (!have) a_st.skills.push_back({ av, 0.0f, -1.0f });
            }
            for (auto& e : a_st.skills)
                ReconcileSkill(avo, e, autoFor(e.av) + e.manualPoints,
                               BaselineFloor(a_st, e.av), id, a_log);
        }

        // ── perk grant plumbing (P1/P3-proven paths only) ───────────────────

        RE::BGSPerk* PerkByID(RE::FormID a_id) {
            return a_id ? RE::TESForm::LookupByID<RE::BGSPerk>(a_id) : nullptr;
        }

        // The §5 double gate. Returns the 1-based rank this follower may take
        // NEXT on this node, or 0 with a_whyNot filled. Pure read — callers
        // decide whether the refusal is worth a log line (AllocatePerk always
        // logs; the auto-picker probes many nodes quietly).
        int GateNextRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         ProgState& a_st, const Progression::PerkNodeView& a_node,
                         std::string& a_whyNot) {
            auto* alloc = FindAlloc(a_st, a_node.perkFormID);
            const int have = alloc ? alloc->rank : 0;

            // v1: never build on NATIVE ownership. If the load order (Requiem
            // stats, SPID distributions) already gave the follower this perk,
            // MFO neither stacks on it nor ever removes it at respec — the
            // clean ownership boundary for the unique-base v1.
            if (have == 0 && OwnsAnyRank(a_actor, a_base, a_st, a_node.perkFormID)) {
                a_whyNot = "owned natively (granted by the load order, not MFO)";
                return 0;
            }
            if (have >= static_cast<int>(a_node.ranks.size())) {
                a_whyNot = std::format("already at max rank {}", have);
                return 0;
            }
            // The L1 companion: an upgrade (have > 0) requires MFO's OWN rank
            // form to actually be on the base. When reapply deferred to a
            // natively-appeared rank, granting rank N+1 would RemovePerk a
            // form MFO never placed and stack entries on the native one —
            // the node is frozen until the native grant goes away or respec
            // clears the alloc.
            if (have > 0) {
                auto* ours = PerkByID(a_node.ranks[static_cast<std::size_t>(have - 1)].perkFormID);
                if (!ours || !a_base->GetPerkIndex(ours).has_value()) {
                    // ASCII only -- renders in the board's locked tooltip.
                    a_whyNot = "MFO's rank is not on the base (native rank owns this node) -- frozen";
                    return 0;
                }
            }
            const auto& rank = a_node.ranks[static_cast<std::size_t>(have)];
            if (rank.verdict == Progression::Verdict::kDead) {
                a_whyNot = std::format("rank {} is not NPC-effective", have + 1);
                return 0;
            }
            if (a_st.unspentPerk + 1e-4f < 1.0f) {
                a_whyNot = std::format("insufficient perk points ({:.2f} < 1)", a_st.unspentPerk);
                return 0;
            }
            // §5.1 prereq: reachable iff a child of the tree root (no perk-
            // carrying parents) OR at least one parent perk owned at rank ≥1.
            // parentPerkIDs is the FULL truth — filtered parents included.
            if (have == 0 && !a_node.parentPerkIDs.empty()) {
                bool anyParent = false;
                for (const auto pid : a_node.parentPerkIDs)
                    if (OwnsAnyRank(a_actor, a_base, a_st, pid)) { anyParent = true; break; }
                if (!anyParent) {
                    a_whyNot = std::format("prerequisite not met ({} parent perk(s), none owned)",
                                           a_node.parentPerkIDs.size());
                    return 0;
                }
            }
            // §5.2 the perk's own conditions, evaluated ON THE FOLLOWER — the
            // same record the player's skill menu gates on, so skill-level and
            // any overhaul-added condition enforce player-identically.
            auto* rankForm = PerkByID(rank.perkFormID);
            if (!rankForm) {
                a_whyNot = std::format("rank form {:08X} did not resolve", rank.perkFormID);
                return 0;
            }
            if (!rankForm->perkConditions.IsTrue(a_actor, a_actor)) {
                a_whyNot = std::format("perkConditions false on follower{}",
                                       a_node.ranks[static_cast<std::size_t>(have)].skillReq.empty()
                                           ? std::string{}
                                           : std::format(" (needs {})", rank.skillReq));
                return 0;
            }
            return have + 1;
        }

        // The mutation: rank K replaces rank K−1 on the BASE (the player's
        // own single-highest-rank shape — two ranks of one perk both present
        // would double-apply their entries), then ApplyPerksFromBase settles
        // the actor. Main thread only.
        bool GrantRank(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st,
                       const Progression::PerkNodeView& a_node, int a_targetRank) {
            const auto& rank = a_node.ranks[static_cast<std::size_t>(a_targetRank - 1)];
            auto* target = PerkByID(rank.perkFormID);
            if (!target) {
                spdlog::error("[prog] grant aborted: rank form {:08X} unresolvable", rank.perkFormID);
                return false;
            }
            if (a_targetRank > 1) {
                if (auto* prev = PerkByID(a_node.ranks[static_cast<std::size_t>(a_targetRank - 2)].perkFormID))
                    a_base->RemovePerk(prev);
            }
            a_base->AddPerk(target, 1);
            a_actor->ApplyPerksFromBase();

            auto* alloc = FindAlloc(a_st, a_node.perkFormID);
            if (!alloc) {
                a_st.perks.push_back({ a_node.perkFormID, 0 });
                alloc = &a_st.perks.back();
            }
            alloc->rank = static_cast<std::uint8_t>(a_targetRank);
            a_st.unspentPerk -= 1.0f;
            if (a_st.unspentPerk < 0.0f) a_st.unspentPerk = 0.0f;

            spdlog::info("[prog] {:08X} GRANTED {} rank {}/{} ({:08X}) — {:.2f} point(s) left",
                         a_actor->GetFormID(), a_node.name, a_targetRank,
                         a_node.ranks.size(), rank.perkFormID, a_st.unspentPerk);
            return true;
        }

        // ── the guarded session reapply (P3) ────────────────────────────────
        // Base-form perk mutations are runtime-only; the co-save alloc is the
        // truth, reasserted here: AddPerk only when the base lacks the target
        // rank form, ApplyPerksFromBase only on change (no doubling — P3's
        // measured verdict). Skills re-reconcile through the same §4.2 path.
        void ReapplyFollower(RE::Actor* a_actor, ProgState& a_st) {
            auto* base = a_actor->GetActorBase();
            if (!base) return;

            int reAdded = 0, present = 0, dropped = 0, nativeDeferred = 0;
            bool changed = false;
            for (const auto& alloc : a_st.perks) {
                auto ref = FindNode(alloc.nodePerkID);
                if (!ref.node || alloc.rank == 0 ||
                    alloc.rank > static_cast<std::uint8_t>(ref.node->ranks.size())) {
                    ++dropped;   // catalog changed under the save — counted, left for load-time validation
                    continue;
                }
                auto* target = PerkByID(ref.node->ranks[alloc.rank - 1].perkFormID);
                if (!target) { ++dropped; continue; }
                if (base->GetPerkIndex(target).has_value()) { ++present; continue; }
                // ANOTHER rank of this node on the base at reapply time is
                // NATIVE ownership: base perk mutations do not survive a load
                // (P3), so at session start the array holds only what the
                // load order authored — and mid-session MFO's own grants keep
                // exactly the target rank present (GrantRank swaps at
                // upgrade). Removing it here would strip a perk MFO never
                // granted (SPID/Requiem gave it between sessions) — the L1
                // violation. Stacking ours on top would double entries. So:
                // touch NOTHING, defer with a named line. The alloc is kept —
                // respec later refunds it without removing the native form
                // (RemovePerk is GetPerkIndex-guarded on OUR rank form only).
                bool nativeRank = false;
                for (const auto& r : ref.node->ranks) {
                    if (r.perkFormID == target->GetFormID()) continue;
                    if (auto* sib = PerkByID(r.perkFormID); sib && base->GetPerkIndex(sib).has_value()) {
                        nativeRank = true;
                        break;
                    }
                }
                if (nativeRank) {
                    ++nativeDeferred;
                    spdlog::info("[prog] {:08X} reapply: {} rank {} NOT re-added — the load order "
                                 "now grants another rank of this node natively; MFO never removes "
                                 "or stacks on a rank it did not grant",
                                 a_actor->GetFormID(), ref.node->name, alloc.rank);
                    continue;
                }
                base->AddPerk(target, 1);
                ++reAdded;
                changed = true;
            }
            if (changed) a_actor->ApplyPerksFromBase();

            RecomputeSkills(a_actor, a_st, /*log*/ false);

            spdlog::info("[prog] {:08X} {} reapply: {} perk(s) re-added, {} already on base, "
                         "{} unresolvable, {} deferred to native ownership — ApplyPerksFromBase {}",
                         a_actor->GetFormID(), NameOf(a_actor), reAdded, present, dropped,
                         nativeDeferred, changed ? "called" : "SKIPPED (no change)");
            a_st.applied = true;
        }

        // ── board views (component 3 — see ProgAllocator.h) ─────────────────
        // Written on the MAIN thread only; the mutex guards the shared_ptr
        // swap so any thread may take a refcounted copy. The snap itself is
        // immutable once published — the Snapshot discipline, by pointer.
        std::mutex g_viewMx;
        std::shared_ptr<const BoardProgSnap> g_boardSnap;
        std::atomic<RE::FormID> g_boardFocus{ 0 };   // written by the render thread
        RE::FormID g_lastPublishedFocus = 0;         // main-thread-only
        bool g_boardWasOpen = false;                 // main-thread-only
        int  g_viewFrames   = 0;
        constexpr int kViewFrames = 30;   // ~500ms at 60fps while the board is open

        // Read-only mirror of Enroll()'s refusals, for the board to explain
        // an ineligible follower BEFORE the player tries (same checks, same
        // order — keep in sync with Enroll).
        const char* EnrollBlocker(RE::Actor* a_actor, RE::TESNPC* a_base) {
            if (!a_base) return "no actor base";
            // ASCII only: these strings render in the board UI and the baked
            // fonts carry the default (Basic Latin) glyph ranges.
            if (!Followers::IsPersistableID(a_actor->GetFormID()))
                return "temporary (runtime) actor -- cannot persist across saves";
            if (!a_base->IsUnique())
                return "shared template -- not eligible (v1 is unique-base only)";
            if (!Followers::IsEligibleFollower(a_actor))
                return "not an eligible follower";
            return nullptr;
        }

        // One BoardNodeView per catalog node, catalog order (skill-major) —
        // the render thread indexes by prefix sums off the same frozen
        // catalog. Cost is bounded by the publish cadence (kViewFrames), not
        // per frame: GateNextRank's condition evaluation only runs for nodes
        // that survive the cheaper gates (its points check sits before the
        // prereq walk and perkConditions.IsTrue).
        void BuildNodeViews(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState* a_st,
                            std::vector<BoardNodeView>& a_out) {
            static const ProgState kNoState{};
            const auto& cat = Progression::Get();
            std::size_t total = 0;
            for (const auto& t : cat.skills) total += t.nodes.size();
            a_out.clear();
            a_out.reserve(total);
            const bool gateable = a_st && a_st->enrolled && a_st->cls != Class::kNone;
            for (const auto& tree : cat.skills) {
                for (const auto& node : tree.nodes) {
                    BoardNodeView v;
                    if (a_st)
                        if (auto* al = FindAlloc(*a_st, node.perkFormID))
                            v.ownedRank = al->rank;
                    if (v.ownedRank == 0 &&
                        OwnsAnyRank(a_actor, a_base, a_st ? *a_st : kNoState, node.perkFormID))
                        v.native = true;   // load-order-granted — MFO never builds on it (§4.1)
                    if (gateable && !v.native &&
                        v.ownedRank < static_cast<std::uint8_t>(node.ranks.size())) {
                        std::string why;
                        if (GateNextRank(a_actor, a_base, *a_st, node, why) > 0)
                            v.available = true;
                        else
                            v.whyNot = std::move(why);
                    }
                    a_out.push_back(std::move(v));
                }
            }
        }

        // ── activity + economy (the poll body) ──────────────────────────────

        bool IsActiveFollower(RE::FormID a_id) {
            return std::find(Followers::g_activeIds.begin(), Followers::g_activeIds.end(), a_id)
                   != Followers::g_activeIds.end();
        }

        void PollWork() {
            auto* player = RE::PlayerCharacter::GetSingleton();
            if (!player) return;
            const auto pl = player->GetLevel();
            if (g_lastPlayerLevel == 0) g_lastPlayerLevel = pl;   // first observation, no retro-grant
            const int d = static_cast<int>(pl) - static_cast<int>(g_lastPlayerLevel);
            if (d != 0) {
                spdlog::info("[prog] player level {} -> {} ({} enrolled record(s) to advance)",
                             g_lastPlayerLevel, pl, g_prog.size());
                g_lastPlayerLevel = pl;
            }

            const double ratio = ScarcityRatio();
            for (auto& [id, st] : g_prog) {
                if (!st.enrolled) continue;

                RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(id);

                if (st.cls != Class::kNone) {
                    const bool active = IsActiveFollower(id);
                    const int  lag    = std::max(0, static_cast<int>(pl) - static_cast<int>(st.progressionLevel));
                    int gain = 0;
                    if (!Config::g_sharedGrowth.load()) {
                        // Shared Growth OFF (§15): everyone matches the
                        // player's level outright — close any lag now.
                        gain = lag;
                        st.sharedGrowthRemainder = 0;
                    } else if (active) {
                        // Active + ON: earn at the player's own RATE (the
                        // delta), never an instant catch-up — a bench-lag is
                        // the PRICE of Shared Growth's half rate, and
                        // re-recruiting must not refund it. The banked
                        // remainder keeps for the next bench spell.
                        gain = std::min(std::max(0, d), lag);
                    } else if (d > 0) {
                        // Benched + Shared Growth ON: bank player levels and
                        // convert at the divisor (§15: half rate by default).
                        st.sharedGrowthRemainder = static_cast<std::uint16_t>(st.sharedGrowthRemainder + d);
                        const int div = std::max(1, g_econ.sharedGrowthDivisor);
                        gain = st.sharedGrowthRemainder / div;
                        st.sharedGrowthRemainder = static_cast<std::uint16_t>(st.sharedGrowthRemainder % div);
                        gain = std::min(gain, std::max(0, static_cast<int>(pl) - static_cast<int>(st.progressionLevel)));
                    }
                    if (gain > 0) {
                        st.progressionLevel = static_cast<std::uint16_t>(st.progressionLevel + gain);
                        const float pts = static_cast<float>(gain) * g_econ.perkPointsPerLevel *
                                          static_cast<float>(ratio);
                        st.unspentPerk += pts;
                        spdlog::info("[prog] {:08X} level {} (+{}) — +{:.2f} perk point(s) "
                                     "(rate {:.0f}/lvl x scarcity {:.2f}) -> {:.2f} unspent",
                                     id, st.progressionLevel, gain, pts,
                                     g_econ.perkPointsPerLevel, ratio, st.unspentPerk);
                        if (actor) RecomputeSkills(actor, st, /*log*/ true);
                    }
                }

                if (actor) {
                    if (!st.applied) {
                        ReapplyFollower(actor, st);   // lazy: runs once the actor resolves
                    } else if (st.cls != Class::kNone && IsActiveFollower(id)) {
                        // Drift watch on the active party: an engine recompute
                        // (level-up autocalc, another mod's write) is adopted
                        // and re-topped by the reconcile. Writes only on
                        // divergence, so the steady state is pure reads.
                        RecomputeSkills(actor, st, /*log*/ true);
                    }
                }
            }
        }

        void PollTick(int a_gen) {
            if (a_gen != g_pollGen) return;   // superseded by revert/reload
            if (--g_pollFrames <= 0) {
                g_pollFrames = kPollFrames;
                PollWork();
            }
            // Board-view refresh (component 3): while the board is open,
            // republish on the open edge, on a follower-focus change (the tab
            // switched who it is looking at — don't make the tree lag half a
            // second behind an L1/R1), and on the ~500ms cadence. Closed =
            // free (one atomic read + a bool).
            if (Board::IsOpen()) {
                const bool focusChanged = g_boardFocus.load() != g_lastPublishedFocus;
                if (!g_boardWasOpen || focusChanged || --g_viewFrames <= 0) {
                    g_viewFrames = kViewFrames;
                    PublishBoardViews();
                }
                g_boardWasOpen = true;
            } else {
                g_boardWasOpen = false;
            }
            MainThread::Post([a_gen]() { PollTick(a_gen); });
        }

        // ── ESL reads (Init helpers) ────────────────────────────────────────

        float ReadGlob(RE::TESDataHandler* a_dh, RE::FormID a_id, const char* a_name, float a_fallback) {
            if (auto* g = a_dh->LookupForm<RE::TESGlobal>(a_id, Progression::kAddonPlugin))
                return g->value;
            spdlog::warn("[prog] economy GLOB {} (0x{:03X}) missing from {} — using DLL default {:g}",
                         a_name, a_id, Progression::kAddonPlugin, a_fallback);
            return a_fallback;
        }

        // Translate an ESL ClassSkills FormList (AVIF forms) into ActorValues.
        // The AVIF→AV mapping is derived from the live ActorValueList — no
        // hardcoded AVIF ids in the DLL, so a load order that replaces the
        // AVIF records still maps correctly.
        std::vector<RE::ActorValue> ReadSkillList(RE::TESDataHandler* a_dh, RE::FormID a_id,
                                                  const char* a_name) {
            std::vector<RE::ActorValue> out;
            auto* list = a_dh->LookupForm<RE::BGSListForm>(a_id, Progression::kAddonPlugin);
            if (!list) {
                spdlog::warn("[prog] class list {} (0x{:03X}) missing from {} — DLL fallback weights",
                             a_name, a_id, Progression::kAddonPlugin);
                return out;
            }
            auto* avl = RE::ActorValueList::GetSingleton();
            for (auto* form : list->forms) {
                if (!form) continue;
                RE::ActorValue matched = RE::ActorValue::kNone;
                if (avl) {
                    for (const auto& s : kSkillNames) {
                        auto* avi = avl->GetActorValue(s.av);
                        if (avi && avi->GetFormID() == form->GetFormID()) { matched = s.av; break; }
                    }
                }
                if (matched == RE::ActorValue::kNone) {
                    spdlog::warn("[prog] class list {}: entry {:08X} is not a known skill AVIF — skipped",
                                 a_name, form->GetFormID());
                    continue;
                }
                out.push_back(matched);
            }
            return out;
        }

        std::vector<RE::FormID> ReadPerkList(RE::TESDataHandler* a_dh, RE::FormID a_id,
                                             const char* a_name) {
            std::vector<RE::FormID> out;
            auto* list = a_dh->LookupForm<RE::BGSListForm>(a_id, Progression::kAddonPlugin);
            if (!list) return out;   // absent == empty == fallback; shipped lists ARE empty
            for (auto* form : list->forms)
                if (form) out.push_back(form->GetFormID());
            if (!out.empty())
                spdlog::info("[prog] class perk-priority {}: {} authored entr(ies)", a_name, out.size());
            return out;
        }

        void FallbackClassSkills() {
            using AV = RE::ActorValue;
            // Mirrors the shipped ESL lists exactly, so "record missing" and
            // "record default" behave identically (§6 weights after pruning:
            // Melee 40/30/20/10, Ranged 40/30/20/10, Mage 33/27/20/13/7).
            if (g_class[1].skills.empty())
                g_class[1].skills = { AV::kOneHanded, AV::kTwoHanded, AV::kHeavyArmor,
                                      AV::kLightArmor, AV::kBlock, AV::kArchery };
            if (g_class[2].skills.empty())
                g_class[2].skills = { AV::kArchery, AV::kLightArmor, AV::kHeavyArmor,
                                      AV::kSneak, AV::kOneHanded };
            if (g_class[3].skills.empty())
                g_class[3].skills = { AV::kDestruction, AV::kAlteration, AV::kRestoration,
                                      AV::kConjuration, AV::kIllusion };
        }

        // ── the dev harness (bProgHarness) ──────────────────────────────────

        // First unique-base persistable teammate, else first — the ProgProbe
        // pick, without its per-candidate spam (the harness runs repeatedly).
        RE::Actor* PickFollower() {
            RE::Actor* first = nullptr;
            RE::Actor* firstUnique = nullptr;
            for (const auto& h : Followers::g_active) {
                auto* a = h.get().get();
                if (!a) continue;
                if (!first) first = a;
                auto* base = a->GetActorBase();
                if (!firstUnique && base && base->IsUnique() &&
                    Followers::IsPersistableID(a->GetFormID()))
                    firstUnique = a;
            }
            if (!first) spdlog::info("[prog] harness: no active teammates — recruit a follower first");
            return firstUnique ? firstUnique : first;
        }

        void HarnessStatus(RE::Actor* a_actor) {
            const auto id = a_actor->GetFormID();
            auto it = g_prog.find(id);
            if (it == g_prog.end() || !it->second.enrolled) {
                spdlog::info("[prog] status {}: NOT ENROLLED (cmd 1 enrolls; player level {})",
                             NameOf(a_actor),
                             RE::PlayerCharacter::GetSingleton()
                                 ? RE::PlayerCharacter::GetSingleton()->GetLevel() : 0);
                return;
            }
            auto& st = it->second;
            spdlog::info("[prog] status {} ({:08X}): class {} | progression level {} | "
                         "{:.2f} perk point(s) unspent | {} perk(s) allocated | {} | "
                         "sharedRemainder {} | veteran {} | provenance PFF={}",
                         NameOf(a_actor), id, ClassName(st.cls), st.progressionLevel,
                         st.unspentPerk, st.perks.size(),
                         IsActiveFollower(id) ? "ACTIVE" : "benched",
                         st.sharedGrowthRemainder,
                         st.veteranConsumed ? "consumed" : "pending",
                         st.wasInPotentialFollowerFaction ? "yes" : "no");
            for (const auto& p : st.perks) {
                auto ref = FindNode(p.nodePerkID);
                spdlog::info("[prog]   perk {} ({:08X}) rank {}",
                             ref.node ? ref.node->name.c_str() : "<off-catalog>",
                             p.nodePerkID, p.rank);
            }
        }

        void HarnessSkillDump(RE::Actor* a_actor) {
            auto it = g_prog.find(a_actor->GetFormID());
            if (it == g_prog.end() || !it->second.enrolled || it->second.cls == Class::kNone) {
                spdlog::info("[prog] skills {}: no class set — nothing auto-scaled", NameOf(a_actor));
                return;
            }
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) return;
            spdlog::info("[prog] skills {} (class {}, level {}):", NameOf(a_actor),
                         ClassName(it->second.cls), it->second.progressionLevel);
            for (const auto& e : it->second.skills) {
                spdlog::info("[prog]   {:<12} natural {:.1f} + alloc {:.0f} = base {:.1f} "
                             "(effective {:.1f})",
                             AvName(e.av), e.lastWrittenBase - e.points, e.points,
                             avo->GetBaseActorValue(e.av), avo->GetActorValue(e.av));
            }
            if (it->second.skills.empty())
                spdlog::info("[prog]   (no allocations — level too low for any points yet)");
        }

        void HarnessEconomyDump() {
            const auto& c = Progression::Get();
            spdlog::info("[prog] economy: perk/lvl {:g} | skill/lvl {:g} | sharedDiv {} | "
                         "respec rapport {:g} | veteranMult {:g} | cap {:g} | "
                         "scarcity {}/{} = {:.3f} | lastPlayerLevel {}",
                         g_econ.perkPointsPerLevel, g_econ.skillPointsPerLevel,
                         g_econ.sharedGrowthDivisor, g_econ.respecRapportCost,
                         g_econ.veteranCatchupMult, g_econ.skillCap,
                         c.effectiveRanks, c.totalRanks, ScarcityRatio(), g_lastPlayerLevel);
        }

    }   // anonymous namespace

    const char* ClassName(Class a_cls) {
        switch (a_cls) {
        case Class::kMelee:  return "Melee";
        case Class::kRanged: return "Ranged";
        case Class::kMage:   return "Mage";
        default:             return "none";
        }
    }

    bool Active() { return g_ready; }

    void Init() {
        if (!Progression::Detected()) {
            // One named line, never an error — the addon is optional (§1).
            spdlog::info("[prog] allocator inert ({} absent)", Progression::kAddonPlugin);
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[prog] TESDataHandler unavailable — allocator cannot initialize");
            return;
        }

        // Economy GLOBs — the RECORD DEFAULTS (kDataLoaded is pre-save, §10).
        g_econ.perkPointsPerLevel  = ReadGlob(dh, kGlobPerkPointsPerLevel,  "MFOP_PerkPointsPerLevel",  1.0f);
        g_econ.skillPointsPerLevel = ReadGlob(dh, kGlobSkillPointsPerLevel, "MFOP_SkillPointsPerLevel", 3.0f);
        g_econ.sharedGrowthDivisor = std::max(1, static_cast<int>(
            ReadGlob(dh, kGlobSharedGrowthDivisor, "MFOP_SharedGrowthDivisor", 2.0f)));
        g_econ.respecRapportCost   = ReadGlob(dh, kGlobRespecRapportCost,   "MFOP_RespecRapportCost",   500.0f);
        g_econ.veteranCatchupMult  = ReadGlob(dh, kGlobVeteranCatchupMult,  "MFOP_VeteranCatchupMult",  1.0f);
        g_econ.skillCap            = ReadGlob(dh, kGlobSkillCap,            "MFOP_SkillCap",            100.0f);

        // The harness selector is the ONE live-read glob (console-set).
        g_devCmd = dh->LookupForm<RE::TESGlobal>(kGlobDevCmd, Progression::kAddonPlugin);

        // Class lists (ESL-authored; DLL fallback mirrors the shipped lists).
        g_class[1].skills = ReadSkillList(dh, kListClassSkillsMelee,  "MFOP_ClassSkills_Melee");
        g_class[2].skills = ReadSkillList(dh, kListClassSkillsRanged, "MFOP_ClassSkills_Ranged");
        g_class[3].skills = ReadSkillList(dh, kListClassSkillsMage,   "MFOP_ClassSkills_Mage");
        g_class[1].perkPriority = ReadPerkList(dh, kListClassPerksMelee,  "MFOP_ClassPerks_Melee");
        g_class[2].perkPriority = ReadPerkList(dh, kListClassPerksRanged, "MFOP_ClassPerks_Ranged");
        g_class[3].perkPriority = ReadPerkList(dh, kListClassPerksMage,   "MFOP_ClassPerks_Mage");
        FallbackClassSkills();

        g_ready = true;
        spdlog::info("[prog] allocator ready: perk/lvl {:g}, skill/lvl {:g}, sharedDiv {}, "
                     "respec {:g} rapport, cap {:g}, class lists {}/{}/{} skill(s), "
                     "perk priorities {}/{}/{}, devCmd {}",
                     g_econ.perkPointsPerLevel, g_econ.skillPointsPerLevel,
                     g_econ.sharedGrowthDivisor, g_econ.respecRapportCost, g_econ.skillCap,
                     g_class[1].skills.size(), g_class[2].skills.size(), g_class[3].skills.size(),
                     g_class[1].perkPriority.size(), g_class[2].perkPriority.size(),
                     g_class[3].perkPriority.size(),
                     g_devCmd ? "resolved" : "MISSING (harness cmds default to 0)");
    }

    void OnPostLoad() {
        if (!g_ready) {
            if (!g_prog.empty())
                spdlog::warn("[prog] {} progression record(s) loaded but the addon is absent — "
                             "inert this session (data preserved, nothing applied)", g_prog.size());
            return;
        }
        // Fresh session: everything reapplies lazily (the poll retries until
        // each enrolled actor resolves — P3's guarded shape, never eager).
        for (auto& [id, st] : g_prog) st.applied = false;

        ++g_pollGen;
        g_pollFrames = kPollFrames;
        const int gen = g_pollGen;
        MainThread::Post([gen]() { PollTick(gen); });
        // Seed the board views once so the Progression TAB exists on the very
        // first board open (the power's own PublishSnapshot copies the prog
        // pointer before the open-gated poll refresh would have run).
        PublishBoardViews();
        spdlog::info("[prog] post-load: {} progression record(s); level poll started (gen {})",
                     g_prog.size(), gen);
    }

    // ── board views (component 3) ───────────────────────────────────────────

    void SetBoardFocus(RE::FormID a_id) { g_boardFocus.store(a_id); }

    std::shared_ptr<const BoardProgSnap> CopyBoardViews() {
        std::scoped_lock lk(g_viewMx);
        return g_boardSnap;
    }

    void PublishBoardViews() {
        auto snap = std::make_shared<BoardProgSnap>();
        snap->active = g_ready && Progression::Get().built;
        if (snap->active) {
            const auto& cat      = Progression::Get();
            snap->totalRanks     = cat.totalRanks;
            snap->effectiveRanks = cat.effectiveRanks;
            snap->scarcity       = static_cast<float>(ScarcityRatio());
            snap->respecRapportCost = g_econ.respecRapportCost;
            snap->perkPtsPerLevel   = g_econ.perkPointsPerLevel;
            snap->skillPtsPerLevel  = g_econ.skillPointsPerLevel;
            snap->skillCap          = g_econ.skillCap;

            const RE::FormID focus = g_boardFocus.load();
            for (const auto& h : Followers::g_active) {
                auto* a = h.get().get();
                if (!a) continue;
                BoardFollowerView v;
                v.id   = a->GetFormID();
                v.name = a->GetName() ? a->GetName() : "?";
                auto* base = a->GetActorBase();
                const char* blk = EnrollBlocker(a, base);
                v.eligible = (blk == nullptr);
                if (blk) v.blocker = blk;
                ProgState* st = nullptr;
                if (auto it = g_prog.find(v.id); it != g_prog.end() && it->second.enrolled)
                    st = &it->second;
                if (st) {
                    v.enrolled     = true;
                    v.cls          = static_cast<std::uint8_t>(st->cls);
                    v.level        = st->progressionLevel;
                    v.unspentPerk  = st->unspentPerk;
                    v.manualSkills = st->manualSkills;             // §16
                    v.manualAvail  = ManualAvail(*st);
                }
                if (auto* avo = a->AsActorValueOwner()) {
                    v.skills.reserve(std::size(kSkillNames));
                    for (const auto& s : kSkillNames) {
                        BoardSkillLine line;
                        line.name = s.name;
                        line.av   = s.av;
                        line.base = avo->GetBaseActorValue(s.av);
                        if (st)
                            for (const auto& e : st->skills)
                                if (e.av == s.av) {
                                    line.alloc  = e.points;
                                    line.manual = e.manualPoints;   // §16
                                    break;
                                }
                        v.skills.push_back(std::move(line));
                    }
                }
                // The full per-node gate walk only for the follower the tab
                // is actually LOOKING at — one tree per publish, not four.
                if (base && v.id == focus) {
                    snap->treeFor = focus;
                    BuildNodeViews(a, base, st, snap->nodes);
                }
                snap->rows.push_back(std::move(v));
            }
            g_lastPublishedFocus = focus;
        }
        std::scoped_lock lk(g_viewMx);
        g_boardSnap = std::move(snap);
    }

    // ── verbs ───────────────────────────────────────────────────────────────

    bool Enroll(RE::Actor* a_actor) {
        if (!g_ready) { spdlog::info("[prog] enroll refused: addon absent"); return false; }
        if (!a_actor) { spdlog::warn("[prog] enroll refused: null actor"); return false; }
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
            spdlog::info("[prog] {} already enrolled (class {})", NameOf(a_actor), ClassName(st.cls));
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
        }

        spdlog::info("[prog] ENROLLED {} ({:08X}) — unique base {:08X}, PotentialFollowerFaction={}, "
                     "baseline {} skill(s) captured. Progression INACTIVE until a class is set (§15).",
                     NameOf(a_actor), id, base->GetFormID(),
                     st.wasInPotentialFollowerFaction ? "yes" : "NO",
                     st.baseline.size());
        return true;
    }

    bool SetClass(RE::Actor* a_actor, Class a_cls) {
        if (!g_ready) { spdlog::info("[prog] set-class refused: addon absent"); return false; }
        if (!a_actor) return false;
        const auto id = a_actor->GetFormID();
        auto it = g_prog.find(id);
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] set-class refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        if (a_cls == Class::kNone) {
            spdlog::info("[prog] set-class refused {}: a concrete class is required (§15)", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        auto* player = RE::PlayerCharacter::GetSingleton();
        const auto pl = player ? player->GetLevel() : std::uint16_t{ 1 };

        // First class pick = the on-ramp (§15): level-match to the player and
        // grant the veteran catch-up ONCE — level-matched, scarcity-scaled,
        // NOT perk-penalized for arriving late. No bonus skill points: the
        // auto-scale below already reflects the level.
        if (!st.veteranConsumed) {
            st.progressionLevel = pl;
            const double ratio = ScarcityRatio();
            const float grant = static_cast<float>(std::max(0, pl - 1)) *
                                g_econ.perkPointsPerLevel * static_cast<float>(ratio) *
                                g_econ.veteranCatchupMult;
            st.unspentPerk += grant;
            st.veteranConsumed = true;
            spdlog::info("[prog] {} veteran catch-up: level {} x rate {:g} x scarcity {:.2f} x mult {:g} "
                         "= {:.2f} perk point(s)",
                         NameOf(a_actor), pl, g_econ.perkPointsPerLevel, ratio,
                         g_econ.veteranCatchupMult, grant);
        }

        const auto before = st.cls;
        st.cls = a_cls;

        // #65 alignment: the progression class IS the combat class — mirror
        // it into the FollowerState override (same ordinals by construction)
        // so the stance machinery follows the chosen class immediately.
        if (auto* rec = Followers::TryEnsureRecord(id)) {
            rec->combatClassOverride = static_cast<std::uint8_t>(a_cls);
        }

        RecomputeSkills(a_actor, st, /*log*/ true);
        spdlog::info("[prog] {} class {} -> {} — skills auto-scaled to level {} "
                     "({} skill(s) allocated), perk allocation unlocked",
                     NameOf(a_actor), ClassName(before), ClassName(a_cls),
                     st.progressionLevel, st.skills.size());
        return true;
    }

    bool AllocatePerk(RE::Actor* a_actor, RE::FormID a_nodePerkID) {
        if (!g_ready) { spdlog::info("[prog] allocate refused: addon absent"); return false; }
        if (!a_actor) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] allocate refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (st.cls == Class::kNone) {
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
        if (!g_ready || !a_actor) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled || it->second.cls == Class::kNone) {
            spdlog::info("[prog] auto-pick refused {}: not enrolled or no class", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        std::string whyNot;

        // 1) the ESL-authored perk priority for this class, in list order.
        // An entry may be any rank form of a node — match either way.
        for (const auto fid : g_class[static_cast<std::size_t>(st.cls)].perkPriority) {
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
        const auto weights = WeightsFor(a_actor, st.cls);
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
        spdlog::info("[prog] auto-pick {}: no eligible perk right now ({:.2f} point(s) unspent) — "
                     "gates (skill reqs / prereqs) block everything reachable",
                     NameOf(a_actor), st.unspentPerk);
        return false;
    }

    bool Respec(RE::Actor* a_actor) {
        if (!g_ready) { spdlog::info("[prog] respec refused: addon absent"); return false; }
        if (!a_actor) return false;
        auto* base = a_actor->GetActorBase();
        if (!base) return false;
        const auto id = a_actor->GetFormID();
        auto it = g_prog.find(id);
        if (it == g_prog.end() || !it->second.enrolled) {
            spdlog::info("[prog] respec refused {}: not enrolled", NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (st.perks.empty()) {
            // No perks, no cost — the rapport hit is FOR the reset, and a
            // no-op reset must not bill anyone.
            spdlog::info("[prog] respec {}: nothing allocated — no perks removed, no rapport spent",
                         NameOf(a_actor));
            return false;
        }

        int removed = 0;
        float refund = 0.0f;
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
            refund += static_cast<float>(p.rank);   // 1 point per rank taken
        }
        a_actor->ApplyPerksFromBase();   // §4.1: re-settle the actor once
        st.perks.clear();
        st.unspentPerk += refund;

        // §15: free of gold, −500 rapport (record default; the follower
        // resents the reset). Reuses the Rapport rank machinery wholesale.
        Rapport::Spend(id, g_econ.respecRapportCost, "progression respec");

        spdlog::info("[prog] RESPEC {} ({:08X}): {} perk(s) removed, {:.0f} point(s) refunded "
                     "-> {:.2f} unspent, rapport -{:.0f}",
                     NameOf(a_actor), id, removed, refund, st.unspentPerk, g_econ.respecRapportCost);
        return true;
    }

    // ── §16 manual skill points ─────────────────────────────────────────────

    bool SetManualSkills(RE::Actor* a_actor, bool a_on) {
        if (!g_ready) { spdlog::info("[prog] manual-skills refused: addon absent"); return false; }
        if (!a_actor) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled || it->second.cls == Class::kNone) {
            spdlog::info("[prog] manual-skills refused {}: not enrolled or no class (§15 gate)",
                         NameOf(a_actor));
            return false;
        }
        auto& st = it->second;
        if (st.manualSkills == a_on) return true;   // idempotent no-op
        st.manualSkills = a_on;
        // First enable LATCHES the accrual baseline; afterwards the toggle
        // only gates spending — the pool stays a pure function of level, so
        // off/on cycling can neither farm points nor forfeit them.
        if (a_on && st.manualBaselineLevel == 0)
            st.manualBaselineLevel = std::max<std::uint16_t>(1, st.progressionLevel);
        spdlog::info("[prog] {} manual skill points {} (baseline level {}, {} available, "
                     "rate {:g}/level)",
                     NameOf(a_actor), a_on ? "ON" : "OFF", st.manualBaselineLevel,
                     ManualAvail(st), g_econ.skillPointsPerLevel);
        return true;
    }

    bool ApplyManualSkillPoint(RE::Actor* a_actor, RE::ActorValue a_av) {
        if (!g_ready) { spdlog::info("[prog] apply-skill refused: addon absent"); return false; }
        if (!a_actor) return false;
        auto it = g_prog.find(a_actor->GetFormID());
        if (it == g_prog.end() || !it->second.enrolled || it->second.cls == Class::kNone) {
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
                         "(level {} - baseline {} at {:g}/level, {} applied)",
                         NameOf(a_actor), st.progressionLevel, st.manualBaselineLevel,
                         g_econ.skillPointsPerLevel, st.manualPointsApplied);
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

    // ── co-save ('PRGN' v2, §8 + §16) ───────────────────────────────────────
    //
    //   u16 lastPlayerLevel
    //   u32 followerCount, then per follower:
    //     u32 formID | u8 flags | u8 class | u16 progressionLevel
    //     u16 sharedGrowthRemainder | f32 unspentPerk
    //     v2: u16 manualBaselineLevel | u16 manualPointsApplied
    //     u16 perkCount   { u32 nodePerkID, u8 rank }*
    //     u16 skillCount  { u32 av, f32 points, f32 lastWrittenBase,
    //                       v2: f32 manualPoints }*
    //     u16 baseCount   { u32 av, f32 value }*
    //
    // FormIDs go through ResolveFormID on load (INVARIANTS #8): a gone
    // follower drops with its whole block consumed; a gone perk drops and
    // REFUNDS its rank-count in points. AV ids are engine enum ordinals, not
    // FormIDs — no resolution, bounds-checked only. Runtime (0xFF) ids are
    // never written (#9). New fields go behind `if (version >= N)` (#12).

    void CoSaveSave(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecProgression, kProgVersion)) {
            spdlog::error("[cosave] OpenRecord('{}') failed -- progression NOT saved", "PRGN");
            return;
        }
        a_intfc->WriteRecordData(g_lastPlayerLevel);

        std::uint32_t persistable = 0;
        for (const auto& [id, st] : g_prog)
            if (st.enrolled && Followers::IsPersistableID(id)) ++persistable;
        a_intfc->WriteRecordData(persistable);

        std::uint32_t written = 0, skippedRuntime = 0;
        for (const auto& [id, st] : g_prog) {
            if (!st.enrolled) continue;
            if (!Followers::IsPersistableID(id)) { ++skippedRuntime; continue; }
            a_intfc->WriteRecordData(id);
            const std::uint8_t flags =
                (st.enrolled ? 1u : 0u) | (st.autoSpend ? 2u : 0u) |
                (st.veteranConsumed ? 4u : 0u) | (st.wasInPotentialFollowerFaction ? 8u : 0u) |
                (st.manualSkills ? 16u : 0u);   // v2 (§16) — spare bit in the same byte
            a_intfc->WriteRecordData(flags);
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(st.cls));
            a_intfc->WriteRecordData(st.progressionLevel);
            a_intfc->WriteRecordData(st.sharedGrowthRemainder);
            a_intfc->WriteRecordData(st.unspentPerk);
            // v2 (§16): the manual-pool BASELINES — never a pool value; the
            // pool is recomputed from these, so a save replays to the same
            // number by construction.
            a_intfc->WriteRecordData(st.manualBaselineLevel);
            a_intfc->WriteRecordData(st.manualPointsApplied);

            const auto perkCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.perks.size(), kMaxPerkAllocs));
            a_intfc->WriteRecordData(perkCount);
            for (std::uint16_t i = 0; i < perkCount; ++i) {
                a_intfc->WriteRecordData(st.perks[i].nodePerkID);
                a_intfc->WriteRecordData(st.perks[i].rank);
            }
            const auto skillCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.skills.size(), kMaxSkillAllocs));
            a_intfc->WriteRecordData(skillCount);
            for (std::uint16_t i = 0; i < skillCount; ++i) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(st.skills[i].av));
                a_intfc->WriteRecordData(st.skills[i].points);
                a_intfc->WriteRecordData(st.skills[i].lastWrittenBase);
                a_intfc->WriteRecordData(st.skills[i].manualPoints);   // v2 (§16)
            }
            const auto baseCount = static_cast<std::uint16_t>(
                std::min<std::size_t>(st.baseline.size(), kMaxSkillAllocs));
            a_intfc->WriteRecordData(baseCount);
            for (std::uint16_t i = 0; i < baseCount; ++i) {
                a_intfc->WriteRecordData(static_cast<std::uint32_t>(st.baseline[i].av));
                a_intfc->WriteRecordData(st.baseline[i].value);
            }
            ++written;
        }
        spdlog::info("[cosave] saved {} progression record(s), schema v{}{}", written, kProgVersion,
                     skippedRuntime ? std::format(" -- SKIPPED {} runtime (0xFF) record(s)", skippedRuntime)
                                    : std::string{});
    }

    void CoSaveLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version) {
        // v1 saves lack the §16 manual fields — every v2 read below gates on
        // a_version >= 2 and defaults to "manual off, nothing applied".
        g_prog.clear();    // defence in depth — ClearAll ran at revert already

        std::uint16_t lastPl = 0;
        if (!a_intfc->ReadRecordData(lastPl)) {
            spdlog::error("[cosave] short read on progression header -- ABORTING progression load");
            return;
        }
        g_lastPlayerLevel = lastPl;

        std::uint32_t count = 0;
        if (!a_intfc->ReadRecordData(count)) return;
        if (count > kMaxProgFollowers) {
            spdlog::error("[cosave] implausible progression follower count {} -- ABORTING", count);
            return;
        }

        const bool haveCatalog = Progression::Get().built;
        std::uint32_t loaded = 0, droppedActor = 0, droppedPerk = 0, droppedAv = 0;

        for (std::uint32_t i = 0; i < count; ++i) {
            RE::FormID rawID = 0;
            if (!a_intfc->ReadRecordData(rawID)) return;

            RE::FormID resolvedID = 0;
            const bool resolved = a_intfc->ResolveFormID(rawID, resolvedID);

            ProgState st{};
            std::uint8_t flags = 0, clsRaw = 0;
            if (!a_intfc->ReadRecordData(flags)) return;
            if (!a_intfc->ReadRecordData(clsRaw)) return;
            if (!a_intfc->ReadRecordData(st.progressionLevel)) return;
            if (!a_intfc->ReadRecordData(st.sharedGrowthRemainder)) return;
            if (!a_intfc->ReadRecordData(st.unspentPerk)) return;
            st.enrolled                       = (flags & 1u) != 0;
            st.autoSpend                      = (flags & 2u) != 0;
            st.veteranConsumed                = (flags & 4u) != 0;
            st.wasInPotentialFollowerFaction  = (flags & 8u) != 0;
            st.manualSkills                   = (flags & 16u) != 0;   // v2 (§16)
            st.cls = static_cast<Class>(std::min<std::uint8_t>(clsRaw, 3));
            if (st.unspentPerk < 0.0f || !std::isfinite(st.unspentPerk)) st.unspentPerk = 0.0f;
            if (a_version >= 2) {
                if (!a_intfc->ReadRecordData(st.manualBaselineLevel)) return;
                if (!a_intfc->ReadRecordData(st.manualPointsApplied)) return;
            }
            // Coherence guard: manual ON without a latched baseline cannot
            // happen through the verbs — a hand-edited/corrupt record reads
            // as OFF rather than as an infinite level-1 pool.
            if (st.manualSkills && st.manualBaselineLevel == 0) st.manualSkills = false;

            std::uint16_t perkCount = 0;
            if (!a_intfc->ReadRecordData(perkCount)) return;
            if (perkCount > kMaxPerkAllocs) {
                spdlog::error("[cosave] implausible perk alloc count {} -- ABORTING progression load", perkCount);
                return;
            }
            for (std::uint16_t p = 0; p < perkCount; ++p) {
                RE::FormID rawPerk = 0;
                std::uint8_t rank = 0;
                // NOTE: read unconditionally even for a dropped follower —
                // bailing early would desync every byte after this block.
                if (!a_intfc->ReadRecordData(rawPerk)) return;
                if (!a_intfc->ReadRecordData(rank)) return;
                if (!resolved) continue;
                RE::FormID resolvedPerk = 0;
                if (!a_intfc->ResolveFormID(rawPerk, resolvedPerk)) {
                    // INVARIANTS #8 + §8: DROP the alloc, REFUND its points —
                    // the load order lost this perk; the investment returns.
                    st.unspentPerk += static_cast<float>(rank);
                    ++droppedPerk;
                    continue;
                }
                if (haveCatalog) {
                    // The catalog is this session's truth: a node the current
                    // trees no longer carry (overhaul swap mid-playthrough)
                    // also drops + refunds, with the same counter.
                    auto ref = FindNode(resolvedPerk);
                    if (!ref.node || rank > ref.node->ranks.size()) {
                        st.unspentPerk += static_cast<float>(rank);
                        ++droppedPerk;
                        continue;
                    }
                }
                st.perks.push_back({ resolvedPerk, rank });
            }

            std::uint16_t skillCount = 0;
            if (!a_intfc->ReadRecordData(skillCount)) return;
            if (skillCount > kMaxSkillAllocs) {
                spdlog::error("[cosave] implausible skill alloc count {} -- ABORTING progression load", skillCount);
                return;
            }
            for (std::uint16_t s = 0; s < skillCount; ++s) {
                std::uint32_t av = 0;
                float points = 0.0f, lastBase = -1.0f, manual = 0.0f;
                if (!a_intfc->ReadRecordData(av)) return;
                if (!a_intfc->ReadRecordData(points)) return;
                if (!a_intfc->ReadRecordData(lastBase)) return;
                if (a_version >= 2 && !a_intfc->ReadRecordData(manual)) return;   // §16
                if (!resolved) continue;
                if (!IsKnownSkillAv(av)) { ++droppedAv; continue; }   // L2: value, not just count
                if (!std::isfinite(manual) || manual < 0.0f) manual = 0.0f;   // L2 for v2
                st.skills.push_back({ static_cast<RE::ActorValue>(av), points, lastBase, manual });
            }

            std::uint16_t baseCount = 0;
            if (!a_intfc->ReadRecordData(baseCount)) return;
            if (baseCount > kMaxSkillAllocs) {
                spdlog::error("[cosave] implausible baseline count {} -- ABORTING progression load", baseCount);
                return;
            }
            for (std::uint16_t b = 0; b < baseCount; ++b) {
                std::uint32_t av = 0;
                float value = 0.0f;
                if (!a_intfc->ReadRecordData(av)) return;
                if (!a_intfc->ReadRecordData(value)) return;
                if (!resolved) continue;
                if (!IsKnownSkillAv(av)) { ++droppedAv; continue; }   // L2: value, not just count
                st.baseline.push_back({ static_cast<RE::ActorValue>(av), value });
            }

            if (!resolved) { ++droppedActor; continue; }
            g_prog[resolvedID] = std::move(st);
            ++loaded;
        }
        // Split counters by reason (INVARIANTS #47).
        spdlog::info("[cosave] loaded {} progression record(s); dropped {} unresolvable actor(s), "
                     "{} unresolvable/off-catalog perk alloc(s) (points refunded), "
                     "{} unknown-skill AV entr(ies); lastPlayerLevel {}",
                     loaded, droppedActor, droppedPerk, droppedAv, g_lastPlayerLevel);
    }

    void ClearAll() {
        g_prog.clear();
        g_lastPlayerLevel = 0;
        ++g_pollGen;   // orphan any in-flight poll chain (MainThread::Clear
                       // drops the queued closure too — belt and braces)
        // Drop the published board views: a view built from the old save must
        // never be drawn over a freshly loaded one (the ClearPendingEdits rule
        // applied to reads). OnPostLoad reseeds it.
        std::scoped_lock lk(g_viewMx);
        g_boardSnap.reset();
    }

    // ── dev harness ─────────────────────────────────────────────────────────

    void OnHarnessHotkey() {
        if (!Config::g_progHarness.load()) return;
        if (!g_ready) {
            spdlog::info("[prog] harness: addon absent ({}) — nothing to exercise",
                         Progression::kAddonPlugin);
            return;
        }
        const int cmd = g_devCmd ? static_cast<int>(g_devCmd->value) : 0;
        auto* f = PickFollower();
        if (!f) return;   // logged by the pick
        spdlog::info("[prog] ===== HARNESS cmd {} on {} ({:08X}) =====",
                     cmd, NameOf(f), f->GetFormID());
        switch (cmd) {
        case 0:
            HarnessStatus(f);
            spdlog::info("[prog] harness cmds (console `set MFOP_DevCmd to N`): 0=status "
                         "1=enroll 2=cycle-class 3=skills 4=alloc-next-perk 5=respec 6=economy");
            break;
        case 1:
            Enroll(f);
            break;
        case 2: {
            auto it = g_prog.find(f->GetFormID());
            if (it == g_prog.end() || !it->second.enrolled) {
                spdlog::info("[prog] harness: {} not enrolled — cmd 1 first", NameOf(f));
                break;
            }
            const auto cur = it->second.cls;
            const auto next = (cur == Class::kNone)   ? Class::kMelee
                              : (cur == Class::kMelee) ? Class::kRanged
                              : (cur == Class::kRanged) ? Class::kMage
                                                        : Class::kMelee;
            SetClass(f, next);
            break;
        }
        case 3:
            HarnessSkillDump(f);
            break;
        case 4:
            AllocateNextEligible(f);
            break;
        case 5:
            Respec(f);
            break;
        case 6:
            HarnessEconomyDump();
            break;
        default:
            spdlog::info("[prog] harness: unknown cmd {} (0..6)", cmd);
            break;
        }
    }

}
