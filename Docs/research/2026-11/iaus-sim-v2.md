# IAUS simulator v2: a game-faithful combat model, and the lab for IAUS intelligence

Status: WORK IN PROGRESS on `feat/iaus-sim-v2`. This file is the research record. Every layout, formula and
calibration number found so far is here so nobody has to derive it again. The handoff log with the exact next step
is `scratchpad/agentlogs/iaus-sim-v2.md` (session scratchpad, not in the repo).

Host-only tools. Nothing here is part of the DLL. List data is TEST INPUT only: it lives in
`~/.cache/mfo-list-balance/`, it is never committed, and `native/` never reads it.

## 1. Pipeline

```
installed list (read-only, 'rb')            unpacked SkyrimSE.exe (read-only)
        |                                              |
tools/list_balance/extract_world.py          tools/list_balance/gmst_defaults.py
tools/list_balance/extract.py                          |
        |                                    gmst_defaults.json (3726 code defaults)
<list>.world.json (raw winning records)                |
<list>.json (taught spell pool)                        |
        +----------- tools/list_balance/compile_world.py ---+
                              |
                    <list>.sim.json  (6 to 11 MB)
                              |
                    tools/iaus_sim/run.sh --list <list>.sim.json
```

Commands (each list takes under a minute):

```
python3 -I tools/list_balance/gmst_defaults.py binaries/1.6.1170/SkyrimSE.unpacked.exe
python3 -I tools/list_balance/extract_world.py --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data"
python3 -I tools/list_balance/extract.py       --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data"
python3 -I tools/list_balance/compile_world.py --name LoreRim
# Tuxborn: --profile "Tuxborn - Deck" --data "Game Root/Data".  Vanilla reference set: add --vanilla to both extractors (name Vanilla).
tools/iaus_sim/run.sh [--list ~/.cache/mfo-list-balance/LoreRim.sim.json] [--seed N] [--fights N] [--threads N]
                      [--baseline out] [--compare base] [--taunt-hidden] [--no-engine-casts] [--g10-sticky]
                      [--classes legacy|real|slug,slug] [--profiles mech] [--print-classes]
                      [--scenario all|name,name [--oracle --oracle-k 8 --oracle-h 150] [--trace N]]
python3 -I tools/iaus_sim/calib_logs.py        # real fight shapes out of _evidence/*/MFO.log
```

Three data sets exist: `Vanilla` (the five base ESMs of the Tuxborn install), `Tuxborn` (SimonRim) and `LoreRim`
(Requiem). With no `--list` the sim runs its BUILT-IN fixture set: Skyrim.esm's game-setting values plus a small
hand-written roster. The fixtures are not game data.

**v1 extractor bug, fixed here.** `plugins.txt` does not list the implicit masters (Skyrim.esm, Update.esm,
Dawnguard.esm, HearthFires.esm, Dragonborn.esm, then the Creation Club files of `Skyrim.ccc`). The v1 `extract.py`
therefore read only vanilla records that some mod overrode. `find_plugins` now loads them first. The v1 JSONs are kept
as `<list>.v1-noimplicit.json`.

## 2. Record layouts used (each one self-checked)

`extract_world.py --selfcheck` prints one row per anchor and a MISS when a list overrides the vanilla value (that is
information, not an error). All offsets are bytes into the subrecord.

| Record | Subrecord | Layout | Check |
|---|---|---|---|
| WEAP | DATA | value u32 @0, weight f32 @4, damage u16 @8 | Iron Sword 7 |
| WEAP | DNAM (100) | anim type u8 @0, speed f32 @4, reach f32 @8, flags u16 @12, flags2 u32 @40, skill i32 @76, stagger f32 @96 | Iron Sword anim 1 skill 6 speed 1.0 stagger 0.75, Iron Greatsword anim 5 skill 7 speed 0.7, Hunting Bow anim 7 skill 8 |
| WEAP | CRDT | crit damage u16 @0 | |
| ARMO | DNAM | armor rating x100 i32 | Iron Cuirass 25 |
| ARMO | BOD2 | slot mask u32 @0, type u32 @4 (0 light, 1 heavy, 2 clothing). BODT: type @8 | Iron Cuirass heavy, slot 0x4 |
| AMMO | DATA | flags u32 @4 (0x4 = not a bolt), damage f32 @8, value u32 @12 | |
| RACE | DATA | 7 x (skill AV i8, boost i8) @0, flags u32 @32 (bit 0 playable), health @36, magicka @40, stamina @44, mass @52, heal rate @84, magicka rate @88, stamina rate @92, unarmed damage @96, unarmed reach @100 | Nord playable, hp 50, unarmed 4 |
| RACE | ATKD (44) | damage mult f32 @0, chance @4, flags u32 @12 (0x4 power), stagger @24, recovery @36, stamina mult @40 | |
| NPC_ | ACBS (24) | flags u32 @0, magicka offset i16 @4, stamina offset i16 @6, level u16 @8 (x1000 multiplier when flag 0x80), calc min @10, calc max @12, speed mult @14, template flags u16 @18, health offset i16 @20 | Bandit hp 35, mp 25, sp 70 |
| NPC_ | DNAM | 18 skill values, 18 skill offsets, then u16 health @36, magicka @38, stamina @40. These are the values the CK calculated | wolf 22, frost troll 460, giant 591, dragon 905 |
| NPC_ | AIDT | aggression u8 @0, confidence u8 @1, assistance u8 @5 | |
| NPC_ | TPLT + ACBS template flags | 0x1 traits (race), 0x2 stats, 0x4 factions, 0x8 spell list and perks, 0x10 AI data and combat style, 0x80 base data, 0x100 inventory, 0x1000 keywords | |
| CLAS | DATA (36) | 18 skill weights u8 @6, health / magicka / stamina weights u8 @32 / 33 / 34 | used by the auto-calc proof below |
| CSTY | CSGD | 10 floats: offensive, defensive, group offensive, melee, magic, ranged, shout, unarmed, staff, avoid threat | |
| CSTY | CSME | 8 floats: attack staggered, power attack staggered, power attack blocking, bash, bash recoil, bash attack, bash power attack, special attack | |
| CSTY | CSCR / CSLR / DATA | close range 4 floats (circle, fallback, flank distance, stalk). long range strafe. flags (1 dueling, 2 flanking, 4 dual wield) | |
| MOVT | SPED | 11 floats. forward walk @16, forward run @20 | NPC_1HM_MT 80 / 370, Wolf_Run_MT 556, Draugr1HM_MT 345 |
| LVLN / LVLI | LVLO (12) | level u16 @0, form u32 @4, count u16 @8. LVLF flags (1 = all levels at or under the PC, 4 = use all). LVLD chance none | |
| ECZN | DATA (12) | min level u8 @9, flags u8 @10 (2 = match PC below minimum), max level u8 @11 | |
| FACT | XNAM (12) | faction u32, modifier i32, combat reaction u32 (0 neutral, 1 enemy, 2 ally, 3 friend) | |
| PERK | PRKE / DATA / PRKC / CTDA / EPFD | entry type u8 (1 ability, 2 entry point). entry point u8, function u8. condition tab i8. EPFD one or two floats | entry point ids from the fork's `BGSEntryPoint.h` |
| CTDA (32) | | compare op in the top 3 bits of byte 0 (0 ==, 1 !=, 2 >, 3 >=, 4 <, 5 <=), OR flag bit 0, value f32 @4, function u16 @8, param1 u32 @12, param2 u32 @16 | function ids from the fork's `TESCondition.h` |
| SPEL | SPIT | cost u32 @0, flags u32 @4 (1 = cost override), type u32 @8 (0 spell, 4 ability), charge time f32 @12, cast type @16, delivery @20, range @28 | (v1) |
| MGEF | DATA | flags @0 (1 hostile, 2 recover, 4 detrimental), base cost f32 @4, school AV i32 @12, resist AV i32 @16, min skill u32 @40, archetype u32 @64, primary AV i32 @68, cast type @80, delivery @84 | (v1) |
| CELL / ACHR | | ACHR NAME = base (always an NPC_), DATA position 3 floats, XEZN encounter zone. CELL XEZN, DATA bit 0 interior | |

Facts about the data that changed the design:

- A placed actor's base is always an `NPC_`. Leveling is the NPC's template pointing at a leveled list (`LVLN`).
  The sim resolves the template chain per ACBS template flag at the encounter's level.
- Bandits are hostile to the player because their AI data says "very aggressive" (aggression 2 attacks neutrals),
  NOT because of a faction enemy relation. Hostility rule used: aggression 3, or aggression 1+ with an enemy
  reaction toward PlayerFaction in either direction, or aggression 2 with no ally / friend reaction.
- The engine binds a race to its movement types through behaviour graph state names. No record states the link.
  MODEL: the `MOVT` whose editor id starts with the race's name stem, else `NPC_1HM_MT`.
