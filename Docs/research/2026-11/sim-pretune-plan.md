# Sim bundle A, before the one T1/T2 tune: the plan for what is left

Branch `feat/sim-v4-pretune` (worktree `/tmp/claude-1000/worktrees/sim-pretune`), on `origin/main` `efd097dc` with `feat/sim-v4-choice` `e663ca7a` merged. Written 2026-11-24 by the Fable planner for an Opus coder. Log: `/mnt/gaming/modlists/Projects/_agentlogs/mfo/sim-pretune.md`.

Scope ruling (marth 2026-11-24): the tune covers T1 and T2 behaviour only, 15 cells (the 12 tree-ON rows of T2 and below, plus the 3 base cells). T3 to T5 mechanics (Arch-Mage hands, T4 passives, Dragonborn) are OUT. Movement (spots) and light are the NEXT bundle: leave the extension points named below, build none of it.

Line numbers are of commit `bd604951`. Re-grep before editing, they drift.

## 0. What is already coded and pushed

| Commit | What | Proof run |
|---|---|---|
| `c21d2341` | Item 1. `tune_driver.py` `LOOT_NOT_SEARCHED` holds `loot.gatePassU` and `loot.gatePassRunU` with `_PASS_WHY`. `test_tune_driver.py` list extended | driver tests green |
| `bd604951` | Item 2 code and item 3 code, listed below | driver tests 56 OK, `live/test_live.py` OK, `--loot-test` 15 PASS, baselines v2, v3, v4 `0 metric(s) changed` at 8 threads |

