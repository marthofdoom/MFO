# Off-class storage: design and implementation plan

ClickUp 86e3t212u. Branch `feat/offclass-storage`. Written 2026-11-13 against `origin/main` 6a5c3c9f.

**State: IMPLEMENTED 2026-11-13 (tier A, co-save), fix round after review 2026-11-13, NOT merged.** The first version of
this document (sections 1 to 12 below) was an outline written before marth's rulings of 2026-11-12 and 2026-11-13. Those
rulings OVERRIDE it wherever they differ; section 0a says what was built and supersedes the plan of sections 3, 4 (the trip
parts), 5 (the layout), 7 and 8, and everything below about player homes, home learning, interim chests, migration and the
home stop. The code is `native/logistics/Storage.h` / `Storage.cpp`, `Papyrus::DispatchMoveItem`, the 'STOR' record in
`Serialization.h/.cpp`, the sale-loop hook in `Economy.cpp`, `Storage::Tick` in `Service.cpp`, the MFO storage chest in
`MFO_GenerateESP.py` / `Forms.h`, and `native/selftest/TestsStorage.cpp`.

## 0a. Rulings 2026-11-12 / 2026-11-13 (verbatim) and what was built

- Classes OFF: "No storing" (he sells as today).
- What is stored: "It only stores for previously mastered, or future in line. But Armor too."
- Access: "As I said they can access it from anywhere. But only for this purpose."
- Where (2026-11-12, SUPERSEDED the next day): "I would say the main player home is preferred, but other safe storage exists,
  id have the storage moved to the player home once one is bought."
- Where (2026-11-13, Q-S-13, the ruling in force): "Give the followers thier own mfo storage they can access."
- Economy decides what is worth keeping; retrieval on a class change and when a stored piece beats what he carries.

Built:
- **No errand stop, no ERND change.** The sale loop (`Economy.cpp`, before the weapon / armor `push_back`) asks
  `Storage::HoldFromSale`; a Store verdict holds the row (one copy) and records a want. `Storage::Tick` (the service tick's
  idle branch, worker, out of combat, ~2 s) moves it with `Papyrus::DispatchMoveItem` (follower -> chest,
  `ObjectReference.RemoveItem(item, 1, true, chest)`) wherever he is, and confirms it from HIS count within 30 s.
- **Planned** = (mastered classes | classes ahead of the equipped one through `parentMask` with the same T1 root) minus the
  equipped class. Weapon bits ignore unrestricted sets (T1, Dragonborn); armor kind per class: the mage line wears clothing
  (with bMageWearRobes on and dolls mode off), every other class rated armor; Dragonborn (no T1 root) adds no kind.
  Classes off / none equipped: no storing.
- **Buckets** (one stored piece each): dagger, one-hander, two-hander, bow, crossbow, staff, shield, and head / body / hands /
  feet for rated armor and for clothing. Store only when the piece outranks every other carried piece of its bucket and
  his stored one, he carries exactly ONE copy (a count drop could otherwise be a sale), and use value minus sale value
  (`Pricing::Apply` with `Pricing::Resolve(follower, none, 0)`) is at least `iErrandMinSellGold` (placeholder, Q-S-3).
- **The chest (Q-S-13).** ONE MFO-owned chest, generated in `MFO.esp` by `MFO_GenerateESP.py`: base CONT `0x917`
  `MFO_StorageChest` (no respawn flag), and ONE
  PERSISTENT REFR `0x919` `MFO_StorageChestRef` of the chest, placed in the VANILLA interior cell Skyrim.esm `001052FD`
  `LoadScreenTesting` (see "Where the chest lives" below; `0x918`, the first draft's new CELL, is RETIRED, never reused) (`Forms::kStorageChestRef`, resolved to
  `Forms::g_storageChest`). Persistent, so the ref stays in memory with its cell unloaded and a Papyrus RemoveItem reaches it
  from anywhere. Shared by every follower; what each follower stored is tracked per follower in 'STOR'. No home search, no
  interim chest, no migration (all removed). The chest missing (an older MFO.esp) = no storing, one error line.
  `tools/audit_esp.py` checks the three records.
- **Receiver binding (review F1).** `DispatchMethodCall2` resolves its receiver through a bound script object, and the chest
  has no script, so `Papyrus::DispatchMoveItem` first binds one through CommonLib's own `PackHandle` road (FindBoundObject,
  else CreateObject + BindID, fork `src/RE/P/PackUnpack.cpp`) for the receiver; the destination is bound by the same road when
  the arguments are packed. A store dispatch failure blocks that base for the session (it sells as before); a retrieval
  dispatch failure is a miss with the 5 minute backoff (one WARN per try, never every 5 s), and the entry stays.
