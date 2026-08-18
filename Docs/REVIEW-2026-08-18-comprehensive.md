# MFO Comprehensive Fable Review — 2026-08-18

6 parallel Fable reviewers (coding + logic), read-only, judged against
INVARIANTS/MAP/CLAUDE.md. Consolidated, de-duped, ranked. Tags: **[65]** =
introduced/exercised by the v1.0.65 self-cast+AUTO work (release-relevant);
**[PRE]** = pre-existing (shipped in v1.0.64 or earlier), not a v1.0.65 regression.

## Headline
- **Saves are safe.** The co-save version-reader chain is FULLY INTACT (FLWR v1–4,
  MSTK v1, PRGN v1–4, FWPN v1); ResetAllState order, evict-marker (non-actor
  XMarker), resetAI=false, hooks install-once/VR-refused/members <0x68, §4.6
  arbitration — all verified. **v1.0.65 introduces NO save-format regression.**
- **One SEV-1 cluster, pre-existing:** the follower lists (`g_active`/`g_followers`)
  are read from off-worker threads (combat hook, event sinks, MainThread::Post poll,
  SaveCallback) without a snapshot/mirror, racing the worker's `Refresh`/insert.
- **The v1.0.65 feature needs ~5 closes** before shipping ON — chiefly our own
  half-finished F3 fix (combat side) + a combat dispel-beat.

## SEV-1

- **[PRE] CONFIRMED — `IsTracked` walked from the COMBAT thread.** `CasterConsent.cpp:162`
  `ConcUnboundedDeny` → `Followers::IsTracked` iterates unlocked `g_active`
  (`Followers.cpp:170`) from the CheckStartCast/CheckCast thunks (`:499`, `:810` —
  non-main). At `iCastControl>=4`+FORCE, every concentration cast in the world walks
  it concurrently with `Refresh` rebuilding it → UAF CTD. Violates "combat-thread
  hooks read FormIDs/atomic mirrors, never the follower lists." **Fix:** g_mx-guarded
  `unordered_set<FormID>` mirror maintained by `Refresh`; probe that.
- **[PRE] PLAUSIBLE — `SaveCallback` iterates `g_followers` with no worker drain.**
  `Serialization.cpp:92-175`. Worker `Refresh`/`TryEnsureRecord` insert + `Scheduler::Tick`
  rule-writes + Board `ApplyEdits` realloc the map mid-save → iterator UAF / torn FLWR
  = corrupt co-save. MSTK/FWPN/PRGN defend (lock+copy / main-domain); FLWR is the
  unguarded odd one. **Fix:** PausePump/ResumePump around SaveCallback, or snapshot.
  NOTE: hinges on BSJobs-worker vs main-thread concurrency (see below).

