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
    // §18.6 Stage 3: ECONOMY is FULLY addon-declared. The DLL no longer reads
    // any economy GLOB by fixed local id — every knob (perk divisor, skill
    // pts/level, manual pts/level, shared-growth divisor, respec rapport cost,
    // skill cap, dev-cmd selector) is a manifest FLST GLOB entry matched by
    // editor-id SUFFIX (ProgAllocator.cpp AssignEconomyGlob), with the DLL's
    // g_econ initializers as the documented default. The old fixed-id GLOB
    // constants (kGlob*) are therefore gone — the generator still emits the
    // records at their frozen local ids, but the DLL finds them via the
    // manifest, not by id/plugin. (Legacy perk-rate 0x802 / veteran-mult 0x806
    // GLOBs remain unread; 0x802 is repurposed to MFOP_LevelsPerPerkPoint.)
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

    // ── classes: N-DECLARED, not fixed (§18.6 Stage 2) ──────────────────────
    // The fixed enum Class + g_class[4] + ClassName is gone. Classes are now
    // declared by the addon: the manifest points to ONE classes-list FLST
    // whose entries are class-def FLSTs. Each class-def declares its display
    // name (a MESG's FULL), its skills (AVIF forms, order = weight), an
    // optional perk-priority list (PERK forms), and a #65 combat-stance
    // mirror (a GLOB whose editor id ends "_Stance", 0-3).
    //
    // Identity = the class-def FLST's FormID — STABLE across sessions and
    // load-order changes (ResolveFormID on the co-save side, PRGN v3), unlike
    // an index into the enumerated list.
    struct ClassDef {
        RE::FormID   id{ 0 };            // the class-def FLST (serialized identity)
        std::string  name;               // the MESG's FULL
        std::uint8_t stance{ 0 };        // #65 combatClassOverride mirror (0 = none)
        std::vector<RE::ActorValue> skills;        // order = weight
        std::vector<RE::FormID>     perkPriority;  // optional, tried first
        // v1.1 §HMS class ratios as manifest DATA (lifted out of the DLL's old
        // hardcoded HmsProfile switch). Parsed POSITIONALLY from the class-def
        // FLST's GLOBs (editor-ids are discarded at runtime, so order not suffix
        // identifies them). v1.1 Phase 7: CONSUMED — HmsProfile() now reads these
        // (keyed by base-class stance), no hardcoded ratio remains. No DLL default.
        float        hmsWeights[3]{ 0.f, 0.f, 0.f };  // H,M,S (raw weights)
        std::uint8_t primaryPool{ 0 };                // 0=H 1=M 2=S
        bool         hmsWeightsSet{ false };
    };
    // Frozen after Init (the catalog discipline) — lock-free reads.
    const std::vector<ClassDef>& Classes();
    const ClassDef* FindClassDef(RE::FormID a_id);

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

        // §18.6: the CLASS-DEF FLST FormID (0 = no class picked yet — the
        // §15 gate). This is the RESOLVED RUNTIME id for THIS session; it is
        // 0 whenever the addon ESL is absent or the class failed to resolve.
        RE::FormID    clsId{ 0 };
        // PRGN v4 (SEV-2 class-wipe fix): the class's STABLE plugin-qualified
        // identity — its SOURCE plugin filename + local FormID. This is what is
        // serialized (not clsId), so a session run WITHOUT the addon can round-
        // trip the class rather than persisting a cleared 0 over it. On save,
        // when clsId != 0 these are re-derived from the live form; when clsId
        // == 0 (unresolved this session) they are echoed back verbatim.
        std::string   clsPlugin;
        RE::FormID    clsLocal{ 0 };
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

        // §16 manual skill points — an OVERRIDE of automatic skill growth,
        // NEVER additive (marth, round-4 correction: stacking both was wrong
        // and overpowered). While the toggle is ON, this follower's ongoing
        // skill progression IS the flat 5 points/level the player places —
        // automatic per-level growth is SUPPRESSED, frozen at the enable
        // level. Still no per-tick accumulator (the SEV-1 lesson):
        //   pool        = (progressionLevel − manualBaselineLevel) × 5
        //                 − manualPointsApplied
        //   autoLevel   = (manual ? manualBaselineLevel : progressionLevel)
        //                 − manualExcludedLevels
        // Each OFF→ON re-latches the baseline (a fresh stint; the pool
        // accrues from zero); each ON→OFF adds the stint's levels to
        // manualExcludedLevels so auto growth never back-fills levels that
        // progressed manually — never both, in any order of toggling. All
        // three fields change ONLY on the toggle transitions — replay-safe.
        std::uint16_t manualBaselineLevel{ 0 };
        std::uint16_t manualPointsApplied{ 0 };
        std::uint16_t manualExcludedLevels{ 0 };

        std::vector<PerkAlloc>  perks;
        std::vector<SkillAlloc> skills;
        std::vector<BaselineAV> baseline;

        // ── HMS class-redistribution (§HMS, PRGN v5) ─────────────────────────
        // A SIBLING of the skill reconcile: capture the engine's per-level
        // Health/Magicka/Stamina award, then re-grant it reshaped to the class
        // profile (Mage 15/80/5, Ranged 40/5/55, Melee 60/5/35). Pool index is
        // FIXED {0=Health, 1=Magicka, 2=Stamina} everywhere (co-save order too).
        //
        // MEASURE-not-clobber (the key difference from ReconcileSkill): each
        // recompute reads cur = GetBaseActorValue(pool), measures the POSITIVE
        // drift (cur - hmsTarget) as this level's fresh engine award BEFORE
        // re-asserting, sums the three pool deltas into the budget, redistributes
        // that budget by class%+skew into hmsCumulative, then holds
        // hmsTarget = hmsBaseline + hmsCumulative (single SetBaseActorValue).
        // Between level-ups base==target so the drift reads 0 (pure reads).
        float hmsBaseline[3]{ 0.0f, 0.0f, 0.0f };   // §HMS: base H/M/S at enroll (the floor)
        float hmsTarget[3]{ 0.0f, 0.0f, 0.0f };     // §HMS: last value MFO wrote (== lastWritten)
        float hmsSkew[3]{ 0.0f, 0.0f, 0.0f };        // §HMS: cumulative skew shifted into/out of pool
        float hmsCumulative[3]{ 0.0f, 0.0f, 0.0f };  // §HMS: running redistributed award per pool
        // §HMS skew usage: battles since the last HMS award (redistribution) and,
        // of those, how many exercised an OFF-CLASS pool. Ratio drives the skew.
        // SERIALIZED (a level-up may be many battles / sessions apart).
        std::uint32_t battlesSinceLevelUp{ 0 };
        std::uint32_t battlesOffClass{ 0 };
        // §HMS: the off-class pool exercised since the last award (0=none, else
        // 1+pool index i.e. 1=Health 2=Magicka 3=Stamina). First-off-class-wins
        // per award window; the skew shifts primary → (this pool - 1). Serialized.
        std::uint8_t  offClassPool{ 0 };
        // §HMS: has the enrollment baseline been captured? A pre-v5 save has no
        // HMS block (stays false) → the first RecomputeHMS ADOPTS the follower's
        // current base H/M/S as the baseline (mirror the skill ADOPT fallback),
        // so existing followers are not retro-slammed. Serialized (a v5 save
        // reloads with the real baseline, never re-adopting an MFO-inflated one).
        bool hmsCaptured{ false };

        // ── §HMS fixed-stat grant (v1.1 Phase 3, PRGN v6) ────────────────────
        // A fixed-stat NPC gets 0 engine HMS award per level (unique/no-autocalc
        // followers), so the redistribution budget is always 0 and it never
        // grows. Phase 3 gives it progression: once the PLAYER's total HMS
        // catches up to this follower's baseline total, grant the follower the
        // player's per-level HMS gain, reshaped to the class profile.
        //   fixedStat            — detected fixed-stat (SERIALIZED as flags bit
        //                          0x20). Set after TWO player level-ups of 0
        //                          engine award; cleared the moment an award > 0
        //                          is measured (it is a leveling follower).
        //   hmsZeroAwardStreak   — consecutive player level-ups this follower saw
        //                          0 engine award (SERIALIZED, clamped 0..2). At
        //                          2 → fixedStat. A single quiet level is not proof.
        //   hmsGrantRemainder    — fractional per-pool grant carried across levels
        //                          so a 15/80/5 split lands as WHOLE base-AV points
        //                          over time instead of truncating each level
        //                          (SERIALIZED). Only ever touched on the grant path.
        bool          fixedStat{ false };
        std::uint8_t  hmsZeroAwardStreak{ 0 };
        float         hmsGrantRemainder[3]{ 0.0f, 0.0f, 0.0f };
        // §HMS fixed-stat detection tally (SERIALIZED, PRGN v6): the engine HMS
        // award measured for this follower since the last player level-up.
        // RecomputeHMS adds each measured (positive) budget; PollWork reads it at
        // the next player level-up to decide 0-award, then zeroes it. MUST be
        // serialized (was runtime-only) — the streak it feeds is serialized, so a
        // save/load BETWEEN two player level-ups would otherwise wipe the award
        // evidence and falsely flag a LEVELING follower fixedStat within 2 levels.
        float         hmsAwardAccum{ 0.0f };

        // §HMS runtime-only, never serialized: combat-edge tracking for the
        // battle counters. hmsInBattle = currently inside a (dwell-smoothed)
        // battle; hmsBattleOffCounted = this battle already counted as off-class;
        // hmsLastCombat = last main-thread instant IsInCombat() read true.
        bool hmsInBattle{ false };
        bool hmsBattleOffCounted{ false };
        std::chrono::steady_clock::time_point hmsLastCombat{};

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
    // Also re-reads the economy so a save's persisted GLOB values apply live.
    void OnPostLoad();

    // MCM/Journal close: re-read the economy knobs from the manifests so an
    // MCM GlobalValue slider (which writes a GLOB's runtime value) takes effect
    // this session, not at next load. Marshals to the true main thread itself,
    // so it is safe to call from the MenuSink's task worker. No-op when the
    // addon is absent. The DLL discovers the economy GLOBs generically off the
    // addon manifest — it never names any addon plugin.
    void OnMenuClose();

    // ── the backend verbs (main thread; named [prog] reject lines) ──────────
    bool Enroll(RE::Actor* a_actor);
    // a_classId = a declared ClassDef's id (§18.6). Auto-scales skills.
    bool SetClass(RE::Actor* a_actor, RE::FormID a_classId);
    // §5 double gate (prereq perk(s)/rank order + perkConditions.IsTrue on
    // the follower at apply time). a_nodePerkID = the catalog node's rank-1
    // form; takes the NEXT untaken rank.
    bool AllocatePerk(RE::Actor* a_actor, RE::FormID a_nodePerkID);
    // Deterministic auto-pick: ESL class-perk list first, then the highest-
    // weight class skill's tree in catalog (BFS/depth) order. Used by the
    // harness now, the board's auto-spend later.
    bool AllocateNextEligible(RE::Actor* a_actor);
    bool Respec(RE::Actor* a_actor);                      // refunds points, −500 rapport

    // Revert/reload generation — bumped by ClearAll (revert) and OnPostLoad.
    // A board prog-edit captures this at post time and bails inside its
    // MainThread::Post closure if it moved, so an edit that survives a revert
    // (MainThread::Clear raced the queue) can't land on the NEXT save's
    // same-FormID actor. Main-thread-only value.
    int PollGeneration();

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
        RE::FormID    clsId{ 0 };        // class-def id (0 = not chosen yet, §15 gate)
        std::string   clsName;           // resolved display name ("" when none)
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
        // Economy cadence, so the board self-describes from the LIVE values
        // instead of hardcoded literals (which drifted from the shipped 2/2).
        int   levelsPerPerkPoint{ 2 };     // §17 perk cadence: floor(level/N)
        int   manualSkillPtsPerLevel{ 2 }; // §16 manual mode: pts banked per level
        // §18.6: the declared classes for the board's dynamic-N prompt.
        std::vector<std::pair<RE::FormID, std::string>> classes;
        std::vector<BoardFollowerView> rows;  // the active party, g_active order
        RE::FormID treeFor{ 0 };              // whose nodes[] below describe
        std::vector<BoardNodeView> nodes;     // catalog order; empty until a focus published
    };

    // v1.1 Phase 6b: the GENERIC board-tab view payload the host renders. The
    // add-on POPULATES it; the host reads only the generic header (label, active)
    // to build the tab and hands `content` to the tab body. One content type
    // today (BoardProgSnap — 6c generalizes the widgets that read it). Add-on-
    // agnostic envelope: the Board snapshot carries a vector of these, one per
    // hosted add-on tab. Immutable once published (refcount-copied like the view).
    struct BoardTabView {
        std::string label;              // tab title (add-on-supplied, no DLL string)
        bool        active{ false };    // payload published + ready → render the tab
        std::shared_ptr<const BoardProgSnap> content;   // the per-add-on payload
    };
    // Render thread: which follower's tree the tab wants published (atomic).
    void SetBoardFocus(RE::FormID a_id);
    // MAIN THREAD only: rebuild + publish the views. PollTick calls this on a
    // cadence while the board is open; the board's queued verbs call it after
    // applying for an immediate echo.
    void PublishBoardViews();
    // Any thread: the latest published views (null before the first publish).
    std::shared_ptr<const BoardProgSnap> CopyBoardViews();
    // v1.1 Phase 6b: the GENERIC hosted board-tab list the board renders — one
    // BoardTabView per add-on that DECLARES a board tab (Manifests()), carrying
    // its label + the published payload. Empty when no add-on declares a tab.
    std::vector<BoardTabView> CopyBoardTabViews();

    // §HMS off-class usage (F3): the combat scheduler (WORKER thread) calls this
    // when a combat gambit FIRES, publishing the action's exercised pool into a
    // race-free per-follower mirror. HmsTrackBattle (main poll) consumes it to
    // credit an off-class battle for the level-up skew. Cheap + self-gating (a
    // no-op when the feature is off or the action is neutral); never serialized.
    void NoteCombatFire(RE::Actor* a_actor, const std::string& a_actionOpcode);

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
