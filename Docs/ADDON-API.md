# MFO Addon API — building a follower-progression addon (no C++ required)

**Status: FROZEN public contract (§18.6). v1 — 2026-08-17.**

This document is the complete contract for building an **MFO progression addon**:
a plugin (ESP/ESL/ESM) that declares its own **classes** and **economy**, which
`MFO.dll` then drives at runtime. You need **only a plugin + xEdit/CK** — no
access to MFO's C++ source. `MFO_Progression.esl` (shipped with MFO) is the
worked reference example; everything below is exactly what it does.

MFO provides the **engine** — runtime perk-tree reading, NPC-effectiveness
filtering, player-identical gating, allocation, the ImGui board, the SPID-style
perk grant, the co-save. Your addon provides only **declarations** (records).
That split is the whole point: the engine is shared; the addon is data.

---

## 1. How registration works

`MFO.esp` ships one sentinel keyword:

| Record | Editor ID | Type |
|---|---|---|
| Registration sentinel | `MFO_AddonManifest` | `KYWD` |

An addon announces itself by shipping **exactly one `FLST` (FormList)** — its
**manifest** — whose **first entry is that sentinel keyword**. At game load
(`kDataLoaded`), MFO walks every `FLST` in the merged load order and treats each
one whose `[0]` element is `MFO_AddonManifest` as an addon manifest. Multiple
addons can register — each owns its own manifest `FLST`; there is **no shared
list to inject into** (that would collide), and **no plugin-name lookup** (your
plugin can be named anything).

Because the manifest references a form defined in `MFO.esp`, **your plugin must
master `MFO.esp`** (add it as a master). It will also master `Skyrim.esm` for
the vanilla skill (`AVIF`) forms.

---

## 2. The manifest FLST

After the sentinel, the manifest lists your declaration records **in any order**
— MFO dispatches by record **type**, not position:

```
Manifest FLST (e.g. "MyAddon_Manifest"):
  [0] MFO_AddonManifest      (KYWD)  ← REQUIRED, must be first
  [ ] <your classes-list FLST>       ← exactly one FLST → your classes
  [ ] <economy GLOB>                 ← zero or more GLOBs → economy knobs
  [ ] <economy GLOB>
  ...
```

- **Entry `[0]` MUST be the `MFO_AddonManifest` keyword.** If it isn't, MFO
  ignores the FormList.
- **Exactly one `FLST` entry** (besides `[0]`) is read as your **classes list**
  (§3). If you declare no classes the addon is inert.
- **Every `GLOB` entry** is read as an **economy knob** (§4), matched by its
  editor-id suffix. Unrecognized suffixes are ignored (logged).

The reference manifest `MFOP_AddonManifest` has 9 entries: sentinel + one
classes-list FLST + 7 economy GLOBs.

---

## 3. Declaring classes

A follower's **class** drives skill auto-scaling weights, the auto perk picker,
and its combat stance. Classes are **N-declared** — ship as many as you like.

### 3.1 The classes-list FLST
One `FLST` whose entries are your **class-def FLSTs**, in display order:

```
Classes-list FLST ("MyAddon_Classes"):
  [0] <class-def FLST: Warrior>
  [1] <class-def FLST: Archer>
  [2] <class-def FLST: Mage>
  ...
```

### 3.2 A class-def FLST
Each class is **one `FLST`** whose entries declare the class, **in any order**
(dispatched by type):

| Entry type | Meaning | Required |
|---|---|---|
| `MESG` | The class **display name** = the MESG's `FULL` (title) field | Yes (1) |
| `AVIF` | A **skill** the class scales. **Order = weight** (see below) | ≥1 |
| `PERK` | Auto-pick **priority** perks, tried first by the auto-picker | Optional |
| `GLOB` | The **combat stance**: editor-id must end `_Stance`, value 0–3 | Optional (1) |

- **Skill weight is positional.** List the `AVIF` skill forms most-important
  first. MFO applies a triangular weight over the (sibling-pruned) list — with
  the reference Melee list (One-Handed, Two-Handed, Heavy, Light, Block,
  Marksman) that lands on ~40/30/20/10 after pruning to the follower's dominant
  weapon + armor. Use the **live** `AVIF` forms (Skyrim.esm's One-Handed =
  `0x0000044C`, etc.); MFO maps them through the running `ActorValueList`, so an
  overhaul that moves a skill still resolves, and an unmappable entry is skipped
  with a warning.
- **Stance** (`_Stance` GLOB) mirrors MFO's combat-class override:
  `0` = none/auto, `1` = Melee, `2` = Ranged, `3` = Cast/Mage. Omit for `0`.
- **Perk priority** is optional and typically shipped **empty** — a plugin that
  only masters `Skyrim.esm` can't name an overhaul's perks. MFO's auto-picker
  falls back to a name-agnostic, weight-driven pick that works under any
  overhaul. An overhaul-specific patch can fill these in xEdit later.

The reference example ships three class-def FLSTs (`MFOP_ClassDef_Melee/Ranged/
Mage`), each = 1 MESG name + its AVIF skills + an empty PERK list + a `_Stance`
GLOB (1/2/3).

---

## 4. Declaring economy

