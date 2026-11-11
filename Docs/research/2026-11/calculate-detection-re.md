# CalculateDetection, read on 1.6.1170 (RE card beside G2, marth Q88)

Opus 5.5, 2026-11-11. Read-only. No code, no seat. Binary: `binaries/1.6.1170/SkyrimSE.unpacked.exe`, ids from
`versionlib-1-6-1170-0.bin`. Addresses below are RVAs (image base 0x140000000). The tool is a small capstone
script (agent scratchpad `g2re/sdis.py`, also described in Linux-Native-Tools `stealth-detection.md`).

Proof grades:
- **DISASM**: read on the 1.6.1170 binary in this pass. Disassembly proves a path EXISTS, not that it runs.
- **LIKELY**: my reading of a register or a byte whose meaning I inferred from context. Re-verify before building on it.
- **UNREAD**: not read in this pass.
- **FIELD**: seen running in a field log. Nothing in this card is FIELD yet.

1.5.97 and 1.7.104 were NOT read in this pass. `class-m0-re.md` section 3 found CalculateDetection's prologue
byte-identical on all three and the bodies matching by instruction count (478 / 483 / 477). The formula below is the
1.6.1170 one. Any seat or offset read built from it needs its own per-runtime proof (principle 11).

Game-setting values quoted are the IMAGE DEFAULTS read from the binary's data. A plugin can override any GMST, so
MFO never hard-codes them. Read them at runtime by name (memory `no-list-specific-data`).

## 1. Where it sits

| Piece | AE id / RVA | Grade |
|---|---|---|
| Detection manager job (the only caller) | id 42742, 0x7B0710, call at 0x7B0D8B | DISASM |
| CalculateDetection | id 37774, 0x690870 | DISASM |
| Sight / stealth score (the formula) | id 26365, 0x415A10 | DISASM |
| Detection range | id 26364, 0x4159B0 | DISASM |
| Noise-event score (same shape, noise inputs) | id 26366, 0x415EB0 | DISASM (strings only) |
| Line of sight test | id 37768, 0x68FD00 | DISASM (callees only, body UNREAD) |
| Heading angle to a point | id 37773, 0x690780 | DISASM (callees only) |
| Cached light level of an actor | id 39509, 0x6EC8B0: `AIProcess->middleHigh + 0x3A8` | DISASM |
| Light level writer | id 39510, 0x6EC8D0, called only from id 39946, 0x713710 | DISASM |
| Group-side consumer | `CombatGroup::UpdateTargetDetection`, 0x805090 | DISASM |
| ForceDetect (stores 100) | id 37765, 0x68FA50, 9 callers | `class-m0-re.md` |
| RequestDetectionLevel (cache reader) | id 37764, 0x68F9F0 | `class-m0-re.md` |

### The job's gate (id 42742, DISASM)

The job computes a pair only when all of these hold:
- the AI detection toggle byte `[*(0x20F69B0)+1]` is set (tdetect),
- neither actor's form flags carry 0x820 (deleted 0x20, disabled 0x800),
- the observer is within 8192 units of the player (`0x18B93E0` = 67108864.0 = 8192 squared),
- the target is within 8192 units of the observer.

There is no player-only test in the gate. A foe observing a follower is a pair like any other, as long as somebody
requested it (the combat group requests its targets, `class-m0-re.md` section 3). Grade: DISASM for the gate. That
foe-to-follower pairs are actually requested and computed in a fight is UNOBSERVED (probe P2 below).

### The signature (DISASM, from the job's call and the function's stores)

```
void CalculateDetection(Actor* observer, Actor* target,
                        int32_t*  outLevel,      // init -1000 by the caller
                        bool*     outInViewCone,
                        bool*     outLineOfSight,
                        int32_t*  outLosResult,  // the raw 37768 result
                        NiPoint3* outWhere,      // target position, or the noise point on the noise road
                        float*    outSound,      // components of the sight road (26365 writes them)
                        float*    outLight,
                        float*    outSkill);
```
The three float outs are new. They mean a future post-call seat can log the engine's own sound / light / skill parts
for free. Re-verify the full signature against the binary at build (principle 6). If either actor has no
`currentProcess` (AE +0xF8) the function returns at once and writes nothing.

