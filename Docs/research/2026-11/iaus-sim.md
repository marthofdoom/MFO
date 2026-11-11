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
S3 a control spell sets `paralysed`/`fleeing` for the effect's own duration. S4 pool = spells some BOOK teaches (DATA flag 0x04), stored SPIT cost used when non-zero (the effect-cost formula only when it is 0; see Cost), cost scaled by skill, no invented cost.
S5 Hold fires order[0] as a continuation. S6 gates written from the ClassTable T1/T2 rows, tree ON and tree OFF (Q63) cells, Knight and Evoker T2 cells, mage staff option. S7 taunt: 70% success, 10 s duration.
S9 SPIT charge time offset 12, S11 MGEF guard >= 88 inside the try, `s` GMSTs skipped. Metrics: healed% only over episodes that had a heal candidate (deaths reported separately), iC% = idle among ticks with a candidate, iR% = idle with a candidate and a foe in reach.
**Q63 flag:** `SpellBook::GateFor` with the tree OFF still derives the gate from the T1 row (melee T1: `spellsOnClass=false`, no power attack / taunt / disengage). The sim EMULATES Q63 (everything allowed) in the `*-off` cells. The native change belongs to I6.
Nature counts after round 3 (taught spells only, Offense = hostile OR detrimental effect as CasterConsent::ClassifySpell): LoreRim 1050 of 4549 taught: offense 390, control 73, heal 30, buff 170, summon 20, ward 0; dropped placement/target-location 257, not castable 0, reanimate 5, unknown (script-costliest) 105. Tuxborn 1207 of 3881 taught: offense 447, control 43, heal 47, buff 158, summon 4, ward 0; dropped placement 362, not castable 0, reanimate 5, unknown (script) 141.
Ward stays 0 on purpose: the sim mirrors a real cast/ gap (SpellShape::Ward is not classified from records today), being handled separately.
Cost: the stored SPIT cost is always used when non-zero (SPIT flag 0x1 = kCostOverride means the stored cost is authoritative). Only a stored 0 on a spell without the override flag is replaced by a sum of effect costs, `baseCost*(mag*dur)^1.1`, an APPROXIMATION of the engine's auto-calc.
BEFORE (802272d, same seed): LoreRim mage offense 1-8% of fired casts and control 20-50%, mage Idle 9-33%, heal spells 21-28%; those were classifier/geometry artefacts (control swallowed damage spells, every ally "inside every blast").

