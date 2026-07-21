# marth's Follower Overhaul (MFO)

Programmable follower behavior for Skyrim SE. Every follower carries an
ordered list of **Gambits** — `[Condition] -> [Action]` rules, first match
wins — authored live, in game, per follower. Final Fantasy XII's gambit
system rebuilt on the engine's own actor primitives.

**Status: pre-alpha (v0.0.1).** P0 only — the DLL loads and the SKSE co-save
round-trips a rule list. No gameplay yet. See `Docs/DESIGN.md` §10 for the
phase plan.

## What it does (when it's done)

Crosshair a follower, use the **Field Orders** power, and author rules:

```
1.  Ally: HP < 40%        ->  Cast Fast Healing
2.  Foe: undead           ->  Cast Turn Undead
3.  Self: magicka < 20%   ->  Drink potion
4.  Always                ->  Attack lowest-HP foe
```

Top-down, first match wins, one action per tick. Gambits **layer on top of**
vanilla AI — a follower with no matching rule behaves exactly as if MFO were
not installed.

Slots and vocabulary are earned through **Rapport**, built by fighting
alongside that specific follower. What unlocks depends on the follower's own
skills, and a taught spell they lack the magicka or skill to cast simply
fails — competence is not permission.

## Docs

Start at [`Docs/INDEX.md`](Docs/INDEX.md). It sets the read order and marks
which documents are specs versus which record proven behavior.

- [`DESIGN.md`](Docs/DESIGN.md) — the spec
- [`BALANCE.md`](Docs/BALANCE.md) — Rapport ladder and the reaction curve
- [`ARCHITECTURE.md`](Docs/ARCHITECTURE.md) — subsystems, threads, co-save schema
- [`INVARIANTS.md`](Docs/INVARIANTS.md) — read before any code change
- [`ENGINE_NOTES.md`](Docs/ENGINE_NOTES.md) — what is proven vs merely researched
- [`DEBUGGING.md`](Docs/DEBUGGING.md) — symptom → cause → fix

## Building

C++ sources live in `native/`. The DLL is built by GitHub Actions on
`windows-latest` (CommonLibSSE-NG needs the MSVC linker; there is no Linux
cross-build). `git push` is the build button.

```bash
gh run list --limit 1
gh run watch <id> --exit-status
gh run download <id> -n MFO-dll -D /tmp/mfo-dll
```

vcpkg registry baselines are **pinned deliberately** in
`native/vcpkg-configuration.json` — bump them on purpose, never float them.

## Licence

MFO's own code is MIT — see [`LICENSE`](LICENSE). All of it lives in
`native/`; no third-party source is vendored into this repository.

The shipped DLL statically links CommonLibSSE-NG (MIT, © 2018
Ryan-rsm-McKenzie) and its dependencies. Their notices are in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md), which ships with every
release. Update it in the same commit that changes `native/vcpkg.json`.
