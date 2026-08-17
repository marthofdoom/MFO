#include "PCH.h"
#include <cmath>   // std::floor / std::isfinite — not guaranteed via the PCH
#include "ProgAllocator.h"
#include "Progression.h"
#include "Forms.h"   // §18.6: the addon-manifest sentinel keyword
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

        // ── economy — FULLY addon-declared (§18.6 Stage 3) ─────────────────
        // EVERY field here is declared by the addon via a manifest GLOB matched
        // by editor-id SUFFIX (see AssignEconomyGlob). These initializers ARE
        // the documented DLL DEFAULTS a missing GLOB degrades to — each fall-
        // back gets its own named [prog] line at Init. Read the RECORD DEFAULT
        // at kDataLoaded (§10: GLOB *values* are save-persisted, so a post-load
        // read could be a stale saved number). The perk divisor and the manual
        // rate were C++ constexprs before Stage 3; they now live here so an
        // addon owns them like every other knob.
        struct Economy {
            // §17: perk cadence — 1 point per N follower levels, floor(level/N).
            // (was constexpr kLevelsPerPerkPoint; default 2 — marth 2026-08-13.)
            int   levelsPerPerkPoint    = 2;
            // §6 auto-scale skill points per level (was 3 — marth 2026-08-17).
            float skillPointsPerLevel   = 2.0f;
            // §16 manual pool — flat points/level while the toggle is on (was
            // constexpr kManualSkillPtsPerLevel=5; default 2 — marth 2026-08-17).
            int   manualSkillPtsPerLevel = 2;
            int   sharedGrowthDivisor   = 2;       // §15: benched = half rate
            float respecRapportCost     = 500.0f;  // §15: respec costs rapport
            float skillCap              = 100.0f;
        };
        Economy g_econ;

        bool           g_ready  = false;    // Detected + economy latched
        RE::TESGlobal* g_devCmd = nullptr;  // harness selector — read LIVE on
                                            // purpose (console `set` writes the
                                            // live value); dev-only, never a
                                            // gameplay input

        // ── the declared classes (§18.6 Stage 2 — N, not fixed 3) ───────────
        // Built once at Init from every registered manifest, in manifest ×
        // declaration order, then FROZEN (lock-free reads, the catalog
        // discipline). Identity = the class-def FLST FormID.
        std::vector<ClassDef> g_classes;

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

        // §17: ranks MFO has allocated — each one cost a point.
        int AllocatedRanks(const ProgState& a_st) {
            int total = 0;
            for (const auto& p : a_st.perks) total += p.rank;
            return total;
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
        // drift under replay; a formula cannot). Rate: flat 5/level (marth).
        // Self-clamping: applied can never read as a negative pool.
        int ManualAvail(const ProgState& a_st) {
            if (!a_st.manualSkills || a_st.manualBaselineLevel == 0) return 0;
            const int lvls = std::max(0, static_cast<int>(a_st.progressionLevel) -
                                          static_cast<int>(a_st.manualBaselineLevel));
            const int accrued = lvls * g_econ.manualSkillPtsPerLevel;
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

        // ── the auto-scale (§15: AUTO skills, by class, to level) ───────────
        // total = skillPointsPerLevel × (progressionLevel − 1), split by the
        // class weights, applied through the reconcile. Recomputed whole on
        // every level gain / class change; entries leaving the set settle
        // back to natural and are dropped — UNLESS they carry §16 manual
        // points, which are ADDITIVE on top of the class share and survive
        // any class change (the manual override outranks the auto default).
        void RecomputeSkills(RE::Actor* a_actor, ProgState& a_st, bool a_log) {
            const ClassDef* def = FindClassDef(a_st.clsId);
            if (!def) return;   // no class picked, or the declaring addon left
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) {
                spdlog::warn("[prog] {:08X}: no ActorValueOwner — skill auto-scale skipped",
                             a_actor->GetFormID());
                return;
            }

            // §16 (round-4 correction): manual REPLACES auto. While the
            // toggle is ON the auto total is FROZEN at the stint baseline;
            // levels progressed manually (manualExcludedLevels) are excluded
            // from the auto total forever — a manual-mode follower does NOT
            // auto-gain skills on level-up, and toggling back never
            // back-fills the manual stint.
            const int effAutoLvl = std::max(0,
                (a_st.manualSkills ? static_cast<int>(a_st.manualBaselineLevel)
                                   : static_cast<int>(a_st.progressionLevel)) -
                static_cast<int>(a_st.manualExcludedLevels));
            const float total = g_econ.skillPointsPerLevel *
                                static_cast<float>(std::max(0, effAutoLvl - 1));
            const auto weights = WeightsFor(a_actor, *def);

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

        // A follower's perks live on the BASE (TESNPC), not the actor —
        // Actor::HasPerk alone reports FALSE for a base/native perk the
        // follower genuinely owns (e.g. a root Mastery perk). Ownership must
        // test the base index too, exactly like OwnsAnyRank (§ deck
        // 2026-08-15: Force of Nature read its owned Destruction-Mastery
        // prereq as MISSING because HasPerk-only missed the base perk).
        bool OwnsPerkForm(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            return a_perk && ((a_base && a_base->GetPerkIndex(a_perk).has_value()) ||
                              a_actor->HasPerk(a_perk));
        }

        // Is this perk something a follower could actually allocate on the
        // board — i.e. it lives in a catalog node that survived the §3 filter
        // (at least one non-dead rank)? A HasPerk PREREQ pointing at a perk
        // that is NOT board-allocatable (filtered player-only rank, or a perk
        // in no MFO tree at all) can never be satisfied on a follower, so §5.1
        // already bridges around it — §5.2 must NOT hard-block on it.
        bool PerkAllocatableInCatalog(RE::FormID a_perk) {
            if (!a_perk) return false;
            for (const auto& tree : Progression::Get().skills)
                for (const auto& node : tree.nodes) {
                    bool hasThis = false, anyKept = false;
                    for (const auto& r : node.ranks) {
                        if (r.perkFormID == a_perk) hasThis = true;
                        if (r.verdict != Progression::Verdict::kDead) anyKept = true;
                    }
                    if (hasThis && anyKept) return true;
                }
            return false;
        }

        // §5.2b (deck 2026-08-15): a skill perk's own conditions normally
        // reduce to a pure AND-chain of the skill-level gate
        // (GetBaseActorValue-on-self >= N) plus, in some overhauls, HasPerk
        // <prereq> items that encode a prerequisite as a condition. When such a
        // prereq perk has been FILTERED out of the tree, the engine's
        // whole-chain IsTrue locks the node forever even though the skill is
        // met and §5.1's bridging already routed the prerequisite around the
        // filtered perk. We evaluate that common shape ourselves:
        //   - GetBaseActorValue-on-self, op >= / >  : real skill gate, enforced.
        //   - HasPerk <P>, "req" shape (==1 / >=1 / >0): if P is board-
        //     allocatable it is a real in-tree prereq → enforce ownership; if
        //     P is filtered/absent → treat as satisfied (§5.1 owns it).
        // Anything else — an OR group, an exclusion HasPerk (==0), GetLevel, a
        // quest/faction/global condition, an unhandled opcode — returns
        // kFallback and the caller runs the engine evaluator UNCHANGED, so no
        // exotic perk's gating is ever relaxed by guesswork.
        // One condition item's truth for THIS follower, tri-stated:
        //   kTrue/kFalse — we evaluated it; kUnknown — a shape we won't guess
        //   (the caller then defers the whole perk to the engine).
        // Handled: GetBaseActorValue/GetActorValue-on-self (>= / >), GetLevel-
        // on-self counting MFO's progression levels, and HasPerk "req" shape.
        // A HasPerk on a FILTERED/absent perk is kTrue — the follower can never
        // hold it, §5.1's bridging already owns that prerequisite, so it must
        // not lock the node (the Force-of-Nature / Destruction-Mastery case).
        enum class Tri { kTrue, kFalse, kUnknown };
        Tri EvalCondItem(const RE::CONDITION_ITEM_DATA& d, RE::Actor* a_actor,
                         RE::TESNPC* a_base, RE::ActorValueOwner* a_avo,
                         std::uint16_t a_progLevel) {
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            using Fn = RE::FUNCTION_DATA::FunctionID;
            const auto fn  = d.functionData.function.get();
            const float rhs = d.flags.global
                                  ? (d.comparisonValue.g ? d.comparisonValue.g->value : 0.0f)
                                  : d.comparisonValue.f;
            auto cmp = [&](float lhs) -> Tri {
                switch (d.flags.opCode) {
                case Op::kGreaterThan:          return lhs >  rhs ? Tri::kTrue : Tri::kFalse;
                case Op::kGreaterThanOrEqualTo: return lhs >= rhs ? Tri::kTrue : Tri::kFalse;
                default: return Tri::kUnknown;   // <, ==, != — unusual on a gate, defer
                }
            };
            if (fn == Fn::kGetBaseActorValue || fn == Fn::kGetActorValue) {
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf || !a_avo) return Tri::kUnknown;
                const auto av = static_cast<RE::ActorValue>(
                    reinterpret_cast<std::uintptr_t>(d.functionData.params[0]));
                return cmp(fn == Fn::kGetBaseActorValue ? a_avo->GetBaseActorValue(av)
                                                        : a_avo->GetActorValue(av));
            }
            if (fn == Fn::kGetLevel) {
                if (d.object.get() != RE::CONDITIONITEMOBJECT::kSelf) return Tri::kUnknown;
                return cmp(static_cast<float>(std::max<int>(
                    static_cast<int>(a_actor->GetLevel()), static_cast<int>(a_progLevel))));
            }
            if (fn == Fn::kHasPerk) {
                const bool isReqShape =
                    (d.flags.opCode == Op::kEqualTo && rhs == 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThanOrEqualTo && rhs > 0.0f && rhs <= 1.0f) ||
                    (d.flags.opCode == Op::kGreaterThan && rhs >= 0.0f && rhs < 1.0f);
                if (!isReqShape) return Tri::kUnknown;   // exclusion (==0) / odd shape — defer
                auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                const RE::FormID pid = form ? form->GetFormID() : 0;
                if (!PerkAllocatableInCatalog(pid)) return Tri::kTrue;   // filtered → satisfied
                return OwnsPerkForm(a_actor, a_base, PerkByID(pid)) ? Tri::kTrue : Tri::kFalse;
            }
            return Tri::kUnknown;   // GetLevel-on-target / quest / faction / … — don't guess
        }

        // §5.2 gate over a perk's own conditions, with correct AND/OR grouping:
        // items joined by the isOR flag form a disjunction (the group passes if
        // ANY member is true); groups are ANDed. Any kUnknown item makes the
        // whole perk defer (kFallback) so the engine — never guesswork — decides
        // exotic gating. This is what lets a FILTERED HasPerk (kTrue) unlock a
        // node whether it stands alone or sits inside an OR group.
        enum class CondEval { kPass, kFail, kFallback };
        CondEval EvalPerkConditions(RE::Actor* a_actor, RE::TESNPC* a_base,
                                    RE::BGSPerk* a_rank, std::uint16_t a_progLevel) {
            auto* avo = a_actor->AsActorValueOwner();
            bool result = true;
            for (auto* it = a_rank->perkConditions.head; it;) {
                bool groupVal = false;                 // OR accumulator for this group
                for (;;) {
                    const Tri t = EvalCondItem(it->data, a_actor, a_base, avo, a_progLevel);
                    if (t == Tri::kUnknown) return CondEval::kFallback;
                    groupVal = groupVal || (t == Tri::kTrue);
                    const bool orNext = it->data.flags.isOR;   // OR'd with the next item
                    it = it->next;
                    if (!orNext || !it) break;                 // group ends (or list ends)
                }
                result = result && groupVal;
            }
            return result ? CondEval::kPass : CondEval::kFail;
        }

        // EVALUATIVE dump of a perk's conditions — appended to the refusal on
        // ANY block, annotating each item with the follower's live value and
        // a [T]/[F] verdict, so a genuinely-locked perk names its exact
        // blocker (which skill/level/perk, and what the follower actually has)
        // instead of an opaque "perkConditions false". Diagnostic; bounded.
        std::string DumpConditions(RE::BGSPerk* a_rank, RE::Actor* a_actor,
                                   RE::TESNPC* a_base, std::uint16_t a_progLevel) {
            using Fn = RE::FUNCTION_DATA::FunctionID;
            using Op = RE::CONDITION_ITEM_DATA::OpCode;
            auto* avo = a_actor->AsActorValueOwner();
            std::string out;
            int n = 0;
            for (auto* it = a_rank->perkConditions.head; it && n < 8; it = it->next, ++n) {
                const auto& d = it->data;
                const auto fn = d.functionData.function.get();
                const char* op =
                    d.flags.opCode == Op::kEqualTo             ? "==" :
                    d.flags.opCode == Op::kGreaterThan         ? ">"  :
                    d.flags.opCode == Op::kGreaterThanOrEqualTo ? ">=" : "op?";
                const float v = d.flags.global
                                    ? (d.comparisonValue.g ? d.comparisonValue.g->value : 0.0f)
                                    : d.comparisonValue.f;
                if (!out.empty()) out += (d.flags.isOR ? " OR " : " AND ");
                if (fn == Fn::kGetBaseActorValue || fn == Fn::kGetActorValue) {
                    const auto av = static_cast<RE::ActorValue>(
                        reinterpret_cast<std::uintptr_t>(d.functionData.params[0]));
                    const float have = avo ? (fn == Fn::kGetBaseActorValue
                                                  ? avo->GetBaseActorValue(av)
                                                  : avo->GetActorValue(av))
                                           : 0.0f;
                    out += std::format("{} {} {:g} (have {:.0f})", AvName(av), op, v, have);
                } else if (fn == Fn::kGetLevel) {
                    const int have = std::max<int>(static_cast<int>(a_actor->GetLevel()),
                                                   static_cast<int>(a_progLevel));
                    out += std::format("GetLevel {} {:g} (have {})", op, v, have);
                } else if (fn == Fn::kHasPerk) {
                    auto* form = static_cast<RE::TESForm*>(d.functionData.params[0]);
                    const RE::FormID pid = form ? form->GetFormID() : 0;
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    out += std::format("HasPerk({}) {} {:g} [{}, {}]",
                                       nm && *nm ? nm : "?", op, v,
                                       OwnsPerkForm(a_actor, a_base, pp) ? "owned" : "MISSING",
                                       PerkAllocatableInCatalog(pid) ? "in-tree" : "filtered");
                } else {
                    out += std::format("fn#{} obj{} {} {:g}", static_cast<int>(fn),
                                       static_cast<int>(d.object.get()), op, v);
                }
            }
            return out;
        }

        // §17: how many CATALOG-TREE perk ranks the follower already owns —
        // captured ONCE at enrollment as the budget debit. Per node, the
        // highest rank form present counts that many ranks (rank K implies
        // 1..K, each of which would have cost a point). Perks outside the
        // trees (racial, quest, passives) are invisible here on purpose.
        int CountNativeTreeRanks(RE::Actor* a_actor, RE::TESNPC* a_base) {
            int total = 0;
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes) {
                    for (int r = static_cast<int>(node.ranks.size()); r >= 1; --r) {
                        auto* perk = PerkByID(node.ranks[static_cast<std::size_t>(r - 1)].perkFormID);
                        if (perk && (a_base->GetPerkIndex(perk).has_value() || a_actor->HasPerk(perk))) {
                            total += r;
                            break;
                        }
                    }
                }
            }
            return total;
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
            if (PerkPointsAvailable(a_st) < 1) {
                a_whyNot = std::format("no perk points (earned {}, {} pre-trained, {} spent)",
                                       static_cast<int>(a_st.progressionLevel) /
                                           std::max(1, g_econ.levelsPerPerkPoint),
                                       a_st.nativeTreePerksAtEnroll, AllocatedRanks(a_st));
                return 0;
            }
            // §5.1 prereq: reachable iff root-reachable (empty parentPerkIDs
            // — includes lines that BRIDGE to the root through filtered
            // nodes, round 4) OR at least one parent perk owned at rank ≥1.
            // parentPerkIDs holds the bridged KEPT ancestors — allocatable
            // prereqs, the vanilla any-line rule. The perk's own conditions
            // (§5.2 below) stay the final authority on top.
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
            const CondEval ce = EvalPerkConditions(a_actor, a_base, rankForm, a_st.progressionLevel);
            const bool condPass = (ce == CondEval::kFallback)
                                      ? rankForm->perkConditions.IsTrue(a_actor, a_actor)
                                      : (ce == CondEval::kPass);
            if (!condPass) {
                // Name the real blocker. In the §5.2 fail path it is usually a
                // still-allocatable, unowned condition-prereq — say so — before
                // falling back to the skill string with the follower's value.
                std::string detail;
                for (const auto pid : a_node.condPrereqPerkIDs) {
                    if (!PerkAllocatableInCatalog(pid)) continue;   // filtered — not the blocker
                    if (OwnsAnyRank(a_actor, a_base, a_st, pid))    continue;   // owned — not it
                    auto* pp = PerkByID(pid);
                    const char* nm = pp ? pp->GetName() : nullptr;
                    detail = std::format(" (needs perk: {})", nm && *nm ? nm : "prerequisite");
                    break;
                }
                if (detail.empty() && !rank.skillReq.empty()) {
                    if (rank.skillReqAV != RE::ActorValue::kNone) {
                        if (auto* avo = a_actor->AsActorValueOwner())
                            detail = std::format(" (needs {}, have {:.0f})", rank.skillReq,
                                                 avo->GetBaseActorValue(rank.skillReqAV));
                        else
                            detail = std::format(" (needs {})", rank.skillReq);
                    } else {
                        detail = std::format(" (needs {})", rank.skillReq);
                    }
                }
                detail += std::format(" [cond: {}]",
                                      DumpConditions(rankForm, a_actor, a_base, a_st.progressionLevel));
                a_whyNot = std::format("perkConditions false on follower{}", detail);
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
            // §17: no decrement — the pool is derived, and the rank we just
            // recorded is the debit.

            spdlog::info("[prog] {:08X} GRANTED {} rank {}/{} ({:08X}) — {} point(s) left",
                         a_actor->GetFormID(), a_node.name, a_targetRank,
                         a_node.ranks.size(), rank.perkFormID, PerkPointsAvailable(a_st));
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
            const bool gateable = a_st && a_st->enrolled && FindClassDef(a_st->clsId) != nullptr;
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

            for (auto& [id, st] : g_prog) {
                if (!st.enrolled) continue;

                RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(id);

                if (st.clsId != 0) {
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
                        // §17: no grant — the derived pool follows the level.
                        spdlog::info("[prog] {:08X} level {} (+{}) — {} perk point(s) available "
                                     "(floor(level/{}) − {} spent)",
                                     id, st.progressionLevel, gain, PerkPointsAvailable(st),
                                     g_econ.levelsPerPerkPoint, AllocatedRanks(st));
                        if (actor) RecomputeSkills(actor, st, /*log*/ true);
                    }
                }

                if (actor) {
                    if (!st.applied) {
                        ReapplyFollower(actor, st);   // lazy: runs once the actor resolves
                    } else if (st.clsId != 0 && IsActiveFollower(id)) {
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

        // ── §18.6 manifest class parsing (Init helpers) ─────────────────────
        // NO fixed ids: classes are declared through the enumerated manifests.
        // A class-def is ONE BGSListForm whose entries dispatch by form type:
        // MESG (display name) | AVIF (a skill, order = weight) | PERK (auto-
        // pick priority) | GLOB suffixed "_Stance" (#65 combat-stance mirror).

        // True iff the global's runtime editor id ends with a_suffix. Globals
        // keep their editor ids at runtime (console-addressable), so the
        // stance knob is matched by suffix, never a fixed id.
        bool EdidEndsWith(RE::TESGlobal* a_glob, std::string_view a_suffix) {
            const char* e = a_glob->GetFormEditorID();
            if (!e) return false;
            const std::string_view id{ e };
            return id.size() >= a_suffix.size() &&
                   id.substr(id.size() - a_suffix.size()) == a_suffix;
        }

        // ── §18.6 Stage 3: economy is FULLY addon-declared ──────────────────
        // Each economy knob is a manifest GLOB matched by editor-id SUFFIX.
        // Order in this list == index order in AssignEconomyGlob's returns; it
        // exists only so Init can name the knobs that fell back to the default.
        constexpr std::string_view kEconKnobSuffixes[] = {
            "_LevelsPerPerkPoint", "_ManualSkillPointsPerLevel", "_SkillPointsPerLevel",
            "_SharedGrowthDivisor", "_RespecRapportCost", "_SkillCap",
        };
        constexpr int kEconKnobCount = 6;

        // Assign ONE manifest GLOB to its g_econ field by editor-id SUFFIX and
        // return its kEconKnobSuffixes index (0..5); 6 = the _DevCmd selector
        // (pointer stored, read LIVE); -1 = no economy/dev suffix matched.
        // Manual rate is tested BEFORE the auto rate so the longer id wins even
        // if a future suffix would otherwise be a tail of another. Same clamp
        // discipline the old fixed-id reads used (int knobs floored at 1/0).
        int AssignEconomyGlob(RE::TESGlobal* a_glob) {
            if (EdidEndsWith(a_glob, "_LevelsPerPerkPoint")) {
                g_econ.levelsPerPerkPoint = std::max(1, static_cast<int>(a_glob->value));
                return 0;
            }
            if (EdidEndsWith(a_glob, "_ManualSkillPointsPerLevel")) {
                g_econ.manualSkillPtsPerLevel = std::max(0, static_cast<int>(a_glob->value));
                return 1;
            }
            if (EdidEndsWith(a_glob, "_SkillPointsPerLevel")) {
                g_econ.skillPointsPerLevel = a_glob->value;
                return 2;
            }
            if (EdidEndsWith(a_glob, "_SharedGrowthDivisor")) {
                g_econ.sharedGrowthDivisor = std::max(1, static_cast<int>(a_glob->value));
                return 3;
            }
            if (EdidEndsWith(a_glob, "_RespecRapportCost")) {
                g_econ.respecRapportCost = a_glob->value;
                return 4;
            }
            if (EdidEndsWith(a_glob, "_SkillCap")) {
                g_econ.skillCap = a_glob->value;
                return 5;
            }
            if (EdidEndsWith(a_glob, "_DevCmd")) {
                g_devCmd = a_glob;   // pointer only — the ONE live-read knob
                return 6;
            }
            return -1;
        }

        // §18.6 Stage 3: (re-)read EVERY economy knob from the enumerated addon
        // manifests into g_econ. **Main-thread only** — it writes g_econ (the
        // §5 discipline). Re-runnable by design: called at Init, on post-load
        // (a save persists GLOB *values*, so the player's last tuning applies),
        // and on MCM/Journal close (an MCM GlobalValue slider writes the GLOB's
        // RUNTIME value). Reads glob->value — the live value — so a slider edit
        // or a console `set` takes effect without a reload. Classes are static
        // and are NOT re-parsed here. This is FULLY GENERIC: it walks
        // Progression::Addons() and matches by editor-id SUFFIX; it never names
        // any addon plugin, so the DLL stays ignorant of the addon it reads —
        // exactly as it would a third-party addon.
        void ReloadEconomy() {
            bool resolvedEcon[kEconKnobCount] = {};   // which knobs a manifest set
            for (const auto& addon : Progression::Addons()) {
                auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
                if (!manifest) continue;
                for (auto* form : manifest->forms) {
                    if (!form || form == Forms::g_addonSentinel) continue;
                    auto* glob = form->As<RE::TESGlobal>();
                    if (!glob) continue;   // FLSTs (classes) are Init's job, not economy
                    const int idx = AssignEconomyGlob(glob);
                    const char* edid = glob->GetFormEditorID();
                    if (idx < 0) {
                        spdlog::warn("[prog] manifest {:08X}: GLOB {:08X} \"{}\" matches no "
                                     "economy knob — ignored", addon.manifestID,
                                     glob->GetFormID(), edid ? edid : "");
                    } else {
                        if (idx < kEconKnobCount) resolvedEcon[idx] = true;
                        spdlog::info("[prog] economy: {} = {:g} (from {})",
                                     idx < kEconKnobCount ? kEconKnobSuffixes[idx] : "_DevCmd",
                                     glob->value, edid ? edid : "?");
                    }
                }
            }
            // Name every knob that fell back to the DLL default (diagnosable).
            for (int k = 0; k < kEconKnobCount; ++k)
                if (!resolvedEcon[k])
                    spdlog::info("[prog] economy: {} absent from all manifests — DLL default",
                                 kEconKnobSuffixes[k]);
        }

        // AVIF → ActorValue through the LIVE ActorValueList — no hardcoded
        // AVIF ids, so a load order that replaces the records still maps.
        RE::ActorValue MapSkillAvif(RE::TESForm* a_form) {
            auto* avl = RE::ActorValueList::GetSingleton();
            if (!avl) return RE::ActorValue::kNone;
            for (const auto& s : kSkillNames) {
                auto* avi = avl->GetActorValue(s.av);
                if (avi && avi->GetFormID() == a_form->GetFormID()) return s.av;
            }
            return RE::ActorValue::kNone;
        }

        // One class-def FLST → a ClassDef. Type-dispatched, order-free except
        // within-type order (AVIF order = weight, PERK order = pick priority).
        // Robust: an unresolved/unknown entry is skipped with a warning, never
        // a crash. Returns false when the list declares no usable skills.
        bool ParseClassDef(RE::BGSListForm* a_list, ClassDef& a_out) {
            a_out.id = a_list->GetFormID();
            for (auto* form : a_list->forms) {
                if (!form) continue;
                if (auto* msg = form->As<RE::BGSMessage>()) {
                    const char* full = msg->GetFullName();
                    if (full && *full && a_out.name.empty()) a_out.name = full;
                } else if (auto* glob = form->As<RE::TESGlobal>()) {
                    if (EdidEndsWith(glob, "_Stance"))
                        a_out.stance = static_cast<std::uint8_t>(
                            std::clamp(static_cast<int>(glob->value), 0, 3));
                } else if (form->Is(RE::FormType::Perk)) {
                    a_out.perkPriority.push_back(form->GetFormID());
                } else if (const auto av = MapSkillAvif(form); av != RE::ActorValue::kNone) {
                    a_out.skills.push_back(av);
                } else {
                    spdlog::warn("[prog] class-def {:08X}: entry {:08X} is not a MESG/"
                                 "GLOB/PERK/skill-AVIF — skipped",
                                 a_list->GetFormID(), form->GetFormID());
                }
            }
            if (a_out.name.empty())
                a_out.name = std::format("Class {:08X}", a_out.id);
            return !a_out.skills.empty();
        }

        // Class display name by id — "none" for 0/unknown (log convenience).
        const char* ClsName(RE::FormID a_id) {
            const auto* def = FindClassDef(a_id);
            return def ? def->name.c_str() : "none";
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
                         "{} perk point(s) available (§17: floor(lvl/{}) − {} pre-trained − "
                         "{} spent) | {} perk(s) allocated | {} | "
                         "sharedRemainder {} | level-matched {} | provenance PFF={}",
                         NameOf(a_actor), id, ClsName(st.clsId), st.progressionLevel,
                         PerkPointsAvailable(st), g_econ.levelsPerPerkPoint,
                         st.nativeTreePerksAtEnroll, AllocatedRanks(st), st.perks.size(),
                         IsActiveFollower(id) ? "ACTIVE" : "benched",
                         st.sharedGrowthRemainder,
                         st.veteranConsumed ? "yes" : "pending",
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
            if (it == g_prog.end() || !it->second.enrolled || it->second.clsId == 0) {
                spdlog::info("[prog] skills {}: no class set — nothing auto-scaled", NameOf(a_actor));
                return;
            }
            auto* avo = a_actor->AsActorValueOwner();
            if (!avo) return;
            spdlog::info("[prog] skills {} (class {}, level {}):", NameOf(a_actor),
                         ClsName(it->second.clsId), it->second.progressionLevel);
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
            spdlog::info("[prog] economy: perk = 1 per {} level(s) (§17 derived, minus "
                         "pre-trained + spent) | skill/lvl {:g} | manual skill/lvl {} | "
                         "sharedDiv {} | respec rapport {:g} | cap {:g} | catalog ranks {}/{} | "
                         "lastPlayerLevel {}",
                         g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                         g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                         g_econ.respecRapportCost, g_econ.skillCap, c.effectiveRanks,
                         c.totalRanks, g_lastPlayerLevel);
        }

    }   // anonymous namespace

    // §17: THE perk-point authority — derived, never stored. Idempotent
    // across reloads and level-ups by construction; clamped at 0 so a
    // heavily pre-trained follower is simply "ahead", never negative.
    int PerkPointsAvailable(const ProgState& a_st) {
        const int earned = static_cast<int>(a_st.progressionLevel) /
                           std::max(1, g_econ.levelsPerPerkPoint);
        // native tree perks NO LONGER subtracted (marth 2026-08-13): a follower's
        // starting perks are their build, not a debt to earn back. available = earned - spent.
        return std::max(0, earned - AllocatedRanks(a_st));
    }

    const std::vector<ClassDef>& Classes() { return g_classes; }

    const ClassDef* FindClassDef(RE::FormID a_id) {
        if (a_id == 0) return nullptr;
        for (const auto& def : g_classes)
            if (def.id == a_id) return &def;
        return nullptr;
    }

    bool Active() { return g_ready; }

    void Init() {
        if (!Progression::Detected()) {
            // One named line, never an error — the addon is optional (§1).
            spdlog::info("[prog] allocator inert (addon absent)");
            return;
        }
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) {
            spdlog::error("[prog] TESDataHandler unavailable — allocator cannot initialize");
            return;
        }

        // §18.6 Stage 3: CLASSES **and** ECONOMY are declared through the
        // enumerated manifests — NO fixed-id GLOB reads survive. Per registered
        // manifest, the ONE non-sentinel FLST is the classes list (its entries
        // are class-def FLSTs, ParseClassDef); every GLOB entry is an economy
        // knob matched by editor-id SUFFIX (AssignEconomyGlob), last-writer-wins
        // across manifests in load order. g_econ's initializers stand as the
        // documented DEFAULT for any knob no manifest declares. Values are the
        // RECORD DEFAULTS (kDataLoaded is pre-save, §10); _DevCmd stores the
        // pointer (read LIVE). unused (unread) legacy GLOBs — perk rate 0x802 /
        // veteran mult 0x806 — are simply never in the manifest.
        // §18.6 Stage 3: the ECONOMY read is factored into ReloadEconomy() so
        // it can re-run on post-load and MCM close (live GLOB values). Run it
        // once here for the initial (record-default) latch; the loop below then
        // parses only the CLASSES (static — parsed once at Init).
        ReloadEconomy();
        for (const auto& addon : Progression::Addons()) {
            auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
            if (!manifest) continue;   // enumerated moments ago; belt and braces
            RE::BGSListForm* classesList = nullptr;
            for (auto* form : manifest->forms) {
                if (!form || form == Forms::g_addonSentinel) continue;
                if (auto* flst = form->As<RE::BGSListForm>()) {
                    if (!classesList) classesList = flst;
                    else spdlog::warn("[prog] manifest {:08X}: extra FLST {:08X} ignored "
                                      "(one classes list per manifest)",
                                      addon.manifestID, flst->GetFormID());
                } else if (form->As<RE::TESGlobal>()) {
                    // economy GLOB — read by ReloadEconomy() above; nothing here.
                } else {
                    spdlog::warn("[prog] manifest {:08X}: entry {:08X} is neither an FLST "
                                 "(classes) nor a GLOB (economy) — ignored",
                                 addon.manifestID, form->GetFormID());
                }
            }
            int parsed = 0;
            if (classesList) {
                for (auto* form : classesList->forms) {
                    auto* defList = form ? form->As<RE::BGSListForm>() : nullptr;
                    if (!defList) {
                        spdlog::warn("[prog] classes list {:08X}: entry is not a class-def "
                                     "FLST — skipped", classesList->GetFormID());
                        continue;
                    }
                    if (FindClassDef(defList->GetFormID())) continue;   // dedupe across addons
                    ClassDef def;
                    if (ParseClassDef(defList, def)) {
                        spdlog::info("[prog] class \"{}\" ({:08X}): {} skill(s), {} perk "
                                     "priorit(ies), stance {}",
                                     def.name, def.id, def.skills.size(),
                                     def.perkPriority.size(), def.stance);
                        g_classes.push_back(std::move(def));
                        ++parsed;
                    } else {
                        spdlog::warn("[prog] class-def {:08X} (\"{}\") declares no usable "
                                     "skills — dropped", def.id, def.name);
                    }
                }
            }
            spdlog::info("[prog] addon \"{}\" ({:08X}): {} class(es) registered",
                         addon.plugin, addon.manifestID, parsed);
        }

        g_ready = true;
        spdlog::info("[prog] allocator ready: perk = 1 per {} level(s), skill/lvl {:g}, "
                     "manual skill/lvl {}, sharedDiv {}, respec {:g} rapport, cap {:g}, "
                     "{} declared class(es), devCmd {}",
                     g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                     g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                     g_econ.respecRapportCost, g_econ.skillCap, g_classes.size(),
                     g_devCmd ? "resolved" : "MISSING (harness cmds default to 0)");
    }

    void OnPostLoad() {
        if (!g_ready) {
            if (!g_prog.empty())
                spdlog::warn("[prog] {} progression record(s) loaded but the addon is absent — "
                             "inert this session (data preserved, nothing applied)", g_prog.size());
            return;
        }
        // (a) A save persists GLOB *values*, so a slider the player set last
        // session is in the loaded globals now — re-read the economy so it is
        // live, not the record default latched at kDataLoaded (§10).
        ReloadEconomy();
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

    void OnMenuClose() {
        // (b) The MenuSink calls this on MCM/Journal close from its task worker.
        // An MCM GlobalValue slider writes the economy GLOB's RUNTIME value; we
        // re-read it so the change is live this session (mirrors the Config::Read
        // MenuSink). g_econ / g_ready are touched ONLY on the true main thread,
        // so all of it rides MainThread::Post (ProgAllocator's g_prog idiom),
        // never the AddTask worker — do NOT read/write g_econ off-thread.
        MainThread::Post([]() {
            if (!g_ready) return;   // addon absent — nothing to reload
            ReloadEconomy();
            spdlog::info("[prog] economy reloaded on menu close: skill/lvl={:g}, "
                         "perk 1/{} lvl, manual/lvl {}, sharedDiv {}, respec {:g}, cap {:g}",
                         g_econ.skillPointsPerLevel, g_econ.levelsPerPerkPoint,
                         g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                         g_econ.respecRapportCost, g_econ.skillCap);
        });
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
            snap->respecRapportCost = g_econ.respecRapportCost;
            snap->skillCap          = g_econ.skillCap;
            // §18.6: the declared classes, in declaration order — the board's
            // dynamic-N class prompt draws exactly these.
            snap->classes.reserve(g_classes.size());
            for (const auto& def : g_classes)
                snap->classes.emplace_back(def.id, def.name);

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
                    v.enrolled       = true;
                    v.clsId          = st->clsId;
                    if (const auto* def = FindClassDef(st->clsId)) v.clsName = def->name;
                    v.level          = st->progressionLevel;
                    v.unspentPerk    = static_cast<float>(PerkPointsAvailable(*st));   // §17 derived
                    v.nativeAtEnroll = st->nativeTreePerksAtEnroll;
                    v.allocatedRanks = static_cast<std::uint16_t>(
                        std::min(AllocatedRanks(*st), 0xFFFF));
                    v.manualSkills   = st->manualSkills;           // §16
                    v.manualAvail    = ManualAvail(*st);
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
        }

        // §17: the perk-budget debit — tree ranks the follower brought along.
        // Captured ONCE, before MFO ever grants anything, so the derived pool
        // can never double-pay a pre-trained follower.
        st.nativeTreePerksAtEnroll =
            static_cast<std::uint16_t>(std::min(CountNativeTreeRanks(a_actor, base), 0xFFFF));

        spdlog::info("[prog] ENROLLED {} ({:08X}) — unique base {:08X}, PotentialFollowerFaction={}, "
                     "baseline {} skill(s) captured, {} native tree rank(s) counted against the "
                     "perk budget (§17). Progression INACTIVE until a class is set (§15).",
                     NameOf(a_actor), id, base->GetFormID(),
                     st.wasInPotentialFollowerFaction ? "yes" : "NO",
                     st.baseline.size(), st.nativeTreePerksAtEnroll);
        return true;
    }

    bool SetClass(RE::Actor* a_actor, RE::FormID a_classId) {
        if (!g_ready) { spdlog::info("[prog] set-class refused: addon absent"); return false; }
        if (!a_actor) return false;
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
        // from the level directly (floor(level/3) − pre-trained − spent), so
        // matching the level IS the catch-up.
        if (!st.veteranConsumed) {
            st.progressionLevel = pl;
            st.veteranConsumed = true;
            spdlog::info("[prog] {} level-matched to player level {} — {} perk point(s) "
                         "available (§17: floor(level/3) − {} pre-trained)",
                         NameOf(a_actor), pl, PerkPointsAvailable(st),
                         st.nativeTreePerksAtEnroll);
        }

        const auto beforeName = std::string(ClsName(st.clsId));
        st.clsId = def->id;

        // #65 alignment: the class's DECLARED stance mirrors into the
        // FollowerState override (0 = no override) so the stance machinery
        // follows the chosen class immediately (§18.6: the ordinal coupling
        // is gone — the addon declares the stance explicitly).
        if (auto* rec = Followers::TryEnsureRecord(id)) {
            rec->combatClassOverride = def->stance;
        }

        RecomputeSkills(a_actor, st, /*log*/ true);
        spdlog::info("[prog] {} class {} -> \"{}\" ({:08X}) — skills auto-scaled to level {} "
                     "({} skill(s) allocated), perk allocation unlocked",
                     NameOf(a_actor), beforeName, def->name, def->id,
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
        if (!g_ready || !a_actor) return false;
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

        // §15: free of gold, −500 rapport (record default; the follower
        // resents the reset). Reuses the Rapport rank machinery wholesale.
        Rapport::Spend(id, g_econ.respecRapportCost, "progression respec");

        spdlog::info("[prog] RESPEC {} ({:08X}): {} perk(s) removed -> {} point(s) available, "
                     "rapport -{:.0f}",
                     NameOf(a_actor), id, removed, PerkPointsAvailable(st),
                     g_econ.respecRapportCost);
        return true;
    }

    // ── §16 manual skill points ─────────────────────────────────────────────

    bool SetManualSkills(RE::Actor* a_actor, bool a_on) {
        if (!g_ready) { spdlog::info("[prog] manual-skills refused: addon absent"); return false; }
        if (!a_actor) return false;
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
        if (!a_actor) return false;
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

    // ── co-save ('PRGN' v2, §8 + §16 + §17) ─────────────────────────────────
    //
    //   u16 lastPlayerLevel
    //   u32 followerCount, then per follower:
    //     u32 formID | u8 flags | u8 class | u16 progressionLevel
    //     u16 sharedGrowthRemainder
    //     v1 ONLY: f32 unspentPerk        (legacy stored pool — read + DISCARDED;
    //                                      §17 derives the pool instead)
    //     v2: u16 manualBaselineLevel | u16 manualPointsApplied
    //         u16 manualExcludedLevels    (§16 override accounting)
    //         u16 nativeTreePerksAtEnroll (§17 budget debit)
    //     u16 perkCount   { u32 nodePerkID, u8 rank }*
    //     u16 skillCount  { u32 av, f32 points, f32 lastWrittenBase,
    //                       v2: f32 manualPoints }*
    //     u16 baseCount   { u32 av, f32 value }*
    //
    // v2 never shipped in any deployed build, so its layout is defined here
    // once, cleanly — no in-between shape to migrate.
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
            // v3 (§18.6): the class is a FORMID now — the class-def FLST
            // (ResolveFormID-stable identity), not an index into a list whose
            // order the load order can change. Replaces the v1/v2 1-byte
            // ordinal at the SAME field position.
            a_intfc->WriteRecordData(st.clsId);
            a_intfc->WriteRecordData(st.progressionLevel);
            a_intfc->WriteRecordData(st.sharedGrowthRemainder);
            // v2: BASELINES only, never a pool value — both the §16 manual
            // pool and the §17 perk pool are recomputed from these, so a
            // save replays to the same numbers by construction.
            a_intfc->WriteRecordData(st.manualBaselineLevel);
            a_intfc->WriteRecordData(st.manualPointsApplied);
            a_intfc->WriteRecordData(st.manualExcludedLevels);   // §16 override accounting
            a_intfc->WriteRecordData(st.nativeTreePerksAtEnroll);

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
            std::uint8_t flags = 0, legacyClsRaw = 0;
            RE::FormID   rawClsId = 0;
            if (!a_intfc->ReadRecordData(flags)) return;
            // v3 widened the class field from a 1-byte ordinal to a 4-byte
            // FormID at the SAME position. Consume exactly the bytes the
            // save's version wrote (INVARIANT #12 — field order preserved).
            if (a_version < 3) {
                // v1/v2 stored the fixed-3 ordinal (1=Melee 2=Ranged 3=Mage).
                if (!a_intfc->ReadRecordData(legacyClsRaw)) return;
            } else {
                if (!a_intfc->ReadRecordData(rawClsId)) return;
            }
            if (!a_intfc->ReadRecordData(st.progressionLevel)) return;
            if (!a_intfc->ReadRecordData(st.sharedGrowthRemainder)) return;
            if (a_version < 2) {
                // v1 stored the (pre-§17) point pool — consume the bytes,
                // discard the value: the pool is derived now. A v1 follower
                // migrates with nativeTreePerksAtEnroll = 0 (the debit was
                // never captured; the derived pool is simply what §17 says).
                float legacyUnspent = 0.0f;
                if (!a_intfc->ReadRecordData(legacyUnspent)) return;
            }
            st.enrolled                       = (flags & 1u) != 0;
            st.autoSpend                      = (flags & 2u) != 0;
            st.veteranConsumed                = (flags & 4u) != 0;
            st.wasInPotentialFollowerFaction  = (flags & 8u) != 0;
            st.manualSkills                   = (flags & 16u) != 0;   // v2 (§16)
            if (a_version < 3) {
                // MIGRATION (§18.6 PRGN discipline): the legacy ordinal maps
                // to the k-th DECLARED class. Pre-v3 saves were only ever
                // written with MFO_Progression.esl as the sole addon, whose
                // manifest declares Melee/Ranged/Mage in exactly that order,
                // so ordinal k = the k-th declared class. No class declared at
                // slot k → class cleared with a named line (the §15 prompt
                // reappears), never a silent drop of the whole record.
                const int ord = std::min<int>(legacyClsRaw, 3);
                if (ord >= 1 && ord <= static_cast<int>(g_classes.size())) {
                    st.clsId = g_classes[ord - 1].id;
                } else if (ord != 0) {
                    spdlog::warn("[cosave] legacy class ordinal {} has no declared "
                                 "class — cleared (re-pick on the board)", ord);
                }
            } else if (rawClsId != 0) {
                RE::FormID resolvedCls = 0;
                if (a_intfc->ResolveFormID(rawClsId, resolvedCls) &&
                    FindClassDef(resolvedCls)) {
                    st.clsId = resolvedCls;
                } else {
                    spdlog::warn("[cosave] class {:08X} unresolvable or no longer "
                                 "declared — cleared (re-pick on the board)", rawClsId);
                }
            }
            if (a_version >= 2) {
                if (!a_intfc->ReadRecordData(st.manualBaselineLevel)) return;
                if (!a_intfc->ReadRecordData(st.manualPointsApplied)) return;
                if (!a_intfc->ReadRecordData(st.manualExcludedLevels)) return;   // §16
                if (!a_intfc->ReadRecordData(st.nativeTreePerksAtEnroll)) return;
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
                    // INVARIANTS #8 + §8: DROP the alloc — the load order lost
                    // this perk. The "refund" is automatic under §17: a dropped
                    // alloc no longer debits the derived pool.
                    ++droppedPerk;
                    continue;
                }
                if (haveCatalog) {
                    // The catalog is this session's truth: a node the current
                    // trees no longer carry (overhaul swap mid-playthrough)
                    // also drops, with the same counter (same automatic refund).
                    auto ref = FindNode(resolvedPerk);
                    if (!ref.node || rank > ref.node->ranks.size()) {
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
                     "{} unresolvable/off-catalog perk alloc(s) (§17 auto-refund), "
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
            spdlog::info("[prog] harness: addon absent — nothing to exercise");
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
            if (g_classes.empty()) {
                spdlog::info("[prog] harness: no classes declared by any addon");
                break;
            }
            // Cycle through the DECLARED classes (§18.6 dynamic-N).
            std::size_t idx = 0;
            for (std::size_t k = 0; k < g_classes.size(); ++k)
                if (g_classes[k].id == it->second.clsId) { idx = k + 1; break; }
            SetClass(f, g_classes[idx % g_classes.size()].id);
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
