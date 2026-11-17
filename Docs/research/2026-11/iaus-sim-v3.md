# IAUS simulator v3: a positional world (ClickUp 1dfcuabd44c)

marth 2026-11-13: "We've proved the sim doesn't work without distance and movement".

Branch `feat/sim-v3`. Only `tools/iaus_sim` and this note change. `native/` is not touched.
Read `iaus-sim-v2.md` first: v3 is v2 plus the mechanisms below, each behind one switch.
Labels used in every table: **FACT** = read from the game's code at the cited line. **CALIBRATED** = a number
measured in a field log. **MODEL** = our reading of engine behaviour with a source for its shape.
**ASSUMPTION** = no source, a labelled guess with a knob, listed again in section 9 for the reviewer.

## 1. What v2 already had, and what it was blind to

A finding that changes the brief's premise a little, so it is said first. v2 is not a world without positions.
It has x and y per body, facing, circle occluders, a line of sight test, run speeds, melee reach, caster and
archer standoffs, the ch.23 leash, flee and the back-off runs (`World.h` `Brain`, `Move`, `Sees`).
What it lacked is the list below. Each row is one of the field failures.

| Field failure | What v2 did | Why v2 could not see it |
|---|---|---|
| Jesper (Evoker) is handed a dagger, gets the melee style, closes 529 to 209 units, goes down | v2 DID let the posture layer hand an Evoker his dagger: 55 to 80 percent of his time with the blade drawn, 40 percent in contact (section 8.1). `ClassGate::casterLine` was never set in the sim, so the sim still ran the rule from before 11-13 | Nothing punished it. Every chosen cast fired 0.5 s after the pick, with a blade in his hand, through his own party, at point blank. Blade damage came on top. So the tuner read the dagger as free damage |
| The chase cap drops a circling dragon | `Policy.h:78` compared a 2D distance with a flat 4096 | The game's cap is 600 to 3000 by confidence, measured in 3D, with a waiver for a held foe. A dragon in v2 was a flag, with no height and no flight path |
| Line of fire refusals and occlusion drive most missed casts | An occluded release was counted, and nothing else | No corridor test existed. A cast through the player's back always flew |
| Knight blocking only measured under a 2+ foe taunt | `guard2%` | A guard stopped a blow from any direction, and only the taunted case was tallied |
| Cast delivery (inactive controller, claim thrash, hand locks) | none | A cast was one 0.5 s animation that always happened |

## 2. The switches

`--world v3` is the default. `--world v2` turns every v3 switch off and the lab is v2 draw for draw
(`run.sh` pins the three exact v2 numbers with it, and `baseline_builtin.txt` still compares with 0 changes).
Each mechanism is one switch of `Sim::V3` (`Data.h`), `--v3 <name>=on|off[,...]`:

| Switch | Mechanism | Section |
|---|---|---|
| `height` | z per body, 3D distance, the dragon's circuit, a perched foe, the game's OutOfMeleeReach | 3.1 |
| `chase` | the confidence chase cap in 3D and the held-foe waiver (`--waiver none|self|player`) | 3.2 |
| `lof` | the line-of-fire hold | 4.1 |
| `delivery` | claim to fire latency, the inactive controller, claim thrash (`--delivery main|old`) | 4.2 |
| `hands` | the hand a cast takes, the channel that holds it, the two-handed weapon put away | 4.3 |
| `stance` | the engine's movement by stance for followers and foes, attack slots around a target, strafing | 3.3 |
| `casterhold` | the native rule of 11-13 mirrored: `ClassGate::casterLine`, `Snapshot::spellsHoldPrev` | 3.4 |
| `blockdir` | a guard has an arc, blows are tallied by direction for every fight | 5 |
| `areas` | an active area lives in the world: moves with the caster, or fixed at the cast point | 6 |
| `perks` | `Snapshot::perks` spell group | 7 |

