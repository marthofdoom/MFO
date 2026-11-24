# Animation residue plan: every cast that still goes out without its animation

2026-11-24. Planning only, no code. Read against MFO origin/main `143324ad` and Harbinger origin/main `dae3cac` (ABI 46).
marth: "Lets get the spell animations for every spell going soon." Rulings held: animations for all actions, fix in
Harbinger not MFO, combat as a substrate, no masking, composition not substitution, a heal out of combat never starts a
real fight or draws crime.

## 1. What the logs say

Every line is `[cast] <id>: UNANIMATED road '<road>' for '<spell>' -- <reason>` (`native/cast/Unanimated.cpp:54`).
The helper only speaks on the COMBAT TABLE (`Unanimated.cpp:38`, `g_firingRule == kNoRule` returns). The out-of-combat
service has two other lines that also mean "unanimated" and must be in the same census:
`[substrate] ... stays on today's direct road (unanimated) -- <why>` (`cast/SubstrateRoad.cpp:86`) and
`[heal] ... out-of-combat heal -- INSTANT road` (`ComposedCast.cpp:904`).

Counts over all `_evidence/*/MFO.log*` (150 raw lines. Log rotations and overlapping pulls repeat some, so the brief's
deduplicated 74 is the same picture). The three newest runs (1119-jesper, 1123, g0b-1124) show only rows R1 and R2.

| # | Reason text (short) | Raw | Newest 3 runs | Spells | Decided at |
|---|---|---|---|---|---|
| R1 | ambiguous row, a candidate Harbinger does not bound | 47 | 17 | Mystic Wind (Script + Restore, Self, FF, Buff) | `cast/BuffRoad.cpp:347` (`BuffAmbiguousRowWhy`), returned at `:465`, logged from `cast/DirectSelf.cpp:217` |
| R2 | no combat controller, heal | 49 (other 30, self 19) | 9 (Greater Healing 7, Close Wounds 2) | Healing, Greater Healing, Close Wounds, Fast Healing, Rejuvenating Touch, Healing Spring, Vitalisation | `ComposedCast.cpp:855` (`ChooseHealRoad` DirectNoCombat), logged from `cast/DirectTarget.cpp:271` and `DirectSelf.cpp:217` |
| R3 | no combat controller, buff | 41 | 0 | Flame / Frost / Lightning Cloak 31, Circle of Strength 3, Candlelight 3, Oak Heart 4 (auto-fan) | `cast/BuffRoad.cpp:502` (DirectNoCombat), logged from `DirectSelf.cpp:217`, `cast/Auto.cpp:1048` |
| R4 | offense shape with no claim road (Self delivery) | 13 | 0 | Frost Nova | `cast/Unanimated.cpp:128`, logged from `DirectSelf.cpp:217` |
| O1 | out of combat, ambiguous row (same test as R1) | 4 | 0 | older runs | `cast/SubstrateRoad.cpp:182` via `ChooseBuffRoad` |
| O2 | out of combat heal, INSTANT road | 54 | 0 | older runs, before the substrate served heals | `ComposedCast.cpp:896` |

Reasons that exist in code and have zero field lines. Each still needs a road or an explicit owner.

| # | Reason | Site |
|---|---|---|
| L1 | row belongs to another road (a Buff kind whose single row is Restore / Offensive / hostile row) | `BuffRoad.cpp:491` |
| L2 | aimed or location Light (Magelight) | `BuffRoad.cpp:469` (`BuffAimedLightWhy`) |
| L3 | self rune shape, and a non-Self concentration self cast | `BuffRoad.cpp:474`, `:477` |
| L4 | heal self-flip fired but never landed (barred) | `ComposedCast.cpp:869` |
| L5 | AUTO fan: Self offense onto each foe, concentration non-heal, instant beneficial | `cast/Auto.cpp:1037`, `:1042`, `:1045` |
| L6 | summon one-shot the claim road declined | `cast/Summon.cpp:463` |
| L7 | legacy-force (AI-first hybrid force on miss) | `cast/CastOn.cpp:2019` |
| L8 | out of combat: concentration (slice S5), attack spell (round R4), target-location heal | `cast/SubstrateRoad.cpp:160`, `:164`, `:171` |
| L9 | degrade: Harbinger absent or too old, switches off, ABI < 19, town hold on old Harbinger | `BuffRoad.cpp:443`, `:495`, `ComposedCast.cpp:832`, `SubstrateRoad.cpp:149` |

