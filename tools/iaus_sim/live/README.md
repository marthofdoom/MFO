# tools/iaus_sim/live: running a tune round

Host-only. Nothing here is in the DLL. Four small programs around `tools/iaus_sim/tune_driver.py`:

| File | What it does |
|---|---|
| `run_round.py` | starts or resumes one tune round, inside one systemd user scope, and writes the tuning files when it ends |
| `priority.py` | the priority level table, and get / set of a round's level while it runs |
| `gen_class_files.py` | a finished round's `summary.json` to per-class `MFO_Tuning/<class>.txt` files |
| `watch.py` | prints what a round is doing. Read-only |
| `test_live.py` | the tests of all four |

These replace the generator, the runner and the watcher of the 11-18 and 11-19 rounds, which lived in `/tmp` and were lost with it.
A round's directory belongs on a disk that survives a reboot. Never under `/tmp`.

## Start, stop, resume

```
tools/iaus_sim/live/run_round.py --out /mnt/.../tune-rounds/r3 -- --cells all --worlds builtin,LoreRim,Tuxborn,Vanilla
```

Everything after `--` goes to `tune_driver.py` unchanged. `--bin` and `--out` are the runner's own.

- The simulator is built once into `<out>/bin/sim` (or pass `--bin`). The driver's cache is keyed by the binary's hash, so a round keeps one binary.
- Stop: SIGTERM or Ctrl-C to `run_round.py`, or `systemctl --user stop $(cat <out>/unit)`. Every simulator of the round stops with it.
- Resume: run the same command again. Finished simulator results are in `<out>/cache.jsonl` and are not run again.
  The result is the one an uninterrupted round gives. `test_live.py` proves that on a small round (below).
- A resume with different driver arguments is refused. A round answers one question. Use a new `--out`, or `--new-args` when the change is meant.
- `<out>/round-status.json` holds the state: `running`, `stopped`, `failed`, `done`, `refused`.

## Priority

marth, 2026-11-23: "idle priority should be the default. But there should be a drop down to change the current priority".

The driver and all its simulators run in one systemd user scope, `mfo-tune-<out name>-<6 hex>.scope`. Its name is in `<out>/unit`.
The priority is a level. A level is two cgroup weights of that scope, so a change reaches every process at once, with no restart.
No process gets `nice`, `chrt` or `ionice`.

| Level | CPUWeight | IOWeight | Meaning |
|---|---|---|---|
| `idle` (default) | idle | 1 | runs only on CPU time nothing else wants (`cpu.idle = 1`) |
| `low` | 20 | 20 | a fifth of a normal unit's share under contention |
| `normal` | 100 | 100 | systemd's default |
| `high` | 400 | 400 | four times a normal unit's share |

- At launch: `--priority <level>`. Without it, the level remembered in `<out>/priority`. Without that, `idle`. A resume applies the remembered level again.
- While it runs: `priority.py <out> <level>`. It writes `<out>/priority` and calls `systemctl --user set-property --runtime <unit> CPUWeight=.. IOWeight=..`.
- `priority.py <out>` prints the level, the unit and the weights the unit has now, as one JSON line.
- **To add or change a level, edit `LEVELS` in `priority.py`.** It is the only table. The tune app's drop-down should import it:
  `priority.ORDER` (the names in order), `priority.DEFAULT`, `priority.get(out)`, `priority.set_level(out, level)`.
- No systemd user manager: the round runs at `nice 19` in its own process group, `<out>/unit` holds `none`, and both programs say loudly
  that live change is off. The level is still remembered for the next start.

Checked on this machine (systemd 257): the cpu and io controllers are delegated to the user manager, a round starts with `cpu.idle = 1`,
and `priority.py <out> normal` shows `CPUWeight=100 IOWeight=100` on the running unit, then `idle` again.

## The tuning files

`run_round.py` runs `gen_class_files.py --out <out>` when the driver ends with 0. By hand:

```
tools/iaus_sim/live/gen_class_files.py --out <round dir> [--dest <dir>] [--label <text>]
```

Output: `<dest>/<class>.txt` for every class the round searched, and `<dest>/MANIFEST.json`. Default `<dest>` is `<out>/MFO_Tuning`.
The line syntax is the game's (`native/iaus/TuningFile.h`).

Where a value comes from: `tune_driver.final_curves()` over the round's `summary.json`. That is the accepted values of the classes,
rejudge and gap stages, less what the recalib stage withheld. It is the same function the joint gate and `ship.table` use. Nothing else
is a source.

It refuses, writes nothing and exits 2 when:

1. The recalib stage in the summary judged another table. In round 2 of 11-19 the 23 new classes were written empty although the log
   showed accepted values. The likely cause, not verified because the old generator is gone, was a summary seeded from an earlier round
   whose recalib rows covered 15 classes. Here recalib rows are never values. Recalib may only withhold, and only when it judged exactly
   this summary's accepted values. The driver also drops a carried-over joint and recalib result when `--cells` differs from the summary's
   (`tune_driver.carry_over`).
2. The joint gate did not pass on this table. `--unshipped` writes anyway and marks every file `NOT JOINT-GATED`.
3. `progress.log` shows accepted values for a class in its last search and the summary holds none, with nothing recorded that explains it.
   `--accept-log-mismatch` writes anyway and records it in the manifest.
4. A value names a class outside the run's cells, or two values set one knob.
5. A class's search did not finish. Resume the round first.

A searched class with no accepted value gets a file with a header and no value. A cell the round never searched gets no file, because
an empty one would drop the values an earlier round shipped for it.

Shared tables (`shared.txt`, `potions.txt`, `looting.txt`) are not written. The driver reports a shared value as a decision for the
coordinator (`summary.md`).

## Watching

```
tools/iaus_sim/live/watch.py <round dir> [--interval 30] [--once]
```

State, unit, level and live weights, the stage, per class the knobs judged and values accepted, the last log lines, and how long ago the
log moved. It prints `STALLED?` when the unit runs and the log has been quiet for 45 minutes (`--stall-min`). A long oracle pass at idle
priority on a busy machine can be that quiet, so it is a prompt to look. It exits when the round is done (0), failed (1), refused (2) or stopped (3).

## Tests

```
python3 tools/iaus_sim/live/test_live.py                      # 16 tests, under a second
SIM_BIN=<built sim> python3 tools/iaus_sim/live/test_live.py   # + the stop / resume proof, about two minutes
```

The proof runs one small round to the end, runs it again with a SIGTERM inside the classes stage, resumes with the same command, and
compares the two `summary.json` files (less the cache statistics and the wrapper path) and the tuning files. It also checks that no
process of the stopped round is left and that the resume reused the cache.
