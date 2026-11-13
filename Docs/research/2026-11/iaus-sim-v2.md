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
                      [--seeds a,b,c] [--opt p,q [--opt-passes N] [--opt-fights N] [--opt-class slug]]
python3 -I tools/iaus_sim/calib_logs.py        # real fight shapes out of _evidence/*/MFO.log
```

Tuning flags: `--classes slug,slug` also scopes the headline J and the `--opt` objective to those classes (the HEADLINE
line then ends `covers=...`; `--opt-class` still wins if both are given). `--opt-fights N` sets the fights per scenario
of each search evaluation (default `--fights`). `--seeds a,b,c` runs the headline J once per seed and prints each, then
the mean and standard error (no full tables); with `--opt` the objective is the mean over those seeds. Seed noise is
about +-8 J at 60 fights, so a small keep rule needs several seeds. Single `--seed` runs are unchanged.
The headline runs `--fights N` per scenario when `--fights` is given and 24 otherwise, and the HEADLINE line and the results
JSON (`headline.covers`, `headline.fights`) record both; an unknown class slug in `--classes` / `--opt-class` is an error,
`--opt-rows` and `--opt-prefix` honour `--seeds` and `--opt-fights`, and the printed SE is the between-seed SE of absolute J
(n=1 prints `se=n/a`), looser than the paired-difference noise a keep decision needs. `--classes X` (0.5 scenarios / 0.5
natural bracket only) and `--opt-class X` (0.35 / 0.65 over three brackets) are different objectives. A scoped run replays
the all-class scenario fights (the scenario seed uses the original scenario index). `SIM_CLITEST=1 tools/iaus_sim/run.sh`
runs the command-line assertions.

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

Status: open = evidence exists, no design yet. Priority by oracle gap. Later rows: 24.7 (G12 to G17) and 30.8 (G18 to
G22, the stealth and disengage lab).

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

Added 2026-11-11 (section 30): top-level `"stealth": "off|g1|hide|g3"` and `"sneak_profile"`, a `"headline"` object
(`covers`, `fights`, `j`, `j_scenarios`, `j_natural`, `oracle_j` ..., `oracle_fights`, `oracle_over`, `oracle_covers`,
with `covers` and `fights` null when no J was computed), and the G3 / G2 metrics listed in 30.9.

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

## 21. Combat-rule mods in the lab: Chocolate Poise and Dynamic Follower Scaling (successor 1, 2026-11-10)

Both are SKSE plugins that change the fight's rules outside the records, so `extract_world.py` cannot see them.
`tools/list_balance/extract_combat_mods.py --list <list> --profile <profile>` reads their settings from the list
(read-only, the winning file by mod priority) into `~/.cache/mfo-list-balance/<list>.combatmods.json`. The sim loads
that file beside the `.sim.json` (`Load.h LoadCombatMods`). A list without the DLL gets the vanilla model.

| List | Chocolate Poise | Dynamic Follower Scaling |
|---|---|---|
| LoreRim (Ultra) | on. INI from "LoreRim - MCM and INI Settings", JSON from "Chocolate Poise Plus" | on. Mode 1, 20 percent |
| Tuxborn (Deck) | absent | absent |
| Vanilla | absent | absent |

### 21.1 Chocolate Poise

Source of the formula: the mod's own code (`doodlum/skyrim-poise`, `src/Hooks/PoiseAV.cpp` and `HitEventHandler.cpp`),
read for the rules only. Proof level: SOURCE for the formula, MODEL for the three values marked below.

- Poise meter of an actor = (the race's multiplier in `ChocolatePoise.json`, else the race's mass) x BaseMult + worn
  armor weight x ArmorMult. LoreRim: BaseMult 25, ArmorMult 0.02. A human has 25, a troll 50, a giant 137, a dragon 175.
  The sim has no armor weights, so it uses 6 per worn piece (MODEL, under 1 point).
- Poise damage of one weapon hit = lerp(the type's base in the JSON, the weapon's weight, WeightContribution) x
  MeleeMult or BowMult. LoreRim sets every type base to 0 and WeightContribution 1.4, so it is 1.4 x weapon weight
  (bows x 0.5). A creature without a weapon deals physical damage x CreatureMult (2.0). A bash deals the shield's
  value x BashMult (1.0, shield weight 10 is a MODEL).
- Then x (1 + the attack's stagger offset from the race's attack data). The usual power attack has 1.0, a bash 0.5
  (MODEL: the modal record values, the sim does not carry them per attack). Then x (1 - blocked share), x total
  damage over physical damage, x min(1 - the target's armor mitigation, 0.8). Poise damage to an NPC x 1.2, to the
  player x 0.8.
- Spells: health damage x 0.2 (the JSON's "Magic Effects" factor).
- When the meter reaches 0 the actor staggers once and the meter refills. While staggered it takes no poise damage
  (StaggerMode 1). The meter regenerates 5 percent of its maximum per second. The vanilla stagger on a hit is set
  to 0 by the mod, so a power attack no longer staggers by itself. Stagger length: 0.5 s plus 1.0 s x the blow's
  share of the meter (MODEL).

What it changes in the lab: the oracle can no longer hold a boss by bashing it every 0.9 s, which is what made the
knight-boss gaps of section 16.3 (+65, +77) untrusted. Tuxborn's Blade and Blunt (bZeroAllWeaponStagger) is still not
modelled: its power-attack stagger stays the vanilla MODEL.

### 21.2 Dynamic Follower Scaling (ruling Q75: "make sure")

What it scales: `FollowerScaling.dll` 1.1.0 hooks the hit event (its own symbol
`HitEventHook::ProcessHitEvent(TESObjectREFR*, HitData*)`). When the aggressor is an actor in CurrentFollowerFaction
(FormID 0x0005C84E, the immediate at DLL address 0x1800295fa followed by the faction test), it counts the LOADED
actors in that faction and scales the hit:

- Mode 1 (LoreRim): damage x (1 - ReductionPerFollower)^(followers - 1). With 20 percent: 2 followers 0.80 each,
  3 followers 0.64, 4 followers 0.51, 5 followers 0.41.
- Mode 2: damage / followers.

It works on HitData, so on weapon hits (melee and arrows). Damage a spell deals through its magic effects is not in
HitData's damage. So spells are not scaled as far as the code shows. That part is unproven in the field.

Does MFO override it: **no**. `native/` holds no reference to CurrentFollowerFaction or 0x5C84E, no HitData or
ProcessHit hook, and no damage scaling of its own. A follower recruited the normal way is in that faction, so the
scaling applies to MFO followers. This is a finding for marth (his expectation was that MFO already overrides it).

Sim model: a follower's weapon hit x the mode's multiplier with n = followers alive. LoreRim only.

J (set A, scroll road, 30 fights) with both mods modelled: LoreRim 60.90 / 60.70 (seeds 1 / 7), before 61.46 / 61.37.
Tuxborn 63.03 / 61.44 and Vanilla 78.68 / 78.53 do not move (no such mod).

What this means for IAUS on LoreRim: with three followers a weapon hit is worth 0.64 of what the damage estimate
says, and a spell is worth all of it. The estimate (`Estimate::Dps`) cannot read the hook. It matters only where a
follower weighs his own weapon against his own spell, which IAUS does not do with numbers yet.

## 22. Rulings of 2026-11-11 (Q68, Q72 to Q75) and what the lab did with each

| Ruling | Text (short) | In the lab |
|---|---|---|
| Q72 | Taunt success keeps the level rule: clamp(0.5 + 0.05 x (taunter level - foe level), 0.1, 0.9). No extra stats | `Estimate::TauntChance` is that rule already. Nothing to change |
| Q73 | Power-attack cooldown is PER FOLLOWER, one cooldown shared by every row and by IAUS, like the taunt cooldown (Q69) | Table mode: every power-attack row of a follower shares one timer (`Table.h`, also the taunt rows). IAUS already keys the cooldown by (family, form), which is per follower for a power attack. The native table side belongs to the taunt and cooldown card |
| Q74 | Rank I has 3 combat slots. The follower gets the TOP rows in order, the rest join as slots unlock. Every prefix must be a good table | The sim cuts a follower's rows to his rank's slots (3 / 5 / 7 / 9 / 12, `native/State.h` kCombatSlotsByRank). `--opt-prefix` and `tune_prefix.py` order each class's set, see 22.1 |
| Q75 | Dynamic Follower Scaling: research it, make sure about MFO | Section 21.2 |
| Q68 | The swing-gate reach is different for every weapon and is LEARNED from hits and misses | `native/iaus/Reach.h`, see 22.2 |

### 22.1 Q74: the order that makes every prefix a good table

Method: table mode, each class's recommended set (the lab set of section 18 where its verdict was keep, else
today's rows). Every permutation is scored on LoreRim as the mean of J over the prefixes 3 .. n (40 fights). The best
order is then scored against the starting order per prefix on LoreRim, Tuxborn and Vanilla with seeds 1 and 7. Kept
only when no list loses more than 0.75 J and the mean gains more than 0.5 J. Only the ORDER changes, never the rows.

With today's slot table the 3-row prefix is what a Rank I follower holds and Rank II already holds all five. The
4-row prefix is scored too, as ruled.

| Class | Mean gain (J) | New order (rows by their action) |
|---|---|---|
| listener | +2.84 | power attack, attack foe under 30 percent, equip melee, fear cast at 3 foes, attack weaker foe. Today the equip row is last, so a Rank I Listener never draws |
| warlock | +2.46 | fear on a stronger foe, magicka potion, destruction on any foe, paralyze on a stronger foe, armor spell. Today the damage row is fifth, so a Rank I Warlock has no damage row |
| witchhunter | +1.20 | fire on a fire-weak foe, attack lowest health, paralyze a stronger foe, attack the player's attacker, equip ranged |
| scout | +0.90 | heal ally, attack casters, attack cast on highest health, heal player, equip ranged |
| spellsword | +0.78 | destruction beyond 400, fire on fire-weak, equip melee, attack cast on highest health, attack casters |
| grand-battlemage | +0.73 | fire on fire-weak, attack within 400, attack cast on highest health, destruction beyond 600, equip melee |

The other classes with more than 3 rows show no robust gain from a new order (between -0.9 and +0.4 J, inside the
noise): today's order already makes good prefixes. The full evidence per class and per prefix is in
`tools/iaus_sim/defaults/<class>.json`, block `prefix`. The pattern in the six: a class whose only damage or draw
row sits in slot 4 or 5 is a broken table at Rank I.

Caveat (unchanged from section 18): table mode acts through the sim's engine-AI MODEL. The direction is the
finding. The numbers need the in-game check.

### 22.2 Q68: the learned swing reach (`native/iaus/Reach.h`, pure, new file)

marth: "precision reach will be different for every single weapon. so find one formula won't work." and "In game I
would measure by whether it hits or not. A miss that should have hit (based on distance alone) lowers the distance,
hits distance is also tracked."

**What it is.** One number per (follower, weapon form, swing kind). It starts at the best estimate there is:
Precision's own for that weapon when the bridge answers (GetAttackCollisionCapsuleLength, then Precision's
conversion with fAIWeaponReachOffset read from its INI, k and r from its docs or measured), else the vanilla
fMeleeReach. Then every committed swing is one outcome: the distance at the swing's start, and HIT or MISS.

**The estimator.** It keeps the last 12 outcomes and the present value r. After each outcome the new value is the
distance of the longest hit the evidence supports: over every split point it counts the misses inside the split
(weight 1.5, a whiff wastes the swing, its stamina and its cooldown) and the hits beyond it (weight 1), takes the
split with the lowest count (on a tie the one nearest r), and r becomes the longest hit under that split. The
present r votes as one hit, which makes the value a memory and not only a window. With no hit under the split r
goes to 0.95 x the shortest miss. One outcome moves r by 10 percent of itself at most. r stays inside 0.5 to 2.0 x
its start value.

- A hit beyond r raises it. A hit inside r changes nothing.
- A miss inside r lowers it toward that distance, unless hits recorded beyond that distance outvote it (a target
  that stepped aside is one stray miss).
- A miss beyond r changes nothing. It was not expected to hit.
- A miss never raises the value.

**No expiry** (principle 9). Nothing reads a clock. The window is counted in swings (12 is about 15 s of melee at
the measured 1.2 s hit cadence), so a follower who has not swung for a minute knows what he knew.

**What the gate reads.** `Learner::Gate()` = r x 1.05 / 1.15. The range curve vetoes above 1.15 x its reach, so he
still tries 5 percent past what he knows. That is how r can grow from his own swings. The engine's own swings are
outcomes too and they are not gated at all.

**Keying.** Follower and weapon form: yes (scale and moveset differ per follower, the state then lives with the
follower in the worker domain and needs no lock). Swing kind (plain swing, power attack): yes, a power attack
lunges. The power attack reads its own learner once it has 3 outcomes, before that the plain swing's. Race: no, the
follower key covers it. Precision on or off: no, it is fixed for the session and the state is runtime only.
The TARGET's size is the open covariate: a distance to a giant's centre and to a wolf's centre are not the same
reach. Proposal: the engine side records the distance minus the target's bound radius if the probe shows the hit
distances differ by target race. To decide from the probe, not now.

**Proof (harness, 13 new checks, `tools/iaus_harness/run.sh`).** A toy world with a true reach the learner never
sees, 5 percent distance noise, 5 percent of swings missing for other reasons, 7 in 10 swings the engine's own:

| | Result |
|---|---|
| 72 runs (true reach 150 / 240 / 330, start 0.6 / 1.0 / 1.6 x, 8 seeds), value after 480 swings | within 8.7 percent of the true reach in the worst run, 4.5 percent in the mean |
| Gated swings that miss by distance | 6.4 percent mean, 10.8 percent worst |
| Swings that would have hit and the gate still allows | 98.3 percent worst |
| Without noise, start 1.5 x | settles on the true reach (0.1 percent), spread 1.3 percent, no oscillation |
| Heavy noise (10 percent, 15 percent stray misses), reported only | end error 19.9 percent, whiffs 6.6 percent, coverage 97.8 percent |

**In the sim.** Each follower's weapon has its true reach (fCombatDistance x the weapon's reach, plus the body
radii, x 1.2 at the hit frame) and the target moves during the swing, which is the noise. `--reach off|cold|warm`:
off is the constant 200 as on main, cold starts every fight at 200, warm (default) first replays 20 earlier swings.
Effect on J: none that can be measured (LoreRim 60.97 / 60.69 warm against 60.90 / 60.70 off, the others the same to
0.07). The reason: the constant gate whiffs only about 1 percent of power attacks in the sim (new metric
`powWhiff%`), because the sim's true reaches (191 to 284) sit near 200 x 1.15. The sim has no Precision. So the
learner is proven as an estimator, and its value in play is a field question.

**Engine side (not written, tier A: it crosses threads).** MFO already sinks both events as passive probes:
TESHitEvent (`Diagnostics.cpp:129`, registered at `:1354`) and the attack animation events `weaponSwing` /
`HitFrame` / `attackStart` (`Diagnostics.cpp:418-422`, the sink at `:544`). The plan:
1. Probe first (principle 5): one rate-limited line per committed swing of a managed follower with the weapon form,
   the distance at the swing event, and whether a hit event from that attacker with that weapon on the intended
   target arrived inside the swing window. Size the window from the logged gap between the swing event and the hit.
2. A miss counts only "by distance alone": the intended target alive, not blocking the path question of line of
   sight (engine sight Visible), and no hit on ANOTHER actor in the window (a swing that struck someone else is not a
   reach miss).
3. Outcomes go to the worker through the existing posted-snapshot road, never a lock. `Kit` gains the learned reach
   per drawn weapon and `Generate` reads `Kit` instead of `Snapshot::meleeReach` for the power attack's reach.
4. Persistence: runtime only. Saving it would be a co-save change and is a question for marth (section 23).

## 23. Questions for marth (successor 1)

- **Dynamic Follower Scaling (Q76, RULED 2026-11-11).** marth: "Then it's relevant in LoreRim and we factor it in", and then "I just mean it's factored in the sim." So MFO does not neutralise it, nothing in native detects or special-cases it, and the lab models it per list (section 21.2). If the hit-against-estimate probe later shows a gap in the field, that is a new finding for marth then.
- **Learned reach across sessions.** Runtime only today. The learner converges in about 10 to 20 swings from a bad
  start, so a session relearns each weapon in its first fight. Saving it means a co-save change. Recommendation: do
  not save it.
- **Token ledger** (fixed by the coordinator at 22:40): the script summed a message once per content block. Counted once per message the two authors had spent 1.87M, so about 8.13M of the 10M was left.

## 24. Successor 1, round 2: two fidelity bugs, set A2, and five new inputs (2026-11-10)

### 24.1 Two bugs in the lab that every earlier number carried

**Spell reach.** The sim took a spell record's range as its reach whenever it was above 0. The game does not:
`Actuation::AimedReachOf` (`cast/CanAct.cpp:51-71`) reads the record's range only from 300 units up, else the
projectile's range, else no cap. 587 of LoreRim's 4773 spells, 557 of Tuxborn's 4456 and 51 of Vanilla's 677 carry a
small range (Requiem's target heals carry 30). In the sim those spells could only be cast from inside 30 units, so
on LoreRim a heal on another actor was never a candidate at all. Fixed in `Data.h Classify`. Every heal, double-heal
and predictive-heal number for LoreRim and Tuxborn before this section is void.

**The oracle ignored two cooldowns.** It obeyed the power-attack cooldown only when the class row declared one, and
never the taunt's. It taunted 6 times a minute against a 30 s cooldown (ruling Q69). It now obeys every hard cooldown
(rulings Q7, Q69, Q73). The knight scenarios' oracle values fell with it.

**Incoming damage counted foes that cannot reach.** `Estimate::IncomingOn` summed the damage of every foe that TARGETS
an actor, at any distance. A brute 15 m away gave a ranged follower "5 seconds to live" and he fled from the first
tick of the kite scenario without firing an arrow. Now a foe that has to close (no bow, no spell in hand) counts only
inside two swing reaches of the actor (`Estimate::StrikeDistance`). The wider question, what he would take with all of
them on him, is `Estimate::ThreatOn`. New measured input `Combatant::rangedWeapon`.

### 24.2 Parameter set A2

With heals working, set A let a control cast (weight 1.8) outrank a heal on a player at 46 percent health. A new
search on LoreRim moved 23 values and lost 0.6 J on Vanilla, so it was not taken as a whole. One value at a time, three
changes hold on every list and both seeds:

**A2 = A with control casts 1.8 to 3.6, heal casts 1.2 to 2.4, the dying-recipient boost 0.7 to 1.4.**

| J (30 fights) | LoreRim s1 | s7 | Tuxborn s1 | s7 | Vanilla s1 | s7 |
|---|---|---|---|---|---|---|
| set A (after the reach fix, poise, DFS) | 61.54 | 61.18 | 62.88 | 61.08 | 78.35 | 78.09 |
| set A2 | 63.37 | 62.86 | 64.50 | 63.29 | 78.92 | 78.49 |
| A2 + the inputs of 24.3 | 63.98 | 63.64 | 64.37 | 63.85 | 78.88 | 78.64 |

Heal claimed, heal urgency and heal size (section 16) each move J by less than 0.4 on any list, inside the noise.
They stay because they are not applicable without their inputs, and the double-heal scenario is where they act.

A control weight of 3.6 is a sign of a missing input, not a truth: a per-list search still wants 7.2 on Tuxborn.
Paralysis and fear decide whole fights in the sim. The sim's Calm was checked as a suspect (it held through blows).
It now breaks when the party hits the target (MODEL, unproven), and that changes J by under 0.01, so the result does
not rest on Calm.

### 24.3 New inputs (pure layers, all not applicable while unmeasured, harness 0 failed)

| Consideration | Family | Input | Why |
|---|---|---|---|
| ControlWorth | control casts | min(1, seconds held / 10) x (0.4 + 0.6 x the foe's health share). New `SpellEntry::controlSec` | a 1 s hold and a 30 s hold scored the same, and so did a hold on a foe at a quarter health |
| MagickaBuys | magicka potion | casts of his best spell the fight still asks for (kept foes' health / best damage, else missing party health / best heal), over 3 | the potion weight wanted 0.275 on LoreRim and 1.65 on Tuxborn. With the input both searches agree it is low (0.14 and 0.55) |
| DamageValue (changed) | offense casts | a spell with NO damage now scores 0 against his best, once his best is known | an Illusion noise spell (damage 0) had no damage term at all and so outscored real spells. Seen in a trace: a healer cast it three times while the player died. +0.4 to +0.85 J on LoreRim, +0.2 to +0.5 on Tuxborn |
| RangeTaunt | taunt | flat inside the reach, same outer edge as before | the aimed-spell curve shaded a taunt at 90 percent of its reach to 0.38. A taunt is a call. Neutral in J |
| MortalTaunt, BoostMortalDanger | taunt, retreat, health potion | new `Snapshot::mortal`: he dies for good at 0 health | see 24.4 |
| PostureTrade | weapon category | seconds he lives at the damage coming in, over 10, on the melee category | do not put the bow away to trade with a brute. Neutral in J so far |
| kite term in RetreatDanger | retreat | behind `Tuning::kiteDistance`, 0 = off | no effect in any run (the retreat is gated by class and by "no heal available"). Left off, in the ledger as unproven |

Tried and removed: a "sacrifice" term (the taunter's own survival counts for less when the player has seconds
left). It made every knight scenario worse on every list (LoreRim knight-boss 14.6 to 4.4). The boss kills the knight
and then the player anyway. What the oracle does there is step back and stay free to block, not die first.

### 24.4 Follower death (marth 2026-11-10)

marth: "Lorerim by default has follower death. I personally have mine turned off."

The sim's default is his setup: a follower at 0 health goes down for 9.5 s (the field median) and stands up.
`--follower-death` makes it LoreRim's default: dead for good, counted as three downs in the utility (MODEL).
IAUS gets one new fact, `Snapshot::mortal`, and two multipliers that only act when it is true: a taunt he cannot
outlast is all but refused (x0.15 under 3 s to live), and retreat and the health potion weigh x1.5.

| J with follower death on (30 fights) | LoreRim s1 | s7 | Tuxborn s1 | s7 | Vanilla s1 | s7 |
|---|---|---|---|---|---|---|
| IAUS not told (`--mortal-blind`) | 63.72 | 63.44 | 63.54 | 62.79 | 77.90 | 77.19 |
| IAUS told | 63.83 | 63.65 | 63.77 | 62.98 | 78.25 | 77.75 |

Small, and the same sign in all six. Kept. Engine side (not written): what tells the DLL that a follower dies for
good is to be probed, never assumed. Candidates: the actor's essential and protected flags, and how the list's
follower-death option is built (which mod, which flag). No list data in native: the fact is read at runtime or stays
false.

### 24.5 Oracle gap after this round (K = 6, 60 s, 20 fights, set A2, before the inputs of 24.3)

| Scenario | LoreRim | Tuxborn | Vanilla |
|---|---|---|---|
| tank3 | +2.0 | +1.7 | +1.2 |
| mage-dragon | +22.2 | +2.3 | +6.4 |
| bandit-pack | +0.9 | +5.7 | +1.3 |
| focus-healer | 0.0 | +1.0 | -0.6 |
| interrupt | +1.9 | -3.3 | +2.2 |
| weakness | +8.8 | +36.4 | +20.9 |
| predictive-heal | +11.6 | +2.6 | +11.3 |
| peel | +12.5 | +8.5 | -4.3 |
| overkill | +3.5 | +4.4 | +2.7 |
| double-heal | +16.3 | +19.5 | +13.6 |
| mp-budget | +1.8 | +19.3 | +9.2 |
| splash | +5.5 | +15.9 | +10.6 |
| control-setup | +38.6 | +21.0 | +5.7 |
| kite | +0.5 | +1.8 | +38.7 |
| knight-boss | +30.9 | +21.5 | +3.5 |
| knight-boss-healer | +46.2 | +25.3 | +17.6 |
| hurt-knight | -0.2 | +14.1 | +14.1 |
| MEAN | +11.9 | +11.6 | +9.1 |

Before this round, same oracle: +14.8 / +13.2 / +9.8. With the oracle held to the cooldowns the LoreRim mean is +10.8
and Vanilla +5.8 (knight-boss on Vanilla -9.0, hurt-knight -4.0: IAUS is ahead of the one-step oracle there).

What the largest clusters say now:

- **knight-boss, knight-boss-healer, control-setup, kite:** the oracle's winning options are `ext:step_back`,
  `ext:sprint_back`, `ext:to_player` and "do nothing" in place of a power attack. These are the actions IAUS has no
  family for (open work 5: deliberate repositioning, block, bash). Nothing in the catalogue closes these gaps.
- **weakness on Tuxborn and Vanilla, splash:** a control cast where the oracle casts damage, or equips. The control
  weight is carrying what a "what does this hold buy against what my damage buys" input should carry.
- **mage-dragon on LoreRim, predictive-heal:** a dry mage names an attack where the oracle equips a weapon or
  drinks. The weapon category layer has no "my magicka is gone" term.
- **peel:** a power attack on the nearest foe where the oracle names the one further away.

### 24.6 Headline after this round (24 fights per scenario, 12 per class, oracle on the first 8 and 4)

| J | Vanilla | Tuxborn | LoreRim |
|---|---|---|---|
| table mode (IAUS off, default gambit rows, rows cut to the rank's slots, one power-attack cooldown per follower) | 75.5 | 59.1 | 61.0 |
| IAUS as on main (no estimates, constant reach) | 75.8 | 58.5 | 60.2 |
| IAUS+ phase 2 (set A2, the inputs of 24.3, learned reach, scroll road) | 78.9 | 63.5 | 63.4 |
| oracle over IAUS+ | 87.0 | 72.4 | 71.9 |

The LoreRim rows moved against section 19 because the world did (poise, follower scaling, the spell reach). IAUS+ is
2.4 to 4.4 J ahead of the gambit table on every list, and IAUS as on main is still level with it or behind. The oracle
is 8 to 9 J ahead of IAUS+. Results files: label HEADLINE in `~/.cache/mfo-iaus-sim/runs/`.

### 24.7 Gap ledger, changes

| id | Change |
|---|---|
| G3, G4 | heals on another actor did not exist on LoreRim in the lab before the reach fix. Re-measured: double-heal +16 / +19 / +14, predictive-heal +12 / +3 / +11. Open |
| G5 | a no-damage offense spell outscored real ones. Closed in the pure layer (DamageValue) |
| G6 | magicka potion has its input (MagickaBuys). mp-budget still +19 on Tuxborn. Open |
| G12 new | swing reach differs per weapon (Q68). `Reach.h` learned reach, proven in the harness. Engine feed not written (tier A) |
| G13 new | control casts have no worth input beyond the hold's length. The weight 3.6 stands in for it. Open |
| G14 new | a dry mage has no reason to change weapon category. Open |
| G15 new | a follower who dies for good weighed his life like one who stands up. `Snapshot::mortal`. Engine read to probe |
| G16 new | deliberate repositioning, block and bash have no family. The largest gaps left on all three lists (knight-boss, control-setup, kite). Needs the road audit of section 13 first |
| G17 new | incoming damage counted foes out of reach. Closed in the pure layer (StrikeDistance) |

## 25. Road audit for the actions IAUS has no family for (gap G16) and the first family built on one

The oracle's extended set (section 9.2) is step back, sprint back, go to the player, go to the healer, get line of
sight, block, bash, scroll. What each one could be built on today, read from `native/APMF_API.h`, `cast/Fire.cpp`
and the two ch.24 clients MFO already has:

| Action | Road | Verdict |
|---|---|---|
| Go to the player, go to the healer (fall back or peel WHILE fighting) | Harbinger ch.24 CombatApproach (`kIntent_CombatApproach`, target X, radius R, `kApproach_Hold`): the follower's combat area is bounded to (X, R), so the engine's own Return To Combat Area walks him there while attacks, casts and blocking keep running. MFO already runs two clients of it: the held-heal approach (`apmf/CombatApproach.cpp`) and the offense approach (`apmf/OffenseApproach.cpp`) | ROAD EXISTS. Needs a third MFO client ("regroup") and an action. Composition, native animation |
| Get line of sight | the same channel, X = the target. `apmf/OffenseApproach.cpp` is this for casts | EXISTS for casts |
| Step back, sprint back (away from a foe, no anchor) | none. ch.24 takes a reference, its `pos` is not read. ch.23 PursuitLeash only refuses moves that lead away from an anchor. `act.flee` is the retreat package under ignore-combat: he stops fighting, which is a retreat and not a step back | PROVEN LIMIT today. The nearest thing is a regroup on an anchor that stands away from the foe. A real step back needs a new Harbinger capability: ch.24 centred on a POINT (a feature request, tier A in the Harbinger repo) |
| Bash | the bash is its own leaf of the combat behaviour tree. ch.18 AttackSelection can only DENY attack classes in the engine's weighted draw and never adds a move. ch.7 CombatAction denies whole leaf categories. Neither can make him bash NOW | PROVEN LIMIT today for a timed bash (interrupting a cast). Only a standing bias exists: the class package's combat-style bash multiplier (card C8) |
| Block | a tree leaf as well. No promote road. Standing bias only: the package's defensive multiplier | PROVEN LIMIT today |
| Scroll | card S3 (the lab's `--scroll-road`) | planned |

So of the oracle's winning options in the largest gaps, step back, sprint back, bash and block have NO road, and that
is where knight-boss, control-setup and kite sit. Those gaps cannot be closed inside MFO. They need one of two new
Harbinger capabilities, both of which fit the house rules (deny-complete, composed, never a forced move): a combat
area centred on a point, and a one-shot promote of a tree leaf (block, bash) the engine already owns. Both are
recorded as feature requests for marth, not started.

### 25.1 Family Regroup (pure layer, behind `Snapshot::regroupRoad`, off in the game)

`Family::Regroup` (appended), target = the player or a party member who can heal others (new `Combatant::healer`).
Considerations: RegroupNeed (the largest of: the player has seconds left, he has seconds left and the anchor stands
away from what is hitting him, he is under 60 percent and the anchor heals), RegroupFar (vetoed inside 300 units and
beyond 3000), a 6 s hard cooldown. In the sim `--regroup-road` turns it on: the order bounds his movement to the
anchor for 4 s and he still fights a foe in swing reach.

| J (30 fights) | LoreRim s1 | s7 | Tuxborn s1 | s7 | Vanilla s1 | s7 | mean |
|---|---|---|---|---|---|---|---|
| road off | 63.98 | 63.64 | 64.37 | 63.85 | 78.88 | 78.64 | 67.23 |
| on, weight 0.8 | 63.75 | 63.75 | 64.56 | 63.27 | 78.51 | 79.19 | 67.17 |
| on, weight 1.6 | 63.30 | 63.93 | 64.81 | 63.42 | 78.87 | 78.77 | 67.18 |
| on, weight 3.0 | 63.25 | 63.82 | 64.49 | 63.39 | 78.85 | 78.95 | 67.13 |

No gain. Not adopted: the family stays as scored-but-never-generated, like Sneak. What the clusters show instead is
that the oracle sends a follower to the player when he is 1700 to 1850 units from the fight (Vanilla
knight-boss-healer, 51 decisions), which is the pursuit leash's job (ch.23, the package's `pk.leash`), not a need of
the moment. The three needs written here are the wrong inputs for that. Open.

## 26. The two missing roads, proven in the lab first, then designed for Harbinger (coordinator, 2026-11-11)

The mandate authorises the missing roads: designed and authored under the house rules on a feat/ branch of the
Harbinger (APMF) repo, tier A, full Opus review, the coordinator merges. The condition: each one is first proven in
the lab as a modelled road with its expected gain, "so we only build what earns its place".

### 26.1 Lab proof: three families behind road flags

Pure layer (`native/iaus`, never generated in the game: `Snapshot::groundRoad`, `blockRoad`, `bashRoad` are false):

| Family | Candidate | Need (its one shaping input) | Sim model (`--roads ground,block,bash`) |
|---|---|---|---|
| GiveGround | the nearest foe that targets him and has to close | he has under 6 s at the damage coming in, or he fights at range and that foe is inside 450 units and he could not afford it on him. 3 s cooldown | the oracle's step back: 500 units directly away, 2 s |
| Block | a foe whose swing at him is in the air, in striking distance, with a melee weapon drawn | 0.5 for the swing alone, the rest by how few seconds he has. 2 s cooldown | the oracle's block: guard up 1.5 s |
| Bash | each kept foe in swing reach | 1.0 when the foe is charging a spell, 0.4 when it is on him, 0.15 otherwise. Stamina, 4 s cooldown | the oracle's bash (20 stamina. Under Chocolate Poise it deals poise damage only, as on LoreRim) |

New measured inputs: `Combatant::swinging` (its swing is in the air) and `Combatant::casting` (it is charging a spell).

J, 40 fights, the same weight on every list (the default set, bash 1.33):

| | LoreRim s1 | s7 | Tuxborn s1 | s7 | Vanilla s1 | s7 | mean |
|---|---|---|---|---|---|---|---|
| no road | 63.33 | 63.81 | 64.11 | 63.79 | 79.07 | 78.79 | 67.15 |
| give ground only | 63.57 | 63.76 | 64.06 | 64.01 | 79.23 | 78.71 | 67.22 |
| give ground + bash | 63.63 | 63.85 | 64.70 | 64.30 | 79.66 | 79.11 | 67.54 |
| block + bash | 63.23 | 63.91 | 64.91 | 64.78 | 79.08 | 78.87 | 67.46 |
| all three | 63.34 | 63.78 | 64.87 | 64.64 | 79.30 | 79.16 | 67.52 |

Scenario utility (30 fights, seeds 1 and 7 averaged), no road against all three:

| | LoreRim | Tuxborn | Vanilla |
|---|---|---|---|
| knight-boss | 13.6 to 17.9 | 24.5 to 42.2 | 73.5 to 77.3 |
| knight-boss-healer | 15.9 to 14.9 | 13.2 to 21.0 | 40.9 to 41.9 |
| control-setup | 75.4 to 70.9 | 62.4 to 71.0 | 77.3 to 78.2 |
| kite (give ground + bash: 1.4 / 6.2 / 24.1) | 1.5 to 1.5 | 6.8 to 8.7 | 16.0 to 18.3 |
| mean of all 17 | 62.27 to 61.99 | 47.25 to 49.51 | 68.29 to 68.70 |

A per-list search of the three weights and need slopes reaches +0.65 J on LoreRim, +1.4 on Tuxborn and +1.3 on
Vanilla, but the lists want different weights (bash 0.5 on LoreRim, 1.33 elsewhere, block 1.0 / 2.0 / 0.56).

**Verdict of the proof.**
- **Bash earns its place on every list** with one shared weight: give ground + bash is ahead in all six runs
  (+0.04 to +0.6 J). It is weakest on LoreRim, correctly: under Chocolate Poise a bash is poise damage and rarely
  interrupts.
- **Give ground earns a little alone** (+0.07 J) and more with bash. It moves the kite scenario on Vanilla from 16
  to 24 and knight-boss on LoreRim from 13.6 to 19.0.
- **Block earns once it blocks only what is killing him.** With a base need of 0.5 for any swing in the air it was
  worth +13 U on Tuxborn's knight-boss and cost on LoreRim. With the base at 0 (`Tuning::blockBase`, the need is only
  how few seconds he has) and weight 2: +0.43 J in the mean of the six runs (LoreRim -0.27 / +0.21, Tuxborn +1.32 /
  +1.02, Vanilla +0.24 / +0.04), no list beyond the noise. That is now the default.
- **The roads are not the whole gap.** The oracle earns +30 to +46 U on these scenarios with the same three actions.
  First-guess needs capture about a tenth of that. What is missing after the roads is TIMING (when a step back pays,
  which swing to block), which is lab work on the considerations, not engine work.

Expected gain to plan with: +0.4 J shared today, +0.65 to +1.4 J with per-list-free tuning, and a ceiling several
times that once the timing inputs exist. The order below builds the cheaper and better-proven capability first.

### 26.2 Design A: a one-shot combat move (bash, power bash, block) in Harbinger

**What MFO does today for the power attack** (`cast/Fire.cpp`, the act.power_attack executor): once the follower is
inside reach of the chosen foe, `NotifyAnimationGraph("attackPowerStartInPlace")`. It is the engine's own animation,
the hit is the animation's own hit frame, and it has been in the field for months. MFO's diagnostics already observe
the sibling events `bashStart`, `bashPowerStart`, `blockStart`, `blockStop` (`Diagnostics.cpp:416-422`), so the
behaviour graph raises them for followers.

**The capability.** A new Harbinger intent (append-only, next free number, proposed name `kIntent_CombatMove`):
PROMOTE, one-shot, bounded. Param: `ival` = the move (Bash, PowerBash, Block), `target` = the foe (Bash: optional
facing and reach check), `fval` = seconds (Block: how long the guard stays up, bounded, default 1.5). The client
declares WHAT, Harbinger enforces the composition:

1. Refuse (kInvalidHandle, with a reason the client can read) when the move cannot be done as declared: no shield or
   blocking weapon drawn, the actor staggered, in a kill move, mid-cast on the hand it needs, or the graph rejects the
   event. Never a silent success.
2. Send the graph event the engine's own combat AI sends for that move. One event, the engine's animation.
3. DENY-COMPLETENESS for a block: while the guard is declared up, the actor's own Offense leaves would drop it. So
   the claim composes with the existing ch.7 CombatAction deny (kCombatActionCat_Offense) for exactly the block
   window and releases both together. Nothing else is denied. A bash needs no deny: it is one animation.
4. End: the window passes, the actor is staggered or downed, the client releases, a save loads.

**What must be probed before one line of it is trusted (principle 5 and 6):**
- Does a graph-sent `bashStart` on an NPC produce a real bash (the hit event with the bash flag, stamina spent,
  the stagger or poise damage)? The engine's own Bash leaf may set attack data on the actor's process before the
  event. If the hit reads that data, a bare event would swing with stale or missing data. The passive probe: log,
  for the engine's OWN bashes on followers, the process's attack data before `bashStart` and the hit that follows.
  Then one deliberate dev-only test on one follower.
- Does `blockStart` alone make incoming hits count as blocked (HitData percent blocked), or does the engine read an
  actor state the Block leaf sets? Same method: observe the engine's own blocks first.
- The 1.5.97 and 1.7 layouts get their own verification (principle 11). An animation event has no address row, which
  is why this design is the cheaper of the two.

**MFO side.** A thin client like `apmf/OffenseApproach.cpp`, `Snapshot::bashRoad` / `blockRoad` read from "the client
is live", Harbinger absent = no candidate (a documented degrade, not a mask).

### 26.3 Design B: a combat area centred on a point (give ground, kite)

**What exists.** ch.24 CombatApproach substitutes the centre and radius of the actor's own standard combat area
with (X, R) where X is a REFERENCE. The engine's own Return To Combat Area walks him there and he fights all the way.
Its `pos` parameter is not read.

**The capability.** An append-only flag on ch.24 (`kApproach_Point`): the centre is `param.pos`, a world point, and
`target` becomes optional (when given, it is the foe to keep facing: movement is two facets, the GOAL and the
FACING, per the PFP design, and only the goal is claimed). Everything else of ch.24 stands as it is: the area is
answered with the engine's own area outside movement, a package's hold position outranks it, a ch.23 leash is
respected, Harbinger ends the claim itself on arrival / target lost / combat end.

**What MFO declares for a step back:** the point 400 to 500 units from the foe on the line foe to follower, checked
on the navmesh (the engine's own path query: a point he cannot reach is refused, never guessed), R about 150, held
2 s.

**Recon done (read-only, Harbinger `native/channels/CombatApproach.cpp`):** the seat already works on a point. Each sweep it
reads the reference's position (`:1097`) and declares the area as `Geo{ x, y, z, space, radius }` (`:1184`), and an
internal caller can already hand it a point directly (`SetInternalBound(actor, point, space, radius)`, `:1339`). So a
point-centred claim is the same substitution with a fixed point and the actor's own space, plus the end conditions that
today hang on the reference (target lost) restated for a point. No new engine seat.

**To probe first:** that the existing ch.24 seat reads its centre in one place (so a point is the same substitution),
and how the actor moves to an area BEHIND him while a foe is on him (he may turn his back: acceptable for a kite, a
tell for a knight). The facing facet is a second step, only if the field shows the tell.

### 26.4 Order and status

1. Design A (combat move): probes, then the Harbinger intent, then the MFO client. Bash first, block when its need
   has earned it in the lab.
2. Design B (point-centred area): the ch.24 flag, then the MFO client.
3. In the lab meanwhile: the timing inputs (26.1, last bullet).

Status: nothing in the Harbinger repo is written. The lab families and the designs above are the brief. This is the
next author's first item.

### 26.5 Parameter set A2 is now the default tuning

`Catalogue.cpp DefaultTuning()` carries set A2 (commit after e02e360), with one addition that is not from the search:
the health potion weight 1.1 to 2.2. With the power attack at 2.394 a melee follower at 22 percent health would have
swung before he drank, which a reviewed harness check ("the health potion ranks first") forbids. The raise is neutral
in the lab (67.25 against 67.23) and keeps that intent. Harness 0 failed.

Set A2 does not need the estimates. In the game as it can run today (no estimates, constant reach):

| J (30 fights) | LoreRim s1 | s7 | Tuxborn s1 | s7 | Vanilla s1 | s7 |
|---|---|---|---|---|---|---|
| the doc weights (on main) | 61.44 | 60.95 | 59.54 | 58.95 | 75.92 | 73.96 |
| set A2 as default | 63.41 | 62.14 | 63.03 | 62.33 | 78.20 | 77.56 |
| set A2 with the estimates measured | 64.10 | 63.53 | 64.64 | 63.83 | 78.74 | 78.67 |

So the weights alone are worth +1.2 to +3.6 J in the live game, before any engine-side fill. This is a behaviour
change on main once merged (more power attacks and control casts, fewer magicka potions) and wants its Opus review
and a field look.

## 27. Class packages (C8), default rows (C12) and prefix orders (Q74) re-run under the corrected world

Sections 17, 18 and 22.1 were searched before Chocolate Poise, follower scaling, the spell-reach fix and rulings Q73
and Q74 were in the lab. All three searches were run again (table mode, train LoreRim, validate Tuxborn and Vanilla,
seeds 1 and 7, the same keep rule). The files `tools/iaus_sim/packages/<class>.json` and `defaults/<class>.json` now
hold THESE results and supersede the tables of sections 17, 18 and 22.1. The default-row search now runs with the
rows cut to the rank's slots, so the sets it finds already have to work as prefixes.

**Packages kept (7 of 35):**

| Class | Mean gain (J) | Change against today's package |
|---|---|---|
| mercenary | +4.94 | melee stance offensive x4, power share x2, leash 1500 |
| knight | +4.00 | melee stance offensive x4, bash x2, power share x0.75, leash 1000 |
| ebony-warrior | +2.78 | offensive x2.66, power share x2 |
| warrior | +2.38 | offensive x2.66, power attack on a staggered foe x2, power share x2, leash 2500 |
| listener | +1.46 | melee and ranged stance offensive x2, power attack on a staggered foe x2, power share x1.33 |
| priest | +0.80 | cast stance offensive x2 |
| nightblade | +0.76 | offensive x1.33, leash 1500 |

The pattern is the same as in section 17 and stronger: the melee line wants a far more offensive melee stance than
today's and a leash on the player. The other 28 classes show no robust gain (blademaster +2.74 and great-knight +1.09
gain on the train list and lose on a validation list, so they are not kept).

**Default rows kept (11 of 35):** blademaster +5.03, witchhunter +3.91, living-legend +3.28, warrior +3.17, champion
+2.78, conjurer +2.10, sorcerer +1.98, nightingale +1.91, grand-battlemage +1.52, necromancer +1.18, wizard +1.10.
The row sets are in each class's `defaults/<class>.json` (`lab`, with `today` beside it and the per-list evidence).

**Prefix orders kept (3):** listener +1.96, warlock +1.76, grand-battlemage +0.56. Fewer than in 22.1 because the row
search itself now scores the cut sets.

Caveat, unchanged: table mode acts through the sim's engine-AI MODEL, and only some combat-style fields are
modelled (offensive, defensive, the power-attack and bash multipliers, fallback, the three weapon weights). The
direction is the finding. The values want the in-game probe before the C8 and C12 authors take them.

## 28. Review of the phase-2 native diff (Opus tier 3, on 303c14d) and the fix round (successor 2, 2026-11-11)

The review found nothing above SEV-3. Its report is `scratchpad/agentlogs/iaus-p2-review.md`. Cost about 58 µs per
tick at 152 candidates, the harness 0 failed on the branch and on a trial merge with main. This round fixes F2, F4,
F5 and F6, lists F1 and F3 as intended, and merges main (13f2e41 and later: the taunt rules) into the branch.

### 28.1 What phase 2 changes in the LIVE game once it merges

Phase 2 is not only lab work. These reach a follower on Sentient AI the day the branch merges, with no engine-side
fill and no road flag. Lab effect = mean J over LoreRim, Tuxborn and Vanilla, seeds 1 and 7, 30 fights, the
estimates measured. The noise at that size is about 0.5 J.

| Change | Live because | What the player sees | Lab effect |
|---|---|---|---|
| Set A2 as the default weights (26.5) | `DefaultTuning()` | more power attacks and control casts, fewer magicka potions | +1.2 to +3.6 J per list without the estimates |
| TauntChance (F1): a taunt is scaled by its chance to work, 0.8 x chance + 0.2 | main fills both levels (`Combatant::level`, the taunt rules card) | fewer taunt attempts on a foe above his level. With the taunt weight at 0.75 a taunt on a foe 2 levels up scores about 44 percent lower than on main (chance 0.4: the curve gives 0.52, about 0.74 after the compensation, times the 0.75 weight, against main's 1.0) | weight 0 against weight 1: 68.97 against 68.91, inside the noise |
| RangeTaunt (F3) replaces RangeSpell in the taunt set | the taunt set is read every tick | a taunt is no longer shaded down near the edge of its reach (the spell curve gave 0.38 at 90 percent). Same outer edge, 1.15 x reach | neutral (24, table of inputs) |
| BoostControlled x1.3 on a blow, now PARALYSED foes only (F2) | main fills `paralysed` | he presses a paralysed foe. He no longer gets the boost on a FLEEING foe, which with the chase range pulled him after runners | fleeing included 68.92, paralysed only 68.93, boost off 68.99: inside the noise |
| The health potion weight 2.2 (26.5) | `DefaultTuning()` | none intended: it keeps "the potion ranks first when he has no valid heal" under the higher power attack weight | neutral |
| DamageValue scores a no-damage spell 0 against his best | the book is filled today | a spell with no measured damage is not cast as offense while a measured one exists | part of set A2's numbers |

Not live: everything behind `Snapshot::regroupRoad` / `groundRoad` / `blockRoad` / `bashRoad`, `Tuning::kiteDistance`
(0), and every input whose fact is still unmeasured in the game (the estimates, `mortal`, `swinging`, `casting`,
`controlSec`, `castsWanted`): those considerations read "off" until the engine-side fill card.

### 28.2 The fixes

| Finding | Fix | Check |
|---|---|---|
| F2 (SEV-3) BoostControlled boosted fleeing foes | new fact `Facts::foeHelpless` = paralysed. `foeControlled` (paralysed or fleeing) stays the control-cast gate's input | harness 0 failed. J 68.92 to 68.93 |
| F4 (SEV-4) HealClaimed counted as an applicable input on every heal | off unless another heal is in flight on the recipient (the I4 rule: an input that is off does not raise n) | J 68.93 to 68.91 |
| F5 (SEV-3) heal weight 2.4 halved what `noHealDrink` (0.50) and `noHealRetreat` (0.30) meant | `Decision::bestHeal` is now the best heal's score DIVIDED BY its family's action weight, so no weight tuning moves the two gates again. The thresholds are rescaled by the 1.2 they were written against: 0.4167 and 0.25, the same meaning as on main for a heal spell. A heal SCROLL (weight 0.9) now blocks the drink a little sooner than on main (its raw score above 0.375 instead of 0.5) | harness 0 failed, "the health potion ranks first" passes, J unchanged to three decimals (68.91) |
| F6 (SEV-4) `scheduler/IausSource.cpp` had no case for Regroup, GiveGround, Block, Bash | the four join "no action road for this family yet" | CI |

`[iaus-why]` and the readout print `best heal` in the new unit (score over weight).

### 28.3 Merge with main

`Estimate.h` is the branch file, `TauntChance` uses `(std::clamp)` and its comment says the roll in `Taunt.cpp`
calls it (Q72). `Core.h` keeps main's `rowlessTauntCooldownSec = kTauntCooldownSec` with the phase-2 Tuning fields
after it. The harness keeps both includes and runs `SharedTauntPowerRules()` then `ReachLearning()`: 0 failed.

For the engine-fill card (recorded by the coordinator as backlog): F7, `bestOn` filters and a per-entry "measured"
flag, and `Reach` falling back to `fMeleeReach` while `!Known()`.

## 29. Design A built: the bash through Harbinger's combat move (successor 2, 2026-11-11)

Branches: Harbinger `feat/combat-move` (ch.31, `kIntent_CombatMove`, ABI v38) and MFO `feat/iaus-combat-move` (the
mirror, the client, the probe switch, the IAUS executor). Both tier A, both waiting for their Opus review.

### 29.1 The probe that changed the design: a bare animation event is not the engine's road

Section 26.2 proposed sending the engine's own graph event (`bashStart`) and asked first whether the engine's Bash
leaf sets attack data before it. The disassembly of 1.6.1170 (1.5.97 checked as a twin) answers that before any
field run:

| Step | Where (1.6.1170) | What it does |
|---|---|---|
| The Bash leaf's attempt | AE 47841 `0x86B4A0` | tests the actor (`0x6B90F0`: attack state None, graph bool `IsAttackReady`, attacking not disabled), then StartAttack on the bash context |
| StartAttack | AE 49170 `0x8A28B0` | draws an entry of the actor base's attack data, builds a `TESActionData` on its stack (priority 2, source = the actor, animation event = the entry's), calls `Process` (vtable slot 5) |
| The action mediator | AE 38949 `0x6CD0D0` | its pre-checks, the animation event on the actor's graph, and on success the post-action step AE 39004 `0x6CF760` |
| The post-action step | calls AE 39541 `0x6EE0C0` | THE setter of an actor's attack data: looks the event up in the actor base's map, writes `process->high->attackData`, then sets the attack state (bash flag: 6 = Bash) |

AE 39541 has exactly one caller on each build. So the attack data and the attack state a hit reads are set ONLY by
the action road. `NotifyAnimationGraph("bashStart")` alone plays the swing with whatever attack data the actor had
before, and no Bash state. The design is therefore the action, not the event: Harbinger builds the same action data
and calls `Process()` once. That costs two verified address rows (the constructor and the vtable) where 26.2 hoped
for none.

The same reading shows the engine's block: `0x7F9770(actor, 3)` = the Left Attack action through the same `Process`
road, released with the Left Release action (index 4).

### 29.2 What was built

Harbinger (`core/CombatMove.{h,cpp}`): a one-shot like `kIntent_Place`, never a claim, no hook. `RequestEx(actor,
kIntent_CombatMove, 0, {ival = Bash | PowerBash, target = the foe})`. On the game thread: the engine's own bash test,
a yield to any ch.7 Offense deny and to a combat substrate, the player's ranking (the client must outrank Engine on
CombatAction), the actor base's bash entry, ONE `Process()`, then the read-back. `Started` only when the attack data
is that entry and the attack state is Bash. A hit event with the Bash flag inside 2.5 s sets `HitSeen`. Everything
else is `Refused` or `Failed` with a reason, logged. `kCombatMove_Block` is reserved and refused. Exactly 1.6.1170 and
1.5.97, 1.7.104 refused by name. INVARIANTS #0 (l) is written as PROPOSED: its adoption is marth's.

MFO: `apmf/CombatMove.cpp` (RequestBash, PollCombatMove: the outcome is written to MFO.log once), `Vocab::kActBash`
(IAUS-only, in no board table, never serialized), its executor in `cast/Fire.cpp` (the foe alive and inside melee
reach now, else transparent), `Family::Bash` mapped in `scheduler/IausSource.cpp`, `Snapshot::bashRoad` = Harbinger
serves ABI 38 AND `[Debug] bIausBashRoad` (default OFF until the field proves the road).

### 29.3 The field probe (needs marth's run)

Two switches, both default OFF:
- Harbinger `APMF.ini [Probe] bCombatMove=1`: PASSIVE. One line per melee hit by or on a teammate: the hit's flags
  (power, bash, blocked), the attacker's attack data and attack state, the target's blocking state. This shows what
  the engine's own bashes and blocks look like.
- MFO `MFO.ini [Debug] bCombatMoveProbe=1`: a follower whose combat target is in melee reach asks for a bash every
  6 s. This is the one deliberate test.

The road is PROVEN when a `[combat-move] ... STARTED` line is followed by `[combat-move] ... HIT` and the probe's hit
line for it reads `bash=true` with attack data `bashStart`. It is NOT proven on a `FAILED` (the engine refused the
action, or accepted it without the attack data) or on STARTED lines never followed by a hit while he stood in reach.
Then `bIausBashRoad` stays off and the lines go to an Opus diagnosis. Nothing masks either case.

### 29.4 A finding outside the brief: MFO's power attack is a bare animation event

`cast/Fire.cpp`'s power attack is `NotifyAnimationGraph("attackPowerStartInPlace")`. By 29.1 that swing runs without
the engine setting the power attack's data, so its hit may be an ordinary hit in a power attack's animation (no power
damage multiplier, no stamina cost, no power-attack stagger). It has been in the field for months, so it does swing.
The passive probe above answers what its hit is: its line shows `power=` and the attacker's attack data for every
follower power attack. If the flag is missing, the proper road is the same Harbinger action with the power entry (a
`kCombatMove_PowerAttack`, append-only), and the lab's power attack numbers should be re-read, because the sim gives a
power attack its full damage. Saved as a question, not changed here.

### 29.5 Not built, and why

- Block. It is a HELD state: the engine starts it with one action and ends it with another, and the actor's own
  offense leaves and the Block leaf's own release would drop a guard MFO asked for. A complete deny for the window
  (ch.7 Offense, plus the release) is its own Harbinger amendment and round. The lab gain to weigh it against is
  +0.43 J (26.1).
- 1.7.104. Both rows verify there. The road was not read on that build, so it refuses by name.

### 29.6 Design B built: give ground through ch.24 on a point (Harbinger `feat/approach-point`, ABI v39)

Harbinger: one flag bit, `kApproach_Point`. The centre of the ch.24 bound is `param.pos` and `param.target` must be 0.
The recon of 26.3 held: the seats only ever apply a point (`Entry::want`), so the change is the sweep's source of X,
the end condition restated for a point (the actor left the point's cell space), and the call-time refusals. No new
seat, no new address row, no struct, no slot. An older Harbinger refuses the claim for its missing target, so the
bit is safe to send.

MFO: `apmf/GroundApproach.cpp`. The point is 400 units straight away from the foe, R 150, the claim ends on arrival
and never outlives 2.5 s. `Vocab::kActGiveGround` (IAUS-only), its executor in `cast/Fire.cpp`, `Family::GiveGround`
mapped in `scheduler/IausSource.cpp`, `Snapshot::groundRoad` = Harbinger ABI 39 AND `[Debug] bIausGroundRoad`
(default OFF).

Two things 26.3 asked for that this round does NOT do, said plainly:
- **No navmesh check of the point.** MFO declares the point by geometry only. A point the engine cannot path to makes
  the engine's own return fail: Harbinger ends that as EngineDropped after 3 s, and MFO releases first at 2.5 s with
  "the engine never carried the bound" when that is the case. Nothing is retried. A pre-check wants Harbinger's
  space query (main thread only) and is open.
- **No facing.** How he moves to a point behind him while a foe is on him is the engine's. He may turn and walk.
  That is the first thing to look at in the field.

**It cannot fire in the game yet.** GroundNeed reads the follower's seconds to live (`selfTtl`, `threatTtl`), which
come from the damage estimates the engine side does not fill yet (the fill card, next). Until then the need is 0 and
the family is never chosen: the road is built and inert. The same holds for half of Bash: BashNeed is 1.0 only for a
foe that is charging a spell (`Combatant::casting`, unfilled), and 0.4 for a foe on him, which is filled today.

To look at in game once the estimates are filled (`bIausGroundRoad=1`, Sentient AI on an archer or a mage with a brute on him): `[ground] ... step
back from ...` then `[ground] ... over: arrived (the area carried the bound N time(s) ...)` with N above 0, and by eye
whether the step reads as backing off or as turning his back.

### 29.7 Lab numbers to hold the field against

From 26.1 (40 fights, the same weight on every list): give ground + bash +0.39 J in the mean and ahead in all six
runs, block + bash +0.31, all three +0.37. The oracle earns +30 to +46 U on the scenarios these roads were built for
with the same actions, so the remaining gap is the TIMING inputs, which is lab work on the considerations.


## 30. Stealth and disengage in the lab: the engine's detection formula, G2 and G3 (sim structure only, 2026-11-11)

Branch `feat/iaus-sim-stealth`, on top of `feat/g3-disengage` (G2 Layer 1 + G3 + the ch.7 adoption). `native/` is not
touched. Nothing here tunes a curve or a weight: this section changes the WORLD the decision code is measured in, and
hands a baseline to the tuning round (30.8).

It replaces the lab stealth model of `iaus-classes.md` section 6 (a hand-written skill duel with a "close and in view
= seen" rule and stand-in considerations) and the numbers of its section 7.

### 30.1 What changed

| Piece | Before | Now |
|---|---|---|
| Detection | a lab formula of unproven shape, one yes/no per foe | the engine's formula of `calculate-detection-re.md` sections 3 and 4, per (foe, follower) pair, per detection pass: `tools/iaus_sim/Detect.h` |
| What a foe group knows | "sensed him in the last 0.6 s" | the group's own record by section 5 of the card: level, stealth points, known, lost, last known time and place (`World.h` `Body::Rec`, `DetectPass`) |
| Facts IAUS reads | stand-ins (`foeHelpless` for the blow, a hand-made Sneak candidate) | the real ones: `Combatant::detectsMe`, `Snapshot::selfSneaking`, `hideRoad`, `concealRoad`, `concealPoint`, `fleeRun`. The real `Generate` makes the hold row, the blow from stealth and the Disengage family |
| G3 | a 5 s move order behind a random pillar | `native/Concealment.cpp` mirrored: the pick, the run, the hold, every end, the cooldown from the end (`Policy.h` `PreparePick`, `BeginRun`, `ServiceRun`) |
| Stealth scenarios | never in the headline J | in the headline J while a road is modelled (30.5) |

### 30.2 The model, row by row

Grades as in the RE card. DISASM = read on 1.6.1170, which proves a path exists and not that it runs. MODEL = the lab's
own choice. Nothing below is FIELD yet: probes P1 to P3 of `g2-hide-design.md` have not run.

| Row | Model | Source and grade |
|---|---|---|
| Range | `fSneakMaxDistance` x exterior x flying, x `fDetectionCombatNonTargetDistanceMult` when he is not that foe's own target, shrunk past `fDetectionLOSDistanceAngle`. Outside it the sight road does not run | card section 3, DISASM |
| Score | `fSneakBaseValue` + sound + light + skill, every term as the card writes it. Level = the truncated score, 0 becomes -1 | card section 4, DISASM |
| Game settings | read by name from the list. Fallback = what Skyrim.esm ships, which is NOT the executable's image default the card quotes: base -15 (image -25), max distance 2500 (1500), light mult 0.33 (1.4), alert mod 0 (0.2), action mult 2 (1), sneak light mod 15 (0), light level max 300 (100) | the three cached `sim.json` files agree on the vanilla values. A list overrides them at run time |
| Group record | notice above the bar (70 sneaking, 25 not) while stealth points are above 0, any positive level after. Lost after `fCombatDetectionLostTimeLimit` without a positive level, at once when a foe has him in its view cone and scores below 0, or when a foe within 256 units of the last known point scores below 0 | card section 5, DISASM. The view-cone byte is LIKELY |
| One group | every foe of a fight shares one record | MODEL: an encounter is one combat group. It is also what `Hide::Read` returns in the game: the GROUP's record |
| Stealth points refill | full again `fCombatStealthPointRegenLostWaitTime` (10 s) after the group lost him | MODEL from the setting's name. The card: UNREAD (risk R4). The drain the settings name (`fCombatStealthPointDrainMult`) is not modelled |
| Pass cadence | every `fDetectionUpdateTimeMin` (1 s vanilla) | MODEL from the setting's name. The job's cadence is UNREAD |
| Sneak attack | he sneaks and THAT foe's own last level of him is 0 or below: x the weapon's `fCombatSneak*Mult` x the list's entry 18 rows | `iaus-classes.md` 5.2, PROVEN path. The second road through the group is not modelled |
| A hit reveals | ForceDetect: the group has him at level 100. The next pass scores the pair again, so a shot from the dark can be lost again a second later | hit is one of the 9 callers (class-m0-re.md). What happens between is the formula's |
| Sound of an action | swing 50 (Normal), dagger and bow 10 (Silent), cast 50, for `fDetectionActionTimer` (1.2 s) | MODEL. The card: "which actions set it and how loud: UNREAD" |
| Movement | combat movement is at the run speed and counts as running, crouched too (x0.55 speed). A backpedal does not | MODEL. Probe P1 logs IsRunning |
| Light | the cell's light on him 0 to 1, times `--light-scale` (100) as the cached light level | MODEL. The real scale is unknown (probe P4 is not built). Sensitivity in 30.6 |
| Skill | his Sneak actor value plus the list's entry 57 rows his skill reaches. Counts only while he sneaks | card (entry 57, owner the follower). Which perks he holds is MODEL |
| Equipped weight | 3 per light piece, 9 per heavy piece, plus the weapon in his hands | MODEL |
| Facing | a fighter faces his combat target. With none, the way he last moved | MODEL |
| Not modelled | the noise road and the projectile road (their scorer was read by its strings only, and a positive level there stores the noise point, not his position), NightEye, blindness, sleep, ambush packages, race size, entries 47 and 15, foes searching the last known point | stated limits |
| Who is modelled | sneak classes under `--stealth g1` and above. Everyone else is always known to the foes, as before | MODEL |
| Free stealth entry | one fight in five nobody has him until his first blow (card C13) | W Thief, unchanged |

Card G3 (`native/Concealment.cpp`, each number copied from it): 24 candidates on rings of 320 and 600 units, each a
straight walk that stops at the first solid thing. Refused: a walk under 160 units, a point within 250 units of a foe,
a point within 200 units of the leash edge. Hide: every listed foe's line to the point is cut by geometry, the nearest
wins. Flee: at least 150 units farther from the nearest foe, the farthest wins. One pick per 1.5 s, fresh for 3 s, its
answer lands 0.1 s after the ask. Arrival inside 90 units. Hide run: ends hidden after two consecutive laps on which
the group does not detect him, or hit at the point, or failed after 10 s on the way or 12 s at the point. Flee: his
decisions go on (no offense generated, his own heals x2), it ends above 30% health, on arrival, or failed after 10 s.
The 25 s cooldown counts from the end. Retreat and the flee are both generated and the score decides.

Two lab choices inside G3, both MODEL:
- The stance is held through the hide run (a crouched run). G1 holds it for a sneak class all fight.
- On a G3 run his feet keep going through a weapon swap, a potion and a cast. Elsewhere in the lab those three still
  hold the body, as before, so that `--stealth off` stays draw for draw. Saved as Q-SIM-5.

### 30.3 Road levels

`--stealth off | g1 | hide | g3` (`aware` still means g3). **The default is now g3**: every road T2 builds, which is
what the tuning round must tune against. `off` is the game with its switches at their defaults (`bSneakInCombat=0`,
`bIausHideRoad=0`).

| Level | Modelled as ON |
|---|---|
| off | nothing. Equal to the lab before this section, draw for draw (checked: all 12 Beta 1 classes, 6 seeds, 30.6) |
| g1 | the stance in combat. The engine hides him and multiplies his blows. IAUS reads the verdict and boosts the blow |
| hide | + G2's hold row and the free stealth entry |
| g3 | + the hide run of a sneak class and the flee of the other disengage classes |

Sneak data: `--sneak-profile vanilla | steep | list`. The built-in world has no sneak data of its own, so it takes
`vanilla` (Skyrim.esm: Backstab, Assassin's Blade, Deadly Aim, the five Stealth ranks). `steep` (alias
`lorerim-like`) is a hand-written profile with the SHAPE of a Requiem-style list: a flatter skill duel, light and
movement count for more, a 160 degree view cone, a dagger tree up to x4 on the weapon's x3. It is lab test input in
`Detect.h`. It is not read from a list's files and nothing in `native/` sees it. With `--list` the list's own values
stand.

Other switches: `--light-scale N`, `--blow-stands` (30.7 finding 3).

### 30.4 Scenarios added

All four are stealth-lab scenarios (in J only while a road is modelled). Geometry is in the fight's own frame
(`FightSpec::cover`).

| Scenario | Class | What it isolates |
|---|---|---|
| thief-cover | thief | three bandits have found him in a lit hall with three large pillars: cover exists |
| thief-open | thief | the same three at noon on open ground: no point exists, so no hide run can be generated |
| caster-flee | evoker | 27% health, a heal, no potion, three blades on him: the flee and the heal on the way |
| melee-flee | mercenary | 27% health, one potion, no healer, two foes on him: a class without the disengage flag |

### 30.5 What J covers now

With a road modelled the headline's scenario half is the 17 of main plus `ambush`, `thief-cover`, `thief-open`,
`caster-flee`, `melee-flee` (22). The 17 keep their indexes, so their fights are the same in every level. A class's J
therefore moves for two reasons, and the table separates them: its scenario SET changed (Thief 1 to 5 scenarios, Evoker
6 to 7, Mercenary 1 to 2), and its BEHAVIOUR changed.

### 30.6 Baseline (built-in world, vanilla profile, `--seeds 1,2,3,4,5,6`, 24 fights per scenario)

| Class | before (= off now) | g1 | hide | g3 (default) | Why it moved |
|---|---|---|---|---|---|
| melee | 79.08 | 79.08 | 79.08 | 79.08 | |
| ranged | 63.18 | 63.18 | 63.18 | 63.18 | |
| mage | 77.47 | 77.47 | 77.47 | 77.47 | |
| knight | 68.52 | 68.52 | 68.52 | 68.80 | the healer beside him in knight-boss-healer flees |
| warrior | 87.72 | 87.72 | 87.72 | 87.81 | the evoker beside him in control-setup flees |
| mercenary | 84.28 | 81.03 | 81.03 | 81.03 | set only: melee-flee joined. His behaviour is unchanged |
| archer | 78.31 | 78.31 | 78.31 | 78.31 | |
| thief | 39.31 | 80.25 | 75.03 | 74.93 | set (kite alone before) and behaviour. Natural bracket 68.43 to 78.50 |
| evoker | 80.63 | 79.71 | 79.71 | 80.27 | set -0.92 (caster-flee joined), the flee +0.56 |
| conjurer | 69.30 | 69.30 | 69.30 | 70.26 | the flee +0.96 |
| healer | 81.30 | 81.30 | 81.30 | 81.50 | the flee +0.20 |
| warlock | 78.14 | 78.14 | 78.14 | 78.15 | the flee +0.01 |
| all 12 together | 76.69 | | | 78.11 | |

Standard errors over the 6 seeds are 1.5 to 4.5 per class (0.6 for the 12 together), so a difference under about 1 J
inside one row is not a finding by itself. The seeds are the same in every column, which makes the columns paired.

Thief, natural bracket, seed 1 (8 fights x 5 ranks):

| Level | U | win % | dps | taken/s | hidden % | sneak hits % | damage from stealth % | still % | hide runs/min |
|---|---|---|---|---|---|---|---|---|---|
| off | 64.2 | 67.5 | 15.8 | 2.4 | 0 | 0 | 0 | 0 | 0 |
| g1 | 78.1 | 80.0 | 33.8 | 0.9 | 81.1 | 75.8 | 97.0 | 0 | 0 |
| hide | 73.4 | 72.5 | 27.2 | 0.6 | 86.8 | 83.8 | 98.0 | 8.7 | 0 |
| g3 | 73.5 | 72.5 | 27.1 | 0.6 | 86.5 | 80.7 | 97.8 | 8.1 | 0.4 |

Sensitivity of the Thief's J at g3 (74.93): `--light-scale 200` 73.52, `--light-scale 300` 72.61, `--sneak-profile
steep` 72.01, `--blow-stands` 74.88.

Scenarios with the oracle (K = 6, 60 s, 20 fights, seed 1, g3): thief-cover 88.0 (gap +4.4), thief-open 92.1 (-0.5),
caster-flee 84.7 (+4.0), melee-flee 73.6 (+10.5), ambush 90.5 (+0.1), kite 9.2 (+12.6). Mean gap over the 22: +1.15.
double-heal (-23.6) and knight-boss-healer (-11.7) are negative: the oracle at this K and horizon loses to IAUS there.
double-heal reads -27.1 on the branch base as well, so that is not from this section.

Checks: `tools/iaus_sim/run.sh` PASS (BAD-FACTS 0, invalid picks 0) at every level, in table mode, on LoreRim, Tuxborn
and Vanilla. `SIM_CLITEST=1` 13 checks PASS. `tools/iaus_harness/run.sh` 0 failed. One address and undefined-behaviour
sanitizer run of the scenarios and the class cells: clean.

### 30.7 Findings

1. **Under the engine's formula a perked sneak follower is very hard to detect.** The follower's Sneak skill enters
   whole (the player's is scaled by `fPlayerDetectionSneakMult` 0.4, an NPC's is not), and the list's entry 57 rows add
   20 to 40 on top. With a skill of about 90 against a bandit's 15 the skill term is about -40, and only bright light
   close up plus the noise of a run or a swing brings the score above 0. So the Thief is hidden 80 to 87% of his time
   and 97% of his damage comes out of stealth. This rests on two things nobody has observed: that the engine scores
   foe to follower pairs this way (R2) and that a follower holds the perk rows his skill reaches. Read every Thief
   number as "if the card's reading and the perk model hold".
2. **The hold row costs him.** g1 80.25, hide 75.03. With the hold row he stands still 8% of his time and his damage
   per second falls from 33.8 to 27.2. The cause is in `Generate`, not in a weight: while the hold stands no attack is
   named on an unaware foe he cannot strike now, so nothing competes with the hold, and it has no exit but a foe
   walking into his reach or his sight. Halving or doubling the Sneak family's weight changes nothing (tried as a
   command check, not as tuning). Gap G18.
3. **The hide run adds nothing at these numbers.** hide 75.03, g3 74.93. Every run ends hidden (100%), because the
   engine loses him on its next pass anyway. His own blow reveals him (ForceDetect), the group record says found, the
   need is 1, and he runs. Gap G19. The need is one bit: all foes of one group carry the same record.
4. **The blow and the stance.** `scheduler/IausSource.cpp` releases the hide hold's stance claim when the blow is
   PICKED (`Concealment::NoteBlow`). If nothing else holds the stance he may be on his feet when the hit lands, and the
   engine's sneak attack needs the attacker sneaking. The lab keeps the stance by default. `--blow-stands` measures
   the other reading: 74.88 against 74.93, small here because that claim only exists after a hide run. Saved as
   Q-SIM-2 for the reviewer to settle in `native/`.
5. **The flee is a small gain, and the oracle wants more.** Evoker +0.56 J, Conjurer +0.96. In caster-flee the oracle
   is 4 U ahead: it heals before it runs, leaves earlier than 30%, and keeps stepping back after. Gap G20.
6. **A class without the disengage flag has no way out.** melee-flee: the oracle is 10.5 U ahead with step back,
   sprint back and "to the player". Gap G21.
7. **Group record against pair level.** `Facts::foeUnaware` reads the group, the engine's multiplier reads the pair.
   Blows the boost named that landed without the multiplier: 0.6 to 1.6% (`stealthMiss%`). Small. Gap G22.

### 30.8 Gap ledger, changes

| id | Change |
|---|---|
| G18 new | the hold row has no exit of its own (finding 2). Scenario thief-cover. Open. Not reachable by tuning |
| G19 new | the hide run's need is one bit of group knowledge (finding 3). Open |
| G20 new | the flee's fixed 30% line, one run per 25 s, the end on arrival, heal not ordered first. Scenario caster-flee. Open. The weight, the heal boost and the cooldown are tunable, the line is a constant in `iaus/Disengage.h` |
| G21 new | no way out without the disengage flag. Scenario melee-flee. Open |
| G22 new | group record against pair level. Measured small. Open |
| C-G1, C-G2, C-G3 | status closing: built on `feat/g3-disengage`, modelled here, not proven in the field |
| C-FLEE2 new | the flee of the disengage classes: closing |
| C-NOFLEE new | knight, warrior, mercenary: no Retreat and no flee is ever generated. Open |

### 30.9 Sim bugs fixed on the way

- MFO-B755: a full run's headline J is always 24 fights per scenario again, whatever `--fights` says (a `--seeds` run
  still honours `--fights`). A `--baseline` file now starts with a `#meta` line (cell size, seed, stealth level,
  profile, class set) and `--compare` REFUSES a baseline whose line differs.