## 2. The structure (DISASM)

CalculateDetection tries three roads in order and stops at the first positive level.

1. **Sight road (the stealth formula).** Only when `distance^2 < range^2`, range from section 3. It calls 26365 and,
   if that gives a level above 0, stores it. A result of exactly 0 becomes -1. A negative result is kept.
2. **Noise road.** Only when the observer or the target has a `combatController` (AE +0x160), or the target is the
   player and the observer is hostile to him (id 37537). It also skips when id 37666 returns 2 or 3 (LIKELY a
   relationship test, not read). It reads the target's newest noise record (id 39285 on the target's process, the
   record `AIProcess::SetActorsDetectionEvent` writes). The noise must be within `fDetectEventDistanceNPC` (512) of the
   observer for an NPC target and `fDetectEventDistancePlayer` (1536) for the player, doubled by
   `fDetectEventDistanceVeryLoudMult` (2) at `iSoundLevelVeryLoud` (200) or more. Then 26366 scores it. A positive
   level stores the NOISE POINT as `outWhere`, not the target's position.
3. **Projectile road.** Projectiles near the observer whose shooter is the target, within
   `fDetectProjectileDistanceNPC` (512) for an NPC target and `fDetectProjectileDistancePlayer` (1024) for the player.
   Each is scored by 26366 with a fixed sound level of 50.

**The only player-specific branches** in the whole function and the formula:
- noise and projectile reach: NPC targets are heard at 1/3 (noise) and 1/2 (projectile) of the player's reach,
- the sneak skill transform `skill * fPlayerDetectionSneakMult + fPlayerDetectionSneakBase` (defaults 1 and 0, so
  identity) applies only when the observer or the target is the player,
- `fDetectionCombatNonTargetDistanceMult` (0.75) shrinks the range only for a NON-player target that is not the
  observer's own combat target.
- the light level is recalculated every `fLightRecalcTimerPlayer` (0.5 s) for the player and every
  `fLightRecalcTimer` (2 s) for an NPC (id 39946).

Every one of them favours a sneaking NPC. **Answer to Q88: on 1.6.1170 the engine computes NPC-to-NPC detection with
the same formula as for the player. A follower is, if anything, harder to detect than the player in the same spot.**
Grade: DISASM. The open risks are in section 6.

## 3. The range (DISASM)

```
range = fSneakMaxDistance (1500)
      * (observer cell is exterior ? fSneakExteriorDistanceMult (2) : 1)
      * (either actor flying ? fSneakFlyingDistanceMult (2) : 1)
if observer in combat, target is not the player, target is not observer's combat target (and observer not
   kSearchingInCombat):  range *= fDetectionCombatNonTargetDistanceMult (0.75)
angle = heading angle observer -> target (37773)
if angle > fDetectionLOSDistanceAngle (60 deg):
   range *= 1 + (angle - 60deg) / (180deg - 60deg) * ((exterior ? fDetectionLOSDistanceMultExterior (0.5)
                                                                : fDetectionLOSDistanceMultInterior (0.75)) - 1)
LOS test (37768) only inside that shrunk range.
inViewCone = LOS && angle < fDetectionViewCone (190) / 2
```
Outside the first range nothing of the sight road runs. Beyond the shrunk range there is no LOS and no view cone, but
the formula still runs (sound and skill only).

## 4. The formula, id 26365 (DISASM, every term read)

Inputs the caller passes: O = observer, T = target.

