# tools/entrypoint_census

Offline (read-only) census of how the Skyrim executable evaluates perk entry points,
used to decide which `BGSEntryPoint` ids apply only to the player. Results are in
`Docs/ENGINE_NOTES.md` section 0.54. A new runtime re-runs these and diffs the table.

## What it does

`BGSEntryPoint::HandleEntryPoint` (AE `0x385F30` on 1.6.1170, `0x32ECE0` on 1.5.97,
`0x38CFF0` on 1.7.104; fork `BGSEntryPoint.h`, `RELOCATION_ID(23073, 23526)`) is the only
road that evaluates entry-point effects. These scripts find every call site, disassemble up
to it tracking `ecx` (entry point) and `rdx` (perk owner), and report whether the owner is
loaded from the PlayerCharacter singleton.

| Script | Purpose |
|---|---|
| `ep_census.py` | Every `call/jmp rel32` to HandleEntryPoint: containing function (from `.pdata`), entry point id, owner is player or not, PlayerCharacter-only vfunc. Emits JSON. |
| `vf_census.py` | Indirect call sites of `Actor` vfuncs `HasPerkEntries` (+0x7F8) and `ForEachPerkEntry` (+0x800). Proves HandleEntryPoint is the only road. Emits JSON. |
| `arity.py` | Argument setup per site (the condition-argument count between owner and `&value`). The engine's EP table is zero-filled in the image, so arity must come from call sites. Takes the `ep_census.py` JSON. |
| `ctx.py` | Address Library id and string context for each player-owned site. Takes the `ep_census.py` JSON. |
| `dz.py` | Plain disassembly window helper: `dz.py <exe> <start:end> ...` (hex RVAs). |

## Inputs

- Unpacked executables: `binaries/<version>/SkyrimSE.unpacked.exe` for 1.6.1170, 1.5.97 and 1.7.104
  (in the main checkout, not in git).
- Address Library `.bin` data files (offline only): e.g. `versionlib-1-6-1170-0.bin`, `version-1-5-97-0.bin`.
- Python with `pefile` and `capstone`; the helper `addrlib` from the Linux-Native-Tools
  repo (`tools/steamstub-rtti/addrlib.py`, path hardcoded at the top of each script, edit if it moved).

## Run

```
python3 ep_census.py <exe> <addrlib.bin> <hep_id> <player_singleton_id> <pc_vtbl_id> <char_vtbl_id> [selfcheck_id selfcheck_rva] > ae.json
python3 vf_census.py <exe> <addrlib.bin> <player_singleton_id> > aevf.json
python3 arity.py <exe> ae.json <ep,ep,...>
python3 ctx.py <exe> <addrlib.bin> ae.json
```

Ids (RELOCATION_ID order is SE, AE): HandleEntryPoint `23073` (SE) / `23526` (AE); player singleton `517014` (SE) / `403521` (AE).
Pass `0` for a vtable id you do not have (PlayerCharacter-only vfunc detection is then skipped).
Use the selfcheck pair to prove the Address Library file matches the binary (the research used
NotifyAnimationGraph 38048 -> 0x6A35F0 and SE 37020 -> 0x60F240).

## Expected result

1.6.1170: 115 call sites. 1.7.104: 115, classification identical to AE. 1.5.97: 109 (differs only in
site counts for EP 6/10/12/46/82/83/84, inlining). No EP changes class between runtimes.

Note: these scripts are the research tooling as run on 2026-11-09. They were committed
as-is and have no automated selftest; a rerun by hand against the three runtimes is the check.
