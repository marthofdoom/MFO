# Tight movement control: mechanism trace and build plan

Planner document (Fable plans, Opus codes). 2026-11-24. No product code here.
Bases: Harbinger `origin/main` a8db589e (ABI 47), MFO `origin/main` 33ce987b, dev hold client on `origin/feat/anim-test` 79f72b44.
Evidence: `_evidence/deck-g0b-1124b`, `_evidence/deck-anim-1124c`, diagnosis `_agentlogs/mfo/diag-anim-1124c.md`.
Disk log with every address as it was found: `_agentlogs/harbinger/tight-move-plan.md`. RE helpers: `_agentlogs/harbinger/re2/y.py`, `leafmap.py`, `gmst.py`.

Every claim carries one of three marks.

- **DISASM**: read in `binaries/1.6.1170/SkyrimSE.unpacked.exe`. The address is a 1.6.1170 RVA unless it says SE (1.5.97) or 1.7 (1.7.104).
- **RUN**: a log line in the evidence shows it executing.
- **PROBE**: not known. Section 9 names the smallest passive probe that settles it.

## 0. The short version

1. The engine's combat movement is one tree with one chooser. The Movement Selector re-picks a child every 0.25 s and interrupts the running one (DISASM). Ten children, fixed order. Return To Combat Area sits above the three fighting subtrees, so the ch.24 bound already wins the pick whenever the body is outside it (RUN: asked 4 times a second, answered outside).
2. ch.24 today only moves one input of that chooser (the area). It never chooses. That is why it gives an area and not a point. The return leaf paths to the CENTRE, but the chooser takes it away at the first re-pick after the body crosses R plus the body's own radius. Inside the disc the fighting leaves run again and their destinations are cut at the disc edge. An archer with a foe closer than his range is pushed to the far edge and sits on it. That is the edge stop and the drift, both by disassembly.
3. Melee: outside the disc no tree leaf except the return leaf can be picked. What carries him back onto the foe is outside the Movement tree. The disassembly names one such writer for certain: the engine refuses an attack whose END POINT leaves the combat area, and Harbinger's S5 bracket hides the bound from exactly that test, so lunging attacks out of the disc are allowed today. He also returns facing the foe, at backpedal speed (RUN: 60 to 193 u/s), with the foe following. Which share is lunge and which is something else needs probe P1.
4. EngineDropped: the return leaf's update turns the area OFF when its path reports status 5 (DISASM `0x8AD2F2`). After that the engine no longer updates the area and no longer sees the body as outside. Why the path reaches status 5 inside the first second in melee contact is PROBE P2.
5. Calm style (ch.33) cannot tighten a hold. It changes which in-disc leaf the fighting subtrees pick. It touches neither the chooser, the clamp, the return leaf nor the attack test. Do not use it for holding.
6. The fix is to own the CHOICE, not to fight the leaves. A strict order makes Harbinger answer the Movement Selector's pick for that actor: child 0 (the engine's own wait leaf) to stand, child 4 (the engine's own return leaf, which paths to the point) to go. Nothing is failed, nothing is fabricated, the Action half keeps turning, attacking and casting. The attack end-point test is left to see the bound, so attacks that leave the leash are refused by the engine itself. The area being switched off is undone and reported.
7. ABI 48, on ch.24: one flag and two slots. Placed spells H1 becomes ABI 49 (it is not built yet). Section 6.
8. Tune: movement kinds that need a point, a kite or a disengage must not be tuned before the Harbinger package lands and its dev gambit rows pass. Section 11 says what may run before.

## 1. The movement map for a fighting NPC (1.6.1170)

### 1.1 The tree (DISASM)

Builder of the Movement tree: `0x8AD390`. Root of all combat behaviour: `0x85F7D0`, a Parallel named "Combat Parallel" with two links, Action and Movement.

```
Movement Parallel                                  (Parallel, name string 0x18E55F8)
  Acquire Movement Resource ("Movement", prio 100) (AcquireResource<2>, 0x8ADEEF)
    Movement Repeat
      Movement Selector                            (DynamicSelector<ConditionalChildSelector,float>, vtable 0x18E2198, period 0.25 s at node+0x28, 0x8ADDE7)
        0 Essential Down         -> Idle<float> 10000 s (0x8ADCC0)     value: actor life bits == 0xE00000 (0x8AE750)
        1 Flight                 -> tree "Flight"                      value: 0x661190 and 0x8AE770
        2 Flee                   -> tree "Flee" (0x88BE10)             value: 0x89B910 (state bits)
        3 Hide                   -> tree "Hide" (0x88C8C0)             value: 0x841EF0
        4 ReturnToCombatArea     -> ONE leaf (0x8A7A40)                value: NOT IsActorInArea (0x8AE710 -> 0x55AEC0)
        5 ExitWater              -> leaf                               value: 0x661140 and 0x693ED0
        6 Search                 -> tree "Search" (0x8B3580)           value: [+0xD8] > 0 (0x89E940)
        7 RangedMovement         -> tree (0x8AF660)                    value: 0x8AE7A0 float > threshold (0x8AE640)
        8 FlankingMovement       -> tree (0x883DA0)                    value: combat style flags & 2 (0x8AE790)
        9 CloseMovement          -> tree (0x870D50)                    value: constant 1
  Acquire Weapon Link            -> tree "AcquireWeapon" (0x8627C0)
  Check Unreachable Target Link  -> tree (0x8A7880)
  Dodge Threat Link              -> tree "DodgeThreat" (0x8699B0), takes the Movement resource with AcquireResource<3>
```

Child order is the chain at `0x8ADFF3` to `0x8AE05C`.

How the selector works (DISASM):

- Pick: `0x85AEE0` returns the first child whose vfunc 0x0A answers non-zero.
- Enter: `0x89AD90` picks once (call at `0x89AE22`) and starts that child.
- Update: `0x89D1F0`. While the child runs, when 0.25 s have passed it picks again (call at `0x89D2E3`). A different index means the running child's thread is interrupted (`0x85DF30`) and the new child starts on the next step. It is preemptive.
- Dodge is a sibling of the selector, not a child. It runs beside it and takes the movement resource from it when its own condition holds.

The subtrees:

- CloseMovement: Idle, Backoff, Circle, Fallback, Surround, Reposition, Circle Distant, Advance, Fallback To Ranged, grouped as In Range, Out of Position and Out of Range selectors.
- RangedMovement: Idle, Circle, Strafe, Find Attack Location (tree `0x879980`), Use Cover (tree `0x8BF170`), Maintain Optimal Range.
- FlankingMovement: Watch Target, Flank Distant, Chase, Flank, Stalk.
- Melee action tree (`0x8A46D0`): Special Attack, Attack, Attack Idle. None of them starts a path.

### 1.2 The one road every tree mover uses (DISASM)

Every leaf that moves the body builds a `CombatPath` and starts it with `0x8C3A00`. `leafmap.py` walked all 95 node vtables. 51 call sites, all accounted for. Attack, Bash, Block, Special Attack, Ranged Attack and the cast leaves start no path.

The area enters a path in three different ways. This is the whole of "how the bound is consumed".

| Way | Where | What it does |
|---|---|---|
| A. Link value | `0x55AEC0` IsActorInArea: area vfunc 0x06 (`0x7F9B40`) on the actor's position, pad = `0x672E40(actor)` | inside when dist < R + pad. Only the Movement Selector's child 4 reads it |
| B. Clamp | `0x55B340` -> area vfunc 0x07 (`0x7F9BB0`) | a destination inside the disc is kept. One outside is replaced by the point where the segment from the body to it crosses the sphere (`0x55E850`). No crossing means "stay" |
| C. Path build | `0x8C4ED0` calls `0x55B820` at `0x8C4FE1` | unless the request carries flag `+0x82 & 2` or `+0xBA & 1`: goal outside the area (vfunc 0x06 with the request's own pad) sets path status 5 at `0x8C4FEA`. Otherwise the area constrains the request (vfunc 0x0A `0x7F9D10`) |
| D. Location test | `0x55B310` IsInCombatArea | a yes or no on a candidate location |

### 1.3 The deny-completeness table

"Seat today" is what Harbinger a8db589e does at that writer. "Under a strict order" is section 6. A row with nothing in the last column would be a hole.

| # | Writer | Enter fn (1.6.1170) | Chosen by | Bound today | Harbinger seat today | Under a strict order |
|---|---|---|---|---|---|---|
| 1 | Return To Combat Area | `0x8A6EE0`, update `0x8AD2C0` | selector child 4, way A | goal is the area CENTRE (area vfunc 0x08 `0x7F9CF0`). R is not in the goal | S1 writes the area. S6 observes way A | the only travel leaf. Picked by Harbinger until the client's arrive radius |
| 2 | Advance, Reposition | `0x86D870` | CloseMovement | way C only | ch.23 leash (act and update) | never picked (pick seat) |
| 3 | Backoff, Circle, Circle Distant, Fallback, Fallback To Ranged | `0x870260`, `0x86F7C0`, `0x8708A0`, `0x86E8C0`, `0x86F0E0` | CloseMovement | way B and C | none | never picked |
| 4 | Surround | `0x86E390` | CloseMovement | way D and C | none | never picked |
| 5 | Strafe, Maintain Optimal Range | `0x8AEB30`, `0x8AF040` | RangedMovement | way B and C | none | never picked |
| 6 | Find Attack Location, Find Lateral, Find Ally Attack Location | `0x878E90`, `0x878720`, `0x8792F0` | RangedMovement | C, B and C, D and C | none | never picked |
| 7 | Find Cover, Attack From Cover, Wait Behind Cover | `0x8BDE30`, `0x8BE720`, `0x8BECA0` | RangedMovement Use Cover | way C | none | never picked |
| 8 | Chase, Flank, Flank Distant, Stalk | `0x8827C0`, `0x881D90`, `0x8836F0`, `0x883280` | FlankingMovement | way C. Stalk also `0x55B4B0` | ch.23 leash on the pursuit ones | never picked |
| 9 | Search, Search Center, Search Location, Search Wander, Investigate Door | `0x8B1440`, `0x8B29C0`, `0x8B2330`, `0x8B1EC0`, `0x8B2CF0` | selector child 6 | C. Search Center also D | ch.23 leash | never picked |
| 10 | Flee, Flee To Ally, Flee To Cover, Flee Through Door | `0x88B4A0`, `0x889E20`, `0x88ADD0`, `0x88A600` | selector child 2 | C. Two also D. A fleeing actor's area is not selectable | none (MFO owns confidence) | never picked while the order stands. Wounded retreat is the client's GoTo |
| 11 | Hide | tree `0x88C8C0` | selector child 3 | way C | none | never picked |
| 12 | Exit Water | `0x8A72F0` | selector child 5 | way C | none | PASSES (safety). Reported in the state |
| 13 | Essential Down wait | Idle `0x557570` | selector child 0 | none, it does not move | none | PASSES. It is also the stand leaf |
| 14 | Flight (dragons) | tree | selector child 1 | none | none | PASSES. Strict orders are refused for flying actors |
| 15 | Dodge Threat | `0x868A70` | its own Repeat beside the selector | way B and C: the dodge is cut at the disc edge | none | allowed, cut at the leash. Flag `kMove_NoDodge` denies it (H-M2) |
| 16 | Acquire Weapon (Find Weapon, Move to Weapon, Follow Path) | `0x8610C0`, `0x861EF0`, `0x8A6D30` | its own link beside the selector | D twice, path C | S5 bracket EXEMPTS the search from the bound (APMF-B57) | bracket not opened: the search sees the bound, so nothing outside the leash is fetched |
| 17 | Check Unreachable Target | update `0x8A6660` | its own link | D | deliberately no bracket | unchanged. It does not move the body |
| 18 | Attack displacement (lunge, power attack step, sprint attack) | pick `0x8A28B0`, end-point test `0x8A2EE0`, IsInCombatArea at `0x8A3186`, refuse at `0x8A318F` | Action half, Attack and Special Attack and Bash act | the engine REFUSES an attack whose end point is outside the area | S5 bracket hides the bound from this test, so attacks may leave | bracket not opened: the engine's own veto works against the leash |
| 19 | Animation root motion of an attack already started | animation | n/a | none | none | residue. Bounded by row 18. Measured by P1 |
| 20 | Stagger, knockback, ragdoll, paralysis, bleedout | physics and animation | hit reactions | none | stall floor excused only for animation-driven moves | not denied (they are the world acting on him). The order stands, the wait is excused, he walks back after |
| 21 | Collision push, a player or NPC walking into him, moving platform | character controller | physics | none | none | not denied. Drift past the leash re-picks child 4 |
| 22 | Package procedures (follow, travel, sandbox, escort), scenes, furniture, mount | package and procedure | the package stack. Suspended while the combat controller drives the body | a package Hold Position AREA outranks the standard area by the engine's own priority | ch.24 yields (state Yielded). ch.19 travel and ch.9 are arbitrated in the control map | refused or yielded with a named reason. Never overridden |
| 23 | Low process combat | LowCombat tree `0x89DD30`: Pursue Target `0x89DAC0`, Attack Low, Flee | actor not in high process | area not enabled | none | order reports NotCarried. The follower leash (MFO) owns far-away followers |
| 24 | Facing and turning | CombatMeleeAimController built at `0x8A25B0` (track distance GMSTs 128 and 512), ranged aim, Track Target | Action half | none | ch.29 aim (projectile) | stays with the engine. Disengage needs it released, see H-M3 |

Rows 1 to 11 are one seat (the pick). Rows 16 and 18 are one decision (do not open S5 for a strict actor). Rows 19 to 21 are physical and are handled by re-picking child 4, not by a deny.

## 2. Archer drift: why he stops at the edge

All DISASM unless marked.

1. The return leaf's goal is the centre. `0x8A6EE0` reads the current area (`controller+0x98`), calls area vfunc 0x08 and gets `area+0x30`, the centre Harbinger wrote. It builds the path with a goal radius argument of -1.
2. The leaf never gets there. Child 4's value is way A. At the first 0.25 s re-pick after dist < R + pad the value turns false, the selector picks child 7 (RangedMovement) and interrupts the return leaf. So R enters only through the selector, as the place where travel is cut off.
3. Inside, RangedMovement runs as in any fight. Strafe, Circle and Maintain Optimal Range are way B: every destination outside the disc is cut at the edge. A foe nearer than his preferred range makes him back away, the clamp turns "back away" into "the edge point on that line", and he stands there.
4. The two tests use two radii. The clamp uses R with pad 0. The outside verdict uses R + pad. In the band between them nothing pulls him in. Root motion of his own shots and bashes, dodges and pushes move him through that band.

RUN: arrived bow legs average 0.9 to 1.0 R from the point (236 of 256, 142 to 146 of 150, 60 of 64) and he shoots normally (5 shots in about 13 s). The few large escapes (511, 342, 375) are not explained by rows 1 to 9. Candidates by the table: row 18 (bash or attack with the bracket open, he had "attacks 1 to 2"), row 20, or the area switched off (section 4). PROBE P1.

Tight means a point. With this mechanism a point cannot come from shrinking R alone: at a tiny R the clamp still places him on the edge and the two-radius band is as wide as his own body. It comes from choosing the return leaf until an arrive radius the client names, then choosing the wait leaf.

## 3. Melee pull: 0 of 6 arrivals with a foe within 830 units

What is certain (DISASM, RUN):

- Outside the disc the selector picks child 4 and nothing below it. In the cover leg the value was asked every 0.25 s and answered outside 37 times in 9 s (RUN: "outside answers 37", "standard area CURRENT"). Advance, Chase, Circle and the rest cannot have been picked in that state.
- So the bound does not lose to a movement leaf. It loses to writers that are not movement leaves.

The writers, in the order the evidence supports them:

1. **He returns backwards.** RUN: every return sample is BACKPEDAL at 177 to 180 degrees, 60 to 193 u/s. The Action half keeps him aimed at the foe (melee aim controller, row 24) while the return path moves him, so he retreats at backpedal speed with the foe walking after him at full speed (follower to foe distance stays 103 to 108 units).
2. **Attacks carry him back.** The engine's own rule would refuse an attack whose end point is outside the area (`0x8A3186`). The S5 bracket answers that test from the engine's geometry, so the attack is allowed and its displacement is forward, onto the foe. RUN: a 598 u/s forward burst follows each backpedal stretch, and the leg has 3 attacks. That speed fits a lunge or sprint attack, not a walk. The attribution is an inference from timing: PROBE P1 counts it.
3. **The first second is free.** The engine's own area period is 1.0 s. RUN: at claim time "area carried the bound 0 times, standard area not current", and he covered 230 units toward the foe before the first outside verdict. A strict order writes the pick at once and does not wait for the area update.
4. **In 3 of 6 legs the area was switched off** (section 4). After that he is "inside" by definition and CloseMovement runs freely.

## 4. EngineDropped, 6 of 27 legs: the exact path

DISASM:

1. Return leaf update `0x8AD2C0` runs `CombatPath::Update` (`0x8C37A0`) and reads the status at `path+0x14`.
2. Status 5: `mov byte ptr [area+0x2C], 0` at `0x8AD2F2`. That is the area's active byte. Then SetFailed (`0x85E0A0`) and Ascend (`0x85DD70`).
3. An inactive area's Update (vfunc 0x0B) is no longer called by `CombatController::UpdateAreas` (`0x55BF50`) and it is no longer the current area. `0x55AEC0` answers "inside" when there is no current area, so child 4 is never picked again.
4. Harbinger's S1 sits on that Update, sees nothing for 3 s and ends the claim as EngineDropped. RUN: "engine stopped updating the bound for 3 s", bound applied 1 to 3 times, so the switch-off came within the first 0.25 to 0.75 s.

Where status 5 can come from (DISASM):

- At build: way C, `0x8C4FEA`, the request's tested point is outside the area.
- While following: `0x8C38E9`, the actor's movement controller is not following this request (interface call at `0x8C3841` answers 0 or less) for longer than `path+0x24` seconds since `path+0x20`. Until then the path waits in status 3 and re-requests every 0.25 s (`0x8C38F7`).

Which of the two fires in melee contact, and what `path+0x24` holds, is PROBE P2. The timing (inside the first three updates) fits the build test or a very short timeout. A body locked in an attack or a stagger cannot take a new path, which fits "3 of 6 dropped only with a foe in contact".

The fix does not depend on the answer. The area being switched off is a competing write to the facet the client claimed. Under a strict order Harbinger puts the active byte back on the actor's own combat thread and reports it (section 6.4). The probe decides only the retry pacing.

## 5. Calm style (ch.33): why drift went up, and whether to use it

What a combat style can reach in the Movement tree (DISASM): the Flanking link value (style flags & 2, `0x8AE790`), and the random and value nodes inside CloseMovement and RangedMovement (Backoff, Circle Distant, the Fallback or Circle selector, the Strafe selector). It reaches none of: the Movement Selector's order, way A, way B, way C, the return leaf, the attack end-point test, Dodge.

So a calm style cannot hold anyone. It only changes the mix of in-disc leaves. For a bow follower it lowers Strafe and Circle (short, clamped steps) and leaves more time to Maintain Optimal Range and Find Attack Location, which take longer legs to the edge. Each style change also makes the engine rebuild its cached style on the controller. RUN: n = 4 per arm, bow only, drift 31 against 65 u/s, two calm legs cut short. That is not evidence of anything, and the mechanism gives no reason to expect a benefit.

Ruling asked in section 13: ch.33 is not used for holding. It stays for what it was built for (the melee moves mix). Under a strict order the fighting subtrees are never picked, so the style has no movement effect at all. This closes decision 3 of the movement plan with "no".

## 6. The design: a strict order on ch.24

### 6.1 Principle

The engine has one chooser of movement. A hold claim that leaves the chooser to the engine is a suggestion. The strict order takes the chooser for that actor and gives back only what the client named. Every leaf it picks is the engine's own, entered the engine's own way. Nothing is force-failed (a failed movement child fails the Movement Parallel and kills the Action half, the 2026-10-20 lesson). This is "gate the source": the pick is the source.

### 6.2 ABI 48 (append-only, on ch.24)

- `kApproach_Strict = 1u << 2` in `CombatApproachFlags`. Needs `kApproach_Point | kApproach_Hold`. Without it ch.24 is exactly today's area road.
- `APMF_API_v48 : APMF_API_v47` with two slots.
  - `SetMoveOrder(ClaimHandle, const APMF_MoveOrder*)`. Changes the order of a LIVE claim in place. No re-file, no arbitration pass, no path restart unless the point moved (gate P3 of the movement plan).
  - `GetMoveState(ClaimHandle, APMF_MoveStateInfo*)`. Any thread, a copy under the leaf lock.
- `APMF_MoveOrder` (64 byte POD): `mode`, `flags`, `pos[3]`, `leashR`, `arriveR`, `faceTarget` (FormID, 0 = the combat target), `waypointCount`, reserved. Path mode carries up to 6 further points through a second call with the same struct (waypoint index in `flags`), so the POD stays fixed.
- Modes.

| Mode | Meaning | Pick while dist > arriveR | Pick once arrived | Leaves the leash when |
|---|---|---|---|---|
| `kMove_Free` | fight as the engine likes. `leashR` 0 = no bound, else today's area road | engine | engine | n/a |
| `kMove_Stand` | go to the point and stand on it | child 4 | child 0 | dist > leashR: child 4 again |
| `kMove_GoTo` | the same, and the state says Arrived once. The client then sends the next order | child 4 | child 0 | the same |
| `kMove_Kite` | back away to the point facing the foe, Action half free (he shoots or casts on the way) | child 4 | child 0 | the same |
| `kMove_Disengage` | go to the point with attacks held and facing released (H-M3) | child 4 | child 0 | the same |
| `kMove_Path` | waypoints in order, each with `arriveR`, the last one is a Stand | child 4 to the current waypoint | next waypoint, then child 0 | the same |

- Flags: `kMove_NoDodge`, `kMove_AttackInPlace` (default on: the engine's end-point veto sees the leash), `kMove_AttackFree` (the S5 bracket is opened as today, for a client that wants lunges), `kMove_HoldAttacks` (set by Disengage).
- `leashR` is the exact leash: the largest distance from the point at which he is left alone. `arriveR` is where travel ends. Defaults when 0: `arriveR` 24, `leashR` = the claim's R. `leashR` below `arriveR` is refused.
- State: `mode`, `phase` (Travelling, Standing, Returning, Blocked, Yielded, NotCarried, Displaced), `dist`, `sinceMs`, `blockedCount`, `lastBlockWhy`, `passedThrough` (which native child was let through: Essential Down, Flight, Exit Water).
- `MinReleaseForAbi(48)` and the cast seat style coverage bit `kMoveCaps_Strict` so a client can ask before it files.

Placed spells H1 was planned on "ABI 47", which fire-once has since taken. H1 is not built. Recommendation: this package takes 48 because the tune waits on it, H1 takes 49. They share no struct and no seat. The only meeting point is `APMF_Param.pos`, which ch.24 already reads for `kApproach_Point` and a cast claim will read for its own intent. Two intents, two claims, no conflict.

### 6.3 The seats

| Seat | 1.6.1170 | 1.5.97 | 1.7.104 | Kind |
|---|---|---|---|---|
| M1 pick at selector enter | call at `0x89AE22` in `0x89AD90` | inside SE `0x804D80` | inside `0x8B0230` | `write_call<5>`, byte window proven |
| M2 pick at selector update | call at `0x89D2E3` in `0x89D1F0` | inside SE `0x806810` | inside `0x8B2690` | the same |
| M3 return leaf update, observe and repair | vfunc slot 0x04 `0x8AD2C0` of vtable `0x18E54C0` | vtable `0x169FA78`, slot `0x816470` | vtable `0x1963A88`, slot `0x8C2830` | `write_vfunc`, chains |
| S1 to S6 | existing | existing | existing | unchanged, except S5 is not opened for a strict actor |

The SE and 1.7 call offsets inside the selector functions are NOT derived here. That is the author's first step, per runtime, each with a `VerifiedAddresses` row and a `ProveSite` byte window. A runtime whose rows fail refuses `kApproach_Strict` loudly and keeps today's area road. Nothing is weakened to span runtimes.

The pick thunk (combat thread, the actor's own):

1. `rdx` is the selector node (the thread's current node, `thread+0x138`). If it is not THE Movement Selector, call the original. The node is resolved once at install by walking the built Movement tree for the node of vtable `0x18E2198` named "Movement Selector" with ten children, and checking child 0 is the Idle wait and child 4's subtree is the single return leaf. Any mismatch (another mod rebuilt the tree) refuses the facet.
2. Resolve the actor the way ch.23 does (thread, master controller `+0x158`, controller `+0x20`, attacker handle `+0x28`). Look up the strict slot table: 64 slots of atomics with a sequence word, the ch.33 pattern. No lock, no form lookup, no allocation.
3. No live order, or mode Free: original.
4. Call the original first and keep its answer `n`. If `n` is 0, 1 or 5 (Essential Down, Flight, Exit Water): return `n` and note it. These pass.
5. The controller's current area is not the standard area carrying our bound (a package area outranks, the area is disabled, low process): return `n` and mark Yielded or NotCarried. Returning 4 here would enter the return leaf on somebody else's area or on none.
6. Read the actor's position and the order's point. Travel phase or dist > leashR: return 4. Else return 0.
7. Hysteresis is the pair (arriveR, leashR). The phase lives in the slot and flips only in this thunk.

Why child 0 is a safe stand: its node is `Idle<float>` with 10000 s (`0x8ADCC0`), act `0x557570` pushes 8 bytes and starts a timer, update `0x557FD0` only checks it. No path, no resource beyond what the selector already holds. The selector still re-picks every 0.25 s, so a push past the leash is answered within a quarter second.

Why child 4 is the right travel leaf: it is the engine's own pather to a location, it reads the centre S1 already writes, and it needs no new engine call. Going beyond what the engine would do is only in WHEN it is picked.

### 6.4 The other rows

- **Attacks (rows 18, 19).** For a strict actor S5 does not open its bracket on Attack, Bash and Find Weapon unless `kMove_AttackFree`. The engine's own veto then tests each attack's end point against the leash with pad 0. He attacks in place when the foe is in reach and the attack ends inside the leash. He does not lunge out. S3 (target score) and S4 (invisibility and bound item casts) stay bracketed: the bound must not leak into target choice or casts.
- **Area switched off (section 4).** M3 runs the original update, then, if the area's active byte went 1 to 0 on a strict actor's bound, writes it back, counts it, stores `lastBlockWhy` from the path status fields, and logs one line (rate limited per actor). Phase Blocked. Three blocks inside 10 s with no approach progress end the claim as Unreachable, loudly. This is not a mask: the block is reported every time and the client sees the count. It is the deny of a competing write.
- **Stall floor.** Excused while the actor is staggered, knocked down, ragdolled, paralysed, in bleedout or animation driven, capped as today. Phase Displaced.
- **Dodge.** Allowed inside the leash by the engine's own clamp. `kMove_NoDodge` needs the Check Should Dodge value seat. H-M2, after P1 shows whether dodges matter.
- **Disengage and facing.** Holding attacks is ch.7 (existing). Turning his back needs the aim released while he travels. Which object keeps the melee aim between attacks, and whether denying the Attack leaves drops it, is P3. H-M3.
- **Release.** Clear the slot (sequence word), the next pick is the engine's. S1 restores the engine geometry as today. The standing child is interrupted by the selector's own re-pick within 0.25 s. No tree restart, no forced fail.

### 6.5 Lifecycle and edge cases

| Case | Handling |
|---|---|
| Save | orders are not saved. `RestoreBeforeSave` as today, plus the active byte is left as the engine had it. The slot table survives the save, since the game keeps running |
| Load, revert, new game | `ResetAll` clears the slot table. The client re-files (MFO does this for every claim already) |
| Cell change, 3D unload | point claims end as TargetLost when the actor's space is not the point's (existing). APMF-B176 applies unchanged |
| Death, bleedout | death ends the claim. Bleedout is child 0 passing, order kept |
| Mount, furniture, scene, dialogue, kill move, swimming, flying | filing refused with a reason. A live order on an actor entering one of them goes Yielded and returns when it ends, or ends after 10 s |
| Stagger, ragdoll, paralysis | Displaced, excused, walks back after |
| Stairs, ledges, doors | the return leaf uses the engine's pathfinder, so stairs and open doors are its normal work. A closed or load door between him and the point is a path failure, reported as Blocked, then Unreachable. The client must pick points in his space (MFO's spot picker already does) |
| Moving platform | not supported. The point is a world position. The leash re-pick makes him walk after it badly. MFO does not file strict orders for an actor standing on a moving reference (client rule, sim ignores it) |
| The player or an NPC stands on the point | he stops where the path ends against the body. If that is inside `leashR` he stands. If not, Blocked after the path gives up, and the client re-plans. No pushing |
| Player far away | the ch.23 leash outranks by priority as today (APMF-B55 clamp). A strict order whose point lies outside the leash is refused at filing |
| Combat ends | ch.24 waits out of combat and moves nothing (RUN, the idle legs). The order ends with the claim |

### 6.6 Conflicts

| With | Rule |
|---|---|
| Cast seats (self, aimed, line of fire) | Action half only, untouched by the pick. A cast that needs line of sight from a spot is the client's choice of spot. ch.25 holds still apply |
| ch.8 allow-list | none. See section 7 for its own fault |
| ch.24 arbiter in MFO (G2 `MoveClaim`) | the strict order is one more row and the ONLY row that sets the flag. Old clients keep the area road until G5 moves them |
| Offense approach (RUN: it ended a doorway leg after 4 s) | with G2 the offense approach asks the table. A standing strict order wins unless the client's own decision was Free |
| Loot travel, errands (ch.19) | out of combat roads. The control map already refuses ch.24 against a live travel of another client. Same client: the travel yields while he fights |
| Taunt | a target and action matter. A taunting tank holding a spot is Stand plus the taunt. If the taunt rule wants him to close, the client sends GoTo or Free |
| Wounded retreat, flee | the engine's Flee is never picked under an order. MFO's retreat becomes a Disengage order to a spot, then Stand. The existing `fix/retreat-hold-until-flee-ends` rule is kept until G5 moves retreat over |
| Hold-position gambit rows | Stand at his current position |
| Followers far from the player | the leash, as above |
| Other mods moving NPCs | a package area outranks (Yielded). A mod that hooks the same selector functions shows up in the byte window and the seat refuses. A mod that teleports or pushes him is row 21 |
| Combat substrate (ch.27) movement hold | it holds the Movement Parallel, so the selector never runs. A strict order on a substrate holder is refused |
| Held engagement (ch.21) internal bound | internal bounds are consulted only when no client claim is live (existing). A strict order replaces the bleed-out drive for that time |

## 7. Two Harbinger faults

### (a) ch.8 allow-list denies weapon enchantments (diag F1, SEV-2)

- Site: `core/CastGate.cpp`, `CheckCastThunk` (`:112`), the allow-list decision around `:230` to `:296`, deny line at `:295`. RUN: `CheckCast DENIED` formType 21 on the right hand, 64 times throttled, enchant casts 0 of 22, 0 of 19, 0 of 72 with an allow-list against 33 of 53 without.
- Cause: every form that reaches a hand caster's CheckCast is treated as a cast the list governs. A weapon's enchantment applied on hit also goes through that caster.
- Fix: before the allow-list is asked, pass through to the original when the form is an EnchantmentItem (formType 21), the caster is a hand caster, the object in that hand is a weapon whose enchantment is that form (base enchantment or the instance's extra enchantment, which is where the FF ids come from), and the weapon is not a staff. A staff's enchantment is a cast and stays governed. The pass is not a served claim and is counted on its own heartbeat line.
- Verify first (author): the equipped object and its extra list read on the combat thread without a form lookup (the hand's `GetEquippedObject` and entry data, the pattern `EquipAuthority` uses), and the staff test (weapon type) on all three runtimes.
- Tier B (logic inside an existing seat), Opus codes because it sits on the cast gate, one Opus review. About 150 lines with the host test.
- Test, dev gambit `ench.on_hit`: any follower, lend an enchanted one-hander, give him one gambit cast row so an allow-list stands, any foe. Pass: enchant applications per weapon hit at least 0.8 of the control ratio measured in the same test with the row off, and 0 DENIED lines for formType 21 on a non-staff. Second leg with a lent staff: the staff is still denied when not on the list.

### (b) Row equip admission for Ward, Summon, Bound Item (diag C, A residual, D)

- Sites: `core/EquipGate.cpp`, the engine verdict log at `:839` to `:845`, the row admission at `:847` to `:850`, `g_rowItemVts` at `:234`, filled at `:1114` to `:1121`. Engine conditions on 1.6.1170: Ward item 0x0F `0x8334D0` -> `0x81ECC0`, Summon `0x8318F0` -> `0x8201F0`, Bound Item `0x82C760` -> `0x821710`.
- Step 1, the probe (ships first, alone): one `LogDue` line with the engine's 0x0F verdict for a live claim's driven form on a row that has no admission, naming the row. The Ward "NO against a mudcrab" is still an inference. One session settles it for Ward, Summon and Bound Item, now that MFO's `fix/summon-claim` keeps the summon claim alive long enough to reach the seat.
- Step 2, the admission: add the Magic and Staff item vtables of Ward, Summon and Bound Item to `g_rowItemVts` with `kCastSeat_Ward`, `kCastSeat_Summon`, `kCastSeat_BoundItem` (bits exist, `APMF_API.h:3916` to `:3921`). Same contract as v46: only the claim's driven form, on its own hand, while the cast claim wins, and the conditions that protect the world are kept and re-asked by Harbinger (not fleeing, affordable, not already applied). What is dropped is the engine's opinion of whether the cast is worth it: the ward's threat test on the target (`0x856200`), the summon's state gate, the bound item's own gate. The client decided that.
- Armor, Cloak, Invisibility: Armor was picked by the engine on its own in the evidence (Oakflesh fired). Recommendation: include them in the probe line now, admit them only if the probe shows a NO on a claimed form. Invisibility's 0x0F also reads the combat area (S4), so its admission is written with the bracket in mind.
- Tier A (it widens a seat's forced verdict and the coverage mask). Opus codes, Opus reviews. No ABI number: bits 2, 3, 7 gain the "equip admitted" meaning under the existing `kCastSeat_TypeEquip` bit, documented. About 250 lines.
- Tests, dev gambit: `anim.ward_claim` (pass: Ward seat 0x06 YES within 2 s of the claim and a charge), `anim.summon_gambit` and `anim.summon_iaus` (pass: caster fired at least 1 and a body), `bound_bow.fires` (pass: bound bow in hand). Each on any follower with lent spells and a spawned foe.

## 8. Packages, in order

| # | Package | Repo | Tier, author | Size | Needs | Proves |
|---|---|---|---|---|---|---|
| H-P | Probes P1, P2, P3 (section 9) and the 7(b) verdict line | Harbinger | B, Opus (combat-thread counters on existing seats) | 250 lines, about 250k | nothing | which writer pulls melee, why status 5, what holds the aim |
| H-F | Fault 7(a) | Harbinger | B, Opus | 150 lines, about 200k | nothing | enchantments back |
| H-M1 | The strict order: flag, slots, pick seats M1 and M2, M3 repair, S5 rule, stall excuses, three runtimes, docs, deny audit row | Harbinger | A. Fable plan = this document plus a seat brief, Opus codes, Opus reviews | 900 to 1,200 lines, 0.9M to 1.3M | H-P's session read (does not block the code, blocks the merge) | Stand, GoTo, Path, Kite as "GoTo facing the foe" |
| M-1 | MFO adoption: `apmf/MoveOrder.cpp` (one sender), the strict row in `MoveClaim` (G2 step 1 first if not landed), `DevHold` modes, `TestsStrict.cpp` | MFO | A for the bridge and G2, B for the tests. Opus, then Sonnet for the rows | 700 lines, about 600k | H-M1 header | the dev gambit rows of section 10 |
| H-F2 | Fault 7(b) admission | Harbinger | A, Opus | 250 lines, about 350k | H-P's verdict lines | ward, summon, bound item cast |
| H-M2 | `kMove_NoDodge` (the dodge condition seat), only if P1 shows dodges in the residue | Harbinger | A, Opus | 200 lines | H-M1, P1 | |
| H-M3 | Disengage facing: release the aim while travelling | Harbinger | A, Fable RE note then Opus | unknown until P3. Estimate 300 to 500 lines | H-M1, P3 | turn and run |
| S-M | Sim: strict executor, modes, residue table, scenario set (section 11) | MFO sim | sim rule: Fable plan, Opus codes | 800 lines | H-M1's order vocabulary (the header, not the DLL) | lab equals game |
| M-2 | G3 pass and G5 moves use orders, kind by kind | MFO | as the movement plan | as planned | M-1, S-M | |

H-P and H-F can start now and run side by side (two agents, one per tree is not possible in one repo, so H-F goes in a worktree of its own). H-M1 starts as soon as marth rules on section 13. One agent per build tree throughout.

Extension points (principle 12): a new mode is one row of the mode table in `core/MoveOrder.h` (pick while travelling, pick when arrived, flags). A new passing child is one entry of `kPassChildren`. Radii defaults live in `MoveTuning`. In MFO every sender goes through `apmf/MoveOrder.cpp`, and kinds map to modes in one table.

## 9. What must be probed first

All three ride seats that exist. No hotkey, config-gated (`[Probe]` keys, default 0), rate limited, read-only.

- **P1 leaf histogram and attack displacement.** `ActionGate` already runs at every leaf act. While an actor has a live ch.24 claim, count acts per leaf name and, on the game thread in ch.24's Poll, add up displacement toward and away from the point split by "attack state set" and "not set". One summary line at claim end. Answers: section 3 item 2, the escapes in section 2, whether Dodge matters. Size: about 80 lines.
- **P2 return path failure.** One observe-only vfunc seat on the return leaf's update (the M3 seat, shipped first as a probe). On status 5: log the previous status, `path+0x1C` to `+0x2C`, the request flags `+0x82` and `+0xBA`, the actor's attack and knock state. Answers section 4. About 60 lines plus the address rows.
- **P3 aim holder.** In the gait probe line add: which aim controller class the controller holds (vtable RVA only) and whether an attack leaf is running. Compared between backpedal and forward samples it shows whether the backwards return is the aim controller's doing. About 30 lines.

One deck session with the existing `g0b.*` rows is enough for all three.

## 10. Dev gambit tests that prove strict holding

Rows in `selftest/TestsStrict.cpp`, driven through `DevHold` in strict mode. Any follower, any place: the test lends the weapon it needs (bow or one-hander), takes the point from the follower's own trail or the navmesh around him, spawns its own foe of the needed kind (melee rusher, archer) and marks it for deletion. Knock, stagger and ragdoll time is measured apart and excluded from "inside".

| Row | Setup | Pass |
|---|---|---|
| `strict.stand_bow` R 64 and R 32 | bow lent, melee foe that reaches him | inside leash at least 97 percent of held time, max distance at most R + 15, shots per minute at least 0.7 of his free-fight rate in the same test |
| `strict.stand_melee` R 64 and R 32 | one-hander lent, melee foe in contact the whole leg | inside at least 95 percent, max at most R + 20, attacks per minute at least 0.6 of free-fight, 0 forward displacement beyond the leash while attacking |
| `strict.stand_melee_far` | foe held 600 units off | 0 steps toward the foe, inside 100 percent |
| `strict.goto` 5 legs | points 200 to 600 units along his trail, foe in contact at the start for melee, at range for bow | arrived 5 of 5, stop error at most `arriveR` + 10, time at most 1.3 times path length over his run speed plus 0.5 s. Kite legs are judged against backpedal speed |
| `strict.kite` | bow, melee foe closing, 3 points each 300 units further from the foe | he faces the foe at least 90 percent of travel time, at least 1 shot per 4 s while travelling, arrives 3 of 3 |
| `strict.disengage` (after H-M3) | melee foe in contact, point 600 units away | 0 attacks started in travel, faces travel direction at least 80 percent, arrives, time at most 1.3 times run time |
| `strict.path` | 4 waypoints from the trail | each reached in order, none skipped |
| `strict.keep` | Stand for 30 s, no call after filing | still inside, 0 path restarts logged |
| `strict.release` | Stand, then release with a foe 400 units off | a fighting leaf act within 1 s, area geometry bit-equal to the engine's, no tree restart line |
| `strict.free` | order Free with leash 0 | leaf histogram matches an unclaimed fight (same top 3 leaves) |
| `strict.blocked` | point behind a closed door | Blocked reported inside 3 s, Unreachable after three, 0 EngineDropped |
| `strict.save` | save during Stand, load | no claim after load, engine area as saved, then re-file works |
| overall | all rows above | EngineDropped 0. Blocked legs under 5 percent on trail points |

The old `g0b.*` rows stay as the area-road reference.

## 11. What the sim must change

**Built on `feat/sim-strict`: `iaus-sim-v4.md` section 21, differences in 21.8.**

The rule from the movement plan stands: decisions are pure code in `native/iaus`, the sim compiles the same files, the game side only executes. What changes is the executor model in `tools/iaus_sim` (`Move.h`, `MoveWorld.h`, the S2 pass world, S5 oracle) and the vocabulary S1 emits.

1. **Orders, not claims.** S1's output per follower per window is an order: mode, point, `leashR`, `arriveR`. The same enum as `APMF_MoveOrder`. Golden vectors (G3) carry the order.
2. **The strict executor.** Travel along `FindPath` at his run speed to within `arriveR`, then no movement at all until displaced. A push beyond `leashR` is followed by a return after `selectorLag`.
3. **The residue table** (`EnvTuning`, one table, every row a field measurement or a labelled placeholder until its dev gambit row reports):

| Row | Meaning | Placeholder | Measured by |
|---|---|---|---|
| `selectorLag` | time from a condition to the engine acting on it | 0.25 s (DISASM, the selector period) | fixed |
| `areaLag` | first carry of a new point | up to 1.0 s on the area road, 0 with the pick seat | `strict.goto` |
| `arriveError` | where he stops inside `arriveR` | uniform 0 to `arriveR` | `strict.goto` |
| `standDrift` | movement while standing | 0 | `strict.stand_*` max distance |
| `kiteSpeed` | travel speed while facing the foe | 150 u/s (RUN: 60 to 193) | `strict.kite` |
| `attackInPlaceRate` | attack rate when lunges are refused | 0.6 of free | `strict.stand_melee` |
| `blockedShare` | orders that end Blocked | 5 percent | overall row |
| `displaceReturn` | time lost per stagger or knockdown | stagger length + `selectorLag` + walk back | `strict.stand_*` |

4. **Free fight stays the engine's.** Mode Free in the sim is the existing engine movement model (close, circle, range keeping). It is not made tighter.
5. **Kinds gate on caps.** `RoadCaps` gains `strict`, `kite`, `disengage`. The oracle and IAUS are refused a kind whose cap is off, as today.
6. **Scenario variety** (marth: the right mode depends on enemy type, terrain and so on). The `spots` stage and the oracle need fights in which each mode is the best answer, or the tune learns one habit. Minimum set, each axis crossed where it makes sense:
   - Enemy kind: slow heavy melee with long reach (giant, troll), fast melee pack that outruns him (wolves, skeevers), a single duellist, shield melee, archers, a mage with a bolt, a mage with an area spell, a mage with a concentration stream, a summoner, a charger (sabre cat, bear), a boss with adds, a flyer.
   - Enemy speed against his: slower, equal, faster. This decides kite against stand more than anything else.
   - Enemy count: 1, 2 to 3, 6 or more.
   - Terrain: open ground, corridor, doorway chokepoint, room with pillars, stairs and a ledge above, a bridge, a dead end, a slope, water edge, a closed door between the groups.
   - Party: alone with the player, with a tank holding, with a healer who needs a line, two ranged followers who must not share a spot.
   - Player: near, at the leash, moving away.
   - Light, for the sneak kinds (already in S4).
   Acceptance for the set: with the oracle unlimited, each of Stand, Kite, Disengage and Free is the best choice in at least 10 percent of scenarios for at least one class, and no mode is best in more than 60 percent. A set that fails this is not varied enough to tune on.
7. **Parity re-check.** After the dev gambit session, `calib_point.py` rewrites the residue rows. If any measured row is outside its placeholder by more than its noise, the `spots` stage reruns on the new binary. Unchanged from the movement plan, section 7.

### What the tune may run on before the Harbinger work lands

- Everything that does not depend on where he stands: `follow`, `spells`, loot, potions, buffs, target choice.
- Mode Free for every class.
- Nothing else. Stand, GoTo, Kite, Disengage, cover, chokepoint, flank and high ground weights tuned against the area road would be tuned for a body that sits on the edge of a disc at 0.9 to 1.0 R, arrives 0 of 6 times in melee contact and loses the claim in 22 percent of legs. Those numbers are too far from "stands on the point" for any weight to carry over. The `spots` stage waits for H-M1, M-1 and the measured residue table. S-M can be written and reviewed meanwhile, with placeholders, so the stage starts the day the session is read.

If a residue remains after H-M1 (it will: staggers, started attacks, pushes), the sim does not assume it away. It runs the measured `standDrift`, `displaceReturn` and `blockedShare`, and the oracle plays under the same rows.

## 12. Risks and traps

1. **Failing a movement child.** Never SetFailed inside the Movement tree for a strict actor. It fails the Parallel and the Action half dies each frame (2026-10-20). The design returns indexes only.
2. **Returning child 4 without our area current.** The return leaf fails at enter when there is no area and paths to somebody else's centre when a package area is current. Rule 5 of the thunk.
3. **The node identity.** There are two nodes named "Movement Selector" (the second is inside CloseMovement, built at `0x8719E1`). Vtable, child count and the two child checks decide, not the name alone.
4. **The pick function is shared.** `0x85AEE0` serves every conditional selector, the flight tree included. The seats are the two call sites inside the dynamic selector's own functions, and the thunk leaves any other node alone in one compare.
5. **Combat thread.** The thunk reads atomics and the actor's position. No form lookup, no lock that the game thread holds across an engine call, no log except through the rate-limited ring.
6. **The area repair hides a real dead end** if it has no bound. Three blocks, then Unreachable, always logged.
7. **Attack veto too tight at a very small leash.** At R 32 an attack with 40 units of displacement is refused even when harmless. `strict.stand_melee` measures the attack rate. If it is under the pass number the client raises `leashR` for melee or sets `kMove_AttackFree` knowingly. Not a framework default.
8. **Kite is slow.** 60 to 193 u/s against a foe at 370. The sim must know (`kiteSpeed`), or the oracle will kite things that cannot be kited.
9. **1.5.97 and 1.7.104 call sites** are underived. CommonLib has no declaration for any of this. Each is a row with a byte window, proven on the unpacked executable, before code.
10. **G2 is still the gate in MFO.** A strict order filed by a sixth client beside five pairwise-ruled ones would be released by the offense approach again (RUN: the doorway leg).
11. **Two APIs for one thing.** After G5 nothing in MFO should file ch.24 without going through `MoveOrder`. Until then both exist, on purpose, and the table says which kind uses which.

## 13. Decisions for marth

1. **ABI numbering.** A: strict movement is ABI 48, placed spells H1 moves to 49. B: H1 keeps 48 and movement waits. Recommend A. The tune waits on movement, H1 is post tune.
2. **Attacks while holding.** A: default is attack in place, the engine's own end-point rule enforces the leash, lunges out are refused. B: default lets lunges through as today and the client opts into in-place. Recommend A. It is what "no chasing" means and it is the engine's own rule, not a new one.
3. **When the engine switches the area off.** A: Harbinger puts it back, reports Blocked each time, ends as Unreachable after three in 10 s. B: end the claim at once as today and let the client re-file. Recommend A. B costs the 3 s stall plus a re-file each time and is the 22 percent.
4. **Flee and Hide under an order.** A: never picked, the client's retreat is a Disengage order. B: the engine's Flee passes like Essential Down. Recommend A. A strict order that the confidence roll can cancel is not strict.
5. **Dodge.** A: allowed inside the leash now, a deny flag later only if P1 shows it matters. B: denied from the start (one more seat in H-M1). Recommend A.
6. **Calm style.** Rule that ch.33 is not used for holding, and close decision 3 of the movement plan as "no". Recommend yes.
7. **Order of work.** A: probes and the enchant fix now, H-M1 right behind, the `spots` tune after the dev gambit session. B: tune `spots` now on the area road and re-tune later. Recommend A. B tunes weights for a body the game will not have.

## 14. Engine notes for Linux-Native-Tools (reusable, no game data)

Suggested file: `combat-movement-tree.md` at the repo root. Text below is ready to copy.

> **Skyrim SE combat movement: the tree, the chooser and the combat area.**
>
> A fighting NPC's `CombatController` runs a behaviour tree whose root is a Parallel with two halves, Action and Movement. They are independent threads of the same tree. Failing any child of a Parallel fails the Parallel and interrupts its siblings, so force-failing a movement node also ends the action half for that step.
>
> The Movement half is: acquire the "Movement" resource, repeat, then one dynamic conditional selector with ten children in fixed priority order: essential-down wait, flight, flee, hide, return to combat area, exit water, search, ranged movement, flanking movement, close movement. Each child has a value node. The selector takes the first child whose value is non-zero. It re-evaluates on a period stored in the node (0.25 s) and interrupts the running child when the answer changes. Close movement's value is the constant 1, so it is the default. Beside the selector, under the same Parallel, run the acquire-weapon subtree, the unreachable-target check and the dodge subtree. Dodge takes the movement resource from the selector when it fires.
>
> Every leaf that moves the body builds a `CombatPath` and starts it through one function. Attack, bash, block and cast leaves do not. A path has a status: 1 requested, 2 following, 3 waiting to re-request, 4 complete, 5 failed, 6 queued. It fails when the actor's movement controller does not take the request up before a timeout, or at build time when its tested point lies outside the current combat area.
>
> The combat area (`CombatAreaRadius` family) is consumed in four ways. (1) The return-to-area child's value: actor position against radius PLUS the actor's own radius. (2) A clamp used by the short tactical moves (circle, strafe, back off, fall back, maintain range, dodge): a destination outside the sphere is replaced by where the segment from the actor to it crosses the sphere. (3) The path build test above. (4) A plain location test used by surround, ally attack location, search centre, find weapon, some flee leaves, and by the melee attack picker for the attack's END POINT (actor position plus the attack's displacement): an attack that would end outside the area is not picked.
>
> The return-to-area leaf paths to the area's CENTRE. It stops at the edge only because the selector takes it away once the actor counts as inside. If its path fails, the leaf clears the area's active byte. The area then no longer updates and no longer counts, so the actor is "inside" from then on.
>
> Consequences for anyone steering NPC combat movement. Changing the area gives an area, never a point: the actor is cut loose at radius plus body and then rides the edge the clamp gives him. Combat style data only changes the mix of leaves inside the fighting subtrees and cannot hold anyone in place. The clean control point is the selector's choice: its first child is a pure timed wait (a native stand still) and its fifth child is a native go-to-centre.
>
> The idle wait node (`CombatBehaviorIdle<float>`) pushes a timer and checks it. It starts no path and takes no resource.
>
> Method note: list every caller of the path start function and map each to its node vtable by walking the vtable's act and update slots. That gives the complete set of tree movers in minutes and is the right first step on any new runtime.

Addresses for our three runtimes stay in Harbinger's `Docs/VERIFIED-ADDRESSES.md` (rows to add with H-P and H-M1) and in the disk log, not in the knowledge repo.
