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
| `hittail` | a foe's physical hit on the party is drawn from the field's hit-size spread, mean 1 (2026-11-17) | 15.5 |

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

## 14. The oracle under the standing tuning rule (feat/oracle-unbounded, 2026-11-17)

**Read 14.10 first.** Sections 14.1 to 14.9 are the first round (d8b686a7) and keep its numbers as the record. 14.10 is the fix round after the Opus review and marth's rulings of the same day. It changes the oracle (random streams per actor, cooldowns, hide and flee, the sprint away) and re-measures everything. Where 14.10 and an earlier subsection disagree, 14.10 is the oracle that ships.

marth, 2026-11-17, verbatim: "as a standing tuning rule the Oracle gets as many tries as it wants for the best class based decisions. The tuning then follows the Oracle as its guide. The Oracle only has the classes abilities at its disposal". And the goal: "The goal is that a classes tune will be the actual best possible class behavior".

Code: `tools/iaus_sim/Oracle.h` (the search, the class filter), `sim.cpp` (the gap pass prints), `tune_driver.py` (the tables of the extended set), `Playstyles.h` (the survival bounds). No native change.

### 14.1 What changed in the search

| | Before | Now |
|---|---|---|
| Options per decision | at most 14 (IAUS's ranked list, then one to four per family) | every candidate the generator offers, every posture, the extended set, his own AI. No limit |
| Decision points | at most one every 0.4 s. With three followers a lap is 0.399 s, so every second lap was skipped | every time the pump serves him with his body free (`every = 0`). That is every decision point the game has: `kPumpMs` 133 ms times the followers |
| Rollout length | 150 s | to the fight's end. 150 s is `Fight::maxSec`, so this was already true. It is now stated and `RunFor` no longer depends on the number |
| Streams per option | 8, fixed | 8, then 16, 32, 64, 128. After each round an option is dropped once the best option's paired lead over it passes a one-sided Student-t bound at p = 0.001. The race ends when one option stands or at the safety ceiling of 128 |
| Random streams | shared by all options | first round: unchanged. Fix round (14.10): one stream per actor and draw site inside a rollout |
| Class rulings | off unless `--class-oracle` | on for every oracle. `--no-class-rulings` is a labelled diagnostic |
| Automatic gap pass of a results file | a cut oracle (K = 6, 90 s) | the same oracle as everywhere |
| What IAUS's pick costs (the ledger's dU) | difference of two means | the paired lead on the streams both options ran |

`--oracle-k N` and `--oracle-h S` still exist as labelled diagnostics (the CLI checks use one stream and 5 s for speed). `--oracle-ceiling N` moves the ceiling. `--oracle-every S` brings a throttle back. The results file and every oracle print say which setting ran.

One part of the rollout is a model and not a budget: an option is held for 2.5 s inside a rollout (an attack order, a retreat, his own AI) before the base policy takes over. The oracle itself is asked again at his next decision point. The search is still one step deep over IAUS as the base policy. It is not an exhaustive plan, and no feasible search is.

### 14.2 The stopping rule, the ceiling, and how often it binds

The honest finding: **the ceiling ends most races.** Most options of a decision differ by less than a fight's luck. The shared seed stops pairing two rollouts as soon as they diverge, because the world draws from one stream. So the paired difference of two options has a deviation of about 5 U, and a lead of half a U cannot be separated at any stream count we can pay for.

Measured on 16 natural fights per class, subject only, the same fights at every ceiling. Oracle U, then the paired change against the next lower ceiling (se):

| Class | ceiling 8 | 32 | 128 | 512 | 8 to 32 | 32 to 128 | 128 to 512 |
|---|---|---|---|---|---|---|---|
| knight | 79.65 | 84.06 | 82.60 | 87.59 | +4.41 (4.27) | -1.47 (5.03) | +5.00 (4.54) |
| warrior | 92.36 | 91.80 | 92.45 | 92.39 | -0.55 (0.67) | +0.65 (0.65) | -0.06 (0.41) |
| evoker | 88.29 | 94.28 | 94.17 | 94.27 | +6.00 (5.90) | -0.12 (0.69) | +0.10 (0.55) |
| thief | 94.98 | 94.43 | 94.29 | 94.47 | -0.56 (0.35) | -0.14 (0.44) | +0.18 (0.44) |
| healer | 93.12 | 92.64 | 92.68 | 93.66 | -0.48 (0.74) | +0.05 (0.62) | +0.98 (0.51) |

