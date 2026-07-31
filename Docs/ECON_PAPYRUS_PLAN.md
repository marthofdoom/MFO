# ECON_PAPYRUS_PLAN — the Papyrus merchant read + follower trade (#21)

Status: PLAN (no code). Produced 2026-07-31. Supersedes the disabled native
econ probe (`native/Logistics.cpp` `EconomyProbe`, v0.8.31) which CTD'd on the
merchant-chest read on every thread. This document is the crash-free redesign:
the merchant read and the buy/sell transaction move to **Papyrus** (the path the
game's own barter menu uses); native keeps the **decision** (catalog, gambit
quotas, quality ranking).

> The installed-modlist survey is complete (§A): Papyrus compiler availability,
> exact PapyrusUtil/po3 + vanilla signatures, MCM import paths, and the shipped
> prior-art mod (C.O.I.N.) are all confirmed and folded into the plan below.

---

## 0. TL;DR for marth

1. **MFO's native→Papyrus dispatch already exists and works** — `native/Papyrus.cpp`
   `DispatchMethodCall2`. But it is **fire-and-forget**: it passes arguments,
   it does **not** read return values (the `IStackCallbackFunctor` callback is
   always left empty). It targets **vanilla** natives by class-name against a
   form handle with no bound script. It can also target a **custom** script's
   function if that script is attached to the handle's form.
2. **MFO ships NO Papyrus source and has NO compiler step.** The one `.pex` it
   ships (`out/Scripts/MFO_MCM.pex`, an `MCM_ConfigBase` subclass) is a
   **prebuilt binary committed to the repo** — there is no `.psc` for it in the
   tree and no compile in CI/`release.sh`. A real `.psc → .pex` pipeline is new
   work this feature must establish.
3. **The data channel back to native is NOT the dispatch callback** — it is
   **MFO-registered Papyrus native functions** (`SKSE::GetPapyrusInterface()->
   Register`), which return values to Papyrus **synchronously**. MFO registers
   none today; this is also new work.
4. **Chosen architecture (one round trip):** native computes the whole
   decision it *can* from safe reads (the follower's own inventory, the item
   catalog, the gambit quotas, quality ranking), packages it as a **TradeOrder**
   keyed by a token, fills the follower into a **new ReferenceAlias**, and
   **dispatches one call** to a custom script `MFO_Trade.psc` on that alias. The
   script **pulls** the order via MFO natives, does the **merchant-safe read**
   (chest gold + per-candidate stock counts) and the **transaction**
   (`RemoveItem`/`AddItem`), then **reports results** via an MFO native. Native
   owns the *what*; Papyrus owns the *merchant setup + read + mutation*.

---

## 1. Research findings

### 1.1 MFO's current native→Papyrus dispatch machinery

Source: `native/Papyrus.h`, `native/Papyrus.cpp`.

- **What it dispatches:** `vm->DispatchMethodCall2(handle, "Actor",
  "DoCombatSpellApply", args, callback)` (`Papyrus.cpp:70`) and
  `vm->DispatchMethodCall2(handle, "ObjectReference", "Activate", args,
  callback)` (`Papyrus.cpp:104`). These are **vanilla Papyrus natives**,
  reached **by class name against the form's VM handle** — no bound script
  instance required, because Actor.psc / ObjectReference.psc natives resolve by
  class against the handle (`Papyrus.cpp:68-69`, `102-103`).
- **Handle resolution:** `HandleFor()` (`Papyrus.cpp:22-32`) maps a `TESForm*`
  to its `RE::VMHandle` via `vm->GetObjectHandlePolicy()->GetHandleForObject(
  formType, form)`, guarding against the non-zero `EmptyHandle()` sentinel.
