# iaus_dash: terminal dashboard for the IAUS simulator

Host-only viewer for the per-run JSON files the offline simulator (`tools/iaus_sim`) writes. Python 3, standard library only
(curses, json, argparse, os). Nothing here is read by the DLL. The files under `fixtures/` are SYNTHETIC (invented numbers,
no list data); regenerate them with `fixtures/gen_fixtures.py` (fx1 to fx3 are schema 1 and prove old files still load, fx4
to fx6 are schema 2 and are shaped like a normal class report run: no named scenario rows, a gap ledger that names class
groups, the per-class gaps and the rating).

## Run

    python3 tools/iaus_dash/dash.py                      # reads ~/.cache/mfo-iaus-sim/runs/
    python3 tools/iaus_dash/dash.py --runs DIR           # another directory
    python3 tools/iaus_dash/dash.py --runs tools/iaus_dash/fixtures
    python3 tools/iaus_dash/dash.py --ascii              # ASCII bars/arrows (auto when the locale is not UTF-8)

The runs directory is polled every 2 s. A new run appears and becomes current unless you pinned one. A malformed or
half-written file is skipped and named in the status line. Works at 80x24 (use left/right to scroll wide tables).

## Keys

| Key | Action |
|---|---|
| `1`..`6`, Tab | Classes, Detail, Scenarios, Gaps, Trends, Runs |
| `j` `k` / up down, PgUp PgDn, `g` `G` | move |
| left / right | scroll columns |
| `/` | filter rows by text (Enter applies, empty clears, Esc clears) |
| `s` / `r` | cycle sort column / reverse |
| Enter | open class detail (Classes, Scenarios, Gaps) or pin the run as current (Runs) |
| Esc | back from detail |
| `x` | toggle compare deltas (against the compare run, else the previous run of the same list) |
| `?` | help |
| `q` | quit |

Classes view: `t` cycle tier, `L` cycle line, `l` cycle list, `P` toggle planned classes.
Trends view: `m` / `M` next / previous metric (also `gap`, `rating`, `rulings_pass_pct`), `c` cycle class (default all classes).
Runs view: Enter pins the selected run as current, `u` unpins (follow newest), `c` sets the compare run, `C` clears it.
Deltas are green when the change is in the better direction (see `BETTER_RULES` in dash.py), red when worse, plain for
neutral metrics.

## Views

1. Classes: one row per class aggregated (fights-weighted mean) over brackets and ranks. Columns: Class, T, **Rating (Tx)**,
   Line, Rul (rulings passed/total, red if any fail), Gap, Trend, then the metrics.
   - **Rating (Tx)** is the class's score on the simulator's ONE shared rating battery (`tools/iaus_sim/Rating.h`: 40 cells =
     10 foe archetypes x 2 group sizes x 2 level bands, the same fights for every class of a tier), ranked inside its tier:
     `T2 3/9 -2.1` = third of the nine T2 classes of this run, 2.1 U under the tier's mean. A trailing ` !` (red) is the
     below-tier flag the simulator sets: under the tier mean by more than 2.0 U and by more than two standard errors of the
     paired difference. `-` = the run has no rating (schema 1, or `--rating 0`). Sorting the column groups by tier, best first.
   - **Gap** is oracle U minus IAUS U for the class. First source that has a number: `class_gaps` (the gap pass every class
     report run now runs), `main_gap` on the class's rows, the mean gap of its scenario rows, the summed `delta_u` of the
     ledger rows that cover it. A ledger row names a class GROUP ("knight line", "casters", "*"): schema 2 rows carry the slugs
     they cover (`gaps[].classes`); for an older file the group is resolved from the class's line and name.
   - **Trend** is this gap against the nearest EARLIER run of the same list (and the same mode and stealth road, when both
     files say) that has a gap for this class. Most files in a runs directory are one-class probes, so "the run before" is
     usually not that. With compare on (`x`) it is against the compare run.
2. Class detail: the rating (score, by foe archetype, weakest archetype) and the oracle gap with its fight counts, metrics per
   bracket, action-mix bar per bracket, the class's rulings, scenarios and gaps.
3. Scenarios: IAUS U, oracle U, gap, delta vs previous (or compare) run; sorted by gap by default.
4. Gap ledger: coloured by status (open red, closing yellow, closed green, limit cyan).
5. Trends: sparkline and table over every run of the current run's list, oldest to newest.
6. Runs: time, list, branch, sha, label. `*` current, `c` compare, `^` pinned.

## Non-interactive

    python3 tools/iaus_dash/dash.py --runs DIR --report [--run ID] [--compare ID]
    python3 tools/iaus_dash/dash.py --json-check FILE

`--report` prints the classes table, the rating table, failing rulings, scenario gaps and the gap ledger as plain text
(newest run by default). Exit 2 if there is no run or the id is unknown. `--json-check` validates one file against the
schema (1 or 2), prints one problem per line, exits 1 on any problem, 0 if clean. `tools/iaus_dash/test.sh` runs both on
the fixtures.

## Schema v2 (unknown and extra fields are ignored, optional fields may be missing; a schema 1 file is a v2 file without the last three)

    { "schema": 2, "run_id": "YYYYMMDD-HHMMSS-<short>", "time": "<ISO>", "branch": "", "sha": "",
      "list": "Vanilla|Tuxborn|LoreRim|builtin", "seed": N, "fights_per_cell": N, "label": "",
      "oracle": { "k": N, "horizon_s": N, "kind": "" },
      "classes": [ { "class": "", "tier": N, "line": "Melee|Ranged|Mage|Hybrid|...", "planned": false,
                     "bracket": "", "rank": "I|II|III|IV|V|all", "fights": N,
                     "metrics": { "<name>": number }, "action_mix": { "<family>": pct } } ],
      "scenarios": [ { "name": "", "class": "", "iaus_u": x, "oracle_u": x, "gap": x, "metrics": {} } ],
      "rulings": [ { "id": "", "text": "", "class": "<name>|*", "metric": "", "op": "<=|>=|<|>", "target": x, "value": x, "pass": true } ],
      "gaps": [ { "id": "G1", "family": "", "class": "<name>|<group>|*", "classes": ["<name>"], "desc": "", "delta_u": x, "status": "open|closing|closed|limit" } ],
      "class_gaps": [ { "class": "", "gap": x, "se": x, "fights": N, "natural_gap": x, "natural_fights": N, "scenario_gap": x, "scenario_fights": N } ],
      "rating": { "fights_per_cell": N, "cells": 40,
                  "classes": [ { "class": "", "tier": N, "fights": N, "score": x, "se": x, "rank": N, "of": N, "tier_mean": x, "delta": x,
                                 "below": false, "weakest": "<archetype>", "weakest_delta": x, "by_archetype": { "<archetype>": x } } ] } }

Required for a file to load: `run_id`, `classes`. `--json-check` additionally wants `schema` 1 or 2, `time`, `list` and
checks types (also inside `rating` and `class_gaps` when present). Known metric keys are shown first in a fixed order (idle, hold, switches, heal, melee/taunt/power, deaths,
ttk, damage, bad_facts/invalid, decide_us); any other key follows alphabetically.
A ruling with class `*` is counted against every class. A class's gap is read as the Classes view above says.
