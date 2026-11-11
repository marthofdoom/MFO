# Movement batch: H3 disengage drive, ch.24 on a point, G3 disengage to out of sight, anchor leak Q92

2026-11-11. Branches: Harbinger `feat/h3-disengage`, MFO `feat/g3-disengage`. Cards: class-build-plan.md H3 and G3,
g2-hide-design.md section 5, iaus-sim-v2.md 26.3. Tier A (a change inside an engine-seat channel in Harbinger, a new
mechanism in MFO). Not field run.

## 1. What already existed (found at the start, so it was not built again)

- **ch.24 on a point is on both mains.** `kApproach_Point` (Harbinger ABI v39) was merged from `feat/approach-point`
  before this batch started, and MFO's `native/APMF_API.h` is its byte copy (md5 equal). MFO already has one client,
  the give-ground step (`apmf/GroundApproach.cpp`, `act.give_ground`, `[Debug] bIausGroundRoad`).
- So this batch changes **no ABI**. Nothing was appended to `APMF_API.h`.

## 2. Anchor leak Q92 (Harbinger)

**What happened** (LoreRim 2026-11-11 09:16:45, `APMF.log`). Auri opened an out-of-combat Candlelight in a 1.4 s lull
of a party fight. 620 ms later:

    ENTRY StartCombat 0x483139C8 (a live holder) target(rsi)=0x00000000 group(rbp)=0x22EAF70D00 [not a substrate group] caller RVA 0x71C2E2
    ANCHOR LEAK (sweep): 0x00013491 'Sven' is NOT a substrate holder and its group lists the anchor -- group 0x22EAF70D00 (2 member(s), a held holder among them, 1 target(s), 0 not the anchor)
    ANCHOR LEAK 0x00013491 cleanup: a held holder shares the group: the anchor stays in it (the holder's substrate stands); combat ENDED (Actor::StopCombat) + package re-pick.

**Why** (1.6.1170 disassembly, read in this batch).
- `Actor::StartCombat` (0x6B6930), for an actor that ALREADY has a combat controller, jumps to 0x6B6D39. With a null
  target, or a target that is already the controller's, and a group, it calls the group join: 0x6B6D78 -> 0x83D2C0.
- The join (0x83D2C0), for an actor whose controller is in ANOTHER group, calls 0x804A80 (new group, old group) at
  0x83D325 and then 0x8048D0 (old group). 0x804A80 walks the old group's targets (stride 0xA8 at +0x08, count +0x18)
  and members (stride 0x0C at +0x20, count +0x30) into the new group. It is a MERGE.
- The merge has one call site (0x83D325). The join has two, both inside StartCombat (0x6B6CB7, 0x6B6D78). Byte scan
  of .text for E8 and E9 rel32. 1.5.97: join 0x7A5FA0 (callers 0x62553B, 0x6255FD), merge 0x76B160 (one caller,
  0x7A6005). 1.7.104: StartCombat is instruction-identical to 1.6.1170, join 0x8521A0.
- So the engine's "an ally is fighting, join his group" call (function 0x71C020) on the HELD holder merged her
  substrate group, whose only target is the anchor, into Sven's real group. Sven's last foe had died 620 ms before, so
  his group then held one target: the anchor. The sweep's branch for "a held holder shares the group and nothing but
  the anchor is left" was written for the opposite case (an outsider who joined the holder's group) and stopped Sven.

**The fix.**
1. At the seat (the existing StartCombat self-check seat, no new address): a HELD holder's StartCombat with a group
   that is not a substrate group, and a target that is none or the anchor, is refused. A held holder's substrate group
   never joins anyone's fight. `[CombatSubstrate] bSubstrateHolderJoinGuard` (1).
2. In the sweep: it remembers the controller each actor was last seen clean with. An actor found with the anchor while
   that controller is still his was fighting before the anchor reached him. No StopCombat on him in any branch. The
   held holder in his group drops as real combat instead (`merged-into-a-genuine-fight`), which removes the anchor.

**What changes for a client.** A held holder is no longer pulled into an ally's fight by the group join. It still
goes real through a target of its own, a hit, or the party's fight (the three triggers).

## 3. H3: the disengage drive

**The engine does the run.** With ch.24 on a point and a small R the follower is outside his combat area, so the
Movement Selector's Return To Combat Area takes the Movement half and paths him there at RUN speed (class-m0-re.md
section 1: speed enum 3 on all three runtimes, static proof). MFO's client is `apmf/Disengage.cpp`:
- ch.24 with `kApproach_Point | kApproach_Hold` (he stays inside R after he arrives, until MFO releases),
- a ch.7 Offense deny for the leg (optional): no swing, bash or cast on the way. Two claims, two facets. No combined
  Harbinger flag was added: its intents stay orthogonal.

