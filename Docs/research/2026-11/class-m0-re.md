# MFO class update, milestone M0: read-only engine RE (Opus 5.5, 2026-11-09)

Scope: six RE briefs (H3, H2, G2, H5, G7, G11). No code, no branches, no seats. Every address was read from
our unpacked images (`binaries/{1.6.1170,1.5.97}/SkyrimSE.unpacked.exe`, `binaries/1.7.104/SkyrimSE.exe`).
AE ids come from `versionlib-1-6-1170-0.bin`, SE ids from `version-1-5-97-0.bin`, and 1.7.104 RVAs from a
masked-signature search (or the fork idmap). Tools: `scratchpad/m0re/` (ids.py, sdis.py, cmp.py, sig.py, rip.py,
ripf.py, xref.py). Proof grades: PROVEN = read on that binary. LIKELY = shape match only. UNPROVEN = not read.
ClickUp comments were posted on all six tasks (ids in the agent log).

Summary table

| Item | Verdict | Tier / effort | The one thing to know |
|---|---|---|---|
| H3 disengage | FEASIBLE-WITH-RISK | A / 3 (+2) | ReturnToCombatArea paths at RUN speed (enum 3) on all 3 runtimes. Facing (forward vs strafe) is the open risk |
| H2 aversion | FEASIBLE-WITH-RISK (AE, 1.7); SE UNPROVEN | A / 4 (+2 SE) | No vanilla lever. Gate the claimant's ADMISSION per selection pass at the IsValidTarget call. SE inlines it |
| G2 hide | FEASIBLE-WITH-RISK | A / 5 | RequestDetectionLevel is a cache READER. The source is CalculateDetection's out level at its ONE call site |
| H5 archer | FEASIBLE-WITH-RISK | detect B / 2; correct A / 4 (+2) | Scatter = aimController+0xA0 x fCombatAimProjectileRandomOffset. +0xA0 is a per-controller copy: gate it per actor |
| G7 reanimate | FEASIBLE | B / 3 (road A); A / 2 (road B) | ch.8b cannot target a corpse (Reanimate not seated, dead target ends the claim). The engine picks its own corpse |
| G11 ward | FEASIBLE | B / 3 | desiredTarget +0x20 / spell +0x28 / state +0x30 proven. Read-only mirror, no seat |

---

## 1. H3: disengage / reposition drive with regular movement mid-fight (task 1dfcuabcqeu)

**Where the engine decides.** The combat tree's Movement half runs the "Movement Selector" (a DynamicSelector,
re-picked every 0.25 s, first non-zero child): Essential Down > Flight > Flee > Hide > **Return To Combat Area** >
Exit Water > Search > Ranged > Flanking > Close (from `channels/CombatApproach.cpp` header, origin/main). Return To
Combat Area fires when the actor is outside its current combat area (area vfunc 0x06). ch.24 already recentres the
standard area (S1, `CombatAreaStandard` vfunc 0x0B).

**Gait: PROVEN RUN.**
- ReturnToCombatArea leaf act (vtable slot 0x02): AE 0x8AC700 -> `jmp 0x8A6EE0`; SE 0x813C70 -> 0x810190; 1.7.104
  0x8C1C70 -> 0x8BC3C0.
- That body builds a `CombatPathRequestGeneric<CombatPathingGoalPolicyReturnToCombatArea, ...StandardSearch,
  CombatPathDestinationLocation>` through AE 0x8ABDC0 / SE 0x811450 / 1.7 0x8C1320, with
  `mov dword ptr [rsp+0x20], 3` (the 5th argument) on all three runtimes.
- AE 0x8ABDC0 passes that 5th argument (`[rsp+0x150]`) to the path's SetSpeed, AE 0x8C3FE0. SetSpeed stores the
  enum at +0x1C and indexes the setting-pointer table at AE 0x2016A38 = {fCombatSpeedValueWalk, FastWalk, Jog,
  Run, Run}. The debug names at 0x2016A70 are WALK / FAST WALK / JOG / RUN / SPRINT. **3 = RUN.**
- The table read is AE-only. On SE and 1.7 only the literal 3 is proven, so the table on those two is LIKELY.

**Facing: UNPROVEN.** Run speed does not say whether the body faces along the path or keeps facing the target
(strafe or backpedal). The movement handler decides it (`MovementHandlerAgentStrafing` exists in RTTI). Not
resolved statically in this pass.

**ch.19.** Its combat cancel cannot be avoided without breaking its contract. In combat, a package reaches the
body only through a HoldPosition combat area anyway. So H3 is a ch.24 extension, not ch.19.

