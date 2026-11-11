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

## Fix round (review of 802272d): what changed
S1 real x/y geometry (player at origin, foes on rays ahead of the follower, allies near the player; `dist` derived, foes walk to their target) so the splash-safe gate is no longer always false.
S2 `Classify` mirrors `Prune::ClassifyNature` precedence (reanimate, summon, ward, control archetypes only, self cloak, any hostile = Offense, heal, script/undeclared = Unknown) and takes resistAv from the costliest hostile effect.
S3 a control spell sets `paralysed`/`fleeing` for the effect's own duration. S4 pool = spells some BOOK teaches (DATA flag 0x04), auto-calculate spells costed from effect base costs, cost scaled by skill, no invented cost.
S5 Hold fires order[0] as a continuation. S6 gates written from the ClassTable T1/T2 rows, tree ON and tree OFF (Q63) cells, Knight and Evoker T2 cells, mage staff option. S7 taunt: 70% success, 10 s duration.
S9 SPIT charge time offset 12, S11 MGEF guard >= 88 inside the try, `s` GMSTs skipped. Metrics: healed% only over episodes that had a heal candidate (deaths reported separately), iC% = idle among ticks with a candidate, iR% = idle with a candidate and a foe in reach.
**Q63 flag:** `SpellBook::GateFor` with the tree OFF still derives the gate from the T1 row (melee T1: `spellsOnClass=false`, no power attack / taunt / disengage). The sim EMULATES Q63 (everything allowed) in the `*-off` cells. The native change belongs to I6.
New nature counts (taught spells only): LoreRim offense 375, control 73, heal 31, buff 177, summon 20, ward 0, unknown dropped 374 (of 1050 taught, 4549 total). Tuxborn offense 421, control 43, heal 52, buff 161, summon 4, ward 0, unknown dropped 526 (of 1207 taught, 3881 total). Ward is 0 because the sim approximates SpellShape::Ward as a beneficial ValueMod on kWardPower, which no record uses.
BEFORE (802272d, same seed): LoreRim mage offense 1-8% of fired casts and control 20-50%, mage Idle 9-33%, heal spells 21-28%; those were classifier/geometry artefacts (control swallowed damage spells, every ally "inside every blast").