**Facing is not known, and the drive does not depend on it.** The static RE could not say whether the body faces
along the path or keeps facing its target. Harbinger now logs it (`[ch.24] ... GAIT PROBE`: speed, the angle between
heading and velocity, the engine's own move bits; one summary at the claim's end). Arrival is the engine's own "inside
the area" answer, so a strafe or a backpedal still arrives, only slower. **A facing seat is not built** (marth's
default for B#16: only if the field shows a backpedal). If the summary reads mostly strafe or backpedal, that seat is
the next card, tier A, in Harbinger.

## 4. G3: disengage to out of sight, then sneak

`native/Concealment.cpp`, switch `[Debug] bIausHideRoad` (default OFF until a field run proves the drive).

1. **Who.** A follower on Sentient AI whose equipped class has both `Flag::kSneakAlways` and `Flag::kDisengage`
   (the Thief line), class tree ON, and whom a foe has found.
2. **The pick** (main thread, 4 candidates per frame, at most one pick per 1.5 s per follower). 24 candidates: 12
   bearings on rings of 320 and 600 units, each from Harbinger's `FindEmptySpace` (standable, empty, a straight walk
   from him). Each is tested with MFO's OWN rays (marth 2026-11-11) from every listed foe's eye to the point at three
   heights of a crouched body (`Sightline::PointSeenNow`). A point is taken only when every foe's rays are stopped by
   geometry. A ray that could not run is never read as hidden. Only solid world geometry is cover (static,
   animated static, tree, prop, terrain, ground): actors, overlap volumes, stair helpers, invisible walls and glass
   are passed through, an allow-list, because a point wrongly read as hidden is the defect. Also refused: a point within 250 units of a foe, within 200 units of the leash edge, or less
   than 160 units from him. The nearest hidden point wins.
3. **The run.** The IAUS `Disengage` family (weight 1.0, hard cooldown 25 s) is generated only once a fresh point is
   known. Chosen, `act.disengage` starts the drive (R 90, Offense denied). While the run or its hold stands his rule
   scan is skipped, as in a retreat.
