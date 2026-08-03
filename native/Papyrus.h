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

    // ObjectReference.Activate(akActionRef, abDefaultProcessingOnly=false) --
    // the loose-item ACQUIRE PROBE (route 2b). Dispatched on the REF's handle
    // with the follower as the activator: the ENGINE runs the pickup on its own
    // scheduling, which is the whole point -- no PickUpObject and no SKSE
    // AddTask from the worker tick (§0.30 / INVARIANTS #72).
    //
    // Returns whether the call was DISPATCHED, not whether the item moved; the
    // caller must observe the ref/inventory on a later tick.
    bool DispatchActivate(RE::TESObjectREFR* a_ref, RE::TESObjectREFR* a_activator);

    // MFO_Trade.RunTrade(token) -- the #21 econ bridge trigger. Unlike the two
    // calls above (vanilla-class natives reached by class name), this targets a
    // CUSTOM script class attached to a_quest's VMAD, so the quest MUST be
    // running with MFO_Trade bound (it is: start-game-enabled + SEQ). The script
    // pulls the follower/vendor/lists back through MFO-registered natives keyed
    // by token. Returns whether the call was DISPATCHED (async, like the others).
    bool DispatchTradeRun(RE::TESForm* a_quest, std::int32_t a_token);

    // Actor.KeepOffsetFromActor(target, 0, -dist, 0, ...) -- makes the follower try
    // to hold position `dist` BEHIND the foe, which reads as keep-distance (small
    // dist) or flee (large dist). ClearKeepOffset releases it. EXPERIMENTAL (#35):
    // whether the combat controller honours a movement override is unproven.
    bool KeepOffset(RE::Actor* a_follower, RE::Actor* a_target, float a_dist);
    bool ClearKeepOffset(RE::Actor* a_follower);

    // How many dispatches this session, for the heartbeat (#53). A subsystem
    // whose correct behaviour is invisible must publish one.
    std::uint32_t Dispatches();
    std::uint32_t Failures();
    void ClearTransientState();

}
