# IAUS I8: skill fit + engine damage estimate, research (2026-11-10)

Target path in the repo: `Docs/research/2026-11/iaus-i8-damage-research.md`. Ruling: `Docs/research/2026-11/class-walkthrough-decisions.md`, sections "IAUS mage melee" and "IAUS skill fit + damage estimate".

Method: everything below comes from disassembling the unpacked binaries (`binaries/1.6.1170/SkyrimSE.unpacked.exe` = AE, `binaries/1.5.97/SkyrimSE.unpacked.exe` = SE) with capstone. Ids were resolved through the installed Address Library bins (versionlib-1-6-1170-0 / version-1-5-97-0). GMST names were read from each static `Setting` object (value at +8, name pointer at +0x10). Symbols were checked against the fork `_commonlib/mit-3.7-fork` at `0b904231`. **1.7.104 was NOT disassembled.** Every id below needs its 1.7.104 row from the generator's id map before use there.

Proof levels:
- **PROVEN**: read in the disassembly on both AE and SE, with the inputs and GMST names decoded.
- **DISASM-ONLY**: the path exists in the disassembly, but it has not been observed running, or only one runtime was read.
- **UNPROVEN**: not established.

Per house principle 5, nothing here has been observed running. Every row still needs its passive probe (section 4) before it drives scoring.

## 1. What MFO already has (reuse, do not fork)

- `BGSEntryPoint::HandleEntryPoint` (SE 23073 / AE 23526) already has a VerifiedAddresses row on all three runtimes (`native/VerifiedAddresses.h:85` (1.6.1170), `:153` (1.5.97), `:222` (1.7.104), name `TradePerks.BGSEntryPoint.HandleEntryPoint`). The guard pattern is in `cast/CostParity.cpp:217-227`. House rule (CostParity.cpp:277, Progression.h lockpick block): **HandleEntryPoint is main thread only**, because it evaluates perk conditions.
- Magicka cost per caster is `MagicItem::CalculateMagickaCost(Actor*)`, called on the main thread everywhere (Board.cpp:1754, cast/Auto.cpp:860 ...). The dual cost is `Loadout.cpp:347-362` (`kDualCastCostMult` with the perk, else x2). Reuse it for "per magicka".
- `Progression.cpp:89` has the entry-point name table (0 CalculateWeaponDamage, 26 ModBashingDamage, 28 ModPowerAttackDamage, 29 ModSpellMagnitude, 30 ModSpellDuration, 35 ModAttackDamage, 36 ModIncomingDamage, 37 ModTargetDamageResistance, 38 ModSpellCost).
- `ArmorScore` / `TradeBridge::ArmorScoreOf` (TradeBridge.h:173-178) is the follower's OWN armor-piece judge (rating x class/skill bias). It is **not** target mitigation and does not feed this estimate.
- `TESObjectWEAP::GetAttackDamage()` raw reads (cast/Equip.cpp:697, EquipSilver.cpp:408, TradeBridge.cpp:502) take the record value only: no skill, perks or GMSTs.
- IAUS: `Facts` (`native/iaus/Core.h:201-262`) already carries `resist` (the target's resist AV for the spell's element), `cost`, `magicka`, `reach`. `SpellEntry` (`native/iaus/Snapshot.h:28-52`) carries `resistAv` and `cost`, measured on the MAIN thread by `SpellBook::Measure` (`iaus/SpellBook.cpp:239`, posted by `PostRefresh` at :~317). The book is `Snapshot.h:70` (`entries` + `Kit`).
- There is no existing weapon-damage, magnitude-after-perk or armor-mitigation computation anywhere in `native/` on main.

## 2. Per-item findings

### 2.1 Weapon damage for an NPC attacker

The engine has ONE core per-hit formula. It is wrapped twice: once by the inventory "displayed damage" path, and once by the real hit path.

