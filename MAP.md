# MFO — Architecture & Change-Impact Map

A **blast-radius map**, not a symbol index. Its job is to tell you what a change
*breaks* before you make it. Every non-trivial claim carries a `file:line`
citation so you can re-check it against the live code.

Complements the prose docs: `Docs/INVARIANTS.md` (95 rules — 81 numbered `#1`–
`#81` plus 14 lettered — cited here as `#N`), `Docs/ENGINE_NOTES.md` (proven
engine mechanisms). This map is the "what-depends-on-what" layer those don't
carry. `Docs/ARCHITECTURE.md` is HISTORICAL (2026-09-07 banner): it describes a
pre-implementation design and contradicts shipped code; do not use it as intent.

**Citation namespace (2026-09-07):** `#N` here always means an INVARIANT.
A task / issue / feature number is written **`T#N`** (`T#75` the equip gate,
`T#76` the equip force-hold, `T#78` `mfoEnabled`, `T#67` the SE cast-crash gate).
They shared one namespace until 2026-09-07 and five task numbers collided with
real rules — see `Docs/INVARIANTS.md` "CITATION NAMESPACE".

---

## How to use this map

1. **Navigate by `file:line`.** Jump straight to the cited line; don't read
   whole files. Sizes verified on `feat/mfo-idle-hand-and-churn` 2026-09-08 after
   the Actuation split: ProgAllocator 2354, Actuation 2237, Logistics_Loot 2285,
   Packages 2153, Logistics 2072, Actuation_Direct 1679, Board 1346,
   CasterConsent 1243, Board_Progression 1234, Board_FieldKit 1135,
   Actuation_Hands 894. None of these should sit in context — grep to a symbol,
   read a narrow window. **2026-09-14 (`feat/mfo-dualwield-combat-pick` `1ac3c6b`):
   Actuation 2605 — OVER the 2500 cap; marth relaxed the cap until the next major
   release (report, do not split). Actuation_Hands 918, Actuation_internal.h 378,
   Loadout 677.**
   *(Two files have breached the 2500-line HARD RULE and been resolved the same
   way — an internal header for the shared state plus a cohesive module.
   `Board.cpp` hit 2577 at the overlay-swapchain merge `924f3a9`;
   `refactor/board-split` moved the Field Kit panel into
   `native/Board_FieldKit.cpp`. `Actuation.cpp` hit 3282 on this branch;
   `refactor/actuation-split` moved the per-hand cast lock into
   `native/Actuation_Hands.cpp` behind `Actuation_internal.h`. The cap holds
   codebase-wide.)*
   **CAUTION — `Actuation.cpp` line anchors below are a mixed bag.** The
   2026-09-08 split re-anchored every citation that was accurate against this
   branch. Citations that were ALREADY stale before it (written against `main`,
   where the file is 2271 lines and those anchors still resolve — e.g. `CastOn` at `:600`, `Fire` at `:1832`,
   `ConcentrationCast` at `:470`, and the interior call-site lists) were left
   exactly as they were and still need their own re-anchor pass. Grep the symbol,
   don't trust those numbers.
2. **Re-verify before editing.** Line numbers drift with every commit. Before
   changing a subsystem, re-read its "What breaks" entry *against current code*
   and **update this map if the structure moved.** A stale impact note is worse
   than none.
3. **`native/` only.** `imgui_impl_win32.*` is the only vendored code in
   `native/` — **do not read it.** No `extern/`; ImGui itself comes via vcpkg.
4. **When unsure a ripple is real,** entries say "UNVERIFIED — check before
   relying." Treat those as leads, not facts.
5. 39 real translation units + headers; ~38k lines (verified 2026-09-07). Startup wiring lives in
   `plugin.cpp` (read it first for the load order).

---

## High-blast-radius zones

| Zone | Where | Why it ripples / what breaks |
|---|---|---|
| **Co-save (4 records)** | `Serialization.cpp`, `Serialization.h`, `State.h` | FLWR `v5`, MSTK `v1`, PRGN `v7`, FWPN `v1` (`Serialization.h`). FLWR v5 (T#78) APPENDED `mfoEnabled` u8 after `combatClassOverride` (`if(version>=5)`); v1–v4 byte-identical, pre-v5 defaults `true`. Changing a field order/type/count, or bumping a version without a matching gated reader, **desyncs the byte stream and corrupts live saves**. A downgraded DLL destroys newer records (#12) — warned on-screen. PRGN v5 APPENDED the §HMS block (`if(a_version>=5)`); **v6 (§HMS Phase 3) DROPS `hmsTarget` (recomputed on load), ADDS a global `g_playerHmsTotalLast` f32 in the header + per-follower `hmsZeroAwardStreak` u8 + `hmsGrantRemainder` f32×3 + `hmsAwardAccum` f32 + flags bit 0x20 `fixedStat`.** v5 reader KEPT (reads+discards the old target, defaults the new fields); v1–v4 byte-identical. **v7 (2026-09-13, A′/B′) APPENDS per skill `autoPoints` f32 (after `manualPoints`) and per follower, after the HMS block, `autoLevelsGranted` u16 + `freeRespec` u8 + `strippedCount` u16 + stripped perk FormIDs u32×N (ResolveFormID'd) — NO per-session flag (`nativeHeld` is runtime-only, Fable F1).** v6 reader KEPT: `autoPoints` migrates as `max(0, points − manualPoints)` (today's APPLIED value, frozen — under-records cap-wasted auto points, REVIEW-BACKLOG MFO-B11; and a cap-saturated skill with a FRACTIONAL natural can display one integer lower after the v7 hold-time floor, REVIEW-BACKLOG MFO-B13), `autoLevelsGranted` = the loaded partition's auto levels (nothing pending, nothing re-split). |
| **Serialized string/ordinal contracts** | `Vocabulary.h`, `State.h` | Gambit opcode **strings** are persisted verbatim (#10); `Subject` enum and `CombatStyle::Stance`/`combatClassOverride` ordinals are persisted as raw bytes. Renaming an opcode or renumbering an enum is a **schema migration, not an edit** — old saves silently misread. |
| **`ResetAllState` teardown order** | `Serialization.cpp:680-746` | `StopPump()` MUST run first (`:686`) to drain the worker before any `clear()`; concurrent map insert+clear is UB. Every subsystem's `ClearTransientState`/`ClearAll`/`ReleaseAll` is ordered here. Reordering re-opens the load-screen-crash race. |
| **Alias fills / evict marker** | `Packages.cpp` | Alias fills at static priority 60 are **serialized into the `.ess`** (`plugin.cpp:313-337`). Missing/reordered `ReleaseAll` on kPreLoadGame / post-load / revert latches actors permanently across all descendant saves. The evict marker must stay a non-actor XMarker (base `0x3B`) or the **furniture-ejection bug** re-breaks (player forced into a package alias). |
| **The worker pump** | `Diagnostics.cpp` | One sleeper thread (`SleeperLoop`, 133 ms) drives the *entire* per-follower tick (`Scheduler::Tick`/`Loadout::Tick`/`Probe::Tick`) via `AddTask`. `kPumpMs` is the evaluator deadline, not a HUD constant. StopPump-before-clear is the linchpin invariant. |
| **Combat vfunc hooks** | `Targeting.cpp`, `CasterConsent.cpp`, `CombatStyle.cpp` | Three engine vtable hooks, install-once at `plugin.cpp:299-301`, VR-refused. Run on the **combat thread**. Any `CombatController` member touched there must be `< 0x68` (AE +8 layout bug; static_asserts). Signature mismatch corrupts every actor's combat/cast call. |
| **Frozen FormID / ESP contract** | `Forms.h` | Local FormIDs (`0x800`+) are a frozen contract with `MFO_GenerateESP.py`, audited by `tools/audit_esp.py` (#41). Changing one orphans every save that saw it; `0x802` stays reserved. |
| **Config INI keys** | `Config.cpp/.h` | Each key is wired in ~6 coupled places; the key **name** is the MCM-Helper persistence identity. Renaming unbinds the control; changing a key's *semantics* without renaming reinterprets the persisted value (the MEO ~100x-XP class of bug). |
| **External ABIs** | `MEO_API.h`, `APMF_API.h`, `TradeBridge.cpp`, `Papyrus.cpp` | `MEO_API.h` is a wire ABI shared byte-for-byte with a *separate* shipped MEO.dll (append-only). `APMF_API.h` is likewise byte-shared with a *separate* APMF.dll (append-only, C-ABI POD; consumed by `APMFBridge`) — mirror APMF's copy exactly, never edit it locally. `TradeBridge`'s 10 Papyrus natives + `Papyrus.cpp`'s 3 method-name strings are called by shipped `.pex` — renaming breaks scripts silently. |

---

## Startup / teardown wiring (`plugin.cpp`)

The single source of truth for ordering. Everything below depends on it.

- **`SKSEPluginLoad`** (`plugin.cpp:489`): `SetupLog` (`:76`; rotates the previous
  `MFO.log` to `MFO.log.1` first — `RotateLog` `:67`, one `[startup]` line — and
  sets `flush_on(info)`, no background flusher: the per-line flush is the only policy
  that keeps the tail through a fast-fail / hang+kill, 2026-09-21), then
  `Diagnostics::InstallFatalHook` (`[fatal]`, needs the logger + `REL::Module`), logs
  version (stale-binary guard #44), registers the unique id + the 3
  serialization callbacks, the message listener, and
  `TradeBridge::RegisterFuncs` via the Papyrus interface (must be here,
  runs each VM init).
- **`kInputLoaded`** (`:263`): `Config::EnsureMcmDefaults()` — seeds the MCM
  store *before* MCM Helper reads it at its own kDataLoaded (bind-on-first-load).
- **`kDataLoaded`** (`:276`) — the once-per-launch install order (comment
  `:277`): `EnsureMcmDefaults` → `Config::Read` → `Forms::Resolve` →
  `Gait::Apply` → `Catalog::Load` → `Progression::Init` → `ProgAllocator::Init`
  → `Logistics::ComputeWeakPotionFloor` → `MEOBridge::Acquire` →
  `APMFBridge::Acquire` (APMF cast-select client; null-degrades if APMF absent) →
  `Diagnostics::RefreshFatalModules` (APMF.dll's range for the `[fatal]` line) →
  `Followers::ResolveQuirks` → `MainThread::Install` → `Targeting::InstallHook`
  → `CasterConsent::InstallHook` → `CombatStyle::InstallEquipGate` →
  **sinks LAST** (`Rapport::RegisterSinks`, `Logistics::RegisterSinks`,
  `MEOBridge::RegisterSink`) → `Diagnostics::Install` → `Board::Install`
  (overlay: swapchain-vtable hooks + input sink IF the trampoline did not take,
  VR-refused, `:306`; the input trampoline itself went in earlier, at plugin
  load -- `Board::InstallInputHook`). **Sinks
  must follow form resolution or they fire against unresolved forms.**
- **`kPreLoadGame`** (`:313`): `Diagnostics::StopPump()` then
  `Packages::ReleaseAll("kPreLoadGame")` — release the alias the engine would
  otherwise serialize into the outgoing `.ess` — then
  `APMFBridge::ClearTransientState()` (drop runtime-only cast-select claims once the
  pump is drained).
- **`kPostLoadGame`/`kNewGame`** (`:339`): warn if newer save (`:353`) →
  `Probe::ReleaseAll` → `Packages::EnsureEvictMarker` (**before** the reconcile)
  → `Packages::ReleaseAll("post-load reconcile")` → `Forms::EnsurePlayerSetup`
  → `Followers::Refresh` → seeds → `Logistics::SweepBeastHeadsOnLoad` →
  `Loadout::Reconcile` → `ProgProbe::OnPostLoad` → `ProgAllocator::OnPostLoad`
  → `Board::SetHud` → `Diagnostics::StartPump`. **Runs after the co-save loads**
  (ARCHITECTURE §9) or ledgers look empty.

---

## 1. Co-save & authoritative state — `Serialization.*`, `State.h`  ⚠️ HIGHEST BLAST RADIUS

**Responsibility.** Owns the SKSE serialization callbacks and the four
independent co-save records. `State.h` defines the authoritative in-memory
state (`g_followers`, `Gambit`, `FollowerState`).

**Four records (`Serialization.h`), each with its own version + reader:**
- **`'FLWR'` / `kSchemaVersion=5`** (`Serialization.h:8,134`) — per-follower
  `{rapport, rank, combatClassOverride(v4), mfoEnabled(v5), tables[Combat,Logistics][], overrides[]}`.
  Written `SaveCallback` `Serialization.cpp:86`; read by `LoadCallback` `:569`
  dispatching into the per-record helper `ReadFollowersRecord` `:351` (a short
  read/implausible count aborts THAT record only and the loop continues to
  MSTK/PRGN/FWPN — the same isolation contract as `CoSaveLoad`/`CoLoadForcedWeapons`;
  on the write side a `WriteString` failure sets `flwrOk` and BREAKS to the
  sibling records instead of returning — never let either side skip the siblings).
  Version history v1→v5 documented `Serialization.h:113-133`; v1 tutored-block
  reader kept forever (`Serialization.cpp:499`). **v5 (T#78) APPENDS `mfoEnabled`
  as one u8 right after `combatClassOverride`, read gated `if(version>=5)`; a
  pre-v5 record has none and defaults `true` — every existing follower stays
  MFO-enabled, v1–v4 byte-identical.**
- **`'MSTK'` / `kStockVersion=1`** (`Serialization.h:13`) — Logistics'
  per-follower stock-gear sets; second independent record, never touches FLWR.
  Write `Serialization.cpp:223-254`, read `ReadStockRecord` `:286-346`. Owner: `Logistics.cpp`.
- **`'PRGN'` / `kProgVersion=6`** (`Serialization.h:96`) — GENERAL per-follower
  **follower-allocation-state slot** (host machinery, v1.1 Phase 8 reframe): all
  fields are general allocation-engine state (enrolled flag, an OPAQUE plugin-
  qualified class-def reference, allocated perks/skills, HMS pools, battle
  counters, fixed-stat); it holds **no add-on judgment** — ratios/verdicts/layout
  live in the manifest form the class reference points to. Delete the manifest →
  nothing enrolls → `g_prog` empty → writes header + count=0 (Phase 9 test). The
  `'PRGN'` fourCC + name are **historical/frozen** (deployed Tuxborn v6 save), not
  an add-on-specific schema. Layout + I/O live in `ProgAllocator.cpp` (`CoSaveSave`,
  `CoSaveLoad` — grep the symbols, line numbers drift). Written **even when the
  addon ESL is absent** (state echoed back verbatim, not destroyed).
  v-history v1→v6 in `Serialization.h:21-83` (v4 = plugin-qualified class identity,
  v3 reader KEPT; v5 = §HMS class-redistribution block APPENDED at END). **v6 (§HMS
  Phase 3 fixed-stat grant):** (a) global header gains `g_playerHmsTotalLast` f32
  right after `lastPlayerLevel`, before the follower count (gated `if(version>=6)`;
  pre-v6 seeds it from the live player total); (b) flags bit `0x20 = fixedStat` (free
  in v1–v5, reads 0); (c) the per-follower §HMS block **DROPS `hmsTarget`** (always
  `max(baseline, baseline+cumulative)` — recomputed on load) so v6 writes per pool
  `hmsBaseline`,`hmsSkew`,`hmsCumulative` (f32×3, was 4), then the counters/captured
  as before, then **APPENDS `hmsZeroAwardStreak` u8 + `hmsGrantRemainder` f32×3**.
  **v5 reader KEPT (#12):** reads the old 4-f32/pool layout, DISCARDS the stored
  target + recomputes it, defaults the new fields (streak 0, remainder 0). A v4
  record read has no block → `hmsCaptured=false` → first `RecomputeHMS` adopts the
  live base. All floats finite-guarded; streak clamped 0..2.
- **`'FWPN'` / `kForcedWeaponVersion=1`** (`Serialization.h:105-106`) — T#76 force-hold:
  the weapons MFO force-equipped for an active equip gambit. Owner
  `Actuation.cpp` (`CoSaveForcedWeapons` `:2409`/`CoLoadForcedWeapons` `:2444`); **CoLoad
  RELEASES the locks, never repopulates** (a session starts with no force-hold,
  the gambit re-forces if still true). Fourth independent record. **LAYOUT UNCHANGED
  by the 2026-09-13 dual-wield left hold:** the ledger value became
  `ForcedHold{right,left}` (`Actuation_internal.h:329`) but the record is still a
  flat list of (follower, weapon) pairs — a dual hold writes TWO pairs for one
  follower, and the loader has always released pairs by OBJECT, never by slot
  (since `1ac3c6b`, F4, the loader names the slot from the LIVE hands: left slot
  when the form is held left, default when right, both for a same-form dual
  hold). `kMaxForcedWeapons` 64 is a PAIR cap now (REVIEW-BACKLOG MFO-B20).

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
(`ConsumeNewerSaveWarning` `:673`, `plugin.cpp:353`).

**Load-time migrations to preserve:** `"act.equip_torch"` in the combat table is
redirected to logistics (#35, `:486`); an empty board is backfilled with
`Followers::ApplyDefaultKit` (`:559`); over-cap gambits are fully *consumed* but
not stored (`:491`, #22f) or the stream desyncs.

**`ResetAllState()` (`:680`) — the teardown-order contract (⚠️).** RevertCallback
and the load window both funnel here. Order is load-bearing:
`Diagnostics::StopPump()` **first** (`:686`, drains the worker) → `MainThread::Clear`
→ `g_followers.clear()` → `Followers::g_active.clear()` → each subsystem's
`ClearTransientState`/`ClearAll` (Followers, Scheduler, Logistics, +`ClearStockGear`,
ProgAllocator, Loadout, Targeting, CasterConsent, CombatStyle, Sightline, Board,
Packages `ReleaseAll("revert")` (`:733`), Papyrus, TradeBridge, MEOBridge, Probe, Rapport)
→ `Board::SetHud(false)`. **What breaks:** move any clear out, or run it while the
pump is live, and you race a worker insert (UB). `Packages::ReleaseAll` here
re-reads the alias from the quest even when the module believes it holds nothing
(`:604-609`) — that independence is deliberate.

**`State.h` structures (the serialized shapes):**
- `Gambit` (`State.h:28`): `conditionOpcode`/`actionOpcode` (strings, serialized),
  `conditionParam`, `actionParamForm` (FormID, ResolveFormID on load),
  `subjectSelector`, `subjectActorForm` (#68, v3). `uid` is **runtime-only, never
  serialized** (`:29`) — the board keys edits on it, not row index.
  `lastFired*` display-only, never read back by the evaluator.
- `FollowerState` (`State.h:72`): `rapport`, `rank` (clamped [1,5]),
  `combatClassOverride` (v4; ordinals == `CombatStyle::Stance`),
  `mfoEnabled` (v5, T#78; the per-follower MFO master switch, default true),
  `tables[kCount]`, `overrides`.
- `g_followers` (`State.h:126`) — **MAIN-THREAD-ONLY, takes no lock (#4).** Keyed
  on persistent FormID; dismissed followers stay. Off-thread access must snapshot.
- Slot caps `kCombatSlotsByRank`/`kLogisticsSlotsByRank` (`:112-113`) — the
  default kit must fit Rank I (3 combat / 4 logistics) or a round-trip truncates it.
- **`overrides` / `PackageOverride` (`State.h:57,94`) is vestigial:** the only
  writer is the co-save loader (`Serialization.cpp:532`); no runtime code
  populates it (grep-confirmed). It round-trips but is inert — MFO drives packages
  via alias fills, not PapyrusUtil overrides (banned #18/#19).

---

## 2. Actuation core — `Packages.*`, `Actuation.*`, `Scheduler.*`, `Gait.*`, `MainThread.*`  ⚠️ ENGINE-DANGEROUS

The most engine-dangerous cluster: force-fills quest aliases (serialized into the
`.ess`), mutates shared `TESPackage` records, and pumps a state machine off engine
observation. All five files are **main-thread / serialized-queue only; none is
thread-safe.** `Scheduler::Tick` and `Packages::Pump` run on the **SKSE AddTask
job worker** (dispatched `Diagnostics.cpp:343`), the same serialized queue as
`Packages::NotifyCast` — which is why `g_holder` needs no lock.

### Packages.cpp / Packages.h — the alias/marker engine
Force-fills MFO's quest aliases at static priority 60 to run MFO's packages
(cast / loot-travel / retreat), observes engine state to advance a phase machine,
releases **by eviction** with a non-actor XMarker.

- `g_holder` (`Packages.cpp:174`) single-writer cast state (no lock);
  `g_liveStream` atomic mirror (`:195`) is the ONLY holder field the caster-thread
  consent hooks may read.
- `EnsureEvictMarker` (`:1215`) — caller `plugin.cpp:349` only, **before** the
  post-load reconcile. **What breaks:** if it stops minting, `EvictionRef` falls
  back to the PLAYER → furniture-ejection bug (v1.0.25/26) re-breaks.
  Must be main-thread (`PlaceObjectAtMe` mutates the cell) + force-persisted.
  Base-`0x3B` revalidation is load-bearing (handle indices rebuild per load).
- `ReleaseAll(why)` (`:1240`) — callers `plugin.cpp:321,355`, `Serialization.cpp:733`.
  The save-corruption backstop: sweeps all 4 loot aliases + retreat + command,
  evicting any actor occupant including the player (#48b) with the marker.
  **Ordering:** at kPreLoadGame runs after `StopPump` (can't race `Pump`); the
  player-sweep requires the marker (`haveMarker` guard `:1265`).
- `Pump()` (`:1087`) — caller `Scheduler.cpp:135`, unconditional before every early
  return. Only advancer of `Requested→Filled→Running→Done`. `EvaluatePackage(true,
  false)` at `:1118` — **`resetAI` must stay false** everywhere (`:713,1118,1411,
  1434,1499,1577,1598`); `true` clears the combat group → zero-damage next hit.
  Timeouts `kFillTimeout=3.0`/`kRunTimeout=12.0` (`:132-133`).
- **SELF-CAST does NOT use the package (SPEC-self-cast-forced, superseded 2026-08-17).**
  Deck-proven: a no-QNAM/t6 package can be DELIVERED (equips the spell) but never
  TRIGGERS the cast — the QNAM + target-alias linkage is what drives the engine to
  EXECUTE the foe cast — and a package is DECLINED outright on package-locked custom
  followers (Lucien, prio-80 quest). So self routes through
  **`Actuation::CastSelfDirect`** (`Actuation_Direct.cpp:829`, public) — effect + magicka only,
  **NO equip, NO channel** (registry `g_selfCast`, worker-serial; `SelfCastReconcile`
  ticks it from `Diagnostics` before `Loadout::Tick`; `ClearSelfCasts` on revert).
  **NEVER equips the spell**: `CastSpellImmediate`(kInstant) applies the effect
  hands-free. Leaving a light spell equipped let the follower's OWN AI spam-cast it
  → 55+ non-MFO lights → `ShadowSceneNode` light-limit CTD (deck 2026-08-19); the
  equip/`HoldStow`/caster-drive scaffolding was REMOVED (animation deferred to a
  polish pass). FIRE (`CastSelfDirect`, every tick the rule wins): refresh the entry
  and — once per `fCastCooldown` — `ApplySelfEffect` (main thread). Its **already-
  active guard is the foe cast's own predicate** — `AsMagicTarget()->HasMagicEffect(
  sp->GetCostliestEffectItem()->baseEffect)` — so a duration self-buff/light is NOT
  re-applied while active (exactly one light per effect-duration cycle); an instant
  heal (no lingering effect) re-fires when the condition recurs. RELEASE
  (`SelfCastReconcile`, on rule-stale / follower gone — **FF self-buffs have NO time cap**,
  a long light lives its authored duration; only CONCENTRATION self-streams carry the
  `DrawConcCap` random cap + heal-full): `DispelSpellEffectsOn`
  removes a lingering **ward/buff** so it cannot persist as a stuck gameplay effect
  (functional bounding); nothing to unequip. Touches only the ACTOR, no alias →
  **follower-agnostic** (deck-proven on Lucien). Gated behind `Config::g_castSelf`
  (bCastSelf). Callers: `CastOn` self-intercept (combat, BEFORE the
  concentration fork), `ConcentrationCast` self guard (defence-in-depth), `Logistics.cpp`
  `act.cast_self` branch (out-of-combat, `selfPkg`). **RANDOMIZED self cap
  (`SelfCastReconcile`, CONCENTRATION-ONLY):** a self-cast concentration channel is
  bounded by a per-stream RANDOM cap drawn at start (`DrawConcCap`, stored in
  `SelfCastState::cap`): **heal/utility/buff → uniform `[8,15]` s**, offense → `[2,6]` s
  (loose human timing, never serialized). ALSO ends on **MAGICKA-OUT** (`have <
  CalculateMagickaCost` → stop, dispel — a held cast stops when magicka runs, which makes
  the long caps safe) and a **self-HEAL at ~full own HP** (`kHealFullPct` = `Vocab::kHealFull`
  0.9995). **DISPEL-on-release** for a sticky Buff (any release), a heal on
  heal-full/magicka-out/stale (true end-of-stream); a plain cap on a still-wanted stream re-streams
  next tick with a fresh random cap. An **FF self buff keeps its authored duration** (not
  concentration → no cap). NOTE `kConcHealCap`/`kConcSelfUtilityCap` are now the per-beat
  SUSTAIN WINDOW (AE duration bridge), NOT the stream cap.
- **CONCENTRATION = DIRECT FORCE everywhere, no package (`CastTargetDirect`) — see
  `Docs/CAST-DELIVERY.md` (canonical).** BOTH the Logistics OOC dispatch AND combat's
  `ConcentrationCast` deliver EVERY non-self concentration cast (player/ally/foe)
  through `Actuation::CastTargetDirect` (`Actuation_Direct.cpp:1141`) — `CastSpellImmediate` straight onto the target
  + magicka deduct, the SAME known-working force `CastSelfDirect` uses, touching NO
  package. Why: the package route `§4.6`-DECLINED every tick for a **package-locked
  custom follower** (Lucien 2F00591F, prio-80 quest owns the cast alias) — OOC his
  on-PLAYER heal never landed (c539257 regression), and in COMBAT the same decline
  compounded with the consent hooks denying his own AI (`CheckStartCast`/`CheckCast`)
  into a TOTAL lockout. The direct apply passes through NEITHER hook (they sit on the
  AI's `RequestCastImpl` pipeline, which `CastSpellImmediate` skips — ENGINE_NOTES
  §0.13), so the AI stays denied while MFO's stream delivers. **CADENCE CONTRACT
  (`kConcApplyPeriod`, 1 s):** a concentration magnitude/cost is authored PER SECOND,
  so the channel re-applies every ~1 s (fCastCooldown pacing quartered heal throughput
  — the "heals feel broken" bug); FF spells keep the fCastCooldown beat. **REAL-EFFECT
  CONTRACT (`SustainConcentrationEffect`, main thread — marth's ruling, supersedes the
  removed `ApplyConcentrationBeat` RestoreActorValue recreation):** a bare one-shot
  `CastSpellImmediate` applies ~0 of a per-second concentration magnitude (rate × ~one
  frame — b63beb9 field A/B: magicka drained, HP flat on self AND player), so the REAL
  effect is attached ONCE per stream and SUSTAINED: each beat pins a real `duration`
  (the stream's window) + re-arms `elapsedSeconds` on that single ActiveEffect
  (instance-local, never MGEF mutation, never touching the ENGINE-COMPUTED per-caster
  `magnitude`) — the engine channels the magnitude itself, every archetype
  (waterbreathing/invisibility/ward just LAST), resists and skill/perks included; MFO
  owns delivery + per-second cost + bounds only. ONE sustained HUD entry, shader plays
  continuously (effect VFX is IN scope — only the caster POSE is deferred). Wired into
  `ApplySelfEffect`, `ApplyTargetEffect`, AND AUTO's `ApplyEffectFromTo`.
  **CONCENTRATION + SELF-delivery off-self → DELIVERY-FLIPPED PROXY (`ConcProxy`/`DeliverySpell`,
  `Actuation_Direct.cpp:174`/`:247`):** baseline `CastSpellImmediate(sp,target,follower)` lands an FF Self effect on
  `target` (Candlelight/flesh work — do NOT touch), but a `kSelf` CONCENTRATION channel binds to
  the caster's OWNER, so a player/ally conc heal collapses onto the follower. Gated on
  `kSelf && kConcentration && target!=follower`, MFO casts a transient COPY with casting style
  PRESERVED and ONLY `data.delivery` flipped `kSelf→kTargetActor` (via the unchanged
  `ApplyTargetEffect` conc branch + `SustainConcentrationEffect` keyed on the copy) — lands
  on the recipient, follower is caster (rate+cost), player uninvolved. **A proxy cast starts a
  REAL ENGINE CHANNEL that drains the follower per-second independent of MFO's apply** (runaway
  = magicka drain with NO `FORCE-CAST` log). **SLOT-FOR-DURATION (owner-keyed `Slot g_slot[kSlotCount=6]
  {form,source,owner}`, one per concurrent party healer — grown from 2 so a 3rd+
  self-delivery stream no longer overflows to a silent skip):** each live stream OWNS a slot (`ConcProxy::Acquire(follower, src)` —
  reuse owner's slot / `Configure` a FREE slot / else nullptr → caller SKIPS); a slot is
  Configure'd ONLY when free, never while its channel lives (else freeze + heal-full-stops-1st-
  not-2nd). RELEASE (`TargetCastEndActor(target,spell,owner)`, EVERY release) = dispel source +
  owner proxy AE + **`InterruptCast` the follower's kInstant caster** (stops the engine channel)
  + `ConcProxy::Free`. Reconcile makes every release a true END (heal-full/magicka-out/cap/stale/
  gone); a wounded heal's cap re-serves a FRESH stream (new slot/channel). **AUTO ally-heal for a
  CONCENTRATION heal = SEQUENTIAL MOST-HURT** (`CastAuto`, before the fan/g_autoCast gate): one
  channel per caster, so it picks the single most-hurt member below `min(threshold,kHealFull)`
  (player/teammate/self) and serves it via `CastTargetDirect`(other) / `CastSelfDirect`(self) EACH
  tick; when it tops off (heal-full FREE) the next-most-hurt is served — every hurt ally cycled
  over seconds, one slot at a time. FF/instant heals + non-heal buffs still FAN (`ApplyEffectFromTo`);
  a conc-Self non-heal buff fanned via AUTO is skipped. `SelfCastEndActor` likewise
  `InterruptCast`s the self channel. Breadcrumbs: `proxy slot ACQUIRE/RECONFIG/FREE/OVERFLOW`,
  `stream RELEASE (reason)`. Never serialized; main-thread-only; `ConcProxy::Reset` on revert/load.
  FF/self-cast/non-Self UNTOUCHED. Momentary
  sustained effect dispels at END-of-stream (stale/gone/switch) only — it genuinely
  channels, so the stream's end must cut it; a cap-only release keeps the one entry
  alive across re-streams. Evidence line "conc effect ATTACHED": once per stream =
  engine honors the sustain; repeating per beat = it does not (then: apply the
  engine-computed `ae->magnitude` per second or an FF-variant spell — NEVER base-value
  recreation). Bounded/
  released by `TargetCastReconcile` (registry `g_targetCast`, one stream per follower)
  on a RANDOMIZED per-stream cap (`DrawConcCap`, stored `TargetCastState::cap`, drawn at
  start, never serialized): **heal/utility 8-15 s, offense 2-6 s** (LoS + line-of-fire
  re-checked on EVERY apply in `CastTargetDirect`). ALSO ends on **MAGICKA-OUT** (`have <
  CalculateMagickaCost(follower)` → stop, dispel — makes the long caps safe, no over-drain)
  and a **HEAL at ~full recipient HP** (`kHealFullPct` = `Vocab::kHealFull` 0.9995).
  Dispel-on-release for a sticky Buff (any release) and a heal on
  heal-full/magicka-out/stale (true end-of-stream); a plain cap on a
  still-wounded heal is release-only and **re-streams with a FRESH random cap** so a
  wounded target tops up across bursts. `TargetCastReconcile` runs each tick in
  `Diagnostics.cpp` beside `SelfCastReconcile`; both cleared in `ClearSelfCasts`. SELF
  stays on
  `CastSelfDirect`; self-with-gate-off is skipped legibly (never direct-applied behind
  bCastSelf). The old combat package stream (`Packages::CastAt` + `CastHold`,
  v1.0.58-65) is REMOVED from `ConcentrationCast`, and its
  bForceCastOnMiss+bUsePackages gate with it.
- **DEAD/superseded: the package self-route.** `Packages::CastSelf`, `Begin`'s self
  branch, command-quest **alias 2** (`kAliasCommandSelfActor` → `MFO_CastPackageSelf`,
  Forms `0x835`), `SetSelfSpell`, `HolderActorAlias`/`HolderPackage`'s self side, and
  the alias-2 `ReleaseAll` sweep are all still present but NO LONGER on the live path
  (nothing calls `CastSelf` with the gate on). Left in place (harmless; the ESP record
  never fills) rather than churn the frozen `0x835` FormID; can be removed later.
- `CastAt`/`Available`/`StreamLive` (FOE cast) — callers `Actuation.cpp` (ForceCast,
  the FF combat force-half ONLY) + `Logistics.cpp` (OOC FF-hostile-at-foe, now with a
  direct-force fallback when the package `§4.6`-declines), `CasterConsent.cpp:163`
  (reads the atomic mirror — `StreamLive` is now always false for concentration since
  no concentration package stream exists; the exemption is dormant, harmless). One
  `MFO_CastPackage` on alias 0 → single holder forced by shared `TESPackage::refCount`
  (`:790`); multi-holder needs per-verb records at 0x821+. The `CastHold` overload of
  `CastAt` is DORMANT (concentration no longer dispatches a package). **Concentration
  stream time-cap** is drawn per-stream by ONE helper `DrawConcCap(kind)` (`Actuation_internal.h`,
  shared `inline`, `std::mt19937` + `uniform_real_distribution`): heal/utility uniform `[8,15]`s,
  offense `[2,6]`s, stored in `TargetCastState::cap`/`SelfCastState::cap` at stream start,
  never serialized; consumed by `TargetCastReconcile` and `SelfCastReconcile` (+ magicka-out
  stop + heal-full `kHealFullPct` = `Vocab::kHealFull` 0.9995). (Replaced the old fixed
  `ConcentrationHold` numbers.) **HEAL-BOUNDARY fix (Vocab::kHealFull, Vocabulary.h):** an
  HP-below heal threshold of 100% never stops (HealthPct asymptotes to 1.0), so every heal
  re-dispatch/target-select clamps the TOP to 99.95% via `min(param, kHealFull)` —
  `Evaluator::ConditionTrue` (`kCondSelfHpBelow`/`kCondPlayerHpBelow`), `Evaluator::PickAlly`
  (`lowest`), `CastAuto` heal fan (`>= min(threshold, kHealFull)`); the stream heal-full uses
  the SAME mark so re-dispatch and stream stop agree. The FOE package
  `§4.6`-DECLINES for package-locked custom
  followers — every concentration path avoids it entirely (`CastTargetDirect`), and
  the FF paths fall back to a direct silent cast.
- `LootTravelFill/Retarget/Clear/EvictIf`, `RetreatFill/Clear/EvictIf` (`:1380-1623`)
  — callers throughout Logistics/Scheduler + dismissal. **All release by eviction,
  never VM Clear** (scriptless aliases no-op a VM Clear); priority 60 is static and
  can't be lowered to release. `LootTravelRetarget` refills only the TARGET alias,
  leaving actor alias 0 filled (no hand-back between corpses). **PASS B: the loot
  trio now tries the APMF ch.9 0x49 route FIRST** (see the dedicated Packages.cpp
  entry below, near APMFBridge.cpp) — degrades to this unchanged alias route when
  APMF is absent/off.
  The full per-follower teardown (Loadout/Targeting/CombatStyle/ForcedWeapon/
  CasterConsent/Packages/OnFollowerRemoved/RetreatEvictIf) is now ONE helper
  `Followers::ReleaseHeldState(id)` (`Followers.cpp`, worker-only, idempotent) —
  shared by the dismissal sweep (`Refresh`) and the T#78 MFO-OFF toggle (Scheduler).
- `ForceRefToNative` (`:319`) = `REL::RelocationID(24523, 25052)` `TESQuest::ForceRefTo`,
  AE + SE (SE id verified 2026-09-13 via the engine's own `ReferenceAlias.ForceRefTo`
  Papyrus callback tail-jump; **CONFIRMED 2026-09-15** — `Docs/ADDRESS-TABLE-2026-09-15.md`
  row "`Packages.cpp:302-305 TESQuest::ForceRefTo`": AE 25052 → `0x3CDEE0`, SE 24523 →
  `0x375050`, both by raw objdump; 1.7.104 `0x3D4FA0` confirmed but NOT placed — no address
  library, no `REL::Offset` convention, marth's call). Gated by `ForceRefToNativeAvailable()`
  (`:319`) = **`Runtime::CastPathsVerified()`** (`Runtime.h`: the AE bucket or EXACTLY 1.5.97;
  VR and every other 1.5.x refused, since the SE id was confirmed on 1.5.97 only) — every
  call-site gate (`:911`, `:979`, `:1656`, `:1767`, `:2032`) goes through that ONE predicate;
  VM path only where it is false. **What breaks:** the predicate lives in `Runtime.h` and is
  SHARED with the five cast gates and `plugin.cpp`'s `[runtime]` line — change it in one
  place and every consumer moves together; hand-roll a local `IsSE()` here and a 1.5.80 user
  gets an unverified id (pinned CommonLib 3.7.0 files EVERY 1.5.x, and any `major.minor` that is
  not 1.6/1.4 — a 1.7.x binary included — under `IsSE()`). Two-class layout offsets (`kPointerOffFromIPackageData=0x10`,
  `:76`) + `kTypeTargetSelector`/`kTypeSingleRef` guard (`:88`, `ReadTarget` `:431`)
  are **memory-safety critical** — `SetInputs` (`:467`) writes nothing if guards fail.

### Actuation.cpp / Actuation_Direct.cpp / Actuation_Hands.cpp / Actuation_internal.h / Actuation.h — "a package IS the action"
Only module that mutates actor state; main-thread only. **Split mechanically
TWICE, no logic change either time — 2026-08-31 (`Actuation_Direct.cpp`) and
2026-09-08 (`Actuation_Hands.cpp`, when `Actuation.cpp` reached 3282 lines):**
`Actuation.cpp` (2237) = the combat-rule dispatch — `Fire` (`:1764`) + verbs,
`CastOn` (`:396`)/`ConcentrationCast` (`:266`)/`ForceCast` (`:88`),
`EquipWeapon` (`:1765`, **PERK-DRIVEN since 2026-09-13 — see "COMBAT PICK +
DUAL WIELD BY PERKS" below**) with its anon helpers `WeaponRolesFor` (`:1660`),
`IsOneHandMelee` (`:1668`), `PickOffHandWeapon` (`:1678`), `PickShield` (`:1695`),
`EquipShieldOnMain` (`:1715`), `EquipLeftHeld` (`:1731`) + the off-hand top-up
cadence `g_offHandRetryAt` / `kOffHandRetry` (`:1762-1763`), `NearestAlly`
(`:53`)/`ResolveCastTarget` (`:1973`), the APMF-refusal log, `ClearCastLock(s)`
(`:2474`/`:2490`), the T#76 force-hold map (`ForcedHold{right,left}`,
`g_forcedWeapon` `:43`) + `LogLeftHandReadback` (`:2306`) / `ReleaseForcedWeapon`
(`:2320`) / `YieldForcedLeftHand` (`:2365`) / `ReconcileForcedWeapon` (`:2396`) /
`ClearForcedWeapons` (`:2460`) + FWPN co-save (`CoSaveForcedWeapons` `:2503`,
`CoLoadForcedWeapons` `:2538`); `Actuation_Hands.cpp` (918) = THE PER-HAND CAST
LOCK's implementation — `HoldCastLock`/`ClearCastLockHand` (`:61`/`:77`), the
liveness ladder (`ClaimLiveOnHand` `:89`, `CastInFlightOnHand` `:251` (PUBLIC since 2.0.5 — declared in `Actuation.h`),
`CastLockLive` `:299`), rank preemption (`CanPreemptHand` `:394`,
`IncumbentTargetLost` `:433`, `IsOwnRetarget` `:484`, `PreemptHand` `:495`),
`WeaponHandExposure` (`:163`), `CastProxyOnHand` (`:109`), `HandFree` (`:549`)
and THE JUGGLE `ResolveCastHand` (`:575`); `Actuation_Direct.cpp` (1679) = the
direct-delivery streams (`CastSelfDirect`/`CastTargetDirect` +
reconciles/`ClearSelfCasts`, `CastAuto`) + their apply substrate
(`ConcProxy`/`DeliverySpell`, dispel/sustain,
`Apply{Self,Target}Effect`/`ApplyEffectFromTo`, beneficial-recast pacing) — the
two direct-cast registries (`g_selfCast`/`g_targetCast`) are file-local there;
`Actuation_internal.h` = everything that crosses a TU boundary: the shared
concentration numbers (`kConc*` sustain windows, `kConcApplyPeriod` cadence
contract, `DrawConcCap` random stream cap) AND the cast lock's shared state
(`g_castLock` `:255` + the three rate-limited log maps, `g_firingRule` `:192`
and `g_firingAllyThreshold` `:202`, `CastLock` `:204`, `HandPlan` `:304`, the
hand indices `:168`) plus the lock's six cross-TU entry points, all as
**`inline`** — any definition added to that header MUST be `inline` or it's an
LNK2005. Since 2026-09-13 it also carries the force-hold ledger's VALUE type
`ForcedHold{right,left}` (`:329`, `extern g_forcedWeapon`/`g_forcedMx` `:334-335`,
defined in `Actuation.cpp:43/:49`) and two non-inline cross-TU declarations:
`CastHandHeld(actor, hand)` (`:344`, defined `Actuation_Hands.cpp:363` =
`ClaimLiveOnHand || CastLockLive` — THE one question the equip side asks before
touching the left hand) and `bool YieldForcedLeftHand(actor, why)` (`:357`, defined
`Actuation.cpp:2365`; handed to `Loadout::Prepare` as its `LeftHandYield` callback
from `CastOn` `:1047`, and called from `ReconcileForcedWeapon` `:2421`). `Fire(follower,
choice)` (`Actuation.cpp:1832`) dispatches one action/tick: Wait / Attack (→`Targeting::Command`) /
Cast{Self,Player,Target}→`CastOn` / Equip{Ranged,Melee} / Flee→`Packages::RetreatFill`
/ PowerAttack / drink / unknown→fail-closed. First-match-wins.
- **PowerAttack (`kActPowerAttack`) is RANGE-GATED** (`Actuation.cpp` ~:1008): it
  latches the chosen foe (`Targeting::Command`, so the engine's combat AI closes
  distance — MFO invents no approach) and fires `attackPowerStartInPlace` ONLY when
  `GetDistance(follower,foe) <= Config::g_meleeReach` (200u, `fMeleeReach`).
  Out of reach → latch + `Fired` "closing" (OPAQUE, like Attack); in reach → anim
  then latch (reject = transparent fall-through, mutates nothing). Foe = PickFoe's
  target (the specific blocking foe for `kCondFoeBlocking`). Without the gate it
  swung at air whenever any foe blocked.
- `Outcome.transparent` (`Actuation.h:38`) is the fall-through contract the
  scheduler reads (`Scheduler.cpp:521`); default false = "wall" = safe. Flipping it
  changes suppression + hand-claim + spellsword fallback.
- **RUNTIME GATES — THE 1.5.97 PASS (`feat/mfo-1.5.97-pass`, 2026-09-15; Fable round 2 on
  `87cabc1` applied).** The five cast-control gates — `CastOn` (`Actuation.cpp:432`),
  `CastSelfDirect` (`Actuation_Direct.cpp:849`), `CastTargetDirect` (`:1166`), `CastAuto`
  (`:1470`) and `ComposedCast::Enabled` (`ComposedCast.cpp:61`) — evaluate ONE shared
  predicate, **`Runtime::CastPathsVerified()`** (`native/Runtime.h`, header-only) **=
  `REL::Module::IsAE() || Runtime::IsVerified1_5_97()`** — the pre-existing AE bucket, or
  EXACTLY 1.5.97 (`major.minor.patch == 1.5.97`). VR and every other 1.5.x are refused.
  `Packages::ForceRefToNativeAvailable()` and `plugin.cpp`'s `[runtime]` line evaluate the
  SAME function. WHY EXACT (Fable SEV-3): pinned CommonLib 3.7.0 classifies the runtime from
  the exe's second version field alone (`REL/Relocation.h` `load_version`: 4 → VR, 6 → AE,
  anything else → SE), so `IsSE()` is the DEFAULT bucket — true on 1.5.3 through 1.5.97 (all
  of which have address libraries, so the plugin loads and a bucket gate would OPEN with ids
  confirmed on 1.5.97 only) and on any future `major.minor` that is not 1.6/1.4, a 1.7.x
  binary included (moot only while no format-1 `version-1.7.x.bin` exists, because
  `IDDatabase::load` then dies fatally at plugin init). `Board.cpp:1594`'s input trampoline
  already gated on the exact pair; `Runtime.h` is that predicate made shareable. **THE AE
  SIDE IS STILL THE `IsAE()` BUCKET (open question, outside the 1.5.97 brief):** the gates were
  `IsAE()` since v1.0.48 and shipped on every 1.6.x that way; every AE offset in the table was
  measured on 1.6.1170 only. `Runtime::IsVerified1_6_1170()` exists so the `[runtime]` line can
  tag the build (`[1.6.1170]` vs `[AE (not 1.6.1170)]`); narrowing the gate itself is its own
  decision.
  The gates were `IsAE()`-only since v1.0.48 as **the T#67 SE crash gate**, whose fault was
  `Loadout::LeftHandSlot()`'s `BGSDefaultObjectManager::GetObject` read (see Loadout.cpp's
  entry) — gone, now a FormID lookup. **THAT LOOKUP CHANGES 1.6.1170 TOO (Fable SEV-2,
  measured):** on AE the `+0xB80` read is `objects[364]` = DOBJ `HMAE` = Update.esm FLST
  `0x01003275`, record flags `0x0`, so `LeftHandSlot()` ALWAYS returned `nullptr` on 1.6.1170
  in the field — every spell equip ran `EquipSpell(actor, spell, nullptr)` (engine-chosen hand
  for an EitherHand spell) and `EquipLeftHeld` (`Actuation.cpp:1748`, `if (!slot) return
  false`) was DEAD, with its five `UnequipObject(..., LeftHandSlot())` companions. This is the
  first build where the LeftHand slot reaches the engine on AE: gambit spells land LEFT,
  `[equip] ... GAMBIT equip off-hand` lines appear, a displaced left weapon returns LEFT at
  combat end, the per-hand claims / `kApmfHandLeft` / `CastInFlightOnHand(left)` / displaced-
  left debts are exercised for the first time. Kept on purpose (coordinator, round 2): it is
  the design intent.
  Every 1.5.97 value the gated paths reach is CONFIRMED in `Docs/ADDRESS-TABLE-2026-09-15.md`:
  rows "held branch `Loadout.cpp` `LookupByID<BGSEquipSlot>(0x00013F43)`" + "`BGSDefault
  ObjectManager::objects[]/objectInit[]` count" (364 SE / 366 AE / 372 1.7 — CommonLib's
  `IsObjectInitialized +0xB80` is right on SE only), "`Packages.cpp:302-305 TESQuest::
  ForceRefTo`" (the forced-cast package route), "`CombatController::attackerHandle @0x28,
  targetHandle @0x2C, combatStyle @0x38` (all `< 0x68`)" (MFO reads `combatController` via the
  SE/AE-shifted `GetActorRuntimeData()` accessor; `combatGroup` at `+0x00` is NOT a table row —
  it is CommonLib's own SE-origin declaration, right on SE by construction), "`CasterConsent.cpp:
  1087-1095` 14 seat vtables", "`CombatStyle.cpp:384-417` `CombatInventoryItemMagicT`" (30/30,
  the two `_CombatMagicCasterArmor_` rows supplied by the confirmation) and "`GetMagicTarget`
  sret" (same 16-byte out-slot ABI on SE). What the table does NOT row, and Fable's round-2
  review then verified on 1.5.97 by raw disassembly: `Actor::GetMagicCaster` vfunc 0x5C
  (`magicCasters[]` @0x1A0 SE / 0x1A8 AE), `MagicCaster::CastSpellImmediate` vfunc 01 (same
  ABI, `currentSpell` @0x28 / `state` @0x30), `ActorEquipManager::EquipSpell` `RELOCATION_ID
  (37939, 38895)`, `UnequipObject` (37945/38901), `MagicItem::CalculateCost` (11213/11321),
  `InterruptCast` (33630/34408), `DeselectSpell` (37820/38769), `GetActorValueModifier`
  (37524/38469), and `kPointerOffFromIPackageData=0x10` (SE ctor `0x3EC590` stores the
  PackageTarget at `[this+0x10]`) — the library's own SE-first bindings, not MFO-derived values.
  `plugin.cpp` (`:306`, kDataLoaded after `Forms::Resolve`) prints ONE `[runtime] <version>
  [<build>]: cast gates <open|gated (reason)>, ForceRefTo <native|fallback>, equip-slot
  <formid>` line — build `1.6.1170` / `1.5.97` / `AE (not 1.6.1170)` / the reason; a non-97
  1.5.x prints `gated (unverified 1.5.x)`, VR `gated (VR)`; error line if the EQUP form is
  missing. **What breaks if you change this:** re-gating any ONE of the five on a local
  `IsAE()`/`IsSE()` silently re-splits the runtimes (a 1.5.97 log then shows `cast gates open`
  while that path declines) — every consumer MUST call `Runtime::CastPathsVerified()`, and the
  `[runtime]` line MUST keep printing that same function or it lies. Widening the predicate to
  another 1.5.x needs that build's own disassembly (rule 11). A 1.7 path is a separate design
  decision, never an accident of the predicate. **Not touched, on purpose:** the Board input
  trampoline (per-runtime since v2.0.6, its own exact-version pair — the two predicates are
  the same test written twice; folding Board onto `Runtime.h` is a separate change), VR (refused
  everywhere), and anything 1.7.104 (no address library; the confirmed 1.7 values are recorded
  in the table for marth's `REL::Offset` decision). `Actuation.cpp` is 2678 lines — over the
  2500 cap, reported not split (rule 1; REVIEW-BACKLOG **MFO-B37** holds the split brief).
- **CAST-ROAD SELECTION BY SPELL NATURE (`fix/mfo-combat-restoration-direct`, 2026-09-21 — Deck
  2026-09-21, Jesper 750012C6).** On the COMBAT table a RESTORATION cast takes the DIRECT road and an
  OFFENSIVE cast keeps the AI-fired road. The classifier is **`Actuation::IsRestorationSpell`**
  (`Actuation_Direct.cpp:838`, declared `Actuation.h:168`): `CasterConsent::ClassifySpell != Offense`
  AND (`SpellHealsHealth` — a beneficial Health effect, the same read `ClassifySpell`'s Heal kind and
  `CastAuto` make — OR any effect whose `EffectSetting::data.associatedSkill == kRestoration`, a plain
  member read, never the `GetAssociatedSkill` vfunc). A hostile Restoration-school spell (Sun Fire,
  Turn Undead) is OFFENSE and stays AI-fired. Five sites consult it: `CastSelfDirect`
  (`Actuation_Direct.cpp:933`) and `CastTargetDirect` (`:1302`) skip `ComposedCast::Try` for a
  restoration spell and fall to their own kInstant direct force, and their two TASK 1
  concentration-offense claims (`ClaimOffenseCast`, `:974` / `:1337`) are gated on it too so a
  Restoration-school ward streams direct instead of claiming the AI-fired road (**open `MFO-B58`:**
  those two sites carry NO `combatController` test, so a non-restoration concentration Buff on an
  own-OOC party-combat follower reaches `ClaimOffenseCast` with no caster — mirror `:1302`'s
  `combatController &&` there when drained); `CastOn` forks a fire-and-forget
  restoration cast at an ally/player (`Actuation.cpp:874`, after the concentration fork, non-self
  only) to **`RestorationCastDirect`** (`:412`) = `CastTargetDirect` with the SAME outcome map, LEFT
  hand lock and `CasterConsent::Want` as `ConcentrationCast`'s non-self branch (labels `restoration
  (direct force)` / `restoration direct refresh (paced)`). WHY: the ch.8b AI-fired heal claim held
  the LEFT hand naming Fast Healing while the engine's kMultipleCast re-deliberation kept selecting
  Healing Hands and was DENIED against the claim (`t2c CheckCast DENIED`, 3x `InterruptCast`, `[cfc]
  claim live 7607 ms`), and the idle-hand floor closed the RIGHT hand under it — no dagger, no heal,
  frozen; the OOC logistics heal on the direct road delivered 22/22 the same session.
  **CONSEQUENCE, STATED:** `ComposedCast::Try` is HEAL-ONLY and every Heal-kind spell IS restoration,
  so the CFC heal claim (`ClaimHealCast`, the F1 incumbent hold, `RefreshHealCastClaim`, the
  `bHealAnimPackage` MCM toggle) is UNREACHABLE from all three of its call sites now — kept compiled,
  its removal is its own brief. `bHealAnimPackage` is inert on both tables now — on `main` it still
  reached `Try` on the OOC self road (`CastSelfDirect` asked with no `combatController` test) and on
  every combat road; only `CastTargetDirect`'s OOC ally road had the controller test. Its MCM help text
  still promises an animated hand cast (backlog `MFO-B48`). **What breaks if you change this:** re-routing any heal-kind spell
  back through `Try` re-creates the frozen follower unless the floor gate below holds; a self target
  reaching `CastOn` with `bCastSelf` OFF keeps its old road on purpose (`a_target != a_follower`).
- `CastOn` (`Actuation.cpp:600`) escalation, IN EXECUTION ORDER: runtime gate `Runtime::CastPathsVerified()` (`:432`, AE bucket or exactly 1.5.97 — see RUNTIME GATES above) →
  `:624` → `:649` → competence gate `HasSpell` (`:704`) → magicka reserve (`:742-752`) → range/competence/reserve →
  **the Task 2 firing-spell gambit lock, PER-HAND now (`ResolveCastHand`, feat/per-hand-
  cast-slots 2026-09-06 — renamed off `CheckCastLock`; TWO lock slots per follower,
  index 0=left/1=right, so a spell firing in one hand no longer holds off a DIFFERENT
  spell that would use the other. Self/concentration/Heal-Buff are forced LEFT; an
  Offense spell at a real target consults `Loadout::PlanCastHand` and juggles
  EitherFree onto whichever hand is free (DualCast needs BOTH free-or-refreshing or
  it holds off entirely — never a half-landed dual-cast). `handPlan`/`lockHands`
  thread the resolved hand(s) to every `HoldCastLock`/`ClaimOffenseCast` call site
  below in the SAME function so the lock and the actual APMF claim never disagree)** →
  **the SATISFIED-IN-FLIGHT gate (F9, 2026-09-08, `Actuation.cpp:643`)** →
  concentration fork (→ `ConcentrationCast`) → equip + **AI-first grace** (`:461`,
  follower's own AI casts first) → on miss `ForceCast` (`Actuation.cpp:74`) via `Packages::CastAt`.
- **THE PER-HAND CAST LOCK, AND THE TWO 2026-09-08 CHANGES TO IT** (`Docs/DIAG-2026-09-08-field.md`
  RC1/RC2). `CastLockLive` (`Actuation_Hands.cpp:311`) now answers "this hand is still busy" THREE ways,
  in order: (1) `ClaimLiveOnHand` (`:89`, factored out — a live offense claim on that slot, or the
  heal claim for LEFT); (2) the round-robin `FacetExpiry()` staleness window, unchanged; (3) **NEW
  (F8)** — `CastInFlightOnHand` (`:251`): the follower's own `RE::MagicCaster` for that hand is
  mid-cast (`state` not `kNone`/`kUnk08`/`kUnk09`) of the locked spell **or its APMF delivery-flip
  proxy** — read from `GetActorRuntimeData().magicCasters[]` DIRECTLY, never through the
  `Actor::GetMagicCaster` virtual (that body is the game's, CommonLib implements none of it, and
  SkyrimSE.exe is Steam-DRM encrypted on disk so it cannot be disassembled here — if it lazily allocates
  the caster, this always-on job-worker predicate would be allocating off-main; the array read removes the
  question) — bounded by `kInFlightHoldCap` (`:274`, = `APMFBridge::kHealCastTtlMs`) so a wedged caster
  cannot own a hand forever. That third answer is what stops another rule taking a hand whose charged
  cast has not fired yet when the CLAIM lapsed mid-charge. **The cap is anchored on `CastLock::claimGoneAt`
  (`Actuation_internal.h:221`), NOT on `lastSeen`** — F9 deliberately stops re-stamping the lock, so `lastSeen`
  freezes at the first claiming lap and anchoring there made the protection dead for any claim older than
  the cap. `claimGoneAt` is stamped the first time this third answer is reached, cleared when the claim is
  seen live again, and cleared by `HoldCastLock` (fresh hold = fresh cast).
  The proxy both this and the in-flight watch re-arm use comes from ONE resolver, `CastProxyOnHand`
  (`:487`): offense proxy, falling back to the HEAL proxy on the left hand — heals are LEFT always, and a
  left lookup that consults only the offense accessor answers 0 for every proxied heal, which silently
  rebuilds the RC4 "[cfc] NO observed cast" false alarm. **It takes an `RE::Actor*` now** — so do
  `HandFree` (`:925`) and `ResolveCastHand` (`:974`); every call site already had one.
  `ResolveCastHand` additionally **PINS a request to the hand(s) its own live claim already occupies**
  (the incumbent pin) instead of honouring a freshly re-derived `Loadout::PlanCastHand` answer: a
  hand-mode flip (Dual↔single, left↔right) is a CHANGE to `EnsureCastClaimLocked`, i.e.
  Release+RequestCast, i.e. the engine InterruptCasts whatever was charging.
  **What breaks if you change this:** widen the in-flight state set and a stuck caster owns a hand
  until the cap; drop the proxy match and every APMF-proxied cast reads as "not ours" and loses its
  protection; drop the incumbent pin and the Dual↔single churn returns.
- **SATISFIED IN FLIGHT (F9, `Actuation.cpp:643`, `HandPlan::inFlight` at `Actuation_internal.h:304`).** A cast rule whose
  own cast is already running (its exact (spell,target) locks the hand AND a live claim stands there)
  no longer re-runs the claim/equip/consent path and returns `Fired` — which ENDED THE SCAN and let one
  self-heal monopolise 22 consecutive laps over 44 s with zero offense rules reached. It now calls
  `APMFBridge::RefreshOwnedCastOnHand` (`APMFBridge.cpp:1245`), re-arms the `[cfc]` watch, logs one
  throttled `[eval] ... SATISFIED IN FLIGHT` line (`LogCastInFlight`, `:345`, own map
  `g_lastInFlightLog`, cleared with `g_lastLockLog` in `ClearCastLock`/`ClearCastLocks`), and returns a
  TRANSPARENT NoOp so the scan continues to the rules below.
  **What breaks if you change this:** call `lockHands` here and the lock outlives the claim it is
  supposed to be bounded by; skip the refresh and `Tick()`'s `FacetExpiry` sweep kills the live claim;
- **RANK PREEMPTION — THE GAMBIT LIST ORDER IS THE PRIORITY ORDER (marth, 2026-09-08).** A higher-ranked
  rule must be able to TAKE a hand from a lower-ranked incumbent, not be held off behind it and not wait for
  its condition to go false. **Rank is CARRIED, not inferred:** `Actuation::Fire` records the winning rule's
  index in `g_firingRule` (`Actuation_internal.h:192`) — the single entry point into every actuation in the family —
  `HoldCastLock` stamps it into `CastLock::owningRule` (`:252`) when a hand is claimed, and `CanPreemptHand`
  (`Actuation_Hands.cpp:406`) compares the two directly. Lower index == higher priority; strictly lower may take the hand,
  EQUAL is the incumbent itself (`IsOwnRetarget`, `:484` — a rule re-aiming its own cast, which fails
  `HandFree`'s incumbent match on `target`), higher is held off. `ResolveCastHand` keeps those two on
  SEPARATE predicates (`mine` vs `outranks`): a self-retarget displaces nothing and records no preempt flag,
  so it can never release its own claim through the preemption path (which for a heal would tear down
  ComposedCast's bookkeeping) or log "rule 3 outranks rule 3". Preemption's one safety condition is **never
  mid-charge**, reusing `CastInFlightOnHand` rather than inventing a second notion of busy.
  **A self-retarget carries TWO bounds, and never-mid-charge is NOT the load-bearing one:** it protects a
  cast only from the moment charging BEGINS, and the claim-to-first-charge window is 2.3-4.5 s with the
  caster reading `kNone` throughout. `IncumbentTargetLost` (`Actuation_Hands.cpp:445`) is the bound that matters —
  it asks the evaluator's own three questions (`Evaluator.cpp`'s `PickAlly`, `:354-375`, mirrored not
  invented): does the target still resolve to a live actor, is it still inside `fSharedRadius`, and — when
  the firing rule's condition is the ally selector that owns that number (`g_firingAllyThreshold`, `:254`,
  carried from `Eval::Choice::conditionParam`) — is it still strictly under `min(param, kHealFull)`. A
  target that merely lost the "lowest HP" race to another ally is NOT lost and the re-aim waits. Without
  it, two allies trading that slot as hits land restarted APMF's window every lap and the heal never
  charged at all — strictly worse than the pre-branch behaviour, where the rule was held off by its own
  lock and the first target got healed.
  **A HELD RE-AIM MUST HEARTBEAT WHAT IT IS HOLDING FOR** (`refreshHeldOwnClaim`, in `ResolveCastHand`).
  On a held lap NOTHING else touches the incumbent claim — F9's in-flight refresh runs only when
  `HandFree`'s (spell,target) match SUCCEEDS, which is exactly the match a re-aimed target fails — so
  `refreshed` stopped moving and `APMFBridge::Tick()` swept the claim at `FacetExpiry()` (~2.45 s default,
  floor 0.77 s) inside the 2.3-4.5 s claim-to-first-charge window: the hold was killing the claim it was
  protecting (principle 9, the stale-cadence class). It calls the same
  `APMFBridge::RefreshOwnedCastOnHand` the in-flight path uses, **and that renews APMF's TTL as well as
  MFO's stamp — deliberately.** The heal-hold heartbeat refuses to renew because it feeds an incumbent
  whose own rule may have gone SILENT; here the rule won its lap and is asking for the identical spell on
  the identical hand, differing only in a target MFO just decided not to honour yet, which is exactly the
  precondition `EnsureCastClaimLocked`'s renewal is written against. Refreshing MFO's stamp without
  renewing APMF's window would be two budgets that disagree in a new place. Only for a hand whose lock is
  OURS: a lower-ranked rule being held off must never feed an incumbent that never asked.
  **AND THE HEARTBEAT IS CAPPED, because renewing the TTL is only safe if the claim can still fire.**
  "Is the rule asking?" is the wrong question — nothing in `IsOwnRetarget` + `ClaimLiveOnHand` tests
  whether the claim can EVER fire, and `IncumbentTargetLost` cannot: it knows death, radius and the heal
  threshold, and **nothing about sight**. A foe that steps behind a pillar stays alive and in radius while
  `PickFoe`'s soft LoS preference names a sighted foe every lap, so an unconditional heartbeat renews a
  claim that can never fire, forever, with every lower-ranked rule queued behind it. The exits would be
  the foe dying by another hand, leaving the radius, the rule going false, or combat ending — "the cast
  fires" is not among them. The codebase already answered this one file over:
  `APMFBridge::RefreshHealCastClaim` refuses to renew AND caps a never-observed claim at
  `kHealHoldNeverObservedMs` (4000 ms), and this path reaches the same heal slot. So the hold heartbeats
  only while the claim was **observed firing RECENTLY** (`ComposedCast::ObservedFiring`, a hand-scoped,
  spell-checked read of the `observed` latch **within a caller-supplied window** — NOT `ExpectingCast`,
  which answers the opposite question) **or is younger than that same 4 s window**, anchored on `lastSeen`
  (stamped by `HoldCastLock` at the claim or re-aim, frozen after). **The recency bound is not decoration:**
  `observed` is a LATCH that no offense release path clears — not `ReleaseCastClaimOnHand`,
  `ReleaseOffenseCast`, `PreemptHand`'s offense side or the expiry sweep, and the fresh-claim site re-arms
  with the same spell, which is `WatchArmed`'s no-reset branch — so the raw latch means "this spell fired
  once on this hand this fight", across claims and across RULES. Reading it as "alive now" left the cap
  unbounded for the ordinary case (any rule whose spell had landed even once), not an edge.
  `NoteObservedCast` therefore stamps `Watch::lastObservedAt` and the window is compared against that.
  **The real bound is the sum, not the 4 s:** feeding stops at 4000 ms, then the claim dies when `Tick()`'s
  sweep sees `refreshed` go stale at `FacetExpiry()` — so the hold stands ~4.8 s (0.77 s floor) to ~6.5 s
  (~2.45 s at the defaults). A cast already charging is exempt either way (`inFlight`).
  The hold refresh also names its SPELL (`a_holdSpell`), because on the LEFT an offense claim and a heal
  claim can stand together and a held offense re-aim must not renew a coexisting heal past its own
  never-observed cap (that cap gates `RefreshHealCastClaim`'s answer and reads `created`; the claim's LIFE
  is `refreshed`, which this call bumps). That caps how long a
  rule waits on its OWN silent claim; it delays no higher-ranked rule, which is the opposite direction
  from the overruled tenure. It also **re-arms the `[cfc]` watch** on every held lap
  (`ComposedCast::WatchClaim`), because the "NO observed cast" warning is emitted only from `WatchArmed`
  with no pump — without it the sole line a held re-aim produced was `LogCastLockHold`'s "still firing",
  asserting a fire that may never have happened, on the one path that renews such a claim. A hand whose
  own target IS lost is fed only while its cast is genuinely in flight (otherwise a `DualCast` hold, which
  triggers when the WHOLE plan fails, would keep renewing a single-hand claim aimed at a dead actor just
  because the other hand is taken). `RefreshOwnedCastOnHand`'s `a_forHold` also refuses on ABI < 6, for
  the same reason `RefreshHealCastClaim` does: with no `IsClaimLive` the refresh would bump MFO's stamp
  for a claim APMF may have expired and stop the only sweep that could end the hold.
  **An earlier cut inferred rank from scan arrival ("did the incumbent assert itself earlier this lap?") and
  was replaced, not tuned:** a rule whose condition is TRUE but which exits transparently BEFORE the hand
  gate (#68 out-of-range, the magicka cost/reserve exits, `HasSpell`) never asserts at all, so two such rules
  took the hand from each other on alternate laps and neither ever charged; and a retargeting rule outranked
  itself. The lap counter, `BeginCastLap` and the `Scheduler` call site that fed it are all gone.
  **The displacement is DEFERRED, never done at resolve time:** `ResolveCastHand` only records
  `HandPlan::preemptLeft/preemptRight` (`:961`), and `CastOn`'s `commitPreempt` (`:1759`) fires immediately
  before the claim it is for — the self fork, the concentration fork, `ClaimOffenseCast`, `ComposedCast::Try`
  and (for the APMF-absent world, where `Loadout::Prepare` IS the claim on the hand) the equip switch. Every
  transparent refusal in between therefore refuses without having touched the incumbent; committing at
  resolve time turned a deterministically-failing higher rule into a dead hand at lap rate.
  `PreemptHand` (`:873`) then releases via `APMFBridge::ReleaseCastClaimOnHand` (`APMFBridge.cpp:1357`,
  per-hand, **offense slot only**), routes a heal-backed release through `ComposedCast::End` when
  `APMFBridge::GetHealCastSpell` (`APMFBridge.cpp:1571`) says the lock being displaced IS the heal claim
  (otherwise an unrelated coexisting heal would be dropped, and its watch/bounds/hold left armed for a claim
  that no longer exists), clears the lock, and — for a MIRRORED DUAL incumbent (both hands, same
  spell+target+owningRule) — clears BOTH locks, because one APMF handle on two hands has no half-release and
  `HandFree`'s incumbent short-circuit does not consult liveness.
  **STRUCTURAL RESIDUAL — deferring the commit NARROWS the window, it does not close it.** Everything that
  can still refuse the asker AFTER `commitPreempt` — `CastSelfDirect` → `Held`/`Declined`, a refused
  `ClaimOffenseCast`, `ComposedCast::Try` → `Held`/`ApmfRefused`, a failed `Loadout::Prepare` — still
  displaces the incumbent first, so a DETERMINISTIC refusal of that shape still churns at lap rate. Closing
  it generally needs an APMF dry-run ("would this claim be granted?") that does not exist. The one candidate
  for a pure MFO pre-check, `Try`'s `Held`, is NOT cleanly pre-checkable: its deciding term is
  `APMFBridge::RefreshHealCastClaim`, which is side-effecting (it bumps `refreshed` and latches `everLive`),
  so only the first three `g_watch` terms can be evaluated purely — and a "might hold → do not displace"
  answer would trade this bounded churn for a SILENT arbitration loss (the newcomer claims while the
  incumbent still stands, and APMF's equal-basis tie keeps the earliest). In the case that actually matters
  it cannot fire at all: preempting a heal routes through `ComposedCast::End`, which clears `g_watch.hand[0]`
  before `Try` runs, so the incumbent is 0 and the hold is impossible by construction. The residual is the
  narrow one F4 deliberately left: an OFFENSE claim displaced from the left while an unrelated heal claim
  coexists.
  **What breaks if you change this:** compare anything but the carried index and the pre-gate transparent
  exits fool it again; drop the mid-charge guard and the charged-cast loss returns; drop
  `IncumbentTargetLost` and a flickering ally target starves the heal it was picked for; commit the
  displacement at resolve time and a failing rule kills a working one; release the heal slot from the bridge
  and ComposedCast is left holding bookkeeping for a dead claim.
- **AUTO PREFERS THE UNCLAIMED HAND ("there are two hands, auto should figure it out" — marth).**
  `ResolveCastHand`'s `EitherFree` case took the LEFT hand whenever it was free, which is where the
  collisions came from: the heal facet is LEFT-ONLY by contract (`ClaimHealCast`'s hard rule), so an offense
  cast idling on the left stands exactly where the next heal must go while the right hand sits empty. It now
  prefers the RIGHT hand — free-and-unclaimed first, then free, then by rank — leaving the left for the
  facet that can use no other, **unless `WeaponHandExposure` (`Actuation_Hands.cpp:163`) says a weapon owns the
  right hand or is coming back to it**, in which case the old LEFT-first order stands. That gate is not
  optional: `PlanCastHand` returns `EitherFree` only when `APMFBridge::WeaponHandActive` is false, and that
  reads the LIVE grip and the live equipment CLAIM — both false during the documented transient-unarmed
  facet-expiry gap (`Docs/CAST-DELIVERY.md`, the 2026-09-05 HAND FIX: auto took the right hand, the equip
  gambit re-equipped ~500 ms later and displaced the spell, the cast never left rest). `g_forcedWeapon` —
  this file's own T#76 force-hold ledger, released when the equip gambit's condition is known false **and
  never written at all while `bWeaponStyleControl` is off** (see the residual below) —
  is the one signal that survives that gap. A pure caster never has an entry, so right-first still applies
  to exactly the follower marth's ruling was about. (`DualCast` still takes both hands in that gap; that
  exposure predates this branch and is unchanged by it.)
  **RESIDUAL, non-default config:** the ledger is only WRITTEN while `bWeaponStyleControl` is on —
  `EquipWeapon`'s kill-switch-off branch is a plain `EquipObject` with no entry, and
  `ReconcileForcedWeapon` releases unconditionally when the switch is off. With that feature off a melee
  follower in the transient-unarmed gap therefore still lands RIGHT, i.e. the 2026-09-05 shape on a
  non-default setting. Recorded, not closed: closing it needs a signal that does not depend on that
  feature being on.
  `g_forcedWeapon` is read under `g_forcedMx` here like every other access — the map has an OFF-THREAD
  reader (the SKSE save callback, `CoSaveForcedWeapons`), so "the writers are worker-serial" is not
  sufficient. Its declaration comment used to assert BOTH "no lock (#4)" and "guard every access"; the
  no-lock half is now removed rather than left to mislead the next reader. (A first cut only preferred the right once the left was
  already CLAIMED, which never fires in the flow that matters: a standing heal claim makes `leftFree` false
  long before that test is reached, so the opening state — two free unclaimed hands — still took the left.)
  **KNOWN CONFLICT AT THE EQUIPMENT LAYER, outside this file's boundary and NOT fixed here:**
  `Loadout::Prepare` equips into `LeftHandSlot()` unconditionally (`Loadout.cpp:279, :359`) and at cast
  level 4 `DeselectSpell`s the RIGHT hand's other spell (`:296-298`). A RIGHT-hand claim therefore has MFO
  physically equipping the same spell into the LEFT hand — a wasted equip, and on a follower holding a
  shield or weapon there, a needless displacement recorded as a restore debt. `Prepare` needs a hand
  parameter; that is its own brief. `DualCast` may also preempt, but only when BOTH hands are takeable (checked
  as a pure predicate before either is committed, so a released claim is never spent for nothing).
    make it opaque (transparent=false) and RC2 comes straight back. If the refresh comes back FALSE the
  claim really is gone, so the pin that named it is VOID: the gate drops those hands' locks
  (`ClearCastLockHand`, `Actuation_Hands.cpp:77`) and re-derives the plan (`resolveHands` lambda) before
  falling through — without that, a LEFT-always heal pinned to the RIGHT by a stale lock would claim,
  equip and lock the wrong hand.
  On an unverified runtime (VR, a non-97 1.5.x) the whole path declines transparently (the
  former T#67 AE-only gate; opened for exactly 1.5.97 on 2026-09-15 — see RUNTIME GATES above)
  so vanilla AI keeps casting.
  **The FF silent cast (and every other `CastSpellImmediate` on a live path) is now
  `MainThread::Post`ed** — CastOn runs on the job worker and the old inline engine call
  was the prime suspect for the queued 1.5.x `act.cast_target` AV reports (#14).
- **COMBAT PICK + DUAL WIELD BY PERKS (2026-09-13, branch `feat/mfo-dualwield-combat-pick`,
  marth: "both gaps are clear to fix right away"; Fable tier-3 on `f771399` F1–F5 fixed in `1ac3c6b`).**
  `EquipWeapon` (`Actuation.cpp:1765`) now CONSUMES the weapon-style decision the loot/keep/buy
  judges already share: the pick inside the ordered class is `Logistics::WeaponScore(roles, w)` with
  `roles = Logistics::ComputeWeaponRoles(actor, g_followers[id])` (`WeaponRolesFor` `:1660`,
  worker-serial `g_followers` read, #4; no record → default roles). **`Actuation.cpp` therefore
  includes `Logistics_internal.h` (`:13`)** — its first non-Logistics include; `ComputeWeaponRoles`/
  `WeaponScore` are NOT in `Logistics.h` (REVIEW-BACKLOG **MFO-B19**: the proper seam is a public
  declaration there). DEFAULT-CASE PROOF: `preferKinds==0` → score == `float(uint16 damage)`, same
  `>=` last-equal-wins loop, same inventory order → the same weapon as before. The melee CLASS is
  NOT a filter here (the pick still spans 1H+2H by score, as it always did; `roles.melee` is the
  higher SKILL, so a two-handed-by-skill follower carrying only one-handers still draws one).
  Ranged never has a preferred kind. **THE LEFT HAND:** when the right-hand pick is a one-hander
  (`IsOneHandMelee` `:1668` = the four `IsOneHanded*` tests) and `roles.offHand` votes: `2` →
  `PickOffHandWeapon` (`:1678`, SAME `WeaponScore`, excludes the right hand's ONLY copy — count ≥ 2
  of the same form is allowed — same eligibility: no staff, no non-playable, daggers only for a
  `bMageDaggersOnly` base mage) is FORCE-HELD in the left via `EquipLeftHeld` (`:1762`:
  `EquipObject` with `Loadout::LeftHandSlot()`, `forceEquip=true`, ledger `.left` written under
  `g_forcedMx`, engine calls outside it; **since round 2 of `feat/mfo-1.5.97-pass` it takes an
  optional `const char** a_whyNot` and a refused precondition — follower / equip manager /
  left slot form / weapon null — is an ERROR line `[hold] <id>: EquipLeftHeld REFUSED -- <what>
  null`, once per follower per reason via `g_leftHeldRefusal` (`:1753`, worker-serial, erased
  on a success, on `ReleaseForcedWeapon`, and in `ClearForcedWeapons`). Both callers consume
  the verdict: the pick path's `[equip] GAMBIT equip ...` line says `+ off-hand '<w>' (dual
  wield by perks)` ONLY when the hold happened and `off-hand '<w>' NOT held (<what> null)`
  otherwise; the top-up prints its own `NOT held` info line. The Deck 2026-09-14 log showed
  five `+ off-hand 'Ebony Dagger'` claims with zero `[hold]` lines — the hold had returned
  false every time (the null AE slot, SEV-2 above) and nothing said so. `g_offHandRetryAt` is
  stamped BEFORE the attempt on purpose (a cadence floor, principle 9), never a success
  record; the only success state is the ledger `.left` the function writes itself.**); `1` → `PickShield` (`:1695`, best `GetArmorRating`)
  PLAIN-equipped **on the main thread** via `EquipShieldOnMain` (`:1715`, F3/#62: FormIDs captured,
  re-resolved under `MainThread::Post`, VR inline — the `Logistics_Loot.cpp` equipIt precedent; no
  ledger entry, the AI keeps shields on its own); `0` → nothing. Plus a SATISFIED-LAP TOP-UP
  (`:1805-1844`): right already holds a one-hander, left holds no weapon, `!CastHandHeld(left)`,
  **`!Loadout::OwesLeft(id)`** (`:1820`; Fable F-A on `1ac3c6b`: a heal that displaced the AI's
  own shield/off-hand booked a LEFT gear debt; a prevent-removal hold placed over that unpaid debt
  can never be settled by the AI and `Prepare`'s "already owe this follower gear" gate then
  Debounces every later cast until combat end — heals dead for the fight; the pick path's left
  WEAPON hold is gated the same way `:1884`, the shield plain-equip is not), rate-limited by
  `g_offHandRetryAt`/`kOffHandRetry` 5 s (`:1762`). `Loadout::OwesLeft` (`Loadout.h:195`,
  `Loadout.cpp:672`) is a pure read of the worker-serial debt map (`displacedLeft != null`).
  With both left weapon holds refused over an open debt the hold+debt coexistence has NO writer
  (Prepare books no debt when the yield returned true, and the yield always runs when a hold
  stands), so Fable F-B's combat-end / dismissal repay-under-lock cannot arise. **The top-up is TRANSPARENT
  (F5):** both refills fall through to the satisfied lap's `NoOp`, never `Fired` — a `Fired` there
  armed the Scheduler's suppression window (heals below the equip rule starved ~1.5 s per refill),
  stamped `lastFired` and fed `ProgAllocator::NoteCombatFire`. Every left-hand write is gated on
  `bWeaponStyleControl`. **THE LEFT WEAPON ALWAYS YIELDS TO A LEFT-HAND SPELL — marth's DECISION
  (2026-09-14, verbatim: "for F6 the weapon should always yield to a spell on left hand";
  REVIEW-BACKLOG MFO-B18):** heals are left-only and `WeaponHandExposure` steers casts LEFT while a
  hold exists, so a dual-wielder's left weapon yields and refills at cast cadence — accepted by
  design; no right-hand cast preference and no "dual-wielder does not cast" rule may be added.
  **WHERE the yield happens (F1):** NOT in `CastOn`'s commitPreempt (`:719`, which runs ahead of
  refusals that still return transparently — an APMF claim refusal, a heal `ComposedCast` holds
  off, a `Prepare` that debounces/fails — and a yield ahead of a refusal flickered the left hand
  weapon→spell→weapon per lap). It happens INSIDE `Loadout::Prepare`, immediately before each of
  its two `EquipSpell`s (`Loadout.cpp:304`, `:391`), through the `LeftHandYield` function-pointer
  parameter (`Loadout.h:128`) that `CastOn` passes (`Actuation.cpp:1047`) as a non-capturing lambda
  over `YieldForcedLeftHand` — keyed to the PHYSICAL hand `Prepare` takes (always LEFT), not to
  `handPlan.left`. `YieldForcedLeftHand` (`:2365`) returns `bool` (a hold was released) and
  re-stamps `g_offHandRetryAt` to `now + kOffHandRetry` — a FLOOR (principle 9), not an erase — so
  a refused cast costs at most one flicker per 5 s. `ReconcileForcedWeapon` (`:2421`) still yields
  when `CastHandHeld(left)` is live (covers the ComposedCast heal claim, which never passes
  `CastOn`), ORDERED BEFORE its release/claim decision. **ONE LEDGER PER HAND (F2):** `Prepare`
  uses the yield's `true` return to null `willDisplaceLeft`, so MFO's own hold is never booked as
  gear debt (the queued unequip is invisible to `Read()`'s synchronous view, which would otherwise
  book the yielded weapon and repay it in the RIGHT hand at combat end); and `RestoreOne`
  (`Loadout.cpp:98`) returns a displaced LEFT-hand WEAPON with `LeftHandSlot()` (`EquipBack` `:88`
  gained a slot parameter; shields/stowed two-handers unchanged). On any NEW right-hand equip an old
  left hold not in the plan is unequipped FIRST (a bow or two-hander needs that hand).
  `ReleaseForcedWeapon` (`:2320`) releases BOTH hands; `ClearForcedWeapons` (`:2460`) clears
  `g_offHandRetryAt` too.
  **STAND DOWN = SHEATHE, NOT UNEQUIP (`a_standDown`, marth 2026-09-22: "no one ever sees an unarmed
  follower, they sheathe weapons. Not unequip.").** `ReleaseForcedWeapon(a_follower, bool a_standDown
  = false)`. The force-unequip is UNCHANGED and is KEPT, but **NOT for the reason this entry and the code
  both used to give. "A plain unequip is REFUSED against a forced item" is FALSE — corrected 2026-09-22
  from disassembly (Fable spot check):** `UnequipObject`'s dispatcher (AE `0x6CB550` @ `0x6CB5DA`) clears
  `ExtraCannotWear` **unconditionally**, with a constant zero, before it even reads the force byte — so a
  plain unequip releases the lock exactly as a forced one does and "stuck holding it forever" was never
  reachable here. The only non-forced refusal on this path is on the **EQUIP** side (`0x69FB2A`): an equip
  with `!forceEquip` is refused when the CALLER-SUPPLIED extraData carries `kCannotWear`, and MFO passes
  `nullptr`, so that gate never applies to us. The flag stays because it is harmless, because every
  release path in the file passes one shape, and because the lock must be gone before the AI may re-arm —
  **not** because it prevents a freeze. Lock facts (what it is, where it is written, why the queued path
  cannot set it): `Docs/ENGINE_NOTES.md` §0.48. What the
  unequip also costs is VISIBILITY: an unequipped weapon stops being drawn on the body at all, where a
  SHEATHED one is still worn — which is the "weapons vanishing and returning" the party-OOC teardown
  note at `Scheduler.cpp` already records. So with `a_standDown` the SAME weapon(s) go back on
  **NON-FORCED** (`EquipObject(..., a_forceEquip = false)` — no lock, the AI owns them again) and the
  follower is sheathed (`Actor::DrawWeaponMagicHands(false)`, vfunc 0A6, pinned
  `include/RE/A/Actor.h:360`; already used by `Loadout.cpp:341/429` and `Probe.cpp:270`), gated on
  `!IsInCombat()` so a stand-down racing a fresh fight never puts a drawn weapon away.
  **A TENSION, STATED:** `Docs/GAMBIT_FLAIR.md:299` rejected a post-combat sheathe flourish with
  "sheathing is the vanilla AI's; sending sheath/pose animation events against it is a tug-of-war with
  the engine". This is not that — it is the engine's own call, not a graph event, and it is made only
  OUT OF COMBAT where the AI wants the weapon sheathed anyway. And it is not the part that delivers
  the fix: the RE-ARM is (an equipped weapon is drawn on the body, an unequipped one is not), so the
  sheathe line is deletable on its own if it ever proves to contend. Runs on the
  SAME double-`MainThread::Post` idiom as `LogLeftHandReadback` (the unequip is a QUEUED engine op, so
  re-equipping in the same breath would race it; and #62 puts any equip on the main thread). Re-resolved
  on the frame that runs; skipped for a dead/disabled follower, for an item no longer owned, and for a
  hand something has ALREADY armed — it re-arms, it never evicts. **THE ONLY SITE THAT PASSES TRUE** is
  `Scheduler.cpp`'s party-OOC 2-tick teardown (the fight is genuinely over and no specific item has to
  leave the hand). Everything else keeps the default `false`, deliberately: the dead/disabled teardown
  (`Scheduler.cpp:387`, the item must leave so a resurrection is not stuck), `ReconcileForcedWeapon`'s
  release branch (`Actuation.cpp:~2737` — mid-combat kill-switch / category flip, where re-arming the
  old category would fight the next gambit), `Followers.cpp:382` (dismissal — OUT OF this change's file
  boundary), `YieldForcedLeftHand` (the hand must be FREE for a spell), the four swap unequips
  (`:1853/:1890/:2169/:2171` — a replacement item takes the hand in the same breath) and
  `CoLoadForcedWeapons` (clearing a stale lock on a possibly-3D-absent actor; an equip there is the
  #62 invisible-head class). **What breaks:** remove the force-unequip and the lock survives (a
  follower who can never cast); use `forceEquip = true` on the re-arm and the lock comes straight back;
  run the re-arm on the job worker and it is a 3D rebuild off the main thread; pass `true` from a
  mid-combat release and MFO re-arms a category its own next gambit is about to replace.
  **OPEN BACKLOG — read before editing:** `MFO-B64` (`CoLoadForcedWeapons` still leaves a loaded follower
  empty-handed; re-arming there needs an `Is3DLoaded()` gate it does not have).
  **FIELD OBSERVABLE THE NEXT DECK RUN MUST SETTLE (a).** `DrawWeaponMagicHands(false)` IS honoured —
  vfunc `0xA6` -> AE `0x662F10`, and the `a_draw = false` branch fires the engine's OWN sheathe graph
  event and notifies the AIProcess, the same entry the engine's post-combat sheathe uses (disassembly,
  2026-09-22). What could NOT be settled statically is the **post-combat search/alert window**: a stretch
  where `IsInCombat()` is already false but the AI still wants the weapon OUT. So: after an
  `[equip] <id>: stand-down re-arm` line out of combat, **does the follower visibly RE-DRAW within ~1 s?**
  If he does, the sheathe is contending with the AI in that window and the one
  `DrawWeaponMagicHands(false)` line is deletable on its own (the re-arm, not the sheathe, is what
  delivers the fix). Purely cosmetic either way — it cannot disarm anyone. **LEFT-SLOT RELEASES + READBACK (F4, principle 5):** every left-hand
  unequip names `Loadout::LeftHandSlot()` (Yield, Release; `CoLoadForcedWeapons` `:2538` picks the
  slot from the LIVE hands because a v1 pair carries no hand — left slot when held left, default
  when held right, BOTH for a same-form count≥2 dual hold, the old slot-less call when held in
  neither), and `LogLeftHandReadback` (`:2306`) posts a `[hold] <id>: left readback = <form|none>` after every
  Yield and after a Release that had a left hold — posted TWICE (Fable F-C on `1ac3c6b`: the
  unequip is queued on the FOLLOWER's AI process and drains in its update, MainThread's queue drains
  in the PLAYER's Update vfunc, and their in-frame order is not established; a repost lands next
  frame per `MainThread.h`, so the read is one full frame boundary after the drain either way).
  Readings: a spell = conclusive, none = informative, the weapon = the lock survived (no longer
  confusable with a stale read). **The first dual-wield field run must read that line**: whether the slot-named
  unequip clears the left prevent-removal lock, and which instance a same-form count≥2 hold clears,
  are UNVERIFIED engine behaviour (`Docs/STATUS.md`). **CSTY:** `MFO_MeleeStyle` DATA = `1|4`
  (dueling | `kAllowDualWielding` = `TESCombatStyle::FLAG 1<<2`, pinned 3.7.0 header;
  `MFO_GenerateESP.py:1124`, `_csty_record` gained `data_flags` `:1043`) — `out/MFO.esp`
  regenerated, exactly ONE byte changed (offset 6743, `0x01→0x05`), FormIDs unmoved. Census of all
  145 Skyrim.esm CSTY records: 22 carry DATA bit 2, 7 of them with NO record-header bit 19, so DATA
  is the word the engine honours. **EXPOSURE, OPEN DECISION FOR MARTH (`Docs/STATUS.md`):** the
  flag is GLOBAL to every follower forced melee — an UNPERKED follower carrying two one-handers may
  be dual-wielded by its OWN AI (`EquipGateThunk` denies spells/staves only); not deny-complete; a
  per-follower CSTY needs `Forms.h` + `CombatStyle.cpp`.
  **SHED INTERACTION (traced 2026-09-14, no fight):** `ShedOffRoleWeapon` (`Logistics.cpp:506`)
  never reads the ledger — it does not need to, by ORDERING: the equip gambit fires only in
  `Scheduler.cpp`'s party-combat branch, the party-OOC branch calls `ReleaseForcedWeapon`
  (`:384-385`, both hands, after 2 party-OOC services) BEFORE `ServiceFollower` (`:393`), and
  the shed additionally waits `kShedPostBattleDwell` 3 s (`Logistics_internal.h:122`) past
  the last PARTY-combat stamp (`NoteInCombat` `:409`, stamped on every party-combat service
  since 2026-09-21 — an own-OOC follower inside a party fight now keeps his hold AND runs
  `ServiceFollower` via `serviceOwnOoc`, so the dwell must key on the party fight). So the
  shed only ever sees an UNHELD pack; the top-up runs only in
  combat, picks from `GetInventory()` (a dropped weapon is gone), so it cannot re-equip what the
  shed dropped; and a second one-hander is in-role whenever `roles.melee == OneHand` (or the
  magic-user sidearm rule) — `inRole` (`Logistics.cpp:567`) is per class with no count cap. A
  two-handed-BY-SKILL follower dual-wielding one-handers has BOTH judged off-role post-battle,
  exactly as the right hand alone was before this branch. **What breaks:** moving the yield back
  ahead of a transparent refusal (into commitPreempt, or before `Prepare`'s cooldown/debounce/debt
  returns) or erasing `g_offHandRetryAt` on yield re-creates the F1 per-lap weapon↔spell flicker;
  a change that stops the left weapon yielding to a left-hand spell (a right-hand cast preference,
  a "dual-wielder does not cast" gate) BREAKS marth's F6 decision; booking `willDisplaceLeft` when
  the yield returned true, or dropping the `LeftHandSlot()` from `RestoreOne`'s weapon repay,
  re-creates F2 (the repay lands MFO's released weapon in the RIGHT hand at combat end and the open
  debt Debounces every later `Prepare`); a `Fired` from the top-up re-creates F5 (heal starvation +
  false `NoteCombatFire`); equipping the shield inline on the worker re-creates F3 (#62 render-thread
  race); dropping the slot from a left unequip returns to the F4 unverified path; running the shed
  while a hold stands (moving it into the combat branch, or dropping the OOC release before
  `ServiceFollower`) makes the shed's `DropObject` meet the prevent-removal lock and leaves a
  dangling ledger entry; `IsOneHandMelee` admitting a staff/bow makes `EquipLeftHeld` force a
  two-hand form into the left slot; writing `.left` without the `g_forcedMx` lock races the save
  callback (`CoSaveForcedWeapons` walks the map off-thread); `PickOffHandWeapon` losing the "only
  copy" test makes the engine MOVE the right-hand weapon to the left; a `WeaponScore` divergence
  between this file and `Logistics_internal.h` is impossible by construction (one inline) — do NOT
  re-implement it here; placing a left WEAPON hold without the `OwesLeft` gate re-creates F-A
  (heals dead until combat end) and re-opens F-B; a single `MainThread::Post` for the readback
  re-creates the F-C stale-read ambiguity. Backlog: MFO-B22 (shield log precedes the posted
  equip), MFO-B23 (CoLoad same-form second unequip is slot-less; no `RightHandSlot()` exists).
  **FWPN co-save LAYOUT UNCHANGED v1:** a dual hold writes TWO (follower,
  weapon) pairs; `CoLoadForcedWeapons` has always released per pair by object (now slot-named from
  the live hands, F4); `kMaxForcedWeapons` 64 is now a PAIR cap and the reader aborts above it
  (REVIEW-BACKLOG **MFO-B20**, unreachable at party scale). **KNOWN GAP (economy/loot, other
  files):** the economy keep buckets keep ONE 1H form, so a DIFFERENT second one-hander sells at
  the next vendor; loot never fetches a second one-hander. **PERF NOTE (MFO-B21):**
  `ComputeWeaponRoles` now also runs per pick lap and per 5 s top-up attempt — negligible at party
  scale. **NOT FIELD-VERIFIED:** whether the AI attacks with the left weapon; the F4 readback
  questions above.
- **UNDER THE APMF EQUIP AUTHORITY (ch.17, ABI v8; `feat/mfo-equip-authority` 2026-09-15, PORT
  #1 of an MFO engine mechanism into APMF) THE DECLARATION IS THE EQUIP PATH — APMF PLACES THE
  HANDS.** Per follower, `authority = APMFBridge::EquipAuthoritySupported() &&
  APMFBridge::ClaimEquipAuthority(id)` is read on the Fired lap (`EquipWeapon`, inside the
  `bWeaponStyleControl` block) and on the satisfied-lap top-up (a refused claim = APMF's seat not
  installed = `authority` false = the legacy block below, byte-identical). With it TRUE: (1) MFO
  makes NO equip call — the old holds are still force-UNEQUIPPED (they may carry MFO's own T#76
  prevent-removal lock from a legacy-path hold, which APMF's non-forced equip could not displace;
  unequips are never seated), the ledger is written exactly as before (`g_forcedWeapon[id].right =
  best`; the left through `RecordLeftHold` (anon, beside `EquipLeftHeld`): ledger `.left`, and the
  force-unequip of a DIFFERENT weapon MFO had locked there); (2) `DeclareFromLedger` (anon) reads
  the ledger under `g_forcedMx`, the `FollowerState` off `g_followers` (worker-serial,
  `WeaponRolesFor`'s road) and `CastHandHeld(left)`, and calls
  `Logistics::RefreshEquipDeclaration(..., judgeArmor=false, ...)` (Logistics_Economy.cpp) which
  SENDS the set iff it changed — `.right` as `kEquipSlot_Right`, `.left` as `kEquipSlot_Left`,
  so APMF's own pass (`SetEquipSetEx`, v8) places each weapon in the hand the entry names; (3)
  `LogLeftHandReadback` (two hops) is posted after a left declaration — the probe's proof that
  APMF's `hand=left` equip reached the engine (APMF criterion 7). RETIRED in round 3 (v8): the
  round-1/2 `PostHandEquipsDeferred` double-hop placement and `EquipLeftHeld`'s deferred mode —
  they existed only because v7's slot-less pass put a second one-hander in the RIGHT hand
  (`Loadout.cpp` `EquipBack` doc, Fable F2). `EquipLeftHeld` is LEGACY-ONLY again. **SCOPED (ABI
  v9, `feat/mfo-equip-authority-v9` 2026-09-16):** the shield branches (`EquipShieldOnMain`, the
  gambit equip's `offHandSh` arm and the 5 s top-up's `offHand==1` arm) are ALWAYS DIRECT again,
  authority or not — Shield is never an owned category, so the direct equip passes the seat
  (`owned=0`); the top-up no longer calls `DeclareFromLedger` for a shield. `DeclareFromLedger`
  is unchanged in signature: `RefreshEquipDeclaration` computes the SCOPE from the ledger it is
  handed (a hand is owned only while a hold stands; see the Logistics_Economy entry) and sends
  `SetEquipScope` before `SetEquipSetEx`. `ReleaseForcedWeapon` /
  `YieldForcedLeftHand` / `ReconcileForcedWeapon` / the ch.15 `ClaimEquipment` are UNCHANGED (the
  force-unequips on release clear locks where they exist and are plain unequips otherwise; ch.15's
  param form is unaffected by a ch.17 claim, APMF INTEGRATION.md). After a release no refresh is
  sent: the next OOC `ServiceFollower` reads the (now empty) ledger through
  `Actuation::ForcedHoldFor(id)` → `{right, left}` FormIDs under `g_forcedMx` (the ONE public
  ledger read; Fable F1 on `7857446`) and declares `owned=Armor`, so the AI re-arms (F6 c) — and
  while a hold STANDS the OOC road keeps owning its hand: `ServiceFollower` runs on ANY OOC
  service (the T#76 two-tick debounce guards only `ReleaseForcedWeapon`), so passing 0/0 there
  let a one-tick `IsInCombat` flap demote a standing hold to Armor-only for the rest of the fight
  (nothing re-declares until the next equip event). **What breaks:** passing 0/0 (or any
  constant) for the holds on ANY road re-creates that F1 demotion; declaring
  a one-hand weapon with `kEquipSlot_Default` re-creates F2 (the engine puts it in the RIGHT hand)
  and under v9 competes for BOTH hands (equipped only when both are owned); routing the shield
  back through the declaration re-freezes the left hand (Left would have to be owned for the pass
  to place it, and an owned Left refuses the AI's own bow/two-hander); gating the shield's direct
  equip on `!authority` again leaves an offHand==1 follower shield-less under the authority;
  force-EQUIPPING under the authority puts a lock APMF's pass cannot displace on the next swap and
  reaches the seat as `External(MFO.dll)`; dropping the old-hold force-unequips leaves a legacy
  lock standing across the toggle's OFF→ON; a forced re-declaration from the top-up (F3) is a
  declaration event per 5 s; declaring from the worker BEFORE the ledger write declares the OLD
  hold; skipping `ClaimEquipAuthority` on the combat lap leaves a follower first seen in combat
  unclaimed until combat end. **Direct equips that remain under the authority (`Docs/STATUS.md`
  "to migrate")**: the `bWeaponStyleControl=0` plain `EquipObject` (`EquipWeapon`'s kill-switch
  branch), `Loadout.cpp` `EquipBack` (the cast-debt repay; in-set when it repays a declared item),
  `Logistics.cpp` `EquipTorch` (v9: skips while Left is owned, `APMFBridge::EquipAuthorityOwns`;
  otherwise an unowned-category equip the seat allows) and `HealExcludedWeapon`'s best-weapon
  re-equip, `AcquireEquip`'s WEAPON equip-in-place hop (v9: passes when no hold owns the hand).
  `DrinkPotion`'s `EquipObject(potion)` is NOT governed (v8: only ARMO/WEAP/AMMO/LIGH are). **BOUND WEAPONS (round 4, `fix/mfo-equip-authority-bound`):** the
  engine's `BoundItemEffect` equips a conjured WEAP through `EquipObject` (seat path `BoundItem`)
  — off-set, refused under enforcement — so `ReconcileForcedWeapon` (every in-combat lap, hold or
  no hold) compares `Logistics::LiveBoundWeapons` lap to lap (`g_boundSeen`, anon) and calls
  `DeclareFromLedger` on a change; the builder's rule 1b declares each live bound weapon (see
  the Logistics_Economy entry). **MFO-B41 fixed:** `DeclareFromLedger` mirrors the ledger it
  last declared (`g_lastLedgerDeclared`); unchanged ledger + its weapon in neither hand →
  `Logistics::ResendEquipDeclaration` (drops the change detector only) so the set is re-sent,
  throttled by `kB41Hold` 3 s (APMF's per-item re-issue hold); all three maps cleared in
  `ClearForcedWeapons`. **What breaks:** trigger it from a seat that only runs while a hold
  stands and the AI's own Bound Sword is never declared; drop the throttle and an observe-mode
  take-back re-sends every lap. Observe-only this cycle (APMF ships `[EquipAuthority]
  bEquipObserveOnly=1`; the claim line prints `mode=observe-only|ENFORCE` from APMF's v8
  `IsEquipAuthorityEnforced`).
- `ConcentrationCast` (anon, `Actuation.cpp:470`, COMBAT) = self→`CastSelfDirect`; non-self→
  **`CastTargetDirect` (DIRECT FORCE, PRIMARY — the package delivery is REMOVED)**.
  Latches `CasterConsent::Want` on each Applied so the slider keeps denying competing
  AI spells and the AI's own unbounded concentration attempt; the direct apply itself
  bypasses both consent hooks (ENGINE_NOTES §0.13 — `CastSpellImmediate` skips the AI
  cast pipeline they sit on), so deny-the-AI + deliver-directly is coherent, never a
  lockout. No bForceCastOnMiss/bUsePackages gate, no Loadout cooldown (the channel
  self-paces; a 4 s cooldown hole would let the ~2 s stale window tear it down).
  Both its Applied/Refreshed branches now also call `HoldCastLock(id, kHandLeft, ...)`
  (PASS H; hardcoded LEFT — concentration always is) so a live stream is protected by
  the SAME per-hand lock `CastOn` checks up front (feat/per-hand-cast-slots).
- `CastTargetDirect` (PUBLIC, `Actuation_Direct.cpp:1141`) = `CastSelfDirect` generalized to a NON-self target: the
  known-working DIRECT FORCE (`CastSpellImmediate` onto the target + magicka deduct, NO
  package → beats the `§4.6` lock). Registry `g_targetCast`; concentration re-applies
  at `kConcApplyPeriod` (~1 s, the heal cadence contract — per-second authored
  magnitude AND cost), FF at fCastCooldown; bounded/released by `TargetCastReconcile`.
  Callers: **Logistics OOC dispatch AND combat `ConcentrationCast` — BOTH primary**
  (marth's ruling: always the known-working force). LoS+LoF gate hostile offense on
  every apply. (The c539257 `CastConcentrationAt` package wrapper was REMOVED — it
  caused the Lucien OOC-heal regression.) **PASS H (feat/cast-gambit-concentration):**
  right after the existing `ComposedCast::Try` call (HEAL-ONLY, unchanged), both this
  function and `CastSelfDirect` now try `APMFBridge::ClaimOffenseCast` directly
  (`concentration=true, stopPct=0`) for a `kind != Heal` concentration spell — the
  engine-seat path offense/buff concentration never reached before. **Split by
  `fix/mfo-no-decline-fallback` (2026-09-07):** APMF ABSENT for the facet
  (`APMFBridge::OffenseCastClaimSupported()` false — ABI < 5 / `bApmfCast` off) falls through
  to the SAME direct-force stream below, unchanged; APMF present + capable + REFUSED now
  **FAILS CLOSED** (`SelfCast::Declined`, one rate-limited `[apmf]` error, no direct-force
  stream) — with APMF present there is no fallback.
  **COMBAT-AI GATE (v2.0.5, `fix/mfo-heal-recognition`, 2026-09-09):** the
  `ComposedCast::Try` call is now wrapped in
  `if (a_follower->GetActorRuntimeData().combatController)`. **Every APMF cast seat
  is a vfunc on the engine's COMBAT AI objects** — `CheckStartCast`/`CheckStopCast`/
  `GetMagicTarget` are `bool(CombatMagicCaster*, CombatController*)`, and the equip
  gate reads `CombatController::inventory` (APMF `core/CastSeats.cpp`,
  `core/EquipGate.cpp`) — so with no `CombatController` NOTHING can drive the cast;
  only `CheckCast` still runs OOC and that is a DENY gate. Field proof: after combat
  ended 14:19:14 the claim stood and heartbeated for 2+ minutes with ZERO APMF seat
  lines for that actor and no heal. This RESTORES marth's ruling recorded at
  `Logistics.cpp:1513-1522` ("OOC concentration delivery must always use the known
  working force"), which the claim path silently overrode once `bHealAnimPackage`
  went ON. **Not a decline-fallback:** `ApmfRefused` still FAILS CLOSED; out of
  combat APMF has no delivery to arbitrate for, so this is the APMF-ABSENT degrade
  contract evaluated per SITUATION. `combatController` and NOT `IsInCombat()`
  deliberately — it IS the object the seats hang off (same read as
  `CombatSense.h`'s `FoeCount`). **IN COMBAT NOTHING CHANGES.** Twins deliberately
  left alone and recorded instead: `CastSelfDirect`'s own `ComposedCast::Try`
  (`Actuation_Direct.cpp:886`) and this function's OOC offense-concentration
  `ClaimOffenseCast` (`:1216`) carry the identical exposure.
- `CastAuto` (PUBLIC, `Actuation_Direct.cpp:1390`) — AUTO target inference for `act.cast_target`,
  engaged ONLY when the board's default "Auto" pick is set (subject `Self`, no
  subject actor, no selector target). **Wired into BOTH paths:** combat `Fire`'s
  `kActCastTarget` branch AND `Logistics::ServiceFollower`'s OOC cast dispatch
  (`Logistics.cpp:~1138-1298`, inside `ServiceFollower`). On that OOC path a **non-AUTO** resolved `cast_target`
  now routes by nature: a **CONCENTRATION** spell (any non-self target) is
  intercepted FIRST → `CastTargetDirect` (direct force, package-lock-proof); the rest is
  FIRE-AND-FORGET, routed by `CasterConsent::ClassifySpell` + foe test — a **hostile
  FF spell aimed at a foe** takes `Packages::CastAt` (the animated alias-0 foe
  package); a **beneficial FF spell OR an ally/player target** is applied DIRECTLY to
  `tgt` via `CastSpellImmediate` (same immediate route as cast_player), so an "Ally
  HP<X% → Cast heal (instant)" gambit heals the wounded PLAYER (it used to send every
  cast_target through the foe package and never land).
  A NON-auto MANUAL pick (Target=Nearest-ally/named/
  Player) on the OOC path resolves through the shared **public**
  `Actuation::ResolveCastTarget` (`Actuation.cpp:1792`, moved out of the anon namespace
  — same ladder combat `Fire` uses) so it fires instead of being dropped (Wave 6
  #1); AUTO still routes to `CastAuto`. Classifies
  HOSTILE via `CasterConsent::ClassifySpell` — **delivery is NOT consulted** (MFO
  applies effects DIRECTLY to the target actor, bypassing the engine delivery
  system, so no spell is "self-only"): hostile → every foe in the combat group
  within `Confidence::ChaseRadius` (OOC: no group → NoOp); **beneficial → the WHOLE
  PARTY** (every active follower + the player within `g_sharedRadius` who NEEDS it —
  the caster included as one of N, so a self-delivery Candlelight lights everyone;
  **#2:** a health-restoring spell is filtered PER TARGET to the FIRING gambit's own
  health threshold, not HP<full — Fire plumbs the rule's `cond.*_hp_pct_below` param
  through `CastAuto`'s `a_healThreshold` (default 1.0 = anyone-below-full for
  `Always`/world-gated buffs and the Logistics caller); a `Choice` now carries
  `conditionOpcode`/`conditionParam` for this. The heal gate keys off the SPELL'S
  EFFECTS (`SpellHealsHealth`/`IsHealEffect`) **OR** `ClassifySpell==Heal`, so a
  restore-health spell the classifier does NOT tag Heal is still gated per target
  (field fix: a full-HP player was being healed because a different ally was hurt).
  The player runs the SAME `consider` gate as followers — full-HP members are never
  in the fan. **Per-target apply guard `ShouldApplyTo` (`Actuation_Direct.cpp:733`):**
  **#5:** a CONCENTRATION spell (Healing Hands, streams) returns true immediately —
  never blocked by the already-active/DoT gate; **#6a:** already-active detection is
  robust — HasMagicEffect(costliest) OR an active-effect-list scan for THIS spell
  with remaining duration (catches lights that never register HasMagicEffect);
  not-currently-affected → apply; beneficial/ally already-affected → skip (no
  re-stack); a hostile DURATION (DoT) spell is NOT blanket-skipped — it recasts
  when `burst >= dotRate * timeRemaining * fDotRecastBurstRatio` (`Config`), so a
  big-burst/small-tail spell re-lands as the tail decays while a pure DoT waits it
  out. Fans out via `ApplyEffectFromTo` (direct `CastSpellImmediate` + manual
  magicka deduct — NOT the single-holder foe package), **collecting FormIDs never
  raw `Actor*` past the combat-group read-lock (UAF)**, per-cast magicka with
  reserve floor (cost clamped to available, never negative magicka), one broadcast
  per `fCastCooldown` (`g_autoCast`, cleared in `ClearSelfCasts`). **#3/#6b:** a
  BENEFICIAL DURATION buff (light/ward/fortify) is additionally held off by a
  per-`(caster,spellID)` `g_beneficialRecast` window sized to the spell's authored
  duration × `fBeneficialRecastFrac` × a per-fire ±`fBeneficialRecastJitter` jitter
  (instant + concentration exempt) so a light that never registers as active is not
  respammed and the recast beat looks human. A manual target
  pick keeps the single-target `CastOn` path. `CastSelfDirect` now returns
  `SelfCast{Declined,Refreshed,Applied,Held}` so a pacing REFRESH doesn't count as an
  action that suppresses lower rules (logistics starvation, F3) — and `Held` (Fable
  SEV-2, 2026-09-06) says `ComposedCast::Try` held this spell off behind a DIFFERENT
  spell's live heal claim: transparent NoOp, never `Fired`, never a `HoldCastLock`
  re-stamp. Its
  `SelfCastReconcile` release has NO time cap — an earlier 30 s cap dispelled long
  self-buffs mid-duration (~30 s re-cast beat); release is stale/follower-gone only.
  **F3 tri-state maps the SAME in BOTH combat call sites** (`CastOn` self fork +
  `ConcentrationCast` self guard) as in logistics: `Applied → Fired`,
  `Refreshed → transparent NoOp` (a paced channel-refresh does NOT occupy the tick
  or suppress the rules below), `Declined → transparent`. The combat suppression
  window (`releaseSec`) is tuned so a party's round-robin gap can't outlast the
  self-buff and re-open the dispel/re-cast beat.
- **Summon spam guard (v1.1.1)** — `Actuation::CasterHasLiveSummon` (PUBLIC,
  `Actuation_Direct.cpp:811`). A conjured familiar/atronach (`SummonCreatureEffect`)
  or reanimated corpse (`ReanimateEffect`) is a COMMANDED ACTOR, not a caster-side
  magic effect, so every OOC already-active guard (each reads a magic effect on the
  cast TARGET) misses it and a `cast [summon]` gambit re-summons every cadence
  (marth, field). The helper scans the CASTER's active-effect list for a summon/
  reanimate effect THIS spell created (`ae->spell == spell`, `skyrim_cast` to
  `SummonCreatureEffect`/`ReanimateEffect`) whose `commandedActor` still resolves to
  a live (not dead/deleted) actor. PER-SPELL (distinct conjures + a Twin Souls pair
  track independently), keyed on the LIVE actor (a killed/expired/despawned summon
  recasts at once). Wired as a single guard at the TOP of the OOC cast block
  (`Logistics.cpp`, right after the `HasSpell` check, before target resolution) so
  it covers self/target/player/AUTO routes; returns false for every non-summon
  spell, so candlelight/buff/heal pacing is byte-identical. The SAME guard is wired
  into the COMBAT path (v1.1.1): `Actuation::Fire` (`Actuation.cpp:1832`, the sole
  caller is the Scheduler combat scan `Scheduler.cpp:612`) checks it once before the
  three cast opcodes (`kActCastSelf`/`kActCastPlayer`/`kActCastTarget`) and returns a
  TRANSPARENT NoOp ("summon still live") so the scan falls through to the next combat
  rule, exactly like the cast-grace hold. Non-summon combat casts are byte-identical
  (helper returns false) and the AI-first-grace pacing is untouched; the helper only
  READS the active-effect list, the same off-worker read as the other combat guards.

### Scheduler.cpp / Scheduler.h — the tick / combat scan
Round-robin one follower per 133 ms tick (`kTickInterval` `:33`), pumps packages
first, runs the combat table while the PARTY fights and the logistics table when
it does not, owns suppression + retreat/loot teardown. Runs on the AddTask worker.
- `Tick()` (`:252`) — caller `Diagnostics.cpp:931`. `Packages::Pump()` must stay
  first + unconditional. Reads `g_followers` — safe only because StopPump
  brackets the load window. Retreating follower `return`s before the gambit table
  so a cast rule can't fight the retreat travel.
- **PARTY COMBAT (2026-09-21, Deck/Fable — the Jesper freeze + the Cicero hold
  teardown; line numbers refreshed on `fix/mfo-field-batch-0921`).** `g_partyCombat`
  (`:199`) = the player `IsInCombat()` OR any managed active follower `IsInCombat()`
  OR — for followers whose own flag is TRUE this lap — his `CombatSense::FoeCount`
  (the `[sense] foes=` read — his own combat group, no radius) `> 0`. Computed ONCE
  per tick at `:285-345` on the worker from the same `g_active` walk the round-robin
  makes (#4: no lock, no off-road roster read), transition-logged
  `[sched] party combat ON|OFF (player= followers= foes=)` (#79). Reset in
  `ClearTransientState`. **CONTROLLER LIFETIME (item H of the field batch, tier-A
  carve-out; `ENGINE_NOTES.md` §0.47):** `FoeCount` derefs `combatController->
  combatGroup->lock`, and `Actor::combatController` is a raw pointer that
  `Actor::StopCombat` (vtable slot 0xE5, both runtimes) frees INLINE on BSJobs
  worker threads with no refcount and no lock — so the group read is gated on a
  same-lap `IsInCombat()` (`:330`), the pre-build shape. The `foes` term is then
  implied by `followersFighting`; it survives as the transition line's count.
  Un-gating it re-opens a per-follower, per-tick, in-AND-out-of-combat read of
  freed memory through every controller rebuild. **The gate is a strict REDUCTION,
  not a fix (Fable tier-A on `3315848`):** it removes the null and the
  allocated-but-inactive phases; a `StopCombat` on a worker thread between the flag
  read and the group read still frees the controller under the reader, and the
  Evaluator selectors, the `[sense]` line and `EquipRangeUndecidable` still read the
  group. A bare controller epoch cannot close it (validate-after-read still derefs
  `cc->combatGroup->lock` first). The sound next brief is the combat-thread MIRROR —
  `UpdateCombat` seat publishes `foeCount`/`inCombat` atomics per FormID, a
  `StopCombat` seat (slot 0xE5: SE `0x625920` / AE `0x6b70a0`) clears them, the
  worker reads only the mirror — `ENGINE_NOTES.md` §0.47 "Fable's judgement", STATUS
  "MFO-next: FoeCount/inCombat mirror". Until it lands, every new worker-side
  `combatController` read is a new instance of this exposure. **THE TWO GATES key on it, never
  on the serviced follower's own flag:** GATE 1 (`:434`) — the party-OOC branch
  runs only while party combat is FALSE, so the combat table runs for every managed
  follower while the party fights; a follower with no combat controller of his own
  has every foe-keyed rule fall through transparently (the Evaluator reads HIS
  group, `Evaluator.cpp:176`) and is logged once per fight `[sched] <id>: party
  combat, own combat=0 -- combat table live` (`:574`, latch `g_partyCombatNoted`,
  erased in the party-OOC branch). GATE 2 (`:451`, `MFO-B60` fixed, item G) — the
  WHOLE party-OOC teardown (RetreatClear, the ready-beat / proposal / per-fight
  latches, `CasterConsent::Clear`, `ClearCastLock`, `CombatStyle::Clear`,
  `ReleaseForcedWeapon`, the dwell erase) sits behind `++g_outOfCombatTicks[id] >= 2`
  — two consecutive PARTY-OOC services. Before item G only the hold release waited;
  a 145 ms party OFF/ON flap (21:57:34.884/35.029, a REAL engine combat-controller
  restart — the `[wstyle] OWNED` lines re-print) tore down a caster's consent latch
  and cast lock on the first tick. **Two `party combat OFF` edges in one fight are
  legitimate on such a restart**; the debounce is what makes them harmless.
  `Logistics::ServiceFollower` still runs on every party-OOC tick. Still keyed on
  the follower's OWN flag (`ownCombat`): `ReleaseTravelOnCombat` (unconditional
  eviction, and the own-OOC service below may legitimately arm a loot walk near a
  fight he is not in — the PLAYER's combat ends every excursion inside
  `ServiceFollower` anyway), the auto-retreat fill, and `Confidence::Of`.
  `NoteInCombat` stamps on EVERY party-combat service so the shed dwell cannot
  mature inside a party fight (the SHED INTERACTION ordering in the Equip
  force-hold entry rests on it).
  **THE PARTY-OOC TEARDOWN NOW STANDS DOWN INSTEAD OF DISARMING
  (`fix/mfo-spell-authority-0922`, 2026-09-22).** That branch's `ReleaseForcedWeapon(f)` is the
  ONE call site in the codebase that passes `a_standDown = true`: the fight is genuinely over (two
  consecutive party-OOC services), the hold is simply finished and no specific item has to leave the
  hand, so the lock is still force-cleared but the same weapon goes back on NON-FORCED and the
  follower sheathes — marth 2026-09-22, "no one ever sees an unarmed follower, they sheathe weapons.
  Not unequip." Full mechanism + the per-site audit of every other unequip in `Actuation`'s
  force-hold entry. **What breaks:** pass `true` from the dead/disabled teardown at `:387` and a
  resurrecting follower is re-armed with a weapon MFO no longer tracks.
  **Own-OOC inside party combat — the tick order:** combat table first (all of the
  in-combat branch), then `serviceOwnOoc(castFacetHeld)` (`:568`) = `Logistics::ServiceFollower`
  on a re-found record, called ONLY at the no-action exits — empty combat rules,
  the ready beat, and the scan ending without a Fired / opaque hold (which passes
  `castSeen`) — and never on a tick that acted or on the retreat-holder exit. So
  a party-combat/own-OOC follower still gets loot / economy / the equip declaration
  at the same ~1 s cadence as before, and the §4.3 one-real-action-per-tick bound
  holds because the second table runs only when the first produced nothing.
  **An own-OOC follower whose every condition is false prints NO skip-chain line**
  (the chain is empty); since item E the no-action exit prints `[eval] <name> (<id>)
  combat table: N rules, none matched` once per follower per fight (`:1213`, latch
  `g_noneMatchedNoted`) so a running table is distinguishable from one that never
  ran. STATUS's earlier "[eval] scan lines" promise for Jesper was wrong: nothing
  matched, so nothing printed.
  **`MFO-B55` FIXED (item F, `920a73f`):** `EquipRangeUndecidable` (`:116`) — a lap
  on which a target-relative equip rule (`cond.foe_within_range` / `_beyond_range`)
  was evaluated against a null / dead / non-hostile `currentCombatTarget` while
  `FoeCount(self) > 0` is UNKNOWN, not false: `equipCondKnownFalse` is demoted
  (`:1006`), the dwell clock refreshes, `ReconcileForcedWeapon` (`:1138`) keeps the
  hold. Both Cicero mid-fight weapon vanishes (21:57:34.226, 21:57:44.075) were the
  dwell timing out on an engine-side null; **the dwell path, not the party debounce,
  was the operative foe-keyed release.** A genuinely empty group (`FoeCount` 0) or a
  real party-OOC still releases. One `[sched] ... hold kept (truth unknown, not
  false; MFO-B55)` line per follower per fight.
  **What breaks:** keying EITHER gate back on the follower's own `IsInCombat()`
  re-creates the 2026-09-21 Jesper freeze (own flag false most of a dragon fight →
  stuck on the OOC table, only the logistics heal could act) and the Cicero hold
  teardown (three mid-fight `ReleaseForcedWeapon`s off a flapping flag); moving any
  teardown call back out of the `>= 2` block re-opens `MFO-B60`; calling
  `serviceOwnOoc()` on a Fired tick double-fires (a combat cast + a logistics heal
  in one tick); moving `NoteInCombat` back under `ownCombat` lets the shed drop a
  HELD weapon mid-party-fight; dropping the `g_active` walk from the top of `Tick`
  for a per-follower read re-introduces a per-follower "truth" that flaps; dropping
  the `EquipRangeUndecidable` demotion re-opens `MFO-B55` (weapons vanishing mid-fight
  on a LoS loss). Still open from the gate review: `MFO-B56` (`Loadout::Tick` erases
  `g_equipClock` on the OWN flag, so the AI-first grace collapses for an own-OOC
  hybrid cast) and `MFO-B57` (`[sense] foes=0` every 3 s for own-OOC followers; the
  ready beat is consumed at the party edge).
  **Since the merge with `fix/mfo-combat-restoration-direct` (2026-09-21):** the post-scan
  `serviceOwnOoc(castSeen)` call passes the scan's `castSeen` — a lap on which a combat
  cast rule's condition held does NOT run logistics, because the combat table's direct
  restoration stream (`RestorationCastDirect` / `CastSelfDirect`, paced = transparent NoOp)
  and Logistics' OOC cast dispatch would otherwise trade one follower's single
  `g_targetCast` slot every lap (`stream RELEASE (switch)` churn) and the OOC
  `DeclareAtExit` would read the left hand as free under the direct road's live LEFT cast
  lock. `castSeen` is the signal this file already bounds the cast lock by (`!castSeen` →
  `ClearCastLock`), so every lap that reaches logistics has the lock cleared. Calling
  `serviceOwnOoc(false)` from the post-scan exit re-opens both. Residuals recorded, not fixed:
  `MFO-B59` (the `ownedCast` explicit-subject ally + Offense road, the stream outliving a
  castSeen-false lap by the reconcile's stale window, a magicka-dry Declined stretch delaying
  logistics) and `MFO-B58` (the Task-1 concentration claims with no controller test).
- `ClearTransientState` (`:230`) — caller `Serialization.cpp:699`; must run inside
  the StopPump bracket. Save-scoped maps: `g_recent` (suppression), `g_lastServiced`
  (round-robin cursor), `g_retreatNotes`, `g_combatEnteredAt`, `g_proposedTarget`,
  plus `g_partyCombat` / `g_partyCombatNoted` / `g_noneMatchedNoted` /
  `g_equipRangeUndecidableNoted`.
- Casts `combatClassOverride` directly to `CombatStyle::Stance` (`:630,949`) — the
  ordinal-equality contract.
- **T#78 per-follower MFO master switch** — gate right after the `g_followers.find`
  in the per-follower service (`ServiceFollower`-caller path, `Scheduler.cpp`): if
  `!it->second.mfoEnabled`, SKIP the whole tick (combat + logistics) and `return`,
  so the follower stays vanilla. On the ON→OFF edge (`g_mfoDisabledSwept` latch,
  cleared in `ClearTransientState` + when re-enabled) run `Followers::ReleaseHeldState(id)`
  ONCE — same worker + helper as the dismissal sweep. `Logistics::ServiceFollower`
  carries a defence-in-depth `!a_state.mfoEnabled` early-out too. **Progression
  reads the switch through the `g_mx` mirror** (`Followers::IsMfoEnabled` ←
  `g_mfoOff`, republished by `PublishActiveMirror` at `Refresh` only — NOT at the
  Board write site `Board.cpp:1203`, so a poll inside the ≤532 ms diag-turn window
  can still see the old value once: **REVIEW-BACKLOG MFO-B12**, SEV-5, fix = one
  `PublishActiveMirror()` after that write — BUT `PublishActiveMirror` is file-local
  (Followers.cpp anonymous namespace `:43`/`:98`, no Followers.h decl), so the drain
  must first export it (Followers.h + Followers.cpp), verified 2026-09-14).

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
  (`Rapport.cpp:394`), Logistics 3D/merchant/activate (family-wide: `AcquireEquip`/`doDrop`/route-2b in
  `Logistics_Loot.cpp` + `Logistics.cpp`, merchant read in `Logistics_Economy.cpp`), Board/ProgProbe/ProgAllocator hotkeys+polls. Callers that must still
  run on VR check `IsInstalled()` and fall back to a direct call.
- `Clear()` (`:79`) — caller `Serialization.cpp:690`; drops pending work whose
  captured handles would re-resolve against the next session's reused handle table.

---

## 3. Combat hooks — `CasterConsent.*`, `Targeting.*`, `CombatStyle.*`, `Sightline.*`, `CombatSense.h`, `Confidence.h`, `Temperament.h`  ⚠️ HOOK/ABI RISK

Three engine vtable hooks, install-once at `plugin.cpp:293-295`, `.exchange(true)`-
guarded, VR-refused. Thunk bodies run on the **combat thread**; latch/consent
writers run on the main thread or job worker. Teardown order fixed at
`Serialization.cpp:716-718`.

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
- `Command` (`:162`) callers `Actuation.cpp:1222,1887,2033,2048`; `Current` (`:171`)
  `Scheduler.cpp:484`; `Clear` (`:177`) `Followers.cpp:280`, `Rapport.cpp:315`;
  `ClearAll` (`:183`) `Serialization.cpp:597`, `Probe.cpp:344,446`.
- Co-writes `currentCombatTarget`/`combatController->targetHandle` (`:101`) with
  SmartNPCTargetSelector.dll if present (`g_conflict` logged, not resolved).
- **Also drives CombatStyle:** `AnyActive()` (`:51`) + `ApplyTick(a_this, cc)`
  (`:62`). Renaming/moving those stops stance re-assertion. `InstallHook` installs
  if EITHER `g_commandTarget` OR `g_weaponStyleControl` is on (`:122`).

### CasterConsent.cpp — cast control (magic twin of Targeting)
Hooks `CombatMagicCaster::CheckStartCast` (advisory, 14 vtables, idx `0x06`,
`thunk` at `:495`, `InstallHook` at `:1010`) and `MagicCaster::CheckCast` (hard
gate, `VTABLE_ActorMagicCaster[0]` ONLY, idx `0x0A`, `CheckCastThunk` at `:874`,
`InstallCheckCastHook` at `:994` — [1]/[2] are base-subobject vtables, patching
them clobbers unrelated engine vtables).

- **THE LATCH'S CONTRACT — IT DRIVES, IT DOES NOT GATE (2026-09-23,
  `fix/mfo-cast-latch-gambit-set`).** `g_want` (`:73`, follower FormID -> spell) says
  what MFO is currently DRIVING: the cast stance (`Scheduler.cpp:1102` via
  `WantedSpell`), the equip-order gate (`CombatStyle.cpp:341`) and the force-YES of
  that one spell. It NO LONGER decides what the follower is ALLOWED to cast. Both
  LATCHED denies — `thunk`'s `!isWanted` branch (`:698`) and `ShouldDeny` (`:823`,
  behind the 0x0A hard gate) — now consult the gambit SET through `CtrlHasSpell`
  (`:359`, membership in `g_ctrl`, the same set `CtrlUnlatchedDeny` `:317` has always
  read on the unlatched path). marth's rule: "ONLY non-gambited spells get denied. Any
  spell allowed to be equipped is allowed to cast." Field cost of the old shape
  (2026-09-23): Lightning Bolt `0002DD29`, a configured gambit spell, was refused by
  MFO's own consent for the whole session because the latch held a different spell —
  equipped constantly, charged zero times. **THE PER-HAND RULES ARE UNTOUCHED AND MUST
  STAY SO ("there are two hands"):** the firing gambit's own per-hand lock,
  rank-carried preemption (the stored rule index, never tenure) and the concentration
  bound still run — `ConcUnboundedDeny` is checked AHEAD of the set test in both paths
  (inline at `:717` in `thunk`; already ahead of `ShouldDeny` in `CheckCastThunk`), so
  exact-mode bounding keeps its no-unbounded-channel invariant. **What breaks if you
  change this:** narrowing the set test back to the latched spell re-creates "a
  follower with two valid spells casts one and then stands there" (the same behaviour
  marth REJECTED for APMF on 2026-09-06); widening it past `g_ctrl` hands the AI its
  own spells back. `g_ctrl`'s single writer is `NoteGambits` (`:1190`), which publishes
  APMF's `SetSpellAllowList` from the SAME vector — the deny and the equip allow-list
  cannot disagree, and that is structural, not a promise.
- **NO CAST PACING (2026-09-23, same branch).** The v1.0.32 `permitAfter` dial on the
  latch entry, `NoteCooldown`, `g_lastPacedWindow` and the `if (cooling)` deny are
  DELETED (not disabled), and `Loadout::StartCooldown` no longer mirrors into this
  file. marth DECIDED cast pacing goes inert under the owned model
  ([[cast-cooldown-inert-is-correct]]); it was never inert in practice, because consent
  only stands down while a client claim LIVES, so every no-claim window paced an
  already-charged spell (field 2026-09-23: 3.7 s of a 5.3 s hold). `Loadout`'s own
  `g_coolUntil` re-EQUIP debounce is a DIFFERENT dial and it stays. Do not re-add a
  cast-time pace here: the engine's own cast timing is the floor.
- Signatures (mismatch corrupts every actor's cast gate): CheckStartCast
  `bool(*)(CombatMagicCaster*, CombatController*)` (`:495`); CheckCast
  `bool(*)(MagicCaster*, MagicItem*, bool, float*, CannotCastReason*, bool)` (`:874`).
  Both dispatch to per-vtable original via `g_orig`/`g_castOrig` keyed on the vtable
  pointer; unrecognized vtable returns benign.
- **§0.29 guard:** reads the actor via `a_cc->attackerHandle` (0x28) ONLY, never
  `cachedAttacker`; static_asserts pin `attackerHandle==0x28 && <0x68` (`:333,336`).
- `ClassifySpell` (PUBLIC, `:34`) → `Actuation.cpp:239`. `Want` (`:1137`) →
  `Actuation.cpp:788,1003,1080`. `WantedSpell` (`:1131`) → `Scheduler.cpp:1102` +
  `CombatStyle.cpp:341` (the equip gate's one exemption). `ClearTransientState`
  (`:1267`) → `Serialization.cpp:718`. (`NoteCooldown` is GONE — see "NO CAST
  PACING" above.)
- **Phase 2 APMF hand-off (2026-09-02, ALLOWANCE-TEMPLATE.md §7):** both exclusivity
  denies (`thunk`'s `!isWanted` branch `:651`, `CheckCastThunk`'s `exclusivityDeny`
  verdict `:948-949`) now check `APMFBridge::IsOwnedCastActive(fid)` first and STAND
  DOWN (return the AI's own answer) when true — APMF's own CheckCast/CheckShouldEquip
  T2 hooks (separate APMF.dll) already enforce the identical exclusivity via the ch.8
  claim for an owned-cast follower; running MFO's OWN deny too is redundant and
  fights it (two independently-configured deny paths, iCastControl slider vs. hard
  claim). `Want` (the consent GRANT / veto-removal) is UNCHANGED and still called
  unconditionally — that mechanism is MFO's alone, APMF only ever narrows a YES to a
  NO. Legacy (non-APMF / `bLegacyCastHybrid`) followers are unaffected:
  `IsOwnedCastActive` returns false for them, so both denies run exactly as before.
- **GRADUATED CAST SHELVED (2026-09-04, marth):** `CastExempt` (`:142`, the castLvl
  1-3 kind-filter) now starts with `if (kGraduatedShelved) return false;` (`:134`
  the flag, dormant switch left in place below it) — collapses every graduated call
  site to EXACT for any castLvl in 1-3 (only `CtrlUnlatchedDeny` `:282`,
  `ShouldDeny` `:770`, and `thunk`'s inline check `:632-650` route through
  `CastExempt`, so this ONE flag governs all three; `CheckCastThunk` `:874` is
  unaffected directly — it calls `ShouldDeny`/`CtrlUnlatchedDeny`, which now always
  says "deny" at 1-3, same as it always did at 4). Reason: porting the graduated
  exemption to APMF's T2c pre-computed allow-list can't match this hook's reactive
  per-cast gate without enumerating every castable spell (a staff's bound spell /
  scroll / mod-granted spell would fall off the list); revival is gated on a
  Synthesis-patcher spell classification pass. **UNTOUCHED by the shelve:** the
  owned/exact APMF stand-down (`IsOwnedCastActive` at `:661` and `:949` — the
  `!isWanted`/`exclusivityDeny` branches), `ConcUnboundedDeny` (`:218`, EXACT-only
  already — `lvl<4` fast-outs before ever reaching `CastExempt`, so it was never
  part of the graduated path), and the `CheckCast` (0x0A) hook install (`:994`,
  stays installed — still needed for `ConcUnboundedDeny`'s hard bound and the
  APMF-absent exact-cast degrade). `Config::g_castControl` (`Config.h:475`) keeps
  its stored 0-4 range; 1-3 now behaves identically to 4 (only 0/off is distinct).
  MCM slider labels ("ignore heals" etc.) are now stale-until-revival cosmetic
  text, not touched (not a functional gap — see `Docs/MFO-CONVERSION-ROADMAP.md`
  facet #2 in the APMF repo, not updated here per the shelving brief's
  do-not-touch-APMF-repo constraint).
- **`ClientCastClaimed(fid, magicItem)` — THE GENERALIZED CLIENT-CAST-CLAIM
  STANDDOWN (`:222`, anon ns; 2026-09-05, Task 2 audit of `Docs/CAST-ON-FRIENDLY.md`'s
  "let the AI cast it, don't drive it" architecture; RE-VERIFIED feat/mfo-cast-port
  Task 5, incl. the 0x06 install-order finding documented in this same comment
  block in the source).** Consolidates what used to be three hand-duplicated
  OR-expressions (the 2026-09-04 `CastBounds` HARD-ABORT fix,
  `Docs/SPEC-FORCED-CAST.md` §2/ENGINE_NOTES §0.40/INVARIANTS #76, then BROADENED
  2026-09-05 by the S1 field fix) into one helper, called FIRST — ahead of every
  other gate — at all three sites that must never hard-abort, deny, or force a
  claimed cast: `ConcUnboundedDeny` (`:244`, folded into its `Packages::StreamLive ||
  ClientCastClaimed` OR at `:271`), the `CheckStartCast` thunk's early-pass (`:579`),
  and `CheckCastThunk`'s early-pass (`:947`). Two independent proofs, either
  sufficient: (1) `CastBounds::Live(fid, spell)` — an MFO-side executor (today:
  `ComposedCast::Try`) armed exactly this (actor, spell) as a cast it already
  vetted; narrow, lock-free. (2) `APMFBridge::IsHealCastActive(fid)` — a live
  heal-cast claim exists for this actor at all; BROADER (per-actor, any
  spell) because APMF's own drive (or its guaranteed-delivery `CastSpellImmediate`
  fallback) is not guaranteed to land through the EXACT FormID `CastBounds` was
  armed for (the 2026-09-05 S1 field HARD-ABORT, "HARD-ABORTED cast of 0004D3F2
  (concentration unbounded)", proved (1) alone fragile for the APMF-driven path).
  (1) is always implied by (2) today (`ComposedCast::Try` only ever Arms `CastBounds`
  in the same call that succeeds a heal claim), so (2) alone already covers every
  observed case — (1) stays because `CastBounds` is the general reusable primitive
  (see its own entry below) and a future MFO-side executor could reintroduce a
  (1)-only case. **What breaks:** any executor that arms `CastBounds` for a spell it
  does NOT actually control (or forgets to `Disarm` on exit), or any caller of
  `ClaimHealCast` that leaves a stale claim, gets a standing bypass of EVERY consent
  gate in this file for that actor (heal-claim case: any spell, not just the armed
  one) until the TTL/expiry lapses — treat both as carrying the same weight the
  legacy `Packages::Begin` alias fill did. Deliberately does NOT fold in
  `IsOwnedCastActive` (offense's own, separate exclusivity standdown, checked
  independently at `:687`/`:975` — heal and offense stay two distinct claims, Task 3).
  `ConcProxy`'s plain `kInstant` `CastSpellImmediate` direct force skips the
  `MagicCaster` state machine entirely (ENGINE_NOTES §0.13: `RequestCastImpl →
  StartChargeImpl → StartReadyImpl → StartCastImpl → FinishCastImpl`), so it never
  reaches these hooked thunks and needs no claim/bound at all. Sole `CastBounds`
  writer today: `ComposedCast::Try` (see that entry, near `Packages.cpp`'s APMF
  section below) — by design, not a gap. **Audited 2026-09-05 (feat/mfo-claim-only-heal,
  Task 2): every DENIED/HARD-ABORT/veto path in this file was walked and confirmed to
  sit downstream of one of these two early-passes** — `ConcUnboundedDeny`,
  `CtrlUnlatchedDeny` (only reachable past the thunk's own early-pass),
  `ShouldDeny`/the exclusivity deny, the concentration force-block, the pacing deny,
  the force-YES path, and `CheckCastThunk`'s friendly-fire hold all sit AFTER the
  early-pass return in their respective functions, so a live claim silences all of
  them, never just the concentration bound.
- **`NoteGambits` ALSO PUBLISHES THE ch.8 ALLOW-LIST (`fix/mfo-spell-authority-0922`, 2026-09-22).**
  This function is the SINGLE writer of `g_ctrl`, so it is where
  `APMFBridge::PublishSpellAllowList(follower, spells)` is called — BEFORE it takes `g_mx` — and
  `Clear` calls `APMFBridge::ReleaseSpellAllowList` before taking `g_mx` for the same reason. The
  deny (`CtrlUnlatchedDeny`) and APMF's CANDIDATE REFUSAL are therefore built from ONE vector on ONE
  lap and cannot disagree about what a follower may use. Full design: APMFBridge's "ch.8 CAST-SELECT
  GATE" entry below. **The asymmetry that matters when reading either side:** `CtrlUnlatchedDeny` is
  NORMAL SPELLS ONLY (`Is(FormType::Spell)` + `GetSpellType() == kSpell`), so staves, scrolls,
  shouts, powers and abilities are deliberately NOT denied here — and that is exactly why the
  allow-list has to ENUMERATE them (APMF's side is a pure FormID-set test). **What breaks:** publish
  from anywhere else and the two sets drift; call either APMF function while holding `g_mx` and the
  combat thread's deny hooks queue behind an inventory walk.
- **Latch lifetime:** the Want latch does NOT clear on cast (spans the whole
  combat); `Want` overwrites SPELL only, never `permitAfter` or pacing breaks.
  Force-YES must NEVER apply to concentration spells (permanent-stream freeze,
  `:684`). Fast-out needs ALL THREE `g_wantCount==0 && g_styleSwapCount==0 &&
  g_ctrlCount==0` (`:516`). `WouldHitTeammate`'s `highActorHandles` walk is
  main-thread-gated (`:763,818`), fails open off-main (§0.30 crash).

### CombatStyle.cpp — weapon stance + equip gate (T#75)
Owns a follower's live per-combat `CombatController::combatStyle` (0x38); gates
`CombatInventoryItem::CheckShouldEquip` (idx `0x0F`, 30 template vtables, `:311`) so
the AI can't re-arm magic over a forced weapon.
- `enum Stance {None=0,Melee=1,Ranged=2,Cast=3}` (`CombatStyle.h:36`) — **ordinals
  are a serialized ABI**: equal to `State.h:82 combatClassOverride`, written raw
  (`Serialization.cpp:106`), read v4 (`:380`), cast directly by Scheduler
  (`:630,949`). Renumbering corrupts every saved override → **requires a
  serialization version bump.**
- `ApplyTick` (`:138`) is NOT a hook — called by Targeting's thunk; writes through
  the live controller, never dereferences the stored pointer (identity-compare only).
  Three write branches: A new-controller one-shot ownership (`:165-176`), B stance
  handoff one-shot (`:184-188`), C engine-re-derive re-assert (`:189-219`). Branch C
  is gated on `Config::g_cstyReassert` (default ON = byte-identical re-assert every
  tick, as before; OFF = the A/B proof/fix mode for combat-only positioning stutter,
  `Docs/SPEC-COMBATSTYLE-SOURCEGATE.md` — one-shot stance from branch A stands, no
  per-tick fight, relies on the equip gate below to hold the weapon). Flippable live
  via the Numpad-7 dev hotkey (`Config::g_cstyReassertKey`, `Board.cpp:671` in
  `InputSink`, mirrors the ProgProbe/ProgHarness hotkey pattern) — a bare
  atomic store, no `MainThread::Post` needed. Equip-gate static_asserts pin
  `combatStyle==0x38<0x68` (`:19,22`), `attackerHandle==0x28` (`:240,243`),
  `CombatInventoryItem::item==0x10` (`:250`). The 30-vtable list deliberately
  EXCLUDES weapon/potion/scroll/shout (`:376`) — widening it denies combat drinking
  (v1.0.32 lesson).
- `Want` (`:76`) → `Scheduler.cpp:378,380,612`; `Clear`/`ClearAll` →
  `Followers.cpp:285`, `Serialization.cpp:599`. Only a WEAPON stance can be an
  equip order (`:92`); class-override (#65) + magicka-dry fallback deliberately do
  NOT engage the gate (must not mute the AI's own magic). Reads
  `Forms::g_meleeStyle/rangedStyle/castStyle` (`:53`) — missing form → stance
  silently disabled.
- **Phase 2 APMF hand-off (2026-09-02, ALLOWANCE-TEMPLATE.md §7):** the equip-gate
  thunk checks `APMFBridge::IsOwnedCastActive(fid)` right after resolving `fid` and
  stands down (returns the AI's own answer) unconditionally for that follower — APMF's
  own T2a `CheckShouldEquip` hook (separate APMF.dll, mutex-free RCU read) already
  denies any spell/staff that isn't the ch.8-claimed one, covering exactly what this
  gate's `WantedSpell` exemption protects (same source spell — `ClaimCasting` passes
  the identical FormID `Want` does). This is a full stand-down, not just the
  exemption: it also means MFO's own weapon-equip-order deny does not apply to this
  follower's magic items while APMF owns its cast — safe because APMF's hook denies
  every non-claimed spell/staff regardless of weapon-order state, so the forced weapon
  stays equally protected. Legacy followers (APMF absent / `bLegacyCastHybrid`) are
  unaffected — `IsOwnedCastActive` returns false and the gate runs exactly as before.
- **Phase 2 APMF hand-off #2 (2026-09-03, ch.15 `kIntent_Equipment`):** the equip-gate
  thunk ALSO checks `APMFBridge::IsEquipmentClaimActive(fid)` (right after the
  `IsOwnedCastActive` check, independent of it — either/both/neither may hold for a
  follower on a given tick) and stands down for the follower's OWN weapon-order deny
  when true. The claim is engaged/refreshed by `Actuation::ReconcileForcedWeapon`
  every tick the force-hold (`g_forcedWeapon`) survives, and released by
  `Actuation::ReleaseForcedWeapon` (the single choke point every teardown path
  funnels through) the instant it releases — so the claim is alive for exactly as
  long as the native force-hold is. APMF's own T2a `CheckShouldEquip` hook enforces
  the SAME deny + the SAME `kIntent_SelectSpell` off-hand-loan exemption while the
  claim is live (APMF_API.h's `kIntent_Equipment` comment). `IsEquipmentClaimActive`
  is false whenever APMF is absent/off or no claim was made — the APMF-absent path
  is byte-identical native enforcement.

### Sightline.cpp — line-of-sight split across threads (NOT a hook)
Raycast runs only on the main thread, results cached, worker reads the cache.
- **Two-stage Measure (cast LoS).** `CustomRayConfirmsOcclusion` (`:52`, MAIN
  THREAD ONLY) is MFO's own `bhkWorld::PickObject` point-raycast, fired by
  `Measure` ONLY when the engine `HasLineOfSight` already said CLEAR — so the
  extra pick runs on the ambiguous cases only. Layer = `COL_LAYER::kCharController`
  ("could a walking body travel this line": catches camp-tent/cloth/anim-static
  the engine LoS sees through, while a THIN point-ray passes over railings and
  through open doorways/gates — no over-block). Caster self-excluded via its own
  char-controller **system group** in `filterInfo`; target self-excluded by ending
  each ray `kTargetMargin=48u` short of the body point (kCharController rays stop
  on any actor capsule). Samples feet/torso/head (`h*0.55`/`h*0.90` off
  `GetPosition`, `GetHeight` fallback 120) — ANY clear sample => VISIBLE (fixes
  height/stairs). Can only flip VISIBLE→OCCLUDED; **fail-open** (no cell / no
  bhkWorld / no controller / VR → clear, engine verdict stands). Held under
  `world->worldLock` (`BSReadLockGuard`); `g_mx` taken only AFTER, so the leaf
  discipline is intact. **Cost bound:** every caller reaches this only through
  `Want`'s `kRepostSeconds=0.3` per-viewer repost throttle → ≤1 pick-batch per
  ~0.3s per caster for BOTH discrete and concentration casts (Measure is
  file-local; nobody calls it directly), so concentration re-check is NOT a
  per-tick ray. **No hardcoded engine offsets added** (all via CommonLib types),
  no co-save/ESP/version touch.
- `Measure` (`:~135`, MAIN THREAD ONLY) calls `HasLineOfSight` (RELOCATION_ID
  53029/53829) inside `MainThread::Post` (§0.30 crash class off-worker). `Check`
  (decl `Sightline.h:52`, worker-safe cache read) → `Actuation.cpp:91,113`,
  `Actuation_Direct.cpp:1240,1656`, `Evaluator.cpp:320`.
  `Want` (decl `Sightline.h:58`) → `Evaluator.cpp:330`, `Actuation_Direct.cpp:1594` (F7 auto-cast),
  `Logistics.cpp:1801` (OOC hostile cast — seeds the `Check` at `:1805`; added to
  close the 2026-08-18 review SEV-3 "Check without a Want → Unknown always passes"
  inert wall-gate). `g_mx` is a strict LEAF (nothing called while held). Fail-open
  by design (cold/stale/VR → Unknown). **Every `Check` must have a `Want` seeding
  its pair** or the gate is inert; the seeders are Evaluator (combat foes),
  Actuation_Direct F7 (auto-cast fan), and Logistics (OOC hostile).
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

### Logistics family: Logistics.cpp / _Cast / _Economy / _Loot / _Loot_Equipment / _internal.h / Logistics.h
**MODULE SPLIT (mechanical, v1.1 split pass; +1 module 2026-09-06 — the
2500-line hard rule crossed again after the route-2b generalization pass, see
below).** One TU became five + a shared internal header. Cross-module
state/types/small helpers live as `inline` members of `namespace
MFO::Logistics` in `Logistics_internal.h` (ONE instance across the TUs — it
replaces the old single anonymous namespace; big cross-module helpers are
declared there and defined in their home module). Layout:
- `Logistics.cpp` (2192) — core tick: `ServiceFollower` (`:722`, INCLUDING the
  OOC cast dispatch `:~1128-1368` — concentration direct-force `:~1258`,
  fire-and-forget `:~1348`), drink (`DrinkBest`), `EquipTorch`/`HealExcludedWeapon`/
  `ShedOffRoleWeapon`, sinks, lifecycle + MSTK API, evaluator pure reads.
  **PLAYER-COMBAT LOOT INTERRUPT (2026-09-06):** the GLOBAL travel-intent
  backstop (`:664-728`, runs on EVERY out-of-combat service call, ANY
  follower, ~133 ms-scale — not gated on the 1 s logistics cadence) now also
  clears every active loot-travel slot when `RE::PlayerCharacter::IsInCombat()`
  is true (`playerInCombat:~700`), same mechanism as the existing cap/
  subsystem-off exits (`Packages::LootTravelClear`, reason `"player combat"`,
  no `MarkTravelFailed` — the corpse isn't penalized). Distinct from THIS
  follower's own combat yield (`ReleaseTravelOnCombat`, Scheduler's in-combat
  branch, unchanged) — a traveller can sit a room away from a fight for
  several ticks before his own `IsInCombat()` flips, which is the gap this
  closes.
- `Logistics_Cast.cpp` (269) — mage-identity/school classifiers:
  `TargetMagicSchool:24`, `HasCastGambit:64`, `IsCasterFollower:101`,
  `TopTwoSchoolMask`, `LearnCarriedTomes:137`, school name/keyword helpers.
- `Logistics_Economy.cpp` (1188) — #21 economy: mage-apparel scoring,
  `UnlockCollegeTomes:226`, `EquipBestOwnedGear:297`, `BuildBuyThresholds:425`,
  `EconomyProbe:543`, public buy helpers (`MageApparelBuyKey:1148` et al).
- **WEAPON / ARMOR STYLE BY PERKS (2026-09-13, marth: "highest skill wins, most
  perked style within a category wins, strongly prefers").** THE decision is
  `ComputeWeaponRoles` (`Logistics_Loot.cpp:442`): the melee CLASS is still the
  higher skill (`two > one`, tie → 1H, unchanged); inside that class the kind(s)
  with the MOST owned perk ranks conditioned on them (`Progression::StyleVotes`
  via `StyleVotesFor:405`) become `WeaponRoles::preferKinds` (0 = no votes or all
  kinds level = NO preference); `WeaponRoles::offHand` (1 shield / 2 dual wield)
  is DETECTED + LOGGED ONLY (`[style]` line, once per follower per change).
  `WeaponScore` (`Logistics_internal.h:310`) = attack damage × `kStyleBias` (1.5,
  `:276`) for a preferred kind (`WeaponKindOf:283`, by the record's own WeapType
  keyword, WEAPON_TYPE fallback; battleaxe vs warhammer is keyword-only) — a
  BIAS, never a filter. CONSUMERS (all previously raw `GetAttackDamage`
  compares on the in-role melee weapon): `BuildEquipmentContext:26`
  (`EquipmentContext::baseScore` + `roles`), `LooseEquipmentQualifies:175`,
  `LootEquipment:222` (`bestWeapScore`), the economy keep buckets 1H/2H
  (`Logistics_Economy.cpp:613` `keepRoles`), `BuildBuyThresholds:391` →
  `TradeBridge::BuyThresholds::meleeBaseScore`/`preferKinds` → `PlanBuy`
  (`TradeBridge.cpp:234` via public `Logistics::WeaponBuyScore:993`).
  Armor: see the ARMOR CLASS BY SKILL + PERKS entry below (2026-09-14) —
  `ArmorClassSuits` no longer gates anything; `ArmorScore` is the judge.
  **THREADING — the perk-style MIRROR:** `TallyStyleVotes` is main-thread-only, so
  `StyleVotesFor` reads a per-follower copy under `g_styleMx`
  (`Logistics_internal.h:338`, `g_styleMirror`, `kStyleRefresh` 10 s) and posts
  the re-tally through `MainThread::Post` (FormID captured, actor re-resolved);
  the first read of a follower returns NO votes (default behaviour) until the
  posted tally lands next frame; VR (no pump) never refreshes → default. Cleared on
  revert by `Logistics::ClearStyleMirror` (`Logistics_Loot.cpp`, called from
  `Serialization::ResetAllState` after `MainThread::Clear` — Fable F4: a latch left
  `inFlight` after Clear dropped its closure froze that follower's votes for the
  process lifetime).
  **DEFAULT-CASE PROOF:** with `preferKinds == 0` `WeaponScore` is the uint16
  damage widened to float, so every `>` / `>=` compare orders identically; with
  with the catalog unbuilt every
  vote is 0. **KNOWN GAPS (not consumers yet):** the COMBAT equip
  `Actuation.cpp EquipWeapon` (`:1603`) still picks raw max damage across BOTH
  melee classes (it needs `WeaponScore` — Actuation was outside the boundary);
  bow vs crossbow stays the ammo/damage rule (no perk record distinguishes
  them — both carry `WeapTypeBow`); dual wield needs a CSTY that allows it
  (`MFO_MeleeStyle` DATA=1 is dueling-only) + a left-hand equip — `offHand`
  steers nothing until that mechanism exists. **What breaks:** changing
  no armor tie `ArmorClassSuits` is untouched; with the catalog unbuilt every
  vote is 0. **CONSUMERS ADDED 2026-09-13 (`feat/mfo-dualwield-combat-pick`):** the
  COMBAT equip `Actuation.cpp EquipWeapon` (`:1729`) now picks by `WeaponScore`
  too (via `Logistics_internal.h`, included from `Actuation.cpp`), and `offHand`
  STEERS: 2 → a second one-hander force-held in the left, 1 → best shield, under
  `MFO_MeleeStyle` DATA = `1|4` (`kAllowDualWielding`) — see §2 Actuation "COMBAT
  PICK + DUAL WIELD BY PERKS". **STILL A GAP:** bow vs crossbow stays the
  ammo/damage rule (no perk record distinguishes them — both carry `WeapTypeBow`).
  **CLOSED 2026-09-14 (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`) — THE
  SECOND ONE-HANDER, all three paths at once, ONE rule:** `wantOffHand` =
  `roles.offHand == 2 && meleeTargetClass == OneHand`; `offHandBaseScore` = the
  SECOND-best owned in-class one-hander's `WeaponScore` (0 with fewer than two
  owned; a stack of >= 2 of one form counts twice — it covers both hands).
  KEEP (`Logistics_Economy.cpp` `keepSecond1H`, the weapon keep buckets): bucket
  1 also keeps its runner-up form UNLESS the best form is a stack >= 2 (then
  `PickOffHandWeapon` takes the second copy and the runner-up is junk). BUY
  (`BuildBuyThresholds` → `TradeBridge::BuyThresholds` APPENDED `wantOffHand` /
  `offHandBaseScore` after `armorBaseScore` → `PlanBuy` "SECOND ONE-HANDER"
  block): one more in-class 1H above `offHandBaseScore`, or above
  `meleeBaseScore` when a primary was JUST bought (the old best drops to the
  off-hand); a stock line the primary took the last copy of is skipped; the
  `[econ] bought plan` names it `(second one-hander, dual wield)`. LOOT
  (`EquipmentContext::wantOffHand` / `offHandBaseScore`, `BuildEquipmentContext`;
  `LootEquipment` `bestOffHand`, priority after bestRanged, before bestBackup;
  `LooseEquipmentQualifies` the mirrored rule): a 1H above `offHandBaseScore` is
  taken and STOCKED (`AcquireEquip` `a_forceStock`) — never into the right hand;
  Actuation's `PickOffHandWeapon` pairs it at the next combat equip, unchanged.
  **ALIASING (Fable round-2 SEV-3 on `5f814d5`, fixed):** `bestOffHandScore <=
  bestWeapScore` throughout the scan, so the max-scored one-hander passes BOTH
  gates and `bestOffHand` ALIASES `bestWeap` whenever a primary upgrade exists —
  `forceStock` is `best == bestBackup || (best == bestOffHand && best != bestWeap)`
  (`trueSecond`), so a primary upgrade is equipped in place with its gems carried and
  only a TRUE second is stocked (and, being stocked, captures no gems). `usesShield`
  (buy `BuildBuyThresholds` + keep `EconomyProbe`) and `shieldUseless` (both loot
  sites) gained `roles.offHand != 2` — a dual wielder's left hand is a weapon.
  `[equip] LOOT-*` prints `offHand= offHandBase=` and tags the second-1H pick.
  **What breaks:** the three `offHandBaseScore` computations (`BuildEquipmentContext`,
  `BuildBuyThresholds`) and the keep runner-up must stay the SAME top-2 rule or
  loot fetches what keep sells; `PlanBuy`'s `plan[c.idx] >= c.avail` skip is what
  stops the off-hand buy from over-buying a single stock line; the LOOT take must stay
  `a_forceStock` (it replaces nothing → no MEO gem capture, the SEV-2 above). **OPEN
  BACKLOG: `Docs/REVIEW-BACKLOG.md` MFO-B24** (SEV-5) — keep gates on
  `keepRoles.melee`, loot/buy on `meleeTargetClass` (a base caster keeps two daggers,
  fetches none); "ONE rule" holds for weapon-role followers; **MFO-B28** (SEV-5,
  pre-existing) — a BOUGHT primary upgrade never passes `AcquireEquip`, so it carries
  no gems. **What breaks:** the `trueSecond` test must stay `best != bestWeap` — a bare
  `best == bestOffHand` stocks every primary upgrade. Changing
  `kStyleBias` re-orders every loot/keep/buy compare at once (they share ONE
  score by design — never bias one site alone or loot and keep disagree and a
  looted greatsword sells); `preferKinds` must stay `Progression::WeaponKind`
  bits (shared with the buy thresholds); reading `TallyStyleVotes` directly from
  the worker instead of `StyleVotesFor` races the allocator.
- **ARMOR CLASS BY SKILL + PERKS (2026-09-14, field fix; branch
  `fix/mfo-armor-class-score`).** FIELD (Deck log on `69c5b3c`, Fable): after a
  respec Adelinda (Heavy 15 / Light 69) and Cicero (Heavy 20 / Light 77) stayed
  in heavy and Cicero BOUGHT the heavy Falkreath Helmet (18) over light 13s.
  ROOT CAUSE: MFO never un-wore anything and the ENGINE's auto-equip (highest
  rating per slot, skill-blind) was the wearer; every MFO rated-armor compare was
  rating-vs-worn-rating with no class term, `ArmorClassSuits` was the only skill
  read and a loot-side FILTER (`Docs/ENGINE_NOTES.md` §0.46). THE JUDGE:
  `ArmorScore` (`Logistics_internal.h:397` pref-taking inline; `Logistics_Loot.cpp:235`
  Actor* overload) = rating × `kArmorClassBias` (2.0, `Logistics_internal.h:362`,
  on the class of the higher BASE armor skill — `ArmorPrefFor:213`,
  `GetBaseActorValue`, the `DominantArmorSkill` read; exact tie → the armor
  perk votes `StyleVotes::armor[0]` vs `[1]`; still tied → light) ×
  `kArmorPerkBias` (1.25, `:369`, on the class the perk votes LEAD, whichever
  class that is; votes via `StyleVotesFor`, the `g_styleMx` mirror). Arithmetic:
  leather 26×2=52 > steel 31, hide 20×2=40 > iron 25, glass 38×2=76 > ebony 43;
  a BIAS never a filter — hide (40) still takes a found ebony plate (43), a bare
  slot takes off-class over nothing, clothing (rating 0) scores 0, creature
  armor is filtered upstream. The arithmetic itself is `TradeBridge::ArmorScoreOf`
  (`TradeBridge.h:115`) so the VM-side buy planner ranks identically.
  `ArmorPref` (`Logistics_internal.h:380`) carries the two multipliers + the
  inputs; scans compute it ONCE (`EquipmentContext::armorPref`,
  `BuildEquipmentContext` `Logistics_Loot_Equipment.cpp:45`; `keepPref`
  `Logistics_Economy.cpp:735`; `armorPref` in `EquipBestOwnedGear` and
  `BuildBuyThresholds`). **CONSUMERS — ALL of them, by design ONE score:**
  `ArmorIsBetter` (`Logistics_Loot.cpp:321`, candidate score vs worn score per
  slot; the `ArmorClassSuits` early-out is DELETED — `ArmorClassSuits:303` is kept
  as the named category predicate, rebased on `ArmorPrefFor`, and has NO callers),
  `CarriesSlotArmorAtLeast:371` (score, or a worn heavy 31 in the pack blocks the
  light 26), `LootEquipment:224` best-first (`bestArmorScore`) +
  `LooseEquipmentQualifies:177`, **`EquipBestOwnedGear` rated branch
  (`Logistics_Economy.cpp:297`) — THE WEAR DECISION:** highest-scored owned piece
  that beats the worn score → the existing `AcquireEquip` `MainThread::Post` path
  (#62; equipping auto-unequips the displaced piece), the keep buckets
  (`primary = ArmorScore`, key 10+slot), the redundant-inferior force-sell
  (`:926`; slot-best by score so the worn OFF-CLASS piece is what sells — the
  trade's `RemoveItem` is the proven un-wear; worn-is-kept (a) yields to it via
  `forceSell`; **and since `fix/mfo-spell-authority-0922` a THIRD `forceSell`
  reason: `deniedWorn` — a WORN item whose APMF equip CATEGORY our OWN declaration
  DENIES** (`APMFBridge::EquipAuthorityDenies(fid, kEquipCat_Shield)`, the mirror of
  `EquipAuthorityOwns` reading the `denied` half of the scope last SENT). **THE BUG
  IT FIXES (Cicero's shield, field 2026-09-22):** the gate order is `count<=0 → stock
  → keepWeap → playerPick → keepArmor → forceSell | worn → gemHold → excluded →
  SELL`; `worn` is tested BEFORE `gemHold`, so **`gemHold` was never the blocker and
  `worn` always was**. For a denied category that read is permanently wrong: APMF's
  seat refuses every re-equip (`RefreshEquipDeclaration` rule 3 denies Shield when
  `roles.offHand == 2` under `bWeaponStyleControl`), APMF ch.17 never UNEQUIPS an
  already-worn item, and nothing in MFO takes it off — so it is worn for the rest of
  the save and never offered. **Deliberately narrow:** only SHIELD is mapped (the
  only category MFO ever puts in `denied`), an unmapped category is left alone rather
  than guessed at, and `gemHold`/`Catalog::IsExcluded` still run after it so a
  gem-socketed or artifact shield is still protected. Log:
  `[sell] <id> '<name>' -> force-sell (worn, category DENIED by our own declaration)`.
  **A PLAYER-GIFTED SHIELD IS DELIBERATELY SELLABLE — DECIDED BY MARTH, 2026-09-22: "Let them sell."**
  Raised as an open question before the line shipped; this is the answer, so do NOT "fix" it later.
  `IsPlayerPick` **cannot** protect a shield: the pass that RECORDS player picks (rule 4b, `:~885`)
  skips shields outright (`if (!ar || ar->IsShield() || ar == pick) continue;`), so no shield is ever in
  `g_playerPicks`. A shield the player handed a follower who has since taken up dual wielding therefore
  reaches this line with no player-intent protection and sells — intended, because the declaration
  denies the category, nothing can equip it again, and a permanently unusable item on his arm is worse
  than the gold. Still protecting a shield, so this is bounded rather than blanket: `IsStockGear`
  (T#69), `gemHold`, and `Catalog::IsExcluded`.
  **What breaks:** widen `denied` without widening the mapping and the new category
  silently keeps its old worn-is-kept behaviour; map a category MFO owns rather than
  denies and the sell path starts selling gear the follower is wearing on purpose), **OPEN BACKLOG — read
  before editing `RefreshEquipDeclaration`:** `MFO-B63` (the declaration is pick+worn rather than
  best-per-slot, it is sent into a 3D-absent actor with no re-send, and SEND-ONLY-ON-CHANGE then
  swallows the correction — a follower stood bare for 2 min 36 s in the field. DEFERRED, SEV-2), `BuildBuyThresholds:425` → `TradeBridge::BuyThresholds`
  **APPENDED** `armorHeavyBias` / `armorLightBias` / `armorBaseScore[5]`
  (`TradeBridge.h:99-107`; `armorBaseRat` keeps the raw rating as a diagnostic — a
  float score truncated into the int32 would tie its own baseline and re-buy every
  visit) → `PlanBuy` armor (`TradeBridge.cpp:266`, ranks by `ArmorScoreOf` against
  `armorBaseScore`). `MFO_Trade.psc` + the 10 natives untouched. **LOG LINES:**
  `[armor] <id> '<name>': heavy=<base> light=<base> -> class LIGHT|HEAVY votes h/l=
  bias h/l= | worn body '..' [Heavy] rat= score=, head .., hands .., feet .., shield ..`
  (`LogArmorClassIfChanged:248`, once per follower per {class, worn-set} change,
  deduped through `StyleMirror::loggedArmor`, called from `EquipBestOwnedGear`'s
  rated branch — so a mage-apparel follower never emits it); `[equip] <id>: OWNED
  armor '..' [Light] rat= score= <- worn '..' [Heavy] rat= score= | class ..`
  (`EquipBestOwnedGear`); `[equip] <id>: LOOT armor/apparel '..' type= rat= score=`
  (`LootEquipment`); `[econ] .. | offered [..] | bought plan [..]` (`ReportTrade`,
  the offered sell rows + `PlanBuy`'s named picks, `TradeOrder::buyPlan`);
  `[style]` now prints `armor h/l/s=`; passive `[armor-obs] <id> '<name>': EQUIP|
  UNEQUIP '<item>' (<fid>) [Heavy|Light] rat=` in `BeastHeadSink::ProcessEvent`
  (`Logistics.cpp:156`, BEFORE the beast-head gates, rated ARMO only, follower
  membership via `Followers::IsTrackedFast` — the locked mirror, #4/#74 — zero new
  hooks, no rate limit). **DEFAULT-CASE PROOF:** a follower whose skills and
  inventory are all one class gets a uniform ×2 (×2.5 with perks) on every compare,
  so every `>` / `>=` orders as before; no votes → perkBias 1.0. **What breaks:**
  changing `kArmorClassBias`/`kArmorPerkBias` re-orders loot, owned-equip, keep,
  sell AND buy at once — they share ONE score, never bias one site alone or a
  looted light piece is force-sold as a redundant inferior / a bought piece is
  never worn; `ArmorPrefFor` must stay on `GetBaseActorValue` (an actual-value
  read would let a Fortify enchant on the very candidate move the category) and
  on `StyleVotesFor` (never `TallyStyleVotes` off-main); `EquipBestOwnedGear` is
  now the ONLY place MFO actively swaps a worn rated piece — gate it or rate-limit
  it and the follower stays in the engine's pick; `ArmorScoreOf` in `TradeBridge.h`
  and the inline `ArmorScore` must stay the same arithmetic or buy and loot
  disagree; `BuyThresholds` is append-only (do not move the new fields ahead of
  `eligibleSchools`); `ArmorClassSuits` is dead code by design — do not re-add it
  as a gate.
- **THE DECLARED WORN SET — `RefreshEquipDeclaration` (`Logistics_Economy.cpp`, beside
  `EquipBestOwnedGear`; `feat/mfo-equip-authority` 2026-09-15, PORT #1; SCOPED by
  `feat/mfo-equip-authority-v9` 2026-09-16).** When
  `APMFBridge::EquipAuthoritySupported()` (APMF present, ABI ≥ 9, `bApmfEquipAuthority`) and
  `FollowerState::mfoEnabled`, MFO no longer equips ARMOR (nor a HELD hand) directly: it
  DECLARES the worn set to APMF's ch.17 channel as `APMF_EquipEntry {form, hand}` (v8
  `SetEquipSetEx`) under a SCOPE (v9 `SetEquipScope`, sent FIRST in the same call), and APMF
  equips every declared item the follower is not wearing — in the hand the entry names, only
  when every category it competes for is owned — and refuses every other engine equip in an
  OWNED or DENIED category; an equip in a category the scope neither owns nor denies passes
  (`owned=0 verdict=allow`). **THE SCOPE (v9 — MFO DECLARES WHAT IT OWNS AND ACTS DIRECTLY IN
  WHAT IT DOES NOT):** `owned = Armor | Right (a right hold) | Left (a left hold) |
  Right+Left+Ammo (a held bow/crossbow) | Right+Left (a held two-hander)`; `denied = Shield` iff
  `roles.offHand==2 && bWeaponStyleControl`. Light never owned, Shield never owned, Armor always
  owned; a hand is owned ONLY while a `ForcedHold` stands in it (`a_holdRight`/`a_holdLeft`;
  EVERY road passes the ledger — the OOC service road through `Actuation::ForcedHoldFor`, Fable
  F1 on `7857446` — so the scope follows the ledger and an empty ledger → `owned=Armor`). THE
  HOLD WINS OVER RANGE: a dual-wield hold
  under an enemy at range is not released for it (F1/F6 precedent; the exits are the gambit's
  own condition, combat end, a spell taking the left). HANDS: declared from the ledger ONLY — a
  one-hand hold carries `kEquipSlot_Right`/`kEquipSlot_Left` (the same form may stand once per
  hand); a held two-hander/bow is `kEquipSlot_Default` (it competes for both hands from its
  TYPE); ammo and armor are Default. A shield ARMO is NOT Shield-only at APMF's seat: a mixed
  piece with a shield bit AND a body bit (a modded "shield on back") competes Armor+Shield+Left,
  so the Armor-owned scope holds the body slot against it — rule 5 excludes every `IsShield()`
  piece from the set, which is exactly why such a piece is refused, never declared. The v8 "else the weapon currently in that hand" and torch
  reads are DELETED: they were the F6 freeze (117 `CombatNode` would-denies against no hold).
  `a_leftReserved` (a cast on the left) → nothing left, owned or declared. RULE 1b,
  BOUND WEAPONS (round 4): `LiveBoundWeapons(actor)` = the WEAP `associatedForm` of every live
  (not dispelled/inactive) ActiveEffect whose base MGEF `data.archetype == kBoundWeapon`
  (`RE/E/EffectSetting.h:71` / `:88`; the `CasterHasLiveSummon` road, worker, read-only);
  each is declared while live with the hand it is actually held in, else Right for a one-hander
  unless `a_holdLeft` names it, Default for a bow/two-hander, and REPLACES the ledger's entry for
  that hand (`EquipDecl::DropHand`) — v9: ONLY INTO AN OWNED HAND (a one-hander needs its hand
  owned, a bound bow/two-hander both; an unowned-hand `BoundItem` equip passes the seat
  undeclared); logged `declare bound '<name>' (<hand>)` when new since the
  last send. `ResendEquipDeclaration(id)` drops the change detector only (MFO-B41's re-send;
  `ForgetEquipDeclaration` also drops the player picks and is the dismiss road). THE COMPOSITION (declare→enforce, principle 4 — only
  what MFO decides elsewhere): hands = `a_holdRight/a_holdLeft` (Actuation's `ForcedHold`
  ledger) and nothing else (a two-hander/bow in the right empties the left; never a weapon not
  held — a stowed bow beside a held sword is not simultaneously wearable); AMMO only under a
  bow/crossbow HOLD (worn of the matching kind, else best carried by damage; bolts for a
  crossbow) — the archer AI's own ammo equips pass in the unowned category; the SHIELD is NOT
  DECLARED (v9): `roles.offHand==2` → the Shield category is DENIED (THE FIELD FIX, now without
  owning a hand); 1 → Actuation's direct `EquipShieldOnMain` (an unowned-category equip the seat
  allows); 0 → the engine's own; the JUDGED ARMOR PICK = `ComputeOwnedGearPick` (EquipBestOwnedGear's own pick, factored
  out VERBATIM so there is one judge; one pick per declaration, it replaces every worn ARMO its
  slot mask overlaps); everything else WORN (jewelry, clothing, circlets, modded slots) stays
  declared. **PLAYER AGENCY (round 3, v8 — APMF's seat allows `path=PlayerMenu` by default and
  MFO does NOT set `kEquipAuth_DenyPlayerMenu`):** rule 4b — a worn non-shield ARMO that the last
  declaration did not carry and that is not this tick's pick was put there by the player (in
  observe mode also by the engine; same treatment): recorded in `g_playerPicks` (per follower,
  pruned to what he owns; cleared with the declarations), logged once `[equip-auth] <id>: player
  put on '<name>' -- kept`, and it STANDS: the rated judge only ever picks a STRICTLY better-scored
  piece than what is worn (`ArmorIsBetter`), and the mage judge's FormID tie-break is overridden —
  a pick that only ties the player's piece on (tier, metric) is dropped. Once MFO's strictly
  better pick displaces it, the economy KEEPS it: `IsPlayerPick(fid, form)` in the sell loop
  (`sdiag "playerPick"`), never a sale. Dolls mode declares the worn set as-is. SENT ONLY ON
  CHANGE: `g_lastDeclared` (anon, worker-serial like `g_nextTick`) holds the last sent
  `SentDecl {sorted (form, hand) pairs, owned, denied}` — a scope-only change (a new hold owning
  a hand) is sent too; `APMFBridge::DeclareEquipScope` then `DeclareEquipSet`, both in one call
  (one APMF Drain, one Publish); a fresh claim handle
  (F2) forgets it so the new handle is declared at once. **NO forced re-issue (F3, Fable round
  2):** a re-send is a declaration EVENT and under v7 APMF's slot-less pass answers it by putting
  the declared off-hand weapon in the RIGHT hand before MFO's hop puts both back — four ops and
  a flicker per re-send; the top-up's "give it back" is Actuation's explicit placement, which
  needs no re-declaration (the item is already in-set). **The judged armor pick is the OOC road's
  only (`a_judgeArmor`, F9)** — the combat road (`DeclareFromLedger`) declares worn armor as is,
  because legacy never wears armor in combat — and is computed ONCE per tick (F10):
  `EquipBestOwnedGear` hands its pick to the guard through `g_handedPick` (follower-keyed,
  consumed on the same worker call). Log lines: `[equip-auth] <id>: declare n=<count> [<names>]
  owned=<Armor+Right+...|none> denied=<Shield|none> (<why>[, new claim])` (`CategoryNames`,
  anon), `claim`, `release`.
  ROADS: `ServiceFollower` (`Logistics.cpp`, OOC, after the ~1 s cadence gate — CLAIM, then a
  scope guard declares at every exit so a loot/buy/owned-upgrade THIS tick is declared THIS
  tick; `a_leftReserved` from `APMFBridge::IsHealCastActive || IsOwnedCastActiveOnHand(left)`);
  `Actuation::DeclareFromLedger` (combat weapon events; the Scheduler never reaches
  `ServiceFollower` in combat); `OnFollowerRemoved` (dismiss + T#78 MFO-OFF edge via
  `Followers::ReleaseHeldState`) RELEASES + forgets — and **`ReleaseHeldState` itself releases the
  authority FIRST (F7) and, when it was live, defers `Loadout::Restore` (the pre-MFO gear repay,
  off-set) two `MainThread::Post` hops past APMF's next Drain and back onto the serial AddTask
  worker under `PumpTickGate`, so an enforcing APMF does not refuse the repay and the ledger is
  never mutated off its thread**; `ClearTransientState` →
  `ClearEquipDeclarations` (the claims go in `APMFBridge::ClearTransientState`, kPreLoadGame),
  so the first service after a load re-claims and re-declares. The gate OFF releases any standing
  claim (and `APMFBridge::Tick` releases on `bApmfEquipAuthority=0` for in-combat followers).
  DIRECT SITES GATED: `AcquireEquip` (`Logistics_Loot.cpp`) skips its `MainThread::Post`
  `EquipObject` hop for ARMOR when `EquipAuthorityLive(id)` (transfer, verdicts, MEO gem carry
  untouched; logs `'<name>' declared`); `EquipBestOwnedGear`'s `[equip] OWNED` line appends
  `| declared (APMF equip authority)`. `bLogistics=0` → no OOC road at all (the combat road
  still claims/declares). **What breaks:** declaring BOTH a held melee weapon and a stowed
  ranged one (or two items for one slot) makes APMF's pass displace one with the other on every
  declaration event — the set must be simultaneously wearable; declaring the shield at all
  re-creates the Fable 2026-09-15 shield-over-dagger loop as an APMF-performed equip (and would
  need Left owned, which refuses the AI's own bow); re-adding ANY "what is in the hand" read to
  the hands rule re-creates F6 (the AI's own pick declared, its next switch refused); owning a
  hand without an entry for it makes the AI's every equip there off-set (a blanket lock on that
  hand); dropping `(owned, denied)` from the change-detector key swallows a scope-only change (a
  new hold on an already-declared set never reaches APMF); sending the set BEFORE the scope
  lets APMF's seat pair the new set with the old scope for one Drain; a per-tick unconditional send is bounded by APMF (3 s per item) but is exactly the churn
  the log will show — keep the change detector; computing the armor pick with a second loop
  instead of `ComputeOwnedGearPick` lets the declaration and `EquipBestOwnedGear` drift; a
  refresh from a non-worker thread races `g_lastDeclared` (#4); dropping the `mfoEnabled` gate
  keeps a claim standing on a follower MFO must leave alone (T#78); an empty declaration
  (`SetEquipSet(h, nullptr, 0)`) is PASS-THROUGH, not "deny all" — never send it as a "hold
  nothing" instruction. APMF-SIDE GAPS THE PROBE MUST SETTLE BEFORE ENFORCEMENT (Docs/STATUS.md):
  all three round-1 gaps were closed by APMF v8 (governed types exclude potions; `PlayerMenu`
  allowed by default; `SetEquipSetEx` carries the hand) — and F6 (the hands freeze) is RESOLVED
  by the v9 scope (STATUS): the field observables are APMF INTEGRATION.md criteria 8-11 plus
  MFO's `declare ... owned=` line. OPEN BACKLOG on this entry (`Docs/REVIEW-BACKLOG.md`, raised
  against `7857446`): MFO-B44 (owning Ammo under a bow hold pins the archer to one ammo stack —
  marth's call), MFO-B45 (scope and set are two enqueues; a Drain between them is a one-frame
  self-healing mismatch), MFO-B46 (the `abiVersion < 9` warn is unreachable belt-and-braces).
- **THE HEAD SLOT IS THREE BIPED BITS (2026-09-14, field fix; branch
  `fix/mfo-deck-0914-helmet-offhand-verdict-meo`).** FIELD (Deck log, Fable, ROOT
  CAUSE CONFIRMED): vanilla helmets (Imperial Light Helmet `00013EDB`, Elven Helmet
  `0001391D`, Leather/Iron Helmet, Shrouded Cowl `000D2842`) carry BOD2 = 31 Hair +
  42 Circlet with NO bit 30 Head; IA/Dwemer helmets carry 30+31+42+43. Every
  kHead-only test therefore saw a vanilla helmet as "not a head piece": `ArmorBuySlot`
  returned -1 → never bought (`PlanBuy`'s per-slot `ArmorBuySlot(a) != slot`), 0 head
  baseline, never kept (the keep lambda), sold as junk; `ArmorIsBetter`'s `kSlots`
  had no Hair/Circlet → never looted/worn; and the INVERSE: a bit-30 helmet vs a
  worn 31+42 one read "bare" and MFO executed a scored DOWNGRADE (`OWNED armor
  'Dwemer Helmet' [Heavy] 20 <- worn 'Shrouded Cowl' [Light] 32.5`);
  `LogArmorClassIfChanged` printed `head (bare)` for a worn Cowl. THE FIX: ONE
  predicate `IsHeadSlotMask(mask)` (`Logistics_internal.h`, `Head|Hair|Circlet` —
  the SAME three bits `MageClothingSlot` and `WornInLogicalSlot(0)` already use; no
  third notion). CONSUMERS: `ArmorBuySlot` (`Logistics_Economy.cpp`, public — so
  `PlanBuy`, the buy baseline `slotScore[0]`, the `[equip] OWNED` diag's `old` read
  and the redundant-inferior force-sell key `10+ls` all follow), the keep lambda
  `armorLogicalSlot` (bucket 10), `ArmorIsBetter` (`Logistics_Loot.cpp` — the head
  is judged ONCE as a logical slot against `WornInLogicalSlot(a_follower, 0)`; the
  other slots stay per-bit), and the `[armor]` head row (`WornInLogicalSlot(0)`).
  Rating-0 circlets stay excluded by every consumer's own `rating<=0` gate — this is
  slot IDENTITY only. The mage-apparel path (`kMageSlots`, `MageApparelKey`'s
  "never fills a bare circlet bit" rule) is untouched. **What breaks:** a new
  head consumer that tests `Slot::kHead` alone re-opens the whole class of bug —
  route it through `IsHeadSlotMask` / `WornInLogicalSlot(0)`; `ArmorIsBetter`'s
  head compare must stay ONE comparison (per-bit Hair AND Circlet against two
  `GetWornArmor` reads can double-judge the same worn helmet).
- `Logistics_Loot.cpp` (2399) — the loot judge + per-category looters,
  claim-and-release, navmesh reach, the armor judge (`ArmorPrefFor:213`,
  `ArmorScore:235`, `LogArmorClassIfChanged:248`, `ArmorClassSuits:303`,
  `ArmorIsBetter:321`, `CarriesSlotArmorAtLeast:371`), `StyleVotesFor:513`,
  `ComputeWeaponRoles:555`, `AcquireEquip:791`, `LootGold:927`,
  `LootValuables:1131`, `HasLoot:1426`, `LootNearby:1523`, `StripCorpse:2291`,
  `RunExcursionScan:2355`. `LootEquipment` itself now lives in
  `Logistics_Loot_Equipment.cpp` (see below); everything else that was here
  (Jewelry/SoulGems/Ingredients/Valuables/Gold/Ammo/Potions/Lockpicks judges)
  is unmoved.
  **ROUTE 2b GENERALIZED TO EVERY CATEGORY (2026-09-06):** the loose-ref
  whitelist inside `LootNearby` (`:~1988`, a `switch (a_cat)` now, was an
  if/else chain) covers ALL ten `Category` values, each via the SAME
  predicate its container-side `Loot*`/`Is*Item` function already runs on one
  item — `IsCoinLoot`/`kGold001` (Gold, and Valuables which also still takes
  `IsValuableMisc`), `IsJewelryPiece` (Jewelry), `IsSoulGemItem` (SoulGems),
  `IsIngredientItem` (Ingredients), a `kLockpick` FormID match (Lockpicks),
  and `LooseEquipmentQualifies` (Equipment, see the new module below) — so
  `act.loot_soul_gems`/`_ingredients`/`_equipment`/`_jewelry`/`_lockpicks` now
  all pick up a loose item exactly like a container one. Quest/catalog
  NEVER-LOOT gating for the loose path is `LooseSpecialItemBlocked`
  (`:533`, wraps `IsQuestObjectRef:521` — the loose-ref analog of
  `IsQuestObjectInstance:510`, since a bare world ref has no
  `InventoryEntryData` to ask; calls `TESObjectREFR::HasQuestObject()`
  directly — VERIFIED against the exact pinned CommonLibSSE commit this repo
  builds against, portfile REF `c4ab853d`/`CharmedBaryon/CommonLibSSE`, not
  the newer CommonLibSSE-NG fork; a first pass guessed `ExtraDataList::
  HasQuestObjectAlias()`, which doesn't exist in the pinned version and
  failed CI) + the same `Catalog::IsExcluded`/`bLootSpecialItems` toggle —
  applied to every category whose container form also gates on it
  (Jewelry/SoulGems/Ingredients/Equipment/the Valuables-MISC branch); Arrows/
  Bolts/Potions/Lockpicks/Gold get none, matching their container form. The
  acquire MECHANISM is unchanged and untouched by this pass — every loose
  category still transfers via the engine's own `ActivateRef`
  (`Logistics.cpp`'s excursion arrival, MainThread-posted), never
  `PickUpObject`/AddTask (crash4 class); the whitelist only decides
  ELIGIBILITY. Nothing was deliberately excluded from the generalization —
  every `Category` ordinal now has a loose-ref path.
- `Logistics_Loot_Equipment.cpp` (475, NEW 2026-09-06) — split out of
  `Logistics_Loot.cpp` purely to stay under the 2500-line hard rule (pure
  mechanical move, no logic change). Owns the equipment judge:
  `BuildEquipmentContext:26` (the role/mage-mode gate + the follower's-own-
  gear baselines, extracted verbatim from `LootEquipment`'s old inline setup),
  `LootEquipment:221` (container scan, unchanged besides reading its context
  via `ctx.*` aliases instead of computing it inline), and the route-2b twin
  `LooseEquipmentQualifies:174` — mirrors `LootEquipment`'s armor/weapon
  branches for a SINGLE loose candidate (the container loop's "beats the
  running best" collapses to "beats the follower's baseline" for a lone
  item — same initial-value thresholds, so no separate rule to drift out of
  sync). `EquipmentContext` itself is a shared type declared in
  `Logistics_internal.h` (WeaponRoles-adjacent), since `LootNearby`
  (`Logistics_Loot.cpp`) constructs/holds one too (lazily, once per
  `LootNearby(Category::Equipment)` call, not per candidate ref).
  `IsCreatureWeapon`/`IsCreatureArmor`/`CarriesSlotArmorAtLeast` (still
  defined in `Logistics_Loot.cpp`, declared in the internal header —
  `CarriesSlotArmorAtLeast` newly added there, the split's other cross-TU
  miss CI caught) gate the loose path exactly like the container one — the
  creature-gear protection is not weakened.
- `Logistics_internal.h` (~750) — shared substrate: all `g_*` maps/state
- `Logistics_Loot.cpp` (2419) — the loot judge + per-category looters,
  claim-and-release, navmesh reach, `AcquireEquip:550`, `LootEquipment:629`,
  `LootGold:1026`, `LootValuables:1230`, `HasLoot:1525`, `LootNearby:1622`,
  `StripCorpse:2282`, `RunExcursionScan:2346`.
  **GOLD + LOOSE GEMS FOLD INTO VALUABLES (2026-09-05):** `Category::Valuables`
  (`Logistics_internal.h:256`) now also matches gold — `LootValuables`
  (`Logistics_Loot.cpp:1230`) peeks/takes gold by calling `LootGold` (`:1026`)
  directly rather than re-deriving a gold count (never `Actor::GetGoldAmount`,
  which null-derefs — `LootGold` sums `Gold001`/OCF-coin off `GetInventory`).
  The route-2b loose-ref whitelist inside `LootNearby` (`:1773`) accepts a
  loose `Gold001` ref for `Category::Valuables` the same as it always has for
  `Category::Gold`, AND a loose value-dense MISC ref (a dropped gem etc.) for
  `Category::Valuables`, gated by the same `IsValuableMisc` (`:1203`,
  value/weight ratio vs `Config::g_valuablesRatio` — `Config.h:586`) the
  container take already uses — a loose ref qualifies iff a container holding
  it would have been looted. `act.loot_gold` is UNCHANGED, still a gold-only
  rule. Loose soul gems/jewelry/ingredients/equipment are still NOT
  whitelisted — loose-item pickup (route 2b) isn't generalized to every item
  type yet; that generalization is a documented follow-up, not built here.
  BEHAVIOUR CHANGE: an existing `act.loot_valuables` rule now also picks up
  coin and loose gems.
  **[L] GLYPH RELIABILITY FIX (2026-09-05, marth):** `IsLooting` (`:626`,
  `SlotOf(id) != nullptr`) is a pure travel-slot proxy — true the whole walk-
  there and even on an arrival that finds nothing, false for arm's-reach loot
  that never claims a slot. The board's "Looting" signal now reads
  `JustLooted` (`:657`) instead, stamped by `MarkJustLooted` (`:651`) at the
  THREE confirmed-acquisition points in `Logistics.cpp` — the arm's-reach
  fall-through (`:1563`, gated `IsLootOp` — `Logistics_internal.h:318`), the
  loose-item Activate readback (`:790`), and the `StripCorpse` call (`:946`).
  Window sized off the round-robin cadence (`partySize * kPumpMs`, #9), not a
  guessed constant — see `Logistics.h`'s `JustLooted` doc. `IsLooting` itself
  is UNCHANGED and still used by other callers; don't re-wire the board back
  to it.
  `EconomyProbe:488` (follower-side sell-candidate/buy-needs state built ONCE
  per call, ~583-909; only the per-vendor VEND-filter pass + chest/gold read
  stay inside the `for (auto& h : living)` loop, ~911-968 -- 2026-09 perf fix),
  public buy helpers (`MageApparelBuyKey:1066` et al).
- `Logistics_Loot.cpp` (2358) — the loot judge + per-category looters,
  claim-and-release, navmesh reach, `AcquireEquip:538`, `LootEquipment:617`,
  `LootNearby:1609`, `StripCorpse:2250`, `RunExcursionScan:2314`.
- `Logistics_internal.h` (715) — shared substrate: all `g_*` maps/state
  (`g_svc:222`, `TravelIntent:283`, `g_travelSlots:323`, `g_stockMx:568`,
  `g_stockGear:569`, econ clocks), `Category`/`LootMode`/`WeaponRoles`/
  `EquipmentContext`/`Claim`, inline small helpers, cross-module declarations.
  NOT public API.
Adding shared state? Put it in `_internal.h` as `inline` (never a per-TU
anonymous-namespace copy — that silently forks the instance).
- **SAVE-COMPAT — `g_stockGear`/'MSTK'** (`Logistics_internal.h:569`, guarded
  `g_stockMx` `:568`, the one cross-thread map here): the only serialized state the cluster
  owns. `CopyStockGear` (`Logistics.cpp:2105`) → `Serialization.cpp:209`; `LoadStockRecord`
  (`:1690`) → `:332`; `ClearStockGear` (`:1695`) → `:256,646`. Only
  `IsPersistableID` FormIDs written, sets capped 512, unresolvable IDs dropped.
  Changing the map's key/value shape or record framing breaks the shed-protection
  ("Gauldurbow fix") — signature gear could get shed (dropped on the floor) after a load.
  **Not** cleared by `ClearTransientState` — cleared separately by `ClearStockGear`.
- `ServiceFollower` (`Logistics.cpp:802`) — callers `Scheduler.cpp:393` (the party-OOC
  branch) and `serviceOwnOoc` `Scheduler.cpp:459` (an own-OOC follower inside a party fight,
  no-action exits only), both on the worker. Sets
  `g_svc` (`Logistics_internal.h:222`) raw pointer valid only for that call — safe only because the
  worker services followers sequentially; parallelizing dangles it.
- `ShedOffRoleWeapon` (`Logistics.cpp:506`) — one off-role weapon per idle tick, **DROPPED on
  the floor** (no longer handed to the player; no value split, no knob — marth
  simplified). Disposal is the engine's `DropObject` vfunc (a world-ref/3D create) so it MUST
  go through `MainThread::Post` (`doDrop` `:607`, mirrors the #62 equip / ActivateRef
  hops in this file). **`doDrop` calls the vfunc DIRECTLY (slot 0xCB SE/AE, 0xCD VR)
  with an explicit `ObjectRefHandle* a_out` second parameter -- NEVER through
  `RE::Actor::DropObject`**: CommonLib 3.7.0's `RelocateVirtual<>` wrapper builds a
  free-function type with no non-POD-return handling, so MSVC puts the sret slot in
  `rcx` and `this` in `rdx`; the engine then reads MFO's stack temporary as the Actor
  (deterministic CTD, 4x on LoreRim, fixed v2.0.7; principle 6, the third instance).
  Verified against the unpacked 1.6.1170 (`+0x6781D0`) and 1.5.97 (`+0x5E6150`) binaries; on VR (`!MainThread::IsInstalled()`) it SKIPS rather than
  drop off-worker. **POST-BATTLE GATE:** early-returns until `kShedPostBattleDwell`
  (3 s) since `g_lastCombatSeen[id]`, stamped by `NoteInCombat` (`Logistics.cpp:2158`) ←
  `Scheduler.cpp:409` (the PARTY-combat branch, every party-combat service, own flag or
  not — since 2026-09-21 this path also runs for an own-OOC follower inside a party fight,
  via `serviceOwnOoc`). Survives an `IsInCombat()` mid-fight flap: a party-combat service
  re-stamps `now`, so the dwell can't mature inside a lull or a party fight (the field
  2h-follower-hands-a-looted-mace bug). `g_lastCombatSeen`
  worker-only/no-lock (#4), cleared in `ClearTransientState`. Guards unchanged
  (never disarm/`inRoleWeapons>0`, `IsStockGear`, `IsCreatureWeapon`, socketed,
  `Catalog::IsExcluded`). **FISTS RULE (2026-09-14, marth: "they can hold them, but
  they shouldn't be valid for fighting, unless progression is installed AND unarmed
  perks are selected via progression"):** the engine's Unarmed record (`0x1F4`,
  `kHandToHandMelee` → `WepClass::Other`, in EVERY inventory) used to fall through
  `inRole` as `true`, so fists alone satisfied `inRoleWeapons>0` and the shed dropped
  a follower's ONLY real weapon (LoreRim 2026-09-12, Cosnach). Now `inRole` returns
  `fistsValid = Progression::Get().built && StyleVotesFor(f).unarmed > 0` for `Other`
  (the same worker-side mirror `ComputeWeaponRoles` reads — no perk walk on the
  worker); a not-valid Unarmed record is skipped, never a shed candidate. Logged
  `[shed] <id>: fists <valid|not valid> (progression=<y/n>, unarmed perks=<n>)` with a
  candidate in hand, once per CHANGE (`g_shedFistsLogged`, `Logistics_internal.h`,
  worker-only, cleared in `ClearTransientState`). **Default-case delta:** the ONLY
  changed outcome is a follower holding one off-role weapon and no real in-role one
  (DROPPED before, KEPT now); every other path is unchanged. **What breaks:** making
  `Other` in-role again (or counting `unarmed` off `HasPerk`) re-opens the
  disarm; the vote MUST come from the allocation record (see Progression's
  `TallyStyleVotes` note). The mirror the shed reads MUST stay cleared on load
  (`Serialization.cpp ResetAllState` → `Logistics::ClearStyleMirror`, after
  `StopPump` + `MainThread::Clear`) or a stale `unarmed` from the previous save
  drops a weapon on the first post-load tick (Fable F1 on `49a9cc2`). **OPEN
  BACKLOG: `Docs/REVIEW-BACKLOG.md` MFO-B14** (SEV-4) — a "left hand empty"-only
  one-hand perk can vote `unarmed` and make the shed strip a legitimately wielded
  off-role weapon; read it before touching `inRole` or the unarmed classifier.
- `ClearTransientState` (`Logistics.cpp:2049`) → `Serialization.cpp:641`, after StopPump. Wipes
  the loot/drink/econ/travel maps (calls `Packages::LootTravelClear` first). Moving
  a clear out, or calling while the pump is live, races a worker insert (UB).
- Pure reads (evaluator + economy, shared classifiers): `PotionRestores` (`Logistics.cpp:259`),
  `AmmoIsBolt`, `CountPotions`/`ArrowCount`/`BoltCount` (`:305-362`) →
  `Evaluator.cpp:397-409` + `TradeBridge.cpp:52-71` (buy side shares them so bought
  supply matches looted). `ComputeWeakPotionFloor` (`Logistics.cpp:332`) ← `plugin.cpp:288`
  (after `Catalog::Load`).
- **Alias/travel:** `g_travelSlots` (`Logistics_internal.h:323`, `kMaxLootSlots=4`) maps follower→loot
  alias pair. Travel fill is **engine-serialized**; every exit path MUST call
  `Packages::LootTravelClear` (this follower's own combat via `ReleaseTravelOnCombat`
  `Logistics.cpp:2192` ← `Scheduler.cpp:486`, keyed on the follower's OWN flag; the PLAYER's combat via the global
  backstop `Logistics.cpp:~664-728`, see the PLAYER-COMBAT LOOT INTERRUPT note
  above; cap/leash/dismissal/revert). Leash hysteresis guards
  (`followerBeyondLeash` in `LootNearby`, ×1.15 in `ServiceFollower`) prevent the ~1/sec claim/evict churn.
  **Theft guard (RC#4):** the Walking driver (`ServiceFollower`, `Logistics.cpp:~700`) detects an EXTERNAL package
  holding a claimed follower (scene/framework; onTravelPkg=false mid-walk), pauses
  the stall/deadline clocks (`stolenSince`, `kStealGrace=10s` `Logistics_internal.h`) and re-asserts
  via `EvaluatePackage(true,false)`; only a genuine on-package zero-move stall
  (`kNoProgress=5s`) reaches the sticky blocklist — routing theft through the stall
  path re-poisons reachable loot 5 min at a time (the 12:25 deck trace).
  **Theft BACK-OFF:** a claim stolen repeatedly re-asserts forever (deck: "travel
  pkg stolen … re-asserting claim" every few sec, leg never completing). `g_stealStrikes`
  (`Logistics_internal.h`, keyed `StealKey`=follower<<32|target) counts displacements;
  at `kStealStrikeMax=4`, or while `IsInCombat()`, the leg ABANDONS to the transient
  blocklist (`MarkTravelFailed`, never sticky) instead of re-asserting. Reset on
  arrival (`Logistics.cpp:~820`, provably reachable) or target change (fresh key);
  erased on every give-up. Normal single-steal-then-reclaim path unchanged.
  **QUIET HOLD — NOW OBSERVATION-GATED (2026-09-03, CORRECTED 2026-09-07,
  `Docs/DIAG-2026-09-06-loot-travel.md`):** the theft-guard above is skipped for an
  APMF-held leg ONLY once that leg has been SEEN running the travel package.
  `Logistics.cpp:~1080` now reads `apmfLeg && tr.legEngaged`, not
  `Packages::IsAPMFTravelHeld(slot)` alone. The old unconditional bypass rested on a
  probe-era premise ("0x49 re-holds the package every engine re-eval, so a momentary
  framework `curPkg` is benign") that the 2026-09-06 field session falsified: **0 of
  6 dispatches ever adopted the package**, so the hold being trusted did not exist,
  and the stall/deadline clocks convicted corpses while the follower merely followed
  the player. `IsAPMFTravelHeld` (`Packages.cpp`, right after `LootTravelClear`)
  still exposes the file-local `g_apmfSlotActive[slot]` flag across the TU boundary.
  - **`legEngaged` / `legStart` / `nextLegPkgDiag`** (`Logistics_internal.h`
    `TravelIntent`, worker-tick-only, NOT serialized) are the per-leg record; all
    three are RESET at every dispatch AND every retarget (`Logistics_Loot.cpp`, both
    `TravelDeadline(...)` sites). `legEngaged` is set from ONE
    `Forms::IsTravelPackage(GetCurrentPackage())` read per Walking tick, shared by
    the engagement record, the WALK diagnostic and the guard decision.
  - **Three new [loot] lines** (all throttled, all report-only — no retry, no
    watchdog): `TRAVEL PKG ENGAGED` (once per leg), `TRAVEL PKG NOT ENGAGED`
    (first Walking tick of a leg then ≤1/4s, per-leg timer so a new dispatch is
    never swallowed), `TRAVEL PKG DISPLACED` (engaged-then-displaced = RC#3, the
    runtime-FF-package case, ≤1/4s — deliberately OBSERVABLE ONLY, no recovery).
  - **`DEADLINE EXPIRED`** (`Logistics.cpp`, plain-deadline branch, ≤1/2s per
    follower) ends the silent `MarkTravelFailed` that hid three of five "batch done"
    releases; it carries cat/budget/overrun/dist/curPkg/route/`legEngaged`.
  - A never-engaged APMF leg now falls into the NORMAL guard: `stolenSince` pauses
    the ref's clocks, the strike/grace machinery runs, and the leg abandons to the
    TRANSIENT blocklist after `kStealGrace` — the ref is never stickied for a
    package that never ran. Its log line says `NEVER ENGAGED (not a theft)`.
  - **What breaks if you change this:** dropping `legEngaged` from the bypass
    condition restores the false-verdict masking; setting it from anything other than
    a live `GetCurrentPackage()` read re-introduces "claim requested == leg engaged",
    which is the exact inference this whole entry exists to forbid.
- **Loot scan is MULTI-CELL** (`LootNearby` `Logistics_Loot.cpp:1307`; cell set built at `:1417`):
  follower's + player's + live travel-target's ATTACHED parent cells, all anchored
  to refs in hand — **never** `TES::ForEachReferenceInRange`/worldspace derefs
  (crash4). Dropping back to one cell re-blinds exterior scans across cell borders
  ("second gold pile on the same table"). Idle blocklist reassess (`ServiceFollower`) is
  AGE-GATED (≥10s): a full wipe let follower B erase follower A's 200ms-old fail
  verdict → instant same-target redispatch churn.
- **LOOT RUN ORDERING, no-dibs-first (2026-09, marth: "no-dibs + closest only
  first, then the dibs items later in the runs, no pauses").** Both gambit-
  table dispatch loops that pick a loot action — the normal per-tick loop
  (`Logistics.cpp:~1114-1512`) and the excursion leg-boundary dispatcher
  (`RunExcursionScan`, `Logistics_Loot.cpp:2128`) — now run the follower's
  gambit table TWICE: pass 0 defers any dibs-tier loot op (Equipment/Gold/
  Jewelry/SoulGems/Valuables — `IsDibsTierLootOp`, `Logistics_internal.h:308`)
  so free-tier categories (arrows/bolts/potions/lockpicks/ingredients —
  `IsFreeTierCat`, `Logistics_internal.h:298`, mirrors `TierReleased`'s own
  free branch) always get first crack at the tick REGARDLESS of the player's
  gambit-table priority order; pass 1 is the original unrestricted walk, so a
  released/cleared dibs item still loots normally once nothing free-tier
  fired. `StripCorpse` (arm's-reach full drain once already at a body,
  `Logistics_Loot.cpp:2064`) is UNCHANGED — it already skips a still-dibs'd
  category per visit without stalling (leaves the body eligible for a future
  revisit), so reordering it can't remove a pause that doesn't exist there.
  Also removed: the excursion `Holding`-phase linger-and-re-scan hold that
  fired specifically when the only remaining nearby loot was dibs-waiting
  (`g_scanSawWaiting`, bounded by `fBatchLinger`, `Logistics.cpp` HOLDING
  block) — that was a real, if short, stall-in-place; the follower now
  releases the excursion at once instead of holding position, and the
  dibs-waiting item re-enters naturally once `TierReleased` flips.
  `g_scanSawWaiting` (`Logistics_internal.h:430`) is now write-only (still set
  by `LootNearby`, no remaining read) — harmless, kept for potential future
  diagnostic use. Ordering only: `TierReleased`'s timing (`fFirstDibsDelay`/
  `fFairChance`/`g_abandonDelay`), the APMF loot-travel path
  (`OfferPackage`/quiet-hold, `Packages.cpp`), the combat-eviction, the
  excursion cap, and the leash are all untouched.
- **UNIFIED LOOT-FAILURE MODEL + in-reach drain (perf/stall pass, 2026-09):**
  (a) candidate sort (`LootNearby`, `Logistics_Loot.cpp:~1769`) is failed-recently-
  LAST then closest-first — a path-troubled target is DEPRIORITIZED, never removed;
  (b) a NON-LOOSE in-reach source is DRAINED whole (`StripCorpse` from inside
  `LootNearby`) and the normal-mode loop keeps draining further in-reach sources
  the same tick (`drained` counter; movement dispatches still end the tick);
  (c) GROWN GRAB (`g_grabGrow`/`GrabRadiusFor`/`NotePathFail`,
  `Logistics_internal.h:~402-432`): each path-fail (off-navmesh pre-gate, walked
  no-progress stall) widens that ref's from-range grab radius
  `kArrivalDist+100/fail` capped 600u — the PRIMARY stall cure; player-bubble +
  leash + `TierReleased` dibs still gate a grown grab, loose refs excluded;
  (d) the off-navmesh PRE-gate (`Logistics_Loot.cpp:~1904/~1963`) is TRANSIENT-only
  (`MarkTravelFailed`, never a sticky strike) — only a walked no-progress stall or
  the loose/unacquirable case reaches the sticky set, whose cooldown is now
  `kTravelStickyCooldown=60s` (was 5 min). Walk paths still hard-skip inside the
  25s fail cooldown (re-walking resets the stall clock and would defeat the
  verdict); grab paths never consult the blocklist. Weakening (d) or removing the
  walk-skip re-opens the frozen-Erik churn loop; removing (a)'s sort key stalls
  followers on unreachable-first ordering again.
- **Sinks** (`RegisterSinks` `Logistics.cpp:1949` ← `plugin.cpp:297`): `ContainerSink`
  (`TESContainerChangedEvent`) — **direction filter mandatory** (`newContainer==
  PlayerID()`, `ContainerSink` in `Logistics.cpp`) or it re-fires on its own removal (MAO infinite-credit loop);
  only QUEUES to the worker. `BeastHeadSink` (`TESEquipEvent`, `Config::g_beastHeadFix`)
  → `KeepHeadClear`; since 2026-09-14 it also emits the passive `[armor-obs]` line
  (`Logistics.cpp:156`) for every rated-ARMO equip/unequip on a tracked follower
  BEFORE its own equipped-only / toggle gates (pure reads, `Followers::IsTrackedFast`). `SweepBeastHeadsOnLoad` (`Logistics.cpp:1968`) ← `plugin.cpp:360`.
- `OnFollowerRemoved` (`Logistics.cpp:2040`) ← `Followers.cpp:306` (dismissal alias eviction).
- Hardcoded base FormIDs (stable): Gold `0x0F`, Lockpick `0x0A`, player `0x14`,
  house loc types, PlayerFaction — resolved/used throughout.
- Economy probe (`EconomyProbe`, worker, `Config::g_economy && Po3Present`, now takes
  the whole `FollowerState`) pushes the actual merchant read to Papyrus via TradeBridge
  — native `GetInventory`/`GetGoldAmount` CTD on merchant chests. **Do not** move the
  read back to native. `BuildBuyThresholds` (just above it) reuses the loot judge to
  fill `TradeBridge::BuyThresholds` (weapon/armor/apparel/tome buy). `LearnCarriedTomes`
  (worker, gated on `HasCastGambit`, called from `ServiceFollower` beside the econ
  probe) auto-learns castable tomes a mage carries (copy of `Board.cpp`'s teach
  primitive; `AddSpell`+`RemoveItem`, worker/edit-drain-safe, NEVER `MainThread::Post`).
  Mage-vs-armor apparel gate in the loot judge keys off `useMageApparel = mageMode &&
  Config::g_mageWearRobes` (bMageWearRobes OFF → caster loots rated armor).
  `mageMode` (`Logistics_Loot_Equipment.cpp:77`, inside `BuildEquipmentContext`)
  requires `castGambits > 0` AND the follower is
  PRIMARILY a caster (base class Mage, or — Auto/no class — no melee/ranged attack
  gambit) — a Ranged/Melee follower with a secondary cast gambit stays out of mageMode
  and keeps his class loadout (2026-09 fix; was any-cast-gambit, which flipped a
  Ranged follower like Adelinda into school-scored robes).
- **#21 College tome-gate unlock.** `UnlockCollegeTomes` (`Logistics_Economy.cpp:220`, worker,
  `ServiceFollower` idle branch, gated `g_economy && g_economyBuyTomes`, GLOBAL ~30s
  rate-limit) generalizes vanilla's player-skill tome gate to the party: for each of
  15 `PC{School}{tier}` globals (Skyrim.esm `0x000F2584..0x000F2592`, dumped via
  `tools/esp_inspect.py`), if the party MAX **base** skill (player + `g_active`,
  `GetBaseActorValue`) ≥ the tier threshold (Adept 50 / Expert 75 / Master 100) and
  the gate is ≠ 0, flip it to 0. **ONE-WAY** (never back to 100 — mirrors WISkill-
  Increase02, never fights the player's tracking), **natural restock only** (no chest
  regen, no merchant mutation), idempotent. GLOB writes batched through
  `MainThread::Post` (re-resolve on-frame; GLOB values are save-persisted, so this is
  the same field vanilla writes — no co-save risk). `[college]` log on first flip.
- **#21 equip + unified apparel judge (loot ⇄ buy).** The loot equip step is factored
  into `AcquireEquip` (`Logistics_Loot.cpp:575`, v1.0.38 SAFE path: `MainThread::Post` +
  `ActorEquipManager::EquipObject`, **never DoReset3D** #62; MEO gem capture +
  `QueueGemMove`). `a_src==nullptr` ⇒ the follower already owns the item (buy / owned
  upgrade). `LootEquipment` (`Logistics_Loot_Equipment.cpp:221`) routes through it;
  the **mage apparel selection is now the
  unified `MageApparelBuyKey`** (MEO-aware value/school ranking) across clothing slots
  (jewelry stays on the Valuables/`LootJewelry` path — dibs preserved). `EquipBestOwnedGear`
  (worker, `ServiceFollower` idle branch, dolls-gated) wears the single best OWNED
  upgrade per tick — bought, looted-as-valuable jewelry, or player-handed — via
  `AcquireEquip(src=null)`, which is what fires the **buy-path gem transfer**. Idempotent
  + one-per-tick ⇒ converges, no thrash. Old `MageApparelIsBetter` is now `[[maybe_unused]]`.
For a `useMageApparel` follower the branch is now **AUTHORITATIVE** (clothing-oscillation
fix): it computes the single best OWNED piece per logical slot (deterministic, FormID
tiebreak) and force-equips it whenever the worn piece isn't exactly it — so an
engine-re-equipped LESSER clothing piece is replaced and falls through to the sell loop
as an extra instead of being worn-protected forever. Thrash-guarded: **out of combat +
rate-limited 5 s/follower**, no-op once the best is worn (equip auto-unequips the lesser,
never strips naked). The rated-armor branch (2026-09-14) is THE armor wear decision:
highest `ArmorScore` owned piece that strictly beats the worn score — see ARMOR CLASS
BY SKILL + PERKS above.

### Loadout.cpp / Loadout.h — the equip/spell-in-hand ledger (NOT serialized)
Puts a gambit spell in a follower's hand, records displaced gear as **transient
debt**, hands it back on hit/combat-end/dismissal. Reconstructed from live state on
load — no co-save record.
- Five deliberately-separate main-thread-only maps (`:9-57`): `g_debt`, `g_lastStow`,
  `g_equipClock`, `g_coolUntil`, `g_mfoSpell` — merging them re-introduces named
  regressions.
- `LeftHandSlot()` (`Loadout.cpp:142`, decl `Loadout.h:49`) — PUBLIC since 2026-09-13
  (moved out of the anon namespace); **body CHANGED 2026-09-15 (merge of
  `feat/mfo-1.5.97-equipslot`, `04da1d4`): `RE::TESForm::LookupByID<RE::BGSEquipSlot>
  (kLeftHandEquipSlot = 0x00013F43)`, NOT `BGSDefaultObjectManager::GetObject<>
  (kLeftHandEquip)`.** Pinned CommonLib's `IsObjectInitialized(idx)` is `RelocateMember<bool*>
  (this, 0xB80, 0xBA8)[idx]` — it DEREFERENCES the objectInit bool array as a pointer: on 1.5.97
  that is the `0x0101010101010101` poison AV of the T#67 crash reports; **on 1.6.1170 `+0xB80`
  is `objects[364]` = DOBJ `HMAE` = Update.esm FLST `0x01003275` with record flags `0x0`, so
  the "init" byte was ALWAYS 0 and the function ALWAYS returned `nullptr` in the field (Fable
  round 2, measured against the AE DOBJ tag table: 366 entries, 19 = LHEQ, 20 = RHEQ, 364 =
  HMAE, 365 = MHFL). Every AE spell equip therefore ran `EquipSpell(actor, spell, nullptr)`
  and the engine's helper (AE `0x6CAB20`) picked the hand for an EitherHand spell;
  `EquipLeftHeld` (`Actuation.cpp:1748`) returned false on its `!slot` test and the dual-wield
  off-hand force never fired. THIS is the first build where the slot reaches the engine on AE
  — see Actuation's RUNTIME GATES entry for the observables.** `Docs/ADDRESS-TABLE-2026-09-15.md`
  rows: "held branch `Loadout.cpp:63,81`" (by construction — a Skyrim.esm FormID, the same
  form the engine's own `InitItemImpl` fills `objects[19]` from; the engine's `GetLeftHandSlot`
  SE `0x3315F0` reads `[dom+0xB93]` then `[dom+0xB8]`) and "`BGSDefaultObjectManager::objects[]/
  objectInit[]` count" (364 / 366 / 372). ONE implementation, no `RightHandSlot` twin.
  `plugin.cpp`'s `[runtime]` line prints the resolved FormID (error if null). The `<d3d11.h>` `GetObject`
  hijack note no longer applies to this TU. The slot is shared by `Prepare`'s `EquipSpell`
  (`:325, :413`) and `Actuation.cpp`'s `EquipLeftHeld` (dual-wield left-hand WEAPON equip). It was the only hand-machinery
  piece usable for a weapon — `PlanCastHand`/`CastProxyOnHand`/`ClaimLiveOnHand`/`kHand*`
  are cast-only, and no `RightHandSlot` exists. Changing which slot form it resolves
  re-points BOTH the spell-in-hand path and the left weapon hold at once.
- `Prepare(actor, spell, why, LeftHandYield a_yieldLeft = nullptr)` (`Loadout.h:128-135`,
  `Loadout.cpp:261`) — since `1ac3c6b` (Fable F1/F2 on `f771399`) carries THE LEFT HAND'S POINT
  OF NO RETURN: the function-pointer callback is invoked immediately before each `EquipSpell`
  (`:304` the spell→spell branch, `:391` the displacing branch), i.e. AFTER every refusal
  `Prepare` can still make (cooldown, two-handed debounce, open gear debt). On the displacing
  branch a `true` return ("a hold was released") nulls `willDisplaceLeft` so MFO's OWN left
  weapon hold is never booked as gear debt. `RestoreOne` (`:98`) repays a displaced LEFT-hand
  WEAPON with `LeftHandSlot()` (`EquipBack` `:88` gained a slot parameter). Sole caller
  `Actuation.cpp:1047` (`CastOn`, always passes the yield). **What breaks:** calling the yield
  before the refusals re-creates the F1 per-lap flicker; booking the debt on a `true` return
  re-creates F2 (repay in the RIGHT hand at combat end, then every later `Prepare` Debounces on
  the open debt); a second `Prepare` caller that omits the callback makes its `EquipSpell` meet
  the left prevent-removal lock. `OwesLeft(id)` (`Loadout.h:195`, `:672`) is the query
  Actuation's left weapon holds are gated on (F-A) — it must keep reading `displacedLeft`
  only (a stowed two-hander is a RIGHT-hand debt and must not block the left).
  See §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS".
- `Prepare` (`:240`) → sole call `Actuation.cpp:1097`. `StartCooldown` (`:482`) → Actuation
  + Diagnostics; it sets the re-EQUIP debounce `g_coolUntil` and `ReleaseSpell`s, and
  since 2026-09-23 mirrors NOTHING into `CasterConsent` (the cast-time pacing deny is
  deleted — see the CasterConsent entry's "NO CAST PACING"). `Tick` (`:465`) ←
  `Diagnostics.cpp:256` (settles debts). `Reconcile` (`:561`) ← `plugin.cpp:361`
  (undo a save taken mid-cast; iterates `g_active`, main-thread). `ClearTransientState`
  (`:625`) ← `Serialization.cpp:595`.
- **Collect-then-act** (`:485`): `EquipObject` dispatches synchronous events;
  mutating `g_debt` mid-iteration is the MEO use-after-free shape. `DeselectSpell`
  (not `UnequipObject`) is load-bearing for spells.
- **`HandPick`/`PlanCastHand`/`CanDualCast` (2026-09-06, `:157-238` in `Loadout.h`,
  impl `:156-238` here) — the intelligent hand-selection POLICY (marth's "MFO
  decides WHICH hand(s) to claim" pass).** Pure decision, no engine writes, no
  APMF dependency (this module stays framework-agnostic). `PlanCastHand(actor,
  spell, weaponHandActive)`: weapon-hand-active → `Left`; else both hands free
  → `CanDualCast` (real HasPerk-or-base-template-perk check against the vanilla
  Skyrim.esm Dual Casting perk for the spell's OWN school, `0x000153CD`..
  `0x000153D1`, `DualCastPerkForSchool` anon-ns `:168`, + `CalculateMagickaCost
  x 2.8` affordability, the vanilla `fMagicDualCastingCostMult` hardcoded the
  same way `fBarterMax`/`fBarterMin` already are in `Logistics_Economy.cpp`) →
  `DualCast` or `EitherFree`. **Claim shape now supports it (2026-09-06)** —
  `APMFBridge::HandFor(HandPick)` + `kApmfHandDualCast` translate a `DualCast`
  plan into `APMF_API::kCastFlag_DualCast` on a `kIntent_Cast` claim (see
  `APMFBridge.h`/`.cpp`'s `EnsureHealClaimLocked`) — but **still NOT wired to
  any live caller**: heals bypass this policy entirely by hard rule (heal is
  LEFT, unconditionally, see `APMFBridge.h`'s `ClaimHealCast` doc), and
  offense's `ClaimCasting` (`Actuation.cpp`) neither carries a hand parameter
  nor uses a claim shape (`kIntent_SelectSpell`) that can carry `CastFlags` in
  the first place. `APMFBridge::WeaponHandActive` (`APMFBridge.cpp`, combines
  this file's `Read().grip` with `IsEquipmentClaimActive`) is the intended
  `a_weaponHandActive` producer for the APMF-owned path. **What breaks:** none
  yet (no live call sites) — but `DualCastPerkForSchool`'s FormIDs are
  hardcoded (no INI override, unlike `Config::g_merchantPerkID`); a
  perk-overhaul that relocates the vanilla dual-cast perks would need one
  added before this is wired live.

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

### Progression.cpp / Progression.h — component 1: the catalog reader + addon discovery
One pass at kDataLoaded over merged AVIF perk trees → a value-only frozen `Catalog`.
Mutates nothing. `Init()` (`:655`) ← `plugin.cpp:286` (immediately before
`ProgAllocator::Init` — order load-bearing).
**v1.1 addon discovery (Vortex fix):** `Init` recognizes a manifest FLST by its
FIRST entry being a KEYWORD whose editor-id ends `_MFOAddonManifest`
(`EdidHasSuffix`, `kManifestKeywordSuffix`) — NOT the retired MFO.esp sentinel.
Keyword editor-ids persist at runtime; GLOB/FLST edids do NOT (the Phase 2 root
cause), so this is the one edid match that resolves. The addon references only its
OWN keyword → no MFO.esp master. `AddonRef` gained `keywordEdid`.
**v1.1 generic manifest model:** `AddonManifest`/`ManifestClass`/`ManifestEconomy`/
`ManifestAllocation`/`ManifestVerdict` (+ `ManifestBoardTab`) is the
add-on-agnostic host model, built by `ProgAllocator::BuildGenericManifests` and
exposed via `Progression::Manifests()` (defined at the foot of `ProgAllocator_Manifest.cpp`).
**v1.1 residual #3 — perk-effectiveness SPLIT (the last compiled-in judgment, removed):**
the WALK is the general public primitive `WalkPerkEntries` (`:657`, Progression.h) +
`EntryPointName` (`:653`) — add-on-agnostic, enumerates a perk's entries, their
kind/entry-point index+name, and the mechanical `firesForNpc` fact (quest / player-gated
ability); NO verdict. The effective/marginal/dead VERDICTS are now ADD-ON DATA: an add-on
declares them in a verdicts sub-FLST (front keyword edid `_MFOEntryPointVerdicts`, then 92
POSITIONAL verdict GLOBs). `ReadEntryPointVerdicts` (`:717`, the ONE reader) fills the
classifier's runtime `g_verdicts[92]` at `Init` (before `BuildCatalog`) AND
`AddonManifest::entryPointVerdicts` in `BuildGenericManifests` — NO DLL default (delete the
add-on → all -1, `ClassifyRank` shows no effectiveness hint). The old `kEntryPoints[]` verdict
table is gone; `kEntryPointNames[92]` (names only, a general engine fact) remains.
**Consumers routing on:** Phase 5 (`sharedGrowthEnabled`); **Phase 6a** — `Board_FieldKit.cpp`
`DrawFieldKit` iterates `Manifests()[].boardTab.declared` to decide the hosted board-tab
count (was hardcoded `snap.prog->active`); `BuildGenericManifests` sets `boardTab.declared=true`
+ `label`. **Phase 6b** — `BoardProgSnap`/`BoardFollowerView` wrapped behind a generic
`BoardTabView` (`ProgAllocator.h:378`, `CopyBoardTabViews`). **Phase 6c (Phase 6 COMPLETE)** —
(1) the tab CAPTION is `boardTab.label`, sourced from the add-on's own `MFOP_BoardTabLabel`
MESG (FULL "Progression", manifest entry[1] before the classes FLST; `BuildGenericManifests`
captures its FULL, no DLL literal) — `Board_Progression.cpp:43` `BeginTabItem(hostedTabLabel)`. (2) The
board edit queue's progression verbs collapsed to ONE generic carrier `EditKind::AddonAction`
+ `EditCmd::verbId` (`AddonVerb` enum, `Board_internal.h:48`); the `verbId`→backend dispatch
(`ApplyEdits`, `Board.cpp:882`) stays progression-shaped (Phase 7/9). The tab BODY
(all of Board_Progression.cpp) + view payload are still add-on-typed (Phase 7/9). `Get()` read by ProgAllocator (`:143,
150,437,666,935,1047,1264,1554`) + `Board_Progression.cpp:202`. `kAddonPlugin=
"MFO_Progression.esl"` (`:30`). **What breaks:** the catalog is the load-time drop
oracle — `CoSaveLoad` drops any perk alloc whose node is no longer in `Get()`
(`ProgAllocator.cpp:2117`), so re-tuning the add-on's declared verdicts (the
`MFOP_EntryPointVerdicts` GLOBs in `MFO_GenerateESP.py`, read via
`ReadEntryPointVerdicts`→`g_verdicts`) or `ClassifyRank` (`:446`) silently changes which
saved perks survive a load (with an auto §17 refund). The verdicts are ESL DATA now, not a
DLL table — changing them is a generator+regen change, not a code edit.
**STYLE FACTS (2026-09-13, marth: "primary weapon, armor and wielding style are
determined by perks") — the classifier's one extension, still no verdict.**
`PerkEntryFact::style` (`Progression.h` `PerkStyleFacts`) records what an entry is
CONDITIONED on: the engine weapon/armor vocabulary keyword a `HasKeyword`/
`WornHasKeyword`/`WornApparelHasKeywordCount` item names (`WeapTypeSword` …
`WeapTypeBow`, `ArmorHeavy`/`ArmorLight`/`ArmorShield`, matched by KEYWORD EDITOR-ID,
positive tests only) and the `GetEquippedItemType(LEFT)` signatures (dual wield =
excludes 0, admits 1..4, admits nothing ≥5; shield = `== 10`). Read by
`ReadStyleFacts` (`:283`) off every entry-point TAB (`BGSEntryPointPerkEntry::conditions`,
bounded by `entryData.numArgs` AND the array size) and off every EFFECT of an ability
entry (`SpellItem::effects[i]->conditions`, the AbilityPlayerGated list) inside
`WalkPerkEntries` (`:766`); unioned per rank into `RankView::style` at build (`:580`).
`TallyStyleVotes` (`:886`, **MAIN THREAD ONLY** — walks the base's live perk array the
allocator reallocs on AddPerk/RemovePerk) sums, over the frozen catalog, the facts of
every OWNED rank 1..K per node (rank depth = investment) into `StyleVotes`. Perks
outside the catalog (hidden engine perks like PerkSkillBoosts, creature perks, dead
player-UI perks) never vote; a catalog rank conditioned on nothing classifiable votes
for nothing (`classified`/`owned` counters say how many). NO overhaul is assumed
anywhere — the facts come off the perk record's own conditions. **UNARMED (2026-09-14):**
`PerkStyleFacts::unarmed` = a list whose `GetEquippedItemType` test on EITHER hand admits
code 0 only (`== 0` / `<= 0` / `< 1`), no hand test on that list excludes 0, and no
weapon-kind keyword is named on it (per-list, not per-merged-entry); OR (second signal,
read in `WalkPerkEntries` beside the effect-condition read) an ability entry with an effect
whose `baseEffect->data.primaryAV == kUnarmedDamage` (35) — vanilla Fists of Steel
(`PERK 0x58F6E` → `SPEL 0x424E9` → `MGEF 0x10C4E6`, primaryAV 35 measured off Skyrim.esm)
conditions only on worn gauntlet keywords, never the hands, so without this the exception
could never fire on a vanilla-tree list. NO engine keyword
exists — Skyrim.esm ships no `WeapTypeHandToHand` KYWD (checked 2026-09-14; `Unarmed` is
the WEAP `0x1F4` edid). `StyleVotes::unarmed` is the ONE vote NOT counted off held perks:
`TallyStyleVotes` reads MFO's allocation record `ProgAllocator::g_prog[id].perks`
(enrolled records; ranks 1..K of each allocated node; main thread, no lock — where the
tally already runs) via a deliberate, commented component-1→component-2 include of
`ProgAllocator.h` in `Progression.cpp` (headers stay acyclic). Consumer:
`Logistics::ShedOffRoleWeapon` (fists rule). **OPEN BACKLOG: `Docs/REVIEW-BACKLOG.md`
MFO-B14** (SEV-4) — the per-list empty-hand rule also matches a "left hand empty"-only
one-hand perk (no keyword, no right-hand test); tightening shape recorded there.
**STYLE-FACT RANKS ARE NEVER MARGINAL (2026-09-14, `fix/mfo-deck-0914-helmet-offhand-verdict-meo`):**
at the rank build (`BuildSkill`, right after the `rank.style` merge) a rank with
`rank.style.Any()` whose `ClassifyRank` verdict is not `kEffective` is PROMOTED to
`kEffective` (`"style-facts"` appended to its `why`; one `spdlog::debug` line per
promotion; counted in `SkillTree::stylePromotedRanks` / `Catalog::stylePromotedRanks`,
printed in the `[prog] census` line — nonzero is the "stale/absent verdicts" tell).
Belt-and-braces: the field root cause was a stale deployed `MFO_Progression.esl`
with 0 verdicts (every rank `no-verdict` → marginal → the allocator never spent on
an equipment-deciding perk → zero style votes), redeployed separately; this makes the
perk-style decision survive a missing or overhaul-odd verdict. The 92-row verdict
table, `ReadEntryPointVerdicts`, `ClassifyRank` and the generator are untouched;
every rank WITHOUT a style fact still follows the declared verdict. Consumers
(allocator, board, census) unchanged — they read `verdict`. **OPEN BACKLOG:
`Docs/REVIEW-BACKLOG.md` MFO-B25** (SEV-5, marth's call) — a player-gated ability
conditioned on a weapon/armor keyword is promoted too, so a follower point can buy a
rank whose only follower effect is its style vote. **What breaks:**
renaming/renumbering `WeaponKind`/`ArmorKind` bits breaks `Logistics::WeaponKindOf` +
`TradeBridge::BuyThresholds::preferKinds` (same bit space); calling `TallyStyleVotes`
off the main thread races the allocator's perk writes (the Logistics mirror exists for
exactly this); with the catalog unbuilt (addon absent) every vote is 0 → the whole
perk-style decision is INERT and equipment behaves exactly as before.

### ProgAllocator.cpp / ProgAllocator_Hms.cpp / ProgAllocator_Manifest.cpp / ProgAllocator_internal.h / ProgAllocator.h — component 2: allocator + 'PRGN' owner  ⚠️ SAVE-LAYOUT
The engine-mutating half: writes perks (`AddPerk/RemovePerk`+`ApplyPerksFromBase`)
and skill AVs onto real actors, runs the level poll, owns 'PRGN'.
- **Module layout (mechanical split, 2026-08-31):** `ProgAllocator.cpp` (2354) =
  the allocation engine + 'PRGN' — skill reconcile (`ReconcileSkill:235`,
  `RecomputeSkills:366`), perk plumbing + `ReapplyFollower` (`:826`), the level
  poll (`PollWork:959`), the verbs, board views, dev harness, and the WHOLE
  co-save block (`CoSaveSave:1806`, `CoSaveLoad:1922`, `ClearAll:2258` — the
  serializers NEVER leave this TU, and the PRGN field order inside them is the
  save format). `ProgAllocator_Hms.cpp` (384) = §HMS — `HmsProfile` (`:37`),
  the F3 fired-pool mirror consumers, `HmsTrackBattle` (`:113`), `RecomputeHMS`
  (`:171`), `NoteCombatFire` (`:375`). `ProgAllocator_Manifest.cpp` (485) =
  the §18.6 manifest reader — economy GLOB discovery + `ApplyEconomyOverride`
  (`:149`), `ParseClassDef` (`:222`), `BuildGenericManifests` (`:277`), `Init`
  (`:347`), and the PRGN class-identity resolvers `DeriveClassIdentity` (`:446`)
  / `LookupAddonForm` (`:465`) called by the co-save. `ProgAllocator_internal.h`
  = the shared substrate (these 3 TUs only): `Economy`/`g_econ`/`g_econDefaults`,
  `g_ready`/`g_devCmd`, `g_classes`, `g_manifests`, `kSkillNames`, `kHmsAV`,
  `g_hmsFireMx`/`g_hmsFiredMask`, + the cross-module decls — every definition in
  it is `inline` (ODR: one shared instance, the `Logistics_internal.h` pattern).
- **SAVE-COMPAT — `CoSaveSave` / `CoSaveLoad` exact order** (grep the symbols; line
  numbers drift. Any field-order/type/version change corrupts live saves): header
  `{lastPlayerLevel u16, [v6: g_playerHmsTotalLast f32], count u32}`; per follower
  `{formID u32, flags u8 (bit0 enrolled…bit4 manualSkills v2, bit5 0x20 fixedStat v6),
  cls u8, progressionLevel u16, sharedGrowthRemainder u16, [v1-only unspentPerk
  f32 read+discarded], [v2: manualBaselineLevel u16, manualPointsApplied u16,
  manualExcludedLevels u16, nativeTreePerksAtEnroll u16], perkCount u16 +
  {nodePerkID u32, rank u8}×N, skillCount u16 + {av u32, points f32, lastWrittenBase
  f32, manualPoints f32(v2), **autoPoints f32(v7)**}×N, baseCount u16 + {av u32, value f32}×N,
  [v5 §HMS block APPENDED at END: per pool {H,M,S} order {hmsBaseline f32,
  **v5-ONLY hmsTarget f32 (dropped in v6 — recomputed)**, hmsSkew f32,
  hmsCumulative f32}, then battlesSinceLevelUp u32, battlesOffClass u32,
  offClassPool u8, hmsCaptured u8, **[v6: hmsZeroAwardStreak u8, hmsGrantRemainder
  f32×3, hmsAwardAccum f32]**], **[v7 APPENDED after the HMS block: autoLevelsGranted
  u16, freeRespec u8, strippedCount u16 + {perkFormID u32}×N]**}`. Bounds: 4096 followers / 1024 perks / 64 skills. New fields MUST go
  behind `if(version>=N)` (**v6 header + block additions gated `if(version>=6)`;
  v5 keeps the old 4-f32/pool reader, reads+discards target, recomputes it; all
  floats finite-guarded, streak clamped 0..2**). The HMS block is read
  UNCONDITIONALLY (even for a dropped follower) or the stream desyncs. The
  docstring above `CoSaveSave` is commentary — the code is authority.
- Version guard `Serialization.cpp:320` (newer PRGN skips this record only).
  `CoSaveSave` has no `g_ready` gate — writes even when the addon is disabled so the
  data survives a session without the ESL.
- Lifecycle: `Init` (`ProgAllocator_Manifest.cpp:347`) ← `plugin.cpp:287` (early-returns `g_ready=false` if
  `!Progression::Detected()`); `OnPostLoad` (`ProgAllocator.cpp:1235`) ← `plugin.cpp:363` (after the
  co-save load); `ClearAll` (`ProgAllocator.cpp:2258`) ← `Serialization.cpp:591` (clears `g_prog`,
  bumps `g_pollGen` to orphan in-flight polls).
- Verbs (all ← Board.cpp, `g_ready`-gated): `Enroll`/`SetClass`/`AllocatePerk`/
  `Respec`/`SetManualSkills`/`ApplyManualSkillPoint` (`ProgAllocator.cpp:1370-1750`).
- **STRICT POINTS (#81, 2026-09-13; A′/A″ PRGN v7).** A skill point, once placed
  by ANY path, stays on that skill until Respec. `SkillAlloc::manualPoints` has
  EXACTLY TWO writers — `ApplyManualSkillPoint` (`:1797`, +1) and `Respec`
  (`:1685`, → 0); `SkillAlloc::autoPoints` (v7) has EXACTLY TWO — the GRANT step of
  `RecomputeSkills` (`:354`, +=) and `Respec` (→ 0). `RecomputeSkills` is an
  ACCUMULATOR, not a re-derivation: `pending = (effAutoLvl − 1) − autoLevelsGranted`
  auto levels yield `skillPointsPerLevel` each, split by `WeightsFor` AT THAT
  MOMENT as the EXACT float share and ADDED to `autoPoints` (Fable F3: a per-level
  whole-point split starved the 10 % skill forever at the shipped 5 pts/level —
  2/2/1/0 every level; the exact share converges: 20 levels × 5 at 40/30/20/10 →
  40/30/20/10); the weights are not consulted when nothing is pending, so a class
  change / dominance flip only steers FUTURE levels. Then every entry is held at
  natural + `floor(autoPoints + 1e-3)` + manualPoints through the single
  `ReconcileSkill` (`:235`, baseline floor; REVERT drift-clobber unchanged) — the
  fraction stays in the ledger for the next grant to complete. `DominantWeaponSkill`
  (`:297`) / `DominantArmorSkill` (`:305`) read BASE SKILLS ONLY (the old loadout
  read re-homed the share every ~2 s — marth's "fluidly managed" drift). **A″
  Respec returns ALL points:** perks cleared as before, every `autoPoints` and
  `manualPoints` → 0, `autoLevelsGranted` → 0, `manualPointsApplied` → 0,
  `manualExcludedLevels` → 0, manual ON → `manualBaselineLevel = 1`; then one
  `RecomputeSkills` re-grants every level under AUTO by the current weights, or
  leaves the whole pool (`(level − 1) × manualSkillPtsPerLevel`) to the player under
  MANUAL. Rapport as before — EXCEPT the ONE FREE post-migration respec
  (`ProgState::freeRespec`, v7 u8: set ONLY by the `a_version < 7` reader because a
  v6 follower's applied values came from the old drifting split and A′ freezes
  them; `Respec` skips `Rapport::Spend`, logs `RESPEC <name> -- FREE (one-time,
  post-migration)`, clears the flag; a fresh v7 enrollee has none; unenroll/bench
  never touch it). `HasFreeRespec(id)` + `BoardFollowerView::freeRespec` expose it
  — the Board's respec footer + confirm popup (`Board_Progression.cpp:1193-1229`)
  read `who->freeRespec` from the SNAPSHOT row (render thread, never `g_prog`) and
  show "free (one time)" / "no rapport is lost" instead of the rapport cost;
  `HasFreeRespec` itself still has no caller (the snapshot bit is the consumer).
  No-op (no cost) only when nothing at all is placed.
  Every other path (`SetManualSkills:1758`, `SetClass:1539`, `PollWork:1029`,
  `ReapplyFollower`, HMS, the fixed-stat grant, `CoSaveLoad`) only READS the point
  fields. **What breaks:** a third writer of either field violates #81; the grant
  relies on every level being exactly one of auto / pooled / excluded (`effAutoLvl`)
  — change that partition and levels double-grant or vanish; `autoLevelsGranted`
  must never be lowered except by Respec; the v6 migration freezes
  `autoPoints = max(0, points − manualPoints)` — a cap-clamped v6 skill keeps its
  clamped value (never re-split), by design.
- **B′ NATIVE-PERK STRIP (2026-09-13, PRGN v7).** *A perk is stripped iff it is a
  rank form of a node in the progression CATALOG (`Progression::Get().skills[*].nodes`
  — the effective/marginal nodes of the 18 skill trees, which is exactly what the
  add-on's Board tab draws for EVERY class: `Board_Progression.cpp:340-356` lists all
  18 `kSkillNames` rows and opens a tree wherever the catalog has nodes; the class
  FLSTs of `ProgAllocator_Manifest.cpp` are WEIGHTS, not visibility), the follower's
  base TESNPC holds that form, and MFO did not itself grant that form (MFO holds
  exactly `ranks[alloc.rank−1]`).* Not per class. Non-catalog perks (hidden engine,
  creature, filtered player-UI, non-tree abilities) are never touched; an
  ACTOR-only rank (never on the base) is logged and left. `StripNativePerks`
  (`:~697`) runs at `Enroll` (`:1461`) and, for an unstripped enrolled follower (v6
  save / enrolled while benched), on the first `PollWork` poll it reads ACTIVE —
  BEFORE `ReapplyFollower`, re-arming it (`applied=false`) when anything was removed
  so a rank the reapply had deferred to native ownership (`:~918`) is re-added now
  the native is gone; that deferral is therefore UNREACHABLE for a stripped node
  (it can only fire for a native rank that lands on the base AFTER the strip within
  a session — `nativeHeld` stays true until the next load / re-check). **THE STRIP
  IS PER-SESSION BY NATURE (Fable F1):** base `AddPerk`/`RemovePerk` do not survive a
  load (P3, the reason `ReapplyFollower` exists), so every load puts the natives
  back; `nativeHeld` is RUNTIME-ONLY (never serialized) and `OnPostLoad` (`:1325`)
  re-arms it beside `applied`, so the first managed ACTIVE poll re-strips (union)
  BEFORE the reapply. **Never restored while
  enrolled — a benched/dismissed follower stays stripped** (marth: "a clean restore
  when it's uninstalled"), which rules out the native+MFO same-node double rank by
  construction. The record `ProgState::strippedPerks` (set, union on re-strip,
  unresolvable ids drop on load) is the v7 co-save field the safe-removal reads: public `ProgAllocator::RestoreNativePerks(actor)` (`:1754`,
  header contract in `ProgAllocator.h`) puts every recorded perk back on the base,
  settles, CLEARS the record — meant to run once per enrolled follower right before
  the mod is removed; **nothing calls it yet** (the Board/MCM uninstall verb is a
  separate brief). No refund: not credited, and `nativeTreePerksAtEnroll` (the §17
  debit) is recounted AFTER every strip as `CountNativeTreeRanks − AllocatedRanks`
  (native ranks STILL held — i.e. only unstrippable actor-only ranks — never MFO's
  own), which also corrects a pre-v7 follower's debit. Fixes marth's "engine gives
  rank 1, rank 2 inaccessible" bug: `GateNextRank` (`:764`) froze natively-owned
  nodes; stripped nodes are takeable. **What breaks:** stripping a form MFO granted
  (the `mfoForm` skip) would silently undo an allocation; restoring while enrolled
  re-creates the double-rank case; a caller of `RestoreNativePerks` that does not
  then stop MFO leaves the follower with natives AND grants.
- **UNENROLL = the T#78 per-follower MFO toggle unchecked (marth 2026-09-13:
  "unmanage, don't touch ... remember the state when dropped and restore it plus
  back debt when re-enrolled").** Before this change `ProgAllocator` never read
  `FollowerState::mfoEnabled` (the Board writes it at `Board.cpp:1203`, the
  Scheduler/Logistics ticks skip on it at `Scheduler.cpp:318` / `Logistics.cpp:808`)
  — the toggle preserved `ProgState` but did NOT stop progression's actor writes.
  Now `Followers::IsMfoEnabled(id)` (`Followers.cpp`, an **off-worker-safe read of
  the `g_mx`-guarded `g_mfoOff` mirror** that `PublishActiveMirror` republishes from
  `g_followers` on the worker at every `Refresh` — the `IsTrackedFast` road, #74;
  Fable F2 on 4a62688 rejected the first version's unlocked `g_followers.find`)
  gates: in `PollWork` (`:~1055`) the LEVEL LEDGER still advances (`progressionLevel`
  gain, shared-growth banking — that IS the back debt) but every actor-touching
  branch is skipped (`RecomputeSkills`/`RecomputeHMS` on level gain, the strip, the
  reapply, the drift-watch/HMS block), and the record is re-armed
  (`applied=false`, `nativeHeld=false`) so the first managed poll re-strips (union)
  and re-applies; every board verb (`Enroll`/`SetClass`/`AllocatePerk`/
  `AllocateNextEligible`/`Respec`/`SetManualSkills`/`ApplyManualSkillPoint`) refuses
  through `Unmanaged(actor, verb)` with a `[prog] … refused …: MFO is unchecked`
  line. **Nothing is cleared**: `ProgState` (allocations, autoPoints/manualPoints,
  ledgers, strippedPerks) is saved for an unmanaged follower exactly like a managed
  one (`CoSaveSave` writes every `enrolled` record — no `mfoEnabled` gate).
  **Back debt on re-check falls out of the accumulator:** `pending = (effAutoLvl−1)
  − autoLevelsGranted` is the levels gained while unchecked, granted by the weights
  of the re-check moment; the manual pool `(progressionLevel − baseline) × rate −
  applied` has grown by the same levels; perk points are `floor(level/N) − spent`
  (derived). **Rapport is untouched on both edges:** progression's only rapport
  write is `Rapport::Spend` in `Respec` (`ProgAllocator.cpp:1738`); the veteran
  level-match (`SetClass`) writes `progressionLevel` only. **What breaks:** gating
  the level ledger on `managed` would erase the back debt; touching the actor while
  unmanaged violates "don't touch"; `IsMfoEnabled` must keep reading the mirror,
  never `g_followers` (the SEV-1 cluster shape); a Board toggle is visible to the
  poll only after the next `Refresh` (one diag turn) — do not shorten that by
  reading the map.
- **Actor-write safety:** perk reapply is idempotent — re-adds a rank only if
  `GetPerkIndex` absent (`:841`) + native-ownership deferral (`:853`, if another mod
  granted a rank, MFO touches nothing). Skill writes funnel through the single
  `ReconcileSkill` (`:235`) with the enrollment baseline as a **hard floor** — a
  shrink can never write below the follower's captured natural. `IsKnownSkillAv`
  (`:68`) validates the raw AV value before Get/SetBaseActorValue (OOB guard).
  **Two skill models, one write site, live MCM toggle** `g_econ.cancelEngineAwards`
  (default ON, addon INI `bCancelEngineAwards` via `ApplyEconomyOverride` — no GLOB,
  no PRGN touch): ON = `natural = enrollmentBaseline` (REVERT — engine per-level/
  autocalc drift ignored + clobbered each ~2s drift-watch cycle, base = baseline +
  MFOaward, pure MFO, no inflation); OFF = the shipped `natural = (cur==lastWritten)
  ? lastWritten-points : cur` ADOPT path (engine leveling + MFO stack). Baseline
  uncaptured (old save) → ADOPT fallback. Both idempotent/replay-safe; toggle
  changes NO co-save layout (baseline already serialized).
- **§HMS class-redistribution (PRGN v5) — SIBLING of the skill reconcile.**
  `RecomputeHMS` (`ProgAllocator_Hms.cpp:171`; the whole §HMS engine lives in that module) is called at the same
  three sites as `RecomputeSkills`: the level-gain edge + the ~2s drift-watch in
  `PollWork`, and `ReapplyFollower`. **MEASURE-not-clobber** (the key difference
  from skills): it reads `cur=GetBaseActorValue(H/M/S)`, measures the POSITIVE
  drift off the held `hmsTarget` (= the engine's live per-modlist per-level award)
  BEFORE re-asserting, sums the 3 pool deltas SIGNED then clamps ≥0 (a starved pool
  reads negative under the engine's absolute-autocalc slam; signed telescopes to the
  fresh award). **v1.1:** the cur-read + signed-diff + clamp is the general
  `Followers::MeasureEngineVitalAward(actor, heldTarget, curOut, deltaOut)` primitive
  (Followers.h/.cpp), and the base-AV get/set go through `Followers::Get/SetFollowerHMS`
  (canonical pool order {0=H,1=M,2=S} == ProgAllocator `kHmsAV`). RecomputeHMS is
  byte-identical — it just calls the seed API. Then it redistributes by class profile
  (**v1.1 Phase 2: the STANCE that selects the profile now comes from the base Gambit
  class `Followers::GetBaseClass(actor)` = `FollowerState::combatClassOverride`, NOT
  `ClassDef::stance` — the latter is a GLOB editor-id suffix the engine discards at
  runtime, always 0, which wrongly skipped HMS. `FindClassDef(clsId)` stays as the
  enrollment/MFO-managed gate + skew/weights source; only the stance value moved. Same
  swap in `HmsTrackBattle`.**) — **v1.1 Phase 7: the ratio profile is now ADD-ON DATA,
  not a hardcoded switch. `HmsProfile(stance)` walks `g_classes` for the ClassDef whose
  `stance` matches and returns its normalized `hmsWeights[]`/`primaryPool` (no DLL default —
  no declared class/weights for that stance → false → HMS reshapes nothing). Byte-identical
  for the shipped add-on (Melee 60/5/35, Ranged 40/5/55, Mage 15/80/5 emitted raw, each
  sums to 100 so `weight/sum` reproduces the old float literals exactly).** The class
  ratios (0/none → skip) feed a usage-scaled **skew** (pulled
  from the class-primary pool toward the exercised off-class pool; ≥1-pt floor, but
  the **cap wins** — clamped to `Config::g_hmsSkewMaxFrac`×budget so a tiny budget
  never exceeds it), accumulates into `hmsCumulative`, then holds
  `hmsTarget=hmsBaseline+hmsCumulative` via one `SetBaseActorValue` per pool. Net
  follower total gain == the measured budget, reshaped. Fixed-stat follower →
  measured budget 0 → 0 redistribution (vanilla) — **BUT see the Phase-3 grant
  below.** Skew usage =
  `battlesOffClass/battlesSinceLevelUp`; battles counted in `HmsTrackBattle`
  (main-thread combat rising-edge, 3s dwell). **Off-class signal = the REAL fired
  gambit (F3):** the combat scheduler (WORKER) calls `ProgAllocator::NoteCombatFire`
  on every `Result::Fired` (`Scheduler.cpp` ~:886), mapping the action opcode →
  pool (cast→Magicka, melee attack/power→Health, bow attack/equip_ranged→Stamina)
  into a **mutex-guarded per-follower mirror** (`g_hmsFiredMask`, CombatStyle::g_owned
  pattern); `HmsTrackBattle` consumes the mask (cleared each rising edge), credits a
  battle off-class when an off-class pool fired. Deliberately NOT reading
  worker-written `Gambit.lastFired` off-thread (#4). Counters reset when an award is
  consumed (seed 1 if a battle is in progress). **Knobs live on the MAIN MFO MCM,
  not `g_econ`:** `Config::g_hmsRedistribute` (bHmsRedistribute, default ON, master
  switch — also gates `NoteCombatFire`) + `Config::g_hmsSkewMaxFrac` (fHmsSkewMax,
  MCM stores a PERCENT 0-100, scaled /100 in `Config::ReadFile`). Baseline captured
  at `Enroll` (`hmsCaptured=true`); a pre-v5 save (`hmsCaptured=false`) ADOPTS the
  live base on first `RecomputeHMS`. REQUIRED `[hms]` probe logs the measured award
  + budget + profile + skew + per-pool award + final targets. **v1.1 Phase 2 adds a
  once-per-call `[hms-diag]` line at RecomputeHMS entry** (`clsId`/`def`/`defStance`/
  `baseClass`/`earlyReturn`{none|nodef|noprofile|noavo}/`redistribute` budget
  /`fixedStat`/`grantBudget`) — confirms on-deck that a Gambit-Mage follower reads
  `baseClass=3` and reveals which ClassDef the `clsId` resolved to.
- **§HMS FIXED-STAT GRANT (PRGN v6, v1.1 Phase 3).** A fixed-stat NPC gets 0 engine
  award → 0 budget → never grows. Phase 3 gives it progression, gated by the SAME
  `Config::g_hmsRedistribute` master switch (no new MCM/Config). Three parts, all in
  `PollWork` + `RecomputeHMS`: **(1) DETECT** (active party only, on a player level-up):
  `RecomputeHMS` tallies each MEASURED engine award into `ProgState::hmsAwardAccum` (SERIALIZED
  in v6 — a save between two player level-ups must not wipe the award evidence);
  `PollWork` reads it at the next player level-up — 0 award ⇒ `++hmsZeroAwardStreak`,
  at **2** sets `fixedStat`; any award > 0 resets the streak AND clears `fixedStat`
  (a leveling follower). A single quiet level never flags. **(2) PLAYER RATE:** on a
  player level-up `PollWork` sums the player's base H/M/S → `playerTotalNow`,
  `playerGain = max(0, playerTotalNow − g_playerHmsTotalLast)` (LIVE, modlist-agnostic;
  first observation inits with gain 0). **(3) GATE + GRANT:** `fixedStat` follower with
  `playerTotalNow >= Σ hmsBaseline` (player caught up) ⇒ `RecomputeHMS(actor,st,log,grantBudget=playerGain)`
  — the injected budget feeds the SAME converging/skew allocation, distributed by
  `HmsProfile(GetBaseClass)`; the grant path carries fractions in `hmsGrantRemainder[3]`
  so a 15/80/5 split lands as WHOLE base-AV points over levels. While
  `playerTotalNow < Σ hmsBaseline` the follower stays FROZEN (budget 0). **RETROACTIVE
  = per-level going forward, NOT a lump backfill** (the gate compares CURRENT player
  total vs baseline, so a high-level player opens the gate immediately). A non-fixed-stat
  follower passes `grantBudget=0` → byte-identical to Phase 2. `[hms] … fixed-stat grant:
  streak/caughtUp/npcTotal/playerTotal/playerGain/budget` logs the decision.
- **`Class` enum ordinals (`:84`) MUST stay == `combatClassOverride`** — the base
  class (`combatClassOverride`) is the STANCE AUTHORITY, set by the user via the Gambit
  tab (Board `SetClassOverride`). **v1.1 Phase 2: progression-tab `SetClass` NO LONGER
  mirrors into it** — the old `Followers::SetBaseClass(id, def->stance)` write only ever
  wrote 0 (the discarded GLOB suffix) and clobbered the user's Gambit pick with Auto;
  removed. `SetClass` now sets the SKILL class (`clsId`) only. Reads of
  `combatClassOverride` on the serial-worker path (Scheduler/Actuation stance) — and now
  HMS (`RecomputeHMS`/`HmsTrackBattle`) — go through `Followers::GetBaseClass`. Board snapshot is the
  one cross-thread structure (guarded `g_viewMx`); `Rapport::Spend` (`:1346`) is a
  cross-module write on respec.

### ProgProbe.cpp / ProgProbe.h — throwaway field probe (NOT serialized)
Dev-only (`bProgProbe`, INI, default OFF) log probe. `OnPostLoad` (`:444`) ←
`plugin.cpp:362`; `OnHotkey` (`:436`) ← `Board.cpp:640`. Writes no save record;
its perk/AV mutations are runtime-only. Safe to delete without touching saves; only
`plugin.cpp:362` + `Board.cpp:640` reference it. Idempotent reapply guarded on
`GetPerkIndex` (`:468`).

---

## 6. Board / UI / Papyrus — `Board.*`, `Papyrus.*`

### Board.cpp / Board_FieldKit.cpp / Board_Progression.cpp / Board_internal.h / Board.h — the Field Kit overlay
Hooks the **runtime D3D11 swapchain vtable** (no game offsets) + an input sink,
draws live state via ImGui on the **render thread** from a mutex-guarded snapshot,
funnels all rule edits through a main-thread-drained edit queue. **ImGui/
`imgui_impl_win32` = vendored, do not read.**
- **Overlay mechanism (`Board.cpp:337-1000`):** RENDER is offset-free — `PresentThunk`
  (`:610`)/`ResizeBuffersThunk` swapped into **IDXGISwapChain vtable slots 8/13**
  (frozen COM/DXGI ABI → version-independent; `HookSwapchainVtable` `:689`), the
  unchanged `WndProcHook` swap (`:313`, WM_CHAR/WM_KILLFOCUS), and `LazyInit`
  (`:401`, ImGui context + DX11/Win32 backend on first Present). `TryInstallHooks`
  (`:711`) polls for the live swapchain then patches.
- **INPUT: TWO MUTUALLY EXCLUSIVE CARRIERS over ONE translation body (v2.0.4).**
  The translation is `InputSink::Feed` (`:751`, hotkeys → close grace → ImGui feed);
  it returns TRUE when the board has taken the batch. `g_inputTrampoline` (`:92`)
  says which carrier is live, and NOTHING may drive both:
  * **1.6.1170 AND 1.5.97 — `InputDispatchHook` (`:987`), the v1.1.4 call-site
    trampoline, RESTORED.** `write_call<5>` at `REL::RelocationID(67315, 68617)`
    +`0x7B` (`BSInputDeviceManager::PollInputDevices`, AE `0x140CD8F40` from id
    `68617`, SE `0x140C150B0` from id `67315`), installed by
    `InstallInputHook` (`:1581`) from `plugin.cpp` at **PLUGIN LOAD** — it rewrites
    five live bytes in a function that runs on the input thread, and at plugin load
    that thread does not exist yet. It calls `Feed` and, on TRUE, **nulls the
    caller's head pointer** so the engine's own sinks never see the batch. THE
    TRAMPOLINE FEEDS THE BOARD here: `InputSink` is NOT registered and
    `SyncControlBlock` early-returns to a no-op.
    **TWO SEPARATE GATED BRANCHES, EACH LICENSED BY ITS OWN DISASSEMBLY** (CLAUDE.md
    principle 11) — the offset is `0x7B` on both because the two functions really
    are laid out identically, NOT because one measurement was assumed to cover both.
    SE 1.5.97 evidence (2026-09-09, `SE.unpacked.exe` + `version-1-5-97-0.bin`):
    the body is instruction-for-instruction the AE body, its four calls sit at the
    same `+0x53 / +0x5B / +0x7B / +0x87`, and the first two land on `0x140C11600`
    and `0x140C10860` — the exact addresses the pinned CommonLib source comment
    (`BSInputDeviceManager.cpp:147-150`) names for the ControlMap mapping pass and
    `Rumble::Update`, which independently confirms both the id resolution and the
    function. `+0x7B` = `e8 d0 0c 00 00`, a whole 5-byte `E8 rel32` (previous
    instruction `48 8b ce` ends at `+0x7B`, next starts at `+0x80`); `rcx` = the
    manager, `rdx` = `lea [rsp+0x40]`, a caller STACK SLOT, so the null is scoped
    to the dispatch. Full derivation in the `InstallInputHook` comment.
  * **every other runtime — `InputSink::ProcessEvent` (`:963`)**, a
    `BSTEventSink<InputEvent*>` on `BSInputDeviceManager` registered by `Install`.
    THE SINK FEEDS THE BOARD here; it cannot consume, so consumption falls to
    `SyncControlBlock` (`:485`, ControlMap toggle, edge-driven from Present) and is
    **only as complete as the control flags are**. That is a known hole, not a fix:
    5c0957c removed the trampoline for offset fragility and called the sink +
    ControlMap "behavior-identical", which was FALSE and cost three regressions —
    the SE-layout `ToggleControls` crash (v2.0.2), a partial category block
    (v2.0.3), and Favorites still opening on d-pad up with the board up. 1.5.97 left
    this path in **v2.0.6** once its `67315` was disassembled; **1.7.x still keeps
    it** until someone disassembles that runtime's `67315`/`68617` and verifies the
    in-function offset. **DO NOT widen the version gate on a guess.**
- **`SyncControlBlock` (`:485`) — when it does run, that toggle MUST call the ENGINE's `ToggleControls`
  (`REL::RelocationID(67245, 68545)` — SE `0xC11C60`, AE `0xCD5650`, both verified
  against the disassembly), NEVER `RE::ControlMap::ToggleControls`** — see the
  ABI note at `Board.cpp:524-590`. The pinned 3.7.0 header is an SE-17-context
  layout whose inline reimplementation writes `+0x118`, which on 1.6.1130+ (18
  contexts) is `contextPriorityStack.size`, not `enabledControls`; that corrupted
  the context stack on every board open/close and crashed the engine's per-frame
  user-event mapping pass (AE id 68542) at `SkyrimSE.exe+0CD509D`, plus a latent
  OOB write on the next `PushInputContext`. Field-reproduced 2026-09-08/09, fixed
  2026-09-09. **Anything reading a `ControlMap` member PAST
  `controlMap[]` (`0x60`) through the 3.7.0 header has this same defect.**
  `ShoutKey` (`:139`, `GetMappedKey` on `kGameplay`) is SAFE: `controlMap[]` at
  `0x60` and the `InputContext` stride `0x18` are identical on both runtimes.
  **PORTABLE UNIT
  for MAO/MEO:** those functions + `InputDispatchHook`/`WriteThunkCall`/
  `InstallInputHook` + `Install`; only the two `Draw*` calls in
  `PresentThunk` and the hotkeys in `InputSink::Feed` are mod-specific. Greppable
  `[overlay-probe]` log lines report every component + a SUMMARY on board close.
- **Module layout (mechanical splits, 2026-08-31 + 2026-09-07):** THREE draw/host
  TUs over one shared substrate header. Board.cpp had grown to 2577 at the
  overlay-swapchain merge (924f3a9) and the panel was cut out of it.
  * `Board.cpp` (1665) = **shell + the overlay host**: file-local render state and
    the overlay-probe atomics (`:86`) incl. `g_inputTrampoline` (`:92`), input
    translation (`:120`), `CloseBoard`
    (`:183` — see the anon-namespace note below), `SpellTooltip` (`:207`), `DrawHud`
    (`:226`), `WndProcHook` (`:313`) + the whole overlay hook section (`:337-1000`),
    then the public API: `ToggleHud` (`:1008`), `Toggle` (`:1015`), `FillRuleViews`
    (`:1032`), `ApplyEdits` (`:1081`), `PublishSnapshot` (`:1316`),
    `InstallInputHook` (`:1581`), `Install` (`:1622`, end).
  * `Board_FieldKit.cpp` (1135) = **the whole panel**, ONE public function
    `DrawFieldKit` (`:151`, Followers+Gambits tabs, the list-picker, cascaded-B
    close) plus the four helpers it is the SOLE caller of, in its own anonymous
    namespace: `kClassNames` (`:46`), the picker-submenu predicates `IsFoeCond`/
    `IsPotionLootAct`/`IsMiscLootAct` (`:56`), `PushSkin` (`:73`),
    `DrawSpellHoverTooltip` (`:135`). Future panel work lands here, NOT in Board.cpp.
  * `Board_Progression.cpp` (1248) = the hosted progression tab body, ONE function
    `DrawProgressionTab` — called from `DrawFieldKit` (`Board_FieldKit.cpp:1062`);
    future progression-tab work lands here.
  * `Board_internal.h` (305) = the shared substrate (Board TUs only):
    `EditKind` (`:31`)/`AddonVerb` (`:48`)/`EditCmd` (`:50`), the edit queue
    `g_editMx`/`g_edits`/`QueueEdit` (`:69`), `g_fontHead` (`:73`), `MenuSkin`/
    `kSkins` (`:79`), the four cross-TU state atomics `g_ioMx`/`g_open`/
    `g_wantClose`/`g_justOpened` (`:117`), **the editor vocabulary** `ParamKind`/
    `VocabEntry`/`kCondsCombat` (`:157`)/`kActsCombat` (`:205`)/`kCondsLogi`
    (`:221`)/`kActsLogi` (`:241`) + `cycleIdx`/`labelFor`/`kindFor` (`:268`), and
    the decls for `CloseBoard` (`:287`), `DrawFieldKit` (`:292`),
    `DrawProgressionTab` (`:300`) — every definition `inline` (ODR).
- **What breaks if you move a symbol between these TUs:** anything Board.cpp keeps
  in its anonymous namespace is INVISIBLE to the other two — the snapshot
  (`g_snapMx`/`g_snapshot`), the overlay-probe atomics, the D3D handles, `g_ready`/
  `g_hud`/`g_closeGrace`, `SpellTooltip`, `DrawHud` and every hook symbol are
  deliberately file-local. Needing one from a draw TU means promoting it to
  `Board_internal.h` as `inline` (one instance), not re-declaring it. **Board.cpp's
  anonymous namespace PAUSES around `CloseBoard` (`:149-178`) and resumes after it**
  so `CloseBoard` has the external linkage `Board_FieldKit.cpp`'s [B]/Esc path
  needs while the probe atomics it logs stay file-local — do not "tidy" that pause
  away. The vocabulary tables are `inline constexpr` in the header because BOTH the
  panel (which renders them) and Board.cpp's `ApplyEdits`/`FillRuleViews` (which
  resolve an opcode back to a label/ParamKind) scan them; their opcode strings are
  a FROZEN co-save contract (#10).
- **TWO install entry points, at DIFFERENT times, and neither may move:**
  * `InstallInputHook()` (`Board.h`, body `Board.cpp:1581`) — caller `plugin.cpp`
    inside `SKSEPluginLoad`, **PLUGIN LOAD ONLY**. Writes the `68617`/`67315`+`0x7B`
    call-site trampoline. Must run before the input thread exists (it patches five
    live bytes inside that thread's own poll function). Refuses VR, then gates on
    `REL::Module::get().version()` major/minor/patch == 1/6/1170 **or** == 1/5/97
    (two separate branches, each licensed by its own disassembly); every other
    runtime is a logged no-op. This is the ONLY `SKSE::AllocTrampoline` in MFO
    (`AllocTrampoline(64)`) — do not add a second without merging the reservations.
  * `Install()` (`Board.h`, body `Board.cpp:1622`) — caller `plugin.cpp:306`
    (kDataLoaded) only, VR-refused. Installs AFTER the renderer is up (the vtable
    path needs the swapchain LIVE and polls for it). Registers `InputSink` **only
    when `g_inputTrampoline` is false**.
  The other offset pair in this family is `SyncControlBlock`'s
  `REL::RelocationID(67245, 68545)` engine `ToggleControls` call
  (`Board.cpp:485-596`) — a plain call, not a patch, never reached on VR because
  `Install()` refuses VR first, and never reached at all on the trampoline path.
- **Snapshot carries all actor-derived display data** (render thread reads plain
  cached values, never a live actor — #4): `FollowerRow` (`Board.h`) holds vitals as
  pct **and** raw `health/magicka/staminaCur/Max` (Followers tab, `Vocab::VitalCur/
  VitalMax`), and `knownSpells`/`teachableSpells` are `SpellPick`/`Teachable` structs
  carrying precomputed `magickaCost` (`spell->CalculateMagickaCost(follower)`, actor
  overload) + a synthesized `tooltip` (`SpellTooltip`, effect name+mag/dur/area) —
  all filled in `PublishSnapshot` (main). The gambit spell-picker renders the hover
  tooltip via `DrawSpellHoverTooltip` from those cached values.
- **`DrawHud`'s `[C][L][T]` strip (`:254`, marth 2026-09-06)** — persistent
  bracket SLOTS: the bracket is always drawn (`TextDisabled("[ ]")` when idle),
  only the letter+colour comes and goes, so the strip's width never shifts.
  `r.looting` is fed by `Logistics::JustLooted`, not `IsLooting` — see the
  Logistics-family entry's "[L] GLYPH RELIABILITY FIX" for why.
- **T#78 Followers-tab MFO toggle** — the tab's FIRST column is a per-row checkbox
  bound to `FollowerRow.mfoEnabled` (mirrored from `FollowerState::mfoEnabled` in
  `PublishSnapshot`, both the active + retained builders). The `##followers` table
  is now 8 columns. On toggle it `QueueEdit`s `EditKind::SetMfoEnabled` (param 0/1);
  `ApplyEdits` flips `it->second.mfoEnabled`. The release-on-disable runs on the
  Scheduler tick's OFF edge, not in `ApplyEdits`.
- **Thread discipline:** `DXGIPresentHook` (render thread) copies `g_snapshot` under
  `g_snapMx` **before** taking `g_ioMx` (`:550`); reversing = render-thread deadlock.
  The two mutexes are never nested (#6). Draw functions **never touch `g_followers`**
  — every mutation is a `QueueEdit` (`Board_internal.h:69`, sites in both draw TUs)
  drained by `ApplyEdits`
  (`:953`). **Rule edits key on `Gambit.uid`, not row index** (`:1082` — resolve by
  identity #31); applying by index misapplies a command to the wrong rule.
- **Correction to the header:** `PublishSnapshot` (`Board.h:116` says "MAIN THREAD
  ONLY") actually drains on the **task worker**, the same context that
  owns `g_followers`/`Scheduler::Tick` — so its `g_followers` reads are safe *there*.
  But addon edit verbs ride `MainThread::Post` (`:910`) because `g_prog` lives on
  the real main/poll thread. Callers: `Diagnostics.cpp:216,263`.
- `ClearPendingEdits` (`Board.h:124`) ← `Serialization.cpp:603` (revert) — drops
  queued edits so a command from the old save can't hit a freshly loaded one.
  `SetHud` ← `plugin.cpp:365`, `Diagnostics.cpp:94`, `Serialization.cpp:621`.
  `IsOpen`/`IsAvailable`/`Toggle` ← Diagnostics (publish cadence + Field Orders
  power). `ToggleHud` (`Board.cpp:809`) is **dead** (no caller).

### Papyrus.cpp / Papyrus.h — outbound VM dispatch shim
Reaches Papyrus-only natives by class-name+method-name string, async fire-and-forget.
Registers NO natives (that's TradeBridge). The **three method-name strings are the
script-compat surface:** `"Actor"/"DoCombatSpellApply"` (`:70`), `"ObjectReference"/
"Activate"` (`:104`), `"MFO_Trade"/"RunTrade"` (`:126`) — changing the `.psc` native's
name/arity breaks dispatch silently (bumps `g_failures`). `DoCombatSpellApply` (`:41`)
← `Actuation.cpp:1496-1497` (gated on `g_commandCast`+`Available()`). `DispatchTradeRun`
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
sinks or moves never flush). `QueueGemMove`/`WornUid`/`Available` ←
`Logistics_Loot.cpp` (`AcquireEquip:575`) + `Logistics_Loot_Equipment.cpp`
(`LootEquipment:221`), worker tick. `PreviewWithGems` (`:105`, main-thread queries) has no
live caller (**UNVERIFIED** — check Board before removing). `g_pending` keys on
`(followerFormID<<32|toBase)`; if `ClearTransientState` (`:100` ← `Serialization.cpp:
616`) stops being called on revert, a reused FormID next session moves gems onto the
wrong actor.
**GEM RECONCILE (ABI v3)** — `GemReconcileSupported`/`RequestGemReconcile` (MEOBridge.h)
← `Logistics::ServiceFollower` idle branch (`Logistics.cpp` ~`:1325`, each management
scan). Decoupled re-socket of a follower's OWN loose gems (left loose by the
ungem-then-sell `UnsocketItemGems`) back into his WORN gear's empty sockets, so a gem
extracted for a sale never stays loose. `RequestGemReconcile` posts the whole pass to
the main thread (`ReconcileLooseGems`, anon) — GetLooseGems/GetEmptySocketCount/
GetGemDetails are main-thread-only; SocketGem/UnsocketGem queue to main. The old
"no dedup state — async ops land within a frame or two, so the next pass never
re-issues" assumption held for a request MEO accepts and LANDS; the deck log
(2026-09-14) showed one identical socket request re-issued 192× at the ~1.2 s cadence
— MEO accepted it (`SocketGem` true means "queued", `MEO_API.h`) and then failed in
its OWN log. **RETURN LOGGING + STALL DETECTOR (2026-09-14,
`fix/mfo-deck-0914-helmet-offhand-verdict-meo`):** every `SocketGem` / `UnsocketGem` /
`MoveGems` bool is logged (`(queued)` on true, `REFUSED by MEO ... -- see MEO.log`
warn on false — the `EquipSink` move, `UnsocketItemGems`, both reconcile sites).
`ReconcileLooseGems` keeps a MAIN-THREAD-ONLY per-actor map `g_stuck` (anon, no
lock — same domain as the pass; VR runs the pass inline off the worker, which nothing
else shares) keyed `StuckKey{op, base, uid, slot, gemBase}` (op 0 = `SocketGem`, op 1 = the tier-2
swap-out `UnsocketGem`, gemBase 0 there — `GemDetail` carries none) →
`StuckState{emptyAtIssue, passes, reported, backoffUntil, inventoryKey}`, applied by ONE
lambda `stallGate` at BOTH issue sites: a request seen `kStuckPasses` (3) consecutive
passes with `GetEmptySocketCount` unMOVED (a socket drops it, an unsocket raises it) —
i.e. 2 accepted issues, ~2.4 s, STUCK declared on the 3rd pass BEFORE issuing — logs
`[meo] reconcile STUCK <actor> <item>/<uid> slot <n> gem <base> -- <api> accepted 2
time(s) ... -- see MEO.log [api] <api>` ONCE and backs that key off. A backed-off
socket NEITHER RESERVES NOR SHADOWS its gem (Fable SEV-4 on `b3ac577` + round-2 SEV-2
on `5f814d5`): the gem is not taken from the pass-local `avail` (a later worn item may
take it), the slot marks it `excluded` for THIS item and RE-PICKS (`pickGem` lambda)
so the next-best gem fills the slot, and the tier-2 swap-up skips any loose gem that is
`excluded` or whose socket key on this item is still backed off (`socketBackedOff`) —
without that the swap-up unsocketed a worn gem to make room for the refused one, our
own unsocket changed the inventory fingerprint, lifted the back-off, and the item
cycled unsocket/re-socket every ~5 s forever. The back-off holds until the follower's
loose-gem/worn fingerprint changes or `kStuckBackoff` (60 s — principle 9: sized from
the ~1.2 s cadence, ~50 passes, never silently forever); then it retries from a clean
count and reports again if it stalls again. Keys not re-issued
in a pass are forgotten at its end EXCEPT keys still in back-off (the swap-up's
`socketBackedOff` read needs them when the item has no empty slot to gate on; they
expire at their own `backoffUntil`); the `nLoose == 0` early return drops the actor's
keys; `ClearTransientState` clears the map (revert/load, `Serialization.cpp`). NOT a
mask: the failure is loud and retried. Domain is matched here so MEO never rejects
into a retry loop. Tier 1 conservation always runs (fill any domain-matching gem);
tier 2 effect-aware (MCM `bMeoAwareGems`, default OFF) ranks by a class-preference
heuristic on gid/name + magnitude and swaps up a socketed gem a better loose gem
beats. Whole feature no-ops below MEO v3. **What breaks:** `g_stuck` is written
without `g_mx` on purpose (a lock held across `g_meo->` calls is the #4 re-entrant
deadlock class) — never touch it from the worker while the pump is live; lowering
`kStuckPasses` below 2 turns every first issue into a STUCK line; a `MEO_API.h`
change is off the table (byte-shared, append-only); the swap-up MUST keep skipping
backed-off gems or the unsocket/re-socket loop returns. **OPEN BACKLOG:
`Docs/REVIEW-BACKLOG.md` MFO-B26** (SEV-5) — the back-off lifts on ANY inventory
change, so under churn the one warn per 60 s becomes one per change + 3 passes;
**MFO-B27** (SEV-5) — progress is the per-ITEM empty count, so a sibling slot
landing delays a stuck key's detection (never spurious).
**THE INVARIANT (marth 2026-09-14, `fix/mfo-meo-no-loose-gems`): "there should never be
unequipped gems when there are free spaces on equipped items."** After a
`ReconcileLooseGems` pass no loose gem may remain while any WORN item has an empty
socket MEO would accept it in. Three things make that true and one thing says so when
it is not:
- **THE WORN SET = MEO's OWN eligibility, mirrored** (`MeoSocketableArmor` /
  `MeoSocketableWeapon`, `kMeoArmorSlots`, anon, above `ReconcileLooseGems`). MEO's API
  does NOT gate on eligibility: `SocketCapacity` (MEO `plugin.cpp:1628`) returns 1 for
  ANY armor or weapon, so `GetEmptySocketCount` reported an empty socket on a
  base-enchanted ring or a bound sword and the pass issued into it (STUCK loop, or a
  slot MEO's menu cannot see — MEO's shield story, `plugin.cpp:2315-2319`). The set is
  now exactly MEO's `IsSocketableArmorBase` (`plugin.cpp:2300-2326`): worn in one of
  **kHead, kHair, kBody, kHands, kAmulet, kRing, kCirclet, kFeet, kShield** (kForearms /
  kCalves were enumerated before and are NOT in MEO's list — dropped), not
  base-enchanted (`formEnchanting`), `GetPlayable`, no `MagicDisallowEnchanting`; plus
  the two hands via `IsSocketableWeaponBase` (`:2270-2298`): the same verdicts plus not
  unarmed / bound / nameless / a tool (MEO's name blocklist). **An item is a worn
  INSTANCE = (base, worn xList)**, not a base: an identical dual-wield pair is two
  instances with their own sockets, so a weapon is looked up by its HAND
  (`WornXList(actor, base, hand)`: 0 = kWorn, 1 = kWornLeft, -1 = either for armor)
  and the dedupe is on (base, xList) — Fable SEV-4 on `1efa3e3`: deduping by base hid
  the second dagger (uid-0 pair: deferred forever as "duplicate-copy"; one minted: the
  other's empty socket invisible and NO LEFTOVER line). A weapon with no worn xList
  for its hand is skipped (MEO's uid-0 mint wants a worn xList too, `:9143`). The
  duplicate-copy deferral counts only UNWORN copies (`invBaseCount > wornCopies`), and
  a uid-0 base takes ONE request per pass (`mintedBases`; the other instance is
  `mintWait`, classified `minting`). A worn instance whose xList carries a FOREIGN
  enchant (kEnchantment with no MEO socket record — uid 0, or `GetGemDetails` == 0)
  is skipped in the item loop: `ApiSocketGem` refuses it (`:9258`) and at uid 0 MEO's
  mint path falls through to a DIFFERENT copy.
- **THE DOMAIN RULE = `GemFits(gem, ItemFit{capacity,isArmor,hasSupport,hasConduit})`**
  (replaces `DomainFits`): weapon gems → weapons, armor gems → armor, EXCEPT an item
  holding a socketed Conduit (GemDetail gid `conduit`) admits off-domain gems
  (`ApiSocketGem :9277-9279`); a SUPPORT gem fits any item that is DUAL-socket
  (`:9241`, and `MEO_API.h:112` says "dual-socket" too) and holds no other support gem
  (`:9272`), counting one issued THIS pass (`rec.fitNow.hasSupport`, and
  `.hasConduit` for a queued Conduit — MEO runs its queue in order). Before, support
  "fit any" — a Focus picked for a single-socket item was accepted, failed in MEO.log
  and went STUCK 60 s at a time. **The tier-2 swap-up candidate must fit WITHOUT the
  evictee** (`sansEvictee`: a support evictee clears `hasSupport`, a Conduit evictee
  clears `hasConduit`; and the swap-up's `hasConduit` comes from LANDED state,
  `fitAtStart`, never from a Conduit merely queued this pass — Fable SEV-4 on
  `9dacc0e`: a refused queued Conduit would otherwise have licensed an eviction for
  nothing, and our own fingerprint change lifts the back-off, so it would churn) —
  Fable SEV-2 on `1efa3e3`, confirmed by trace: with the
  candidate judged against the item AS-IS, an off-domain gem admitted through the
  Conduit out-scored the Conduit itself, evicted it, was no longer admitted next pass,
  the Conduit re-socketed, and the pair cycled every ~2.4 s with the empty count moving
  each time — a loop the stall detector cannot see (progress on the socket key, the
  unsocket key forgotten every other pass).
- **CAPACITY ACCOUNTING:** `avail[pick]` is decremented ONLY when `SocketGem` returned
  true. That false branch is a TRIPWIRE, not a live path: today's MEO returns false
  only for a null actor (`plugin.cpp:9416-9427`), which the pass excludes — every real
  refusal happens inside MEO's queued task. So the predicate mirror + `GemFits` are the
  ONLY defence against a refused request and the STUCK detector the only detector of
  one.
- **THE LEFTOVER LINE** (end of pass, `g_leftover`, anon, MAIN THREAD like `g_stuck`,
  cleared by `ClearTransientState` and the `nLoose == 0` return): for every loose gem
  still in stock while any considered worn item still has `emptyAtStart - issued > 0`,
  ONE `[meo] reconcile LEFTOVER <actor> <gemBase> '<gem>' x<n> -- <why>` per
  (actor, gemBase, why), rate-limited like STUCK (again after `kStuckBackoff` 60 s or
  on the loose/worn fingerprint change; keys not seen in a pass are forgotten).
  `why` ∈ **off-domain** (no worn item of its domain has an empty socket), **stuck**
  (every compatible empty socket is `excluded`/`socketBackedOff` for it), **capacity**
  (a compatible socket was open at the top of the pass but other gems filled it),
  **refused** (the tripwire above fired), **duplicate-copy** (its only compatible
  empty sockets are on a uid-0 item deferred for a carried UNWORN second copy — can
  persist with economy off), **minting** (a uid-0 item took its ONE first-mint gem
  this pass, or is the other instance of a base that did; the rest fill next pass —
  the one by-design class, logged at INFO, the rest WARN — **MFO-B33** records the
  open call on whether off-domain / duplicate-copy should be INFO too),
  **support-limit** (a support gem: a dual-socket item is open but already holds, or
  was just given, its one support seat — was misreported as `capacity`),
  **unclassified** (a compatible, un-deferred socket the pass walked past — a bug,
  never dropped). A copy a swap-out was issued FOR this pass (`swapPending`, a
  per-entry COUNT) is not leftover — its socket opens next pass — but the rest of a
  stack still is (`leftN = avail - pending`, the `x<n>` printed). Open notes:
  **MFO-B35** (the duplicate-copy deferral is redundant against MEO's in-place mint
  and starves a worn item while a spare is carried) and **MFO-B36** (the swap-up never
  decrements `avail[loot]`, so two items can evict for one loose copy — one wasted
  unsocket/re-socket, no loop). When no considered item has an empty socket
  NOTHING is logged: that is the invariant holding. Classification relies on the slot
  loop's exhaustiveness: it re-picks until `pickGem` returns -1, so on a non-deferred
  item with sockets left every fitting gem in stock is in `excluded`; the deferral
  flags are tested BEFORE `excluded` because a deferred item never ran its slot loop
  (`rec.considered` is set only past the dup deferral).
**What breaks (2026-09-14 additions):** `kMeoArmorSlots` / the two predicates must
track MEO's `IsSocketable*Base` — a slot MEO adds is a slot MFO silently never fills
(the LEFTOVER line then says off-domain for a gem that DOES fit); a slot MFO adds that
MEO's menu cannot see socket-stamps a gem the player cannot recover. `GemFits` must
stay the conservative side of `ApiSocketGem` (never looser) or the STUCK loop returns.
**MFO-B34** (policy, open): should the swap-up ever evict a Conduit that an
already-socketed off-domain gem depends on? Today it may, if a same-domain candidate
strictly out-scores the Conduit's bonus. The swap-up MUST keep judging its candidate
against `sansEvictee`, never `fitNow`, or the Conduit loop returns. Weapons MUST stay
enumerated per hand xList or the second instance of a pair goes dark again.
`rec.fitNow.hasSupport` must be set on a queued support issue or a second support gem
is issued to the same item and refused. The LEFTOVER classifier's `hole → unclassified`
branch is the tripwire for a pick/gate change that leaves a fitting gem unwalked.
**GEM CAPTURE RULE (Fable SEV-2 on `b3ac577`, fixed):** `AcquireEquip`
(`Logistics_Loot.cpp`) captures the worn same-role item's gems ONLY when the new item
REPLACES it — `a_forceStock` (mage backup, the dual wielder's second one-hander) skips
the capture and queues NO move; before the fix a stocked second 1H captured the
primary's uid and Actuation's left-hand equip at the next combat fired
`MoveGems(primary → second)`, migrating the gems to a weapon that is un-worn at combat
end (overflow to the PLAYER's pouch). A future stock path must keep `a_forceStock`
true or re-open this.

### APMFBridge.cpp / APMFBridge.h — optional APMF client (MODERATOR model, Phase 3)
Makes MFO a client of the SEPARATE APMF.dll (AI Package Management Framework) via the
byte-shared `APMF_API.h` (C-ABI POD; append-only, mirror APMF's copy). `Acquire()` ←
`plugin.cpp:289` (kDataLoaded): `GetModuleHandleA("APMF.dll")` + `GetProcAddress(
"APMF_GetInterface")` → `fn(kABIVersion)` → cast up to `APMF_API_v2*`; **null-degrades** with one
log line if APMF is absent/old — MFO then runs the legacy cast hybrid, byte-identical.
- **THE MODEL: APMF ARBITRATES/DRIVES; MFO EXECUTES the rest.** APMF never generates behaviour on its
  own initiative (its channels are client-declared, arbitration/drive-only). This bridge only ever
  CLAIMS facets (`ClaimOffenseCast`/`ClaimCombatTarget` → `RequestCast`/`RequestEx`/`Repoint`, basis
  200); MFO executes the equip/target/consent half with its OWN mechanisms.
- **EQUIP AUTHORITY (ch.17, `kIntent_EquipAuthority`, ABI v8 `SetEquipSetEx` + ABI v9
  `SetEquipScope`; `feat/mfo-equip-authority` 2026-09-15 — PORT #1, the first MFO engine
  mechanism moved into APMF; SCOPED by `feat/mfo-equip-authority-v9` 2026-09-16).** `APMF_API.h`
  copied byte-for-byte from APMF `main` `9226f77`, the merged v9 (md5
  `2b941cd753d2e5520a0cdf558adf66f8`; the first v9 cut `3d5cab8` was
  `6494513d598d4a3bb768579a3e46dc51`; the v8 copy was main `03f04d5`, md5
  `aade60882647106ad3c928ada46ee2f2`); `kABIVersion` 8→9, so `Acquire()` now asks an older APMF
  for v9 and gets nullptr → the WHOLE APMF path degrades (the DLL pair ships together).
  `EquipAuthoritySupported()` = `Available() && abiVersion >= 9 && Config::g_apmfEquipAuthority`
  (**v9 is the floor**: MFO's v9 declaration OMITS the hands it does not hold, and a v8 APMF owns
  every category by default — a v9 MFO on a v8 APMF would refuse every engine weapon equip on
  the actor, the F6 freeze; so a too-old APMF gets NO authority, `warn` once
  `[equip-auth] APMF ABI v{} has no SetEquipScope (need >= 9)`, never a blanket lock).
  `IsEquipAuthorityEnforced()` wraps APMF's v8 read (seat installed AND `bEquipObserveOnly=0`)
  and is printed as `mode=` in the claim line. `DeclareEquipScope(fid, owned, denied)` →
  `APMF_API_v9::SetEquipScope` with a stack `APMF_EquipScope` (copied inside the call), mirrored
  on `Owned::equipOwned/equipDenied` (reset to APMF's default `{All, 0}` on mint; NOT the set,
  which stays in Logistics_Economy); sent by `RefreshEquipDeclaration` immediately BEFORE
  `DeclareEquipSet` so both apply in one Drain; does not clear `equipAuthFresh` (the set does).
  `EquipAuthorityOwns(fid, cats)` = a claim stands, a declaration has gone out on it, and the
  last SENT `owned` overlaps `cats` — THE gate for a direct path writing into a held hand
  (`Logistics::EquipTorch` on `kEquipCat_Left`). `DeclareEquipSet(fid,
  vector<APMF_EquipEntry>)` → `APMF_API_v8::SetEquipSetEx`. **A REFUSED CLAIM IS NOT FAIL-CLOSED (F1/F5, Fable round 2 on
  `c66dc80`):** APMF refuses a ch.17 claim exactly when its #17a equip seat is not installed, and
  then nothing on APMF's side can perform or deny an equip — so a refusal means "MFO keeps its
  own equips": `IsEquipAuthorityClaimed` stays false and every direct path runs as without APMF
  (Actuation's `authority=false`, `AcquireEquip`'s `EquipAuthorityLive` false,
  `RefreshEquipDeclaration` returns without sending). `spdlog::warn` once per streak. Only a
  DECLARATION on a live claim fails closed (`DeclareEquipSet` false). `ClaimEquipAuthority(fid,
  bool* a_outFresh)`: a STANDING `RequestEx(kIntent_EquipAuthority, kOwnBasis, ival =
  kEquipAuth_None)` on `Owned::equipAuthHandle` — kept if live, but **RE-VALIDATED (F2)**: a kept
  handle ≥ 2 s old (`kEquipAuthValidateAfter`; younger ones are not yet in APMF's published
  snapshot, which is all `IsClaimLive` scans) is checked with v6 `IsClaimLive` and re-minted when
  APMF no longer knows it (APMF's hotkey release-all / unload sweep, a New Game, the toggle's
  OFF→ON); `a_outFresh` reports "minted since the last `DeclareEquipSet`" (`Owned::equipAuthFresh`,
  cleared by `DeclareEquipSet`) so `RefreshEquipDeclaration` drops its change detector and the new
  handle gets a declaration at once; `plugin.cpp` kNewGame calls `ClearTransientState` too (SKSE
  sends no kPreLoadGame for a New Game from a running session); refusal logged once per streak
  (`g_equipAuthRefused`); `ReleaseEquipAuthority(fid)`; `IsEquipAuthorityClaimed(fid)`;
  `DeclareEquipSet(fid, forms)` → `APMF_API_v7::SetEquipSet` (copied inside the call; > kMaxEquipSet
  32 logged as an error and truncated; no claim → error + false). NOT swept by `Tick()` (no cadence
  sizes it) — `Tick()` only RELEASES it when `bApmfEquipAuthority` reads OFF (the kill switch
  reaches in-combat followers the OOC service never does); `ClearTransientState` releases it;
  `EraseIfEmpty`/Tick's empty test include it. MFO does NOT set `kEquipAuth_ObserveOnly`: APMF's
  own `[EquipAuthority] bEquipObserveOnly` is the ONE enforcement switch. Thread road = the
  worker (`ServiceFollower`, `Actuation::EquipWeapon`), `ClaimEquipment`'s discipline: thread-safe
  APMF enqueues, `g_mx` for the map, FormIDs only. Ch.15 `ClaimEquipment` (param form, gate only)
  stands beside it — APMF INTEGRATION.md: unaffected by a ch.17 claim. The declaration itself
  (what is in the set, when it is sent) lives in `Logistics_Economy.cpp` `RefreshEquipDeclaration`
  and `Actuation.cpp` `DeclareFromLedger` — see those entries. **What breaks:** sweeping the claim
  on `FacetExpiry()`/`kExpiry` drops a standing authority mid-session and the follower's own AI
  re-dresses him the moment the set clears (nothing re-declares until the next change); releasing
  without `ForgetEquipDeclaration` leaves the change detector believing the set is still
  declared, so a re-claim never re-sends; passing anything but the standing handle to `SetEquipSet`
  is a silent no-op on APMF's side (no error comes back — the `[apmf][equip-auth]` pass line is the
  only readback); calling `Acquire()` with a header older than APMF's does NOT break (a v7 APMF
  serves v6), the reverse does; lowering the equip floor back to 8 turns a v8 APMF's whole-facet
  default into a lock on every hand MFO's v9 declaration leaves out; `EquipAuthorityOwns`
  answering from APMF's `{All, 0}` default on a claim with NO declaration out would skip the
  torch on every fresh claim for nothing (APMF allows everything until a set is declared).
- **THE OWNED CAST (default), a real AI-DECIDED animated cast — `Actuation::CastOn` FF-non-self
  hostile branch (`Actuation.cpp:600` `CastOn`, worker).**
  **ORDER OF OPERATIONS, RE-NARRATED 2026-09-07 (`fix/mfo-fourstate-followups`):** this bullet used
  to read *equip → consent → claim*, with the refusal returning from inside the equip switch. **That
  ordering is now WRONG.** `CastOn` runs the COMPETENCE gate (`follower->HasSpell`,
  `Actuation.cpp:704`) and then a **PRE-FLIGHT** (comment `:901`, the asks at `:1013-1094`) that makes
  BOTH APMF asks BEFORE `Loadout::Prepare` touches a hand and BEFORE `CasterConsent::Want` latches
  consent — so a refusal returns with NO side effects at all. See "THE ASK RUNS BEFORE THE EQUIP
  (F3-2)" under `APMFBridge` below for why that reordering was needed and what it cost to restore.
  What follows describes the SAME machinery in dependency order, not in execution order.
  SELECTION is `Loadout::Prepare`'s
  `EquipSpell(..., LeftHandSlot())` (§0.28) — **never** a hand-written `selectedSpells`/`currentSpell`
  (ENGINE_NOTES §585: a hand-write desyncs the engine's select/deselect bookkeeping; the original
  `SelectCasterSpell` helper did exactly that, on the WRONG hand, every tick, resetting the charge
  before release — root cause of the fire-and-forget hang, fixed 2026-09-02 and removed). At `Grip::
  Caster` with a DIFFERENT spell already in hand, `Loadout::Prepare` (`Loadout.cpp:191-213`) also
  **neutralizes a competing spell in the OTHER hand** (`DeselectSpell`) — but ONLY at `iCastControl`
  level 4 (exact); looser levels leave that hand to the AI (`CastExempt`). MFO (a) CLAIMS the
  `kIntent_Cast` facet via `ClaimOffenseCast` **FIRST, in the pre-flight** (called at
  `Actuation.cpp:1036`, PORTED feat/offense-cast-seats
  2026-09-05 off the retired ch.8 `kIntent_SelectSpell` gate-only claim — see PASS G below); ONLY on a
  LIVE claim does it also (b) claim `ClaimCombatTarget(create=true)` and (c) `Targeting::Command(
  follower, target->GetHandle())` to command the target (its UpdateCombat hook re-asserts
  `currentCombatTarget`) — **every owned tick this branch wins, deliberately not deduped here**:
  `EnsureCastClaimLocked` (`APMFBridge.cpp:470`, shared with `ClaimHealCast`) already no-ops an
  unchanged claim but still stamps its liveness timestamp on every call, which is what keeps the claim
  alive past its `FacetExpiry()` backstop (round-robin-aware, `APMFBridge.cpp:332`; NOT the flat `kExpiry`, `:298` —
  proven round-robin-bound by the Cicero equipment-claim capture) — a per-follower dedupe latch that
  skipped these calls on an unchanged tick was tried 2026-09-02 and reverted the same day (Fable
  review) for starving the claim mid-cast ("casting facet released" while the AI was still charging);
  `Targeting::Command` has its own unchanged-latch dedupe (`Targeting.h:37-41`) so calling it every
  tick is equally cheap. **A REFUSED claim FAILS CLOSED, from the pre-flight, with nothing done:**
  APMF present AND capable AND refusing → `Actuation.cpp:1039-1052` returns
  `{FailedSkill, "APMF refused the cast claim", transparent}` plus a rate-limited
  `[apmf] … APMF REFUSED …`; nothing routes around APMF, and neither the equip nor the consent latch
  has run yet. Only the channel being ABSENT (`OffenseCastClaimSupported()` false) or
  `bLegacyCastHybrid` degrades to the hybrid. Full story: "Gating (NO DECLINE-FALLBACK…)" and
  "THE ASK RUNS BEFORE THE EQUIP (F3-2)" below. `CasterConsent::Want` (granted at `Actuation.cpp:1109`, every tick —
  same reasoning) permits our spell + is the only remaining source of "deny competing" (APMF's own
  seats now enforce exclusivity too, see `IsOwnedCastActive` below); the **Cast-biased combat style**
  (`Scheduler.cpp:~805` sets `Stance::Cast` when a cast is wanted → raises the magic score) STILL
  supplies motive at the fringe, but once the `kIntent_Cast` claim is live APMF's five engine seats
  (`CheckShouldEquip` 0x0F, `CheckStartCast` 0x06, `GetMagicTarget` 0x0A, `CheckStopCast` 0x07,
  `SetupAimController` 0x0D — the SAME seats `ClaimHealCast` drives) select/equip/charge/aim/fire/
  channel EXACTLY the claimed spell at EXACTLY the claimed target directly — closing the
  long-standing "target right, spell wrong" defect ([[cast-gambit-spell-choice-not-enforced]]).
  Result: the vanilla AI casts our spell at our target, FULL animation, still MOBILE (ENGINE_NOTES
  §0.15a/§0.27/§0.28). Resolves [[cast-animations-deferred-to-post-town-polish]]. **GRANULAR:
  movement facet is NOT claimed** — the follower keeps kiting while it casts. **NO force on this
  path** — `CastSpellImmediate` never runs here; the returned `{NoOp,"owned cast: AI deciding
  (animated, mobile)"}` just lets the AI act. Concentration never enters this branch (bounded direct
  fork returns earlier — exact-bounding intact; `a_concentration` is always passed `false` to
  `ClaimOffenseCast` at this call site for that reason).
- **Heal/Buff FF non-self claim (feat/castone-heal-gate, 2026-09-06, API-PORT-AUDIT.md #1):**
  `ownedCast` above is Offense-only by its own classification check, so a fire-and-forget Heal/Buff
  spell aimed at an ally or the player (target ≠ follower) fell through it untouched. Immediately
  after `ownedCast`'s block (whether or not it ran — it never fires for a non-Offense spell), `CastOn`
  now also calls `ComposedCast::Try(a_follower, spell, a_target, ClassifySpell(spell), /*stopPct=*/0)`
  — the SAME `kIntent_Cast` claim `CastSelfDirect`/`CastTargetDirect` already make for a heal, reused
  verbatim (HEAL-ONLY internal gate in `ComposedCast::Enabled`, so a Buff kind degrades to
  `TryResult::NotApplicable` immediately — `Try` returns the FOUR-STATE `ComposedCast::TryResult
  {NotApplicable,Claimed,Held,ApmfRefused}`, not a bool: `Held` since Fable SEV-2 2026-09-06, and the
  `NotApplicable`/`ApmfRefused` split — the old `Refused` RENAMED and halved — since
  `fix/mfo-no-decline-fallback` 2026-09-07). On `Claimed`: `HoldCastLock` +
  `{NoOp,"composed cast: AI deciding (animated, mobile)"}`, same OPAQUE-hold shape as `ownedCast`'s
  own success return. On `Held` (a different spell's live claim owns the heal slot): NO lock, a
  TRANSPARENT `{NoOp,"composed cast: held off (another heal owns the claim)"}` — never `Fired`, and
  never a fall-through to the force hybrid. On `NotApplicable` (Buff kind, AE/APMF/
  `bHealAnimPackage` off, ABI < 5): falls straight through to the unchanged AI-first-grace +
  `ForceCast` hybrid below — byte-identical **degrade-when-absent**, a heal never silently vanishes.
  On `ApmfRefused` (APMF present, capable, and it said NO): **FAILS CLOSED** —
  `{FailedSkill,"APMF refused the heal claim",transparent}` + one rate-limited `[apmf]` error, and
  explicitly NOT the force hybrid (`fix/mfo-no-decline-fallback`). Explicitly guards
  `a_target != a_follower` (self-target CAN reach this far when `bCastSelf`, dev-only default off,
  never forked it off earlier in `CastOn` — self-cast stays `CastSelfDirect`'s own gated mechanism,
  not annexed here). **IN-COMBAT ONLY** — `CastOn` only runs from `Actuation::Fire`'s combat dispatch;
  the OOC mirror (`Logistics.cpp`'s FF beneficial direct-apply) is deliberately untouched (open
  question: whether `kIntent_Cast`'s engine seats function without a live `CombatController`).
- **PER-HAND offense claims (feat/per-hand-cast-slots, 2026-09-06).** The field problem this
  closes: a heal (LEFT, always) and an offense spell serialised onto ONE hand while the
  follower's OTHER hand sat idle — 36 cast rules fired, only 22 claims made. The parallel APMF
  change arbitrates `kIntent_Cast` PER (actor, hand) now, so MFO calls `RequestCast` up to
  twice per follower (no API change on APMF's side). `Owned::offense` is now `CastClaim[2]`
  ([0]=left, [1]=right; `Owned::heal` stays a single `CastClaim`, always left).
  `ClaimOffenseCast(a_hand)` routes to `offense[0]` for `kApmfHandLeft`, `offense[1]` for
  anything else (the NEW `kApmfHandRight` constant, or bare 0), or BOTH (mirrored, ONE
  underlying handle) for `kApmfHandDualCast` — a single-hand ask un-mirrors a stale dual
  mirror in the OTHER slot first so a shared handle is never double-released.
  `ReleaseOffenseCast(follower)` (no hand param — its two callers, `Scheduler`'s `!castSeen`
  and `Followers`' dismissal, both mean "nothing wanted on either hand anymore") releases
  BOTH slots, dedupe-aware for a mirrored dual claim. `IsOwnedCastActive(follower)` keeps its
  existing "ANY hand" per-actor contract (CasterConsent/CombatStyle's standdowns never cared
  which hand); NEW `IsOwnedCastActiveOnHand(follower, hand)` answers ONE slot, consumed by
  `Actuation.cpp`'s per-hand cast-gambit lock (`CastLockLive`). `ComposedCast`'s `[cfc]`
  silent-claim watch (`g_watch`) is likewise now `FollowerWatch{Watch hand[2]}` — `WatchClaim`
  takes an `a_hand` param (default `kApmfHandLeft`, so heal's own callers are unchanged);
  `ComposedCast::End()`/a refused heal claim clear ONLY the left slot so a concurrent
  right-hand offense watch survives.
- **THE IDLE-HAND FLOOR (F10, marth's design ruling 2026-09-08).** A THIRD claim this bridge makes,
  internal — no entry point. `Owned::floor` (`APMFBridge.cpp:284`) is a deny-only `kIntent_Cast` claim
  (`APMF_API::kCastFlag_DenyHandOnly`) standing on whichever ONE hand MFO's driving claims do NOT
  occupy, so nothing un-gambited can arm there (APMF leaving an unclaimed hand permissive is correct
  for a framework; MFO's design is that only gambited spells occur). Derived — not commanded — by
  `ReconcileHandFloorLocked` (`APMFBridge.cpp:937`), called from ONE place: `Tick()`
  (`APMFBridge.cpp:1698`), after every expiry sweep, every pump (~133 ms), gated on `bApmfCast` +
  ABI ≥ 5. That pass also owns the floor's `refreshed` stamp and TTL heartbeat.
  **Exactly one hand driven → floor the other; both driven → none; NEITHER driven → NEITHER hand is
  floored** (not both: principle 4 — nothing was declared; it would mute followers with no cast gambit
  authored; a floor with nothing driving is the standing hold `kIntent_Cast` may never be).
  Requested at the SAME `kOwnBasis` (200.0f) as every other claim — APMF's `BetterClaim` makes a
  deny-only claim LOSE to a driving one at an EQUAL basis, so MFO's own next gambit takes the hand with
  no release/re-request gap and the floor stands underneath again when it ends. A floor at a higher
  basis would silence MFO's own casts (APMF warns loudly about it).
  `EnsureCastClaimLocked` (`APMFBridge.cpp:546`) gained a `wantDenyOnly` parameter rather than a
  parallel mint path, so the floor inherits the same liveness check / heartbeat / fail-closed split /
  `reqParam` mirror; `CastClaim::denyOnly` (`:102`) is part of the claim's IDENTITY; the release
  sentinel is now "no spell AND not a floor". Both log throttles became **two entries per follower**
  (driving / floor) — one shared entry would have alternated spell X ↔ 0 and defeated both 5 s windows.
  **What breaks if you change this:** floor at a higher basis → MFO denies its own casts; floor with no
  driving claim → the actor-wide (`Hand::kUnknown`) allowance read starts denying everything; forget
  `ClearTransientState`/`EraseIfEmpty` and a floor outlives MFO's handle for it and holds a hand shut
  until APMF's TTL. It does NOT deny MFO's own direct force (`CastSpellImmediate` is `MagicCaster`
  vtable slot 01, `CheckCast` is 0A — pinned `include/RE/M/MagicCaster.h:46,55`) and does NOT deny
  weapons (APMF's 0x0F seat is on the spell/staff selector vtables only).
  **THE UNOBSERVED GATE (`fix/mfo-combat-restoration-direct`, 2026-09-21; RE-SIZED AND ARMED ON CHARGE
  `fix/mfo-spell-authority-0922`, 2026-09-22).** A driving claim earns the
  floor only while it can be believed to be driving: younger than **`kIdleFloorUnobservedMs`**
  (`APMFBridge.h`, **8000 ms, NO LONGER ALIASED** to `kHealHoldNeverObservedMs`. The 2026-09-22 Deck
  log supplied the measurement `MFO-B47` asked for: an OFFENSE claim reaches an observed SpellFire in
  **5.8 s** with an equip cycle in front (Jesper 750012C6, claim 08:26:59.202 → SpellFire
  08:27:04.997), so the old 4000 ms fired EARLY on every offense claim — and every claim is an
  offense claim now that heals take the direct road. 8000 ms clears that by 2.2 s and the worst heal
  claim-to-fire ever measured (**6.1 s**, `Docs/DIAG-2026-09-08-field.md:267`) by 1.9 s. The two
  constants are de-aliased because they no longer bound one quantity: `kHealHoldNeverObservedMs`
  governs a heal claim's heartbeat and the cast-lock hold cap, this one an offense claim's
  equip+charge+fire pipeline. `MFO-B7`/`MFO-B8` remain open for `kHealHoldNeverObservedMs` itself,
  which this change did NOT touch), or observed firing within its
  own lifetime (`ComposedCast::ObservedFiring(follower, c.hand, c.spell, age)`, age from the
  mint-only `CastClaim::created`). Past that with no observed cast, `ReconcileHandFloorLocked`
  RELEASES a standing floor, logs `[apmf] <id> IDLE-HAND FLOOR released -- driving claim has no
  observed cast (...)`, and mints none until the claim is observed (then the floor returns with its
  usual `claimed` line). The driving claim itself is untouched (TTL / sweep / hold caps end it). Left
  drives = `offense[0]` AND `heal` both silent; a mirrored DualCast never reaches the gate (both
  driven). `ComposedCast.cpp` static_asserts the gate outlasts `kSilentWarnAfter` so the `[cfc] NO
  observed cast` line precedes the release. A claim whose watch was never armed reads silent past the
  gate and opens the other hand — the pre-F10 state, never a freeze.
  **AND THE AGE IS NOT THE WHOLE TEST ANY MORE (2026-09-22).** `silentPastGate` also refuses to call a
  claim silent while the ENGINE IS ACTUALLY CHARGING IT — `Actuation::CastInFlightOnHand(actor, hand,
  c.spell, c.proxy)`, THE one in-flight definition (`Actuation_Hands.cpp:251`, a plain read of
  `ACTOR_RUNTIME_DATA::magicCasters[slot]->state`/`currentSpell`, no virtual call). On 09-22 the floor
  lifted **60 ms AFTER** the engine began charging the claimed Firebolt and the AI charged its own
  spell in the freed hand 0.78 s later: a longer timer alone could not have prevented that, only
  reading the engine could. `ReconcileHandFloorLocked` resolves the actor ONCE per pass
  (`RE::TESForm::LookupByID<RE::Actor>`); a null actor leaves the age test as the only word, which is
  the pre-2026-09-22 behaviour. `APMFBridge.cpp` now includes `ComposedCast.h` AND `Actuation.h`; `Tick()` runs INSIDE the AddTask body (`Diagnostics.cpp:346`), i.e. ComposedCast's
  own serialized context, and ComposedCast never takes `g_mx`. **What breaks:** call the gate from any
  road that is not the job worker and the lock-free watch map races; shorten N below the measured
  tail and the other hand opens while a genuine cast is still charging.
  **Recorded, not fixed (understated in the first version of this note):** `Loadout::Prepare` takes no hand
  parameter and always equips into `LeftHandSlot()` (`Loadout.cpp:279, :359`), so on a RIGHT-hand offense
  claim MFO equips the gambit spell into the LEFT hand, which the floor has closed to the AI. The cast still
  works (the claim on the other hand drives it), but the cost is **a wasted left-hand equip on every such
  claim**, and on a follower carrying a shield or weapon in the left hand it DISPLACES that gear and books a
  restore debt. With right-first AUTO this is now the common case, not an edge. The fix is a hand parameter
  on `Prepare` — outside this brief's file boundary, reported rather than reached for.
- **`RefreshOwnedCastOnHand(follower, hand, holdSpell = 0)` (`APMFBridge.cpp:1261`, F9).** Refreshes the cast claim(s)
  MFO already holds on a hand WITHOUT re-requesting: it replays each claim's OWN stored tuple through
  `EnsureCastClaimLocked`, so the identity compare matches by construction: the TTL is renewed in place
  (liveness, lazy proxy read, heartbeat Repoint, `refreshed` stamp) and the hand mode / target / a
  still-in-force claim can never be disturbed. **It is NOT true that the fast path is the only path it can
  take** — a handle APMF has already auto-expired takes the aged-out branch and is RE-REQUESTED from here
  (correct, and not a concurrent second claim: the dead handle is dropped first). `LEFT`
  covers the left offense slot AND the heal slot; `kApmfHandDualCast` re-mirrors both slots so their
  stamps move together (a stale mirror would have `Tick()` release the shared handle out from under the
  live slot). A `false` means the claim is genuinely gone — the caller must fall through to its normal
  claim path and must NOT log it as an APMF refusal.
  **What breaks if you change this:** pass anything but the stored tuple and it mints a second claim /
  interrupts the charge — the exact churn F8 removes.
- **THE ch.8 CAST-SELECT GATE — CANDIDATE REFUSAL (`fix/mfo-spell-authority-0922`, 2026-09-22).**
  `PublishSpellAllowList` / `ReleaseSpellAllowList` / `IsSpellAllowListClaimed` (`APMFBridge.h`,
  `APMFBridge.cpp` just above `ClaimHealCast`) + `Owned::selectHandle/selectList/selectRefreshed/
  selectMintedAt`. A GATE-ONLY `kIntent_SelectSpell` claim (`param.form == 0` — it names no spell and
  drives nothing) carrying an ABI v4 `SetSpellAllowList` allow-set. **THE HOLE IT CLOSES:** MFO governed
  CASTING but governed EQUIP/SCORE only on a hand with a live cast claim — the score steer only ADDS
  +1000 to the claimed spell and never lowers anything, and a zero would not work either (only
  `CheckShouldEquip` 0x0F REMOVES a candidate). Field 2026-09-22: the AI equipped an ungambited Vampiric
  Bolt (`841B8A15`) in BOTH hands and charged it while MFO's consent denied every cast — ~133 s of 221 s
  of party combat visibly inactive. APMF has shipped the allow-list since ABI v4 and MFO had NEVER called
  it. APMF reads it at `core/Allowance.cpp:60-79` and consults it at BOTH seats, **actor-wide and
  independent of per-hand cast claims and of the idle-hand floor**: `core/EquipGate.cpp:609` (t2a
  `CheckShouldEquip`, engine-answer-first — THE candidate refusal) and `core/CastGate.cpp:160` (t2c
  `CheckCast`). **ONE SOURCE OF TRUTH BY CONSTRUCTION:** published from inside
  `CasterConsent::NoteGambits` (`CasterConsent.cpp`), the SINGLE writer of the `g_ctrl` set the
  continuous deny reads, so the deny and the allow-list are built from one vector on one lap and cannot
  disagree — that disagreement IS the principle-2 violation the fix exists to close. Released from
  `CasterConsent::Clear` (combat end / dismissal), `ClearTransientState` (revert/load), and `Tick()`'s
  `FacetExpiry()` sweep + kill-switch check. **THE LIST MUST CARRY MORE THAN THE GAMBITS:**
  `CtrlUnlatchedDeny` is NORMAL SPELLS ONLY (`Is(FormType::Spell)` + `GetSpellType() == kSpell`), so
  staves/scrolls/powers are DELIBERATELY exempt from MFO's own deny — and APMF's allowance is a pure
  FormID-set test that cannot be told "spells only", so `AppendDenyExemptForms` enumerates carried
  STAVES (the staff form AND its `formEnchanting`, because t2a sees the staff and t2c sees the spell) +
  carried SCROLLS + `kPower`/`kLesserPower`/`kVoicePower` from `TESNPC::GetSpellList()` and
  `ACTOR_RUNTIME_DATA::addedSpells`. `kAbility` is deliberately NOT enumerated (passive, never cast, and
  it alone would exhaust the budget). APMF's own delivery-flip proxies for live cast claims are appended
  from `Owned`. MFO's own `ConcProxy` pool is NOT enumerated and does not need to be: those are 0xFF
  `IFormFactory` spells never in any actor's spell list (so `CombatInventory` can never build a candidate)
  cast via `CastSpellImmediate` (`MagicCaster` vtable slot 01) while APMF hooks 0x0A / 0x0F.
  **OVERFLOW FAILS OPEN, LOUDLY:** past `kMaxSpellAllowList` (32) APMF's degrade is "the excess are
  non-exempt", i.e. DENIED, which would disarm the follower — so an oversized list means NO CLAIM AT ALL
  plus an `[cast-select] ... the gate is NOT claimed` error, and the cast-time deny carries the load
  (#7). **AT FIGHT END THE GATE LIFTS COMPLETELY** (the decision, recorded in `Clear`): out of combat MFO
  leaves a follower's own casting alone and the cast-time deny is gone too. Kill switch
  `bApmfSpellAllowList` (INI-only, default ON, same shape as `bApmfCast`), additionally inert with
  `bApmfCast` off or cast control off. Log: one `[cast-select] <id>: allow-list CLAIMED/updated n=N [...]`
  line per claim/change, one `RELEASED` line per release.
  **What breaks if you change this:** drop a form the follower legitimately needs off the list and MFO's
  OWN gate disarms him (this has bitten twice — staves and the graduated levels are the two known traps);
  TRUNCATE instead of refusing on overflow and the excess is silently DENIED; publish it from anywhere
  but `NoteGambits` and the deny and the allow-list can drift apart; call it while holding
  CasterConsent's `g_mx` and the combat thread's deny hooks queue behind an inventory walk; forget the
  `Tick()` sweep and a follower who stops being serviced keeps a DENY nothing can lift.
  **COMBAT POTIONS ARE ON THE LIST, AND THAT IS NOT PRECAUTIONARY.** Every carried `AlchemyItem` that is
  not food and not poison is enumerated, because **this own-goal already shipped once**:
  `Docs/ENGINE_NOTES.md:1759-1762` records "the deny was suppressing combat potions —
  `CombatMagicCasterRestore` is also the drink-potion caster", fixed in v1.0.32 only by restricting MFO's
  own deny to `formType == Spell`. APMF's allowance cannot be told "spells only", so a potion must be
  NAMED or this gate re-creates that regression, and `act.drink_health_potion` depends on it.
  `IsMedicine()` is deliberately NOT the filter (an authored flag a mod potion can lack).
  **FIELD OBSERVABLE THE NEXT DECK RUN MUST SETTLE (b).** It is NOT statically determinable whether the
  AI's combat potion use routes an `AlchemyItem` through `CheckCast` at all. So watch for **any
  `[t2c] ... ch.8 select DENY` naming an ALCH form while a follower is low on health and carrying
  potions** — that is the hole, and it should now be impossible. The `[cast-select] ... allow-list
  CLAIMED n=N (... + N potion + ...)` line prints the potion count, so a log identifies a missing class
  by subtraction. **Kill switch if anything about the gate misbehaves: `bApmfSpellAllowList=0`** (INI,
  default 1) — the cast-time deny then carries the load exactly as before the gate existed.
  **THE COST OF THE POTION EXEMPTION, stated:** it spends the 32-form budget, so a follower with a large
  alchemy hoard can push the list past `kMaxSpellAllowList` and lose the gate entirely (fail-open, loudly
  logged, never a mute). That trade is deliberate — a denied healing potion is far worse than an inert
  gate.
  **OPEN BACKLOG — read before editing:** `MFO-B65` (the ALCH specifics + the budget consequence).
- **Claim lifecycles (arbitration records, `g_owned` mutex-guarded — worker+main):** offense-cast =
  PER-CAST, TTL-bounded (`kIntent_Cast`, PER-HAND now — see above; refreshed each winning cast
  tick; released crisply by
  `ReleaseOffenseCast` ← `Scheduler.cpp:~901` on `!castSeen`, which also clears the shared `[cfc]`
  watch, `ComposedCast::ClearWatch`; **and since 2026-09-21 `Tick()`'s own FacetExpiry sweep clears the
  SWEPT claim's hand watch too — `ComposedCast::ClearWatchHand(follower, hand)`, per-hand, hand 0 only
  when no heal still shares it — so a later same-spell claim no longer inherits a dead `since` (deck:
  `[cfc] claim live 32383 ms` for a claim swept 19 s earlier)**). combat-target = PER-COMBAT (created by EITHER the cast directive
  OR the ATTACK directive — both `ClaimCombatTarget(create=true)` ← `Actuation.cpp:1212` (and `:1869` from `Fire`) (2026-09-03:
  melee-attack directives now get the same arbitration a caster already had, not just a re-point of a
  pre-existing claim) — re-pointed via APMF `Repoint` when the foe changes; `RefreshCombatTarget` ←
  `Scheduler.cpp:~330` keeps it alive every in-combat tick; released only at combat end via the
  expiry sweep). `Tick()` ← `Diagnostics.cpp` (~`:334`) is the per-claim expiry sweep — offense-cast/
  combat-target/weapon-order-equipment/heal-cast compare against `FacetExpiry()` (round-robin-
  aware, see the 2026-09-05 entry below), package-offer alone against the flat 500 ms `kExpiry`.
- **Gating (NO DECLINE-FALLBACK, `fix/mfo-no-decline-fallback` 2026-09-07):** owned model active iff
  `Available() && bApmfCast (INI, default ON) && !bLegacyCastHybrid (MCM, default OFF)`. A failed
  `ClaimOffenseCast` splits into the TWO ASYMMETRIC halves of marth's rule ("without APMF, it needs to
  just work, whatever unpolished way works. With APMF theres no fallback for it. APMF shoudl work") —
  the SAME rule the `Packages.cpp` loot-travel entry below (§ "APMF LOOT-TRAVEL") has stated since
  2026-09-03, now enforced here too:
  - **DEGRADE-WHEN-ABSENT** — `bLegacyCastHybrid` ON, APMF absent, or the claim channel is not there at
    all (`APMFBridge::OffenseCastClaimSupported()` false: ABI < 5, or `bApmfCast` off) → the ORIGINAL
    AI-first-wait + force-on-miss package hybrid (unchanged legacy branch below the owned block — the
    ONLY place `CastSpellImmediate` force and the rooting UseMagic package survive). Silent (APMFBridge
    already logs a too-old ABI once per session).
  - **FAIL CLOSED** — APMF present AND capable AND it REFUSED (`OffenseCastClaimSupported()` true) →
    `Actuation.cpp` returns `{FailedSkill, "APMF refused the cast claim", transparent}`; the cast does
    NOT happen and NOTHING routes around APMF into the legacy hybrid. One `spdlog::error` `[apmf] …
    APMF REFUSED the … claim` names actor/spell/target/hand, rate-limited to one line per
    (follower, spell, target) per ~5s (`LogApmfRefusal`, `Actuation.cpp` anon ns; a twin lives in
    `Actuation_Direct.cpp`). A refusal is a bug to fix in APMF, never a condition to degrade through
    (`CLAUDE.md` principle 7). The heal twin (`ComposedCast::TryResult`, widened from three values
    to FOUR by this branch — see the ComposedCast entry below) and the two concentration
    `ClaimOffenseCast` sites in `Actuation_Direct.cpp` (`CastSelfDirect`/`CastTargetDirect` →
    `SelfCast::Declined` on a refusal) follow the identical split. **This bullet no longer contradicts the FAILS-CLOSED principle stated
    for `Packages.cpp` below.**
  - **THE ASK RUNS BEFORE THE EQUIP (F3-2, `fix/mfo-fourstate-followups` 2026-09-07).** Both asks used to
    live INSIDE `CastOn`'s equip switch, i.e. AFTER `Loadout::Prepare` had put MFO's spell in the LEFT hand
    and AFTER `CasterConsent::Want` had latched consent — and nothing undid either on a refusal. Against an
    owner holding the facet with its own spell equipped, `Prepare` short-circuits only when MFO's spell is
    already in hand, so MFO re-equipped over that owner EVERY 133 ms tick while logging "this cast does NOT
    happen", and latched consent let the follower's own AI cast it unforced — a fallback nobody asked for,
    wearing a refusal's clothes. Both asks now sit in a **pre-flight** (comment `Actuation.cpp:901`, the
    asks themselves `:1013-1094`), still
    gated on `bEquipToCast`, and a refusal returns from there with nothing done.
    **Two things the reordering spent that main got for free, both restored by the pre-merge review
    2026-09-07:** (1) the COMPETENCE gate — `Prepare`'s `!HasSpell` check used to precede every ask, so a
    claim for a spell the actor does not know was structurally impossible; `CastOn` now runs `HasSpell`
    itself AHEAD of the pre-flight (`Prepare` keeps its own as defence in depth). (2) "a live claim implies
    a hand lock" — every claim used to be minted in the Equipped arm, which always stamps the lock, and
    `HandFree` returns true on `lock.spell == 0` BEFORE consulting `CastLockLive`; the **Debounced arm now
    stamps the lock when a claim is standing**.
    Deliberate consequence, documented at the site: a claim can now be minted on a tick whose `Prepare`
    then debounces — correct under the owned model, since APMF's seats do the equipping while the claim
    stands. It does NOT get released by Scheduler's `!castSeen` path in that state (the rule still holds,
    so `castSeen` is true); it ends when the RULE stops holding. **`fCastCooldown` NO LONGER PACES AN
    APMF-OWNED CAST, AND THAT IS CORRECT — SETTLED (marth 2026-09-07, memory
    `cast-cooldown-inert-is-correct`); do NOT re-raise it as a regression.** Precisely one effect went away:
    `CasterConsent::ClientCastClaimed` (`CasterConsent.cpp:231-235`) early-passes while an APMF cast claim
    is live, so the deny thunks never compute a verdict and the pacing deny (`CasterConsent.cpp:762-775`)
    cannot fire for a claim-holding follower; on main the claim went un-refreshed through a cooldown,
    `FacetExpiry` swept it and that deny re-engaged for the tail. **The key is NOT inert generally** — it
    still stamps via `Loadout::StartCooldown` (`Loadout.cpp:413`, from the `[cast]` SpellSink
    `Diagnostics.cpp:263`) and still `ReleaseSpell`s, `Prepare` still returns `Debounced` so MFO does not
    re-equip in the window, the pacing deny **still bites in full on the APMF-ABSENT path — and only that
    one** (`bLegacyCastHybrid` / `bApmfCast`-off do NOT restore it; they gate `ownedCast` only, while
    `ComposedCast::Enabled` (`ComposedCast.cpp:38-45`) reads neither, so with APMF present and
    `bHealAnimPackage` ON a HEAL gambit still mints a claim on those paths and `ClientCastClaimed` stands the
    deny down identically — corrected by the re-review 2026-09-07), and the direct-force FF beats
    (`Actuation_Direct.cpp:989`, `:1282`) and
    `CastAuto`'s interval (`:1509`) are untouched. marth: *"a cast can only cast as fast as a cast. No
    reason to be slower."* The engine's own equip→charge→fire pipeline (~2.3-2.5s offense, 2.95s
    claim-to-observed on the one landed heal) already paces casting at the fastest a cast can physically
    occur, so a cooldown on top can only make a follower SLOWER than the engine allows — losing it on the
    owned path is **the absence of a redundant limiter, not a lost guard**, and gating the ask on
    `Loadout::CoolingDown` would re-add the redundancy. Reviving it there needs a NEW justification (magicka
    economy, or a thrash the engine does not bound), never "it used to fire". **Keep this separate from the
    hand lock:** the cooldown not pacing an owned cast is fine, a claim standing without its lock was not —
    that was the SEV-2 defect fixed in the Debounced arm above.
  - **AN OUTRIGHT REFUSAL IS A REFUSAL (F3-1, same branch).** APMF's `ControlMap::EnqueueCast` returns
    `kInvalidHandle` synchronously ONLY when no channel serves `kIntent_Cast`; everything else is decided
    later, in `Drain()`. So a request APMF ends up publishing NO claim for still returned a handle, reported
    `Claimed`, took the opaque `"owned cast: AI deciding"` NoOp that walls off every rule below, read
    not-live next tick, and was re-requested at INFO forever — the fail-closed path was nearly unreachable
    in the field. `CastClaim` now carries **`everLive`** (`APMFBridge.cpp`, latched the first time
    `IsClaimLive` answers true — including from `RefreshHealCastClaim`'s own sighting — and reset with the
    claim): a not-live handle that WAS live aged out at APMF's TTL and re-requests as before; one that was
    **never published** is released and NOT re-requested, so the claim comes back empty, the caller fails
    closed, and a `spdlog::warn` (deduped 5s per follower+spell) names the shape. Not a latch — the next
    winning tick asks afresh.
    **SCOPE, corrected by the pre-merge review 2026-09-07 — do NOT read this as "MFO detects arbitration
    losses".** APMF `push_back`s a claim REGARDLESS of ownership and `IsClaimLive` answers true for ANY
    unexpired claim carrying that handle, owner or not, so an ordinary **basis or tie loss reads live,
    latches `everLive`, and is invisible here** — MFO reports `Claimed` while APMF drives another client's
    spell. This catches OUTRIGHT refusals only: hand-collision loser, unloadable actor,
    FromPackage-without-spell.
    **FOLLOW-UP `apmf-isclaimowning-v7` (OPEN, tracked — not a note):** telling a basis/tie loss apart needs
    a real owner query, `IsClaimOwning(handle)`, which is an **APMF v7 ABI addition that does not exist
    yet**. It belongs to the APMF repo (append-only `APMF_API.h` + a `ControlMap` owner lookup) with an MFO
    consumer change behind an `abiVersion >= 7` guard, exactly like the ABI-6 `IsClaimLive`/`GetCastProxy`
    adoption. Until it lands, MFO CANNOT detect an arbitration loss and must not be documented as though it
    can.
- **CAST-CLAIM HEARTBEAT — the TTL is a renewable FLOOR now, not an expiry (F5-1, 2026-09-08; RC2 of
  `Docs/DIAG-2026-09-06-deny-heal-failures.md`).** A `kIntent_Cast` claim used to hard-expire at APMF's
  6 s TTL (`kHealCastTtlMs`, `APMFBridge.h:543`) and get re-requested on the next round-robin lap, which
  left the actor **UNCLAIMED for 0.3–1.2 s every 6 s** — measured, with a foreign Stone Rune observed
  equipping and charging **110 ms** into one of those gaps. APMF's half of the fix is merged
  (`core/ControlMap.cpp`'s `ApplyRepoint` TTL RENEWAL: a `Repoint` on a **live** cast claim moves its
  deadline to `now + the claim's own granted ttlMs`, and REFUSES to resurrect a lapsed one). MFO's half is
  here: `EnsureCastClaimLocked`'s **unchanged-AND-still-live** fast path now sends that `Repoint` before
  its early return.
  - **Where, and only there.** The heartbeat is inside the `liveNow` branch of `EnsureCastClaimLocked`
    (`APMFBridge.cpp:470`), so it fires ONLY from `ClaimHealCast`/`ClaimOffenseCast` — a rule that WON its
    lap re-asking for the identical `(spell,target,hand,conc,stopPct)` it already holds. It is
    **deliberately NOT in `RefreshHealCastClaim`**: that is `ComposedCast`'s incumbent HOLD, which keeps
    `refreshed` moving for a claim whose rule may have gone silent, and renewing APMF's TTL from there
    would remove the liveness bound that hold is documented to rely on.
  - **Covers heal AND both offense hands**, because it lives in the create-or-refresh helper all three
    `CastClaim` slots already share (`Owned::heal` `:202`, `Owned::offense[2]` `:213`). A mirrored
    `DualCast` claim is ONE handle in both slots and `ClaimOffenseCast` only ever `Ensure`s slot 0, so it
    is renewed exactly once.
  - **Interval = `min(TTL/2, TTL − FacetExpiry())`, floored at 0** (`CastHeartbeatInterval()`,
    `APMFBridge.cpp:449`, anon ns, right above `EnsureCastClaimLocked`). Sized from the REAL cadence (#9), and
    from the window this file ALREADY agrees on rather than a second invented budget: `FacetExpiry()`
    (`:306`) is MFO's own upper bound on the gap between two services of one follower, so an interval
    above it could let the window lapse between renewals. At the defaults (`fSuppressWindow` 1.5, solo)
    `FacetExpiry()` ≈ 2446 ms → the `TTL/2` = 3000 ms term wins; `fSuppressWindow` 3.0 + 5 followers →
    ≈ 4658 ms → interval ≈ 1342 ms. **Not an expiry:** nothing is released on it, and no release decision
    moved — the `Tick()` sweep still runs off `refreshed`.
  - **New `CastClaim` fields** (`APMFBridge.cpp:82`): `renewed` (when APMF's window last started —
    stamped with `created` at the mint, then advanced by each renewal; `created` still never moves, so the
    never-observed hold cap keeps its bound and in fact TIGHTENS, since the 6 s expire-and-re-request cycle
    that used to reset it no longer happens under a still-winning rule) and `reqParam` (the exact
    `APMF_Param` APMF stored — `form = spell`, `ival = flags`, everything else zero, mirroring
    `ControlMap::EnqueueCast` — re-sent verbatim, never rebuilt, because `ApplyRepoint` REFUSES a form
    change on a cast claim and a rebuilt flags word is free to drift).
  - **ABI ≥ 6 ONLY** — a correctness gate, not caution. On ABI < 6 `liveNow` is an assumption (no
    `IsClaimLive` to ask), so a `Repoint` from there could target a long-expired handle and log a renewal
    that never happened (a mask, #7). ABI < 6 stays byte-identical. Inert in practice (`kABIVersion` is 6).
  - **Observability:** `spdlog::info` `"[apmf] … kIntent_Cast claim (spell …) HEARTBEAT -- Repointed in
    place, renewing APMF's 6000ms TTL (claim age …ms, …ms since the last renewal, next in ~…ms)"`, throttled
    by `g_lastRenewLog` — the SAME shape/5 s-per-(follower,spell) window as `g_lastNeverLiveWarn`, so a new
    spell always logs its first renewal. The line is self-proving rather than a count: ONE line whose
    **claim age exceeds 6000 ms** is proof the window was renewed and not re-requested. **No watchdog, no
    retry:** a renewal that does not take surfaces on the next lap as the existing
    `"already auto-expired -- re-requesting"` line.
  - **`everLive` (above) is untouched by this.** The heartbeat runs only where `liveNow` is already true,
    which is exactly where `everLive` was already being latched, so it can neither latch it on its own nor
    reach the never-published fail-closed branch. An OUTRIGHT refusal never publishes, so `IsClaimLive` stays
    false, no heartbeat is ever sent for it, and the fail-closed path is unchanged.
- **`IsOwnedCastActive(follower)` (Phase 2, ALLOWANCE-TEMPLATE.md §7; REPOINTED feat/offense-cast-seats
  2026-09-05; backing store re-shaped feat/per-hand-cast-slots 2026-09-06):** worker- AND
  combat-thread-safe read (the same `g_mx` every other accessor takes) —
  true iff the follower holds a LIVE `kIntent_Cast` offense claim on EITHER hand
  (`g_owned[id].offense[0].handle` OR `offense[1].handle` valid — was one flat `offenseHandle`,
  was `spellHandle`/`kIntent_SelectSpell` before that port; same accessor name, same call sites,
  new backing store each time — its own per-actor "any hand" CONTRACT never changed). `CasterConsent.cpp`'s two exclusivity denies AND (new this pass) `ClientCastClaimed`'s
  generalized early-pass, plus `CombatStyle.cpp`'s equip gate, all consult it to stand down for that
  follower once APMF's own seats/T2 hooks (a separate APMF.dll) enforce the identical exclusivity via
  the SAME claim — avoiding two independently-configured deny mechanisms disagreeing, and (via
  `ClientCastClaimed`) preventing a HARD-ABORT of a claimed offense cast the same way the S1 fix
  protects heal. Does NOT touch consent (`CasterConsent::Want`'s veto-removal stays MFO's alone,
  called unconditionally).
- **Runtime-only — NO save/co-save state**; `ClearTransientState()` ← `plugin.cpp` kPreLoadGame
  (after `StopPump`). `g_apmf` is `std::atomic`. APMF holds ZERO MFO code; this bridge is the ONLY
  MFO code aware of APMF.
- **`MaybeWarnAbsence()` (`:184` area, beside `Available()`)** — the 2.0.0 "APMF recommended"
  nudge: an unobtrusive `RE::DebugNotification` corner toast (never a modal), ~10 min cadence, ONLY
  while `Available()` is false and `Config::g_warnNoApmf` (MCM `bWarnNoApmf`, default ON) is set.
  Zero cost and can never fire once APMF IS present (one `Available()` check short-circuits it).
  Called every pump tick from `Diagnostics.cpp`'s `SleeperLoop` beside `Tick()` (worker thread) —
  cadence state is plain `Rapport::SessionMinutes()` math (no new co-save/serialization hook: a
  load makes `SessionMinutes()` go backwards, which this reads as "new session, reset the gate").
  The actual `DebugNotification` call is `MainThread::Post`ed (DebugNotification is main-thread-only;
  this function itself runs on the worker).
- **PASS B (ch.9/ch.7 channels): `OfferPackage`/`ReleaseOfferPackage` (package-offer, `kIntent_
  OfferPackage`) + `ClaimCombatActionDeny`/`ReleaseCombatActionDeny` (combat-action deny,
  `kIntent_CombatAction`, `param.ival` category bitmask) — SAME `g_owned`/`EnsureClaimLocked`
  machinery, two new fields (`packageHandle`/`package`, `actionHandle`/`actionMask`) + an ival
  twin `EnsureIvalClaimLocked`. `ClaimCombatActionDeny` is built but NOT wired into loot-travel
  (see Packages.cpp below) — MFO's own PACKAGE-THEFT guard already concedes loot-travel to a
  live combat package on purpose.
- **PASS C (ch.15 `kIntent_Equipment`, 2026-09-03): `ClaimEquipment`/`ReleaseEquipment`/
  `IsEquipmentClaimActive`** — retires MFO's native weapon-order equip gate
  (`CombatStyle.cpp`'s `EquipGateThunk`) onto APMF's T2a `CheckShouldEquip` hook, same
  `g_owned`/`EnsureClaimLocked` shape, one new field pair (`equipHandle`/`equip`). GATE-ONLY:
  the claim's `param.form` (the forced weapon's FormID) makes APMF write nothing; MFO still
  EXECUTES the force-equip itself (`Actuation.cpp`'s `EquipWeapon`/`g_forcedWeapon`). Engaged
  + refreshed every tick the force-hold survives by `Actuation::ReconcileForcedWeapon`
  (`Actuation.cpp:1984` `kActPowerAttack`, gated `Config::g_weaponStyleControl`); released by
  `Actuation::ReleaseForcedWeapon` (`:~1227`, the single choke point every teardown path —
  `Followers.cpp`, `Scheduler.cpp` — funnels through). `CombatStyle.cpp`'s `EquipGateThunk`
  consults `IsEquipmentClaimActive(fid)` right after the existing `IsOwnedCastActive` check
  (independent of it) and stands its own weapon-order deny down when true — APMF's hook
  replicates the SAME off-hand-loan exemption `IsOwnedCastActive`'s own kIntent_Cast claim
  protects (`APMF_API.h`'s `kIntent_Equipment` comment; PRE-PORT this was
  `kIntent_SelectSpell`'s exemption — see PASS G below). False whenever APMF is absent/off or
  no claim was made → byte-identical native enforcement on that path.
- **PASS D — RETIRED (2026-09-05, superseded by PASS E below).** The original
  ch.8b `kIntent_Cast`/`RequestCast` TTL-bounded cast-EXECUTION facet
  (`ClaimCast`/`ReleaseCast`/`IsCastClaimActive`, `APMF_API_v5::RequestCast`)
  is GONE from both `APMFBridge.*` and `APMF_API.h`'s consumption — it required
  MFO to force-equip and hand-drive the cast itself (`ComposedCast`'s
  `DriveObservedCast`), which raced APMF/the AI for the SAME hand and caused a
  cross-thread use-after-free CTD in the field. `APMF_API.h` itself still
  DECLARES `kIntent_Cast`/`APMF_API_v5::RequestCast`/`APMF_CastRequest` (it is
  APMF's byte-shared header, mirrored verbatim — MFO never edits it locally),
  but MFO no longer calls any of it.
- **PASS E — RETIRED (ch.8 `kIntent_SelectSpell` +ACT, feat/cast-act,
  2026-09-05; superseded by PASS F below, feat/mfo-cast-port, same day-ish).**
  Named the heal-cast facet a DECLARATIVE `kIntent_SelectSpell` claim
  (`p.form`/`p.ival`/`p.target`, `Repoint`-in-place on a change), driven by
  APMF's own `core/CastExecutor.cpp` force-equipping the hand and replaying the
  observed cast sequence. APMF's `feat/ai-cast-seats-impl` retired that whole
  drive in turn: `kIntent_SelectSpell`'s `ival`/`target`/`pos` are now
  accepted-and-ignored (gate-only again, exactly PASS E's own offense sibling
  `ClaimCasting` always was) — so PASS E's mechanism no longer does anything to
  a heal claim built on it. Nothing of PASS E's CODE survives here (see PASS F);
  kept as history because the doc-comment trail (`APMFBridge.h`/`.cpp`,
  `Docs/CAST-DELIVERY.md`) still narrates it.
- **PASS F (ch.8b `kIntent_Cast`/`RequestCast`, feat/mfo-cast-port, 2026-09-05):
  `ClaimHealCast`/`ReleaseHealCast`/`IsHealCastActive`/`GetHealCastProxy`/
  `RefreshHealCastClaim`** (defs `APMFBridge.cpp:1116`/`:1155`/`:1163`/`:1170`/`:1184`
  in that order — `:885` is the SEPARATE `GetOffenseCastProxy`, not one of these
  five; decls `APMFBridge.h:610,625,635,644,719`; the last two ADDED 2026-09-06 by
  feat/consume-cast-observability + `fix/mfo-heal-slot-and-proxy` — see the F1
  hold bullet under `ComposedCast.cpp` below for what they are for) — the
  heal-cast facet `ComposedCast` claims, PORTED BACK to the same ch.8b Intent PASS D used (the
  Intent was always declared in `APMF_API.h`, just unused by MFO between PASS D
  and PASS F) now that APMF's `feat/ai-cast-seats-impl` answers FIVE engine
  vfunc seats — `CheckShouldEquip` (0x0F), `CheckStartCast` (0x06),
  `GetMagicTarget` (0x0A), `CheckStopCast` (0x07), `SetupAimController` (0x0D)
  — while the claim stands, so the follower's OWN AI equips/aims/charges/fires/
  channels the spell natively. Unlike PASS D, **APMF fires NOTHING itself**
  (no `EquipSpell`/`CastSpell`/`CastSpellImmediate`/anim-graph write) — every
  engine call is the AI's own; MFO's force-equip-and-hand-drive race that
  killed PASS D never reappears because MFO STILL makes no engine call either.
  Own `g_owned` slot — **field names corrected 2026-09-07: these were written as
  `healHandle`/`healSpell`/`healTarget`/`healHand`/`healConc`/`healStopPct`/
  `healRefreshed` and NO SUCH FIELDS EXIST.** The slot is `Owned::heal`, a
  `CastClaim` (`APMFBridge.cpp:228`), whose members are plain
  `handle`/`spell`/`target`/`hand`/`conc`/`stopPct`/`refreshed`. Heal and hostile casts are
  mutually exclusive per tick by `CasterConsent::SpellKind`, never concurrent
  on one follower, but kept distinct to avoid cross-talk. AT THE TIME (2026-09-05,
  same day): also structurally separate from offense's `ClaimCasting` (still
  `kIntent_SelectSpell`, ch.8) — the two facets shared NO channel, unlike PASS
  E's brief overlap. **SUPERSEDED shortly after by PASS G below (feat/offense-
  cast-seats, same day): offense ALSO moved onto `kIntent_Cast`**, riding the
  SAME facet as this heal claim via its OWN distinct `offenseHandle` slot —
  still never concurrent, never cross-talking, just sharing one underlying
  mechanism instead of two.
  `EnsureCastClaimLocked` (`APMFBridge.cpp:470`, anon ns, RENAMED from `EnsureHealClaimLocked`
  by PASS G — it was always generic) create-or-REFRESHES: unlike PASS
  E's `Repoint`, `RequestCast`'s rich payload has NO in-place re-point, so a
  CHANGE in `(spell, target, hand, concentration, stopPct)` releases the old
  claim and requests a fresh one (a new bounded window); the SAME values are
  still a cheap no-op refresh. `ClaimHealCast` requires `Config::
  g_healAnimPackage` + APMF present + `api->abiVersion >= 5` (`RequestCast` is
  a v5 slot — logged-once warn + clean degrade to kInstant on an older
  APMF.dll, never a crash). `a_hand` is always `kApmfHandLeft` (2,
  `APMFBridge.h:102`) — see the HAND FIX below, unchanged in policy from PASS
  E/D, just re-expressed as `APMF_API::kCastFlag_LeftHand` in `req.flags`
  instead of `ival` bits (which are gone with the retired drive). NEW this
  pass: `a_concentration` sets `kCastFlag_Concentration` (TTL floor for a held
  stream) and `a_stopPct` (0..100, `APMF_API::MakeStopPct`) tells seat 0x07
  `CheckStopCast` where to end a concentration channel instead of always full
  restoration — see `Actuation_Direct.cpp`'s `CastAuto` entry below for the one
  real threshold source. `req.ttlMs` is `APMFBridge::kHealCastTtlMs`
  (`APMFBridge.h:543`, 6000) — the SAME constant `ComposedCast.cpp`'s
  `kHealBoundsTtlMs` now aliases, so the `RequestCast` window and the
  `CastBounds::Arm` window can never drift apart.
- **HAND FIX (2026-09-05, deck: heal driven right hand, then displaced by the
  equip gambit's own re-equip ~500ms later, cast never left "rest").** Still
  the live policy under PASS F, just re-expressed on the new payload.
  `ClaimHealCast` used to pass `a_hand=0` (auto); APMF's auto resolution prefers
  the RIGHT hand when it reads free, and while a follower was transiently
  unarmed (the SAME facet-expiry bug below briefly drops the equip claim) auto
  grabbed the right hand — then the equip gambit's own periodic re-equip put
  the melee weapon right back into that hand, displacing the spell. THE RULE:
  whenever an equip gambit is actively force-holding a weapon
  (`APMFBridge::IsEquipmentClaimActive`), that weapon owns the RIGHT hand, so a
  spell must claim LEFT rather than contest it; LEFT is also the correct
  fallback with no weapon held (heals are left-hand almost always regardless).
  `ComposedCast::Try` (`ComposedCast.cpp`) now passes `APMFBridge::
  kApmfHandLeft` unconditionally, forwarded into `req.flags` as
  `kCastFlag_LeftHand` — UNCHANGED, on purpose: heals bypass the intelligent
  hand pass below entirely (heal is left, regardless).
- **`WeaponHandActive`** (`APMFBridge.cpp:1052`, decl `APMFBridge.h:347`,
  2026-09-06) — the canonical "does a weapon own this hand" signal for cast
  hand-selection: `Loadout::Read(follower, nullptr).grip` (a weapon equipped
  RIGHT NOW) OR `IsEquipmentClaimActive` (an equip gambit about to reassert
  one — the race the HAND FIX above closes). Works with or without APMF
  present (the `Read()` half needs no claim). Not yet called from this
  file's own `ClaimHealCast` (heal's hand is unconditional, see above); the
  intended consumer is a future offense hand-claim.
- **`Loadout::HandPick`/`PlanCastHand`/`CanDualCast`** (`Loadout.h`/`.cpp`, see
  that file's MAP entry, §4) — marth's full intelligent hand-selection POLICY
  now EXISTS (weapon-active → Left; both-hands-free single-spell → DualCast
  when the follower's REAL dual-cast perk + magicka afford it, else
  EitherFree for the caller to assign, including a second "juggle" spell).
  **The claim SHAPE gap is closed (2026-09-06):** `APMF_API::kCastFlag_DualCast`
  (bit 3) exists, `APMFBridge::HandFor(HandPick)` (`APMFBridge.h`) translates a
  `PlanCastHand` result into this bridge's `a_hand` encoding (`kApmfHandLeft`
  / new `kApmfHandDualCast` / 0), and `EnsureHealClaimLocked`'s `req.flags`
  build sets `kCastFlag_DualCast` INSTEAD OF `kCastFlag_LeftHand` for
  `kApmfHandDualCast` (never both bits together). A `[cfc]` log line
  distinguishes "asked for dual, claim granted" from "asked for dual, claim
  REFUSED outright" (silence = never asked). **NOW WIRED (integration/2026-09-06
  then feat/per-hand-cast-slots, 2026-09-06)** — superseded the "still NOT
  wired" state this entry used to describe: `Actuation.cpp`'s `CastOn`
  (`ownedCast` branch, ch.8b `ClaimOffenseCast`) consults `PlanCastHand`
  (via the per-hand cast-gambit lock's `ResolveCastHand`, see this file's
  `Actuation.cpp` entry) and passes the resolved `HandPlan` through as
  `ClaimOffenseCast`'s `a_hand` — `HandFor` is no longer the last step for this
  call site (it cannot express an already-juggled concrete Right; `CastOn`
  builds the `a_hand` value directly from `HandPlan`) but still documents the
  raw `HandPick` → `a_hand` mapping for the Left/DualCast cases. Supersedes
  ([[mage-dualcast-diff-hands-perk-gated-todo]]) as the design; that backlog
  note's WIRING half is now closed for offense (assassin/dual-wield work is a
  separate, later backlog item).
- **FIELD BUG FOUND + FIXED (2026-09-05, deck: claim/release every ~530ms,
  caster stuck at rest forever — `feat/heal-claim-hold`; RE-PROVEN the SAME day
  on the weapon-order equipment claim, Cicero deck capture, `feat/facet-expiry`).**
  `healHandle` used to share the flat 500ms `kExpiry` backstop with every other
  facet in this file. **The comment here used to assert this was "fine for
  spellHandle/targetHandle (refreshed on a tight combat-thread cadence)" —
  THAT WAS WRONG, disproven in the field the same day:** `ClaimCasting`/
  `ClaimCombatTarget` (`Actuation::CastOn`) and `ClaimEquipment`
  (`Actuation::ReconcileForcedWeapon`) are ALL refreshed from inside the SAME
  per-follower `Scheduler::Tick` ROUND-ROBIN lap the heal claim uses — ONE
  follower serviced per ~133ms (`Scheduler.cpp`), so a given follower's own
  gambit only re-fires (and re-Claims/Repoints/refreshes) every
  ~0.133s × partySize. For anything but a 1-2-follower party that gap already
  exceeds 500ms, so `Tick()`'s sweep released a live, still-wanted claim every
  round-robin lap — first observed on the heal claim, then on the equipment
  claim (Cicero: `CLAIMED gate-only` → APMF-side `RELEASED` 919ms later, a
  full round-robin lap early, while MFO's own `g_forcedWeapon` force-hold was
  still standing — proven the sweep and NOT a deliberate release because
  `ReleaseEquipment` is only ever called from `Actuation::ReleaseForcedWeapon`,
  which ALSO logs `"force-hold released"` in the SAME call — that log appeared
  540ms LATER as a separate event, so the earlier APMF-side release could only
  have come from `Tick()`'s expiry sweep). User-visible symptom: a follower's
  APMF-side equipment GATE lapsing mid-hold let something else (the AI's own
  idle/holster behavior, or a competing custom-AI framework's own equip logic
  under APMF arbitration) interfere with the weapon while MFO's own force-hold
  was still nominally up, and since `ReconcileForcedWeapon` (`Actuation.cpp:2106`) never re-asserts
  the physical equip (only the claim) between `EquipWeapon`'s one-shot fires,
  a follower stuck in that gap the moment reads as "standing around unarmed
  until the next fight re-fires the equip gambit." Fixed with a dedicated
  `FacetExpiry()` (`APMFBridge.cpp:332`, decl `APMFBridge.h:81`, renamed from `HealExpiry()`)
  sized the SAME way `TargetCastReconcile`/`SelfCastReconcile` already size
  their own round-robin-aware release windows (`suppress*1.12 +
  0.133*partySize + 0.5`, floored at the old 500ms) — `Tick()`'s
  (`APMFBridge.cpp:1289`) per-claim expiry checks (the `CastClaim` slots plus
  `targetHandle`/`equipHandle`) all compare against `FacetExpiry()` now, not
  the flat `kExpiry`. package-offer
  (`packageHandle`) stays on the flat `kExpiry` — VERIFIED (not assumed) it is
  genuinely refreshed every ~133ms flat, unconditionally, for every active
  loot slot, by `Packages::Pump()` (not gated by the round-robin cursor).
  combat-action-deny (`actionHandle`) also stays flat — it has NO current
  caller anywhere in the tree (still "built but not wired", see PASS B above),
  so there is no live refresh cadence to size against; revisit when it is
  actually wired. Reads `Followers::g_active.size()` from `Tick()`/the
  claiming worker context, same as the Reconcile functions already do from the
  identical `SleeperLoop` `AddTask` body (`Diagnostics.cpp`) — safe by the same
  serial-job-worker reasoning (#4), not a new access pattern. **Second bug,
  same symptom (heal claim only):** `Logistics.cpp`'s OOC concentration branch
  (`ServiceFollower`'s `for (pass < 2 && !acted)` dibs-tier loot-order wrapper)
  `break`'d its inner scan on a delivered `CastTargetDirect` Applied without
  setting `acted = true`, so pass 1 re-ran the WHOLE scan and re-fired the same
  already-delivered cast a second time this tick (double `"[logistics] ... OOC
  concentration ..."` log line, double `ClaimHealCast` call — `CastTargetDirect`
  returns `Applied` unconditionally, unpaced, the instant `ComposedCast::Try`
  claims, so the 2nd call was a real 2nd claim call, not just a log dupe).
  Fixed: `acted = true` before the `break`, matching the `selfPkg`/`immediate`
  branches beside it. The IN-COMBAT concentration path (`Scheduler.cpp:552`'s
  single-pass combat scan) shares the SAME `CastTargetDirect`/`ComposedCast::Try`
  call (so it got the `FacetExpiry()` fix for free) but never had the
  double-fire bug — its scan has no pass-0/pass-1 wrapper, `stopped=true;
  break;` ends it after one Fire.
- **PASS G (ch.8b `kIntent_Cast`/`RequestCast`, feat/offense-cast-seats,
  2026-09-05): offense PORTED off ch.8 `kIntent_SelectSpell` onto the SAME
  facet PASS F ported heal onto.** `ClaimOffenseCast`/`ReleaseOffenseCast`
  (defs `APMFBridge.cpp:902`/`:977`) REPLACE the retired
  `ClaimCasting`/`ReleaseCasting` (`kIntent_SelectSpell`, ARBITRATE+DENY only --
  the follower's own AI still picked whichever spell IT wanted, the
  long-standing "target right, spell wrong" defect,
  [[cast-gambit-spell-choice-not-enforced]]). Now, while the claim stands,
  APMF's FIVE engine seats (the SAME ones PASS F wired for heal) drive the
  AI's own selection/equip/charge/aim/fire/channel of EXACTLY the claimed
  spell at EXACTLY the claimed target -- closing the defect. Own `g_owned`
  slot. **STRUCTURE, CORRECTED 2026-09-07 — this used to name
  `offenseHandle`/`offenseSpell`/`offenseTarget`/`offenseHand`/`offenseConc`/
  `offenseStopPct`/`offenseRefreshed` at `APMFBridge.cpp:111-114`. NO SUCH FIELDS
  EXIST** (`:111-114` is the `everLive` comment). The real shape: `Owned` holds
  `CastClaim heal` (`APMFBridge.cpp:228`) and `CastClaim offense[2]` (`:239`,
  [0]=left/[1]=right); the per-claim state — `handle`/`spell`/`target`/`hand`/
  `conc`/`stopPct`/`refreshed`/`created`/`proxy`/`everLive` — lives on `CastClaim`
  itself (`conc`/`stopPct` at `:87-88`). So offense is
  a DISTINCT claim slot from the heal one (Task 3's invariant preserved),
  sharing its create-or-refresh plumbing via the RENAMED, now-generic
  `EnsureCastClaimLocked` (`APMFBridge.cpp:470`, was `EnsureHealClaimLocked` -- the function
  was always generic over its handle/out-params). Sole call site:
  `Actuation::CastOn`'s `ownedCast` branch (`Actuation.cpp:1036`, in the pre-flight) always
  passes `hand=kApmfHandLeft` (matches `Loadout::Prepare`'s own
  `LeftHandSlot()` equip target exactly -- no weapon-state branching needed,
  since MFO's offense equip has ALWAYS targeted LEFT regardless of grip),
  `concentration=false` (concentration forks off earlier in `CastOn`, to
  `ConcentrationCast`'s DIFFERENT direct-force delivery model -- untouched by
  THIS pass, but see PASS H below, feat/cast-gambit-concentration: that model
  now ALSO calls `ClaimOffenseCast` with `concentration=true` from a sibling
  call site), `stopPct=0` (a heal-only concept). **`IsOwnedCastActive`
  REPOINTED** (same name, same call sites -- `CasterConsent.cpp`'s exclusivity
  denies, `CombatStyle.cpp`'s equip gate -- now backed by `offenseHandle`
  instead of the retired `spellHandle`) -- still correct because a live
  `kIntent_Cast` claim ALSO answers the 0x0F `CheckShouldEquip` seat, covering
  what those standdowns relied on ch.8 for. **`CasterConsent.cpp`'s
  `ClientCastClaimed` EXTENDED** (`:231`) to OR in `IsOwnedCastActive` alongside
  `CastBounds::Live`/`IsHealCastActive` -- the SAME HARD-ABORT protection
  heal's port needed now covers offense too (its two now-redundant standalone
  `IsOwnedCastActive` checks, `:704` and `:999`, are UNREACHABLE in practice
  once the early-pass returns first -- left as defense-in-depth, not removed).
  **DEGRADE vs FAIL-CLOSED — CORRECTED 2026-09-07.** This bullet used to be
  labelled **DEGRADE** and said a refused claim (including an *arbitration loss*)
  "makes `CastOn` fall through to the legacy AI-first-grace + force-on-miss
  hybrid … never a silent cast drop". `fix/mfo-no-decline-fallback` removed that,
  and it was the exact decline-fallback the APMF SHOWPIECE PRINCIPLE forbids. The
  two halves are now asymmetric:
  * **DEGRADE-WHEN-ABSENT** — the channel is not there at all
    (`APMFBridge::OffenseCastClaimSupported()` false: APMF absent, ABI < 5,
    `bApmfCast` off), or `bLegacyCastHybrid` is ON → the ORIGINAL AI-first-grace +
    force-on-miss hybrid, byte-identical to pre-APMF. A documented contract.
  * **FAIL CLOSED** — APMF present AND capable AND it REFUSED (an arbitration
    loss lands HERE, not above; and since `fix/mfo-fourstate-followups` it returns from the
    PRE-FLIGHT, before any hand is touched — see "THE ASK RUNS BEFORE THE EQUIP (F3-2)") →
    `Actuation.cpp:1039-1052` returns
    `{FailedSkill, "APMF refused the cast claim", transparent}` with a
    rate-limited `[apmf] … APMF REFUSED …`. The cast IS dropped, deliberately and
    loudly; nothing routes around APMF. `transparent` keeps the rules below it
    running, so it is a visible non-cast, not paralysis.
  See the "Gating (NO DECLINE-FALLBACK…)" entry and
  `Docs/CAST-DELIVERY.md`'s DEGRADE PATH block, which this bullet used to
  contradict. **Diagnostic reused, not duplicated:** the `[cfc]`
  silent-claim watch (`ComposedCast.cpp`'s `g_watch`/`WatchArmed`, previously
  private to `Try()`) is exposed as `ComposedCast::WatchClaim`/`ClearWatch`
  (`ComposedCast.h:~116-131`) so `Actuation::CastOn` can arm the SAME watch on
  a live offense claim without routing through `Try()` (which stays
  HEAL-ONLY-gated, unchanged) -- `Scheduler.cpp`'s `!castSeen` release and
  `Followers::OnFollowerRemoved`'s dismissal teardown both clear it alongside
  `ReleaseOffenseCast`. See `Docs/CAST-DELIVERY.md`'s "OFFENSE CAST PORT"
  section for the full writeup.
- **PASS H (feat/cast-gambit-concentration, 2026-09-06): a non-heal
  CONCENTRATION stream now reaches the engine-seat path, + the firing-spell
  gambit LOCK.** Two independent fixes; full writeup in `Docs/CAST-DELIVERY.md`'s
  "CONCENTRATION CLAIM PORT + THE FIRING-SPELL GAMBIT LOCK" section.
  (1) `CastSelfDirect`/`CastTargetDirect` (`Actuation_Direct.cpp`), right after
  their existing `ComposedCast::Try` call (which stays HEAL-ONLY, untouched —
  owned by a parallel change), now call `APMFBridge::ClaimOffenseCast` DIRECTLY
  with `concentration=true, stopPct=0` when `kind != Heal` and the spell is
  `kConcentration` -- the exact "FUTURE call site" PASS G's own doc predicted
  and left unwired. Absent/off/SE/ABI < 5 degrades to the SAME direct-force
  stream that already ran, byte-identical; a REFUSAL from a present, capable APMF fails
  closed instead (`fix/mfo-no-decline-fallback`, 2026-09-07). (2) `Actuation.cpp` gained a
  cast-gambit lock (`g_castLock`/`g_lastLockLog`, anon-namespace, file-local)
  that `CastOn` consults right after the range/competence gates before letting
  a DIFFERENT `(spell,target)` than one already occupying the follower touch
  equip/consent/claims -- held off (transparent `NoOp`, rate-limited `[eval]`
  log) while the lock says the old one is still live.
  **PER-HAND (feat/per-hand-cast-slots, 2026-09-06): re-keyed off ONE
  `CastLock{spell,target,lastSeen}` per follower onto `FollowerCastLocks{CastLock
  hand[2]}` (index 0=left, 1=right) — the field problem this closes: a heal
  (always LEFT) and an offense spell serialised onto ONE hand while the
  follower's OTHER hand sat idle (36 rules fired, 22 claims made). `CheckCastLock`
  is RETIRED, replaced by `ResolveCastHand(follower, HandPick, spell, target,
  HandPlan&)` — the SAME gate, now hand-aware: self-target/concentration/
  Heal-Buff are forced `HandPick::Left`; an Offense spell at a real target
  passes its `Loadout::PlanCastHand` result through. `ResolveCastHand`
  implements THE JUGGLE per `HandPick`: `Left` checks hand 0 only; `EitherFree`
  tries hand 0 then hand 1 (`HandFree`, which treats a live-false/stale lock as
  free and drops it in place); `DualCast` requires BOTH hands free-or-
  refreshing-the-same-spell at once or holds off entirely (never a half-landed
  dual-cast — CLAUDE.md principle 7). `CastLockLive(follower,hand,lock)` checks
  `APMFBridge::IsOwnedCastActiveOnHand(follower, hand)` (NEW) for that ONE
  slot — ORed with `IsHealCastActive` for hand 0 only (heals are always LEFT) —
  else falls back to `APMFBridge::FacetExpiry()` (REUSED, #9) as the
  round-robin-aware staleness window, same as before. `CastOn` resolves
  `handPlan` ONCE (right after the gate) and reuses it via a `lockHands` lambda
  at every `HoldCastLock`/`ClaimOffenseCast`/`ComposedCast::WatchClaim` call
  site in the function, so the lock and the actual APMF claim never disagree on
  which hand(s) they occupy; `ConcentrationCast` (a separate function) hardcodes
  hand 0, matching its own always-LEFT delivery. Released by
  `Actuation::ClearCastLock(id)` (erases BOTH hands for that follower — wired
  into `Scheduler.cpp`'s `!castSeen` release AND its out-of-combat teardown
  block, and `Followers::ReleaseHeldState`'s dismissal teardown) -- and in bulk
  by `Actuation::ClearCastLocks()`, called from `Actuation::ClearSelfCasts()`
  (the existing `Serialization.cpp` revert call site; no new one added).
  `APMFBridge::FacetExpiry()` (`APMFBridge.cpp:332` was anon-namespace-private;
  MOVED to external linkage + declared in `APMFBridge.h` for this cross-TU
  reuse -- pure visibility change, same formula, every in-TU caller unaffected).
  **Scope, deliberate:** the legacy AI-first-grace + force-on-miss hybrid and
  `CastAuto`'s sequential-most-hurt heal fan are NOT covered by the lock (own
  protections already; see the Docs section for why).
- **CAST-CLAIM OBSERVABILITY (ABI v6, feat/consume-cast-observability, 2026-09-06):**
  `APMF_API.h` bumped to `kABIVersion = 6` (append-only `APMF_API_v6 : APMF_API_v5`,
  mirrored byte-identically, never hand-edited) adding two read-only queries:
  `GetCastProxy(Handle)` (the delivery-flip proxy FormID APMF minted for a claim, or
  0) and `IsClaimLive(Handle)` (whether APMF still holds it — it auto-expires at its
  own TTL with no notice to the client). Closes two blind spots diagnosed the same
  day: (a) MFO could only ever match a cast's ORIGINAL spell, so a claim whose
  delivery APMF flipped through its own proxy (e.g. a heal-other) landed as a cast of
  the PROXY and was filed as "their own spell, not ours" — every `[cfc] ... NO
  observed cast` warning on that claim was a false alarm; (b) `EnsureCastClaimLocked`'s
  unchanged-claim fast path (`APMFBridge.cpp`) trusted a stored handle blindly and
  returned early, so once APMF auto-expired a claim MFO kept believing it was still
  live and never re-requested (field: 23s of re-fires, zero `RequestCast` reaching
  APMF). Fix, guarded `abiVersion >= 6` throughout (ABI < 6 = byte-identical
  degrade): `EnsureCastClaimLocked` fetches `GetCastProxy` right after a successful
  `RequestCast` and stores it on `CastClaim::proxy` (new field); the unchanged-claim
  fast path now calls `IsClaimLive(c.handle)` before returning — `false` resets the
  claim to empty and falls through to the SAME fresh-`RequestCast` code the
  "values changed" branch already used, rather than inventing a new re-claim loop.
  Two new accessors expose the proxy read-only: `APMFBridge::GetHealCastProxy`/
  `GetOffenseCastProxy`. `ComposedCast`'s silent-claim `Watch` struct
  (`ComposedCast.cpp`) gained a `proxy` field alongside `spell`;
  `WatchArmed`/`WatchClaim` take a new `a_proxy` parameter (every call site —
  `ComposedCast::Try`, `Actuation.cpp`'s owned-cast branch,
  `Actuation_Direct.cpp`'s two concentration-offense claims — fetches it via the new
  accessors and forwards it); `ExpectingCast`/`NoteObservedCast` now match on spell
  OR proxy. `Diagnostics.cpp`'s `SpellSink` "ours" check (the `*** MFO GAMBIT
  SPELL ***` log tag, previously `g.actionParamForm == spellID` only) now also
  counts a hit via `ComposedCast::ExpectingCast`. **The diagnostic is not weakened**
  — a claim that produces neither the spell's nor the proxy's cast still warns
  exactly as before. See `Docs/CAST-DELIVERY.md`'s "CAST-CLAIM OBSERVABILITY"
  section for the full writeup.

### LOOT TRAVEL ROAD 2 — APMF ch.19 `kIntent_Travel` (A/B, `test/mfo-loot-travel-via-ch19`, NOT on main)
An A/B alternative to the ch.9 road documented immediately below, behind the INI-only
`bLootTravelViaApmfTravel` (`Config.h`, **DEFAULT 0 = the ch.9 road, which is the CONTROL**).
With it ON, `Logistics_Loot.cpp`'s dispatch edge claims APMF's ch.19 travel facet
(`APMFBridge::ClaimLootTravel`, arrival radius 128u = the control road's own
`kAPMFTravelRadius`) naming the loot ref as the destination, and APMF points ITS own package
(`Data/APMF.esl`, 8 slots), writes the location, files its own internal ch.9 offer at MFO's
basis, and ends the leg on arrival / the actor entering combat / the destination going away.

**Depended-on-by / what breaks if you change this.**
- **The two roads are MUTUALLY EXCLUSIVE per excursion.** Road 2 never calls
  `Packages::LootTravelFill/Retarget`, so MFO's own package is never pointed and
  `APMFBridge::OfferPackage` is never called — exactly ONE ch.9 package-offer claim exists
  for the follower either way. Breaking that (e.g. calling `LootTravelFill` "as well") gives
  one actor two competing package-offer claims.
- **The release funnel is `Packages::LootTravelClear` + `LootTravelEvictIf`**, which call
  `APMFBridge::ReleaseLootTravelSlot` / `ReleaseLootTravelFor` unconditionally (no-ops on the
  control road). The ch.19 leg table is keyed by LOOT SLOT because `Logistics.cpp`'s sweeps
  pass a NULL actor and only a slot index. APMF ends the WORK it started but never revokes a
  client's claim, so dropping either hook pins one of APMF's eight travel records per leak.
- **`Forms::IsTravelPackage` DOES NOT recognise APMF's ch.19 package**, so on road 2
  `Logistics.cpp`'s `onTravelNow` / `TravelIntent::legEngaged` are permanently false: every leg
  logs `TRAVEL PKG NOT ENGAGED`, takes the theft guard's re-assert branch, and is ABANDONED
  after `kStealGrace` (10 s). Legs needing more than ~10 s of walking therefore fail on road 2
  and succeed on road 1. **A known, reported limitation of the test branch, not a design
  choice** — fixing it needs `Forms.h` + `Logistics.cpp`, outside that branch's file boundary.
- **ABI:** `kIntent_Travel` is APMF ABI v10 and `native/APMF_API.h` is still the byte-shared v9
  mirror. `APMFBridge.cpp` declares the intent value and flag bit LOCALLY and gates on the live
  `api->abiVersion >= 10`; v10 adds no struct field and no function-pointer slot, so the v9
  mirror stays ABI-correct. Re-mirroring that header is its own step.

### Packages.cpp — APMF LOOT-TRAVEL (ch.9 0x49 route, PASS B, the Cicero fix)
`LootTravelFill/Retarget/Clear/EvictIf` (`:1380-1710`, see the OPTION A entry above) now ROUTE
THROUGH and COMMIT TO `APMFBridge::OfferPackage` whenever APMF is present (`Available() &&
bApmfLootTravel`), instead of the alias/static-priority-60 race: claims the package-offer facet
naming `Forms::APMFLootTravelPackage(slot)` (4 NEW packages, `Forms.h` `kAPMFLootTravelPackage0-3`
= `0x836-0x839`, `MFO_GenerateESP.py make_apmf_loot_travel_package()`) so APMF's 0x49 hook hands
the follower that package directly **on every nudge while the claim is published** — no alias
fill, no arbitration race, so a follower package-locked by an outranking custom AI framework
(Cicero) still gets walked to the loot. **The hook does NOT redirect on the engine's own
cadence** (`Docs/DIAG-2026-09-06-loot-travel.md` §2: all 16 field wins reconcile to explicit
nudges; the engine's own re-evaluation contributed zero), so a claim that is published but never
nudged again does nothing — and a nudge that runs BEFORE the claim is published does nothing
either, which is exactly RC#1 of that DIAG (0 of 6 dispatches engaged the travel package). **APMF SHOWPIECE PRINCIPLE (marth, `Docs/STATUS.md`): with APMF present, MFO is COMMITTED
to it — a decline on the APMF path (unresolved record, layout-guard, lost arbitration) FAILS
CLOSED (logged loudly, no dispatch this tick) and NEVER falls through to the alias route.** The
alias route runs ONLY when APMF is entirely ABSENT (or `bApmfLootTravel` is off) — the sole
reason it still exists — so it stays byte-identical **degrade-when-absent**, never a
decline-fallback.
- **WHO NUDGES `EvaluatePackage` ON THE APMF ROUTE (2026-09-07, fix 1):** APMF does, not MFO.
  MFO's inline nudges on the APMF dispatch/release edges (loot `LootTravelFill`/`LootTravelClear`,
  retreat `RetreatFill`/`RetreatClear`) are GONE: they ran BEFORE the claim was drained/published
  to the 0x49 hook (so engage re-evaluated a claimless snapshot and release asserted the package
  at the instant of withdrawal), and they ran on the AddTask job worker — an engine AI write off
  the main thread. APMF's `OfferPackageChannel::Engage/OnOwnerChanged/Release` now posts the nudge
  one hop past its own `Publish` via `apmf::mainthread::Post`. **The ONE survivor on the APMF route
  is `LootTravelRetarget`'s** (`Packages.cpp`, in the `g_apmfSlotActive[a_slot]` branch): a leg
  retarget offers the SAME package form, so `APMFBridge::EnsureClaimLocked` takes its
  unchanged-claim fast path and APMF is never told anything happened — nothing but this call
  re-plans the running package to the rewritten runtime target. It is marshalled through
  `MFO::MainThread::Post` (FormID looked up inside the lambda); no pump (VR) → loud `spdlog::error`,
  never an off-thread fallback. **LEGACY alias-route nudges are untouched and are still inline
  off-worker** (`Packages.cpp` legacy branches of Fill/Retarget/Clear + `RetreatFill/Clear`) — a
  pre-existing condition, deliberately out of scope for that fix; do not "tidy" them without a
  field cycle, `LootTravelClear`'s legacy nudge in particular is ordered before `VerifyDetached`.
- **RUNTIME-HANDLE TARGET (no alias to carry it):** the new packages are authored PLDT type 0
  ("Near Reference", `RE::PackageLocation::Type::kNearReference`) instead of type 8 (alias);
  `SetAPMFLootTravelTarget` (`Packages.cpp` ~`:628`) overwrites `PackageLocation::data.refHandle`
  at runtime via `ReadLocation`, the Location twin of `ReadTarget` (SAME `kPointerOffFromIPackageData`
  offset model, generalised from `PackageTarget` to `PackageLocation`; guarded identically: any
  layout mismatch declines loudly, never a blind write).
- **FIELD BUG FOUND + FIXED (2026-09-03, deck Tuxborn, first Cicero test): `kInputLocation` was
  authored as `"Location"` — WRONG.** `FindInput`'s name→uid lookup (`Packages.cpp:389`) resolves
  the TEMPLATE's own BNAM-declared human parameter name (like `kInputSpell="Spell"`/`kInputTarget=
  "Target"` on the UseMagic template), NOT the ANAM type-string (`kTypeLocation="Location"`,
  ReadLocation's own, correct, unrelated guard). The one-shot both-maps-miss dump
  (`FindInput`'s `DumpNameMap`) read the vanilla Travel template's (`00016FAA`) REAL 3 parameter
  names verbatim off the deck log: **`"Place to Travel"` (uid 0, the Location input)**, `"Ride
  Horse if possible?"` (uid 2), `"Prefer Preferred Path?"` (uid 4) — an exhaustive 3-of-3 miss on
  `"Location"`, so `FindInput` returned null EVERY call and the runtime write never ran (never
  reached `ReadLocation`/the `PackageLocation` write at all — logged, not a crash, per the guard
  discipline). Fixed: `kInputLocation = "Place to Travel"sv`. **The offset-model/`GetTypeName()`
  risk on the write itself is THEREFORE STILL UNVERIFIED** — this fix only clears the parameter-
  name miss standing in front of it; the NEXT Cicero field test is the first one that actually
  exercises `ReadLocation`/the `PackageLocation::data.refHandle` write.
- **Per-slot bookkeeping** (`g_apmfSlotActive`/`g_apmfSlotFollower`, file-local, never serialized —
  the claim itself is runtime-only) tracks which of the 4 slots is APMF-routed so Retarget/Clear/
  EvictIf touch the right mechanism, and who to release when a caller clears by slot index alone
  (`LootTravelClear`'s `a_follower` is optional). Swept on `ReleaseAll` (every load/revert) exactly
  like `g_travelSlots` self-heals in `Logistics.cpp`.
- **PUMP REFRESH IS LOAD-BEARING:** `Packages::Pump()` (`:1176`) now refreshes every active
  APMF-routed slot's claim UNCONDITIONALLY, first, before its own cast-holder early-return — a
  multi-second walk needs a keep-alive well under APMFBridge's 500ms expiry backstop, and
  Fill/Retarget only touch the claim at excursion-start/leg-boundary, not every tick mid-walk
  (the exact starvation lesson from the owned-cast dedupe-latch revert above, applied up front
  instead of discovered the same way).
- **`Forms::IsTravelPackage`** now recognizes all 8 packages (4 alias + 4 APMF) — load-bearing for
  the PACKAGE-THEFT guard (`Logistics.cpp:917-974`): a follower legitimately running his
  APMF-delivered package must read as "on the travel package", never as "stolen" (0x49 wins on
  every nudge while the claim is published — but NOT on the engine's own cadence, so a claim
  nobody re-nudges can be displaced; the guard must not misfire and churn regardless).
- **`ClaimCombatActionDeny` is NOT wired here** (see APMFBridge.cpp above) — left available, not
  scoped into this dispatch, per marth's "keep it scoped" instruction and the existing
  concede-to-combat design.
- **APMF RETREAT (ch.9 0x49 route, 2026-09-03), the same PASS B conversion applied to
  `RetreatFill/Clear/EvictIf` (`Packages.cpp:1848-2021`) — shared by BOTH callers, the
  `act.flee` gambit (`Actuation.cpp:1969`, `Vocab::kActFlee`) and the opt-in
  auto-retreat leash safety (`Scheduler.cpp:462`).** Same showpiece/commit principle as
  loot-travel: `Available() && Config::g_apmfRetreat` routes through `APMFBridge::
  OfferPackage`/`ReleaseOfferPackage` naming `Forms::g_apmfRetreatPackage`
  (`kAPMFRetreatPackage = 0x83A`, `MFO_GenerateESP.py make_apmf_retreat_package()`) —
  fails CLOSED on any decline, never falls back to the alias route. **Only ONE new
  record needed** (not 4 like loot's slots): retreat's destination is ALWAYS the
  player, so there is no per-follower runtime-target collision — `SetAPMFLootTravelTarget`
  (the SAME generic Location-writer loot-travel uses) is reused verbatim, called once at
  engage with the player ref as a defensive reassertion (the authored FREF_PLAYER
  placeholder is already the permanent correct value for retreat specifically). Single-
  holder semantics UNCHANGED from the legacy probe (`g_retreatHold`, ONE retreat at a
  time — a second `RetreatFill` while one is held is declined, same as before); the
  route choice is tracked by a NEW `RetreatHold::viaAPMF` bool (no alias exists to query
  under the 0x49 route, unlike loot's `g_apmfSlotActive` per-slot array — retreat only
  ever has one hold, so one bool suffices) consulted by `RetreatClear`/`RetreatEvictIf`/
  `ReleaseAll` to pick the right release mechanism. `Pump()` refreshes the claim
  unconditionally every worker tick (same 500ms-expiry keep-alive loot-travel needs —
  Scheduler's per-tick `StopCombat` while falling back does NOT touch this claim).
  `Forms::IsRetreatPackage` (mirrors `IsTravelPackage`) recognizes both the legacy alias
  package and `g_apmfRetreatPackage` — load-bearing for `Scheduler.cpp:419`'s `note.took`
  measurement (would silently never latch under the APMF route without it). `bApmfRetreat`
  (`Config.h`, default ON, inert without APMF) is the retreat-specific A/B kill switch,
  same pattern as `bApmfLootTravel`/`bApmfCast`. kIgnoreCombat (`0x00102000`) preserved on
  the new package — retreat is the one Travel use that must win THROUGH combat, opposite
  of loot-travel's plain preferred-speed flag.
- **HARD MUTUAL-EXCLUSION GUARD (2026-09-03): retreat and loot-travel share ONE
  per-follower `APMFBridge::OfferPackage` handle** (`APMFBridge.cpp`'s `g_owned` map is
  keyed by `FormID` alone, not by intent+slot) — if a follower ever held BOTH a live
  loot-travel excursion and a retreat hold, the two claims would stomp that single
  handle every `Pump()` tick (loot's slot-refresh loop and the retreat refresh both call
  `OfferPackage` unconditionally, unaware of each other). Belt-and-suspenders, retreat
  wins (the disengage is the more urgent directive): `RetreatFill`'s APMF branch
  (`Packages.cpp:~1911`, right where `g_retreatHold.actorID` is set) calls
  `LootTravelEvictIf(fid, "retreat engaging")` to drop any live loot claim BEFORE
  finalizing the retreat hold; `LootTravelFill`/`LootTravelRetarget` (`Packages.cpp:1548`,
  `1633`) reciprocally early-return `false` while `g_retreatHold.actorID` names that same
  follower, covering the reverse race. `LootTravelEvictIf` gained an `a_why` parameter
  (default `"dismissed"`, its original/only caller `Logistics.cpp:1725` unaffected) so the
  retreat-triggered eviction logs distinctly from the roster-dismissal one. **This closes a
  window that was live in this SAME change** (retreat never touched `OfferPackage` before
  this pass — see the paragraph above — so the collision could not occur on any
  previously-shipped code; it existed only between this pass's retreat-APMF commit and
  this guard, gated on the scheduler's combat/logistics table split which SHOULD keep
  loot and retreat mutually exclusive but is not a hard guarantee — see `Scheduler.cpp`'s
  "THE TWO TABLES NEVER INTERLEAVE" note, `§4.8`).
- **ANIMATED HEAL via the forced-cast package — DELETED (2026-09-04, the Composed
  Forced Cast rework, `Docs/SPEC-FORCED-CAST.md` §3).** `HealAnimFill`/
  `HealAnimHolds`/`HealAnimEvictIf` + `g_healAnimMap` + `IsHealSpell`/
  `SetPackageSpell` are GONE from `Packages.cpp`/`Packages.h`, and
  `make_apmf_heal_packages()` is gone from `MFO_GenerateESP.py`. The two UseMagic
  records `MFO_APMFHealSelfPackage`/`MFO_APMFHealPlayerPackage` (`Forms.h`
  `kAPMFHealSelfPackage_RETIRED = 0x83B`, `kAPMFHealPlayerPackage_RETIRED = 0x83C`)
  stay reserved forever, never recycled (#41) — the ESP records are dead, the
  generator no longer emits them. The package-substitution route is replaced by
  the `ComposedCast` executor below: a heal is a cast facet now, not a package
  facet. `Config::g_healAnimPackage` (INI key `bHealAnimPackage`, KEPT — frozen
  MCM identity, #5) is REPURPOSED as `ComposedCast`'s master enable; see that
  entry and `Docs/CAST-DELIVERY.md`'s "COMPOSED FORCED CAST" section.

### CastBounds.cpp / CastBounds.h — the MFO-executed-cast bound (§2 HARD-ABORT fix)
Lock-free 8-slot `{atomic key, atomic expiry}` registry answering "is (actor,
spell) an MFO-EXECUTED, bounded cast right now" — the general form of the legacy
`Packages::StreamLive`/`g_liveStream` one-slot contract, read by `CasterConsent`
on the COMBAT thread (see that entry's "MFO-executed-cast early-pass" note below).
- `Arm(RE::FormID, RE::FormID spell, RE::FormID proxy, std::uint32_t ttlMs)`
  (`CastBounds.cpp:71`) — writer, worker OR main thread. Registers (actor, spell)
  and, if `proxy != 0 && proxy != spell`, (actor, proxy) too, both expiring at
  `now + ttlMs`. Idempotent per key (re-Arm refreshes the expiry, never grows the
  table); overflow evicts the soonest-to-expire slot. Called from
  `ComposedCast::Try` (spell key, pre-arm) BEFORE the hand is touched, then re-armed
  with the real proxy key on the main thread inside the drive (`PhaseSelect`).
- `Disarm(RE::FormID actor)` (`:79`) — clears EVERY slot the actor owns (both spell
  and proxy keys). Called on every `ComposedCast` exit (`Try`'s refused-claim
  branch; `End`), by `Actuation_Direct.cpp:928` beside `ConcProxy::Reset()`, and
  by `Followers.cpp:324` (`ReleaseHeldState`, dismissal path, beside
  `APMFBridge::ReleaseHealCast` — replaces the deleted
  `Packages::HealAnimEvictIf`).
- `Live(RE::FormID actor, RE::FormID spell)` (`:91`) — the COMBAT-THREAD reader.
  Lock-free (relaxed/acquire atomic loads only, no engine call); a torn, cleared,
  or expired slot reads false (fail-safe toward deny, never toward a false
  permit). Consulted (via `CasterConsent.cpp`'s `ClientCastClaimed` helper,
  `:222`) by `ConcUnboundedDeny` (`:244`), the `CheckStartCast` thunk's
  early-pass (`:579`), and `CheckCastThunk`'s early-pass (`:947`, 0x0A).
- `Reset()` (`:104`) — clears every slot. `kPreLoadGame`/revert, called from
  `Actuation_Direct.cpp:928` beside `ConcProxy::Reset()`, NOT independently wired
  into `plugin.cpp` (folded into `ClearSelfCasts`'s existing teardown call site).
- `LiveCount()` (`:111`) — diagnostics only, cheap and lock-free.
- **What breaks:** ONLY relevant to a cast that DELIBERATES through the engine's
  `MagicCaster` state machine (`RequestCastImpl → ... → FinishCastImpl`,
  ENGINE_NOTES §0.13) and so reaches the hooked `CheckStartCast`/`CheckCast`
  thunks — a plain `kInstant` `CastSpellImmediate` apply (`ConcProxy`'s direct
  force) never reaches them and needs no `Arm` at all (ENGINE_NOTES §0.40). As of
  feat/mfo-cast-port (2026-09-05, PASS F) the deliberated cast IS live: APMF's
  engine seats drive the follower's OWN AI through the SAME globally-hooked
  `CheckStartCast`/`CheckCast` thunks regardless of which DLL's seat is
  answering — so `ComposedCast::Try` still `Arm`s BEFORE claiming (neither MFO
  nor APMF makes an engine cast call itself; the AI's own deliberation is what
  reaches the thunks). A claim without a
  matching `Arm`, or an `Arm` without a matching `Disarm` on every exit path, is
  vetoed by `ConcUnboundedDeny`/`CheckCastThunk` exactly as the pre-fix
  HARD-ABORT was (`0002F3B8`/`FF001BA4` deck crash, ENGINE_NOTES §0.40,
  INVARIANTS #76), or leaves a stale early-pass outliving its stream (bounded
  only by TTL — a backstop, not a substitute for disarming).
- **TASK 3 VERDICT (feat/mfo-claim-only-heal, 2026-09-05): KEEP, not retired —
  but its per-spell precision is now PROVABLY REDUNDANT for the heal path
  specifically.** `CasterConsent`'s `ClientCastClaimed` OR's `CastBounds::Live`
  with `APMFBridge::IsHealCastActive`, and the latter is a strict superset for
  every observed case: `ComposedCast::Try` only ever `Arm`s `CastBounds` in the
  SAME call that a heal claim succeeds, and only ever `Disarm`s it in the same
  call/exit path that releases the claim — so whenever `CastBounds::Live` is
  true for the heal path, `IsHealCastActive` is already true too, and the
  broadening that added the latter (2026-09-05 S1 field fix, see
  `CasterConsent.cpp`'s entry above) exists precisely because a per-spell match
  proved fragile where the coarser per-actor one did not. Kept anyway, for two
  reasons neither of which is "it might do something today": (1) it is a
  GENERAL, cheap (lock-free, 8 slots), already-battle-tested primitive — "the
  MFO-executed-cast bound" — documented as outliving any one caller (this
  entry's own header), so a future MFO-side executor that once again drives a
  bounded cast directly (not through APMF) would need this exact mechanism
  back; deleting it now means re-inventing it later. (2) `Docs/CAST-DELIVERY.md`
  and `ENGINE_NOTES §0.40`/`INVARIANTS #76` document it as the fix for a REAL
  field HARD-ABORT crash — removing a crash-class safety net to save ~190 lines
  of dead-weight-today code is the wrong trade (`CLAUDE.md`: "do not delete it
  reflexively"). No code changed here this pass.

### ComposedCast.cpp / ComposedCast.h — the Composed Forced Cast (CFC) shim (feat/mfo-cast-port, 2026-09-05)
THIN SHIM, now over PASS F (`APMFBridge.cpp`'s entry above) — replaces the deleted
`HealAnimFill` package route AND its own former MFO-side hand-drive. Composes a
`kIntent_Cast` claim (`APMFBridge::ClaimHealCast`) + a `CastBounds` arm; APMF's
engine seats drive the follower's OWN AI to equip/animate/deliver the cast
natively, so this module makes NO engine call at all (nor does APMF — see the
PASS F entry). See `Docs/CAST-DELIVERY.md`'s "COMPOSED FORCED CAST (CFC)" section
for the full history (PASS D's raced hand-drive → PASS E's +ACT drive → PASS F's
native seats) and ENGINE_NOTES §0.40.
- `Try(RE::Actor* follower, RE::SpellItem* spell, RE::Actor* target,
  CasterConsent::SpellKind kind, std::uint32_t stopPct = 0)` → **`TryResult`, a
  FOUR-STATE** (`ComposedCast.cpp:198`, enum `ComposedCast.h:104`) —
  `{NotApplicable, Claimed, Held, ApmfRefused}`. TWO orthogonal splits produced it: `Held`
  (Fable SEV-2 2026-09-06) is MFO-internal slot ownership BETWEEN TWO HEALS, APMF never asked;
  `NotApplicable` vs `ApmfRefused` (`fix/mfo-no-decline-fallback` 2026-09-07) is what APMF's
  ARBITRATION answered. The old `Refused` was **renamed** to `NotApplicable` rather than
  re-pointed, deliberately: re-using the name for the opposite meaning would have let every
  call site keep compiling with inverted semantics.
  ← `Actuation_Direct.cpp:886` (in `CastSelfDirect`, `:829`), `:1183` (in
  `CastTargetDirect`, `:1141`) **and `Actuation.cpp:1079`** (`CastOn`'s ally/player
  branch — a third call site, MISSING from this entry until 2026-09-06; MOVED by
  F3-2 out of the equip switch into `CastOn`'s pre-flight, so the ask now precedes
  `Loadout::Prepare`/`CasterConsent::Want` and a refusal leaves no side effects).
  Call sites forward an extra `a_stopPct` (see below);
  replacing the two deleted `Packages::HealAnimFill` call sites historically.
  Gated `Enabled()`: `Runtime::CastPathsVerified()` (the AE bucket or exactly 1.5.97 — the
  former `T#67` mirror, opened 2026-09-15; see Actuation's RUNTIME GATES entry), **HEAL-ONLY** (`kind !=
  SpellKind::Heal` → immediate false — offense/buff never enter this module,
  they stay on the byte-identical AI-fired/kInstant paths), `Config::
  g_healAnimPackage` (repurposed toggle), `APMFBridge::Available()`. Sequence:
  computes `isConcentration` from `spell->GetCastingType()` →
  `APMFBridge::ClaimHealCast(fid, spellID, targetID, kApmfHandLeft,
  isConcentration, stopPct)` — spellID/targetID FORWARDED UNCHANGED (a_spell
  always the gambit's own configured spell; MFO does not inspect delivery,
  does not proxy, does not substitute) → refused → `CastBounds::Disarm` +
  drop hand 0's diagnostic watch (UNCONDITIONALLY — the F1 hold's documented invariant rests
  on it) + **`APMFBridge::HealCastClaimSupported()` splits the outcome**: true →
  **`TryResult::ApmfRefused`** (caller FAILS CLOSED, no kInstant apply, one rate-limited
  `[apmf]` error at the call site), false → **`TryResult::NotApplicable`** (caller degrades to
  kInstant, byte-identical to before). The old unconditional `[cfc] … kInstant apply` info line
  is GONE — it asserted a fallback that no longer happens;
  claimed → `CastBounds::Arm(fid, spellID, APMFBridge::GetHealCastProxy(fid),
  kHealBoundsTtlMs)` (`ComposedCast.cpp:468`) — the third argument was a literal
  `0` until F4 (2026-09-06); passing the claim's minted delivery-flip proxy is
  what lets the MFO-side consent standdown and the watch recognise a cast of the
  PROXY as this claim firing. `kHealBoundsTtlMs` aliases
  `APMFBridge::kHealCastTtlMs`, 6000ms — the SAME window `req.ttlMs` carries
  → arms the silent-claim diagnostic watch (`WatchArmed`, below) →
  **`TryResult::Claimed`** (caller skips its kInstant apply). **`TryResult::Held`**
  is the F1 incumbent hold — its own bullet below. `target` is 0 for a self-cast, else
  `a_target`'s FormID — now LOAD-BEARING on APMF's side (seats 0x0A/0x0D read
  it directly), was RECORD ONLY under PASS E. A Self-delivery spell gambited
  at a non-self target is still APMF's OWN problem to solve (its own
  delivery-flip proxy, `core/CastProxy.h`) — MFO forwards `proxy=0` and never
  inspects/substitutes, same discipline as PASS E, field-proven under it (deck
  APMF.log: `driving left hand -- spell 0002F3B8 cast-as FF001A7D target
  0009BCB0`, Fast Healing/Self correctly proxied onto an ally). MFO still SENDS
  APMF `req.proxy = 0` (`APMFBridge.cpp`'s `EnsureCastClaimLocked`) — F4 changed
  only what MFO READS BACK (`GetCastProxy`) and forwards to `CastBounds::Arm` /
  `WatchArmed`, never what it asks for.
- **STOP-PERCENT (`a_stopPct`, NEW this pass).** Forwarded into
  `ClaimHealCast`'s `a_stopPct` → `req.flags`'s `APMF_API::MakeStopPct` bits —
  tells seat 0x07 `CheckStopCast` to end a CONCENTRATION channel at a whole
  percent (1..100) of the target's PERMANENT actor value, not always full
  restoration. `Actuation_Direct.cpp`'s `CastAuto` (its conc-heal branch,
  `:1154`) is the ONE call site with a real per-gambit threshold in scope
  (the firing rule's own `conditionParam`, already used there to pick the
  neediest target) and converts it to a whole percent; every other
  `CastSelfDirect`/`CastTargetDirect` caller passes the default 0 (stop at
  full), byte-identical to pre-port behaviour.
- **SILENT-CLAIM DIAGNOSTIC (`WatchArmed`/`g_watch`, anon ns, Task 6; PER-HAND
  feat/per-hand-cast-slots 2026-09-06).** NOT a
  delivery watchdog — logs only, never re-fires/re-claims/falls back
  (APMF's INVARIANTS.md #0). `g_watch` is now `unordered_map<FormID,
  FollowerWatch{Watch hand[2]}>` (was one `Watch` per follower) — a heal claim
  (always hand 0) and a concurrent offense claim on hand 1 no longer overwrite
  each other's watch. `Try()` arms/refreshes hand 0's `{spell, proxy, since,
  observed, lastWarn}` record on every successful HEAL claim (`proxy` field
  ADDED feat/consume-cast-observability, 2026-09-06 — see below); `WatchClaim
  (follower, spell, a_hand = kApmfHandLeft, a_proxy = 0)` (PUBLIC, `WatchSlot(a_hand)`
  local to this TU, mirrors `APMFBridge`'s own `OffenseSlot` mapping — kept
  separate since this TU has no access to that anon-ns helper, but the two
  MUST agree) is the same arm for a caller claiming `kIntent_Cast` directly
  (`Actuation::CastOn`'s `ownedCast` branch, via `ClaimOffenseCast`, and
  `Actuation_Direct.cpp`'s two concentration-offense claims) — called
  once per hand the claim actually occupies (both, for a DualCast plan). If
  2s+ pass with no observed cast on that hand (rate-limited to once per 5s),
  logs a `[cfc]` warning naming the follower/spell/hand.
  **`proxy` (ABI v6, feat/consume-cast-observability, 2026-09-06):** every
  `WatchClaim`/`WatchArmed` caller now fetches the SAME claim's delivery-flip
  proxy FormID (`APMFBridge::GetHealCastProxy`/`GetOffenseCastProxy`; 0 on
  ABI < 6 or a claim that minted none) and records it alongside `spell` — a
  claim whose delivery APMF flipped lands as a cast of the PROXY, never the
  original spell, so a spell-only match filed that landing as "not ours" and
  fired a FALSE `[cfc]` alarm on a claim that had genuinely fired (field-
  diagnosed 2026-09-06). `ExpectingCast(follower, spell)` (REVIVED from a
  permanent no-op under PASS E; signature UNCHANGED — `Diagnostics.cpp`'s
  sink has no hand to report) now returns true when EITHER hand's watch
  `spell` **OR** `proxy` matches (`a_spell` here is the sink's OBSERVED
  form); `NoteObservedCast` likewise marks BOTH hands observed on either
  field matching (a spell is never claimed on both hands independently
  outside a DualCast mirror, so this cannot cross-confirm two different
  spells). The diagnostic is NOT weakened — a claim producing neither match
  still warns exactly as before. Both are worker-serial, no lock
  — `Try`/`End` and the sink's call site are the SAME serialized AddTask
  job-worker queue (mirrors `Actuation_Direct.cpp`'s unlocked
  `g_selfCast`/`g_targetCast`).
- **THE F1 INCUMBENT HOLD (`TryResult::Held`) — `fix/mfo-heal-slot-and-proxy`,
  2026-09-06. NEW: nothing in MAP described this machinery before.** The heal slot
  is ONE `CastClaim` per follower (`APMFBridge.cpp`'s `Owned::heal`), and two heal
  rules alternating on it every 1-3s each released the live claim before the
  engine's equip+charge finished, so no heal ever landed
  (`Docs/DIAG-2026-09-06-deny-heal-failures.md` RC1 — that file lives on `main`,
  `88398dd`; this branch predates it, so the path resolves only after merge). `Try()` (`:360-404`) now
  HOLDS the incumbent instead: if hand 0's watch holds a DIFFERENT spell, is
  `!observed`, and `APMFBridge::RefreshHealCastClaim(fid)` says the claim is still
  live, it logs (`LogHealHoldOff`, `:186`, deduped 2s per (follower, spell)),
  records `g_lastHold[fid]` (`HoldRecord`, `:173`) and returns `TryResult::Held` —
  the incumbent keeps its charge window; NOTHING is claimed and NOTHING applied.
  Pieces:
  - `APMFBridge::RefreshHealCastClaim` (`APMFBridge.cpp:1184`, decl
    `APMFBridge.h:719`) — the hold's HEARTBEAT: bumps the same `refreshed` stamp
    `ClaimHealCast` bumps, because the hold path deliberately never reaches
    `ClaimHealCast` and `Tick()`'s `FacetExpiry()` sweep would otherwise release
    the very claim the hold protects (~2.45s at the default `fSuppressWindow`,
    ~0.77s at a legal 0). Bounded twice: APMF's `IsClaimLive` (ABI ≥ 6; **ABI < 6
    returns false outright** — an honest degrade to the pre-F1 thrash, never a
    hold nothing can break, #7), AND the claim-age cap below.
  - `APMFBridge::kHealHoldNeverObservedMs` (`APMFBridge.h`, **4000ms**) vs
    `CastClaim::created` (`APMFBridge.cpp:103`, stamped once at `:734`) — the
    NEVER-OBSERVED cap. **It races `observed` (the `[cast]` SpellSink signal), not
    the fire, and it is sized from the HEAL datum:** the one heal that landed on
    the deck took **2.95s claim-to-observed** (minted 19:59:11.158 → `[cast]`
    19:59:14.110, DIAG RC1); the 2.3-2.5s equip+charge figure quoted elsewhere in
    that DIAG is an **OFFENSE Firebolt** and must not be used here. It is **NOT**
    `kHealCastTtlMs` (6000ms — what it read until 2026-09-06) and **NOT** 3000ms
    (what an offense-sized first cut used, 2026-09-06 → corrected 2026-09-07:
    3000ms would lift while a real heal was still CASTING, whose newcomer claim
    then releases the incumbent's handle — RC1 re-created). The check is
    lap-granular (~1.1-1.3s OOC service lap, DIAG RC2), so the lift lands in
    `[cap, cap + ~1.3s]`. **There is no "provably not going to fire" point in the
    evidence** — the sizing is an explicit trade-off (a held rule waits cap + one
    lap either way; cutting a firing heal is the field failure F1 exists to fix).
    Must stay ABOVE `kSilentWarnAfter` (`ComposedCast.cpp:95`) so the `[cfc]`
    silent-claim warning precedes the lift — `static_assert`ed at
    `ComposedCast.cpp:101` — and below `kHealCastTtlMs` so it bites before the
    claim it guards dies on its own. **UNCHANGED by `fix/mfo-spell-authority-0922`**
    (which re-sized `kIdleFloorUnobservedMs` and DE-ALIASED it from this constant —
    `MFO-B7`/`MFO-B8` stay open for THIS one), but note the ordering budget moved:
    `kSilentWarnAfter` is now **3500 ms**, so the headroom here is 500 ms.
  - `kSilentWarnAfter` (`ComposedCast.cpp`) — **RE-SIZED 2000 → 3500 ms and ARMED ON
    CHARGE (`fix/mfo-spell-authority-0922`, 2026-09-22).** At 2000 ms the `[cfc] ...
    NO observed cast ... APMF's engine seats may not be firing it` warning was FALSE
    for the WHOLE 2026-09-22 session: the measured claim → first CHARGE STATE latency
    for an offense cast is 2.3-2.5 s (claim → observed SpellFire 5.8 s with an equip
    cycle in front), so every claim tripped it while the seats were firing perfectly
    (principle 8 — a diagnostic that cries wolf on every claim is worse than none).
    3500 ms sits above the charge latency and strictly BELOW both
    `kHealHoldNeverObservedMs` (4000) and `kIdleFloorUnobservedMs` (8000), so both
    `static_assert`s still hold. **The timer is not the real fix:** `WatchArmed` now
    also stays quiet while `Actuation::CastInFlightOnHand(actor, hand, spell,
    w.proxy)` is true — a claim mid-charge is by definition not one the seats are
    failing to fire. Same predicate, same measurement and the same reasoning as the
    idle-hand floor's gate, so the two diagnostics are sized off ONE quantity.
    **What breaks:** raise it at or above 4000 and the `static_assert` fires; drop the
    in-flight read and the false warnings come straight back.
  - **REACHABILITY — TWO cases, not one.** (i) While the incumbent's condition
    still holds, only a rule ABOVE it can be held (a Claimed incumbent stops all
    three scans), so that hold is a genuine PRIORITY INVERSION. (ii) Once the
    incumbent's condition goes false its rule stops being reached, so a
    LOWER-ranked rule is held too — behind a claim nobody wants, kept alive by the
    hold's own heartbeat until the cap. Consequently lifting the cap is normally a
    one-time HANDOVER, but not unconditionally: if the ex-incumbent's condition
    flaps back true and neither claim is ever observed, the slot can ping-pong on
    a cap-plus-lap beat (bounded, and loud — every swap prints a HELD OFF line).
  - `HeldOffBy(follower, spell)` (`ComposedCast.cpp:514`, decl
    `ComposedCast.h:218`) ← `Logistics.cpp:1545` — names WHICH incumbent held a
    spell off, for the LOG ONLY. Reads `g_lastHold`, which every `Try()` erases at
    its top (`:207`), so it needs no expiry and MUST NOT grow one (#9).
  - **Depended-on-by:** `Actuation_Direct.cpp:888`/`:1185` map `Held` →
    `SelfCast::Held` (`Actuation.h:93`) → `Actuation_Direct.cpp:1488` (`CastAuto`,
    transparent NoOp) and `Logistics.cpp:1506` (transparent `continue`, log deduped
    2s); `Actuation.cpp:289-291` (`ConcentrationCast`) returns a TRANSPARENT NoOp with NO hand
    lock.
- **THE F12 IN-FLIGHT HOLD — `fix/mfo-heal-recognition`, v2.0.5, 2026-09-09.** The
  SECOND answer in `Try()` (`ComposedCast.cpp:484-556`), reached only when the F1
  hold above did NOT stand. **A heal claim whose spell (or whose delivery-flip
  proxy) the engine is ACTUALLY CASTING on the left hand is never released or
  re-pointed by another heal rule.** Field root cause (deck 2026-09-09, Jesper
  `0x750012C6`): the incumbent handle was minted 14:18:41.708, the engine reached
  `state=4(Casting)` at 14:18:47.148, and 17 ms later a different heal rule
  re-pointed the claim — so APMF's own `CheckCast` correctly DENIED the spell it
  had been steering for 5.4 s. **F1 did not fire because every term held EXCEPT
  `RefreshHealCastClaim`**, which on ABI 6 with a live claim has exactly ONE
  refusal: the `kHealHoldNeverObservedMs` (4000 ms) claim-age cap. The handle was
  5457 ms old and still `!observed`, because `observed` means the
  `TESSpellCastEvent` FIRED and this cast had only reached Casting. The cap is not
  wrong; it is blind to the engine's own state.
  - **Reuses `Actuation::CastInFlightOnHand`** (`Actuation_Hands.cpp:251`) — that
    predicate was file-local and went PUBLIC (decl `Actuation.h`, and
    `kHandLeft`/`kHandRight`/`kHandCount` MOVED from `Actuation_internal.h` to
    `Actuation.h` for the same reason: `ComposedCast.cpp` is not one of the three
    Actuation TUs). ONE definition of "in flight", not a second liveness test.
  - **Bounded by `kInFlightHoldCap`** (`ComposedCast.cpp:32-42`, `=
    APMFBridge::kHealCastTtlMs`, the same source constant as
    `Actuation_Hands.cpp:286`), anchored on `Watch::inFlightHoldSince` — the twin
    of `CastLock::claimGoneAt`, stamped when the extension first engages and zeroed
    the moment the engine is no longer casting that spell, so an idle gap between
    casts hands the slot over at once (matching `CanPreemptHand`'s "only the idle
    window BETWEEN casts is takeable"). Past the cap the anchor stays STAMPED so a
    wedged caster cannot re-open the window every lap.
  - **It does NOT heartbeat.** This branch never calls `RefreshHealCastClaim`, so it
    adds no lifetime to the incumbent's claim — the hazard F1's own doc warns about.
  - **Logs `LogHealHoldOffInFlight`** (`ComposedCast.cpp:242`), its OWN `[cfc]`
    message, sharing F1's dedup map and 2 s window (the two holds are mutually
    exclusive on any one call, so sharing cannot silence either).
  - **Why HERE and not in the hand lock.** `ComposedCast::Try` is the ONE
    `ClaimHealCast` call site in `native/`; BOTH the Logistics OOC concentration
    dispatch (`Logistics.cpp:~1500` → `CastTargetDirect`) and combat `CastOn`'s
    composed branch (`Actuation.cpp:1004`) bottom out here. The per-hand cast lock
    never saw the Logistics claim at all, which is why F8's identical protection did
    not cover this.
  - **What breaks if you change this:** (1) folding `Held` back into
    `Applied`/`true` re-creates the Fable SEV-2 bug — the held-off spell counts as
    FIRED, buys the `Scheduler.cpp:579-588` suppression window on a no-op,
    re-stamps `Actuation`'s Task-2 hand lock with the wrong spell, and sets
    `Logistics`' `acted` so every rule below it is walled off for the tick.
    (2) Re-aliasing the cap to `kHealCastTtlMs` (or removing it) restores the
    priority inversion; raising it past the claim TTL removes the hold's last
    bound, since the incumbent's own rule re-requests a fresh handle — and a fresh
    `created` — each time APMF auto-expires the old one. (3) The hold check rests
    on a NARROW invariant: **not** "one heal `Try()` per follower per tick" (false
    — a `Held` outcome continues the scan at `Logistics.cpp:1547`, and the
    `pass < 2 && !acted` wrapper at `:1323` re-runs it), but **"no heal `Try()`
    runs after a same-tick `ClaimHealCast` that MINTED"** — a `Claimed` outcome
    ends all three scans, and a NON-claimed one out of the `ClaimHealCast` branch (either
    half of the split `NotApplicable`/`ApmfRefused`, formerly `Refused`) clears `hand[0]`
    UNCONDITIONALLY so no later `Try()` finds an incumbent to hold. That matters because APMF's
    `IsClaimLive`/`GetCastProxy` read the PUBLISHED snapshot only, so a check in
    the same frame as a fresh `RequestCast` reads NOT-live and would thrash. Both
    sites carry the corrected dependency as a comment (`ComposedCast.cpp`'s hold
    check and `EnsureCastClaimLocked`'s unchanged fast path in `APMFBridge.cpp`).
  - `Logistics.cpp:1444` labels an `Applied` heal **"APMF claimed"**, not
    "delivered": a live claim may sit UNOBSERVED for its whole window. The FIRING
    signal is separate (`Diagnostics.cpp`'s SpellSink → the watch; `[cfc] ... NO
    observed cast` when it never fires). Do not word that back to "delivered" (#7).
    **Since 2026-09-21 that label is derived from the ROAD, not from claim liveness:**
    `Logistics.cpp:~1741` reads `Actuation::TargetStreamLive(id, spell, target)`
    (`Actuation_Direct.cpp:1484`, the direct road's own `g_targetCast` entry) — `IsHealCastActive`
    answered "any live heal claim on this follower" and labelled a direct Healing Hands "APMF
    claimed" because rule 0's Fast Healing claim was live (deck 2026-09-21 08:09:39).
- `End(RE::FormID follower)` — `APMFBridge::ReleaseHealCast` + `CastBounds::Disarm`
  + clears ONLY hand 0's watch slot (heal is always LEFT — a concurrent
  offense watch on hand 1 must survive a heal ending; PER-HAND, feat/per-hand-
  cast-slots). `ClearWatch(follower)` (no hand param) clears BOTH hands —
  reserved for a caller meaning "nothing wanted on this follower at all"
  (`Scheduler.cpp`'s `!castSeen`, `Followers`' dismissal teardown).
  **`ClearWatchHand(follower, hand)`** (2026-09-21) clears ONE hand's record with none of `End`'s
  bridge side effects (pure map op, so it is safe under `APMFBridge`'s `g_mx`) — the
  expiry sweep's clear (`APMFBridge::Tick`). `ObservedFiring` now has a second caller: the
  idle-hand floor's unobserved gate (`ReconcileHandFloorLocked`, window = the claim's own age). Call `End`
  the instant the gambit stops wanting the heal; the `APMFBridge`
  `FacetExpiry()` backstop (`Tick()`,
  `Diagnostics.cpp` — round-robin/party-size-aware) AND the claim's own TTL (on
  APMF's side) cover a caller that forgets — **there is no per-tick reconcile
  wired to call `End` explicitly** (Try short-circuits `CastSelfDirect`/
  `CastTargetDirect` before their own `g_selfCast`/`g_targetCast` bookkeeping
  runs), so release relies on the SAME "stop refreshing → expire" idiom
  `ClaimCombatTarget`/`ClaimOffenseCast` already use — not a new pattern.
- `Reset()` — clears `g_watch` (the diagnostic's only local state): `APMFBridge::
  ClearTransientState` drops the claim on `kPreLoadGame`, `CastBounds::Reset` drops
  the bound. Kept as the seam `Actuation_Direct.cpp`'s `ClearSelfCasts` already calls.
- **Threading:** `Try`/`End` run on the AddTask job worker (#4), matching every other
  `Actuation_Direct` entry point. No main-thread posting left in this module — APMF's
  own seats run inside APMF.dll's driven engine calls. `CastBounds` is lock-free;
  `APMFBridge` calls are any-thread-safe; `g_watch` is worker-serial-unlocked (see
  the diagnostic bullet above for why that's safe).
- **`CasterConsent.cpp`'s standdown is now BROADER than `CastBounds::Live` alone**
  (2026-09-05, S1 field fix, RE-VERIFIED under PASS F/Task 5) — all hard-abort/
  deny/veto sites in BOTH hooked thunks (0x06 `thunk`, 0x0A `CheckCastThunk`)
  early-pass on `ClientCastClaimed` (which ORs `CastBounds::Live` with
  `APMFBridge::IsHealCastActive(fid)`) BEFORE any deny logic runs — safe even
  though APMF now ALSO answers 0x06 on the SAME vtable MFO hooks globally,
  regardless of SKSE's plugin install order. See `CasterConsent.cpp`'s
  `ClientCastClaimed` doc comment for the full write-up.
- **What breaks:** if `Enabled()`'s kind-gate is ever removed/loosened, offense
  casts would ALSO route through this claim — this pass's explicit "offense
  stays on the legacy AI-fired path" boundary depends on that one check.
  Offense's OWN `ClaimCasting`/`kIntent_SelectSpell` claim (PASS 1 above,
  `Actuation.cpp:522`) no longer shares ANY channel with the heal claim here
  (PASS F moved heal to its own `kIntent_Cast` facet) — the PASS E-era
  cross-talk risk this bullet used to flag is structurally gone.
- **TASK 1 VERDICT (feat/mfo-claim-only-heal, 2026-09-05): already claim-only —
  audited, not reworked.** Superseded in mechanism by PASS F, but the VERDICT
  still holds: `Try`/`End` issue+release a claim only, no equip/drive/proxy/
  `CastSpellImmediate` of MFO's own, under either PASS E or PASS F.
- **feat/mfo-cast-port (this pass): the "let the native AI cast it" design,
  previously RE'd-but-not-built, is SHIPPED.** MAP's own former "FORWARD LOOK"
  entry here predicted exactly this shape once APMF answered the five seats —
  see the PASS F bullet in `APMFBridge.cpp`'s entry above for the mechanism,
  and `Docs/CAST-DELIVERY.md`'s "Native-AI seat-answering — SHIPPED" section
  for the full narrative closing that loop.

### TradeBridge.cpp / TradeBridge.h — Papyrus econ bridge (#21) ⚠️ SCRIPT-COMPAT
Native owns the trade DECISION; merchant read/mutation runs in `MFO_Trade.psc`
(native `GetInventory`/`GetGoldAmount` CTD on merchant chests). `RegisterFuncs()`
(`:365`) ← `plugin.cpp:422`, registers **10 Papyrus natives** on class `MFO_Trade`
(`:209-218`) called by the shipped `MFO_Trade.pex` — renaming/re-signing any breaks
trading silently. `VendorTrade` (`:223`) ← `Logistics_Economy.cpp` (`EconomyProbe:488`). `SellRow`/`NeedCat::
Kind` (`TradeBridge.h:25,35`) are the wire vocabulary with Logistics. Cross-save
safety: per-chest in-flight guard (`:250`) + `ClearTransientState`'s `g_nextToken +=
1'000'000` jump (`:282` ← `Serialization.cpp:612`) so a resumed stale token can't name
a fresh order.
- **#21 BUY expansion (weapon/armor/mage-apparel + spell tomes).** `NeedCat::Kind`
  appended `kWeaponMelee..kSpellTome` (append-only — never renumber). `ClassifyBuy`
  and `PlanBuy` now take a `BuyThresholds` (`TradeBridge.h`) the worker fills from the
  loot judge (`Logistics::BuildBuyThresholds`) — native stays a pure comparator (no
  new merchant/actor reads; `follower->HasSpell` is the one tome read, on a loaded
  actor). Gear = one best-in-category upgrade per window at ≤50 % of the remaining
  purse; mage apparel is per-slot (head/body/hands/feet/ring/amulet, `MageClothingSlot`)
  ranked MEO-aware (`MageApparelBuyKey`: value-primary when `MEOBridge::Available()`,
  else school-enchant-primary) with a villain-coded blacklist + necromancer exception.
  Bought gear is protected from re-sell by the `keepArmor` set (buckets by LOGICAL
  slot — `MageClothingSlot` for clothing/jewelry, primary biped slot for rated armor
  — keeping worn + one best-per-slot upgrade; NOT the raw bitmask, which let varied
  modded robes each survive) + the existing socketed exclusion in `EconomyProbe`.
  Toggles: `bEconomyBuyGear`, `bEconomyBuyTomes`, `bMageWearRobes`, `bMageApparelStrictSchool`.
  **ARMOR BY CLASS (2026-09-14):** `BuyThresholds` APPENDED `armorHeavyBias` /
  `armorLightBias` / `armorBaseScore[5]` (`TradeBridge.h:99-107`, after
  `eligibleSchools`; append-only); `ArmorScoreOf` (`TradeBridge.h:115`) is the shared
  score arithmetic; `PlanBuy` armor (`:266`) ranks by it against `armorBaseScore`, no
  longer by raw rating against `armorBaseRat`. `ReportTrade` (`:393`) names the offered
  sell rows and `TradeOrder::buyPlan` (built at the end of `PlanBuy`, `:349`).
- **#21 SELL bypass + pricing** (`EconomyProbe`, INI-only, no MCM). `bMerchantPerkBypass`
  + `xMerchantPerkID` (0x00058F7A): a follower holding the merchant perk (dual-check
  `GetActorBase()->GetPerkIndex` + `HasPerk`, the `OwnsExactPerk` idiom) sells past the
  vendor's VEND filter. `bSpeechPricing`: `SellRow.value = lround(baseValue *
  sellFraction)`, `sellFraction = 1/(fBarterMax-(fBarterMax-fBarterMin)*speech/100)`
  (0.30→0.50 over Speech 0→100). BUY stays base value. (Per-perk `kModSellPrices`
  boosts are NOT applied — needs a CI-verified EPFD read; flagged.)

### Diagnostics.cpp / Diagnostics.h — event sinks + THE WORKER PUMP ⚠️ RACE LINCHPIN
Owns the one persistent sleeper thread driving the per-follower tick, four event
sinks, `DumpReport`, and (2026-09-21 field batch) the `[fatal]` passive hook and
the `[hb]` heartbeat. `Install()` (`:1305`) ← `plugin.cpp` kDataLoaded (registers
SpellSink/HitSink/MenuSink + Probe crosshair sink). `SleeperLoop` (`:893`, `kPumpMs=
133`, `kDiagEveryNth=4`) never touches game state directly — only `AddTask`s a lambda
that re-checks the epoch, sets `TickActiveGuard`, then runs `Followers::Refresh`
(diag turn), `Scheduler::Tick`, `Loadout::Tick`, `AtkObserveTick`, `Probe::Tick`
(diag turn), `Board::PublishSnapshot` (`:911-940`), each step preceded by an
`HbStage("<callee>")` store. (Line numbers refreshed on `fix/mfo-field-batch-0921`;
older `Diagnostics.cpp:NNN` cross-references elsewhere in this map read low.)
- **`[hb]` HEARTBEAT (item C; `bHeartbeat`, INI-only `[Debug]`, default ON).** Every
  37 wakes (~5 s) the SLEEPER thread itself prints `[hb] worker ticks=N main drains=M
  mainAge=<ms> lastWorkerStage='<callee>'` (`:945-970`): `ticks` = AddTask bodies that
  ran, `drains` = heartbeat probes that ran on the main thread (one `MainThread::Post`
  per line, counted when it runs; 0 forever on VR), `mainAge` = ms since the last probe
  ran, `lastWorkerStage` = the callee the worker body was about to run. Relaxed atomics
  (`:79-84`), not save-scoped. On a freeze the counter that stops names the thread;
  the stage names the callee that never returned. Not in the line: a last-main-LABEL
  (`MainThread::Post` takes none; adding one is a `MainThread.cpp/.h` change).
- **`[fatal]` PASSIVE FATAL HOOK (item B; `:1180-1303`, installed from
  `SKSEPluginLoad` right after `SetupLog`).** A LAST-position
  `AddVectoredExceptionHandler` + `std::set_terminate`. The handler flushes spdlog,
  prints ONE `[fatal] code=<hex> addr=<module+rva> thread=<id>` line for an
  error-severity SEH code without the customer bit (0xC0xxxxxx — AV, illegal
  instruction, stack overflow, int divide, in-page, heap corruption), and returns
  `EXCEPTION_CONTINUE_SEARCH`: CrashLogger (an unhandled-exception filter, which
  runs after every vectored handler declines) keeps the report. NEVER handles or
  swallows. Vectored handlers see FIRST-CHANCE exceptions, so a `[fatal]` line
  followed by ordinary lines was handled by somebody (SEH probing); the LAST line
  is the crash. C++ throws (0xE06D7363), .NET, thread-naming codes never print.
  Capped at 16 lines/session; `thread_local` re-entry guard; STACK_OVERFLOW gets the
  flush only. Module ranges: MFO by address (`GetModuleHandleExA` FROM_ADDRESS),
  APMF.dll by name (re-resolved at kDataLoaded, `RefreshFatalModules` `:1280`), the
  game image from `REL::Module`; size from the mapped PE header (`SizeOfImage` at
  NT+0x50). Win32 shapes hand-declared at the top of the file (no `<windows.h>`).
  MSVC keeps the terminate handler PER THREAD: installed on the plugin-load thread,
  the sleeper (`:894`), the AddTask worker (thread_local latch in the tick body,
  `:905`) and main (one `MainThread::Post` at `StartPump` `:1405`). The combat-thread
  hooks are NOT covered. **What breaks:** returning anything but 0 from the handler,
  or logging inside it for informational codes, hijacks the crash from CrashLogger /
  spams try-catch traffic; calling the engine or allocating in the handler crashes
  the crash. The flush policy this pairs with is `flush_on(info)` in `plugin.cpp`
  `SetupLog` (the 1 s background flusher is gone) — the only policy that keeps the
  tail through a fast-fail, a hang+kill or an OS teardown, which no handler sees.
- **StopPump-before-clear invariant:** `StopPump()` (`:1074`) is the FIRST statement
  in `ResetAllState` (`Serialization.cpp:686`) and runs at kPreLoadGame
  (`plugin.cpp:369`). It clears `g_pumpRunning`, bumps `g_pumpEpoch` (strands mid-
  sleep threads), then spin-waits ≤2000 ms on `g_tickActive` (`:1092`) so any in-
  flight tick finishes before the maps are wiped (concurrent map insert+clear = UB).
  Reordering it after the clears, dropping the `g_tickActive` drain, or letting a
  sink/tick mutate save-scoped maps unguarded re-opens the load-screen crash (loud
  error `:1100`). **`kPumpMs` is the evaluator deadline** — changing it re-times the
  scheduler, not just diagnostics. `DumpReport` uses `find()` not `operator[]`
  (`:1023`) to avoid persisting a spurious `0xFF`-keyed record.
- **Wave-1 worker-quiesce API:** every MFO `AddTask` body now runs under a
  `PumpTickGate(epoch)` RAII — STOREs `g_tickActive=true` (seq_cst) then
  re-checks the epoch (Dekker handshake, closes the check-then-set TOCTOU), and
  bails if a `StopPump`/`PausePump` is in progress. **Now DECLARED in `Diagnostics.h`
  + defined out-of-line (the g_active concurrency wave exposed `PumpTickGate` +
  `CurrentPumpEpoch` so the cross-TU sinks — Rapport death, Logistics loot-waiver,
  Board focus-fire — drain identically; pump atomics stay file-local).**
  `PausePump()`/`ResumePump()` are the RESUMABLE quiesce for `SaveCallback` (SEV-1): PausePump
  drains the worker like StopPump but WITHOUT tearing the pump down, so the save
  can iterate `g_followers` with no worker insert racing it; ResumePump lifts it.
  MUST be paired (RAII across the save). Off-worker sink bodies gate on
  `IsTrackedFast` before taking the epoch.
- **`[atk-obs]` passive attack-event probe (feat/mfo-attack-observe, 2026-09-21;
  `bAttackObserve`, INI-only, default OFF; the Deck test INI sets 1).** File-local block before `SleeperLoop`:
  `AttackSink` (`:520`, one `BSTEventSink<BSAnimationGraphEvent>` per follower slot,
  `g_atkSinks[16]`), `AtkSlot g_atk[16]` (`:446`, the fixed-size counter rows),
  `g_atkUnk[32]` (`:482`, the session-wide CAS-claimed unknown-tag table),
  `AtkObserveTick()` (`:835`, called in the sleeper body after `Loadout::Tick`),
  `AtkPostAttach/Detach` (`:590`, `MainThread::Post`:
  `actor->GetAnimationGraphManager(mgr)` → each `mgr->graphs[i]` →
  `BSTEventSource<BSAnimationGraphEvent>::AddEventSink`, posted whenever the
  follower is managed AND `Is3DLoaded()` — in OR out of combat, item D of the
  2026-09-21 field batch, so a non-fighting follower's parkour / stuck tags reach
  the sink — and re-posted every 2 s (`kAtkAttachMs`) because graphs rebuild on a
  3D reload; AddEventSink dedupes). Counters, `lastAttackMs` and the dump stay
  FIGHT-scoped: zeroed at fight open (`AtkService` `:711`); out-of-combat events
  surface only through the session-wide `new-tag` first-sight line. Public:
  `DumpAttackHistogram(fid)` (`:916`; idempotent, exported, NO caller today — DECIDED
  (Fable round on 2ac4163): NOT wired into the Scheduler's 2-tick OOC debounce,
  which would split one fight into two lines on an `IsInCombat` flap; the probe
  closes fights itself 1.5 s after `IsInCombat` drops) and `ResetAttackObserve()` (`:925`)
  ← `Serialization.cpp:742` in `ResetAllState`, after `StopPump`. The attach is
  re-posted every 2 s while the follower is managed + 3D-loaded (`kAtkAttachMs`),
  not only at fight open — a graph rebuilt mid-combat would otherwise leave the
  fight open with no registration and print "(no attack events)" (SEV-3 fix). The idle-in-reach
  compare against `lastAttackMs` is STRICT `>` (SEV-2 fix; `>=` closed every run as
  0 ms after a restart). **Open backlog: `Docs/REVIEW-BACKLOG.md` MFO-B50 (clear-
  vs-in-flight-event race, ≤1 stale count), B51-B54 (notes: CommonLib's
  `Actor::AddAnimationGraphEventSink` equivalent, `updateLock` not taken on the
  walk, session-wide unknown table, redundant `!seen[i]`).**
  **The sink runs on whatever thread the animation graph dispatches on (the
  `[atk-obs] sink thread=` line reports it — principle 5). What it must NEVER do:
  send/notify an animation event, touch `g_followers`/`g_active`/`IsTracked`
  (membership is the slot's `fid` atomic; #4/#74), allocate, log, lock, or call the
  engine — it reads `holder->GetFormID()` and the tag/payload `c_str()` and writes
  relaxed atomics only.** The worker owns slots, fight state, the idle-in-reach
  accumulator (actor reads on the worker) and every log line. Static sink storage:
  a registration left on a graph never dangles; an event on a slot whose fid is 0
  or differs is ignored (slot reuse is safe). Counter reset is `exchange(0)` — an
  increment racing the dump lands in the next fight (boundary off-by-one,
  accepted). Adding a counted tag = append to `kAtkTags` (exact) or `kAtkFamilies`
  (+ its label); never grow `kAtkSlots`/`kAtkUnknown` without re-checking the
  per-event scan cost (linear over both).

### Probe.cpp / Probe.h — M4 debug/research harness
Fires one engine primitive at a follower and records emergent behavior; nothing
outlives a session. `ReleaseAll()` (`:445`) ← `plugin.cpp:359`, `Serialization.cpp:741`
— must keep calling `Targeting::ClearAll()`+`Stop()` or a stale latch/watch handle
survives. `Tick()` (`:368`) ← `Diagnostics.cpp:350` (main-thread task). **Real
gameplay dependency (not debug-only):** `CrosshairTarget()` (`:307`) ←
`Logistics_Loot.cpp:1066` (QuickLoot-aware player-claim signal; it moved out of
`Logistics.cpp` in the 2026-08-31 split). `FocusOnCrosshair()` (`:309`) ←
`Board.cpp:622`.
`StartCombatOn` uses po3 RelocationID(37608,38561), VR-refused. Most `Fire`/`GetLast`/
enum entry points have **no in-tree C++ caller** (the ImGui probe panel isn't wired) —
**UNVERIFIED — check Board before assuming dead.**

---

## 8. Core state / evaluator / config / forms — `Followers.*`, `Rapport.*`, `Evaluator.*`, `Vocabulary.h`, `Config.*`, `Forms.*`

Shared foundation. `g_active`/`g_activeIds`/`g_followers` are **serial worker /
main pump only** (#4, #74) — mutated only on that domain. **Off-worker readers
(combat cast hooks, event sinks, `SaveCallback`, the progression main-thread
poll) MUST use the Wave-1 any-thread mirror, never the live lists:**
`Followers::IsTrackedFast(FormID)` (`:209`, membership; locks `g_mx`, tests the
mirror `Refresh` republishes) and `Followers::ActiveSnapshot()` (`:214`, an
immutable `shared_ptr<const vector<FormID>>` iterated lock-free). The unlocked
`IsTracked` walk is worker/main-domain ONLY. See #74 for the BSJobs-vs-main
concurrency question (distinct threads, mutual exclusion UNPROVEN).

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
- `TryEnsureRecord` (`:233`) vs `EnsureRecord` (`:248`): Rapport/ProgAllocator use
  `TryEnsureRecord` because `g_active` holds `0xFF` cloned teammates; `EnsureRecord`
  would mint a doomed record SaveCallback skips (F2). **`TryEnsureRecord` INSERTS
  (rehash).** `Followers::IsTrackedFast`/`ActiveSnapshot` (`:209`/`:214`) are the
  Wave-1 off-worker accessors; `BoardEditScope` (`Followers.h`) is a tripwire that
  logs a HAZARD if a MAIN-thread board Prog edit (SetClass/Respec →
  `MainThread::Post`) ever inserts (item 2b — proven safe because a board-
  addressable follower already has a record, so it FINDS not inserts).
- `ApplyDefaultKit` (`:24`) — 3 combat + 4 logistics; called on record creation
  (`:198`) and as the empty-board load backfill (`Serialization.cpp:539`). **Verified
  to fit Rank I** (3/4); adding a rule overflows and the load clamp silently drops it.
- **General follower-mutation API (v1.1 add-on-architecture SEED)** — `GetBaseClass`/
  `SetBaseClass` (read/write `FollowerState::combatClassOverride`; SetBaseClass →
  `TryEnsureRecord`), `GetFollowerHMS`/`SetFollowerHMS` (base H/M/S by canonical pool
  {0=H,1=M,2=S} == ProgAllocator `kHmsAV`), and `MeasureEngineVitalAward` (pure
  observe-not-clobber engine-award read). **General, add-on-agnostic — no progression
  types in the signatures**; the host applies add-on DATA through them. **Domain: main-
  thread / serial-worker only** (same as `TryEnsureRecord`). Callers today: `SetBaseClass`
  ← `ProgAllocator::SetClass`; `GetBaseClass` ← `Scheduler.cpp` stance + `Actuation.cpp`
  dagger-melee; `Get/SetFollowerHMS`+`MeasureEngineVitalAward` ← `ProgAllocator::RecomputeHMS`.
  This is the surface Phases 2+ grow (SetSkill/GrantPerk/level-up events) — keep it general.
- `Refresh` (`:419`, the eviction hub) — **runs on the JOB WORKER** (`Diagnostics.cpp`
  tick), not the main thread; it rebuilds `g_active`/`g_activeIds` and republishes the
  `IsTrackedFast`/`ActiveSnapshot` mirror under `g_mx` (`PublishActiveMirror`).
  **`[follower]` STATE LINE (item D of the 2026-09-21 field batch):** `LogStateLine`
  (`:99`) prints `[follower] <id> '<name>' pkg=<GetCurrentPackage form> waitingForPlayer=
  <AV 95> dPlayer=<u> moved=<u since last line> aiEnabled=<IsAIEnabled> inCombat=<0/1>
  [roster|30s]` on the `+` roster line and every `kStateLineS` (30 s) per active
  follower from the sweep (`:455-461`); sample map `g_stateSample` is worker-domain
  like `g_lastCombat`, cleared in `ClearTransientState` (`:190`). Pure reads
  (pinned 3.7.0 `Actor.h:533/:570`, `ActorValues.h:103`). Why: Follow and Wait are
  the same vanilla `PlayerFollowerPackage` form branched on `WaitingForPlayer`, and
  nothing else MFO printed could tell Jesper's three-minute stand-still apart. On drop
  calls `Loadout::Restore`, `Targeting::Clear`, `CombatStyle::Clear`,
  `CasterConsent::Clear`, `Packages::Release`, `Logistics::OnFollowerRemoved`,
  `Packages::RetreatEvictIf`. Several release engine-serialized alias fills — removing
  any leaves a latch that re-fills on every future load. Callers `plugin.cpp:357`,
  `Rapport.cpp:147`, `Diagnostics.cpp:253`. (Followers.h's per-symbol header comments
  predate the worker move — the DOMAIN is the serial worker/main pump, #4/#74.)

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
(`Serialization.cpp:155,179`), read (`:434,459`); there's even a hard literal
`"act.equip_torch"` (`Serialization.cpp:486`) tracking `kActEquipTorch`
(`Vocabulary.h:136`). Renaming any `kCond*`/`kAct*`
is a **schema migration, not an edit** (old saves carry the old string; the `==`
compares silently stop matching). Adding an opcode requires wiring in Evaluator +
Actuation + Board's picker or it's inert. **`Subject` enum** (Self=0/Player=1/
NearestAlly=2, `:37`) is serialized as the raw `subject` byte (read `Actuation.cpp:73`,
`Board.cpp:850,858-863`) — reordering reinterprets every saved byte (a specific follower is
carried as `subjectActorForm`, NOT an enum value, precisely to keep the enum frozen).
`Pct`/`HealthPct`/etc. (`:214`) use permanent+temporary AV — changing the max formula
re-times every "HP below X%" rule + Confidence.

### Config.cpp / Config.h
~90 `g_*` atomics read across 22 files (cross-thread-safe by design). INI-only
`[Debug]` keys with no MCM face: `bAttackObserve` (default OFF), `bHeartbeat`
(default ON, 2026-09-21) — wired in `Apply` + `ResetToDefaults` + the seed INI only,
NOT in `kMcmDefaults`. `Read` (`:363`,
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
`g_castPackage`, `g_lootQuest`, `g_travelPackage[1-3]`, `g_apmfLootTravelPackage0-3`,
`g_retreatQuest/Package`, `g_apmfRetreatPackage`,
`g_tradeQuest`, `g_meleeStyle/rangedStyle/castStyle/probeCastStyle`) are resolved once
on the main thread specifically so combat-thread hooks only READ settled pointers —
never do a data-handler lookup from those hooks. `EnsurePlayerSetup` (`:96`) grants the
Field Orders power (idempotent via `HasSpell`) ← `plugin.cpp:356` (kPostLoadGame only,
after co-save loads); must NOT latch a failed grant (`:100`) so a missing ESP retries.

---

## Appendix — SKSE registration & sink inventory (the wiring choke points)

| Registration | Site | Notes |
|---|---|---|
| Serialization Save/Load/Revert callbacks | `plugin.cpp:412-414` | `kSerID='MFO0'` |
| Message listener | `plugin.cpp:416` | drives the whole lifecycle |
| Board overlay: swapchain-vtable Present(8)/ResizeBuffers(13) + WndProc + InputSink (sink path only) | `plugin.cpp:306` → `Board::Install` (`Board.cpp:1622`, end) | at kDataLoaded, VR-refused; polls for live swapchain, ZERO game offsets |
| Board input: `InputDispatchHook` `write_call<5>` on `(67315, 68617)` +`0x7B` | `plugin.cpp` (SKSEPluginLoad) → `Board::InstallInputHook` (`Board.cpp:1581`) | **at PLUGIN LOAD**, before the input thread exists; VR-refused and gated to **1.6.1170 or 1.5.97**, each a separate branch with its own verified disassembly (AE `0x140CD8F40`+`0x7B`, SE `0x140C150B0`+`0x7B`, both whole `E8 rel32`); every other runtime is a logged no-op; nulls the batch so the board takes input outright |
| `MainThread::Install` (player Update vfunc 0x0AD) | `plugin.cpp:297` | true main-thread pump |
| `Targeting::InstallHook` (Character::UpdateCombat 0xE4) | `plugin.cpp:299` | also drives CombatStyle |
| `CasterConsent::InstallHook` (CheckStartCast 0x06 + CheckCast 0x0A) | `plugin.cpp:300` | 14 + 1 vtables |
| `CombatStyle::InstallEquipGate` (CheckShouldEquip 0x0F) | `plugin.cpp:301` | 30 template vtables |
| `Rapport::RegisterSinks` (TESDeath, TESCombat) | `plugin.cpp:302` → `Rapport.cpp:521` | sinks LAST |
| `Logistics::RegisterSinks` (TESContainerChanged, TESEquip) | `plugin.cpp:303` → `Logistics.cpp:1949` | direction filter mandatory |
| `MEOBridge::RegisterSink` (TESEquip) | `plugin.cpp:298` → `MEOBridge.cpp:75` | optional |
| `Diagnostics::Install` (TESSpellCast, TESHit, MenuOpenClose, + Probe crosshair) | `plugin.cpp:341` → `Diagnostics.cpp` `Install()` | + the worker pump; prints the `[atk-obs] frameworks` line |
| `[atk-obs]` `BSAnimationGraphEvent` sinks (per follower slot, on his own graphs) | `Diagnostics.cpp` `AtkPostAttach` ← `AtkService` (worker) → `MainThread::Post` | whenever managed + `Is3DLoaded()`, every 2 s (`kAtkAttachMs`), in or out of combat, deduped; counters/dumps fight-scoped; observation only, `bAttackObserve` |
| `[fatal]` vectored exception handler (LAST) + `std::terminate` handler | `Diagnostics.cpp` `InstallFatalHook` ← `plugin.cpp` `SKSEPluginLoad` after `SetupLog`; terminate copies per thread (sleeper, worker, main via Post) | process lifetime; flush + one `[fatal]` line, `EXCEPTION_CONTINUE_SEARCH`; never handles |
| `TradeBridge::RegisterFuncs` (10 Papyrus natives) | `plugin.cpp:422` → `TradeBridge.cpp:365-376` | script ABI |
| `MEOBridge::Acquire` (MEO interface) | `plugin.cpp:289` | external ABI |
