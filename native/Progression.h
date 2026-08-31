#pragma once
#include "PCH.h"

// FOLLOWER PROGRESSION — component 1 of 4: the CATALOG reader.
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §1 (detection), §2 (runtime AVIF/
// PERK reading), §3 (NPC-effectiveness filter). Allocator, board tab and ESL
// generation are LATER components — nothing here mutates an actor, a form, or
// a save.
//
// WHAT THIS IS: one pass at kDataLoaded (main thread) over the FULLY-MERGED
// AVIF perk trees — Requiem/Ordinator/Vokrii replacements and any Synthesis
// output are the live forms, so the catalog always describes the load order
// that is actually running. The pass emits value-only views (FormIDs +
// extracted strings/numbers, never a stored form pointer — the Snapshot
// discipline) and then the catalog is FROZEN: no writer exists after Init()
// returns, so the render thread may read it lock-free forever.
//
// GATING: the addon ESL is the feature switch (§1). Absent = one named log
// line, no catalog, feature off — never an error (Forms failure doctrine).
// Because MFO_Progression.esl does not exist yet, bProgCatalogDump (INI-only,
// default OFF — the bProgProbe precedent) forces the build + a [prog] census
// dump so the reader is field-verifiable on the deck TODAY.

namespace MFO::Progression {

    // ── the ADDON API front door (§18.6 — the FROZEN public contract) ───────
    // v1.1 SELF-DECLARATION: an addon announces itself with ONE BGSListForm
    // MANIFEST whose FIRST entry is a KEYWORD the addon OWNS, its editor-id
    // ending "_MFOAddonManifest". Init enumerates every such manifest across
    // the merged load order at kDataLoaded (N addons, each its own record — no
    // shared injection point, no by-name plugin lookup, no fixed FormIDs, and
    // crucially NO MFO.esp form referenced, so the addon needs no MFO.esp
    // master: the Vortex fix). Keyword editor-ids PERSIST at runtime (GLOB/FLST
    // edids do not), so this suffix match provably resolves. The manifest's
    // remaining entries declare the addon's classes list + economy GLOBs
    // (parsed in ProgAllocator). Full contract: Docs/ADDON-API.md.
    struct AddonRef {
        RE::FormID  manifestID{ 0 };   // the manifest FLST (runtime FormID)
        std::string plugin;            // source file — logs/migration only
        std::string keywordEdid;       // v1.1 self-declaration keyword edid (the
                                       // "_MFOAddonManifest"-suffixed join key)
    };

    // ── v1.1 GENERIC add-on manifest model (host-side, add-on-AGNOSTIC) ──────
    // The DLL reads an add-on's self-declaration into THIS general in-memory
    // model — deliberately NOT named for progression. Populated ALONGSIDE the
    // existing progression parse (which still drives behavior); consumers are
    // routed onto it in later phases. Every progression JUDGMENT here is DATA
    // the add-on declares, with NO DLL default (the governing rule): delete the
    // add-on and this vector is simply empty.
    struct ManifestClass {
        RE::FormID   id{ 0 };                 // class-def FLST FormID (identity)
        std::string  name;                    // MESG FULL
        std::uint8_t stance{ 0 };             // combat stance 0-3
        std::vector<RE::ActorValue> skills;   // order = weight
        std::vector<RE::FormID>     perkPriority;
        float        hmsWeights[3]{ 0.f, 0.f, 0.f };  // H,M,S — add-on DATA, no DLL default
        std::uint8_t primaryPool{ 0 };        // 0=H 1=M 2=S
        bool         hmsWeightsSet{ false };  // false = the add-on declared none
    };
    struct ManifestEconomy {
        int   levelsPerPerkPoint{ 0 };
        float skillPointsPerLevel{ 0.f };
        int   manualSkillPointsPerLevel{ 0 };
        int   sharedGrowthDivisor{ 0 };
        float respecRapportCost{ 0.f };
        float skillCap{ 0.f };
        bool  cancelEngineAwards{ true };
        bool  sharedGrowthEnabled{ true };   // §15 master toggle (add-on-owned)
    };
    struct ManifestAllocation {           // allocation-rule scalars (Phase 7)
        std::uint8_t engineAwardPolicy{ 0 };  // 0=Revert 1=Adopt
        float        hmsSkewMaxFrac{ 0.f };
    };
    // PLACEHOLDERS — fields declared now, populated/consumed in Phases 6-7.
    struct ManifestVerdict { std::uint8_t entryPointIndex{ 0 }; std::uint8_t verdict{ 0 }; };
    struct ManifestBoardTab { bool declared{ false }; };   // composition — Phase 6
    struct AddonManifest {
        // header
        std::string addonType;        // self-declared type (from the manifest keyword)
        float       version{ 0.f };   // TODO(Phase 5): carry from the manifest
        std::string displayName;
        RE::FormID  manifestID{ 0 };
        std::string plugin;
        // parsed content (mirrors the live progression parse for now)
        std::vector<ManifestClass> classes;
        ManifestEconomy            economy;
        ManifestAllocation         allocation;
        // Phase 6-7 placeholders — declared, not yet populated/consumed.
        std::vector<ManifestVerdict> entryPointVerdicts;
        ManifestBoardTab             boardTab;
    };
    // Frozen after Init (built once, alongside the progression parse).
    const std::vector<AddonManifest>& Manifests();

    // ── the frozen catalog (§2.3) ───────────────────────────────────────────

    // §3 verdicts. kDead never appears inside a kept PerkNodeView — perks
    // whose EVERY rank is dead are excluded from `nodes` and recorded in
    // `filtered` with a reason instead (they must never reach the board).
    enum class Verdict : std::uint8_t {
        kEffective = 0,   // does something real on an NPC follower
        kMarginal  = 1,   // §3 marginal set — board shows behind "show marginal"
        kDead      = 2,   // player UI/mechanics only — filtered out
    };

    // One RANK of a perk (the nextPerk chain, §2.2).
    struct RankView {
        RE::FormID  perkFormID{ 0 };
        // Display-only skill gate from perkConditions ("One-Handed 40"; empty
        // = none found). ENFORCEMENT never parses this — §5 re-evaluates the
        // full perkConditions on the follower via the kept FormID.
        std::string skillReq;
        // Numeric form of the SAME first GetBaseActorValue-on-self gate, kept
        // beside the display string so §5's refusal can name the follower's
        // ACTUAL value ("needs Destruction 50, have 47") — diagnosis + UX.
        // kNone = no skill-level condition was found.
        RE::ActorValue skillReqAV{ RE::ActorValue::kNone };
        float          skillReqVal{ 0.0f };
        Verdict     verdict{ Verdict::kDead };
    };

    // One perk-carrying tree node — everything the board needs to draw and
    // the allocator needs to gate, with no live pointer kept.
    struct PerkNodeView {
        RE::FormID    perkFormID{ 0 };    // rank 1
        std::string   name;
        std::string   description;
        std::uint32_t gridX{ 0 };         // integer grid — controller nav
        std::uint32_t gridY{ 0 };
        float         hpos{ 0.0f };       // float dome coords — layout
        float         vpos{ 0.0f };
        // Prereq graph, BRIDGED THROUGH FILTERED NODES (deck round 4): a
        // filtered direct parent (dead entry-less marker, player-only perk)
        // is not allocatable, so wiring it as the prereq left its whole
        // subtree permanently locked AND drew its children as root-row
        // orphans. Both views now resolve THROUGH filtered parents to the
        // nearest KEPT ancestors:
        //  - parentIndices: kept-ancestor indices into this skill's `nodes`
        //    (the edges the board draws AND tiers by).
        //  - parentPerkIDs: the same ancestors by rank-1 FormID — the §5
        //    gate's reachability truth (ANY owned unlocks, the vanilla
        //    any-line rule). A line that reaches the ROOT through only
        //    filtered nodes makes the node root-reachable: parentPerkIDs is
        //    cleared (empty = reachable), exactly like a first-tier perk.
        // The perk's own CONDITIONS remain the final authority either way —
        // §5 evaluates the full perkConditions on the follower at gate time.
        std::vector<std::uint32_t> parentIndices;
        std::vector<RE::FormID>    parentPerkIDs;
        // Same-authority prereqs the OVERHAUL wrote as conditions instead of
        // tree edges: rank-1 perkConditions items of the narrow shape
        // "HasPerk X == 1 (AND)". Enforcement already happens via IsTrue;
        // this list exists so the LAYOUT can tier by them too (visual order
        // must match dependency order).
        std::vector<RE::FormID>    condPrereqPerkIDs;
        std::vector<RankView>      ranks;
        Verdict       verdict{ Verdict::kEffective };   // best rank's verdict
    };

    // A perk the filter removed — kept so the [prog] dump can explain every
    // exclusion (§3: over-filtering under a new overhaul must be diagnosable
    // from a deck log).
    struct FilteredPerk {
        RE::FormID  perkFormID{ 0 };
        std::string name;
        std::string reason;
    };

    struct SkillTree {
        RE::ActorValue av{ RE::ActorValue::kNone };
        std::string    skillName;         // AVIF full name ("One-Handed")
        std::vector<PerkNodeView> nodes;      // effective + marginal only
        std::vector<FilteredPerk> filtered;   // dead — never reach the board
        // §15 scarcity inputs: the player can take every rank; the follower
        // only the effective ones. Summed over skills these give the
        // (follower pool ÷ player pool) perk-point scale.
        int totalRanks{ 0 };       // player pool (pre-filter)
        int effectiveRanks{ 0 };   // follower pool (kEffective only)
    };

    struct Catalog {
        bool built{ false };
        std::vector<SkillTree> skills;
        int keptPerks{ 0 };
        int marginalPerks{ 0 };
        int filteredPerks{ 0 };
        int totalRanks{ 0 };
        int effectiveRanks{ 0 };
    };

    // ── API ─────────────────────────────────────────────────────────────────

    // ≥1 addon manifest found in the load order this session (latched at
    // Init). The tab, the allocator and the catalog all gate on this.
    bool Detected();

    // Every enumerated addon manifest, load-order stable (latched at Init,
    // immutable after — lock-free reads, the catalog discipline).
    const std::vector<AddonRef>& Addons();

    // The frozen catalog. `built == false` (empty) when the addon is absent
    // and no dump was forced. Immutable after Init() — lock-free reads.
    const Catalog& Get();

    // kDataLoaded, MAIN THREAD, once: detect the addon, resolve the version
    // GLOB, build the catalog (when detected OR bProgCatalogDump), and emit
    // the [prog] census when bProgCatalogDump is set.
    void Init();

}