How the race ended at each ceiling (share of decisions the ceiling ended with more than one option standing, and the best option's mean lead over the best other one standing):

| Class | 8 | 32 | 128 | 512 |
|---|---|---|---|---|
| knight | 72 %, 0.90 U | 46 %, 0.97 U | 39 %, 0.69 U | 64 %, 0.40 U |
| warrior | 89 %, 1.30 U | 76 %, 0.73 U | 72 %, 0.40 U | 67 %, 0.32 U |
| evoker | 88 %, 1.02 U | 91 %, 0.72 U | 77 %, 0.40 U | 64 %, 0.28 U |
| thief | 87 %, 0.88 U | 80 %, 0.62 U | 78 %, 0.33 U | 77 %, 0.18 U |
| healer | 76 %, 2.51 U | 36 %, 1.00 U | 75 %, 0.51 U | 63 %, 0.15 U |

Reading. No step from 32 to 128 or from 128 to 512 is outside two standard errors for the warrior, the evoker and the thief. The healer's last step is +0.98 (0.51), which is 1.9 se. The knight is too noisy on 16 fights to say anything. Ceiling 128 was chosen because it is four times past the last ceiling at which any class moved, and 512 costs four times more for a gain this data cannot show. The ceiling is one number (`OracleCfg::ceiling`, `--oracle-ceiling`). This is the first thing the reviewer should weigh. If marth wants the larger number, 512 is a flag away and the cost is in 14.4.

What would sharpen the race itself, and is not done here: one random stream per actor inside a rollout, so two options stay paired on everything the changed action does not touch. That is a change of the world's draw order (24 draw sites in `World.h`), so it belongs in its own batch with its own baselines.

On the 32 natural fights per class of 14.5, at ceiling 128: the ceiling ended 34 to 88 percent of the decisions, with 3.4 to 9.1 options standing and a mean unresolved lead of 0.28 to 0.88 U. The gap pass prints this per class (`ORCSTOP`).

### 14.3 The class-bound action set

`Oracle.h ClassRefuses` is the one reader of "is this option an ability of his class". It returns the reason an option is refused. `RulingRefuses` (the old `ClassAllows`) is the written rulings of `Playstyles.h`. Every refusal is counted by reason and printed per class (`ORCREFUSED`), and a trace run prints each one.

| Option | Rule |
|---|---|
| A catalogue candidate | the generator already holds it to the class gate (`ClassGate`, built by `RealClasses.cpp` from `native/classes/ClassTable.cpp`). The filter checks the same bit again per family: power attack, taunt, block, retreat and disengage, the weapon of a posture, a spell by its source (class spell, scroll, staff). A refusal here would be a generator leak. Reasons start with `cat:`. Measured: 0 |
| `ext:block`, `ext:bash` | only with the shield, or a melee weapon of the class's weapon set, in hand. The guard also honours the game's rest now (no new guard for 1.5 s after a hold, `apmf/BlockClaims.cpp kRest`) |
| `ext:scroll` | only a scroll user (ruling Q36) and only a scroll he carries |
| `ext:hide` | only a sneak class that has the disengage flag. Before: anyone who was in stealth |
| `ext:flee` | only a class with the disengage flag. Before: anyone under 60 percent health with a foe on him |
| Potions | the game has no class gate for a potion, so there is none here |
| Repositioning (`ext:step_back`, `sprint_back`, `to_player`, `to_healer`, `get_sight`, `clear_line`, and the Regroup, GiveGround, BackOff and Step families) | always allowed by the class filter. Movement is not an ability. The written rulings can still forbid a move (never retreats, backs off only, stays near the player) |
| A posture the game forbids | new ruling line: a posture candidate whose `classAllowsWeapon` fact is false is refused. That is the strict weapon gate, or "spells stay on spells" for the Mage line. The old oracle could draw the dagger there |

The oracle stays free of the generator's situation gates (the 30 percent flee line, "a foe has found him"). Those are decisions, not abilities, and what the oracle gains by ignoring them is their cost.

A class's spell book in the lab is what `Load.h BuildFollower` gives a follower of that class, plus the spells its own default rows name. The game's class table has no school list per class, so there is no school filter to mirror.

Refused option offers on the 32 natural fights per class of 14.5 (subject only). An offer is one option at one decision:

| Class | By the ability filter | By a written ruling |
|---|---|---|
| melee | 109 flee (no disengage) | 0 |
| ranged | 14 flee | 0 |
| mage | 19 flee | 306 splash, 555 forbidden posture, 5 no-melee |
| knight | 183 flee | 1859 never retreats |
| warrior | 131 flee | 2065 never retreats |
| mercenary | 47 flee | 885 backs off only |
| archer | 3 flee | 0 |
| thief | 0 | 0 |
| evoker | 0 | 409 splash, 522 forbidden posture, 6 no-melee |
| conjurer | 0 | 354 splash, 558 forbidden posture, 3 no-melee |
| healer | 0 | 1722 splash, 1198 forbidden posture, 948 near the player, 75 no-melee |
| warlock | 0 | 763 splash, 592 forbidden posture, 32 no-melee |
| melee-off, ranged-off | 0 | 0 |
| mage-off | 0 | 181 splash, 628 forbidden posture |

No `cat:` refusal, no scroll, block or bash refusal: the kit of a lab follower is built from his class's weapon set, so those rules guard against a future kit change. The only ability the old oracle used outside a class was the flee run of a class without the disengage flag.

### 14.4 Run time

CPU seconds per natural fight (IAUS arm and oracle arm together), 32 fights per class, subject only, which is the gap stage's shape. One CPU of a 16-core host:

| Class | Before | Now | x |
|---|---|---|---|
| melee | 3.8 | 32.6 | 8.5 |
| ranged | 0.5 | 6.0 | 12 |
| mage | 4.2 | 43.5 | 10 |
| knight | 3.7 | 30.7 | 8.4 |
| warrior | 1.8 | 22.2 | 12 |
| mercenary | 1.4 | 30.2 | 22 |
| archer | 0.7 | 9.6 | 14 |
| thief | 0.4 | 4.4 | 11 |
| evoker | 6.2 | 57.4 | 9.2 |
| conjurer | 4.0 | 41.1 | 10 |
| healer | 2.7 | 36.6 | 14 |
| warlock | 8.3 | 80.6 | 9.7 |
| melee-off | 3.9 | 66.5 | 17 |
| ranged-off | 1.1 | 22.9 | 21 |
| mage-off | 2.9 | 51.8 | 18 |
| all 15 | 3.05 | 35.8 | 11.7 |

The search inside one fight is serial on purpose. Every caller already runs one fight per core (the gap pass's worker threads, the driver's process pool), so threads inside a decision would add nothing to a tune round, and a fight's result cannot depend on the thread count. Nothing is cached, because no two decisions share a state.

What a tune round pays. Only the gap stage, the gap order and the dashboard's small report runs use the oracle. The search stages judge on IAUS's own J and are untouched. The gap stage's first pass is 15 cells times 12 seeds times about 5 fights times 2 tables, subject only, plus 900 fights with every follower planned: about 2,700 fights, about 41 CPU hours, about 2.6 hours on 16 cores per world (a fight with every follower planned costs 92 CPU seconds on the Mage line, measured on the 800 fights of 14.8). Cells that grow to 48 seeds and the re-measures after an accepted value add to that. It was about 10 minutes. Levers if a round must be shorter: `--oracle-ceiling 32` costs about a third and lost nothing measurable in 14.2.

A single run with few fights uses the cores badly (its wall time is its longest fight). The 32-fight runs of this section took 22 to 750 s of wall time each.

### 14.5 The gap per class, before and after

Natural bracket, built-in world, v3, shipped tunings, seed 1, 32 fights per class, subject only (his companions play IAUS in both arms). IAUS U is the same number in both columns, because IAUS did not change:

| Class | IAUS U | Oracle U before | Gap before (se) | Oracle U now | Gap now (se) |
|---|---|---|---|---|---|
| melee | 84.28 | 85.62 | +1.34 (3.65) | 89.38 | +5.11 (2.63) |
| ranged | 72.84 | 75.78 | +2.94 (2.40) | 76.33 | +3.49 (2.54) |
| mage | 75.22 | 82.46 | +7.24 (3.64) | 85.30 | +10.09 (4.48) |
| knight | 84.49 | 88.02 | +3.54 (1.45) | 87.99 | +3.50 (3.01) |
| warrior | 82.39 | 88.10 | +5.71 (3.17) | 89.80 | +7.42 (3.48) |
| mercenary | 88.55 | 89.58 | +1.02 (2.75) | 92.62 | +4.06 (1.49) |
| archer | 72.63 | 73.43 | +0.80 (3.57) | 78.47 | +5.85 (3.44) |
| thief | 88.29 | 93.53 | +5.24 (3.77) | 91.35 | +3.06 (4.58) |
| evoker | 76.90 | 84.37 | +7.48 (5.32) | 90.73 | +13.83 (4.78) |
| conjurer | 78.94 | 87.42 | +8.48 (4.24) | 86.28 | +7.34 (5.29) |
| healer | 69.31 | 77.47 | +8.16 (3.67) | 83.53 | +14.22 (5.08) |
| warlock | 65.82 | 75.76 | +9.94 (5.62) | 84.15 | +18.33 (5.79) |
| melee-off | 76.78 | 83.60 | +6.83 (4.99) | 88.06 | +11.29 (3.99) |
| ranged-off | 75.97 | 85.61 | +9.63 (3.80) | 88.86 | +12.89 (4.51) |
| mage-off | 74.79 | 85.79 | +11.00 (5.78) | 88.53 | +13.74 (6.33) |

The oracle is stronger for 12 of 15 cells although it lost options (the flee of five classes, the forbidden postures, the guard without a rest). The thief and the conjurer are lower by less than one se. The knight is level. The gain is not from more streams (14.2). It is from deciding at every decision point and from seeing every candidate.

### 14.6 Where the remaining gap sits, per class, and what a tune cannot reach

From the subject's divergence clusters of the same fights (`--gap-dump`, classified by `tune_driver.py classify`). Share = that class of divergence in the oracle's summed per-decision advantage. It orders the causes. It is not an additive split of the gap.

- (a) a curve or a weight: IAUS had the oracle's pick and ranked it lower. **A tune can close it.**
- (b) IAUS generated the oracle's pick and scored it under the floor or vetoed it. A gate, a veto or a missing input. **A tune cannot move it.**
- (c) the oracle's action has no candidate IAUS ever generates for this class. **A tune cannot reach it.**
- (d) the oracle's version of the action is better than the game's (the bash on demand, the guard with no row cooldown). A modelling limit of the oracle, not a goal.

| Class | (a) | (b) | (c) | (d) | The largest (a) clusters: IAUS pick to oracle pick |
|---|---|---|---|---|---|
| melee | 55 % | 12 % | 20 % | 13 % | attack to attack on another target 39 %, attack to his own AI 10 % |
| ranged | 62 % | 10 % | 25 % | 3 % | attack to a weapon swap 42 %, attack retarget 18 % |
| mage | 67 % | 16 % | 16 % | 0 | cast_control to attack 36 % |
| knight | 77 % | 7 % | 4 % | 11 % | taunt to attack 16 %, power attack to attack 15 %, attack retarget 14 %, drink_health to attack 13 % |
| warrior | 68 % | 14 % | 2 % | 16 % | attack retarget 41 %, power attack to attack 21 % |
| mercenary | 62 % | 21 % | 12 % | 5 % | attack retarget 47 % |
| archer | 61 % | 16 % | 20 % | 3 % | attack retarget 33 %, attack to a weapon swap 24 % |
| thief | 31 % | 42 % | 27 % | 0 | attack retarget 22 % |
| evoker | 42 % | 30 % | 28 % | 0 | cast_offense to attack 11 %, drink_health to his own AI 8 % |
| conjurer | 55 % | 34 % | 11 % | 0 | cast_control to attack 32 %, cast_control to cast_offense 6 %, to cast_heal 5 % |
| healer | 51 % | 35 % | 14 % | 0 | cast_control to attack 26 %, cast_heal to his own AI 8 % |
| warlock | 43 % | 43 % | 14 % | 0 | cast_control to attack 13 %, cast_control to cast_heal 6 %, to a weapon swap 5 % |
| melee-off | 63 % | 15 % | 15 % | 7 % | cast_offense to attack 12 %, taunt to attack 12 %, power attack to attack 9 % |
| ranged-off | 71 % | 12 % | 16 % | 0 | cast_offense to attack 40 % |
| mage-off | 39 % | 41 % | 21 % | 0 | cast_control to attack 12 %, cast_control to cast_offense 7 % |

Two patterns carry most of the tunable gap. On the weapon classes it is WHICH foe the attack is on (the attack family's target considerations). On every caster it is cast_control chosen where the oracle lets the plain attack order run (the control family's weight against attack).

**Structural limits. A tune cannot close these. Each is a native follow-up, listed so the tuner does not fail on it:**

| Class | Limit (share of his divergence cost) | Kind | What it needs |
|---|---|---|---|
| warlock | cast_control where the oracle flees: 17 % | (b) | the flee candidate exists and is scored under the floor or vetoed. The Disengage flee is held to its 30 percent health line and its "threatened" read. Needs the line as a tunable input, or a consideration that reads the pressure the oracle reacts to |
| healer | the same flee: 14 % | (b) | as the warlock |
| conjurer | the same flee: 8 %. A sprint back the BackOff gate did not offer: 7 % | (b) | as the warlock. BackOff is only offered inside the minimum range of his equipped spell |
| mage-off | cast_heal or cast_control where the oracle flees: 16 % | (b) | as the warlock |
| ranged-off | flee: 7 % | (b) | as the warlock |
| thief | attack where the oracle holds the sneak row: 25 % | (b) | the sneak candidate is generated and scored under the floor. Needs a consideration for "hold still and stay hidden" that can outscore the attack |
| evoker, healer | cast_control where the oracle summons: 9 % and 6 % | (b) | cast_summon is generated and vetoed or under the floor at that moment. Needs a look at the summon family's veto for a class whose rows do not name a summon |
| conjurer, mage, evoker | cast_control where the oracle casts offense: 13 %, 5 %, 4 % | (b) | the offense candidate is under the floor or vetoed (will-work, line of fire or the range veto). Needs the veto's reason logged per candidate to split it |
| every class | a health, stamina or magicka potion the oracle drinks and IAUS scored under the floor: 3 to 9 % | (b) | partly real (the drink line). The magicka potion of a class that cannot cast is an artefact of Utility's "magicka left" term and should not be chased. See the backlog |
| melee, mercenary, archer, evoker, ranged-off | a step back from a foe: 5 to 14 % | (c) | no road today (section 25 of the v2 note: needs a Harbinger combat area centred on a point). GiveGround is lab-only |
| melee, ranged, archer, melee-off | a sprint back: 5 to 8 % | (c) | the BackOff family is never generated for these classes. Its gate is the Mage line and the Mercenary |
| thief, warlock, healer, mage-off, archer, ranged, ranged-off | close on the player: 4 to 22 % | (c) | the road exists (Harbinger ch.24 CombatApproach). The Regroup family is built and never generated. Needs its MFO client (`Snapshot::regroupRoad`) |
| mage, conjurer | a side step to a clear line of fire: 5 % and 7 % | (c) | no IAUS family. The game only holds a cast through a friend |
| ranged | a step to get sight of his target: 9 % | (c) | the road exists for casts only. No IAUS family |
| warrior, melee, knight | the bash on demand 4 to 10 %, the warrior's guard 5 % | (d) | the oracle's bash has no cooldown and the guard no row cooldown. Not a target. Binding them is a backlog item |

A curve shape that cannot express the oracle's choice was not found by this method. The classification sees whether a candidate was ranked, not which response curve would rank it first. The gap stage's (a) search is what finds that: a family whose knobs it sweeps without closing its clusters is the candidate for a new consideration.

### 14.7 The tuning follows the oracle

Checked: every place the driver reads an oracle result runs the new one. The driver never names a K, a horizon or an option limit, so it gets the default.

| Stage | What it reads | Status |
|---|---|---|
| `gap_order` | the per-class gap of a small report run (`class_gaps` of the results file), to tune the furthest class first | the new oracle |
| `stage_gap` | `GAPFIGHT`, `GAPSIT`, `GAPGEN`, `GAPDEC` of `--class-oracle --gap-dump --oracle-subject-only`. The (a) clusters pick the families of its extra search | the new oracle. The dump format is unchanged. `ORCSTOP` and `ORCREFUSED` are new lines the parser ignores |
| `emit_run` | the dashboard's small results file after each sweep (2 natural fights and 1 scenario per class) | the new oracle. It costs more now, one to two minutes of two threads per class |
| The class bands (`load_cards`) | the oracle-relative survival bounds of `Playstyles.h` | re-measured, 14.8 |
| The edge, joint, globals, consts, floor and rating stages | IAUS's own J and the class bands. No oracle | unchanged |

The driver's result cache is keyed by the binary's hash, so no result of the old oracle can be replayed. `EXT_FAMILY` and `EXT_ROAD` now name `ext:clear_line` (it was classified by the generic rule), and a unit test holds both tables to the names in `Oracle.h`.

### 14.8 The survival bounds, re-measured

160 natural fights per class, built-in world, v3, every follower planned by the class oracle, seed 1 (`--class-oracle 160 --gap-scenarios 0`, the `ORCSURV` lines). The run took 97 minutes on 16 cores (an estimated 4 minutes before, from 4.5 CPU seconds a fight). Bound = the oracle's value plus the same margins as before (0.05 downs a fight, 5 points):

| Class | Oracle downs / fight, before | now | Bound before | Bound now | IAUS | Oracle self heals begun under 25 % health, before | now | Bound before | Bound now | IAUS |
|---|---|---|---|---|---|---|---|---|---|---|
| mage | 0.044 | 0.006 | 0.09 | 0.06 | 0.031 | 5.3 % | 4.8 % | 10 % | 10 % | 10.6 % |
| evoker | 0.000 | 0.006 | 0.05 | 0.06 | 0.013 | 1.0 % | 3.3 % | 6 % | 8 % | 15.0 % |
| conjurer | 0.000 | 0.000 | 0.05 | 0.05 | 0.000 | 0.0 % | 1.3 % | 5 % | 6 % | 4.7 % |
| healer | 0.006 | 0.000 | 0.06 | 0.05 | 0.025 | 0.9 % | 2.6 % | 6 % | 8 % | 6.2 % |
| warlock | 0.013 | 0.013 | 0.06 | 0.06 | 0.019 | 1.5 % | 3.2 % | 7 % | 8 % | 13.0 % |

The downs bound is tighter for the mage and the healer and one down in a hundred fights looser for the evoker. The self-heal bound is looser by one or two points for four classes: the stronger oracle heals late a little more often, because it can afford to. With the shipped tunings the evoker and the warlock are still outside the self-heal band and the mage is just outside it. The healer is inside now. All five are inside the downs band.

Gap on these fights (every follower planned, 160 fights): mage +11.22 (se 2.17), evoker +13.22 (2.07), conjurer +9.41 (2.11), healer +9.23 (1.88), warlock +13.23 (2.56).

### 14.9 Checks

- `SIM_CLITEST=1 tools/iaus_sim/run.sh`: CLI-CHECKS PASS. That includes the v2 world against `baseline_builtin.txt` and the v3 world against `baseline_v3_builtin.txt` with 0 metrics changed, so IAUS and both worlds are untouched.
- `test_tune_driver.py`: 26 tests pass (one new: the extended set's names in `Oracle.h` and the driver's two tables agree).
- Determinism: a gap run with `--threads 1` and with 16 prints the same `GAPFIGHT`, `GAPSIT`, `GAPGEN`, `GAPDEC`, `ORCSURV`, `ORCSTOP` and `ORCREFUSED` lines. Only the wall-clock `decide_us` column of the class table differs, as it does on main.
- `--world v2` with the class oracle runs and passes its fact checks.
- No `cat:` refusal on any of the 1,280 measured fights: the generator leaks no candidate past the class gate.

### 14.10 The fix round (Opus review of d8b686a7, marth's rulings of 11-17)

marth's rulings, verbatim: ceiling "128 + fix the noise". "Bash/block cooldowns bind". "Hide/flee gates bind". "It may disengage whenever it wants. That's not flee for its purposes".

**Random streams per actor (the noise fix).** `World.h Fight::split`. A real fight draws from its two streams in world order, exactly as before, so no fight and no baseline moves (the regenerated baselines differ in U lines only, see F3). Inside an oracle rollout every draw comes from a stream of its own per (actor, draw site): the n-th draw of that actor at that site is the same number in every option's rollout of one stream index. There are 36 draw sites (24 of the base world, 12 of v3), each with a fixed number. The actor is the body the draw belongs to: the one who dodges, blocks, thinks, aims, picks a target. What an option does not touch therefore plays out the same in both rollouts, and the paired test compares the options on their own consequences. The streams are 8 bytes of state each (a splitmix counter), created on first use. `--oracle-shared-stream` brings the single stream back as a labelled diagnostic.

**What changed in the action set.**

| Item | Before (d8b686a7) | Now |
|---|---|---|
| `ext:block` (review F2, "cooldowns bind") | any blade, the lab's own 1.5 s guard, the rest only | only a class with a Block default row (`ClassGate::block`, as `Generate.cpp:291`). It IS that row's guard: `Fire` of the Block family (the Harbinger hold of 2.5 s, its renewal, its 1.5 s rest), under the row's cooldown (`blockCooldownSec`, else the tuning's), written to the same cooldown slot the catalogue's Block candidate uses. What the oracle keeps: it may raise the guard when it wants. The generator offers it only against a coming swing or on the guard line |
| `ext:bash` ("cooldowns bind") | no cooldown | `Tuning::bashCooldownSec`, in the Bash family's own slot, and 20 stamina as `Fire` asks |
| `ext:hide`, `ext:flee` ("gates bind", review F4) | the oracle's own ungated runs (anyone in stealth and detected, anyone under 60 % with a foe on him) | not offered. The game's runs are the catalogue's Disengage candidates behind the game's own gates (a sneak class a foe has found. 30 % health and threatened, tree ON, no sneak class) and the oracle takes those. The two names stay in the tables for old logs |
| `ext:sprint_back` ("it may disengage whenever it wants") | movement, anyone | only a class with the disengage flag. No situation gate |
| Targets of an extended action (review F5) | one or two picked for him: the nearest foe to step away from, the strongest foe and the foe on the player for a scroll, a heal scroll only under 70 %, the first healer, a guard only against the foe on him, a bash only at a caster or the foe on him | every target: a step and a sprint away from each foe within 900 units, a scroll at every foe it can work on that he sees and at every hurt friend, every healer, a bash at every foe in reach, a guard with no target |

