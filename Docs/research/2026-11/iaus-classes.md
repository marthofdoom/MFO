# IAUS class playstyles and stealth: Classes Beta 1 (tiers 1 and 2)

Fable author, 2026-11-11, branch `feat/iaus-classes`. Lab and docs only: nothing under `native/` changed.

**Scope (marth 2026-11-11):** the class release order is T2 first (Classes Beta 1), then T3. This document is in
force for the 12 tier 1 and tier 2 classes and for the stealth pieces the Thief needs. Everything written for tier 3,
tier 4 and the Dragonborn before that ruling is in section 9, marked PARKED.

marth's two questions, answered for today's main:

| Question | Answer on main `6bc3f38` | After this work |
|---|---|---|
| Do thieves and stealth classes have sneak awareness? | No. The catalogue has a Sneak family but nothing generates it, `Facts::unseen` is never filled, and there is no `act.sneak`. A Thief plays as a generic bow follower. | Still no in the game. The lab now models it, the roads are audited, and the design for the input is in section 5. |
| Did IAUS use the class information we sent? | Only the gates (who may cast, power attack, taunt, flee, which weapons). The cards, the per-class rulings and the packages were never turned into playstyles. | Every T1 and T2 card is a playstyle with measurable rulings, an oracle constraint set and a personality score (sections 1 to 3). The results show where IAUS does not play the class (section 4). |

## 1. What a class playstyle is in the lab

`tools/iaus_sim/Playstyles.h` holds one row per class. Each row says what a skilled human playing that class does and
never does, taken from the class's card and rulings, and turns it into three things:

- **Bands.** A band is a metric of the results file with a bound, for example Knight `b.block >= 15`. Every band is one
  ruling in the results JSON (`rulings`, id `P-<class>-<n>`), checked on the class's natural bracket.
- **Persona.** The class-personality score, 0 to 100: the mean credit over the class's bands. A band inside its bound
  earns 1. Outside, the credit falls to 0 at half the bound (or 5, whichever is larger). 100 means an observer sees the
  class. A low score means he plays like a generic bot of his line. It is written as the metric `persona` on every cell.
- **Forbid bits.** The oracle constraint set. The expert oracle may not take an option the class's rulings forbid, so a
  class's oracle gap is measured against an expert who plays that class. They are applied only with `--class-rulings` (or `--class-oracle`, which implies it); without a flag the oracle is main's unconstrained one. `--no-class-rulings` turns them off even under `--class-oracle`.

A band's numbers come from the card, on the lab's scales. They are never fitted to what IAUS does today, so a band that
fails is a finding.

Scales on this lab (natural bracket, stealth off): a melee follower swings 45 to 52% of his time and is in contact
(a foe inside 260 units) 60 to 72%. A ranged one shoots 54 to 68% and is in contact 11 to 22%. A mage casts 42 to 65%
and is in contact 4 to 15%.

## 2. The twelve playstyles

Sources: `class-walkthrough-decisions.md` (W), `class-build-plan.md` (P), `open-questions-for-marth.md` (Q),
`native/classes/ClassTable.cpp` (T).

### Tier 1: "t1 behaves vanilla for those roles" (W Global)

| Class | Does | Never | Bands | Oracle constraints |
|---|---|---|---|---|
| Melee | Closes and fights the way the vanilla AI does, drinks when low. No class package. | Casts: he has mastered no mage class (Q34). | `melee_share_pct >= 35`, `b.cast <= 2` | none |
| Ranged | Shoots from range, swaps to melee only out of arrows or with a foe on him. | Stands in melee by choice. | `b.shoot >= 45`, `contact% <= 25` | none |
| Mage | Casts from range. Reaches for magicka, a staff or distance before a blade. | Melees while any affordable cast, magicka potion or staff charge is left (W "IAUS mage melee"). | `melee_share_pct <= 5`, `b.cast >= 40`, `contact% <= 20` | no melee |

### Tier 2

| Class | Does | Never | Bands | Oracle constraints |
|---|---|---|---|---|
| Knight | The wall. Stays on the player, taunts what attacks an ally, keeps his guard up and drops it only to attack, bashes. Little circling, no flank (W Knight, Q2). | Leaves the line, backs off, swings heavy more than a quarter of the time (ch.18 cap 0.25). | `contact% >= 60`, `pDist <= 450`, `b.block >= 15`, `powShare% <= 25`, `taunt_per_min >= 0.5`, `retreat% <= 1` | no retreat |
| Warrior | Pushes hard and head on, circles and flanks, presses with allies. Power attacks a blocking or a stronger foe, one per 10 s (W Warrior, Q7, Q73). | Backs off. | `contact% >= 55`, `powShare% >= 40`, `pow/min <= 6.5`, `retreat% <= 0.5` | no retreat |
| Mercenary | In and out: circles wide, flanks, backs away while still facing and fighting. Casters, then archers, then the weakest (W Mercenary). | Flees. Power attacks more than 15%. | `contact% >= 40`, `powShare% <= 15`, `prio% >= 60`, `retreat% <= 1` | backing away only |
| Archer | A line archer. Casters first, then archers. A one-handed weapon only with a foe on him. Picks up arrows (W Archer). | Wields a two-hander. Keeps missing the same shot (Q56: at most 5 misses in 10). | `b.shoot >= 50`, `contact% <= 20`, `melee_share_pct <= 12`, `prio% >= 55`, `miss% <= 50` | none |
| Thief | Sneaks whatever the player does. One fight in five he opens undetected, the others by his own sneak against the foes' senses. Dagger when close and flanking, else an undetected bow on the lowest-health foe. Breaks off to where nobody sees him and hides (W Thief, W Master Thief "SNEAK RULES"). | Fights in the open when he could be hidden. Uses a crossbow (Q23). | `sneak% >= 85`, `opener% >= 20`, `sneakDmg% >= 15`, `b.shoot >= 30`, `contact% <= 40`, `powShare% <= 20` | none |
| Evoker | Stays at the furthest range his spell works at, steps out of incoming spells. Fire on a fire-weak foe, else the nearest. A magicka potion when low (W Evoker). | Melees. Casts into a resistance. Blasts the party. | `b.cast >= 50`, `melee_share_pct <= 3`, `contact% <= 15`, `immune% <= 0`, `resisted% <= 15` | no melee |
| Conjurer | Summons first and stays behind it, re-summons when it falls. Shoots what attacks his summon. A dagger only with a foe on him (W Conjurer). | Fights without a summon up. | `summonUp% >= 60`, `melee_share_pct <= 3`, `contact% <= 15`, `b.cast >= 30` | no melee |
| Healer | The medic: behind the party, near the player. Heals the player, then an ally, then himself, below half. Attacks only when nobody needs him (W Healer). | Lets someone die with a heal in hand. Wanders from the player. Melees. | `pDist <= 600`, `melee_share_pct <= 3`, `contact% <= 15`, `healAt% <= 60`, `dwh/f <= 0.1`, `overheal_pct <= 35` | no melee, near the player |
| Warlock | The controller. Paralyses or frightens the stronger foe first, keeps his armor spell up, then Destruction on the nearest. Never spends a cast on a foe his spell cannot touch (W Warlock). | Casts a level-capped spell at a foe above its cap. Melees. | `a.cast_control >= 8`, `ctrlFail% <= 10`, `melee_share_pct <= 3`, `contact% <= 15` | no melee |

