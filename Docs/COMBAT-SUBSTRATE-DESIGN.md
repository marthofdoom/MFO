# Combat substrate and animated casting everywhere: design

Status: DESIGN ONLY (2026-10-12/13, Opus design pass; revised 2026-10-13 with marth's answers: the anchor must be an actor (3.3), the follower keeps moving under its own package (3.4), AUTO attack series approved, the two holes approved). No production code. ClickUp 86e3f1nf5 (due 2026-10-16).
Repos: MFO `main` e511b2f (code read at 955645e; no cast-road change between them), Harbinger (APMF) `main`
3f22ae4 (includes the engagement hold 52d55c4).

Every claim below is marked **PROVEN** (disassembly of our unpacked executables, or a field log) or **INFERRED**.
Addresses are RVAs (VA = 0x140000000 + RVA). AE = 1.6.1170, SE = 1.5.97. Ids are Address Library ids.

## 0. The rulings this design serves

- marth 2026-10-12: "Getting animations for everything is non negotiable." Every follower cast is animated, in and
  out of combat.
- "We dont want any other spells except for the gambited ones to cast." The ch.8 allow-list deny stays.
- "We absolutely want Combat substrate and OOC animated casting."
- The gambit's spell equip (`Loadout::Prepare`'s direct `EquipSpell`) moves to Harbinger with this work.
- marth (relayed 2026-10-12 night): **"weighting gains us nothing, unless its the only path to animation."**
  So the design uses claims, pins and denies, and uses weighting only where nothing else animates. Section 2.3
  shows there is no such place on any cast road. The ch.8b score steer (`EnableScoreSteer`) may stay on, but
  nothing here depends on it.
- Principles: think with portals, deny-completeness licenses state-forcing, composition not substitution,
  disassembly proves a path exists not that it runs, never mask a failure.

## 1. Every road a follower cast takes today

Two tables run casts. The **combat table** runs in `Actuation::Fire` (`cast/Fire.cpp`) when the follower has a
combat controller. The **out-of-combat (OOC) table** runs in `logistics/Service.cpp` (the
`kActCastSelf/Target/Player` block at about `:1478-1975`). The road for one cast is picked by
`ComposedCast::ChooseHealRoad` (`ComposedCast.cpp:770`), `ChooseBuffRoad` (`cast/BuffRoad.cpp:287`), the forks in
`CastOn` (`cast/CastOn.cpp`) and `CastAuto` (`cast/Auto.cpp`).

Every claim road below is the follower's own AI casting through Harbinger's ch.8b seats. That road is animated
(PROVEN for Restore: LoreRim 2026-10-12 `[ctcensus] ... VERDICT: FIRED via Restore (the hand charged: ANIMATED`,
30 windows across APMF.log.1-5). Every direct road is `CastSpellImmediate` on the kInstant caster, which plays no
animation (PROVEN, ENGINE_NOTES §0.8 / §0.13).

**Table 1. Per-road status (APMF present, ABI 22, MFO main)**