4. **The crouch.** On arrival: the G1 stance claim (`apmf/SneakClaims.cpp`), and a hold at the point for up to 12 s
   (the engine's own lost timer is 10 s). The hold ends early when he is hit there.
5. **Every failure is said out loud** (`[conceal] ... hide run FAILED: <why>`): Harbinger dropped the bound (no path),
   another ch.24 claim owned him, the point lay outside the leash, he did not arrive in 10 s. No retry inside the
   cooldown.

**No engine state is written.** No detection value, no actor value. The engine loses him by its own rules.

### Seams to G2 Layer 1 (branch `feat/g2-hide-layer1`, not on main)

- **"Has a foe found him."** `native/iaus/Disengage.h` `FoeHasFound`. On main it reads "this foe's combat target is
  the follower". Layer 1 reads the foe group's own record into `Combatant::detectsMe`. When Layer 1 is on main the
  body becomes `return a_foe.detectsMe == Tri::Yes;`. One line.
- **"Is he hidden yet."** G3 does not read it. The hold is a fixed 12 s. With Layer 1's `Facts::unseen` the hold could
  end the moment every foe has lost him. Not built: it needs Layer 1's field.
- **Who keeps the stance after the hold.** G3 files the G1 claim through `ServiceSneakRule`. After the hold the claim
  ages out in three table laps unless a sneak rule stands. Layer 1's hold row assumes the stance is held. For an IAUS
  follower nothing holds it today. This is a gap between G1 and Layer 1, reported, not solved here.
- In combat `ServiceSneakRule` needs `[Debug] bSneakInCombat=1` (default 0). With it off he reaches the point and
  holds there standing, and the log says so.

### Limits, stated

- Candidates are straight walks from him. A point around a second corner is not found.
- Line of sight only: no view cone and no light (g2-hide-design.md section 5 chose that for the first version).
- The point is not checked on the navmesh by MFO. A point the engine cannot path to costs up to 3 s and is logged.
- The pick costs rays on the main thread: at most 4 candidates per frame, each one FindEmptySpace (up to 14 rays)
  plus up to 6 foes x 3 rays.
- The weight 1.0 and the 25 s cooldown were chosen, not searched. The sim has no model of this family.

## 5. Files

Harbinger `feat/h3-disengage` (7639f18): `channels/AnchorGuard.cpp`, `channels/CombatSubstrate.{h,cpp}`,
`channels/CombatApproach.cpp`, `APMF.ini`, docs.
MFO `feat/g3-disengage`: new `native/Concealment.{h,cpp}`, `native/apmf/Disengage.cpp`, `native/iaus/Disengage.h`;
edits in `Sightline.{h,cpp}` (PointSeenNow), `apmf/APMFBridge.h`, `apmf/Bridge.cpp`, `iaus/Core.h`, `iaus/Snapshot.h`,
`iaus/SpellBook.cpp`, `iaus/Candidates.cpp`, `iaus/Generate.cpp`, `iaus/Catalogue.cpp`, `scheduler/IausSource.cpp`,
`scheduler/CombatScan.cpp`, `cast/Fire.cpp`, `Followers.cpp`, `Serialization.cpp` (one transient clear), `Vocabulary.h`,
`Config.{h,cpp}`, `CMakeLists.txt`.

## 6. Fix round (marth rulings 2026-11-11, on top of the merge with G2 Layer 1)

- **Seam closed.** `iaus/Disengage.h` `FoeHasFound` = `detectsMe == Tri::Yes`. G3's snapshot fields are `concealRoad` /
  `concealPoint` / `fleeRun` (G2 owns `hideRoad`).
- **Pacing.** The main purpose of the run is to become hidden. A sneak class's run / hold ends as soon as no listed foe detects
  him (`Concealment::Service`, G2's own verdict read, on the way or at the point); the 12 s hold is only the upper bound and is
  logged when hit. G2's hold row and the blow from stealth take over. For anyone else the disengage is a FLEE: same drive, a
  point at least 150 units farther from the nearest foe (no cover test), heals first (`Cons::BoostFleeHeal` x2, offense not
  generated), over when health is above 30% (the retreat's own line, `Cons::RetreatHp` / `RetreatDanger`), on arrival or on
  failure. Need terms: hide = share of counted foes that found him, flee = RetreatDanger (low health) plus a foe on him.
  The 25 s hard cooldown counts from the end.
- **MB3.** Class tree OFF: no hide run (`ClassGate::sneakClass` is false then) and no flee (the generator requires `treeOn`).
- **MB5.** The stance claim is held for the hide from arrival (or from "hidden") through G2's hold, released on the blow, on a
  reveal, at combat end, dismissal, death, stand-down and load. API: `APMFBridge::HoldSneakForHide` / `ReleaseSneakHide`.
- **Review fixes (1f59152).** `RequestDisengage` releases the offense approach and the give-ground step and `Begin` consumes the
  pick only after Harbinger accepted; the dead/disabled early return in `CombatScan` releases the run and the hold.
