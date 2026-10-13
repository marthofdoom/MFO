# WEIGHTING INVENTORY (Step 0 of the weighting rework, VERIFIED 2026-10-12)

ClickUp 86e3jwnm5 (Step 0), parent 86e3jwmk7 (the rework), feeds 86e3940z6 (MFO fully on Harbinger).

- **First written** 2026-10-12 against MFO `origin/main` 2576701 (60 rows, read-only Sonnet pass).
- **Verified** 2026-10-12 (this revision) against MFO `origin/main` **f3e864a** (v2.6.1 + the B15 footer fix) and Harbinger (APMF) `origin/main` **80f699b** (code identical to 7f2d638, the commits past it are docs only). Every row was re-checked by grep on f3e864a, and every line number below is as of f3e864a. Line numbers drift: grep the symbol.
- **Disassembly** for the phase 1 rows was redone on our own unpacked executables (`binaries/1.6.1170/SkyrimSE.unpacked.exe`, `binaries/1.5.97/SkyrimSE.unpacked.exe`, `binaries/1.7.104/SkyrimSE.exe`) with the `tools/verified_addresses` image and RTTI code plus capstone. No CommonLib declaration was trusted for ABI. Where a scorer is not verified, the row says "unverified" and names the probe that would settle it.
- **Field evidence** is the LoreRim deck logs of 2026-10-12 (game 1.6.1170.0, MFO 2.6.0, APMF 0.13.0, `APMF.ini` with `EnableScoreSteer=1`, `EnableWeaponScoreProbe=1`, `bEquipObserveOnly=1`, `bEquipEnforceArmor=1`, `bCasterTypeCensus=1`). Five `APMF.log` generations, 10:59 to 14:18. Called "the 10-12 logs" below.

## What this is, and what it is not

The rule (marth 2026-10-01, made absolute 2026-10-05): if the engine decides a choice by weighting or scoring, Harbinger enforces a client's wish ONLY by biasing that weighting. Deny+substitute is allowed solely where the engine does not weight. Another mod's direct force always wins over weighting ("they force they win"). Outside mods are low-priority inputs per facet. Force is an opt-in argument, discouraged.

This file lists every MFO feature that touches the engine on an NPC, classifies how it is enforced today, says whether the engine decides that choice by weighting, and (new in this revision) carries the phase 1 plan for weapons, armor and spells/casting. It is a classification and a plan, not a design spec. The Harbinger API shape in the plan is a proposal for the tier A brief.

## Column key

- **Status (10-12)**: `STILL-ACCURATE` (feature, mechanism and classification hold; line numbers corrected in place), `CHANGED` (the code or the classification changed, the cell says how), `NEW` (added since 10-11 or missed by the first pass), `GONE` (removed).
- **Mechanism today** is one of `deny+substitute` (Harbinger or MFO denies the engine's source and supplies another), `force` (a direct engine call, write or package fill that makes the NPC do the thing), `weighting` (MFO feeds an input the engine's own scorer reads), `observe` (reads only, or a passive log). A row can carry a second, bracketed mechanism.
- **Engine decides by weighting?** cites the scorer. `PASS S` = memory `equip-score-steering-premise` / ENGINE_NOTES §0.42-0.43. `RE 10-12` = re-disassembled for this revision (see "Disassembly evidence"). `CH-MAP` = Harbinger `Docs/CHANNEL-MAP.md`, not re-verified. `unverified` = no disassembly settles it, the probe is named.
- **Channel** is the Harbinger channel MFO uses, or `-`.
- **Captures outside writes? (from the census)** is what Harbinger's 1b attribution (FacetAttribution, the seat census, the equip caller recovery) does today with ANOTHER source's write on that facet. Source: Harbinger `Docs/FACETS.md` Appendix A and "Observe-only facets", and the seat code on 80f699b. Short forms:
  - `yes, ranked` = a Harbinger seat names the source and enforces only when the claim's priority is strictly above it (`facetattr::Decide`).
  - `yes, named` = the seat names the source (engine path, `External(<dll>)`, Script, Console) and enforces by its own rule, not by priority. This is the Equip facet: an identified off-set ARMOR equip is denied, weapons, shields, torches and ammo are observe-only (`bEquipObserveOnly=1`), an unattributed equip is allowed.
  - `observe` = the source is named and counted, nothing is denied.
  - `census` = only a foreign HOOK on the same vtable slot is named (seat census, every 60 s). There is no per-write input.
  - `no` = no Harbinger seat sees it. `n/a` = a read or an MFO-internal step.
- **Target** is one of `weighting` (bias the engine scorer through a Harbinger facet), `keep deny` (unweighted choice, the rule allows deny), `keep force` (no engine decision exists to bias, for example out of combat), `n/a`, or a mix.

## SUMMARY

### Verification counts (66 rows: the 60 of 10-11 plus 6 new)

| Status | Rows | Which |
|---|---|---|
| STILL-ACCURATE | 51 | all rows not listed below (line numbers corrected) |
| CHANGED | 9 | E1, E3, E7, C1, C3, C4, C5, T3, T5 |
| NEW | 6 | E10, E11, E12, C17, L6, S4 |
| GONE | 0 | |

What changed in the classification:

1. **Offensive spells share category 0 with weapons (RE 10-12, both runtimes).** `CombatInventory::AddItem` (AE `0x811AC0`) files every item into `inventoryItems[GetCategory()]` (vfunc `0x0B`, call at `0x811BA1`, index at `0x811BA7`). Melee, Ranged and every OFFENSIVE magic or staff item return 0. Restore returns 1, Ward 2 or 3, the buff and utility casters (Summon, Armor, Cloak, Invisibility, BoundItem, Stagger, Paralyze, Reanimate) 4 or 5, Disarm and Light 4, Shield and Torch 3, OneHandedBlock 6. So **weapon versus offensive spell is a real score comparison inside category 0**, not the score-blind admission PASS S described. PASS S is right for weapon versus Restore, Ward and buff spells (categories 1, 2 and 4 are walked before 0 by the order table `[1,2,4,0,3,5,0,6]`). This corrects the 10-11 bullet "Weapons (cat 0) can never out-score a spell that already holds the hand", and ENGINE_NOTES §0.42's implication that magic is never in category 0 (§0.42 is not edited here: a follow-up docs task).
2. **Real score magnitudes on LoreRim are not 0.08 to 29.** The 10-12 logs carry 4629 `WEAPON-SCORE` lines (Melee 917, Ranged 3562, Shield 94, Torch 56): min 0.22, median 147, p90 154, max 1320 (`Harbinger's Battleaxe`). Spell items scored 10.08, 504 (Healing) and 1008 (an unclaimed proxy). Harbinger's `kScoreSteerBias` is now `1000.0f` (not 100000), additive. **An additive +1000 does not dominate on LoreRim**: a 1320 battleaxe or a 1008 spell beats a steered 10.08 + 1000.
3. **The spell score steer is RUNNING in the field.** With `EnableScoreSteer=1`, the 10-12 logs show 167 `SCORE-STEER ... STEERED` lines on Auri's claimed heal proxies (`engineScore=10.080 STEERED->1010.080`, hands L and R). The steer lifts only a form that a live ch.8b claim drives on that hand. It is the existing seed of the spells facet.
4. **No engine-scored armor choice was observed.** Across the 10-12 logs, every ARMOR equip on a claimed follower came from `OutfitApply` (40 allow, 18 deny), `External(MEO.dll)` (51), `External(po3_SpellPerkItemDistributor.dll)` (20 allow, 18 deny), or APMF's own equip pass (16, plus its 16 deferred `QueuedApply`). Zero armor equips came through `CombatNode` or `AiCommand`. ENGINE_NOTES §0.46's "the engine auto-equip wears by rating" names no address and no observed path. The armor scorer is **unverified**, and may not exist.
5. **The weapon half of ch.17 fights the engine's scored pick today.** `CombatNode q=1 ... Right ... would-deny` 39 times and `QueuedApply ... Right ... would-deny` 39 times: the combat AI re-picks a weapon outside MFO's declared set. Weapons are observe-only, so the AI's pick stands, and MFO's MFO-B41 path re-declares every 3 s. That re-assert loop against a scored choice is exactly what the rule forbids and what weighting replaces.
6. **"Captures outside writes" is now answered for every row** (column filled from the 1b attribution). The score seats themselves are `census` only: Harbinger would name a foreign hook on `CalculateScore`, but it never sees a per-call foreign input. The census found no foreign hook on any score seat on LoreRim. It DID find MFO.dll hooked over Harbinger's own 0xE4 `TargetPin` seat (row T3) and re-hooked on top of it.

### Weighted by the engine (rule says: must move to weighting)

| Facet | Scorer (verified) | Runs? (principle 5) | Status of the move |
|---|---|---|---|
| Weapon pick inside category 0 (melee vs ranged, which weapon) AND weapon vs OFFENSIVE spell, in combat | `CalculateScore` vfunc `0x0C`: Melee `0x8183E0` / SE `0x77E0A0` / 1.7.104 `0x82D2C0`; Ranged `0x8188B0` / `0x77E550` / `0x82D790`; magic (all MagicT, one function) `0x819AA0` / `0x77F6F0` / `0x82E980`. Live caller `CombatInventory::Update` AE `0x80F380` (`0x80F40E call [rax+0x60]`, store to `+0x18` at `0x80F415`) | YES: 4629 `WEAPON-SCORE` lines 10-12 (Group C, census rows `AiCastSeats.GroupC.*`) | Harbinger seats exist on all three runtimes (verified rows). Weapon bias: none (the old weapon steer was deleted, F5). Spell bias: `SteerScoreForCastClaim`, live, additive +1000 |
| Shield vs torch (category 3) | Shield `0x818DF0` / `0x77EAC0` / `0x82DCD0`; Torch `0x819480` / `0x77F0E0` / `0x82E360` | YES: Shield 94, Torch 56 lines 10-12 | seat exists, no bias |
| Spell inside one caster category (which heal, which ward, which buff) | magic `0x0C` `0x819AA0` (shared) | YES: steer lines 10-12; claimed Restore casters built and FIRED (census) | steer exists for ch.8b-driven forms only |
| Combat target | CH-MAP ch.20: `UpdateTarget` AE `0x559930` / SE `0x4FE300` re-chooses by threat | runs (ch.20 restores against it in the field) | the threat-score function itself is unverified (not phase 1) |
| Armor wearing | **unverified**: no scorer found (summary item 4) | no scored path observed | blocked on probe P0c |

### NOT weighted (the rule allows deny or force here)

