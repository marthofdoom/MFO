# S5: the oracle over places and modes, the `spots` stage, and what the one T1/T2 tune may stand on

Planner document (Fable plans, Opus codes). 2026-11-25. No product code here.
Base: MFO `origin/main` 6d2f6db9 (the merge of `feat/sim-strict`). Harbinger `origin/main` read for its `[move]` lines (`native/core/MoveOrder.cpp`).
Replaces: `movement-plan.md` S5, and the cost table of its section 6. Extends: `tight-movement-plan.md` 11, `iaus-sim-v4.md` 21.
Read with: `_agentlogs/mfo/review-sim-strict.md` (T1 to T3, F1 to F10), `_agentlogs/mfo/review-sim-pretune.md` (P1 to P4), `REVIEW-BACKLOG.md` MOVE-S2 and HEAL-MOVE.
Disk log: `_agentlogs/mfo/s5-plan.md`. Timing evidence: `/mnt/gaming/modlists/Projects/_tuning/s5-plan-timing/`.

Marks: VERIFIED (read in code or measured for this note), REVIEW (from the Opus review, not re-run), ESTIMATE, PLACEHOLDER, FIELD (needs a Deck log).

marth's rulings this plan is bound by: T1 and T2 behaviour only. "Likely the last time this behaviour needs to be redone." The oracle has unlimited tries and only the class's abilities. As many scenario types as possible. Hold, kite, disengage or free is chosen by enemy and terrain. A hold is strict. The oracle decides movement. The game serves Stand now, Kite with H-M2, Disengage later, so the lab's default caps are `strict,kite`. The `spots` stage waits for the field residues.

## 0. The short version

1. S5 is six packages, about 3,400 changed lines, not the 1,400 of the movement plan. The extra is what the review found: the oracle's subject holds no claim, Free against Hold is three hand rules, the acceptance was a tournament of constant rules, the scenario set fights 2.9 percent of its cells, and the driver has three holes that let a worse table through.
2. The order is fixed by one fact: **the tune's cache dies with the sim binary.** So every row the executor may ever need (fear, strafe, 3D arrival, re-file, short stops) is coded BEFORE the freeze with a placeholder value, and field numbers enter afterwards only as `--residue` VALUES, which are in the cache key of road-on fights alone. Then the Deck can report whenever it reports and nothing already computed is lost.
3. All search stages except `spots` run with the road OFF. That is the game as it ships while `bIausMovePass` is 0, it is 2.3 times cheaper for a blade and 1.6 times for a caster by the measurement of section 7, and it can start as soon as the S5 binary is frozen. It does not wait for the Deck.
4. The `spots` stage, the road-on joint check and the road-on gap wait for the field residues. The sensitivity arms of section 9 say WHICH rows they wait for. Rows that flip nothing do not hold the stage.
5. The oracle races orders (mode x place) together with his three best actions, at the pass's own windows and no oftener, under the same caps and the same residue table as IAUS. Hold becomes a scored spot kind. `StayMode`'s three rules are removed.
6. The scenario set is trusted only when the ORACLE, on seeds nothing was built on, needs each served mode in at least 10 percent of a class's resolved scenarios (lower 95 percent bound), with at least 40 resolved a class, fixtures and extracted places judged apart. A class that honestly has one answer passes by proof, not by exemption (section 5.4).
7. Cost, 15 cells: the road-off part about 500 CPU hours (380 to 720, unchanged from 15.7). The road-on part is 350, 800 or 1,500 CPU hours by budget (section 8). The middle one is recommended: the round is then about 1,300 CPU hours, about 3.5 days on 16 cores at idle priority, of which 500 can run now.

## 1. What is wrong today, in one table

| # | Fact | Source | Consequence for the tune | Closed by |
|---|---|---|---|---|
| 1 | The oracle's subject has no ranking in the picture, so he holds no claim. With the road on the "gap" is not a gap (env-ridge evoker went from +3.81 to -0.74) | MOVE-S2 F3, 17.6 item 5, VERIFIED `MovePass.cpp:39 Scores()` needs `nTop > 0`, only `IausPolicy::Serve` calls `MoveNoteRanked` (`Policy.h:552`) | no oracle number on the road may be read | S5-C |
| 2 | Free against a Stand on his own place is `StayMode`: `t.orderHoldFoes` 2, `t.orderNearU` 400, `t.orderStandMinU` 300. It sees neither foe kind nor terrain, has no hysteresis and is never compared with Free | VERIFIED `MovePass.cpp:840`, review F7 and uncertain spot 1 | marth's biggest choice is not tuned at all | S5-A |
| 3 | Lab caps default to strict + kite + disengage. Harbinger serves the implicit Stand only | review F1, VERIFIED Harbinger `MoveOrder.cpp:798` | a tune for a body the game lacks | S5-B (T1) |
| 4 | Forced arms ignore the caps. A dead order after Ended stays dead | review F9 | the forced-arm tool lies about Disengage | S5-B |
| 5 | Fear under an order does nothing in the lab. Strafing is at full run. Arrival is 2D. A new point costs 0.25 s where the game needs a release, a request and a carry | review F2, F3, kinder-list 4, 5, 7, 8 | the oracle learns "Stand answers fear" and kites sideways at 370 | S5-B (T3) |
| 6 | Acceptance was ten constant rules under IAUS actions. 15 percent resolved. Kite rests on 7 of 68. Extracted 4.2 percent resolved | review "Acceptance", 21.6 | the set is unproven | S5-D (T2) |
| 7 | 360 of 12,600 cells are ever fought. Level 25 and rank 3 fixed. One fixture a terrain. 6+ is always 6 | review F8 | one habit | S5-D |
| 8 | Loot search cannot produce a table (P2). A worse held-out loot table is kept under `--decisions auto` (P3). The joint verdict does not bind the shared values (P4). `epi:` and `lootrep:` keys hash no `<set>.loot.json` | review-sim-pretune | a tune that accepts worse, or replays stale loot | S5-E |
| 9 | Lab book is not the game's book for Wall, Rune and impact-hazard spells (P1) | review-sim-pretune | caster cells train on spells IAUS never gets | NOT S5. Named in section 12 as a blocker of the caster cells |
| 10 | No oracle fight had been timed on the strict road | review "Cost" | every estimate | section 7 (done here), repeated by S5-F on the real thing |

## 2. Packages, in order

One agent per build tree. A to D and F share the sim tree and are serial. E1 touches two Python files nothing else touches and runs beside A in its own worktree. Author by the ruling of 11-24: Opus codes the sim and the pure `native/iaus` files from this plan, a second Opus reviews. Sizes are ESTIMATED changed lines with rows and docs.