**What is still part of the model, declared (review F5):**

- The catalogue's candidates are the game's: the generator works on the game's own foe list, which keeps the top five foes (`Prune::kTopFoes`). The oracle sees what the game's snapshot gives.
- A step or a sprint away needs a foe within 900 units to move away from. A move to the player or a healer needs him to be 350 or 300 units from them. These are "there is something to do" conditions, not choices.
- `ext:get_sight` and `ext:clear_line` act on the foe he is fighting. They move him to a line on his target. Another target is the Attack candidate's job.
- A rollout holds an option for 2.5 s and the search is one step deep (14.1).
- The incumbent rule: the action he is on wins when the race has not dropped it and it is within 0.3 U of the best mean.

**Review fixes.**

- F1: a divergence is logged in the gap ledger only when the race DROPPED IAUS's pick. Before, a pick still standing (not told apart from the oracle's) was logged too. The reviewer measured 45 of 194 logged disagreements and 20 % of their cost in that state.
- F2, F4, F5: the table above.
- F3: `Utility` paid every follower for magicka left, so a Knight was paid for a pool he cannot use and his magicka potion (worth 0 in `PotionWorth.h`) was free utility. The term is now 1 for a follower with no class spell he may cast (`Oracle.h Casts`). This touches J. Both baselines are regenerated: only U lines changed, 88 of 17,176 lines in v2 and 89 of 17,178 in v3, by +0.01 to +0.13 U (the largest: archer and thief +0.13 and +0.12 in one bracket. Per class mean +0.01 to +0.07). The three exact pins of the CLI checks run with the legacy potion rate, which keeps its own raw term, and did not move. Open: the potion-worth tables were measured with the old term, so a non-caster's magicka row should be re-measured by the driver's prep stage before the retune reads it.
- F6 (not fixed, stated): an option can be dropped after 8 streams by a test that assumes a roughly normal paired difference, and the looks at 8, 16, 32, 64 and 128 are not corrected for multiplicity. Utility differences have a heavy tail (a down, a player death). So a small share of early drops are wrong. The bound is p = 0.001 per look to keep that share small. No measurement of it exists.
- F7 (stated): the survival bounds of 14.8 and below are measured with EVERY follower planned by the oracle, not the subject only. The companions are oracle-planned too, so the subject is better protected than he would be beside IAUS companions. The gap stage's own runs are subject only.
- F8 (stated): with `--oracle-every` (a diagnostic throttle) a sneak hold lasts until his next decision. Two options with the same outcome on every stream are treated as one (the later is dropped). `edge_search.py:99` writes the same small oracle report run as the driver's `emit_run` and so also runs the new oracle.
- `tune_driver.py FIDELITY` is empty now: no extended action is better than the game's any more, so no divergence is classified (d). A bash the Bash family did not offer is (b) or (c) by the generic rule.

