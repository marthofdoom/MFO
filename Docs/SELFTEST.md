# The dev-build self-test gambit (self-driving)

ClickUp 1dfcuabcqr7. marth 2026-11-09:

> "for dev builds just have a gambit that runs the tests and once a test is complete its not run by that
> gambit so on dev builds i just leave the gambit on and it tests each test when its possible"
>
> "If the follower doesnt have the equipment the gambit spawns it for the test then remove only what it ADDED"
>
> "the test gambit should also log which tests are still pending"

marth 2026-11-10 (v2, this document):

> "I can boot into lorerim and use the debug gambit on someone. It needs to be able to test everything on its own."

## 0. ONE STEP (marth)

1. **Put the dev DLL in.** Download the `MFO-dll-DEV-SELFTEST` artifact of the CI run you want (`MFO.dll`, `MFO.pdb`).
   * **LoreRim on this PC:** replace `MFO.dll` (and `MFO.pdb`) in the MFO mod folder of the LoreRim profile (MO2 `mods/MFO`), keeping the release DLL aside to go back.
   * **The Deck:** same two files into the MFO mod folder of the instance you play (memory `deploy-workflow`: Tuxbornrc1 is a direct `scp`, custom-modlist is syncthing). Quit the game fully first (SKSE loads the DLL at process start).
2. **Make sure the test inputs exist** (nothing else to set): the class add-on `MFO_Classes.esl` enabled with `bClassTree = 1` (the class tests), Harbinger (APMF) installed (the claim-road and H5 tests), `bAimBudget = 1` (default). A test whose input is missing says so in the log and waits; the others still run.
3. **Add ONE rule to ONE follower:** open the board on him, **Combat** page, add a rule, set **DO** to **"DEV: run self-tests"**, keep it enabled. The WHEN cell is not read.
4. **Play normally or just stand around.** Stand somewhere quiet and open (a field, a road): **not in a town, inn or house** (it refuses there), nobody essential or friendly nearby. Each pending test sets itself up, runs, judges, cleans up and logs. Do not open menus for long (a paused game freezes the run).
5. **Read the result:** `grep '\[selftest\]' MFO.log`. The last line when everything is judged is
   `[selftest] ALL DONE: PASS n, FAIL n, MANUAL n (ids), INFO n`. If some test is waiting on an input the runner cannot supply (no class add-on, no second ally, no Harbinger), the same line is printed as `ALL RUNNABLE DONE: ... | BLOCKED n: <id> (needs ...)` once everything else is judged.
   The log also lists, after each load and after each completion, which tests are still pending and why, and one `MANUAL` line per thing a human has to look at, with the exact thing to look at.

To run them again: `Data/SKSE/Plugins/MFO_SelfTest.ini`, section 7.

## 1. Dev builds only

* CMake option `MFO_DEV_SELFTEST` (default **OFF**). ON compiles `native/selftest/*.cpp` in and defines `MFO_DEV_SELFTEST`. OFF compiles none of those files; every `SelfTest::` call in shared code is an empty inline (`SelfTest.h`), and the opcode, the board entry, the `'MFST'` record id and every dev-only accessor listed in section 11 sit inside `#ifdef MFO_DEV_SELFTEST`. A release DLL holds no opcode, no string and no state of the mechanism.
* CI: `.github/workflows/native.yml` job **`build-dev`** builds it on every push and uploads it as **`MFO-dll-DEV-SELFTEST`** (MFO.dll, MFO.pdb, build-info.txt with `dev_selftest=true`). The shipped artifact `MFO-dll` is the normal job and never this DLL. The splitcheck proof dispatches skip the dev job.
* Local: `cmake --preset release -DMFO_DEV_SELFTEST=ON`.
* The dev DLL logs one `[selftest] DEV SELF-TEST BUILD ... Never ship this DLL.` warning on its first pump tick.

## 2. How a run goes

