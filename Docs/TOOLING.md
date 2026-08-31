# TOOLING.md — the MFO build pipeline, end to end

The plugin is a Linux-native build: **no Creation Kit, no xEdit, no Windows.**
The ESP/ESL are emitted from Python, the Papyrus scripts compile under Proton,
the DLL builds in CI, and two Steam Decks receive the package. This doc is the
consolidated pipeline so the next session doesn't have to reconstruct it from
MEO. It complements `ARCHITECTURE.md` §8 (ESP-from-source) and the
`Linux-Native-Tools/` notes (`papyrus-on-linux.md`, `esp-without-xedit.md`,
`native-dll-via-github-actions.md`).

The whole loop, in order:

```
tools/compile.sh   →  out/Scripts/*.pex          (Papyrus, once per .psc change)
MFO_GenerateESP.py →  out/MFO.esp, MFO_Progression.esl, SEQ/*, MCM/*   (every ESP change)
tools/audit_esp.py    +  tools/audit_mcm.py       (merge gates — PASS required)
CI (native workflow)  →  MFO.dll                  (never local; verify green)
tools/package_test.sh →  MFO-test-vX.zip          (DLL + ESP + SEQ + MCM + Scripts)
deploy               →  custom-modlist (syncthing) + Tuxborn (scp)
```

---

## (a) Papyrus compile — `tools/compile.sh`

MFO ships a handful of `.psc` scripts (the TradeBridge script `MFO_Trade`, and
the two empty MCM-Helper shims `MFO_MCM` / `MFOP_MCM`). They compile to `.pex`
under Proton because a native Linux Papyrus compiler doesn't exist.

- **Wine:** the **Proton Hotfix** bundle's wine (`.../Proton Hotfix/files/bin/wine`)
  — it carries wine-mono, which system wine lacks. `WINEDATADIR` points at the
  Proton wine share so mono resolves.
- **Compiler:** the **Nemesis-bundled** `PapyrusCompiler.exe` (under
  `LoreRim/mods/Project New Reign - Nemesis .../Papyrus Compiler/`), with its
  `TESV_Papyrus_Flags.flg`.
- **Import order (matters — first match wins):**
  1. `Source/Scripts` — MFO's own scripts.
  2. `Source/Stubs` — **compile-only** shims for base types absent from the SKSE64
     source dump (`Class.psc`, `Message.psc`, `GlobalVariable.psc`, and the two
     MCM bases `SKI_ConfigBase.psc` / `MCM_ConfigBase.psc`, etc.). These shadow
     nothing at runtime — they exist only so the compiler resolves `extends`.
  3. SKSE64 `Scripts/Source` (the full engine script sources).
  4. po3 Papyrus Extender + PapyrusUtil sources.
  5. the Nemesis compiler's own `scripts/`.

Usage:

```bash
tools/compile.sh MFOP_MCM      # one script, no .psc suffix
tools/compile.sh all           # every MFO-owned script
```

Success is the literal `1 succeeded, 0 failed` line; the script greps for it and
prints `OK`/`FAIL` per script.

**When to recompile:** only when a `.psc` changes. The compiled `.pex` for the
stable shims is **committed** (`out/Scripts/MFO_MCM.pex`,
`out/Scripts/MFOP_MCM.pex`) — the empty MCM shims never change, so their `.pex`
is checked in. `out/Scripts/MFO_Trade.pex` is `.gitignore`d (recompiled fresh
each release; its build timestamp is nondeterministic).

### The MCM shim script

An MCM-Helper config needs a quest carrying a script that `extends
MCM_ConfigBase`, but the script body can be **empty** — MCM Helper renders the
menu from `config.json`, not from Papyrus. So `MFOP_MCM.psc` (and `MFO_MCM.psc`)
are one-line shims:

```papyrus
ScriptName MFOP_MCM extends MCM_ConfigBase
```

---

## (b) The MCM-Helper config pattern

Four moving parts make a working MCM menu. MFO's main plugin and the progression
addon each carry a full set; the two differ only in how a control is *bound*.

1. **The empty shim script** (§a) — `extends MCM_ConfigBase`, compiled to
   `out/Scripts/<Name>.pex`.
2. **A QUST carrying it via VMAD**, start-game-enabled and **NOT run-once**
   (SkyUI cannot re-register a run-once quest). Zero VMAD properties. Emitted by
   the generator (`make_mcm_quest` for MFO.esp `0x808`; `make_prog_mcm_quest` for
   the ESL `0x870`).
3. **`Data/MCM/Config/<modName>/config.json`** — the menu definition. MCM Helper
   derives `<modName>` from the config folder and matches it to the plugin. Top
   keys: `modName`, `displayName`, `minMcmVersion` (MFO/MEO use **9** — a config
   without it silently fails to load), `pages[]` each with `content[]` controls.
