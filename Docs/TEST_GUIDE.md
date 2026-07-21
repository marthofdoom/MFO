# MFO — Test Guide

In-game verification. **Before claiming anything works, work the matrix.**

**Rule zero, inherited and non-negotiable: a stale binary voids every test.**
The first line of `MFO.log` prints the version and the game version. Check it
before believing any result. MEO was bitten by this twice.

**Rule one:** keep a read-only baseline save that has never seen this plugin.
Reload it for every test of a build whose *records* changed. When in doubt,
clean reload — persisted state lies.

---

## Session 1 — M1 + M2: the DLL loads, forms resolve, the co-save round-trips

The first real evidence the mod exists. Nothing here is gameplay.

### Package

```
Data/
  MFO.esp
  SEQ/MFO.seq
  SKSE/Plugins/MFO.dll
```

Build it with:

```bash
./tools/package_test.sh            # writes MFO-test.zip, installable in MO2
```

Install as a normal mod. **MO2 has TWO checkboxes** — the left-pane mod *and*
the right-pane plugin. A starless entry in `profiles/<P>/plugins.txt` is not
loaded, and the only symptom is MFO's own "form not found" line.

### Where the logs are (verified 2026-07-21 on this machine)

| Log | Path |
|---|---|
| MFO's own | `LoreRim/overwrite/SKSE/Plugins/MFO.log` |
| SKSE's | same directory, `skse64.log` |

If `MFO.log` is missing:
`find /mnt/gaming/modlists/LoreRim -iname 'MFO.log'` **before** concluding the
DLL never ran. (MRO's playbook cites a Proton appid and a `My Games/.../SKSE/`
path that do not exist on this machine — see `DEBUGGING.md` §0.)

### The matrix

| # | Step | Expect | Fails ⇒ |
|---|---|---|---|
| 1 | Launch, reach main menu | `skse64.log`: `plugin MFO.dll ... loaded correctly` | DLL not deployed, or MO2 checkbox |
| 2 | `MFO.log` first line | `=== MFO 0.0.1 loading — game 1.6.xxxx ===` | **stale binary — stop, redeploy** |
| 3 | Same log | `[forms] resolved MFO_FieldOrdersPower -> XX000801` | ESP missing/disabled, or FormID drift — run `tools/audit_esp.py` |
| 4 | Same log | `[forms] resolved MFO_GrantedSpell -> XX000802` | as above |
| 5 | Load any save | `[startup] kPostLoadGame — N follower record(s) live` | messaging listener not firing |
| 6 | Same | `[setup] granted Field Orders power` (first time) | form unresolved; check 3 |
| 7 | Console: `player.showinventory` / magic menu | **Field Orders** present as a Power | SPIT type wrong — audit checks this, so suspect deployment |
| 8 | Same log | `[p0] seeded 2 combat + 1 logistics gambit(s)` | seed guard tripped — already had data |
| 9 | **Save. Quit to desktop. Relaunch. Load that save.** | `[cosave] saved 1 follower record(s), schema v1` on save, then `[cosave] loaded 1 follower(s); dropped 0 ... disabled 0 ...` on load | **the P0 gate — this is the whole session** |
| 10 | Same log | `[setup] Field Orders power already present` | the grant is not idempotent |

### The two tests that actually matter

**A. Load-order change (proves `ResolveFormID`).**
Save with MFO active. Exit. **Enable or disable an unrelated ESP** so plugin
indices shift. Reload the save.

- Expect: the follower record still loads, and the gambit whose param is
  `0x00012FCD` (Healing) still resolves — its index moved and was remapped.
- A `disabled 1 rule(s) with missing targets` line here means resolution is
  broken, **not** that the mod is being cautious.
- Removing a plugin the rule depended on *should* disable that one rule and
  keep the rest. That is correct behavior, not a bug.

**B. Downgrade guard (proves the schema warning).**
Temporarily bump `kSchemaVersion` to 2, build, save with it, then revert to
the v1 DLL and load.

- Expect a **loud** `[cosave] SAVE IS NEWER (v2) THAN THIS DLL (v1)` error and
  **no records loaded**.
- If it silently loads anything, the guard is broken and users will lose data
  on the next save. This is the single most destructive failure mode in the
  co-save and it is worth the ten minutes to prove.

### Not in scope for session 1

No follower detection, no Rapport, no evaluator, no board. Using the Field
Orders power does nothing yet — it is granted so that step 7 can prove the
SPEL record is well-formed. **The board opener arrives at M7.**

---

## Session 2+ — added as milestones land

Per `ROADMAP.md`: M3 detection/Rapport, M4 the stick-poking harness, M5 the
evaluator, M6 logistics, M7 the board.

**Write each session's matrix before writing the code it tests.** A gate you
define afterwards is a gate you will define to fit whatever you built.
