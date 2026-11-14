# Tuner auto-decisions: no round waits for a human

marth 11-13: "have a sonnet agent look at how to resolve the tuning decisions permanently. Only restriction a class must play like that
class, and potion spam is not allowed. Potions other than hsm are emergency options" (hsm = health / stamina / magicka restores).

`tools/iaus_sim/tune_driver.py --decisions auto` (the default) applies this document. `--decisions ask` is the old behaviour (open decisions,
no card / potion gate). Every automatic decision is written to `summary.json` `auto_decisions` (kind, verdict, rule, evidence, numbers) and
listed in `summary.md`. `--replay <summary.json>` replays an earlier round's open decisions through these rules offline.

The statistical gates are untouched (train d > 2.5 se and >= 0.15 J, holdout, many-seed gates, cross-world, Knight floor, joint). The two
gates below are ADDED to them: a value is accepted only if the old rule AND both new gates hold.

## 1. Gate one: class identity (the class card)

The cards already exist as numbers: `tools/iaus_sim/Playstyles.h` (the doc is `iaus-classes.md` section 2). Each band is one
measurable ruling (`P-<class>-<n>`) and one term of the persona score. The driver PARSES that file at run time (`load_cards()`), so the
gate and the simulator's own rulings can never disagree and no band is copied or fitted to current values.

Measurement: the class alone, natural bracket, built-in world, `CFG card_seeds` single-seed runs of `card_fights` fights (full 3 x 48,
smoke 1 x 8), the `--baseline` metrics, mean and se over the seeds (a metric of -1 is "no sample" and skipped; an `a.<family>` share that
never fired is 0, as in Results.h RulingValue). The -off base cells (melee-off, ranged-off, mage-off) have no card row of their own and
play the card of their role (melee, ranged, mage): a PLACEHOLDER mapping, marth may give them their own.

Rule: with `start` = the class's start table (the T1 baseline of the class, before any of his own values) and `new` = start + all his
accepted values + the candidate, for EVERY band: `outside(new) <= outside(start) + tol`, where `outside` is the distance past the band's
bound (0 inside) and `tol = max(CARD_SLACK * scale, 2 * se(difference))`, `scale = max(5, 0.5 * |bound|)` (the unit of the simulator's
own BandCredit, so `CARD_SLACK = 0.05` is 5% of the credit scale: PLACEHOLDER, marth has given no number).

Why "no further out than the start" and not "inside the band": several cards already fail in the shipped table (Knight `b.block`
4.7 against >= 15, the sim measured it 11-13), because the bands are set from the cards, never fitted. "Inside the band" would
accept nothing for those classes. This rule keeps every class that is inside a band inside it (up to noise), and never lets a value move a
failing class further from his card. A band the start table meets with room still binds at the bound itself: once `outside(start)` is 0,
`new` may go outside by `tol` only, so a value that walks the class out of a band is rejected.

Bands (value, source line in `tools/iaus_sim/Playstyles.h`; the line is where the band appears):

| Class | Bands (metric, bound) | Source lines |
|---|---|---|
| melee | melee_share_pct (b.melee) >= 35; b.cast <= 2 | 53, 55 |
| ranged | b.shoot >= 45; contact% <= 25 | 56, 58 |
| mage | melee_share_pct <= 5; b.cast >= 40; contact% <= 20 | 59, 61 |
| knight | contact% >= 60; pDist <= 450; b.block >= 15; powShare% <= 25; taunt_per_min >= 0.5; retreat% <= 1 | 64-67 |
| warrior | contact% >= 55; powShare% >= 40; pow/min <= 6.5; retreat% <= 0.5 | 68-70 |
| mercenary | contact% >= 40; powShare% <= 15; prio% >= 60; retreat% <= 1 | 71-73 |
| archer | b.shoot >= 50; contact% <= 20; melee_share_pct <= 12; prio% >= 55; miss% <= 50 | 74-76 |
| thief | sneak% >= 85; opener% >= 20; sneakDmg% >= 15; b.shoot >= 30; contact% <= 40; powShare% <= 20 | 77-80 |
| evoker | b.cast >= 50; melee_share_pct <= 3; contact% <= 15; immune% <= 0; resisted% <= 15 | 81-83 |
| conjurer | summonUp% >= 60; melee_share_pct <= 3; contact% <= 15; b.cast >= 30 | 84-86 |
| healer | pDist <= 600; melee_share_pct <= 3; contact% <= 15; healAt% <= 60; dwh/f <= 0.1; overheal_pct <= 35 | 87-89 |
| warlock | a.cast_control >= 8; ctrlFail% <= 10; melee_share_pct <= 3; contact% <= 15 | 90-92 |

