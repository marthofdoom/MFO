# MFO — Roadmap to a functional build

`DESIGN.md` §10 lists the phase *gates*. This is the build order: what to
write, in what sequence, and which unknowns could reorder it.

**Two targets worth naming separately, because they are far apart in effort
and close together in value:**

| Target | Milestone | What it means |
|---|---|---|
| **First playable** | **M5** | Followers execute gambits. Rules seeded from console — no UI. Proves the entire thesis. |
| **First shareable** | **M7** | A human can author rules in-game on keyboard or pad. This is the first build worth showing anyone. |

Everything after M7 is depth, not viability.

---

## Read `BUILD.md` first

It carries the per-build checklist, and the one rule this project has broken
hardest: **a milestone is tested in-game before the next is written.** Five
releases were cut on a single test session, which is why defects accumulated
across builds rather than bisecting to one.

## The gate that applies to every milestone below

**A Fable code review runs before any substantive commit reaches CI**
(`INVARIANTS.md` #45a). This is a standing family rule — MAO tags builds
"Fable-reviewed", MEO requires one for its tally-cap rework.

It matters more here than in the siblings: **there is no local compiler.**
MSVC and CommonLibSSE-NG live only on the CI runner, so CI is the first
compiler to see any of this and an in-game session is the first runtime. The
review is the only check that happens before both.

Applies to: new subsystems, engine-facing code, anything touching the co-save
or threading. Not doc-only commits.

---

## M0 — CI green *(BLOCKING EVERYTHING)*

**Status: in progress.** First cold vcpkg build was still in `Configure` at
26 min; vcpkg compiles CommonLibSSE-NG from source at cmake-configure time.
Warm runs hit the archive cache at ~2 min.

Nothing in `native/` has ever been compiled, so expect a round of errors.
Likely suspects, in order: `SKSE::PluginDeclaration` availability, the
`SKSEPluginLoad` macro's expectations, `ReadRecordData` overload resolution
on the string path, `std::clamp` template deduction on `uint8_t`.

**Gate:** `gh run download <id> -n MFO-dll` produces a DLL.

---

## M1 — P0 proof: the co-save round-trips *(DEFERRED — marth, 2026-07-21)*

**Not being tested yet: no saving with MFO active until further along.** Safe,
because the co-save callbacks only fire on save/load — with seeding off and no
saving, MFO writes nothing.

**The carried risk:** the co-save is the highest-blast-radius subsystem here,
it has been Fable-reviewed but never *executed*, and every milestone from M5
adds per-follower state that depends on it. A schema defect gets more
expensive to find with each one. **Un-defer before M5**, on a save file made
solely to be thrown away.

The steps below stand for whenever that happens. No ESP required — the P0 seed is code-side, so the DLL installs alone.

1. Deploy to a test profile; confirm `skse64.log` shows it loading and
   `MFO.log` prints the version header.
2. New game → seed fires → save → reload → **both tables come back intact.**
3. **Load-order change test**: add or remove a plugin between save and load,
   confirm `ResolveFormID` drops cleanly rather than mis-keying.
4. Downgrade test: bump `kSchemaVersion`, save, revert the DLL, confirm the
   loud newer-save warning fires.

**Gate:** `DESIGN.md` §10 P0. This is the first real evidence the mod exists.

---

## M2 — The ESP generator

`MFO_GenerateESP.py`, copying MEO's record primitives (`subrec`, `record`,
`group`, `zstr`, `prop_obj`, `make_tes4`). Records needed:

| Form | Purpose |
|---|---|
| `0x800/0x801` | Field Orders MGEF + SPEL (**SPIT type 3, lesser power**) |
| `0x802` | shipped as a granted-spell keyword; now RESERVED and unused (DESIGN 5.4) |
| `0x804` | startup QUST + `SEQ/MFO.seq` |
| `0x808` | MCM QUST |

Plus `data/mfo_forms.frozen.json` (the freeze anchor, tripping on **both**
drift and shrink) and `tools/audit_esp.py` as a merge gate.

**Do not build the command quest / alias pool / packages yet** — those are
Tier B (M9) and the band is already reserved for them.

**Gate:** `audit_esp.py` PASS; `help MFO_ 0` finds the forms in game.

---

## M3 — Follower detection + Rapport *(P1)*

- `ProcessLists::highActorHandles` teammate sweep, held by `ActorHandle`.
- **The quirk table** (`data/follower_quirks.json`) — Inigo's
  `WaitingForPlayer == -1`, Vilja's and Tindra's factions, pet frameworks.
  Every entry resolves against the live load order and is skipped silently
  when its plugin is absent.
- Blocklist by actor **and** ActorBase.
- `TESDeathEvent` (act on `dead == true` only) and `TESCombatEvent` sinks.
- Rapport accrual, ranks, `BALANCE.md` thresholds.