A v3 switch never draws from the fight's stream `R` when it is off. Draws that v3 adds (flight path, stream
length, slot angles) come from a second stream `Fight::V` seeded from the fight seed, so one v3 switch does not
shift the draws of another mechanism.

## 3. World and movement

### 3.1 Height (`height`)

| Row | Value | Level | Source |
|---|---|---|---|
| Distance | 3D between the two origins whenever either z is not 0 | FACT | `iaus/Candidates.cpp:344` `GetPosition().GetDistance` and `Combatant::dist` |
| Out of melee reach | airborne and abs(dz) > body height 120 + fMeleeReach 200 | FACT | `CombatSense.h:211-221`, `kNominalBodyHeight`, horizontal distance is not part of it |
| A dragon's ground time | 15 s | MODEL (v2, kept) | iaus-sim-v2.md model table |
| A dragon's air time | T = U(12, 20) s (v2: 12 s) | ASSUMPTION | long enough to fly the circuit below at a plausible speed |
| Flight path | a circuit around the party's centroid at take-off: radius r(t) = r0 + (rFar - r0) sin^2(pi t / T), half a turn per air phase (he lands on the far side), altitude ramps to `alt` in 1.5 s | ASSUMPTION | two field figures give the size: "2184 to 13242 u" (ClickUp 1dfcuabd44c) and "an airborne Alduin 5-11k away" (`CombatSense.h:124`). rFar = U(4000, 11000), alt = U(900, 2400) |
| A perched foe (scenario `ledge-archer`) | stands at z 350, not airborne | MODEL | the game's OutOfMeleeReach is false for it (not airborne), so IAUS offers Attack and the swing can never land. The sim shows the same thing: he runs under the ledge and whiffs |

### 3.2 Chase cap and the waiver (`chase`)

Mirror of `Confidence::ChaseRadius` (`Confidence.h:213`), computed per decision lap:

- cap = fChaseMin 600 + Of x (fChaseMax 3000 - 600) (FACT, `Config.h:1239`).
- Of = Vitality x Trend x FoeMultiplier(load), clamped to 0..1 (FACT, `Confidence.h:174`).
- Vitality = 0.75 health share + 0.25 stamina share (FACT, `:58`).
- FoeMultiplier(L) = clamp(0.90 / (1 + 0.25 L), 0.15, 0.90). L = sum over live foes of
  clamp(sqrt(level ratio x foe health / his max health), 0.5, 2.0) (FACT, `:64`, `CombatSense.h:81`).
- Trend = clamp((health share / loss rate) / 20, 0.30, 1), 1 when the rate is under 0.002 per second. The rate
  is the net loss over a 5 s window of his own service samples, one burst capped at 0.25 (FACT, `:95-157`).
  The sim keeps the same ring per follower (samples at his own serves, 0.25 s minimum step).
- A foe past the cap is not in the snapshot at all (FACT, `Candidates.cpp:345-353`), unless the waiver holds:
  Harbinger holds the engagement on it (the sim: it is his `steer`, the act.attack pin) AND it targets him or
  the player. `--waiver self` is the game of 10-19 (it must target HIM). `--waiver none` is no waiver.
- With the switch off the cap is the flat 4096 of v2.

So a healthy follower in a dragon duel reads Of = 1 x 1 x 0.6 and a cap of 2040 units. That is the number the
field figure starts at (2184).

### 3.3 Stance and engine movement (`stance`)

| Row | Value | Level | Source |
|---|---|---|---|
| A follower's stance | blade or silver drawn: melee. Bow: ranged. Hands on spells or a staff: cast while a kept offense spell is affordable, else MELEE | FACT | `scheduler/CombatScan.cpp:1512-1576`: an equip order sets the stance, else the cast latch "magicka >= cost, else (dry) Melee" |
| The class style follows the stance | the package style of that stance | FACT | `CombatStyle.cpp:156`, `classes/Package.cpp:29` (v2 had it) |
| A foe steps back after a blow | chance = CSCR fallback mult, 0.5 to 1.5 s | MODEL | v2 `--foe-styles`, now on with the switch |
| Attack slots | two or more melee attackers on one target approach points 50 degrees apart on a ring around it, the first one straight on | ASSUMPTION | the engine spreads attackers around a target. The angle has no source |
| A caster strafes when his cast is held | on each 0.25 s of a hold, with chance min(1, 2 x CSLR strafe mult), he side steps 160 units across the line, away from the friend in his way | ASSUMPTION | `Docs/ENGINE_NOTES.md:1165` (casters strafe and close while casting). Sensitivity in 12.3: without it most held casts never fire |