- **Answers to the four sub-questions:**
  - **(a) Call arbitrary Papyrus functions?** Yes — any function reachable by
    `(handle, className, methodName)`. Vanilla natives need no bound script;
    a **custom script's** function needs the script **attached to the form**
    whose handle is used. `DispatchStaticCall(className, method, …)` (same VM,
    not currently used by MFO) reaches global/native functions with no handle.
  - **(b) Pass arguments?** Yes — `RE::MakeFunctionArguments(std::move(a), …)`
    (`Papyrus.cpp:63-64`, `98-99`). Already packs a `SpellItem*`+`REFR*` and a
    `REFR*`+`bool`. It packs ints/floats/forms and can pack arrays (`VMArray`)
    too. **Ownership trap already documented:** `MakeFunctionArguments` returns a
    raw `new`; MFO wraps it in `unique_ptr` to avoid a per-call leak
    (`Papyrus.cpp:60-64`).
  - **(c) Receive RETURN VALUES?** **Not as used.** The
    `BSTSmartPointer<IStackCallbackFunctor> callback` is default-constructed
    (empty) at both call sites (`Papyrus.cpp:65`, `100`) — the call is
    **fire-and-forget**. CommonLibSSE-NG *can* return a value, but only by
    subclassing `IStackCallbackFunctor` and handling `operator()(Variable
    a_result)` **asynchronously** (the value lands a frame or more later, on the
    VM's schedule). The header states the whole module's cost honestly:
    "dispatch is ASYNCHRONOUS … MFO can never treat 'dispatched' as 'happened'"
    (`Papyrus.h:31-35`). **Conclusion: do not build the trade on the dispatch
    return path.** Use registered natives (§1.3) for the data channel — they are
    synchronous on the Papyrus side.
  - **(d) Run callbacks?** Only via that same async `IStackCallbackFunctor`.
    MFO has never wired one.
- **Counters:** `Dispatches()`/`Failures()` for the heartbeat (`Papyrus.h:63-64`),
  cleared on revert.

### 1.2 Does MFO have any Papyrus source / a build+ship pipeline?

**No `.psc` in the repo; one prebuilt `.pex`; no compiler step.**

- The only Papyrus artifact is `out/Scripts/MFO_MCM.pex` (found by `find`), an
  `MCM_ConfigBase` subclass. `release.sh:125-134` copies it into the zip and
  documents that both it and `config.json` MUST ship or the MCM silently fails.
  **There is no `MFO_MCM.psc` anywhere on the machine** (`find` returned none)
  and **no compile step** in `.github/workflows/`, `release.sh`, or `Docs/BUILD.md`
  (all grepped, all empty for papyrus/psc/pex/compile). The `.pex` was compiled
  once, externally, and **committed as a binary**.
- **VMAD machinery already exists in the ESP generator.** `MFO_GenerateESP.py`
  has a working `VMADBuilder` (`:207-223`) and attaches `MFO_MCM` to
  `MFO_MCMQuest` with **zero properties** (`:351-353`). So attaching a script to
  a record is a solved problem in the generator; what is missing is the
  **compiler** to produce the `.pex` the VMAD references, and MFO's own natives.
- **The design doc's "no companion .psc" ruling is narrow and does NOT block
  this.** `ENGINE_NOTES.md:283-286` settles that a script cannot deliver the
  **attack verb** (no combat-target setter exists in Papyrus). That is a
  statement about *commanded targeting*, not about *merchant I/O*. And
  `ENGINE_NOTES.md:1124-1127` already anticipates a shipped `.psc` fallback and
  explicitly says the attack-verb objection "does not apply to a one-line alias
  filler." A merchant-read/transaction script is squarely in the allowed class.

**Therefore the plan must establish a `.psc → .pex` pipeline.** See §3.

### 1.3 The data channel: MFO-registered Papyrus natives

MFO registers **no** Papyrus native functions today (`grep RegisterFunction /
GetPapyrusInterface native/` → empty; `plugin.cpp` only installs the main-thread
pump at `:258`). This feature adds them. The SKSE pattern:

```cpp
// plugin.cpp, at SKSE messaging kDataLoaded (or in SKSEPlugin_Load):
SKSE::GetPapyrusInterface()->Register(MFO::TradeBridge::RegisterFuncs);

// TradeBridge::RegisterFuncs(RE::BSScript::IVirtualMachine* a_vm):
a_vm->RegisterFunction("GetSellForms",   "MFO_Trade", MFO::TradeBridge::GetSellForms);
// ... one per accessor. Native returns are delivered to Papyrus SYNCHRONOUSLY.
```

A registered native's return value (`Form[]`, `int[]`, `int`, `bool`) is
returned to the calling Papyrus frame **in-line**, unlike the dispatch callback.
That is why the Papyrus script **pulls** the TradeOrder from native rather than
having native push a list across the (single-string/single-float) mod-event bus.

### 1.4 The safe Papyrus merchant-read API

The crash (`economy-vendor-detection-excludes-teammates` memory; `Logistics.cpp:
1632-1646`) is specifically **native `TESObjectREFR::GetInventory()` on a
persistent merchant chest whose `InventoryChanges` no barter menu has
populated** — it null-derefs a form cast on ANY thread. The safe surface:

- **Vendor GOLD:** `int chest.GetItemCount(Gold001)` on the merchant chest
  reference (Gold001 = `0x0000000F`). Papyrus `GetItemCount` routes through the
  engine's inventory-changes init (the same path the barter menu uses), which is
  what makes it safe where raw native `GetInventory()` is not. Add the vendor
  actor's pocket via `Actor.GetGoldAmount()` (also safe from Papyrus, and safe
  natively — the native probe already used `vendor->GetGoldAmount()`). **[VALIDATE
  in the read-only probe (§5) that `GetItemCount` on the unpopulated chest does
  not itself fault — this is the load-bearing empirical question.]**
- **Vendor STOCK — candidate-driven, no enumeration needed.** MFO does **not**
  need to walk the whole merchant inventory. Native already knows the finite set
  of forms a follower would *buy* (the item catalog's `potions`/`ammo` sections,
  resolved to runtime FormIDs), and it ranks them itself by gold value (readable
  off the base form: `Form.GetGoldValue()` / native `TESObjectWEAP/ARMO/…` value
  — no chest access). So the script only asks, per ranked candidate,
  `int chest.GetItemCount(candidateForm)` — "does this vendor stock ≥1, and how
  many?" This sidesteps needing a container enumerator entirely.
- **If full enumeration is ever wanted** (e.g. to discover arrow types not in
  the catalog), po3 provides `AddAllItemsToArray(chest, …)` /
  `AddItemsWithKeywordToArray(chest, keyword, …)` and vanilla gives the
  `GetNumItems()`+`GetNthForm(i)` walk (§A.2). The candidate-driven design above
  is preferred precisely because it avoids the extra dependency and cost.
- **Leveled-list stock** resolves into the merchant chest's live inventory when
  the vendor is instantiated/refreshed; `GetItemCount` sees the resolved leaf
  forms, not the LVLI. No special call needed once the chest exists.