**The final oracle, measured (2026-11-18).** All numbers below are the fix-round oracle on the world WITH the field's hit-size spread (15.5). Built-in world, v3, shipped tunings, seed 1.

*How often the race is decided.* The same 16 natural fights per class, subject only, one shared stream against the streams per actor. Share of decisions the ceiling ended with more than one option standing:

| Class | One shared stream | Streams per actor |
|---|---|---|
| knight | 76 % | 21 % |
| warrior | 75 % | 48 % |
| thief | 78 % | 57 % |
| evoker | 77 % | 67 % |
| healer | 84 % | 28 % |

On the 32 fights per class of the gap table below the ceiling ends 27 to 82 % of decisions (it was 34 to 88 % in the first round with fewer options a decision). The melee line and the healer separate most of the time now. A caster with 15 to 18 options a decision still has 10 of them standing at the ceiling on two decisions in three, with a mean unresolved lead of 0.55 to 0.88 U: many of his options are the same spell on another foe and do differ by less than the noise.

*Gap per class.* 32 natural fights, subject only. IAUS U differs from 14.5 because the world changed (hit sizes) and Utility changed (F3):

| Class | IAUS U | Oracle U | Gap (se) | Ceiling ended | Tunable share (a) | Largest clusters |
|---|---|---|---|---|---|---|
| melee | 78.79 | 89.56 | +10.77 (4.10) | 50 % | 45 % | attack retarget 32 %. A step back, no road (c) 30 % |
| ranged | 73.18 | 73.64 | +0.46 (0.68) | 77 % | 55 % | attack retarget 47 %. A step back (c) 22 % |
| mage | 72.73 | 78.94 | +6.21 (5.21) | 78 % | 72 % | cast_control to attack 34 %, cast_heal to attack 18 % |
| knight | 82.37 | 90.44 | +8.08 (3.70) | 27 % | 85 % | taunt to attack 36 %, attack retarget 33 %, power attack to attack 11 % |
| warrior | 75.80 | 81.43 | +5.62 (2.80) | 50 % | 85 % | attack retarget 60 %, power attack to attack 19 % |
| mercenary | 85.35 | 90.41 | +5.06 (2.94) | 75 % | 43 % | attack retarget 18 %, power attack to attack 15 %. A bash (c) 11 % |
| archer | 70.64 | 77.87 | +7.22 (4.31) | 82 % | 56 % | attack retarget 33 %, attack to a weapon swap 23 %. Close on the player (c) 15 % |
| thief | 88.43 | 89.07 | +0.64 (5.42) | 68 % | 87 % | sneak to a weapon swap 34 %, attack retarget 25 % |
| evoker | 67.08 | 82.37 | +15.29 (6.87) | 68 % | 36 % | a side step to a clear line (c) 10 %, a vetoed summon (b) 10 %, cast_control to attack 8 % |
| conjurer | 78.93 | 84.87 | +5.94 (5.62) | 73 % | 89 % | attack retarget 40 %, cast_control to attack 22 %, to cast_summon 11 % |
| healer | 71.81 | 81.14 | +9.33 (4.35) | 37 % | 57 % | cast_heal to his own AI 18 %, cast_control to attack 17 %, a vetoed summon (b) 12 % |
| warlock | 58.88 | 74.70 | +15.82 (7.78) | 47 % | 60 % | cast_control to cast_heal 18 %, attack retarget 15 %. Close on the player (c) 9 % |
| melee-off | 79.94 | 84.36 | +4.42 (4.41) | 43 % | 69 % | cast_heal to attack 15 %, taunt to attack 14 % |
| ranged-off | 69.18 | 87.71 | +18.54 (5.87) | 69 % | 81 % | cast_offense to attack 33 %, cast_buff to attack 16 % |
| mage-off | 74.20 | 79.36 | +5.16 (5.90) | 69 % | 66 % | cast_control to attack 16 %, to cast_summon 12 % |

