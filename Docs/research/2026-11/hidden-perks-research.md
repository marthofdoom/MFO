# Hidden player-only perk effects for followers: research + design (ClickUp 86e3eevgz)

Read-only research round, 2026-11-09. Sources: ClickUp task text, MFO origin/main @ 912e545 (git show only), the CommonLib
fork mit-3.7 @ 0b904231, and the unpacked executables in `binaries/` (1.6.1170.0, 1.5.97, 1.7.104). No list or mod data
was read. Every list-dependent decision below is made at runtime by a heuristic.

Reproducible tooling (scratchpad, not committed):
`<scratchpad>/re/ep_census.py` (HandleEntryPoint call-site census), `vf_census.py` (HasPerkEntries / ForEachPerkEntry
vfunc sites), `ctx.py` (function id + string context per site), `arity.py` (argument setup per site), `dz.py`
(disassembly window). Outputs: `ae.json`, `se.json`, `v7.json`, `aevf.json`.
Address Library files used offline: `_commonlib/live-alandtse-ng/tests/REL/versionlib-1-6-1170-0.bin` and
`version-1-5-97-0.bin` (data files only; self-check NotifyAnimationGraph 38048 -> 0x6A35F0 and SE 37020 -> 0x60F240 passed).

---------------------------------------------------------------------------------------------------------------------

## (a) Which entry points the executable applies only to the player

### Method

`BGSEntryPoint::HandleEntryPoint` (fork `BGSEntryPoint.h:112`, `RELOCATION_ID(23073, 23526)`; MFO row
`TradePerks.BGSEntryPoint.HandleEntryPoint`): 1.6.1170 `0x385F30`, 1.5.97 `0x32ECE0`, 1.7.104 `0x38CFF0`. It is not a
thunk and has no thunks. Every `call/jmp rel32` to it was found, the containing function taken from `.pdata`, and the
function disassembled up to the call while tracking `ecx` (the entry point) and `rdx` (the perk owner). An owner loaded
straight from the PlayerCharacter singleton (`Offsets.h:457` `RELOCATION_ID(517014, 403521)`: AE `0x31874F8`; 1.7.104
`0x3230778`, derived from the EP59 lockpick site `0x94EB79`) is "owner = player". Seven sites pass the entry point in a
register; all seven were resolved by hand (below).

The other evaluation roads were checked too. `Actor::HasPerkEntries` (vfunc 0xFF, +0x7F8) and `ForEachPerkEntry` (0x100,
+0x800) have only 3 indirect call sites on 1.6.1170: two inside HandleEntryPoint itself and one EP18 check. So
HandleEntryPoint is the only road for entry-point effects.

Result: **1.6.1170 has 115 call sites, 1.7.104 has 115 with an identical classification, 1.5.97 has 109.** SE differs
only in site counts for EP 6/10/12/46/82/83/84 (inlining). No EP changes class between runtimes.

### Class 1: player-only BY ENGINE CODE (every site passes the player singleton as owner, all three runtimes)

"Function" is the Address Library id (AE) of the function that holds the call. Arity is read from the call-site
argument setup (condition args between the owner and `&value`). The engine's EP table (AE `0x1FF6110`) is zero-filled in
the image and built by a static initializer, so arity has to come from the call sites. That is the WillWork lesson: arity
varies per EP and is checked for each one.

