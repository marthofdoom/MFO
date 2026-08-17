# Follower Progression ESL — Design + Feasibility

**Status: DESIGN (no code). Target: the update after v1.0.61. Supersedes the town update as "next" per marth 2026-08-11.**

An optional, separately-shipped ESL addon (`MFO_Progression.esl`), detected at runtime, adding: (1) real follower leveling — skill AVs + player-selectable perks from the load order's *actual* perk trees; (2) vanilla-style roster recall; (3) a gated Field Orders "Progression" tab; (4) MCM detection indicator. All heavy machinery lives in the DLL; the ESL is a switch plus a version stamp.

**Feasibility verdict: GREEN.** Every risky engine unknown verified present in the CommonLibSSE-NG headers; the one surprise (`Actor::AddPerk` is a no-op on NPCs) has a proven alternative — `TESNPC::AddPerk` + `Actor::ApplyPerksFromBase()` (the SPID pattern), already running in marth's LoreRim via "Requiem - SPID Apprentice and Novice Perks for All Followers." The build is DLL-heavy, records-minimal, save-safe by construction, and sits entirely on existing MFO machinery (Board tabs, EditCmd queue, MainThread::Post, co-save records, §4.6, #65 class override).

---

## 1. Architecture and component split

```
MFO_Progression.esl        (generated, separate file; ~1-3 records; the feature switch)
        │ detected at kDataLoaded
        ▼
native/Progression.cpp/.h  (new module)
  ├─ Catalog     — one-time runtime read of merged AVIF trees + PERK effectiveness (kDataLoaded)
  ├─ Allocator   — points economy, gate enforcement, apply/reapply/respec (main thread only)
  ├─ Roster      — recall validation + MoveTo + standard-path recruit
  └─ Snapshot    — value-only views for the board (existing Board discipline)
native/Board.cpp           — third tab "Progression", gated; kTabCount becomes runtime
native/Serialization.cpp   — new independent co-save record 'PRGN' (MSTK precedent)
MFO_GenerateESP.py         — second-file emitter for the .esl
MCM                        — Addons section, Detected line
```

**Detection:** `TESDataHandler::LookupLoadedLightModByName("MFO_Progression.esl")` at kDataLoaded + resolution of an `MFOP_Version` GLOB via a `Forms::Look`-style helper with a second plugin-name constant. Absent = feature off with one named log line, never an error (Forms failure doctrine). Plugin-file presence (not the MEO DLL↔DLL messaging handshake) is the right signal for a data addon.

**Threading:** every engine mutation (AddPerk, SetBaseActorValue, MoveTo, VM calls) goes through `MainThread::Post` — never `AddTask` (job worker). Catalog build runs at kDataLoaded (already main thread).

---

## 2. Runtime AVIF/PERK reading — VERIFIED FEASIBLE

### 2.1 The tree graph (`ActorValueInfo.h`, `BGSSkillPerkTreeNode.h`)
`ActorValueInfo::perkTree` → root `BGSSkillPerkTreeNode`; each node exposes `perk` (PNAM, null on root), `children`/`parents` (directed + reverse edges), `perkGridX/Y` (integer grid for nav), `horizontalPosition/verticalPosition` (float dome coords for layout), `associatedSkill`. Skill AVIF via `ActorValueList::GetActorValue(av)` for the 18 skills. Live merged forms → Requiem/Ordinator/Vokrii replacements + any Synthesis output are seen automatically. Zero Synthesis dependency.

Note: the skill-level requirement is NOT on the node — it lives on the PERK as conditions (§2.2). Tree gives prereq edges; perk gives the skill gate. Both are in the runtime read.

### 2.2 PERK introspection (`BGSPerk.h`, entry + entry-point headers, `TESCondition.h`)
`BGSPerk`: `data` (numRanks/playable/hidden), `perkConditions` (the skill gate, CTDA), `perkEntries`, `nextPerk` (rank chain). Entries polymorphic via `GetType()` → kQuest/kAbility/kEntryPoint; `BGSEntryPointPerkEntry` carries `entryPoint` (one of 92 ENTRY_POINT values) + per-entry conditions; `BGSAbilityPerkEntry` carries an ability spell. Conditions walk `TESCondition::head` → items exposing function id, params, comparison value/global, opcode, object (kSelf/kTarget). `TESCondition::IsTrue(actionRef, targetRef)` evaluates.

### 2.3 The Catalog
Built once at kDataLoaded, main thread, then **frozen** (immutable → render thread reads lock-free; per-follower *state* still crosses via Snapshot). Per skill: BFS from root; per node with a perk, emit value-only `PerkNodeView { perkFormID, name, desc, gridX/Y, hpos/vpos, parentIndices[], ranks[] }`, ranks[] walking `nextPerk` recording `{ formID, extracted skill req, effectiveness verdict }`. Skill-req extraction for display: condition items `function == kGetBaseActorValue` on kSelf with opcode `>=`. A few hundred nodes total, kilobytes.

---

## 3. NPC-effectiveness classification (the dead-perk filter)

