# H-M1 seat brief: the strict movement order (ABI 48) in Harbinger

Planner document (Fable plans, Opus codes). 2026-11-24. No product code here.
For: one fresh Opus author in `/mnt/gaming/modlists/Projects/ai-package-management-framework`, then one Opus tier A review.
Base plan: `Docs/research/2026-11/tight-movement-plan.md` (sections 6, 8, 10, 12). Where this brief differs from it, this brief wins. The differences are listed in section 1.
Bases: Harbinger `origin/main` a8db589e (ABI 47). `feat/move-probes` 2cfc4a2a and `fix/ch8-enchant` 391054c3 are in review. H-M1 branches from main AFTER both are merged (it chains over the probe's seat, section 4.3). File:line below is a8db589e unless it says 2cfc4a2a.
Disk log with every address as found: `/mnt/gaming/modlists/Projects/_agentlogs/harbinger/hm1-seat-brief.md`. RE helpers: `_agentlogs/harbinger/re2/y.py` (`fn`, `d`, `xref`, `vt`), `re2/ad.sh` (Address Library ids), `re2/sig.py`.

Marks on every fact:

- **DISASM**: read in the unpacked executable. AE = 1.6.1170, SE = 1.5.97, 1.7 = 1.7.104. All addresses are RVAs.
- **RUNS**: a field log shows it executing.
- **UNPROVEN**: not known. Section 14 names the probe and says how the code behaves until then.

## 1. Rulings applied and what changed from the base plan

marth 2026-11-24:

1. Lunges and attacks that carry him out of the circle are ALLOWED by default. He is walked back at once. So S5 stays as it is for a strict actor. Attack in place becomes the opt-in flag (`kMove_AttackInPlace`). Section 6.
2. A movement order beats Flee and Hide. They are denied while an order stands.
3. Dodging is allowed inside the circle. `kMove_NoDodge` is reserved and refused until H-M2.
4. Strict movement is ABI 48. Placed spells H1 is ABI 49.
5. The area being switched off is restored and reported Blocked. Three in 10 s ends the claim as Unreachable.
6. Calm style (ch.33) is not used for holding.

Changes from the base plan, each forced by a fact found while writing this brief:

| Base plan | This brief | Why |
|---|---|---|
| M1 and M2 are `write_call<5>` at the two pick call sites | Six `write_vfunc` seats: two brackets on the selector node (act, update) and four value seats on the selector's child links | INVARIANTS #17 bans call-site patches. #17a allows one only for a facet with NO virtual seat, with its own argument and marth's sign-off. The pick's only inputs are virtual (each child link's slot 0x0A) and the selector's act and update are vtable slots. A virtual seat exists, so #17a cannot be claimed. The call-site offsets are still derived in section 3.4 for the record |
| Thunk calls the original pick first and keeps `n` | Each value seat calls its own original first. Flight and Exit Water are asked through their own nodes | Same engine-answer-first rule, per child |
| Default attack in place, `kMove_AttackFree` opts out | Default lunges allowed, `kMove_AttackInPlace` opts in | Ruling 1 |
| `GetMoveState`, `APMF_MoveStateInfo` | `GetMoveOrderState`, `APMF_MoveOrderInfo` | `APMF_CombatMoveStateInfo` and `GetCombatMoveState` already exist (ABI 38, the bash one-shot, `APMF_API.h:3662`). Too close |
| Disengage is a mode of H-M1 | The mode value is reserved. `SetMoveOrder` refuses it until H-M3 | It needs the aim released (probe P3) |
| A live order on a mounted or scened actor ends after 10 s | It stays Yielded, as every ch.24 claim does today. The client sees `phase` and `sinceMs` and decides | No new end reason without a need. Flagged for marth in section 16 |

## 2. The mechanism in one page