| EP | Name (fork) | AE site | Function (AE id), context | Cond args | MFO follower activity |
|---|---|---|---|---|---|
| 8 | kModBuyPrices | 0x8EAFCA | 50945 BarterMenu (`InventoryMode`, `entryList`) | 1 (vendor ref) | **Pricing** (`logistics/Pricing.cpp:104`, already queried with the follower as owner) |
| 60 | kModSellPrices | 0x8EAFE5 | 50945 BarterMenu | 1 (vendor ref) | **Pricing** (`Pricing.cpp:105`) |
| 59 | kModLockpickSweetSpot | 0x9381E5 | 51964 LockpickingMenu Show (`UpdateSweetSpot`) | 1 (lock ref) | **Lockpick** (`Progression.cpp:1525` `LockpickEntryPointsFor`) |
| 63 | kSetLockpickStartingArc | 0x9382A7 | 51964 | 0 | **Lockpick** |
| 65 | kMakeLockpicksUnbreakable | 0x938D72 | 51975 pick damage (`UpdatePickHealth`) | 0 | **Lockpick** |
| 86 | kModLockpickingCrimeChance | 0x939AE9 | 51981 lockpick menu | 1 (lock ref) | Lockpick, but **not consumed today** (MFO picks only non-crime locks) |
| 90 | kModLockpickingKeyRewardChance | 0x938807 | 51968 lockpick menu | 1 | Lockpick, **not consumed today** (no key reward road) |
| 16 | kIgnoreBrokenLock | 0x17B793 | 12402 | 0 | Lockpick, **not consumed today** |
| 61 | kCanPickpocketEquippedItem | 0x8FEBC0 (register-passed, `mov ecx,0x3D`) | pickpocket item filter | 2 | **Steal gambit** (future) |
| 75 | kCanDualCastSpell | 0x6C3F4A, 0x6C415D (register-passed `lea ecx,[rax+0x4B]`, rax = 0) | magic caster | 1 (spell) | **Cast dual-cast decision** (`Loadout.cpp:310` `OwnsDualCastPerk` reads a vanilla perk form today) |
| 22 | kModSkillUse | 0x77AFB8 | 41561 PlayerSkills | 0 visible, **the skill is passed through `PlayerCharacter+0xAF4`, written just before the call** | **XP**: not portable (see note) |
| 5 | kAdjustBookSkillPoints | 0x27AB7F | 17842 skill book read (`UISpellLearned`) | 0 | none (followers do not read books) |
| 9 | kAddLeveledListOnDeath | 0x697B61 | 37896 death handler, gated `killer == player && victim != player` | 2 | **Loot**: follower kills (needs its own road, card C6) |
| 72 | kModInitialIngredientEffectsLearned | 0x6C2B34 (register-passed, `lea ecx,[rdi+0x48]`, rdi = 0) | eating, gated actor == global id 401069 and form type INGR | 1 | none |
| 76 | kModTemperingHealth | 0x90BA85 | 51360 smithing menu | 1 (item) | **ErrandSmith** (`logistics/ErrandSmith.cpp:416`, already queried with the follower) |
| 66 / 73 / 89 | AlchemyEffectiveness / PurifyAlchemyIngredients / PotionsCreated | 0x906EE7, 0x906F66 / 0x90336D / 0x909B98 | 51289 / 51239 / 51354 alchemy menu | 0 / 0 / 1 | none (no follower alchemy today) |
| 77 / 79 / 80 | EnchantmentPower / SoulGemEnchanting / NumberAppliedEnchantmentsAllowed | 0x906A47 / 0x9113C4 / 0x90519D | 51283 / 51449 / 51242 enchanting menu | 2 / 2 / 0 | none |
| 49 | kModSoulGemRecharge | 0x92DD88, 0x92E0E9 | 51858 / 51859 recharge menu | 1 | none |
| 83 | kModPoisonDoseCount | 0x7367DD | 40482 apply poison (`ITMPoisonUse`) | 2 | none (MFO applies no poisons) |
| 43 / 46 | ModPlayerIntimidation / ModBribeAmount | 0x675BC8 / 0x67595F, 0x675ACC | 37500 / 37498-37499 speech | 1 | none |
| 14 / 74 / 81 | Activate / FilterActivation / SetActivateLabel | 0x757B91 / 0x4031AD, 0x744F5C / 0x744FC7 | 41003 / 26127, 40621 activation | 2 / - / - | none |
| 82 | kModShoutOK | 0x7455EC, 0x745863 | 40628 / 40631 | 0 | none |
| 3 / 19 | MineExplodeChance / ModMaxPlaceableMines | 0x7EE160 / 0x41AEBF | 44217 / 26521 | 1 / 0 | none |
| 21 | kModRecoverArrowChance | 0x7DF990 | 44031 (the player's arrows only) | 0 | none (follower arrows are not the player's) |
| 24 / 25 | TelekinesisDamageMult / TelekinesisDamage | 0x5D75E8 / 0x7DB719 | 35052 / 44005 | 0 / 1 | none |
| 84 | kShouldApplyPlacedItem | 0x8FC555, 0x8FDB84 | 51141 / 51151 | 2 | none |
| 20 | kModBowZoom | 0x733051 (PlayerCharacter-only vfunc 173), 0x7B9454 | player camera | - | none |

