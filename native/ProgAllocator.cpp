#include "PCH.h"
#include <cmath>     // std::floor / std::isfinite — not guaranteed via the PCH
#include <fstream>   // ApplyEconomyOverride reads the addon's MCM Settings INI
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
            // §4.2 skill model: when TRUE (default) MFO CANCELS the engine's
            // per-level/autocalc skill growth and applies ONLY its own award —
            // natural is FROZEN at the enrollment baseline, engine drift is
            // reverted each reconcile (see ReconcileSkill). When FALSE (compat)
            // the old ADOPT-drift path runs: engine gains stack under MFO's
            // award. Live, non-save addon MCM/INI knob (no GLOB, no PRGN touch).
            bool  cancelEngineAwards    = true;
            // §15 Shared Growth master toggle. ON (default): a benched follower
            // banks player-levels and converts them at sharedGrowthDivisor (half
            // rate); an active one earns at the player's rate. OFF: everyone
            // matches the player's level outright. v1.1: this was a progression-
            // specific DLL Config global (Config::g_sharedGrowth); it is now an
            // add-on-owned economy knob (the add-on's own MCM/INI, read by
            // ApplyEconomyOverride — same path as cancelEngineAwards, no GLOB, no
            // PRGN touch). Delete the add-on → this default (true) stands; NO
            // progression Config line remains in the DLL.
            bool  sharedGrowthEnabled   = true;
            // §HMS class-redistribution knobs live on the MAIN MFO MCM, NOT here:
            // Config::g_hmsRedistribute (master switch) + Config::g_hmsSkewMaxFrac
            // (skew ceiling). RecomputeHMS reads those directly.
        };
        Economy g_econ;
        // The RECORD DEFAULTS latched at kDataLoaded (before any save loads, so
        // GLOB values are genuine record defaults, §10). g_econ is reset to this
        // and then re-overlaid from the addon's MCM INI on every re-apply — so
        // a save's stale GLOB runtime values NEVER re-enter the economy (the
        // 2026-08-17 perk-pool corruption: a repurposed GLOB's saved value read
        // floor(level/1) = double the perk pool).
        Economy g_econDefaults;

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

        // v1.1 GENERIC add-on manifest model (host-side, add-on-agnostic). Built
        // once at Init ALONGSIDE the progression parse above, then frozen. Parses
        // but is NOT yet consumed — the progression path still drives behavior;
        // later phases route consumers onto this general model. Exposed via
        // Progression::Manifests() (defined at the foot of this TU).
        std::vector<Progression::AddonManifest> g_manifests;

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

        // §HMS fixed-stat grant (v1.1 Phase 3): the running total of the PLAYER's
        // base H/M/S the last poll saw. On a player level-up the positive delta is
        // the LIVE catch-up rate granted to caught-up fixed-stat followers (never a
        // hardcoded number — "whatever the player actually gains"). Serialized in
        // the PRGN v6 header next to g_lastPlayerLevel; O(1), not per-follower.
        // 0 == unobserved (player base HMS is never 0) → the first observation
        // initializes it with a 0 grant, never a giant first-run catch-up.
        float g_playerHmsTotalLast = 0.0f;

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

        // O(1) node index over the FROZEN catalog. FindNode / OwnsAnyRank /
        // PerkAllocatableInCatalog were each a full skill-major scan; called per
        // parent per node inside BuildNodeViews (the ~500ms board publish) they
        // made it O(N^2) — a main-thread hitch. The catalog is frozen after
        // Progression::Init, so one map keyed by EVERY rank's perk FormID -> its
        // NodeRef replaces the scans. Rebuilt only when the catalog identity
        // changes (revert/reload), detected by an O(1) (trees-buffer, tree-count)
        // signature so the lookup path stays O(1) and never re-walks the catalog.
        std::unordered_map<RE::FormID, NodeRef> g_nodeIndex;
        const void* g_nodeIndexBase  = reinterpret_cast<const void*>(-1);   // != any real ptr
        std::size_t g_nodeIndexTrees = 0;

        const std::unordered_map<RE::FormID, NodeRef>& NodeIndex() {
            const auto& skills = Progression::Get().skills;
            const void* base = skills.empty() ? nullptr : static_cast<const void*>(skills.data());
            if (base == g_nodeIndexBase && skills.size() == g_nodeIndexTrees)
                return g_nodeIndex;   // catalog unchanged — reuse
            g_nodeIndex.clear();
            std::size_t total = 0;
            for (const auto& t : skills) total += t.nodes.size();
            g_nodeIndex.reserve(total * 2 + 1);
            for (const auto& tree : skills)
                for (const auto& node : tree.nodes)
                    for (const auto& r : node.ranks)
                        g_nodeIndex.emplace(r.perkFormID, NodeRef{ &tree, &node });
            g_nodeIndexBase  = base;
            g_nodeIndexTrees = skills.size();
            return g_nodeIndex;
        }

        // Find a node by its identity (the rank-1 perk FormID). O(1) via the
        // index; preserves the old scan's rank-1-only semantics (the id must be
        // the node's IDENTITY, not merely one of its later ranks).
        NodeRef FindNode(RE::FormID a_nodePerkID) {
            const auto& idx = NodeIndex();
            auto it = idx.find(a_nodePerkID);
            if (it != idx.end() && it->second.node->perkFormID == a_nodePerkID)
                return it->second;
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

        // ── HMS class-redistribution (§HMS, PRGN v5) ────────────────────────
        //
        // Pool index is FIXED {0=Health, 1=Magicka, 2=Stamina} — this order is
        // the co-save order and the profile-table column order. NEVER reorder.
        static constexpr RE::ActorValue kHmsAV[3] = {
            RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina
        };
        static const char* HmsPoolName(int p) {
            switch (p) { case 0: return "Health"; case 1: return "Magicka"; case 2: return "Stamina"; }
            return "?";
        }

        // Class profile by CombatStyle::Stance ordinal (ClassDef::stance):
        //   1=Melee  60/ 5/35   (primary Health)
        //   2=Ranged 40/ 5/55   (primary Stamina)
        //   3=Mage   15/80/ 5   (primary Magicka)
        //   0/other  → no profile (HMS skipped entirely).
        // Returns false for a stance with no profile.
        bool HmsProfile(std::uint8_t a_stance, float a_out[3], int& a_primary) {
            switch (a_stance) {
            case 1: a_out[0] = 0.60f; a_out[1] = 0.05f; a_out[2] = 0.35f; a_primary = 0; return true;
            case 2: a_out[0] = 0.40f; a_out[1] = 0.05f; a_out[2] = 0.55f; a_primary = 2; return true;
            case 3: a_out[0] = 0.15f; a_out[1] = 0.80f; a_out[2] = 0.05f; a_primary = 1; return true;
            default: return false;
            }
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
        // §HMS off-class-usage mirror (F3). The REAL "off-class gambit fired"
        // signal: the combat scheduler (WORKER thread) publishes the pool a
        // FIRED combat gambit action exercised, into this per-follower mirror;
        // HmsTrackBattle (MAIN poll) consumes it. Mutex-guarded map — the same
        // cross-thread per-follower pattern as CombatStyle::g_owned — so it is
        // race-free without reading g_followers off-thread. Runtime-only, never
        // serialized. The stored byte is a BITMASK of pools fired since the last
        // consume: bit0=Health, bit1=Magicka, bit2=Stamina.
        std::mutex g_hmsFireMx;
        std::unordered_map<RE::FormID, std::uint8_t> g_hmsFiredMask;

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
        void RecomputeHMS(RE::Actor* a_actor, ProgState& a_st, bool a_log, float a_grantBudget = 0.0f) {
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
        bool OwnsExactPerk(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            return a_perk && ((a_base && a_base->GetPerkIndex(a_perk).has_value()) ||
                              a_actor->HasPerk(a_perk));
        }

        // RANK-AWARE prereq ownership (RC65 #1). A HasPerk prereq that names
        // rank J of a MULTI-RANK chain is a ">= J" gate, not "== J": owning a
        // LATER rank K>J in the same `nextPerk` chain necessarily means the
        // follower already passed rank J. But the engine's perk list may carry
        // only the HIGHEST rank held, so an EXACT-form test reports the lower
        // prereq MISSING (marth's "gated == when the skill has multiple
        // levels"). So we walk `nextPerk` FORWARD from the required rank and
        // accept ownership of the rank itself OR any later rank. We deliberately
        // NEVER walk backward — owning an EARLIER rank must NOT satisfy a
        // later-rank prereq. Bounded + cycle-guarded (cap 16, like
        // Progression.cpp:449) against a malformed overhaul record.
        bool OwnsPerkForm(RE::Actor* a_actor, RE::TESNPC* a_base, RE::BGSPerk* a_perk) {
            RE::BGSPerk* const head = a_perk;
            int rankNo = 0;
            for (RE::BGSPerk* r = a_perk; r && rankNo < 16; r = r->nextPerk, ++rankNo) {
                if (rankNo > 0 && r == head) break;   // looped chain — bail
                if (OwnsExactPerk(a_actor, a_base, r)) return true;
            }
            return false;
        }

        // Is this perk something a follower could actually allocate on the
        // board — i.e. it lives in a catalog node that survived the §3 filter
        // (at least one non-dead rank)? A HasPerk PREREQ pointing at a perk
        // that is NOT board-allocatable (filtered player-only rank, or a perk
        // in no MFO tree at all) can never be satisfied on a follower, so §5.1
        // already bridges around it — §5.2 must NOT hard-block on it.
        bool PerkAllocatableInCatalog(RE::FormID a_perk) {
            if (!a_perk) return false;
            // O(1) via the index: the map ties a_perk to the sole node holding it
            // (as one of its ranks); allocatable iff that node has any non-dead rank.
            const auto& idx = NodeIndex();
            auto it = idx.find(a_perk);
            if (it == idx.end() || !it->second.node) return false;
            for (const auto& r : it->second.node->ranks)
                if (r.verdict != Progression::Verdict::kDead) return true;
            return false;
        }

        // Is this perk KNOWN to the catalog AT ALL — present in some node's
        // ranks (kept OR dead) or in a tree's `filtered` list? Distinct from
        // PerkAllocatableInCatalog, which additionally requires a non-dead
        // (allocatable) rank. A prereq HasPerk on a perk in NO tree at all
        // (a quest/SPID-granted gate) is NOT known here — §5.2 must then DEFER
        // it to the engine rather than granting it (a follower may genuinely
        // lack such a perk). Only a KNOWN-but-filtered perk is safe to bypass.
        bool PerkKnownToCatalog(RE::FormID a_perk) {
            if (!a_perk) return false;
            for (const auto& tree : Progression::Get().skills) {
                for (const auto& node : tree.nodes)
                    for (const auto& r : node.ranks)
                        if (r.perkFormID == a_perk) return true;
                for (const auto& f : tree.filtered)
                    if (f.perkFormID == a_perk) return true;
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
                if (!PerkAllocatableInCatalog(pid)) {
                    // A KNOWN-but-filtered catalog perk can never be held by a
                    // follower and §5.1 bridges around it → treat as satisfied
                    // (the Destruction-Mastery case). But a perk in NO tree at
                    // all (quest/SPID-granted gate) is genuinely unknown — don't
                    // grant it; defer to the engine's IsTrue.
                    return PerkKnownToCatalog(pid) ? Tri::kTrue : Tri::kUnknown;
                }
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
                a_whyNot = std::format("no perk points (earned {}, {} spent)",
                                       static_cast<int>(a_st.progressionLevel) /
                                           std::max(1, g_econ.levelsPerPerkPoint),
                                       AllocatedRanks(a_st));
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
            RecomputeHMS(a_actor, a_st, /*log*/ false);   // §HMS: capture baseline / hold target

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
            // Off-worker membership probe: g_activeIds is reassigned by Refresh on
            // the worker, so walk the lock-guarded FormID mirror instead of the
            // live list (SEV-1 cluster).
            return Followers::IsTrackedFast(a_id);
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

            // §HMS Phase 3: the player's LIVE per-level HMS gain (modlist-agnostic,
            // "whatever the player actually gains") — the catch-up rate granted to
            // caught-up fixed-stat followers this level. Measured once on a player
            // level-up, off the player's own base H/M/S total.
            const bool playerLeveled = (d > 0);
            float playerTotalNow = 0.0f, playerGain = 0.0f;
            if (playerLeveled) {
                for (int p = 0; p < 3; ++p) playerTotalNow += Followers::GetFollowerHMS(player, p);
                if (g_playerHmsTotalLast <= 0.0f) {
                    g_playerHmsTotalLast = playerTotalNow;   // first observation → no retro grant
                } else {
                    playerGain           = std::max(0.0f, playerTotalNow - g_playerHmsTotalLast);
                    g_playerHmsTotalLast = playerTotalNow;
                }
            }

            for (auto& [id, st] : g_prog) {
                if (!st.enrolled) continue;

                RE::Actor* actor = RE::TESForm::LookupByID<RE::Actor>(id);

                if (st.clsId != 0) {
                    const bool active = IsActiveFollower(id);
                    const int  lag    = std::max(0, static_cast<int>(pl) - static_cast<int>(st.progressionLevel));
                    int gain = 0;
                    if (!g_econ.sharedGrowthEnabled) {
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
                        if (actor) RecomputeHMS(actor, st, /*log*/ true);   // §HMS
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
                        // §HMS: the drift-watch is the PRIMARY measure site —
                        // it MEASURES the engine's positive HMS drift (the award)
                        // and redistributes it, then holds target. Between awards
                        // base==target so this is pure reads. Battle counting
                        // (for the skew) runs every poll on the active party.
                        HmsTrackBattle(actor, st);
                        // §HMS Phase 3 fixed-stat DETECTION + GRANT (active party
                        // only — a benched follower isn't measured, so it must not
                        // be judged). Evaluated on a player level-up: the award
                        // tallied over the window that just closed decides 0-award.
                        float grantBudget = 0.0f;
                        if (playerLeveled) {
                            const bool gotAward = (st.hmsAwardAccum > 1e-4f);
                            st.hmsAwardAccum = 0.0f;   // close the window
                            if (gotAward) {
                                st.hmsZeroAwardStreak = 0;
                                st.fixedStat          = false;   // it's a leveling follower
                            } else {
                                if (st.hmsZeroAwardStreak < 2) ++st.hmsZeroAwardStreak;
                                if (st.hmsZeroAwardStreak >= 2) st.fixedStat = true;
                            }
                            if (st.fixedStat) {
                                // GATE: freeze until the player's total HMS catches up
                                // to this follower's baseline total, THEN converge the
                                // follower's TOTAL toward the player's total.
                                // v1.1 Phase-3 BACKFILL (marth 2026-08-26): the grant is
                                // NOT the per-level delta — it is the SHORTFALL to the
                                // target granted-HMS max(0, playerTotal - npcTotal). So
                                // an EXISTING fixed-stat follower jumps its owed HMS in
                                // ONE shot at activation (the backfill: playerTotal ->
                                // follower total), then tracks the player per-level as
                                // the target grows. hmsCumulative already holds the
                                // granted running total (no new co-save field);
                                // RecomputeHMS reshapes the budget by class ratio and
                                // carries the fractional remainder to whole points.
                                float npcTotal = 0.0f, grantedTotal = 0.0f;
                                for (int p = 0; p < 3; ++p) {
                                    npcTotal     += st.hmsBaseline[p];
                                    grantedTotal += st.hmsCumulative[p];
                                }
                                const bool caughtUp = (playerTotalNow >= npcTotal);
                                if (caughtUp)
                                    grantBudget = std::max(0.0f,
                                        (playerTotalNow - npcTotal) - grantedTotal);
                                spdlog::info("[hms] {:08X} fixed-stat grant: streak {} caughtUp {} "
                                             "npcTotal {:.0f} playerTotal {:.0f} playerGain {:.1f} "
                                             "granted {:.0f} -> backfill budget {:.1f}",
                                             id, st.hmsZeroAwardStreak, caughtUp, npcTotal,
                                             playerTotalNow, playerGain, grantedTotal, grantBudget);
                            }
                        }
                        RecomputeHMS(actor, st, /*log*/ true, grantBudget);
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
                // Clamp ≥0: a negative respec cost would make Rapport::Spend a
                // GRANT (respec pays the follower).
                g_econ.respecRapportCost = std::max(0.0f, a_glob->value);
                return 4;
            }
            if (EdidEndsWith(a_glob, "_SkillCap")) {
                // Clamp ≥1: skillCap ≤ 0 neutralizes every auto-scale skill write.
                g_econ.skillCap = std::max(1.0f, a_glob->value);
                return 5;
            }
            if (EdidEndsWith(a_glob, "_DevCmd")) {
                g_devCmd = a_glob;   // pointer only — the ONE live-read knob
                return 6;
            }
            return -1;
        }

        // §18.6 Stage 3: read EVERY economy knob's RECORD DEFAULT from the
        // enumerated addon manifests into g_econ. **Main-thread only** — writes
        // g_econ (the §5 discipline). ***CALLED ONCE, AT INIT (kDataLoaded)
        // ONLY.*** Do NOT re-call it post-load or on menu close: a GLOB's
        // runtime value is SAVE-PERSISTED, so a re-read after a load feeds a
        // stale saved number back into the economy (the 2026-08-17 doubled-perk-
        // pool bug — a repurposed GLOB's old value read floor(level/1)). The
        // live MCM override rides a NON-save-persisted INI instead
        // (ApplyEconomyOverride), which is what post-load / menu-close call.
        // Classes are static
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
                    // Skip the self-declaration keyword (manifest entry[0]) generically.
                    if (!form || form->Is(RE::FormType::Keyword)) continue;
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

        // Case-insensitive "does key end with core". INI keys are the addon's
        // MCM ModSetting ids (e.g. "iLevelsPerPerkPoint") — a type char prefix,
        // no leading underscore, so we match the knob core, not the GLOB suffix.
        bool KeyEndsWith(const std::string& a_key, std::string_view a_core) {
            if (a_key.size() < a_core.size()) return false;
            return _stricmp(a_key.c_str() + (a_key.size() - a_core.size()),
                            std::string(a_core).c_str()) == 0;
        }

        // §18.6 (perk-bug fix 2026-08-17): the LIVE MCM override. Reset g_econ to
        // the cached record DEFAULTS, then overlay each registered addon's MCM
        // Settings INI — Data/MCM/Settings/<addon-basename>.ini, which MCM Helper
        // ModSetting controls write and which is NOT save-persisted, so a slider
        // edit is live WITHOUT the stale-save-GLOB perk corruption. The path is
        // derived from the addon plugin name (AddonRef.plugin) — never hardcoded,
        // so the DLL stays addon-agnostic. Main-thread only (writes g_econ).
        // "Manual" is tested before the shorter "SkillPointsPerLevel" tail.
        void ApplyEconomyOverride() {
            g_econ = g_econDefaults;
            auto trim = [](std::string s) {
                s.erase(0, s.find_first_not_of(" \t\r\n"));
                if (auto p = s.find_last_not_of(" \t\r\n"); p != std::string::npos) s.erase(p + 1);
                else s.clear();
                return s;
            };
            for (const auto& addon : Progression::Addons()) {
                std::string mod = addon.plugin;
                if (auto d = mod.rfind('.'); d != std::string::npos) mod.erase(d);
                if (mod.empty()) continue;
                std::ifstream f("Data/MCM/Settings/" + mod + ".ini");
                if (!f) continue;
                std::string line;
                int applied = 0;
                while (std::getline(f, line)) {
                    const auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    const std::string key = trim(line.substr(0, eq));
                    const std::string valS = trim(line.substr(eq + 1));
                    if (key.empty() || valS.empty() ||
                        key.front() == '[' || key.front() == ';' || key.front() == '#') continue;
                    float v;
                    try { v = std::stof(valS); } catch (...) { continue; }
                    if (KeyEndsWith(key, "LevelsPerPerkPoint"))
                        g_econ.levelsPerPerkPoint = std::max(1, static_cast<int>(v));
                    else if (KeyEndsWith(key, "ManualSkillPointsPerLevel"))
                        g_econ.manualSkillPtsPerLevel = std::max(0, static_cast<int>(v));
                    else if (KeyEndsWith(key, "SkillPointsPerLevel"))
                        g_econ.skillPointsPerLevel = v;
                    else if (KeyEndsWith(key, "SharedGrowthDivisor"))
                        g_econ.sharedGrowthDivisor = std::max(1, static_cast<int>(v));
                    else if (KeyEndsWith(key, "RespecRapportCost"))
                        g_econ.respecRapportCost = std::max(0.0f, v);   // ≥0: negative = a grant
                    else if (KeyEndsWith(key, "SkillCap"))
                        g_econ.skillCap = std::max(1.0f, v);            // ≥1: ≤0 kills skill writes
                    else if (KeyEndsWith(key, "CancelEngineAwards"))
                        g_econ.cancelEngineAwards = (v != 0.0f);        // §4.2 revert vs adopt
                    else if (KeyEndsWith(key, "SharedGrowth"))          // AFTER *Divisor above
                        g_econ.sharedGrowthEnabled = (v != 0.0f);       // §15 master toggle
                    else
                        continue;
                    ++applied;
                }
                if (applied)
                    spdlog::info("[prog] economy override from {}.ini: {} knob(s) — perk 1/{} lvl, "
                                 "skill/lvl {:g}, manual/lvl {}, sharedDiv {}, respec {:g}, cap {:g}, "
                                 "cancelEngineAwards {}",
                                 mod, applied, g_econ.levelsPerPerkPoint, g_econ.skillPointsPerLevel,
                                 g_econ.manualSkillPtsPerLevel, g_econ.sharedGrowthDivisor,
                                 g_econ.respecRapportCost, g_econ.skillCap, g_econ.cancelEngineAwards);
            }
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
            // GLOBs are collected in FLST ORDER and interpreted POSITIONALLY — the
            // engine DISCARDS editor-ids for TESGlobal at runtime (GetFormEditorID()
            // returns "", the Phase 2 root cause), so the old "_Stance" suffix match
            // never fired and stance always parsed 0. Order is the reliable carrier:
            //   glob[0] = combat stance (0-3)
            //   glob[1..3] = HMS weights H,M,S   (v1.1 class ratios as data)
            //   glob[4] = primary pool 0=H/1=M/2=S
            std::vector<RE::TESGlobal*> globs;
            for (auto* form : a_list->forms) {
                if (!form) continue;
                if (auto* msg = form->As<RE::BGSMessage>()) {
                    const char* full = msg->GetFullName();
                    if (full && *full && a_out.name.empty()) a_out.name = full;
                } else if (auto* glob = form->As<RE::TESGlobal>()) {
                    globs.push_back(glob);
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
            if (!globs.empty())
                a_out.stance = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(globs[0]->value), 0, 3));
            if (globs.size() >= 4) {
                a_out.hmsWeights[0] = globs[1]->value;
                a_out.hmsWeights[1] = globs[2]->value;
                a_out.hmsWeights[2] = globs[3]->value;
                a_out.hmsWeightsSet  = true;
            }
            if (globs.size() >= 5)
                a_out.primaryPool = static_cast<std::uint8_t>(
                    std::clamp(static_cast<int>(globs[4]->value), 0, 2));
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
            // Iterate the immutable snapshot, not live g_active (Refresh rebuilds
            // it on the worker -> UAF for this off-worker poll). Resolve each id
            // on the form table (main-thread safe here).
            auto snap = Followers::ActiveSnapshot();
            for (RE::FormID id : *snap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(id);
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
                         "{} perk point(s) available (§17: floor(lvl/{}) − {} spent) | "
                         "{} perk(s) allocated | {} | "
                         "sharedRemainder {} | level-matched {} | provenance PFF={}",
                         NameOf(a_actor), id, ClsName(st.clsId), st.progressionLevel,
                         PerkPointsAvailable(st), g_econ.levelsPerPerkPoint,
                         AllocatedRanks(st), st.perks.size(),
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
                         "spent) | skill/lvl {:g} | manual skill/lvl {} | "
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

    // Forwarder so Progression::Manifests() (defined at the foot of this TU, in a
    // different namespace) can reach the anon-namespace store.
    const std::vector<Progression::AddonManifest>& ManifestsRef() { return g_manifests; }

    const ClassDef* FindClassDef(RE::FormID a_id) {
        if (a_id == 0) return nullptr;
        for (const auto& def : g_classes)
            if (def.id == a_id) return &def;
        return nullptr;
    }

    bool Active() { return g_ready; }

    // v1.1: populate the GENERIC add-on manifest model (Progression::AddonManifest)
    // from the just-parsed progression data (g_classes + g_econ) — one manifest per
    // registered add-on. Add-on-agnostic: it copies DATA, no progression concept
    // leaks into a signature. PARSES only; nothing consumes g_manifests yet. Called
    // at the foot of Init, after classes + economy are parsed. Main-thread (§5).
    void BuildGenericManifests() {
        g_manifests.clear();
        for (const auto& addon : Progression::Addons()) {
            Progression::AddonManifest man;
            man.manifestID  = addon.manifestID;
            man.plugin      = addon.plugin;
            man.addonType   = addon.keywordEdid;   // self-declared type (join keyword)
            man.displayName = addon.plugin;        // TODO(Phase 5): richer header carrier
            // Economy + allocation mirror the live g_econ (whatever it resolved to).
            man.economy.levelsPerPerkPoint        = g_econ.levelsPerPerkPoint;
            man.economy.skillPointsPerLevel       = g_econ.skillPointsPerLevel;
            man.economy.manualSkillPointsPerLevel = g_econ.manualSkillPtsPerLevel;
            man.economy.sharedGrowthDivisor       = g_econ.sharedGrowthDivisor;
            man.economy.respecRapportCost         = g_econ.respecRapportCost;
            man.economy.skillCap                  = g_econ.skillCap;
            man.economy.cancelEngineAwards        = g_econ.cancelEngineAwards;
            man.economy.sharedGrowthEnabled       = g_econ.sharedGrowthEnabled;
            // Classes: re-walk this add-on's classes list, pulling the ALREADY-parsed
            // ClassDef (with hmsWeights) via FindClassDef — no re-parse, no duplication.
            auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
            if (manifest) {
                for (auto* form : manifest->forms) {
                    auto* classesList = form ? form->As<RE::BGSListForm>() : nullptr;
                    if (!classesList) continue;   // keyword / economy GLOBs
                    for (auto* cf : classesList->forms) {
                        auto* defList = cf ? cf->As<RE::BGSListForm>() : nullptr;
                        const ClassDef* def = defList ? FindClassDef(defList->GetFormID()) : nullptr;
                        if (!def) continue;
                        Progression::ManifestClass mc;
                        mc.id            = def->id;
                        mc.name          = def->name;
                        mc.stance        = def->stance;
                        mc.skills        = def->skills;
                        mc.perkPriority  = def->perkPriority;
                        mc.hmsWeights[0] = def->hmsWeights[0];
                        mc.hmsWeights[1] = def->hmsWeights[1];
                        mc.hmsWeights[2] = def->hmsWeights[2];
                        mc.primaryPool   = def->primaryPool;
                        mc.hmsWeightsSet = def->hmsWeightsSet;
                        man.classes.push_back(std::move(mc));
                    }
                    break;   // one classes list per manifest (Init warns on extras)
                }
            }
            spdlog::info("[prog] generic manifest {:08X} (\"{}\", type \"{}\"): {} class(es) "
                         "modeled [parsed, unused]", man.manifestID, man.plugin,
                         man.addonType, man.classes.size());
            g_manifests.push_back(std::move(man));
        }
    }

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
        // §18.6 Stage 3: the ECONOMY record-default read is factored into
        // ReloadEconomy(), called ONCE here (kDataLoaded is genuinely pre-save).
        // It is NEVER re-run post-load/menu-close — see ReloadEconomy's header:
        // GLOB runtime values are save-persisted (the doubled-perk-pool bug). The
        // loop below parses only the CLASSES (static — parsed once at Init).
        ReloadEconomy();
        g_econDefaults = g_econ;    // cache the RECORD DEFAULTS (§10) before any
        ApplyEconomyOverride();     // save loads; overlay the MCM INI (if present)
        for (const auto& addon : Progression::Addons()) {
            auto* manifest = RE::TESForm::LookupByID<RE::BGSListForm>(addon.manifestID);
            if (!manifest) continue;   // enumerated moments ago; belt and braces
            RE::BGSListForm* classesList = nullptr;
            for (auto* form : manifest->forms) {
                // Skip the self-declaration keyword (manifest entry[0]) generically.
                if (!form || form->Is(RE::FormType::Keyword)) continue;
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

        // v1.1: mirror everything just parsed into the GENERIC manifest model
        // (parsed, unused this phase — see BuildGenericManifests).
        BuildGenericManifests();
    }

    void OnPostLoad() {
        if (!g_ready) {
            if (!g_prog.empty())
                spdlog::warn("[prog] {} progression record(s) loaded but the addon is absent — "
                             "inert this session (data preserved, nothing applied)", g_prog.size());
            return;
        }
        // §10 (perk-bug fix 2026-08-17): DO NOT re-read the economy from GLOB
        // values here. GLOB values are SAVE-PERSISTED — a save made before an
        // economy GLOB was repurposed/re-defaulted carries a STALE value (e.g.
        // 0x802 held 1 under the old MFOP_PerkPointsPerLevel; reading it post-
        // load gave floor(level/1) = double the perk pool). The economy stays
        // at the RECORD DEFAULTS latched at kDataLoaded. A live MCM override
        // rides a NON-save-persisted INI instead (ApplyEconomyOverride), never
        // the globals.
        ApplyEconomyOverride();   // re-overlay the addon MCM INI (defaults cached)
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
        // (b) The MenuSink calls this on MCM/Journal close. A live MCM override
        // re-applies here from a NON-save-persisted INI (ApplyEconomyOverride),
        // never from the GLOB runtime values (those are save-persisted — the
        // 2026-08-17 perk-pool corruption). Marshals to the true main thread:
        // g_econ / g_ready are touched ONLY there.
        MainThread::Post([]() {
            if (!g_ready) return;   // addon absent — nothing to apply
            ApplyEconomyOverride();
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
            snap->respecRapportCost     = g_econ.respecRapportCost;
            snap->skillCap              = g_econ.skillCap;
            snap->levelsPerPerkPoint    = std::max(1, g_econ.levelsPerPerkPoint);
            snap->manualSkillPtsPerLevel = g_econ.manualSkillPtsPerLevel;
            // §18.6: the declared classes, in declaration order — the board's
            // dynamic-N class prompt draws exactly these.
            snap->classes.reserve(g_classes.size());
            for (const auto& def : g_classes)
                snap->classes.emplace_back(def.id, def.name);

            const RE::FormID focus = g_boardFocus.load();
            // Immutable snapshot, not live g_active: Refresh reassigns it on the
            // worker while this poll runs on the MainThread::Post chain (SEV-1).
            auto activeSnap = Followers::ActiveSnapshot();
            for (RE::FormID snapId : *activeSnap) {
                auto* a = RE::TESForm::LookupByID<RE::Actor>(snapId);
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
            st.hmsCaptured = true;
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

    // ── co-save ('PRGN' v2, §8 + §16 + §17; v5/v6 §HMS) ─────────────────────
    //
    //   u16 lastPlayerLevel
    //   v6: f32 g_playerHmsTotalLast        (§HMS Phase 3 catch-up rate anchor)
    //   u32 followerCount, then per follower:
    //     u32 formID | u8 flags | u8 class | u16 progressionLevel
    //     u16 sharedGrowthRemainder
    //     … (v2/v3/v4/v5 fields) …
    //     v5 §HMS block (END of record): per pool {H,M,S}: f32 baseline,
    //         [v5 ONLY: f32 target — recomputed, not stored in v6], f32 skew,
    //         f32 cumulative; then u32 battlesSinceLevelUp, u32 battlesOffClass,
    //         u8 offClassPool, u8 hmsCaptured
    //     v6 §HMS additions (after hmsCaptured): u8 hmsZeroAwardStreak,
    //         f32 hmsGrantRemainder[3], f32 hmsAwardAccum; + flags bit 0x20 = fixedStat
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

    namespace {
        // PRGN v4 class-identity codec. The class is written as a stable
        // plugin-qualified pair {u16 pluginLen, plugin bytes, u32 localFormID}
        // so it survives an addon-absent session (see Serialization.h v4 note).
        constexpr std::uint16_t kMaxPluginLen = 260;   // MAX_PATH; a plugin name can't exceed it

        void WritePluginName(SKSE::SerializationInterface* a_intfc, const std::string& a_s) {
            const auto len = static_cast<std::uint16_t>(
                std::min<std::size_t>(a_s.size(), kMaxPluginLen));
            a_intfc->WriteRecordData(len);
            if (len) a_intfc->WriteRecordData(a_s.data(), len);
        }
        // Returns false on short read / implausible length — caller aborts the
        // whole load (a desynced byte stream can't be trusted, INVARIANT #12).
        bool ReadPluginName(SKSE::SerializationInterface* a_intfc, std::string& a_out) {
            std::uint16_t len = 0;
            if (!a_intfc->ReadRecordData(len)) return false;
            if (len > kMaxPluginLen) {
                spdlog::error("[cosave] class plugin name length {} exceeds max {} -- aborting",
                              len, kMaxPluginLen);
                return false;
            }
            a_out.resize(len);
            if (len == 0) return true;
            return a_intfc->ReadRecordData(a_out.data(), len) == len;
        }
        // Derive a class-def form's SOURCE plugin filename + local FormID (the
        // stable identity). False if the form is gone this session — then the
        // caller falls back to the already-loaded clsPlugin/clsLocal.
        bool DeriveClassIdentity(RE::FormID a_clsId, std::string& a_plugin, RE::FormID& a_local) {
            auto* form = RE::TESForm::LookupByID(a_clsId);
            if (!form) return false;
            auto* file = form->GetFile(0);
            if (!file) return false;
            a_plugin = std::string(file->GetFilename());
            a_local  = form->GetLocalFormID();
            return true;
        }

        // #21 addon .esl <-> .esp sibling resolve (READ-SIDE ONLY -- no co-save
        // format change). The progression addon ships BOTH an ESL and a Vortex-
        // compatible ESP variant (an ESL that masters a regular ESP is an
        // unbreakable Vortex load-order cycle). Both carry the SAME masters + local
        // ids, so the ONLY difference the DLL sees is the plugin NAME. A save written
        // under one name must still resolve its class under the other. Try the stored
        // name; if that misses and it is one of the two known addon names, retry the
        // sibling. Anything else resolves plainly (no sibling). The next save then
        // self-heals to the actually-loaded plugin (CoSaveSave's DeriveClassIdentity).
        RE::TESForm* LookupAddonForm(RE::FormID a_local, std::string_view a_plugin) {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return nullptr;
            if (auto* f = dh->LookupForm(a_local, a_plugin)) return f;
            constexpr std::string_view kEsl = "MFO_Progression.esl";
            constexpr std::string_view kEsp = "MFO_Progression.esp";
            std::string_view sib;
            if      (a_plugin == kEsl) sib = kEsp;
            else if (a_plugin == kEsp) sib = kEsl;
            else return nullptr;   // not an addon name -> no sibling
            return dh->LookupForm(a_local, sib);
        }
    }

    void CoSaveSave(SKSE::SerializationInterface* a_intfc) {
        if (!a_intfc->OpenRecord(kRecProgression, kProgVersion)) {
            spdlog::error("[cosave] OpenRecord('{}') failed -- progression NOT saved", "PRGN");
            return;
        }
        a_intfc->WriteRecordData(g_lastPlayerLevel);
        // v6 GLOBAL HEADER: the player's running base-HMS total (§HMS Phase 3),
        // right after lastPlayerLevel and before the follower count. Read gated on
        // version>=6; a pre-v6 stream inits it from the live player on load.
        a_intfc->WriteRecordData(g_playerHmsTotalLast);

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
                (st.manualSkills ? 16u : 0u) |   // v2 (§16) — spare bit in the same byte
                (st.fixedStat ? 32u : 0u);       // v6 (§HMS Phase 3) — 0x20, free in v1–v5
            a_intfc->WriteRecordData(flags);
            // v4 (SEV-2 class-wipe fix): the class is written as its STABLE
            // plugin-qualified identity {u16 pluginLen, plugin bytes, u32
            // localFormID}, NOT the runtime clsId (which v3 wrote and a session
            // without the addon could no longer resolve → cleared → 0 persisted
            // → class lost forever). Replaces the v3 4-byte FormID at the SAME
            // field position.
            //   clsId != 0 → resolved this session: re-derive the authoritative
            //     identity from the live form (also self-heals a v3→v4 upgrade).
            //   clsId == 0 but clsPlugin non-empty → failed to resolve THIS
            //     session (addon absent): echo the loaded identity VERBATIM so a
            //     save never persists a cleared class over one that merely
            //     failed to resolve.
            std::string clsPlugin = st.clsPlugin;
            RE::FormID  clsLocal  = st.clsLocal;
            if (st.clsId != 0) {
                std::string p; RE::FormID l = 0;
                if (DeriveClassIdentity(st.clsId, p, l)) { clsPlugin = p; clsLocal = l; }
            }
            WritePluginName(a_intfc, clsPlugin);
            a_intfc->WriteRecordData(clsLocal);
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
            // ── v6 §HMS block — APPENDED at the very END of the record ───────
            // Fixed order, NO count prefix (always exactly 3 pools). Per pool in
            // fixed {Health, Magicka, Stamina} order: baseline, skew, cumulative
            // (f32 each — v6 DROPS hmsTarget, always max(baseline,baseline+cumul),
            // recomputed on load); then the 2 battle counters (u32), the off-class
            // pool (u8), the captured flag (u8), and the v6 Phase-3 additions:
            // hmsZeroAwardStreak (u8), hmsGrantRemainder (f32×3), hmsAwardAccum
            // (f32). fixedStat rides flags bit 0x20 above — NOT a byte here.
            // Read GATED on version
            // in CoSaveLoad (v6 layout / v5 keeps the old 4-f32/pool reader).
            // Nothing above this moved (byte-identical for v1–v4 saves).
            for (int p = 0; p < 3; ++p) {
                a_intfc->WriteRecordData(st.hmsBaseline[p]);
                a_intfc->WriteRecordData(st.hmsSkew[p]);
                a_intfc->WriteRecordData(st.hmsCumulative[p]);
            }
            a_intfc->WriteRecordData(st.battlesSinceLevelUp);
            a_intfc->WriteRecordData(st.battlesOffClass);
            a_intfc->WriteRecordData(st.offClassPool);
            a_intfc->WriteRecordData(static_cast<std::uint8_t>(st.hmsCaptured ? 1u : 0u));
            a_intfc->WriteRecordData(st.hmsZeroAwardStreak);            // v6
            for (int p = 0; p < 3; ++p)
                a_intfc->WriteRecordData(st.hmsGrantRemainder[p]);      // v6
            a_intfc->WriteRecordData(st.hmsAwardAccum);                 // v6 (detection tally, serialized)
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

        // v6 GLOBAL HEADER: the player's running base-HMS total (§HMS Phase 3),
        // read right after lastPlayerLevel. A pre-v6 stream has no such field →
        // seed it from the LIVE player so the first post-load level-up grants no
        // spurious catch-up (belt-and-braces with PollWork's 0-init guard).
        if (a_version >= 6) {
            float ph = 0.0f;
            if (!a_intfc->ReadRecordData(ph)) {
                spdlog::error("[cosave] short read on progression header (playerHms) -- ABORTING");
                return;
            }
            g_playerHmsTotalLast = std::isfinite(ph) ? ph : 0.0f;
        } else {
            float t = 0.0f;
            if (auto* player = RE::PlayerCharacter::GetSingleton())
                for (int p = 0; p < 3; ++p) t += Followers::GetFollowerHMS(player, p);
            g_playerHmsTotalLast = t;
        }

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
            RE::FormID   rawClsId = 0;        // v3 only
            std::string  v4ClsPlugin;        // v4+ only
            RE::FormID   v4ClsLocal = 0;      // v4+ only
            if (!a_intfc->ReadRecordData(flags)) return;
            // The class field has migrated TWICE, always at the SAME position.
            // Consume EXACTLY the bytes the save's version wrote (INVARIANT #12
            // — a wrong byte count desyncs every field after this):
            //   v<3  : 1-byte ordinal        (migrated to the k-th declared class)
            //   v==3 : 4-byte runtime FormID (ResolveFormID'd — but a session
            //          without the addon cleared it: the SEV-2 wipe v4 closes)
            //   v>=4 : {u16 pluginLen, plugin bytes, u32 localFormID} — a stable
            //          plugin-qualified identity that survives an addon-absent
            //          session.
            if (a_version < 3) {
                // v1/v2 stored the fixed-3 ordinal (1=Melee 2=Ranged 3=Mage).
                if (!a_intfc->ReadRecordData(legacyClsRaw)) return;
            } else if (a_version < 4) {
                if (!a_intfc->ReadRecordData(rawClsId)) return;
            } else {
                if (!ReadPluginName(a_intfc, v4ClsPlugin)) return;
                if (!a_intfc->ReadRecordData(v4ClsLocal)) return;
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
            st.fixedStat                      = (flags & 32u) != 0;   // v6 (§HMS Phase 3); 0 in v1–v5
            if (a_version < 3) {
                // MIGRATION (§18.6 PRGN discipline). Pre-v3 saves were ONLY ever
                // written by MFO_Progression.esl as the sole addon, at the FIXED
                // ordinals 1=Melee 2=Ranged 3=Mage. Resolve the ordinal against
                // THAT plugin's known class-def local ids (SEV-3: NOT an index
                // into the global g_classes, which a 2nd addon would shift), and
                // SYNTHESIZE the v4 plugin-qualified identity so the class is KEPT
                // even when the addon is absent this session (SEV-3: the old code
                // cleared clsId when g_classes was empty, then the v4 save wrote an
                // EMPTY identity → class lost forever). Mirrors the v4 reader:
                // keep clsPlugin/clsLocal always, resolve to a runtime clsId only
                // when the plugin is present.
                static constexpr const char* kProgPlugin = "MFO_Progression.esl";
                // Class-def FLST local ids — FROZEN contract with
                // MFO_GenerateESP.py (PGID_CLASSDEF_MELEE/RANGED/MAGE = 0x850/1/2).
                static constexpr RE::FormID kClassDefLocal[3] = { 0x850, 0x851, 0x852 };
                const int ord = static_cast<int>(legacyClsRaw);
                if (ord >= 1 && ord <= 3) {
                    st.clsPlugin = kProgPlugin;
                    st.clsLocal  = kClassDefLocal[ord - 1];
                    // #21 route through the .esl<->.esp sibling resolve so a .esp-only
                    // load still resolves the legacy ordinals (kProgPlugin literal kept).
                    RE::TESForm* form = LookupAddonForm(st.clsLocal, st.clsPlugin);
                    if (form && FindClassDef(form->GetFormID())) {
                        st.clsId = form->GetFormID();
                    } else {
                        spdlog::info("[cosave] legacy class ordinal {} migrated to {}|{:06X} — "
                                     "not resolvable this session (addon absent), identity KEPT "
                                     "(running class-less until it returns)", ord, kProgPlugin, st.clsLocal);
                    }
                } else if (ord > 3) {
                    // DON'T silently clamp a corrupt ordinal to 3 (Mage). Warn and
                    // leave class-less (nothing to keep).
                    spdlog::warn("[cosave] corrupt legacy class ordinal {} (>3) — left "
                                 "class-less (re-pick on the board), NOT mapped to Mage", ord);
                }
                // ord == 0: never had a class; leave class-less silently.
            } else if (a_version < 4) {
                // v3: bare runtime FormID. ResolveFormID + FindClassDef; on
                // FAILURE (addon absent) clsId stays 0 for the session — a v3
                // record carries NO plugin string, so recovering the identity
                // is impossible (the SEV-2 wipe is only fully closed for v4+
                // saves). A v3 record that DOES resolve self-heals: its next
                // save is written in the v4 plugin-qualified form.
                if (rawClsId != 0) {
                    RE::FormID resolvedCls = 0;
                    if (a_intfc->ResolveFormID(rawClsId, resolvedCls) &&
                        FindClassDef(resolvedCls)) {
                        st.clsId = resolvedCls;
                    } else {
                        spdlog::warn("[cosave] v3 class {:08X} unresolvable this session — "
                                     "running class-less (a v3 record carries no plugin "
                                     "string; re-pick on the board, or it self-heals to v4 "
                                     "once it resolves)", rawClsId);
                    }
                }
            } else {
                // v4: plugin-qualified identity. ALWAYS keep clsPlugin/clsLocal
                // (so a save this session — even one taken WITHOUT the addon —
                // echoes them back verbatim rather than persisting a cleared
                // class). Resolve to a runtime clsId only when the plugin is
                // present; absent/removed → clsId 0, all gates already handle
                // "no class", and the next save re-writes the real identity once
                // the addon returns.
                st.clsPlugin = v4ClsPlugin;
                st.clsLocal  = v4ClsLocal;
                if (!v4ClsPlugin.empty()) {
                    // #21 .esl<->.esp sibling resolve (cross-variant save loads either way).
                    RE::TESForm* form = LookupAddonForm(v4ClsLocal, v4ClsPlugin);
                    if (form && FindClassDef(form->GetFormID())) {
                        st.clsId = form->GetFormID();
                    } else {
                        spdlog::info("[cosave] class {}|{:06X} not resolvable this session "
                                     "(addon absent or class removed) — identity KEPT, "
                                     "running class-less until it returns", v4ClsPlugin, v4ClsLocal);
                    }
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

            // ── §HMS block (read ONLY when a_version >= 5) ───────────────────
            // MUST be read UNCONDITIONALLY (even for a dropped/unresolved
            // follower) or every byte after this record desyncs. Fixed order,
            // no count prefix — mirror the write exactly. Finite/NaN-guard every
            // float (a corrupt read must never poison SetBaseActorValue).
            //   v6: per pool baseline,skew,cumulative (3 f32; target DROPPED,
            //       recomputed); then counters+captured; then hmsZeroAwardStreak
            //       (u8) + hmsGrantRemainder (f32×3) + hmsAwardAccum (f32).
            //   v5: per pool baseline,TARGET,skew,cumulative (4 f32) — the stored
            //       target is READ to stay byte-aligned then DISCARDED + recomputed;
            //       the v6 fields default (fixedStat already read false via flags,
            //       streak=0, remainder={0,0,0}).
            if (a_version >= 5) {
                const bool v6 = (a_version >= 6);
                bool hmsOk = true;
                for (int p = 0; p < 3; ++p) {
                    float b = 0.0f, t = 0.0f, sk = 0.0f, c = 0.0f;
                    if (!a_intfc->ReadRecordData(b))  return;
                    if (!v6 && !a_intfc->ReadRecordData(t)) return;   // v5 ONLY: target, discarded below
                    if (!a_intfc->ReadRecordData(sk)) return;
                    if (!a_intfc->ReadRecordData(c))  return;
                    // v5's stored target is NOT trusted (it is always derivable);
                    // exclude it from the finite check — a poisoned target must not
                    // force a re-adopt when baseline/cumulative are sound.
                    if (!std::isfinite(b) || !std::isfinite(sk) || !std::isfinite(c)) hmsOk = false;
                    st.hmsBaseline[p]   = std::isfinite(b)  ? b  : 0.0f;
                    st.hmsSkew[p]       = std::isfinite(sk) ? sk : 0.0f;
                    st.hmsCumulative[p] = std::isfinite(c)  ? c  : 0.0f;
                }
                // RECOMPUTE the held target (never read) — the invariant every
                // write site maintains: target == max(baseline, baseline+cumul).
                for (int p = 0; p < 3; ++p) {
                    const float tgt = st.hmsBaseline[p] + st.hmsCumulative[p];
                    st.hmsTarget[p] = (tgt < st.hmsBaseline[p]) ? st.hmsBaseline[p] : tgt;
                }
                std::uint32_t bSince = 0, bOff = 0;
                std::uint8_t  offPool = 0, captured = 0;
                if (!a_intfc->ReadRecordData(bSince))   return;
                if (!a_intfc->ReadRecordData(bOff))     return;
                if (!a_intfc->ReadRecordData(offPool))  return;
                if (!a_intfc->ReadRecordData(captured)) return;
                st.battlesSinceLevelUp = bSince;
                st.battlesOffClass     = std::min(bOff, bSince);   // ratio ≤ 1 by construction
                st.offClassPool        = (offPool <= 3) ? offPool : 0;   // clamp [0,3]
                // A corrupt (non-finite) HMS payload → force re-ADOPT on the next
                // RecomputeHMS rather than trusting a poisoned baseline.
                st.hmsCaptured = (captured != 0) && hmsOk;
                if (v6) {
                    // v6 Phase-3 additions.
                    std::uint8_t streak = 0;
                    if (!a_intfc->ReadRecordData(streak)) return;
                    st.hmsZeroAwardStreak = std::min<std::uint8_t>(streak, 2);   // clamp 0..2
                    for (int p = 0; p < 3; ++p) {
                        float r = 0.0f;
                        if (!a_intfc->ReadRecordData(r)) return;
                        // contract is 0 <= frac < 1 — reject finite-but-out-of-range
                        // corruption (a huge value would slam SetBaseActorValue).
                        st.hmsGrantRemainder[p] =
                            (std::isfinite(r) && r >= 0.0f && r < 1.0f) ? r : 0.0f;
                    }
                    float acc = 0.0f;   // detection tally (serialized in v6)
                    if (!a_intfc->ReadRecordData(acc)) return;
                    st.hmsAwardAccum = (std::isfinite(acc) && acc >= 0.0f) ? acc : 0.0f;
                }
                // A poisoned HMS payload forces a re-ADOPT (hmsCaptured=false); it
                // must ALSO restart detection so a stale flags-bit 0x20 (fixedStat)
                // or serialized streak/tally can't linger against a freshly
                // re-adopted baseline. Applied AFTER the v6 reads so it wins.
                if (!hmsOk) {
                    st.fixedStat          = false;
                    st.hmsZeroAwardStreak = 0;
                    st.hmsAwardAccum      = 0.0f;
                }
                // a_version == 5: fixedStat=false (flags), streak=0, remainder={0}
                // (struct defaults) — a v5 follower begins fresh 0-award detection.
            }
            // a_version < 5: no HMS block on disk → hmsCaptured stays false
            // (struct default) → the first RecomputeHMS adopts the live base.

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
        g_playerHmsTotalLast = 0.0f;   // §HMS Phase 3: re-seeded on the next load/observe
        ++g_pollGen;   // orphan any in-flight poll chain (MainThread::Clear
                       // drops the queued closure too — belt and braces)
        // §HMS: drop the off-class fire mirror (runtime-only, save-scoped).
        // Own lock, taken + released BEFORE g_viewMx — never nested.
        { std::scoped_lock fl(g_hmsFireMx); g_hmsFiredMask.clear(); }
        // Drop the published board views: a view built from the old save must
        // never be drawn over a freshly loaded one (the ClearPendingEdits rule
        // applied to reads). OnPostLoad reseeds it.
        std::scoped_lock lk(g_viewMx);
        g_boardSnap.reset();
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

    int PollGeneration() { return g_pollGen; }

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

// v1.1: the GENERIC add-on manifest model is host-side (Progression namespace),
// but is populated in this TU where the progression data is parsed. Defined here
// (not in Progression.cpp) so it reads the store directly. Parsed, unused yet.
namespace MFO::Progression {
    const std::vector<AddonManifest>& Manifests() { return MFO::ProgAllocator::ManifestsRef(); }
}
