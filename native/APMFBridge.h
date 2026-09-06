#pragma once
#include <RE/Skyrim.h>
#include <chrono>   // FacetExpiry()'s std::chrono::milliseconds return type

// ─────────────────────────────────────────────────────────────────────────────
// APMF integration — MFO as an APMF client (Phase 3: the OWNED cast gambit). APMF
// (AI Package Management Framework) is a SEPARATE SKSE DLL; MFO talks to it ONLY
// through the byte-shared, append-only header `APMF_API.h` (the same discipline MFO
// applies to `MEO_API.h`) plus APMF's runtime interface query. APMF holds ZERO
// MFO-specific code; this bridge is the ONLY MFO code that knows APMF exists.
//
// THE MODERATOR MODEL (marth 2026-09-02). APMF ARBITRATES facets; it NEVER generates
// behaviour. MFO makes the behaviour with its OWN proven mechanisms and EXECUTES it;
// APMF only makes it WIN (single arbiter of the facet + suppresses competitors). So
// this bridge only ever CLAIMS facets — it never asks APMF to cast, command a target,
// or move a body (APMF would refuse; its channels are arbitration-only).
//
// The OWNED CAST, granularly: for a hostile cast gambit MFO CLAIMS two facets here —
//   * ClaimOffenseCast  (kIntent_Cast)         — "the AI's own seats cast THIS spell at
//                                                THIS target" (PORTED feat/offense-cast-
//                                                seats, 2026-09-05, off the retired ch.8
//                                                kIntent_SelectSpell gate-only claim this
//                                                site used to make — see the offense-cast
//                                                section below for why)
//   * ClaimCombatTarget (kIntent_CombatTarget) — "MFO owns this follower's combat target"
// and then, in Actuation::CastOn, EXECUTES the equip half itself: Loadout::Prepare puts
// the spell in the follower's LEFT hand, commands the target via `Targeting::Command`
// (currentCombatTarget), grants its own `CasterConsent`, and — while the kIntent_Cast
// claim stands — APMF's five engine vfunc seats drive the follower's OWN combat AI to
// select/equip/charge/aim/fire/channel EXACTLY that spell at EXACTLY that target: a real,
// fully-animated, MOBILE cast, and (unlike the retired gate-only claim) the gambit's named
// spell is the one that actually fires, not whatever the AI would have picked on its own.
// Crucially MFO does NOT claim the MOVEMENT facet, so the follower keeps kiting while it
// casts — that granular non-interruption is exactly why the cast routes through APMF. No
// forced cast on this path (see CAST-DELIVERY.md; force lives only in the legacy hybrid).
//
// Claim lifecycles: casting = PER-CAST (refreshed each winning cast tick; released
// crisply by ReleaseOffenseCast the instant no cast rule holds). combat-target = PER-COMBAT
// (created by a cast directive, re-pointed via APMF Repoint when the foe changes, kept
// alive every in-combat tick by RefreshCombatTarget, released only at combat end via the
// expiry sweep) — so a cast->melee transition keeps the claim, it does not drop it.
//
// DEGRADE-WHEN-ABSENT. APMF is a declared prerequisite, but MFO must never hard-fail
// without it: Acquire() logs once and leaves the interface null; every call here
// no-ops when APMF is absent (or Config::g_apmfCast is off), and CastOn falls back to
// the LEGACY AI-first-wait + force-on-miss hybrid (also selectable via the MCM
// bLegacyCastHybrid toggle), so MFO's casting is byte-identical without APMF.
//
// THREADING (APMF's contract, APMF_API.h). Request/RequestEx/Release are safe from
// ANY thread — they copy POD and enqueue; APMF applies on the game thread. MFO's
// cast dispatch (CastOn) and the per-pump sweep (Tick) run on the AddTask job worker,
// so they may call these directly. This bridge's own claim map is guarded by a mutex
// (worker touches Select/Hold/Refresh/Release/Tick; kDataLoaded/kPreLoadGame touch
// Acquire/ClearTransientState).
//
// NO SAVE/CO-SAVE STATE. Cast ownership is runtime-only; nothing here is serialized.
// On kPreLoadGame ClearTransientState() drops every claim (APMF wipes its own control
// map there too, so a stale handle Release is a harmless no-op).
// ─────────────────────────────────────────────────────────────────────────────
namespace MFO::APMFBridge {

