#pragma once
#include "PCH.h"

// ─────────────────────────────────────────────────────────────────────────────
// CastBounds -- the general "this is an MFO-EXECUTED, bounded cast" registry.
//
// WHY IT EXISTS (SPEC-FORCED-CAST.md §2). MFO's exact-bounding gate
// (CasterConsent::ConcUnboundedDeny) HARD-ABORTS a tracked follower's own
// concentration cast at Exact unless MFO can prove the stream is one MFO itself
// is metering. Historically the ONLY such proof was the legacy alias-package
// stream (`Packages::StreamLive` / `g_liveStream`, written only by
// `Packages::Begin`). Any cast MFO arranges THROUGH the AI/caster deliberation --
// the (now deleted) heal-anim UseMagic PACKAGE, and the new ComposedCast
// executor's HAND cast -- passes through the same CheckCast (0x0A) / CheckStartCast
// thunks, but was NEVER registered as ours, so exact-bounding vetoed it as an
// "unbounded AI stream." That was the deck HARD-ABORT of 0002F3B8 / FF001BA4.
// (The kInstant ConcProxy direct-force path -- `CastSpellImmediate` -- does NOT
// deliberate through those hooks, so it is never vetoed and needs no bound; it is
// deliberately NOT a CastBounds writer. `ComposedCast::Try` is the SOLE writer.)
//
// THE FIX. Before an MFO executor touches the hand it ARMS (actor, spell) here
// -- and (actor, proxy) too, since the hand may hold a delivery-flipped copy --
// for a bounded TTL. CasterConsent then early-passes a registered (actor, spell)
// (§2.2): an MFO-executed cast has already cleared every gambit gate on the
// worker, so the combat-thread consent hooks must not second-guess it. This is
// exactly the legacy `g_liveStream` contract, generalized from one slot to
// eight and made lock-free so the combat-thread reader never blocks.
//
// THREADING. Writers (Arm/Disarm) run on the worker OR the main thread; the
// reader (Live) runs on the COMBAT thread inside the CasterConsent thunks. The
// store is a fixed array of {atomic<u64> key, atomic<u64> expiry} slots -- NO
// lock. key = (actor<<32 | spell); expiry = steady_clock ms. Publication order
// (expiry before key, release on key; acquire on key before expiry) guarantees a
// reader that sees a matching key also sees its real expiry; a torn/cleared slot
// reads as no-match or expired -> the reader FALLS BACK to the deny (fail-safe:
// the worst case is a legitimately-bounded cast being denied one tick, never a
// truly-unbounded stream let through).
//
// AUTO-EXPIRY is the crash guardrail: a follower whose executor crashed/timed
// out mid-phase never leaves a standing early-pass -- the slot lapses at the TTL
// the claim carried, the same bound APMF's kIntent_Cast claim auto-releases at.
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::CastBounds {

    // Register (actor, spell) AND (actor, proxy) as MFO-EXECUTED, bounded casts
    // for ttlMs. Idempotent per key (re-Arm refreshes the expiry). proxy == 0 or
    // proxy == spell registers a single key. Writers: worker or main thread.
    void Arm(RE::FormID a_actor, RE::FormID a_spell, RE::FormID a_proxy, std::uint32_t a_ttlMs);

    // Clear EVERY slot owned by a_actor (its spell AND proxy keys). Called in the
    // executor's RESTORE phase and on every abort/degrade path.
    void Disarm(RE::FormID a_actor);

    // Combat-thread reader (CasterConsent's ConcUnboundedDeny / CheckCastThunk /
    // CheckStartCast). True iff a live, non-expired (a_actor, a_spell) slot exists.
    // Lock-free; fail-safe (a torn/cleared/expired slot reads false).
    bool Live(RE::FormID a_actor, RE::FormID a_spell);

    // kPreLoadGame / revert reset -- beside ConcProxy::Reset(). Clears all slots.
    void Reset();

    // Diagnostics: how many slots are currently live (non-expired). Cheap, lock-free.
    std::size_t LiveCount();

}