**Candidate (no new seat).** Extend ch.24:
(a) a POSITION mode: `posX/Y/Z` instead of a reference (S1 already writes the centre and R),
(b) a small R, so the actor is outside the area and Return To Combat Area takes the Movement half at RUN,
(c) an optional "disengage" flag that files a ch.7 Offense deny for the leg.
MFO picks the point (`FindEmptySpace`, plus multi-viewer LOS for G3).

**Deny holes.**
- Essential Down, Flight, Flee and Hide outrank Return To Combat Area. That is engine priority and correct.
- A package HoldPosition area wins (Yielded). A ch.23 leash can exclude the point (Leashed).
- Dodge Threat runs in parallel.
- Without the ch.7 deny, the Action half keeps attacking or casting while running. That is wanted for mages and
  maybe not for a Thief.
- Inside R the nine movement leaves clamp to the area, so the actor keeps fighting in the small disc.
- EngineDropped: no area update for 3 s.

**Probe.** One rate-limited line per second per claimed actor:
- which movement leaf ran (ActionGate already sees the ReturnToCombatArea act),
- the speed, and the angle between heading and velocity: under 45 deg = forward run, over 135 deg = backpedal,
  otherwise strafe,
- ch.24 `applied` and `engineInside`.
One field session with a far position answers facing.

**Verdict.** FEASIBLE-WITH-RISK. **Estimate:** tier A, effort 3. Add 2 if facing needs a movement-handler seat.

---

## 2. H2: "less likely to be targeted" (task 1dfcuabcqet)

**Where the engine decides.**
- `CombatTargetSelectorStandard` slot 0x06 (AE 0x84CFE0 / SE 0x7B5E50 / 1.7 0x862040) returns the cached pick at
  selector+0x18. It re-scores only when `now - [+0x28] > [+0x2C]`, with an interval of 1.0 s, or 3.0 s right
  after a change (AE constants 0x1FFE2F8 / 0x1FFE310).
- The re-score is FindBestTarget: AE 0x84D280 / SE 0x7B5FF0 / 1.7 0x8622E0. AE and 1.7 are identical over 267
  insns. SE has the same shape with different register allocation.
- It is called by E8 from slot 6 at AE 0x84D048 / SE 0x7B5EB8 / 1.7 0x8620A8.

**The score, per group CombatTarget (stride 0xA8), max wins.**

Admission:
- detectLevel (+0x04) > 0 AND stealthPoints (+0x08) <= 0 AND NOT kTargetLost, or the candidate is the current
  target and kTargetKnown. Everyone else goes to a fallback list (sorted by AE 0x84E000) that is used only when
  nothing is admitted.
- Then `IsValidTarget(selector, attacker, target, ezone)`, AE 0x84D940. It applies the
  bEncounterZoneTargetRestrict rule and an attacker-flag (+0xE8 bit 26) plus candidate-vfunc-0x5E test.

Terms:

| Term | Value | Condition |
|---|---|---|
| Combat memory (AE 0x68F7A0(attacker, target)) | +1000 | hit within 2 s, or current target attacked within 2 s |
| Combat memory | +100 | memory flag +0x14 |
| Current target (stickiness) | +100 | |
| AE 0x853D80 same-state test | +100 | attacker and target give the same result. Meaning UNPROVEN |
| Proximity | +(1 - min(d, 2048) / 2048) x 1000 | |
| attackerCount (+0xA4) | + global AE 0x31AB8C8 | count above (current ? 1 : 0). The global is 0 in the file and no RIP writer was found |
| Outside the combat area | -2000 | only when the area radius is under 1024 or the target is in another cell space. The call is AE 0x84D66F / SE 0x7B6455 |
| AE 0x853F10 false | -2500 | only in another space |
| Candidate vfunc 0x99 (`[rax+0x4C8]`) | -500 | |
| AE 0x67EFE0 true | -500 | |

**No vanilla lever.** No actor value, perk entry point or keyword feeds the score. The only vanilla input is
DETECTION (admission), and that is G2's road.

**Candidate: gate the input (admission).**
- (1) Pre-pass seat on the FindBest E8 call (all three runtimes have it). Count the admissible candidates that are
  not averse, and roll the claimant's weight once for this pass (thread-local).
- (2) `write_call<5>` on the IsValidTarget call: AE 0x84D52C (-> 0x84D940) / 1.7 0x86258C (-> 0x8629A0). The
  claimant reads "not valid" for this pass only if the roll says avert AND another admissible candidate exists.
  The engine's scorer ranks the rest. Nothing is written to the group or the selector.