**EP22 note.** The call is `HandleEntryPoint(22, player, &v)` right after `mov [player+0xAF4], skill`. The perk
conditions find "which skill" through that player field. A follower evaluation cannot supply it without writing the
player's field (forbidden). MFO's progression is level-with-player and has no skill-use XP for EP22 to scale. So EP22 is
**no follower activity** unless marth wants a use-XP model (question Q8).

### Class 2: applies to ANY actor (generic owner), engine-applied once the perk is on the follower

0 CalculateWeaponDamage, 1/2 crit chance and damage, 4 AdjustLimbDamage, 6 ModRecoveredHealth (AV-indexed table at
0x6B2124), 7 GetShouldAttack, 10 GetMaxCarryWeight (2 generic sites + 1 player site), 11/12 addiction, 15
IgnoreRunningDuringDetection, 17/18 enemy crit / sneak attack mult, 23 TK distance, 26 bash damage, 27 power-attack stamina
(one generic site + PlayerCharacter vfunc 173), 28 power-attack damage, **29/30 spell magnitude/duration** (the generic
sites apply them; the player-owned sites at 51900/51905 are only ItemCard description text), 31/32 weights, 33/34 stagger,
35-42 attack/incoming damage, resist, spell cost, block, deflect, incoming spell magnitude/duration, **47/48/57 detection**,
50 sweep, 51/52/53/67/69 apply-spell EPs, 54/55 graph var / cast sound, **56 ModPickpocketChance** (the owner is the thief
argument; only the player pickpockets in vanilla, so this is the "activity is player-only" kind: Steal gambit), 58 falling,
68 commanded-actor limit, 70 magic slowdown, 71 ward absorb, 78 soul to weapon, **85 ModArmorRating**, 87 ingredients
harvested, 88 spell range, 91 mount.

### Class 3: never evaluated by the executable

13 ModPositiveChemDuration, 44 ModPlayerReputation, 45 ModFavorPoints, **62 ModLockpickLevelAllowed**, 64
SetProgressionPicking. No immediate site exists on any runtime, and every register-passed site except two data-driven
dispatchers (0x38766E reads the EP from a data byte; 0x6B2124 maps an actor value to an EP, sentinel 0x5C) now resolves to
a known EP. **This strengthens the Progression.h:448 "EP62 PARTIAL" note:** the 22 unresolved register sites from the LP-M1
review are now resolved, and the two that remain are generic dispatchers, not lockpick code.

### Ship these as FIXED ENGINE DATA

The class table is identical on 1.6.1170 and 1.7.104 and matches on 1.5.97. It ships as a constexpr table (EP -> {class,
activity}) in the new progression file, documented in Docs/ENGINE_NOTES.md with the site addresses above. The census
script goes into `tools/` so a new runtime regenerates and diffs it, the same discipline as `VerifiedAddresses.h`.

---------------------------------------------------------------------------------------------------------------------

## (b) MFO machinery to reuse

