# G2 hide: design (class plan card G2, sneak line of the class update T2)

Opus 5.5, 2026-11-11. Design only, no code. Read with `calculate-detection-re.md` (the engine formula, every claim
graded there), `class-build-plan.md` G1 / G2 / G3 / H3 / H10, `iaus-classes.md` section 5 and Q87 / Q88,
`class-m0-re.md` section 3.

Grades as in the RE card: DISASM (read on 1.6.1170, proves a path exists), LIKELY, UNREAD, UNOBSERVED (exists, never
seen running), FIELD.

## 1. The finding that changes the card

Card G2 assumed a new Harbinger seat ("H-DETECT") was the only way to hide a follower. The CalculateDetection read
shows otherwise (DISASM):
- The engine scores NPC-to-NPC detection with the same formula as for the player. Its only player-specific branches
  make an NPC harder to detect (shorter noise and projectile reach, a 0.75 range for a foe busy with someone else).
- The combat group already HIDES a target by engine logic. A foe that looks toward him and scores 0 or below loses
  him at once (`kTargetLost`). A foe that does not look loses him after `fCombatDetectionLostTimeLimit` (10 s).
- A sneaking target needs a level above 70, not 25, to be noticed while the group's stealth points are above 0.
- The sneak attack test reads the same per-pair level (`iaus-classes.md` 5.2), and the target selector admits only
  detected, not-lost targets.

So G2 can be built in two layers. **Layer 1 needs no new engine seat.** Layer 2 is the Q88 fallback and is built
only if the field says Layer 1 is not enough.

## 2. Layer 1: engine-native hide (no seat, no new address)

### What MFO does

MFO does not decide that a foe has lost him. The engine does. MFO's job is to put the follower in the state the
engine hides, and to read the engine's verdict.

1. **Be sneaking.** G1 (`apmf/SneakClaims.cpp`, ch.3 stance) holds the stance. In a fight this needs
   `bSneakInCombat` (default 0 today) and the P1 probe result (G1's open question). Sneaking is what puts the
   follower's own Sneak skill into the formula and raises the notice bar to 70.
2. **Read the verdict.** For each foe already in the IAUS gather, read the foe group's `CombatTarget` record of the
   follower (the `TauntProbe.cpp:193-204` read: one read lock per foe group, nothing nested). This is
   `iaus-classes.md` 5.3 Part A, unchanged: `Combatant::detectsMe`, `Snapshot::selfSneaking`, `Facts::unseen`.
   Hidden from foe Y = the record is lost, absent, or `detectLevel <= 0`.
3. **Behave like a hider while hiding.** This is MFO's own choice of action, not an engine write:
   - While `Facts::unseen` is Yes for every engaged foe and no blow from stealth is ready, the Sneak family's "hold"
     row keeps him still (no movement noise) and does not attack or cast (no action noise).
   - The blow from stealth (5.4) is offered on a foe whose record says not detected. The engine applies the sneak
     multiplier through its own logic (5.2). A hit reveals him through ForceDetect. That is the truth, not a hole.
   - When he is detected and a sneak class wants to hide, G3 moves him to a point no engaged foe sees, then the
     engine's own loss does the rest (at once when a foe looks and fails, otherwise within 10 s).
4. **No writes to anyone's actor values.** ch.16 stays unclaimed (it flattens the Thief's own noise passive). A foe's
   values are never touched (memory `gate-the-source`).

### What Layer 1 does not give

- The D&D "hidden at once" rule when the foe is NOT looking his way: the engine waits up to 10 s
  (`fCombatDetectionLostTimeLimit`, a GMST a list may change). Card G2 asked for no forget timer. Layer 1 gives the
  engine's timer. This is saved as a question for marth (Q-G2-1).
- Concealment the engine would not grant. Point blank in front of a foe in light, he is seen. That is correct.

### Tier

- Layer 1's verdict read (Part A): an engine read on the worker under the group lock. `iaus-classes.md` calls it
  tier A. The read itself has a shipped precedent (TauntProbe), it adds no seat, no address and no ABI. By the cost
  table's shape it is **tier B** (logic inside the existing gather) with one tier-A-flavoured spot the Opus review
  must answer: the lock and the off-thread read of `combatController->combatGroup`. Recommendation: **B, Sonnet
  author, Opus review told to treat the lock read as its first uncertain spot.** If the coordinator reads the read
  as threading-shaped, raise it to A. Saved as Q-G2-3.
- The "hold" row and the blow-from-stealth boost: **B, Sonnet** (IAUS candidates and catalogue fields).
- Turning `bSneakInCombat` on by default: an INI default, **C**, after P1 shows the stance holds.