The ledger now holds only divergences the race decided (F1), so its shares are firmer and its totals smaller than in 14.6. No (d) class is left.

*Structural limits after the rulings (a tune cannot close them, native follow-ups):*

- No step back road: melee 30 %, ranged 22 %, ranged-off 6 %. The largest single untunable item of the melee cell.
- No Regroup client (close on the player): archer 15 %, healer 11 %, warlock 9 %, evoker 7 %, mage 6 %.
- A bash the Bash family did not offer or has no road for: mercenary 11 %, archer 12 %, melee 8 %, and warrior 10 % and knight 6 % over two clusters.
- A summon generated and vetoed or under the floor: healer 12 %, evoker 10 %.
- No "clear line of fire" family: evoker 10 %.
- A potion scored under the floor: mercenary 21 % in two clusters, melee 9 %, ranged 8 %, mage 7 %, warlock 5 %. The magicka potion of a non-caster no longer pays the oracle (F3), so what is left here is real.
- A scroll with the scroll road off: mage-off 9 %.
- GONE from the list: the flee of the warlock, the healer, the conjurer and the tree-OFF cells (14 to 17 % in 14.6). The oracle no longer has a flee the game does not have, so that was the oracle's own artefact. The thief's sneak hold is gone from the top rows too.

*Refused option offers* (the same 480 fights, subject only): 0 `cat:` refusals. `ext:block` without a Block row: every decision of melee, warrior, mercenary and melee-off, and a few of the ranged cells (it is offered whenever a blade is in hand, and refused for anyone but the Knight line). `ext:sprint_back` without the disengage flag: melee 2820, knight 3379, warrior 1763, mercenary 1257, mage 593, archer 405, ranged 392. Rulings: knight 3705 and warrior 2141 never-retreats, healer 1014 near-player, splash 254 to 891 and forbidden posture 444 to 1064 on each Mage-line cell, no-melee 3 to 79.

