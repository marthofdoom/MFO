#pragma once
#include "PCH.h"
#include "CasterConsent.h"   // SpellKind

// ─────────────────────────────────────────────────────────────────────────────
// ComposedCast -- the Composed Forced Cast (CFC) executor, now a THIN SHIM
// (S1, marth 2026-09-05: prove cast+heal on APMF).
//
// WHAT CHANGED. The old drive lived here: DriveObservedCast/PhaseSelect/
// PhaseFire/HealProxy force-equipped the follower's hand caster and replayed
// the engine's observed animated-cast sequence by hand. It stayed OBSERVE-ONLY
// (always degraded to the caller's kInstant apply) because it raced APMF/the
// AI for the SAME hand -- a cross-thread use-after-free that CTD'd in the
// field. APMF's feat/cast-act graduated kIntent_SelectSpell into a DECLARATIVE
// contract: name the spell/hand/target (APMFBridge::ClaimHealCast) and APMF
// itself equips the hand, drives the observed sequence, and GUARANTEES
// delivery (its own CastSpellImmediate fallback). MFO makes NO engine call for
// this path any more -- the force-equip that caused the race is gone
// structurally, not just gated off.
//
// WHAT THIS MODULE STILL DOES. (1) HEAL-ONLY gate: only CasterConsent::
// SpellKind::Heal is ever routed through APMF here -- offense and buff return
// false immediately and stay on the byte-identical AI-fired / kInstant paths,
// untouched by this pass. (2) Arms CastBounds so MFO's OWN CasterConsent hook
// -- installed globally, so it ALSO intercepts APMF's driven CheckCast/
// RequestCastImpl on the SAME hand caster -- stands down for the window
// (SPEC-FORCED-CAST.md §2); that hook doesn't know or care which DLL is
// driving the hand, only that MFO itself vouches for the (actor, spell) pair.
//
// Try() returns TRUE only once APMF confirms it holds the claim (the caller
// then skips its own kInstant apply); FALSE degrades to the caller's proven
// kInstant heal (a heal must never vanish -- APMF absent, ABI too old, toggle
// off, SE/VR, or a refused claim all degrade cleanly, byte-identical to today).
//
// THREADING. Try()/End() run on the AddTask job WORKER (the per-follower tick),
// matching every other Actuation_Direct entry point (#4). CastBounds is
// lock-free; APMFBridge calls are any-thread-safe.
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::ComposedCast {

    // Try to CLAIM a_spell as a declarative APMF-driven cast by a_follower at
    // a_target (a_target == a_follower, or nullptr, -> self; wire target = 0).
    // a_kind gates: only SpellKind::Heal is ever routed through APMF here.
    //
    // Call every tick the gambit still wants the heal -- a repeat call with the
    // SAME (spell, target) is a cheap refresh; a CHANGE re-points the SAME APMF
    // claim in place (no release/re-engage churn), so a heal that switches
    // target or spell mid-stream stays one continuous claim. Returns TRUE only
    // once APMF holds the claim (caller returns without applying its own
    // effect); FALSE degrades to the caller's kInstant apply.
    //
    // NEVER SUBSTITUTES: a_spell is always the gambit's own configured spell,
    // forwarded to APMF UNCHANGED, along with the actual a_target -- MFO does
    // not inspect delivery, does not proxy, and does not pick a different
    // spell the follower happens to know. A DELIVERY/TARGET MISMATCH (a_spell
    // is Self-delivery, a_target is not the caster -- e.g. a Self-only heal
    // gambited to heal an ally) is APMF's OWN problem to solve: its
    // feat/cast-act drive mints its own delivery-flip proxy synchronously on
    // ITS confirmed-main thread (core/CastExecutor.cpp's proxy pool, field-
    // proven) and drives that instead. MFO has no business building one here.
    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind);

    // Release a_follower's heal-cast claim + its CastBounds arm now. Call the
    // instant the gambit stops wanting the heal (target lost / rule no longer
    // wins) so APMF restores the hand immediately; the shared APMFBridge
    // kExpiry backstop (~500 ms) covers a caller that forgets. Safe to call
    // when nothing is held (no-op).
    void End(RE::FormID a_follower);

    // ── observe hand-off (Diagnostics::SpellSink's call site, kept inert) ───────
    // The retired hand-drive used to arm an "expected cast" here and report a
    // positive TESSpellCastEvent match as the animated path landing. APMF now
    // owns that whole observation on its own side; these are permanent no-ops
    // so Diagnostics.cpp's existing call site keeps compiling unchanged.
    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell);
    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell);

    // kPreLoadGame / revert -- beside CastBounds::Reset(). This shim holds no
    // local state of its own (APMFBridge::ClearTransientState drops the claim;
    // CastBounds::Reset drops the bound); kept as the one seam Actuation_Direct.
    // cpp's ClearSelfCasts already calls.
    void Reset();

}