    // Fetch the APMF C-ABI interface. Call ONCE at kDataLoaded (APMF.dll is loaded
    // by then). Null / logs "absent" if APMF is not in the load order or too old.
    void Acquire();

    // Is the APMF interface live (present + ABI >= 2, so RequestEx is available)?
    bool Available();

    // ROUND-ROBIN-AWARE STALENESS WINDOW (APMFBridge.cpp, 2026-09-05; moved to
    // external linkage feat/cast-gambit-concentration so it has ONE definition
    // shared cross-TU). Every claim in this file that is refreshed from INSIDE
    // the per-follower Scheduler::Tick round-robin service (cast-select,
    // combat-target, weapon-order equipment, heal-cast) is only re-touched every
    // ~0.133s * partySize, not on a flat beat -- this returns the release window
    // sized for that gap (worst-case suppression + round-robin lap, floored at
    // 500ms), NOT the flat package-offer kExpiry. Reused verbatim by
    // Actuation.cpp's firing-spell-gambit lock (Task 2, #9: a floor is safe, a
    // guessed budget is not) as the staleness bound for the un-claimed direct-
    // force concentration case, so both budgets can never drift apart.
    std::chrono::milliseconds FacetExpiry();

    // Worker-safe (called from the pump, Diagnostics.cpp's SleeperLoop, beside
    // Tick()). No-ops instantly when Available() is true -- ZERO cost beyond
    // that one check whenever APMF is present, and the toast can NEVER fire in
    // that case. When APMF is absent AND Config::g_warnNoApmf is on, posts one
    // unobtrusive RE::DebugNotification corner toast (never a modal) reminding
    // the player MFO is running its legacy fallback: once ~30-60s after load,
    // then roughly every 10 minutes for as long as APMF stays absent. Cadence
    // state is plain session-relative timing (Rapport::SessionMinutes(), which
    // itself resets on load) -- no new co-save state, nothing serialized. The
    // actual DebugNotification call is routed through MainThread::Post since
    // this runs on the job worker, not the main thread.
    void MaybeWarnAbsence();

    // Hand-mode constant for the a_hand parameter both ClaimOffenseCast and
    // ClaimHealCast below take (2 = left, per APMF's ival encoding -- see
    // each function's own a_hand doc for the weapon-hand-exclusion rule this
    // implements). Declared HERE (moved up from beside ClaimHealCast) so it
    // is in scope for ClaimOffenseCast's default argument too -- named so a
    // call site reads as policy, not a bare magic 2.
    inline constexpr std::int32_t kApmfHandLeft = 2;

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every
    // other accessor here takes). Does `a_follower` currently hold a LIVE
    // offense kIntent_Cast claim (i.e. is APMF's engine-seat drive actively
    // casting this follower's gambit spell right now)? REPOINTED (feat/
    // offense-cast-seats, 2026-09-05) from the retired ch.8 kIntent_SelectSpell
    // gate-only claim this accessor used to back -- same name, same call
    // sites, now backed by ClaimOffenseCast's kIntent_Cast handle instead
    // (Owned::offenseHandle, APMFBridge.cpp). Two independent consumers:
    // (1) CasterConsent's own exclusivity/hard-abort standdowns (folded into
    // `ClientCastClaimed` too, feat/offense-cast-seats) and (2) CombatStyle's
    // equip-gate standdown -- both still correct under kIntent_Cast, since a
    // live claim ALSO answers the 0x0F CheckShouldEquip seat (APMF_API.h),
    // covering exactly what those two standdowns used to rely on ch.8 for.
    // Does NOT affect CasterConsent's own consent-GRANT (the veto-removal
    // that lets the AI consider casting at all) -- that stays MFO's, APMF
    // only ever narrows/drives, never invents unrequested consent.
    bool IsOwnedCastActive(RE::FormID a_follower);

