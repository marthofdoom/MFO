#pragma once
#include "PCH.h"
#include "CasterConsent.h"   // SpellKind
#include "apmf/APMFBridge.h"      // kApmfHandLeft/kApmfHandRight -- WatchClaim's a_hand default/param

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
// Try() answers FOUR outcomes (TryResult below). It was a bool; Fable SEV-2
// (2026-09-06) split out `Held`, which had been hiding inside `true`, and
// fix/mfo-no-decline-fallback (marth 2026-09-07) split the remaining `Refused`
// into the two asymmetric halves it had been conflating:
//   * Claimed       -- APMF confirms it holds the claim; the caller skips its own
//                      kInstant apply.
//   * Held          -- a DIFFERENT spell's live claim owns the follower's single
//                      heal slot; nothing was delivered and nothing was applied.
//   * NotApplicable -- APMF was NEVER ASKED (SE/VR, non-Heal kind,
//                      bHealAnimPackage off, APMF absent, or an ABI that predates
//                      the cast facet). The caller's proven kInstant heal runs,
//                      byte-identical to before -- this is the DEGRADE-WHEN-ABSENT
//                      contract, not a fallback from a decline.
//   * ApmfRefused   -- APMF is PRESENT and CAPABLE and it said NO. There is NO
//                      fallback: the caller FAILS CLOSED, logs loudly, and the
//                      heal visibly does not happen. Masking it (principle 7)
//                      would hide an APMF bug indefinitely.
// The distinction is the whole point: "a heal must never vanish" holds only when
// MFO never asked. Once MFO has asked and been refused, the heal SHOULD visibly
// not happen.
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

    // ── Try()'s OUTCOME SET (Fable SEV-2 2026-09-06; widened to FOUR by
    // fix/mfo-no-decline-fallback 2026-09-07) ─────────────────────────────────
    // Try() used to return a bool, and the F1 incumbent hold rode home inside
    // `true` -- so every caller that treats "true" as delivery reported a spell
    // that was never cast as FIRED, bought the Scheduler's suppression window on
    // that no-op, and re-stamped Actuation's Task-2 hand lock with the held-off
    // spell (starving BOTH rules until a TTL). A hold is not a delivery and it is
    // not a refusal either -- it is its own outcome, so it gets its own value and
    // every caller must decide about it explicitly.
    //
    // THE SECOND SPLIT (marth 2026-09-07, verbatim: "without APMF, it needs to
    // just work, whatever unpolished way works. With APMF theres no fallback for
    // it. APMF shoudl work"). The old `Refused` value ALSO conflated two
    // asymmetric worlds -- "there was nothing to ask" and "APMF was asked and
    // said NO" -- and collapsing them is the DECLINE-FALLBACK that rule forbids.
    // They are now separate values. The two splits are ORTHOGONAL: `Held` is
    // about slot ownership BETWEEN TWO HEALS (MFO-internal, APMF never asked),
    // `NotApplicable`/`ApmfRefused` is about what APMF's ARBITRATION answered,
    // so all four distinctions are real and none subsumes another.
    //
    // NAMING NOTE (deliberate, do not "tidy"): the old `Refused` was RENAMED to
    // `NotApplicable` and the new value is `ApmfRefused`, NOT a re-pointed
    // `Refused`. Re-using the name `Refused` for the opposite meaning would have
    // let every existing call site keep compiling with INVERTED semantics; with
    // the identifier gone, the compiler had to be shown every one of them.
    enum class TryResult : std::uint8_t {
        // Not routed here at all: a non-Heal kind, SE/VR, bHealAnimPackage off,
        // APMF ABSENT, or an APMF whose ABI predates the cast facet (< 5). APMF
        // was never asked, so there is nothing to fail closed on -- the caller
        // MUST run its own kInstant apply. This is the DEGRADE-WHEN-ABSENT
        // contract ("without APMF it needs to just work"), byte-identical to the
        // pre-existing `false`/`Refused`.
        NotApplicable = 0,
        // APMF holds a live kIntent_Cast claim naming THIS spell: its engine
        // seats drive the follower's own AI to cast it, so the caller skips its
        // own apply (the pre-existing `true`).
        Claimed,
        // HELD OFF: a DIFFERENT spell's claim already owns this follower's single
        // heal slot and has not been observed firing, so this spell was neither
        // claimed nor applied. The caller must treat this as a TRANSPARENT no-op
        // -- never Fired, never a suppression window, never a hand-lock re-stamp
        // -- and let the rules below it run. HeldOffBy() names the incumbent.
        Held,
        // APMF is PRESENT, CAPABLE (ABI >= 5, toggle on) and it REFUSED the
        // claim. There is NO fallback here: the caller must FAIL CLOSED -- no
        // kInstant apply, no force, no legacy hybrid -- log loudly and let the
        // heal visibly not happen. A refusal is a BUG TO FIX IN APMF, and
        // masking it (CLAUDE.md principle 7) hides it indefinitely.
        ApmfRefused,
    };

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
    // cast/Auto.cpp's CastAuto, the one caller that DOES have one) is
    // unaffected.
    //
    // Call every tick the gambit still wants the heal -- a repeat call with the
    // SAME (spell, target, stop-percent) is a cheap refresh; a CHANGE releases
    // and re-requests the SAME facet (a new bounded claim -- RequestCast has no
    // in-place re-point, unlike the retired +ACT drive's Repoint). The FOUR
    // outcomes (TryResult above) are what the caller must decide about:
    //   * Claimed       -> APMF holds the claim; the caller applies NOTHING.
    //   * Held          -> a DIFFERENT spell's live claim owns the slot and
    //                      NOTHING happened; transparent no-op (Fable SEV-2).
    //   * NotApplicable -> there was nothing to ask; the caller's own kInstant
    //                      apply is the SUPPORTED degrade contract.
    //   * ApmfRefused   -> APMF is PRESENT and CAPABLE and said NO; the caller
    //                      must FAIL CLOSED, never route around it.
    //
    // NEVER SUBSTITUTES: a_spell is always the gambit's own configured spell,
    // forwarded to APMF UNCHANGED, along with the actual a_target -- MFO does
    // not inspect delivery, does not proxy, and does not pick a different
    // spell the follower happens to know. A DELIVERY/TARGET MISMATCH (a_spell
    // is Self-delivery, a_target is not the caster -- e.g. a Self-only heal
    // gambited to heal an ally) is APMF's OWN problem to solve: it mints its
    // own delivery-flip proxy for the engine's Self branch (core/CastProxy.h)
    // rather than landing on the caster. MFO has no business building one here.
    // a_hand (feat/mfo-perhand-heal): the heal slot this claim lives in -- LEFT (the
    // default, every caller but one) or RIGHT for the heal road's second recipient
    // (cast/HealRoad.cpp). The hold, the in-flight hold and the watch are that hand's.
    TryResult Try(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target,
                  CasterConsent::SpellKind a_kind, std::uint32_t a_stopPct = 0,
                  std::int32_t a_hand = APMFBridge::kApmfHandLeft);

    // ── WHICH ROAD A HEAL TAKES (animheal phase 2, 2026-09-30) ─────────────────
    // ONE decision, asked by every heal caller (CastOn, CastAuto, CastSelfDirect,
    // CastTargetDirect) so the combat table, the AUTO series and the OOC table can
    // never pick two different roads for one actor at one instant (RC-1 of the
    // 2026-09-21 freeze was exactly that: MFO's own claim and MFO's own direct cast
    // colliding on one follower). COMBAT TABLE ONLY (MFO-B173/B175): CastAuto,
    // CastSelfDirect and CastTargetDirect ask it only inside Fire() (g_firingRule set);
    // the out-of-combat table never mints a claim, and while a combat claim still
    // stands its heal waits (Actuation's OocHealWaitsForClaim) instead of casting
    // direct beside it.
    //   NotHeal       -- not a Heal-kind spell (CasterConsent::ClassifySpell). The
    //                     caller's own road, unchanged. Wards and other Restoration-
    //                     school buffs stay on the direct road (IsRestorationSpell).
    //   Claim          -- THE heal road. Harbinger present and capable (ABI >= 5,
    //                     bHealAnimPackage ON), a verified runtime (1.6.1170 or
    //                     1.5.97: Harbinger installs the heal seats on both), and the
    //                     follower HAS a CombatController, the object every heal seat
    //                     hangs off. The follower's own AI casts it, animated. A
    //                     direct HEAL stream still standing on the follower is ENDED
    //                     when the claim is MINTED (Try()'s Claimed return, MFO-B176 /
    //                     MFO-B192; Actuation::EndDirectHealStreams), so the two
    //                     roads never overlap in this direction either.
    //   DirectNoCombat -- Harbinger present and capable, but no CombatController (out
    //                     of combat, an own-OOC follower in a party fight, a healer
    //                     who retreated). marth's D1 default: the direct road,
    //                     unanimated, with its own rate-limited [heal] line. A heal
    //                     claim still standing on that follower is RELEASED here, so
    //                     the two roads never overlap (one road per actor).
    //   DirectNoSeat   -- Harbinger present, a controller, but the shape has no seat:
    //                     a heal whose delivery is not Self aimed at its own caster.
    //                     Harbinger resolves a self claim to the claimant only for a
    //                     Self-delivery spell (APMF 12c456e), so such a claim could
    //                     never fire. Direct road, labelled. (AUTO never picks it.)
    //   DirectDegrade  -- Harbinger absent or too old, an unverified runtime, or the
    //                     kill switches (bHealAnimPackage / bEquipToCast) OFF: the
    //                     documented "legacy = Harbinger-absent" degrade, no line.
    // NEVER a fallback: a Claim that is then refused or never fires stays loud and
    // falls through per the gambit; nothing re-routes it to the direct road.
    // a_target: the recipient (the caster for a self heal); nullptr when the caller
    // has not picked one yet (CastAuto), which skips only the DirectNoSeat test.
    // Worker-serial (same worker as Try/End; its log dedup is unlocked for that reason).
    enum class HealRoad : std::uint8_t { NotHeal, Claim, DirectNoCombat, DirectNoSeat, DirectDegrade };
    HealRoad ChooseHealRoad(RE::Actor* a_follower, RE::SpellItem* a_spell, RE::Actor* a_target);

    // NoteOocDirectHeal (review F2 on 3a42371): the out-of-combat heal callers
    // (CastSelfDirect / CastTargetDirect with g_firingRule == kNoRule) no longer ask
    // ChooseHealRoad (MFO-B173), so a follower that still holds a CombatController
    // while the party is out of combat healed on the instant road with NO [heal]
    // line. marth 2026-09-30: nothing uses the instant road in the finished build
    // (aside from where 1.5 needs it), and every cause of fallback usage is a problem
    // to fix, so the use is named. One [heal] line per (follower, spell) per 15 s
    // (RoadLogDue); a Heal-kind spell only. Logging only, changes nothing. Worker-serial.
    void NoteOocDirectHeal(RE::Actor* a_follower, RE::SpellItem* a_spell);

    // Release a_follower's heal-cast claim + its CastBounds arm now. Call the
    // instant the gambit stops wanting the heal (target lost / rule no longer
    // wins) so APMF restores the AI's own cast deliberation immediately; the
    // shared APMFBridge FacetExpiry() backstop AND the claim's own TTL (on
    // APMF's side) cover a caller that forgets. Safe to call when nothing is
    // held (no-op). Also clears this module's own silent-cast diagnostic watch.
    // a_keepSpell (review round 2, R2-5): true leaves the heal spell in the hand
    // (no Loadout::ReleaseSpellIf). ONLY the concentration stream cap passes it: that
    // release is a re-stream while the rule still wins, not the claim's real end, so
    // the next lap re-claims with the spell already in hand (Prepare: AlreadyReady).
    // End releases BOTH hands' heal claims (feat/mfo-perhand-heal); EndHand one hand's
    // (a_hand = kApmfHandLeft / kApmfHandRight). Only the LEFT takes its spell back.
    void End(RE::FormID a_follower, bool a_keepSpell = false);
    void EndHand(RE::FormID a_follower, std::int32_t a_hand, bool a_keepSpell = false);

    // animheal phase 2 (review F2). True when a_fired (the SpellSink's observed
    // form) is the LIVE heal claim's spell or its delivery-flip proxy AND that
    // spell is CONCENTRATION: its TESSpellCastEvent arrives as the channel STARTS,
    // so the sink must not take the spell back then (Loadout::StartCooldown's
    // a_release = false); End() takes it back when the claim ends. Worker-safe.
    bool HealClaimFireKeepsSpell(RE::FormID a_follower, RE::FormID a_fired);

    // IS A LIVE HEAL CLAIM'S CAST PENDING ON THE LEFT HAND? (review round 2, marth's
    // ruling 2026-09-30: "Gambit order wins, so in most cases it's heal. But a poorly
    // ordered gambit board shouldn't be rescued programmatically.") True while a heal
    // claim stands (it exists only because its rule WON the gambit order) AND its
    // heal is either charging / channelling on the left (CastInFlightOnHand, the one
    // in-flight definition) or has NOT fired within the post-fire cooldown
    // (fCastCooldown; ObservedFiring over the [cfc] watch). So it turns false the
    // moment the heal fires, stays false through the cooldown, turns true again when
    // the next cast of the same claim is due, and is false once the claim ends.
    // What reads it: MFO's own NON-GAMBIT left-hand automation (Loadout's shield-
    // on-hit restore and two-hander give-back) is suspended while it is true and
    // resumes right after; an equip gambit's hold ranked BELOW the heal rule yields
    // the left hand while it is true (Actuation's equip side adds the rank test).
    // No cooldown configured (fCastCooldown <= 0): pending for the whole claim.
    // Also true in the stream cap's one-lap re-stream gap (no claim, the LEFT lock
    // kept with its rank until the re-claim, Actuation::HealRestreamGap, MFO-B177).
    // Worker-serial (reads the watch map, like ObservedFiring).
    bool HealTakesLeft(RE::Actor* a_follower);

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
    //
    // MATCHES ON SPELL **OR** PROXY (cast-claim observability, 2026-09-06).
    // `a_spell` here is the event's OBSERVED spell -- for a claim whose delivery
    // APMF flipped through its own minted proxy (APMF_API::APMF_API_v6::
    // GetCastProxy, kSelf-delivery aimed at a non-self target), the cast that
    // actually lands is of the PROXY FormID, never the original. Before this,
    // the watch only ever recorded the original spell, so that landing cast
    // was filed as "their own spell, not ours" and Try()'s silent-claim
    // diagnostic fired a FALSE ALARM even though the claimed cast genuinely
    // fired (the exact false alarm diagnosed 2026-09-06). WatchArmed/WatchClaim
    // below now also record the proxy (0 on ABI < 6 or a claim that minted
    // none) and a match on EITHER field counts as "this claim's cast landed."
    // This does NOT weaken the diagnostic: a claim that produces neither the
    // original spell's nor the proxy's cast still warns exactly as before.
    bool ExpectingCast(RE::FormID a_follower, RE::FormID a_spell);
    void NoteObservedCast(RE::FormID a_follower, RE::FormID a_spell);
    // PER-HAND FIRE ATTRIBUTION (feat/mfo-perhand-heal, review R2-1 on 57bd5e7). The hand
    // NoteObservedCast's last fire of a_form went to (kApmfHandLeft / kApmfHandRight; 0 =
    // undecided or both): HealObsNoteClaimFire reads it right after, on the same task.
    // UndecidedFire: did a fire of a_spell that matched BOTH hands go undecided on a_hand
    // within a_withinMs? The never-fired bound stays off such a hand. Worker-serial.
    std::int32_t LastFireHand(RE::FormID a_follower, RE::FormID a_form);
    bool UndecidedFire(RE::FormID a_follower, std::int32_t a_hand, RE::FormID a_spell, std::uint32_t a_withinMs);

    // ── HELD-OFF query (Fable amendment (b), 2026-09-06) ──────────────────────
    // WHICH incumbent held a_spell off on a_follower's last Try(). Try() itself
    // now REPORTS the hold as TryResult::Held (Fable SEV-2 -- the hold used to
    // ride home inside `true`, so a caller could not tell it from a delivery at
    // all); this answers the follow-up question a LOG line needs: which spell's
    // claim was it that won the slot. Logistics.cpp's OOC concentration line
    // derived "APMF delivered" from bare claim liveness and therefore asserted
    // delivery of the WRONG spell in exactly this state.
    // Returns the INCUMBENT spell that held a_spell off on a_follower's most
    // recent Try(), or 0 if that Try was not a hold of a_spell (delivered,
    // refused, non-heal, or no Try since). Cleared at the top of every Try(), so
    // it is always "the last Try's outcome" and never a timed-out guess; read it
    // immediately after the CastTargetDirect/CastSelfDirect call it belongs to.
    // Worker-serial, no lock (see the THREADING note above).
    RE::FormID HeldOffBy(RE::FormID a_follower, RE::FormID a_spell);

    // ── generic claim-observed diagnostic (feat/offense-cast-seats, 2026-09-05;
    // PER-HAND feat/per-hand-cast-slots, 2026-09-06) ────────────────────────────
    // Arm/clear the SAME "[cfc]-style, claim live N ms with NO observed cast"
    // watch Try() arms for a heal, for a caller that claims kIntent_Cast through
    // a DIFFERENT path -- today: Actuation::CastOn's owned-cast branch, via
    // APMFBridge::ClaimOffenseCast, which does not go through Try() (Try() stays
    // HEAL-ONLY-gated, see Enabled() above; this is reuse of the diagnostic, NOT
    // a widening of the claim gate).
    //
    // PER-HAND now: a follower can hold a LIVE heal claim (always LEFT) and an
    // offense claim (LEFT, RIGHT, or both for a DualCast plan) CONCURRENTLY
    // (feat/per-hand-cast-slots -- APMF arbitrates kIntent_Cast per (actor,
    // hand) now), so a single shared-by-follower watch slot would let a second
    // hand's claim silently overwrite the first's diagnostic. Two slots per
    // follower now (a_hand selects which -- kApmfHandLeft or kApmfHandRight;
    // omit for the heal-matching default of LEFT). ExpectingCast/
    // NoteObservedCast above are unchanged in SIGNATURE (still (follower,
    // spell) only, matching Diagnostics.cpp's SpellSink call site, which has no
    // hand to report) -- internally they now check BOTH hand slots for a
    // matching spell.
    // WatchClaim: call every tick the caller's OWN claim call (ClaimOffenseCast/
    // ClaimHealCast) returns live, naming the hand(s) actually granted (both,
    // for a DualCast plan). ClearWatch: clears BOTH hands -- call when NOTHING
    // is wanted on this follower at all anymore (mirrors End()'s own
    // g_watch.erase for a caller that manages its own APMFBridge release
    // directly rather than through End()/Try()); a caller releasing only ONE
    // hand's claim (e.g. ComposedCast::End() for the heal, always LEFT) clears
    // only that hand's slot internally instead.
    //
    // a_proxy (cast-claim observability, 2026-09-06): the delivery-flip proxy
    // FormID APMF minted for the SAME claim (APMFBridge::GetOffenseCastProxy/
    // GetHealCastProxy, fetched by the caller right after the claim call this
    // WatchClaim call reports) -- 0 (the default) on ABI < 6 or a claim that
    // minted none. Recorded alongside a_spell so ExpectingCast above matches
    // either.
    void WatchClaim(RE::FormID a_follower, RE::FormID a_spell,
                    std::int32_t a_hand = APMFBridge::kApmfHandLeft, RE::FormID a_proxy = 0);
    void ClearWatch(RE::FormID a_follower);
    // ClearWatchHand: drop ONE hand's watch record (the other hand's survives) --
    // the per-hand twin of End()'s left-slot clear, WITHOUT End()'s
    // ReleaseHealCast/CastBounds side effects. Made for APMFBridge::Tick()'s
    // expiry sweep (fix/mfo-combat-restoration-direct, 2026-09-21): that sweep
    // drops a claim whose watch nobody cleared, so the NEXT claim of the same
    // spell re-armed through WatchArmed's no-reset branch and inherited the dead
    // claim's `since` -- deck 2026-09-21, a `[cfc] claim live 32383 ms` at 08:10:00
    // for a claim swept at 08:09:41. Pure map op, safe under the bridge's g_mx
    // (this module never takes it). Worker-serial like every call here.
    void ClearWatchHand(RE::FormID a_follower, std::int32_t a_hand);

    // Has the watch on THIS hand seen `a_spell` (or its recorded delivery-flip
    // proxy) actually FIRE within the last `a_withinMs`? (0 = "ever", the raw
    // latch.) The `observed` latch NoteObservedCast sets, read back with a
    // recency bound.
    //
    // THE RECENCY BOUND IS NOT OPTIONAL FOR A LIVENESS CALLER (review,
    // 2026-09-08). `observed` is a LATCH and no offense release path clears it --
    // not ReleaseCastClaimOnHand, ReleaseOffenseCast, PreemptHand's offense side
    // or APMFBridge's expiry sweep, and the fresh-claim site re-arms with the SAME
    // spell, which is WatchArmed's no-reset branch. So the raw latch means "this
    // spell fired once on this hand this fight", across claims AND across rules;
    // reading it as "this claim is alive" is unbounded for the ordinary case, not
    // an edge. Pass the window you actually mean.
    //
    // WHY IT IS EXPOSED (2026-09-08). Actuation's cast-hand lock heartbeats an
    // incumbent claim while its own rule is held off from re-aiming, and a
    // heartbeat that renews APMF's TTL must never be handed to a claim that can
    // never fire -- otherwise the hold is unbounded, which is the exact failure
    // APMFBridge::RefreshHealCastClaim's never-observed age cap exists to prevent.
    // "Has it fired?" is the only honest test of that, and this is where the
    // answer lives. Deliberately NOT ExpectingCast: that one answers "is this form
    // one we are watching for", i.e. the opposite question, and is unhand-scoped.
    //
    // Worker-serial, no lock -- same discipline and same map as every other
    // function in this header (see the THREADING note above).
    bool ObservedFiring(RE::FormID a_follower, std::int32_t a_hand, RE::FormID a_spell,
                        std::uint32_t a_withinMs);

    // kPreLoadGame / revert -- beside CastBounds::Reset(). Drops this module's
    // own silent-cast diagnostic watch map (APMFBridge::ClearTransientState
    // drops the claim; CastBounds::Reset drops the bound); kept as the one seam
    // cast/Direct.cpp's ClearSelfCasts already calls.
    void Reset();

}
