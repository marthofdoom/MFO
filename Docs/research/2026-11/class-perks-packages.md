# MFO class add-on: passives, signatures and combat packages (design draft, 2026-11-08)

Status: DESIGN DRAFT for marth. Nothing here is built. Every number is a starting value to tune in a field run.
Roster and rules: memory `class-update-fire-emblem-spec`, ClickUp 86e3dhbd3, FE study R3 / R4 / R7, tree page.

## 0. Ground rules and the facts this draft stands on

### 0.1 Feasibility marks
- **NPC-SAFE**: the mechanism is known to run on NPCs. Either an actor value change through a constant-effect ABILITY
  spell (the same shape as every racial ability), or a perk entry point that MFO's own verdict table grades **E
  (effective on NPCs)**: `MFO_GenerateESP.py:1704` `PROG_ENTRYPOINT_VERDICTS`, with the live proof that Requiem's SPID
  follower perks fire on followers in LoreRim (`Docs/FOLLOWER-PROGRESSION-ESL-DESIGN.md:83`).
- **LIKELY**: the parts are NPC-safe but the effect depends on a situation or condition that has not been observed on an
  NPC (for example a sneak attack opening mid-fight, a cloak spell filtered to allies, a condition on a bound weapon).
  Principle 5 applies: observe it running before building on it.
- **PLAYER-ONLY RISK**: the entry point is graded **D** (dead on NPCs) or **M** (marginal or unproven on NPCs) in the
  verdict table, or the engine only runs that system for the player. These need MFO's procedural handling or are cut.

Verdict table, the parts this draft uses. E: CalculateWeaponDamage, ModAttackDamage, ModPowerAttackDamage,
ModPowerAttackStamina, ModBashingDamage, ModIncomingDamage, ModIncomingStagger, ModTargetStagger, ModArmorRating,
ModTargetDamageResistance, ModPercentBlocked, ModShieldDeflectArrowChance, ModSpellMagnitude, ModSpellDuration,
ModSpellCost, ModIncomingSpellMagnitude, ModWardMagickaAbsorptionPct, ModSneakAttackMult, ModDetectionLight,
ModDetectionMovement, ModDetectionSneakSkill, ApplyCombatHitSpell, ApplyBashingSpell, ApplyWeaponSwingSpell,
ApplySneakingSpell, ApplyReanimateSpell, SetSweepAttack, CalculateMyCriticalHitChance, ModEnemyCriticalHitChance.
M: ModCommandedActorLimit, CanDualCastSpell, ModRecoverArrowChance, ModArmorWeight, ModSpellRange_TargetLoc,
GetMaxCarryWeight, SetBooleanGraphVariable. D: ModPickpocketChance, CanPickpocketEquippedItem, ModShoutOK,
ModPoisonDoseCount, ModPlayerMagicSlowdown, every lockpick, alchemy and enchanting entry point.
Note: ModCommandedActorLimit is M for the engine, but MFO's own summon road reads it itself through
`BGSEntryPoint::HandleEntryPoint` (`native/cast/Summon.cpp:106-113`), so it is effective for MFO-driven summons.

### 0.2 How a passive is carried (one shape for every line)
- Each trait line is ONE perk in the add-on ESL with two sets of entries: the permanent rank, and the equipped rank.
  The equipped entries are conditioned on the actor's rank in a per-line faction (`MFOC_LineEquipped_<line>`, rank 1 =
  this line is lifted by the equipped class). The permanent entries carry the opposite condition, so the two never
  stack: the equipped value REPLACES the permanent value (FE R3, non-stacking).
- Actor value parts (Health, Stamina, regen, SpeedMult) are ABILITY spells added per reference with
  `Actor::AddSpell`, again one permanent and one equipped ability, swapped on class change.
- Why factions and abilities and not perk add/remove on each switch: `TESNPC::AddPerk` writes the BASE record
  (`native/ProgProbe.cpp:408-409`, `AddPerk` then `ApplyPerksFromBase`). A switch must not churn base records, and a
  faction rank or an ability is per reference. Perks are granted once at mastery, and re-applied at load by the class
  add-on's own versioned record (it may not live in PRGN, marth 2026-09-23 architecture ruling).
- A line counts ONCE however many mastered classes own it. Higher tiers never add a second copy (FE R7).
- **Which lines the equipped class lifts:** a tier-2 class lifts its own lines. A pure tier 3 lifts its parent's lines.
  A hybrid lifts its school line plus the weapon line the follower is wielding right now (entry conditions on the
  equipped weapon type, so a Spellsword with a two-hander lifts Two-Handed, with a sword and board lifts One-Handed and
  Shield). Armor lines are never lifted by a hybrid (armor is free, FE R6), they stay at the permanent rank. A tier 4
  lifts the lines of both tier-3 parents. Dragonborn lifts every line the follower has mastered.

### 0.3 How a signature is carried
One ability spell or one perk per tier-3 / tier-4 / tier-5 class, added only while that class is equipped
(an `MFOC_ClassEquipped` faction rank gates its perk entries, abilities are added and removed with the class).
Signatures never stack across classes because only one class is equipped.

### 0.4 What a combat package is made of (verified field names)
Fields from `_commonlib/mit-3.7-fork/include/RE/T/TESCombatStyle.h` (the authoritative fork):
- CSGD `CombatStyleGeneralData`: offensiveMult, defensiveMult, groupOffensiveMult, meleeScoreMult, magicScoreMult,
  rangedScoreMult, shoutScoreMult, unarmedScoreMult, staffScoreMult, avoidThreatChance.
- CSME `CombatStyleMeleeData`: attackIncapacitatedMult, powerAttackIncapacitatedMult, powerAttackBlockingMult,
  bashMult, bashRecoilMult, bashAttackMult, bashPowerAttackMult, specialAttackMult. On disk CSME holds only 7 floats,
  so specialAttackMult is zero-filled by the loader and cannot be set from the ESL (`MFO_GenerateESP.py:1201` docstring).