    // ── offense-cast facet CLAIM: PER-CAST, TTL-bounded (ch.8b, APMF v5) ────────
    // PORTED (feat/offense-cast-seats, 2026-09-05) off the retired ch.8
    // kIntent_SelectSpell gate-only claim (`ClaimCasting`/`ReleaseCasting`,
    // removed) this call site used to make: that channel only ARBITRATEs +
    // DENIES a competing framework's own spell selection -- the follower's
    // OWN AI still picked whichever spell IT wanted, the long-standing
    // "target right, spell wrong" defect (see the module doc block's cast
    // gambit history). This claim instead rides the SAME kIntent_Cast facet
    // `ClaimHealCast` uses (full engine-seat shape documented on that
    // function below) -- while it stands, APMF's five seats drive the
    // follower's OWN combat AI to select/equip/charge/aim/fire/channel THIS
    // spell at THIS target natively, closing the defect: the gambit's named
    // spell is now the one that actually fires. Kept as its OWN claim slot
    // (`Owned::offenseHandle`, distinct from `Owned::healHandle`) -- heal and
    // offense casts are mutually exclusive per tick by
    // `CasterConsent::SpellKind` (never concurrent on one follower) but never
    // share state, so neither claim's release/refresh cross-talks the other.
    //
    // a_hand: LEFT, ALWAYS -- matches `Loadout::Prepare`'s own `EquipSpell`
    // target (`LeftHandSlot()`, `Actuation.cpp`) exactly, so the claimed hand
    // and the physically-equipped hand never disagree (see `ClaimHealCast`'s
    // doc below for the deck-proven failure mode an auto/right hand caused).
    // The right hand is reserved for a weapon regardless of whether one is
    // currently held -- MFO's own offense equip never contests it either.
    // a_concentration: pass true only for a held-stream offense cast that
    // reaches this claim -- today's sole call site (`Actuation::CastOn`'s
    // owned-cast branch) never sees one (concentration forks off earlier, to
    // `ConcentrationCast`'s direct-force stream, a DIFFERENT delivery model
    // that predates and is untouched by this claim), so it always passes
    // false; wired for a future AI-driven concentration offense call site
    // without inventing one here. a_stopPct: a HEAL-ONLY concept (a client
    // restore threshold read by seat 0x07); offense has no such threshold in
    // scope -- always 0.
    //
    // Worker-safe. CREATE-OR-REFRESH: call every tick the gambit still wins --
    // a repeat call with the SAME (spell, target, hand, concentration,
    // stopPct) is a cheap refresh; a CHANGE releases and re-requests (no
    // in-place re-point on RequestCast's rich payload). Returns whether
    // a_follower now holds a LIVE claim (false -> caller falls back to the
    // legacy AI-first-grace + force-on-miss hybrid, byte-identical to the
    // APMF-absent path -- a refused claim must never silently drop the cast).
    // No-op (returns false) when APMF is absent, its resolved interface has
    // no RequestCast slot (ABI < 5), or Config::g_apmfCast is off.
    bool ClaimOffenseCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                          std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                          std::uint32_t a_stopPct = 0);

    // Worker-safe. Release ONLY the offense-cast claim now (the combat-target claim, if
    // any, is left alone). Call the instant no cast rule holds (Scheduler !castSeen) so
    // the claim releases crisply, not after the round-robin-aware expiry backstop.
    void ReleaseOffenseCast(RE::FormID a_follower);

    // ── combat-target facet CLAIM: PER-COMBAT ────────────────────────────────────
    // Worker-safe. CLAIM the combat-target facet for this follower (APMF records the
    // owner; the intended `a_target` rides along). This does NOT command the target —
    // MFO commands it via Targeting::Command itself. RE-POINTS the claim in place when
    // the target changes (same handle, via APMF Repoint — a mid-battle retarget is not a
    // release). `a_create`: true creates the claim if none exists yet — both a CAST
    // directive and an Attack directive pass true, so a pure-melee follower gets the
    // same arbitration a caster does (Actuation.cpp's kActAttack handler); pass false to
    // re-point ONLY an existing claim without ever creating one. No-op when APMF is
    // absent or Config::g_apmfCast is off.
    void ClaimCombatTarget(RE::FormID a_follower, RE::FormID a_target, bool a_create);

    // Worker-safe. Keep the follower's EXISTING combat-target claim alive (refresh its
    // expiry timestamp only — no create, no re-point). Call every in-combat service tick
    // so the claim persists across non-targeting rules (potion/flee/wait) for the WHOLE
    // fight; it is released by the expiry sweep only once this STOPS being called (i.e.
    // combat ended). No-op if the follower holds no combat-target claim.
    void RefreshCombatTarget(RE::FormID a_follower);

    // Worker-safe. Once-per-pump sweep: release each claim not refreshed within its
    // expiry window (offense-cast backstop; combat-target = combat-end detector).
    // offense-cast/combat-target/weapon-order-equipment/heal-cast all use the
    // round-robin-aware FacetExpiry() (APMFBridge.cpp, anon ns) -- proven
    // round-robin-bound, not flat-cadence (2026-09-05: first the heal claim, then
    // the equipment claim, deck-captured); package-offer alone stays on the flat
    // kExpiry (proven genuinely flat-refreshed, Packages::Pump()); combat-action-
    // deny is unwired and inherits the flat backstop pending real evidence. Call
    // from the pump body.
    void Tick();

    // ── weapon-order EQUIPMENT facet CLAIM: PER-ORDER (ch.15) ───────────────────
    // Retires MFO's native weapon-order equip gate (CombatStyle.cpp's EquipGateThunk)
    // onto APMF's T2a CheckShouldEquip hook, mirroring the cast-select/CasterConsent
    // retirement above -- MFO still EXECUTES the force-equip itself (ActorEquipManager
    // forceEquip, Actuation.cpp's EquipWeapon/g_forcedWeapon); this claim only makes
    // APMF's OWN gate enforce "no spell/staff re-arm while the order holds the hands"
    // instead of MFO's own gate doing it natively. GATE-ONLY: APMF makes no engine
    // write for a param'd Equipment claim (APMF_API.h's kIntent_Equipment comment).
    //
    // Worker-safe. CLAIM (or refresh-in-place) the equipment facet for a_follower,
    // naming a_weaponForm (kIntent_Equipment, param.form = the forced weapon's
    // FormID). Call every tick the force-hold survives (Actuation::ReconcileForcedWeapon
    // when it decides to KEEP the hold) -- same "the claim call also refreshes" idiom
    // ClaimOffenseCast uses, so one call both engages a new hold and keeps an existing one
    // alive under the round-robin-aware FacetExpiry() backstop (2026-09-05: this
    // facet used to share the flat kExpiry and a deck capture on Cicero proved that
    // dropped a still-wanted claim ~919ms in -- ReconcileForcedWeapon runs from the
    // SAME per-follower Scheduler::Tick round-robin lap as cast-select, not a tight
    // beat, so it needed the same fix). RE-POINTS in place (same handle) if the held
    // weapon FormID changes (a melee<->ranged flip re-forces a different weapon).
    // No-op when APMF is absent or Config::g_weaponStyleControl is off.
    void ClaimEquipment(RE::FormID a_follower, RE::FormID a_weaponForm);

    // Worker-safe. Release the equipment claim. Call the instant the force-hold
    // actually releases (Actuation::ReleaseForcedWeapon, its single choke point --
    // reached from ReconcileForcedWeapon's release branch AND every teardown site
    // that force-unequips directly) so MFO's native gate re-enforces immediately if
    // APMF is absent, with no gap between "hands freed" and "gate re-armed".
    void ReleaseEquipment(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every other
    // accessor here takes, matching IsOwnedCastActive's shape exactly). Does
    // `a_follower` currently hold a LIVE equipment facet claim? CombatStyle.cpp's
    // EquipGateThunk consults this to stand its OWN weapon-order deny down (APMF's
    // T2a now owns that enforcement) -- independent of, and checked alongside,
    // IsOwnedCastActive's existing cast stand-down; either, both, or neither may be
    // true for a given follower on a given tick. Returns false (native gate keeps
    // enforcing, byte-identical to pre-APMF) whenever APMF is absent, the claim was
    // never made, or it already expired.
    bool IsEquipmentClaimActive(RE::FormID a_follower);

    // ── package-offer facet CLAIM: PER-EXCURSION (ch.9, T3 0x49) ────────────────
    // Worker-safe. CLAIM the package-offer facet for a_follower, naming a_packageForm
    // (kIntent_OfferPackage, param.form). While this claim is held, APMF's 0x49 hook
    // (Actor::CheckForCurrentAliasPackage) hands a_follower this package DIRECTLY,
    // unconditionally -- no alias fill, no quest-priority race, so a follower who is
    // package-locked by an outranking custom AI framework (the Cicero case) still
    // gets it. The CALLER owns the package's own runtime target (Packages.cpp writes
    // a targType-0 runtime handle into it BEFORE calling this) -- this bridge only
    // ever names the FormID. RE-POINTS in place (same handle) on a repeat call with a
    // DIFFERENT form; a repeat call with the SAME form is a cheap refresh (matches
    // EnsureClaimLocked's existing unchanged-claim fast path). No-op when APMF is
    // absent or Config::g_apmfLootTravel is off.
    //
    // Returns whether a_follower now holds a LIVE package-offer claim (false on a
    // no-op no-APMF call, OR on a live APMF that REFUSED the claim -- e.g. it lost
    // arbitration to a higher-basis client, or no channel serves the intent on an
    // older ABI). MFO IS AN APMF SHOWPIECE (Docs/STATUS.md): the caller uses this
    // to LOG a refusal loudly, never to silently fall back to a pre-APMF route --
    // with APMF present, the APMF path is committed, not a first-try-then-decline.
    bool OfferPackage(RE::FormID a_follower, RE::FormID a_packageForm);

    // Worker-safe. Release ONLY the package-offer claim (any combat-action-deny claim
    // held for the same follower, if any, is left alone -- release it separately).
    // Call the instant the excursion ends (arrival / loot done / abandoned) so the
    // follower's framework package resumes immediately, not after the expiry backstop.
    void ReleaseOfferPackage(RE::FormID a_follower);

    // ── combat-action DENY facet CLAIM: PER-EXCURSION (ch.7, T1) ────────────────
    // Worker-safe. CLAIM the combat-action-deny facet for a_follower, naming
    // a_categoryMask (kIntent_CombatAction, param.ival -- an OR of
    // APMF_API::CombatActionCategory bits). While held, APMF denies the combat
    // behavior-tree leaves classified under the claimed categories (e.g.
    // kCombatActionCat_Offense denies Attack/Bash/RangedAttack/Cast*/etc). NOT
    // wired into the loot-travel dispatch by default (Logistics_Loot.cpp) --
    // MFO's own PACKAGE-THEFT guard already concedes loot-travel to a live combat
    // package on purpose (a follower who is actually fighting should keep
    // fighting); this helper exists for a caller that has a narrower, considered
    // need. No-op when APMF is absent or Config::g_apmfLootTravel is off. Returns
    // whether a_follower now holds a LIVE claim -- same showpiece-logging contract
    // as OfferPackage's return, for whichever caller eventually wires this in.
    bool ClaimCombatActionDeny(RE::FormID a_follower, std::uint32_t a_categoryMask);

    // Worker-safe. Release the combat-action-deny claim.
    void ReleaseCombatActionDeny(RE::FormID a_follower);

    // ── heal-cast facet CLAIM: PER-CAST, TTL-bounded (ch.8b, APMF v5) ───────────
    // MFO's Composed Forced Cast (Docs/SPEC-FORCED-CAST.md) makes a follower cast
    // a spell his AI would not choose -- the canonical case a heal at an ally.
    // PORTED (feat/mfo-cast-port, 2026-09-05) from the kIntent_SelectSpell +ACT
    // drive back to kIntent_Cast/RequestCast: APMF's feat/ai-cast-seats-impl
    // RETIRED the +ACT drive entirely (`ival`/`target`/`pos` on kIntent_SelectSpell
    // are now accepted-and-ignored -- see APMF_API.h) and replaced it with FIVE
    // engine vfunc seats answered on the Restore caster vtable: while a kIntent_Cast
    // claim stands, the NPC's OWN combat AI selects/equips/charges/aims/fires/
    // channels `spell` at `target` -- a real native animated cast. APMF fires
    // NOTHING itself (no EquipSpell/CastSpell/CastSpellImmediate/anim-graph write);
    // every engine call is the AI's own, from its own behavior tree. `target` is
    // now LOAD-BEARING (seat 0x0A hands it to the engine as the magic target, seat
    // 0x0D as the aim override) -- it was RECORD ONLY under the retired +ACT drive.
    //
    //     RequestCast(actor, basis, {spell, proxy, target, flags, ttlMs})
    //     (repeat call, same values)   -- cheap refresh, no release/re-engage churn
    //     (repeat call, changed values) -- a NEW bounded claim (release + re-
    //                                      request; the rich payload has no
    //                                      in-place re-point, unlike SelectSpell's
    //                                      Repoint)
    //     Release(handle)              -- ends the claim, restores the AI's own
    //                                      cast deliberation
    //
    // A kSelf-delivery spell aimed at a non-self target needs `proxy` (or APMF
    // mints one itself -- core/CastProxy.h), because the engine's Self branch
    // always lands on the caster; MFO forwards `proxy = 0` and lets APMF mint it
    // (Try() does not inspect delivery, does not proxy, and does not substitute --
    // ComposedCast.cpp's own comment on this still applies verbatim).
    //
    // Rides its OWN kIntent_Cast channel (ch.8b) -- a DISTINCT claim slot from
    // offense's ClaimOffenseCast (also kIntent_Cast, ch.8b, above, PORTED
    // feat/offense-cast-seats off the retired ch.8 kIntent_SelectSpell claim);
    // heal and hostile casts are mutually exclusive per tick by
    // CasterConsent::SpellKind, never concurrent on one follower, but keeping
    // distinct facets avoids any accidental cross-talk.
    //
    // a_hand: LEFT, ALWAYS (THE RULE, marth 2026-09-05, deck-proven): whenever an
    // equip gambit is actively force-holding a weapon (APMFBridge::
    // IsEquipmentClaimActive), that weapon owns the RIGHT hand, so a spell must
    // claim LEFT (kApmfHandLeft above) rather than auto -- auto prefers the right
    // hand when it reads free, and a follower briefly unarmed (e.g. the
    // facet-expiry bug this same file fixed 2026-09-05) let auto grab the right
    // hand, only for the equip gambit's own re-equip to shove the spell right back
    // out ~500ms later (deck: "driving right hand" -> re-equipped 'Elven Dagger'
    // -> "never left rest -- degrading"). LEFT is also the correct fallback with
    // no weapon held: heals are left-hand almost always regardless. MFO always
    // passes kApmfHandLeft today (a full per-perk/loadout-aware hand pass -- both-
    // hands-free dual-cast/juggle, melee-vs-caster balance -- is tracked
    // separately as future work, not built here) -- there is no more auto/right/
    // dual hand-mode plumbing on this path; the retired +ACT drive's ival hand
    // bits are gone with it (APMF_API.h's kCastFlag_LeftHand carries the SAME
    // policy on the new payload). a_target: 0 = self; an ally/player FormID for
    // heal-other.
    //
    // a_concentration: pass true when a_spell->GetCastingType() ==
    // kConcentration -- sets APMF_API::kCastFlag_Concentration so the claim's TTL
    // floor applies (a held stream is never cut mid-channel by an early TTL).
    //
    // a_stopPct: 0 = no client threshold (the seat stops the channel at FULL
    // restoration, same as before this claim carried the bit); 1..100 = a whole
    // percent of the TARGET's PERMANENT actor value at which seat 0x07
    // CheckStopCast ends the channel -- lets a "Health < N% -> Heal" gambit's own
    // configured threshold stop the native channel exactly where the gambit says,
    // not always at 100%. Built into the claim via APMF_API::MakeStopPct(pct);
    // see that header for the exact semantics. Ignored (harmless) on an instant
    // (non-concentration) heal -- there is no channel to stop early.
    //
    // Worker-safe. CREATE-OR-REFRESH: call every tick the gambit still wants the
    // heal -- a repeat call with the SAME (spell, target, hand, concentration,
    // stopPct) is a cheap refresh; a CHANGE in any of them releases and
    // re-requests (a new bounded claim -- RequestCast has no in-place re-point).
    // Returns whether a_follower now holds a LIVE claim (false -> caller degrades
    // to kInstant, a heal must never vanish). No-op (returns false) when APMF is
    // absent, its resolved interface has no RequestCast slot (ABI < 5 -- an older
    // APMF.dll degrades cleanly rather than crashing), or Config::g_healAnimPackage
    // is off.

    // The heal-cast claim's TTL, ms (APMF_CastRequest::ttlMs). A SINGLE named
    // constant so ComposedCast.cpp's CastBounds::Arm window (the MFO-side consent
    // standdown, native/CastBounds.h) and APMF's own claim TTL (the auto-release
    // backstop if MFO ever forgets to release) never drift apart -- both windows
    // exist to bound the SAME thing (a heal claim MFO already vetted), so one
    // value serves both call sites. 6s: long enough to outlast one HoT tick + the
    // round-robin refresh gap (FacetExpiry() below sizes the SAME margin for the
    // local backstop), short enough a crashed/forgetful caller's claim
    // self-releases quickly (SPEC-FORCED-CAST.md §1.4's Heal case).
    inline constexpr std::uint32_t kHealCastTtlMs = 6000;

    bool ClaimHealCast(RE::FormID a_follower, RE::FormID a_spell, RE::FormID a_target,
                       std::int32_t a_hand = kApmfHandLeft, bool a_concentration = false,
                       std::uint32_t a_stopPct = 0);

    // Worker-safe. Release ONLY the heal-cast claim now (every other facet left
    // alone). Call the instant the gambit stops wanting the heal (target lost /
    // spell/rule no longer wins) so APMF restores the AI's own cast deliberation
    // immediately -- the round-robin-aware FacetExpiry() backstop (Tick()) AND the
    // claim's own TTL (on APMF's side) cover a caller that forgets.
    void ReleaseHealCast(RE::FormID a_follower);

    // Worker- AND combat-thread-safe (mutex-guarded read; the SAME g_mx every
    // other accessor here takes). Does a_follower currently hold a LIVE heal-cast
    // claim? NOTE: MFO's OWN CasterConsent hook is installed globally, so it also
    // intercepts APMF's seat-answered CheckStartCast/CheckCast on the SAME
    // Restore caster vtable -- that hook stands down via CastBounds
    // (native/CastBounds.h) OR'd with this very accessor (CasterConsent.cpp's
    // ClientCastClaimed), NOT this accessor alone. This also exists for parity/
    // observability with the other IsXActive queries above.
    bool IsHealCastActive(RE::FormID a_follower);

    // Release every claim and clear the map. kPreLoadGame / revert, AFTER the pump is
    // drained (so no worker tick races the map).
    void ClearTransientState();
}