## 3. The probes (all passive, log only, rate-limited, config-gated, default OFF)

One INI key gates all of them: `[Debug] bStealthProbe=0` (MFO.ini, default OFF). The coordinator sets it for the
field run (memory `no-xedit-ship-features-on`: we set test switches). Every line family is capped per follower:
at most 1 line per second per (foe, follower) pair, at most 20 hit lines per minute, and a "N lines suppressed" line
closes a capped window (the `SneakClaims.cpp` [sneak] pattern). No hotkeys. Nothing written to the game. Tier B
(new log lines in existing files, or one new read file if the coordinator wants it separate).

| Probe | Line | Where it reads | Answers |
|---|---|---|---|
| P1 inputs | `[stealth-probe] self`: 1/s per sneaking follower: IsSneaking, IsMoving, IsRunning, Sneak AV, MovementNoiseMult AV, Invisibility AV, equipped weight, combat state, and on the main thread `HandleEntryPoint` values for entries 57, 48, 47, 15 with the follower as owner and his current target as the second argument (the `progression/Progression.cpp:1540` call shape) | worker for the AVs, main thread Post for the entry points | Whether the follower's progression sneak perks evaluate for him (Q87 second half). Whether the stance holds in combat (G1, R3) |
| P2 verdict | `[stealth-probe] seen`: 1/s per (engaged foe, sneaking follower): the foe group's record (detectLevel, stealthPoints, known bit, lost bit, age of lastKnownTimeStamp), the foe's distance and MFO's `SenseActor` verdict, and MFO's PREDICTED score from section 4 of the RE card with the inputs P1 can read (light written as unknown) | worker, the TauntProbe read | That the job computes foe-to-follower pairs (R2). How fast the engine loses a hidden follower (immediate vs 10 s). Where stealthPoints is set back above 0 (R4). How far the predicted sign agrees with the engine (validates the formula read) |
| P3 sneak hit | `[stealth-probe] hit`: each TESHitEvent with a follower aggressor: the kSneakAttack flag, the weapon and its animation type, the target's detectLevel in the record at that moment, the EXPECTED multiplier (the weapon's `fCombatSneak*Mult` read by name from the game settings at runtime, times `HandleEntryPoint(kModSneakAttackMult, follower, weapon, target)` on the main thread), and the target's health on the last pump tick before the hit and on the first tick after | the existing hit sinks (`Diagnostics.cpp:130` HitSink, or the `iaus/Fills.cpp` sink), main thread Post for the entry point | Q87: whether the list's sneak multiplier reaches a follower |
| P4 light (optional) | `[stealth-probe] light`: the follower's cached light level | needs `AIProcess->middleHigh + 0x3A8` on 1.6.1170, an offset read that needs a `VerifiedAddresses` row per runtime | R1. Only if P2 shows the predicted sign disagreeing in exteriors. Saved as Q-G2-4 because it adds a verified row |

**The Q87 decision rule (binding, marth Q87).** The list's sneak multipliers stay the list's, with no MFO damage
code, only if P3 shows, on each list tested:
1. follower hits with kSneakAttack set exist (the engine's sneak attack ran for an NPC: the flag is set only when the
   computed multiplier is above 1, `iaus-classes.md` 5.2 step 5), and
2. on those hits the health drop divided by the median drop of the same follower's non-sneak hits with the same
   weapon is within 25% of the expected multiplier (enough samples to see it: 5 sneak hits and 5 plain hits per
   weapon type), and
3. for a follower that owns a progression sneak perk, P1 shows entry 18 (and 57 / 48 for the detection perks) above
   its no-perk value, and P3's ratio follows the perk's value.
If 1 fails, the engine's sneak attack does not run for an NPC on that list, and the multiplier question goes back to
marth. If 2 or 3 fails, the perk or the GMST is not reaching him and that is a defect to find, not a number to
patch around (principle 7).

Health deltas are noisy (armour, resist, other attackers). The rule compares like with like (same follower, same
weapon, same fight), and the report gives the raw pairs, not only the ratio.

## 4. Layer 2: the Q88 fallback (Harbinger forced non-detection)

Built only if one of these comes back:
- P2 shows the engine does not compute foe-to-follower pairs (R2), or
- P2 shows the engine's verdict is wrong in a way the formula does not explain (for example R1, stale exterior light),
  or
- marth rules that the engine's 10 s unwatched loss is not acceptable (Q-G2-1).