- MFO-B756: the JSON headline has `oracle_covers`.
- MFO-B757: with no J computed the JSON headline's `covers` and `fights` are null.
- The committed `baseline_builtin.txt` could not be reproduced on the branch base (1385 metrics off, an older class
  set). It is now `run.sh --no-json --classes beta1 --baseline tools/iaus_sim/baseline_builtin.txt`.
- Results file, fields added: top-level `stealth` and `sneak_profile`, `headline.oracle_covers`. New metrics:
  `hideOk%`, `hideFail%`, `flee/f`, `fleeHealed%`, `fleeFail%`, `fleeHpGain%`, `runDown%`, `stealthMiss%`, `run%`,
  `still%`. The dashboard needs no change (`dash.py --json-check` passes on the new runs).
- `Tune.h`: `t.disengageCd` (card G3's hard cooldown) can be set and searched.

### 30.10 Handoff to the tuning round (curves and weights only)

**Updated by section 31 (T2 batch S, 2026-11-12).** The world this round tunes against is now the one of section 31: every T2
road modelled and the feet stop removed. The baselines below (74.93, 80.27, 78.11, 76.69) are the `--t2 off` lab and are kept
as the reference. The numbers to tune against are in 31.11, with the two gates the review of a645ca2 required:
the `--stealth off` no-loss gate stays REQUIRED, and the feet asymmetry (MFO-B783) is GONE, so `aw.4` against `aw.21` and
`c.63.*` may be searched again.

The world is fixed for this round. Do not edit `tools/iaus_sim/*.h`, `sim.cpp` or `native/`. Search with `--set` and
`--opt`, then report the values. A change is kept only when it holds on other seeds and does not lose on a list.

Indexes (`run.sh --list-params`): families 0 attack, 1 power_attack, 3 sneak (the hold row), 4 flee (Retreat),
5 drink_health, 11 cast_heal, 21 disengage. Considerations 63 danger (RetreatDanger), 83 strike (BoostStrike),
84 hold (HoldStill, a gate), 85 need (DisengageNeed), 86 flee_heal (BoostFleeHeal).

Iterate first, in this order:
1. Flee and the heal on the way (Evoker, Conjurer, Healer, Warlock): `aw.21`, `c.86.b`, `t.disengageCd`. Do NOT
   search `aw.4` (Retreat) against `aw.21`, nor `c.63.*`, until the feet asymmetry is gone (review of a645ca2,
   SEV-3: only a G3 run keeps his feet moving through a swap, potion or cast, so the lab favours the flee over Retreat;
   Q-SIM-5, backlog MFO-B783).
2. The blow from stealth (Thief): `c.83.b`, `aw.1` against `aw.0` for the dagger case.
3. The hide run (Thief): `aw.21`, `c.85.a`, `c.85.b`, `t.disengageCd`. Expect little: findings 2 and 3 are structural.

```
# 0. build once, check the baseline is the one of 30.6
tools/iaus_sim/run.sh --no-json --classes thief --seeds 1,2,3,4,5,6        # 74.93
tools/iaus_sim/run.sh --no-json --classes evoker --seeds 1,2,3,4,5,6       # 80.27
# 1. search one class on seeds 1 to 3 (about half a minute for four parameters and one pass)
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --opt-class evoker --opt aw.21,c.86.b,aw.4,c.63.a,c.63.b,t.disengageCd --seeds 1,2,3 --opt-fights 12 --opt-passes 2
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --opt-class thief  --opt c.83.b,aw.21,c.85.a,c.85.b,t.disengageCd --seeds 1,2,3 --opt-fights 12 --opt-passes 2
# 2. validate the printed "--set ..." line on the held-out seeds, for that class and for all twelve
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --classes evoker --seeds 4,5,6 --set <line>
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --classes beta1  --seeds 1,2,3,4,5,6 --set <line>      # baseline 78.11
# 2b. REQUIRED no-loss gate on the game AS SHIPPED (bSneakInCombat and bIausHideRoad ship OFF; review of a645ca2, SEV-3)
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --stealth off --classes beta1 --seeds 1,2,3,4,5,6 --set <line>   # baseline 76.69
# 3. and on the lists and the other profile
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --list ~/.cache/mfo-list-balance/LoreRim.sim.json --classes beta1 --seeds 1,2,3 --set <line>
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --sneak-profile steep --classes thief --seeds 1,2,3,4,5,6 --set <line>
# 4. what the change did to behaviour: one results file for the dashboard
SIM_NOBUILD=1 tools/iaus_sim/run.sh --classes beta1 --seed 1 --set <line> --label "tuning: <what>"
```

`--opt-class` and `--classes` are different objectives (`--opt-class` adds the balanced and strong1 brackets), so
compare a search's start and end with each other, and a validation with the 30.6 column. `tune_defaults.py`,
`tune_packages.py` and `tune_prefix.py` search default rows and class packages in table mode: they are not part of
this round.

Baseline runs for `tools/iaus_dash` are in `~/.cache/mfo-iaus-sim/runs/` with labels starting "sim-stealth baseline":
six Beta 1 runs (seeds 1 to 6) and one run of the 22 scenarios with the oracle.

## 31. T2 batch S: every T2 mechanism in the lab, the feet stop removed, a new baseline (2026-11-12)

Branch `feat/iaus-sim-t2` on main 33c643f. Only `tools/iaus_sim` and docs change. `native/` is not touched and no curve or
weight is tuned. This section closes the OPEN cells of the Sim column in `t2-manifest.md`.

### 31.1 The switches

Every mechanism is one switch, ON by default. `--t2 off` turns all of them off and restores the feet stop. That is the lab of
section 30, draw for draw: all 12 Beta 1 classes over 6 seeds at both stealth levels give the same J as the base binary
(78.11 at g3, 76.69 at `--stealth off`), and a full table run (`--classes beta1 --fights 3`, 7419 metrics) has 0 differences.
A switch that is off never draws from the fight's random stream. The headline line and the `--baseline` meta line name the
roads of the run (`t2=all`, `t2=off`, `t2=feet-stop,block=off`), and `--compare` refuses a baseline made with other roads.

| Switch | What it models |
|---|---|
| `--feet-stop` | Q-SIM-5 off: a swap, a potion and a cast stop his feet again, except on a G3 run |
| `--c8 off` | the ch.18 share window, the ch.23 leash, the engine's own power attacks for a class with no rule, the Mercenary's Avoid Threat |
| `--dodge off` | the side step out of a projectile |
| `--aversion off` | ch.32 aversion (Conjurer, Healer) |
| `--aim off` | the ch.29 miss budget |
| `--block off` | the held guard (the class row sets `ClassGate::block`, MFO-B827) |
| `--range off` | H4: the distance a mage fights at, and the mage back-off |
| `--backoff off` | the Mercenary's back-off |
| `--step off` | the hidden step (gap G18) |
| `--c13 lever` | the free stealth entry as a real guarantee (card H10, not built). Default `built` |
| `--unweighted off` | spells with no effect IAUS can value stay in the pool |
| `--reanimate off` | reanimate spells are dropped again |
| `--disengage-dnd` | OFF by default. marth's variant: a disengage drops him as a valid target for 6 s. Not built in the game |

Other new flags: `--aim-miss P`, `--aim-scatter P`, `--dodge-flight S`, `--no-fill-swing`, `--teach <token>`,
`--disengage-report N`, `--steal-lab N`.

### 31.2 The model, row by row

FACT = read in the named source at 33c643f (code is a contract, not a field observation). MODEL = the lab's own choice.
Nothing here is FIELD.

| Mechanism | FACT and its source | MODEL |
|---|---|---|
| Feet (Q-SIM-5) | marth 11-11: "why wouldnt a follower be able to move and do an action. I believe he should". No game or MFO rule stops his feet | while his hands are busy with a swap, a potion or a cast he makes every deliberate movement the lab's engine AI would make: Retreat, a move order, a G3 run, the approach, the standoff and the fallback (`World.h Feet`). Full speed: no sourced slowdown value exists in Docs/research, so none is applied. Followers only. Foes are as before |
| C8 style | the class CSTY applies in full with the tree on (Q-melee-6, Harbinger v45). Values from `MFO_GenerateESP.py class_csty_params` and `CLS_AVOID_THREAT` (Q-S-3, SET on every stance: Mercenary line 0.35 / 0.50 / 0.55, Thief 0.25, Master Thief 0.30, Knight line 0, others 0.20, ranged +0.10; `RealClasses.cpp` mirrors them by hand; baseline_builtin.txt re-baselined: 79.52 at g3 unchanged, 77.48 at `--stealth off` from 77.44). Tier 1 has no package | the lab already read the stance styles. Which style fields move the lab's engine AI is the model table of section 4 |
| C8 ch.18 | `apmf/AttackMix.cpp`: deny only, over the last 8 STARTED attacks. Above the ceiling Power is denied (Knight 0.25, Mercenary 0.15, Thief 0.20). Under the floor Normal is denied, and that deny is lifted when no Power attack could be drawn (Warrior 0.40). A class with no rule files nothing | the engine's own power draw is the lab's (12% x the style, more against a blocking or staggered foe). Before this section the lab let only a class with a power row or share power attack by itself: that was the lab's assumption, and dropping it is not card C8. No share is read, so nothing is denied, until 8 started attacks fill the window (Harbinger's `Share()`, review F3; `--mix-partial` is the other reading). Under the floor a denied Normal BECOMES a Power attack whenever stamina can pay, which is the strongest reading: in the engine the roll among the undenied attacks can fail. A power attack IAUS asks for counts in the window and is not denied by it |
| C8 ch.23 | `classes/Package.cpp PackageLeashScale`: `fClassLeashMult` 0.5 for a tier 2+ class flagged short leash or near player (Knight, Healer in T2), never under `fLeashMin` 512. Every managed follower is leashed to the player (`apmf/PursuitLeash.cpp`) | the radius before the factor is 2256, the middle of `fLeashMin` 512 and `fLeashMax` 4000: the lab has no confidence value. Outside it with his target outside it too, he walks back |
| Dodge | batch M research (Q-melee-7): the engine's side step rolls against the style's Avoid Threat, and a class style reaches the engine since v45 | one roll per projectile launched at a follower with a class package. Not while his own blow, shot or cast is committed. The projectile must fly at least 0.2 s (`--dodge-flight`). A rolled step clears a non-area projectile and costs him 0.4 s. A blast, a stream and a touch are not stepped out of. Foes keep the old flat miss |
| H2 aversion | `apmf/AversionClaims.cpp`: weight 0.25 for the EQUIPPED Conjurer or Healer (Q93, Q94), Dragonborn keeps a mastered one at full (Q107 default). Harbinger: when a foe's pick lands on him, in that share of 6 s windows it takes its next best valid target. With none he is picked as always. The next best must pass the game's own tests (H2-1, Q101) | the tests in the lab: up, not fleeing, reachable for a melee-only foe. The draw per foe, follower and window is a hash of the fight's seed, so it never moves another draw |
| H5 miss budget | `apmf/AimClaims.cpp`: over his last 10 shots, when the misses plus the shots in flight reach the budget (Archer 5), the next launch is re-aimed at the engine's unscattered solution | a corrected shot skips the lab's miss roll (it still stops at a pillar). The UNCORRECTED miss chance stays the lab's own: 12% against a moving target (`--aim-miss`), plus `--aim-scatter P` for a flat scatter (default none). The engine's real scatter is not measured |
| Block | `apmf/BlockClaims.cpp`, MFO.ini: one raise holds 2.5 s, the row's cooldown is 4 s, one renewal at most, 1.5 s of rest after any hold. A held guard denies all his own offense, casts included (Q-melee-3). Rows: Knight, Great Knight, Ebony Warrior (Q-melee-2). `iaus/Generate.cpp`: a Block candidate needs the class row, the road, a melee weapon and a foe's swing in the air. Its score is 0 unless the time-to-live estimate is known | "his hands can block" = a melee weapon is drawn. How much a guard takes off a hit is the lab's block row of section 4. The lab fills `swinging` and the estimates, which the game ships OFF (`bIausFillSwing`, `bIausFillHit`): `--no-fill-swing --no-estimates` is the game as shipped |
| Power attack | Q82 and Q-melee-4: the move goes through Harbinger ch.31, and a refusal gives the cooldown back | the lab's power attack was already the engine's (real damage, real stamina). A refusal is a declined fire, which never stamped the cooldown. Refusals are now counted (`powRefused/f`) |
| H4 range | `iaus/Reposition.h SpellRange`, called as is: touch = melee reach x0.8, a stream = range x0.8, aimed or lock-on = range x0.9, and a minimum of 0.4 of that for a spell cast from afar. Closing when the foe is beyond the distance x1.15 (`apmf/RangeApproach.cpp`). The claim is a BOUND ("stay inside R"): it can make him close, it never holds him back. The mage back-off is `MageBackOffNeed`, called as is | "the spell in his hand" = the last book spell he cast. The distance beyond which he closes is the SMALLER of the lab's own standoff and the H4 bound, so H4 only acts for a touch spell or a short stream. The lab's spell reach is the record's range from 300 up, else 4096 as a stand-in for the projectile's (not extracted) |
| Mercenary back-off | `Reposition.h PressedBackOffNeed`, `SpellBook.cpp GateFor` (Mercenary only), `Concealment.cpp` Kind::BackOff: picked like a flee, denies nothing, ends on arrival or after 5 s, 6 s family cooldown | on the way he swings at a foe already inside his reach when his blade is free |
| Hidden step (G18) | `Reposition.h StepTarget`, `Concealment.cpp PickStep` mirrored (10 candidates at 0, 25 and 50 degrees on rings of 200 and 420, at least 100 units nearer, never beside another foe, a bow needs a line from the foe's eye). Quiet, ends the moment a foe's group has him, on arrival, or after 8 s. 3 s family cooldown | the step moves at the lab's sneak run, so the detection formula scores it as running while crouched |
| C13 as built | `classes/StealthEntry.cpp`, Q112: no engine lever. The counted fight only changes how MFO reads a foe with no combat group | the lab has no such foe (one group, a record from the first pass), so as built the entry does nothing. `--c13 lever` keeps the old guarantee for comparison |
| Unweighted spells | `iaus/Prune.h EffectWeighable`, called as is: a spell with no effect of a known archetype is dropped | the built-in pool has none. LoreRim drops 35, Tuxborn 66, Vanilla 2 |
| G7 corpse and reanimate | `Vocabulary.h cond.corpse_within`, questions 102 to 104: a dead humanoid in range, the spell must be able to raise it, highest level first, nearest on a tie, combat only. `iaus/Prune.h`: a Reanimate spell has no IAUS family | table mode only. Humanoid = not a creature, undead, daedra, dragon or animal. A body is raised once and one thrall stands at a time. The thrall is the dead foe's own spec at full health for the spell's duration. The built-in reanimate spells are fixtures with the shape of the vanilla ladder (caps 6, 13, 21, 30, 40) |
| Teach Spells | `Board_Classes.cpp`: a tome the IAUS can weigh is taught to a casting class | `--teach <token>`: the subject of a casting class also knows one such spell from the weighable pool |
| G13 / G14 pre-check | 31.8 | 31.8 |

### 31.3 Baseline (built-in world, vanilla profile, `--seeds 1,2,3,4,5,6`, 24 fights per scenario)

`old` is the base binary at 33c643f (equal to `--t2 off`). `new` is the default now. `shipped` is the game with its switches
at their defaults: `--stealth off --no-fill-swing --no-estimates`. The last column is the default minus the default with that
one road off, so it carries the road's interactions with the others.

| Class | old g3 | new g3 | old `--stealth off` | new `--stealth off` | new shipped | What a road is worth in the default |
|---|---|---|---|---|---|---|
| melee | 79.08 | 79.99 | 79.08 | 79.99 | 80.15 | the engine's own power attacks +0.60 |
| ranged | 63.18 | 65.71 | 63.18 | 65.71 | 66.48 | feet +2.50, power attacks and leash +1.05 |
| mage | 77.47 | 78.81 | 77.47 | 78.83 | 79.41 | feet +1.41, power attacks and leash +0.80 |
| knight | 68.80 | 70.26 | 68.52 | 70.36 | 68.83 | block +0.83, C8 +0.38, feet -0.28 |
| warrior | 87.81 | 88.25 | 87.72 | 88.21 | 88.21 | C8 +0.35, feet -0.08 |
| mercenary | 81.03 | 81.81 | 84.28 | 85.18 | 85.32 | feet -0.34, back-off -0.11, dodge +0.03, C8 -0.01 |
| archer | 78.31 | 78.33 | 78.31 | 78.33 | 78.44 | C8 -0.53, feet -0.32. The miss budget never engages |
| thief | 74.93 | 80.14 | 39.31 | 39.60 | 39.60 | the hidden step (+3.97 as the only road on, +0.33 in the default), feet +1.29 |
| evoker | 80.27 | 81.44 | 80.63 | 81.57 | 81.66 | feet +0.30 |
| conjurer | 70.26 | 74.50 | 69.30 | 72.42 | 73.16 | feet +2.55, power attacks and leash +2.42, aversion +1.70 |
| healer | 81.50 | 83.26 | 81.30 | 83.20 | 85.41 | C8 -0.55, aversion -0.05, feet -0.04 |
| warlock | 78.15 | 77.13 | 78.14 | 77.84 | 80.08 | feet -1.11, power attacks and leash +0.55 |
| all 12 together | 78.11 | 79.52 | 76.69 | 77.44 | 77.64 | feet +0.22, C8 +0.16, block +0.08, aversion +0.06, step +0.06, H4 +0.02, dodge +0.01, back-off 0.00 |

How to read the last column:
- Standard errors are as in 30.6 (1.5 to 4.5 per class, 0.4 for the 12 together). Every one-road number for the 12 together is
  UNDER that error, and so are most per-class ones. Read them as "no visible effect" unless they are well above 1.
- The columns share seeds and fights, but the pairing is partial: once a road changes one draw, the fight's later draws
  differ, so a paired difference still carries most of the fight-to-fight noise.
- "Power attacks and leash" is the `--c8` switch on a class with NO ch.18 rule (every T1, the mage line, the Archer). What
  moves those rows is the lab dropping its old assumption that such a class never power attacks by itself, plus the ch.23
  leash. It is not an effect of card C8's packages. The card's own effect is the Knight, Warrior, Mercenary and Thief rows,
  and it is small.

The default with one road off, all 12 together: `--feet-stop` 79.30, `--c8 off` 79.36, `--dodge off` 79.51, `--aversion off`
79.46, `--aim off` 79.52, `--block off` 79.44, `--range off` 79.50, `--backoff off` 79.52, `--step off` 79.46, `--c13 lever`
79.50, `--unweighted off` and `--reanimate off` 79.52, `--mix-partial` 79.53. Only the feet road on over `--t2 off`: 79.15.
The fills as shipped at g3 (`--no-fill-swing --no-estimates`): 79.73, Knight 69.36.

Lists, Beta 1 set, seeds 1 to 3: LoreRim 64.20 (63.13 with `--t2 off`), Tuxborn 69.74 (68.06).

Checks: `tools/iaus_sim/run.sh` PASS (BAD-FACTS 0, invalid picks 0) in IAUS mode, table mode, at `--stealth off`, with
`--parked`, over all scenarios, and on LoreRim, Tuxborn and Vanilla. `SIM_CLITEST=1`: 19 checks PASS (6 new).
`tools/iaus_harness/run.sh`: 0 failed. One address and undefined-behaviour sanitizer run of the Beta 1 cells on the final
code: clean. The oracle is draw for draw with the base under `--t2 off` (three scenarios,
byte-equal output), and so is `--t2 off --roads bash` (review F1: the foe-casting fill had slipped behind a comment).

### 31.4 Findings

1. **Moving feet are worth little in total, and up to 2.5 J either way per class.** Alone over the old lab the road is
   +1.04 J. In the default it is +0.22: Conjurer +2.55, ranged +2.50, mage +1.41, Thief +1.29, Warlock -1.11. A mage who
   closes while he casts is nearer the blades, and a mage out of magicka walks in with his dagger while he drinks, because
   that is what the lab's engine AI does with a free body. The asymmetry of MFO-B783 is gone either way: Retreat, the flee,
   the approach and the standoff all keep moving. The number depends on WHAT the feet do: a first version that kept a
   caster's standoff during every cast, also with no magicka left, scored 0.75 J higher for the 12 together.
2. **The Knight does not block under IAUS.** With the lab's fills ON he raises his guard 0.08 times a fight (0.5% of his
   time), because `BlockNeed` is 0 until his time-to-live estimate says he is in danger (`t.blockBase` 0). With the fills as
   shipped he never does. In TABLE mode his row guards 33% of his time and that costs him: natural U 76.33 with the row,
   78.10 without (`--block off`), because a held guard denies his own offense for 2.5 s of every 4. "Blocks a lot" is
   reachable by tuning `t.blockBase` and the Block family's weight, and the lab says it is not free. Question Q-S-5.
