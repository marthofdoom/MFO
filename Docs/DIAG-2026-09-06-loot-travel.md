# DIAG 2026-09-06 -- "Loot travel is genuinely broken"

Field session 19:51-19:59, Tuxbornrc1. MFO `aae6df64` (DLL 843490ac), APMF `69c57bde`
(DLL 84872e0d). All `file:line` below are against those two commits (read via
`git show`, not the working tree). Followers: Jesper 750012C6, Cicero 0009BCB0,
Adelinda 00015D09. Evidence file: scratchpad `log0906b/LOOT-EVIDENCE.txt` (A-G).

## 1. Verdict

**Yes, loot travel is broken, and the owner's read is exactly right: every arrival
in this log is a walk-by, not a loot path.** The chain breaks at ONE stage: the
package offer is claimed, but the engine is never told to re-evaluate while the
claim is visible. Both `EvaluatePackage` nudges that are supposed to make the
engine adopt the offered package run BEFORE the claim is published to the 0x49
hook, so the hook answers "no claim" and the follower stays on his follow package.
Nothing re-nudges after publication. The only times the engine ever picked up the
travel package in this session were leg retargets and releases, where MFO happens
to nudge again while the (earlier) claim is already published.

Concretely, in this session:

- 6 dispatches. **0 of 6** put the follower on the travel package. All 14
  `onTravelPkg=false` WALK lines belong to dispatch windows.
- 3 leg retargets. **3 of 3** put the follower on the travel package. All 5
  `onTravelPkg=true` WALK lines follow a leg nudge (19:58:08-19:58:18).
- The 16 `claimedHits` at 0x49 are fully accounted for by explicit nudges (below).
  **Zero** came from the engine's own evaluation cadence and **zero** from either
  dispatch-time nudge. `claimedActorsNow=2, claimedHits=0` at 19:57:37.300, six
  seconds into two live claims, is this bug printed as a number.
- Because the follower never walks, the stall/deadline clocks convict the corpse
  (silently, on the deadline path) and the batch ends "done". That is what makes
  the failure read as "unreachable"/"batch done" instead of "package never engaged".

This is a regression introduced when the field-proven `AliasPkgProbe` was graduated
into `ControlMap` + `OfferPackage`: the probe set the claim first and nudged one
frame later; the graduated code nudges inside the apply loop and publishes after.

## 2. Stage-by-stage chain

