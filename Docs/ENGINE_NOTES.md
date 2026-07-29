# MFO — Engine Notes

Engine mechanisms MFO depends on, and how much each is actually trusted.

**Read this before any native work.** But read it knowing what it is: in the
sibling projects `ENGINE_NOTES.md` records mechanisms **proven in-game**, with
dates and symptoms. **MFO has proven nothing in-game.** Writing this file in
that voice would make it exactly the document the family's own doctrine warns
about — *never trust format docs, including this documentation.*

So this is a **research ledger** instead. Every entry carries a status, and
the status is the most important field on it.

| Status | Means | Trust |
|---|---|---|
| **PROVEN (sibling)** | Shipped and field-validated in MRO / MEO / MAO. Cited. | Build on it |
| **RESEARCHED** | Mapped from a primary source (SKSE64 `Actor.psc`, PapyrusUtil, po3, a reference repo) but **never run by anyone in this family** | Design against it; verify before relying |
| **UNKNOWN** | Named, not investigated | Do not plan around |
| **PROVEN (MFO)** | Validated in-game by MFO, with date, game version, and observed symptom | — |

**PROVEN (MFO) — first entries below, from the 2026-07-21 session.** The
promotion protocol is §10.

---

## 0. PROVEN (MFO) — validated in-game

### 0.1 Follower detection via `IsPlayerTeammate` + `ProcessLists`
**2026-07-21, game 1.6.1170, MFO 0.0.1, `custom-modlist`/Requiem.**

Observed: a Bruma Guard set to `setplayerteammate 1` appeared within one 2 s
refresh as `[follower] + 1008B402 Bruma Guard (teammate)`; `setplayerteammate
0` produced `[follower] - 1008B402 (record and Rapport retained)`; re-setting
it re-added the same FormID. Cycled three times, deterministic each time.

Proves: the `highActorHandles` sweep filtered by `IsPlayerTeammate` detects
and un-detects reliably, `ActorHandle` round-trips without a stale resolve,
and dismissal is **non-destructive** — the record persisted in the STORED
list across all three cycles.

### 0.2 Form resolution and the power grant
**Same session.** `[forms] resolved MFO_FieldOrdersPower -> FE08F801` and
`MFO_GrantedSpell -> FE08F802` — note **`FE08`**, i.e. the ESL slot, which
also confirms the TES4 `0x200` flag and the `0x800`–`0xFFF` band are correct
end-to-end. `[setup] granted Field Orders power` fired once and the power was
usable, which proves **SPIT type 3 is right for a castable lesser power**.

### 0.3 The quirk table degrades correctly on an absent plugin
**Same session.** `[follower] quirk table: 0 active, 2 inactive (of 2)` —
Vilja/Tindra are not in this load order, and the absence was logged at info
without a single error. Proves the DYNAMIC_OR_DROP handling: an absent plugin
is normal, not a fault.

### 0.4 `TESSpellCastEvent` fires for a lesser power
**Same session.** Nine `STATE REPORT (power)` blocks, one per cast. Proves
the opener mechanism M7's board depends on.

### 0.5 Rapport crediting works end to end
**2026-07-21, game 1.6.1170, MFO 0.4.1.** Six kills with follower Cosnach
(`000198FA`) present: each logged `+1.0 (standard) -> N`, rapport climbing
1→2→3→4→5→6, exactly one award per kill. `TESDeathEvent`'s double-fire is
correctly guarded (no +2), the follower-shared gate credits, and kills before
recruitment credited nobody (correct). **This is the first time any of Rapport
has executed.** Still untested: the boss/dragon multipliers landing on the
right actors (see the boss-detection correction, §0.7), and rank-threshold
crossings.

### 0.6 RETENTION — CLOSED BY READING THE SOURCE, not by testing (2026-07-22)

**ANSWER: commanded targets are NOT sticky. The engine re-picks every combat
update.**

This is stated outright by the reference implementation: SmartTargetingNPC hooks
`Character::UpdateCombat` and rewrites `currentCombatTarget` /
`targetHandle` **after every single call**, because a one-shot write is
overwritten by the engine's own re-pick. That is its entire design premise.

**Nothing needed to be measured.** MFO's hook is correct *because* targeting is
not sticky — re-asserting at the engine's cadence is the mechanism, not a
workaround for it. §4.7's "does our target stick?" was answered in a public
source file the whole time.

**Cost of not looking: ~90 minutes of marth's play time**, across two sessions,
plus a probe, a crosshair sink, a hotkey and three builds — all to re-derive one
sentence. INVARIANTS #61.

The historical detail, kept because the confound is instructive:

