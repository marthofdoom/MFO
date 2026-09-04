#pragma once
#include "PCH.h"

// Runtime form resolution against MFO.esp.
//
// The local ids here are a FROZEN contract with MFO_GenerateESP.py and are
// asserted by tools/audit_esp.py (INVARIANTS #41). Changing one orphans every
// save that has seen it.
//
// LOOKUP TRAP (INVARIANTS, and it cost MEO a 10,146-row table that resolved
// "0 live"): TESDataHandler::LookupForm<T> gates on form->Is(T::FORMTYPE).
// Abstract intermediates such as TESBoundObject inherit FormType::None, so it
// returns nullptr 100% of the time -- compiles clean, fails silently. The
// types below are all CONCRETE, so the template is safe here. If you ever add
// one that is not, use the non-template LookupForm + ->As<T>().

namespace MFO::Forms {

    inline constexpr const char* kPlugin = "MFO.esp";

    inline constexpr RE::FormID kFieldOrdersMGEF  = 0x800;
    inline constexpr RE::FormID kFieldOrdersSpell = 0x801;
    // 0x802 shipped in v0.1.0-v0.3.0 as the tutored-spell tag. Tutoring is
    // OUT OF SCOPE (DESIGN.md 5.4) but FormIDs are forever: the id stays
    // reserved and is never recycled, per INVARIANTS #41.
    inline constexpr RE::FormID kGrantedKeyword   = 0x802;   // RESERVED, unused
    // 0x803 RETIRED (v1.1): was the addon-manifest sentinel keyword (kAddonSentinel).
    // Add-ons now self-declare with their OWN keyword (edid suffix "_MFOAddonManifest"),
    // so no MFO.esp form is referenced and no MFO.esp master is needed (the Vortex
    // fix). Id stays retired, never recycled (INVARIANTS #41). Contract: Docs/ADDON-API.md.
    inline constexpr RE::FormID kStartupQuest     = 0x804;
    inline constexpr RE::FormID kMCMQuest         = 0x808;
    // M9 (DESIGN §4.5c): the actuation records.
    inline constexpr RE::FormID kCommandQuest     = 0x80A;
    inline constexpr RE::FormID kLootQuest        = 0x80C;   // Option A: travel-to-loot delivery
    inline constexpr RE::FormID kCastPackage      = 0x820;
    inline constexpr RE::FormID kTravelPackage    = 0x828;   // Option A: rides vanilla Travel (slot 0)
    // P7 multi-follower loot: one travel package per concurrent slot. Each is
    // byte-identical to slot 0's but names its OWN target alias (3/5/7). The
    // native never fills BY these ids (the alias fill delivers the package); they
    // are resolved ONLY for the WALK diagnostic (Forms::IsTravelPackage).
    inline constexpr RE::FormID kTravelPackage1   = 0x900;   // slot 1
    inline constexpr RE::FormID kTravelPackage2   = 0x901;   // slot 2
    inline constexpr RE::FormID kTravelPackage3   = 0x902;   // slot 3
    // RETREAT PROBE: travel-to-player under kIgnoreCombat -- can an alias
    // Travel package pull a follower away from a live combat controller?
    inline constexpr RE::FormID kRetreatQuest     = 0x830;
    inline constexpr RE::FormID kRetreatPackage   = 0x831;
    // #21 econ bridge: carries MFO_Trade (VMAD), dispatched by the DLL.
    inline constexpr RE::FormID kTradeQuest       = 0x80E;
    // P1 PROBE (bProbeCastStyle, default OFF): the caster-forward CSTY the
    // consent hook swaps onto a latched follower's LIVE CombatController
    // (per-combat instance, never the base record) to measure whether a
    // magic-inclined style makes his own AI cast more -- §0.28's melee-bias
    // lever, promoted to a gated experiment.
    inline constexpr RE::FormID kProbeCastStyle   = 0x832;
    // Stance-ownership CSTYs (bWeaponStyleControl, default ON). The DLL swaps
    // one onto a follower's LIVE CombatController when an equip gambit wins the
    // hand, so the engine stops re-drawing the weapon MFO didn't pick.
    inline constexpr RE::FormID kMeleeStyle       = 0x833;
    inline constexpr RE::FormID kRangedStyle      = 0x834;
    // FORCED SELF-CAST (Docs/SPEC-self-cast-forced.md): the dedicated no-QNAM
    // targType-6 self package, delivered by MFO_CommandQuest's own alias 2.
    // §0.22 proved probe 6's t6+no-QNAM self-cast casts cleanly (REVOKED #67);
    // the shipped kCastPackage carries a QNAM (foe target t4 -> alias 1), so it
    // cannot serve self without writing t6 into a QNAM-carrying record at
    // runtime -- the rev-4 crash cell. Route is DLL-gated (bCastSelf) pending a
    // deck-confirmed production run.
    inline constexpr RE::FormID kCastPackageSelf  = 0x835;
    // APMF LOOT-TRAVEL (ch.9 0x49 route, APMFBridge::OfferPackage): ONE package
    // per concurrent loot slot (kMaxLootSlots), mirroring kTravelPackage{,1,2,3}'s
    // per-slot shape but with a RUNTIME-HANDLE Location input (PLDT type 0, "Near
    // Reference") instead of an alias -- APMF's 0x49 hook delivers the package
    // directly to the claimed actor, so there is no alias to carry the target;
    // Packages.cpp overwrites PackageLocation::data.refHandle at runtime before
    // claiming. No QNAM (no alias-valued input). See MFO_GenerateESP.py
    // make_apmf_loot_travel_package().
    inline constexpr RE::FormID kAPMFLootTravelPackage0 = 0x836;
    inline constexpr RE::FormID kAPMFLootTravelPackage1 = 0x837;
    inline constexpr RE::FormID kAPMFLootTravelPackage2 = 0x838;
    inline constexpr RE::FormID kAPMFLootTravelPackage3 = 0x839;
    // APMF RETREAT (ch.9 0x49 route): the flee-to-player counterpart of
    // kAPMFLootTravelPackage0-3, but ONE record -- retreat's destination is
    // ALWAYS the player, so there is no per-follower runtime-target collision
    // the way loot's per-corpse case needs 4 slots to avoid. See
    // MFO_GenerateESP.py make_apmf_retreat_package() / native/Packages.cpp
    // RetreatFill.
    inline constexpr RE::FormID kAPMFRetreatPackage = 0x83A;