3. **Aversion at 0.25 is mild.** It turns away 0.6 picks a fight for the Healer and 0.5 for the Conjurer (natural
   bracket). Conjurer +1.70 J, Healer -0.05: the foes it turns away go to the player and the others, and the objective
   counts the whole party, so a gain for him can be a loss for them.
4. **The Archer's miss budget never engages in the lab.** The lab's own miss chance leaves him hitting 88 to 90% of his
   shots, and a budget of 5 in 10 needs half of them to miss. With `--aim-scatter 0.5` about 11% of his shots are corrected
   and the budget is worth +0.37 J (77.46 against 77.09), at 0.6 +1.31, at 0.3 +0.07. Whether the game's scatter is anywhere
   near that is what the `[aim]` probe measures. Question Q-S-4.
5. **The ch.18 rules barely act in lab fights, and this finding depends on the model.** Harbinger reads no share until 8
   started attacks fill the window. A lab fight rarely gets that far with stamina left: the Warrior's floor denies Normal 0.6
   times a fight and is lifted 1.6 times for want of stamina, and his power attacks stay at 14% of his time, the same as
   with no rule. The Knight's ceiling denies 0.08 power attacks a fight, the Mercenary's 0.13. Under the other reading
   (`--mix-partial`: the share is read from the first attack) the floor denies 1.5 times a fight and his power time is 26%,
   for +0.11 J. And the lab's floor turns every denied Normal into a Power attack when stamina allows, where the engine's
   roll can fail. So the lab cannot say how hard the floor bites in the game: field step 24s can.
