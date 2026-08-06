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

    inline RE::SpellItem*  g_fieldOrders  = nullptr;
    inline RE::BGSKeyword* g_grantedKywd  = nullptr;

    // M9. Resolved so the first questions about these records are answered at
    // LOAD, in the log, before any behaviour depends on them: do they exist,
    // did the quest start, and does the package still ride a vanilla template
    // after the ESP round-trip?
    inline RE::TESQuest*   g_commandQuest = nullptr;
    inline RE::TESPackage* g_castPackage  = nullptr;
    inline RE::TESQuest*   g_lootQuest    = nullptr;   // Option A
    inline RE::TESPackage* g_travelPackage  = nullptr; // Option A (slot 0)
    inline RE::TESPackage* g_travelPackage1 = nullptr; // P7 slot 1
    inline RE::TESPackage* g_travelPackage2 = nullptr; // P7 slot 2
    inline RE::TESPackage* g_travelPackage3 = nullptr; // P7 slot 3

    // WALK-diagnostic predicate: is a_pkg ANY of the (up to 4) loot-travel
    // packages? A slot-k follower rides slot k's package, so the single-package
    // comparison would read false for slots 1-3 and mislead. Never gates
    // behaviour -- diagnostic only. Unresolved extras compare as nullptr (safe).
    inline bool IsTravelPackage(const RE::TESPackage* a_pkg) {
        return a_pkg && (a_pkg == g_travelPackage  || a_pkg == g_travelPackage1 ||
                         a_pkg == g_travelPackage2 || a_pkg == g_travelPackage3);
    }
    inline RE::TESQuest*   g_retreatQuest   = nullptr; // RETREAT PROBE
    inline RE::TESPackage* g_retreatPackage = nullptr; // RETREAT PROBE
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

    // Resolve at kDataLoaded. Returns false if anything required is missing.
    // A missing form disables ONE feature with a named log line -- never a
    // hard requirement, never a crash.
    bool Resolve();

    // Idempotent. Called on kPostLoadGame and kNewGame, never kDataLoaded:
    // it must run AFTER the co-save has loaded (ARCHITECTURE.md §9).
    void EnsurePlayerSetup();

}