| Symbol | Source | Whose |
|---|---|---|
| `skillO` | O's Sneak AV (15), clamped (id 27284) | foe |
| `skillT` | T's Sneak AV (15), clamped, then perk entry 57 `ModDetectionSneakSkill` (owner T, arg O) | follower |
| `sneakingT` | id 24917: ActorState1 sneaking (0x200) and not swimming (0x400), with an extra-data 0xA9 exception | follower |
| `moving`, `running` | id 37953 IsMoving, id 37234 IsRunning on T. Perk entry 15 `IgnoreRunningDuringDetection` (owner T) clears `running`, and clears `moving` too when T is sneaking | follower |
| `noiseMult` | T's MovementNoiseMult AV (92) | follower |
| `weight` | T's equipped weight (id 38286, cached at Actor AE +0x200) | follower |
| `action` | T's `actionValue` (Actor AE +0x11C): the noise of what he is doing now | follower |
| `light` | T's cached light level (39509), times `fDetectionNightEyeBonus` (3) when O has NightEye (AV 55). Minus `iLightLevelExteriorMod` (0) outside, or minus `min(sky ambient * 100, iLightLevelInteriorMod (50))` inside. Clamped 0..`iLightLevelMax` (100), then perk entry 47 `ModDetectionLight` (owner T, arg O), rounded | follower, foe |
| `invisible` | T's Invisibility AV (54) > 0 | follower |
| `blind` | O's Blindness AV (84) | foe |
| `size` | T's race size class (race +0x128): 0.5 / 1 / 1.5 / 2 for small / normal / large / very large | follower |
| `asleep` | O's sit/sleep state == sleeping (ActorState1 bits 14-17 == 7) | foe |
| `inCombatO` | O IsInCombat (vfunc 0xE3) | foe |
| `busyO` | O's combat target is someone other than T | foe |
| `ambush` | 0 / 1 / 2 from O's package (procedure type 9 check, id 39315). LIKELY: 1 = O is ambushing and T is its target, 2 = O is ambushing someone else | foe |
| `dist`, `LOS`, `inViewCone`, `exterior`, `flying` | section 3 | both |

```
alert      = 1 + inCombatO * fSneakAlertMod (0.2) + asleep * fSneakSleepMod (-0.2)
               + busyO * fSneakCombatMod (-0.4)
               + (ambush == 1 ? fSneakAmbushTargetMod (2.5) : ambush == 2 ? fSneakAmbushNonTargetMod (-0.25) : 0)
perception = fSneakPerceptionSkillMin (10) + (Max (90) - Min (10)) * skillO / 100
maxDist    = fSneakMaxDistance (1500) * (exterior ? 2 : 1) * (flying ? 2 : 1)
distF      = max(0, (maxDist - dist) / maxDist) ^ fSneakDistanceAttenuationExponent (2)

soundF     = distF, times fSneakSoundLosMult (0.25) when there is no LOS and ambush != 1
moveNoise  = moving * max(noiseMult, 0) * (fSneakEquippedWeightBase (12) + fSneakEquippedWeightMult (0.5) * weight)
sound      = max(0, (int(fSneakActionMult (1)) * action + (running ? fSneakRunningMult (2) : 1) * moveNoise)
                    * fSneakSoundsMult (1.6) * soundF * (asleep ? 1 + fSneakSleepBonus (-0.5) : 1))

lightMove  = 1 + fSneakLightMoveMult (0.01) * moving + fSneakLightRunMult (0.2) * running,
             then perk entry 48 ModDetectionMovement (owner T, arg O)
lightBase  = light * (exterior ? fSneakLightExteriorMult (1) : fSneakLightMult (1.4)) + fDetectionSneakLightMod (0)
sight      = asleep ? 0 : (100 - blind) / 100
lightTerm  = max(0, (inViewCone && !invisible) * distF * lightMove * lightBase * sight
                    * size * fDetectionLargeActorSizeMult (1))

skillTerm  = (distF * alert * perception - (sneakingT ? skillT : 0)) * fSneakSkillMult (0.5)

score      = fSneakBaseValue (-25) + sound + lightTerm + skillTerm
level      = score >= 1 ? int(score) : score > 0 ? 1 : int(score)     (0 then becomes -1 in CalculateDetection)
```
`sound`, `lightTerm` and `skillTerm` go out through the three float outs.

What this says, in plain words:
- **Sneak skill counts only while sneaking.** Not sneaking, the follower's own skill is not in the formula at all.
- **Invisibility removes the light term only.** An invisible follower can still be detected by noise and by the
  foe's perception at close range.