4. **The SEQ entry** — a start-game-enabled non-run-once quest never starts on an
   *existing* save unless its FormID is listed in `Data/SEQ/<plugin-stem>.seq`
   (a flat array of `uint32` FormIDs). MFO.esp's quest rides `SEQ/MFO.seq`; the
   addon's rides `SEQ/MFO_Progression.seq`.

### GlobalValue vs ModSettingFloat binding — the key fork

A slider's `valueOptions.sourceType` decides where its value lives:

- **`ModSettingFloat` / `ModSettingBool` / `ModSettingInt`** (MFO.esp's menu):
  MCM Helper persists the value to `Data/MCM/Settings/<mod>.ini`, and the DLL
  reads that INI. This is a **five-place wiring** (config.json, the defaults
  `settings.ini`, the seed user store, `Config.cpp kMcmDefaults[]`, and the
  `Config.cpp` parse switch) — all five must agree or the control is dead (#55).
  `tools/audit_mcm.py` (no args) is the gate that enforces it.
- **`GlobalValue`** (the progression addon's menu): the slider binds directly to
  a `GLOB` record by `sourceForm: "<Plugin>|0xLOCALID"` (e.g.
  `"MFO_Progression.esl|0x802"`). Moving the slider writes the **global's runtime
  value** — no INI, no `Config.cpp` wiring at all. This is why the addon can ship
  its entire tab **inside the ESL + its own config**, with zero references in
  MFO.esp or MFO.dll.

### The DLL live-reads on menu close

An MCM edit must apply *this session*, not at next load. The `MenuSink`
(`native/Diagnostics.cpp`) fires on Journal/MCM close and re-reads settings:

- For MFO.esp's ModSetting menu → `Config::Read()` re-ingests `MFO.ini`.
- For the addon's GlobalValue menu → `ProgAllocator::OnMenuClose()`, which
  marshals to the true main thread and calls `ReloadEconomy()` (re-reads each
  economy `GLOB`'s runtime value into `g_econ`). **This is fully generic:** the
  DLL discovers the economy GLOBs by walking `Progression::Addons()` and matching
  by editor-id *suffix* — it never names `MFO_Progression`, exactly as it would a
  third-party addon. Post-load-game re-reads too, so a save's persisted global
  values apply.

---

## (c) The ESP/ESL generator — `MFO_GenerateESP.py`

Emits `out/MFO.esp` (the main plugin, masters Skyrim.esm) and
`out/MFO_Progression.esl` (the optional addon, masters Skyrim.esm + MFO.esp),
plus their SEQs and the MCM configs, all from pure Python — no CK. `run:
python3 MFO_GenerateESP.py out`.

### FormID bands / master-index rules

- **MFO.esp** — own local FormIDs `0x800`+, master index **0x01** (one master).
  `FID_*` constants at the top; `OWN = 0x01000000`. These are a **frozen
  contract** with `native/Forms.h` (#41) — changing one orphans every save that
  saw it. `0x802` is permanently reserved.
- **MFO_Progression.esl** — `PGID_*` constants, prefix `OWN_PROG = 0x02000000`
  (own records move to master index **0x02** because the ESL masters *two*
  plugins). Local ids still `0x800`+ (ESL-legal range `0x800`–`0xFFF`).
  `PROG_NEXT_OBJECT_ID` must stay **above every emitted local id** (bumped to
  `0x871` when the MCM quest `0x870` was added).
- Cross-references from the ESL into Skyrim.esm use master index `0x00`; into
  MFO.esp use `0x01` (the addon-sentinel keyword the manifest points at).

### Record helpers

Low-level shape helpers (all little-endian, dumped from the shipped master, never
inferred): `subrec(type, data)`, `record(type, fid, flags, body)`,
`group(label, data)`, `zstr(s)` (**ASCII-only** — multibyte UTF-8 desyncs Papyrus
error lines), `qust_dnam(flags, priority, qtype)` (the decoded QUEST_DATA byte
layout), and `VMADBuilder` (script-attach records). Higher-level makers per record
type (`make_mcm_quest`, `prog_glob`, `prog_flst`, `prog_mesg`,
`make_prog_mcm_quest`, …). Top-group order **mirrors Skyrim.esm's relative
order** (e.g. the ESL emits KYWD < GLOB < QUST < FLST < MESG).

> **INVARIANT #75 — subrecord ORDER within a record must match the real record
> definition.** Concatenating `subrec()` blobs means the maker controls the order,
> and the game engine reads by type regardless of order (so a wrong order LOADS FINE
> and passes `audit_esp.py`). But xEdit / Vortex / Synthesis validate strictly and
> REJECT out-of-order records (xEdit: "unexpected (or out of order) subrecord" +
> a fatal `EVariantTypeCastError` that disables editing — a real v1.1.1 user report).
> NEVER guess the order: dump a reference record of that type from an installed
> `Skyrim.esm` and mirror it. Known traps (fixed v1.1.2): **QUST** puts `VMAD` right
> after `EDID`, before `FULL` (`EDID, VMAD, FULL, DNAM, …`); **MESG** is
> `EDID, DESC, FULL, INAM, DNAM` (DESC before FULL, a 4-byte INAM icon FormID present).
> Affects both `MFO.esp` and the ESL. Dump-verify
> the regenerated `out/*.esp`/`.esl` against Skyrim.esm after touching any record maker.

### The addon manifest seam (§18.6)

The addon is discovered **generically**: an `FLST` (`MFOP_AddonManifest`) whose
first entry is MFO.esp's sentinel keyword, then the classes-list FLST, then every
economy `GLOB`. The DLL enumerates these — it never looks the addon up by
filename. New economy knobs are added to `PROG_GLOBS` + `PROG_MANIFEST_ECONOMY`
and matched in the DLL by editor-id suffix (`ProgAllocator::AssignEconomyGlob`).
The addon's MCM `config.json` + `settings.ini` are emitted by
`write_prog_mcm_files()`; its SEQ beside the ESL write in `main()`.

---

## (d) The audits — merge gates

Both run in `package_test.sh` before a zip is built; a FAIL stops the package.
The engine drops malformed records **silently**, so these parse the emitted
artifacts back and check them.

### `tools/audit_esp.py` — structural + FormID validation

`python3 tools/audit_esp.py out/MFO.esp out/MFO_Progression.esl` (no args audits
both). Per-plugin **PROFILES** carry a `REQUIRED` table (every FormID the DLL
hardcodes, with its record type + required subrecords) and a SEQ expectation set.
Checks: TES4 parses + ESL flag + exact master list; every own record uses the
right master-index prefix; local ids in ESL range; `NEXT_OBJECT_ID` above every
emitted id; no duplicates; required records present with required subrecords;
and the **SEQ** lists every start-game-enabled non-run-once quest (the SEQ file
is derived from the plugin stem — `MFO.seq` / `MFO_Progression.seq`). Adding a
new DLL-referenced record means adding it to `REQUIRED`/`PROG_REQUIRED` **and**
to its SEQ set if it's such a quest.

### `tools/audit_mcm.py` — MCM consistency

- **No args** → the MFO.esp five-place ModSetting audit (the #55 gate): every
  `config.json` control must exist in the defaults `settings.ini`, the seed user
  store, `Config.cpp kMcmDefaults[]`, and the `Config.cpp` parse switch, with
  type-aware default agreement across all four.
- **A `config.json` path arg** → the **GlobalValue** audit (for a
  GlobalValue-bound addon tab that has no ModSetting store / Config.cpp wiring):
  checks `modName` / `displayName` / `minMcmVersion` present, and every
  GlobalValue slider carries a well-formed `sourceForm` + numeric
  min/max/step + in-range `defaultValue`. Used as:
  `python3 tools/audit_mcm.py out/MCM/Config/MFO_Progression/config.json`.

---

## (e) Deploy flow

The DLL is **CI-only** — never built locally. `package_test.sh` pulls the latest
green `native` run's `MFO-dll` artifact, regenerates + audits the ESP, and stages
a `Data/`-rooted zip containing: `MFO.esp`, `MFO_Progression.esl`,
`SEQ/{MFO,MFO_Progression}.seq`, `SKSE/Plugins/MFO.dll` (+ `MFO.ini` + baked board
fonts), the whole `MCM/` tree (both configs), `Scripts/` (both compiled shims +
`MFO_Trade.pex`), and `THIRD-PARTY-NOTICES.md`.

> **Always verify CI is green first** (`gh run list --workflow=native` shows
> `success`). A summary claiming "it built" is not proof — v1.0.8/1.0.9 both
> shipped red. (verify-ci-green memory, INVARIANTS #44.)

Two decks receive it:

- **custom-modlist deck (syncthing):** unzip the package over
  `/mnt/gaming/modlists/custom-modlist/mods/MFO/` **only** (keep its `meta.ini`),
  force a syncthing rescan, then poll the deck until its `MFO.dll` sha256 matches
  the packaged one. Log:
  `deck@marthdeck:~/Games/custom-modlist/overwrite/SKSE/Plugins/MFO.log`.
- **Tuxborn deck (scp):** Tuxborn is **not** syncthing-linked — deploy directly
  over SSH. Log:
  `/home/deck/Games/Tuxbornrc1/overwrite/SKSE/Plugins/MFO.log`.

**Hash-verify both decks** against the packaged DLL — a matching sha is the only
proof the deploy landed (a stale MO2 overwrite is the classic silent failure). If
the deck is unreachable it is usually **asleep** (SSH timeout), not a deploy
failure — retry when awake (deck-sleeps-ssh-timeout memory).

`MFO.log` is **truncated every launch** (one file, no backup) — a bug's session
is gone after a relaunch, so reproduce and pull the log before the next launch.

Releases (tagged, immutable) go through the two-phase `release.sh`; the public
`gh release create` is run by the **main agent** in direct conversation with
marth, never a subagent (release-scope memory).