The rule is a STANDING MARKER, not an action: the Evaluator skips it, so it never takes a tick from the follower's real gambits. Several followers can carry it; each runs at most one test at a time and a test runs on one follower at a time.

Per follower, on the AddTask worker (`Scheduler::Tick` -> `SelfTest::Tick`):

| Phase | What happens | Log |
|---|---|---|
| pick | Once a second: the first test in registry order (highest value first) that is not complete, not running elsewhere, not backing off for this follower, and whose `precondition()` holds. No test starts while fixtures or class restore points from a load are still being put back. | `[selftest] START <id> (<ref>) on <follower> ...: setup asked: ...` |
| setup (main thread) | Items / spells the follower lacks are added; if the test wants a hostile: the SAFETY check, then the follower is hurt (if asked) and ONE weak hostile is spawned and set on him. Waits up to 10 s. A safety refusal is a RETRY (info level, 120 s backoff), never an error. | `[selftest] fixtures for <id> ...` |
| setup (worker) | Class snapshot, temp rows (front of the table), probe flags. Every step logs at info. | `[selftest] setup <fid>: TEMP ROW uid ...`, `... class snapshot taken`, `... probe ... ON in memory` |
| drive | `step()` every 0.5 s of unpaused run time until it returns a verdict, or the test's timeout fires (`onTimeout()`, default FAIL). A spawn run counts as "in combat" for the first 10 s (the hostile is walking up); a dead hostile is replaced (bounded, default 3, H5 6). | |
| end | **PASS / FAIL / INFO**: complete, kept in the co-save, never run again. **RETRY**: the situation did not allow a judgement (hostile died, pool too low, safety refusal); stays pending, 30 s backoff on that follower. **ABORT**: the follower left / died / lost the rule, or setup failed (error level); stays pending. Every end TEARS DOWN first (rows, class state, probes), then removes the fixtures, the spawn and the damage. | `[selftest] PASS\|FAIL\|INFO\|RETRY\|ABORT <id> <follower> t=<s>: <evidence> \| setup: ...` then `teardown (<id>) ...` lines |
| pending | After every load (one line per test, plus one `MANUAL` line per human check) and after every completion (one summary line). When nothing is pending: the ALL DONE line. | `[selftest] pending (3 of 20, after load): ... \| complete 1: PASS 1, FAIL 0, INFO 0 \| 1 follower(s) hold ...` |

Never masked: a fixture that did not land, a removal that did not take, a row still in a table, a class state that did not restore, an unresolvable follower, all log at ERROR and say what was left.

A verdict is never derived from a timer alone. A window that ends without the observation is RETRY (or INFO with the standing numbers), not a pass.

## 3. What a test can ask for (the self-setup)

`fixtures(Ctx&, FixtureRequest&)` fills any of:

| Request | What the runner does | Put back by |
|---|---|---|
| `items` / `spells` | adds only what is missing, records it (section 5) | `PostFixtureCleanup`: `min(added, now - before)`; a taught spell deselected then removed |
| `rows` (`TempRow`) | inserts a gambit row at the FRONT of the follower's combat or logistics table (highest priority; the player's rows keep their order below it), uid from `NextRuleUID`, registered in `g_runRows` BEFORE it exists | `SetupTeardown`: erased by uid from every table; a row still present after is an ERROR |
| `cls.snapshot` | copies the follower's whole MCLS state (equipped, per-class xp and level, mastered, unlocked, stealth counters) into `g_classSnaps` (also written to `'MFST'`). The test then applies classes with `ApplyClass()` (the C5c writers `SetUnlocked / SetProgress / SetMastered / SetEquipped`, EXACT level) | `SetupTeardown`: the same writers write the snapshot back, then READ IT BACK and compare field by field; a difference is an ERROR and the restore point stays (retried every 30 s) |
| `spawn` | after the safety check, `PlaceObjectAtMe` of a Skyrim.esm base (default `EncMudcrabMedium` 000E4010, an NPC_ verified in the master) ~700 units in front of the follower, `StartCombat(hostile, follower)` through the same relocation row as `Probe.cpp` (`Probe.StartCombat`, seat-verified; if the runtime refuses, it fights only if it aggroes on its own). `hurtPct` also damages the follower's Health by that percent for the run | `PostFixtureCleanup`: `StopCombat`, `Disable`, `SetDelete(true)`, logged; the damage is given back (never more than the damage he still has) |
| `probes` | `kProbeAim` flips `Config::g_aimProbe` IN MEMORY (the INI is untouched) | `SetupTeardown` writes the previous value back |

