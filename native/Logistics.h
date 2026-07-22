#pragma once
#include "PCH.h"
#include "State.h"

// The logistics table's actuation. DESIGN.md §4.8.
//
// A follower's SECOND gambit table (State::logistics() == tables[1]) governs
// upkeep -- drinking potions, looting arrows/potions/gear -- and runs on the
// OUT-OF-COMBAT idle tick. §4.8: the combat and logistics tables NEVER
// interleave, so the Scheduler runs this exactly when the follower is not in
// combat, one action per tick.
//
// TWO KINDS of thing live here:
//   * PURE READS used by the evaluator to answer the supply conditions
//     (CountPotions, ArrowCount, PotionRestores). No mutation.
//   * ACTUATION (ServiceFollower and the sink) that mutates the world, main
//     thread only, behind bLogistics.
//
// TRANSIENT-STATE DISCIPLINE (like Loadout): every map here describes live
// engine or session state, is main-thread-only, takes no lock, and is cleared
// on revert (ClearTransientState). None of it is serialized -- the loot LRU is
// deliberately not persisted (#22h: worst case after a load is one more delay
// on an already-picked corpse, not worth a growing save record).

namespace MFO::Logistics {

    // ── pure reads (the evaluator's supply conditions) ──────────────────────

    // Which resource a potion RESTORES, or kNone if it is not a plain H/S/M
    // restorative. Classified by MGEF ARCHETYPE (kValueModifier /
    // kDualValueModifier) on the costliest effect, never by name or by the
    // effect's target actor-value alone -- the portable classifier from MRO's
    // Requiem note (DESIGN §3.3/§4.8.2). Poisons and fortify/cure potions
    // return kNone by construction. Reads only the POTION, no actor.
    RE::ActorValue PotionRestores(RE::AlchemyItem* a_potion);

    // Count of H/S/M restore potions of a_which resource in a_follower's OWN
    // inventory (#14 -- the named follower, never the player). Walks the
    // inventory, so it is a ~1 s logistics-tick read, not a combat one.
    int CountPotions(RE::Actor* a_follower, RE::ActorValue a_which);

    // How many arrows/bolts a_follower carries matching their EQUIPPED bow or
    // crossbow. Returns -1 when no bow/crossbow is equipped, so the "out of
    // arrows" rule reads as N/A rather than true (a melee follower is never
    // "out of arrows"). Reads the named follower only.
    int ArrowCount(RE::Actor* a_follower);

    // ── actuation ───────────────────────────────────────────────────────────

    // Run one out-of-combat logistics tick for a_follower: cadence-gated
    // internally (~1 s, §4.8's idle rate -- NOT the 133 ms combat deadline),
    // then evaluate the logistics table and fire at most one action. No-op and
    // cheap when bLogistics is off or the follower is not yet due. Main thread
    // only; a_state is the follower's live record.
    void ServiceFollower(RE::Actor* a_follower, const FollowerState& a_state);

    // The player-looted WAIVER sink (#22h). A TESContainerChangedEvent filtered
    // to items entering the player collapses the first-dibs delay on the ref
    // they came from. Registered once at kDataLoaded, after form resolution.
    void RegisterSinks();

    // Save-scoped: the cadence clocks and the two loot LRUs. RevertCallback
    // clears them -- they describe a live session, and a stale entry keyed by
    // FormID would apply the previous save's timers to this one.
    void ClearTransientState();

}
