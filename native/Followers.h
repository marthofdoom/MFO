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
// IsPlayerTeammate() is necessary-ish but NOT sufficient: Inigo never sets it,
// Vilja and Tindra carry their own dismissal factions, pet frameworks use
// their own. The quirk table (data/follower_quirks.json, compiled in) covers
// them and resolves against the live load order.

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

    // Ensure a co-save record exists for this actor. Dismissed followers keep
    // their record -- membership here is independent of active status.
    FollowerState& EnsureRecord(RE::FormID a_actorID);

    // Is this actor currently one of ours? Reads THE SUBJECT's state.
    bool IsTracked(RE::FormID a_actorID);

}