Circling by CSCR circle mult is NOT modelled (it would be a third assumption about the same thing the slots already do).

### 3.4 Casters stay on spells (`casterhold`)

`ClassGate::casterLine` is set for a Mage-line class and for the Mage T1 root (FACT, `iaus/SpellBook.cpp:677,688`)
and the last decision's `Generated::spellsHold` is fed back as `Snapshot::spellsHoldPrev` (FACT, `Candidates::Build`).
Off = the game before 0ed9c705: the posture layer may hand a caster his dagger. This is the "old rules" arm
of the Jesper validation.

## 4. Cast delivery

Every cast a follower's decision fires goes through the stages below, and every stage has a tally
(`dl.*` metrics, section 8). The tuner now optimises what is delivered, not what is chosen.

### 4.1 Line of fire (`lof`)

| Row | Value | Level | Source |
|---|---|---|---|
| Corridor | a friendly within 80 units of the segment caster to target, 3D perpendicular, only between the two (t in 0..1) | FACT | `Sightline.cpp:429-440` `kFireLinePad = 80` |
| Area clause | or a friendly within area + 64 of the aim | FACT | `APMF_API.h:801-806` |
| Who is a friendly | the player and the active followers. Not summons | FACT for MFO's list (`Sightline.cpp:449-477`), ASSUMPTION for Harbinger's own list |
| Which casts | every hostile cast that is not Self delivered | FACT | `APMF_API.h:801`, `CombatScan.cpp:1078`, `bFriendlyFireHold` ships ON |
| When | at the start and at the release. A held cast does not fire and is not paid for | FACT (0x06 start, 0x0A release) | same |
| A held stream | each 1 s beat is tested, a blocked beat does nothing | FACT | `cast/DirectTarget.cpp:234-239`, `kConcApplyPeriod` |

### 4.2 Claim, build and fire (`delivery`)

| Row | Value | Level | Source |
|---|---|---|---|
| Which road | a fire-and-forget cast is CLAIMED and the follower's own AI performs it (`ComposedCast::Try`). A concentration spell goes down the DIRECT road (1 s beats), with no claim to wait for | FACT | `Docs/CAST-DELIVERY.md` "WHICH PATH IS PRIMARY", `cast/Roads.cpp:200-213` |
| Claim to fire, offense | 2.4 s from the pick to the release (the charge time is inside it) | CALIBRATED, 1 session | `cast/Hands.cpp:362`: "2.3-2.5 s is an offense cast's claim to FIRE (Firebolt, DIAG 2026-09-06)" |
| Claim to fire, heal | 4.5 s | CALIBRATED, one cast, soft | same comment: "4.5 s ... for the one heal ... treat it as soft" |
| Self buff, summon, ward | as offense | ASSUMPTION | no measurement |
| Controller inactive | from the fight's start until a foe first stands within 2000 units in his sight, or he is hit | ASSUMPTION | the shape is FACT (`STATUS.md` fix/heal-claim-gates: Harbinger builds the controller INACTIVE until the engine's detection activates it). The 2000 is ours |
| A cast picked while inactive, `--delivery main` | takes the direct road: fires after 1.0 s | FACT for the road (`ComposedCast.cpp:785-849`), MODEL for the 1 s (CAST-DELIVERY.md "Posted, about 1 s") |
| The same, `--delivery old` | the claim stands with no cast until the 6 s hold cap, the hand is locked for it | FACT | `Hands.cpp:369` `kInFlightHoldCap`, field "heal claims lived 3.5 to 9 s with no cast" |
| Claim thrash, `--delivery old` | while a claim waits, a different non-heal cast picked on a later lap takes the hand and the wait starts again | FACT | `STATUS.md` fix/iaus-claim-thrash: "swapped the left hand 22 laps in 10 s with 0 fires" |
| Claim thrash, `--delivery main` | a fresh incumbent keeps the hand | FACT | the same fix, merged |