- Condition function ids verified in the fork: GetActorValue 14, GetIsRace 69, GetIsID 72, GetRandomPercent 77,
  GetWeaponAnimType 108, IsWeaponSkillType 109, GetEquipped 182, HasMagicEffect 214, HasSpell 264,
  GetBaseActorValue 277, GetIsObjectType 432, HasPerk 448, HasKeyword 560, GetEquippedItemType 597,
  IsPowerAttacking 673, WornHasKeyword 682, EPMagic_SpellHasKeyword 693, EPMagic_SpellHasSkill 696, IsAttackType 697,
  HasMagicEffectKeyword 699, WornApparelHasKeywordCount 722.
- Entry point ids verified in the fork: CalculateWeaponDamage 0, ModPowerAttackStamina 27, ModPowerAttackDamage 28,
  ModSpellMagnitude 29, ModSpellDuration 30, ModAttackDamage 35, ModIncomingDamage 36, ModTargetDamageResistance 37,
  ModSpellCost 38, ModIncomingSpellMagnitude 41, ModArmorRating 85.

## 3. Game settings: code default, then Skyrim.esm, then the list

`gmst_defaults.py` reads every setting's compiled-in value from the executable. Each one is a statically initialised
`Setting` object in `.data`: vtable pointer at +0, the value at +8, a pointer to the name string at +0x10. Observed at
`fArmorScalingFactor` on 1.6.1170 (0x3E19999A = 0.15 in code, Skyrim.esm sets 0.12). 3726 settings.

Skyrim.esm does NOT define some settings, so the code default is live in an unmodded game. The ones that matter:

| Setting | Code default | Skyrim.esm | Tuxborn | LoreRim | Meaning |
|---|---|---|---|---|---|
| fDamageSkillMin / Max (NPC) | 1.0 / 3.0 | not defined | not defined | 1.0 / 1.0 | NPC weapon damage is x1 at skill 0 and x3 at skill 100 in vanilla. Requiem turns that off |
| fDamagePCSkillMax | 3.0 | 1.5 | 2.0 | 1.0 | the player's version |
| fArmorRatingMax (NPC) | 2.5 | not defined | 2.5 | 1.0 | NPC armor rating x1 to x2.5 by armor skill |
| fArmorScalingFactor | 0.15 | 0.12 | 0.15 | 0.10 | mitigation percent per point of rating |
| fMaxArmorRating | 95 | 80 | 75 | 75 | mitigation cap in percent |
| fArmorBaseFactor | 0.02 | 0.03 | 0.0 | 0.0 | hidden mitigation per worn piece |
| fNPCHealthLevelBonus | 5 | not defined | 5 | 2 | NPC health per level |
| iAVDhmsLevelUp | 10 | 10 | 10 | 5 | attribute points per level, split by class weights |
| iAVDSkillsLevelUp | 10 | 8 | 8 | 8 | NPC skill points per level, split by class weights |
| fPowerAttackDefaultBonus | 3.0 | 1.0 | 1.0 | 1.0 | power attack = x(1 + bonus) |
| fStaminaAttackWeaponBase / Mult | 7 / 0.1 | 20 / 1.0 | 20 / 1.0 | 20 / 0.25 | power attack stamina |
| fPowerAttackStaminaPenalty | 5 | 2 | 2 | 2 | |
| fCombatMagickaRegenRateMult | 0.75 | 0.33 | 0.5 | 0.5 | |
| fCombatStaminaRegenRateMult | 0.75 | 0.35 | 0.5 | 1.0 | |
| fCombatHealthRegenRateMult | 0.75 | 0.0 | 0.0 | 1.0 | see the UNPROVEN note in the model table |
| fMagicCasterSkillCostBase / Mult, fMagicSkillCostScale (NPC) | 0.005 / 1.0 / 0.5 | - / 0.5 / 0.5 | - / 0.5 / 0.5 | - / 1.0 / 0.5 | NPC magicka cost by skill |
| fDiffMultHPToPCN | 1.0 | - | 1.5 | 1.0 | damage to the player on the list's normal difficulty |
| fBowDrawTime | 1.6667 | - | - | 1.6667 | |

The damage / mitigation call chain itself was proven in the disassembly by the I8 research agent (its log:
`scratchpad/agentlogs/iaus-i8-research.md`): GetDamage reads fDamageWeaponMult and fDamageSkillMin / Max through the
clamped skill value. HitData::Populate runs entry points 0x1A, 0x1C, 0x23, 0x24. Mitigation is the DamageResist AV
x fArmorScalingFactor x 0.01 plus the armor base factor sum, entry point 0x25, clamped by fMaxArmorRating. Spell
magnitude runs entry point 0x1D, dual casting multiplies by fMagicDualCastingEffectivenessBase.

## 4. Model table

Proof levels: PROVEN (engine formula or code verified), DATA (read from the list's records), CALIBRATED (a model
value fitted to field logs), MODEL (a documented approximation), UNPROVEN (shape assumed, flagged).