- **Out of the view cone, light does not matter.** Behind a foe, only sound and the skill duel count.
- **Standing still removes movement noise.** Then only `action` (attacking, casting) makes sound.
- **Close range is dangerous whatever you do.** At point blank in combat with the foe's Sneak 100:
  `skillTerm = 0.5 * (1.2 * 90 - skillT) = 54 - skillT / 2`. With score = -25 + skillTerm and no sound or light, a
  sneaking follower stays undetected only with Sneak skill above 58 (perk entry 57 included). At half the maximum
  distance `distF` is 0.25 and any skill hides him from the skill duel alone.

## 5. What the combat group does with the level (UpdateTargetDetection 0x805090, DISASM, LIKELY on the flag bytes)

The group's `CombatTarget` (fork `CombatGroup.h`: +0x04 detectLevel, +0x08 stealthPoints, +0x10 lastKnownLoc, +0x90
lastKnownTimeStamp, +0xA0 the member that set the level, +0xA6 flags: bit 0 kTargetKnown, bit 1 kTargetLost):

1. The group keeps the HIGHEST level any member reported. A lower level replaces it only when it comes from the
   member that set it (or through id 44786 when the group has more than one member).
2. **The notice threshold.** While `stealthPoints > 0`, a positive level counts only when it is above
   `iCombatStealthPointSneakDetectionThreshold` (70) for a SNEAKING target, or
   `iCombatStealthPointDetectionThreshold` (25) otherwise. Below it, the level is treated as 0. Above it, stealthPoints
   goes to 0, the target is "known" (bit 0), and its position and time are recorded. With `stealthPoints <= 0` any
   positive level is a notice. So sneaking raises the bar from 25 to 70. Where stealthPoints is set above 0 again is
   UNREAD (probe P2 logs it).
3. **Lost, the timed road.** When no positive level has refreshed `lastKnownTimeStamp` for
   `fCombatDetectionLostTimeLimit` (10 s), bit 1 (lost) is set.
4. **Lost, the immediate road.** When the new level is NEGATIVE and the result record's byte +0x14 is set (LIKELY the
   view-cone out: "the foe looked and did not see him"), the target is lost AT ONCE unless it was known this pass and
   is visible. An invisible target (AV 54 > 0) is lost at once even when known.
5. **Lost, the search road.** With a negative level and the byte at +0x14 clear, when the foe is within
   `fCombatDetectionLostCheckNoticedDistance` (256) of the last known point and a check on that point passes (id 47311,
   UNREAD), the target is lost.
6. A target both known and lost has its lost bit cleared (found again).

So **the engine already has "hide when unseen"**: a foe that looks his way and fails the formula loses him at once,
and one that does not look loses him after 10 s. The target selector admits only detected, not-lost targets
(`class-m0-re.md` H2), and the sneak attack test reads the per-pair level (`iaus-classes.md` 5.2). This is the
basis of the G2 design (`g2-hide-design.md`).

## 6. What MFO can influence through normal game state, and what needs a hook