| # | When | Spell kind | Road today (file:line) | Animated? | Needs weighting? | Target state |
|---|---|---|---|---|---|---|
| R1 | combat | heal-other, heal-self (Heal kind, Self delivery for self) | ch.8b heal claim: `CastOn` heal road `:375` -> `ComposedCast::Try` | **yes** (PROVEN, field) | **no**: seat 0 classify mints the Restore item, 0x0F denies rival spells on the hand, the hand block refuses weapons | keep |
| R2 | combat | heal at self whose delivery is not Self | `ChooseHealRoad` -> `DirectNoSeat` -> direct stream | **no** | no | claim with the caster's own FormID as target (Harbinger's self-flip proxy, as the buff road already does for `ExplicitSelfBuff`) |
| R3 | combat | self buff: armor, cloak, invisibility, bound item, ward (native rows) | ch.8b buff claim: `CastSelfDirect` -> `BuffSelfClaim` (`DirectSelf.cpp:146`) / `CastOn` self fork | **yes by design, NOT field-run** (no `[buff]` line in any LoreRim log) | no: native row, 0x0F deny | prove it (probe P1) |
| R4 | combat | kSelf light (Candlelight), summon, reanimate, rowless (muffle, fortify, cures, ...), buff at an ally or the player | ch.8b buff claim (`BuffClaim`, `CastOn:791`), needs ABI >= 19 | **yes by design, NOT field-run** | no: seat 0 files the spell under its row or Script | prove it (P1) |
| R5 | combat | summon when the buff road answers anything but Claim | `CastSummonOnce` -> `Summon.cpp:352` `CastSpellImmediate` | **no** | no | the R4 claim; the direct road becomes the APMF-absent degrade only |
| R6 | combat | aimed Light (Magelight), self-cast rune (kTargetLocation), ambiguous multi-row spell (`rowApprox`) | `ChooseBuffRoad` `DirectNoSeat` -> direct | **no** | no | research item RS1 (no proven animated road yet) |
| R7 | combat | fire-and-forget offense at a foe | owned offense claim (`CastOn`, `ClaimOffenseCast`) | **yes** (PROVEN 2026-09-06, 17 animated offense fires) | **no**: 0x0F deny + hand block; offense shares category 0 with weapons, and the hand block, not a score, keeps the weapon out | keep |
| R8 | combat | concentration offense or non-Restoration buff, any target | `ClaimOffenseCast(conc=true)` (`DirectSelf.cpp:178`, `DirectTarget.cpp:198`) | **yes by design** | no | keep (note: this branch has no controller test; under the substrate it gets one) |
| R9 | combat | Restoration non-heal (ward, Restoration-school buff) at an ally, concentration or fire-and-forget, when no claim road took it | `ConcentrationCast` -> `CastTargetDirect` stream (`CastOn:807`), `RestorationCastDirect` (`CastOn:822`) | **no** | no | claim. The 2026-09-21 reason for the exclusion (`fix/mfo-combat-restoration-direct`, MFO's own direct cast colliding with its own claim on the instant caster) was fixed in Harbinger d41ed43, the same fix that let heals back onto the claim road |
| R10 | combat | **AUTO hostile** (the board's Auto target on an offense spell) | fan-out: one `CastSpellImmediate` per foe in one tick (`Auto.cpp:163/176` via `ApplyEffectFromTo`) | **no** | no | a SERIES through `CastOn`, one foe at a time (the shape AUTO heal and AUTO buff already use). **Approved by marth 2026-10-12** |
| R11 | combat | AUTO heal, AUTO buff | series through `CastOn` (claim per recipient) | **yes** (heal PROVEN; buff by design) | no | keep |
| R12 | OOC | every heal (self, ally, player, AUTO) | `CastSelfDirect` / `CastTargetDirect` direct stream, or posted `CastSpellImmediate` (`Service.cpp:1892`) | **no** (PROVEN: 19 `[heal] ... out-of-combat heal -- INSTANT road, unanimated` lines on LoreRim 2026-10-12) | no | substrate + ch.8b claim (section 3); the follower keeps moving under its own package (3.4) |
| R13 | OOC | every buff, ward, light, AUTO buff fan | direct (`CastSelfDirect`, `ApplyEffectFromTo`, posted `CastSpellImmediate`) | **no** | no | substrate + claim; the follower keeps moving under its own package (3.4) |
| R14 | OOC | summon | `CastSummonOnce` direct | **no** | no | substrate + claim; the follower keeps moving under its own package (3.4) |
| R15 | OOC | fire-and-forget hostile at a foe | `Packages::CastAt` UseMagic package (`Service.cpp:1941`), else direct (`:1966`) | package: **yes but rooting**, a substituted package; direct: **no** | no | an OOC hostile cast at a foe is a combat start. It becomes ch.21 entry + the R7 claim (a real fight, no substrate) |
| R16 | OOC | concentration at any target | `CastTargetDirect` stream | **no** | no | substrate + claim, held for the channel; the follower keeps moving under its own package (3.4) |
| R17 | either | Harbinger absent or `bLegacyCastHybrid` | legacy hybrid (`ForceCast`, `CastOn:1656`) | partly | n/a | unchanged. The APMF-absent degrade, out of scope |

**The column that matters.** "Needs weighting" is **no on every row.** Each animated road is a claim, and what
makes the claimed spell the one that fires is a deny (0x0F `CheckShouldEquip` on the claimed hand, the ch.8
allow-list, the hand block), plus seat 0 classify where the engine would build no candidate at all. No weighting
is involved. Section 2.3 gives the proof per category.

## 2. In combat: every forced cast becomes a claimed cast

### 2.1 What is left to convert

Most of this is already built. The buff, summon, rowless and ally-buff claim road landed on 2026-10-05
(`cast/BuffRoad.cpp`), but **no buff, ward or summon claim has ever run in the field**. The LoreRim logs of
2026-10-12 have zero `[buff]` lines, and Harbinger's claim census shows claims only for Restore. So
"forced casts become claimed casts" means:

1. **Prove R3/R4** in the field (probe P1, no code).
2. **Convert R2, R5, R9, R10** (slice S1, MFO, tier B).
3. **Research R6** (RS1).

### 2.2 Does the engine fire each caster type? (evidence)

Harbinger's caster-type census counts every actor's casts (`[ctcensus] HEARTBEAT ... ENGINE-WIDE (every actor)
fires [...]`). The LoreRim session of 2026-10-12 shows the engine's own `CombatMagicCaster` FIRING these types for
NPCs (last heartbeat of each log, APMF.log.4 / .2 / .1 / current):

| Type | engine-wide fires (4 logs) | CheckStartCast calls | Claimed by MFO |
|---|---|---|---|
| Restore | 15 / 62 / 12 / 4 | 68 / 332 / 120 / 38 | yes, fired 11 / 55 / 7 / 4 |
| Offensive | 17 / 8 / 18 / 2 | 220 / 11 / 50 / 3 | not claimed in these runs |
| Armor | 0 / 6 / 7 / 7 | 0 / 19 / 30 / 14 | never |
| Cloak | 0 / 2 / 3 / 0 | 0 / 2 / 18 / 0 | never |
| BoundItem | 0 / 2 / 0 / 1 | 0 / 8 / 0 / 1 | never |
| Summon | 0 / 0 / 1 / 1 | 0 / 11 / 55 / 2 | never |
| Reanimate | 0 / 0 / 4 / 0 | 0 / 0 / 26 / 0 | never |
| Ward | 0 / 1 / 0 / 0 | 0 / 7 / 28 / 1 | never |
| Stagger | 0 / 0 / 5 / 0 | 0 / 0 / 164 / 0 | never |
| Script (rowless) | 7 / 4 / 4 / 2 | 13 / 4 / 4 / 2 | never |
| Light, Invisibility, Disarm, Paralyze, TargetEffect | 0 | 0 | never |

**PROVEN (field):** the engine's combat AI builds and fires Armor, Cloak, BoundItem, Summon, Reanimate, Ward and
Script casters on its own, and every fire goes through the same caster seats Harbinger answers. So the claim road
for R3/R4 is a road that RUNS, not only one that exists.
**NOT PROVEN:** that a CLAIMED buff, summon or ward fires. Harbinger's claim seats answered only for Restore in
every log (`claim seats answered 0x06/0x07/0x0A/0x0D [Restore=..., Ward=0/0/0/0, Summon=0/0/0/0, ...]`).
**NEVER SEEN, any actor:** Light, Invisibility. A claimed Light has no field evidence at all. Treat R4's light row
as unproven until P1 shows a `BUILT type=Light`.

### 2.3 Which categories score these items, and why no weighting is needed

From the 2026-10-12 RE (`Docs/WEIGHTING-INVENTORY.md`, ENGINE_NOTES §0.42 correction, PROVEN on both runtimes):
`CombatInventory::AddItem` (AE `0x811AC0`) files an item by `GetCategory`. Restore is 1. Ward is 2 or 3. Summon,
Armor, Cloak, Invisibility, BoundItem, Stagger, Paralyze and Reanimate are 4 or 5. Light and Disarm are 4.
Offensive spells are 0, with the weapons. The selector walks the fixed order `[1, 2, 4, 0, 3, 5, 0, 6]`, first
come, and compares scores only inside a category.

Why no weight is needed on any row:

- **Inside the claimed spell's category**, the rival spells are refused at `CheckShouldEquip` (0x0F) on the claimed
  hand, and the ch.8 allow-list refuses every non-gambit spell on both hands. With the rivals refused, the claimed
  spell is the only spell left, and no score comparison is left to tip.
- **Across categories**, nothing is scored at all (first come). Categories 1, 2 and 4 are walked before weapons, so
  a buff or heal claim is never beaten by a weapon. Category 5 is walked after weapons, but the hand block closes
  the claimed hand to every weapon, shield and torch.
- **Offense (category 0) against weapons** is a real score comparison, and the hand block, not a score, keeps the
  weapon off the claimed hand.
- **When the engine would build no candidate at all** (a beneficial spell aimed at someone else, a rowless spell),
  the fix is seat 0 classify, not a weight. A weight cannot lift a candidate that does not exist, and Harbinger's
  steer "never lifts a 0-scored item" (D1).

**The one place a weight could matter**, and why we still do not need it: a melee-committed follower in a real
fight may rarely enter the magic branch of its behaviour tree (§0.28 "melee bias"), so `CheckStartCast` is seldom
asked. The candidate lever is the combat-style magic multiplier, which is a weight. The deny alternative already
exists: a short ch.7 deny of the weapon leaves while a cast claim waits to fire makes the tree fall to its magic
branch. That needs ch.7 to gain a weapon-only bit, on the same ActionGate act()/pop() thunks the substrate's leaf allow-list uses (section 3.5). Use the deny.
Field status: claims fire in 0.63-2.85 s (CAST-DELIVERY, archer heals), so no field case has yet needed either.

Known non-weighting failure shape to keep apart: a DUAL-CAST heal claim (`hand=D`) that builds and is answered
YES but never charges (`VERDICT: BUILT Restore=1 NOT FIRED (CheckStartCast YES 22 / NO 23)`, 14 windows on
LoreRim). That is the dual-hand claim, not a score problem. It belongs to the archer-heal work.

### 2.4 The probe still missing for the in-combat half

**P1 (field run, no code).** Run Harbinger with `[Probe] bCasterTypeCensus=1` (default) and give two followers
combat rules for: Oakflesh (Armor, self), Flame Cloak (Cloak, self), Bound Sword (BoundItem, self), Conjure Familiar
(Summon), Lesser Ward (Ward, self, concentration), Candlelight (Light, kSelf), Muffle (rowless, Script) and one
ally buff (Oakflesh at the player). Pass criteria per spell, all three:

1. MFO.log: `[buff] <id> <name> (<spell>): ... CLAIM at <target> (animated; the engine's own <Row> caster decides when it fires)`.
2. APMF.log: `[ctcensus] <id> BUILT type=<Row> ...` then `[ctcensus] <id> CLOSE ... VERDICT: FIRED via <Row> (the hand charged: ANIMATED`.
3. No `[buff] ... claim never fired ... released` WARN, and no `NOT BUILT`.

A row that reads `NOT BUILT` or `BUILT ... NOT FIRED` is a seat gap in Harbinger for that row. Fix it there,
never by falling back to the direct road.

## 3. Out of combat: the substrate

### 3.1 Why a controller is needed (PROVEN)

Every ch.8b seat is a vfunc taking a `CombatController*`. The engine builds the caster candidates inside
`CombatController::Update` only (§0.42: `0x80F380` <- only `0x558A84` <- only `0x6B7042`, the per-actor combat tick
`0x6B6E70`, which skips the whole block when `actor+0x160` is null). With no controller there is no candidate, no
caster and no seat. So an animated cast decided by the AI needs a controller. That is the substrate.

### 3.2 How a controller is born (PROVEN, disassembly of `Actor::StartCombat`)

`Actor::StartCombat(Actor* this, Actor* target, CombatGroup* join)`, AE id 38561 `0x6B6930`, SE id 37608
`0x6251B0`. The only non-load road to a controller (Harbinger INVARIANTS #0 (h), CombatManager AE 46873/46874,
SE 45573/45574). What it does, in order (AE addresses; SE twins verified where given):

| Step | AE | What | Status |
|---|---|---|---|
| refuse if `this` is one engine-global actor | `0x6B6985` | identity test | PROVEN |
| refuse on life state 3 or 6 (unconscious, restrained) | `0x6B6992` | | PROVEN |
| **refuse if `target == this`** | `0x6B69B3` (SE `0x625231`) | `cmp rdx, rdi; je refuse` | **PROVEN, both runtimes** |
| refuse if `this->IsDead(true)` (slot 0x99) | `0x6B69C4` (SE `0x625242`) | ch.22's seat | PROVEN |
| refuse if `target->IsDead(false)` (slot 0x99) | `0x6B6A04`, return `0x6B6A0A` (SE `0x625282`, return `0x625288`) | the TARGET's check, a separate return address | PROVEN |
| refuse on a distance test (a float from `0x667260`/id 37346 equal to 0, then distance from `0x2EDC50`/id 19824 against a radius from `0x6749D0`/id 37478) | `0x6B6A30-0x6B6A6B` | | PROVEN shape, names INFERRED |
| global spinlock | `0x6B6A85` | | PROVEN |
| already in combat: add the target (`0x559150`, id 33228) | `0x6B6D39` | | PROVEN |
| a process flag (`AIProcess+0x136` bit 2) set on `this` when the target does not regard it as hostile (`0x679650`, id 37533) | `0x6B6AED-0x6B6B7F` | likely the AGGRESSOR mark for crime reporting | flag PROVEN, meaning INFERRED |
| a weapon re-arm equip (`0x6C9730`, id 38893, next to EquipObject 38894) | `0x6B6BED` | the "re-arm equip" ch.17 already governs | INFERRED name |
| `EndInterruptPackage(false)` (id 37474, CommonLib-named) | `0x6B6C54` | | PROVEN |
| maybe end the current package (`0x70F4C0`, id 39850) unless its type is in mask `0x28004000000` | `0x6B6C96` | | INFERRED |
| **build the controller**: with a join group `0x83D2C0` (46874), else with a target `0x83D180` (46873), stored at `+0x160` | `0x6B6CA5-0x6B6CD5` | | PROVEN |
| **no target and no group: no controller is built** | `0x6B6CBE` falls to `0x6B6CDC` | | **PROVEN** |
| in combat: a vfunc 0x4A result passed to `0x3A0D20` (id 24131) | `0x6B6D0D` | INFERRED: the combat-start topic or event | INFERRED |

### 3.3 Who is the combat target? (the research question of 86e3f1nf5)

marth asked (2026-10-12): "Can it be an inanimate object? since we are blocking combat movement, it could be anything
nearby in that case?" **No. The engine needs an ACTOR, and the disassembly shows why.**

**Why a non-actor reference cannot be the target (PROVEN, AE 1.6.1170; SE has the same shape):**

- `Actor::StartCombat` reads Actor-only fields of its target before it builds anything. It reads `target+0x204` at
  `0x6B6AED` (AE `Actor::boolFlags`, runtime data `+0x1FC` SE / `+0x204` AE) and `target+0xF8` at `0x6B6B4F`
  (`currentProcess`). A `TESObjectREFR` is `0x98` bytes (fork `TESObjectREFR.h:511`). For a static, an activator,
  furniture or an XMarker, both reads land past the end of the object. The function would act on garbage: undefined
  behaviour, not a refusal.
- Everything downstream holds the target as an actor. `CombatTarget::targetHandle` (+0x00) is an `ActorHandle`
  (fork `CombatGroup.h:26`). `CombatController` keeps `targetHandle` (+0x2C) as an `ActorHandle` and `cachedTarget`
  (+0x68) as a `NiPointer<Actor>`. The selector, the target-detection pass and the cast seats resolve it as an
  `Actor` and call Actor methods on it. These declarations are INFERRED as ABI facts (we never trust CommonLib for
  ABI). The StartCombat field reads above are PROVEN.
- So the XMarker MFO keeps as its evict marker (base `0x3B`, `Packages.cpp`) cannot be reused, and neither can
  "anything nearby". A nearby REAL actor is out for the reasons in the table below.

| Candidate | Verdict | Why |
|---|---|---|
| Any non-actor reference (static, activator, furniture, the evict XMarker) | **impossible** | Actor-only reads past the end of a 0x98-byte REFR (above). PROVEN |
| The follower itself | **impossible** | refused at `0x6B69B3` (SE `0x625231`). PROVEN |
| No target, a bare controller | **impossible** through StartCombat | no target and no group builds nothing (`0x6B6CBE`). PROVEN. Building one by hand skips the engine's entry and ch.22's seat. Rejected |
| The heal recipient, or another nearby actor | **rejected** | it becomes the follower's combat target. StartCombat sets the aggressor mark (`AIProcess+0x136` bit 2) because the target does not regard the follower as hostile. That actor's AI and its allies may answer (INFERRED). A self-buff has no recipient |
| The player | **rejected** | combat music, a teammate in combat with the player |
| **A Harbinger anchor actor** | **chosen** | below |

**The answer: the combat target is an ANCHOR ACTOR, and the cast target is the claim's target.**

- **The anchor is a small POOL of persistent anchor actors authored in `APMF.esl`.** It is not spawned with
  PlaceAtMe. The refs live in a Harbinger holding cell. Each is invisible, a ghost, without collision, AI off, in no
  factions, unaggressive, essential and level 1.
- **Open:** Harbinger moves one anchor to the follower.
- **Close:** it moves the anchor back home.
- **Pool size:** one anchor per concurrent substrate (a short fixed count, for example 8). A follower that finds the
  pool empty waits one lap. The wait is logged.
- **Why a pool, not PlaceAtMe:**
  - nothing new enters the `.ess`;
  - no reference is created or deleted;
  - the load sweep shrinks to "move every anchor home".
- **The cast target** is the ch.8b claim's own `target`. Seat `GetMagicTarget` (0x0A on the combat casters) hands
  that target to the AI, whatever the combat target is:
  - a heal: the recipient;
  - a self-buff or a summon: target 0 (self, or the caster's placement);
  - an ally buff: the ally.
- **Already proven in the field:** that the seats aim at the claim target while the combat target is another actor.
  Every claimed heal on LoreRim was cast at an ally while the controller's target was a foe.
- **Range (INFERRED):** the selector's range gate (`0x813A2F`, vfunc 6) measures the item against the combat target.
  An anchor at the follower is in range of every spell, touch spells included.
  - The follower keeps walking during the substrate (section 3.4), so Harbinger moves the anchor with it each frame
    (`SetPosition` on the main thread).
  - Probe P3 checks that a touch-range claim still fires after the follower has walked about 200 units.

### 3.4 Movement: combat must not drive the body, the follower's own package does

marth (2026-10-12): "block combat from affecting movement and have them continue using the non combat package for
movement". So there is no stand-and-cast. While the substrate is up, the follower keeps following, travelling or
sandboxing under its own package while its hands cast.

**What the engine does with a controller (PROVEN unless marked):**

1. **Combat owns the body by default.** The combat tree is a Parallel of a Movement half and an Action half
   (`CombatBehaviorTreeCombat`, AE builder `0x85F7D0`). The Movement half's selector re-selects every 0.25 s and
   always runs some movement leaf, because "Close Movement" has a constant value of 1. This is Harbinger's RE of
   2026-10-05, on all three runtimes (`channels/CombatApproach.cpp` header, agentlog `apmf-combat-moveto.md`).
2. **The package stops running in active combat.** `0x6B6DF0` (id 38563) is "in active combat". It returns
   `IsInCombat()` (slot 0xE3) and NOT `process->ignoringCombat` (`AIProcess+0x139`, read by `0x6D6780`). The package
   gate `0x6D8950` (id 39199) lets a package run only when that test is false, except for package types 26 and 42
   and one procedure class. The function and the test are PROVEN; that this is the "may the package run" gate is
   INFERRED from its shape. The field agrees (§0.27): a package without IgnoreCombat does nothing in combat.
3. **"Ignoring combat" has TWO halves, and the engine sets them together.**
   - `AIProcess::UpdateIgnoringCombat` (`0x6D7E80`, id 39181; called from `0x485B2D` and `0x6EA9A3`) copies the
     running package's IgnoreCombat flag (package flags bit 20, `0x00100000`, the bit MFO's old UseMagic package and
     Mercer's override carry) into the PROCESS bit `+0x139`.
   - It also pauses the CONTROLLER with `0x55A870`: controller `+0x42 = 1`, then it stops the behaviour controller
     (`0x858C20`). It resumes with `0x55A890`.
   - The controller constructor copies the process bit into `+0x42` at birth (`0x5582F8`).
   - With both set, the package moves the actor and the whole combat tree is paused, the cast leaves included. That
     is why the old UseMagic package rooted the follower and why an IgnoreCombat package drops every combat facet.
4. **Upper-body casting while the legs walk is normal.** Vanilla combat casters cast while strafing (marth's own
   observation, §0.27). The cast is driven by the actor's magic caster on the animation graph, not by locomotion.
   INFERRED for package-driven walking; probe P6 shows it.

**The composition: split the two halves of "ignoring combat".**

| Half | State during the substrate | Effect |
|---|---|---|
| Process `ignoringCombat` (`+0x139`) | **ON** | the package gate (`0x6D8950`) lets the follower's own package run, so the follow / travel / sandbox procedure moves the body. Composition: the package is never replaced, and its source keeps its slot |
| Controller paused (`+0x42`) | **OFF** | the combat tree keeps running, so the Action half reaches the cast leaves and the claim fires animated |
| Combat Movement half | **DENIED** | every leaf of the Movement half is force-failed by the ActionGate act()/pop() pair (section 3.5's leaf allow-list already denies them). The tree issues no movement request, so the package is the only driver. Includes Acquire Movement Resource and Dodge Threat, so combat never takes the movement controller from the package |

**The order matters (PROVEN by the constructor copy at `0x5582F8`):**

1. Call `StartCombat(anchor)` with the process bit OFF, so the controller is born unpaused.
2. Set the process bit, and do not call `0x55A870`.
3. Call `EvaluatePackage(false, false)` once, so a package StartCombat ended (`0x70F4C0`, INFERRED) is re-picked.
   `resetAI` stays false (MFO's rule: a reset clears the combat group).

`UpdateIgnoringCombat` re-syncs the process bit from the package flag whenever the package is re-evaluated. Its two
callers are call sites, which INVARIANTS #17 forbids patching. So Harbinger re-asserts the bit once per frame from
its game-thread pump for the substrate's bounded life (1-3 s, hard TTL 15 s), and counts every reset it has to undo
(`[substrate] ... ignoring-bit reset by the engine (n)`).

This is a sustained write. Harbinger's rules forbid sustaining a decision outside a declared, bounded facet. The
substrate's INVARIANTS amendment must name it.

**Other readers of the process bit to clear in P3 (INFERRED, not yet traced):** `IsIgnoringCombat` (`0x6B6E40`,
id 38564) is also read at `0x2EB421`, `0x3318E4`, `0x3398E3`, `0x662294`, `0x67D151`, `0x71C0DD` and `0x8410C5`
(CombatManager id 46941). Some may be detection or alarm. P3 logs them.

**A side effect that may close the bark hole (INFERRED):** inside StartCombat, the block at `0x6B6CFB-0x6B6D32`
(the vfunc 0x4A result into `0x3A0D20` / id 24131, then `0x6EBCF0`) is skipped when the actor is ignoring combat.
Setting the process bit BEFORE the entry would skip it, but it would also be copied into `+0x42` and pause the newborn
controller. The fix would be to call the engine's own resume (`0x55A890`) right after. That variant is research item
RS2. The design ships the simple order above, with the bark as a known hole (marth approved).

**Fallback if the split fails in P6:** a Harbinger-internal ch.24 bound centred on the package's goal (for a follow
package, the player) at the follow distance. This is the engine's own package-to-combat movement bridge
(`BGSProcedureHoldPosition` adds a `CombatAreaHoldPosition`, AE `0x55B010`). It walks the follower after the player in
combat gait. It is combat movement aimed at the package's goal, so it is second best against marth's ruling, and it is
used only if P6 shows the package cannot drive while a controller exists.

### 3.5 Deny every facet the substrate switches on

Deny-completeness is the licence (principle 2). The substrate is legal only when every row below has a complete
deny or a recorded hole that marth accepted.

**Harbinger owns the deny set, not the client.** The client cannot be trusted to claim every facet, and a substrate
opened with one facet unclaimed is the exact hole the principle forbids. So the substrate facet installs its denies
itself, the same way the engagement hold installs its internal ch.24 bound.

| Facet combat switches on | Existing Harbinger deny | Substrate deny | Hole |
|---|---|---|---|
| Attack leaves (melee, ranged, bash, special, ground, flying) | ch.7 `Offense` (also denies the cast leaves, so it cannot be used here) | **a substrate leaf ALLOW-list** in `core/ActionGate.cpp`. While a substrate is live, every combat behaviour-tree leaf is force-failed (the existing act()/pop() pair) **except** `CastImmediateSpell`, `CastConcentrationSpell` and `PrepareDualCast`. Allow, not block: a leaf nobody listed is denied by construction | none for leaves, if the catalog is complete (a 70-leaf catalog, matched 1:1 on this build) |
| Shouts | ch.14 is arbitration only; ch.7 `Cast` includes CastShout | covered by the allow-list | none |
| **Combat movement** (every Movement-half leaf: close, flank, chase, strafe, orbit, dodge, cover, flee, hide, return to area, search, acquire weapon / movement resource) | ch.23 leash, ch.24 bound | **denied by the allow-list; the body belongs to the follower's own package** through the ignoring-combat split (section 3.4) | P6 must show the package moving the body. The engine may reset the process bit (re-asserted and counted) |
| Flee | none needed | an anchor is no threat; the flee leaves are off the allow-list | none |
| Search, target lost | the engagement hold's knowledge seats (`[EngagementHold]`, group-keyed on `CombatGroup::UpdateTargetDetection`; listener AE `0x7AF410 -> 0x7AF830`, group pass in `CombatGroup::Update` AE `0x803710`) | register the anchor as a held target, so it is never "lost" and the group never searches | none |
| Weapon equip (StartCombat's re-arm, the combat selector) | ch.17 (ARMO/WEAP/AMMO/LIGH), HandBlock | the hand block closes the claimed hand. The other hand gets a full deny-only floor **without** `kCastFlag_FloorSpellsOnly`, so no weapon, shield or torch enters it | the re-arm equip at `0x6B6BED` runs inside the entry call. Mitigation: publish the floor and the claim BEFORE the entry. P3 checks it |
| Other spells | ch.8 allow-list, 0x0F, 0x0A CheckCast | unchanged; only the gambit spell is admitted (marth's ruling) | none |
| Weapon draw | ch.4 draws; nothing denies a draw | casting raises the hands, which IS the animation; the hands hold only the spell | afterwards the follower sheathes on the engine's own delay |
| Hostility of others toward the follower | none | the anchor is in no faction and never attacks (INFERRED) | P3 counts it |
| Allies and teammates joining | ch.22 denies StartCombat per actor in a window | **a new deny on StartCombat's TARGET check**: the slot 0x99 hook ch.22 already owns answers "dead" at the target return address (`0x6B6A0A` AE / `0x625288` SE) when the target is an anchor and `this` is not the substrate's owner | a group join (third argument, `0x83D2C0`) is not caught there; commanded actors (summons) may join this way. The substrate ends at fire, before a summon arrives (about 1 s). P3 |
| The aggressor mark (`AIProcess+0x136` bit 2) | none | the anchor owns no crime faction and is never hit. Clear the bit at close if P3 shows it outlives the substrate | INFERRED |
| Crime, bounty | none | no hit lands; the anchor has no owner faction | none known |
| Combat music | none | driven by actors in combat with the PLAYER (INFERRED). Nobody targets the player | P3 |
| The player's combat state, saving | none | the player is in no combat group | none known |
| Dialogue barks (combat-start topic, attack barks) | ch.10 only pauses dialogue | attack barks: no attack leaf runs. Combat-start topic: likely `0x3A0D20` (id 24131) at `0x6B6D24` (INFERRED) | **KNOWN HOLE, approved by marth 2026-10-12.** Logged per substrate: `[substrate] ... known hole: combat-start topic path ran`. RS2 runs in parallel (section 3.4's ordering variant is its first candidate) |
| Papyrus `OnCombatStateChanged` and `TESCombatEvent` | none | none: the event is the engine's, and hiding it would hide state from other mods' scripts | **KNOWN HOLE, approved by marth 2026-10-12.** Logged once per substrate: `[substrate] ... known hole: combat state event sent` |
| Combat regen rates | none | none | accepted: 1-3 s, negligible |
| MFO's own combat systems (party combat gate, Evaluator foe pick, FoeCount, engage on sight, combat table, combat-end edges) | none | **MFO change (slice S3)**: a Harbinger query `GetSubstrateState(actor)`. MFO treats a substrate-only controller as OUT of combat and never lists an anchor as a foe | none once S3 lands |
| Harbinger's own combat features (engagement hold watchdog, waiting ch.24 claims, R-1) | | the substrate is not an engagement. The hold ignores substrate controllers; a waiting client ch.24 claim does not apply under a substrate | none once S2 lands |
| A real foe arrives during the substrate | ch.22 only inside its window | StartCombat(foe) on the follower takes the "already in combat" path and adds the foe. The substrate sees the second target (its StartCombat observer at the ch.22 seat) and ENDS AT ONCE: the anchor goes home, the process bit is cleared, the leaf allow-list and floors drop, and the fight is the engine's own. The cast claim stays, now as an ordinary in-combat claim | none, but it is the transition to test first in the field |

### 3.6 Lifetime and the clean end

**Open** (the client asks; Harbinger runs it on the game thread):

1. Validate: the follower is loaded, alive, not in combat, not in a scene or dialogue, and high process.
2. Take an anchor from the pool and move it to the follower.
3. Publish the leaf allow-list and the floors, and mark the anchor held.
4. Call `StartCombat(anchor, nullptr)` once, through ch.21's verified call. A false return ends the substrate and
   logs the reason.
5. Set the process ignoring-combat bit and call `EvaluatePackage(false, false)` (section 3.4).

**Serve:**
- The client's ch.8b claim casts. The engine's own selector equips the spell (section 4).
- The package moves the body.
- The anchor follows the follower.
- The process bit is re-asserted every frame.

**Close** triggers:
- the claim's first observed FIRE (fire-and-forget);
- the channel end (concentration: the claim's stop percent or its release);
- the client's release;
- a real foe;
- TTL (default 6 s, clamped to 15 s);
- the follower's death, unload, load or revert.

Close runs in one game-thread pass:

1. Clear the process bit.
2. Call `Actor::StopCombat` once (slot 0xE5, AE `0x6B70A0` / SE `0x625920`). It frees the controller inline and
   nulls `+0x160` and the current combat target (§0.47).
3. Release every internal effect: the hold slot, the leaf allow-list, the floors.
4. Move the anchor home.
5. Call `EvaluatePackage(false, false)`.

Prior behaviour returns because nothing else was written; the package was never replaced. This is the shape
`DropNotEngaged` uses (`channels/CombatEntry.cpp:1034`). That function is engagement-specific, so its release steps
are factored into one shared `EndCombatRestore` helper rather than calling it.

**Load and revert:** every pool anchor is moved home at `kPostLoadGame`, and its held state is dropped. No co-save.
The anchors are ESL-authored references, so the `.ess` only ever carries their position.

**Threading:**
- Open, serve and close run on the game thread (Harbinger's MainThread pump, `Arbiter::OncePerFrame`).
- StopCombat frees the controller on the calling thread with no lock (§0.47), so no Harbinger reader may hold the
  controller across that pass.
- Every substrate reader reads a FormID-keyed state record, never the controller.

### 3.7 Rejected alternatives

- **The UseMagic package** (`Packages::CastAt`, OOC hostile today). Animated (PROVEN §0.21) and needs no combat, but
  it SUBSTITUTES the follower's package (the source loses its slot and fires OnPackageEnd), roots the follower, and
  declines for package-locked custom followers. Composition, not substitution, rules it out, and marth chose the
  substrate.
- **Driving the caster state machine by hand** (`ComposedCast::DriveObservedCast`). Retired 2026-09-06; it never
  fired in seven cycles.
- **Forging a controller without StartCombat.** Skips the engine's entry and ch.22's seat.

## 4. The spell equip moves to Harbinger

Today every claim road first calls `Loadout::Prepare`, which `EquipSpell`s the spell into the LEFT hand directly
(the "equip half" in `BuffClaim`, `ComposedCast::Try`, the owned offense branch).

**The engine can equip it itself (PROVEN path, not yet observed running):** `CombatInventory::Init` (AE `0x80F010`)
calls `Actor::VisitSpells` (AE `0x6C4AA0`, id 38781, CommonLib-named) at `0x80F209` and passes each known spell to
`0x811CC0` (id 44882), so every spell the follower knows is a combat-inventory candidate. The selector then equips
the winner of the behaviour equip slot. With the claim standing, `CheckShouldEquip` (0x0F) admits only the claimed
spell on the claimed hand, so the winner is the claimed spell.

**What ch.17 and ch.8b already provide.** ch.17 governs ARMO/WEAP/AMMO/LIGH only (FACETS ch.17: "the form is not
ARMO, WEAP, AMMO or LIGH: allow, not governed"). Spells are not its business. ch.8b's 0x0F seat is the spell equip
arbiter already. So "the equip moves to Harbinger" needs **no new facet**: MFO stops calling `EquipSpell` on a claim
road and lets the engine equip under the claim. This works in combat today and out of combat under the substrate,
because both have a controller.

**What must be proven first:** probe P2. If the engine does not take the claimed spell within the claim's window,
the fallback is a Harbinger-side bounded equip of the claimed spell on the claimed hand (a ch.8b option bit, ABI
append, using the same `#17a`-licensed equip shape ch.17 uses for declared items), never MFO's direct call.


## 5. The probe ladder (principle 5: observe each path running before building on it)

| Probe | Code? | Where | INI | Exact log line(s) | Proves |
|---|---|---|---|---|---|
| **P1** a claimed buff / summon / ward fires | none (field setup only) | MFO + APMF as shipped | `[Probe] bCasterTypeCensus=1` (default) | MFO `[buff] ... CLAIM at ...`; APMF `[ctcensus] ... BUILT type=<Row>` and `... VERDICT: FIRED via <Row> (the hand charged: ANIMATED` | R3/R4 run in the field |
| **P2** the engine equips a claimed spell itself | passive, APMF | `core/EquipGate.cpp` 0x0F seat | `[Probe] bClaimEquipProbe=1` (new, default 0) | `[equip-probe] 0x<actor> 0x0F ASKED form=0x<spell> hand=<L/R> driven=yes in-hand-before=<yes/no>` once per claim window, and at the window close `[equip-probe] 0x<actor> CLOSE claimed hand took driven form via <engine/client Prepare> at +<ms>` | whether the engine equips the claimed spell without `Loadout::Prepare` |
| **P3** substrate dry run (a controller on an anchor, no cast) | yes, behind a default-off switch: the S2 build itself | APMF S2 | `[CombatSubstrate] bCombatSubstrate=1`, `bSubstrateDryRun=1` | `[substrate] 0x<f> OPEN anchor=0x<ref> entry=<true/false>`; `[substrate] 0x<f> LEAF DENIED <leaf> (n)`; `[substrate] 0x<f> STARTCOMBAT-AT-ANCHOR REFUSED caller=0x<actor>`; `[substrate] 0x<f> OBSERVED music=<type> player-in-combat=<y/n> aggressor-bit=<0/1> ignoring-readers=<site:n,...>`; `[substrate] 0x<f> known hole: combat state event sent` / `... combat-start topic path ran`; `[substrate] 0x<f> CLOSE reason=<...> controller-null=<y/n> anchor-home=<y/n> pkg-after=0x<pkg>` | every row of the deny table, with numbers, before a cast rides on it |
| **P6** the package drives the body under the substrate | P3's build, same switch | APMF S2 | `[CombatSubstrate] bSubstrateMoveTrace=1` (new, default 0) | once per 250 ms while open: `[substrate] 0x<f> MOVE pkg=0x<pkg> proc=<type> ctrl+0x42=<0/1> proc+0x139=<0/1> speed=<u/s> dist-to-pkg-goal=<u> movement-leaves-denied=<n> ignoring-bit reset by the engine (n)`; with a claim (P4): `[castobs] ... ANIM-EVENT 'BeginCast<L/R>'` and `'M<L/R>h_SpellFire_Event'` at a sample with `speed > 50` | the split works: the package moves the follower (speed > 0, distance to the player holding), the tree is not paused (`ctrl+0x42=0`), and an animated cast fires while walking |
| **P3a** console rehearsal (before S2 lands) | none | in-game console | none | what marth sees and hears | barks, music and teammates joining for combat against a non-hostile target. Steps: `placeatme` any harmless NPC; select it, then `tai`, `setghost 1`, `setav aggression 0`; select a follower, `startcombat <ref>`; watch 30 s; `stopcombat`. A cheap early look, not a substitute for P3 |
| **P4** substrate + claim fires | S3 | MFO + APMF | as P3, `bSubstrateDryRun=0` | APMF `[ctcensus] ... controller=yes ... VERDICT: FIRED via <Row> (the hand charged: ANIMATED`; MFO `[cast] ... road=substrate-claim ... FIRED`; then `[substrate] ... CLOSE reason=fired` | an animated OOC cast |
| **P5** the transition to a real fight | S3 | field | as P4 | `[substrate] ... CLOSE reason=real-foe target=0x<foe>`, then the claim continuing as an in-combat claim, and no anchor in MFO's foe pick | a substrate meeting a real fight ends cleanly |

Every new probe line is rate-limited and passive. A failure is logged loudly and never masked (principle 7). A
substrate that cannot open does not fall back to the direct road while Harbinger reports the substrate capable.

## 6. The build plan

| Slice | Tier | Repo | Author | Depends on | What it proves in the field | Due 10-16? |
|---|---|---|---|---|---|---|
| **S0** P1 field run, plus the P3a console rehearsal | none | (field) | marth plays, the coordinator gathers | nothing | the in-combat claim roads R3/R4 fire; the world facets of combat against a non-hostile target | **yes** |
| **S1** convert the remaining in-combat direct roads (details below) | B | MFO | Sonnet | S0 for R5 confidence (not a hard block) | every in-combat cast except R6 is animated | **yes** |
| **S2** the Harbinger substrate facet (details below) | **A** | APMF | Opus | marth's adoption of the amendment (anchor actor + process-bit split) | P3 and P6: the deny table and the movement split, with numbers | **no** (tier A; three review rounds is the norm) |
| **S3** the MFO substrate client (details below) | **A** (the Scheduler combat gate and its threading) | MFO | Opus | S2 | P4, P5 | no |
| **S4** the spell equip moves: the P2 probe first (APMF, passive); then MFO stops `Loadout::Prepare`'s `EquipSpell` on claim roads when Harbinger reports engine-equip capability; the bounded Harbinger equip bit only if P2 says the engine does not equip | P2: B; MFO change: B; a Harbinger bit: A | APMF + MFO | Sonnet (B), Opus (A) | P2 field data | the claimed hand takes the driven form with no MFO equip | P2 probe only |
| **S5** concentration and AUTO under the substrate: a heal channel held for its stop percent while walking; the AUTO heal / buff series out of combat | B | MFO | Sonnet | S3 | OOC channelled heals are animated | no |
| **S6** retire the OOC direct roads with Harbinger present (they stay the APMF-absent degrade); update CAST-DELIVERY, MAP, ENGINE_NOTES, STATUS | B (code) / C (docs) | MFO | Sonnet | S3-S5 field-proven | nothing new: the docs match the code | no |
| **RS1** research: an animated road for aimed Light and runes (kTargetLocation), and `rowApprox` spells | research | APMF | Opus | none | which caster, if any, the engine builds for them | no |
| **RS2** research, in parallel (marth approved shipping without it): a deny for the combat-start topic (first candidate: section 3.4's ordering variant); the meaning of `AIProcess+0x136` bit 2; the seven other readers of the process ignoring-combat bit | research | APMF | Opus | P3 numbers | whether the bark hole can be closed | no |

**S1 detail (MFO, tier B):**
- R2: a self heal whose delivery is not Self is claimed with the caster's own FormID as the target.
- R5: the summon claim is the only road with Harbinger present; the direct road is the APMF-absent degrade.
- R9: drop the `!IsRestorationSpell` exclusion on the claim forks.
- **R10: AUTO hostile becomes a series through `CastOn`, one foe at a time (marth approved).**
- Every road that is still direct logs one `[road] <id> <spell> UNANIMATED (<reason>)` WARN per fight, so the
  residue is visible.

**S2 detail (Harbinger, tier A):**
- A new intent, for example `kIntent_CombatSubstrate`, appended to the ABI.
- The anchor POOL records in `APMF.esl`, and the open / serve / close / load-home lifecycle.
- The entry through ch.21's verified StartCombat call.
- The anchor held by the knowledge seats, and the anchor following the follower.
- The ActionGate leaf allow-list, Movement half included.
- **The ignoring-combat split:** the process bit on after the entry and re-asserted per frame, the controller
  unpaused, and the `EvaluatePackage` re-pick.
- The anchor-target refusal at StartCombat's target `IsDead(false)` return address: a new `VerifiedAddresses`
  call-site row on both runtimes, byte-checked.
- The dry-run switch, and the P3 / P6 probe lines.
- The INVARIANTS #0 amendment and a FACETS chapter.

**S3 detail (MFO, tier A):**
- The OOC table asks for a substrate and a ch.8b claim for every OOC cast (R12-R14, R16).
- MFO's combat readers treat a substrate-only controller as out of combat and never see an anchor as a foe: the
  Scheduler party-combat gate, the Evaluator foe pick, `FoeCount`, engage on sight and the combat-end edges.
- R15 becomes a ch.21 entry plus the R7 claim.

**The first buildable slice is S1** (tier B, Sonnet, MFO only, no Harbinger change), together with S0 in the field.
S0, S1 and the P2 probe fit the 2026-10-16 date.

**S2 and S3 cannot meet 2026-10-16.** S2 is a new Harbinger facet with an engine call, a new verified call-site row,
a sustained engine-state write and an INVARIANTS amendment. That is tier A, and it runs until its review finds
nothing above SEV-3. S3 depends on it and touches the Scheduler's combat gate.

## 7. Decisions (marth, 2026-10-12) and what is still open

**Answered:**
- **Q1, the anchor.** marth asked whether it can be an inanimate object. The disassembly says no (section 3.3), so
  the anchor is an actor, now a pooled, ESL-authored one. **Still needs his yes:** S2's amendment, which lets
  Harbinger move its own anchor actors, start combat against them, and hold the process ignoring-combat bit for the
  substrate's bounded life.
- **Q2, movement.** marth: "block combat from affecting movement and have them continue using the non combat package
  for movement". The design is section 3.4: split ignoring combat, deny the Movement half, and let the package drive.
  Proven by P6 before anything rides on it. The ch.24 fallback is used only if P6 fails.
- **Q3, AUTO on attack spells.** One foe at a time, animated. Approved; in S1.
- **Q4, the two holes.** The combat-start bark and `OnCombatStateChanged` ship as known holes, logged per substrate.
  Approved; RS2 runs in parallel.

**Open:** none beyond the S2 amendment above.
