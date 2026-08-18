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

    // Parallel id list. g_active holds handles, and a handle that fails to
    // resolve cannot tell us WHO it was -- so a follower whose handle briefly
    // nulls would leave the active set with no miss streak and no log line,
    // bypassing the hysteresis entirely (F6).
    inline std::vector<RE::FormID> g_activeIds;

    // Resolve quirk-table forms at kDataLoaded. Absent plugins are NORMAL and
    // are logged at debug level, never as errors.
    void ResolveQuirks();

    // Save-scoped transient state. RevertCallback must clear it: the streak map
    // is keyed by FormID and the same NPC has the same FormID in every save, so
    // a streak carried from save A applies to save B (F1).
    void ClearTransientState();

    // How many quirk entries resolved against this load order. Reported in the
    // Field Kit -- a diagnostic that always said 0/0 is worse than none.
    int QuirksActive();
    int QuirksInactive();

    // Reads THE SUBJECT ACTOR's state (not the player's).
    bool IsEligibleFollower(RE::Actor* a_actor);

    // Reads THE SUBJECT ACTOR's state. True when a custom follower signals
    // dismissal in its own way despite still looking like a teammate.
    bool IsDismissedCustomFollower(RE::Actor* a_actor);

    // Rebuild g_active from ProcessLists::highActorHandles. Runs on the AddTask
    // JOB WORKER (Diagnostics' sleeper tick), NOT the main thread -- despite the
    // "serial SKSE-task" doctrine, the pump body executes on BSJobs::JobThread
    // (ENGINE_NOTES §0.37). Every reassignment of g_active/g_activeIds here can
    // therefore race an off-worker READER; that is why Refresh republishes the
    // g_mx-guarded FormID mirror + immutable snapshot at its tail, and why
    // off-worker callers MUST use IsTrackedFast / ActiveSnapshot below rather
    // than walking the live lists. Logs additions and removals by name; logs the
    // zero case too, or "found none" and "never ran" are indistinguishable
    // (INVARIANTS #46).
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

    // Lay down the base gambit kit (2 combat, 4 logistics). Called on record
    // creation and as a load-time backfill for a pre-defaults save whose board
    // is entirely empty. See the definition for the full contract.
    void ApplyDefaultKit(FollowerState& st);

    // ── item 2b: main-thread board-edit INSERT tripwire ──────────────────────
    // g_followers mutation belongs to the serial worker/main pump (#4). A Board
    // Prog edit (SetClass/Respec) runs on the true MAIN thread via
    // MainThread::Post and calls TryEnsureRecord, which would INSERT (rehash =
    // UB) if the target record did not already exist while the worker iterates
    // the map. It is proven safe because every board-addressable follower is
    // active/retained and therefore already has a record (Refresh TryEnsureRecord'd
    // it, or it is a retained entry). This RAII scope makes that assumption
    // ASSERTABLE: while one is live, any TryEnsureRecord that actually CREATES a
    // record logs a loud hazard line (the invariant was violated). Wrap the
    // MainThread::Post prog-edit body in a BoardEditScope. Nesting-safe (counter).
    struct BoardEditScope {
        BoardEditScope();
        ~BoardEditScope();
        BoardEditScope(const BoardEditScope&)            = delete;
        BoardEditScope& operator=(const BoardEditScope&) = delete;
    };

    // Is this actor currently one of ours? Resolves handles by walking the live
    // g_active vector -- UNLOCKED, so MAIN-THREAD / JOB-WORKER callers ONLY (the
    // serial pump domain). NEVER call this from the combat thread or an event
    // sink: Refresh reassigns/reallocates g_active on the worker and an
    // off-domain walk is a use-after-free (SEV-1, CasterConsent's old bug). For
    // any off-worker caller use IsTrackedFast instead.
    bool IsTracked(RE::FormID a_actorID);

    // Off-worker membership probe: locks g_mx and tests the FormID mirror that
    // Refresh republishes under the same lock. Resolves no handles, touches no
    // live list -- safe to call from ANY thread (combat cast-hook, event sinks,
    // SaveCallback). This is the road CLAUDE.md's "combat-thread hooks read
    // FormIDs/atomic mirrors, never the follower lists" describes; it REPLACES
    // the unlocked IsTracked walk for every off-worker caller. Membership only:
    // it cannot say WHICH handle, only that the id is currently tracked.
    bool IsTrackedFast(RE::FormID a_actorID);

    // An immutable copy of the active FormID list, published atomically from
    // Refresh under g_mx. Off-worker readers (progression's main-thread poll,
    // any sink) hold the returned shared_ptr and iterate it lock-free instead of
    // walking live g_active/g_activeIds while the worker rebuilds them. The
    // pointee never mutates; a later Refresh swaps in a NEW vector, leaving
    // outstanding readers on their stable copy. Safe to call from ANY thread.
    std::shared_ptr<const std::vector<RE::FormID>> ActiveSnapshot();

    // Seconds since this follower was last OBSERVED in combat, or a huge value
    // if never. Rapport needs this because combat state is already gone by the
    // time a death event's queued task runs: killing the last enemy ENDS the
    // fight, so a follower who fought the whole battle reads IsInCombat()==false
    // at the only moment we get to ask (#51).
    float SecondsSinceCombat(RE::FormID a_actorID);

}