| Piece | AE id | SE id | Fork symbol | What it is | Proof |
|---|---|---|---|---|---|
| Displayed damage | 40253 | 39179 | `PlayerCharacter::GetDamage(InventoryEntryData*)`: `include/RE/P/PlayerCharacter.h:557`, `src/RE/P/PlayerCharacter.cpp:115`, `Offsets.h:453` | WEAP: `calc(this, weap, nullptr, entryHealth)`. AMMO: `calc(this, nullptr, ammo, 1.0)` | PROVEN |
| Display calc | 40291 | 39215 | none | `core(AVOwner(this), weap, ammo, health, 1.0, false)`, then EP 0x23 kModAttackDamage then EP 0x00 kCalculateWeaponDamage, both `(owner=this, weap, **PlayerCharacter singleton**, &v)`, then `x GetActorValue(kAttackDamageMult 154)` | PROVEN |
| **Core formula** | **26410** | **25847** (skill mult factored into helper 25845) | none | see below | PROVEN |
| Real hit | 44001 | 42832 | `HitData::Populate(Actor* aggressor, Actor* target, InventoryEntryData*)`: `include/RE/H/HitData.h:44-49` | `id16016/15752 wrapper -> 26409 -> core` with `mult = attackData->data.damageMult` (else 1.0); EP 0x1A bash; **EP 0x1C kModPowerAttackDamage `(aggressor, weapon, target, &dmg)` only when the attack is flagged power**; EP 0x00 `(aggressor, weapon, target, &dmg)`; then 44016/42844 (EP 0x23, EP 0x24), 44014/42842 (armor, 2.3), 44015 | DISASM-ONLY (AE read; SE call sites match by EP census) |
| Engine AI DPS | 26411 | 25848 | none | `core(AVOwner, weap or fists(401061), ammo, 1.0, 1.0, false) x weaponData.speed x (bow/crossbow ? fCombatDPSBowSpeedMult : fCombatDPSMeleeSpeedMult)` | PROVEN (AE read; SE id is a direct caller of 25847) |

**Core formula** (AE 26410, decoded; SE 25847 + 25845 use the same GMST names), `float core(ActorValueOwner* av, TESObjectWEAP* w, TESAmmo* ammo, float itemHealth, float attackMult, bool useEquippedAmmoIfPlayer)`:
```
skillAV  = w->weaponData.skill            (+0x190; fork TESObjectWEAP.h:176)
base     = (w->GetAttackDamage() + ammo->data.damage) * fDamageWeaponMult
temper   = f(itemHealth) using fSmithingWeaponMax  (AE 26498 / SE 25915; two further unnamed settings)   [melee, bow, crossbow; not staff]
addAV    = HandToHand: GetActorValue(kUnarmedDamage 35); 1H/2H melee: GetActorValue(kMeleeDamage 34); bow/staff: 0
skill    = GetClampedActorValue(skillAV)   (AE 27284 / SE 26616 = fork ActorValueOwner::GetClampedActorValue, Offsets.h:32): GetActorValue then clamp [0,100]
skillMul = av->IsPlayer() ? fDamagePCSkillMin + (fDamagePCSkillMax - fDamagePCSkillMin) * skill * 0.01
                          : fDamageSkillMin   + (fDamageSkillMax   - fDamageSkillMin)   * skill * 0.01
           (1.0 when the skill AV is outside 6..23)
result   = ((base + temper) * skillMul * attackMult + addAV) * finalMul
finalMul = 1.0, except an unnamed global (AE 410199) when weaponData.flags bit 0x02 and a singleton (AE 400883)+0x20 == 0  [UNPROVEN meaning; the fork names that bit kAutomatic, "unused"]
```
GMST ids (AE / SE), PROVEN by name: fDamageWeaponMult 374110 / 504995, fDamageSkillMin 374113 / 504997, fDamageSkillMax 374116 / 504999, fDamagePCSkillMin 374119 / 505001, fDamagePCSkillMax 374122 / 505003, fSmithingWeaponMax 375804 / 506043, fCombatDPSMeleeSpeedMult 374467, fCombatDPSBowSpeedMult 374470 (SE ids not decoded).