- **SE 1.5.97:** IsValidTarget is INLINED in FindBest (the `call [rax+0x2F0]` at 0x7B6306 is inline), so there is
  no call site. The SE seat needs its own RE (principle 11). UNPROVEN.

**Deny holes.**
- (a) The Fixed selector (slot 6 AE 0x84DAF0: a scripted or package-fixed target) is untouched, by design.
- (b) A cached pick lasts 1-3 s, so the effective weight depends on the measured cadence (principle 9).
- (c) Never refuse the only admissible candidate, or the selector falls through to the search list.
- (d) Other writers: script StartCombat or SetTarget on the foe, another client's ch.20 pin on the foe, ch.21
  entry.
- (e) The +100 stickiness keeps a switched foe switched.
- (f) AoE and spells on others still hit the claimant.

**Probe.** At the existing ch.20 slot-6 seat, a rate-limited line (1/s per foe) whenever FindBest ran and a
claimant was a candidate: foe, candidate count, admissible count, the pick, and whether the pick was the claimant.

**Verdict.** FEASIBLE-WITH-RISK on 1.6.1170 and 1.7.104. SE UNPROVEN. **Estimate:** tier A, effort 4, plus 2 for
the SE seat RE.

---

## 3. G2: hide-when-unseen, H-DETECT (task 1dfcuabcqep)

**RequestDetectionLevel is NOT the input.** SE id 36748 0x5FC9A0 / AE id 37764 0x68F9F0 / 1.7 0x6A2520. On a
high-process observer it:
- gets the observer's detection record for the target (AE 0x6EC990, id 39515),
- asks AE 0x7AF3A0 (id 42702) for the result, re-requesting it when it is older than the per-priority staleness
  table,
- returns result+0x10, or -1000 if there is none.
It is a cache reader. Gating it changes one reader and leaves the cache and the combat group as they were.

**The source.**
- CalculateDetection: AE id 37774 0x690870 / SE id 36758 0x5FD870 / 1.7 0x6A33B0. The prologue is byte-identical
  and the bodies match apart from frame size (478 / 483 / 477 insns compared).
- **It has ONE caller on every runtime**: the detection manager job, AE id 42742 0x7B0710 (call at 0x7B0D8B) /
  SE id 41659 0x719990 (call at 0x719EB6) / 1.7 call at 0x7C589B.
- The job computes only if AI detection is on (`[*(AE 0x20F69B0)+1]`, the tdetect toggle), neither actor carries
  flags 0x820, and both are within range of the player.
- Arguments: rcx = observer, rdx = target, r8 = int* level, r9 = byte*, then on the stack: byte*, int*, and a
  point / 3 ints. Re-verify the full signature at build (principle 6).
- The job then stores the result (AE 0x7B1AF0, id 42760) and dispatches to the listeners (AE 0x7AF4B0, id 42704
  -> 0x7AF830, listener slot 0x10).
- `CombatGroupDetectionListener` is one listener, which reaches `CombatGroup::UpdateTargetDetection` (AE 0x805090
  / SE 0x76B5D0 / 1.7 0x819EF0). That function writes the group CombatTarget's detectLevel / stealthPoints /
  kTargetLost. A negative level is its own lost path (AE 0x8054F2, per the CombatEntry.cpp header).
- The target selector admits only detected, not-lost candidates (H2). So a gated level also hides the follower
  from targeting, through engine logic.

**Candidate (gate the source).** A post-call `write_call<5>` on the sole CalculateDetection call site, on all
three runtimes. For a pair (Y observer, X target) that MFO declared unsensed, the out level becomes the engine's
own "not detected" value before the store and the dispatch.
- MFO's truth test (`SenseActor` plus a sight ray, main thread) publishes the pairs lock-free.
- The seat runs on the detection job worker, so this is tier A threading.

**Deny holes (re-acquire paths).**
- (a) ForceDetect, AE id 37765 0x68FA50. It stores level 100 directly through 0x7B1AF0. It has 9 callers: AE ids
  24715, 35294, 37422, 37423 (x2), 39667, 40076, 40654, 40814 (hit, alarm, StartCombat and similar). These are
  legitimate reveals. H-DETECT must yield on that pair until the truth test is unseen again.
- (b) The group's periodic detection pass, AE id 44804 0x80CE00, stores its own result (call at AE 0x80CF30) for
  low-process members. That is a second seat, or a declared hole.
- (c) Harbinger's held-engagement knowledge seats (ch.20 / ch.21) UN-lose a holder's target. Arbitrate on the
  existing `kDetection` sources facet.
- (d) The refresh cadence follows request priority. Measure it.
- (e) ch.16 AV writes stay separate (do not claim ch.16; it flattens the Thief perk).