| Need | Existing piece (origin/main) | Note |
|---|---|---|
| Add a perk to a follower and settle the live actor | `TESNPC::AddPerk` (fork `TESNPC.h:238`) + `SetLivePerkRank(actor, perk, old, new)` (`progression/PerkGate.cpp:386`, declared `ProgAllocator_internal.h:316`) | **Never `Actor::ApplyPerksFromBase`**: it re-applies every base perk with old rank 0 and duplicates every EP entry (fixed in c3018f1, MAP §5 "LIVE PERK-ENTRY SETTLE"). `SetLivePerkRank` is presence-checked, MAIN THREAD, and a no-op with no middle-high process. |
| Remove | `TESNPC::RemovePerk` + `SetLivePerkRank(actor, perk, 1, 0)` | Same pair `ForgetFollower` / `Respec` use (`Verbs.cpp`). |
| Persistence facts | ProgAllocator.h (`RestoreNativePerks` comment) + MAP §5 B' strip | **Base AddPerk/RemovePerk are NOT written to the .ess (P3), but they DO persist in memory across a same-session reload (review F1).** This drives the (f) design. |
| Main-thread cadence | `PollWork` (`progression/Poll.cpp:72`), posted on the MainThread pump by `PollTick` | Already walks `g_prog` and `Followers::ActiveSnapshot()` on the main thread. The reconcile belongs here. |
| Dismiss / off / forget | Followers drop after `kMissesBeforeDrop` (3) misses; `ReleaseHeldStateImpl` (`Followers.cpp:421`, worker side); `ForgetFollower` (`Followers.cpp:583`, posted to main); MFO-OFF via `IsMfoEnabled` | Do not hook the worker drop. Diff "should hold" against "holds" in PollWork (main), which covers dismiss, OFF, death and forget in one place. `ForgetFollower` also calls the remover. |
| Old-save sweep pattern | memory `fix-forward-paths-sweep-old-save-state.md`; `Packages::ReleaseAll("kPreLoadGame")` (`plugin.cpp:440-459`); `RestoreNativePerks` | Two halves: stop writing it, and sweep what is standing at load. |
| EP evaluation, guarded | `TradePerks::EntryPointValue` (`logistics/TradePerks.cpp:115/122`): `ProvenCondArgs` (`:74`, 8/60/76/59 = 1 arg, 63/65 = 0), seat `SeatVerified`, middle-high/perkData guard, arg-type check (MFO-B323) | MAIN THREAD. Today `LockpickEntryPointsFor` (`Progression.cpp:1525`) and `cast/Summon.cpp:112` call `RE::BGSEntryPoint::HandleEntryPoint` directly, unguarded (open MFO-B324). |
| EP arity lesson | `cast/WillWork.cpp:118`: EP29 is (owner, spell, target ref, &v), EP68 is (owner, spell, &v) | Every new EP added to `ProvenCondArgs` takes its count from the call-site table in (a). |
| Player-pin detection | `Progression.cpp:387` `ConditionPlayerGated` (narrow: `GetIsID`/`GetIsReference` Player 0x7 / PlayerRef 0x14, `==`, literal 1, not OR'd) and `:428` `ReadOwnerFacts` (selfPin, playerRefRunOn, playerRefParam, teammate) | Both are in Progression.cpp's anonymous namespace. Progression.cpp is **1617 lines** (past ~1500), so exposing them is a STOP-and-propose under the split rule. |
| Identical-conditions test | `SameConditions` (`Progression.cpp`, used by `BuildPerkTwins`) | Reuse for rule 2. |
| Per-item condition evaluation | `progression/PerkGate.cpp` `EvalPerkConditions` / `EngineItemTrue` (stack copy of one item through `TESCondition::IsTrue` row 29888) | The road for a later remap evaluator (Q4). |
| Live-entry census | `CensusBasePerkEntries` + `[perk-probe]` (`bPerkEntryProbe`) | Use it to prove no duplication in the tests. |
| VM script detection | `IObjectHandlePolicy::GetHandleForObject` (used at `Packages.cpp:257`, `Papyrus.cpp:27`); fork `VirtualMachine.h:219-220` `attachedScriptsLock` / `attachedScripts` | For rule 4 (a scripted MGEF on an ability). |

---------------------------------------------------------------------------------------------------------------------

## (c) Progression tab display hook

- Payload: `ProgAllocator::BoardFollowerView` (`progression/ProgAllocator.h:501`). Add `std::vector<InheritedPerkLine>
  inherited` (name, status enum, short reason, EP list text). Built in `PublishBoardViews` (`progression/BoardViews.cpp:306`,
  main thread) from the progression-owned per-follower inherit state. Refcount-copied snapshot, so the render thread never
  touches live state.
- Draw: `Board_Progression.cpp` (1291 lines) `DrawProgressionTab`, on the per-follower overview after the skills table
  (`~:296-420`). Add a collapsible "Inherited from you" section with one row per discovered perk and a status chip:
  `Copied`, `Duplicate (<perk>)`, `Player only (conditions)`, `Not copied: script part`, `Not copied: no follower use`,
  `Not copied: shared base`. It must be gamepad-navigable (family controller rule). Strings go in
  `i18n/Strings_keys.h` (`Pk_`/`Pg_` keys; `Pk_PlayerOnlyPart` already exists).
- The tab exists only with the add-on (`g_ready`). See Q1 for whether the feature itself is core.

---------------------------------------------------------------------------------------------------------------------

## (d) Runtime discovery: "a perk on the player that is in no ActorValueInfo perk tree"

**Tree set, once at kDataLoaded (main thread):**
1. Every built-in AV: `RE::ActorValueList::GetSingleton()->GetActorValue(av)` for `av` in `[0, ActorValue::kTotal)`
   (fork `ActorValueList.h:11/17/23`). This covers the 18 skills AND the beast trees (werewolf, vampire lord).
2. Every AVIF record in the data: `TESDataHandler::GetFormArray<RE::ActorValueInfo>()`. This also covers custom skill trees
   that a framework defines as their own AVIF records (verify the FormType mapping before relying on it).
3. For each `ActorValueInfo::perkTree` (`ActorValueInfo.h:89`), BFS `BGSSkillPerkTreeNode::children`
   (`BGSSkillPerkTreeNode.h:24`, `perk` at `:26`) with a visited set, and follow each node perk's `BGSPerk::nextPerk`
   chain (`BGSPerk.h:91`, bounded 16 and cycle-guarded like `Progression.cpp:859`). Store an
   `unordered_set<FormID>`. MFO's `BuildSkill` already does this for the 18 skills (`Progression.cpp:791-859`), so reuse
   its walker.

Cost: roughly 20-25 trees and about 1,000 perk forms, once. Negligible.

**Player perks, main thread:** the union, deduped, of
- the player base `TESNPC` perks (`BGSPerkRankArray::perks/perkCount`, `BGSPerkRankArray.h:40-41`);
- `PlayerCharacter::GetPlayerRuntimeData()` (`PlayerCharacter.h:828`) `.addedPerks` (`PerkRankData*`), `.perks`
  (`BGSPerk*`) and `.standingStonePerks` (`:679-681`). **Disasm check:** PlayerCharacter vfunc 0xFB (AddPerk) inserts the
  perk into the array at `this+0x4C8` on SE (`0x6B41C0`) and `this+0x4D0` on AE (`0x748F20`). That matches the fork's
  `perks` member with the +8 AE runtime-data shift. The accessor resolves the shift, so use it, never a raw offset.
- each candidate confirmed with `player->HasPerk(perk)`. A held rank N counts as its own form.

Candidate = held by the player AND not in the tree set AND not one of MFO's own forms. `BGSPerk::data.hidden/playable`
(`BGSPerk.h:21-22`) goes into the census log as a diagnostic only. It is not the criterion.

**When:**
- `kPostLoadGame` / `kNewGame`, after the player's perks are restored (first PollWork after the load).
- Then on change only. Each PollWork computes an O(1)/O(N) signature: the sizes of the three runtime arrays plus the base
  count, plus a hash of (perk, rank) over `addedPerks` (about 100-400 entries, microseconds). No reliable "perk added"
  event exists. Player level-up is already seen in PollWork (`d != 0`) and also triggers a recompute.
- Classification per candidate walks its `perkEntries` and their condition lists: microseconds per perk. The whole
  rediscovery stays well under 1 ms.

---------------------------------------------------------------------------------------------------------------------

## (e) Classification, procedural (in the task's order)

Fork ids (`TESCondition.h`): GetIsID 72, GetIsReference 136, GetPCIsClass 129, GetPCIsRace 130, GetPCIsSex 131,
GetPCInFaction 132, IsPCSleeping 175, IsPCAMurderer 176, GetDistance 1, GetInSameCell 32, GetPlayerTeammate 453, HasPerk
448, GetBaseActorValue 277, GetActorValue 14, GetGlobalValue 74, GetLockLevel 65. Perk entry types (`BGSPerkEntry.h:14-16`):
kQuest 0, kAbility 1, kEntryPoint 2. EP functions (`BGSEntryPointPerkEntry.h:22-36`): value functions 1-7 and 12-14,
kAddLeveledList 8, **kAddActivateChoice 9 (the only carrier of perk script fragments)**, kSelectSpell 10, kSelectText 11,
kSetText 15.

**Design core: copy the perk FORM onto the follower and let the engine evaluate it with the follower as owner.** Then the
engine's own `HandleEntryPoint(ep, follower, ...)`:
- evaluates every condition with the follower as Subject. That is rule 3's "skill AVs, globals, lock band" for free.
- applies the class-2 EPs (damage, armor, detection) natively.
- answers progression's class-1 queries (Pricing, Lockpick, ErrandSmith already ask with the follower as owner) with the
  inherited entries included, with no reimplementation of EP function math.

Per candidate perk P:

0. **Already held.** The follower's base already holds P (same form, e.g. an engine skill-boost perk many NPCs carry):
   `Duplicate`, skip.
1. **Rule 1 (activity).** Collect P's EP ids. If every EP entry is class 1 with activity `none` (and P has no
   ability/class-2 part), mark `Not copied: no follower use`. The EP class table comes from (a).