Answers:
- **Is there ONE function for "final per-hit damage, actor + weapon"?** Not for an NPC against a target.
  - `PlayerCharacter::GetDamage` (fork symbol) only uses `this` as an Actor, so a non-player `this` is legal by disasm. But it hard-codes the **player** as the perk-condition target of EP 0x23 / EP 0x00, it skips power-attack (EP 0x1C) and the attack's damageMult, and it applies no mitigation. It is right for an item card and wrong for "follower vs this foe".
  - The exact engine path is `HitData::Populate`, which needs a live HitData and fires mid-swing: not callable as an estimate.
  - So the estimate has to be composed: **core (id call) + HandleEntryPoint with the REAL target**, in the same order as Populate.
- Power-attack multiplier: `BGSAttackData::data.damageMult` (fork `BGSAttackData.h:26`, data at +0x18 `:54`) feeds `attackMult`. Then EP 0x1C is evaluated when the attack data's flags say power attack. The per-race attack data that would supply damageMult for a power attack is not resolved here, so it is UNPROVEN as a pre-swing read. The fallback is attackMult = 1.0 plus EP 0x1C only.
- `kAttackDamageMult` (AV 154): proven on the display path. Whether the hit path applies it as well (inside 44016) is UNPROVEN.
- Thread: the core body only reads (AV vfuncs, GMST statics, the IsPlayer vfunc, record fields), and no write was observed. By house rule, any id call on a live actor plus HandleEntryPoint goes on the **main thread** (`SpellBook::Measure`).

### 2.2 Spell magnitude after perks

| Piece | AE id | SE id | Fork symbol | What it is | Proof |
|---|---|---|---|---|---|
| Per-effect magnitude adjuster | 34048 | 33273 | none | `HandleEntryPoint(0x1D kModSpellMagnitude, caster, spell, targetActor, &mag)`. Then, if `dual` and the MGEF has `kPowerAffectsMagnitude` (fork `EffectSetting.h:61`, bit 21): `mag *= fMagicDualCastingEffectivenessBase + fMagicDualCastingEffectivenessMult * CalculateCost(spell, caster)` (AE 26518). Afterwards it compares `mag` with the target's level (`GetLevel`, 37334) and marks the effect "resisted by level" (state 2) when level > mag | PROVEN (AE read in full; SE EP site at the same offsets) |
| Magnitude + duration adjuster | 34053 | 33278 | none | EP 0x1E kModSpellDuration then EP 0x1D, both `(caster, spell, targetActor, &v)`; then EP 0x2A, 0x29 | DISASM-ONLY |
| Target from MagicTarget | 34529 | n/a | none | `MagicTarget -> Actor` (AE: `MagicTarget` at Actor+0xA0) | PROVEN (AE) |
| Raw magnitude | n/a | n/a | `Effect::GetMagnitude` `include/RE/E/Effect.h:31` / `src/RE/E/Effect.cpp:22` (0 if kNoMagnitude); `GetDuration` `:33` | record value | PROVEN (fork source) |
| Cost per caster | 11321 | 11213 | `MagicItem::CalculateMagickaCost` `include/RE/M/MagicItem.h:111` -> `CalculateCost`, `Offsets.h:378` | already used across MFO | field-proven by MFO use |

- GMSTs (AE): fMagicDualCastingEffectivenessBase 376256 (exe default 2.0), fMagicDualCastingEffectivenessMult 376259 (exe default 0.0). Plugins can override both, so read them at runtime.
- **EP 0x1D argument order is PROVEN: `(perkOwner = caster, MagicItem* spell, Actor* target, float* magnitude)`.** Seed it with the effect's own magnitude.
- The `dual` flag in the adjuster struct is set upstream and was not traced. Use MFO's existing `Loadout::CanDualCast` gate (the perk check) to decide whether to apply the dual multiplier. That gate's link to the engine flag is DISASM-ONLY.
- The school power-modifier AVs (kDestructionPowerModifier etc.) do not appear in this adjuster. In vanilla they reach magnitude through the player-only `PerkSkillBoosts` perk entries (EP 0x1D with AV conditions), so the EP call covers whatever the follower actually has. Whether a follower carries those entries belongs to Progression's hidden-perk work, not I8. UNPROVEN for followers.
- Expected damage of a hostile ValueModifier effect: instant = mag. With a duration: mag is per second, so total = mag x dur. The per-second reading of DoT archetypes is UNPROVEN until the probe confirms it.
- Thread: main thread (HandleEntryPoint, CalculateMagickaCost). Both belong in `SpellBook::Measure`.