- **Retrieval** (5 s cadence, one per pass, review F2): the piece fits his class now (weapon bit in his set or unrestricted;
  armor of the kind he wears), and the SALE PATH ITSELF would keep it were he carrying it: `ComputeKeepSet(..., a_extra)`
  with the piece added, plus the sell loop's own unworn-row gates (`SaleKeepGates` / `SaleKeepsUnworn` in `Economy.cpp`,
  one shared predicate, no mirror). Only when he carries no copy. Three confirm misses drop the entry loudly.
- **Confirms (review F5).** Every move is confirmed from the FOLLOWER's own count; the shared chest's inventory is never read.
  While a container / gift / barter menu is open no confirm reads his count, no window runs out and no move starts. One
  stored entry per base per follower (review F8: a second copy of a stored base sells, the stored one is never replaced).
  REMAINING LIMIT: the chest holds items, not owners. Two followers who store the same base each have their own entry, and a
  RemoveItem by base takes whichever copy the engine picks, so a retrieval can hand him the other follower's copy (a
  different temper / enchant instance of the same base). Counts stay right per follower; instance data can swap.
- **Load (review F4, co-save carve-out).** An entry saved mid-move (flag 0x01 store, 0x02 retrieval) becomes a settle op on
  the follower's first loaded tick of the session with a FRESH 30 s window, through the same confirm path; never judged on
  the first pass.