6. **H4 is neutral in the lab.** Native H4 is a bound: it can make a mage close, it never holds him back. The lab's own
   standoff (close beyond 1050 units) is tighter than the bound for every aimed spell, so H4 only acts for a touch spell
   or a short stream: 79.52 with it, 79.50 without, no class moves by more than 0.3. The mage back-off almost never fires (0
   to 0.05 runs a fight: a foe that has to close must be inside 0.4 of the spell's distance, on him). The lab's spell
   reach is still a stand-in (MFO-B833), which now matters only for that minimum.
7. **The Mercenary's back-off arrives and shows no gain here.** 0.15 runs a fight, all reach the point, -0.11 J (noise). The
   side step clears 31% of the projectiles it is rolled for (0.2 a fight), +0.03 J. `--dodge-flight` 0 / 0.2 / 0.35: 81.81 /
   81.81 / 82.25. Under `--mix-partial` the same class reads 82.31, so the half J these two roads showed in the first run of
   this section was the fight changing shape, not the roads.
8. **The hidden step repairs gap G18.** As the only road on: Thief 74.93 to 78.90. In the default it adds +0.33 on top of
   the moving feet, which remove much of the same standing still. 0.22 steps a fight in the natural bracket, 92% arrive, 8%
   end because a foe's group found him on the way.
9. **C13 as built is worth nothing either way.** Thief 80.14 as built, 80.05 with the lever: inside the noise. The lab cannot
   show a case for card H10 at these numbers.
10. **G7 almost never fires from a table.** IAUS has no Reanimate family, so only a table row casts it. In level-matched
    fights the corpse row could not be paid for on 78% of the laps a humanoid corpse lay there (the attack row had spent the
    magicka by the first kill), and on 22% the spell's level cap was under the corpse's. One raise in 120 strong fights. The
    Necromancer's own default row is still a Todo row in `ClassTable.cpp` (T3, not T2): MFO-B834.

### 31.5 What batch S did not model, and why

- A slowdown of his feet while casting or drawing: no sourced engine value.
- The engine's projectile scatter: not measured (31.4 finding 4).
- Dodge for foes by their own style: they keep the lab's flat 12% miss, so the old numbers stay comparable.
- A dodge framework's own dodge (Ultimate NPC Dodging on LoreRim): the lab models the vanilla side step only.
- Pathing in the aversion's next-best test, and a second foe group.
- G16 (equip only) and the theft fix: no combat model, n/a as in the manifest.
- MFO-B786 and MFO-B787 stay open: fixing them changes the reference lab of section 30, which `--t2 off` must reproduce.

### 31.6 G7 in table mode

```
rows="cond.corpse_within|act.cast_target|0|15|0;cond.foe_any|act.cast_target|0|1|0;cond.self_mp_pct_below|act.drink_magicka_potion|0.25|0|0"
tools/iaus_sim/run.sh --no-json --mode table --parked --classes necromancer --fights 24 --teach reanimate --rows "necromancer=$rows"
```

`raise/f` is 0 in the natural bracket and 0.008 in the strong one (one raise in 120 fights). U moves by under 2 between the
run with the spell and the run with `--reanimate off` (68.87 against 68.03 natural), which is his book being drawn
differently, not the thrall.

### 31.7 Sensitivity

| Switch | Class | J |
|---|---|---|
| `--no-fill-swing --no-estimates` | knight | 69.36 (70.26 with the lab's fills) |
| `--mix-partial` | warrior / mercenary / knight | 88.36 / 82.31 / 70.48 (88.25 / 81.81 / 70.26 in the default) |
| `--aim-scatter 0.3 / 0.5 / 0.6` | archer | 77.85 / 77.46 / 77.45 with the budget, 77.78 / 77.09 / 76.14 with `--aim off` |
| `--aim-miss 0.3 / 0.5` | archer | 78.19 / 77.90, the budget never engages |
| `--dodge-flight 0 / 0.2 / 0.35` | mercenary | 81.81 / 81.81 / 82.25 |
| `--c13 lever` | thief | 80.05 (80.14 as built) |

### 31.8 Cards G13 and G14: the witness pre-check against the detection formula

`tools/iaus_sim/StealLab.h`, `run.sh --steal-lab 20000`. The pre-check (`logistics/Steal.cpp Evaluate`) is geometric: a sight
ray in a cone, a heard noise, plain nearness, the owner's line of sight, a guard. The engine decides a witness by detection.
The lab draws town scenes (0 to 6 actors, 15% of them guards, an owner in half the scenes that have anyone, walls, the player
200 to 1500 units away, sneaking or not) and asks both. FACT and MODEL are listed at the top of the file. The engine's
witness loop is not read yet (`theft-and-crime-alarm-re.md`), so "the engine" here is the detection formula applied to
every actor inside `iCrimeAlarmRecDistance`, for the follower and for the player.

| Light | Scenes | Take | Take, an actor detects the follower | Take, an actor detects only the player | Refused | Refused with nobody detecting |
|---|---|---|---|---|---|---|
| dark (the Thief's row: Dark -> Steal) | 9402 | 16.8% | 0.00% | 1.46% | 83.2% | 44.5% |
| dim | 6635 | 15.9% | 0.00% | 0.09% | 84.1% | 54.4% |
| bright | 3963 | 16.3% | 0.00% | 1.71% | 83.7% | 11.8% |

Refusals by reason (dark): a guard 38.5%, an actor senses the follower 37.9%, an actor senses the player 12.6%, he stands
5% of the time by construction 6.0%, the owner sees 5.0%.

Reading: the pre-check never let a take through that the formula says an actor detects the FOLLOWER for. That 0.00% is
largely BY CONSTRUCTION and not evidence: both sides use the same view cone and the same line-of-sight geometry, and the
formula's sight term needs exactly the cone and the line the pre-check already refused on. What the row can show is only
that sound and the skill duel alone never detected a sneaking follower the geometry had cleared, in these scenes. In 1 to 2% of the
allowed takes an actor detects the PLAYER by sound from behind a wall (he walks crouched: the lab's reading of Harbinger's
hearing calls that silent, the engine's formula still scores it). That rests on two MODEL rows, so it is a question for the
`bCrimeGoldProbe` field run and not a defect. The price of failing closed is high in the dark: about half the refusals
would have gone unseen. At 1.3 sight pairs a check the 8-pairs-a-frame limit never bites. G14 opens unowned chests only
(Q-L-6), and its owned-lock path asks this same check.

### 31.9 How useful is card G3, and marth's D&D variant

`run.sh --disengage-report 24 --seeds 1,2,3,4,5,6`. The same fights three ways: no hide run and no flee (every other road
stands), the run as built, and `--disengage-dnd` (from the pick he is no valid target for 6 s while anyone else stands:
ch.32 at full weight on every foe. Not built in the game). Each class plays 144 natural fights and 144 of each of its own
scenarios. A fight is "touched" when a run began in it.

| Class | Mode | U | Downs per fight | Taken per s | Runs per fight | Run length s | Safe within 20 s | Time to safe s | Health lost on the run | Touched fights | U change in touched | Touched fights that end differently | Downs change in touched |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| thief | none | 78.09 | 0.10 | 3.00 | | | | | | | | | |
| thief | g3 | 77.97 | 0.10 | 2.84 | 0.30 | 0.70 | 100% | 0.05 | 0.0% | 29.9% | -0.45 | 7.9% | 0.00 |
| thief | dnd | 77.98 | 0.10 | 2.84 | 0.30 | 0.70 | 100% | 0.06 | 0.0% | 29.9% | -0.41 | 7.9% | 0.00 |
| evoker | none | 85.14 | 0.17 | 3.33 | | | | | | | | | |
| evoker | g3 | 85.70 | 0.12 | 3.39 | 0.25 | 1.34 | 84.7% | 1.22 | 1.0% | 19.6% | +2.87 | 38.4% | -0.23 |
| evoker | dnd | 86.28 | 0.04 | 2.83 | 0.26 | 1.34 | 93.2% | 0.68 | 1.1% | 19.6% | +5.82 | 56.6% | -0.62 |
| conjurer | none | 71.60 | 0.10 | 5.89 | | | | | | | | | |
| conjurer | g3 | 72.09 | 0.07 | 5.83 | 0.16 | 1.09 | 56.5% | 2.54 | 1.2% | 16.0% | +3.07 | 30.4% | -0.17 |
| conjurer | dnd | 72.12 | 0.03 | 5.33 | 0.16 | 1.09 | 73.9% | 1.45 | 0.9% | 16.0% | +3.24 | 47.8% | -0.39 |
| healer | none | 86.88 | 0.06 | 4.77 | | | | | | | | | |
| healer | g3 | 86.96 | 0.05 | 4.77 | 0.10 | 1.17 | 61.9% | 2.41 | 3.0% | 9.7% | +0.73 | 19.0% | -0.10 |
| healer | dnd | 87.40 | 0.02 | 4.60 | 0.10 | 1.12 | 81.8% | 1.68 | 2.4% | 10.0% | +5.26 | 46.5% | -0.47 |
| warlock | none | 74.77 | 0.17 | 6.29 | | | | | | | | | |
| warlock | g3 | 75.81 | 0.14 | 6.23 | 0.21 | 1.15 | 60.0% | 3.80 | 5.1% | 20.1% | +5.16 | 31.0% | -0.17 |
| warlock | dnd | 74.86 | 0.06 | 6.16 | 0.20 | 1.27 | 82.8% | 2.81 | 4.3% | 20.1% | +0.46 | 51.7% | -0.59 |

"Safe" = the first moment after the run began with no foe within 400 units of him, he still up, inside 20 s. The time is the
mean over the runs that got there. Each class row is 288 or more paired fights and its touched fights number in the tens, so a U
change in the touched fights under about 4 is not a finding by itself. The down counts are steadier.

**The `dnd` rows are an UPPER BOUND on the variant, not a forecast of it.** The lab makes every foe that has him re-pick at
once, at weight 1.0, and the 6 s window runs on after a short run has ended. Harbinger's aversion acts only when the engine
next selects a target (its own 1 to 3 s cadence) and its weight is capped at 0.95. A built variant can do no better than
these rows and will likely do less.

Reading:
- **As built the run is short and worth a little.** A flee lasts 1.1 to 1.3 s (it ends at the 30% health line, on arrival or
  after 10 s) and begins in 10 to 20% of a caster's fights. Over all fights it moves U by +1.0 (Warlock), +0.6 (Evoker), +0.5
  (Conjurer), +0.1 (Healer): inside the noise. What it does steadily is cut his own downs in the fights it touches, by
  0.10 to 0.23. The Thief's hide run always ends hidden (the engine loses him on its next pass, 30.7 finding 3) and buys
  nothing.
- **He is often not safe after it.** Evoker 85% safe in 1.2 s. Conjurer 57%, Healer 62%, Warlock 60%, and those take 2.4 to
  3.8 s: the foes follow him. He loses 1 to 5% of his health on the run and goes down on up to 13% of the runs.
- **The D&D variant, at its upper bound, does what it says for HIM.** Safe more often (74 to 93%) and sooner (0.7 to 2.8 s),
  and it cuts his downs in the touched fights two to five times as much as the run as built (-0.39 to -0.62).
- **For the party it is mixed.** Against the run as built: Evoker +0.6 U, Healer +0.4, Conjurer level, Warlock -1.0. The foes
  he sheds go to the player and the others. In the headline set the variant is 79.73 against 79.52 (Healer +1.12, Evoker
  +0.35, Conjurer -0.89, Warlock -0.48).
- **The numbers lean on the model.** Across three versions of the feet and range models in this round the Evoker's and the
  Warlock's U changes switched sign. What held in all of them: short runs, he is often not safe after, the variant makes
  him safer and cuts his downs by half or more. Whether that is worth building is a design call and the field session's,
  not the lab's. Question Q-S-11.

### 31.10 Gap ledger and backlog, changes

| id | Change |
|---|---|
| C-C8, C-H2, C-H5, C-POWER, C-H4 | modelled here: closing, not proven in the field |
| C-BLOCK | modelled. Open as a tuning item: IAUS never blocks at `t.blockBase` 0 (finding 2) |
| C-NOFLEE | Mercenary: closed by the back-off. Knight and Warrior hold the line by design |
| G18 | the hidden step is modelled and repairs it (finding 8) |
| G19, G20, G22 | unchanged |
| G21 | Mercenary closed (back-off). Other classes without the flag: by design |
| C-G7 new | IAUS has no Reanimate family, and a table row rarely gets to cast it (finding 10). Open, T3 |
| MFO-B783 | closed: the feet stop is gone, `--feet-stop` keeps the old lab |
| MFO-B827 | closed: the class row sets `ClassGate::block` and its cooldown |
| MFO-B784, B785, B788 | text corrected (the comments and this document) |
| MFO-B786, B787 | open: fixing them would change the reference lab |
| MFO-B833, B834, B835 new | The spell reach is a stand-in (now only the back-off minimum). The Necromancer row is still Todo. Dodge and miss models have no field anchor |

Correction to 30.7 finding 4 (MFO-B784): main's 3db7882 keeps the stance until the blow RESOLVES, so the lab's default
(the stance outlives the blow) is the game's behaviour and `--blow-stands` measures a reading the game does not have.
Correction to 30.3 (MFO-B785): the `steep` profile's game settings are equal to the cached LoreRim extract. It is lab test
input, nothing in `native/` reads it, and it should be read as "LoreRim's settings", not as a hand-written shape.
Stated limit (MFO-B788): `Detect.h` takes "flying" from `outOfMeleeReach`, which triples the detection range against any
actor out of melee reach. It errs against the follower.

### 31.11 Handoff to the tuning round (replaces the numbers of 30.10)

The world is fixed for this round. Do not edit `tools/iaus_sim/*.h`, `sim.cpp` or `native/`. Rules as in 30.10, with these
baselines and gates:

```
# 0. build once, check the baselines
tools/iaus_sim/run.sh --no-json --classes beta1 --seeds 1,2,3,4,5,6                 # 79.52
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --classes beta1 --seeds 1,2,3,4,5,6 --stealth off   # 77.44
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --classes beta1 --seeds 1,2,3,4,5,6 --stealth off --no-fill-swing --no-estimates   # 77.64 (the game as shipped)
SIM_NOBUILD=1 tools/iaus_sim/run.sh --no-json --classes beta1 --seeds 1,2,3,4,5,6 --t2 off        # 78.11 (the lab of section 30)
# per class      melee ranged  mage knight warrior mercenary archer thief evoker conjurer healer warlock
# g3 (default)   79.99  65.71 78.81  70.26   88.25     81.81  78.33 80.14  81.44    74.50  83.26   77.13
# --stealth off  79.99  65.71 78.83  70.36   88.21     85.18  78.33 39.60  81.57    72.42  83.20   77.84
# shipped        80.15  66.48 79.41  68.83   88.21     85.32  78.44 39.60  81.66    73.16  85.41   80.08
```

Gates on every kept change. All three are REQUIRED (review of a645ca2 and of 0880667):
1. no loss at g3 (`--classes beta1`, 79.52): the tuning target.
2. no loss on `--stealth off --classes beta1` (77.44): the game ships `bSneakInCombat` and `bIausHideRoad` OFF.
3. no loss on the game AS SHIPPED, as its own gate: `--stealth off --no-fill-swing --no-estimates --classes beta1` (77.64).
   Passing 2 and a fills-off run at g3 separately does not stand in for it.

For every gate print the 12 classes one by one (`--classes <slug>` per class, same seeds) and hold each class to its own
band: no class may fall more than 1.0 J under its row above, and the Thief and the Mercenary no more than 0.5 J. Those two
have the largest gap between their columns (Thief 80.14 against 39.60, Mercenary 81.81 against 85.18), so a change that
helps one column can hide a loss in the other inside the 12-class mean.

The feet asymmetry is gone (MFO-B783 closed), so step 1 of 30.10 may search `aw.4` (Retreat) against `aw.21` (the flee)
and `c.63.*` again. New things to search, in this order:
1. Knight block: `t.blockBase`, the Block family's weight, the row's cooldown (`run.sh --list-params` for the indexes). Target: the
   playstyle line `b.block` at least 15, without losing J. Check `--mode table` too (the row costs 1.8 U there).
2. Mercenary back-off: the BackOff family's weight, `t.backOffCooldownSec`, the need table is a constant in `iaus/Reposition.h`.
3. Hidden step: the Step family's weight, `t.stepCooldownSec`.
4. The flee against Retreat (30.10 step 1), now on equal feet.
Do not tune against `--disengage-dnd`, `--c13 lever`, `--aim-scatter` or `--aim-miss`: they are not the game.

### 31.12 Addendum 2026-11-12: the pre-T2-session changes (tuned Step weight, H4 hands, the Knight line's guard)

1. **Step weight** `aw.23` 0.9 -> 1.197 (native default, `Catalogue.cpp`). Gates, 6 seeds: g3 beta1 79.52 -> 79.61, `--stealth off` 77.44 =, shipped 77.64 =. Only the Thief rows of `baseline_builtin.txt` moved (regenerated).
2. **Q-I-2, H4 from both hands** (`Reposition::HandsRange`; sim `--hands off|on`, default on, `--t2 off` includes it). The range is the SHORTEST of the equipped scoreable spells of one delivery type; touch + distance together keep the distance range unless the touch spell is the cast IAUS chose on its last decision (`Snapshot::chosenCast`; marth: touch range only when the touch spell is the chosen cast), and for 3 s after the touch cast (`Snapshot::touchSpent`) the mage back-off takes him out, foe on him or not. Sim model: a second hand holds the spell cast before the last (`Ctl::handSpell2`). `--hands off` reproduces 79.61 / 77.44 / 77.64. On: g3 79.63, 77.44, 77.64; Mage 78.81 -> 78.97, LoreRim beta1 seeds 1-3 64.22 -> 64.23. The built-in fixtures rarely put a touch spell next to an aimed one, so the delta is small: the field step 3b is the check.
3. **Q-S-5, the Knight line's guard under taunt.** Tally (World.h `inc2` / `guard2`, printed as `GUARD UNDER TAUNT`, metrics `guard2%`, `inc2/f`): of the melee blows that LAND on the subject while two or more foes are taunted onto him (`tauntBy == him`, live taunt), the share that meet his guard (`blockUntil`, IAUS-held or the engine's own). Before: 46% knight (boost 1, swing-gated Block). Two changes reach it: (a) `Cons::BoostGuardLine` (APPENDED, `Bin(1, 2.0)`, Block only) plus `BlockNeed` = 1.0 and a Block candidate generated for a taunted foe on him in striking distance WITHOUT a seen swing (the swing flag needs `bIausFillSwing`, shipped OFF); (b) a standing guard's renewal carries no row cooldown (2.0 s from the raise against a 2.5 s window left half a second to renew in): 56% -> 70% (with the x2.0 boost kept). Result, 6 seeds, blows-weighted over the brackets: knight 69.5, great-knight 69.5, ebony-warrior 69.9 (shipped config `--stealth off --no-fill-swing --no-estimates`: 68.4 / 70.9 / 68.8). The boost value is flat above 1.5 (share 68.4 at 1.5, 69.5 at 2, 69.6 at 3); 2.0 is kept because it stays under a health potion (2.2) and a heal cast (2.4). The ceiling is Harbinger's constants (2.5 s hold, one renewal, 1.5 s rest = 77% of the time), not changed here.
   Gates, 12 classes per gate (the 31.11 bands hold; Knight +0.1 / +0.1 / +0.0): g3 melee 80.03 ranged 65.71 mage 78.97 knight 70.38 warrior 88.25 mercenary 81.81 archer 78.33 thief 81.02 evoker 81.44 conjurer 74.51 healer 83.25 warlock 77.13; `--stealth off` 79.99 65.71 78.83 70.44 88.21 85.18 78.33 39.60 81.57 72.42 83.18 77.84; shipped 80.15 66.48 79.41 68.86 88.21 85.32 78.44 39.60 81.66 73.11 85.41 80.09.
4. **Review round of 24b4421.** (a) The guard line is counted from the knight's OWN taunts (`Combatant::tauntedByMe`, `Taunt::TauntedBy`), as the sim tally; (b) it is OFF below `kDrinkHealthLine` (0.34) with a health potion in stock, so a hurt Knight drinks first; (c) the H4 touch range follows the cast IAUS chose on its last decision, not a damage proxy. Re-run: gates identical in the beta1 means (79.63 / 77.44 / 77.64), Knight 70.35 / 70.41 / 68.86; share of blows meeting the guard under 2+ taunted, 6 seeds, blows-weighted: knight 67.9, great-knight 67.9, ebony-warrior 67.5 (min over seeds 63.3); shipped config 67.6 / 68.7 / 68.6 (min 63.4).


## 32. The sim batch: tree-OFF base cells, the rating battery, the per-class gap, health decline, the edge-number search (2026-11-12)

Branch `feat/sim-batch` on main 9f328c6. Fable author (sim structure is Fable-only). Changes `tools/iaus_sim`, `tools/iaus_dash`
and docs. Proof levels as in 31.2: FACT = read in the named source, MODEL = the lab's own choice. Nothing here is FIELD.

### 32.1 The tree-OFF base cells

marth 11-12: "I dont see the 3 base classes with no tree in the sim. I imagine theyd say Line: Base", "They do technically have
weapon gates (mage knife etc), and should still loosely follow tree on versions. They just have all available options at thier
disposal".

`RealClasses.cpp` appends `melee-off`, `ranged-off`, `mage-off` after the 35 table rows (indexes 35 to 37, so every table class
keeps its index and its fights). Line `Base`, tier 1. Each is a copy of its tree-ON T1 row (family, kit, default kit rows, the
spell tokens of its rows: the mage keeps dagger and staff), then its gate is the tree-OFF branch of `SpellBook::GateFor`
(FACT, `iaus/SpellBook.cpp`): any known spell, power attack, taunt, retreat, `scrollUser` by the T1 root, no row cooldown (the
`Tuning` rowless floors apply), no sneak class, no pressed back-off, no Block row. Curve row = `ClassCurves::kBaseOffFirst` + root.
MODEL: no class package and no class flag, so the style is MFO's stance style with the vanilla Avoid Threat 0.20 and without the
`kDisengage` fallback offset (`SimClass::csDisengage`); tier 1 rolls no dodge in the lab, as every T1.

`--classes beta1` is now 15 cells (the 12 and the 3 base cells). `--classes beta1-tree` is the 12: every J of sections 30 and 31
is a `beta1-tree` number and the sim batch's own changes leave it unchanged (79.63 / 77.44 / 77.64, checked draw for draw on the
built-in world and on LoreRim 64.21, Tuxborn 69.81, Vanilla 82.70 against the base binary). The School Circle then moves it by
design (32.7). `--curve melee-off:...` addresses a base
row. `--opt c.N` next to a `--curve` on consideration N is refused (the override is a copy of the shared curve).

### 32.2 The rating battery and the Rating (Tx) column

marth 11-12: "id also like the third comlumn to be a rating(Tx) column. so we can easily see if a class in a tier is less
effective than another. which means scenarios will have to be even more diverse".

`Rating.h`. A cell is (foe archetype, group size, level band): 10 archetypes (swarm, archers, mages, healer-backed, boss, undead,
big creature, ambush, kite, mixed) x 2 sizes x 2 bands = 40 cells, N fights each (`--rating N`, 3 in a class report run).
A fight is a function of the tier, the cell, the fight index and the seed, never of the class:

| Identical for every class of a tier | How |
|---|---|
| level, and so the gear rung of every weapon and armor piece | the tier's band: T1 12 / 18, T2 22 / 28, T3 32 / 38, T4 42 / 48 (`Load.h LadderAt` reads the level only) |
| potions, arrows | 2 health, 2 magicka, 1 stamina, 60 arrows |
| the player, the second follower (large cells) | built from a stream the subject never draws from (`FightSpec::geoSeed`) |
| the foes, their strength | picked by role from the loaded world, scaled ONCE against a reference party whose follower is the T1 melee row (`FightSpec::refCls`), so the scale never reads the rated class |
| places, cover, light, the fight's opening random state | `geoSeed` |

What differs is the class: gate, kit, package, curves, and the spells its own default rows name. MODEL: the archetype strength
ratios (0.80 to 1.10) and the potion counts are the lab's choice. A list's actors are test input only: foes are picked by role
(`Data::roles`), a missing role falls back to the next and then to the first spec, and the built-in fixtures cover every role.

Score = mean Utility over the battery. Rank and distance from the tier mean are among the classes of the run. BELOW flag = under
the tier mean by more than 2.0 U and by more than two standard errors of the per-fight paired difference.

### 32.3 The per-class gap in every class report run

CAUSE of the empty Gap and Trend columns (coordinator, confirmed): a normal class run wrote no scenario rows and no `main_gap`,
and the ledger names groups. Now a run that writes a results file runs a GAP PASS by itself: per class 4 natural fights and 2 of
each of its own named scenarios, played by IAUS and by the class oracle on the same fights. Price: the oracle at K=6, 90 s
(the file records it), about 25 s of wall time for the 15 Beta 1 cells on 8 threads, 2 s for a one-class probe.
`--class-oracle N` / `--gap-scenarios N` size it, 0 turns it off, `--no-json` runs skip it. Results schema 2 adds `class_gaps`,
`rating` and `gaps[].classes` (the slugs of the run a ledger group covers). A 4-fight gap is noisy (se 1 to 20 U per class):
read the Trend column as "same seed, same fights, did it move", not as a measurement of the gap's size.

### 32.4 Health decline and the drink record

`--decline N` (`sim.cpp DeclineReport`, tallies in `World.h Tally::dec*`). Per class and seed: N fights in each of the natural,
balanced and strong1 brackets and N of every rating cell. Band k = his health in [k/10, (k+1)/10) of his maximum.
MODEL choices: only seconds in which a foe has him as its target are counted; "too late to drink" = the damage of his last ~2 s
(`Body::recentDmg`, a 2 s decaying sum) is at least the health he has left; a potion takes 0.6 s from the start of the gulp to
its effect (`TM::drinkAt`) after a serve lap of up to about 1 s.

### 32.5 Rating, built-in world (`--rating-only --rating 3 --classes beta1 --seeds 1,2,3,4,5,6`, 720 fights per class)

| Tier | Class | Rating | se | Rank | vs tier mean | Flag | Weakest archetype (vs its tier there) |
|---|---|---|---|---|---|---|---|
| T1 | melee-off | 77.64 | 0.95 | 1/6 | +8.03 | | |
| T1 | melee | 76.24 | 1.03 | 2/6 | +6.62 | | archers -0.4 |
| T1 | mage-off | 72.51 | 1.14 | 3/6 | +2.89 | | boss -1.5 |
| T1 | mage | 71.76 | 1.16 | 4/6 | +2.15 | | ambush -1.6 |
| T1 | ranged-off | 62.45 | 1.35 | 5/6 | -7.16 | BELOW | beast -18.2 |
| T1 | ranged | 57.08 | 1.44 | 6/6 | -12.53 | BELOW | beast -26.7 |
| T2 | knight | 83.77 | 0.74 | 1/9 | +9.63 | | archers -1.1 |
| T2 | warrior | 81.92 | 0.83 | 2/9 | +7.78 | | |
| T2 | mercenary | 80.93 | 0.90 | 3/9 | +6.79 | | mixed -0.3 |
| T2 | thief | 75.08 | 1.20 | 4/9 | +0.94 | | mages -9.9 |
| T2 | healer | 74.36 | 1.18 | 5/9 | +0.22 | | kite -12.7 |
| T2 | conjurer | 73.38 | 1.20 | 6/9 | -0.76 | | kite -9.4 |
| T2 | evoker | 71.10 | 1.24 | 7/9 | -3.04 | BELOW | kite -16.7 |
| T2 | warlock | 70.60 | 1.24 | 8/9 | -3.54 | BELOW | kite -15.1 |
| T2 | archer | 56.12 | 1.44 | 9/9 | -18.02 | BELOW | beast -37.6 |

What the table can and cannot carry:
- The ranged line is last in both tiers, by 12 to 18 U, and it is the big creature and the brute on him that sink it (Archer 19.6
  against a tier mean of 57 on `beast`, 8.4 on `kite`). This is the same weakness the headline J has shown since section 30
  (ranged 65.7 against melee 80.0), now on fights that are the same for the tier. Whether the game's Archer is this weak is
  NOT shown: the lab's bow model (12% miss on a mover, no kiting drive, C-H3 open) is the first suspect, not the class.
- The base cells rate at or above their T1 rows on the battery (melee-off +1.4, mage-off +0.8, ranged-off +5.4), while their
  headline J on natural fights is lower for melee-off and mage-off (74.3 against 80.0, 67.7 against 79.0, 6 seeds, se 3 to 4,
  different fights for the two rows). The two measures disagree in sign for two of the three; neither difference is large
  against its error. No claim is made about which of a T1 row and its base cell is stronger.

### 32.6 Health decline, built-in world (`--decline 4 --classes beta1 --seeds 1,2,3,4,5,6`, 1032 fights per class)

Share of his maximum health lost per second while a foe is on him, all bands together: knight 2.9, mercenary 3.2, warrior 3.4,
melee 3.5, melee-off 3.6, archer 4.8, ranged-off 4.9, ranged 4.9, thief 5.2, healer 5.6, mage-off 5.6, warlock 6.0, mage 6.0,
evoker 6.1, conjurer 6.2. The curve is flat from 70% down to 20% for the weapon classes (3.3 to 4.9 %/s) and peaks between
60% and 40% for the mage line (7.5 to 8.4 %/s).

"Too late to drink" (the last 2 s of damage would down him), share of the pressed seconds:

| Health band | melee | knight | ranged | mage | evoker | conjurer |
|---|---|---|---|---|---|---|
| 40 to 50% | 0.2 | 0.1 | 0.1 | 2.0 | 2.2 | 3.1 |
| 30 to 40% | 3.4 | 1.2 | 3.3 | 9.6 | 8.7 | 10.6 |
| 20 to 30% | 16.9 | 10.5 | 18.9 | 27.1 | 23.3 | 31.3 |
| 10 to 20% | 37.9 | 22.3 | 32.2 | 56.5 | 53.6 | 58.6 |

Read off the curve: LAST-SAFE line (lowest tenth with under 10% too-late seconds) is 0.30 for every class but the Conjurer (0.40),
and the NO-WASTE line (1 minus the potion's share of his health, potions are 20 to 27% of it) is 0.73 to 0.80. Nobody wastes a
potion today (0.0% ran over). The shipped line, 0.34, sits just above LAST-SAFE: the curve does not ask for another line, it
only bounds it to [0.30, 0.73].

His drinks and downs:

| Class | drinks/f | drinks at | his downs/f | of them with a health potion in the kit | self heal known | party downs (he or anyone) while he held an affordable heal for that member /f |
|---|---|---|---|---|---|---|
| melee | 0.82 | 34% | 0.17 | 0.035 | 33% | 0 |
| knight | 0.71 | 34% | 0.09 | 0.025 | 33% | 0 |
| ranged | 0.63 | 33% | 0.27 | 0.046 | 34% | 0 |
| mage | 0.28 | 30% | 0.32 | 0.250 | 99% | 0.31 |
| evoker | 0.18 | 30% | 0.30 | 0.261 | 87% | 0.31 |
| conjurer | 0.15 | 30% | 0.19 | 0.172 | 87% | 0.21 |
| healer | 0.20 | 31% | 0.21 | 0.199 | 99% | 0.28 |
| warlock | 0.19 | 31% | 0.29 | 0.260 | 86% | 0.30 |

marth 11-12: "I imagine any mage line with heals would be able to use them? Not specifically restoration line." CHECKED: the
lab's mage-line cells already know a self heal in 86 to 99% of their fights (`BuildFollower` gives a mage-family caster one
spell of every nature he can afford, Restoration line or not) and fire 4 to 7.6 heal decisions per fight. Nothing was added to
the model.

OBSERVED: a mage-line follower who goes down nearly always still holds a health potion (0.25 of the mage's 0.32 downs per
fight, 0.26 of the evoker's 0.30). A weapon class almost never does (melee 0.035 of 0.17). The last column counts ANY party
member's down, so it cannot be set against his own downs: it says only that an affordable heal was in his book at about that
many downs per fight.
NOT ESTABLISHED: why. Two readings were tested and neither is supported by the objective:
1. "The potion is vetoed while a heal scores" (`Tuning::noHealDrink`): lifting the veto entirely (`t.noHealDrink=1.0`) is
   worth +0.05 J over the 15 cells (se 0.01). Real, and far too small to be the cause.
2. "He should drink earlier" (a later midpoint): section 32.7.
Untested readings: the heal's cast time against a 6 to 8 %/s decline; a mage dies in the two bands (20 to 40%) where a quarter
of his pressed seconds are already too late, so the error is made earlier, in where he stands, not in what he drinks.

### 32.7 The School Circle is in the lab (a fixed design input, never searched)

marth 11-12: "My settings there are truth. The school circle I just designed isn't something that should get tweaked".
FACT (`native/iaus/ClassCurves.cpp SeedSchoolPeaks`, main 8d3620f + this branch): the 14 rows with a home school carry floor
0.35, sharpness 1.5 and a mastered bump of 0.15; the Arch-Mage is any school; every other row is flat. On the circle
Destruction, Alteration, Restoration, Illusion, Conjuration the weight is 1.0 at home, 0.694 one step away, 0.369 two steps away.
Lab: `Load.h` fills `SpellEntry::school` from the spell record's skill (the costliest effect's, as the game reads it), and
`RealClasses.cpp` calls `ClassCurves::ApplyMastered` with the classes his unlock rule requires. `edge_search.py` has no school
parameter on purpose. Beta 1 home schools: Evoker Destruction, Conjurer Conjuration, Healer Restoration, Warlock Illusion.

What it does to the lab's objective (6 seeds, the gates of 31.11; se of a class 1 to 4.5, of a set 0.3 to 0.45):

| | g3 before | g3 with the circle | `--stealth off` | shipped |
|---|---|---|---|---|
| the 12 (`beta1-tree`) | 79.63 | 79.53 | 77.44 -> 77.21 | 77.64 -> 77.25 |
| the 15 (`beta1`) | 78.83 | 78.73 | 76.82 -> 76.61 | 77.03 -> 76.68 |
| evoker | 81.44 | 80.67 | 81.56 -> 80.76 | 81.68 -> 81.40 |
| conjurer | 74.51 | 74.97 | 72.45 -> 73.87 | 73.30 -> 74.45 |
| healer | 83.25 | 82.29 | 83.23 -> 81.78 | 85.41 -> 81.49 |
| warlock | 77.13 | 76.70 | 78.63 -> 75.80 | 80.28 -> 79.15 |
| knight | 70.35 | 71.33 | 70.41 -> 71.28 | 68.86 -> 69.92 |

Mage, melee, ranged and the other weapon classes are unchanged to the last digit (no home school; the Knight moves because two
of his scenarios hold a Healer). The circle is a design rule about who a class IS, not a tuning result, and the lab's objective
prices only the fight's outcome: on it the circle costs the set 0.1 to 0.4 J and the Healer about 1 J at g3 and 3.9 J as
shipped (85.41 se 1.5 against 81.49 se 2.2, the same six seeds; the "before" columns were re-measured on this branch's binary
before the merge, so they differ by a few hundredths from the rows of 31.12). The Healer's loss is the largest single effect
in this section and it comes from a fixed input: it is reported, not tuned around. These are the new gate rows. Every search below runs on top of the circle. The two `--t2 off` pins of the CLI checks
moved with it (76.96 -> 76.29, 79.45 -> 79.06) and were re-pinned with that reason.

### 32.8 Audit of the sim edits that were not Fable's (dodge rates, the Knight guard tally, `--hands`, `--curve`)

| Edit | Checked against | Verdict |
|---|---|---|
| dodge-rate mirror (`RealClasses.cpp avoidThreat`, `Setup.h MfoPackage`) | `MFO_GenerateESP.py CLS_AVOID_THREAT`, `class_csty_params`, `apmf/StyleClaims.cpp` | values and the stance order match (generator stance 1 / 2 / 3 = lab 0 / 1 / 2, the +0.10 lands on the missile stance). DEFECT, fixed (32.9): a follower in a heavy cuirass dodges not at all in the game, while the lab dressed every melee-root class in heavy armor AND let the Mercenary dodge at 0.35 |
| Knight guard tally (`World.h` `inc2` / `guard2`) | `iaus/Generate.cpp` `tauntedOnMe`, `Policy.h` `tauntedByMe` | the tally counts every foe under a live taunt of his, without the gate's "has him as target, not paralysed, not fleeing" filter, and it counts the seconds in which the line is off by design (hurt with a potion). Its denominator is BROADER than the game's guard-line condition, so the share it prints is a lower bound for the 60% floor. Not changed |
| `--hands`, the second-hand model, `TouchSpent` | `iaus/Candidates.cpp`, `Reposition.h HandsRange` | mirrors the native code, reads the class's own `touchSpentSec`. FOUND: the touch window never opens in the lab. `touchSpentSec` 0 against 3 gives the same J to the last digit for every casting cell on the built-in world, LoreRim and Tuxborn. The model ("the other hand holds the spell cast before the last") almost never pairs a touch spell with a distance spell, so the lab cannot rate this constant. Not fixed here (it needs a model of which two spells a mage equips, which is its own piece of work) |
| `--curve` and the `RealClasses` profile build | `ClassCurves.h` | correct. Two things added (32.1): base-row slugs, and the refusal of `--opt c.N` next to a `--curve` on N. To know: a T1 row override also changes the T1 companions in every other class's fights (faithful to the game; the per-class gates catch a loss) |
| found on the way | | `Setup.h BuildFight` drew a foe's two spread offsets inside one call's argument list, whose evaluation order C++ leaves open. Now two statements in the order g++ used: the same fights, on every compiler |

### 32.9 Two model corrections every number below stands on: armor by the class card, and potions

**Armor and the dodge.** FACT: `apmf/StyleClaims.cpp` (main fcbfec5) claims a class style's zero-avoid twin while his cuirass is
heavy, and marth 11-12 ruled the same for a follower with no class style. FACT (class cards, `class-perks-packages.md`): L2 Heavy
Armor is owned by Knight and Warrior, L5 Light Armor by Mercenary, Archer and Thief, L13 Arcane Robes by the mage line. The lab's
"melee root = heavy" contradicted the Mercenary card. Now: a melee class whose card gives it an Avoid Threat above the vanilla
0.20 (Mercenary 0.35, Blademaster 0.50, Living Legend 0.55) wears light armor, and a heavy cuirass means Avoid Threat 0 in every
stance, tier 1 included. MODEL: what a follower actually wears is the player's choice; the lab dresses him by his card.

| 6 seeds | g3 | `--stealth off` | shipped |
|---|---|---|---|
| mercenary | 81.83 -> 79.62 | 85.21 -> 80.98 | 85.35 -> 81.13 |
| warrior (loses a 0.20 dodge) | 88.01 -> 87.97 | 87.96 -> 87.92 | 87.67 -> 87.65 |
| the 12 | 79.53 -> 79.33 | 77.21 -> 76.94 | 77.25 -> 76.98 |
| the 15 | 78.73 -> 78.54 | 76.61 -> 76.35 | 76.68 -> 76.43 |

Every other class is identical. In the lab the Mercenary's dodge does not pay for his lighter armor: he is 2.2 to 4.2 J worse
dressed by his card than he was in plate with a dodge the game would not have given him. That is a statement about the lab's
armor and dodge models (a dodge is rolled only against projectiles, 31.2), not yet about the class.

**Potions.** DEFECT, fixed: a list's `follower.potions` table is (magnitude, duration). The lab read the magnitude as a heal at
once, and `compile_world.py` laddered fortify potions (archetype 34) and food into it. A health potion was 3% of his health on
LoreRim and 0.5% on Tuxborn (20 to 27% on the built-in world, whose fixtures have no duration and are unchanged). Now the
ladder holds restore potions only (archetype 0, at least 20 points in total) and a potion with a duration restores its
magnitude per second for that long. LoreRim's health potion is 100 over 10 s, Tuxborn's 100 or 150 at once. The shared cache
files were not overwritten: this section's list runs use corrected copies; the next `compile_world.py` run writes them so.
Every list number that involves a drink and predates this is void for the drink line.

### 32.10 The edge-number search: method

marth 11-12: "The sim should be used to find all the ideal edge case numbers, thats the point of a sim for iaus", and later:
"sim and oracle decide everything about the curves (as long as it doesnt deviate from the class designs, but even that has
small wiggle room for best class performance)".

`tools/iaus_sim/edge_search.py`. The rule was fixed before the first search result was read, and was not loosened afterwards:

- objective: J of `--classes <slug>` at the tuning target (g3), 48 fights per scenario and 24 natural fights per seed;
- train: seeds 1..N (48 on the built-in world, 32 on a list). Per candidate value, the paired difference d against the shared
  value on the same seeds, and its standard error;
- accept the best value of a sweep only if: train d > 2 se and d >= 0.30 J; holdout (seeds 1001.., never used to pick) d > 0
  and d > 1 se; `--stealth off` and the game as shipped on all seeds d >= -1 se and inside the 31.11 band; the Knight line's
  guard floor >= 60%;
- a value that passes on one world is not written unless it holds across worlds: either it also does not lose on the others
  when it passed on the built-in world, or (for a pass on a list only) a confirmatory on FRESH seeds 3001..3096 whose
  equal-weight mean over built-in, LoreRim and Tuxborn is > 2 se, with no world under -1 se. A list's actors are test input;
  a number that is best on one list only may not be baked in;
- every confirmatory and every side test below was written into the agent log with its pass condition BEFORE it was run.

Per class the search covers what `ClassCurves` reaches: the health potion's `DrinkNeed` midpoint (10 values, 0.20 to 0.70)
and slope (7 values), the magicka potion's midpoint (casting cells) or the stamina potion's (weapon cells), `touchSpentSec`
(casting cells), and for the Knight the guard-line boost and an explicit `drinkHealthLine`. 55 sweeps, 412 values per world.
With a 2 se bar on the best of 7 to 10 values, a handful of train passes per world are expected by chance; the holdout and the
cross-world step are what remove them. Sensitivity: the paired se of a sweep is 0.05 to 0.25 J for most classes (0.3 to 0.4
for the T1 and base cells), so the search can see an effect of about 0.3 to 0.5 J per class and cannot see less.

The search was run three times, because the world under it changed twice while it ran: before the School Circle (32.7), with
it, and with the armor and potion corrections (32.9). Only the last is the result. The earlier two are kept as history in 32.12.

### 32.11 The edge-number search: results on the final world

**Per class, built-in world (48 + 48 seeds): 412 values, 0 accepted, not one train pass.** Largest best-of-sweep deltas: knight
stamina midpoint 0.40 +0.26 (se 0.09), warrior stamina midpoint 0.15 +0.23 (se 0.11), archer health midpoint 0.38 +0.13
(se 0.06): over 2 se, under the 0.30 J floor. `touchSpentSec` is exactly 0.00 everywhere (32.8).

**Per class, LoreRim (32 + 32 seeds, its health potion is 100 over 10 s): 412 values.**

| Sweep | Best | Train | Holdout | off / shipped | Verdict |
|---|---|---|---|---|---|
| knight health midpoint | 0.70 (edge of the range; 0.55 +0.37, 0.60 +0.54) | +0.94 (se 0.19) | +0.89 (se 0.28) | +0.92 / +0.99 | rejected: the guard floor, already 61.7 / 57.3 on LoreRim with the shipped table, falls to 57.3 / 52.9 with the line coupled at 0.70; confirmatory with the line held at 0.34 below |
| thief health midpoint | 0.70 (rises steadily from 0.38) | +0.39 (se 0.12) | +0.33 (se 0.15) | -0.14 / -0.16 | rejected: loses as shipped |
| melee health midpoint | 0.30 (0.60 and 0.70 are -0.40) | +0.37 (se 0.17) | +0.20 (se 0.19) | +0.32 / +0.24 | passes on LoreRim; confirmatory below |
| ranged-off health midpoint | 0.70 | +0.47 (se 0.22) | -0.16 (se 0.25) | | holdout: not confirmed |

**Per class, Tuxborn (32 + 32 seeds, health potions 100 or 150 at once): 412 values.**

| Sweep | Best | Train | Holdout | off / shipped | Verdict |
|---|---|---|---|---|---|
| thief health midpoint | 0.60 | +0.34 (se 0.16) | +0.10 (se 0.10) | +0.77 / +0.73 | passes on Tuxborn; confirmatory below |
| mage health midpoint | 0.50 | +0.40 (se 0.19) | 0.00 (se 0.02) | | holdout: not confirmed |
| melee-off health midpoint | 0.50 | +0.48 (se 0.20) | +0.10 (se 0.17) | | holdout: not confirmed |
| knight health midpoint | every value from 0.38 up is NEGATIVE: 0.50 -0.43 (se 0.26), 0.70 -0.50 (se 0.27) | | | | the opposite of LoreRim |

What the three worlds say together about the health line:
- On the built-in world it is flat for every class between 0.20 and 0.70, inside 0.3 J.
- The Knight wants 0.70 on LoreRim (+0.9 J, twice confirmed there) and 0.34 or lower on Tuxborn (0.70 is -0.5 J). The best
  line is a property of the world, not of the class.
- "It is the potion's delivery" was tested (declared first: built-in foes with every potion delivered over 10 s,
  `--potion-over 10`, 48 seeds): the Knight's 0.70 is +0.34 (se 0.17) there against +0.08 with at-once potions, exactly 2.0 se
  and a third of LoreRim's gain: not established at the bar. The Thief's 0.70 is -0.22 (se 0.14) there: refuted for him. One
  thing is clear in every over-time run: a LOWER line loses (thief 0.20: -0.43, se 0.06).

**Confirmatories, fresh seeds 3001..3096 (each declared with its pass condition before it ran).** d = paired J difference.

| Value | built-in g3 | built-in off / shipped | LoreRim | Tuxborn | Mean of the three | Verdict |
|---|---|---|---|---|---|---|
| warrior stamina-potion midpoint 0.34 -> 0.15 | +0.287 (se 0.079) | +0.275 / +0.279 | +0.065 (se 0.031) | +0.109 (se 0.054) | +0.154 (se 0.030) | passes the search rule, FAILS the fixed 31.11 gates: see below |
| mercenary stamina-potion midpoint -> 0.20 | +0.134 (se 0.082) | +0.141 / +0.140 | +0.008 | +0.075 | +0.072 (se 0.035) | fails (built-in under 2 se) |
| conjurer health midpoint -> 0.25 | -0.090 (se 0.101) | -0.113 / -0.041 | -0.166 (se 0.077) | +0.095 | -0.054 (se 0.051) | fails |
| thief health midpoint -> 0.60 | +0.153 (se 0.070) | **-0.284 (se 0.152)** / -0.150 | +0.123 (se 0.069) | +0.207 (se 0.083) | +0.161 (se 0.046) | fails: loses with stealth off, which is how the game ships |
| knight health midpoint -> 0.70, guard-line cut-off held at 0.34 | +0.028 (se 0.120) | +0.098 / -0.015 | **+0.686 (se 0.116)** | **-0.371 (se 0.127)** | +0.114 (se 0.070) | fails: one list's gain is another's loss |
| melee health midpoint -> 0.30 | +0.128 (se 0.115) | +0.129 / +0.142 | -0.001 | +0.072 | +0.066 (se 0.059) | fails |

**The Warrior's stamina value, in full, because it is the one that came closest.** It replicated on four worlds and on two
versions of the lab. Two things stand against it. (1) About 0.13 of its 0.29 J is Utility's "resources left" term: a stamina
potion he no longer drinks counts as a gain (`--utility-no-resources`: +0.156, se 0.080, on the same seeds). With it he power
attacks about 10% less often per minute (6.5 to 5.9 natural, 8.1 to 7.2 strong1), a small step off his card. (2) On the six
fixed gate seeds of 31.11 he is -0.50 / -0.48 / -0.41 J and the 12-class mean falls by 0.04, 0.04 and 0.02. Those gates are
hard constraints, so the value is NOT in `ClassCurves.cpp` (it was written, verified to reproduce the `--curve` run exactly,
and reverted). It is recorded there as a comment with the exact line. To know about the gate itself: a six-seed set mean has
a standard error of about 0.44 J, so "may not fall" on it rejects about half of all small real gains by chance.

**RESULT: no per-class value passed everything. `ClassCurves.cpp` carries no searched override; the shared curves ship.**
On the final world 1236 values were tried in 165 sweeps (and 2472 more on the two earlier versions of the world). What that
supports: no drink-curve, guard-boost or drink-line value is worth more than about 0.3 to 0.5 J to a class on every world.
What it does not support: that the shared values are optimal. Smaller gains are below what the search can see.

**The Knight line's guard (marth: at least 60% under two or more of his own taunted foes).** Blows-weighted share, 6 seeds,
default / as shipped, shipped table: built-in knight 67.5 / 67.4, great-knight 67.5 / 67.4, ebony-warrior 68.8 / 66.0;
Tuxborn knight 61.8 / 60.8; **LoreRim knight 61.7 / 57.3: under the floor as shipped.** Boost on the built-in world: 1.0 gives
61.9 / 62.3, 1.25 66.3, 1.5 67.1 / 67.2, 2.0 67.5 / 67.4, 3.0 and 4.0 the same: 2.0 is where the plateau starts and J is flat
across it, so 2.0 stays, now with its curve. Nothing the table reaches lifts LoreRim's 57.3 (the ceiling is Harbinger's hold
and rest times, 31.12). The tally's denominator is broader than the game's guard-line condition (32.8), so these are lower bounds.

