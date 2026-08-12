#pragma once
#include "PCH.h"

// FOLLOWER PROGRESSION — component 2 of 4: the ALLOCATOR backend.
// Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md §4 (apply paths), §5 (gating),
// §6+§15 (economy, as LOCKED by marth), §8 (co-save), §10 (ESL records).
//
// WHAT THIS IS: the engine-mutating half of the progression addon. It
// consumes Progression's frozen catalog (component 1 — never rewritten here)
// and drives the three field-proven mechanisms from the ProgProbe build:
//   P1  TESNPC::AddPerk(perk, rank) + Actor::ApplyPerksFromBase()  (perks)
//   P2  SetBaseActorValue + the §4.2 reconcile                     (skills)
//   P3  GetPerkIndex-guarded reapply at kPostLoadGame              (no doubling)
//
// THE LOCKED MODEL (§15 — do not re-litigate):
//   - AUTO skills, MANUAL perks. No manual skill pool: on class assignment
//     the follower's skills auto-scale to their progression level by class
//     weights. Perks are the only manual allocation.
//   - CLASS GATE: a follower gets NOTHING until the player picks a concrete
//     class (Melee/Ranged/Mage — the #65 combatClassOverride ordinals).
//   - Perk points (§17, round 3 — SUPERSEDES the old scarcity model): DERIVED,
//     never stored: floor(level/3) − native tree perks at enroll − ranks
//     MFO allocated, clamped ≥ 0.
//   - Shared Growth default ON: benched enrollees level at half rate.
//   - Mid-game recruit: progression level = player level, no bonus skill
//     points, NOT perk-penalized (level-matched scarcity-scaled points).
//   - Respec: free of gold, costs −500 RAPPORT (Rapport::Spend).
//   - v1: unique-base followers only (shared-base perk leak, §4.1).
//
// THREADING: every engine mutation runs on the MAIN thread. The poll is a
// MainThread::Post self-chain (the ProgProbe DelayedTick shape); the harness
// hotkey and serialization callbacks are already main-thread. g_prog is
// main-thread-only by construction, no lock — the g_followers discipline.
//
// GATING: everything is inert unless Progression::Detected() (the ESL is the
// switch, §1). Absent = named log line at Init, every verb refuses politely.

namespace MFO::ProgAllocator {

    // ── the ESL contract (records this module READS) ────────────────────────
    // Local ids in MFO_Progression.esl — a FROZEN contract with the generator
    // profile in MFO_GenerateESP.py, exactly like Forms.h ids are with the
    // MFO.esp emitter. 0x800/0x801 are owned by Progression.h (detection).
    //
    // ECONOMY GLOBs: the DLL reads the RECORD DEFAULT at kDataLoaded (§10 —
    // GLOB *values* are save-persisted, a post-load read could be stale).
    // Author-tunable in xEdit; a missing record degrades to the DLL default
    // beside it with a named log line, never an error.
    // UNUSED since the round-3 perk economy (marth, 2026-08-12): perk points
    // are now DERIVED — floor(level/3) − native tree perks at enroll − ranks
    // MFO allocated (design doc §17) — so the per-level rate and the veteran
    // multiplier have no consumer. The records stay in the ESL (frozen ids,
    // author-visible); the DLL simply no longer reads them.
    inline constexpr RE::FormID kGlobPerkPointsPerLevel = 0x802;  // UNUSED (§17)
    inline constexpr RE::FormID kGlobSkillPointsPerLevel = 0x803; // default 3
    inline constexpr RE::FormID kGlobSharedGrowthDivisor = 0x804; // default 2
    inline constexpr RE::FormID kGlobRespecRapportCost   = 0x805; // default 500
    inline constexpr RE::FormID kGlobVeteranCatchupMult  = 0x806; // UNUSED (§17)
    inline constexpr RE::FormID kGlobSkillCap            = 0x807; // default 100
    // Dev-harness command selector — the ONE glob read LIVE on purpose (the
    // console writes the live value: `set MFOP_DevCmd to N`). Dev-only.
    inline constexpr RE::FormID kGlobDevCmd              = 0x808;
    // Per-class AUTO-PICK FormLists. ClassSkills: ordered AVIF forms = the
    // skill priority (position → triangular weight, see the .cpp); authored
    // in the plugin so class behaviour is data, not code. ClassPerks: ordered
    // PERK forms tried FIRST by the auto-picker; ship EMPTY (the ESL can only
    // master Skyrim.esm, and overhauls replace perks) — an overhaul patch can
    // fill them in xEdit; empty/exhausted falls back to the name-agnostic
    // weight-driven pick, which works under any overhaul.
    inline constexpr RE::FormID kListClassSkillsMelee  = 0x810;
    inline constexpr RE::FormID kListClassSkillsRanged = 0x811;
    inline constexpr RE::FormID kListClassSkillsMage   = 0x812;
    inline constexpr RE::FormID kListClassPerksMelee   = 0x818;
    inline constexpr RE::FormID kListClassPerksRanged  = 0x819;
    inline constexpr RE::FormID kListClassPerksMage    = 0x81A;
    // Enrollment tag keyword — RESERVED in the ESL for conditions/SPID use;
    // v1 does not stamp it onto actors (base keyword-array edits are the one
    // write this module has no probe data for).
    inline constexpr RE::FormID kKywdEnrolled          = 0x820;