Every class also carries the global constraint "never an area cast that reaches the party".

New metrics behind the bands (`sim.cpp` Metrics): `contact%` (time with a foe inside 260 units), `pDist` (mean distance
to the player), `tgtDist`, `powShare%` (power attack share of his swing time), `miss%` (arrows launched that did not
hit), `summonUp%`, `healAt%` (the recipient's health when he starts a heal on another), `goInBad%`, and the stealth set:
`sneak%`, `hidden%`, `opener%` (fights whose first blow was a sneak attack), `sneakAtk%`, `sneakDmg%`, `hide/min`.

`--class-kit` gives the subject the spells his class's own default rows name (a Conjurer knows a summon, a Healer a
heal on others, a Warlock a paralysis and a fear). It is OFF by default so the headline J stays comparable with main.
The class report runs with it ON.

## 3. What the classes cannot express today (the class gap ledger, T1 and T2)

These rows are in the results JSON `gaps`, one per (gap, class), and show on the dashboard's class detail.

| Id | Classes | What is missing | Road |
|---|---|---|---|
| C-G1 | Thief | The stance in a fight. BUILT (G1), `bSneakInCombat` 0 by default, unobserved in combat. Status closing | Probe P1 |
| C-G2 | Thief | Hide-when-unseen. BUILT as G2 Layer 1 (the engine's own loss, read through the group record, the hold row, the blow from stealth). Layer 2 (the forced seat) is not built. Status closing | Probes P2, P3 |
| C-G3 | Thief | The break-off to a point no foe sees. BUILT behind `bIausHideRoad` (default off). Status closing. What the decision code still cannot express: G18, G19 (`iaus-sim-v2.md` 30.8) | A field run |
| C-FLEE2 | Evoker, Conjurer, Healer, Warlock | Card G3's flee. BUILT. Fixed 30% line, one run per 25 s, ends on arrival (G20). Status closing | A field run, then tuning |
| C-NOFLEE | Knight, Warrior, Mercenary | No disengage flag: neither Retreat nor the flee is ever generated (G21) | A ruling: by design, or a road for the Mercenary's "backs away while fighting" |
| C-G13 | Thief | Steal, pick locks | Cards G13 / G14: logistics, outside the combat lab |
| C-BLOCK | Knight | No held guard, so "blocks near-constantly" cannot be played | The Harbinger block state (question Q83), then Block as a held action |
| C-C8 | Knight, Warrior, Mercenary | Power share, circling, flanking and leash are the class package, not an IAUS decision | Card C8 (CSTY + ch.18 + ch.23) |
| C-POWER | Warrior | MFO's own power attack may land as an ordinary hit (question Q82) | The passive probe of doc iaus-sim-v2 29.1 |
| C-H3 | Evoker, Conjurer, Healer, Warlock (and Thief through G3) | Cannot leave melee by choice | Card H3, Harbinger, tier A |
| C-H4 | Evoker | Range does not follow the equipped spell | Card H4 |
| C-H5 | Archer | The miss budget cannot be played: the lab's miss rate is one fixed model number | Card H5 (ch.29, built) + a lab model of it |
| C-G5 | Conjurer | Table mode cannot summon on need | Cards G5 / G6. IAUS already has CastSummon |
| C-H2 | Conjurer, Healer | "Less likely to be targeted" is a foe-side weight | Card H2, not modelled in the lab |

Not modelled at all: the class passives' numbers (Warrior +15% melee damage, Evoker +15% Destruction and so on). Only
the Thief's "30% quieter" is in the lab, because it changes what he can do.

## 4. Results: the baseline of the T1 and T2 rulings

This is a MEASUREMENT of today's IAUS against the playstyles. No weight was tuned and nothing was changed to close a
gap: marth ruled that the T1 + T2 gap round comes after the class features are built.

Runs: three lists, the 12 classes, `--class-kit`, 10 fights x 5 rapport ranks per bracket, the class-constrained oracle
on 10 natural fights per class (K = 6, 60 s). Noise: a class's oracle gap from 10 fights moves by several U between
seeds, so read a gap under about 5 as zero. Rulings and persona come from 50 fights and are steadier.

What the tables say:

- **Persona reads the oracle too.** Where the class-constrained oracle also scores low (Knight 65 to 80, Thief 35 to 50
  with stealth off), today's ACTIONS cannot express the class: that is the gap ledger of section 3. Where the oracle
  scores high and IAUS does not (Mage on Tuxborn 33 against 100, Conjurer, Healer on Vanilla 67 against 94), the
  actions are there and the DECISION is off.
- **Mage lines melee more than the ruling allows on every list.** T1 Mage 4.6 / 17.7 / 6.7% against 5. The four T2 mages
  3 to 13% against 3. Tuxborn is the worst, and there the mages are also in contact 19 to 32% of the time.
- **Knight.** He never holds a guard (0.2 to 0.8% against 15): there is no block road. His power share is 34 to 41%
  against the cap of 25.
- **Power share fails for every melee package** (Knight, Mercenary 35 to 42 against 15, Thief 35 to 44 against 20,
  Warrior under his floor at 31 to 38 against 40). The lab runs today's package, which has no ch.18 mix. These are card
  C8's to fix, not IAUS's. The Warrior also power attacks 7 to 8.6 times a minute against 6.5: the 10 s cooldown binds
  IAUS and the rows, the engine's own power attacks come on top.
- **Conjurer.** A summon is up 0 to 30% of the time against 60, with the summon spell known.
- **Healer.** He stands 616 to 755 units from the player against 600, is in contact 19 to 24% against 15, melees 8 to
  10%, and heals a little late or early by list.
- **Archer.** Kills priority foes first 46 to 52% on two lists against 55.
- **Thief.** With stealth off nothing of his card shows (persona 33). Section 7.
- **R-NO-SPLASH** still fails for every caster (0.1 to 0.2 ally hits a fight), as on main.

### 4.1 Per class: rulings passed / total, persona (IAUS | oracle), oracle gap, by list (stealth off = the game today)

| class | LoreRim rulings | LoreRim persona I / O | LoreRim gap | Tuxborn rulings | Tuxborn persona I / O | Tuxborn gap | Vanilla rulings | Vanilla persona I / O | Vanilla gap |
|---|---|---|---|---|---|---|---|---|---|
| melee | 6 / 6 | 100 / 100 | +1.2 | 6 / 6 | 100 / 84 | +8.0 | 6 / 6 | 100 / 98 | +0.4 |
| ranged | 6 / 6 | 100 / 50 | +8.7 | 6 / 6 | 100 / 100 | +9.4 | 6 / 6 | 100 / 100 | +0.6 |
| mage | 7 / 8 | 100 / 100 | +4.0 | 4 / 8 | 33 / 100 | +18.3 | 5 / 8 | 89 / 100 | +9.5 |
| knight | 8 / 10 | 67 / 65 | -8.9 | 8 / 10 | 71 / 67 | +2.4 | 8 / 10 | 72 / 80 | +0.5 |
| warrior | 6 / 8 | 86 / 95 | +1.7 | 6 / 8 | 81 / 82 | +1.6 | 6 / 8 | 93 / 92 | +1.4 |
| mercenary | 7 / 8 | 75 / 73 | +0.2 | 7 / 8 | 75 / 67 | -0.2 | 6 / 8 | 74 / 50 | +0.9 |
| archer | 7 / 9 | 89 / 99 | +2.4 | 8 / 9 | 98 / 100 | +1.1 | 8 / 9 | 90 / 70 | +2.0 |
| thief | 6 / 10 | 33 / 50 | +0.1 | 6 / 10 | 33 / 35 | -9.3 | 6 / 10 | 33 / 49 | +0.6 |
| evoker | 7 / 10 | 99 / 100 | +11.1 | 6 / 10 | 60 / 85 | +2.0 | 8 / 10 | 93 / 94 | +2.7 |
| conjurer | 5 / 9 | 64 / 83 | +14.1 | 4 / 9 | 25 / 64 | +3.0 | 5 / 9 | 70 / 75 | +10.5 |
| healer | 4 / 11 | 65 / 65 | +25.5 | 4 / 11 | 62 / 64 | -24.6 | 6 / 11 | 67 / 94 | +8.3 |
| warlock | 5 / 9 | 85 / 61 | +1.5 | 6 / 9 | 75 / 68 | +2.3 | 6 / 9 | 92 / 57 | +9.9 |

### 4.2 Every class ruling that FAILS on at least one list (stealth off): value per list

| ruling | class | what | bound | LoreRim | Tuxborn | Vanilla |
|---|---|---|---|---|---|---|
| R-MAGE-MELEE-T1 | mage | a T1 mage almost never melees (low single digits) | melee_share_pct <= 5 | 4.6 | 17.7 FAIL | 6.7 FAIL |
| R-MAGE-MELEE-T2+ | evoker | pure mages up the tree melee even less | melee_share_pct <= 3 | 3.2 FAIL | 13.3 FAIL | 4.9 FAIL |
| R-MAGE-MELEE-T2+ | conjurer | pure mages up the tree melee even less | melee_share_pct <= 3 | 5.3 FAIL | 13.0 FAIL | 4.1 FAIL |
| R-MAGE-MELEE-T2+ | healer | pure mages up the tree melee even less | melee_share_pct <= 3 | 8.0 FAIL | 10.4 FAIL | 8.8 FAIL |
| R-MAGE-MELEE-T2+ | warlock | pure mages up the tree melee even less | melee_share_pct <= 3 | 5.8 FAIL | 9.1 FAIL | 4.7 FAIL |
| R-NO-SPLASH | mage | never splash the party | splash_ally_hits_per_fight <= 0 | 0.2 FAIL | 0.2 FAIL | 0.1 FAIL |
| R-NO-SPLASH | evoker | never splash the party | splash_ally_hits_per_fight <= 0 | 0.2 FAIL | 0.1 FAIL | 0.0 |
| R-NO-SPLASH | conjurer | never splash the party | splash_ally_hits_per_fight <= 0 | 0.1 FAIL | 0.2 FAIL | 0.1 FAIL |
| R-NO-SPLASH | healer | never splash the party | splash_ally_hits_per_fight <= 0 | 0.1 FAIL | 0.1 FAIL | 0.1 FAIL |
| R-NO-SPLASH | warlock | never splash the party | splash_ally_hits_per_fight <= 0 | 0.2 FAIL | 0.2 FAIL | 0.0 FAIL |
| P-mage-1 | mage | marth 11-10: a T1 mage almost never melees | melee_share_pct <= 5 | 4.6 | 17.7 FAIL | 6.7 FAIL |
| P-mage-3 | mage | spell range, not melee range | contact% <= 20 | 8.4 | 31.5 FAIL | 12.4 |
| P-knight-3 | knight | marth Q2: an in-your-face tank only stops blocking to attack | b.block >= 15 | 0.5 FAIL | 0.8 FAIL | 0.2 FAIL |
| P-knight-4 | knight | ch.18 power share max 0.25 | powShare% <= 25 | 41.4 FAIL | 34.0 FAIL | 33.5 FAIL |
| P-warrior-2 | warrior | ch.18 power floor 0.40 | powShare% >= 40 | 37.5 FAIL | 31.1 FAIL | 36.2 FAIL |
| P-warrior-3 | warrior | rulings Q7 / Q73: one power-attack cooldown of 10 s | pow/min <= 6.5 | 8.6 FAIL | 8.0 FAIL | 7.0 FAIL |
| P-mercenary-2 | mercenary | ch.18 power cap 0.15 | powShare% <= 15 | 42.2 FAIL | 36.5 FAIL | 34.5 FAIL |
| P-mercenary-3 | mercenary | casters, then archers, then the lowest health | prio% >= 60 | 66.7 | 64.3 | 59.4 FAIL |
| P-archer-3 | archer | melee only when a foe is inside 150 | melee_share_pct <= 12 | 13.4 FAIL | 9.1 | 15.0 FAIL |
| P-archer-4 | archer | casters, then archers | prio% >= 55 | 46.2 FAIL | 52.2 FAIL | 69.1 |
| P-thief-1 | thief | marth: sneak regardless of player action | sneak% >= 85 | 0.0 FAIL | 0.0 FAIL | 0.0 FAIL |
| P-thief-2 | thief | a free stealth entry every 5 fights, the rest by his stats | opener% >= 20 | 0.0 FAIL | 0.0 FAIL | 0.0 FAIL |
| P-thief-3 | thief | his damage comes out of stealth | sneakDmg% >= 15 | 0.0 FAIL | 0.0 FAIL | 0.0 FAIL |
| P-thief-6 | thief | ch.18 cap 0.20 (ruling Q27) | powShare% <= 20 | 43.6 FAIL | 43.8 FAIL | 35.4 FAIL |
| P-evoker-2 | evoker | marth 11-10: pure mages up the tree melee even less | melee_share_pct <= 3 | 3.2 FAIL | 13.3 FAIL | 4.9 FAIL |
| P-evoker-3 | evoker | spell range (H4) | contact% <= 15 | 5.8 | 26.3 FAIL | 14.6 |
| P-conjurer-1 | conjurer | summon first, re-summon | summonUp% >= 60 | 19.8 FAIL | 30.3 FAIL | 0.0 FAIL |
| P-conjurer-2 | conjurer | a pure mage | melee_share_pct <= 3 | 5.3 FAIL | 13.0 FAIL | 4.1 FAIL |
| P-conjurer-3 | conjurer | behind his summon | contact% <= 15 | 12.4 | 22.6 FAIL | 11.7 |
| P-healer-1 | healer | leashed near the player | pDist <= 600 | 616.1 FAIL | 701.9 FAIL | 755.3 FAIL |
| P-healer-2 | healer | a pure mage | melee_share_pct <= 3 | 8.0 FAIL | 10.4 FAIL | 8.8 FAIL |
| P-healer-3 | healer | behind the party | contact% <= 15 | 23.8 FAIL | 19.2 FAIL | 18.6 FAIL |
| P-healer-4 | healer | heals below half (his rows say 50) | healAt% <= 60 | 61.2 FAIL | 62.5 FAIL | 58.7 |
| P-healer-5 | healer | nobody goes down while he holds a heal | dwh/f <= 0.1 | 0.2 FAIL | 0.0 | 0.1 |
| P-healer-6 | healer | the heal fits the wound | overheal_pct <= 35 | 4.1 | 39.6 FAIL | 14.6 |
| P-warlock-1 | warlock | control comes first | a.cast_control >= 8 | 7.8 FAIL | 10.6 | 15.4 |
| P-warlock-3 | warlock | a pure mage | melee_share_pct <= 3 | 5.8 FAIL | 9.1 FAIL | 4.7 FAIL |

### 4.3 Thief under the stealth roads (natural bracket): mode x list

| list | mode | utility | win_pct | persona | sneak% | hidden% | opener% | sneakAtk% | sneakDmg% | hide/min | b.shoot | melee_share_pct | contact% | taken/s | dps | rulings | oracle gap | oracle persona |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| LoreRim | off | 59.5 | 62.0 | 33.3 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 58.1 | 10.0 | 21.7 | 2.6 | 17.0 | 6 / 10 | +0.1 | 50 |
| LoreRim | hide | 59.7 | 62.0 | 83.3 | 100.0 | 49.7 | 78.1 | 30.5 | 54.4 | 0.0 | 61.5 | 6.7 | 13.2 | 1.3 | 19.0 | 9 / 10 | +1.2 | 83 |
| LoreRim | aware | 59.5 | 62.0 | 83.3 | 99.1 | 49.6 | 81.2 | 35.3 | 58.4 | 0.1 | 64.8 | 5.2 | 11.1 | 1.0 | 19.1 | 9 / 10 | +1.2 | 100 |
| Tuxborn | off | 73.5 | 78.0 | 33.3 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 57.0 | 8.9 | 14.0 | 0.9 | 17.4 | 6 / 10 | -9.3 | 35 |
| Tuxborn | hide | 74.8 | 80.0 | 83.3 | 100.0 | 43.6 | 72.5 | 30.9 | 61.1 | 0.0 | 47.3 | 10.6 | 17.8 | 1.3 | 19.8 | 9 / 10 | +0.2 | 83 |
| Tuxborn | aware | 75.2 | 80.0 | 83.3 | 100.0 | 42.8 | 72.5 | 36.0 | 64.3 | 0.0 | 48.9 | 8.4 | 15.6 | 1.1 | 21.0 | 9 / 10 | +0.1 | 92 |
| Vanilla | off | 89.1 | 94.0 | 33.3 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 0.0 | 63.1 | 9.3 | 14.0 | 1.4 | 15.9 | 6 / 10 | +0.6 | 49 |
| Vanilla | hide | 90.9 | 96.0 | 83.3 | 100.0 | 37.3 | 55.0 | 22.8 | 42.7 | 0.0 | 55.9 | 8.9 | 16.0 | 1.4 | 17.5 | 9 / 10 | +0.5 | 100 |
| Vanilla | aware | 90.8 | 96.0 | 83.3 | 99.4 | 37.6 | 52.5 | 25.5 | 44.9 | 0.3 | 55.4 | 8.9 | 16.2 | 1.5 | 17.8 | 9 / 10 | -0.2 | 100 |

### 4.4 Mean over the 12 classes: U, persona, oracle gap by mode (only the thief changes with the mode)

| list | mode | mean U | mean persona | mean oracle gap | rulings passed |
|---|---|---|---|---|---|
| LoreRim | off | 64.05 | 80.2 | +5.12 | 74 / 104 |
| LoreRim | hide | 64.06 | 84.4 | +5.22 | 77 / 104 |
| LoreRim | aware | 64.05 | 84.4 | +5.22 | 77 / 104 |
| Tuxborn | off | 82.02 | 67.9 | +1.17 | 71 / 104 |
| Tuxborn | hide | 82.12 | 72.1 | +1.96 | 74 / 104 |
| Tuxborn | aware | 82.16 | 72.1 | +1.95 | 74 / 104 |
| Vanilla | off | 91.25 | 81.0 | +3.95 | 76 / 104 |
| Vanilla | hide | 91.40 | 85.2 | +3.94 | 79 / 104 |
| Vanilla | aware | 91.39 | 85.2 | +3.88 | 78 / 104 |

### 4.5 Scenarios with the class rulings in force (oracle K=6, 60 s, 20 fights): gap = oracle U - IAUS U

| scenario | class | LoreRim off | LoreRim aware | Tuxborn off | Tuxborn aware | Vanilla off | Vanilla aware |
|---|---|---|---|---|---|---|---|
| tank3 | warrior | 90.3 (+1.6) | 90.3 (+1.6) | 89.8 (+1.0) | 89.8 (+1.0) | 91.1 (+1.1) | 91.1 (+1.1) |
| mage-dragon | evoker | 4.1 (+18.6) | 4.1 (+18.6) | 0.9 (+2.2) | 0.9 (+2.2) | 2.3 (+5.8) | 2.3 (+5.8) |
| bandit-pack | archer | 91.0 (+0.9) | 91.0 (+0.9) | 84.1 (+5.8) | 84.1 (+5.8) | 90.0 (+1.3) | 90.0 (+1.3) |
| focus-healer | archer | 91.2 (-0.0) | 91.2 (-0.0) | 3.4 (+1.0) | 3.4 (+1.0) | 88.9 (-1.1) | 88.9 (-1.1) |
| interrupt | warrior | 93.0 (+1.7) | 93.0 (+1.7) | 95.8 (-0.1) | 95.8 (-0.1) | 91.4 (+0.5) | 91.4 (+0.5) |
| weakness | evoker | 84.1 (+8.9) | 84.1 (+8.9) | 34.4 (+31.4) | 34.4 (+31.4) | 77.3 (+11.4) | 77.3 (+11.4) |
| predictive-heal | healer | 82.0 (+9.9) | 82.0 (+9.9) | 15.5 (+5.1) | 15.5 (+5.1) | 75.1 (+6.2) | 75.1 (+6.2) |
| peel | mercenary | 82.9 (+12.2) | 82.9 (+12.2) | 81.9 (+8.6) | 81.9 (+8.6) | 88.8 (-2.8) | 88.8 (-2.8) |
| overkill | evoker | 92.0 (+2.4) | 92.0 (+2.4) | 90.2 (+2.8) | 90.2 (+2.8) | 94.0 (+1.9) | 94.0 (+1.9) |
| double-heal | healer | 66.3 (+8.1) | 66.3 (+8.1) | 27.6 (+22.1) | 27.6 (+22.1) | 63.9 (-5.0) | 63.9 (-5.0) |
| mp-budget | evoker | 89.7 (+2.0) | 89.7 (+2.0) | 59.2 (+21.7) | 59.2 (+21.7) | 84.0 (+4.2) | 84.0 (+4.2) |
| splash | evoker | 90.2 (+4.7) | 90.2 (+4.7) | 78.9 (+7.6) | 78.9 (+7.6) | 79.2 (+12.3) | 79.2 (+12.3) |
| control-setup | warrior | 66.8 (+28.0) | 66.8 (+28.0) | 73.8 (+2.0) | 73.8 (+2.0) | 79.0 (+4.0) | 79.0 (+4.0) |
| kite | thief | 1.6 (+0.4) | 2.4 (-0.0) | 6.0 (+7.8) | 5.6 (+7.4) | 10.4 (+18.5) | 16.8 (+20.0) |
| knight-boss | knight | 11.6 (+14.0) | 11.6 (+14.0) | 32.5 (+33.2) | 32.5 (+33.2) | 63.7 (+15.8) | 63.7 (+15.8) |
| knight-boss-healer | knight | 20.9 (+27.6) | 20.9 (+27.6) | 8.7 (+4.4) | 8.7 (+4.4) | 55.7 (+17.2) | 55.7 (+17.2) |
| hurt-knight | knight | 4.4 (-0.2) | 4.4 (-0.2) | 14.1 (+11.2) | 14.1 (+11.2) | 26.2 (+3.3) | 26.2 (+3.3) |
| ambush | thief | 89.4 (+2.0) | 91.3 (+0.9) | 79.9 (+0.7) | 90.1 (-3.2) | 89.6 (+0.7) | 89.6 (+0.4) |
| MEAN GAP (17, without ambush) | | +8.28 | +8.25 | +9.87 | +9.84 | +5.57 | +5.66 |

Headline J of those runs (IAUS, 24 fights, the 17 scenarios + every class's natural bracket; stealth scenarios are never in J): LoreRim off: J 63.31; LoreRim aware: J 63.56; Tuxborn off: J 63.78; Tuxborn aware: J 63.98; Vanilla off: J 79.10; Vanilla aware: J 79.27

## 5. Stealth: road audit and the H-DETECT design (as far as the Thief needs it)

Proof grades as in `class-m0-re.md`: PROVEN = read on that binary or in our code. UNPROVEN = not read. UNOBSERVED = the
path exists but nobody has seen it run (principle 5).

### 5.1 What exists today

| Piece | Where | State |
|---|---|---|
| Sneak state of an actor | `Actor::IsSneaking()` (fork `ActorState.h:184`, ActorState1 bit 9). MFO reads it for the PLAYER only (`Evaluator.cpp:845`, `EngageOnSight.cpp:734`) | PROVEN read. Never read for a follower |
| Entering sneak | Harbinger ch.3 `kIntent_Stance` (`APMF_API.h:165`): one SneakStart, no re-assert | Field-proven out of combat. UNOBSERVED in combat |
| "Does this foe know where he is" | The foe's combat group keeps a `CombatTarget` per target with `detectLevel`, `stealthPoints` and `kTargetLost` (fork `CombatGroup.h:27-28`). **MFO already reads it**: `TauntProbe.cpp:193-204` takes the foe group's read lock and reads `kTargetLost`, `detectLevel`, `attackerCount` for one actor | PROVEN read, passive, in shipped code. This is the precedent for the input |
| Omnidirectional sense test | Harbinger `SenseActor` (`APMF_API.h:2169-2286`, ABI 18): sight, hearing, proximity, engaged. Main thread only | Field-proven (taunt, engage on sight) |
| The engine's detection level per pair | `Actor::RequestDetectionLevel` (AE 37764). A cache reader that re-requests a stale result, so it is not a passive read. Resolved in Harbinger, unused | PROVEN (`class-m0-re.md` section 3) |
| Writing a foe's detection | Nothing. ch.16 sets the follower's own noise to a fixed 0, which would erase the Thief's "30% quieter" passive, so class sneak must not claim it | Gap (card G2) |
| Sneak attack damage | See 5.2 | PROVEN path, UNOBSERVED for an NPC |
| Detection perks | Entry points 47 / 48 / 57 (light, movement, sneak skill) and 18 (sneak attack) apply to any actor that owns the perk (`ENGINE_NOTES.md` "Class 2") | PROVEN call sites |

### 5.2 New finding: the engine's sneak attack is not player-only

Read on 1.6.1170, function AE id 44016 (base 0x7DCAF0, the block at +0x180 to +0x3E0, the hit data fill). With the
attacker in `rdi` and the target in `rbx`:

1. The target is not dead (vfunc 0x99).
2. The attacker's ActorState1 has bit 0x200 (sneaking) and not 0x400 (swimming) (AE 0x6CEA40 on `attacker+0xC0`).
3. One more attacker-side exclusion that I did not identify: extra data type 0xA9, skipped when `[attacker+0x204]` bit 1
   is set.
4. The target's `RequestDetectionLevel(attacker)` (AE 37764, 0x68F9F0) is not above 0. A second road goes through the
   target's combat group (AE 44706, 0x803DF0). Re-verify the branch order at build.
5. Then the damage is multiplied by the weapon's sneak multiplier (AE 18100, 0x286C50: a game-setting table indexed by
   the weapon's animation type, `fCombatSneak1HDaggerMult` and its nine siblings), then by perk entry point 0x12
   `ModSneakAttackMult(attacker, weapon, target)`, and hit flag 0x800 (kSneakAttack) is set when the result is above 1.

There is no compare with the player anywhere in the block. So a follower that is sneaking and that its target does not
detect gets the list's own sneak attack damage, perks included, through engine logic. Two consequences:

- The Thief's sneak attacks need no damage code of ours. They need the stance (G1) and an undetected state (G2).
- The same cache that step 4 reads is the one H-DETECT's gate feeds. Gating the detection result at its source gives
  the sneak attack and hides him from targeting in one move.

Not done: 1.5.97 and 1.7.104 were not read. Nobody has seen an NPC land a sneak attack. Both are probe and build items.

### 5.3 The design

**Part A, the detection INPUT (a read, no new seat, no new address).** For a follower whose class sneaks, the gather
(`iaus/Candidates.cpp`, the worker) reads, for each foe it already lists, that foe's group record of the follower: the
`TauntProbe.cpp:193-204` read, one read lock per foe group, nothing nested. It fills:

- `Combatant::detectsMe` (Tri): Yes when the record exists, is not `kTargetLost` and `detectLevel > 0`. No when the
  record is absent, lost, or at 0 or below. Unknown when the foe has no group.
- `Snapshot::selfSneaking` from `IsSneaking()`.
- `Facts::unseen` per candidate: Yes when he is sneaking and no listed foe detects him.

Caveat that the probe must settle: the group record is group knowledge, while the sneak attack test reads the per-pair
cache. They can disagree. The input is only trusted once the probe shows a follower's `kSneakAttack` hits line up with
`detectsMe == No` on the struck foe.

**Part B, hide-when-unseen (card G2, tier A, Harbinger).** As `class-m0-re.md` section 3 found: a post-call seat on the
ONE call site of CalculateDetection (AE 37774, caller AE 42742 at 0x7B0D8B). For a pair (foe, follower) that MFO has
declared unsensed, the out level becomes the engine's own "not detected" value before the store and the dispatch. MFO's
truth test is `SenseActor` plus a sight ray on the main thread, published lock-free. The seat runs on the detection job
worker. Deny holes to list in the brief: ForceDetect (9 callers: a hit, an alarm, StartCombat are legitimate reveals
and the gate must yield on that pair until the truth test says unseen again), the group's low-process pass (AE 44804),
Harbinger's own ch.20 / ch.21 held-engagement seats, the refresh cadence, and ch.16 stays unclaimed.

**Part C, probes first, all passive, behind `[Debug]` keys that default OFF.**

| Probe | Line | Answers |
|---|---|---|
| P1 stance | `[stealth-probe] stance`: once a second per follower with a ch.3 claim, `IsSneaking()` and his combat state | Does the stance hold in combat, or does the combat behaviour stand him up (card G1's open question) |
| P2 detection | `[stealth-probe] detect`: once a second per (foe, sneaking follower), the group record's `detectLevel`, `stealthPoints`, lost flag, and MFO's own `SenseActor` verdict | How fast the engine loses a follower that is out of sight, and how far the truth test and the engine disagree. This sizes G2 |
| P3 sneak attack | The hit sink (`Diagnostics.cpp:129`, and the fill probe on `feat/iaus-engine-fills`) prints TESHitEvent's `kSneakAttack` flag for a follower aggressor, with the damage | Whether 5.2 runs for an NPC at all |

Order: P1 and P3 need only G1's stance claim on one follower. P2 needs nothing new. Run them in one Deck session, then
brief Part A (tier A: an engine read on the worker) and Part B (tier A, Harbinger).

### 5.4 The Sneak family's roads

| Road | What IAUS decides | Needs |
|---|---|---|
| The stance (G1) | Nothing. marth: sneak "regardless of player action", so for a sneak class it is an always-on row that MFO re-asserts. The catalogue's Sneak family as written (an opener, gated on Unseen) stays for a user row on another class | ch.3, P1 |
| The blow from stealth | Which foe and which weapon. A blow on a foe that does not detect him is worth his sneak multiplier. Input: `Facts::strikeMult`, the expected multiplier of a weapon blow on this target now, read by a new boost on Attack and PowerAttack and by the posture layer (dagger against bow) | Part A. The multiplier is read at runtime: the weapon's game setting by name, times `HandleEntryPoint(kModSneakAttackMult)` on the main thread (the `Summon.cpp:106` road), mirrored for the worker. Never a constant, never list data |
| Going to hide (G3) | When to break off. A candidate of the Sneak family, offered when a foe detects him and a point no foe sees exists. Inputs: `Facts::canHide`, the worth of the next blow from stealth against the seconds the walk costs, the player's danger, a hard cooldown | H3 (the run), a concealment-point query (Harbinger: `FindEmptySpace` filtered by `GetLineOfSight`), Part B |
| The stealth entry (C13) | Nothing. One fight in five it is given | Part B |

The catalogue changes are appended fields and one new consideration set. They are a tier A slice of their own (engine
read + new mechanism), after the probes, on a branch that does not collide with `feat/iaus-engine-fills` and the
combat-move branches.

## 6. The stealth model in the lab

> **SUPERSEDED 2026-11-11 by `iaus-sim-v2.md` section 30.** The lab now scores detection with the engine's own formula
> (`calculate-detection-re.md`, `tools/iaus_sim/Detect.h`), IAUS reads the real facts (`detectsMe`, `selfSneaking`,
> `hideRoad`, `concealRoad`) instead of the stand-ins described below, G3 is `native/Concealment.cpp` mirrored, and the
> levels are `--stealth off | g1 | hide | g3` with g3 the default. The "close and in view = seen" rule, the 0.6 s
> memory and `StealthInject` are gone. The text below and the numbers of section 7 and tables 4.3 to 4.5 describe the
> OLD model and are kept as the record of what was measured then.

`--stealth off | g1 | hide | aware` (World.h `g_stealth`). Off is the game today and reproduces main's headline J draw
for draw (68.91 at 30 fights, all six list and seed values equal).

| Mode | Roads modelled as existing |
|---|---|
| g1 | The sneak stance, held all fight. A foe that sensed him keeps knowing where he is for `fCombatDetectionLostTimeLimit` |
| hide | g1 + G2: a sneaking follower no foe senses is hidden at once. The free stealth entry works |
| aware | hide + the detection input and G3: IAUS is told which foes do not detect him and may go and hide |

Model rows (each is MODEL unless a source is named):

| Row | Model | Source and grade |
|---|---|---|
| Sneak attack damage | weapon multiplier (`fCombatSneak*Mult` by weapon type) x the list's `ModSneakAttackMult` perks | PROVEN path (5.2). Which perks he holds is MODEL: every playable chain his skill reaches, as the lab's other perk tables assume. An entry with a condition the lab does not model is left out |
| Detection | skill against skill (`fSneakBaseValue + fSneakSkillMult x skill`, the observer clamped by `fSneakPerceptionSkill*`), sight by light inside `fDetectionViewCone` with line of sight, the sound of movement (`fSneakSoundsMult`, equipped weight, `fSneakSoundLosMult` without sight), a falloff over `fSneakMaxDistance` (x `fSneakExteriorDistanceMult` outside) with `fSneakDistanceAttenuationExponent` | **UNPROVEN shape.** The terms are the ones the settings name. The body of CalculateDetection (478 instructions) was not read into a formula. Every stealth number below is therefore the worth of the ROADS relative to each other, not a prediction of the game |
| Close and in view | Inside 12% of the sneak range and inside the cone he is seen whatever his skill | MODEL, conservative. With the settings alone a perked sneak would walk up to a foe's face in daylight |
| Light | Interior 0.15 to 0.6, outside day 0.9 or night 0.2 | MODEL |
| Sneak gait | x0.55 | MODEL |
| A blow reveals him to the victim | The engine's ForceDetect on a hit puts the victim in combat with the attacker: it keeps knowing him for `fCombatDetectionLostTimeLimit` (read from the list's game settings, vanilla 10 s if absent) after the hit, in every mode, and retargets him | PROVEN that a hit is one of its 9 callers. The 10 s window is MODEL (the engine's lost-target timer) |
| Free stealth entry | 1 fight in 5, undetected until his first blow | W Thief |
| Thief passive | Movement noise x0.70 | W Thief |
| Hide (G3) | A run to a point behind an occluder that no foe has sight of, then the stance again. 12 s pacing | MODEL of H3 + the concealment query |

The lists differ where it matters: `fSneakSkillMult` 0.5 / 0.5 / 0.425 and `fSneakBaseValue` -15 / -15 / -7.5
(Vanilla / Tuxborn / LoreRim), `fSneakLightMult` 0.33 / 0.33 / 0.5, `fDetectionViewCone` 190 / 190 / 160. Sneak attack
perks: Vanilla dagger 3 x 2 x 2.5 = 15, bow 2 x 1.5. Tuxborn dagger 3 x 1.5 x up to 2, bow 2 x up to 2. LoreRim dagger
3 x up to 4, bow 2 x 2. `tools/list_balance/compile_world.py` now writes these as `follower.perks.stealth`. The three
`sim.json` files were recompiled and are identical apart from that key.

**The detection input in the lab is a stand-in.** `native/iaus` is not edited in this phase, so in `aware` mode the
input rides considerations that exist: a blow on a foe that does not detect him is boosted like a blow on a helpless
foe (`foeHelpless`, BoostControlled), and "go and hide" is a Sneak candidate whose worth rides the FightStart curve
(`Policy.h` StealthInject). The numbers are the worth of having the input, not of the final wiring.

## 7. What the stealth roads are worth to the Thief (lab, section 4 table C)

Read table C with the caveat of section 6: the detection model's shape is unproven, so these are the roads' worth
relative to each other.

- **Off (the game today):** the Thief is a bow follower. Persona 33, 6 of 10 rulings. Nothing of his card shows.
- **Hide (G1 + G2):** the stance holds all fight, he is unseen 37 to 50% of the time, 55 to 78% of his fights open
  with a sneak attack and 43 to 61% of his damage comes out of stealth. Persona 83, 9 of 10 rulings. The one that still
  fails is the ch.18 power cap, which is card C8's. His natural-bracket utility moves little (LoreRim +0.2, Tuxborn
  +1.3, Vanilla +1.8): he plays his class, and the fights were mostly won or lost by the rest of the party.
- **Aware (the detection input and the hide action on top):** the same as hide in the natural bracket. Once G2 hides
  him by itself he is already where the input would send him. A foe he has struck now keeps him as its enemy for
  `fCombatDetectionLostTimeLimit` and turns on him (the engine's ForceDetect, section 6), so a thief that has hit a
  foe cannot vanish behind cover at once. That removed most of the earlier kite gains: kite went from 1.6 to 2.4 U on
  LoreRim, from 10.4 to 16.8 on Vanilla, and 6.0 to 5.6 on Tuxborn (the first version of this document, where a
  struck foe forgot him after 0.6 s, had 35.8 and 46.3). The ambush goes from 89.4 to 91.3 on LoreRim and from 79.9
  to 90.1 on Tuxborn (Vanilla 89.6 both).

So for Beta 1 the order by worth is G1, then G2. The detection input and G3 come after, and their worth should be
measured again once the detection formula is the engine's own.

## 8. Questions for marth (answered 11-11; also in `open-questions-for-marth.md`)

- **Q87. [ANSWERED 11-11]** A sneaking follower gets the list's own sneak attack multipliers from the engine (5.2): up to
  x15 with a dagger on Vanilla perks, x12 on LoreRim. Leave them to the list? marth: "If we confirm the lists number
  affects followers yes, the related perks the follower can get through progression must also work". So the multipliers
  stay the list's only once a field probe confirms they apply to a follower, and the sneak perks a follower gets through
  progression must also work for him.
- **Q88. [ANSWERED 11-11]** Before any stealth number is quoted as a prediction, read CalculateDetection (AE 37774, 478
  instructions) into a formula and replace the lab's model. marth: "Yes read it, worst case we may need to add forced non
  detection blocks into harbinger on npcs who would otherwise detect if the game doesnt properly calculate npc to npc
  detection". So CalculateDetection is read as a read-only RE card beside G2. The fallback, if the engine does not
  calculate NPC to NPC detection properly, is a Harbinger forced non-detection deny on the foes that would otherwise
  detect him.
- **Q89 (T3, parked). [ANSWERED 11-11]** Card G4 gives T3+ sneak classes half the sneak bonus on a flanked foe. With a
  list multiplier of x12 to x18 that is about x9 on every flanking hit. marth: cap the G4 flanking bonus. The cap value
  is the T3 round's to choose.
- **Q90 (T4, parked). [ANSWERED 11-11]** The Listener's table root is Ranged (a bow kit) while his card and default rows
  are a blade executioner. marth: "Personality says they enjoy the blood of melee, but they will bow when needed". So
  the Listener's primary is the blade and the bow is the fallback.

## 9. PARKED: tier 3, tier 4 and the Dragonborn

marth 2026-11-11: T2 ships first as Classes Beta 1, then T3. Work on the higher tiers stopped where it stood. Nothing
here is in force, measured or reviewed.

| What | Where | State |
|---|---|---|
| Playstyle rows of the 14 T3 classes, the 8 T4 classes and the Dragonborn | `tools/iaus_sim/Playstyles.h`, below the PARKED banner | First draft from the cards. Never run, numbers unchecked |
| Their gap ledger rows (C-G4, C-G7, C-G11, C-G17, C-H6 to C-H11, C-C9, C-FLEE, C-UNDEAD, C-AURA, C-ROOT, C-MODE, C-MARK, C-G8, C-DB) | `tools/iaus_sim/Results.h` ClassGaps | Text only. A row is written to a results file only for a class that ran |
| Go-in rule check (card C9) and the `goInBad%` metric | `World.h` GoInOk, the `kPsGoIn` constraint | Written, used by no Beta 1 class |
| The flanking half bonus (G4) and Shadowcloak (H10) | `World.h` Physical, UpdateStealth | Written, reached only by Master Thief, Nightingale, Listener |
| Scenarios hit-and-hide, shadowcloak, flank, assassinate | `Scenarios.h`, `--scenario stealth` | Ran once on two lists as a smoke test, results not kept |
| One early all-35 run on LoreRim | Not kept | It showed, as leads only: Necromancer in melee 23% of his time, Templar fleeing his line (the Retreat family reads the same disengage flag as the H3 reposition), the Listener shooting 77% |

None of it takes effect by default: a parked playstyle's rulings, persona, forbid bits and stealth passives, and the four parked stealth scenarios, need `--parked` (section 10), so a plain `--classes real` run is Beta 1 plus the unchanged T3+ table behaviour of main.

The T3 round starts from the cards again and may use these as a draft.

## 10. How to run

```
tools/iaus_sim/run.sh --list ~/.cache/mfo-list-balance/LoreRim.sim.json --classes beta1 --class-kit \
    --stealth off|g1|hide|g3 --class-oracle 10 --oracle-k 6 --oracle-h 60 --fights 10 --scroll-road --label ".."
tools/iaus_sim/run.sh --list ... --scenario all,ambush --oracle --class-rulings --oracle-k 6 --oracle-h 60 --fights 20 --stealth aware
```

- `--classes beta1`: the 12 T1 and T2 classes. `--class-kit`: the class knows its own rows' spells.
- `--class-oracle N`: N natural fights per class played by IAUS and by the oracle held to the class's rulings. It
  writes the per-class oracle gap (`scenarios` rows named `class:<slug>`, which the dashboard's Classes view shows as
  the class gap) and the oracle's own persona.
- `--class-rulings`: hold the oracle of `--oracle` / `--headline-oracle` / the scenarios to the class rulings (implied by `--class-oracle`). Without it every oracle is main's unconstrained one of `iaus-sim-v2.md`, and the headline oracle J equals main's. `--no-class-rulings` forces that even under `--class-oracle`.
- `--parked`: the T3 / T4 / Dragonborn playstyles take effect (rulings, persona, forbid bits, the Master Thief / Nightingale / Listener stealth passives) and the four parked stealth scenarios (hit-and-hide, shadowcloak, flank, assassinate) become runnable. Without it only the 12 Beta 1 classes have a playstyle, whatever `--classes` says.
- `--scenario all` is the headline's scenarios: the 17 of main, plus the stealth and disengage lab's while a road is
  modelled (`--stealth g1` and above, the default is g3: `iaus-sim-v2.md` 30.5). `--scenario stealth` names the lab's
  in any mode. Under `--stealth off` a stealth scenario is not part of the headline J.
- The dashboard needs no change: class rulings, persona, class gaps and the per-class oracle gap ride fields it
  already reads.

Checks made: with `--stealth off` and no `--class-kit` the headline J equals main's on all three lists and both seeds
at 30 fights (mean 68.91). Every run in this document ended with 0 bad facts and 0 invalid picks.

Results file (schema 1, fields only added): a new top-level `playstyles` array (class, does, never, forbid, gaps) for the classes that ran, class rulings in `rulings` with ids `P-<class>-<n>`, class gap rows in `gaps` with ids `C-*`, the metric `persona` on every class row, and `class:<slug>` rows in `scenarios` from `--class-oracle`.