**Shared `Tuning` scalars (built-in, 24 + 24 seeds, the 15 cells pooled; 21 scalars, 66 values).** Beat noise with the holdout
and both gates, in all three versions of the world:

| Scalar | Shipped -> best (edge of the range tried) | Train | Holdout | off / shipped |
|---|---|---|---|---|
| `momentum` | 1.25 -> 1.0 | +0.56 (se 0.07) | +0.62 (se 0.08) | +0.71 / +0.28 |
| `rowlessPowerCooldownSec` | 10 -> 5 | +0.32 (se 0.05) | +0.31 (se 0.04) | +0.36 / +0.33 |
| `postureMomentum` | 1.5 -> 2.0 | +0.16 (se 0.04) | +0.17 (se 0.04) | +0.05 / +0.04 |

NOT applied: they are `Core.h` initialisers, outside this branch's native scope, and each needs its own confirmation first.
Momentum is the anti-switching term and J does not price switching: at 1.0 the decision switches per minute rise from 18.2
to 26.9 (evoker), 16.6 to 25.4 (conjurer), 14.2 to 25.8 (melee-off), 7.5 to 10.2 (melee), while the gap ledger already lists
target switching above the field rate (G9). The power cooldown is a ruling (10 s rows), not a tuning. `actFloor` 0.10 was
+0.18 before the School Circle and +0.13 (se 0.05) after: it does not hold. `noHealDrink` 1.0 is +0.06 (se 0.02). Exactly 0
at every value (the road is off or nothing reads it in the lab): `groundCooldownSec`, `blockCooldownSec`, `bashCooldownSec`,
`stepCooldownSec`, `regroupCooldownSec`, `kiteDistance`, `emergencyHpHigh` / `Low`, `rowlessTauntCooldownSec`.

