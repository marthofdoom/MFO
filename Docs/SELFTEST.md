# The dev-build self-test gambit

ClickUp 1dfcuabcqr7. marth 2026-11-09:

> "for dev builds just have a gambit that runs the tests and once a test is complete its not run by that
> gambit so on dev builds i just leave the gambit on and it tests each test when its possible"
>
> "If the follower doesnt have the equipment the gambit spawns it for the test then remove only what it ADDED"
>
> "the test gambit should also log which tests are still pending"

Code: `native/selftest/` (public header `SelfTest.h`, internal header `SelfTest_internal.h`, `Runner.cpp`,
`Fixtures.cpp`, `Record.cpp`, `Tests.cpp`). Architecture notes and what breaks: MAP.md section 10 "selftest/".

## 1. Dev builds only

* CMake option `MFO_DEV_SELFTEST` (default **OFF**). ON compiles `native/selftest/*.cpp` in and defines
  `MFO_DEV_SELFTEST`. OFF compiles none of those files; every `SelfTest::` call in shared code is an empty
  inline (`SelfTest.h`), and the opcode, the board entry and the `'MFST'` record id sit inside
  `#ifdef MFO_DEV_SELFTEST`. A release DLL holds no opcode, no string and no state of the mechanism.
* CI: `.github/workflows/native.yml` job **`build-dev`** builds it on every push (so the `#ifdef`'d code never
  rots unbuilt) and on a plain workflow dispatch, and uploads it as **`MFO-dll-DEV-SELFTEST`** (MFO.dll, MFO.pdb,
  build-info.txt with `dev_selftest=true`). The shipped artifact `MFO-dll` is the normal job and never this DLL.
  The splitcheck proof dispatches (`noinline` / `noopt`) skip the dev job.
* Local: `cmake --preset release -DMFO_DEV_SELFTEST=ON`.
* The dev DLL logs one `[selftest] DEV SELF-TEST BUILD ... Never ship this DLL.` warning on its first pump tick.

## 2. Turning it on (the Deck)

1. Download the `MFO-dll-DEV-SELFTEST` artifact of the run you want and put its `MFO.dll` (+ `MFO.pdb`) where
   the normal MFO.dll goes (the deploy path in memory `deploy-workflow`). Keep the release DLL aside to go back.
2. In game, open the board on a follower, **Combat** page, add a rule, set **DO** to **"DEV: run self-tests"**
   (last entry of the combat action list). The WHEN cell is not read; leave it on anything. Keep the rule
   enabled. It needs a free combat slot.
3. Give the follower the gambits a test watches (the pending log says what each test needs, section 4).
   Then just play. Each pending test runs when its situation comes up, once, and is never run again.
4. Read `MFO.log`: `grep '\[selftest\]'`.

The rule is a STANDING MARKER, not an action: the Evaluator skips it, so it never takes a tick from the
follower's real gambits (which are exactly what most tests watch). Several followers can carry it; each runs
at most one test at a time and a test runs on one follower at a time.

## 3. What a run does

Per follower, on the AddTask worker (`Scheduler::Tick` -> `SelfTest::Tick`):

| Phase | What happens | Log |
|---|---|---|
| pick | Once a second: the first test in registry order that is not complete, not running elsewhere, not backing off for this follower, and whose `precondition()` holds. No test starts while fixtures from a load are still being removed. | `[selftest] START <id> (<ref>) on <follower> ...` |
| fixtures | The items / spells the test asked for that the follower LACKS are added on the main thread (section 5). The run waits up to 10 s for that. | `[selftest] fixtures for <id> on <follower>: Skooma x1 (had 0), ...` |
| drive | `step()` every 0.5 s of unpaused run time until it returns a verdict, or the test's timeout fires (`onTimeout()`, default FAIL). Paused game = frozen run time. | |
| end | **PASS / FAIL**: complete, kept in the co-save, never run again. **RETRY**: the situation ended before the test could judge (combat ended, nobody needed healing); stays pending, 30 s backoff on that follower. **ABORT**: the follower left / died / lost the rule, or the fixtures failed (error level); stays pending. Every end removes the run's fixtures. | `[selftest] PASS\|FAIL\|RETRY\|ABORT <id> <follower> t=<s>: <evidence> \| fixtures: ...` |
| pending | After every load (one line per test) and after every completion (one summary line). | `[selftest] pending (3 of 4, after load): a, b, c \| complete 1: PASS 1, FAIL 0 \| 1 follower(s) hold the 'DEV: run self-tests' rule` then `[selftest]   pending <id> (<ref>): needs <when it can run>` and `[selftest]   done <id>: PASS on <follower> -- <evidence>` |