| # | Package | Files | Size | Review tier | Needs |
|---|---|---|---|---|---|
| S5-A | Hold as a scored kind, `StayMode` removed, order hysteresis, `OrderOf` masks by caps, golden v6, the six surviving mutants get rows (F4, F6, F7) | `native/iaus/Core.h`, `Spots.h/.cpp`, `MoveOrder.h`, `MovePass.cpp`, `Catalogue.cpp`, `TuningFile.cpp`, `ClassCurves.h`, `tools/iaus_sim/Tune.h`, `tools/iaus_harness/move_rows.cpp`, golden | 650 | full (pure code the DLL will run, a new kind) | main |
| S5-B | Executor honesty: default caps `strict,kite`, forced arms obey caps and revive, the new residue rows, every executor number into the one table (F5), a strict spot-test row per new row | `tools/iaus_sim/Move.h`, `StrictWorld.h`, `MoveWorld.h`, `ModeSet.h`, `SpotTest.h`, `sim.cpp` | 500 | full | A |
| S5-C | The oracle with a claim: the subject's ranking, the order race, the dump and replay, the mode-lead lines, the greedy-assignment regret, the ext moves' retirement report | `tools/iaus_sim/Oracle.h`, `Policy.h`, `MoveWorld.h`, `sim.cpp`, `oracle_replay.py` | 750 | full | A, B |
| S5-D | Scenario generator v2 and the acceptance (`--mode-accept`), the difficulty calibration, the fixtures, the extracted strata | `tools/iaus_sim/ModeSet.h` (past 264 lines it splits: `ModeGen.h` new, by name in the brief), `envs/make_fixtures.py`, `sim.cpp`, `tools/iaus_sim/mode_accept.py` (new) | 700 | focused | C |
| S5-E1 | Driver holes: P2, P3, P4, the loot file in the `epi:` and `lootrep:` keys, a missing `LOOTSEED` line is an error, an empty pairing fails | `tune_driver.py`, `test_tune_driver.py` | 300 | focused, ONE pass | main. Starts now |
| S5-E2 | `stage_spots`, the road flag per stage, `--bin-spots`, the residue file in road-on keys, the not-searched list, provisional values kept out of `ship` | `tune_driver.py`, `test_tune_driver.py`, `live/run_round.py`, `calib_point.py` (new, reads the `[move]` lines of section 10) | 550 | focused | C, D, E1 |
| S5-F | Cost cuts with their replay proof, the 12-fight end to end with and without the order race, the pilot, the sensitivity arms, the measured budget written into this file | `Oracle.h`, `MoveWorld.h`, `sim.cpp`, docs | 300 plus runs | focused | C, D |

Total about 3,750 lines with tests, 3,400 without the fixtures. Then the freeze, the one-time measurements of 15.7 (with the pathing bounds measured road off AND road on), and the round.

Extension points (principle 12), to be written into each file's "TO ADD OR CHANGE" block: a mode the oracle may order is one row of `kOrderRows` plus its cap. A consideration of the hold is one `Cons` row and one line in `FillStatic`. A residue is one row of `kResidue`. A scenario axis is one row of `kModeAxes` (new) and its case in the builder. A searched knob is one entry of `SPOT_KNOBS`. A knob that may not be searched is one entry of `SPOT_NOT_SEARCHED` with its reason.

## 3. S5-A: Hold is a place like any other

### 3.1 Mechanism

- `SpotKind::Hold` is APPENDED before `kCount` (after `ReachUnreach`). Appending keeps every stored kind number. VERIFY before coding: no co-save record and no shipped file holds a `SpotKind` index (the kinds are bits in the golden file and in memory only). The golden file moves to format 6 once.
- The generator offers exactly one Hold place per member per ranked action: the point he stands on, or, while he is within twice the leash of a standing order's point, THAT point (today's "a hold does not creep with every push", kept). The gate is what remains of `StayMode` when the three rules are taken out: the cap `strict`, an action with a target that is not himself, not a move family, not a move-only heal, and the action works from here (inside `act.reach`).
- `kOrderRows` gains `{ SpotKind::Hold, MoveMode::Stand, false }` directly above `Stay`. `Stay` is always Free. `StayMode` is deleted with its declaration. `OrderOf` loses its special case.
- Hold is scored by the pass with the same product of considerations as every other place, so it competes with Stay (Free), with a firing place, with a kite place, in the one assignment table. No rule decides before the scores do.

### 3.2 The hold's considerations (rows of `Cons`, curves shared, weights per class as `spot.hold.<cons>`)

Every input is something the game's filler can read from the snapshot it already builds. None reads a foe's name or a list's record.

| Consideration | Input | Why it is there |
|---|---|---|
| `hold_foes_on` | foes whose target he is AND whose own reach covers him now (review F7: not "within 400") | a blade surrounded holds, a blade with one foe may fight free |
| `hold_contact_sec` | distance of the nearest foe that targets him, less that foe's reach, over that foe's speed | time until he is hit. A shooter stands while it is long |
| `hold_foe_speed` | fastest foe that targets him over his own run | a slow heavy can be walked from, a pack cannot |
| `hold_foe_ranged` | share of the foes that target him that fight at range (bow, spell) | standing still in a bolt mage's line is not standing still in a troll's |
| `hold_sight` | the action's sight from the hold point (the fact the pass already has) | a hold without sight is a wait |
| `hold_cover` | share of the foes that target him who do not see the point (the cover fact, existing) | |
| `hold_narrow` | width of the narrowest passage between him and the nearest foe (the portal fact, existing) | a doorway is held, open ground is not |
| `hold_ally_front` | an ally with a foe on him stands between him and the foes | the tank is holding, so the shooter may |
| `hold_player` | the player's distance over the leash, and whether he is leaving | a hold the leash will break is worth little |
| `hold_kept` | he holds this point already (1) or not (0), read through `t.orderHoldMomentum` (default 1.25, PLACEHOLDER, searched) | the hysteresis F7 asks for. In the game every flip is a claim filed and released |

Unknown inputs are `In::Off()`, never a favourable value.

### 3.3 What goes

`Tuning::orderHoldStay`, `orderHoldFoes`, `orderNearU`, `orderStandMinU`, their keys in `Catalogue.cpp:551`, `TuningFile.cpp` and `Tune.h:54`. They were added on 11-24 in `1666d9e8`, have no caller in the DLL and are in no release. VERIFY that with `git tag --contains 1666d9e8` before deleting a key. If any tag holds them, keep the keys as read-and-ignored with one log line.

### 3.4 With it, from the backlog

- F6: `OrderOf` masks `a.kinds` with the kinds the caps allow before `OrderRowOf`.
- F7: the doc line of 21.1 and the reach test above.
- F4: one row each for hold from a Kite order, hold keeps the pin radius, the strict-leashed case (ch.23 wins), caps without disengage, the block back-off timing. Each row must fail under the mutation the reviewer used. `t.orderNearU` has no row because it no longer exists.
- F10: the mode-number row becomes a `static_assert` only when MFO's `APMF_API.h` reaches v48. Not in this package. Say so in the row's comment.

