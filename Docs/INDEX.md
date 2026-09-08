# MFO Docs Index

This project is designed so any capable model or person can continue it from
these docs alone. Load documents on demand, not all at once.

> **START HERE, in this order (corrected 2026-09-07):**
> 1. [`../CLAUDE.md`](../CLAUDE.md) — the working rules, the scope discipline, the
>    engineering principles, the five things that corrupt saves, the threading model.
> 2. [`../MAP.md`](../MAP.md) — **the architecture and change-impact map.** Per-subsystem
>    responsibility, key symbols at `file:line`, and "what breaks if you change this".
>    CLAUDE.md says to consult it FIRST; this index used to not mention it at all.
> 3. [`INVARIANTS.md`](INVARIANTS.md) — 94 rules (80 numbered `#1`-`#80` + 14 lettered).
>    Read before ANY code change. Its "CITATION NAMESPACE" section explains `#N` vs `T#N`.
> 4. [`STATUS.md`](STATUS.md) — the living handoff: where the work is right now, what is
>    unmerged, what the last field session measured.
> 5. [`CAST-DELIVERY.md`](CAST-DELIVERY.md) — before touching any cast path.
> 6. [`ENGINE_NOTES.md`](ENGINE_NOTES.md) — before any native work.
>
> This INDEX is the map of the doc set. STATUS is "you are here". MAP.md is the code.

**CURRENT STATE (v2.0.1, 2026-09-07).** MFO is a shipped SKSE C++ plugin on the 2.0
line. It is an **APMF (Harbinger) client**: with the framework present, MFO routes its
cast, loot-travel and retreat behaviour through APMF's package/cast claim model, and the
older alias/force paths remain ONLY as the APMF-absent degrade. Four co-save records
(FLWR v5 / MSTK v1 / PRGN v6 / FWPN v1), three combat vfunc hooks plus a main-thread
pump, a worker-thread per-follower tick, an ImGui board on the live swapchain vtable, a
follower economy, and a progression add-on architecture (v1.1.0) with `MFO_Progression.esl`
as the worked example.

> **THIS BLOCK WAS PINNED AT v1.0.31 (2026-08-05) UNTIL 2026-09-07** while `VERSION`
> read `2.0.1`. Three of its claims were false and one was actively dangerous; they are
> corrected below rather than deleted, because a front door that quietly changes its
> story teaches nothing.

**HISTORICAL — the "MFO does not cast" finding is RETRACTED (2026-09-07).** This index
used to lead with *"The headline finding stands: MFO does not cast … `CastSpellImmediate`
(§0.8/§0.10) … refuted"*. **MFO casts.** `CastSpellImmediate` is the canonical
APMF-absent delivery (`Docs/CAST-DELIVERY.md`; `MAP.md` "known-working DIRECT FORCE"),
main-thread-posted with a hand-computed magicka deduct, and with APMF present the
follower's OWN AI performs a real animated cast driven by APMF's `CombatMagicCaster`
seats. `DoCombatSpellApply` is likewise not "removed": it is still dispatched at
`native/Actuation.cpp:1497` behind the default-OFF `bCommandCast`, kept for the
magicka-deduct measurement. `Projectile::LaunchSpell` remains genuinely refused for a
self-targeted spell (INVARIANTS `#56`).

**Next work is NOT "town errands (#31)".** The current head of work is the 2026-09-06
field diagnosis and its fixes — see `STATUS.md` and the two DIAGs
(`DIAG-2026-09-06-deny-heal-failures.md`, `DIAG-2026-09-06-loot-travel.md`). Town errands
(task `T#31`) sit behind the APMF Nexus package.

