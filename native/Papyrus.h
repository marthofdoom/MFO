#pragma once
#include "PCH.h"

// VM DISPATCH — reaching Papyrus-only natives from C++.
//
// DESIGN.md §4.5aa named this "the binding gap": the Tier-B vocabulary MFO
// needs is declared native in Actor.psc and has NO C++ binding in
// CommonLibSSE-NG. Confirmed in Skyrim's own Actor.psc:
//
//     Function DoCombatSpellApply(Spell akSpell, ObjectReference akTarget) native
//     Function KeepOffsetFromActor(Actor arTarget, float x, y, z, ...)     native
//     Function ClearKeepOffsetFromActor()                                  native
//     Function SetDontMove(bool abDontMove = true)                         native
//     Function EquipSpell(Spell akSpell, int aiSource)                     native
//
// These are not four problems. They are one door with four things behind it,
// and this module is the door.
//
// WHY THIS AND NOT RELOCATIONS: a sourced address is faster and synchronous,
// but costs a reverse-engineered offset per function, per game version, and
// StartCombat took that route and DID NOT WORK IN THE FIELD (ENGINE_NOTES
// §0.12). VM dispatch needs no offsets at all -- the VM resolves by name
// against script the game already loaded -- so it covers every function above
// at once and cannot drift with a game update.
//
// WHY MFO CANNOT JUST STEER THE AI INSTEAD (ruled, marth): gambits choose
// TARGETS. "Attack -> lowest HP", "Heal -> player" -- the target IS the rule.
// An AI picking its own target is not running the player's list, so commanding
// is not a preference here, it is the design.
//
// COST, stated honestly: dispatch is ASYNCHRONOUS. The call is queued to the VM
// and runs on its own schedule, so the cast lands a frame or two after the tick
// that chose it. Against §4.1's 133 ms budget that is acceptable -- it is well
// inside the 4-frame human reaction deadline -- but it means MFO can never
// treat "dispatched" as "happened".

namespace MFO::Papyrus {

    // Is the VM reachable? False means every call below is a no-op, and the
    // caller must fall back rather than assume success.
    bool Available();

    // Actor.DoCombatSpellApply(spell, target) -- the commanded cast. The actor
    // casts it AS A COMBAT ACTION at a named target, which is the whole reason
    // this module exists.
    //
    // Returns whether the call was DISPATCHED, not whether the spell fired.
    bool DoCombatSpellApply(RE::Actor* a_caster, RE::SpellItem* a_spell,
                            RE::TESObjectREFR* a_target);

    // How many dispatches this session, for the heartbeat (#53). A subsystem
    // whose correct behaviour is invisible must publish one.
    std::uint32_t Dispatches();
    std::uint32_t Failures();
    void ClearTransientState();

}
