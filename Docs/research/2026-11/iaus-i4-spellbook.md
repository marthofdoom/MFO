# IAUS card I4: the SpellBook mirror and candidate generation

2026-11-10. Branch `feat/iaus-i4-spellbook`. Tier A (a new main-thread mirror, so threading). Design:
`iaus-design.md` section 1.5 (threading), 1.6 (budget, pruning P1 to P3), 2 (candidate generation), 3 (what each
family reads), section 9 row I4. The IAUS idea for MFO is LandingCrew's.

Nobody calls this yet. Card I6 calls it from the combat scan and wires the two `Clear()` calls.

## Shape: gather, then a pure generator

marth wants an offline combat simulator (its own card) that feeds IAUS randomized fight state tick by tick. So
generation is two layers, and the second one is the same code in the game, in the harness and in the simulator.

| layer | file | what it does | engine types |
|---|---|---|---|
| mirror | `native/iaus/SpellBook.h/.cpp` | main-thread measurement of spells, scrolls, staves and kit, read on the worker | yes |
| gather | `native/iaus/Candidates.h/.cpp` | worker reads plus the mirror fill a `Snapshot` | yes |
| data | `native/iaus/Snapshot.h` | `Snapshot`, `Combatant`, `SpellEntry`, `Kit`, `Book`, `ClassGate` | none |
| rules | `native/iaus/Prune.h` | nature, the never-generated list, P2, P3, guarded ratios | none |
| generate | `native/iaus/Generate.h/.cpp` | `Generate(snapshot, classGate, tuning)` returns the candidates | none |

No family had to stay engine-side. Every "is this a candidate" rule is in `Generate.cpp` or `Prune.h`.

The snapshot actor type is `Combatant`. `Core.h` already has a function called `Unit`, so that name is taken.

## The SpellBook mirror

Follows `SelfStatus` exactly. `SpellBook::Read(actor)` runs on the worker. It hands out the last measured book as an
immutable `shared_ptr<const Book>` under one leaf mutex and posts one `MainThread::Post` refresh when needed. The
measurement (`Measure`) runs on the main thread only. No lock is held across an engine call or a log call.

Refresh triggers, all inside `Read`:

- no book yet. The first `Read` of a fight is the combat start trigger.
- the book is 5 s old (`kRefreshSec`).
- the book is dirty. `logistics/Sinks.cpp`'s one `TESContainerChangedEvent` sink calls
  `SpellBook::OnContainerChanged(old, new)`, which marks the book of either side. I6 should call
  `SpellBook::Invalidate(follower)` after a Fired summon, drink or scroll.
- a refresh lost to a load is asked again after 3 s (`kAskRetrySec`).

MFO has no "spell learned" event and the board has none either (it rebuilds its spell list on its own drain). A
newly learned spell shows up at the next 5 s refresh.

A book is trusted for 15 s (`kTrustSec`, three missed refreshes). Past that `Read` returns nothing and asks, so a
fight never starts on the previous fight's stock. An unloaded follower gets no book at all, never an empty one.
`Clear()` bumps a generation counter, so a refresh that was in flight across a load is dropped.

On VR there is no pump, so there is no book and an IAUS follower has no cast, drink or posture candidates there.
That is the same documented degrade `SelfStatus` has.

What one entry caches (`SpellEntry`): form, source (spell, scroll, staff), nature, the drop reason, role tag, the
resist actor value of the costliest hostile effect, magicka cost for this caster, aimed reach, area in units,
self or touch delivery, two-handed, concentration, undead-only, melee buff, the class bits, live summon, the party
members the effect is already on, count and charge for scrolls and staves.

The kit (`Kit`): health, stamina and magicka potion counts (the same `PotionRestores` test `CountPotions` makes),
arrows, bolts, and whether he carries a melee weapon, a silver melee weapon, a bow, a crossbow, a staff, a charged
staff.

### Nature comes from cast/, not from a second rule

`MakeEntry` asks `CasterConsent::ClassifySpell`, `Actuation::ClassifyArchetype`, `IsSummonSpell`,
`SpellNatureUndeclared` and `CastRoadUnsupported`, and passes their answers to `Prune::ClassifyNature`:

1. Reanimate shape: Unknown (it needs a corpse, there is no family for it today).
2. `IsSummonSpell`: Summon.
3. Ward shape: Ward.
4. Any effect of archetype Calm, Frenzy, Demoralize, Paralysis or Turn Undead: Control.
5. Cloak shape with Self delivery: Buff (worn on himself, marked `meleeBuff`).
6. `ClassifySpell` says Offense: Offense. Heal: Heal.
7. Buff kind whose costliest effect is Script, or `SpellNatureUndeclared`: Unknown.
8. Otherwise Buff.

