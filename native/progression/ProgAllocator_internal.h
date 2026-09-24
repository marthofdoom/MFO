#pragma once
// progression/ProgAllocator_internal.h -- the ProgAllocator family's SHARED
// substrate. NOT a public API: only the native/progression/*.cpp TUs may include
// this (the public API is progression/ProgAllocator.h). History: one TU
// (ProgAllocator.cpp) used to hold all of this in its anonymous namespaces; the
// mechanical module split (ProgAllocator.cpp / _Hms / _Manifest) moved the
// cross-module state, types, and constants here as `inline` (ONE shared
// instance across the TUs -- never per-TU copies), and declares the big
// cross-module helpers next to the module that defines them. The wave-1
// subsystem-folder split (2026-09-24) cut ProgAllocator.cpp by concern and added
// the section at the END of this file. Single-module helpers stay file-local.

#include "PCH.h"
#include "ProgAllocator.h"
#include "Progression.h"
#include "Followers.h"   // wave-1 split: IsActiveFollower (below) reads Followers::IsTrackedFast
#include <mutex>          // g_hmsFireMx (worker-thread fire mirror)
#include <unordered_map>  // g_hmsFiredMask
#include <vector>
#include <string>
#include <string_view>

namespace MFO::ProgAllocator {

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
        inline Economy g_econ;
        // The RECORD DEFAULTS latched at kDataLoaded (before any save loads, so
        // GLOB values are genuine record defaults, §10). g_econ is reset to this
        // and then re-overlaid from the addon's MCM INI on every re-apply — so
        // a save's stale GLOB runtime values NEVER re-enter the economy (the
        // 2026-08-17 perk-pool corruption: a repurposed GLOB's saved value read
        // floor(level/1) = double the perk pool).
        inline Economy g_econDefaults;

        inline bool           g_ready  = false;    // Detected + economy latched
        inline RE::TESGlobal* g_devCmd = nullptr;  // harness selector — read LIVE on
                                            // purpose (console `set` writes the
                                            // live value); dev-only, never a
                                            // gameplay input

        // ── the declared classes (§18.6 Stage 2 — N, not fixed 3) ───────────
        // Built once at Init from every registered manifest, in manifest ×
        // declaration order, then FROZEN (lock-free reads, the catalog
        // discipline). Identity = the class-def FLST FormID.
        inline std::vector<ClassDef> g_classes;

        // v1.1 GENERIC add-on manifest model (host-side, add-on-agnostic). Built
        // once at Init ALONGSIDE the progression parse above, then frozen. Parses
        // but is NOT yet consumed — the progression path still drives behavior;
        // later phases route consumers onto this general model. Exposed via
        // Progression::Manifests() (defined at the foot of ProgAllocator_Manifest.cpp).
        inline std::vector<Progression::AddonManifest> g_manifests;

