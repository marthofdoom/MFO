# IAUS engine fills: the measured inputs, probe first (2026-11-11)

Branch `feat/iaus-engine-fills`. Card: the engine side of the phase-2 estimates (`iaus-sim-v2.md` 24 to 28).
Formulas and their proof levels: `iaus-i8-damage-research.md` 2.1 to 2.4. Code: `native/iaus/Fills.h` and `Fills.cpp`.

## 1. What this is

Phase 2 gave the scorer considerations that read measured numbers: how hard a foe hits, what armor stops, what a
spell does, who is in the middle of a swing or a cast, who dies for good. On main those numbers are all "not
measured", so the considerations read "off". This card measures them in the game.

Nothing here has been watched running yet. So every input ships in two steps:

1. **Probe.** `[Debug] bIausFillProbe = 1` measures every input and writes `[iaus-fill]` lines next to what really
   happened. It writes nothing into the scorer's snapshot. No score changes.
2. **Switch.** Each input has its own `[Debug]` key, default OFF. A key goes ON only after a field log shows that
   input's lines are right (section 4 says what "right" means for each).

With every key off the module still does one thing at load: `StartupCheck` resolves the GMST pointers and the
entry-point row and logs its status lines (the GMST values, the switch states, and the sink's registration line).
After that it does nothing per tick: no read, no state, no line.

## 2. The inputs

| Input (Snapshot.h) | Switch | How it is measured | Thread | Left out, and said so in the line |
|---|---|---|---|---|
| `Combatant::armor` | `bIausFillArmor` | `kDamageResist x fArmorScalingFactor x 0.01`, capped at `fMaxArmorRating x 0.01` (i8 note 2.3) | worker | the armor base factor sum (Actor vtable 0xE7 writes a cache, main thread only), the attacker's EP 0x25 |
| `Combatant::magicResist` | `bIausFillArmor` | `kResistMagic` | worker | |
| `Combatant::hit`, weapon | `bIausFillHit` | `weapon damage x fDamageWeaponMult x skillMul + kMeleeDamage` (bows add nothing), `skillMul = fDamageSkillMin + (Max - Min) x skill / 100`, the PC pair for the player (2.1, the core formula) | worker | tempering, the arrow or bolt, the attack data's multiplier, every perk entry point |
| `Combatant::hit`, spell or staff in hand | `bIausFillHit` | the record health damage of the spell (or the staff's enchantment), `hitIsMagic = true` | worker | the caster's perks, the element resist |
| `Combatant::hit`, bare hands or a creature | `bIausFillHit` | the race's unarmed damage plus `kUnarmedDamage` | worker | NOT decoded from the engine. An estimate the hit line must prove |
| `Combatant::hitsPerSec` | `bIausFillHit` | weapon: `speed x fCombatDPSMeleeSpeedMult` or `x fCombatDPSBowSpeedMult` (2.4, the engine's own AI model). Spell: ruling Q86, below | worker | spell cost reductions (skill, perks) |
| `Combatant::rangedWeapon` | `bIausFillHit` | a bow or crossbow in the right hand | worker | |
| `Combatant::lossPerSec` | `bIausFillLoss` | health lost between two laps, weighted over 2 s (the lab's window) | worker | |
| `Combatant::swinging` | `bIausFillSwing` | `ActorState` attack state is Draw or Swing, with a melee weapon | worker | a lap is 133 ms x party, so a short swing can fall between two laps |
| `Combatant::casting` | `bIausFillCasting` | one of the actor's `magicCasters[]` is not idle and not ending (the same test `cast/Hands.cpp` makes) | worker | |
| `SpellEntry::damage`, `heal` | `bIausFillSpells` | the effect records: a health ValueModifier, DualValueModifier or Absorb. Instant once, a duration for its seconds, a stream for 1 second. Magnitude through EP 0x1D (2.2) | main | duration perks (EP 0x1E), dual casting |
| `SpellEntry::controlSec` | `bIausFillSpells` | the longest Paralysis, Calm, Demoralize, Frenzy or TurnUndead duration | main | duration perks |
| `SpellEntry::measured` | `bIausFillSpells` | true once the three above were written (MFO-B684) | main | |
| `Snapshot::mortal` | `bIausFillMortal` | not essential and not protected (the reference's own flags) | worker | whether the LIST lets a follower die another way (Q77) |
| `Snapshot::swingReach` | `bIausReachLearn` | ruling Q68, section 3. Read by PowerAttack and Bash only | worker | |

`selfTtl`, `threatTtl`, `tauntTtl`, `recipientTtl` are not filled here. The pure layer (`Estimate.h`) derives them
from `hit`, `hitsPerSec`, `armor` and `lossPerSec`. So GroundNeed, BlockNeed and the retreat terms go live with
`bIausFillHit` plus `bIausFillArmor`, and BashNeed with `bIausFillCasting`.

### A foe's spell rate (ruling Q86)

marth: "no, it's IAUS weighted based on magicka, damage, etc." A foe's spell threat is not a cast rate measured
per actor. It is estimated from what can be read:

- what the spell does: its record health damage (`hit`)
- what it costs: the record cost of its effects (no skill or perk reduction)
- the magicka he has, and what he regains in combat: `max magicka x kMagickaRate% x kMagickaRateMult% x
  fCombatMagickaRegenRateMult`
- its charge time

Casts per second is the smaller of what the charge time allows (`1 / charge`, a stream counts one second of
damage per second) and what his magicka pays for over the next 10 s (`(magicka + regen x 10) / cost / 10`), and
never above 1. A staff spends its own charge, so only the charge-time bound applies. A foe out of magicka is
therefore a small threat, and a foe with a deep pool and a cheap spell is a large one.

The two constants (the 10 s horizon, the ceiling of one cast a second) are model constants, in `Fills.cpp`.
The `cast-start` line still prints the seconds since his last cast start. That is for reading the log only. The
rate is never taken from it.

Not filled by this card: `Combatant::beingHealed` (it needs a "heal in flight on him by anyone" query in `cast/`,
a different subsystem, its own small card) and `Combatant::healer` (only the regroup road reads it, and that road
is off).

### No new engine address

Every read is an actor value, a record field, a GMST, an `ActorState` bit field, or the actor's own
`magicCasters[]` array. All of them have a precedent on the same thread in MFO today. The one id call is
`BGSEntryPoint::HandleEntryPoint` for EP 0x1D, through its existing verified row
`TradePerks.BGSEntryPoint.HandleEntryPoint`, on the main thread, refused when the row does not verify (the record
magnitude then stands and the line says REFUSED). `cast/WillWork.cpp` makes the same call with the same
arguments. No `VerifiedAddresses.h` change.

EP 0x1D is called only with a real target: the caster's combat target for a damage effect, himself for a heal.
With no combat target the entry point is not called. A null target is not a proven argument.

### GMSTs

Resolved once at kDataLoaded (`Fills::StartupCheck`), read live. A missing one makes the estimates that need it
stay 0 ("not measured") and logs an error. Nothing falls back to a vanilla default.

## 3. The learned swing reach (ruling Q68)

`iaus/Reach.h` is the estimator (in the harness since phase 2). This card feeds it.

- A swing OPENS on the lap the follower's attack state enters Draw or Swing with a melee weapon. The distance is
  centre to centre to his engine combat target at that lap, the same measure the swing gate compares.
- It RESOLVES on the first lap the state has left Draw and Swing (the hit frame is behind him), or after 2.5 s.
- HIT: a `TESHitEvent` he caused on that target with that weapon since the swing began. A blocked hit is a hit.
- MISS: the swing ended and he hit nothing.
- A hit on someone else only gives no outcome.
- One learner per (follower, weapon). It starts at `fMeleeReach`. Runtime only, emptied on a load.
- A swing is resolved only after he has been out of Draw and Swing for 0.3 s, or swings again, or after 2.5 s.
  The hit event can arrive after the lap that saw the state change. The next swing's window starts no earlier
  than that resolution, so a late hit is never credited to the swing after it (review F4).
- A dual wielder: a hit on his target by the LEFT-hand weapon gives no outcome (review F1). The attack state
  does not say which hand swung and the learner is the right-hand weapon's. His misses stay ambiguous the same
  way and count for the right-hand weapon.
- With `bIausReachLearn = 1` the learner's `Gate()` goes into `Snapshot::swingReach`, which only the swing
  families read: PowerAttack and Bash (review F5, rulings Q68 and Q70). `Snapshot::meleeReach` is never changed,
  so the attack's target naming, touch spells, the melee-buff test and the strike distance keep `fMeleeReach`.
  With no learner yet `swingReach` is 0 and the swing families read `fMeleeReach` (MFO-B687).

Known limits, on purpose:
- Plain swings and power attacks share one learner. A missed swing does not say which it was. The hit's power
  flag is in the line, so the log shows whether they need two.
- Bare hands are not learned.
- Precision's capsule length is not the start value. It is a weapon length from the hand, not a reach
  (`iaus-i6.md`).
- The distance is to the target's centre, so a very large target (a dragon) is hit from farther than a man.

## 4. The probe lines and what turns each switch on

All under `bIausFillProbe = 1`. Each needs the follower's Sentient AI switch on, because the gather only runs
for him then.

| Line | When | Turns on |
|---|---|---|
| `[iaus-fill] gmst ...` | once at load | read it first on a Requiem list: `fArmorScalingFactor`, `fMaxArmorRating`, `fDamageSkillMin/Max` |
| `[iaus-fill] unit <name> ...` | each actor in a fight, once per 10 s | sanity: a hit above 0 for every armed foe, armor between 0 and the cap |
| `[iaus-fill] hit A -> B ... took X ... estimate Y ratio R` | every hit a follower deals or takes (3 lines a second at most) | `bIausFillHit` + `bIausFillArmor`: the ratio for plain weapon hits with "0 other hit(s)" sits near 1. A steady ratio that is not 1 names what is left out (tempering and arrows push it above 1, a power attack about 2) |
| same line, `health before / at-event / after` | | whether the event comes before or after the damage (the i8 note's open point). `at-event` is read only when the event arrives on the main thread (review F3), else it prints nan. The line's `sink thread` against the status line's `main thread` says which |
| `[iaus-fill] cast-start` | a foe's caster leaves idle | `bIausFillCasting`: one line per real cast, none while he only holds the spell |
| `[iaus-fill] swing-start` | a foe's attack state enters Draw or Swing | `bIausFillSwing`: about one line per real swing. Far fewer lines than `hit` lines from that foe means the laps miss swings, and the block road needs an event feed instead |
| `[spellbook] [iaus-fill] spell ...` | each offense, control or heal spell, once without and once with the perks run | `bIausFillSpells`: `damage` matches the spell's tooltip for the follower, and a spell hit's `took` matches it after resist. An offense spell whose damage is not in its own effects (it spawns a hazard, a cloak, a script, a weapon enchantment) is logged "NOT measured" and stays unmeasured, so it is never scored as "does nothing" (review F2) |
| `[iaus-fill] mortal ...` | once per follower, again when a flag changes | `bIausFillMortal`, after Q77 |
| `[iaus-fill] swing <name> ... HIT / miss ... learned reach a -> b` | each resolved swing of a follower | `bIausReachLearn`: the value settles within about 12 swings and misses are a small share once it has |

Field run plan (one session, no reload needed between steps):
1. INI: `bIausFillProbe = 1`, every `bIausFill*` and `bIausReachLearn` at 0. Sentient AI on for one melee
   follower and one mage.
2. One fight against melee bandits, one against a mage, one against draugr or wolves (creatures), a few minutes
   each.
3. Pull MFO.log, grep `iaus-fill`. The evidence file is the ratio histogram per attacker kind (weapon, spell,
   unarmed), the cast-start and swing-start counts against the hit counts, and the reach lines.

## 5. Pure-layer changes (run in the harness and the sim)

MFO-B684, from the phase-2 review:
- `SpellEntry::measured`. A 0 with `measured = false` is "not known". `Facts::expDamage` is then below 0 and
  DamageValue does not apply. A measured 0 scores 0 against his best measured spell.
- The best-damage denominator (`Generate.cpp` bestOn) now makes the same tests the candidate loop makes: class
  (Q34), a held hand, cost, an undead-only spell against a living foe, a self blast with no area, and the reach
  the range curve vetoes at (1.15 x).
- The sim sets `measured` on every entry it fills. Harness: 6 new checks, 0 failed.

J, 30 fights, seeds 1 and 7 (noise about 0.5):

| | LoreRim | Tuxborn | Vanilla | mean |
|---|---|---|---|---|
| main 6bc3f38 | 63.958 / 63.519 | 64.743 / 63.594 | 78.889 / 78.764 | 68.91 |
| this branch | 63.957 / 63.450 | 64.823 / 63.588 | 78.960 / 78.818 | 68.93 |
| after the review round (swingReach, main merged) | 63.976 / 63.449 | 64.825 / 63.661 | 78.963 / 78.842 | 68.95 |

## 6. Later, each with its own row or brief

- The core weapon formula as an id call (AE 26410 / SE 25847) for tempering: a new `VerifiedAddresses` row on
  three runtimes, main thread. Worth it only if the hit ratios say tempering matters.
- `GetArmorBaseFactorSum` (vtable 0xE7) and the weapon entry points (EP 0x00, 0x1C, 0x23, 0x25), main thread,
  through the book.
- `beingHealed`, `healer`.
- A learner per swing kind, if the log asks for it.

## 7. Questions

- Q84: the learned reach is emptied on every load. Keep it for the session across loads, put it in the co-save,
  or leave it (about 12 swings to relearn)?
- Q85: `mortal` reads the engine flags only. Which mods on the lists decide follower death (Q77), and is there a
  runtime-agnostic read for them, or does `mortal` stay the engine flags?
- Q86: ANSWERED (marth 2026-11-11): "no, it's IAUS weighted based on magicka, damage, etc." No measured
  per-actor rate. Built as section 2 "A foe's spell rate".

## 7a. The review round (Opus tier 3 on 541ccb4)

Log: `scratchpad/agentlogs/iaus-fills-review.md`. Confirmed: every switch off equals main, the core formula,
armor, the rate, the finite guards.

| Finding | Fix |
|---|---|
| F3 threading: the sink read health on an unknown thread | the main thread id is captured in `StartupCheck`. The sink reads the at-event health only on that thread, else NaN |
| F1 a dual wielder's left-hand hit was a miss | a hit on the target by the left-hand weapon is "no outcome" |
| F4 resolution raced the hit event | a 0.3 s grace before a swing resolves. The next window starts no earlier than the resolution |
| F5 `meleeReach` replaced wholesale | new `Snapshot::swingReach`, read by PowerAttack and Bash only (`Generate.cpp` `Ctx::SwingReach`). The sim sets it too. Harness: 2 checks |
| F2 indirect-damage offense spells written as a measured 0 | SpawnHazard, Cloak, Script and EnhanceWeapon carriers stay unmeasured |
| F6 to F8 wording | the all-off wording here and in `Fills.h`. The "left out" lists name the weapon's own resist value and the poison-resist swap |
| ruling Q86 | section 2 "A foe's spell rate" |

## 8. For the reviewer: the spots the author is least sure of

1. `magicCasters[]` of a FOE read on the worker (`Fills.cpp` Unit). The precedent reads a follower's. Gather holds
   the foe's NiPointer and has checked `Is3DLoaded`.
2. The hit sink reads the target's health on the engine's event thread (`HitSink::ProcessEvent`). One AV read,
   no list walk, only while the probe is on.
3. `Classify`: which effects count as health damage or a heal (the `kRecover` exclusion, Absorb as damage).
4. The swing window (`LearnReach`): `windowStart` is the previous lap, so a hit from the swing before can be
   credited to this one when two whole swings fall inside one lap. (The resolve race is fixed, 7a F4.)
5. (Fixed, 7a F5.) The learned reach is `Snapshot::swingReach`, PowerAttack and Bash only.
6. `CastsPerSec` (ruling Q86): the record cost with no reduction, and the two model constants.
