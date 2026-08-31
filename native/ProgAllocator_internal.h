#pragma once
// ProgAllocator_internal.h -- the ProgAllocator family's SHARED substrate. One
// TU (ProgAllocator.cpp) used to hold all of this in its anonymous namespaces;
// the mechanical module split (ProgAllocator.cpp / _Hms / _Manifest) moved the
// cross-module state, types, and constants here as `inline` (ONE shared
// instance across the three TUs -- never per-TU copies), and declares the big
// cross-module helpers next to the module that defines them. Single-module
// helpers stay file-local in their module. NOT a public API: only the three
// ProgAllocator*.cpp TUs may include this.

#include "PCH.h"
#include "ProgAllocator.h"
#include "Progression.h"
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

}