**Gate:** detection survives dismissal, death, cell change, and a follower
framework installed. **Also measures real kills/hour** — the number the
entire Rapport ladder rests on (`BALANCE.md` §7).

---

## M4 — The probe harness *(BUILT, unrun)*

Became a **Probe tab in the Field Kit** rather than console commands, once the
overlay existed. Pick a follower, press a primitive, watch what the engine
actually does. Nothing persists.

**It already produced its most valuable result before running.** A Fable
review found that **five of the twelve primitives `DESIGN.md` §4.5 lists as
Tier B have no C++ binding in CommonLibSSE-NG**: `KeepOffsetFromActor`,
`ClearKeepOffsetFromActor`, `SetDontMove` and `DoCombatSpellApply` are
Papyrus-only, and `StartCombat` exists only in po3's fork. See §4.5aa — the
research was sound, but *"a Papyrus native exists"* and *"I can call it from
C++"* are different claims and only the first had been checked.

**Ships only what the pinned library verifiably binds**, plus `StartCombat`
via po3's published relocation ID. The unreachable primitives are shown in
the UI rather than omitted, because a probe that hides what it cannot reach
hides its most important finding.

| Question | Status |
|---|---|
| **Does a commanded target STICK, or does the controller re-pick?** | **Answerable now** — the retention watch. §4.7's whole model depends on it |
| Does `EvaluatePackage()` no-op when the same package would be chosen? | Answerable now |
| Does `CastSpellImmediate` read as a real combat action? | Answerable now |
| Does `KeepOffsetFromActor` fight the combat controller? | **Blocked** — no binding; needs VM dispatch (its own milestone) |
| Does an alias `ForceRefTo` drive a conditioned package? | **Blocked** — needs the command QUST and alias pool, which are unbuilt |

**Run this BEFORE the evaluator.** Everything learned goes into
`ENGINE_NOTES.md` §9 with a date and an observed symptom, and into
Linux-Native-Tools per the standing debt.

---

## M5 — Evaluator + Tier A *(P2)* — **FIRST PLAYABLE**

- Scheduler: frame clock off the present hook with a `chrono` fallback,
  `max(4 frames, 133 ms)`, round-robin with K, jitter, **no catch-up**.
- Evaluator: snapshot → top-down scan → first match. Lazy world snapshot.
- Tier A actuation: cast, drink (H/S/M), equip swap.
- Suppression: per-action duration, **positional**.
- Target latch (§4.7) — if M4 said targets stick.
- `bProfileEvaluator` timing, split by phase.

**Gate — a hard one:** budget met in a Lorerim-class order, real
multi-follower fight, worst-case all-expensive rule list. Per-tick cost flat
1→12 followers. Tick rate holds ~7.5/s across 30/60/144 fps. No burst after
a load screen. **Behavior diffed against an MFO-absent baseline save** to
prove the do-nothing guarantee.

At this point the mod works. It just has no interface.

---

## M6 — Logistics table *(§4.8)*

Deliberately before the board, because it is console-testable and its
mechanisms are independent.