Never masked: a fixture that did not land, a removal that did not take, an unresolvable follower, all log
at error level and say what was left.

## 4. The seeded tests

From the IN-GAME TEST LIST (ClickUp 1dfcuabcqdm). Each rides the follower's own board: it starts only when his
combat table already holds the gambits it watches.

| id | ref | needs | PASS | FAIL | RETRY | timeout |
|---|---|---|---|---|---|---|
| `b1.heal_left_offense_right` | MFO-B1 | in combat; enabled combat rules casting a Health heal (`Actuation::HealsHealth`) AND an offensive spell (`ClassifyArchetype` kind Offense) | a sample with a heal spell in the LEFT hand and an offense spell in the RIGHT at once | timeout after BOTH halves (heal left, offense right) were each seen but never together | combat ended first; or timeout with either half never seen | 120 s |
| `skooma.not_drunk_by_ai` | 86e3t1gfq | in combat; an enabled combat cast rule. Fixtures: Skooma x1 (Skyrim.esm `00057A7A`), and 2 Potions of Minor Healing (`0003EADD`) if he carries no health potion | Skooma count never fell, with drink pressure after >= 20 s of combat: a pool SKOOMA'S OWN RECORD restores (its beneficial Value Modifier effects on Health / Magicka / Stamina, read at precondition time; all three if it declares none) under 50% at some point, or a real health potion drunk when Health is one of them | the Skooma count fell during combat | combat ended without that pressure | 180 s |
| `bound_bow.fires` | 86e3t1gfx | in combat; an enabled combat rule casting a bound-BOW spell (shape BoundWeapon, an effect's associated weapon is a bow); the self-test hit sink live | a bound bow in a hand, then an ARROW hit by a bound bow from this follower (a passive TESHitEvent sink in `Tests.cpp`: cause = the follower, source = a bound bow, `projectile != 0` and `kBashAttack` not set, so a bow bash never counts). The equipped-ammo count is not used (an NPC's bound-arrow decrement is unobserved, and a dismiss removes the arrows too) | the rule FIRED (`Gambit::lastFiredAt`) and no bound bow reached a hand within 15 s | bow in hand 30 s with no hit (a miss, out of range, no LoS, melee range); combat ended first; the rule never fired; the window ended | 90 s |
| `hms.cap_holds` | 86e3d6dnv | out of combat; `bHmsRedistribute` and `bHmsCapToPlayerTotal` on; a CAPTURED HMS record with a class profile and a player sample (`ProgAllocator::ReadHmsCapView`, dev-only, read on the main thread); the follower in the party 2 minutes this session | the held total (hmsBaseline + hmsCumulative) <= the cap's player sample `g_playerHmsTotalLast` (taken at the last level-up) + 0.5, after two reads 2 s apart agree (held, sample and the live base AVs all unchanged), with every pool's LIVE base within 0.5 of its held value; or above the sample with EVERY pool at its floor `max(class share x sample, 50)` (the cap's documented limit) | a pool's live base more than 0.5 off its held value after the settle (the engine does not carry what MFO holds); above the sample with a pool above its floor | combat started; the record gone / uncaptured; a live base unreadable (<= 0); no settled read in the window | 60 s |
| `prog.perk_entries_unique` | 1dfcuabcrqq | a follower ENROLLED in Progression with at least one MFO-granted rank and a middle-high process (loaded nearby), in the party 30 s this session (the session reapply ran). Read by `ProgAllocator::ReadPerkEntryView` (dev-only, main thread): every entry-point entry each base perk says is live (entry rank+1 == the base rank), counted in his live arrays | no such entry in its array more than once, on two reads 2 s apart that agree | any of them in its array more than once (MFO, or another plugin calling `Actor::ApplyPerksFromBase`); the evidence names the worst perk, its entry point and the copy count | the record gone; the follower unloaded (no process); no settled read in the window | 60 s |

Skipped on purpose (need engine work MFO does not have yet): ch.18 power deny / floor (no ch.18 ABI in MFO),
Script-archetype animated casts, substrate OOC timeouts, taunt, the class M0 / H3 / H5 / G7 / G11 probes,
retreat flee-origin, the inactive-winner trace, magicka cost capture, the Batch A freeze capture.

## 5. Fixtures (`Fixtures.cpp`)

* **Add only what is missing.** An item is topped up to the test's `atLeast` (his own copies count). A spell
  is taught only if he does not know it.
* **Record exactly what was added** in the fixture ledger: follower, item or spell, form, count added, his own
  count before. The engine call and the ledger entry happen in the SAME `MainThread::Post` closure, and the
  save callback also runs on the main thread, so a save never sees one without the other.
* **Remove only what was added.** An item comes off as `min(added, now - before)`: never below what he had
  before the test (what the test used up is gone already and is not taken from his stock). A taught spell is
  deselected from a hand that holds it (`Actor::DeselectSpell`, as Papyrus UnequipSpell does), then removed.
* **Equip:** the runner never equips anything itself; the gambits under test do, through MFO's own roads,
  which own their release. The only equip state it can leave behind is a taught spell still selected in a
  hand, which the cleanup deselects.
* **Reload / crash mid-test:** the ledger is in the co-save. A save taken mid-test carries it; on the next
  load those entries are orphans and the first tick removes them before any test may start (`[selftest]
  removing fixture(s) a test added before this save was made`). A crash saved nothing newer than the last
  save, whose ledger matches its world. A follower that does not resolve at cleanup keeps its entry (error
  line, retried every 30 s).

## 6. Persistence: `'MFST'` v1 and release compatibility

Layout (`selftest/Record.cpp` header, `Serialization.h` `kRecSelfTest` / `kSelfTestVersion` inside the
`#ifdef`): `u32 resetGeneration; u32 resultCount (<=256) {str id, str verdict "pass"|"fail", u32 follower,
lstr evidence (<=1024)}; u32 fixtureCount (<=1024) {u32 follower, str kind "item"|"spell", u32 form, i32 added,
i32 before, str testId}`. Strings, never ordinals (#10); bounded counts, short read aborts this record only
(#11); every FormID through `ResolveFormID`, an unresolvable fixture DROPPED with an error line (#8, #13).
Written only by a dev build.

**Why a dev save loads cleanly on a release build** (read from the reader code):

1. *The rule.* The FLWR reader stores the action opcode string verbatim, with no vocabulary check
   (`Serialization.cpp` `GambitWire` `ReadRowT`, `ReadOpT(a_in, g.actionOpcode)`, which `ReadFollowersRecord` calls for every row since class card C3; the MCLS stored tables use the same reader); only an UNRESOLVABLE
   `actionParamForm` disables a rule. A marker made fresh has param 0; one converted from a cast rule keeps
   that rule's spell (resolvable or not, either way harmless: resolvable keeps it as is, unresolvable disables
   the rule). The writer writes the string back verbatim,
   so the rule survives a release session. In the release scan the rule can match; `Actuation::Fire`'s final
   branch returns `{ FailedOther, "unknown action 'act.dev_selftest'", transparent = true }` (`cast/Fire.cpp`),
   and the combat scan falls past every transparent outcome (`Scheduler.cpp`, `if (outcome.transparent) { ...
   start = choice.ruleIndex + 1; continue; }`), so it never walls the rules below. The board shows the raw
   opcode as its label (`labelFor` falls back to the opcode string, `Board_internal.h`). The marker is offered
   on the COMBAT table only, whose fall-through is the proven one. On a release build the rule's WHEN cell IS
   evaluated (only a dev build skips the marker), so when it holds the rule takes that harmless transparent
   fall-through each scan, and the rule keeps using a combat slot (REVIEW-BACKLOG MFO-B565).
2. *The record.* A release LoadCallback has no `kRecSelfTest` branch: `'MFST'` reaches `if (type !=
   kRecFollowers) { warn "unknown record type ... skipped"; continue; }` and `GetNextRecordInfo` seeks past it.
   No byte of any other record moves. The release DLL's next save drops `'MFST'` (#12).
3. *What is lost going back to release:* the results and the fixture ledger. A save made MID-TEST and then
   played on a release DLL keeps the fixture items in the follower's inventory (nothing knows to remove them).
   Play dev saves on dev builds, or let a test finish (or remove the rule) before saving for a release session.

## 7. Re-running tests

`Data/SKSE/Plugins/MFO_SelfTest.ini` (dev builds only; read on the first tick after each load):

```ini
[SelfTest]
; Raise above the value the save holds to clear results ONCE (the applied value is saved in 'MFST').
iResetGeneration=1
; Optional: clear only these ids (comma list). Empty = every result.
sResetOnly=
```

Log: `[selftest] ...MFO_SelfTest.ini: reset generation 0 -> 1: cleared N result(s) (all|sResetOnly)`. A value
at or below the saved one does nothing, so the file can stay in place.

## 8. Adding a test

1. Pick a FROZEN id (`area.what_is_checked`), it becomes the `'MFST'` key.
2. In `native/selftest/Tests.cpp`, write the functions (all pure reads, #23; they run on the worker):
   * `const char* Pre(Ctx& c)`: return `nullptr` when the test can start now, else a short static text of
     what is missing. Stash what you found (spells, forms) in `c.s->set[]` / `c.s->n[]`, which the run keeps.
   * optional `void Fix(Ctx& c, FixtureRequest& r)`: push `r.items.push_back({form, atLeast})` /
     `r.spells.push_back(spell)`. Only the missing part is added; the runner removes it afterwards.
   * `Step St(Ctx& c)`: called every 0.5 s. Return `Step::Continue()`, `Step::Pass(evidence)`,
     `Step::Fail(evidence)` or `Step::Retry(why)` (situation ended before a verdict). Put the running summary
     in `c.s->note`: the default timeout FAIL quotes it.
   * optional `Step To(Ctx& c)`: the verdict at timeout (default FAIL "timeout after N s: <note>").
3. Add ONE `Register({ id, ref, needs, timeoutSec, Pre, Fix, St, To });` line to `RegisterAll()`. Registry order
   is pick order.
4. Evidence must let a reader judge without the code: names, counts, times.
5. Only read what MFO already exposes (or add a minimal read in its own subsystem, in its own brief). Engine
   actions never happen inside a test: a test that needs to make something happen does it through the
   follower's own gambits (its precondition requires them).
6. Add the row to section 4 and to MAP.md section 10 "selftest/".

## 9. Known limits

* The WHEN cell of the marker rule is not read.
* Results are per SAVE (the co-save), not per machine: a new game starts with every test pending.
* `skooma.not_drunk_by_ai` and `bound_bow.fires` walk the follower's inventory every 0.5 s during their run
  (dev build only; the logistics tick does the same walk at 1 s).
* A spell learned for real DURING the run (the player teaches it, a book) is indistinguishable from the
  fixture and is removed with it (REVIEW-BACKLOG MFO-B564).
* `bound_bow.fires` registers a passive TESHitEvent sink once (posted to the main thread on the first tick,
  retried every 5 s until live); it stores the last 64 hits whose cause is a tracked follower. A missed shot
  makes no hit, so that test never FAILs on "no shot".
* `hms.cap_holds` posts one `ReadHmsCapView` to the main thread per second while it is pending or running.
* `prog.perk_entries_unique` posts one `ReadPerkEntryView` to the main thread per second while it is pending or running (a walk of the follower's base perks and one entry-point array per entry).