    // ── classes (§15) — SAME ordinals as FollowerState::combatClassOverride
    // (0=none, 1=Melee, 2=Ranged, 3=Cast/Mage) so SetClass can mirror into
    // the #65 override without a mapping table.
    enum class Class : std::uint8_t { kNone = 0, kMelee = 1, kRanged = 2, kMage = 3 };
    const char* ClassName(Class a_cls);

    // ── per-follower progression state (co-save record 'PRGN', §8) ──────────
    struct PerkAlloc {
        RE::FormID   nodePerkID{ 0 };   // the node identity = rank-1 PERK id
        std::uint8_t rank{ 0 };         // ranks taken (1-based; the base holds
                                        // ONLY ranks[rank-1]'s form, like the player)
    };
    struct SkillAlloc {
        RE::ActorValue av{ RE::ActorValue::kNone };
        float points{ 0.0f };            // TOTAL applied delta (auto + manual) — the
                                         // §4.2 recovery term, exact by construction
        float lastWrittenBase{ -1.0f };  // §4.2 reconcile anchor; <0 = never wrote
        // §16 manual points the PLAYER spent on this skill (requested, whole).
        // Additive on top of the class auto-share; an entry with manual > 0
        // survives a class change instead of settling out.
        float manualPoints{ 0.0f };
    };
    struct BaselineAV {
        RE::ActorValue av{ RE::ActorValue::kNone };
        float value{ 0.0f };             // natural base at enrollment (respec floor)
    };

    struct ProgState {
        // flags (serialized as one byte)
        bool enrolled{ false };
        bool autoSpend{ false };                     // reserved for the board (comp 3)
        bool veteranConsumed{ false };               // one-shot catch-up granted
        bool wasInPotentialFollowerFaction{ false }; // provenance at enroll (§9.5)
        bool manualSkills{ false };                  // §16 manual skill points ON

        Class         cls{ Class::kNone };
        std::uint16_t progressionLevel{ 0 };
        std::uint16_t sharedGrowthRemainder{ 0 };    // banked player-levels while benched

        // §17 perk economy (marth, round 3): there is NO stored point pool.
        // unspent = max(0, floor(progressionLevel / 3)
        //                  − nativeTreePerksAtEnroll − Σ allocated ranks)
        // — derived every time it is asked for, so it is idempotent across
        // reloads/level-ups by construction. The one serialized input is the
        // enrollment baseline below: how many CATALOG-TREE perk ranks the
        // follower already owned when MFO first met them (pre-trained perks
        // count against the budget — a loaded custom follower doesn't get a
        // double pile; racial/quest perks outside the trees never count).
        std::uint16_t nativeTreePerksAtEnroll{ 0 };

        // §16 manual skill-point pool — NO incremental accumulator (the round-2
        // SEV-1 lesson): the pool is a PURE FUNCTION of serialized baselines,
        //   available = (progressionLevel − manualBaselineLevel) × 5
        //               − manualPointsApplied
        // (flat 5/level by decree — marth, round 3; a separate economy from
        // the §17 perk points) so any replay/reload/level recompute lands on
        // the same number. manualBaselineLevel latches ONCE at first enable
        // (0 = never enabled); the toggle afterwards only gates spending,
        // never mutates the math.
        std::uint16_t manualBaselineLevel{ 0 };
        std::uint16_t manualPointsApplied{ 0 };

        std::vector<PerkAlloc>  perks;
        std::vector<SkillAlloc> skills;
        std::vector<BaselineAV> baseline;

        // runtime-only, never serialized: has this session's guarded reapply
        // (P3) run for this follower yet? Reset by ClearAll; the poll retries
        // until the actor resolves.
        bool applied{ false };
    };

    // Keyed on the actor's persistent FormID (the g_followers discipline).
    // MAIN THREAD ONLY, no lock.
    inline std::unordered_map<RE::FormID, ProgState> g_prog;

    // ── lifecycle ───────────────────────────────────────────────────────────
    // kDataLoaded, main thread, after Progression::Init(): read the ESL's
    // economy GLOB defaults + class FormLists. Inert (one named line) when
    // the addon is absent.
    void Init();

    // Addon detected AND economy initialized — every verb gates on this.
    bool Active();

    // kPostLoadGame/kNewGame, main thread, AFTER the co-save loaded: start
    // the level poll and queue the guarded reapply for every enrolled record.
    void OnPostLoad();

    // ── the backend verbs (main thread; named [prog] reject lines) ──────────
    bool Enroll(RE::Actor* a_actor);
    bool SetClass(RE::Actor* a_actor, Class a_cls);       // auto-scales skills
    // §5 double gate (prereq perk(s)/rank order + perkConditions.IsTrue on
    // the follower at apply time). a_nodePerkID = the catalog node's rank-1
    // form; takes the NEXT untaken rank.
    bool AllocatePerk(RE::Actor* a_actor, RE::FormID a_nodePerkID);
    // Deterministic auto-pick: ESL class-perk list first, then the highest-
    // weight class skill's tree in catalog (BFS/depth) order. Used by the
    // harness now, the board's auto-spend later.
    bool AllocateNextEligible(RE::Actor* a_actor);
    bool Respec(RE::Actor* a_actor);                      // refunds points, −500 rapport