- CSCR `CombatStyleCloseRangeData`: circleMult, fallbackMult, flankDistanceMult, stalkTimeMult.
- CSLR `CombatStyleLongRangeData`: strafeMult.
- CSFL flight data (dragons only, left at MFO's current values).
- DATA flags: kDuelingStyle (1), kFlankingStyle (2), kAllowDualWielding (4).

**There is NO dodge, block, or power-attack-chance field in CSTY.** The caller's list named those, so here is where each
really lives:
- Dodge: vanilla NPCs only step out of threats (avoidThreatChance) and strafe (strafeMult). A real dodge ROLL is an
  animation from a dodge framework the list may or may not have. No Harbinger facet drives it today. GAP G-H3.
- Block: no CSTY multiplier. Vanilla block frequency follows defensiveMult (LIKELY, not measured). Blocking is a
  defence leaf that ch.7 can never deny (FACETS ch.7: defence leaves have no category bit).
- Power attack share: Harbinger's design pass (ClickUp 86e3bw775, section D4) read the engine. The roll weight is the
  race ATKD attackChance times a CSTY multiplier that applies ONLY while the target is staggered
  (powerAttackIncapacitatedMult) or, for a power attack, while the target blocks (powerAttackBlockingMult). In the neutral
  case the power split is the race's ATKD data alone. So CSTY can raise power attacks only on staggered or blocking foes.
  A class-wide power share needs Harbinger ch.18 attack selection (designed, not built). GAP G-H1, G-H2.

**Score mults are WEAPON CHOICE, not attack share.** MFO's stance styles use 10.0 for the stance's category and 0.1 for the
rest (`MFO_GenerateESP.py:1267-1294`) so the AI does not re-draw a different weapon. Every class CSTY keeps that stance
dominance. So a class ships ONE CSTY PER STANCE IT FIGHTS IN (melee, ranged, cast), plus the dual-wield twin of each
melee variant (the perk-vote dual-wield gate stays, `CombatStyle::Want a_allowDual`). `CombatStyle::ApplyTick` then
writes the equipped class's variant of the stance instead of `MFO_MeleeStyle` / `MFO_RangedStyle` / `MFO_CastStyle`.
With the class tree OFF, MFO's current styles are used exactly as today. Hybrids switch stance through the equip
gambits, and the stance switch already is a CSTY handoff (branch B, MAP.md CombatStyle.cpp).
Below, only the fields that differ from the stance's base are given:
- melee base: CSCR circle 0.73, fallback 0.42, flank 0.2, stalk 0.2 (csHumanMeleeLvl1). CSME all 1.0. DATA dueling.
- ranged base: CSCR circle 0.45, fallback 0.65, flank 0.2, stalk 0.2 (csHumanMissile). avoidThreat 0.5.
- cast base: CSCR circle 0.30, fallback 0.50, flank 0.2, stalk 0.2 (csHumanMagic). avoidThreat 0.2.
- all: CSGD offensive 1.0, defensive 0.5, groupOffensive 1.0, CSLR strafe 0.2.

Field meanings (vanilla Creation Kit sense, LIKELY until probed): offensive = how eagerly it attacks, defensive = how much
it blocks and backs off when threatened, groupOffensive = how much more it presses when allies engage, avoidThreat =
chance to step out of incoming spells, areas and arrows, circle = moving around the target, fallback = backing off,
flankDistance = how wide it swings round to flank, stalkTime = how long it stalks before closing, strafe = side
movement at range, the bash fields = chance to bash in those situations, attackIncapacitated = preference to hit a
staggered or downed foe.

### 0.5 What a class package needs from Harbinger beyond CSTY
- ch.18 ATTACK SELECTION (designed in ClickUp 86e3bw775, not on main): class bits Normal, Power, Bash, BashPower, Left,
  DualWield. Deny-only, `powerShareMax` cap over a sliding window, `powerShareMin` observe-only in v1. This is how a
  Mercenary attacks with fewer power attacks than a Warrior without removing any move (attacks additive rule).
- A combat-style facet / slider (marth 2026-10-27: strict adherence with the tree ON, overriding a follower's authored
  style "would need a combat style slider in harbinger"). Today MFO writes `CombatController::combatStyle` (0x38) itself.
- ch.3 Stance (sneak), ch.16 Detection (silent movement), ch.20 Target pin, ch.23 Pursuit leash, ch.24 Combat approach,
  ch.25 Cast guard and ch.7 Combat action already exist and are used as named below.

### 0.6 Gambits
Gambits are BASE MFO. A class applies a default, editable set and never owns one. Names below are the real opcodes in
`native/Vocabulary.h` (origin/main). Format: `condition -> action`. Anything not in base is marked **(MISSING)** and listed
in section 4.

---

## 1. The trait passive lines (13) plus the tier-1 foundation (3)

All values are TOTALS. The equipped value replaces the permanent value, never adds to it. Permanent is about a third of
equipped (FE R3).

| # | Line | Owner class(es) | Permanent (mastered) | Equipped (class lifts the line) | Carrier | Mark |
|---|---|---|---|---|---|---|
| L1 | Shield (Bulwark) | Knight | blocking stops 5% more damage | blocking stops 15% more, shield bash +25% damage | ModPercentBlocked, ModBashingDamage | NPC-SAFE |
| L2 | Heavy Armor | Knight, Warrior | +8% armor rating from heavy pieces | +25% armor rating, staggered 15% less | ModArmorRating (heavy keyword), ModIncomingStagger | NPC-SAFE |
| L3 | Two-Handed | Warrior | +5% two-handed damage | +15% damage, power attacks cost 20% less stamina | ModAttackDamage (weapon type), ModPowerAttackStamina | NPC-SAFE |
| L4 | One-Handed | Knight, Mercenary | +5% one-handed damage | +15% damage, +10% more while dual wielding | ModAttackDamage (weapon type, both hands condition) | NPC-SAFE |
| L5 | Light Armor | Mercenary, Archer, Thief | +8% armor rating from light pieces | +25% armor rating | ModArmorRating (light keyword) | NPC-SAFE |
| L6 | Evasion | Mercenary | +15 Stamina | +40 Stamina, +25% stamina regen, +5 SpeedMult | ability: Stamina, StaminaRateMult, SpeedMult | NPC-SAFE (SpeedMult LIKELY, the engine applies a SpeedMult change only after a movement refresh) |
| L7 | Marksman | Archer | +5% bow and crossbow damage | +15% damage, arrows stagger 20% more | ModAttackDamage (ranged condition), ModTargetStagger | NPC-SAFE |
| L8 | Stealth | Thief | sneaking movement 10% quieter | 30% quieter, sneak attacks x1.5 more damage | ModDetectionMovement, ModSneakAttackMult | quiet: NPC-SAFE. Sneak attack: LIKELY (an NPC rarely gets the opening mid-fight, tree page "stealth identity") |
| L9 | Destruction | Evoker, Warlock | Destruction +5% magnitude | +15% magnitude, cost 10% less | ModSpellMagnitude, ModSpellCost (school condition) | NPC-SAFE |
| L10 | Conjuration | Conjurer | Conjuration +10% duration | +30% duration, cost 15% less | ModSpellDuration, ModSpellCost | NPC-SAFE |
| L11 | Restoration | Healer | Restoration +5% magnitude | +15% magnitude, cost 10% less | ModSpellMagnitude, ModSpellCost | NPC-SAFE |
| L12 | Illusion / Alteration | Warlock | Illusion +5% magnitude, Alteration +10% duration | Illusion +15% magnitude (higher level cap for fear and calm), Alteration +30% duration, both cost 10% less | ModSpellMagnitude, ModSpellDuration, ModSpellCost | NPC-SAFE |
| L13 | Arcane Robes | Evoker, Conjurer, Healer, Warlock | +15 Magicka, +10% magicka regen when wearing no heavy or light armor | +40 Magicka, +30% regen | ability with a worn-armor condition | NPC-SAFE |
| F1 | Foundation: Melee | Melee (tier 1) | +10 Health | +25 Health | ability | NPC-SAFE |
| F2 | Foundation: Ranged | Ranged (tier 1) | +10 Stamina | +25 Stamina | ability | NPC-SAFE |
| F3 | Foundation: Mage | Mage (tier 1) | +10 Magicka | +25 Magicka | ability | NPC-SAFE |

Notes:
- The foundation lines are not traits. Tier 1 owns no trait (FE R7), so a mastered tier-1 class gives a small pool bonus.
  The equipped tier-1 class lifts its foundation line. Higher tiers lift the foundation of their tier-1 root as well
  (a Knight equipped lifts F1), so a follower always feels its root.
- Worst case, a follower that has mastered everything and equips Dragonborn: every line at equipped rank, about +25%
  armor, +15% weapon and spell numbers, +40 to each pool. That stays sane, which is the point of R3.
- Dual wield sits inside L4 (FE R7). The dagger stays a Thief exception and does not count as a melee weapon for the
  hybrid gate (tree page), but L4's damage entry still applies to it.

---

## 2. Per-class blocks

Abbreviations: off / def / grp = CSGD offensive / defensive / groupOffensive, aThr = avoidThreatChance, atkInc /
paInc / paBlk = attackIncapacitated / powerAttackIncapacitated / powerAttackBlocking, bash / bRec / bAtk / bPA = the four
bash multipliers, circ / fall / flank / stalk = CSCR, strafe = CSLR. "base" means the stance base in section 0.4.

### TIER 1

#### Melee
- Passive: F1 Foundation Melee (+10 / +25 Health). No signature (tier 1).
- Package: the trainee brawler. Closes at once, fights head on, average everything. Same as MFO_MeleeStyle today.
- CSTY melee: base values (MFO_MeleeStyle / MFO_MeleeSingleStyle as shipped).
- Harbinger: none beyond today.
- Default gambits: `cond.self_hp_pct_below 30 -> act.drink_health_potion`, `cond.foe_attacking_player -> act.attack`,
  `cond.foe_any -> act.equip_melee`.
- Feasibility: all NPC-SAFE.

#### Ranged
- Passive: F2 Foundation Ranged (+10 / +25 Stamina).
- Package: stands back and shoots, swaps to melee when a foe is on top of it. MFO_RangedStyle today.
- CSTY ranged: base. Melee fallback uses the melee base.
- Default gambits: `cond.self_out_of_arrows -> act.equip_melee`, `cond.foe_within_range 300 -> act.equip_melee`,
  `cond.foe_any -> act.equip_ranged`, `cond.foe_attacking_player -> act.attack`.
- Feasibility: NPC-SAFE.

#### Mage
- Passive: F3 Foundation Mage (+10 / +25 Magicka).
- Package: keeps distance and casts, heals itself when hurt. MFO_CastStyle values.
- CSTY cast: base.
- Default gambits: `cond.self_hp_pct_below 40 -> act.cast_self (a heal)`, `cond.self_mp_pct_below 20 -> act.drink_magicka_potion`,
  `cond.foe_any -> act.cast_target (best attack spell, Enemy)`.
- Feasibility: NPC-SAFE.

### TIER 2

#### Knight (tank, shield)
- Passive: owns L1 Shield, L2 Heavy Armor, L4 One-Handed. Equipped lifts L1, L2, L4, F1.
- Package: the wall. Plants itself between the party and the strongest foe, blocks a lot, bashes attackers out of their
  swings, rarely chases, never flanks. Holds the line rather than pressing. Target priority: whoever attacks an ally,
  then whoever attacks the player.
- CSTY melee: off 0.8, def 1.5, grp 0.6, aThr 0.1. bash 2.0, bRec 2.0, bAtk 1.5, bPA 1.5, paBlk 1.0. circ 0.3, fall 0.2,
  flank 0.1, stalk 0.1. DATA dueling only (no flanking flag).
- Harbinger: ch.18 declare Bash and BashPower governed, power share max 0.25 (a tank swings light and fast).
  ch.23 pursuit leash at a short radius around the player, so the Knight does not chase off the line.
- Default gambits: `cond.foe_attacking_ally -> act.taunt`, `cond.foe_attacking_player -> act.attack`,
  `cond.self_hp_pct_below 35 -> act.drink_health_potion`, `cond.foe_any -> act.equip_melee`.
- Feasibility: passives NPC-SAFE. "Blocks a lot" rests on defensiveMult (LIKELY). No act.block exists, and no seat can
  add a block (defence leaves are never denied or driven). Fine for a tank in practice.

#### Warrior (heavy, power attacks, head on)
- Passive: owns L2 Heavy Armor, L3 Two-Handed. Equipped lifts L2, L3, F1.
- Package: the bruiser. Walks straight in, trades blows, power attacks anything that blocks or staggers, presses harder
  when allies engage. Little circling, no flanking, never backs off. Target: the strongest foe in reach.
- CSTY melee: off 1.6, def 0.3, grp 1.4, aThr 0.0. atkInc 1.5, paInc 3.0, paBlk 3.0, bash 0.5. circ 0.2, fall 0.1,
  flank 0.1, stalk 0.0. DATA dueling.
- Harbinger: ch.18 power share is the identity. Under the v1 ABI only a ceiling exists, so the Warrior declares no cap
  (the engine's own split) and every OTHER melee class declares a lower cap, so the Warrior reads as the power attacker.
  A real power FLOOR needs a "Normal share max" (deny Normal while the window is over it). GAP G-H2.
- Default gambits: `cond.foe_blocking -> act.power_attack`, `cond.foe_stronger_than_me -> act.power_attack`,
  `cond.foe_highest_hp -> act.attack`, `cond.self_sp_pct_below 25 -> act.drink_stamina_potion`, `cond.foe_any -> act.equip_melee`.
- Feasibility: NPC-SAFE. `act.power_attack` today is MFO's own graph drive (`cast/Fire.cpp:478`), retired into ch.18 by the
  planned port (ClickUp 86e3bw775 ruling 1).

#### Mercenary (light, agile, dodge and flank)
- Passive: owns L4 One-Handed, L5 Light Armor, L6 Evasion. Equipped lifts L4, L5, L6, F1.
- Package: the skirmisher. Circles wide, flanks to the target's side or back, steps out of incoming spells and arrows,
  backs off when two foes press, then darts in again. Quick normal attacks, few power attacks, bashes rarely. Target:
  casters and archers first (they cannot block a flanker), then the weakest foe.
- CSTY melee: off 1.2, def 0.4, grp 1.0, aThr 0.8. atkInc 1.2, paInc 1.0, paBlk 0.5, bash 0.3. circ 1.5, fall 0.8,
  flank 2.5, stalk 0.6, strafe 0.6. DATA dueling + flanking (1|2), dual twin 1|2|4.
- Harbinger: ch.18 power share max 0.15. A real dodge roll needs G-H3 (no facet, and a list-agnostic dodge means
  "use the list's dodge framework when present, else the vanilla side step"). Until then "dodge" = aThr + strafe + circle.
- Default gambits: `cond.foe_is_caster -> act.attack`, `cond.foe_is_ranged -> act.attack`,
  `cond.foe_lowest_hp -> act.attack`, `cond.self_sp_pct_below 25 -> act.drink_stamina_potion`,
  `cond.foe_any -> act.equip_melee`.
- Feasibility: passives NPC-SAFE (SpeedMult LIKELY). Dodge is the biggest risk in tier 2 (G-H3).

#### Archer
- Passive: owns L5 Light Armor, L7 Marksman. Equipped lifts L5, L7, F2.
- Package: the line archer. Holds long range, strafes to keep a clear shot, backs away from a closing melee foe instead
  of swapping to a blade, only draws melee when cornered. Target: casters and other archers first.
- CSTY ranged: off 1.0, def 0.6, grp 0.8, aThr 0.6. circ 0.4, fall 1.2, flank 0.3, stalk 0.4, strafe 0.8.
  Melee variant: melee base with fall 0.8.
- Harbinger: ch.25 cast guard is not needed (no spells), but the bystander hold already covers weapon attacks at ABI 32
  (MAP.md Combat vfunc hooks row), so the Archer does not shoot through the party.
- Default gambits: `cond.foe_is_caster -> act.attack`, `cond.foe_is_ranged -> act.attack`,
  `cond.self_out_of_arrows -> act.loot_arrows`, `cond.foe_within_range 150 -> act.equip_melee`, `cond.foe_any -> act.equip_ranged`.
- Feasibility: NPC-SAFE. ModRecoverArrowChance is M, so no arrow-recovery passive (MFO loots arrows instead).

#### Thief (sneak, Steal)
- Passive: owns L5 Light Armor, L8 Stealth. Equipped lifts L5, L8, F1.
- Package: the opportunist. Sneaks between fights, opens from stealth, fights with a dagger or short blade from the
  flank, falls back to re-hide when a fight turns. Target: the foe that is not looking at it, else the weakest.
- CSTY melee: off 0.9, def 0.3, grp 1.2, aThr 0.6. atkInc 2.0, paInc 1.5, paBlk 0.3, bash 0.2. circ 1.2, fall 1.0,
  flank 2.0, stalk 1.5, strafe 0.5. DATA dueling + flanking.
- Harbinger: ch.3 Stance (sneak) and ch.16 Detection (silent movement) out of combat. ch.18 power share max 0.2.
- Default gambits: `cond.player_sneaking -> act.sneak` **(MISSING act.sneak, ch.3 exists)**, `cond.foe_lowest_hp -> act.attack`,
  `cond.foe_fleeing -> act.attack`, `cond.dark + cond.always -> act.steal` **(MISSING, Steal is designed, not built)**,
  `cond.foe_any -> act.equip_melee`.
- Feasibility: Steal cannot use the engine's pickpocket path (ModPickpocketChance and CanPickpocketEquippedItem are D),
  it is a loose-item take while hidden (rogue design memo) and must pre-check every crime (marth 2026-11-02 ruling).
  Sneak attack is LIKELY. PLAYER-ONLY RISK: pickpocket entry points (cut, not used).

#### Evoker (Destruction)
- Passive: owns L9 Destruction, L13 Arcane Robes. Equipped lifts L9, L13, F3.
- Package: the artillery. Mid range, casts constantly, picks the element the foe is weak to, steps out of incoming
  spells, falls back from melee to keep casting. Target: groups (area spells) and foes weak to its element.
- CSTY cast: off 1.4, def 0.3, grp 1.0, aThr 0.5. circ 0.3, fall 0.9, flank 0.2, stalk 0.1, strafe 0.5.
- Harbinger: ch.8b cast claims and ch.25 cast guard (friendly fire hold) as MFO uses them today.
- Default gambits: `cond.foe_weak_fire -> act.cast_target (fire)`, `cond.foe_weak_frost -> act.cast_target (frost)`,
  `cond.foe_weak_shock -> act.cast_target (shock)`, `cond.foe_multiple_within 3 -> act.cast_target (area spell)`,
  `cond.self_mp_pct_below 20 -> act.drink_magicka_potion`, `cond.foe_any -> act.cast_target (Enemy)`.
- Feasibility: NPC-SAFE.

#### Conjurer
- Passive: owns L10 Conjuration, L13 Arcane Robes. Equipped lifts L10, L13, F3.
- Package: the summoner. Opens every fight with a summon, stays behind it at long range, re-summons when it dies,
  casts a bound bow or minor Destruction in between. Retreats from melee, its summon holds the line. Target: whatever
  attacks it (the summon tanks the rest).
- CSTY cast: off 0.6, def 0.8, grp 0.6, aThr 0.6. circ 0.2, fall 1.4, flank 0.1, stalk 0.3, strafe 0.4.
- Harbinger: ch.8b cast for the summon, the summon ground target is a TODO (memory "summons target ground XYZ").
- Default gambits: `cond.foe_any -> act.cast_self (summon)` (re-cast held off by MFO's summon limit read, Summon.cpp),
  `cond.foe_attacking_me -> act.cast_target (Enemy)`, `cond.self_mp_pct_below 25 -> act.drink_magicka_potion`.
  A "no summon active" condition would make the order clearer: **(MISSING cond.self_has_summon)**.
- Feasibility: NPC-SAFE. L10 duration NPC-SAFE.

#### Healer
- Passive: owns L11 Restoration, L13 Arcane Robes. Equipped lifts L11, L13, F3.
- Package: the medic. Stays close behind the party, never leads, heals the most hurt ally, wards against casters,
  turns undead. Fights only when nobody needs healing. Leashed near the player.
- CSTY cast: off 0.3, def 1.0, grp 0.4, aThr 0.7. circ 0.2, fall 1.2, flank 0.0, stalk 0.5, strafe 0.4.
- Harbinger: ch.23 pursuit leash near the player. ch.25 heal stop percent (MakeStopPct 80) for channelled heals.
- Default gambits: `cond.ally_downed -> act.cast_target (heal, ally)`, `cond.player_hp_pct_below 50 -> act.cast_player (heal)`,
  `cond.ally_hp_pct_below 50 -> act.cast_target (heal, ally)`, `cond.self_hp_pct_below 50 -> act.cast_self (heal)`,
  `cond.foe_is_undead -> act.cast_target (turn undead)`, `cond.foe_is_caster -> act.cast_self (ward)`.
- Feasibility: NPC-SAFE (self-concentration solved via CastSelfDirect, memory).

#### Warlock (Illusion + Alteration + Destruction)
- Passive: owns L12 Illusion / Alteration, L9 Destruction, L13 Arcane Robes. Equipped lifts L9, L12, L13, F3.
- Package: the controller. Fears or calms the second foe, paralyzes the strongest, keeps an armor spell up, finishes
  with Destruction. Mid range, keeps moving. Target: the foe stronger than it (control first), then the weakest.
- CSTY cast: off 1.0, def 0.5, grp 0.8, aThr 0.6. circ 0.6, fall 0.7, flank 0.3, stalk 0.3, strafe 0.6.
- Default gambits: `cond.foe_stronger_than_me -> act.cast_target (paralyze or fear)`, `cond.foe_count_at_least 3 -> act.cast_target (calm, Enemy)`,
  `cond.always -> act.cast_self (armor spell)` (the already-active gate in `cast/Auto.cpp:250` stops re-casting),
  `cond.foe_any -> act.cast_target (Destruction, Enemy)`.
- Feasibility: NPC-SAFE. Illusion magnitude against level works on NPC casters. Fear and calm cast by an NPC rely on
  the "Script / enemy-control archetype" nature handling (commit d8b0aa3), so the gambit must pick Enemy, not Auto.

### TIER 3 PURE

#### Great Knight (from Knight)
- Passive: lifts L1, L2, L4, F1 (Knight's lines).
- Signature, "Shieldwall": while equipped, staggered 50% less (ModIncomingStagger x0.5), blocking deflects arrows 40%
  more often (ModShieldDeflectArrowChance), and every shield bash applies "Rebuke", a 15-point stagger push
  (ApplyBashingSpell). NPC-SAFE.
- Package: Knight, more extreme. Never gives ground, bashes every recoil, taunts on its own.
- CSTY melee: off 0.7, def 2.0, grp 0.5, aThr 0.0. bash 3.0, bRec 3.0, bAtk 2.0, bPA 2.0, paBlk 0.8. circ 0.2, fall 0.1,
  flank 0.0, stalk 0.0. DATA dueling.
- Harbinger: ch.18 power share max 0.2, Bash governed (never denied). ch.23 short leash.
- Default gambits: Knight's set plus `cond.foe_count_at_least 3 -> act.taunt`.
- Feasibility: NPC-SAFE.

#### Champion (from Warrior)
- Passive: lifts L2, L3, F1.
- Signature, "Crushing Blows": power attacks deal +30% damage (ModPowerAttackDamage), two-handed power attacks sweep and
  hit everything in the arc (SetSweepAttack), power hits stagger 50% more (ModTargetStagger). NPC-SAFE.
- Package: Warrior, more extreme. Charges the biggest foe, power attacks every opening, hits downed foes.
- CSTY melee: off 2.0, def 0.2, grp 1.6, aThr 0.0. atkInc 2.5, paInc 4.0, paBlk 4.0, bash 0.3. circ 0.1, fall 0.0,
  flank 0.0, stalk 0.0. DATA dueling.
- Harbinger: no power cap. Needs the G-H2 power floor to be fully itself.
- Default gambits: Warrior's set plus `cond.foe_multiple_within 2 -> act.power_attack` (sweep). A "foe staggered"
  condition would let it chain power attacks on stagger **(MISSING cond.foe_staggered)**.
- Feasibility: NPC-SAFE.

#### Blademaster (from Mercenary, dual wield)
- Passive: lifts L4, L5, L6, F1.
- Signature, "Flurry": while dual wielding, attacks deal +20% damage and dual power attacks cost 40% less stamina
  (ModAttackDamage and ModPowerAttackStamina with a both-hands condition), every hit bleeds 3 damage a second for 4 s
  (ApplyCombatHitSpell). NPC-SAFE.
- Package: Mercenary, more extreme. Constant movement, widest flanks, dual-wield chains, almost no power attacks except
  dual power on a staggered foe.
- CSTY melee (dual twin is the main one): off 1.5, def 0.2, grp 1.0, aThr 1.0. atkInc 1.5, paInc 1.5, paBlk 0.3, bash 0.0.
  circ 2.0, fall 0.9, flank 3.0, stalk 0.4, strafe 0.8. DATA 1|2|4.
- Harbinger: ch.18 DualWield and Left governed, power share max 0.15. The design pass warns the tree may never build the
  left-hand context for a dual wielder (open question 2 on 86e3bw775), so "more left-hand attacks" may be unreachable by
  deny. Dodge is G-H3.
- Default gambits: Mercenary's set, with `cond.foe_is_caster -> act.attack` first.
- Feasibility: perk effects NPC-SAFE. The dual-wield attack MIX is the risk (G-H1, G-H4).

#### Ranger (from Archer)
- Passive: lifts L5, L7, F2.
- Signature, "Hunter's Mark": a bow or crossbow hit marks the target for 8 s, the mark lowers its armor by 100
  (ApplyCombatHitSpell, a target-side DamageResist modifier), and the Ranger's own shots ignore 25% of armor
  (ModTargetDamageResistance). NPC-SAFE.
- Package: the hunter. Longest range in the roster, strafes and repositions after each shot, kites melee foes backwards
  instead of drawing a blade, marks the strongest foe so the party focuses it.
- CSTY ranged: off 1.1, def 0.6, grp 0.8, aThr 0.8. circ 0.6, fall 1.6, flank 0.5, stalk 0.6, strafe 1.2.
- Harbinger: ch.20 target pin on the marked foe while the mark lasts.
- Default gambits: `cond.foe_highest_hp -> act.attack`, `cond.foe_is_caster -> act.attack`, `cond.self_out_of_arrows -> act.loot_arrows`,
  `cond.foe_any -> act.equip_ranged`. (No melee swap by default, the Ranger kites.)
- Feasibility: NPC-SAFE.

#### Master Thief (from Thief)
- Passive: lifts L5, L8, F1.
- Signature, "Shadow Hand": the first hit on a foe that has not detected the Master Thief paralyzes it for 2 s
  (ApplyCombatHitSpell with a target "not detecting the attacker" condition), and while sneaking it gains a muffle ability
  (ApplySneakingSpell). Paralyze, not poison, because followers cannot apply poisons (tree page) and ModPoisonDoseCount is D.
  LIKELY (detection condition on an NPC attacker, ApplySneakingSpell unobserved on NPCs).
- Package: Thief, more extreme. Breaks off to re-sneak, stalks longest, attacks paralyzed or fleeing foes.
- CSTY melee: off 0.8, def 0.2, grp 1.4, aThr 0.8. atkInc 3.0, paInc 2.0, paBlk 0.2, bash 0.0. circ 1.4, fall 1.4, flank 2.5,
  stalk 2.5, strafe 0.6. DATA 1|2.
- Harbinger: ch.3 sneak and ch.16 detection, ch.18 power share max 0.2.
- Default gambits: Thief's set, with `cond.foe_fleeing -> act.attack` first. Needs **(MISSING act.sneak,
  cond.self_hidden, cond.foe_unaware, act.steal)**.
- Feasibility: LIKELY overall. The stealth line is the weakest NPC fit in the roster.

#### Wizard (from Evoker + Warlock)
- Passive: lifts L9, L12, L13, F3.
- Signature, "Arcane Ward": wards absorb 50% of the spell damage they stop as magicka (ModWardMagickaAbsorptionPct),
  Destruction +20% magnitude against a target under one of its Illusion effects (ModSpellMagnitude with a target
  magic-effect keyword condition). NPC-SAFE for the ward part, LIKELY for the keyword condition on the target.
- Package: the battle caster. Controls the strongest foe, wards against casters, then burns down the rest. Mid range,
  more movement than an Evoker.
- CSTY cast: off 1.3, def 0.6, grp 0.9, aThr 0.7. circ 0.5, fall 0.8, flank 0.3, stalk 0.2, strafe 0.7.
- Default gambits: `cond.foe_is_caster -> act.cast_self (ward)`, `cond.foe_stronger_than_me -> act.cast_target (paralyze, Enemy)`,
  then Evoker's element gambits.
- Feasibility: NPC-SAFE / LIKELY as above.

#### Necromancer (from Conjurer)
- Passive: lifts L10, L13, F3.
- Signature, "Legion": one more summon or thrall at a time (ModCommandedActorLimit +1), reanimated thralls rise with
  +50% health (ApplyReanimateSpell). Limit: M for the engine's own AI casts, effective for MFO's summon road (Summon.cpp
  reads it). PLAYER-ONLY RISK for AI-chosen casts. Reanimate: NPC-SAFE.
- Package: the grave caller. Stays far back, raises the fallen, keeps two minions up, drains the foe that reaches it.
- CSTY cast: off 0.5, def 0.9, grp 0.5, aThr 0.6. circ 0.2, fall 1.6, flank 0.0, stalk 0.5, strafe 0.3.
- Default gambits: Conjurer's set plus `cond.foe_any -> act.cast_target (reanimate on a corpse)` **(MISSING: a
  corpse target, cast_target picks live foes)**.
- Feasibility: the second-summon part and the corpse target are the risks.

#### Priest (from Healer)
- Passive: lifts L11, L13, F3.
- Signature, "Sanctuary": a cloak ability, undead and Daedra within 6 m take 4 sun damage a second (cloak spell
  conditioned on the victim's ActorTypeUndead or ActorTypeDaedra keyword), Restoration costs 20% less. Cloak abilities run
  on NPCs (vanilla flame cloak). NPC-SAFE for the cost, LIKELY for the cloak filter.
- Package: Healer with teeth. Same leash and heal priority, but steps forward against undead.
- CSTY cast: off 0.6, def 1.0, grp 0.5, aThr 0.7. circ 0.3, fall 0.9, flank 0.0, stalk 0.3, strafe 0.4.
- Default gambits: Healer's set with `cond.foe_is_unholy -> act.cast_target (sun / turn undead)` raised above the
  self-heal.
- Feasibility: as above.

### TIER 3 HYBRIDS (armor free)

#### Spellsword (melee + Destruction)
- Passive: lifts L9 and the wielded melee line (L3 or L4, plus L1 with a shield).
- Signature, "Spellstrike": melee hits deal 10 extra damage of the element of the spell in the other hand, or fire with
  none (ApplyCombatHitSpell, three conditioned entries), and Destruction costs 20% less while a melee weapon is in the
  right hand (ModSpellCost). NPC-SAFE.
- Package: blade in one hand, fire in the other. Opens with a spell at mid range, closes and fights head on, casts again
  when the foe backs off.
- CSTY melee: off 1.4, def 0.5, grp 1.0, aThr 0.4. paInc 1.5, paBlk 1.5, bash 0.5. circ 0.6, fall 0.4, flank 0.4, stalk 0.1.
  CSTY cast: base with off 1.4, fall 0.3 (it does not back away to cast).
- Harbinger: the weapon plus spell hand split is the F3 deny-hand work (memory apmf-f3-denyhand-followups), and the swap
  must be clean through the equip authority (tree page open point).
- Default gambits: `cond.foe_beyond_range 400 -> act.cast_target (Destruction, Enemy)`, `cond.foe_weak_fire -> act.cast_target (fire)`,
  `cond.foe_within_range 400 -> act.attack`, `cond.foe_any -> act.equip_melee`.
- Feasibility: NPC-SAFE. Hand split risk (G-M3).

#### Sorcerer (melee + Conjuration)
- Passive: lifts L10 and the wielded melee line.
- Signature, "Bound Mastery": bound weapons deal +30% damage (ModAttackDamage with a bound-weapon condition) and
  summons last twice as long (ModSpellDuration on summon effects). LIKELY: the bound-weapon condition must be verified on
  the record (a keyword or the weapon's bound flag).
- Package: summons first, then fights beside the summon with a bound weapon, flanks while the summon holds the front.
- CSTY melee: off 1.2, def 0.5, grp 1.6, aThr 0.4. circ 1.0, fall 0.4, flank 1.5, stalk 0.4. DATA 1|2.
  CSTY cast: base.
- Default gambits: `cond.foe_any -> act.cast_self (summon)`, `cond.always -> act.cast_self (bound weapon)` **(VERIFY: a
  bound-weapon spell must count as the melee weapon for the equip authority)**, `cond.foe_any -> act.attack`.
- Feasibility: LIKELY.

#### Templar (melee + Restoration, defensive shield healer)
- Passive: lifts L11 and the wielded melee line (normally L4 + L1).
- Signature, "Bash of Mending": a shield bash heals the Templar 20 Health and deals 20 sun damage to undead
  (ApplyBashingSpell with two conditioned spells), Restoration costs 25% less while a shield is equipped. NPC-SAFE.
- Package: front-line paladin. Holds the line like a Knight, heals itself and the ally beside it, bashes often.
- CSTY melee: off 0.8, def 1.6, grp 0.6, aThr 0.2. bash 2.0, bRec 2.0, bAtk 1.0, bPA 1.0. circ 0.3, fall 0.2, flank 0.0,
  stalk 0.0. CSTY cast: base with fall 0.2 (heals from the front).
- Harbinger: ch.23 short leash, ch.18 Bash governed, power share max 0.25.
- Default gambits: `cond.ally_hp_pct_below 40 -> act.cast_target (heal, ally)`, `cond.self_hp_pct_below 50 -> act.cast_self (heal)`,
  `cond.foe_attacking_ally -> act.taunt`, `cond.foe_is_undead -> act.attack`, `cond.foe_any -> act.equip_melee`.
- Feasibility: NPC-SAFE.

#### Nightblade (melee + Illusion / Alteration, shadow fighter)
- Passive: lifts L12 and the wielded melee line.
- Signature, "Shadowstrike": melee hits on a feared, calmed or paralyzed foe deal +50% damage (ModAttackDamage with a target
  magic-effect keyword condition), and Illusion costs 25% less. LIKELY (target keyword condition).
- Package: fears the strongest foe, then strikes it from the flank while it flees. Muffled, keeps moving, re-engages
  from the side.
- CSTY melee: off 1.1, def 0.3, grp 1.2, aThr 0.6. atkInc 2.5, paInc 2.0, paBlk 0.5, bash 0.2. circ 1.2, fall 0.7, flank 2.0,
  stalk 1.0, strafe 0.5. DATA 1|2. CSTY cast: base with flank 1.0.
- Default gambits: `cond.foe_stronger_than_me -> act.cast_target (fear, Enemy)`, `cond.foe_fleeing -> act.attack`,
  `cond.always -> act.cast_self (muffle or armor spell)`, `cond.foe_any -> act.equip_melee`.
- Feasibility: LIKELY. Note the tree page open point: its only one-handed source is the Knight unless the dagger counts.

#### Witchhunter (ranged + Illusion / Alteration utility)
- Passive: lifts L7 and L12.
- Signature, "Hexbreaker": arrows deal +25% damage to undead, Daedra and casters, and every hit drains 15 Magicka
  (ApplyCombatHitSpell, DamageMagicka). NPC-SAFE.
- Package: the hunter of mages. Long range, paralyzes or slows the foe that closes, keeps a mage armor up, shoots casters
  first.
- CSTY ranged: off 1.0, def 0.6, grp 0.8, aThr 0.9 (it reads incoming spells best). circ 0.5, fall 1.2, flank 0.4, stalk 0.4,
  strafe 1.0. CSTY cast: base.
- Default gambits: `cond.foe_is_caster -> act.attack`, `cond.foe_is_undead -> act.attack`, `cond.foe_is_daedra -> act.attack`,
  `cond.foe_within_range 250 -> act.cast_target (paralyze, Enemy)`, `cond.foe_any -> act.equip_ranged`.
- Feasibility: NPC-SAFE.

#### Scout (ranged + Restoration, healing archer)
- Passive: lifts L7 and L11.
- Signature, "Field Medic": a cloak ability, allies within 6 m regenerate +50% Health (cloak conditioned to non-hostile
  actors), and Restoration costs 20% less. LIKELY (ally filter on a cloak).
- Package: shoots from behind the party, swaps to a heal when an ally drops low, then back to the bow. Leashed near the
  player, never in front.
- CSTY ranged: off 0.8, def 0.8, grp 0.6, aThr 0.7. circ 0.3, fall 1.0, flank 0.1, stalk 0.4, strafe 0.6. CSTY cast: base.
- Harbinger: ch.23 leash.
- Default gambits: `cond.ally_hp_pct_below 40 -> act.cast_target (heal, ally)`, `cond.player_hp_pct_below 50 -> act.cast_player (heal)`,
  `cond.foe_any -> act.equip_ranged`, `cond.foe_attacking_player -> act.attack`.
- Feasibility: LIKELY (cloak). The bow to spell swap is G-M3.

### TIER 4 MASTERS

#### Arch-Mage (two of Wizard, Necromancer, Priest)
- Passive: lifts the lines of both tier-3 parents (always L13 and F3, plus two or three school lines).
- Signature, "Magister": all spells cost 20% less, and every school line the follower has MASTERED is at equipped rank, not
  only the parents' (ModSpellCost, faction ranks). NPC-SAFE.
- Package: picks per fight: summons when outnumbered, wards against casters, burns the rest. The steadiest caster.
- CSTY cast: off 1.2, def 0.8, grp 0.8, aThr 0.8. circ 0.4, fall 1.0, flank 0.2, stalk 0.3, strafe 0.6.
- Default gambits: the parents' sets merged by priority: ward vs caster, summon vs 3+ foes, heal below 40%, element vs weakness.
- Feasibility: NPC-SAFE.

#### Living Legend (Blademaster + Champion)
- Passive: lifts L2, L3, L4, L5, L6, F1.
- Signature, "Legend": power attacks cost no stamina (ModPowerAttackStamina x0), every power attack sweeps (SetSweepAttack),
  incoming damage -10% (ModIncomingDamage). NPC-SAFE.
- Package: switches between the Champion's head-on power style (one big foe) and the Blademaster's flanking flurry
  (many foes). Never retreats.
- CSTY melee: off 1.8, def 0.3, grp 1.4, aThr 0.6. atkInc 2.0, paInc 3.0, paBlk 3.0, bash 0.5. circ 1.0, fall 0.2, flank 1.5,
  stalk 0.1. DATA 1|2 and dual twin.
- Harbinger: ch.18 no cap. The style switch by foe count needs a per-fight CSTY switch (a second variant) or the G-H2 floor.
- Default gambits: `cond.foe_multiple_within 2 -> act.power_attack`, `cond.foe_blocking -> act.power_attack`,
  `cond.foe_highest_hp -> act.attack`, `cond.foe_any -> act.equip_melee`.
- Feasibility: NPC-SAFE.

#### Ebony Warrior (Great Knight + Templar, ultimate tank)
- Passive: lifts L1, L2, L4, L11, F1.
- Signature, "Unbroken": never staggered (ModIncomingStagger x0), incoming damage -15%, and below 25% Health an ability
  adds +150 armor (ability with a health-percent condition). NPC-SAFE.
- Package: Great Knight that heals. Taunts, bashes, never moves back, heals itself from the front.
- CSTY melee: off 0.9, def 2.2, grp 0.5, aThr 0.0. bash 3.0, bRec 3.0, bAtk 2.0, bPA 2.0. circ 0.1, fall 0.0, flank 0.0,
  stalk 0.0. DATA dueling.
- Harbinger: ch.18 power share max 0.2, ch.23 leash.
- Default gambits: `cond.foe_attacking_ally -> act.taunt`, `cond.foe_count_at_least 2 -> act.taunt`,
  `cond.self_hp_pct_below 40 -> act.cast_self (heal)`, `cond.foe_any -> act.equip_melee`.
- Feasibility: NPC-SAFE.

#### Grand Battlemage (two of Spellsword, Sorcerer, Nightblade)
- Passive: lifts the two parents' school lines and the wielded melee line.
- Signature, "Conduit": weapon hits absorb 8 Magicka from the target (ApplyCombatHitSpell, absorb archetype), spells cost
  20% less while a foe is within melee range. LIKELY (absorb from a hit spell restores the attacker in vanilla, unobserved
  on an MFO follower).
- Package: the most fluid hybrid. Spell at range, blade up close, never stands still, flanks while casting.
- CSTY melee: off 1.4, def 0.5, grp 1.2, aThr 0.6. circ 1.0, fall 0.4, flank 1.2, stalk 0.2. CSTY cast: off 1.4, fall 0.3, flank 0.8.
- Default gambits: the parents' sets, spell before closing, melee inside 400 units.
- Feasibility: LIKELY. G-M3 hand split.

#### Jaqspur (two of Ranger, Scout, Witchhunter, the bow master)
- Passive: lifts L5, L7 and the parents' school line if any.
- Signature, "Thousand Arrows": bow damage +20%, every arrow ignores 50% of armor (ModTargetDamageResistance) and always
  staggers (ModTargetStagger large). NPC-SAFE.
- Package: the sniper. Longest stalk, holds a high spot, focuses one target at a time until it drops, kites everything.
- CSTY ranged: off 1.3, def 0.5, grp 0.6, aThr 0.9. circ 0.4, fall 1.8, flank 0.4, stalk 1.0, strafe 1.0.
- Harbinger: ch.20 target pin on the focus target. High ground positioning does not exist (memory combat-los-obstructions-and-terrain backlog).
- Default gambits: `cond.foe_highest_level -> act.attack`, `cond.foe_is_ranged -> act.attack`, `cond.self_out_of_arrows -> act.loot_arrows`,
  `cond.foe_any -> act.equip_ranged`.
- Feasibility: NPC-SAFE.

#### Nightingale (Master Thief + Witchhunter)
- Passive: lifts L5, L7, L8, L12.
- Signature, "Shroud of Nocturnal": in darkness (light level condition) +30% damage and 50% harder to see
  (ModDetectionLight), and weapon hits absorb 10 Health. ModDetectionLight NPC-SAFE, darkness condition NPC-SAFE (conditions
  are evaluated for any actor), absorb LIKELY.
- Package: shadow archer and blade. Fights from the dark at range, closes only on paralyzed or fleeing foes.
- CSTY ranged: off 1.0, def 0.4, grp 1.0, aThr 0.9. circ 0.8, fall 1.4, flank 1.5, stalk 2.0, strafe 1.0. CSTY melee: Master Thief's.
- Default gambits: `cond.dark -> act.sneak` **(MISSING)**, `cond.foe_is_caster -> act.attack`, `cond.foe_fleeing -> act.equip_melee`,
  `cond.foe_any -> act.equip_ranged`.
- Feasibility: LIKELY.

#### Listener (Master Thief + Nightblade)
- Passive: lifts L5, L8, L12 and the wielded melee line.
- Signature, "Black Sacrament": sneak attacks x2 more (ModSneakAttackMult, LIKELY), hits on a foe below 25% Health deal +50%
  (ModAttackDamage with a target health condition, NPC-SAFE).
- Package: the executioner. Fears the pack, picks the weakest or wounded foe and finishes it, then the next.
- CSTY melee: off 1.3, def 0.2, grp 1.6, aThr 0.7. atkInc 3.0, paInc 2.5, paBlk 0.5, bash 0.0. circ 1.4, fall 0.8, flank 2.5,
  stalk 1.5, strafe 0.6. DATA 1|2 and dual twin.
- Harbinger: ch.3 / ch.16 stealth, ch.18 power share max 0.25.
- Default gambits: `cond.foe_lowest_hp -> act.attack`, `cond.foe_hp_pct_below 25 -> act.power_attack`,
  `cond.foe_count_at_least 3 -> act.cast_target (fear, Enemy)`, `cond.foe_any -> act.equip_melee`.
- Feasibility: execute NPC-SAFE, sneak part LIKELY.

#### Saint (two of Priest, Templar, Scout, the holy warrior)
- Passive: lifts L11, L13 or the wielded weapon line, and the parents' other lines.
- Signature, "Dawn's Grace": weapon hits deal 15 sun damage, 30 to undead (ApplyCombatHitSpell with conditions), and a
  cloak gives allies within 6 m +50 armor (cloak to non-hostile actors). Hit part NPC-SAFE, cloak LIKELY.
- Package: leads the party against undead and Daedra, heals from the front, holds a short leash otherwise.
- CSTY melee: off 1.1, def 1.2, grp 1.0, aThr 0.3. bash 1.5, bRec 1.5. circ 0.4, fall 0.3, flank 0.2, stalk 0.1.
  CSTY cast: base with fall 0.4. CSTY ranged (Scout parent): ranged base.
- Default gambits: `cond.ally_downed -> act.cast_target (heal, ally)`, `cond.foe_is_unholy -> act.attack`,
  `cond.ally_hp_pct_below 40 -> act.cast_target (heal, ally)`, `cond.foe_attacking_ally -> act.taunt`.
- Feasibility: as above.

### TIER 5 (hidden)

#### Dragonborn (3 distinct tier 4 mastered)
- Passive: lifts EVERY mastered line to equipped rank. No class restriction on gear or magic (the exception to the
  off-class weapon rule, memory).
- Signature, "Thu'um": knows every common shout (Actor::AddShout of the vanilla word-of-power shouts, resolved at
  runtime, never list data), shout cooldowns 30% shorter (ShoutRecoveryMult ability, LIKELY on NPCs), and the class CSTY
  scores shouts high. NPC shouting itself is vanilla engine behaviour (draugr, Greybeards).
  PLAYER-ONLY RISK: ModShoutOK is D, so no perk entry may gate shouting.
- Package: the engine's best fighter of whatever kind the follower currently holds: inherits the CSTY of its strongest
  mastered tier-4 for the stance, with shoutScoreMult 5.0 added to every variant.
- CSTY: per stance, the best tier-4 variant plus shoutScoreMult 5.0.
- Harbinger: ch.14 is arbitration only, so a gambit-driven shout needs a shout drive. GAP G-H5.
- Default gambits: `cond.foe_multiple_within 3 -> act.shout (Unrelenting Force)` **(MISSING act.shout)**,
  `cond.foe_is_dragon_flying -> act.shout (Dragonrend)` **(MISSING)**, then the strongest tier-4 parent's set.
- Feasibility: shouting via the AI is NPC-SAFE, shouting by gambit needs G-H5 and the base action.

---

## 3. Feasibility tally

Counted per class effect above (passive lines counted once in section 1):
- PLAYER-ONLY RISK: 4. Thief / Master Thief Steal (pickpocket entry points D, Steal must be a loose take), Necromancer's
  extra summon for the AI's own casts (ModCommandedActorLimit M), Dragonborn shout gating (ModShoutOK D), and the dual-cast
  idea (CanDualCastSpell M, deliberately not used anywhere in this draft).
- LIKELY: 15. L6 SpeedMult, L8 sneak attack, Master Thief Shadow Hand, Wizard target keyword, Priest cloak, Sorcerer
  bound condition, Nightblade target keyword, Scout cloak, Grand Battlemage absorb, Nightingale absorb, Listener sneak
  attack, Saint cloak, Dragonborn ShoutRecoveryMult, plus two behaviour claims that rest on an unmeasured CSTY meaning
  (defensiveMult as block frequency, avoidThreat as dodge).
- Everything else NPC-SAFE.

---

## 4. Gaps

### Base gambits that do not exist (Vocabulary.h on origin/main)
- G-G1 `act.sneak` (enter and hold sneak). Harbinger ch.3 Stance already makes an actor sneak.
- G-G2 `act.steal` (designed in the rogue memo, not built, crime pre-check required).
- G-G3 `cond.self_hidden` / `cond.foe_unaware` (no detection conditions in base, Harbinger `SenseActor` can back them).
- G-G4 `act.shout` (Dragonborn, and marth's spec says shouts are gambitable).
- G-G5 `cond.foe_staggered` (Champion, Living Legend).
- G-G6 `cond.self_has_summon` (Conjurer order clarity, today the summon limit read stops the recast).
- G-G7 a corpse target for `act.cast_target` (Necromancer reanimate).
- G-G8 `act.bash` and a held block. Not needed if ch.18 governs Bash, so lowest priority.
- G-G9 dual cast (memory mage-dualcast TODO). Not used by this draft, CanDualCastSpell is M.

### Harbinger work
- G-H1 ch.18 attack selection: designed on 86e3bw775, probe in review, not on main. Every melee class's power share,
  Bash and DualWield governance depends on it. With it absent, the classes still differ through CSTY and gambits.
- G-H2 a power FLOOR. v1 has powerShareMin observe-only. Proposal: a declared "Normal share max" (deny Normal over the
  window) is a deny, so it fits the additive rule and makes Warrior and Champion power attack more without adding moves.
- G-H3 an evasion facet. No CSTY field, no seat. List-agnostic shape: when a dodge framework is present, ask it, else
  the vanilla avoid-threat side step. Animations must exist (memory "animations required for all actions"), so no
  invented dodge. Mercenary, Blademaster, Living Legend need it most.
- G-H4 the left-hand context for dual wielders may never be built by the tree (86e3bw775 open question 2). Blademaster's
  identity depends on it.
- G-H5 a shout drive behind ch.14 (today arbitration only).
- G-H6 a combat-style facet or slider (marth 2026-10-27) so strict adherence can override a follower's authored style
  through Harbinger instead of MFO's own 0x38 write.

### MFO work
- G-M1 class CSTY variants: one per class per stance plus dual twins (about 70 records, generated, not hand made), and
  `CombatStyle::ApplyTick` picking the class variant. The Stance enum ordinals stay frozen (serialized ABI).
- G-M2 the class add-on's own versioned record re-applies mastery perks, faction ranks and abilities at load (not PRGN).
- G-M3 clean weapon and spell hand splits for hybrids (F3 deny-hand follow-ups, equip authority swap with animation).
- G-M4 progression query for perk effects (memory progression-owns-player-perk-effects): any MFO code that reads a
  class passive's value (summon limit, steal) asks progression, never reads the perk itself.
- G-M5 verify every perk condition used above against the real record and condition functions before authoring the ESL
  (bound-weapon test, target magic-effect keyword, cloak ally filter). Observe each LIKELY item in one probe run first
  (principle 5).
- G-M6 CSTY field meanings are vanilla Creation Kit sense, not measured. A passive probe of block and side-step rates per
  defensiveMult and avoidThreatChance would turn the two behaviour LIKELYs into facts.