While a claim waits his feet move (Q-SIM-5) and his body does nothing else.

### 4.3 Hands (`hands`)

| Row | Value | Level | Source |
|---|---|---|---|
| Which hand | a self cast, a heal, a buff and any concentration spell take the LEFT hand. Offense at a target takes a free hand, never the weapon hand | FACT | `cast/Actuation_internal.h:162-165` |
| A weapon that needs both hands (bow, crossbow, two-hander; a staff is one-handed) | is put away for the cast and drawn again after it: + 0.65 s | FACT that it is denied (field 11-12, Auri), ASSUMPTION for 0.65 s (half of the v2 swap time) |
| A shield | is off the arm while a cast holds the left hand: no guard for that time | MODEL | the left hand holds the spell |
| Stream length | offense U(2, 6) s, heal and utility U(8, 15) s, a heal stops when the recipient is full | FACT | `Actuation_internal.h:115-119` `DrawConcCap` |
| A stream's effect | per second: the record's magnitude and cost each 1 s beat (v2: a 2 s lump) | FACT (`kConcApplyPeriod = 1.0`) | |
| `Snapshot::handBusyLeft` | true while any stream of his stands on the left hand, heal or not | MODEL | the game fills it for a heal only (`Candidates.cpp:272`). See section 10 item N2 |
| Tally | seconds a weapon was denied by a cast, and the weapon damage that time would have dealt at his measured rate | | the input of the weapon-hand-lost curve (ClickUp 1dfcuabd22x) |

## 5. The guard has a direction (`blockdir`)

- The guard stops a blow only when the attacker stands inside the arc in front of him: 90 degrees to each side
  of where he faces. ASSUMPTION: the engine's block angle is not in `Docs/ENGINE_NOTES.md` or any research note.
  Knob `--block-arc <degrees>`.
- A follower behind his guard keeps facing his combat target (MODEL).
- Every melee blow that lands on the subject is tallied, taunt or not: how many met a guard, how many came from
  outside the arc, how many found the guard down by timing. Metrics `guardAll%`, `arcMiss%`, `inc/f`.
- Timing was already in v2: a guard counts only when it is up at the hit frame. The held guard's 2.5 s, one
  renewal and 1.5 s rest are FACT (`apmf/BlockClaims.cpp`).

## 6. Active areas (`areas`, for ClickUp 1dfcuabd246)

An `Area` lives in the fight from the cast until its duration ends: centre, radius, owner, hostile or
protective, and whether it MOVES WITH THE CASTER (a cloak) or stays FIXED at the cast point (a circle, a rune,
a wall). Each second it applies its effect to whoever stands in it.

- Which spell makes one. A cloak: a Self delivered effect of archetype Cloak (35), read from the world extract
  (FACT, `Data.h` `kArCloak`). It moves with him. A circle: a Self delivered spell with an area whose editor id
  holds "Circle" (MODEL: the hazard archetype is not in the extract, section 10 item N5). It stays where it was cast.
  Runes and walls are dropped as Placement before IAUS sees them (`iaus/Prune.h:70,93`), in the game and here.
- Fixtures (`Load.h` `AreaFixtures`, by name only, used when the loaded world has no spell of that shape):
  FixFlameCloak (moves, 8 health per second, 60 s, 10 ft), FixCircleOfProtection (fixed, +60 armor to friends
  inside, 60 s, 20 ft) and FixChannelledStride (a concentration self buff, for the weapon-hand scenario).
