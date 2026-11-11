# iaus_dash: terminal dashboard for the IAUS simulator

Host-only viewer for the per-run JSON files the offline simulator (`tools/iaus_sim`) writes. Python 3, standard library only
(curses, json, argparse, os). Nothing here is read by the DLL. The files under `fixtures/` are SYNTHETIC (invented numbers,
no list data); regenerate them with `fixtures/gen_fixtures.py`.

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
Trends view: `m` / `M` next / previous metric (also `gap`, `rulings_pass_pct`), `c` cycle class (default all classes).
Runs view: Enter pins the selected run as current, `u` unpins (follow newest), `c` sets the compare run, `C` clears it.
Deltas are green when the change is in the better direction (see `BETTER_RULES` in dash.py), red when worse, plain for
neutral metrics.

## Views

1. Classes: one row per class aggregated (fights-weighted mean) over brackets and ranks. Tier, line, rulings passed/total
   (red if any fail), mean scenario gap, trend vs previous run of the same list, then the metrics.
2. Class detail: metrics per bracket, action-mix bar per bracket, the class's rulings, scenarios and gaps.
3. Scenarios: IAUS U, oracle U, gap, delta vs previous (or compare) run; sorted by gap by default.
4. Gap ledger: coloured by status (open red, closing yellow, closed green, limit cyan).
5. Trends: sparkline and table over every run of the current run's list, oldest to newest.
6. Runs: time, list, branch, sha, label. `*` current, `c` compare, `^` pinned.

## Non-interactive

    python3 tools/iaus_dash/dash.py --runs DIR --report [--run ID] [--compare ID]
    python3 tools/iaus_dash/dash.py --json-check FILE

`--report` prints the classes table, failing rulings, scenario gaps and the gap ledger as plain text (newest run by
default). Exit 2 if there is no run or the id is unknown. `--json-check` validates one file against schema v1, prints one
problem per line, exits 1 on any problem, 0 if clean. `tools/iaus_dash/test.sh` runs both on the fixtures.

## Schema v1 (unknown and extra fields are ignored, optional fields may be missing)

    { "schema": 1, "run_id": "YYYYMMDD-HHMMSS-<short>", "time": "<ISO>", "branch": "", "sha": "",
      "list": "Vanilla|Tuxborn|LoreRim|builtin", "seed": N, "fights_per_cell": N, "label": "",
      "oracle": { "k": N, "horizon_s": N, "kind": "" },
      "classes": [ { "class": "", "tier": N, "line": "Melee|Ranged|Mage|Hybrid|...", "planned": false,
                     "bracket": "", "rank": "I|II|III|IV|V|all", "fights": N,
                     "metrics": { "<name>": number }, "action_mix": { "<family>": pct } } ],
      "scenarios": [ { "name": "", "class": "", "iaus_u": x, "oracle_u": x, "gap": x, "metrics": {} } ],
      "rulings": [ { "id": "", "text": "", "class": "<name>|*", "metric": "", "op": "<=|>=|<|>", "target": x, "value": x, "pass": true } ],
      "gaps": [ { "id": "G1", "family": "", "class": "<name>|*", "desc": "", "delta_u": x, "status": "open|closing|closed|limit" } ] }

Required for a file to load: `run_id`, `classes`. `--json-check` additionally wants `schema` == 1, `time`, `list` and
checks types. Known metric keys are shown first in a fixed order (idle, hold, switches, heal, melee/taunt/power, deaths,
ttk, damage, bad_facts/invalid, decide_us); any other key follows alphabetically.
A ruling with class `*` is counted against every class. A class's "main gap" is the mean of its scenario gaps (the summed
ledger `delta_u` if it has no scenarios).
