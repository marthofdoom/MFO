# MFO Docs Index

This project is designed so any capable model or person can continue it from
these docs alone. Load documents on demand, not all at once.

**CURRENT STATE (v0.6.0, 2026-07-22).** Gambits execute. A follower with a rule
list evaluates it ~7.5x/sec, one action per tick, first match wins, and acts.

**Proven in-game** (`ENGINE_NOTES.md` §0 carries dates and observed symptoms):
follower detection and un-detection, form resolution, the **populated co-save
round-trip** (M1, closed), Rapport crediting including boss multipliers, the
evaluator firing and correctly refusing a spell the follower cannot afford
(§5.3's competence gate), the `UpdateCombat` targeting hook installing, and
`act.attack` latching a chosen foe.

**The headline finding: MFO does not cast.** It puts a spell in the follower's
hand and their own AI casts it — animated, magicka-charged, correctly aimed,
because it is the vanilla path (§0.15). Three cast *verbs* were refuted getting
there: `CastSpellImmediate` (§0.8/§0.10), `Projectile::LaunchSpell` (#56), and
`DoCombatSpellApply` (§0.14). Animation events cannot drive it either — the
graph emits them (§0.17).

**The live constraint (§0.16):** that path is **AI-discretionary**. The RULE
always fires and the effect always lands, but the ANIMATION only happens when
the follower's AI independently judges the spell worth casting. For a weak heal
it never did.

**Next, and it is the last unknown in the core loop: M9, the forced casting
package.** It is the only mechanism that gives an uninterruptible commanded
cast. Fully researched from ALYSLC's shipped ESP; §0.17 has the byte-level
record layout. **One question decides its size and is not yet answered:** can a
PACK instance point `PKCU.template` at a VANILLA template, or must one be
authored? That is a read of `Skyrim.esm` and costs no play time.

**Deferred:** M6 (logistics) — nobody can author those rules until the board
exists. **M7 the board** is *first shareable*. MCM after it, because the board
defines what is configurable.

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
| Symptom → cause → fix | `DEBUGGING.md`, promoted from [PREDICTED] |
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

1. **DESIGN.md** (always) — what MFO is: the player loop, the gambit
   vocabulary, the evaluator, Rapport, the board. **Two rule tables per
   follower** — combat (§3–§4) and logistics (§4.8, upkeep: potions, ammo,
   equipment) — with separate slots, separate cadences, and no interleaving.
   §4.5 is the one section to read before estimating anything; it splits the
   mod into proven and unproven engine ground.
2. **BALANCE.md** — the Rapport ladder (250/1,000/2,500/5,000), its content
   budget, shared-kill credit, the reaction-spread curve, and the full tuning
   surface. **Derived, not validated** — every number has a stated model and
   is expected to move once P1 measures real kills-per-hour.
3. **ENGINE_NOTES.md** (before any native work) — the engine mechanisms MFO
   depends on, **each tagged with how much it is actually trusted**:
   `PROVEN (sibling)`, `RESEARCHED` (mapped from a primary source, never run
   by anyone here), or `UNKNOWN`. **§0 holds the first `PROVEN (MFO)` entries**
   — detection, form resolution, the ESL band, SPIT type 3, and what the same
   session explicitly did NOT prove. §9 is the verification queue (the real
   research plan); §10 is the promotion protocol. Also read **MEO's copy**, the authoritative one in the family —
   but not MAO's, which is a stale fork whose §3 lacks the cost-override fix
   and whose §6 still contains a retracted co-save claim.
4. **ARCHITECTURE.md** (before touching `native/plugin.cpp`) — module split,
   subsystem map, the thread/lock model, the tick pipeline, hook and sink
   inventories, co-save schema, generator↔DLL contracts, startup order.
   **Planned, not built** — its `file:line` refs are placeholders tagged with
   the phase that will fill them.
5. **INVARIANTS.md** (before ANY code change) — 49 load-bearing rules, each
   an imperative plus the failure mode that violating it produced. All are
   currently tagged `INHERITED` (cited to the sibling that paid) or `DESIGN`
   (following from MFO's own decisions). **Replace a tag with a version and
   symptom the moment MFO earns its own scar** — a local incident outranks a
   borrowed one because the next person will believe it.
6. **ANTI_PATTERNS.md** (before repeating history) — the portable "never
   again" catalog. MEO's digest trimmed to what applies, plus **MFO's own**
   tagged `[MFO]` and dated. Most MFO entries come from Fable reviews: with no
   local compiler, review is the only gate before CI, and it has caught two
   save-corruption paths and a cursor-over-gameplay blocker.
7. **MANUAL_MOD_CREATION_GUIDE.md** — the binary format reference. Copy
   MEO's; MFO's record needs (MGEF, SPEL, QUST+VMAD, SEQ) are a strict
   subset of what it already documents.
8. **DYNAMIC_OR_DROP.md** — the portability ledger. MFO is structurally
   compliant by construction (the whole action vocabulary derives from the
   live actor), so this file may stay thin — but the ledger still gates 1.0.
9. **BUILD.md** (before starting any milestone) — the working agreement: the
   per-build checklist, review recording, release procedure, testing gates,
   and the division of labour. **This file was missing for MFO's first
   session, and every rule in it was already written down in MAO's and MRO's
   `BUILD.md` — that gap is why the review rule went unfound.** It carries an
   honest table of the process violations that cost real work.
10. **ROADMAP.md** — the build order from here: M0 (CI green) through M9+
   (Tier B). Names two targets explicitly — **M5 first playable** (gambits
   work, console-seeded) and **M7 first shareable** (a human can author
   them). Its M4 is the de-risking step that is deliberately *not* in
   `DESIGN.md`'s phase table: poke each engine primitive with a stick and log
   what happens, before building on assumptions about it.

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
