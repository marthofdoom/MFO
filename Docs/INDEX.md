# MFO Docs Index

This project is designed so any capable model or person can continue it from
these docs alone. Load documents on demand, not all at once.

> **START HERE to continue work: [`STATUS.md`](STATUS.md)** — the living handoff
> (latest version, what shipped, what's awaiting marth's field test, ranked open
> issues, backlog, and the workflow in one screen). It is kept current with every
> release/issue change; this INDEX is the map, STATUS is the "you are here."

**CURRENT STATE (v1.0.31, 2026-08-05).** The 1.0 line is in active field
testing and every planned milestone through M10 has shipped: gambits execute
on both tables, the Field Orders board and MCM are live (four skins, full
controller parity), logistics loots/restocks/upgrades — up to four followers
on concurrent loot excursions (P7) — the follower economy trades at real
merchants, auto-retreat is default-on, and Rapport gates slots. The 1.0.x
patch line is field-fix driven; read `CHANGELOG.md` newest-first for what
changed and why.

**Proven in-game** (`ENGINE_NOTES.md` §0 carries dates and observed symptoms):
everything above, plus the claim model the walk-to behaviours ride on — alias
fill at static priority 60 claims an actor, release is by EVICTION, and as of
v1.0.25/26 the evicting ref is a session-minted non-actor XMarker, never the
player (a player latched into a package-carrying alias breaks furniture; the
v1.0.26 load sweep un-latches saves that predate the fix).

**The headline finding stands: MFO does not cast.** It puts a spell in the
follower's hand and their own AI casts it — animated, magicka-charged,
correctly aimed, because it is the vanilla path (§0.15); where the AI declines
a commanded action, the M9 ACTUATION LAYER (a package *is* the action) owns
the action, never the follower. Three cast *verbs* were refuted getting there:
`CastSpellImmediate` (§0.8/§0.10), `Projectile::LaunchSpell` (#56), and
`DoCombatSpellApply` (§0.14).

**Next: town errands (#31)** — followers autonomously walking to merchants and
doors on their own business, generalising the loot-travel machinery
(`Packages.*` + `Logistics.*`). Deferred but designed: vocabulary tiering by
Rapport rank (ROADMAP), and the leveling-mod interop that is blocked on the
other mod's missing API.

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
11. **ADDON-API.md** (when touching #74 progression / the ESL seam) — the
   FROZEN public contract for third-party progression addons: the
   `MFO_AddonManifest` sentinel, the manifest FLST layout, N-declared classes,
   and suffix-matched economy GLOBs. `FOLLOWER-PROGRESSION-ESL-DESIGN.md` §18 is
   the design rationale; ADDON-API.md is the authored contract a third party
   builds against. `MFO_Progression.esl` is its worked example.
12. **TOOLING.md** (before touching the build pipeline) — the consolidated
   Linux-native toolchain end to end: Papyrus compile (`tools/compile.sh`), the
   MCM-Helper config pattern (empty `extends MCM_ConfigBase` shim + QUST VMAD +
   `config.json` + SEQ; GlobalValue vs ModSettingFloat binding; the DLL
   live-reads on menu close), the ESP/ESL generator (`MFO_GenerateESP.py`,
   FormID bands / master-index rules, record helpers, the addon-manifest seam),
   the audits (`audit_esp.py` / `audit_mcm.py`) as merge gates, and the two-deck
   deploy flow.
13. **BOARD-EXTENSION-API-DESIGN.md** (SCOPING, not built) — how a third party
   could add their own tabs/features to MFO's in-game ImGui board: Tier 1
   declarative ESL/JSON panels (manifest + GlobalValue/Papyrus binding) and
   Tier 2 native companion-DLL tabs (`MEO_API.h`-style versioned interface +
   stable C draw shim). Frozen into ADDON-API.md when a tier is built.

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