## Results AFTER (seed 1, 8 fights x 5 ranks per cell)
Columns: idle/hold = % of ticks, sw/m = decisions switched per minute, heal.s = seconds from HP<35% to first heal action, heal% = of low-HP episodes that HAD a heal candidate, noHC% = episodes with none, holdL = % of low-HP-with-heal-candidate ticks that were Hold (R2), iC% / iR% idle with a candidate / and a foe in reach, tnt/m taunts per minute, tR% taunts on a foe still taunted. bad = 0 and inv = 0 everywhere.
### built-in
```
IAUS SIM  list=builtin  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=10 taught/kept=10  natures: offense=4 control=2 heal=2 buff=1 summon=1 ward=0  dropped-unknown=0  potions(h/m/s)=100/100/100
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced    11.1   6.2  28.8   1.53   80.0    17  23.6  11.0  7.6    0   5.0    92     8   0.0     0  attack=34 cast_buff=1 cast_control=5 cast_heal=25 cast_offense=20 cast_summon=1 drink_health=2 drink_ma
evoker-T2|mixed-boss  14.5   5.8  24.0   0.74   59.4    18  22.3  14.4 10.6    0   5.3    62    38   0.0     0  attack=34 cast_control=5 cast_heal=30 cast_offense=20 drink_magicka=7 flee=1 
evoker-T2|strong1     11.2   8.2  27.6   0.78   73.2     5   8.3  11.2 10.1    0   5.1    48    52   0.0     0  attack=32 cast_buff=1 cast_control=4 cast_heal=40 cast_offense=11 drink_health=3 drink_magicka=8 
evoker-T2|strong2     16.9   7.0  24.2   0.68   58.5    10  18.4  16.9  7.2    0   3.8    15    85   0.0     0  attack=34 cast_buff=1 cast_control=7 cast_heal=28 cast_offense=17 drink_health=2 drink_magicka=8 flee=1
evoker-T2|weak        15.3   3.8  31.9  -1.00   -1.0    -1  -1.0  15.3 14.9    0   6.4   100     0   0.0     0  attack=26 cast_buff=5 cast_control=2 cast_heal=24 cast_offense=30 cast_summon=2 drink_magicka=10 
knight-T2|balanced    23.4   4.6  28.5   1.36   96.0    36   9.1  23.4  0.0    0   2.0    70    30  12.2     0  attack=63 drink_health=8 drink_stamina=4 power_attack=3 taunt=21 
knight-T2|mixed-boss  21.4   3.6  33.2   0.49   82.9    43   8.4  21.4  0.0    0   2.3     2    98  16.1     0  attack=54 drink_health=14 drink_magicka=1 drink_stamina=3 power_attack=2 taunt=26 
knight-T2|strong1     17.1   4.7  28.5   0.56   59.5    35  18.0  17.1  0.0    0   1.9    22    78  12.2     0  attack=59 drink_health=12 drink_magicka=3 power_attack=4 taunt=21 
knight-T2|strong2     26.4   9.5  37.7   0.56   53.3    20  33.2  26.4  0.0    0   1.6     2    98  15.1     0  attack=44 drink_health=19 drink_magicka=5 drink_stamina=3 power_attack=5 taunt=24 
knight-T2|weak        32.3   2.1  33.5   1.23  100.0    33   7.3  32.3  0.0    0   2.1    95     5  18.2     0  attack=56 drink_health=3 drink_magicka=3 drink_stamina=2 power_attack=3 taunt=33 
mage-off|balanced     12.4   5.1  29.4   1.30   80.0     0  41.2  12.4  7.4    0   5.9    98     2   0.0     0  attack=34 cast_control=9 cast_heal=26 cast_offense=19 cast_summon=1 drink_magicka=8 power_attack=1 
mage-off|mixed-boss   18.6   4.1  25.8   0.60   58.8    19   9.2  18.6 13.5    0   6.1    52    48   0.0     0  attack=29 cast_control=6 cast_heal=27 cast_offense=25 cast_summon=1 drink_health=2 drink_magicka=6 flee
mage-off|strong1      16.5   6.6  24.6   0.87   73.7    14  19.5  16.5  5.1    0   4.6    50    50   0.0     0  attack=21 cast_control=8 cast_heal=30 cast_offense=28 drink_health=2 drink_magicka=8 flee=1 
mage-off|strong2      22.6   5.1  22.8   1.09   59.3    11  10.2  22.6 18.1    0   4.3    20    80   0.0     0  attack=28 cast_control=9 cast_heal=31 cast_offense=17 drink_health=2 drink_magicka=6 flee=5 
mage-off|weak         10.6   5.2  32.7  -1.00   -1.0    -1  -1.0  10.6  9.9    0   7.3   100     0   0.0     0  attack=23 cast_buff=5 cast_control=4 cast_heal=26 cast_offense=30 cast_summon=2 drink_magicka=10 
mage|balanced          9.8   4.6  28.9   0.85   68.8    11   8.1   9.8  9.7    0   5.2    85    15   0.0     0  attack=36 cast_buff=2 cast_control=4 cast_heal=31 cast_offense=17 drink_health=2 drink_magicka=6 
mage|mixed-boss       14.5   6.3  24.0   0.60   67.4     7  14.7  14.5 10.4    0   5.4    60    40   0.0     0  attack=33 cast_control=5 cast_heal=31 cast_offense=21 drink_health=2 drink_magicka=7 
mage|strong1           4.7   8.6  27.8   1.02   69.8    10  24.8   4.7  4.4    0   4.9    48    52   0.0     0  attack=27 cast_control=4 cast_heal=31 cast_offense=25 drink_health=3 drink_magicka=8 
mage|strong2          25.8   5.0  23.5   0.45   65.4    10   8.7  25.8 16.2    0   3.8    22    78   0.0     0  attack=28 cast_control=7 cast_heal=37 cast_offense=15 drink_health=3 drink_magicka=9 
mage|weak             14.6   3.1  34.4   0.13  100.0    50   0.0  14.5 12.3    0   5.7   100     0   0.0     0  attack=27 cast_buff=4 cast_control=3 cast_heal=27 cast_offense=24 cast_summon=3 drink_magicka=10 drink_
melee-off|balanced    17.4   5.9  35.1   1.02   72.2    10  24.1  17.4  0.0    0   3.6    82    18   6.9     1  attack=25 cast_buff=2 cast_control=7 cast_heal=21 cast_offense=12 cast_summon=2 drink_health=4 drink_ma
melee-off|mixed-boss  22.2   7.0  31.2   0.50   62.5    22  30.7  22.2  0.0    0   4.1    30    70   5.5    14  attack=19 cast_buff=1 cast_control=10 cast_heal=23 cast_offense=21 cast_summon=1 drink_health=3 drink_m
melee-off|strong1     18.3   8.4  34.4   0.84   68.9    18  10.6  18.2  0.0    0   3.7    40    60   5.5     0  attack=21 cast_control=10 cast_heal=19 cast_offense=20 cast_summon=2 drink_health=5 drink_magicka=9 dri
melee-off|strong2     22.5   7.8  26.8   0.37   66.7    21  16.7  22.5  0.0    0   2.6    30    70   2.8     4  attack=8 cast_control=8 cast_heal=33 cast_offense=24 drink_health=3 drink_magicka=8 flee=8 power_attack
melee-off|weak         8.7   5.5  42.7   1.26  100.0    60  23.8   8.6  0.0    0   4.3   100     0  12.2     0  attack=18 cast_buff=2 cast_control=4 cast_heal=10 cast_offense=31 cast_summon=3 drink_health=1 drink_ma
melee|balanced        27.2   3.2  21.6   1.08   85.7    32  18.1  27.1  0.0    0   1.1    75    25   0.0     0  attack=84 drink_health=10 drink_magicka=2 drink_stamina=4 
melee|mixed-boss      20.6   5.9  23.5   0.66   52.3    28  29.2  20.6  0.0    0   1.3     8    92   0.0     0  attack=86 drink_health=9 drink_magicka=2 drink_stamina=3 
melee|strong1         18.9   6.4  17.3   0.68   64.9    29  16.8  18.9  0.0    0   1.0    28    72   0.0     0  attack=88 drink_health=7 drink_magicka=2 drink_stamina=3 
melee|strong2         27.3  11.0  14.9   0.45   39.5    17  31.8  27.3  0.0    0   1.0    15    85   0.0     0  attack=89 drink_health=8 drink_magicka=1 drink_stamina=2 
melee|weak            36.1   1.5  27.1   0.49  100.0    50   0.0  36.1  0.0    0   1.3    98     2   0.0     0  attack=90 drink_health=2 drink_magicka=3 drink_stamina=5 
ranged-off|balanced    2.3   8.6  30.6   1.32   82.4    29  31.0   2.2  1.3    0   3.4    85    15   0.0     0  attack=58 cast_buff=1 cast_control=3 cast_heal=16 cast_offense=7 cast_summon=2 drink_health=3 drink_mag
ranged-off|mixed-boss   5.9  11.0  26.0   0.73   58.5    18  13.7   5.9  0.7    0   3.8    35    65   0.0     0  attack=55 cast_control=5 cast_heal=14 cast_offense=11 drink_health=4 drink_magicka=5 drink_stamina=3 f
ranged-off|strong1    13.3   8.9  22.8   1.11   58.6    24  29.6  13.3  2.1    0   3.0    55    45   0.0     0  attack=59 cast_control=8 cast_heal=11 cast_offense=10 drink_health=3 drink_magicka=5 drink_stamina=2 
ranged-off|strong2    12.6  17.0  23.4   0.82   46.3    20  37.0  12.6  1.7    0   2.5    15    85   0.0     0  attack=65 cast_control=8 cast_heal=11 cast_offense=5 drink_health=3 drink_magicka=4 drink_stamina=2 fle
ranged-off|weak        4.2   5.7  39.0   0.93   50.0     0  42.9   4.2  2.2    0   4.4   100     0   0.0     0  attack=54 cast_buff=2 cast_control=4 cast_heal=13 cast_offense=12 cast_summon=3 drink_magicka=8 drink_s
ranged|balanced        5.2   5.8  21.4   0.90   85.2    29  18.8   5.1  4.7    0   1.5    72    28   0.0     0  attack=88 drink_health=5 drink_stamina=6 
ranged|mixed-boss      0.1  15.3  18.0   0.66   66.0    24  35.3   0.1  0.0    0   1.8    15    85   0.0     0  attack=88 drink_health=8 drink_stamina=4 
ranged|strong1         0.4   7.5  20.3   0.85   65.1    23  18.4   0.4  0.3    0   1.4    28    72   0.0     0  attack=86 drink_health=9 drink_stamina=5 
ranged|strong2         0.1  16.0  18.4   0.51   38.5    19  30.1   0.1  0.0    0   1.1    10    90   0.0     0  attack=89 drink_health=7 drink_stamina=4 
ranged|weak            2.6   3.6  34.5   0.07   66.7    40   0.0   2.5  1.9    0   1.9   100     0   0.0     0  attack=89 drink_health=1 drink_stamina=9 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
baseline written: tools/iaus_sim/baseline_builtin.txt
```
### LoreRim (Requiem)
```
IAUS SIM  list=LoreRim  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=4549 taught/kept=1050  natures: offense=375 control=73 heal=31 buff=177 summon=20 ward=0  dropped-unknown=374  potions(h/m/s)=500/500/500
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced     8.6   7.5  18.5   1.57   90.0     3   5.9   8.6  8.4    0   4.7    70    30   0.0     0  attack=33 cast_buff=4 cast_control=3 cast_heal=36 cast_offense=16 drink_health=2 drink_magicka=5 drink_
evoker-T2|mixed-boss   3.6   7.0  21.8   0.59   80.3     9  11.2   3.6  3.6    0   6.6    18    82   0.0     0  attack=17 cast_buff=4 cast_control=7 cast_heal=36 cast_offense=28 drink_health=1 drink_magicka=5 
evoker-T2|strong1      0.0   5.5  18.2   0.69   79.3     0   3.0   0.0  0.0    0   5.4    25    75   0.0     0  attack=10 cast_buff=2 cast_control=4 cast_heal=43 cast_offense=34 drink_magicka=7 
evoker-T2|strong2     19.1   5.4  19.8   0.39   71.7     5   6.3  19.1 19.1    0   4.1    12    88   0.0     0  attack=18 cast_buff=2 cast_control=9 cast_heal=34 cast_offense=27 drink_health=3 drink_magicka=5 flee=2
evoker-T2|weak        12.4   7.1  19.3   1.26  100.0     0  18.9  12.4 12.1    0   6.3   100     0   0.0     0  attack=32 cast_buff=5 cast_control=2 cast_heal=23 cast_offense=31 drink_magicka=4 drink_stamina=2 
knight-T2|balanced    20.3   6.7  26.7   1.34   82.1    33  28.1  20.3  0.0    0   1.9    70    30  11.7     0  attack=67 drink_health=6 drink_stamina=4 power_attack=3 taunt=20 
knight-T2|mixed-boss  16.9   8.9  31.5   0.67   62.5    28  46.1  16.9  0.0    0   2.3     8    92  15.0     0  attack=60 drink_health=8 drink_magicka=1 drink_stamina=4 power_attack=3 taunt=24 
knight-T2|strong1     13.0   4.9  24.3   0.96   73.9    31  20.9  13.0  0.0    0   1.7    25    75   8.3     0  attack=71 drink_health=8 drink_magicka=2 drink_stamina=2 power_attack=3 taunt=14 
knight-T2|strong2     22.7  10.4  38.2   0.53   49.1    17  37.9  22.7  0.0    0   1.7     2    98  13.5     0  attack=49 drink_health=15 drink_magicka=4 drink_stamina=3 power_attack=7 taunt=22 
knight-T2|weak        21.0   5.5  23.2   5.85  100.0    55  55.6  20.9  0.0    0   2.0    92     8  12.8     0  attack=70 drink_health=2 drink_magicka=2 drink_stamina=3 power_attack=1 taunt=22 
mage-off|balanced     18.4   7.1  17.6   1.02   80.0     9  18.1  18.3 18.3    0   5.3    68    32   0.0     0  attack=25 cast_buff=3 cast_control=6 cast_heal=34 cast_offense=23 drink_health=2 drink_magicka=5 
mage-off|mixed-boss    9.2   7.8  23.0   0.63   73.8     4   9.7   9.2  7.6    0   7.1    25    75   0.0     0  attack=25 cast_buff=3 cast_control=10 cast_heal=35 cast_offense=18 drink_health=2 drink_magicka=5 drink
mage-off|strong1       7.5   7.6  24.7   0.76   74.1    11   9.1   7.5  7.5    0   5.4    35    65   0.0     0  attack=15 cast_buff=2 cast_control=11 cast_heal=44 cast_offense=18 drink_health=2 drink_magicka=8 
mage-off|strong2      10.2   8.0  19.8   0.44   61.8     5  10.5  10.2 10.3    0   4.2    10    90   0.0     0  attack=19 cast_buff=3 cast_control=7 cast_heal=31 cast_offense=29 drink_health=2 drink_magicka=6 flee=2
mage-off|weak         16.8   6.9  19.1   1.41  100.0     0   0.7  16.8 15.7    0   7.1    95     5   0.0     0  attack=23 cast_buff=9 cast_control=2 cast_heal=33 cast_offense=24 cast_summon=2 drink_magicka=6 
mage|balanced         18.8   5.3  17.7   1.14   86.2     9   5.3  18.8 18.8    0   4.3    78    20   0.0     0  attack=26 cast_buff=4 cast_control=4 cast_heal=32 cast_offense=26 drink_health=2 drink_magicka=6 
mage|mixed-boss        8.3   7.0  19.1   0.65   68.9     3  15.3   8.3  8.4    0   5.7    12    88   0.0     0  attack=18 cast_buff=3 cast_control=7 cast_heal=33 cast_offense=31 drink_health=3 drink_magicka=5 
mage|strong1           7.3   6.3  18.9   0.71   73.9     5   9.2   7.3  5.0    0   4.5    32    68   0.0     0  attack=18 cast_buff=3 cast_control=11 cast_heal=42 cast_offense=15 drink_health=3 drink_magicka=6 
mage|strong2           3.9   6.4  23.2   0.65   62.5     2  10.3   3.9  3.8    0   4.3    10    90   0.0     0  attack=14 cast_buff=3 cast_control=8 cast_heal=41 cast_offense=25 drink_health=1 drink_magicka=7 
mage|weak             28.2   3.6  18.5   1.73  100.0    43   0.0  28.2 26.8    0   4.9    90     8   0.0     0  attack=25 cast_buff=8 cast_control=3 cast_heal=28 cast_offense=26 drink_magicka=7 drink_stamina=1 
melee-off|balanced     5.2   7.8  28.7   1.30   80.5    13  21.0   5.2  0.0    0   4.1    75    25   5.5     0  attack=21 cast_buff=2 cast_control=6 cast_heal=3 cast_offense=41 drink_health=5 drink_magicka=4 drink_s
melee-off|mixed-boss   4.9  10.7  32.8   0.88   68.8    26  18.8   4.9  0.0    0   5.6    10    90   8.2     0  attack=18 cast_buff=4 cast_control=4 cast_heal=9 cast_offense=36 drink_health=5 drink_magicka=7 power_a
melee-off|strong1      7.2   8.2  28.9   1.14   67.9    16  18.2   7.2  0.0    0   3.6    20    78   4.8     0  attack=25 cast_buff=2 cast_control=7 cast_heal=11 cast_offense=30 drink_health=6 drink_magicka=6 drink_
melee-off|strong2      6.6   6.4  33.1   0.38   59.5    26  19.3   6.6  0.0    0   2.9    15    85   4.2     0  attack=13 cast_buff=2 cast_control=6 cast_heal=5 cast_offense=41 cast_summon=2 drink_health=5 drink_mag
melee-off|weak         6.1   9.6  31.1   1.68   88.9    25  19.7   6.0  0.0    0   4.3   100     0  10.8     0  attack=26 cast_buff=4 cast_control=2 cast_heal=3 cast_offense=36 drink_health=2 drink_magicka=5 drink_s
melee|balanced        16.6   6.1  16.1   1.48   85.7    22  40.1  16.6  0.0    0   1.1    70    30   0.0     0  attack=91 drink_health=5 drink_magicka=1 drink_stamina=3 
melee|mixed-boss      15.8   8.3  21.4   0.96   68.8    26  37.4  15.8  0.0    0   1.3    18    82   0.0     0  attack=88 drink_health=7 drink_magicka=1 drink_stamina=3 
melee|strong1         17.7  12.1  15.9   0.77   51.7    34  61.5  17.7  0.0    0   1.0    32    68   0.0     0  attack=90 drink_health=5 drink_magicka=1 drink_stamina=4 
melee|strong2         25.5  11.5  15.6   0.48   36.8    19  43.9  25.5  0.0    0   1.0    15    85   0.0     0  attack=91 drink_health=6 drink_magicka=1 drink_stamina=3 
melee|weak            18.8   4.3  16.9   1.97  100.0    38  41.8  18.8  0.0    0   1.3    88    12   0.0     0  attack=93 drink_health=2 drink_magicka=1 drink_stamina=3 
ranged-off|balanced    6.5   6.1  20.0   1.35   92.3    16  30.9   6.5  5.5    0   3.1    90    10   0.0     0  attack=49 cast_control=3 cast_heal=5 cast_offense=32 drink_health=4 drink_magicka=3 drink_stamina=2 
ranged-off|mixed-boss   0.9  10.7  26.6   0.97   72.4    21  14.0   0.9  0.3    0   3.8    12    88   0.0     0  attack=57 cast_buff=3 cast_control=6 cast_heal=4 cast_offense=16 drink_health=6 drink_magicka=4 drink_
ranged-off|strong1     0.1  11.2  22.0   0.64   72.2    22  38.7   0.0  0.0    0   3.4    28    72   0.0     0  attack=54 cast_control=6 cast_heal=6 cast_offense=21 drink_health=5 drink_magicka=4 drink_stamina=2 
ranged-off|strong2     1.6  11.2  20.4   0.44   42.9    27  40.6   1.6  1.6    0   2.8    12    88   0.0     0  attack=57 cast_buff=1 cast_control=2 cast_heal=2 cast_offense=23 drink_health=4 drink_magicka=4 drink_s
ranged-off|weak        2.6   6.2  25.7   1.66   85.7    30   8.1   2.6  1.7    0   4.3    95     5   0.0     0  attack=59 cast_buff=4 cast_control=2 cast_heal=3 cast_offense=19 drink_health=2 drink_magicka=4 drink_s
ranged|balanced        4.9  10.2  20.1   2.00   83.3    33  28.8   4.9  4.6    0   1.4    75    25   0.0     0  attack=90 drink_health=3 drink_magicka=1 drink_stamina=5 
ranged|mixed-boss      1.8  10.7  18.0   0.73   63.3    23  29.5   1.8  1.7    0   1.7    15    85   0.0     0  attack=90 drink_health=6 drink_stamina=4 
ranged|strong1         3.9   7.5  16.8   0.91   66.0    18  26.8   3.9  3.9    0   1.3    35    65   0.0     0  attack=90 drink_health=6 drink_stamina=4 
ranged|strong2         0.0  14.9  16.3   0.49   42.9    21  36.8   0.0  0.0    0   1.1     8    92   0.0     0  attack=92 drink_health=5 drink_stamina=3 
ranged|weak            5.2   3.5  24.0   0.40  100.0    50   0.0   5.2  4.7    0   1.6   100     0   0.0     0  attack=92 drink_stamina=7 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
```
### Tuxborn (SimonRim)
```
IAUS SIM  list=Tuxborn  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=3881 taught/kept=1207  natures: offense=421 control=43 heal=52 buff=161 summon=4 ward=0  dropped-unknown=526  potions(h/m/s)=300/300/300
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced     0.1   5.3  19.5   0.73   81.8     3   2.6   0.1  0.0    0   6.3    42    58   0.0     0  attack=6 cast_buff=6 cast_control=3 cast_heal=61 cast_offense=15 cast_summon=1 drink_health=1 drink_mag
evoker-T2|mixed-boss   9.2   3.4  25.9   0.92   40.9     4   3.4   9.2  5.8    0   7.0    12    88   0.0     0  attack=10 cast_buff=4 cast_control=20 cast_heal=33 cast_offense=25 drink_health=3 drink_magicka=5 
evoker-T2|strong1      5.1   3.0  26.0   0.73   66.7     2   6.8   5.1  4.3    0   5.8    15    85   0.0     0  attack=8 cast_buff=4 cast_control=18 cast_heal=39 cast_offense=22 drink_health=3 drink_magicka=6 
evoker-T2|strong2      3.9   3.9  24.4   0.76   45.5     4   9.4   3.9  3.8    0   5.5     5    95   0.0     0  attack=17 cast_buff=1 cast_control=16 cast_heal=40 cast_offense=18 drink_magicka=6 flee=1 
evoker-T2|weak         4.0   4.2  22.4   0.92  100.0     0   0.0   4.0  3.7    0   7.4    92     8   0.0     0  attack=11 cast_buff=9 cast_control=5 cast_heal=47 cast_offense=19 drink_health=1 drink_magicka=6 
knight-T2|balanced    24.7   7.3  34.9   1.22   70.0    29  23.4  24.6  0.0    0   1.9    55    45  13.2     0  attack=58 drink_health=9 drink_magicka=1 drink_stamina=7 power_attack=3 taunt=22 
knight-T2|mixed-boss  29.6   4.0  42.0   0.41   62.5    37  15.9  29.6  0.0    0   2.3     2    98  20.1     0  attack=46 drink_health=12 drink_magicka=2 drink_stamina=5 power_attack=2 taunt=33 
knight-T2|strong1     20.4   6.5  32.2   0.53   52.5    31  33.5  20.4  0.0    0   1.7    20    80  13.7     0  attack=57 drink_health=11 drink_magicka=4 power_attack=4 taunt=23 
knight-T2|strong2     35.7   5.9  47.3   0.33   47.9    16  26.5  35.7  0.0    0   1.6     0   100  20.0     0  attack=31 drink_health=21 drink_magicka=7 drink_stamina=5 power_attack=5 taunt=31 
knight-T2|weak        36.9   1.0  36.5   0.96   90.0    33  13.2  36.8  0.0    0   2.2    95     5  18.6     0  attack=51 drink_health=6 drink_magicka=3 drink_stamina=4 power_attack=2 taunt=34 
mage-off|balanced      3.9   4.0  24.7   0.74   72.4     3   7.1   3.9  1.5    0   6.6    65    35   0.0     0  attack=10 cast_buff=8 cast_control=16 cast_heal=37 cast_offense=19 drink_health=2 drink_magicka=5 drink
mage-off|mixed-boss    2.1   6.2  25.3   0.54   56.1     2  13.6   2.1  1.9    0   8.8    10    90   0.0     0  attack=12 cast_buff=4 cast_control=14 cast_heal=48 cast_offense=15 cast_summon=2 drink_health=2 drink_m
mage-off|strong1       4.6   5.0  26.5   0.76   54.2     2   8.2   4.6  4.6    0   6.1    20    80   0.0     0  attack=10 cast_buff=4 cast_control=19 cast_heal=38 cast_offense=19 cast_summon=1 drink_health=3 drink_m
mage-off|strong2       5.1   4.1  28.4   0.58   38.1     5   3.7   5.1  5.1    0   5.0     2    98   0.0     0  attack=6 cast_buff=2 cast_control=19 cast_heal=31 cast_offense=30 drink_health=4 drink_magicka=7 
mage-off|weak          8.4   3.0  21.5   0.65   90.9     0   2.5   8.4  8.3    0   7.5    92     8   0.0     0  attack=10 cast_buff=7 cast_control=7 cast_heal=57 cast_offense=11 drink_magicka=7 
mage|balanced          0.7   2.8  20.5   0.74   76.3     7   2.5   0.7  0.7    0   5.5    60    40   0.0     0  attack=16 cast_buff=6 cast_control=6 cast_heal=51 cast_offense=11 drink_health=3 drink_magicka=5 drink_
mage|mixed-boss        7.4   3.9  27.3   0.54   57.4     4   9.4   7.4  7.5    0   6.3     5    95   0.0     0  attack=23 cast_buff=3 cast_control=16 cast_heal=29 cast_offense=15 cast_summon=2 drink_health=5 drink_m
mage|strong1           1.3   5.3  25.8   0.80   63.6     6   6.2   1.3  1.3    0   5.2    12    88   0.0     0  attack=14 cast_buff=5 cast_control=12 cast_heal=46 cast_offense=10 drink_health=2 drink_magicka=10 drin
mage|strong2           5.9   6.3  28.9   0.69   39.5     7   0.9   5.9  5.9    0   4.4     0   100   0.0     0  attack=22 cast_buff=3 cast_control=19 cast_heal=31 cast_offense=19 drink_health=2 drink_magicka=4 
mage|weak             10.8   2.3  22.8   0.81  100.0     0   1.6  10.8 10.8    0   7.3    82    18   0.0     0  attack=8 cast_buff=10 cast_control=7 cast_heal=49 cast_offense=17 drink_health=1 drink_magicka=8 
melee-off|balanced     6.8   4.0  34.1   1.12   86.4    10   6.8   6.8  0.0    0   4.2    60    40   7.4     0  attack=14 cast_buff=6 cast_heal=12 cast_offense=32 drink_health=9 drink_magicka=8 drink_stamina=1 flee=
melee-off|mixed-boss   4.1   6.0  39.8   0.69   51.0    17   7.4   4.1  0.0    0   5.6     2    98   8.2     0  attack=16 cast_buff=6 cast_control=2 cast_heal=15 cast_offense=27 drink_health=7 drink_magicka=8 drink_
melee-off|strong1      2.8  10.2  37.3   0.56   57.9    12  23.3   2.8  0.0    0   4.4    15    85   7.2     0  attack=14 cast_buff=5 cast_control=2 cast_heal=9 cast_offense=34 drink_health=12 drink_magicka=6 drink_
melee-off|strong2      1.6   4.5  30.5   0.54   32.4    21   7.0   1.6  0.0    0   3.3     2    98   5.6     0  attack=6 cast_buff=5 cast_heal=28 cast_offense=41 drink_health=2 drink_magicka=7 taunt=9 
melee-off|weak         7.6   5.3  31.2   1.04  100.0    29   0.7   7.4  0.0    0   3.9    95     5   8.5     0  attack=12 cast_buff=6 cast_heal=24 cast_offense=28 drink_health=1 drink_magicka=9 taunt=18 
melee|balanced        32.0   3.5  26.4   0.63   72.0    22  30.1  32.0  0.0    0   1.1    72    28   0.0     0  attack=84 drink_health=9 drink_magicka=2 drink_stamina=5 
melee|mixed-boss      18.5   5.1  26.5   0.90   42.1    28  31.1  18.5  0.0    0   1.3     0   100   0.0     0  attack=86 drink_health=6 drink_magicka=2 drink_stamina=5 
melee|strong1         21.3   7.2  20.0   0.28   41.7    25  27.5  21.3  0.0    0   1.0    12    88   0.0     0  attack=88 drink_health=5 drink_magicka=2 drink_stamina=5 
melee|strong2         32.0  10.1  19.3   0.13   35.0    18  28.2  32.0  0.0    0   1.0    10    90   0.0     0  attack=88 drink_health=7 drink_magicka=1 drink_stamina=4 
melee|weak            38.6   1.2  29.6   0.83  100.0    11  32.8  38.6  0.0    0   1.2    98     2   0.0     0  attack=86 drink_health=5 drink_magicka=3 drink_stamina=6 
ranged-off|balanced    0.4   6.7  23.4   1.04   75.0    22  10.7   0.3  0.1    0   3.5    60    40   0.0     0  attack=57 cast_buff=4 cast_heal=4 cast_offense=23 drink_health=5 drink_magicka=3 drink_stamina=3 
ranged-off|mixed-boss   1.1  11.4  27.2   0.54   34.9    20  18.2   1.1  1.0    0   5.0     5    95   0.0     0  attack=46 cast_buff=4 cast_heal=10 cast_offense=29 drink_health=5 drink_magicka=4 drink_stamina=2 
ranged-off|strong1     3.0  12.3  30.3   0.39   51.3    22  15.2   3.0  3.0    0   3.9    20    80   0.0     0  attack=46 cast_buff=3 cast_control=8 cast_heal=5 cast_offense=24 drink_health=6 drink_magicka=5 drink_s
ranged-off|strong2     0.0  13.5  30.9   0.41   42.5    27  48.7   0.0  0.0    0   2.8     0   100   0.0     0  attack=45 cast_buff=5 cast_control=3 cast_heal=3 cast_offense=28 drink_health=7 drink_magicka=4 drink_s
ranged-off|weak        4.3   5.0  32.6   1.30  100.0    33   7.8   4.3  3.6    0   4.6    88    12   0.0     0  attack=38 cast_buff=7 cast_heal=10 cast_offense=26 drink_health=2 drink_magicka=7 drink_stamina=4 flee=
ranged|balanced        6.9   6.8  23.3   1.18   68.2    27  15.7   6.9  6.5    0   1.3    68    32   0.0     0  attack=89 drink_health=4 drink_magicka=1 drink_stamina=6 
ranged|mixed-boss      0.0  16.3  25.3   1.09   44.4    21  32.9   0.0  0.0    0   1.9     2    98   0.0     0  attack=87 drink_health=7 drink_stamina=5 
ranged|strong1         0.2   8.2  23.5   0.58   39.5    14  20.9   0.2  0.0    0   1.4    25    75   0.0     0  attack=86 drink_health=6 drink_stamina=7 
ranged|strong2         0.1  22.7  24.5   0.77   30.6    20  36.3   0.1  0.0    0   1.1     8    92   0.0     0  attack=89 drink_health=5 drink_stamina=6 
ranged|weak            1.0   3.6  33.6   1.38   83.3    14  21.6   1.0  0.3    0   1.6    98     2   0.0     0  attack=86 drink_health=2 drink_stamina=11 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
```

