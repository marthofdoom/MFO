# IAUS card I6: the full combat mode (2026-11-10)

Branch `feat/iaus-i6-full`, base `b35b0a3` (main). Card row: `iaus-design.md` section 9, I6. Design: sections 1.1 to 1.5,
4.5. Tier A (threading, a new scan source). The IAUS idea for MFO is LandingCrew's.

IAUS runs only for a follower whose FLWR bit 3 (`FollowerState::iausCombat`, card I5) is set. With the bit clear the
scan is the table scan. No co-save change.

## What changed

| Piece | Where | What |
|---|---|---|
| IAUS source | `native/scheduler/IausSource.h/.cpp` (new) | the second `ChoiceSource`. Gathers and decides in its constructor, logs in its destructor |
| Tick branch | `native/scheduler/CombatScan.cpp` | picks the source at the seam, the toggle edge, the sweep, combat end, the retreat self heal, the IAUS reconciliation reads |
| Seam | `native/scheduler/ChoiceSource.h` | `Reject` carries the scan's reason text (the table source ignores it) |
| Core | `native/iaus/Decide.cpp`, `Core.h` | Q67: a repeat Fired does not re-anchor the window. Two tuning floors for rowless cooldowns |
| Generator | `native/iaus/Generate.cpp`, `Snapshot.h`, `Prune.h` | taunt cooldown, taunt busy, B661 blast rule |
| Mirror | `native/iaus/SpellBook.cpp` | Q63 open gate, B661 payload explosions, the Precision reach probe |
| Gather | `native/iaus/Candidates.cpp` | `Combatant::taunted`, `Snapshot::tauntBusy` |
| Adapter | `native/iaus/Engine.h/.cpp` | `NoteFired(..., startCooldown)`, the source note, log pacing, `Dismiss` |
| Taunt | `native/Taunt.h/.cpp` | `TauntedBy`, `Busy` (reads of the module's own tables) |
| Ward | `native/cast/Archetype.cpp` | the ward shape test uses the value-modifier alias set |
| Precision | `native/PrecisionBridge.h/.cpp` (new) | one read-only query, measured only |
| Reset | `native/Serialization.cpp` | three `Clear()` calls after `StopPump()` |
| Tools | `tools/iaus_harness`, `tools/iaus_sim` | new checks, the sim follows the new behaviour |

## How the source plugs into the scan

`Scan()` is unchanged apart from passing its reason text to `Reject`. The IAUS source hands it this walk:

1. **The posture winner** as an `act.equip_melee`, `act.equip_ranged` or `act.equip_silver` choice. A satisfied equip is
   transparent, so the scan sets `handClaim`, `wantStance`, `equipHeld` and `silverHeld` itself and goes on. Its rank is
   `Iaus::kRankBase`. That is a stable identity that outranks no action decision, so a weapon swap never takes a hand
   from a standing heal.
2. **The action order** from `Decide`, at most `maxTries` (6). A transparent outcome moves to the next candidate.

The commitment window lives inside `Decide` (`Verdict::Hold`). `SuppressionAnchor` always answers "no window", so the
table's positional window never runs for an IAUS follower. A Hold that took no action is reported as the
"suppressed" stop.

### Reconciliation flags

| Table read | IAUS answer |
|---|---|
| `EquipEvaluatedAboveStop`, `SilverEvaluatedAboveStop` | the posture layer decided this lap (a fresh book and a readable foe). They are only asked when the scan left `equipHeld` or `silverHeld` clear, so "decided, and the order did not hold" is "known false" |
| `EquipRangeUndecidable`, `UnholySilverEvaluated` | the posture layer had NO answer (no fresh book, or no readable foe). The truth is unknown and the holds are kept, the MFO-B55 shape |
| `castSeen`, `matchedCast` | set by the scan for every cast it offers. `AmendScan` adds the RUNNING cast decision while its candidate still clears the floor (design 1.3) |

A staff posture has no equip road (`EquipWeapon`'s ranged category is bow and crossbow). When the staff wins and is
not drawn, no equip order is issued and one `[iaus]` line says so. The reconciliation then releases any melee or bow
hold after the dwell, so his own AI may draw the staff. A staff equip road is its own card.

### Targets

Every cast names its target with a live handle. A target that no longer resolves is passed over in `Next` and never
reaches `Fire`. Deviation from design 1.4: the design says "subject = Player". Here a HOSTILE cast carries subject Self
(AUTO) and a beneficial one subject Player. The handle always wins in `Fire`'s ladder, so the subject only decides
where a handle that dies between `Next` and `Fire` falls. With Player a hostile spell would fall to the player.
With AUTO it falls to the spell's nature, which is enemies. A Self-delivered spell goes out as `act.cast_self`.

### An attack already in effect

`act.attack` answers "already on that target" with an opaque NoOp, not a Fired. For IAUS that still means this
decision is the one in effect. `Record` reads the latch (`Targeting::Current`), not the reason text, and adopts the
decision once (`NoteFired` without a cooldown stamp). Without this the Mind would keep an older decision as
"running", with its momentum and its cast loan.

### Ranks inside one lap

Every non-running candidate of one lap carries the same rank (`RankOf(proposedEpoch)`), as `Decide` was designed. The
hand lock compares ranks strictly, so of two same-lap casts the one that took the hand first keeps it until it fires
or its lock lapses. Then the next decision gets a new epoch and outranks it. This is a short hysteresis, not a
deadlock. It is least-sure spot 2 below.

## The MUST list

1. **ResetAllState.** `Iaus::Engine::Clear()`, `Iaus::SpellBook::Clear()`, `Iaus::Candidates::Clear()` right after
   `SelfStatus::Clear()`, which is after `StopPump()`. Nothing else moved.
2. **Worker-only calls.** No IAUS call is made from main or Papyrus. Dismissal: `IausSweep()` runs on the worker once
   per pump (it acts every 32nd pump, about 4 s) and forgets every noted follower who left `g_activeIds` or lost his
   record. The toggle: the board (I7) only writes the `iausCombat` bool. The edge is seen in his own service on the
   worker (`IausToggleEdge`). Combat end: `IausCombatEnded` in the debounced party-OOC teardown.
3. **Candidate cycle.** `Build`, `FillCooldowns`, `Decide`, `DecidePosture`, `NoteDecision`, in the source's
   constructor. The `Set` is a member of the source and is never moved. `[iaus-gen]` prints `Describe` every 10 s.
4. **B658.** `SpellBook::Invalidate` after every Fired buff, ward, summon, drink and scroll, and after a weapon swap.
5. **B661.** `Prune::BlastUnits(area, explosion, payload)`. An explosion counts on its own only with a payload
   (`BGSExplosion data.damage > 0` at 0x34, or `formEnchanting`, both read in the fork). A cosmetic one can only enlarge
   an EFIT area the record already states. The area probe line names a cosmetic explosion.
6. **Taunt (R1).** The Taunt module already keeps one record per pinned foe (`g_active`, keyed by foe, with the
   taunter). It is filed by the roll and dropped by `Taunt::Tick` on the taunt's real ends: the duration
   (`fTauntDuration`), the taunter down, the foe dead, combat over, Harbinger ending the pin. So the expiry is the
   module's own and is never a second guessed timer (principle 9). `Taunt::TauntedBy(foe, taunter)` fills
   `Combatant::taunted`. `Taunt::Busy(taunter)` (an attempt in flight, or a foe still pinned to him, the two tests
   `Taunt::Fire` makes) suppresses the candidate. Hard cooldown: the class row's, else
   `Tuning::rowlessTauntCooldownSec` (10 s). **No taunt row declares a cooldown today** (`ClassTable.cpp`, all 0), so
   the floor is what applies. See "Outside the file boundary".
7. **Q67.** `OnFired` anchors `firedAt` only when a NEW decision fires. Closes MFO-B651.
8. **Q63.** `GateFor` returns an open gate when the tree is off: spells on class, power attack, taunt, retreat, every
   weapon. The rowless power attack gets `Tuning::rowlessPowerCooldownSec` (10 s, the value every class row
   declares). Tree on: the I4 behaviour. Closes MFO-B657.
9. **Precision reach.** STOPPED at the scoring step, with evidence. See the next section.
10. **Ward.** See "The ward fix".
11. **B667.** See "Ranks across sources".
12. **Toggle edge.** `IausToggleEdge` on either direction, and on the MFO master switch. It releases the spell in
    hand, the offense claim, the heal hold records and the heal claim, the `[cfc]` watch, the hand locks, the
    cast-control latch, both approach claims, the target latch and its proposal, the suppression window, the Mind and
    the gather's per-fight state. Every call is the existing idempotent release. The forced weapon is NOT stripped,
    because a mid-fight unequip is visible. The new source's reconciliation keeps it or releases it through the dwell.
    A retreat in flight and a taunt in flight keep their own lifecycles.

## Precision reach (Q64): evidence, and why scoring did not change

marth: "Almost every list runs precision, the correct answer is within precision reach".

**What was verified**

- Precision 2.0.4 (Nexus 72347). `Precision.dll` sha256 `b4ab4a5a3673324b222b59beed1378abe7982f85af5248d2a7225bc46d0c4f32`,
  the same file in LoreRim, Tuxborn and the custom list. License GPL-3.0-or-later with a modding exception, so no
  code was copied. The bridge declares nothing of Precision's and calls one vtable slot by index.
- Published header `src/PrecisionAPI.h` (branch main): `IVPrecision1` has four Add and four Remove callback
  registrations, then `virtual float GetAttackCollisionCapsuleLength(RE::ActorHandle, RequestedAttackCollisionType =
  Default) const noexcept`. Enum: Default 0, Current 1, RightWeapon 2, LeftWeapon 3. Export
  `RequestPluginAPI(InterfaceVersion)`, V1 to V4 are 0 to 3.
- Binary: the export at `+0x1170f0` accepts 0 to 3 and returns the singleton at `+0x1b6890`, vtable `+0x1867a0`. Slot 8
  is `+0x117fe0`: `movzx edi, r8b` and `mov ebx, edx`, then a call to `PrecisionHandler::GetAttackCollisionReach`
  (`+0x122ea0`, named by its lambda RTTI). So the ABI is `float(this, handle by value in edx, uint8 type in r8b)`.
- `GetAttackCollisionReach`: handle 0 returns 0. It resolves the actor. With Precision's own disable flag set, or a
  computed length of 0, it returns the ENGINE's reach function (Address Library 37588 / 38538, relocation at
  `+0x6eac0`). Otherwise it returns the largest active collision, or computes the length from the actor's WEAPON or
  SHIELD node. It walks the actor's 3D, so MFO calls it on the main thread only.
- Precision's own AI reach is NOT this value. Two engine-call hooks (`+0x108e90`, `+0x109080`), gated by
  `bHookAIWeaponReach`, compute `(length + fAIWeaponReachOffset) x k + r`. That sum is not exposed by the API.

**Why scoring still reads fMeleeReach**

The API gives the weapon's collision LENGTH from the hand. IAUS needs a centre-to-centre reach, because
`Facts::dist` is the distance between the two actors and the range gate vetoes above 1.15 times the reach. Turning a
length into that reach needs the arm extent and the foe's body radius, or Precision's private offset. None of them is
a proven read. A reach that is too short vetoes every melee attack, and an IAUS melee follower would idle. So no
formula was invented.

**What ships**

- `PrecisionBridge::RightWeaponLength` (runtime detection by module handle, interface V2, a self-check that the vtable
  and slot 8 lie inside Precision's image, refusal with one WARN otherwise).
- `SpellBook::Measure` calls it on the main thread every refresh (5 s) for IAUS followers only, stores it in
  `Kit::precisionWeaponLength`, and logs one `[spellbook] reach probe:` line per follower and weapon, once without and
  once with a combat target: the capsule length, `fMeleeReach`, and the distance to his target.
- The `[iaus-gen]` line prints `precision len` beside `reach`.

**To finish Q64** one field run gives the numbers (length against the distance at which swings land). Then a ruling on
the formula, for example length plus a measured arm and body term, and one line in `Candidates.cpp`
(`s.meleeReach`).

## The ward fix (sim review F2) and its blast radius

`cast/Archetype.cpp` read a ward as `kValueModifier` on `kWardPower` only. Real wards are `kAccumulateMagnitude` (32).
The test now uses the alias set the engine-row table already uses (`IsVmAlias`: Absorb, DualValueModifier,
AccumulateMagnitude, PeakValueModifier).

Readers, table mode included:

- `EngineRow::Ward` is unchanged. `LookupAv` already matched these effects through the same alias, so `BuffRoad.cpp`
  (`:276`, `:380`) sees the same row as before.
- `SpellShape::Ward` has ONE functional reader, `iaus/SpellBook.cpp` (`in.ward`), which is IAUS only.
- A ward used to come out as `Plain`. No code compares a shape with `Plain`.
- `Ward` sits before `Light` in the precedence. A spell with both a ward effect and a Light effect now reads Ward.
  `BuffRoad.cpp:292` tests `anyLight || shape == Light || row == Light`, and `anyLight` is its own archetype walk, so
  its answer does not change.
- Log text: the archetype census prints "Ward" where it printed "Plain".

So the table-mode change is the shape name in log lines. Under IAUS wards become `CastWard` candidates
(`act.cast_self`) instead of buffs.

## Ranks across sources (MFO-B667)

`g_firingRule = rank` feeds the hand lock. A table rank (0 to 254) is below every IAUS rank (1000 and up), so a table
rank always outranks. The comparison is left as it is, for these reasons:

1. One follower runs ONE combat source. The toggle edge releases every rank-carrying lock, so a table rank and an IAUS
   rank never meet across it.
2. The logistics walk is table-only and its rank is its own row index. That already mixes with combat rows today.
   It runs at the combat scan's no-action exits, and never while a cast candidate holds the facet (`castSeen`).
3. The one new meeting was a logistics cast against a standing IAUS heal. Every logistics row would displace the
   heal, which breaks heal-to-full. So under IAUS a standing heal also keeps the cast facet
   (`castSeen || (iaus && healHeld)`). Table followers are unchanged.
4. A lock a logistics cast left behind cannot be preempted by IAUS. It ends on the first lap with no cast candidate
   (`ClearCastLock`) or by its own staleness, as for a combat row below a logistics row today.

## Retreat self heal

The table's retreat walk is by name a table walk. An IAUS follower runs no row, so he gets `IausRetreatSelfHeal`: his
best self heal by score, under the same three gates and the same road test (never a claim road).

## Sim before and after

`tools/iaus_sim/run.sh`, seed 1, 8 fights per cell. Min / average / max over the five brackets. Before = main
`b35b0a3`. After = this branch. The sim now keeps the cooldown store (`FillCooldowns`), shows `taunted` and the busy
rule, starts no cooldown on an approach leg, classifies wards with the native test, and gives a mage a ward when the
list has one.

| List | Class | taunt / min | holdL % | heal.s |
|---|---|---|---|---|
| builtin | knight-T2 | 12.2 / 14.8 / 18.2 to 5.0 / 6.0 / 7.9 | 7.3 / 15.2 / 33.2 to 0.0 / 8.6 / 21.7 | 0.5 / 0.8 / 1.4 to 0.3 / 0.8 / 1.1 |
| builtin | melee | 0 | 0.0 / 19.2 / 31.8 to 0.0 / 6.9 / 13.8 | 0.5 / 0.7 / 1.1 to 0.5 / 0.7 / 0.9 |
| builtin | mage | 0 | 0.0 / 11.3 / 24.8 to 0.0 / 2.5 / 5.2 | 0.1 / 0.6 / 1.0 to 0.6 / 0.7 / 0.8 |
| LoreRim | knight-T2 | 8.3 / 12.3 / 15.0 to 4.5 / 5.4 / 7.0 | 20.9 / 37.7 / 55.6 to 1.4 / 6.0 / 19.6 | 0.5 / 1.9 / 5.8 to 0.4 / 1.2 / 3.0 |
| LoreRim | melee-off | 5.1 / 8.6 / 11.4 to 3.8 / 4.3 / 4.9 | 13.7 / 20.3 / 29.1 to 1.9 / 4.7 / 8.4 | 0.4 / 1.0 / 1.9 to 0.5 / 0.9 / 1.1 |
| LoreRim | melee | 0 | 37.4 / 44.9 / 61.5 to 2.0 / 4.4 / 8.5 | 0.5 / 1.1 / 2.0 to 0.5 / 0.9 / 1.4 |
| LoreRim | ranged | 0 | 0.0 / 24.4 / 36.8 to 0.0 / 2.9 / 5.3 | 0.4 / 0.9 / 2.0 to 0.4 / 0.8 / 1.1 |
| LoreRim | mage | 0 | 7.1 / 11.3 / 16.1 to 0.0 / 0.6 / 1.6 | 0.6 / 0.9 / 1.2 to 0.7 / 0.9 / 1.1 |
| Tuxborn | knight-T2 | 13.2 / 17.1 / 20.1 to 6.1 / 7.3 / 11.1 | 13.2 / 22.5 / 33.5 to 10.3 / 13.3 / 19.0 | 0.3 / 0.7 / 1.2 to 0.4 / 0.7 / 1.1 |
| Tuxborn | melee-off | 4.0 / 6.8 / 10.5 to 3.2 / 3.8 / 5.1 | 0.0 / 9.4 / 16.9 to 0.0 / 5.0 / 12.8 | 0.5 / 0.6 / 0.8 to 0.6 / 0.8 / 1.3 |
| Tuxborn | melee | 0 | 27.5 / 29.9 / 32.8 to 5.9 / 12.1 / 18.0 | 0.1 / 0.6 / 0.9 to 0.4 / 0.5 / 0.8 |
| Tuxborn | ranged | 0 | 15.7 / 25.5 / 36.3 to 0.0 / 8.3 / 18.6 | 0.6 / 1.0 / 1.4 to 0.6 / 1.0 / 1.8 |
| Tuxborn | mage | 0 | 8.3 / 12.6 / 17.5 to 0.5 / 4.0 / 8.4 | 0.4 / 0.7 / 0.9 to 0.5 / 0.8 / 1.1 |

- **Taunt.** Knight's taunt share of fired actions: LoreRim 14 to 24% became 8 to 12%. Tuxborn 22 to 34% became 10 to
  17%. The sim's taunt lasts 10 s with a 70% success, so its rate is still above the game's. In game a hit blocks the
  next taunt for `fTauntDuration` (15 s).
- **Ward candidates.** Spell pool, ward nature: LoreRim 0 became 6, Tuxborn 0 became 5 (buff 170 became 164, and 158
  became 153). `cast_ward` share of a mage's fired actions: 0 before, 0 to 3% after (LoreRim mage 0 to 2, mage-off 0
  to 3, Tuxborn mage 0 to 2).
- `BAD-FACTS` 0 and invalid picks 0 in every run. The committed `baseline_builtin.txt` is the new run.
- Not changed by this card: the mage attack share (I8) and the absolute win rates.

## Outside the file boundary, not touched

- **`bIausExplain`.** The `[iaus-why]` lines need a Config key. Config is outside this card, so the source passes
  `false`. One INI key (tier C) would turn them on for the first field run.
- **Taunt row cooldowns.** Every taunt row in `classes/ClassTable.cpp` has `cooldownSec` 0. IAUS reads the row's value
  first, so setting one there changes IAUS at once. It would also change table mode through C12 and G9.
- **A staff equip road** (`act.equip_staff` or a staff branch in `EquipWeapon`).
- **A writer for `iausCombat`.** No code sets the bit (card I7's board toggle). Until then IAUS cannot be turned on
  in game, and every follower runs his table.
- **`Docs/REVIEW-BACKLOG.md`.** This card closes MFO-B651 (Q67), B657 (Q63), B658, B661 and B667, and does the
  marshalling B639 asked for. The entries are not edited here.
- **`Docs/research/2026-11/iaus-sim.md`.** Its result tables are the pre-I6 runs. The table above is the update.

## In-game test list: the first IAUS run (Melee, Ranged and Mage T1)

Setup: three followers, one per default class, `iausCombat` set on each. **Nothing writes that bit yet.** The only
writer is the FLWR loader, and the board toggle is card I7. So this run needs I7 (or a dev-only setter) first. Tree
OFF for the first pass, then tree ON.

**A. Off means off**

1. With the bit clear on all three, fight one group. `MFO.log` has no `[iaus]`, `[iaus-gen]`, `[spellbook]` or
   `[precision]` line, and the `[eval]` lines read as on main.

**B. The source runs**

2. Bit set, enter combat. Within about a second: one `[spellbook] <name>: spells offense=..` line per follower, then
   `[iaus-gen]` every 10 s and an `[iaus]` line on every change of pick. No `[eval] combat table: N rules, none
   matched` line for them.
3. Melee T1: `[iaus] pick=attack -> <foe>` and he fights that foe. The line must not flip between foes every second
   (momentum and the window). `[eval] ... held off: already on that target` appears once, not per tick.
4. Ranged T1 with a bow and a sword: `[iaus-gen] ... posture bow` at range. Walk a foe into him: the posture turns to
   melee after the dwell, with ONE swap, and back when the foe leaves. No weapon vanishing.
5. Mage T1: casts are animated, at the foe the `[iaus]` line names. No cast at the player or an ally with a hostile
   spell. Known for this card, tuned in I8: a T1 mage who carries a dagger draws it when a foe is close.
6. Idle: when nothing clears the floor the line reads `idle: nothing above the floor` and the follower fights on with
   his own AI. He must not stand still.

**C. The I6 rules**

7. Q67: hurt a follower who has a heal while he keeps attacking. The heal comes within about a second, not after a
   long hold. `[iaus] hold=` lines stay rare.
8. Heal hand: while a heal runs on one hand, no other cast takes that hand until the recipient is full.
9. Taunt (tree OFF, a melee follower, `bTaunt=1`): he taunts a foe that is on the player or an ally. No second taunt
   while `[taunt] ... pinned for 15s` is live (`[iaus-gen] ... taunt busy=1`). After a MISS the next taunt is at
   least 10 s later.
10. Power attack (tree OFF): at most one swing per 10 s. The closing leg does not start the cooldown.
11. Ward: a mage who knows a ward, against a caster. `[spellbook]` counts `ward=1` and a `cast_ward` pick appears.
12. B661: a mage with a bolt spell while a melee follower stands on the target. He still casts it. The
    `[spellbook] area probe:` line for that spell says `cosmetic explosion: no area`. With Fireball he must NOT cast
    into the melee.
13. B658: after he summons, the summon is not a candidate again while it lives. After a drink the potion count in the
    next `[spellbook]` line is one lower.
14. Player target probe: `[iaus-probe] player currentCombatTarget: set on N of M`. In a real fight N must be above 0.
    If it stays 0, report it: the player-target term has no input.
15. Precision: `[precision] bridge live` once, and `[spellbook] reach probe:` lines with a capsule length above 0 for
    a follower holding a weapon. Note the length and the distance for Q64. Without Precision the line says the DLL is
    not loaded.

**D. Edges**

16. Toggle the bit off in a fight (and on again). One `[iaus] ... combat source: IAUS -> gambit table` line. The
    spell leaves his hand, no cast keeps running, the weapon stays, and the table takes over on the next lap.
17. Dismiss an IAUS follower mid-fight, then recruit him again. No crash, and his first `[iaus]` line starts clean.
18. Save and load mid-fight. No `[iaus]` or `[spellbook]` line from before the load, a fresh `[spellbook]` line after.
19. Retreat: an IAUS follower who falls back heals himself (`[retreat-heal] ... IAUS self-heal`) and nobody else.
20. Knock him down or paralyse him: `[iaus] ... walk=0 (1: ... 'cannot act: ...')` once, no cast, and his weapon and
    spell stay.
21. Budget: `[iaus]` lines end with the decision time. With 8 foes and about 30 spells it stays under 0.5 ms. No
    `over fIausBudgetMs` warning.

**E. The I4 checks** (`iaus-i4-spellbook.md`, now reachable)

22. The rune is counted under not-candidates and gets one `no IAUS candidate: target location` line.
23. Give him a potion mid-fight, then take it back: the count follows within one lap.
24. Teach him a spell by console mid-fight: it appears within 5 s.
25. A staff, charged and empty: the line says `(charged)` or `(EMPTY)`. This read is unobserved (MFO-B656).
26. A buff already on him (Oakflesh): no buff candidate for him while it runs.
27. A melee-only follower against a flying dragon: no attack names the dragon while it is airborne out of reach.
28. Tavern brawl: no candidate names the brawl partner.
29. Tree ON, a Warrior with a known heal and no mastered mage class: `dropped class` above 0 and no cast. Master a
    mage class: the heal appears and scores low until someone is badly hurt. Tree OFF: the same Warrior casts it.
30. VR, if anyone has it: no book, attacks only, no crash.