2. **Rule 2 (identical only).** P is a duplicate only if EVERY EP entry of P has an identical twin among the follower's
   base perks' entries: same `entryData.entryPoint`, same `function`, same function data (`OneValue::data`, spell, leveled
   list or text), and identical conditions (`SameConditions`). The perk's rank and priority do not matter. Anything less
   is copied, so a double bonus is possible and accepted.
3. **Rule 3 (is-the-player only).** Skip P when every firing part (every EP entry's owner-tab condition list AND every
   ability effect's conditions) is player-pinned in the NARROW shape: `ConditionPlayerGated` true, meaning an AND-required
   `GetIsID`/`GetIsReference` naming 0x7 / 0x14, `==`, literal 1, not OR'd. GetPCIs* on the Subject count the same way:
   they test the player and are false on a follower. A part that also tests skills, globals or a lock band stays: the
   pin is then the only thing the engine rejects, and the follower-as-Subject evaluation does the rest. Note that the
   engine would itself evaluate a pinned entry false on a follower. The skip is about not mutating the actor and not
   cluttering the list, not about correctness.
4. **Rule 4 (script or spell parts).** A script part is (i) an EP entry whose function is `kAddActivateChoice` (the
   fragment carrier), (ii) a `kAbility` entry whose spell has an MGEF with archetype `kScript` (`EffectArchetypes.h:11`) or
   with an attached VM script (`GetHandleForObject(MGEF type, mgef)` + `attachedScripts` lookup under
   `attachedScriptsLock`), or (iii) a `kQuest` entry (quest stage). Engine-side `AddPerk` is all-or-nothing, so until
   card C5 exists a perk with a script part is held back WHOLE and shown `Not copied: script part`. With C5 its EP
   entries are applied entry by entry (below) and only the script part is held back, which is exactly rule 4.
5. **Flag, not a verdict:** `ReadOwnerFacts.playerRefRunOn` / `playerRefParam` mean a condition reads the PLAYER's state
   (for example `PlayerRef.GetActorValue Lockpicking`). Copied as is, the follower would get a bonus sized by the player's
   skill (Q4). Census-count it in C1 before deciding.

---------------------------------------------------------------------------------------------------------------------

## (f) Engine-applied effects: application, removal, sweep, co-save impact

**Mode M1 (whole perk, card C2):** for each managed, MFO-enabled, persistable, non-commanded active follower and each P
classified `Copy`: `base->AddPerk(P, 0)` then `SetLivePerkRank(actor, P, 0, 1)`. Entries live on the base, so the engine
re-applies them at every `Load3D` and nothing has to be re-asserted.

**Ledger, runtime-only:** `inherit::g_held : FormID(base) -> set<FormID(perk)>`, main thread. The perk set is derivable
from the player at any time and the per-follower verdicts are recomputed, so the ledger records only what WE added, so
removal never touches a perk the follower had natively or one MFO granted (the catalog is disjoint by construction: no
inherited perk is a tree perk).

**Reconcile in PollWork (main):** want(actor) = classified-Copy set when the follower is active and enabled, else empty.
Diff it against `g_held` and add or remove. That one place covers dismiss, MFO-OFF, death, recruit, and player perk
gained or lost. `ForgetFollower` calls the remover first.

**Same-session reload sweep (the fix-forward half):** base edits survive a reload in memory, and the DLL's ledger
survives it too. At `kPreLoadGame` and `kNewGame` (`plugin.cpp:440`): for every ledger entry, `base->RemovePerk(P)` (no
live settle: the actors are being torn down and `Load3D` rebuilds from the base), then clear the ledger. **The ledger
must NOT be cleared by `ResetAllState` / `ClearAll` before that sweep runs**, or every reload into an older save leaves
the previous session's copies on bases forever. The first PollWork after the load re-adds for whoever is following in
the loaded save.

**Co-save impact: none in the first design, on purpose.** Base perk edits are not serialized (P3, measured), so a fresh
process loads clean bases, and the in-memory ledger mirrors exactly the in-memory edits. No PRGN field and no new record
is needed. **The one open risk** is ability entries: if an inherited ability's active effect were written into the .ess
with the follower, a later load without the perk would leave an orphan effect. MFO's P3 test (save/load x3 with an
ability perk, no doubling) points to "not persisted". Test T6 below proves it for inherited perks specifically. **If T6
fails,** the fallback is a persisted ledger (a new co-save record, tier A, readers forever) plus a load sweep that removes
the orphan effects. That would be its own card, not folded into C2.