**Source-selection rule (#64), the biggest lesson of the M5/M9 stretch:**
*"Can I call X?"* is answered by CommonLibSSE headers. **"How does the game
already do X?" is answered by `Skyrim.esm` and shipped mods' ESPs.** MFO is the
first mod in this family that drives actor behaviour rather than menus, and its
questions are nearly all the second kind. One dump of `Skyrim.esm` shows every
package template vanilla ships. Read the data before reaching for an API.

**Sequencing rule learned the hard way (#61):** if a shipped mod already solves
it, read its source before building a probe. The §4.7 retention question cost
~90 minutes of play time and three builds; the answer was one sentence in an
open-source plugin already installed on this machine.

**Knowledge routing — where a finding goes when you learn it:**

| What you learned | Goes to |
|---|---|
| An engine mechanism worked, with a date/version/observed symptom | `ENGINE_NOTES.md` §0 (PROVEN), promoted per §10 |
| A mechanism is mapped but unrun | `ENGINE_NOTES.md` with a RESEARCHED tag |
| A rule whose violation caused a real failure | `INVARIANTS.md`, with the incident |
| A portable "never again" | `ANTI_PATTERNS.md`, tagged `[MFO]` + dated |
| Symptom → cause → fix | a dated `DIAG-*.md` (the live route); `DEBUGGING.md` is a July snapshot |
| The structure of the code moved | `../MAP.md`, **in the same change** |
| A working-practice rule | `BUILD.md` |
| **Generic to any CommonLibSSE-NG project** | **`../Linux-Native-Tools/`, in the same session** |

That last row is the one that decays if left. MFO has already contributed the
`actions/cache`-only-saves-on-success finding, the PCH `std::literals` trap,
follower-detection semantics, and the two log destinations.

**Two docs carry a status tag rather than facts, deliberately.**
`ENGINE_NOTES.md` marks every mechanism `PROVEN (sibling)` / `RESEARCHED` /
`UNKNOWN`, and `DEBUGGING.md` marks every entry `[SIBLING]` (hit for real) or
`[PREDICTED]` (derived, untested). **Promote the tags as things are proven;
delete predictions that turn out wrong rather than quietly editing them.** A
doc that reads as confident before anything has run is the exact failure the
family's one principle warns about.

## Read order

**Items 1-6 are the START HERE list at the top of this file.** What follows is the rest
of the doc set, with each entry's real status. Anything marked HISTORICAL carries a
banner on its own first lines; do not plan against it.

1. **`../CLAUDE.md`** — the rules. Scope discipline, the model policy, the ten
   engineering principles, the five save-corrupting/crashing areas, threading.
2. **`../MAP.md`** — the architecture + change-impact map. Navigate by `file:line`;
   re-verify before editing, and update the map in the same change if structure moved.
3. **INVARIANTS.md** (before ANY code change) — 94 load-bearing rules, each an
   imperative plus the failure mode that violating it produced. Tags: `INHERITED`,
   `DESIGN`, `MFO` (earned here, with version and symptom). Read its "CITATION
   NAMESPACE" section first: `#N` is an invariant, `T#N` is a task number.
4. **STATUS.md** — the living handoff.
5. **CAST-DELIVERY.md** (before touching any cast path) — the claim model, the
   APMF-present primary path and the APMF-absent degrade, ConcProxy, CastBounds, the
   per-hand cast lock, and the REJECTED APPROACHES list so nobody retries them.
6. **ENGINE_NOTES.md** (before any native work) — engine mechanisms, each tagged
   `PROVEN (MFO)` / `PROVEN (sibling)` / `RESEARCHED` / `UNKNOWN`. §0 is the proof
   record (40+ dated entries); §10 is the promotion protocol.
7. **The 2026-09-06 DIAGs** — `DIAG-2026-09-06-deny-heal-failures.md` (RC1-RC7 plus a
   14-row cast-facet deny audit) and `DIAG-2026-09-06-loot-travel.md` (why 0 of 6 loot
   dispatches engaged the travel package). These are the only documents that describe
   the field as it actually is; read them before theorising about cast or loot travel.
8. **ANTI_PATTERNS.md** (before repeating history) — the portable "never again" catalog,
   MFO's own entries tagged `[MFO]` and dated.
9. **BUILD.md** (before starting any milestone) — the working agreement: the per-build
   checklist, review recording, release procedure, testing gates. Its release-line
   paragraph is older than the current 2.0.x line; STATUS.md is authoritative there.
10. **TOOLING.md** (before touching the build pipeline) — the Linux-native toolchain end
   to end: Papyrus compile (`tools/compile.sh`), the MCM-Helper config pattern, the
   ESP/ESL generator (`MFO_GenerateESP.py`), the audits (`audit_esp.py`/`audit_mcm.py`)
   as merge gates, and the deploy flow.
11. **ADDON-API.md** (when touching task `T#74` progression / the ESL seam) — the FROZEN
   public contract for third-party progression addons. `MFO_Progression.esl` is its
   worked example. (`FOLLOWER-PROGRESSION-ESL-DESIGN.md` §18 is the rationale; §1-§17 of
   that file are HISTORICAL.)
12. **BOARD-EXTENSION-API-DESIGN.md** (SCOPING, not built) — how a third party could add
   tabs to MFO's in-game board. Frozen into ADDON-API.md when a tier is built.
13. **BALANCE.md** — the Rapport ladder and the tuning surface. Model doc; the numbers
   are derived, not measured.

**HISTORICAL — kept for the reasoning trail, banner-marked, not to be planned against:**
`DESIGN.md` (the Jul 2026 spec; its dated rulings are the value), `ARCHITECTURE.md`
(contradicts shipped code on hooks, records, threads and Papyrus — `MAP.md` replaces it),
`ROADMAP.md` (M0-M10 all shipped), `TEST_GUIDE.md` and `RUNBOOK-session1-2.md` (July test
procedure on the wrong modlist), `DEBUGGING.md` (a July snapshot with none of the
APMF-era log tags), `ECON_PAPYRUS_PLAN.md`, `V1.1-ADDON-ARCHITECTURE-PLAN.md`,
`V1.1-ACCEPTANCE-AUDIT.md`, `FOLLOWER-PROGRESSION-ESL-DESIGN.md` §1-§17,
`REVIEW-2026-08-18-comprehensive.md`, and the three `GAMBIT_*` design docs
(`GAMBIT-GUIDE.md` is the canonical vocabulary description).

**DELETED ENTRIES (2026-09-07):** this list used to include `MANUAL_MOD_CREATION_GUIDE.md`
and `DYNAMIC_OR_DROP.md` as read-order items 7 and 8. **Neither file has ever existed in
this repo.** `DESIGN.md` still cites them in five places; those citations are dead too.

## Sibling projects — reuse, don't re-derive

- **MEO** (`../marth-enchanting-overhaul/`) — the primary cross-reference.
  Source of the ImGui menu architecture, the co-save discipline, the MCM/INI
  surface, the generator patterns, and the newest ENGINE_NOTES.
- **MRO** (`../Requiem-modification/`) — the native pipeline's origin;
  `docs/PROJECT_PLAYBOOK.md` is the operational manual and
  `docs/NATIVE_REWRITE_PLAN.md` the hook doctrine.
- **MAO** (`../marth-alchemy-overhaul/`) — the most recent build of the
  toolchain; useful for CI and packaging shape, not for engine facts.
- **Linux-Native-Tools** (`../Linux-Native-Tools/`) — where this knowledge is
  supposed to coalesce. `instance-data-and-events.md`, `known-hooks.md`,
  `hook-site-verification.md`, `native-dll-via-github-actions.md`,
  `esp-without-xedit.md`, `papyrus-on-linux.md`.

## Known gaps in the shared knowledge base

**Actor AI is undocumented in this family, but it is not unknown.** Nothing
in Linux-Native-Tools or any sibling's docs covers AI packages,
`EvaluatePackage`, combat targeting, or `TESCombatEvent` — those projects
were item-and-effect mods. That absence is a fact about the docs. Applying
the family's own research method (`DESIGN.md` §2 — read the SKSE64 source for
the equivalent Papyrus native) maps the whole domain in an afternoon from
`Actor.psc`, PapyrusUtil's `ActorUtil.psc`, and po3's `PO3_SKSEFunctions.psc`,
all of which ship their sources inside the LoreRim install. The result is
`DESIGN.md` §4.5. MFO still owes Linux-Native-Tools a written actor-AI
document — **written as each mechanism is proven, in the release that ships
it, not afterward.**

**Two false-negative searches produced two wrong sections of this design, and
both are worth remembering:**

1. **Controller support.** MEO shipped full gamepad navigation (m32f/m36e,
   v0.41.0+), but it exists only in `native/plugin.cpp` and a CHANGELOG line
   — `ENGINE_NOTES.md` and Linux-Native-Tools say nothing. A docs-only search
   concluded it did not exist and the design budgeted it as novel risk.
2. **Actor AI.** A docs-only search concluded the entire Tier-B action
   vocabulary was unmapped ground requiring a from-scratch verification arc.
   It was one `Actor.psc` read away.

**The rule both cases teach:** an empty search of the knowledge base is
evidence about the *documentation*, never about the engine or the codebase.
Before writing "no precedent exists" into any document, grep the siblings'
`native/plugin.cpp` and CHANGELOGs, and read the Papyrus surface of the
mechanism in question. The knowledge base is a cache, not the source of
truth — and this family's method exists precisely because the source of truth
is the engine, the shipped code, and live memory.

## Research assets on this machine

Beyond the sibling repos, the installed modlists are a primary source and
should be read before anything is called unknown:

- `LoreRim/mods/Skyrim Script Extender (SKSE64)/Scripts/Source/` — the full
  vanilla + SKSE Papyrus surface (`Actor.psc` is the actor-control map, and
  is a **mandatory** Papyrus import path; the compiler-bundled copy is
  stripped).
- `LoreRim/mods/PapyrusUtil .../Scripts/Source/ActorUtil.psc` — package
  override semantics.
- `LoreRim/mods/powerofthree's Papyrus Extender/Source/scripts/` — the po3
  API surface MFO leans on for spell enumeration, combat target sets, and the
  tutored-spell revoke backstop.
- `LoreRim/mods/` at large — 3,000+ plugins for corpus scans, and installed
  reference mods (Wheeler, Valhalla Combat) whose published hook sites the
  family already cites.

## The one principle

When touching the binary format: copy a working vanilla record, never trust
documentation — including this documentation. If a record misbehaves, dump it
and its vanilla twin and diff subrecords. Every multi-day bug in this family
(TES4 flags, FOMOD wrapper, SPIT type, PERK layout, FormID prefix, SEQ, MGEF
fortify archetype) ended the moment we compared bytes against something that
worked.