L9 is the documented degrade contract and stays (INFO line). Everything else gets a road below.

## 2. Why, and the animated road for each

### 2.1 R2 and R3: "no combat controller" is a party-fight gap, not an out-of-combat gap

The out-of-combat heal already animates through the substrate (zero INSTANT lines in the four newest runs, and
`[substrate] FIRED` lines are present). These lines come from somewhere else. `scheduler/CombatScan.cpp:999` runs the
combat table for a follower whenever the PARTY is in combat, own combat or not ("party combat, own combat=0, combat
table live"). In that state:

- the claim roads need his own controller and answer DirectNoCombat (`BuffRoad.cpp:498`, `ComposedCast.cpp:838`),
- the substrate road is never asked, because `SubstrateRoadFor` returns false off the out-of-combat table
  (`SubstrateRoad.cpp:122`), and would hold anyway while party combat is on (`:196`),
- Harbinger refuses an open while the player fights (`channels/CombatSubstrate.cpp:1097`) and drops an open pair as
  "party-combat" (`PartyPoll`, `:2099`). That is design 3.6a, trigger (c).

So the answer to "switch off, refused, or not asked": NOT ASKED, and it would be refused if asked. Deck run 1123 shows
the three shapes. The line falls 0.5 to 1.1 s before `party combat OFF` (his own fight just ended, 16:42:14, 16:43:00),
about 1 s after `party combat ON` (he has not joined yet, 16:50:05), or right after he stands up from bleedout while
the player is at 27 percent health (16:52:15). The stall probe shows the gap can last tens of seconds when he fails to
join (16:42:24 to 16:43:01).

Road for buffs (R3, L6): hold. A cloak or Circle cast by a follower who is not in the fight has no use. When
`ChooseBuffRoad` passes every test except the controller, the cast is not made this lap and one `[cast-hold]` line says
so. It fires as a claim the moment he has a controller, or through the substrate once the party is out of combat. This
gates the source and masks nothing. Package A1.

Road for heals (R2): a hold is not free, an ally can be dying in that gap. The proper road is to give him a controller
for the length of one cast, which is exactly the substrate, opened while the party fights. Harbinger change (package
H-B): a pair opened with a new "party tolerant" bit is not refused at `:1097` and is skipped by trigger (c) at `:2099`.
Triggers (a) (a real target enters his group) and (b) (a hit from a non-party actor) still turn it real, so the
deny-complete set is untouched and no fight is started by the heal. MFO then sends combat-table casts whose only
failing gate is the controller to the substrate road (package A3). Decision D1 covers what heals do until H-B lands.

### 2.2 R1, O1, L1: ambiguous rows (Mystic Wind)

`ClassifyArchetype` cannot read the engine's effect score, so a spell whose effects map to several rows is `rowApprox`
(`cast/Archetype.cpp:338`). The claim is allowed only when every candidate row is one Harbinger bounds
(`BuffRoad.cpp:341`). The bound lives in Harbinger's seat table, `core/CastSeats.cpp:196`: each caster type has
`Bound::kTarget`, `kCaster` or `kNone`, applied at `:561` by `AlreadyApplied` (Harbinger's own walk of the active
effect list, no engine call). Restore and Offensive are `kNone` on purpose: a heal claim must repeat. Mystic Wind's
second candidate is Restore, so a claim the engine builds as a Restore caster would re-cast every hand cycle (the
55-light spam shape).

Road: make the bound a property of the CLAIM, declared by the client (declare, then enforce). New cast flag
`kCastFlag_FireOnce` (bit 17, the next free bit after `kCastFlag_CorpseTarget` at `APMF_API.h:1567`), ABI 47, plus a
coverage bit in `castSeats` (`APMF_API.h:3313`). On a claim that carries it, seat 0x06 answers NO on ANY caster type
when the spell's effect is already on the claim's target, and after the claim's first observed fire. The second half
covers instant spells that leave nothing to read back. No new engine address, so no new `VerifiedAddresses` row.
Package H-A. MFO adoption (A2): with the capability present, `BuffAmbiguousRowWhy` and the `:491` default no longer
refuse, `BuffClaim` sets the flag. The out-of-combat O1 lines disappear with it because `SubstrateRoadFor` asks the
same chooser.

### 2.3 R4, L5 first case: Self offense blasts (Frost Nova)

This is package M4 of `placed-spells-plan.md` (section 3.3): `BuffClaim` with target 0, probe first, guarded by the M2
area census. Not planned again here. It needs `kCastFlag_FireOnce` semantics too (one blast per claim), so M4 should
land after H-A and use it in place of the "cast pace" it sketches.

### 2.4 The rest

- L5 instant beneficial AUTO fan: a per-recipient claim series with `kCastFlag_FireOnce` (no read-back needed).
  L5 concentration non-heal fan: the existing concentration claim at another recipient (`cast/DirectTarget.cpp`
  Task 1), one recipient at a time. Package A4.
- L8 concentration out of combat (slice S5) and target-location heal: S5 is its own slice, sized below as A6. The
  attack spell out of combat (R4 of the substrate plan) has no caller today and stays refused, not direct.
- L2 Magelight, L3 runes, target-location anything: placed-spells H1 / M5 / H2 / M6.
- L4: the bar is a symptom. With H-A the self-flip heal can carry FireOnce semantics per cast. Re-examine after A2 with
  field data, no package yet.
- L7 legacy-force: only reachable with `bLegacyCastHybrid` on or Harbinger absent. Degrade. Decision D2 decides
  whether any direct cast survives with Harbinger present.

## 3. Packages

Tier A is authored by Opus per the brief (Fable budget), reviewed by Opus tier 3. Sizes are author plus review tokens.

| # | Package | Repo | Tier, author | Size | Needs |
|---|---|---|---|---|---|
| A1 | Party-fight hold for non-heal casts whose only missing gate is the controller. One reader `OwnControllerOnlyGap()` in a new `cast/PartyGap.cpp`, called before the four direct sites. `[cast-hold]` line. | MFO | B, Sonnet | 200k | nothing |
| H-A | `kCastFlag_FireOnce`, ABI 47, coverage bit, claim-declared bound on every caster type (`CastSeats.cpp:561`) | Harbinger + mirrored `APMF_API.h` | A, Opus | 450k | nothing |
| A2 | Adopt FireOnce: ambiguous rows and `:491` rows claim, debug-gambit case, MAP "to add a bounded row" note | MFO | B, Sonnet | 250k | H-A, placed-l1m1 merged |
| H-B | Party tolerant substrate pair (open flag, `:1097`, `PartyPoll :2099`), anchor leak re-checked at the fight edge | Harbinger | A, Opus | 600k to 800k | D1, G2 ch.24 arbiter order (section 5) |
| A3 | Combat-table casts with only the controller missing take the substrate road (heal self and other first, then A1's held buffs if wanted) | MFO | B, Sonnet. The scheduler read of "holder under party combat" is threading-shaped: ask the Opus reviewer for a tier 3 look at that one function | 300k | H-B |
| A4 | AUTO fan residues as claim series | MFO | B, Sonnet | 200k | A2 |
| A6 | Substrate slice S5: concentration out of combat (stream held under a substrate, closed by MFO's stop) | MFO, maybe Harbinger close rule | A if Harbinger changes, else B | 300k to 500k | A3 |
| A5 | Census: `tools/field/anim_census.py` plus the self-test table (section 6). Then the D2 flip. | MFO | B, Sonnet. Flip is C sized | 200k | all above |

Order: A1, then H-A, A2 (kills the largest count, 47 lines), then H-B, A3 (kills the heals), then A4, A6, A5.
A1 and H-A can run in parallel (different repos). One deck session after A2 and one after A3.

### Risks

- 1.5.97 and 1.7.104: H-A adds no engine address. `AlreadyApplied` is Harbinger's own walk through fork members and
  already runs on all three. H-B changes poll logic only, no new seat. Neither has a 1.5.97 list to field test, so both
  ship with the three-runtime self-check green and the reviewer confirming no new row was needed. If either author
  finds it needs a new id, that is a STOP and a re-size.
- H-A: a claim flagged FireOnce on a HEAL would stop repeating. MFO never sets it on Heal kind, and Harbinger logs once
  when the flag meets a Restore caster whose claim came through the heal entry.
- H-A: the "after first fire" half needs Harbinger's per-claim fire observation. The substrate already logs FIRED per
  pair. The brief must make the author locate that counter for ordinary claims before designing (not verified here).
- H-B: this reopens the exact window the party hold closed (`SubstrateRoad.cpp:196`, field 2026-11: an open dropped as
  party-combat delayed his join, and an open at the fight edge leaked the anchor). The package must prove with a
  passive probe that a tolerant pair closes clean with foes still loaded, before MFO adopts it.
- A1: a held buff that never gets a controller is never cast in that fight. That is correct, and the hold line is rate
  limited the same way as the UNANIMATED line.
- Crime: H-B keeps the anchor-only target and the Spectator fix (ABI 32). A3 keeps `FileSubstrateCast`'s sneaking,
  scene, dialogue and occlusion refusals as they are.

## 4. Tests, field steps, what the log must show

Debug gambit steps (dev build, one deploy each for the A2 and A3 stops):

1. Mystic Wind, in his own combat. Expect a `[buff]` claim line, `MRh_/MLh_SpellFire`, one fire, the buff up, no second
   cast in 30 s, `[ch.8b]` NO answers naming FireOnce.
2. Cloak with party combat on and own combat off (stand him away from the fight). Expect `[cast-hold]`, then the claim
   when his controller appears.
3. Heal another and heal self in the same state, and right after a bleedout recovery. After A3: `[substrate] OPEN` with
   the tolerant bit, `FIRED`, close, no new combat target on him, no bounty, the foe list unchanged.
4. Same heal at the fight edge (last foe dies while the heal is wanted). No anchor leak line, no `CLOSED UNFIRED` run.
5. AUTO instant beneficial and AUTO concentration buff (A4).

Pass for a whole session, from `anim_census.py`: zero `UNANIMATED road` lines, zero
`stays on today's direct road (unanimated)` lines, zero `INSTANT road` lines, with Harbinger present. Allowed
remainder after this plan: nothing on these three lines, because runes and bursts are dropped before the cast
(`iaus/Prune.h`) until the placed plan gives them a road. `[cast-hold]` lines are reported with their longest gap per
follower, so a hold that hides a stuck follower is visible.

## 5. Conflicts

- `feat/placed-l1m1` (about to merge) rewrites the row switch of `ChooseBuffRoad` into `BuffRowServed` and adds
  `SummonClaimUnservedWhy`, which calls `BuffAmbiguousRowWhy` (`BuffRoad.cpp` around `:309` to `:372` and `:479`). A2
  edits those same functions, so A2 starts only after it merges, and must update `SummonClaimUnservedWhy` in the same
  change (one reader, principle 12). A1 touches `Fire.cpp` call sites not at all (it sits in the direct entries), so
  it does not collide. `SpellBook` and `Prune` are untouched by this plan.
- M2 area census: no shared files. M4 (self blast) should consume FireOnce, so H-A before M4. Both M2 and A3 read
  main-built state on the worker, keep them in separate rounds.
- H1 point seat in the placed plan also names "ABI 47". Whichever lands second takes 48. With this order H-A is 47,
  H-B 48, H1 49. The coordinator assigns numbers at brief time.
- G2 ch.24 arbiter: it decides who owns a facet while combat is live. H-B opens a substrate during party combat, so it
  must be written against the arbiter's rules, not beside them. Land ch.24 first, or brief H-B with the ch.24 design as
  required reading. H-A does not touch arbitration.

## 6. How this slots with placed-spells M2 to M6, and the final census

After A1 to A6 here and M3, M4 there, the only kinds without a road are location bursts (H1, M5) and runes (H2, M6),
and both are dropped before the cast. The census is one table, kept in one place (`native/selftest`, mirrored by
`tools/field/anim_census.py` for logs), one row per combination. The self-test builds a synthetic spell per row, asks
the road choosers in each of three states, and fails on any Direct answer that is not listed as expected. A new
kind is one new row.

States: own combat (C), party fight without own controller (P), out of combat (O).

| Kind | Delivery | Casting | Recipient | C | P | O |
|---|---|---|---|---|---|---|
| Heal | Self | FF, Conc | self | heal claim | substrate (A3) | substrate (Conc: A6) |
| Heal | Touch, Aimed, TargetActor | FF, Conc | other | heal claim | substrate (A3) | substrate (Conc: A6) |
| Heal | Touch, Aimed, TargetActor | FF, Conc | self | self-flip claim | substrate (A3) | substrate |
| Heal | TargetLocation | any | any | dropped until H2 | dropped | dropped |
| Buff, single bounded row | Self | FF | self | buff claim | hold (A1) | substrate |
| Buff, ambiguous or Restore / Offensive row | Self | FF | self | FireOnce claim (A2) | hold (A1) | substrate (A2) |
| Buff | Touch, Aimed, TargetActor | FF | self | explicit-self claim | hold | substrate |
| Buff | any but Location | FF | other | buff claim, seat 0 flip | hold | substrate |
| Buff | any but Location | Conc | self, other | concentration claim | hold | substrate (A6) |
| Buff, instant, AUTO fan | any | FF | party | FireOnce series (A4) | hold | substrate |
| Light | Self, TargetActor | FF | self, other | buff claim | hold | substrate |
| Light | Aimed, TargetLocation | FF | point | dropped until H1 | dropped | dropped |
| Summon, Reanimate | Self, TargetLocation | FF | none, corpse | buff claim (M1) | hold | substrate |
| Offense | Aimed, Touch, TargetActor | FF, Conc | foe | offense claim | not picked (no own target) | refused (substrate R4) |
| Offense, wall | Aimed | Conc | foe | offense stream (M3) | not picked | refused |
| Offense, self blast | Self | FF | none | BuffClaim target 0 + FireOnce (M4) | hold | refused |
| Offense, location burst | TargetLocation | FF | point | dropped until H1 / M5 | dropped | dropped |
| Rune | TargetLocation | FF | point | dropped until H2 / M6 | dropped | dropped |

"Dropped" and "refused" mean not cast at all, never the direct road.

## 7. Decisions for marth

1. **D1, heals in the party-fight gap until H-B and A3 land.** Option A: keep today's instant heal, still logged, so
   the UNANIMATED heal lines remain until A3. Option B: hold the heal like the buffs, zero lines at once, but an ally
   can wait seconds for a heal (the 16:52 case was the player at 27 percent). Recommendation: A, and build H-B. A
   withheld heal is a worse fault than an unanimated one, and the gap is where heals matter most.
2. **D2, the end state.** Once A5's census is green, should a cast that still finds no animated road with Harbinger
   present be REFUSED (not cast, one WARN line) in place of the direct road? Recommendation: yes. It makes "zero
   unanimated" true by construction and every miss shows as a named refusal. The direct roads stay only as the
   Harbinger-absent degrade.

## 8. Sources read

MFO `native/cast/Unanimated.cpp`, `BuffRoad.cpp`, `DirectSelf.cpp`, `DirectTarget.cpp`, `Auto.cpp`, `Summon.cpp`,
`CastOn.cpp`, `SubstrateRoad.cpp`, `Archetype.cpp`, `native/ComposedCast.cpp`, `scheduler/CombatScan.cpp`. Harbinger
`native/core/CastSeats.cpp`, `native/channels/CombatSubstrate.cpp`, `native/APMF_API.h`. Evidence `_evidence/*/MFO.log*`
(timing from `deck-run-1123`). `Docs/research/2026-11/placed-spells-plan.md`. Diff of `origin/feat/placed-l1m1`.
Not verified here and left to the package briefs: Harbinger's per-claim fire counter for ordinary claims, the current
state of the anchor leak fix, the ch.24 arbiter design text.
