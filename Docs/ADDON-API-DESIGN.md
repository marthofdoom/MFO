# MFO v1.1 — Add-On API + Manifest Design (Phase 0 output)

**Status:** design, awaiting marth's approval. This is the Phase 0 deliverable of
`Docs/V1.1-ADDON-ARCHITECTURE-PLAN.md`. Docs-only, no code changed.

**Phase 0 acceptance question:** *is every PROGRESSION-SPECIFIC item expressible as
data/rules over a general API?* Answer below: **yes for all but four**, and those four
resolve to a small set of NEW GENERAL primitives (never progression-specific C++), so the
ruling holds.

---

## 0. Headline: the strangler is already ~70% done

The classified audit (Phase 0, read-only, 2026-08-26) found that the DLL is **already a
working add-on host** for most of progression:

- **Discovery already works with no by-name lookup and no fixed IDs.** At `kDataLoaded`,
  `Progression::Init` scans every `BGSListForm` in the merged load order and registers each
  one that self-declares as a manifest (`Progression.cpp:640-671`, `Addons()`). N add-ons,
  no shared injection point.
- **Class defs are already manifest data.** `ClassDef = {id(FLST), name(MESG.FULL),
  stance(GLOB _Stance), skills[](AVIF, order=weight), perkPriority[](PERK)}`, parsed
  generically by `ParseClassDef` (`ProgAllocator.cpp:1631`) with type dispatch — no
  progression-specific record shapes.
- **The whole economy is already manifest data.** Every knob is a GLOB matched by
  editor-id **suffix** (`AssignEconomyGlob` + `kEconKnobSuffixes[6]`, `EdidEndsWith` at
  `ProgAllocator.cpp:1442`) — zero fixed IDs, zero cross-plugin references.
- **The MCM tab is already a separate add-on JSON** (`out/MCM/Config/MFO_Progression/
  config.json`), read as its own config, not host globals.

So the rework is **not** a rebuild. It is: (a) fix the ONE cross-plugin reference that
forces the MFO.esp master (the Vortex bug — §4), (b) lift **four** hard-coded design values
into the manifest (§3), (c) re-root HMS on the base class and surface a thin general API
(§2, §5), (d) generalize the board tab + co-save slot (the two biggest remaining pieces).

---

## 1. The four buckets (from the audit)

- **GENERAL-API** — primitives any add-on wants; stay in the DLL, surfaced as a clean API.
  Almost all already exist *internally* (private to `MFO::ProgAllocator`/`MFO::Progression`);
  the work is exposure, not invention.
- **ADDON-HOST** — manifest discovery/parse, generic MCM/board rendering, generic co-save
  slots, the registry. A real host already exists (`AddonRef`, `Addons()`, the suffix-match
  parse loop, `LookupAddonForm`, the snapshot plumbing).
- **PROGRESSION-SPECIFIC** — design/content that must become add-on-declared DATA. Residual
  C++-baked list is short (§3).
- **NOT-PROGRESSION (leave alone)** — `FoeCount`, `Temperament` sit near progression by
  directory adjacency but are combat/personality; do not move them.

---

## 2. General follower API surface

