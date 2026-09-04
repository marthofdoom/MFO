#pragma once
#include "PCH.h"
#include "CasterConsent.h"   // SpellKind

// ─────────────────────────────────────────────────────────────────────────────
// ComposedCast -- the Composed Forced Cast (CFC) executor.
//
// GOAL (Docs/SPEC-FORCED-CAST.md). Make a follower cast a spell his AI will NOT
// choose (the canonical case: a heal at an ally / the player) such that it is a
// REAL, ANIMATED cast, lands on the chosen target, the follower's MOVEMENT stays
// his own, it is BOUNDED, and a heal NEVER silently vanishes. It composes five
// things -- a delivery-flipped proxy, an APMF cast-execution claim (deny the AI's
// own casting/re-arm, keep movement), a CastBounds registration (so MFO's own
// consent gate recognizes the cast as bounded, §2), the hand arm, and the TRIGGER
// -- then observes, bounds, restores, and DEGRADES to today's kInstant apply on
// any failure. MOVEMENT is never claimed.
//
// THE TRIGGER (steering 2026-09-04, marth-approved): OBSERVE-AND-REPLICATE.
// The spec's originally-guessed trigger (a hand-built TESActionData::Process with
// ActionRightAttack/Release) is NOT used -- it was unproven and version-fragile.
// Instead a parallel APMF passive observer at the 0xAD seat will capture the
// engine's REAL full-animation NPC cast sequence from a deck cycle (the
// MagicCaster state machine + the animation-graph cast events + the charge/fire
// boundary). MFO will then replicate THAT proven sequence in the ONE isolated
// seam `DriveObservedCast` (ComposedCast.cpp). Until that sequence lands, the seam
// is UNIMPLEMENTED and reports so, and Try() DEGRADES to the caller's kInstant
// apply -- the build is complete and the heal still lands. The whole executor is
// therefore runtime-inert today (byte-identical to the kInstant heal); it activates
// the day DriveObservedCast is filled in.
//
// THREADING. Try() and the reconcile hooks run on the AddTask job WORKER (the
// per-follower tick). Any hand/equip/caster mutation the trigger performs is
// MainThread::Post'd (#62). CastBounds is armed on the worker (lock-free);
// APMFBridge calls are any-thread-safe. Never touch g_followers/g_active off the
// worker (#4).
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::ComposedCast {

    // Try to EXECUTE a_spell as an ANIMATED forced cast by a_follower at a_target
    // (a_target == a_follower => a self cast, no proxy). a_kind is the caster-
    // consent classification (Heal / Buff / Offense), used to bound the window.
    //
    // Returns TRUE only when the executor OWNED this cast (armed + driving), so the
    // caller returns without applying its own effect. Returns FALSE to DEGRADE:
    // the caller applies its existing kInstant effect THIS tick (a heal must land).
    // TODAY it always returns false (the trigger seam is not yet filled in).
    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind);

    // Is a_follower currently running an executor-held (animated) stream? The
    // concentration reconciles consult this so they treat an executor stream as
    // "live" and hand its END to this module rather than dispel + InterruptCast.
    // FALSE today (no stream is ever held until the trigger lands).
    bool StreamLive(RE::FormID a_follower);

    // End an executor-held stream (RESTORE the hand, disarm bounds, release the
    // APMF claim, free the proxy). Safe to call when no stream is held (no-op).
    void End(RE::FormID a_follower);

    // ── observe hand-off (Diagnostics::SpellSink consults these) ────────────────
    // The trigger arms an "expected cast" for (follower, spell) just before it
    // fires; the TESSpellCastEvent sink reports a positive match as the ANIMATED
    // path. Both no-ops today.
    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell);
    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell);

    // kPreLoadGame / revert -- beside ConcProxy::Reset() / CastBounds::Reset().
    // Drops every stream, frees the dedicated proxy pool, releases every claim.
    void Reset();

}
