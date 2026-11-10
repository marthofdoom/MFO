# Complex spells NPCs cannot cast properly: research round 2 (ClickUp 86e3dmtr1)

Date 2026-11-09. Read-only. MFO origin/main `0315597`, Harbinger origin/main `acc7788` (ABI 35), CommonLib
fork `mit-3.7` at `0b904231`, binary 1.6.1170 (`binaries/1.6.1170/SkyrimSE.unpacked.exe`).
Agent log: `scratchpad/agentlogs/complex-spells-research.md`.

**This round updates a plan that already exists. It does not replace it.** The first research
(`Docs/handoff/cloud-2026-10-01/complex-spells-research.md` on MFO main, archetype rows A1-A22, phases P0-P5,
recorded as backlog MFO-B215) still stands as the design. Its P0 (the passive `cast/Archetype.cpp` classifier
plus the `[archetype]` and `[cfc]` lines) is merged. This round answers the items that doc marked NEEDS LOCAL,
from three sources:
- the vanilla records, read as evidence only (a throwaway parser in `scratchpad/tools/esmshape.py` over the
  Tuxborn `Skyrim.esm`; nothing from it goes into code);
- the field logs in `_evidence/` (`[archetype]`, `[cfc]`, Harbinger `[ctcensus]`);
- the fork headers and RTTI.

It also records what has changed since 10-01, and turns the plan into ordered cards.

Evidence labels used below:
- **[rec]**: a vanilla record read.
- **[fork]**: a CommonLib fork header, file:line.
- **[field]**: a deck or LoreRim log.
- **[disasm]**: from earlier disassembly passes. Proves the path exists, not that it runs.
- **[pred]**: the decoded engine table's prediction, not observed.

---

## 0. What changed since the 10-01 doc (do not re-derive)

1. **Harbinger seats 10 of the 15 caster types now, not 2.** Offensive, Restore, Ward, Summon, Cloak, Light,
   Invisibility, BoundItem, Armor and Script are seated (APMF `CastSeats.cpp:182-191`). Stagger, Disarm,
   TargetEffect, Paralyze and Reanimate are not. Shipped in v0.11.0. Field-proven only for heal-other and
   offense (FACETS.md:1049).
2. **Rowless serve exists.** A claimed spell that keys no caster row is handed the engine's own Script row:
   row 9 {Script, other, hostile} or row 22 {Script, self, beneficial} (APMF `CastClassify.cpp:298-445`).
   The 10-01 doc's "no row = the AI can never cast it" has therefore been partly answered in code. It is not
   field-run.
3. **MFO has claim roads for buffs, summons and rowless spells** (`cast/BuffRoad.cpp` `ChooseBuffRoad`). It
   also has the nature-undeclared AUTO refusal (e4b8968 / d8b0aa3 / c62d4cf), and `ClassifySpell` now files
   Calm, Frenzy, Demoralize, Paralysis and TurnUndead as Offense (CasterConsent.cpp:54-75).
4. **Summons (86e39pz55) are done.** They are one-shot on every road, the engine places them, and they were
   deck-confirmed 2026-09-24 (923e72d). The position approach was dropped with evidence:
   `SummonCreatureEffect::Start` places the creature itself.