    // ── §16 manual skill points (design doc §16 — auto is a default, not a
    // cage; this is the escape hatch for mage/multiclass builds) ────────────
    // Toggle per follower. First enable latches manualBaselineLevel at the
    // current progression level; accrual rate is the SAME ESL GLOB the auto-
    // scale uses (MFOP_SkillPointsPerLevel 0x803) — additive on top of it.
    bool SetManualSkills(RE::Actor* a_actor, bool a_on);
    // Spend one pooled point: chosen skill's base +1 through the ONE
    // SetBaseActorValue call site (ReconcileSkill — baseline floor + exact
    // recovery), clamped at the economy skillCap. Refuses with a named line.
    bool ApplyManualSkillPoint(RE::Actor* a_actor, RE::ActorValue a_av);

    // §17: the derived perk-point pool (see ProgState) — the ONE authority
    // every gate, log line and board view asks.
    int PerkPointsAvailable(const ProgState& a_st);

    // ── board views (component 3 — the Progression tab's read seam) ─────────
    // The tab draws on the RENDER thread; g_prog and every engine read here
    // are main-thread-only (the poll's thread). So the board reads VALUE-ONLY
    // views published on the MAIN thread behind a small mutex as an immutable
    // shared_ptr — Board::PublishSnapshot (task worker) and the render
    // thread's per-frame snapshot copy only bump a refcount, never deep-copy
    // or touch live state. `nodes` is in CATALOG order (skill-major, node
    // order), so the render thread indexes it with prefix sums off the frozen
    // catalog it already reads lock-free.
    struct BoardNodeView {
        std::uint8_t ownedRank{ 0 };     // ranks MFO allocated (0 = none)
        bool         native{ false };    // owned by the load order, not MFO (§4.1 boundary)
        bool         available{ false }; // §5 gate passes NOW (points + prereq + conditions)
        std::string  whyNot;             // locked reason (empty when owned/available/no class)
    };
    struct BoardSkillLine {
        std::string    name;
        RE::ActorValue av{ RE::ActorValue::kNone };   // join key to the catalog's SkillTree.av
        float          base{ 0.0f };     // current base AV (post-scaling)
        float          alloc{ 0.0f };    // MFO's applied share of that base (auto + manual)
        float          manual{ 0.0f };   // §16: the manual share of alloc (requested)
    };
    struct BoardFollowerView {
        RE::FormID    id{ 0 };
        std::string   name;
        bool          eligible{ false };
        std::string   blocker;           // why enrollment is refused (empty when eligible)
        bool          enrolled{ false };
        std::uint8_t  cls{ 0 };          // Class ordinal (0 = class not chosen yet, §15 gate)
        std::uint16_t level{ 0 };
        float         unspentPerk{ 0.0f };    // §17 DERIVED pool (PerkPointsAvailable)
        std::uint16_t nativeAtEnroll{ 0 };    // §17: pre-trained tree ranks (the budget debit)
        std::uint16_t allocatedRanks{ 0 };    // §17: ranks MFO has spent
        bool          manualSkills{ false };  // §16 toggle state
        int           manualAvail{ 0 };       // §16 pool (deterministic, see ProgState)
        std::vector<BoardSkillLine> skills;   // the 18, kSkillNames order
    };
    struct BoardProgSnap {
        bool  active{ false };           // addon detected + catalog built → tab exists
        float respecRapportCost{ 500.0f };
        float skillCap{ 100.0f };        // §16 apply gate — the board disables at cap
        std::vector<BoardFollowerView> rows;  // the active party, g_active order
        RE::FormID treeFor{ 0 };              // whose nodes[] below describe
        std::vector<BoardNodeView> nodes;     // catalog order; empty until a focus published
    };
    // Render thread: which follower's tree the tab wants published (atomic).
    void SetBoardFocus(RE::FormID a_id);
    // MAIN THREAD only: rebuild + publish the views. PollTick calls this on a
    // cadence while the board is open; the board's queued verbs call it after
    // applying for an immediate echo.
    void PublishBoardViews();
    // Any thread: the latest published views (null before the first publish).
    std::shared_ptr<const BoardProgSnap> CopyBoardViews();

    // ── co-save ('PRGN' — independent record beside FLWR/MSTK) ──────────────
    void CoSaveSave(SKSE::SerializationInterface* a_intfc);
    void CoSaveLoad(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version);
    void ClearAll();   // RevertCallback

    // ── dev harness (bProgHarness, default OFF; INI-only) ───────────────────
    // One hotkey; the verb comes from the MFOP_DevCmd GLOB, set from the
    // console (`set MFOP_DevCmd to N`): 0=status, 1=enroll, 2=cycle class,
    // 3=dump skills, 4=allocate next eligible perk, 5=respec, 6=economy dump.
    void OnHarnessHotkey();

}