### 2.3 Target mitigation

**Physical**: AE 44014 / SE 42842 (called from HitData::Populate). Decoded on AE; the SE body shows the same EP 0x25 site and the same vtable call +0x738:
```
r  = target.GetActorValue(kDamageResist 39) * fArmorScalingFactor * 0.01      (AE 38568 = GetActorValue(39))
   + target->GetArmorBaseFactorSum()                                           (vtable 0xE7)
HandleEntryPoint(0x25 kModTargetDamageResistance, aggressor, weapon, target, &r)   (skipped when no weapon)
r  = min(r, fMaxArmorRating * 0.01)
dmg *= (1 - r)
if weaponData.resistance (+0x194) != -1:  q = min(target.GetActorValue(resistance),100)*0.01; if 0<q<=1: dmg *= (1-q)
```
- GMSTs (AE), PROVEN by name: fArmorScalingFactor 374515 (exe 0.15), fMaxArmorRating 374509 (exe 95.0). Requiem changes these, so read them at runtime through `GameSettingCollection::GetSetting` (fork `GameSettingCollection.h:26`; MFO precedent CostParity.cpp:192, Summon.cpp:93).
- Character vtable, resolved from RTTI on both runtimes in this research: **0xE6 = CalcArmorRating** (AE 40248 / SE 39174), **0xE7 = GetArmorBaseFactorSum** (it reads a cached float at AE +0x274 / SE +0x26C, and recomputes it when the cache is negative), **0xE8 = CalcUnarmedDamage**. These match the fork `Actor.h:428-430`. The fork's `RelocateVirtual(0x0E6, 0x0E8)` is (SE/AE index, VR index), so it is correct.
- A cache miss in GetArmorBaseFactorSum recomputes and writes the cache, so call it on the **main thread only**. The DamageResist AV already holds the summed armor rating. GetActorValue is the engine's own read, so CalcArmorRating does not need to be called.
- EP 0x24 kModIncomingDamage (target-owned perks, applied in 44016): the call site exists, but its argument order was not decoded. UNPROVEN, so leave it out at first and log it.

**Magic**: AE 38737 (decoded; SE twin not located):
```
p = target.GetActorValue(spell->GetSpellType()==kPoison(5) ? kPoisonResist 40 : kResistMagic 44)
s = target.GetActorValue(mgef->data.resistVariable)   if != -1 and != p's AV     (fork EffectSetting.h:73)
if target is the PLAYER: p,s = min(fPlayerMaxResistance, .)                       (AE 375301; NPC targets are NOT capped)
mult = (p>=100 || s>=100) ? 0 : (1 - p/100) * (1 - (s/100))                       (AE 26507; algebraically (1-p)(1-s))
```
Some gates at the top of 38737 (vtable calls +0x298/+0x308/+0x2E8 and id 11013, which decide when resistance applies at all) were not decoded. They are DISASM-ONLY. The rest is PROVEN on AE.

### 2.4 Attack cadence

- **Engine AI DPS** (AE 26411 / SE 25848), PROVEN: `perHit x weaponData.speed x fCombatDPSMeleeSpeedMult (exe 1.0) | fCombatDPSBowSpeedMult (exe 0.2)` for bow/crossbow. `weaponData.speed` is fork `TESObjectWEAP.h:168` / `GetSpeed()` `src/RE/T/TESObjectWEAP.cpp:7`. This is the engine's own combat-AI damage-rate model, so adopt it as the DPS for weapons. Which AI code consumes it (callers 26416, 38609, 47334, 18092) was not identified, so "the AI uses it" is DISASM-ONLY.
- Real swing time (animation length, the kWeaponSpeedMult AV): **UNPROVEN**. No proven read exists, so do not model it.
- Spell cadence: the charge time is `EffectSetting::data.spellmakingChargeTime` (fork `EffectSetting.h:84`) or a spell-level charge time. Neither was checked as the engine's actual cast duration, so it is **UNPROVEN**. Compare casts with a weapon on **damage per magicka** and **damage per cast**, plus the weapon's engine DPS. Do not invent a spell DPS until a probe measures cast-to-cast seconds.