**Concurrency crux (ENGINE_NOTES §0.37):** AddTask drains on `BSJobs::JobThread`;
`MainThread::Post` drains inside the player Update on the true main thread — TWO
threads, no proven mutual exclusion. If they overlap, the whole g_active/g_followers
"one serial domain" assumption (#4) is unsafe and the below SEV-2 races are real.
The combat-thread IsTracked case is unambiguous regardless. **Action: prove or
disprove BSJobs-vs-main serialization and write it into INVARIANTS.**

## SEV-2

- **[65] Combat half of the F3 tri-state fix is missing.** `Actuation.cpp:455-460`
  (CastOn self fork) + `:236-239` (ConcentrationCast) return `Fired` for a pacing
  `Refreshed` tick (nothing applied). A persistently-true `cast_self` in COMBAT
  starves attack/drink/heal below it, and `lastFired`/`[eval] fired` logs lie on
  no-op ticks. **Fix:** map `Applied→Fired`, `Refreshed→transparent NoOp` in both
  combat call sites, mirroring `Logistics.cpp:3987-3996`.
- **[65] Combat suppression window re-introduces the dispel/re-cast beat.**
  `Actuation.cpp:1123` `releaseSec=2.0f` vs suppression window (`fSuppressWindow`
  1.5×temperament ≈1.68s) + round-robin gap (133ms×party). Party ≥3: >2.0s →
  `SelfCastReconcile` dispels the live self-buff and re-applies — the exact beat the
  300s-cap fix just removed, back at window cadence in combat; raising fSuppressWindow
  breaks it outright. **Fix:** refresh `lastFired` on the suppressed-at-cast-rule path,
  or `releaseSec = fSuppressWindow*1.12 + 0.133*partySize + margin`.
- **[PRE] Ally-selector target → Attack/PowerAttack friendly-fire.** `Evaluator.cpp:465-469`
  fills `choice.target` with the lowest-HP ally (PickAlly includes the PLAYER); board
  freely pairs "Ally HP below → Attack", and `Targeting::Command` force-writes the
  combat target with no hostility check → follower attacks the wounded teammate/player.
  The code already gates this for `kActCastTarget` (`:479-484`); the attack path is the
  same hole. **Fix:** refuse ally-selector target for kActAttack/kActPowerAttack (treat
  as no-match/transparent).
- **[PRE] Logistics `cast_target` ignores manual Target picks.** `Logistics.cpp:3909-3935`
  handles only AUTO + selector-filled target; a logistics rule with Target=Nearest-ally/
  named/Player is evaluated true then silently skipped forever (same class as the just-
  fixed AUTO gap). **Fix:** resolve via the Fire ladder before the skip, or gray manual
  picks out of the logistics picker.
- **[PRE] Worker-thread `EquipObject`/`EquipTorch` (#62 race).** `Logistics.cpp:3403-3405`
  (HealExcludedWeapon), `:3361` (EquipTorch) run engine equips on the job worker,
  rebuilding biped 3D → the invisible-head/render race the file's own #62 fix Posts
  around. **Fix:** capture FormIDs + `MainThread::Post` like `doEquip`.
- **[PRE] `Begin` at `Phase::Done` steals the cast slot without `ClearAlias`.**
  `Packages.cpp:787`. Done set by CastPopDue inside Running (incl. ff teammate-crossed);
  a second follower's cast in the same tick rewrites the shared package + displaces the
  holder with no `InterruptCast`/EvaluatePackage → a hostile beam released because a
  teammate walked in keeps streaming. Defeats ffWatch; likelier with AUTO fan-out.
  **Fix:** `ClearAlias("superseded")` before proceeding at Done, or return Busy.
- **[PRE] StripCorpse bypasses claim-and-release dibs.** `Logistics.cpp:2814-2853` — the
  excursion arrival full-strip loots Gear/Valuables with no released-check, taking a
  fresh kill's enchanted sword+gold ahead of the player's grace. **Fix:** gate Gear/
  Valuables inside StripCorpse with the tier-release predicate.
- **[PRE] Economy sells snapshotted stock gear.** `Logistics.cpp:3078-3097` sell loop
  lacks `IsStockGear` → a custom follower's spare signature weapon / unworn own armor is
  sold. bEconomy off by default. **Fix:** `if (IsStockGear(...)) continue;`.
- **[PRE] StopPump drain check-then-set TOCTOU** (`Diagnostics.cpp:256` vs
  `:422-432`) and **sink lambdas + IsTracked outside the drain** — part of the SEV-1
  concurrency cluster. **Fix:** set `g_tickActive` first then re-check (Dekker); one
  shared epoch-guard for every MFO AddTask body.
- **[65] Progression main-thread reads of g_active/g_followers** race worker Refresh —
  part of the SEV-1 cluster (`ProgAllocator.cpp:934,1275,1529,1685,1829`). Same fix:
  snapshot + queue the TryEnsureRecord mutations.
- **[PRE] Cast-alias sweep player fallback** (`Packages.cpp:1244/1311`) — if marker mint
  fails, force-fills the PLAYER into a UseMagic-ALPC alias (#48/#73 furniture pattern).
  Low prob, high blast radius. **Fix:** use `EvictMarkerRef()` (no player fallback),
  matching ClearAlias.

## SEV-3 (batch — hardening/polish)

- **[65]** Magicka can go negative (AUTO validates N vs one snapshot; deduct unclamped)
  — `Actuation.cpp:960/1019/746`. Fix: `cost = min(cost, have)`.
- **[65]** Hostile AUTO LoS gate near-dead — never `Sightline::Want` for fan-out foes
  → effects land through walls (`Actuation.cpp:1282`). Fix: Want per candidate, or doc.
- **[65]** ConcentrationCast dead-`self` residue comments (`Actuation.cpp:246-296`) — F6
  leftover; misleads toward the barred package self-route.
- **[PRE]** FWPN load sweep force-unequips a spell/off-hand (`Actuation.cpp:1586`) — only
  weapons carry the lock. Fix: `if (held->As<TESObjectWEAP>())`.
- **[PRE]** Unobserved concentration stream unwatched 12s (ffWatch/holds inert until
  castSeen) — `Packages.cpp:1049`. Fix: run ffWatch in Running regardless of castSeen.
- **[PRE]** Targeting writes 2 CombatController members with NO static_assert
  (`Targeting.cpp:103-104`; 0x30 today per ENGINE_NOTES:310). Fix: add offsetof<0x68 asserts.
- **[PRE] Static maps escaping ClearTransientState** (pattern): `s_nextSense`
  (Scheduler:316), `s_logiCastUntil` (Logistics:3963), `Packages::ClearTransientState`
  zero callers. Fix: hoist + clear on revert. + FWPN write unclamped vs read-clamp-64
  (Actuation:1544 vs :1555). + FLWR newer-version abort abandons MSTK/PRGN/FWPN reads.
- **[PRE]** Board close-grace "30 frames" ≈ 16s (g_frame ticks ~532ms) — a missed button
  release swallows input up to 16s (`Board.cpp:57/426`). Fix: tick g_frame in Present
  hook or use steady_clock.
- **[PRE] Board/gambit logic:** SetCond keeps param across ParamKind change (silently
  always/never-true); suppression window keyed by rule index not uid; IsDark duplicates
  kCondIsNight; combat table exposes supply conds Vocabulary.h says it forbids (per-tick
  GetInventory); board `r.name` null-guard missing (`Board.cpp:3199`); ContainerSink
  counts MFO hand-backs as player takes (false claim release); StripCorpse equips worse
  of two same-slot upgrades; LootEquipment peek ignores carry weight.
- **[PRE] Progression:** pre-v3 class still wiped in an addon-absent session
  (synthesize `MFO_Progression.esl`+known local id instead); legacy ordinal indexes the
  GLOBAL class list (breaks with a 2nd addon); economy knobs unclamped (skillCap≤0
  neutralizes writes, negative respec cost = grant); revert edit-leak race
  (capture g_pollGen at post); O(N²) tree walk main-thread hitch; §16 manual points
  retroactive repricing + no undo (design decision); DispatchAlias hardcodes alias 0.
- **[DOCS] MAP.md/CLAUDE.md drift** (recurring): co-save map says "3 records/PRGN v2",
  actually 4 records / PRGN v4 + FWPN v1; Packages.cpp line numbers drifted (now 1618);
  §17 contract comments stale; `Followers::Refresh` doc says "main thread only" but runs
  on the worker; CastSelfDirect docs (partially fixed).

## Verified CLEAN
Co-save codec (all versions), ResetAllState order, evict marker + resetAI, hooks
(install-once/VR/members<0x68), §4.6 arbitration, CastAuto enumeration + F1 UAF fix,
no double magicka-deducts, first-match-wins scan consistency, no dead opcodes beyond the
2 gaps above, PickFoe/PickAlly handle discipline, frozen opcode strings + enums, #76
force-hold, hybrid forced cast, Targeting redirect-only, travel machine (bounded),
ownership/vendor CTD handling, controller parity (#36), B/close single-path, board edit
queue (identity-keyed, single-fire), rule-editor↔evaluator parity, PRGN perk/skill
idempotency, detection seam.
