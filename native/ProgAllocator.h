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
//   - Perk earn rate COPIES the player's (1 point per player level),
//     SCARCITY-SCALED by (follower-effective ranks ÷ player ranks) from the
//     catalog, so the follower affords the same % of their tree.
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
    inline constexpr RE::FormID kGlobPerkPointsPerLevel = 0x802;  // default 1
    inline constexpr RE::FormID kGlobSkillPointsPerLevel = 0x803; // default 3
    inline constexpr RE::FormID kGlobSharedGrowthDivisor = 0x804; // default 2
    inline constexpr RE::FormID kGlobRespecRapportCost   = 0x805; // default 500
    inline constexpr RE::FormID kGlobVeteranCatchupMult  = 0x806; // default 1
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
        float points{ 0.0f };            // auto-granted points currently applied
        float lastWrittenBase{ -1.0f };  // §4.2 reconcile anchor; <0 = never wrote
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

        Class         cls{ Class::kNone };
        std::uint16_t progressionLevel{ 0 };
        std::uint16_t sharedGrowthRemainder{ 0 };    // banked player-levels while benched
        float         unspentPerk{ 0.0f };           // scarcity-scaled points (fractional)

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