*Run time.* Subject only: 51.2 CPU seconds a natural fight over the 15 cells (35.8 in the first round, 3.05 before). Every follower planned, Mage line: 114 CPU seconds a fight (92). The cost went up because every target of an extended action is an option now. A gap stage first pass is about 54 CPU hours, about 3.4 hours on 16 cores per world. `--oracle-ceiling 32` remains the lever.

*Checks on the final tree.* CLI-CHECKS PASS (three new ones: the stop line and the class filter, the stream diagnostic and `--gap-bracket`, the damage report). 26 unit tests pass. A gap run prints the same `GAP*` and `ORC*` lines with one thread and with 16. `--world v2` compares to its baseline with 0 metrics changed and runs the class oracle.

## 15. Incoming damage on followers: the field against the lab (2026-11-17)

marth, verbatim: "Healing seems to come too late ... If it's the same timing a restoration class would have, it's way too late" and "Which tells me the sim is wrong with incoming damage rates or something".

Tools: `tools/iaus_sim/calib_damage.py <run-dir> ...` reads the field side. `sim --damage-report N [--damage-bracket <name>] [--list <world>]` computes the same numbers in the lab. Sources: `_evidence/deck-run-1117/part4`, `deck-run-1117b-full`, `deck-run-1117f` (three Deck sessions of 11-17, the same three followers: Cicero melee, Adelinda ranged, Jesper mage, level 45 and 46).

### 15.1 What the logs hold

No per-hit damage line exists in two of the three sessions. What every session has:

- `[sense]`, one line per follower about every 3.3 s: `hpLoss` is `Confidence::HpLossRate`, the NET health fraction lost over the last 5 s, the loss CAPPED at 25 % of his maximum. It tops out at 5.0 %/s. A one-hit kill reads 5.0. The lab computes the same capped number, so the columns compare.
- `[bleed] DOWN (essential down ...)`. Four lines that read `DOWN (?(9); ...) hp 100%` are life state 9, which the log itself says is not a down. They are not counted.
- Health points of heal recipients (`[heal-obs]`, the `recipient_hp` fact).

Only `deck-run-1117f` has the fill probe's hit lines (`[iaus-fill] hit`, `bIausFillProbe`): 140 lines, 39 of them a foe's hit on a follower, one fight.

Not in any log: a foe's level, who a foe is attacking, a follower's maximum health. So incoming damage per foe class and level, which the brief asked for, cannot be measured from these sessions. What can be measured is below.

### 15.2 Field numbers

| | n | |
|---|---|---|
| In-combat samples (own combat flag, foes > 0) | 749 | 40.0 follower-minutes |
| Share of samples losing health, all | 749 | 10.1 % |
| at 1 to 2 foes | 371 | 4.9 %, mean 0.11 %/s |
| at 3 to 4 foes | 252 | 10.3 %, mean 0.17 %/s |
| at 5 or more foes | 126 | 25.4 %, mean 0.91 %/s, 14.3 % of samples at the 5.0 cap |
| Mean capped loss while losing | 76 | 2.60 %/s (13 % of his health in the window) |
| Downs | 9 | 6 first downs, 3 re-downs within 60 s of getting up at 1 to 3 % health. 0.225 per follower-minute (first downs 0.150). With 9 events the 95 % range is 0.10 to 0.43 |
| First downs by foes count | 6 | all six at 5 or more foes |
| Downs within 12 s of another follower's | 9 | 5 |
| From his last no-loss sample to the down, first downs | 6 | Jesper (cloth, mage) 0.4 s, 1.2 s, 3.2 s. Cicero 5.9 s and 8.3 s. Adelinda 9.1 s |
| Loss between two health points of a heal recipient | 19 | median 15 %/s, p90 57 %/s |

Per-hit lines of 1117f: Jesper (armor 0) went from 172.0 to 5.6 health inside one 0.13 s window, five hit events of a Conqueror Archer with four other hits on him in it, and was down 0.4 s later. Arrows on Cicero (armor 0.51 to 0.63): 18 to 20 a shot most times, 34 to 41 twice, 70 once, about 1.9 s between shots of one archer. One melee blow of a Conqueror Berserker took 92.6 from Adelinda (armor 0.75). Field followers: Cicero at least 426 health, Adelinda 308, Jesper at least 172 (the only value seen for him, so not known to be his maximum).

### 15.3 The lab on the same statistics, before the calibration of 15.5

