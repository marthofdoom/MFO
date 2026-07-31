#pragma once
#include "PCH.h"

// GAIT (task 16, ENGINE_NOTES §0.35). The walk-to-loot travel package's
// preferredSpeed (PKDT byte 6) picks the follower's gait for the whole leg:
// 0=Walk, 1=Jog, 2=Run, 3=FastWalk. MEASURED, not lore: v0.8.2 shipped byte 6
// = Run with flags 0x0 and the follower WALKED (~89 u/s); the byte is honored
// ONLY under general flag 0x2000 (Preferred Speed enable), which the ESP has
// shipped since v0.8.3.
//
// Mechanism: a one-byte write on MFO's OWN TESPackage record (MFO_TravelPackage,
// Forms::g_travelPackage). PACK form data is not serialized into the save --
// the in-memory record lives for the session and every session re-applies from
// config at kDataLoaded -- so this is the lowest-risk class of engine write MFO
// makes: no hook, no alias, no per-tick work, nothing outlives the session.

namespace MFO::Gait {

    // Copy Config::g_travelGait onto the travel package's preferredSpeed, and
    // (defensively) assert the 0x2000 enable flag the byte is inert without.
    // Safe before Forms::Resolve (no-ops on a null package) and safe to call
    // repeatedly. Called from the end of Config::Read() -- config -> engine
    // record is derived state, and Read()'s callers (kDataLoaded, MCM close)
    // are exactly the moments it must refresh -- plus once from kDataLoaded
    // AFTER Forms::Resolve, because the first Read() runs before the package
    // exists.
    void Apply();

}