- **Force-load:** the merchant chest is a persistent ref; its inventory lives in
  extra data, not 3D, so a `Utility.Wait`/force-load is **not** expected to be
  required. The probe measures whether any settling wait is needed.

### 1.5 Primary sources in the installed modlist — see §A (survey complete)

Headlines (full detail + paths in §A):

- **Shipped prior art that does EXACTLY this read+transaction:** `C.O.I.N. -
  Merchant Exchange` (LoreRim). Its dialogue fragment reads a merchant's stock
  chest and gold and moves goods, all in Papyrus, using
  `PO3_SKSEFunctions.GetVendorFaction(npc)` →
  `GetVendorFactionContainer(faction)` → `vendorChest.GetItemCount(Gold001)` →
  cap by affordability → `RemoveItem`/`AddItem` + `vendorChest.RemoveItem(
  Gold001, gold)`. **This is a live, shipped proof that the Papyrus merchant
  read is crash-free** — it directly de-risks the plan's one load-bearing
  empirical unknown (§6.1). MFO mirrors the *method*, substituting the follower
  for the player and driving it from a package/native trigger instead of dialogue.
- **The family already compiles Papyrus with a stubs pattern.** MAO and MEO
  (`../marth-alchemy-overhaul`, `../marth-enchanting-overhaul`) each carry
  `Source/Scripts/` (their real `.psc`, incl. `MCM_ConfigBase.psc`) +
  `Source/Stubs/` (hand-rolled minimal imports: `SKI_ConfigBase.psc`,
  `ReferenceAlias.psc`, `Quest.psc`, `Package.psc`, …). This is the established
  route MFO's own MCM `.pex` came from and the route this feature adopts — **not**
  a from-scratch Caprica setup.
- **The Papyrus extender surface is all present** in the install: po3
  `GetVendorFaction`/`GetVendorFactionContainer`/`AddAllItemsToArray`/
  `AddItemsWithKeywordToArray`; PapyrusUtil `StorageUtil` FormList arrays;
  vanilla `ObjectReference.GetItemCount`/`GetNumItems`/`GetNthForm`/`AddItem`/
  `RemoveItem`, `Actor.GetGoldAmount`, `Form.SendModEvent`/`RegisterForModEvent`.

### 1.6 Constraints carried from the MFO memories

- **`AddTask` ≠ main thread** (`skse-addtask-runs-on-job-worker`): worker→main
  is `MFO::MainThread::Post` (the player-`Update` vfunc pump, `MainThread.cpp`).
  **Papyrus dispatch and mod events run on the VM/main context**, which is
  exactly why the merchant mutation belongs in Papyrus, not on a worker.
- **Merchant read/mutation must be main/VM** (`economy-vendor-detection…`,
  `ENGINE_NOTES §0.37`): the whole reason the read moved to Papyrus.
- **Vendor resolution guards (reuse, already in `EconomyProbe`):** skip
  `IsPlayerTeammate()` (`Logistics.cpp:1585`); resolve the faction via
  `VisitFactions` + `IsVendor() && OffersServices()`, **not** `GetVendorFaction`
  (NG bug, mutates state) (`Logistics.cpp:1600-1605`); gate any chest touch on
  `chest->GetContainer()` (`Logistics.cpp:1635`).
- **Catalog-first classification** (`synthesis-item-catalog`): sellables clear
  `Catalog::IsExcluded`; buy candidates come from the catalog's potion/ammo
  sections; never trust runtime archetype/flag guesses as primary.
