# Field diagnosis 1001: healing (session 2026-10-01 08:27-09:09)

Build: MFO 2.0.16 (86c9984), APMF 0.9.11 (tag v0.9.11, d9ff0ef; cloned to /tmp/apmf).
Main (0d353fc) changes no file cited below (`git diff --stat 86c9984 0d353fc -- native`: only Board*,
Config*, Forms.h, logistics/Lockpick.cpp), so every file:line also holds on main.

Actors: 750012C6 Jesper the Guard (the only healer; heal rule = combat rule 1, Fast Healing 0002F3B8
cast as APMF delivery-flip proxy FF001F02 at the player 00000014; offense rules 6 = Sun Fire 02003F52,
7 = Incinerate 0010F7ED). 0009BCB0 Cicero and 00015D09 Adelinda cast no heals (Cicero casts only his
own Absorb Stamina FF00212F). Enemy casters in the heal fight: 02008E2B / 02008E2D Vampire's Thrall
(0007231B, FE254806). No enemy cast is attributed to a follower below.

---

## 1. Timeline of heal-relevant episodes

| # | time | actor / hand | spell -> target | MFO intent | what the engine / APMF did |
|---|---|---|---|---|---|
| E1 | 08:53:34.574 | Jesper, OOC road (logistics rule 0) | Spring of Life FE20A898 -> player, d=1121 | OOC heal | refused on main: `no line of sight` (own ray OCCLUDED 08:53:34.573). No cast. |
| E2 | 08:54:01.637 | Jesper, OOC road | Spring of Life -> player, d=1352 | OOC heal | refused: no LoS (ray OCCLUDED 08:54:01.637). |
| E3 | 09:02:57.099 | Jesper, OOC road | Spring of Life -> Adelinda 00015D09, **d=198** | OOC heal | refused: no LoS (ray OCCLUDED 09:02:57.098). |
| E4 | 09:03:16.302 | Jesper, OOC road | Spring of Life -> player, d=2070 | OOC heal | refused: no LoS. |
| E5 (#7.1) | 09:06:03.279-05.938 | Jesper LEFT | Fast Healing via proxy FF001F02 -> player | rule 1 heal claim (ch.8b), R hand under IDLE-HAND FLOOR (03.275), no offense claim, no ch.6/ch.20 pin | seat 0x06 YES + BUILT Restore + NotifyStartCast all at +1200 ms (04.521); L state 1->4->6 (04.545/04.647/05.105); MFO CFC-fired 05.035; `[heal-obs] road=claim ... hp 53% -> 62% ... landed=effect+hp` (06.075). **Works.** |
| E6 (#7.4) | 09:06:13.786-28.868 | Jesper LEFT | Fast Healing via FF001F02 -> player (LoS VISIBLE 13.791) | rule 1 PREEMPTED L from Incinerate (13.786); the heal refresh is transparent, so rules 6/7 claimed the RIGHT hand (Incinerate #7.5 R 14.80, Sun Fire #7.6 R 18.89) and re-pinned the foe (ch.6/ch.20 02008E2B 14.73, FF001B4E 18.86); IDLE-HAND FLOOR released 14.727 "both hands driven" | BUILT Restore at +1627 (15.544); seat 0x06 YES **32 times** (census `seat06=32 (claim YES=32, native YES=0) seat0A=66`), yet NotifyStartCast only at **+11628 ms (25.540)**. During the wait the R hand charged and fired Incinerate (17.26 -> 18.047) and Sun Fire (21.76 -> 22.36, 23.10 -> 25.53). MFO `[cfc] ... NO observed cast (spell 0002F3B8, left hand)` 17.662 / 22.722 and `[heal] ... REPAIR ... no Prepare` 18.187 / 23.517 (log only). L reached state 4 at 25.631. |
| E6b | 09:06:26.034-26.199 | Jesper | (same in-flight heal) | auto-retreat `falling back -- confidence=0.21 foes=3 dPlayer=518` | ch.9 package swap (26.038, PACKAGE CHANGED 26.170) + StopCombat landed 26.199. The heal never reached state 6 (CLOSE #7.4 `hand max state 4=Casting`; #7.1 had `6=Concluding`). No MFO CFC-fired / `[heal-obs]` line for it. `[atk-obs]` fight#3 `InterruptCast=14`. Claim then died by expiry sweep 28.864 (`not refreshed for 2831ms`). |
| E7 | 09:06:26.0-41.9 | Jesper | none | retreat travel; gambit table and OOC service both skipped | no heal of any kind for 15.9 s; Jesper's own hpLoss 16.5%/s (28.864). |
| E8 | 09:06:43.5 / 47.4 / 51.0 | Jesper, OOC road | Spring of Life -> player | OOC heal | landed: hp 50->61%, 70->83%, 91->100% (`road=OOC FF ... landed=effect+hp`). Player was at 50%, i.e. LOWER than the 62% E5 left him at: E6's heal never landed. |

Not heal episodes (excluded on attribution): all 10 `[cfc]` WARNs except 17.662/22.722 name OFFENSE
spells (Sun Fire R / Incinerate L, 08:54:19-30 and 09:06:11) and the `CLASSIFY ... Heal Other /
Healing Hands ... not forced` lines are the classify rescue correctly ignoring spells the claim does not
drive (by design, APMF ch.8b seat 0). The `[ch.8b seat 0x0D] ... A kAimed heal now flies at the ally`
lines are a fixed message printed for every claim, offense included (targets are foes).

---

## 2. Distinct healing problems, ranked by user impact

### P1 (highest). The heal claim is starved while MFO's own lower-ranked offense rules drive the other hand

**What happens.** A heal claim that is standing but has not yet fired is reported "SATISFIED IN FLIGHT"
and returns TRANSPARENT, so the scan continues and rules 6/7 take the right hand and pin a foe. With both
hands driven (heal L, offense R) the engine answered the heal's CheckStartCast YES 32 times over 10 s
and did not start it until the right hand released Sun Fire (R Casting 25.531, heal NotifyStartCast
25.540, 9 ms later). With the right hand floored and no pin (E5) the same heal, same caster, same target
started at +1.2 s. The gambit list ranks the heal first; the result is a priority inversion: the top
rule waits ~10 s behind rules 6/7.

**Root cause (MFO, by design, wrong trade).**
- `native/cast/CastOn.cpp:472-519` (the F9 "SATISFIED IN FLIGHT" design: "The heal keeps the left hand,
  an offense rule below gets its lap and takes the right") and the transparent return at
  `native/cast/CastOn.cpp:684-686` (`LogCastInFlight` ... `{ Result::NoOp, "cast already in flight
  (claim refreshed)", true }`). It treats "claim standing" as "cast running"; for E6 the cast was not
  running, it was pending.
- Consequence path: `native/apmf/Bridge.cpp:787-797` drops the IDLE-HAND FLOOR when two driving
  claims exist, so the condition that made E5 work is removed by the offense claim.
- The stall detector is log-only: `native/cast/Hands.cpp:946-963` (`kNeverFired`) and
  `native/cast/CastOn.cpp:651-669` ("no Prepare: the heal or its proxy is in the left hand"). It
  correctly reports, but has no remedy for this shape. That is not a mask; it simply has nothing to do.

**Confidence: high** that the heal waited ~10 s with seat 0x06 answering YES and that MFO's concurrent
R-hand offense claims (+ foe pin) are the only difference from the working window. **Medium** on the
ENGINE mechanism that turns "other hand driven" into "heal not started" (see NEEDS LOCAL 4.1). The
same dual-drive pattern holds for offense pairs in this log: #5.1 R fired / #6.1 L NOT BUILT; #7.2 R
BUILT NOT FIRED / #7.3 L fired; single-hand windows #4.1 (R, fired x4) and #7.1 fire promptly.
(Series #3 at 08:54 is excluded as evidence: its foe 0004CEF4 was measured Occluded, which stops a
charge on its own.)

Proving lines:
```
MFO  [09:06:13.786] [eval] 750012C6 cast gambit PREEMPTED the left hand -- rule 1 (spell 0002F3B8) outranks rule 7 ...
MFO  [09:06:14.727] [apmf] 750012C6 IDLE-HAND FLOOR released (right hand) -- ... (both hands driven, or none).
MFO  [09:06:17.663] [eval] Jesper ... rules 1(cast already in flight (claim refreshed)),6(cast gambit locked -- both hands busy (left: spell 0002F3B8, right: spell 0010F7ED)),7(cast already in flight ...)
APMF [09:06:15.544] [ctcensus] 0x750012C6 BUILT type=Restore item=0xFF001F02 ... hand=L at +1627 ms
APMF [09:06:17.091] [ctcensus] 0x750012C6 BUILT type=Offensive item=0x0010F7ED ... hand=R at +2291 ms
APMF [09:06:18.047] [ctcensus] 0x750012C6 FIRED type=Offensive item=0x0010F7ED ... hand=R
APMF [09:06:22.359] [ctcensus] 0x750012C6 FIRED type=Offensive item=0x02003F52 ... hand=R
APMF [09:06:25.531] [castobs] ... CASTER[R] state=4(Casting) spell=0x02003F52
APMF [09:06:25.540] [ctcensus] 0x750012C6 FIRED type=Restore item=0xFF001F02 ... hand=L ... at +11628 ms (first build +1627 ms)
APMF [09:06:28.868] [census] ... restore casters seen=2 seat06=32 (claim YES=32, native YES=0) seat0A=66 | first seat 1627 ms / first claim YES 1627 ms / first hand engage 11685 ms
contrast E5:
APMF [09:06:04.521] [ctcensus] BUILT type=Restore ... at +1200 ms  /  FIRED type=Restore ... at +1200 ms
MFO  [09:06:03.275] [apmf] 750012C6 IDLE-HAND FLOOR claimed (right hand, deny-only)
```
Score steer is NOT the cause: the heal proxy scored 2890 (steered) against 1835 / 2650 for the offense
items every lap (`[aicastseats] SCORE-STEER`), and seat 0x0F denied every competing item on L.

**What would disprove it.** A field window where a heal claim stands on L with an offense claim
driving R and a foe pin live, and the heal still starts within ~1.5 s of its first seat-0x06 YES; or,
after the fix below, heals with the R hand floored still showing multi-second YES-without-start (then
the blocker is elsewhere, e.g. aim/facing on the heal's own context).

### P2. MFO's auto-retreat interrupts the in-flight heal and then suppresses all healing for ~16 s

**What happens.** The E6 heal finally started at 25.540 and was in state 4 at 25.631. At 26.034 MFO
filled the retreat: APMF ch.9 swapped the package (26.170) and the single StopCombat landed (26.199).
The heal never reached Concluding (E5 went state 4 -> 6 in 458 ms; E6 had 568 ms and stopped at 4), no
SpellCast event reached MFO, no `[heal-obs]` was written, and the player is at 50% at 09:06:43 against
62% after E5. Then for the whole retreat (26.0-41.9) neither the combat gambit table nor the OOC
service runs, so no heal of any kind is attempted while the player is still in combat (player
`inCombat=1` at 28.864) and at ~50%.

**Root cause (MFO).**
- `native/Scheduler.cpp:1288-1303`: the retreat trigger (`IsInCombat && dPlayer > kRetreatMinDist(400)`
  and `Confidence::Of < 0.25`) has no test for the actor's own cast in flight; `Packages::RetreatFill`
  runs mid-cast. MFO's own heal rule already states the principle "a cast in flight finishes" (D8,
  `CastOn` per-lap recipient check, MAP "ANIMATED HEAL CLAIM ROAD"); the retreat does not honour it.
- `native/Scheduler.cpp:1274-1283`: while `ServiceRetreat` is active the tick returns before the gambit
  table and deliberately skips `serviceOwnOoc()`. So healing is off for the retreat's duration by
  design. Whether a retreating healer should still heal is a DESIGN question for marth, not a defect;
  the interrupted cast is a defect.
- Secondary: the heal claim is not released by the retreat, it dies by the FacetExpiry sweep 2.8 s later
  (`native/apmf/Bridge.cpp:1009` line text), during which seat 0x06 still answers for a claim no rule owns.

**Confidence: high** that the cast was stopped before delivery and that the retreat fill is the event at
that moment; **medium** that the StopCombat (rather than the package swap 30 ms earlier) is what
interrupted it. Either is MFO's own retreat.

Proving lines:
```
APMF [09:06:25.631] [castobs] ... CASTER[L] state=4(Casting) spell=0xFF001F02
MFO  [09:06:26.034] [retreat] 750012C6: falling back -- confidence=0.21 (pre-StopCombat) foes=3 dPlayer=518
APMF [09:06:26.170] [obs] 0x750012C6 pkg=0xFE06883A(Package) [PACKAGE CHANGED!!]
MFO  [09:06:26.199] [retreat] 750012C6: StopCombat (engage (ch.22 deny live)) landed on main (wasInCombat=true)
APMF [09:06:28.868] [ctcensus] CLOSE #7.4 ... VERDICT: FIRED via Restore (... hand max state 4=Casting ...)
     (vs CLOSE #7.1 ... hand max state 6=Concluding)
MFO  [09:06:27.934] [atk-obs] 750012C6 'Jesper the Guard' fight#3 ... InterruptCast=14 ... [combat end]
MFO  [09:06:28.864] [heal] 750012C6: heal claim RELEASED by the expiry sweep ... not refreshed for 2831ms
MFO  [09:06:41.909] [retreat] 750012C6: ... ending (travel after 15.9s, moved 1556, dPlayer=1650)
MFO  [09:06:43.521] [heal-obs] 750012C6 -> 00000014 Spring of Life ... hp 50% -> 61%
```
No MFO `[cast] ... CFC-fired (unnamed) (FF001F02)` exists after 09:06:05.036.

**What would disprove it.** A CFC-fired FF001F02 / heal-obs line for the 25.54 cast in another log
sink, or a castobs L state 6 for FF001F02 between 25.63 and 26.2 that this log dropped.

### P3 (lower, partly by design). OOC heals refused for "no line of sight", one at 198 units

The four OOC refusals (E1-E4) are the designed main-thread re-check (`native/cast/CanAct.cpp:228-251`,
`RefuseHealApplyOnMain` -> `Sightline::MeasureNow(..., Basis::Own)`), and each is preceded by an
`OCCLUDED (own ray)` line a few ms earlier, so the refusal itself is consistent. E1/E2/E4 at 1100-2100
units in a fight are plausibly genuine. E3 (Adelinda, 198 units) is suspect: the ray
(`native/Sightline.cpp:103-163`, `CustomRay`) collides on layer `kCharController` from the eye to three
target points and calls the pair occluded only if all three hit, so another actor's capsule standing
between (player / Cicero) would occlude a teammate at arm's length. The log cannot say what was hit.
Note also the pick/refusal disagreement: logistics rule 0 fired on a target the main-thread ray then
refused, every time (the pick trusts a cached verdict, the apply re-measures). The refusal is correct
behaviour (principle: no heal through walls), the cost is a 3 s refresh lost each time.
**Confidence: low** that anything is wrong here. NEEDS LOCAL 4.3.

### Not a healing problem (checked and excluded)
- `[bleed] ... DOWN (?(9) ...) hp 100% ... no MFO heal landed on him while down` (Cicero 08:40:13,
  08:56:08; Jesper 08:40:16, 08:40:38): raw life state 9 appears during IdleGive / IdleLockPick
  (APMF ch.12 lines at the same times). `NoteUnknownLifeState` says "not a down state" but `LifeUp`
  (`native/cast/CanAct.cpp:88-90`) counts only Alive/Reanimate as up, so the transition logger prints DOWN.
  Misleading diagnostics, no behavioural effect seen (0 `cannot act` refusals). See section 5.
- The `[cfc]` WARNs on Sun Fire / Incinerate are offense claims against a foe measured Occluded,
  released by MFO's own rule 6/7 (`forced offense cast ... RELEASED ... foe measured Occluded`), by design.

---

## 3. Fix shapes

| # | repo | file | shape | tier |
|---|---|---|---|---|
| P1 | MFO | `native/cast/CastOn.cpp:472-519/684-686` (in-flight refresh) plus the offense hand pick it feeds (`resolveHands` `:463-470` / `ResolveCastHand`, defined `cast/Hands.cpp:1075`) | Distinguish PENDING from RUNNING for a heal claim. While the standing heal claim has not been observed firing since it was stamped (the same test `HealClaimNeedsRepair` already makes: `ComposedCast::ObservedFiring` over `now - lock.lastSeen`, or `CastInFlightOnHand` false), lower-ranked OFFENSE rules must not take the other hand or re-point ch.6/ch.20: report them held ("heal pending on the left hand"), so `Bridge.cpp:787` keeps the IDLE-HAND FLOOR on the right as in E5. Once the heal is observed firing (or the claim is released / capped), the existing transparent behaviour resumes. Rank-respecting composition, not a retry or watchdog; the `kNeverFired` WARN stays as the loud signal. Touches MAP "ANIMATED HEAL CLAIM ROAD" / F9 text. | **B** (logic inside the existing hand-lock / in-flight mechanism; no seat, no ABI). |
| P2a | MFO | `native/Scheduler.cpp:1288-1296` (retreat trigger) | Do not fill the retreat while the actor has its OWN cast in flight on a hand (heal claim observed in a live cast state, `CastInFlightOnHand`); defer to the next sense lap (the cast concludes in < 1 s). Release the heal claim explicitly when the retreat fills instead of leaving it to the FacetExpiry sweep. | **B** |
| P2b | MFO | `native/Scheduler.cpp:1274-1283` | DESIGN question for marth: should a retreating follower still run heal rules (at least on the player / self) during the retreat travel? Today all healing is off for the retreat's duration. No code change without his decision. | B if chosen |
| P3 | MFO | `native/Sightline.cpp:103-163` | Only if NEEDS LOCAL 4.3 shows the d=198 hit is a teammate capsule: exclude actor capsules of party members from the heal LoS ray (or use a static-geometry layer). Do not drop the refusal. | B |
| bleed | MFO | `native/cast/CanAct.cpp:88-90` / NoteLifeState | Make the unnamed raw value(s) the transition logger calls DOWN agree with `NoteUnknownLifeState` (treat 9 as up, or stop claiming "not a down state"). | C (diagnostic text/logic only) |

Nothing here needs APMF changes for the fix itself.

---

## 4. Cannot be concluded from these logs (NEEDS LOCAL)

4.1 **The engine mechanism behind P1.** APMF `native/core/CastSeats.cpp:262-266` says the cast leaf
(0x89f3c0) checks `currentSpell == null`, `IsCastingSourceReady` and the not-dual test BEFORE it calls
seat 0x06, so 32 YES answers mean those passed. What stops the leaf from starting the cast after a YES
is unknown: (a) the combat behaviour runs one magic cast leaf per actor and the R offense leaf holds it;
(b) facing/aim: the actor is turned to the ch.20-pinned foe while the heal's aim context targets the
player; (c) something else after the YES. Settling log: an APMF probe on the consumer of
Restore::CheckStartCast's YES (what the leaf returns / which branch it takes next), castobs lines for
EVERY hand-state change including 0 on both hands, and the actor heading vs. bearing to the claim
target at each YES. Needs the 1.6.1170 disassembly of the leaf after 0x89f3c0's seat call and a Deck
test (heal claim with R floored vs. R driven, pin on vs. off). The P1 fix is correct under (a) and (b)
alike, and its field result discriminates them.

4.2 **Which retreat step killed the cast (P2):** package swap at 26.170 vs. StopCombat at 26.199.
Settling log: a castobs line with the L state transition (4 -> 0 / interrupt) timestamped against both.

4.3 **What the d=198 ray hit (P3).** Settling log: on an `OCCLUDED (own ray)` verdict for a HEAL
recipient, log the hit body's owner (`pick.rayOutput` collidable -> TESObjectREFR form id / layer) per
sample.

4.4 **Player HP when E6 was claimed (13.786) and between E5 and E8.** `HealObsClaimLap` stamps it but
does not log it; heal-obs logs only on a fire. Settling log: one line per heal claim open with the
recipient's HP and the rule threshold.

---

## 5. Non-heal anomalies worth noting

- `[travel-leg] ... ABANDONED ... BLOCKED` x4 (APMF errors): 08:40:16 Cicero (static block), 08:40:41 and
  08:55:48 Jesper (TEAMMATE 0009BCB0 in front), 09:09:13 Adelinda. The message says the ch.19 claim
  then does nothing until the client releases or repoints it.
- `[retreat]` moved Jesper AWAY from the player: start dPlayer=518, end dPlayer=1650 after moving 1556
  (09:06:41.909). The retreat's stated intent is to fall back to the player. Not concluded (the player
  may have moved); worth a look with the retreat destination logged.
- `[pkg] 750012C6: package still running after 12.0s -- hard timeout` x2 (08:54:24, 08:54:47) during
  the forced Inferno (80022DAB) casts held for no LoS.
- `[bleed]` DOWN/UP pairs at hp 100% on raw life state 9 during IdleGive / IdleLockPick (section 2).
- `[meo] reconcile LEFTOVER ... off-domain` x104 (26 each for 4 enchant forms on Jesper / Cicero):
  repeating every pass, loud by construction, likely benign.
- `[loot]` DEADLINE EXPIRED x2 and TRAVEL PKG NOT ENGAGED x1 (08:55:38, 09:09:13, 09:09:31).
- Backlog note MFO-B195 (no field log of the held-charge state value): this session records held
  charges as `state=3(Charged)` (APMF castobs, e.g. 09:06:03.111 R Incinerate; ctcensus `hand max
  state 3=Charged` with MFO's `fully charged ... for 17856 ms` 08:54:33). That is the evidence B195 asked for.
- MFO-B196 (non-urgent heal waits on a kReady offense incumbent, SAME hand) is a different shape from
  P1 (OTHER hand), but both are rank inversions on the heal; drain them together.
