# IAUS offline simulator + list balance extractor (card I2s)

Host-only tools, never in the DLL. `tools/iaus_sim/sim.cpp` links the REAL pure layers (`iaus/Curves|Catalogue|Score|Decide|Explain|Generate.cpp`)
and builds a `Snapshot` by hand every 133 ms tick (the worker pump cadence, MAP `Diagnostics.cpp` `kPumpMs`). `tools/list_balance/extract.py`
reads an MO2 list's WINNING records (plugins.txt load order, modlist.txt priority, last definer wins) into
`~/.cache/mfo-list-balance/<list>.json` (outside the repo; list data is TEST INPUT only, native/ never reads it).

## Run
    python3 -I tools/list_balance/extract.py --list /mnt/gaming/modlists/LoreRim --profile Ultra --data "Stock Game/Data"
    python3 -I tools/list_balance/extract.py --list /mnt/gaming/modlists/Tuxborn --profile "Tuxborn - Deck" --data "Game Root/Data"
    tools/iaus_sim/run.sh [--list ~/.cache/mfo-list-balance/LoreRim.json] [--seed N] [--fights N] [--baseline out] [--compare baseline]
Committed baseline (built-in vanilla-like book, no list data): `tools/iaus_sim/baseline_builtin.txt`. Exit code 1 on any BAD-FACTS tick or invalid pick.

## What the extractor reads
SPEL/SCRL (SPIT cost, castType, delivery, castDuration, range; effects EFID/EFIT mag/area/dur), MGEF DATA (hostile/detrimental flags, resist AV,
school, minSkill, archetype, AV, delivery), ALCH single-effect restores of AV 24/25/26, RACE DATA (hp/mp/sp, regens, playable bit), ECZN max level
histogram, and GMSTs matching `Regen|DiffMult|Difficulty|PowerAttack|MagicCost|MagicSkill|MagicDiff|MagicCasting|StaminaAttack|MeleeWeaponStamina|BowDrawStamina|BlockStamina|AVDamage|AVDHealth|fMagickaReturn|fMeleeReach|fWeaponDamage|fDamage|fSneakAttack|fCombatDistance`
(the exact names found are in the JSON `gmst` map; the sim reads fCombatHealth/Magicka/StaminaRegenRateMult, fDiffMultHPToPCN, fStaminaAttackWeaponMult).
Requiem (LoreRim) shows as expected: race hp 140 vs 50, regen 0.25 vs 0.7, fDifficultyDamageMultiplier etc.

## World model (CRUDE on purpose)
Foe/party hp = race hp + 8/level, damage (6+lvl) x (level ratio)^0.8, melee foes swing every 1.5 s, spell damage = sum of damage effects x (1+2%/level) x (1-resist).
Brackets are strength relative to the party: balanced 1.0, weak 0.45, strong1 1.6, strong2 2.4, mixed (one 2.2 boss + 0.7 pack). The class gate/kit is hand-written
per base class. The spell nature classifier (Offense/Control/Heal/Buff/Summon) is a rough guess from MGEF archetype + flags, NOT CasterConsent::ClassifySpell.
Absolute win/death rates are not calibrated, only relative changes between runs (`--compare`) mean something.