| # | Stage | Status | Evidence (code / log) |
|---|-------|--------|------------------------|
| 1 | Loot rule fires | OBSERVED-WORKING | `Logistics.cpp:1606` "rule N fired"; section D: 10 firings 19:57:31-19:58:11, all after combat ended (~19:57:27). |
| 2 | Candidate selection + eligibility census | OBSERVED-WORKING | Census loop `Logistics_Loot.cpp:1596-1760`. The 19:56:07/19:56:17 "0 eligible" lines are IN-COMBAT scans (combat logs run to 19:57:27); drops are `farleash` (player-to-ref > 4000 AND follower not already on it, `:1677-1680`) and `empty` (`HasLoot` false for the category, `:1688`). No owned/locked/waiting drops. Post-combat scans produced candidates for all 6 dispatches. Filter is not rejecting anything it should accept. See Q4. |
| 3 | Dibs / blocklist | OBSERVED-WORKING mechanically, FED FALSE VERDICTS | `TravelFailedRecently` `Logistics_internal.h:541-547`; every `[lootskip] blocklist=true` names the ref just deadlined/stalled/DONE by this follower's own leg (timestamps match to the ms). The verdicts are wrong because of stage 6 (RC#2). |
| 4 | Walk decision -> `LootTravelFill` | OBSERVED-WORKING | `Logistics_Loot.cpp:1969`; "APMF travel dispatched" `Packages.cpp:1605` x6. Navmesh gate passed (navdist 50-89 < 300); no "off-navmesh" lines. |
| 5 | Package offer claim (`kIntent_OfferPackage`) | OBSERVED-WORKING but DEFERRED | `APMFBridge.cpp:654-664` -> `EnsureClaimLocked` -> `api->RequestEx` = `ControlMap::EnqueueRequest` (`ControlMap.cpp:66`, enqueue only). "CLAIMED" prints 3 ms AFTER MFO's "dispatched" (31.115 -> 31.118 etc.), i.e. after the next `Drain()`. |
| 6 | 0x49 redirect answers MFO's package while the engine QUERIES | **OBSERVED-BROKEN at dispatch** | Thunk `PackageGate.cpp:102-130` reads `m_published` (`ControlMap.cpp:794`). MFO's nudge `Packages.cpp:1604` runs before the op is even drained; APMF's Engage nudge `OfferPackage.cpp:50` runs at `ControlMap.cpp:514`, inside the apply loop, before `Publish` at `:261`. Log: first `[ch.9-redirect]` for each dispatch prints at the RELEASE (40.560, 45.077, 55.607), never at the dispatch; heartbeat 19:57:37.300 `claimedHits=0` with 2 claims live. |
| 7 | Engine actually RUNS the package | OBSERVED-WORKING after a leg/release nudge; NEVER after a dispatch | `onTravelPkg` = `Forms::IsTravelPackage(GetCurrentPackage())` (`Logistics.cpp:863`, `Forms.h:140-145`, includes the 4 APMF packages). 5 true lines, all with `curPkg=FE06783[678]`, all within 1-5 s after a leg nudge (02.805, 05.072, 07.736). 0 true lines in any dispatch window. |
| 8 | Actor physically moves under it | OBSERVED (leg phase only) | Jesper on FE067837: dist 1088 (08.666) -> 249 (13.466). Adelinda on FE067838: 387 (10.131) -> arrival (13.731). `curPkg` is a direct engine-state read, so this motion was not the follow package. Not proven for any dispatch (see Q5/unobservables). |
| 9 | Arrival | OBSERVED, but 2 of 3 are walk-bys | Cicero 01.602 (`onTravelPkg=false`, 326 u away 1.2 s earlier) and Adelinda 06.534 (`onTravelPkg=false`, 220 u) arrived on their FOLLOW package = the player walked them there. Only Adelinda 13.731 arrived on a travel package. `kArrivalDist=200` (`Logistics_internal.h:360`). |
| 10 | Take | OBSERVED-WORKING | `StripCorpse` at `Logistics.cpp:975`; 1 "looted" (Cicero, walk-by). |
| 11 | Batch release | OBSERVED, but 3 of 5 "batch done" are false verdicts | Deadline path `Logistics.cpp:1094-1113` marks the ref failed with NO log line; `TravelDeadline = clamp(dist/150+5, 6, 20) s` (`Logistics_internal.h:609-612`). Cicero->0009B1C6: 514 u -> 8 s from 31.485 = 39.5, silent fail on the 40.55 tick, `[lootskip] blocklist=true` 40.558, "batch done" 40.560. Cicero->FF001870: 410 u -> 7 s from 48.152 = 55.15, released 55.607. Jesper->FF001870: 987 u -> 11 s from 53.073 = 04.07, leg at 05.072. Adelinda->0009B1C6 stalled instead (`kNoProgress` 5 s, `kMoveEps` 50 u, `:434-442`): "unreachable" 45.075 while on 00037D11, her own package. |

### The claimedHits accounting (why "zero natural hits" is a fact, not an inference)

The `[ch.9-redirect]` line dedups on transition (`PackageGate.cpp:151-169`), so lines
under-count; the heartbeat counters do not (`:138-141`). Every hit attributable:

| Heartbeat | Reported | Nudges with a PUBLISHED claim visible |
|---|---|---|
| 19:57:37.300 | 0 | none: MFO dispatch nudges (31.115, 31.485) ran pre-drain; APMF Engage nudges (31.118, 31.488) ran pre-publish. |
| 19:58:07.305 | 8 | 40.560 MFO release nudge (`Packages.cpp:1768`, before the deferred release), 40.563 APMF `Release()` nudge (`OfferPackage.cpp:62`, also pre-publish, claim still visible), 45.077 + 45.080 same pair, 55.607 + 55.609 same pair, 02.805 leg (`Packages.cpp:1686`), 05.072 leg = **8**. |
| 19:58:37.301 | 14 | + 07.736 leg, 10.000 + 10.004 pair, 14.932 + 14.936 pair, 19.469 APMF-only (player-combat clear passes `nullptr` at `Logistics.cpp:759-761`, so no MFO nudge) = **14**. |
| 19:59:07.295 | 16 | + 55.060 retreat clear MFO nudge + 55.062 APMF release nudge = **16**. |

