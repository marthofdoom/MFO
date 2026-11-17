# Per-weapon, per-sequence melee reach (feat/anim-reach, 2026-11-17)

marth, 2026-11-17: the per-weapon reach should feed the new reach checks and be on by default: "Yes on, it will need to read
each animation too. since some have different reach". Then: "some lists (both our testing ones) have combos on light attacks,
and maybe on heavy, and dont allow adjustment during. so it will be the sequence thats tracked."

## Research

**Where reach varies per animation at runtime, and what is readable.**

- `RE::BGSAttackData` (fork `include/RE/B/BGSAttackData.h`, 0x50 bytes): the ATKE event name plus `AttackData` {damageMult,
  attackChance, attackSpell, flags (ignore weapon, bash, power, charge, rotating, continuous), attackAngle, strikeAngle,
  staggerOffset, attackType keyword, knockDown, recoveryTime, staminaMult}. **No reach field.** The only geometric fields are
  the two angles. The actor's current one is `HighProcessData::attackData` (header offset 0x258), which this build does not read
  (an unverified ABI offset on three runtimes, and it names the vanilla attack data of the race, not the clip a combo
  framework plays).
- A combo framework (MCO, ADXP style; Tuxborn and LoreRim) selects the clip itself and chains steps through behaviour
  variables. The clip's name is not a graph event. The graph RAISES the same tags for every clip: `weaponSwing` /
  `weaponLeftSwing`, `HitFrame`, `MCO_WinOpen` / `MCO_PowerWinOpen` / `MCO_WinClose`, `attackStop`. A sink never sees
  `NotifyAnimationGraph` INPUTS (`attackStart` is invisible: STATUS 2026-09-21 field result `attackStart*=0`, `weaponSwing=25`,
  `MCO_PowerWinOpen=18`). So **the single clip is not observable; the chain of swings is the finest identity there is.**
- Precision (`PrecisionBridge`, `PrecisionAPI.h` V1): `GetAttackCollisionCapsuleLength(handle, Default|Current|RightWeapon|
  LeftWeapon)` is per WEAPON / hand (the largest ACTIVE collision when one is live, else the weapon node's computed length). It
  exposes nothing per attack. It is a length from the hand, not a centre-to-centre reach (iaus-i6.md "Precision reach", Q64), so
  it is NOT a seed: the weapon's learner starts at fMeleeReach, as before this change. (The brief's "starting from Precision's
  own reach" does not match the code or the Q64 evidence; left as it was.)
- Attribution of an outcome to a sequence: the swing events of the follower's own graph (counted per follower by `iaus/SwingSeq`)
  bound the chain, the `TESHitEvent` sink MFO already runs (`[iaus-fill]` hit records) says whether a blow of it hit. The existing
  swing tracker already resolves one outcome per attack-state window; a chain is that window stretched over the swing events.

## Design

- **Unit = the sequence.** A chain runs from the first swing (attack state Draw / Swing at a lap, with his combat target in
  the list) until no swing event has come for 1.1 s and his attack state is none (cap 8 s). Its key is (kind, swing count): kind
  light / power / bash (a power window tag or a power-flagged hit, a bash tag), swing count from the graph's `weaponSwing` events
  (a both-hands swing within 150 ms counts once), capped at 15. LIST-AGNOSTIC: tag names only, no framework check, no list data.
- **Outcome** (as before, per chain): distance at the chain's start and whether a blow of his weapon hit his target. So a
  sequence's learned value is how far a chain STARTED from there reaches, its forward carry included. The carry is not measured
  separately (laps are 133 ms x party, too coarse to place a mid-chain position).
- **Fallback chain:** sequence -> weapon -> fMeleeReach. A sequence learner is created at its first outcome and seeded with the
  weapon learner's value then; the weapon learner (seeded fMeleeReach) also takes every outcome. With no graph events for him (VR,
  sink not attached, a framework that raises none) the chain is one attack-state window and only the weapon learner takes it.
- **ONE reader** (`iaus/ReachBook.h`): `Reachability` = max over the weapon's value and its sequences ("the longest thing he is
  likely to swing"), `Gate(kind)` = the swing gate of that kind's sequences else the weapon's. Published per follower
  (`Publish` / `Published`, leaf mutex, any thread) for readers with no Snapshot.
- **Readers moved to it:** `Snapshot::bladeReach` through `Reposition::BladeReach` (`MeleeUnreachableNow`, `StrikeReady`,
  `Generate` Attack family reach), `CombatSense::OutOfMeleeReach` (through `ReachBook::ReachOr`, which feeds the vertical line
  `Reposition::OutOfVerticalReach` and `Combatant::outOfMeleeReach`), `Snapshot::swingReach` (PowerAttack, Bash). Not moved (they are
  not a weapon's reach): touch spells, the melee-buff test and `Estimate::StrikeDistance` (`Snapshot::meleeReach`).
- **Ruling Q70 ("never the target choice") is SUPERSEDED for reachability** by marth's 2026-11-17 answer: whether a foe is
  reachable is read from the learned reach. What Q70 still means in code: the swing gate (the 1.15 x veto) stays the swing
  families'; the Attack family's range curve reads `bladeReach` as its reach (it did read `meleeReach` before).
- **ON by default**, `bIausReachLearn` is the kill switch (INI key unchanged). Off = the flat fMeleeReach everywhere, nothing
  published, the sink not attached.
- **Threading:** the sink writes fixed-size atomics only (the [atk-obs] contract); attach / detach are `MainThread::Post`ed; the
  book and the chain state are the g_followers worker's; no worker walks a live engine list for this. Session state only (no
  co-save), cleared in `Fills::Clear` after `StopPump`.

## To add a reach source

Add a key to `ReachBook::Book::Observe` (and a term to `Reachability` / `Gate`); the engine feed is `Fills.cpp` `LearnReach`; the
tag classifier is `SwingSeq.cpp` `Classify`. No reader changes.
