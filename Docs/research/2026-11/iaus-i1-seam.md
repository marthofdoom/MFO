# IAUS card I1: the ChoiceSource seam and Choice.source/rank (2026-11-10)

Branch `feat/iaus-i1-seam`, base `9372a6e` (main). Card row: `iaus-design.md` section 9, I1. Design: sections 1.1 and 1.3.

## What changed

- `Eval::Choice` (`native/Evaluator.h`): `int ruleIndex` became `int rank`, and a new `Eval::Source source` field was
  added (`Table = 0`, `Iaus = 1`, default `Table`). `Choice` is the evaluator's per-tick output. It is never serialized,
  so this is not a co-save or ABI change. The evaluator writes `out.rank = i` (`Evaluator.cpp:982`), the same int.
- `native/scheduler/Rank.h` (new, PURE): the rank arithmetic the scan uses (`IsChoice`, `NextTableRow`, `TableStop`,
  `WindowActive`, `StopsAtAnchor`, `OfTableRow`), plus `kMaxTableRow = 254` and `kIausRankFloor = 1000`, with a
  `static_assert` against `Iaus::kRankBase` (`native/iaus/Core.h:327`).
- `native/scheduler/ChoiceSource.h` (new): the interface. Design 1.1 placed it in `native/iaus/Iaus.h`. It lives with
  its one caller in the scheduler instead, so the scan does not depend on `iaus/`. I6 can include it from there.
- `native/scheduler/CombatScan.cpp`: `TableSource` (the table behind the interface), `ScanOut` and `Scan()` (the walk
  loop, now a function of its own, which is the extraction the I0 note left to I1). `Tick` builds a `TableSource`,
  calls `Scan`, and makes every later row read through `src`. The retreat self-heal walk uses a `TableSource` by name.
- `native/cast/Fire.cpp`, `native/logistics/Service.cpp`, `LootScan.cpp`, `Upkeep.cpp`: `.ruleIndex` -> `.rank`, a pure
  rename (proof 2 below).
- Not changed: `cast/`'s `g_firingRule` (`cast/Actuation_internal.h:220`) already carries the rank: `Fire.cpp:216` sets
  it from the choice, and every hand-lock / heal-hold / shield / forced-hold compare in `cast/` reads that int. It is
  the same int as before, so none of those files changed. `Recent::firedRule` / `failRule` (`Scheduler_internal.h`) store
  the rank and kept their names. No co-save, no Serialization, no APMF bridge, no `iaus/` file was touched.

## The rank mapping and why the order is exactly the same

A table choice's rank IS its row index: `Rank::OfTableRow(i) == i`. Every comparison the scan made on `ruleIndex` is
therefore the same comparison on the same int. The sentinels are unchanged too:

| Sentinel | Value | Before | After |
|---|---|---|---|
| no rule matched | -1 | `ruleIndex < 0` | `!Rank::IsChoice(rank)` = `!(rank >= 0)` |
| void window anchor | -1 | `firedRuleNow = -1`, `windowActive = inWindow && firedRuleNow >= 0` | `Rank::kNone`, `Rank::WindowActive` |
| walk ran out (stop index) | rank -1 | `(ruleIndex < 0) ? rows : ruleIndex` | `Rank::TableStop(rank, rows)` |
| not the combat table (`cast/`) | `kNoRule` = INT_MAX | `g_firingRule = ruleIndex` | `g_firingRule = rank` (same int) |
| record vanished mid-scan | (none) | `break` and keep the LAST choice | `Next()` returns false, the scan breaks and keeps the last choice |

The last row matters. The pre-I1 loop broke out when `g_followers.find(id)` failed, and `choice` still held the previous
(rejected) choice, so the reconciliation's stop index came from it. `ChoiceSource::Next` therefore returns `false` for
"record gone", leaving the output untouched, and returns `true` with `rank < 0` for "no more candidates".