    inline RE::SpellItem*  g_fieldOrders  = nullptr;
    inline RE::BGSKeyword* g_grantedKywd  = nullptr;

    // M9. Resolved so the first questions about these records are answered at
    // LOAD, in the log, before any behaviour depends on them: do they exist,
    // did the quest start, and does the package still ride a vanilla template
    // after the ESP round-trip?
    inline RE::TESQuest*   g_commandQuest = nullptr;
    inline RE::TESPackage* g_castPackage  = nullptr;
    // FORCED SELF-CAST: the t6/no-QNAM self package on command-quest alias 2. A
    // miss (old ESP) disables ONLY the self route -- Packages::Begin's self
    // branch declines NoRecord and the caller falls back, never a crash.
    inline RE::TESPackage* g_castPackageSelf = nullptr;
    inline RE::TESQuest*   g_lootQuest    = nullptr;   // Option A
    inline RE::TESPackage* g_travelPackage  = nullptr; // Option A (slot 0)
    inline RE::TESPackage* g_travelPackage1 = nullptr; // P7 slot 1
    inline RE::TESPackage* g_travelPackage2 = nullptr; // P7 slot 2
    inline RE::TESPackage* g_travelPackage3 = nullptr; // P7 slot 3
    // APMF loot-travel (ch.9 0x49 route), one per slot -- see kAPMFLootTravelPackage0-3.
    inline RE::TESPackage* g_apmfLootTravelPackage0 = nullptr;
    inline RE::TESPackage* g_apmfLootTravelPackage1 = nullptr;
    inline RE::TESPackage* g_apmfLootTravelPackage2 = nullptr;
    inline RE::TESPackage* g_apmfLootTravelPackage3 = nullptr;

