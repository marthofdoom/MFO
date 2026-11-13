# MFO Tuning Lab

A Linux desktop app (PySide6 / Qt 6, follows the KDE light/dark theme) for the IAUS tuning simulator. It never imports the simulator: it drives
`tools/iaus_sim/tune_driver.py` and the sim binary through their command lines and reads their output files. `tools/iaus_sim/` is not touched.

    tools/tune_app/install.sh     # KDE menu entry (~/.local/share/applications/mfo-tuning-lab.desktop) + the venv
    tools/tune_app/run.sh         # or start it directly; first start builds ~/.local/share/mfo-tune-app/venv from requirements.txt (pinned)
    tools/tune_app/selftest.py    # headless: readers on real run dirs, grammar, deploy to a temp dir, offscreen GUI + screenshots
                                  #   MFO_TUNE_CONF=<dir> QT_QPA_PLATFORM=offscreen selftest.py --runs <run dir>... --shot out.png

Settings live in `~/.config/mfo-tune-app/settings.json` (`MFO_TUNE_CONF` overrides the directory). Run dirs are found under the "runs root" and any
"extra run roots" (a directory holding `progress.log` or `summary.json`), plus the `--out` of every running driver.

## Tabs

| Tab | What it does |
|---|---|
| Launch run | Every driver option as a control (classes, stages, worlds, `--knobs`, `--no-race` when the chosen driver has it, `--rejudge`, `--sim-flags`, `--jobs`, taskset cores, nice, `--smoke`, `--for-class`, out dir, extra args). The exact command is shown before launch; it starts detached (`setsid`), so closing the app never stops it. Resuming an existing out dir is flagged. "Build…" runs `tools/iaus_sim/run.sh` into the chosen binary path. |
| Current runs | Every known run and running driver: stage, class, pass, cores in use, elapsed, ETA, last line age. Selected run: stage timings, `phases.txt`, live `progress.log` (filters: accepted only / class / text), Stop (SIGTERM to the process group, after a confirm; a stopped run resumes by launching the same command). The ETA is rough: finished classes of this round give the per-class mean, other stages use their longest earlier timing. |
| Tunings | Per run and class: knob table (shipped, new, pass, d, se, d/se, verdict and why), the kept list, a diff of two runs' kept tables, open decisions, per-class overview. |
| Vs oracle | `gap-run.json`: J headline, per-class gap bar chart (1 se bars), compare two runs, metrics per class, divergence ledger, the driver's gap stage (shipped vs tuned), J over runs. |
| Oracle / scenario runs | Builds a sim command (world, classes, scenarios, `--oracle`, K / horizon, seeds, fights, `--curve` / `--set`, class-oracle, gap scenarios, rating, decline, flags), launches it detached into `<runs root>/simruns/<name>/` (`command.txt`, `out.txt`, `result.json`) and shows earlier results. |
| Tuning files | Generates `<class>.txt`, `shared.txt`, `potions.txt` (`worth.*`), `looting.txt` (`loot.*`) from a run's kept values, edits them with line validation, backs up and deploys to LoreRim (local copy) and/or the Deck (ssh + scp; an asleep Deck is reported and a retry offered), then reads `[tuning]` lines from both MFO.log files. Backups are `name.bak-<time>` next to the file. |
| Settings | Paths. |

## Assumptions to check

- The hot-swap file grammar is the one in the brief (lines are the sim's `--curve` / `--set` specs, one per line, `#` comments). The in-game loader is
  not in this repository, so the app can only validate against the sim's grammar (`mfo_tune/tuningfiles.py`).
- Which `summary.json` entries become `shared.txt` lines: `stages.globals|worth|loot|logistics` items with a `set` and a verdict starting with `ACCEPT`.
- A driver older than the one the running tunings used may lack options (`--no-race`): controls are greyed from `tune_driver.py --help`.
