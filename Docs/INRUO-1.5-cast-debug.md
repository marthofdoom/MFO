# INRUO — Skyrim SE 1.5.97 cast-control crash diagnosis

**Branch:** `inruo` (debug only — never merged, never released).
**Goal:** re-enable MFO's mage cast-control on Skyrim SE **1.5.97** so a willing
1.5 tester can reproduce the CTD and capture a fresh log that pins the exact
fault line — so #67 can be *fixed* on 1.5 instead of just disabled.

Background: the shipped fix (#67) gates the whole cast path to AE-only because it
access-violates on 1.5.97 (`Scheduler::Tick → Actuation::Fire → CastOn`, byte read
off a poisoned pointer). That stops the crash but also disables cast gambits on
1.5. This build turns the cast path back on for 1.5 **behind a toggle**, and
breadcrumbs each suspect step so the crash log tells us which one faults.

> ⚠️ This build **crashes on purpose**. It is a diagnostic tool, not a fix. Only
> hand it to a tester who's expecting a CTD and can send logs.

## What the tester does

1. **Install the debug DLL.** Replace `…/MFO/SKSE/Plugins/MFO.dll` with the
   `inruo` build's `MFO.dll` (from the branch CI artifact). Keep everything else
   (ESP, MCM, scripts) as-is.
2. **Turn the debug switch on.** In `Data/SKSE/Plugins/MFO.ini` set:
   ```
   bDebugCastSE=1
   ```
   (add the line if it isn't there). Also make sure cast-control is active:
   `iCastControl=2` (or higher) and `bForceCastOnMiss=1`.
3. **Confirm the build loaded.** Near the top of `MFO.log` you should see:
   ```
   === INRUO 1.5 CAST-DEBUG build — bDebugCastSE=ON runtime=SE ===
   ```
   If it says `runtime=AE` this isn't 1.5 (the crash won't happen); if
   `bDebugCastSE=off` the toggle didn't take.
4. **Reproduce.** Use a MAGE follower with a **cast** gambit set on the board
   (e.g. `Foe: any → Cast <a spell they know>`, or the exact spell from the
   original report — *Astral Wave*). Enter combat / let the gambit fire. The game
   will CTD.
5. **Send two files:**
   - **`MFO.log`** — the important part is the **last few `[bc] CastOn/…` lines**
     before the log ends. Example:
     ```
     [bc] CastOn/enter fol=XXXX spell=YYYY tgt=ZZZZ
     [bc] CastOn/avo …
     [bc] CastOn/magicka-cost …
     [bc] CastOn/equip-prepare …      <-- if this is the LAST line, the crash is in the spell equip
     ```
   - **the Crash Logger `.txt`** (the `EXCEPTION_ACCESS_VIOLATION` report).

## How to read it (for us)

The **last `[bc] CastOn/<step>` line** in `MFO.log` is the step that crashed —
the very next operation faulted before its own breadcrumb could print:

| last breadcrumb | fault is in | likely cause |
|---|---|---|
| `enter` | the range/`GetDelivery`/`GetRange` block | spell vtable read |
| `avo` | `AsActorValueOwner` / the reserve reads | AV owner offset |
| `magicka-cost` | `SpellItem::CalculateMagickaCost` | version-sensitive native / effect walk |
| `equip-prepare` | `Loadout::Prepare` (spell equip) | inventory-map walk / off-main equip |
| `consent-want` | `CasterConsent::Want` | latch map |
| `force-cast` | `ForceCast` → `Packages` (VM route on SE) | alias/VM |

Cross-check with the crash log's `MFO.dll+<offset>` (map via **this branch's**
PDB from the same CI run + `llvm-symbolizer`). Once the step is pinned, fix it
1.5-safely (e.g. do the equip on the main thread, or use a version-correct
offset/relocation), then the #67 gate can be lifted for 1.5.

## Turning it back off

Set `bDebugCastSE=0` (or reinstall a normal release DLL). With the toggle off,
this build behaves exactly like the shipped one — cast-control stays disabled on
1.5, no crash.
