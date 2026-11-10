# MFO IAUS COMBAT MODE: design (Opus 5.5 design pass, 2026-11-09)

ClickUp 1dfcuabcr1d (class release). The wider IAUS update (logistics too) is 1dfcuabcr0g. The idea is
LandingCrew's (memory credits-landingcrew-iaus): credit them in the release notes, README and Nexus page.

Inputs: marth's rulings of 2026-11-09 (the brief), `class-build-plan.md`, `class-walkthrough-decisions.md`
(Q8, Q22/23, Q26 and the turn undead ruling), MFO origin/main `c3f3732`, branch `feat/classes-c3-mcls` `83ae1a8`,
Harbinger origin/main `c0a720e` (ch.18 merged, ABI v34), CommonLib fork `mit-3.7`.
DESIGN ONLY. No code, no branches. Every `file:line` below was read this pass on origin/main unless marked.

---

## 0. Facts this design stands on (verified this pass)

1. **The scan's seam is `Eval::Choice`** (`native/Evaluator.h`): ruleIndex, actionOpcode, actionParam, subject,
   subjectActorForm, target handle, conditionOpcode, conditionParam. `Actuation::Fire(f, choice)` executes it and
   returns `Outcome{result, reason, transparent}` (`cast/Actuation.h:10-44`). Transparent = "fall past me", opaque or
   Fired = "this tick is taken".
