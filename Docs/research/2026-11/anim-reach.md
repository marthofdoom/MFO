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

## Design (after the Opus review, same day)

- **Unit = the sequence.** A chain runs from the first swing (attack state Draw / Swing at a lap, with his combat target in the
  list) until his attack state has been kNone for 0.45 s (cap 8 s). The grace is sized under the measured ~0.6 s the engine takes
  to decide the next attack (so two attacks are two chains) and over a lap; it is NOT field proven for the combo frameworks (the
  `[iaus-fill] swing` line prints each chain's swing count). The key is the NUMBER OF SWINGS (capped at 15; a both-hands swing within
  150 ms counts once). LIST-AGNOSTIC: tag names only, no list data. There is NO light / power / bash key: `MCO_PowerWinOpen` /
  `BFCO_NextPowerWinStart` are combo WINDOWS for chaining into a power attack (09-21: 18 of them against 25 swings), `bashStart` and
  `attackPowerStart*` are behaviour inputs a graph sink never sees, and the hit's power flag only exists on a hit. The swing line
  prints the power-window and bash tag counts per chain so a field run can prove one; then the key gets a kind back.
- **Edge reach (marth, "Edge reach + split uses").** The outcome's distance is the centre distance at the chain's start minus the
  foe's body radius (`CombatSense::BodyRadius`: half the wider OBND x / y extent x the base height, field reads only), so a giant's
  size drops out. A reader adds ITS foe's radius back. The seed is fMeleeReach less a humanoid radius (22), which reads back as
  exactly 200 on a humanoid. `TESObjectWEAP::Data::reach` (fork TESObjectWEAP.h:169) is NOT used as a seed: nothing here proves
  Precision / the engine's hit geometry follows it, so the learner's own seed stays flat and the field line should log it first.
- **Two uses, one reader (`iaus/ReachBook.h`).** `Use::Blow` = the plain per-weapon edge value (fed by single swings and by chains
  whose swings could not be counted, the closest observable thing to one blow without lunge): the HEIGHT line
  (`MeleeUnreachableNow`, `CombatSense::OutOfMeleeReach`; with a humanoid radius, not the foe's: how high a blade reaches does not
  grow with the width of what is up there, so a dragon 450 up stays unreachable) and `StrikeReady`. `Use::Combo` = the max over the
  weapon's sequences and the weapon (lunge included): the approach and the swing gate (the attack's range test, PowerAttack, Bash,
  and the `cast/Fire.cpp` executors' close-or-swing limit through `SwingLimit`, so a power attack the IAUS picked at the learned
  gate is not turned into a closing Fired). Never the max over chains for the height line.
- **Fallback chain.** Sequence -> weapon -> fMeleeReach. A new sequence learner is seeded from the weapon learner's value at its
  first outcome. With no graph events for him (VR, sink not attached, a framework that raises none) the chain is one attack-state
  window and only the weapon learner takes it.
- **Hit attribution.** A per-follower latch (the latest time of each (target, source) pair he hit, eight pairs), not a shared ring: a
  cloak tick or a neighbour's hits cannot evict a chain's early blow and file a false miss.
- **ON by default**, `bIausReachLearn` is the kill switch (INI key unchanged). Off = the flat fMeleeReach everywhere, nothing
  published, the sink not attached. The published pair is erased at `CombatEnded` / `Forget`.
- **Ruling Q70 ("never the target choice") is SUPERSEDED for reachability** by marth's 2026-11-17 answer. What it still means in code:
  the swing gate stays the swing families', and no reach decides WHICH foe is named.
- **Threading:** the sink writes fixed-size atomics only (the [atk-obs] contract); attach / detach are `MainThread::Post`ed; the book
  and the chain state are the g_followers worker's; no worker walks a live engine list for this. Session state only (no co-save),
  cleared in `Fills::Clear` after `StopPump`.

## To add a reach source

Add a key to `ReachBook::Book::Observe` (and a term to `BlowEdge` / `ComboEdge`); the engine feed is `Fills.cpp` `LearnReach`; the
tag classifier is `SwingSeq.cpp` `Classify`. No reader changes.