### 0.6a (historical) the first retention test was confounded
**Same session, probe.** `StartCombat` held the commanded Boethiah Cultist for
7.0s with no defection — but **the cultist was the ONLY hostile present**
(marth's observation). Vanilla AI would have stuck to the only target
regardless, so this proves the engine keeps the *only* target, not that it
keeps *our commanded* one. **No evidence either way about the §4.7 model.**

To be meaningful the test must: (a) have **multiple** candidate hostiles, and
(b) command the follower onto one they are **not already fighting** — then see
whether our pick survives or the engine drifts back to its own. The v0.4.2
probe does exactly that (counts candidates, targets a non-current foe, and
says "inconclusive by construction" when only one exists). Until that runs in
a real multi-enemy fight, `StartCombat` retention is **UNKNOWN**, and §4.7
rests on nothing measured.

### 0.7 `EvaluatePackage()` NO-OPS when the package is unchanged — CONFIRMED
**Same session, probe.** `EvaluatePackage on Cosnach -> package 0005C84B ->
0005C84B (UNCHANGED)`. This is exactly the §4.5a hazard, now proven in-game:
re-evaluating when the same conditioned package would be chosen does nothing,
so **any native re-targeting will need the condition flicker** (set 0 →
evaluate → set 1 → evaluate). Promotes §4.5a rule 3 from RESEARCHED to PROVEN.

### 0.8 `CastSpellImmediate` applies the effect but plays NO ANIMATION
**Same session, probe + marth's report:** healing was cast (effect applied)
but the follower showed no casting animation. So `CastSpellImmediate` is a
**silent** effect application, not a visible combat action. Acceptable for a
self-heal; **not** acceptable as the primary "follower casts a spell at a foe"
verb, where a player expects to see it. The intended alternative
`DoCombatSpellApply` is Papyrus-only (§2.0), so a visible cast needs either VM
dispatch or `LaunchSpell` (po3) — an M5 verification-queue item, not a solved
one. **But test the cheap explanation first: §0.10.** This probe used
`kInstant`, which is by name the no-animation caster; a hand source may animate
and make both alternatives unnecessary.

### 0.9 `CastSpellImmediate` DOES deduct magicka — PROVEN 2026-07-21
**marth's report:** firing a probe cast removed magicka from the caster.

This **refutes** the assumption carried into the M5 review. The reasoning there
was that `CastSpellImmediate` is the scripted/trap path — the same call MEO
uses for free follower-shares — and therefore a free cast. It is not.

Consequences, all favourable:

- DESIGN §5.3 ("competence is not permission") is **load-bearing, not
  decorative.** A gambit cast spends the follower's pool, so a caster with a
  heal rule genuinely runs dry and genuinely falls through to the rules below
  it. The resource economy the design assumes actually exists.
- MFO must **not** hand-write a deduction. The engine produces this state;
  writing it ourselves would double-spend (INVARIANTS #16).
- `Actuation::CastOn`'s pre-check against `CalculateMagickaCost` gates a real
  resource rather than being a tautology.

**RESOLVED 2026-07-21, and the answer matters:** the engine does **NOT** refuse.
marth cast repeatedly with no magicka — *unlimited* casting. So
`CastSpellImmediate` deducts while there is a pool to deduct from, and then
casts for free forever.

**MFO's own pre-check is therefore the ONLY gate that exists.** Not
belt-and-braces — the sole belt. `Actuation::CastOn`'s `CalculateMagickaCost`
comparison is what makes §5.3 true; delete it and every follower becomes an
infinite spell battery. The probe bypasses that check by design (it calls the
engine directly), which is exactly why it could demonstrate this.

### 0.10 Casting source does NOT explain the animation — HYPOTHESIS REFUTED 2026-07-21
**marth, all four sources fired from the probe:** `kLeftHand`, `kRightHand`,
`kOther` and `kInstant` **all cast with no animation.**

The hypothesis was that `kInstant` is by name the no-animation caster, so a
hand source would route through the animated path. It does not. The casting
source is not the variable — `CastSpellImmediate` is a silent effect
application *regardless* of which `MagicCaster` issues it.

`iCastSource` stays in the INI (it costs nothing and the next question may want
it) but is defaulted back to `3`/kInstant, which at least names what happens.

**A visible cast therefore still needs a different mechanism**, and §0.8's
options are back on the table unreduced: po3's `LaunchSpell`, Papyrus VM
dispatch of `DoCombatSpellApply`, or driving the animation separately via
`NotifyAnimationGraph` / an equipped-spell + AI-issued cast. All are their own
milestone. **This is the single biggest open problem in the mod** — the core
loop's most visible action currently has no visible action.

### 0.11 The populated co-save ROUND-TRIPS — PROVEN 2026-07-21, M1 CLOSED
**MFO 0.5.0, game 1.6.1170.** A save carrying Cosnach at rapport 5 was reloaded:

```
[cosave] loaded 1 follower(s); dropped 0 unresolvable actor(s), 0 unresolvable
override(s); disabled 0 rule(s) with missing targets; 0 id collision(s)
000198FA Cosnach   rapport 5  rank 1  slots 2c/1l
```

Rapport survived, the FormID resolved, nothing was dropped or disabled. This is
the **highest-blast-radius subsystem in the mod** — the "mod ate my save" class
— and it had been reviewed four times but never executed end to end. It is now
proven. **M1 is closed.**

Still untested within it: load-order remap (test A) and the downgrade guard
(test B), both of which need a deliberately perturbed load order.

### 0.12 `StartCombat` (po3 relocation) did NOT take — 2026-07-21
`[probe] StartCombat (nearest foe) on Cosnach -> OK (target Fox at 2792u, 4
candidates -- valid: commanded onto a foe they were NOT fighting)` was followed
0.4 s later by `Cosnach now on <none>` and then `NEVER ENTERED COMBAT --
StartCombat did not take`.

The call returned without error and had **no effect**. Note the earlier §0.6
session *looked* like it worked, but that was the confounded single-target
case. With a proper multi-candidate setup the honest answer is that the
relocation either resolved to the wrong function, needs different arguments, or
the engine rejected the target at 2792u.

**§4.7 standing orders rest on nothing.** Not building the target latch was the
right call.

### 0.13 THE CAST FLOW — why nothing animates, from the engine side (2026-07-21)

Researched from primary sources after marth asked why this had not been done.
It had not, and that was a process failure: the animation problem was declared
"the biggest open problem in the mod" on the strength of guessing, without
reading the engine surface or looking at mods that already solved it.

**`MagicCaster` is a state machine, and `CastSpellImmediate` skips it.**

```
RequestCastImpl -> StartChargeImpl -> StartReadyImpl -> StartCastImpl -> FinishCastImpl
```

with `SetCurrentSpell(MagicItem*)`, `desiredTarget` (an ObjectRefHandle),
`castingTimer`, `currentSpellCost` and a `state` enum as members.

**And `ActorMagicCaster` inherits `SimpleAnimationGraphManagerHolder` AND sinks
`BSAnimationGraphEvent`.** That is the whole answer: an actor's caster is
*driven by the animation graph*. The animation is not a decoration on the cast
— the animation IS what advances the cast. `CastSpellImmediate` is the "apply
this now, from anywhere, no actor required" path (traps, scripts, enchantments)
and it necessarily bypasses the graph, which is why no `CastingSource` argument
could ever have made it animate. §0.10's refutation was predictable from the
class declaration.

**Prior art, and it points the other way.** [Dynamic Animation
Casting](https://github.com/LXIV-CXXVIII/DynamicAnimationCasting) and [Payload
Interpreter](https://github.com/D7ry/PayloadInterpreter) both cast from
animation events. DAC's call is *the same one MFO makes* -- 
`GetMagicCaster(kInstant)->Cast(spell, false, target, 1.0f, false, 0.0f, cause)`
-- deliberately using kInstant, because in DAC **the animation is already
playing** and the spell is applied to sync with it. Nobody tries to make
`CastSpellImmediate` animate. The pattern is: *animation first, effect second.*

(DAC also deducts its own costs via
`RestoreActorValue(kDamage, kMagicka, -cost)`, which is worth remembering
against §0.9.)

**What this means for MFO, and it is a design correction.** MFO was casting
*instead of* the follower. The doctrine in DESIGN §4.4 is to layer on top of
vanilla AI and never replace it -- and the vanilla flow is fully animated
already, because every enemy mage in the game casts with animation. The right
shape is to make the follower **want** to cast (equip/select the spell and let
combat AI drive it) rather than to reach in and apply the effect ourselves.
That is both the animated path and the one consistent with the mod's own rules.

Two candidate mechanisms, neither yet tested:
1. **Equip + let the AI cast.** `ActorEquipManager::EquipSpell` into a hand,
   `DrawWeaponMagicHands`, and let the combat controller fire it. Fully
   animated because it *is* the vanilla path. Less deterministic -- the AI
   chooses when, which is a feature under §4.4's layering doctrine and a
   problem under §4.3's one-rule-per-tick contract.
2. **Drive the caster state machine directly.** `SetCurrentSpell` + set
   `desiredTarget` + the `*Impl` sequence. More control, entirely unproven,
   and the `Impl` methods are virtuals whose preconditions are unknown.

**M4-style probe first, both mechanisms, before any design commitment.**

### 0.14 THE ATTACK VERB — found, with a shipped reference implementation (2026-07-21)

**`DoCombatSpellApply` is NOT a commanded animated cast.** It is the Papyrus
twin of `CastSpellImmediate`: an instant, silent effect apply. Three primary
sources, all local:

* Skyrim's own `Actor.psc`: `; Apply a spell to a target in combat`
* The Papyrus index positions it as the alternative to `Actor.AddSpell` — a
  *proc/apply* verb, not a cast verb
* Every shipped call site in ten modlists uses it that way, including
  **Bethesda's own Dawnguard** `DLC1dunHarkonShieldEffectScript` (instantly
  ejecting the player from a shield sphere) and a bow-draw self-stagger script
  that runs OUT of combat, where no animation is possible

**Papyrus cannot express commanded targeting AT ALL.** There is no combat-target
setter anywhere in `Actor.psc`, SKSE's additions, po3, or PapyrusUtil — only
`StartCombat`/`StopCombat` (entry/exit) and `GetCombatTarget` (a getter). The
target lives in `CombatController::targetHandle` and the actor's
`currentCombatTarget`: engine-internal data with no script binding.

That is why a survey of ten modlists found ZERO Papyrus attack commands, and
why "Swiftly Order Squad — Follower Commands UI" ships only Wait / Follow /
Inventory / Teleport. **Not convention — impossibility.** It also settles the
"should MFO grow a companion .psc?" question permanently: no. A script cannot
deliver the attack verb, and everything a script *could* deliver is reachable
from the DLL by VM dispatch with identical semantics.

#### The mechanism, and it is already installed on this machine

`Aggro Management in Skyrim` ships `SmartNPCTargetSelector.dll`, open source at
<https://github.com/RedyellowUnit/SmartTargetingNPC>, **running in LoreRim right
now**, tested by its author on 1.5.97 and 1.6.1170. It does exactly what MFO's
attack verb needs, for every NPC in the load order:

1. `write_vfunc` on `RE::VTABLE_Character[0]` index **`0xE4` (`UpdateCombat`)** —
   a VTABLE INDEX, not an AddressLib offset, so it does not drift across game
   versions.
2. Inside the hook, after calling the original: enumerate candidates from
   `combatGroup->targets` under `RE::BSReadLockGuard(combatGroup->lock)`, then
   write **both** `GetActorRuntimeData().currentCombatTarget` **and**
   `combatController->targetHandle` (plus `previousTargetHandle`).
3. **Only redirect when the engine already HAS a target.** If vanilla cleared it
   (target fled, undetected), respect that — this guard is what keeps the mod
   from fighting the engine's own validity logic.
4. Optional reinforcement: a call-hook on detection forcing the focus target's
   detection high and others low, which stops the AI drifting back via
   detection re-picks.

`actor->GetActorRuntimeData().combatController` reaches the controller directly;
`targetHandle` is at 0x2C, `previousTargetHandle` at 0x30, `combatGroup` at 0x00.

#### Consequences for MFO

* **Retention (§4.7) is a property of the MECHANISM, not the target.** The
  engine re-picks constantly — that is SmartTargetingNPC's whole premise — so a
  bare write from MFO's 7.5 Hz tick would drift in the gaps. The correct cadence
  is the engine's own: re-assert inside the hook, which is a handle
  compare-and-write, not a `StopCombat`/`StartCombat` reset (#22a preserved).
* **Do NOT hook the target selector.** `CombatTargetSelectorStandard` exists in
  NG only as a forward declaration — no vtable, no members. The post-`UpdateCombat`
  write achieves the same result with a published precedent.
* **KNOWN CONFLICT: SmartTargetingNPC itself.** LoreRim ships it, and it also
  steers follower targets via a hate table; its detection hook can suppress a
  follower's perception of MFO's chosen target. MFO must write only for
  followers under an active gambit latch, detect the DLL at startup, and say so
  in the log. TDM / SCAR / Valhalla / Precision / NPCsLearnToAim are all
  DOWNSTREAM consumers of the target and will simply act on MFO's choice.
* **Threading:** the hook body runs inside the engine's per-actor combat update,
  so writing there is safe by construction — but it READS MFO's latch, which is
  main-thread actuation state. That needs an atomics snapshot or a lock
  (INVARIANTS #4), designed before the probe, not after.

### 0.15 THE ANIMATION PATH — CONFIRMED IN THE FIELD (2026-07-22)

**marth, watching Cosnach with an MFO-equipped Heal Self:** *"he did actually
equip it and it seemed to instant cast and regular cast multiple times, he
managed to use 1000 magicka."*

**"Regular cast" is the animated one.** Both paths were visible in the same
fight and they look different:

| What was seen | Which path |
|---|---|
| **Regular cast** — the follower plays the casting animation | Their OWN AI firing a spell MFO put in their hand |
| **Instant cast** — the effect just happens | MFO's `CastSpellImmediate` fallback after the grace expired |

~1000 magicka spent confirms these were real casts arbitrated by the engine,
not free applies.

**So the architecture is right: MFO does not cast. It arranges the conditions
and the follower's own AI casts** — animated, magicka-arbitrated, correctly
aimed, because it IS the vanilla path. Three cast VERBS were refuted before
this (§0.8, §0.10, #56); the verb was never the missing piece. The
preconditions were: spell in hand (`Loadout`) plus a target.

**Still to settle:** the `[cast]` sink caught only ONE of those casts and
printed `?` for the spell, so the log cannot yet distinguish an AI-fired gambit
spell from the follower's own. Fixed by resolving as `TESForm` and flagging MFO
gambit spells explicitly. And the AI is ENTHUSIASTIC -- 1000 magicka is a lot of
casting, which is a balance question §5 will have to answer.

### 0.16 THE ANIMATED PATH IS AI-DISCRETIONARY — 2026-07-22

**MFO can put a spell in a follower's hand. It cannot make them cast it.**

v0.6.0 field session, Cosnach holding Heal Self (Rank I), a Requiem rank-1
restoration spell that heals very little: `[eval] fired rule 0 (act.cast_self)`
fired repeatedly across two fights, and **not one `*** MFO GAMBIT SPELL ***`
line appeared.** His own AI never chose to cast it. Every heal in that session
was MFO's silent fallback. marth: *"the heal is too weak to help him, and I
don't really see when he's casting it."*

The one AI-fired cast the sink caught was `Ale (00034C5E) formType=46` — a
potion he drank of his own accord, correctly reported as *not ours*.

**The consequence for the design, and it is not small:**

* **The RULE always fires.** `act.cast_self` executes whenever its condition is
  true, and the fallback guarantees the effect lands. Player sovereignty holds:
  a badly-chosen spell still runs (§4.3a), and MFO never second-guesses the
  list.
* **The ANIMATION does not always happen.** It requires the follower's combat
  AI to independently judge the spell worth casting. For a spell it does not
  value, MFO's silent path is what runs.

So §0.15's "the animated path works" is true and incomplete. It works **when
the AI agrees**. That is a property of the spell and the follower, not of MFO,
and it cannot be forced without going back to the refuted verbs.

**What MFO must NOT do about it:** make the seed prefer spells the AI likes, or
otherwise steer which spell a rule names. The rule says what it says. Ruled by
marth, 2026-07-22 — *"a bad gambit setting still needs to fire."*

**Open, and worth one cheap observation:** whether the AI casts a spell it DOES
value -- an offensive spell at a foe -- when MFO equips it. That would tell us
whether the animated path is broadly available or narrow. It needs a follower
who knows such a spell, not a change to MFO.

### 0.17 THE FORCED CASTING PACKAGE — the mechanism with shipped precedent (2026-07-22)

Found while reviewing the caster-drive probe, and it is probably the answer.

**[ALYSLC](https://github.com/Joessarian/Adventurers-Like-You-Skyrim-Local-Co-op)
(Skyrim local co-op) solves exactly MFO's problem** — making a companion NPC
body perform a full ANIMATED cast on demand — and it does **not** drive the
caster. For NPC players it pushes a **ranged casting package** onto the package
stack and evaluates (`SetUpCastingPackage`,
`src/PlayerActionFunctionsHolder.cpp:6219`). Only for the real player does it
synthesise button events.

That is consistent with everything MFO has proven: the release step is
graph-gated (`MRh_SpellFire_Event` -> `StartCastImpl`), and a package makes the
AI itself run the cast, which plays the animation, which fires the release.
It is the same shape as §0.15's confirmed path (arrange the conditions, let the
AI act) but with the AI's judgement REMOVED from the loop -- which is exactly
what §0.16 says is missing when the AI declines a spell it does not rate.

**Caveat MFO already owns:** §0.7 proved `EvaluatePackage()` NO-OPS when the
chosen package is unchanged, so a package approach needs the condition flicker
(§4.5a rule 3). And package overrides are §4.5a's "last resort" for good reason
-- they are the loudest thing MFO could do to another mod's follower.

#### The mechanism in full, read from ALYSLC's source

1. **A conditioned PACKAGE** — a ranged-attack/cast package whose conditions
   test a **global variable**.
2. **Arm it**: set the global (`lhCasting->value = 1.0f`).
3. **Attach it**: write the package into the actor's package stacks —
   `packageStackMap[kDefault]->forms[0]` and `[kCombatOverride]->forms[0]`.
   ALYSLC also clears the current scene, because scene packages override both
   run-once and package-stack packages.
4. **`EvaluatePackage()`**, and only when the package actually differs (§0.7's
   no-op finding, independently confirmed by their `a_evaluateOnlyIfDifferent`).
5. **`RequestCastImpl` is a RESTART NUDGE, not the trigger.** Their own comment:
   *"if a caster is stuck at state 1 for multiple frames, the casting package is
   either not being executed or has stalled... Request to cast again if this
   happens."* They call it only when `state == kUnk01`.

That last point is the review's prediction confirmed from their source before
MFO ever ran the probe: **state 1 IS the wedge.** The package drives the cast;
`RequestCastImpl` only unsticks it.

**Cost of applying this to MFO, stated honestly.** ALYSLC owns its actors and
ships per-player package FormLists. MFO attaches to *arbitrary* followers it
does not own, so it needs its own authored records:

| Piece | Status |
|---|---|
| GLOB — the casting condition variable | New record type for the generator |
| PACK — a conditioned cast package | **New, and the hardest record MFO would emit** (PKDT/PSDT/PLDT, conditions, package data) |
| QUST + alias pool — how a package reaches a follower MFO does not own | New |
| Runtime: arm global, set stack, evaluate, nudge | Straightforward once the records exist |

The FormID band was reserved for exactly this at project start (`0x80A-0x80F`
command QUST + alias pool + globals; `0x820+` MFO's own conditioned PACKAGEs,
both tagged *Tier B, M9*). **This is M9, and it is the largest single mechanism
in the project** — not a patch on M5.

#### THE ACTUAL RECORD, read from their shipped ESP (2026-07-22)

Downloaded `Data/ALYSLC.esp` and dumped it. 18 PACK records. The relevant pair:

```
01000894  type=19  __CoopPlayerRangedAttackPackageTemplate
01000866  type=18  __CoopPlayerRangedAttackPackage1   -> PKCU template=01000894
```

The instance is **2606 bytes**: `PKCU inputs=65 template=01000894`, 65
`ANAM`/`UNAM` data-input pairs, 35 `PTDA` target-data entries, 30 `CNAM`,
`PLDT`, `POBA`/`POEA`/`POCA` blocks, 3 `PDTO`. The template itself declares the
same 65 inputs including **20 TargetSelectors**.

**And there is NOT ONE `CTDA` on either record.** So the globals do NOT gate the
package by condition, which is what the earlier reading assumed. ALYSLC selects
the package by **swapping it into the stack** — the package IS the selection,
and the globals are their own bookkeeping. That simplifies MFO's runtime and
complicates the record.

#### What this means for MFO — honest assessment

**In favour:** the mechanism demonstrably produces uninterruptible animated
casts on an NPC body, it is the only mechanism not yet refuted, and we can now
read the exact byte layout rather than guess it.

**Against, and these are not small:**

1. **Size.** A working instance is ~2.6 KB with 65 data inputs, riding a custom
   template that is another large record. This would be by far the biggest
   record MFO has authored, and the ESP generator has only ever emitted MGEF /
   SPEL / KYWD / QUST — all trivial by comparison.
2. **ALYSLC owns its actors; MFO does not.** Their FormLists are attached to
   co-op bodies they configured. MFO must force an arbitrary follower — one
   already managed by NFF, Inigo, or another framework — into its own alias and
   override the package stack. §4.6 contention, a problem they never had.
3. **Their own code admits fragility:** the stacks *"seem to clear when going
   through load doors at times"*, so they re-assert defensively.
4. **Most of those 65 inputs are for things MFO does not want** — co-op aiming,
   dual-cast, shouts, 20 target selectors.

#### The question that decides the cost, and it is NOT yet answered

**Can a PACK instance point `PKCU.template` at a VANILLA Skyrim template
instead of a hand-authored one?** If yes, MFO supplies only the data inputs it
cares about and inherits the machinery — M9 shrinks from "author a package
system" to "author one instance". If no, the template must be built from
scratch and M9 is genuinely the largest thing in the project.

**Answer that before committing.** It is a read of `Skyrim.esm`'s PACK records,
costs no play time, and swings the estimate by an order of magnitude.

#### THE ANSWER: vanilla ships the templates. M9 is one record per action. (2026-07-22)

Dumped `Skyrim.esm`. **104 package templates (PACK type 19)**, and every action
MFO needs already exists:

| Vanilla template | FormID | MFO action |
|---|---|---|
| **UseMagic** | `000504F5` | `act.cast_self` / `act.cast_target` |
| UseMagicRepeat | `000F5842` | sustained casting |
| **UseWeapon** | `0001C338` | `act.attack` |
| **HoldPosition** | `000503D0` | hold position |
| **Travel** | `00016FAA` | move / formation |
| **Activate** | `00019B2D` | loot / use |

**So MFO does NOT author a template.** ALYSLC built their own 65-input monster
because a co-op body needs 20 target selectors and dual-cast and shouts. MFO
points `PKCU.template` at a vanilla one and supplies only the inputs it cares
about. This is the order-of-magnitude question, answered: **M9 is one PACK
instance per action, not a package system.**

##### `UseMagic` (`000504F5`) — 11 inputs, named

```
PKDT flags=0x00000000 type=19 interrupt=0
PKCU inputs=11 template=00000000 ver=1
  0  Place to Travel   (Location)
  1  Destination
  2  Location
  3  Spell             <- ours
  4  Target            <- ours
  5  HoldWhenBlocked   (Bool)
  6  CastTimeMin       (Float)
  7  CastTimeMax       (Float)
  8  CooldownTimeMin   (Float)
  9  CooldownTimeMax   (Float)
 10  NumToCastMin      (Int)
 11  NumToCastMax      (Int)
 12  DualCast          (Bool)
  +  Procedure "UseMagic"
```

**Vanilla already provides the rate limiting** MFO hand-rolled as
`fCastCooldown`: `CooldownTimeMin/Max` and `NumToCastMin/Max` are package
inputs. And `CastTimeMin/Max` bounds the action, which §4.5c requires
("every action gets a completion condition and a hard timeout").

##### The one design problem left

Data inputs are baked into the record, but MFO needs a DIFFERENT spell and
target per rule. Two routes:

1. **Mutate the package's inputs at runtime.** One PACK record; write the Spell
   and Target inputs in memory before pushing it. `TESPackage` is a live form.
2. **Alias for the target, one record per spell.** The Target slot can reference
   a quest alias MFO fills; the Spell cannot, so this needs a record per spell —
   unbounded, and therefore wrong.

**Route 1 was WRONG — superseded by §0.18.** Verify `TESPackage`'s runtime data-input surface in
CommonLibSSE-NG before building.

**Status: RESEARCHED, not built.** The caster-drive probe (§0.13 mechanism 2)
still ships first because it is cheap and it DISCRIMINATES: `CheckCast` refusing
means the engine rejected the cast outright; accepted-then-wedged means the
graph gating is real and the package is the way.

#### What the probe already taught, from the review rather than the field

* `RequestCastImpl` IS the engine's real entry point -- three shipped SKSE mods
  hook it at `ActorMagicCaster` vtable index 0x3. Calling it is not a category
  error the way sending `BeginCastLeft` would have been.
* But **no shipped mod calls it to trigger a cast.** Every public use is a hook.
* Its preconditions: `state == kNone`, `currentSpell` already selected by the
  equip, and `CheckCast` passing. MSCO's hook exists specifically to DENY the
  request once the caster is past its early states.
* `MagicCaster::SetCurrentSpell` **does not exist at the pinned rev** -- that is
  a current-master name, and §0.13 cited it from modern docs. Only
  `SetCurrentSpellImpl` (a no-op on `ActorMagicCaster`) and the public
  `currentSpell` member. Verify it; never write it by hand, or the engine's
  select/deselect bookkeeping desyncs.

### 0.15a (historical) the path as designed, before it was observed

Three cast VERBS have now been refuted: `CastSpellImmediate` (all four casting
sources, §0.8/§0.10), `Projectile::LaunchSpell` (no projectile on a
Self-delivery spell, #56), and `DoCombatSpellApply` (the Papyrus twin of
CastSpellImmediate, §0.14).

**The verb was never the missing piece.** `ActorMagicCaster` is driven by the
animation graph, and the graph is driven by the follower's own combat AI. So the
question is not "which call animates" but "what state does an NPC need to be in
before its AI casts?" — and the answer is the state every enemy mage in the game
is already in:

| Precondition | Provided by | Status |
|---|---|---|
| The spell is in their hand | `Loadout::Prepare` | Built |
| They have a target | `Targeting` (`UpdateCombat` hook) | Built |
| Something fires it | **their own combat AI** | Vanilla |

Both preconditions land in the same batch. When the AI fires an equipped spell
at a latched target, the cast is animated, magicka-arbitrated and correctly
aimed **because it is the vanilla path** — MFO is not casting at all, it is
arranging the conditions and letting the engine cast.

**What this costs:** the AI chooses the INSTANT. MFO commands the what and the
who, not the exact frame. That is consistent with §4.4's layering doctrine, and
the suppression window already absorbs the timing slop.

**NOT YET OBSERVED, and the distinction matters.** The `TESSpellCastEvent` sink
now logs follower casts (`[cast] ... AI-fired`), which is the only way to tell
an AI-fired cast from one MFO issued. Until a `[cast]` line appears for a
follower holding an MFO-equipped spell, this is an assembled hypothesis, not a
result — see #57, twice violated already.

**The governing principle (marth):** *if the game code can trigger it, we can
too.* The engine's combat AI makes actors cast with animation many times a
second. That code path is IN THE BINARY and is reachable — MFO has been hunting
for a *published verb* when the real question is which internal call the AI
makes. `MagicCaster`'s state machine is that call, and its members are public
virtuals. So the fallback below is not speculation; it is calling what the
engine calls.

**Fallback if the AI will not fire on a useful timescale:** drive the
`MagicCaster` state machine directly — `SetCurrentSpell` + `desiredTarget` +
`RequestCastImpl`/`StartChargeImpl`/`StartReadyImpl`/`StartCastImpl`/`FinishCastImpl`
(§0.13 option 2). Deterministic and animated IF the vfunc preconditions
cooperate; entirely unproven; one INI-gated probe.

### 0.18 THE PACKAGE ROUTE, AS SHIPPED — supersedes §0.17's conclusion (2026-07-22)

§0.17 concluded "Route 1 is the design" (mutate a package's inputs) and dismissed
the alias route as "unbounded, and therefore wrong". **Both judgements were
wrong, and the shipped records use the alias route.** §0.17 remains accurate on
everything it OBSERVED — the templates, the input names, ALYSLC's technique —
and wrong only in what it concluded. This section is the authority.

#### How a package reaches a follower MFO does not own

**A quest ALIAS carries packages via `ALPC`, and they apply to whoever fills
it.** 4,125 `ALPC` entries across 365 vanilla quests reference 2,070 distinct
packages: this is the mainline vanilla mechanism, not a corner of one.

The runtime carrier is **`ExtraAliasInstanceArray`** on the *reference*:

```cpp
struct BGSRefAliasInstanceData {
    TESQuest*                    quest;
    const BGSBaseAlias*          alias;
    const BSTArray<TESPackage*>* instancedPackages;
};
```

read by `Actor::CheckForCurrentAliasPackage()` (vfunc `0x049`).

**Two consequences that decide the architecture:**

1. **It is per-REFERENCE.** NFF's copy of the base NPC, other instances, the base
   record — untouched.
2. **It is ADDITIVE.** `BSTArray<BGSRefAliasInstanceData*>` — NFF's alias entries
   and MFO's coexist on the same actor. **MFO structurally cannot stomp another
   framework**, which is stronger than §4.6 dared assume.

#### Ruled out, with reasons

* **`AIProcess::currentPackage`** — `ActorPackageData` is opaque at the pinned
  rev; writing it desyncs `data` and `currentProcedureIndex`.
* **The base record's package lists** (`TESNPC::defaultPackList` DPLT,
  `TESAIForm::aiPackages` PKID) — **shared by every instance of that NPC.**
  This is what ALYSLC does, and they can only do it because they own their
  bodies.
* **ALYSLC's `packageStackMap` / `kCombatOverride`** — those symbols **do not
  exist at the pinned CommonLib rev at all.** They are ALYSLC's own map into
  FormLists they ship.

#### The target does NOT need an alias

`PackageTarget::targType == 0` takes an `ObjectRefHandle` at runtime, so an
arbitrary foe is named directly. The alias only ever DELIVERS the package.
**This deletes the alias-pool design and the `GLOB` record M9 was budgeted for.**

#### `EvaluatePackage`'s no-op finding does not apply here

§0.7 proved `EvaluatePackage()` no-ops when the chosen package is unchanged.
Filling or clearing an alias **changes the candidate set**, so no condition
flicker and no global are needed.

#### Record requirements, verified against Skyrim.esm

| Requirement | Evidence |
|---|---|
| `QNAM` (owner quest) whenever any input names an alias | 159 packages use `PTDA` targType 4; widened to include `PLDT` type 8 it is 626 — **all carry `QNAM`** |
| `kIgnoreCombat` (`0x00100000`), **not** `kMustComplete` | all 6 vanilla `UseMagic` instances meant to fire in a fight set it; `kMustComplete` appears on exactly 1, a stationary channeling thrall |
| `PLDT` type 12, not type 0 | type 0 means "near reference" and needs one — **0 of 4,048** vanilla type-0 entries have a null target |
| `ANAM` before the alias blocks | **1,607 of 1,607** vanilla quests with aliases |
| `DNAM` = flags u16, priority u8, pad, delay f32, type u32 | decoded across all 1,811 vanilla quests; delay is 0.0 in every one |

**The reference record to copy is `TG08BMercerCombatOverrideCastAtBrynjolf`
(`000FCC26`)** — `UseMagic` + `QNAM` + `PTDA` type 4, i.e. MFO's exact shape.
`MG07AncanoCastAtEye`, copied first, targets a *specific reference* and
therefore has no `QNAM` at all.

#### Priority is the contention lever

`QUEST_DATA::priority`. Vanilla spreads it deliberately: default 30,
`DialogueFollower` 50, scene quests 80–96. **MFO takes 60**, set once. §4.6
forbids escalating it in a fight with another mod. Detect contention with
`CheckForCurrentAliasPackage()` and `TESPackage::ownerQuest` names the
contender — so the log can say *which mod*.

#### Still unverified, and the first is load-bearing

1. **That filling an alias INSTANCES its `ALPC` packages onto the reference.**
   Structurally implied and matched by vanilla behaviour; the fill function's
   body was not read.
2. `GetHandleForObject(BGSRefAlias::VMTYPEID=140, alias)` yielding a usable VM
   handle for `ForceRefTo`.
3. `TESForm::CreateDuplicateForm` on `FormType::Package`.
4. Whether `ExtraAliasInstanceArray` survives a 3D unload / load door.

### 0.19 THE PACKAGE ROUTE IS PROVEN — an alias DOES instance its packages (2026-07-22)

**FIELD RESULT, and it settles §0.18's load-bearing assumption.**

marth, ESP-only PoC, no DLL code running: *"Its running and he is locked in
place and unresponsive."*

That single observation proves the entire delivery chain, every link of which
was unverified an hour earlier:

| Link | Proven by |
|---|---|
| A start-game-enabled quest starts on an EXISTING save via SEQ | `sqv` reported Running |
| `ALFR` fills the alias with a specific reference | alias 0 held CosnachREF |
| **Filling an alias INSTANCES its `ALPC` packages onto the reference** | **he was OWNED by the package** |
| Quest priority arbitrates against a follower framework | MFO at 60 beat Requiem's `DialogueFollower` at 50 |
| `kIgnoreCombat` runs the package out of combat | he was rooted while idle |

**MFO can take ownership of an arbitrary follower's behaviour for the duration
of an action.** That is DESIGN §4.5c's whole premise, and it is now a field
fact rather than a plan.

#### And the same test refuted the target model

He never cast. **No animation, no effect, and no magicka spent** -- the
procedure never attempted it. The Target input was `targType 4 -> alias 0`,
reasoning that since the alias holds the follower, alias 0 *is* himself.

**Vanilla never self-targets that way.** Of the 46 `UseMagic` instances:

* **8 use `targType 4` (reference alias) -- and every one names a DIFFERENT actor.**
* **7 use `targType 6`, value 0. That is SELF.** `WCollegeColettePracticeHeal13x2`
  is literally "practice healing on self"; also `DA16ErandurCastSpell`,
  `SprigganCallOverride`, and Colette's other practice packages.

So an alias is for pointing at SOMEONE ELSE. Self has its own target type, and
using an alias that happens to contain the caster is not a substitute --
the procedure resolves nothing and stalls silently, holding the actor.

**That silent stall is the signature to remember:** package owns the actor,
actor does nothing, no resource is consumed. It means the procedure's INPUTS
are unresolvable, not that delivery failed.

### 0.20 THE CTD — two unprecedented axes, not three (2026-07-22, CORRECTED TWICE)

This section has been wrong twice. Both errors came from extracting a rule from
too NARROW a population, which is the same mistake in two costumes.

* **v1 blamed concentration spells.** Refuted: 12 of the 46 vanilla `UseMagic`
  packages cast them, two with Flames. Cause: read `SPIT` offset 0x0C
  (`chargeTime`) as `castingType`.
* **v2 said "all 9 alias-delivered packages carry no QNAM, so QNAM must be
  absent".** True of those 9 and MISLEADING. Counted over all **5,857 PACK
  instances**:

```
delivered=Y  aliasInput=Y  QNAM=Y    356   <- MFO's generalized shape, SHIPPED by Bethesda
delivered=Y  aliasInput=-  QNAM=Y    611   <- QNAM on alias-delivered: NORMAL
delivered=*  aliasInput=Y  QNAM=-      0   <- the actual hard rule (626/626)
```

**QNAM is not forbidden. It is REQUIRED whenever any input names an alias, and
harmless otherwise.**

#### The crash's actually-unprecedented axes

1. **`targType 6` in the PROCEDURE TARGET slot of an alias-delivered package** —
   0 instances. (`t6` under `ALPC` in *auxiliary* slots ships 14 times, so the
   rule is slot-specific, not type-specific.)
2. **`QNAM` emitted AFTER `PKCU`** — 0 of 2,109; every vanilla record puts it
   immediately before.

The crash asm (`cmp [rbx+0x1A], 0x3E` with rbx null; 0x1A is `TESForm::formType`)
reads as *"is the resolved target an ACHR"* against a null resolution —
consistent with (1), and not statically verifiable.

#### And the "next problem" was already solved by Bethesda

§0.20 v2 said generalising to an arbitrary follower was unsolved. **Wrong the
day it was written.** `CWFinaleLeaderExecuteEnemyLeader` (`000D1E21`) is an
alias-delivered `UseWeapon` whose `Target to Attack` is `PTDA t4 -> alias 14`,
with QNAM, stage-gated: **follower-in-alias-A attacks actor-in-alias-B.** That
is exactly MFO's generalized shape, shipped. The WERoad quests are the same
architecture for movement (`Travel` + `PLDT t8 -> alias`, 263 instances).

### 0.21 M9 PROVEN — packages produce ANIMATED casts, at a CHOSEN target (2026-07-22)

**Field result, probe ladder, marth:** *"They all work except for 4... also 5,
cast on player... spells and animations are cast."*

| # | Spell | castingType | delivery | Target input | Result |
|---|---|---|---|---|---|
| 1 | Magelight | FireAndForget | Aimed | `t0` -> PlayerRef | **CASTS** |
| 2 | FastHealing | FireAndForget | Self | `t0` -> PlayerRef | **CASTS** |
| 3 | FastHealing | FireAndForget | Self | `t0` -> CosnachREF (himself) | **CASTS** |
| 4 | CollegePracticeWard | **Concentration** | Self | `t0` -> PlayerRef | **fails** |
| 5 | Magelight | FireAndForget | Aimed | **`t4` -> alias 1** | **CASTS** |

#### What this settles

**1. The architecture works.** A package MFO authored, delivered through a quest
alias, makes a follower cast **with a real animation**. That is DESIGN §4.5c's
premise and the end of the search that refuted `CastSpellImmediate`,
`LaunchSpell`, `DoCombatSpellApply`, animation events and driving `MagicCaster`.

**2. Probe 5 is the important one.** `PTDA t4 -> alias 1` + `QNAM`, with the
follower in alias 0 and the victim in alias 1, **casts at the aliased target**.
That is the `CWFinaleLeaderExecuteEnemyLeader` shape, and it means MFO can aim a
gambit at an actor chosen at runtime by filling an alias. Targeting is solved.

**3. Probe 3 REFUTES the stall hypothesis, and refines #65.** Both #65 and the
synthesis algorithm predicted "target == the runner" goes inert. It does not:
`t0` naming the caster's own reference casts fine. The rev-3 stall was
specifically **`t4` pointing at the DELIVERING alias** -- an alias indirection
back to itself -- not self-targeting in general. So `cast_self` HAS a proven
shape: **`t0` -> the follower's own reference.**

**4. Probe 4 was another ZERO-PRECEDENT cell, and it may be unreachable.**

marth's read -- *"how could Cosnach cast a ward on the player, no wonder it
failed"* -- points at the right area. The exact rule is sharper. For
SELF-delivery spells in vanilla `UseMagic`:

| castingType | targType | count |
|---|---|---|
| Concentration | **6 (self)** | 2 |
| FireForget | 6 (self) | 4 |
| FireForget | 0 (a ref) | **1** <- probe 2's precedent (`dunReachwaterRockGauldurReforgeAmulet`) |
| FireForget | 4 (alias) | 1 |
| **Concentration** | **0 (a ref)** | **0** <- probe 4 |

Probe 2 works because exactly one vanilla record does that. Probe 4 sits in an
empty cell. It is not "concentration is broken" -- it is *this combination* is
unshipped.

**AND THE OBVIOUS FIX IS BLOCKED.** Concentration+Self is only ever shipped with
`targType 6` (2 of 2) -- but `targType 6` in the TARGET slot of an
**alias-delivered** package is itself a zero cell, and is what CTD'd in rev 4
(§0.20). Vanilla's two concentration-self records are not alias-delivered.

**So concentration self-casts may be UNREACHABLE through alias delivery**, and
MFO must refuse them with a reason (§5.3's shape: the rule fails legibly) rather
than crash or stall. A cheap probe could still settle it: concentration + AIMED
at `t0`/`t4`, which vanilla does ship under `ALPC`.

#### Still open

* **Magicka consumption unverified** -- marth could not tell. It decides whether
  §5.3's competence gate is real for package casts or decorative.
* **A cast takes a few seconds to start.** Package evaluation is nowhere near
  §4.1's 133 ms budget. Gambits will respond on a package cadence, not a tick
  cadence, and BALANCE/DESIGN must say so rather than implying otherwise.
* **The actor stays ROOTED.** The package owns him, which §4.5c sanctions for an
  action's duration -- but the action must then END and hand him back. Nothing
  currently pops the package.

### 0.22 EVERY SHAPE WORKS — and package casts are FREE (2026-07-22)

**Probe group 2, field:** *"Magick is not being used, all 3 work."*

| # | Spell | castingType | delivery | Target | Result |
|---|---|---|---|---|---|
| 6 | CollegePracticeWard | Concentration | Self | **`t6` self** | **CASTS** |
| 7 | Thunderbolt (cost 343) | FireForget | Aimed | `t4` alias 1 | **CASTS** |
| 8 | HealingHands | Concentration | TargetActor | `t0` PlayerRef | **CASTS** |

Combined with group 1, **every axis MFO needs is now proven**:

| Axis | Proven values |
|---|---|
| Target | `t0` specific ref · `t4` reference alias · **`t6` self** |
| castingType | FireAndForget · **Concentration** |
| delivery | Self · Aimed · TargetActor |

**The synthesis algorithm has no unreachable cells left.** Any (verb, spell,
target) a gambit can name is expressible.

#### #67 IS REVOKED, and the rev-4 crash is re-explained

§0.20 concluded `targType 6` in an alias-delivered package was the crash axis,
and #67 forbade concentration self-casts on that basis. **Probe 6 is exactly
that shape and it casts.** So `t6` was never the problem.

Rev 4 had `t6` **and a QNAM** (misordered, after `PKCU`). Probe 6 has `t6` and
**no QNAM** — correct, because a `t6` target names no alias. So the crash was
the QNAM: either its position (0 of 2,109 vanilla put it after `PKCU`) or its
mere presence on a record whose inputs reference no alias. The generator now
gets both right, which is why this shape is safe today and was not then.

**Standing rule, unchanged and now load-bearing:** emit `QNAM` **only** when an
input names an alias, and **always immediately before `PKCU`**.

#### PACKAGE CASTS DO NOT COST MAGICKA — measured

Thunderbolt costs **343**. With regen frozen and the pool forced to 1000,
Cosnach cast it and the pool read **1000 afterwards**.

**This is the third actuator to spend nothing**, and it decides §5.3. The
competence gate ("a follower who cannot afford a spell fails the rule") is
**decorative for package casts** unless MFO deducts. See §0.23.

### 0.23 THE THREE GAPS, and what the data already says about them (2026-07-22)

marth: *"they actually need to move during casting. And we know thats possible
because of vanilla. Deduct on fired is correct, but it needs to represent what
the spell would cost for them under their current skills and perks. cadence can
be flexible with casts."*

#### (a) Movement — the record is probably NOT the cause

**MFO's package already matches Mercer's combat override byte-for-byte:**
`PKDT flags = 0x00100000` (bit 20) and `PLDT type 12, radius 10000`. Mercer
moves while casting in that fight. Same flags, same location input.

Flag frequency across the 46 vanilla `UseMagic` packages, for the record:

```
bit 13 (0x00002000)  7      bit 20 (0x00100000)  6   <- MFO and Mercer
bit 10 (0x00000400)  7      bit  2 MustComplete  1
```

**Hypothesis, and it is cheap to test: the package does not root them — the
absence of COMBAT does.** In a fight the combat AI drives movement while the
package drives the casting; `kIgnoreCombat` is what lets the package keep
running through it. Every probe so far ran with Cosnach idle, which is the
artificial case — a gambit only ever fires in combat.

**Test:** select a probe, then start a fight. If he moves and casts, there is
nothing to fix and the rooting was an artefact of testing out of combat. If he
stays rooted mid-fight, the next candidate is the `Place to Travel` input
(currently type 12 = no destination) or a travel-capable template.

#### (b) The deduction must be THEIR cost, not the spell's

`SpellItem::CalculateMagickaCost(actor)` — the actor overload, which already
accounts for skill level and perks. MFO already calls it for §5.3's gate in
`Actuation::CastOn`; the same value is what gets deducted. A Destruction-perked
follower pays less for Thunderbolt than a novice, and the gambit must reflect
that or the "competence" gate is measuring the wrong thing.

**#16 does not block this.** That rule forbids hand-writing state a flow
produces. The package flow produces NO deduction at all (§0.22, measured), so
this fills a gap rather than duplicating one. DAC sets the precedent with
`RestoreActorValue(kDamage, kMagicka, -cost)`.

#### (c) Cadence is flexible — do not over-engineer it

marth's ruling: casts may take a variable few seconds. §4.1's 133 ms is the
**evaluator** cadence — how fast MFO DECIDES — and that stays. Actuation
latency is a separate, looser number, and DESIGN currently conflates them.
The package's own `CastTimeMin/Max` and `CooldownTimeMin/Max` are the knobs,
and they are already per-record.

### 0.24 AN ALIAS FILL IS NOT FREE — an empty alias ROOTS the actor (2026-07-22)

marth: *"hes been rooted to the exact same spot for several iterations, on
load"* — with `MFO_ProbeSelect = 0`, i.e. **before selecting any probe.**

Verified: every probe carries a correct gate (`GetGlobalValue(MFO_ProbeSelect)
== N`, func 74), and at 0 none is valid. Alias 0 carries probes only. So the
actor is in MFO's alias, MFO's quest outranks his own at priority 60, and the
alias supplies **nothing** — and he stands still.

**This is the first thing found that breaks a follower without MFO doing
anything.** The PoC's `ALFR` force-fill makes it permanent and therefore
visible; the real runtime would produce the same freeze in every window between
filling the alias and a package becoming valid.

#### Vanilla's fix, and why MFO must NOT copy it

Of 740 vanilla aliases carrying ≥2 packages, **299 end in an UNGATED package** —
Travel (123), Sandbox (63), Patrol (41), Follow (4). Gated on top, always-valid
at the bottom. That is the shape that keeps an actor sane when every gate fails.

**MFO must not do this.** An always-valid fallback on MFO's alias means MFO's
priority-60 quest ALWAYS supplies the winning package, which is precisely
"MFO owns the FOLLOWER" — the violation DESIGN §4.5c exists to forbid. It would
permanently override whatever framework actually manages that follower (§4.6).

#### The rule this creates

**Never leave a follower in MFO's alias without a valid package.** The alias is
filled for the duration of ONE action and cleared the moment it completes.
Between actions the follower is not in the alias at all — which is what §4.5c
already says, now with a mechanical reason rather than a doctrinal one.

Consequences for the runtime:

* **Clearing is not cleanup, it is correctness.** A missed or delayed clear is a
  frozen follower, not an untidy one.
* Fill and clear are async VM dispatches, so the fill→valid and
  complete→cleared windows are real. They must be measured, and the watchdog
  that clears on timeout is mandatory, not defensive.
* A crash or a save/load between fill and clear strands a rooted follower.
  `kPreLoadGame` release and post-load reconcile already exist for this and are
  now load-bearing.

**Not yet distinguished:** whether the freeze is "high-priority quest wins and
supplies nothing" or something narrower. Cheap test: drop the quest priority
below 50 and see whether he follows normally while still in the alias. That
also tells us whether priority 60 is even needed.

### 0.25 ARBITRATION IS BY QUEST PRIORITY, NOT PACKAGE VALIDITY — and that
resolves #69 (2026-07-22)

Two field results, same build, one byte apart:

| Quest priority | Probe selected | Probe at 0 |
|---|---|---|
| **60** (above `DialogueFollower`'s 50) | **casts** | **follower ROOTED** |
| **25** (below it) | **nothing happens** | — |

**So the engine does not pick "the highest-priority quest that HAS a valid
package". It picks the highest-priority quest whose ALIAS CLAIMS THE ACTOR, and
then asks that quest for a package.** MFO at 60 claims the follower; if no alias
package is valid, MFO supplies nothing and the actor idles. MFO at 25 never
claims him, so its packages are never consulted even when their conditions pass.

That is the mechanism behind §0.24, and it means the two facts are not in
tension — they are the same fact.

#### The resolution: ALIAS MEMBERSHIP IS THE GATE

MFO must be high-priority to act at all. It must therefore hold a follower in
the alias **only while it wants an action**, which is exactly what §4.5c and #69
already require — now with the mechanism understood rather than inferred.

**And the CTDA gates are a testing artefact.** The probe ladder needs them
because it force-fills the alias permanently via `ALFR` and switches probes with
a global. Production does the opposite:

```
fill alias  ->  package is UNGATED, so it is valid immediately  ->  action runs
clear alias ->  MFO no longer claims the follower               ->  normal AI
```

No condition, no global, and **no freeze window**: the package is valid the
instant the alias fills, because MFO fills the alias precisely when it wants
that package to run. There is never a moment where MFO claims a follower and
offers nothing.

| | Probe ladder | Production |
|---|---|---|
| Alias fill | permanent (`ALFR`) | per-action, cleared on completion |
| Package | gated on a GLOB | **ungated** |
| What selects | the global | **alias membership** |

#### Consequences

* **Priority 60 stays**, and it is not the cause of anything — it is the
  requirement.
* A follower is either mid-action-and-claimed, or free. There is no third state,
  and any code path that can produce one is a bug (#69).
* §4.6 contention gets simpler than feared: MFO does not permanently outrank
  another framework, it outranks it for the seconds an action takes.

### 0.26 CONDITIONAL ALIAS FILLS: half the answer, and the end of the ESP-only route (2026-07-22)

Gated the `ALFR` fill itself with `GetGlobalValue(MFO_ProbeSelect) > 0`.

**WORKS — the release half.** At `MFO_ProbeSelect = 0` the alias stays empty,
MFO never claims Cosnach, and he **followed and fought normally**: the log
credits three kills (Wolf, Bandit) while the quest was running at priority 60.
That is #69 answered at the ESP level — an unclaimed follower is genuinely free,
and the freeze is not inherent to MFO's quest existing.

**FAILS — the claim half.** Setting the global (with `resetquest`) produced **no
cast at all**, plus **performance problems**. MFO's own log is 49 lines with
nothing repeating, so the cost is in the ENGINE, not the DLL.

**Conclusion: alias fill conditions are evaluated when the quest starts, not
continuously** — the suspicion marth raised before the test. A conditional fill
can decide whether a follower is claimed AT LOAD; it cannot claim one on demand,
which is exactly what a gambit needs. And something about repeatedly
re-evaluating or re-filling is expensive.

#### This ends the ESP-only route, and that is fine

The ESP-only PoC has now delivered everything it can:

* alias delivery instances packages (§0.19)
* every target/casting/delivery shape works (§0.22)
* arbitration is by quest priority (§0.25)
* an unclaimed follower is free; a claimed one with nothing valid freezes
  (§0.24, and the release half here)

**The remaining piece requires the DLL: fill the alias on demand, clear it when
the action completes.** That was always the production design (#69, #70); the
conditional fill was an attempt to test without it, and it has now shown its
ceiling.

**The load-bearing unknown is therefore promoted to the critical path:** can
`ForceRefTo` be dispatched to a `BGSRefAlias` handle
(`GetHandleForObject(BGSRefAlias::VMTYPEID, alias)`)? Nothing else in the design
is blocked. If that route does not work, the fallback is a shipped `.psc` on the
command quest — which §4.7b ruled out on the grounds that a script could not
deliver the ATTACK verb, a judgement that does not apply to a one-line alias
filler.

### 0.27 THE DLL FILL WORKS — and the package DOES root them in combat (2026-07-22)

Native `TESQuest::ForceRefTo` (id 25052), no authored fill, no conditions.

**WORKS:** `MFO_ProbeSelect = 0` **returns control.** The claim/release cycle is
the production mechanism and it functions — MFO can take a follower, act, and
give them back.

**REFUTED — §0.23(a) was wrong.** The hypothesis was that the package does not
root them, the absence of combat does, and that combat AI would supply movement
while the package supplied casting. marth: *"He does still stop in combat."*
**A `UseMagic` package roots the actor even mid-fight**, with the same
`PKDT 0x00100000` / `PLDT type 12` that Mercer's combat override carries.

#### And "vanilla casters plant themselves too" is FALSE

marth: *"Vanilla casters certainly do not stand still."* Correct -- enemy mages
strafe, back off and close distance while casting. That kills the cheap answer.

**The reason is structural: vanilla combat casters are not running packages at
all.** They are running combat AI, which casts AND moves. A `UseMagic` package
is a "stand and cast" behaviour used by quest and scene NPCs -- Colette
practising, Ancano at the Eye, Mercer in a set-piece.

The `PLDT` "Place to Travel" data says the same thing:

| PLDT type | count | meaning |
|---|---|---|
| 0 near reference (radius 500/128/50/32) | 20 | walk to the marker, then cast |
| 12 NO LOCATION | 20 | cast where you stand -- **MFO's current value** |
| 6 object type / 2 near pkg start / 8 alias | 6 | |

Every one of those is *go there and stand*, or *stand*. **None is "cast while
moving freely".**

#### So this is a real architectural limit, and the choice is a design one

| | animated? | mobile? | deterministic? |
|---|---|---|---|
| Package (`UseMagic`) | yes | **NO** | yes -- MFO picks spell and target |
| Equip + let their AI cast | yes | yes | **NO** -- AI declines what it does not rate (§0.16) |

**MFO cannot currently have all three.** The honest options:

1. **Minimise the root.** Short `CastTime`, clear the alias the instant the cast
   fires. The follower stops for about a second per cast rather than standing
   through a whole exchange. Cheap, and worth measuring before anything else.
2. **`PLDT type 0` with a radius**, matching Ancano's shape (`radius 500`)
   instead of type 12. UNTESTED and it may only mean "walk there first", but it
   is a one-line change and 20 vanilla records use it.
3. **Hybrid:** equip-and-let-the-AI-cast as the normal path (mobile), and the
   package only when a rule must fire and the AI will not. Two actuators, and
   the board would have to say which one ran.
4. **Accept it** for casting specifically, and note that `act.attack` does not
   have this problem -- the targeting hook steers combat AI, which keeps moving.

**Not yet known:** whether option 2 changes anything. That is the next cheap
experiment and it needs no new mechanism.

#### The 10-second delay was ours, not the engine's

marth measured ~10 s between releasing the follower and getting control back.
The driver filled and cleared the alias and **never told the ACTOR to re-pick**,
so the engine got round to it on its own schedule.
`Actor::EvaluatePackage(true, false)` now runs immediately after both, with
`a_resetAI` FALSE always -- ALYSLC field-proved that resetting AI clears the
combat group and the next hit does zero damage, and this fires in combat by
definition.

### 0.28 THE INFLUENCE HOOK WORKS — and one caster symbol is not a caster (2026-07-22)

Hooked `CombatMagicCaster::CheckStartCast` (vtable 0x06) across the concrete
caster vtables. marth, force mode, in a fight: *"looked pretty good."* The
follower cast his own spell, mobile -- the mechanism the whole M9 arc was for.
MFO removes the AI's veto (§0.16) and the AI does the rest: movement, aim,
animation, magicka. Influence, not insertion.

**CTD, and it was a bad vtable in my list.** `mov ebx, [rsi+0x14]` with rsi = 1
is `magicItem->GetFormID()` on a garbage pointer -- `this+0x18` was not
magicItem because `this` was not a `CombatMagicCaster`. Cause:
**`VTABLE_CombatMagicCasterArmor` is a vtable symbol with NO class** in the
pinned headers; it does not derive `CombatMagicCaster`, so its index 6 is a
different function. The 14 that DO derive it all have headers. I added Armor
from a `grep VTABLE_CombatMagicCaster` on a guess -- #66a in C++: a symbol that
matches the name pattern is not proof it is the same kind of thing.

Removed, plus a guard: the thunk now returns immediately if the runtime vtable
is not one it recorded at install, so a future mis-hook is a no-op, not a crash.

#### The real finding: melee bias

marth: *"He didnt want to stop attacking with melee long enough to cast."* The
veto is removed, but a follower committed to melee does not ENTER the magic
branch often enough to notice the open permission. `CheckStartCast` only fires
when the AI is already considering a cast, and a sword-swinging combat style
rarely is.

This is the combat-style lever the §0.28 research flagged as a fallback,
promoted: a follower needs a magic-inclined disposition for the consent hook to
get frequent chances. Options, cheapest first:
1. Only expect reliable casting from followers whose style already favours magic
   (a court wizard, a Companions mage) -- the base case, and honest.
2. Nudge the combat style while a cast rule is latched -- surgically (clone,
   raise only `magicScoreMult`, restore on unlatch), accepting that it also
   shifts movement.
3. A stronger consent: also override the AVOID/attack decision so the follower
   breaks off melee to cast. Unexplored, and closest to "insertion".

**(1) is the right default.** A melee follower casting constantly would itself
look wrong; the gambit firing when the follower is disposed to cast is the
believable behaviour.

### 0.29 A CommonLibSSE-NG HEADER BUG shifts CombatController's AE layout (2026-07-22)

Two crashes, "fixed" twice, recurred identically -- because both fixes touched
the wrong side. Fable disassembled the deployed DLL and found the real cause.

**`CombatController.h` in the pinned NG guards its AE-only member
(`aimControllerLock`, a `BSSpinLock` at 0x68) on `#ifdef SKYRIM_SUPPORT_AE` --
a macro NG NEVER DEFINES.** NG's own build uses `ENABLE_SKYRIM_AE`;
`SKYRIM_SUPPORT_AE` appears in exactly one file and is dead in every build. So
the struct always compiles with the **SE layout**, and on the 1.6.1170 (AE)
runtime every member past 0x68 is **shifted +8**.

The consequence that crashed us: `cachedAttacker` compiles to 0xC8, but at
runtime 0xC8 holds `handleCount` -- an integer that equals **1 when the actor
fights a single enemy**. Our hook read `[cc+0xC8]` as an `Actor*`, got the
value 1, passed the null check, and faulted reading `formID` at `1+0x14 = 0x15`.
Deterministic; identical every time.

**The rule:** never dereference ANY `CombatController` member at offset >= 0x68
through this header -- `aimControllers`, `currentAimController`, `areas`,
`targetSelectors`, `cachedAttacker`, `cachedTarget` are all +8 at runtime on AE.
Use the members BEFORE 0x68 (`combatGroup` 0x00, `targetHandle` 0x2C,
`attackerHandle` 0x28), which are layout-stable, or read the shifted ones via
`REL::Module::IsAE()`-gated manual offsets.

**Scope fact, not a bug:** `CombatMagicCasterRestore` is also the caster for
POTION drinking (the drink-potion combat behavior uses it), so the CheckStartCast
consent hook governs a follower's potion use as well as spell casting. Worth
knowing when logistics and casting interact.

### 0.30 The Scheduler tick runs on a JOB WORKER thread, and TES::ForEachReferenceInRange tears its worldspace pointer (2026-07-22)

crash4: an `EXCEPTION_ACCESS_VIOLATION` inside the engine at
`TESWorldSpace::sub_30B0B3  mov rdi,[rsi+0xF8]`, with `rsi = 0x450FE000_45242000`
-- a **torn 64-bit pointer**, two mismatched 32-bit halves. Player in an exterior
(Wilderness), 9 min in, moving. The MFO frame above the engine was a `std::function`
inside `Logistics::LootNearby`.

**Two findings, one crash.**

1. **`RE::TES::ForEachReferenceInRange` is not safe in an exterior during a
   stream.** Its exterior branch walks `gridCells` and then ends with
   `worldSpace ? worldSpace->GetSkyCell() : nullptr` -- it dereferences
   `TES::worldSpace`. During a worldspace/cell transition that global was being
   rewritten, so the read tore and `GetSkyCell()` faulted on garbage. The fix:
   iterate the follower's OWN parent cell -- `GetParentCell()`, gated on
   `IsAttached()`, then `cell->ForEachReferenceInRange(pos, r, cb)`. The
   cell-level method walks only that cell's reference list under the cell's
   `BSSpinLock` and touches NO worldspace/grid/skycell pointer. Loot radius 600u
   fits one 4096u cell. Bonus: interiors were also being walked with
   `TES::interiorCell` (player-centered) at the FOLLOWER's origin -- the parent-cell
   walk fixes that latent mismatch too.

2. **The SKSE-task tick genuinely runs on a JOB WORKER thread**, not the main
   thread. crash4's stack is `... -> skse64 task delegate -> Job_Post_process ->
   BSJobs::JobThread`. So `Scheduler::Tick` and everything it calls
   (ServiceFollower, logistics, Loadout::Tick, PublishSnapshot) execute off the
   main thread, overlapping the cell-streaming threads. **The rule this sets:**
   any world walk must stay READ-ONLY inside the walk and MUTATE AFTER on
   re-resolved handles (LootNearby already does), and must iterate under an engine
   lock (an attached cell's list qualifies) -- never chase unlocked globals. See
   INVARIANTS #72.

### 0.31 MCM Helper rejects a whole config for one bad field; bind with `id`, not `modSettingName` (2026-07-28)

The MFO MCM never appeared in-game. MCM Helper's log (`MCMHelper.log` in the
prefix's `Documents/My Games/.../SKSE/`, NOT the modlist overwrite) showed
`Registered 9 mod configs` while **10** enabled mods had a `config.json` --
MFO was the one silently thrown out. A malformed config is not partially
loaded; the ENTIRE mod is dropped, with no per-mod error line.

Three things a `config.json` must get right, learned by diffing MFO against the
sibling mods **MEO and MAO** (both marth's, both register fine) in the SAME
modlist -- installed working mods are the primary source (#64):

1. **Bind each control with a top-level `"id": "key:Section"`, NOT
   `"modSettingName"` inside `valueOptions`.** `modSettingName` is not a schema
   field; its presence fails validation and drops the whole config. MEO/MAO use
   `id` everywhere and `modSettingName` zero times. This was the actual bug --
   all 18 MFO controls used `modSettingName`.
2. **`minMcmVersion` is required at root** (MEO/MAO/every LoreRim config declare
   it; value 9 is safe for basic controls).
3. **`enum` options go in `valueOptions.options`** (with optional
   `shortNames`), never a top-level `enumOptions`.

ESP-less config-only mods work fine: `sourceType: ModSettingBool/Int/Float`
persists to `Data/MCM/Settings/<modName>.ini`, which is also the tell that a mod
registered -- if that settings file never appears, MCM Helper rejected the
config. `defaultValue` inside `valueOptions` is valid.

### 0.32 How "Followers Can Loot" (Nexus 4744) actually loots, and why loose-item pickup is a PACKAGE feature, not a tick feature (2026-07-28)

marth pointed at FCL (powerofthree) as prior art for follower looting. We use
the METHOD, not the code. Teardown of the shipped 1.8 archive (ESP + BSA with
`.psc` sources; 3 `PACK`, 3 `FACT`, 3 `QUST`, 2 `SPEL`, 1 `MGEF`, globals):

* **The looting itself is NOT scripted.** The follower physically walking to an
  item and picking it up is done by **3 native AI packages** (Sandbox/Find-type
  "acquire"). That is the whole product. Papyrus does only bookkeeping.
* **Category filter = faction membership.** ~13 factions (ammo/armour/potion/
  keys/misc/ingredients/gold/books/…); the MCM adds/removes the follower from
  them; package CONDITIONS read membership to decide what to grab.
* **Combat/sneak gate = a global the package conditions read.**
  `OnCombatStateChanged` and the player's `tailSneakIdle`/`tailSneakLocomotion`
  anim events flip `CheckForCombatGlobal`/`ManualLootGlobal`; the packages only
  run when out of combat and the player is not sneaking.
* **Looted items reroute to a hidden "chest" per follower.** `OnItemAdded` on the
  follower alias → `RemoveItem` from the actor, `AddItem` to a paired container;
  the player activates the chest to collect. Followers are pack-mules.
* **SKSE tune:** `fAIDistanceRadiusMinLocation = 200` — the sandbox acquire
  radius. Independently the same 200u we chose for `kLootRadius`.

**What we take:** the engine-AI-package acquisition (real navmesh pathing to the
item, satisfies marth's "the follower must GO to the item") and the don't-loot-
while-sneaking gate. **What we reject:** the chest stash — MFO followers USE
loot (equip the better weapon, drink the potion); and the faction/MCM category
system — our GAMBIT BOARD is the category selector, finer and conditional
("loot arrows only when out of arrows") where FCL is unconditional.

**The decisive consequence for our own code.** The one thing the package method
gets for free — walking to a LOOSE world item and grabbing it — we CANNOT do
from our tick, and this crash4-reinforced fact is why. Picking up a loose ref is
`PickUpObject`, which tears down the ref's 3D and mutates the cell. Our tick runs
on a BSJobs JOB WORKER (§0.30). We had written a "marshal to the main thread"
via `SKSE::GetTaskInterface()->AddTask` — **that does not reach the main thread
in this runtime.** `Scheduler::Tick` is ITSELF scheduled with that same `AddTask`
(`Diagnostics.cpp`), and crash4's stack proves the queue drains inside
`Job_Post_process` on `BSJobs::JobThread`. Re-queuing from inside the tick lands
right back on a worker, one drain later — a delayed crash4, not a fix. So
`AddTask` in this plugin is "run soon on a worker," NOT "run on the main thread."
A real main-thread hop needs a genuine main-thread hook (a `Main::Update`-class
hook), which we have not built or measured.

Therefore loose-item pickup was CUT from the v0.7.x tick-loot path. It belongs to
the package-acquisition feature (ROADMAP "Loot Option A"): the engine walks the
follower and grabs the item — no `PickUpObject` on our side at all, exactly as
FCL's packages do. The shipping tick-loot path stays on the proven pattern:
read-only cell walk, then inventory TRANSFER (`RemoveItem`/`AddItem`) from
corpses/containers after the walk on re-resolved handles (§0.30 / #72). Inventory
transfer does not tear 3D, so it is worker-safe; `PickUpObject`/`Activate` are
not, and there is no shortcut around that.

### 0.33 The Travel package for Option A — authored from the vanilla shape, locally verified (2026-07-29)

Option A (walk-to-loot) needs a PACK riding the vanilla **Travel template
`00016FAA`**. Rather than guess the byte layout (the §0.17-class trap), dumped
`Skyrim.esm` with `esp_inspect`:

* **Travel template `00016FAA`** — PKCU inputs=3, version=3. Settable inputs:
  `0` Location (PLDT), `2` RideHorseIfPossible (Bool), `4` PreferPreferredPath
  (Bool). UNAM run `0/2/4`, XNAM `3`.
* **Exemplar `VC01FalionAtSummoningCircle` (0010FF16)** — a shipped
  alias-delivered Travel: `PLDT locType 8 -> ALIAS INDEX n` (radius in units),
  `QNAM` naming the owner quest, subrecord order EDID/PKDT/PSDT/[CTDA]/QNAM/
  PKCU/(ANAM+value)*/UNAM*/XNAM/POBA·POEA·POCA. This is the 263-instance WERoad
  movement pattern (§0.20).

`build_travel()` reproduces that verbatim; `MFO_TravelPackage (0x828)` rides
`00016FAA` with `PLDT t8 -> alias 1`, radius 128 (~arm's reach so the engine
stops the follower ON the loot), `QNAM -> MFO_LootQuest (0x80C)`. PKDT flags **0,
NOT kIgnoreCombat** — a fight must be able to pull the follower off looting.

DELIVERY is a SEPARATE quest, `MFO_LootQuest`, not a second package on
`MFO_CommandQuest`'s alias 0: a follower is only ever loot-travelling OR
cast-commanded, so two one-package quests avoid ALPC arbitration (which would
need conditions and a probe ladder to trust). alias 0 = follower (carries the
travel package), alias 1 = the DLL-filled loot ref the PLDT points at.

VERIFIED OFFLINE (no game needed): `audit_esp.py` PASS, `esp_inspect --selftest`
PASS (52 assertions incl. travel-package existence + template), and a
`--dump 01000828` byte-match to the exemplar. The NATIVE driver
(`Packages::TravelTo` mirroring `CastAt` + distance-arrival + release) and the
logistics dispatch are the next step, and like the cast package (§0.21 probes
1–5) will want 1–2 deck cycles to tune walk/arrival/release.

### NOT yet proven, despite the session
- ~~**The populated co-save ROUND-TRIP.**~~ **CLOSED — see §0.11.**
- (historical) v0.4.1 *saved* a real record twice
  (`saved 1 follower record(s), schema v2`) and *loaded* empty saves cleanly,
  but the save-with-a-record was never reloaded in-session. Save works; load
  of a real record is still unproven. **This is the next test.**
- **Boss/dragon multipliers on the right actors.** The boss test surfaced a
  bug (§0.7 / the v0.4.2 fix), so the corrected classification is untested.
- **`TESCombatEvent` volume** at scale — no large battle occurred.
- **The retention answer at all** — the one test was confounded (§0.6). Genuinely open.

The living truth for behavior will always be `native/plugin.cpp`. When this
file and the code disagree, the code is right.

---

## 1. Actor control — Tier A primitives

**Status: PROVEN (sibling).** Each has a working call site in shipped code.

| Mechanism | Call | Cited |
|---|---|---|
| Cast a spell as an actor | `ActorMagicCaster::CastSpellImmediate` | MEO Echo follower-share; MAO flask payload |
| Grant / revoke a spell | `Actor::AddSpell` / `RemoveSpell` | MEO startup grants |
| Remove an active effect | `ActiveEffect::Dispel(true)` | MEO `DispelStaleGemEffects` |
| Equip / unequip | `ActorEquipManager::UnequipObject` → `EquipObject`; hand slots from `BGSDefaultObjectManager` (`kLeftHandEquip` / `kRightHandEquip`); armor is slotless | MEO worn-ability cycle |
| Drink a potion | the equip path on an `AlchemyItem` | MAO consume intercept |
| Read actor state | `AsActorValueOwner()`, `HasSpell`, `HasPerk`, active-effect walk | all three siblings |
| Per-actor passives | `AddPerk` / `RemovePerk` | MRO — "the way to give player/follower-only passives" |

**Load-bearing details carried over:**

- **`Dispel` — collect the full list BEFORE dispelling.** Never dispel while
  walking the active-effect list.
- **The unequip→equip cycle is idempotent**: unequip drops all old abilities,
  equip installs exactly one, so repeated passes cannot accumulate. This is
  why it is the *only* complete teardown — `Update*Ability` early-outs on
  teardown, so a strip-and-restamp leaves old abilities alive and effects
  doubled.
- **`BGSBipedObjectForm::HasPartOf(mask)` is `.all()`** — a combined
  multi-slot mask is always false. Test each slot and OR.
- **`TESDataHandler::LookupForm<T>` takes CONCRETE record classes only.** It
  gates on `form->Is(T::FORMTYPE)`, and abstract intermediates like
  `TESBoundObject` inherit `FormType::None`, so it returns nullptr **100% of
  the time** — compiles clean, fails silently. Use `TESForm::LookupByID<T>`
  (routes through `As<T>()`) or the non-template lookup plus `->As<T>()`.
  *This one cost MEO a 10,146-row table that resolved "0 live."*

### 1.1 The Tier-A trap MFO steps on constantly

**Equip/unequip dispatch is SYNCHRONOUS into every registered sink.** Cycling
gear on a follower hands control, mid-call, to follower AI and to third-party
outfit managers — **guaranteed present in a Lorerim-class order** — which
mutate the same inventory. MEO ate a node use-after-free here.

Discipline, non-negotiable on MFO's equip path: snapshot `(object, xList,
key)` tuples first, act second, **re-find live records by key at act time**,
hold actors by `ActorHandle` re-resolved at act time.

`INVARIANTS.md` #2/#3. Status: PROVEN (sibling) — the *trap* is proven, which
is the part that matters.

---

## 2. Actor control — Tier B

**Status: RESEARCHED.** Mapped from SKSE64's `Actor.psc`, PapyrusUtil's
`ActorUtil.psc`, and po3's `PO3_SKSEFunctions.psc` — all of which ship their
sources inside the LoreRim install. **No one in this family has run any of
it.** Per the standing doctrine, the Papyrus native names the engine flow;
the implementation path is to read SKSE64 / po3 / PapyrusUtil source for what
each native actually calls.

| Capability | Papyrus native | Notes |
|---|---|---|
| Positioning | `KeepOffsetFromActor(target, x,y,z, angX,angY,angZ, catchUpRadius, followRadius)` / `ClearKeepOffsetFromActor()` | **Preferred.** A state set, not a journey. No package involved, explicitly reversible, composes with vanilla AI |
| Hold ground | `SetDontMove(bool)` | Trivially reversible |
| Combat targeting | `StartCombat(target)`, `StopCombat()`, `GetCombatTarget()`, `GetCombatState()` | State: 0 not in combat, 1 in combat, 2 searching |
| Combat-context cast | `DoCombatSpellApply(spell, target)` | Likely a better fit than `CastSpellImmediate` for gambit casts — verify which the engine treats as a real combat action |
| Projectile-level cast | `PO3.LaunchSpell(actor, spell, source)` | Alternative |
| Hand assignment | `EquipSpell(spell, source)` (0 left, 1 right), `EquipShout(shout)` | |
| Look-at | `SetLookAt(target, pathingLookAt)` / `ClearLookAt()` | Cheap, cosmetic, high perceived value |
| Stance | `StartSneaking()`, `DrawWeapon()`, `IsWeaponDrawn()`, `IsSneaking()` | |
| Package control | `EvaluatePackage()`, `GetCurrentPackage()`, `PO3.GetRunningPackage()`, PapyrusUtil `AddPackageOverride(actor, pkg, priority 0–100, flags)` / `RemovePackageOverride` / `CountPackageOverride` | **Last resort** — see the three rules below |
| Combat observation | `TESCombatEvent` (== `OnCombatStateChanged`), `OnPackageStart` / `OnPackageChange` / `OnPackageEnd` | |
| Full AI disable | `EnableAI(bool)` | Documented; **not used by MFO** — it is exactly the "seize control" this design refuses |

### 2.0 A Papyrus native is NOT a C++ binding (2026-07-21, review-found)

Before any of §2's table can be called RESEARCHED-and-reachable, note what a
Fable review of the M4 harness established by grepping the real headers:

**`KeepOffsetFromActor`, `ClearKeepOffsetFromActor`, `SetDontMove` and
`DoCombatSpellApply` do not exist in CommonLibSSE-NG or po3's fork.** They are
Papyrus-only natives. **`StartCombat` exists only in po3's fork**, as a
relocation thunk at `RelocationID(37608, 38561)` (SE/AE; no sourced VR id).

The research method was sound and the flows are real — but *"a Papyrus native
exists"* and *"I can call it from C++"* are separate claims, and only the
first had been checked. Reaching the rest means VM dispatch or a sourced
relocation; see `DESIGN.md` §4.5aa.

**Generalised rule: verify the BINDING, not just the mechanism.** Grep the
pinned library's headers before a design depends on a call being available.

### 2.1 Three facts found while mapping, each of which changed the design

1. **`PathToReference` is LATENT.** Its own doc: *"this method doesn't return
   until the goal is reached or pathing failed or was interrupted."* A call
   from the tick would stall the main thread for the duration of a walk.
   **Banned outright** (`INVARIANTS.md` #17) — there is no safe caller in
   this architecture.
2. **Package overrides PERSIST THROUGH SAVES.** PapyrusUtil states it
   plainly. An unledgered override outlives the mod and breaks the
   clean-uninstall promise. Hence the co-save ledger and the
   `CountPackageOverride` reconcile (`INVARIANTS.md` #19).
3. **`ClearPackageOverride` removes overrides added by OTHER MODS.** Banned
   (`INVARIANTS.md` #18). Same shape as NG's `RemoveByType`: a library call
   whose contract is wider than the caller's intent — obeyed before the
   crash, for once, rather than after.

### 2.2 Verification each Tier-B item needs before it ships

Per `DESIGN.md` §4.5, one mechanism per release, in reversibility order.
Each needs, minimally: does the call reach the actor at all (log the return);
does vanilla AI resume cleanly when released; does anything persist across a
save/load cycle that shouldn't; does it fight a follower framework's own
packages.

---

## 3. Follower enumeration and lifetime

**Status: PROVEN (sibling).**

- **Native:** iterate `RE::ProcessLists::highActorHandles`, filter to
  teammates. MEO's `ReapplyFollowerSockets` shape.
- **Papyrus cross-check:** `PO3_SKSEFunctions.GetPlayerFollowers()` — no
  quest alias needed. The documented fix for "works for player, not
  followers."
- **Hold by `ActorHandle`, re-resolve at act time.** Followers cross cells,
  get dismissed, and die mid-tick.

**RESEARCHED additions MFO wants:**
`PO3.GetCombatAllies(actor)` / `GetCombatTargets(actor)` (engine-maintained
target sets — prefer over hand-rolled hostility scans),
`PO3.GetCommandedActors(actor)` (summons),
`PO3.GetAllActorPlayableSpells(actor)` (**the action-vocabulary query**,
exactly), `PO3.CanActorDetect` / `CanActorBeDetected` / `IsDetectedByAnyone`
(stealth conditions), `PO3.EvaluateConditionList(form, actionRef, targetRef)`
(would let conditions be authored as engine CTDA data — the open option in
`DESIGN.md` §3.2).

---

## 4. Event sinks

**Status: PROVEN (sibling)** except where noted. Register on
`RE::ScriptEventSourceHolder`; **defer all mutation to `AddTask`.**

| Event | Shape | Gotcha |
|---|---|---|
| `TESDeathEvent` | `{actorDying, actorKiller, dead}` | **Fires twice — act only on `dead == true`** |
| `TESCombatEvent` | combat state change | **UNKNOWN to this family.** No sibling has used it. Verify: does it fire for followers as well as the player's targets; does it fire on searching↔combat transitions; does it fire during load |
| `TESSpellCastEvent` | `{object, spell}` | Fires for lesser powers too — this is what makes the power-opener work |
| `SKSE::CrosshairRefEvent` | `{crosshairRef}` | via `GetCrosshairRefEventSource()` |
| `MenuOpenCloseEvent` | menu name + opening | **LoadingMenu-CLOSE is the "gameplay resumed" anchor** — never a blind timer; one fired during a long load is swallowed |
| `TESEquipEvent` | `{actor, baseObject, uniqueID, equipped}` | Synchronous into follower AI (§1.1). Plain items report `uniqueID = 0` |

**The load-wide performance warning that shaped `DESIGN.md` §4.2:** MRO found
that a *global* SKSE actor event (`RegisterForActorAction`) fires for **every
actor in the load order**, and each firing is dispatched to the handler even
when it bails immediately. **The cost is the dispatch, not the handler body,
so filtering inside the handler does not help.** On a large list this tanked
FPS. MFO's sinks are all low-frequency by comparison, but the lesson governs
any future "watch every actor do X" idea.

---

## 5. Co-save serialization

**Status: PROVEN (sibling).** Full rules in `INVARIANTS.md` §B; the
mechanisms:

- Versioned records; **keep readers for every shipped version forever**;
  write only the newest.
- **Every stored FormID through `SerializationInterface::ResolveFormID`.**
  Applies to dynamic FF ids too — an unresolvable one means the object is
  gone; recreate, never reuse.
- **SKSE does NOT round-trip unread records** — a downgraded DLL destroys
  newer ones on its next save.
- Store **stable string identities**, never enumeration indexes.
- `RevertCallback` zeroes everything save-scoped.
- **One-time grants:** consume the latch only when the grant actually
  succeeded — a missing ESP must retry next load, not burn it.

**GlobalVariable values are save-persisted.** A DLL that writes a global at
`kDataLoaded` is overwritten when a save loads. Re-assert on `kPostLoadGame`
**and** `kNewGame`. (MRO lost a release to this on its DR handshake.)

---

## 6. The ImGui board

**Status: PROVEN (sibling)** — MEO shipped and field-validated this under an
ENB/Community Shaders stack on 1.6.1170, verified against `D7ry/wheeler`.
MFO copies it wholesale; full implementation brief in `DESIGN.md` §6 and the
hook table in `ARCHITECTURE.md` §5.

Facts worth repeating here because they are engine-level, not design-level:

- **`io.DisplaySize` lies under Proton and upscalers** — the Win32 backend
  reads `GetClientRect`, which disagrees with the backbuffer. Cache
  `sd.BufferDesc.{Width,Height}` and overwrite every frame between
  `ImGui_ImplWin32_NewFrame()` and `ImGui::NewFrame()`.
- **Renderer access (NG 3.7):**
  `RE::BSGraphics::Renderer::GetSingleton()->data.{forwarder, context,
  renderWindows[0].swapChain}`; hwnd from `swapChain->GetDesc().OutputWindow`.
- **Input:** while open, walk the event list into ImGui IO and set
  `*a_events = nullptr`. The game sees nothing — no vanilla bleed-through, no
  control-flag toggling, no stuck-controls failure mode.
- **`SKSE::AllocTrampoline(256)`** — MEO's ENGINE_NOTES says 64; **the
  shipped code uses 256. Trust the code.**
- **Build traps:** `d3d11.h` pulls `windows.h` (which NG never includes) —
  `WIN32_LEAN_AND_MEAN` + `NOMINMAX` or its min/max macros break
  `std::max`/`std::clamp` everywhere; and `wingdi.h` `#define`s
  `GetObject`→`GetObjectW`, hijacking
  `BGSDefaultObjectManager::GetObject<T>()` — **`#undef GetObject` after the
  D3D includes.**
- **Hiding a vanilla menu fires that menu's own CLOSE event.** Any "that menu
  closed → close mine" coupling must be gated or it kills the menu you just
  opened.

---

## 7. Records and forms

**Status: PROVEN (sibling).** MFO's record needs (MGEF, SPEL, KYWD, QUST +
VMAD, SEQ) are a strict subset of what MEO's
`MANUAL_MOD_CREATION_GUIDE.md` already documents. Copy that file rather than
re-deriving.

The two that bite:
- **SPEL type must be 4 (Ability) for a constant ability**; type 3 is Lesser
  Power. A lesser power is what MFO's Field Orders opener actually wants.
- **The engine drops malformed records silently.** Dump a vanilla twin and
  diff subrecords — type, order, size, bytes.

---

## 8. Traps inherited that will reach MFO

Listed because each is a live hazard on a path MFO will walk:

- **`ExtraDataList::RemoveByType` null-derefs when the removal empties the
  list** (disasm-proven, MEO m50, a deterministic every-load CTD). Never call
  it. MFO touches extra data far less than MEO, but any code that strips
  extras must own its empty-list behavior.
- **`AddObjectToContainer` LINKS the `ExtraDataList` pointer — it does not
  copy.** The entry takes ownership. Two owners of one allocation, one of
  them freeing, is the whole bug class. *"When a fix hands memory across an
  API boundary, prove who owns it, don't infer it from behavior."*
- **`PlaceObjectAtMe` refs have no owner** — `SetOwner(player)` before any
  pickup, or it is witnessed theft.
- **Never give an item two live effect sources.**
- **po3 Tweaks' editorID caching populates the map for ALL forms, and LoreRim
  ships it** — so an editorID lookup that works on this machine may fail for
  Nexus users. **Never validate an editorID lookup on the dev deck alone.**
- **po3 per-form events only deliver to scripts extending ObjectReference,
  ActiveMagicEffect, or ReferenceAlias.** Registering a Quest script
  "succeeds" and never delivers. (Relevant only if MFO ever grows a Papyrus
  surface — it currently must not.)

---

## 9. The verification queue

What MFO must actually establish in-game, in phase order. **This is the real
content of this document right now.**

| # | Question | Phase | Method |
|---|---|---|---|
| 1 | Does the co-save round-trip a rule list across a load-order change? | P0 | Add/remove a plugin between saves; check `ResolveFormID` drops cleanly |
| 2 | Does teammate detection catch NFF/AFT-managed followers? | P1 | Install a framework; log the detected set vs `GetPlayerFollowers()` |
| 3 | Does `TESCombatEvent` fire usefully for followers? | P1 | Log every event with actor/state; watch a real fight |
| 4 | Real kills/hour on a Requiem-class list | P1 | Instrument Rapport income for several sessions |
| 5 | Does the evaluator hold its budget under a real fight? | P2 | `bProfileEvaluator`, Lorerim order, multi-follower, worst-case rule list |
| 6 | Is per-tick cost flat from 1→12 followers? | P2 | Same run, vary party size |
| 7 | Does the frame clock adapt (7.5 ticks/s at 30/60/144)? | P2 | Same run, cap framerate |
| 8 | Does `CastSpellImmediate` or `DoCombatSpellApply` read as a real combat action? | P2 | Both, on the same spell, observe AI and animation |
| 9 | Does a load screen produce a tick burst? | P2 | Instrument tick timestamps across a load |
| 10 | Does controller nav reach every board action including reorder? | P3 | Gamepad only, no keyboard |
| 11 | Does `IsItemActivated()` hold against the task-pump race? | P3 | Rapid clicks on a delete row; count actual deletions |
| 12 | Does uninstall leave zero MFO spells on a follower? | P5 | Tutor, uninstall, inspect save |
| 13 | Do package overrides survive a save/load, and does the ledger catch it? | P6+ | Apply, save, reload, `CountPackageOverride` |
| 14 | Does a commanded target actually **stick** — does the combat controller keep it, or re-pick within seconds? | P6+ | Issue once, log `GetCombatTarget()` every tick for 60 s without re-issuing. **This is the load-bearing question for §4.7** — if the AI re-picks, the standing-order model needs a refresh cadence rather than pure invalidation |
| 15 | Does `EvaluatePackage()` need the §4.5a condition flicker from native code, and does it need the delay? | P6+ | Set global, evaluate, read `GetCurrentPackage()`; then flicker and compare |
| 16 | Does an alias `ForceRefTo` from native code drive a conditioned package the same way Papyrus' does? | P6+ | The whole declarative route rests on this |

---

## 10. Promotion protocol

An entry moves to **PROVEN (MFO)** only when it has:

1. A **date** and the **game version** it was tested on.
2. The **observed symptom** that constitutes proof — not "it worked," but
   what was seen. *"Follower cast Fast Healing within 400 ms of the ally
   dropping below 50%, logged 14 times across 3 fights"* is proof;
   *"healing works"* is not.
3. A **log line or console command** that reproduces the observation.

Then, per `INDEX.md`, the mechanism is written up **here and into
Linux-Native-Tools in the same release that ships it** — MFO is the project
that owes the family an actor-AI document, and that debt is paid
incrementally or not at all.

**Instrument, don't eyeball.** Temporary `spdlog::info` dumping the values in
question, reproduce in-game, read the log, strip before release. A mechanism
"confirmed" by watching a follower and feeling good about it is not confirmed.
