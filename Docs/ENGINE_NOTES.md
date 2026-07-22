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