### 2.5 Skill fit

- Weapon: the skill AV is `weaponData.skill` (fork `TESObjectWEAP.h:176`), which is exactly the field the core formula reads (PROVEN). Values: kOneHanded 6, kTwoHanded 7, kArchery 8 (crossbows carry their record's skill); unarmed carries none (outside 6..23, so skillMul = 1).
- Spell: `EffectSetting::GetMagickSkill()` / `data.associatedSkill` (fork `EffectSetting.h:139` / `:72`) of the costliest effect, or `MagicItem::GetAssociatedSkill()` (vtable 0x66, `MagicItem.h:99`). Values: kAlteration 18 .. kRestoration 22 (Conjuration 19, Destruction 20, Illusion 21). Staves use their enchantment's effect skill, and scrolls have no caster skill.
- **Use GetActorValue, not GetBaseActorValue.** The engine reads the CURRENT value (fortify skill included), clamped to [0,100] (`GetClampedActorValue` AE 27284 / SE 26616, fork `ActorValueOwner.h:25`). That function is a plain `GetActorValue` vtable call (`ActorValueOwner.h:15`) plus a clamp chosen by the ActorValueInfo flag (one branch caps at a constant, AE 470027, that was not decoded). For skill fit, `min(100, max(0, GetActorValue(av)))` is engine-identical for the skill AVs (flag-bit-4 branch, PROVEN on AE).
- Thread: an AV vfunc read is the same kind of access the IAUS gather already does for self HP, stamina and magicka. Follow whatever thread that gather reads `selfHp` on. This research did not re-verify that thread, so check it at authoring time. The safe default is to read the skill AVs in `Measure` (main).

## 3. Recommended I8 design

**Principle:** compose the estimate from the engine's own pieces, in the order HitData::Populate uses them. Read GMSTs at runtime, and evaluate every perk entry point through the existing verified HandleEntryPoint row, with the REAL target. Keep exactly ONE new id call (the core formula), because the tempering term depends on two unnamed settings and cannot be replicated faithfully. Replicate the parts that are pure AV/GMST arithmetic (mitigation, resist) from runtime reads instead of calling unregistered internals.

### 3.1 New data
- `SpellEntry` (Snapshot.h), per offensive spell/scroll/staff, measured in `SpellBook::Measure` (main):
  - `skillAv`, `skill` (clamped current)
  - `mag0`, `dur0` (raw costliest hostile effect)
  - `magVsTarget`, `durVsTarget` (after EP 0x1D / 0x1E against the Measure-time target, and after dual if `CanDualCast`)
  - `dmgPerCast`, `dmgPerMagicka` (`dmgPerCast / cost`, or the dual cost from `Loadout`)
  - `primaryResistAv` (Magic or Poison), alongside the existing `resistAv` (= resistVariable)
  - `estTargetId` (the FormID the EP chain was evaluated against; 0 = none)
  - `estOk` (Tri: Yes / No / Unproven) plus a reason code
- A new `WeaponEntry` list in `Book` (or extend `Kit`), per carried weapon category the posture candidates use (equipped first):
  - `form`, `skillAv`, `skill`
  - `perHitCore` (core id), `perHitVsTarget` (after EP 0x00 / 0x23 with the target; a power variant through EP 0x1C)
  - `speed`, `dps` (`perHit x speed x fCombatDPS*SpeedMult`)
  - `estTargetId`, `estOk`
- `Facts` (Core.h), filled on the worker gather from the book plus target AVs:
  - `skillFit` (0..1 = skill/100; 1 for scrolls)
  - `estDamage` (per hit or per cast, after mitigation)
  - `estDps` (weapons only)
  - `estPerMagicka` (casts only)
  - `estProven` (bool: false means use skill fit only)
  - `targetMitigation` (physical 1 - r, or magic resist mult)
  - Keep the existing `resist` for logging.
- Cached GMSTs (`Setting*` resolved once at kDataLoaded on main, values re-read each Measure; static after load, so a worker read of the float is a plain read):
  - fDamageWeaponMult, fDamageSkillMin/Max
  - fArmorScalingFactor, fMaxArmorRating
  - fCombatDPSMeleeSpeedMult, fCombatDPSBowSpeedMult
  - fMagicDualCastingEffectivenessBase/Mult, fPlayerMaxResistance
  - A missing GMST sets `estProven=false` for the candidates that need it, and logs it loudly.

### 3.2 Where each value is filled
- **Main (`SpellBook::Measure`, already posted by PostRefresh):**
  - the core id call
  - HandleEntryPoint 0x00 / 0x23 / 0x1C / 0x1D / 0x1E / 0x25 against `Targeting::Current` (or nullptr if none; the EP chain is then the "target-free" variant, recorded as `estTargetId=0`)
  - GetArmorBaseFactorSum on that target
  - CalculateMagickaCost
  - The book also needs a refresh trigger on combat-target change, not only on the inventory/spell dirty seq. Without it, the EP chain goes stale against a new foe. This is the one new trigger.
- **Worker (IAUS gather):**
  - per candidate: the target's DamageResist / ResistMagic / PoisonResist / resistVariable AVs (the same access class as `foeHpPct`)
  - the GMST floats
  - then the mitigation and resist arithmetic of 2.3 applied to the book's numbers
  - If the candidate's target != `estTargetId`, use the target-free EP variant and replicated mitigation **without** EP 0x25, and mark it `estPartial` in the log. Never fabricate the missing perk term.
- **Scoring:**
  - `skill fit` is a new consideration curve on every attack, power-attack and cast candidate (the ruling's step 1).
  - The class weight sits in the class profile (the mage-melee ruling).
  - When `estProven`, a damage consideration compares `estDps` (weapons) against `dmgPerCast` / `estPerMagicka` (casts), normalised by the best offensive candidate this tick, so "firebolt 3x dagger" is visible.
  - When not `estProven`, that consideration is neutral (1.0) and the `[iaus-dmg]` reason is logged, which is the "skill fit only" fallback.
- **Sim:** mirror the same formula with the vanilla GMST defaults above as inputs, so the tuning target (T1 mage attack share in the low single digits) can be met.

### 3.3 Passive probe lines (config-gated `bIausDamageProbe`, default ON for the test list, rate-limited per follower+form like `logRepeatSec`)
1. Once per load: `[iaus-dmg] gmst fDamageWeaponMult=%g fDamageSkillMin=%g fDamageSkillMax=%g fArmorScalingFactor=%g fMaxArmorRating=%g fCombatDPSMeleeSpeedMult=%g fCombatDPSBowSpeedMult=%g fMagicDualCastingEffectivenessBase=%g fMagicDualCastingEffectivenessMult=%g fPlayerMaxResistance=%g`. This is the Requiem check.
2. Per Measure: `[iaus-dmg] weap f=%08X w=%08X(%s) skillAv=%d skill=%.1f core=%.2f ep00=%.2f ep23=%.2f ep1c=%.2f speed=%.2f dps=%.2f tgt=%08X ok=%s`
3. Per Measure: `[iaus-dmg] spell f=%08X s=%08X(%s) skillAv=%d skill=%.1f mag0=%.1f dur0=%u ep1d=%.2f ep1e=%.2f dual=%d mult=%.2f cost=%.1f perCast=%.1f perMp=%.3f tgt=%08X ok=%s`
4. Per gather (on change): `[iaus-dmg] mit f=%08X tgt=%08X dr=%.1f bfs=%.3f ep25=%.3f r=%.3f physMul=%.3f rmP=%.1f rmS=%.1f magMul=%.3f partial=%d`
5. **Observation against reality:** sink `TESHitEvent` (cause = follower, source = weapon/spell form). Sample the target's Health AV on the main tick before and after: `[iaus-dmg] hit f=%08X tgt=%08X src=%08X power=%d hpBefore=%.1f hpAfter=%.1f delta=%.1f est=%.1f ratio=%.2f others=%d`, where `others` = other hostile attackers on that target in the window (the noise flag). A ratio histogram near 1.0 over a field session is what turns rows 2.1 and 2.3 from DISASM-ONLY into observed. Whether TESHitEvent fires before or after damage is applied is UNPROVEN, which is why it samples on the next tick.
6. Spells: after a follower's offensive cast lands, read the target's active effect for that spell (`ActiveEffect::GetMagnitude`, fork `ActiveEffect.h:94`; an id call, see 5): `[iaus-dmg] ae f=%08X tgt=%08X s=%08X mgef=%08X magApplied=%.2f est=%.2f`. If that id call is not wanted, use the health-delta form of line 5.

## 4. Tier-A pieces (new id calls or seats that need VerifiedAddresses rows + `SeatVerified()`)

1. **Core weapon damage formula**: AE 26410 / SE 25847 / 1.7.104 via the MFO id map (keyed by 26410). No fork symbol, so it must be declared in MFO as `float(*)(RE::ActorValueOwner*, RE::TESObjectWEAP*, RE::TESAmmo*, float itemHealth, float attackMult, bool useEquippedAmmoIfPlayer)`. The signature was checked on AE+SE: rcx/rdx/r8, xmm3, then the float at [rsp+0x20] and the bool at [rsp+0x28] after the shadow space. Pass `actor->AsActorValueOwner()` (AE +0xB8 / SE +0xB0, which matches the display caller) and `useEquippedAmmoIfPlayer=false`. Pass the ammo explicitly for bows. Without this row, every weapon candidate falls back to skill fit only.
2. **Item health for tempering**: AE 15990 / SE 15752 is the InventoryEntryData health getter used by both GetDamage and Populate. It has no fork symbol and was not decoded. Preferred: read the equipped entry's `ExtraHealth` directly (no row), but which extra it reads is UNPROVEN. Until then pass 1.0 (untempered) and log `temper=unproven`.
3. **`GetArmorBaseFactorSum` vtable 0xE7**: not an id, but ABI-shaped. Its slot was verified on 1.6.1170 and 1.5.97 in this research. **1.7.104 is unchecked.** Main thread only (lazy cache write).
4. Optional: `ActorValueOwner::GetClampedActorValue` (fork, AE 27284 / SE 26616) if the clamp is called rather than replicated. Replication is engine-identical for skills, so no row is recommended.
5. Optional: `ActiveEffect::GetMagnitude` (fork `ActiveEffect.h:94`, an id call) for probe line 6.
6. Optional, only if the hit-delta probe is too noisy: a passive observation seat on `HitData::Populate` (AE 44001 / SE 42832, fork `HitData.h:47`) to log `totalDamage` after the call. This is a new engine seat, so it gets a full tier-A brief.

**Not tier A (existing rows / plain reads):**
- HandleEntryPoint (rows exist on all 3 runtimes). New EP indices 0x00 / 0x1C / 0x1D / 0x1E / 0x23 / 0x25 go through the same verified function, and each argument order above was decoded from an engine call site.
- GMST reads, AV vtable reads, record fields, CalculateMagickaCost (pre-existing MFO use).

## 5. Open (UNPROVEN) list
- `finalMul` branch in the core (flag 0x02, global 410199).
- Whether kAttackDamageMult applies on the hit path.
- EP 0x24 argument order.
- Gates at the top of the magic-resist function 38737, and its SE twin.
- The upstream `dual` flag source.
- DoT per-second semantics.
- Spell cast cadence.
- Real swing time.
- Who consumes the engine AI DPS (26411).
- The item-health getter.
- Every 1.7.104 address.
