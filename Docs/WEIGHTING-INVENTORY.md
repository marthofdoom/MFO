# WEIGHTING INVENTORY (Step 0 of the weighting rework)

ClickUp 86e3jwnm5 (Step 0), parent 86e3jwmk7 (the rework), feeds 86e3940z6 (MFO fully on Harbinger).
Written 2026-10-12 against `origin/main` 2576701 (after MFO moved its gate hooks to Harbinger client mode, ch.25 CastGuard). Read-only pass over the MFO tree. Line numbers are as of that SHA and drift: **MAP.md's line numbers in the hook sections are already stale** (for example `CasterConsent.cpp` `thunk` is at `:552` now, MAP says `:495`).

## What this is, and what it is not

The rule (marth 2026-10-01, made absolute 2026-10-05): if the engine decides a choice by weighting or scoring, Harbinger enforces a client's wish ONLY by biasing that weighting. Deny+substitute is allowed solely where the engine does not weight. Another mod's direct force always wins over weighting ("they force they win"). Outside mods are low-priority inputs per facet. Force is an opt-in argument, discouraged.

This file lists every MFO feature that touches the engine on an NPC, classifies how it is enforced today, and says whether the engine decides that choice by weighting. It is a classification, not a design. **The weighted/unweighted calls are the ones that need an Opus check against the disassembly** (the ClickUp task asks for that). Each row's evidence cell says what the call rests on:

- `PASS S` = disassembled and field-observed (memory `equip-score-steering-premise`, ENGINE_NOTES).
- `CH-MAP` = stated in Harbinger `Docs/CHANNEL-MAP.md` (APMF repo, read only), not independently re-verified here.
- `unknown` = no disassembly note exists in either repo. These are the rows the Opus pass must settle before any build.

## Column key

- **Mechanism today** is one of `deny+substitute` (Harbinger or MFO denies the engine's source and supplies another), `force` (a direct engine call, write or package fill that makes the NPC do the thing), `weighting` (MFO already feeds an input the engine's own scorer reads), `observe` (reads only, or a passive log). A row can carry a second, bracketed mechanism.
- **Channel** is the Harbinger (APMF) channel number MFO uses, or `-` for none.
- **Captures outside writes?** is whether the hook point MFO relies on can take another mod's write as an INPUT to arbitration today. **Answer for every row: no.** No Harbinger facet captures outside writes yet (ClickUp 86e3jwmk7 triage comment: "No weighting facet exists in APMF origin/main yet"). Where the Phase 1a/1b plan (`_research/harbinger-controller-plan-2026-10-11.md`) names an existing Harbinger seat as the capture point the cell says `no (R 1a)` or `no (R 1b)` (R = reuse an existing seat, 1a/1b = plan phase). `no (P 1b)` means state-drift poll only, no seat. `no (no seat)` means the plan has no row for it. `n/a` is a read or an MFO-internal step. MFO's own vtable hooks (Targeting, CasterConsent, CombatStyle) run with whatever install order gives them: they call the original and then act, with no source attribution.
- **Target** is one of `weighting` (bias the engine scorer through a new Harbinger facet), `keep deny` (unweighted choice, the rule allows deny), `keep force` (no engine decision exists to bias, for example out of combat), `n/a` (not a behaviour decision), or a mix.

## SUMMARY

### Row counts (the tables below; one row per feature, 60 rows)

| Mechanism today (primary) | Rows |
|---|---|
| deny+substitute | 17 |
| force | 34 |
| weighting | 0 (none: MFO feeds no engine scorer today; T2 is the closest, an input write) |
| observe | 9 |
| **total** | **60** |

19 rows carry a second mechanism in brackets (a claim route over a force, or the reverse). Engine decides by weighting: **yes 7** (E1, E2, C1, C3, C4, T1, T2), no 49, unknown/partly 4. Captures outside writes today: **0 of 60** (no Harbinger weighting facet exists; the plan names an existing seat for most rows, see the column key).

### Weighted by the engine (rule says: must move to weighting)