5. **Runes are proven dead on the NPC road** (code plus marth's field look on 09-23: "the answer is no"). They
   are deferred to batch d (86e3dwq70).
6. **Harbinger's position cast (marker, exception (e)) is built, ships OFF, and has never run in the field.**
   It refuses summons, every projectile spell and every concentration spell. Per marth it is not an endpoint
   (animations are required).

---

## 1. Census by RECORD SHAPE

Every field below is verified in the fork, and every shape rule reads only these fields:
- `EffectSetting::data` sits at +0x68 of the form (`include/RE/E/EffectSetting.h:69-112`). Its fields:
  - `flags` +00: kHostile bit 0, kDetrimental bit 2, kSnapToNavMesh bit 3, kNoArea bit 11, kHideInUI bit 15.
  - `associatedForm` +08.
  - `archetype` +58.
  - `primaryAV` +5C.
  - `projectileBase` +60.
  - `explosion` +68.
  - `castingType` +70.
  - `delivery` +74.
  - `impactDataSet` +90.
  - `dualCastData` +A0.
  - `aiScore` +E4.
- `SpellItem::data` (`S/SpellItem.h:49-57`):
  - `flags` +04: kInstantCast bit 18, kIgnoreLOSCheck bit 19, kNoDualCastMods bit 23.
  - `spellType` +08, `chargeTime` +0C, `castingType` +10, `delivery` +14, `castDuration` +18, `range` +1C.
- `SpellItem::IsTwoHanded()`: vfunc 0x67, the equip slot's flags & 1, i.e. the BothHands slot (`S/SpellItem.h:82`).
- `SpellItem::GetChargeTime()`: vfunc 0x64 (`:80`).
- `BGSProjectile::data.types` +02 (`B/BGSProjectile.h:32-45`): Missile 1, Lobber 2, Beam 4, Flame 8, Cone 16,
  Barrier 32, Arrow 64.
- `BGSImpactDataSet::impactMap` +28 (`B/BGSImpactDataSet.h:36`). `BGSImpactData::hazard` +80, NAM2
  (`B/BGSImpactData.h:70`).
- `BGSHazard::data.spell` +18 (`B/BGSHazard.h:31`).
- `EffectArchetypes::ArchetypeID` (`E/EffectArchetypes.h:9-56`): Script 1, Absorb 4, DualVM 5, Calm 6,
  Demoralize 7, Frenzy 8, Paralysis 21, Reanimate 22, BoundWeapon 17, Summon 18, AccumulateMagnitude 32,
  PeakVM 34, Cloak 35, Rally 38, SpawnHazard 40, SpawnScriptedRef 43.

| # | Shape | How the record identifies it (procedural) | Vanilla examples [rec] | What MFO does today | Mishandled? |
|---|---|---|---|---|---|
| S1 | **Rune / trap** | spell delivery kTargetLocation, FF, an effect with `projectileBase` of type Lobber | Fire/Frost/Shock Rune: FF/TL, range 20, Lobber, ValueMod/DualVM Health, Hostile | `ClassifySpell` = Offense, so it takes the owned offense claim (`CastOn.cpp:1054-1102`; that claim has no delivery filter). `shape=Rune` is a log label only | **YES.** The AI picks and animates it, then nothing is placed (the player-only pick gate; field 09-23). Batch d |
| S2 | **Summon** | any effect archetype kSummonCreature. Vanilla: FF/TL, kSnapToNavMesh, no projectile | Conjure Familiar / Atronachs / Dremora Lord / Dragon Priest: FF/TL range 24. Thralls: BothHands, charge 3.0, range 100 | one-shot. Claim road when `ChooseBuffRoad` = Claim (needs ABI ≥ 19), else direct `CastSummonOnce` (unanimated) | Placement: no, the engine places it. Claim road built but **claimed Summon built = 0 in every evidence session**, so it is unproven. Two-handed thralls: see S9 |
| S3 | **Reanimate** | any effect archetype kReanimate. Vanilla: FF/Aimed, Reanimate projectile | Raise Zombie (plus a Script rider), Dead Thrall (BothHands, charge 3.0) | B584: unreachable in WillWork because the Summon branch returns first. B84: old roads with a living target | **YES.** The wrong target kind (a living actor). The engine's own corpse search is not used |
| S4 | **Wall** | **not** Target Location. A concentration Aimed effect whose `impactDataSet` maps a material to a `BGSImpactData` with a non-null `hazard`: the wall is the hazard the impact lays down | Wall of Flames / Frost / Storms: Conc/Aimed, Flame or Beam projectile, `MAG*BarrierImpactSet` (Flames uses `MAGFlames01ImpactSet` with the same `FlamesProjectile`) | the concentration claim (Offensive) | **Mislabel:** MFO's `Wall` rule (TL + concentration, `Archetype.cpp:282-298`) never matches a vanilla wall. A wall logs as `Plain`. The cast itself is plausibly fine (aimed at the foe; the wall forms where the flame meets the ground). **Not field-observed** |
| S5 | **Aimed AoE** | Aimed, plus an effect with `explosion` or `area` > 0 | Fireball (Missile + explosion, area 15), Ice Storm (Cone, area 15), Chain Lightning (Beam + explosion), Rout (Missile + explosion) | owned offense claim (animated, field since 09-05) | No, while aimed at an actor. Aiming at a chosen ground point is the P5 zone gambit, and needs the aim-point seat (86e3dwn9d step 3) |
| S6 | **Cloak** | Self delivery, archetype kCloak. The cloak's damage is a separate Conc/Aimed spell the cloak applies | Flame / Frost / Lightning Cloak: FF/Self, **not** Hostile-flagged | Buff, so BuffSelfClaim on the native Cloak row | Self: no (claim built, not field-run). **AUTO is unclear:** a non-hostile Self buff enters AUTO's buff series at allies. Needs an in-game test |
| S7 | **Ward** | archetype AccumulateMagnitude (32), primaryAV WardPower (63), Conc/Self | Lesser / Steadfast / Greater Ward | direct stream (`IsRestorationSpell`) or the buff claim | **YES, through the mirror bug (§2.1).** MFO predicts no row; the engine keys the Ward row |
| S8 | **Self-centred hostile AoE** | Self delivery, an effect Hostile with `area` > 0 | Fire Storm (area 100), Mass Paralysis (40), Hysteria / Mayhem (250): all BothHands | `ClassifySpell` = Offense. CastOn's owned claim needs a non-self target. The self fork sends it to the direct self stream or a claim with target 0 | **YES.** [pred] no engine row (Offensive needs `other`; Paralyze row is `other`). The rowless serve MAY hand it the Script row (unproven). Plus two-handed (S9) |
| S9 | **Two-handed / ritual (master)** | `SpellItem::IsTwoHanded()` (BothHands equip slot). Long charge is `GetChargeTime()` (the SPIT value is 0.00 on Fire Storm and 3.00 on the other masters, so read the vfunc, not SPIT) | Fire Storm, Blizzard, Lightning Storm (Conc/Aimed), Mass Paralysis, Harmony, Hysteria, Mayhem, Call to Arms, Dragonhide, Guardian Circle, Dead Thrall, Thralls | **MFO never reads `IsTwoHanded` for a spell** (Loadout's `IsTwoHanded` is weapon-only, `Loadout.cpp:93`). `Loadout::Prepare` equips LEFT. The claim TTL is 6 s with a heartbeat (`Bridge.cpp:293`) | **Probably YES:** a one-hand equip of a both-hands spell. Unobserved |
| S10 | **Self hazard** | Self delivery, archetype kSpawnHazard (40). Its nature lives one hop away: `associatedForm` is a `BGSHazard`, whose `data.spell` carries the real (hostile or not) effects | Blizzard (the hazard spell `HazardFrostBlizzardSpell03`: DualVM plus PeakVM, both Hostile), Guardian Circle | **`ClassifySpell` = Buff**, because the SpawnHazard effect is not Hostile-flagged. So Blizzard is treated as a self buff | **YES (classification bug).** [pred] no engine row for SpawnHazard |
| S11 | **Concentration beam / spray** | Conc/Aimed, projectile Beam or Flame | Flames, Sparks, Lightning Storm (BothHands) | the concentration claim (Offensive) | No (A10, animated) except Lightning Storm (S9) |
| S12 | **Script-primary** | the costliest effect (`GetCostliestEffectItem`) has archetype kScript | mod effects (Arcane Pull FE205ACE, Oak Heart FE20B802) | Script row seated. Nature-undeclared refusal applies when every effect is Script | Partly. Silent `[cfc]` claims for Arcane Pull (15 times) [field] |
| S13 | **Script rider** | an effect with archetype kScript that is NOT the costliest effect | LoreRim's Incinerate 0010F7ED (3 effects; vanilla has no Script effect), Cold Combustion FE1FF825, Raise Zombie's secondary | owned offense claim. Logged `shape=Script` because Script outranks Plain in the precedence | **Mislabel** (log). The row is right (Offensive), but silent `[cfc]` lines remain: Incinerate 18, Cold Combustion 20 [field] |
| S14 | **TL burst (no projectile, not summon)** | kTargetLocation, no projectile, no Summon | none in vanilla. The only vanilla TL spells are summons, runes and one quest ritual (DA16RitualSpell) [rec] | the TL arm lands it at the caster's magic node | Only if a mod ships one. Ask the load-time census (C0) before building anything |
| S15 | **Charged / hold-time variants** | **not identifiable from the record.** Hold time is read by scripts or perks (`CalcMagicHoldTime` 0x09 on the AI side) | Adamant "Charged Arcane Mist / Touch" [field name only] | by kind | Unknown. Treat by its other shape and log it |
| S16 | **Dual-cast-only** | **not identifiable from the record.** `dualCastData` / `dualCastScale` / `kNoDualCastMods` exist, but no "dual only" flag; scripts test IsDualCasting | n/a | `kApmfHandDualCast` hint (archer dual heal only) | Unknown. A marth rule, not a shape |

### 1.1 The field truth so far (`_evidence/`, 18 sessions, 10-26 to 11-02)

- **Only two shapes were ever dispatched by a gambit:** `Plain` and `Script` (16 unique `[archetype]` lines:
  Healing, Fast Healing, Close Greater Wounds, Candlelight, Oakflesh, Oak Heart, Incinerate, Cold Combustion,
  Arcane Pull, Blood Bolt, Reflective Light). **No rune, wall, summon, reanimate, cloak, ward, AoE or master
  spell has ever been gambited on a P0 build.** The 10-01 doc's P1 census session has not happened.
- **Harbinger `[ctcensus]`, the ENGINE-WIDE counts, every actor** (2026-10-30 / 11-01 LoreRim, fires per
  CheckStartCast):
  - Offensive 115/293
  - Restore 29/518
  - Ward 13/219
  - Light 22/205
  - Summon 2/84
  - Reanimate 10/36
  - Cloak 4/35
  - Armor 25/39
  - Paralyze 2/10
  - Script 10/10
  - BoundItem 1/1
  - Stagger, Disarm, TargetEffect, Invisibility: 0/0

  **So the engine's own AI does cast summons, reanimates, cloaks, wards, Paralyze and Script spells natively on
  1.6.1170.** Stagger, Disarm, TargetEffect and Invisibility were never seen.
- **Under an MFO claim, only Offensive, Restore, Armor and Light casters were ever built.** No claim of the
  types Summon, Cloak, Ward, Script, Reanimate or Paralyze has ever been observed.
- **841D7023 is Greater Flames,** an offense spell on the Offensive row. It appears only in APMF.log, as the
  AI's own pick refused at 0x0F because a claim named another spell. It is **not** a complex-shape case. The
  09-23 datum is closed as "a rival spell under a claim", not an archetype gap.
- Silent claims (`[cfc] ... NO observed cast`): Healing 69, Cold Combustion 20, Incinerate 18, Arcane Pull 15,
  Reflective Light 4, Oak Heart 1. The Offensive-row silents (Incinerate, Cold Combustion) are not explained by
  the archetype. They belong to gap 13 (a foreign spell already charging) or the 0x06 timing, not to this task.

---

## 2. Can the engine's NPC AI cast each shape, and what must be supplied

The mechanism, proven: casters are minted **per cast** from the equip-slot `CombatInventoryItem`'s
CreateCaster (slot 0x15). The item's row comes from the 23-row table keyed on (archetype, AV, self, hostile)
(ENGINE_NOTES §0.41; APMF `CastClassify.h:9-12`). RTTI on 1.6.1170 lists 15 CombatMagicCaster subclasses:
Armor, BoundItem, Cloak, Disarm, Invisibility, Light, Offensive, Paralyze, Reanimate, Restore, Script,
Stagger, Summon, TargetEffect, Ward. **There is no rune, wall, Target Location or self-AoE class.**

| Shape | Engine AI can cast it? | What is missing | Road (animated AI-fired unless stated) | Owner task |
|---|---|---|---|---|
| S1 Rune | Animates yes [field]; places **no** [disasm + field] | a ground point plus a real pick at release | complete-at-release launcher 34452 on the actor's own hand caster (needs a #0 amendment) | **86e3dwq70** (batch d) |
| S2 Summon | **yes**, natively (2/84) [field]. Placement is the engine's own | nothing for placement. Under a claim: proof that the Summon 0x06 seat answers | the claim (target 0) | field-prove only (C2). 86e39pz55 closed |
| S3 Reanimate | **yes**, natively (10/36) [field], with its own corpse search at 0x0A/0x0D | MFO must stop supplying a living target. Harbinger has no Reanimate seat, so a claim cannot answer WHETHER | the claim, target 0, **engine picks the corpse** | **new card C5** (subsumes B84 / B584 / B77) |
| S4 Wall | Offensive row [pred]; cast like Flames at the target actor | a chosen point only matters for the zone gambit; the at-foe cast needs nothing | the existing concentration claim; a point needs the aim-point seat | field-prove (C2); point is **86e3dwn9d step 3** |
| S5 Aimed AoE | yes (field) | a point only for the zone gambit | existing owned offense claim | point = **86e3dwn9d step 3** + MFO-B215 P5 |
| S6 Cloak | yes, natively (4/35) | claim proof; the AUTO recipient rule | the BuffSelfClaim on the native Cloak row | field-prove (C2); AUTO rule C4 |
| S7 Ward | yes, natively (13/219) | MFO's mirror must key the Ward row | the buff claim on the Ward row | **C1** (mirror alias) |
| S8 Self hostile AoE | [pred] **no row** (Offensive and Paralyze need `other`). The rowless serve may hand it the Script row | a row: either the existing rowless serve proves it (C2), or a seat-0 self-bit flip to "other" for a claimed self-delivered hostile spell (the mirror image of heal-other's flip at `+0x4c`) | the claim; the seat-0 flip is Harbinger, tier A | **C6**, gated on C2 |
| S9 Two-handed | the AI equips a both-hands spell itself under the 0x0F admission [pred]; unobserved | MFO must not equip it into LEFT; the claim should carry the dual hint; the TTL must cover `GetChargeTime()` | the claim (the AI equips) | **C3** |
| S10 Self hazard | [pred] **no row** for SpawnHazard | MFO must classify it by the hazard's spell, so Blizzard is Offense; then as S8 | as S8 | **C3** (classification) + C6 |
| S12/S13 Script | Script row natively (10/10) | log accuracy (primary vs rider) | the existing claim | C0 (log) |
| S14 TL burst | lands at the hand [disasm] | a point (TL WHERE seat 34447) | 86e3dwn9d step 1 | **only if C0 finds any** |
| S15/S16 | n/a | a marth rule | by other shape | questions |

**Out-of-combat:** every claim road above needs a live CombatController. Out of combat it waits on the combat
substrate (ch.27, S3). No new work here.

### 2.1 The engine-row mirror bug (new, a behaviour bug)

`native/cast/Archetype.cpp` `kRows` (lines 73-112) keys the ValueModifier rows on `Arch::kValueModifier`
only. Harbinger's decode shows **the engine also files every ValueModifier row under archetypes 4 (Absorb),
5 (DualVM), 32 (AccumulateMagnitude) and 34 (PeakVM)** (map builder AE 0x81DCA0 / SE 0x7816C0, APMF
`CastClassify.cpp:91-98`, APMF-B59 (e)). So MFO predicts `none` for:
- Oakflesh, Stoneflesh, Dragonhide (PeakVM, AV 39): the engine row is Armor.
- every ward (AccumulateMagnitude, AV 63): the engine row is Ward.
- the frost and shock damage spells (DualVM Health hostile): the engine row is Offensive.

`ChooseBuffRoad` routes on `.row`, so this changes behaviour: [field] `Auri cast "Oakflesh on Self":
road=direct-force predicted=none`, which is unanimated, while the census shows claimed Armor casters built 35
times and fired 22 times (10-27b). This is the strongest argument yet for the 10-01 doc's open question 6: a
Harbinger row-query slot instead of a mirror in MFO.

---

## 3. What already exists, and where each shape belongs (no duplication)

| Piece | Status | Used by |
|---|---|---|
| MFO `ClassifySpell` / `SpellKind` (CasterConsent.cpp:45-81) | live | everything. Gap: SpawnHazard (S10) |
| MFO `ClassifyArchetype` / `SpellShape` (cast/Archetype.cpp, P0) | live, log plus `.row` | gaps: §2.1 alias, the Wall rule, Script primary vs rider, no SelfHazard, `chargeTime` read from SPIT |
| MFO nature-undeclared AUTO refusal (SpellSupport.cpp:63-82, c62d4cf) | live | S12 only (every effect Script). Keep as is |
| MFO picker hide `CastRoadUnsupported` | hides aimed Light only | unchanged |
| Harbinger seat 0 classify + rowless Script serve | built, not field-run for rowless | S8 / S10 might already be served (C2 decides before C6 is built) |
| Harbinger 10 seated casters + redundancy bound | built, field: Offensive / Restore only | S2 / S6 / S7 field proof (C2) |
| `APMF_Param.posX/Y/Z` + `kCastFlag_AtPosition` (ABI 11) + marker position cast | built, OFF, never field-run, unanimated | no shape here. It refuses S1 / S2 / S4 / S11. Not an endpoint |
| `FindEmptySpace` / `FindHostilesInSpace` (ABI 11) | built | MFO-B215 P5 zone gambit |
| 86e3dwn9d point casting | step 1 WHERE seat 34447; step 3 aim-point seat | step 1 = S14 only (vanilla has none); step 3 = S4 / S5 at a point |
| 86e3dwq70 runes | batch d | S1 |
| 86e39412x gaps 10 / 12 / 13 | gap 10 closed; 13 open | the Incinerate / Cold Combustion silents (§1.1) belong here, not to this task |
| 86e39pz55 summons | done, deck-confirmed | S2 placement: nothing left |
| MFO-B215 P1-P5 | P0 merged; P1 never run | this doc's C0 / C2 are P1; C3 / C5 are P2; C6 is P3; 86e3dwn9d is P4; P5 is unchanged |

---

## 4. Cards, in order (passive probes first)

Tiers follow the CLAUDE.md cost table. Every MFO code card states "load-order agnostic, runtime-resolved, no
FormID tables". Every Harbinger card asks "does the follower animate this?" first.

**C0. Load-time spell-shape census (passive probe).** MFO, tier B, Effort 2, sonnet-coder plus one Opus pass.
- **At kDataLoaded:** walk every loaded SPEL (castable spell type, not constant effect) through
  `ClassifyArchetype`. Log one `[shape-census]` count per (shape, predicted row, two-handed) with up to 3
  example names and FormIDs each. This replaces the 10-01 doc's "test list from real FormIDs" with a
  runtime-derived one.
- **Label fixes**, which change the log only:
  - `Wall` = an effect whose `impactDataSet` has an entry with `hazard != null`, plus the shape's delivery
    and casting printed.
  - `Script` split into `ScriptPrimary` (the costliest effect) and `ScriptRider`.
  - A new `SelfHazard` (archetype 40) that prints the hazard spell's hostility.
  - `charge` read from `GetChargeTime()`.
  - Add `impactHazard=` and `twoHanded=` to the `[archetype]` line.
- **Rule:** no road may read the new labels in this card. MAP.md entry.
- **In-game test:** one load on the Deck. Grep `[shape-census]`. Confirm Wall / Rune / Summon / SelfHazard /
  TwoHanded buckets are non-empty on Tuxborn, and check whether any TL burst (S14) exists in the list.

**C1. Mirror alias fix (archetypes 4 / 5 / 32 / 34 key the ValueModifier rows).** MFO, tier B, Effort 1.
- **Change:** `Archetype.cpp` `kRows` lookup, a small change inside the existing mechanism. It changes
  routing: Oakflesh, Stoneflesh, Dragonhide and the wards go to their native claim rows (Armor / Ward)
  instead of direct-force.
- **Note:** if marth picks Q1 = query slot, C1 is still the interim.
- **In-game test:** a gambit with Oakflesh on Self, and a gambit with Lesser Ward in combat. Expect
  `road=` claim, `predicted=Armor` / `Ward`, `[ctcensus] claimed built Armor/Ward > 0` with fired > 0, and a
  visible cast animation.

**C2. Field session per shape (the old P1, LOCAL).** Deck, Effort 2, no code. Run after C0 + C1.
- **Setup:** Harbinger `[ctcensus]` on. One gambit per shape, using spells C0's census names on the test
  list: Conjure Familiar, Raise Zombie, Flame Cloak (single target and AUTO), Lesser Ward, Oakflesh, Wall of
  Flames, Fireball, Fire Rune (expect nothing placed), Fire Storm, Blizzard, Mass Paralysis, Lightning Storm,
  Calm and Fear.
- **Evidence to collect:** for each spell, which caster was claimed-built and claimed-fired, the `[cfc]`
  silents, whether the cast animates, and where the effect lands (the wall on the ground, a summon in front,
  a cloak on the caster, the Fire Storm burst).
- **Rule:** Opus concludes from the evidence file (field diagnosis rule 4). It decides C5 / C6 and whether
  C3's equip half is needed.

**C3. Two-handed and self-hazard classification.** MFO, tier B, Effort 2.
- **(a)** `ClassifySpell` follows a SpawnHazard effect to `associatedForm` → `BGSHazard::data.spell` and
  classifies by that spell's effects, one hop, no recursion past it. Blizzard becomes Offense. Guardian
  Circle is classified by its own hazard spell.
- **(b)** `IsTwoHanded()` spells:
  - MFO stops `Loadout::Prepare` from equipping them into LEFT.
  - The claim carries `kCastFlag_DualCast` as the hint, and the AI equips. Per fix-in-Harbinger, MFO adds
    no equip action of its own.
  - If C2 shows the AI does not equip both hands under a claim, that becomes a Harbinger card, not MFO.
- **(c)** The `[cfc]` silent-claim threshold accounts for `GetChargeTime()` (3-5 s rituals).
- **In-game test:** gambit Blizzard and Lightning Storm. Expect `kind=Offense`, both hands raised, the
  release, the effect visible, and no false silent warn during the 3 s charge.

**C4. AUTO recipient rule for Self-only buffs (cloaks).** MFO, tier B, Effort 1. Gated on C2 showing where an
AUTO Flame Cloak goes.
- **Change:** a Self-delivered buff under AUTO casts once on the caster and never enters the ally series.
- **In-game test:** an AUTO Flame Cloak gambit. Expect one cast on the follower, the cloak visible, no
  per-ally repeats.

**C5. Reanimate through the engine's own corpse search.** Harbinger tier A plus MFO tier B. Effort 3.
- **Harbinger:** seat the Reanimate caster's 0x06 (WHETHER) and 0x07 from the claim. Keep the engine's own
  0x0A / 0x0D (the corpse search), with a SeatVerified row.
- **MFO:**
  - Route Reanimate to a claim with target 0 and no living recipient.
  - Fix the B584 ordering in WillWork (the Summon branch returns first).
  - Liveness reads the corpse's ReanimateEffect (B84).
  - Subsumes MFO-B84 / B77 / B584 and B232 N3.
- **In-game test:** gambit Raise Zombie with a fresh corpse nearby, and with no corpse. Expect an animated
  cast on the corpse in the first case. In the second, expect one loud "no corpse" decline per fight and no
  cast at a living ally.

**C6. A row for self-delivered hostile spells** (Fire Storm, Mass Paralysis, Hysteria, Mayhem, Blizzard after
C3). Harbinger tier A, Effort 3. **Only if C2 shows the rowless Script serve does not fire them.**
- **Change:** for a CLAIMED driven form only, seat 0 flips the self bit (+0x4c) to 0 so the hostile effect
  keys the Offensive (or Paralyze) row. This is the mirror of the heal-other flip, through the same seat.
- **Ruled out:** no new engine data, no manufactured decision. The AI still decides when to fire.
- **Passive probe first:** a one-line `[ch.8b classify]` log of the row that the rowless serve hands such a
  spell.
- **In-game test:** gambit Fire Storm in a crowd. Expect the AI to charge two-handed and release, and enemies
  in 100 ft to take the burst.

**C7. Harbinger row-query slot (replace the MFO mirror)** (only if marth picks it, Q1). Harbinger tier A
(append-only ABI), then MFO tier B to consume it. Effort 2 + 1.
- **Harbinger:** returns Harbinger's decoded engine row (including the alias and the rowless serve) for
  (spell, self flag).
- **MFO:** deletes `kRows`.
- **In-game test:** C1's tests, with predicted rows identical to `[ctcensus]`.

**Unchanged, owned elsewhere (no new card):**
- runes: 86e3dwq70, batch d.
- a wall or AoE at a chosen point: 86e3dwn9d step 3 (RE about 250k, then tier A), only when the P5 zone
  gambit is scheduled.
- TL WHERE seat 34447: 86e3dwn9d step 1, which **should be descoped unless C0 finds a TL burst**. In
  vanilla, the TL spells are summons and runes only, and the engine places summons.
- the Offensive-row silent claims: 86e39412x gap 13.

---

## 5. Questions for marth (each with a recommended default)

1. **Engine-row source:** keep MFO's mirrored table (fixed by C1), or add a Harbinger row-query slot (C7) so
   MFO stops copying engine data? *Default: C1 now, C7 later in the next Harbinger ABI batch.*
2. **Self-delivered hostile spells (Fire Storm and friends):** if the field shows they never fire under a
   claim, may Harbinger's seat 0 flip their self bit, as it already does for heal-other (C6)? *Default: yes,
   claimed forms only, after C2.*
3. **86e3dwn9d step 1 (TL WHERE seat):** descope it, since vanilla has no projectile-less TL spell besides
   summons and the engine places summons? *Default: descope unless C0's census finds a TL burst in the
   list.*
4. **Charged and dual-cast-only mod spells (Adamant):** the record does not mark them. Treat them by their
   other shape and accept the AI's own hold time, or write a per-spell rule? *Default: by other shape, no
   rule, logged.*
5. **Reanimate with no corpse in range:** decline silently per fight (one WARN), or fall back to something?
   *Default: decline with one WARN per fight. No fallback (principle 7).*

---

## 6. Corrections to the record

- **Walls:** MAP.md / Archetype.cpp call a wall "Target Location + concentration". Vanilla walls are
  **Aimed** concentration spells whose impact data set lays a hazard. The 10-01 doc's A17 row and the
  rune-batch notes that group walls with TL are wrong on this point. The 09-23 research had it right.
- **841D7023:** this is Greater Flames, an Offensive-row spell refused at 0x0F under a rival claim. It is not
  an archetype gap.
- **"No row = never cast"** (10-01 fact 1) is now "no row = the rowless Script serve, unproven".
- **c62d4cf's title** cites "MFO-B235 (F-D..F-H)". On main those findings live under B236; B235 is the
  idle-hand floor.

## marth rulings 2026-11-10 (interactive)
- Q53 ground-position cast (86e3dwn9d step 1): KEEP. marth: "There are several other spells that need it. Drop zone for example needs a very specific spot where the player will be landing." The research's "no vanilla use" finding does not descope it; modded spot-cast spells (e.g. a drop zone at the player's landing spot) need a chosen ground point. Target-point choice is per-spell intent (player landing spot, foe cluster, etc.) and belongs in the gambit/target design.
- Q55 Reanimate with no corpse in range: SKIP SILENTLY (the rule falls through, no log line).
- Q54 charged / dual-cast-only mod spells: treat by their other shape, logged, no per-spell rules.
- Q51/Q52 remain open (technical): defaults stand (C1 table fix now; Harbinger row query + seat-0 self-bit flip only after the Deck census).