All three checkpoints match exactly. There is no room for a single hit from the
engine's own package-evaluation cadence across roughly 75 s of cumulative claim
time on three followers. **`won=true` on 16/16 therefore means only: whenever MFO
or APMF nudged with a published claim, the hook redirected.** It never means the
engine queried on its own, and it says nothing about the dispatch windows, where
the hook was never consulted at all.

## 3. Ranked root causes

### RC#1 -- CONFIRMED: both dispatch-time nudges run before the claim is published

Read end to end:

- `Packages.cpp:1595-1607` `LootTravelFill`: `OfferPackage(...)` then
  `a_follower->EvaluatePackage(true,false)` then log. `OfferPackage` ->
  `APMFBridge.cpp:659` `EnsureClaimLocked` -> `api->RequestEx` ->
  `ControlMap::EnqueueRequest` (`ControlMap.cpp:66-87`): pushes a `PendingOp`, returns.
  MFO's nudge therefore runs with the op still in `m_queue`.
- Next frame, `Arbiter.cpp:33` -> `ControlMap::Drain()` (`ControlMap.cpp:173`):
  ops applied at `:210-218`; `ApplyRequest` calls `channel->Engage(...)` at `:514`;
  `Engage` (`OfferPackage.cpp:46-51`) calls `packagegate::EvaluatePackage`
  synchronously; `Publish(std::move(next))` only at `:261`, after the loop.
- The 0x49 thunk (`PackageGate.cpp:122`) calls `TryGetOwningClaim`, which loads
  `m_published` (`ControlMap.cpp:794`). At Engage time that is still the OLD
  snapshot. The thunk sees no claim, returns the engine's own answer, counts an
  anchor hit only. The engine keeps the follow package.
- Nothing nudges again until MFO's next `EvaluatePackage` (leg retarget
  `Packages.cpp:1686`, or release `:1768`). By then the claim has been published for
  seconds, so those nudges win. That is the exact partition the log shows.
- The engine does not consult 0x49 on its own for a follower on a stable follow
  package (accounting above). The `anchorHits` rate (~2.7k/s) comes from other
  actors; which ones is unobservable here (section 5).

The mirror image on release is also wrong: `ApplyRelease` (`ControlMap.cpp:535`)
calls `channel->Release()` at `:575` inside the loop, and `Release()` nudges
(`OfferPackage.cpp:62`) while the claim is STILL published. So at every release the
engine is told to run the travel package (that is why every release-time redirect
prints `won=true`), then the claim vanishes with no further nudge. Whether the
follower then keeps walking to the stale target until the engine's slow re-eval is
not visible in this log (no WALK lines after release) but is predicted.

Why the earlier "quiet hold" finding said the opposite: `AliasPkgProbe.cpp`
(first at APMF `5edbdd32`) did this in the right order. `OnHotkey` stores
`g_claimActor` FIRST ("redirect takes effect on the next 0x49") and queues the
nudge for the NEXT frame's `OncePerFrame`; on release it clears the claim first
("stop redirecting BEFORE the release eval") and nudges after. `PackageGate.cpp:30-33`
says the graduation kept "the SAME EvaluatePackage nudge" but moved it into
Engage/Release; it kept the call and inverted the ordering on both edges. The probe
proved the mechanism; the graduation broke the precondition.

### RC#2 -- CONFIRMED: stall/deadline verdicts run against legs whose package never engaged, and the guard that would notice is bypassed for APMF legs