## Results (seed 1, 8 fights x 5 ranks per cell)
### built-in
```
IAUS SIM  list=builtin  seed=1  fights/cell=8 (x5 rapport ranks)  spellpool=9  potions(h/m/s)=100/100/100
class|bracket         idle%  hold% sw/min  heal.s healed%   bad  inv post dec.us dec.max   win% death%  actions (% of fired)
mage|balanced          19.9    4.8   29.8    0.91    73.7     0    0  3.4    4.8     22     90     10  iR=20 tR=0 attack=29 cast_control=4 cast_heal=29 cast_offense=22 drink_health=2 drink_magicka=9 d
mage|mixed-boss        20.3    5.1   24.1    1.24    50.0     0    0  2.8    4.9     23     45     55  iR=20 tR=0 attack=30 cast_control=10 cast_heal=19 cast_offense=22 drink_health=4 drink_magicka=9 
mage|strong1           22.0    7.8   27.7    0.74    71.2     0    0  3.7    3.8     16     60     40  iR=22 tR=0 attack=23 cast_control=9 cast_heal=30 cast_offense=16 drink_health=5 drink_magicka=11 
mage|strong2            8.4    8.8   30.5    0.59    47.8     0    0  4.6    3.4     18     10     90  iR=8 tR=0 attack=29 cast_control=8 cast_heal=30 cast_offense=14 drink_health=4 drink_magicka=12 
mage|weak              22.9    3.9   31.9    0.27   100.0     0    0  4.0    5.8     23    100      0  iR=23 tR=0 attack=27 cast_buff=4 cast_control=2 cast_heal=21 cast_offense=33 drink_health=1 drink
melee|balanced         37.7    4.5   30.1    0.87    68.5     0    0  0.0    2.0     13     68     32  iR=0 tR=0 attack=59 drink_health=14 drink_magicka=2 drink_stamina=4 power_attack=3 taunt=18 
melee|mixed-boss       16.4    8.7   36.1    0.76    44.8     0    0  0.0    2.5     10      2     98  iR=0 tR=0 attack=60 drink_health=13 drink_stamina=3 power_attack=4 taunt=20 
melee|strong1          23.4   10.4   29.9    0.92    54.2     0    0  0.0    1.8     11     28     72  iR=0 tR=0 attack=59 drink_health=15 drink_magicka=1 drink_stamina=4 power_attack=6 taunt=14 
melee|strong2          33.6    5.3   30.2    0.42    41.9     0    0  0.0    1.5      9      2     98  iR=0 tR=0 attack=57 drink_health=20 drink_magicka=2 drink_stamina=1 power_attack=5 taunt=15 
melee|weak             44.2    3.1   33.9    0.58    42.9     0    0  0.0    2.2     30    100      0  iR=0 tR=0 attack=59 drink_health=4 drink_magicka=2 drink_stamina=3 power_attack=3 taunt=30 
ranged|balanced         1.6    9.2   23.7    0.90    67.3     0    0  0.0    1.5      9     70     30  iR=2 tR=0 attack=84 drink_health=8 drink_stamina=6 
ranged|mixed-boss       0.7    9.4   22.9    0.66    56.8     0    0  0.0    1.9      9     10     90  iR=1 tR=0 attack=81 drink_health=11 drink_stamina=5 flee=3 
ranged|strong1          0.1    9.3   18.3    0.93    49.0     0    0  0.0    1.3      9     42     58  iR=0 tR=0 attack=88 drink_health=7 drink_stamina=4 
ranged|strong2          0.0   11.5   20.6    0.78    37.7     0    0  0.0    1.2      7      8     92  iR=0 tR=0 attack=87 drink_health=8 drink_stamina=4 
ranged|weak             4.6    5.5   28.9    0.70    63.6     0    0  0.0    1.8     32    100      0  iR=5 tR=0 attack=86 drink_health=2 drink_magicka=2 drink_stamina=6 flee=4 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
baseline written: tools/iaus_sim/baseline_builtin.txt
```
### LoreRim (Requiem)
```
IAUS SIM  list=LoreRim  seed=1  fights/cell=8 (x5 rapport ranks)  spellpool=4076  potions(h/m/s)=500/500/500
class|bracket         idle%  hold% sw/min  heal.s healed%   bad  inv post dec.us dec.max   win% death%  actions (% of fired)
mage|balanced          18.7    5.5   18.8    0.88    69.2     0    0  2.9    4.9     26     70     30  iR=19 tR=0 attack=12 cast_buff=9 cast_control=39 cast_heal=28 cast_offense=3 drink_health=1 drink
mage|mixed-boss        10.2    7.3   19.4    0.78    61.8     0    0  3.1    5.9     47     28     72  iR=10 tR=0 attack=14 cast_buff=4 cast_control=50 cast_heal=21 cast_offense=1 drink_health=3 drink
mage|strong1            9.4    7.2   23.3    0.79    64.0     0    0  3.9    4.4     18     42     58  iR=9 tR=0 attack=29 cast_buff=10 cast_control=23 cast_heal=24 cast_offense=3 drink_health=3 drink
mage|strong2           12.8    7.8   23.1    0.79    61.5     0    0  5.1    3.8     18     10     90  iR=13 tR=0 attack=19 cast_buff=10 cast_control=26 cast_heal=26 cast_offense=8 drink_health=4 drin
mage|weak              33.4    4.0   21.7    1.16    91.7     0    0  2.4    4.5     24     92      5  iR=33 tR=0 attack=19 cast_buff=18 cast_control=21 cast_heal=26 cast_offense=7 drink_health=2 drin
melee|balanced         27.1    5.2   26.4    1.59    67.4     0    0  0.0    2.0     15     68     32  iR=0 tR=0 attack=71 drink_health=8 drink_magicka=1 drink_stamina=4 power_attack=2 taunt=14 
melee|mixed-boss       15.0    5.5   31.9    0.95    39.1     0    0  0.0    2.4     13     10     90  iR=0 tR=0 attack=69 drink_health=7 drink_magicka=1 drink_stamina=4 power_attack=2 taunt=17 
melee|strong1          18.1    7.4   30.1    1.12    52.5     0    0  0.0    1.8     11     30     70  iR=0 tR=0 attack=65 drink_health=11 drink_magicka=1 drink_stamina=5 power_attack=4 taunt=13 
melee|strong2          27.3    6.5   29.5    0.66    38.6     0    0  0.0    1.6     10     12     88  iR=0 tR=0 attack=65 drink_health=12 drink_magicka=2 drink_stamina=3 power_attack=6 taunt=13 
melee|weak             29.2    3.3   24.8    1.09    70.0     0    0  0.0    2.3     19     92      8  iR=0 tR=0 attack=73 drink_health=3 drink_stamina=3 power_attack=2 taunt=18 
ranged|balanced         3.9    7.1   18.9    1.18    73.9     0    0  0.0    1.5     11     75     25  iR=4 tR=0 attack=89 drink_health=6 drink_stamina=5 
ranged|mixed-boss       0.3    7.1   18.5    0.86    53.5     0    0  0.0    1.7    249     20     80  iR=0 tR=0 attack=90 drink_health=6 drink_stamina=4 
ranged|strong1          0.1    7.7   15.9    1.25    52.3     0    0  0.0    1.2     11     40     60  iR=0 tR=0 attack=90 drink_health=6 drink_stamina=3 
ranged|strong2          0.0    9.7   20.0    0.69    37.7     0    0  0.0    1.4     14     10     90  iR=0 tR=0 attack=89 drink_health=6 drink_stamina=4 
ranged|weak             2.2    4.7   22.3    0.80    72.7     0    0  0.0    1.8     11     95      5  iR=2 tR=0 attack=87 drink_health=2 drink_magicka=2 drink_stamina=5 flee=4 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
baseline written: /tmp/claude-1000/-mnt-gaming-modlists-Projects-marth-follower-overhaul/a935cdde-4a9b-4a50-983f-339ce7b41062/scratchpad/base_LoreRim.txt
```
### Tuxborn (SimonRim)
```
IAUS SIM  list=Tuxborn  seed=1  fights/cell=8 (x5 rapport ranks)  spellpool=3303  potions(h/m/s)=300/300/300
class|bracket         idle%  hold% sw/min  heal.s healed%   bad  inv post dec.us dec.max   win% death%  actions (% of fired)
mage|balanced          10.8    7.1   29.1    0.87    76.9     0    0  5.0    5.4     28     50     50  iR=11 tR=0 attack=15 cast_buff=10 cast_control=18 cast_heal=40 cast_offense=2 drink_health=3 drin
mage|mixed-boss         5.8    8.4   27.7    0.78    49.0     0    0  6.1    6.6     18     10     90  iR=6 tR=0 attack=10 cast_buff=7 cast_control=31 cast_heal=34 cast_offense=5 drink_health=2 drink_
mage|strong1           14.5    9.2   26.7    0.68    52.1     0    0  5.5    4.5     15     30     70  iR=14 tR=0 attack=15 cast_buff=7 cast_control=29 cast_heal=28 cast_offense=10 drink_health=2 drin
mage|strong2           10.7    4.4   32.9    0.43    47.2     0    0  8.7    4.4     17      2     98  iR=11 tR=0 attack=12 cast_buff=15 cast_control=25 cast_heal=28 cast_offense=7 drink_health=3 drin
mage|weak              12.9    5.3   31.3    1.05    91.7     0    0  4.1    5.8     28     90     10  iR=13 tR=0 attack=8 cast_buff=18 cast_control=19 cast_heal=38 cast_offense=5 drink_health=2 drink
melee|balanced         35.0    3.3   35.6    0.68    53.3     0    0  0.0    2.0     14     50     50  iR=0 tR=0 attack=55 drink_health=13 drink_magicka=2 drink_stamina=6 power_attack=2 taunt=21 
melee|mixed-boss       16.0    6.7   41.1    0.57    45.9     0    0  0.0    2.6     10      5     95  iR=0 tR=0 attack=55 drink_health=14 drink_stamina=5 power_attack=3 taunt=23 
melee|strong1          31.4    7.7   37.8    0.51    47.8     0    0  0.0    1.8      8     20     80  iR=0 tR=0 attack=51 drink_health=17 drink_magicka=2 drink_stamina=4 power_attack=5 taunt=22 
melee|strong2          44.2    8.1   42.2    0.28    28.0     0    0  0.0    1.5      7      0    100  iR=0 tR=0 attack=46 drink_health=17 drink_magicka=3 drink_stamina=2 power_attack=7 taunt=25 
melee|weak             42.5    2.5   34.1    0.61    46.7     0    0  0.0    2.2     13     95      5  iR=0 tR=0 attack=59 drink_health=6 drink_magicka=2 drink_stamina=4 power_attack=3 taunt=26 
ranged|balanced         1.7    7.7   29.9    0.62    67.3     0    0  0.0    1.6      9     62     38  iR=2 tR=0 attack=82 drink_health=9 drink_stamina=8 
ranged|mixed-boss       3.3    7.5   29.6    0.70    55.1     0    0  0.0    1.9      8      5     95  iR=3 tR=0 attack=81 drink_health=11 drink_stamina=6 
ranged|strong1          0.2   11.0   28.1    0.45    54.2     0    0  0.0    1.4      7     28     72  iR=0 tR=0 attack=80 drink_health=11 drink_stamina=7 
ranged|strong2          0.1   14.3   26.1    0.58    37.3     0    0  0.0    1.3      7      5     95  iR=0 tR=0 attack=84 drink_health=9 drink_stamina=6 
ranged|weak             4.3    4.7   32.1    0.36    61.9     0    0  0.0    1.8      9     90     10  iR=4 tR=0 attack=82 drink_health=3 drink_magicka=2 drink_stamina=8 flee=5 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
baseline written: /tmp/claude-1000/-mnt-gaming-modlists-Projects-marth-follower-overhaul/a935cdde-4a9b-4a50-983f-339ce7b41062/scratchpad/base_Tuxborn.txt
```

## Findings for the coordinator (see the report for the reasoning)
See the card report; nothing in native/ was changed.
