# Combat substrate and animated casting everywhere: design

Status: DESIGN ONLY (2026-10-12/13, Opus design pass). No production code. ClickUp 86e3f1nf5 (due 2026-10-16).
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
| R10 | combat | **AUTO hostile** (the board's Auto target on an offense spell) | fan-out: one `CastSpellImmediate` per foe in one tick (`Auto.cpp:163/176` via `ApplyEffectFromTo`) | **no** | no | a SERIES through `CastOn`, one foe at a time (the shape AUTO heal and AUTO buff already use). Question Q3 |
| R11 | combat | AUTO heal, AUTO buff | series through `CastOn` (claim per recipient) | **yes** (heal PROVEN; buff by design) | no | keep |
| R12 | OOC | every heal (self, ally, player, AUTO) | `CastSelfDirect` / `CastTargetDirect` direct stream, or posted `CastSpellImmediate` (`Service.cpp:1892`) | **no** (PROVEN: 19 `[heal] ... out-of-combat heal -- INSTANT road, unanimated` lines on LoreRim 2026-10-12) | no | substrate + ch.8b claim (section 3) |
| R13 | OOC | every buff, ward, light, AUTO buff fan | direct (`CastSelfDirect`, `ApplyEffectFromTo`, posted `CastSpellImmediate`) | **no** | no | substrate + claim |
| R14 | OOC | summon | `CastSummonOnce` direct | **no** | no | substrate + claim |
| R15 | OOC | fire-and-forget hostile at a foe | `Packages::CastAt` UseMagic package (`Service.cpp:1941`), else direct (`:1966`) | package: **yes but rooting**, a substituted package; direct: **no** | no | an OOC hostile cast at a foe is a combat start. It becomes ch.21 entry + the R7 claim (a real fight, no substrate) |
| R16 | OOC | concentration at any target | `CastTargetDirect` stream | **no** | no | substrate + claim, held for the channel |
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
branch. That needs ch.7 to gain a weapon-only bit, which the substrate needs anyway (section 3.4). Use the deny.
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

Candidates, judged against the disassembly:

| Candidate | Verdict | Why |
|---|---|---|
| The follower itself | **impossible** | refused at `0x6B69B3` (SE `0x625231`). PROVEN |
| No target, a bare controller | **impossible** through StartCombat | no target and no group builds nothing (`0x6B6CBE`). PROVEN. Building one by hand (calling 46873 directly, or the constructor) skips the engine's entry and ch.22's seat. Rejected: wrong layer, and a second road into combat that ch.22 could not see |
| The heal recipient (an ally) | **rejected** | the ally becomes the follower's combat target and a member of a hostile pair. The aggressor mark (`AIProcess+0x136` bit 2) is set because the ally does not regard the follower as hostile. Other teammates and the ally's own AI may answer (INFERRED). A self buff has no recipient at all |
| The player | **rejected** | the worst case: combat music, a teammate in combat with the player, the follower's attack leaves aimed at the player |
| **A Harbinger-owned anchor actor** | **chosen** | see below |

**The answer: the combat target is an ANCHOR, and the cast target is the claim's target.**
The anchor is a Harbinger-owned dummy actor (a new record in `APMF.esl`): invisible, ghost, no collision, AI off,
no factions, no inventory, unaggressive, essential, level 1. It is placed at the follower for the cast and deleted
after. The follower's combat target is the anchor. The magic target is the ch.8b claim's own `target`, which
seat `GetMagicTarget` (0x0A on the combat casters) hands to the AI whatever the combat target is. For a heal that is
the recipient. For a self buff or a summon it is target 0 (self, or the caster's placement). For an ally buff it is
the ally.

Why the anchor works with the pipeline (INFERRED until probe P3): the selector's range gate (`0x813A2F`, vfunc 6)
measures the item against the combat target, and an anchor at the follower's feet is in range of every spell,
touch included. The behaviour tree has a target and runs. Nothing in the world is the follower's enemy.

That the seats aim the cast at the claim target while the combat target is another actor is PROVEN in the field
for heals: every claimed heal on LoreRim was cast at an ally while the controller's target was a foe.

### 3.4 Deny every facet the substrate switches on

Deny-completeness is the licence (principle 2). The substrate is legal only when every row below has a complete
deny or a recorded, accepted hole. **Harbinger owns the deny set, not the client.** The client cannot be trusted to
claim every facet, and a substrate opened with one facet unclaimed is the exact hole the principle forbids. So the
substrate facet installs its denies itself, as the engagement hold already installs its internal ch.24 bound.

| Facet combat switches on | Existing Harbinger deny | Substrate deny | Hole |
|---|---|---|---|
| Attack leaves (melee, ranged, bash, special, ground, flying) | ch.7 `Offense` (also denies the cast leaves, so unusable here) | **a substrate leaf ALLOW-list** in `core/ActionGate.cpp`: while a substrate is live, every combat behaviour-tree leaf is force-failed (the existing act()/pop() pair) **except** `CastImmediateSpell`, `CastConcentrationSpell`, `PrepareDualCast`. Allow, not block: a leaf nobody listed is denied by construction | none for leaves, if the catalog is complete (it is a 70-leaf catalog, matched 1:1 on this build) |
| Shouts | ch.14 is arbitration only; ch.7 `Cast` bit includes CastShout | covered by the allow-list (`CastShout` not on it) | none |
| Combat movement (advance, flank, chase, strafe, orbit, dodge, cover, flee, hide, return to area, search ...) | ch.23 leash, ch.24 bound | covered by the allow-list: the follower stands and casts (vanilla OOC casters stand too). Option Q2: keep following via a Harbinger-internal ch.24 bound centred on the player instead | the substrate lasts one cast (1-3 s, field claim-to-fire 0.63-2.85 s) |
| Flee | none needed | an anchor is no threat; flee leaves are also off the allow-list | none |
| Search, target lost | the engagement hold's knowledge seats (`[EngagementHold]`, group-keyed on `CombatGroup::UpdateTargetDetection`, listener AE `0x7AF410 -> 0x7AF830`, group pass in `CombatGroup::Update` AE `0x803710`) | register the anchor as a held target, so it is never "lost" and the group never searches. Search leaves are off the allow-list anyway | none |
| Weapon equip (StartCombat's re-arm, the combat selector) | ch.17 (governs ARMO/WEAP/AMMO/LIGH), HandBlock | the claim's hand is closed by the hand block; the other hand gets a full deny-only floor **without** `kCastFlag_FloorSpellsOnly` (the archer-heal floor mode), so no weapon, shield or torch enters it | the re-arm equip at `0x6B6BED` fires before any claim can be live on a fresh controller. Mitigation: the floor and the claim are published BEFORE the entry call (Harbinger orders its own drain). Probe P3 checks it |
| Other spells | ch.8 allow-list, 0x0F, 0x0A CheckCast | unchanged; only the gambit spell is admitted (marth's ruling) | none |
| Weapon draw | ch.4 draws; nothing denies a draw | casting needs the hands up (that IS the animation). With both hands holding only the spell or nothing, the draw shows the spell | after the substrate the follower sheathes on the engine's own delay |
| Hostility of others toward the follower | none | the anchor is in no faction and never attacks, so nobody has a reason (INFERRED) | probe P3 counts it |
| Allies and teammates joining the fight | ch.22 denies StartCombat per actor in a window | **a new deny on StartCombat's TARGET check**: the slot 0x99 hook ch.22 already owns answers "dead" at the target return address (`0x6B6A0A` AE / `0x625288` SE) when the target is an anchor and `this` is not the substrate's owner. Nobody else can start combat against an anchor | an actor that joins through a group (third argument, `0x83D2C0`) is not caught there. Commanded actors (summons) may join this way. Probe P3; the substrate ends at fire, before a summon arrives (about 1 s) |
| The aggressor mark (`AIProcess+0x136` bit 2) | none | the anchor owns no crime faction and is never hit, so a mark with no victim (INFERRED). Clear the bit at the end if P3 shows it outlives the fight | INFERRED, probe |
| Crime, bounty | none (ch.21 leaves them to the client) | no hit lands on anyone; the anchor has no owner faction | none known |
| Combat music | none | driven by actors in combat with the PLAYER (INFERRED). Nobody targets the player | probe P3 (`[subprobe] music`) |
| The player's combat state, saving | none | the player is in no combat group | none known |
| Dialogue barks (combat-start topic, attack barks) | ch.10 only pauses dialogue | **HOLE.** The combat-start topic is likely `0x3A0D20` (id 24131) at `0x6B6D24` (INFERRED). No deny exists. Research RS2: a seat on the combat-topic selection. Question Q4 |
| Papyrus `OnCombatStateChanged` and `TESCombatEvent` (follower frameworks, other mods' scripts) | none | **HOLE, accepted only with marth's word.** The event is the engine's; a deny would hide state from other mods' scripts. Question Q4 |
| Combat regen rates | none | **HOLE, accepted**: the combat regen multipliers apply for 1-3 s | negligible |
| MFO's own combat systems (party combat gate, Evaluator foe pick, FoeCount, engage on sight, combat table, torch combat-end edge) | none | **MFO change (slice S3)**: a Harbinger query `GetSubstrateState(actor)` (or the anchor base FormID); MFO treats a substrate-only controller as OUT of combat everywhere, and never lists the anchor as a foe | none once S3 lands |
| Harbinger's own combat features (engagement hold watchdog, ch.24 waiting claims, R-1 "another target attacking the party") | | the substrate is not an engagement: the hold ignores substrate controllers; a waiting ch.24 claim of the client does not apply under a substrate | none once S2 lands |
| A real foe arrives during the substrate | ch.22 refuses entry only inside its window | the engine's StartCombat(foe) on the follower takes the "already in combat" path and adds the foe. The substrate sees the second target (its StartCombat observer at the ch.22 seat) and ENDS AT ONCE: the anchor is deleted, the leaf allow-list and floors drop, and the fight is the engine's own from that pass. The cast claim stays, now as an ordinary in-combat claim | none, but it is the transition to test first in the field |

### 3.5 Lifetime and the clean end

- **Open** (client asks; Harbinger, game thread): validate (loaded, alive, not in combat, not in a scene or
  dialogue, high process), place the anchor at the follower, publish the leaf allow-list and the floors, mark the
  anchor held, call `StartCombat(anchor, nullptr)` once through ch.21's verified call. A false return ends the
  substrate with the reason.
- **Serve:** the client's ch.8b claim casts. The engine's own selector equips the spell (section 4).
- **Close:** on the claim's first observed FIRE (fire-and-forget), at the channel end (concentration, the claim's
  stop percent), on the client's release, on a real foe, on TTL (default 6 s, clamped 15 s), on the follower's
  death, unload, load or revert. Close does, in one game-thread pass, the shape `DropNotEngaged`
  (`channels/CombatEntry.cpp:1034`) uses: `Actor::StopCombat` once (slot 0xE5, AE `0x6B70A0` / SE `0x625920`,
  which frees the controller inline and nulls `+0x160` and the current combat target, §0.47), release every
  internal effect (the hold slot, the leaf allow-list, the floors, any internal ch.24 bound), delete the anchor
  (`Disable` + `SetDelete`), and nothing else. Prior behaviour returns because nothing else was written: the
  package was never replaced, so the follow package resumes on the engine's own evaluation. `DropNotEngaged` is
  engagement-specific, so the design factors its release steps into one shared `EndCombatRestore` helper rather
  than calling it.
- **Load and revert sweep:** an anchor placed before a save is in the `.ess`. At `kPostLoadGame` Harbinger deletes
  every reference of the anchor base in loaded cells and on cell attach (fix-forward, the furniture-latch lesson).
  No co-save.
- **Threading:** open and close run on the game thread (Harbinger's MainThread pump, `Arbiter::OncePerFrame`).
  StopCombat frees the controller on the calling thread with no lock (§0.47), so no other Harbinger reader may
  hold the controller across that pass. Every substrate reader reads a FormID-keyed state record, never the
  controller.

### 3.6 Rejected alternatives

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
| **P1** claimed buff/summon/ward fires | none (field setup only) | MFO + APMF as shipped | `[Probe] bCasterTypeCensus=1` (default) | MFO `[buff] ... CLAIM at ...`; APMF `[ctcensus] ... BUILT type=<Row>` and `... VERDICT: FIRED via <Row> (the hand charged: ANIMATED` | R3/R4 run in the field |
| **P2** the engine equips a claimed spell itself | passive, APMF | `core/EquipGate.cpp` 0x0F seat | `[Probe] bClaimEquipProbe=1` (new, default 0) | `[equip-probe] 0x<actor> 0x0F ASKED form=0x<spell> hand=<L/R> driven=yes in-hand-before=<yes/no> by=<engine selector/client>` once per claim window, plus the window close `[equip-probe] 0x<actor> CLOSE claimed hand took driven form via <engine/client Prepare> at +<ms>` | whether the engine equips the claimed spell without `Loadout::Prepare` |
| **P3** substrate dry run (combat with an anchor, no cast) | yes, behind a default-off switch: the S2 build itself | APMF S2 | `[CombatSubstrate] bCombatSubstrate=1`, `bSubstrateDryRun=1` | `[substrate] 0x<f> OPEN anchor=0x<ref> entry=<true/false>`, `[substrate] 0x<f> LEAF DENIED <leaf> (n)`, `[substrate] 0x<f> STARTCOMBAT-AT-ANCHOR REFUSED caller=0x<actor>`, `[substrate] 0x<f> OBSERVED combat-event state=<n>`, `[substrate] 0x<f> OBSERVED music=<type> player-in-combat=<y/n> aggressor-bit=<0/1> regen-mult=<f>`, `[substrate] 0x<f> CLOSE reason=<...> controller-null=<y/n> anchor-deleted=<y/n> pkg-after=0x<pkg>` | every row of the deny table, with numbers, before any cast rides on it |
| **P3a** console rehearsal of the facets (before S2 lands) | none | in-game console | none | what marth sees and hears | barks, music, teammates joining, for combat against a non-hostile target. Steps: place any harmless NPC with `placeatme`, select it and `tai` + `setghost 1` + `setav aggression 0`; select a follower and `startcombat <ref>`; watch for 30 s; `stopcombat`. Not a substitute for P3, only a cheap early look |
| **P4** substrate + claim fires | S3 | MFO + APMF | as P3, `bSubstrateDryRun=0` | APMF `[ctcensus] ... controller=yes ... VERDICT: FIRED via <Row> (the hand charged: ANIMATED`; MFO `[cast] ... road=substrate-claim ... FIRED`; then `[substrate] ... CLOSE reason=fired` | an animated OOC cast |
| **P5** the transition | S3 | field | as P4 | `[substrate] ... CLOSE reason=real-foe target=0x<foe>` then the claim continuing as an in-combat claim, no `TARGET IS ANCHOR` lines from MFO's foe pick | a substrate meeting a real fight ends cleanly |

Every new probe line is rate-limited and passive. A failure is logged loudly and never masked (principle 7): a
substrate that cannot open does not fall back to the direct road while Harbinger reports the substrate capable.

## 6. The build plan

| Slice | Tier | Repo | Author | Depends on | What it proves in the field | Due 10-16? |
|---|---|---|---|---|---|---|
| **S0** P1 field run + P3a console rehearsal | none | (field) | marth plays, coordinator gathers | nothing | the in-combat claim roads R3/R4 fire; the substrate's world facets with a non-hostile target | **yes** |
| **S1** convert the remaining in-combat direct roads: R2 (claim with own FormID), R5 (the summon claim is the only road with Harbinger present; direct = APMF-absent degrade), R9 (drop the `!IsRestorationSpell` exclusion on the claim forks), R10 (AUTO hostile becomes a series through `CastOn`); every road still direct logs one `[road] <id> <spell> UNANIMATED (<reason>)` WARN per fight so the residue is visible | B | MFO | Sonnet | S0 for R5 confidence (not a hard block) | every in-combat cast except R6 animated | **yes** |
| **S2** Harbinger substrate facet (new intent, e.g. `kIntent_CombatSubstrate`, ABI append): the anchor record in `APMF.esl`, open/close/sweep, entry through ch.21's verified StartCombat call, anchor held by the knowledge seats, the ActionGate leaf allow-list, the anchor-target refusal at StartCombat's target `IsDead(false)` return address (a new `VerifiedAddresses` call-site row, both runtimes, byte-checked), the dry-run switch, the P3 probe lines; INVARIANTS #0 amendment (needs Q1); FACETS chapter | **A** | APMF | Opus | Q1 | P3: the deny table with numbers | **no** (tier A, 3 review rounds is the norm) |
| **S3** MFO substrate client: the OOC table asks for a substrate and a ch.8b claim for every OOC cast (R12-R14, R16); MFO's combat readers (Scheduler party-combat gate, Evaluator foe pick, `FoeCount`, engage on sight, the combat-end edges) treat a substrate-only controller as out of combat and never see the anchor as a foe; R15 becomes ch.21 entry + the R7 claim | **A** (the Scheduler combat gate and its threading) | MFO | Opus | S2 | P4, P5 | no |
| **S4** spell equip moves: P2 probe first (APMF, passive), then MFO stops `Loadout::Prepare`'s `EquipSpell` on claim roads when Harbinger reports engine-equip capability; the bounded Harbinger equip bit only if P2 says the engine does not equip | P2: B (passive probe); MFO change: B; a Harbinger equip bit: A | APMF + MFO | Sonnet (B), Opus (A) | P2 field data | the claimed hand takes the driven form with no MFO equip | P2 probe only |
| **S5** concentration and AUTO under the substrate: a heal channel held for its stop percent, the AUTO series out of combat | B | MFO | Sonnet | S3 | OOC channelled heals animated | no |
| **S6** retire the OOC direct roads with Harbinger present (they stay the APMF-absent degrade); update CAST-DELIVERY, MAP, ENGINE_NOTES, STATUS | B (code) / C (docs) | MFO | Sonnet | S3-S5 field-proven | nothing new: the docs match the code | no |
| **RS1** research: an animated road for aimed Light and runes (kTargetLocation), and `rowApprox` spells | research | APMF | Opus | none | which caster, if any, the engine builds for them | no |
| **RS2** research: a deny for the combat-start topic and combat barks; the meaning of `AIProcess+0x136` bit 2 | research | APMF | Opus | P3 numbers | whether the bark hole can be closed | no |

**The first buildable slice is S1** (tier B, Sonnet, MFO only, no Harbinger change), together with S0 in the field.
S0 + S1 + the P2 probe fit the 2026-10-16 date. **S2 and S3 cannot meet 2026-10-16**: S2 is a new Harbinger facet
with an engine call, a new verified call-site row and an INVARIANTS amendment, which is tier A and runs until its
review finds nothing above SEV-3; S3 depends on it and touches the Scheduler's combat gate.

## 7. Questions for marth

- **Q1. A Harbinger-owned anchor actor.** The substrate needs Harbinger to place its own invisible dummy actor and
  start combat against it. That is a new kind of Harbinger action (it creates a reference). Adopt it as a new
  legal action (INVARIANTS #0), yes or no? Without it there is no combat target that is not a real actor.
- **Q2. Out-of-combat casts: stand and cast, or keep walking?** Recommended: stand and cast for the 1-3 s of the
  cast, as vanilla casters do. The alternative keeps the follower walking toward you in combat gait while casting,
  through an internal combat-area bound.
- **Q3. AUTO on an attack spell becomes one foe at a time.** Today it hits every foe in one tick, unanimated. The
  animated shape is a series, one real cast per foe, the same way AUTO heals already work. OK?
- **Q4. Two holes with no deny today:** the combat-start bark ("there you are" style lines) and the Papyrus
  `OnCombatStateChanged` event other mods' scripts see, both for the 1-3 s of a substrate. Ship S3 with these
  logged as known holes while RS2 looks for a deny, or hold S3 until RS2 closes the bark?