        // ── the 18 skills (display + baseline; same set as the catalog) ─────
        struct SkillName { RE::ActorValue av; const char* name; };
        inline constexpr SkillName kSkillNames[] = {
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

        // ── HMS class-redistribution (§HMS, PRGN v5) ────────────────────────
        //
        // Pool index is FIXED {0=Health, 1=Magicka, 2=Stamina} — this order is
        // the co-save order and the profile-table column order. NEVER reorder.
        inline constexpr RE::ActorValue kHmsAV[3] = {
            RE::ActorValue::kHealth, RE::ActorValue::kMagicka, RE::ActorValue::kStamina
        };

        // §HMS off-class-usage mirror (F3). The REAL "off-class gambit fired"
        // signal: the combat scheduler (WORKER thread) publishes the pool a
        // FIRED combat gambit action exercised, into this per-follower mirror;
        // HmsTrackBattle (MAIN poll) consumes it. Mutex-guarded map — the same
        // cross-thread per-follower pattern as CombatStyle::g_owned — so it is
        // race-free without reading g_followers off-thread. Runtime-only, never
        // serialized. The stored byte is a BITMASK of pools fired since the last
        // consume: bit0=Health, bit1=Magicka, bit2=Stamina.
        inline std::mutex g_hmsFireMx;
        inline std::unordered_map<RE::FormID, std::uint8_t> g_hmsFiredMask;

        // ── cross-module helpers (declared here, defined next to their module) ──

        // ProgAllocator_Manifest.cpp — the live MCM INI overlay (main thread;
        // called by Init / OnPostLoad / OnMenuClose).
        void ApplyEconomyOverride();

        // ProgAllocator_Manifest.cpp — PRGN class-identity resolution, used by
        // the co-save block in ProgAllocator.cpp (CoSaveSave/CoSaveLoad).
        bool DeriveClassIdentity(RE::FormID a_clsId, std::string& a_plugin, RE::FormID& a_local);
        RE::TESForm* LookupAddonForm(RE::FormID a_local, std::string_view a_plugin);

        // ProgAllocator_Hms.cpp — §HMS class-redistribution, called from the
        // level poll / ReapplyFollower in ProgAllocator.cpp.
        void HmsTrackBattle(RE::Actor* a_actor, ProgState& a_st);
        void RecomputeHMS(RE::Actor* a_actor, ProgState& a_st, bool a_log, float a_grantBudget = 0.0f);
        // PRGN v8 one-time RETROACTIVE player-rate parity, called by CoSaveLoad
        // for a record written by an older (v<8) PRGN. Main thread (load).
        void HmsRetroParity(RE::FormID a_id, ProgState& a_st);

        // ── WAVE-1 SPLIT (2026-09-24): FILE-LOCAL -> SHARED, BODIES UNCHANGED ──────
        // The subsystem-folder split cut ProgAllocator.cpp by concern into
        // progression/{Allocator,SkillScale,PerkGate,BoardViews,Poll,Harness,Verbs}.cpp.
        // Each symbol below was file-local (anonymous namespace) and is now used from a
        // second TU, so it gained external linkage: functions are declared here and
        // defined where they were (unchanged), variables are `extern` here and defined
        // in the file that owns them, and the small helpers the compiler INLINED across
        // the new cut moved here whole as `inline` (so every TU can still inline them).
        // tools/splitcheck proves the generated code is unchanged (modulo per-TU
        // inlining drift, which it lists).
        //
        // Session state (defined in progression/Allocator.cpp; the level poll, the
        // harness and the PRGN co-save all read/write it):
        extern int           g_pollGen;
        extern int           g_pollFrames;
        extern std::uint16_t g_lastPlayerLevel;
        extern float         g_playerHmsTotalLast;
        inline constexpr int kPollFrames = 120;   // ~2s at 60fps
        inline constexpr std::uint16_t kMaxPerkAllocs    = 1024;

        // Small helpers (were anon in ProgAllocator.cpp; inlined at most call sites):
        inline const char* AvName(RE::ActorValue a_av) {
            for (const auto& s : kSkillNames)
                if (s.av == a_av) return s.name;
            return "?";
        }

        // Is this raw co-save ordinal one of the 18 skill AVs this module
        // ever writes? Ingestion defense (INVARIANTS #11): a garbage av fed
        // to Get/SetBaseActorValue would index the engine's AV array out of
        // bounds — validate the VALUE, not just the count.
        inline bool IsKnownSkillAv(std::uint32_t a_raw) {
            for (const auto& s : kSkillNames)
                if (static_cast<std::uint32_t>(s.av) == a_raw) return true;
            return false;
        }

        inline const char* NameOf(RE::TESForm* a_form) {
            if (!a_form) return "<none>";
            const char* n = a_form->GetName();
            return (n && *n) ? n : "<unnamed>";
        }

        // §17: ranks MFO has allocated — each one cost a point.
        inline int AllocatedRanks(const ProgState& a_st) {
            int total = 0;
            for (const auto& p : a_st.perks) total += p.rank;
            return total;
        }

        inline PerkAlloc* FindAlloc(ProgState& a_st, RE::FormID a_nodePerkID) {
            for (auto& p : a_st.perks)
                if (p.nodePerkID == a_nodePerkID) return &p;
            return nullptr;
        }

        // §16 manual pool — a PURE FUNCTION of serialized baselines, never an
        // incremental accumulator (the round-2 SEV-1 lesson: accumulators
        // drift under replay; a formula cannot). Rate: flat 5/level (marth).
        // Self-clamping: applied can never read as a negative pool.
        inline int ManualAvail(const ProgState& a_st) {
            if (!a_st.manualSkills || a_st.manualBaselineLevel == 0) return 0;
            const int lvls = std::max(0, static_cast<int>(a_st.progressionLevel) -
                                          static_cast<int>(a_st.manualBaselineLevel));
            const int accrued = lvls * g_econ.manualSkillPtsPerLevel;
            return std::max(0, accrued - static_cast<int>(a_st.manualPointsApplied));
        }

        inline RE::BGSPerk* PerkByID(RE::FormID a_id) {
            return a_id ? RE::TESForm::LookupByID<RE::BGSPerk>(a_id) : nullptr;
        }

        inline bool IsActiveFollower(RE::FormID a_id) {
            // Off-worker membership probe: g_activeIds is reassigned by Refresh on
            // the worker, so walk the lock-guarded FormID mirror instead of the
            // live list (SEV-1 cluster).
            return Followers::IsTrackedFast(a_id);
        }

    // §17: THE perk-point authority — derived, never stored. Idempotent
    // across reloads and level-ups by construction; clamped at 0 so a
    // heavily pre-trained follower is simply "ahead", never negative.
    inline int PerkPointsAvailable(const ProgState& a_st) {
        const int earned = static_cast<int>(a_st.progressionLevel) /
                           std::max(1, g_econ.levelsPerPerkPoint);
        // native tree perks NO LONGER subtracted (marth 2026-08-13): a follower's
        // starting perks are their build, not a debt to earn back. available = earned - spent.
        return std::max(0, earned - AllocatedRanks(a_st));
    }

        // The catalog index (progression/Allocator.cpp):
        struct NodeRef {
            const Progression::SkillTree*    tree = nullptr;
            const Progression::PerkNodeView* node = nullptr;
        };
        const std::unordered_map<RE::FormID, NodeRef>& NodeIndex();
        NodeRef FindNode(RE::FormID a_nodePerkID);
        bool OwnsAnyRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         const ProgState& a_st, RE::FormID a_nodePerkID);