    // Per-slot accessor for the APMF loot-travel packages (Packages.cpp). nullptr
    // for an out-of-range slot or an unresolved record.
    inline RE::TESPackage* APMFLootTravelPackage(int a_slot) {
        switch (a_slot) {
        case 0: return g_apmfLootTravelPackage0;
        case 1: return g_apmfLootTravelPackage1;
        case 2: return g_apmfLootTravelPackage2;
        case 3: return g_apmfLootTravelPackage3;
        default: return nullptr;
        }
    }

    // WALK-diagnostic predicate: is a_pkg ANY of the (up to 8, 4 alias + 4 APMF)
    // loot-travel packages? A slot-k follower rides slot k's package, so the
    // single-package comparison would read false for other slots and mislead.
    // Also load-bearing for the PACKAGE-THEFT guard (Logistics.cpp): a follower
    // legitimately running his APMF-delivered package must read as "on the travel
    // package", never as "stolen". Unresolved extras compare as nullptr (safe).
    inline bool IsTravelPackage(const RE::TESPackage* a_pkg) {
        return a_pkg && (a_pkg == g_travelPackage  || a_pkg == g_travelPackage1 ||
                         a_pkg == g_travelPackage2 || a_pkg == g_travelPackage3 ||
                         a_pkg == g_apmfLootTravelPackage0 || a_pkg == g_apmfLootTravelPackage1 ||
                         a_pkg == g_apmfLootTravelPackage2 || a_pkg == g_apmfLootTravelPackage3);
    }
    inline RE::TESQuest*   g_retreatQuest   = nullptr; // RETREAT PROBE
    inline RE::TESPackage* g_retreatPackage = nullptr; // RETREAT PROBE
    // APMF retreat (ch.9 0x49 route) -- see kAPMFRetreatPackage.
    inline RE::TESPackage* g_apmfRetreatPackage = nullptr;

    // WALK-diagnostic predicate: is a_pkg either retreat package (legacy alias
    // or APMF-routed)? Mirrors IsTravelPackage's role for loot -- a follower
    // legitimately running his APMF-delivered retreat package must read as
    // "on the retreat package", not as some unrelated package. Unresolved
    // g_apmfRetreatPackage compares as nullptr (safe).
    inline bool IsRetreatPackage(const RE::TESPackage* a_pkg) {
        return a_pkg && (a_pkg == g_retreatPackage || a_pkg == g_apmfRetreatPackage);
    }
    inline RE::TESQuest*   g_tradeQuest     = nullptr; // #21 econ bridge
    // P1 probe. Resolved at kDataLoaded (main thread) so the combat-thread
    // consent hook only ever READS a settled pointer -- it must never run a
    // data-handler lookup itself. TESCombatStyle is concrete, so the template
    // Look<> is safe (see the LOOKUP TRAP note above).
    inline RE::TESCombatStyle* g_probeCastStyle = nullptr;
    // Stance-ownership CSTYs. Resolved at kDataLoaded (main thread) so the
    // combat-thread apply only ever READS settled pointers. A miss disables
    // that one stance with a named log line -- never a crash.
    inline RE::TESCombatStyle* g_meleeStyle  = nullptr;
    inline RE::TESCombatStyle* g_rangedStyle = nullptr;
    // The Cast stance's pure-mage style. Reuses the existing MFO_CastStyle CSTY
    // (0x832) -- magic-dominant, melee/ranged starved -- the same record the P1
    // probe swaps; here it is a shipped feature driven by cast gambits.
    inline RE::TESCombatStyle* g_castStyle   = nullptr;

    // Resolve at kDataLoaded. Returns false if anything required is missing.
    // A missing form disables ONE feature with a named log line -- never a
    // hard requirement, never a crash.
    bool Resolve();

    // Idempotent. Called on kPostLoadGame and kNewGame, never kDataLoaded:
    // it must run AFTER the co-save has loaded (ARCHITECTURE.md §9).
    void EnsurePlayerSetup();

}
