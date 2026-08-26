# CLAUDE.md — MFO (marth's Follower Overhaul), SKSE C++ plugin

**Consult `MAP.md` first.** It is the architecture + change-impact map: per-
subsystem responsibility, key symbols at `file:line`, and — the point —
"Depended-on-by / What breaks if you change this." A bare symbol read will cause
regressions here; the ripple notes are why the map exists.

## Working rules

- **Navigate by `file:line` from MAP.md.** Grep to a symbol, read a narrow
  window — do NOT read whole files. The big ones (Logistics.cpp 4355, Board.cpp
  3347, ProgAllocator.cpp 2427, Packages.cpp 1657, CasterConsent.cpp 1066) must
  never linger in context.
- **Delegate bulk file-reads to a subagent** and keep only its conclusion, so
  large files never sit in the main context.
- **Never read vendored code:** `native/imgui_impl_win32.*` (the only vendored
  file in `native/`), plus any `build/`, `.git/`. ImGui comes via vcpkg.
- **BEFORE editing a subsystem:** re-read its MAP.md "What breaks" entry and
  re-verify it against current code (line numbers drift). **If the structure
  moved, update MAP.md as part of the change.** A stale map misleads the next
  session.
- Also consult `Docs/INVARIANTS.md` (49 numbered rules) and `Docs/ARCHITECTURE.md`
  before non-trivial changes; MAP.md cites both as `#N` / §N.

## The five things that corrupt saves or crash — verify before touching

1. **Co-save layout** (`Serialization.cpp`, `ProgAllocator::CoSaveSave/Load`,
   `State.h`, `Vocabulary.h`): 4 records FLWR v4 / MSTK v1 / PRGN v5 / FWPN v1
   (`Serialization.h`). Changing a
   field order/type/count, bumping a version without a matching `if(version>=N)`
   reader, renaming a serialized opcode string, or renumbering the `Subject` /
   `Stance` / `combatClassOverride` enums corrupts live saves. Keep readers for
   every shipped version forever (#12).
2. **`ResetAllState` order** (`Serialization.cpp:562`): `StopPump()` first, then
   clears. Reordering, or mutating save-scoped maps off the drained worker, is UB.
3. **Alias fills** (`Packages.cpp`): persist into the `.ess`. Never skip/reorder
   `ReleaseAll` (kPreLoadGame / post-load / revert). Evict marker must stay a
   non-actor XMarker (base `0x3B`) or furniture-eject re-breaks. `EvaluatePackage`
   `resetAI` must stay `false`.
4. **Combat vfunc hooks** (`Targeting`/`CasterConsent`/`CombatStyle`): install-once
   at `plugin.cpp:293-295`, VR-refused, run on the combat thread. Any
   `CombatController` member touched there must be `< 0x68` (AE +8 layout bug).
5. **Frozen external contracts:** `Forms.h` FormIDs (↔ `MFO_GenerateESP.py`);
   `MEO_API.h` (byte-shared with a separate MEO.dll, append-only); TradeBridge's
   10 Papyrus natives + Papyrus.cpp's 3 method-name strings (called by shipped
   `.pex`); Config INI key names (MCM-Helper persistence identity).

## Threading (one wrong access = a data race)

- `g_followers` / `Followers::g_active` / `g_activeIds` are **main-thread /
  serial-SKSE-task only, no lock** (#4). Off-thread → snapshot, never lock.
- The per-follower tick runs on the **AddTask job worker** (driven by the sleeper
  thread in `Diagnostics.cpp`, `kPumpMs=133`). `MainThread::Post` is the *only*
  road to the true main thread (for 3D/cell mutation, physics queries).
- Combat-thread hooks read FormIDs / atomic mirrors, never the follower lists.
