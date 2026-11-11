# MFO CLASS UPDATE: build plan (Opus 5.5 design pass, 2026-11-09)

Inputs, in authority order: `class-walkthrough-decisions.md` (marth's binding rulings, 2026-11-09), the superseded
draft `class-perks-packages.md` (only its verified field facts are reused here), memory `class-update-fire-emblem-spec`,
memory `followers-never-commit-detected-crimes`, MFO origin/main `ed70d36`, Harbinger origin/main `4cff499`, and Harbinger
`feat/apmf-ch18-attack-select` `c91471c` (ch.18, ABI v34, not merged).
This is a design and planning document. No code, branches or ClickUp writes were made. Durable notes belong on ClickUp
86e3dhbd3; the coordinator pastes them there.

## Key facts this plan stands on (verified this pass)
- **Gambit vocabulary** (`native/Vocabulary.h` origin/main, 363 lines): there is no `act.sneak`, `act.steal`, `act.shout`,
  `cond.self_has_summon`, foe-attacking-my-summon selector, corpse target, party-healthy condition, ward-on-charge
  condition, or any detection condition. New opcodes are APPENDED strings. Renaming one is a schema migration (MAP.md
  "Vocabulary.h", #10). Adding one needs Evaluator + Actuation + Board picker wiring, or it is inert.
- **There is NO per-rule gambit cooldown.** `struct Gambit` (`native/State.h:28-53`) holds uid, opcodes, `conditionParam`,
  `actionParamForm`, `subjectSelector`, `subjectActorForm` and `enabled`, plus display-only `lastFired` / `lastFiredAt`. The
  evaluator must never read those (#14 companion, State.h:44-46). The only cooldown is the global cast pacing
  `fCastCooldown` (`Config.cpp:177`). A per-rule cooldown is a new serialized field: FLWR v5 -> v6 (writer
  `Serialization.cpp:469`, reader `:759`, version gates `:706/:717/:768`). That makes it **tier A (co-save)**.
- **Evaluator** (`native/Evaluator.cpp`, 992 lines): `IsFoeSelector` (`:26-43`), `IsAllySelector` (`:53-56`), `FoeTargets(foe, who)`
  (`:60`), `PickFoe` (`:322`), `Evaluate` (`:677`). Conditions are pure reads (#23), so engine state must come from
  worker-safe mirrors (the `SelfStatus.cpp` pattern: one `MainThread::Post` refresh per actor).
- **Summons**: `cast/Summon.cpp` already counts live commanded actors (`:230-261`, `commandedActors.size()` `:242`) and reads
  `kModCommandedActorLimit` through `HandleEntryPoint` (`:106-113`). It walks the effect list on the MAIN thread only.
- **ESL generator**: `MFO_GenerateESP.py` (2197 lines) has never emitted a PERK or FACT record. Its precedents are CSTY
  (`_csty_record` `:1201`, `make_csty` `:1239`), the add-on ESL (`make_progression_esl` `:1817`), the manifest FLST (`:1508`), the
  entry-point verdict table (`PROG_ENTRYPOINT_VERDICTS` `:1704`, 92 rows, graded E/M/D on NPCs) and the add-on MCM
  (`prog_mcm_config` `:1990`). PERK entry points with CTDA conditions are a NEW byte shape and need a byte audit
  (`tools/esp_inspect.py`, `tools/audit_esp.py`, no xEdit per the standing rule).
- **CombatStyle** (`CombatStyle.cpp`, 625 lines): `ApplyTick` (`:138`) writes `CombatController::combatStyle` 0x38 from
  `Forms::g_meleeStyle/rangedStyle/castStyle`. The `Stance` enum ordinals are a serialized ABI (`CombatStyle.h:36`).
- **Harbinger facets** (`Docs/FACETS.md` / `CHANNEL-MAP.md`, origin/main):
  - ch.3 Stance: one `SneakStart` event, additive, no re-assert. A package that forces a stance undoes it. Field-proven.
  - ch.16 Detection: sets `MovementNoiseMult` to a FIXED 0 and `DetectLifeRange` x1.5. This flattens a Thief's "30%
    quieter" perk, so class sneak must not claim ch.16.
  - ch.14 Shout: arbitration only, no drive.
  - ch.18 Attack selection (branch only): `SetAttackMix` with `powerShareMax` and `powerShareMin`, plus the stamina rule
    on every Normal deny (`FACETS.md:2423-2522` on the branch). Floor threshold 1 per marth's ruling.
  - ch.19 Travel: "combat interrupts and cancels the movement". It cannot be the mid-fight disengage.
  - ch.20 Target pin: pins only the CLAIMANT, among its group's targets. It never touches detection.
  - ch.23 Pursuit leash and ch.24 Combat approach (centre the combat area on a REFERENCE X with radius R, in combat gait).
    ch.24 has no position mode and no "run" mode.
  - ch.25 Cast guard. `SenseActor` (omnidirectional, main thread only). `GetLineOfSight`. `FindEmptySpace`.
  - **Harbinger has no combat-style facet, no hit or health-damage seat, no projectile seat, and no write of a FOE's
    detection of an actor.** `RequestDetectionLevel` is resolved but deliberately unused
    (`ADDRESS-TABLE-2026-09-15.md:371/393`).
- **File size**: `Scheduler.cpp` is 2271 lines and `Board.cpp` 2069, both past the ~1500 plan mark, and `Board_FieldKit.cpp`
  is 1499. Per CLAUDE.md, every new mechanism below is a NEW FILE in its subsystem. A brief that must edit Scheduler or
  Board proposes the split as its own round and never does it inline.
- **Lockpicking already exists for chests** (`logistics/Lockpick.cpp`, 1141 lines, LP-M1, with ownership and
  `IsOffLimits` gates in `Admit()`). Doors are LP-M2, still open.

Tier key (CLAUDE.md cost table): **A** = engine seat / ABI / co-save / threading / TU split, Opus authors, full Opus
review until nothing is above SEV-3. **B** = logic inside an existing mechanism, sonnet-coder authors, one Opus pass.
**C** = docs / strings / INI, sonnet-chore or haiku-coder, mechanical proof.
Effort is 1-5. Repos: **MFO base** (MFO.dll and MFO.esp, works with the class add-on absent), **Class add-on**
(MFO.dll subsystem `native/classes/` + the generated `MFO_Classes.esl`, active only when the ESL is present),
**Harbinger** (APMF.dll).

---

## A. Task cards

### A1. Base MFO gambits (repo: MFO base; all work with the class tree OFF, ruling 2026-10-27)

**G1. `act.sneak` (enter and hold sneak), plus always-on sneak for sneak classes**
- What: an appended combat AND logistics action that files a Harbinger ch.3 Stance claim and holds it while the rule
  stands. Sneak classes (Thief, Master Thief, Nightingale, Listener) get it as an always-true default row ("sneak
  regardless of player action"). The re-assert is MFO's: ch.3 does not re-assert, so if `IsSneaking` is false while
  the claim is live, MFO releases and re-files with a throttle. ch.16 is NOT used (it would erase the Thief's perk).
- Classes: Thief, Master Thief, Nightingale, Listener, plus any user row.
- Depends: none (ch.3 is field-proven, ABI 1).
- Tier B, effort 2. **Field session: yes.** Does the combat behaviour tree stand an NPC up mid-fight? ch.3's own "Check"
  says a forcing package can undo it.
- Feasibility: FEASIBLE out of combat (FACETS ch.3 "Field-proven"). In combat it is UNOBSERVED, so principle 5 applies:
  a passive probe run comes first.

**G2. Hide-when-unseen: an unseen sneaking follower is HIDDEN at once (D&D rule)**
- What: when a sneaking follower is not sensed by a foe, that foe stops knowing where it is now, with no
  forget-timer. The truth test is MFO's (`SenseActor` per foe, `kAware_NoHearing` optional, sight ray). The ENFORCEMENT is a
  foe-side detection write: the foe's combat-target record for the follower drops to "lost", which is the state a sneak
  attack needs.
- Classes: all sneak classes. Also the base of C14 (the stealth entry) and H10.
- Depends: G1, and a NEW Harbinger facet ("H-DETECT": declare that actor X is unsensed by foe Y).
- Tier **A** (new Harbinger seat), effort 5. **Field: yes.**
- Feasibility: **NEEDS-RE.** No Harbinger facet writes a foe's detection. ch.20 explicitly keeps the group's knowledge
  ("held engagement", FACETS ch.20 Lifetime). Research targets: the `CombatTarget` record in `CombatGroup->targets` (the
  +0xA6 flags ch.20 already verified, CHANNEL-MAP row 20), the detection-state update that feeds it, and whether
  `RequestDetectionLevel` (id 36748, resolved, unused) is the input to gate. Rule: gate the INPUT the engine reads (the
  foe's detection result for this pair), never patch the output (memory gate-the-source). Deny-completeness: every
  other path that re-acquires (noise, hit by the follower, engaged sense) must be listed.

**G3. Disengage to out-of-sight, then sneak**
- What: a sneak class that is detected picks a point that no engaged foe can see, moves there with H3 (regular movement),
  and re-enters sneak (G1). G2 then hides it.
- Classes: Thief line, Nightingale, Listener.
- Depends: H3, G1, G2, and a concealment-point query (a Harbinger read-only query: candidate points from
  `FindEmptySpace` filtered by multi-viewer `GetLineOfSight`, main thread, budgeted).
- Tier B in MFO, plus a Harbinger query that is tier B (a new read-only query, no seat). Effort 3. **Field: yes.**
- Feasibility: FEASIBLE once H3 exists. Both queries are built (FACETS "Space queries", "Line of sight").

**G4. D&D flanking: T3+ sneak classes deal half the sneak-attack bonus on an engaged foe, even when detected**
- What: when a foe's current combat target is an ally of the follower (flanked / engaged), the T3+ sneak class's hits
  deal `1 + 0.5 x (sneakMult - 1)` of base damage. The engine's sneak multiplier applies only to an UNDETECTED attacker,
  so `ModSneakAttackMult` cannot carry it. Shape: MFO (worker read: `FoeTargets(foe, ally)`, `Evaluator.cpp:60`) toggles a
  per-reference marker on that foe (a scriptless keyword MGEF, MFO's own record), and the class perk carries
  `ModAttackDamage` conditioned on the subject's class faction rank AND the target's `HasMagicEffectKeyword MFOC_Flanked`.
  The multiplier is read from the attacker's own sneak multiplier through `HandleEntryPoint(kModSneakAttackMult)` (the
  `Summon.cpp:106` road), never a constant.
- Classes: Master Thief (its smaller effect), Nightingale, Listener.
- Depends: C6 / C7 (perk carriers), a marker-apply road (see H-MARK below, the same road).
- Tier B, effort 3. **Field: yes.**
- Feasibility: LIKELY. `ModAttackDamage` is graded E on NPCs (`MFO_GenerateESP.py:1704` table). Applying the marker must
  be a non-hostile, unanimated, no-magnitude effect. That is not a player-visible action, so "animations required" does
  not apply. Confirm with marth only if he objects.

**G5. `cond.self_has_summon` (and its negation "no summon active")**
- What: an appended self gate, true when the follower has at least one live commanded actor (optionally "at the limit").
  Read from a worker-safe mirror that `Summon.cpp` refreshes on the main thread (it already computes `live` at `:242`),
  in the `SelfStatus` shape (1 s refresh, 5 s trust).
- Classes: Conjurer, Necromancer, Sorcerer ("full summons" go-in rule), Arch-Mage, Sorcerer's passive smaller effect.
- Depends: none. Tier B, effort 2. Field: light (one run).
- Feasibility: FEASIBLE, `Summon.cpp:230-261`.

**G6. Foe selector "foe attacking my summon"**
- What: an appended foe selector in `PickFoe`'s group scan: the nearest foe whose combat target is one of this follower's
  commanded actors. The commanded handles come from G5's mirror. `FoeTargets` (`Evaluator.cpp:60`) does the test.
- Classes: Conjurer, Necromancer, Sorcerer, Arch-Mage.
- Depends: G5. Tier B, effort 2. Field: light.
- Feasibility: FEASIBLE.

**G7. Corpse target / reanimate**
- What: an appended selector `cond.corpse_within` (param = units) that picks a dead, non-ally, reanimatable humanoid
  corpse in range. The rule's target is the corpse, and `act.cast_target` casts a Reanimate spell at it through ch.8b.
  The spell's own level cap is checked by G10 (reanimate spells carry a level condition).
- Classes: Necromancer, Arch-Mage (Necromancer parent).
- Depends: G10. Tier B, effort 3. **Field: yes.**
- Feasibility: **NEEDS-RE (small).** Does ch.8b accept a DEAD target? The 0x0A / 0x06 seats target a live actor
  (CHANNEL-MAP row 8b). Read the `kIntent_Cast` refusals in FACETS ch.8b, and the engine's reanimate aim path
  (`ReanimateEffect` is already handled in `Summon.cpp:43`). If ch.8b refuses dead targets, it needs a Harbinger tier-A
  extension.

**G8. `act.shout` + a Harbinger shout DRIVE**
- What: an appended action with param = a shout form. MFO equips the shout (`EquipShout`, Posted) and files ch.14. A NEW
  Harbinger drive makes the AI shout NOW at the rule's target, in the shape of ch.8b for spells (activate the AI's own
  shout path, never a forced effect).
- Classes: Dragonborn (T5) only, plus any user row.
- Depends: the Harbinger shout drive.
- Tier **A** (Harbinger seat), effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE.** ch.14 is arbitration only (FACETS ch.14). Research the combat shout decision: the shout
  combat-inventory item's score, the recovery gate, and which of ch.7's 70 leaves fires a shout.
  `ModShoutOK` is D on NPCs, so no perk may gate it.

**G9. Per-rule gambit cooldown**
- What: a per-row "cooldown seconds" field (0 = none). The evaluator skips a rule whose own last-FIRE time (a NEW
  evaluator-side timestamp, not the display-only `lastFiredAt`) is within its cooldown, and falls through. Board: a
  cooldown cell on the row. Warrior and Champion defaults set 10 s on their power-attack rules.
- Classes: Warrior, Champion, Living Legend (Champion mode), Grand Battlemage. Usable on every rule.
- Depends: none. **Tier A (co-save: FLWR v5 -> v6, a reader for every shipped version forever, #12)**, effort 3.
  Field: light.
- Feasibility: FEASIBLE. Verified that no cooldown exists (`State.h:28-53`). The runtime map is keyed by rule uid
  (runtime-only, `NextRuleUID`, `State.h:23`). Saving it is not needed, because a cooldown that resets on load is the
  documented contract. Only the configured seconds are serialized. Downgrade: an older DLL ignores the trailing field
  only if it is placed per the v6 reader gate. The author must prove the v5 reader skips cleanly, or document a
  one-way upgrade.

**G10. "Spell will work" level skip**
- What: before a spell rule FIRES, compute whether the spell can affect the chosen target. If it cannot, the rule SKIPS
  (falls through, `lastFailReason` names it) and never fires. Two checks:
  (a) level cap: for level-capped archetypes (Calm, Frenzy, Demoralize/Fear, Rally, Turn Undead, Paralysis when the list
  levels it, Reanimate), effective max level = the effect magnitude after the caster's `kModSpellMagnitude` entry point
  (`HandleEntryPoint`, the `Summon.cpp:106` road) plus the class passives that add levels, compared against the target's level;
  (b) target conditions: evaluate the effect's own CTDA conditions with (caster, target), so "not undead" style immunity
  is caught.
- Classes: every class (base rule). Warlock, Wizard, Nightblade, Witchhunter and Listener most.
- Depends: none for (a). (b) needs the condition check run on the main thread (mirror), because `TESCondition` reads
  live actor state.
- Tier B, effort 3. **Field: yes** (a known capped spell against a higher-level foe).
- Feasibility: (a) FEASIBLE, with the archetype table already in `cast/Archetype.cpp:88-124`. (b) LIKELY. New file
  `cast/WillWork.cpp` (do not grow `SpellSupport.cpp`).

**G11. Ward on hostile charge**
- What: an appended condition "a hostile spell is being charged at me" (a foe's hand caster in Charging or Ready state
  whose desired target is this follower, or an aimed projectile spell whose line from the foe passes the follower). An
  action holds a ward ONLY for that charge, then releases.
- Classes: Wizard, Arch-Mage, plus any user row.
- Depends: a foe-caster mirror (main thread read of each engaged foe's `ActorMagicCaster` state, Posted, in the
  SelfStatus shape), and ch.8b on a Ward spell (the Ward row exists in the seat list, CHANNEL-MAP row 8b).
- Tier B MFO, effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE (small):** which `MagicCaster` field holds the foe's desired target while charging (verify
  against the disassembled 1.6.1170 binary, never the CommonLib header, principle 6). Also the release timing of a held
  concentration ward under ch.8b.

**G12. Party-healthy threshold: `cond.party_hp_pct_above`**
- What: an appended gate, true when the player and every active follower in `g_sharedRadius` are at or above X% health
  (the `PickAlly` candidate set, MAP.md "ALLY STATE SELECTORS"). Plus its use as the Templar/Scout heal thresholds at 70.
- Classes: Templar, Scout, Ebony Warrior, Saint (go-in rules + heal thresholds).
- Depends: none. Tier B, effort 1. Field: light.
- Feasibility: FEASIBLE (`Pct` helpers, `Vocabulary.h:214` note in MAP).

**G13. Steal, with the crime pre-check**
- What: a logistics action `act.steal`: the loot selection as a superset. It also takes OWNED loose items, but only when
  the pre-check proves no witness. Pre-check, run before every take: every loaded, live, non-teammate NPC within hearing
  range passes `SenseActor` with all senses = 0, the item's owner is not in line of sight, and the follower is sneaking
  (G1). Any sense hit means the take does not happen (marth 2026-11-02: never a refund after the fact). Pickpocket is
  cut (`ModPickpocketChance` and `CanPickpocketEquippedItem` are D).
- Classes: Thief, Master Thief, Nightingale, Listener (class defaults). Works with the tree OFF.
- Depends: G1, the loot selection (`logistics/LootScan.cpp` / `LootTake.cpp`), and `CrimeProbe` (`bCrimeGoldProbe`) as the
  evidence tool.
- Tier B, effort 4. **Field: yes** (with the crime probe on, LoreRim trespass = 500 g).
- Feasibility: LIKELY. `SenseActor` is the conservative test (omnidirectional), but the engine's own witness logic
  differs, so a field run with the crime probe must show zero bounties. Absorbs memory `rogue-class-and-steal-gambit-design`.

**G14. Pick nearby locks gambit (ClickUp 86e3t1gg0)**
- What: a logistics action that lets the existing chest lockpicking (`logistics/Lockpick.cpp`, LP-M1) seek nearby locked
  chests on its own. Town locked doors stay blanket-blacklisted (crime ruling). Owned locks go through the same
  pre-check as G13.
- Classes: Thief line default. Works with the tree OFF.
- Depends: G13's pre-check. Tier B, effort 2. Field: yes.
- Feasibility: FEASIBLE. `Admit()` already has the ownership and `IsOffLimits` gates.

**G15. Class-only gambit availability gate (Mark, Shadowcloak, ...)**
- What: the action lives in base MFO (gambits are base, marth), but the class add-on can mark an opcode "class-gated":
  the Board picker hides it, and the evaluator fails closed unless the add-on says the follower's equipped class unlocks
  it. With the add-on absent, a gated opcode is simply unavailable. A saved row stays saved and reads as disabled.
- Classes: Ranger, Jaqspur (Mark), Nightingale (Shadowcloak), Dragonborn.
- Depends: C1. Tier B, effort 1.

**G16. Off-class weapon rule (ClickUp 1dfcuabcn02)**
- What: with the tree ON, an off-class weapon (for example a two-hander on an Archer, who may hold only one-handed) is never
  equipped unless the follower would otherwise be unarmed. Then the player gets a notification and the follower's damage with it
  is halved. Enforcement: equip authority (`logistics/EquipAuthority.cpp`, ch.17 per-item deny, ABI 33). Halving:
  `ModAttackDamage x0.5` on a class perk, conditioned on a per-reference faction rank `MFOC_OffClassWeapon` that MFO sets
  while the fallback weapon is held.
- Classes: every class except Dragonborn.
- Depends: C2 (class weapon sets), C6. Tier B, effort 3. Field: yes.

**G17. Bound weapons (ClickUp 86e3t1gfx), Sorcerer blocker**
- What: diagnose the field bug ("bound weapons don't work", LoreRim). The note on the task says ch.17 enforce-all refuses
  an off-set BoundItem equip unless the bound weapon form is declared. Then make a bound weapon count as the melee
  weapon for equip authority and the Sorcerer go-in rule.
- Classes: Sorcerer, Grand Battlemage.
- Depends: field evidence in `Projects/_evidence/2026-10-17-lorerim/`, diagnosed by Opus (field-diagnosis rule 4).
- Tier B (possibly a Harbinger ch.17 declaration change, B), effort 3. **Field: yes.**

**G18. Per-gambit summon count (ClickUp 86e3e89w7)**
- What: as specced on the task. The effective cap = min(setting, the engine limit), and liveness counts against the cap.
  It interacts with the Necromancer's +1 passive (`ModCommandedActorLimit` is read by MFO's own road, `Summon.cpp:106-113`).
- Classes: Conjurer, Necromancer, Sorcerer.
- Depends: G5. Tier B, effort 2. Field: yes.

### A2. Harbinger pieces (repo: Harbinger, unless noted)

**H1. ch.18 attack selection: close the review and merge**
- What: `feat/apmf-ch18-attack-select` at `c91471c` (ABI v34, round-2 closing; backlog B258-B265). It gives every melee
  package its power cap (Knight 0.25, Mercenary/Blademaster 0.15, Great Knight 0.20, Listener 0.25) and floor (Warrior and
  Champion 0.40, with the stamina fallback marth required).
- Classes: every T2+ melee and hybrid package, and Grand Battlemage's AI.
- Tier A (already authored and reviewed), remaining effort 1. **Field: yes** (the probe criteria in FACETS ch.18 "Check").
- Feasibility: BUILT on the branch.

**H2. "Less likely to be targeted" (foe-side target weighting)**
- What: a new intent, "aversion": the claimant declares a weight in (0,1). When a foe's target selection lands on it,
  Harbinger lowers its standing among that foe's candidates, so another valid target is picked instead with that
  probability. The intent never invents a target that is not already in the foe's group targets.
- Classes: Conjurer, Healer (T2), Necromancer (stronger), Priest, Arch-Mage, Saint.
- Depends: the vfunc 0x06 `SelectTarget` seat that ch.20 already owns, on both selector vtables (CHANNEL-MAP row 20).
- Tier **A**, effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE.** First check for a vanilla lever, as marth asked. No perk entry point in the 92-row verdict
  table lowers aggro, and no actor value is known to feed the selector's score. Then find the selector's SCORE input
  (the clean gate is weighting the input score, not replacing the output answer, memory gate-the-source). Replacing the
  answer for an actor that does not claim anything is a cross-actor deny, so the deny-completeness question is "who else
  writes a foe's target" (scripts via `SetTarget`, ch.20 pins of other clients).

**H3. Disengage / reposition drive with regular movement mid-fight**
- What: "thief and mage classes both can disengage, and use regular movement in combat to reposition faster". The
  follower turns and RUNS to a point (not a combat backpedal), then resumes fighting. Today ch.24 moves toward a
  REFERENCE in combat gait, and ch.19 ends on combat. Shape: either extend ch.24 with a position mode plus a run gait
  bit, or a new intent that, for the leg's duration, denies the Action half (ch.7 offense) and the pursuit leaves (ch.23
  seats) while the area is recentred.
- Classes: Thief, Evoker, Conjurer, Healer, Warlock and every line built from them (Master Thief, Wizard, Necromancer,
  Priest, Spellsword, Sorcerer, Templar, Nightblade, Witchhunter, Scout and all T4 built from them).
- Depends: ch.24 (`CombatApproach.cpp`), ch.7, ch.23 seats.
- Tier **A**, effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE.** Does a recentred combat area far from the actor produce a facing-forward run, or the
  engine's Return To Combat Area path in combat gait (FACETS ch.24 Key Concept 17)? The probe answers it before building.
  MFO picks the point: away from the foes, within the leash, using `FindEmptySpace` and, for G3, out of sight.

**H4. Spell range follows the equipped spell (MFO + ch.24)**
- What: MFO derives the desired distance from the equipped spell: touch or cone/concentration means close to the spell's
  range x0.8; aimed or target-actor means near the furthest range at which it works x0.9; a target-location spell means
  its range. Closing uses ch.24 (field-proven, healers already use it) with X = the target and R = the derived range.
  Keeping back uses H3 when the follower is inside the minimum.
- Classes: every mage class and line, hybrids in their spell phase.
- Depends: ch.24 (exists), H3 for the back-off half.
- Tier B (MFO), effort 3. **Field: yes.**
- Feasibility: closing is FEASIBLE (ch.24 field-proven). Backing off depends on H3. The engine's own
  MaintainOptimalRange leaf may already respect spell range (vanilla mages keep range), so observe first.

**H5. Ranged miss detection and correction (Archer; Ranger at most 3 misses in 10; Jaqspur at most 1 in 10, never-miss accepted -- marth 11-10)**
- What: detection counts arrows or bolts that the follower launched at its target and that did not hit it (launch vs
  `TESHitEvent` on the target, within a time window). Correction adjusts the launch so the shot lands: aim with lead on a
  moving target, at the projectile's real speed and gravity. Budgets over a sliding window of 10 shots: Ranger at most 3 misses, Jaqspur at most 1 (or never).
- Classes: Archer (detection + correction), Ranger (3-in-10 budget), Jaqspur (1-in-10, or never misses; marth OK with either), Scout, Witchhunter.
- Depends: a NEW projectile launch seat (memory `ranged-accuracy-needs-projectile-hook`: `WeaponFireProjectile` /
  `Projectile::Launch`).
- Tier **A**, effort 5. **Field: yes** (several).
- Feasibility: **NEEDS-RE.** No projectile seat exists in Harbinger (FACETS index). Detection alone is FEASIBLE today
  through a `TESHitEvent` sink plus a launch count, which could land first as a passive probe (principle 5).

**H6. Illusion immunity lift for the Witchhunter**
- What: for the Witchhunter's own Illusion casts, the targets' undead / Daedra / automaton immunity conditions do not
  block the effect (marth approved). The level bonus (+18 on undead, Daedra and casters) is a plain perk
  (`ModSpellMagnitude` conditioned on the target keyword), but a level bonus does not lift a condition.
- Classes: Witchhunter, Nightingale (Witchhunter parent at full).
- Tier **A**, effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE.** A perk cannot override an effect's own CTDA. Options to research: (1) a Harbinger seat at the
  effect-condition check when an effect is added to a target (`MagicTarget` add-effect path), scoped to this caster;
  (2) a runtime-created condition-free clone of the spell, made procedurally from the cast spell and never from list
  data (memory no-list-specific-data). It needs persistence of created forms. Option (1) is the "gate the source" shape.

**H7. Arch-Mage: two SIMULTANEOUS casts of different spells**
- Classes: Arch-Mage.
- Tier **A**, effort 5. **Field: yes.**
- Feasibility: **NEEDS-RE (deep, a Fable escalation candidate if Opus stalls).** The cast latch is actor-wide and
  hand-blind (memory `cast-latch-is-actor-wide-hand-blind`), and ch.8a L/R is arbitration-only. It needs a design pass
  over the per-hand casters, the combat magic caster selection, and both hands' charge states at once.

**H8. Living Legend: 40% auto-block of hostile attacks while it is attacking (class add-on, no Harbinger)**
- What: a perk entry `ModIncomingDamage x0` (or x the block fraction) conditioned on the subject `IsAttacking == 1` and
  `GetRandomPercent < 40`, plus a block hit sound or spark if feasible.
- Classes: Living Legend.
- Tier B, effort 2. **Field: yes.**
- Feasibility: LIKELY. `ModIncomingDamage` is E. Whether `GetRandomPercent` re-rolls per hit inside a perk entry
  condition is to be observed. Ask marth whether "automatically blocked" needs a visible block reaction
  (animations-required rule). Recommendation: a sound/spark only, not a forced block animation.

**H9. Ebony Warrior "Guardian": allies near it take 25% less damage, and it takes that share instead**
- What: a cloak ability on the Ebony Warrior marks allies within radius (`MFOC_Guarded` keyword effect). Their perk entry
  `ModIncomingDamage x0.75` conditions on it (class add-on, B). The TRANSFER needs the damage amount, so Harbinger
  needs a passive health-damage observe seat (`Actor` HandleHealthDamage, slot to be verified on the disassembly) that
  reports (victim, attacker, amount). MFO then applies the same share to the Ebony Warrior (Posted `DamageActorValue`).
- Classes: Ebony Warrior.
- Tier **A** (new seat), effort 4. **Field: yes.**
- Feasibility: **NEEDS-RE** for the seat (slot and signature from the 1.6.1170 and 1.5.97 binaries, `VerifiedAddresses.h`
  row). The reduction half is LIKELY (cloak ally filter, draft G-M5).

**H10. Nightingale "Shadowcloak": once per 30 s in a fight, undetected for 3 s, re-open with a sneak attack**
- What: the vanilla lever exists and is lore-exact: the Nightingale "Shadowcloak of Nocturnal" is an Invisibility power.
  A 3 s Invisibility-archetype ability, applied by MFO every 30 s from a fight timer, dispels on attack, and the attack out
  of it is a sneak attack.
- Classes: Nightingale.
- Depends: C6. Tier B, effort 2. **Field: yes.**
- Feasibility: LIKELY. Observe that engaged foes really drop detection of an invisible NPC within 3 s. If they do not,
  it falls back to G2's H-DETECT seat (tier A).

**H11. Grand Battlemage: each power attack releases the off-hand spell at the struck foe**
- What: a `TESHitEvent` sink (power-attack flag, aggressor = GB) Posts a cast of the spell in the GB's off hand at the
  struck foe, free (no magicka), from the off-hand caster (`cast/Direct.cpp` road, the canonical delivery per memory
  `cast-delivery-direct-force-canonical`; the power attack is the animation). Splash guard: never release an area spell
  if the party is in its radius (ch.25 line-of-fire / `FindHostilesInSpace` for the friendly count). AI awareness: when
  the off hand holds a spell the foe is weak to, raise the ch.18 power floor for the GB (H1).
- Classes: Grand Battlemage.
- Depends: H1, ch.25. Tier B, effort 3. **Field: yes.**
- Feasibility: LIKELY. The hit-event power-attack flag on NPC hits is to be observed.

**H-MARK. Mark (Ranger-only gambit): the whole party targets the foe for 10 s, and it takes +10% damage**
- What: `act.mark`, class-gated (G15). MFO files ch.20 `kIntent_TargetPin` for EVERY managed follower on the marked foe
  for 10 s (ch.20 pins only the claimant, so the fan-out is MFO's, `Targeting.cpp` route). The +10% is applied by the
  Ranger's NEXT HIT (`ApplyCombatHitSpell`, E, gated on a "mark pending" faction rank), which puts a 10 s `MFOC_Marked`
  keyword effect on the foe. Every party member's class perk (and a player perk granted once by the add-on) carries
  `ModAttackDamage x1.1` conditioned on the target `HasMagicEffectKeyword MFOC_Marked`. The mark rides an attack, so the
  animation rule holds.
- Classes: Ranger, Jaqspur.
- Depends: ch.20 (field-proven), C6, C7, G15. Tier B, effort 3. **Field: yes.**
- Feasibility: FEASIBLE / LIKELY. The player is not pinned (no claimant). Ask only if marth wants the player covered by
  the damage bonus.

**H12. Combat-style facet (strict adherence override): NOT REQUIRED for the build**
- MFO's own `ApplyTick` already owns 0x38 during combat (`CombatStyle.cpp:138`). With the tree ON, writing the class
  variant there IS strict adherence. The Harbinger slider (draft G-H6) is only needed if a second client fights over
  0x38. That is deferred, with no card in the milestones.

**H13. Evasion / dodge facet: NOT REQUIRED**
- marth's dodge ruling: on lists with Ultimate NPC Dodging, the dodge score already reads CSTY avoidThreat / fallback /
  circle / strafe (`dodge-research.md`), so the class CSTYs drive it. Detecting UND is only a log line. No card.

### A3. Class add-on machinery (repo: Class add-on = `native/classes/` + `MFO_Classes.esl`)

**C1. Subsystem skeleton, presence, tree toggle, strict adherence**
- What: `native/classes/` with ONE public header (`classes/Classes.h`) and one internal header. Presence = the ESL's
  manifest keyword (the `_MFOAddonManifest` precedent, MAP.md progression/Manifest.cpp `:5267`). INI keys
  `bClassTree` (default ON) and `bClassStrict` (default ON with the tree). OFF = today's three classes exactly. Works with
  or without Progression.
- Tier B, effort 2.

**C2. Class data model (35 classes)**
- What: a compiled table: id slug (the page's db doc ids), tier, parents, unlock rule (>= 2 masteries of the previous tier;
  hidden T5 = 3 distinct T4), allowed weapon set (Archer one-handed only, dagger exception), default gambit set (base
  opcodes), passive ids, CSTY ids per stance, ch.18 mix, leash, and go-in rule id. Class data is our own design data, not
  list data, so it is allowed.
- Single source of truth with the Gambit-tab class: T1 = `combatClassOverride` (`State.h:82`, frozen ordinals). The
  equipped T2+ class's root T1 drives the existing stance and Auto logic (`logistics/ClassResolve.cpp`).
- Tier B, effort 3.

**C3. The add-on's own co-save record (`MCLS` v1)**
- What: a NEW record in MFO.dll (not PRGN, per the architecture ruling): per follower = equipped class, per-class XP and
  level, mastered bitset, unlocked bitset, the Thief/Master Thief fight counter. `ResetAllState` clears it after
  `StopPump()` (`Serialization.cpp:680/686`). Recommendation: when the ESL is absent, the DLL keeps the loaded blob and
  re-writes it unchanged, so removing the ESL for one session does not wipe progress (see question C4).
- **Tier A (co-save)**, effort 3. Field: a save/load round-trip run.
- Feasibility: FEASIBLE, with the PGIV/ERND precedent (MAP.md sections for PlayerGiven, Errand).

**C4. Class XP: fights won while equipped, plus heal and summon credit**
- What: a fight won = the follower's combat ends with a hostile killed by the party and the party not wiped (Scheduler's
  combat-end edge plus a `TESDeathEvent` killer = the follower or its commanded actor). Credit: kills and assists, heals
  (observed by `cast/HealObs.cpp`), and summon uptime (`Summon.cpp`). The XP curve is sized from the ~3 h / 6 h / 10 h
  targets, and the rate is MCM-tunable.
- Tier B, effort 3. **Field: yes** (rate tuning). New file `classes/Xp.cpp` (Scheduler.cpp is at 2271, so hook only via a
  one-line call).

**C5. Mastery = level cap, levels, free switching, Auto picks from stats**
- What: class level 1..cap; mastery when the cap is hit. **Caps by tier (marth 11-10): T1 10, T2 15, T3 20, T4 25.** Switching between unlocked classes is free and instant. With
  strict adherence, Auto resolves the class from the follower's stats (the `ClassResolve.cpp` precedent).
- Tier B, effort 2.

**C6. Passive carriers: equipped full, mastered 1/4, other mastered passives at 1/4, T4 parents at full**
- **marth 11-10: the T3 STRONG effect and the T4 UNIQUE scale linearly with class level, 25% at L1 to 100% at the cap (T3 20, T4 25); the mastered-unequipped 1/4 is of the full value. T1/T2 passives and the T3 smaller effect stay flat.** Carrier design must carry a per-level magnitude (rank = level on the class faction, or a level-driven value), not just ranks 2/1. Decide the cheapest proven road in C6 (perk entry count grows with ranks; prefer one entry whose value reads the level).
- What: ONE perk per class passive in the ESL, granted ONCE to each managed follower when the add-on first sees it
  (`TESNPC::AddPerk` writes the base record, `ProgProbe.cpp:408-409`, so it is never churned on switches). Each perk has
  two entry sets conditioned on a per-reference faction rank `MFOC_P_<class>`: rank 2 = full, rank 1 = quarter. MFO sets
  ranks from MCLS: the equipped class = 2, its T4 parents = 2, every other mastered class = 1, else 0. The same passive is
  one faction, so it never doubles (max rank wins). Actor-value parts (Mercenary stamina, Scout speed) are two ability
  spells per class (full / quarter), added per reference. Everything is re-applied at load from MCLS.
- Tier B, effort 4. Field: yes (verify each LIKELY carrier).
- Feasibility: FEASIBLE for entry points graded E (`MFO_GenerateESP.py:1704`). Open LIKELY carriers to observe:
  Sorcerer summon duration, Necromancer thrall health (ability on commanded actors at spawn), Wizard / Nightblade
  "controlled foe" target keyword, the Priest / Saint cloaks, Templar "always silver" (own entry on undead / Unholy target
  keywords, because the vanilla silver bonus is player-only), Listener "up to own level" (**NEEDS-RE: no entry function
  sets magnitude = level; it may need G10's road plus a magnitude floor at cast time**), Jaqspur draw speed (**NEEDS-RE:
  which AV or entry point drives NPC bow draw speed**), Blademaster "all stamina costs -10%" (partial, the power-attack
  and bash entries only).

**C7. ESL generation (`MFO_Classes.esl` from `MFO_GenerateESP.py`)**
- What: `make_classes_esl()` beside `make_progression_esl` (`:1817`): manifest keyword, PERK records (NEW byte shape: entry
  points + CTDA on faction rank / keywords / IsAttacking / GetRandomPercent), FACT records (passive ranks,
  `MFOC_OffClassWeapon`, mark-pending), abilities (SPEL + MGEF), marker MGEFs (`MFOC_Marked`, `MFOC_Flanked`, `MFOC_Guarded`),
  class CSTYs per stance (melee / ranged / cast + dual twins, ~70, generated from C2's table with `_csty_record` `:1201`),
  GLOBs for MCM values. Forms.h FormIDs stay frozen (the add-on has its own local ids). Byte audit with
  `tools/esp_inspect.py` against vanilla PERK dumps.
- Tier B (new record shapes, mechanically audited), effort 4. Field: the load test.

**C8. Class combat packages (CSTY per stance + ch.18 mixes + leash)**
- What: `CombatStyle::ApplyTick` writes the equipped class's variant for the stance instead of `MFO_*Style` (tree ON, T2+).
  T1 stays on today's styles ("vanilla for its role"). Per-class ch.18 `SetAttackMix` (H1) and the ch.23 leash length.
  Packages never choose targets and never swap weapons (rulings).
- Tier B, effort 4. **Field: yes** (one session per tier).
- Depends: H1 merged, C7. The `Stance` ordinals stay frozen.

**C9. Hybrid "go in" rules**
- What: a per-class predicate decides IN (melee range) vs RANGE each tick. Spellsword = a health / magicka / range bias;
  Sorcerer = high health AND full summons (G5); Templar = all party >= 70% (G12) or no magicka; Nightblade = the target is
  controlled, or none can be controlled (G10's "will work"); Witchhunter = never; Scout = ask (question 5). Output: the
  movement goal (ch.24 close-in or H3 back off). The weapon swap stays the gambit's job.
- Tier B, effort 3. **Field: yes.**
- Depends: H3, H4, G5, G10, G12.

**C10. Classes GUI tab + the class-info combat-GUI toggle (ClickUp 86e3dhaba)**
- What: a new Board tab (new file `Board_Classes.cpp`, because Board.cpp is 2069 lines and Board_FieldKit.cpp 1499):
  the tree (T5 hidden until unlocked), XP bars, mastery marks, equip and switch, passive text (equipped / 1/4), and full
  gamepad navigation (family rule). The tab hides when the add-on is absent. The 86e3dhaba toggle shows the class, level
  and XP in the combat GUI (OFF by default; the INI key name is frozen once shipped).
- Tier B, effort 4.

**C11. MCM: tree toggle, strict adherence, XP rate, class-info toggle, every /s value**
- What: the add-on MCM (the `prog_mcm_config` `:1990` precedent, MCM Helper schema rules: `minMcmVersion`, enums inside
  `valueOptions`). Per marth's /s ruling: Priest sun 2/s, Saint +30 HP/s and 30/s on undead are INI/MCM values, with defaults
  scaled at runtime (never per-list tables). MFO patches the abilities' effect magnitudes at data load and on MCM change.
- Tier C (keys) + B (the runtime magnitude patch), effort 2.

**C12. Default gambit sets per class**
- What: on class equip, apply the class's default base-gambit set (rulings list them per class). The user can edit it.
  See question 3 for the overwrite policy.
- Tier B, effort 2.
- **STATUS 2026-11-11: BUILT with MCLS v2 on `feat/mcls-v2` (tier A, co-save; NOT merged, NOT field-run).** What it does:
  - `Classes::EquipClass` (`native/classes/Defaults.cpp`) is the equip road for C10. First equip of a class: the COMBAT table becomes
    the class defaults (Q8, Q59). Later equips: his own combat AND logistics tables for that class come back (Q8, Q29). The tables
    he had with classes OFF are kept too and come back with `UnequipClass`. `ResetClassTables` is the reset action for C10's button.
  - Defaults: T2 = the applied `DefaultGambit` rows; T1 = today's base kit (the 2026-11-09 ruling); T3+ = parked, the table is
    carried over. The logistics table has no class default (none exists in the rulings), so first equip and reset leave it alone.
  - Q74: only the top rows that fit the rank's combat slots are laid down; a rank-up adds the next ones while the table is still
    the untouched default prefix.
  - Spell roles: a cast row arrives disabled, the follower's own spell for the role is picked (the strongest he can pay for) and
    the row is enabled. No spell for the role: the row stays disabled for the player to fill.
  - Row data changed: Warrior and Conjurer = the simulator's kept sets, Warlock = its kept prefix order (iaus-sim-v2.md section 27).
    Starting values, they still want the in-game probe.
  - NOT done here (other cards): the board button and the reset button (C10), reading Dragonborn's picked T4 (C8, C10), the
    placeholder rows whose opcodes now exist (G1 sneak, G5 no-summon, G6 foe-hits-my-summon are still `Todo` rows in `ClassTable.cpp`).
  - In-game check (DEV build, `MFO_Classes.esl` loaded, `bClassTree = 1`, `bClassCoSaveSelfTest = 1`):
    1. Load an existing save made BEFORE this build with class state in it. Expect `loaded class state ... (MCLS v1 ...) ... an older
       record: the next save writes it as v2`, the four `MCLS self-test ... PASS` lines and no `FAIL`. Save, load that save: expect
       `MCLS v2` and the same follower count.
    2. Edit a follower's combat and logistics tables on the board so they are recognisable. Set
       `sDevClassEquip = <follower>:knight:1, <follower>:warrior:1, <follower>:knight:1` and load. Expect three
       `[classes] <fid>: ... -> ...` lines: `classes OFF -> knight ... DEFAULTS (first equip)`, `knight -> warrior ... DEFAULTS`,
       `warrior -> knight ... RESTORED`. On the board the combat table is the Knight default (taunt, attack, potion: 3 rows at Rank I).
    3. With a mage follower: `sDevClassEquip = <follower>:healer:1`. Expect `default combat table laid down ... cast row(s) waiting
       for a spell pick`, then one `default row ... casts <spell>` line per role he knows a spell for, or `knows no Heal spell ...
       stays DISABLED`. On the board each cast row shows its spell and is enabled, or is disabled with no spell.
    4. Save and load with the entry removed: the class, the tables and the stored tables are the same (`saved class state ... N combat
       + N logistics + 2 classes-OFF stored table(s)` on save, the same counts on load).
    5. Set `bClassTree = 0`, load, save, set it back to 1, load: the class and every stored table are unchanged.

**C13. Thief stealth entry: guaranteed undetected open 1 fight in 5 (Master Thief 1 in 3), the others roll on stats**
- What: a fight counter in MCLS. On a guaranteed fight, the Thief enters the fight hidden (G1 + G2, or H10's invisibility
  lever briefly at engage). On the other fights, its sneak skill rolls against the foes' detection.
- Tier B, effort 2. **Field: yes.** Depends: G1, G2 (or the H10 fallback).

**C14. Dragonborn (hidden T5)**
- What: unlock on 3 distinct T4 masteries. Every mastered passive at full, no class limits (the 1dfcuabcn02 exception),
  all common shouts known (`Actor::AddShout`, resolved at runtime) and gambitable, and the package copies the strongest
  mastered T4 plus a shout score.
- Tier B, effort 2. Depends: G8.

---

## B. Build order (milestones)

Rules that apply to every milestone: one complete brief per agent, at most 3 agents at once, one agent per build tree,
an Opus review per commit at the tier shown, and a Deck play session at the end of each batch.
**[A] = tier A, Opus authors.**

**M0: prep, this week (before Mon 11-16).**
- marth answers part C.
- H1 ch.18: finish the review, merge, field run [A, already authored].
- Read-only RE briefs (Opus, no code), started now because they gate M2: H3 (does a recentred area run or backpedal),
  H2 (target-selector score input), G2 (foe detection input), H5 (projectile launch seat), G7 (ch.8b dead target),
  G11 (the foe caster's desired-target field). Each brief writes its findings into ClickUp 86e3dhbd3.
- One passive probe run on the Deck: G1 sneak in combat, the H10 invisibility drop, the G4 / H8 perk-condition behaviour.

**M1: machinery + tiers 1-2 (marth's goal, from Mon 11-16).**
1. C1 skeleton -> C2 data model -> **C3 MCLS record [A]** (blocks C4, C5, C6).
2. C7 ESL generator (perks, factions, abilities, T1-T2 CSTYs) -> C6 carriers -> C5 mastery and switching -> C4 XP.
3. C8 T2 packages (needs H1 merged and C7).
4. Base gambits used by T2 defaults: **G9 per-rule cooldown [A, FLWR v6]** (Warrior), G1 act.sneak (Thief), G5 + G6
   (Conjurer), G10 level skip (Warlock and all), H4 spell range, closing half (mages).
5. C10 GUI tab + 86e3dhaba, C11 MCM, C12 defaults, G15 class gate, G16 off-class rule (Archer one-handed only).
- Field session M1: T1-T2 all ten classes, a save/load round trip, the XP rate.
- **Gaps that T2 classes need but that are tier-A RE** (H2 for Conjurer and Healer, H3 for Thief and mage disengage,
  H5 for the Archer, G2 for Thief hiding) land in M2 IF marth accepts (question 1). Until then those T2 classes run
  without that one effect, and the log names the missing facet (a documented degrade, not a mask).

**M2: the Harbinger tier-A seats + stealth (parallel tracks, at most 3 agents).**
- Track 1: **H3 disengage drive [A]** -> H4 back-off half -> G3 disengage to out-of-sight.
- Track 2: **G2 / H-DETECT hide-when-unseen [A]** -> C13 stealth entry -> G13 Steal -> G14 pick locks.
- Track 3: **H2 aversion [A]**, then **H5 projectile seat [A]** (detection first as a probe, then correction).
- Field session M2.

**M3: tier 3 (8 pure + 6 hybrids).**
- G4 flanking, G7 reanimate, G11 ward-on-charge, G12 party-healthy, C9 go-in rules (needs H3, H4, G5, G10, G12), H-MARK
  Mark, **H6 Illusion immunity lift [A]**, G17 bound weapons (Sorcerer blocker), G18 summon count (Necromancer), T3 CSTYs and
  passives in C7.
- Field sessions: pure, then hybrids.

**M4: tier 4 uniques.**
- H8 Living Legend auto-block, H10 Shadowcloak, H11 Grand Battlemage release, **H9 Guardian seat [A]**, Jaqspur draw speed
  (after its RE), Saint aura (C11 /s), Listener level floor (after its RE), **H7 Arch-Mage dual cast [A, deep RE, Fable
  escalation candidate]** last.

**M5: tier 5 Dragonborn.** **G8 act.shout + Harbinger shout drive [A]** -> C14.

Blocking graph (short form): H1 -> C8. C3 -> C4 / C5 / C6. C7 -> C6 / C8. G5 -> G6 / G18 / C9. H3 -> G3 / H4-back / C9.
G1 -> G2 -> C13 -> G13 -> G14. G10 -> G7 / C9. G12 -> C9. G15 -> H-MARK. G8 -> C14.

---

## C. Questions only marth can answer (each with a recommendation)

1. **What "M1 = tiers 1-2" must include.** Four T2 effects need tier-A engine research: Archer miss correction (H5),
   Conjurer/Healer "less targeted" (H2), Thief instant hiding (G2), Thief/mage disengage (H3). Ship M1 with T2 running
   without those four, each logged as missing, and land them in M2?
   *Recommended: yes. Nothing is cut, only sequenced. The research starts in M0.*
2. **Per-rule cooldown scope.** A cooldown cell on EVERY gambit row (0 = none), or only on power-attack rules?
   *Recommended: every row. It is one save-format bump either way.*
3. **Class default gambits on a switch.** Replace the follower's combat table, or apply the defaults only the first time a
   class is equipped, plus a "reset to class defaults" button?
   *Recommended: first equip only, plus the reset button. Never overwrite the player's edits silently.*
4. **Class progress when the add-on ESL is removed for a session.** Keep it (the DLL re-saves the record untouched), or
   drop it? *Recommended: keep it.*
5. **Go-in rules still open.** Witchhunter and Scout. *Recommended: Witchhunter never goes in (its package already says
   so). Scout uses the Templar rule (all party >= 70%), else stays ranged and swaps to melee only when out of ammo.*
6. **Visible "auto-block" for Living Legend.** Is a damage negation with a block sound/spark enough, or must a block
   animation play? *Recommended: sound/spark only. A forced block animation would interrupt its own attack.*
7. **Confirm the walkthrough's open follow-ups.** (a) 1/4-when-mastered applies to the T3 two-effect passives; (b) Knight
   = only the 15% block effect; (c) we draft lore names for every T3/T4 passive for you to approve; (d) Shadowcloak =
   the vanilla Invisibility lever (lore: the Shadowcloak of Nocturnal), with the follower visibly cloaked for 3 s.
   *Recommended: yes to all four.*

---

## D. Existing tracked items: absorb, don't duplicate

| Item | Where it lands | Note |
|---|---|---|
| memory `rogue-class-and-steal-gambit-design` | G13 (Steal), G1 (sneak) | The Rogue class itself is superseded by the Thief line. Steal = the loot selection superset, hidden-only, plus the 2026-11-02 crime pre-check. |
| memory `assassin-stealth-update-backlog` | G1-G4, G13, C13 | Item 3 (dual wield vs shield by perks) and item 4 (per-follower MFO toggle) are NOT class work. They stay separate backlog items. |
| ClickUp 1dfcuabcn02 off-class weapons | G16 | Archer one-handed only is its first instance. Dragonborn is the exception. |
| ClickUp 86e3dhaba class-info toggle | C10 | Source of truth changes from "PRGN class level" to the MCLS record (C3). Update the task text when built. |
| ClickUp 86e3bw775 combat-behaviour facet | H1 (ch.18) | ch.18 is its built form. Close 86e3bw775 on the ch.18 merge, or re-scope it to the left-hand context (draft G-H4, Blademaster). |
| ClickUp 86e3t1gg0 pick locks gambit | G14 | Builds on `logistics/Lockpick.cpp` (LP-M1 chests). Doors = LP-M2, with town doors blacklisted. |
| ClickUp 86e3e89w7 per-gambit summon count | G18 | Lands with the Necromancer (M3). Uses G5's mirror. |
| ClickUp 86e3t1gfx bound weapons broken | G17 | Blocks the Sorcerer (M3). Field diagnosis goes to Opus first (rule 4). |
| ClickUp 86e3dhbd3 CLASS UPDATE epic | this plan | Paste parts A-C there as a comment (the coordinator does it; this agent made no ClickUp writes). |

**C5b. Remove the class Auto picker (marth 11-10: "Auto is disabled with class add on")**
- What: delete AutoClassFor / PickAutoClass / ReadStats / kSkillMap + StatProfile sneak/block from classes/Mastery.cpp + Classes.h (merged 2f584aa), and drop backlog MFO-B629 R3. Decide the first-seen class for a follower the add-on has never seen (proposal: its root T1 from today's Auto resolution, applied once, then player-owned). C10 hides/disables the Gambit-tab Auto option PER FOLLOWER while that follower has classes ON (marth 11-10: "Auto is disabled per follower when classes are enabled on that follower"); followers with classes OFF keep today's Auto. Proposed encoding: MCLS equipped == kNoClass = classes OFF (no layout change). So no first-seen class is assigned automatically: a follower starts with classes OFF until the player equips one.
- Tier B, effort 1. Queue: class track, next free slot.

**C2b. Route every class-pick reader through one effective-class accessor (C5b follow-up; text from the C5b Opus review)**
- marth 2026-11-10: "Auto is disabled per follower when classes are enabled on that follower." C5b (b8f287a) made only Logistics::ResolveBaseClass honour this (HMS split, Gear.cpp:550 last-resort role). Every other reader still tests FollowerState::combatClassOverride directly, so a classes-ON follower whose Gambit-tab pick is Auto is still Auto there, and an old explicit pick beats his class.
- Readers: Cast.cpp:118; Gear.cpp:536 (perks and gambits run before the class: a Mage-class follower with a 1H perk gets a melee role) and :570 (log); LootEquipment.cpp:61; EquipAuthority.cpp:1087; Errand.cpp:1585 and :1844; Scheduler.cpp:1623; SwapUp.cpp:103; Economy.cpp:319 and :873-874; Followers.cpp:821 GetBaseClass (and its callers); Followers.cpp:180 (PublishActiveMirror); ClassResolve.cpp:49 (explicit pick before class). Board_FieldKit.cpp:728/820 keeps the RAW value (it edits the pick).
- Fix: add EffectiveClassPick(fid, state) in logistics/ClassResolve.cpp (declared in Logistics.h): Active() and equipped != kNoClass -> that class's rootT1 (Dragonborn per ruling), else combatClassOverride. Route all readers through it. Do NOT write combatClassOverride on class equip (it is the persisted classes-OFF pick). Use an atomic per-follower mirror for any combat-thread reader. C10's SwitchClass must call Followers::RepublishActiveMirror() (also on a bClassTree toggle).
- Rulings needed: Q59, Q60. Inert until something equips a class (C10/C12). Tier B, effort 2.

**C5c. Dev-only class equip for field testing (unblocks C6/H5/C5b Deck tests, 2026-11-10)**
- What: in DEV builds only (the MFST dev self-test build flag), an INI or self-test hook that equips/levels/masters a class on a named follower (e.g. `sDevClassEquip=<follower edid or fid>:<class slug>:<level>[:mastered]`), applied once after load through ActivateClass/ApplyLevel. Never compiled into release builds. Lets the Deck run test C6 carriers and H5 miss budgets before C10 exists.
- Tier B, effort 1. Queue: next class slot (before C2b).

**C6b. Strip class passives on dismiss, restore on rejoin (marth Q57, 11-10)**
- What: on dismissal, Carriers zero the follower's MFOC_P_* ranks and remove the class abilities; on rejoin RefreshCarriers restores them from MCLS. Class levels/mastery untouched. Hook the existing dismiss path (Followers.cpp release) and rejoin/Enroll.
- Tier B, effort 1.

**Post-release quick patch: Warlock/Nightblade foe-only Illusion bonus (marth Q58, 11-10)**
- Ship the Animage-style "+N levels" for release; right after, find a proven foe-only road (target-tab hostility, a MagicInfluence condition, or a DLL-side magnitude cap at cast time), especially if the Deck shows ally buffs (Courage, Rally, Call to Arms) becoming very strong. Field-check buff strength on the Deck.

- MCLS v2 must also store Dragonborn's player-picked T4 combat behaviour (marth Q60, 11-10). DONE 2026-11-11 (`feat/mcls-v2`: `u8 pickedT4`, `Classes::GetPickedT4 / SetPickedT4 / ClearPickedT4`; stored only, C8 / C10 read it).
