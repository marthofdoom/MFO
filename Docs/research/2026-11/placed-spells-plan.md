# Placed spells plan (location summons, runes, walls, location bursts, self blasts)

Fable 5.2, 2026-11-24. PLANNING ONLY, no code. Branch `docs/placed-spells-plan`.
Tracks: backlog MFO-B865 item 1 and the real-wall note, the two Frost Nova items under `fix/field-1117-cast`,
ClickUp 1dfcuabe2d3, review P1 of `feat/sim-v4-pretune` (`_agentlogs/mfo/review-sim-pretune.md`), review F1 / F2 of
`fix/impact-hazard-shape` (`_agentlogs/mfo/review-bolt-fix.md`).

Tags used below. FACT = read in code or disassembly, the source is named. FIELD = seen in a deck log. PRED = follows
from the code but nobody has watched it run (principle 5). NEW = read for this plan today.

## 0. Status and rulings (2026-11-24, branch `feat/placed-l1m1`)

- **L1 built.** `Prune::IsPlacement` (`iaus/Prune.h`) is the one rule, `SpellBook.cpp StaticOf` fills it. The lab (`Data.h Classify`) now calls
  `Prune::ClassifyNature`, `IsPlacement`, `StaticDrop` and the Magelight rule from extracted record bits (`extract_world.py placed_row`, joined by
  `compile_world.py` as `placed`, `placedSchema` 2). A list without it is refused by the sim and by `tune_driver.py check_world`. Count test:
  `tools/list_balance/book_census.py` (an independent derivation) and the `[spellbook]` lines of deck log 1123. Numbers in section 11.
- **M1 built.** A summon is never a placement drop. No road change. F1 (the field look) is still the proof the claim fires.
- **D2 ruled (marth: "Block them for now") and built.** The AI picks no Self, fire-and-forget, hostile AREA spell until M2 and M4 land
  (`Prune::IsSelfBlastPlacement`, own reason `Drop::SelfBlast`). Gambit roads unchanged. It covers nature Control as well as Offense: Fire Storm and Frost
  Nova carry a fear / paralysis rider, so their nature is Control and the older rule never caught Fire Storm. It also covers a caster-centred hazard whose
  spell is hostile (Blizzard). The Teach Spells page lists such a tome greyed, with the reason.
- **M2 built (2026-11-24, `feat/placed-m2`, not merged, not field run).** `Estimate::AreaCensus` (`iaus/AreaCensus.h`), the main-built table `iaus/AreaActors.*`, `kAreaGuard`.
  A hostile control area is guarded too, and for allies (Fire Storm class); Harmony is not. The interim self-blast block stays until M4. The lab has no bystanders, so it is not gated.
