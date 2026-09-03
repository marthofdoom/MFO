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
   whole files. Big files (ProgAllocator 2333, Board 2190, Logistics_Loot 2227,
   Packages 1657, Logistics 1700, CasterConsent 1066) should never sit in context — grep to a
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
| **Co-save (4 records)** | `Serialization.cpp`, `Serialization.h`, `State.h` | FLWR `v5`, MSTK `v1`, PRGN `v6`, FWPN `v1` (`Serialization.h:7-90`). FLWR v5 (#78) APPENDED `mfoEnabled` u8 after `combatClassOverride` (`if(version>=5)`); v1–v4 byte-identical, pre-v5 defaults `true`. Changing a field order/type/count, or bumping a version without a matching gated reader, **desyncs the byte stream and corrupts live saves**. A downgraded DLL destroys newer records (#12) — warned on-screen. PRGN v5 APPENDED the §HMS block (`if(a_version>=5)`); **v6 (§HMS Phase 3) DROPS `hmsTarget` (recomputed on load), ADDS a global `g_playerHmsTotalLast` f32 in the header + per-follower `hmsZeroAwardStreak` u8 + `hmsGrantRemainder` f32×3 + `hmsAwardAccum` f32 + flags bit 0x20 `fixedStat`.** v5 reader KEPT (reads+discards the old target, defaults the new fields); v1–v4 byte-identical. |
| **Serialized string/ordinal contracts** | `Vocabulary.h`, `State.h` | Gambit opcode **strings** are persisted verbatim (#10); `Subject` enum and `CombatStyle::Stance`/`combatClassOverride` ordinals are persisted as raw bytes. Renaming an opcode or renumbering an enum is a **schema migration, not an edit** — old saves silently misread. |
| **`ResetAllState` teardown order** | `Serialization.cpp:562-622` | `StopPump()` MUST run first (`:568`) to drain the worker before any `clear()`; concurrent map insert+clear is UB. Every subsystem's `ClearTransientState`/`ClearAll`/`ReleaseAll` is ordered here. Reordering re-opens the load-screen-crash race. |
| **Alias fills / evict marker** | `Packages.cpp` | Alias fills at static priority 60 are **serialized into the `.ess`** (`plugin.cpp:302-322`). Missing/reordered `ReleaseAll` on kPreLoadGame / post-load / revert latches actors permanently across all descendant saves. The evict marker must stay a non-actor XMarker (base `0x3B`) or the **furniture-ejection bug** re-breaks (player forced into a package alias). |
| **The worker pump** | `Diagnostics.cpp` | One sleeper thread (`SleeperLoop`, 133 ms) drives the *entire* per-follower tick (`Scheduler::Tick`/`Loadout::Tick`/`Probe::Tick`) via `AddTask`. `kPumpMs` is the evaluator deadline, not a HUD constant. StopPump-before-clear is the linchpin invariant. |
| **Combat vfunc hooks** | `Targeting.cpp`, `CasterConsent.cpp`, `CombatStyle.cpp` | Three engine vtable hooks, install-once at `plugin.cpp:293-295`, VR-refused. Run on the **combat thread**. Any `CombatController` member touched there must be `< 0x68` (AE +8 layout bug; static_asserts). Signature mismatch corrupts every actor's combat/cast call. |
| **Frozen FormID / ESP contract** | `Forms.h` | Local FormIDs (`0x800`+) are a frozen contract with `MFO_GenerateESP.py`, audited by `tools/audit_esp.py` (#41). Changing one orphans every save that saw it; `0x802` stays reserved. |
| **Config INI keys** | `Config.cpp/.h` | Each key is wired in ~6 coupled places; the key **name** is the MCM-Helper persistence identity. Renaming unbinds the control; changing a key's *semantics* without renaming reinterprets the persisted value (the MEO ~100x-XP class of bug). |
| **External ABIs** | `MEO_API.h`, `APMF_API.h`, `TradeBridge.cpp`, `Papyrus.cpp` | `MEO_API.h` is a wire ABI shared byte-for-byte with a *separate* shipped MEO.dll (append-only). `APMF_API.h` is likewise byte-shared with a *separate* APMF.dll (append-only, C-ABI POD; consumed by `APMFBridge`) — mirror APMF's copy exactly, never edit it locally. `TradeBridge`'s 10 Papyrus natives + `Papyrus.cpp`'s 3 method-name strings are called by shipped `.pex` — renaming breaks scripts silently. |

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
  `APMFBridge::Acquire` (APMF cast-select client; null-degrades if APMF absent) →
  `Followers::ResolveQuirks` → `MainThread::Install` → `Targeting::InstallHook`
  → `CasterConsent::InstallHook` → `CombatStyle::InstallEquipGate` →
  **sinks LAST** (`Rapport::RegisterSinks`, `Logistics::RegisterSinks`,
  `MEOBridge::RegisterSink`) → `Diagnostics::Install`. **Sinks must follow
  form resolution or they fire against unresolved forms.**
- **`kPreLoadGame`** (`:302`): `Diagnostics::StopPump()` then
  `Packages::ReleaseAll("kPreLoadGame")` — release the alias the engine would
  otherwise serialize into the outgoing `.ess` — then
  `APMFBridge::ClearTransientState()` (drop runtime-only cast-select claims once the
  pump is drained).
- **`kPostLoadGame`/`kNewGame`** (`:324`): warn if newer save (`:338`) →
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
- **`'FLWR'` / `kSchemaVersion=5`** (`Serialization.h:8,50`) — per-follower
  `{rapport, rank, combatClassOverride(v4), mfoEnabled(v5), tables[Combat,Logistics][], overrides[]}`.
  Written `SaveCallback` `Serialization.cpp:82`; read `LoadCallback` `:229`.
  Version history v1→v5 documented `Serialization.h:31-50`; v1 tutored-block
  reader kept forever (`Serialization.cpp:479`). **v5 (#78) APPENDS `mfoEnabled`
  as one u8 right after `combatClassOverride`, read gated `if(version>=5)`; a
  pre-v5 record has none and defaults `true` — every existing follower stays
  MFO-enabled, v1–v4 byte-identical.**
- **`'MSTK'` / `kStockVersion=1`** (`Serialization.h:13`) — Logistics'
  per-follower stock-gear sets; second independent record, never touches FLWR.
  Write `Serialization.cpp:187-217`, read `:239-313`. Owner: `Logistics.cpp`.
- **`'PRGN'` / `kProgVersion=6`** (`Serialization.h:16`) — GENERAL per-follower
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
- **`'FWPN'` / `kForcedWeaponVersion=1`** (`Serialization.h:59`) — #76 force-hold:
  the weapons MFO force-equipped for an active equip gambit. Owner
  `Actuation.cpp` (`CoSaveForcedWeapons`/`CoLoadForcedWeapons`); **CoLoad
  RELEASES the locks, never repopulates** (a session starts with no force-hold,
  the gambit re-forces if still true). Fourth independent record.

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
  `combatClassOverride` (v4; ordinals == `CombatStyle::Stance`),
  `mfoEnabled` (v5, #78; the per-follower MFO master switch, default true),
  `tables[kCount]`, `overrides`.
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

- `g_holder` (`Packages.cpp:174`) single-writer cast state (no lock);
  `g_liveStream` atomic mirror (`:195`) is the ONLY holder field the caster-thread
  consent hooks may read.
- `EnsureEvictMarker` (`:1215`) — caller `plugin.cpp:349` only, **before** the
  post-load reconcile. **What breaks:** if it stops minting, `EvictionRef` falls
  back to the PLAYER → furniture-ejection bug (v1.0.25/26) re-breaks.
  Must be main-thread (`PlaceObjectAtMe` mutates the cell) + force-persisted.
  Base-`0x3B` revalidation is load-bearing (handle indices rebuild per load).
- `ReleaseAll(why)` (`:1240`) — callers `plugin.cpp:321,355`, `Serialization.cpp:610`.
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
  **`Actuation::CastSelfDirect`** (`Actuation_Direct.cpp:729`, public) — effect + magicka only,
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
  through `Actuation::CastTargetDirect` (`Actuation_Direct.cpp:893`) — `CastSpellImmediate` straight onto the target
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
  = magicka drain with NO `FORCE-CAST` log). **SLOT-FOR-DURATION (owner-keyed `Slot g_slot[2]
  {form,source,owner}`):** each live stream OWNS a slot (`ConcProxy::Acquire(follower, src)` —
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
  shared by the dismissal sweep (`Refresh`) and the #78 MFO-OFF toggle (Scheduler).
- `ForceRefToNative` (`:266`) = `REL::ID(25052)` `TESQuest::ForceRefTo`, AE-only
  (VM path off AE). Two-class layout offsets (`kPointerOffFromIPackageData=0x10`,
  `:76`) + `kTypeTargetSelector`/`kTypeSingleRef` guard (`:88`, `ReadTarget` `:431`)
  are **memory-safety critical** — `SetInputs` (`:467`) writes nothing if guards fail.

### Actuation.cpp / Actuation_Direct.cpp / Actuation_internal.h / Actuation.h — "a package IS the action"
Only module that mutates actor state; main-thread only. **Split mechanically
2026-08-31 (no logic change):** `Actuation.cpp` (1244) = the combat-rule
dispatch — `Fire` + verbs, `CastOn`/`ConcentrationCast`/`ForceCast`,
`EquipWeapon`, `NearestAlly`/`ResolveCastTarget` (`:856`), the #76 force-hold
map + FWPN co-save; `Actuation_Direct.cpp` (1309) = the direct-delivery
streams (`CastSelfDirect`/`CastTargetDirect` + reconciles/`ClearSelfCasts`,
`CastAuto`) + their apply substrate (`ConcProxy`/`DeliverySpell`,
dispel/sustain, `Apply{Self,Target}Effect`/`ApplyEffectFromTo`,
beneficial-recast pacing) — the two direct-cast registries (`g_selfCast`/
`g_targetCast`) are file-local there; `Actuation_internal.h` = the shared
concentration numbers (`kConc*` sustain windows, `kConcApplyPeriod` cadence
contract, `DrawConcCap` random stream cap) as **`inline`** — any definition
added to that header MUST be `inline` or it's an LNK2005. `Fire(follower,
choice)` (`Actuation.cpp:896`) dispatches one action/tick: Wait / Attack (→`Targeting::Command`) /
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
- `CastOn` (`Actuation.cpp:265`) escalation: AE-only gate (`:314`) → range/competence/reserve →
  concentration fork (→ `ConcentrationCast`) → equip + **AI-first grace** (`:461`,
  follower's own AI casts first) → on miss `ForceCast` (`Actuation.cpp:70`) via `Packages::CastAt`.
  Off-AE the whole path declines transparently (#67) so vanilla AI keeps casting.
  **The FF silent cast (and every other `CastSpellImmediate` on a live path) is now
  `MainThread::Post`ed** — CastOn runs on the job worker and the old inline engine call
  was the prime suspect for the queued 1.5.x `act.cast_target` AV reports (#14).
- `ConcentrationCast` (anon, `Actuation.cpp:168`, COMBAT) = self→`CastSelfDirect`; non-self→
  **`CastTargetDirect` (DIRECT FORCE, PRIMARY — the package delivery is REMOVED)**.
  Latches `CasterConsent::Want` on each Applied so the slider keeps denying competing
  AI spells and the AI's own unbounded concentration attempt; the direct apply itself
  bypasses both consent hooks (ENGINE_NOTES §0.13 — `CastSpellImmediate` skips the AI
  cast pipeline they sit on), so deny-the-AI + deliver-directly is coherent, never a
  lockout. No bForceCastOnMiss/bUsePackages gate, no Loadout cooldown (the channel
  self-paces; a 4 s cooldown hole would let the ~2 s stale window tear it down).
- `CastTargetDirect` (PUBLIC, `Actuation_Direct.cpp:927`) = `CastSelfDirect` generalized to a NON-self target: the
  known-working DIRECT FORCE (`CastSpellImmediate` onto the target + magicka deduct, NO
  package → beats the `§4.6` lock). Registry `g_targetCast`; concentration re-applies
  at `kConcApplyPeriod` (~1 s, the heal cadence contract — per-second authored
  magnitude AND cost), FF at fCastCooldown; bounded/released by `TargetCastReconcile`.
  Callers: **Logistics OOC dispatch AND combat `ConcentrationCast` — BOTH primary**
  (marth's ruling: always the known-working force). LoS+LoF gate hostile offense on
  every apply. (The c539257 `CastConcentrationAt` package wrapper was REMOVED — it
  caused the Lucien OOC-heal regression.)
- `CastAuto` (PUBLIC, `Actuation_Direct.cpp:1106`) — AUTO target inference for `act.cast_target`,
  engaged ONLY when the board's default "Auto" pick is set (subject `Self`, no
  subject actor, no selector target). **Wired into BOTH paths:** combat `Fire`'s
  `kActCastTarget` branch AND `Logistics::ServiceFollower`'s OOC cast dispatch
  (`Logistics.cpp:~1090-1250`, inside `ServiceFollower`). On that OOC path a **non-AUTO** resolved `cast_target`
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
  `Actuation::ResolveCastTarget` (`Actuation.cpp:856`, moved out of the anon namespace
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
  in the fan. **Per-target apply guard `ShouldApplyTo` (`Actuation_Direct.cpp:633`):**
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
  `SelfCast{Declined,Refreshed,Applied}` so a pacing REFRESH doesn't count as an
  action that suppresses lower rules (logistics starvation, F3); its
  `SelfCastReconcile` release has NO time cap — an earlier 30 s cap dispelled long
  self-buffs mid-duration (~30 s re-cast beat); release is stale/follower-gone only.
  **F3 tri-state maps the SAME in BOTH combat call sites** (`CastOn` self fork +
  `ConcentrationCast` self guard) as in logistics: `Applied → Fired`,
  `Refreshed → transparent NoOp` (a paced channel-refresh does NOT occupy the tick
  or suppress the rules below), `Declined → transparent`. The combat suppression
  window (`releaseSec`) is tuned so a party's round-robin gap can't outlast the
  self-buff and re-open the dispel/re-cast beat.
- **Summon spam guard (v1.1.1)** — `Actuation::CasterHasLiveSummon` (PUBLIC,
  `Actuation_Direct.cpp:711`). A conjured familiar/atronach (`SummonCreatureEffect`)
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
  into the COMBAT path (v1.1.1): `Actuation::Fire` (`Actuation.cpp:~950`, the sole
  caller is the Scheduler combat scan `Scheduler.cpp:612`) checks it once before the
  three cast opcodes (`kActCastSelf`/`kActCastPlayer`/`kActCastTarget`) and returns a
  TRANSPARENT NoOp ("summon still live") so the scan falls through to the next combat
  rule, exactly like the cast-grace hold. Non-summon combat casts are byte-identical
  (helper returns false) and the AI-first-grace pacing is untouched; the helper only
  READS the active-effect list, the same off-worker read as the other combat guards.

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
- **#78 per-follower MFO master switch** — gate right after the `g_followers.find`
  in the per-follower service (`ServiceFollower`-caller path, `Scheduler.cpp`): if
  `!it->second.mfoEnabled`, SKIP the whole tick (combat + logistics) and `return`,
  so the follower stays vanilla. On the ON→OFF edge (`g_mfoDisabledSwept` latch,
  cleared in `ClearTransientState` + when re-enabled) run `Followers::ReleaseHeldState(id)`
  ONCE — same worker + helper as the dismissal sweep. `Logistics::ServiceFollower`
  carries a defence-in-depth `!a_state.mfoEnabled` early-out too.

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
- **Phase 2 APMF hand-off (2026-09-02, ALLOWANCE-TEMPLATE.md §7):** both exclusivity
  denies (`thunk`'s `!isWanted` branch, `CheckCastThunk`'s `ShouldDeny` verdict) now
  check `APMFBridge::IsOwnedCastActive(fid)` first and STAND DOWN (return the AI's own
  answer) when true — APMF's own CheckCast/CheckShouldEquip T2 hooks (separate
  APMF.dll) already enforce the identical exclusivity via the ch.8 claim for an
  owned-cast follower; running MFO's OWN deny too is redundant and fights it (two
  independently-configured deny paths, iCastControl slider vs. hard claim). `Want`
  (the consent GRANT / veto-removal) is UNCHANGED and still called unconditionally —
  that mechanism is MFO's alone, APMF only ever narrows a YES to a NO. Legacy
  (non-APMF / `bLegacyCastHybrid`) followers are unaffected: `IsOwnedCastActive`
  returns false for them, so both denies run exactly as before.
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
  (`:93`, worker-safe cache read) → `Actuation.cpp:127,149,234`, `Evaluator.cpp:307`.
  `Want` (`:101`) → `Evaluator.cpp:317`, `Actuation_Direct.cpp:1268` (F7 auto-cast),
  `Logistics.cpp:1350` (OOC hostile cast — seeds the `Check` at `:1353`; added to
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

### Logistics family: Logistics.cpp / _Cast / _Economy / _Loot / _internal.h / Logistics.h
**MODULE SPLIT (mechanical, v1.1 split pass):** one TU became four + a shared
internal header. Cross-module state/types/small helpers live as `inline` members
of `namespace MFO::Logistics` in `Logistics_internal.h` (ONE instance across the
TUs — it replaces the old single anonymous namespace; big cross-module helpers
are declared there and defined in their home module). Layout:
- `Logistics.cpp` (1700) — core tick: `ServiceFollower` (`:625`, INCLUDING the
  OOC cast dispatch `:~1080-1320` — concentration direct-force `:~1210`,
  fire-and-forget `:~1300`), drink (`DrinkBest`), `EquipTorch`/`HealExcludedWeapon`/
  `ShedOffRoleWeapon`, sinks, lifecycle + MSTK API, evaluator pure reads.
- `Logistics_Cast.cpp` (269) — mage-identity/school classifiers:
  `TargetMagicSchool:24`, `HasCastGambit:64`, `IsCasterFollower:101`,
  `TopTwoSchoolMask`, `LearnCarriedTomes:137`, school name/keyword helpers.
- `Logistics_Economy.cpp` (1034) — #21 economy: mage-apparel scoring,
  `UnlockCollegeTomes:220`, `EquipBestOwnedGear:291`, `BuildBuyThresholds:385`,
  `EconomyProbe:488`, public buy helpers (`MageApparelBuyKey:994` et al).
- `Logistics_Loot.cpp` (2227) — the loot judge + per-category looters,
  claim-and-release, navmesh reach, `AcquireEquip:538`, `LootEquipment:617`,
  `LootNearby:1477`, `StripCorpse:2108`, `RunExcursionScan:2170`.
- `Logistics_internal.h` (715) — shared substrate: all `g_*` maps/state
  (`g_svc:222`, `TravelIntent:283`, `g_travelSlots:323`, `g_stockMx:568`,
  `g_stockGear:569`, econ clocks), `Category`/`LootMode`/`WeaponRoles`/`Claim`,
  inline small helpers, cross-module declarations. NOT public API.
Adding shared state? Put it in `_internal.h` as `inline` (never a per-TU
anonymous-namespace copy — that silently forks the instance).
- **SAVE-COMPAT — `g_stockGear`/'MSTK'** (`Logistics_internal.h:569`, guarded
  `g_stockMx` `:568`, the one cross-thread map here): the only serialized state the cluster
  owns. `CopyStockGear` (`Logistics.cpp:1685`) → `Serialization.cpp:209`; `LoadStockRecord`
  (`:1690`) → `:332`; `ClearStockGear` (`:1695`) → `:256,646`. Only
  `IsPersistableID` FormIDs written, sets capped 512, unresolvable IDs dropped.
  Changing the map's key/value shape or record framing breaks the shed-protection
  ("Gauldurbow fix") — signature gear could get shed (dropped on the floor) after a load.
  **Not** cleared by `ClearTransientState` — cleared separately by `ClearStockGear`.
- `ServiceFollower` (`Logistics.cpp:625`) — sole caller `Scheduler.cpp:308` (worker). Sets
  `g_svc` (`Logistics_internal.h:222`) raw pointer valid only for that call — safe only because the
  worker services followers sequentially; parallelizing dangles it.
- `ShedOffRoleWeapon` (`Logistics.cpp:494`) — one off-role weapon per idle tick, **DROPPED on
  the floor** (no longer handed to the player; no value split, no knob — marth
  simplified). Disposal is `Actor::DropObject` (a world-ref/3D create) so it MUST
  go through `MainThread::Post` (`doDrop`, mirrors the #62 equip / ActivateRef
  hops in this file); on VR (`!MainThread::IsInstalled()`) it SKIPS rather than
  drop off-worker. **POST-BATTLE GATE:** early-returns until `kShedPostBattleDwell`
  (3 s) since `g_lastCombatSeen[id]`, stamped by `NoteInCombat` (`Logistics.cpp:1626`) ←
  `Scheduler.cpp:321` (the in-combat branch — the only place combat=true is seen,
  since this path is out-of-combat-only). Survives an `IsInCombat()` mid-fight
  flap: a real combat frame re-stamps `now`, so the dwell can't mature inside a
  lull (the field 2h-follower-hands-a-looted-mace bug). `g_lastCombatSeen`
  worker-only/no-lock (#4), cleared in `ClearTransientState`. Guards unchanged
  (never disarm/`inRoleWeapons>0`, `IsStockGear`, `IsCreatureWeapon`, socketed,
  `Catalog::IsExcluded`).
- `ClearTransientState` (`Logistics.cpp:1630`) → `Serialization.cpp:641`, after StopPump. Wipes
  the loot/drink/econ/travel maps (calls `Packages::LootTravelClear` first). Moving
  a clear out, or calling while the pump is live, races a worker insert (UB).
- Pure reads (evaluator + economy, shared classifiers): `PotionRestores` (`Logistics.cpp:249`),
  `AmmoIsBolt`, `CountPotions`/`ArrowCount`/`BoltCount` (`:305-362`) →
  `Evaluator.cpp:397-409` + `TradeBridge.cpp:52-71` (buy side shares them so bought
  supply matches looted). `ComputeWeakPotionFloor` (`Logistics.cpp:320`) ← `plugin.cpp:288`
  (after `Catalog::Load`).
- **Alias/travel:** `g_travelSlots` (`Logistics_internal.h:323`, `kMaxLootSlots=4`) maps follower→loot
  alias pair. Travel fill is **engine-serialized**; every exit path MUST call
  `Packages::LootTravelClear` (combat via `ReleaseTravelOnCombat` `Logistics.cpp:1655` ←
  `Scheduler.cpp:328`; cap/leash/dismissal/revert). Leash hysteresis guards
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
- **Loot scan is MULTI-CELL** (`LootNearby` `Logistics_Loot.cpp:1477`; cell set built just below it):
  follower's + player's + live travel-target's ATTACHED parent cells, all anchored
  to refs in hand — **never** `TES::ForEachReferenceInRange`/worldspace derefs
  (crash4). Dropping back to one cell re-blinds exterior scans across cell borders
  ("second gold pile on the same table"). Idle blocklist reassess (`ServiceFollower`) is
  AGE-GATED (≥10s): a full wipe let follower B erase follower A's 200ms-old fail
  verdict → instant same-target redispatch churn.
- **UNIFIED LOOT-FAILURE MODEL + in-reach drain (perf/stall pass, 2026-09):**
  (a) candidate sort (`LootNearby`, `Logistics_Loot.cpp:~1830`) is failed-recently-
  LAST then closest-first — a path-troubled target is DEPRIORITIZED, never removed;
  (b) a NON-LOOSE in-reach source is DRAINED whole (`StripCorpse` from inside
  `LootNearby`) and the normal-mode loop keeps draining further in-reach sources
  the same tick (`drained` counter; movement dispatches still end the tick);
  (c) GROWN GRAB (`g_grabGrow`/`GrabRadiusFor`/`NotePathFail`,
  `Logistics_internal.h:~402-432`): each path-fail (off-navmesh pre-gate, walked
  no-progress stall) widens that ref's from-range grab radius
  `kArrivalDist+100/fail` capped 600u — the PRIMARY stall cure; player-bubble +
  leash + `TierReleased` dibs still gate a grown grab, loose refs excluded;
  (d) the off-navmesh PRE-gate (`Logistics_Loot.cpp:~1915/~2000`) is TRANSIENT-only
  (`MarkTravelFailed`, never a sticky strike) — only a walked no-progress stall or
  the loose/unacquirable case reaches the sticky set, whose cooldown is now
  `kTravelStickyCooldown=60s` (was 5 min). Walk paths still hard-skip inside the
  25s fail cooldown (re-walking resets the stall clock and would defeat the
  verdict); grab paths never consult the blocklist. Weakening (d) or removing the
  walk-skip re-opens the frozen-Erik churn loop; removing (a)'s sort key stalls
  followers on unreachable-first ordering again.
- **Sinks** (`RegisterSinks` `Logistics.cpp:1583` ← `plugin.cpp:297`): `ContainerSink`
  (`TESContainerChangedEvent`) — **direction filter mandatory** (`newContainer==
  PlayerID()`, `ContainerSink` in `Logistics.cpp`) or it re-fires on its own removal (MAO infinite-credit loop);
  only QUEUES to the worker. `BeastHeadSink` (`TESEquipEvent`, `Config::g_beastHeadFix`)
  → `KeepHeadClear`. `SweepBeastHeadsOnLoad` (`Logistics.cpp:1602`) ← `plugin.cpp:360`.
- `OnFollowerRemoved` (`Logistics.cpp:1668`) ← `Followers.cpp:306` (dismissal alias eviction).
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
  into `AcquireEquip` (`Logistics_Loot.cpp:538`, v1.0.38 SAFE path: `MainThread::Post` +
  `ActorEquipManager::EquipObject`, **never DoReset3D** #62; MEO gem capture +
  `QueueGemMove`). `a_src==nullptr` ⇒ the follower already owns the item (buy / owned
  upgrade). `LootEquipment` routes through it; the **mage apparel selection is now the
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
never strips naked). The rated-armor branch is unchanged (strictly-better-only).

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
**Consumers routing on:** Phase 5 (`sharedGrowthEnabled`); **Phase 6a** — `Board.cpp`
`DrawFieldKit` iterates `Manifests()[].boardTab.declared` to decide the hosted board-tab
count (was hardcoded `snap.prog->active`); `BuildGenericManifests` sets `boardTab.declared=true`
+ `label`. **Phase 6b** — `BoardProgSnap`/`BoardFollowerView` wrapped behind a generic
`BoardTabView` (`ProgAllocator.h:378`, `CopyBoardTabViews`). **Phase 6c (Phase 6 COMPLETE)** —
(1) the tab CAPTION is `boardTab.label`, sourced from the add-on's own `MFOP_BoardTabLabel`
MESG (FULL "Progression", manifest entry[1] before the classes FLST; `BuildGenericManifests`
captures its FULL, no DLL literal) — `Board_Progression.cpp:43` `BeginTabItem(hostedTabLabel)`. (2) The
board edit queue's progression verbs collapsed to ONE generic carrier `EditKind::AddonAction`
+ `EditCmd::verbId` (`AddonVerb` enum, `Board_internal.h:45`); the `verbId`→backend dispatch
(`ApplyEdits`, `Board.cpp:1775`) stays progression-shaped (Phase 7/9). The tab BODY
(all of Board_Progression.cpp) + view payload are still add-on-typed (Phase 7/9). `Get()` read by ProgAllocator (`:143,
150,437,666,935,1047,1264,1554`) + `Board_Progression.cpp:202`. `kAddonPlugin=
"MFO_Progression.esl"` (`:30`). **What breaks:** the catalog is the load-time drop
oracle — `CoSaveLoad` drops any perk alloc whose node is no longer in `Get()`
(`ProgAllocator.cpp:2117`), so re-tuning the add-on's declared verdicts (the
`MFOP_EntryPointVerdicts` GLOBs in `MFO_GenerateESP.py`, read via
`ReadEntryPointVerdicts`→`g_verdicts`) or `ClassifyRank` (`:329`) silently changes which
saved perks survive a load (with an auto §17 refund). The verdicts are ESL DATA now, not a
DLL table — changing them is a generator+regen change, not a code edit.

### ProgAllocator.cpp / ProgAllocator_Hms.cpp / ProgAllocator_Manifest.cpp / ProgAllocator_internal.h / ProgAllocator.h — component 2: allocator + 'PRGN' owner  ⚠️ SAVE-LAYOUT
The engine-mutating half: writes perks (`AddPerk/RemovePerk`+`ApplyPerksFromBase`)
and skill AVs onto real actors, runs the level poll, owns 'PRGN'.
- **Module layout (mechanical split, 2026-08-31):** `ProgAllocator.cpp` (2333) =
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
  f32, manualPoints f32(v2)}×N, baseCount u16 + {av u32, value f32}×N,
  [v5 §HMS block APPENDED at END: per pool {H,M,S} order {hmsBaseline f32,
  **v5-ONLY hmsTarget f32 (dropped in v6 — recomputed)**, hmsSkew f32,
  hmsCumulative f32}, then battlesSinceLevelUp u32, battlesOffClass u32,
  offClassPool u8, hmsCaptured u8, **[v6: hmsZeroAwardStreak u8, hmsGrantRemainder
  f32×3, hmsAwardAccum f32]**]}`. Bounds: 4096 followers / 1024 perks / 64 skills. New fields MUST go
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
  `Respec`/`SetManualSkills`/`ApplyManualSkillPoint` (`ProgAllocator.cpp:1382-1727`).
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
`plugin.cpp:362`; `OnHotkey` (`:436`) ← `Board.cpp:1512`. Writes no save record;
its perk/AV mutations are runtime-only. Safe to delete without touching saves; only
`plugin.cpp:362` + `Board.cpp:1512` reference it. Idempotent reapply guarded on
`GetPerkIndex` (`:468`).

---

## 6. Board / UI / Papyrus — `Board.*`, `Papyrus.*`

### Board.cpp / Board_Progression.cpp / Board_internal.h / Board.h — the Field Kit overlay
Installs three trampoline hooks at plugin load, draws live state via ImGui on the
**render thread** from a mutex-guarded snapshot, funnels all rule edits through a
main-thread-drained edit queue. **ImGui/`imgui_impl_win32` = vendored, do not read.**
- **Module layout (mechanical split, 2026-08-31):** `Board.cpp` (2190) = shell —
  shared render state, input translation, `PushSkin` (`:290`), `DrawFieldKit`
  (`:397`, Followers+Gambits tabs + tab bar + cascaded-B close), `DrawHud`
  (`:1251`), the 3 hooks (`WndProcHook:1321`, `D3DInitHook:1345`,
  `InputDispatchHook:1477`), public API + `ApplyEdits` (`:1775`) +
  `PublishSnapshot` (`:1995`) + `Install` (`:2181`). `Board_Progression.cpp`
  (1234) = the hosted progression tab body, ONE function `DrawProgressionTab`
  (class prompt, skill table, perk-dome window, node/skill popups, respec) —
  called from `DrawFieldKit` at `Board.cpp:1174`; future progression-tab work
  lands here. `Board_internal.h` = the shared substrate (Board TUs only):
  `EditKind`/`AddonVerb`/`EditCmd`, the edit queue `g_editMx`/`g_edits`/
  `QueueEdit` (`:69`), `g_fontHead`, `MenuSkin`/`kSkins`,
  `DrawProgressionTab` decl — every definition in it is `inline` (ODR: one
  shared instance across TUs, same pattern as `Logistics_internal.h`).
- `Install()` (`Board.h:113`) — caller `plugin.cpp:393` only. **MUST** install
  before renderer init (only place, `plugin.cpp:391`); moving it → `D3DInitHook`
  misses init → `g_ready` never set → overlay silently disabled. Writes a 256-byte
  trampoline with 3 game-version-keyed `RelocationID`/offset pairs (`Board.cpp:2183`)
  — a bad offset corrupts the call site.
- **Snapshot carries all actor-derived display data** (render thread reads plain
  cached values, never a live actor — #4): `FollowerRow` (`Board.h`) holds vitals as
  pct **and** raw `health/magicka/staminaCur/Max` (Followers tab, `Vocab::VitalCur/
  VitalMax`), and `knownSpells`/`teachableSpells` are `SpellPick`/`Teachable` structs
  carrying precomputed `magickaCost` (`spell->CalculateMagickaCost(follower)`, actor
  overload) + a synthesized `tooltip` (`SpellTooltip`, effect name+mag/dur/area) —
  all filled in `PublishSnapshot` (main). The gambit spell-picker renders the hover
  tooltip via `DrawSpellHoverTooltip` from those cached values.
- **#78 Followers-tab MFO toggle** — the tab's FIRST column is a per-row checkbox
  bound to `FollowerRow.mfoEnabled` (mirrored from `FollowerState::mfoEnabled` in
  `PublishSnapshot`, both the active + retained builders). The `##followers` table
  is now 8 columns. On toggle it `QueueEdit`s `EditKind::SetMfoEnabled` (param 0/1);
  `ApplyEdits` flips `it->second.mfoEnabled`. The release-on-disable runs on the
  Scheduler tick's OFF edge, not in `ApplyEdits`.
- **Thread discipline:** `DXGIPresentHook` (render thread) copies `g_snapshot` under
  `g_snapMx` **before** taking `g_ioMx` (`:1438`); reversing = render-thread deadlock.
  The two mutexes are never nested (#6). Draw functions **never touch `g_followers`**
  — every mutation is a `QueueEdit` (`Board_internal.h:69`, sites in both draw TUs)
  drained by `ApplyEdits`
  (`:1775`). **Rule edits key on `Gambit.uid`, not row index** (`:1889` — resolve by
  identity #31); applying by index misapplies a command to the wrong rule.
- **Correction to the header:** `PublishSnapshot` (`Board.h:116` says "MAIN THREAD
  ONLY") actually drains on the **task worker**, the same context that
  owns `g_followers`/`Scheduler::Tick` — so its `g_followers` reads are safe *there*.
  But addon edit verbs ride `MainThread::Post` (`:1803`) because `g_prog` lives on
  the real main/poll thread. Callers: `Diagnostics.cpp:216,263`.
- `ClearPendingEdits` (`Board.h:124`) ← `Serialization.cpp:603` (revert) — drops
  queued edits so a command from the old save can't hit a freshly loaded one.
  `SetHud` ← `plugin.cpp:365`, `Diagnostics.cpp:94`, `Serialization.cpp:621`.
  `IsOpen`/`IsAvailable`/`Toggle` ← Diagnostics (publish cadence + Field Orders
  power). `ToggleHud` (`Board.cpp:1702`) is **dead** (no caller).

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
sinks or moves never flush). `QueueGemMove`/`WornUid`/`Available` ← `Logistics_Loot.cpp`
(`AcquireEquip:538` + `LootEquipment`, worker tick). `PreviewWithGems` (`:105`, main-thread queries) has no
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
GetGemDetails are main-thread-only; SocketGem/UnsocketGem queue to main. **No dedup
state** — GetEmptySocketCount reflects only LANDED sockets and async ops land within a
frame or two (<< the ~1 s cadence), so the next pass never re-issues; domain is matched
here so MEO never rejects into a retry loop. Tier 1 conservation always runs (fill any
domain-matching gem); tier 2 effect-aware (MCM `bMeoAwareGems`, default OFF) ranks by a
class-preference heuristic on gid/name + magnitude and swaps up a socketed gem a better
loose gem beats. Whole feature no-ops below MEO v3.

### APMFBridge.cpp / APMFBridge.h — optional APMF client (MODERATOR model, Phase 3)
Makes MFO a client of the SEPARATE APMF.dll (AI Package Management Framework) via the
byte-shared `APMF_API.h` (C-ABI POD; append-only, mirror APMF's copy). `Acquire()` ←
`plugin.cpp:289` (kDataLoaded): `GetModuleHandleA("APMF.dll")` + `GetProcAddress(
"APMF_GetInterface")` → `fn(kABIVersion)` → cast up to `APMF_API_v2*`; **null-degrades** with one
log line if APMF is absent/old — MFO then runs the legacy cast hybrid, byte-identical.
- **THE MODEL: APMF ARBITRATES; MFO EXECUTES.** APMF never generates behaviour (its channels are
  arbitration-only). This bridge only ever CLAIMS facets (`ClaimCasting`/`ClaimCombatTarget` →
  `RequestEx`/`Repoint`, basis 200); MFO executes the real cast with its OWN mechanisms.
- **THE OWNED CAST (default), a real AI-DECIDED animated cast — `Actuation::CastOn` FF-non-self
  hostile branch (`Actuation.cpp` ~`:487`, worker):** SELECTION is `Loadout::Prepare`'s
  `EquipSpell(..., LeftHandSlot())` (§0.28) — **never** a hand-written `selectedSpells`/`currentSpell`
  (ENGINE_NOTES §585: a hand-write desyncs the engine's select/deselect bookkeeping; the original
  `SelectCasterSpell` helper did exactly that, on the WRONG hand, every tick, resetting the charge
  before release — root cause of the fire-and-forget hang, fixed 2026-09-02 and removed). At `Grip::
  Caster` with a DIFFERENT spell already in hand, `Loadout::Prepare` (`Loadout.cpp:191-213`) also
  **neutralizes a competing spell in the OTHER hand** (`DeselectSpell`) — but ONLY at `iCastControl`
  level 4 (exact); looser levels leave that hand to the AI (`CastExempt`). MFO then (a) CLAIMS the
  cast + combat-target facets via APMF (`Actuation.cpp:522-523`) and (b) `Targeting::Command(follower,
  target->GetHandle())` commands the target (its UpdateCombat hook re-asserts `currentCombatTarget`,
  `:533`) — **every owned tick, deliberately not deduped here**: `EnsureClaimLocked`
  (`APMFBridge.cpp:70`) already no-ops an unchanged claim but still stamps its liveness timestamp on
  every call (`:144`/`:173`), which is what keeps the claim alive past its `kExpiry` backstop (500ms,
  `:56`/`:192-194`) — a per-follower dedupe latch that skipped these calls on an unchanged tick was
  tried 2026-09-02 and reverted the same day (Fable review) for starving the claim mid-cast
  ("casting facet released" while the AI was still charging); `Targeting::Command` has its own
  unchanged-latch dedupe (`Targeting.h:37-41`) so calling it every tick is equally cheap.
  `CasterConsent::Want` (granted at `:460`, every tick — same reasoning) permits our spell + denies
  competing; the
  **Cast-biased combat style** (`Scheduler.cpp:~805` sets `Stance::Cast` when a cast is wanted → raises
  the magic score) makes the follower's own AI DECIDE to cast. Result: the vanilla AI casts our spell
  at our target, FULL animation, still MOBILE (ENGINE_NOTES §0.15a/§0.27/§0.28). Resolves
  [[cast-animations-deferred-to-post-town-polish]]. **GRANULAR: movement facet is NOT claimed** — the
  follower keeps kiting while it casts. **NO force on this path** — `CastSpellImmediate` never runs
  here; the returned `{NoOp,"owned cast: AI deciding (animated, mobile)"}` (or `"...latched
  (unchanged)"` on a deduped tick) just lets the AI act. Concentration never enters this branch
  (bounded direct fork returns earlier — exact-bounding intact).
- **Claim lifecycles (arbitration records, `g_owned` mutex-guarded — worker+main):** casting =
  PER-CAST (refreshed each winning cast tick; released crisply by `ReleaseCasting` ← `Scheduler.cpp:~894`
  on `!castSeen`). combat-target = PER-COMBAT (created by the cast directive; re-pointed via APMF
  `Repoint` when the foe changes; the ATTACK directive `ClaimCombatTarget(create=false)` ← `Actuation.cpp:~1009`
  re-points an existing claim so cast→melee follows; `RefreshCombatTarget` ← `Scheduler.cpp:~330` keeps
  it alive every in-combat tick; released only at combat end via the expiry sweep). `Tick()` ←
  `Diagnostics.cpp` (~`:334`) is the per-claim 500 ms expiry.
- **Gating:** owned model active iff `Available() && bApmfCast (INI, default ON) && !bLegacyCastHybrid
  (MCM, default OFF)`. Toggle ON, or APMF absent → the ORIGINAL AI-first-wait + force-on-miss package
  hybrid (unchanged legacy branch below the owned block — the ONLY place `CastSpellImmediate` force
  and the rooting UseMagic package survive).
- **`IsOwnedCastActive(follower)` (Phase 2, ALLOWANCE-TEMPLATE.md §7):** worker- AND
  combat-thread-safe read (the same `g_mx` every other accessor takes) — true iff the
  follower holds a LIVE cast-select claim (`g_owned[id].spellHandle` valid).
  `CasterConsent.cpp`'s two exclusivity denies and `CombatStyle.cpp`'s equip gate all
  consult it to stand down for that follower once APMF's own T2 hooks (a separate
  APMF.dll) enforce the identical exclusivity via the SAME claim — avoiding two
  independently-configured deny mechanisms disagreeing. Does NOT touch consent
  (`CasterConsent::Want`'s veto-removal stays MFO's alone, called unconditionally).
- **Runtime-only — NO save/co-save state**; `ClearTransientState()` ← `plugin.cpp` kPreLoadGame
  (after `StopPump`). `g_apmf` is `std::atomic`. APMF holds ZERO MFO code; this bridge is the ONLY
  MFO code aware of APMF.
- **PASS B (ch.9/ch.7 channels): `OfferPackage`/`ReleaseOfferPackage` (package-offer, `kIntent_
  OfferPackage`) + `ClaimCombatActionDeny`/`ReleaseCombatActionDeny` (combat-action deny,
  `kIntent_CombatAction`, `param.ival` category bitmask) — SAME `g_owned`/`EnsureClaimLocked`
  machinery, two new fields (`packageHandle`/`package`, `actionHandle`/`actionMask`) + an ival
  twin `EnsureIvalClaimLocked`. `ClaimCombatActionDeny` is built but NOT wired into loot-travel
  (see Packages.cpp below) — MFO's own PACKAGE-THEFT guard already concedes loot-travel to a
  live combat package on purpose.

### Packages.cpp — APMF LOOT-TRAVEL (ch.9 0x49 route, PASS B, the Cicero fix)
`LootTravelFill/Retarget/Clear/EvictIf` (`:1380-1710`, see the OPTION A entry above) now try
`APMFBridge::OfferPackage` FIRST, before the alias/static-priority-60 race: claims the
package-offer facet naming `Forms::APMFLootTravelPackage(slot)` (4 NEW packages, `Forms.h`
`kAPMFLootTravelPackage0-3` = `0x836-0x839`, `MFO_GenerateESP.py make_apmf_loot_travel_package()`)
so APMF's 0x49 hook hands the follower that package directly and unconditionally — no alias fill,
no arbitration race, so a follower package-locked by an outranking custom AI framework (Cicero)
still gets walked to the loot. Falls through to the unchanged legacy alias route on any decline
(APMF absent, `bApmfLootTravel` off, or the package write below fails) — **byte-identical
degrade-when-absent**.
- **RUNTIME-HANDLE TARGET (no alias to carry it):** the new packages are authored PLDT type 0
  ("Near Reference", `RE::PackageLocation::Type::kNearReference`) instead of type 8 (alias);
  `SetAPMFLootTravelTarget` (`Packages.cpp` ~`:628`) overwrites `PackageLocation::data.refHandle`
  at runtime via `ReadLocation`, the Location twin of `ReadTarget` (SAME `kPointerOffFromIPackageData`
  offset model, generalised from `PackageTarget` to `PackageLocation` — **UNVERIFIED IN THE FIELD**
  for a Location input specifically, pending the Cicero test; guarded identically: any layout
  mismatch declines loudly, never a blind write).
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
  APMF-delivered package must read as "on the travel package", never as "stolen" (theft cannot
  actually happen on the APMF route — 0x49 wins unconditionally for as long as the claim holds —
  but the guard must not misfire and churn regardless).
- **`ClaimCombatActionDeny` is NOT wired here** (see APMFBridge.cpp above) — left available, not
  scoped into this dispatch, per marth's "keep it scoped" instruction and the existing
  concede-to-combat design.

### TradeBridge.cpp / TradeBridge.h — Papyrus econ bridge (#21) ⚠️ SCRIPT-COMPAT
Native owns the trade DECISION; merchant read/mutation runs in `MFO_Trade.psc`
(native `GetInventory`/`GetGoldAmount` CTD on merchant chests). `RegisterFuncs()`
(`:207`) ← `plugin.cpp:407`, registers **10 Papyrus natives** on class `MFO_Trade`
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
- **#21 SELL bypass + pricing** (`EconomyProbe`, INI-only, no MCM). `bMerchantPerkBypass`
  + `xMerchantPerkID` (0x00058F7A): a follower holding the merchant perk (dual-check
  `GetActorBase()->GetPerkIndex` + `HasPerk`, the `OwnsExactPerk` idiom) sells past the
  vendor's VEND filter. `bSpeechPricing`: `SellRow.value = lround(baseValue *
  sellFraction)`, `sellFraction = 1/(fBarterMax-(fBarterMax-fBarterMin)*speech/100)`
  (0.30→0.50 over Speech 0→100). BUY stays base value. (Per-perk `kModSellPrices`
  boosts are NOT applied — needs a CI-verified EPFD read; flagged.)

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
- **Wave-1 worker-quiesce API:** every MFO `AddTask` body now runs under a
  `PumpTickGate(epoch)` RAII (`:77`) — STOREs `g_tickActive=true` (seq_cst) then
  re-checks the epoch (Dekker handshake, closes the check-then-set TOCTOU), and
  bails if a `StopPump`/`PausePump` is in progress. `PausePump()`/`ResumePump()`
  (`:515`/`:532`) are the RESUMABLE quiesce for `SaveCallback` (SEV-1): PausePump
  drains the worker like StopPump but WITHOUT tearing the pump down, so the save
  can iterate `g_followers` with no worker insert racing it; ResumePump lifts it.
  MUST be paired (RAII across the save). Off-worker sink bodies gate on
  `IsTrackedFast` before taking the epoch.

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
- `Refresh` (`:254`, the eviction hub) — **runs on the JOB WORKER** (`Diagnostics.cpp`
  tick), not the main thread; it rebuilds `g_active`/`g_activeIds` and republishes the
  `IsTrackedFast`/`ActiveSnapshot` mirror under `g_mx` (`PublishActiveMirror`). On drop
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
| `Logistics::RegisterSinks` (TESContainerChanged, TESEquip) | `plugin.cpp:297` → `Logistics.cpp:1495` | direction filter mandatory |
| `MEOBridge::RegisterSink` (TESEquip) | `plugin.cpp:298` → `MEOBridge.cpp:75` | optional |
| `Diagnostics::Install` (TESSpellCast, TESHit, MenuOpenClose, + Probe crosshair) | `plugin.cpp:299` → `Diagnostics.cpp:397` | + the worker pump |
| `TradeBridge::RegisterFuncs` (10 Papyrus natives) | `plugin.cpp:407` → `TradeBridge.cpp:209` | script ABI |
| `MEOBridge::Acquire` (MEO interface) | `plugin.cpp:289` | external ABI |