**Probe.** The post-call seat logs once/s per (Y, X) pair when X is claimed: level, LOS / sound flags, and MFO's
verdict (would-gate). It also counts ForceDetect calls on the pair.

**Verdict.** FEASIBLE-WITH-RISK. **Estimate:** tier A, effort 5.

---

## 4. H5: ranged miss detection and correction (task 1dfcuabcqev)

**Launch.**
- Projectile::Launch: AE id 44108 0x7E46C0 / SE id 42928 0x74B170 / 1.7 0x7F9300.
- AE callers: ids 13723, 13728 (no E8), 18102 = TESObjectWEAP::Fire 0x286C90, 34452 (magic), 36419.
- SE Fire is id 17693 0x235240, LIKELY: same call shape, beside GetFireNode 17689.
- WEAP::Fire AE callers: ids 22545, 36991, 42859, 56653.
- LaunchData (0xA8): shooter +0x28, desiredTarget +0x58, angleZ / angleX +0x48 / +0x4C, alwaysHit +0x9C, autoAim
  +0x9E.

**Aim.**
- The NPC aim point comes from the CombatProjectileAimController solve: AE id 44396 0x7F7C40 / SE id 43174
  0x75D7B0 / 1.7 (site 0x80CDFF). It reads iCombatAimMaxIterations (an iterative ballistic solve).
- It then adds a uniform random offset to each axis of the aim point (+0x8C / +0x90 / +0x94). The radius is
  `aimController+0xA0 x fCombatAimProjectileRandomOffset`, read at AE 0x7F80BF / SE 0x75DC81 / 1.7 0x80CDFF.
- **+0xA0 is written once, at aim-controller creation, from fCombatRangedAimVariance.** The writer is AE id 33135
  0x5557E0 (store at 0x5559B0) / SE id 32385 0x4FA040 (0x4FA232) / 1.7 at about 0x55D820 (same bytes at 0x55D80E).
  It is a per-controller copy of a global, so it is a per-actor input.

