# Taunt gambit: design and RE (ClickUp 86e3g6duw)

Status: DESIGN. Slice S0 (probes) and slice S2 (`cond.foe_attacking_ally`, `act.taunt`, the executor: `native/Taunt.cpp`, `native/apmf/TauntClaims.cpp`, MAP section 2 "Taunt.cpp") are built, see MAP.md. Written as design only, no production code. Written 2026-10-12 against MFO origin/main `955645e` and
Harbinger (APMF) origin/main `3f22ae4`. Addresses were read from our unpacked executables
(1.6.1170 "AE", 1.5.97 "SE") with the RE helper; ids are Address Library ids from LoreRim's
`versionlib-1-6-1170-0.bin` / `version-1-5-97-0.bin`.

Every claim below is tagged **PROVEN** (read in disassembly, records or a field log) or
**INFERRED** (reasoned, not observed). Principle 5 applies: nothing INFERRED is built on until a
probe or a field line shows it.

## 0. The shape marth chose (2026-10-12)

marth, verbatim: *"weighting gains us nothing, unless its the only path to animation … even taunt is
pretty simple, a coin toss based on level or the existing weighting we can read and then pin the
enemy NPCs affected by the taunt to the taunter as the target for 15 seconds or something."*
And: *"How it ranks doesnt matter, its configured in harbinger. We have MFO configured to win
whenever its active, someone else might do something different."*

So the taunt is:

1. **Animation:** the follower plays a taunt idle through Harbinger ch.12. This part is
   non-negotiable.
2. **Roll:** each affected foe gets a level-based coin toss. The brief allowed reading an existing
   engine weight instead. Section 1 shows there is no stored per-candidate value to read.
3. **Effect:** on success, a Harbinger **ch.20 pin on the ENEMY** with target = the taunter. It
   lasts about 15 s, then MFO releases it and the engine picks its own target again.
4. **Ranking:** the enemy pin is a normal ranked Harbinger claim. Who wins against
   SmartNPCTargetSelector / Aggro Management or any other source is the user's Harbinger slider
   setting, not part of this design.

Rejected in one line: biasing the engine's per-candidate score (weighting), the engine's own aggro
events, and a bare `StartCombat(taunter)` are not the mechanism. marth ruled weighting out for this
case, and the other two do not hold the foe (sections 1 and 2).

ClickUp constraints carried: no dependency on JaySerpa's mods (Taunt Your Enemies and Remote
Interactions are installed on LoreRim, reference only). No soft target preferences in MFO; that is
a possible later Harbinger feature, ClickUp 86e3pkftn. Note: the reference file
`scratchpad/jayserpa-gambits-and-hotkeys.md` was not found on disk, so this doc uses only the
brief's summary of it.

## 1. How the engine chooses an enemy's combat target

### 1.1 The call chain: PROVEN on both runtimes

| Step | AE (1.6.1170) | SE (1.5.97) | Notes |
|---|---|---|---|
| `Actor::UpdateCombat`, Character vtable slot 0xE4 | 0x6B6E70 (id 38565) | 0x625700 (id 37612) | per-actor combat update, BSJobs worker |
| `CombatController::UpdateTarget` | 0x559930 (id 33236) | 0x4FE300 (id 32489) | only caller is UpdateCombat (AE 0x6B6F1C, SE 0x6257AC) |
| `CombatTargetSelector::SelectTarget`, vtable slot 6 | | | walks `targetSelectors`; the FIRST non-zero answer wins |
| `CombatTargetSelectorStandard` slot 6 | 0x84CFE0 (id 47191) | 0x7B5E50 (id 45918) | the scoring picker |
| `CombatTargetSelectorStandard` scorer | 0x84D280 (id 47195) | 0x7B5FF0 (id 45922) | called by slot 6 when the cache is stale |
| `CombatTargetSelectorFixed` slot 6 | 0x84DAF0 (id 47199) | 0x7B6920 (id 45926) | returns the handle it was built with |
| `CombatController::SetTarget` | 0x559630 (id 33235) | 0x4FDFE0 (id 32488) | writes `targetHandle` (+0x2C) and the actor's `currentCombatTarget` |