- `Logistics.cpp:1023-1027`: for `Packages::IsAPMFTravelHeld(slot)` the package-theft
  guard is skipped entirely, on the stated premise (`:1007-1022`) that "0x49
  overrode it back to the APMF package every tick". That premise was true under the
  probe (RC#1) and is false in this build. So `!IsTravelPackage(cur)` on an APMF
  leg is never detected, never re-asserted, never logged.
- The stall/deadline clocks then run against a follower who is following the
  player: `kNoProgress` 5 s / `kMoveEps` 50 u (`Logistics_internal.h:434-442`),
  `TravelDeadline` 6-20 s (`:609-612`). The deadline branch (`Logistics.cpp:1094-1113`,
  `gone || stalled || now > deadline`) calls `MarkTravelFailed` with NO log line when
  it is a plain deadline; only the `stalled` case prints "unreachable". Three of the
  five "batch done" releases here are silent deadlines (timings in the table).
- `MarkTravelStalled` (`Logistics_internal.h:555-568`) accrues `g_stallStrikes`;
  2 strikes -> `g_travelUnreach` sticky for `kTravelStickyCooldown` = 60 s (`:539`).
  Strikes are only erased on arrival (`Logistics.cpp:916`) and deliberately survive
  the idle reassess (`:1634-1642`). A reachable corpse 800+ u away therefore goes
  sticky after two dispatches without a travel package ever running. Not reached in
  this 8-minute session (no STICKY line), but it is the wedge on a longer one.
- This is the masking layer (CLAUDE.md principle 7): the real failure ("package never
  engaged") is reported as a reachability verdict on the corpse.

### RC#3 -- PLAUSIBLE: runtime FF package displacing an engaged travel package, with no re-assert

Cicero, 2.4 s after his leg nudge (02.805): WALK 05.202 `onTravelPkg=false
curPkg=FF001F36` (a runtime-created package), then 09.999 `onTravelPkg=true`. One
observation; the FF package may have been transient. With the guard bypassed
(RC#2) nothing would re-assert if it were not. Worth watching once RC#1 is fixed,
not a cause of this session's failure.

### Not present in this log (Q5)

- "Wins by re-assert, not a quiet 0x49 hold": the opposite. For APMF legs there is
  NO re-assert at all (guard bypassed, `Logistics.cpp:1023`); the only re-nudges are
  leg retargets, and they win because the claim is published by then, not because
  of any loop. The quiet hold never existed in this build.
- Off-navmesh 5-minute sticky false-positive: not present. No "off-navmesh" lines;
  navdist 50-89 < gate 300; the sticky window is 60 s now; no STICKY lines. The
  strike->sticky path is reachable through RC#2's false stalls instead.

## 4. Minimal fixes (described, not written)

**RC#1, APMF side (the root, and the right place):** in `OfferPackageChannel`, do
not call `EvaluatePackage` inside `Engage`/`OnOwnerChanged`/`Release`. Defer the nudge
one hop past this Drain's `Publish` with `apmf::mainthread::Post`, which the
CastCompose eviction path already does for exactly this ordering reason
(`ControlMap.cpp:478-489`, "one hop past this Drain's Publish"). This restores the
probe's proven order on both edges: engage = publish then nudge; release = publish
(claim gone) then nudge, so the framework package resumes instead of the travel
package being asserted at the instant of release. Look up the actor by FormID inside
the posted lambda, never capture the pointer.

**RC#1, MFO side (independent, belt-and-braces, and the only option if APMF cannot
change this cycle):** in `LootTravelFill` the dispatch-time nudge at
`Packages.cpp:1604` can never work on the APMF route (the op is not even drained);
either drop it or move it to the next logistics tick. Cleanest: in the Walking phase
for an APMF-held slot, when `!IsTravelPackage(GetCurrentPackage())` and the leg has
not yet been seen engaged, nudge ONCE and stamp the leg (a single post-publish
confirm, not a re-assert loop), and log it so a second miss is loud.

**RC#2 (must ship with RC#1 or the next false verdict lands on the fixed path):**
for APMF legs, do not let the stall/deadline clocks run until the package has been
observed engaged at least once (`IsTravelPackage(cur)` seen true for this leg);
before that, a `!IsTravelPackage` tick is a "package not engaged" condition, logged
as such, and abandoned after a short grace WITHOUT `MarkTravelStalled` (transient
`MarkTravelFailed` at most). Give the deadline branch a log line; a silent
`MarkTravelFailed` is how this session's three "batch done" releases hid their
cause. Reconsider `g_stallStrikes` surviving the idle reassess once stall verdicts
are trustworthy; until then it is a wedge amplifier.

**RC#3:** no change now; once RC#1 lands, re-enable the theft guard for APMF legs
in its logging-only form (strike accounting off) and see whether FF packages ever
displace an engaged travel package.

## 5. Unobservable from this log, and the probe that settles each

1. **Does the engine EVER consult 0x49 on its own for a follower on a stable follow
   package?** The heartbeat only splits anchor vs claimed; the ~2.7k/s anchor hits
   are anonymous. The accounting says "no" for these three actors over ~75 s of
   claim time, but a longer claim might eventually hit the engine's periodic
   re-eval. Probe: the existing `[ch.9-observe]`/`[ch.9-poll]` (`NonAliasProbe`,
   `PackageGate.cpp:186-204`) is NumLock-gated and was OFF; arm it, or cheaper, add
   a per-actor hit counter for the three follower FormIDs regardless of claim
   state, printed on the same heartbeat. One cycle.
2. **Does the follower keep running the travel package after release** (the
   release-edge half of RC#1)? No WALK line prints once `tr.phase` leaves Walking.
   Probe: in `LootTravelClear`'s APMF branch, post a one-tick-later readback of
   `GetCurrentPackage()` (the probe's own "GetCurrentPackage now 0x.." line) and log
   it. One cycle.
3. **Was the leg-phase movement travel-driven or player-following?** `curPkg` proves
   which package was current, which is the mechanism; the position deltas alone
   cannot separate the two, and `pathSpeed` reads 0.0 even while closing 1088->249
   (`pathingCurrentMovementSpeed` is not a usable signal, as the code comment already
   says). Probe: log follower-position delta alongside player-position delta on the
   WALK line; a follower closing on a corpse while the player stands still is
   unambiguous. Cheap, same line.
4. **Whether `Engage`'s nudge reaches 0x49 at all** (as opposed to reaching it with
   the old snapshot). Both produce the same zero. Settled by the same fix -- but
   NOT by the criterion this section originally named. See "Pass criterion" below:
   `[ch.9-redirect]` is a DEDUPED line and cannot be used as the observable.

**The single highest-value change for one deck cycle:** defer the ch.9
`Engage`/`Release` nudge one hop past `Publish` (RC#1, APMF side), and in the same
build make MFO's Walking phase log "APMF leg not engaged (curPkg=..)" instead of
silently deadlining (RC#2's logging half).

## 6. Pass criterion (CORRECTED 2026-09-07)

**The criterion this document originally gave was WRONG and must not be used.** It
read: "a `[ch.9-redirect]` line within one frame of every CLAIMED". A later APMF
review proved that produces FALSE NEGATIVES, by construction:

- `[ch.9-redirect]` dedups on the answer tuple (`PackageGate.cpp`, RULE D). A repeat
  dispatch of the SAME package form for the same actor re-derives a byte-identical
  tuple, so the line is suppressed and never prints -- even though the redirect
  worked. Loot travel re-offers one fixed package form per slot, so this is the
  NORMAL case here, not an edge case.
- The release-then-same-form-re-request pair inside ONE `Drain` makes it worse: the
  release nudge is correctly dropped as stale (the claim is already back by Pump
  time), so 0x49 is never consulted with no claim standing and the remembered tuple
  is never invalidated. APMF has since added `PackageGate::ForgetRedirect` on the
  release edge to close that specific hole, but the dedup itself remains -- a
  working redirect can still print nothing.

**Use these two instead. Both are positive, non-deduped observations:**

1. **APMF side --** `[ch.9-nudge] 0x<actor> engage nudge FIRED post-publish (claim=true
   form=0x<pkg>); curPkg now 0x<pkg>` within one frame of that actor's `[ch.9]
   ... CLAIMED`, with **`curPkg now` equal to the claimed package form**. That
   readback is the proof the engine adopted it; the word "FIRED" alone only proves
   the nudge was not dropped.
2. **MFO side --** `onTravelPkg=true` on the first `[loot] ... WALK->` line after every
   `APMF travel dispatched`, and **no** `[loot] ... TRAVEL PKG NOT ENGAGED` line for
   that dispatch. (MFO gained that explicit not-engaged line, plus a non-silent
   `DEADLINE EXPIRED` line carrying `legEngaged=`, on `fix/mfo-loot-travel-client`.)

Plus the unchanged behavioural check: no `[lootskip] blocklist=true` on a ref the
follower never actually reached.