- Tallies: seconds an area of his was up, foe seconds inside his hostile area, ally seconds inside it, seconds
  he stood outside his own protective circle, recasts while one was still up.
- IAUS has no consideration for a live area. The sim only measures the cost today. Section 10 item N3.

## 7. Perk-effect mirror (`perks`, perk-effects.md section 9)

`Policy.h` fills `sn.perks.spellsRead` and, per School identity, the MEAN over the spells he knows of that
school of the lab's ModSpellCost and ModSpellMagnitude multipliers, plus `spellCount`. The multipliers are
recorded per spell when the follower is built (`Load.h`, the same perk chain that already scaled cost and
magnitude). `c.spell_perks.py0 / py1 / py2` and `c.perks.*` are searchable with `--opt` as any curve point.
Off = `spellsRead` false, the consideration is not applicable, the score is the one before.

## 8. Oracle and scenarios

- The oracle gets one new extended action: `ext:clear_line`, a short side step to the nearest point from which
  the line to his target is clear of friends (and of cover). It is the evidence for a "step for a clear line"
  family, which the catalogue lacks.
- Utility is unchanged. One objective for v2 and v3.
- New scenario group `field` (by name or `--scenario field`, not part of the headline J so the headline keeps
  its composition): `jesper-dagger`, `dragon-circle`, `fire-lane`, `knight-ring`, `bow-channel`, `ledge-archer`,
  `cloak-area`.

## 9. What is calibrated, and what is assumed

Calibrated from field logs: claim to fire (two measurements), everything v2 already calibrated (pump, bleedout,
hit cadence, engine cast gap).
Assumed, each with a knob, in the order the reviewer should distrust them:

1. Claim to fire 2.4 s for every non-heal cast and 4.5 s for a heal (`--deliver-sec o,h`). Two field points.
2. The block arc of 180 degrees (`--block-arc`).
3. The dragon's circuit (rFar, altitude).
4. Attack slots 50 degrees apart, circling rate.
5. The controller is inactive until a foe is within 2000 units in sight.
6. The re-draw of 0.65 s.
7. Friendlies for the line of fire are the player and followers, not summons.