### 3.5 Acceptance of S5-A

Harness rows: Hold is offered only when its gate holds. With every `spot.hold.*` weight 0 the pass's assignments equal the assignments with the kind masked off (the pin). With weights chosen to mimic the old rules on the three old scenes the order equals the old order (proves nothing was lost). Order independence under every permutation with Hold in the table. The hysteresis: on a scene that sits on the old flip edge, orders change at most once in 20 windows. Road off: `--world v2`, `v3`, `v4` baselines byte identical at 1 and 8 threads. gcc and MSVC replay of the golden file, 0 failed. Local MSVC build green on the final SHA.

## 4. S5-B and S5-C: the executor's honesty, and the oracle with a claim

### 4.1 S5-B, rows added to `Residue` (all coded before the freeze, all PLACEHOLDER until section 10 fills them)

| Row | Placeholder | Low, high (section 9) | What it does in the executor | Field source |
|---|---|---|---|---|
| `kiteSpeed` (exists) | 150 | 60, 193 | speed while backing from the foe he faces | H-M2 session, and the walk-back of the stand session as a first read |
| `strafeSpeed` (new) | 150 | 150, 370 | speed across a foe (the dot test becomes: toward 1.0 run, across this row, away `kiteSpeed`) | H-M2 session |
| `lungeOut`, `lungeOutPower`, `lungeShare` (exist) | 60, 110, 0.5 | 0 and 0, 120 and 200 with share 1 | attack displacement past the leash | stand session |
| `walkBackSpeed` (new, was `kiteSpeed` by assumption) | 150 | 60, 193 | the return after a displacement | stand session |
| `standDrift` (exists) | 0 | 0, 15 u | movement of his own while standing | stand session |
| `displaceU`, `displaceDownU` (exists, new) | 0, 0 | 0, 120 and 300 | stagger and knockdown carry | stand session |
| `startDelay` (exists) | 0 | 0, 1.0 s | first carry of an order | stand session |
| `refileStall` (new, beside `repointStall` 0.25) | 0.25 | 0.25, 1.1 s | a NEW point is a release, a request and an area carry until P-C | P-C session |
| `arriveError` (exists) | 1 x arriveR | 1 x arriveR, leashR | where he stops. High models accepted short stops | goto rows (not in the stand session) |
| `arriveZ` (new) | off | off, 24 u | on: a point whose z is more than this off his never arrives and blocks | goto rows on a slope |
| `blockedShare` (exists) | 0.05 | 0, 0.22 | orders that end Blocked | every session |
| `blockTold` (new) | 1 | 0, 1 | 0: Blocked never becomes Unreachable and nobody is told (today's game until P-B) | P-B |
| `fearUncarried` (new) | 1.5 s | 0, 3 s | under a fear effect the order is not carried for this long at a time and he is the engine's, flee and hide denied | `strict.flee` |
| `fearActs` (new) | 1 | 0, 1 | whether he attacks while it lasts | `strict.flee` |
| `shotStop` (new) | 0 | 0, 1.0 s | travel time lost per shot or cast begun while travelling | H-M2 session |
| `bridgeLag` (new) | 0 | 0, 0.27 s | time from a pass window to the order being live | any session (PUBLISHED line against MFO's own line) |
| `attackInPlaceRate` (exists, unused) | 0.6 | not armed | stays unused while lunges are the default. Delete the "not used" note and say why in the row |

Also in S5-B: default `--move-caps strict,kite`. The forced arms read `g_moveCaps` and an arm whose mode is not served is not played. A Kite or Disengage that ended gets a new `gen` when its rule fires again (F9). Free is the client contract "end the claim" (review uncertain spot 2), so a Free after an order pays `refileStall` when the next order comes, which is what the game will pay. The numbers the review listed outside the table (0.5 s and 1.0 s block timing, minimum stop 6, sneak 0.55, drift 0.3 and 3 s, lunge standoff 60) move into `kResidue` or `MoveTuning` with a name (F5).

Acceptance of S5-B: each new row has a spot-test arm that fails when the row is zeroed (the "off arm" pattern of 21.4). Road off, point and pinned baselines byte identical with main. Strict baseline at 1 and 8 threads identical. The world tag carries every non-default row.

### 4.2 S5-C, the oracle's subject holds a claim

1. **Ranking.** `OraclePolicy::Serve` already runs IAUS's own Decide for the subject (it is the "IAUS's own pick" of every divergence). After the oracle has chosen, it calls `MoveNoteRanked` with that ranking, the oracle's chosen action moved to the front as the held action. `RolloutPolicy` must do the same for every body it serves. VERIFY first whether it does today: the S2 note says rollouts "run the pass for the others", which suggests it does for them and not for the subject.
2. **When he is asked for an order.** At the end of a pass window (`MoveDecisionSec`, floor 0.5 s) in which the pass itself would decide for him: not committed, or marked. Never between windows (8.7: a lab that re-decides oftener tunes a creature the game does not have). The hook is `MovePass::Phase3`'s caller in `MoveWorld.h`: for the subject the assignment is the oracle's.
3. **The options.** An order option is (mode, place), paired with one of his three ranked actions:
   - Free.
   - Hold on his own place.
   - For every place of the pool that his class's `AllowedKinds` and the caps allow: the mode of `kOrderRows` for it, AND every other served mode that can go there (Stand after travel, Kite, GoTo). So "walk to that corner facing the foe" and "run to that corner" are separate options. Under `strict,kite` that is three modes.
   - Places of an allowed kind that the generator did NOT pair with any of his three actions are offered too. A use of one is a (c) divergence, counted like a missing family, with the kind in its token.
   - Refusals go through `ClassRefuses`, which reads `kSpotKinds`, `AllowedKinds` and the caps and returns a token per reason (`order:kite:cap off`, `order:cover:class has no cover kind`). The ONE reader, as today.
4. **The race.** An order option is applied, held as a commit (until he arrives, or an event of 8.7 marks him, exactly the pass's commit), then the fight rolls to its end with the base policy for everybody, the subject included: IAUS's actions and the pass's own orders. Streams are shared by all options. The race, its p and the ceiling of 128 are unchanged. Between windows the oracle races actions as today, under the order that stands.
5. **Joint, not factorized.** At a window where he is asked, the table raced is orders x his three ranked actions (the same three the pass pairs). It is not "best order for the action he happens to hold". The cuts of section 7 are what keep this affordable.
6. **What is written.** Per order decision one `ORCMODE` line: for each served mode the mean of its best option over the streams it stood, the paired lead of the winner over each other mode's best with its standard error, IAUS's own order at that window, and whether the oracle's place was in the generator's pairing. The dump (`--oracle-dump`) carries order options like action options, so `oracle_replay.py --check` reproduces a dump with orders in it.
7. **Regret of the assignment.** With every follower planned (`--oracle-subject-only` off) on a small fixed set, the greedy assignment's U against the oracle's, per party size. A report line, as 8.7 promised. It decides nothing in this round.
8. **The ext moves.** `ext:step_back`, `sprint_back`, `to_player`, `to_healer`, `get_sight`, `clear_line` stay offered. S5-F prints, on the same fights, how often each wins with order options present. One that never wins within its noise is listed "retired by" its order form in the docs and stays in the enum for old logs. None is deleted in this round.

Acceptance of S5-C: the subject's `claim%` in an oracle fight is above zero and of the size of an IAUS subject's in the same fights. env-ridge evoker's gap with the road on is no longer negative beyond its noise (the F3 symptom). `oracle_replay.py --check` on a dump with orders: every pick and rollout count reproduced. With the caps at `strict` alone no Kite option is ever raced (counted in `ORCREFUSED`). Threads 1 and 8 give the same picks. Road off: every oracle number byte identical with main.

## 5. S5-D: the scenario set and its acceptance

### 5.1 The generator (`ModeGen.h`, one table `kModeAxes`, every draw from the scenario seed)

A scenario is drawn, not enumerated. Seed s and index k give one scenario by a Latin hypercube over the axes, so every seed fights NEW cells and 1,000 scenarios cover the pairwise space far better than the fixed 360.

| Axis | Draw | Note |
|---|---|---|
| Foe speed over his run | one third uniform 0.20 to 0.40, one third 0.40 to 1.00, one third 1.00 to 1.50 | 0.40 is `kiteSpeed` over 370. The first third is what a kite can hold off: slowed, frosted, giants, draugr walkers |
| Foe count | 1 (25 percent), 2 to 3 (30), 4 to 6 (25), 7 to 10 (20) | no fixed 6 |
| Foe kind | the 12 kinds, pure in 60 percent of scenarios | |
| Mixed groups | 40 percent, from rows of `kModeGroups`: shield line + archers + a mage, boss + adds, pack + an archer behind, summoner + melee, melee only (no foe has a bow or a spell), stream mage + chargers | a row is a list of (kind, share) |
| His level | 10, 25, 45, equal thirds | he and his party are built at it by the existing class build |
| Difficulty | a scalar on the foes' health and damage, from the calibration of 5.2 | tuned so the base win rate is 30 to 70 percent |
| Terrain class | the 10 classes, each with at least 3 fixtures (30 or more, about 20 new from `make_fixtures.py` with width, length and height as parameters), mirrored and rotated at random | one fixture a terrain was F8 |
| Start distance | uniform 300 to 2,500 u, cut to the place | |
| Open ground | of the scenarios with one foe, a quarter are forced onto open ground of 1,500 u or more | a kite needs room |
| Second group | 15 percent: another group arrives after 8 to 25 s, or when the first is half dead | chained fights |
| Party | alone, a tank holding, a healer who needs a line, two shooters, a second DECIDING follower (IAUS with claims), party size 1 to 4 | |
| Healer | where a healer is present, in half of them a corner stands between him and the front | the subject is the healer when the class cell is one |
| Player | near, at the leash, leaving, hurt (under 40 percent), and his own style: in melee, at range, passive | |
| Light | not an axis here. S4's rows own it | |

**Extracted places** are a second set, never mixed into the first. The library's places are measured as today (narrowest passage of the walked way, height between the groups, doors) and stratified: narrowest under 150, 150 to 400, over 400. Height under 64, 64 to 200, over 200. Doors 0, 1 to 5, more. 27 strata. The draw takes up to 3 places a stratum from every extracted set on the machine, 60 or more places against today's 14, by measured geometry only. No place is chosen by name and no list's name appears in code (the standing rule). Empty strata are printed.

**What is reported is what was FOUGHT.** Every axis is read back from the bodies and the mesh of the fights that were played (F8), as histograms over the fought sample, and the pairwise table of axes lists its holes. The 99.6 percent of `--mode-cover` stays what it is, a build check, and is never quoted as variety.

### 5.2 Difficulty calibration (a pilot, once per generator version)

For each (class, foe-kind group, count band, level) the difficulty scalar is found by bisection on 16 fights so that the win rate lands near 50 percent. The arm is IAUS with the road OFF: it depends on no value this round tunes. The table is written to the round's directory with the generator's version and is in the cache key of every mode-set fight. On the acceptance seeds at least 80 percent of scenarios must measure 30 to 70 percent base win, and a class whose share is lower is not judged. A fight that is always won or always lost resolves nothing, which is most of why only 15 percent resolved before. Extracted places get their own calibration by the same pilot.

### 5.3 The acceptance (`--mode-accept`, `mode_accept.py`)

**Seeds.** Three families, written into the round's manifest. DESIGN seeds: anything used while building and calibrating the generator. TRAIN seeds: what `stage_spots` searches on. HELD-OUT seeds: drawn once, after the generator and its calibration are frozen, used for this acceptance and for the stage's held-out checks, never for a search. If the generator changes after a held-out run, a NEW held-out family is drawn. The old one is never reused, and the driver refuses a run whose generator version does not match the family's.

**The measure is the oracle's, per scenario.** For class c and scenario s the subject-only oracle plays F fights (other lucks, same scenario). From the `ORCMODE` lines:

- need(m, fight) = the sum, over the order decisions the mode m won, of its paired lead over the best option of any OTHER mode. That is what the oracle would have lost at those decisions without m.
- A decision whose lead over every other mode is inside the equivalence margin is a TIE and belongs to no mode. Twins (the same utility on every stream) are ties by definition.
- Scenario s NEEDS m when the mean of need(m) over its fights is above the scenario margin and its lower 95 percent bound over fights is above zero. A scenario may need two modes. A scenario that needs none is unresolved.
- F starts at 4 and doubles to 16 while a scenario is neither resolved nor clearly tied (upper bound of every mode's need under the margin).

**Margins.** Decision margin 0.10 U, scenario margin 0.25 U a fight. The review asked for 0.01 U. That cannot be measured: the pair rule retires options inside 0.10 U as indifferent by construction, and at 128 streams a difference of 0.01 U is far inside the standard error. 0.10 is the margin the oracle itself runs on. 0.25 U a fight is a quarter of the divergence floor of 15.7. Both are named constants of `mode_accept.py`.

**The race keeps what the measure reads.** In acceptance runs the best standing option of each served mode is never retired by the pair rule (it joins the list "the best, IAUS's own pick, an alternate table's pick, the action he is on"). At most three more standing options.

**Check of the measure.** On a tenth of the scenarios a true leave-one-out arm is played: the oracle with mode m refused. The paired U loss must agree in sign with need(m) in at least 80 percent of the scenarios that need m. If it does not, the decision-level measure is not a proxy for "the oracle needs this mode" and the acceptance is redone on leave-one-out arms at four times the cost. That outcome is reported, not papered over.

**The lines, per class, over modes the class can use under the caps** (the oracle is class-bound, so a blade has no Kite line):

| Line | Rule |
|---|---|
| Resolved | at least 40 resolved scenarios in the generated set, and at least 40 in the extracted set, each judged apart |
| Each mode is taught | for every mode of the class: the share of resolved scenarios that need it has a lower 95 percent bound (Wilson) of at least 10 percent. With 40 resolved that is 8 scenarios, with 80 it is 14, with 120 it is 19 |
| No single habit | the share of resolved scenarios that need ONLY mode m has a lower 95 percent bound under 60 percent, for every m |
| Base win | 80 percent of the scenarios inside 30 to 70 percent |
| Over all classes | marth's own line as the plan wrote it: every served mode needed in at least 10 percent for at least one class, none over 60 percent over all. Kept, and now a consequence of the per-class lines |

Disengage has no line in this round (not served). GoTo is reported in its own column and has no line: it is a way to a place, not one of marth's four answers.

### 5.4 A class whose best answer genuinely is one mode

An archer needed Stand in 85 percent of what resolved. That may be the truth about archers and not a fault of the set. Such a class passes as ONE-MODE(m) only when all four hold:

1. It was given its chance. The generator has adversarial rows per mode (`kModeAgainst`): for a shooter's Stand, a fast pack on open ground, a charger from 600 u, a foe in contact at the start, a dead end. At least 30 percent of that class's held-out scenarios come from the rows built against its dominant mode.
2. On those rows too, no other mode reaches the 10 percent line.
3. The true leave-one-out arm confirms it on the class's whole held-out set: the oracle WITHOUT the minority modes loses less than the scenario margin, upper 95 percent bound. This is run in full for such a class, not on a tenth.
4. It still has 40 resolved scenarios, and they include decisions between m and Free and between places, so the class has something to tune.

Then the class's weights for the kinds of the modes it does not need go to the not-searched list with the reason "the oracle does not need it: loss under the margin", they keep their defaults, and the class does not count toward the over-all line. The list is printed in the round's summary for marth. A class that fails 2 or 3 is not one-mode: the set is short for it and the generator gets rows, on a new held-out family.

What "pass" never means: an exemption because the numbers came out one-sided.

### 5.5 Fights

| | Scenarios a class | Fights each | Oracle fights, 15 cells |
|---|---|---|---|
| Pilot (2 classes, to measure the resolved share r) | 40 | 4 to 8 | about 500 |
| Generated set, held-out | 40 / r, capped at 160. At r 0.4: 100 | 4, growing to 16 where open. Mean about 6 | about 9,000 |
| Extracted set, held-out | the same | the same | about 9,000 |
| Leave-one-out check | a tenth | as above, three arms | about 5,000 |

These same held-out oracle fights are the `spots` stage's held-out baseline (the oracle arm is paid once and cached, a candidate table is the IAUS arm of the same fights). They are not spent twice. Section 8 prices them.

## 6. S5-E: the driver

### 6.1 E1, starts now

- **P2.** `loot_judge` runs `loot_gate_check` on the candidate itself (train places). A step that worsens `leftRR`, `roadLeft` or `dibsNear` beyond its bound is refused there, so the search can keep something. The final check on the kept table stays.
- **P3.** A held-out VALUE loss of the loot table (paired per seed, d below -1.5 se) REJECTS under `--decisions auto`. It is no longer a note. Without `--loot-places` the stage refuses to keep a table and says so.
- **P4.** `ship_state()` compares `joint.sets` with `final_sets()` as it compares the curves. `rerun_after("worth")` and `("spells")` name `joint`. A failed joint empties the shared sets in `ship`. The stage order puts `worth` before `joint`.
- **Loot keys.** `ident()` for `epi:` and `lootrep:` hashes `<set>.loot.json`. The combat keys already hash the binary, `sim.json`, the mods, the worth table and every `.mfoenv`.
- **Small ones from the same review.** A row with no `LOOTSEED` line is an error. `held_out_check` with an empty pairing fails.
- Tests in `test_tune_driver.py`: each of the above planted and caught, the pattern of `ChoiceBatch`.

### 6.2 E2, after C and D

- **Road per stage.** One table `STAGE_ROAD`: every stage `off`, `spots` and the two checks below `strict`. The road flags, the caps and the residue file go on the command line of road-on commands only, so a road-off key never moves when a residue does.
- **`--bin-spots`** (optional, default the round's binary). The road-on commands run on it and key on its hash. If a Deck session ever shows the executor's CODE is wrong, the fix is a second binary and only road-on results die. The driver refuses the pair unless the two binaries give byte-identical road-off baselines on a fixed command.
- **The residue file.** `calib_point.py` writes `<round>/residue.json`: per row the value, `source: placeholder | field`, the evidence folder and the sample size. Its hash is in every road-on key.
- **`stage_spots`**, after `spells` and the first `joint`:
  - World: the generator's TRAIN seeds, the 14 environment scenarios, and natural v4 fights from places where IAUS today has nonzero `holdUnreach`, `reblock` or occluded time (cut 7 of the movement plan). Caps `strict,kite`.
  - Knobs (`SPOT_KNOBS`): the shared `t.spot*`, `t.light*` that spots read, the hold curves and `t.orderHoldMomentum`, `t.spotActionMomentum`, **`t.moveHealYield`** (HEAL-MOVE: it must be searched), then per class `spot.<kind>` for every kind its `AllowedKinds` and the caps allow, and `spot.hold.<cons>`.
  - Not searched (`SPOT_NOT_SEARCHED`, each with its reason, a test holds the list): `spot.retreat_exit` and every Disengage weight (cap off), `t.orderLeash`, `t.orderLeashPin`, `t.orderArrive` (they must equal radii a dev gambit row passed at, 64 and 32 today), `t.moveDecisionSec` (marth's floor), kinds of a ONE-MODE class, sight-driven knobs as in `follow`.
  - Score and rule: the `follow` rule. The gap to the class's own oracle on its gap fights, searched on IAUS fights, accepted when it closes the gap by more than 2 se with no world worse, holds on fresh train seeds, is not worse on held-out seeds and extracted places, passes J as a fence, the class card and the pathing gate, and the gap did not rise when the oracle ran again. Two fences are new: orders changed per minute must not rise beyond its bound (each is a claim filed and released in the game), and the strict spot tests of 21.4 still pass on the candidate table.
  - Shared scalars first, then class rows, T1 rows before the rest, as `follow`.
- **Provisional values.** A value accepted while any row that section 9 marks SENSITIVE is still `placeholder` is recorded as provisional. `ship_state()` leaves it out and `gen_class_files` lists it under "not written: waits for field residue <row>".
- **Checks after the stage.** `joint` a second time with the road on, as a fence only: no cell's J under its road-off J beyond noise, on held-out seeds. The final `gap` prints both roads.
- Tests: an improvement and a regression planted for the stage, a value that fails only on held-out scenario seeds, a not-searched knob refused, a provisional value kept out of `ship`, a residue change that re-runs road-on fights and replays road-off ones.

## 7. Cost: what was measured, and the cuts in the order to take them

### 7.1 Twenty oracle fights an arm, timed for this plan (VERIFIED, the first measurement on the strict road)

Binary `_tuning/sim-strict/sim` (main's sim). The tune's world: `--world v4 --env-set Tuxborn --env-hold train --books field`, the list's sim file, `--potion-worth legacy`, the tune's cuts (`--oracle-prune target --oracle-pair 0.10 --oracle-nmin 4`), subject only, 8 threads, user CPU. 12 fights of archer, knight, thief, warrior and 8 of healer, evoker per arm. **The subject holds no claim in this binary**, so the road arms price the pass inside the rollouts for everybody else and nothing of the order race. They are a floor.

TIMING_TABLE

### 7.2 The cuts, biggest first

| # | Cut | What it saves | Exact? | Proof owed |
|---|---|---|---|---|
| 1 | Road off in every stage but `spots` and its two checks | the whole road factor of 7.1 on about 500 CPU hours of stages | exact for what ships with the switch off | road-off baselines byte identical (standing) |
| 2 | 15 cells, 3 worlds for the road-on part | linear | exact | none |
| 3 | Orders raced only at windows where the pass would decide for him (uncommitted or marked) | most windows. With a commit per leg an order decision comes every few seconds, not twice a second | exact: it is the game's cadence | counted: order decisions a fight, printed |
| 4 | Order options only from `AllowedKinds` and the caps, places clustered (one option per triangle cluster and per `arriveR`) before the race, the pair rule and twins after 4 streams | ESTIMATE 3 to 5 times fewer order rollouts | the cluster and pair rules change picks a little | replay table as 15.7 on at least 74 order decisions: same pick at least 95 percent, added loss at most 0.005 U a decision |
| 5 | Static spots once per place and stamp, shared by every fight and rollout there. The (spot, actor) sight fact reused while neither end left its triangle | part of the 12 to 15 percent sight and path share, more on the road | exact (the game's own reuse) | byte-identical road-on baseline before and after |
| 6 | The rollout's pass is frozen: no Phase 1 and no rays inside a rollout, the orders that stood are kept, Phase 3 refreshes on the frozen pool every 2 s | ESTIMATE up to the pass's share of a rollout, which 7.1 puts at ROLLPASS | NOT exact: the base policy places bodies worse late in a rollout | the same replay table, plus the oracle's U on 12 fights not lower beyond noise. If it fails the cut is dropped, it is not tuned until it passes |
| 7 | Search on IAUS fights, the oracle only to accept and verify (the `follow` shape) | a candidate costs milliseconds | exact | planted tests |
| 8 | Sequential growth: 4 fights a scenario, more only while unresolved. A class stops when its lines are decided | about half of the acceptance | exact | none |
| 9 | Sensitivity arms on IAUS fights only | section 9 costs tens of CPU hours, not thousands | it measures IAUS's flips, not the oracle's | the rows it marks sensitive get one oracle arm in the full budget |
| 10 | Cached search baselines reused (`table_slice`) | as built | exact | none |

Not offered: a lower ceiling (marth's call, 15.7 shows the price), a smaller pool for the oracle than for IAUS (it hides the (c) divergences), fewer decision points than the pass has.

S5-F measures 3, 4 and 6 on the real oracle, and repeats 7.1 with the subject's claim and the order race in. That number replaces section 8's road-on estimates before the round is started.

## 8. The round, re-estimated (15 cells)

Road-off part: `prep`, `rejudge`, `classes`, `edge`, `follow`, `spells`, `worth`, `joint`, `gap`, `globals`, `consts`, `floor`, `rating`, `recalib`, `logistics`, `loot`. 15.7 put 38 cells on four worlds at about 1,250 CPU hours and 15 cells at about 40 percent of that: **about 500 CPU hours, between 380 and 720.** Nothing in S5 changes it. The pretune smoke's `follow` (7,778 s for 2 cells on 2 worlds) is inside that range. The one-time measurements before it: 80 to 150 CPU hours more, as 15.7 says, plus the pathing bounds a second time with the road on.

Road-on part, three budgets. ROADON_BASIS

| | Lean | Standard (recommended) | Full |
|---|---|---|---|
| Acceptance classes | 8 (the `kModeClasses`), the other 7 cells take their group's verdict | all 15 | all 15, grown to 80 resolved |
| Sets judged apart | generated per class, extracted per class GROUP (blade, shooter, caster, healer, sneak) | generated and extracted per class | the same |
| Leave-one-out check | a twentieth | a tenth, in full for one-mode classes | a fifth |
| `spots` search | shared scalars and the hold rows per class. Kind weights per class group | shared, then every class row | the same, two passes |
| Oracle verify | per accepted SET | per value for shared scalars and T1 rows, per set for the rest | per value |
| Worlds on the road | built-in and one list | built-in and two lists | all four |
| Sensitivity arms | IAUS only | IAUS only, plus one oracle arm for each row marked sensitive | oracle arms for every row |
| Extras | none | the assignment regret report | Disengage measured under `strict,kite,disengage` as a PRICE of H-M3 (no weight kept), exact assignment tried |
| Oracle fights on the road, about | 12,000 | 30,000 | 60,000 |
| CPU hours on the road, ESTIMATE | **350** (250 to 500) | **800** (550 to 1,200) | **1,500** (1,000 to 2,300) |
| Round with the road-off part | about 850 | about 1,300 | about 2,000 |
| Wall, 16 cores idle priority, machine quiet | 2.2 days | 3.4 days | 5.2 days |
| What it buys | the tune can run and Free against Hold is tuned for every class. Kind weights are shared inside a group, and a group's verdict may hide one class's one-mode habit | every class is judged and tuned on its own, on fixtures and real places apart. This is the smallest budget under which "T1 and T2 movement need not be redone" is a claim with evidence per class | the same claim with tighter bounds, and H-M3's worth known before it is built. It does not make the tune more right if the residues are wrong |

The review's 3,000 to 3,800 CPU hours was the road in every stage. Cut 1 is what brings the standard round to about 1,300.

A residue that lands outside its placeholder after the stage ran costs the road-on part again and nothing else (section 6.2), which is the reason to wait for the sensitive rows and not for all of them.

## 9. The residue sensitivity arms: which field numbers the `spots` stage must wait for

**Question.** For each placeholder row, does its plausible range change WHICH mode wins, or how much a place is worth, by more than the acceptance's own margins? If not, the stage may run on the placeholder.

**Arms.** The low and high of section 4.1's table, one row at a time, all else default (2 arms a row, 15 armed rows, 30 arms), plus two corners (every row at its kinder end, every row at its harsher end), plus the default. 33 arms.

**Instrument.** IAUS fights only. The forced-arm tournament of 21.6 with arms that obey the caps (S5-B), on the generator's DESIGN seeds, 180 scenarios x 8 classes x up to 32 fights. A fight is 16.6 ms (21.6), so an arm is about 1.4 CPU hours and the set about 45. It reads modes, not weights, which is the question.

**Per row, three numbers against the default arm, paired by scenario and luck:**

1. flip share: of the scenarios resolved in both arms, the share whose winning mode changed.
2. lead shift: the change of the paired lead between the two best modes (Hold against Free, Kite against Hold), in U a fight.
3. resolved shift: scenarios that resolve in one arm and not in the other.

**Verdict.** A row is INSENSITIVE when the flip share's upper 95 percent bound is under 10 percent AND the lead shift is under the scenario margin (0.25 U) for every class. Else it is SENSITIVE, and the `spots` stage waits for that row's field value. The corners guard against rows that only matter together: if a corner flips more than the sum of its rows' flips by more than its noise, the largest contributors are marked sensitive as a group.

**Expected, to be shown wrong or right by the run:** `kiteSpeed`, `strafeSpeed`, `lungeShare` with `lungeOut`, `walkBackSpeed`, `blockedShare` and `fearUncarried` are sensitive. `bridgeLag`, `standDrift`, `startDelay` are not. `arriveZ` is sensitive only on stairs and slopes, which is a third of the terrain. This is a guess and is labelled one.

**What the verdict decides.**

- Every sensitive row is one the STAND session can measure: the stage may start after that session, caps `strict`, and Kite follows as a delta stage. Section 13, decision 1.
- A sensitive row needs H-M2 (`kiteSpeed` under fire, `strafeSpeed`, `shotStop`): the stage with caps `strict,kite` waits for the H-M2 session.
- A sensitive row needs P-B or P-C (`blockTold`, `refileStall`): the stage runs with that row at its HARSH end (today's game) and the value is provisional until the package lands.

In the standard budget each sensitive row also gets one oracle arm on 2 classes, to see that the oracle's own choice moves the way IAUS's did.

## 10. The coming Deck session (strict stand tests): what S5 needs from it, by log line

Yes, it feeds S5, and it should be read for S5 before anything else is concluded from it. It is the first field source for six rows. No new probe and no new code is asked for the session. Two cheap asks are at the end.

Harbinger's lines (VERIFIED in `native/core/MoveOrder.cpp` on its main), all tagged `[move]`:

| Line | Fields S5 reads | Residue row |
|---|---|---|
| `order #N SUMMARY (<why>): <mode> for <ms>; travelling / standing / returning / yielded / not carried <ms>; picks stand / travel / engine; max dist while standing <u>, last arrival at <u>; displaced <n>, walked back <n> (mean <ms>); Flee denied, Hide denied, essential down; short stops accepted <n> (last at <u>), waits outside the leash <n>, flee-only picks <n>.` | everything, per order. This is the line `calib_point.py` is written against | `standDrift` (max dist while standing with 0 displaced), `lungeOut` and `displaceU` (max dist with displaced above 0, split by the row's test), `walkBackSpeed` (excursion distance over mean walk-back ms), `startDelay` (not carried ms at the start), `arriveError` (last arrival, short stops), `fearUncarried` (flee-only picks x 0.25 s, with Flee denied) |
| `0x<id> DISPLACED (#n): <u> from the point, past the leash <u>; walking back.` | one per excursion, with its distance | `lungeShare` (excursions over attacks begun, the attack count from MFO's row line), the distribution of `lungeOut` |
| `0x<id> <mode> <phase>: dist <u> of x,y,z, leash, arrive; picks stand / travel / engine; Flee denied, Hide denied, displaced, short stops / waits, flee-only picks, blocks 0 (P-B).` | the periodic line while an order lives | time inside the leash, pick counts against time (`selectorLag`) |
| `0x<id> <phase> -> <phase> (dist <u>, engine passed 0x..)` and `0x<id> <event>: <u> from the point, <ms> since the order went live` | arrivals and returns with their time | `startDelay`, the walk-back time per excursion |
| `order #N PUBLISHED: <mode> at x,y,z, leash, arrive (claim h, slot)` | the time an order went live | `bridgeLag`, against MFO's own line for the same order |
| `order #N CLEARED (...): the engine never asked for his movement under it.` and `... the selector was never bracketed for him` and `... the engine stopped asking for his movement` | orders that were never carried | `blockedShare`'s cousin: the share of orders the game did not carry at all. Today the lab has no row for it. If it is above zero S5-B gets `notCarriedShare` |
| `heartbeat: ... answers stand / travel / engine; Flee denied, Hide denied, flee-only picks` | session totals | cross-check of the sums |
| `SetMoveOrder: this build serves the implicit Stand order only (mode <m> answered ModeNotServed ...)` | must appear for nothing S5 leans on | the caps: confirms `strict` alone is served |

MFO's side: the dev gambit rows' own result lines for `strict.stand_bow`, `strict.stand_melee`, `strict.stand_melee_far`, `strict.flee`, `strict.keep`, `strict.release`, `strict.save`, `strict.refused` (hm1-seat-brief 13.2). From them: shots per minute and attacks per minute held against free (the lab's attack rate while standing), and for `strict.flee` whether he ATTACKS while flee is denied (`fearActs`). I did not find `TestsStrict.cpp` on MFO main, so the exact text of these lines is not verified here. The coordinator should confirm the deployed branch prints, per row, the attack or shot count and the free-fight count it compared with. If H-P's probes are in the build: P1's line at claim end (acts per leaf, displacement toward and away split by attack state) gives `lungeShare` directly.

What the stand session can NOT give S5: `kiteSpeed` while shooting, `strafeSpeed`, `shotStop` (H-M2). `arriveError` and `arriveZ` over real legs, `refileStall`, `blockedShare` on fresh points (the goto rows, P-B, P-C).

Sample sizes that make a row usable: 100 attacks begun under a melee stand for `lungeShare` to plus or minus 0.1. 30 excursions for `walkBackSpeed`. 20 orders for `startDelay`. One fear event is an anecdote: three are the least to set `fearUncarried`.

Two asks that cost almost nothing:

1. Run the melee stand at both R 64 and R 32 with at least two followers, and let each hold run a full minute in contact. The attack count is the scarce number.
2. One stand on stairs or a slope if the place has one, so the periodic line shows whether `dist` settles above `arriveR` there. That is the only hint of `arriveZ` the session can give. If `dist` in these lines is 3D, a tier-C log change in Harbinger to print the height apart would make it a measurement. Not required for this session.

The evidence goes to `_evidence/deck-strict-<date>/` with the deployed DLL hashes, and the field diagnosis rule holds: the coordinator gathers, an Opus agent concludes.

## 11. What can start now, and what waits

| Now | Waits for | Then |
|---|---|---|
| S5-A, then B, C, D, F in the sim tree | nothing | |
| S5-E1 beside them (own worktree) | nothing | |
| The pilot, the difficulty calibration, the replay proofs, the 12-fight end to end | S5-C, S5-D | |
| The sensitivity arms | S5-B (IAUS only, no oracle) | names the rows the stage waits for |
| The freeze, the one-time measurements, **the road-off stages of the round** | S5 merged, and P1 ruled for the caster cells (section 12) | about 500 CPU hours that need no Deck |
| The acceptance of the scenario set on placeholders | S5-D | a FIRST verdict. It is run again on the field residues, on the same held-out seeds, because the residues are not the generator |
| `calib_point.py` against real lines | the stand session's log | the residue file with `field` rows |
| `stage_spots`, the road-on joint check, the road-on gap | the sensitive rows measured (section 9), the acceptance passing on them | the tuned movement |
| Kite weights, if the stage ran on `strict` alone | H-M2 and its session | a delta stage: kite kinds and the hold rows only, everything else fixed, the joint check again |
| Disengage weights | H-M3 and its session | the same delta shape |

## 12. Traps

1. **A binary change after the freeze kills the round's cache.** Every executor row is coded first. Field numbers are values. `--bin-spots` is the escape if the executor's code is wrong, and it costs only the road-on part.
2. **Reading an oracle gap on the road before S5-C.** It is not a gap (F3). No number from `--oracle` with `--position-road` other than `off` is quoted from a binary older than S5-C. The timing of 7.1 is quoted for its cost only.
3. **The highest mean.** It gave 9 / 41 / 20 / 29 percent with every arm Free. It is deleted from the verdict line of `--mode-test` in S5-B and never returns.
4. **Resolved is not power.** 0 of 360 resolving under the null proves the false rate, not that a real effect would be found. The leave-one-out check of 5.3 is the power check.
5. **Tuning the generator on the acceptance seeds.** Seed families are in the manifest and the driver refuses a mismatch. A changed generator means a new held-out family.
6. **A one-sided class waved through.** Section 5.4 or it fails.
7. **The oracle deciding oftener than the pass.** Order options exist only at windows where the pass would decide for him. The count is printed and a spot-test row holds it.
8. **Kinder lab.** The fourteen items of the review's list each have a row now or are named as not modelled in the residue file's header (package areas, water, flight, mount). A row left at its kind end by default is the mistake: placeholders sit at the middle or the harsh end, and `blockTold` and `refileStall` run harsh until P-B and P-C land.
9. **Free has no leash in the lab.** The contract is "Free ends the claim". MFO's client must do exactly that (M-1), or the lab bounds Free by R. Whoever writes M-1's Free path reads this line.
10. **A cut that changes picks without a replay proof.** Cuts 4 and 6 are not on in a tune until their table is in this file.
11. **Hold flip-flop.** Without `hold_kept` the pass files and releases a claim every window on an edge. The orders-per-minute fence is in the stage for that.
12. **Searching what the game cannot do.** Disengage weights, leash radii no dev gambit row passed, kinds behind a closed cap: the not-searched list, with a test.
13. **Stealth baselines.** The v4 stealth baseline moved with S4's `self_light` (19). The round's baselines are made on the frozen binary, never carried from before.
14. **P1 is outside S5 and still blocks the caster cells.** The lab offers Wall, Rune and impact-hazard spells the game's book drops. Until marth has ruled B865 and the lab's `StaticDrop` uses native's predicate, the conjurer, warlock and the Vanilla world's caster cells cannot be accepted, road on or off.
15. **Two things named P1 to P4.** The pretune review's driver findings (this plan's S5-E1) and the tight-movement plan's probes. This file writes "driver P2" or "probe P2" where it matters.
16. **Level and difficulty calibrated on a tuned value.** The calibration arm is road-off IAUS on the round's START table. It is redone only when the generator changes, never when a value is accepted.
17. **The extracted places are not in git** and differ by machine. The acceptance prints the strata it filled and the hash of every place, and a run on another library is another run.
18. **`ModeSet.h` is not in `Scenarios.h` on purpose** (a scenario there moves the v4 baseline). The new generator stays outside it too.

## 13. Decisions for marth

1. **When the `spots` stage runs.** A: once, with caps `strict,kite`, after both the stand session and the H-M2 session are read. B: after the stand session with caps `strict`, then a delta stage for Kite after H-M2 and one for Disengage after H-M3. Recommend A if H-M2 lands in the same field cycle as the stand tests, because Hold weights tuned without Kite are tuned for a shooter with no other answer and the delta cannot fully undo that. Recommend B if H-M2 is more than one field cycle away: the road-on part is then paid about 1.3 times, and Stand ships sooner. The sensitivity arms will say how much A and B differ before anything is spent.
2. **Budget for the road-on part.** Lean 350, standard 800, full 1,500 CPU hours (section 8). Recommend standard. Lean cannot support "need not be redone" per class.
3. **A class with one answer.** A: it passes by the four proofs of 5.4 and its other kinds are not searched. B: it must show two modes or the set is extended until it does. Recommend A. B would bend the set until an archer kites, whether or not that is right.
4. **The margin.** 0.10 U a decision and 0.25 U a fight, where the review said 0.01 U. Recommend the larger: 0.01 cannot be measured at the oracle's own ceiling. A note more than a decision, here so it is not a silent change.
5. **The caster cells and driver P1 (B865).** A: rule B865 now and fix the lab's drop to native's predicate before the freeze. B: freeze without it and leave conjurer, warlock and Vanilla's caster cells out of this round. Recommend A, since the round is the one that should not be redone.
6. **Two binaries.** Allow `--bin-spots` (section 6.2). Recommend yes. It is 30 lines against losing 500 CPU hours to one executor fix.

## 14. Acceptance of S5 as a whole (what the reviewer of the last package signs)

- Road off: v2, v3, v4 baselines byte identical with main at 1 and 8 threads. The DLL has no new caller. Local MSVC build green on the final SHA.
- Golden v6: every old line is the old line with fields appended, gcc and MSVC replay 0 failed.
- `oracle_replay.py --check` on a dump with orders. The cut tables of 7.2 for cuts 4 and 6.
- The subject's claim share in oracle fights. The F3 symptom gone.
- `--mode-accept` runs end to end on a pilot of two classes and prints the lines of 5.3 with their bounds. The histograms are of fought scenarios.
- `test_tune_driver.py`: P2, P3, P4, the loot key, `stage_spots`, the not-searched list, provisional values, the residue key.
- The 12-fight end to end with and without the order race, and section 7.1 repeated on the real thing, written into this file in place of the estimates.
- MAP.md's sim entries and `iaus-sim-v4.md` get a section 22 "S5 as built", with differences from this plan.