**Compile-time constants (`const_probe.sh`, 24 seeds, pooled, g3 / off / shipped).** `kFleeHealthLine` 0.30: 0.20 +0.03
(se 0.03) / +0.03 / +0.04, 0.40 +0.03 (se 0.015), 0.50 +0.02: flat. `kGuardLineFoes` 2: 1 is -0.12 (se 0.016) / -0.15, 3 is
+0.02 (se 0.009): 2 holds. `kPressCount` 2: 1 is +0.01, 3 is -0.04 (se 0.014): 2 holds. NOT probed (the run was stopped):
`kCloseSlack`, `kMinimumFrac`, `kRangeVeto`, `kPostureDist`. `kRangeVeto` 1.15 also exists as literals in three `Catalogue.cpp`
curves, so a probe of the constant alone moves only the generator's half.

**The drink line, the oracle and what J can say.** The class oracle drinks at 72 to 94% health and lets 33 to 81% of the
potion run over, with 0 to 0.05 downs per fight. That is not a perfect drink time. Utility pays 5 points for the party's
health at the end and charges 2.5 / (potions carried) for a potion, so a planner tops up; the Warrior's stamina value is the
same exchange rate read the other way. How much a potion in the bag is worth against health in the body is a value choice in
Utility, and the "best drink time" J returns moves with it. On the lists (corrected potions) the decline is steeper (LoreRim:
melee 6.5 %/s, mage 14.8 %/s against 3.5 and 6.0 on the built-in world) and a potion is 45 to 58% of his health there.