## Results AFTER (seed 1, 8 fights x 5 ranks per cell)
Columns: idle/hold = % of ticks, sw/m = decisions switched per minute, heal.s = seconds from HP<35% to first heal action, heal% = of low-HP episodes that HAD a heal candidate, noHC% = episodes with none, holdL = % of low-HP-with-heal-candidate ticks that were Hold (R2), iC% / iR% idle with a candidate / and a foe in reach, tnt/m taunts per minute, tR% taunts on a foe still taunted. bad = 0 and inv = 0 everywhere.
### built-in
```
IAUS SIM  list=builtin  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=10 taught/kept=10  natures: offense=4 control=2 heal=2 buff=1 summon=1 ward=0  dropped: placement=0 notCastable=0 reanimate=0 unknown(script)=0  potions(h/m/s)=100/100/100
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced    11.1   6.2  28.8   1.53   80.0    17  23.6  11.0  7.6    0   5.1    92     8   0.0     0  attack=34 cast_buff=1 cast_control=5 cast_heal=25 cast_offense=20 cast_summon=1 drink_health=2 drink_ma
evoker-T2|mixed-boss  14.5   5.8  24.0   0.74   59.4    18  22.3  14.4 10.6    0   5.4    62    38   0.0     0  attack=34 cast_control=5 cast_heal=30 cast_offense=20 drink_magicka=7 flee=1 
evoker-T2|strong1     11.2   8.2  27.6   0.78   73.2     5   8.3  11.2 10.1    0   5.1    48    52   0.0     0  attack=32 cast_buff=1 cast_control=4 cast_heal=40 cast_offense=11 drink_health=3 drink_magicka=8 
evoker-T2|strong2     16.9   7.0  24.2   0.68   58.5    10  18.4  16.9  7.2    0   3.8    15    85   0.0     0  attack=34 cast_buff=1 cast_control=7 cast_heal=28 cast_offense=17 drink_health=2 drink_magicka=8 flee=1
evoker-T2|weak        15.3   3.8  31.9  -1.00   -1.0    -1  -1.0  15.3 14.9    0   6.5   100     0   0.0     0  attack=26 cast_buff=5 cast_control=2 cast_heal=24 cast_offense=30 cast_summon=2 drink_magicka=10 
knight-T2|balanced    23.4   4.6  28.5   1.36   96.0    36   9.1  23.4  0.0    0   1.9    70    30  12.2     0  attack=63 drink_health=8 drink_stamina=4 power_attack=3 taunt=21 
knight-T2|mixed-boss  21.4   3.6  33.2   0.49   82.9    43   8.4  21.4  0.0    0   2.3     2    98  16.1     0  attack=54 drink_health=14 drink_magicka=1 drink_stamina=3 power_attack=2 taunt=26 
knight-T2|strong1     17.1   4.7  28.5   0.56   59.5    35  18.0  17.1  0.0    0   1.8    22    78  12.2     0  attack=59 drink_health=12 drink_magicka=3 power_attack=4 taunt=21 
knight-T2|strong2     26.4   9.5  37.7   0.56   53.3    20  33.2  26.4  0.0    0   1.6     2    98  15.1     0  attack=44 drink_health=19 drink_magicka=5 drink_stamina=3 power_attack=5 taunt=24 
knight-T2|weak        32.3   2.1  33.5   1.23  100.0    33   7.3  32.3  0.0    0   2.2    95     5  18.2     0  attack=56 drink_health=3 drink_magicka=3 drink_stamina=2 power_attack=3 taunt=33 
mage-off|balanced     12.4   5.1  29.4   1.30   80.0     0  41.2  12.4  7.4    0   5.9    98     2   0.0     0  attack=34 cast_control=9 cast_heal=26 cast_offense=19 cast_summon=1 drink_magicka=8 power_attack=1 
mage-off|mixed-boss   18.6   4.1  25.8   0.60   58.8    19   9.2  18.6 13.5    0   6.3    52    48   0.0     0  attack=29 cast_control=6 cast_heal=27 cast_offense=25 cast_summon=1 drink_health=2 drink_magicka=6 flee
mage-off|strong1      16.5   6.6  24.6   0.87   73.7    14  19.5  16.5  5.1    0   4.7    50    50   0.0     0  attack=21 cast_control=8 cast_heal=30 cast_offense=28 drink_health=2 drink_magicka=8 flee=1 
mage-off|strong2      22.6   5.1  22.8   1.09   59.3    11  10.2  22.6 18.1    0   4.4    20    80   0.0     0  attack=28 cast_control=9 cast_heal=31 cast_offense=17 drink_health=2 drink_magicka=6 flee=5 
mage-off|weak         10.6   5.2  32.7  -1.00   -1.0    -1  -1.0  10.6  9.9    0   7.4   100     0   0.0     0  attack=23 cast_buff=5 cast_control=4 cast_heal=26 cast_offense=30 cast_summon=2 drink_magicka=10 
mage|balanced          9.8   4.6  28.9   0.85   68.8    11   8.1   9.8  9.7    0   5.2    85    15   0.0     0  attack=36 cast_buff=2 cast_control=4 cast_heal=31 cast_offense=17 drink_health=2 drink_magicka=6 
mage|mixed-boss       14.5   6.3  24.0   0.60   67.4     7  14.7  14.5 10.4    0   5.5    60    40   0.0     0  attack=33 cast_control=5 cast_heal=31 cast_offense=21 drink_health=2 drink_magicka=7 
mage|strong1           4.7   8.6  27.8   1.02   69.8    10  24.8   4.7  4.4    0   5.0    48    52   0.0     0  attack=27 cast_control=4 cast_heal=31 cast_offense=25 drink_health=3 drink_magicka=8 
mage|strong2          25.8   5.0  23.5   0.45   65.4    10   8.7  25.8 16.2    0   3.8    22    78   0.0     0  attack=28 cast_control=7 cast_heal=37 cast_offense=15 drink_health=3 drink_magicka=9 
mage|weak             14.6   3.1  34.4   0.13  100.0    50   0.0  14.5 12.3    0   5.9   100     0   0.0     0  attack=27 cast_buff=4 cast_control=3 cast_heal=27 cast_offense=24 cast_summon=3 drink_magicka=10 drink_
melee-off|balanced    17.4   5.9  35.1   1.02   72.2    10  24.1  17.4  0.0    0   3.6    82    18   6.9     1  attack=25 cast_buff=2 cast_control=7 cast_heal=21 cast_offense=12 cast_summon=2 drink_health=4 drink_ma
melee-off|mixed-boss  22.2   7.0  31.2   0.50   62.5    22  30.7  22.2  0.0    0   4.1    30    70   5.5    14  attack=19 cast_buff=1 cast_control=10 cast_heal=23 cast_offense=21 cast_summon=1 drink_health=3 drink_m
melee-off|strong1     18.3   8.4  34.4   0.84   68.9    18  10.6  18.2  0.0    0   3.7    40    60   5.5     0  attack=21 cast_control=10 cast_heal=19 cast_offense=20 cast_summon=2 drink_health=5 drink_magicka=9 dri
melee-off|strong2     22.5   7.8  26.8   0.37   66.7    21  16.7  22.5  0.0    0   2.6    30    70   2.8     4  attack=8 cast_control=8 cast_heal=33 cast_offense=24 drink_health=3 drink_magicka=8 flee=8 power_attack
melee-off|weak         8.7   5.5  42.7   1.26  100.0    60  23.8   8.6  0.0    0   4.3   100     0  12.2     0  attack=18 cast_buff=2 cast_control=4 cast_heal=10 cast_offense=31 cast_summon=3 drink_health=1 drink_ma
melee|balanced        27.2   3.2  21.6   1.08   85.7    32  18.1  27.1  0.0    0   1.1    75    25   0.0     0  attack=84 drink_health=10 drink_magicka=2 drink_stamina=4 
melee|mixed-boss      20.6   5.9  23.5   0.66   52.3    28  29.2  20.6  0.0    0   1.4     8    92   0.0     0  attack=86 drink_health=9 drink_magicka=2 drink_stamina=3 
melee|strong1         18.9   6.4  17.3   0.68   64.9    29  16.8  18.9  0.0    0   1.1    28    72   0.0     0  attack=88 drink_health=7 drink_magicka=2 drink_stamina=3 
melee|strong2         27.3  11.0  14.9   0.45   39.5    17  31.8  27.3  0.0    0   1.1    15    85   0.0     0  attack=89 drink_health=8 drink_magicka=1 drink_stamina=2 
melee|weak            36.1   1.5  27.1   0.49  100.0    50   0.0  36.1  0.0    0   1.3    98     2   0.0     0  attack=90 drink_health=2 drink_magicka=3 drink_stamina=5 
ranged-off|balanced    2.3   8.6  30.6   1.32   82.4    29  31.0   2.2  1.3    0   3.5    85    15   0.0     0  attack=58 cast_buff=1 cast_control=3 cast_heal=16 cast_offense=7 cast_summon=2 drink_health=3 drink_mag
ranged-off|mixed-boss   5.9  11.0  26.0   0.73   58.5    18  13.7   5.9  0.7    0   3.9    35    65   0.0     0  attack=55 cast_control=5 cast_heal=14 cast_offense=11 drink_health=4 drink_magicka=5 drink_stamina=3 f
ranged-off|strong1    13.3   8.9  22.8   1.11   58.6    24  29.6  13.3  2.1    0   3.1    55    45   0.0     0  attack=59 cast_control=8 cast_heal=11 cast_offense=10 drink_health=3 drink_magicka=5 drink_stamina=2 
ranged-off|strong2    12.6  17.0  23.4   0.82   46.3    20  37.0  12.6  1.7    0   2.5    15    85   0.0     0  attack=65 cast_control=8 cast_heal=11 cast_offense=5 drink_health=3 drink_magicka=4 drink_stamina=2 fle
ranged-off|weak        4.2   5.7  39.0   0.93   50.0     0  42.9   4.2  2.2    0   4.4   100     0   0.0     0  attack=54 cast_buff=2 cast_control=4 cast_heal=13 cast_offense=12 cast_summon=3 drink_magicka=8 drink_s
ranged|balanced        5.2   5.8  21.4   0.90   85.2    29  18.8   5.1  4.7    0   1.5    72    28   0.0     0  attack=88 drink_health=5 drink_stamina=6 
ranged|mixed-boss      0.1  15.3  18.0   0.66   66.0    24  35.3   0.1  0.0    0   1.8    15    85   0.0     0  attack=88 drink_health=8 drink_stamina=4 
ranged|strong1         0.4   7.5  20.3   0.85   65.1    23  18.4   0.4  0.3    0   1.4    28    72   0.0     0  attack=86 drink_health=9 drink_stamina=5 
ranged|strong2         0.1  16.0  18.4   0.51   38.5    19  30.1   0.1  0.0    0   1.1    10    90   0.0     0  attack=89 drink_health=7 drink_stamina=4 
ranged|weak            2.6   3.6  34.5   0.07   66.7    40   0.0   2.5  1.9    0   1.7   100     0   0.0     0  attack=89 drink_health=1 drink_stamina=9 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
baseline written: tools/iaus_sim/baseline_builtin.txt
```
### LoreRim (Requiem)
```
IAUS SIM  list=LoreRim  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=4549 taught/kept=1050  natures: offense=390 control=73 heal=30 buff=170 summon=20 ward=0  dropped: placement=257 notCastable=0 reanimate=5 unknown(script)=105  potions(h/m/s)=500/500/500
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced    16.8   5.6  20.7   1.78   88.5    10  24.2  16.8 16.5    0   3.9    68    32   0.0     0  attack=48 cast_buff=4 cast_control=5 cast_heal=23 cast_offense=6 drink_health=4 drink_magicka=8 drink_s
evoker-T2|mixed-boss  14.7   4.5  25.0   0.81   72.5    20  13.1  14.7 14.5    0   4.9    15    85   0.0     0  attack=31 cast_buff=5 cast_control=9 cast_heal=21 cast_offense=10 drink_health=5 drink_magicka=11 flee=
evoker-T2|strong1     13.4   4.8  23.2   0.86   69.1     8   9.9  13.4 13.5    0   3.8    32    68   0.0     0  attack=31 cast_buff=4 cast_control=7 cast_heal=33 cast_offense=8 drink_health=4 drink_magicka=11 drink_
evoker-T2|strong2     21.0   2.3  27.0   0.62   53.8     7   6.0  21.0 13.2    0   3.3    15    85   0.0     0  attack=39 cast_buff=4 cast_control=12 cast_heal=17 cast_offense=11 drink_health=5 drink_magicka=11 
evoker-T2|weak        21.1   3.0  21.9   1.55  100.0     0  21.1  21.1 20.7    0   4.0   100     0   0.0     0  attack=53 cast_buff=6 cast_control=2 cast_heal=18 cast_offense=8 drink_health=1 drink_magicka=8 drink_s
knight-T2|balanced    20.3   6.7  26.7   1.34   82.1    33  28.1  20.3  0.0    0   1.9    70    30  11.7     0  attack=67 drink_health=6 drink_stamina=4 power_attack=3 taunt=20 
knight-T2|mixed-boss  16.9   8.9  31.5   0.67   62.5    28  46.1  16.9  0.0    0   2.3     8    92  15.0     0  attack=60 drink_health=8 drink_magicka=1 drink_stamina=4 power_attack=3 taunt=24 
knight-T2|strong1     13.0   4.9  24.3   0.96   73.9    31  20.9  13.0  0.0    0   1.7    25    75   8.3     0  attack=71 drink_health=8 drink_magicka=2 drink_stamina=2 power_attack=3 taunt=14 
knight-T2|strong2     22.7  10.4  38.2   0.53   49.1    17  37.9  22.7  0.0    0   1.7     2    98  13.5     0  attack=49 drink_health=15 drink_magicka=4 drink_stamina=3 power_attack=7 taunt=22 
knight-T2|weak        21.0   5.5  23.2   5.85  100.0    55  55.6  20.9  0.0    0   2.0    92     8  12.8     0  attack=70 drink_health=2 drink_magicka=2 drink_stamina=3 power_attack=1 taunt=22 
mage-off|balanced     26.3   4.7  20.1   1.37   83.3    23  17.6  26.3 26.5    0   3.8    72    28   0.0     0  attack=40 cast_buff=4 cast_control=6 cast_heal=21 cast_offense=11 drink_health=3 drink_magicka=9 drink_
mage-off|mixed-boss   18.4   3.5  25.5   0.71   77.0    15  10.4  18.4 17.6    0   4.8    20    80   0.0     0  attack=36 cast_buff=4 cast_control=5 cast_heal=21 cast_offense=8 drink_health=7 drink_magicka=8 drink_s
mage-off|strong1      11.0   5.1  25.7   1.14   70.3    14  11.2  11.0 11.1    0   4.2    30    70   0.0     0  attack=32 cast_buff=5 cast_control=11 cast_heal=21 cast_offense=14 drink_health=2 drink_magicka=10 flee
mage-off|strong2       3.8   7.6  25.9   0.52   41.5    11  19.4   3.8  3.8    0   4.3     5    95   0.0     0  attack=30 cast_buff=4 cast_control=9 cast_heal=19 cast_offense=23 drink_health=2 drink_magicka=8 flee=1
mage-off|weak         23.8   5.0  22.4   2.08   85.7     0  17.5  23.8 22.7    0   5.2   100     0   0.0     0  attack=35 cast_buff=11 cast_control=3 cast_heal=25 cast_offense=12 cast_summon=1 drink_health=2 drink_m
mage|balanced         28.0   3.8  14.6   0.98   78.6     3  14.0  28.0 27.7    0   3.1    72    25   0.0     0  attack=42 cast_buff=4 cast_control=1 cast_heal=28 cast_offense=12 drink_health=3 drink_magicka=7 drink_
mage|mixed-boss       27.8   4.5  21.5   0.65   58.0    11  16.1  27.8 26.4    0   4.0    12    88   0.0     0  attack=38 cast_buff=4 cast_control=6 cast_heal=28 cast_offense=9 drink_health=3 drink_magicka=10 drink_
mage|strong1          21.4   4.1  22.6   1.23   71.1    20  10.8  21.4 19.2    0   3.2    25    75   0.0     0  attack=34 cast_buff=4 cast_control=9 cast_heal=20 cast_offense=15 drink_health=6 drink_magicka=10 drink
mage|strong2           8.8   6.3  25.0   0.64   62.5    14   7.1   8.8  9.1    0   3.2     8    92   0.0     0  attack=41 cast_buff=4 cast_control=9 cast_heal=21 cast_offense=12 drink_health=4 drink_magicka=8 
mage|weak             29.1   2.5  18.3   1.08  100.0    18   8.4  29.1 28.9    0   3.9    92     8   0.0     0  attack=42 cast_buff=9 cast_control=3 cast_heal=24 cast_offense=10 drink_health=1 drink_magicka=9 drink_
melee-off|balanced     9.0   5.6  34.0   0.93   83.3    16  29.1   8.9  0.0    0   3.3    70    30   8.5     0  attack=42 cast_buff=6 cast_control=4 cast_heal=5 cast_offense=8 drink_health=6 drink_magicka=6 drink_st
melee-off|mixed-boss  16.0   4.5  37.5   1.15   73.8    31  13.7  16.0  0.0    0   3.8    20    80   7.9     0  attack=36 cast_buff=7 cast_control=2 cast_heal=3 cast_offense=17 cast_summon=1 drink_health=7 drink_mag
melee-off|strong1     10.8   8.2  36.0   0.65   71.9    15  19.6  10.7  0.0    0   3.1    25    75   9.9    13  attack=37 cast_buff=4 cast_control=4 cast_heal=6 cast_offense=12 drink_health=10 drink_magicka=6 drink_
melee-off|strong2     12.2   3.3  36.1   0.42   58.1    28  15.2  12.2  0.0    0   2.5    12    88   5.1     0  attack=22 cast_buff=5 cast_control=3 cast_heal=7 cast_offense=28 drink_health=7 drink_magicka=11 flee=5
melee-off|weak        11.8   4.5  32.3   1.92  100.0    12  24.1  11.8  0.0    0   3.7   100     0  11.4     0  attack=40 cast_buff=5 cast_control=2 cast_heal=4 cast_offense=16 drink_health=2 drink_magicka=7 drink_s
melee|balanced        16.6   6.1  16.1   1.48   85.7    22  40.1  16.6  0.0    0   1.1    70    30   0.0     0  attack=91 drink_health=5 drink_magicka=1 drink_stamina=3 
melee|mixed-boss      15.8   8.3  21.4   0.96   68.8    26  37.4  15.8  0.0    0   1.3    18    82   0.0     0  attack=88 drink_health=7 drink_magicka=1 drink_stamina=3 
melee|strong1         17.7  12.1  15.9   0.77   51.7    34  61.5  17.7  0.0    0   1.0    32    68   0.0     0  attack=90 drink_health=5 drink_magicka=1 drink_stamina=4 
melee|strong2         25.5  11.5  15.6   0.48   36.8    19  43.9  25.5  0.0    0   1.0    15    85   0.0     0  attack=91 drink_health=6 drink_magicka=1 drink_stamina=3 
melee|weak            18.8   4.3  16.9   1.97  100.0    38  41.8  18.8  0.0    0   1.4    88    12   0.0     0  attack=93 drink_health=2 drink_magicka=1 drink_stamina=3 
ranged-off|balanced    5.7   5.6  23.8   1.57   93.3    14  28.8   5.7  5.6    0   2.8    88    12   0.0     0  attack=73 cast_buff=3 cast_control=5 cast_heal=3 cast_offense=6 drink_health=4 drink_magicka=4 drink_st
ranged-off|mixed-boss   0.8   9.7  25.3   0.93   64.3    23  15.5   0.8  0.8    0   3.4     5    95   0.0     0  attack=73 cast_buff=2 cast_control=2 cast_heal=1 cast_offense=7 drink_health=6 drink_magicka=3 drink_s
ranged-off|strong1     0.4  11.2  21.8   1.08   63.4    29  39.9   0.4  0.4    0   2.8    25    75   0.0     0  attack=68 cast_buff=2 cast_control=5 cast_heal=3 cast_offense=7 drink_health=5 drink_magicka=3 drink_st
ranged-off|strong2     9.6  15.5  19.2   0.46   48.6    29  36.9   9.5  0.0    0   2.2    12    88   0.0     0  attack=71 cast_buff=2 cast_control=3 cast_heal=1 cast_offense=8 drink_health=4 drink_magicka=3 drink_st
ranged-off|weak        2.2   5.6  24.9   1.80   90.9    31  39.7   2.1  1.4    0   3.7    95     5   0.0     0  attack=71 cast_buff=5 cast_heal=3 cast_offense=9 drink_health=2 drink_magicka=6 drink_stamina=3 flee=1 
ranged|balanced        4.9  10.2  20.1   2.00   83.3    33  28.8   4.9  4.6    0   1.4    75    25   0.0     0  attack=90 drink_health=3 drink_magicka=1 drink_stamina=5 
ranged|mixed-boss      1.8  10.7  18.0   0.73   63.3    23  29.5   1.8  1.7    0   1.8    15    85   0.0     0  attack=90 drink_health=6 drink_stamina=4 
ranged|strong1         3.9   7.5  16.8   0.91   66.0    18  26.8   3.9  3.9    0   1.3    35    65   0.0     0  attack=90 drink_health=6 drink_stamina=4 
ranged|strong2         0.0  14.9  16.3   0.49   42.9    21  36.8   0.0  0.0    0   1.1     8    92   0.0     0  attack=92 drink_health=5 drink_stamina=3 
ranged|weak            5.2   3.5  24.0   0.40  100.0    50   0.0   5.2  4.7    0   1.6   100     0   0.0     0  attack=92 drink_stamina=7 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
```
### Tuxborn (SimonRim)
```
IAUS SIM  list=Tuxborn  seed=1  fights/cell=8 (x5 rapport ranks)  taunted-visible=0
spell pool: raw=3881 taught/kept=1207  natures: offense=447 control=43 heal=47 buff=158 summon=4 ward=0  dropped: placement=362 notCastable=0 reanimate=5 unknown(script)=141  potions(h/m/s)=300/300/300
class|bracket         idle  hold  sw/m heal.s  heal% noHC% holdL   iC%  iR%  inv dec.us  win% death tnt/m   tR%  actions (% of fired)
evoker-T2|balanced    12.9   4.0  26.4   0.62   89.2    10   1.8  12.9  9.6    0   4.9    68    32   0.0     0  attack=23 cast_buff=4 cast_control=3 cast_heal=40 cast_offense=15 drink_health=3 drink_magicka=9 drink_
evoker-T2|mixed-boss  20.3   3.9  20.0   0.75   63.2     7   5.6  20.2  4.3    0   5.5    28    72   0.0     0  attack=30 cast_buff=6 cast_control=5 cast_heal=35 cast_offense=15 drink_magicka=7 
evoker-T2|strong1     32.1   3.3  22.0   0.45   75.0     5   9.1  32.1 17.2    0   3.9    28    72   0.0     0  attack=21 cast_buff=3 cast_control=4 cast_heal=32 cast_offense=24 drink_health=4 drink_magicka=11 
evoker-T2|strong2     22.2   2.1  24.4   0.52   52.2     8   1.3  22.2  3.8    0   3.9    10    90   0.0     0  attack=15 cast_buff=2 cast_control=7 cast_heal=43 cast_offense=14 drink_health=5 drink_magicka=9 flee=4
evoker-T2|weak        13.7   3.7  27.9   0.43  100.0     0  19.1  13.7 13.5    0   6.1    90    10   0.0     0  attack=19 cast_buff=8 cast_heal=45 cast_offense=16 drink_health=1 drink_magicka=8 drink_stamina=2 
knight-T2|balanced    24.7   7.3  34.9   1.22   70.0    29  23.4  24.6  0.0    0   1.9    55    45  13.2     0  attack=58 drink_health=9 drink_magicka=1 drink_stamina=7 power_attack=3 taunt=22 
knight-T2|mixed-boss  29.6   4.0  42.0   0.41   62.5    37  15.9  29.6  0.0    0   2.3     2    98  20.1     0  attack=46 drink_health=12 drink_magicka=2 drink_stamina=5 power_attack=2 taunt=33 
knight-T2|strong1     20.4   6.5  32.2   0.53   52.5    31  33.5  20.4  0.0    0   1.7    20    80  13.7     0  attack=57 drink_health=11 drink_magicka=4 power_attack=4 taunt=23 
knight-T2|strong2     35.7   5.9  47.3   0.33   47.9    16  26.5  35.7  0.0    0   1.7     0   100  20.0     0  attack=31 drink_health=21 drink_magicka=7 drink_stamina=5 power_attack=5 taunt=31 
knight-T2|weak        36.9   1.0  36.5   0.96   90.0    33  13.2  36.8  0.0    0   2.2    95     5  18.6     0  attack=51 drink_health=6 drink_magicka=3 drink_stamina=4 power_attack=2 taunt=34 
mage-off|balanced      4.8   6.2  26.7   0.80   87.5     6   2.1   4.8  4.1    0   6.0    65    35   0.0     0  attack=25 cast_buff=5 cast_control=2 cast_heal=30 cast_offense=21 cast_summon=1 drink_health=2 drink_ma
mage-off|mixed-boss   22.2   6.9  25.4   0.50   63.2     7  10.2  22.2  5.8    0   6.4    18    82   0.0     0  attack=23 cast_buff=5 cast_control=5 cast_heal=36 cast_offense=12 drink_health=6 drink_magicka=8 drink_
mage-off|strong1      27.0   7.1  20.5   0.57   69.8     4  12.7  27.0  4.6    0   4.3    30    70   0.0     0  attack=17 cast_buff=2 cast_control=8 cast_heal=42 cast_offense=19 drink_health=2 drink_magicka=8 flee=2
mage-off|strong2      26.4   3.2  24.9   0.47   43.5     8   5.8  26.4  9.6    0   4.1    12    88   0.0     0  attack=12 cast_buff=5 cast_control=13 cast_heal=36 cast_offense=17 drink_health=5 drink_magicka=8 flee=
mage-off|weak         18.8   3.9  28.4   0.71  100.0     0   5.8  18.8 17.9    0   7.0    95     5   0.0     0  attack=16 cast_buff=9 cast_control=2 cast_heal=37 cast_offense=21 drink_health=1 drink_magicka=10 drink
mage|balanced          5.0   7.3  25.5   0.88   72.4     3  11.7   5.0  4.9    0   5.0    70    30   0.0     0  attack=25 cast_buff=4 cast_control=1 cast_heal=43 cast_offense=14 drink_health=2 drink_magicka=10 
mage|mixed-boss       24.1   4.4  23.7   0.63   63.0     6   8.3  24.1  6.5    0   4.8    35    65   0.0     0  attack=34 cast_buff=6 cast_control=5 cast_heal=24 cast_offense=20 drink_health=3 drink_magicka=6 drink_
mage|strong1          20.4   4.7  25.4   0.62   60.0     8  10.7  20.3  4.8    0   4.2    20    80   0.0     0  attack=30 cast_buff=6 cast_control=7 cast_heal=28 cast_offense=17 drink_health=1 drink_magicka=9 drink_
mage|strong2          36.5   2.5  22.2   0.40   39.5    12  17.5  36.4 21.2    0   3.6     8    92   0.0     0  attack=23 cast_buff=5 cast_control=10 cast_heal=13 cast_offense=36 drink_health=5 drink_magicka=8 
mage|weak             17.0   4.8  26.4   0.88   81.2    11  15.0  17.0 17.0    0   5.8    88    12   0.0     0  attack=21 cast_buff=9 cast_heal=32 cast_offense=25 drink_health=2 drink_magicka=9 drink_stamina=1 
melee-off|balanced    20.3   4.0  31.8   0.66   80.0    17  12.8  20.3  0.0    0   3.7    70    30   5.5     0  attack=12 cast_buff=3 cast_control=4 cast_heal=7 cast_offense=33 drink_health=7 drink_magicka=8 drink_s
melee-off|mixed-boss   8.5   6.1  37.8   0.59   53.1    23  16.9   8.5  0.0    0   5.2     5    95   7.7     0  attack=19 cast_buff=7 cast_heal=4 cast_offense=35 drink_health=9 drink_magicka=9 flee=3 taunt=14 
melee-off|strong1     15.8   5.0  33.1   0.76   59.2    16  11.6  15.8  0.0    0   3.7    18    82   6.3     0  attack=22 cast_buff=3 cast_control=3 cast_heal=10 cast_offense=30 drink_health=11 drink_magicka=7 power
melee-off|strong2     34.1   2.5  25.9   0.51   34.4    29   5.9  34.1  0.0    0   2.5     8    92   4.0     0  attack=12 cast_buff=6 cast_control=2 cast_heal=13 cast_offense=37 cast_summon=1 drink_health=6 drink_ma
melee-off|weak        14.5   3.3  40.5   0.51  100.0    25   0.0  14.4  0.0    0   3.9    98     2  10.5     0  attack=16 cast_buff=6 cast_heal=17 cast_offense=26 drink_health=2 drink_magicka=7 drink_stamina=2 flee=
melee|balanced        32.0   3.5  26.4   0.63   72.0    22  30.1  32.0  0.0    0   1.1    72    28   0.0     0  attack=84 drink_health=9 drink_magicka=2 drink_stamina=5 
melee|mixed-boss      18.5   5.1  26.5   0.90   42.1    28  31.1  18.5  0.0    0   1.3     0   100   0.0     0  attack=86 drink_health=6 drink_magicka=2 drink_stamina=5 
melee|strong1         21.3   7.2  20.0   0.28   41.7    25  27.5  21.3  0.0    0   1.0    12    88   0.0     0  attack=88 drink_health=5 drink_magicka=2 drink_stamina=5 
melee|strong2         32.0  10.1  19.3   0.13   35.0    18  28.2  32.0  0.0    0   1.0    10    90   0.0     0  attack=88 drink_health=7 drink_magicka=1 drink_stamina=4 
melee|weak            38.6   1.2  29.6   0.83  100.0    11  32.8  38.6  0.0    0   1.3    98     2   0.0     0  attack=86 drink_health=5 drink_magicka=3 drink_stamina=6 
ranged-off|balanced    3.5   7.7  30.2   0.85   58.3    10  10.0   3.5  0.6    0   3.6    62    38   0.0     0  attack=58 cast_buff=4 cast_heal=4 cast_offense=18 drink_health=5 drink_magicka=5 drink_stamina=4 
ranged-off|mixed-boss   1.3   8.8  30.6   0.97   37.0    15  13.5   1.3  0.6    0   4.7     5    95   0.0     0  attack=42 cast_buff=2 cast_control=2 cast_heal=7 cast_offense=34 drink_health=4 drink_magicka=5 drink_
ranged-off|strong1    10.6  10.5  25.2   0.29   36.8    14  16.9  10.5  6.0    0   3.1    25    75   0.0     0  attack=57 cast_buff=3 cast_control=3 cast_heal=10 cast_offense=16 drink_health=3 drink_magicka=5 drink_
ranged-off|strong2     0.0  17.5  30.0   0.34   36.6    18  27.1   0.0  0.0    0   2.7     0   100   0.0     0  attack=51 cast_buff=2 cast_control=2 cast_heal=9 cast_offense=24 drink_health=5 drink_magicka=4 drink_s
ranged-off|weak        3.0   6.6  38.4   0.73  100.0    27  24.6   2.9  2.3    0   4.6    92     8   0.0     0  attack=46 cast_buff=5 cast_heal=14 cast_offense=21 drink_health=2 drink_magicka=7 drink_stamina=6 
ranged|balanced        6.9   6.8  23.3   1.18   68.2    27  15.7   6.9  6.5    0   1.4    68    32   0.0     0  attack=89 drink_health=4 drink_magicka=1 drink_stamina=6 
ranged|mixed-boss      0.0  16.3  25.3   1.09   44.4    21  32.9   0.0  0.0    0   1.9     2    98   0.0     0  attack=87 drink_health=7 drink_stamina=5 
ranged|strong1         0.2   8.2  23.5   0.58   39.5    14  20.9   0.2  0.0    0   1.4    25    75   0.0     0  attack=86 drink_health=6 drink_stamina=7 
ranged|strong2         0.1  22.7  24.5   0.77   30.6    20  36.3   0.1  0.0    0   1.1     8    92   0.0     0  attack=89 drink_health=5 drink_stamina=6 
ranged|weak            1.0   3.6  33.6   1.38   83.3    14  21.6   1.0  0.3    0   1.6    98     2   0.0     0  attack=86 drink_health=2 drink_stamina=11 
CHECKS: BAD-FACTS ticks=0 (must be 0)  invalid picks=0 (must be 0)  => PASS
```

