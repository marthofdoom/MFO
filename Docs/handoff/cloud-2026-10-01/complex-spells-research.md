# Complex spells NPCs cannot cast properly: research and design (ClickUp 86e3dmtr1)

Read-only research pass, 2026-10-01. Trees read: MFO `origin/main` `86c99844` (v2.0.16), Harbinger (APMF)
`origin/main` `b0decf9` (v0.9.11, ABI v17), CommonLib `mit-3.7` fork at `fde0f3a` (/tmp/claude-0/cl).
`native/APMF_API.h` is byte-identical in the two repos. No repo was modified.

Rule for this document: **NEEDS LOCAL** marks anything that needs game data (.esm/.esp), a Deck log, a
Deck test or a disassembly that this cloud session does not have. Nothing below guesses a FormID or an
engine symbol. Every engine fact cites the document that recorded the disassembly.

---

## 0. The five facts the design rests on

1. **The engine's NPC combat AI picks a caster TYPE per spell from a fixed 23-row table**, keyed on
   (effect archetype, actor value, self-delivery, hostile). The spell takes the row of its highest-scoring
   effect. No row means no `CombatInventoryItem`, so no caster: the AI never casts that spell at all.
   Table at AE `0x20163B0` / SE `0x1DF2B00`, identical on both. Source: APMF `Docs/STATUS.md:139-196`
   (caster-type census head of work, disassembly of both runtimes).

   | Key (archetype, AV, self, hostile) | Engine caster |
   |---|---|
   | ValueMod Health, other, hostile | Offensive |
   | ValueMod Magicka / Stamina, other, hostile | none (null creator) |
   | Stagger / Disarm, other, hostile | Stagger / Disarm |
   | CommandSummoned, Banish, TurnUndead, hostile | TargetEffect |
   | Paralysis, hostile | Paralyze |
   | Script, hostile at other, or non-hostile on self | Script |
   | ValueMod Health / Magicka, self, beneficial | Restore |
   | ValueMod Stamina, self, beneficial | none |
   | ValueMod WardPower, self | Ward |
   | SummonCreature, self or other | Summon |
   | Cloak, self | Cloak |
   | Light, self | Light |
   | Invisibility, self | Invisibility |
   | BoundWeapon, self | BoundItem |
   | ValueMod DamageResist, self | Armor |
   | Reanimate, other | Reanimate |

   **No row at all:** Calm, Demoralize (Fear), Frenzy, Rally (Courage), every other ValueMod AV (fortify
   skills, resists, Muffle), NightEye, DetectLife, Telekinesis, Dispel, cures, and every BENEFICIAL spell
   delivered at another actor except Summon. Harbinger's seat 0 (`core/CastClassify.cpp`) only flips the
   self bit, which is how heal-OTHER reaches the Restore row. It cannot create a row.

2. **Harbinger's ch.8b claim seats (WHETHER/WHERE/HOW LONG/AIM) sit on only 2 of the 15 caster types:
   Restore and Offensive.** `EquipGate` 0x0F covers all 15 item vtables, so a claimed spell is admitted at
   equip whatever its type, but for the other 13 types the type's OWN `CheckStartCast` (0x06) decides
   whether to cast. Per-type overrides: 0x06 on every type; 0x07 on Offensive, Ward, Restore, Stagger,
   Cloak, Light, Armor; 0x0A/0x0D only Reanimate (its corpse); 0x0B Offensive, Restore. (APMF
   `Docs/STATUS.md:179-190`; CommonLib fork `include/RE/C/CombatMagicCaster*.h` agrees on the override
   sets: Summon and Script override 0x06+0x0C, Cloak and Ward 0x06+0x07, TargetEffect 0x06 only.)

3. **A Target Location spell cast by an ACTOR lands at the caster's own magic node** (or its position
   if it has no node), with a navmesh snap when the effect has `kSnapToNavMesh` (EffectSetting flag bit
   3). The cast core's Target Location arm never reads the target it was handed. Only the PLAYER gets a
   crosshair pick. (APMF `Docs/ADDRESS-TABLE-2026-09-15.md:493-503`, AE 34410 / SE 33632.)