### 32.12 History: the two earlier versions of the world

Before the School Circle (412 values per world): built-in 0 accepted (conjurer health midpoint 0.70 passed train +0.67, failed
holdout +0.16); LoreRim 0; Tuxborn 2 single-world passes that lost elsewhere. A pooled test declared in advance (health
midpoint 0.50 for all 15 cells, fresh seeds 2001..2048): +0.053 (se 0.030), failed. With the circle, before the armor and
potion corrections: built-in 0, LoreRim 0, Tuxborn 3 single-world passes. Every list number of these two runs involving a
drink is void (32.9): a health potion was 0.5 to 3% of his health there.

### 32.13 HANDOFF to the per-class curve tuning (a fresh Fable takes it from here)

**Tools.** Build: `SIM_OUT=<bin> tools/iaus_sim/run.sh --list-uses` (builds, prints the 190 (family, consideration) pairs).
- `edge_search.py --bin <bin> per-class [--classes a,b] [--list L.sim.json] [--seeds 48]`: the narrow search of 32.10.
- `edge_search.py --bin <bin> wide [--top 12] [--seeds 24]`: every curve parameter the table can move, per class: liveness on
  six seeds (each parameter x0.67 and x1.5), then the most sensitive ones at x0.5 / x0.75 / x1.33 / x2, an accepted value
  carried forward. **WRITTEN, NOT RUN END TO END** (only `--help`, the `--list-uses` parse and part of one liveness pass ran):
  test it on one class first. Known weak spot: liveness runs with `--fights 48`, about 150 s per class.
- `edge_search.py --bin <bin> global`, `gates --curves "<specs>"` (the literal 31.11 gates, class by class), `floor`.
- After every sweep one normal results file goes to `~/.cache/mfo-iaus-sim/runs` (label `sweep ...` / `wide ...`), so the
  dashboard shows progress (`--no-emit-runs` off). main's dashboard reads these files but its `--json-check` rejects schema 2.
- `const_probe.sh <constant> <value> <out>`: a variant binary with one `constexpr` of `native/iaus` rewritten in a scratch copy.
- Diagnostics: `--decline N [--oracle]`, `--rating-only`, `--utility-no-resources`, `--potion-over S`.
- Worlds: built-in; LoreRim / Tuxborn / Vanilla need the potion-corrected inputs (re-run `tools/list_balance/compile_world.py
  --name <L>`, which overwrites the shared cache file, or rebuild the ladders as 32.9 says). Seeds: train 1..N, holdout
  1001.., confirmatory 3001..3096, pooled test 2001..2048 are USED; take new ranges for new confirmatories.

**The acceptance rule** is 32.10, plus the literal 31.11 gates on the final table. Write every side test's pass condition down
before running it.

**Known gaps.**
1. Piece curves (20: attack / power_attack / bash range, los and engaged, taunt survive and mortal, cast overkill and exploit,
   heal urgency, regroup far) cannot be moved through `--curve` or `ClassCurves::SetCurveParam` (a, b, floor, zeroAbove,
   zeroBelow only). Searching them needs a point-edit surface in `ClassCurves` (a native change, for review).
2. Per-class family and role weights (card I8): `Profile::weights` exists, `ClassCurves::Row` has no field for them, nothing
   in native fills them. The sim can move a weight only for everyone (`--set w.<f>.<c>`).
3. `touchSpentSec` cannot be rated: the lab never opens a touch window (32.8). It needs a model of the two spells a mage holds.
4. The 58 pure 0 / 1 gates are rules; the wide search never touches them.
5. Utility's potion-against-health exchange rate decides every drink result (above). Settle it with marth before tuning drinks.
6. The fixed gates are six seeds; a paired many-seed gate would stop them rejecting small real gains at random.
7. The rating's ranged line (32.5) and the Mercenary in light armor (32.9) may be lab artefacts (bow and dodge models).
8. School Circle: fixed, never searched. Its measured cost stands in 32.7 (Healer about -3.9 J as shipped).

**Next steps, in order.** (1) Run `wide` on one class, read the liveness list, fix what it shows. (2) Use the per-class oracle
gap (`class_gaps` of the newest full run: melee and the base cells are furthest from the oracle) to order the classes.
(3) `wide` on the built-in world for the 15 cells, then a fresh-seed three-world confirmatory and `gates` for each pass.
(4) Decide with the coordinator whether the Warrior stamina value ships despite the six-seed gate. (5) Confirm the three
`Tuning` winners on fresh seeds and on the lists, with a switching metric next to J for momentum.

## 33. The per-class tuning: what was built, and the RUNBOOK for the run (2026-11-12)

Branch `feat/class-tuning`. Fable author (the structural part only: marth 11-12 moved the run itself to a cheaper agent). This
section is written for that agent. Nothing below is a tuning result: the run has not been made. FACT / MODEL as in 31.2.

### 33.1 What changed in the lab and in native

1. **Every curve is a knob now.** `ClassCurves::SetCurveParam` takes `px0..px5` / `py0..py5` for a Piece curve (one point's x or
   y; refused on a curve that is not a Piece, on a point it does not have, on a non-finite value, on an x past a neighbour).
   The 20 Piece curves of 32.13 gap 1 are searchable. Sim: `--curve <class>:<family>.<cons>.py1=0.6`.