Every economy value is a **`GLOB`** listed directly in the manifest, matched by
its **editor-id SUFFIX** (case-sensitive). Editor IDs persist at runtime for
globals, so this is stable and console-addressable. MFO reads each global's
**record default** at load (not any save-persisted value). **A knob you omit
falls back to MFO's default** — declare only what you want to change.

| Editor-id suffix | Meaning | Default |
|---|---|---|
| `_LevelsPerPerkPoint` | 1 perk point per N levels (`floor(level/N)`) | `2` |
| `_SkillPointsPerLevel` | Auto class-scale skill points per level | `2` |
| `_ManualSkillPointsPerLevel` | Manual-mode skill points per level | `2` |
| `_SharedGrowthDivisor` | Benched-follower growth divisor (2 = half rate) | `2` |
| `_RespecRapportCost` | Rapport cost to respec | `500` |
| `_SkillCap` | Auto-scale base-AV ceiling | `100` |
| `_DevCmd` | Dev-harness verb selector (read **live**, dev-only) | — |

Name your global anything ending in the suffix (the reference uses
`MFOP_SkillPointsPerLevel`, etc.). If two addons declare the same knob, the last
in load order wins. At load MFO logs each resolved knob
(`[prog] economy: _SkillPointsPerLevel = 2 (from MyAddon_SkillPtsPerLevel)`) and
each defaulted knob — check `MFO.log` to confirm your values took.

---

## 5. Plugin requirements & gotchas

- **Masters:** `Skyrim.esm` (skill AVIFs) **and** `MFO.esp` (the sentinel). If
  you reference nothing else, those two suffice. An ESL-flagged plugin is fine
  (the reference is one) — keep own records in the ESL-legal local range and
  remember own records sit at master-index = your master count.
- **Load order:** your addon must load **after `MFO.esp`** (it masters it, so
  this is enforced).
- **Record defaults, not values:** MFO reads `GLOB` record defaults at
  `kDataLoaded`. Setting a global's value from a script/console at runtime does
  **not** change the economy for that session — edit the record default.
- **The engine is not yours to ship.** Perk-tree reading, the effectiveness
  filter, gating, the board, and the perk grant are MFO's. You cannot (and need
  not) reimplement them — you declare classes + economy and MFO does the rest.
- **Detection is all-or-nothing per session:** MFO latches registered addons at
  load. Add/remove your plugin between sessions, not mid-session.

---

## 6. Save data (PRGN) — MFO's, not yours

MFO stores per-follower progression (class, level, allocated perks/skills) in
its own co-save record `PRGN`. **Your addon does not serialize anything.** One
consequence you should know: a follower's stored class is the **class-def FLST's
FormID**, resolved on load via the save system's form-resolution — so it
survives load-order changes. If your addon is removed, affected followers lose
their class cleanly (re-pick on the board); nothing corrupts.

---

## 7. Worked example — `MFO_Progression.esl`

The shipped reference addon, generated by `MFO_GenerateESP.py`. Its records
(local IDs are illustrative — author your own):

| Editor ID | Type | Role |
|---|---|---|
| `MFOP_AddonManifest` | `FLST` | manifest — `[0]`=`MFO_AddonManifest`, `[1]`=`MFOP_Classes`, `[2..]`=economy GLOBs |
| `MFOP_Classes` | `FLST` | classes list → the 3 class-def FLSTs |
| `MFOP_ClassDef_Melee/Ranged/Mage` | `FLST` | class defs (MESG + AVIF skills + PERK + `_Stance`) |
| `MFOP_ClassName_Melee/Ranged/Mage` | `MESG` | class display names (via `FULL`) |
| `MFOP_ClassMelee/Ranged/Mage_Stance` | `GLOB` | `_Stance` = 1 / 2 / 3 |
| `MFOP_ClassSkills_*` | `FLST` | (per-class AVIF skill lists, referenced by the class-defs) |
| `MFOP_ClassPerks_*` | `FLST` | (per-class PERK priority — shipped empty) |
| `MFOP_LevelsPerPerkPoint` | `GLOB` | economy `_LevelsPerPerkPoint` = 2 |
| `MFOP_SkillPointsPerLevel` | `GLOB` | economy `_SkillPointsPerLevel` = 2 |
| `MFOP_ManualSkillPointsPerLevel` | `GLOB` | economy `_ManualSkillPointsPerLevel` = 2 |
| `MFOP_SharedGrowthDivisor` | `GLOB` | economy `_SharedGrowthDivisor` = 2 |
| `MFOP_RespecRapportCost` | `GLOB` | economy `_RespecRapportCost` = 500 |
| `MFOP_SkillCap` | `GLOB` | economy `_SkillCap` = 100 |
| `MFOP_DevCmd` | `GLOB` | economy `_DevCmd` (dev-only) |

To build your own: master `MFO.esp` + `Skyrim.esm`, create the MESG names, the
`_Stance` globals, the class-def FLSTs, the classes-list FLST, and your economy
globals; then a manifest FLST leading with `MFO_AddonManifest`, followed by the
classes-list and your economy globals. Load in, and confirm the
`[prog] addon manifest … registered`, `[prog] class "…" registered`, and
`[prog] economy: … (from …)` lines in `MFO.log`.

---

*This contract is frozen. Additions are made by new suffixes / new optional
record types, never by changing the meaning of an existing one.*