The Movement Selector is ONE node object shared by every fighter (the tree is a static graph, per-actor state lives on the tree thread's data stack). Every 0.25 s, and once at enter, it asks its ten child links in order for a value and takes the first non-zero one (DISASM, section 3). Under a strict order Harbinger answers four of those questions for that actor:

- child 0 (Essential Down, the native 10000 s wait): YES when the order says stand.
- child 2 (Flee) and child 3 (Hide): NO while an order stands.
- child 4 (Return To Combat Area, the native go-to-centre): YES when the order says travel.

Everything else is the engine's. Children 1 (Flight) and 5 (Exit Water) pass on their own answer. Children 6 to 9 are never reached, because child 0 or child 4 already answered. Nothing is failed and nothing is entered by Harbinger: the selector starts and interrupts its children exactly as it does for its own reasons.

The standard combat area keeps carrying the point (S1, existing). Its centre is the travel leaf's goal. Its radius is now `leashR`, so the dodge clamp cuts at the leash.

## 3. Disassembly facts, three runtimes

### 3.1 The selector node

Class `CombatBehaviorTreeNodeObject1<CombatBehaviorDynamicSelector<ConditionalChildSelector>, float>`. All DISASM.

| | AE | SE | 1.7 |
|---|---|---|---|
| vtable | `0x18E2198` | `0x169C510` | `0x1960760` |
| Address Library id | 213473 | 266618 | AE id through the fork table (`data/idmap-1.7.104-fork-full.csv`, EXACT) |
| slot 0x02 act | `0x89AD90` | `0x804D80` (3 instructions, tail-jumps to `0x804AD0`) | `0x8B0230` |
| slot 0x04 update | `0x89D1F0` | `0x806810` (tail-jumps to `0x8066F0`) | `0x8B2690` |
| pick function | `0x85AEE0` | `0x7C4080` | `0x870110` |

- The vtable has 10 slots (0x00 to 0x09). Node fields: `+0x08` name (BSFixedString), `+0x10` parent, `+0x18` child array, `+0x20` child count (u32), `+0x28` the re-pick period (float 0.25, set at AE `0x8ADDE7`).
- The pick is identical on all three: `for i = r8+1 .. count-1: if (child[i]->vfunc[0x0A]()) return i` then `-1`. Both callers pass `r8 = -1`, so child 0 is always asked first. The value call takes only `this`.
- act (AE `0x89AD90`): pushes a 0x20 byte frame on the thread's data stack. Frame: `+0x08` child thread pointer, `+0x10` chosen index (i32), `+0x14` period, `+0x18` time of the last pick, `+0x1C` period. Then pick, store the index, start the child on a child thread (AE `0x858540`). Index -1 means SetFailed and Ascend.
- update (AE `0x89D1F0`): if the child thread has finished (`child+0x138 == 0`): result 2 (interrupted) restarts the child at the stored index (AE `0x858640`), result 1 is SetFailed and Ascend, result 0 is Ascend (the Movement Repeat above then re-enters the selector, which picks again at once). If the child is running and `now - frame+0x18 > frame+0x1C` and the child is not already interrupted: pick. A different index is stored, the child thread is interrupted (AE `0x85DF30`) and the wait bit is set.
- AE and 1.7 act and update are instruction for instruction the same (normalised diff empty). SE has the same body out of line with the same frame offsets.
- The vtable is used by three tree builders on AE (`0x8AD390` Movement, `0x89DD30` LowCombat, `0x897D20`). So the vtable does NOT identify the Movement Selector. Section 4.1.
- RUNS: the selector's pick executes 4 times a second (S6 logged the Return link's own question 37 times in 9 s, `_evidence/deck-anim-1124c`). The hooks on these slots are UNPROVEN until the first session (P-A's first log line, section 12).

### 3.2 The ten child links

Each child is a 0x40 byte LINK node, class `CombatBehaviorTreeValueNodeT<unsigned int, CombatBehaviorExpression<...>>`. Slots 0x02 to 0x05 forward to the node at `link+0x28` (`mov rcx,[rcx+0x28]; mov rax,[rcx]; jmp [rax+0x10]`, AE `0x8603B0`, SE `0x7C8F20`, 1.7 `0x8755E0`). Slot 0x0A is the value and is the LAST slot of the vtable. The expression lives at `link+0x30`. All DISASM, matched by exact RTTI name on all three.

| # | Link | AE vtable (slot 0x0A) | SE vtable (slot 0x0A) | 1.7 vtable (slot 0x0A) | ids SE / AE | Seat |
|---|---|---|---|---|---|---|
| 0 | Essential Down | `0x18E5740` (`0x8AE6E0`) | `0x169FEF0` (`0x817BF0`) | `0x1963D08` (`0x8C3C50`) | 266897 / 213963 | V0 |
| 1 | Flight | `0x18E57A0` (`0x8AE5A0`) | `0x169FF50` (`0x817A90`) | `0x1963D68` (`0x8C3B10`) | 266898 / 213965 | asked, not hooked |
| 2 | Flee | `0x18E5800` (`0x8AE670`) | `0x169FFB0` (`0x817B70`) | `0x1963DC8` (`0x8C3BE0`) | 266899 / 213967 | V2 |
| 3 | Hide | `0x18E2EB0` (`0x89E8F0`) | `0x169D318` (`0x807FD0`) | `0x1961478` (`0x8B3D90`) | 266656 / 213655 | V3 |
| 4 | Return To Combat Area | `0x18E5860` (`0x8AE710`) | `0x16A0010` (`0x817C20`) | `0x1963E28` (`0x8C3C80`) | 266900 / 213969 | V4 |
| 5 | Exit Water | `0x18E58C0` (`0x8AE5F0`) | `0x16A0070` (`0x817AF0`) | `0x1963E88` (`0x8C3B60`) | 266901 / 213971 | asked, not hooked |
| 6 to 9 | Search, Ranged, Flanking, Close | `0x18E2FD0`, `0x18E5920`, `0x18E5980`, `0x18DCCC8` | | | | none |

- Link vtable users on AE: Essential Down, Flee and Return links are built only by the Movement builder. The Hide link's class is generic (`MemberFunc<CombatState, bool()>`) and LowCombat builds one too. Every value seat therefore checks node identity by pointer.
- The value functions read their controller as `[[TLS thread + 0x158] + 0x20]` (AE `0x8599D0`). So inside a value call the selector thread's owner is valid, and `owner+0x20` is the CombatController, the same read every leaf makes (`BehaviorTreeCatalog.h:49`).
- Child 0's node is `CombatBehaviorTreeNodeObject1<CombatBehaviorIdle, float>` (vtable AE `0x18733B0`, SE `0x162FD40`, 1.7 `0x18ED490`, `BehaviorTreeCatalog.h:254`) with `node+0x28 = 10000.0f` (AE `0x8ADCC0`). act (AE `0x557570`) pushes 8 bytes and starts a timer, update only checks it. No path, no resource.
- Child 4's node is the single leaf `CombatBehaviorReturnToCombatArea` (`BehaviorTreeCatalog.h:220`): vtable AE `0x18E54C0`, SE `0x169FA78`, 1.7 `0x1963A88`. act AE `0x8AC700`, SE `0x813C70`, 1.7 `0x8C1C70`. update (slot 0x04) AE `0x8AD2C0`, SE `0x816470` (body `0x8103E0`), 1.7 `0x8C2830`.

### 3.3 The return leaf

All DISASM (base plan sections 2 and 4, re-read for SE and 1.7 by the probe author, `_agentlogs/harbinger/hp-hf.md`).

- Enter reads the current area (`controller+0x98` AE and 1.7, `+0x90` SE, `GetRuntimeData().currentArea`), takes its centre (`area+0x30`) and starts a `CombatPath` to it with goal radius -1.
- Leaf frame: `[thread+0x00] + u32 [thread+0x120]`: `+0x00` CombatPath pointer, `+0x08` the area. Path: `+0x14` status (1 requested, 2 following, 3 waiting, 4 complete, 5 failed), `+0x18` the status at the last update's entry, `+0x20` start time, `+0x24` timeout.
- Update: status 5 clears the area's active byte (`mov byte [area+0x2C],0`: AE `0x8AD2F2`, SE `0x81040A`, 1.7 `0x8C2862`), then SetFailed and Ascend. Status 0 or 4 is Ascend (success). Status 1 to 3 keeps running.
- RUNS: EngineDropped in 6 of 27 legs is this write.
- UNPROVEN: which of the two status 5 sources fires in melee contact and what `path+0x24` holds (probe P2, shipped on `feat/move-probes`, not read yet). UNPROVEN: how far from the centre a completed path leaves the body.

### 3.4 The pick call sites (not used, recorded)

| | AE | SE | 1.7 |
|---|---|---|---|
| in act | `0x89AE22` (`0x89AD90 + 0x92`) | `0x804B0A` (`0x804AD0 + 0x3A`) | `0x8B02C2` (`0x8B0230 + 0x92`) |
| in update | `0x89D2E3` (`0x89D1F0 + 0xF3`) | `0x8067D9` (`0x8066F0 + 0xE9`) | `0x8B2783` (`0x8B2690 + 0xF3`) |

Each is `E8 rel32` to the pick, preceded by `mov rdx,[thread+0x138]` and (update) `mov r8d,-1` or `or r8d,-1`. If the value seats ever prove unworkable, a call-site design needs marth's #17a ruling first. Do not build it under this brief.

### 3.5 Attack displacement (for section 6)

- The attack pick (AE `0x8A28B0`) has exactly two callers, the Attack act chain (in `0x8A4080`) and the Bash act chain (in `0x86B4A0`). S5 brackets both leaves, so S5 covers every caller (DISASM).
- Its end-point test (AE `0x8A2EE0`) calls IsInCombatArea (AE `0x55B310`) at `0x8A3186`. IsInCombatArea is `currentArea->vfunc[0x05]`, which is S2's slot (DISASM).

## 4. The seats

Eight vtable writes. All `write_vfunc` through `allowance::InstallOnVtables` (the pattern at `core/MoveProbe.cpp:428` on 2cfc4a2a), all chaining, any order with other hooks.

| Seat | Vtable | Slot | Thunk signature | Job |
|---|---|---|---|---|
| B1 | selector | 0x02 | `void* (void* node, void* thread)` (`cbt::Act_t`) | open the pick bracket, call the original, close it |
| B2 | selector | 0x04 | `void (void* node, void* thread)` (`cbt::Update_t`) | the same |
| V0 | Essential Down link | 0x0A | `std::uint32_t (void* link)` | stand: answer 1 |
| V2 | Flee link | 0x0A | the same | order live: answer 0 |
| V3 | Hide link | 0x0A | the same | order live: answer 0 |
| V4 | Return link | 0x0A | the same | travel: answer 1 |
| M3 | Return To Combat Area leaf | 0x04 | `cbt::Update_t` | repair the area's active byte, note the path's end |
| S5 | Attack and Bash act (existing) | 0x02 | existing | unchanged by default. Section 6 |

New file: `native/core/MoveOrder.cpp` with `core/MoveOrder.h` (public, small) and the pure decision core in `core/MoveOrderLogic.h` (header only, no engine types, host tested, the `core/WeaponEnchant.h` and `core/FireOnce.h` pattern). `channels/CombatApproach.cpp` is 1569 lines: do not grow it with the mechanism. It gains only the calls listed in section 8. If any file would cross 2500 lines, stop and report.

### 4.1 Which node is THE Movement Selector

Decided structurally, on the combat thread, in the bracket. `IsMovementSelector(node)`:

1. `node` vtable is the selector vtable (implied by the hook, checked anyway).
2. `u32 [node+0x20] == 10`.
3. `child[0]` vtable is the Essential Down link vtable, `child[0]+0x28` has the Idle<float> vtable and `float [that+0x28] == 10000.0f`.
4. `child[2]`, `child[3]`, `child[4]` vtables are the Flee, Hide and Return link vtables. `child[4]+0x28` has the Return To Combat Area leaf vtable.
5. `child[1]` and `child[5]` vtables are the Flight and Exit Water link vtables (they are asked through, so their identity matters).

The first node that passes is stored in an atomic (`g_moveSel`) with its ten child pointers. Later brackets compare the pointer first and run the test only on a miss. Log once: `[move] Movement Selector identified: node RVA-less pointer, 10 children, first seen on actor 0x...` (this is the principle 5 line). A node with the selector vtable and the name "Movement Selector" that FAILS the test while a strict order is live is logged once as a refusal (another mod rebuilt the tree) and the facet reports NotCarried. The name alone is never used (there is a second node of that name inside CloseMovement).

UNPROVEN: that the game holds exactly one such node for the whole session. The design does not depend on it (a second passing node replaces the cache and is logged).

### 4.2 The bracket (B1, B2) and the decision

`thread_local` state: `t_sel { void* thread; bool ours; bool decided; Decision d; int slot; }`. B1 and B2 save the previous value, set `thread` and `ours = (g_strictLive != 0 && node is the Movement Selector)`, call the original, restore. With no live order the cost is one atomic load, one thread-local store pair and the tail of the original.

The value seats run `Decide()` lazily, once per bracket, at the first value question (child 0, because the pick starts there). `Decide()` on the actor's own combat thread:

1. Controller: `ctl = [[thread+0x158]+0x20]`, actor = `ctl->attackerHandle.get()`, Actor form type checked. This is `ResolveController` (`core/ActionGate.cpp:753`). It lives in an anonymous namespace there. Do not copy it: move the two resolvers (`:735`, `:753`) into `core/BehaviorTreeCatalog.h` as inline functions in the same commit, with ActionGate and MoveProbe (`ActorOf`, `core/MoveProbe.cpp:118`) calling them. That is the one shared reader (principle 12). It is a move, so name it in the commit message for the reviewer.
2. Slot lookup by FormID in the strict slot table (section 7.1). None, or mode Free: `d = Orig`.
3. Ask Flight and Exit Water: `child[1]->vfunc[0x0A]()` and `child[5]->vfunc[0x0A]()` through their own vtables (so other hooks compose). Either non-zero: `d = Orig`, set the `passedThrough` bit, phase Yielded.
4. Carried test: `cur = ctl->GetRuntimeData().currentArea`. It must be non-null, have the standard area vtable (`g_vtStd`), be active and enabled (`+0x2C`, `+0x2D`), and its centre and radius must be bit-equal to the slot's current target and `leashR`. If the area is current, standard, active and enabled but does not carry the target yet, call `combatapproach::CarryNow` (section 8.1) and test again. Still not carried: `d = Orig`, phase NotCarried (or Yielded when a higher-priority area is current). Never answer travel without it: the return leaf would path to somebody else's centre.
5. Blocked back-off: `now < retryAtMs`: `d = Stand`, phase Blocked.
6. Otherwise the pure function `movelogic::Step(order, runtime, dist, now)` returns the phase and `Stand` or `Travel` (section 7.2). `dist` is the 3D distance from `actor->data.location` to the current target.

The four value thunks, each after its own original (`a`):

```
V0: not ours, or link != child[0]            -> a
    a != 0                                   -> a   (really down: passes, counted)
    Decide() == Stand                        -> 1
    else                                     -> 0
V2, V3: not ours, or link != child[2|3]      -> a
    Decide() is Stand or Travel              -> 0   (a != 0 is counted: fleeDenied / hideDenied)
    else                                     -> a
V4: not ours, or link != child[4]            -> a
    Decide() == Travel                       -> 1
    else                                     -> a
```

Under `Stand` the pick ends at child 0. Under `Travel` child 0 says 0, Flight says its own (0, or `Decide` would be `Orig`), Flee and Hide say 0, child 4 says 1. Under `Orig` every seat returns the engine's answer. The engine's own Return question still runs inside V4 every pick, so S6's verdict (`CombatApproach.cpp:692`) keeps flowing.

A foreign object in a hooked slot (vtable not in the original map) returns 0, the benign default (#17, the `ShouldEquipThunk` precedent at `CombatApproach.cpp:761`).

### 4.3 M3: the return leaf's update

`feat/move-probes` already seats this slot observe-only (`MoveProbe::ReturnUpdateThunk`, `core/MoveProbe.cpp:127` on 2cfc4a2a). M3 is installed by `MoveOrder` on the same vtable and chains over whatever is there. The probe stays (its MAP entry says so: "install it over the probe, never instead of the chain"). Install order in `plugin.cpp`: `moveorder::Install()` right after `combatapproach::Install()` (`:239`). If the probe installs later it wraps M3, which is equally fine.

M3 body:

1. `g_strictLive == 0`: original, return.
2. Before the original: `frameOff = u32 [thread+0x120]`, `area = [frame+0x08]`, `wasActive = u8 [area+0x2C]` (only when the area has the standard vtable).
3. Original.
4. If `u32 [thread+0x120] != frameOff`: count an anomaly, touch nothing (the probe's rule).
5. Actor from the thread. Strict slot with a live order, else return.
6. Path status at `[frame+0x00]+0x14`. Status 0 or 4: `returnDone` counter up (the short-stop logic reads it). Status 5 and `wasActive == 1` and `u8 [area+0x2C] == 0` and the area is this actor's carried area (`combatapproach::IsCarriedArea(area, fid)`, a shared-lock read of `g_touched`): write `area+0x2C = 1`, record the block (time, the previous status `path+0x18`, the body state bits read from the actor), set `retryAtMs`, log one line. This is the deny of a competing write to the claimed facet, on the actor's own thread, the only thread that may write an area (`CombatApproach.cpp` What breaks (8)).

Size guard: the seat is a vtable slot, so there is no byte window. The guards are the `VerifiedAddresses` row for the leaf vtable (exists, slots list gains nothing new: the probe already added 0x04), the RTTI walk in `InstallOnVtables`, the frame-offset check, and the standard-vtable check before any area byte is read or written.

### 4.4 Address rows, guards and the refusal rule

New rows in `tools/verified_addresses/spec.json`, then regenerate `native/VerifiedAddresses.h` and `Docs/VERIFIED-ADDRESSES.md` (`re2/regen.sh <worktree>`):

| Row name | What | Guard |
|---|---|---|
| `MoveOrder.Selector.vtable` | the selector vtable, slots 0x02 and 0x04 | id pair 266618 / 213473, exact RTTI name |
| `MoveOrder.Link.EssentialDown.vtable` | slot 0x0A | 266897 / 213963 |
| `MoveOrder.Link.Flee.vtable` | slot 0x0A | 266899 / 213967 |
| `MoveOrder.Link.Hide.vtable` | slot 0x0A | 266656 / 213655 |
| `MoveOrder.Link.Return.vtable` | slot 0x0A | 266900 / 213969 |
| `MoveOrder.Link.Flight.vtable`, `MoveOrder.Link.ExitWater.vtable` | identity only, no write | 266898 / 213965, 266901 / 213971 |
| `MoveOrder.reads` (an offsets row, the `[Probe] bMoveProbe reads` row is the model) | node `+0x18`, `+0x20`, `+0x28`. Link `+0x28`. Idle<float> `+0x28`. Selector frame `+0x10`. Area `+0x2C` | verified_by: this document, section 3 |

Byte guards at install, per runtime, compared against the ORIGINAL slot target before the write. They prove the function is the one disassembled and that nobody replaced it (a replaced slot refuses, it is never chained blind for the value seats, because the forced answers assume the engine's selector semantics):

- Selector act head. AE and 1.7: `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B FA 48 8B D9` (21 bytes). SE (`0x804D80`): `40 53 48 83 EC 30 48 8B DA 4C 8D 41 28 48 8B CB`.
- Selector update head. AE and 1.7: `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B 0D`. SE (`0x806810`): `8B 8A 20 01 00 00 48 03 0A E9`.
- Link forward slot 0x02 of each hooked link: `48 8B 49 28 48 8B 01 48 FF 60 10` (the same on all three). It proves `link+0x28` is the child.
- The author re-reads each byte string from the executable with `y.py <ver> d` before pasting it. The strings above were read as raw bytes from the three executables on 2026-11-24. That is not a licence to skip the read.

The refusal rule. Install is all or nothing: every row verifies, every byte guard matches, all seven new writes land, or NONE is written and `kMoveCaps_Strict` stays clear. Then:

- A claim with `kApproach_Strict` is refused at the call (`kInvalidHandle`, the reason logged once per client and returned by `CanServe`). It is NOT quietly served as an area claim. The client asked for a point hold and must know it did not get one.
- `SetMoveOrder` returns `kMoveOrder_NotInstalled`.
- A runtime that is not exactly one of the three refuses the same way. VR refuses.
- 1.7: the five AE ids resolve through the fork's id table. They are in the table at fork tip 961e880a. The author checks the registry baseline Harbinger pins (`native/vcpkg-configuration.json`) carries them. If not, use raw-RVA rows guarded by `SeatVerified`, the `ActionGate.Hold.ParallelDataPush.1_7_104` precedent (`core/ActionGate.cpp:1946`). An id miss is fatal at load on the fork, so this check comes before the first build.
- INI: `[MoveOrder] bStrictOrders` (default 1). 0 installs nothing and refuses as above.

## 5. The two leaves and the arrival test

- **Stand leaf**: child index 0, link pointer `g_child[0]`, node `Idle<float>` 10000 s. There is no handle to keep. The selector owns the child thread (`frame+0x08`) and Harbinger never touches it. If the wait ever ended (2.7 hours) the selector would re-pick and enter it again.
- **Go-to-centre leaf**: child index 4, link pointer `g_child[4]`, node the Return To Combat Area leaf. Its goal is the carried area's centre, read at enter. Harbinger never calls it.
- **Arrival**: `dist <= arriveR` at a pick, or the leaf's own path completing (status 0 or 4, the selector then re-enters and picks at once). Distance is 3D, body origin to the target.
- **Leaving**: `dist > leashR` at a pick. Picks happen every 0.25 s while a child runs, so the worst latency from crossing the leash to the travel pick is 0.25 s, plus one tree step for the child switch.
- **Hysteresis**: travel always runs to `arriveR`, not to the leash edge. Stand holds until `leashR`.

## 6. Lunges allowed: what stays, what moves him, how he comes back

What stays, exactly. S5 (`LeafActThunk`, `CombatApproach.cpp:787`) opens its deny bracket on `CombatBehaviorAttack` act (AE `0x8A5230`, SE `0x80E9A0`, 1.7 `0x8BA700`) and `CombatBehaviorBash` act (AE `0x86C4E0`, SE `0x7D5350`, 1.7 `0x881720`) for every actor whose area carries a bound. Inside the bracket S2 (`:660`) answers the area's slot 0x05 from the engine's own geometry. So the attack pick's end-point test sees the engine's wide area, not the leash, and picks lunges, power attack steps and sprint attacks as in a free fight. H-M1 does not change this code path at all for the default order. The Find Weapon bracket (APMF-B57) also stays as it is.

What then pulls the body out. Not a path and not the Movement half. The chosen attack's own displacement, played as animation root motion, carries him toward his target for the length of that attack (base plan rows 18 and 19). RUNS: a 598 u/s forward burst after each backpedal stretch. UNPROVEN: the share and size (probe P1, shipped, not read yet).

How the walk-back is immediate. The wait leaf keeps the selector re-picking every 0.25 s. At the first pick with `dist > leashR` the phase goes Standing to Returning, V0 answers 0 and V4 answers 1. The selector stores index 4, interrupts the wait and starts the return leaf, which paths to the point. Worst case 0.25 s after the body crosses the leash, no client call, no claim re-file, and the first second is not free any more (the pick does not wait for the 1.0 s area period). He returns facing the foe at backpedal speed (RUNS: 60 to 193 u/s), because the aim controller is the Action half's. Then `dist <= arriveR`, phase Standing, the wait leaf again, and he may attack and lunge again. That cycle (lunge, walk back) is the intended behaviour under ruling 1.

A path that cannot start while the attack animation still owns the body may fail (status 5). M3 repairs it. A block that lands while the actor is attacking, staggered, knocked down or animation driven is an EXCUSED block: repaired, counted in `displacedBlocks`, logged, retried at the next pick, and it does not count toward Unreachable. UNPROVEN whether this happens at all (P2).

`kMove_AttackInPlace` (opt-in). For an actor whose live order has the flag, `LeafActThunk` does not open the bracket on Attack and Bash (Find Weapon unchanged). The engine's own end-point veto then tests each attack against the leash. It needs the actor at each attack act, so it is gated by an atomic count of live in-place orders and resolves the actor only when that is non-zero. It ships in piece P-D and has its own caps bit.

## 7. Per-actor data and the state machine

### 7.1 Data, threads, lifetime

| Data | Writer | Readers | Lifetime |
|---|---|---|---|
| Strict slot table: 64 slots, each a seqlock (`seq`, `fid`, `owner` handle, `mode`, `flags`, `leashR`, `arriveR`, `waypointCount`, `points[7][3]`, `gen`). The ch.33 pattern (`core/CombatStyle.cpp:94` to `:128`) | GAME thread only: ch.24 `Poll` and `Apply`, `SetMoveOrder`, `End`, `ResetAll` | combat threads (Decide, M3, S1, S5), any thread (`GetMoveOrderState`) | written when the claim goes live, cleared when it stops being live. Never crosses a load |
| Slot runtime block (plain atomics beside the slot): `seenGen`, `phase`, `wp` (current waypoint), `distBits`, `phaseSinceMs`, `retryAtMs`, `blockMs[3]`, `blockedCount`, `displacedBlocks`, `lastBlockWhy`, `passedThrough`, `arrivedCount`, `displacedCount`, `shortStops`, `returnDone`, `stopErrBits`, pick counters (stand, travel, orig, fleeDenied, hideDenied) | the actor's OWN combat thread only (one actor, one job) | game thread (Poll, state query, the status line) | reset by the combat thread when it sees `gen != seenGen` |
| `t_sel` | the bracket | the value seats on that thread | one act or update call |
| `g_moveSel`, `g_child[10]` | first bracket that identifies the node | all | session |
| `g_strictLive`, `g_inPlaceLive` | game thread | all, relaxed | counts of live slots |

No lock is taken in any seat except the two `combatapproach` helpers (section 8.1), which use `g_areaMx` exactly as S1 does. No form lookup, no allocation, no log except through a per-actor rate limit (CAS on a last-logged time, the `RateOk` pattern at `ActionGate.cpp:401`).

The slot is live exactly when the ch.24 entry is live (`Entry::live`, `CombatApproach.cpp:281`), the claim has `kApproach_Strict`, and the seats are installed. One writer function, `moveorder::Publish(fid, entry snapshot)`, called from the places `SetLive` and `want` change. A claim that is Waiting, Leashed, released or ending has no slot.

More than 64 live strict actors: the 65th claim is refused at filing with a reason. Followers are far fewer.

### 7.2 Modes and transitions

`movelogic::Step` is pure and host tested. Phases: `Pending` (order published, not decided yet), `Travelling`, `Standing`, `Returning`, `Blocked`, `Yielded`, `NotCarried`, `Unreachable`.

| Mode | H-M1 | On publish | Travelling | Standing | Reports |
|---|---|---|---|---|---|
| `kMove_Free` (0) | yes | no slot decision: every seat answers the engine's. The area carries the claim's R as today (0 is not allowed by ch.24, so Free keeps a bound) | n/a | n/a | phase Yielded never set. `picksOrig` counts |
| `kMove_Stand` (1) | yes | Travelling when `dist > arriveR`, else Standing | pick 4 until `dist <= arriveR` or the path ends | pick 0. `dist > leashR`: Returning (pick 4), `displacedCount` up | nothing on arrival |
| `kMove_GoTo` (2) | yes | the same | the same | the same | `arrivedCount` goes up once per order. The client sends the next order |
| `kMove_Kite` (3) | yes | the same as GoTo | the same. He backs away facing the foe because the Action half keeps its aim. Nothing extra is done | the same | the same as GoTo. The mode is kept apart so H-M3 can release the aim for GoTo and Disengage but never for Kite |
| `kMove_Disengage` (4) | REFUSED (`kMoveOrder_ModeNotServed`) until H-M3 | | | | |
| `kMove_Path` (5) | yes, piece P-D | Travelling to waypoint 0 | `dist <= arriveR` of waypoint i and i is not the last: `wp = i+1`, `CarryNow` the new centre, stay Travelling | at the last waypoint, as Stand | `waypoint` in the state. `arrivedCount` once, at the last one |

Transitions that apply to every strict mode:

- **Returning** behaves as Travelling and ends at `arriveR`.
- **Short stop.** The return path ended (`returnDone` moved) and `dist > arriveR`. Up to `kShortStopRetries` (2) fresh picks of child 4. After that: `dist <= leashR` is accepted as Standing and `stopError = dist` is reported. `dist > leashR` is a block with why ShortPath.
- **Blocked.** M3 saw status 5 and repaired the area. `retryAtMs = now + kBlockBackoffMs` (1000). Until then the pick is 0 (he stands, he does not chase). Then travel again.
- **Unreachable.** Three counted blocks inside `kBlockWindowMs` (10000) with no approach progress (`dist` not reduced by `kBlockProgress`, 32 units, between the first and the third). The combat thread sets phase Unreachable and answers Stand. `Poll` sees it within 100 ms and ends the claim as `kApproachState_Unreachable` (new, 14), loudly. Excused blocks (section 6) are not counted. With the 1 s back-off the three blocks take about 2 to 3 s.
- **Yielded / NotCarried.** `Decide` steps 3 and 4. Every seat answers the engine's. The phase flips back by itself at the first pick where the bound is carried again. Phase history is not kept: he re-enters as Travelling or Standing by distance.
- **A new order on a live claim** (`SetMoveOrder`): `gen` goes up, the combat thread resets the runtime block and starts at Pending. If only `leashR`, `arriveR` or flags changed and he stands inside the new leash, the first decision is Standing and the wait leaf is never interrupted (the pick stays 0). If the point moved, the first decision is Travelling: `CarryNow` writes the new centre before the answer, the selector interrupts the wait (0 to 4) or, when he was already travelling (4 to 4, no interrupt), the running path still ends at the OLD centre first. To avoid that, a point change while phase is Travelling or Returning answers Stand for exactly one pick (an interrupt), then Travel. Cost: at most 0.25 s once per re-point in flight. It is in the residue table.

Tunables, one table `MoveTuning` in `core/MoveOrder.h`: `kDefaultArriveR` 24, `kMinArriveR` 8, `kBlockBackoffMs` 1000, `kBlockWindowMs` 10000, `kBlocksToUnreachable` 3, `kBlockProgress` 32, `kShortStopRetries` 2, `kMaxWaypoints` 7, `kLineMs` 3000. All are labelled placeholders until the P-A and P-B sessions are read. Extension points for MAP.md: a new mode is one row of the mode table in `MoveOrderLogic.h`. A new passing child is one entry of `kAskChildren` ({1, 5}). A new denied child is one value seat row in the install table.

### 7.3 How leashR and arriveR are honoured

It is a point hold, not an area.

- The pick decides against the POINT with the client's two radii. The engine's area test (R plus body radius) no longer decides anything for this actor.
- S1 writes `leashR` as the carried area's radius (through `Entry::want.radius`, set from the order, not from `param.fval`). So the engine's own clamp (way B) cuts dodges, and nothing else under an order, at the leash with pad 0. The travel leaf ignores the radius (its goal is the centre).
- Defaults: `arriveR` 0 means `kDefaultArriveR`. `leashR` 0 means the claim's R. Refused: `leashR < arriveR`, `arriveR < kMinArriveR`, non-finite values, a point out of `PointInBounds`.
- What the claim's `param.fval` (R) means under `kApproach_Strict`: the first `leashR`, and the radius the ch.23 leash test in `Poll` uses (`:1244`).

## 8. Interplay with every existing seat

### 8.1 ch.24 (`channels/CombatApproach.cpp`)

| Piece | file:line | Change |
|---|---|---|
| `Apply` (declaration checks) | `:850`, point check `:886` | accept `kApproach_Strict` only with `Point | Hold`. Refuse it when the seats are not installed, the slot table is full, or the actor is a substrate holder. Build the implicit order (Stand, `pos`, `leashR = fval`, default `arriveR`) |
| `Entry` | `:267` | gains the order (mode, flags, radii, waypoints, `gen`). Game thread only, under `g_entryMx` |
| S1 `UpdateThunk` | `:527`, the write block `:636` to `:652` | no logic change. The block is factored into `CarryLocked(area, ctl, actor, fid, want, now)` so `CarryNow` can call it. `want` for a strict entry is `{current target, pointSpace, leashR}`. For Path the current target is the slot's waypoint `wp`, read from the slot, not from `Entry` |
| new `combatapproach::CarryNow(ctl, fid)` | new, exported in `CombatApproach.h` | actor's combat thread only. Runs S1's steps for this area without the engine update: entry lookup (shared), the 1b `facetattr::Decide`, then `CarryLocked`. If the area already carries an older bound of ours it first puts the vanilla geometry back in the record (keep `Touched::vanilla` from the existing record, replace only `ours`). Returns whether the area now carries the target |
| new `combatapproach::IsCarriedArea(area, fid)` | new | shared lock on `g_areaMx`, `g_touched[area].actor == fid` and `SameGeo` with `ours` |
| S2, S3, S4 | `:660`, `:751`, `:758` | unchanged. The bound must still not leak into target choice and casts. With a 32 to 64 unit leash almost every target is outside the area, which is exactly what S3 hides |
| S5 | `:787` | unchanged by default. `kMove_AttackInPlace`: section 6 |
| S6 | `:692` | unchanged. Still the principle 5 line that the engine asks |
| F3 put-back (`ReactivateFor`) | `:732` | unchanged. It serves FINISHED claims. M3 serves live ones. Both write `+0x2C` only on the actor's own thread |
| `Poll` | `:1105`, state decision `:1286` to `:1334` | publishes and clears the slot. Maps the runtime phase into the reported state: Travelling and Returning are `Approaching`, Standing is `Holding`, Blocked keeps `Approaching`. Ends Unreachable. The stall floor (`kStallMs`, `:178`, end at `:1327`) is unchanged and should no longer fire for a strict actor, because M3 keeps the area active. If it does fire, that is a finding, not something to suppress |
| arrival end | `:1300` | cannot trigger: Strict requires Hold |
| `End` | `:806` | clears the slot first, then as today |
| `RestoreBeforeSave` | `:1408` (called at `plugin.cpp:520`) | unchanged. The slot table is not touched by a save (the game keeps running) |
| `ResetAll` | `:1449` (called at `plugin.cpp:134`, `:457`) | also `moveorder::ResetAll`: zero the slot table and counts. `g_moveSel` is kept (the tree is not rebuilt by a load. UNPROVEN, and harmless: a stale pointer fails the pointer compare and the structural test runs again) |
| internal bounds (held engagement, leash return) | S1 `:585` to `:590`, `SetInternalBound` | unchanged. They apply only when the actor has no live ch.24 claim, and they are never strict |

Lock order stays: `g_entryMx` then `g_areaMx`, never `g_entryMx` with `g_intMx`. The slot seqlock is not a lock.

### 8.2 The others

| Seat | file:line | Rule |
|---|---|---|
| ch.23 pursuit leash (leaf denies in ActionGate, `LeashReturnPoll`) | `core/ActionGate.cpp:1291` (`ActThunkBody`), `:2289`, internal bound calls `:2341`, `:2367` | The leash test in ch.24 `Poll` (`CombatApproach.cpp:1235` to `:1262`) still decides `Leashed`: a Leashed claim is not live, so it has no slot and the engine picks. The 14 leaf denies never meet a strict pick (those leaves are not picked). APMF-B55's goal clamp applies to the strict target too: the slot's target is the CLAMPED point, and the state reports it |
| ch.33 combat style | `core/CombatStyle.cpp:362` (Install), slot table `:94` | no shared slot, no conflict. Under an order the fighting subtrees are never picked, so a style claim has no movement effect. Not used for holding (ruling 6) |
| ch.7 action gate (70 leaf act and pop pairs) | `core/ActionGate.cpp:1612` | the wait leaf (`Idle<float>`) and the return leaf are both in its tables and chain. A ch.7 deny on a leaf the selector started would force-fail a movement child. ch.7 categories do not cover Idle or Return To Combat Area today. The author confirms that from `kLeaves` categories and adds one line to the install log saying so |
| substrate movement hold (ch.27) | `core/ActionGate.cpp:1331` to `:1335`, armed at `:1913` | the held Movement Parallel is entered empty, so the selector never runs. `kApproach_Strict` on a substrate holder is refused at filing. A substrate that starts on an actor with a live order: `Poll` already treats a suppressed actor as not in combat (`CombatApproach.cpp:1232`), the claim waits, the slot is cleared |
| ch.8 cast allow-list and `fix/ch8-enchant` | `core/CastGate.cpp:112` (`CheckCastThunk`) | caster side, Action half. No shared slot or state. Merge order only matters for CHANGELOG and STATUS conflicts |
| cast seats 0x06, AI cast seats, ch.25 line of fire, ch.29 aim | `core/CastSeats.cpp`, `core/AiCastSeats.cpp`, `core/CastGuard.cpp`, `core/ProjectileAim.cpp` | Action half only. The pick does not touch them. A cast that needs a spot is the client's choice of point |
| ch.18 attack mix, ch.20 target pin, ch.21 entry | `core/AttackSelect.cpp:581`, `channels/TargetPin.cpp`, `channels/CombatEntry.cpp` | no interplay. ch.21's held engagement bound is internal (above) |
| ch.19 travel | `channels/Travel.cpp` | out of combat. The control map already arbitrates ch.24 against a live travel |
| MoveProbe P1, P2, P3 | `core/MoveProbe.cpp` (2cfc4a2a) | P2 shares M3's slot and chains. P1 keeps counting leaf acts, which is the cheapest proof that a strict actor enters only Idle and Return To Combat Area. Keep `[Probe] bMoveProbe=1` in every H-M1 session |

## 9. API surface (ABI 48)

`native/APMF_API.h` is byte-shared with MFO (`native/APMF_API.h` there mirrors it exactly) and append-only. Nothing existing is reordered or resized.

- `kABIVersion` 47 to 48 (`:96`). ABI history note beside `:93`. `MinReleaseForAbi(48)` in `core/ClientAPI.cpp`.
- `CombatApproachFlags` (`:832`): `kApproach_Strict = 1u << 2`. Requires `kApproach_Point | kApproach_Hold`. An APMF under 48 would ignore the bit, so a client gates on `abiVersion >= 48` AND `kMoveCaps_Strict`.
- `CombatApproachState` (`:2477`): `kApproachState_Unreachable = 14` (ENDED).
- `MoveMode`: `kMove_Free = 0, kMove_Stand = 1, kMove_GoTo = 2, kMove_Kite = 3, kMove_Disengage = 4, kMove_Path = 5`.
- `MoveFlags`: `kMove_AttackInPlace = 1u << 0`, `kMove_NoDodge = 1u << 1` (reserved, refused until H-M2), `kMove_HoldAttacks = 1u << 2` (reserved, H-M3). Any other bit is refused.
- `MovePhase`: `kMovePhase_None = 0, Pending = 1, Travelling = 2, Standing = 3, Returning = 4, Blocked = 5, Yielded = 6, NotCarried = 7, Unreachable = 8`.
- `MoveOrderResult`: `kMoveOrder_Ok = 0, NotInstalled = 1, BadHandle = 2` (not a live ch.24 claim of the caller), `NotStrict = 3, BadSize = 4, BadMode = 5, ModeNotServed = 6, BadFlags = 7, BadRadii = 8, BadPoint = 9, BadWaypoint = 10, WrongThread = 11`.
- `MoveBlockWhy`: `None = 0, PathBuild = 1` (previous status 5), `PathFollow = 2, ShortPath = 3`.
- `MovePass` bits for `passedThrough`: `EssentialDown = 1, Flight = 2, ExitWater = 4`.
- `MoveCaps` bits: `kMoveCaps_Strict = 1u << 0, kMoveCaps_Path = 1u << 1, kMoveCaps_AttackInPlace = 1u << 2`. Reported in `APMF_ServeInfo` at `+124`, today `reserved` (`:3331`), renamed `moveCaps`, the same move ABI 46 made with `castSeats` at `+120`. The struct stays 128 bytes, its three offset asserts stay.

`APMF_MoveOrder`, 64 bytes, every field 4 bytes, `static_assert(sizeof == 64)` plus offset asserts:

| Off | Field | Meaning |
|---|---|---|
| +0 | `size` | caller sets 64. Smaller is `BadSize`. Larger is read up to 64 |
| +4 | `mode` | `MoveMode` |
| +8 | `flags` | `MoveFlags` |
| +12 | `pos[3]` | the point (or this call's waypoint) |
| +24 | `leashR` | 0 = the claim's R |
| +28 | `arriveR` | 0 = 24 |
| +32 | `faceTarget` | must be 0 in ABI 48 (H-M3) |
| +36 | `waypointIndex` | Path only: which waypoint this call carries, 0 based |
| +40 | `waypointCount` | Path only: 2 to 7. The order goes live when the call with `waypointIndex == waypointCount - 1` arrives and every earlier index was sent for the same count. Any other mode: both 0 |
| +44 | `reserved[5]` | zero |

`APMF_MoveOrderInfo`, 64 bytes, `kMoveOrderInfoV48Size = 64`, the caller sets `size`:

| Off | Field |
|---|---|
| +0 | `size` |
| +4 | `mode` |
| +8 | `phase` |
| +12 | `flags` |
| +16 | `dist` (float, to the current target, as of the last pick) |
| +20 | `sinceMs` (world ms in this phase) |
| +24 | `blockedCount` (counted blocks since the order) |
| +28 | `lastBlockWhy` |
| +32 | `passedThrough` |
| +36 | `waypoint` |
| +40 | `arrivedCount` |
| +44 | `displacedCount` (times he left the leash and was walked back) |
| +48 | `ownerHandle` |
| +52 | `gen` (the order's sequence, so a client can tell its own order from the previous one) |
| +56 | `stopError` (float, distance at which a short stop was accepted, else 0) |
| +60 | `displacedBlocks` (excused blocks) |

`APMF_API_v48 : APMF_API_v38` (v38 is the newest struct with a slot, `:3695`, and the interface object today, `core/ClientAPI.cpp:728`). Two slots:

- `std::uint32_t (*SetMoveOrder)(Handle claim, const APMF_MoveOrder* order)`. GAME THREAD only (it changes claim state, like `Repoint`. `hook::OnMainThread()` false returns `WrongThread`). Changes the order of a live claim in place: no re-file, no arbitration pass. Catch-all, returns a `MoveOrderResult`.
- `std::uint32_t (*GetMoveOrderState)(RE::FormID actor, APMF_MoveOrderInfo* out)`. ANY thread: seqlock read of the slot and relaxed reads of the runtime block. Returns the phase. Never throws out.

`g_api` becomes `constexpr APMF_API_v48` with the two layout asserts in the v38 style (`ClientAPI.cpp:724` to `:727`). The existing `GetCombatApproachState` keeps working for a strict claim and reports the mapped state.

A claim filed with `kApproach_Strict` starts with the implicit Stand order. MFO's bridge sends `SetMoveOrder` only when it wants something else.

## 10. Release, teardown and lifecycle

| Event | Handling | Where |
|---|---|---|
| Release by the client, outranked, dropped | `End` clears the slot (seqlock), then today's road. The next pick is the engine's. The standing child is interrupted by the selector's own re-pick within 0.25 s. S1 restores the engine geometry at its next update, F3 re-activates if needed. No tree restart, no forced fail | `CombatApproach.cpp:806` |
| Save | orders are not saved. `RestoreBeforeSave` as today. M3 never leaves an area inactive, so the saved active byte is the engine's own or 1 | `plugin.cpp:520` |
| Load, revert, new game | `combatapproach::ResetAll` calls `moveorder::ResetAll`. The client re-files | `plugin.cpp:134`, `:457` |
| Cell change, 3D unload | existing: TargetLost when the actor's space is not the point's. APMF-B176 unchanged | `CombatApproach.cpp:1214` |
| Death | existing: ActorGone | Poll |
| Bleedout, essential down | V0's original answers yes, the wait leaf runs by the engine's own choice, `passedThrough` bit set, the order is kept | V0 |
| Combat ends | existing: CombatEnded. Before combat the claim is Waiting and has no slot | Poll |
| Low process | the standard area is disabled: NotCarried, the engine picks. Existing `Disabled` state | Decide step 4 |
| Mount, furniture, scene, dialogue, kill move | no new refusal code in H-M1. While the combat tree does not run the selector, nothing is asked and nothing is answered. If the bound is not carried the phase is NotCarried. The client reads phase and `sinceMs`. Animation driven moves keep the existing stall excuse | Decide, `CombatApproach.cpp:1313` |
| Flying actor | Flight's own value passes (Decide step 3). The order has no effect on a dragon. Refusing at filing would need a fly-state accessor the author must verify in the fork first. If it exists, refuse. If not, leave it to the pass | Decide |
| Swimming | Exit Water's own value passes. He leaves the water, then the order applies again | Decide |
| Stagger, ragdoll, paralysis | not denied. The pick keeps answering. The return path may fail while the body cannot move: excused blocks | section 6 |
| Harbinger unloads or the facet is switched off in the INI | nothing installed, strict refused | section 4.4 |

## 11. Logging and performance

Logging, all rate limited, tag `[move]`:

- Install: one line per seat with its address, one summary (`7 seats + M3, strict orders SERVED` or the refusal with the row that failed).
- First bracket: the identification line (section 4.1). First forced answer of the session per seat: `seat OBSERVED` lines, the S1 pattern (`CombatApproach.cpp:531`).
- Per actor every `kLineMs` while an order is live (game thread, in Poll, from the runtime block): mode, phase, dist, target, leashR, arriveR, picks (stand, travel, orig), fleeDenied, hideDenied, displaced, blocks.
- Every phase change that matters: order published, arrived (with dist and ms since publish), displaced (dist), returned, blocked (why, previous status, body state, repaired), unreachable, yielded and back.
- Heartbeat (30 s, with ch.24's at `CombatApproach.cpp:1382`): totals, zeros included.
- SUMMARY at claim end: time in each phase, max dist while Standing, displaced count, mean walk-back ms, blocks, excused blocks, short stops with stop error. This line is what the sim's residue table is filled from.

Performance:

- No live order: B1 and B2 cost one relaxed atomic load each per selector act or update. The four value seats cost one thread-local read each per pick. M3 costs one load per return leaf update.
- Live order: per pick (4 per second per strict actor) one handle lookup, a slot scan of at most 64 atomics (stop at `g_hi`), two extra virtual value calls, about ten field reads. Per frame nothing beyond the bracket.
- No lock in the pick path except `CarryNow` (only when the target changed) and `IsCarriedArea` (only on status 5).

## 12. Implementation order inside H-M1

Four pieces. Each is one commit that builds and can go to the deck alone. The Opus review is on the whole branch, tier A.

| Piece | Content | Field test it enables |
|---|---|---|
| P-A | ABI 48 header (everything in section 9, so the header is final from the first commit and MFO can start M-1), slot table, `MoveOrderLogic.h` with host tests, rows and byte guards, B1, B2, V0, V2, V3, V4, the implicit Stand order, `Publish` from Poll, `GetMoveOrderState`, logging, radius = leashR | `strict.stand_*`, `strict.keep`, `strict.release`, `strict.free`. Uses the existing dev hold with one extra flag bit. This is the piece that proves the seats run (principle 5) and gives the first residue numbers |
| P-B | M3 repair, Blocked back-off, excused blocks, short stop, Unreachable end | `strict.blocked`, and EngineDropped 0 across all rows |
| P-C | `SetMoveOrder` for Stand, GoTo, Kite. `CarryNow` and `IsCarriedArea`, the one-pick interrupt on a re-point in flight | `strict.goto`, `strict.kite`, `strict.save` |
| P-D | Path waypoints, `kMove_AttackInPlace` (S5 rule), caps bits, docs: MAP.md (new `core/MoveOrder` section, ch.24 What breaks, extension points), `Docs/DENY-COMPLETENESS-AUDIT.md` row 24 (the pick), INVARIANTS (a new numbered rule: the value seats answer only inside the bracket for THE node, never fail a movement child, child 4 only on a carried bound), VERIFIED-ADDRESSES, INI-REFERENCE, STATUS, CHANGELOG, `Docs/BEHAVIOR-TREE-CATALOG.md` (the link table of 3.2) | `strict.path`, the in-place variant of `strict.stand_melee` |

P-A is the smallest thing that can be field tested: about 450 lines plus the header. If the P-A session shows the selector never brackets, or child 0 does not hold him, stop and hand the log to an Opus diagnosis. Do not build P-B to P-D on it.

Hard rules for the author (from CLAUDE.md, repeated because the brief is the only context): verify every CommonLib symbol in `_commonlib/mit-3.7-fork` at the pinned baseline. Local MSVC build green on the final SHA (`re2/lb.sh`). Disk log `_agentlogs/harbinger/hm1-code.md`, WIP pushes at each piece. No merge, tag or force push. No file outside: `native/APMF_API.h`, `native/core/MoveOrder.{h,cpp}`, `native/core/MoveOrderLogic.h`, `native/core/BehaviorTreeCatalog.h`, `native/core/ActionGate.cpp` and `native/core/MoveProbe.cpp` (resolver move only), `native/channels/CombatApproach.{h,cpp}`, `native/core/ClientAPI.cpp`, `native/plugin.cpp`, `native/CMakeLists.txt` if sources are listed, `Data/SKSE/Plugins/APMF.ini`, `tools/verified_addresses/spec.json` and its generated outputs, `tools/move_order/`, the docs named above.

## 13. Tests

### 13.1 Host (`tools/move_order/move_order_test.cpp`, built like `tools/weapon_enchant`)

Against `MoveOrderLogic.h` only. Pass: every case, and the count printed.

| Group | Cases | Pass |
|---|---|---|
| Validation | sizes, each bad mode, flag, radius pair, point, waypoint sequence (gap, repeat, wrong count, more than 7) | 20 of 20 named results |
| Stand | publish outside and inside `arriveR`. Arrive. Stay between `arriveR` and `leashR` (no flip). Cross `leashR`, return to `arriveR` | 8 of 8, and 0 phase flips in a 1000 step walk held at `(arriveR + leashR) / 2` |
| GoTo, Kite | `arrivedCount` is 1 after arrival, still 1 after a displacement and return | 4 of 4 |
| Path | 7 waypoints in order, none skipped. `arrivedCount` 1 only at the last | 4 of 4 |
| Blocks | back-off holds Stand for 1000 ms. Three counted blocks in 10 s with no progress is Unreachable. Three blocks with 40 units of progress is not. Three blocks over 11 s is not. Excused blocks never count | 8 of 8 |
| Short stop | two retries, then accept inside the leash with `stopError`. Outside the leash it is a block | 4 of 4 |
| New order | radii change while standing inside: no Travel answer. Point change while Travelling: exactly one Stand answer, then Travel | 4 of 4 |
| Seqlock | a writer thread rewriting one slot 1e6 times against 4 readers: 0 mixed snapshots | 0 |

### 13.2 Dev gambit rows M-1 needs (MFO `selftest/TestsStrict.cpp`, through `DevHold`)

The rows and pass numbers of base plan section 10 stand, with these changes for the rulings and for what H-M1 can hold:

| Row | Change | Pass |
|---|---|---|
| `strict.stand_bow` R 64 and R 32 | none | inside the leash at least 97 percent of held time, max distance at most R + 15, shots per minute at least 0.7 of free fight |
| `strict.stand_melee` R 64 and R 32 | lunges are allowed now. "Inside percent" is no longer the measure | every excursion past the leash is followed by a travel pick within 300 ms (state `displacedCount` against the log), back inside `arriveR` within excursion distance over 60 u/s plus 1 s, attacks per minute at least 0.8 of free fight, 0 steps toward the foe that are not attack displacement (P1 split) |
| `strict.stand_melee_inplace` (new, after P-D) | `kMove_AttackInPlace` | inside at least 95 percent, max at most R + 20, attacks per minute at least 0.6 of free fight |
| `strict.stand_melee_far` | none | 0 steps toward the foe, inside 100 percent |
| `strict.flee` (new) | follower brought under the flee threshold (the test lowers health, any follower) while standing with a foe in contact | 0 Flee or Hide leaf acts (P1 histogram), `fleeDenied` above 0, still inside the leash rule of his row |
| `strict.goto` 5 legs | time is judged against backpedal speed whenever a foe is his target within 600 units, else run speed | arrived 5 of 5, stop error at most `arriveR + 10`, time at most 1.3 times path length over the judged speed plus 0.75 s |
| `strict.kite` | none | faces the foe at least 90 percent of travel time, at least 1 shot per 4 s, arrives 3 of 3 |
| `strict.disengage` | not in M-1 (H-M3) | |
| `strict.path` | none | each reached in order, none skipped |
| `strict.keep` | none | still inside, 0 path restarts, 0 calls after filing |
| `strict.release` | none | a fighting leaf act within 1 s, area geometry bit-equal to the engine's, no tree restart line |
| `strict.free` | none | leaf histogram top 3 equals an unclaimed fight |
| `strict.blocked` | none | Blocked inside 3 s, Unreachable after three, 0 EngineDropped |
| `strict.save` | none | no claim after load, engine area as saved, re-file works |
| `strict.refused` (new) | `[MoveOrder] bStrictOrders=0` | filing returns no handle, `CanServe` names the reason, the follower fights free |
| overall | | EngineDropped 0. Counted blocks under 5 percent of legs on trail points. 0 `[move]` anomaly lines |

Every row runs on any follower, anywhere: the test lends the weapon, takes points from his own trail, spawns and deletes its own foe.

## 14. What is unproven, the probe, and the behaviour until then

| # | Unknown | Probe | Until then |
|---|---|---|---|
| U1 | the hooks on the selector and link slots execute as read | P-A's first session: the identification line and the OBSERVED lines | nothing depends on it before P-A is deployed. No lines after 60 s of a live order in combat: the per-actor line says `selector never bracketed` and the phase stays Pending |
| U2 | exactly one Movement Selector node per session | the identification line, counted | cache replaced and logged on a second node |
| U3 | why the return path reaches status 5 in melee contact, and `path+0x24` | P2 (on `feat/move-probes`, one session) plus M3's own block line | repair, 1 s back-off, excused when the body is busy, three counted blocks end the claim |
| U4 | how much of the melee pull is attack displacement, and whether dodges matter | P1 (same session) and the SUMMARY line | lunges allowed, walked back. `kMove_NoDodge` refused |
| U5 | how close a completed return path leaves him to the centre | P-A SUMMARY (`stopError`, max dist while Standing) | short-stop rule, default `arriveR` 24 |
| U6 | what the selector's parents do when the return leaf fails (does one failure restart the Movement or Combat Parallel) | P-B session: ActionGate's Combat Parallel and Movement Parallel entry counters around each Blocked line | the back-off bounds it to one failure per second and three in total |
| U7 | whether the return leaf can start while an attack animation owns the body | P-B block lines with the body state | excused blocks |
| U8 | what Dodge does to the selector's child when it takes the Movement resource | P1 leaf histogram (Dodge acts against Idle and Return re-enters) | none needed: the selector re-enters and the pick is asked again |
| U9 | the aim holder between attacks (P3) | P3 on the gait line | GoTo in contact is a backpedal. Disengage refused |
| U10 | `g_moveSel` survives a load | the identification line after a load | pointer miss runs the structural test again |

## 15. What H-M1 cannot hold (residue, for the sim)

The sim models each row with a measured parameter, filled from the SUMMARY lines of the P-A to P-C sessions. Placeholders until then.

| Residue | Size | Sim parameter |
|---|---|---|
| Attack displacement out of the leash (default, by ruling) | UNPROVEN. One attack's displacement per attack, then a walk back at backpedal speed | `lungeOut` (distance per attack kind), `lungeShare`, `walkBackSpeed` |
| Pick latency | up to 0.25 s from crossing the leash to the travel pick, mean 0.125 s | `pickPeriod` 0.25 |
| Travel while a foe is his target | backpedal, RUNS 60 to 193 u/s against a foe at about 370. GoTo and Kite are the same until H-M3 | `kiteSpeed` |
| Stop error | UNPROVEN (U5). Bounded by `arriveR` or, after two short stops, by `leashR` | `stopError` |
| Stagger, knockback, ragdoll, paralysis, pushes | not denied. He is walked back after | `displaceReturn` |
| Dodge | allowed, cut at the leash by the engine's clamp | `dodgeInLeash` |
| Blocked legs | 1 s stand per block, three ends the order | `blockedShare`, `blockBackoff` 1.0 |
| Order start | none when the area is already current. Up to one area period (1.0 s vanilla) if the standard area is not current at filing, he stands meanwhile | `startDelay` |
| Re-point in flight | one pick (0.25 s) of standing per point change while travelling | `repointStall` |
| Path mode | he reaches each waypoint's centre before turning, no corner cutting | `pathTouchesCentres` |
| Flight, Exit Water, Essential Down | pass. While in water the engine may also flee | not modelled, except down time |
| Package hold-position area, low process, substrate hold, mount, scene | Yielded or NotCarried: the engine moves him | `yielded` = free fight |
| Disarmed weapon search (Find Weapon) | as today: the search is exempt from the bound, the path to the weapon is confined | not modelled |
| Facing | the engine's. He cannot turn his back (H-M3) | `canDisengage` false |
| Moving platforms | not supported | client rule |

## 16. For marth

One deviation needs a yes or no (ask it through AskUserQuestion with the merge decision, it does not block the code): a strict order on an actor who gets mounted, scened or package-held stays Yielded with no time limit, as every ch.24 claim does today, instead of ending after 10 s as the base plan said. The client sees the phase and its age and can release. Recommend: keep it as written here.

Find Weapon stays exempt from the bound under a strict order (the 2026-10-11 ruling APMF-B57 is left alone). Recommend: keep, revisit only if P1 shows followers fetching weapons out of the leash.