The seat, its declaration and its deny holes are specified in `calculate-detection-re.md` section 7: a post-call seat
on the sole CalculateDetection call site, which writes the engine's own "looked, not seen" result (-1 with the view
cone byte set) for a pair the client declared, so the group's own immediate-loss road hides him. The declaration
rule keeps it a gate on the source: Harbinger enforces only for pairs where the engine's own score is not positive
and the follower is sneaking. It removes the timer, it never invents concealment.

**Tier A, Fable author, Opus 5.5 tier-3 review, effort 5.** It is a new engine seat on a job worker thread, on three
runtimes.

## 5. G3's concealment-point query: what it needs, so its tier can be judged

G3 needs "a point within the leash that no engaged foe can see". Pieces:
- **Candidates.** `FindEmptySpace` (APMF ABI v11, exists, main thread only, at most 14 rays per call) at N bearings
  away from the foes, within the leash. Exists.
- **Visibility of a POINT from each foe.** This does NOT exist in the Harbinger API. `GetLineOfSight` (ABI v18) is
  actor to actor (viewer FormID, target FormID), not actor to point. `SenseActor` is actor to actor too.
- **Light at the point.** Not readable at all without new RE (the cached light is per actor, section 6 R1 of the
  RE card). The first version of G3 should ignore light and use view and line of sight only.

Two ways to build the point visibility:
1. **A new read-only Harbinger query** (`IsPointConcealed(viewers[], point)` or a batched version of it): rays from
   each viewer's eye to the point at feet, torso and head height, main thread, budgeted. No seat. But it APPENDS
   function slots to `APMF_API.h`, a byte-shared header with an ABI bump. By the project cost table, "ABI /
   byte-shared header" is **tier A by shape**, even though the logic is simple. Recommendation if this road is
   taken: tier A, but a light one: Fable or Opus author, ONE Opus tier-3 review aimed at the header append, the ABI
   gate (`abiVersion >= 40` or whatever is next) and the main-thread contract.
2. **An MFO-only composition**: MFO already casts its own rays (`Sightline.cpp:155`, `bhkPickData` / `PickObject`).
   A point variant of `Sightline::MeasureNow` (viewer to point) on the main thread, plus `FindEmptySpace`
   candidates, gives the same answer with no ABI change. **Tier B, Sonnet author.** Against it: memory
   `fix-in-harbinger-not-mfo` puts engine work in Harbinger. A ray is a read, not an engine action, so the rule does
   not clearly forbid it.

Default taken: road 2 for Beta 1 (no ABI change, tier B), with the option to lift it into Harbinger later. Saved as
Q-G2-2 for marth to overrule.

## 6. Implementation pieces and tiers

| Piece | What | Tier | Author | Gate |
|---|---|---|---|---|
| P1-P3 stealth probe | passive lines behind `bStealthProbe` | B | Sonnet | none, first |
| P4 light probe | one verified offset read per runtime | A (new verified row) | Fable | only if P2 asks for it |
| Layer 1 verdict read (Part A) | group record read in the gather, `detectsMe` / `unseen` | B (A if the lock read is judged threading) | Sonnet | after P2 |
| Layer 1 behaviour | hold row, blow-from-stealth boost, `Facts::strikeMult` | B | Sonnet | after P1-P3 |
| `bSneakInCombat` default ON | INI default | C | Haiku or inline | after P1 shows the stance holds |
| G3 point visibility, road 2 | Sightline point variant + FindEmptySpace candidates | B | Sonnet | after H3 |
| G3 point visibility, road 1 | new Harbinger query, ABI append | A by shape | Fable | only if marth picks road 1 |
| Layer 2 H-DETECT seat | Harbinger forced non-detection | A | Fable | only if the probe or a ruling asks for it |

Order: the probe first (one Deck session with G1 on one follower and `bSneakInCombat=1`), then Layer 1, then G3.
Layer 2 waits on the probe.

## 7. Open questions (saved to `pending-questions.md`, defaults taken)

- **Q-G2-1.** The engine loses an unwatched follower after 10 s, a watched one at once. Accept the engine's timing for
  G2 (default), or require the D&D "at once" rule, which needs the Layer 2 seat?
- **Q-G2-2.** G3's point visibility: MFO's own rays (tier B, default) or a new Harbinger query (ABI append, tier A by
  shape)?
- **Q-G2-3.** Is the Part A group-record read in the gather tier B (default, shipped precedent in TauntProbe) or tier A
  (threading)?
- **Q-G2-4.** Allow the optional P4 light probe (one verified offset row per runtime) if P2 shows exterior
  disagreement?