IAUS ranks (`Iaus::RankOf`, `iaus/Decide.cpp:30`) are in `[1000, 1e9]`, which is above every table row (at most 254,
because FLWR's `gambitCount` is one byte, `Serialization.cpp:609/879`) and below `kNoRule`. So an IAUS decision outranks
an unranked lock exactly as a table rule does. The two ranges never mix on one follower (design 1.3).

## Proof 1: the reader audit

`git grep -n ruleIndex -- native` on main `9372a6e`: 81 hits. After: 6 hits, all comments (`ComposedCast.cpp:563` quotes
old code and was left alone. The 5 new ones are this card's own "was `ruleIndex`" notes).

### Combat scan, `native/scheduler/CombatScan.cpp` (before = main `9372a6e`, after = this branch)

| Before | Before expression | After | After expression |
|---|---|---|---|
| :755 | `if (sc.ruleIndex < 0) break;` | :1108 | `if (!Rank::IsChoice(sc.rank)) break;` |
| :752-754 | `rec = find; if end break; sc = Evaluate(f, rec->second, Combat, start)` | :1107 | `if (!retreatTable.Next(sc)) break;` (`TableSource::Next` :110) |
| :756 | `start = sc.ruleIndex + 1;` | :1109 | `retreatTable.Reject(sc);` (`m_start = Rank::NextTableRow(rank)` :116) |
| :776 | log arg `sc.ruleIndex` | :1129 | `sc.rank` |
| :923-931 | `firedRuleNow` = `recent.firedRule`, or the uid re-resolve `firedRuleNow = i` | :1274 | `src.SuppressionAnchor(inWindow, recent.firedRule, recent.firedUID)` (:122, same body) |
| :932 | `inWindow && firedRuleNow >= 0` | :1275 | `Rank::WindowActive(inWindow, firedRuleNow)` |
| :960-964 | `rec = find; if end break; choice = Evaluate(..., start); if (choice.ruleIndex < 0) break;` | :283-284 | `if (!a_src.Next(choice)) break; if (!Rank::IsChoice(choice.rank)) break;` |
| :983 | `windowActive && choice.ruleIndex >= firedRuleNow` | :303 | `Rank::StopsAtAnchor(windowActive, choice.rank, firedRuleNow)` |
| :984, :1018 | `matchedCast.push_back(choice.ruleIndex)` | :304, :338 | `matchedCast.push_back(choice.rank)` |
| :1008, :1032, :1121 | chain format arg `choice.ruleIndex` | :328, :352, :427 | `choice.rank` |
| :1009, :1033, :1131 | `start = choice.ruleIndex + 1;` | :329, :353, :436 | `a_src.Reject(choice);` |
| :1044-1052 | G9 cooldown: `find && ruleIndex < size` -> `combat()[ruleIndex]` | :363 | `a_src.CoolingReason(choice, now)` (:141, same body) |
| :1071-1076 | hesitation exempt: `ruleIndex < rec->...size()` -> `combat()[ruleIndex].conditionOpcode` | :382 | `a_src.HesitationExempt(choice)` (:152) |
| :1129-1130 | G10 skip reason: `find && ruleIndex < size` -> `combat()[ruleIndex].lastFailReason = ...` | :435 | `a_src.NoteSkipReason(choice, outcome.reason)` (:161) |
| :1197-1199 | `stopIdx = (ruleIndex < 0) ? size : ruleIndex`, equip above stop | :1339 | `src.EquipEvaluatedAboveStop(choice)` (:181) |
| :1219 | same stopIdx, silver above stop | :1350 | `src.SilverEvaluatedAboveStop(choice)` (:193) |
| :1234 | same stopIdx, Unholy silver evaluated | :1360 | `src.UnholySilverEvaluated(choice)` (:203) |
| :1252 | same stopIdx, `EquipRangeUndecidable(f, rules, stopIdx)` | :1371 | `src.EquipRangeUndecidable(f, choice)` (:214) |
| :1506-1508 | bookkeeping: `ruleIndex < size` -> `combat()[ruleIndex]` lastFired/lastFailReason/lastFiredAt/G9 NoteFired | :1622 | `src.Record(choice, outcome, now)` (:165, same body) |
| :1539 | `recent.firedRule = choice.ruleIndex;` | :1644 | `recent.firedRule = choice.rank;` |
| :1544-1545 | `firedUID = combat()[ruleIndex].uid` (if `ruleIndex < size`) | :1647 | `recent.firedUID = src.FiredIdentity(choice);` (:136) |
| :1553, :1556 | `[eval] fired rule {}` arg | :1655, :1658 | `choice.rank` |
| :1569-1570, :1574, :1577 | #22j fail dedupe + `[eval] ... did NOT fire` args | :1671-1672, :1676, :1679 | `choice.rank` |
| :1596-1597, :1601, :1604 | #22j hold dedupe + `[eval] ... held off` args | :1698-1699, :1703, :1706 | `choice.rank` |

Every table-row index read goes through `TableSource::Row` (:225), which keeps the old guard (`rank < combat().size()`
after a fresh `g_followers.find`) and adds `rank >= 0`. That case is unreachable at every caller (they all run after the
`rank < 0` break, or on a Fired / stopped choice). It is there because an interface method must not index `[-1]`.

### Elsewhere

| File:line (before = after) | Before | After |
|---|---|---|
| `Evaluator.h:12` -> `:20-21` | `int ruleIndex = -1;` | `int rank = -1; Source source = Source::Table;` |
| `Evaluator.cpp:982` | `out.ruleIndex = i;` | `out.rank = i;` |
| `cast/Fire.cpp:193` | `a_choice.ruleIndex < 0` (no rule -> no engine call) | `a_choice.rank < 0` |
| `cast/Fire.cpp:216` | `g_firingRule = a_choice.ruleIndex;` | `g_firingRule = a_choice.rank;` |
| `cast/Fire.cpp:364` | `CastSummonOnce(..., a_choice.ruleIndex, ...)` | `a_choice.rank` |
| `logistics/Upkeep.cpp:240` | `Evaluate(...).ruleIndex == i` | `.rank == i` |
| `logistics/Service.cpp` (26 sites: :380-381, :1487-2086) | `ruleIndex < 0`, `start = ruleIndex + 1`, `fired = ruleIndex`, log / `CastSummonOnce` args | `.rank`, same expressions |
| `logistics/LootScan.cpp` (6 sites: :1000-1102) | `ruleIndex < 0`, `start = ruleIndex + 1` | `.rank`, same expressions |

The logistics walks are table-only. The IAUS design runs combat only ("Logistics greyed", card I7), so `.rank` is their
row index by construction. They do not go through a `ChoiceSource`.

## Proof 2: the mechanical rename

Each of these files equals main's copy with `s/\.ruleIndex\b/.rank/g` applied, byte for byte (`cmp`):

```
IDENTICAL after the .ruleIndex->.rank rename: native/logistics/Service.cpp (26 sites)
IDENTICAL after the .ruleIndex->.rank rename: native/logistics/LootScan.cpp (6 sites)
IDENTICAL after the .ruleIndex->.rank rename: native/logistics/Upkeep.cpp (1 sites)
IDENTICAL after the .ruleIndex->.rank rename: native/cast/Fire.cpp (3 sites)
```

`Evaluator.cpp` / `Evaluator.h` with comments stripped: the only code deltas are `out.ruleIndex = i` -> `out.rank = i`,
the `ruleIndex` member -> `rank`, and the new `Source` enum + `source` member.

## Proof 3: host equivalence check of the predicates

`tools/rank_equiv/run.sh` (host g++, no CommonLib). It writes out the pre-I1 expressions verbatim (`ruleIndex < 0`,
`inWindow && firedRuleNow >= 0`, `windowActive && ruleIndex >= firedRuleNow`, `(ruleIndex < 0) ? rows : ruleIndex`,
`ruleIndex + 1`, the anchor `= i`) and compares each with the `Rank::` helper the scan now calls. The domain is every
pair of: -3..256 (the sentinel, every row a save can hold, just past it), INT_MIN, `kNoRule` (INT_MAX) and ten
`Iaus::RankOf` values including the clamp ends. Both window states, row counts 0/1/2/12/64/254/255. It also checks the
cross-source facts: IAUS rank >= 1000 > every row, every rank < `kNoRule`, `RankOf` strictly decreasing in the epoch.

```
rank_equiv: 304732 checks, 0 failed
```

What it does NOT prove: the `TableSource` methods are the old inline bodies moved (see the audit table). The host cannot
build them (they read `g_followers` and RE types). The reviewer's check is the side-by-side of each method with its
"before" lines. `tools/splitcheck` is not valid for this card: it is not a split, and `Tick` changed on purpose.

## Behaviour with IAUS off

Every follower runs the table source. `iausCombat` (FLWR bit 3, I5) still has no reader, and this card adds none, so:
- Same rule picked: the same `Evaluate(f, state, Combat, start)` calls in the same order, with `start` advanced at the
  same three sites.
- Same preemption: the window stop, `matchedCast` (HealSustainLap) and `g_firingRule` all carry the same int.
- Same `[eval]` lines: the format strings are untouched and every argument is the same int.
- Same cooldowns: `Cooldowns::Remaining` / `NoteFired` with the same (actor, `Kind::Rule`, uid, duration, now).
- Same co-save bytes: nothing serialized was touched.

## In-game check (for the next deck cycle)

IAUS-OFF `[eval]` parity run. Load the same save on a main build (`9372a6e` or the last main deployed) and on this
branch's DLL. Run the same fight start (same follower, same table) for about 60 s each, then `grep '\[eval\]'` both
`MFO.log`s, strip the timestamps, and compare. Expect: the same rule numbers in `fired rule N` / `rules A,B skipped ->
rule N` / `did NOT fire` / `held off` lines in the same order, and the same `combat table: N rules, none matched` line.
The order may only differ through combat timing. Also check that the board pulses (lastFired) and a G9 rule's "cooling
down" appear as on main. A mismatch in the rule NUMBERS is a defect. A mismatch in timing alone is not.

## Notes for I6

- The interface is the place I6 branches: `CombatScan.cpp:1272` builds the `TableSource`. An IAUS source implements the
  same methods. `SuppressionAnchor` returns the running decision's rank (identity = the epoch), `Record` feeds momentum /
  `OnFired`, `FiredIdentity` is the epoch, and the four reconciliation reads come from the posture layer.
- `ScanOut::matchedCast` stays a vector of ranks. For IAUS it is `{ rank of the running cast decision }` (design 1.3).
- The retreat self-heal walk (`:1104`) is deliberately a `TableSource` by name. Whether an IAUS follower gets a
  retreat self-heal is I6's decision.
- The `[eval] combat table: N rules, none matched` line still reads `combat().size()` directly. I6 decides its IAUS text.
- `g_firingRule` keeps its name. Under IAUS it carries ranks near 1e9, and `Hands.cpp:600` hashes `g_firingRule & 0xFF`
  for a once-per-change log key (`g_freshKeptLog`), which stays harmless (only a collision in log dedupe).
- `CombatScan.cpp` is 1730 lines (was 1622). The split (MFO-B655) needs the reconciliation and the bookkeeping extracted
  next. They are still spans inside `Tick`.