Item 2 as coded (choice review `_agentlogs/mfo/review-sim-choice.md`):
- R2: `stage_joint` plays the final arm with `final_sets()`, the class card too (`card_potion_gate(..., new_sets)`, `play_metrics(..., sets)`). `stage_gap` final arm and `allf` carry `fsets` (`gap_runs` task tuple grew to `(world, cls, curves, seed, subject_only[, hold[, sets]])`). `stage_recalib` plays both arms with `final_sets()`. The joint runs when there is a class value OR a shared value.
- R3: `held_out_check(a, b, who, a_sets, b_sets)` (above `u_of`): IAUS arm only, held-out places and books, seeds `HOLD_FINAL0 = 7601..`, paired per fight, cells weighted equally. Worse = pooled mean under `-HOLD_SET_SE` (1.5) se, or one cell worse by `worse(d, se, many=True)`. Called at the end of `follow_class` (drops the class's set), after the spells rounds (drops the spells set), and in `stage_joint` (fails the joint). New auto-decision kind `held-out set` in `RULES`.
- R4: `Oracle.h` an alternate table's pick that is not among the options is charged idle's loss (`altCast` decides whether the decision counts when the pick is missing). `sim.cpp` prints `SPELLALTMISS <class> <alt index> <n>`.
- R5: `follow_class` fresh check reads `follow_iaus(cls, base, fresh)` on both sides.
- SEV-4: `Oracle.h` best rule, `sd == 0 && d == 0` retires only from `kTwinMin` streams when `cfg.pair >= 0`. `Policy.h FillSpellEstimate` zeroes `levelCap` when the spell has direct damage. `SPELL_SCALARS` tries 0.0 and 0.1.
- SEV-5: `TCrit` adds the fifth expansion term under 8 streams only (so the default race is bit for bit the old one). `env_ident()` hashes every `.mfoenv`. `Load.h` fallback heal comes from the book's own half.
- Round examples (`live/run_round.py`, `live/README.md`) say `--cells 15`. The driver's default was already 15.

Item 3 as coded (loot review `_agentlogs/mfo/review-sim-loot-r2.md`):
- R1: `LootLab.h Episode::Hard()`, `ScoreOver(shippedHard)`. `LootCli.h RunReport` under `--loot-score` replays the same episode under the shipped `LootTuning` when the table under search differs (`pairShipped`) and charges only the excess. Measured on Tuxborn held-out, 4 seeds by 6 episodes, beta1: `loot.buffScale=0.475` against shipped was +56 per episode before the fix (72.8 to 129.2) and is -2 after (133.9 to 132.0).
- R2: `LootCli.h` prints `LOOTSEED <seed> <value> <row>` for every gate row. `loot_report` reads them into `"seeds"`. `loot_gate_check` returns `(reason, rows)`, paired per seed, `LOOT_ROW_SE = 2.0`, rejects on `LOOT_REJECT_ROWS` and on one more crime or guard-seen take. `CFG["loot_gate_seeds"]` 16 (smoke 2). New `RULES` kind `loot gate`.
- R7: `_GATE_WHY` reworded.

## 1. Left on items 2 and 3 (small, do these first)

1. **Doc sentences.** `Docs/research/2026-11/iaus-sim-v4.md`:
   - 14.3 (line ~1079 block): the sentence "So no row gets better because a table wants less" is false (loot review R3). Replace with what is true: `leftRR` and the censored waits do not see an ignored dibs stack (the claim clock `Loot.h TierReleased` runs only for categories in the live plan), value, `dibsNear` and `roadLeft%` do catch it, and `loot_gate_check` now rejects on those rows.
   - 14.5: the "2 to 5 times" sentence gets the R7 wording now in `_GATE_WHY`.
   - 15.5: add the set checks and the whole-table joint. 15.7: the tune command becomes `--cells 15`, and the bounds command (pre-tune item 1) drops `priest` and `--parked` (T3 is out of scope).
   - 15.8 and 15.9: move the fixed review items out of Open.
2. **Loot review R3, code part (optional, conservative choice: do it only if under 40 lines).** Count a never-scanned in-reach dibs stack as `rr` in the ledger: `LootLab.h` ~255 (`why = "player dibs (never scanned)"` branch). Run the shipped plan's claim clock for the ledger there. If it moves the v4 baseline's loot rows, it is in scope of the one baseline rewrite. If it is not done, add it to `Docs/REVIEW-BACKLOG.md` with the review's verbatim text (SEV-3 at 59d0a63d).
3. **`Docs/research/2026-11/tuner-auto-decisions.md`**: add the two new `RULES` kinds verbatim.
4. **Backlog entries** (`Docs/REVIEW-BACKLOG.md`, next id after MFO-B858, verbatim review text): choice SEV-4 "proof coverage: the replay is 74 decisions, Tuxborn mage-line only", loot SEV-4 R4 (censored waits saturate), loot SEV-4 R6 if the baselines are not rewritten by you (they will be, so close it instead).
5. **Coordinator checks.**
   - (a) env-hold against `<set>/heldout`: ANSWERED by reading `Env.h:324-328`. The hash split is applied only under `!held`, so batch L's held-out list and batch C's hash split never both filter. Add one CLI check so it stays true: `--env-set Tuxborn/heldout --env-hold train` and `--env-set Tuxborn/heldout` print the same place count in the headline.
   - (b) driver smoke with the cuts ON: started on the merged tree before the edits, not finished when this was written. Run it again on the final binary: `python3 tools/iaus_sim/tune_driver.py --bin $B --out <scratch>/smoke --smoke --jobs 8 --worlds builtin,Tuxborn --classes evoker,healer --stages prep,follow,spells,joint,gap,summary`. Expect `exit 0`, a `follow` and a `spells` log line, `joint:` line, and in `summary.json` `stages.joint.held_out` present when anything was accepted (smoke forces acceptance). Check `progress.log` commands of the oracle arm hold `--oracle-prune target --oracle-pair 0.10 --oracle-nmin 4` and the IAUS arm's do not.
   - (c) Tuxborn healer regret 31.97 (old rules) to 58.56 (today), doc 15.4 table, 6 fights: NOT answered. The doc itself says six fights is a small table. Check: `$B --no-json --threads 8 --world v3 --books field --seed 1 --fights 1 --rating 0 --gap-scenarios 0 --gap-dump --oracle-subject-only --spell-regret --classes healer --class-oracle 6 --list ~/.cache/mfo-list-balance/Tuxborn.sim.json --potion-worth legacy --spell-rules old` and the same with `today`, on seeds 1, 2, 3. Read the `SPELLFIGHT` lines: the two runs are NOT paired per decision (the fights diverge), so compare per-fight regret means with their se over 18 fights. Verdict rule: a difference under 2 se is noise. If it is beyond, split by the `SPELLREG` cause column: a rise in `weight` or `fact` under today's rules on heal decisions is a fault to report (likely suspects: the concentration halving of `FillSpellEstimate`, the heal fallback), a rise in `gate` is the ask list. Report, do not fix inside this bundle unless it is a one-line mirror error.

## 2. Item 4: the gate rule mirror

### What native does now (`efd097dc`)

- `native/iaus/LootNeed.h:138-152`: `gateReadmitSec 0`, `gateReadmitMoveU 0` (both off), `gateRepeatU 300`, `gateShortU 300`, `gateNearU 200`, `gatePassU 150`, `gatePassRunU 1000`.
- `native/iaus/LootNeed.h:158-178`: PURE `GatePt`, `GatePassCrossed(block, target, a, b, tolU, zBand)`, `GatePassStepPlausible(dist, dtSec, runU, slack)`. The lab already compiles this header (`Loot.h` uses `MFO::Iaus::LootTuning`), so call them directly. Never restate them.
- `native/logistics/LootTravel_internal.h:511-575` `GateReadmitSweep`: once per follower service lap and per party-combat lap. Order per record: timer, then near (any OTHER active party member or the player within `gateNearU` of the target, same space), then pass (the player's step segment between two consecutive samples), then the move rule (off). The pass sample: same space, `dt <= kPassMaxGapSec` (3.0), `GatePassStepPlausible(step, dt, gatePassRunU, kPassSlackU = 64)`. A failed test drops the segment but the sample is always replaced. `kGateFloorBand = 256` (`:394`) is the z band.
- The idle lift (`ClearGatesBy` after four idle ticks) is GONE.
- N3: `Service.cpp:1350-1378`. `LiveGateAtPoint` (`LootTravel_internal.h:704-725`) first: a second block within `gateRepeatU` of a LIVE gate's block point, in the target's space, with the target BEHIND that gate, gates the new target only (`MarkGated(..., targetOnly = true)`) and is NOT stuck evidence. A real gate (`MarkGated` true) sets `feedStuck = false`. If the target is not behind the live gate he is jammed: stuck evidence as before (review r2 R2 of fix/loot-native).
- N2: `Service.cpp:1380-1392`. No obstacle and no ground closed on the SAME item since his previous block is a stall strike (`MarkTravelStalled`, blocklist, second strike sticky), `g_lastBlock.erase(id)`, and a deferred item is not re-dispatched in the tick that deferred it (`LootScan.cpp:862` region).
- N1: `LootTake.cpp TierReleased`, near is enough (no facing).
- N4: `LootTake.cpp:383, 422, 450, 491, 556`: every category's peek weighs like its take (`FitsCarryWeight` on the planned stack).

### What the lab has (`tools/iaus_sim/Loot.h`)

- `Rules` `:73-75`: `dibsNearEnough 1` (N1 already mirrored as the default), `gateEventsOnly 1` (N6 mirrored). Comments say "NOT IN NATIVE YET" and "0 = today's native": stale.
- `GateSweep` `:384-392`: timer, near, move (only when `gateEventsOnly <= 0`). No pass rule.
- Idle lift `:648-649`: still lifts his own records after `idleTicks`. Native no longer does.
- Block reaction `:545-556`: `real` then `StuckRepeats` then `MarkGated`. No `LiveGateAtPoint`, a real gate still feeds the stuck run, no N2 stall strike on the same item.
- N4: read `Loot.h` carry peek (grep `carryCap`, `HasLoot`, `"fully stripped"`) and the scenario `carry-full` in `LootCli.h` to see whether the peek weighs.

### Mechanism

1. **Pass lift.** In `World` (Loot.h `:273` block) add the sample `struct PassSample { bool valid = false; EQ::Vec3 pos; double at = 0; } pass;` (one per loot world: the lab has one space per episode, so no space key. Leave a comment naming the space check as native's and where a multi-space world would add it). In `GateSweep`, before the per-record loop: read the player's position `P(Fp->b[0])`, form `havePass` exactly as native (`dt <= kPassMaxGapSec`, `GatePassStepPlausible`), replace the sample. In the record loop after the near rule: `if (!lift && havePass && T->gatePassU > 0 && MFO::Iaus::GatePassCrossed({G.block}, {G.tpos}, {passA}, {passB}, T->gatePassU, R.gateFloorU))` then `why = "the player moved through"`. Order of rules as native.
2. **Constants.** `kPassMaxGapSec` and `kPassSlackU` live inside a native function body with engine types around them, so the lab cannot include them. Add two `Rules` fields `passMaxGapSec = 3.0f`, `passSlackU = 64.0f` with the native file:line in the comment, register them in `SetRule`'s table (`:80-88`), and add a driver test in `test_tune_driver.py` that greps both literals out of `native/logistics/LootTravel_internal.h` and compares (the pattern of `test_loot_knobs_cover_loottuning`). `gateFloorU 256` exists already (`:60`): add the same grep for `kGateFloorBand`. Do NOT edit native to share them. If you believe they must move into `iaus/`, STOP that step and report (native has its own route).
3. **Sweep cadence.** Native sweeps per service lap and per party-combat lap. The lab calls `GateSweep()` at `:571`. Confirm it also runs during party combat (the player walks through doors mid fight). If not, call it from the combat-lap path too, and say so in the doc.
4. **Idle lift.** Remove the `:649` lift from the default path. Keep it behind an older-build rule so the pinned scenarios still reproduce: add `Rules::gateIdleLift = 0.0f` ("1 = builds before efd097dc"), wired to `--loot-rule gateIdle=1`.
5. **`gateEventsOnly`.** Native's default is now `gateReadmitMoveU 0`, so the lab's extra switch is redundant with the table. Keep the field (scenarios and pins use `gateEvents=0`), but rewrite its comment: it is "the builds before efd097dc with `gateReadmitMoveU 500`". Update `LootCli.h` texts that say "today's native" (`:126` and the `ruling-gate-events-only` scenario) and the block comment at `Loot.h:73`. `tune_driver.py _GATE_WHY` already says it.
6. **N3.** Add `LiveGateAt(int si, const EQ::Vec3& bp) const` returning `{bool live, bool behind}`: a gate whose `block` is within `T->gateRepeatU` of `bp`, and whether source `si` is behind it (reuse the cone test of `GatedBy` `:371-377` against that one gate, with `targetOnly` ignored. Read native `LiveGateAtPoint` for the exact "behind" test and copy its conditions). In the block reaction `:549-552`: `real` first. If a live gate is at the point and the target is behind it: `MarkGated(fi, si, bp, false)` (target only), no stuck note. Else if `real`: `MarkGated` and no stuck note. The `StuckRepeats` branch stays only for "live gate at the point, target not behind it".
7. **N2.** In the `ground == 2` branch for the SAME target: a stall strike (the lab has `stallStrikes` and `MarkFailed`, read `:550-556` and the leg end states), clear `f.hasLast`, stuck evidence, and bar re-dispatch of that source in the same tick (a per-follower `deferredAt` stamp equal to `Now()`).
8. **N4.** Make every category's peek weigh the planned stack like the take. One shared function, `FitsCarry(const FState&, float addWeight)`, called by peek and take (design rule: one reader).
9. **Older builds.** Everything native changed needs an "older build" rule value so the fault-reproducing scenarios keep reproducing: `gate=old1117` exists. Add ONE rule `Rules::native1124 = 1.0f` ("0 = the build before efd097dc: idle lift, a gate feeds the stuck run, no live-gate rule, no same-item stall strike, equipment-only peek") rather than five switches, plus `gateEvents=0` for the move lift. The three scenarios that "reproduce a fault of today's rules" (`freeze-1117g`, `locked-door-cycle`, `carry-full`) must be re-pointed: their "today" arm becomes `native1124=0` and they gain an arm under the default rules that must now PASS. Reword `--loot-test`'s summary line accordingly ("3 of them reproduce a fault of the builds before efd097dc") and the CLI check in `run.sh` that greps it.

### Scenarios to add (`LootCli.h`, the `v.push_back({...})` table from `:101`)

Fixture: reuse `envs/loot-gate` if its geometry has a door with a block point in front. Else add one fixture through `envs/make_fixtures.py` (and its `--check` list in `run.sh`).

| Name | Setup | Must |
|---|---|---|
| `gate-pass-real` | gated record, the player walks from the near side through the block point to the far side at walking speed | lift, reason "the player moved through" |
| `gate-pass-jitter` | the player steps back and forth within 64 u at the near side of the block point, never past the shifted plane | no lift |
| `gate-pass-beside` | the player crosses the plane more than `gatePassU` to the side | no lift |
| `gate-pass-other-route` | the player reaches the far side by another door, crossing far from the block point | no lift by pass (the near rule may lift when he reaches the item: assert the reason) |
| `gate-pass-teleport` | the player's position jumps across in one sample, farther than `gatePassRunU * dt + 64` | no lift |
| `gate-no-idle` | the `locked-door-cycle` setup under default rules for 120 s | 0 "idle" re-admits, legs under a small bound, and the old arm (`native1124=0`) still shows the cycle |
| `gate-second-item` | N3: two items behind one locked door | second item "GATED (target only)", 0 "stuck himself" |
| `stall-same-item` | N2: the `freeze-1117g` place under default rules | legs to the unreachable potion at most 3, everything else taken |
| `carry-peek` | N4: `carry-full` under default rules | 0 legs to a stack that does not fit |

Unit rows for the pure function are native's self-test rows (`native/selftest/TestsT2.cpp`, t2.theft e). Do not duplicate them, the scenarios test the lab's wiring.

### Traps

- The teleport scenario needs the lab's player mover to be able to jump. If the player body is moved by the plan (`Plan::*`), add a scenario-only hook that sets his position. Do not add a general teleport.
- `GatePassCrossed` shifts the plane `min(96, len/2)` toward the target: a block point closer than 2 u to the target (len < 1) never lifts. Scenario geometry must keep the item more than 200 u behind the door or the near rule fires first.
- The sweep's first sample has no segment. A scenario shorter than two sweeps proves nothing.
- Determinism: the pass sample is world state, so it must be copied with `Episode` (the loot oracle replays copies, `LootLab.h:296`). Put it in `World`, not in a static.
- Baselines: the v4 baseline holds no loot rows unless `--loot-*` is on (confirm with the features-off compare before touching anything else). The loot report numbers in doc 14.3 WILL move (idle lift gone, N2 to N4). Re-measure that table once at the end.
- `calib_loot.py` reads field log event names. New event names ("the player moved through") must match native's log text (`LootTravel_internal.h:567`) so the tool's keys line up.

### Acceptance

- `$B --no-json --threads 8 --loot-test` prints `LOOT-TESTS PASS: 24 scenarios, 0 failed` (15 + 9, adjust to the real count) and the reworded tail.
- `$B --no-json --loot-scenario gate-pass-real` shows the re-admit line with the pass reason. The four negative scenarios show 0 "moved through".
- `--loot-rule native1124=0 --loot-rule gateEvents=0` reproduces the three old faults (legs counts of doc 14.6 within their old values: 27, 25, 15).
- `--loot-report 2 --seeds 1,2` at 1 thread equals 8 threads (existing check).
- Driver tests: the three constant greps.

## 3. Item 5: summons as real bodies (S14, the T1/T2 part: conjurer, warlock)

### Today

- `World.h:1349-1356`: a Summon-nature cast spawns ONE generic body: `ActorSpec` "Summon", race `AtronachFlameRace`, level = the caster's, hp `60 + 12 L`, unarmed `8 + 0.8 L`, no magicka, run 400, at caster + (150, 60), expiry in `airUntil < 0`. `Role::Summon` (`:107`).
- `World.h:1326-1331`: reanimate raises the corpse's own spec (card G7). Leave as is ("no reanimate beyond what exists").
- No limit: `ctl[].summonUntil[spell]` is per SPELL, so two different summon spells give two bodies, and the same spell again gives a second body while `liveSummon` (Policy.h:177) only tells IAUS the first is live. The oracle is not bound by `liveSummon` at all.
- The summon acts through the foe AI path (grep `Role::Summon` in `World.h`: `:743` Down, `:761`, `:801`, `:1684`, `:1731` expiry). It has no combat style of its own and no ranged attack, though a Flame Atronach is a ranged caster.
- Oracle: `Oracle.h:364` a summon has "no single number", its value comes only from rollouts. That part is already "valued as a body". `Oracle.h:149` and `World.h:801` exclude summons from heal targets.
- IAUS: native `iaus/Generate.cpp:962-964` drops a summon cast while `liveSummon`. `iaus/Catalogue.cpp:447, 632` the family weight and `kSummon` considerations. Native gate `cast/Summon.cpp:121-133` `SummonLimit` = `iMaxSummonedCreatures` (GMST, default 1) through entry point `kModCommandedActorLimit` (0x44), rounded half up. `:240-262`: the count compared is EVERY `commandedActors` entry, dead ones and reanimated thralls included, plus other summon spells still appearing.

### Mechanism

1. **Who is summoned (data).** `tools/list_balance/extract_world.py`: MGEF `DATA` offset 8 is the associated form. For archetype 18 (Summon) export `assoc` = the NPC key, resolved through the existing `actor_ref` (`:291`) so the summoned actor's race, level, stats, spells, combat style and weapons come through the SAME tables every foe uses (`b_npc :399`, `b_csty :243`). In `effects()` (`:150-163`) add `summon=actor_ref(assoc)` to the effect dict for that archetype only. `compile_world.py:91-92` passes it through as `summon` (an index into the compiled actor table, the one `spell_ref`'s neighbours use). No per-list logic, no name tests. Verify the offset by dumping one vanilla record first (`ConjureFlameAtronach`'s effect must resolve to an NPC whose race edid holds "AtronachFlame").
2. **Sim load.** `Data.h SimSpell` (`:190-212`): add `std::shared_ptr<const ActorSpec> summonSpec;` filled in `Classify` (`:419-475`, where `summon = true` is set at `:434`) from the json's actor row through the SAME function that builds a foe's `ActorSpec` from the world json (grep `ActorSpec` construction in `Load.h`. One builder, called for foes and summons). When the json has no `summon` (the built-in fixtures, an old cache file) keep today's generic atronach, built by ONE function `GenericSummonSpec(level)` moved out of `World.h:1351-1352`, marked MODEL.
3. **Level.** A leveled summon NPC (PC level mult flag) levels with the CASTER in the engine. Use the existing level resolution for foes with the caster's level as the reference. Mark MODEL if the extractor gives a fixed level only.
4. **Limit.** New in `Fight`: per caster `std::vector<std::uint32_t> commanded` (body ids, in cast order, summons and raised thralls both). `SummonLimit(const Body& caster)` = `kSummon.limitBase` (1) plus the perk row. The perk comes from the perk table the lab already loads for other entry points (`compile_world.py:274-312` reads entry points by number: add EP 0x44 `ModCommandedActorLimit` to `ptab` as `summonLimit` [threshold, value] rows the same way `spellCost` is done at `:277`). In the lab a follower has the perk when his class row's skill meets the threshold (the rule the other perk rows use: read how `ptab['spellCost']` is consumed in `Load.h` and do the same). T1/T2 classes normally do not reach Twin Souls (Conjuration 100): the code path must exist and a test must show limit 1 for conjurer and warlock.
5. **At the limit, the engine evicts.** When a summon lands and `commanded.size() >= limit`, the OLDEST entry is dismissed (`alive = false`, trace line `UNSUMMON`), dead-but-listed entries count until removed. Native REFUSES to cast in that state (`cast/Summon.cpp:243-262`), so IAUS never gets there, but the oracle and foes can. Mirror both: (a) `Policy.h:177` `liveSummon` stays per spell, and add the native gate's third rule to the lab's cast gate for the IAUS arm: a summon cast is refused when `commanded.size() + pending >= limit` unless the same spell's own summon is the one listed (read `cast/Summon.cpp:205-270` and copy the verdict order: Live, Landing, limit). (b) the oracle's option list (`Oracle.h Ok()` near `:480`) applies the same gate: the oracle is class-bound and engine-bound, it cannot cast what the game refuses.
6. **Body on the navmesh.** `World.h:1353`: replace the fixed offset by `EnvWorld.h`'s placement helper (`:32` "Put a body on the mesh near where it is (a spawn, a summon, a landing)"). Confirm the summon path calls it under `g_v4.env`. The engine places a summon at the aimed point within the spell's range for location-delivery conjurations: MODEL = 150 u in front of the caster toward his target, snapped to the mesh, same floor.
7. **Own AI and combat style.** The summon runs the SAME actor AI a foe of its spec runs (ranged style, spells, melee), with party membership flipped. Find where foe AI branches on `Role::Foe` and make the test `!IsFollower && !IsPlayer` or pass through `Party()`. A Flame Atronach must cast its Firebolt at foes. Check `World.h:658-663` (friendly fire lane) covers a summon's shots: a summon's projectile into a follower is held the same way.
8. **Targets and blocking.** Foes must be able to target a summon (it draws threat: that is the summon's main value) and it blocks doorways as a body under `g_v4.bodies`. Check the foe target choice (grep the threat / nearest pick in `World.h`) includes `Role::Summon`. `World.h:761` excludes summons from the burst read: keep. Followers never heal or buff a summon (`:801`, `Oracle.h:149`): keep.
9. **IAUS values it as a body.** Native's summon considerations are what they are (`Catalogue.cpp kSummon`). The lab feeds them: `Policy.h:177` and whatever `Snapshot` fields `kSummon` reads (grep `kSummon` rows in `Catalogue.cpp:632` region and list the inputs. Each must be filled from the lab's world, none invented). If a consideration needs a number the lab does not have (the summon's strength), mark the fill MODEL and name it in the doc. Do NOT add considerations: that is native work.
10. **Tunables.** One table in `World.h` (or `Data.h` beside `kFieldBook`): `struct SummonModel { int limitBase = 1; float spawnAheadU = 150; float genericHpBase = 60, genericHpPerLevel = 12, genericUnarmedBase = 8, genericUnarmedPerLevel = 0.8f, genericRun = 400; } inline constexpr kSummon;` Every number above moves into it. MAP: "to change how a summon spawns or its cap, edit `kSummon`. To add a summon kind, nothing: it is the list's NPC record."
11. **Switch.** `--v4 summons=on|off` in the `V4` struct (`Env.h:80-90`, and its `Defaults()` and name table). OFF = today's generic single body with no limit, draw for draw. This is what proves "features off = 0 changed". Default ON only under `--world v4` (the tune's world). v2 and v3 never see it.
12. **Extension point for the next bundle (do not build).** Leave a comment at the spawn site: "spot choice for a summon (where to place it) is the movement bundle's: the caster's aim point comes from `Spots` then".

### Validation against native

Read `cast/Summon.cpp` whole (it is short) and write, in the doc, a table "native rule, lab line, proven or MODEL". Proven = the gate order and the limit arithmetic (copy the half-up rounding). MODEL = spawn position, the summon's AI being the foe AI, level scaling, the perk grant by skill threshold.

### Traps

- **Random draws.** Adding a body with spells consumes draws from the fight's stream in a different order. With `summons=off` the old code path must run EXACTLY (keep the old block under the switch, do not "simplify" it), or the v2 and v3 baselines move.
- `Add()` invalidates references (`World.h:1330` comment "`x`, `tg` are stale after Add"). The eviction and the placement must not hold `Body&` across `Add`.
- `nextId` gives ids. The oracle copies fights: `commanded` holds ids, never pointers or indices that a copy or a compaction breaks.
- The cache file `~/.cache/mfo-list-balance/*.sim.json` must be REBUILT (`extract_world.py` then `compile_world.py`) for the new field. A stale file silently gives generic summons. Print in the headline which it was (`summons=list|generic`) and make the tune driver's `check_world` refuse a list world whose json has no `summon` field on any Summon-archetype effect. The json is game-derived: never commit it.
- The driver's cache keys hold `sha(sim.json)` (`ident()`), so a rebuilt list invalidates old results by itself. Good, and it means the pre-tune measurements of doc 15.7 run AFTER this.
- Potion-worth tables are measured per list (`prep`): they are re-measured automatically for the same reason.
- Field-size books give a conjurer 6 summon spells (`Load.h:229`, an ASSUMPTION). With a real limit most are refused: expect the conjurer's cast mix and his regret table to move a lot. That is the point, not a regression.

### Acceptance

- New scenario(s) in `Scenarios.h` or the env scenarios: `summon-limit` (conjurer with two summon spells, limit 1: never two live summons, IAUS never casts the second while the first lives, the oracle neither), `summon-evict` (a foe conjurer at its limit casting again: oldest dismissed), `summon-ranged` (list world only: a Flame Atronach deals ranged damage), `summon-doorway` (v4: the summon stands on the mesh and blocks a one-body door).
- `--world v4 --v4 summons=off --classes beta1 --compare baseline_v4_builtin.txt` = `0 metric(s) changed` BEFORE the rewrite. v2 and v3 compares `0 changed` with no switch.
- Same run at `--threads 1` and `--threads 8`, conjurer and warlock, list world: identical first line.
- `extract_world.py` self-checks (`ck()` at `:534`) still pass on the three lists. Count of Summon effects with a resolved actor printed.

## 4. Item 6: hazard damage (walls, runes)

### Today

- `Data.h:166-168`: `kIndirectArch` = Script 1, Cloak 35, EnhanceWeapon 39, SpawnHazard 40. A spell with one of them and no direct damage is "unmeasured" (`Estimate::DamageUnmeasurable`, native's own function). The lab's WORLD deals nothing for it (doc 15.2, 15.9).
- Native: `iaus/Fills.cpp:880-939` (the indirect switch at `:897`), `cast/Actuation.h:907-909` (`impactHazard`, `spawnHazard`, `hazardHostile`), `cast/Archetype.cpp:211-218` `HasImpactHazard` (the effect's impact data set holds an impact whose hazard is set = a Wall), and the SelfHazard read just under it (a SpawnHazard effect's associated form is the hazard, whose spell is read).
- Extractor: `extract_world.py:25` loads no HAZD, IPDS, IPCT, PROJ or EXPL. `:141-148` MGEF reads flags, cost, school, resist, minSkill, archetype, av, castType, delivery, av2.

### The three fields (per effect, in the world json)

`hazard`: null or `{radius, lifetime, interval, limit, flags, spell: <a spell row>}` where `spell` is the hazard's own spell exported through the SAME `b_spel` (so its effects come out of the same `effects()` reader and are classified by the same `Data.h Classify`). Reached by exactly the two roads native reads, in this order:
1. archetype 40: MGEF `DATA` +8 (associated form) is the HAZD.
2. else MGEF `DATA` +100 (impact data set) to IPDS, its `PNAM` pairs (material, impact) to IPCT, the first IPCT whose hazard field is set.

Plus `hazardVia`: `"spawn"` or `"impact"`. Plus, on the SPELL row, nothing new: `delivery` and `castType` are there (a wall is concentration + aimed, a rune is fire-and-forget + target location).

VERIFY EVERY OFFSET by dumping vanilla records before writing the reader (the "never guess" rule applies to record layouts too): HAZD `DATA` is believed to be limit u32, radius f32, lifetime f32, image space radius f32, target interval f32, flags u32, spell formid, light, impact data set, sound (40 bytes). IPCT's hazard subrecord is believed to be `NAM2`. MGEF +100 follows from the offsets the extractor already uses (archetype 64, castType 80, delivery 84, av2 88). Dump `WallOfFlames`-like and `FireRune`-like records of Skyrim.esm with the extractor's own `rec_subs` and print subrecord tags and lengths. If a RUNE's damage turns out to live in the projectile's EXPLOSION (PROJ to EXPL to its enchantment) and not in a hazard, that is a third road native does NOT read as a hazard: then runes stay unmeasured in this bundle, say so in the doc and backlog it. Do not invent a road native lacks without marking it MODEL and getting it reviewed.

`compile_world.py:91-92`: pass `hazard` through, with the nested spell compiled by the same per-spell function (recursion depth 1: a hazard spell's own hazards are dropped and counted).

### Sim mechanism

1. `Data.h SimSpell`: `struct HazardSpec { float radius = 0, lifetime = 0, interval = 1; int limit = 0; bool viaImpact = false; std::shared_ptr<const SimSpell> spell; }; std::optional<HazardSpec> hazard;` filled in `Classify` by calling the spell loader on the nested row. ONE classifier: the nested spell goes through `Classify` like any other.
2. `World.h`: `struct Hazard { EQ::Vec3 p; float radius; double until, nextTick; float interval; std::uint32_t owner; bool party; std::shared_ptr<const SimSpell> spell; int ownerCtl; };` and `std::vector<Hazard> hazards;` in `Fight` (copied with the fight, ids not pointers).
3. Spawn: in the cast-landing path (`World.h:1363 Land` for projectiles, and the concentration stream tick for walls). Wall (`viaImpact`): while the stream runs, one hazard per `kHazard.wallStepSec` at the stream's impact point (the aim point on the ground at the target's position, MODEL), capped by `limit` (oldest removed, the HAZD limit's meaning). Rune or SpawnHazard: one hazard at the target location (delivery TargetLocation) or at the caster (self delivery).
4. Tick: in the fight step, before movement resolution: for each hazard past `nextTick`, every body hostile to the owner's side within `radius` (and on the same floor, `kHazard.floorBandU`) receives the hazard spell's effects through the SAME function that applies a landed spell's effects (find the one `Land` uses for damage, resist and DoT. Do not write a second damage path). `nextTick += interval`. Expired hazards are erased in order (stable, no swap-remove: determinism).
5. Credit: damage is credited to the owner (`ownerCtl`) in the tallies the same way a DoT tick is, so Utility and the regret see it.
6. Foe AI does not avoid hazards (MODEL, named). Followers neither. The movement bundle may add avoidance: leave the comment "hazard avoidance is a spot-scoring input (next bundle)".
7. IAUS side is UNCHANGED: native still calls these spells unmeasured (`t.unmeasuredDamage`). That asymmetry is the purpose: the oracle now sees their real worth, so the regret report prices `t.unmeasuredDamage` honestly and the SpellWorth table (S11b) gets a target. Do NOT make the lab's estimate measure them.
8. Tunables, one table: `inline constexpr struct { float wallStepSec = 0.5f, floorBandU = 128, minInterval = 0.25f; int maxLive = 64; } kHazard;` MAP: "to change how spawned hazards are placed or ticked, edit `kHazard`. A new hazard kind is the list's record."
9. Switch: `--v4 hazards=on|off` beside `summons`. OFF = no hazard is ever created.

### Traps

- The built-in fixtures have no hazard spell: with no list the feature is inert, so v2, v3, v4 builtin baselines cannot move from this item. Prove it (the compare) and ALSO add one fixture spell (`Load.h:170` region, like `ConjureFlameAtronach`) ONLY under a scenario, never in the default fixture book, or the baselines move.
- Interval 0 in a record: clamp to `kHazard.minInterval` or a hazard ticks every step.
- A wall's stream already halves per-second numbers (`Policy.h:86` `per`). The hazard spell is a separate spell: do not halve it twice.
- Friendly fire: hazards hit bodies hostile to the owner only (MODEL: the engine's hazards can hit anyone. Named, backlog).
- Cache file rebuild and refusal: the same rule as summons. Do the two extractor changes in ONE extractor commit and rebuild the three lists ONCE.
- `tools/list_balance` reads game data. Nothing it writes goes into git.

### Acceptance

- Extractor: on Vanilla, the count of spells with a hazard by road (`spawn`, `impact`) printed, and at least the vanilla walls resolve with a hostile hazard spell. `ck()` checks pass.
- Scenario `hazard-wall` (a foe standing in a wall takes the hazard spell's damage per interval, a foe outside takes none, the caster's party takes none) and `hazard-expire` (no damage after `lifetime`).
- Oracle check on a list world: evoker with field books, `--spell-regret`, a wall or rune spell appears among the oracle's picks at least once in 6 fights, where before it never did (doc 15.2's sentence). Report the count, it is not a gate.
- `hazards=off` compare = 0 changed on all three baselines. 1 thread equals 8 threads.

## 5. Order, interactions, and the one-time steps

1. Section 1 leftovers (docs can wait until the end, the R3 code part now).
2. Item 4 (touches `Loot.h`, `LootCli.h`, `LootLab.h`, fixtures). Independent of 5 and 6.
3. Extractor commit for items 5 and 6 together, rebuild Vanilla, LoreRim, Tuxborn caches once (keep the old files under `backup-pre-pretune/` beside the existing backup folders).
4. Item 6 then item 5 (hazards are smaller and exercise the nested-spell loader the summon's spell list also needs).
5. Before any baseline rewrite: the three "features off" compares (`--v4 summons=off,hazards=off` for v4, plain for v2 and v3) must print `0 metric(s) changed` against the files as merged. Paste the lines in the log.
6. Rewrite `baseline_v4_builtin.txt` ONCE (summons ON changes conjurer and warlock cells even on fixtures through the limit). v2 and v3 baselines and the three J pins in `run.sh` must NOT move. If they do, a switch leaks: find it, do not re-pin.
7. Long CLI suite ONCE: `SIM_CLITEST=1 SIM_OUT=<scratch>/sim taskset -c 0-7 nice -n 10 tools/iaus_sim/run.sh`. Expect `CLI-CHECKS PASS`. Add CLI checks for: the env-hold line of section 1, `SPELLALTMISS` appears only with `--regret-alt`, the loot score of the shipped table equals its uncharged value (`--loot-score` first SEED line equals the report's value on a guarded set), `LOOTSEED` lines exist, summons and hazards switches named in the headline and off = v3-equal as the existing "every v4 switch off by name" check does.
8. Driver smoke (section 1 b) on the final binary.
9. Docs: `iaus-sim-v4.md` new section 16 "Bundle A as built", `MAP.md` (the sim entry near line 9646: "to add X edit Y" for `kSummon`, `kHazard`, the loot rules, `held_out_check`, `LOOT_REJECT_ROWS`), one `Docs/STATUS.md` line, backlog.
10. Never run `run.sh` without `--no-json` for a compare. Any measurement over an hour: `systemd-run --user` unit with output under `/mnt/gaming/modlists/Projects/_tuning/`, and hand back.

Who reads what:
- Batch W (world): items 5 and 6 add `V4` switches and use `EnvWorld.h` placement. They must not touch `FindPath` or the route cache (keyed centroid to centroid: a summon body changes occupancy, not routes).
- Batch L (loot): item 4 only. Summons do not loot and are not loot witnesses (check `Loot.h` witness and guard code iterates followers by `fs`, not by `Party()` bodies, so a summon cannot become a looter or lift a gate by the near rule).
- Batch C (choice): the regret report and the spells stage read the oracle. Items 5 and 6 change the oracle's world, so doc 15.3 and 15.4 tables are stale after this bundle: mark them "before bundle A", do not re-measure (the tune's `spells` stage prints its own).
- Cache keys: binary sha and list sha cover everything. No new key part is needed. `--sim-flags` is where a non-default switch would go.

## 6. What must NOT change

- `native/` (any file). If a shared function is needed there, STOP that item and report.
- The simulator's defaults for v2 and v3, the oracle's ceiling (128), the cuts' defaults (OFF in the simulator, ON in the driver).
- The `RULE` texts of driver stages that were "written before any result was read", except by ADDING the set check sentence (already in code comments. Add one line to `stage_follow` and `stage_spells` docstrings saying the set check exists).
- T3 to T5 code paths, `--parked` behaviour.
- No game-derived data in git. No per-list logic or names.

## 7. Tunables added by this bundle (all placeholders unless marked)

| Name | Where | Value | Kind |
|---|---|---|---|
| `HOLD_SET_SE` | `tune_driver.py` | 1.5 | PLACEHOLDER (review's number) |
| `HOLD_FINAL0` | `tune_driver.py` | 7601 | seed range |
| `LOOT_ROW_SE` | `tune_driver.py` | 2.0 | PLACEHOLDER |
| `LOOT_REJECT_ROWS` | `tune_driver.py` | 4 rows | ruling by review |
| `loot_gate_seeds` | `FULL` / `SMOKE` | 16 / 2 | cost |
| `Rules::passMaxGapSec`, `passSlackU` | `Loot.h` | 3.0, 64 | MIRROR of native, test-pinned |
| `Rules::native1124`, `gateIdleLift` | `Loot.h` | 1, 0 | older-build switches |
| `kSummon.*` | `World.h` | section 3 | MODEL |
| `kHazard.*` | `World.h` | section 4 | MODEL |

## 8. The riskiest points

1. **Record layouts for hazards and the rune road.** HAZD, IPDS, IPCT offsets here are from memory, not from a dump. A wrong offset gives plausible garbage. Dump first, and if runes do not go through a hazard record, leave them unmeasured and say so.
2. **Summons changing random-draw order.** The off switch must keep the old block byte for byte in behaviour, or v2 and v3 baselines and the three pins move and the "features off = 0 changed" proof is lost.
3. **The loot scenarios that pin old faults.** Native fixed N2, N3, N4 and removed the idle lift. Re-pointing `freeze-1117g`, `locked-door-cycle` and `carry-full` to an older-build rule while the default must now pass is where a mirror can quietly be made to "pass" by weakening the scenario. The old arm must still show the old numbers.
4. The set check on held-out uses `gap_nat` fights per seed by `follow_seeds` seeds: 24 fights a cell at FULL. Its power is modest. If the first round shows it never fires or always fires, the number to move is `HOLD_SET_SE`, in the open.
5. `ScoreOver` doubles the loot stage's episode cost for every non-shipped table. It is milliseconds per episode, but check the loot stage's wall time in the smoke.