- **Evidence for the old NOT VERIFIED items.** (1) Persistent home chests: parsed Skyrim.esm / HearthFires.esm (tool
  `Linux-Native-Tools/tools/esp_query.py` plus a cell walker): the vanilla city houses (Breezehome, Vlindrel Hall, Proudspire,
  Honeyside) have NO persistent general container (PlayerHouseChest refs are temporary); Hjerim has a persistent wardrobe;
  Hearthfire houses have many. So vanilla city houses do not qualify (Q-S-13, ANSWERED 2026-11-13: MFO's own chest, above). (2) VM RemoveItem into
  an unloaded persistent container: the standard Papyrus idiom (holding-cell containers), and MFO_Trade's merchant-chest
  moves are the same shape; every move here is confirmed from the follower's count and a miss is a WARN, so the first field
  run proves or refutes it.

'STOR' v1 as built (supersedes section 5's block and the 2026-11-13 morning draft with a homes table, which never
shipped; the reader rules of section 5 stand):

```
u32 followerCount <= 4096 (more = aborted)
per follower: u32 follower (unresolvable -> his items read past and dropped), u32 itemCount <= 16
  per item:   u32 base (WEAP/ARMO; unresolvable / other type / duplicate base -> dropped), u16 count (1; 0 dropped, >1 clamped),
              u8 flags (0x01 a store in flight at the save, 0x02 a retrieval in flight; others masked)
```
Version 0 skipped, a newer version skipped with the "newer save" warning, a short read keeps what was parsed.

**Where the chest lives (review r2 SEV-3 fix, marth 2026-11-13: "1.5 is supported", "As is 1.7").** MFO.esp is ESL-flagged, and
a NEW CELL in an ESL loses its refs' data on reload on the pre-1.6.1130 runtimes (the "ESL CELL load bug"; fixed in 1.6.1130+).
1.5.97, 1.6.x and 1.7.104 are all supported, so MFO.esp defines NO new CELL. The REFR `0x919` goes into an existing vanilla
interior cell and MFO.esp carries that CELL as an IDENTICAL override plus the one new persistent child. The placement holds on
1.5.97, 1.6.x and 1.7.104 alike (a plain master-cell override with a persistent child, nothing runtime specific).
Choice: Skyrim.esm CELL `001052FD`, EDID `LoadScreenTesting`. Evidence (`Linux-Native-Tools/tools/esp_query.py` helpers over the
Tuxborn `Game Root/Data/Skyrim.esm`, 2026-11-13): interior (DATA `0100`, bit 0x0001), record flags 0 (not compressed), subrecords
only EDID / DATA / XCLL (92 bytes) / LTMP / XCLW (no FULL, no XLCN location, no XOWN owner, no XCIM); the only FormID references to it
in Skyrim.esm are its own record and its two child-group labels (so no door, teleport, quest or script targets it); the other
`Data/` masters carry none. A cell the game never loads, so no one meets the chest (considered and rejected: ThievesGuildHoldingCell
`000C0BBA`, which has a location and a quest using it). Its vanilla children (11 REFRs in a temporary-children group) are not
repeated. Structure written to `out/MFO.esp`: `GRUP CELL > GRUP type 2 (label 1) > GRUP type 3 (label 2) > CELL 001052FD (flags 0,
EDID/DATA/XCLL/LTMP/XCLW copied byte for byte) > GRUP type 6 (label 001052FD) > GRUP type 8 (label 001052FD) > REFR 01000919 (flags
0x400, NAME 01000917)`; vanilla nests the cell block 1 > sub-block 2, the same. `tools/audit_esp.py` checks: no CELL other than
that override, the override identical in shape, REFR 0x919 persistent. Residual: another mod overriding the same cell later
in the load order would win the CELL record only, the persistent child list still merges (children are additive).

Superseded and removed pieces (fix round after review r2, 665c3f46): the chest is unreachable by the player, so nothing is ever
"released to the player" in it. A superseded piece is retrieved back to the follower (in-memory `g_evict` mark, the entry stays
until the retrieval confirms; a save between the mark and the confirm just leaves an ordinary stored entry). On roster removal
(`Forget`) each stored piece is dispatched from the chest to him if he still resolves, else to the player, one log line per
piece, no confirm tracking. KNOWN LIMIT: a save loaded by an older DLL without STOR loses track of the stored pieces (they stay
in the chest, untracked and unreachable).

## 0. The rulings this implements

- marth 2026-10-18: "Id recommend a follower keep a tracked set expensive off class items that they store
  in the player home, with no player home, they dont store." Today off-role weapons are sold.
- marth 2026-11-12:
  - Kept = the economy decides (use value later beats sale value now), derived from Pricing / Economy, no
    hand threshold. An unavoidable constant is labelled a placeholder for the sim logistics world.
  - "Followers can access the storage from anywhere. But it's only for equipment they plan to use."
  - Container: owned, safe (non-respawning), in a cell the player owns, picked at runtime, the same chest
    every time once chosen.
  - Retrieval triggers: class change, and a stored piece better than what he carries for his class.
  - Storing: a stop on the town errand itinerary, walked or simulated. No player home = no storing.
  - The tracked set is saved. Readers for every shipped version forever (#12).
  - Equip authority, PGIV and the never-sell rules keep working. A stored piece is never sold.

## 1. The chest (runtime pick, no list data)

**What counts as the player's home.** The existing in-tree definition, `RefInPlayerStorage`
(`logistics/LootTake.cpp:611`, READ): a location chain carrying `LocTypePlayerHouse` (Skyrim.esm 000FC1A3)
or a cell whose owner is the Player (00000007) or `PlayerFaction` (00000DB1). Storage uses the stricter
half for the pick: the chest's CELL owner must be the player or PlayerFaction, or the chest ref's own owner
must be. Base-game forms only.

**When it is learned.** Only while the cell is loaded, because the pick walks the cell's references:
`Storage::LearnHomeOnMain` (MAIN thread), posted by `Storage::NoteWorkerTick` when the player's cell
changes. So a home is unknown until the player has stood in it once on this save with the feature on. That
is the "no player home = no storing" state and it is logged once.

**Which container.** All of:
- `IsPersistent()` (fork `RE/T/TESObjectREFR.h:453`, flag `kPersistent = 1 << 10` at `:181`, READ). Needed
  because retrieval and the off-screen store address the chest while its cell is unloaded.
- base is `TESObjectCONT` without `kRespawn` (fork `RE/T/TESObjectCONT.h:22`, READ the enum; the author
  confirms the member path `data.flags` in that header).
- not locked, not disabled, not deleted.
- not `IsLOTDDropOff` (`LootTake.cpp:643`), not `IsFactionServiceContainer` (`LootTake.cpp:680`).
- ref owner null, the player or PlayerFaction.

Among those: the one with the fewest item stacks (stay out of the chest the player organised), lowest
FormID on a tie. Once chosen for a cell it is never re-picked while it still resolves and still passes the
tests. Recorded as `Home{chest, cell, town}` with `town = LocationTypes::TownAnchor` (`LocationTypes.h:92`)
of the chest's location chain, 0 when there is none.

NOT VERIFIED:
- that vanilla and common player-home containers are persistent refs. If the first field read shows homes
  with no persistent container, see open question Q-S-4.
- that a persistent container in an unloaded interior accepts a VM `RemoveItem` into it and out of it. The
  precedent is the merchant chest of the off-screen trade (`TradeBridge` / `ErrandSim.cpp DispatchTrade`),
  which is the same shape. A passive probe line on the first off-screen store is the proof.

## 2. "Plans to use"

Scope of v1: **weapons and shields**, the vocabulary the class data itself has (`Classes::Weapon` bits,
`classes/Classes.h:68`). Armor is open question Q-S-2.

The piece's class bit is `ClassWeaponBit` (`logistics/EquipAuthority.cpp:351`, READ; today in an anonymous
namespace, step 3 of the plan lifts it into `Logistics_internal.h` unchanged) and `Weapon::kShield` for a
shield.

**Classes ON for him** (`Classes::Active()` and `GetEquipped(fid) < kClassCount`):
- in his equipped class's set now -> not storage. It reached the sale path because a better one of its
  kind is kept. It sells as today.
- else it is planned when some OTHER class `c` has the bit in a RESTRICTED set (not `kUnrestricted`: T1
  and Dragonborn fit everything and would make every item "planned") and `c` is one he can take or is
  next on his tree: `Bit(c) & (GetUnlocked | GetMastered)`, or `Get(c).parentMask & Bit(equipped)`.

**Classes OFF** (open question Q-S-1, default implemented by the template's contract): planned when his own
perks are invested in the piece's skill tree, `StyleVotes::skillRanks[skill - kOneHanded] > 0`
(`Progression.h:434`, through the `StyleVotesFor` mirror, and only once `StyleVotesReady`). One-handed
types -> One-Handed, two-handed -> Two-Handed, bow and crossbow -> Archery, shield -> Block, staff -> none.
This is the follower with an explicit Melee pick and Archery perks: `ComputeWeaponRoles` (`Gear.cpp:459`)
never adds an off-class role from perks to an explicit pick, so his bow sells today.

**Top of its bucket.** Buckets are the keep set's (`SwapUp.cpp` `bucketOf` inside `ComputeKeepSet`: 1H, 2H,
bow, crossbow, staff) plus shield. Ranks are the keep set's too: `WeaponScore` for melee, attack damage for
launchers, gold value for staves, armor rating for shields. A piece is stored only when it outranks every
other carried piece of its bucket and the piece he already has stored in that bucket (tie: higher value,
then lower FormID). So he never stores a duplicate or an inferior, and the tracked set is at most one piece
per bucket per follower. When a better piece of a bucket is stored, the older entry is SUPERSEDED: it is brought back to HIM from the
chest through the ordinary retrieval dispatch + confirm (unconditionally, whatever his class), and the sale path then judges it.
The chest is unreachable by the player, so a piece must never be left in it untracked.

## 3. The keep test and the store stop

**The judge of "would be sold today" is the sale path itself.** The hook sits where a weapon or armor row
becomes a sell candidate, `logistics/Economy.cpp:910` (`sellCandidates.push_back`, READ). Every protection
above it has already run: museum hold, smithing material, stock gear (`IsStockGear`), `IsPlayerPick`, the
keep set, worn, the gem hold, `Catalog::IsExcluded`. So storage never sees stock gear, a player pick, a
relic, a quest item, a socketed piece, a worn piece or his never-disarm floor weapon, and it needs no copy
of those rules. `Storage::BeginSaleScan(fid)` goes before the loop at `Economy.cpp:723`.

**The comparison (no new constant).**
- sale value now = `Pricing::Apply(value, quote.sell)`
- use value later = `Pricing::Apply(value, quote.buy)`: what he would pay a vendor for it when he takes
  the class. `quote = Pricing::Resolve(handle, {}, 0)` (`Pricing.cpp`, READ: with no vendor it returns the
  cached or Speech-only factors and never blocks).
- store when `useValue - saleValue >= iErrandMinSellGold` (`Config::g_errandMinSellGold`, default 100, the
  existing "this much gold makes a stop worth walking" knob).

With `bSpeechPricing` off both factors are 1.0, the margin is 0 and nothing is stored: selling and buying
back is then free, so that is the economically right answer and it is logged once. The comparison is
derived, not tuned. It is not very selective (at Speech 15 it stores a planned piece worth about 36 gold
or more). Selectivity is the lever the sim logistics world should own: open question Q-S-3.

**Hold only when the stop can happen.** `Storage::Prepare(follower, playerTown)` runs in `Errand::TryStart`
before the plan probe (beside `Smith::Prepare`, `Errand.cpp:1913`). It answers Ready only when a Home's
`town` is the player's town and the chest is reachable now:
- the follower is in the chest's cell -> the stop walks to the chest ref (`DestKind::Ref`);
- else `Rest::ShopDoorFor(cell, pos, &lockedId, &level)` (`ErrandRest.cpp:1475`, the shop catalog's
  main-thread door read, cached): Found -> `DestKind::Cell` with the door position, Pending -> the plan
  waits one cadence (the `doorWait` shape at `Errand.cpp:1904`), Locked or None -> no storing this visit.
  A locked door is never walked (marth 2026-11-02, never a locked door in town).
`HoldFromSale` holds a piece only when Prepare said Ready. Legacy proximity trading never calls Prepare, so
it sells exactly as today.

**The stop.** `StopKind::Home`, modelled on the Smith stop, which is the "own file, own phase" precedent:
- planner: `Storage::Augment` right after `Smith::Augment` (`Errand.cpp:1999`), before `Rest::Augment`. It
  appends the stop when his want list (filled by this plan's sale scan) is not empty. Room rule
  `a_stops.size() + 3 <= kMaxTripStops`, the Smith rule, so the temple and the inn keep their room and the
  ERND bound of 8 stops holds. A home-only trip is a trip.
- physical: `Phase::Storing` in `Errand.cpp`, every `Phase::Smithing` site mirrored (list in section 8).
  `Storage::Begin / Step / End`: walk to the chest, the give idle (presentation only), the move, the confirm.
- off screen: the stop is a `Stage::Trade` of the simulation like the smith stop; at its end
  `Storage::SimFinish` (`ErrandSim.cpp:1109` is the smith call) dispatches the moves from the PLANNED list
  and flags the entries `kUnverified`. The common case is off screen: the player stands in the street, the
  follower goes through the door and unloads.
- failure: three store stops in a row that end without a piece landing block storing for that follower
  and chest for the session, loudly. The pieces then sell as today.

**The move.** A new `Papyrus::DispatchMoveItem(from, item, count, to)`: `ObjectReference.RemoveItem(akItem,
aiCount, abSilent = true, akOtherContainer = to)`, the sibling of `DispatchRemoveItem` (`Papyrus.cpp:148`,
READ, MISC only today). The item is packed by its concrete type (WEAP or ARMO), as that function's comment
explains. It is asynchronous: "dispatched" is never "happened", every move is confirmed from his inventory
count on a later tick.

## 4. Retrieval (from anywhere, no trip)

`Storage::RetrieveTick` from the idle branch of `ServiceFollower`, own cadence (about 5 s), follower loaded,
out of combat. One rule covers both triggers:

> a stored piece comes back when it fits his class now (his set is unrestricted or holds the bit), his
> weapon roles keep its bucket (`ComputeWeaponRoles`; a shield: off hand not a weapon), and it outranks
> the best piece of that bucket he carries.

A class change makes the first two true, losing or selling gear makes the third true. The line says "class
change" when his equipped class differs from the last pass. No hook in `Classes::SwitchClass` is needed.
After the piece lands the existing keep set and equip authority own it: it is the best of an in-role
bucket, so it is kept, declared and equipped by today's code. Storage never equips anything.

Confirm: his count of the base rose -> the entry is dropped, `RETRIEVED`. Not within the window (30 s, a
floor well above VM latency) -> WARN and a 5 minute backoff. The third miss drops the entry loudly: the
piece is not in the chest any more (the player took it). Nothing is masked.

## 5. Co-save: a NEW record `'STOR'` v1

**Decision: a new independent record, not an extension of FLWR, ERND or PGIV.**
- its lifetime is its own (pieces outlive errands and are unrelated to a gambit row);
- a FLWR bump is the highest blast radius in the tree and drags MCLS's row codec along (`GambitWire`);
- ERND is "errands in progress" and is written only while one exists;
- PGIV is relic-only by ruling and its entry is `{base, bits}` with no room for a chest;
- an older DLL meets an unknown record type, skips it (`Serialization.cpp:1109`, READ) and drops it on its
  next save. Nothing desyncs. The pieces stay in the chest and are simply the player's. That downgrade
  result is acceptable and is the documented one.

**ERND** gains only the stop-kind opcode string `"home"` (value space, the R7 / R8 precedent: `"temple"`,
`"inn"`, `"smith"` were added the same way, no layout change, no version bump). An older reader meets it as
an unknown kind: a later stop loads SKIPPED, the current stop DROPS the entry, both logged, as documented
at `Serialization.h:211`.

**Layout, `kRecStorage = 'STOR'`, `kStorageVersion = 1`.** Little-endian, written in this order:

```
u32 homeCount                      <= 16   (more = the record is aborted)
per home:
    u32 chest                      REFR FormID
    u32 cell                       CELL FormID
    u32 town                       BGSLocation FormID, 0 = none
u32 followerCount                  <= 4096
per follower:
    u32 follower                   Actor FormID
    u32 itemCount                  <= 8
    per item:
        u32 chest                  REFR FormID, must name a home that loaded
        u32 base                   WEAP / ARMO FormID
        u16 count                  1..1 today; 0 = the item is dropped; clamped to 1 (#11)
        u8  flags                  0x01 unverified; other bits masked off on read (#11)
```

Rules (the PGIV codec at `logistics/PlayerGiven.cpp:201-290` is the model, READ):
- written ONLY when a home or an item exists. A save without the record loads with nothing.
- snapshot under the leaf mutex, write from the copy.
- writer skips any 0xFF id (`Followers::IsPersistableID`, #9).
- reader: `ResolveFormID` on every id (#8). Unresolvable chest -> that home and its items are dropped.
  Unresolvable cell or town -> 0 (the chest still serves retrieval; storing needs a re-learn). Unresolvable
  follower -> his items are read past and dropped. Unresolvable base or `LookupByID` null -> the item is
  dropped. An item whose chest is not a loaded home -> dropped. All counted in one `[cosave]` line.
- a short read or an implausible count aborts THIS record only; what was parsed is kept.
- version guard in `LoadCallback`: newer than this DLL -> skipped with the standard "WILL BE DESTROYED"
  line and `g_sawNewerSave`; version 0 -> skipped (the MFO-B309 rule).
- v1 is the only version. A later version appends and gates on `version >= N`. The v1 reader stays forever.

## 6. Threading

| Thread | What it touches |
|---|---|
| AddTask job worker | `Prepare`, `BeginSaleScan`, `HoldFromSale`, `Augment`, `Begin / Step / End`, `SimFinish`, `RetrieveTick`, `Forget`. Reads the follower's inventory only while he is loaded (as the sale path does). Reads `g_followers` only through the `FollowerState` the caller passes. |
| MAIN (`MainThread::Post`) | `LearnHomeOnMain` (walks a loaded cell's references), `Rest::ShopDoorFor`'s own read. On VR `Post` is a no-op: no home is ever learned, storing is off, logged once. |
| Papyrus VM | the item moves (`DispatchMoveItem`). Asynchronous. |
| SKSE save / load callback | `CoSave` (pump paused), `CoLoad` (pump stopped). They take the leaf mutex. |
| revert | `ClearRecord` in `ResetAllState` after `StopPump` (its own call beside `PlayerGiven::ClearRecord`, `Serialization.cpp:1208`), `ClearTransientState` from `Logistics::ClearTransientState`. |
| combat thread | nothing. No hook reads this module. |

One leaf mutex for the table (homes, stored, want lists, work records). Never held across an engine,
Harbinger, Papyrus or `WriteRecordData` call, never nested with the Errand or Sim mutex.

## 7. Log lines (all passive, tag `[storage]`)

```
[storage] home chest CHOSEN: {chest} '{name}' in cell {cell} (town {town} '{name}'), {n} candidate(s), fewest stacks {k}
[storage] no usable chest in the player's cell {cell}: {n} container(s), {why counts: not persistent / respawns / locked / owned by another / service}
[storage] {fid} '{name}': HOLD '{item}' for the home stop -- planned for {class | perks in <skill>}, top of its {bucket}, use {u}g vs sale {s}g (margin {m}g >= {min}g)
[storage] {fid} '{name}': '{item}' sells -- {why}                       (rate-limited with the [sell] diag, 15 s per follower)
[storage] {fid} '{name}': no home stop this visit -- {door locked ({word}) | no door to cell {cell} | the trip is full | storing blocked after 3 failed stops}
[storage] {fid} '{name}': home stop {k} at chest {chest}: {n} piece(s)
[storage] {fid} '{name}': STORED '{item}' in {chest} ({on screen | off screen, unverified}){; '{old}' superseded -- it comes back to him}
[storage] {fid} '{name}': '{item}' did NOT leave his pack within {s} s -- not recorded      (WARN)
[storage] {fid} '{name}': off-screen store of '{item}' CONFIRMED | NOT confirmed (he still carries it) -- entry dropped (WARN)
[storage] {fid} '{name}': RETRIEVE '{item}' from {chest} -- {class change | better than what he carries} (rank {r} vs {c})
[storage] {fid} '{name}': RETRIEVED '{item}' | '{item}' did not arrive in {s} s (try {k}/3) | '{item}' is no longer in the chest -- entry dropped (WARN)
[storage] {fid}: roster removal -- '{item}' from the MFO chest to him | the player ({dispatched | dispatch FAILED})   (one per piece)
[cosave] saved {h} home chest(s), {n} stored piece(s) for {f} follower(s), schema v1
[cosave] loaded {h} home chest(s), {n} stored piece(s); dropped {a} home(s), {b} follower(s), {c} piece(s)
```

## 8. Ordered implementation plan (seats at 6a5c3c9f)

Tier A (co-save). One author end to end, Opus tier-3 review. Each step builds on its own.

1. **Config.** `bOffClassStorage` (default ON): `Config.h` near `g_errandSmith` (`:957`), `Config.cpp:177`
   (reader) and `:553` (default), `Docs/INI-REFERENCE.md` (near `:143`), `out/SKSE/Plugins/MFO.ini`.
   INI-only, no MCM key.
2. **Pure decision.** Fill `DecideStore` / `DecideRetrieve` in `Storage.cpp`. No engine read.
3. **Shared class bit.** Move `ClassWeaponBit` and `ClassSet` from the anonymous namespace at
   `logistics/EquipAuthority.cpp:351-369` to `Logistics_internal.h` as inline functions, text unchanged.
   Name this move in the brief (scope rule 1).
4. **Papyrus.** `DispatchMoveItem` in `Papyrus.h` / `Papyrus.cpp` beside `DispatchRemoveItem` (`:148`).
5. **Co-save.** `Serialization.h` after ERND (`:225`): the constants and the layout comment from section 5.
   `Serialization.cpp`: include (`:34`), `Storage::CoSave` after `WriteErrandRecord` (`:723`),
   `Storage::ClearRecord` at the load start (`:1000`), the `kRecStorage` dispatch branch after the ERND one
   (`:1076-1090`), `Storage::ClearRecord` in `ResetAllState` (`:1208`). `Followers.cpp:634`: `Storage::Forget`.
   ERND opcode: `SavedStopKind::Home` (`Errand.h:79`), `{ ErrStopKind::Home, "home" }`
   (`Serialization.cpp:243`), `ToSaved` / `FromSaved` (`ErrandTrip.cpp:138`, `:152`).
6. **Home learning.** `LearnHomeOnMain`, `NoteWorkerTick` called from the top of `ServiceFollower`
   (`Service.cpp`, once per lap is enough) or the pump lap in `Diagnostics.cpp` beside `Errand::Sweep`.
7. **Sale hook.** `Economy.cpp:723` `BeginSaleScan`; `:910` before the `push_back`:
   `hold = Storage::HoldFromSale(...)`; `hold >= count` -> `sdiag(obj, "storage")` and `continue`, else the
   row's count drops by `hold`. Weapons and shields only.
8. **Planner.** `StopKind::Home` appended (`Errand_internal.h:151`). `Storage::Prepare` at `Errand.cpp:1913`,
   `Storage::Augment` at `:1999` (declared in `Errand_internal.h` beside the Smith block at `:575`, since it
   takes `std::vector<Stop>&`).
9. **Physical stop.** `Phase::Storing` appended (`Errand.cpp:111`) and every `Phase::Smithing` site
   mirrored: `PhaseName` `:121`, `Finish` `:863`, `Describe` `:1041 / :1049`, `SnapshotForSave` `:1098`
   (saved as `PhysTaking`, like the smith stop: a restored home stop is not resumed), the step block
   `:2316`, the arrival dispatch `:2532`, the switch arm `:2900`, the hand-off `:2995`. Also the label at
   `:1351` and `ClearTransientState` `:1076`.
10. **Off-screen stop.** `ErrandSim.cpp`: `WorkDays` `:179`, `Describe` `:438`, the toast branch `:1053`
    (no toast, or a new i18n key: ask), `SimFinish` at `:1109`. Check the kind tests at `:103`, `:239`,
    `:617`, `:632`, `:655`: a Home stop has no order and must not be skipped as an order-less SHOP (it is
    not one), and `Rest::InStopCell` (`ErrandRest.cpp:1335`) answers false for it today.
11. **Retrieval.** `RetrieveTick` from the idle branch of `ServiceFollower` near `Service.cpp:2289`.
12. **Self-test** (section 9), **docs**: MAP.md (new `logistics/Storage` entry with "What breaks", the
    co-save section's record list, the Errand entry's stop kinds, CLAUDE.md "five things" record count),
    `Docs/STATUS.md`, `Docs/INVARIANTS.md` if a rule is added, `Docs/SELFTEST.md`.
13. Builds: release and DEV (`MFO_DEV_SELFTEST=ON`) green on the final SHA.

Backlog to read before editing those files: MFO-B349 to B358 (R7 stops), B360 to B363 (R6), B130 to B132
(PGIV), B309 (ERND v0). `ErrandRest.cpp` (1615 lines), `Service.cpp` (2294) and `Errand.cpp` (3020) are
past the 1500-line plan-a-split mark: the storage mechanism stays in `Storage.cpp`, only call lines go in.

## 9. Self-test cases (`native/selftest/TestsStorage.cpp`, the TestsT2.cpp style)

All quick, no spawn. `RegisterStorage()` declared in `SelfTest_internal.h:139` and called from
`RegisterAll` (`Tests.cpp:654`). Ids are frozen once a dev build ships them.

- `storage.decide_store` (pure, table-driven over `DecideStore`): no home -> Sell. In his class set -> Sell.
  Classes on, bit in a planned set, top of bucket, margin clears -> Store. Bit in no planned set -> Sell.
  Unrestricted-only candidates (T1, Dragonborn) -> Sell. Outranked by a carried piece -> Sell. Equal rank
  and `tieLoses` -> Sell. Margin under `minMargin` -> Sell. Factors 1.0 / 1.0 -> Sell. Classes off with
  perk ranks 0 -> Sell, with ranks > 0 -> Store.
- `storage.decide_retrieve` (pure): off-class now -> false. In class, role keeps it, outranks carried ->
  true. Carried equal or better -> false. Role does not keep the bucket -> false. `classChanged` only
  changes the reason text.
- `storage.class_sets` (needs the class add-on, the `ClassPre` precondition): for a snapshotted follower,
  equip a T2 class and check that the planned-set union excludes unrestricted classes and includes the
  classes whose `parentMask` names it. Class state restored by the runner (`ClassSnapFix`).
- `storage.record_roundtrip`: `CoSave` into a `CoSaveBuffer` (`Serialization.h`, the MCLS self-test's
  tool) and `CoLoad` back: identical homes and items; a 0xFF id is not written; a flags byte of 0xFF reads
  as 0x01; an item count of 9 aborts the record and keeps what was parsed.
- `storage.home_pick` (INFO, never FAIL): the homes known and, when the player stands in an owned cell,
  the candidate counts by refusal reason.
- MANUAL items: a walked store with the player in the home; an off-screen store confirmed on the next
  loaded tick; a retrieval after a class switch; the board text.

## 10. Never-sell, PGIV and equip authority

- A stored piece is in the chest, not in his pack, so no sale path can reach it. Between the decision and
  the stop it is held by `HoldFromSale`. A superseded piece is brought back to him by the retrieval path;
  the sale path then judges it like any carried piece (it is not top of its bucket, so it sells).
- PGIV is untouched. A player-given relic never reaches the hook (`Lotd::HoldFromSale` at the top of the
  sell loop, `Economy.cpp:728`). Stock gear and `IsPlayerPick` are tested above the hook as well.
- Equip authority is untouched. Storage moves only unworn pieces at `Begin` (live re-check). Off screen it
  cannot re-check: the piece was a sale candidate at the plan, which already excludes worn pieces and his
  never-disarm floor weapon. NOT VERIFIED: that a VM `RemoveItem` of a base he has since equipped cannot
  happen in the short window between plan and off-screen store. Reviewer spot 3.
- G16 (off-class fallback, `EquipAuthority.cpp:413`): a follower with no weapon of his class keeps his best
  off-class weapon through the keep set's floor, so it is never a sale candidate and never stored.
- Crimes: the only container touched is in a cell the player owns. The walk never opens a locked door.

## 11. Open questions (defaults in brackets are what the plan above assumes)

Also filed in `open-questions-for-marth.md`.

- **Q-S-1. "Plans to use" with classes OFF.** [His own perk ranks in the piece's skill tree.] Alternative:
  no storing at all without the class add-on.
- **Q-S-2. Armor.** [Not stored in v1: weapons and shields only. The class data has no armor sets.] The
  real case is robes against armor when he moves between the mage line and the others.
- **Q-S-3. How selective is "expensive".** [Use value minus sale value at least `iErrandMinSellGold`,
  100.] A placeholder for the sim logistics world, which should say how likely a planned class is taken.
- **Q-S-4. A home with no persistent container.** [No storing there, one log line.] Alternative: store
  only while the player is inside, which breaks "access from anywhere".
- **Q-S-5. Home in another town.** [He stores only in the town the home is in and sells elsewhere as
  today, so he never hoards across the map.] Alternative: carry planned pieces until the next home visit.
- **Q-S-6. Roster removal.** [His entries are dropped and the pieces stay in the chest as the player's,
  matching what FLWR / MSTK / PGIV do.]
- **Q-S-7. Several homes.** [One chest per owned cell, up to 16. A piece is retrieved from whichever chest
  holds it.]

## 12. Least-sure spots for the tier-3 reviewer (of the eventual implementation)

1. The persistent-chest assumption and the VM move into and out of an unloaded container (section 1).
2. The off-screen store records an entry before anything proves the move (`kUnverified`), and the confirm
   reads "he no longer carries the copy", which a sale of another copy could fake.
3. The window between the plan's sale scan and the off-screen move: a piece equipped in between.
4. The Home stop inside `ErrandSim.cpp`'s kind tests (step 10): each `!= StopKind::Shop` branch was
   written for the temple and the inn.
5. `Pricing::Resolve` with vendor key 0 posts a perk refresh with no target. It is harmless by reading,
   but it adds a cache key the pricing code never had.