## Remaining findings after the artefacts are gone
- **R2 CONFIRMED:** 5-44% of the ticks where the follower is under 35% HP with a heal candidate offered are Hold (melee 17-44% in LoreRim/Tuxborn, mage 1-25%). The commitment window holds the running non-heal decision while a heal waits (heal.s 0.4-1.6 s, up to 2x slower in strong brackets). marth's "no re-anchor" choice for I6 targets exactly this.
- **R1 PARTLY CONFIRMED:** `Candidates.cpp` never sets `Combatant::taunted` (grep: no assignment), so the FoeTaunted gate cannot fire in game. In the sim `--taunt-visible` changes nothing (redundant taunts 0-14% either way), because taunt is offered only for foes hitting an ally and a landed taunt retargets the foe. But the taunt rate is 12-20 per minute for Knight (one every 3-5 s) with no cooldown, 22-33% of its fired actions in weak fights: a cooldown consideration is still missing.
- Hold 8-17% for ranged in strong brackets stays (the window keeps a bow shot running after the target dies).
- Mage with a staff or spells: Idle is now 0.7-26%; LoreRim/mage balanced 19% idle with candidates present is the highest. The mage "attack" share (6-36%) means a mage melees with a dagger when out of magicka or spells are vetoed; check the offense score floor.
- Heal spells are 31-61% of a mage's fired casts (Tuxborn up to 61% for balanced evoker): the heal score probably lifts too easily when only the follower's own HP sags (allies start 40-100%). Look at the heal consideration weights before trusting.
- Tree OFF (Q63) casting melee: offense casts are 20-41% of its actions (melee-off in LoreRim/Tuxborn) and attack drops to 6-25%: a spell book beats a swing for a non-caster under the current weights. Possibly intended, worth a decision.
- Switches/min stays 15-47 (every 1.3-4 s).
- Not in scope (S8, movement model): approach time and Idle during the walk-in.