Same catalog pass. A perk rank is **effective** iff ≥1 entry is effective; perks with zero effective ranks never reach the board.

- **kQuest** → dead (script/player-mechanic drivers; Ordinator's script perks land here).
- **kAbility** → effective by default (constant ability on the owner), unless the ability's effects are player-gated.
- **kEntryPoint** → allow/deny on the entry point:
  - **Effective (combat/defense):** weapon-damage, attack/power-attack/bash damage, incoming damage/stagger, target stagger, damage/spell resistance, spell magnitude/duration/cost, ward absorption, block %, armor rating, apply-*-spell (combat hit/bashing/weapon-swing/reanimate/sneaking), recovered health, sneak-attack mult, detection, sweep attack, falling damage.
  - **Dead (player UI/mechanics):** all lockpicking/pickpocketing, all crafting/harvest (tempering/enchanting/alchemy/soul gems), commerce/social (prices, intimidation, reputation, favors, bribes), player-camera/UI (bow zoom, activate labels, book skill points, shout OK, magic slowdown).
  - **Marginal (MCM "show marginal", default hidden):** max carry weight, arrow recovery, dual-cast, mount, commanded-actor limit.
- **Player-gate condition scan** (heuristic backstop): an AND-required item testing `kGetIsID`/`kGetIsReference` against the player marks the entry dead. Primary filter is structural; §5's evaluate-on-the-follower catches most player-gated perks for free.

MCM-gated `[prog]` dump lists every filtered perk + reason, so over-filtering under a new overhaul is diagnosable from a deck log.

---

## 4. Applying perks and skills to a follower

### 4.1 Perks: the SPID pattern
**Verified:** `Actor::AddPerk/RemovePerk` are base no-ops (only PlayerCharacter implements them). NPC perks live on the base — `TESNPC` inherits a perk-rank array; `Actor::ApplyPerksFromBase()` applies them to the loaded actor. CommonLib provides `TESNPC::AddPerk(perk, rank)`/`RemovePerk`/`GetPerkIndex`.

**Apply (main thread):** `GetActorBase()->AddPerk(perk, rank)` → `ApplyPerksFromBase()`. Same path SPID uses; LoreRim ships Requiem SPID follower perks — live proof entry points fire on followers in marth's environment.

**Persistence:** treat base mutations as runtime-only; the co-save allocation record (§8) is the source of truth, reapplied **idempotently at kPostLoadGame** (guard `GetPerkIndex` before AddPerk; only `ApplyPerksFromBase` on change) — the fix-forward-proof shape (nothing engine-serialized to sweep).

**Respec:** `TESNPC::RemovePerk` on the base, retract applied entries on the actor, re-settle via `ApplyPerksFromBase()`.

**Shared-base guard:** base-array edits hit every actor sharing that TESNPC. v1 restricts enrollment to unique-flagged bases (housecarls, named/custom followers); generic hirelings show "not eligible (shared template)."

### 4.2 Skill AVs: base-delta with reconciliation
Perk gates condition on GetBaseActorValue, so allocation must raise the **base**. But NPC base skills are autocalc-derived and can be recomputed on level-up. Design: co-save per skill `{ allocPoints, lastWrittenBase }`; reconcile at apply, kPostLoadGame, and on level change:
```
cur     = GetBaseActorValue(av)
natural = (cur == lastWrittenBase) ? lastWrittenBase - allocPoints : cur   // adopt engine recompute
desired = min(natural + allocPoints, 100)
SetBaseActorValue(av, desired); lastWrittenBase = desired
```
Idempotent, converges, survives static-stat (Requiem) and PC-level-mult growth. Attributes (Health/Magicka/Stamina) allocatable via the same path.

---

## 5. Requirement enforcement — identical to the player (marth, load-bearing)

A follower may take a perk **only** when it meets both gates the player would face. No free-picking.

1. **Prerequisite perk(s):** node `parents` edges — reachable iff a child of root OR ≥1 parent perk owned (rank ≥1); rank N+1 requires rank N in order (never skipping).
2. **Skill-level (+ anything else the overhaul conditions on):** evaluate the rank's `perkConditions.IsTrue(follower, follower)` — the same record the player's skill menu gates on, evaluated against the follower → player-identical semantics generically, including overhaul-added conditions. (Side benefit: hard player-gated perks evaluate false forever — defense in depth over §3.)

**Enforced in two places:**
- **Board (UI):** each node renders owned / available / **locked** (greyed, non-activatable, shows the unmet requirement). Locked nodes can't be selected by mouse or pad; A shows the requirement, never a confirm.
- **Allocator (backend):** UI never mutates — it queues an `AllocPerk` EditCmd; `ApplyEdits()` on the main thread re-validates everything against live state (addon detected, follower enrolled+eligible, perk in catalog+effective, points sufficient, prereq via HasPerk, `perkConditions.IsTrue` at apply time). Any failure → reject + named log line, no mutation. A stale/bypassed UI cannot over-allocate.

---

## 6. Leveling economy (proposal — MCM-tunable)

- **XP source: level-with-player.** Each player level: active followers +1 progression level. **Shared Growth** (toggle, default ON): retained followers +1 per 2 player levels. Deterministic, save-safe, no combat hooks, overhaul-agnostic. (Own-kill XP rejected for v1.)
- **Per level:** +3 skill points (1 pt = +1 base AV, cap 100), +1 perk point.
- **Veteran catch-up** (toggle, default ON): first enrollment grants `floor(level/2)` skill + `floor(level/5)` perk points (one-shot, co-saved).
- **Respec:** board action, hold-to-confirm; refunds points, removes perks, zeroes AV allocs. Free by default; optional MCM "costs gold" (gold via GetInventory).
- **Class auto-allocation** (ties to #65 combatClassOverride): when a class is set + "auto-spend" on, points spend on level-up. Weights — **Melee:** dominant weapon 40% / dominant armor 30% / Block 20% (+Health/Stamina). **Ranged:** Marksman 45% / LightArmor 25% / Sneak 20% (+Stamina/Health). **Mage:** Destruction 35% / Alteration 20% / Restoration 20% / Conjuration 15% (+Magicka/Health). Auto (class 0) infers from loadout. Perk auto-pick deterministic + name-agnostic: highest-weight eligible effective perk; tiebreak lowest skill req, then lowest BFS depth. No hardcoded perk EDIDs.

---

## 7. Board UI — the Progression tab

Third `BeginTabItem("Progression")`, emitted only when detected; `kTabCount` becomes runtime so the tab cycler stays correct. Layout: follower header (reuse L1/R1 cycling) → skill strip (18 skills, AV + allocated + unspent) → tree canvas (scrollable child, ImDrawList `AddLine` edges bright when both ends owned, `AddCircleFilled`+label per node at scaled float positions, root at bottom) → node detail panel (name/desc/ranks/requirements/cost/Take) → roster section (retained followers + availability + Recall).

**Controller (standing family rule):** all input via the existing InputDispatchHook — no new paths. D-pad = spatial node nav via grid coords; A = select/confirm (listPopup pattern); B = cancel cascade single-path (detail → tree → tab → close); X = respec/auto-spend; Y = next skill; L1/R1 = follower. Vendored imgui backend has gamepad polling disabled (v1.0.59) → B stays single-path.

**Threading:** catalog immutable (lock-free); mutable state crosses as plain values in Snapshot; all mutations via new EditKinds drained in ApplyEdits.

---

## 8. Co-save schema

New **independent** record `'PRGN'` v1 alongside FLWR/MSTK under `'MFO0'` (older DLLs skip unknown record; newer PRGN version → skip that record only). All FormIDs via `ResolveFormID`; unresolvable perk → drop + refund. Per follower: formID, flags (enrolled / wasInPotentialFollowerFaction / autoSpend / veteranGrantConsumed), progressionLevel, sharedGrowthRemainder, unspentSkill, unspentPerk, skillAlloc[]{avId, points, lastWrittenBase}, perkAlloc[]{perkFormID, rank}, enrollBaseline[]{avId, f32} (respec floor). New fields behind `if (version >= N)`. Runtime-only 0xFF FormIDs excluded. Perks/AVs live natively on the actor between saves; the co-save exists for reapply, UI, and respec.

---

## 9. Roster recall — safety design

Roster = retained followers minus active. Dismissal = standard path. No storage, no benches, no player-touching aliases → the furniture-ejection class of bug is structurally impossible.

**Availability (at draw AND re-validated at recall):**
1. `LookupByID<Actor>` unresolvable → unavailable.
2. IsDead/IsDisabled/IsDeleted/bleedout → unavailable ("fallen").
3. **Primary:** `IsInFaction(PotentialFollowerFaction 0x0005C84D)` — not in it and not a teammate → **"away/unavailable," never MoveTo.**
4. **§4.6 guard:** `ForeignOwnerBlocks()` — foreign quest at ≥ MFO priority → "busy" (covers scenes; never drag out mid-quest).
5. **Provenance (new):** co-save flag `wasInPotentialFollowerFaction` at enrollment → distinguishes "away on their own business" (recoverable) from "managed by their own mod — use their dialogue" (permanent; correct for Inigo/Lucien — byte-scan found zero PotentialFollowerFaction refs, their systems are fully custom). MFO logs faction state at enroll/dismiss → the framework-behavior question answers itself from deck logs.

**Recall flow (main thread):** re-validate → swap dismisses current via standard path → entrance = nearest loaded door ref within ~2000u, out of view frustum if possible (`MoveTo(door)`), else `MoveTo(player)` behind camera → recruit via **standard path** VM `DialogueFollowerScript.SetFollower(actor)` on quest 0x750BA (NFF hooks the same quest, so it lands in whatever the standard path is). VM failure → log + abort (never a manual half-recruit).

---

## 10. ESL record footprint + generator

Minimal — the ESL is a switch:

| FormID | Type | EDID | Purpose |
|---|---|---|---|
| 0x800 | GLOB | `MFOP_Version` | Detection anchor + addon version stamp |
| 0x801 | GLOB | `MFOP_Reserved` | Spare future gate |

Master: Skyrim.esm only. No recall markers, storage, packages, or quest — everything else is DLL. GLOB *values* are save-persisted, so the DLL reads the record default at kDataLoaded, never trusts a saved value. Generator: add a second output profile (own `make_tes4()` flags 0x200, local IDs 0x800-0xFFF) → `out/MFO_Progression.esl`; `audit_esp.py` gains a `--plugin` profile. No SEQ (no start-enabled quests).

---

## 11. MCM Detected / Not-Detected

Board tab presence = primary indicator. MCM line: add an **Addons** section, text bound to `bProgressionDetected:Addons` (seeded false), and at kDataLoaded a one-shot VM call to MCM Helper's `SetModSettingBool("MFO", "bProgressionDetected:Addons", ...)`. Fallback if brittle: static text "Progression addon: see Field Orders board." Diff against installed LoreRim configs before shipping. Addons section also holds XP sliders, Shared Growth, Veteran catch-up, respec cost, show-marginal, strict/lenient filter, framework-leveling warning.

---

## 12. Framework interop

- **Framework followers (Inigo/Lucien/Kaidan):** perks SAFE (inert base combat-stat adds, no packages/aliases touched); recall OFF (provenance → "use their dialogue"); AV allocation default OFF, per-follower toggle with warning. Enrollment is explicit per follower — nothing happens by default.
- **Manager frameworks (NFF/AFT/Nether's):** coexist-and-defer; recall goes through the DialogueFollower VM path they hook (lands in their pipeline or fails loudly). Roster header notes management framework if detected.
- **Leveling mods (FLSR — interop blocked):** detect by name; default AV allocation OFF globally (perks stay on), MCM warning + override. Re-check FLSR API on updates.

---

## 13. Ranked risks + field probes

| # | Risk | Severity | Mitigation / probe |
|---|---|---|---|
| 1 | Entry-point variance on NPCs per overhaul (filter wrong for Requiem/Ordinator edge perks) | High (quality) | Structural filter + condition backstop + `[prog]` dump. **P1:** LoreRim — grant Armsman/Augmented perks, measure damage delta in logs. |
| 2 | SetBaseActorValue vs autocalc/level-up | High | §4.2 reconcile. **P2:** level player ×2 with a PC-mult follower; verify base = natural+alloc. |
| 3 | Reapply idempotency (double-applied ability entries) | Med-high | GetPerkIndex guard; reapply on delta. **P3:** save/load ×3 with an ability perk; count effects. |
| 4 | PotentialFollowerFaction fidelity across frameworks | Medium | Provenance flag + never-MoveTo default. **P4:** enroll-time faction logging → deck-log data. |
| 5 | Respec completeness (entry retraction residue) | Medium | §4.1 removal. **P5:** allocate→respec→verify HasPerk false + effects gone. |
| 6 | DialogueFollower VM under NFF/vanilla | Medium | Standard-path-only + loud abort. **P6:** recall on Tuxborn + an NFF list. |
| 7 | Shared-base/templated followers (perk leak) | Med (contained) | v1 unique-base restriction. |
| 8 | Requiem conditions over-filtering legit combat perks | Low-med | Lenient-filter toggle + P1 dump review. |
| 9 | Generator second-file + audit profile regressions | Low | Mechanical; audit profile + CI-green rule. |
| 10 | Board perf/nav on dense trees | Low | Few hundred nodes; existing render discipline. |

**Sinkers:** only #1-3 could sink pillars — all three have cheap log-only probes that should be the **first field build** (a probe-instrumented dev build before UI polish).

---

## 14. Fable additions (proposed, all optional/toggleable, in-ethos)

1. **Loadout-aware perk advisor** — tag perks the follower's current loadout exercises (bow user → Marksman glows). Runtime-only, high value/line.
2. **Auto-respec offer on class change** — flipping #65 prompts "Respec + auto-spend for <class>?"
3. **Shared Growth** (woven into §6) — retained followers grow at half rate; makes the roster strategic.
4. **Milestone rapport moments** — a capstone-depth perk grants a small rapport bump + notification via existing Rapport machinery.
5. **Trainer fees mode** (off by default) — points cost gold by rank; a sink using existing economy code.

Not proposed: training scenes, any Synthesis-side anything, any storage records.

---

## 15. DECISIONS LOCKED (marth, 2026-08-11) — override the §6 proposals above

- **Skill model: AUTO skills, MANUAL perks.** Skills are NOT a manual point pool. MFO auto-scales a follower's skills to their level **by class** (Requiem/LoreRim make NPC stats static → MFO drives it via the §4.2 base-AV path, not engine autocalc). Perks are the only manual allocation.
- **Class gate / onboarding.** A newly-enrolled follower gets **no skills assigned until the player explicitly picks a CLASS** for them. Selecting an unclassed follower on the Progression tab pops a **class-selection prompt**. On class pick → auto-skills scale to level + perk allocation unlocks. Before that, progression is inactive for that follower (skills stay vanilla). Concrete class (not "Auto"); aligns with the #65 combat-class override.
- **Perk earn rate = COPIES the player's** (1 perk/level, whatever the game grants the player).
- **Scarcity-scaled perks (load-bearing).** Perk points awarded to a follower are scaled by **(perks usable by a follower ÷ perks usable by the player)** so the follower can afford the *same percentage of their available tree* the player can of theirs — preserving felt scarcity. Applies to ongoing earning AND the late-recruit catch-up. Compute the ratio from the runtime catalog (follower-effective perk count vs full player pool).
- **Shared Growth:** default **ON** = retained/benched followers grow at **half** rate; toggle **OFF** = they match player level (full rate).
- **Mid-game recruit:** progression level = **player level**. **No** bonus/free skill points (auto-skills scale to level). **Not** perk-penalized for arriving late — gets the level-matched, scarcity-scaled perk points.
- **Respec:** free of gold, but **costs −500 rapport** (the follower resents the reset). Uses the existing Rapport machinery.
- **v1 scope:** unique-base followers only (confirmed).
- **Detection:** ESL presence is the signal (confirmed). Do it to the robustness bar of how **Precision detects Nemesis** and how **DynDOLOD surfaces dynamic info** — study those patterns for the detection + MCM "Detected" display (Fable, at build time).
- **The 5 proposed additions (§14):** not yet individually ruled on; carry as optional/toggleable.

---

## 16. Manual skill points (2026-08-12) — and the manual-override lens

**THE STANDING DESIGN LENS (marth — engrain this): every auto-behavior in the
progression system is a helpful DEFAULT, never a cage, and therefore needs a manual
override.** Class auto-scaling picks sensible skills for the archetype, but real
builds outgrow archetypes — **mage followers especially** (the Mage weights split
across five schools and can't know you're building a pure conjurer), and
**unexpected/desired multiclass builds** (a spellsword, a sneak-archer housecarl)
that no fixed weight table serves. When an auto-system and the player disagree, the
player wins. Apply this lens to every future auto-feature in this system: auto-spend
perks will need a manual picker (it has one — the tree), auto-skills need this
toggle, any future auto-class inference needs the explicit class prompt it already
has.

**The feature:** a per-follower **"Manual skill points" toggle** on the Progression
tab, **OFF by default** (auto-scaling stays the norm). ON = the follower **banks
skill points every progression level** into a visible pool; the player applies them
from the skill picker, **+1 base per point**, capped at the economy `skillCap`.
**REPLACES auto growth — never additive (marth, round-4 correction: stacking both
was incorrect and overpowered).** While the toggle is ON, the 5 points/level IS the
follower's ongoing skill progression: automatic per-level growth is SUPPRESSED,
frozen at the enable level. The one-time class enrollment baseline (auto-scale to
level at class pick) stays as the starting stats; only ongoing progression switches
auto↔manual. Toggling OFF resumes auto growth — but levels progressed manually are
excluded from the auto total forever (`manualExcludedLevels`), so toggle-cycling can
never double-dip in either direction. Manual points placed **survive class changes**
(the override outranks the default).

**Economy (marth, round 3):** **flat 5 skill points per level while the toggle is
ON** — no detection, no GLOB, a plain constant ("5 skill points per level when
manual mode is on"). One point = one base level. `MFOP_SkillPointsPerLevel` (0x803)
remains the AUTO-scale total only; the two are independent, and both are separate
from the §17 perk-point economy.

**Save-safety (the round-2 SEV-1 lesson, applied):** the pool is **never an
incremental accumulator** — it is a pure function of two serialized baselines:

```
available = (progressionLevel − manualBaselineLevel) × 5 − manualPointsApplied
autoLevel = (manual ? manualBaselineLevel : progressionLevel) − manualExcludedLevels
```

Each OFF→ON re-latches the baseline (a fresh stint, pool from zero); each ON→OFF
banks the stint's levels into `manualExcludedLevels`. All three fields change only
on toggle transitions — replay-safe, no per-tick accumulator anywhere.

`manualBaselineLevel` latches ONCE at first enable (0 = never enabled); after that
the toggle only gates spending — off/on cycling can neither farm nor forfeit points,
and any reload/replay recomputes the identical number. Applied points ride the SAME
single `SetBaseActorValue` call site as everything else (`ReconcileSkill`: target =
auto share + manual points, baseline floor, applied-delta recovery), so a manual
point can never push a skill below its enrollment natural and recovers exactly.
Spending refuses at the cap rather than silently absorbing into the clamp. Co-save:
PRGN **v2** — flags bit 0x10 (manual ON), `manualBaselineLevel`/`manualPointsApplied`
u16s, per-skill `manualPoints` f32 — version-guarded reads, value-validated on
ingestion, coherence-guarded (ON without a latched baseline loads as OFF).

---

## 17. Perk-point economy v2 + the legible tree (deck round 3, 2026-08-12)

**PERK POINTS — SUPERSEDES §15's "copies the player" rate (marth: it was far too
generous; scarcity is the point).** There is no stored pool, no per-level grant, no
scarcity ratio. The pool is **derived**, every time it is asked for:

```
unspent = max(0, floor(followerLevel / 3)
              − nativeTreePerksAtEnroll
              − ranks MFO has allocated)
```

- **1 point per 3 levels, past or present** — a level-30 follower has earned 10;
  the curve is level 10 → 3, level 25 → 8, level 50 → 16, minus the debits below.
- **Pre-trained perks count against the budget**: at ENROLLMENT, MFO counts the
  catalog-tree perk ranks the follower already owns (`nativeTreePerksAtEnroll`,
  captured once, serialized in PRGN v2). A level-30 recruit who came with 4 tree
  perks has 10 − 4 = 6 to spend — a heavily-perked custom follower doesn't get a
  double pile; racial/quest/passive perks outside the trees never count.
- **Clamped at 0** — an already-"ahead" follower simply has nothing to spend.
- **Refunds are automatic**: respec and any dropped/unresolvable alloc remove the
  debit, so the derived pool rises by exactly the ranks returned. No accumulator
  anywhere — idempotent across reloads by construction. `MFOP_PerkPointsPerLevel`
  (0x802) and the veteran multiplier (0x806) are unread; the records stay in the
  ESL for id stability.

**THE TREE (deck rounds 1–2 both failed on ImGui auto-nav; round 3 rebuilds):**

- **Explicit selection nav** — the canvas owns a selected-node index; d-pad/stick/
  arrows move it by deterministic nearest-in-direction over the LAYOUT (forward
  projection + lateral penalty), so culled off-screen nodes are reachable and the
  canvas follow-scrolls to the selection. Every ImGui item in the tree window is
  NoNav (auto-nav is out of the loop entirely); A/Enter opens the detail popup,
  whose Selectables keep normal nav (the deck-proven listPopup pattern). Mouse
  hover/click drive the same selection state.
- **Tiered layout** — tier = prerequisite depth over the catalog's kept edges;
  roots on the bottom row, one row per tier, siblings ordered by the authored
  hpos (used for stable left-right ordering ONLY — the dome scatter is gone).
- **Low-value perks hidden** — NPC-dead perks never reach the catalog (component
  1); MARGINAL perks are hidden by default, kept only as dimmed passthroughs when
  an effective descendant needs them for connectivity ("Show marginal" toggle —
  [View] on the pad — reveals everything).

**Round 4 (polish pass, same day) — prereq correctness + feel:**

- **Bridged prereqs (the "order ignores prereqs" root cause).** Pass 2 of the
  catalog used to record a FILTERED direct parent (dead entry-less marker perk,
  player-only perk) as the prereq truth: unallocatable through MFO, so its whole
  subtree was permanently locked, and with no drawable edge its children rendered
  as root-row orphans. Prereq lines now resolve THROUGH filtered nodes to the
  nearest KEPT ancestors — those are both the drawable/tiered edges and the §5
  reachability truth (vanilla ANY-line rule); a line reaching the ROOT through
  only filtered nodes makes the node root-reachable. The perk's own CONDITIONS
  stay the final authority regardless: §5 evaluates full perkConditions on the
  follower at gate time, so an overhaul's HasPerk conditions still bind even when
  the bridge would be more lenient.
- **Condition-authored prereqs tier the layout** — rank-1 `HasPerk X == 1` (AND,
  literal) conditions are extracted into the catalog and added as tier edges, so
  visual order matches the conditions authority; exclusivity conditions
  (`HasPerk == 0`) are deliberately not matched.
- **Strict topological tiers** — Kahn longest-path from the roots replaced the
  bounded relaxation: every node sits strictly above all of its true, drawable
  prerequisites; cycle-defensive (authored cycles keep max-so-far tiers, nothing
  hangs).
- **Feel fixes**: the A press that opens the tree can no longer instantly open the
  root's take popup (release-guard, the r1Ready pattern); scroll-home no longer
  fights same-frame follow-scroll; follow-scroll runs on intent only (pad move /
  zoom / filter reflow) so the mouse wheel roams freely; a parked cursor no longer
  steals the pad's selection (hover steers only while the mouse moves); pad moves
  throttled to ~8 hops/s; zoom recentres on the selection; the marginal toggle
  keeps your place (selection restored by node, pulled back into view); an
  all-filtered tree says so and still answers [Y]/[View]/zoom.

**Round-4 ACCEPTANCE TRACE (the Pyromancer case, marth) — verified against the
installed load order.** Offline parse of the WINNING records in marth's
custom-modlist (profile Requiem; winning Destruction AVIF = Requiem.esp, 18 nodes,
root INAM 0 → single child Novice Destruction), with the round-4 classify/bridge/
gate pipeline ported over the real data:

- **Root intact:** Novice Destruction classifies EFFECTIVE (ModSpellCost entries) —
  kept, allocatable, the entry gate for the whole tree (single root child, so every
  node transitively requires it — vanilla-identical).
- **Pyromancy** (`Skyrim.esm:0581E7`): raw direct parent = Novice Destruction
  (kept) → the bridged prereq set is exactly {Novice Destruction}; nothing was
  bridged past, rootLine = false. Its own perkConditions carry
  `HasPerk(Novice Destruction) == 1` AND `GetBaseActorValue(Destruction) >= 25` —
  the conditions authority agrees with the tree edge, and §5 evaluates them in
  full on the follower. Three-state gate: owns nothing → unavailable; owns
  everything except Novice Destruction → unavailable; owns Novice Destruction
  (+ conditions) → available. The gate keys on OWNERSHIP of the nearest kept
  parent, never structural reachability.
- **The one real gap the trace exposed (fixed):** Requiem's Cremation / Deep
  Freeze / Electrostatic Discharge / Impact are ENTRY-LESS perks (script-driven —
  Requiem implements their effects outside the perk-entry engine) that the §3
  classifier filtered as dead markers. Their children (Fire/Frost/Lightning
  Mastery) carry `HasPerk(<that perk>) == 1` conditions — with the parent
  unallocatable, the conditions authority locked the whole Mastery tier forever.
  Entry-less perks are now **kMarginal**: takeable (one point, like the player
  pays), drawn as dimmed passthroughs when an effective descendant needs them —
  the player-identical chain (Novice → Pyromancy → Cremation → Fire Mastery)
  restored, no bridging involved.
- **Likely explanation for the original field sighting:** the load order ships
  "Requiem - SPID Apprentice and Novice Perks for All Followers" — Novice
  Destruction is granted NATIVELY to followers, making Pyromancy legitimately
  available while its parent renders in the subtle native style (accent ring on a
  dim disc). Correct gating that read as a violation; unverifiable offline, noted
  for the next field pass.

**Round 5 (2026-08-12) — AUTHORED DOME LAYOUT replaces the tiers (marth: combat
trees tiered fine, magic trees were a crammed mess; must be universal).** The tree
renders from the catalog's authored `hpos`/`vpos` — the exact coordinates the
game's own constellation menu draws, which every perk mod ships by construction —
so any tree, Vokriinator-scale merges included, arrives WITH good positions.
LAYOUT only: prereq gating (allocator, §5) untouched. **Normalization (the
universal part):** the kept set's bounding box is translated to origin, vpos
flipped (root at the canvas bottom), and coordinates scaled by the tree's own
**median nearest-neighbour distance** — one unit ≈ one typical node gap for ANY
authored scale; a single pathological close pair can't inflate the layout (median,
never min); exact-coincident nodes get a tiny deterministic nudge (rendered as
close as authored, never stacked dead-on). Per frame, units map to pixels by
fit-to-canvas CLAMPED to a 110–300px typical gap (×zoom): dense merged trees
scroll at readable density instead of clumping, tiny trees don't stretch across
the screen. **Presentation:** vertically-eased cubic edges with a wide soft
underlay beneath a thin core (tapered constellation look; lateral edges ease to
straight), a soft outer glow on available nodes; owned/available/locked/native
styling, explicit nav, culling, zoom, filter, and the 90%-display window all
unchanged. Kahn tiering and the condition-prereq layout edges are gone from the
board (the conditions authority still gates via IsTrue; the catalog field
remains for the census).

## 18. Addon API — making the add-on third-party-reproducible (2026-08-12)

**Why this section exists (marth):** the Progression Add-On (and the sibling
Roster Add-On, now a separate product) are meant to be the *worked examples* of a
public MFO addon API — after they ship, marth wants third parties to build their
own MFO add-ons. An "example" only qualifies if a third party could reproduce it
**with an ESL (+ Papyrus/config) and ZERO access to MFO's C++ source.** The add-on
therefore **cannot go to Nexus until this API layer exists** — it is the release
gate. See [[progression-addon-is-api-reference-example]].

### 18.1 What is already generic (the expensive part — done)
The entire runtime *engine* operates on load-order data, not on anything specific
to our ESL, so it is already reusable by any add-on:
- runtime read of the merged perk trees (§2), NPC-effectiveness classification (§3),
- player-identical requirement gating (§5), the tiered/bridged legible layout (§17),
- allocation + reconciliation (§4.2), the SPID perk-grant (§4.1), the ImGui board (§7).
None of this is coupled to `MFO_Progression.esl`. This is the shared runtime a
third-party add-on would consume unchanged.

### 18.2 What is hardcoded to OUR ESL today (the coupling to break)
1. **Plugin discovery by NAME** — `Progression.h:30` `kAddonPlugin =
   "MFO_Progression.esl"`, detected via `LookupLoadedLightModByName`. A third-party
   ESL has a different name and is never seen.
2. **A frozen FormID contract** — `ProgAllocator.h:41` ("a FROZEN contract with the
   generator"): the DLL reads records at fixed local ids via
   `LookupForm(id, "MFO_Progression.esl")`, and in particular a **fixed set of 3
   classes** (Melee/Ranged/Mage FormLists at frozen ids `0x81x`, with DLL
   fallbacks). A third party cannot add a class or supply their own set.
3. **Economy hardcoded in the DLL** — the §17 simplification put
   `floor(level/3)` perk points and flat 5/level skill points in C++ constants. For
   an add-on to set its own rates this must move back out to ESL-declared config —
   note this pulls opposite to §17's simplification (accept the reversal for the API).

### 18.3 The gap = a bounded front-door refactor (NOT a rewrite)
- **Generic registration/discovery.** Replace the by-name lookup with a MFO.esp
  anchor that any add-on ESL joins to announce itself — a keyword or a FormList in
  MFO.esp (`MFO_AddonRegistry`-style) that the add-on adds its config-holder record
  to; the DLL scans members at load instead of hardcoding a plugin name. This is the
  actual "API surface."
- **Declared N-class model.** Read an arbitrary set of classes the add-on defines
  (name + skill FormList + perk-priority FormList per class), instead of the fixed 3.
  Biggest single piece: the allocator's class model and the board's class prompt go
  from fixed-3 to N-declared; the generator/contract describes how an add-on lays
  out a class.
- **Re-expose per-add-on config** (economy, display name, MCM label) as
  ESL-declared records the DLL reads per registered add-on.
- **Document the API contract** — the records/keywords/layout a third-party ESL must
  define, plus a minimal example ESL. This *writing* deliverable is the thing that
  actually makes it reproducible "without the main code"; it is not optional.

### 18.4 Effort + sequencing
Moderate and contained — the engine (§18.1) carries over untouched; this generalizes
the declaration layer + ships a spec. Rough size: ~one focused build round for the
DLL generalization (registration + N-class + re-exposed config) + one for the API
doc/example ESL — comparable to a single board round, not the whole saga. Do it as
its OWN effort **after** the feature is field-verified (don't destabilize working
code). It is the **Nexus-release gate** for the add-on: the Serana cast-takeover and
other core fixes can ship as normal MFO updates meanwhile; the Progression Add-On
releases as the first API example once §18.3 lands. The Roster Add-On, not yet built,
should be authored API-first against the same registration mechanism.

### 18.5 Design test going forward
For every remaining #74 decision, ask: *"could a third party replicate this with only
their ESL and the documented API?"* If a behavior can only be reached by editing
MFO's C++, it belongs behind the API, not baked to our plugin.

### 18.6 CONTRACT LOCKED (marth, 2026-08-13)
- **Registration = conflict-free per-addon, enumerable.** MFO.esp ships a sentinel (keyword or marker form). Each addon owns ONE manifest record that carries/points to that sentinel; MFO enumerates every addon manifest across the merged load order at kDataLoaded (supports N addons, though we ship one). Replaces the by-name `LookupLoadedLightModByName("MFO_Progression.esl")`. NO shared FormList that addons inject into (that collides) — each addon has its own record. The implementing round picks the exact enumerable record type/layout CommonLib supports cleanly and DOCUMENTS it as the frozen contract.
- **Classes: N-declared, not fixed 3.** Manifest → a FormList of class definitions; each class = `{ display name, skills FormList (AVIF forms, order = weight), perk-priority FormList }`. Allocator `g_class[]` and the board class prompt become dynamic-N.
- **Economy: FULLY addon-configurable — option (a), marth.** EVERY economy value is addon-declared via GLOBs the manifest references: perk divisor (`levelsPerPerkPoint`), skill points/level, shared-growth divisor, respec rapport cost, skill cap, manual skill points/level. MFO applies documented defaults when a GLOB is absent. Re-exposes what §17 hardcoded. **Perk divisor default is now 2** (`floor(level/2)`, was /3 — too few points, marth 2026-08-13).
- **`MFO_Progression.esl` = the first addon built to this contract** — the worked reference example (its 3 classes + its economy GLOBs, declared through the public records, no special-casing in the DLL).
- **PRGN discipline:** if the record layout changes (e.g. class stored as an index into a now-dynamic list), BUMP the PRGN version and gate/migrate — per the cross-update lesson (§/serialization).

**18.6 Stage 3 SHIPPED — economy record shape (frozen for the Stage 4 API doc).**
The manifest is ONE FLST; the DLL type-dispatches its entries: entry[0] = the MFO.esp sentinel keyword (skipped), the ONE FLST entry = the classes-list, and **every GLOB entry is an economy knob**. Each knob is matched by **editor-id SUFFIX** (case-sensitive, last-writer-wins across manifests in load order), NOT by fixed FormID — the editor id is the contract, the local id only has to be a stable unique own-form id:
`_LevelsPerPerkPoint` (perk divisor, floor(level/N), **default 2**), `_SkillPointsPerLevel` (auto skill pts/level, **default 2** — was 3, marth 2026-08-17), `_ManualSkillPointsPerLevel` (manual pool rate, **default 2** — was 5, marth 2026-08-17), `_SharedGrowthDivisor` (default 2), `_RespecRapportCost` (default 500), `_SkillCap` (default 100), and `_DevCmd` (dev-harness selector — the ONE knob read LIVE, its value not a record default). A knob whose suffix is absent from every manifest falls back to the DLL default (each logged). MFO's DLL hardcodes only these defaults; `MFO_Progression.esl` declares its whole economy via manifest GLOBs. Economy is NOT serialized — no PRGN bump.