Tuxborn world (the Deck's list), IAUS, the three T1 cells with their companions, 288 fights per bracket. A bracket is a window of the lab's party-against-foes strength ratio:

| | Field | natural | balanced | strong1 | strong2 |
|---|---|---|---|---|---|
| Downs per follower-minute | 0.225 (0.10 to 0.43) | 0.008 | 0.071 | 0.214 | 0.301 |
| Losing share at 1 to 2 foes | 4.9 % | 12.8 % | 21.7 % | 29.9 % | 25.4 % |
| at 3 to 4 foes | 10.3 % | 17.5 % | 29.0 % | 40.4 % | 33.9 % |
| at 5 or more | 25.4 % | 17.4 % | 24.7 % | 28.1 % | 27.6 % |
| Mean capped loss at 5 or more, %/s | 0.91 | 0.34 | 0.55 | 0.78 | 0.81 |
| At the cap, at 5 or more | 14.3 % | 2.9 % | 3.2 % | 7.5 % | 8.1 % |
| Mean capped loss while losing, %/s | 2.60 | 1.92 | 2.15 | 2.49 | 2.67 |
| Downs under 3.5 s after a no-loss sample | 3 of 6 first downs | 0 of 1 | 4 of 13 | 4 of 34 | 4 of 44 |
| Foes a sample | 1-2: 50 %, 3-4: 34 %, 5+: 17 % | 2.4 mean | 3.7 | 4.7 | 4.6 |

Built-in world, downs per follower-minute: natural 0.021, balanced 0.027, strong1 0.107, strong2 0.170. Its fixture groups never reach five foes.

The lab's followers on Tuxborn: level 29 to 30 (drawn 12 to 45), melee 380 health and armor 0.41, ranged 332 and 0.30, mage 292 and 0.00. Foes level 15 in the natural bracket, 23 in strong1.

### 15.4 What this says

1. **The lab's damage rate is not off by a constant.** In the brackets where the lab's followers go down as often as the field's (strong1 and strong2 on Tuxborn: 0.21 and 0.30 a minute against 0.225), the capped loss at five or more foes is 0.78 to 0.81 %/s against the field's 0.91, and the loss while losing 2.5 to 2.7 against 2.6. Those agree.
2. **The natural bracket is far gentler than the field day.** 0.008 downs a follower-minute on Tuxborn and 0.021 on the built-in world against 0.225. The field is 11 to 28 times that. Every survival band, the heal timing and the oracle-relative bounds are measured on natural fights. So they are measured where almost nobody goes down, and "healing comes too late" cannot show there. This is what the field data supports as the answer to marth's question. Caveat: the field day is three sessions in one hard place (Ash Spawn groups and reavers on Solstheim). It is not a sample of all play. The natural bracket draws any encounter of the list at a random level, wolves included.
3. **The lab's damage arrives too evenly.** At one to four foes a field follower is losing health in 5 to 10 % of the samples. The lab's is in 13 to 40 %. And three of the six field first downs came within 3.5 s of a sample that read no loss, all three the cloth mage. The lab does that for 9 to 12 % of its downs in the strong brackets. The field's shape is quiet, then a burst.
4. **What cannot be decided from these logs.** Point 3 rests on 623 samples for the quiet part, which is solid, and on six downs for the burst part, which is not. The burst has at least three possible causes the logs cannot separate: who the foes pick as a group grows (no log line says who a foe attacks), several hits landing at once (one 0.13 s window, once), and the followers' own health and armor (the field mage has at most 172 health seen once, the lab's 292 at a lower level. The field's ranged follower has armor 0.75, the lab's 0.30). Each would be a different constant. Fitting one of them to six downs would be a guess that reads as a calibration.

The one lever the solid part points at, the foes' preference for the player (`World.h PickTarget`, the 1.25 on a foe's score for the player, a model value), was not moved: raising it makes followers safer in small fights, which the field supports, and safer in large ones too, which the field contradicts.

What was changed is in 15.5: how a hit's size is drawn. That one the hit lines of 1117f do measure.

**What it needs to go further (asks, not done):**

- Run every field session with `bIausFillProbe = 1`. Only one of three had it. Its hit lines give damage per hit, the attacker and the target's health before and after. A dozen fights of that would decide between the three causes above.
- One passive line per foe per few seconds with its current target would settle who the foes pick.
- A ruling on whether the survival bands should also gate under pressure (a second band row per caster on the strong bracket). That is a change of what the tune optimises and was not made here. The gap pass takes `--gap-bracket <name>` so the oracle and IAUS can be read under field pressure. The bracket whose downs match the field: strong1 on Tuxborn (0.214 a follower-minute), strong2 on the built-in world (0.170, the nearest it has).
- Not mirrored: the native heal-timing fix (branch `fix/heal-timing`: a burst-aware dying boost by hits to down, the gross incoming rate, the ally dying fallback). The branch was not on origin when this batch closed. Mirror it when it lands, then re-measure 15.6.

### 15.5 The calibration: the size of a hit (`hittail`)

Input from the Opus heal-timing diagnosis of the same day: single hits take 30 to 97 % of a follower's health in the field, and the game's own per-hit estimates are 4 to 6 times under what was taken.

The hit lines of `deck-run-1117f` give 16 hit windows with damage that foes landed on followers (19:44:45 to 19:45:51, one fight). As a share of the target's health: median 4.7 %, 75th percentile 9.7 %, 90th percentile 30.1 %, the largest 96.7 %. Two of the 16 took 30 % or more. So the usual hit is small and a few are very large. Before armor (the line prints the target's armor share), and over the median of the same attacker kind:

| Attacker | n | Health before armor, over the kind's median |
|---|---|---|
| Arrows of the Conqueror and Ravager Archers (median 53) | 11 | 1.32, 3.13, 0.91, 1.74, 1, 3.55, 1, 1, 1, 2.08, 1 |
| Blows of one Conqueror Berserker (median 54) | 5 | 0.67, 6.76, 1, 1, 0.5 |

Mean 1.73, median 1. Four of sixteen are twice the median or more, three are three times or more.

The lab's foes hit with one size per weapon (a power attack apart). The change: a foe's physical hit on a member of the party is multiplied by one of those sixteen values, drawn evenly, each divided by their mean so the mean multiplier is 1 (`World.h kFieldHit`, the v3 switch `hittail`, its draw from the v3 stream). The mean is 1 on purpose. The lab's damage per second already agrees with the field at matched pressure (15.4, point 1). What differed is how it arrives. So a usual hit is now 0.58 of what it was, and one hit in four is 1.2 to 3.9 times it.

It applies to the player model too, since he is hit by the same foes. It does not apply to spells (no field hit line of a spell on a follower exists) or to the party's own hits.

**Honest size of the evidence:** sixteen hits, one fight, two archer kinds and one melee foe, three followers. The spread is real in that fight. Whether it holds for other foes is not known. `--v3 hittail=off` gives the lab before it, and the comment at `kFieldHit` names the lines to re-derive it from.

