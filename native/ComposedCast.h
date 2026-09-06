#pragma once
#include "PCH.h"
#include "CasterConsent.h"   // SpellKind

// ─────────────────────────────────────────────────────────────────────────────
// ComposedCast -- the Composed Forced Cast (CFC) executor, a THIN SHIM over
// APMFBridge::ClaimHealCast (feat/mfo-cast-port, 2026-09-05).
//
// WHAT CHANGED (twice). The old drive lived here: DriveObservedCast/
// PhaseSelect/PhaseFire/HealProxy force-equipped the follower's hand caster and
// replayed the engine's observed animated-cast sequence by hand. It stayed
// OBSERVE-ONLY (always degraded to the caller's kInstant apply) because it
// raced APMF/the AI for the SAME hand -- a cross-thread use-after-free that
// CTD'd in the field. APMF's feat/cast-act then graduated kIntent_SelectSpell
// into a DECLARATIVE contract (name the spell/hand/target, APMF equips + drives
// + guarantees delivery) -- but APMF's feat/ai-cast-seats-impl RETIRED that
// +ACT drive in turn: `ival`/`target`/`pos` on kIntent_SelectSpell are now
// accepted-and-ignored (APMF_API.h). A cast that must actually HAPPEN is now a
// kIntent_Cast (ch.8b) claim (APMFBridge::ClaimHealCast, ported back to
// RequestCast this pass): while it stands, APMF answers FIVE engine vfunc
// seats on the Restore caster vtable so the NPC's OWN combat AI selects,
// equips, charges, aims, fires and channels the spell -- a real native
// animated cast. APMF fires NOTHING itself (no EquipSpell/CastSpell/
// CastSpellImmediate/anim-graph write); MFO makes no engine call here either --
// the force-equip that caused the original race is gone structurally, not just
// gated off, under either design.
//
// WHAT THIS MODULE STILL DOES. (1) HEAL-ONLY gate: only CasterConsent::
// SpellKind::Heal is ever routed through APMF here -- offense and buff return
// false immediately and stay on the byte-identical AI-fired / kInstant paths,
// untouched by this pass. (2) Arms CastBounds so MFO's OWN CasterConsent hook
// -- installed globally, so it ALSO intercepts APMF's seat-answered
// CheckStartCast/CheckCast on the SAME Restore caster vtable -- stands down
// for the window (SPEC-FORCED-CAST.md §2; CasterConsent.cpp's
// ClientCastClaimed, which ORs CastBounds::Live with the broader per-actor
// APMFBridge::IsHealCastActive); that hook doesn't know or care which DLL/
// seat is driving the cast, only that MFO itself vouches for the (actor,
// spell) pair. (3) A rate-limited DIAGNOSTIC: if a claim stands but the
// TESSpellCastEvent sink (Diagnostics.cpp's SpellSink) never reports this
// (follower, spell) actually firing, logs a warning -- see ExpectingCast/
// NoteObservedCast below. This is observation only: it never re-fires, never
// falls back, never manufactures the cast (APMF's INVARIANTS.md #0) -- a claim that
// stands silently STAYS silent; the log line is the diagnostic signal, not a
// fix.
//
// Try() returns TRUE only once APMF confirms it holds the claim (the caller
// then skips its own kInstant apply); FALSE degrades to the caller's proven
// kInstant heal (a heal must never vanish -- APMF absent, ABI too old, toggle
// off, SE/VR, or a refused claim all degrade cleanly, byte-identical to today).
//
// THREADING. Try()/End() run on the AddTask job WORKER (the per-follower tick),
// matching every other Actuation_Direct entry point (#4). CastBounds is
// lock-free; APMFBridge calls are any-thread-safe. ExpectingCast/
// NoteObservedCast are called from Diagnostics.cpp's SpellSink, itself an
// SKSE::GetTaskInterface()->AddTask closure -- the SAME serialized job-worker
// queue as Try()/End(), never concurrent with them, so the diagnostic map
// below needs no lock of its own (mirrors Actuation_Direct.cpp's g_selfCast/
// g_targetCast, also worker-serial and unlocked).
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::ComposedCast {

    // Try to CLAIM a_spell as a declarative APMF-driven cast by a_follower at
    // a_target (a_target == a_follower, or nullptr, -> self; wire target = 0).
    // a_kind gates: only SpellKind::Heal is ever routed through APMF here.
    //
    // a_stopPct: 0 = no client threshold (the claim's channel, if any, stops at
    // FULL restoration); 1..100 = the gambit's own configured heal-below
    // threshold, as a whole percent of the target's PERMANENT actor value --
    // forwarded to APMFBridge::ClaimHealCast's stop-percent (APMF_API::
    // MakeStopPct), which is read only by a CONCENTRATION channel's stop-cast
    // seat; harmless (ignored) on an instant heal. Defaults to 0 so every
    // caller that has no numeric threshold in scope (most of them -- see
    // Actuation_Direct.cpp's CastAuto, the one caller that DOES have one) is
    // unaffected.
    //
    // Call every tick the gambit still wants the heal -- a repeat call with the
    // SAME (spell, target, stop-percent) is a cheap refresh; a CHANGE releases
    // and re-requests the SAME facet (a new bounded claim -- RequestCast has no
    // in-place re-point, unlike the retired +ACT drive's Repoint). Returns TRUE
    // only once APMF holds the claim (caller returns without applying its own
    // effect); FALSE degrades to the caller's kInstant apply.
    //
    // NEVER SUBSTITUTES: a_spell is always the gambit's own configured spell,
    // forwarded to APMF UNCHANGED, along with the actual a_target -- MFO does
    // not inspect delivery, does not proxy, and does not pick a different
    // spell the follower happens to know. A DELIVERY/TARGET MISMATCH (a_spell
    // is Self-delivery, a_target is not the caster -- e.g. a Self-only heal
    // gambited to heal an ally) is APMF's OWN problem to solve: it mints its
    // own delivery-flip proxy for the engine's Self branch (core/CastProxy.h)
    // rather than landing on the caster. MFO has no business building one here.
    bool Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
             CasterConsent::SpellKind a_kind, std::uint32_t a_stopPct = 0);

    // Release a_follower's heal-cast claim + its CastBounds arm now. Call the
    // instant the gambit stops wanting the heal (target lost / rule no longer
    // wins) so APMF restores the AI's own cast deliberation immediately; the
    // shared APMFBridge FacetExpiry() backstop AND the claim's own TTL (on
    // APMF's side) cover a caller that forgets. Safe to call when nothing is
    // held (no-op). Also clears this module's own silent-cast diagnostic watch.
    void End(RE::FormID a_follower);

    // ── observe hand-off (Diagnostics::SpellSink's call site) ──────────────────
    // Diagnostics.cpp's TESSpellCastEvent sink calls ExpectingCast(caster,
    // spell) for EVERY cast a tracked follower's own AI fires; when Try() holds
    // a LIVE claim naming that exact (follower, spell) pair, this returns true
    // and the sink calls NoteObservedCast -- the positive fire signal that
    // APMF's seats actually landed a real cast this window (logged by the sink
    // itself as "*** THE ANIMATED PATH ***"). NoteObservedCast marks the
    // watch observed so Try()'s own silent-claim diagnostic (module doc above)
    // stops warning for the rest of this claim's life. Both are cheap
    // worker-serial map lookups -- see the THREADING note above for why no
    // lock is needed.
    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell);
    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell);

    // kPreLoadGame / revert -- beside CastBounds::Reset(). Drops this module's
    // own silent-cast diagnostic watch map (APMFBridge::ClearTransientState
    // drops the claim; CastBounds::Reset drops the bound); kept as the one seam
    // Actuation_Direct.cpp's ClearSelfCasts already calls.
    void Reset();

}