4. **The engine launches a Target Location PROJECTILE only from that player pick.** The inline check at
   AE `0x5bd04e`-`0x5bd08e` (SE 33671 +0x8d) needs pick data with a ground-layer hit (static 1, terrain 13,
   ground 17). An NPC never has a pick, so for an NPC the projectile branch is skipped. (APMF
   `Docs/ADDRESS-TABLE-2026-09-15.md:515-526`; APMF `Docs/INVARIANTS.md` #0 (e) condition 2.) Field: a
   follower's OWN AI chooses and animates runes (Natura Stone Rune FE209C83 fired 5 and 25 times, APMF
   INVARIANTS #0 (e)). **Whether anything was placed on those casts is not recorded: NEEDS LOCAL.**

5. **An NPC's summon is placed by the engine itself.** For a Target Location spell with a Summon
   Creature effect the target becomes the caster reference, the summon effect applies only to the
   caster's own actor, and `SummonCreatureEffect::Start` picks its own spot in front of the caster when
   the given location is zero or within a set distance of it (AE 34990 `0x5d4620`, `0x5d4726`-`0x5d481f`,
   helper `0x5d50c0`). A marker can never summon. (APMF `Docs/ADDRESS-TABLE-2026-09-15.md:510-513`.)

Plus marth's release gate (MFO `Docs/STATUS.md:25-35`, 2026-09-30): **no release until every in-combat
cast is animated** (AE 1.6.1170; 1.5.97 may keep an unanimated road where needed, marth 2026-09-29, APMF
STATUS:136). MFO-B181 (reopened by marth): any in-combat instant-road use outside 1.5.97 is a defect. And
APMF INVARIANTS #0 (e) condition 8, marth verbatim: *"this is not an acceptable endpoint, proper
animations are required for ALL actions."* So an unanimated position cast can only be the delivery half
of an animated composition.

---

## 1. Archetype table

### 1.1 MFO's cast roads today (Harbinger present, in combat, AE)

| Road | Where | What it is |
|---|---|---|
| **Owned offense claim** (AI-fired, animated) | `cast/CastOn.cpp:948-990` (`ownedCast` = APMF + `bApmfCast` + not legacy + target is a foe, not self, not the player + `ClassifySpell == Offense`) | `ClaimOffenseCast` ch.8b; the follower's own AI equips, charges, aims, fires |
| **Concentration claim** (AI-fired) | `cast/DirectTarget.cpp:191-210` (at a target), `cast/DirectSelf.cpp:161-180` (self) | non-heal, non-restoration concentration: `ClaimOffenseCast(conc=true)` |
| **Heal claim road** (AI-fired, animheal phase 2) | `ComposedCast::ChooseHealRoad` `ComposedCast.cpp:701`, callers `CastOn`, `CastAuto` (`cast/Auto.cpp:325` series), `cast/DirectSelf.cpp:108`, `cast/DirectTarget.cpp:147` | Heal-kind spells, needs a combat controller |
| **Direct force** (unanimated) | `cast/Direct.cpp` `ApplySelfEffect :323`, `ApplyTargetEffect :464`; `RestorationCastDirect` `cast/Roads.cpp:269` | `CastSpellImmediate` on the kInstant caster + hand magicka deduct. Takes: FF self non-heal, wards and other Restoration non-heals, the APMF-absent/legacy degrade, OOC casts |
| **Summon one-shot** (unanimated) | `cast/Summon.cpp` `SummonOnMain :171`, `CastSummonOnce :305`; dispatched from `cast/Fire.cpp:275`, `logistics/Service.cpp:1513`, `cast/Auto.cpp:599` | main-thread decision + one `CastSpellImmediate` on the caster |
| **Legacy hybrid** | `cast/CastOn.cpp:1382-1411` (AI grace, then `ForceCast` `cast/Roads.cpp:31` = UseMagic package; `doCast` `:1618`) | a non-heal, non-offense FF cast at another actor (an ally buff), or `bLegacyCastHybrid` |
| **AUTO** | `CastAuto` `cast/Auto.cpp:308` | heal: claim series; other beneficial: direct fan `ApplyEffectFromTo :77`; hostile: direct fan |

Classification MFO uses today: `CasterConsent::ClassifySpell` (`CasterConsent.cpp:35`: any hostile or
detrimental effect = Offense, else a beneficial Health effect = Heal, else Buff),
`Actuation::IsRestorationSpell` (`cast/Direct.cpp:690`), `IsSummonSpell` (`cast/Auto.cpp:266`,
kSummonCreature only), `IsCastableSpell` (`Vocabulary.h:28`, refuses constant effect and non-spell types).
**MFO never reads Target Location, Aimed, the projectile, the area or the equip slot as a decision input**
(only `DeliveryAppliesInCall` `cast/Actuation_internal.h:715`, the range gate `cast/CastOn.cpp:178` and
`MeleeOnly` `cast/Fire.cpp:116` read delivery). Every archetype below is routed by Offense/Heal/Buff alone.

### 1.2 The table

"Engine row" = the caster type the decoded table predicts (fact 1). "Works?" = what actually reaches the
target today. Unanimated in combat fails the release gate even when the effect lands.

| # | Archetype (delivery x casting) | Examples (vanilla FormIDs NEEDS LOCAL) | MFO road today | Engine row (AI can cast?) | Works? Why not? Evidence |
|---|---|---|---|---|---|
| A1 | Self FF heal | Healing | heal claim road | Restore | Animated; animheal p2 field plan pending (MFO STATUS:45-60) |
| A2 | Self FF buff with a row | Oakflesh (Armor), Candlelight (Light), Invisibility, bound weapons (BoundItem) | direct force `ApplySelfEffect` (FF self non-heal is never claimed: the self claim at `DirectSelf.cpp:161` needs concentration) | yes, native rows | Lands, **unanimated** (release gate) |
| A3 | Self FF buff with NO row | Muffle, fortify-skill / resist buffs | direct force | **no row: the AI never casts it** | Lands, unanimated. An animated road needs a classify change (APMF) |
| A4 | Cloak (Self FF, Cloak archetype) | Flame / Frost / Lightning Cloak | direct force | Cloak, own 0x06 (cloak distance + tactical duration GMSTs) and 0x07 | Lands, unanimated. AI timing is the Cloak caster's own unless seated |
| A5 | Ward (Self concentration, WardPower, Restoration) | Lesser / Steadfast / Greater Ward | direct stream (`IsRestorationSpell`; open MFO-B58) | Ward, own 0x06 (cooldown, magicka, duration GMSTs) / 0x07 | Lands, unanimated (STATUS:33 names wards as not covered) |
| A6 | Self concentration non-restoration | Detect Life (no row), mod self streams | concentration claim, target 0 | row only if the archetype has one | **No-row spells: claim can never build -> `[cfc] NO observed cast`, cast never happens** (fails closed by design, `DirectSelf.cpp:176`) |
| A7 | Summon (SummonCreature, Target Location or Aimed: per-spell delivery NEEDS LOCAL) | Conjure Familiar / atronachs / Dremora | summon one-shot, direct, by the caster | Summon, own 0x06 (summon duration GMST) + 0x0C; not claim-seated | Lands (engine places it, fact 5), **unanimated**. 923e72d fixed spam, limit, appear window. See section 4 |
| A8 | Reanimate (Reanimate, at a corpse) | Raise Zombie, Dead Thrall, Serana's raise spells | **old roads** (MFO-B84): by ClassifySpell, so CastOn / CastSelfDirect / CastAuto with a LIVING target | Reanimate, own 0x0A/0x0D (the corpse search) | Broken: MFO supplies a living actor, the spell needs a corpse. AUTO casts it once per living party member (MFO-B77, the Serana freeze lead). `CasterHasLiveSummon` reads the caster for a ReanimateEffect that sits on the corpse (MFO-B84) |
| A9 | Aimed FF offense, incl. aimed AoE | Firebolt, Ice Spike, Fireball (explosion + area) | owned offense claim | Offensive | **Works, animated** (CAST-DELIVERY "OFFENSE CAST PORT", field since 2026-09-05). AoE aims at the actor's body: fine |
| A10 | Aimed concentration offense | Flames, Sparks, Vampiric Drain | concentration claim | Offensive (absorb: row by its highest-scoring effect) | Works, animated; duration owned by the Offensive 0x07 seat |
| A11 | Touch (contact) FF / concentration | mod touch spells; Adamant touch variants (data NEEDS LOCAL) | as A9/A10 by kind; range gate treats Touch as contact (`CastOn.cpp:178`) | row by archetype | Whether the AI closes to touch range under a claim is unobserved: NEEDS LOCAL |
| A12 | Target Actor / Aimed hostile with NO row | Calm, Fear, Fury, Rally/Courage (illusion) | **owned offense claim** (ClassifySpell = Offense because the effect is hostile) | **no row: the AI never casts it** | **Broken under Harbinger:** the claim stands to its TTL with no cast (`[cfc]` shape), then fails closed. Direct road would land it (Target Actor arm reads the target) but unanimated |
| A13 | Target Actor / Aimed with a non-Offensive row | Paralyze, Stagger/Disarm, Banish / Turn Undead / Command (TargetEffect) | owned offense claim | Paralyze / Stagger / Disarm / TargetEffect: **not claim-seated**, the type's own 0x06 decides | Unknown: the claim seats cannot answer WHETHER for these types. Census decides (case 2 below) |
| A14 | Ally buff FF at another actor (non-heal) | Heal-other-style buffs, mod buffs | **legacy hybrid** (`CastOn.cpp:1382-1411`: AI grace then ForceCast package, rooting) | **no row** (beneficial at another actor) unless seat 0 maps it to a self row (Armor, Light, Cloak, Invisibility, BoundItem, Ward) | Mostly forced, unanimated or package-rooted |
| A15 | **Rune / trap** (Target Location FF, effect has a projectile) | Fire / Frost / Lightning Rune, Natura Stone Rune | owned offense claim at the foe | Offensive (if the rune effect is ValueMod Health hostile: NEEDS LOCAL per spell) | AI picks and animates it (field), but lands at its own hand and the projectile branch is skipped (facts 3, 4). **Most likely places nothing: NEEDS LOCAL to confirm.** Direct road takes the same arm, same result. Harbinger position cast REFUSES a projectile spell |
| A16 | Target Location FF, no projectile | location bursts (vanilla examples NEEDS LOCAL) | owned claim / direct | by archetype | Lands at the caster's magic node, not at the foe. Harbinger position cast can put it at a point, unanimated, off by default |
| A17 | **Wall** (Target Location concentration) | Wall of Flames / Frost / Storms | concentration claim (Offense) | Offensive if ValueMod Health hostile (NEEDS LOCAL) | Same TL arm: at the hand, projectile (barrier type? NEEDS LOCAL) needs a pick. Position cast refuses concentration |
| A18 | Master / ritual (two-handed, long charge) | Fire Storm, Blizzard, Mass Paralysis, Dead Thrall | by kind; **MFO never reads `SpellItem::IsTwoHanded()`**; `Loadout::Prepare` always equips into the LEFT slot (`CastOn.cpp` comment ~:1180 records the dual/right gap) | self-delivered hostile ValueMod Health has **no row in the decoded table** (verify in census); Mass Paralysis = Paralyze row | Likely broken twice: a two-handed spell equipped in one hand, and no engine row for self AoE destruction. NEEDS LOCAL |
| A19 | Script archetype (Adamant / Mysticism / Apocalypse scripted effects) | mod data NEEDS LOCAL | by ClassifySpell; Script summons are not `IsSummonSpell` (MFO-B75) | Script row only for hostile-at-other or non-hostile-self; Script caster not claim-seated; beneficial Script at another actor has no row | Direct road works (the script runs on the effect). AI road unseated: census decides. A scripted summon spams under AUTO (MFO-B75) |
| A20 | Charged / variable charge (Adamant "Charged Arcane Mist / Touch") | mod data NEEDS LOCAL | by kind | by archetype | If the script reads hold time: the AI's hold is `CalcMagicHoldTime` (0x09, Offensive only); the direct road has no charge at all. NEEDS LOCAL |
| A21 | Dual-cast-only / dual-cast-sensitive | perk-gated dual effects, mod "dual only" spells | owned claim can ask `kApmfHandDualCast` (`CastOn.cpp:972`); equip stays LEFT only | by archetype | Partial: the claim says dual, the physical equip does not. Direct road never dual-casts. Which spells need it: NEEDS LOCAL |
| A22 | Constant effect | abilities | excluded from the board (`IsCastableSpell`) | n/a | n/a |

### 1.3 What fails, grouped by WHY

1. **No engine row** (A3, A6, A12, A14, probably A18): the AI cannot cast it at all, so any Harbinger
   claim stands silently to its TTL. This is the most likely shape of the 841D7023 datum (section 5).
2. **Row exists but is not claim-seated** (A4, A5, A7, A13, A19, A2): the AI might cast it under a
   claim, on the type's own native timing. Unproven: the census decides.
3. **Target Location needs a point the engine never gives an NPC** (A15, A16, A17): the AI animates the
   cast but delivers at its own hand, and a projectile never launches.
4. **MFO supplies the wrong target kind** (A8 Reanimate: a living actor instead of a corpse).
5. **MFO equips wrong** (A18 two-handed, A21 dual).
6. **Unanimated by MFO's own choice** (A2, A4, A5, A7 on the direct road): effect lands, release gate fails.

---

## 2. What exists to build on

| Piece | Where | State |
|---|---|---|
| `CasterConsent::ClassifySpell` / `SpellKind {Offense, Buff, Heal}` | `CasterConsent.cpp:35`, `.h:32` | Shipped; effect-based, delivery-blind |
| `Actuation::IsRestorationSpell`, `SpellHealsHealth` | `cast/Direct.cpp:690`, `:675` | Shipped |
| `IsSummonSpell` (kSummonCreature only) | `cast/Auto.cpp:266` | Shipped |
| `IsCastableSpell` | `Vocabulary.h:28` | Shipped |
| APMF ch.8b seat 0 classify (self-bit flip for a claimed non-hostile effect) | APMF `native/core/CastClassify.{h,cpp}` | Shipped, AE + SE |
| APMF caster-type census `[ctcensus]` (passive 0x06 + 0x0B on all 15 types, predicted row, BUILT / FIRED / ZERO verdicts per claim window) | APMF `native/core/CasterTypeCensus.{h,cpp}`, merged `da28d50`, `[Probe] bCasterTypeCensus` default 1 for the field | **The principle-5 instrument this task needs. It only sees CLAIMS** (APMF STATUS:218-222) |
| `APMF_Param.pos` + `kCastFlag_AtPosition` (position cast) | APMF_API.h:1210-1293, `core/PositionCast.cpp` | **Merged** (`614bcad`, v0.9.8, ABI v11). RequestEx only, `[PositionCast] bPositionCast=0` by default, unanimated marker RemoteCast. Accepts only Target Location + FF + no summon + **no projectile** |
| Space queries `FindEmptySpace` / `FindHostilesInSpace` | APMF_API.h:1811-1930, `core/SpaceQuery.cpp` | **Merged** (ABI v11). Main thread only (MFO's `MainThread::Post` qualifies). Hostile query = point + radius + side actor, capped at 64 (`kMaxHostileResults`) |
| `kTravel_ToPosition` | APMF_API.h:721 | Merged; MFO already uses it (`apmf/Excursion.cpp:508-535`) |
| `kCastFlag_DualCast`, `kCastFlag_LeftHand`, `kCastFlag_Concentration`, stop percent | APMF_API.h:1054-1308 | Shipped |
| Summon one-shot (923e72d) | `cast/Summon.cpp` | Shipped v2.0.13: per-spell liveness, engine appear window (`fMagicSummonMaxAppearTime`), the list's summon limit (`iMaxSummonedCreatures` + `kModCommandedActorLimit`, round half-up), the engine skip-cap flag behind its self-check rows, every engine read on the main thread, worker result always transparent, never a stream, never dismissed |
| Branch `feat/apmf-position-cast-and-space-queries` | APMF | Exists and is **merged into main** (`git merge-base --is-ancestor` true) |

MFO does not yet call `kCastFlag_AtPosition` or either space query (only `kTravel_ToPosition`).

---

## 3. Design: recognise the archetype, supply what the AI cannot

Roles (unchanged from every other channel): **MFO declares** WHAT spell, at WHOM or WHERE, for HOW LONG.
**Harbinger executes** by composing the engine's own seats. Harbinger selects nothing. Where no seat can
compose, the gap is stated, not masked (principle 7).

### 3.1 The archetype classifier (MFO, data-driven, passive first)

A new file in the cast subsystem, `native/cast/Archetype.cpp` with its declarations in `cast/Actuation.h`
(CLAUDE.md: a new mechanism is a new file in its subsystem). A pure function of the spell record, cached per
FormID (record data is static after load):

```
struct SpellArchetype {           // every field read from the records, never guessed
  Delivery delivery;  CastingType casting;          // SpellItem::GetDelivery / GetCastingType
  SpellKind kind;     bool restoration;             // existing classifiers, unchanged
  EngineRow row;      // predicted caster type from the mirrored table (fact 1), or NoRow
  Shape shape;        // Summon, Reanimate, BoundWeapon, Cloak, Ward, Light, Script,
                      // Rune (TL + projectile), Wall (TL + concentration), TLBurst (TL, no projectile),
                      // AimedAoE (projectile + explosion/area), Touch, SelfAoE, Plain
  bool twoHanded;     // SpellItem::IsTwoHanded() (equip slot flag 1)
  bool noDualMods;    // SpellItem data.flags kNoDualCastMods
  float chargeTime, range, area;   // SPIT chargeTime / range; max EFIT area over effects
  bool snapToNavMesh; // any effect flag kSnapToNavMesh
};
```

Verified inputs (CommonLib fork at `fde0f3a`): `EffectSetting::data.archetype` (+0x58), `primaryAV`
(+0x5C), `secondaryAV` (+0x78), `projectileBase` (+0x60), `explosion` (+0x68), `castingType` (+0x70),
`delivery` (+0x74), `associatedSkill`, flags `kHostile`/`kDetrimental`/`kSnapToNavMesh`;
`BGSProjectile::IsMissile/IsGrenade/IsBarrier` (+ `data.types`); `SpellItem::data` `flags`
(`kNoDualCastMods`), `spellType`, `chargeTime`, `castDuration`, `range`, `IsTwoHanded()`;
`Effect::effectItem.area/duration/magnitude`; `EffectArchetypes::ArchetypeID` (kScript 1,
kCommandSummoned 10, kLight 12, kBoundWeapon 17, kSummonCreature 18, kReanimate 22, kCloak 35, ...).

**The engine-row mirror.** The 16 rows of fact 1 as a constexpr table, keyed the way the engine keys it
(archetype, AV byte for the 17 AV-keyed archetypes, self, hostile; highest-scoring effect wins). MFO cannot
read the engine table or the effect score without new addresses, so this is a PREDICTION, labelled
`predicted=` in every log line, and the census's own decoded prediction (`[ctcensus] OPEN ... predicted
native[...] seat0[...]`) is the authority it is checked against. Ordering among multiple effects (the
highest score) is approximated by "costliest effect" (`MagicItem::GetCostliestEffectItem`) and logged as an
approximation. **Open question for marth:** a mirrored table is engine data copied into MFO. The
alternative is a Harbinger query slot (append-only ABI) that returns Harbinger's decoded row for a spell.
The first slice uses the mirror, passive only, so nothing depends on it being exact.

Shape rules (all from verified fields, per effect, then per spell):
- `Summon` = any kSummonCreature; `Reanimate` = any kReanimate; `BoundWeapon`, `Cloak`, `Light` by archetype;
  `Ward` = ValueMod with primaryAV `ActorValue::kWardPower` (= 63 in the fork's `RE/A/ActorValues.h:71`).
- `Rune` = delivery Target Location, FF, an effect with `projectileBase != null`.
- `Wall` = delivery Target Location, concentration.
- `TLBurst` = Target Location, no projectile on any effect.
- `AimedAoE` = Aimed with an effect explosion or area > 0.
- `SelfAoE` = Self with a hostile effect and area > 0.
- `Script` = any kScript effect (marks "MFO cannot see what it does").

### 3.2 Per-archetype: what MFO supplies, what Harbinger must provide

| Shape | Target MFO declares | Timing / duration MFO declares | MFO-only | Needs a Harbinger facet |
|---|---|---|---|---|
| Aimed / Target Actor with Offensive or Restore row (A9, A10, A1) | the actor | today's | nothing new | nothing new |
| Self buff / cloak / ward / bound / light with a native row (A2, A4, A5) | self (target 0) | FF: once; conc (ward): stop percent / TTL | route to a claim instead of the direct force | **per-type 0x06 WHETHER seat** on the types the census proves BUILT-NOT-FIRED (APMF-internal, no ABI change: the claim already names the spell) |
| Summon (A7) | **self only** (never a foe, never a point) | one-shot: claim, release on the observed fire | keep 923e72d's main-thread decision, then claim | Summon type 0x06 seat if census says BUILT-NOT-FIRED. **No position: the engine places an actor's summon (fact 5)** |
| Reanimate (A8) | **a corpse**, or nothing | one-shot | stop handing it a living actor: either claim with target 0 and let the Reanimate caster's own 0x0A/0x0D find the corpse (engine-native), or pick a corpse MFO-side and refuse when none | Reanimate is not claim-seated: census first. Do NOT route it through Offensive seats (it would lose its corpse targeting, APMF STATUS:203) |
| No-row hostile (A12, illusion) and no-row beneficial (A3, A6, A14) | the actor / self | as today | **stop claiming a road the engine cannot build** (today: silent TTL claim then fail closed) | the only animated answer is a **seat-0 row substitution** (classify a claimed no-row spell into the Offensive or Restore row, which then answer WHETHER/WHERE/HOW LONG from the claim). New engine data, a new design, **marth decides** (APMF STATUS:200-204) |
| Rune, TL burst (A15, A16) | **a ground point** (from `FindEmptySpace` / a foe's feet / a cluster centre) | FF once | pick the point | **new engine seat: a claimed actor's Target Location cast takes the claim's point and a synthesized ground pick** (the cast core's location helper AE 34447 and the pick at AE 34457 / the inline check AE `0x5bd04e`), so the follower's own animated cast lands at the point and the rune projectile launches. This is exactly the "animated path ... composed through the engine's seats" #0 (e)(8) asks to be researched. ABI: a new cast flag bit plus a point appended at the END of `APMF_CastRequest` (append-only, `abiVersion >= 18`). The existing unanimated position cast stays the stepping stone it was approved as |
| Wall (A17) | a ground point (and a facing) | concentration, stop by TTL | pick point + facing | the same TL seat held across a channel; plus whatever the barrier projectile needs (NEEDS LOCAL disassembly). Highest-risk row; last |
| Two-handed / ritual (A18) | by shape | long charge: TTL floor above `chargeTime` | equip into both hands (dual claim + physical equip of both slots) when `IsTwoHanded()`; extend the TTL by chargeTime | if the decoded table has no row for self hostile AoE: the seat-0 row substitution above |
| Dual-cast (A21) | by shape | | physical equip must match `kApmfHandDualCast` (close the documented gap) | none new |
| Script (A19) | by ClassifySpell | | log `shape=Script` so the field can see it; a scripted summon needs a marth rule (cast once, re-cast after the effect's duration) because MFO cannot see the creature (MFO-B75) | Script type 0x06 seat if census says so |
| Charged (A20) | by shape | the hold time, if a script reads it | NEEDS LOCAL first | maybe a 0x09 hold-time answer (Offensive only today) |

### 3.3 Tiers (CLAUDE.md cost table)

- **Tier B (Sonnet author, one Opus pass):** the classifier and its logging (passive); routing changes that
  only pick a different EXISTING road (summon to a claim, reanimate target fix, no-row refusal, two-handed
  equip, dual equip). These are logic inside existing mechanisms.
- **Tier A (Opus author, full Opus review until nothing above SEV-3):** every Harbinger engine seat (per-type
  0x06 seats, seat-0 row substitution, the Target Location point seat), every ABI bump, and the zone
  condition's new serialized opcode in `Vocabulary.h` (an opcode string is a co-save contract).

### 3.4 Phased plan

| Phase | Where | What | Gate |
|---|---|---|---|
| **P0** (cloud, now) | MFO | Classifier + passive `[archetype]` logging + `[cfc]` enrichment + summon landing probe (section 6). No behaviour change | CI green, Opus one pass |
| **P1** (LOCAL) | Deck | One session with census on, a gambit per archetype row (test list built locally from real FormIDs), Serana repro, 841D7023 identified | marth reads the evidence; Opus concludes (field diagnosis rule 4) |
| **P2** | MFO | Fix the MFO-only rows the evidence confirms: Reanimate target (MFO-B84/B77), no-row refusal or marth's choice, two-handed / dual equip, summon and self buffs moved to claims where the census shows FIRED natively | tier B per change |
| **P3** | APMF | Per-type 0x06 seats where the census shows BUILT-NOT-FIRED; seat-0 row substitution only if marth chooses it | tier A each, own brief |
| **P4** | APMF + MFO | Target Location point seat (research brief first: disassembly of the TL arm, the pick, the barrier projectile), ABI v18; MFO declares points for runes / bursts / walls | tier A |
| **P5** | MFO (+ APMF queries, already merged) | The future gambit condition "N+ enemies in zone" (default 2) -> AoE cast with the point inferred from the spell: centre from `FindHostilesInSpace` (point + radius from the spell's area, hostility relative to the caster, capped count), landing from `FindEmptySpace`; aimed AoE needs no point (aim at the densest foe), Rune / TL need P4 | tier A for the opcode |

---

## 4. Worked case: the summon fix (86e39pz55)

### 4.1 Done in 923e72d (v2.0.13)

One-shot on every target and table, per-spell liveness, the engine appear window, the list's summon limit
computed as the engine does, the engine skip-cap flag behind self-check rows, every engine read on the main
thread, transparent worker result, never a stream, no hand lock, never dismissed by MFO
(`cast/Summon.cpp:171-300`; MAP.md cast/ summon block; CAST-DELIVERY "SUMMON RULE"). Deck-confirmed
"summons stay up" (MFO STATUS:391).

### 4.2 What is left

1. **Animation (the release gate).** The summon is a `CastSpellImmediate` on the kInstant caster:
   unanimated (MFO STATUS:34 lists summons as not covered). This is ClickUp 86e3dvkwm per CAST-DELIVERY.
   Design: keep `SummonOnMain`'s decision closure exactly as it is (live, landing, limit, can-act, magicka);
   where it now calls `CastSpellImmediate`, it instead files a `kIntent_Cast` claim naming the summon spell,
   **target 0 (self)**, the hand from the per-hand plan, bounded TTL, and releases on the first observed
   fire (one-shot, never a stream; the claim never enters `g_selfCast`). The engine row is Summon (native,
   "self or other"), `EquipGate` 0x0F admits it, and the Summon caster's own 0x06 decides when. The census
   window answers which: FIRED ANIMATED (done), BUILT NOT FIRED (Harbinger seats 0x06 on Summon), NOT BUILT
   (research the upstream gate). The magicka deduct goes away on the claim road (the engine charges the real
   cast). Without a controller, out of combat, or on 1.5.97 if needed: today's direct road stays, labelled.
2. **The ground-locked position.** Research result: **for an actor caster the engine already does this.**
   `SummonCreatureEffect::Start` picks its own spot in front of the caster (fact 5), and a marker can never
   summon (Harbinger refuses summons in a position cast by name). So MFO should NOT supply a point for a
   summon, and the claim's target must be self, never the foe (the Target Location arm would ignore a foe
   target anyway, fact 3). A point becomes necessary only if the field shows bad placement (in the air,
   inside geometry, falling). Then the fix is a Harbinger seat on the summon's start location fed by
   `FindEmptySpace(origin = caster, distance, clearance, maxDrop)`: tier A, own brief, only on evidence.
   **NEEDS LOCAL:** where summons actually appear today (P0 adds the probe that records it).
3. **Backlog that touches summons** (MAP.md summon block): MFO-B75 (Script-archetype summons fan out,
   invisible to liveness), MFO-B76, MFO-B77 (Serana's Reanimate fan-out), MFO-B84 (Reanimate on the old
   roads, liveness reads the caster), MFO-B85 (HMS credit), MFO-B86, MFO-B87. Task 86e3e89w7 (per-gambit
   summon count) builds on the same closure.

### 4.3 Serana's summon hard-freeze

**Known:**
- marth reported that Serana's summon, cast from a gambit, could hard-freeze the game. v2.0.13's CHANGELOG
  says the one-shot fix *may* be the cause and asks for a retest; it explicitly says raise-dead spells are
  not affected by the fix.
- Pre-923e72d shapes that could stack engine work on one frame: AUTO cast a summon once per party member at
  the same moment (fixed `dd4c56f`), and the self stream dispelled and recast the summon every ~3 s (fixed
  `78f9631`).
- **Open lead MFO-B77:** Serana's raise spells are Reanimate archetype; `IsSummonSpell` does not match them,
  so AUTO still casts them once per LIVING party member through `CastSpellImmediate` (a Reanimate aimed at a
  living actor). MFO-B84: the same spells still take CastOn / CastSelfDirect, and the self stream's
  stale-dispel loop can hit a `cast_self` raise row.

**NEEDS LOCAL to diagnose:**
- Which spell froze: its FormID and its archetype (kSummonCreature vs kReanimate vs Script), from Serana's
  NPC record and any mod that edits her spell list (Dawnguard.esm plus the load order).
- Which road and target setting the gambit used (self / player / foe / AUTO) and the MFO version.
- Whether it still reproduces on v2.0.13+ with one gambit per spell, each target setting.
- The last MFO.log / APMF.log lines before the freeze, and a stack of the hung process (a hard freeze leaves
  no crash log; capture method depends on the Deck / Proton tooling).
- P0's breadcrumb (section 6) makes the last log line name the engine call MFO was about to make.

---

## 5. Field datum: 841D7023

`[cfc] 750012C6 kIntent_Cast claim live ~3.6-3.9 s with NO observed cast (spell 841D7023, left hand)`, 14x
in v2.0.12, 4x in v2.0.11.

What the code says about the shape: the warning fires once a claim passes `kSilentWarnAfter` = 3500 ms with
no observed fire and no live cast state on that hand (`ComposedCast.cpp:162`, `:233-258`). Harbinger's
default claim TTL is 4000 ms (`kCastDefaultTtlMs`, APMF_API.h). A claim seen at 3.6-3.9 s every time is a
claim that never charged and then ended at about its TTL, over and over. That matches census case "NOT BUILT"
(no row, or the upstream gate) or "BUILT NOT FIRED" (a non-seated type's own 0x06 said no).

Candidates by the table (hypotheses only, not a conclusion): a no-row hostile spell (A12, illusion, which
ClassifySpell calls Offense), a Script-row or other non-seated hostile spell (A13 / A19), or a Target
Location spell (A15 / A17). **Identity is NEEDS LOCAL:** `0x84` is a load-order index, so only marth's load
order resolves the plugin and record. With P0 shipped, the next log prints the spell name, delivery,
casting type, shape and predicted engine row on that same `[cfc]` line, so the identity and the archetype
arrive without a load-order lookup. Per field-diagnosis rule 4, the conclusion belongs to an Opus pass over
that evidence.

---

## 6. Recommended first slice (cloud-buildable, passive, principle 5)

**Branch `feat/mfo-spell-archetype-probe` (tier B: Sonnet author, one Opus pass).** No behaviour change:
every road, gate and outcome byte-identical; only log lines added.

1. `native/cast/Archetype.cpp` (new file, cast subsystem) + declarations in `cast/Actuation.h`: the
   `SpellArchetype` classifier of section 3.1, record reads only, cached per FormID under a leaf mutex
   (callers are the worker and the main thread; spell records are static after load).
2. `[archetype]` line, once per (follower, spell) per session, when a gambit rule first dispatches a spell:
   name, FormID, delivery, casting, kind, restoration, shape, predicted engine row, twoHanded, noDualMods,
   chargeTime, area, and **the road MFO is about to take** (owned claim / conc claim / heal claim / direct /
   summon / legacy). Plus one WARN-level line when the predicted row is NoRow and the road is a Harbinger
   claim ("the engine has no caster for this spell; this claim cannot fire").
3. Enrich the existing `[cfc] ... NO observed cast` warning (`ComposedCast.cpp:253`) with the spell's name,
   shape and predicted row. This alone answers the 841D7023 question in the next deck log.
4. Summon landing probe: after a successful `SummonOnMain` cast, a main-thread one-shot check after the
   appear window logs the commanded actor's offset from the caster (distance, height difference from the
   caster's feet). Answers "is a ground-locked point needed?".
5. Serana breadcrumb: a flushed `[summon]` / `[cast]` line immediately BEFORE each `CastSpellImmediate` on
   the summon and direct roads, naming spell, archetype and target, so a freeze's last line names the call.
6. MAP.md cast/ entry for the new file; the stale file:line cites found while reading (MAP says
   `cast/Fire.cpp:215` and `logistics/Service.cpp:1060` for the summon dispatch, current main is
   `cast/Fire.cpp:275` and `logistics/Service.cpp:1513`).

Field read for that slice, alongside Harbinger's `[ctcensus]` (default on), one session with a gambit per
archetype row of section 1.2. That evidence, not this document, decides P2/P3.

---

## 7. What marth must decide

1. **Release-gate scope:** which archetypes must be animated before a release, and whether the direct road
   stays acceptable anywhere in combat on AE (summons, wards, self buffs, no-row spells). Today's rule
   (MFO-B181) says no.
2. **No-row spells** (illusion Calm/Fear/Frenzy/Courage, fortify buffs, Detect Life, ally buffs): accept
   unanimated direct delivery, refuse them in combat, or commission the Harbinger **seat-0 row substitution**
   (new engine data, tier A, a new design).
3. **Runes / walls / location bursts:** commission the Target Location point seat research (P4, deep
   disassembly), and confirm that the existing unanimated position cast stays off.
4. **Reanimate:** let the engine's own corpse search pick the corpse (claim, target 0), or have MFO pick a
   corpse and declare it.
5. **Script-archetype summons:** what rule MFO uses when it cannot see the creature (cast once, re-cast after
   the effect's duration?).
6. **The engine-row mirror:** keep a copied table in MFO, or add a Harbinger query slot returning Harbinger's
   decoded row (append-only ABI).

---

## 8. NEEDS LOCAL (consolidated)

- Spell 841D7023: plugin and record (load-order index 0x84), archetype.
- Serana: the freezing spell's FormID and archetype, road, repro on v2.0.13+, last log lines, a hung-process
  stack.
- Per-spell record data for every example in section 1.2: vanilla rune / wall / summon / master / illusion
  FormIDs, delivery, projectile type, area, two-handed flag; Adamant / Mysticism / Apocalypse records for
  the scripted, charged and dual-only rows.
- Whether an NPC's AI-fired rune (A15) or wall (A17) places anything (deck observation plus log).
- Census verdicts per archetype under claims (needs MFO to file a claim for each kind).
- Where direct-road summons appear (the P0 probe).
- Whether the AI closes to Touch range under a claim (A11).
- Disassembly for P4: the cast core's Target Location arm, location helper (AE 34447), pick (AE 34457),
  the projectile placement check (AE `0x5bd04e`, SE 33671), the barrier projectile path, and
  `CombatProjectileAimController` for aiming at a point (zone AoE).
