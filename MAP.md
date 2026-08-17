# MFO — Architecture & Change-Impact Map

A **blast-radius map**, not a symbol index. Its job is to tell you what a change
*breaks* before you make it. Every non-trivial claim carries a `file:line`
citation so you can re-check it against the live code.

Complements the prose docs: `Docs/ARCHITECTURE.md` (design intent),
`Docs/INVARIANTS.md` (the 49 numbered rules — cited here as `#N`),
`Docs/ENGINE_NOTES.md` (proven engine mechanisms). This map is the
"what-depends-on-what" layer those don't carry.

---

## How to use this map

1. **Navigate by `file:line`.** Jump straight to the cited line; don't read
   whole files. Big files (Logistics 4050, Board 3278, ProgAllocator 1743,
   Packages 1542, CasterConsent 1062) should never sit in context — grep to a
   symbol, read a narrow window.
2. **Re-verify before editing.** Line numbers drift with every commit. Before
   changing a subsystem, re-read its "What breaks" entry *against current code*
   and **update this map if the structure moved.** A stale impact note is worse
   than none.
3. **`native/` only.** `imgui_impl_win32.*` is the only vendored code in
   `native/` — **do not read it.** No `extern/`; ImGui itself comes via vcpkg.
4. **When unsure a ripple is real,** entries say "UNVERIFIED — check before
   relying." Treat those as leads, not facts.
5. 28 real translation units + headers; ~24k lines. Startup wiring lives in
   `plugin.cpp` (read it first for the load order).

---

## High-blast-radius zones