- Idle-tick evaluation, never interleaved with combat.
- Loot H/S/M potions, ammo, better equipment (generalized by category and
  metric, weighted by the follower's own skills).
- **First dibs**: 25 s delay, collapsing to 4 s on a player *take*, timer
  reset per take. Detection via `TESContainerChangedEvent` filtered to items
  entering the player — **not** menu-close, which QuickLoot never fires.
- Ownership absolute; never mutate a container whose menu is open.

**Gate:** a follower tops up arrows and potions from bodies at their feet,
never takes owned goods, never beats the player to a fresh corpse, and never
snatches mid-QuickLoot.

---

## M7 — The board *(P3)* — **FIRST SHAREABLE**

The largest single chunk, and mostly transcription: MEO's `menuhook`
namespace is ~900 lines and MFO re-skins it.

- Three trampoline hooks, `AllocTrampoline(256)`, installed first in
  `SKSEPluginLoad`.
- Two-pane layout: roster | rule list. Per-pane draw lists.
- **`IsItemActivated()` single-shot** — the year-long MEO bug; here it
  deletes two rules per click.
- **Selection keyed on rule id**, never index — matters more here than in
  MEO because reorder is a primary action.
- Reorder via `▲▼` and **L1/R1** (MEO translates them already and doesn't
  use them). No drag.
- Controller parity in the same milestone, not after.
- **The four skins in the same milestone, not after** (DESIGN §6.7a) —
  Ebony & Brass / Dwemer Parchment / Soul Cairn / Quicksilver, `iMenuStyle`
  0..3, palettes copied VERBATIM from MEO. Branding, same standing rule as
  controller support.
- Both tables reachable; cost tier and failure reason shown per rule.

**Gate:** every action including reorder reachable on a pad with no keyboard;
no double-fire under a task-pump race.

---

## M8 — *(removed: tutoring is out of scope)*

Spell acquisition belongs to *A Fun Way To Level Followers*, which already
does it well and is open source. MFO reads what a follower knows and decides
when to use it. See `DESIGN.md` §5.4 — the deletion removed the largest
remaining surface in MFO that could damage another mod's state.

---

## M9 — The actuation layer: a package IS the action *(RULED 2026-07-22)*

**Scope changed.** This is no longer the casting fix — it is how EVERY
single-action gambit executes: cast, attack, move, hold position, use item.
See DESIGN.md §4.5c for the ruling and the doctrine reasoning.

**Fully researched, not started.** ENGINE_NOTES §0.17 records the mechanism read
from ALYSLC's source. This is the thing that makes a follower cast **on
command** with animation, rather than only when their own AI agrees the spell is
worth casting (§0.16).

FormIDs were reserved for this at project start: `0x80A-0x80F` (command QUST +
alias pool + globals), `0x820+` (MFO's own conditioned PACKAGEs).

> **SUPERSEDED IN PART — read ENGINE_NOTES §0.18 first.** The steps below
> predate the record survey. There is **no GLOB**: the alias carries the package
> via `ALPC` and filling it changes the candidate set, so no condition flicker
> is needed. And `packageStackMap` / `kCombatOverride` do not exist at the
> pinned CommonLib rev — that was ALYSLC's own map into FormLists they ship.

### The records to generate

Python, no xEdit — the family's standard practice, precedented across every
sibling project.

| Record | Contents |
|---|---|
| **GLOB** | `MFO_CastFlag` — the condition variable the package tests. Short, value 0. |
| **PACK** | A ranged/cast package: `PKDT` (type + general flags), `PSDT` (schedule), `PLDT` (target = the alias), condition on the GLOB `== 1`, package data for the cast. |
| **QUST** | Command quest, `Start Game Enabled`, with an **alias pool** — the mechanism by which a package reaches a follower MFO does not own. |

### The runtime, once the records exist

1. Fill an alias with the follower.
2. Set `MFO_CastFlag = 1`.
3. Write the package into **both** stacks — `kDefault` and `kCombatOverride` —
   and clear the current scene (scene packages override both).
4. `EvaluatePackage()`, **only when the package actually differs** (§0.7 proved
   it no-ops otherwise; ALYSLC guards the same way).
5. If the caster wedges at `State::kUnk01`, nudge with `RequestCastImpl()` —
   their restart trick, not a trigger.
6. Clear the flag and restore the package when the rule stops wanting it.

### Why it is worth the size

§4.5a calls package overrides a last resort, and that stands — this is the
loudest thing MFO can do to another mod's follower, so it goes behind a flag and
gets removed the instant the rule stops firing. But it is the ONLY route to a
commanded animated cast, and every alternative is now refuted in writing:
`CastSpellImmediate` (§0.8/§0.10), `LaunchSpell` (#56), `DoCombatSpellApply`
(§0.14), animation events (§0.17 — the graph emits them, nothing sends them),
and driving the caster alone (§0.17 — wedges at state 1 without a package).

---

## M10+ — Tier B, one mechanism per release

In reversibility order, per §4.5: reversible state sets → combat-state calls
→ conditioned packages → package overrides last. Each its own release, each
instrumented, each written up on landing. **Anything that cannot cleanly
release the AI is dropped, not shipped half-working.**

Requires the command QUST, alias pool (8), globals, and MFO's own conditioned
packages — all band-reserved, none built.

---

## Loot Option A — engine-pathed acquisition *(spec, 2026-07-28)*

**The method** (prior art: *Followers Can Loot*, Nexus 4744 — teardown in
`ENGINE_NOTES §0.32`; we use the METHOD, never its code). The follower walking
to a world item and grabbing it is done by a native **AI package** (Sandbox/
Find "acquire"), not by script and not by our tick. The engine owns the navmesh
pathing, the bend-down animation, and the pickup. This is the correct primitive
for marth's requirement *"the follower should have to GO to the item to loot
it,"* and it is the ONLY safe way to pick up a loose world ref — `PickUpObject`
from our BSJobs-worker tick is the crash4 class and `SKSE AddTask` does NOT
escape the worker (`§0.30`, `§0.32`). This REPLACES both today's arm's-reach
inventory transfer (v0.7.x, corpses/containers only) for the *loose-item* case
and the loose-item pickup that was cut from the tick loop.

**Why it is small: the infrastructure already exists.** M9's `native/Packages.*`
is built and proven — `MFO_CommandQuest` (0x80A, priority 60, in the SEQ), alias
fill via `ForceRefTo` through the VM, the `Pump()` observe-only lifecycle
(Idle→Requested→Filled→Running→Done), priority-60 arbitration, and the four hard
constraints (never write `refAliasMap`/`currentPackage`/`defaultPackList`; never
`EvaluatePackage(_, true)` in combat). The verb-package band `0x821+` is already
reserved for exactly this. Option A is "author one more package record and drive
it through the machinery that already works," not new infrastructure.

**The records to add (ESP, via the generator):**
- A **loot/acquire package** at a reserved `0x82x` slot. Two shapes to prototype
  and pick between (decide in the field, per §4.5's reversibility order):
  - *Acquire-by-type* (FCL's shape): a Sandbox/Find package with an item-type
    filter + radius; the engine picks its own nearby target. Simplest; but the
    ENGINE chooses the ref, so our `GetOwner`/`IsOffLimits`/`IsLocked` legality
    gate cannot pre-screen it — **must verify the vanilla Acquire procedure
    honors ownership/crime before trusting it** (open question).
  - *Travel-to-ref* (gambit-precise): add a second alias `MFO_LootTargetRef`;
    C++ picks the exact loose ref (the existing read-only cell walk already
    finds and legality-gates candidates), fills follower→alias0 + ref→alias1,
    package travels to alias1. Keeps our legality gate authoritative; costs a
    "then acquire/activate on arrival" step the template may not express cleanly.
- Reuse `MFO_CommandQuest`; no new quest.

**The runtime:** when a `loot_*` gambit is the winning logistics rule AND a legal
loose candidate exists, request the fill (category → package/condition, the way
today's dispatcher maps category → transfer fn); `Pump()` drives it to Running;
release the alias on pickup, on interrupt, or on combat start (clean release is
mandatory — a loot package that cannot release is dropped, not shipped). Category
selection is our GAMBIT, not FCL's faction toggles — finer and conditional.

**Gate:** a follower breaks off, walks to a dropped arrow/potion within radius,
picks it up with animation, returns to follow — takes nothing owned, does it only
when a loot gambit fires, releases cleanly on a new fight, and never `PickUpObject`s
from our thread. Corpse/container transfer (today's path) stays as-is for the
inventory case; Option A adds the loose-world-item case the tick cannot do.

**Also fold in here** (cheap, from §0.32): don't loot while the PLAYER is
sneaking — a follower sprinting to grab loot blows the player's stealth. Read
player sneak state in the logistics gate (C++, no package needed).

---

## The three things most likely to reorder this

1. **M4 says commanded targets don't stick.** §4.7 becomes a refresh model.
   Cheap to find out, expensive to discover at M9 — which is exactly why M4
   sits where it does.
2. **M5's perf gate fails.** Most likely culprit is the world snapshot being
   built more often than the design assumes. Mitigations are already
   specified (interval extension, K, Tier-B suspension); the risk is that the
   *architecture* needs distance-based LOD, which Enhanced Combat AI ships
   and MFO currently lacks.
3. **CI never goes green cheaply.** If cold builds keep exceeding ~25 min,
   pin a narrower vcpkg baseline or vendor a prebuilt CommonLibSSE-NG.

## Next build — vocabulary tiering by rank *(marth-approved, deferred 2026-07-28)*

Today gambit TYPES are ungated — the board offers the whole vocabulary at
every rank; only slot COUNT scales with rapport. **Build the design intent
(DESIGN §): each condition/action carries a minimum rank, and the board's
picker (cycleIdx/labelFor over kConds*/kActs*) only offers entries the
follower has earned.** Tiers are already assigned in `GAMBIT_LIBRARY.md`
(Basic / Intermediate = Rank II–III / Advanced = Rank IV–V). The existing
`bDebugUnlockSlots` debug flag should ALSO unlock the full vocabulary when
set (rename to convey "unlock everything"). Watch: a co-saved rule using a
now-locked opcode must still LOAD and RUN (don't strip authored rules on a
rank drop) — the gate is only on ASSIGNING new ones in the picker. Deferred
so marth can confirm v0.7.7 (MCM/looting/gambits) works in the field first.

## Compatibility backlog *(not scheduled)*

- **"NPCs use potions" (and kin).** These give followers automatic potion-
  drinking, which hands the player a free gambit — the follower heals without
  the player ever setting a drink rule, undercutting the point of the gambit
  board. MFO should SUPPRESS that behaviour for followers it manages (so
  drinking only happens because a gambit says so), with an **MCM toggle to
  re-enable** it for players who prefer the mod's own drinking. Mechanism TBD
  (detect the mod; likely strip/gate whatever perk/package/AI-flag it grants a
  managed follower). marth, 2026-07-28. Not today.

## What is deliberately NOT on this roadmap

No installer, no patch plugin, no calibration pass, no leveled-list edits,
no runtime Papyrus. MFO bakes nothing from a load order — its entire data
surface is the actor in front of you, which is why it needs none of the
machinery MEO and MAO required.
