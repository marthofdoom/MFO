#pragma once
#include "PCH.h"

// PRECISION BRIDGE (IAUS card I6, marth Q64: "Almost every list runs precision, the correct answer is within
// precision reach"). The MINIMAL read-only bridge to Precision (ersh1, Nexus 72347): one query, the length of an
// actor's weapon attack-collision capsule.
//
// DETECTED AT RUNTIME, NEVER ASSUMED: GetModuleHandle("Precision.dll") + its exported RequestPluginAPI. No plugin
// name, no load order, no list data. Absent / refused = 0, and the caller keeps fMeleeReach.
//
// NO PRECISION CODE IS COPIED (it is GPL-3.0-or-later with a modding exception, not MIT-compatible). This file
// declares nothing of Precision's: it calls ONE vtable slot by index, with the ABI read from the shipped binary.
//
// WHAT IS VERIFIED, AND AGAINST WHAT (Docs/research/2026-11/iaus-i6.md "Precision reach" has the disassembly):
//   - Precision 2.0.4, Precision.dll sha256 b4ab4a5a3673324b222b59beed1378abe7982f85af5248d2a7225bc46d0c4f32 (the
//     one build in LoreRim, Tuxborn and the custom list), and its published header (PrecisionAPI.h, branch main).
//   - export RequestPluginAPI(uint8 version): 0..3 (V1..V4) return the API singleton, anything else nullptr.
//   - IVPrecision1 slot 8 = GetAttackCollisionCapsuleLength: float(this, ActorHandle BY VALUE in edx, uint8 type in
//     r8b), result in xmm0. Slots 0-7 are the four Add / four Remove callback registrations.
//   - type 2 = RightWeapon: "the length of the current right weapon collision if it exists, or calculate it".
//   - the implementation resolves the handle and walks the actor's 3D for its WEAPON node: MAIN THREAD ONLY here.
//     With Precision's own bDisableMod set, or a computed length of 0, it returns the ENGINE's reach function
//     (Address Library 37588 / 38538) instead.
//
// SEAT SELF-CHECK (memory seat-verify-or-refuse): the bridge asks for interface V2. A Precision that grants V2
// has IVPrecision1 complete (an interface is append-only once a later one derives from it), so slot 8 is the
// query on every build that answers. The returned object's vtable and its slot 8 must both lie inside
// Precision.dll's own image, or the bridge REFUSES for the session with one WARN. It never guesses.
//
// THREADING: WeaponLength is MAIN THREAD ONLY (iaus/SpellBook.cpp Measure calls it). Resolve happens on first
// use, also on the main thread.
namespace MFO::PrecisionBridge {

    // MAIN THREAD. The capsule length of a_actor's right-hand weapon in world units; 0 when Precision is not
    // loaded, the bridge refused itself, the actor is null, or Precision returned a non-finite / non-positive value.
    [[nodiscard]] float RightWeaponLength(RE::Actor* a_actor);

    // MAIN THREAD. "absent" / "live" / the refusal reason, for the probe line.
    [[nodiscard]] const char* State();

}