| # | Sub-model | What the sim does | Source | Level |
|---|---|---|---|---|
| 1 | Decision cadence | world steps at 30 Hz. One follower is served per 133 ms pump tick, round-robin, so each follower decides every 133 ms x followers | `Diagnostics` kPumpMs, `Candidates.cpp:40`. Field logs: 133.16 ms median over 3168 heartbeat intervals | PROVEN |
| 2 | What act.attack does | it only names the target. The engine's combat AI closes, swings, blocks and shoots | `Vocabulary.h:201` | PROVEN (code) |
| 3 | NPC health / magicka / stamina | race base + ACBS offset + (level - 1) x (fNPCHealthLevelBonus [health only] + iAVDhmsLevelUp x class weight share) | matches the CK-stored DNAM health on 111 of 111 vanilla static auto-calc actors | PROVEN |
| 4 | NPC skills | iAVDSkillStart 15 + race boost + (level - 1) x iAVDSkillsLevelUp x class skill weight share. Non auto-calc actors use DNAM values + offsets | matches DNAM skills within 1 on 105 of 111 vanilla actors (rounding) | PROVEN |
| 5 | Leveled actors | template chain per ACBS flag. LVLN / LVLI entries at or under the level, highest level only unless the list's flag says all. PC-level-mult actors: level = PC level x mult, clamped by calc min / max. Zone level = PC level clamped by the encounter zone | record semantics (CK wiki), not re-verified in the disassembly | DATA + MODEL |
| 6 | Weapon damage | (weapon + ammo) x fDamageWeaponMult x lerp(fDamageSkillMin, fDamageSkillMax, skill / 100) x ModAttackDamage perks. Power attack x (1 + fPowerAttackDefaultBonus) | I8 research 2.1: the core formula decoded on AE 26410 (linear in the clamped skill). The power attack multiplier as a pre-swing read is unproven there | PROVEN (normal hit), MODEL (power multiplier) |
| 7 | Unarmed / creature damage | race unarmed damage x the mean ATKD damage mult of its normal attacks | record fields. No level scaling assumed | MODEL |
| 8 | Armor mitigation | min(fMaxArmorRating, rating x fArmorScalingFactor + worn pieces x fArmorBaseFactor x 100) percent | I8 disassembly + GMSTs | PROVEN |
| 9 | Armor rating | sum of piece ratings x lerp(fArmorRatingBase, fArmorRatingMax, armor skill / 100), plus the race skin armor and ability armor | GMSTs, records. Shape by analogy with damage | UNPROVEN shape |
| 10 | NPC perks (attack / power multipliers) | only UNCONDITIONED multiply entries are applied. 87 to 99 percent of the entries on NPC perks carry conditions and are skipped | see section 8 (perks v2 is open) | MODEL, known gap |
| 11 | Follower perks | he holds every playable perk of his weapon / magic tree that his skill meets. A ranked chain = the best value whose skill requirement he meets | PERK conditions (GetBaseActorValue) | MODEL |
| 12 | Spell damage | magnitude x ModSpellMagnitude perks x (1 - element resist) x (1 - magic resist). Damage over time for its duration. Stamina / magicka damage applied | records. Resist stacking as two factors is standard, not re-verified | DATA + MODEL |
| 13 | Magicka cost | record cost x max(0, 1 - fMagicCasterSkillCostMult x (fMagicCasterSkillCostBase x skill)^fMagicSkillCostScale) x ModSpellCost perk of the spell's tier | the shape reproduces the documented player curve with the player's settings. NPC shape not verified | UNPROVEN shape |
| 14 | Illusion / control | a calm / fear / frenzy / turn spell works when the target's level is at or under the magnitude. Paralysis fails on ImmuneParalysis. Turn undead only on undead | MGEF semantics | DATA + MODEL |
| 15 | Resists and weaknesses | from constant abilities (race, NPC, perk abilities): value modifiers on AV 39 to 44, detrimental = a weakness | records | DATA |
| 16 | Melee timing | one-handed swing 1.00 s / weapon speed, two-handed 1.40 s / speed, creature 1.30 s. Hit frame at 45 percent. Power attack x1.55, hit at 55 percent. Then the AI pauses U(0.05, 0.45) s / offensive mult | field hit cadence (weapon enchant procs of one follower): p50 1.21 s, p10 0.83 s, n = 28 | CALIBRATED (small sample) |
| 17 | Bow timing | nock 0.55 s + fBowDrawTime / speed x 0.6 + hold, recover 0.35 s. Arrow speed 3600 units / s | GMST + MODEL | MODEL |
| 18 | Spell timing | SPIT charge time, then the release, 0.45 s recovery. A concentration spell is a 2 s stream. Projectile 2400 units / s | SPIT (DATA), rest MODEL | DATA + MODEL |
| 19 | Potion, weapon swap, taunt | 1.2 s (effect at 0.6 s), 1.3 s, 1.2 s | none | MODEL |
| 20 | Stagger | an unblocked power attack staggers 1.1 s x clamp(mass ratio). Bash 0.8 s. A stagger cancels a cast in its charge | none (the fStagger settings are extracted, not used yet) | MODEL |
| 21 | Block | shield 0.45 + 0.30 x block skill, weapon 0.30 + 0.20 x skill, cap 0.85. A power attack halves it. The defender blocks an incoming swing with chance 0.20 x defensive mult (+0.25 with a shield) | CSTY defensive mult, fBlockPowerAttackMult | MODEL |
| 22 | Engine combat AI | target by distance, last attacker and a retarget timer. Mode from the combat style weights and the kit (caster / ranged / melee). Power attack chance 0.12 x offensive mult (x the blocking / staggered mults). Bash chance 0.18 x bash mult against a caster or blocker. Ranged standoff from fCombatRangedDistance / MinimumRange. Caster standoff 750 | CSTY fields (DATA), every chance MODEL | MODEL |
| 23 | Flee | AIDT confidence: cowardly under 75 percent health, cautious 40, average 20, brave 8, foolhardy never | none | MODEL |
| 24 | Movement | 2D. Run speed = the race's MOVT forward run x ACBS speed mult. Back-pedal at 55 percent | MOVT (DATA), binding MODEL | DATA + MODEL |
| 25 | Line of sight | 0 to 3 circular occluders between the sides. A blocked ray stops shots and aimed spells | none | MODEL |
| 26 | Regen | max x rate percent x combat mult per second, paused for fDamaged*RegenDelay after a loss. Magicka and stamina use fCombat*RegenRateMult. Health uses 0.7 | GMSTs. Health in combat: Skyrim.esm sets the GMST to 0, yet the player does regenerate in combat, so the actor value CombatHealthRegenMult (0.7) is assumed to rule | PROVEN magicka / stamina, UNPROVEN health |
| 27 | Power attack stamina | (fStaminaAttackWeaponBase + weapon weight x fStaminaAttackWeaponMult) x fPowerAttackStaminaPenalty. Normal attacks are free | GMSTs. Weapon weight is not carried through compile_world yet (8 assumed) | DATA, small gap |
| 28 | Bleedout | a follower at 0 health is down 9.5 s, then stands at fBleedoutDefault of his health. The player model dies | field logs: p50 9.5 s, mean 13.1 s, n = 50 | CALIBRATED |
| 29 | Taunt | one attempt, 70 percent, pins the foe 15 s. No new taunt while one is in flight or holds a foe | `Config` fTauntDuration default 15 s, `Snapshot::tauntBusy` (I6). The 70 percent is a model value | PROVEN duration, MODEL chance |
| 30 | Summon | a level-scaled atronach (60 + 12 x level health) for the spell's duration | none | MODEL |
| 31 | Player model | a T1 kit run by the same engine-AI model, 4 potions (drinks at 35 percent health), takes fDiffMultHPToPCN | none | MODEL |
| 32 | Follower kit | weapon / armor / potion ladders of the list's playable unenchanted gear sorted by value, position = (level - 1) / 45 x 0.75 | records (DATA), the ladder rule MODEL | DATA + MODEL |
| 33 | Encounters | hostile placed actors clustered per cell (1800 units), up to 8, with the cell's encounter zone. 1200 sampled per list, at 6 player-level buckets (5, 12, 20, 28, 38, 50) | CELL / ACHR / ECZN | DATA |
| 34 | Brackets | natural = encounters as placed. The others are bands of sqrt(sum effective health x sum dps), foes over party: weak under 0.6, balanced 0.6 to 1.25, strong1 to 2.0, strong2 to 3.5, mixed-boss (one foe holds half the health, 3+ foes) | definition | MODEL |
| 35 | G10 answers | a (spell, target) pair has a WillWork answer only on the lap after it was ranked in the top 6. That is what `Candidates.cpp` does today | code mirror | PROVEN (code) |
| 36 | Cooldown facts | `hardCooldownLeft` and `sinceFired` are filled like `Engine::FillCooldowns`. Sim v1 never filled them | code mirror | PROVEN (code) |

## 5. Calibration against field logs

`tools/iaus_sim/calib_logs.py` over the 28 saved logs in `_evidence/` (LoreRim and deck sessions, 2026-10-20 to
2026-11-02):

| Metric | Field | Sim | Verdict |
|---|---|---|---|
| pump period | p50 133.16 ms (n = 3168) | 133 ms | match |
| bleedout length | p10 6.3, p50 9.5, p90 22.1 s (n = 50) | 9.5 s fixed | median fitted, no spread |
| hit cadence of a melee follower (consecutive weapon enchant procs under 6 s) | p10 0.83, p50 1.21, p90 4.16 s (n = 28) | swing 1.0 / speed + pause 0.05 to 0.45 | median fitted |
| engine spell cast gap, same actor, under 8 s | p50 4.3 s (n = 44, mostly buffs and heals) | not fitted | open |
| incoming damage on a follower, non-zero samples | p10 0.4, p50 3.0, p90 5.0 percent of health per second (n = 95) | to compare per list (todo: the same statistic from the sim) | open |
| fight span seen by `[sense]` | p10 3.2, p50 16.3, p90 100.8 s (n = 68) | LoreRim natural mean 9 to 16 s | same order, sim shorter at the tail |
| foes on one follower | p50 1, p90 3, mean 1.5 | natural encounters p50 1 to 2 | plausible |
| time alive before a down | p50 158 s | not comparable yet (single fights) | open |
| target pins per fight minute | p50 2.1, p90 6.2 | tsw/min 3 to 10 | sim higher, to check |

What the logs can NOT give, and the passive probe lines the next in-game run should produce (rate-limited, config
gated, no hotkeys):

- `[hit-probe] <attacker> -> <target> dmg=<health removed> weapon=<form> power=<0/1> blocked=<0/1> t=<ms>` from the
  hit event sink, for swing cadence per weapon type and damage per hit against the sim's formulas.
- `[fight-probe] <follower> start / end, duration, foes killed, health / magicka / stamina at start and end`, for
  time-to-kill and the resource curves.
- `[cast-probe] <actor> begin / release / interrupted <spell>` with timestamps, for charge time, recovery and
  interrupt rates.
- `[swap-probe]` equip start and the first swing after it, and `[drink-probe]` use and the pool change, for the two
  timing rows that are pure MODEL today.

## 6. First runs (seed 1, 8 fights x 5 ranks per cell, natural bracket)

Win rate, fight length and the player's survival now come from records. Natural encounters, party level 12 to 45:

| List | win % | fight s | player dead % |
|---|---|---|---|
| Vanilla | 90 to 98 | 7 to 17 | 2 to 10 |
| Tuxborn | 80 to 92 | 10 to 17 | 8 to 20 |
| LoreRim | 50 to 78 | 9 to 16 | 22 to 48 |

LoreRim is harder for a level 12 to 45 party because Requiem's world is not leveled. The full three-list run takes
under one second per list on 8 cores.

## 7. Native findings so far (reported, not fixed: native/ is not touched on this branch)

- **D1, G10 answers are forgotten every second lap (`native/iaus/Candidates.cpp`, `Generate.cpp` TopSpellPairs).**
  The ask list held only the pairs of the last decision's `ranked` list. A pair answered No is vetoed by the WillWork
  gate, so it is not ranked, so it is not asked on the next lap, so it has no note, so it is Unknown, and Unknown
  passes. The pair is a live candidate again every other lap.
  **Severity, corrected.** My first measurement (failed control casts 27 / 35 / 60 percent against 15 / 27 / 25 with the
  answers kept) was taken before the sim mirrored `cast/Fire.cpp:430`, where the actuation itself refuses a cast that
  WillWork answers No. With that mirrored the failed casts are the same before and after (they are the first ask, where
  Unknown passes once by design). What D1 costs in the game is a wasted walk try and a decision flicker: 0.1 to 1.0
  refused tries per minute per caster (Tuxborn, 40 fights per cell, mage lines), 0 with the fix.
  **Fix:** branch `fix/iaus-d1-willwork` (0c70d81): `Generate.h NextAskList` (pure, harness-checked) keeps a refused pair
  on the ask list while its last No is younger than 6 s (cast/WillWork's trust window). Gather re-stamps it each lap.
- (more as the gap audit runs)

## 8. Open work on the world model

- **Perks v2.** Requiem (LoreRim) puts most of its combat rules into perk entry points with conditions:
  ModIncomingDamage keyed on weapon keywords (`REQ_DamageType_Slash / Blunt / Pierce / Ranged`) against armor
  keywords (`WornApparelHasKeywordCount`, `WornHasKeyword`), ModTargetDamageResistance, spell magnitude scaled by
  actor values (entry function 14 = multiply by 1 + AV x mult). The extractor already resolves every condition
  parameter by function. A generic evaluator over (owner, weapon or spell, target) is the next sub-model. Until then
  LoreRim melee and ranged damage ignore armor-type interactions.
- Weapon weight through `compile_world.py` (power attack stamina).
- Animation data (hkx annotations) for the swing table instead of the calibrated constants.
- The sim's own "incoming damage percent per second" and fight-span distribution, for the open rows of section 5.

## 9. The lab: utility, the expert oracle, the scenarios

### 9.1 Utility U (0 to 100), one objective for every policy

`Oracle.h Utility()`. Player dead: `10 x share of foe health removed`. Otherwise:
`50 x share of foe health removed + 15 x (1 - duration / 150 s) on a win + 15 x player health share
+ 10 x (1 - follower downs per follower, capped at 1) + 5 x mean follower health share
+ 5 x resources left (half magicka share, half potions left)`.
It says what a skilled player wants: win, fast, the player and the party healthy, nobody down, resources kept.

### 9.2 The expert oracle

A rollout search with full information and no time budget over the same world. At each decision of a free follower:
1. options = IAUS's ranked candidates, plus the best-looking unranked candidate per family (weakest foe, the foe on the
   player, a caster, the most hurt recipient), plus every other weapon category, plus "let his own AI run". At most 16.