marth's ruling is that IAUS understands spells live from records with no Synthesis data. Nothing here reads a
per-list table.

**Unknown nature is not a candidate.** The brief asked for "low score, logged once per spell per session". It is
logged once (`[spellbook] <name> (<id>): nature=unknown -- no IAUS candidate: nature unknown`). It is not scored
low, because a candidate needs an explicit target (design 1.4) and a spell with no readable nature has none, and
because a generic "low score" would need a new consideration in the I2 core, which this card may not change. If
marth wants these cast, the options are a new `Cons` in the core, or a per-spell declared nature on the board.

## Candidate generation

Families generated: attack, power attack, taunt, retreat, the three drinks, posture, cast offense, control, heal,
buff, summon, ward.

Not generated: sneak (G1 has not landed), scroll use (S3, the scrolls are in the book with count and nature),
exploit (I3), bash, a "party fan" buff candidate (a `Candidate` has no party target and no record test says which
spells fan, so buffs are per recipient), any spell of Unknown nature.

Never generated, per design section 2: a spell only the unanimated road can cast, runes and other target location
spells (delivery, or a Rune, Wall or TLBurst shape), constant effects, a non-hostile actor as a foe, the player or
a party member as the target of a hostile spell.

Pruning:

- P1: `SpellEntry::drop`, the class gate, and a two-hander while a heal stands on a hand.
- P2: a cost over the current magicka drops every pair of that spell. An unreadable pool drops every priced spell.
  No stock drops a drink.