What a field log would settle, and the log line that carries it: the share of chosen casts that fire
(`[iaus]` pick against `[cast]` fire), the line-of-fire hold share (Harbinger's own log, MFO prints nothing),
the dragon's distance series (`[sense]` chase lines), the block arc (a probe).

## 10. Changes the game would need (NOT made, listed per the brief)

- N1. The posture layer has no "spells" candidate. The 11-13 gate is the interim. The sim can tune one when it exists.
- N2. `Snapshot::handBusyLeft` is filled for heals only. A weapon-hand-lost consideration (1dfcuabd22x) needs:
  the stream's expected length, whether his fighting weapon needs that hand, and his weapon damage rate.
- N3. A live area (1dfcuabd246): a Snapshot list of his active areas (centre, radius, moves or fixed, time left).
- N4. A "step for a clear line" road for a caster whose cast is held (the bow has the hidden step only).
- N5. `tools/list_balance` extract: the hazard archetype (a circle is a spawned hazard) so section 6 does not read an editor id.
- N6. FOUND BY THE MIRROR, verify in native before acting: the waiver keeps a held foe in the snapshot, but
  `Generate.cpp:135-136` still gives act.attack the chase cap as its reach, so the range curve vetoes it past
  1.15 x the cap. Then no candidate names it, the decision goes idle, and the pin that the waiver needs is no
  longer renewed. In the lab the dragon is dropped on 21 percent of the laps even with `--waiver player` (12.2).

## 11. Validation plan

v3 must reproduce each field failure with the rules and tunings of the day it happened, then score the
shipped tunings. The numbers are in section 12.

| Failure | Arms | Reproduced when |
|---|---|---|
| Jesper with the dagger | `casterhold` off against on, in v2 and in v3 | v3: the dagger arm is clearly worse (downs, damage taken, U). v2: it is not |
| Circling dragon | `--waiver none`, `self`, `player`, and v2 | laps with the dragon dropped: near 0 in v2, high with none / self, low with player |
| Missed casts | v2 against v3 on `fire-lane` and the natural bracket | fired share of chosen casts under 100 percent, with holds the largest cause |
| Knight guard | `knight-ring` and the natural bracket | a guard figure exists outside a taunt, and blows from outside the arc appear |
| Delivery | `--delivery old` against `main` | claims with no cast and thrash appear with old, and are gone with main |

Then: the shipped tunings on the v3 world, per class, against their v2 score (no retune here).

## 12. Results (built-in world, vanilla profile, shipped tunings of origin/main 0468bc3c)

Commands: `tools/iaus_sim/run.sh --scenario <name> --fights 300 [arm]` for 12.1 to 12.5, and
`--classes beta1 --seeds 1,2,3,4,5,6` for 12.6 and 12.7. The natural-bracket rows are from
`tools/iaus_sim/baseline_v3_builtin.txt` (`--classes beta1 --baseline`, 8 fights x 5 ranks per cell).

### 12.1 Jesper with the dagger: REPRODUCED, with one honest limit

`jesper-dagger`: an Evoker (level 25) with a dagger, a melee player, three bandits that start on him.

| Arm | U | downs / fight | taken / s | blade drawn | in contact | cast dps |
|---|---|---|---|---|---|---|
| v2, old rule (the sim until today) | 89.23 | 0.09 | 7.50 | 58.5 % | 58.1 % | 20.3 |
| v2, the 11-13 rule | 90.53 | 0.00 | 3.53 | 31.8 % | 48.5 % | 25.2 |
| v3, old rule (`--v3 casterhold=off`) | 85.57 | 0.11 | 3.92 | 37.3 % | 37.5 % | 5.4 |
| v3, the 11-13 rule (default) | 87.63 | 0.00 | 2.64 | 4.2 % | 30.0 % | 6.9 |

With the foes on him the dagger arm goes down 0.11 times a fight against 0.00, and costs 2.1 U in v3
(1.3 U in v2). The nearest he comes to his target is 143 units in the dagger arm (field: 209).
The limit: v2 shows the same sign in THIS scenario. What v2 could not do is show it where the tuner looks.
In the natural bracket and the headline scenarios the v2 Evoker stood 40 percent of his time in contact with
his blade out and still went down in 5 percent of fights, so no search ever saw a cost. That part does not
change in v3: over the six caster cells the rule is worth nothing on average (12.7, `casterhold=off` reads
79.96 against 79.11, about one and a half standard errors the WRONG way). The reason is in 12.3: when four
casts in five fire 2.4 s late, a dagger is extra damage. So the dagger is punished when blades reach him and is
still read as useful in an average fight. That is a statement about the world model's foes (they rarely choose
the caster) as much as about the rule, and it is the first thing a field log should check.

### 12.2 The circling dragon: REPRODUCED

`dragon-circle`: an Evoker (level 30), a ranged player, one dragon that fights the player.

| Arm | laps with the dragon dropped | mean cap | kept by the waiver / fight | idle laps |
|---|---|---|---|---|
| v2 | 0.00 % | 4096 (flat) | 0 | 0.0 % |
| v3 `--waiver none` | 36.57 % | 2099 | 0 | 23.8 % |
| v3 `--waiver self` (the game of 10-19) | 26.70 % | 2070 | 30.4 | 18.9 % |
| v3 `--waiver player` (the game today) | 21.06 % | 2058 | 47.2 | 15.9 % |

The cap reads 2058 to 2099 units against a dragon, the number the field figure starts at (2184). v2 never
dropped it. The fix of 11-13 helps (26.7 to 21.1 percent) and does not close it: see item N6 in section 10.
In the natural bracket the cap removes at least one live foe on 11 to 40 percent of a caster's laps
(v2: never), because encounters start 900 to 2200 units out.