**Mode M2 (entry level, card C5):** for a perk with a script part, apply only its EP entries straight into the live
arrays with the same presence-checked insert `SetLivePerkRank` uses (`ApplyPerkEntry` per entry), never touching the
base. Live arrays are rebuilt from the base at every `Load3D`, so the reconcile re-asserts them each poll with
`CountLive`. That is more engine-sensitive than M1 (live-only state), so it is a separate tier-A card, and only if the C1
census shows script-part perks matter.

**Shared bases:** `AddPerk` edits the TESNPC. A unique follower, or a leveled reference's own temporary base, is fine. A
base shared with other loaded references would hand the perk to all of them. Copy only when `base->IsUnique()` or the
base is the reference's own temporary leveled base. Otherwise mark `Not copied: shared base`. MFO's own `GrantRank` has
the same exposure today, so match whatever guard it uses.

**Threading:** everything is main thread (PollWork, load handlers, PublishBoardViews). Worker code (Scheduler, Fire)
never reads the inherit state. If it ever needs to, add a mirror.

---------------------------------------------------------------------------------------------------------------------

## (g) Cards

| Card | What | Tier (author / review) | Effort (1-5) |
|---|---|---|---|
| **C0** | Commit the EP census tooling to `tools/ep_census/` (ep_census.py + vf_census.py + arity.py + README + a selftest that the three runtimes reproduce the (a) table) and add the (a) table to `Docs/ENGINE_NOTES.md` | C (sonnet-chore) / mechanical + coordinator read | 1 |
| **C1** | DISCOVERY + CLASSIFICATION, read-only. New file `progression/Inherit.cpp` (+ small header): tree set at kDataLoaded, player-perk discovery on load + signature change, rules 0-4, constexpr EP class table, `[inherit]` census (per perk: verdict, EPs, playerRefRunOn, script parts, `hidden` flag). **Mutates nothing.** Needs `ConditionPlayerGated`/`ReadOwnerFacts`/`SameConditions` callable from the new file, but Progression.cpp is 1617 lines, so either duplicate nothing and propose the split first, or declare-only access (STOP-and-report per scope rule 1; coordinator decides) | B (sonnet-coder) / Opus one pass | 3 |
| **C2** | APPLICATION M1: PollWork reconcile, runtime ledger, kPreLoadGame/kNewGame sweep (ordered before any ClearAll), `ForgetFollower` hook, shared-base guard, INI `bInheritPlayerPerks` (default ON), `[inherit] COPIED/REMOVED` lines | **A** (Opus author / Opus tier-3 review: new engine-mutating mechanism, load-order-sensitive teardown) | 3 |
| **C3** | PROGRESSION QUERY CONTRACT: `Progression::FollowerEntryPoint(ep, follower, args, seed)` over `TradePerks::EntryPointValue`; route `Pricing` (8/60), `LockpickEntryPointsFor` (59/63/65, which also closes MFO-B324 for that caller) and `ErrandSmith` (76) through it, so other subsystems ASK progression. The values already include inherited perks after C2; this card makes the contract explicit | B / Opus one pass | 2 |
| **C4** | BOARD: `BoardFollowerView::inherited` + `PublishBoardViews` + an "Inherited from you" section in `Board_Progression.cpp`, strings, gamepad nav | B / Opus one pass | 2 |
| **C5** | (conditional on the C1 census) M2 entry-level application for script-part perks | A / Opus tier 3 | 4 |
| **C6a** | EP75 dual cast: `Loadout::OwnsDualCastPerk` asks progression `EP75(follower, spell)` (adds 75 to `ProvenCondArgs`, 1 arg, spell) instead of a fixed perk form | B / Opus one pass | 2 |
| **C6b** | EP9 on follower kills: a death sink adds `EP9(follower, victim, ...)`'s leveled list to the victim when the killer is a managed follower (2 cond args; crime pre-check per the "never commit detected crimes" rule) | B / Opus one pass | 3 |
| **C6c** | Steal gambit shapes: record EP56 (thief, target, item, &chance) and EP61 (2 args) in `ProvenCondArgs` when the Steal gambit lands (no code before that) | C (note in the Steal design) | 1 |
| **C6d** | Lockpick extras EP16 / 86 / 90: only if marth wants broken-lock picking, crime chance or key rewards for followers | B | 2 |
| **C7** | MAP.md §5 entry, INVARIANTS (the ledger sweep order), STATUS | C | 1 |

