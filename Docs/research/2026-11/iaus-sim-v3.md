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
| `stance` | the engine's movement by stance for followers and foes, attack slots around a target, foe fallback | 3.3 |
| `sidestep` | a caster whose cast is held side steps (`--side-step random|away`). Its own switch: unsourced and load-bearing | 3.3 |
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
| A caster side steps when his cast is held (switch `sidestep`, not `stance`) | on each 0.25 s of a hold, with chance min(1, 2 x CSLR strafe mult), he side steps 160 units across the line, to a RANDOM side. `--side-step away` = to the side the friend in his way is not on, which nothing says he knows | ASSUMPTION | `Docs/ENGINE_NOTES.md:1165` (casters strafe and close while casting) is the only source, and it gives no direction and no rate. Swept in 12.8 |

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
| Who is a friendly | anyone not hostile to the caster: the player, followers, summons, allied NPCs (the lab has no allied NPCs) | FACT | `APMF_API.h:801-803`. MFO's own direct road walks the player and the active followers only (`Sightline.cpp:449-477`): the lab uses Harbinger's wider list for both |
| Which casts | every hostile cast that is not Self delivered | FACT | `APMF_API.h:801`, `CombatScan.cpp:1078`, `bFriendlyFireHold` ships ON |
| When | at the START (0x06 CheckStartCast) and at the RELEASE (0x0A). A held cast does not fire and is not paid for | FACT | `APMF_API.h:806,618` |
| An engine-AI cast | a blocked start is never made: the AI passes that spell over and picks something else. A blocked release is refused and the cast is over. No hold | FACT for the two answers, MODEL for "picks something else" | same |
| A claimed cast (a decision's) | MFO's claim stands while the guard says no: the start, then the release, wait in 0.25 s steps until the line is clear or the claim is 6 s old | FACT | `cast/Hands.cpp:369` `kInFlightHoldCap` |
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
4. The held caster's side step (160 units, chance by CSLR, random side). Its own switch and knob. Swept in 12.8.
5. Attack slots 50 degrees apart.
6. The controller is inactive until a foe is within 2000 units in sight.
7. The re-draw of 0.65 s.
8. Who a foe picks (`World.h` `PickTarget`: nearness, x1.8 for its last attacker, x1.25 for the player). v2's MODEL of an
   undecoded engine choice. It decides how often blades reach a caster. `--caster-pressure` adds fights where they do.

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
- N6. FOUND BY THE MIRROR, verify in native before acting. The waiver keeps a held foe in the snapshot, and then nothing
  can name it. For every drawn state but a bow or a staff, act.attack reads RangeChase = distance / chase cap with
  VetoAbove(1.0) (`Catalogue.cpp:313-315,796-799`, `Generate.cpp:135`). A waived foe is past the cap by definition, so
  that attack is ALWAYS vetoed. A bow or a staff reads RangeSpell with the cap as its reach and is vetoed past 1.15 x
  the cap. A cast is vetoed by its own spell range. So the decision goes idle and the pin the waiver needs is not renewed.
  In the lab the dragon is still dropped on 20.5 percent of the laps with `--waiver player` (12.2). That share is an
  UPPER reading for two reasons: the lab clears the pin the moment a decision is idle (the game's pin may outlive a few
  laps), and the lab's `held` is the act.attack pin only, while the game also counts the ch.21 entry foe
  (`entryFoeID`, `Candidates.cpp:321,351`), which the lab does not model.

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

After the review fix round (line of fire tested at the start too, summons count as friends, the side step on its
own switch with a random side). Commands: `tools/iaus_sim/run.sh --scenario <name> --fights 300 [arm]` for 12.1 to
12.6, `--classes <cells> --seeds 1,2,3,4,5,6` for 12.7 and 12.8. Natural-bracket rows are from
`tools/iaus_sim/baseline_v3_builtin.txt` (`--classes beta1 --baseline`).

### 12.1 Jesper with the dagger: REPRODUCED, with one honest limit

`jesper-dagger`: an Evoker (level 25) with a dagger, a melee player, three bandits that start on him.

| Arm | U | downs / fight | taken / s | blade drawn | in contact | nearest to target | cast dps |
|---|---|---|---|---|---|---|---|
| v2, old rule (the sim until today) | 89.23 | 0.09 | 7.50 | 58.5 % | 58.1 % | 138 | 20.3 |
| v2, the 11-13 rule | 90.53 | 0.00 | 3.53 | 31.8 % | 48.5 % | 152 | 25.2 |
| v3, old rule (`--v3 casterhold=off`) | 85.69 | 0.16 | 4.66 | 52.4 % | 41.7 % | 138 | 6.2 |
| v3, the 11-13 rule (default) | 88.35 | 0.01 | 2.99 | 7.4 % | 31.7 % | 158 | 8.4 |

With the foes on him the dagger arm goes down 0.16 times a fight against 0.01 and costs 2.7 U in v3 (1.3 U in v2).
He comes within 138 units of his target (field: 209).
The limit: v2 shows the same sign in THIS scenario. What v2 could not do is show it where the tuner looks: in the
natural bracket and the headline scenarios its Evoker stood 40 percent of his time in contact with his blade out
and went down in 5 percent of fights. In v3's headline the rule is still worth nothing measurable
(`casterhold=off` 80.20 against 80.43, inside one standard error). The reason is who the lab's foes pick
(section 9 item 8): they rarely go for the caster. With `--caster-pressure` (one dive fight per caster cell in the
objective) the rule reads 80.98 against 80.44 without it in v3, and 82.52 against 82.26 in v2. Right sign, small.
"Mean distance to his target" is NOT a standoff measure in these runs. Traced over 40 fights: on 54 percent of his
laps no decision names a target, and the measured target is then his engine target, which on a third of those laps
is a routed foe about 4800 units away. With the cap on, a routed foe past the cap also leaves the snapshot, so fewer
laps are steered at one (3.7 against 12.7 percent). That mix is what moves the mean between `chase` on and off. Read
`minTgt` and `contact%` instead.

### 12.2 The circling dragon: REPRODUCED

`dragon-circle`: an Evoker (level 30), a ranged player, one dragon that fights the player.

| Arm | laps with the dragon dropped | mean cap | kept by the waiver / fight | idle laps |
|---|---|---|---|---|
| v2 | 0.00 % | 4096 (flat) | 0 | 0.0 % |
| v3 `--waiver none` | 36.74 % | 2096 | 0 | 24.1 % |
| v3 `--waiver self` (the game of 10-19) | 25.35 % | 2067 | 34.2 | 17.3 % |
| v3 `--waiver player` (the game today) | 20.50 % | 2056 | 48.1 | 14.9 % |

The cap reads about 2060 to 2100 units against a dragon, the number the field figure starts at (2184). v2 never
dropped it. The fix of 11-13 helps and does not close it: item N6 in section 10, with its two reasons why the
lab's share is an upper reading. In the natural bracket the cap removes at least one live foe on 12 to 40 percent of
a caster's laps (v2: never), because encounters start 900 to 2200 units out.

### 12.3 Missed casts: REPRODUCED

`fire-lane` (the player and a knight between the Evoker and two bandits) and the natural bracket.

| Arm | fired of picked | held by the line of fire | given up after 6 s | claim to fire | cast dps |
|---|---|---|---|---|---|
| v2 | 99.4 % | 0 | 0 | 0.54 s | 23.0 |
| v3 (side step to a random side) | 67.4 % | 41.0 % (3.5 s each) | 26.0 % | 2.87 s | 6.7 |
| v3 `--v3 lof=off` | 95.2 % | 0 | 0 | 2.33 s | 18.4 |
| v3 `--v3 sidestep=off` | 57.2 % | 48.1 % | 36.4 % | 2.88 s | 5.3 |
| v3 `--side-step away` | 78.9 % | 32.7 % | 15.0 % | 2.89 s | 8.9 |
| v3 `--deliver-sec 1.0,1.5` | 73.0 % | 37.6 % | 23.0 % | 1.47 s | 9.6 |

Natural bracket, v3, per class.

| Class | picks / fight | fired | held at least once | given up | released out of sight | direct road |
|---|---|---|---|---|---|---|
| mage | 5.3 | 70.9 % | 32.9 % | 15.5 % | 0.0 % | 14.1 % |
| evoker | 4.6 | 66.1 % | 39.9 % | 21.3 % | 0.5 % | 15.8 % |
| conjurer | 5.1 | 68.5 % | 32.5 % | 17.2 % | 1.0 % | 13.8 % |
| healer | 3.4 | 76.6 % | 21.2 % | 13.1 % | 0.7 % | 24.8 % |
| warlock | 4.0 | 85.7 % | 13.0 % | 6.8 % | 1.2 % | 20.5 % |
| mage-off | 7.8 | 78.8 % | 19.6 % | 12.9 % | 1.0 % | 9.3 % |

The line of fire is the largest single cause, as the field said: one cast in three is held and one in six is never
made. Occlusion is small here because the built-in open fights carry 0 or 1 pillar: that is the world's cover
density, not a finding. A caster's damage falls from about 23 a second to about 7.

### 12.4 Delivery: REPRODUCED

Natural bracket, 60 fights, `--delivery old` against `main`.

| Class | Arm | fired of picked | never built | picks dropped by a newer pick | picks / fight |
|---|---|---|---|---|---|
| evoker | old | 12.6 % | 3.9 % | 77 % | 17.4 |
| evoker | main | 67.4 % | 0 | 0 | 5.0 |
| conjurer | old | 14.3 % | 4.2 % | 76 % | 15.9 |
| conjurer | main | 70.4 % | 0 | 0 | 4.6 |

With the rules of before 11-13 a waiting claim is replaced by the next lap's different pick again and again, and
six casts in seven never happen (field: "22 laps in 10 s with 0 fires"). Headline J: 78.59 old, 80.43 main.

### 12.5 The knight's guard: REPRODUCED (a number now exists outside a taunt)

| Cell | blows landed on him / fight | met his guard | guard up, blow from outside the arc | no guard up | the old `guard2%` |
|---|---|---|---|---|---|
| knight, natural bracket | 6.1 | 22.6 % | 1.6 % | 75.7 % | 71.4 % (on 0.9 of the 6.1 blows) |
| knight, `knight-ring` (four bandits around him) | 22.7 | 4.2 % | 0.7 % | 95.1 % | 69.0 % |
| the same, `--block-arc 120` | 22.7 | 3.9 % | 1.0 % | 95.1 % | 68.5 % |
| warrior, natural | 5.0 | 1.0 % | 0.0 % | 99.0 % | none |

`guard2%` looked at one blow in seven. Over all blows the Knight meets 23 percent with his guard, and 4 percent
when surrounded. Direction matters little (1 to 2 percent). The guard is simply not up: the Block candidate needs a
swing in the air on him or two taunted foes. Timing and candidate supply are the gap, not the arc.

### 12.6 The other mechanisms

| Scenario | v2 | v3 |
|---|---|---|
| `bow-channel`: bow denied by a channelled self buff | 0 % of her time, dps 29.0 | 46.9 % of her time (305 weapon damage a fight), stream 10.8 s, dps 12.7 |
| `cloak-area`: seconds a foe stands in his cloak, per fight | no area | 0.07 s, with the cloak up 15 % of the fight: he backs away from the foes it needs |
| `circle-area`: time he stands outside his own circle | no area | 61 % of the time it is up |
| `ledge-archer` | U 92.4 | U 90.8 (the ranged player kills it, the warrior cannot) |

### 12.7 The shipped tunings under v3, one switch off at a time, per class (no retune)

J, 6 seeds. `beta1` is the 15 cells run together (se about 0.4 to 0.6). Each other row is that cell run alone, so
its standard error is 1 to 6 J: read a row's pattern, not a single cell. Columns after `v3` are v3 with that one
switch off.

| cell | v2 | v3 | height | chase | lof | delivery | hands | stance | sidestep | casterhold | blockdir | areas | perks |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| beta1 | 81.76 | 80.43 | 80.13 | 79.90 | 81.43 | 82.11 | 80.41 | 78.55 | 79.66 | 80.20 | 80.42 | 80.43 | 80.47 |
| melee | 83.20 | 80.19 | 80.19 | 80.52 | 81.46 | 81.54 | 80.20 | 81.00 | 80.23 | 80.16 | 80.19 | 80.19 | 80.20 |
| ranged | 73.78 | 78.31 | 78.31 | 73.04 | 77.38 | 78.36 | 78.18 | 71.76 | 77.09 | 78.09 | 78.31 | 78.31 | 78.18 |
| mage | 82.40 | 73.52 | 73.52 | 72.11 | 78.27 | 77.61 | 73.48 | 70.69 | 70.03 | 75.19 | 73.53 | 73.52 | 73.44 |
| knight | 73.72 | 73.68 | 73.68 | 74.15 | 73.92 | 74.39 | 73.63 | 71.81 | 73.57 | 72.43 | 73.65 | 73.68 | 73.70 |
| warrior | 88.48 | 88.30 | 88.30 | 89.52 | 88.43 | 89.19 | 88.23 | 88.25 | 88.81 | 88.36 | 88.30 | 88.30 | 88.30 |
| mercenary | 81.36 | 82.85 | 82.85 | 82.03 | 83.18 | 82.72 | 83.00 | 81.41 | 83.08 | 82.80 | 82.85 | 82.85 | 82.78 |
| archer | 83.74 | 84.01 | 84.01 | 83.85 | 83.53 | 85.45 | 84.01 | 82.45 | 83.51 | 83.50 | 84.01 | 84.01 | 84.06 |
| thief | 86.48 | 88.00 | 88.00 | 87.62 | 88.06 | 88.73 | 88.55 | 84.89 | 88.16 | 87.98 | 88.00 | 88.00 | 88.00 |
| evoker | 83.73 | 77.22 | 76.12 | 75.44 | 80.97 | 80.27 | 77.40 | 74.83 | 75.41 | 76.17 | 77.20 | 77.22 | 77.79 |
| conjurer | 76.63 | 62.36 | 62.36 | 63.96 | 69.70 | 73.32 | 62.49 | 63.80 | 62.95 | 62.07 | 62.36 | 62.36 | 62.93 |
| healer | 83.45 | 80.59 | 80.59 | 79.46 | 81.30 | 82.60 | 81.12 | 79.72 | 77.30 | 80.20 | 80.58 | 80.59 | 80.99 |
| warlock | 80.00 | 73.79 | 73.79 | 73.37 | 75.49 | 81.15 | 73.54 | 74.27 | 75.99 | 76.22 | 73.79 | 73.79 | 73.91 |
| melee-off | 76.74 | 80.52 | 80.52 | 78.59 | 79.30 | 81.23 | 74.56 | 76.50 | 78.75 | 80.52 | 80.52 | 80.52 | 80.44 |
| ranged-off | 74.83 | 69.05 | 69.05 | 68.33 | 73.61 | 71.02 | 70.91 | 66.44 | 69.17 | 70.39 | 69.05 | 69.05 | 69.09 |
| mage-off | 71.91 | 68.91 | 68.91 | 67.48 | 71.88 | 71.46 | 69.28 | 62.24 | 62.10 | 66.95 | 68.84 | 68.91 | 67.92 |

What it says: the casters lose 3 to 14 J under v3 and `delivery` and `lof` are what take it (conjurer +11 and +7
with one of them off, warlock +7 with delivery off). `height`, `blockdir`, `areas` and `perks` move nothing in the
headline cells (no dragon in most fights, guards rarely up, no area spell in a random book, flat perk curve):
their effect is in their own scenarios. `stance` and `sidestep` off both cost the casters.
Other headline arms: `--delivery old` 78.59, `--deliver-sec 1.0,1.5` 81.56, `--waiver self` 80.36,
`--caster-pressure` 80.98. LoreRim was not re-run after the fix round.

### 12.8 The side step sweep (review F4)

| Arm | headline J (beta1) | the six caster cells together | `fire-lane` fired | given up | cast dps |
|---|---|---|---|---|---|
| `--v3 sidestep=off` | 79.66 | 76.98 | 57.2 % | 36.4 % | 5.3 |
| random side (default) | 80.43 | 78.70 | 67.4 % | 26.0 % | 6.7 |
| `--side-step away` | 80.74 | 79.97 | 78.9 % | 15.0 % | 8.9 |

The whole assumption is worth about 1 J on the headline and 3 J on the caster cells, between "never steps" and
"always steps the right way". The default sits in the middle. A retune should be checked at both ends.

### 12.9 Run time

`--classes beta1 --seeds 1,2,3,4,5,6 --threads 16`: v2 9.0 s CPU (0.76 s wall), v3 11.2 s CPU (0.94 s wall).
About 24 percent more. `--world v2` in the new binary costs the same as the old binary.

### 12.10 Checks

- `--world v2` is v2: the full metric dump of `--classes beta1` is identical to the binary of origin/main, line for
  line, and `--compare baseline_builtin.txt` reads 0 metrics changed. Re-checked after the fix round.
- `run.sh` CLI checks: the three exact v2 pins run with `--world v2`. Eight v3 checks added, all pass.
  One check fails and failed before this branch: "logistics world ... a list or an IAUS policy is refused"
  (`--logi-policy iaus` exists now, the check is stale).
- `tune_driver.py` now ALWAYS names the world: `--world v2` by default, `--world v3` to opt in, through the same
  wrapper as `--sim-flags` (which may not carry `--world` itself). `test_tune_driver.py`: 23 tests OK.
  `potion_worth.py` and `edge_search.py` run on their own still take the binary's default (v3) unless given a
  wrapper or `--world v2` in their simulator flags.
- Address and undefined-behaviour sanitizers (before the fix round): beta1, the field scenarios, the oracle, table
  mode and LoreRim, no report.
- The perk mirror is live: with `perks` on, `--curve evoker:cast_offense.94.py1=3` moves J. With it off it does not.
- The banner prints the world (`IAUS SIM v3` / `IAUS SIM v2`).

## 13. The sim mirror batch of 2026-11-17 (feat/sim-mirror-1117)

marth 11-17: "for some reason we didn't retune when new features were added". This batch brings the lab level with `origin/main` 6e0ccfe1 (anim-reach, field-1117 cast / engage, chase-waiver, hand_lost, perk effects, field-1117b choice), replaces the two single-session delivery figures with the Deck's measured distributions, and makes v3 the tuning default (his 11-16 ruling "v3 default after data").

### 13.1 What is mirrored, and where

| Game change | Lab | Switch |
|---|---|---|
| perk-effect reader spell boost (`c.spell_perks`) | already in v3 (section 7), checked against `iaus/PerkEffects.cpp` | `perks` |
| `hand_lost` (1dfcuabd22x) | `Combatant::weaponLeftShare` = 1 for a bow or a two-hander in hand (`Policy.h MakeSnapshot`, the rule of `Fills.cpp:345`). The lab has no second melee weapon, so no 0.5 row. `t.concChannelSec`, `t.handRedrawSec` in `Tune.h` | `hands` |
| chase-waiver veto fix | the waiver goes through the game's `ChaseWaived` reader, the kept foe is marked `chaseWaived`, the generator clamps it (compiled in). Entry foe: OFF by default as ch.21 EngageOnSight ships (`--engage-on-sight`); when on, a MODEL: his engine target at his first decision, over when it falls or his target moves on. `t.waivedRangeX` | `chase` |
| noApproach foes (11-17 A3) | a foe past the cap in bow range, or one that targets him, is kept and marked. `Brain`: a named noApproach foe is shot at from where he stands, never walked to. `t.bowRange`. Ranking, Swings, RangeFoe and the cast skips are the game's own code | `chase` |
| engagement fixes | the reachable-foe preference and `MeleeUnreachableNow` are the game's code and read the lab's z. The 128 approach on a ledge with a way up is a climb MODEL (a path three times the height, at run speed, `FightSpec::perchPath`). A ledge with no way up stays out of reach. See 13.4 for what has no lab counterpart | `height` |
| melee reach model | `Ctl::reach` is the game's `ReachBook::Book`. Outcomes are EDGE distances, one chain of one swing each (the lab has no combo framework). `weaponEdge` and `comboEdge` are read from it only with a blade in hand. This fixes the N3 line of `Policy.h` (it fed a centre value less a radius for any weapon drawn) | `--reach` |
| cast fixes | a self buff or ward with no combat controller is deferred, so is a heal during a run. A heal claim whose recipient reads full before the charge began is released. The substrate back-off has no lab counterpart (no out of combat phase) | `delivery` |
| field-1117b choice | heal_rate, the effect-rate tie-break, the line of fire gate, HealRefused and the self dying boost are the game's code. Lab side: the refused (spell, foe) pairs are remembered for `t.fireLineKeepSec`, a channelled heal yields its hand to a non-heal pick while a foe hurts him. `t.spellCastSec`, `t.dyingSelfHp`. The fills the game now ships ON were already on in the lab | none |

`--mirror off` turns the lab side of this batch off (not the delivery tables, not the reach book, not the game's own generator) for before / after runs.

### 13.2 Cast delivery, measured

Tool: `tools/iaus_sim/calib_cast_delay.py`. Source: `_evidence/deck-run-1117/part4` and `_evidence/deck-run-1117b-full`. The join: an `[iaus] pick=cast_*` line fires its claim in the same lap, Harbinger's caster census opens one `[ctcensus]` window per claim (its `spell=` is the pick's form whatever proxy drives the hand), `FIRED +ms` is the claim to fire time, `CLOSE` carries the verdict. Repeat fires come from MFO's `CFC-fired` lines. Substrate claims (40 windows) and windows over a main thread stall (5) are left out. 122 windows remain, ALL of one follower (Jesper).

| Nature | Windows | First fire n | p25 | median | p75 | Repeat median (n) | Never fired |
|---|---|---|---|---|---|---|---|
| offense | 46 | 21 | 1.29 | 1.72 | 2.66 | 1.22 (39) | 54.3 % |
| offense, concentration | 28 | 9 | 0.54 | 0.61 | 1.46 | none | 67.9 % |
| heal | 9 | 8 | 1.82 | 2.23 | 2.64 | 1.04 (36) | 11.1 % |
| heal, concentration | 11 | 10 | 0.68 | 1.04 | 1.19 | 2.26 (9) | 9.1 % |
| buff | 28 | 7 | 1.43 | 1.66 | 3.09 | none | 75.0 % |

Pick to claim at Harbinger: median 0.108 s (n=57).

**Never fired is mostly not delivery failure (review F1 of 5749026a).** By cause, per row:

| Row | Fired | Replaced | Built, yes, not released | Not built | Refused | Target died | Shape gap |
|---|---|---|---|---|---|---|---|
| offense (46) | 21 | 7 | 1 | 3 | 3 | 11 | 0 |
| offense, concentration (28) | 9 | 2 | 0 | 0 | 2 | 8 | 7 |
| heal (9) | 8 | 0 | 1 | 0 | 0 | 0 | 0 |
| heal, concentration (11) | 10 | 0 | 1 | 0 | 0 | 0 | 0 |
| buff (28) | 7 | 1 | 0 | 0 | 2 | 2 | 16 |

- Target died: the window's target died inside it or within 1.5 s of its close. The lab ends such a cast itself (`castLost`).
- Refused: a caster was built and CheckStartCast said no every time (sight, line of fire, range). The lab holds those casts itself.
- Shape gap: no caster was ever built, for a spell shape Harbinger never builds one for. It is the same every cast, so it is a framework gap and not a chance. FOR HARBINGER: Frost Cloak 0003AEA2 (10 of 16 windows), Flame Cloak 0003AE9F (5 of 7), Lightning Cloak 0003AEA3 (1 of 1), all "no-row". Frostbite 0002B96B (6 of 14) and Creeping Cold 800279BD (1 of 10), "av26 row-without-caster". Turn Lesser Undead 0004D3F8 "TargetEffect no-row" (the review counts 4 not-built windows. Here each also had its target die, so they sit in the target-died column).

The lab's unfired share is (replaced + built not released + not built) over (those + fired): **offense 0.344 (11 of 32), offense concentration 0.182 (2 of 11), heal 0.111 (1 of 9), heal concentration 0.091 (1 of 11), a cloak 0.125 (1 of 8)**. The first cut of this batch used 0.500 / 0.654 / 0.111 / 0.091 / 0.731.

In the lab (`Data.h Delivery`, `World.h StartCast`): a claimed cast draws its first fire from the decile table of its row plus 0.11 s. A re-pick of the same cast at the same target within 2 s of its release (ASSUMPTION) draws the repeat table (`dl.repeat%` counts how often). With the row's unfired share the claim closes with no cast after a time drawn from how long such claims stood. The buff row is a CLOAK's only: a summon, a ward, an armor spell or any other buff was never sampled, so it always fires, after the pooled first-fire time of the fire-and-forget claims (n=36, ASSUMPTION). `--deliver-unfired k` scales the shares, `--deliver-sec o,h` is the fixed model of the first cut.

### 13.3 Field cases of 11-17, before and after (built-in world, shipped tunings, 96 fights)

What "before" means differs by row. For `heal-channel` and `offense-pick` it is the GAME before fix/field-1117b-choice (the lab built against native f5249bce). For `bow-occluded`, `tower-blade` and `cap-archer` it is only the LAB's own model switched off (`--mirror off`): the same game code, without this batch's lab side. It is not the game before 11-17, so those three rows show what the lab model adds, not what the fix changed in the field.

| Scenario | Before | After |
|---|---|---|
| `heal-channel` (before = game f5249bce, first cut's shares) | channelled heals 66.3 %, mean heal 37.5, self heal at 53 % health, player dead 24.0 %, downs 0.03, U 65.8 | channelled 6.1 %, mean heal 71.5, self heal at 45 %, player dead 12.5 %, downs 0.00, U 76.9 |
| `offense-pick` (before = game f5249bce, first cut's shares) | pick damage 34.4, cost 17.3, resisted 0 %, dps 3.63, U 83.7 | pick damage 32.9, cost 16.4, resisted 0 %, dps 5.10, U 88.3 |
| `bow-occluded` (before = `--mirror off`) | first hit 5.27 s, idle 39.2 %, hide runs 0.41 / min, U 89.8 | first hit 5.17 s, idle 42.4 %, hide runs 0.29 / min, U 89.7 |
| `tower-blade` (before = `--mirror off`) | never hits (100 % of fights), fight 20.1 s | first hit 8.1 s, 4.3 hits a fight, fight 15.0 s |
| `cap-archer` (before = `--mirror off`) | idle 94.9 %, empty foe list 94.8 % of laps, shooting 22.4 % of his time, downs 0.92, U 77.8 | idle 7.2 %, empty 7.2 %, shooting 27.8 %, downs 1.22, U 76.7 |

The "after" column is the final build (corrected unfired shares, the entry foe off). The two game-before rows were measured with the first cut's shares, so part of their gain in dps and U is the delivery correction and not the 1117b fix. `bow-occluded` shows no gain from the lab model (its two runs differ by noise).

Limits. The lab does not reproduce Jesper being burst down (0.03 downs a fight before): the evoker survives the pile. `offense-pick` shows no cheapest-spell pick before the fix either (the lab's books are measured by default). In `cap-archer` the lab's engine model shoots on its own when IAUS is idle, so the hits barely move and he goes down a little more often once he answers. `bow-occluded` keeps its hide runs: the 11-17b stance check has no lab counterpart (the lab's sneak stance always takes).

### 13.4 Not mirrored

- The occluded approach's standoff by attack road, its tightening and the leash block scoping: the lab's engine model already walks a caster or archer toward a target it cannot see until it has sight, and a blade always closes. No navmesh, no leash block list.
- The failed-hide abort and the 11-17b stance check: the lab's stance always takes.
- The substrate back-off: no out of combat casting in the lab.
- Combo sequences: every lab swing is a chain of one, so the sequence learner equals the weapon's.
- The unreachable pin release: the lab's pin follows the pick every lap.

### 13.5 Tuning

`tune_driver.py --world` defaults to v3 (v2 still runs). Its shared placeholders now include `t.spellCastSec` and `t.dyingSelfHp`. `t.waivedRangeX` and `t.bowRange` are lab knobs (`Tune.h`) but are NOT searched: the game's tuning file has no entry for them (`iaus/Catalogue.cpp SetTuningScalar`), so a found value could not ship. NATIVE FOLLOW-UP: add both names there, then put them back into `GLOBALS`.

The entry foe of the chase waiver is behind a switch that is OFF by default (`--engage-on-sight`, or `FightSpec::engageOnSight` for a scenario that models it), because the game only has an entry foe through ch.21 EngageOnSight, which ships OFF.

**Survival bands, oracle-relative (marth 11-17, verbatim: "Oracle downs").** No oracle-relative band existed in the lab before, so this is the first: the gap pass prints an `ORCSURV` line per class (IAUS and the class oracle on the same natural fights), and the band's bound is the oracle's value plus a labelled margin, written into `Playstyles.h` with the measurement in the reason. Measured on 160 natural fights per class, built-in world, v3, class rulings in force:

| Class | Oracle downs / fight | Bound (+ 0.05) | IAUS downs / fight | Oracle self heals begun under 25 % health | Bound (+ 5 points) | IAUS |
|---|---|---|---|---|---|---|
| mage | 0.044 | 0.09 | 0.031 | 5.3 % | 10 % | 10.6 % |
| evoker | 0.000 | 0.05 | 0.013 | 1.0 % | 6 % | 15.0 % |
| conjurer | 0.000 | 0.05 | 0.000 | 0.0 % | 5 % | 4.7 % |
| healer | 0.006 | 0.06 | 0.025 | 0.9 % | 6 % | 6.2 % |
| warlock | 0.013 | 0.06 | 0.019 | 1.5 % | 7 % | 13.0 % |

The self-heal timing band replaces the mean `selfHealAt%` (still a metric) with `selfHealLow%`, the share of his self heals (a heal cast at himself, a health potion) that BEGIN under a quarter of his health. The oracle can express it (it fires through the same code), so the band is kept and is oracle-relative too. With the shipped tunings the evoker and the warlock are outside it, the mage and the healer just outside. The bounds must be re-measured when the world or the oracle changes. One weakness: the persona credit of a band falls off over max(5, half the bound), so a `downs/f` bound of 0.05 costs almost no persona when it is broken. The driver's "no band may end further outside its bound" gate reads the raw distance and does bind.

`tune_driver.py load_cards` now parses a two-sided `inRange()` band and refuses a raw `Band{ }` in a class row (review F2).

Shipped tunings, 8 seeds, J (se). "v3 before" is the build before this batch (f5249bce, 2.4 / 4.5 s delivery). The other columns are the final build:

| Class | v3 before | v3 after | v2 after |
|---|---|---|---|
| melee | 81.88 (3.10) | 82.96 (3.05) | 84.48 (2.49) |
| ranged | 77.75 (3.50) | 76.48 (3.06) | 72.24 (2.51) |
| mage | 73.55 (1.45) | 77.03 (2.58) | 79.90 (2.29) |
| knight | 74.10 (1.21) | 75.85 (0.76) | 74.21 (0.65) |
| warrior | 87.51 (1.14) | 87.50 (0.99) | 87.73 (1.26) |
| mercenary | 82.24 (1.11) | 81.78 (1.31) | 80.36 (1.34) |
| archer | 82.46 (1.99) | 82.98 (2.31) | 81.45 (2.40) |
| thief | 88.54 (0.75) | 88.77 (0.78) | 86.69 (0.63) |
| evoker | 75.32 (2.30) | 76.55 (2.17) | 81.13 (1.50) |
| conjurer | 64.90 (4.79) | 69.97 (4.68) | 76.83 (2.48) |
| healer | 77.45 (2.91) | 78.76 (2.25) | 80.64 (2.49) |
| warlock | 74.47 (1.76) | 77.66 (2.69) | 79.03 (3.47) |
| melee-off | 81.28 (3.20) | 77.67 (3.07) | 74.52 (3.19) |
| ranged-off | 68.53 (4.36) | 74.42 (3.57) | 76.21 (3.74) |
| mage-off | 70.81 (4.36) | 73.00 (2.55) | 73.96 (2.98) |

Natural bracket, how often the repeat table is drawn (`dl.repeat%` of his cast picks): mage 29 %, evoker 21 %, conjurer 27 %, healer 33 %, warlock 27 %. Unfired: mage 12.6 %, evoker 18.2 %, conjurer 9.6 %, healer 13.6 %, warlock 7.8 %.

Both baselines are regenerated (`baseline_builtin.txt`, `baseline_v3_builtin.txt`). v2 moved because the game's generator and the reach book moved: with `--reach off` the build before this batch and the build after it agreed draw for draw on v2 until the 1117b merge.