2. **Per-class weights (card I8) have a table surface.** `ClassCurves::Row` gains `weights` (a consideration's exponent for this
   class's family, `Profile::weights`) and `families` (the class's multiplier on a whole family, `Profile::family`);
   `SetWeight`, `SetFamilyWeight`; `Build` copies them into the Profile. Unset = bit-identical (harness: every shipped row
   has no override, no weight and every family multiplier exactly 1.0). Sim: `--curve <class>:<family>.<cons>.w=<v>` and
   `--curve <class>:family.<family>=<v>`. `ClassCurves.cpp` has an empty `SeedSearchedValues(Table&)` where accepted values go.
3. **What a potion is worth is measured (marth, Q-potion-value: "Sim calibrates it").** Utility used to pay
   2.5 / (potions carried) / (followers) for a potion left over, about 0.3 J. Now: `--potion-calib N` plays the same natural
   fights with the subject's stock of one kind forced to 0..4 and prints what one more potion at the start of a fight buys
   and how many he drinks; `potion_worth.py` turns that into the worth of the k-th potion left in the bag over a CHAIN of
   fights (W_h(p) = U(p) + sum_u P[p][u] W_{h-1}(p - u); the mean over the 8 places of a chain of 8 fights between restocks);
   Utility charges each potion drunk that worth (`Oracle.h PotionCost`). The table belongs to a world: the built-in one is the
   generated `tools/iaus_sim/PotionWorth.h`, a list's is `--potion-worth <file>` (the driver measures it). `--potion-worth
   legacy` is the old rate and reproduces the old J exactly. MODEL choices: the chain is 8 fights (the file prints 4 and 16
   next to it); a measured d[k] counts only when it beats its own noise (d > 2 se), else 0; the fights are the natural bracket; end health and end magicka keep
   their old terms (they regenerate between fights: a possible next step, not done).
4. **The gate rule (marth, Q-gate-rule: "Gate on many seeds").** The no-class-worse gates are judged on the same seeds as the
   search (train + holdout), paired, and a drop counts only beyond noise. The full rule is the `RULE` block of
   `tune_driver.py`, copied in 33.3. It was written before any result of the driver was read.
5. **Decision switching is visible next to J**: `--print-switch` prints the subject's decision switches and fight minutes
   per seed. J does not price switching, so the driver reports the rate and never applies a shared value that raises it
   by more than 10% for any class.
6. **Why a blow found no guard**: `--guard-why` adds `ung2*%` metrics (staggered, the rest after a hold, hurt with a
   potion = drink first, drinking, mid action, free) to the Knight line's tally.
7. **Review fixes of the sim batch** (`edge_search.py`): a single-world pass is labelled "passes on this world only: not a
   result" (the accept path is the driver's); `moved()` never crosses 0 and a boost never goes under 1; the potion kind follows
   the cell's family (melee-off and ranged-off are stamina cells); `global` uses the per-class rule (2 se, 0.30 J, the band).
   `Results.h` writes `kRatingCells`; the dashboard's Trend compares two gaps only when they come from the same source.

### 33.2 Two findings that change what 32.11 said

- **The Knight floor on LoreRim was measured on a world without LoreRim's combat mods.** The simulator reads
  `<list>.combatmods.json` BESIDE the `.sim.json` and takes a missing file for "no such mod" (`Load.h`). The final-world list
  runs of 32.11 used copies in a scratch directory, without that file. With it (the shared cache, re-compiled with the
  corrected potion ladders): Knight, as shipped, 48 seeds: **63.1%** (six-seed groups 61.2 to 65.4); without it 58.5% (57.3
  on seeds 1 to 6, the number of 32.11). So the shipped table is OVER the floor on LoreRim. Every LoreRim number of 32.11
  has the same defect (Tuxborn's and Vanilla's files are empty, so those are unaffected). The driver refuses a list whose
  companion file is missing.
- **Where the unguarded blows go** (Knight, as shipped, 6 seeds, percent of all blows under two or more own-taunted foes):
  built-in: the rest after a hold 18.4, mid action 8.4, staggered 2.6, drinking 1.9; LoreRim: rest 18.1, mid action 6.0,
  staggered 4.9, drinking 4.0, drink-first 1.3; Tuxborn: rest 16.5, staggered 10.4, mid action 9.6. The 1.5 s rest after a
  hold (Harbinger's) is the largest cause on every world: if the floor ever has to rise, that is the lever, and it is not a
  table value.

- **What a potion is worth on the built-in world** (`PotionWorth.h`, 9600 fights per class, kind and stock, chain of 8): a
  health potion left in the bag is worth 1.2 to 2.1 J to the Knight, the Warrior, the Mercenary and the melee cells, 0.3 to
  1.1 to the ranged cells and the Evoker and Mage, up to 0.5 to the Healer, Conjurer, Warlock and Thief. A stamina
  potion is worth something only to the Warrior (0.3 to 0.6) and the Mercenary (0.1 to 0.4); to every other class, the Knight
  included, its worth cannot be told from 0 (the Knight drinks 0.58 a fight and gains nothing measurable). A magicka potion:
  the first is worth 0.2 to 0.5 to the four casting T2 classes, the rest nothing. The old rate was about 0.3 J for any potion:
  health potions were underpriced several times over, which is why the class oracle drank at 72 to 94% health (32.11).
  With a chain of 4 the Knight's first health potion is 1.2 J, with 16 it is 2.6: the chain length is a MODEL choice.

### 33.3 The rule (copied from `tune_driver.py`; the script is the authority)

A number is ACCEPTED only when every line holds.

- train: built-in world, default roads (g3), seeds 1..64: the best value of a sweep has paired d > 2.5 se and d >= 0.15 J.
- holdout: seeds 1001..1064, never used to pick: d > 1.5 se.
- gates (many seeds): on the 128 train + holdout seeds, with stealth off and as shipped, the class may not get worse, and
  worse = d < -2 se and d < -0.05 J (when the 15 cells are judged at once: -2.5 se); never more than 0.5 J down (Thief,
  Mercenary 0.25) when that drop is itself outside one se. A T1 row (melee / ranged / mage) is the companion in other classes' fights, so its value is gated on all
  15 cells at all three configs.
- cross-world: fresh seeds 4001..4096 on built-in, LoreRim, Tuxborn, Vanilla: the equal-weight mean of the four d is > 2 se,
  no world is worse, and as shipped the mean is not worse and inside the band.
- joint: the T1 rows (melee, ranged, mage) are searched first and their accepted values are part of every later class's
  baseline. When the classes are done, the whole final table is gated against the shipped one on all 15 cells (128 seeds,
  three configs: no cell worse at the 15-at-once bar, no 15-cell mean worse) and each T1 value is left out in turn (the
  table without it may not be better). A failure writes nothing and is an open decision.
- Knight line: the guard floor holds (>= 60%, measured on 36 seeds per world and config; the six-seed groups of 33.2
  spread over about 4 points) on every world, default and as shipped (a Knight-line value, a T1 melee value and a shared
  value are all held to it). A floor run with no blow in its tally stops the driver: it never reads as a floor that holds.
- fixed: the School Circle is never a knob; pure 0 / 1 gates and Step vetoes are never knobs; a knob never leaves its valid
  range (no negative offset, floor or level; a boost never under 1; a Piece point at 0 stays a veto).

Changes from 32.10, and why: 64 + 64 seeds instead of 48 + 48 (a run is 0.17 s per seed and class); the train bar is 2.5 se
and 0.15 J instead of 2 se and 0.30 J (more seeds see smaller effects; the stricter se bar pays for the many more knobs);
the holdout bar is 1.5 se instead of 1; the gates follow Q-gate-rule; Vanilla is a fourth world; the literal six-seed gates
of 31.11 are gone (Q-gate-rule).

### 33.4 RUNBOOK

**Before the run** (once, about 2 minutes):

```
cd <repo>
SIM_OUT=/tmp/claude-1000/class-tuning-bin/sim-r2 taskset -c 0-11 nice -n 10 tools/iaus_sim/run.sh --print-classes | head -3
ls ~/.cache/mfo-list-balance/{LoreRim,Tuxborn,Vanilla}.{sim,combatmods}.json     # all six must exist
```

If a `.sim.json` is missing or older than `tools/list_balance/compile_world.py`: `python3 tools/list_balance/compile_world.py
--name <List>`. `PotionWorth.h` is committed; re-measure it only if the sim's fights changed
(`tools/iaus_sim/potion_worth.py --bin <sim> --out <file> --header`, about 25 minutes, then rebuild the sim).

**Launch** (detached; the run directory must be durable for the length of the run):

```
OUT=<scratchpad>/tune/full
mkdir -p $OUT
setsid nohup taskset -c 0-11 nice -n 10 python3 tools/iaus_sim/tune_driver.py \
    --bin /tmp/claude-1000/class-tuning-bin/sim-r2 --out $OUT --jobs 12 > $OUT/driver.out 2>&1 &
```

Do not poll it in a loop. `tail -3 $OUT/progress.log` shows the last finished step; the dashboard gets one results file
per finished sweep. Expected length: hours (15 cells, every live knob of each (34 to about 190 of 396; cap 250), 2 passes).

**Resume**: run the same command again. Everything finished is in `$OUT/cache.jsonl` and replays in seconds; never delete
that file, never edit it. One stage only: add `--stages floor,rating,summary` (any subset, in order). One class only:
`--classes knight` (with `--stages classes,joint,summary`); the driver refuses it until melee, ranged and mage are done in
that `--out`, because their values are the Knight's baseline.

**What a cached result is tied to**: the sha256 of the simulator binary (the built-in potion table and every curve are
compiled into it), of the world's `.sim.json` and `.combatmods.json`, and of its potion-worth table are part of every cache
key (`summary.json` `stages.prep.identity`). Rebuild the simulator, re-extract a list or re-measure a table and the
results that depend on it are measured again; nothing stale is replayed. Never rebuild the binary in place while a run
is going: build to a new path and start a new `--out`. A list's potion-worth table is reused only while the binary and
the list files it was measured from are the same (the `.inputs` stamp beside it). A simulator run that fails stops the
driver with the command and the simulator's message; it is never cached and never read as an empty result. The simulator
itself refuses a `--list` run without `--potion-worth` (`edge_search.py` too).

**Where the result lands**: `$OUT/summary.md` (read this), `$OUT/summary.json` (every number), `$OUT/progress.log`. Both
summaries are rewritten after every sweep, so a half-finished run can be read too (classes marked "(running)").

**What each verdict means**

| Verdict | Meaning | What the runner does |
|---|---|---|
| `ACCEPTED: holdout, many-seed gates, cross-world on fresh seeds` | passed every line of the rule | write it (33.5), unless an open decision names it |
| `train: does not beat noise` / `beats noise but under 0.15 J` | nothing to see at 64 seeds | nothing; the shared value stays |
| `holdout: not confirmed` | the train pick was chance | nothing |
| `gate: <class> gets worse at <config>` | a real loss with stealth off or as shipped | nothing |
| `cross-world: ...` | the gain does not hold on fresh seeds over the four worlds | nothing |
| `Knight guard floor: ...` | the value pushes the Knight line under 60% | nothing |
| joint `PASS` | the whole final table holds against the shipped one on all 15 cells | go on |
| joint `FAIL: ...` | the values do not hold together | open decision: write NO value, report |
| globals `PASSES RULE` or `BEATS NOISE, NOT APPLIED` | a shared default would gain | ALWAYS an open decision: report, do not write |
| consts `BEATS NOISE` | a compile-time constant would gain | open decision: report, do not write |
| recalib `DOES NOT HOLD` | an accepted value loses under the re-measured potion worth | do not write it; report |
| rating `BELOW` | a class is under its tier on that world | report |

**Decisions the rule leaves open. The runner STOPS and reports these; it does not decide.** They are listed at the end of
`summary.md` under OPEN DECISIONS with their numbers.

1. Any shared `Tuning` default (`t.momentum`, `t.powerCd`, `t.postureMomentum`): a design value. `t.powerCd` is also a
   ruling (10 s rows). `t.momentum` is the anti-thrash term and J does not price switching: whether switching needs a price
   in Utility first is marth's or the coordinator's call. The driver gives J and the switching rate per class for 1.0, 1.05,
   1.1, 1.15 and 1.2.
2. Any compile-time constant that beats noise (`kCloseSlack`, `kMinimumFrac`, `kRangeVeto`, `kPostureDist`): a native edit
   of its own (`kRangeVeto` is also three literals in `Catalogue.cpp`).
3. The class-card read of every class with an accepted value: the driver lists the twelve largest changes in his play
   (`play_changes_natural`). If a change takes him off his card (a Knight who stops taunting, a Warrior who power attacks
   much less), report it with its gain instead of writing it; small deviations that measurably help are allowed, each listed.
4. A Knight line under 60% on any world with the final table: a mechanism change (the guard's hold, renewal and rest, or a
   taunt-count term), not a table value.
5. A class flagged BELOW its tier by the rating battery.
6. An accepted value that fails `recalib`.
7. More than ten accepted values for one class, or a consideration weight accepted at the edge of the range tried (x2 or
   x0.5 of the catalogue's): report before writing (a sign the shared catalogue, not the class, is off). The driver raises
   both itself.
9. A failed joint gate: no value of the run is written until it is decided.
8. Anything the driver stops on (a missing input, a simulator error): report the last lines of `driver.out`. Do not edit
   the driver's rule or thresholds to get past it; a rule change is written down first and is Fable's.

**The smoke run** (`--smoke`: 4 seeds, 4 knobs, first sweep pushed through every later step; its numbers are NOT results
and its dashboard files are labelled so). Made on the final binary, Knight, built-in + LoreRim: every stage ran (prep with
LoreRim's potion worth, rejudge, classes: 397 knobs, 85 live, 312 dead for the Knight, holdout, gates, cross-world, Knight
floor; globals with the switching rate; one constant variant built and compared; floor with causes; rating; recalib;
summary) in 72 s on 12 cores. Killed with SIGKILL 22 s in (mid liveness), the same command finished the run from 818 cached
results; a third run of the finished directory replayed in under a second and wrote no second dashboard file.
After the review round (cache identity, failed runs, the joint gate; melee + Knight, built-in + LoreRim, binary `sim-r2`):
(A) a simulator whose Knight-floor run exits 3 stopped the driver with the command and "planted failure", and no floor
result was cached; (B) SIGKILL 45 s in (mid sweep), the same command finished from 3214 cached results, the Knight searched
on top of the melee row's value, joint gate run; a third run: 0 s, 0 new results; (C) one line of LoreRim's potion table
changed: the LoreRim floor was measured again (4 new results), the built-in one replayed. All 15 cells
with the oracle-gap order (ranged-off, melee, mercenary, melee-off first): a second smoke run, about 9 minutes; live knobs per cell 34 (melee) to 191 (mage-off) of 396. To repeat it: the launch command with `--smoke --classes knight --worlds
builtin,LoreRim` and another `--out`.

### 33.5 Writing accepted values into native (mechanical)

1. `summary.md` has a block "Lines for ClassCurves.cpp": one comment line with the evidence and one statement per accepted
   value. Paste them into `SeedSearchedValues` in `native/iaus/ClassCurves.cpp`, after the `sh` line, grouped by class.
   Do not write a value by hand and do not round it.
2. Replace the "NO VALUE PASSED EVERYTHING" paragraph of that file's comment with one sentence naming the run (date, seeds,
   worlds) and this section.
3. `tools/iaus_harness/harness.cpp`: the check "every shipped row has no curve override ..." is now false for the rows that
   took a value. Change it to skip exactly those rows (list their indexes in the check) and keep it for every other row.
4. Prove the write: for each class with values, `<sim> --no-json --classes <class> --seeds 1,2,3` built from the edited
   tree with NO `--curve` must print the same three SEED lines as the unedited tree with the accepted `--curve` specs.
5. `tools/iaus_sim/run.sh --no-json --classes beta1 --baseline tools/iaus_sim/baseline_builtin.txt` (regenerated on
   purpose; say which classes moved in the commit message), `SIM_CLITEST=1 tools/iaus_sim/run.sh` (re-pin only the pins of
   classes that took a value), `tools/iaus_harness/run.sh` (0 failed), `tools/iaus_dash/test.sh`, the local MSVC build.
6. Shared defaults and constants are NOT written by the runner (open decisions 1 and 2).
7. The diff goes to an Opus review (tier B: values inside an existing mechanism).

## 34. The pipeline closes on the oracle: driver fixes, the edge stage, the gap stage, the new-class workflow (2026-11-12)

Branch `feat/oracle-gap` on `feat/class-tuning` 4f0cd62d. Opus author. marth 11-12: "getting close to oracle should be a goal";
re-scope: "adding the final oracle gap close to the end of it. So that every time we add a class, that script itself will iterate
through exactly what needs it." Native scope: `iaus/ClassCurves.cpp` one comparison (F7). Everything else is `tools/iaus_sim`.

### 34.1 Review F7 / F8 (the class-tuning backlog, raised against 33a0b963)

- F7: a Logi midpoint candidate keeps its sign (`tune_driver.py logi_mid`; a negative midpoint was clamped to +0.02). Native
  `ClassCurves::SetCurveParam` refuses a Piece x EQUAL to a neighbour (strict), harness check added. No shipped Piece curve has a
  duplicate x (`--list-uses` scan), so no shipped behaviour moves.
- F8: `check_world()` runs inside `world_args()` and `ident()`: every stage that reads a list checks its `.sim.json`,
  `.combatmods.json` and potion-worth table, not only `stage_prep`.

### 34.2 The edge stage (marth 11-12 "Widen to x4 / x0.25")

Only knobs whose ACCEPTED value is the outermost value tried on its side are searched again past that edge: multiplicative knobs
at x3 / x4 or x0.35 / x0.25 of the shared value, additive knobs at 2 and 4 times the largest step tried on that side, always inside
the knob's valid range (`KNOB_META`, filled by `all_knobs`; a Piece x stays strictly between its neighbours). Same RULE; the wider
value REPLACES the accepted one in its test (`judge(replace=)`), so two specs never sit on one parameter. The table is never changed
here: the edge value stays written, and a wider value that passes every line is an OPEN DECISION ("far from the shared curve").

### 34.3 The gap stage

The rule is `stage_gap`'s docstring (written before any of its results was read). In short:
- the class oracle plans for the SUBJECT only (`--oracle-subject-only`). FOUND: the default oracle (`OracleCfg.allFollowers`)
  drives every follower, so a class's gap in every earlier table also carried its T1 companions' gap. Smoke, Knight, built-in,
  8 fights: subject-only -1.32 (se 2.20) against all followers +1.74 (se 0.67). The all-followers gap is still measured on the
  built-in world for continuity with the dashboard;
- per class and world, the shipped and the final table on the same fights (seeds 6001..): the tuning's effect is a paired
  difference. 12 seeds x (4 natural + 1 of each of his own scenarios); a cell with under 2 fights or se > max(0.5 U, |gap| / 4) gets
  12 more seeds, up to 48;
- `--gap-dump` (sim): `GAPFIGHT` per fight, `GAPSIT` the subject's divergence clusters (IAUS pick | oracle pick | kind |
  situation, summed dU; kind = retarget, ranked, unranked, posture, ext, idle), `GAPGEN` candidates generated per family, `GAPDEC`;
- `classify`: (d) the oracle's action is in `FIDELITY` (its version is more favourable than the game's: `ext:block` and `ext:bash`
  have no cooldown in `Oracle.h Apply`, the game's guard holds 2.5 s with a 4 s row cooldown and a 1.5 s rest, a timed bash has no
  road); (b) IAUS's pick broke its class ruling, the oracle took a candidate IAUS scored under the floor or vetoed, or an extended
  action of a family the class is offered elsewhere; (c) an action IAUS never generates for this class, with its road from
  section 25 (`EXT_ROAD`); (a) the rest;
- (a) drives a BOUNDED extra search: the families of the (a) clusters with the most dU (up to 3), their live knobs that took no
  value (up to 12, most sensitive first), at the midpoints between the shared value and each value tried (acceptable under RULE)
  and past the outermost ones (a pass is an open decision). An accepted value re-runs the joint gate and re-measures that gap;
- (b) to (d): one open decision per class with the evidence, and `<out>/gap-ledger.md` rows in the format of `iaus-classes.md`
  section 3. dU per fight is the oracle's local per-decision advantage summed: it ORDERS divergences, it does not add up to the gap.

Tests: `tools/iaus_sim/test_tune_driver.py` (9: logi_mid, edge_of, widen, check_world, classify, gap_parse); CLI-CHECK "--gap-dump
prints its lines, only when asked"; smoke of edge (planted accepted-at-edge rows), gap (built-in), a simulator that fails on gap runs
(stops, nothing cached), a dump without GAPFIGHT (stops, nothing cached: `text_run(check=)` validates before caching), SIGKILL
mid-stage and resume (3 cached, 3 new, a third run 0 new in 0 s).

### 34.4 RUNBOOK: a new or changed class

```
SIM_OUT=<new sim> tools/iaus_sim/run.sh --print-classes | head -3          # build the simulator with the new row
setsid nohup taskset -c 0-11 nice -n 10 python3 tools/iaus_sim/tune_driver.py --bin <new sim> --out <the earlier run's dir> \
    --for-class <slug> --jobs 12 > <dir>/driver-<slug>.out 2>&1 &
```

`--for-class` = `--classes <slug> --stages prep,classes,joint,edge,floor,gap,summary`. What it re-measures: every cache key carries
the sha256 of the simulator binary, the world's `.sim.json` / `.combatmods.json` and its potion-worth table, so a new binary
re-measures everything a stage needs (the list potion-worth tables included: `prep`) and nothing stale is replayed; an unchanged
binary replays. The T1 rows must be complete in that `--out` (their values are the new class's baseline). The other classes'
accepted values, rows, gap cells and open decisions stay (scoped stages merge; `open_decision(cls=)`). `cells()` adds the new class
to the joint gate and to a T1 value's gate. A fresh `--out` runs the whole pipeline (the T1 rows first).