Order: C0 -> C1 (field census) -> C2 + C4 -> C3 -> C6a/b. C5 only on census evidence.

### In-game tests (Deck, stacked into one deploy)

- **T1 census (C1):** load a save; the `[inherit]` lines list the player's off-tree perks with verdicts. No skill-tree or
  beast-tree perk appears. Count playerRefRunOn and script-part perks.
- **T2 copy (C2):** with a follower recruited, `[inherit] COPIED` lines appear; console `HasPerk` on the follower returns
  1. A shop errand's `[trade]` price and the `[lockpick]` sweet-spot multiplier change by the inherited entry.
- **T3 dismiss:** dismiss him; `REMOVED` lines appear; `HasPerk` returns 0; prices and lockpick revert.
- **T4 reload no-dup:** save, reload twice (same session) with him following. `[perk-probe]` census reports no duplicated
  entries, and each inherited perk is added once.
- **T5 sweep:** in the same session, load an older save in which he was never recruited. `HasPerk` returns 0 on him (the
  kPreLoadGame sweep).
- **T6 ability persistence (the co-save gate):** with an inherited ABILITY perk on him, save, quit to desktop, set
  `bInheritPlayerPerks=0`, load. He must have no inherited ability effect. If he does, open the persisted-ledger card.
- **T7 board (C4):** the "Inherited from you" list shows Copied / Duplicate / Player only / script part / no follower use
  with reasons, and is navigable on the gamepad.