**Detection (no seat first).** Count shots from the follower's graph event (BowRelease / arrowRelease sink), or
from an observe-only seat on WEAP::Fire -> Launch. Match each to a TESHitEvent (cause = follower, target = the
shot's desiredTarget, projectile form) within the flight-time window. A miss is a shot with no hit on its target
in the window.

**Correction (gate the source).** Write the follower's own aim controller +0xA0 (a scale in [0, 1]) while the
class claim stands. Apply it at each creation (a post-call seat on the creation call is cleaner than polling), with
a per-call vtable identity check (the CastSeats pattern). The engine's own solve still aims. Never move or
teleport arrows.

**Holes / risks.**
- (a) Lead on moving targets: whether 44396 uses target velocity was NOT read. OPEN.
- (b) Obstruction misses (fAIAimBlocked*) are geometry.
- (c) A second +0xA0 store at AE 0x7F42CD (id 44312) is on a different object (a distance squared). Verify the
  class first.
- (d) The controller is rebuilt per engagement.
- (e) Spells use fCombatMagic*AimVariance (other code).

**Probe.** Per follower, one line per 10 s: shots, hits, misses, the live +0xA0 value and the aim-controller
vtable.

**Verdict.** FEASIBLE-WITH-RISK. **Estimate:** detection tier B, effort 2. Correction tier A, effort 4, plus 2 if
lead needs its own input.

---

## 5. G7: corpse target / reanimate (task 1dfcuabcqey)

**ch.8b and a dead target: NO.**
- `core/CastSeats.cpp:154` deliberately does not seat the Reanimate caster ("its own 0x06 corpse search, 0x0A and
  0x0D are what aim it").
- Reanimate is a placement spell, so ch.8b's target is 0 (self) and GetMagicTarget answers natively
  (`CastSeats.cpp:662`).
- ch.8b's CheckStopCast seat ends a claim whose target is dead (`CastSeats.cpp:731`).

**How the engine targets it.** `CombatMagicCasterReanimate` slot 0x06 (AE 0x822490 / SE 0x7860F0 / 1.7 0x837980;
AE and 1.7 identical over 109 insns):
- It keeps the cached reanimateTarget (+0x28) if it is still dead (+0xC8 & 0x1E00000 == 0x400000) and passes AE
  0x81E290 (id 45343, args: controller, corpse, magicItem).
- Otherwise it calls FindReanimateTarget: AE 0x822890 (call at 0x822596) / SE 0x7864E0 (0x78620D) / 1.7 0x837D80
  (0x837A86). The handle goes to +0x28 AND to aimController(+0x20)+0x30, the aim override ch.8b also uses.
- Slot 0x0A (AE 0x8222C0) returns {+0x28, null} through the hidden sret.
- **CORRECTION posted to ClickUp:** whether 0x81E290 checks the spell's conditions and level cap is UNPROVEN. It
  calls AE id 34554 0x5C52E0 and a vfunc 0xB0. Read it before relying on it, and keep G10 as the level check.

**Road A (tier B, today).** MFO's `cond.corpse_within` gates WHETHER. The action files ch.8b with the Reanimate
spell and target 0. EquipGate's 0x0F is seated on the MagicReanimate / StaffReanimate item vtables, so the spell
enters the hand, and the ENGINE picks the corpse.

**Road B (tier A, small).** A post-call seat on the FindReanimateTarget call. It answers the claim's corpse only
when the engine's own validity test passes, plus a ch.8b reanimate flag that does not end on a dead target.

**Holes.**
- The cached corpse is kept while it is valid, so Road B must also cover the keep branch.
- Another caster's raise on the same corpse.
- Staff reanimate shares the caster.

**Probe.** AiCastSeats Group B (`EnableCasterSeatProbe`) already observes Reanimate 0x06 / 0x0A. Add one line per
cast: the engine's corpse versus MFO's corpse.

**Verdict.** FEASIBLE: Road A. Road B only if A picks wrong in the field. **Estimate:** A is tier B, effort 3. B is
tier A, effort 2.

---

## 6. G11: ward on hostile charge (task 1dfcuabcqez)

**Fields PROVEN (not trusted from CommonLib).** On the hand caster (`ActorMagicCaster`, Actor `magicCasters[L/R]`):
desiredTarget (ObjectRefHandle) at +0x20, currentSpell at +0x28, state at +0x30.
- AE ActorMagicCaster vtable (id 205828 0x187B8B0) slot 1 (AE id 34404 0x5BB9C0): rdx = spell, r8b = bool,
  r9 = target. It stores handle(target) at +0x20 (0x5BBA06), the spell at +0x28 and 0 at +0x30.
- SE uses SetDesiredTarget 0x54F0E0 (the store at 0x54F108), called from slot 1 0x54C5F0.
- 1.7.104 has the AE byte pattern at 0x5CA523.

**State values.** Use the field-observed numbers (memory `observed-vanilla-cast-sequence`):
1 -> 2 Charging -> 3 Charged -> 4 Casting. CommonLib's enum names (kReady = 3, kCharging = 5) disagree with the
field.

**Other target sources for AI casts.**
- CombatMagicCaster 0x0A GetMagicTarget (by value through the sret; for Offensive it is the controller's target).
- The aim controller +0x30 override.
- Actor::currentCombatTarget +0x104 (identical on all three).

**Candidate: no seat.** A read-only MFO mirror (the SelfStatus shape: one `MainThread::Post` per tick). For each
engaged foe, read both hand casters and publish "F charges hostile S at me" when:
- the state is 2 or 3,
- S is hostile and not Self-delivery,
- desiredTarget == me, OR desiredTarget is empty and F's currentCombatTarget == me.
The ward goes through ch.8b (the Ward row is seated), held with Repoint heartbeats, and released when F's state
leaves 2/3 or its target changes.

**Open.**
- (a) Whether the AI fills desiredTarget for AIMED spells during the charge. The fallback is needed and the probe
  decides.
- (b) The magicCasters offset is version-dependent. Use the fork accessor.
- (c) AoE and cone splash near an ally is MFO geometry.
- (d) The ward's release is MFO's call.
- (e) Read on the main thread only.

**Probe.** For each engaged foe, log each transition into state 2: spell, delivery, desiredTarget,
currentCombatTarget, aim override. Max 1 per foe per 2 s.

**Verdict.** FEASIBLE. **Estimate:** tier B, effort 3. Not tier A.

---

## Open questions (for marth, none blocking)
1. H3: if the field probe shows the Return-To-Area run is a strafe or backpedal, is a facing-only tier-A seat
   (movement handler) in scope for M2, or is the strafe acceptable for mages and only Thieves need forward running?
2. H2: SE 1.5.97 needs a separate seat (IsValidTarget is inlined). Ship H2 on 1.6 / 1.7 first, with SE refused by
   name (per-runtime path), or hold it until the SE RE is done?
3. G2: the low-process group detection pass (AE 44804). Seat it too, or declare it a hole (it only matters for
   foes outside high process, which are usually far from the player)?
4. H5: if the engine's solve does not lead moving targets, is a second input (the target-velocity read) in scope,
   or is a scatter-only Ranger (1 miss in 10 on mostly static targets) enough?