- **Collect-then-act, re-find by key** (`ENGINE_NOTES §0.37`, INVARIANTS #2/#3):
  the equip/inventory dispatch is synchronous into third-party sinks; snapshot
  `(object, count)` tuples, hold actors by handle, act second. In this design
  native snapshots into the TradeOrder; Papyrus acts on FormIDs re-resolved live.
- **Magnum-opus posture** (`mfo-is-magnum-opus…`): build the real bridge
  (registered natives + a shipped script + a compiler pipeline), not a fragile
  mod-event-only hack.

---

## 2. Architecture + data flow

### 2.1 Work split

| Concern | Owner | Why |
|---|---|---|
| Vendor resolution (teammate skip, faction, `GetContainer` gate) | **Native** | already written in `EconomyProbe`; reuse verbatim |
| Sellables selection (unworn WEAP/ARMO/jewellery, `IsExcluded`, VEND keyword filter) | **Native** | reads the **follower's own** inventory (safe) + faction VEND list (safe); catalog lookups live here |
| Buy candidates + quota (which supply gambits are below N, how many to buy) | **Native** | the logistics gambit table + `CountPotions`/`ArrowCount`/`BoltCount` live here |
| Quality ranking of buy candidates | **Native** | gold value read off the base form; catalog knows arrow/bolt/potion class |
| Vendor gold + per-candidate stock **read** | **Papyrus** | merchant-chest access — the crash-free path |
| The actual `RemoveItem`/`AddItem`/gold transfer | **Papyrus** | merchant-safe mutation on the VM/main context |
| Result accounting (what actually sold/bought, gold delta) | **Papyrus → Native** | via a registered native |

### 2.2 The TradeOrder (native → Papyrus, pulled)

Native builds, per trade, a `TradeOrder` in a token-keyed map:

```
TradeOrder {
  token          : int32          // opaque handle for this trade
  follower       : ActorHandle    // filled into the trade alias
  vendorActor    : ObjectRefHandle // for pocket gold + as AddItem/RemoveItem peer
  merchantChest  : ObjectRefHandle // fac->vendorData.merchantContainer (GetContainer-gated)
  sellForms[]    : FormID          // unworn, un-excluded, VEND-tradeable
  sellCounts[]   : int             // parallel to sellForms
  buyCandidates[]: FormID          // ranked best-quality-first, per category
  buyQuota[]     : int             // how many MORE of each category to reach N
  buyBudget      : int             // follower purse (GetGoldAmount)
}
```

`sellForms`/`buyCandidates` are what native hands over; **preference is encoded
as ORDER** (buyCandidates already sorted best→worst quality). Papyrus never
ranks — it walks the list and stops at quota/budget/stock. This keeps all
catalog/quality logic native (`§4.5aa` "native owns the data") and the merchant
math trivial in Papyrus.

### 2.3 Sequence diagram

```
  WORKER TICK (Logistics::ServiceFollower)                MAIN / VM (MFO_Trade.psc)
  ────────────────────────────────────────                ─────────────────────────
  1. resolve vendor (VisitFactions, teammate
     skip, GetContainer gate)  [native, safe]
  2. build sell list from FOLLOWER inv +
     VEND filter                [native, safe]
  3. build ranked buy candidates + quota from
     logistics gambits          [native, safe]
  4. store TradeOrder{token}     [native map]
  5. MainThread::Post( fill alias, dispatch ) ──┐
                                                │  (frame boundary)
                                                ▼
                            6. ForceRefTo(tradeAlias, follower)   [native, main]
                            7. DispatchMethodCall2(aliasHandle,
                                 "MFO_Trade","RunTrade", {token}) ──► 8. RunTrade(token)
                                                                        │
                            ◄── GetVendorChest(token) ───────────────── │ (MFO native, sync ret)
                            ◄── GetSellForms/Counts(token) ──────────── │
                            ◄── GetBuyCandidates/Quota/Budget(token) ── │
                                                                        │ 9. read chest gold =
                                                                        │    chest.GetItemCount(Gold001)
                                                                        │    + vendor.GetGoldAmount()
                                                                        │ 10. SELL loop: for each
                                                                        │    sellForm, while vendorGold>=val:
                                                                        │      follower.RemoveItem(f,n,false,chest)
                                                                        │      follower.AddItem(Gold001,val*n) //  or vendor pays
                                                                        │ 11. BUY loop: down buyCandidates,
                                                                        │    have=chest.GetItemCount(c); buy
                                                                        │    min(quota,have,budget/val); 
                                                                        │      chest.RemoveItem(c,k,false,follower)
                                                                        │      follower.RemoveItem(Gold001,val*k,false,chest)
                            ◄── ReportTrade(token,soldVal,boughtN) ──── │ 12.
                            13. evict alias (ForceRefTo player),
                                free TradeOrder{token}   [native]
```

Steps 6, 13 reuse MFO's **existing alias-fill/evict machinery** (`ForceRefTo`,
the loot/command quests' pattern). Step 7 reuses **existing `DispatchMethodCall2`**
— the only new dispatch wrinkle is targeting a **custom** script class
(`MFO_Trade`) instead of a vanilla one, which requires the script be attached to
the alias whose handle is used (it is — §3.2).

### 2.4 Why a ReferenceAlias, not `DispatchStaticCall`

Two viable triggers:

- **(chosen) ReferenceAlias + `DispatchMethodCall2`.** Attach `MFO_Trade` to a
  new `MFO_TradeActor` ReferenceAlias. Native fills the follower, gets the
  alias's `VMHandle`, dispatches `RunTrade(token)`. The script's
  `Self as ReferenceAlias).GetActorReference()` *is* the follower — natural, and
  it reuses the alias-fill code MFO already trusts. The vendor/chest come from
  the token via natives.
- **(rejected) `DispatchStaticCall("MFO_Trade","RunTrade",{token})` to a global
  function** with no alias. Simpler (no alias record), but then the script has
  no bound instance / `Self` and must pull *everything* (follower included) from
  natives, and MFO loses the alias as a natural place to hang state/lifecycle.
  Keep it in reserve if the alias attach proves fiddly.

---

## 3. Exact new / changed files

### 3.1 Papyrus source (NEW) — `papyrus/MFO_Trade.psc`

A new top-level `papyrus/` source dir (mirrors the `Source/Scripts` convention).
`MFO_Trade extends ReferenceAlias`. Declares MFO natives + implements `RunTrade`:

```papyrus
Scriptname MFO_Trade extends ReferenceAlias

; ── MFO-registered natives (return SYNCHRONOUSLY to Papyrus) ──
Form[]  Function GetSellForms(int token)      global native
Int[]   Function GetSellCounts(int token)     global native
Form[]  Function GetBuyCandidates(int token)  global native
Int[]   Function GetBuyQuota(int token)       global native
Int     Function GetBuyBudget(int token)      global native
ObjectReference Function GetVendorChest(int token)  global native
Actor   Function GetVendorActor(int token)    global native
Bool    Function GetProbeOnly(int token)      global native   ; §5 read-only gate
Function ReportTrade(int token, int soldValue, int boughtCount, int vendorGold) global native

Function RunTrade(int token)
    Actor follower = GetActorReference()
    ObjectReference chest = GetVendorChest(token)
    Actor vendor = GetVendorActor(token)
    if follower == none || chest == none
        ReportTrade(token, 0, 0, -1)  ; native logs + evicts
        return
    endif
    int vendorGold = chest.GetItemCount(Gold001) + vendor.GetGoldAmount()
    bool probe = GetProbeOnly(token)
    ; ... SELL loop, BUY loop exactly as §2.3; guarded by `if !probe` for mutations,
    ;     but ALWAYS compute what it WOULD do and pass to ReportTrade.
    ReportTrade(token, soldValue, boughtCount, vendorGold)
EndFunction
```

`Gold001` is resolved once (`Game.GetFormFromFile(0xF, "Skyrim.esm")` or a
filled property). Property vs. code-lookup decided at implementation; code-lookup
avoids a VMAD property.

### 3.2 ESP records (CHANGED — `MFO_GenerateESP.py`)

Add a **new script-bearing quest** `MFO_TradeQuest` with one ReferenceAlias
`MFO_TradeActor`, mirroring `make_loot_quest`'s DLL-filled-alias shape but
carrying a **VMAD script** on the alias (new: aliases can carry VMAD too — the
generator currently only VMADs a quest; extend `VMADBuilder`/alias emit to write
an alias-level script object, or attach `MFO_Trade` at the **quest** level and
have it register the alias — decide at implementation; **quest-level script that
`RegisterForSingleUpdate`-style drives the alias is the simplest VMAD**).

New FormIDs (append; FormIDs are frozen once shipped — INVARIANTS #41):

```
FID_TRADE_QUEST   = OWN | 0x80E   # QUST: 1 ReferenceAlias, DLL-filled, carries MFO_Trade
                                   # (0x80D is free between LootQuest 0x80C and 0x80E)
```

- No new PACK (the trade has no AI package — the follower is already standing at
  the merchant when native decides to trade; if "walk to the merchant" is later
  wanted, that is Loot-Option-A-style travel, a separate package, out of scope
  here).
- The quest is **start-game-enabled, NOT run-once**, so it **MUST get a SEQ
  entry** (the generator already emits `out/SEQ/MFO.seq`; add this quest — see
  `make_retreat_quest`'s SEQ note `:504-506`).
- `tools/audit_esp.py` frozen-id table (`:39` region) gets the new record +
  its subrecords (`EDID`,`DNAM`,`VMAD` on the quest or alias).
- `native/Forms.h` gets `kTradeQuest = 0x80E` + `g_tradeQuest` + resolution in
  `Forms::Resolve()` (a missing form disables ONLY the trade, with a named log
  line — never a crash; the existing doctrine, `Forms.h:53-56`).

### 3.3 Papyrus compile + ship pipeline (NEW) — adopt the MAO/MEO stubs pattern

The pipeline must turn `MFO_Trade.psc` into `Scripts/MFO_Trade.pex` and ship it
beside `MFO_MCM.pex`. **This is not new ground for the family — MAO and MEO
already do it**, and MFO's own `MFO_MCM.pex` came from this route (it was just
never checked into MFO's own tree). Mirror their layout exactly:

- **Repo layout (NEW dirs, copy MAO/MEO):**
  `Source/Scripts/` — the real MFO `.psc` (`MFO_Trade.psc`, and fold in the
  existing `MFO_MCM.psc` so the MCM stops being an orphan binary); `Source/Stubs/`
  — hand-rolled minimal import stubs. From MAO/MEO's `Source/Stubs/` MFO needs at
  least: `ReferenceAlias.psc`, `Quest.psc`, `Actor.psc`, `ObjectReference.psc`,
  `Form.psc`, `Game.psc`, `Debug.psc`, plus the po3 extender declaration
  `PO3_SKSEFunctions.psc` (or import po3's real source dir). Copy the MAO/MEO
  stubs wholesale, add only the vendor/inventory functions this script calls.
- **Compiler:** `PapyrusCompiler.exe` (Bethesda's; **no full vanilla source tree
  exists anywhere on the machine**, which is exactly why the family compiles
  against STUBS, not the CK `Scripts/Source`). Nearest binaries are Nemesis-
  bundled: `LoreRim/mods/Project New Reign - Nemesis .../Nemesis_Engine/Papyrus
  Compiler/PapyrusCompiler.exe` (+ `PapyrusAssembler.exe`, `PCompiler.dll`,
  `TESV_Papyrus_Flags.flg`). On the Linux dev box run it under **Wine**; on CI
  it is a native Windows step. Check MAO/MEO's build script for the exact
  invocation and reuse it verbatim (flags: `-import=Source/Stubs` +
  po3/SKSE partial `Scripts/Source`, `-output=out/Scripts`,
  `-flags=TESV_Papyrus_Flags.flg`).
- **Where it runs:** a `tools/compile_papyrus.sh` (Wine wrapper) invoked by
  `release.sh` before the `cp out/Scripts/*.pex` stage (`release.sh:134`), and/or
  a CI step in `.github/workflows/native.yml`. Emit to `out/Scripts/`.
- **Ship:** add `cp out/Scripts/MFO_Trade.pex "$STAGE/pkg/Scripts/"` next to the
  MCM copy (`release.sh:134`).
- **Fallback (unblocks immediately):** compile once via Wine locally and **commit
  `MFO_Trade.pex` as a prebuilt binary**, exactly as `MFO_MCM.pex` is committed
  today. The real compile step is the magnum-opus answer but is not on the
  feature's critical path — the prebuilt gets the bridge running now.
- **Imports note:** `INDEX.md:187-190` calls the SKSE64 partial `Scripts/Source`
  the mandatory import path; the survey confirms it is only 62 files (SKSE-
  extended base), not the full CK tree — which is why the STUBS supplement is
  mandatory for anything those 62 don't cover (`ReferenceAlias.psc`,
  `MCM_ConfigBase.psc`, `PO3_SKSEFunctions.psc`).

### 3.4 Native (NEW/CHANGED)

- **NEW `native/TradeBridge.{h,cpp}`** — the registered-natives module +
  TradeOrder store:
  - `bool RegisterFuncs(RE::BSScript::IVirtualMachine*)` — registers the ~9
    natives on class `"MFO_Trade"`.
  - The token→TradeOrder map (main/VM-thread-only, same transient-state
    discipline as `Logistics`; cleared on revert).
  - Native impls: `GetSellForms`/`GetSellCounts`/`GetBuyCandidates`/
    `GetBuyQuota`/`GetBuyBudget`/`GetVendorChest`/`GetVendorActor`/
    `GetProbeOnly` (all `(RE::StaticFunctionTag*, int token)` → value), and
    `ReportTrade(tag, token, soldValue, boughtCount, vendorGold)` which logs
    the result, **evicts** the follower from the trade alias, and frees the
    token. Forms returned as `RE::TESForm*`/`RE::BSScript::VMArray<...>`.
  - `int BeginTrade(TradeOrder&&)` — allocate a token, store, return it.
  - `void Dispatch(int token)` — fill `MFO_TradeActor` with the follower and
    `DispatchMethodCall2(aliasHandle, "MFO_Trade", "RunTrade", {token})`.
- **CHANGED `native/Papyrus.{h,cpp}`** — add a generic dispatcher that targets a
  **custom** class + method by handle (the existing two are vanilla-class
  specific). Signature e.g. `bool DispatchScriptCall(RE::TESForm* handle_form,
  const char* scriptClass, const char* method, args…)`. Reuses `HandleFor` and
  the `unique_ptr<IFunctionArguments>` leak guard verbatim.
- **CHANGED `native/plugin.cpp`** — at the SKSE `kDataLoaded`/`Load` path:
  `SKSE::GetPapyrusInterface()->Register(MFO::TradeBridge::RegisterFuncs)`
  (currently there is no papyrus registration at all).
- **CHANGED `native/Logistics.cpp`** — replace `EconomyProbe`'s inline
  merchant-chest read with: build the `TradeOrder` from the safe reads it
  already does (follower inv, VEND filter, gambit quotas), `TradeBridge::
  BeginTrade`, then `MainThread::Post([token]{ TradeBridge::Dispatch(token); })`.
  Keep the **cadence gates** (15 s scan, 60 s per-pair) and the vendor-resolution
  guards. The buy-candidate ranking (currently the `scanStock` value compare)
  moves to a native ranking over **catalog** forms (no chest access).
- **CHANGED `native/Forms.{h,cpp}`** — the `kTradeQuest` resolution above.
- **CHANGED `native/CMakeLists.txt`** — add `TradeBridge.cpp`.

---

## 4. API surface

### 4.1 What native calls (into the engine / VM)

- `RE::TESQuest::ForceRefTo(alias, follower)` — fill the trade alias (existing
  machinery). Evict = `ForceRefTo(alias, player)` (the proven evict, `§0.36`).
- `vm->DispatchMethodCall2(aliasHandle, "MFO_Trade", "RunTrade", args, {})` —
  fire-and-forget trigger (existing dispatch, custom class).
- `SKSE::GetPapyrusInterface()->Register(RegisterFuncs)` — once, at load.
- Safe native reads only: `follower->GetInventory()` (the follower's own),
  `vendor->GetGoldAmount()`, `fac->vendorData.{vendorSellBuyList,vendorValues,
  merchantContainer}`, `chest->GetContainer()` (gate), `entry->IsWorn()`,
  `Catalog::*`, `Form::GetGoldValue`-equivalent off WEAP/ARMO/AMMO/ALCH.
  **Never** `merchantChest->GetInventory()` — that is the banned call.

### 4.2 What Papyrus exposes (registered by native, called by `MFO_Trade.psc`)

| Function | Returns | Purpose |
|---|---|---|
| `GetSellForms(token)` | `Form[]` | items to sell |
| `GetSellCounts(token)` | `Int[]` | counts parallel to sell forms |
| `GetBuyCandidates(token)` | `Form[]` | ranked best-first |
| `GetBuyQuota(token)` | `Int[]` | per-candidate/category max to buy |
| `GetBuyBudget(token)` | `Int` | follower purse |
| `GetVendorChest(token)` | `ObjectReference` | merchant container |
| `GetVendorActor(token)` | `Actor` | vendor (pocket gold + trade peer) |
| `GetProbeOnly(token)` | `Bool` | §5: true = compute-but-don't-mutate |
| `ReportTrade(token, soldValue, boughtCount, vendorGold)` | — | result + triggers native evict/free |

### 4.3 What Papyrus calls (vanilla natives, the crash-free surface)

`chest.GetItemCount(form)`, `Actor.GetGoldAmount()`,
`ObjectReference.RemoveItem(form, count, silent=false, dest)`,
`ObjectReference.AddItem(form, count, silent)`, `GetActorReference()` (alias).

---

## 5. Phased task list (read-only probe → transactions)

**Phase 0 — pipeline proof (no behaviour).** Stand up the `.psc → .pex`
compile+ship (§3.3) with a trivial `MFO_Trade.psc` that only logs on `RunTrade`.
Attach it to `MFO_TradeQuest`/alias (§3.2). Register a single no-op native.
Ship, load on the deck, confirm: quest starts (SEQ), script attaches (no Papyrus
load error à la INVARIANTS #43), the native registers, and a dispatched
`RunTrade` logs. **This proves the whole bridge before any merchant is touched.**

**Phase 1 — READ-ONLY merchant probe (zero transactions).** `GetProbeOnly`
returns **true**. `RunTrade` reads `chest.GetItemCount(Gold001)` + candidate
stock, computes the full WOULD-SELL / WOULD-BUY plan, and `ReportTrade`s it;
native logs it in the exact `[econprobe] WOULD SELL / WOULD BUY` format the old
native probe used (`Logistics.cpp:1681-1734`) — same discipline, now crash-free.
**Success = the log shows a correct plan for a real vendor (Ma'dran, Belethor)
with NO crash.** This is the measurement the native probe could never survive.

**Phase 2 — SELL only.** Flip `GetProbeOnly` false for the sell loop only. Cap
total sale at `vendorGold`. Verify gold/inventory deltas on the deck; verify the
VEND filter and `IsExcluded` actually gate (nothing quest/unique sold).

**Phase 3 — BUY.** Enable the buy loop: down the ranked candidates, buy
`min(quota, chest stock, budget/value)` of each, decrementing budget. Verify
quota fills (arrows/potions rise to N), purse respected, vendor stock decrements.

**Phase 4 — hardening.** Cadence/anti-thrash (don't re-trade the same vendor
every scan; the 60 s per-pair gate + a "traded recently" cooldown). Interaction
with the loot system (a follower mid-loot-travel doesn't also trade). MCM toggle
(`bEconomy`) + gold/quota tunables. Heartbeat counters (Dispatches parity).

---

## 6. Open questions / risks for marth

1. **Does `chest.GetItemCount()` (Papyrus) itself fault on the unpopulated
   merchant chest?** **Largely DE-RISKED by prior art:** the shipped
   `C.O.I.N. - Merchant Exchange` reads a live merchant's stock chest via
   `PO3_SKSEFunctions.GetVendorFactionContainer(faction)` then
   `vendorChest.GetItemCount(Gold001)` and moves goods, in Papyrus, in LoreRim
   right now (§A.4) — so the Papyrus read path on a real merchant is proven
   crash-free where native `GetInventory` was not. Residual risk: C.O.I.N.
   triggers from *dialogue* (which itself opens the vendor and primes the
   chest); MFO triggers from a *package/native* with no menu open, so the chest
   may be one step less initialised. **Phase 1 measures this directly, zero
   transactions.** If MFO's cold read differs, mitigations in preference order:
   (a) resolve the chest with po3 `GetVendorFactionContainer` (what the proven
   mod uses) instead of the raw `vendorData.merchantContainer` handle; (b)
   prime it (a benign `GetItemCount` warm-up, or force-init InventoryChanges
   natively) before the read loop. Need marth's read only if (a)/(b) both fail.
2. **Papyrus compile: wire it up, or commit a prebuilt?** The family route
   exists (MAO/MEO stubs + Nemesis `PapyrusCompiler.exe` under Wine, §3.3), so
   this is a preference, not a blocker. Does marth want a `tools/compile_papyrus.sh`
   + CI step now (and the existing orphan `MFO_MCM.pex` folded into it — its
   `.psc` should be recovered from a sibling or decompiled), or just a committed
   `MFO_Trade.pex` to unblock, matching how `MFO_MCM.pex` ships today?
3. **Who pays / where does the sold-item gold go?** Vanilla barter moves the
   item to the merchant chest and gold to the seller; a follower selling to a
   vendor should mirror that (follower gains gold, vendor gains item, vendor gold
   pool drops). Confirm the desired accounting (does the vendor's gold pool
   actually deplete, or is it cosmetic?) and whether follower purchases should
   ever dip into the **player's** gold (design says no — follower's own purse).
4. **Alias-level vs quest-level script attach** (§3.2) — the generator currently
   only VMADs a quest. Attaching `MFO_Trade` to the alias is cleanest for
   `GetActorReference()`; if extending the alias VMAD emit is fiddly, fall back
   to a quest-level script or the `DispatchStaticCall` global-function variant
   (§2.4). Low risk, but names a small implementation fork.

---

## A. Installed-modlist survey (read-only, `/mnt/gaming/modlists/custom-modlist` + siblings)

### A.1 Papyrus compiler + script sources

- **No compiler in `custom-modlist` or `Projects/`.** No `PapyrusCompiler.exe`,
  Caprica, pyro, or CK. Nearest binaries are Nemesis-bundled Windows exes:
  `/mnt/gaming/modlists/LoreRim/mods/Project New Reign - Nemesis Unlimited
  Behavior Engine/Nemesis_Engine/Papyrus Compiler/PapyrusCompiler.exe` (also in
  BottleRim, Tuxborn). That folder also has `PapyrusAssembler.exe`, `PCompiler.dll`,
  `TESV_Papyrus_Flags.flg`, and an 8-file stub `scripts/`.
- **No full vanilla `Scripts/Source` tree anywhere.** Only a 62-file SKSE-
  extended base set at `/mnt/gaming/modlists/custom-modlist/mods/Skyrim Script
  Extender (SKSE64)/Scripts/Source/` (has `ObjectReference.psc`, `Actor.psc`,
  `Form.psc`, `Quest.psc`, `Faction.psc`, `ModEvent.psc`). Stock Game's
  `Data/Scripts/` is `.pex` only.
- **Family compile pattern = STUBS.** `Projects/marth-alchemy-overhaul/Source/
  Stubs/` and `Projects/marth-enchanting-overhaul/Source/Stubs/` hold hand-rolled
  minimal imports (`SKI_ConfigBase.psc`, `SKI_QuestBase.psc`, `ReferenceAlias.psc`,
  `Quest.psc`, `Package.psc`, `Message.psc`, `GlobalVariable.psc`, …) alongside
  the real `Source/Scripts/`. This is the route MFO adopts (§3.3).

### A.2 Extender + vanilla API surface (exact signatures)

**po3 `PO3_SKSEFunctions.psc`** (`custom-modlist/mods/powerofthree's Papyrus
Extender/Source/scripts/`) — the merchant-read core:
```
Faction         Function GetVendorFaction(Actor akActor)                     ; :101
ObjectReference Function GetVendorFactionContainer(Faction akVendorFaction)  ; :448
ObjectReference Function GetMenuContainer()                                  ; :1106
Form[] Function AddAllItemsToArray(ObjectReference akRef, bool abNoEquipped=true, bool abNoFavorited=false, bool abNoQuestItem=false)      ; :788
Form[] Function AddItemsWithKeywordToArray(ObjectReference akRef, Keyword akKeyword, bool abNoEquipped=true, ...)                          ; :796
Form[] Function GetContentFromLeveledItem(LeveledItem akLeveledItem, ObjectReference akRef)   ; :712
Function RemoveListFromContainer(ObjectReference akRef, FormList akList, ..., ObjectReference akDestination=None)  ; :888
```
**vanilla `ObjectReference.psc`** (SKSE64 source) — the safe read + transfer:
```
int  Function GetItemCount(Form akItem) native                              ; :263  (pass Gold001 for chest gold)
int  Function GetNumItems() native                                          ; :771
Form Function GetNthForm(int index) native                                  ; :772  (GetNumItems+GetNthForm = full walk)
Function AddItem(Form akItemToAdd, int aiCount=1, bool abSilent=false) native            ; :156
Function RemoveItem(Form akItemToRemove, int aiCount=1, bool abSilent=false, ObjectReference akOtherContainer=None) native  ; :505
```
**vanilla `Actor.psc`:** `int Function GetGoldAmount() native` (`:252`) — NPC
pocket gold, NOT the vendor chest; use `vendorChest.GetItemCount(Gold001)` for
merchant funds. **`Form.psc`:** `RegisterForModEvent`/`SendModEvent`/
`RegisterForSingleUpdate`. **`ModEvent.psc`:** multi-arg `Create`/`Push*`/`Send`.
**PapyrusUtil `StorageUtil.psc`** (`custom-modlist/mods/PapyrusUtil SE .../Scripts/
Source/`): FormList/IntList arrays (`FormListAdd/Get/Count/ToArray`, `IntListAdd/
ToArray`) if array passing is ever wanted instead of the registered-native pull.

### A.3 MCM base imports

- Full `SKI_ConfigBase.psc` (38 KB): `custom-modlist/mods/MCM Recorder/Scripts/
  Source/SKI_ConfigBase.psc`.
- `MCM_ConfigBase.psc` stub: `Projects/marth-alchemy-overhaul/Source/Scripts/
  MCM_ConfigBase.psc` (reuse for folding the existing MFO_MCM into the pipeline).

### A.4 Prior art — the method to mirror

**No follower/NPC autonomous-trade mod exists** in the install. But
`C.O.I.N. - Merchant Exchange` (`/mnt/gaming/modlists/LoreRim/mods/C.O.I.N. -
Merchant Exchange/Source/Scripts/`, esp. `TIF__0504760E.psc`) is a shipped,
dialogue-driven player↔merchant exchange whose Papyrus is exactly MFO's needed
flow:
1. `Faction vf = PO3_SKSEFunctions.GetVendorFaction(akSpeaker)`
2. `ObjectReference chest = PO3_SKSEFunctions.GetVendorFactionContainer(vf)`
3. `int vendorGold = chest.GetItemCount(gold001)`
4. affordability cap: `totalCount = (vendorGold / valueMult) as int`
5. transfer: `Player.RemoveItem(item, count)`, `Player.AddItem(gold001, gold)`,
   `chest.RemoveItem(gold001, gold)` (deduct merchant funds).

MFO substitutes the follower for `Player` and a native/package trigger for the
dialogue, and adds a BUY leg (`chest.RemoveItem(cand, k, false, follower)` +
`follower.RemoveItem(gold001, val*k, false, chest)`). Ruled-out name matches
(non-scripted or wrong engine): Barter Limit Fix, Trade and Barter, Buy and Sell
Torches, and the FUSION/Magnum Opus/Begin Again hits (Fallout, different engine).