- Dev self-test gambit: add `prog.inherit_ledger_matches_base` (every ledger entry is on the base, and nothing on the base
  that the ledger says we removed) and reuse `prog.perk_entries_unique`.

---------------------------------------------------------------------------------------------------------------------

## Questions for marth (each with a recommended default)

- **Q1** Should inheritance be core for every managed follower, like HMS, or only for followers enrolled in the
  Progression add-on? *Default: core.* Pricing and lockpick are core, and the board list shows when the tab exists.
- **Q2** Should standing-stone perks (`standingStonePerks`) be excluded? They are the player's birthsign choice, not the
  list's hidden effects. *Default: exclude.*
- **Q3** Before C5 exists, should a perk with a script part be held back WHOLE, or copied whole, scripts included?
  *Default: hold back whole and show "not copied: script part".*
- **Q4** Conditions that read the PLAYER's values (run on PlayerRef) would give the follower a bonus sized by the
  player's skill. Should that be accepted for now and flagged in the board? *Default: accept, flag it, and decide after
  the C1 census count.*
- **Q5** Does duplicate identity include identical conditions (not only EP + function + value)? *Default: yes.* That is
  stricter, so it copies more, which fits "bias toward effect".
- **Q6** Is extra loot from EP9 on follower kills in scope? *Default: yes, as a later card (C6b).*
- **Q7** Should the lockpick extras EP16/86/90 be honored for followers? *Default: no for now.* MFO only picks non-crime
  locks.
- **Q8** EP22 (skill-use XP) cannot be evaluated for a follower: the skill is passed in a player field. Should it be
  dropped as "no follower activity"? *Default: drop.* MFO's progression is level-with-player.