**Spawn safety (refuses with RETRY, never an error):** only ONE spawn-using run at a time across all followers (a second waits); a test that hurts the follower is refused unless he is essential or protected; the player already in a fight; the player's location (innermost-first) is a lived-in place (`LocationTypes::Classify` kCivilised: city, town, inn, house, farm, shop, temple ...); any living essential or protected non-follower, or any living non-hostile non-teammate actor, within 3500 units. Checked on the main thread inside the same closure that would add anything, so a refusal changes nothing.

## 4. The registry

Pick order is the order below. **Setup** is what the runner puts in place and removes; **PASS / FAIL** are from observed state or the dev counters named in section 11.

| id | ref | setup (restored afterwards) | PASS | FAIL / INFO / RETRY |
|---|---|---|---|---|
| `b1.heal_left_offense_right` | MFO-B1 | Healing + Flames taught; rows: self HP < 95% -> cast Healing, any enemy -> cast Flames, any enemy -> attack; spawn, follower hurt 40% | a sample with the heal spell in the LEFT hand and the offense spell in the RIGHT at once | INFO "not applicable" when the right hand holds a one-hander (b1a covers it). FAIL at timeout after both halves seen but never together. RETRY otherwise |
| `costparity.fireforget` | 1dfcuabcr0c | Healing taught; row: self HP < 95% -> cast Healing; attack row; spawn, hurt 45% | the magicka drop equals the SUM of the player-curve cost P over the `CostParity::DevEvents` bills for the cast(s), within `2 + 10% + regen` | FAIL when the drop equals the ENGINE cost E and P differs by more than twice the tolerance. INFO when parity does not apply (every bill `applies = false`). RETRY: pool too low, or the drop matches neither. The evidence names the road(s): direct / animated-once |
| `costparity.concentration` | 1dfcuabcr0c | Fast Healing taught; same rows; spawn, hurt 45% | the drain rate over a >= 3 s open channel equals P per second (`max(1, 20%) + 1`/s tolerance) | FAIL when the rate equals E per second. INFO when parity does not apply. RETRY when the channel is shorter than 3 s |
| `b1a.weapon_stays_right` | MFO-B1a | Healing + Flames taught; the same rows; spawn, hurt 40%. Needs a one-hander in the right hand at the start | heal in the LEFT in >= 3 samples, the weapon stayed in the RIGHT, no spell ever in the right hand | FAIL when a spell takes the right hand while the heal holds the left. INFO "not applicable" with no one-hander. RETRY if the heal never held the left long enough |
| `buff.animated_claim_road` | bbf90fd | Oakflesh + Lesser Ward taught; rows: always -> cast Oakflesh, always -> cast Lesser Ward (the first becomes "buff already up", transparent, and the scan falls to the second); spawn | per spell: `DevBuffClaimCount` rose (a claim filed) AND (`DevBuffLandedCount` rose OR the effect is active on him); the predicted engine row is printed | FAIL at timeout if either did not complete. INFO when Harbinger is absent / below ABI 19 (the direct road is the documented degrade). The "caster built" half is the effect landing; `[ctcensus]` stays log-only |
| `class.c6_carriers` | C6 84678a5 | class snapshot; five scenarios applied by `ApplyClass`: Warrior L7; Warrior mastered + Knight L3; T4 Living Legend over mastered T3s; Mercenary L5; classes OFF | after each, `DevReadCarriers` (main thread) equals `CarrierRanks()` for every class that has a carrier faction, plus the named expectations (warrior 7; warrior 100 with Knight 3; two T3 parents at 20; Mercenary full ability present; OFF = all ranks 0 and the abilities removed); scenario 0 also checks all carrier perks on his base and every rank-1 entry-point entry live (when he has a process) | FAIL with the per-class diff after 8 s of retries |
| `class.scout_speed` | C6 reviewer SEV-3-field | class snapshot; a mastered Scout is applied (no loot involved) | `SpeedMult` >= 110% of its pre-equip value within 8 s | FAIL otherwise, with the numbers |
| `aim.h5_miss_budget` | H5 | class snapshot; Hunting Bow x1 + Iron Arrow x40 added; rows: foe -> equip ranged, foe -> attack; spawn (6 respawns); `kProbeAim`; the test applies Archer L5 | `APMFBridge::DevReadAim` (APMF `GetProjectileAimState`): >= 10 shots resolved, the claim governing (corrected or would-correct > 0), window misses <= budget | FAIL: window over budget; no claim 45 s after the equip; the seat not installed; 10+ shots with no correction. RETRY: fewer than 10 shots in the window |
| `class.ally_buff_strength` | marth Q58 | class snapshot; Courage taught; row: always -> cast Courage at the nearest ally; spawn. Needs a SECOND ally in the party | **INFO**: the Courage effect magnitude on the ally, each read from a fresh cast (elapsed < 4 s), with no class, mastered Warlock, mastered Nightblade; the evidence says whether they differ (the reviewer's rule: the bonus must not apply to an ally buff) | RETRY if a phase cannot be set |
| `skooma.not_drunk_by_ai` | 86e3t1gfq | Skooma x1 (+ 2 minor healing potions if he has no health potion), Flames taught; rows: enemy -> cast Flames, enemy -> attack; spawn, hurt 60% | Skooma count never fell, with drink pressure after >= 20 s of combat (a pool SKOOMA'S RECORD restores under 50%, or a real health potion drunk) | FAIL: the Skooma count fell. RETRY: no pressure |
| `bound_bow.fires` | 86e3t1gfx | Bound Bow (000211ED) taught; rows: enemy -> cast Bound Bow (self), enemy -> attack; spawn; the passive hit sink | a bound bow in a hand, then an ARROW hit by a bound bow from this follower (TESHitEvent sink; bash never counts) | FAIL: the rule fired and no bound bow reached a hand within 15 s. RETRY: no hit in 30 s of bow |
| `hms.cap_holds` | 86e3d6dnv | none (out of combat; a captured HMS record, 2 minutes in the party) | held total <= the cap's player sample + 0.5 after two reads 2 s apart agree, every pool's live base within 0.5 of its held value; or every pool at its floor | FAIL: live base off the held value, or above the sample with a pool above its floor |
| `hms.above_parity_gate` | 86e3jw3b4 | none (same record) | a player level-up happened in the window and the held total stayed <= the new sample + 0.5 (or every pool at its floor) | FAIL: above it with a pool above its floor. **INFO "NOT EXERCISED"** at the end of the window if no level-up happened (the DLL cannot cause one), with the standing numbers and whether the gate is armed |
| `prog.perk_entries_unique` | 1dfcuabcrqq | none | no entry-point entry live more than once on two reads 2 s apart | FAIL: a duplicate, naming the worst perk |
| `census.shape_ran` | bbf90fd | none | the shape census reached its totals line at load (`DevShapeCensus`: castable total > 0, buckets > 0) | INFO when `bSpellShapeCensus = 0`; FAIL if it is on and never finished |
| `census.hidden_perk_ran` | 92604e0 | none | **INFO** with the counts (summaries, treePerksHeld of tree size, held-in-no-tree) once the census summary is logged: BRING THE LIST TO MARTH before card C2 | FAIL when `treePerksHeld = 0` (an invalid census); RETRY when no summary in 90 s |

### MANUAL (listed, counted in ALL DONE, never run)

Each is logged after every load as `[selftest]   MANUAL <id> (<ref>): the DLL cannot judge it (<why>). LOOK AT: <the exact thing>`. The table is `ManualItems()` in `Tests.cpp`:

`retreat.flee_hold` (an engine Flee cannot be provoked without forcing the AI), `visual.animated_casts`, `visual.frame_hitches`, `ch18.power_floor`, `taunt.pull`, `substrate.ooc_timeouts`, `probe.m0_h3_g11_g7_h5miss`, `log.inactive_winner`, `iaus.scroll_probe`, `iaus.exploit_rows`, `g10.will_work`, `auto.control_target`, `g9.cooldown`, `tailor.compat`, `magicka.cost_capture`, `freeze.batch_a`, `play.batch8`. Several of these (`g10.will_work`, `auto.control_target`) could be automated with a dev counter on the board outcome; they are listed with that reason rather than faked.

## 5. Fixtures (`Fixtures.cpp`)

* **Add only what is missing.** An item is topped up to the test's `atLeast` (his own copies count). A spell is taught only if he does not know it.
* **Record exactly what was added** in the fixture ledger: follower, item or spell, form, count added, his own count before. The engine call and the ledger entry happen in the SAME `MainThread::Post` closure, and the save callback also runs on the main thread, so a save never sees one without the other.
* **Remove only what was added.** An item comes off as `min(added, now - before)`: never below what he had before the test. A taught spell is deselected from a hand that holds it (`Actor::DeselectSpell`), then removed.
* **Equip:** the runner never equips anything itself; the gambits under test do, through MFO's own roads, which own their release. The only equip state it can leave behind is a taught spell still selected in a hand, which the cleanup deselects.
* **Spawn / hurt** entries ride the same ledger (kinds `spawn`, `hurt`).
* A follower that does not resolve at cleanup keeps its entry (error line, retried every 30 s).

## 6. Mid-run saves (the safety design)

A save can happen at any time (autosave, quicksave). Nothing a run changes may leak into it permanently:

| Change | What a mid-run save carries | What repairs it |
|---|---|---|
| **temp rows** | NOTHING: the FLWR writer (`Serialization.cpp`, `#ifdef MFO_DEV_SELFTEST`) skips every row whose uid is registered in `g_runRows`; the row count written is the count of rows written; the layout and version are unchanged. The registration happens before the row exists, and the save runs on the main thread with the pump paused | not needed |
| **class state (MCLS)** | the TEMPORARY class IS in MCLS (written as it stands), but the restore point is in `'MFST'` v2 | the first tick after the next load restores it through the writers (`RestoreOrphanClassSnaps`, retried every 30 s while it fails) and no test starts until it is applied |
| **fixtures / spawn / damage** | the ledger in `'MFST'` | the first tick after the load removes them (the spawn's raw runtime ref id is looked up and deleted if it still resolves; "already gone" is logged at info) |
| **probe flags** | memory only, never saved | gone with the process |

A **load or revert** during a run: `ClearAll` forgets every run (no engine call, the world is being replaced); the loaded save's own `'MFST'` supplies the ledger and class restore point. A **dismissal / death / losing the rule**: the next tick ABORTs the run and tears it down. A **crash** saved nothing newer than the last save, whose ledger matches its world.

## 7. Re-running tests

`Data/SKSE/Plugins/MFO_SelfTest.ini` (dev builds only; read on the first tick after each load):

```ini
[SelfTest]
; Raise above the value the save holds to clear results ONCE (the applied value is saved in 'MFST').
iResetGeneration=1
; Optional: clear only these ids (comma list). Empty = every result.
sResetOnly=
```

Log: `[selftest] ...MFO_SelfTest.ini: reset generation 0 -> 1: cleared N result(s) (all|sResetOnly)`. A value at or below the saved one does nothing, so the file can stay in place.

## 8. Adding a test

1. Pick a FROZEN id (`area.what_is_checked`), it becomes the `'MFST'` key.
2. In `native/selftest/TestsDrive.cpp` (or `Tests.cpp`), write the functions:
   * `const char* Pre(Ctx& c)`: `nullptr` when the test can start now, else a short static text of what is missing. Stash what you found in `c.s->set[]` / `c.s->n[]`.
   * `void Fix(Ctx& c, FixtureRequest& r)`: the self-setup (section 3): items, spells, `r.rows.push_back(Row(...))`, `r.cls.snapshot`, `r.spawn`, `r.probes`.
   * `Step St(Ctx& c)`: every 0.5 s. `Step::Continue()`, `Pass(e)`, `Fail(e)`, `Info(e)` (a measurement, complete) or `Retry(why)`. `c.hostile` is the live spawned hostile; `c.inCombat` includes the 10 s grace. A test may call the class writers through `ApplyClass()` after its `cls.snapshot`.
   * optional `Step To(Ctx& c)`: the verdict at timeout (default FAIL).
3. Add ONE `Register({ id, ref, needs, timeoutSec, Pre, Fix, St, To });` line. Registry order is pick order.
4. Evidence must let a reader judge without the code: names, counts, times. Never judge from a timer alone.
5. Need a number the DLL does not expose? Add the smallest dev-only read accessor inside `#ifdef MFO_DEV_SELFTEST` in the owning subsystem (section 11), or STOP if it would touch release logic.
6. Add the row to section 4 and to MAP.md section 10.

## 9. Known limits

* The WHEN cell of the marker rule is not read.
* Results are per SAVE (the co-save), not per machine: a new game starts with every test pending.
* A spawned hostile that never aggroes (the relocation row refused) fights only if it notices; the test then RETRYs.
* The mudcrab dies quickly to a strong follower; the runner respawns it (bounded) and the tests RETRY when the fight ends first.
* `class.ally_buff_strength` needs a second ally; with a lone follower it stays pending with that reason.
* A spell learned for real DURING the run is indistinguishable from the fixture and is removed with it (REVIEW-BACKLOG MFO-B564).
* `bound_bow.fires` registers a passive TESHitEvent sink once; a missed shot makes no hit, so that test never FAILs on "no shot".
* The class restore reads the state back through `Classes::GetFollower`; the per-class stored gambit TABLES (card C12) are not part of the snapshot because no run touches them (C12 is not wired).
* Release compatibility of a dev save: section 10.

## 10. Persistence: `'MFST'` v2 and release compatibility

Layout (`selftest/Record.cpp` header; `Serialization.h` `kRecSelfTest` / `kSelfTestVersion = 2` inside the `#ifdef`): `u32 resetGeneration; u32 resultCount (<=256) {str id, str verdict "pass"|"fail"|"info", u32 follower, lstr evidence (<=1024)}; u32 fixtureCount (<=1024) {u32 follower, str kind "item"|"spell"|"spawn"|"hurt", u32 form, i32 added, i32 before, str testId}`; v2 appends `u32 classSnapCount (<=64) {u32 follower, u8 hadRecord, u8 equipped, u8 n, n x {u32 xp, u8 level}, u64 mastered, u64 unlocked, u16 thief, u16 masterThief}`. A `spawn` fixture's form is the spawned ref's runtime id, kept RAW (no `ResolveFormID`); a `hurt` fixture's `added` is the damage x10. Strings, never ordinals (#10); bounded counts, a short read aborts this record only (#11); every follower and (non-spawn) form through `ResolveFormID`, an unresolvable entry DROPPED with an error line. v1 saves still load (the class block is read only for version >= 2). Written only by a dev build.

**Why a dev save loads cleanly on a release build** (read from the reader code):

1. *The rule.* The FLWR reader stores the action opcode string verbatim with no vocabulary check; only an UNRESOLVABLE `actionParamForm` disables a rule. In the release scan `Actuation::Fire`'s final branch returns a TRANSPARENT `FailedOther` ("unknown action") and the combat scan falls past it. Temp rows are never in the save at all (section 6).
2. *The record.* A release LoadCallback has no `kRecSelfTest` branch: `'MFST'` is skipped as an unknown record type, and no byte of any other record moves. The release DLL's next save drops `'MFST'` (#12).
3. *What is lost going back to release:* the results, the fixture ledger and the class restore point. A dev save made MID-RUN and played on a release DLL keeps the fixture items AND the temporary class (nothing knows to remove them). Play dev saves on dev builds, or let the run finish (or remove the rule) before saving for a release session.

## 11. The dev-only accessors (all inside `#ifdef MFO_DEV_SELFTEST`; a release DLL has none)

| Where | What | Used by |
|---|---|---|
| `cast/CostParity.h/.cpp` | `DevEvents()` (the last 64 billing events: road, applies, P, E, spent) | `costparity.*` |
| `cast/Actuation.h`, `cast/BuffRoad.cpp` | `DevBuffClaimCount / DevBuffLandedCount(spell)` | `buff.animated_claim_road` |
| `cast/Actuation.h`, `cast/Archetype.cpp` | `DevShapeCensus()` | `census.shape_ran` |
| `classes/Classes.h`, `classes/Carriers.cpp` | `DevReadCarriers(fid)` (main thread: faction ranks, ability spells, carrier perks on the base, live entries) | `class.c6_carriers`, `class.scout_speed` |
| `apmf/APMFBridge.h`, `apmf/AimClaims.cpp` | `DevReadAim(fid)` (one `GetProjectileAimState`) | `aim.h5_miss_budget` |
| `progression/ProgAllocator.h`, `progression/Inherit.cpp` | `ReadInheritCensusStats()` (atomics set where the census logs its summary) | `census.hidden_perk_ran` |
| `progression/ProgAllocator.h` (existing) | `ReadHmsCapView`, `ReadPerkEntryView` | `hms.*`, `prog.perk_entries_unique` |
| `Serialization.cpp` | the FLWR writer skips run rows (`SelfTest::HasRunRows / IsRunRow`) | section 6 |

## 12. Dev class equip (card C5c)

The same dev DLL can put a follower into a class by hand: `[Debug] sDevClassEquip` in `MFO.ini` (format, example and warnings in `Docs/INI-REFERENCE.md`). Entries are `<follower>:<class slug>:<level>[:mastered]`, comma separated, follower = `Plugin.esp|HEXLOCAL` or a full hex FormID of the actor ref. It is NOT part of the self-driving flow (the runner snapshots and restores class state by itself) and it WRITES MCLS permanently, so do not set it while running the self-tests.

* Runs on the pump worker from `SelfTest::Tick` (`Runner.cpp`, `DevClassEquipTick`), once per entry per game load, as soon as the follower is a live managed follower and `Classes::Active()`. `ClearAll` (every load) re-arms it.
* It unlocks with `SetUnlocked`, equips through `Classes::EquipClass` (card C12, the road the board card C10 will use, so the class's default or remembered gambit tables are laid down too and `[classes] ... -> <class> -- combat table ...` lines follow), and levels with `ApplyLevel`. Several entries for ONE follower apply in order in the same tick, which walks a class switch (for example `Lydia|...:knight:1,Lydia|...:warrior:1,Lydia|...:knight:1`). Log lines (all WARN): `[classes] DEV equip ...`.
* A release DLL has no `sDevClassEquip`, no `DevClassEquipTick` and no `[classes] DEV equip` string.