### 12.3 Missed casts: REPRODUCED

`fire-lane` (the player and a knight between the Evoker and two bandits) and the natural bracket.

| Arm | fired of picked | held by the line of fire | given up after 6 s | claim to fire | cast dps |
|---|---|---|---|---|---|
| v2 | 99.4 % | 0 | 0 | 0.54 s | 23.0 |
| v3 | 83.2 % | 24.3 % (2.7 s each) | 11.4 % | 2.73 s | 5.8 |
| v3 `--v3 lof=off` | 96.3 % | 0 | 0 | 2.36 s | 14.0 |
| v3 `--v3 stance=off` (no side step, no slots) | 33.3 % | 67.9 % | 62.3 % | 2.75 s | 1.1 |
| v3 `--deliver-sec 1.0,1.5` | 89.3 % | 20.4 % | 8.2 % | 1.28 s | 8.5 |

Natural bracket, v3, per class: picked casts that fired / held at least once / given up / released out of sight.

| Class | picks / fight | fired | held | given up | out of sight | direct road |
|---|---|---|---|---|---|---|
| mage | 3.9 | 76.4 % | 21.0 % | 6.4 % | 1.3 % | 19.7 % |
| evoker | 3.5 | 77.9 % | 26.4 % | 10.7 % | 0.7 % | 20.7 % |
| conjurer | 4.2 | 85.2 % | 20.1 % | 4.1 % | 1.2 % | 16.6 % |
| healer | 2.6 | 77.9 % | 12.5 % | 2.9 % | 1.9 % | 32.7 % |
| warlock | 3.5 | 87.3 % | 10.6 % | 2.1 % | 0.0 % | 23.9 % |
| mage-off | 5.4 | 87.1 % | 11.5 % | 3.7 % | 2.8 % | 14.3 % |

The line of fire is the largest single cause, as the field said. Occlusion is small here: the built-in open
fights carry 0 or 1 pillar, which is the world's cover density and not a finding. A caster's damage falls from
about 23 a second to about 5.5, and that is the main reason the caster cells lose 5 to 15 J (12.6).
The side step is an ASSUMPTION and the result leans on it hard (33 percent fired without it).

### 12.4 Delivery: REPRODUCED

Natural bracket, 60 fights, `--delivery old` against `main`.

| Class | Arm | fired of picked | never built | picks dropped by a newer pick | picks / fight |
|---|---|---|---|---|---|
| evoker | old | 15.9 % | 4.6 % | most of them | 14.9 |
| evoker | main | 82.4 % | 0 | 0 | 4.5 |
| conjurer | old | 16.3 % | 4.8 % | most of them | 13.9 |
| conjurer | main | 83.6 % | 0 | 0 | 4.1 |

With the rules of before 11-13 a claim is replaced by the next lap's different pick again and again and five
casts in six never happen (field: "22 laps in 10 s with 0 fires"). With main's guard it is gone.
Headline J: 77.38 old against 79.11 main.

### 12.5 The knight's guard: REPRODUCED (a number now exists outside a taunt)

| Cell | blows landed on him / fight | met his guard | guard up, blow from outside the arc | no guard up | the old `guard2%` |
|---|---|---|---|---|---|
| knight, natural bracket | 6.0 | 22.6 % | 2.1 % | 75.3 % | 71.0 % (on 0.8 of the 6.0 blows) |
| knight, `knight-ring` (four bandits around him) | 22.7 | 4.2 % | 0.7 % | 95.1 % | 69.0 % |
| the same, `--block-arc 120` | 22.7 | 3.9 % | 1.0 % | 95.1 % | 68.5 % |
| warrior, natural | 5.2 | 0.5 % | 0.0 % | 99.5 % | none |

