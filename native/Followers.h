#pragma once
#include "PCH.h"
#include "State.h"

// Follower detection. DESIGN.md 3.1.
//
// THE RULE THAT SHAPES THIS FILE (INVARIANTS #14, MEO's worst 1.0.6 blocker):
// every question here is a state scan against a NON-PLAYER actor. Each helper
// names whose state it reads. A nullptr subject is an error, never a wildcard,
// and nothing defaults to the player.
//
// IsPlayerTeammate() is NECESSARY but NOT SUFFICIENT.
//
// Precise statement, because an earlier draft got this wrong: Inigo DOES set
// PlayerTeammate while following. What he does not do is CLEAR it on
// dismissal -- he signals that with WaitingForPlayer == -1 instead. So the
// teammate flag alone reports him as an active follower forever after you
// dismiss him. Vilja and Tindra likewise stay teammates and signal dismissal
// through their own factions.
//
// Hence the shape here, copied from Swiftly Order Squad's shipped IsFollower
// / IsDismissedCustomFollower: teammate is the cheap GATE, and the quirk
// table REVOKES eligibility for followers whose dismissal the flag misses.
// The one case that would need an inclusion path instead is a pet framework
// (faction rank grants eligibility without teammate status) -- carried in the
// quirk data but disabled until its real FormID is confirmed on a list that
// ships it.

namespace MFO::Followers {

    // Handles only -- never a raw Actor*. Followers cross cells, get
    // dismissed, and die mid-tick (INVARIANTS #2).
    inline std::vector<RE::ActorHandle> g_active;

    // Resolve quirk-table forms at kDataLoaded. Absent plugins are NORMAL and
    // are logged at debug level, never as errors.
    void ResolveQuirks();

    // Reads THE SUBJECT ACTOR's state (not the player's).
    bool IsEligibleFollower(RE::Actor* a_actor);

    // Reads THE SUBJECT ACTOR's state. True when a custom follower signals
    // dismissal in its own way despite still looking like a teammate.
    bool IsDismissedCustomFollower(RE::Actor* a_actor);

    // Rebuild g_active from ProcessLists::highActorHandles. Main thread only.
    // Logs additions and removals by name; logs the zero case too, or
    // "found none" and "never ran" are indistinguishable (INVARIANTS #46).
    void Refresh();

    // False for runtime (0xFF) FormIDs, which must NEVER be persisted
    // (INVARIANTS #9). IsCommandedActor() does not cover this: a spawned or
    // cloned teammate is not commanded but still carries a 0xFF id, and
    // ResolveFormID passes 0xFF ids through as resolved so the load side
    // cannot catch it either.
    bool IsPersistableID(RE::FormID a_actorID);

    // Ensure a co-save record exists for this actor. Dismissed followers keep
    // their record -- membership here is independent of active status.
    // Returns nullptr and logs for a non-persistable id.
    FollowerState* TryEnsureRecord(RE::FormID a_actorID);
    FollowerState& EnsureRecord(RE::FormID a_actorID);

    // Is this actor currently one of ours? Reads THE SUBJECT's state.
    bool IsTracked(RE::FormID a_actorID);

}
