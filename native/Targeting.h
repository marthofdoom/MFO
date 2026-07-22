#pragma once
#include "PCH.h"

// THE ATTACK VERB. ENGINE_NOTES §0.14, DESIGN.md §4.7.
//
// Papyrus cannot express commanded targeting AT ALL -- there is no combat-target
// setter in Actor.psc, SKSE, po3 or PapyrusUtil, only StartCombat/StopCombat and
// a GETTER. The target lives in engine-internal data. That is why a survey of
// ten modlists found zero Papyrus attack commands: impossibility, not fashion.
//
// The mechanism, and the reference implementation is open source and installed
// on this machine (LoreRim ships `Aggro Management in Skyrim` /
// SmartNPCTargetSelector.dll, https://github.com/RedyellowUnit/SmartTargetingNPC):
// hook `Character`'s UpdateCombat vfunc, and after the original runs, write the
// chosen target into BOTH `currentCombatTarget` and `combatController->targetHandle`.
//
// WHY A HOOK AND NOT A PLAIN WRITE: the engine re-picks its target constantly.
// A write from MFO's 7.5 Hz tick would drift in the gaps between ticks. The
// hook re-asserts at the ENGINE's own cadence. This also finally answers §4.7 --
// retention is a property of the MECHANISM, not of the target -- and it does so
// without any StopCombat/StartCombat churn (#22a): re-asserting is a handle
// compare-and-write, nothing more.
//
// SCOPE, deliberately narrow: MFO writes ONLY for a follower with an active
// gambit latch. Every other actor in the game passes straight through. That
// keeps the blast radius to "followers under player orders", which is the only
// defensible half of the split with a target-selection mod also installed.

namespace MFO::Targeting {

    // Install the vfunc hook. Once, at kDataLoaded. Safe to call when the
    // feature is disabled -- it simply does not install.
    void InstallHook();

    // Latch a follower onto a target. MAIN THREAD. Held until cleared, and
    // re-asserted by the hook every combat update.
    void Command(RE::FormID a_follower, RE::ActorHandle a_target);
    void Clear(RE::FormID a_follower);
    void ClearAll();

    bool IsHooked();

    // Probe instrumentation (§0.14's decisive measurement).
    struct Stats {
        std::uint32_t asserts   = 0;   // times we wrote our target
        std::uint32_t drifts    = 0;   // times the engine had re-picked before we did
        std::uint32_t passes    = 0;   // hooked updates we deliberately left alone
        std::uint32_t latched   = 0;   // followers currently latched
        bool conflictMod = false;      // SmartNPCTargetSelector.dll also loaded
    };
    Stats GetStats();

}
