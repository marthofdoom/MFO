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

    inline RE::SpellItem*  g_fieldOrders  = nullptr;
    inline RE::BGSKeyword* g_grantedKywd  = nullptr;

    // Resolve at kDataLoaded. Returns false if anything required is missing.
    // A missing form disables ONE feature with a named log line -- never a
    // hard requirement, never a crash.
    bool Resolve();

    // Idempotent. Called on kPostLoadGame and kNewGame, never kDataLoaded:
    // it must run AFTER the co-save has loaded (ARCHITECTURE.md §9).
    void EnsurePlayerSetup();

}