        // Skill points (progression/SkillScale.cpp):
        std::vector<std::pair<RE::ActorValue, float>> WeightsFor(RE::Actor* a_actor,
                                                                 const ClassDef& a_def);
        void RecomputeSkills(RE::Actor* a_actor, ProgState& a_st, bool a_log);

        // Perks (progression/PerkGate.cpp):
        int CountNativeTreeRanks(RE::Actor* a_actor, RE::TESNPC* a_base);
        int StripNativePerks(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st);
        int RestoreNativePerksImpl(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st);
        int GateNextRank(RE::Actor* a_actor, RE::TESNPC* a_base,
                         ProgState& a_st, const Progression::PerkNodeView& a_node,
                         std::string& a_whyNot);
        bool GrantRank(RE::Actor* a_actor, RE::TESNPC* a_base, ProgState& a_st,
                       const Progression::PerkNodeView& a_node, int a_targetRank);
        void ReapplyFollower(RE::Actor* a_actor, ProgState& a_st);

        // Board-view state (defined in progression/BoardViews.cpp; the poll drives the
        // refresh, ClearAll drops the snapshot):
        extern std::mutex                           g_viewMx;
        extern std::shared_ptr<const BoardProgSnap> g_boardSnap;
        extern std::atomic<RE::FormID>              g_boardFocus;
        extern RE::FormID                           g_lastPublishedFocus;
        extern bool                                 g_boardWasOpen;
        extern int                                  g_viewFrames;

        // The level poll (progression/Poll.cpp) and the harness (progression/Harness.cpp):
        bool Unmanaged(RE::Actor* a_actor, const char* a_verb);
        void PollTick(int a_gen);
        const char* ClsName(RE::FormID a_id);

}