Selector layout (from both constructors, Harbinger `TargetPin.cpp` header): +0x10 controller, +0x18
cached handle, +0x1C priority, +0x20 flags, +0x28 last-evaluation time, +0x2C re-evaluation
interval. Only two concrete selector classes exist in the image's RTTI: `Standard` and `Fixed`.
The base class slot 6 is a purecall.

### 1.2 Standard slot 6: a cached pick, re-scored on a timer (PROVEN, AE 0x84CFE0)

- It reads the cached handle at +0x18 and asks `0x807E20(group, cached)` whether it is still valid.
  If it is valid and `now - (+0x28) <= (+0x2C)`, it returns the cached handle without scoring.
- Otherwise it calls the scorer (0x84D280). If the pick changed, it stores it and sets the interval
  to **`fCombatSelectTargetSwitchUpdateTime`** (exe default 3.0 s). If the pick is the same, it uses
  **`fCombatSelectTargetUpdateTime`** (exe default 1.0 s). Both GMST names were resolved through
  the Setting objects. SE uses the same two GMSTs.
- Consequence for the taunt: Harbinger's slot-6 seat runs on **every** UpdateTarget call, after the
  original. The engine's cache never decides what the pinned foe fights. INFERRED, to be shown in
  the field: after release, the cached pick (often the foe's pre-taunt target) can come back as
  soon as the next update. That is the hand-back.

### 1.3 The per-candidate score (PROVEN, AE 0x84D280 and SE 0x7B5FF0, same constants)

There **is** a per-candidate score. It is an additive number computed on the stack for each entry
of `combatGroup->targets` and thrown away afterwards. Nothing persistent stores it. The highest
score wins; the initial best is -FLT_MAX.

**Who is a candidate.** A `CombatTarget` entry (stride 0xA8) is scored when `detectLevel` (+0x04)
is > 0, `stealthPoints` (+0x08) is <= 0 and `kTargetLost` (+0xA6 bit 1) is clear. The current
target (+0x2C) is also scored when it carries `kTargetKnown` (bit 0). Each candidate must also pass
`0x84D940`: the `bEncounterZoneTargetRestrict:Combat` encounter-zone test and one actor-flag test
(+0xE8 bit 26 with a target vfunc 0x2F0). If nothing is scored, a fallback list sorted by the age of
`lastKnownTimeStamp` (+0x90) supplies the first entry that passes the same gate.

**The terms.** Self = the attacker, T = the candidate, cur = the controller's current target.

| Term | Value | What feeds it | Tag |
|---|---|---|---|
| Recent hostile act by T | +1000 if a record from `0x68F7A0(self, T)` is < 2 s old and its byte +0x15 is set, or T is cur and its +0x18 stamp is < 2 s old. +100 more if byte +0x14 is set | a per-(self, T) record found through `self->currentProcess->middleHigh` (+0xF8 then +0x10, map lookup `0x70B5C0`) | Terms PROVEN. That the record means "T hit or attacked me" is INFERRED |
| Stickiness | +100 if T is cur | the current target | PROVEN |
| Same state | +100 if `0x853D80(T) == 0x853D80(self)` | an actor boolean, not identified | value PROVEN, meaning INFERRED |
| Proximity | +1000 x (1 - min(dist, 2048)/2048). Another cell counts as 2048 (0 for cur) | distance | PROVEN |
| Crowding | + a runtime float (BSS 0x31AB8C8) if `attackerCount` (+0xA4) > (T is cur ? 1 : 0) | how many group members already attack T | term PROVEN. The value is set at runtime, sign unread |
| Unreachable | -2000 if the melee reach test `0x55B310` fails. It is skipped for a ranged attacker (reach >= `fCombatRangedDistance` = 1024) in the same cell | pathing / reach | PROVEN |
| Other cell, no path | -2500 if T is in another cell and `0x853F10` fails | cross-cell reach | PROVEN |
| T state A | -500 if T's vfunc +0x4C8 (arg 0) is true | an actor state | value PROVEN, meaning INFERRED |
| T state B | -500 if `0x67EFE0(T)` is true | an actor state | value PROVEN, meaning INFERRED |

**What is NOT in it (PROVEN by absence in the scorer):** accumulated damage, a threat table, faction
or "attacked by" history older than 2 s, and the detection level beyond the > 0 gate. The vanilla
engine has **no aggro memory**. Its "threat" is proximity, a 2 s recent-attack bump and stickiness.
SmartNPCTargetSelector exists to add a hate table on top of this (its INI: `fHateHalfLife = 30`,
`fTauntBonus = 1000` "for targets under the effect of Courage or Ally spells").

**`CombatMember::threatValue`** (CombatGroup members, +0x08) is the group-strength value of the
group's own members, not a per-target score (CommonLib layout, not used by the scorer). It is not
a taunt input.

### 1.4 What this means for "read an existing weight"

Nothing per-candidate survives the scoring call, so there is nothing to read without a hook. A
read-only probe could log the score, but a hook on the scorer is a new tier-A seat just to feed a
coin toss. **The roll is level-based** (section 4.3).

## 2. Making the foe target the follower

### 2.1 Chosen: a Harbinger ch.20 pin ON THE ENEMY

**Can ch.20 pin a hostile NPC today? Yes, by the code. It is not field-proven.**

- The call-time refusals (`ControlMap.cpp:240` and siblings, FACETS ch.20 "Refusals") reject only
  the PLAYER as the actor, form 0, the actor itself, and the global gates (VR, runtime, self-check,
  `bTargetPin`). Nothing checks for a teammate or a client follower. PROVEN (code read).
- The seat is on both selector vtables and so runs for every NPC. It looks the actor up by the
  controller's `attackerHandle` in the ControlMap snapshot (`TargetPin.cpp:255-300`). A claim on a
  bandit is found exactly like a claim on Lydia. PROVEN (code read).
- **Pinnability condition:** the seat replaces the answer only when the engine's own answer is
  non-zero (the foe is fighting) **and** the taunter is in the FOE's `combatGroup->targets`. A foe
  fighting the party normally holds every party member it has detected. INFERRED, and that is probe
  P2. If the taunter is absent, the pin pauses ("not in group"). The fix for that case is a ch.21
  entry on the foe naming the taunter (2.2).
- **The engagement hold applies to the foe (PROVEN, `TargetPin.cpp:862` `hold::SetPin`).** Every
  ch.20 pin registers a held engagement for the PINNED actor. For the taunt, that is the foe. See
  section 5 for what that does and what must be checked.

**Coexistence:** the pin is one ranked claim on the `Target` sources facet. With marth's defaults
MFO.dll (50) outranks a hook DLL (20), so Harbinger restores the pin over a foreign 0xE4 write. A
user who ranks a hook above MFO gets that hook's target. Per marth, this is configuration, not
design.

**Animated?** The pin itself plays nothing. The foe fights the taunter with its own AI: its own
attacks, movement and spells, all animated. The taunter's animation is ch.12 (section 3).

**Pin or weighting?** A pin, by marth's ruling. It is a source deny at the selector seat, but it
only chooses among targets the engine already holds, so it never invents a fight.

### 2.2 Helper: ch.21 entry on the foe, only when the taunter is not a group target

`StartCombat(taunter)` on a foe that is already fighting adds the taunter to the foe's group targets
**and keeps its current target** (FACETS ch.21 Key Concept 14; this is how Provoke behaves). On its
own it does not re-target. That is the open ClickUp question, answered by the existing ch.21
contract (INFERRED from the engine read behind ch.21; the field will show it). As a helper it makes
the pin pinnable. Use it only when P2 shows the taunter ABSENT from the foe's group, with `fval = 0`
so the NOT-ENGAGED drop never applies (section 5). Release it together with the pin. Releasing it
stops nothing.

### 2.3 Rejected (one line each)

- (b) Biasing the per-candidate score: weighting. marth ruled it gains nothing here, and it would be
  a new tier-A seat on the scorer.
- (c) The engine's own aggro events: the only one is the 2 s recent-hostile-act +1000. It fades in
  2 s, so it cannot hold for 15 s.
- (d) Bare `StartCombat(taunter)` (Provoke): it adds the target and keeps the current one, so it
  does not re-target. It survives only as the helper in 2.2.

## 3. Animation: the taunt idle through ch.12 in combat

### 3.1 The record (PROVEN, Skyrim.esm parsed from LoreRim Stock Game)

`0x000B8C73 CombatIdleABC`: behaviour `0_Master.hkx`, event **`IdleCombatStart`**, **no conditions
of its own**. It sits in this tree:

| Record | Conditions (func ids from the fork's `TESCondition.h`) |
|---|---|
| `0x0B8C70 CombatIdleRoot` | not the player (GetIsID 7 == 0); IsWeaponOut == 2; IsBleedingOut == 0; GetCombatState != 2 (not searching); IsAttacking == 0; IsCasting == 0; IsStaggered == 0; IsRecoiling == 0; IsFleeing == 0; GetGraphVariableInt(hash 0x1F88ABC0) != 1 |
| `0x01BA8B CombatIdleStandard` | race list (OR, 11 races); **IsPathing == 0**; **GetDistance(combat target) > 250**; right hand GetEquippedItemType **1..6** (melee weapon) |
| `0x0B8C72 CombatIdleStandard_withShield` | left hand item type == 10 (**shield**), right hand 1..4 (one-handed) |
| `0x0B8C74 CombatIdleShield` (tried first) | GetRandomPercent < 70; event `IdleCombatShieldStart` |
| **`0x0B8C73 CombatIdleABC`** | none; event `IdleCombatStart` |
| `0x0B8C71 CombatIdleRegular` (under CombatIdleStandard, no shield) | none; event **`IdleCombatStart`**, the same event |

What this means:

- **0x0B8C73 fits only a shield + one-hander.** For other melee followers the same animation event
  is reachable through `0x0B8C71 CombatIdleRegular`. The taunt should pick the idle by loadout:
  shield + 1H uses 0x0B8C73, other melee uses 0x0B8C71.
- **Archers, staff users and casters have no humanoid combat idle in this tree** (right hand 7..9
  fails CombatIdleStandard). That is open question Q2.
- The draugr taunts (`1HMTauntACombat` 0x0BE3C8 and its siblings) are DraugrBehavior-only and
  useless for humanoid followers.
- Harbinger's PlayIdle is `SetupSpecialIdle(process, actor, kActionIdle, idle, checkConditions =
  true, false, target)` (AE 0x6DDE70 id 39256, SE 0x64B140 id 38290; PROVEN, `Idle.cpp:47` and
  VERIFIED-ADDRESSES). So the engine evaluates conditions. Whether it also evaluates the PARENT
  chain (root, Standard, withShield) is **INFERRED**, and probe P1 settles it. If it does, the
  taunter must be standing (not pathing) **more than 250 units from its own combat target** with
  the weapon out. That favours a taunt at range, which is right for the role. Hold the body with
  ch.1 during the idle (the FACETS ch.12 recipe).

### 3.2 Field evidence: none in combat yet

The LoreRim logs `APMF.log` through `.log.5` (2026-10-12, 11:24 to 21:47) hold exactly one ch.12
play: `0x00013491 idle IdleLockPick ... ANIMATION CONFIRMED (WEAK evidence: 'IdleOffsetStop' ...)`
at 21:26:48, out of combat. Earlier field proof is IdleGive (2026-09-28), also out of combat. **No
log shows a ch.12 idle in combat.** It is UNPROVEN that the combat behaviour graph accepts
`IdleCombatStart` from PlayIdle. The engine's own combat idle leaf uses the same records, which
makes it plausible (INFERRED). P1 and the first gated field run must show `ANIMATION CONFIRMED`
with a non-WEAK entry event.

## 4. The gambit shape

### 4.1 Vocabulary (append-only opcode strings, INVARIANTS #10)

- **Condition `cond.foe_attacking_ally`** ("Foe: Attacking an ally"): a FOE SELECTOR in the existing
  shape (`Evaluator.cpp` around :479, the same loop as `cond.foe_attacking_player` /
  `cond.foe_attacking_me`). It picks a foe whose combat target is a party member other than self:
  the player, a player teammate, or a summon commanded by one of them. Ties go to the nearest foe.
  The existing `Foe: Attacking player` also works with Taunt, so a dedicated ally selector covers
  the follower-tank case.
- **Action `act.taunt`** ("Taunt"): no parameter. The selected foe is the primary target; the area
  rule (4.2) adds others.
- New board strings go in the i18n tables. No co-save layout change, because opcodes persist as
  strings.

### 4.2 Which foes the taunt affects

Proposed defaults, which are Q1:

- **Primary:** the foe the condition selected.
- **Area:** every other foe within **`fTauntRadius` = 1024** units of the taunter (the engine's own
  `fCombatRangedDistance`), up to **`iTauntMaxFoes` = 3** in total, nearest first.
- **Eligible foe:** alive, loaded, not the player, in combat, and **its combat target is in the
  party** (the same party test Harbinger's NOT-ENGAGED check uses, `CombatEntry.cpp:902-918`). A
  foe that is not fighting the party is never taunted, because a taunt must not start fights.
- **It must perceive the taunter:** Harbinger `SenseActor(foe, taunter)` (ABI 18, "does this actor
  sense that one"), so a foe that cannot see or hear the taunt is not affected. Its own ray, not the
  engine detection gate.
- A foe already targeting the taunter is still pinned. The pin holds it for the duration, and that
  is the point of a tank.

### 4.3 The roll (level coin toss)

For each eligible foe, after the idle is confirmed:
`p = clamp(0.5 + 0.05 x (L_taunter - L_foe), 0.10, 0.90)`. Equal levels give 50%. Ten levels above
gives 90%. Ten below gives 10%. The numbers are proposals (Q3). Every roll is logged with p and
the result.

### 4.4 Duration and pacing

- **Does a taunt need a duration? Yes.** The pin is a source deny with no time limit (FACETS ch.20
  "Standing, no time limit") and a held engagement. Without an end the foe stays on the taunter for
  the rest of the fight, and that is no longer a taunt. **15 s** (`fTauntDuration`), per marth.
- **No cooldown.** marth's "no waiting" ruling was about engage-on-sight, but nothing here needs a
  wait either. The **animation paces the taunt**: one attempt is one idle play, and the next attempt
  cannot start until that idle has ended (`IsClaimLive` false on the ch.12 handle). A failed roll
  therefore re-rolls only on the next animated taunt, not seven times a second. A foe already
  pinned by a live taunt is not eligible again (its pin is refreshed only by a new success).
- A second follower taunting the same foe REPOINTS MFO's existing pin on that foe (one MFO claim per
  foe, keyed by foe) instead of stacking claims.

### 4.5 How the taunt ends

MFO releases the foe's pin (and the 2.2 helper, if used) when the first of these happens:

1. **15 s** have passed since the pin was granted.
2. The **taunter is downed or dead:** `IsBleedingOut`, essential-down, or dead. Release in that same
   tick. This is mandatory, because otherwise the hold's bleed-out drive (section 5) walks the foe to
   the downed taunter.
3. The **foe dies**, is disabled or unloads. Harbinger ends the pin itself and logs `pin ended:
   <reason>`; MFO sees `IsClaimLive` false.
4. **Combat ends** for the taunter (no combat controller), or the follower leaves the party, or a
   load happens (Harbinger ends every claim at a load).
5. Harbinger ends the hold early (no path progress, or arrived but not fighting; section 5). MFO
   treats this as the taunt ending, logs it, and does not re-pin.

**Hand-back:** release only. MFO writes no target. The engine's selector answers on the next
update. Because the taunter is now the controller's current target, it gets the +100 stickiness,
plus +1000 if it hit the foe in the last 2 s. If the taunter is close and keeps fighting, the foe
often stays on it naturally. If the selector cache still holds the old pick, the foe may snap back
straight away (1.2, INFERRED). Either is the engine's choice, which is the point.

## 5. Interaction with the engagement hold (APMF `52d55c4`+, INVARIANTS #0 (i))

A ch.20 pin is a held engagement (`TargetPin.cpp:862`), so pinning the foe makes the **foe** the
holder and the **taunter** the held target. From the hold header (`CombatEntry.cpp:518-600`):

| Hold behaviour | Effect on a taunt | Verdict |
|---|---|---|
| Knowledge seats un-lose the held target in the holder's group and refresh its last-known spot | the foe cannot lose the taunter during the 15 s | wanted |
| Ends on NO PATH PROGRESS / ARRIVED BUT NOT FIGHTING | the taunt can end early when the foe cannot reach the taunter | acceptable: MFO logs it as an early end |
| NOT-ENGAGED drop with `StopCombat` | only for **ch.21 with `fval > 0`**. The taunt never sets fval (pin only, and the helper at fval 0) | never fires on the foe. **Verify in S1** that `DropNotEngaged` cannot reach a pin-only holder |
| Bleed-out DRIVE (internal ch.24 bound) | would walk the foe to a downed taunter | prevented by 4.5 item 2. S1 decides whether a non-party holder should skip it |
| SEARCH ELSEWHERE drive | can steer the foe toward the taunter while its group searches for someone else | wanted for a taunt, but it is Harbinger moving a HOSTILE NPC for the first time. S1 must look at it |
| Taunter's OWN engagement | the taunter (a follower) may hold its own engagement on the foe. Its NOT-ENGAGED drop is blocked while the foe is actively hostile (its target is the actor), R-1 | consistent: a taunted foe is by definition hostile to the party |

Nothing in the hold reads "is the holder a client's follower". Every pin until now was on a
follower, so a hostile holder is new ground. That is why S1 exists.

## 6. Passive probes first (principle 5)

Both are config-gated logging in MFO, rate-limited, and change nothing (no claim, no write). INI
`[Taunt] bTauntProbe=0` by default; on for the probe run.

**P1: can the taunt idle play in combat right now?** Every 5 s per follower in combat, evaluate
`TESIdleForm::conditions.IsTrue(follower, combatTarget)` for 0x0B8C73 / 0x0B8C71 and each parent
(walk `parentIdle`, `TESIdleForm` +0x38), without playing anything:

```
[taunt-probe] 0x<follower> idle <B8C73|B8C71> in combat (target 0x<foe>, dist <d>u, pathing <0|1>, R=<type> L=<type>): root=<PASS|FAIL> standard=<PASS|FAIL> shieldBranch=<PASS|FAIL|n/a> self=<PASS|FAIL> -> WOULD PLAY | WOULD NOT (first failing: <record>)
```

This answers what share of combat time a follower could taunt at all, and which condition blocks it
most (pathing, the 250 u distance, the loadout). Whether PlayIdle checks the parent chain is shown
by the first gated play in S2 (`ANIMATION CONFIRMED` while P1 says `standard=FAIL`, or not).

**P2: would a pin on the foe be pinnable, and how stable is the foe's target?** Every 2 s, for each
foe in the follower's combat group within 2048 u whose combat target is in the party, read the
foe's controller and group under the group read lock (the same read `TargetPin.cpp` does):

```
[taunt-probe] foe 0x<foe> (L<lvl>) targets 0x<cur> (party=<1|0>); follower 0x<f> (L<lvl>) in foe's group targets: <HELD|LOST|ABSENT> detect=<n> attackers=<n>; senses follower: <yes|no|unknown>
[taunt-probe] foe 0x<foe> target switch 0x<old> -> 0x<new> after <ms> ms
```

HELD most of the time means the pin alone works. ABSENT often means the ch.21 helper is required.
The switch line gives the baseline flip rate, so the field run can tell a held taunt from the
engine's own stickiness.

Note: P2 reads a hostile actor's `combatController` off the main thread's view. It must run where
MFO already reads combat groups (the evaluator's group snapshot), never from a new thread
(INVARIANTS #4).

## 7. Build slices

| Slice | Repo | What | Tier | Author / review |
|---|---|---|---|---|
| S0 | MFO | P1 + P2 passive probes, `[Taunt] bTauntProbe` | B (logging inside existing mechanisms, reads a foe's group under its lock) | Sonnet 5.5 / Opus one pass |
| S1 | APMF | Audit ch.20 + the hold for a **non-party holder**: confirm no teammate assumption (seat, census, menu, sight service, `DropNotEngaged`, the drives). Decide whether to skip the bleed-out / search drives for a holder that is not a party member. FACETS ch.20 gets a "pin a hostile NPC" example. If the audit finds no code change, it is a tier-C docs round | A if any hold/seat code changes, else C | Opus 5.5 / Opus tier 3 (or a mechanical check if docs-only) |
| S2 | MFO | `cond.foe_attacking_ally`, `act.taunt`, board and i18n strings. The executor: ch.1 hold + ch.12 idle by loadout, then on `ANIMATION CONFIRMED` the area pick (4.2), the roll (4.3), the ch.20 pin foe -> taunter (plus the ch.21 helper per P2), the 15 s timer and the early ends (4.5). One MFO pin per foe, repointed by a second taunter. INI `fTauntDuration=15`, `fTauntRadius=1024`, `iTauntMaxFoes=3`. A new mechanism goes in a new file in its subsystem (proposed `native/apmf/Taunt.cpp` for the bridge calls; the brief names the exact files) | B (uses shipped Harbinger facets, no new seat) | Sonnet 5.5 / Opus one pass |
| S3 | field | LoreRim: P1/P2 run first, then S2 with the gate on | n/a | coordinator gathers, Opus concludes |

Order: S0 and S1 can run in parallel, then S2, which depends on P1 for the idle choice and on P2
for whether the ch.21 helper is needed. S2's acceptance lines are `[ch.12] ... ANIMATION CONFIRMED`
on the taunter, then `[ch.20] FIRST SOURCE DENY` for the FOE's form id with no `SOURCE SEAT
MISSED`, then MFO's own `[taunt] released 0x<foe> (<reason>) after <s> s`.

## 8. Open questions for marth

1. **Area or single target?** Proposed: the selected foe plus up to 3 within 1024 u that sense the
   taunter. Or should it be the selected foe only?
2. **Archers, staff users and casters:** the vanilla humanoid combat idles need a melee weapon in
   the right hand. Should they taunt with another vanilla idle (named by you), switch to melee for
   the taunt, or not get Taunt at all?
3. **Roll numbers:** is 50% at equal level, plus or minus 5% per level, clamped 10 to 90%, right?
4. **Duration:** 15 s fixed, or an MCM/INI value (default 15)?

## Decisions (marth, 2026-10-12)
- **Q1 targets:** the chosen foe plus up to 3 nearby foes (about 1024 u, in combat with the party, can sense the taunter). Each rolls separately.
- **Q2 ranged / staff / casters:** Taunt is a MELEE-ONLY gambit action (offered only while a melee weapon is in the right hand), so it always plays an animation. Animations are non-negotiable.
- **Q3 roll:** p = clamp(0.5 + 0.05 x (taunter level - foe level), 0.1, 0.9), as proposed.
- **Q4 duration:** 15 s by default, adjustable with an MCM slider.
- Ranking vs other target mods is the user's Harbinger slider configuration, not a design question; the enemy pin is a normal ranked ch.20 claim. Nothing escapes Harbinger.
