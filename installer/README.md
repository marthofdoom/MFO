# MFO Synthesis patcher

Install-time patcher that adapts MFO to the user's load order. Unlike MEO,
there is **no standalone installer project** here — `MFO.Synthesis/` is the
only patcher, and the only shipped path. (The one non-Synthesis entry point is
the dev-only `selftest` verb in `Program.cs`, which runs the same analysis
off-VFS on Linux — see below.)

- **`MFO.Synthesis/`** — a [Synthesis](https://github.com/Mutagen-Modding/Synthesis)
  patcher, added in Synthesis from this GitHub repo and compiled locally on the
  user's machine. Distributing source (no binary on Nexus) sidesteps the exe/AV
  screening that got MEO's bundled installer blocked. Requires Synthesis.

It does ONE job, run once (and again after any load-order change):

**Item catalog** — it does NOT edit records. It scans the winning load order
with Mutagen and writes `Data/SKSE/Plugins/MFO/mfo_items.json`, the catalog the
DLL loads: potion restore types (health/stamina/magicka, read from real effect
records — Requiem/CACO-proof, unlike a runtime archetype guess), ammo class
(arrow vs bolt), jewellery, and the "never loot" exclusion list (quest items,
unique enchantments, scripted items with a second special signal). The analysis
lives in `Catalog.cs` (`Catalog.Write`); re-running is safe and idempotent.
Under MO2 the JSON lands in Synthesis's overwrite. The Synthesis output plugin
(`MFO - Patch.esp`) stays **empty** and is flagged ESL, so it costs no
load-order slot.

## Stack

.NET 9 console app + [Mutagen](https://github.com/Mutagen-Modding/Mutagen)
(`Mutagen.Bethesda.Skyrim`), on the pinned pairing MEO/MAO proved: Synthesis
0.36.5 → Mutagen 0.54.2. Runs natively on Linux — no game or VFS needed.

**Users add it in Synthesis** as a Git-repository patcher pointing at this repo,
project `installer/MFO.Synthesis/MFO.Synthesis.csproj` (or by opening
`assets/MFO.synth`, which does it for them).

### TRAP: Synthesis requires a SOLUTION at the repo root

Synthesis's git-patcher flow **clones this repo, locates a `.sln`, and builds
the project named by `MFO.synth`'s `SelectedProject`**. With no solution it
stops on a blocking "could not locate solution to run" — the patcher never
runs. MEO shipped broken for three releases exactly this way (it only ever got
built locally with `dotnet run`, never through Synthesis's clone-and-build).
Rules, learned there:

- **A minimal `MFO.sln` must exist at the repo root** — the patcher project
  only — and its project path must match `MFO.synth`'s `SelectedProject`
  **verbatim** (backslashes included).
  **STATUS: this repo does not have one yet. Until `MFO.sln` is added, the
  Synthesis onboarding path is broken for every user.**
- Keep the solution minimal: Synthesis rewrites Mutagen/Synthesis package
  versions in the projects it builds; extra projects are needless build
  surface and version-conflict risk on the user's machine.
- No new tag/release is needed to fix the patcher: Synthesis defaults to
  `PatcherVersioning=Branch` + `FollowDefaultBranch=true`, so a push to `main`
  reaches users immediately.

### TRAP: a Synthesis GROUP named "MFO" outputs `MFO.esp` and destroys the mod

Synthesis names each group's output plugin after the GROUP (`<Group>.esp`);
our `SetTypicalOpen("MFO - Patch.esp")` only applies to standalone runs. A
user who names their Synthesis group **"MFO"** gets an output plugin `MFO.esp`
— here an *empty ESL* — that overwrites (vanilla) or shadows (MO2) the real
`MFO.esp`, so every MFO form vanishes and the DLL goes dead while the
Synthesis run reports success. This bit MEO with a real user report; MEO's
`RunPatch` now refuses if the output is `MEO.esp`. **MFO's `RunPatch` does not
carry that guard yet.** User fix: rename the group, reinstall `MFO.esp`,
re-run.

### Known limitation: Creation Club plugins are not scanned

MEO measured that Synthesis's `state.LoadOrder` (and the raw `plugins.txt`)
OMITS the ~74 Creation Club plugins on an AE install — they load via
`<GameRoot>/Skyrim.ccc`. MEO builds its own calibration load order to
compensate; MFO's catalog pass currently uses `state.LoadOrder` as-is, so CC
items (Saints & Seducers potions, CC ammo, etc.) are absent from
`mfo_items.json` and the follower falls back to the DLL's runtime handling for
them. Adopt MEO's `BuildCalibrationLoadOrder` approach if this ever matters.

### Native-Linux case-sensitivity caveat

Run on a case-sensitive filesystem (native Linux), Mutagen fails to resolve
some CC master references → fewer records. This is a Mutagen-on-Linux
limitation, not app-specific (MEO measured 4139 vs 5392 conversions, same
binary, native vs Proton). Validate under Proton/Wine or Windows; users
running Synthesis via Proton (the typical Linux Skyrim setup) are fine.

## Build & run

```sh
# one-time: user-local SDK (no root)
curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 9.0 --install-dir ~/.dotnet
export DOTNET_ROOT=~/.dotnet PATH=~/.dotnet:$PATH
ulimit -n 8192   # 3400+ plugins = 3400+ open memory-mapped files

cd installer/MFO.Synthesis

# dev-only self-test: resolve an MO2 modlist off-VFS (Linux, no game/Synthesis),
# run Catalog.Write, print category counts + a sample
dotnet run -c Release -- selftest /mnt/gaming/modlists/LoreRim Default /tmp/mfo_items.json

# Synthesis patcher, standalone run (verification): pass a REAL data folder + plugins.txt
dotnet run -c Release -- run-patcher \
  --GameRelease SkyrimSE --DataFolderPath <Data> --LoadOrderFilePath <plugins.txt> \
  --OutputPath "<out>/MFO - Patch.esp"
```

Parity check: byte-diff the standalone/Synthesis `mfo_items.json` against a
`selftest` run on the SAME load order.