- **M3 built (2026-11-24, `feat/placed-m3`, not merged, not field run).** A wall is no placement drop (`Prune::IsPlacement`); its area is the hazard's footprint and its stream's line is guarded for bystanders (`AreaCensusLine`). Value = the existing unmeasured-offense placeholder: the hazard-dwell value of 3.2 is NOT built (MFO-B871). The M2 leftovers landed with it: crime-faction non-fighters stay bystanders (F1), the MCM option `bAreaSummonsBlock` (F2, default OFF: party summons and the player's horse are not bystanders), the 6000 unit distance cull (F3), harness rows for the surviving mutations (F6). F1's wall look (a watched Wall of Flames cast) is still the field proof and M3 is NOT claimed field-ready before it.
- **M4 built (2026-11-25, `feat/placed-m4`, not merged, not field run).** The interim self-blast block is DELETED for the area arm: a Self, fire-and-forget, hostile area spell (Frost Nova, Fire Storm, Harmony, Mass Paralysis) is an IAUS candidate, guarded by the M2 census centred on him (allies and the player in the blast, bystanders within radius + body + margin), its radius is the hostile effect's own area in units (`BlastOf.hostileUnits`), and its foe must stand inside it. An IAUS pick of it takes the animated buff claim road (target 0, `kCastFlag_FireOnce`, `cast/BuffRoad.cpp`; the claim's fire-once bound is the plan's "cast pace"); with no claim road it is held, never cast unanimated. Gambit rules are unchanged. NOT deleted: the hazard arm (Blizzard, the circles), `Prune::IsSelfHazardBlock`, MFO-B872. The lab still drops every self blast (section 7). The probe the plan asks for before the build does not exist; the F1 look decides whether Harbinger C6 is needed (MFO-B872 1).
- **D1 ruled: APPROVED, NOT BUILT.** A rune-only exception for a perform call at release (package H2, after the tune). It becomes a Harbinger
  INVARIANTS #0 amendment when H2 is built, not before.

## 1. The problem in one paragraph

`iaus/SpellBook.cpp:253` sets `StaticIn::targetLocation` for five different things and `Prune::StaticDrop` drops all
of them as `Drop::Placement`: a spell with Target Location delivery, shape Rune, shape Wall, shape TLBurst, and (interim)
`IsSelfBlastPlacement`. The rule was written for "we do not know where to put it". That reason does not hold for a
summon (the engine places it) and does not hold for a wall (it is an aimed stream). marth's rulings say the end state is
no static drop at all: every spell stays a candidate, every cast is animated, MFO drives nothing in the engine, and no
follower commits a detected crime.

## 2. Engine facts

### 2.1 What an NPC's Target Location cast does

- FACT (Harbinger `Docs/ADDRESS-TABLE-2026-09-15.md` addendum 2026-09-23, both runtimes). The cast core (AE 34410
  `0x5bc160`, SE 33632 `0x54cd10`) switches on delivery. The Target Location arm takes its point from the location
  helper (AE 34447 `0x5bf350`, SE 33667 `0x54fca0`): the CASTER's magic node, else his position. It never reads the
  target the cast was handed (`MagicCaster+0x20`). An effect with `kSnapToNavMesh` (every vanilla summon, no rune) gets
  that point snapped to the navmesh.
- FACT. The player alone gets a crosshair pick (AE 34457 `0x5c0470`, gated at AE `0x5bc3ec` / SE `0x54cf8a`).
- FACT. A Target Location effect WITH a projectile (a rune's Lobber) is launched only when that pick hit static, terrain
  or ground (AE inline `0x5bd04e`, SE 33671 `+0x8d`). An NPC has no pick. The projectile is dropped. The gate reads the
  EFFECT's delivery (`EffectSetting+0xdc`), so a delivery-flipped copy of the spell that shares its effects is caught by
  the same gate (PRED from that read). The proxy idea of 11-17 does not rescue a rune.

### 2.2 Summons

- FACT. For a Target Location spell with a Summon Creature effect the target becomes the caster, and the summon is
  applied only to the actor that cast it. So a marker can never summon.
- NEW (AE 1.6.1170 `SummonCreatureEffect::Start` 34990 `0x5d4620`, read `0x5d4726` to `0x5d481f` and the helper
  `0x5d50c0`). The effect's location is chosen like this:
  1. No location given: the helper picks. If the helper fails the creature appears at the caster's own position.
  2. A location closer to the caster than `fSummonDistanceCheckThreshold` (750): the helper picks too, and its point
     replaces the given one when it is farther from the caster.
  3. A location 750 or more away: used as given.
  The helper's point is the caster's position plus his heading times (caster radius + creature radius + half of
  `fSummonedCreatureSearchRadius`, 512, so 256 plus the two radii), and then a pathing test from the caster to that
  point decides whether it holds. An NPC's cast gives the hand position (2.1), so case 2 applies: the creature lands
  about 300 units in front of him on ground the engine has tested. SE and 1.7.104 were not read today. Slice 1 adds no
  address, so it does not depend on them.
- FACT + FIELD. The engine's own combat AI casts summons through `CombatMagicCasterSummon`, animated (2 of 84 casters in
  the census). Harbinger seats that caster since ABI 19 (`feat/apmf-buff-summon-seats`, ten caster vtables) and
  `castclassify::IsPlacementSpell` keeps the engine's own placement (`NativePlacement`).
- FACT. MFO already has the road. `ChooseBuffRoad` (`cast/BuffRoad.cpp:424`) answers `Claim` for shape Summon whatever
  the delivery is (`placement` skips every delivery test), target 0, when Harbinger has the seats and the follower has a
  combat controller. Out of combat the substrate (S3) files the same claim. Any other answer is `CastSummonOnce`, the
  direct one-shot, which works but does not animate and logs `UNANIMATED road 'summon-oneshot'`.
- So what happens today when a GAMBIT casts a location summon: it still works. In combat with Harbinger 0.20 or newer it
  is the claim, cast by his own AI with the engine's animation and the engine's spot. `SummonGate` (live, landing,
  limit) guards it on every road. The board's picker lists it (`CastRoadUnsupported` is false for a placement).
- NOT PROVEN. A CLAIMED summon has never been seen firing in an evidence session (complex-spells-research S2), and the
  five newest deck logs (1117f to 1123) hold 0 `[summon]` lines because nobody ran a summon rule and IAUS drops them.
  The 2026-09-24 deck confirmation was the direct one-shot.

### 2.3 Can `APMF_Param.pos` carry a point

- FACT (`APMF_API.h` `kCastFlag_AtPosition`, ABI 11). `pos` is read on `kIntent_Cast` only with that flag, and that is
  the marker one-shot: not a claim, not animated, off by default (`[PositionCast] bPositionCast=0`), refuses summons,
  every projectile spell and every concentration spell. marth's condition on it stands: not an endpoint.
- FACT. On a live cast claim `pos` is not read. No seat feeds a point into an animated cast today.
- Designed, not built (`_research/animated-point-cast-2026-09-23`): a detour on the location helper, scoped to
  (claimed actor, spell, Target Location), hands the engine the client's point. It is compose-shaped (#0 action d): the
  AI still casts and animates. It serves location bursts fully and summons past the 750 rule of 2.2.

### 2.4 The other kinds

| Kind | Record shape | Today's road for a gambit | Animated | Lands |
|---|---|---|---|---|
| Rune | Target Location, FF, effect with a projectile | owned offense claim | yes (FIELD, Stone Rune fired 5 and 25 times) | nothing is placed (FACT, marth's look 09-23) |
| Wall | Aimed CONCENTRATION, impact hazard with kDropToGround (`Prune::IsWallShape`) | the concentration offense claim at the foe | PRED yes, same road as Flames | PRED the hazard forms where the stream meets the ground. Never watched |
| Location burst | Target Location, no projectile, no summon. None in vanilla | owned offense claim | PRED yes | at the caster's hand (2.1), so it behaves as a self blast |
| Self blast | Self, FF, hostile area (Frost Nova, Fire Storm) | `CastSelfDirect`, the self stream | NO (FIELD 11-17 10:36:54) | centred on him |

Harbinger ABI 46 seated Stagger, Disarm, TargetEffect and Paralyze. It added nothing for Target Location or for a
self-delivered hostile spell. There is no rune, wall or self-area caster class in the engine (15 classes by RTTI).

### 2.5 Runtime cost

| Work | 1.6.1170 | 1.5.97 | 1.7.104 |
|---|---|---|---|
| Slice 1 (summons kept) | none | none | none. No address, no seat, no ABI change |
| Point seat on the location helper | id 34447, 2 callers | id 33667, 2 callers, same shape | id to map from `_research/1.7.104-idmap`, body to read. One `VerifiedAddresses` row per runtime, prologue checked |
| Rune completion | launcher 34452 `0x5bfd60`, release chain 34400 to 34407 | launcher 33672 `0x5506c0`. The release chain is NOT traced | nothing read yet |

## 3. Plan per spell kind

### 3.1 Location summon

- Road: the existing claim, target 0. No Harbinger work.
- Point: v1 is the engine's own spot (about 300 units ahead, pathing tested, navmesh snapped). A person does the same
  thing: he turns to the fight and conjures in front of himself. v2 (with the point seat, package H1) lets IAUS name a
  point 750 or more away, or the movement plan's spot picker name one. Until then facing is the only lever, and the
  claim already faces him at his combat target.
- Animation proof: one field look (section 6). Expect the cast under a claim, `MRh_/MLh_SpellFire`, the creature.
- Crime and friendly fire: a summon harms nobody by landing. Its fights are the engine's commanded-actor rules.
  Existing ruling: a crime-faction commander's summon is filtered (MFO-B112 / B113). Nothing new.
- IAUS: the Summon family is live for the Self summons already (`Generate.cpp:961`, veto on a live summon, foe count,
  outnumbered, affordable). A Target Location summon needs no new term. "Complex spells take priority" is the existing
  bump. Thralls are two-handed with a 3 s charge: they wait for card C3 (S9) and are named in the exclude list.
- Gambits: unchanged. Teach page: unchanged, they are already listed.

### 3.2 Wall

- Road: the existing concentration offense claim at a foe. It is not a placement problem. Keep `IsWallShape` as the
  one classifier and stop routing it to `Drop::Placement`.
- Point: the foe. A person sprays the ground in front of a charging enemy. Value comes from a foe who will cross the
  wall, so the target rule prefers a melee foe that is closing.
- Guard: the area census of package M2 run on the hazard footprint (hazard radius, around the foe and along the line
  from caster to foe). An ally who fights that foe in melee stands in it, so a wall on an engaged foe scores 0.
- IAUS value: stream damage while held plus hazard damage for the time a foe is expected to stand in it. The lab has
  the hazard damage (iaus-sim-v4 16.3). The dwell time is a sim-derived number, placeholder until then.
- Proof first: one debug-gambit cast of Wall of Flames at a foe. Does it animate, does the hazard appear, where.

### 3.3 Self blast

- Road: the design already in the backlog (11-17 round 2). A Self, FF, hostile area spell takes `BuffClaim` with target
  0 when a controller exists. No form is minted. `ChooseBuffRoad`'s kind gate is widened and `BuffClaim` gets a cast
  pace in place of the "already up" test.
- Harbinger: a PROBE before any build. File that claim from the debug gambit for Frost Nova and Fire Storm and read
  `[ctcensus]`: which caster is built, does it fire. If no row serves a hostile Self spell, Harbinger needs the seat-0
  flip of complex-spells-research C6 (tier A). Fire Storm is both-hands with a long charge, so it also needs C3.
- Guard: package M2, centred on the caster with the larger of EFIT area and explosion radius (`Prune.h:219`). Today the
  test covers the player and active followers only. Townsfolk, guards, other mods' followers, allied summons and
  mounts are not seen. That hole is live now for Frost Nova.
- IAUS value: the existing AreaValue with foes in radius. A person uses it when surrounded.
- When the road is field-proven, `IsSelfBlastPlacement` is deleted.

### 3.4 Location burst

- Without the point seat it lands at his hand (2.1). That is not what the spell is for, so it stays out until H1.
- With H1: claim plus `pos`. Point = a foe's feet or a cluster centre from the shared spot picker. Guard = M2 at that
  point. Value = AreaValue at that point.
- Lists first: ask the shape census how many TLBurst spells Tuxborn and LoreRim teach before sizing this. Vanilla has 0.

### 3.5 Rune

- Road: only "complete at release" works (animated-point-cast 09-23 item 7). The AI casts and animates the rune as it
  already does. At the release Harbinger calls the engine's own launcher on the actor's own hand caster with the point
  and a real downward pick. That is a perform call, so it needs a written #0 amendment (decision D1).
- Point: at the feet of a closing melee foe, or on the path between the foes and the party. Needs the spot picker and a
  ground pick, so it shares the point plumbing of H1.
- Guard: M2 at the point with the rune's trigger radius, and it must hold for the rune's life, not only the cast
  moment. A rune in a town is a standing crime risk: no rune where a bystander can path within its life (placeholder
  rule, a rune out of combat in a settlement scores 0).
- Value: expected trigger (a foe will cross) times damage, a sim-derived term. The lab has no rune model (MFO-B864).
- Largest and last. Until then the rune stays dropped, and gambits keep the note that a rune animates and places nothing
  (that should become a picker warning, package M5).

## 4. Packages

Authors follow this brief (Fable plans, Opus codes tier A, Sonnet tier B). Sizes are author plus review tokens, rough.

| # | Package | Repo | Tier, author | Size | Needs | Lands |
|---|---|---|---|---|---|---|
| L1 | Lab uses native's drop predicate (review P1), re-extract, count test | MFO tools + a pure function in `Prune.h` | B, Opus (sim rule) | 250k | nothing | now, whatever else happens |
| M1 | Summons leave the Placement drop (slice 1) | MFO | B, Sonnet | 150k | L1's pure function | before the tune |
| F1 | Field look: claimed summon, wall, self-blast probe, one deck session | deck | coordinator gathers, Opus concludes | 80k | M1, a dev build | before the tune if D3 says so |
| M2 | Area census: one reader for "who does this area hit", bystanders included | MFO | A (main-built table read on the worker, #85 pattern), Opus | 450k | nothing | next, it closes a live crime hole |
| M3 | Wall as an offense stream candidate | MFO | B, Sonnet | 250k | M2, F1's wall look | after M2 |
| M4 | Self blast claim road, delete the interim rule | MFO, maybe Harbinger C6 | B Sonnet for MFO. A Opus if the seat-0 flip is needed | 400k, plus 600k if C6 | M2, F1's probe, C3 for both-hands | after M3 |
| H1 | Point seat: a live cast claim reads `pos` for a Target Location spell (ABI 47), three runtimes | Harbinger | A, Opus | 700k to 900k | nothing | post tune |
| M5 | Location burst and far-point summon adoption, spot picker, picker warnings | MFO | B, Sonnet | 350k | H1, M2, movement plan's spot picker | post tune |
| H2 | Rune completion at release, three runtimes | Harbinger | A, Opus | 0.9M to 1.3M | D1, H1's point plumbing | last |
| M6 | Rune candidates, guard over the rune's life, lab rune model | MFO + sim | B, Sonnet. Sim part Opus | 400k | H2 | last |

### L1. Lab book equals game book (lands regardless)

1. Native, tier B, no behaviour change: move the expression at `SpellBook.cpp:253` into one pure function in `Prune.h`,
   `IsPlacement(PlacementIn)` with booleans `targetLocationDelivery, rune, wall, tlBurst, selfBlast, summon`.
   `SpellBook.cpp` fills the struct and calls it. Harness rows pin today's answers. This is the extension point: "to keep
   a placed kind, edit `Prune::IsPlacement`".
2. Extract (`tools/list_balance/extract.py`, `compile_world.py`): export per spell the booleans native reads, computed
   from records the way `cast/Archetype.cpp` computes them: any effect projectile, any summon effect, concentration,
   delivery, hostile area, `impactHazard` (spell-less hazards included, `hazard_row` returns None for those so `hazard`
   is not that boolean), `impactHazardDrops` (flag 16), script rider.
3. `tools/iaus_sim/Data.h:521`: build the same `PlacementIn` and call `Prune::IsPlacement`. No second rule.
4. Re-extract Vanilla, Tuxborn, LoreRim. Old files to the backup folder. Compiled lists carry a version field the
   driver refuses when stale (the `check_world` pattern).
5. Count test: on one list, the lab's drops by reason equal a native census. The census is the `[spellbook] ... no IAUS
   candidate` lines of one deck log for the followers in it, plus a record-level script for the whole list.
6. Baselines are rewritten once and the diff is explained by name (Vanilla loses 3 walls and Fire Storm until M3 / M4).
7. Model fix from 2.2, optional in the same package: `kSummon` spawn distance 150 becomes caster radius + creature
   radius + 256, marked FACT (AE). It moves baselines, so it is its own commit.

### M1. Slice 1, the smallest thing that makes location summons castable and animated

- One line of logic: `IsPlacement` answers false when `summon` is true. A summon that is also a rune or a wall does not
  exist in the lists we read, the harness row says summon wins.
- Harness: Conjure Familiar (Target Location, summon) keeps. A rune still drops. A Self summon unchanged.
- `Reposition.h:29` comment and MAP's IAUS entry updated. iaus-design section 2 "never generated" list corrected.
- No road change, no Harbinger change, no INI, no co-save. The direct one-shot stays the documented degrade and stays
  loud. If the claim does not fire in F1 that is a diagnosis for Opus, never a fallback to hide it.
- Thralls and other both-hands summons: the author checks what the claim does with them today and reports. If they are
  not served they go on the exclude list, they are not patched here.
- Lab: nothing to write. L1's shared function makes the lab keep them at the next build. The count test is re-run.

### M2. Area census (the crime and friendly-fire guard)

- One reader, `Estimate::AreaCensus(center, radius, caster)`, returns allies hit, bystanders hit, foes hit. Bystander =
  any actor that is not hostile to the caster and not in his party: townsfolk, guards, other mods' followers, an ally's
  summon, mounts, the player's horse. Dead and disabled actors do not count.
- Source: a main-built table of nearby actors (position, radius, hostile-to-party flag, kind), refreshed on the real
  cadence, read on the worker. Never a worker walk of the process lists. Budget it from the tick (principle 9).
- Every area consumer calls it: `splash_safe` (aimed AoE, self blast, wall, later burst and rune), and the line-of-fire
  test keeps its own reader. A bystander in the area is a gate at 0, the same as an ally. The spell stays a candidate,
  the situation scores 0.
- Tunables in one table (`AreaGuardTuning`: safety margin on the radius, refresh period, the rune life rule).
- It fixes a live defect (Frost Nova can hit townsfolk today), so it is not gated on any road.

### H1. Point seat (Harbinger)

- Research brief first (one pass): the 1.7.104 body of the location helper, both callers on each runtime, the thread
  the cast core runs on at an NPC release.
- A new flag on a LIVE `kIntent_Cast` claim (not the one-shot path) and `param.pos`. Refused by name for anything but a
  Target Location spell. The detour answers only for (claimed actor, claimed spell). Everything else falls through to
  the engine's own code. Seat row in `VERIFIED-ADDRESSES`, refuse on a failed row.
- Deny question (principle 2): what else calls the helper (AE 43911 / SE 42744 is the second caller) and can a claim's
  point leak into it. The brief must answer that with the hole list.
- Summons: the seat hands Start a location. Inside 750 units the engine still substitutes its own spot (2.2), which is
  correct behaviour to keep. A far point is honoured as given, so MFO must send a navmesh-valid point
  (Harbinger `FindEmptySpace`, main thread).

### H2. Rune completion

- Only after D1. Release hook on the actor's own cast, the engine's launcher, a real `PickObject` at the point, main
  thread. The SE release chain is untraced and 1.7.104 is unread: the research brief is about 250k on its own.

## 5. Order

1. L1 now. M1 right behind it (same day, different files except `Prune.h`: L1 first, M1 rebases).
2. F1 as soon as a dev build with M1 exists. One deck session covers the summon proof, the wall look and the self-blast
   probe (stack tests, one deploy).
3. M2. Then M3, then M4.
4. The tune runs after step 1 or step 2 (decision D3) with the exclude list of section 7.
5. H1, M5 after the tune. H2, M6 last.

## 6. Field steps for the debug gambit (F1)

Dev build (`MFO_DEV_SELFTEST`), Harbinger 0.20.5 or newer, one conjurer with Conjure Familiar and Flame Atronach taught,
one mage with Wall of Flames and Frost Nova. Open field, then one pass inside Whiterun for the guard lines.

1. Summon by IAUS. Start a fight. Expect `[spellbook] Conjure Familiar ... nature=summon` with no "no IAUS candidate"
   line, `[iaus] pick=cast_summon`, a `[buff]`-free claim (no `DIRECT road` line), Harbinger `[ctcensus]` showing a
   Summon caster built under the claim, the hand animation, the creature about 300 units ahead of him.
   FAIL lines: `UNANIMATED road 'summon-oneshot'`, or the buff road's never-fired WARN.
2. Summon by gambit rule, same spell: same lines through `Fire`. This is the first proof of the claimed road at all.
3. Summon with a wall right in front of him (stand him facing a cliff): where does the creature land. This tests the
   helper's fallback (2.2 case 1 says his own position).
4. Second summon while the first lives: no second cast (`SummonGate`).
5. Wall gambit at a foe 600 units away on flat ground: does he animate the stream, does the hazard appear, where. Log
   the hazard ref if the dev build can (passive).
6. Self-blast probe: the dev gambit files the target-0 claim for Frost Nova. Read which caster `[ctcensus]` builds and
   whether it fires. No road change ships from this, it only answers the question in 3.3.
7. Rune gambit (Fire Rune), once: confirm animation and that nothing is placed, and whether magicka is spent.

Method note for the diagnosis: do not argue from counts of casts. One watched cast with its log lines is the evidence.

## 7. What the tune must leave out if a road is not ready

The lab book must equal the game book first (L1). On top of that, these rows are excluded from what a tune may conclude.

| Rows | Why | Until |
|---|---|---|
| Runes (Target Location with a projectile), every nature | nothing is placed, no lab model (MFO-B864) | H2 + M6 |
| Location bursts | lands at the hand | H1 + M5 |
| Walls (`IsWallShape`): Vanilla Wall of Flames, Frost, Storms. Tuxborn 6 and LoreRim 5 no-script impact rows | M3: native offers them now. The LAB still drops them on purpose (`Data.h` `Classify`): it has no footprint gate and its hazards never hurt the party, and the game scores those 0 | M3 field-run, then the footprint (`areaUnits`) and party hazard damage in the lab |
| Self blasts: every Self, fire-and-forget hostile area spell (counts in section 11) | the GAME offers them since M4 (the hazard arm, Blizzard and the circles, stays out: MFO-B872 2); the LAB still drops them all (`Prune::IsSelfBlastPlacement`, `Data.h Classify`, `Drop::SelfBlast` in the lab), so no lab book holds one and nothing about them may be concluded | the M4 field run (a watched Frost Nova claim that fires), then the lab's census-equivalent and party friendly fire |
| Both-hands summons (the thralls) | CANDIDATES since M1, but the claim is filed on LEFT with one hand prepared (`BuffClaim`, `Loadout::Prepare`) and no both-hands cast under a claim was ever seen (S9 [pred]). Their rows stay out | S9 / C3 |
| Reanimate (Dead Thrall) | card C5, not this plan | its own card |
| `cast_summon` and the Summon family rows for conjurer and warlock, and Vanilla in `--worlds` for caster cells | M1 is built. Under D3 option B they stay excluded until F1 has seen a claimed summon fire | F1 |
| Summon spawn point and any score that depends on where the creature stands | the lab's 150 units is a MODEL, the engine's rule is 2.2 | L1 item 7 |

With M1 in, the claim-to-fire share for the Summon row comes from the generic delivery table. That is an ASSUMPTION and
is labelled so until F1 gives real windows.

## 8. Risks

- The claimed summon road has never been watched firing. M1 puts every conjurer on it. If Harbinger's Summon seat does
  not answer, summons stall on the never-fired release and the fight loses its opener. F1 exists for that.
- Both-hands summons may not equip under the claim (S9). They could look like "summons do not work" in the field.
- The engine's spot is in front of HIM. In a doorway the creature can block the party. The lab models bodies in doors,
  so the tune will see it. The point seat is the fix, not a nudge.
- M2 is a new main-built table. A stale table is a crime. Size the refresh from the real cadence and gate on age.
- 2.2 was read on 1.6.1170 only. H1 must read the other two before it relies on the 750 rule.
- The rune road needs a perform call. If D1 is no, runes stay a spell the follower can never use, and the picker should
  say so.

## 9. Decisions for marth

1. **D1, the rune amendment. RULED: approved as a rune-only exception, not built (H2, after the tune).** Runes can only land if Harbinger completes the AI's own animated cast at its release with
   the engine's launcher and a ground pick. That is a perform call, outside INVARIANTS #0 as written. Recommendation:
   yes, as a narrow amendment (only on a cast the AI already chose and animated, only the declared point, once). It is
   the last package, so nothing waits on this today.
2. **D2, Frost Nova until its road exists. RULED: "Block them for now" (built, section 0; deleted by M4 for the area arm).** Today it is kept, goes out unanimated, and can hit townsfolk. Both break
   standing rulings. Recommendation: widen the interim rule to every Self, FF, hostile area spell (drop it, labelled
   interim) until M2 and M4 land, then delete the rule. The other choice is to keep it and accept both defects for now.
3. **D3, when the tune may start.** Option A: after L1 and M1 are merged and reviewed, with the summon delivery share
   as a labelled assumption. Option B: after the F1 deck look confirms a claimed summon fires. Recommendation: B. It
   costs one deck session and it is the only thing that tells us whether conjurer and warlock rows describe an action
   the game takes.

## 10. Sources read

`native/iaus/Prune.h`, `iaus/SpellBook.cpp:225-262`, `iaus/Generate.cpp:961`, `cast/BuffRoad.cpp:312-520`,
`cast/Archetype.cpp:355-380`, `cast/SpellSupport.cpp:24`, `Docs/CAST-DELIVERY.md` (summon rule, buff road, S1, S3),
`Docs/REVIEW-BACKLOG.md` (B865, Frost Nova), `complex-spells-research.md`, `iaus-sim-v4.md` 5 and 16, `iaus-design.md`
section 2, Harbinger `native/APMF_API.h` (`kCastFlag_AtPosition`), `Docs/ADDRESS-TABLE-2026-09-15.md` addendum 09-23,
`Docs/INVARIANTS.md` #0 (e), origin/main `MAP.md` placement entries, `_research/animated-point-cast-2026-09-23`, the two
review logs, deck logs 1117f to 1123, and `binaries/1.6.1170` for 2.2.

## 11. L1 / M1 / D2 counts (taught spells, `book_census.py`)

| List | Taught | Summon kept (before -> after) | Placement dropped (before -> after) | of the rest: runes / bursts / walls (by shape; scripted ones are named for their script) | Self blast blocked (interim) | Offense / Control kept after |
|---|---|---|---|---|---|---|
| Vanilla | 110 | 0 -> 14 | 23 -> 9 | 6 / 0 / 3 | 7 (Blizzard, Fire Storm, Mass Paralysis, Hysteria, Harmony, Mayhem, Bane of the Undead) | 12 / 21 |
| Tuxborn | 1242 | 4 -> 311 | 380 -> 67 | 45 / 4 / 0, scripted 18 | 49 | 371 / 34 |
| LoreRim | 1065 | 20 -> 224 | 270 -> 60 | 3 / 7 / 1, scripted 49 | 62 | 321 / 61 |

"Before" is main's rule (L1, behaviour identical). The lab kept Vanilla's 3 walls and Fire Storm before L1: that is the L1 change. 0 of 2417 verdicts and 0
natures differ between `book_census.py` and the lab, after L1, after M1 and after D2. Deck log 1123 (Tuxborn): 36 dropped spells named, 33 resolve to one
taught spell, 0 differ from L1's lab verdict. Blocked although their own flags say Buff (only the spawned hazard's spell is hostile): Tuxborn `MAG_Blizzard`,
the Apocalypse circles, `WB_A100_TalonsOfNirn_Spell_PC`, `ArnimaCircleMaraSpellDark`. LoreRim: the same circles and Talons, `_LIP_Sanctuary`, `_K01_StormZone`,
`TVR_Cleric_D050_Spell_ConsecratedGround`, `TVR_Cleric_D100_Spell_StormOfVengeance`, `TVR_Shadow_D100_Spell_DrainingMist`. A hostile effect on a townsman is a crime
whatever the caster calls it. The hazard arm cannot tell a circle from Blizzard: both are a benign Spawn
Hazard whose spell carries hostile effects; conditions are not read (Circle of Strength absorbs stamina, Glory and the Moons absorb, Talons of Nirn, Sanctuary and
Consecrated Ground damage). A circle stays only if marth rules that a harm other than health is allowed, and Sanctuary / Consecrated Ground not even then.

**Review F4 (2026-11-24, after the table above): the rule is per effect and reads the hostile FLAG** (`Prune::HostileAreaEffect`: hostile flag AND its own EFIT
area, impact hazard or payload explosion; nature Offense / Control only, or a hostile hazard). Blocked: Vanilla 7 -> 6, Tuxborn 49 -> 42, LoreRim 62 -> 57.
Released and candidates: Vanilla `Harmony`. Tuxborn `MAG_Harmony`, `XWB_R075_HallowedWeapon_Spell_PC`. LoreRim `WB_I000_DispelMagic_Spell_PC`,
`WB_I075_Vanish_Spell_PC`, `XWB_R075_HallowedWeapon_Spell_PC`, `_BL_MassInhibition`. Released, then dropped as script-only: Tuxborn Dispel Magic, Vanish,
Bone Storm, Entanglement, Devastating Corpse Explosion. LoreRim Bone Storm. Still blocked because conditions are not read: Bane of the Undead, Turn
(undead only), `MAG_CircleofProtection` (a hostile flagged 50 ft effect beside the benign hazard).
**Review F1:** a summon the claim road can never serve (`SummonClaimUnservedWhy`) is `Drop::Unanimated`: Tuxborn and Vanilla none, LoreRim 3 (FylgjaWind,
FylgjaSun, Pride of Hirstaang), so LoreRim summons kept = 221. Conjure Staada is served (Spawn Scripted Ref has no caster row, so one row, Summon).