| Input | Lever | Road | Notes |
|---|---|---|---|
| `sneakingT` (unlocks `skillT` and the 70 threshold) | G1 `act.sneak` (ch.3 stance) | normal state | Shipped on main. In combat UNOBSERVED (bSneakInCombat=0) |
| `skillT` | progression Sneak skill, perk entry 57 perks | normal state | Perks live on the base NPC (`progression/PerkGate.cpp:626`). The formula's owner is the follower: DISASM. The perk evaluating for him: probe P1 |
| `moving`, `running` | hold still while hidden, never sprint | normal state (MFO's own movement choice) | Entry 15 perks also clear them |
| `noiseMult` | the follower's own perks and gear (Muffle) | normal state | ch.16 holds it at 0 and is NOT claimed (it would flatten the Thief's own passive, `SneakClaims.cpp` header) |
| `weight` | his gear | normal state | Lighter gear is quieter |
| `action` | do not attack or cast while waiting to hide | normal state (MFO's rule choice) | Which actions set it and how loud: UNREAD |
| `light` | stand in shadow (G3 picks the point), perk entry 47 | normal state | NPC light refresh: see risk R1 |
| `inViewCone` | be behind or beside the foe (G3, H3) | normal state | |
| `LOS` | break line of sight (G3) | normal state | |
| `dist` | keep distance (G3) | normal state | |
| `invisible` | an Invisibility effect (H10 Shadowcloak) | normal state (a spell) | Only for the class whose kit has it |
| The 10 s lost timer | none | **hook** | Only the fallback seat can make an unwatched loss instant |
| A foe's own inputs (skillO, alert, blind, NightEye) | none that is ours | not MFO's to change | Never write a foe's values (memory `gate-the-source`) |
| ForceDetect reveals (a hit, an alarm, StartCombat) | none, and none wanted | engine truth | A hit on a foe reveals the attacker. Correct |

**Conclusion.** Every input on the follower's side is reachable through normal game state. A hook is needed only
for the D&D "hidden at once" rule when the foe is NOT looking toward him (the 10 s timer), and for the case the
probe might show: the engine not computing the pair at all. That hook is the fallback below.

### Risks the probe must settle

- **R1 NPC light refresh.** The light writer (id 39946) runs when the middle-high process byte +0x328 is set, OR the
  actor is in an interior cell. In an exterior, an NPC's light level updates only while that unidentified byte is
  set. If it is clear for followers, their cached light is stale (or 0). Grade: DISASM for the gate, the byte's meaning
  UNREAD.
- **R2 Pairs computed.** That the job really computes (foe, follower) pairs in a fight: UNOBSERVED.
- **R3 Stance in combat.** Whether the combat behaviour tree stands a sneaking NPC up: UNOBSERVED (G1).
- **R4 stealthPoints refill.** What sets stealthPoints back above 0 after a notice (so the 70 bar applies again): UNREAD.
- **R5 The +0x14 byte.** That it is the view-cone result and not something else: LIKELY.

## 7. The fallback, specified only (Harbinger forced non-detection, a later tier-A Fable round)

Only if the probe shows R1 or R2 failing, or marth rules that the 10 s timer is not acceptable for G2.

- **Seat.** A post-call `write_call<5>` on the sole CalculateDetection call site (AE 0x7B0D8B, SE 0x719EB6, 1.7.104
  0x7C589B per `class-m0-re.md`), each runtime with its own `VerifiedAddresses.h` row and `SeatVerified()` guard.
  The seat calls the original, then, for a pair (observer Y, target X) that a client DECLARED unsensed, writes
  `*outLevel = -1` (the engine's own "looked, not seen" value, the same value CalculateDetection makes of a 0) and
  `*outInViewCone = 1` before the job stores and dispatches. Through section 5 road 4, the group loses X at once,
  through engine logic. Nothing else is written. The three float outs are logged in the probe mode.
- **Declare -> enforce.** A new Harbinger intent ("H-DETECT", one facet: a foe's detection of this actor). The client
  declares X and the foes. Harbinger enforces only for declared pairs. MFO's declaration is "the engine's own score
  for this pair is not positive AND X is sneaking" (it reads the float outs through the seat's mirror), so the seat
  removes only the timer and never invents concealment the engine would not grant. That keeps it composition, not
  substitution. A hard "unsensed no matter what" mode is a separate, later ruling.
- **Deny holes to list in that brief.** ForceDetect (id 37765, 9 callers: hit, alarm, StartCombat) are legitimate
  reveals: the seat yields on that pair until the next negative engine score. The group's low-process detection pass
  (AE id 44804, store at 0x80CF30) is a second store and either a second seat or a declared hole. ch.20 / ch.21
  held-engagement seats un-lose a holder's target: arbitrate on the `Detection` sources facet. The noise road (section
  2 road 2) and the projectile road can still return a positive level after the seat: they are inside the same call,
  so the seat sees the final level and the declaration test covers them.
- **Threading.** The job runs on the detection job worker. The declared pairs are published lock-free. Tier A
  threading.
- **Tier.** A, Fable author, Opus 5.5 tier-3 review. Effort 5 (as card G2).
