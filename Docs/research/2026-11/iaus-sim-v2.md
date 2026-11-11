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
                      [--baseline out] [--compare base] [--taunt-visible] [--no-engine-casts] [--g10-sticky]
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
| 6 | Weapon damage | (weapon + ammo) x fDamageWeaponMult x lerp(fDamageSkillMin, fDamageSkillMax, skill / 100) x ModAttackDamage perks. Power attack x (1 + fPowerAttackDefaultBonus) | I8 disassembly for the chain, GMST values per list. The lerp shape is the I8 agent's reading | PROVEN chain, UNPROVEN that it is exactly linear |
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
| 29 | Taunt | one attempt, 70 percent, pins the foe 10 s | v1 values, not checked against Taunt.cpp yet | MODEL |
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

- **D1, G10 answers are forgotten every second lap (`native/iaus/Candidates.cpp:235`, `Generate.cpp:348`).**
  `g_wants` holds only the pairs of the last decision's `ranked` list. A pair answered No is vetoed by the WillWork
  gate, so it is not ranked, so it is not asked on the next lap, so it has no note, so it is Unknown, and Unknown
  passes. The pair is a live candidate again every other lap. Evidence: Tuxborn, 20 fights per cell. Failed control
  casts with the code as it is: mage natural 26.7 percent, mixed-boss 35.3, weak 60.0. With the answers remembered
  (`--g10-sticky`): 15.4, 26.7, 25.0. The rest is the first ask, where Unknown passes once by design.
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

## 9. Intelligence scenarios, the oracle, the gap ledger

To be filled as phase 1 completes: the named scenarios with their scored metrics, the expert oracle and the
"humanlike" criterion, every class from the real class table, and the GAP LEDGER (id, class, family, missing input or
behaviour, evidence, closing action or proven limit, status).