`guard2%` looked at one blow in seven. Over all blows the Knight meets 23 percent with his guard, and 4 percent
when he is surrounded. Direction matters little (1 to 2 percent). The guard is simply not up: the Block
candidate needs a swing in the air on him or two taunted foes. Timing and candidate supply are the gap, not the arc.

### 12.6 The other mechanisms

| Scenario | v2 | v3 |
|---|---|---|
| `bow-channel`: bow denied by a channelled self buff | 0 % of her time, dps 29.0 | 40.6 % of her time (217 weapon damage a fight), stream 10.8 s, dps 17.0 |
| `cloak-area`: seconds a foe stands in his cloak, per fight | no area | 0.06 s, with the cloak up 16 % of the fight: he backs away from the foes it needs |
| `circle-area`: time he stands outside his own circle | no area | 83 % of the time it is up |
| `ledge-archer` | U 92.4 | U 90.8 (the ranged player kills it, the warrior cannot) |
| Oracle gap on `fire-lane` / `jesper-dagger` / `dragon-circle` (K 4, 30 s, 10 fights) | | +3.64 / +5.27 / +3.24 U. `ext:clear_line` is offered and is not among the oracle's top wins: a 1.2 s step rarely clears a friend who stands next to the target |

### 12.7 The shipped tunings under v3 (no retune)

Headline J, 6 seeds, `--classes beta1`: v2 81.76 (se 0.39), v3 79.11 (se 0.4).
One switch off at a time: `delivery` 81.85, `lof` 81.43, `stance` 73.24, `casterhold` 79.96, `chase` 79.17,
`hands` 79.00, `blockdir` 79.12. `--delivery old` 77.38. `--deliver-sec 1.0,1.5` 81.33. `--waiver self` 79.09.

Per class cell, each run alone (`--classes <cell> --seeds 1..6`, so the standard errors are wide):

| Cell | v2 | v3 | | Cell | v2 | v3 |
|---|---|---|---|---|---|---|
| melee | 83.20 | 79.08 | | evoker | 83.73 | 74.70 |
| ranged | 73.78 | 77.00 | | conjurer | 76.63 | 63.48 |
| mage | 82.40 | 71.82 | | healer | 83.45 | 68.14 |
| knight | 73.72 | 73.28 | | warlock | 80.00 | 75.43 |
| warrior | 88.48 | 88.75 | | melee-off | 76.74 | 79.18 |
| mercenary | 81.36 | 83.05 | | ranged-off | 74.83 | 69.31 |
| archer | 83.74 | 82.87 | | mage-off | 71.91 | 69.51 |
| thief | 86.48 | 88.59 | | | | |

(These cells were scored before the staff was made one-handed. The headline and every other table are after it.)
The casters lose 5 to 15 J, the weapon classes move inside their noise. LoreRim (evoker, knight, archer, 2 seeds,
legacy potion price): v2 58.57, v3 54.51.

### 12.8 Run time

`--classes beta1 --seeds 1,2,3,4,5,6 --threads 16`: v2 9.1 s CPU (0.75 to 0.9 s wall), v3 11.1 s CPU
(0.85 s wall). About 22 percent more. `--world v2` in the new binary costs the same as the old binary.

### 12.9 Checks

- `--world v2` is v2: the full metric dump of `--classes beta1` is identical to the binary of origin/main, line for
  line, and `--compare baseline_builtin.txt` reads 0 metrics changed.
- `run.sh` CLI checks: the three exact v2 pins run with `--world v2`. Eight v3 checks added, all pass.
  One check fails and failed before this branch: "logistics world ... a list or an IAUS policy is refused"
  (`--logi-policy iaus` exists now, the check is stale).
- `test_tune_driver.py`: 21 tests OK. The driver needs no change: `--sim-flags "--world v2"` runs it on v2.
- Address and undefined-behaviour sanitizers: beta1, the field scenarios (both delivery modes), the oracle,
  table mode and LoreRim, no report.
- The perk mirror is live: with `perks` on, `--curve evoker:cast_offense.94.py1=3` moves J. With it off it does not.