The DLL's permanent, add-on-agnostic API. Everything here already has a working
implementation inside progression; Phase 1 lifts it to a clean entry point. All calls are
**main-thread** (the `g_followers`/`g_prog` discipline, #4) except the telemetry hook, which
keeps its worker→main mirror.

| API | Backs onto | Notes |
|---|---|---|
| `GetBaseClass(follower)` / `SetBaseClass(follower, stance)` | `FollowerState::combatClassOverride` (State.h:82) | **NEW primitive, the HMS crux.** Base Gambit class, present with or without any add-on. The add-on *feeds* it, never *owns* it. |
| `GetFollowerSkill` / `SetFollowerSkill(follower, av, delta)` | `ReconcileSkill` (`ProgAllocator.cpp:301`) | Baseline-floored, skill-cap clamped skill write. |
| `GetFollowerHMS` / `SetFollowerHMS(follower, pool)` | `kHmsAV[3]` base-AV read/write (`:655,:772`) | General vital-base primitive. |
| `MeasureEngineVitalAward(follower)` | `RecomputeHMS` measure-not-clobber core (`:644`) | Observes what the engine granted this level *before* re-asserting. Cannot be reimplemented from data — a host primitive. |
| `SetEngineAwardPolicy(follower, Revert\|Adopt)` | `cancelEngineAwards` revert path (`:432,:778`) | The named "revert engine's per-level award" capability. Policy VALUE is add-on data; the mechanism is API. |
| `GrantFollowerPerk` / `RevokeFollowerPerk` | `AddPerk`/`RemovePerk`/`ApplyPerksFromBase` (`:598,:1244`) | Thin over CommonLib; the idempotent native-ownership deferral is the value. |
| `CaptureFollowerNaturalBaseline(follower)` | Enroll baseline snapshot (`:2043`) | Opaque respec floor token. |
| `SubscribeFollowerLevelUp(cb)` | `PollWork` player-delta loop (`:1340`) | General level-up event. Gain *formula* (shared-growth divisor) is add-on data. |
| `NoteFollowerCombatAction(follower, opcode)` | `NoteCombatFire` (`:2843`) | **Worker-thread** usage telemetry, mutex-mirrored to main (`g_hmsFireMx`). Must NOT read `g_followers` off-thread. |
| `QueryFollowerPerkCatalog()` | `Progression::Get()` merged-AVIF walk | Reads the live/merged perk trees → value-only views (grid coords, prereq graph, skill gates). Overhaul-agnostic, no name-match. Genuinely general. |

**Design rule:** no progression concept (class ratios, scarcity pool, allocation policy)
appears in any signature above. Those are data (§3) or callbacks the add-on supplies.

---

## 3. What must move OUT of C++ into the manifest (the residual four + schema)

Everything else is already data. These four are the C++-baked design still to lift:

1. **HMS class ratios** — `HmsProfile` (`ProgAllocator.cpp:515`: Melee 60/5/35, Ranged
   40/5/55, Mage 15/80/5), today a hardcoded switch on `stance`. → add `hmsWeights[3]` +
   `primaryPool` to each manifest `ClassDef`. *Single biggest hardcoded design value.*
2. **Perk-effectiveness verdict table** — `kEntryPoints[92]` (`Progression.cpp:65-158`),
   opinionated content. → optional manifest `entryPointVerdicts[]`, with the host shipping
   the current table as a default (the indices are engine-frozen, so a default is sane).
3. **Skill-name/AV list** — `kSkillNames[18]` (`Progression.cpp:39`). → host default; it is
   the 18 vanilla skills, content but stable.
4. **Board-tab layout + its view contract** — `Board.cpp:1251-2427` +
   `BoardProgSnap/BoardFollowerView/BoardSkillLine/BoardNodeView` (`ProgAllocator.h:291`). →
   add-on-declared UI *composition* over host widgets (§5, tension #4).

### Manifest self-declaration schema (derived)

```
Addon manifest = one self-declared BGSListForm, recognized by editor-id SUFFIX (§4):
  header:  { addonType: "progression-tab", version, displayName }

  classes: [ ClassDef ]
    ClassDef = { id(FLST), name(MESG.FULL), stance(GLOB _Stance 0..3),
                 skills[](AVIF, order=weight), perkPriority[](PERK),
                 hmsWeights[3] + primaryPool }        # hmsWeights is the ONE new field

  economy (already GLOBs, suffix-matched):
    { levelsPerPerkPoint, skillPointsPerLevel, manualSkillPointsPerLevel,
      sharedGrowthDivisor, respecRapportCost, skillCap, cancelEngineAwards }

  allocation rules:
    { skillWeightCurve (triangular default), engineAwardPolicy: Revert|Adopt,
      hmsSkewMaxFrac (off-class ceiling) }

  perk-effectiveness policy (optional; host default = current kEntryPoints[]):
    entryPointVerdicts: [ {entryPointIndex 0..91, verdict} ]

  board-tab layout (composition over host widgets):
    { classPicker, perkTree(from catalog coords), skillRows[18],
      perkPointsHeadline, manualSkillToggle, respecButton }
    actions: { SetClass, AllocPerk, Respec, SetManual, ApplySkillPoint }

  MCM tab: separate config.json (already add-on-owned)

  co-save state (generic per-follower "add-on state blob" the host frames — §6):
    flags, class identity {clsPlugin, clsLocal}, progressionLevel,
    sharedGrowthRemainder, nativeTreePerksAtEnroll, manual{...},
    perks[], skills[], baseline[],
    HMS{ per-pool[H,M,S]{baseline,target,skew,cumulative},
         battlesSinceLevelUp, battlesOffClass, offClassPool, hmsCaptured }
```

Everything above `hmsWeights` / `entryPointVerdicts` / board-layout / co-save-blob is
**already** manifest-declared today.

---

## 4. RESOLVED: manifest form + the Vortex fix (open call #2)

**Recommendation: the manifest stays ESL records — no separate data file.** It already is
records, discovered by self-declaration, and that scheme already carries classes and the
whole economy with zero fixed IDs. A separate data file would throw that away and add a
second load path for no gain.

**The Vortex bug and its fix are now pinpointed.** Discovery today requires
`list->forms.front() == Forms::g_addonSentinel`, where `g_addonSentinel` (`kAddonSentinel
0x803`) is the `MFO_AddonManifest` keyword shipped **inside MFO.esp**. Because the add-on's
manifest FLST literally references an MFO.esp form, the add-on **must declare MFO.esp as a
master** — that is the reciprocal ESL↔ESP dependency Vortex chokes on.

**Fix (one place):** swap discovery from "first entry points at MFO.esp:0x803" to
**editor-id suffix match on the add-on's OWN manifest form** — the exact scheme the economy
GLOBs already use (`EdidEndsWith`, no cross-plugin reference). The add-on then references
only its own forms, needs no MFO.esp master, and Vortex is unblocked. `g_addonSentinel` /
`kAddonSentinel 0x803` become dead and retire from MFO.esp. Change surface:
`Progression.cpp:640-671` + drop the sentinel keyword from `MFO_GenerateESP.py`.

This directly delivers required outcome #2 (self-contained manifest, no MFO.esp master).

---

## 5. RECOMMENDED phase order (open call #1)

**Recommendation: a thin general-API slice first, then HMS, then the bulk generalization.**
The audit changes the calculus: the API HMS needs already exists internally and is *small*,
so we get the release blocker fixed EARLY *and* correctly (on the real API, not a throwaway
patch). Proposed sequence, each chunk CI-green + separately reviewable:

1. **Phase 1a — thin API slice HMS needs:** surface `GetBaseClass/SetBaseClass` (over
   `combatClassOverride`), `Get/SetFollowerHMS`, `MeasureEngineVitalAward`. Route existing
   progression code through them, no behavior change. *Light review.*
2. **Phase 2 — HMS base feature + class-read fix (RELEASE BLOCKER).** Re-root
   `RecomputeHMS`/`HmsTrackBattle`/`SetClass` from `clsId->stance` onto `GetBaseClass`
   (`combatClassOverride`), so HMS steers by the base Gambit class and works with the add-on
   absent. Root-cause "Mage reads as Melee" (it reads `clsId`, which is 0/wrong when the
   add-on class didn't resolve). **Unblocks release testing.** *Light-to-medium review.*
3. **Phase 3 — fixed-stat HMS grant (RELEASE BLOCKER, HEAVY co-save review).** Detect
   fixed-stat NPCs, tally vs player, grant at player rate once caught up, retroactive.
   Extends the still-unshipped v5 block.
4. **Phase 1b — finish the general API surface** (perks, skills, level-up subscription,
   catalog query, award policy) and route existing code through it.
5. **Phases 4-9 — manifest completion (hmsWeights + verdict table + board layout), host the
   MCM/board tab from the manifest, allocation-as-data, generic co-save slot, verify + cut.**
   Progression stays functional after every chunk.

**Why not manifest-foundation-first (Phase 4 first):** the manifest is already carrying
classes + economy; the four residual values (§3) are cheap to add *after* HMS is unblocked.
Fixing the release blocker first buys back the stalled testing without a throwaway patch,
because Phase 2 lands on the same `GetBaseClass` primitive the full API needs anyway. If you
prefer foundation-first for cleanliness, the cost is testing stays stalled longer; I don't
think that trade is worth it given the API HMS needs is a 3-call slice.

---

## 6. Extraction risks (carry into every chunk)

- **HMS class-source crux (confirmed):** `RecomputeHMS`/`HmsTrackBattle` read
  `FindClassDef(a_st.clsId)->stance`; `clsId` is the ADD-ON class id and is `0` when the ESL
  is absent or the class failed to resolve. `SetClass` currently mirrors *from* the add-on
  (`combatClassOverride = def->stance`, `:2120`) — base is downstream of add-on. Phase 2
  inverts this: base `combatClassOverride` becomes source of truth, `hmsWeights` an optional
  override. Sites: `:646` (HMS), `:594` (track), `:2120` (mirror), invoked `:1251/1384/1402/1403`.
- **Co-save (HIGHEST blast radius):** 'PRGN' v5 is written **even when the add-on is absent**
  (no `g_ready` gate) so state survives an add-on-less session. Any generic "add-on state
  blob" slot MUST keep that write-when-absent behavior and keep every shipped reader forever
  (#12), or class/perk state wipes on an add-on-less run. Serialize plugin-qualified identity
  `{clsPlugin, clsLocal}` (not runtime `clsId`), re-resolved via `LookupAddonForm` `.esl↔.esp`
  fallback, or a load-order change wipes classes.
- **Threading:** allocator verbs / poll / RecomputeSkills+HMS / co-save = MAIN thread only,
  no lock. `NoteCombatFire` is on the BSJobs worker and writes a mutex mirror consumed on the
  main poll — a general telemetry API must keep that hop and never read `g_followers`
  off-thread (#4). Board `Prog*` edits ride `MainThread::Post`; the generic edit-apply must
  preserve it.
- **Enum ABI:** `stance`/`combatClassOverride`/`CombatStyle::Stance` share ordinals 0-3,
  written raw to FLWR v4. Renumbering is a schema migration, not an edit.

---

## 7. Design tensions — the four that resist pure data (→ NEW general primitive, never add-on C++)

1. **Base combat class primitive** — `Get/SetBaseClass` over `combatClassOverride` must exist
   independent of any add-on (HMS-as-base depends on it). New API, not data. *(Resolves the crux.)*
2. **Measure-the-engine-award** — diffing base-AV against last-written is engine-coupled; a
   host primitive. Policy + ratios are data.
3. **Perk-effectiveness classifier** — the `kEntryPoints[92]` walk resists CommonLib entry-type
   churn; the algorithm is C++. Host ships the table as a default (optionally add-on-overridable)
   + exposes `QueryFollowerPerkCatalog()`.
4. **Board-tab rendering** — the perk dome (Bezier edges, AABB cull, zoom, controller nav,
   `Board.cpp:1928-2320`) is real ImGui. The manifest declares *composition* (node coords are
   already catalog data); the host must own a generic widget vocabulary (perk-tree, skill-table,
   action-button) rich enough that the add-on never ships C++. If a future add-on needs a widget
   the host lacks, the **host** grows a general widget — never add-on-specific code.

These four are exactly where "widen the API with a general primitive, never smuggle
progression math back in" applies. Everything else is data/rules.

---

## 8. Phase 0 verdict

Every PROGRESSION-SPECIFIC item is expressible as data or as rules over the general API,
with the four §7 items handled by NEW GENERAL primitives (base-class, engine-award
measurement, catalog query, host widget vocabulary) — none progression-specific. The ruling
("delete the add-on, the DLL loses zero lines") is achievable. **Two open calls resolved:
manifest = ESL records with suffix discovery (§4); order = thin-API-slice → HMS → bulk (§5).**
Awaiting marth's approval to close Phase 0 and start Phase 1a.