- **Weapon vs Restore, Ward or buff spell**: the order table `[1,2,4,0,3,5,0,6]` (AE data `0x20162C8`, count at `0x20162F0` = 8) plus the slot occupancy test (`0x8139B7 test [r15+0x130],eax`). Categories 1, 2 and 4 are walked first and take the hand. Score-blind across categories (PASS S, re-read 10-12). Lever: `CheckShouldEquip` (`0x0F`) admission, the hand block, the hand floors.
- **Shield or torch vs a second (off-hand) weapon**: category 3 is walked at order index 4, before the second category-0 pass at index 6. Score-blind. Harbinger's dual-wield preference is correctly an admission deny.
- **Torch "needs light" veto**: Torch overrides `0x0F` (`0x819760` / `0x77F350` / `0x82E640`) with a light gate. A top-scored torch can be vetoed.
- **Out of combat**: the whole pipeline is combat-only (`0x80F380` is called only from `CombatController::Update` `0x5589B0`, which runs only from the per-actor combat tick `0x6B6E70`, skipped at `0x6B6EDC` when `actor+0x160` is null; PASS S, unchanged). Every out-of-combat equip or cast stays force, or waits for the combat substrate (this week's item 3).
- Package selection, cast aim (`GetMagicTarget` 0x0A), `CheckStartCast` (0x06) yes/no, the seat-0 caster classify, flee, combat entry, pursuit, approach, movement block, idle, draw/sheathe, stance: unchanged from 10-11 (not scores).

## Disassembly evidence (RE 10-12)

All on 1.6.1170.0 unless marked. The per-class rows were re-derived from RTTI on all three executables and match Harbinger's generated `Docs/VERIFIED-ADDRESSES.md` rows exactly. `=N` is the constant the `0x0B` (GetCategory) body returns.

| Fact | 1.6.1170 | 1.5.97 | 1.7.104 | How |
|---|---|---|---|---|
| Melee item: vtable / `0x0B` / `0x0C` / `0x0F` | `0x18C9028` / `0x8183D0` (=0) / `0x8183E0` / `0x817FC0` | `0x1681A88` / `0x77E090` / `0x77E0A0` / `0x77DC90` | `0x19475C8` / `0x82D2B0` / `0x82D2C0` / `0x82CEA0` | RTTI `.?AVCombatInventoryItemMelee@@`, slot read |
| Ranged | `0x18C90D8` / =0 / `0x8188B0` / `0x817FC0` | `0x1681B58` / - / `0x77E550` / `0x77DC90` | `0x1947678` / - / `0x82D790` / `0x82CEA0` | same |
| Shield | `0x18C9188` / =3 / `0x818DF0` / `0x817FC0` | `0x1681C28` / - / `0x77EAC0` / `0x77DC90` | `0x1947728` / - / `0x82DCD0` / `0x82CEA0` | same |
| Torch | `0x18C92E8` / =3 / `0x819480` / `0x819760` | `0x1681DD0` / - / `0x77F0E0` / `0x77F350` | `0x1947888` / - / `0x82E360` / `0x82E640` | same |
| OneHandedBlock | `0x18C9238` / =6 / `0x819250` / `0x819300` | `0x1681CF8` / - / `0x77EEB0` / `0x77EF60` | `0x19477D8` / - / `0x82E130` / `0x82E1E0` | same |
| Magic Offensive / Restore `0x0C` | `0x819AA0` (both) | `0x77F6F0` | `0x82E980` | RTTI MagicT vtables `0x18D0FF0` / `0x18D0570` |
| GetCategory (`0x0B`) per magic class | Offensive =0, Restore =1, Ward `2 + (k > [this+0x38])`, Summon / Stagger / Cloak / Invisibility / BoundItem / Paralyze / Reanimate / Armor `4 + (...)`, Disarm =4, Light =4 | identical shapes | not read | every MagicT `0x0B` body disassembled, Magic and Staff variants |
| `AddItem` files by category | `0x811AC0`: `0x811BA1 call [rax+0x58]`, `0x811BA7 lea rax,[rcx+rcx*2]`, `r14+8+cat*0x18` = `inventoryItems[cat]`, sorted insert through `0x815220` | | | disassembly |
| Rescore cadence | `0x80F573 mov [r14+0x198], 1.0f` after each pass; `dirty` (`+0x1C4`) resets the timer (`0x80F3B8`) | | | disassembly: about once per second per actor in combat |
| Selector range gate | `0x813A2F call [rax+0x30]` (vfunc 6, max range) compared with the controller's ranges; a top-scored item out of range is skipped | | | disassembly |

Armor: a whole-`.text` scan of 1.6.1170 for an inlined `TESObjectARMO::armorRating` (`+0x200`) read followed by a float convert found no armor-rating compare, and none of the engine equip paths Harbinger names (`OutfitApply` 418622, `AddWornOutfit` 19692, `RemoveItemReequip` 16059/19689/37797, `InventoryReequip` 41244, `AiCommand` 39637/39649/39650) reads `+0x200` directly. This does NOT prove that no scorer exists (a helper call would hide it). It is why the armor row says unverified and names probe P0c.

## Key to ids and sources

Rows are grouped: **E** equip, **C** cast, **T** target and combat, **P** package/movement/idle, **L** logistics item moves, **K** hooks and infrastructure, **S** stats/progression, **O** observe-only reads.

## E: Equip (12)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| E1 | CHANGED (classification; lines) | In-combat weapon / off-hand hold (gambit equip, left-hand rule) | cast/Equip.cpp:1092 ReconcileForcedWeapon, :348 DeclareFromLedger, :743 unequip old + :745 legacy EquipObject, :880/:881 release unequips, :824 ForcedHoldFor; CombatStyle.cpp:210 ClaimEquipment; Scheduler.cpp:1932 | **With Harbinger: deny+substitute** (ch.17 declaration from the FWPN ledger, Harbinger's equip pass places it, MFO-B41 re-declares every 3 s while the weapon is in neither hand). **Without Harbinger: force** (forceEquip prevent-removal lock). Plus the ch.15 gate-only claim. The 10-11 pass missed the ch.17 road | yes in combat, RE 10-12: category 0, Melee/Ranged `0x0C`, and offensive spells compete in the same category. Field: 39 `CombatNode ... would-deny` = the AI's scored re-pick standing against the declaration | 17 (+15, +8a) | yes, named (equip seat; weapons observe-only); score seat: census | weighting in combat (phase 1 W1), keep force out of combat | high: the re-declare loop vs the engine's pick; FWPN co-save |
| E2 | STILL-ACCURATE | Gambit shield / torch equip | cast/Equip.cpp:206 EquipShieldOnMain (always direct, never in the declaration); logistics/Upkeep.cpp:177 EquipTorch, :212 EquipObject | force | yes in combat (Shield `0x818DF0`, Torch `0x819480`, category 3; field: Shield 94, Torch 56 score lines). But the torch has a light veto (`0x0F`), and shield vs off-hand weapon is order-blind | none | yes, named (equip seat; shields and torches observe-only) | weighting in combat for shield vs torch; keep deny or force for shield vs off-hand weapon and out of combat | medium |
| E3 | CHANGED (evidence; lines) | Armor / gear authority (declared worn set, scope, player picks) | logistics/EquipAuthority.cpp:659 RefreshEquipDeclaration, :234 EquipBestOwnedGear, :292 AcquireEquip of the pick; apmf/Equip.cpp:111 ClaimEquipAuthority, :194 DeclareEquipScope, :91 g_apmfEquipAuthority gate | deny+substitute (ch.17, armor ENFORCED by `bEquipEnforceArmor=1`) | **unverified, likely no**: no engine-scored armor equip in the 10-12 logs (armor came from OutfitApply, SPID, MEO, APMF); no scorer found (RE 10-12). M1a removed `ArmorClassSuits` (B17) | 17 | yes, named: the equip seat names and DENIES off-set armor from any identified source, not priority-ranked (10-12: OutfitApply 18 deny, SPID 18 deny) | keep deny (unweighted) until P0c finds a scorer; see decision D2 | high: biggest deny block; MFO-B16/B39 open |
| E4 | STILL-ACCURATE | Legacy gear wear (Harbinger absent or declined) | logistics/Gear.cpp:771 EquipObject (AcquireEquip :669), logistics/Service.cpp:145, logistics/Upkeep.cpp:275/:276 | force | unverified, same fact as E3 | none (legacy) | yes, named when Harbinger is present and declined; nothing sees it when Harbinger is absent | delete once E3 is settled; legacy = Harbinger-absent degrade only | medium |
| E5 | STILL-ACCURATE | Head-slot clear and excluded-armor strip | logistics/Gear.cpp:331 UnequipObject, :333/:334 RemoveItem | force | no (MFO policy) | none | no (unequips and RemoveItem are not seated) | keep force | low |
| E6 | STILL-ACCURATE | Off-role / excluded weapon heal and shed | logistics/Upkeep.cpp:229 HealExcludedWeapon (:275 equip), :332 ShedOffRoleWeapon, :554 DropObject vfunc | force | no (inventory policy) | none | equip half: yes, named; drop: no | keep force | low: DropObject SeatVerified |
| E7 | CHANGED (classification) | Spell / staff equip into a hand for a cast | Loadout.cpp:115 EquipObject(slot); cast/Equip.cpp:932/:936 forceEquip=false | force | an OFFENSIVE spell or staff vs a weapon: yes, category 0 (RE 10-12). Restore / Ward / buff vs a weapon: no, order table; admission through `0x0F` | 15 (gate), 8a | yes, ranked (cast facet: the 0x0F ch.8/8b paths decide against InsideSource) | in combat: weighting for an offensive spell (it shares category 0); keep force out of combat and for the cross-category hand | medium |
| E8 | STILL-ACCURATE | Weapon draw / sheathe | cast/Equip.cpp:561/:776 DrawWeaponMagicHands(true), :957 (false) | force | no (sticky flag state) | 4 (no seat of its own) | no (Core: counted only) | keep force | low |
| E9 | STILL-ACCURATE | LOTD museum deposit: unequip then store | logistics/Lotd.cpp:1292 UnequipObject, :1297 RemoveItem | force | no | none | no | keep force | low |
| E10 | NEW (2026-10-11/12, a105aba, 9313905) | Silver gambit: the silver melee weapon goes in the main hand while the foe is Unholy, the main weapon comes back after 2 s | cast/EquipSilver.cpp:398 DeclareLedger, :386/:391 legacy EquipObject (force-hold), :490-:496 switch back; ReconcileSilver runs before ReconcileForcedWeapon | the E1 road: ch.17 declaration from the FWPN ledger with Harbinger, force-hold without | yes in combat (category 0, Melee `0x0C`) | 17 (+15) | yes, named (equip seat; weapons observe-only) | weighting in combat (a W1 preference "silver while the condition holds") | medium: in-memory episode, FWPN ledger |
| E11 | NEW (9313905) | Silver ammo: ranged followers nock silver arrows or bolts under the silver gambit; restock and protect | cast/EquipSilver.cpp:184 EquipObject (forceEquip=false), :263 DeclareLedger; logistics/Silver.cpp:466 restock RemoveItem | force [declaration with Harbinger] | **unverified**: ammo has no `CombatInventoryItem` class; the combat AI does equip ammo (`CombatNode q=1 Ammo` 14 lines 10-12) but how it picks is not disassembled. Probe: disassemble the CombatNode equip paths (AE ids 48124/48126) for the ammo pick, or log ammo would-deny against the AI's pick | 17 (ammo category) | yes, named (equip seat; ammo observe-only) | keep force until the ammo pick is understood | low |
| E12 | NEW (57d387c, M1a) | Out-of-combat best owned weapon upgrade (B28/B32), gems follow | logistics/Gear.cpp:669 AcquireEquip; logistics/EquipAuthority.cpp:228/:292; logistics/LootEquipment.cpp:544 | force (MainThread::Post EquipObject; B134 stock-only when the claim is refused) | no (out of combat, the score pipeline does not run) | 17 when Harbinger is present | yes, named | keep force (or the combat substrate, week item 3) | low |

## C: Cast (17)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| C1 | CHANGED (evidence) | Spell choice: allow-list and consent (ch.8) | apmf/SpellAllowList.cpp:259 PublishSpellAllowList via CasterConsent.cpp:1318 (NoteGambits :1297); legacy 0x06 CheckStartCast CasterConsent.cpp:1215 write, thunk :552 | deny+substitute [observe in client mode: allow-list only] | yes inside a category (magic `0x0C` `0x819AA0`; categories per caster type, RE 10-12); offensive spells also compete with weapons in category 0; across categories the order table decides | 8 / 8b | yes, ranked (cast facet) | weighting inside a category (W2); keep deny only for a cross-category hand | high: the "free the AI" run (P0b) is still owed |
| C2 | STILL-ACCURATE | Friendly-fire hold and Exact concentration bound | apmf/CastGuard.cpp:62 ServiceCastGuard, Scheduler.cpp:1260; legacy 0x0A CheckCast CasterConsent.cpp:1085 | deny+substitute [claim: ch.25 in client mode] | no (aim and channel timing are inputs, not a score) | 25 | yes, ranked (CastGuard Decide) | keep deny | medium |
| C3 | CHANGED (evidence) | Owned offense cast, per hand | apmf/CastClaims.cpp:92 ClaimOffenseCast (Bridge.cpp:256 kIntent_Cast); cast/Auto.cpp | deny+substitute [claim; the AI fires its own cast] + Harbinger's `SCORE-STEER` (+1000 on the driven form, armed in the 10-12 INI) | yes, category 0 (shared with weapons); field: the steer runs | 8b | yes, ranked | weighting (the steer, re-sized) with force as the escalation argument | high |
| C4 | CHANGED (code + evidence) | Per-hand heal cast. Archers DUAL-CAST the heal when the pool allows, else a single left-hand heal plus a FULL right floor (03b25ff) | apmf/CastClaims.cpp:400 ClaimHealCast (:429 dual), :470 NoteArcherRightFloor; cast/HealRoad.cpp:829 HealNoteLauncherFloor | deny+substitute [claim] + steer | yes inside Restore (category 1). Field: the steer on Auri's heal proxies, and census verdicts `FIRED via Restore (the hand charged: ANIMATED)`. The seat-0 classify is still needed for a Restore caster to be built (§0.41) | 8b | yes, ranked | weighting for which heal; keep the seat-0 classify and the floors (cross-category) | high |
| C5 | CHANGED (evidence resolved) | Animated self-buff claim | cast/BuffRoad.cpp; apmf/CastClaims.cpp | deny+substitute [claim] | yes inside its category in combat (RE 10-12: Ward 2/3, Armor / Cloak / Invisibility / BoundItem 4/5, Light 4); out of combat no | 8b | yes, ranked | weighting in combat, force out of combat | medium |
| C6 | STILL-ACCURATE | Direct force cast (CastSpellImmediate) | cast/Direct.cpp:564/:591, cast/Auto.cpp:163/:176, cast/CastOn.cpp:1656, logistics/Service.cpp:1892/:1966 | force | no (a direct call) | none | no (CastSpellImmediate is not seated) | keep force (canonical delivery, Docs/CAST-DELIVERY.md) | medium |
| C7 | STILL-ACCURATE | Self cast direct (CastSelfDirect, anim chain) | cast/DirectSelf.cpp; cast/Direct.cpp:401/:415 attach-once | force | no | none | no | keep force | low |
| C8 | STILL-ACCURATE | Summon cast | cast/Summon.cpp:352 CastSpellImmediate | force | in combat a summon item is scored (category 4/5, Raise Zombie 120000 on 09-05), but MFO force-casts | none | no | weighting in combat, force out of combat | medium |
| C9 | STILL-ACCURATE | Forced package cast road | cast/Roads.cpp:28 ForceCast; Packages.cpp:534 SetInputs, :589 SetSelfSpell | force [package fill] | no (package precedence) | 9 | observe (Package facet: the running package's source is named, nothing denied) | keep force (fallback road) | medium: alias fills persist |
| C10 | STILL-ACCURATE | Concentration stream and restoration-at-target | cast/Roads.cpp:126 ConcentrationCast, RestorationCastDirect | force | no | none | no | keep force | medium |
| C11 | STILL-ACCURATE | Composed cast | ComposedCast.cpp | force | no | none | no | keep force | low |
| C12 | STILL-ACCURATE | Cast pose / power-attack anim kick | cast/Fire.cpp NotifyAnimationGraph | force | no | 12 | no (Core: counted only) | keep force | low |
| C13 | STILL-ACCURATE | Held-heal approach | apmf/CombatApproach.cpp:162 ServiceHealApproach | deny+substitute [claim] | no (behaviour-tree goal) | 24 | yes, ranked (Combat facet: the bound is written only while above the engine's area) | keep deny | low |
| C14 | STILL-ACCURATE | Offense approach | apmf/OffenseApproach.cpp:126 ServiceOffenseApproach; Evaluator.cpp:552 | deny+substitute [claim] | no | 24 | yes, ranked | keep deny | low |
| C15 | STILL-ACCURATE | Auto tome learn | logistics/Cast.cpp:193 AddSpell, :194 RemoveItem | force | no | none | n/a | n/a | low |
| C16 | STILL-ACCURATE | Per-hand cast lock (MFO-internal arbiter) | cast/Hands.cpp (the B3 right-first gate now also covers the Auto hybrid) | observe | no | none | n/a | n/a | low: decides who owns each hand, feeds E1/C3/C4 |
| C17 | NEW (missed 10-11; changed by 03b25ff) | Hand floors: a deny-only claim reserves an idle hand against SPELLS (`kCastFlag_FloorSpellsOnly`), or against everything for an archer's right hand during a short-pool heal | apmf/Bridge.cpp:738 ReconcileHandFloorLocked, :55 kFloorSpellsOnlyAbi; apmf/CastClaims.cpp:470 | deny+substitute (`kCastFlag_DenyHandOnly`) | no: it guards a hand across categories (a floor stops a category 1/2/4 spell or a melee swap taking the hand), which the order table decides, not a score | 8b | yes, ranked (cast facet, hand block) | keep deny | medium: field 1006b, a full floor costs the weapon |

## T: Target and combat (9)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| T1 | STILL-ACCURATE | Combat target pin / command | Targeting.cpp:39 -> apmf/Excursion.cpp:246 PinTarget (ch.20); legacy currentCombatTarget write Targeting.cpp:112-122 | deny+substitute [claim: ch.20 pin]; legacy force | yes per CH-MAP ch.20 (threat re-choose in `UpdateTarget`); the threat scorer is **unverified** (probe: disassemble `UpdateTarget` AE `0x559930` for the per-candidate score and log it passively for one fight) | 20 (+6) | yes, ranked (TargetPin restores through `SetTarget` when an identified writer is outranked; SmartNPCTargetSelector.dll seen inside the seat 10-12) | weighting if the threat score is biasable, else keep pin (not phase 1) | high |
| T2 | STILL-ACCURATE | Combat style ownership (CSTY on the live controller) | CombatStyle.cpp:216 ApplyTick, :248/:263/:292 combatStyle write | force (input write) | yes, an input: style multipliers feed the per-class `0x0C` scores (PASS S) | none | no (a foreign `combatStyle` write is not seated) | weighting (it is an input); drop the per-tick re-assert; overlaps W1's bias, see W1 risks | medium |
| T3 | CHANGED (census) | UpdateCombat hook 0xE4 (host of the T1 legacy write and T2) | Targeting.cpp:146 idx 0xE4, :200 write_vfunc, :83 ApplyTick | observe (carrier for a force write) | n/a | 20 | **yes, named, field 10-12**: `census (kPostLoadGame): seat TargetPin.Character.UpdateCombat (Target) was hooked over AFTER Harbinger by MFO.dll -- RE-HOOKED ON TOP`. MFO now runs INSIDE Harbinger's seat as a Target source | keep as the carrier of T2 only, or move ApplyTick into a Harbinger client request | high: MFO is an outside writer on a Harbinger seat |
| T4 | STILL-ACCURATE | Combat-target claim record | apmf/Excursion.cpp:142 ClaimCombatTarget | observe (record owner) | n/a | 6 | n/a (ch.6 restores nothing) | fold into T1 | low |
| T5 | CHANGED (code; anchor) | Engage on sight: start combat. Enemy test reworked (the engine's IsHostileToActor without the Aggression-0 gate, territorial creatures, a never-first skip list, 3ee638b) | EngageOnSight.cpp:699 RequestCombatEntry -> apmf/CombatEntry.cpp:115; enemy test :210-:374 | force [claim: ch.21] | no (entry is a threshold, CH-MAP 21) | 21 | observe (CombatEntry names the StartCombat source) | keep force (claim) | medium |
| T6 | STILL-ACCURATE | Retreat: StopCombat once + re-entry deny | Packages.cpp:1170 PostRetreatStopCombat, :1189 StopCombat, :2123 RetreatFill; apmf/ReentryDeny.cpp:93; Scheduler.cpp:595 | force + deny | no (an AV threshold) | 22 (+9) | yes, ranked (re-entry deny refuses a StartCombat only while above the calling source) | keep | high: StopCombat on main |
| T7 | STILL-ACCURATE | In-combat pursuit leash | apmf/PursuitLeash.cpp:85; Scheduler.cpp:687 | deny+substitute [claim] | no (leaf order) | 23 (+7) | yes, ranked (ActionGate: a leaf is stopped only while above Engine) | keep deny | low |
| T8 | STILL-ACCURATE | Peace quash: teammates fighting teammates | Rapport.cpp:373/:374 StopCombat; Scheduler.cpp:1302 QuashAllyPair | force | no | none | no | keep force | low |
| T9 | STILL-ACCURATE | Confidence / fight-or-flee reads | Evaluator.cpp; EngageOnSight.cpp:234 kAggression | observe | n/a | 11 (reads) | n/a | n/a | low |

## P: Package, movement, idle (11)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| P1 | STILL-ACCURATE | Loot travel | apmf/Excursion.cpp:435/:501 kIntent_Travel; Packages.cpp:686 SetAPMFLootTravelTarget, :1720 LootTravelFill | deny+substitute [claim: ch.19]; legacy force | no (precedence) | 19 (+9) | observe (Package facet) | keep deny | high |
| P2 | STILL-ACCURATE | Package offer | apmf/Excursion.cpp:389 OfferPackage | deny+substitute [claim: ch.9] | no | 9 | observe (the Spectator hold-off is the one enforced case) | keep deny | medium |
| P3 | STILL-ACCURATE | Alias fill / release, evict marker, EvaluatePackage | Packages.cpp:833 ClearAlias, :861 EvaluatePackage(true,false), :1526 EnsureEvictMarker, :1551 ReleaseAll | force | no | 9 | observe | keep force | high: ReleaseAll order, XMarker |
| P4 | STILL-ACCURATE | Retreat package fill | Packages.cpp:2123 RetreatFill | force [offer: ch.9] | no | 9 | observe | keep | medium |
| P5 | STILL-ACCURATE | Travel gait | Gait.cpp:8 Apply (packData.maxSpeed, :26) | force (record write) | no (a package field, CH-MAP 1a) | 1a | no (a package record write, not an AV) | keep | low |
| P6 | STILL-ACCURATE | Movement block | apmf/Excursion.cpp:725 kIntent_MovementBlock | deny+substitute [claim] | no | 1 | no (Core: counted only) | keep deny | low |
| P7 | STILL-ACCURATE | Idle during LOTD deposit | apmf/Excursion.cpp:711 kIntent_Idle; apmf/Deposit.cpp:168 | force [claim] | no | 12 | no (Core: counted only) | keep | low |
| P8 | STILL-ACCURATE | LOTD deposit trip travel | apmf/Deposit.cpp:106, :140 | deny+substitute [claim] | no | 19 | observe | keep | low |
| P9 | STILL-ACCURATE | Loose pickup and pickup sound seat | logistics/Service.cpp:931 ActivateRef; logistics/PickupSound.cpp:203 write_call<5> | force | no | none | no | keep | medium |
| P10 | STILL-ACCURATE | Chest lockpicking | logistics/Lockpick.cpp:214 SetPosition, :953 RemoveItem | force | no | none | no | keep | low |
| P11 | STILL-ACCURATE | Follow / wait / sandbox state | Followers.cpp:110/:184 read kWaitingForPlayer | observe | n/a | none | n/a | n/a | low |

## L: Logistics item moves (6)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| L1 | STILL-ACCURATE | Loot take / transfers | logistics/LootTake.cpp:129..514 RemoveItem | force | no | none | no | keep force | low |
| L2 | STILL-ACCURATE | Swap-up | logistics/SwapUp.cpp:549/:561/:671 RemoveItem | force | no | none | no | keep force | low |
| L3 | STILL-ACCURATE | Economy buy / sell, gem rules, LOTD transfers | logistics/Economy.cpp, GemRules.cpp:164/:292, Lotd.cpp:1885 | force | no | none | no | keep force | low |
| L4 | STILL-ACCURATE | Drink potion | logistics/Service.cpp:66 DrinkBest (:145 EquipObject of the potion) | force | no | none | no (ALCH is not governed by the equip seat) | keep force | low |
| L5 | STILL-ACCURATE | Out-of-combat spell cast (magicka spend) | logistics/Service.cpp:1892/:1966 CastSpellImmediate | force | no | none | no | keep force | low |
| L6 | NEW (e4f7d6d F3, MEO ABI v4) | MEO gem reconcile and carried-gem moves: MFO asks MEO.dll, and MEO re-equips the item | logistics/Service.cpp:2150 RequestGemReconcile; logistics/Gear.cpp:783 QueueGemMove; MEOBridge.cpp:1308/:1318 | force (by MEO, at MFO's request) | no | none | yes, named: 51 `External(MEO.dll)` armor equips allowed 10-12 (in-set) | keep force; the armor rework must keep MEO's re-equip of a declared piece allowed | low |

## S: Stats and progression (4)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| S1 | STILL-ACCURATE | Vital HMS base values | Followers.cpp:728 SetBaseActorValue; progression/Hms.cpp:508/:691 RestoreActorValue | force | no (inputs the engine consumes) | none | no (the AV watch covers only the ch.16 / 1a / 11 held values) | keep force | medium: PRGN v8 |
| S2 | STILL-ACCURATE | Skill points auto-scale | progression/SkillScale.cpp:98 SetBaseActorValue | force | no | none | no | keep force | medium |
| S3 | STILL-ACCURATE | Perk grant / strip (incl. the P1 owner remap, 6785897) | progression/PerkGate.cpp:318/:453/:519 AddPerk, :292/:451 RemovePerk | force | no | none | no | keep force | medium |
| S4 | NEW (d4f09c4, M2) | Roster removal: undo the perks (RestoreNativePerks), skills and HMS MFO wrote, release every claim and alias, erase the co-save records | Followers.cpp:470 RequestRosterRemoval, :530 DrainRosterRemovals; Board.cpp:1356 | force (undo writes) | no | releases all | no | keep force | medium: worker -> main -> worker sequence |

## K: Hooks and infrastructure (5)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| K1 | STILL-ACCURATE | CheckCast 0x0A hook (guard-only, Harbinger ABI 10..20) | CasterConsent.cpp:1085 write_vfunc | deny+substitute | no | 8 | n/a (MFO is the outside writer on Harbinger's t2c seat when installed) | remove once ABI 21 is the floor | high |
| K2 | STILL-ACCURATE | CheckShouldEquip 0x0F hook | CombatStyle.cpp:527 write_vfunc, Harbinger-absent only | deny+substitute | no (boolean gate) | 8 / 15 | n/a (Harbinger-absent only) | remove in client mode | medium |
| K3 | STILL-ACCURATE | ClaimCombatActionDeny | apmf/Excursion.cpp:627; still no MFO caller (APMFBridge.h:797 declaration only) | deny+substitute | no | 7 | yes, ranked (ActionGate), if it were used | delete or mark dormant | low |
| K4 | STILL-ACCURATE | Player Update hook (main-thread pump) | MainThread.cpp:115 write_vfunc | observe (infra) | n/a | none | n/a | keep | high: the only road to main |
| K5 | STILL-ACCURATE | UI input / present hooks | Board.cpp trampolines, ImGui overlay | observe (UI) | n/a | none | n/a | keep | not an NPC decision |

## O: Observe-only (2)

| ID | Status (10-12) | Feature | File:line and facet | Mechanism today | Engine decides by weighting? (evidence) | Harbinger channel | Captures outside writes? (from the census) | Target after rework | Risk |
|---|---|---|---|---|---|---|---|---|---|
| O1 | STILL-ACCURATE | MFO line of sight (Harbinger v18 first) | apmf/Los.cpp:78 LosOf; Sightline.cpp | observe | n/a | LOS (v18) | n/a | keep | low |
| O2 | STILL-ACCURATE | Passive probes / observers | Probe.cpp:34 StartCombatOn (debug), ProgProbe.cpp, [obs] logger | observe | n/a | none | n/a | keep | low |

## PHASE 1 PLAN: weapons, armor and spells/casting on weighting

marth's order for this week (2026-10-12): (1) verify Step 0 (this file), (2) **weapons, armor and spells/casting on weighting (this plan)**, (3) the combat substrate for out-of-combat casting, (4) the rest of the weighting.

### The one mechanism under all three

There is ONE engine scorer for weapons, shields, torches and spells in combat: `CombatInventoryItem::CalculateScore` (vfunc `0x0C`), one body per weapon class and one shared body for every magic and staff item. `CombatInventory::Update` rescores every item about once a second, sorts each category, and the selector equips the top of each category in the order `[1,2,4,0,3,5,0,6]` while hands are free. Harbinger already has a verified, installed, field-running seat on every one of those `0x0C` slots on all three runtimes. **Phase 1 is therefore not a new engine seat. It is a new client-facing bias on seats that exist**, plus retiring the deny paths that fight the score today. That keeps the Harbinger half at tier A (ABI and the behaviour of an engine seat) but small.

Bias model (needs marth's call, D1): the field shows an additive constant is the wrong shape (scores 0.22 to 1320 on LoreRim 10-12, 0.08 to 29 on the 09-06 Tuxborn run). The proposal is **relative lift inside the category**: the seat records the highest engine score each actor's category produced on the previous pass, and a preferred item returns `max(own, rivalMax) * (1 + margin) + epsilon`. The rescore is per item, so it uses the previous pass's rival maximum; the cadence is about 1 s (principle 9). Two hard rules: never lift an item the engine scored 0 (a bow with no ammo scores 0, PASS S; lifting it equips an unusable bow), and never bias a form the declaring client did not declare for that actor and hand.

### Probes first (principle 5; no behaviour change; one deck run, can share a deploy)

| Probe | Question | How | Settles |
|---|---|---|---|
| P0a | What bias size steers without breaking things? | mostly answered by the 10-12 Group C data (per actor, per category score spread). Pull the per-actor maximum per category from the same lines on the Tuxborn deck too | the bias model (D1) |
| P0b | "Free the AI": does the AI cast the top-scored spell when nothing narrows it? (the 09-05 run was confounded by MFO's own claim) | MFO cast gambits off for one follower; `[AiCastSeats] EnableItemScoreProbe=1` (the magic `0x0C` log) + `bCasterTypeCensus=1`; read which item the selector equipped and which caster fired | whether score alone selects spells in practice (the W2 premise) |
| P0c | Is there an engine-scored armor choice at all? | one follower with `bEquipEnforceArmor=0` (or an unclaimed NPC), hand it a higher-rated off-class piece by trade; read `[apmf][equip-obs] path=` for that piece. If a named engine path equips it, disassemble that path for the rating compare | whether armor can ever be weighting (W3) |
| P0d | Weapon vs offensive spell in category 0, in the field | from P0b's log: a hybrid follower's category-0 list carrying both weapon and spell items, and which one the selector equipped | the hybrid half of W1/W2 |

### W1. Weapons in combat (E1, E2, E7 offensive half, E10)

- **Engine scorer to bias**: Melee `0x8183E0`, Ranged `0x8188B0` (category 0), Shield `0x818DF0`, Torch `0x819480` (category 3); SE and 1.7.104 per the evidence table. **Runs**: yes, field 10-12 (4629 lines). Torch and OneHandedBlock `0x0F` seats are refused on 1.7.104 by HandBlock today; their `0x0C` rows are verified.
- **What MFO declares**: per follower, the in-combat preferred weapon per hand (from the FWPN ledger it already keeps: `.right`, `.left`), the shield-or-torch preference, and the silver episode's weapon while it is open. One declaration per change, never per tick.
- **What Harbinger changes**: a new append-only "equip preference" request (proposal: `kIntent_EquipPreference`, a list of `{form, hand, weight}` plus a `force` flag; APMF_API.h append-only, ABI bump). The `0x0C` thunks apply the bias above for that actor's declared forms. `force=1` means today's behaviour: the ch.17 weapon declaration with weapons enforced for that claim (the existing `bEquipObserveOnly=0` path, per claim). It is logged loudly and documented as discouraged. The Equip facet priority applies: a lower-priority client's preference does not bias.
- **Retires** (in combat, with Harbinger present): the weapon entries of the ch.17 declaration from the ledger (`DeclareFromLedger` for hands), the MFO-B41 "weapon in neither hand, re-declare" loop, Harbinger's equip pass placing weapons, and the ch.15 gate-only claim's denial of OFFENSIVE spell re-arm (category 0 is a score fight, so W1 and W2 must agree, see D3). **Stays**: the force-hold road when Harbinger is absent (the documented degrade), the ch.15 / hand-block admission deny for Restore, Ward and buff spells (categories 1/2/4, order table), shield vs off-hand weapon (order table), every out-of-combat weapon equip (E12, M1a), draw and sheathe.
- **Risks**: (a) T2 also changes these scores (CombatStyle multipliers), so a bias sized on one style is wrong under another: size against the live score, never a constant; (b) the selector's range gate (`0x813A2F`) skips an out-of-range top item, so a preferred bow at melee range will not be picked: that is the engine being right, not a miss to escalate on; (c) MFO's own spell bias and weapon bias meet in category 0: one coherent per-hand preference, or MFO fights itself; (d) the second category-0 pass (order index 6) fills the second hand, so a two-hand preference must be expressed per hand; (e) FWPN co-save: the ledger stays as it is, only how it is enforced changes (a layout change would make it tier A).
- **Tier**: the Harbinger facet is A (ABI + seat behaviour). The MFO consumer is B, unless it touches the FWPN layout (A).

### W2. Spells and casting in combat (C1, C3, C4, C5, C8, E7)

- **Engine scorer to bias**: magic `0x0C` `0x819AA0` / `0x77F6F0` / `0x82E980` (one function, all 30 MagicT vtables, Group A). **Runs**: yes, the steer fired on Auri's heal proxies in the 10-12 logs; Restore casters were built for claims and the census verdict reads `FIRED via Restore (the hand charged: ANIMATED)`.
- **What MFO declares**: the chosen spell per hand (it already does, in the ch.8b claim), as a preference instead of a deny of the rest. An offensive spell competes with weapons in category 0 (W1, D3).
- **What Harbinger changes**: the existing `SteerScoreForCastClaim` becomes the default enforcement of the ch.8b spell choice, with the W1 bias model instead of `+1000`, and `EnableScoreSteer` defaults to on. The ch.8b `force` flag keeps today's denies (the allow-list, CheckStartCast NO for other spells) as the escalation.
- **Retires**: the ch.8 allow-list deny (`PublishSpellAllowList`) for a spell in the same category as the chosen one, and the claim's refusal of the AI's other in-category spells. **Stays** (unweighted): `GetMagicTarget` (0x0A) aim substitution, the CheckStartCast yes/no for target validity, the seat-0 classify that makes the engine build a Restore caster (§0.41), the hand floors (C17) and the "one spell at a time while a heal charges" rule (cross-category hands), the friendly-fire hold and concentration bound (C2), the approach claims (C13/C14), and every out-of-combat cast (C6, C7, C10, L5: week item 3).
- **Risks**: (a) P0b is unrun: if the selector does not pick the top-scored spell in practice, W2 falls back to force by data, not by default; (b) scores across caster types are not comparable (120000 Raise Zombie vs 360 Firebolt on 09-05), so the bias must work within a category only, which the relative model gives by construction; (c) a proxy spell (a minted form) scores differently from its base spell (10.08 vs 1008 seen), so the bias keys on the driven form, as the steer does today; (d) heals: Restore is walked first, so a biased heal wins its hand before offense, which is the wanted order; an unwanted heal is still an admission deny (floors).
- **Tier**: Harbinger A (seat behaviour + ABI flag). MFO B.

### W3. Armor (E3, E4, L6)

- **Engine scorer to bias**: **none found.** Weighting cannot apply to a choice the engine does not score, and the 10-12 logs show that armor changes on followers come from the outfit re-apply (unweighted, §0.46 addendum) and from other mods' direct force (SPID, MEO).
- **Plan**: run P0c. If a scored engine path appears, it gets its own brief (disassemble it, seat it, bias it like W1). If not, **armor keeps the ch.17 declaration + deny as the honest answer**, which the rule allows for an unweighted choice. Phase 1 changes nothing in armor enforcement.
- **Still to fix either way** (already tracked): MFO-B16 (marth's stopgap (a)), MFO-B39 (the authority toggle), APMF-B7.
- **Risk / decision**: today's ch.17 armor deny is not priority-ranked, and it refused SPID 18 times and OutfitApply 18 times in the 10-12 logs. See D2.

### Where weighting cannot work (deny or force stays the honest answer)

1. Weapon vs Restore, Ward or buff spell for a hand (order table, categories 1/2/4 before 0): admission deny (ch.15 gate, hand block, floors).
2. Shield or torch vs a second weapon in the off hand (category 3 before the second category-0 pass): admission deny (the dual-wield preference).
3. The torch light veto (Torch `0x0F`).
4. A zero-scored item (a bow without ammo): never lift it.
5. Anything out of combat: the pipeline does not run. Force, or the combat substrate (week item 3).
6. Armor, until P0c shows a scorer.
7. Cast aim, CheckStartCast validity, the caster classify, channel bounds: inputs and gates, not scores.

### Build order

1. **P0a-P0d probes** in one deck run (config only, no code). Opus concludes (the field-diagnosis rule).
2. **Harbinger: the equip-preference bias on the existing `0x0C` seats** (weapons and magic, one change), ABI append, the `force` flag, the relative bias model, `EnableScoreSteer` default on. Tier A, Opus author, Opus tier-3 review.
3. **MFO W1 consumer**: declare weapon preferences from the ledger in combat; retire the weapon half of the ch.17 declaration and the MFO-B41 re-declare loop when Harbinger is present; observe the outcome (`[equip]` readback, equip-obs) and escalate to `force` only on an observed miss. Tier B (A if FWPN changes).
4. **MFO W2 consumer**: ch.8b claims rely on the bias; retire the in-category allow-list deny. Tier B.
5. **W3 armor**: only after P0c; otherwise no change.
6. **Then** (week item 3) the combat substrate decides how much of the out-of-combat force (E12, C6, C7, L5) can become weighting by giving the engine a controller to score with.

### Decisions marth must make before phase 1 starts

- **D1 Bias model.** An additive constant (today's `+1000`: too small on LoreRim, far too big on the 09-06 Tuxborn numbers) or a relative lift inside the category (proposed), and the margin.
- **D2 Equip facet priority.** The ch.17 equip deny is "not ranked by priority" (Harbinger FACETS Appendix A), so it denies a positively identified SPID or outfit re-apply armor equip whatever their priority. The rule allows it (armor is not weighted), but it differs from "priority wins outright, per facet" and, in spirit, from "they force they win". Keep it, or rank equip denies by priority like every other facet (SPID at the unregistered 20 would still lose to MFO at 50)?
- **D3 Category-0 policy for hybrids.** Offensive spells and weapons share category 0. When a follower has both a weapon preference and a cast claim on the same hand, which does MFO declare? Proposed: the cast claim's hand carries only the spell preference (marth 2026-10-06: "a claimed hand blocks every other action ... until we redo using the weighting, which will accomplish the same thing").
- **D4 Escalation threshold.** How long MFO watches a missed preference before it sends `force` (the 2026-10-05 API design). Proposed: two rescore passes (about 2 s) in combat, logged loudly.
- **D5 `EnableScoreSteer` default.** It is on in the LoreRim INI and off in code. Phase 1 makes it the default enforcement.

## Method and gaps

- Rows were re-confirmed by grep on f3e864a. New rows came from `git diff 2576701..f3e864a -- native` and the merge log.
- The "captures outside writes" column comes from Harbinger 80f699b: `native/core/FacetAttribution.*`, `Sources.h` (facets), the `Decide` / `Observe` call sites (TargetPin, ActionGate, CastGate, CastSeats, CastClassify, CastGuard, EquipGate, EquipSink, PackageSources, SpectatorGate, ActorValueObserve, CombatEntry, CombatReentryDeny, CombatApproach) and `Docs/FACETS.md` Appendix A, cross-checked against the census lines in the 10-12 logs.
- Not re-verified: T1's threat scorer (not phase 1), the ammo pick (E11), the armor scorer (E3, probe P0c), every CH-MAP claim outside phase 1.
- Follow-up docs task (outside this brief): ENGINE_NOTES §0.42 should say that offensive magic is category 0 with the weapons (only Restore, Ward and buff categories precede it), and that its "~0.08-29" range is the 09-06 Tuxborn run only (LoreRim 10-12: 0.22 to 1320).

## Decisions (marth, 2026-10-12)

- **D1 bias model:** relative lift inside the category, "a step above" the best competitor: the smallest margin that makes the preferred item win. Never lift an item the engine scored 0.
- **D2 equip deny priority:** equip denies rank by priority like every other facet. A source that outranks the claim keeps its equip.
- **D3 hybrid hand:** a hand held by a cast claim carries only the spell.
- **D4 escalation:** force after about 2 s, but every escalation is an anomaly to investigate and log when it would violate Harbinger's priority sliders.
- **D5 EnableScoreSteer:** on by default, and still bound by the same priority sliders as everything else.