2. **The combat scan** (`Scheduler.cpp:1583-1828`) walks `Eval::Evaluate(f, state, Combat, start)` until a
   non-transparent outcome. Around the walk it keeps per-tick side state that the reconciliation after it consumes:
   `castSeen`, `matchedCast` (rule indices), `handClaim`, `wantStance`, `equipHeld`, `silverHeld`, `stopped`,
   `suppressed`, the positional suppression window (`g_recent`, #26, re-anchored by rule uid), the two-tick target
   proposal (`g_proposedTarget`, the "sizing up" hold), and the "combat controller not up yet" wait.
3. **The reconciliation reads the TABLE BY INDEX** (`Scheduler.cpp:1830-1880`): `equipCondKnownFalse`,
   `silverKnownFalse`, the Unholy-silver case and `EquipRangeUndecidable` all loop `rules[i]` up to the stop index,
   and the outcome bookkeeping writes `rules[choice.ruleIndex].lastFired/lastFailReason/lastFiredAt`
   (`:2119-2140`). An IAUS follower has no rows, so these reads need a replacement, not a reuse.
4. **The executors carry a RULE RANK, lower index outranks** (marth 2026-09-08): `g_firingRule = a_choice.ruleIndex`
   (`cast/Fire.cpp:216`), `CastLock::owningRule` (`cast/Actuation_internal.h:250-262`), `ForcedHold::rule`
   (`:432-440`), `LeftHoldRule` vs `g_firingRule` on the heal road (`:494-504`), and `HealSustainLap` treats a lock
   whose `owningRule` is in `matchedRules` as "still asked for" (`cast/HealRoad.cpp:992`, the find at `+18`).
   `Fire` refuses `ruleIndex < 0` (`Fire.cpp:193`). So the rank is both an ORDER and an IDENTITY.
5. **Target resolution** (`Fire.cpp:103-135`): for any subject other than Caster / Enemy, a set `choice.target`
   handle wins at rung 1. `AutoNatureUndeclared` is only consulted when the subject is `Self` (AUTO) and no specific
   actor is named (`Fire.cpp:374-386`). So an explicit target never falls into AUTO.
6. **Pure, cached spell data exists**: `ClassifyArchetype` (shape, engine row, delivery, casting, range, area,
   hostile, two-handed, kind Offense/Buff/Heal), `CastRoadUnsupported` (the spell is castable ONLY by the
   unanimated fallback), `SpellNatureUndeclared`, `SelfDeliveredBuff`, `IsSummonSpell`, `CasterHasLiveSummon`,
   `HealReach` / `HealInReach` (`cast/Actuation.h:386-503, 725-790`). All worker-safe form data except the reach
   and summon list reads, which the Evaluator already makes on the worker.
7. **Weakness** is read as "resist AV < 0" (`Evaluator.cpp:48-51`, `IsWeakTo`). The spell side of the match is the
   effect's resist AV: `EffectSetting::data.resistVariable` at `0x14` (fork `include/RE/E/EffectSetting.h:73`).
8. **The player's combat target** is already read on the worker as
   `pc->GetActorRuntimeData().currentCombatTarget.get()` (`Scheduler.cpp:291-294`, the stall probe). It has
   never been shown to be POPULATED for the player in a fight (principle 5): a passive probe line is part of I6.
9. **Scrolls and staves**: `ScrollItem : SpellItem` with `SpellType::kScroll` (fork `ScrollItem.h:12-13, :38`). The
   engine has `CombatInventoryItemScroll` and `CombatInventoryItemStaff` (fork, both 0x50) and Scroll templates for
   every caster row (Harbinger `Docs/STATUS.md:838`, `VERIFIED-ADDRESSES.md:84/87`, `CombatInventoryItemMagicT`
   Scroll rows). MFO already puts carried scrolls and staves on the Harbinger cast allow-list
   (`apmf/SpellAllowList.cpp:72-80, :238`). **MFO never loots a scroll and never uses one**: there is no
   `act.loot_scrolls`, the loose whitelist is arrows, bolts, gold, potions (memory loose-item-pickup-route2b), and
   `Vocab::IsCastableSpell` rejects every non-`kSpell` type (`Vocabulary.h:29-33`).
10. **Missing opcodes** that some actions need: no `act.bash`, no `act.sneak` (G1), no "has summon" (G5), no
    "spell charging at me" (G11). `act.attack`, `act.power_attack`, `act.taunt`, `act.flee`, the three drinks, the
    three equips (melee, ranged, silver) and the three casts exist (`Vocabulary.h:197-271`).
11. **Class data** (`classes/Classes.h`, `ClassTable.cpp` on main): per class a weapon bitset (`Weapon::kDagger ..
    kStaff, kShield, kUnrestricted`), package flags (`kShortLeash, kNearPlayer, kDisengage, kSpellRange,
    kSneakAlways`), `GoIn`, `partyHealthyPct`, and a priority-ordered `DefaultGambit[]` of (cond, act, param,
    subject, SpellRole, cooldownSec, planCard, pendingRuling). Mages: `kMageSet = kDagger | kStaff`.
12. **Cadence**: one sleeper wake per 133 ms (`Diagnostics.cpp:68`), round-robin, one follower serviced per wake
    (`Scheduler.cpp:481`). With four followers each one decides about every 530 ms.
13. **File sizes**: `Scheduler.cpp` 2271 lines, `Board.cpp` 2069. Both are past the ~1500 plan mark, so any brief that
    edits them proposes the split first, as its own round (CLAUDE.md).
14. **Co-save**: the FLWR v5 `mfoEnabled` byte already carries the helmet override in bits 1-2 as a value-space
    change with no bump (`Serialization.cpp:430-434, :723-726`, `Serialization.h:235-242`). Bits 3-7 are free.
    The MCLS v1 record on `feat/classes-c3-mcls` (`classes/State.cpp:8-34`) is per follower and per CLASS, and has no
    spare flag field.

---

## 1. Architecture

### 1.1 Where it plugs in

```
Scheduler::Tick (worker, AddTask job)                       unchanged up to Scheduler.cpp:1583
  pre-scan: retreat, cannot-act, dead-target, reach hold     unchanged
  COMBAT SCAN  ->  ChoiceSource& src = IausOn(f) ? iausSrc : tableSrc
     for (;;) { choice = src.Next(); if none break;          same loop body as today:
                hand claim, cannot-act, sizing-up, controller wait, Actuation::Fire(f, choice),
                transparent -> src.Reject(choice, outcome); continue;  else stop }
  RECONCILIATION  reads src.Flags() instead of rules[i]      stance, silver, forced weapon, heal sustain, cast release
  BOOKKEEPING     src.Record(choice, outcome)                table: lastFired/lastFailReason   IAUS: momentum, explain
```

- **IAUS replaces only the SOURCE of the choices.** Everything that executes a choice stays: `Actuation::Fire`, then
  `CastOn`, `CastAuto` (never used by IAUS, see 1.4), the heal and buff roads, `Taunt::Fire`, `EquipWeapon`,
  `EquipSilver`, the power-attack range gate, the drinks. So every road, Harbinger claim, ch.25 cast guard, crime
  pre-check, animation rule, cast pacing (`fCastCooldown`) and hand rule is inherited. IAUS adds no engine call.
- **`ChoiceSource`** is the one new interface (in `native/iaus/Iaus.h`). The table source is today's
  `Evaluate(start)` walk plus the index loops of fact 3, moved behind methods: `Next()`, `Reject()`,
  `EquipEvaluatedAboveStop()`, `SilverEvaluatedAboveStop()`, `UnholySilverEvaluated()`, `EquipRangeUndecidable()`,
  `Record()`, `FiredIdentity()` (the suppression anchor). Behaviour for a table follower must be identical (proof in
  card I1).
- **The IAUS source** scores once per tick, then hands out the ranked list one candidate at a time. A transparent
  outcome moves to the next candidate, exactly like a transparent rule. The walk is capped at **6 tries** per tick
  (today a table has at most 12 rows; 6 keeps the worst case below a full table).
- **No rows ever run for an IAUS follower, and no rows are deleted.** The combat table stays in `FollowerState`
  and in FLWR, hidden on the board. Switching back restores it, and a DLL downgrade simply runs it.

### 1.2 Two layers per tick: posture, then action

The table mixes weapon equips and actions in one list, and the scan threads `handClaim` / `wantStance` /
`equipHeld` / `silverHeld` through it. IAUS splits that cleanly:

1. **Posture layer** (weapon category): candidates `keep`, `melee`, `ranged` (bow, crossbow, or staff), `silver`.
   The winner is offered first through the same loop as an `act.equip_*` choice. A satisfied equip is transparent
   (today's contract), so the walk goes on to the action layer in the same tick. A swap that Fires takes the tick.
   The layer's result fills `handClaim`, `wantStance`, `equipHeld`, `silverHeld` directly, and its "no equip order
   wanted" answer is what `equipCondKnownFalse` / `silverKnownFalse` read. The existing dwell
   (`MeleeClampDwell`) and `ReconcileForcedWeapon` are untouched.
2. **Action layer**: every other candidate, scored assuming the posture winner.

### 1.3 Rank seam (order and identity)

`Eval::Choice` gains two fields (it is internal and never serialized, so this is not an ABI change):
`std::uint8_t source` (Table / Iaus) and `int rank`. The table sets `rank = ruleIndex`. IAUS sets
`rank = kIausRankBase - decisionEpoch`, where the epoch increments on every NEW decision (not on every tick):

- **Order**: lower outranks, as today. A newer IAUS decision outranks an older hold, which is exactly "IAUS chose to
  switch". `kIausRankBase` is far above any table index (64 rows max), and the two never mix on one follower.
- **Identity**: the running decision keeps its rank while momentum holds it, so `HealSustainLap`'s "still asked for"
  check works unchanged: IAUS passes `matchedCast = { rank of the running cast decision }` while that candidate still
  scores above the floor.
- `Fire.cpp:193` (`ruleIndex < 0` refusal), `:216` (`g_firingRule`), `:358` (summon log) and every other
  `ruleIndex` reader are moved to `rank` / `source`. Card I1 audits them all (the grep list is in this pass's log).
- **Hard rule: IAUS never preempts a standing heal's hand.** While `HealStandsOnHand(f, hand)` is true, every
  candidate that needs that hand scores 0 for it (the heal-to-full contract, ClickUp 86e3m36qr, marth 2026-10-06).
  The other hand stays usable.
- On the IAUS toggle edge (either direction) every cast lock, heal claim and forced hold of that follower is
  released through the existing stand-down path, so no rank from the old source survives into the new one.

### 1.4 Targets are always explicit

IAUS names the target of every cast: `subject = Player` (any non-AUTO value works) with `choice.target` set, per
fact 5. It never uses AUTO (`CastAuto`) for a single target, so `SpellNatureUndeclared` and AUTO's nature guess
never decide an IAUS cast. The one exception is a party-wide beneficial fan (Candlelight shape): its candidate is
"party", and it uses the AUTO road on purpose because that road already fans.

### 1.5 Threading

- The scorer runs **on the AddTask job worker, inside the scan**, in the `g_followers` domain (#4). It only makes
  reads the Evaluator already makes there today: AV percentages, combat group walk (the `PickFoe` group lock),
  `g_active`, positions, `FoeTargets`, race keywords, resist AVs, `Sightline` cache (Unknown passes), `SelfStatus`
  mirrors, `CombatSense`, `Targeting::Current`, `CalculateMagickaCost` (read on the worker at `Scheduler.cpp:1990`).
- **New state that needs the main thread goes through mirrors** (the `SelfStatus` pattern: one `MainThread::Post`
  refresh, worker reads under a leaf mutex):
  - `SpellBook` (new, `iaus/SpellBook.cpp`): per follower, the known castable spells (VisitSpells) and the carried
    scrolls and staves with counts and charge, each with its cached archetype, nature, element (resistVariable),
    cost, range, area and class-allowed bit. Refreshed on combat start and every 5 s in combat, and on the
    inventory/spell change the board already reacts to. Size: about 40 entries x 48 bytes per follower.
  - `WillWork` CTDA results (from G10's `cast/WillWork.cpp`): requested with a `Want()` for the top 6 spell x target
    pairs only, so the next lap has them. Unknown = pass, and the executor's own G10 check at fire time is the gate.
- IAUS runtime state (momentum, decision epoch, the cooldown map, the RNG, the board readout) is runtime-only and
  is cleared in `ResetAllState` after `StopPump()` (`Serialization.cpp:680/686` order).
- The board readout is a small struct the worker publishes under a leaf mutex. The render thread copies it. No lock
  is ever held across an engine call.

### 1.6 Per-tick cost budget

Realistic mage: 30 known spells (14 offense: 6 fire, 4 frost, 4 shock; 3 control including Turn Undead; 4 heal,
2 of them self-only; 4 buff, 2 self-delivery; 2 ward; 3 summon), 4 scrolls (2 offense, 2 self), a dagger and a staff.
8 foes, 4 allies, the player, self (14 actors).

| Family | Raw pairs | After pruning |
|---|---|---|
| Offense spells | 14 x 8 = 112 | 9 affordable x 5 top foes = 45 |
| Control | 3 x 8 = 24 | 3 x 5, Turn Undead only on undead = 11 |
| Heal | 2 x 1 + 2 x 6 = 14 | hurt targets only (HP < 0.9995): about 4 |
| Buff | 2 x 1 + 2 x 6 = 14 | not already active: about 6 |
| Ward, summon | 2 + 3 = 5 | live summon or no hostile caster: about 2 |
| Scrolls | 2 x 8 + 2 = 18 | 2 x 5 + 2 = 12 |
| Attack (target pick) | 8 | 5 |
| Potions | 3 | 0 to 3 (need + stock) |
| Posture | 4 | 3 |
| **Total** | **about 202** | **about 90** |

- **Pruning, in order**: (P1) static, from `SpellBook`: castable, `CastRoadUnsupported` false (the animation rule),
  class-allowed, not a two-hander when a hand is held. (P2) resource: cost > current magicka drops every pair of
  that spell, no stock drops a potion or scroll. (P3) target set: own combat group only, hostile only (the brawl
  gate), inside the chase cap, alive and enabled, then the top 5 foes by a cheap "foe relevance" score (distance,
  targets-me, targets-player, HP). (P4) hard gates first: the cheapest zero-producing considerations (target
  conditions, already active, out of range) run before the expensive ones. (P5) multiplicative early-out: a
  candidate stops evaluating once `weight x partial product` is below the current k-th best, where k comes from the
  rapport table (section 5). Scores only fall as considerations multiply in.
- **Arithmetic**: about 90 candidates x 7 considerations = 630 curve evaluations at 5-20 ns, about 10 us. Per-actor
  reads (14 actors x about 10 reads at 50-100 ns, done once per actor and shared by every pair) about 15 us.
  Sorting and the pick, under 5 us. **Expected 0.03-0.1 ms per follower tick.**
- **Budget and caps** (INI, frozen names once shipped): `fIausBudgetMs = 0.5` (measured with the existing
  `bProfileEvaluator` / `[eval-profile]` line, a warning line when exceeded, never a silent cut),
  `iIausMaxCandidates = 384` (hit = one WARN per session naming the follower and the family counts).

---

## 2. Candidate generation

A candidate is (action, form, target). Gating by class uses the EQUIPPED class when the class tree is ON
(`classes::Active()`), else the T1 class from `combatClassOverride` (Auto resolves through `ClassResolve.cpp`).

| Action | Opcode / road | Generated when | Class gate (tree ON) |
|---|---|---|---|
| Attack (target pick) | `act.attack`, target latch | per foe in the pruned set | every class. Melee-only followers skip foes out of melee reach (`CombatSense::OutOfMeleeReach`, today's reach gate) |
| Power attack | `act.power_attack` | per foe, melee weapon drawn | classes with a ch.18 mix declared (C2 `powerShareMax` set) or a power-attack default row (Warrior, Champion, Living Legend, Grand Battlemage). G9-style cooldown from the row's `cooldownSec` (10 s) |
| Bash | none exists | not a candidate in this release | stays inside the ch.18 mix (C8). See question 8 |
| Taunt | `act.taunt` (`Taunt.cpp`) | per foe attacking an ally or the player | classes with a taunt default row (Knight line, Great Knight, Ebony Warrior, Living Legend) |
| Sneak | `act.sneak` (G1, not yet) | once G1 lands: self | `Flag::kSneakAlways` or a Thief-line class |
| Retreat | `act.flee` | self | `Flag::kDisengage` classes, low weight |
| Potion drink | the three `act.drink_*` | self, stock > 0 (Logistics supply reads) | every class |
| Posture | `act.equip_melee / ranged / silver` | categories the follower carries | only categories inside `ClassDef::weapons` when strict adherence is ON (G16 off-class rule). Staff counts as ranged for mages (`kStaff`) |
| Cast offense | `act.cast_target`, explicit foe | per affordable Offense-kind spell x foe | spell allowed by the class profile (question 4) |
| Cast control | `act.cast_target`, explicit foe | Calm, Frenzy, Fear, Paralysis, Turn Undead archetypes x foes passing the spell's own target conditions | as offense. Treated as HOSTILE (1dfcuabcr0d) |
| Cast heal | `act.cast_target` / `act.cast_self` / `act.cast_player` | per Heal-kind spell x hurt party member in `HealInReach` (self-delivery = self only) | as offense |
| Cast buff | same three | per Buff-kind spell x party member, not already active. Self-delivery buffs self only (`SelfDeliveredBuff`). Party fan spells = one "party" candidate on the AUTO fan road | as offense |
| Summon | `act.cast_self` (one-shot conjure) | per summon spell, no live summon from it (`CasterHasLiveSummon`) | as offense |
| Ward | `act.cast_self` | per Ward-shape spell | as offense. Full value needs G11 (spell charging at me) |
| Scroll use | new `act.use_scroll` (card S3) | per carried scroll x valid target by its nature | mage line and mage hybrids (ruling Q22/23), Dragonborn |
| Exploit | new `act.exploit` (card I3) | in the full mode it is not a separate candidate: it is the weakness consideration on offense and posture candidates. In gambit mode it is a row that runs the Exploit scorer | every class that can cast offense (spells), plus the silver posture for melee |

Never generated: anything whose road is unanimated (`CastRoadUnsupported`), runes and other Target Location spells
(no NPC projectile, memory animations-required), constant-effect spells, non-hostile actors as foes, the player or a
party member as the target of a hostile spell.

---

## 3. Consideration catalogue

**Curve library** (input normalised to x in [0,1] unless noted):
`Lin(m,b) = clamp(m x + b)`, `Pow(k) = x^k`, `Logi(k,x0) = 1/(1+e^(-k(x-x0)))`, `Step(t, lo, hi)`,
`Inv(k) = 1 - x^k`, `Bin(lo,hi)` for booleans. A consideration with a hard 0 is a veto.
**Weight** = an exponent on the consideration's score (`s^w`), default 1.0, so a class profile can make a
consideration matter more (w > 1) or less (w < 1) without touching its curve. Each action also has an **action
weight** (a multiplier, default 1.0) that the class profile sets.

### 3.1 Shared considerations (used by many actions)

| Id | Input (existing read it reuses) | Curve, defaults | w |
|---|---|---|---|
| `range_fit` | distance to target / the action's reach (spell `range` from `ClassifyArchetype`, `HealReach`, melee reach from `CombatSense`) | `Step` 0 above 1.15x reach, then `Inv(2)` on d/reach so close is best for melee, `Logi(-10, 0.85)` for spells | 1.0 |
| `los` | `Sightline` cache (own ray for casts and ranged, engine basis for melee) | Visible 1.0, Unknown 0.9, Occluded 0.2 (not 0: the cast guard holds, the engine re-aims) | 1.0 |
| `foe_threat` | foe level / self level (`cond.foe_stronger_than_me` read), foe HP pct, foe is caster or ranged (`FoeIsCaster`, `FoeIsRanged`) | `Lin(0.5, 0.3)` on level ratio clamped [0,2] / 2, x1.15 caster | 1.0 |
| `foe_on_me` | `FoeTargets(foe, self)` | `Bin(0.6, 1.0)` | 1.0 |
| `foe_on_player` | `FoeTargets(foe, player)` | `Bin(0.6, 1.0)`, **player focus** (section 5) | 1.0 |
| `foe_on_ally` | `FoeTargets(foe, ally)` with `Taunt::IsPartyMember` | `Bin(0.7, 1.0)` | 1.0 |
| `player_target` | `pc->GetActorRuntimeData().currentCombatTarget` (fact 8) | `Bin(0.7, 1.0)`, **player focus** | 1.0 |
| `current_target` | `Targeting::Current` | `Bin(0.85, 1.0)` (target churn costs a "sizing up" tick) | 1.0 |
| `foe_finish` | foe HP pct (`Vocab::HealthPct`) | `Inv(0.5)` floor 0.4 (low HP foes are worth finishing) | 0.7 |
| `affordable` | `CalculateMagickaCost(f)` vs current magicka, potions and scroll stock | `Step` 0 if cost > pool, else `Lin(0.4, 0.6)` on pool after cast / max | 1.0 |
| `mp_reserve` | self magicka pct after the cast | `Logi(12, 0.15)` (cast till dry is allowed, marth: no reserve floor, so this only shapes, it never vetoes) | 0.5 |
| `cooldown` | runtime map keyed (follower, action, form), set on Fired | `Step` 0 inside a hard cooldown (power attack 10 s), else `Lin(1/T, 0)` recovering over T = 4 s for the same spell | 1.0 |
| `variety` | how many of the last 4 Fired were this same form | `Lin(-0.15, 1.0)` (attacks stay additive: this never removes a move, it only shades a repeat) | 0.5 |
| `will_work` | G10 level cap (worker) + CTDA mirror (Unknown passes) | `Bin(0, 1)` veto | 1.0 |
| `target_cond` | the spell's archetype target rule (Turn Undead = undead only, `cond.foe_is_undead` read) + G10(b) | `Bin(0, 1)` veto | 1.0 |
| `already_active` | the existing "already active" guards (CastAuto's ApplyEffect check, `TargetStreamLive`) | `Bin(0, 1)` veto | 1.0 |
| `hand_free` | `HealStandsOnHand`, `CastInFlightOnHand` | `Bin(0, 1)` veto (1.3) | 1.0 |
| `splash_safe` | area > 0: party members (`g_active`, player) within `area` of the target position | `Step` 0 if any (marth: never splash the party). Bystanders are held by Harbinger ch.25 `kCastGuard_Bystanders` | 1.0 |
| `exploit` | spell element = costliest hostile effect's `resistVariable`, target's resist AV (`IsWeakTo` read) | r <= -25: 1.0, r = 0: 0.6, r = 50: 0.3, r >= 85: 0.08, r >= 100: 0 (veto) | 1.0 |

### 3.2 Per action

| Action | Considerations (beyond `range_fit`, `los`, `cooldown`, `hand_free`) | Action weight |
|---|---|---|
| Attack | `foe_threat`, `foe_on_me`, `foe_on_player`, `foe_on_ally`, `player_target`, `current_target` (w 1.5), `foe_finish`, reach gate | 0.8 |
| Power attack | Attack's set + `foe_blocking` (`cond.foe_blocking` read, `Bin(0.5,1)`), self stamina pct (`Logi(10,0.35)`), foe stronger than me (`Bin(0.7,1)`), hard cooldown | 0.9 |
| Taunt | `foe_on_ally` (w 2), `foe_on_player` (w 2), self HP pct (`Logi(10,0.45)`), already taunted (Taunt state, veto) | 1.0 |
| Sneak (after G1) | no foe has me in sight (G2 / H-DETECT input, unavailable until M2, then `Bin`), fight just started (time since combat start, `Inv(1)` over 6 s) | 0.6 |
| Retreat | self HP pct (`Inv(3)` x `Step` 0 above 0.30), no heal candidate above 0.3, `Flag::kDisengage` | 0.5 |
| Drink health / stamina / magicka | the matching self pct (`Inv(2.5)`, so 0.3 HP scores 0.6, 0.1 scores 0.93), stock > 0, no heal spell candidate above 0.5 (health only), self poisoned (`SelfStatus`) raises cure potions | 1.1 |
| Posture | distance to current target (ranged `Logi(8, 0.45)` on d/1200, melee the inverse), ammo (`cond.self_out_of_arrows` reads, 0 = veto ranged bow), staff charge (charge AV > 0), foe out of melee reach (`OutOfMeleeReach`, melee veto), target Unholy (`cond.foe_is_unholy` read, silver `Bin(0.2,1)`), class weapon set (veto when strict), swap momentum x1.5 and the existing dwell | 1.0 |
| Cast offense | `foe_threat`, `foe_on_player`, `player_target`, `current_target`, `exploit` (w 1.3), `affordable`, `mp_reserve`, `splash_safe`, area value (foes within area / 3, `Lin(1,0.5)` when area > 0), `variety`, `will_work` | 1.0 |
| Cast control | `foe_threat` (w 1.5, control the dangerous one), foe not already controlled (veto), `will_work` (veto, G10 level cap), `target_cond` (veto), foe count >= 2 (`cond.foe_count_at_least` read, `Bin(0.3,1)`: controlling the only foe wastes a turn), `affordable` | 0.9 |
| Cast heal | recipient HP pct (`Inv(2)` capped by `kHealFull`, so full = 0), recipient is the player (**player focus**), recipient downed (`ActorDowned`, x1.4), `HealInReach`, `affordable`, self HP pct for self heals (`Inv(2)`) | 1.2 |
| Cast buff | not `already_active`, in combat less than 20 s (`Inv(1)` over 20 s, buffs early), recipient is in melee with a foe (armor / cloak types), recipient is the player (**player focus**), `affordable` | 0.7 |
| Cast summon | no live summon (veto), foe count (`Lin(0.25, 0.5)` on count / 4), outnumbered (foes > party, x1.3), `affordable` | 0.9 |
| Cast ward | hostile caster targeting me (`FoeIsCaster` and `foe_on_me`; full value needs G11's "charging at me"), self HP pct (`Inv(1.5)`), `affordable` | 0.8 |
| Scroll use | the matching spell family's set for the scroll's spell (offense / heal / buff), plus scroll stock (`Lin(0.2, 0.8)` on count / 5, hoard the last one a little), magicka low (`Inv(1)` on self MP: scrolls shine when dry) | 0.9 |
| Exploit (gambit row) | the offense set with `exploit` at w 2.0, restricted to the row's chosen foe | n/a (row) |

---

## 4. Combination

1. **Per consideration**: `s_i = curve_i(x_i) ^ w_i`. Any `s_i = 0` ends the candidate (veto).
2. **Compensation factor** (Dave Mark's IAUS): with `n` considerations, `mod = 1 - 1/n`, `makeUp = (1 - s_i) * mod`,
   `s_i' = s_i + makeUp * s_i`. This stops a candidate with many considerations losing to one with few just by
   multiplying more numbers below 1.
3. **Candidate score**: `S = actionWeight * classWeight(action) * PlayerFocus * Π s_i'`, then x momentum.
4. **Momentum**: the running decision (the last Fired candidate key, while it is still generated) gets x1.25
   (`fIausMomentum`). The posture layer uses x1.5 plus the existing dwell (swaps are expensive and animated).
5. **Commitment window** (replaces the positional suppression window, #26, same length: `g_suppressWindow x (0.88 +
   0.24 Temperament)`): after a Fired, a different candidate may preempt only if `S_new > S_running x (1 +
   fIausPreemptMargin)`, default 0.25. Otherwise the scan stops "suppressed", as today. This keeps #26's spirit (a
   more urgent action still preempts) without row positions.
6. **Floor**: if the best score is below `fIausActFloor = 0.20`, IAUS issues nothing and the follower runs his own
   combat AI and package (the "no rule matched" path, composition not substitution).
7. **Cooldowns**: the engine's own cast timing and `fCastCooldown` stay in the executors (memory
   cast-cooldown-inert-is-correct: no new pacing). The `cooldown` consideration carries the class rows' hard
   cooldowns (power attack 10 s) in a runtime map, not saved (a cooldown that resets on load is G9's documented
   contract too). Card I2 shares one `Cooldowns` helper with G9 so both modes agree.
8. **Ties** (|dS| < 0.005): the running decision, then the candidate on the current target, then lower magicka cost,
   then the lower (action, FormID) key. Deterministic, so logs reproduce.
9. **Per-class weights override defaults** in two places only, both data in the class table (card I8):
   - `classWeight(action family)`: derived mechanically from the class's `DefaultGambit[]`. Each applied row adds
     `0.5 x (rows - index) / rows` to its action family's weight (base 1.0), so the first row of a 5-row set adds
     0.5 and the last adds 0.1. A row's SpellRole (Fire, Heal, Summon, ...) narrows the bump to that spell family.
   - consideration weight overrides: a row's condition opcode maps to a consideration (`cond.foe_is_caster` ->
     `foe_threat` caster term, `cond.foe_attacking_ally` -> `foe_on_ally`, `cond.foe_weak_*` -> `exploit`,
     `cond.ally_hp_pct_below` -> heal recipient HP, ...) and that consideration's `w` becomes 1.3 for that family.
   - plus a short hand-written override list per class for what the rows cannot say (Witchhunter melee veto unless
     every foe is controlled, Q10, Templar silver bonus vs undead, Q27).
   With the tree OFF (or the add-on absent) the three T1 profiles are built the same way from the T1 rows that are
   kept as data (`kMeleeG`, `kRangedG`, `kMageG`).

---

## 5. Rapport mapping

### 5.1 Decision sharpness

Sort candidates by `S`. The pool is the top `k` candidates with `S >= floor x S_best`. Pick with probability
`p_i ∝ exp((S_i - S_best) / τ)`. Rolled once per NEW decision (not every tick, so the follower does not dither),
with a per-follower RNG seeded from `Temperament(id)` and the decision epoch.

| Rank | k | floor | τ | Example: scores 0.80 / 0.72 / 0.65 / 0.55 |
|---|---|---|---|---|
| I | 5 | 0.60 | 0.12 | 52% / 27% / 15% / 6% |
| II | 4 | 0.70 | 0.08 | 66% / 24% / 10% / 0 (0.55 under the floor) |
| III | 3 | 0.80 | 0.05 | 80% / 16% / 4% |
| IV | 2 | 0.90 | 0.02 | 98% / 2% (0.72 is exactly at the floor) |
| V | 1 | 1.00 | n/a | 100% best |

The floor is what keeps a low-rapport follower sane: a clearly best emergency heal (say 0.9 against 0.4) is always
picked at every rank. Randomness only swaps near-equals (question 3).

### 5.2 Player focus

`PlayerFocus = 1 + (m - 1) x relevance`, where relevance is 1 for a candidate that heals or buffs the player,
protects the player (its foe targets the player), or attacks the player's target, and 0 otherwise. Others are not
penalised, they are just outranked more often.

| Rank | I | II | III | IV | V |
|---|---|---|---|---|---|
| m | 1.00 | 1.10 | 1.20 | 1.35 | 1.50 |

Applied once per candidate even when several player relations hold (cap 1.5).

---

## 6. Explainability

- **One log line per DECISION** (on change, plus every 10 s while it runs), tag `[iaus]`:
  `[iaus] Lydia (000A2C94) pick=cast_target Firebolt(0001C789) -> Draugr Wight(0003B547) 0.712 | 2 attack -> Draugr Wight 0.640 | 3 cast_self Fast Healing 0.410 | n=90/202 r=III roll=0.37 mom=+25% walk=1 0.06ms`
- A transparent fall-past is logged in the same line (`walk=2 (1: Firebolt 'insufficient magicka')`), the IAUS
  analogue of today's skip chain, so a decline is never silent.
- `bIausExplain` (default OFF, a debug key): a `[iaus-why]` line per top-3 candidate with every consideration's
  input, curve output and weight, e.g. `exploit r=-50 1.00 w1.3 | range 0.92 | affordable 0.85 | threat 0.70 | cw 1.4`.
- **Board readout**: when IAUS is ON, the combat tab shows a read-only "Thinking" panel instead of the table: the top
  3 candidates (action, form, target, a score bar), the last pick and how long it has run, the rapport rank's
  sharpness, and the floor line. It comes from the worker's published snapshot (1.5). Gamepad: the panel is read-only,
  so only the toggle is focusable (family controller rule). New file `Board_Iaus.cpp` (`Board.cpp` is 2069 lines).
- Optional combat-GUI readout (the 86e3dhaba class-info overlay): one line "Thinking: Firebolt -> Draugr Wight",
  OFF by default, behind the same toggle as the class info.

---

## 7. Save and compat

**Yes, a per-follower, per-table toggle is a co-save change, but only a value-space one, with no layout change and no
version bump.**

- **Recommended home: the FLWR v5 `mfoEnabled` byte, bits 3 and 4**, on the helmet-override precedent (fact 14):
  bit 3 = combat IAUS, bit 4 = logistics IAUS (written 0 and ignored on read until 1dfcuabcr0g ships, which is
  what "greyed out" means). Written only while `mfoEnabled` is true, like the helmet bits.
  - An older DLL reads `byte != 0`, so it sees the same enabled state, ignores the bits, and runs the table that
    IAUS kept. A round trip through an older DLL drops the toggle (it writes 0/1), and the follower comes back on
    gambits: safe in direction.
  - Cost (the same one the helmet override documents): a follower saved with MFO switched off for him loses his
    IAUS toggle.
  - It works with the class add-on absent, which matters because the toggle is per follower and per table, not per
  class.
  - G9 is about to bump FLWR to v6 for the per-rule cooldown. Its brief must keep this byte as is (one line in the G9
    brief). The toggle does not depend on G9.
- **Not MCLS**: MCLS is per follower and per CLASS, exists only with the class ESL, has no flag field, and is still
  unmerged. Putting the toggle there would need MCLS v2 (or reopening a reviewed v1), and the toggle would vanish
  with the ESL.
- **Not a new record**: overkill for 2 bits.
- **Not saved at all**: momentum, the cooldown map, the decision epoch and the RNG are runtime-only (cleared in
  `ResetAllState` after `StopPump()`). The class weight profiles are compiled data.
- Tier: A by the co-save carve-out (small: one writer line, one reader line, the `Serialization.h` note). A save and
  load round trip plus an older-DLL load is the proof.

---

## 8. Scrolls and Exploit: the first IAUS-powered pieces

These ship BEFORE the full mode as gambit actions whose INSIDE is IAUS-scored. They prove the scorer in the field on
a small surface, and the full mode then reuses the same modules.

### 8.1 Exploit (`act.exploit`, new appended opcode)

- Gambit row: "Foe: <any foe selector> -> Exploit". The condition picks the foe as today (`PickFoe`). The action
  runs the IAUS offense scorer (section 3, `exploit` at w 2.0) over the follower's known offense spells for that one
  foe, and fires the winner through `CastOn` with the explicit target. If the follower is melee and the foe is
  Unholy, the silver posture candidate competes too (`EquipSilver`).
- Gates inherited: `will_work` (G10), the spell's own target conditions, ch.25 cast guard, the animation rule
  (`CastRoadUnsupported` spells are not candidates). If nothing scores above the floor the row is transparent
  ("exploit: no spell this foe is weak to or neutral to") and the scan falls through.
- Existing `cond.foe_weak_fire/frost/shock` rows keep working. A class default (Evoker, ruling line 47: "fire on a
  fire-weak foe") can switch to Exploit later, by marth's choice only.

### 8.2 Scrolls

1. **Acquisition is loot** (logistics, untouched by IAUS): a new `act.loot_scrolls` category plus `ScrollItem` on
   the route-2b loose whitelist (walk there, VM `Activate` at arrival, never an in-place pickup). Ownership,
   `IsOffLimits` and the crime pre-check are inherited from `Admit()` (followers never commit detected crimes).
   If the Synthesis item catalogue (`mfo_items.json`) has no scroll category, card S2 adds one.
2. **Use must be animated.** MFO has no scroll road today. The engine has one: the AI's own
   `CombatInventoryItemScroll` (Scroll templates on every caster row) equips the scroll and casts it with the full
   animation, the same "activate the AI-fired cast path" shape as our spells. The Harbinger claim already
   allow-lists carried scrolls. **Unproven** (principle 5): whether the engine builds a scroll item for a FOLLOWER,
   whether a Harbinger cast claim can DRIVE a `kScroll` form (seat 0 classify, CheckCast), and whether the count
   decrements. So card S0 is a passive probe first. If S0 finds no animated road, scrolls stop there and go to an
   Opus RE brief. An unanimated `CastSpellImmediate` from a scroll is not acceptable as the shipped answer.
3. **`act.use_scroll`** (new appended opcode, gambit row) picks the best carried scroll for the moment with the
   IAUS scroll considerations, then the claim road. In the full mode, scrolls are ordinary candidates.
4. **Staves and wands**: weapons, so they belong to the posture layer. The mage weapon set already has `kStaff`.
   Any modded "wand" that is a `WEAPON_TYPE::kStaff` works the same way. Staff charge (the item charge AV) is a
   posture consideration. The engine fires the staff itself (`CombatInventoryItemStaff`), so it is animated.

---

## 9. Build cards

Tier key as in the class plan: **A** = Opus authors, full Opus review until nothing is above SEV-3. **B** =
sonnet-coder authors, one Opus pass. A NEW mechanism gets the full (tier 3) review depth even when Sonnet authors
(CLAUDE.md rule 10). Effort 1-5.

| Card | What | Tier | Effort | Milestone | Depends |
|---|---|---|---|---|---|
| **I0** | Scheduler combat-scan split: move the scan and its reconciliation (`Scheduler.cpp:1583-2230`) into `native/scheduler/CombatScan.cpp`, splitcheck + linecheck proven | A (TU split) | 3 | M1 | none |
| **I1** | `ChoiceSource` seam + `Choice.source/rank`: the table source behind the interface, every `ruleIndex` reader moved to `rank`. Proof: an IAUS-off deck run whose `[eval]` lines match a main run on the same save | B (wide, borderline A: allow 2 rounds) | 4 | M1 | I0 |
| **I2** | `native/iaus/` core: curve library, consideration framework, compensation, momentum, commitment window, ties, rapport pick, cooldown helper shared with G9, `[iaus]` log. Unit-testable off line (a tools/ harness feeding recorded inputs) | B, tier-3 review (new mechanism) | 4 | M1 | none (parallel to I0/I1) |
| **I3** | `act.exploit` gambit row on I2's offense scorer + board picker + silver competition | B | 2 | M1 field | I2, G10 (will_work) |
| **S0** | Passive probe: scroll in a follower's inventory, log scroll combat-inventory builds and uses, a Harbinger claim on a scroll form (read-only), count before and after | Opus RE brief + probe | 2 | M1 (parallel) | none |
| **S1** | Harbinger: let the cast claim drive a `kScroll` form, only if S0 shows a gap | A (Harbinger seat) | 3 | M2 | S0 |
| **S2** | `act.loot_scrolls` + loose whitelist + catalogue category | B | 2 | M2 | none (crime gates inherited) |
| **S3** | `act.use_scroll` gambit row + IAUS scroll candidates | B | 3 | M2 | S0 (and S1 if needed), I2 |
| **I4** | `SpellBook` main-thread mirror + candidate generation for every existing action + the catalogue of section 3 | B, tier-3 review (new mirror) | 4 | M2 | I2, G10 |
| **I5** | FLWR bits 3-4 toggle (writer, reader, `Serialization.h` note) | A (co-save value space) | 1 | M2 | none |
| **I6** | Full mode: IAUS source in the scan, posture layer and its reconciliation flags, heal-hand rule, commitment window, toggle-edge stand-down, player-target probe line | B, tier-3 review | 5 | M2 field | I1, I4, I5 |
| **I7** | Board: per-follower toggle (Combat live, Logistics greyed), "Thinking" panel in `Board_Iaus.cpp`, gamepad, MCM `bIausAvailable` | B | 3 | M2 | I5, I6 (panel data) |
| **I8** | Class profiles: weights derived from `DefaultGambit[]` + override list, T1 profiles for tree OFF | B | 3 | M2 (T1-T2), grows in M3-M5 | C2, C12, I2 |
| **I9** | Tuning pass on field data (curve parameters, rapport table, budget check) | B (data only) | 2 | M3 | I6 field |
| **I10** | Credits: LandingCrew in release notes, README, Nexus bbcode | C | 1 | release cut | none |

**Existing cards this touches:**
- **G9** per-rule cooldown: I2's `Cooldowns` helper is shared, so the 10 s power-attack cooldown means the same thing
  in both modes. IAUS does not need G9's FLWR v6 field (its cooldowns come from class data). G9's brief must keep
  the `mfoEnabled` byte unchanged.
- **G10** level skip: a hard input for I3 and I4 (`will_work`). Until G10 lands, IAUS excludes level-capped
  archetypes (Calm, Frenzy, Fear, Rally, Turn Undead, Paralysis, Reanimate) from candidates and logs it once per
  session. That is a documented degrade, not a mask.
- **1dfcuabcr0d** AUTO hostility: IAUS never relies on AUTO for single targets, but it uses the same nature
  classifier, so the control-is-hostile change must land in the shared classifier (`Archetype.cpp`) and not in
  `Auto.cpp` only. Then the picker, AUTO and IAUS agree.
- **C8** packages: no conflict. IAUS outputs the same `wantStance`, and C8 picks the class CSTY for it. Bash and the
  power share stay in C8's ch.18 mix.
- **C12** defaults: IAUS reads the same `DefaultGambit[]` rows as weights (I8). With IAUS ON, C12 still stores each
  class's table (Q8, MCLS), it just does not run.

**Field sessions**: M1 = I3 Exploit rows on a fire-weak and a frost-resistant foe, plus the S0 probe. M2 = the full
mode on one mage and one melee follower with logs (`bIausExplain` ON for that session only), a save and load round
trip with the toggle set, and an older-DLL load of that save.

---

## 10. Open questions for marth (each with a recommended default)

1. **Per-follower default.** Recommend OFF for every follower (their tables keep running). IAUS is turned on per
   follower on the board. The feature itself ships available (`bIausAvailable = 1`).
2. **Tree OFF / add-on absent.** Recommend IAUS works in base MFO with the three T1 profiles, and the class add-on
   only refines the weights.
3. **Low-rapport randomness in an emergency.** Recommend no special exemption: the floor-gated pool (5.1) already
   makes a clearly best emergency heal win at every rank, so randomness only swaps near-equals.
4. **Spells for melee and ranged classes under strict adherence.** Recommend that spells are not class-gated, only
   weighted (a Warrior can still cast his self heal, it just scores low), and strict adherence keeps gating weapons
   only (G16). The alternative is to limit each line to the schools its default rows name.
5. **Player focus strength.** Recommend m = 1.5 at rank V, and it boosts only (it never suppresses
   non-player candidates).
6. **Who uses scrolls.** Recommend: every follower may LOOT scrolls (the category), and only the mage line, mage
   hybrids and Dragonborn USE them (ruling Q22/23). Tree OFF = the Mage T1.
7. **"The table is gone".** Recommend hidden and kept, not deleted, so switching back restores it and a DLL
   downgrade stays safe.
8. **Bash.** MFO has no bash action. Recommend leaving bash inside the ch.18 attack mix (C8) for this release, rather
   than adding an `act.bash` and a Harbinger drive.