Where the cards are not specific enough: (a) the melee card has no contact or position band (the doc's scale note says 60 to 72% contact, but
no ruling), so a Melee value that drifts out of the fight is not caught by the card, only by the J gates; (b) melee-off / ranged-off /
mage-off borrow their role's card (placeholder); (c) Thief's stealth bands (`sneak%`, `opener%`, `sneakDmg%`) are only measured when the
simulator's stealth model is on (the g3 road of the built-in world), otherwise they are "no sample" and skipped. Each is a card gap to fix
in Playstyles.h, not in the driver.

Where it applies: every accepted value of the classes stage (`class card`), the rejudge, the edge stage (widened value), the gap (a) search
(in range and far), and once more on the whole accepted set of a class (`final_card_check`: while the set fails, its LAST value is dropped
and recorded). The joint stage runs the gate for every cell on the final table against the shipped table.

## 2. Gate two: potions

Facts used (all existing sim metrics, `--baseline` names): `a.drink_health`, `a.drink_stamina`, `a.drink_magicka` (action shares, %),
`a.drink_buff` (every non-restore potion: fortify, resist, invisibility, buff; poison use is excluded, it is not a drink), `drinkH/f`
(health drinks per fight).

* Restores (HMS) are normal tools: they may rise, but never into spam. Spam is measured two ways, both with PLACEHOLDER caps (marth gave no
  number): the share of action time spent
  drinking, all potions, `DRINK_SHARE_CAP = 8%`, plus marth's 11-13 RULING "Per fight is too strict already, spam is drinking like 8 per minute, I'd say emergencies would override that limit anyway": `DRINK_MIN_CAP = 8` health drinks per MINUTE (`drinkH/f` / (`dur.s`/60)), emergency drinks not counted. The sim has no per-drink health split, so the emergency split uses the mean health % of a health drink (`drinkAt%`): at or under `EMERGENCY_HP_PCT` the drinks are emergencies and count 0, above it all count. `buffDrinkAt%` (filed sim counter) or a per-drink health histogram would make the split exact. The per-fight cap is gone. EXACTLY what is checked: HEALTH drinks per minute only (`drinkH/f`); magicka and stamina drinks are covered only by the 8% action-share cap (no per-minute count of them exists); the emergency split is by the MEAN `drinkAt%` of a class's health drinks, so a class whose mean sits at or under `EMERGENCY_HP_PCT` has the per-minute cap switched off entirely (its drinks all count 0). Sim counters `drinkM/f`, `drinkS/f` and `buffDrinkAt%` (filed) would make the per-minute cap exact for every potion. A metric missing on one side: start missing + new present is graded against the bare bound/cap; start present + new missing FAILS (also the drink rate when `dur.s` is gone). A value is rejected when the metric ends over the cap AND rose beyond noise (`tol = max(5%
  of the cap, 2 se)`). A start table already over a cap is not blamed, but may not be made worse.
* Every other potion is emergency only: `a.drink_buff` may NOT rise beyond noise at all (whatever the cap), and must stay under
  `BUFF_SHARE_CAP = 1%` unless the start was already over it (then: may not rise). So no value can make the follower buff-drink more.
* EMERGENCY, proper: "a non-restore drink happens at or under `EMERGENCY_HP_PCT = 50%` health (the hp floor the healer card also uses: heal
  below half)". The simulator has NO counter for the health at a buff drink (only `drinkAtSum` for health drinks, Policy.h
  `++t.drinksH; t.drinkAtSum`). The driver reads `buffDrinkAt%` the moment the simulator reports it and enforces the 50% line; until then
  the rise + share cap above stand in and the gap is recorded on each gate result as `needs sim metric buffDrinkAt%`.
  **SIM CHANGE NEEDED (not done, out of scope):** in `Policy.h` Family::DrinkBuff add `++t.drinksB; t.buffAtSum += x.c.hp / x.c.hpMax`, and in
  `sim.cpp` Metrics `m["buffDrink/f"]`, `m["buffDrinkAt%"]`. Optionally also the danger / cannot-win signal at the drink.

## 3. Per open-decision kind

| Kind (what summary.json holds) | Rule that replaces asking marth | Verdict kinds |
|---|---|---|
| `class card read for <cls>` (classes stage, 15) | Each value is gated when accepted (card + potion, against the start table); at the end the whole set is gated again, last value dropped while it fails. | accepted / rejected |
| `class card read for <cls> (gap search)` | Same, with the table as it now is and his start table. | accepted / rejected |
| `<cls>: a consideration weight accepted at the edge` (9) | Report-only note, never a blocker. The edge stage already widens exactly these weights under the same rule, so the question "may the best weight lie past the edge" is answered by a test, not a person. | note |
| `<cls> <knob>: a value past the edge passes the rule` (9, 8 are `family.drink_buff`) | The driver widens, the value must pass the normal judge (train, holdout, many-seed, cross-world, Knight floor) AND both gates, and replaces the accepted value. A value accepted at the new outer edge is widened again, `EDGE_ROUNDS = 3` widenings in all (the knob's valid range always bounds it). `family.drink_buff` going lower can only lower non-restore drinks, so the potion gate passes it; a value that raises them is rejected. The edge stage now runs BEFORE the joint stage so the joint gate covers the widened table. | accepted / rejected |
| `gap search <cls> <knob> a -> b (past the range tried)` (12) | The same: judge + card + potion gates, no distinction between in range and far once they hold. | accepted / rejected |
| `<cls> took N accepted values (more than 10)` | Report-only note: every value passed every gate, so the count is a smell, not a gate (it points at the shared catalogue, for a human to read later). | note |
| `oracle gap <cls>: divergences of class b/c/d` (8) | Report-only ledger row (`gap-ledger.md`): classes b, c, d are missing considerations, missing behaviours and modelling suspects, none a tunable number. Never a blocker. | note |
| `JOINT GATE FAILED` | The whole table is withheld: `summary.json ship.table` is empty (`ship.joint` says why). The joint stage now also runs the card + potion gates per cell. | rejected |
| `accepted value ... loses with the re-measured potion worth` | The value is withheld from `ship.table` (`ship.withheld`). | rejected |
| worth at the edge, loot not searched, global, constant, Knight floor under 60%, rating under tier, logistics | Report-only notes. The driver never writes these tables, so nothing needs a decision before shipping; they stay visible in `auto_decisions`. | note |

`summary.json` also gets `ship`: `{table, withheld, joint, note}`. `ship.table` is the accepted values minus anything withheld, and is empty when the
joint gate fails: nothing a gate rejected is in it.

## 4. Offline replay of the live round (62 open decisions at 11-13, `--replay`)

See the report that came with the commit. Only the 12 largest play changes per class are in the summary, so a card read is judged on the
band / potion metrics among them; edge and gap decisions need the widened value's own play metrics and are `needs-metric` offline.

## 5. What needs a human, what is a placeholder

Nothing blocks a round. These are PLACEHOLDERS marth can overwrite with one number each (all at the top of `tune_driver.py`): `CARD_SLACK`
0.05, `DRINK_MIN_CAP` 8/min (marth), `DRINK_SHARE_CAP` 8, `BUFF_SHARE_CAP` 1, `EMERGENCY_HP_PCT` 50, `EDGE_ROUNDS` 3, and the -off cells' borrowed
cards. One simulator change would make the emergency rule exact: `buffDrinkAt%` (section 2).

## 6. Knight blocking (marth 11-13)

"That may mean we are measuring the wrong thing" / "the idea is that they block most attacks that are aimed at them." The old bands
`b.block` >= 15 (Knight) and >= 20 (Great Knight) measured the share of time spent in the block action. They became bands on `guard2%`
(sim.cpp:110; counted World.h:573-574: the melee blows that LAND on him while two or more foes are taunted onto him, `inc2`, and how many met
his raised guard, `guard2`). Knight `atLeast("guard2%", 50)`, Great Knight `atLeast("guard2%", 80)` (marth correction 11-13: 80, not 60).
Caveat: it is a share of blows under a two-or-more taunt, not of every blow; with fewer than two foes taunted onto him it has no sample.