- P3: own combat group, not lost, alive, enabled, 3D loaded, hostile (the brawl gate), inside the chase cap with
  the Harbinger held-foe waiver, not given up. That is `PickFoe`'s list of tests. Then the top 5 by
  `Prune::FoeRelevance` (distance, current target, on the player, the player's target, on me, on an ally, low HP).
  Ties go to the lower FormID.

G10: `cast/WillWork` has no separate `Want()`. A call is both the read and the prefetch. So I6 calls
`Candidates::NoteDecision` after `Decide`, which keeps the top 6 offense or control (spell, target) pairs, and the
next `Gather` asks `WillWork` for those pairs only. Every other pair stays Unknown, which passes.

Facts: a family writes only what its considerations read. Everything else keeps the `Facts` default. Ratios go
through `Prune::SafeRatio`, copied floats through `Prune::Finite`. An actor whose maximum health reads 0 is skipped.
The harness feeds NaN pools, distances, resists and a level of 0 and checks every candidate still passes
`FactsFinite`.

### Per family

| family | generated when | facts written beyond the common ones |
|---|---|---|
| Attack | per top foe. A melee-only follower skips a foe airborne out of reach | foe facts, reach, meleeRange |
| Power attack | class gate, melee or silver drawn, foe not out of melee reach | same, hard cooldown from the class row |
| Taunt | `bTaunt` on, class gate, melee in the right hand, foe on an ally or the player | foe facts, reach = taunt radius |
| Retreat | class has `kDisengage`, at least one foe | disengageClass |
| Drinks | stock above 0 | stock |
| Posture | a carried category, against the current target (else the nearest foe) | posture, postureCurrent, dist, ammo, staff charge, unholy, out of reach, classAllowsWeapon |
| Cast offense / control | per kept spell and top foe. Turn Undead only on undead | foe facts, reach, cost, will_work, target_cond, resist, area, splash |
| Cast heal | hurt recipient (below 0.9995) in reach and not occluded. Self-delivery heals himself only | recipient facts, cost, alreadyActive from the live stream table |
| Cast buff | recipient the effect is not already on, in reach | recipient facts, meleeBuff, recipientInMelee |
| Summon | no live summon from that spell | cost |
| Ward | always when known | casterOnMe, alreadyActive |

Common facts: self health, stamina and magicka pct, seconds in combat, foe count, party count, emergency level.

The posture `Key` target stays 0 and `postureCurrent` is the drawn category, as I2 asked.

### Class gates

`SpellBook::GateFor` resolves one class: the equipped class when the tree is on and one is equipped, else the T1
class of `Followers::ResolvedClassFast`. It turns that `ClassDef` into plain flags (`ClassGate`).

- Spells (rulings Q34, Q62): a casting class is the mage line or a hybrid. On a casting class every spell is
  on-class. On any other class a spell is a candidate only when he has mastered a casting class, and then it
  carries `onClass = false, fromMastered = true` so `class_fit` scores it low with the emergency lift.
- Power attack: a ch.18 power share or a power-attack default row. The row's `cooldownSec` is the hard cooldown.
- Taunt: a taunt default row. Retreat: `Flag::kDisengage`.
- Weapons: only under strict adherence, by category.
- No class resolved yet: nothing is gated for spells and weapons, and the row-gated actions stay closed.

## Questions for marth

1. **Tree OFF and a Melee or Ranged T1 follower.** The brief and `Core.h` say the class gate uses the T1 class
   when the tree is off. Read literally that means such a follower has no cast candidates at all (his spells are
   off-class and nothing can be mastered with the tree off), and no power attack, taunt or retreat (the T1 sets
   have no such rows). Today's table lets him have a heal gambit and a power attack row. Is that intended for IAUS
   in base MFO, or should a T1 class gate nothing?
2. **Attack reach.** I kept the I2 harness convention: with a melee weapon the attack's reach is `fMeleeReach`, so
   an attack on a foe more than 1.15 times that away is vetoed and the follower's own AI closes the distance. The
   alternative is the chase cap as the reach of the target pick, which is closer to what a "Foe: nearest, attack"
   gambit does today. The simulator can show the difference.
3. **Unknown-nature spells**, above.

## What I6 has to do

- Call `Candidates::Build(follower, set)`, then `Engine::FillCooldowns` and `Engine::Decide` over
  `set.gen.actions`, `DecidePosture` over `set.gen.postures`, then `Candidates::NoteDecision`.
- Keep the `Set` alive and unmoved while the candidates are in use (the names point into its snapshot).
- Call `SpellBook::Clear()` and `Candidates::Clear()` in `ResetAllState` after `StopPump()`.
- Call `Candidates::CombatEnded(follower)` at combat end and `Candidates::Forget` on dismissal or toggle off.
- Call `SpellBook::Invalidate(follower)` after a Fired summon, drink or scroll.
- Log `Candidates::Describe(set)` somewhere rate-limited. It is the only view of what was generated.

## Known gaps

- `Combatant::taunted` is always false. Taunt has no "already taunted" read. `Taunt::Fire` refuses transparently.
- Hand busy means `HealStandsOnHand` only. `CastInFlightOnHand` needs the spell and proxy of a specific claim and
  is left to the executor.
- The staff charge read is not field-observed yet (principle 5): equipped staff from the hand's charge actor
  value, unequipped from `ExtraCharge`, no `ExtraCharge` read as full. "Charged" is charge above 0 and at least one
  cast's cost. The `[spellbook]` line prints `(charged)` or `(EMPTY)` so it can be checked.
- Effect area is converted from feet at 64 / 3 units per foot. Not measured in game.
- Live summon, buff already-active, stock and ammo are up to 5 s old. The executors' own guards still hold.
- The Evaluator's small foe reads (`FoeTargets`, `FoeIsCaster`, `ActorDowned`, the `PickFoe` gates) live in an
  anonymous namespace in `Evaluator.cpp`. `Gather` repeats those reads instead of editing that file.
- Posture weapon gating is by category. A class that allows a bow but not a crossbow is handled, a class that
  allows one-handers but not two-handers is not (both are "melee" here).

## Proof

- `tools/iaus_harness/run.sh` builds `Generate.cpp` with the host compiler and runs section 8, "Candidate
  generation from a hand-built snapshot": 38 checks on top of the I2 ones.
- CI `native` (build and build-dev) on the final SHA.

## In-game checks (for the test list, once I6 calls it)

1. Mage follower with fire, frost and shock spells, a rune, a summon, a ward, a heal. Enter combat. Expect one
   `[spellbook] <name> (<id>): spells offense=.. control=.. heal=.. buff=.. summon=.. ward=.. not-candidates=..`
   line within about a second. The rune must be counted under not-candidates and get its own
   `no IAUS candidate: target location` line once.
2. Give him a potion mid-fight. Expect a new `[spellbook]` line with the potion count up within one lap (the
   container sink marks the book dirty). Take it back, same.
3. Teach him a spell with the console mid-fight. It appears in the next line within 5 s.
4. Save and load mid-fight. No `[spellbook]` line from before the load may appear after it, and a fresh one
   follows.
5. A staff: carry a charged one and an empty one. The line must say `(charged)`. Drain it. It must say `(EMPTY)`.
   This is the unobserved read, check it first.
6. A summon alive: the summon spell must not be a candidate (`Describe` shows `summon 0`). Kill the summon. It
   comes back within 5 s.
7. A buff already on him (Oakflesh): no buff candidate for him while it runs.
8. A melee-only follower against a flying dragon: no attack candidate names the dragon while it is airborne out of
   reach.
9. Tavern brawl: no candidate names the brawl partner.
10. Tree ON, a Warrior with no mastered mage class and a known heal: `dropped class` is above 0 and there is no
    cast candidate. Master a mage class: the heal becomes a candidate and scores low until someone is badly hurt.
11. Budget: with 8 foes and about 30 spells the `[iaus]` line should stay under `fIausBudgetMs` (0.5 ms).
12. VR, if anyone has it: no book, so attacks only. No crash.