## Remaining findings after the artefacts are gone

> **Stale (coordinator note, 2026-11-10):** the numbers in this section predate 0e76c37 (the cost-flag fix F1). The tables above are current, e.g. mage attack 21-42%, mage idle with candidates 5-37%, mage holdL 7-25%. Real findings that still stand: R2 heal held behind the window (I6: no re-anchor), taunt with no cooldown (I6), ward never classified (cast/Archetype.cpp, I6 must-list).
- **R2 CONFIRMED:** 5-44% of the ticks where the follower is under 35% HP with a heal candidate offered are Hold (melee 17-44% in LoreRim/Tuxborn, mage 1-25%). The commitment window holds the running non-heal decision while a heal waits (heal.s 0.4-1.6 s, up to 2x slower in strong brackets). marth's "no re-anchor" choice for I6 targets exactly this.
- **R1 PARTLY CONFIRMED:** `Candidates.cpp` never sets `Combatant::taunted` (grep: no assignment), so the FoeTaunted gate cannot fire in game. In the sim `--taunt-visible` changes nothing (redundant taunts 0-14% either way), because taunt is offered only for foes hitting an ally and a landed taunt retargets the foe. But the taunt rate is 12-20 per minute for Knight (one every 3-5 s) with no cooldown, 22-33% of its fired actions in weak fights: a cooldown consideration is still missing.
- Hold 8-17% for ranged in strong brackets stays (the window keeps a bow shot running after the target dies).
- Mage with a staff or spells: Idle is now 0.7-26%; LoreRim/mage balanced 19% idle with candidates present is the highest. The mage "attack" share (6-36%) means a mage melees with a dagger when out of magicka or spells are vetoed; check the offense score floor.
- Heal spells are 31-61% of a mage's fired casts (Tuxborn up to 61% for balanced evoker): the heal score probably lifts too easily when only the follower's own HP sags (allies start 40-100%). Look at the heal consideration weights before trusting.
- Tree OFF (Q63) casting melee: offense casts are 20-41% of its actions (melee-off in LoreRim/Tuxborn) and attack drops to 6-25%: a spell book beats a swing for a non-caster under the current weights. Possibly intended, worth a decision.
- Switches/min stays 15-47 (every 1.3-4 s).
- Not in scope (S8, movement model): approach time and Idle during the walk-in.