| Facet | Evidence | Status of the move |
|---|---|---|
| Weapon pick within a category (melee vs ranged, which sword, torch vs shield), IN COMBAT only | PASS S: per-class `CalculateScore` vfunc 0x0C (Melee AE 0x8183e0, Ranged 0x8188b0, Shield 0x818df0, Torch 0x819480, OHB 0x819250), score at `+0x18`, array sorted ascending and popped from the end; pipeline reachable only from `CombatController` update | seats exist in Harbinger (`AiCastSeats` GROUP C probe observed running, field 2026-09-06); `kScoreSteerBias` 100000 was found far too large (real scores 0.08 to 29) and the steer shipped default OFF |
| Spell choice inside a caster class | PASS S + RE 2026-09-05: spells are scored (e.g. Raise Zombie 120000, Healing 472, Firebolt about 360), selector walks sorted scores | NOT shown: whether score selects ACROSS categories (that field run was confounded by MFO's own claim) |
| Combat target | CH-MAP ch.20: `CombatController::UpdateTarget` (AE `0x559930` / SE `0x4FE300`) re-chooses by threat once per `Actor::UpdateCombat`, then `SetTarget` (`0x559630` / `0x4FDFE0`) | the threat-score function itself is not disassembled in any note; unknown whether it is biasable |
| Armor wearing (inventory add, outfit re-apply) | memory `armor-wear-engine-is-the-wearer`: engine auto-equip is rating-based and is "the actual wearer" | no address recorded; the outfit re-apply path is separately believed UNWEIGHTED (ClickUp 86e3jwmk7 open question b) |

### NOT weighted (the rule allows deny or force here)

- Weapon vs magic ADMISSION: `CheckShouldEquip` vfunc 0x0F is a boolean gate; across categories the order is the table `[1,2,4,0,3,5,0,6]` plus slot occupancy and is score-blind (PASS S). Weapons (cat 0) can never out-score a spell that already holds the hand.
- Package selection (alias, quest, scene, travel): a precedence stack, not a weighting (controller plan). The `0x49` gate and alias fills stay.
- Cast aim (`GetMagicTarget` vfunc 0x0A) and cast timing: input substitution, not a score.
- Flee/retreat, combat entry, pursuit and approach: thresholds on AVs and the behaviour tree's selector order (CH-MAP ch.7, 21-24). None found to be a weighted sum.
- Movement block, idle, weapon draw/sheathe, stance toggle: one-shot or flag state.

### Which features should move to weighting first

Ranked by (field pain) x (evidence strength) x (a Harbinger seat already exists). Rows are referenced by id.

1. **E1** in-combat weapon/off-hand pick (cast/Equip.cpp force-hold). Strongest evidence of any row, probe-observed, seats exist, and it is the exact bug class that produced the dagger-displaces-proxy and unarmed-follower field bugs. Needs bias re-sized to the observed 0.08 to 29 range. Out of combat it stays force (no combat controller, nothing to score).
2. **E3** armor/gear authority (the ch.17 declaration road, 3 MCM/INI switches, B134 refused-claim mode, 7 open backlog items superseded by this task). It is the single biggest block of deny+substitute code MFO owns. BLOCKED on one fact: whether the engine's armor auto-equip is a scored comparison at all, or an unweighted outfit re-apply. Settle with the 86e3jwmk7 (a)/(b)/(c) probes first.
3. **C1** spell choice (CasterConsent `CheckStartCast` 0x06 advisory + `CheckCast` 0x0A hard deny + the ch.8 allow-list). Today this is pure deny of the unwanted. If score selects within a caster class, the whole deny layer becomes a bias. BLOCKED on the "free the AI" experiment (cast gambits off, observe what it picks).
4. **T1** combat target (Targeting 0xE4 write, ch.20 pin, ch.6). This is the facet LoreRim fights over (SmartNPCTargetSelector, Aggro Management). Biasing the threat score would make the pin a preference instead of an overwrite that other mods can undo. BLOCKED on locating the threat scorer in `UpdateTarget`.
5. **T2** combat style ownership (CombatStyle `ApplyTick` writes `combatStyle` on the live controller every tick). The style multipliers ARE inputs to the engine's category scoring, so this is already weighting-adjacent. Smallest conversion cost: it is an input write, not a deny, and branch C re-asserts every tick for no reason except the engine re-deriving it.

Quick wins not on the weighting path, listed because the inventory found them: **K3** (`ClaimCombatActionDeny`, ch.7) has no MFO caller and can be deleted or marked dormant; **K1/K2** are the two MFO hooks on slots Harbinger also owns (0x0A and 0x0F). Since the client-mode move K2 installs only when Harbinger is absent and K1 only against a Harbinger older than ABI 21, so both are legacy and can go once ABI 21 is the floor. The one MFO hook still installed beside Harbinger is **T3** (0xE4 UpdateCombat, carrying T2), the live double-hook on a Harbinger seat.

## Key to ids and sources

Rows are grouped: **E** equip, **C** cast, **T** target and combat, **P** package/movement/idle, **L** logistics item moves, **K** hooks and infrastructure, **S** stats/progression, **O** observe-only reads. 

## E: Equip (9)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| E1 | In-combat weapon / off-hand hold (forced weapon, shield, left-hand rule) | cast/Equip.cpp:1076 ReconcileForcedWeapon, :285/:745/:767 EquipObject, :872 Unequip, :824 ForcedHoldFor; CombatStyle.cpp:210 ClaimEquipment; weapon choice | force [deny+substitute: ch.15 gate-only claim] | yes in combat (PASS S: per-class CalculateScore 0x0C, array sorted, popped from end); no out of combat (no CombatController) | 15 (+8a) | no (R 1a) | weighting in combat, keep force out of combat | high: dagger-displaces-proxy and unarmed-follower bugs came from here; co-save FWPN |
| E2 | Gambit shield / torch equip | cast/Equip.cpp:216 EquipShieldOnMain; logistics/Upkeep.cpp:177 EquipTorch, :212 EquipObject; shield/torch slot | force | yes in combat (Shield 0x818df0, Torch 0x819480 score classes, PASS S); out of combat no | none | no (R 1a) | weighting in combat, force out of combat | medium |
| E3 | Armor / gear authority (declared worn set, scope, player picks) | logistics/EquipAuthority.cpp:554 RefreshEquipDeclaration, :167 EquipBestOwnedGear; apmf/Equip.cpp:111 ClaimEquipAuthority, :194 DeclareEquipScope; armor + outfit | deny+substitute | unknown: engine auto-equip is rating-based (memory armor-wear-engine-is-the-wearer, no address); the outfit re-apply is believed unweighted | 17 | no (R 1a) | weighting where scored; keep deny on the outfit re-apply | high: biggest deny block MFO owns; MFO-B16/B39 reopened, 7 B-items superseded |
| E4 | Legacy gear wear (Harbinger absent or declined) | logistics/Gear.cpp:790 EquipObject, logistics/Service.cpp:145, logistics/Upkeep.cpp:275 Equip/:276 Unequip; armor | force | unknown, same fact as E3 | none (legacy) | no (no seat) | delete once E3 is on weighting; legacy = APMF-absent degrade only | medium |
| E5 | Head-slot clear and excluded-armor strip | logistics/Gear.cpp:350 UnequipObject, :352 RemoveItem; armor head slot | force | no (MFO policy, not an engine pick) | none | no (no seat) | keep force | low |
| E6 | Off-role / excluded weapon heal and shed | logistics/Upkeep.cpp:229 HealExcludedWeapon, :332 ShedOffRoleWeapon, :556 DropObject; weapon inventory | force | no (inventory policy) | none | no (no seat) | keep force | low: DropObject vtable slot gated by SeatVerified |
| E7 | Spell / staff equip into a hand for a cast | Loadout.cpp:115 EquipObject(slot); cast/Equip.cpp:918/:922 forceEquip=false; hand slots | force | no across categories (CheckShouldEquip is a boolean, order table [1,2,4,0,3,5,0,6], PASS S); score-blind | 15 (gate), 8a | no (R 1b) | keep force (no score decides admission) | medium |
| E8 | Weapon draw / sheathe | cast/Equip.cpp:561/:776/:943 DrawWeaponMagicHands; weapon state | force | no (sticky flag state) | 4 (no seat of its own) | no (P 1b) | keep force | low |
| E9 | LOTD museum deposit: unequip then store | logistics/Lotd.cpp:1292/:1293 UnequipObject, :1297 RemoveItem; inventory | force | no | none | no (no seat) | keep force | low |

## C: Cast (16)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| C1 | Spell choice: allow-list and consent (ch.8) | apmf/SpellAllowList.cpp:259 PublishSpellAllowList via CasterConsent.cpp:1318 NoteGambits; legacy 0x06 CheckStartCast CasterConsent.cpp:1213/thunk :552; spell select | deny+substitute [observe in client mode: allow-list only] | yes inside a caster class (spells scored: Raise Zombie 120000, Healing 472, Firebolt about 360); across classes not shown (run confounded by MFO claim) | 8 / 8b | no (R 1b) | weighting inside a class; keep deny for the allow-list outside the score | high: needs the free-the-AI experiment first |
| C2 | Friendly-fire hold and Exact concentration bound | apmf/CastGuard.cpp:62 ServiceCastGuard, Scheduler.cpp:1260; legacy 0x0A CheckCast CasterConsent.cpp:1085; cast aim / channel | deny+substitute [claim: ch.25 kIntent_CastGuard in client mode] | no (cast aim and channel timing are inputs, not a score) | 25 | no (R 1b) | keep deny | medium: ABI v21 only; guard-only fallback hooks Harbinger's t2c seat (K1) |
| C3 | Owned offense cast, per hand | apmf/CastClaims.cpp ClaimOffenseCast (Bridge.cpp:256 kIntent_Cast); cast/Auto.cpp; the AI fires its own cast; spell + hand | deny+substitute [claim; AI fires its own cast] | yes inside Offensive (scored), the claim denies all but the chosen spell | 8b | no (R 1b) | weighting (bias the chosen spell's score) with force as an argument | high: deny layer is what the rework retires |
| C4 | Per-hand heal cast | apmf/CastClaims.cpp ClaimHealCast; cast/HealRoad.cpp; spell + hand | deny+substitute [claim] | yes inside Restore (rarely built: 3 vs 120 Offensive, ENGINE_NOTES 0.41), seat 0 classify forces a non-self heal | 8b | no (R 1b) | weighting; keep seat-0 classify | high |
| C5 | Animated self-buff claim | cast/BuffRoad.cpp; apmf/CastClaims.cpp; Armor/Cloak/Invis/BoundItem rows | deny+substitute [claim] | unknown for non-combat rows; yes in combat classes | 8b | no (R 1b) | weighting where the class row scores | medium |
| C6 | Direct force cast (CastSpellImmediate) | cast/Direct.cpp:564/:591, cast/Auto.cpp:125/:138, cast/CastOn.cpp:1636, logistics/Service.cpp:1892 (out of combat); cast target + hand | force | no (a direct call, no scorer) | none | no (R 1b stack scan of CastSpellImmediate callers) | keep force (canonical delivery, Docs/CAST-DELIVERY.md); fallback only when the AI does not cast | medium |
| C7 | Self cast direct (CastSelfDirect, anim chain) | cast/DirectSelf.cpp; cast/Direct.cpp:401/:415 attach-once | force | no | none | no (R 1b) | keep force | low |
| C8 | Summon cast | cast/Summon.cpp:352 CastSpellImmediate | force | no (summon scored in combat as Raise Zombie 120000 but MFO force-casts) | none | no (R 1b) | weighting in combat, force out of combat | medium |
| C9 | Forced package cast road | cast/Roads.cpp:28 ForceCast; Packages.cpp:534 SetInputs, :589 SetSelfSpell, :997 alias fill; package + alias | force [package fill] | no (package precedence, not a score) | 9 | no (R 1a) | keep force (fallback road) | medium: alias fills persist into the .ess |
| C10 | Concentration stream and restoration-at-target | cast/Roads.cpp ConcentrationCast / RestorationCastDirect; time-bounded stream | force | no | none | no (R 1b) | keep force | medium |
| C11 | Composed cast (multi-effect / silent claim watch) | ComposedCast.cpp; spell composition | force | no | none | no | keep force | low |
| C12 | Cast pose / power-attack anim kick | cast/Fire.cpp:475 NotifyAnimationGraph; animation | force | no | 12 | no (P 1b) | keep force | low |
| C13 | Held-heal approach (move toward recipient) | apmf/CombatApproach.cpp:162 ServiceHealApproach; cast/HealRoad.cpp:225; movement toward ally | deny+substitute [claim: S1 area] | no (behaviour-tree goal, not a score) | 24 | no (R 1b) | keep deny | low |
| C14 | Offense approach (move toward occluded foe) | apmf/OffenseApproach.cpp:126 ServiceOffenseApproach; Evaluator.cpp:547; movement toward foe | deny+substitute [claim] | no | 24 | no (R 1b) | keep deny | low |
| C15 | Auto tome learn | logistics/Cast.cpp:193 AddSpell, :194 RemoveItem; spell list | force | no (not a behaviour decision) | none | no (no seat) | n/a | low |
| C16 | Per-hand cast lock (MFO-internal arbiter) | cast/Hands.cpp; nothing sent to the engine | observe | no | none | n/a | n/a | low: defines who owns each hand, feeds E1/C3/C4 |

## T: Target and combat (9)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| T1 | Combat target pin / command | Targeting.cpp:229 -> apmf/Excursion.cpp:246 PinTarget (ch.20); legacy currentCombatTarget write Targeting.cpp:112-122 (retired when the pin route stands); target | deny+substitute [claim: ch.20 pin]; legacy force (compare-and-write) | yes: CombatController::UpdateTarget re-chooses by threat each Actor::UpdateCombat (CH-MAP ch.20); threat scorer not disassembled | 20 (+6) | no (R 1a: selector 0x06 seats + outermost 0xE4) | weighting if the threat score is biasable, else keep pin | high: the facet LoreRim mods fight over |
| T2 | Combat style ownership (CSTY stance on live controller) | CombatStyle.cpp:216 ApplyTick, :248/:263/:292 combatStyle write; style multipliers | force (input write) | yes (style multipliers feed the category scoring: weighting-adjacent) | none | no (no seat) | weighting (it already is an input); drop the per-tick re-assert | medium: re-asserts every tick because the engine re-derives it |
| T3 | UpdateCombat hook 0xE4 (host of T1/T2) | Targeting.cpp:200 write_vfunc, idx 0xE4 (Character::UpdateCombat); still installed in Harbinger mode for ApplyTick | observe (carrier for a force write) | n/a | 20 (Harbinger's outermost 0xE4 observer) | no (R 1a) | keep as carrier of T2 only; a third writer on Harbinger's seat | high: double-hook on a Harbinger seat |
| T4 | Combat-target claim record | apmf/Excursion.cpp:142 ClaimCombatTarget; arbitration record only | observe (record owner) | n/a | 6 | no (R 1a) | fold into T1 | low |
| T5 | Engage on sight: start combat | EngageOnSight.cpp:567 RequestCombatEntry -> apmf/CombatEntry.cpp:115; Actor::StartCombat | force [claim: ch.21] | no (combat entry is a threshold on AVs and aggression, CH-MAP 21) | 21 | no (R 1b: StartCombat self-check seat) | keep force (claim) | medium: no-loop key, field-tested |
| T6 | Retreat: StopCombat once + re-entry deny | Packages.cpp:1170 PostRetreatStopCombat, :1189 StopCombat, :2123 RetreatFill; apmf/ReentryDeny.cpp:93; Scheduler.cpp:595; combat state | force + deny | no (flee is an AV threshold, CH-MAP 7/22) | 22 (+9) | no (R 1b) | keep | high: StopCombat must run on main (v1.0.58 CTD) |
| T7 | In-combat pursuit leash | apmf/PursuitLeash.cpp:85 ServicePursuitLeash; Scheduler.cpp:687; behaviour-tree leaves | deny+substitute [claim] | no (leaf order of the behaviour tree) | 23 (+7) | no (R 1b: ActionGate) | keep deny | low |
| T8 | Peace quash: teammates fighting teammates | Rapport.cpp:373/:374 StopCombat; Scheduler.cpp:1303 | force | no | none | no (R 1b) | keep force | low |
| T9 | Confidence / fight-or-flee reads | Evaluator.cpp, EngageOnSight.cpp:236 reads kAggression; reads only | observe | n/a | 11 (reads) | n/a | n/a | low |

## P: Package, movement, idle (11)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| P1 | Loot travel (walk to item) | apmf/Excursion.cpp:435/:501 kIntent_Travel; Packages.cpp:686 SetAPMFLootTravelTarget, :1720 LootTravelFill; movement/package | deny+substitute [claim: ch.19]; legacy force (alias fill) | no (package precedence stack) | 19 (+9) | no (R 1a) | keep deny | high: Cicero churn history; alias claim arbitrates |
| P2 | Package offer (retreat / other) | apmf/Excursion.cpp:389 OfferPackage kIntent_OfferPackage; package | deny+substitute [claim: ch.9] | no | 9 | no (R 1a: 0x49 PackageGate) | keep deny | medium |
| P3 | Alias fill / release, evict marker, EvaluatePackage | Packages.cpp:833 ClearAlias, :861 EvaluatePackage(true,false), :1526 EnsureEvictMarker, :1551 ReleaseAll; logistics/Service.cpp:1279; package | force | no | 9 | no (R 1a) | keep force (precondition for alias packages) | high: ReleaseAll order, XMarker evict marker are co-save-adjacent invariants |
| P4 | Retreat package fill | Packages.cpp:2123 RetreatFill; package | force [offer: ch.9 when Harbinger] | no | 9 | no (R 1a) | keep | medium |
| P5 | Travel gait | Gait.cpp:8 Apply (packData.maxSpeed on four packages); gait | force (record write) | unknown: a package field, not a score (CH-MAP 1a) | 1a | no (P 1b) | keep | low |
| P6 | Movement block | apmf/Excursion.cpp:725 kIntent_MovementBlock; movement | deny+substitute [claim] | no | 1 | no (P 1b) | keep deny | low |
| P7 | Idle during LOTD deposit | apmf/Excursion.cpp:711 kIntent_Idle; apmf/Deposit.cpp:168 ClaimDepositIdle; idle | force [claim] | no | 12 | no (P 1b) | keep | low |
| P8 | LOTD deposit trip travel | apmf/Deposit.cpp:106 ClaimDepositTravel, :140 ReadDepositLeg; travel | deny+substitute [claim] | no | 19 | no (R 1a) | keep | low |
| P9 | Loose pickup (ActivateRef) and pickup sound seat | logistics/Service.cpp:931 ActivateRef; logistics/PickupSound.cpp:203 write_call<5>; item + audio | force | no | none | no (no seat) | keep | medium: engine seat, SeatVerified guarded |
| P10 | Chest lockpicking | logistics/Lockpick.cpp:214 SetPosition(handle), :953 RemoveItem; world object | force | no | none | no | keep | low |
| P11 | Follow / wait / sandbox state | Followers.cpp:83-106 reads kWaitingForPlayer; Papyrus + Board verbs; package | observe | n/a | none | n/a | n/a | low |

## L: Logistics item moves (5)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| L1 | Loot take / transfers | logistics/LootTake.cpp:129..514 RemoveItem(kStoreInContainer); inventory | force | no | none | no | keep force | low |
| L2 | Swap-up (sell / drop for better) | logistics/SwapUp.cpp:538/:550/:660 RemoveItem; inventory | force | no | none | no | keep force | low |
| L3 | Economy buy / sell, gem rules, LOTD transfers | logistics/Economy.cpp, GemRules.cpp:164/:292, Lotd.cpp:1885; TradeBridge | force | no | none | no | keep force | low |
| L4 | Drink potion in OOC / combat | logistics/Service.cpp:66 DrinkBest, :168; potion use | force | no (item use) | none | no | keep force | low |
| L5 | Out-of-combat spell cast (magicka spend) | logistics/Service.cpp:1892 CastSpellImmediate, :1896/:1970 RestoreActorValue kMagicka | force | no | none | no (R 1b) | keep force | low: MFO charges the cost itself |

## S: Stats and progression (3)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| S1 | Vital HMS base values | Followers.cpp:590 SetBaseActorValue; progression/Hms.cpp:508 RestoreActorValue; AVs | force | no (inputs the engine consumes) | 11 / 1a style (AV) | no (P 1b: AvLedger) | keep force | medium: co-save PRGN v8 |
| S2 | Skill points auto-scale | progression/SkillScale.cpp:98 SetBaseActorValue; AVs | force | no | none | no (P 1b) | keep force | medium |
| S3 | Perk grant / strip | progression/PerkGate.cpp:317/:452/:518 AddPerk, :292/:309 RemovePerk; perks | force | no | none | no | keep force | medium: native perk strip / restore |

## K: Hooks and infrastructure (5)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| K1 | CheckCast 0x0A hook (guard-only, Harbinger ABI 10..20) | CasterConsent.cpp:1085 write_vfunc; installed only when Harbinger lacks ch.25 or is absent | deny+substitute | no | 8 (Harbinger CastGate seat) | n/a (MFO is the outside writer on Harbinger's t2c seat) | remove once ABI 21 is the floor | high: Harbinger census conflict |
| K2 | CheckShouldEquip 0x0F hook | CombatStyle.cpp:527 write_vfunc on 30 spell/staff vtables, installed when Harbinger absent | deny+substitute | no (boolean gate) | 8 / 15 (Harbinger EquipGate seat) | no (R 1b) | remove in client mode (already Harbinger-absent only) | medium |
| K3 | ClaimCombatActionDeny | apmf/Excursion.cpp:627 ClaimCombatActionDeny kIntent_CombatAction; no MFO caller | deny+substitute | no | 7 | no (R 1b) | delete or mark dormant | low |
| K4 | Player Update hook (main-thread pump) | MainThread.cpp:115 write_vfunc on PlayerCharacter::Update | observe (infra) | n/a | none | n/a | keep | high: the only road to main |
| K5 | UI input / present hooks | Board.cpp trampolines, ImGui overlay | observe (UI, not NPC) | n/a | none | n/a | keep | not an NPC decision |

## O: Observe-only (2)

| ID | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|
| O1 | MFO line of sight (Harbinger ABI v18 first) | apmf/Los.cpp:78 LosOf; Sightline.cpp | observe | n/a | LOS (v18) | n/a | keep | low |
| O2 | Passive probes / observers | Probe.cpp:34 StartCombatOn (debug), ProgProbe.cpp, [obs] logger | observe | n/a | none | n/a | keep | low |

## Method and gaps

Rows were confirmed by grep against the tree at 2576701. Line numbers drift. Not covered because they do not change NPC behaviour: Board/Papyrus UI, co-save plumbing, config parsing. Not verified here: every address in the evidence column is quoted from memory notes, ENGINE_NOTES or Harbinger CHANNEL-MAP, none re-disassembled. The weighted/unweighted calls need the Opus disassembly check the ClickUp task asks for, with the `unknown` rows (E3, C5, T1 scorer, P5) first.