| Zone | Where | Why it ripples / what breaks |
|---|---|---|
| **Co-save (3 records)** | `Serialization.cpp`, `Serialization.h`, `State.h` | FLWR `v4`, MSTK `v1`, PRGN `v2` (`Serialization.h:7-50`). Changing a field order/type/count, or bumping a version without a matching gated reader, **desyncs the byte stream and corrupts live saves**. A downgraded DLL destroys newer records (#12) — warned on-screen. |
| **Serialized string/ordinal contracts** | `Vocabulary.h`, `State.h` | Gambit opcode **strings** are persisted verbatim (#10); `Subject` enum and `CombatStyle::Stance`/`combatClassOverride` ordinals are persisted as raw bytes. Renaming an opcode or renumbering an enum is a **schema migration, not an edit** — old saves silently misread. |
| **`ResetAllState` teardown order** | `Serialization.cpp:562-622` | `StopPump()` MUST run first (`:568`) to drain the worker before any `clear()`; concurrent map insert+clear is UB. Every subsystem's `ClearTransientState`/`ClearAll`/`ReleaseAll` is ordered here. Reordering re-opens the load-screen-crash race. |
| **Alias fills / evict marker** | `Packages.cpp` | Alias fills at static priority 60 are **serialized into the `.ess`** (`plugin.cpp:302-322`). Missing/reordered `ReleaseAll` on kPreLoadGame / post-load / revert latches actors permanently across all descendant saves. The evict marker must stay a non-actor XMarker (base `0x3B`) or the **furniture-ejection bug** re-breaks (player forced into a package alias). |
| **The worker pump** | `Diagnostics.cpp` | One sleeper thread (`SleeperLoop`, 133 ms) drives the *entire* per-follower tick (`Scheduler::Tick`/`Loadout::Tick`/`Probe::Tick`) via `AddTask`. `kPumpMs` is the evaluator deadline, not a HUD constant. StopPump-before-clear is the linchpin invariant. |
| **Combat vfunc hooks** | `Targeting.cpp`, `CasterConsent.cpp`, `CombatStyle.cpp` | Three engine vtable hooks, install-once at `plugin.cpp:293-295`, VR-refused. Run on the **combat thread**. Any `CombatController` member touched there must be `< 0x68` (AE +8 layout bug; static_asserts). Signature mismatch corrupts every actor's combat/cast call. |
| **Frozen FormID / ESP contract** | `Forms.h` | Local FormIDs (`0x800`+) are a frozen contract with `MFO_GenerateESP.py`, audited by `tools/audit_esp.py` (#41). Changing one orphans every save that saw it; `0x802` stays reserved. |
| **Config INI keys** | `Config.cpp/.h` | Each key is wired in ~6 coupled places; the key **name** is the MCM-Helper persistence identity. Renaming unbinds the control; changing a key's *semantics* without renaming reinterprets the persisted value (the MEO ~100x-XP class of bug). |
| **External ABIs** | `MEO_API.h`, `TradeBridge.cpp`, `Papyrus.cpp` | `MEO_API.h` is a wire ABI shared byte-for-byte with a *separate* shipped MEO.dll (append-only). `TradeBridge`'s 10 Papyrus natives + `Papyrus.cpp`'s 3 method-name strings are called by shipped `.pex` — renaming breaks scripts silently. |

---

## Startup / teardown wiring (`plugin.cpp`)

The single source of truth for ordering. Everything below depends on it.

- **`SKSEPluginLoad`** (`plugin.cpp:377`): logs version (stale-binary guard #44,
  `:387`), installs the **Board's 3 render/input hooks** *before renderer init*
  (`Board::Install`, `:393` — only place they can go), registers the 3
  serialization callbacks (`:397-399`), the message listener (`:401`), and
  `TradeBridge::RegisterFuncs` via the Papyrus interface (`:406` — must be here,
  runs each VM init).
- **`kInputLoaded`** (`:262`): `Config::EnsureMcmDefaults()` — seeds the MCM
  store *before* MCM Helper reads it at its own kDataLoaded (bind-on-first-load).
- **`kDataLoaded`** (`:275`) — the once-per-launch install order (comment
  `:276`): `EnsureMcmDefaults` → `Config::Read` → `Forms::Resolve` →
  `Gait::Apply` → `Catalog::Load` → `Progression::Init` → `ProgAllocator::Init`
  → `Logistics::ComputeWeakPotionFloor` → `MEOBridge::Acquire` →
  `Followers::ResolveQuirks` → `MainThread::Install` → `Targeting::InstallHook`
  → `CasterConsent::InstallHook` → `CombatStyle::InstallEquipGate` →
  **sinks LAST** (`Rapport::RegisterSinks`, `Logistics::RegisterSinks`,
  `MEOBridge::RegisterSink`) → `Diagnostics::Install`. **Sinks must follow
  form resolution or they fire against unresolved forms.**
- **`kPreLoadGame`** (`:302`): `Diagnostics::StopPump()` then
  `Packages::ReleaseAll("kPreLoadGame")` — release the alias the engine would
  otherwise serialize into the outgoing `.ess`.
- **`kPostLoadGame`/`kNewGame`** (`:324`): warn if newer save (`:338`) →
  `Probe::ReleaseAll` → `Packages::EnsureEvictMarker` (**before** the reconcile)
  → `Packages::ReleaseAll("post-load reconcile")` → `Forms::EnsurePlayerSetup`
  → `Followers::Refresh` → seeds → `Logistics::SweepBeastHeadsOnLoad` →
  `Loadout::Reconcile` → `ProgProbe::OnPostLoad` → `ProgAllocator::OnPostLoad`
  → `Board::SetHud` → `Diagnostics::StartPump`. **Runs after the co-save loads**
  (ARCHITECTURE §9) or ledgers look empty.

---

## 1. Co-save & authoritative state — `Serialization.*`, `State.h`  ⚠️ HIGHEST BLAST RADIUS

**Responsibility.** Owns the SKSE serialization callbacks and the three
independent co-save records. `State.h` defines the authoritative in-memory
state (`g_followers`, `Gambit`, `FollowerState`).

**Three records (`Serialization.h`), each with its own version + reader:**
- **`'FLWR'` / `kSchemaVersion=4`** (`Serialization.h:8,50`) — per-follower
  `{rapport, rank, combatClassOverride(v4), tables[Combat,Logistics][], overrides[]}`.
  Written `SaveCallback` `Serialization.cpp:82`; read `LoadCallback` `:229`.
  Version history v1→v4 documented `Serialization.h:31-50`; v1 tutored-block
  reader kept forever (`Serialization.cpp:479`).
- **`'MSTK'` / `kStockVersion=1`** (`Serialization.h:13`) — Logistics'
  per-follower stock-gear sets; second independent record, never touches FLWR.
  Write `Serialization.cpp:187-217`, read `:239-313`. Owner: `Logistics.cpp`.
- **`'PRGN'` / `kProgVersion=2`** (`Serialization.h:26`) — progression state;
  layout + I/O live in `ProgAllocator.cpp` (`CoSaveSave` called `:226`,
  `CoSaveLoad` `:327`). Written **even when the addon ESL is absent** (`:220`).

**Ingestion discipline (INVARIANTS #8–#12), all enforced here:** every persisted
FormID passes `ResolveFormID` or is DROPPED (`:367,447,511`); runtime `0xFF` IDs
never written (`Followers::IsPersistableID` gate `:92,101,147,194,200`); string
opcodes not enum ordinals; every count bounded + short-read aborts + clamp at
ingestion (`kMaxFollowers=4096`, `kMaxOpcodeLen=64`, `kMaxOverrides=64`,
`kMaxStockGear=512`, `:50-54`).

**Version-gated reads (the desync trap).** New fields MUST sit behind
`if (version >= N)`: `combatClassOverride` gated `version>=4` (`:380`);
`subjectActorForm` gated `version>=3` (`:424`). A newer FLWR aborts the whole
load (`:338`, mid-stream desync risk); a newer MSTK/PRGN skips only that record
(`:247,320`). `g_sawNewerSave` → on-screen warning at kPostLoadGame
(`ConsumeNewerSaveWarning` `:555`, `plugin.cpp:338`).

**Load-time migrations to preserve:** `"act.equip_torch"` in the combat table is
redirected to logistics (#35, `:466`); an empty board is backfilled with
`Followers::ApplyDefaultKit` (`:538`); over-cap gambits are fully *consumed* but
not stored (`:473`, #22f) or the stream desyncs.

**`ResetAllState()` (`:562`) — the teardown-order contract (⚠️).** RevertCallback
and the load window both funnel here. Order is load-bearing:
`Diagnostics::StopPump()` **first** (`:568`, drains the worker) → `MainThread::Clear`
→ `g_followers.clear()` → `Followers::g_active.clear()` → each subsystem's
`ClearTransientState`/`ClearAll` (Followers, Scheduler, Logistics, +`ClearStockGear`,
ProgAllocator, Loadout, Targeting, CasterConsent, CombatStyle, Sightline, Board,
Packages `ReleaseAll("revert")`, Papyrus, TradeBridge, MEOBridge, Probe, Rapport)
→ `Board::SetHud(false)`. **What breaks:** move any clear out, or run it while the
pump is live, and you race a worker insert (UB). `Packages::ReleaseAll` here
re-reads the alias from the quest even when the module believes it holds nothing
(`:604-609`) — that independence is deliberate.

**`State.h` structures (the serialized shapes):**
- `Gambit` (`State.h:28`): `conditionOpcode`/`actionOpcode` (strings, serialized),
  `conditionParam`, `actionParamForm` (FormID, ResolveFormID on load),
  `subjectSelector`, `subjectActorForm` (#68, v3). `uid` is **runtime-only, never
  serialized** (`:23,29`) — the board keys edits on it, not row index.
  `lastFired*` display-only, never read back by the evaluator.
- `FollowerState` (`State.h:72`): `rapport`, `rank` (clamped [1,5]),
  `combatClassOverride` (v4; ordinals == `CombatStyle::Stance`), `tables[kCount]`,
  `overrides`.
- `g_followers` (`State.h:116`) — **MAIN-THREAD-ONLY, takes no lock (#4).** Keyed
  on persistent FormID; dismissed followers stay. Off-thread access must snapshot.
- Slot caps `kCombatSlotsByRank`/`kLogisticsSlotsByRank` (`:102-103`) — the
  default kit must fit Rank I (3 combat / 4 logistics) or a round-trip truncates it.
- **`overrides` / `PackageOverride` (`State.h:57,84`) is vestigial:** the only
  writer is the co-save loader (`Serialization.cpp:513`); no runtime code
  populates it (grep-confirmed). It round-trips but is inert — MFO drives packages
  via alias fills, not PapyrusUtil overrides (banned #18/#19).

---

## 2. Actuation core — `Packages.*`, `Actuation.*`, `Scheduler.*`, `Gait.*`, `MainThread.*`  ⚠️ ENGINE-DANGEROUS

The most engine-dangerous cluster: force-fills quest aliases (serialized into the
`.ess`), mutates shared `TESPackage` records, and pumps a state machine off engine
observation. All five files are **main-thread / serialized-queue only; none is
thread-safe.** `Scheduler::Tick` and `Packages::Pump` run on the **SKSE AddTask
job worker** (dispatched `Diagnostics.cpp:247`), the same serialized queue as
`Packages::NotifyCast` — which is why `g_holder` needs no lock.

### Packages.cpp / Packages.h — the alias/marker engine
Force-fills MFO's quest aliases at static priority 60 to run MFO's packages
(cast / loot-travel / retreat), observes engine state to advance a phase machine,
releases **by eviction** with a non-actor XMarker.

- `g_holder` (`Packages.cpp:163`) single-writer cast state (no lock);
  `g_liveStream` atomic mirror (`:173`) is the ONLY holder field the caster-thread
  consent hooks may read.
- `EnsureEvictMarker` (`:1125`) — caller `plugin.cpp:349` only, **before** the
  post-load reconcile. **What breaks:** if it stops minting, `EvictionRef` falls
  back to the PLAYER (`:884`) → furniture-ejection bug (v1.0.25/26) re-breaks.
  Must be main-thread (`PlaceObjectAtMe` mutates the cell) + force-persisted
  (`:1137`). Base-`0x3B` revalidation (`:877`) is load-bearing (handle indices
  rebuild per load).
- `ReleaseAll(why)` (`:1150`) — callers `plugin.cpp:321,355`, `Serialization.cpp:610`.
  The save-corruption backstop: sweeps all 4 loot aliases + retreat + command,
  evicting any actor occupant including the player (#48b) with the marker.
  **Ordering:** at kPreLoadGame runs after `StopPump` (can't race `Pump`); the
  player-sweep requires the marker (`haveMarker` guard `:1175`).
- `Pump()` (`:997`) — caller `Scheduler.cpp:135`, unconditional before every early
  return. Only advancer of `Requested→Filled→Running→Done`. `EvaluatePackage(true,
  false)` at `:1028` — **`resetAI` must stay false** everywhere (`:1028,1296,1319,
  1384,1462,1483,661`); `true` clears the combat group → zero-damage next hit.
  Timeouts `kFillTimeout=3.0`/`kRunTimeout=12.0` (`:126-127`).
- **SELF-CAST does NOT use the package (SPEC-self-cast-forced, superseded 2026-08-17).**
  Deck-proven: a no-QNAM/t6 package can be DELIVERED (equips the spell) but never
  TRIGGERS the cast — the QNAM + target-alias linkage is what drives the engine to
  EXECUTE the foe cast — and a package is DECLINED outright on package-locked custom
  followers (Lucien, prio-80 quest). So self routes through
  **`Actuation::CastSelfDirect`** (`Actuation.cpp`, public): equip via
  `Loadout::Prepare` → drive the caster's own state machine (`RequestCastImpl`) for
  the ANIMATION (§0.13: the only animated path) → `CastSpellImmediate` + hand-deduct
  for the effect + magicka (§5.3). Touches only the ACTOR, no alias → **follower-
  agnostic** (vanilla AND custom-framework). ONE-SHOT + cooldown-paced → the exact-
  bounding invariant holds with no held channel to leak; re-casts while the rule
  wins, stops when it goes false. Gated behind `Config::g_castSelf` (bCastSelf,
  default OFF). Callers: `CastOn` self-intercept (combat, BEFORE the concentration
  fork), `ConcentrationCast` self guard (defence-in-depth), `Logistics.cpp`
  `act.cast_self` branch (out-of-combat). The Logistics immediate path still SKIPS
  concentration spells legibly (self-gate-off / player) — instant-apply has no
  channel and stuck forever (deck 2026-08-17).
- **DEAD/superseded: the package self-route.** `Packages::CastSelf`, `Begin`'s self
  branch, command-quest **alias 2** (`kAliasCommandSelfActor` → `MFO_CastPackageSelf`,
  Forms `0x835`), `SetSelfSpell`, `HolderActorAlias`/`HolderPackage`'s self side, and
  the alias-2 `ReleaseAll` sweep are all still present but NO LONGER on the live path
  (nothing calls `CastSelf` with the gate on). Left in place (harmless; the ESP record
  never fills) rather than churn the frozen `0x835` FormID; can be removed later.
- `CastAt`/`Available`/`StreamLive` (FOE cast) — callers `Actuation.cpp` (ForceCast +
  ConcentrationCast foe), `CasterConsent.cpp:163` (reads the atomic mirror). One
  `MFO_CastPackage` on alias 0 → single holder forced by shared `TESPackage::refCount`
  (`:734`); multi-holder needs per-verb records at 0x821+.
- `LootTravelFill/Retarget/Clear/EvictIf`, `RetreatFill/Clear/EvictIf` (`:1265-1494`)
  — callers throughout Logistics/Scheduler + dismissal. **All release by eviction,
  never VM Clear** (scriptless aliases no-op a VM Clear); priority 60 is static and
  can't be lowered to release. `LootTravelRetarget` refills only the TARGET alias,
  leaving actor alias 0 filled (no hand-back between corpses).
- `ForceRefToNative` (`:266`) = `REL::ID(25052)` `TESQuest::ForceRefTo`, AE-only
  (VM path off AE). Two-class layout offsets (`kPointerOffFromIPackageData=0x10`,
  `:76`) + `kTypeTargetSelector`/`kTypeSingleRef` guard (`:88`, `ReadTarget` `:431`)
  are **memory-safety critical** — `SetInputs` (`:467`) writes nothing if guards fail.

### Actuation.cpp / Actuation.h — "a package IS the action"
Only module that mutates actor state; main-thread only. `Fire(follower, choice)`
(`:774`) dispatches one action/tick: Wait / Attack (→`Targeting::Command`) /
Cast{Self,Player,Target}→`CastOn` / Equip{Ranged,Melee} / Flee→`Packages::RetreatFill`
/ PowerAttack / drink / unknown→fail-closed. First-match-wins.
- `Outcome.transparent` (`Actuation.h:38`) is the fall-through contract the
  scheduler reads (`Scheduler.cpp:521`); default false = "wall" = safe. Flipping it
  changes suppression + hand-claim + spellsword fallback.
- `CastOn` (`:300`) escalation: AE-only gate (`:314`) → range/competence/reserve →
  concentration fork → equip + **AI-first grace** (`:461`, follower's own AI casts
  first) → on miss `ForceCast` (`:110`) via `Packages::CastAt`. Off-AE the whole
  path declines transparently (#67) so vanilla AI keeps casting.

### Scheduler.cpp / Scheduler.h — the tick / combat scan
Round-robin one follower per 133 ms tick (`kTickInterval` `:28`), pumps packages
first, runs combat OR logistics table (never both), owns suppression + retreat/loot
teardown. Runs on the AddTask worker.
- `Tick()` (`:110`) — caller `Diagnostics.cpp:255`. `Packages::Pump()` must stay
  first + unconditional (`:135`). Reads `g_followers` (`:190,429,564,663`) — safe
  only because StopPump brackets the load window. Retreating follower `return`s
  before the gambit table (`:348`) so a cast rule can't fight the retreat travel.
- `ClearTransientState` (`:96`) — caller `Serialization.cpp:581`; must run inside
  the StopPump bracket. Save-scoped maps: `g_recent` (suppression), `g_lastServiced`
  (round-robin cursor), `g_retreatNotes`, `g_combatEnteredAt`, `g_proposedTarget`.
- Casts `combatClassOverride` directly to `CombatStyle::Stance` (`:378,578`) — the
  ordinal-equality contract.

### Gait.cpp / Gait.h — travel-package speed byte (low risk)
`Apply()` (`:8`) copies `Config::g_travelGait` onto the loot-travel packages'
`preferredSpeed` + re-asserts the `0x2000` enable flag. **PACK data is NOT
serialized** — nothing outlives the session. Callers `Config.cpp:378`,
`plugin.cpp:284` (needs the second call because the first `Read()` precedes
`Forms::Resolve`, so the package is null). Writes MFO's own packages, not base NPCs.

### MainThread.cpp / MainThread.h — the real main-thread pump
Hooks the player's `Update` vfunc (idx `0x0AD`, `:68`) and drains a cross-thread
queue there — the ONLY safe road to the true main thread (SKSE `AddTask` drains on
a job worker here, not main).
- `Install()` (`:88`) — caller `plugin.cpp:291`, once. Writes
  `VTABLE_PlayerCharacter[0]` slot `0x0AD`; VR index differs → `g_dead=true`,
  `Post` becomes a no-op. Writing `0x0AD` on VR = instant CTD.
- `Post(fn)` (`:73`) — callers: Sightline LoS (`Sightline.cpp:112`), Rapport quash
  (`Rapport.cpp:394`), Logistics 3D/merchant/activate (`Logistics.cpp:1304,3082,
  3658,3977`), Board/ProgProbe/ProgAllocator hotkeys+polls. Callers that must still
  run on VR check `IsInstalled()` and fall back to a direct call.
- `Clear()` (`:79`) — caller `Serialization.cpp:572`; drops pending work whose
  captured handles would re-resolve against the next session's reused handle table.

---

## 3. Combat hooks — `CasterConsent.*`, `Targeting.*`, `CombatStyle.*`, `Sightline.*`, `CombatSense.h`, `Confidence.h`, `Temperament.h`  ⚠️ HOOK/ABI RISK

Three engine vtable hooks, install-once at `plugin.cpp:293-295`, `.exchange(true)`-
guarded, VR-refused. Thunk bodies run on the **combat thread**; latch/consent
writers run on the main thread or job worker. Teardown order fixed at
`Serialization.cpp:597-601`.

**Cluster-wide invariants:** (1) three install-once hooks, never per-load;
(2) teardown order Targeting→CasterConsent→CombatStyle→Sightline; (3) **§0.29 AE
layout rule** — any `CombatController` member touched on the combat thread must be
`< 0x68` (static_asserts in CasterConsent.cpp + CombatStyle.cpp); (4) Stance
ordinals `0/1/2/3` are a serialized ABI shared with `combatClassOverride`; (5) one
shared carrier hook — Targeting's `UpdateCombat` thunk drives BOTH targeting and
`CombatStyle::ApplyTick`/`AnyActive`, they can't be decoupled without a 2nd hook.

### Targeting.cpp — combat-target latch
Hooks `Character::UpdateCombat` (`VTABLE_Character[0]`, idx `0xE4`, `:110,142`).
- Thunk `void thunk(RE::Actor*)` (`:40`) — calls original first, only redirects
  when the engine ALREADY has a target (`:91`), never forces one in. Reads under
  `std::shared_lock`, fast-path `g_latchCount` atomic (`:52`). `idx=0xE4` is SE/AE-
  only (VR = different function = CTD, `:132`).
- `Command` (`:162`) callers `Actuation.cpp:818,890`; `Current` (`:171`)
  `Scheduler.cpp:484`; `Clear` (`:177`) `Followers.cpp:280`, `Rapport.cpp:315`;
  `ClearAll` (`:183`) `Serialization.cpp:597`, `Probe.cpp:344,446`.
- Co-writes `currentCombatTarget`/`combatController->targetHandle` (`:101`) with
  SmartNPCTargetSelector.dll if present (`g_conflict` logged, not resolved).
- **Also drives CombatStyle:** `AnyActive()` (`:51`) + `ApplyTick(a_this, cc)`
  (`:62`). Renaming/moving those stops stance re-assertion. `InstallHook` installs
  if EITHER `g_commandTarget` OR `g_weaponStyleControl` is on (`:122`).

### CasterConsent.cpp — cast control (magic twin of Targeting)
Hooks `CombatMagicCaster::CheckStartCast` (advisory, 14 vtables, idx `0x06`, `:906`)
and `MagicCaster::CheckCast` (hard gate, `VTABLE_ActorMagicCaster[0]` ONLY, idx
`0x0A`, `:872` — [1]/[2] are base-subobject vtables, patching them clobbers unrelated
engine vtables).
- Signatures (mismatch corrupts every actor's cast gate): CheckStartCast
  `bool(*)(CombatMagicCaster*, CombatController*)` (`:411`); CheckCast
  `bool(*)(MagicCaster*, MagicItem*, bool, float*, CannotCastReason*, bool)` (`:760`).
  Both dispatch to per-vtable original via `g_orig`/`g_castOrig` keyed on the vtable
  pointer; unrecognized vtable returns benign.
- **§0.29 guard:** reads the actor via `a_cc->attackerHandle` (0x28) ONLY, never
  `cachedAttacker`; static_asserts pin `attackerHandle==0x28 && <0x68` (`:251`).
- `ClassifySpell` (PUBLIC, `:22`) → `Actuation.cpp:239`. `Want` (`:939`) →
  `Actuation.cpp:273,459,533`. `WantedSpell` (`:933`) → `Scheduler.cpp:596` +
  `CombatStyle.cpp:268` (the equip gate's one exemption). `NoteCooldown` (`:1012`)
  → `Loadout.cpp:321`. `ClearTransientState` (`:1057`) → `Serialization.cpp:598`.
- **Latch lifetime:** the Want latch does NOT clear on cast (spans the whole
  combat); `Want` overwrites SPELL only, never `permitAfter` or pacing breaks.
  Force-YES must NEVER apply to concentration spells (permanent-stream freeze,
  `:579`). Fast-out needs ALL THREE `g_wantCount==0 && g_styleSwapCount==0 &&
  g_ctrlCount==0` (`:434`). `WouldHitTeammate`'s `highActorHandles` walk is
  main-thread-gated (`:848`), fails open off-main (§0.30 crash).

### CombatStyle.cpp — weapon stance + equip gate (#75)
Owns a follower's live per-combat `CombatController::combatStyle` (0x38); gates
`CombatInventoryItem::CheckShouldEquip` (idx `0x0F`, 30 template vtables, `:311`) so
the AI can't re-arm magic over a forced weapon.
- `enum Stance {None=0,Melee=1,Ranged=2,Cast=3}` (`CombatStyle.h:36`) — **ordinals
  are a serialized ABI**: equal to `State.h:82 combatClassOverride`, written raw
  (`Serialization.cpp:106`), read v4 (`:380`), cast directly by Scheduler
  (`:378,578`). Renumbering corrupts every saved override → **requires a
  serialization version bump.**
- `ApplyTick` (`:112`) is NOT a hook — called by Targeting's thunk; writes through
  the live controller, never dereferences the stored pointer (identity-compare only).
  Equip-gate static_asserts pin `combatStyle==0x38<0x68`, `attackerHandle==0x28`,
  `CombatInventoryItem::item==0x10` (`:17,196,206`). The 30-vtable list deliberately
  EXCLUDES weapon/potion/scroll/shout (`:305`) — widening it denies combat drinking
  (v1.0.32 lesson).
- `Want` (`:76`) → `Scheduler.cpp:378,380,612`; `Clear`/`ClearAll` →
  `Followers.cpp:285`, `Serialization.cpp:599`. Only a WEAPON stance can be an
  equip order (`:92`); class-override (#65) + magicka-dry fallback deliberately do
  NOT engage the gate (must not mute the AI's own magic). Reads
  `Forms::g_meleeStyle/rangedStyle/castStyle` (`:53`) — missing form → stance
  silently disabled.

### Sightline.cpp — line-of-sight split across threads (NOT a hook)
Raycast runs only on the main thread, results cached, worker reads the cache.
- `Measure` (`:52`, MAIN THREAD ONLY) calls `HasLineOfSight` (RELOCATION_ID
  53029/53829) inside `MainThread::Post` (§0.30 crash class off-worker). `Check`
  (`:93`, worker-safe cache read) → `Actuation.cpp:127,149,234`, `Evaluator.cpp:307`.
  `Want` (`:101`) → `Evaluator.cpp:317`. `g_mx` is a strict LEAF (nothing called
  while held). Fail-open by design (cold/stale/VR → Unknown).
- `TeammateInFireLine` (`:149`) → `Actuation.cpp:247`, `Packages.cpp:990`. Reads
  `Followers::g_active` UNGUARDED (`:168`) — documented as joining an existing
  tolerated pattern. **UNVERIFIED — check before relying** if `g_active` is ever
  rebuilt reallocating concurrently with a worker read.
- `SegDist` (`:129`) is a private copy of CasterConsent's `SegDist` (`:687`) —
  **two copies; a math fix must hit both.**

### CombatSense.h / Confidence.h / Temperament.h (header-only)
- `CombatSense::FoeCount(Actor*)` (`:15`) — canonical live-foe count from the
  follower's own combat group, under `BSReadLockGuard(combatGroup->lock)`. Consumed
  by `Confidence.h:48`, `Evaluator.cpp:81`, `Scheduler.cpp:276`. A semantic change
  shifts confidence + auto-retreat + the foe-count gambit at once (no recompile
  firewall). **Note:** `Confidence.h` is the combat/loot **leash** primitive
  (`Of`/`LeashRadius`/`ChaseRadius`) — misfiled under "progression" by directory
  adjacency; zero relationship to perks/PRGN.
- `Temperament(FormID)` (`:22`) — deterministic per-follower scalar (Knuth hash).
  Consumed by Actuation/Scheduler for cadence deviation (`Actuation.cpp:255,483`,
  `Scheduler.cpp:394,685`). No state/save impact; changing the hash silently
  re-rolls every follower's "personality." 

---

## 4. Logistics / upkeep — `Logistics.*`, `Loadout.*`, `ItemCatalog.*`

Out-of-combat supply/upkeep. `Logistics::ServiceFollower` + its whole loot/heal/
economy tree run on the **BSJobs worker**; 3D mutations marshalled to main via
`MainThread::Post`. Owns the serialized `g_stockGear` ('MSTK') map.

### Logistics.cpp / Logistics.h
- **SAVE-COMPAT — `g_stockGear`/'MSTK'** (`Logistics.cpp:1936`, guarded `g_stockMx`
  `:1935`, the one cross-thread map here): the only serialized state the cluster
  owns. `CopyStockGear` (`:4035`) → `Serialization.cpp:191`; `LoadStockRecord`
  (`:4040`) → `:307`; `ClearStockGear` (`:4045`) → `:231,587`. Only
  `IsPersistableID` FormIDs written, sets capped 512, unresolvable IDs dropped.
  Changing the map's key/value shape or record framing breaks the shed-protection
  ("Gauldurbow fix") — signature gear could get shed to the player after a load.
  **Not** cleared by `ClearTransientState` — cleared separately by `ClearStockGear`.
- `ServiceFollower` (`:3429`) — sole caller `Scheduler.cpp:225` (worker). Sets
  `g_svc` (`:840`) raw pointer valid only for that call — safe only because the
  worker services followers sequentially; parallelizing dangles it.
- `ClearTransientState` (`:3983`) → `Serialization.cpp:582`, after StopPump. Wipes
  the loot/drink/econ/travel maps (calls `Packages::LootTravelClear` first). Moving
  a clear out, or calling while the pump is live, races a worker insert (UB).
- Pure reads (evaluator + economy, shared classifiers): `PotionRestores` (`:3135`),
  `AmmoIsBolt` (`:3191`), `CountPotions`/`ArrowCount`/`BoltCount` (`:3233-3247`) →
  `Evaluator.cpp:397-409` + `TradeBridge.cpp:52-71` (buy side shares them so bought
  supply matches looted). `ComputeWeakPotionFloor` (`:3205`) ← `plugin.cpp:288`
  (after `Catalog::Load`).
- **Alias/travel:** `g_travelSlots` (`:1638`, `kMaxLootSlots=4`) maps follower→loot
  alias pair. Travel fill is **engine-serialized**; every exit path MUST call
  `Packages::LootTravelClear` (combat via `ReleaseTravelOnCombat` `:4005` ←
  `Scheduler.cpp:236`; cap/leash/dismissal/revert). Leash hysteresis guards
  (`followerBeyondLeash` `:2528`, ×1.15 `:3522`) prevent the ~1/sec claim/evict churn.
- **Sinks** (`RegisterSinks` `:3944` ← `plugin.cpp:297`): `ContainerSink`
  (`TESContainerChangedEvent`) — **direction filter mandatory** (`newContainer==
  PlayerID()` `:3107`) or it re-fires on its own removal (MAO infinite-credit loop);
  only QUEUES to the worker. `BeastHeadSink` (`TESEquipEvent`, `Config::g_beastHeadFix`)
  → `KeepHeadClear`. `SweepBeastHeadsOnLoad` (`:3963`) ← `plugin.cpp:360`.
- `OnFollowerRemoved` (`:4018`) ← `Followers.cpp:306` (dismissal alias eviction).
- Hardcoded base FormIDs (stable): Gold `0x0F`, Lockpick `0x0A`, player `0x14`,
  house loc types, PlayerFaction — resolved/used throughout.
- Economy probe (`EconomyProbe` `:2834`, worker, `Config::g_economy && Po3Present`)
  pushes the actual merchant read to Papyrus via TradeBridge — native `GetInventory`/
  `GetGoldAmount` CTD on merchant chests. **Do not** move the read back to native.

### Loadout.cpp / Loadout.h — the equip/spell-in-hand ledger (NOT serialized)
Puts a gambit spell in a follower's hand, records displaced gear as **transient
debt**, hands it back on hit/combat-end/dismissal. Reconstructed from live state on
load — no co-save record.
- Five deliberately-separate main-thread-only maps (`:9-57`): `g_debt`, `g_lastStow`,
  `g_equipClock`, `g_coolUntil`, `g_mfoSpell` — merging them re-introduces named
  regressions.
- `Prepare` (`:156`) → `Actuation.cpp:447-538`. `StartCooldown` (`:309`) → Actuation
  + Diagnostics; **mirrors into `CasterConsent::NoteCooldown`** (`:321`) so the combat
  thread reads the mirror, never these non-atomic maps. `Tick` (`:361`) ←
  `Diagnostics.cpp:256` (settles debts). `Reconcile` (`:457`) ← `plugin.cpp:361`
  (undo a save taken mid-cast; iterates `g_active`, main-thread). `ClearTransientState`
  (`:521`) ← `Serialization.cpp:595`.
- **Collect-then-act** (`:381`): `EquipObject` dispatches synchronous events;
  mutating `g_debt` mid-iteration is the MEO use-after-free shape. `DeselectSpell`
  (not `UnequipObject`) is load-bearing for spells.

### ItemCatalog.cpp / ItemCatalog.h — patcher JSON lookup (pure read, no save)
Loads `Data/SKSE/Plugins/MFO/mfo_items.json` (written by the MFO.Synthesis patcher)
into resolved-FormID tables. `Catalog::Load()` (`:57`) — sole caller `plugin.cpp:285`,
**must run at/after kDataLoaded** (rows resolve via `LookupForm` after load-order
offset) — before that, silent empty catalog. Fail-open: callers OR the catalog with
their own heuristic; making an accessor authoritative-on-miss re-opens the #20 bug.
`IsExcluded` widely used (Logistics + `TradeBridge.cpp:141`). **Dead API:**
`Loaded()`, `PotionCures/CuresPoison/CuresDisease` have zero call sites (the
cure-poison gambit consumer doesn't exist yet).

---

## 5. Progression / perk-economy — `ProgAllocator.*`, `Progression.*`, `ProgProbe.*`

Optional ESL addon (`MFO_Progression.esl`); inert if absent. Owns the third co-save
record 'PRGN'. Main-thread-only.

### Progression.cpp / Progression.h — component 1: the catalog reader (read-only)
One pass at kDataLoaded over merged AVIF perk trees → a value-only frozen `Catalog`.
Mutates nothing. `Init()` (`:588`) ← `plugin.cpp:286` (immediately before
`ProgAllocator::Init` — order load-bearing). `Get()` read by ProgAllocator (`:143,
150,437,666,935,1047,1264,1554`) + `Board.cpp:1333`. `kAddonPlugin=
"MFO_Progression.esl"` (`:30`). **What breaks:** the catalog is the load-time drop
oracle — `CoSaveLoad` drops any perk alloc whose node is no longer in `Get()`
(`ProgAllocator.cpp:1617`), so re-tuning `kEntryPoints[]` (`:64`) or `ClassifyRank`
(`:300`) silently changes which saved perks survive a load (with an auto §17 refund).

### ProgAllocator.cpp / ProgAllocator.h — component 2: allocator + 'PRGN' owner  ⚠️ SAVE-LAYOUT
The engine-mutating half: writes perks (`AddPerk/RemovePerk`+`ApplyPerksFromBase`)
and skill AVs onto real actors, runs the level poll, owns 'PRGN'.
- **SAVE-COMPAT — `CoSaveSave` (`:1472`) / `CoSaveLoad` (`:1535`) exact order** (any
  field-order/type/version change corrupts live saves): header `{lastPlayerLevel u16,
  count u32}`; per follower `{formID u32, flags u8 (bit0 enrolled…bit4 manualSkills
  v2), cls u8, progressionLevel u16, sharedGrowthRemainder u16, [v1-only unspentPerk
  f32 read+discarded], [v2: manualBaselineLevel u16, manualPointsApplied u16,
  manualExcludedLevels u16, nativeTreePerksAtEnroll u16], perkCount u16 +
  {nodePerkID u32, rank u8}×N, skillCount u16 + {av u32, points f32, lastWrittenBase
  f32, manualPoints f32(v2)}×N, baseCount u16 + {av u32, value f32}×N}`. Bounds:
  4096 followers / 1024 perks / 64 skills. New fields MUST go behind `if(version>=N)`
  (v2 at `:1584,1642`). The docstring `:1447` is commentary — the code is authority.
- Version guard `Serialization.cpp:320` (newer PRGN skips this record only).
  `CoSaveSave` has no `g_ready` gate — writes even when the addon is disabled so the
  data survives a session without the ESL.
- Lifecycle: `Init` (`:967`) ← `plugin.cpp:287` (early-returns `g_ready=false` if
  `!Progression::Detected()`); `OnPostLoad` (`:1013`) ← `plugin.cpp:363` (after the
  co-save load); `ClearAll` (`:1676`) ← `Serialization.cpp:591` (clears `g_prog`,
  bumps `g_pollGen` to orphan in-flight polls).
- Verbs (all ← Board.cpp, `g_ready`-gated): `Enroll`/`SetClass`/`AllocatePerk`/
  `Respec`/`SetManualSkills`/`ApplyManualSkillPoint` (`:1110-1396`).
- **Actor-write safety:** perk reapply is idempotent — re-adds a rank only if
  `GetPerkIndex` absent (`:586`) + native-ownership deferral (`:598`, if another mod
  granted a rank, MFO touches nothing). Skill writes funnel through the single
  `ReconcileSkill` (`:239`) with the enrollment baseline as a **hard floor** — a
  shrink can never write below the follower's captured natural. `IsKnownSkillAv`
  (`:117`) validates the raw AV value before Get/SetBaseActorValue (OOB guard).
- **`Class` enum ordinals (`:84`) MUST stay == `combatClassOverride`** — `SetClass`
  mirrors into it via `Followers::TryEnsureRecord` (`:1204`). Board snapshot is the
  one cross-thread structure (guarded `g_viewMx`); `Rapport::Spend` (`:1346`) is a
  cross-module write on respec.

### ProgProbe.cpp / ProgProbe.h — throwaway field probe (NOT serialized)
Dev-only (`bProgProbe`, INI, default OFF) log probe. `OnPostLoad` (`:444`) ←
`plugin.cpp:362`; `OnHotkey` (`:436`) ← `Board.cpp:2672`. Writes no save record;
its perk/AV mutations are runtime-only. Safe to delete without touching saves; only
`plugin.cpp:362` + `Board.cpp:2672` reference it. Idempotent reapply guarded on
`GetPerkIndex` (`:468`).

---

## 6. Board / UI / Papyrus — `Board.*`, `Papyrus.*`

### Board.cpp / Board.h — the Field Kit overlay
Installs three trampoline hooks at plugin load, draws live state via ImGui on the
**render thread** from a mutex-guarded snapshot, funnels all rule edits through a
main-thread-drained edit queue. **ImGui/`imgui_impl_win32` = vendored, do not read.**
- `Install()` (`Board.h:113`) — caller `plugin.cpp:393` only. **MUST** install
  before renderer init (only place, `plugin.cpp:391`); moving it → `D3DInitHook`
  misses init → `g_ready` never set → overlay silently disabled. Writes a 256-byte
  trampoline with 3 game-version-keyed `RelocationID`/offset pairs (`Board.cpp:3271`)
  — a bad offset corrupts the call site.
- **Thread discipline:** `DXGIPresentHook` (render thread) copies `g_snapshot` under
  `g_snapMx` **before** taking `g_ioMx` (`:2596`); reversing = render-thread deadlock.
  The two mutexes are never nested (#6). Draw functions **never touch `g_followers`**
  — every mutation is a `QueueEdit` (`:175`, 21 sites) drained by `ApplyEdits`
  (`:2934`). **Rule edits key on `Gambit.uid`, not row index** (`:3030` — resolve by
  identity #31); applying by index misapplies a command to the wrong rule.
- **Correction to the header:** `PublishSnapshot` (`Board.h:116` says "MAIN THREAD
  ONLY") actually drains on the **task worker** (`:2662,3155`), the same context that
  owns `g_followers`/`Scheduler::Tick` — so its `g_followers` reads are safe *there*.
  But `Prog*` edit verbs ride `MainThread::Post` (`:2956`) because `g_prog` lives on
  the real main/poll thread. Callers: `Diagnostics.cpp:216,263`.
- `ClearPendingEdits` (`Board.h:124`) ← `Serialization.cpp:603` (revert) — drops
  queued edits so a command from the old save can't hit a freshly loaded one.
  `SetHud` ← `plugin.cpp:365`, `Diagnostics.cpp:94`, `Serialization.cpp:621`.
  `IsOpen`/`IsAvailable`/`Toggle` ← Diagnostics (publish cadence + Field Orders
  power). `ToggleHud` (`Board.cpp:2861`) is **dead** (no caller).

### Papyrus.cpp / Papyrus.h — outbound VM dispatch shim
Reaches Papyrus-only natives by class-name+method-name string, async fire-and-forget.
Registers NO natives (that's TradeBridge). The **three method-name strings are the
script-compat surface:** `"Actor"/"DoCombatSpellApply"` (`:70`), `"ObjectReference"/
"Activate"` (`:104`), `"MFO_Trade"/"RunTrade"` (`:126`) — changing the `.psc` native's
name/arity breaks dispatch silently (bumps `g_failures`). `DoCombatSpellApply` (`:41`)
← `Actuation.cpp:561` (gated on `g_commandCast`+`Available()`). `DispatchTradeRun`
(`:111`) ← `TradeBridge.cpp:266`. `DispatchActivate` (`:77`) has no live caller
(latent). `ClearTransientState` (`:136`) ← `Serialization.cpp:611`. `HandleFor` checks
`policy->EmptyHandle()` not `0` (`:29`) — a real correctness point.

---

## 7. External bridges / probes / diagnostics — `MEOBridge.*`, `MEO_API.h`, `TradeBridge.*`, `Probe.*`, `Diagnostics.*`

### MEO_API.h — FROZEN cross-mod ABI ⚠️
The v1 SKSE inter-plugin C++ header shared **byte-for-byte with a separate shipped
MEO.dll.** `kABIVersion=1` (`:35`), `kMessage_RequestInterface=0x4D454F41` (`:34`),
`kPluginName="MEO"` (`:33`), POD `GemInfo` (`:39`), `class IMEO` vtable Version/
GetSocketCapacity/GetActorGems/MoveGems (`:55-75`). **Append-only** (`:54`); reorder/
insert/resize breaks both DLLs. Threading is part of the ABI (queries main-thread,
`MoveGems` any-thread). Only MEOBridge.cpp includes it.

### MEOBridge.cpp / MEOBridge.h — optional MEO gem-transfer adapter
Rides a follower's socketed gems onto looted gear on upgrade. Fully optional.
`Acquire()` (`:61`) ← `plugin.cpp:289` (nullptr on absence/ABI-mismatch).
`RegisterSink()` (`:73`) ← `plugin.cpp:298` (equip sink — must stay with the other
sinks or moves never flush). `QueueGemMove`/`WornUid`/`Available` ← `Logistics.cpp:
1208,1236,1361` (worker tick). `PreviewWithGems` (`:105`, main-thread queries) has no
live caller (**UNVERIFIED** — check Board before removing). `g_pending` keys on
`(followerFormID<<32|toBase)`; if `ClearTransientState` (`:100` ← `Serialization.cpp:
616`) stops being called on revert, a reused FormID next session moves gems onto the
wrong actor.

### TradeBridge.cpp / TradeBridge.h — Papyrus econ bridge (#21) ⚠️ SCRIPT-COMPAT
Native owns the trade DECISION; merchant read/mutation runs in `MFO_Trade.psc`
(native `GetInventory`/`GetGoldAmount` CTD on merchant chests). `RegisterFuncs()`
(`:207`) ← `plugin.cpp:407`, registers **10 Papyrus natives** on class `MFO_Trade`
(`:209-218`) called by the shipped `MFO_Trade.pex` — renaming/re-signing any breaks
trading silently. `VendorTrade` (`:223`) ← `Logistics.cpp:2960`. `SellRow`/`NeedCat::
Kind` (`TradeBridge.h:25,35`) are the wire vocabulary with Logistics. Cross-save
safety: per-chest in-flight guard (`:250`) + `ClearTransientState`'s `g_nextToken +=
1'000'000` jump (`:282` ← `Serialization.cpp:612`) so a resumed stale token can't name
a fresh order.

### Diagnostics.cpp / Diagnostics.h — event sinks + THE WORKER PUMP ⚠️ RACE LINCHPIN
Owns the one persistent sleeper thread driving the per-follower tick, four event
sinks, and `DumpReport`. `Install()` (`:394`) ← `plugin.cpp:299` (registers
SpellSink/HitSink/MenuSink + Probe crosshair sink). `SleeperLoop` (`:236`, `kPumpMs=
133`, `kDiagEveryNth=4`) never touches game state directly — only `AddTask`s a lambda
that re-checks the epoch, sets `TickActiveGuard`, then runs `Followers::Refresh`
(diag turn), `Scheduler::Tick`, `Loadout::Tick`, `Probe::Tick` (diag turn),
`Board::PublishSnapshot` (`:247-263`).
- **StopPump-before-clear invariant:** `StopPump()` (`:409`) is the FIRST statement
  in `ResetAllState` (`Serialization.cpp:568`) and runs at kPreLoadGame
  (`plugin.cpp:320`). It clears `g_pumpRunning`, bumps `g_pumpEpoch` (strands mid-
  sleep threads), then spin-waits ≤2000 ms on `g_tickActive` (`:413`) so any in-
  flight tick finishes before the maps are wiped (concurrent map insert+clear = UB).
  Reordering it after the clears, dropping the `g_tickActive` drain, or letting a
  sink/tick mutate save-scoped maps unguarded re-opens the load-screen crash (loud
  error `:429`). **`kPumpMs` is the evaluator deadline** — changing it re-times the
  scheduler, not just diagnostics. `DumpReport` uses `find()` not `operator[]`
  (`:365`) to avoid persisting a spurious `0xFF`-keyed record.

### Probe.cpp / Probe.h — M4 debug/research harness
Fires one engine primitive at a follower and records emergent behavior; nothing
outlives a session. `ReleaseAll()` (`:445`) ← `plugin.cpp:344`, `Serialization.cpp:617`
— must keep calling `Targeting::ClearAll()`+`Stop()` or a stale latch/watch handle
survives. `Tick()` (`:368`) ← `Diagnostics.cpp:258` (main-thread task). **Real
gameplay dependency (not debug-only):** `CrosshairTarget()` (`:307`) ← `Logistics.cpp:
1897` (QuickLoot-aware player-claim signal). `FocusOnCrosshair()` ← `Board.cpp:2654`.
`StartCombatOn` uses po3 RelocationID(37608,38561), VR-refused. Most `Fire`/`GetLast`/
enum entry points have **no in-tree C++ caller** (the ImGui probe panel isn't wired) —
**UNVERIFIED — check Board before assuming dead.**

---

## 8. Core state / evaluator / config / forms — `Followers.*`, `Rapport.*`, `Evaluator.*`, `Vocabulary.h`, `Config.*`, `Forms.*`

Shared foundation. `g_active`/`g_activeIds`/`g_followers` are **main-thread / serial-
SKSE-task only**; combat-thread hooks go through FormIDs + cached reads, never these.

### Followers.cpp / Followers.h
- `g_active` (`Followers.h:33`) + parallel `g_activeIds` (`:39`) — read in ~15 files
  (plugin, Rapport, Evaluator, Scheduler, Sightline, Actuation, Loadout, Board,
  Logistics, Diagnostics, ProgProbe, ProgAllocator, Probe). Raw non-atomic vector on
  the serial task — off-thread access is a race. **`g_activeIds` must stay index-
  aligned with `g_active`** (`Rapport.cpp:158`, `Scheduler.cpp:167`) or the "kill
  credited nobody" bug (#51/F6) re-opens.
- `IsPersistableID` (`:176`, `(id>>24)!=0xFF`) — the **single authority** on what
  reaches co-saves (#9), used across Serialization/ProgAllocator/Logistics/ProgProbe.
  Loosening lets `0xFF` runtime IDs into the `.ess`; tightening drops legit followers.
- `TryEnsureRecord` (`:190`) vs `EnsureRecord` (`:205`): Rapport/ProgAllocator use
  `TryEnsureRecord` because `g_active` holds `0xFF` cloned teammates; `EnsureRecord`
  would mint a doomed record SaveCallback skips (F2).
- `ApplyDefaultKit` (`:24`) — 3 combat + 4 logistics; called on record creation
  (`:198`) and as the empty-board load backfill (`Serialization.cpp:539`). **Verified
  to fit Rank I** (3/4); adding a rule overflows and the load clamp silently drops it.
- `Refresh` (`:211`, the eviction hub) — on drop calls `Loadout::Restore`,
  `Targeting::Clear`, `CombatStyle::Clear`, `CasterConsent::Clear`, `Packages::Release`,
  `Logistics::OnFollowerRemoved`, `Packages::RetreatEvictIf` (`:275-309`). Several
  release engine-serialized alias fills — removing any leaves a latch that re-fills on
  every future load. Callers `plugin.cpp:357`, `Rapport.cpp:147`, `Diagnostics.cpp:253`.

### Rapport.cpp / Rapport.h
Per-follower rapport/rank + death/combat sinks + the ally-combat quash.
`RegisterSinks` (`:505`) ← `plugin.cpp:296` (LAST, after Config/Forms/Quirks resolve).
Sinks QUEUE, never act inline (#1): `QuashAllyPair` (`:336`) MUST defer `StopCombat`
off the TESCombatEvent dispatch (`:393`) or it's the v1.0.53 hard-freeze; backstop
`Scheduler.cpp:294`. `Award` (`:434`, scaled by `g_rapportRate`) / `Spend` (`:481`,
NOT scaled) main-thread; `Spend` external caller `ProgAllocator.cpp:1346`. `RankFor`
(`:426`) reads `g_rank2..g_rank5` atomics — the sole rapport→slot map. `ResetSessionCounters`
(`:407`) ← `Serialization.cpp:619`.

### Evaluator.cpp / Evaluator.h
Gambit condition/action evaluator: scan a table top-down, **first true wins, PURE
READS (#23).** `Evaluate` (`:436`) ← `Scheduler.cpp:432` (combat) + `Logistics.cpp:
2716,2763,3757`. `Choice` (`Evaluator.h:11`: `actionOpcode`, `actionParam`, `subject`,
`subjectActorForm` #68, `target`) is the ABI to `Actuation::Fire`. The `a_startIndex`
resume contract (`Evaluator.h:41`) prevents a near-always-true rule shadowing rules
below. Opcode dispatch is entirely string-compare vs `Vocab::` constants; unknown →
false (fail-closed). **Threading landmine:** `ChaseRadius` is hoisted OUT of the
`combatGroup->lock` (`:203`) to avoid a nested read-lock deadlock — do not move it back
in. Player-HP special-case gated to `kCondPlayerHpBelow`+`kActCastTarget` only (`:474`,
un-gating was friendly-fire).

### Vocabulary.h — the serialized opcode contract ⚠️
The gambit opcode **strings are a frozen co-save contract (#10)** — written verbatim
(`Serialization.cpp:128,149`), read (`:414,439`); there's even a hard literal
`"act.equip_torch"` (`:466`) tracking `kActEquipTorch`. Renaming any `kCond*`/`kAct*`
is a **schema migration, not an edit** (old saves carry the old string; the `==`
compares silently stop matching). Adding an opcode requires wiring in Evaluator +
Actuation + Board's picker or it's inert. **`Subject` enum** (Self=0/Player=1/
NearestAlly=2, `:37`) is serialized as the raw `subject` byte (read `Actuation.cpp:73`,
`Board.cpp:2909`) — reordering reinterprets every saved byte (a specific follower is
carried as `subjectActorForm`, NOT an enum value, precisely to keep the enum frozen).
`Pct`/`HealthPct`/etc. (`:214`) use permanent+temporary AV — changing the max formula
re-times every "HP below X%" rule + Confidence.

### Config.cpp / Config.h
~90 `g_*` atomics read across 22 files (cross-thread-safe by design). `Read` (`:363`,
ResetToDefaults → seed INI → MCM INI, last wins; runs `Gait::Apply` after) ←
`plugin.cpp:282` + MCM-close MenuSink. `EnsureMcmDefaults` (`:290`) ← `plugin.cpp:272`.
**Each INI key is wired in ~6 coupled places** (atomic decl, `Apply` branch,
`ResetToDefaults`, `kMcmDefaults`, both INIs, config.json). The key **name** is the
MCM-Helper persistence identity: renaming unbinds the control; a key whose *semantics*
change must be RENAMED or MCM Helper reinterprets the stale value (the MEO ~100x-XP
bug). Parse safety (reset-then-parse, clamp-at-parse, skip-unparseable, strip BOM) is
load-bearing — removing any re-opens a silent-zero bug. **UNVERIFIED:** sync of
`kMcmDefaults` (`:291`) with `out/MCM/Settings/MFO.ini` + `config.json` is asserted by
comment, not independently checked here.

### Forms.cpp / Forms.h — FormID resolution + Field Orders grant ⚠️ FROZEN IDs
Frozen local FormIDs (`Forms.h:21-57`, `0x800`+) are a contract with
`MFO_GenerateESP.py`, audited by `tools/audit_esp.py` (#41); changing one orphans
every save that saw it; `0x802` stays reserved. `Resolve` (`:27`) ← `plugin.cpp:283`
(after Config, before Quirks/sinks) — returns false only if `g_fieldOrders` missing;
else degrades to one log line, never a crash. Resolved globals (`g_commandQuest`,
`g_castPackage`, `g_lootQuest`, `g_travelPackage[1-3]`, `g_retreatQuest/Package`,
`g_tradeQuest`, `g_meleeStyle/rangedStyle/castStyle/probeCastStyle`) are resolved once
on the main thread specifically so combat-thread hooks only READ settled pointers —
never do a data-handler lookup from those hooks. `EnsurePlayerSetup` (`:96`) grants the
Field Orders power (idempotent via `HasSpell`) ← `plugin.cpp:356` (kPostLoadGame only,
after co-save loads); must NOT latch a failed grant (`:100`) so a missing ESP retries.

---

## Appendix — SKSE registration & sink inventory (the wiring choke points)

| Registration | Site | Notes |
|---|---|---|
| Serialization Save/Load/Revert callbacks | `plugin.cpp:397-399` | `kSerID='MFO0'` |
| Message listener | `plugin.cpp:401` | drives the whole lifecycle |
| Board 3 trampoline hooks (D3DInit/DXGIPresent/InputDispatch) | `plugin.cpp:393` → `Board.cpp:3269` | before renderer init |
| `MainThread::Install` (player Update vfunc 0x0AD) | `plugin.cpp:291` | true main-thread pump |
| `Targeting::InstallHook` (Character::UpdateCombat 0xE4) | `plugin.cpp:293` | also drives CombatStyle |
| `CasterConsent::InstallHook` (CheckStartCast 0x06 + CheckCast 0x0A) | `plugin.cpp:294` | 14 + 1 vtables |
| `CombatStyle::InstallEquipGate` (CheckShouldEquip 0x0F) | `plugin.cpp:295` | 30 template vtables |
| `Rapport::RegisterSinks` (TESDeath, TESCombat) | `plugin.cpp:296` → `Rapport.cpp:511` | sinks LAST |
| `Logistics::RegisterSinks` (TESContainerChanged, TESEquip) | `plugin.cpp:297` → `Logistics.cpp:3950` | direction filter mandatory |
| `MEOBridge::RegisterSink` (TESEquip) | `plugin.cpp:298` → `MEOBridge.cpp:75` | optional |
| `Diagnostics::Install` (TESSpellCast, TESHit, MenuOpenClose, + Probe crosshair) | `plugin.cpp:299` → `Diagnostics.cpp:397` | + the worker pump |
| `TradeBridge::RegisterFuncs` (10 Papyrus natives) | `plugin.cpp:407` → `TradeBridge.cpp:209` | script ABI |
| `MEOBridge::Acquire` (MEO interface) | `plugin.cpp:289` | external ABI |