2. each option is applied in a copy of the fight and HELD (2.5 s for a target, the action's length for the rest),
3. the copy runs to the horizon with IAUS for everyone, over K random streams shared by all options,
4. the option with the best mean U is taken. The action he is on wins a tie within 0.3 U.

Settings used for the tables: K = 8, horizon = the whole fight (`--oracle-k 8 --oracle-h 150`). Cost: 17 scenarios x 20
fights in 82 s on 8 cores. With a 10 s horizon the oracle was NOT an upper bound (mage-dragon -4.4, double-heal -11.2).
At full horizon it is at or above IAUS on all 17 scenarios.

**What this oracle is not yet.** It is a one-step improvement over IAUS (the rollouts play IAUS), and it only uses the
actions the catalogue generates today. So the gaps below are a FLOOR. Open: an iterated oracle (rollouts over the
previous oracle), and the full action set (scrolls, staves, deliberate repositioning, block, bash, shouts).

Every decision where IAUS's own pick differs from the oracle's by 1 U or more is logged and clustered by
`IAUS family -> oracle family`, with the mean facts of both picks (foe health, on the player, caster, resist,
recipient health, distance). `--scenario <name> --oracle` prints the top five clusters per scenario.

### 9.3 Scenarios (`--scenario all|name,name`)

Each scenario's foes come from the list's own records by role (draugr, troll, frost atronach, dragon, bandit, archer,
healer, mage) and fall back to the fixtures. Health and damage are then scaled so the strength ratio lands on the
scenario's target: a scenario must be winnable by good play on every list.

| Scenario | Class | What a skilled player does | Primary metric |
|---|---|---|---|
| tank3 | warrior | the player tanks three draugr: kill one at a time | U, player damage taken |
| mage-dragon | evoker | range, the weak element, no waste while it flies | U |
| bandit-pack | archer | three bandits and an archer: the archer first if reachable | prio% |
| focus-healer | archer | kill the healer first | prio% |
| interrupt | warrior | stagger a caster before the release | intr% |
| weakness | evoker | troll (weak to fire) and frost atronach (immune to frost) | immune%, resisted%, weak% |
| predictive-heal | healer | a boss on the player: heal by what is coming | player dead %, player min health |
| peel | mercenary | two foes on a squishy player | press%, peel.s |
| overkill | evoker | five weak wolves: no big spell on a dying wolf | overkill% |
| double-heal | healer + healer | one heals, the other fights | dblheal% |
| mp-budget | evoker | four grunts, a boss at 25 s | mpAtLate% |
| splash | evoker | the player in the middle of four foes | splash hits per fight |
| control-setup | warrior + evoker | an ally paralyses: hit the helpless one | ctlFocus% |
| kite | thief | a brute he cannot trade with | damage taken per second |
| knight-boss | knight | a far stronger foe on the player, no healer | U, taunt-then-down % |
| knight-boss-healer | knight + healer | the same with a healer | U |
| hurt-knight | knight | knight at 30 percent, player at 20 with a foe on him | player dead % |

### 9.4 Baseline: IAUS on main 3e21e0c against the oracle (built-in set, 20 fights each)

| Scenario | U IAUS | U oracle | gap | what differs |
|---|---|---|---|---|
| hurt-knight | 20.5 | 54.2 | +36.8 | player dies 80 percent with IAUS, 35 with the oracle |
| knight-boss-healer | 71.8 | 87.8 | +11.9 | downs 0.75 vs 0.30, taunts 1.8 vs 4.3 per minute |
| kite | 5.9 | 16.7 | +11.7 | damage taken 15.2 vs 9.5 per second, retreat 7 vs 16 percent of the time |
| knight-boss | 66.1 | 75.0 | +10.6 | player dead 10 vs 0 percent |
| double-heal | 86.2 | 84.3 | +6.0 | double heals 21 vs 8 percent of heal casts |
| predictive-heal | 86.1 | 91.1 | +5.1 | |
| splash | 90.4 | 93.5 | +3.5 | fight 18 vs 13 s |
| control-setup | 88.3 | 91.8 | +3.5 | |
| mp-budget | 88.0 | 91.4 | +3.0 | magicka at the boss 66 vs 81 percent |
| tank3 | 89.5 | 91.6 | +2.7 | |
| peel | 89.9 | 92.0 | +2.6 | |
| weakness | 91.8 | 93.8 | +1.9 | |
| mage-dragon | 70.9 | 67.2 | +1.4 | |
| overkill | 95.6 | 97.3 | +1.3 | overkill 24 vs 5 percent of his damage |
| interrupt | 90.6 | 91.3 | +1.2 | interrupts 6 vs 11 percent of foe casts |
| focus-healer | 89.3 | 89.8 | +0.4 | healer killed first in 20 vs 83 percent of fights |
| bandit-pack | 90.9 | 90.3 | +0.2 | |
| MEAN | | | +6.1 | |

(U columns are from the K = 6, 30 s run. The gap column is the K = 8 full horizon run. Both are in the session
scratchpad as `sim2/sc_6_30.txt` and `sim2/sc_8_150.txt`. To be re-run and unified once the oracle is final.)

## 10. Enemy AI outside the records (LoreRim and Tuxborn)

`tools/list_balance/inventory_ai_mods.py --list <list> --profile <profile>` lists every enabled mod with an SKSE DLL or
behaviour files and writes `<list>.aimods.json`. LoreRim: 3961 enabled mods, 348 with a DLL or behaviour files, 108
match a combat hint. Tuxborn: 1371, 120, 21.

| List | Mod | What it changes (source) | In the sim |
|---|---|---|---|
| LoreRim | Aggro Management in Skyrim (`SmartNPCTargetSelector.dll`) | NPC target choice by a hate table. `SmartTargetingNPC.ini`: fBaseThreat 5, fSwitchThreshold 0.15, fHateHalfLife 30, fTauntBonus 1000, fExecutionerThreshold 0.30, fExecutionerBonus 500, fBashHateBonus 50, fSummonHateBonus 300, fKillTransferBonus 500, ranges 800 / 2000 / 3000 | NOT YET. Next: hate-based targeting as a per-list Brain parameter (values DATA, semantics MODEL from the key names) |
| LoreRim | Dynamic Follower Scaling (`FollowerScaling.dll`) | `ReductionPerFollower = 20`, `Mode = 1`. No documentation found in the mod folder. A Nexus comment describes the mod as reducing follower damage by a percentage | NOT modelled. Semantics unproven. It directly scales follower damage, so it matters for absolute numbers |
| LoreRim | Chocolate Poise | a poise meter replaces the stagger rule. `ChocolatePoise.ini`: StaggerMode 1, Health BaseMult 25, ArmorMult 0.02, RegenRate 0.05, Damage Melee 1.0, Bow 0.5, Creature 2.0, Unarmed 2.5, ToPC 0.8, ToNPC 1.2, WeightContribution 1.4 | NOT modelled (the formula is not in the INI) |
| LoreRim | Modern Stagger Lock | stagger magnitudes 0 / 0.5 / 0.75 / 1.0 | not modelled |
| LoreRim | SCAR 2 | NPC attack combos | not modelled (attack chains would raise hits per engagement) |
| LoreRim | Ultimate NPC Dodging | NPCs dodge. The INI is empty (defaults) | not modelled |
| LoreRim | Precision, Precision Creatures | weapon reach from collision | reach stays the record formula |
| LoreRim | Project Gap Close | NPC sprint attacks and gap closing | not modelled |
| LoreRim | Combat Pathing Revolution | sets fCombatFallbackChanceMin/Max 0 / 0.75, fCombatCircleChanceMin/Max 0.15 / 0.95 at runtime | not modelled (circling and fallback are not in the Brain) |
| LoreRim | Comprehensive Attack Rate Patch | attack speed formula fix | not modelled |
| LoreRim, Tuxborn | NPCs Learn to Aim | aim and release by skill: bow 1.0 / 1.0, crossbow 0.85 / 7.0, spell 1.15 / 0.85, staff 0.85 / 1.15 | not modelled (the sim's miss chance is a constant) |
| LoreRim | Ultimate Animated Potions NG | potions are animated | the 1.2 s drink is a MODEL value |
| LoreRim, Tuxborn | Spell Perk Item Distributor | 139 / 108 `_DISTR.ini` files hand spells, perks and items to NPCs at runtime | NOT modelled. Enemy kits in the sim are the records' only |
| Tuxborn | Blade and Blunt (`BladeAndBlunt.dll`) | bZeroAllWeaponStagger, bArmorRatingScalingEnabled, injuries at 10 / 25 / 50 percent | not modelled. Its record-level changes (game settings, perks) ARE read |

Honest summary: the record half of each list's enemy AI is in the sim (combat styles, AI data, stats, spells, perks
without conditions). The DLL half is inventoried and not yet modelled. The first one to model is LoreRim's hate-based
targeting, because it changes who the enemies attack, which is what taunt, peel and tank decisions are about.

## 11. Classes

The sim's class cells are the game's own 35 rows: `RealClasses.cpp` compiles `native/classes/ClassTable.cpp` on the
host. `--print-classes` prints the gate and kit of each. `--classes slug,slug` narrows a run. `--classes legacy` keeps
the eight hand-written v1 cells.

Still to do here: the "human expert playstyle" of each class from `class-build-plan.md`,
`class-walkthrough-decisions.md` and the Q1 to Q69 rulings, the planned classes as "planned" cells, and the per-class
gap rows.

First readings of the class table against the catalogue (to be confirmed, then asked):
- No hybrid class has a power attack (no power share, no power row), so "paralyse, then power attack" cannot be done
  by one follower. The control-setup scenario therefore pairs a warrior with an evoker.
- T1 Melee and T1 Ranged have no power attack, taunt or retreat with the tree on (no default rows).

## 12. GAP LEDGER

Status: open = evidence exists, no design yet. Priority by oracle gap.

| id | Class | Family | Missing input or behaviour | Evidence | Closing action, or proven limit | Status |
|---|---|---|---|---|---|---|
| G1 | knight line | taunt | no survivability term (effective health against that foe's damage, a healer in the party, potions) and no sacrifice term (the protected ally is about to die) | hurt-knight +36.8, knight-boss +10.6, knight-boss-healer +11.9 | new considerations from the I8 damage estimate | open |
| G2 | all | retreat, movement | he does not back off when outmatched | kite +11.7: damage taken 15.2 vs 9.5 per second | retreat scored by incoming damage against his health. Deliberate repositioning (section 13) | open |
| G3 | healers | heal | reactive and wasteful: 40 to 60 percent overheal in random fights (mage lines, natural bracket) | random brackets, predictive-heal +5.1 | heal need by predicted incoming damage and by the heal's size against the missing health | open |
| G4 | healers | heal | two followers heal the same target at once | double-heal: 21 vs 8 percent of heal casts | a party claim on (recipient, heal) visible to the other followers | open |
| G5 | casters | cast offense | no overkill term | overkill: 24 vs 5 percent of his damage wasted | expected damage against the target's remaining health | open |
| G6 | casters | all casts | no magicka budget across the fight | mp-budget: 66 vs 81 percent magicka when the boss arrives | reserve by the foes still to come and their strength | open |
| G7 | all | attack target | no value for killing the healer or the archer first | focus-healer: healer first in 20 vs 83 percent | a foe role term (heals others, ranged, caster) and time-to-kill ordering | open |
| G8 | mage lines | attack | melee share of body time 3 to 19 percent | BODY table, LoreRim natural | ruling: a T1 mage almost never melees | open |
| G9 | all | attack target | target switches 8 to 12 per minute | field logs: 2.1 pins per minute (p50) | target momentum | open, the sim's engine targeting may add to it |
| G10 | casters | cast control | a refused (spell, target) pair comes back every second lap and spends a walk try | D1, section 7 | `fix/iaus-d1-willwork` 0c70d81 | fix in review |
| G11 | casters | cast offense | area casts reach allies in 11 to 33 percent of area casts in random fights (v1 run before the I6 merge) | INTELLIGENCE table | to re-measure after I6's payload rule | to verify |

## 13. The full action set (road audit, to do)

A skilled player uses everything the follower can do. For each of these the ledger needs the existing road (an MFO
actuation, a Harbinger facet, an engine call MFO already makes), then a candidate family and its considerations, or a
design, or a proven limit: scrolls (scroll use is off in Generate, card S3), staves as their own action with charge
awareness (B656 / B662: staff charge has never been observed), deliberate repositioning (step back, kite, flank, get
line of sight, leave a blast or make one splash-safe, body-block for the player, retreat to the healer, close to melee
on purpose), shouts, powers, dual casting, blocking, bashing, sprint, sneak, potions beyond the three drinks. The oracle
must search the same full set.

## 14. Questions for marth (not guessed)

- Dynamic Follower Scaling in LoreRim (`ReductionPerFollower = 20`, `Mode = 1`): what does it scale, and is it part of
  the intended follower balance on that list?
- More to come from the class playstyle pass.

## 15. Results file (for the dashboard, `tools/iaus_dash`)

Every run writes one JSON file: `--json <path>`, default `~/.cache/mfo-iaus-sim/runs/<run_id>.json` (cache only, never
committed). `--no-json` turns it off, `--label "<what changed>"` names the run. Writer: `tools/iaus_sim/Results.h`.

Schema 1 (stable: fields are added, never renamed):

```
{ "schema": 1, "run_id": "YYYYMMDD-HHMMSS-<short>", "time": "<ISO>", "branch": "...", "sha": "<git short sha>",
  "list": "Vanilla|Tuxborn|LoreRim|builtin", "seed": N, "fights_per_cell": N, "label": "...",
  "oracle": { "k": N, "horizon_s": N, "kind": "none | rollout over IAUS, catalogue actions | rollout over IAUS, extended action set" },
  "classes": [ { "class": "<ClassTable slug>", "tier": N, "line": "Melee|Ranged|Mage|Hybrid", "planned": false,
                 "bracket": "natural|balanced|weak|strong1|strong2|mixed-boss", "rank": "all", "fights": N,
                 "metrics": { ... }, "action_mix": { "<family>": percent of deciding time } } ],
  "scenarios": [ { "name": "...", "class": "...", "iaus_u": x, "oracle_u": x|null, "gap": x|null,
                   "metrics": { ... IAUS ... }, "oracle_metrics": { ... } } ],
  "rulings": [ { "id", "text", "class", "metric", "op", "target", "value", "pass" } ],     (natural bracket rows)
  "gaps": [ { "id": "G1", "family", "class", "desc", "scenario", "delta_u": x|null, "status": "open|closing|closed|limit" } ] }
```

Fixed metric keys (the sim's own names are written beside them): `idle_pct`, `idle_cand_pct`, `hold_pct`,
`hold_low_pct`, `switches_per_min`, `target_switch_per_min`, `heal_latency_s`, `healed_pct`, `overheal_pct`,
`double_heal_pct`, `overkill_pct`, `mp_at_boss_pct`, `melee_share_pct` (share of the time he is up that his body spends
in a melee swing), `taunt_per_min`, `power_per_min`, `follower_death_pct` (fights in which he went down),
`player_death_pct`, `ttk_s` (time to the first kill), `damage_taken_per_foe`, `bad_facts`, `invalid`,
`decide_us_mean`, `decide_us_max`. Added: `utility`, `win_pct`, `fight_s`, `splash_ally_hits_per_fight`.

Rulings checked today (`Results.h Rulings()`): R-MAGE-MELEE-T1 (`mage` melee_share_pct <= 5), R-MAGE-MELEE-T2+
(pure mage line, tier 2 and up, <= 3), R-NO-SPLASH (0 ally hits per fight), R-BAD-FACTS, R-INVALID, R-BUDGET
(decide_us_max <= 500). More are added with the class playstyles.

## 16. Phase 2: making IAUS itself smarter (work in progress on `feat/iaus-sim-v2`)

### 16.1 The success criterion ("humanlike"), stated so it can be measured

A policy is optimal when no single decision, changed and then followed by the policy itself, improves the outcome at
any state (the policy improvement theorem). The oracle of section 9.2 is exactly that test: it tries every option at
every decision and rolls IAUS forward. So:

1. **Oracle gap.** On every scenario and on the natural bracket of every class, on all three lists, the mean
   utility of IAUS is within the margin of the rollout oracle built over that same IAUS, with the FULL action set.
   Proposed margin: 3 U per scenario and 1.5 U in the mean. Reason: the oracle's own noise at K = 8 is about 1 to 2 U
   per scenario over 20 fights (seen as small negative gaps), and 3 U is a tenth of what one follower down costs.
2. **Bot tells**, each a metric in the results file, inside the range a person shows: decision switches per minute,
   target switches per minute (field logs: 2.1 pins per minute at the median), overkill, double heals, idle with a
   candidate in reach, refused tries, mage melee share against the ruling.
3. **Robustness.** A change is kept only when it is found on one list and holds on the others and on a second seed.

The oracle is rebuilt over every new IAUS, so the gap stays honest as IAUS improves.

### 16.2 What changed in the pure layers so far (all NOT APPLICABLE while their inputs are unmeasured, harness 0 failed)

| Step | Files | What |
|---|---|---|
| 2a estimates | `native/iaus/Estimate.h` (new, pure), `Snapshot.h`, `Core.h`, `Generate.cpp`, `Catalogue.cpp` | Per-combatant inputs (one weapon hit, the engine's hit rate, armor mitigation, magic resist, observed health loss) and per-spell expected damage / healing. New considerations: DamageValue (this spell's expected damage on this target against his best option), Overkill, TauntSurvive (seconds to live with the foe on him too, replaces the self-health term once measured), RetreatDanger (the old 30 percent gate, or seconds to live under 8), HealSize, BoostPlayerDying, BoostRecipientDying |
| 2b scrolls | `Snapshot::scrollRoad`, `Generate.cpp` | Scroll candidates (family Scroll, the catalogue's scroll sets) once a scroll can be read as a cast. Off in the game until card S3 has its road |
| 2c controlled, Q64 variant | `Core.h`, `Generate.cpp`, `Catalogue.cpp`, harness | BoostControlled x1.3 for a blow on a controlled foe. And a VARIANT behind `Tuning::attackChase` (off): a melee attack shaded by distance and vetoed only past the chase cap, instead of the 1.15 x reach gate. Ruling Q64 ("within precision reach") stands as the default, the F4 harness check is unchanged, the variant has its own checks. See 16.5 |

Engine side still to write (CI only): `Candidates.cpp` and `SpellBook.cpp` must fill the new Snapshot fields from the
reads the I8 research proved (weapon damage by the core formula, `weaponData.speed` x fCombatDPS*SpeedMult,
DamageResist x fArmorScalingFactor, kResistMagic, `Confidence::HpLossRate`, spell magnitude through entry point 0x1D).

### 16.3 Tuning by search, not by hand

`tools/iaus_sim/Tune.h`: `--set` overrides a weight, an action weight or a curve parameter at run time, `--opt` runs a
coordinate search on J = half the mean scenario utility plus half the mean natural-bracket utility.

Parameter set A (searched on LoreRim, 12 fights, 2 passes), indexes from `--list-params`:
`w.9.60=0.9975,w.9.61=0.931,w.2.62=1.995,c.65.b=1.8,w.11.64=0.8,c.66.b=0.7,aw.4=0.665,aw.1=2.394,aw.2=0.75,aw.10=1.8,aw.7=0.275,aw.9=1.769,aw.11=1.2,aw.0=0.8,c.63.a=0.665`
In words: power attack weight 0.9 to 2.4, control casts 0.9 to 1.8, offense casts 1.0 to 1.77, magicka potion 1.1 to
0.275, taunt 1.0 to 0.75, retreat 0.5 to 0.665, taunt survive weight 2.0.

| J (30 fights, seed 1) | LoreRim | Tuxborn | Vanilla |
|---|---|---|---|
| IAUS on main (no estimates) | 57.29 | 58.56 | 76.68 |
| + estimates v1, first-guess weights | 57.67 | 59.42 | 77.24 |
| + parameter set A | 61.83 | 61.86 | 78.59 |
| + scroll road | 61.81 | 62.39 | 78.67 |
| + controlled boost (reach gate as ruled, Q64) | 61.74 | 62.35 | 78.81 |

Set A was searched on LoreRim only and holds on Tuxborn, Vanilla and seed 7 (LoreRim 60.1, Tuxborn 61.5, Vanilla
78.7). A set searched on Tuxborn generalised worse and was dropped. Set A is NOT yet written into `Catalogue.cpp`.

Oracle gap over IAUS + set A (K = 6, 60 s, 20 fights): LoreRim mean +14.3 (from +20.1), Tuxborn +15.5 (from +19.5).
Largest left: knight-boss and knight-boss-healer on LoreRim (+65, +77: the oracle bashes and controls the boss, see
below), double-heal (+34, +43), control-setup (+26, +34), predictive-heal (+14, +19), and on Tuxborn mp-budget +24,
weakness +23, splash +21.

### 16.4 Open questions the lab raised

- **Bash and stagger on LoreRim.** The oracle beats the boss by bashing it (a 0.8 s stagger each time). LoreRim runs
  Chocolate Poise, where a poise meter replaces the stagger rule, so the sim overstates stun-locking on that list
  until poise is modelled. And bash has no road in IAUS today.
- **Magicka potions.** The search wants them far less on LoreRim (0.275) and more on Tuxborn (1.65). A weight cannot
  be right on both: the missing input is what the magicka would buy (the foes still standing against his spells).

### 16.5 Q64 variant: reach gate (ruled) against chase range (measured for marth)

Ruling Q64: a melee follower attacks "within precision reach". The sim has no Precision numbers yet, so the gate is
the vanilla reach (`Snapshot::meleeReach` 200, vetoed beyond 1.15 x). The variant lets a melee attack name a foe
anywhere inside the chase cap, on the argument that act.attack only names the target and the engine closes.

J with parameter set A and the scroll road, 30 fights:

| | LoreRim seed 1 | seed 7 | Tuxborn seed 1 | seed 7 | Vanilla seed 1 | seed 7 |
|---|---|---|---|---|---|---|
| reach gate (the ruling) | 61.74 | 60.65 | 62.35 | 61.89 | 78.81 | 78.80 |
| chase variant | 61.36 | 61.00 | 63.79 | 61.43 | 79.45 | 79.26 |
| difference | -0.38 | +0.35 | +1.44 | -0.46 | +0.64 | +0.46 |

Mean difference +0.34 J, inside the run-to-run noise (about 0.5). **The ruling costs nothing the lab can measure.**
The oracle cluster that prompted the variant ("idle -> attack" at 800 units) is real, but under the gate the
follower's own AI closes on its own target and the attack is named once he is in reach. The variant stays off.

## 17. Class combat packages (card C8), searched in the lab

A package is the combat style the DLL swaps on per stance (melee, ranged, cast), the ch.18 power-attack share and the
ch.23 pursuit leash. Today every class runs MFO's three stance styles (`MFO_GenerateESP.py make_csty`) with two class
offsets. The lab puts exactly those under every follower (`Setup.h MfoPackage`) and searches from there.

Method (`tools/iaus_sim/tune_packages.py`): TABLE mode first (IAUS off, the class's default gambit rows, which is what
most players get), a coordinate search on LoreRim, objective = 0.35 x the class's scenarios + 0.65 x its natural,
balanced and strong1 brackets. The found values are then scored on Tuxborn and Vanilla, in table mode AND in IAUS
mode, over two seeds. A package is kept only when it loses on no list in no mode (more than 0.75 U) and gains more
than 0.5 U in table mode on average. Files: `tools/iaus_sim/packages/<class>.json` (values, evidence per list and mode).

**Proof level: MODEL.** The CSTY fields move behaviour through the sim's engine-AI model (section 4 row 22: attack
pause divided by the offensive mult, power attack chance 0.12 x offensive mult x the share, block chance 0.20 x
defensive mult). The DIRECTION of a finding is what the lab stands behind. The numbers need the in-game probe
(section 5) before they become generator rows. Circle, flank, strafe and avoid-threat are not modelled, so the search
cannot speak about them.

| Class | Verdict | Changed from today | Mean table gain (U) | Per list, table / IAUS delta |
|---|---|---|---|---|
| arch-mage | today's | st.2.off 0.75, pk.leash 1000 | +0.15 | LoreRim -0.3 / -0.8, Tuxborn +1.0 / +0.4, Vanilla -0.3 / +0.0 |
| archer | today's | nothing | +0.00 | LoreRim +0.0 / +0.0, Tuxborn +0.0 / +0.0, Vanilla +0.0 / +0.0 |
| blademaster | KEEP | st.0.off 4, st.0.powStag 0.5, pk.power 1.33 | +3.49 | LoreRim +6.7 / +4.8, Tuxborn +1.5 / +2.9, Vanilla +2.3 / -0.5 |
| champion | today's | st.0.off 2, st.0.powStag 2, pk.power 1.33, pk.leash 600 | +1.07 | LoreRim +1.4 / +2.6, Tuxborn +1.3 / +3.5, Vanilla +0.5 / -1.0 |
| conjurer | today's | st.0.off 1.33, st.2.off 1.33 | -0.39 | LoreRim -0.2 / -0.2, Tuxborn -0.9 / +0.4, Vanilla -0.0 / -0.4 |
| dragonborn | today's | st.0.off 1.33 | +0.37 | LoreRim +1.2 / -0.1, Tuxborn -0.6 / +1.1, Vanilla +0.5 / -0.4 |
| ebony-warrior | KEEP | st.0.off 1.33, st.0.def 0.665 | +2.49 | LoreRim +1.7 / -0.4, Tuxborn +2.4 / +0.4, Vanilla +3.4 / +2.1 |
| evoker | today's | st.2.off 1.33 | +0.28 | LoreRim +0.8 / -0.3, Tuxborn +0.1 / +0.4, Vanilla -0.1 / +0.3 |
| grand-battlemage | today's | st.0.def 0.25 | -0.20 | LoreRim +0.0 / -0.0, Tuxborn -0.3 / +0.0, Vanilla -0.3 / -0.0 |
| great-knight | today's | st.0.off 4, st.0.bash 1.33, pk.power 1.33 | +3.30 | LoreRim +5.2 / +3.7, Tuxborn +3.8 / -1.4, Vanilla +0.9 / -0.9 |
| healer | today's | st.2.off 2.66 | +0.27 | LoreRim +0.8 / +0.2, Tuxborn +0.1 / -0.1, Vanilla -0.1 / -0.4 |
| jaqspur | KEEP | st.1.off 1.33 | +0.52 | LoreRim +0.7 / +0.6, Tuxborn -0.0 / +1.1, Vanilla +0.9 / -0.1 |
| knight | KEEP | st.0.off 2, st.0.powStag 1.33, pk.power 2.66 | +6.56 | LoreRim +12.2 / +10.7, Tuxborn +5.3 / +4.3, Vanilla +2.1 / +1.0 |
| listener | today's | st.0.off 2.66, st.0.powStag 1.33, pk.power 2 | +1.78 | LoreRim +4.5 / +2.7, Tuxborn +1.3 / -1.1, Vanilla -0.4 / +1.8 |
| living-legend | today's | st.0.off 2, st.0.powStag 2, pk.power 2.66, pk.leash 600 | +0.94 | LoreRim +5.0 / +4.3, Tuxborn -0.8 / +1.7, Vanilla -1.4 / +0.6 |
| mage | today's | st.2.off 0.5, st.2.def 0.25 | -0.60 | LoreRim -1.0 / -0.3, Tuxborn -1.2 / +0.8, Vanilla +0.3 / -0.2 |
| master-thief | today's | st.1.off 4 | +0.66 | LoreRim +0.9 / -0.7, Tuxborn +0.8 / -2.0, Vanilla +0.2 / +0.8 |
| melee | today's | st.0.off 1.33, st.0.def 0.25 | +0.69 | LoreRim +0.7 / +0.3, Tuxborn +1.2 / -0.8, Vanilla +0.1 / +0.1 |
| mercenary | KEEP | st.0.off 4, st.0.powStag 2, pk.power 2, pk.leash 1000 | +4.36 | LoreRim +2.0 / +5.6, Tuxborn +8.7 / +7.2, Vanilla +2.5 / +1.6 |
| necromancer | today's | st.0.off 1.33, st.2.off 0.5 | +0.40 | LoreRim +1.9 / -0.8, Tuxborn +0.1 / +0.1, Vanilla -0.7 / +0.4 |
| nightblade | KEEP | st.0.off 2 | +1.07 | LoreRim +1.3 / +1.7, Tuxborn +1.3 / +1.1, Vanilla +0.6 / +0.1 |
| nightingale | today's | nothing | +0.00 | LoreRim +0.0 / +0.0, Tuxborn +0.0 / +0.0, Vanilla +0.0 / +0.0 |
| priest | today's | st.2.off 0.75 | -0.50 | LoreRim -0.6 / -0.5, Tuxborn +0.2 / -0.1, Vanilla -1.1 / -0.1 |
| ranged | today's | nothing | +0.00 | LoreRim +0.0 / +0.0, Tuxborn +0.0 / +0.0, Vanilla +0.0 / +0.0 |
| ranger | today's | st.1.off 0.5 | -0.50 | LoreRim -0.3 / +0.0, Tuxborn +0.1 / +0.2, Vanilla -1.3 / -0.0 |
| saint | today's | st.2.off 0.5625 | -0.49 | LoreRim -0.8 / -0.0, Tuxborn -1.2 / +0.2, Vanilla +0.5 / +0.5 |
| scout | today's | st.1.off 0.75 | +0.35 | LoreRim +0.3 / -0.0, Tuxborn +0.5 / +1.2, Vanilla +0.2 / -0.4 |
| sorcerer | today's | st.0.off 1.33, st.0.def 0.25 | +0.80 | LoreRim +2.9 / -0.5, Tuxborn -1.0 / +0.1, Vanilla +0.5 / +1.0 |
| spellsword | today's | st.0.off 0.75, pk.leash 600 | -0.68 | LoreRim -0.1 / -1.8, Tuxborn -0.4 / -0.8, Vanilla -1.6 / -0.6 |
| templar | today's | st.0.off 0.75 | +0.49 | LoreRim +0.9 / -0.6, Tuxborn +0.6 / -0.2, Vanilla -0.0 / -0.9 |
| thief | today's | st.1.off 1.33 | +0.03 | LoreRim +0.5 / +0.3, Tuxborn +0.0 / +0.0, Vanilla -0.4 / -0.1 |
| warlock | today's | st.2.off 1.33 | +0.44 | LoreRim +0.4 / -0.1, Tuxborn +0.9 / -0.0, Vanilla +0.0 / +0.1 |
| warrior | KEEP | st.0.off 4, st.0.powStag 2, pk.power 2 | +3.20 | LoreRim +7.7 / +5.8, Tuxborn +0.9 / +2.4, Vanilla +1.0 / +1.7 |
| witchhunter | today's | st.1.off 0.5 | +0.16 | LoreRim -0.7 / -0.3, Tuxborn +1.2 / +1.9, Vanilla -0.0 / +1.3 |
| wizard | today's | st.2.off 2.66 | +0.37 | LoreRim +0.9 / +0.1, Tuxborn +0.2 / -0.4, Vanilla +0.0 / -1.0 |

Reading: 7 of 35 classes have a package that holds on all three lists. They are the melee lines, and the direction
is the same every time: a higher offensive mult (shorter pauses between attacks, more power attacks) and a larger
power-attack share. No ranged or mage class found a robust gain: their stance styles are already near the best the
modelled fields can do. Parameter names: `st.<0 melee, 1 ranged, 2 cast>.<off, def, fallback, avoid, powBlock, powStag,
bash>`, `pk.power` (ch.18 share scale), `pk.leash` (units from the player, 0 = none).

For the C8 author: take the KEEP rows as the direction per class, start from the listed values, and confirm with the
hit and power-attack probe lines before fixing the generator rows.

## 18. Default gambit rows (card C12), searched with the package

Method (`tools/iaus_sim/tune_defaults.py`, `sim --opt-rows`): table mode, the class's kept package under it, a
hill-climb over row sets on LoreRim: reorder, retune a threshold, drop, add or replace a row. The rows on offer are
only what the board lets that class pick (vocabulary opcodes, gated by the class gate and its weapon set, ruling
Q59). Rules the search keeps: every kind of action in today's rows stays (a knight keeps his taunt), power-attack
rows do not multiply, the row count stays within today's. Validation as for the packages. Files:
`tools/iaus_sim/defaults/<class>.json` (today's rows, the lab's rows in board terms, evidence).

| Class | Verdict | Mean gain (U) | Per list delta | Lab rows (condition > gambit) |
|---|---|---|---|---|
| arch-mage | today's | -0.57 | LoreRim +1.4, Tuxborn -1.0, Vanilla -2.1 | (unchanged) |
| archer | today's | +0.00 | LoreRim +0.0, Tuxborn +0.0, Vanilla +0.0 | (unchanged) |
| blademaster | KEEP | +2.66 | LoreRim +3.8, Tuxborn +2.3, Vanilla +1.9 | foe_attacking_player > attack; foe_attacking_me > attack; foe_lowest_hp > attack; self_sp_pct_below 0.25 > drink_stamina_potion; foe_any > equip_melee |
| champion | KEEP | +2.86 | LoreRim +2.4, Tuxborn +2.4, Vanilla +3.8 | foe_attacking_player > attack; foe_any > power_attack; foe_highest_hp > attack; self_sp_pct_below 0.25 > drink_stamina_potion; foe_any > equip_melee |
| conjurer | KEEP | +2.19 | LoreRim +1.3, Tuxborn +4.3, Vanilla +0.9 | foe_count_at_least 3 > cast_target [Fear]; foe_any > cast_target [Attack]; self_mp_pct_below 0.25 > drink_magicka_potion; foe_within_range 150 > equip_melee; foe_stronger_than_me > attack |
| dragonborn | today's | +0.31 | LoreRim +0.1, Tuxborn +0.5, Vanilla +0.4 | (unchanged) |
| ebony-warrior | today's | +0.00 | LoreRim +0.0, Tuxborn +0.0, Vanilla +0.0 | (unchanged) |
| evoker | today's | +0.41 | LoreRim +1.4, Tuxborn -0.6, Vanilla +0.4 | (unchanged) |
| grand-battlemage | KEEP | +1.68 | LoreRim +1.9, Tuxborn +0.5, Vanilla +2.7 | foe_beyond_range 600 > cast_target [Destruction]; foe_weak_fire > cast_target [Fire]; foe_within_range 400 > attack; foe_any > equip_melee; foe_highest_hp > cast_target [Attack] |
| great-knight | KEEP | +1.47 | LoreRim +3.1, Tuxborn +1.6, Vanilla -0.3 | foe_attacking_player > power_attack; foe_attacking_player > taunt; foe_attacking_me > attack; self_hp_pct_below 0.35 > drink_health_potion; foe_any > equip_melee |
| healer | KEEP | +1.12 | LoreRim +1.7, Tuxborn +0.9, Vanilla +0.8 | player_hp_pct_below 0.5 > cast_player [Heal]; ally_hp_pct_below 0.5 > cast_target [Heal]; self_hp_pct_below 0.5 > cast_self [Heal]; foe_attacking_player > cast_target [Attack]; self_mp_pct_below 0.266 > drink_magicka_potion |
| jaqspur | today's | -0.87 | LoreRim +1.2, Tuxborn -1.9, Vanilla -1.9 | (unchanged) |
| knight | today's | +1.98 | LoreRim +8.1, Tuxborn -1.3, Vanilla -0.8 | (unchanged) |
| listener | KEEP | +3.53 | LoreRim +7.9, Tuxborn +1.3, Vanilla +1.4 | foe_any > power_attack; foe_hp_pct_below 0.3 > attack; foe_weaker_than_me > attack; foe_count_at_least 3 > cast_target [Fear]; foe_any > equip_melee |
| living-legend | KEEP | +2.56 | LoreRim +2.3, Tuxborn +3.5, Vanilla +1.9 | foe_is_caster > power_attack; foe_is_caster > attack; foe_weaker_than_me > attack; foe_highest_level > attack; foe_any > equip_melee |
| mage | KEEP | +2.35 | LoreRim +2.4, Tuxborn +0.3, Vanilla +4.3 | foe_is_caster > cast_target [Attack]; self_hp_pct_below 0.3 > drink_health_potion; foe_count_at_least 3 > cast_target [Fear]; foe_lowest_hp > attack; always > wait |
| master-thief | today's | -0.36 | LoreRim +0.9, Tuxborn -0.8, Vanilla -1.1 | (unchanged) |
| melee | KEEP | +1.46 | LoreRim +4.0, Tuxborn +0.2, Vanilla +0.1 | self_hp_pct_below 0.3 > drink_health_potion; foe_weaker_than_me > attack; foe_lowest_hp > attack; always > wait; foe_highest_level > attack |
| mercenary | KEEP | +0.87 | LoreRim +3.5, Tuxborn -0.4, Vanilla -0.5 | foe_is_ranged > attack; foe_stronger_than_me > power_attack; self_sp_pct_below 0.25 > drink_stamina_potion; foe_any > equip_melee; self_hp_pct_below 0.5 > drink_health_potion |
| necromancer | today's | +1.69 | LoreRim +4.5, Tuxborn -1.2, Vanilla +1.8 | (unchanged) |
| nightblade | today's | -0.03 | LoreRim -0.5, Tuxborn +1.0, Vanilla -0.6 | (unchanged) |
| nightingale | KEEP | +1.64 | LoreRim +1.9, Tuxborn -0.6, Vanilla +3.6 | foe_is_ranged > attack; foe_attacking_player > attack; foe_is_caster > attack; foe_fleeing > equip_melee; foe_any > equip_ranged |
| priest | KEEP | +1.84 | LoreRim +2.4, Tuxborn +0.9, Vanilla +2.2 | player_hp_pct_below 0.5 > cast_player [Heal]; ally_hp_pct_below 0.5 > cast_target [Heal]; foe_is_unholy > cast_target [TurnUndead]; self_hp_pct_below 0.5 > cast_self [Heal]; foe_lowest_hp > cast_target [Attack]; self_mp_pct_below 0.2 > drink_magicka_potion |
| ranged | today's | +0.15 | LoreRim +1.4, Tuxborn +1.8, Vanilla -2.9 | (unchanged) |
| ranger | KEEP | +0.94 | LoreRim +1.2, Tuxborn +1.0, Vanilla +0.7 | foe_attacking_ally > attack; foe_is_caster > attack; foe_attacking_player > attack; self_out_of_arrows 1 > loot_arrows; foe_any > equip_ranged |
| saint | today's | -0.02 | LoreRim +2.4, Tuxborn +2.7, Vanilla -5.1 | (unchanged) |
| scout | KEEP | +1.20 | LoreRim +0.4, Tuxborn +0.6, Vanilla +2.6 | ally_hp_pct_below 0.7 > cast_target [Heal]; player_hp_pct_below 0.7 > cast_player [Heal]; foe_is_caster > attack; foe_any > equip_ranged; foe_highest_hp > cast_target [Attack] |
| sorcerer | KEEP | +1.67 | LoreRim +2.0, Tuxborn -0.4, Vanilla +3.4 | always > cast_self [BoundWeapon]; foe_any > attack; foe_attacking_player > cast_target [Attack] |
| spellsword | KEEP | +1.03 | LoreRim +1.0, Tuxborn +1.1, Vanilla +0.9 | foe_beyond_range 400 > cast_target [Destruction]; foe_weak_fire > cast_target [Fire]; foe_is_caster > attack; foe_any > equip_melee; foe_highest_hp > cast_target [Attack] |
| templar | today's | +0.39 | LoreRim +1.7, Tuxborn -0.8, Vanilla +0.2 | (unchanged) |
| thief | today's | +0.57 | LoreRim +1.2, Tuxborn +0.8, Vanilla -0.3 | (unchanged) |
| warlock | today's | -0.07 | LoreRim +1.5, Tuxborn -1.9, Vanilla +0.2 | (unchanged) |
| warrior | KEEP | +3.67 | LoreRim +2.2, Tuxborn +3.8, Vanilla +5.0 | foe_blocking > power_attack; foe_attacking_player > attack; self_hp_pct_below 0.3 > drink_health_potion; self_sp_pct_below 0.3325 > drink_stamina_potion; foe_any > equip_melee |
| witchhunter | KEEP | +0.88 | LoreRim +1.9, Tuxborn +1.2, Vanilla -0.5 | foe_attacking_player > attack; foe_weak_fire > cast_target [Fire]; foe_lowest_hp > attack; foe_stronger_than_me > cast_target [Paralyze]; foe_any > equip_ranged |
| wizard | KEEP | +1.19 | LoreRim +1.5, Tuxborn -0.2, Vanilla +2.2 | foe_lowest_hp > cast_target [Attack]; foe_stronger_than_me > cast_target [Paralyze]; foe_weak_fire > cast_target [Fire]; foe_any > cast_target [Destruction]; self_mp_pct_below 0.2 > drink_magicka_potion |

Questions for marth that the search raised (also in section 14):
- **Is the power-attack cooldown per row or per follower?** Each default row carries its own 10 s cooldown. An
  unconstrained search stacked three power-attack rows and got three power attacks per 10 s, which goes round
  ruling Q7. The lab now forbids more power rows than today's set has.
- **Rank I has 3 combat slots, the class default sets have 4 or 5 rows** (`State.h kCombatSlotsByRank` = 3, 5, 7,
  9, 12). What happens to the fourth and fifth default row of a Rank I follower?

## 19. Headline: table, IAUS on main, IAUS+ phase 2, the oracle

J = half the mean scenario utility plus half the mean natural-bracket utility. 24 fights per scenario and 12 per class
for the three policies. The oracle row is the rollout oracle (K = 6, 60 s, extended action set) planning over IAUS+
on the first 8 fights per scenario and 4 per class of the same seeds. Every value is also in the results files
(label HEADLINE, fields `headline.j`, `headline.oracle_j`).

| J | Vanilla | Tuxborn | LoreRim |
|---|---|---|---|
| table mode (IAUS off, default gambit rows, today's packages) | 75.5 | 59.1 | 59.2 |
| IAUS as on main (no estimates) | 75.9 | 58.5 | 57.1 |
| IAUS+ phase 2 (estimates, parameter set A, scroll road, heal claim) | 78.5 | 62.5 | 60.9 |
| oracle over IAUS+ | 84.9 | 71.5 | 71.4 |

Reading: IAUS as it is on main does not beat the gambit table on Tuxborn and is 2 J behind it on LoreRim. Phase 2 so
far is 1.7 to 3.4 J ahead of the table on all three lists. The oracle is 6 to 10 J ahead of phase 2: that is the
distance left, and part of it needs actions IAUS does not have yet (bash, block, deliberate repositioning).

## 20. More questions for marth

- **Taunt success formula (ruling Q69).** Today the executor rolls `0.5 + 0.05 x (taunter level - foe level)`, kept in
  0.1 to 0.9, once per candidate foe (`Taunt.cpp:219`). It is stat-based and readable before the choice. IAUS now
  reads exactly that rule as a fact (`Estimate::TauntChance`). Recommendation: keep it, and make the executor call the
  same function. If more than level should count, the engine-readable candidates are the foe's AIDT confidence and
  the taunter's block or armor skill. The lab can score a variant once one is named.
- The two default-row questions of section 18 (power-attack cooldown per row or per follower; Rank I slots).