Before and after, Tuxborn world, strong1 bracket (the field's pressure), 288 fights, IAUS:

| | Field | Before | After |
|---|---|---|---|
| Downs per follower-minute | 0.225 | 0.214 | 0.217 |
| Mean capped loss at 5 or more foes, %/s | 0.91 | 0.78 | 0.78 |
| At the cap, at 5 or more | 14.3 % | 7.5 % | 7.0 % |
| Losing share, all samples | 10.1 % | 31.2 % | 29.4 % |
| Downs within 3.5 s of a no-loss sample | 3 of 6 | 4 of 34 (12 %) | 6 of 33 (18 %) |
| From above 75 % health to the down, under 1 s | Jesper, 0.4 s | 0 of 34 | 2 of 33 |
| Worst loss inside one second, per follower and fight: 90th percentile, largest | 30 %, 97 % (hit windows) | 26 %, 62 % | 28 %, 100 % |

The burst now exists in the lab (a follower can go from full to down in one second, which it could not), and the share of downs that come out of calm went from 12 to 18 %. It is still about a third of the field's share, on six field downs. The step is small because the mean is held at 1 and because a down in the lab's strong fights is mostly the melee follower worn down, not the mage burst. The capped loss numbers did not move toward the field. They cannot: the game's reader caps a window's loss at 25 %, so it does not see hit size.

What the built-in world's tuning baseline does with it (`baseline_v3_builtin.txt`, regenerated, 8 fights a cell, so each row is noisy): U over the 90 class and bracket rows moved by -4.56 on average (from -15.91 to +4.11), and by -2.76 over the 15 natural rows. The player model dies more often when a hard hit lands on him, and a fight the player loses scores low. The v2 world is untouched (0 metrics changed against `baseline_builtin.txt`). Every J of the v3 world is a new number after this, so the retune starts from the new baseline.

### 15.6 Survival, heal timing and ally survival with the final oracle on the calibrated world

160 natural fights per class, built-in world, v3 with the hit-size spread, every follower planned by the class oracle (review F7: the subject is beside oracle-planned companions here, so he is better protected than beside IAUS ones). The run took 139 minutes on 16 cores. Bound = the oracle's value plus the same margins (0.05 downs a fight, 5 points). `Playstyles.h` carries these numbers.

| Class | Oracle downs / fight | Bound | IAUS | Oracle self heals begun under 25 % | Bound | IAUS |
|---|---|---|---|---|---|---|
| mage | 0.019 | 0.07 | 0.031 | 5.4 % | 10 % | 13.7 % |
| evoker | 0.019 | 0.07 | 0.019 | 2.6 % | 8 % | 7.5 % |
| conjurer | 0.031 | 0.08 | 0.006 | 2.4 % | 7 % | 13.7 % |
| healer | 0.000 | 0.05 | 0.019 | 2.2 % | 7 % | 9.5 % |
| warlock | 0.025 | 0.08 | 0.037 | 4.9 % | 10 % | 15.1 % |

With harder single hits the oracle goes down a little more often than in 14.8 (0.019 to 0.031 a fight against 0.000 to 0.013), so the downs bounds are 0.01 to 0.03 looser. Every class is inside its downs band. The mage, the conjurer, the healer and the warlock are outside the self-heal band with the shipped tunings: they begin a self heal under a quarter of their health two to six times as often as the oracle.

**Heal timing, re-derived (the Healer's and the Priest's `healAt% <= 60`).** The old band capped the MEAN health of the recipient at the heal from above: a healer who healed earlier than 60 % failed it. The oracle heals at a mean of 67 to 75 % of the recipient's health on every caster cell. So that band asked for later heals than the best play. It is replaced on those two rows by `healLate%`: the share of his heals on another that BEGIN with the recipient under 30 % of his health. 30 % is one hard field hit (the 90th percentile of the hit windows of 15.5), and a heal takes about 1.8 s from the pick to landing (the Opus heal-timing diagnosis, 1.0 to 2.9 s), so a heal begun under that line can lose the race with the next hit. The bound is the oracle's share plus 5 points.

| Class | Oracle heals begun under 30 % | Bound | IAUS | Oracle mean health at the heal | IAUS |
|---|---|---|---|---|---|
| healer | 2.9 % | 8 % | 1.9 % | 70.9 % | 68.2 % |
| priest | 1.2 % | 6 % | 2.4 % | 70.6 % | 71.8 % |
| mage (no band) | 1.8 % | | 7.4 % | 69.8 % | 64.1 % |
| evoker (no band) | 1.7 % | | 7.7 % | 74.7 % | 57.9 % |
| conjurer (no band) | 2.3 % | | 7.2 % | 73.1 % | 55.5 % |
| warlock (no band) | 4.9 % | | 2.8 % | 66.7 % | 66.2 % |

On natural fights the Healer and the Priest are inside the new band. The classes that heal late are the mage, the evoker and the conjurer: three to four times the oracle's late share, and a mean 6 to 18 points lower. That matches the diagnosis (the Evoker's heals carry a 0.37 school weight). The five other rows that hold `healAt% <= 75` were not asked for and are unchanged. The same argument applies to them.

**The Evoker's ally-survival band.** New on his row: `dwh/f` (an ally goes down while he holds a heal he could cast) at most the oracle's 0.069 a fight plus 0.05, so 0.12. IAUS is at 0.106, inside. The Healer and the Priest keep their card value of 0.1 (oracle 0.069 and 0.056, IAUS 0.106 and 0.106: both are just outside their own band).

**Under field pressure** (`--gap-bracket strong2`, 32 fights per class, every follower planned. The record, not a band):

| Class | IAUS U | Oracle U | Gap (se) | Allies down while he held a heal, IAUS / oracle | Heals begun under 30 %, IAUS / oracle | Mean health at the heal, IAUS / oracle |
|---|---|---|---|---|---|---|
| mage | 33.02 | 59.97 | +26.95 (6.89) | 0.344 / 0.219 | 3.4 % / 3.4 % | 60.3 % / 66.5 % |
| evoker | 35.48 | 57.15 | +21.67 (9.22) | 0.312 / 0.094 | 17.9 % / 11.7 % | 50.4 % / 60.8 % |
| conjurer | 47.21 | 68.45 | +21.24 (5.96) | 0.250 / 0.156 | 15.3 % / 3.3 % | 49.9 % / 68.6 % |
| healer | 27.45 | 53.30 | +25.85 (7.66) | 0.219 / 0.188 | 3.4 % / 6.5 % | 64.5 % / 67.2 % |
| warlock | 33.56 | 51.29 | +17.73 (6.99) | 0.219 / 0.250 | 2.4 % / 3.8 % | 66.9 % / 63.5 % |
| priest | 45.41 | 66.17 | +20.75 (8.09) | 0.156 / 0.094 | 6.1 % / 1.5 % | 59.3 % / 68.6 % |

This is where marth's observation shows. Under the field's pressure the gap to the oracle is 18 to 27 U (10 to 19 on natural fights), the Evoker lets an ally go down while holding a heal three times as often as the oracle (0.31 against 0.09), and the Evoker and the Conjurer begin 15 to 18 % of their heals on an ally who is one hit from down. 32 fights a class, so each number is rough. The direction is the same on most rows. The warlock and the healer's late share go the other way.

Open after this batch: whether the bands should gate under pressure as well (a ruling), the native heal-timing fix to mirror, and the potion-worth tables to re-measure after F3 and the hit sizes.
