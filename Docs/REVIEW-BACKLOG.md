# REVIEW-BACKLOG.md — deferred Fable review findings (MFO)

**What this is.** Findings a Fable review raised that were DEFERRED under `CLAUDE.md` rule 9,
not dropped. A review round returning nothing above SEV-3 ends the cycle; the SEV-4/SEV-5
remainder lands here and is drained as ONE batch at a natural boundary — before a release cut,
or before a field cycle touching that subsystem.

**Deferred is not dropped.** Each entry keeps the finding's VERBATIM text, its severity, the SHA
it was raised against, and the reviewer's reasoning. Nothing here may be closed by assertion; it
is closed by a commit plus a review, or by a reviewer showing it wrong with evidence.

**NEVER deferrable (rule 9 carve-outs), whatever the nominal severity:**
- co-save layout, threading, or ABI / byte-shared-header findings
- anything the NEXT field cycle will exercise

**Read this before editing the named subsystem.** MAP.md's "What breaks" entries cross-reference
here. A deferred finding that is not surfaced at edit time comes back as a higher-severity one.

---

## OPEN

### MFO-B1 — `Loadout::Prepare` has no hand parameter
- **RESOLVED by `fix/mfo-b1-prepare-hand` (SHA: see the final commit of that branch; round 2 fixed the Opus review's SEV-2/SEV-3).** `Prepare` gains `a_rightHand`; `CastOn` passes it for a right-only offense plan. BuffRoad and the heal repair stay LEFT (their claims are LEFT).
- **Raised:** Fable review of `bd53a81`+`ed9cbbc`, restated on `625f3b7`. Outside the author's file boundary both times; the author correctly refused to reach for it.
- **Severity:** SEV-4 (design residual, currently masked by hand-selection order)
- **Finding:** `Loadout::Prepare` equips into `LeftHandSlot()` unconditionally (`Loadout.cpp:279, :359`) and at cast level 4 `DeselectSpell`s the RIGHT hand's other spell (`:296-298`). With heal-LEFT and offense-RIGHT as the designed steady state, the two fight at the equipment layer: an offense claim's first-lap `Prepare` puts its spell into the LEFT slot on top of an in-flight heal, and a heal's `Prepare` deselects the offense spell from the RIGHT.
- **Cost recorded at the site as:** "one wasted left-hand equip per offense claim on a weaponless follower" — which Fable judged an understatement once right-first landing became the norm.
- **Fix shape:** give `Prepare` the target hand. Its own brief; it changes an interface used from several call sites.

### MFO-B1a — residuals from the MFO-B1 review (fix/mfo-b1-prepare-hand)
- **SEV-3 -- RESOLVED by fix/mfo-b1a-hand-planner (SHA in Docs/STATUS.md): `WeaponHandActive` now also tests the RIGHT-hand object (`TESObjectWEAP`) directly. Original text:** `APMFBridge::WeaponHandActive` (apmf/Equip.cpp:65) reports false for a sword in the right hand when a heal holds the left (Read() grips Caster), so `PlanCastHand` yields EitherFree/DualCast and `ResolveCastHand` (cast/Hands.cpp ~:1462-1505) can plan RIGHT over a weapon. `Prepare(a_rightHand)` now refuses (Debounced) as a backstop, but the plan still claims the right hand APMF-side. Fix upstream, not in Loadout.
- **FIXED (same branch):** the Scheduler H3 release (`Scheduler.cpp:2085`) now calls `Loadout::ReleaseSpell(id, true)`, so a right-hand offense spell MFO equipped is taken back at H3 too. `ReleaseSpell`'s other callers stay LEFT-only.
- **SEV-4:** right-target "already holding" accepts the spell in either hand (non-heal callers).
- **SEV-4:** `ReleaseHands` side effects: (a) `g_equipClock` is erased on a right-only release and the grace clock is shared between hands; (b) the right record is erased even when the take-back is skipped (orphan); (c) the left take-back does not guard a right OFFENSE copy of the same spell.

### MFO-B2 — the F2 preemption residual is structural
- **Raised:** Fable review of `376d347`+`db56802` (finding 3, SEV-4). Recorded in MAP.md by `1044816`; text verified true by the review of `625f3b7`.
- **Severity:** SEV-4
- **Finding:** `commitPreempt` fires immediately before the claim, but the refusal sites all sit AFTER it — `CastSelfDirect` -> `Held`/`Declined` (`Actuation_Direct.cpp:888/892/938`), `ClaimOffenseCast` refused, `ComposedCast::Try` -> `Held`/`ApmfRefused`, `Loadout::Prepare` failed. A deterministic refusal still displaces the incumbent and still yields lap-rate churn for that shape. Only `Try`'s `Held` is pre-checkable (pure MFO state).
- **Reviewer's reasoning for deferral:** not fixable in general without an APMF dry-run / "can you serve this?" query. Recorded so the next reader does not assume F2 is closed.
- **NARROWED (`feat/mfo-b2-canserve`, 2026-11-09, Harbinger ABI v35 `CanServe`, APMF `178b8ed`):** the dry run now exists and is asked BEFORE `commitPreempt` wherever `CastOn` itself names the claim: the buff claim fork, the owned offense `ClaimOffenseCast` and the composed heal `ComposedCast::Try` (`healClaim` laps). `APMFBridge::CanServeCast` asks about the exact `APMF_CastRequest` (`BuildCastRequestLocked`, the builder `EnsureCastClaimLocked` uses) and about the shape FINALLY filed (the `kCastFlag_OwnLineOfSight` no-sight-line retry: APMF-B269). On Refused the route returns transparently, nothing is displaced, `[apmf] ... Harbinger WOULD REFUSE ...` is logged (5 s per follower/spell/target). Below ABI 35, or Unknown / Accepted, nothing changed. For MFO's cast shapes Harbinger's call-time refusals are `NoDrain`, `NoChannel` and OwnLineOfSight-not-armed (the last removed by the retry), so in practice the gate fires when the arbiter seat is refused or the cast channel is off.
- **STILL OPEN (the residual, SEV-4):**
  - **Routes whose road is chosen inside a helper, after the preempt:** the self fork (`CastSelfDirect`: heal claim, self-buff claim, concentration claim, or the direct stream), the concentration fork (`ConcentrationCast` -> `CastSelfDirect` / `CastTargetDirect`) and the restoration-direct fork (`CastTargetDirect`). A `CastOn`-side gate would have to copy `ChooseHealRoad` / `ChooseBuffRoad` and the concentration-claim predicate, or it would block a direct-road cast that never needs Harbinger. Closing these needs the helper to receive the preempt (deferred to its own claim point) or a pure road query: its own brief.
  - **`Loadout::Prepare` failing** (an MFO equip step), **`Declined`** (cast paths not verified, magicka, reserve, the OOC heal wait) inside the helpers.
  - **`Held`: deliberately NOT pre-checked.** Not pure (`RefreshHealCastClaim` heartbeats the claim; the in-flight hold writes `inFlightHoldSince`), and `PreemptHand` -> `ComposedCast::EndHand` clears the incumbent's watch slot, so a Held asked before the preempt would see the very incumbent the preempt ends and would HOLD where today's code preempts and claims (inverting rule rank). A Held that survives a preempt needs an incumbent heal watch that is not the displaced lock's spell: rare and bounded.
  - **Drain-time drops** (`ServePending`: actor not loaded, the dual vs single hand collision lost at Drain, `kCastFlag_FromPackage`, the channel's Engage checks): accepted, then dropped, after the displacement.
  - **A change of state between the dry run and the claim** (sight-line disarming, the arbiter seat, a claim coming or going): the race the real call already has.

### MFO-B3 — right-first hand selection fails toward RIGHT when `bWeaponStyleControl` is OFF
- **DRAINED** (`fix/mfo-bucket3`, 2026-10-11): the Auto (0) hybrid is covered by `Followers::HasEnabledEquipWeaponGambit` (enabled act.equip_melee/ranged combat rule). Earlier NARROWED note follows. (`fix/mfo-logic-bundle2`): with the switch off `WeaponHandExposure` falls back to `Followers::GetBaseClass` Melee/Ranged (switch ON unchanged). STILL OPEN: an Auto (0) hybrid with the switch off; reading its equip gambits needs a new read-only `Followers` accessor (no new plumbing taken in a small-fix round).
- **Raised:** Fable review of `1044816` (finding C, SEV-4). Recorded at the site and in MAP.md by `625f3b7`.
- **Severity:** SEV-4 (non-default config only; default is ON, `Config.h:505`)
- **Finding:** `WeaponHandExposure` depends on a `g_forcedWeapon` entry, and `EquipWeapon` writes one only under `Config::g_weaponStyleControl` (the kill-switch-off branch is a plain `EquipObject`, no ledger); `ReconcileForcedWeapon` releases unconditionally when off. So with that feature off, a melee/hybrid follower in the transient-unarmed gap still lands RIGHT — the 2026-09-05 "cast never left rest" shape.
- **Why not closed:** closing it needs a signal that does not depend on that feature being on.

### MFO-B5 — APMF-side cast retargeting does not exist
- **Raised:** by the author agent, twice, as an out-of-boundary observation.
- **Severity:** SEV-4 (design gap, APMF side)
- **Finding:** `ApplyRepoint` writes only `param` + TTL, so a TARGET change is still Release + RequestCast rather than a re-aim. Every target-flicker defect in this branch traces back to that: MFO must drop and re-take a claim to change who it is aimed at.
- **Fix shape:** an APMF-side re-aim that preserves the claim. Its own brief, in the APMF repo, with its own review.

### MFO-B6 — the reconciled clock comment relabels the WRONG number (we introduced this)
- **Raised:** Fable review of `e1e55fb` (F-1, SEV-4). **This is an error THIS review cycle introduced**, in `e1e55fb`, while fixing a SEV-5 that asked for exactly this reconciliation. It is not a pre-existing inconsistency someone noticed.
- **Severity:** SEV-4. No code reads these numbers, and the direction of error is conservative for the `IsOwnRetarget` argument (it overstates the unprotected window). But it is now written as FACT in a committed comment and later rounds will build on it.
- **The error:** `Actuation_Hands.cpp:270-271` states "2.3-2.5 s is an offense cast's equip + charge (claim to first CHARGE STATE)". The tree's own evidence says that figure is claim-to-**FIRE**:
  - `Docs/DIAG-2026-09-06-deny-heal-failures.md:31` — "offense: claim 19:56:56.873 -> first Firebolt **fire** 19:56:59.155" (2.28 s)
  - same doc RC7 at `:134` — "Measured claim -> first **fire** latency here is 2.3-2.5 s"
  - `APMFBridge.h:659-660` records the same claim->fire pair
- **Consequence:** BOTH endpoints of the "2.3-4.5 s" span are fire latencies, so the six sites calling that span "claim-to-first-charge" remain mislabelled — the commit relabelled the wrong number instead of the range. Sites: `Actuation_Hands.cpp:230, :408, :480-481, :627, :661`; `MAP.md:489, :503`.
- **The only genuine claim-to-first-CHARGE-STATE data in the tree:** ~2.5 s (0906 heal: mint 19:59:11.158 -> castobs Unk1 <=13.643) and ~3.6 s (0908 heal: request 11:35:57.163 -> `CASTER[L] Unk1` 11:36:00.778).
- **Ready-made fix (no re-derivation needed):** relabel both endpoints as claim-to-fire, and quote the two charge-state figures above wherever "claim-to-first-charge" is actually meant.
- **Provenance caveat that does NOT go away now the DIAG is committed:** the 4.5 s heal figure's sole source is `Docs/DIAG-2026-09-08-field.md`, whose own arithmetic disagrees with itself — its timeline (`:257`) gives request 57.163 -> fire 03.258 = **6.1 s**, while it labels 4.5 s "from repoint" (`:52, :257`) and "claim-to-fire" (`:266`). Do not treat a committed citation as a settled measurement.

- **DRAINED by `fix/mfo-cleanup-bundle1` (2026-10-01):  the 2.3-4.5 s span is relabelled claim-to-fire in Hands.cpp, Bridge.cpp and MAP.md, and the two real claim-to-first-charge figures (~2.5 s, ~3.6 s) are quoted once in Hands.cpp.**

### MFO-B7 — `kHealHoldNeverObservedMs` (4000 ms) is sized from a datum the newest session already exceeded
- **Raised:** Fable review of `e1e55fb` (F-2, SEV-4). Pre-existing; NOT introduced by this cycle. `e1e55fb` did, however, bind two further questions to the same number (see MFO-B8).
- **Severity:** SEV-4
- **Finding:** `APMFBridge.h:652-694` sizes the constant from the 0906 heal's 2.95 s claim-to-observed. The 0908 heal is **4.5-6.1 s** by `Docs/DIAG-2026-09-08-field.md`, which itself asks for >=5000 for the mere warning (`:200, :359`). For `RefreshHealCastClaim`'s competing-rule cap that means the lift (cap + a 1.1-1.3 s lap) straddles the measured fire.
- **DELIBERATELY NOT FIXED, and this is the point:** the reviewer's instruction was "report only; do not resize from n=2." Resizing now means guessing from two samples. **The action here is a MEASUREMENT, not an edit.**
- **Tension with rule 9's carve-out, stated openly:** the carve-out says anything the next field cycle exercises is never deferrable. That carve-out exists to stop a deferred FIX from corrupting the next test. Here the next field cycle IS the remedy — the heal path exercises this constant directly, so the deck log answers it. Read it from the next heal cycle rather than editing the number first.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED (sizing holds per the Opus final report; the one long claim was a LoS-held approach the bound excludes) -- the 4000 ms constant is exceeded: heal 0x2F3B8 claim-to-first-fire n=9 was 409,425,482,773,1193,2333,2557,3147,5109 ms, and 'NEVER FIRED' released at 4377ms (14:39:11, census #5.1 BUILT not fired) while the re-claim fired at #5.3 14:39:15.588.

### MFO-B8 — one constant now answers three questions sized by three DIFFERENT measurements
- **Raised:** Fable review of `e1e55fb` (F-3, SEV-5), self-flagged by the author first.
- **Severity:** SEV-5
- **Finding:** `kHealHoldNeverObservedMs` is now consumed by three tests whose correct sizes are unrelated:
  - **(a)** `RefreshHealCastClaim`'s cap on `created` — must exceed claim-to-**observed** (heal). See MFO-B7.
  - **(b)** the hold age cap on `lastSeen` — need only exceed claim-to-first-**charge**, since `inFlight` takes over from the first charge state. Measured 2.5-3.6 s, so **4000 is adequate here.**
  - **(c)** the `ObservedFiring` recency window — must exceed the **inter-fire idle gap** of a repeating claimed cast (caster `kNone` between fire N and charge N+1). **No log has been read for this quantity.**
- **Why the reuse was reasonable:** all three are "how long may a claim go without evidence of life", and inventing three literals would be worse. The defect is that a future change made for one intent silently moves the other two.
- **Fix shape (the author's own, and it is right):** three NAMED constants derived from one, not three literals — so each can later be sized from its own evidence instead of inheriting one number's assumptions.
- **What would size (c):** the maximum caster-`kNone` gap between consecutive `MLh/MRh_SpellFire` events of one claimed spell while its rule keeps winning, from the `CASTER[L/R]` state log. Mis-sizing (c) is cheap both ways — too short gives an earlier re-aim, which is the direction marth's ruling wants; too long gives a longer hold.
- **DRAINED by `fix/mfo-backlog-batch3` (2026-10-01).** `apmf/APMFBridge.h` now derives `kHealClaimNeverObservedCapMs` (a: CastClaims `RefreshHealCastClaim`, CastOn charged-never-fired check), `kHoldLastSeenCapMs` (b: Hands.cpp hold age caps on `lastSeen`), `kObservedFiringRecencyMs` (c: Hands.cpp `ObservedFiring` windows) (Bridge.cpp's expiry-sweep build-window hold is also (a): age since `created` with no ObservedFiring since the claim, i.e. claim-to-observed; a 4th alias was tried and dropped on review) from `kHealHoldNeverObservedMs`; all 4000, no behaviour change. ComposedCast.cpp static_asserts apply to (a) and (b). Sizing of each from its own evidence remains B7's job.

### MFO-B9 — two comments in one file now state two different bounds for the same mechanism
- **Raised:** Fable review of `e1e55fb` (F-4, SEV-5)
- **Severity:** SEV-5
- **Finding:** `Actuation_Hands.cpp:439-442` (landed by `6bfbd79`, and cited as the drain for MFO-B4) says the hold's heartbeat "stops at `kHealHoldNeverObservedMs` for a claim that never fires ... bounded by that window". `Actuation_Hands.cpp:710-716` (landed by `e1e55fb`) says the real bound is the SUM, ~4.8-6.5 s, and that it also covers a claim that fired and went quiet.
- **Fix:** one-line reword at `:439-442`. A file that states two bounds for one mechanism will get the wrong one believed — the same shape as the `g_forcedWeapon` "no lock" / "guard every access" pair this cycle already had to reconcile.

- **DRAINED by `fix/mfo-cleanup-bundle1` (2026-10-01): the `Hands.cpp` heal-lift comment now states the SUM bound, matching the later comment.**

### MFO-B10 — the hand-index enum in `Actuation_internal.h` is unnamed
- **Raised:** Fable review of `89085bd` (the `refactor/actuation-split` mechanical split), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** `Actuation_internal.h:168` is the only *unnamed* enum in any `*_internal.h` (Board/Logistics headers all use named `enum class`). Enumerators of an unnamed namespace-scope enum have no linkage, so strictly the header's use of `kHandCount` inside the external-linkage `FollowerCastLocks` is the same pedantic ODR corner as pre-C++17 `static const` in a header. No practical effect: the enumerators are only ever used as `std::size_t` constants, never as a type (`decltype(kHand…)` occurs nowhere). Naming it is a rename and outside this brief; record and leave.
- **Reviewer's reasoning:** the enum was file-local in `Actuation.cpp` before the split, where linkage of its enumerators could not matter; promoting it to a shared header is what surfaces the corner. It is a CORNER, not a bug — no practical effect, and the reviewer said so in the same breath. The whole cycle graded nothing above SEV-5, so rule 9 ends it here.
- **Why it was NOT fixed in the split:** naming the enum is a RENAME, which the split's brief forbids by name ("no refactors, no renames"), and a rename touching a shared header consumed by three TUs is its own change with its own review. Deferred, not dropped.
- **Fix shape when drained:** give it a name in the house style the other internal headers use (`enum class` where the call sites can take the scoping, or a plain named `enum` if the bare `kHandLeft`/`kHandRight`/`kHandCount` spellings must survive at ~60 call sites across `Actuation.cpp` and `Actuation_Hands.cpp`). Purely mechanical, but it touches every one of those call sites, so it wants a diff review of its own.

- **DRAINED by `fix/mfo-cleanup-bundle1` (2026-10-01): the enum (now in `cast/Actuation.h`) is `enum HandIndex : std::size_t`, enumerators unchanged, so no call site changed.**

### MFO-B11 — v6→v7 PRGN migration under cap saturation under-records the auto ledger
- **Raised:** Fable tier-3 review of `4a62688` (`feat/mfo-progression-strict-points-perk-style`), SEV-5. F5.
- **Severity:** SEV-5
- **Finding (verbatim):** v6 migration under cap saturation under-records the auto ledger (`ProgAllocator.cpp` `CoSaveLoad`, the `a_version < 7` branch): `points` is cap-clamped, so auto points already wasted into `skillCap` are dropped from `autoPoints`, while a v7 grant retains them. Visible value unchanged; matters only if `skillCap` is later raised.
- **Reviewer's reasoning:** `SkillAlloc::points` is the APPLIED delta (`desired − natural`, clamped at `skillCap` by `ReconcileSkill`), the only v6 datum from which the auto share can be recovered; the granted-but-wasted remainder was never persisted anywhere in v6, so it cannot be reconstructed. A v7 ledger keeps the full float share, so a follower migrated at cap holds fewer ledger points than an identical follower leveled under v7 — invisible unless `skillCap` (`MFOP_SkillCap`, addon INI) is raised, when the v7 follower would climb further.
- **Why it was NOT fixed:** unrecoverable by construction (the datum does not exist in a v6 save); documented at the migration site and in `Serialization.h`'s v7 doc. Deferred, not dropped.
- **Fix shape when drained:** none possible for existing saves; if `skillCap` is ever raised as a feature, note in that brief that cap-migrated v6 followers will not receive the wasted points back and decide then whether a one-time `Respec`-style re-grant is offered.
### MFO-B12 — F2 fix republishes the `mfoEnabled` mirror only at `Refresh`, not at the write site
- **Raised:** Fable round-2 review of `028eb5a` (`feat/mfo-progression-strict-points-perk-style`), SEV-5. R2-1.
- **Severity:** SEV-5
- **Finding (verbatim):** F2 fix republishes the `mfoEnabled` mirror only at `Refresh`, not at the write site — PLAUSIBLE, cosmetic lag only. Where: native/Board.cpp:1203 (`it->second.mfoEnabled = (c.param > 0.5f); continue;` — no `PublishActiveMirror()` after it); native/Followers.cpp:98-111 (`PublishActiveMirror`, the only writer of `g_mfoOff`), called from `Refresh` (:427) and `ClearTransientState` (:159). What round 1 demanded: "republish mfoEnabled under g_mx at the write sites + Refresh." The author republished at Refresh only and documented it as "one diag turn" (Followers.h:174-177, MAP). Scenario: player unchecks MFO on the Board; `ApplyEdits` flips the bool on the worker; the diag-turn `Refresh` runs every kPumpMs × kDiagEveryNth = 532 ms (Diagnostics.cpp:45-46, :341); `PollWork` runs every ~2 s (kPollFrames=120). A poll landing inside that ≤532 ms window still sees managed=true and runs one drift-watch RecomputeSkills/HmsTrackBattle (steady-state = pure reads, writes only on divergence) or, if `applied` was false, one ReapplyFollower. The next poll then takes the unmanaged branch. Worst case is one extra tick of "touch" on a follower the player just froze — not a race, not persistent.
- **Reviewer's reasoning (verbatim):** Why SEV-5: the SEV-2 threading defect is genuinely closed (`IsMfoEnabled` Followers.cpp:450-453 takes g_mx, same shape as IsTrackedFast :220-223; g_followers is never read off-worker any more). The load-time window does NOT exist: plugin.cpp:374 runs Followers::Refresh() at kPostLoadGame BEFORE ProgAllocator::OnPostLoad() (:380), so the mirror already holds the FLWR-loaded mfoEnabled bits when the first poll fires.
- **Why it was NOT fixed:** `Board.cpp` is outside the 2026-09-13 brief's file boundary (no `Board*`), which is why the write-site republish was not done. Deferred, not dropped.
- **Fix shape when drained (verbatim):** one `PublishActiveMirror()` call after Board.cpp:1203 (worker-domain, g_mx is a leaf — safe).
- **Drain attempt 2026-09-14 (`feat/mfo-board-free-respec-label`), NOT fixed — boundary stop:** the write site was re-verified as worker-domain with no lock held (`ApplyEdits` `Board.cpp:1081` ← `PublishSnapshot` `:1317` ← the Diagnostics `AddTask` body `Diagnostics.cpp:330-355`, same domain as `Followers::Refresh`; `g_editMx` is released at the `:1083` swap scope). But `PublishActiveMirror` has INTERNAL linkage — it lives in Followers.cpp's anonymous namespace (`:43` open, function `:98`) with no `Followers.h` declaration — so Board.cpp cannot call it as the fix shape assumes. The drain needs a public export in `Followers.h` + `Followers.cpp` (both outside that brief's boundary) before the one-line Board.cpp call. Still open.
- **DRAINED by `fix/mfo-backlog-batch3` (2026-10-01).** The 2026-09-14 stop was file-boundary only (re-verified: `ApplyEdits` drains on the worker, `g_editMx` released after the swap, g_mx is a leaf). New public `Followers::RepublishActiveMirror()` (Followers.h/.cpp, beside `IsMfoEnabled`) wraps the anonymous-namespace `PublishActiveMirror`; Board.cpp's `SetMfoEnabled` write site calls it.

### MFO-B13 — F3's hold-time floor can shave a fraction off a v6-migrated, cap-saturated skill on the first v7 hold
- **Raised:** Fable round-2 review of `028eb5a` (`feat/mfo-progression-strict-points-perk-style`), SEV-5. R2-2. Adjacent to MFO-B11 (same root).
- **Severity:** SEV-5
- **Finding (verbatim):** F3's hold-time floor can shave a fraction off a v6-migrated, cap-saturated skill on the first v7 hold — PLAUSIBLE, likely invisible. Where: native/ProgAllocator.cpp:2293 (`autoPts = max(0, points − manual)`) feeding :418 (`floor(e.autoPoints + 1e-3) + manualPoints`). Scenario: v6 wrote whole-point shares (main RecomputeSkills: floor(total*w + 0.5)), so migrated autoPts is integral unless the skill was cap-clamped: then points = skillCap − natural, and if natural is fractional (engine autocalc base, ADOPT mode), autoPts inherits the fraction (e.g. natural 23.7 → auto 76.3). The v7 hold floors it → desired 99.7 instead of 100.0. Skyrim's skill UI floors, and perk conditions compare >= integers, so 99.7 reads as "99" where 100 read as "100" — a one-integer visible drop only in that exact corner (cap-saturated AND fractional natural). Adjacent to the already-recorded MFO-B11 (same root: the v6 datum is the clamped applied delta).
- **Reviewer's reasoning:** the migration carries the v6 applied delta verbatim (the only datum), and the F3 hold-time floor (a whole-point actor write) is applied to it like any v7 ledger value; the two are individually correct and only meet in the cap-saturated + fractional-natural corner, where the effect is at most one displayed integer. Nothing above SEV-3 remained in the round, so rule 9 ends the cycle here.
- **Why it was NOT fixed:** raised in the round that ended the cycle; a non-layout tweak of the migration branch with its own review cost. Deferred, not dropped.
- **Fix shape when drained (verbatim):** round the migrated autoPts up to the next whole point when points is cap-clamped (autoPts = ceil(autoPts − 1e-3)), or fold into the MFO-B11 entry as a note. Not a co-save layout change.
- **DRAINED by `fix/mfo-cosave-readers` (2026-10-01), pending its tier-A review.** The clamp itself is NOT knowable at migration time: the save-time `skillCap` was never persisted in PRGN, and the live `g_econ.skillCap` is this session's (it may have changed, it is the 100 default with the addon absent, and it is main-thread state the load callback does not read today). Its only consequence for this finding IS knowable from the record: v6 shares were whole points and manual points are whole (+1 per placement, `Verbs.cpp:434`), so `points − manual` is fractional only on a cap-clamped skill with a fractional natural. `progression/Allocator.cpp:653` now applies `autoPts = max(0, ceil(max(0, points − manual) − 1e-3))` in the `a_version < 7` branch: the identity on a whole share (float noise within 1e-3 snaps to the integer), a round-up exactly on the corner. Bytes read unchanged (a value transform after the reads), no version bump. MFO-B11 stays OPEN (the wasted points are still unrecoverable).

### MFO-B14 — the empty-hand condition signal can mark a "free off-hand" one-hand perk as unarmed
- **DRAINED** (`fix/mfo-logic-bundle2`): `ReadStyleFacts` unarmed now requires the RIGHT hand pinned empty; a left-empty-only test no longer votes.
- **Raised:** Fable tier-3 review of `49a9cc2` (`feat/mfo-shed-fists-rule`), SEV-4. F2.
- **Severity:** SEV-4
- **Finding (verbatim):** F2 — SEV-4, PLAUSIBLE: the empty-hand condition signal can mark a 'free off-hand' one-hand perk as unarmed. native/Progression.cpp:326-336, 352-354. The per-list rule fires on a list where any GetEquippedItemType(hand) test admits code 0 only, with no hand test excluding 0 and no weapon-kind keyword. A perk of the shape 'left hand empty' alone (an overhaul's 'no offhand' one-handed perk with no WeapType* keyword and no right-hand GetEquippedItemType test) satisfies it. If MFO allocates such a rank to an enrolled follower, votes.unarmed > 0, fists become valid, and the shed will strip him of an off-role weapon he was legitimately swinging one-handed. Shape-level risk, not demonstrated. Cheap tightening: require the OTHER hand not to be a one-hand-weapon signature on the same tree/node, or require both hands empty unless the primaryAV signal also fires.
- **Reviewer's reasoning:** shape-level, not demonstrated against any installed perk record; the vote only matters when MFO itself allocates such a rank AND the follower carries an off-role weapon with no in-role one, and the consequence is the pre-fix behaviour (one shed) rather than a crash.
- **Why it was NOT fixed:** raised in the round that ended the cycle (nothing above SEV-3 remained once F1 closed by the rebase onto `6623bd8`); a classifier tightening with its own review cost. Deferred, not dropped.
- **Fix shape when drained (verbatim):** require the OTHER hand not to be a one-hand-weapon signature on the same tree/node, or require both hands empty unless the primaryAV signal also fires.
- **Surfaced at edit time from:** MAP.md §4 `ShedOffRoleWeapon` FISTS RULE "What breaks" + the Progression STYLE FACTS "UNARMED" note.

### MFO-B15 — the board footer can say "free (one time)" for ~300 ms after the free respec was spent
- **Raised:** Fable tier-2 review of `5f9d6a9` (`feat/mfo-board-free-respec-label`), SEV-5, PLAUSIBLE.
- **Finding (verbatim):** `native/Board_Progression.cpp:1199` and `:1217`. Chain: Confirm on the render thread → `QueueEdit` → worker `ApplyEdits` on the next pump tick (≤133 ms while the board is open, `Diagnostics.cpp:355`) → `MainThread::Post` → next main-thread frame runs `ProgAllocator::Respec` (clears `st.freeRespec`, `ProgAllocator.cpp:1737`) and immediately `PublishBoardViews()` (`Board.cpp:1153`, swaps `g_boardSnap` under `g_viewMx`) → the Board only picks that up on the NEXT worker `PublishSnapshot` (`Board.cpp:1354`, ≤133 ms) → next render frame copies `g_snapshot` (`Board.cpp:626`). Worst case ~133+16+133+16 ≈ 300 ms during which `who->freeRespec` is still true. Worst-case outcome: an AUTO-class follower (where `RecomputeSkills` re-granted `autoPoints`, so the second respec is not the `:1690` "nothing allocated" no-op) is billed 500 rapport under a popup that said "no rapport is lost". Requires Respec-button + Confirm again inside the window, and the popup lands focus on Cancel (`:1237`).
- **Reviewer's reasoning (verbatim):** Billing authority is correct: `Respec` decides from `g_prog` on the main thread, the label is only a mirror. This is acceptable as the brief anticipated. Recommend a one-line REVIEW-BACKLOG entry rather than a fix (a render-side "spent" latch would be polish, not a root-cause fix).
- **Why it was NOT fixed:** SEV-5, no carve-out; a render-side latch masks nothing but is polish.
- **Fix shape when drained:** none required; if drained, a render-side "respec queued" latch that hides the free label until the next snapshot.

- CLOSED 2026-10-12: render-side latch in `Board_Progression.cpp` (`s_respecQueuedId`/`s_respecQueuedAt`): Confirm on a free respec hides the free label and popup body for up to 1 s or until the snapshot clears `freeRespec`, whichever is first.

### MFO-B16 — after the MFO armor swap and before the vendor sale, the engine's rating-only auto-equip and `EquipBestOwnedGear` disagree; a worn off-class piece can flip back and forth until sold
- **Raised:** Fable tier-2 review of `bb77b69` (`fix/mfo-armor-class-score`), SEV-4, PLAUSIBLE.
- **Finding (verbatim):** `native/Logistics_Economy.cpp:352-376` — `EquipBestOwnedGear` rated branch, every `ServiceFollower` tick (no combat gate on the rated branch). Before this commit MFO's owned-upgrade judge and the engine's auto-equip both ranked by raw rating, so they never fought. Now: a light follower wearing steel (31) who owns leather (26 → 52) gets leather put on by MFO. The engine's own follower auto-equip re-evaluates "best armor" by raw rating on inventory adds (every loot pickup) and on default-outfit re-apply (cell load), and will put the steel back; MFO re-swaps within ≤ N×133 ms. The un-wear the author relies on (redundant-inferior force-sell) only fires inside `EconomyProbe` — a vendor visit. Until then the body slot is contested, event-paced (not per-frame), each flip emitting `[equip] OWNED` + `[armor]` + two `[armor-obs]` lines. Also `FitsCarryWeight` (pre-existing) gates the swap on an owned pick — an over-encumbered follower never gets the light piece worn by MFO and the sale strips him to the engine's choice.
- **Wave-2 location (2026-09-25, `native/logistics/`):** `EquipBestOwnedGear` is `native/logistics/EquipAuthority.cpp:132` (its rated branch follows).
- **Reviewer's reasoning (verbatim):** Not a crash, not co-save, converges at the first vendor. The brief said "No engine deny", so this window is a design consequence the brief accepted, not a defect the author could close. It IS what the next field cycle exercises — the `[armor-obs]` probe is precisely the instrument that will measure the flip rate. Read the first deck log for `[armor-obs] ... EQUIP '<heavy>'` lines that are NOT preceded by an MFO `[equip]` line (engine re-wears).
- **Why it was NOT fixed:** the fix is an engine equip-best deny/steer (new mechanism, principles 1/2) to be sized FROM the probe, not before it (Fable diagnosis 2026-09-14). marth informed.
- **Fix shape when drained:** measure the flip rate from `[armor-obs]`; if the engine re-wears, either (a) drop/sell the off-class piece without a vendor once a better-scored owned piece is worn, or (b) portal+deny on the engine's equip-best for followers MFO dresses.
- **Surfaced at edit time from:** MAP.md `EquipBestOwnedGear` / ARMOR CLASS BY SKILL "What breaks".

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CONFIRMED -- equip-auth mode=observe-only on the deck (MFO.1 12:40:26.828); SPID outfit re-wear at 12:51:31 and 12:54:02 left Adelinda's Shrouded Cowl (13) over the helmet (18) until 13:52:34 and mage Jesper's off-class Ebony Helmet EQUIP at 12:54:02.454 never unequipped; MFO logged 'left to the APMF equip declaration' 232x/38x. Decision task filed (needs-decision).

- REOPENED 2026-10-06 (Opus spot-check): back to LIVE as "stopgap (a) to build". Its field verdict is the SPID outfit re-wear, the outfit re-apply case the rework keeps on deny. marth: "Task is obsoleted by later task, A is an acceptable stopgap"; stopgap (a), drop or sell the off-class piece without a vendor once a better-scored owned piece is worn, is unbuilt and has no other tracker.
- M1a 2026-10-12: stopgap (a) NOT built, STOPPED per brief. There is no vendor-free SELL path (selling exists only as the `EconomyProbe` vendor trade, `logistics/Economy.cpp:~459`); the only vendor-free disposals in the code are a world DROP (`ShedOffRoleWeapon`'s `doDrop`, `logistics/Upkeep.cpp:~487`) and hand-to-player (`Upkeep.cpp:~287`), both barred by marth's 2026-10-12 "never dropped" ruling. Needs a decision (see the M1a report).

### MFO-B17 — armor SEV-5 notes from the same review (`bb77b69`)
- (a) `ArmorClassSuits` (`Logistics_Loot.cpp:303-310`) is now dead code with changed semantics (BASE AV); zero callers. Delete or keep as the named predicate; never re-call on a hot path.
- (b) The "allocator and wardrobe agree" comment (`Logistics_Loot.cpp:201-210`, `Logistics_internal.h:373-376`) overstates: on an exact skill tie the wardrobe takes the perk vote, `DominantArmorSkill` goes straight to light. Doc fix.
- **Wave-2 location (2026-09-25, `native/logistics/`):** (a) `ArmorClassSuits` is `native/logistics/Gear.cpp:136`; (b) the comment pair is in `native/logistics/Gear.cpp` (`ArmorPrefFor:43`) and `native/logistics/Logistics_internal.h:377`.
- (c) `TradeBridge::BuyThresholds` (`TradeBridge.h:91-107`) is NOT byte-shared with the .pex (in-DLL only; the .pex sees the 10 natives). Append-only was honoured anyway; stop calling it byte-shared.
- (d) Shields are half in the wear decision: `EquipBestOwnedGear:372-373` skips shields while keep buckets and force-sell score them; a worn heavy shield with an owned light one is force-sold at a vendor but MFO never puts the light one on. Pre-existing shape.
- CLOSED 2026-10-12 (`fix/mfo-m1a-equip-gem-backlog`): (a) `ArmorClassSuits` deleted (grep: zero callers; only comments named it), declaration removed, MAP updated; (b) the "allocator and wardrobe agree" comment in `Logistics_internal.h` now states the exact-tie difference; (c) no source text calls `BuyThresholds` byte-shared (only this entry did), nothing to change; (d) skipped by decision (shields stay half in the wear decision). `SkillScale.cpp:120,145` comments still name `ArmorClassSuits` (outside that batch's file list).

### MFO-B19 — `Actuation.cpp` includes `Logistics_internal.h` for two symbols
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-5. F7.
- **Severity:** SEV-5
- **Finding (verbatim):** F7 — SEV-5 backlog: #include "Logistics_internal.h" in Actuation.cpp:13 pulls a ~900-line internal header; no ODR hazard (inline globals, WeaponScore pure inline, ComputeWeaponRoles defined once); proper seam is a public declaration in Logistics.h.
- **Reviewer's reasoning:** correctness is unaffected (every definition in that header is `inline` or defined once); the cost is a layering smell and compile-time coupling only.
- **Why it was NOT fixed:** raised in a round with nothing above SEV-3 once F1-F5 were fixed; a public seam in `Logistics.h` was outside the brief's file boundary. Deferred, not dropped.
- **Fix shape when drained (verbatim):** proper seam is a public declaration in Logistics.h (declare `WeaponRoles`/`ComputeWeaponRoles`/`WeaponScore` there, or a thin `Logistics::CombatWeaponScore(actor, weapon)` wrapper, and drop the internal include from `Actuation.cpp`).
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.
- **Drain attempt 2026-10-01 (`fix/mfo-backlog-batch3`), NOT done -- STOP per brief.** `Equip.cpp` also consumes the inline `Logistics::WeaponScore` (`Logistics_internal.h`, which pulls `WeaponKindOf`, `kStyleBias`, `Progression::WeaponKind`), and `WeaponRoles` needs the `WepClass` enum. A seam is the struct + enum + three inline definitions (>40 lines, moves inline defs), or a non-inline `WeaponScore` wrapper; its own brief.

- **PARTLY DRAINED by `fix/mfo-cleanup-bundle1` (2026-10-01): the include is dropped from Fire/Roads/CastOn. `Equip.cpp` really uses `ComputeWeaponRoles`/`WeaponRoles` (and four more internal symbols), so the public seam in Logistics.h is still open.**
- **Cheapest seam (Opus review of `fix/mfo-backlog-batch3`, 2026-10-01):** a public POD `{uint16 preferKinds; uint8 offHand}` in `Logistics.h`; a non-inline `CombatWeaponRolesFor(actor, const FollowerState&)` in `Gear.cpp`; reuse the existing non-inline `Logistics::WeaponBuyScore` (`Logistics.h:80`); and move the four equip-authority declarations (`EquipAuthorityLive`, `RefreshEquipDeclaration`, `LiveBoundWeapons`, `ResendEquipDeclaration`) into `Logistics.h`. About 15-20 header lines; its own brief.
- **Out of scope here (MFO-B206, SEV-5):** `CoLoadForcedWeapons`' `weapon == 0` branch (Equip.cpp ~:1241-1242) still passes `nullptr` for the right hand.
- CLOSED 2026-10-12 (`fix/mfo-m1a-equip-gem-backlog`): public seam in `logistics/Logistics.h` (`CombatWeaponStyle`, `CombatAmmo`, `CombatStyleFor`, `CombatAmmoOwned`, `CombatWeaponScore`, `CombatRangedScore`, plus the declarations of `EquipAuthorityLive` / `RefreshEquipDeclaration` / `LiveBoundWeapons` / `ResendEquipDeclaration`); wrappers defined in `logistics/EquipAuthority.cpp`; `cast/Equip.cpp` no longer includes `Logistics_internal.h`. The inline definitions did not move.

### MFO-B20 — `kMaxForcedWeapons` (64) is now a PAIR cap and the FWPN reader aborts the whole load above it
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-5. F8.
- **Severity:** SEV-5
- **Finding (verbatim):** F8 — SEV-5 backlog: kMaxForcedWeapons = 64 (Actuation.cpp:2407) is now a PAIR cap and the reader aborts the whole FWPN load above it (:2447-2450). Unreachable at party scale.
- **Reviewer's reasoning:** a dual hold writes two pairs per follower, so the cap is effectively 32 dual-wielding followers; the party is bounded far below that, and the abort path only drops the release sweep (a session starts hold-free anyway once the gambit re-forces).
- **Why it was NOT fixed:** unreachable at party scale; raised in a round with nothing above SEV-3 once F1-F5 were fixed. Deferred, not dropped.
- **Fix shape when drained (verbatim):** raise the cap to `2 * kMaxFollowers`-shaped headroom or make the reader skip (not abort) past it; not a layout change (the count field is unchanged).
- **DRAINED by `fix/mfo-cosave-readers` (2026-10-01), pending its tier-A review.** Chose RAISE + a matching WRITER clamp, not skip: `kMaxForcedWeapons` (`cast/Equip.cpp:1154`) = `2u * 4096u` (2 hands x the FLWR `kMaxFollowers`, a literal because that constant is anon-namespace in `Serialization.cpp`), and `CoSaveForcedWeapons` truncates its snapshot to the same cap with an error line, so every count the writer can emit is read in full and an above-cap count can only be a corrupt record (still aborts). A skip would have read the first N pairs and left every later pair's stale force-lock unreleased. Review round (same branch): the truncation path alone stable-sorts by follower FormID and cuts at a follower boundary (whole followers drop, never half a dual hold); ordinary saves are unsorted and byte-identical. Layout unchanged (`u32 count; count x {u32 follower, u32 weapon}`, v1).
- **Surfaced at edit time from:** MAP.md §1 FWPN entry + §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.

### MFO-B21 — `ComputeWeaponRoles` (inventory walk + style mirror lock) now also runs from every pick lap and every 5 s top-up attempt
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-5. F9.
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** F9 — SEV-5 note: ComputeWeaponRoles (inventory walk + StyleVotesFor + mirror lock) now also runs from every pick lap and 5 s top-up attempt plus PickOffHandWeapon/PickShield walks. Negligible at party scale.
- **Reviewer's reasoning:** the pick lap fires once per real equip (it buys a suppression window) and the top-up is rate-limited to one inventory walk per follower per 5 s, so the added cost is a handful of inventory walks per fight per follower.
- **Why it was NOT fixed:** a perf note with no observed cost; no fix requested. Deferred as a note, not dropped.
- **Fix shape when drained (verbatim):** none requested; if a profile ever shows it, cache the roles per follower per service tick (the Scheduler already computes them once per OOC tick in `ShedOffRoleWeapon`).
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.

### MFO-B22 — `EquipShieldOnMain` logs the shield equip before the posted closure has re-resolved
- **Raised:** Fable round-2 review of `1ac3c6b` (`feat/mfo-dualwield-combat-pick`), SEV-5. F-E.
- **Severity:** SEV-5
- **Finding (verbatim):** F-E — SEV-5 backlog: EquipShieldOnMain logs "GAMBIT equip shield" synchronously (:1822-1823, :1912) before the posted closure re-resolves; a closure that finds a null actor/item (:1711-1714) equips nothing but the log already claimed it did; no IsTracked/alive check inside the closure (one-frame window, harmless).
- **Reviewer's reasoning:** a one-frame window between the worker's decision and the main-thread equip; the closure null-checks both re-resolved forms and equips nothing on a miss, so the only defect is a log line that can overstate. Harmless.
- **Why it was NOT fixed:** raised in a round with nothing above SEV-3 once F-A/F-C/F-D were fixed; a log-placement change with its own review cost. Deferred, not dropped.
- **Fix shape when drained (verbatim):** move the `[equip] ... GAMBIT equip shield` line into the posted closure after the null checks (log what actually happened), optionally with an `IsTracked`/alive check; same shape as the loot precedent.
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.

- **DRAINED by `fix/mfo-cleanup-bundle1` (2026-10-01): the line is logged inside the closure after the null checks. The combined `[equip] ... + shield` line at the weapon-equip site is untouched.**

### MFO-B23 — CoLoad both-hands-same-form: the second unequip is slot-less by object after the left-slot one
- **Raised:** Fable round-2 review of `1ac3c6b` (`feat/mfo-dualwield-combat-pick`), SEV-5. F-F.
- **Severity:** SEV-5
- **Finding (verbatim):** F-F — SEV-5 backlog: CoLoad both-hands-same-form (:2552-2553): the second unequip is slot-less by object after the left-slot unequip is queued ahead of it; passing the right-hand slot would make it unambiguous. (Note: there is no RightHandSlot() helper; if you add one it is the LookupByID<BGSEquipSlot>(0x00013F42) twin of LeftHandSlot — only do it if trivial and in-boundary, otherwise backlog as-is.)
- **Reviewer's reasoning:** only the same-form count>=2 dual hold reaches this branch on load; the two queued unequips target the same object and which instance the slot-less second one clears is the same unverified engine question STATUS.md already carries for the F4 probe.
- **Why it was NOT fixed:** backlogged as-is per the coordinator (no `RightHandSlot()` helper exists; adding one is its own small brief). Deferred, not dropped.
- **Fix shape when drained (verbatim):** passing the right-hand slot would make it unambiguous — add `Loadout::RightHandSlot()` (the `kRightHandEquip` twin of `LeftHandSlot`, verify the default-object id in the pinned tree) and pass it on the `inRight` unequip.
- **Surfaced at edit time from:** MAP.md §1 FWPN entry + §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.
- **DRAINED by `fix/mfo-backlog-batch3` (2026-10-01).** `Loadout::RightHandSlot()` (`Loadout.cpp`/`.h`, FormID lookup `0x00013F42`, DOBJ `RHEQ` index 20 = `kRightHandEquip` in the fork's `BGSDefaultObjectManager.h`) is passed on `Equip.cpp`'s CoLoad `inRight` unequip.

### MFO-B24 — keep gates the second one-hander on `keepRoles.melee`, loot/buy on `meleeTargetClass`; a base caster keeps two daggers but never fetches a second
- **DRAINED** (`fix/mfo-logic-bundle2`): `keepSecond1H` (`logistics/SwapUp.cpp`) gates on the identical meleeTargetClass expression as buy/loot; MAP wording softened.
- **Raised:** Fable review of `b3ac577` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5 (a).
- **Severity:** SEV-5
- **Finding (verbatim):** SEV-5 keep gates the second 1H on `keepRoles.melee == OneHand` (Logistics_Economy.cpp:687) while loot/buy gate on `meleeTargetClass == OneHand` (Loot_Equipment.cpp:147, Economy.cpp:459), which is `Other` for a base caster → a caster with a dagger and dual-wield votes keeps two daggers but never fetches a second; harmless; MAP's "ONE rule" overstated for casters.
- **Wave-2 location (2026-09-25, `native/logistics/`):** `keepSecond1H` is `native/logistics/Economy.cpp:560` (in `EconomyProbe`).
- **Reviewer's reasoning:** harmless — the only divergence is that a caster keeps a spare dagger instead of selling it; loot/buy never fetch one, so nothing accumulates.
- **Why it was NOT fixed:** raised in a round whose blocking finding (the SEV-2 gem capture) was fixed; a keep-gate change re-opens the keep/sell contract for casters and wants its own field observation. Deferred, not dropped.
- **Fix shape when drained (verbatim):** derive the keep gate from the SAME `meleeTargetClass` computation the buy side uses (`caster && !baseWeaponUser && (baseCaster || !wantsMelee) ? Other : roles.melee`) instead of `keepRoles.melee`, so keep, buy and loot share one predicate; and soften MAP's "ONE rule" to "one rule for weapon-role followers".
- **Surfaced at edit time from:** MAP.md §4 Logistics "CLOSED 2026-09-14 — THE SECOND ONE-HANDER" What-breaks.

### MFO-B25 — style-facts promotion also promotes a player-gated ability conditioned on a weapon/armor keyword (design remark for marth)
- **Raised:** Fable review of `b3ac577` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5 (b).
- **Severity:** SEV-5
- **Finding (verbatim):** SEV-5 design remark on FIX 3: WalkPerkEntries reads ability effect conditions regardless of firesForNpc, so a player-gated ability conditioned on a weapon/armor keyword is promoted and the allocator may spend a follower point on a rank whose only follower effect is its style vote; consistent with the brief's intent, recorded for marth.
- **Reviewer's reasoning:** the promotion is by design (a style fact is engine truth off the record); the only cost is a follower perk point on a rank whose sole follower-side effect is the vote it casts.
- **Why it was NOT fixed:** consistent with the brief's intent; marth's call whether a player-gated ability's style vote is worth a point. Deferred, not dropped.
- **Fix shape when drained (verbatim):** if marth says no, require `f.firesForNpc` (or `kind != kAbility`) on at least one style-carrying entry before promoting — i.e. promote on `rank.style.Any()` only when the fact came off an entry-point tab or an NPC-firing ability.
- **Surfaced at edit time from:** MAP.md §5 Progression "STYLE-FACT RANKS ARE NEVER MARGINAL" What-breaks.
- CLOSED 2026-10-12, marth's ruling "marginal: buyable, steers": `Progression.cpp` `BuildSkill` promotes a style-fact rank to effective only off a firing entry (`styleFires`); a rank whose style fact comes only off non-firing entries (player-gated ability conditioned on a weapon/armor keyword) is promoted dead -> marginal (marginal and effective unchanged), so it stays buyable, its vote steers, and a saved copy is never dropped on load.

### MFO-B26 — the reconcile back-off lifts on ANY inventory change, so under churn "one warn per 60 s" becomes "one warn per change + 3 passes"
- **Raised:** Fable review of `b3ac577` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5 (c).
- **Severity:** SEV-5
- **Finding (verbatim):** SEV-5 back-off lift: the fingerprint includes loose stack count and every worn (base,uid); any legitimate socket elsewhere or worn swap lifts the back-off, restarts the 3-pass count and re-warns — under heavy churn "one warn per 60 s" becomes "one warn per inventory change + 3 passes"; bounded and loud by design.
- **Reviewer's reasoning:** bounded (every re-warn costs three passes of a changed inventory) and loud by design; the failure it reports is real each time.
- **Why it was NOT fixed:** the lift-on-change is what keeps the back-off from masking a request that WOULD now succeed; narrowing the fingerprint trades that for quieter logs and wants a deck measurement first. Deferred, not dropped.
- **Fix shape when drained (verbatim):** narrow the fingerprint to the stalled key's own inputs (that item's worn (base,uid) + that gem base's loose count) so an unrelated socket/swap does not lift it; or keep the lift but suppress the re-warn within the original 60 s window.
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "RETURN LOGGING + STALL DETECTOR" What-breaks.
- **DRAINED by `fix/mfo-gems-backlog` (2026-10-01), pending its review.** Narrowed fingerprint (the first fix shape): each stall key now lifts only when ITS OWN inputs change, `keyPrint` in `native/MEOBridge.cpp` `ReconcileLooseGems` = the item's worn (base,uid) + that gem base's loose count, checked by one lift sweep at the top of the pass. An unrelated socket or swap no longer lifts it, so B31 (our own swap-up lifting G's back-off) is gone with it. The 60 s floor still bounds a stale key, and `inventoryKey` stays the LEFTOVER line's fingerprint.


### MFO-B27 — the stall detector's progress signal is the per-ITEM empty count shared by both ops; another slot landing on the same item resets a genuinely stuck key
- **Raised:** Fable round-2 review of `5f814d5` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** B27 SEV-5 — MEOBridge.cpp:371 progress signal is the per-ITEM empty count shared by both ops; no false STUCK reachable, but another slot's landing on the same item resets a genuinely stuck key's passes (false progress), delaying detection by up to (empty slots × 3) passes; bounded.
- **Reviewer's reasoning:** no false STUCK is reachable (a key only advances when the count is unchanged); the only cost is delayed detection while sibling slots on the same item still land, bounded by the item's empty-slot count.
- **Why it was NOT fixed:** bounded and in the safe direction (later, never spurious); a per-slot progress signal needs `GetGemDetails` re-read per slot per pass and its own review. Deferred, not dropped.
- **Fix shape when drained (verbatim):** key progress on the SLOT, not the item: record `filled.contains(slot)` (from `GetGemDetails`) at issue and treat "this slot is still empty next pass" as no-progress, independent of sibling slots; the unsocket op mirrors it with "this slot is still filled".
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "RETURN LOGGING + STALL DETECTOR" What-breaks.
- **DRAINED by `fix/mfo-gems-backlog` (2026-10-01), pending its review.** `StuckState.emptyAtIssue` is replaced by `pendingAtIssue`, and `stallGate` takes the slot's own state read from `GetGemDetails` this pass (op 0 still empty, op 1 still filled) instead of the item's empty count. A sibling slot landing no longer resets a stuck key. The STUCK line now says `the slot never filled/emptied`.


### MFO-B28 — bought weapons never reach `AcquireEquip`, so a BOUGHT primary upgrade carries no gems (pre-existing)
- **Raised:** Fable round-2 review of `5f814d5` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** B28 SEV-5 — bought weapons never reach AcquireEquip (Logistics_Economy.cpp:330-378 picks ARMO only), so a BOUGHT primary upgrade wielded by Actuation's EquipObject (Actuation.cpp:1924/1935) carries no gems (QueueGemMove has exactly one caller); pre-existing, outside this SHA.
- **Wave-2 location (2026-09-25, `native/logistics/`):** `EquipBestOwnedGear`'s armor-only pick is `native/logistics/EquipAuthority.cpp:132` onward (the `AcquireEquip` call at `:185`).
- **Reviewer's reasoning:** `EquipBestOwnedGear`'s rated branch only ever picks armor, so the one `QueueGemMove` caller (`AcquireEquip`) never sees a bought weapon; the combat equip in Actuation is a plain `EquipObject` with no gem carry. Pre-existing; the SHA did not change it.
- **Why it was NOT fixed:** outside the SHA and the brief (Actuation + the owned-weapon wear decision are separate mechanisms). Deferred, not dropped.
- **Fix shape when drained (verbatim):** give `EquipBestOwnedGear` (or the trade completion path) a WEAPON branch that routes a bought/owned in-role primary upgrade through `AcquireEquip(a_src=nullptr, myWeap, forceStock=false)` so the same-role gem capture + `QueueGemMove` run for it; the combat equip then finds the gems already moved.
- **Surfaced at edit time from:** MAP.md §4 Logistics "CLOSED 2026-09-14 — THE SECOND ONE-HANDER" What-breaks + §7 MEOBridge gem-transfer entry.

---
- CLOSED 2026-10-12 (`fix/mfo-m1a-equip-gem-backlog`, also covers B32 for owned/looted-to-pack weapons): `EquipBestOwnedWeapon` (`logistics/EquipAuthority.cpp`) routes an owned in-class primary upgrade over the held weapon through `AcquireEquip(nullptr, w, forceStock=false)`. Backup / true second / silver stay stock-only (loot side). No weapon enforcement added.

### MFO-B29 — on a FULLY-socketed item the back-off's expiry re-entry point is the swap-up, so the eviction bounce recurs every ~63 s while MEO keeps refusing, and the wiped key means the warn never escalates
- **Raised:** Fable round-3 review of `64512aa` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-4, CONFIRMED by trace.
- **Finding (verbatim):** `native/MEOBridge.cpp:518-521` (swap-up skip reads only `backoffUntil`), `:546-550` (erase_if drops an expired, untouched key). On the reported case (M fully socketed, W in slot s, G better and refused) the slot loop never touches `keyG`, so at expiry: `socketBackedOff` → false → swap-up evicts W → end of pass `keyG` is ERASED (untouched, not backed off). Next passes: (s,G) is a NEW key → issue, issue, STUCK + warn + back-off → excluded → W re-socketed. Per cycle (~60 s + 3-4 passes): 1 unsocket, 2 socket-G, 1 socket-W, one full STUCK warn that always reads "accepted 2 time(s)". Not the round-2 SEV-2 (that was self-lifted every ~5 s; this honours the full 60 s and is loud), and it IS the retry contract MAP.md documents — but the retry on a full item is an eviction of a worn gem, ~5 % duty loss of W and 4 MEO ops/min, forever.
- **Reviewer's reasoning:** carve-out (b) question: if marth's pass criterion is "M stops bouncing" fix before deploy; if "bounce ≤ once per back-off window, warned" is acceptable, defer.
- **Why it was NOT fixed:** coordinator call 2026-09-14: loud, bounded to once per 60 s window, does not corrupt the next test (the STUCK line still names the request); deploy proceeds; marth informed.
- **Fix shape when drained (verbatim):** (i) at expiry retry ONLY via the slot path — the swap-up keeps skipping a gem whose key EXISTS for this item (expired or not); the key is then erased only by the fingerprint-change lift; or (ii) keep `reported`/pass history across expiry and grow the back-off (exponential) so the warn escalates instead of repeating.
- **DRAINED by `fix/mfo-gems-backlog` (2026-10-01), pending its review.** Fix shape (i). An op-0 key with a declared back-off now survives its expiry untouched (end-of-pass forget keeps it) and is erased only by the lift sweep (B26 fingerprint change). The swap-up reads the new `socketKeyHeld` (key exists, expired or not) so it never evicts for a gem MEO refused; the slot path stays the only retry (`stallGate` resets an expired key). Interaction with B26: the narrowed fingerprint is what makes the held key safe to keep, because our own eviction/re-socket of another gem no longer changes it. A fully-socketed item now never retries the refused gem until a slot opens or its inputs change.


### MFO-B30/B31/B32 — SEV-5 notes from the same review (`64512aa`)
- **B30:** stuck key is per-SLOT, exclusion per-ITEM, and a uid-0 key does not cover the minted uid (`:473`, `:436`, `:398-404`): each newly-opened slot (and the first post-mint pass) re-runs the 2-issue + warn detour for G; bounded by capacity (≤ 8 per item), never spurious. Note against B27.
- **B31 (B26 addendum):** our OWN legitimate swap-up on the item is a fingerprint change that lifts G's back-off in the SLOT path (`:364-369`, `:512-537`): a three-pass detour (2 wasted G issues + a repeat STUCK warn) per legitimate swap-up on that item; no loop.
- **B32 (B28 broadened):** route-2b LOOSE-looted weapons never reach `AcquireEquip` either (`AcquireEquip` callers: `Logistics_Loot_Equipment.cpp:473`, `Logistics_Economy.cpp:417` armor-only; `Logistics_Loot.cpp:1835` only sets `lootable`), so a loose primary one-hander is worn by Actuation's plain `EquipObject` with no gem carry. B28's fix shape becomes "bought OR loose-looted OR owned weapon".
- **Wave-2 location (2026-09-25, `native/logistics/`):** `AcquireEquip` (`native/logistics/Gear.cpp:640`) callers: `native/logistics/LootEquipment.cpp:473`, `native/logistics/EquipAuthority.cpp:185`.

### MFO-B33 — LEFTOVER `off-domain` / `duplicate-copy` WARN every 60 s in by-design steady states
- **DECIDED + FIXED 2026-10-11 (marth: drop to INFO):** the `off-domain` LEFTOVER line is info (MEOBridge.cpp), as the mint deferral already was; the other classes stay WARN. ClickUp 86e3m3v36.
- **Raised:** Fable tier-3 review of `1efa3e3` (`fix/mfo-meo-no-loose-gems`), SEV-5 #3, record only.
- **Finding (verbatim):** kOffDomain and kDuplicateCopy WARN every 60 s in by-design steady states (one loose armor gem, all armor full, a weapon socket open); suggest INFO; marth's call.
- **Reviewer's reasoning:** the invariant is "no loose gem while a socket it FITS is open"; an off-domain gem with only an off-domain socket open is not a broken invariant, and a junk copy that never sells (economy off) is a steady state, so both warn on a condition the user cannot act on from MFO's side.
- **Why it was NOT fixed:** coordinator call 2026-09-15: log LEVEL is marth's call; only `minting` is INFO today (`LeftoverWhy` → the `spdlog::info`/`warn` split at the LEFTOVER issue site, `native/MEOBridge.cpp`).
- **Fix shape when drained (verbatim):** suggest INFO for kOffDomain and kDuplicateCopy; keep stuck / capacity / support-limit / refused / unclassified at WARN.
- **Addendum (Fable round 2 on `9dacc0e`, coordinator 2026-09-15):** `support-limit` belongs on the by-design-steady-state list too (a loose second support gem with every dual-socket item already holding one warns every 60 s on a condition MFO cannot change); same call, same fix shape.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CONFIRMED -- 51 off-domain LEFTOVER warnings across two sessions; level decision filed as a needs-decision task.

### MFO-B35 — the duplicate-copy deferral is redundant per MEO's in-place mint and starves the worn item while a spare copy is carried
- **Raised:** Fable round 2 on `9dacc0e` (`fix/mfo-meo-no-loose-gems`), SEV-4, PLAUSIBLE, pre-existing guard (the deferral predates the branch; the branch narrowed it to UNWORN copies).
- **Finding (verbatim):** the duplicate-copy deferral is redundant per MEO plugin.cpp:9139-9153 and costs the worn item its gems for as long as a spare copy is carried. :586 defers every uid-0 request while invBaseCount > wornCopies, on the theory (:579-581) that a uid-0 SocketGem could mint on the wrong instance. MEO's uid-0 path (ApiEnsureInstanceXList) mints IN PLACE on the first xList that is worn AND has no ExtraUniqueID AND no kEnchantment; it falls through to the drop-stamp-pickup of an unworn copy ONLY when no such worn xList exists. MFO reaches :586 only for an item whose worn xList it already located (addItem :437), uid 0 and !xlEnchanted (:562) — exactly the instance MEO will pick; the junk copy is never a candidate. Cost: a follower carrying a second Iron Dagger never gets the worn one gemmed until the spare sells — with economy off, never.
- **Reviewer's reasoning:** as above — the guard defends against a path MEO's own code cannot take for the instance MFO targets.
- **Why it was NOT fixed:** coordinator call 2026-09-15: round returned nothing above SEV-3, cycle ended (rule 9); the deferral is loud (LEFTOVER `duplicate-copy` every 60 s) and cannot corrupt the next test.
- **Fix shape when drained (verbatim):** drop the dupDeferred branch and kDuplicateCopy (keep mintedBases one-per-base-per-pass).
- CLOSED 2026-10-12 (`fix/mfo-m1a-equip-gem-backlog`): premise verified in MEO `native/plugin.cpp` `ApiEnsureInstanceXList` (:9638-9655 mints in place on a worn, uid-less, un-enchanted xList; :9656-9677 drop-stamp-pickup only when none exists). The deferral and the `kDuplicateCopy` class are removed from `MEOBridge.cpp` (`mintedBases` kept).
- OPEN (SEV-4, deferred, raised by the Opus review of bb925fa): a one-frame race remains between MFO's worn-xList read on the worker/main pass and MEO's main-thread mint (a spare copy could be picked if the worn xList changes in that frame). Not fixed here.

### MFO-B36 — the swap-up never decrements `avail[loot]`, so two items can evict their weakest for one loose copy
- **Raised:** Fable round 2 on `9dacc0e` (`fix/mfo-meo-no-loose-gems`), SEV-5, CONFIRMED.
- **Finding (verbatim):** the swap-up never decrements avail[loot], so item A and a later item B can both evict their weakest for the same single loose copy; next pass one re-fills with its own evicted gem — one wasted unsocket/re-socket, no loop.
- **Reviewer's reasoning:** bounded to one wasted pair of MEO ops per such pass; no loop, no loss.
- **Why it was NOT fixed:** coordinator call 2026-09-15: nothing above SEV-3 this round, cycle ended (rule 9); tier 2 is OFF by default and gem CHOICE is marth's planned redesign.
- **Fix shape when drained (verbatim):** reserve the candidate at the swap-out (`--avail[loot]` when `UnsocketGem` returns true) so a later item cannot evict for the same copy.
- **FIXED-IN `fix/mfo-loot-m1`** (loot round M1, 2026-09-24): the fix shape as written, `--avail[loot]` when `UnsocketGem` returns true (`native/MEOBridge.cpp`, the swap-up block). The LEFTOVER line's `swapPending` subtraction is removed with it (and the now-dead `swapPending` map), because the reserved copy is already out of `avail[]` and subtracting it again would count it twice. Pending its Opus 5.5 review on the branch.
- **ON MAIN (2026-09-25, checked by `feat/mfo-swapup-ammo`):** loot M1 merged (`e39cee2`, tier-A reviewed); `native/MEOBridge.cpp` swap-up block carries `--avail[static_cast<std::uint32_t>(loot)]` on a true `UnsocketGem`. Nothing left to fix in MEOBridge. The swap-up rule for gear and ammo (`native/logistics/SwapUp.cpp`, ClickUp 86e3ebfu3) carries the same reservation shape by construction (one plan per pool; `PlanBuy`'s ammo pass never plans past a stock line's remaining count). **CLOSED 2026-09-25** (coordinator, on the tier-B review of `4f23c30`): drained on main by loot M1 and carried by the swap-up rule.

### MFO-B37 — `Actuation.cpp` is over the 2500-line cap (2678): the split is its own brief
- **Raised:** Fable tier-3 review of `87cabc1` (`feat/mfo-1.5.97-pass`), SEV-5 hygiene, CONFIRMED (`wc -l`: 2605 on `main` `5e1c41b` before the branch, 2678 after round 2's gate comments and the EquipLeftHeld refusal logging).
- **Severity:** SEV-5 (process; no behaviour)
- **Finding (verbatim):** Actuation.cpp 2615 lines reported.
- **Reviewer's reasoning:** CLAUDE.md's 2500-line HARD RULE is overridden by scope rule 1 inside an unrelated task — crossing the cap is a STOP-and-report, never a licence to split; a TU split is tier 3 (never "CI-identical") and needs its own field cycle.
- **Why it was NOT fixed:** the 1.5.97 pass brief said "cap relaxed — report, do not split". The file was already over the cap on `main` (the dual-wield / combat-pick work of 2026-09-13 took it from ~2480 to 2605); this branch added 17 comment lines.
- **Fix shape when drained (verbatim):** a dedicated split brief for `Actuation.cpp` — candidates are the weapon equip block (`WeaponRolesFor` `:1677`, `IsOneHandMelee` `:1685`, `PickOffHandWeapon` `:1695`, `EquipLeftHeld` `:1748`, `EquipWeapon` `:1782`, the `g_forcedWeapon` ledger + `ReconcileForcedWeapon`/`CoLoadForcedWeapons`) into an `Actuation_Equip.cpp` next to `Actuation_Direct.cpp`/`Actuation_Hands.cpp`, shared state through `Actuation_internal.h`, as the two earlier splits did. Full adversarial Fable review, one field cycle on 1.6.1170 before the next feature lands on it.
- **DRAINED by `refactor/subsystem-folders-wave1` (2026-09-24), pending that branch's tier-A review + field cycle.** The dedicated split brief (marth 2026-09-24, "Go with the subsystem folders, tool first") moved the whole Actuation family to `native/cast/` by concern; the equip block this entry names is `cast/Equip.cpp` (`EquipWeapon`, the weapon-style helpers, the `g_forcedWeapon` ledger, `ReconcileForcedWeapon`, the FWPN co-save), largest file `cast/CastOn.cpp` at 1387 lines. The same wave split `APMFBridge.cpp` (the file MFO-B40's addendum asked to include) into `native/apmf/` and `ProgAllocator.cpp` into `native/progression/`. Proof: `tools/splitcheck` function-by-function over the CI builds + `tools/splitcheck/linecheck.py`. Close this entry when that review comes back clean.

### MFO-B39 — `bApmfEquipAuthority` is the first per-feature APMF toggle in the MCM; conflicts with the recorded consolidation plan
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-5 (F12). marth's call.
- **Severity:** SEV-5 (policy)
- **Finding (verbatim):** F12 (bApmfEquipAuthority is the first per-feature APMF toggle in the MCM, conflicts with the recorded consolidation plan — marth call).
- **Reviewer's reasoning:** memory `mcm-apmf-toggle-consolidation` records the plan to drop per-feature APMF toggles and keep ONE global force-fallback in Debug (default OFF); `bApmfCast`/`bApmfLootTravel`/`bApmfRetreat` are INI-only, this one is exposed in the MCM General tab.
- **Why it was NOT fixed:** the brief asked for the MCM toggle by name; the consolidation is marth's decision, not a review fix.
- **Fix shape when drained (verbatim):** either move `bApmfEquipAuthority` to INI-only beside the other three, or fold all four into the planned single Debug-tab fallback switch. The INI key name is frozen either way (MCM-Helper persistence identity).

- REOPENED 2026-10-06 (Opus spot-check): back to LIVE. `g_apmfEquipAuthority` gates the whole ch.17 channel, not only the declaration road (`apmf/Equip.cpp:91,114,196,246`, release in `apmf/Bridge.cpp:1157`), including the outfit re-apply deny that ClickUp 86e3jwmk7 keeps, and that task never mentions the toggle.

### MFO-B40 — `Actuation.cpp` 2803: the split brief (MFO-B37) should precede the v8 re-mirror
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-5 hygiene (F13).
- **Severity:** SEV-5 (process; no behaviour)
- **Finding (verbatim):** F13 (Actuation.cpp 2803: split brief B37 should precede the v8 re-mirror).
- **Reviewer's reasoning:** the branch adds `PostHandEquipsDeferred` / `DeclareFromLedger` and the authority branches inside `EquipWeapon` on top of a file already over the cap (MFO-B37); the v8 hand-aware `SetEquipSetEx` re-mirror will REMOVE the deferred hop and touch the same block again — doing that on a split file costs one review, doing it before the split costs two.
- **Why it was NOT fixed:** scope rule 1 — a split is its own brief and its own field cycle.
- **Fix shape when drained (verbatim):** run MFO-B37's split (`Actuation_Equip.cpp`) BEFORE the v8 re-mirror brief; the v8 brief then retires `PostHandEquipsDeferred` inside the new TU.
- **Still growing, 2026-09-22 (`fix/mfo-spell-authority-0922`, REPORTED not acted on):** `Actuation.cpp` is now **2978** lines — the stand-down re-arm added ~75 to a file already over the cap. Per CLAUDE.md's scope rule the cap is a STOP-and-report, never a licence to split inside an unrelated task, so nothing was split. **Also now on the watch list: `APMFBridge.cpp` is 2471** and the ch.8 gate put ~200 of that there, so the next addition to that file crosses the cap as well and `MFO-B37`'s split brief should be scoped to cover both.
- **Split landed, 2026-09-24 (`refactor/subsystem-folders-wave1`, see MFO-B37):** both files are split (`native/cast/`, `native/apmf/`), which is the ordering this entry asked for: the v8 re-mirror now lands on the split file (`cast/Equip.cpp`). Note for whoever closes it: `PostHandEquipsDeferred` no longer exists in the tree (`grep` 2026-09-24: only two comments still name it, `Followers.cpp:348` and `Logistics_Economy.cpp:972`). Close with MFO-B37 on that branch's review.

### MFO-B42 — `RecordLeftHold` proceeds with a null `LeftHandSlot()`
- **Raised:** Fable tier-3 review of `e0b52b5` (`feat/mfo-equip-authority`), SEV-5 (F-D).
- **Severity:** SEV-5
- **Finding (verbatim):** F-D (RecordLeftHold proceeds with a null LeftHandSlot()).
- **Reviewer's reasoning:** `EquipLeftHeld` refuses (and logs) when `Loadout::LeftHandSlot()` resolves null; `RecordLeftHold`'s one engine call — the force-unequip of a DIFFERENT weapon MFO had locked in the left — passes that slot without checking it, so on a runtime where the slot form is missing the unequip is slot-less (the F4 unverified path) instead of refused.
- **Why it was NOT fixed:** below the floor; the slot is the LeftHand form in Skyrim.esm on every runtime (v2.0.8) and the branch only ever runs when a legacy-path lock stands in the left.
- **Fix shape when drained (verbatim):** mirror `EquipLeftHeld`'s precondition — skip (and log once) the unequip when `LeftHandSlot()` is null.

### MFO-B44 — owning Ammo under a bow hold pins the archer to one ammo stack
- **Raised:** Fable tier-B review of `7857446` (`feat/mfo-equip-authority-v9`), SEV-4 (F2).
- **Severity:** SEV-4 (design; marth's call)
- **Finding (verbatim):** F2 SEV-4 owning Ammo under a bow hold pins the archer to one ammo stack (exhausted stack → engine's switch to another stack denied until a re-declaration; options: do not own Ammo, or re-declare on ammo change — marth's call).
- **Reviewer's reasoning:** under a bow/crossbow hold the scope owns `Ammo` and the declaration carries the worn (else best) ammo of the matching kind; when that stack runs out the engine's own switch to the next stack is an off-set AMMO equip in an owned category and is refused until the next declaration event rebuilds the set with a new best stack (the OOC service, a ledger change, a bound change).
- **Why it was NOT fixed:** below the severity floor, and the two fix shapes are a design choice (own Ammo or not) marth owns.
- **Fix shape when drained (verbatim):** do not own Ammo, or re-declare on ammo change — marth's call.

### MFO-B58 — the two Task-1 concentration claims have no `combatController` test (own-OOC party-combat follower)
- **Raised:** Fable tier-B round 2 on `708fe7a` (`fix/mfo-party-combat-gate` merged with `fix/mfo-combat-restoration-direct`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** the two Task-1 concentration claims -- Actuation_Direct.cpp:974 (self) and :1337 (target) -- gate on `!Heal && !IsRestorationSpell && kConcentration && APMF` with NO `combatController` test. A non-restoration CONCENTRATION Buff on an own-OOC follower (self with bCastSelf ON, or at ally/player through `ConcentrationCast`) reaches `ClaimOffenseCast` on a follower with no caster -> `Applied` -> Fired (opaque) -> a dead claim walls the lap exactly like the closed SEV-2. Vanilla has no such spell on that road (Telekinesis is the only conc self non-restoration; conc buffs at allies are modded). Fix: mirror `:1302`'s `combatController &&` on both Task-1 sites.
- **Reviewer's reasoning:** as stated in the finding — the restoration branch closed the heal road by construction (Heal ⊂ Restoration), but the non-restoration concentration claim road is the same shape and has only `CastTargetDirect`'s `:1302` heal-twin carrying the controller test.
- **Why it was NOT fixed:** below the floor for the cycle (SEV-4), no vanilla spell reaches it, and the gate branch's boundary is `Scheduler.cpp` — the fix is two one-line edits in `Actuation_Direct.cpp` for its own brief.
- **Fix shape when drained (verbatim):** mirror `:1302`'s `combatController &&` on both Task-1 sites.

### MFO-B59 — own-OOC party-combat residuals (SEV-5 bundle)
- **Raised:** Fable tier-B round 2 on `708fe7a` (`fix/mfo-party-combat-gate`), SEV-5 x3.
- **Severity:** SEV-5
- **Findings (verbatim):** (a) `ownedCast` (Actuation.cpp ~:951) with an explicit-subject ally and an Offense spell reaches `ClaimOffenseCast` the same way; (b) the direct STREAM `g_targetCast` outlives a castSeen-false lap by up to `TargetCastReconcile`'s stale window (max(2 s, suppress*1.12 + 0.133*party + 0.5)); benign, holds no hand, at most one `stream RELEASE (switch)` at the condition flip; (c) a magicka-dry Declined stretch skips logistics while the ally stays hurt, same wait own combat imposes; `g_nextTick` leaves `due` in the past so the first post-stretch lap runs at once.
- **Reviewer's reasoning:** (a) is the offense twin of MFO-B58 (an authored Offense spell aimed at an ally by explicit subject, a misauthored gambit); (b) the `castSeen` gate closes the hand lock on the same lap but the stream registry is bounded by its own reconcile, so one lap's OOC dispatch can still hit a live entry once at the flip; (c) the chosen fix shape (the condition, not a live-stream read) makes a Declined lap the combat rule's, so logistics waits — the same wait the follower's own combat imposes, and the cadence gate does not add a second delay.
- **Why it was NOT fixed:** (a) misauthored-gambit edge, no vanilla path; (b) benign by the reviewer's own reading; (c) is the documented trade of the chosen fix shape.
- **Fix shape when drained (verbatim):** (a) with MFO-B58's controller test; (b) none, or clear the direct stream from the `!castSeen` release block beside `ClearCastLock`; (c) none.

### MFO-B60 — the party-OOC teardown other than `ReleaseForcedWeapon` ran on the FIRST party-OFF service (undebounced)
- **Raised:** Fable field diagnosis of the 2026-09-21 21:50 Deck run (`aca7d43`, agentlog `field-diag-20260921-evening.md`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** "Party OFF tick 34.884: Jesper was the serviced follower (his loot scan 34.885 + latch re-print 35.301); [wstyle] OWNED lines 34.936/34.967 = engine re-created both controllers -> genuine engine combat restart, gate mirrored it; 145 ms flap released nothing (N=2 debounce). But the OTHER party-OOC teardowns (:343-373) are NOT debounced -> SEV-4." Memory note: "New SEV-4: party-OFF tick tears down consent latch/cast lock/style undebounced."
- **Reviewer's reasoning:** `Scheduler.cpp` `:343-373` (RetreatClear, `g_combatEnteredAt.erase`, `g_proposedTarget.erase`, `CasterConsent::Clear`, `Actuation::ClearCastLock`, `CombatStyle::Clear`, the per-fight latches) ran on the first party-OFF tick while only `ReleaseForcedWeapon` (`:384`) waited two services. A 145 ms OFF/ON flap -- an engine combat-controller restart, which the field log shows is real -- would drop a caster's consent latch and cast lock mid-fight: the sub-tick leak v1.0.32 closed, re-opened for one lap.
- **FIXED-IN `e0ea399`** (`fix/mfo-field-batch-0921`, item G): the whole party-OOC teardown now sits behind the same `++g_outOfCombatTicks[id] >= 2` count as the hold release; `Logistics::ServiceFollower` still runs on every party-OOC tick. Pending its Fable review on the branch.

### MFO-B61 — a fault inside the `[fatal]` handler's own log call degrades CrashLogger's report to the nested exception
- **Raised:** Fable tier-A review of `7520e3e` / `3315848` (`fix/mfo-field-batch-0921`, item B), closing round.
- **Severity:** SEV-5 (documented degradation, no fix shape without moving the line off the faulting thread)
- **Finding (verbatim):** handler-fault degrades the report: a fault inside log->critical under the VEH makes the NESTED exception (inside MFO.dll) reach CrashLogger and the original address is lost; document in the block comment too.
- **Reviewer's reasoning:** the vectored handler runs on the faulting thread before any filter; the `thread_local` re-entry guard makes the nested fault fall straight through to CrashLogger, so the report names MFO's logger frame (module MFO.dll) rather than the original faulting address. The original `[fatal]` line is never written in that case.
- **Why it was NOT fixed:** the only shapes that avoid it (formatting into a pre-reserved static buffer with no allocation and writing with a raw `WriteFile`, or handing the line to another thread) are a separate mechanism; the block comment now states the degradation (`Diagnostics.cpp` `[fatal]` block comment, `3c733c9`).
- **Fix shape when drained (verbatim):** none given; candidate: pre-reserved static buffer + hand-declared `WriteFile` on the already-open log handle, no spdlog call inside the handler.

### MFO-B62 — `EquipRangeUndecidable` answers for the FIRST range-conditioned equip rule above the stop, even if a second non-range equip rule above the stop was genuinely false
- **Raised:** Fable tier-A review of `7520e3e` / `3315848` (`fix/mfo-field-batch-0921`, item F), closing round.
- **Severity:** SEV-5 (conservative; matches the intent)
- **Finding (verbatim):** EquipRangeUndecidable returns true on the first range-conditioned equip rule above the stop even if a second non-range equip rule above the stop was genuinely false (conservative, matches intent).
- **Reviewer's reasoning:** `Scheduler.cpp` `EquipRangeUndecidable` scans rules `< stopIdx` for the first equip rule whose condition is target-relative and answers for that one; a table with a range-conditioned equip rule AND a second, non-range equip rule (e.g. `cond.always` → `act.equip_melee`) both above the stop reads UNKNOWN on a null-target lap although the second rule's truth was genuinely known. The effect is one extra hold-kept lap per null-target stretch, in the direction the fix intends (keep the weapon).
- **Why it was NOT fixed:** conservative in the safe direction, two equip rules above the stop is not an authored layout the field has shown, and the per-rule answer would need the Evaluator to report which rule it evaluated (an `Evaluator.h` change outside item F's boundary).
- **Fix shape when drained (verbatim):** none given; candidate: have the Evaluator mark per rule whether the target-relative read was undecidable, and let the Scheduler demote only when EVERY evaluated equip rule was undecidable.

### MFO-B56 — `Loadout::Tick` erases `g_equipClock` on own `IsInCombat()==false`, collapsing the AI-first grace for own-OOC hybrid casts
- **Raised:** Fable tier-B review of `dea438f` (`fix/mfo-party-combat-gate`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** Loadout::Tick (Diagnostics.cpp:348, worker pump) erases g_equipClock when own IsInCombat false -> SecondsSinceEquip=1e9 -> AI-first grace collapses for own-OOC hybrid casts -> ForceCast on the second lap. Benign (heal lands), accidental.
- **Reviewer's reasoning:** `Loadout.cpp:545` keys the clock erase on the follower's OWN flag; under party combat an own-OOC follower on the APMF-absent / non-restoration hybrid road gets `SecondsSinceEquip` = 1e9 and `ForceCast` fires without the grace. Restoration casts no longer take that road (they go direct), so the affected set is the APMF-absent hybrid for non-restoration spells.
- **Why it was NOT fixed:** benign (the cast lands, unanimated one lap early), `Loadout.cpp` outside the gate branch's boundary, and the hybrid road is the APMF-absent degrade only.
- **Fix shape when drained (verbatim):** key the `g_equipClock` erase on `Scheduler`'s party-combat state (expose it, or erase from the party-OOC branch) instead of the follower's own flag.

### MFO-B57 — party-combat gate cosmetics: `[sense] foes=0` every 3 s for own-OOC followers; the ready beat is consumed at party entry
- **Raised:** Fable tier-B review of `dea438f` (`fix/mfo-party-combat-gate`), SEV-5 x2.
- **Severity:** SEV-5
- **Findings (verbatim):** [sense] line now prints foes=0 every 3 s for own-OOC followers; ready beat consumed at party entry.
- **Reviewer's reasoning:** the `[sense]` tally reads the follower's OWN combat group, which is empty for an own-OOC follower, so the line repeats a true-but-uninformative 0 at its 3 s cadence for every such follower in a party fight; the once-per-combat ready beat fires on the party-combat edge rather than the follower's own engagement, so his first own-combat lap has no beat.
- **Why it was NOT fixed:** cosmetic; the `[sense]` line is the field's foe-count diagnostic and silencing it for own-OOC followers hides the very state the gate keys on.
- **Fix shape when drained (verbatim):** print the party foe max alongside own foes on the `[sense]` line (`foes=0 party=N`); re-arm the ready beat on the own-combat edge as well as the party edge.

### MFO-B47 — the idle-hand floor's unobserved gate is a FOURTH consumer of the contested 4000 ms constant
- **Raised:** Fable tier-B review of `ed3d9d0` (`fix/mfo-combat-restoration-direct`), SEV-3 (note). Ids `MFO-B47`-`MFO-B49` continue from `main`'s highest, `MFO-B46`.
- **Severity:** SEV-3 (note; the cycle ended with nothing above SEV-3)
- **Finding (verbatim):** the floor gate is consumer (d) of the 4000 ms constant.
- **Reviewer's reasoning:** `kIdleFloorUnobservedMs` aliases `kHealHoldNeverObservedMs`, which `MFO-B8` already records as answering three differently-sized questions (a)-(c). The gate adds (d): an OFFENSE claim's latency WITH an equip cycle in front of it (heals no longer claim on this branch). The header comment as first written asserted "4000 ms clears every measured latency"; `MFO-B7` records the 0908 heal at 4.5-6.1 s (`Docs/DIAG-2026-09-08-field.md:267`), so it does not, and (d) itself is UNMEASURED.
- **Why it was NOT fixed:** B7's ruling — measure, do not resize from n=2. The comment was corrected in the closing round to cite B7/B8 and the 4.5-6.1 s datum and to say (d) is unmeasured; the constant is unchanged. Cost of the gate firing early is the pre-F10 state (the other hand opens to the AI while a real cast still charges), never a freeze.
- **Fix shape when drained (verbatim):** read the next Deck log's `[cfc]`/castobs fire timestamps against the `IDLE-HAND FLOOR released -- driving claim has no observed cast` lines; size (d) from that, as its own named constant under B8's scheme.
- **DRAINED by `fix/mfo-spell-authority-0922` (2026-09-22).** The 2026-09-22 Deck log is the measurement this entry asked for, and it came back against the alias: an OFFENSE claim reaches an observed SpellFire in **5.8 s** with an equip cycle in front (Jesper 750012C6, claim 08:26:59.202 -> SpellFire 08:27:04.997), and the floor lifted **60 ms AFTER** the engine began charging the claimed Firebolt, with the AI charging its own spell in the freed hand 0.78 s later. Consumer (d) is now **its own named constant under B8's scheme**: `kIdleFloorUnobservedMs = 8000` (no longer aliased to `kHealHoldNeverObservedMs`), which clears 5.8 s by 2.2 s and the 0908 heal's 6.1 s by 1.9 s. **And the age is no longer the whole test:** `silentPastGate` also refuses to call a claim silent while `Actuation::CastInFlightOnHand` reports the engine charging that claim's spell-or-proxy on that hand -- the 60 ms figure proves a longer timer alone could not have prevented the failure. `MFO-B7` and `MFO-B8` stay OPEN for `kHealHoldNeverObservedMs` itself, which this change did not touch (its consumers (a)-(c) are the heal-claim heartbeat and the cast-lock hold cap). `kSilentWarnAfter` was re-sized 2000 -> 3500 ms in the same change (its 2000 ms window was below the 2.3-2.5 s claim-to-charge latency, so the `[cfc] NO observed cast` warning was false on every claim of the session) and given the same in-flight exemption; both `static_assert`s still hold.

### MFO-B48 — `bHealAnimPackage` is inert but its MCM help text still promises an animated hand cast
- **Raised:** Fable tier-B review of `ed3d9d0` (`fix/mfo-combat-restoration-direct`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** `bHealAnimPackage` MCM toggle inert, help text at out/MCM/Config/MFO/config.json:236-239 still promises an animated hand cast.
- **Reviewer's reasoning:** every `ComposedCast::Try` call site is now gated on `!IsRestorationSpell`, and `Try` is heal-only, so the toggle that enabled the CFC heal claim changes nothing on either table. The MCM entry ("Animated forced casts (experimental) ... Route forced casts (heals, buffs) through a real animated hand cast") describes a road that no longer exists.
- **Why it was NOT fixed:** the INI key name is a frozen contract (MCM-Helper persistence identity, CLAUDE.md "frozen external contracts"); changing the text or hiding the toggle belongs to the CFC-removal brief, not this field fix.
- **Fix shape when drained (verbatim):** in the removal brief, reword or hide the MCM entry (key name kept) in the same change that deletes the dormant `Try`/`ClaimHealCast`/F1-hold road.
- **DRAINED by `feat/mfo-animheal-p2` (2026-09-30), pending its tier-A review.** The road was not removed, it came back: `bHealAnimPackage` is again what it gates (the combat heal claim road, now DEFAULT ON as a kill switch, marth's D2 "keep it for now"). The MCM text now says so ("Animated heals in combat", help rewritten), key name kept.

### MFO-B49 — closing notes on `fix/mfo-combat-restoration-direct` (SEV-5 bundle)
- **Raised:** Fable tier-B review of `ed3d9d0` (`fix/mfo-combat-restoration-direct`), SEV-5 x4.
- **Severity:** SEV-5
- **Findings (verbatim):**
  1. modded FF Restoration-school Buff at an ally now streams direct with utility cap + dispel;
  2. `RestorationCastDirect`'s `SelfCast::Held` arm (Actuation.cpp:423) is dead;
  3. floor pulses ON for the first 4 s of every re-mint of a perpetually silent claim (by design);
  4. CHANGELOG could say heals are now instant for animated-heal users.
- **Reviewer's reasoning:** (1) a non-heal Restoration-school FF spell aimed at an ally used to take the equip/grace/force road; on the direct road it is a Buff-kind `CastTargetDirect` stream, so it inherits the utility time cap and the dispel-on-release. (2) `CastTargetDirect` only returns `Held` out of `ComposedCast::Try`, which restoration never reaches, so the arm cannot fire; kept for enum completeness (no `default:` masking). (3) an aged-out re-request re-mints `created`, so a claim that never fires earns a floor for the first 4 s of each re-mint before the gate drops it again — the documented consequence of anchoring on the mint-only clock. (4) user-facing wording.
- **Why it was NOT fixed:** (1)-(3) are by-design consequences already stated in code comments; (4) was applied in the closing round (CHANGELOG v2.0.8 bullet).
- **Fix shape when drained (verbatim):** (1) none unless the field shows a ward/buff stream misbehaving; (2) delete the arm when the CFC road is removed; (3) anchor the gate on a per-spell "first minted" stamp if the pulse ever matters; (4) done.

### MFO-B46 — the `abiVersion < 9` warn in `EquipAuthoritySupported` is unreachable
- **Raised:** Fable tier-B review of `7857446` (`feat/mfo-equip-authority-v9`), SEV-5 (F5).
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** F5 SEV-5 the abiVersion<9 warn is unreachable belt-and-braces (note).
- **Reviewer's reasoning:** `Acquire()` asks APMF for `kABIVersion` (9) and an older APMF answers nullptr, so `g_apmf` is never a `< 9` interface; the branch exists only as the same defensive shape the v5/v6 guards use.
- **Why it was NOT fixed:** a note, not a defect; the guard costs nothing and documents the floor.
- **Fix shape when drained (verbatim):** none; keep as belt-and-braces or drop with the other unreachable ABI guards in one sweep.

### MFO-B50 — `AtkClearSlot` from `ResetAllState` can race one in-flight `ProcessEvent`
- **Raised:** Fable tier-A review of `2ac4163` (`feat/mfo-attack-observe`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** `AtkClearSlot` from `ResetAllState` (main) can race one in-flight `ProcessEvent` on the graph thread (Diagnostics.cpp:658-666). `fid` is stored 0 first (release), then counters are stored 0 relaxed; a sink that loaded the old fid before the clear can `fetch_add` after the counter store, leaving <=1 stale count in a free slot that the next occupant inherits into fight#1. Cosmetic (an off-by-one already accepted at fight boundaries). Same for `attachLogged.store(false, relaxed)` vs main's `exchange`. No fix needed; record it.
- **Reviewer's reasoning:** the sink runs on the graph's dispatch thread, which StopPump does not drain; a `ProcessEvent` that loaded `fid` just before `AtkClearSlot` stored 0 can `fetch_add` one counter after the worker's zeroing pass, and that single count is inherited by whoever next takes the slot.
- **Why it was NOT fixed:** below the floor; at most one count on one counter at a load boundary, on a diagnostic line.
- **Fix shape when drained (verbatim):** cosmetic, no fix. (If ever: zero the counters at slot ASSIGNMENT after the fid store, or reject events for a slot whose generation changed.)

### MFO-B51 — pinned 3.7.0 has `Actor::AddAnimationGraphEventSink` / `RemoveAnimationGraphEventSink`
- **Raised:** Fable tier-A review of `2ac4163` (`feat/mfo-attack-observe`), SEV-5 (note).
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** pinned 3.7.0 has `Actor::AddAnimationGraphEventSink/RemoveAnimationGraphEventSink` (Actor.h:494/:603, graphs.front() only, equivalent for an NPC).
- **Reviewer's reasoning:** the CommonLib helper registers on the first graph only; MFO's `AtkPostAttach` walks every graph in `mgr->graphs`. An NPC has one graph, so the two are equivalent there; the hand-rolled walk is a superset.
- **Why it was NOT fixed:** a note; the walk is correct and covers a multi-graph holder too.
- **Fix shape when drained (verbatim):** none required; could swap to the helper for brevity.

### MFO-B52 — `mgr->graphs` walked without `BSAnimationGraphManager::updateLock`
- **Raised:** Fable tier-A review of `2ac4163` (`feat/mfo-attack-observe`), SEV-5 (note).
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** `mgr->graphs` walked without `BSAnimationGraphManager::updateLock` (CommonLib helper does the same; engine path not provable, .text is SteamStub-encrypted).
- **Reviewer's reasoning:** the walk runs on the main thread inside `MainThread::Post`; CommonLib's own `AddAnimationGraphEventSink` reads `graphs` the same way without the lock; whether the engine takes `updateLock` around graph replacement could not be read from the encrypted binary.
- **Why it was NOT fixed:** a note; same shape as the library helper, main-thread only.
- **Fix shape when drained (verbatim):** none; if a graph-swap race is ever observed, take `mgr->GetRuntimeData().updateLock` around the walk.

### MFO-B53 — `g_atkUnk` unknown-tag table is session-wide by design
- **Raised:** Fable tier-A review of `2ac4163` (`feat/mfo-attack-observe`), SEV-5 (note).
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** `g_atkUnk` session-wide by design.
- **Reviewer's reasoning:** the 32-entry table is process-lifetime (survives a revert) so "new-tag" first-sight lines print once per session, not once per save; a table filled by one profile's tags stays filled.
- **Why it was NOT fixed:** by design (documented in the block header).
- **Fix shape when drained (verbatim):** none.

### MFO-B54 — `!seen[i]` in the free-slot search is redundant
- **Raised:** Fable tier-A review of `2ac4163` (`feat/mfo-attack-observe`), SEV-5 (note).
- **Severity:** SEV-5 (note)
- **Finding (verbatim):** `!seen[i]` at :770 redundant.
- **Reviewer's reasoning:** a slot with `fid == 0` cannot have been marked seen in the same pass (seen is set only after a fid is stored or matched), so the extra test never changes the result.
- **Why it was NOT fixed:** harmless; a closing-round nit.
- **Fix shape when drained (verbatim):** drop the `&& !seen[i]`.

### MFO-B43 — `g_playerPicks` survives the toggle OFF path
- **Raised:** Fable tier-3 review of `e0b52b5` (`feat/mfo-equip-authority`), SEV-5 (F-E).
- **Severity:** SEV-5
- **Finding (verbatim):** F-E (g_playerPicks survives the toggle OFF path).
- **Reviewer's reasoning:** `RefreshEquipDeclaration`'s gate-off branch erases `g_lastDeclared[id]` and releases the claim but not `g_playerPicks[id]`; `ForgetEquipDeclaration` (dismiss) and `ClearEquipDeclarations` (revert) clear both. A follower whose authority was turned off keeps his player-pick keep list until dismissal, which only affects the economy's keep verdict for those forms.
- **Why it was NOT fixed:** below the floor; the effect is a kept piece, never a sold or worn one.
- **Fix shape when drained (verbatim):** erase `g_playerPicks[id]` beside `g_lastDeclared.erase(id)` in the gate-off branch.

### MFO-B34 — should the tier-2 swap-up ever evict a Conduit that an already-socketed off-domain gem depends on?
- **Raised:** Fable tier-3 review of `1efa3e3` (`fix/mfo-meo-no-loose-gems`), policy question attached to the SEV-2 (fixed on the branch: the candidate is judged against the item WITHOUT the evictee, `sansEvictee`).
- **Finding (verbatim):** Policy question for marth, record in backlog: should MFO ever evict a Conduit that an already-socketed off-domain gem depends on?
- **Reviewer's reasoning:** the SEV-2 fix stops the self-driven loop (an off-domain candidate can no longer be admitted through the Conduit it would evict), but a SAME-domain candidate that strictly out-scores the Conduit's class bonus (1) still evicts it, and MEO then holds an off-domain gem on the item with no Conduit to remap it — what MEO does with that gem is MEO's rule, not MFO's.
- **Why it was NOT fixed:** gem CHOICE (tier-2 ordering and preference) is marth's planned redesign; the brief forbade changing it.
- **Fix shape when drained (verbatim):** when `det` holds a Conduit AND an off-domain gem, exclude the Conduit from the weakest-gem search (it is load-bearing); or rank a Conduit by the gem it carries.
- **RESOLVED by marth's ruling (2026-10-12, "They won't use conduit"), `feat/mfo-gem-relevance`:** followers never socket a Conduit and MFO never inserts an off-domain gem through one, in either tier, on any MEO version (the insert half is "never insert through a Conduit"). A Conduit already socketed (by the player, or carried by MEO's gear move), its linked partner and any off-domain gem are never evicted by the swap-up. A loose Conduit stays loose with an info `conduit` LEFTOVER line.

### MFO-B66 -- `DenyLog`'s dedupe hides how long a refusal lasts, not which spell it is
- **Raised:** author of `fix/mfo-cast-latch-gambit-set` (2026-09-23), verifying Fable's field-diagnosis item 3 rather than changing it. Fable had suggested a time-based dedupe as a tier-C follow-up.
- **Severity:** SEV-5 (diagnostic legibility, no behaviour)
- **Finding:** `DenyLog` (`CasterConsent.cpp:509`) dedupes on `(follower, last denied spell)` by EQUALITY, so a DIFFERENT spell always logs a line. It cannot hide a new distinct problem spell. What it does hide is the DURATION and the REPEAT RATE of one spell's refusal: the 2026-09-23 session logged Lightning Bolt's refusal twice in eight minutes while the AI asked hundreds of times, which read as two incidents instead of a permanent condition. The dedupe entry resets on `NoteOurCast` and on `Clear`, so a repeat after either logs again.
- **Why it was NOT fixed:** the brief's condition for changing it was "if it CAN hide a new distinct problem spell", and it cannot. Item 1 of that branch should make these denies rare anyway.
- **Fix shape when drained:** carry a `Clock::time_point` beside the spell in `g_lastDenied` and re-log the same pair after ~30 s. About ten lines, touching the map's five use sites (`:513-515`, the two erases, the clear). Same shape would apply to `g_lastConcDeny` and `g_lastAbort`, which have the identical blindness.

### MFO-B67 -- the v<8 retro uses a GMST ratio while the forward cap uses the player's live gain
- **Raised:** Opus 5.5 tier-A review of `6891b0a` (`fix/mfo-hms-player-rate`, §HMS player-rate parity, PRGN v8), 2026-09-23.
- **Severity:** SEV-4
- **Finding (verbatim):** retro uses a GMST ratio while the forward path uses the player's live gain, so a list with an SKSE plugin changing the player's per-level gain would disagree
- **Reviewer's reasoning:** `HmsRetroParity` (`ProgAllocator_Hms.cpp`) scales by iAVDhmsLevelUp/(iAVDhmsLevelUp+fNPCHealthLevelBonus). No per-level player gain is on disk for the past, so the GMSTs are the only source. The forward path reads the live gain, so the two agree only while nothing outside the GMSTs changes the player's per-level gain.
- **Why it was NOT fixed:** below the floor. Tuxborn has no such plugin (research-hms-magicka.md).
- **Fix shape when drained:** persist a running player-gain history, or recompute the retro from the player's own base total vs a stored enrollment-time player total. Both need a new field.

### MFO-B68 -- the retro's grant-history test misses whole-number grants
- **Raised:** Opus 5.5 tier-A review of `6891b0a` (`fix/mfo-hms-player-rate`, §HMS player-rate parity, PRGN v8), 2026-09-23.
- **Severity:** SEV-4
- **Finding (verbatim):** the grant-history test `hmsGrantRemainder != 0` misses whole-number grants, so a formerly fixed-stat follower's grants can be scaled down
- **Reviewer's reasoning:** a fixed-stat grant whose per-pool award lands on exact integers leaves every remainder at 0. If that follower later levels on his own (fixedStat cleared), the retro scales his grants with his engine awards.
- **Why it was NOT fixed:** below the floor. It needs a fixed-stat-then-leveling follower, which is rare.
- **Fix shape when drained:** record grant history explicitly (a flags bit set by the grant path), or keep a separate granted-cumulative.

### MFO-B69 -- the parity credit grows without limit for followers who never take an award
- **Raised:** Opus 5.5 tier-A review of `6891b0a` (`fix/mfo-hms-player-rate`, §HMS player-rate parity, PRGN v8), 2026-09-23.
- **Severity:** SEV-4
- **Finding (verbatim):** credit grows without limit for fixed-stat and level-capped followers, suggest skipping the credit when fixedStat is set
- **Reviewer's reasoning:** `PollWork` credits every enrolled record each player level-up. A fixed-stat or level-capped follower never spends it, so a later award passes in full. Never above the engine's own award.
- **Why it was NOT fixed:** below the floor. The cap is a ceiling, the credit only lets the engine's own award through.
- **Fix shape when drained:** skip `st.hmsParityCredit += playerGain` when `st.fixedStat` (and consider a clamp).
- **Update (2026-11-09, `fix/hms-above-parity`, ClickUp 86e3jw3b4):** the above-parity gate (`progression/Hms.cpp:446`) also caps each applied award at the headroom `max(0, player total - held total)`, so a banked credit can no longer carry a follower past the player's total. A frozen above-parity follower banks credit the same way (left as is, still this entry's call); the over-grant this entry describes is now bounded by the player's total.

### MFO-B70 -- non-level-up engine changes are now credit-capped
- **Raised:** Opus 5.5 tier-A review of `6891b0a` (`fix/mfo-hms-player-rate`, §HMS player-rate parity, PRGN v8), 2026-09-23.
- **Severity:** SEV-4
- **Finding (verbatim):** non-level-up engine changes (race change, another mod raising stats) are now credit-capped when positive and leave W high when negative
- **Reviewer's reasoning:** the signed drift cannot tell a level award from any other engine write to base H/M/S. A positive one is withheld without credit. A negative one leaves hmsWithheld above the engine's real total, so the next awards under-measure by that amount.
- **Why it was NOT fixed:** below the floor. Such writes are rare, and the pre-v8 code also measured them as awards.
- **Fix shape when drained:** separate level awards from other writes (compare the actor's level at each measure), or let a negative signed drift shrink W.
- **Update (2026-09-23, PRGN v8 `hmsHeld`):** DETECTION now exists. `RecomputeHMS` logs one `[hms-parity] <id> outside change` line per divergence when there is no award and the live total differs from `hmsHeld` by neither ≈0 nor ≈W. It does not act on it. The decision (adopt, revert, or shrink W) is still this entry's.

### MFO-B71 -- the parity log can print once with 0.0 values
- **Raised:** Opus 5.5 tier-A review of `6891b0a` (`fix/mfo-hms-player-rate`, §HMS player-rate parity, PRGN v8), 2026-09-23.
- **Severity:** SEV-5
- **Finding (verbatim):** the parity log can print once with 0.0 values after a retro or engine re-slam
- **Reviewer's reasoning:** `engineAward > 0` passes on float residue a few ulps above 0 after W is subtracted.
- **Why it was NOT fixed:** log noise only.
- **Fix shape when drained:** gate the parity block on `engineAward > 1e-3f`.

### MFO-B72 -- the retro-pending bit clears on the first award even if the pending levels arrive in two measurements
- **Raised:** author of `fix/mfo-hms-player-rate` (unsure #3 on `d0f1da6`), graded by the coordinator 2026-09-23.
- **Severity:** SEV-4
- **Finding (verbatim):** the 0x40 bit clears on the first award even if the pending levels arrive in two measurements
- **Reviewer's reasoning:** `RecomputeHMS` clears `hmsRetroPending` on the first non-grant award. If the engine levels pending at a v<8 migration (or after Enroll) are measured across two calls, only the first gets the `max(credit, award x ratio)` cap and the rest is capped by credit alone, so it is withheld.
- **Why it was NOT fixed:** coordinator's call: leave it, record it.
- **Fix shape when drained:** keep the bit until a player level-up has been credited (clear it in `PollWork` on `playerLeveled`), not on the first award.

### MFO-B236 -- Enemy / Self cast targets (Vocab::Subject Enemy=3 / Caster=4): deferred review findings F-D..F-H
- **F-G DECIDED + FIXED 2026-10-11 (marth: AUTO refuses only when ALL effects are undeclared):** `SpellNatureUndeclared` (cast/SpellSupport.cpp) now returns true only when every effect is Script / Calm / Frenzy / Demoralize / Paralysis / TurnUndead; one declared effect keeps AUTO's party behaviour. ClickUp 86e3m3q0j.
Raised against `d8b0aa3` (`feat/mfo-hide-unsupported-spells`, Opus tier-3 review, nothing above SEV-3), 2026-10-06.
Text below is the finding as relayed by the coordinator from the review.
- **F-D (SEV-4):** on an older DLL, re-enabling such a rule runs it at the player fallback, and a round trip silently leaves it disabled. Document it in the Serialization.h v5 note. *Reasoning:* the flags-bit-1 guard only makes an older DLL load the rule disabled; the older DLL still cannot resolve Enemy / Self, and it saves the rule back with flags 0. Documented in the `Serialization.h` v5 note in the F-A commit.
- **F-E (SEV-5):** the writer checks IsAppendedSubject without checking the action, so a stale subject on a non-cast row makes an older DLL load that row disabled. *Reasoning:* SetSubject leaves the subject byte when the action changes; only the downgrade case is affected.
- **F-F (SEV-5):** a Caster-pick fire-and-forget cast with bCastSelf off logs as "ally/player (immediate)" (Service.cpp:1976). *Reasoning:* log wording only; the routing matches act.cast_self.
- **F-G (SEV-5):** SpellNatureUndeclared triggers on ANY one effect, so a mixed ally buff is caught. *Reasoning:* a spell with a declared ally-buff effect plus one Script effect loses AUTO; the survey found no vanilla case.
- **F-H (SEV-5):** the hint shows on cast_self/cast_player rows; the Fire.cpp comment "the exact call act.cast_self makes" is inaccurate (rangeGate=true; behaves the same). *Reasoning:* cosmetic; CastOn reads a_rangeGate only for a target other than the follower.
- **Surfaced at edit time from:** MAP.md `Vocabulary.h` entry and the `cast/SpellSupport.cpp` entry.
- TRIAGE 2026-10-06: F-D is CLOSED: the downgrade behaviour is documented in the `Serialization.h` v5 note (`native/Serialization.h:185-186`). F-G has its own needs-decision task (ClickUp 86e3m3q0j). F-E stays LIVE.

### MFO-B238 (SEV-4 x3, SEV-5 x2) -- heal hold round 2 review (a687fd2): deferred R1-R3, spots 2-3
- **R1 DECIDED + FIXED 2026-10-11:** same change as the MFO-B234 SEV-3 bullet (5 s at the floor, still occluded -> approach-blocked release). ClickUp 86e3m450a.
Raised against `a687fd2` (`fix/mfo-post-2.2.0`, Opus tier-B round 2, MERGE OK), 2026-10-06. Text as relayed by the coordinator.
- R1 (SEV-4): at the 128u approach floor with sight still lost (recipient behind a wall / on another floor) the follower holds there; the never-fired bound stays off because the approach is not "blocked", so the heal holds until combat ends. Matches marth ruling (c); the 5 s still-holding line shows it. Possible follow-up for marth: treat "at the floor and still occluded" as approach-blocked.
- R2 (SEV-5): out of magicka during a restream gap: ReleaseOwnHealClaim returns early (no claim stands), HealHandEnded is not called, the record ends next tick as released-elsewhere instead of out-of-magicka. Log token only.
- R3 (SEV-4, pre-existing): the un-taught-proxy path can cycle claim -> HasSpell check -> restream release -> re-claim with a fresh stamp, each shorter than the never-fired cap; sustain now also runs it after the rule stops asking. Only if Harbinger keeps failing to teach the proxy; each cycle WARNs.
- Spot 2 (SEV-5): a held heal from a rule BELOW a suppression window is never evaluated, so it reads as "stopped asking" (false log line only; behaviour correct).
- Spot 3 (SEV-4): the combat-start gate (IsInCombat() with no combat controller) never casts if the controller never comes up; transparent, lower rules run, but the only trace is the chain reason. Recommendation: one WARN per fight once the wait passes a few seconds; no direct-road fallback (marth ruling d).
- TRIAGE 2026-10-06 (NEEDS-DECISION): R1 is filed as ClickUp needs-decision task 86e3m450a together with the open SEV-3 bullet of MFO-B234 (same shape: at the approach floor, recipient still occluded). R2, R3 and spots 2-3 stay LIVE.

### MFO-B240 (SEV-3 field-verify, SEV-4 x3) -- heal reach + dead-target channel review (f3a0f97)
Raised against `f3a0f97` (`fix/mfo-heal-reach`, Opus tier-B review, MERGE OK), 2026-10-11. Text as relayed.
- SEV-3 (field-verify): fix 2 releases only the APMF claim (`api->Release`); it does not interrupt the engine caster or clear the [cfc] watch / cast lock, and checks only IsDead() (not DeadTgtReason). The direct road already ends on death (DirectTarget.cpp:335). Confirm on the deck: `[cast] ... target died: channel released` next to `[deadtgt]`.
- SEV-4: a real heal reach of 300u or less is now uncapped (0 already meant no cap); a fallback to vanilla Heal Other reach would be tighter.
- SEV-4: the PickAlly reach-drop latch clears only out of combat inside the drop branch, so it logs about once per follower per session, not per fight. Log only.
- SEV-4: ReleaseOffenseCastOnDeadTarget calls the whole ReleaseOffenseCast; with two per-hand offense claims on different targets, one death drops both. ReleaseCastClaimOnHand would be per hand.

### MFO-B241 (SEV-4 x4) -- rulings 2026-10-11 review (0c1046c)
Raised against `0c1046c` (`fix/mfo-rulings-1011`, Opus review, MERGE OK), 2026-10-11. Text as relayed.
- SEV-4 (item 1): a held heal that ends some other way leaves its approach-stuck entry until fight end, so a later claim on the same recipient is released once and skips the in-flight exemption.
- SEV-4 (item 1): kApproachFloorGiveUp uses the wall clock, so a menu pause counts toward the 5 s (APMF fixed the same issue in its review F2).
- SEV-4 (item 4): healWeapon is not reset by the normal booking path (Loadout.cpp:527) or the already-paid path; a stale flag (two-hander stow keeping the entry) could hand back a later ordinary weapon debt early.
- SEV-4 (item 6): after a fan series ends, the spell stays in hand while the AUTO rule matches; released when no cast rule matches (Scheduler.cpp:1913) or at combat end. Fits the ruling.

### MFO-B243 (SEV-4 x2) -- Harbinger client-mode gates review (162c1dd)
Raised against `162c1dd` (`fix/mfo-gates-client-only`, Opus tier-3 review, MERGE OK), 2026-10-12. Text as relayed by the coordinator.
- SEV-4: `CombatStyle::ServiceOrderClaim` releases the ch.15 claim on a lap where the equip order holds but no weapon is in hand, leaving up to a one-lap gap (spell/staff re-arm not denied) before EquipWeapon's ledger hold is claimed by ReconcileForcedWeapon.
- SEV-4: the client-mode claims (ServiceOrderClaim's ch.15, ServiceCastGuard's ch.25) are refreshed only on the Scheduler laps that reach their call sites; an early-return lap (suppression / ready-beat / down) skips the refresh, leaving the bridge's FacetExpiry sweep (ch.15) or Harbinger's 6 s window (ch.25) as the backstop.
- **Surfaced at edit time from:** MAP.md "Combat vfunc hooks" row.

### MFO-B244 (SEV-4, needs marth's call) -- silver gambit review (102e24c)
Raised against `102e24c` (`feat/mfo-silver-gambit`, Opus review, F5), 2026-10-12. Text as relayed by the coordinator.
- F5: the silver LOOT restock runs even with the economy off (`logistics/LootEquipment.cpp` `ctx.wantSilver` rides act.loot_equipment only; the BUY side is gated on bEconomyBuyGear). Left as is pending marth's ruling on whether "if the economy is on" also gates looting one.
- **Surfaced at edit time from:** MAP.md `logistics/Silver.cpp` entry.

## DRAINED

### Drain batch `b795839` (`fix/mfo-post-2.2.0`, tier B/C items of MFO-B226..B236; the originals stay in the open list for the findings NOT drained)
- **MFO-B227 F5 (SEV-3) DRAINED by `b795839`:** `Loadout::Prepare`'s level-4 right-hand `DeselectSpell` skips a spell (or its proxy) that a RIGHT-hand heal claim names.
- **MFO-B229 F7 (SEV-5) DRAINED by `b795839`:** `EngageOnSight.cpp` says once per session when `SenseOf` fails and the own-ray fallback runs.
- **MFO-B230 F8 (SEV-5) DRAINED by `b795839`:** `EndBuffClaim` leaves the LEFT offense claim alone when the left lock names a different spell.
- **MFO-B231 F5b (SEV-5) DRAINED by `b795839`:** the `[heal-approach]` heartbeat is 5 s (state changes still log at once).
- **MFO-B233 SEV-4 (comment) DRAINED by `b795839`:** the BuffRoad.cpp "hostile spell is never a Buff kind" comment now says flagged only.
- **MFO-B236 F-F, F-H (SEV-5) DRAINED by `b795839`:** the OOC Caster-pick cast logs "self (immediate)"; the Fire.cpp comment is accurate about `a_rangeGate`.

### MFO-B55 — a foe-keyed equip hold still releases through the T#76 dwell during an own-OOC stretch inside a party fight
- **Raised:** Fable tier-B review of `dea438f` (`fix/mfo-party-combat-gate`), SEV-4.
- **Severity:** SEV-4
- **Finding (verbatim):** T#76 dwell releases a foe-keyed equip hold after 2-4 s of own-OOC inside a party fight (kCondFoeWithinRange false because currentCombatTarget is null, not because the foe is far); MAP's "genuinely false, by design" over-claims. Slower than main (0.3-0.8 s), so not a regression; backlog.
- **Reviewer's reasoning:** gate 2 keeps the hold across an own-flag flap, but the equip rule's own condition reads the follower's `currentCombatTarget`, which the engine nulls on a LoS loss; after `MeleeClampDwell` (2-4 s) of that the hysteresis path releases the hold as "condition false" although the foe never left range.
- **Why it was NOT fixed:** strictly slower than `main`'s flap release (0.3-0.8 s), and the correct fix (a foe-range read that survives a null target for a party-combat follower) is an Evaluator change outside the gate branch's `Scheduler.cpp` boundary. MAP/STATUS wording corrected in the closing round.
- **Fix shape when drained (verbatim):** none given; candidate: let `kCondFoeWithinRange` fall back to the party's nearest foe (CombatSense) when the follower's own target is null while `g_partyCombat` holds, or exempt a party-combat lap with a null target from the dwell's "known false" count.
- **FIXED-IN `920a73f`** (`fix/mfo-field-batch-0921`, item F of the 2026-09-21 field batch). Promoted from the backlog by the 2026-09-21 21:50 Deck field diagnosis: Cicero's two mid-fight weapon vanishes (21:57:34.226, 21:57:44.075) were exactly this path (`Scheduler.cpp` `ReconcileForcedWeapon(f, 0, true)` via the MeleeClampDwell with `currentCombatTarget` null while `CombatSense::FoeCount(self) > 0`). Fix shape taken: the second candidate -- a lap on which a target-relative equip rule was evaluated against a null / dead / non-hostile target while the follower's own group still has foes is UNKNOWN, not false (`EquipRangeUndecidable`, Scheduler-only; the Evaluator stays pure); the dwell clock refreshes and `equipCondKnownFalse` stays false. A genuinely empty group or a real party-OOC still releases. Pending its Fable review on the branch.

### MFO-B4 — `IncumbentTargetLost` and `PickAlly` disagree on "resolves"
- **Raised:** Fable review of `1044816`, taken as a comment by `625f3b7`; the review of `625f3b7` asked for a reword once the heartbeat cap exists.
- **Severity:** SEV-5
- **Finding:** `PickAlly`'s "resolves" means "in `Followers::g_active`, or the player"; `IncumbentTargetLost` resolves the raw FormID via `LookupByID`. A mid-fight DISMISSED follower therefore reads valid until healed above threshold.
- **Author's reasoning for leaving the DIVERGENCE:** cost is one claim's life against an ally still standing and still hurt; reaching `g_active` would bind the cast lock to the roster list for a case the field has never reported. Unchanged — the divergence itself is still open by choice, and if it ever bites it comes back as a new entry.
- **Follow-up that was owed:** the comment's stated bound ("a re-aim held for at most one claim's life") had to be reworded once the heartbeat cap landed, because that cap is what makes the real bound something other than the claim's life.
- **DRAINED by `6bfbd79`** (the same commit that landed the cap): the comment now states the actual bound — the re-aim is held while the claim is fed, and the hold's own heartbeat stops at `kHealHoldNeverObservedMs` for a claim that never fires, so the cost is bounded by that window rather than by the fight. Verified in the round-6 review of that SHA.

### MFO-B63 — item 5 of the 2026-09-22 field batch (Jesper's outfit) was DEFERRED, not fixed
- **DRAINED (remainder) 2026-10-01 on `fix/mfo-b63-best-per-slot` (ClickUp 86e3d6ay6), Tier B.** Best-per-slot for NON-mage followers shipped as a SLATE beside the unchanged `ComputeOwnedGearPick` (`GearSlate` / `ComputeOwnedGearSlate`, `EquipAuthority.cpp`): rated ArmorBuySlot 0-3 with the `ArmorIsBetter || worn` subset gate, incumbency, FormID tiebreak, shield never declared, no FitsCarryWeight; the mage road reuses `MageBestPerSlot`. pick -> slate -> everything-else-worn is one ordered acceptance with `SlotsOverlap` exclusion, and `playerPieceWins` covers the pick and every slate entry. OOC road only. NEEDS LOCAL (Deck): a NON-mage follower in rated armor with an outfit, after a cell load (Jesper is a mage and his set shipped in cc4e895). STILL OPEN, its own brief: the shield and hood helmet that VANISH from `GetInventory()` after `OutfitApply` (the `TESContainerChangedEvent` probe).
- **AUDIT 2026-10-01 (`fix/mfo-logic-bundle2`, ClickUp 86e3buxgw):** 3D gate (`EquipAuthority.cpp:508`) and not-worn re-send (`kDeclDriftHold`) are shipped; the mage judge declares its full best-per-slot set via `MageBestPerSlot` (`cc4e895`). STILL OPEN: best-per-slot for NON-mage followers (a restructure of the judge's output, not taken). The roster-sweep load-screen drop (kMissesBeforeDrop) is DRAINED on this branch.
- **Raised:** author of `fix/mfo-spell-authority-0922`, 2026-09-22, under the brief's own instruction ("include only if it does not risk the round ... If this makes the round too large, STOP and report it as deferred rather than half-doing it").
- **Severity:** SEV-2 (a follower stood visibly bare for 2 min 36 s in the field, so this is NOT a cosmetic backlog item)
- **Finding (verbatim, from the brief):** "The declaration is 'one judged pick + everything else worn' (`Logistics_Economy.cpp:835,930-937`), so a piece the engine's outfit-apply displaced falls out of the set; the re-declare went into a 3D-absent actor (APMF logged 'equip pass skipped, re-declare once loaded' and MFO never re-sent); then SEND-ONLY-ON-CHANGE (`:950`) swallowed 7 correct picks and the follower stood bare for 2 min 36 s. Fix: declare best-per-slot from the judge rather than pick+worn; treat 'a declared piece is not worn at the next service and nothing is in flight' as a change (erase the last-declared cache and re-send, MFO-B41's shape generalised to armor); skip declaring while `!Is3DLoaded()` and mark dirty so the first loaded tick re-sends."
- **Wave-2 location (2026-09-25, `native/logistics/`):** `RefreshEquipDeclaration` is `native/logistics/EquipAuthority.cpp:391` (the judged-pick + worn-set assembly follows it).
- **Why it was NOT fixed:** it is three independent changes to `RefreshEquipDeclaration` (a new best-per-slot judge output, a worn-set drift detector with its own in-flight notion, and a 3D gate with a dirty flag), in the same function this round already touched for the shield sale, on a round with NO full Fable review. Half of it would ship a declaration that is neither pick+worn nor best-per-slot. The engine-side trigger is also still open: the `08:25:10` `OutfitApply` made the shield and the hood helmet VANISH from `GetInventory` while APMF still listed the shield, and `field-diag-2026-09-22` records that as UNRESOLVED pending a `TESContainerChangedEvent` probe.
- **Fix shape when drained (verbatim):** as the brief states it above. Take the `TESContainerChangedEvent` probe first, because "a declared piece is not worn" and "a declared piece is not in the inventory at all" want different answers.
- **PARTLY DRAINED 2026-09-22 on `fix/mfo-declaration-hygiene-b63` (off `main` `e522f61` = v2.0.10), after marth confirmed it still happening on v2.0.10. Two of the three parts SHIPPED; best-per-slot is STILL OPEN (below).** No Fable review on the round (LOW TOKEN WEEK, marth's order): coordinator diff read plus green CI.
  - **SHIPPED — "not worn is a change".** At the SEND-ONLY-ON-CHANGE compare in `RefreshEquipDeclaration`, a set that matches the last one is still re-sent when any declared ARMO the follower still owns is not on his body. Enforced mode only (`IsEquipAuthorityEnforced`; in observe mode declared-and-not-worn is the designed state). Throttled to `kDeclDriftHold` = 3 s via `g_lastDeclSentAt`, which is ALSO the "nothing is in flight" test, because APMF exposes no "the pass ran" query. `MFO-B41`'s weapon-ledger detector generalised to armor, on the same hold. Non-ARMO keys and forms he no longer owns are skipped.
  - **SHIPPED — no declaration while `!Is3DLoaded()`.** The send is skipped and `g_lastDeclared` + `g_lastDeclSentAt` are dropped, which IS the dirty mark, so the first loaded tick sends the rebuild as a change. The standing claim is kept.
  - **Probe now ADDED 2026-10-01 (`fix/mfo-equip-authority-small`): `InvProbe` in `logistics/Sinks.cpp` logs `[inv-probe]` for an ARMO/WEAP leaving a managed follower; its result NEEDS LOCAL (Deck run).** The shield and hood helmet VANISHING from `GetInventory()` after the `08:25:10` `OutfitApply` is otherwise untouched. The re-send is written so it cannot make that worse: a form he no longer owns is skipped by the drift test, so a vanished piece never drives a re-send loop.
- **STILL OPEN — PART 1, best-per-slot instead of `pick + everything else worn`.** Severity in SYMPTOM drops from SEV-2 to SEV-3: it is no longer the bare-body window (parts 2+3 close that), it is "the follower is wearing the wrong thing" — the engine's outfit piece is adopted into MFO's set BECAUSE it is worn, so APMF has no reason to refuse it. **Cut from the 2026-09-22 round by marth** ("Seems like a ton of changes considering the scope"): it is a restructure of the judge's OUTPUT, not a hygiene fix, and it deserves its own brief and its own field cycle.
  - It was fully designed and fully written before the cut, then backed out (never pushed). **FULL WORKING NOTES: ClickUp `86e3d6ay6`** — the judge's real output shape, the file:line map of pick-vs-assembly, the concrete plan with signatures and callers, the cache/ordering interactions, and the dead ends. Read that before starting; it is a ready-to-dispatch brief.
  - The one fact to know up front, because it is what makes this harder than it looks: `ArmorIsBetter` judges per BIPED BIT while `ArmorBuySlot` / `MageClothingSlot` / `WornInLogicalSlot` are LOGICAL slots, and the two DISAGREE. See MAP.md beside "THE WEAR DECISION".

### MFO-B64 — `CoLoadForcedWeapons` leaves a loaded follower empty-handed
- **Raised:** author of `fix/mfo-spell-authority-0922`, 2026-09-22, during the item-4 per-site unequip audit.
- **Severity:** SEV-4
- **Finding (verbatim):** `CoLoadForcedWeapons` (`Actuation.cpp:~2876-2891`) force-unequips the persisted held weapon (or, for an unresolvable `0xFF` weapon, whatever weapon is in either hand) to clear the stale prevent-removal lock the `.ess` carried. That is correct and must stay, but it is also a stand-down in every sense marth's "nobody ever sees an unarmed follower" rule cares about: after a load with an `FWPN` record the follower carries nothing on his body until the next fight arms him.
- **Why it was NOT fixed:** the same `MainThread::Post` body states the actor "may not be 3D-loaded yet", and an EQUIP into a 3D-absent actor is the `#62` invisible-head class. Re-arming there needs a 3D-loaded gate and a retry road that does not exist yet, which is new machinery inside a co-save load path -- one of the five save-corrupting areas. Out of scope for a round with no full Fable review.
- **Fix shape when drained (verbatim):** re-arm non-forced + sheathe as `ReleaseForcedWeapon(a_standDown = true)` does, but only once `Is3DLoaded()`, with the follower marked dirty for the first loaded tick rather than equipped blind on the load frame.

### MFO-B65 — the ch.8 allow-list is enumerated, and an enumeration can be incomplete
- **Raised:** author of `fix/mfo-spell-authority-0922`, 2026-09-22.
- **Severity:** SEV-3 (note, with a live watch item for the next field cycle)
- **Finding (verbatim):** APMF's `Allowed()` is a pure FormID-set test, so MFO cannot express "normal spells only" -- the exemption `CasterConsent::CtrlUnlatchedDeny` gets for free from `Is(FormType::Spell)` + `GetSpellType() == kSpell` has to be reproduced by ENUMERATING the exempt forms (`AppendDenyExemptForms`). Carried staves + their `formEnchanting`, carried scrolls, and `kPower`/`kLesserPower`/`kVoicePower` are enumerated. `kAbility` deliberately is not. Anything reaching APMF's 30 hooked `CombatInventoryItemMagicT` templates that is NONE of those and is not a gambit spell will be DENIED at the equip seat, where MFO's own deny would have let it through.
- **Why it was NOT fixed:** there is nothing to fix without evidence of a missing class. The failure mode is bounded and observable rather than silent: the `[t2a]`/`[t2c]` deny lines name the actor and the exact form, and the `[cast-select] ... allow-list CLAIMED n=N [...]` line prints every form MFO allowed, so one field log identifies any missing class by subtraction. `bApmfSpellAllowList=0` turns the whole gate off.
- **Fix shape when drained (verbatim):** read the first field log's `[t2c] ... ch.8 select DENY` lines against that follower's `[cast-select]` allow-list line. Any denied form that is not a spell the player declined to gambit is a missing exempt class: add it to `AppendDenyExemptForms`. Also re-check the 32-form budget -- an oversized list is refused outright (`the gate is NOT claimed`), which is safe but silently returns the follower to the pre-fix behaviour, and that error line is the only signal.
- **UPDATED 2026-09-22 after the Fable spot check -- NOT CLOSED, and the reason matters.** The spot check bounded most of this entry and named ONE real hole, which is now guarded in code. Kept open because the *question* is still not statically answerable and the guard introduced a new bounded cost.
  - **Bounded by the spot check (no longer open):** mod scroll variants are covered by `As<ScrollItem>()`; staff enchantments are covered; shout words are covered by `kVoicePower`; **non-staff weapon enchantments cannot surface at all** (they are applied on hit, never cast through a hooked caster); and the `kAbility` exclusion is **safe in the direction that matters**.
  - **THE ONE NAMED HOLE, verbatim from the spot check:** *"it is NOT DETERMINABLE whether the AI's combat potion use routes an `AlchemyItem` through `CheckCast`. If it does, the allow-list would DENY a follower's health potions."*
  - **GUARDED, and it was never hypothetical.** `Docs/ENGINE_NOTES.md:1759-1762` records the identical own-goal ALREADY SHIPPING: *"The deny was suppressing combat potions. `CombatMagicCasterRestore` is also the drink-potion caster (§0.29 scope fact, now load-bearing): the unconditional !isWanted deny covered a latched follower's own combat drinking. v1.0.32 denies only `formType == Spell`."* Since APMF's allowance cannot be told "spells only", `AppendDenyExemptForms` now enumerates every carried `AlchemyItem` that is **not food and not poison**. `IsMedicine()` (vfunc 62) was deliberately rejected as the filter: it is an authored flag a mod potion can lack, and a missing flag would deny someone's healing potion. So a potion can no longer be denied by this gate.
  - **THE NEW COST, stated rather than discovered later:** the potion exemption spends the 32-form budget. A follower with a large alchemy hoard can push his list past `kMaxSpellAllowList` and **lose the candidate-refusal gate entirely** -- fail-open, an `[cast-select] ... the gate is NOT claimed` error naming the gambit and potion counts, never a mute. That trade is deliberate: an inert gate is far better than a denied healing potion. It does mean the headline fix can be silently absent for a hoarder, which is why this entry stays open.
- **UPDATED 2026-09-27 (batch L, `fix/mfo-batchL-field`), BY MARTH'S CALL:** step (2) was answered in the field
  (Serana 02002B74: 38-50 forms, 35-46 of them potions, gate NOT claimed) and marth ruled "keep the best available
  potion in each category within the 32 cap", with NO `kMaxSpellAllowList` raise. This overrides the "NOT
  truncation on MFO's side" line below FOR POTIONS ONLY: `apmf/SpellAllowList.cpp` `SelectPotions` keeps every
  potion when they fit, else each category's best (costliest effect's archetype + primary AV; magnitude, then
  duration, then value) round-robin within the budget the non-potion forms leave. Non-potion forms are still never
  truncated (overflow still fails open). Watch in the next field log: the `trimmed to the best of each category`
  line, and no `[t2c] ... ch.8 select DENY` on a kept potion.
- **Fix shape when drained (verbatim, REVISED 2026-09-22):** from the first field log, (1) confirm NO `[t2c] ... ch.8 select DENY` names an ALCH form while a follower is low on health with potions -- if one appears, the potion enumeration is reaching the wrong set and must be widened before anything else; (2) count how often `the gate is NOT claimed` fires and on whom -- if ordinary followers are overflowing 32, the answer is an APMF-side `kMaxSpellAllowList` raise (its own brief, byte-shared header, tier A), NOT truncation on MFO's side; (3) only then read the remaining `DENY` lines against the `[cast-select]` allow-list line for a missing exempt class.

### Closed by triage 2026-10-06 (moved from OPEN / PENDING; each text kept verbatim, with its CLOSED line)

### MFO-B18 — `WeaponHandExposure` steers every cast LEFT while a force-hold exists, so a dual-wielder's left weapon yields and refills at cast cadence (DECIDED BY MARTH: by design)
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-4. F6.
- **Severity:** SEV-4 (design)
- **Finding (verbatim):** F6 — SEV-4 design, marth decides (record only): WeaponHandExposure (Actuation_Hands.cpp:163-172, consumer :872 preferRight = !exposure) steers every cast to the LEFT while any force-hold exists, and heals are left-only, so each cast on a dual-wield follower yields the left weapon and the top-up re-equips it once the lock lapses — weapon↔spell churn at cast cadence on the hand the CSTY change just filled.
- **Reviewer's reasoning:** a design consequence of two standing rules (right-first-unless-a-weapon-owns-the-right, heals left-only) meeting the new left hold, not a defect in either; the churn is bounded by the top-up floor (5 s after every yield, F1 fix `1ac3c6b`).
- **Decision (marth 2026-09-14, verbatim):** "for F6 the weapon should always yield to a spell on left hand." DECIDED, not open: a force-held left weapon ALWAYS yields to any spell that needs the left hand (heals included); the weapon<->spell churn at cast cadence is accepted by design. No right-hand cast preference and no "dual-wielder does not cast" rule may be added for this. Recorded here so the reasoning survives; nothing to drain.
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks + `Docs/STATUS.md` DECIDED item.

- CLOSED 2026-10-06 (triage): OBSOLETE -- marth decided it by design on 2026-09-14 (the entry's own Decision line, "for F6 the weapon should always yield to a spell on left hand"; ClickUp 86e39411y complete). Nothing to drain.

### MFO-B41 — a displaced hand with an unchanged ledger is never re-equipped under the authority; the equip rule re-Fires every lap
- **Raised:** Fable tier-3 review of `e0b52b5` (`feat/mfo-equip-authority`), SEV-4 (F-B).
- **Severity:** SEV-4
- **Finding (verbatim):** F-B (a displaced hand with an unchanged ledger is never re-equipped under the authority; equip rule re-Fires every lap — fix shape: erase g_lastDeclared[id] before DeclareFromLedger when the ledger's weapon is in neither hand and the ledger did not change).
- **Reviewer's reasoning:** under the authority MFO makes no equip call; the declaration is sent only on change (F3). If something un-seated (a script, an observe-mode engine take-back) removes the ledger's weapon from the hand, the ledger is unchanged, the next lap's `EquipWeapon` Fires again (the hand no longer holds the category), writes the same ledger, and `DeclareFromLedger` compares equal and sends nothing — APMF never re-equips, and the rule Fires every lap.
- **Why it was NOT fixed:** below the severity floor; the cycle ended at nothing above SEV-3; observe mode is where it shows and enforcement (the seat refusing the take-back) is what stops the common cause.
- **Fix shape when drained (verbatim):** erase `g_lastDeclared[id]` before `DeclareFromLedger` when the ledger's weapon is in neither hand and the ledger did not change.

- CLOSED 2026-10-06 (triage): OBSOLETE -- fixed in code, only the backlog text was stale: `cast/Equip.cpp:342-380` (`kB41Hold`, `g_lastB41At`) calls `Logistics::ResendEquipDeclaration` when the ledger's weapon is in neither hand and logs "re-declaring (MFO-B41)"; MAP.md section 2 records it as "MFO-B41 fixed". (The fix shape written here, erase the last-declared entry, is what `ResendEquipDeclaration` does at `logistics/EquipAuthority.cpp:509`.)

### MFO-B45 — scope and set are two enqueues; a Drain can land between them
- **Raised:** Fable tier-B review of `7857446` (`feat/mfo-equip-authority-v9`), SEV-5 (F3).
- **Severity:** SEV-5
- **Finding (verbatim):** F3 SEV-5 scope and set are two enqueues, a Drain can land between them (one-frame self-healing mismatch; the comments at Logistics_Economy.cpp:933-937 / APMFBridge.cpp:1685-1688 / APMFBridge.h:588-589 overstate "same Drain" — soften them).
- **Wave-2 location (2026-09-25, `native/logistics/`):** the declaration builder is `RefreshEquipDeclaration`, `native/logistics/EquipAuthority.cpp:391`.
- **Reviewer's reasoning:** `DeclareEquipScope` and `DeclareEquipSet` are two thread-safe enqueues from the worker; APMF's per-frame Drain on the game thread can run between them, so for one frame the seat pairs the NEW scope with the OLD set. The order is fixed (scope first), so the reverse (a stale scope over a new set) cannot occur, and the next Drain heals it.
- **Why it was NOT fixed:** below the floor; a one-frame window with a fixed, safe order. The three comments were softened in the same round (they no longer claim "same Drain" as a guarantee).
- **Fix shape when drained (verbatim):** none needed beyond the comment fix; an atomic scope+set call would be an APMF ABI change.

- CLOSED 2026-10-06 (triage): OBSOLETE -- the only fix this entry asked for (soften the "same Drain" comments) is in: `apmf/Equip.cpp:211` now says a Drain landing between the two enqueues pairs the NEW scope with the OLD set, and `logistics/EquipAuthority.cpp:1055` / `apmf/APMFBridge.h:900` say "normally". An atomic scope+set call would be an APMF ABI change the entry itself rules out.

### MFO-B135 (SEV-3) -- no decline-fallback: the refused-claim equip outcome is read as a satisfied equip
Raised against 56c032b (`fix/mfo-museum-priority`, tier-B review round 2), 2026-09-28. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-museum-priority.md`, round 2, item (a)): "refused-claim NoOp is Result::NoOp+transparent -> Scheduler reads it as SATISFIED equip -> wantStance/handClaim -> CombatStyle stance + MFO equip gate denies AI spell/staff re-arm (SEV-3, fix: FailedOther transparent)." Reasoning: `Scheduler.cpp` `satisfiedEquip = isEquip && outcome.result == NoOp` on a transparent outcome, so `cast/Equip.cpp EquipWeapon`'s "equip authority refused (MFO leaves the hands to the engine)" NoOp arms the H2 hand claim, the stance and MFO's own equip gate -- MFO is not fully staying out. **NOT FIXED, by marth's call (2026-09-28): "MFO is billed as requiring Harbinger, so we don't need to worry about it." MFO requires Harbinger; seat-not-installed is unsupported.** Fix shape if it is ever supported: return `Result::FailedOther` + transparent for the refused claim (at `EquipWeapon`'s top), which the Scheduler treats as not satisfied and not an activity. Surfaced at edit time from: MAP.md §7 apmf/ refused-claim paragraph.

- CLOSED 2026-10-06 (triage): OBSOLETE -- decided by marth 2026-09-28 (the entry's own text): "MFO is billed as requiring Harbinger, so we don't need to worry about it"; seat-not-installed is unsupported. Not fixed by decision.

### MFO-B141 (SEV-4, recorded -- consistent with marth's intent) -- "Foe attacking me: ranged" widened with "Foe is ranged"
Raised against 73119b8 (`fix/mfo-field-0928c`, tier-B review), 2026-09-28. Reviewer's finding (verbatim): "'Foe attacking me: ranged' (kCondFoeAttackingMeRanged, Evaluator.cpp:340) calls the widened FoeIsRanged, so its meaning changes too: it now fires for a foe targeting him with a staff or any Aimed / Target Actor / Target Location spell in either hand (every hostile mage, Vampiric Drain, a Falmer shaman). The brief named only 'Foe is ranged'. 'Foe attacking me: melee' is unchanged (it already excluded casters and staves). No heal-gambit gate reads FoeIsRanged; the Scheduler's sizing-up exemption is by opcode, unaffected." **Recorded, not a defect:** marth 2026-09-28: "ranged is a concept, spellcaster is specific", so the widened "Foe attacking me: ranged" is consistent with his intent. Surfaced at edit time from: MAP.md §Vocabulary.h (the ranged / mechanical note).

- CLOSED 2026-10-06 (triage): OBSOLETE -- recorded as not a defect by marth 2026-09-28 ("ranged is a concept, spellcaster is specific"); the widened "Foe attacking me: ranged" is the intended meaning.

### MFO-B151 (SEV-5, moot) -- can-act: TouchReach read GameSettingCollection on the job worker
Raised against 1b3af00 (same review), 2026-09-29. Reviewer's finding (verbatim, C8): "TouchReach reads GameSettingCollection::GetSetting on the job worker: a lookup in a map that is fixed after the data load (a console setgs is the only writer). Cached in an atomic, re-armed on revert. Safe." **Moot on the branch:** marth's option B (2026-09-29) replaced the touch reach with Heal Other's reach, so the GMST read and `TouchReach` are gone. Nothing to drain. Surfaced at edit time from: MAP.md §2 cast/ "CAN-ACT".

- CLOSED 2026-10-06 (triage): OBSOLETE -- moot on its own branch: marth's option B (2026-09-29) replaced the touch reach with Heal Other's reach, so the GMST read is gone (`grep TouchReach native/` has 0 hits).

### MFO-B153 (SEV-4, DECIDED -- not a defect) -- can-act: the auto-retreat stays ungated for a downed follower
Raised against 1b3af00 (same review), 2026-09-29. Reviewer's finding (verbatim, C6): "Auto-retreat is left ungated: a bleeding-out follower can still be given a retreat fill (a travel package on an actor that cannot move). EngageOnSight already skips bleedout (EngageOnSight.cpp:236). Recommend gating the retreat fill on CanAct; it is outside the brief's cast/drink scope, so marth's call." **Decided by marth 2026-09-29: leave it ungated** ("retreat can stay if they are still downed and crawling during retreat"). Recorded so nobody gates it. Code: `Scheduler.cpp` `ServiceRetreat` and the auto-retreat fill. Surfaced at edit time from: MAP.md §2 cast/ "CAN-ACT".

- CLOSED 2026-10-06 (triage): OBSOLETE -- decided by marth 2026-09-29 (the entry's own text): "retreat can stay if they are still downed and crawling during retreat". The auto-retreat stays ungated by design.

### MFO-B168 (SEV-4) -- Foe: Multiple within: the rule can pick a fringe member, not the centre
Raised against 1571d55 (`feat/mfo-foe-multiple`, tier-B review), 2026-09-30. Reviewer's finding (verbatim): "Selection happens inside the normal scan, so the chase cap (#22), 3D gate, brawl gate, reach gate and LoS preference all apply to the chosen foe. Scenario: centre C has 4 others within X but is Occluded (or beyond chaseCap); fringe member F has 2 others and is Visible and in the cap. `bestVis` = F, so the rule targets F, not C. Judgement: acceptable, not a defect against the brief. F still satisfies the rule (>=2 others within X), so an AoE of radius >= X centred on F still hits >=3. The forced cast needs sight anyway, so an occluded C would just be refused at Actuation. The chase cap refusing a far cluster matches every other selector. MAP.md documents it. The CHANGELOG does not, so the player-facing text says a little more than the code does." Reviewer's reasoning: acceptable by design. CHANGELOG wording fixed in the chore commit (see MFO-B170); the behaviour is unchanged. Code: `Evaluator.cpp` `PickFoe` cluster pre-pass + scan.

- CLOSED 2026-10-06 (triage): OBSOLETE -- accepted as by-design by the Opus tier-B reviewer ("acceptable, not a defect against the brief": the fringe member still satisfies the rule); the player-facing CHANGELOG wording was fixed (B170). Nothing to drain.

### MFO-B171 (SEV-3, DECIDED -- not a defect) -- animheal-p2: lowest-first re-aims only in the idle gap between casts (review F4)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review), 2026-09-30. Reviewer's finding (verbatim, abridged to its claim): "`mine` = `IsOwnRetarget && !CastInFlightOnHand && (IncumbentTargetLost || IncumbentHealCastDone)`. The re-aim is allowed only on a lap that lands in the engine's idle gap between two casts at the incumbent. ... While the incumbent keeps charging, `refreshHeldOwnClaim` feeds its claim with no cap (`inFlight` exempts it), and `IncumbentTargetLost` (full / out of reach) is also gated on `!inFlight`. So ally A can keep being healed ... while the lowest ally B waits. ... The design question is marth's: D8 says 'finish the current cast'. Does a charge that STARTED after the incumbent's fire count as the 'current cast'?" Reviewer's reasoning: unproven either way; the F2 fix may close the gap. **DECIDED (marth 2026-09-30): keep it.** "It's fine. Otherwise there'd be constant switching." A heal re-aims only when the hand is idle between casts; no charge-boundary re-aim for a same-rule recipient change. Code: `cast/Hands.cpp` `ResolveCastHand`'s `mine`, `IncumbentHealCastDone`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".

- CLOSED 2026-10-06 (triage): OBSOLETE -- decided by marth 2026-09-30 (the entry's own text): "It's fine. Otherwise there'd be constant switching." A heal re-aims only in the idle gap between casts, by design.

### MFO-B184 (SEV-5, CLOSED by design) -- animheal-p2: End inside the 3 s grace leaves the spell in hand (round 2 shadow R2-9)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, shadow reviewer), 2026-09-30. Reviewer's finding (verbatim): "`End()` -> `ReleaseSpellIf` -> `ReleaseSpell` honours the 3 s grace (`Loadout.cpp:~455`), so a claim ended <3 s after equip leaves the spell in hand and `g_mfoSpell` set; the next `!castSeen` lap (`Scheduler.cpp:1690`) takes it. Cosmetic." Code: `Loadout.cpp` `ReleaseSpell` grace, `ComposedCast.cpp` `End`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
CLOSED as by-design (coordinator ruling after the Opus review): the 3 s MINIMUM HOLD grace is working as designed; a later `!castSeen` lap takes the spell.

- CLOSED 2026-10-06 (triage): OBSOLETE -- closed as by-design in the entry itself (coordinator ruling after the Opus review: the 3 s minimum-hold grace works as designed); cosmetic only.

### MFO-B235 -- Idle-hand floor on Harbinger ABI 19 (apmf/Bridge.cpp): deferred review finding F7
Raised against `db9790c` (`fix/mfo-floor-spells-only`, Opus tier-3 review, nothing above SEV-3), 2026-10-06.
- F7 (SEV-5, accepted): "The ABI-19 no-floor fallback also drops floors on ABI-19 dev builds d8bb76d..46f4ce1, which don't have HandBlock. No release has ABI 19, so this is accepted." Reasoning: MFO cannot tell a v19 build with the hand-claim block from one without it, and only unreleased dev builds report v19. On those pre-HandBlock builds the AI may start its own second spell in the idle hand while MFO drives the other.

- CLOSED 2026-10-06 (triage): OBSOLETE -- accepted in the entry itself: the ABI-19 no-floor fallback only affects unreleased dev builds d8bb76d..46f4ce1, no release carries ABI 19 (`apmf/Bridge.cpp:54` `kHandBlockFirstAbi=19`, `:739` handles v19 vs the spells-only v20), and marth's spells-only floor ruling replaced the hand-block floor.

### MFO-B38 — "one judge" holds for the RATED armor branch only; mage-mode may stock a piece legacy wore
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-4 (F11).
- **Severity:** SEV-4
- **Finding (verbatim):** F11 (one-judge holds for the rated branch only; mage-mode may stock a piece legacy wore).
- **Reviewer's reasoning:** `ComputeOwnedGearPick` is EquipBestOwnedGear's pick verbatim, but the mage-apparel branch's equip carried a thrash guard (OOC-only + 5 s per follower) that the declaration does not: the declaration names the pick every tick it changes, while the legacy equip would have waited. A piece the mage branch would have worn after its guard can be declared (and worn by APMF) earlier, or — with the pick replacing the overlapping worn clothing in the set — the worn piece drops to stock a tick before legacy would have swapped it.
- **Why it was NOT fixed:** below the severity floor; the declaration's own change gate bounds it to one send per real change, and the field cycle is observe-only.
- **Fix shape when drained (verbatim):** carry the mage branch's thrash guard into the declaration (only declare the mage pick when EquipBestOwnedGear's guard would have let it through), or drop the guard from legacy so both roads agree.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- mismatch between the legacy mage-apparel equip's thrash guard and the SetEquipSet declaration (`logistics/EquipAuthority.cpp` `RefreshEquipDeclaration`); the rework retires the deny+declaration road ("most of the declaration path") and biases the engine's own equip scoring instead.

### MFO-B136 (SEV-4) -- mage apparel set: a piece that can never stay worn is re-sent every 3 s
Raised against cc4e895 (`fix/mfo-museum-priority`, narrow final check), 2026-09-28. Reviewer's finding (verbatim from `agentlogs/review-mfo-museum-priority.md`, narrow check, (1)): "a piece that can never stay worn loops. If a script unequips it or the engine keeps stripping it, the MFO-B63 check re-sends every 3 s indefinitely. Main's single pick had the same behaviour; the set extends it to up to 6 slots at once. For Jesper this means strip, then re-dress every ~5 s, which is the \"a strip must not change the set\" intent." Reasoning: `MageBestPerSlot` declares by ownership, not by what is worn, so the MFO-B63 drift re-send re-dresses any stripped set piece. Pre-existing shape, widened. Surfaced at edit time from: MAP.md §7 equip authority.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the mage-set 3 s re-send loop is the declaration road's not-worn drift re-send (`kDeclDriftHold`, `RefreshEquipDeclaration`); the rework retires the declaration path.

### MFO-B144 (SEV-5) -- a failed declaration send can still record a flipped-back relic as a player pick
Raised against 73119b8 (`fix/mfo-field-0928c`, tier-B review), 2026-09-28. Reviewer's finding (verbatim): "g_lastWithheld is rewritten every build but g_lastDeclared only on a successful send: if the send that first declares the flipped-back relic fails (DeclareEquipScope/Set false -> g_lastDeclared erased), the next build has neither lastHas(f) nor g_lastWithheld[f], and 4b records the relic as a player pick." Reasoning: needs a failed APMF send on exactly that build. Fix shape: keep the previous g_lastWithheld entry until a send succeeds (update it beside g_lastDeclared). Surfaced at edit time from: MAP.md §4 equip-authority (the museum relics / declaration entry).

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the `g_lastWithheld` / `g_lastDeclared` bookkeeping around a failed declaration send is part of the declaration road the rework retires.

### MFO-B211 — the mage set's whole-set-worn gate is too strict
- **Raised:** Opus review of `fix/mfo-b63-best-per-slot` @8e8c132, 2026-10-01. **Severity:** SEV-4. Raised against the fix commit that confines the gate to the mage road, so the remaining scope is MAGE-ONLY.
- **Finding:** the player-pick recording gate (`slateLanded`, formerly `mageSetLanded`) needs EVERY set piece worn. One never-wearable set piece blocks player-pick recording and, with the not-worn drift re-send (`kDeclDriftHold`), re-sends every 3 s forever.
- **Suggested fix:** gate per slot instead of for the whole set. Cross-ref MFO-B136 (mage set re-send). Surfaced at edit time from MAP.md section 7 equip authority.
- **Note:** other open branches have used MFO-B numbers up to B210, so these two start at B211.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- the mage set's whole-set-worn gate (`slateLanded`) and the drift re-send it feeds belong to the declaration road the rework retires.

### MFO-B212 — the mage road never records a player pick in a set slot
- **Raised:** Opus review of `fix/mfo-b63-best-per-slot` @8e8c132, 2026-10-01. **Severity:** SEV-4. Pre-existing since `cc4e895`.
- **Finding:** a player piece worn in a mage set slot is either the set's best (so 4b skips it as a set piece) or leaves the set best unworn (so the whole-set-worn gate blocks recording). Either way it never reaches `g_playerPicks`, and a later upgrade can sell it or ship it to the museum.
- **Suggested fix:** record per slot, with the same per-slot gate as MFO-B211.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3jwmk7 -- player-pick recording in a mage set slot (`g_playerPicks`, per-slot gate) belongs to the declaration road the rework retires; the per-slot gate it asks for goes with MFO-B211.

- **MFO-B75 (SEV-4, raised against dd4c56f, Opus 5.5 review of fix/mfo-summon-no-fanout).** "Script-archetype mod summons (OnEffectStart PlaceAtMe) do not match kSummonCreature and still fan out under AUTO; CasterHasLiveSummon cannot see them either." Reasoning: the cast-once rule in `CastAuto` keys on the kSummonCreature archetype only, so a mod summon built as a Script effect is still treated as an ordinary beneficial spell (one cast per party member) and its creature is invisible to the liveness guard.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3dmtr1 -- Script-archetype mod summons that still fan out are one of the complex-spell classes that task researches and fixes (its P2 lists "Script-archetype summons (MFO-B75)" under the open decisions).

- **MFO-B77 (evidence gap, raised against dd4c56f).** "Serana's raise spells are Reanimate archetype and still fan out under AUTO (CastSpellImmediate(reanimate, living ally)); confirm which spell froze before calling the freeze fixed." Reasoning: the fix covers kSummonCreature only; if Serana's freezing spell is a Reanimate spell, AUTO still casts it once per living party member and the freeze lead is untouched.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3dmtr1 -- Serana's Reanimate-archetype spells and whether they still fan out are the Reanimate row of that task's P1 census and P2 fix (its P2 names "Reanimate target (MFO-B84/B77)").

### MFO-B84 (SEV-4) -- Reanimate still takes the old cast roads; its liveness read looks at the wrong actor
Raised against 78f9631 (fix/mfo-summon-oneshot), Opus 5.5 tier-B review 2026-09-24. Verbatim: "Reanimate still takes the old roads and CasterHasLiveSummon's ReanimateEffect match reads the caster's list though the effect sits on the corpse." Reasoning: `IsSummonSpell` matches kSummonCreature only, so a raise-dead gambit still goes through CastOn / CastSelfDirect / CastAuto (a `cast_self` raise-dead row can hit the same self-stream stale-dispel loop the summon fix removed), and `CasterHasLiveSummon`'s `ReanimateEffect` branch scans the CASTER's active effects while the engine puts that effect on the reanimated corpse, so it likely never sees a live thrall. Relates to MFO-B77. (Numbered B83 on the branch before the merge with main, whose B83 is the F1b items.) The CasterHasLiveSummon calls left on the worker for this path (Actuation.cpp Fire guard, Logistics.cpp OOC guard) are the pre-existing off-thread active-effect read the review flagged; summons no longer use them.

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3dmtr1 -- Reanimate on the old cast roads and its caster-side liveness read are the Reanimate fix in that task's P2 ("Reanimate target (MFO-B84/B77)").

### MFO-B191 (SEV-4) -- OOC animated heal = combat substrate, future work
Raised against 3a42371 (`fix/mfo-heal-open-items`, tier-A review, Opus, finding F2), 2026-09-30. Reviewer's finding (verbatim from the coordinator's relay): "After B173, out-of-combat heals through CastSelfDirect/CastTargetDirect no longer call ChooseHealRoad. A follower that still has a CombatController while the party is out of combat therefore heals on the instant road with NO [heal] line." marth's ruling (2026-09-30): "nothing will use the instant road in the finished build (aside from where 1.5 needs it) ... any causes of fallback usage are problems to fix." **Labelled in `fix/mfo-heal-field0930`:** `ComposedCast::NoteOocDirectHeal` (one `[heal]` line per follower and spell per 15 s, called by both Direct roads when `g_firingRule == kNoRule`). **Still open:** the out-of-combat heal stays on the instant road. An animated OOC heal needs the combat substrate (force the state as a substrate, portal one facet, deny the rest; CLAUDE.md principles 1-2), which is future work. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".

- CLOSED 2026-10-06 (triage): SUPERSEDED by ClickUp 86e3f1nf5 -- the animated out-of-combat heal needs the combat substrate (force a CombatController with no real foe, deny the other facets), which is exactly that task; today's OOC heals stay on the instant road and are labelled by `ComposedCast::NoteOocDirectHeal`.

---

## PENDING FABLE REVIEW — shipped unreviewed, DEFERRED not waived

marth 2026-09-09, under the low-token order: **"the diffs will be fable reviewed
when tokens become available. They are defferred not removed."** Rule 8 is
suspended BY MARTH for these commits, not by the coordinator, and each one
SHIPPED TO USERS before review. Review them when budget allows, oldest first.

| SHA | what | shipped in | risk |
|---|---|---|---|
| `0b8c816` | widen the board's control mask from 9 to all 12 `USER_EVENT_FLAG` categories | v2.0.3 | low - one constant, restores v1.1.4 semantics. **Did NOT fix the reported symptom.** |
| `6d3e9c5` | `storeState=false` so the overlay stops writing the engine's SAVED control state at +0x124 | v2.0.3 | medium - reasoned from disassembly, never proven in game. **Did NOT fix the reported symptom.** |
| `49c9d1b` | restore the v1.1.4 `InputDispatchHook` call-site trampoline, gated to 1.6.1170 | v2.0.4 | **HIGHEST - review this first.** Patches 5 live bytes in a function on a dedicated input thread. Its one risky line is `*a_events = nullptr`; the author verified `rdx` is a stack slot at that call site so the swallow is scoped to one dispatch, and that `+0x7B` is a whole E8 rel32 call. Both claims want independent confirmation. |

| `8b95800` | `fix/mfo-heal-recognition` — the F12 in-flight heal hold in `ComposedCast::Try` + the combat-AI gate on `CastTargetDirect`'s claim + `CastInFlightOnHand`/`kHandLeft` promoted to `Actuation.h` | v2.0.5 | **HIGH — tier 3.** Changes WHO wins the heal claim slot and adds a hold with a new bound, and moves a predicate out of an anonymous namespace into a public header. marth ordered it built with NO review under the token limit. The three uncertainties the author flagged are in the branch report: (1) an OBSERVED, continuously channelling incumbent is now displaceable only after `kInFlightHoldCap` (6 s) rather than immediately — bounded but a real behaviour change; (2) `CastSelfDirect` (`Actuation_Direct.cpp:886`) and the OOC offense-concentration `ClaimOffenseCast` (`:1216`) have the IDENTICAL undeliverable-claim exposure and were deliberately left alone as outside the brief; (3) a heal claim left standing when combat ends is not released by the gate, so `Logistics.cpp`'s `IsHealCastActive` label can read "APMF claimed" for up to one 6 s TTL while the direct force is what actually delivered. |

Merges `6d8074d` and the docs-only `b8087a9` (CLAUDE.md rule 11) carry no
unreviewed logic.

**Note for whoever reviews `0b8c816` / `6d3e9c5`:** both were coordinator-reasoned
fixes for a symptom neither one fixed (the real cause was that a `BSTEventSink`
cannot consume, which `49c9d1b` addresses). They are not known-wrong, but they are
unproven and were kept only because they are independently defensible. If a review
finds either is a no-op or harmful, reverting is fine.

**TRIAGE 2026-10-06 (LIVE, review debt discharged).** The review of `49c9d1b`, `0b8c816` and `6d3e9c5` was DONE (Opus 5.5, read-only, against `b6b7b905`; recorded as a comment on ClickUp 86e394100). `6d3e9c5` was fixed from it: `Board.cpp:685` now passes `storeState=TRUE`. Its findings that are STILL OPEN (the review said they "go to the backlog" and they had not): (SEV-4) nothing checks at runtime that the byte at +0x7B is still E8 before `write_call<5>` in `InstallInputHook` (another plugin's patch there would crash on the first input poll; fix = refuse unless E8/E9); (SEV-4, [DISASM]) on AE `rdx = &stack slot` at that call site is asserted, not shown, check `0x140CD8F40 +0x60..+0x7B`; (SEV-4, field) when the board takes the whole batch, release and stick-centre events for inputs held while it opens are swallowed; (SEV-4, `0b8c816`, dormant) closing the board re-enables all 12 control categories unconditionally, undoing any disable already in place, and blocking kConsole removes the player's way out; (SEV-5) the "dedicated input thread" claim is unverified; (SEV-5) the VR `VariantOffset` slot 0x7B is a placeholder (published VR offset is 0x81); (SEV-5) closing with the shout key starts the 500 ms grace after the release has already happened. `8b95800` is untouched by this note and stays LIVE.

- **MFO-B73 (SEV-4, raised against f5ea00e, Opus 5.5 tier-A round 2).** "An outside positive write larger than W is measured as an award and permanently inflates W by X - W_old." With W >= X the detection line prints and the hold reverts the write; with W < X (any new or low-level follower) the write is counted as an engine award, withheld with no credit, W becomes X and no line prints, so later real awards are under-measured by X - W_old for good. Belongs with MFO-B70 (what to do about outside changes).
- **MFO-B74 (SEV-5, raised against f5ea00e).** "A false 'outside change' line prints once for a granted fixed-stat follower on a load-time re-slam." The difference is -G (the granted points). Latched, one line, noise not correctness.
- **MFO-B76 (SEV-5, raised against dd4c56f).** "A summon whose effect is flagged hostile/detrimental now fires out of combat via AUTO and skips the hostile-branch Sightline LoS check; no vanilla summon affected." Reasoning: the summon block runs before the hostile/beneficial split, so a hostile-flagged summon no longer needs a foe in the combat group and never reaches the LoS gate.

### MFO-B78 (SEV-4) -- CJK fallback logs "loaded, merged into 0 face(s)" when the file exists but fails to load
Raised against MFO 5d8fe87 / MEO ee646bc (fix/cjk-font-fallback), tier-B review 2026-09-23. Verbatim: "When the file exists but fails to load, the log still says 'cjk fallback font: loaded, merged into 0 face(s)'. Scenario: a truncated or locked cjk.otf gives a log that reads as success. Principle 7 (never mask a failure) says to log it as a failure (warn when cjkMerged == 0). Setting cfg.Flags |= ImFontFlags_NoLoadError would also make the missing-file path explicit instead of relying on NDEBUG." Applies to MFO native/Board.cpp mergeCjk and MEO native/plugin.cpp mergeCjk.

### MFO-B79 (SEV-5) -- CJK font loaded once per face (~9 MB MFO, ~13 MB MEO, kept for the atlas lifetime)
Same review. Load cjk.otf once into a never-freed static and pass it to AddFontFromMemoryTTF with FontDataOwnedByAtlas=false on EVERY merge (never let one merge own it). imgui 1.92.8 does not copy the data (imgui_draw.cpp:3256).

### MFO-B80 (SEV-5) -- CJK follow-up polish
Same review: (a) HUD name column lost its old 16-char minimum, so the right-pinned HUD's left edge moves when a follower joins/leaves (keep a minimum width if unwanted); (b) MFO README.md:273 "Both files ship" now three fonts; (c) MEO release completeness lists (release_native.sh:175-183, release_synthesis.sh:67-71) omit fonts/cjk.otf (cp already enforces it); (d) OFL notice could carry upstream "with Reserved Font Name 'Source'"; (e) fs::exists without error_code can throw (pre-existing); (f) Japanese glyph metrics vs EB Garamond and first-use rasterize stutter: judge in game.

### MFO-B81 (SEV-4) -- F1 self-check: "derived from our disassembly" is partly circular
Raised against the F1 consumer branches (MFO 8f16e69 / APMF aab20f2, fork 17184fb8), tier-A review 2026-09-24. The scratch builders (scratchpad/f1/gen/build_spec_lib.py: vtrow, fnrow) took each vtable row's RTTI name and each function row's signature at the Address Library's RVA. The committed generator re-finds every row by RTTI or unique signature before comparing with the library, and the reviewer confirmed every RTTI name matches its construct, so the table is sound; but the independence claim is weaker than written. Fix: generator asserts RTTI name == the construct's class; reword CLAUDE.md / Docs/VERIFIED-ADDRESSES.md from "derived without the library" to "confirmed against it".

### MFO-B82 (SEV-5) -- F1 review small items
Same review: (a) APMF EquipSink.Path.* rows (27/runtime) are checked but nothing consults them, and a failed row prints a misleading "those seats will not install"; (b) the fork refuses inconsistently on an unverified build (ControlMap/CombatController GetRuntimeData and ExtraDataList::GetRuntimeSize are fatal, DOBJ and GetInputContext soft), unreached today; (c) AE kFavor -> 17 rests on controlmap.txt block order, not disassembly, unused today; (d) the gate is keyed by address not by row; (e) the fork's Actor::GetGoldAmount logs an error on every call when Gold is null (unused by us).

### MFO-B83 (SEV-5 x3) -- F1b review items
Raised against fork bf7e9a5d/fde0f3ae + consumer build/commonlib-f1b, tier-A review 2026-09-24 (verdict: merge-ready, nothing above SEV-5). (1) Fork TESForm.h:215 comment says the form-map pointer is "set once at startup and cleared only at shutdown": wrong on AE, where 36601 @0x648CD0 frees and reallocates both map pointers unlocked on a full data reload (probably main-menu reload); SE frees them in the TESForm dtor when a live-form counter hits 0. Pre-existing, the engine's own lookup has the same exposure: fix the comment; exposure is an MFO worker lookup during that reset. (2) A lookup can now Sleep(0)/Sleep(1) while an engine writer holds the lock (map grow/rehash, mostly at load); at MFO APMFBridge.cpp:1107 the worker would hold g_mx while sleeping, in Loot statics a cell spinLock. Bounded and rare. Optional: move the APMFBridge lookup above scoped_lock(g_mx). (3) The two self-check rows for LockForRead/UnlockForRead are log-only; on a mismatch Runtime.h:84/91 would still print "REFUSED ... those seats will not install" though LookupByID keeps calling them. Reword for log-only rows.

### MFO-B85 (SEV-4) -- HMS no longer credits summons as magic use
Raised against ebf0526 (fix/mfo-summon-oneshot), Opus 5.5 round-2 review 2026-09-24. Verbatim: "HMS no longer credits summons as magic use (always-NoOp means Scheduler never calls ProgAllocator::NoteCombatFire ~:1263; the board pulse never lights for summon rules; fix later: call NoteCombatFire from SummonOnMain on a successful combat-table cast, passing the op into the closure; NoteCombatFire takes g_hmsFireMx so main-thread-safe)." Reasoning: CastSummonOnce always returns a transparent NoOp (the cast is decided on the main thread), so the Scheduler's Fired-only bookkeeping never sees a summon.

### MFO-B86 (SEV-5) -- logistics: a summon never counts as the tick's action
Raised against ebf0526. Verbatim: "a summon never sets `acted`, so the same tick can also loot/drink, `[logistics] rule N fired` never logs, g_idleCycles keeps counting, the `== Fired` branch at Logistics.cpp:1596 is dead." Reasoning: same always-NoOp contract as MFO-B85, on the OOC table.

### MFO-B87 (SEV-5 x2) -- summon liveness edge cases
Raised against ebf0526. (1) Verbatim: "a summon with a resolved handle not yet in commandedActors is missed by both live and appearing checks." (2) The worker trusts the main thread's Live/Landing/Limit verdict for 1 s, so a killed summon is re-cast up to ~1 s + one eval late (SEV-5). Reasoning: SummonAppearing requires an unresolved handle and the limit count reads commandedActors, so the window between the handle resolving and the engine adding the entry is uncovered; the trust window trades eval churn for that delay.

### MFO-B88 (SEV-5) -- a CH19 steal episode's first log line promises a grace it will not get
- **Raised:** Opus 5.5 review of `6fca120` (`fix/mfo-loot-m1`, loot round M1), R5, SEV-5.
- **Finding:** the first-episode line (`[loot] <id> travel pkg NEVER ENGAGED ... grace 10s (strike 1/N)`) prints on a CH19 leg that is conceded without the grace (in `6fca120` in the same tick; after the R4 fix at `kCh19Concede`, 2 s later). The text says "grace 10s" for a leg that never waits it.
- **Reviewer's reasoning:** log text only; the behaviour (no MFO nudge, concede) is right.
- **Why it was NOT fixed:** below the severity floor (rule 9); coordinator's call in the M1 round.
- **Fix shape when drained:** on a CH19 leg print the concede point (`kCh19Concede`) instead of `kStealGrace` in that line, or skip the strike text for CH19.
- **Surfaced at edit time from:** MAP.md §4 Logistics "LOOT ROUND M1" What-breaks.

### MFO-B89 (SEV-5, structure) -- the GATED / actor-block machinery wants its own module
- **Raised:** Opus 5.5 review of `6fca120` (`fix/mfo-loot-m1`), structure note, SEV-5.
- **Finding:** `GateSink` (`Logistics.cpp`) plus `g_gates` / `g_gateMx` / `GatedNow` / `MarkGated` / `FindActorBlocker` / `g_actorDefer` / `SortLootCandidates` (`Logistics_internal.h`) are one mechanism split across the core TU and the shared header, placed there only to keep `Logistics.cpp` under the 2500 cap (2480 after the M1 fix round) with `Logistics_Loot.cpp` at the cap.
- **Wave-2 location (2026-09-25, `native/logistics/`):** `GateSink` is `native/logistics/Sinks.cpp:208`; `g_gates` / `GatedNow` / `MarkGated` / `FindActorBlocker` / `g_actorDefer` / `SortLootCandidates` are in `native/logistics/LootTravel_internal.h` (`:343` / `:389` / `:406` / `:455` / `:502`). The wave-2 move kept them there (a pure move); this entry's own module is still open.
- **Reviewer's reasoning:** cohesion, not correctness; the header carries logic and a cross-thread mutex it would not normally hold.
- **Why it was NOT fixed:** a move is its own brief (scope rule 1; a TU split is tier 3).
- **Fix shape when drained:** in the wave-2 Logistics split, move it to `native/loot/Gate.cpp` (or `Logistics_Gate.{h,cpp}`) with its own small header; `TravelFailedRecently` keeps calling `GatedNow` through that header.
- **Surfaced at edit time from:** MAP.md §4 Logistics "LOOT ROUND M1" What-breaks.

### MFO-B90 (SEV-4) -- actor-block check only walks two cells
Raised against e39cee2 (loot M1, author's least-sure spot 5), coordinator 2026-09-24. `FindActorBlocker` (Logistics_internal.h) walks only the follower's and the player's attached cells with ForEachReferenceInRange, so near an exterior cell border an actor standing in a third cell is missed and an actor jam is read as a GATE (with no timer, that gate lasts until a door/lever/cell event). Prefer the engine's loaded-actor list (ProcessLists high/middle actors) filtered by the 200 u cone, which is cell-independent. Also note spot 4: a block that is neither a gate nor an actor (clutter physics) is GATED and only clears on a door/lever/cell event (marth's no-timer rule; accepted, documented).
- **Wave-2 location (2026-09-25, `native/logistics/`):** `FindActorBlocker` is `native/logistics/LootTravel_internal.h:455`.

### MFO-B91 (SEV-4, latent) -- `PerkPointsAvailable` is declared `inline` in the public header but defined only in the internal header
Raised against 26dd9bd (`refactor/subsystem-folders-wave1`, tier-A review of the wave-1 split), 2026-09-24. `progression/ProgAllocator.h:424` declares `inline int PerkPointsAvailable(const ProgState&)`; the only definition is `progression/ProgAllocator_internal.h:238`. Every caller today (PerkGate, Harness, Verbs, Poll) includes the internal header, so it builds and behaves. A TU outside `progression/` that includes only the public header and calls it would odr-use an inline function with no definition in that TU (a compile/link error, not a silent miscompile). Reviewer's reasoning: the public header promises a function it cannot deliver on its own; the split made the declaration `inline` (to match the internal-header definition) instead of keeping the definition next to it. Why it was NOT fixed: the split round is a pure move (no code changes to the split files). Fix shape when drained: either move the definition into `ProgAllocator.h` (it reads only `ProgState` and `g_econ`, so check `g_econ`'s visibility first) or drop it from the public header if nothing outside `progression/` needs it. Surfaced at edit time from: MAP.md progression "What breaks".

### MFO-B92 (SEV-4, latent) -- splitcheck: the text-prefix rule for unnamed read-only data is broader than documented
Raised against 31d97ab (`fix/splitcheck-hardening`, review of the splitcheck hardening), 2026-09-24. Reviewer's verbatim finding: "text-first rule in _unnamed_equal: any unnamed rodata whose first bytes are printable then NUL (small-int tables e.g. {50,..}->"2", UTF-16 L"..") compared only as the short string -> tail hidden." Reviewer's reasoning: "empirical scan of b0 (37972d1 /Od): 2406 rodata targets from MFO:: fns; all text-looking ones are genuine literals (nonzero tails = separate UTF-16 lib literals). finding (a) = latent only -> SEV-4." Note (author, R2): the R2 fix also reads an object whose first byte is 0 in BOTH builds as the empty literal "" (it may stop at a pooled literal only one build has), which is the same residual class. Fix shape when drained: decide string-vs-data from the referencing instruction's access width / the consumer (a `lea` fed to a string parameter vs a sized load), not from the bytes. **Partly done (wave-2 tool round, 2026-09-25):** when BOTH builds have "" at the target, only the NUL byte is compared now (no longer "compared up to a pooled literal only one build has"); a referenced object in its tail is compared at its own reference (selftest N16). Still open: the text-prefix rule itself, and an UNREFERENCED tail of a non-string object whose first byte is 0.

### MFO-B93 (SEV-4) -- splitcheck provenance does not bind the PDB
Raised against 31d97ab (`fix/splitcheck-hardening`), 2026-09-24. Reviewer's verbatim finding: "Q4 provenance: same SHA / missing / other-run all refused (hash-bound). PDB not bound (SEV-4)." Reviewer's reasoning: the build record binds each MFO.dll by SHA-256, but the MFO.pdb beside it is trusted; a PDB from another build would steer symbol matching. Fix shape when drained: record the PDB's RSDS GUID+age (or its SHA-256) in build-info and have `check_provenance` compare it with the DLL's debug directory and the PDB file.

### MFO-B94 (SEV-5) -- splitcheck F5: a twin vs a unique symbol with no module record is accepted by name
Raised against 31d97ab (`fix/splitcheck-hardening`), 2026-09-24. Reviewer's verbatim finding (review summary, SEV-5): "F5 no-module accept". Reviewer's reasoning: `names_match` TU-checks a twin on one side against a unique symbol on the other through the unique symbol's owning module, but when that symbol has no module record (globals/publics only) it still accepts by name (counted in the notes); on wave 1 every one of the 77 hits had a module record. Fix shape when drained: FAIL (or report) the no-module case instead of accepting it.

### MFO-B95 (SEV-5) -- logistics: Logistics_internal.h header comment still describes the pre-split TU set
Raised against 6badb85 (`refactor/subsystem-folders-wave2`, tier-A review of the wave-2 split), 2026-09-25. Reviewer's verbatim finding: "native/logistics/Logistics_internal.h lines 2-9 still say 'One TU (Logistics.cpp)…', 'the four Logistics*.cpp TUs' and 'only the four … may include this'; the file now has 10 logistics TUs plus the 4 existing cast/*.cpp includers." Reviewer's reasoning: comment only, and fixing it would have broken the pure-move brief.

### MFO-B96 (SEV-5) -- logistics: LootTravel_internal.h is not self-contained
Raised against 6badb85 (`refactor/subsystem-folders-wave2`), 2026-09-25. Reviewer's verbatim finding: "LootTravel_internal.h is not self-contained; including it directly fails to compile (loud, not a silent misbehaviour). Only Logistics_internal.h:583 includes it; #pragma once; the banner says so." Reviewer's reasoning: a direct include fails loudly at compile time, never silently; the one includer and the banner document the contract.

### MFO-B97 (SEV-5, informational) -- logistics split: per-TU copy counts of header statics grew
Raised against 6badb85 (`refactor/subsystem-folders-wave2`), 2026-09-25. Reviewer's verbatim finding: "rapidcsv::s_Utf8BOM gained 5 per-TU copies (a header static std::string with a dynamic initializer), and k* constants went from 9 to 14 copies; all identical, none reads another TU's object." Reviewer's reasoning: informational; every copy is identical and no TU reads another TU's copy, so no state is split.

### MFO-B98 (SEV-5) -- target pin: an outranked MFO pin stays IsClaimLive
Raised against 9f845d4 (`feat/mfo-target-pin`, tier-A review of the ch.20 target-pin client), 2026-09-25. Finding as relayed verbatim by the coordinator: "SEV-5: an outranked MFO pin stays IsClaimLive, so PinnedTarget reports MFO's foe while another client steers." Reviewer's reasoning (as relayed): Harbinger's Poll ends only the WINNING pin claim, so a losing MFO claim is neither ended nor applied, and `Targeting::Current` (the retarget hesitation) believes MFO holds a foe it does not. No second ch.20 client exists today. Fix shape when drained: read ownership, not liveness (an APMF owner query if one exists at the drain's ABI), or document the loser state in PinnedTarget. Surfaced at edit time from: MAP.md §7 apmf/ "ch.20 TARGET PIN" What-breaks and §3 Targeting What-breaks.
- **Also raised in the same round, and FIXED in it (not deferred):** "SEV-5: Diagnostics/Probe show 0 latched / 0 asserts on the pin route." The Diagnostics targeting line now prints the route and the standing pin count (`APMFBridge::TargetPinCount`). Probe.cpp's own lines report only hook status and were left alone.

### MFO-B99 (SEV-4) -- ch.17 equip authority: the kEquipAuthValidateAfter grace runs on wall time
Raised against 9f845d4 (`feat/mfo-target-pin`, same review), 2026-09-25. Finding as relayed verbatim by the coordinator: "The kEquipAuthValidateAfter wall-time grace probably has the same pause exposure as item 2; backlog it for ch.17." Item 2 (fixed for ch.20 in the same round): "the 2 s never-live grace runs on wall time, but Harbinger drains only on the PlayerCharacter::Update 0xAD seat, which does not tick while a game-pausing menu is open. A false 'ended' drops the handle WITHOUT Release, which leaves an orphan claim that keeps winning on equal basis (ControlMap.cpp:923-935)." Reviewer's reasoning (as relayed): the ch.17 standing claim's 2 s `IsClaimLive` grace (`apmf/APMFBridge_internal.h` `kEquipAuthValidateAfter`, read at `apmf/Equip.cpp:123` (ch.17) and `apmf/SpellAllowList.cpp:231` (the ch.8 gate claim)) can be aged by a pausing menu the same way. Fix shape when drained: the ch.20 shape -- count unpaused bridge sweeps instead of wall time, skip the judgement while `UI::GameIsPaused()`, and always `Release` a handle before dropping it. Surfaced at edit time from: MAP.md §7 apmf/ "ch.20 TARGET PIN" What-breaks.

### MFO-B100 (SEV-4) -- target pin: the never-live floor counts pump tasks, not frames
Raised against 3d7f191 (`feat/mfo-target-pin`, tier-A re-review), 2026-09-25. Reviewer's wording (as relayed by the coordinator): "the 15 'unpaused sweeps' are counted in pump tasks, not frames. A 2 s+ main-thread hitch can queue ~15 ticks that run back-to-back with no Drain, falsely judging a pin never-live (then Suppressed). A second route is a pin that goes live and ends between two sweeps." Suggested fix: "count a sweep only when a frame or game-time counter has advanced." Reviewer's reasoning (review log, round 2): "SEV-4 sweep count not frame-anchored (uncoalesced pump tasks after hitch) + missed live window". Effect when it fires: a pin that did go live (or would have) is judged never-live, so its foe is Suppressed until the gambit's choice moves; the handle is still Released, so no orphan. Code: `apmf/Excursion.cpp` `SweepTargetPinsLocked` / `PinEndedLocked`, `kPinNeverLiveSweeps`. Surfaced at edit time from: MAP.md §7 apmf/ "ch.20 TARGET PIN" What-breaks.

### MFO-B101 (SEV-5) -- target pin: log volume on a flickering kTargetLost
Raised against 3d7f191 (`feat/mfo-target-pin`, same re-review), 2026-09-25. Reviewer's wording (as relayed): "log volume on a flickering kTargetLost (3 lines per re-pin cycle, no rate limit)." The three lines are `[target-pin] ... ENDED`, `... is trackable again -- re-pinning` and `... PIN` (apmf/Excursion.cpp). Fix shape when drained: a per-follower rate limit on the re-pin cycle's lines, or fold them into one line. Surfaced at edit time from: MAP.md §7 apmf/ "ch.20 TARGET PIN" What-breaks.

### MFO-B102 (SEV-5, author-raised, pre-existing; CORRECTED by the 7580bea review) -- retreat: a follower who dies mid-retreat keeps his hold briefly
Raised by the author of `fix/mfo-retreat` (ClickUp 86e3erv94), 2026-09-25, as SEV-4; corrected by the tier-A review of 7580bea. Original claim: `Scheduler::Tick`'s dead/disabled early return runs before `ServiceRetreat`, so the hold (and on the APMF road the Pump's ch.9 re-offer) outlives him until load/dismissal. Correction (reviewer): it self-heals in ~1.6 s via Refresh -> ReleaseHeldState -> RetreatEvictIf -- a dead/disabled follower fails IsEligibleFollower and is dropped after kMissesBeforeDrop=3 Refresh sweeps. Residual: ~1.6 s of re-offers for a dead actor. Fix shape if ever wanted: `Packages::RetreatClear("dead", f)` in the dead/disabled branch.

### MFO-B103 (SEV-5) -- retreat: posted-lambda generation check is not held across StopCombat
Raised against 7580bea (`fix/mfo-retreat`, tier-A review), 2026-09-25. Finding (as relayed by the coordinator): "the posted-lambda race (SEV-5, harmless)". The posted StopCombat checks (FormID, gen) under `g_retreatLiveMx`, releases the lock, then calls StopCombat; the worker can clear the retreat in between. Reasoning: worst case one extra StopCombat on a follower who has just stopped retreating; no state corruption.

### MFO-B104 (SEV-5, outside the 86e3erv94 boundary) -- cast/Fire.cpp:276-277 comment is stale
Raised against 7580bea (`fix/mfo-retreat`, tier-A review), 2026-09-25. Finding (as relayed): "the stale comment at cast/Fire.cpp:276-277 (SEV-5, outside the boundary)". The act.flee comment still says the retreat is "Cleared on combat end / arrival by the retreat driver"; since 86e3erv94 it ends on arrival (out-of-combat flee) / stay end / timeout / no engaged foes, never on combat end. cast/ was owned by the ch.20 pin branch during this change.

### MFO-B105 (SEV-4, pre-existing) -- a flee rule re-fires about every 2 services at the player's side
Raised against 7580bea (`fix/mfo-retreat`, tier-A review), 2026-09-25. Finding (as relayed): "a flee rule re-firing about every 2 services at the player's side, brushing #22a (pre-existing)". An act.flee gambit whose condition stays true re-fills a retreat each time the previous one releases on arrival, because the gambit path is not gated by the auto-retreat cooldown. Pre-existing shape; needs its own decision (gate act.flee by the cooldown, or by distance to the player).

### MFO-B106 (SEV-4, structural) -- Packages.cpp is near the 2500-line cap
Raised against 7580bea (`fix/mfo-retreat`, tier-A review), 2026-09-25. Finding (as relayed): "Packages.cpp at 2378 lines, near the cap: the next addition there needs a split plan." After review round 1 (main-thread foe probe) it is 2459 lines. The next non-trivial addition needs a split brief first (e.g. the retreat plumbing into its own TU); a split is tier A.

### MFO-B107 (SEV-4, informational) -- retreat foe probe: engaged filter is target-handle only
Raised by the author in response to the 7580bea review's SEV-4 ("LiveFoeNear counts unengaged hostiles"), 2026-09-25. The probe now counts only hostiles holding a live `currentCombatTarget`; it does NOT read the foe's `IsInCombat()`, which derefs the raw `combatController` that StopCombat frees inline on BSJobs (ENGINE_NOTES §0.47) while main-thread exclusion from those jobs is unproven (#74). A hostile that is in combat with a momentarily empty target handle is therefore not counted on that probe; the 3-probe / 3 s window absorbs single-probe gaps. Revisit when the combat-thread FoeCount/inCombat mirror (STATUS "MFO-next") lands.

### MFO-B108 (SEV-4) -- retreat: after a pause the wall-clock floors are already spent
Raised against 19f67e0 (`fix/mfo-retreat`, closing re-review), 2026-09-25. Reviewer's finding (as relayed): "after a pause, the wall-clock seconds floors are already spent, so only the 3-probe floor guards the ends." Suggested fix: time the floors by service time, or reset the timestamps on the first service after a pause.
Partly resolved in the same closing round: STAY is now timed on the unpaused service clock (marth's "about ten seconds, or until fully healed"). Still open for the no-foe window (3 s) and the cooldown (10 s).

### MFO-B110 (SEV-5) -- retreat: being hit in STAY without entering combat can hold him up to ~3 s
Raised against 19f67e0 (`fix/mfo-retreat`, closing re-review), 2026-09-25. Reviewer's finding (as relayed): "being hit in STAY without entering combat can hold him for up to ~3 s."
Note: with STAY now "about ten seconds, or until fully healed" (same closing round) the bound is up to ~10 s of unpaused time. (MFO-B109, "kRetreatStayMax is unreachable", was dropped: that rework removed the cap.)

### MFO-B111 (SEV-4) -- engage-on-sight: no ghost filter in the enemy test
Raised against 1c50696 (`feat/mfo-ooc-engage`, tier-A review round 1), 2026-09-25. Coordinator's fix list (as relayed): "(c) skip hostile ghosts, restrained, bleeding-out, or ignore-combat-package actors, where each symbol verifies cleanly (otherwise backlog it)."
Deferred part: the GHOST check only. `Actor::IsGhost` in the fork is an id call (`Offset::Actor::GetGhost` = `RELOCATION_ID(36286, 37275)`, `src/RE/A/Actor.cpp:780`) with no row in `native/VerifiedAddresses.h`, so using it needs a spec row, a regenerated header and a `SeatVerified()` guard (CLAUDE.md SEAT SELF-CHECK). Restrained / bleed-out / IgnoreCombat shipped in the same round (`EngageOnSight.cpp` `IsEnemy`).

### MFO-B112 (SEV-4) -- engage-on-sight: a stale currentCombatTarget can let a guard through the crime filter
Raised against 72a7964 (`feat/mfo-ooc-engage`, tier-A re-review), 2026-09-25. Reviewer's finding (verbatim from the review log): "commanded actors of civilians pass the crime filter (GetCrimeFaction returns null for commanded actors); a stale currentCombatTarget can pass it too." Coordinator's framing: "a stale currentCombatTarget letting a guard through after a yield or bounty payment."
The commanded-actor half was fixed in the next commit (the commander's crime faction is checked). Deferred: `FightingParty` trusts `currentCombatTarget` alone; after the player yields or pays a bounty the guard's handle can still name the player for a while, so one engage could slip through in that window.

### MFO-B113 (SEV-4) -- engage-on-sight: three quest mines read as civilised
Raised against 72a7964 (`feat/mfo-ooc-engage`, tier-A re-review), 2026-09-25. Reviewer's finding (verbatim from the review log): "Quest mines Kolskeggr/Redbelly/Soljund's (Mine+Dwelling, no Clearable) do read civilised." Coordinator: "the three quest mines read as civilised (Kolskeggr, Redbelly, Soljund's Sinkhole: inert, harmless)."
Effect: the town/inn filter stops engage-on-sight inside those three while their quest enemies are there (unless the player is fighting). Inert, never a wrong engage.
### MFO-B114 (SEV-4) -- swap-up: the ammo peek ignores carry weight
Raised against 4f23c30 (`feat/mfo-swapup-ammo`, tier-B review), 2026-09-25. Finding (as relayed): "the ammo peek ignoring carry weight". `SwapUpAmmoFrom`'s peek (`native/logistics/SwapUp.cpp`) answers "would take something" without the per-stack `FitsCarryWeight` the take applies, so an over-encumbered follower can walk to a body whose better ammo he then cannot carry (the shipped `LootAmmo` peek had the same shape). Fix shape: run the carry-weight test in the peek too, as `LootEquipment`'s gear peek does.

### MFO-B115 (SEV-4) -- swap-up: ComputeKeepSet runs inside the loot peek
**RESOLVED (moot) 2026-10-18, `fix/mfo-keepset-roles`:** `PlanRoomForSwapUp` is deleted (no dropping to make room), so the peek no longer builds the keep set.
Raised against 4f23c30 (`feat/mfo-swapup-ammo`, tier-B review), 2026-09-25. Finding (as relayed): "the ComputeKeepSet peek cost". When a gear upgrade does not fit, `LootEquipment`'s peek calls `PlanRoomForSwapUp`, which builds the whole keep set (inventory walks, `ComputeWeaponRoles`, `ArmorPrefFor`, and since round 1 `BuildEquipmentContext`) once per candidate body during a scan. Only on the over-weight path. Fix shape: build the keep set once per `LootNearby` call (like the lazily built `EquipmentContext`) and pass it down.

### MFO-B116 (SEV-4) -- swap-up: VendorTrades(null) on a TESAmmo keyword form
**RESOLVED 2026-09-28, branch `fix/mfo-field-0928c`.** Not a VR-only edge: CommonLib is built with SE + AE + VR
all on (the fork's CMakeLists `ENABLE_SKYRIM_*` default ON, the port passes none), and in that configuration
`TESAmmo`'s C++ class derives only `TESValueForm` (`RE/T/TESAmmo.h` #else branch), so `As<BGSKeywordForm>()`
(FormTraits' `is_convertible` static cast) is NULL for EVERY ammo form and every ammo sell row was refused
'vendor-filter' (field 0928c: Adelinda's bolts, 148 refusals at four blacksmiths). EconomyProbe's ammo rows now
read `TESAmmo::AsKeywordForm()` (runtime offset 0xF8 SE/AE, 0xE8 VR). `LootTake.cpp:171` / `Sinks.cpp:225` still
use `As<BGSKeywordForm>()` and may meet ammo -- not changed (outside the branch's boundary).
Raised against 4f23c30 (`feat/mfo-swapup-ammo`, tier-B review), 2026-09-25. Finding (as relayed): "VendorTrades(null) on TESAmmo". The obsolete-ammo sell rows pass `obj->As<RE::BGSKeywordForm>()`; on a layout where `TESAmmo` does not derive from `BGSKeywordForm` (the VR branch of `RE/T/TESAmmo.h`) that is null and `VendorTrades` refuses the row (unless the merchant-perk bypass is on), so obsolete ammo silently never sells there. Fix shape: log the refusal once, or read the ammo's keywords through the runtime's own layout.

### MFO-B117 (SEV-4) -- lockpick doors: an exterior door in an adjacent cell is missed
Raised against c6fc73d (`feat/mfo-lockpick-doors`, tier-A review), 2026-09-26. Coordinator's framing of the reviewer's SEV-4 #3: "the adjacent-cell exterior miss". `DispatchGateDoor` (`native/logistics/Lockpick.cpp`) searches only the follower's and the target's ATTACHED parent cells for the locked door. Outdoors, a door standing just across a cell border from both (the stall point near the border) is not seen, so the item stays GATED under M1's normal re-admit instead of getting a door leg. Fix shape: also walk the attached cell that contains the block point S (or the player's, as `FindActorBlocker` does).

### MFO-B118 (SEV-5) -- Service.cpp needs its split plan
Raised against c6fc73d (`feat/mfo-lockpick-doors`, tier-A review), 2026-09-26. `native/logistics/Service.cpp` is past the ~1500-line plan-a-split mark (about 1880 lines after LP-M2; the 1462-line `ServiceFollower` is one function). CLAUDE.md: the next brief touching it proposes the split, done as its own round with `tools/splitcheck`. Not done here (scope rule 1).
### MFO-B119 (SEV-5) -- confidence v2: the HP-trend ring is not erased on dismissal
Raised against 09cb24e (`feat/mfo-confidence-v2`, tier-B review), 2026-09-26. Finding (as relayed): "the ring not erased on dismissal". `Confidence::detail::g_trend` (`native/Confidence.h`) keeps one small entry per follower ever serviced until a revert (`ClearTrend` in `Scheduler::ClearTransientState`). A dismissed follower's stale samples are harmless on return (a > 10 s service-clock gap restarts the ring before any read), so the cost is only a few hundred bytes per dismissed follower per session. Fix shape: erase his entry on the dismissal path.

### MFO-B120 (SEV-5) -- confidence v2: OfFacing ignores foe weight
Raised against 09cb24e (`feat/mfo-confidence-v2`, tier-B review), 2026-09-26. Finding (as relayed): "OfFacing ignoring foe weight". The engage-on-sight estimate `Confidence::OfFacing(f, n)` counts every joiner as an EVEN foe (weight 1.0), while `Of()` weighs the live group by `CombatSense::FoeWeight` (0.5..2.0). A pack of mudcrabs is therefore judged harder, and a dragon easier, before the fight than once it starts. Partly covered by the review round's `kEngageConfidence` margin (retreat floor + 0.15). Fix shape: have the engage probe return the joiners' summed weight (FormIDs only, on its main-thread probe) and pass a load instead of a count.
### MFO-B121 (SEV-4) -- LOTD: the museum snapshot's VM read races Papyrus
Raised against 7452ccb (`feat/mfo-lotd`, tier-A review round 1), 2026-09-26. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-lotd.md`): "VM property read race vs Papyrus threads". `Lotd.cpp` `RebuildSnapshot` reads the `DBM_MuseumAPI` object's FormList arrays (and, since round 2, `CrateState` reads a crate script's `currentState`) on the MAIN thread while Papyrus VM threads may be writing them (`AddNewDisplaySection` / `MergeLists` during LOTD's patch registration, `GoToState`). Read-only on MFO's side; the worst case is one stale or torn snapshot, replaced at the next rebuild trigger. Fix shape: read inside a VM-safe window (a Papyrus-side hop or LOTD's own `MCMRefresh` completion only).

### MFO-B122 (SEV-4) -- LOTD: alternative items over-count what is needed
Raised against 7452ccb (`feat/mfo-lotd`, tier-A review round 1), 2026-09-26. Reviewer's finding (verbatim from the review log): "alternatives over-count". A slot that accepts any one of several bases (a FormList of alternatives) adds +1 want to EVERY alternative while it is uncovered (`Lotd.cpp` `Uncovered`), so a follower can loot two alternatives for one slot before the next needs refresh counts the first. The greedy cover then releases the surplus from `HoldFromSale` (it sells normally). Fix shape: decrement every alternative of a slot when one of them is taken within a cache window.

### MFO-B123 (SEV-5) -- LOTD: a reload before arrival can ship one duplicate
Raised against 7452ccb (`feat/mfo-lotd`, tier-A review round 1), 2026-09-26. Reviewer's finding (verbatim from the review log): "duplicates after reload (acknowledged)". The in-transit ledger is session-only (no co-save record), so a save loaded while MFO's deposit is still in an unloaded town crate forgets it and a follower can ship a second copy. LOTD leaves the extra copy in DropoffCrate. Round 2 made the ledger a floor until arrival, which narrows but does not close this. Fix shape: a co-save record (tier A, its own brief) or reading the crate when it next loads.

### MFO-B124 (SEV-4) -- LOTD: the in-transit ledger floor can leak for the session
Raised against 5b88bcd (`feat/mfo-lotd`, tier-A re-review round 2), 2026-09-26. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-lotd.md`): "Ledger floor leak: player takes relic back from town crate then sells -> phantom supply for session". (a) as quoted: the entry never sees its item arrive in DropoffCrate, so it keeps counting as supply until a load, and followers stop collecting that relic. (b) the same leak when the player sorts DropoffCrate before the arrival read (the author's round-2 uncertain spot). Both fail toward collecting LESS, never toward selling or shipping a duplicate. Fix shape: re-read the crate when it is loaded and drop entries whose items left it without arriving.

### MFO-B125 (SEV-4) -- LOTD: the same base shipped from two crates releases a ledger entry early
Raised against 5b88bcd (`feat/mfo-lotd`, tier-A re-review round 2), 2026-09-26. Reviewer's finding (verbatim from the review log): "Same-base double deposit baseline -> early release". Two deposits of one base (two crates, or one crate twice) each record a DropoffCrate baseline at their own deposit time; the first arrival can satisfy the second entry's `baseline + count` test and release it while its items are still in transit, so a follower can collect (and ship) one more copy. Fix shape: key arrival on the sum of every pending entry of that base.

### MFO-B126 (SEV-4) -- LOTD: an off-role relic kept for the deposit can be auto-equipped and then never shipped
**RESOLVED 2026-09-27, branch `fix/mfo-batchL-field` (batch L field fix 2, carve-out (b)).** Both halves: (MFO pick)
`cast/Equip.cpp` `EquipWeapon` keeps relics (`Lotd::HoldFromSale`) out of the pick unless nothing else of the category
is carried, `PickOffHandWeapon` skips them, and a HELD relic does not satisfy the equip rule while an alternative is
carried, so an old FWPN relic hold or an AI-equipped relic is replaced on the next equip lap. The ch.17 declaration's
hands come only from that hold ledger, so the declared set never names a relic when an alternative exists (no change
was needed in `EquipAuthority.cpp`; owning an unheld hand there to refuse the AI's equip is the F6 freeze). (Engine
AI in an unowned hand) the backlog's second fix shape: `ShedOffRoleWeapon` reports the relics it kept
(`Lotd::NoteKeptForDeposit`, only with an in-role weapon left), and `Shippable` / `TransferOnMain` ship such a relic
even when WORN, unless an MFO hold names it. The shed would have dropped it worn or not, so this is not the player's
dressing. Review round on 93613c4 (SEV-2: no weapon player-pick signal): the new `logistics/PlayerGiven.cpp` records
player -> follower transfers and in-menu equips; a player-given item is never shipped (worn or not), a worn relic ships
only when NOT given and one of the in-role weapons is still carried at ship time, the kept set is cleared on every shed
entry, a player-equipped relic is never swapped out of his hand, and the transfer unequips a worn instance before the
RemoveItem. Residuals: MFO-B127, MFO-B128.
**Ruling 2026-09-27 (ClickUp 86e3faccn, branch `fix/mfo-relic-given-cosave`):** marth: "What you drop on the ground is
fair game. As well as regular items given to followers. Relics however should be recorded." The player-given record
now covers museum relics only and is SAVED (co-save record 'PGIV' v1), so the round-3 reload limit ("after a load a
given relic is forgotten") is closed for gifts; the AI-equip positive proof stays session-only by design.
Raised against 5b88bcd (`feat/mfo-lotd`, tier-A re-review round 2), 2026-09-26. Reviewer's finding (verbatim from the review log): "KeepForDeposit + AI self-equip -> worn -> never shipped." `ShedOffRoleWeapon` now leaves a needed relic weapon in the pack (`KeepForDeposit`); if the follower's own AI equips it, `Shippable` skips it as worn (the manual-override rule), so it neither goes to the player nor to the crate. Fix shape: exclude a relic from the engine's equip choice while it is kept for the deposit, or ship a worn relic that MFO (not the player) left in the pack.

### MFO-B129 (SEV-5) -- LOTD: a loose museum relic can be over-taken within one needs window
Raised against ea518ef (`feat/mfo-loose-loot`, tier-A review), 2026-09-27. Finding (the coordinator's closing-round wording, "the museum over-take", of the author's uncertain spot 5): "`LooseMuseumQualifies` does not decrement the needs cache, so two loose copies of one relic can both qualify within one 2 s needs window." A container take (`LootMuseum`) decrements `needs.uncoveredAll` as it moves items; the loose road cannot (the pickup is the arrival ActivateRef, a tick or more later), so the same need can admit a second loose copy before the follower supply is re-read. Harm: one spare relic in a follower's pack (it is held, never sold, and ships only while still needed). Fix shape: an in-flight loose-museum ledger (ref -> base) counted as supply by `FreshNeeds` until the readback, or decrement `uncoveredAll` when the leg is armed.
### MFO-B127 (SEV-4) -- batch L: a relic can be picked for one lap while the LOTD needs cache is unanswerable
Raised against 93613c4 (`fix/mfo-batchL-field`, tier-B review), 2026-09-27. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-batchL-field.md`): "HoldFromSale transient false on snapshot swap". `Lotd::FreshNeeds` returns an EMPTY `Needs` while a new snapshot has no main-thread supply copy yet (a rebuild, a load, a LOTD ModEvent), so `HoldFromSale` reads false for that window and `cast/Equip.cpp` `IsMuseumRelic` lets a relic into the ordinary pick pool. If the relic wins that lap it is force-held until the gambit releases it; the next lap's `relicWithAlternative` then swaps it back out, so it is transient, but it is a real equip flicker. Fix shape: a three-state answer from Lotd (relic / not a relic / not answerable yet) and an equip pick that keeps the last answer (or skips HoldFromSale-uncertain weapons) while unanswerable.

### MFO-B128 (SEV-4) -- batch L: a relic in one hand with a non-relic in the other still satisfies the equip rule
Raised against 93613c4 (`fix/mfo-batchL-field`, tier-B review), 2026-09-27. Reviewer's finding (verbatim from the review log): "right-relic+left-nonrelic satisfies". `EquipWeapon`'s "already holding" test is per hand and ORed: a relic in the right (or left) hand with an eligible NON-relic in the other hand satisfies the rule, so the relic hand is not swapped out and the off-hand top-up only runs with an empty left. The mixed-hands residual of MFO-B126's equip half. Fix shape: when either held hand is a relic with an alternative, run the pick for that hand (the right via the normal pick, the left via `PickOffHandWeapon`) instead of the satisfied NoOp.

### MFO-B130 (SEV-4) -- player-given: UI::IsMenuOpen is read on the event thread
Raised against 0742e20 (`fix/mfo-batchL-field`, tier-B re-review), 2026-09-27. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-batchL-field.md`): "IsMenuOpen unlocked menuMap read off-main (precedent exists)". `logistics/PlayerGiven.cpp` `GiveMenuOpen` calls `RE::UI::IsMenuOpen` from the `TESEquipEvent` sink, which may run off the main thread; `IsMenuOpen` reads the UI's menu map without a lock. The same read already exists in the tree (`LootScan.cpp:78`, `LootTake.cpp:540`, `Service.cpp:1195`, `Lotd.cpp:848`). Worst case: a misread verdict for one event (a player equip recorded as an AI equip or the reverse), which the positive-proof rule fails closed on for the ship. Fix shape: an atomic "give menu open" mirror written by a MenuOpenCloseEvent sink on the main thread, read by the equip sink.

### MFO-B131 (SEV-5) -- player-given: a queued mark can land after its item has already left
Raised against 0742e20 (`fix/mfo-batchL-field`, tier-B re-review), 2026-09-27. Reviewer's finding (verbatim from the review log): "Mark-queue vs leave Bits race fails closed". The GIVEN / EQUIPPED marks are queued (AddTask), while the leave event reads `Bits` inline: an item that arrives and leaves before the queued mark lands skips the recount, so a stale "given" entry stays until revert. Fails closed (a stale entry only ever blocks a ship or keeps a relic in hand). Fix shape: queue the leave check through the same AddTask queue (ordered after the mark), or recount on the next arrival.
**Update 2026-09-27 (86e3faccn, `fix/mfo-relic-given-cosave`):** the record is now relics only and SAVED ('PGIV'), so a
stale entry of this kind is written to the co-save and survives a reload (it is still dropped by the next leave of
that base, and still fails closed). Still open.

### MFO-B132 (SEV-5) -- player-given: a gift before LOTD's first snapshot is recorded whatever it is
Raised by the author of `fix/mfo-relic-given-cosave` (86e3faccn), 2026-09-27, as a known limit for the tier-A review.
`Lotd::MayBeRelic` answers TRUE while LOTD is detected but no snapshot has been read (a new game before LOTD's
MCMRefresh, or a failed VM read), so a gift in that window is recorded and saved even when it is not a relic. It is
inert: every consumer (`Shippable`, `TransferOnMain`, `KeptOffRoleWorn`, `IsMuseumRelic`, `relicWithAlternative`)
only asks about relics. Cost: a few extra PGIV entries until the item leaves him. Fix shape: re-test the entries
against the snapshot when it arrives (or at save time) and drop the non-relics.

### MFO-B133 (SEV-5 x2) -- museum priority: the seat-absent WARN also fires in observe-only mode; the other-floor BLOCKED line prints reach 0
Raised against 56c032b (`fix/mfo-museum-priority`, tier-B review round 2), 2026-09-28. Reviewer's finding (verbatim from the review log `agentlogs/review-mfo-museum-priority.md`, round 2, item (e)): "seat-absent WARN also fires in observe-only (SEV-5); !sameFloor fail log prints reach 0 (SEV-5)". Reasoning (from the same log): `RefreshEquipDeclaration` picks WARN vs ERROR on `IsEquipAuthorityEnforced()`, which is also false when the seat is installed but `bEquipObserveOnly=1`, so a refusal there (not expected under the v8 contract, which refuses only while the seat is not installed) would be worded as "seat not installed"; and in `Lotd.cpp DepositTick` a BLOCKED stop on another floor skips `WithinReach`, so the failure line's "outside reach (0 u)" is a default, not a measurement. Both are log wording only. Fix shape: tell observe-only from not-installed if APMF ever exposes it; compute the reach value before the floor test (or omit it from the other-floor failure line). Surfaced at edit time from: MAP.md §4 LOTD "MUSEUM PRIORITY" and §7 apmf/ refused-claim paragraph.

### MFO-B134 (SEV-4) -- no decline-fallback: Gear.cpp's weapon equip hop and EquipTorch still equip directly under a refused claim
**DRAINED 2026-10-01 (`fix/mfo-equip-authority-small`):** the fix shape below was applied as written. `logistics/Gear.cpp` AcquireEquip's top (the weapon becomes a real stock before the MEO gem capture: no capture, no queued move, returns false) and `logistics/Upkeep.cpp:182` (`EquipTorch`) skip the direct equip, with an `[equip]` log line, when `EquipAuthoritySupported() && !IsEquipAuthorityClaimed(fid)`. The whole-APMF-absent legacy path is unchanged. Field check: the confirming line in the next field log under a refused claim is `[equip] <id>: '<item>' stocked, not equipped -- equip authority supported but no claim stands (MFO stays out)` (weapon) and `[equip] <id>: torch skipped -- ... (MFO stays out)` (once per streak), with no direct `[equip]`/EquipObject for that follower.
Raised against 56c032b (`fix/mfo-museum-priority`, tier-B review round 2), 2026-09-28. Reviewer's finding (verbatim from the review log, round 2, item (e)): "Gear.cpp weapon direct hop + EquipTorch still direct under refusal (SEV-4)". Reasoning (from the same log): the "MFO stays out" rule (marth 2026-09-28, "MFO's fallback is deprecated") now covers the combat equip gambit, the off-hand top-up, the shield and armor (`EquipAuthorityLive`), but `AcquireEquip`'s equip-in-place hop for a just-looted WEAPON (`logistics/Gear.cpp`, gated only for ARMOR) and `EquipTorch` (`logistics/Upkeep.cpp`, gated on `EquipAuthorityOwns(Left)`, false without a claim) still make direct equips for a follower whose claim was refused. Fix shape: gate both on "authority supported and claim refused" the same way (`EquipAuthoritySupported() && !IsEquipAuthorityClaimed(fid)` -> no direct equip), keeping the whole-APMF-absent legacy path. Surfaced at edit time from: MAP.md §7 apmf/ refused-claim paragraph.

### MFO-B137 (SEV-4) -- relic shipping: RemoveItem(null extra list) taking unworn copies first is unverified
Raised against cc4e895, 2026-09-28. Reviewer's finding (verbatim, narrow check, (2)): "So \"plain copies are taken first\" is still an assumption. It is also false in one case: when the unworn copies themselves carry extra lists (tempered, enchanted, owned). Harm is low. The shipped copy is the same base, and a player-given base is skipped entirely before this point. The worst case is that the worn copy is removed and he is briefly unequipped in that slot until the next declaration." Reasoning: the trace reached `Actor::RemoveItem` (AE 0x692800, id 37797) -> `TESObjectREFR::RemoveItem` (AE 0x2e1750, id 19689), not the InventoryChanges instance selection. Fix shape: compare `WornCopies` before/after and log an ERROR if a worn copy vanished on an unworn ship; when the plain count is below the amount to move, pass each unworn `ExtraDataList` explicitly. Surfaced at edit time from: MAP.md §4 LOTD.

### MFO-B138 (SEV-5) -- mid-trip heal condition match: one wasted yield per 90 s
Raised against cc4e895, 2026-09-28. Reviewer's finding (verbatim, narrow check, (3)): "at most one wasted yield and restart per 90 s for a heal rule that matches but never fires. `Evaluate` runs under the trip lock again; it is a pure scan, so the round-1 lock analysis still applies." Surfaced at edit time from: MAP.md §4 LOTD.

### MFO-B139 (SEV-5) -- shield deny: a mage on class Auto is not a pure caster
**DEFERRED 2026-10-01 (not drained):** the entry gives no fix shape, and the only fix is to resolve Auto (`combatClassOverride` 0) to a class. Base MFO has no Auto-to-class resolver (a known open decision), so any fix would invent one. Reusing `IsCasterFollower` would widen `pureCaster` to spellswords (review R2-3 deliberately denies the shield only to a BASE class Mage). Revisit when the resolver decision is made.
Raised against cc4e895, 2026-09-28. Reviewer's finding (verbatim, narrow check, (4)): "`pureCaster` keys on `combatClassOverride`, a board setting where 0 means Auto. A mage whose class is left on Auto is not denied unless he has no one-hand melee role." Surfaced at edit time from: MAP.md §7 equip authority.

### MFO-B140 (SEV-4) -- lockpick ready gate: the sheathe can flicker against a "draw with the player" framework
Raised against 73119b8 (`fix/mfo-field-0928c`, tier-B review), 2026-09-28. Reviewer's finding (verbatim from `agentlogs/review-mfo-field-0928c.md`): "Ready-gate sheathe vs a framework that mirrors the player's drawn weapon (NFF/AFT 'draw with player'): MFO sheathes once per job, the framework redraws, the gate refuses notReady after 5 s, the chest gets a transient skip, and the next arrival repeats: one sheathe/redraw flicker plus a refused pick per transient-skip cycle while the player walks with his weapon out. Not a tight loop (sheathePosted is once per job; the post re-checks IsInCombat). Never touches the player." Reasoning: bounded (one sheathe per job), no fight with the combat AI. Fix shape: when the weapon is redrawn after MFO's sheathe within the job, wait on the player's own drawn state instead of re-sheathing on the next job. Surfaced at edit time from: MAP.md §4 LOCKPICK READY GATE.

### MFO-B142 (SEV-5) -- ReportTrade's unknown-token classification was bundled with item 4
Raised against 73119b8 (`fix/mfo-field-0928c`, tier-B review), 2026-09-28. Reviewer's finding (verbatim): "Scope: TradeBridge's ReportTrade unknown-token classification (g_sessionFirstToken, warn -> info for pre-session tokens) is not one of the brief's items; benign log-only change, bundled by the author with item 4." Reasoning: log-only, benign. (The coordinator's brief did ask to "look at" that line; the author changed its level.) No action needed unless the line's wording is disputed. Surfaced at edit time from: MAP.md §7 TradeBridge NO EMPTY TRADES.

### MFO-B143 (SEV-5) -- the idle confirmation window lengthens every deposit and every pick
Raised against 73119b8 (`fix/mfo-field-0928c`, tier-B review), 2026-09-28. Reviewer's finding (verbatim): "Deposit Giving now waits 3.5 s unpaused before the transfer (+2.5 s per deposit vs 1 s), then kIdleSettle 2.5 s after it -- the follower stands ~6 s at the crate after the one-shot IdleGive clip has ended. No interaction with kTripMax 90 s beyond the added seconds; SweepTrip's 10 s stall test is unaffected (DepositTick still ticks). Every lockpick is now >= 3.5 s (max(window, 3.5))." Reasoning: cosmetic dwell, no correctness impact. Fix shape: shorten kIdleSettle by the time already spent in the confirmation window. Surfaced at edit time from: MAP.md §4 LOCKPICK (CONFIRMATION WINDOW) and §4 LOTD.

### MFO-B145 (SEV-4) -- melee-only reach gate: MeleeOnly misses an Auto-class melee follower, and a bow flicker restarts the hold
Raised against 0f9ac10 (`fix/mfo-unreachable-flyer`, tier-B review), 2026-09-29. Reviewer's finding (verbatim): "MeleeOnly covers only base class Melee (combatClassOverride 1, a board setting, 0 = Auto) or a standing equip_melee force-hold. A melee follower left on Auto whose rules have no equip_melee is never gated. And the 'bow briefly in hand' tick: with the seat enforcing, the bow deny refuses the equip at EquipObject (diag step 7), so the bow never lands; in observe-only mode it does land, MeleeOnly reads false, and every gate is off for as long as he holds it -- consistent (he is ranged then), but the hold ends and restarts with a Repoint and two [reach] lines per flicker." Reviewer's reasoning: the definition matches the brief ("MFO has declared him melee"); the gap is an Auto follower MFO never declared, and the flicker only exists in APMF observe-only mode. Code: `cast/Fire.cpp` `MeleeOnly`, `Scheduler.cpp` `ServiceLeashWithReachHold`. Surfaced at edit time from: MAP.md §2 cast/ "MELEE-ONLY REACH GATE" and §2 Scheduler "REACH HOLD".

### MFO-B146 (SEV-4, unverifiable offline) -- melee reach: a hovering dragon's origin may sit at its body centre
Raised against 0f9ac10 (`fix/mfo-unreachable-flyer`, same review), 2026-09-29. Reviewer's finding (verbatim): "Dragon origin vs body: the 'above' test compares the dragon's ORIGIN to his head + 200 u (about 330 u over his feet). If a hovering dragon's origin sits at its body centre, its legs and tail can be within a swing while dz reads just over the line, so he holds instead of swinging at a low hover. Landing counts as reachable, so the miss window is Hovering/Action close passes only. Direction is safe (holds, never chases); confirm with the [reach] dz numbers in the next field log." Reviewer's reasoning: the direction of the error is safe, and only field data can settle it. **The next field log's `[reach]` dz numbers decide it** (a low hover that should be hittable, logged just over the line, means the line needs the foe's hang below its origin). Code: `CombatSense.h` `OutOfMeleeReach`. Surfaced at edit time from: MAP.md §3 CombatSense "MELEE REACH".

### MFO-B147 (SEV-5) -- reach hold: g_reachHold is not erased on dismissal mid-fight
Raised against 0f9ac10 (`fix/mfo-unreachable-flyer`, same review), 2026-09-29. Reviewer's finding (verbatim): "g_reachHold is erased on the party-OOC branch and on revert, not on dismissal mid-fight: a follower dismissed while holding and re-hired into a later fight starts with wasHolding true (one scan at slack -64 and a spurious 'hold ended' line). Harmless." Reviewer's reasoning: one scan at the band and one extra line, no behaviour change. Fix shape when drained: erase it from the dismissal road (`Followers::ReleaseHeldState` has no Scheduler hook today, so it needs one). Surfaced at edit time from: MAP.md §2 Scheduler "REACH HOLD".

### MFO-B148 (SEV-5, informational) -- unreachable-flyer scope + Scheduler.cpp size
Raised against 0f9ac10 (`fix/mfo-unreachable-flyer`, same review), 2026-09-29. Reviewer's finding (verbatim): "Scope: 9 files; PursuitLeash.cpp / APMFBridge.h (PursuitLeashStanding, read-only under g_leashMx) and Actuation.h (the MeleeOnly declaration) are the needed plumbing. Scheduler.cpp is 1773 lines (past the 1500 plan-a-split mark; flagged by the author, not a STOP)." Reviewer's reasoning: the out-of-brief files are needed plumbing; Scheduler.cpp needs its own split brief (now ~1790 lines after the review fixes). Surfaced at edit time from: MAP.md §2 Scheduler "REACH HOLD".

### MFO-B149 (SEV-4) -- can-act: castSeen / equipHeld kept through a long bleedout while the APMF claims lapse
Raised against 1b3af00 (`fix/mfo-can-act`, tier-B review of 91b17a3), 2026-09-29. Reviewer's finding (verbatim, C4): "Keeping castSeen / equipHeld while down keeps MFO's own latches (CasterConsent Want, the equip force-hold) through a long bleedout while the APMF claims behind them lapse by TTL; on the way up MFO's side still believes the loan stands until the next lap re-files it. Consistent with the suppression-window stop it copies; no loop. Worth a field check of the first lap after an UP line." Reviewer's reasoning: consistent with the suppression-window stop, no loop. Check the first lap after a `[bleed] ... UP` line in the next field log. Code: `Scheduler.cpp` combat scan (the `cannotAct` skip). Surfaced at edit time from: MAP.md §2 cast/ "CAN-ACT" and §2 Scheduler "CAN-ACT".

### MFO-B150 (SEV-4) -- can-act: a main-thread reach refusal leaves a registered stream applying nothing for a lap or two
Raised against 1b3af00 (same review), 2026-09-29. Reviewer's finding (verbatim, C5): "Main-thread reach refusal (RefuseHealApplyOnMain: MeasureNow says Occluded while the worker's cache said Unknown/Clear) leaves a registered stream -- hand lock, proxy slot, Fired outcome -- applying nothing until the worker's next check sees Occluded and stops refreshing it (stale release). Bounded to a couple of laps; the Scheduler counted a Fired that delivered nothing." Reviewer's reasoning: bounded to a couple of laps. Fix shape: have the main-thread refusal mark the stream so the next worker lap declines it at once. Code: `cast/Direct.cpp` `RefuseHealApplyOnMain` / `CastTargetDirect`. Surfaced at edit time from: MAP.md §2 cast/ "CAN-ACT".

### MFO-B152 (SEV-5, informational) -- can-act scope + file sizes
Raised against 1b3af00 (same review), 2026-09-29. Reviewer's finding (verbatim, C9): "Scope and size: 14 files (CAST-DELIVERY / GAMBIT-GUIDE edits match the new behaviour). Direct.cpp 1637, Scheduler.cpp 1816, Service.cpp 2133 lines -- all past the 1500 plan-a-split mark, Service.cpp within 370 of the 2500 backstop." Reviewer's reasoning: each needs its own split brief; Direct.cpp's CAN-ACT block (end of file) is the natural cut. Surfaced at edit time from: MAP.md §2 cast/ "CAN-ACT", §4 logistics/Service.cpp.

### MFO-B154 (SEV-4) -- animheal-p0: every follower's allow-list carries ALL minted ConcProxy forms
Raised against 22d735d (`feat/mfo-animheal-p0`, tier-B review), 2026-09-29. Reviewer's finding (verbatim): "allow-list budget. Every follower's list gets ALL minted ConcProxy forms (up to 6), including forms held by OTHER followers. `PublishSpellAllowList` appends `ConcProxyForms()` before the potion budget is computed. Each proxy therefore takes one potion slot from `SelectPotions`. A trimmed combat potion is a refused candidate, so the AI cannot drink it. A borderline follower can also be tipped into the MFO-B65 overflow, which fails OPEN and loud (gate inert). This is bounded and loud, so it is not a correctness break. It could be narrowed by also mirroring each slot's owner atomically and listing only the follower's own forms. That brings back the one-lap lag on a re-acquire, so it is a trade-off. Backlog." Reviewer's reasoning: bounded and loud, a trade-off against the re-acquire lag. Code: `apmf/SpellAllowList.cpp` `PublishSpellAllowList`, `cast/Direct.cpp` `ConcProxyForms`. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B155 (SEV-4) -- animheal-p0: a ConcProxy form minted between publishes is unlisted for up to one service lap
Raised against 22d735d (same review), 2026-09-29. Reviewer's finding (verbatim): "a form minted between publishes is unlisted for up to one service lap. The form is minted on main at the stream's first beat. The follower's next combat service lap (`NoteGambits` runs every combat service tick, and `selectList` dedups) lists it. During that window a d41ed43+ Harbinger denies the channel's CheckCast and interrupts it. This happens once per slot per session, is documented in MAP, and [heal-obs] would show it as `channel=stopped stream=live` (loud). Acceptable. Note it for the field read so the first stream after a load is not mistaken for a new defect." Reviewer's reasoning: once per slot per session, documented, loud. Field read (CORRECTED 2026-09-29, `fix/mfo-field-0929` review): the first proxy stream after a load may show `channel=stopped stream=live` once, but on the two `direct` roads that is now an INFO line only (the channel is printed, not judged, because those roads re-arm the effect themselves). Do not look for a warn; it will not come. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B156 (SEV-5) -- animheal-p0: HealObsNote does its work before the rate-limit check
Raised against 22d735d (same review), 2026-09-29. Reviewer's finding (verbatim): "`HealObsNote` does its work before the rate-limit check. It calls Sightline::Check (a lock), two APMFBridge `g_mx` locks and a distance calculation, and only then takes `g_healObsMx` to test the 3 s limit. On the AUTO fan this runs for every recipient on every apply. Move the `g_healObsLast` check first. The cost is small either way." Reviewer's reasoning: small cost either way. Code: `cast/Direct.cpp` `HealObsNote`. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B157 (SEV-5) -- animheal-p0: an offensive Absent still prints `effect applied`
Raised against 22d735d (same review), 2026-09-29. Reviewer's finding (verbatim): "an offensive Absent still prints `effect applied`. On the target conc road the offensive case logs `conc effect NOT ATTACHED` and then falls through to `FORCE-CAST ... -- effect applied, magicka ...`. The comment says \"no longer prints ATTACHED\", which is literally true, but the two lines contradict each other." Reviewer's reasoning: log wording only; the charge is intended (a resisted hit still costs). Code: `cast/Direct.cpp` `ApplyTargetEffect`. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B158 (SEV-5) -- animheal-p0: AUTO concentration applies are in neither g_selfCast nor g_targetCast
Raised against 22d735d (same review), 2026-09-29. Reviewer's finding (verbatim): "AUTO concentration applies (Auto.cpp conc branch) are in neither `g_selfCast` nor `g_targetCast`. [heal-obs] therefore always reports `stream=released` for them, so the `cut` warn can never fire on that road even when its channel is denied. Label it (e.g. `stream=n/a (AUTO)`) or accept it." Reviewer's reasoning: label or accept. Field read: `stream=released` on an `AUTO` [heal-obs] line is expected, not a stream loss. Code: `cast/Auto.cpp` `ApplyEffectFromTo`, `cast/Direct.cpp` `HealObsSweep`. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B159 (SEV-5) -- animheal-p0: a kNoDuration MGEF with an authored duration gives a false Absent
Raised against 22d735d (same review), 2026-09-29. Reviewer's finding (verbatim): "an MGEF flagged kNoDuration but carrying an authored `effectItem.duration > 0` makes `HasDurationEffect` true while no ActiveEffect persists. The result is a false Absent: free cast plus a warn. This is rare in practice. `HasDurationEffect` could also require that `baseEffect` lacks kNoDuration." Reviewer's reasoning: rare. Code: `cast/Actuation_internal.h` `HasDurationEffect`. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B160 (SEV-5, older comment) -- Auto.cpp's "never launched as a projectile" claim looks wrong for Aimed spells
Raised against 22d735d (same review, inside the SEV-2 finding), 2026-09-29. Reviewer's finding (verbatim): "Separately, `Auto.cpp:302`'s \"never launched as a projectile\" claim looks wrong for Aimed spells. It is an older comment and outside this brief, so it goes to the backlog, not this round." Reviewer's reasoning: `CastSpellImmediate` launches a projectile for an Aimed spell (the SEV-2 fix, `DeliveryAppliesInCall`, relies on exactly that). Code: `cast/Auto.cpp` (~:302 at 22d735d). Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B161 (SEV-4) -- field-0929: an urgent preempt followed by a downstream refusal discards a mid-charge offense
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "commitPreempt precedes some refusals that happen later. These are `RestorationCastDirect`'s main-thread LoS/reach refusal (async), and `ComposedCast::Try` ApmfRefused / `ClaimOffenseCast` refusal after the release. Before, those refusals cost an IDLE incumbent nothing. Now they can discard a MID-CHARGE offense for a heal that then does not land. The PickAlly LoS fix removes the field-observed cause (occluded ally). Record it. A guard is not required now." Reviewer's reasoning: the field-observed cause is already removed by the PickAlly LoS fix. Code: `cast/Hands.cpp` `ResolveCastHand` / `PreemptHand`, `cast/CastOn.cpp`. Surfaced at edit time from: MAP.md §2 cast/ "...but only the charge ALREADY RUNNING".

### MFO-B162 (SEV-4) -- field-0929: IncumbentTargetLost has no LoS test; the heal re-aim is held for the lock's staleness/cap
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "`IncumbentTargetLost` has no LoS test. When an incumbent heal's recipient A goes behind a wall, PickAlly now moves to B. The re-aim is then held while A's lock stands: `mine` needs TargetLost(A), which stays false. Duration is bounded by the lock's staleness or the never-observed cap (~4.8-6.5 s on the claim road; about one staleness window on the direct road). For an in-flight claimed heal at A, the charge at the occluded A is also allowed to finish. Ruling 2 is about SELECTION and is met. Mirroring `CheckWithin(...kHealLosTrustS) == Occluded` into IncumbentTargetLost for heal locks would close the gap. That is a separate change." Reviewer's reasoning: bounded, documented by the author in MAP, a separate change. Code: `cast/Hands.cpp` `IncumbentTargetLost`, `Evaluator.cpp` `PickAlly`. Surfaced at edit time from: MAP.md §2 cast/ "NORMAL HEAL REACH" (the ALLY SELECTOR note).
- **DRAINED by `feat/mfo-animheal-p2` (2026-09-30), pending its tier-A review.** `IncumbentTargetLost` now calls a HEAL lock's recipient lost when it is at full health, beyond `HealInReach` (reach + the 1 s LoS cache) or measured `Occluded` within `kHealLosTrustSec` (3 s, the same trust as PickAlly's `kHealLosTrustS`) -- the verbatim fix shape plus the reach half. Offense locks keep the old three tests.

### MFO-B163 (SEV-4) -- field-0929: `hpRose` (0.5 pt) can be met by natural regen
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "`hpRose` (0.5 pt) can be met by natural regen over a multi-second sweep delay and mark an unattached lasting heal as landed. The apply-time NOT ATTACHED line still fires, so this is partial." Reviewer's reasoning: partial, the apply-time line still fires. Code: `cast/Direct.cpp` `HealObsSweep` verdict. Surfaced at edit time from: MAP.md §2 cast/ "[heal-obs] + APPLY READ-BACK".

### MFO-B164 (SEV-5) -- field-0929: drain skew on the charge boundary
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "`lastObservedAt` is the DRAIN time, not the engine fire time. A fire that happened up to ~one pump BEFORE the wait began can be drained AFTER `waitSince`. It then counts as \"fired since the wait\", and the charge that started right after it (the one the protection was for) is preempted about 0-133 ms into it. The lost work is small. Note it in MAP if you like." Reviewer's reasoning: the lost work is small. Code: `ComposedCast.cpp` `ObservedFiring` / `NoteObservedCast`, `cast/Hands.cpp` `CanPreemptHand`. Surfaced at edit time from: MAP.md §2 cast/ "...but only the charge ALREADY RUNNING".

### MFO-B165 (SEV-5) -- field-0929: SpellHealsHealth on a hostile concentration spell can compute urgentHeal at a foe
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "`SpellHealsHealth` = any non-hostile, non-detrimental Health effect. For a CONCENTRATION spell, `offenseSpell` is forced false (selfOrConc), so a mixed hostile concentration spell with a benign self-heal effect (modded) aimed at a hurt FOE would compute `urgentHeal = true` with the foe as \"recipient\". Rare. Gating on `ClassifySpell(spell) != Offense` closes it." Reviewer's reasoning: rare, modded spells only. Code: `cast/CastOn.cpp` urgentHeal decision. Surfaced at edit time from: MAP.md §2 cast/ "...but only the charge ALREADY RUNNING".

### MFO-B166 (SEV-5) -- field-0929: Sightline per-pair throttle total-cost wording
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's finding (verbatim): "TOTAL main-thread measurement is no longer bounded per viewer, though. It is now about (union of distinct targets per viewer) / 0.3 s, each costing HasLineOfSight plus, on CLEAR, the custom ray. The addition is the ally batch plus AUTO targets, at most ~2x the old worst case. The comment \"the cost per pair is unchanged\" is true, but it is not the whole story (SEV-5). Worth one frame-time glance in the field with a big party. It is not a blocker." Reviewer's reasoning: not a blocker; one frame-time glance in the field. Code: `Sightline.cpp` `Want`. Surfaced at edit time from: MAP.md Sightline "Repost throttle is PER (viewer, target) PAIR".

### MFO-B167 (SEV-5, FIXED in the same commit that recorded it) -- field-0929: five stale doc/comment lines
Raised against 8932655 (`fix/mfo-field-0929`, tier-A review), 2026-09-29. Reviewer's findings (verbatim): "B155 field-read text (5); Excursion.cpp gait comment; STATUS gait text (F7); probe \"no engine call\" wording; EnsureMcmDefaults \"kDataLoaded\" comment." Details (verbatim): "the Docs/REVIEW-BACKLOG.md MFO-B155 \"Field read\" still says the shape shows as `channel=stopped stream=live (loud)`. It is now info-level only on the direct roads."; "the `apmf/Excursion.cpp:420-421` comment still says the setting \"= the engine's PreferredSpeed Walk/Jog/Run/FastWalk\""; "Docs/STATUS.md:212-214 still describes the gait as \"(not working yet)\" ... The legacy road's (Harbinger absent) gait proof is still not on record per STATUS. Either update STATUS or add the caveat."; "MAP says \"position deltas, no engine call\", but it makes the three reads above."; "the header comment at `:406-412` still says \"Runs ONCE at load (kDataLoaded)\". It runs at kInputLoaded (plugin.cpp:348) plus kDataLoaded." Reviewer's reasoning: doc drift only. Resolution: all five corrected in the fix round on `fix/mfo-field-0929` (B155 field read, Excursion.cpp gait comment, STATUS gait caveat, MAP stall-probe engine reads, Config.cpp header comment). Nothing open.

### MFO-B169 (SEV-5) -- MAP.md Evaluator file:line nav drift
Raised against 1571d55 (`feat/mfo-foe-multiple`, tier-B review), 2026-09-30. Reviewer's finding (verbatim): "MAP.md Evaluator.cpp cites `Evaluate (:436)` and `ChaseRadius ... (:203)`, and other sections cite `PickAlly:476`, `Evaluator.cpp:320/330/254`. These were already stale on main (ChaseRadius is :301 on main, PickAlly :482). This commit adds 15 lines above PickFoe's body and ~55 inside it (ChaseRadius -> :316, PickAlly -> :549, Sightline::Want -> :504) without refreshing them. Pre-existing staleness, so SEV-5." Reviewer's reasoning: pre-existing drift. Partly refreshed in the chore commit (Evaluate, ChaseRadius, PickAlly, kHealLosTrustS, Sightline sites, group scan); other Evaluator.cpp cites in MAP.md (`:176`, `:222`, `:81`, `:397-409`) were not re-verified and stay open.

### MFO-B170 (SEV-5, FIXED in the same commit that recorded it) -- Foe: Multiple within: comment and CHANGELOG wording
Raised against 1571d55 (`feat/mfo-foe-multiple`, tier-B review), 2026-09-30. SEV-5-1 (verbatim): "pre-pass comment misdescribes the existing scan ... The main scan right below calls `Is3DLoaded`, `IsHostileToActor`, `GetLevel`, `FoeIsCaster`, `GetEquippedObject`, `Sightline::Check` etc. INSIDE `combatGroup->lock`." SEV-5-2 (verbatim): "'If two tie, they pick the one closest to them.' 'They' should mean the follower, but it reads as the two tied foes." Resolution: `Evaluator.cpp` comment reworded (no code change), CHANGELOG bullet reworded.

### MFO-B172 (SEV-4, ESCALATED to SEV-3 as round 3 R3-2 and FIXED in 130c132) -- animheal-p2: the kNeverFired repair shape WARNs on normal states (round 2 R2-6; shadow R2-8)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, Opus), 2026-09-30. Reviewer's finding (verbatim): "HealClaimNeedsRepair's kNeverFired is "not fired in the LAST 4 s and not re-locked in the last 4 s", not "never fired". It WARNs, with the text "not fired within the never-observed bound", on normal states: an FF heal that fired inside the grace and stays in hand, or a kept concentration heal whose channel ended while the rule still holds. Principle 8: this is WARN on non-news. Rename the shape and word the line as "no fire in the last N s". Keep WARN only when the claim has never been observed at all (`ObservedFiring(..., 0)` false)." The shadow reviewer raised the same shape (SEV-5, verbatim): "kNeverFired WARN text ("not fired within the never-observed bound") can fire for a concentration heal that DID fire >4 s ago, has since ended its channel and is still in hand (F2 keeps it there): ObservedFiring is "recent" only (`ComposedCast.cpp:~808`). Benign re-Prepare (AlreadyReady) plus a misleading WARN every 5 s per follower." Reviewer's reasoning: log level/wording only (principle 8), no behaviour change; deferred under rule 9. Round 3 note: the repair lap no longer re-stamps `lastSeen` (R2-3), so "not re-locked in the last 4 s" now reads "not fired in the last 4 s since the claim". Code: `cast/Hands.cpp` `HealClaimNeedsRepair`, `LogHealRepair`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD". **Round 3 (93b1340, Opus R3-2, SEV-3, carve-out b):** with `lastSeen` frozen at claim time (R2-3), the WARN fired about once per cast in the gap between the re-equip and the next charge, which corrupted the STATUS 6b field read. Resolution (130c132): kNeverFired (WARN) only when `ObservedFiring(.., 0)` (the latch) is false; a claim that has fired and sits in hand between casts logs a DEBUG `kBetweenCasts` shape. **Round 4 (bf24a34, Opus spot 2, SEV-4, fixed in 1c298f4):** the latch was fight-wide, so a re-claim after FacetExpiry / TTL / a refusal could inherit an earlier fire and hide behind the DEBUG shape (principle 7); the never-fired test now reads `ObservedFiring` over `now - lastSeen` (per claim, IncumbentHealCastDone's form).

### MFO-B173 (SEV-4, FIXED in 3a42371) -- animheal-p2: party-OOC teardown ends a Logistics-made heal claim every service (round 2 R2-7)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, Opus), 2026-09-30. Reviewer's finding (verbatim): "the party-OOC teardown now calls `End` on every party-OOC service (Scheduler.cpp:919, which runs every tick from the second one on). Round 1's F7 path (the OOC Logistics `CastAuto` -> `CastOn` claim road while a combat controller outlives party combat) then claims in Logistics and is released by the next service's teardown. That is claim/Release churn to Harbinger at service rate, plus EquipSpell/DeselectSpell (Tick erased `g_equipClock`, so the grace does not hold the deselect), bounded by the controller's lifetime. Closing F7 (gate CastAuto's claim branch to the combat table) closes this." Reviewer's reasoning: bounded by the combat controller's lifetime; closes with MFO-B175 (round-1 F7). Code: `Scheduler.cpp` party-OOC teardown, `cast/Auto.cpp` claim branch. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**REOPENED (Opus review of 141c480, F2):** B175's gate closes the Logistics `CastAuto` claim branch, but OOC `CastAuto` still reaches the claim through `CastSelfDirect` / `CastTargetDirect` -> `ChooseHealRoad` -> `Try` during a combat-controller flap (`cast/DirectSelf.cpp:102`, `cast/DirectTarget.cpp:138`), and the party-OOC teardown (`Scheduler.cpp:925`) Ends it the next service. Still bounded by the controller's lifetime. Needs a gate on those two direct entry points or a ruling.
**FIXED in 3a42371** (branch `fix/mfo-heal-open-items`): `CastSelfDirect` / `CastTargetDirect` ask `ChooseHealRoad` only on the combat table (`g_firingRule != kNoRule`, the B175 test), so the out-of-combat caller never mints a claim the party-OOC teardown ends next service. While a combat heal claim still stands (the first party-OOC service), the OOC heal WAITS (`OocHealWaitsForClaim`, `cast/Direct.cpp`: Declined, transparent to Logistics and CastAuto, a 5 s `[heal] ... WAITS` line) instead of streaming beside it. The combat-substrate OOC animation stays future work.

### MFO-B174 (SEV-5) -- animheal-p2: posted DeselectSpell can take back a fresh same-spell equip; revert residue (round 2 R2-8; shadow R2-4)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, Opus), 2026-09-30. Reviewer's finding (verbatim): "the posted deselect re-resolves by FormID and tests "still ours" by spell identity only. If the worker re-equips the SAME spell before main drains the post, the stale deselect takes back the fresh equip. After a fire the 4 s cooldown blocks that, and after an `End` the next claim lap is at least one service away, so it needs a main-thread stall longer than a service period. A post queued across a revert/load acts on the new world's actor. A kept concentration heal whose claim ends by a path other than `End` (APMF's sweep, or a refusal) stays in hand until !castSeen or combat end. HealClaimNeedsRepair's function-static `s_log` is never cleared on revert (the same as round-1 F12)." The shadow reviewer graded the same race SEV-4 (verbatim): "`Loadout.cpp:~477-487`: `ReleaseSpell` now erases `g_mfoSpell` on the worker and POSTS the deselect. The re-resolve on main checks only `GetEquippedObject(true) == sp`. - If the worker re-Prepares the same spell before the post drains (stream-cap release -> `ReleaseOwnHealClaim` -> End -> ReleaseSpellIf, `Hands.cpp:751`, then the next lap's Prepare; or `!castSeen` release then the rule returns), the late deselect strips the fresh equip and leaves `g_mfoSpell` set with an empty hand. - Window is one frame or main-thread hitch, so low probability. For heal claims the repair self-heals it (out of hand -> Prepare). For offense FF there is no repair. Fix shape: capture a generation/epoch of `g_mfoSpell` or re-check main-side that no newer equip stamp exists." Reviewer's reasoning: needs a main-thread stall longer than a service period; for heal claims the repair re-equips. Round 3 note: the stream-cap release no longer deselects (R2-5), which removes one of the shadow's two named triggers. Code: `Loadout.cpp` `ReleaseSpell` takeBack. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
left open (coordinator ruling): the fix is a cross-thread equip generation (tier-A threading) for a race that needs a main-thread stall longer than a service period.

### MFO-B175 (SEV-4, FIXED in 141c480) -- animheal-p2: CastOn is reachable from the out-of-combat Logistics table (round 1 F7)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "`logistics/Service.cpp:1543` -> `CastAuto` -> (claim road) -> `CastOn`, with `g_firingRule = kNoRule`, `g_firingAllyThreshold = -1` and `a_rangeGate = false`. This happens whenever the OOC table services a follower who still has a CombatController (the flap the DirectSelf/DirectTarget comments name). The lock is stamped `owningRule = kNoRule`, which cannot preempt anything and is itself preemptable. Logistics reads CastOn's OPAQUE "composed cast: AI deciding" as not-acted and keeps scanning. Nothing unsafe was found, but it breaks the invariant stated at `Actuation_internal.h:201-205` ("every reader is reached only from Fire()"). Either gate CastAuto's claim branch to the combat table or update that note and MAP." Reviewer's reasoning: nothing unsafe found; an invariant note (`Actuation_internal.h`, g_firingRule "every reader is reached only from Fire()") is broken. Round 3 note: the new equip-hold gate compares `LeftHoldRule` with `g_firingRule`, so an OOC (kNoRule) heal never takes a hand from any ranked equip hold. Code: `logistics/Service.cpp` -> `cast/Auto.cpp` `CastAuto` -> `CastOn`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `cast/Auto.cpp` claim branch gated on `g_firingRule != kNoRule` (Fire() only); OOC Logistics heals stay on the direct road, and the `Actuation_internal.h` "reached only from Fire()" invariant holds again.

### MFO-B176 (SEV-4, FIXED in 3a42371) -- animheal-p2: a combat-controller flap briefly runs both heal roads on one actor (round 1 F8)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "Direct -> claim: the direct stream (`g_selfCast` / `g_targetCast`) is not ended when the road switches. It lapses on its reconcile's stale window (>= 2 s, larger for big parties), and its sustained effect and MFO's magicka billing overlap the new claim. Claim -> direct is clean (`End` in `ChooseHealRoad`). This is bounded, and Harbinger no longer denies the instant caster (d41ed43), so it is not the 09-21 collision." Reviewer's reasoning: bounded, not the 09-21 collision. Code: `ComposedCast.cpp` `ChooseHealRoad`, `cast/Direct*.cpp` streams. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
needs ruling (assessment): the code's "ONE ROAD PER ACTOR" contract (`ChooseHealRoad`) already decides it: end the direct stream on a road switch. Needs a tier-A brief because `SelfCastEndActor` / `TargetCastEndActor` dispel and interrupt on the main thread.
**FIXED in 3a42371**: `ChooseHealRoad` ends any Heal-kind direct stream (self and on-target) when it answers `Claim` (`Actuation::EndDirectHealStreams`, `cast/Direct.cpp`), with the streams' own switch end: `PostSettle`, then `SelfCastEndActor` / `TargetCastEndActor` (dispel + kInstant `InterruptCast` + ConcProxy free, all inside `MainThread::Post`, FormIDs re-resolved there), then the registry erase. Log `[heal] ... RELEASE (road switch: ...)`. Non-heal streams (wards) are untouched.

### MFO-B177 (SEV-4, FIXED in 3a42371) -- animheal-p2: the stream-cap release lap is transparent (round 1 F9)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "Check (2): no interrupt loop and no double channel. The release is `End` + `CastLock{}`, and the next lap re-claims through the normal path (Prepare answers AlreadyReady). If Harbinger's Release does not stop the running channel, the cap only re-mints the claim and seat 0x07 still stops at full. The side effect: on the release lap a lower-ranked LEFT-only offense can take the freed left hand, and the heal preempts it back the next lap (urgent heal, mid-charge). That throws away one offense claim per 8-15 s cap cycle, plus the multi-second re-claim gap in the heal." Reviewer's reasoning: no loop, no double channel; the cost is one offense claim per cap cycle. Round 3 note: the cap release now keeps the spell in hand (R2-5), so the premise "Prepare answers AlreadyReady" holds again. Code: `cast/CastOn.cpp` in-flight branch stream cap. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
needs ruling (assessment, accepted by the coordinator): marth's "Gambit order wins" plus the carried-rank / no-tenure rule mean a higher-ranked heal must not lose the left hand to a lower-ranked offense on the cap release lap. Not local: the release clears the left lock (`ReleaseOwnHealClaim`), so keeping the rank across the re-stream touches the hand-lock staleness rules. Needs its own brief.
**FIXED in 3a42371**: the stream-cap release (`ReleaseOwnHealClaim(.., a_keepSpell=true)`) keeps the LEFT lock and its `owningRule` for the gap to the re-claim, stamped `CastLock::restreamAt` (in memory; channel cap reset). `CastLockLive` answers live for `FacetExpiry()` from that stamp, so a lower rule is held off and a higher rule preempts as usual; `lastSeen` is untouched (IncumbentHealCastDone / repair windows unchanged); `HoldCastLock` clears it on the re-claim; the owning rule's own re-aim is `mine` in the gap; a no-claim `ReleaseOwnHealClaim` from the owning rule drops it at once. The equip side ranks the gap (`HealRestreamRule` in `HealClaimTakesLeftFrom` / `LeftReservedForCast`) and `HealTakesLeft` stays true (Loadout's shield / two-hander give-back defer one more lap).

### MFO-B178 (SEV-5, FIXED in 141c480) -- animheal-p2: IncumbentTargetLost's Occluded test reads an un-Wanted verdict (round 1 F10)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "`IncumbentTargetLost`'s new `Occluded` test reads a Sightline verdict that nobody `Want`s for the incumbent once the pick moves on. CastOn wants only the current pick. CastAuto's probe wants only the candidates that beat the running lowest in visit order. The verdict ages to Unknown, which reads as "not lost". Bounded by the hold's 4 s never-observed feed cap." Code: `cast/Hands.cpp` `IncumbentTargetLost`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `cast/Hands.cpp` `IncumbentTargetLost` calls `Sightline::Want(follower, {incumbent})` for a heal lock before reading the verdict.

### MFO-B179 (SEV-5, FIXED in 141c480) -- animheal-p2: ChooseHealRoad's no-controller End leaves the LEFT lock (round 1 F11)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "`ChooseHealRoad`'s no-controller `End` leaves the LEFT CastLock naming the released claim, so it holds a different-spell direct heal for <= FacetExpiry plus F8's in-flight read. Bounded. Otherwise the release is correct (check 5): no controller can serve anyone's heal claim, so releasing it whichever rule owns it is right, and the log line is rate-limited." Code: `ComposedCast.cpp` `ChooseHealRoad`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `ComposedCast.cpp` ChooseHealRoad's no-controller branch calls the new `Actuation::ClearLeftCastLockIf(fid, standing)` after `End` (left lock only).
**Review F4:** the kill-switch branch of `ChooseHealRoad` has the same shape and now also calls `ClearLeftCastLockIf`.

### MFO-B180 (SEV-5) -- animheal-p2: static log maps not cleared on revert; kHealLosTrustSec duplicate; HealObsClaimLap ordering (round 1 F12)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "`ReleaseOwnHealClaim`'s function-static `s_log` is never cleared on revert. `kHealLosTrustSec` duplicates Evaluator's `kHealLosTrustS` with only a "keep equal" comment (a static_assert needs a shared header). `HealObsClaimLap` can stamp "HP before" after an instant heal landed, when a pump lap is queued ahead of the fire task. That is harmless for FF (not lasting) and only weakens `hpRose` for concentration." Round 3 note: `LogHealRepair`'s and the new equip-hold / heal-outrank log dedup maps are function-static in the same way. Code: `cast/Hands.cpp`, `cast/CastOn.cpp`, `cast/Equip.cpp`, `cast/HealObs.cpp`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**PARTLY FIXED in 141c480**: the function-static log maps (ReleaseOwnHealClaim, LogHealRepair, equip-hold, heal-outrank) are namespace-scope and cleared on revert. STILL OPEN, needs ruling: the `kHealLosTrustSec` / `kHealLosTrustS` duplicate (a static_assert needs a shared header, a new file or move) and `HealObsClaimLap`'s stamp ordering. Left open (coordinator ruling): the `kHealLosTrustSec` duplicate needs a shared header, a new file.

### MFO-B181 (SEV-5, FIXED in 3a42371; REOPENED by marth 2026-09-30) -- animheal-p2: toggle race between ChooseHealRoad and Try (round 1 F13)
Raised against 02b7dbc (`feat/mfo-animheal-p2`, tier-A review round 1), 2026-09-30. Reviewer's finding (verbatim): "race only: the toggle flipping OFF between `ChooseHealRoad` and `Try` on one lap returns `NotApplicable` inside the `healClaim` branch. That reaches CastOn's AI-grace / ForceCast / silent `CastSpellImmediate` hybrid with MFO's magicka deduction. It is the documented degrade, not a post-refusal fallback. Listed for completeness." Code: `cast/CastOn.cpp` heal road, `ComposedCast.cpp` `Try`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
WAS closed as by-design (coordinator ruling after the Opus review): the toggle flipping between two calls on one lap reaches the documented degrade, not a post-refusal fallback.
REOPENED (marth 2026-09-30): "closing b181 is wrong, nothing will use the instant road in the finished build (aside from where 1.5 needs it). It can be the normal fallback, but any causes of fallback usage are problems to fix." Every path that reaches the instant/direct road in combat (outside 1.5.97 AE-only seats) is a defect to fix, not a by-design close. Fix shape to decide in its own brief: read the toggle once per lap so ChooseHealRoad and Try see the same value.
**FIXED in 3a42371**: one read cannot span the lap (the toggle is read again inside `APMFBridge::ClaimHealCast` / `HealCastClaimSupported`), so the equivalent: on a lap `ChooseHealRoad` chose `Claim`, a heal ask that did not happen (`bEquipToCast` read OFF) or answered `NotApplicable` (only a mid-lap kill-switch flip can) is a loud `[heal] ... switched off between the road choice and the claim` WARN and a transparent no-cast in all three callers (`cast/CastOn.cpp` after the composed block, `cast/DirectSelf.cpp`, `cast/DirectTarget.cpp`). Never the instant road on a claim lap; the next lap's `ChooseHealRoad` releases and labels.

### MFO-B182 (SEV-4) -- animheal-p2: bHealAnimMigrated marker survival through MCM Helper not verified (round 2 shadow R2-6)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, shadow reviewer), 2026-09-30. Reviewer's finding (verbatim): "`Config.cpp:576-672` logic is sound (below), but `bHealAnimMigrated` is not in `config.json` (audit_mcm STORE_ALLOWLIST, `tools/audit_mcm.py`). If MCM Helper rewrites the store from its registered controls only, the marker vanishes after the first MCM visit and the NEXT launch re-flips a player's deliberate OFF (0 -> 1) every time. Field check: open the MCM, change any toggle, cat the store, confirm the marker line survives. Every prior appended key was a control, so there is no precedent in this repo for a non-control key." Reviewer's reasoning: needs one field check; the Opus reviewer (check d) states the marker is a registered ModSetting. Code: `Config.cpp` EnsureMcmDefaults migration, `tools/audit_mcm.py`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
left open (coordinator ruling): needs one field check of the MCM store, not code-decidable.

### MFO-B183 (SEV-5, FIXED in 141c480) -- animheal-p2: HealChannelCapped's doc block sits above HealClaimNeedsRepair (round 2 shadow R2-7)
Raised against 96865ae (`feat/mfo-animheal-p2`, tier-A review round 2, shadow reviewer), 2026-09-30. Reviewer's finding (verbatim): "doc misplacement. `Hands.cpp:761-771` (the "HealChannelCapped. Read the LEFT lock's spell..." block) now sits above the NEW `HealClaimNeedsRepair`; `HealChannelCapped` (`:836`) lost its comment. Move the block back." Code: `cast/Hands.cpp`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): doc block moved above `HealChannelCapped`; `HealClaimNeedsRepair` has its own comment (`cast/Hands.cpp`).

### MFO-B185 (SEV-4) -- iBoardKey / iHudKey: gamepad and mouse keymap codes are silently dropped
Raised against 2d8f16c (`feat/mfo-ui-hotkeys`, tier-B review), 2026-09-30. Reviewer's finding (verbatim from the review round as relayed by the coordinator): "gamepad and mouse keymap codes (256+) are clamped to 255 and filtered to kKeyboard, so they're silently dropped. This conflicts with the controller-support family rule. The power still works on a controller." Reasoning: the MCM keymap control can capture mouse and gamepad codes, but `setI(..., 0, 255)` and the `INPUT_DEVICE::kKeyboard` filter mean such a binding does nothing. Deferred because the Field Orders power already opens the board on a controller. Fix when drained: map MCM Helper's device-offset codes to the device enum, and accept kMouse/kGamepad in both hotkey blocks.

### MFO-B186 (SEV-4, FIXED in 141c480) -- animheal-p2: the shield-by-perks top-up is not a ledger hold, so a lower-ranked heal still displaces it (round 3 R3-4)
Raised against 93b1340 (`feat/mfo-animheal-p2`, tier-A review round 3, Opus), 2026-09-30. Reviewer's finding (verbatim, from the round-3 ledger): "R3-4 (SEV-4): shield-by-perks top-up outside rank path (spot 4)." Stated by the coordinator: the shield-by-perks top-up is not a ledger hold, so a heal ranked below the equip rule still displaces that shield, which contradicts "gambit order wins"; marth will be told. Author spot 4 (round 3): the equip gambit's shield top-up (`EquipShieldOnMain`) is a plain equip with no `ForcedHold` entry, so `LeftHoldRule` never sees it and the heal's `Prepare` displaces it as the follower's own gear. Reviewer's reasoning: a marth call on whether the shield counts as the gambit's hold. Code: `cast/Equip.cpp` EquipWeapon top-up, `LeftHoldRule`. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `cast/Equip.cpp` `g_shieldRank` (in-memory, `g_forcedMx`, not co-saved) stamped by `EquipShieldOnMain` and re-stamped on satisfied laps; `LeftHoldRule` returns it while that shield is in the left hand, so `CastOn`'s rank gate keeps a lower-ranked heal off it. Erased in `ReleaseForcedWeapon` / `ClearForcedWeapons`.
**Review F1 (SEV-3, fixed in the follow-up commit):** a top-up over the AI's own one-hander has no ledger entry, so `ReconcileForcedWeapon` returned at "nothing forced" and never erased the rank. It now applies the release decision (kill-switch off, or condition known false for a one-hander) to `g_shieldRank` before that return. `LeftHoldRule` copies the record under the lock and reads the hand outside it (F5).

### MFO-B187 (SEV-5, FIXED in 141c480) -- animheal-p2: the yield return nulls both displaced items (stale-ledger edge, round 3 R3-5)
Raised against 93b1340 (`feat/mfo-animheal-p2`, tier-A review round 3, Opus), 2026-09-30. Reviewer's finding (verbatim, from the round-3 ledger): "R3-5 SEV-5 misc." Stated by the coordinator: the stale-ledger yield edge at `Loadout.cpp:464`. Author's note (round 2 code): `Prepare` nulls `willDisplaceLeft` AND `willStowWeapon` whenever the yield returns true, so a ledger left hold that is NOT in the hand, met while the follower's own two-hander is in the right, would drop that two-hander's stow debt. `ForcedHold.left` and a two-hander `right` hold are mutually exclusive by construction (EquipWeapon clears `.left` when it holds a two-hander), so it needs a stray ledger entry. Code: `Loadout.cpp` Prepare (the `a_yieldLeft` line). Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `LeftHandYield` reports the released left/right hold objects (`Loadout.h`); `Loadout.cpp` Prepare nulls `willDisplaceLeft` / `willStowWeapon` only when the released object IS that item. Callbacks in `cast/CastOn.cpp`, `YieldForcedLeftHand` in `cast/Equip.cpp`.

### MFO-B188 (SEV-5, FIXED in 141c480) -- animheal-p2: s_heldLog / s_outrankLog function-static dedup maps survive revert (round 3 R3-5, the MFO-B180 class)
Raised against 93b1340 (`feat/mfo-animheal-p2`, tier-A review round 3, Opus), 2026-09-30. Reviewer's finding (verbatim, from the round-3 ledger): "R3-5 SEV-5 misc." Stated by the coordinator: the `s_heldLog` / `s_outrankLog` revert residue (the B180 class). Log dedup only; a stale stamp can suppress one line for up to 5 s after a load. Code: `cast/Equip.cpp` EquipWeapon HELD OFF log, `cast/CastOn.cpp` heal-road outrank log. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `s_heldLog` / `s_outrankLog` are namespace-scope (`g_heldOffLog` / `g_healOutrankLog`, `Actuation_internal.h`) and cleared in `ClearCastLocks`.

### MFO-B189 (SEV-4) -- animheal-p2: a claim that never fires holds HealTakesLeft for the whole claim (round 3, author spot 2; F1 ruling)
Raised against 93b1340 (`feat/mfo-animheal-p2`, tier-A review round 3, Opus), 2026-09-30 as author uncertain spot 2 (verbatim): "`HealTakesLeft` uses the `[cfc]` watch's `ObservedFiring` over a window of `fCastCooldown`. With `fCastCooldown <= 0` the heal counts as pending for the whole claim. A claim that never fires (the census-zero case) keeps the shield suspended until the claim ends." Recorded with the F1 ruling (marth 2026-09-30: the repair loop IS the policy, "Once equipped it's never failed"): a never-firing claim is kept and repaired while its rule wins, so MFO's own left-hand automation stays suspended for it; the never-fired WARN names the case in the log. Code: `ComposedCast.cpp` HealTakesLeft, `Loadout.cpp` OnFollowerHit / Tick. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD". **Round 4 (1c298f4):** the never-fired shape is now per claim (`ObservedFiring` over `now - lastSeen`, MFO-B172), and a FOREIGN spell held on the "proxy still 0" guess is Prepared once past the bound. `HealTakesLeft` itself still reads its `fCastCooldown` window, so a claim that never fires still holds the suspension for the whole claim, as recorded above.
left as recorded (F1 ruling: the repair loop IS the policy); no code change.

### MFO-B190 (SEV-4, FIXED in 141c480) -- animheal-p2: the older [cfc] silent-claim warning reads the fight-wide observed latch (round 4)
Raised against bf24a34 (`feat/mfo-animheal-p2`, tier-A review round 4, Opus), 2026-09-30. Stated by the coordinator (from the round-4 verdict): the never-fired latch must be per claim; "Leave the older [cfc] silent-claim warning alone (it predates this branch), but add a backlog line noting it shares the fight-wide latch." The [cfc] watch's `observed` flag is not cleared when the same spell is re-armed (`WatchArmed`'s no-reset branch, ComposedCast.h ObservedFiring doc: "the raw latch means 'this spell fired once on this hand this fight'"), so a re-claim of a spell that fired earlier in the fight is not reported as silent. Pre-existing, outside this branch's scope; the repair's never-fired WARN was moved to a per-claim window in 1c298f4. Code: `ComposedCast.cpp` WatchArmed / the silent-claim warning. Surfaced at edit time from: MAP.md §2 cast/ "ANIMATED HEAL CLAIM ROAD".
**FIXED in 141c480** (drain round, branch `fix/mfo-animheal-backlog-drain`): `ComposedCast.cpp` `WatchArmed` tests `observed && lastObservedAt >= Actuation::CastLockClaimStamp(...)` (the lock's `lastSeen`); callers with no cast lock (the direct roads' arms) keep the latch, there being no claim stamp to measure from.
**Review F3 (fixed in the follow-up commit):** the grace and the printed age now measure from `max(w.since, claimStamp)`.

### MFO-B192 (SEV-4, FIXED in fix/mfo-heal-field0930) -- EndDirectHealStreams ran at ChooseHealRoad's Claim answer, not at the claim mint
Raised against 3a42371 (`fix/mfo-heal-open-items`, tier-A review, Opus, finding F1), 2026-09-30. Reviewer's finding (verbatim from the coordinator's relay): "EndDirectHealStreams runs at ChooseHealRoad's Claim answer, not at the claim mint. A held-off lower rule can end a higher DirectNoSeat stream; only reachable with bCastSelf ON. The fix is to end streams on Try()'s Claimed return." **FIXED** in `fix/mfo-heal-field0930`: the call moved from `ChooseHealRoad` to `ComposedCast::Try`'s `Claimed` return (`ComposedCast.cpp`).

### MFO-B193 (SEV-5) -- B173's OOC wait covers the direct heal streams only
Raised against 3a42371 (`fix/mfo-heal-open-items`, tier-A review, Opus, finding F3), 2026-09-30. Reviewer's finding (verbatim from the coordinator's relay): "the B173 OOC wait covers streams only. Logistics' immediate branch and CastAuto's fan use CastSpellImmediate directly, so there is a one-service overlap." Left open: one service period of overlap between a standing combat claim and an OOC instant cast. Code: `logistics/Service.cpp` immediate branch, `cast/Auto.cpp` fan.

### MFO-B194 (SEV-5) -- B181's flip lap runs after commitPreempt(); the displaced incumbent is not restored
Raised against 3a42371 (`fix/mfo-heal-open-items`, tier-A review, Opus, finding F4), 2026-09-30. Reviewer's finding (verbatim from the coordinator's relay): "B181's flip lap runs after commitPreempt(). The displaced incumbent is not restored; not a regression." Left open. Code: `cast/CastOn.cpp` commitPreempt and the NotApplicable arm.

### MFO-B195 (SEV-5) -- no field log of the caster state value that proves kReady is the held-charge state
Raised against 8f915c0 (`fix/mfo-heal-field0930`, Opus review), 2026-09-30. `CastChargedWaitingOnHand` and the unsighted-charge release treat `MagicCaster::State::kReady` (3) as "charged and held". The fork enum and APMF's `CasterStateName` names disagree except on 3, and no field log yet records the state value of a held charge. Left open until a `[cfc]`/state capture shows 3 on a held charge. Code: `cast/Hands.cpp` `CastChargedWaitingOnHand`, `cast/CastObserve.cpp` `CasterStateName`.

### MFO-B196 (SEV-5) -- CanPreemptHand makes a non-urgent heal wait on a kReady offense incumbent
Raised against 8f915c0 (`fix/mfo-heal-field0930`, Opus review), 2026-09-30. A heal that is not urgent still waits behind an offense incumbent that is `kReady`, because the F12 yield and `CanPreemptHand` only take the hand at once for the urgent case. Left open; revisit if a field log shows a non-urgent heal starved by a charged offense hold. Code: `cast/Hands.cpp` `CanPreemptHand`, `ComposedCast.cpp` Try F12.

### MFO-B197 (SEV-5) -- cost of EndDirectHealStreams on Claimed refresh laps
Raised against 8f915c0 (`fix/mfo-heal-field0930`, Opus review), 2026-09-30. `EndDirectHealStreams` runs at `Try()`'s `Claimed` return, including refresh laps of a standing claim, so it is called every lap. Not measured; left open until a profile shows it matters. Code: `ComposedCast.cpp` Try `Claimed` return.

### MFO-B198 (SEV-4) -- essential-down heal release churns at the picker
Raised against 6c974fd (`fix/mfo-lifestate-0930`, Sonnet review), 2026-09-30. Reviewer (verbatim): "only kEssentialDown triggers it ... the picker and Hands.cpp are unchanged, and the picker's urgentHeal treats essential-down as 'down'. The ally can therefore be re-picked every lap and released again each time. This is not a re-fire, since no claim is renewed, but it is churn plus a lap-by-lap FailedOther." Fix when drained: skip an essential-down ally at PickAlly / CastAuto pick time (where a heal cannot land), so the release path is not exercised every lap. CHANGELOG wording corrected at merge.

**RESOLVED 2026-10-12 the other way (branch `fix/heal-essential-down`, SHA a25aa6f):** marth ruled a heal on a downed ally is fine and a heal DOES land on an essential-down ally on the DIRECT road (field LoreRim 16:44:15, `[heal-obs] landed=effect`, then UP 0.1 s later), so `RecipientLost` no longer refuses essential-down and there is no release churn to avoid. FIELD WATCH (review SEV-4): the animated CLAIM road landing on a downed recipient is not yet observed; look for `[heal-obs] road=claim ... landed=` with an essential-down recipient, or repeated NeverFiredRelease WARNs naming one.

### MFO-B199 (SEV-5) -- "only measured claim-to-first-charge" overclaims against two later comments
Raised against a9acf09 (`fix/mfo-cleanup-bundle1`, Opus review), 2026-10-01. Reviewer (verbatim): "`native/cast/Hands.cpp:340-342` new text says 'The only measured claim to first CHARGE STATE figures are ~2.5 s (0906 heal) and ~3.6 s (0908 heal)'. That 'only' was copied from the MFO-B6 entry written before 2026-09-22; `native/ComposedCast.cpp:144` ('the measured claim -> first CHARGE STATE latency for an offense cast is 2.3-2.5 s', from the 2026-09-22 deck) and `MAP.md:6329` say the same, contradicting 'only'. The 0922 figure is identical to the 0906 offense claim-to-FIRE figure, so it may be the same mislabelling MFO-B6 describes." Fix: drop "only" in Hands.cpp, and check ComposedCast.cpp:144 / MAP.md:6329 against the 2026-09-22 log for whether that figure is claim-to-charge or claim-to-fire. Comment-only. Surfaced at edit time from: MAP.md Actuation heal hold (MFO-B6 cross-ref).

### MFO-B200 (SEV-5) -- EquipShieldOnMain's '?' name fallback is dead
Raised against a9acf09 (`fix/mfo-cleanup-bundle1`, Opus review), 2026-10-01. Reviewer (verbatim): "`native/cast/Equip.cpp:218-219` `item->GetName() ? item->GetName() : \"?\"` is a dead fallback: in the fork `TESForm::GetName()` returns `\"\"`, never null (`src/RE/T/TESForm.cpp:28-37`). An unnamed shield now logs `''` instead of `'?'` (old code used `TESObjectARMO::GetFullName()`). Same idiom as the existing `nm` lambda at :733. Harmless." Fix: test `*name` instead of the pointer if '?' matters. Log text only.

### MFO-B204 (SEV-5) -- the roster-sweep hold now lasts the whole load screen
Raised against 676b676 (`fix/mfo-logic-bundle2`), Opus review F4, not fixed. Verbatim: "native/Followers.cpp:478-499: the hold window was capped at ~1.6 s (3 sweeps x 532 ms); it now lasts the whole load screen, so the Scheduler keeps ticking a held follower for that long. Memory-safe (g_active stores ActorHandle, .get() nulls a freed actor; tick consumers already null-check), recorded for the longer window."

### MFO-B202 — an expired key is not held during its 2 retry passes, so the swap-up can evict a worn gem for G during the retry
- **Raised:** Opus review of `60d9e66` (`fix/mfo-gems-backlog`), SEV-5, pre-existing.
- **Severity:** SEV-5
- **Finding (verbatim):** When a key expires, stallGate resets it with `st = StuckState{}` (:518), so it is not held during the 2 retry passes; if G's stack has >=2 copies and the item has an empty slot, the swap-up can evict a worn gem for G during that retry. Once per expiry, bounded; same behaviour as socketBackedOff before this commit.
- **Why it was NOT fixed:** pre-existing, bounded to once per expiry, SEV-5 (rule 9). Deferred, not dropped.
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "HELD KEYS" What-breaks.

### MFO-B203 — gem CHOICE review (Opus, 2026-10-01): safe bug fixes DRAINED, design decisions OPEN
- **Raised:** Opus logic review of MFO's gem choice (`agentlogs/review-gem-choice.md`, findings F1-F12), against `origin/main` 0166f75 and MEO a47f99c.
- **Severity:** mixed. F1 and F2 were SEV-2, the rest SEV-4/SEV-5.
- **DRAINED by `fix/mfo-gem-choice-bugs` (2026-10-01), pending its review:** F4 (uid-0 item skips `GetGemDetails`), F1 (tier 2 never admits an off-domain gem through a Conduit, the simple option, because the sibling map would duplicate MEO's catalog and MEO_API carries no theme), F2 and the eviction half of B34/F6 (a support is valued only when its link works, a linked support is protected only while its link works, a linked Conduit and its partner only when the partner is off-domain, partner replacement must keep a working Focus/Echo link), F9 (base vs base magnitude in the swap-up), F10 second half (`WornUid` hand-matched for the weapon capture).
- **OPEN, design decisions for marth:** F3 (gem relevance model: the text heuristic in `GemBonus` misses frost, weapon-skill fortifies, treats `magickadamage` and unarmed as magic), F5 (gear-swap destination and pouch overflow policy, and the `MoveGems` to `UnsocketGem` rework: MoveGems does no eligibility or foreign-enchant check when `toUid` is given, and overflow goes to the shared pouch), F7 (provenance), F8 (default tier, `bMeoAwareGems` is OFF), F10 race (the uid-0 mint race), F11 (VR threading). B34 insert half and B33/B35 still wait.
- **F3 ADDRESSED by `feat/mfo-gem-relevance` (2026-10-12, marth: "MEO exposes gem theme first, then the F3 relevance round"), pending its review:** with MEO ABI v4 tier 2 ranks by MEO's gem class / theme / domain / element bits / support type (`RelevanceV4`, `ScoreV4`, `LinkWorksV4`) against the follower's role and worn gear, level as the cross-family scale; below v4 the heuristic runs unchanged. Perk direction (marth 2026-10-12): a Skill gem is relevant on the follower's perk/equip direction, not the gear in hand; Linear sub-order Health > Stamina > Stamina Rate > Heal Rate > Carry, Magicka > Magicka Rate; the first Health gem beats a used skill. B34: Conduit never used (see MFO-B34). F5, F7, F8 (`bMeoAwareGems` stays OFF), F10 race and F11 stay OPEN here.
- **OPEN, cross-repo:** MEO should expose a gem's theme (and element) in `GemInfo`/`GemDetail`/`LooseGemInfo`. MFO's `IsElementalGid` is a gid list, so MEO's minted `x_` runtime families (calibrated themes, plugin.cpp:985-998, 1346) read as non-elemental and a Focus or Echo linked to one is not valued or guarded. Needs an append-only MEO_API change on MEO's side. **DONE:** MEO ABI v4 `GetGemTheme` (MEO main `9bc497e`), consumed by `feat/mfo-gem-relevance`; on v4 a minted `x_` elemental family links like a catalog one.
- **OPEN, SEV-5:** MEO's "linked" test excludes normal gems with no magic effect and capped player copies (plugin.cpp:1990), which MFO's count of 1 support + 1 normal does not.
- **OPEN, SEV-5:** a second armor Echo on the same actor is inert ("first linked Echo-armor wins", plugin.cpp:7608-7660) but MFO values it +1.
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "GEM CHOICE BUG FIXES".

### MFO-B205 (SEV-5, accepted) -- co-save readers: the FWPN cap literal, and the residual B13 noise-window corner
Raised against ce1b54c (`fix/mfo-cosave-readers`, Opus tier-3 review, verdict MERGE, nothing above SEV-5), 2026-10-01. Reviewer (verbatim, as relayed): "(a) the 4096 literal is tied to kMaxFollowers only by a comment. Harmless either way. Use a shared constant in Serialization.h only when that file is next touched. (b) the residual B13 corner when natural's fraction is in (0.999,1), e.g. natural 20.9995 gives the share 79.0005, ceil(...-1e-3) gives 79, and the actor holds 99.9995. That is the price of the noise window." Reasoning: (a) both sides are 4096 today and the FWPN cap only needs to be at least 2x the follower bound, so drift is harmless until someone lowers it; moving `kMaxFollowers` out of `Serialization.cpp`'s anon namespace is its own change. (b) the 1e-3 window is what makes ceil the identity on float noise around whole shares; a natural within 1e-3 below an integer is the cost. Fix when drained: (a) a shared `kMaxFollowers` in `Serialization.h`, `kMaxForcedWeapons = 2 * kMaxFollowers`, the next time that header is touched; (b) none planned.

### MFO-B216 (SEV-4) -- heal refused for "no line of sight" at 198 units on the OOC road (diagnosis P3)
Raised against 86c9984 (Opus field diagnosis of the 2026-10-01 session, `field1001/diagnosis.md`), deferred from `fix/mfo-heal-starve-retreat`. Verbatim: "The four OOC refusals (E1-E4) are the designed main-thread re-check (`native/cast/CanAct.cpp:228-251`, `RefuseHealApplyOnMain` -> `Sightline::MeasureNow(..., Basis::Own)`), and each is preceded by an `OCCLUDED (own ray)` line a few ms earlier, so the refusal itself is consistent. E1/E2/E4 at 1100-2100 units in a fight are plausibly genuine. E3 (Adelinda, 198 units) is suspect: the ray (`native/Sightline.cpp:103-163`, `CustomRay`) collides on layer `kCharController` from the eye to three target points and calls the pair occluded only if all three hit, so another actor's capsule standing between (player / Cicero) would occlude a teammate at arm's length. The log cannot say what was hit. Note also the pick/refusal disagreement: logistics rule 0 fired on a target the main-thread ray then refused, every time (the pick trusts a cached verdict, the apply re-measures). The refusal is correct behaviour (principle: no heal through walls), the cost is a 3 s refresh lost each time. **Confidence: low** that anything is wrong here." Reviewer's fix shape: only if NEEDS LOCAL 4.3 shows the hit is a teammate capsule, exclude party-member actor capsules from the heal LoS ray (or use a static-geometry layer), never drop the refusal. Settling log: on an `OCCLUDED (own ray)` verdict for a HEAL recipient, log the hit body's owner per sample.
- **Surfaced at edit time from:** MAP.md "ANIMATED HEAL CLAIM ROAD".

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- 9 OOC heal LoS refusals at distances 678-2867u, none short-range, so the 198u case did not recur; needed: an OOC heal refusal under ~300u (logging gap: [los] OCCLUDED lines name no hit body, APMF's own ray).

### MFO-B217 (SEV-5) -- `[bleed]` transition logger prints DOWN for raw life state 9
Raised against 86c9984 (same diagnosis), deferred from `fix/mfo-heal-starve-retreat`. Verbatim: "`[bleed] ... DOWN (?(9) ...) hp 100% ... no MFO heal landed on him while down` (Cicero 08:40:13, 08:56:08; Jesper 08:40:16, 08:40:38): raw life state 9 appears during IdleGive / IdleLockPick (APMF ch.12 lines at the same times). `NoteUnknownLifeState` says "not a down state" but `LifeUp` (`native/cast/CanAct.cpp:88-90`) counts only Alive/Reanimate as up, so the transition logger prints DOWN. Misleading diagnostics, no behavioural effect seen (0 `cannot act` refusals)." Fix shape (tier C): make the unnamed raw value(s) the logger calls DOWN agree with `NoteUnknownLifeState` (treat 9 as up, or stop claiming "not a down state").

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CONFIRMED (still seen) -- MFO.1 12:46:14.716 and 12:55:03.870 print Cicero DOWN '?(9)' at hp 100%.
### MFO-B210 (SEV-5/SEV-4, OPEN) -- ranged kind by perks + ammo: what the first cut does NOT do
Raised by the author of `feat/mfo-ranged-kind-ammo` (ClickUp 86e3940yd), 2026-10-01, against 8f8901b. Round 2 drained the relic items and added the crossbow item-type perk signal. Recorded, not fixed (rule 9):
- **Perk bias is neutral for vanilla keyword perks.** Both vanilla bows and crossbows carry `WeapTypeBow`, so keyword-conditioned Archery perks bias both alike. Round 2 added `kWkCrossbow` and the right-hand `GetEquippedItemType` 7/12 votes, so item-type-conditioned perks do separate them.
- **Ammo pairing is the animation type.** `RangedUsesBolts` = crossbow animation, because a Skyrim WEAP has no ammo field. A modded gun that is bow-animation but fires bolt-flagged ammo (or the reverse) is mismatched. Not disassembly-verified how the engine refuses such a pair.
- **`EquipAuthority.cpp:688` `rightW->IsCrossbow()`** still picks the declared ammo family from the worn weapon's enum. Left alone because it sits inside `RefreshEquipDeclaration` (B63, another branch). Switch it to `RangedUsesBolts(rightW)`.
- **`TradeBridge.cpp:125` `isXbow`** still compares the enum (equivalent today; it has no access to the internal header).
- **Ranged loot/buy weapon compares stay raw damage** (`LootEquipment` `myRangedDmg`, `PlanBuy` ranged), not the perk-biased/ammo score. Only the family (and so the ammo) follows the new pick.
- **Surfaced at edit time from:** MAP.md "RANGED PICK" entry (WEAPON / ARMOR STYLE BY PERKS).
- **SEV-4 (marth's call):** a PlayerGiven relic bow with no arrows is swapped for a crossbow with bolts, which contradicts Equip.cpp:128-130 ("a held one is never swapped out"). Defensible, since it cannot fire.
- **SEV-4 perf:** `CountAmmoOwned` builds a full `GetInventory` map on every satisfied ranged lap per hand (`emptyWhileAmmoElsewhere`). Compute once per lap and only walk when the held family has 0 ammo. Noted under MFO-B21.
- **SEV-4:** a follower carrying only non-playable ranged weapons now gets its ammo family from the ammo counts (arrows by default) because they are not candidates.
- **SEV-5, raised against 73c14560 (Opus tier-2 review, MERGE):** the GetEquippedItemType votes ignore the condition's run-on subject, so a perk testing the TARGET's crossbow would vote too. Reviewer's reasoning (as relayed): the keyword and hand facts already read conditions the same way, ignoring the subject, so this is the existing convention and not new. Deferred, not dropped.

### MFO-B213 (SEV-5) -- a benched follower on Auto has no resolved class for HMS (review F6)
Raised by the author of `feat/mfo-hms-core` (HMS is core, ClickUp 86e3dnt7u), 2026-10-01. Confirmed SEV-5 as F6 by the Opus tier-3 review of f29772c9 (`agentlogs/review-hmscore.md`). Finding: `Followers::PublishActiveMirror` resolves Auto through `Logistics::ResolveBaseClass` only for ACTIVE followers (records with a Gambit pick are mirrored whether active or not). An enrolled follower who is benched, left on Auto, and gains a progression level gets the level-gain `RecomputeHMS` with `resolvedClass=0`, which exits `noclass` before measuring. Reasoning: nothing is lost. The held target is unchanged, so the drift watch measures the same award the first active poll after re-recruiting, and the R2 flag keeps that window from reading as 0-award. Fix shape when drained: resolve benched records too (`LookupByID<Actor>` on an unloaded actor, base skill reads), if the field ever shows a benched award that matters.
Surfaced at edit time from: MAP.md progression "§HMS IS CORE".

### MFO-B214 (SEV-5) -- hmsAwardAccum is not reset across an unjudged window (review F5)
Raised against f29772c9 (`feat/mfo-hms-core`, Opus tier-3 review, `agentlogs/review-hmscore.md`), 2026-10-01. Finding (verbatim, as relayed): "hmsAwardAccum is not reset across unjudged windows (switch off or class unresolved). The reset sits inside the `if`, Poll.cpp ~:214-226, so an old award can count as \"got award\". It only errs toward \"leveling\"." Reasoning: the R2 gate skips the fixed-stat judgment when no measurement ran in the window, and it also skips the `hmsAwardAccum = 0` that closes the window. An award tallied before an unjudged stretch then survives into the next judged window and reads as an award there. The error only ever keeps a follower "leveling" (it delays a fixed-stat flag by a window), never flags a leveling follower fixed-stat. Fix shape when drained: move `st.hmsAwardAccum = 0.0f` out of the `if (st.hmsMeasuredThisWindow)` block in `PollWork`, next to the flag reset.
Surfaced at edit time from: MAP.md progression "§HMS IS CORE".

### MFO-B218 (SEV-4) -- a genuine fixed-stat follower can match the never-processed adopt once (re-review R2-2)
Raised against f0800d22 (`feat/mfo-hms-core`, Opus tier-3 re-review, `agentlogs/review-hmscore.md`), 2026-10-01. Finding (as relayed): a genuine fixed-stat follower can match the never-processed signature once. That clears fixedStat and delays its backfill by two levels, with no points lost. Reasoning: a fixed-stat follower whose HMS never processed an award or a grant (cumulative 0, withheld 0, remainder 0, held == baseline, 0x40 still set) is indistinguishable from a never-processed block, so the adopt clears its flag and the R2-gated detection needs two measured 0-award windows to set it again. The backfill is computed from the shortfall to the player total, so nothing is lost, only delayed. The load-only arming (R2-1 fix, `hmsAdoptArmed`) narrows it to records loaded from a save. Fix shape when drained: none planned unless the field shows it, or keep fixedStat on the adopt when the streak is already 2 and the measured flag says it was judged.
Surfaced at edit time from: MAP.md progression "§HMS IS CORE" (4b).

### MFO-B219 (SEV-5) -- a never-processed block without 0x40 still gets the lump, and a cosmetic grant line on the adopt poll (re-review R2-3)
Raised against f0800d22 (`feat/mfo-hms-core`, Opus tier-3 re-review, `agentlogs/review-hmscore.md`), 2026-10-01. Finding (as relayed): a never-processed block without 0x40 still gets the lump. Also the cosmetic `[hms] fixed-stat grant` log line on the adopt poll (Poll.cpp ~:248). Reasoning: the adopt signature requires 0x40, which every Enroll since v8 and the v<8 load retro set, so a block without it would have to come from a path the reviewer could not rule out entirely. Such a block is measured as one lump, capped by credit and withheld above it, as before this branch. Separately, on the adopt poll a stale fixedStat still computes and logs a backfill budget in `PollWork` before `RecomputeHMS` adopts and drops it, so the log shows a grant that is never applied. Fix shape when drained: (a) none planned without field evidence, (b) skip the grant log, or compute the grant after the adopt.
Surfaced at edit time from: MAP.md progression "§HMS IS CORE" (4b).
- **Also (SEV-5, raised against 49260f2d, third re-review, agentlogs/review-hmscore.md):** "The edge accepted with option (a) remains: Enroll, then save and reload before the follower's first positive engine award. The re-armed record matches the signature on reload and spends its 0x40, which is the same effect as R2-1 but only in that narrow window." Same check, `progression/Hms.cpp:257`.

### MFO-B206 (SEV-4 / SEV-5, deferred) -- i18n (translatable ImGui): review leftovers
Raised against 7b2552fb (`feat/mfo-translations`, Opus review, nothing above SEV-3 after the SEV-3 and the column-width SEV-4 were fixed in the next commits), 2026-10-01. The reviewer's verbatim text was not stored on disk (only its working log, `agentlogs/review-i18n.md`); each item below carries the coordinator's relayed finding and the reviewer's reasoning as relayed.
- **SEV-4, accepted:** `release.sh` is outside the brief's stated file list (it gained the template copy and the `gen_template.py --check` gate, +6 lines). Reasoning: shipping the template is the task's own deliverable, so it was accepted, but it should have been flagged as scope when it was done. Python 3 is now needed on the packaging machine.
- **SEV-5:** `Strings.cpp` `TranslatorUp()` leaves the one AddRef `GetStateAddRef` takes, as the SKSE wrapper does. Reasoning: a singleton state object, bounded leak.
- **SEV-5:** `ParseTranslation("MFO")` runs on the main thread at kDataLoaded and may touch `translationMap` while the engine's UI runs. Reasoning: no race found, and it only runs when nothing was found.
- **SEV-5:** `##` / `###` inside a translated value is not rejected. Reasoning: a translator could embed an ImGui ID separator and change a label's ID or hide text.
- **SEV-5:** a translator who copies `MFO_ENGLISH.txt` whole into `MFO_<LANG>.txt` supplies a line for every key, which blocks the game-term fallback (Health/Magicka/Stamina/Cancel). Reasoning: by design (MFO file wins), but easy to hit.
- **SEV-5:** the hosted tab fallback in `Board_Progression.cpp` (`hostedTabLabel`, now `Win_Header`) is also the tab's ImGui ID, so the ID depends on the language. Reasoning: only matters if the add-on ships an empty label.
- **SEV-5:** the del / sure? button swap changes the ImGui ID with the armed state (kept as before on purpose). Reasoning: unchanged behaviour, noted so nobody "fixes" it blindly.
- **Surfaced at edit time from:** MAP.md section 6, `native/i18n/` entry.

### MFO-B220 (SEV-4) -- retreat self-heal scan skips a higher-ranked act.wait and the g_recent window
Raised against c773a4ff (`fix/mfo-heal-starve-retreat`, Opus review, F4), 2026-10-01. Finding: the retreat's mini scan fires any Heal-kind cast_self rule and does not honour a higher-ranked `act.wait` (the authored suppress idiom) or the `g_recent` suppression window that the normal combat scan keeps. Reviewer's severity SEV-4, deferred, no behaviour harm seen. Fix shape: run the same positional stop rules in the mini scan.

### MFO-B221 (SEV-4) -- field watch: P1 hold can re-arm per claim and starve offense
Raised against c773a4ff (same review, F5). Finding: re-mints restamp `lastSeen`, so a heal that never starts can re-arm the 4 s hold each claim. `kNeverFired` stays the loud signal. A field watch, not a code change: see MAP.md "Telling P1 worked from P1 starved offense".

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED -- 9 hold-arms, 3 ENDED fired, 1 NEVER FIRED (14:39:11.508 -> 14:39:13.239 'no hold -- never-fired bound released it'); no repeated hold+kNeverFired starvation pattern seen; needed: a long offense-starved fight with a healer to rule the re-arm out.

### MFO-B222 (SEV-5) -- a B177 restream-gap kept heal lock survives the retreat
Raised against c773a4ff (same review, F7). Finding: a heal lock kept through the stream-cap restream gap (B177, `restreamAt`) is not cleared when the retreat fills, because `ReleaseHealClaimForRetreat` only acts on a standing claim. Harmless until the gap window lapses.
### MFO-B215 -- complex spells (task 7, ClickUp 86e3dmtr1): follow-up phases P1-P5 after the passive probe
Raised with `feat/mfo-spell-archetype-probe` (the P0 slice: `cast/Archetype.cpp` classifier, `[archetype]` / `[cast-call]` / `[summon-probe]` lines, `[cfc]` enrichment). Not a review finding: the plan of record from the research (scratchpad `complex-spells-research.md` section 3.4), recorded so the phases survive the session. **NEEDS LOCAL first.** P0 changes no behaviour; every phase below is gated on a Deck log read.
- **P1 (LOCAL, Deck):** one session with Harbinger's `[ctcensus]` on (default 1) and a gambit per archetype row (research table A1-A22, test list built locally from real FormIDs). Grep `[archetype]`, the `[cfc] ... NO observed cast` lines (now carrying `spell "name" (id) shape= predicted=`), `[summon-probe]`, and the last `[cast-call]` line before any freeze. Serana repro and the identity of 841D7023 come out of it. Opus concludes from the evidence (field diagnosis rule 4); do not arrive with a theory.
- **P2 (MFO, tier B each):** fix only the MFO-only rows the evidence confirms: Reanimate target (MFO-B84/B77), the no-row refusal or marth's chosen alternative, two-handed and dual equip (`Loadout::Prepare` equips LEFT only; MFO-B1), summon and self buffs moved to claims where the census shows FIRED natively.
- **P3 (APMF, tier A each, own brief):** per-type 0x06 seats where the census shows BUILT-NOT-FIRED; the seat-0 row substitution only if marth chooses it.
- **P4 (APMF + MFO, tier A):** the Target Location point seat (research brief first: the cast core's TL arm, the pick at AE 34457, the inline check AE `0x5bd04e`, the barrier projectile), ABI v18, so runes, bursts and walls land at a declared point.
- **P5 (MFO, tier A for the new opcode):** the gambit condition "N+ enemies in zone" (default 2) driving an AoE cast, point inferred from the spell via APMF `FindHostilesInSpace` / `FindEmptySpace` (already merged).
- **Open decisions for marth (research section 7):** release-gate scope for the direct road; no-row spells (accept unanimated, refuse, or the seat-0 substitution); runes/walls/bursts; Reanimate (engine corpse search vs MFO-picked corpse); Script-archetype summons (MFO-B75); keep the mirrored engine-row table in MFO or add a Harbinger query slot (append-only ABI).
- **Known limits of the P0 mirror (so a wrong `predicted=` is not mistaken for engine truth):** hostile is read from the effect's kHostile flag only (the docs do not say whether the engine key uses kDetrimental too); `self` is the spell's delivery == Self with no seat-0 flip modelled; the engine's effect score is not readable, so several differing rows fall back to the costliest effect and print `(approx)`; rows whose hostile or self condition the docs do not state print `unknown` when the effect has the unusual value; Banish/TurnUndead key "AV 1" in the docs with no field named, so a differing AV prints `unknown`.
- **Remaining SEV-5s from the Opus review of 75292e1 (deferred):** no generation guard on the `[summon-probe]` self-reposting chain (safe today: it holds FormIDs, is dropped by `MainThread::Clear`, and ends at a deadline); the "never resolved" give-up uses wall-clock time, so a menu pause can trip it; the header cite lines are off (`IsTwoHanded` is SpellItem.h:82, `range` :56); MAP's "10 direct sites" wording (corrected in the follow-up round to 9 plus `SummonOnMain`).
- **Surfaced at edit time from:** MAP.md native/cast/ "SPELL ARCHETYPE PROBE".
### MFO-B223 -- Cicero gear fixes: deferred Opus findings F4-F7 (SEV-4/SEV-5)
Raised against 8d153b86 (`fix/mfo-cicero-gear`, Opus review round 1). The SEV-3s and the loot SEV-4 were fixed in the same branch; these remain.
- **F4 (SEV-4):** on Auto, a tie between the best magic skill and the best melee/archery skill now goes to armor (`UsesMageClothing`, `logistics/Cast.cpp`, strict `>`). Reasoning: a tie is Melee in `ResolveBaseClass` too, but a hybrid follower who used to get mage clothing from a lone cast gambit may now stay in armor. Watch the `[gear] <id> clothing mode` lines for hybrids on the next Deck session.
- **F5 (SEV-5):** `DeclaredSetKeeps` ignores the declaration's `accept()` order (two slate pieces that overlap one another: the later is dropped by the declaration but still reads as kept). Reasoning: harmless while the worn piece is the slate's own pick, only matters for a overlapping pair.
- **F6 (SEV-5):** the rated slate (`ComputeOwnedGearSlate`) has no blacklist check, so a blacklisted worn piece can sit in the slate. The sell side still force-sells blacklisted apparel, so no loop, but the declaration may name it.
- **F7 (SEV-5):** the `[gear] clothing mode` dedupe only runs while `IsCasterFollower` is true (a follower who stops being a caster never logs the change), and the dedupe is keyed on `const char*` literal pointers.
- **Surfaced at edit time from:** MAP.md "CICERO GEAR FIXES".

### MFO-B224 (SEV-4 / SEV-5, deferred) -- Cicero gear round 2 leftovers
Raised against 79fb52a5 (`fix/mfo-cicero-gear`, Opus focused re-review, verdict MERGE), 2026-10-01.
- **SEV-4:** `DeclaredSetKeeps` (`logistics/EquipAuthority.cpp:484-490`) uses the raw `gs.slot[]`, not the list `accept()` builds. `accept()` drops a slate piece that overlaps the pick, overlaps an earlier piece or shares the head mask. In mage mode `playerPieceWins` can also swap a piece. Membership then over-claims "kept", which is benign: the piece is no longer worn and sells later. The rule-5 test under-claims: a worn piece W that overlaps only a slate piece `accept()` dropped is still sent by the declaration's rule 5, but `DeclaredSetKeeps` returns false. If the keep set also ranks W `redundantInferior`, force-sell and `gemHold` run again. That needs a hood-on-robe or pick-overlap layout plus a gemmed worn piece, so it is rare. A substituted player piece stays protected (the `IsPlayerPick` check comes before `gemHold`). Fix: move `accept()` into a helper both callers use, as was done for `Rule5Overlaps`.
- **SEV-5:** in mage mode `DeclaredSetKeeps` still calls `ComputeOwnedGearPick`, which walks inventory in `MageBestPerSlot`, and then throws the result away. Checking `gs.mage` first would skip the walk.
- Stale docs from the same review (the "MEO absent" school rule in Logistics.h, TradeBridge.h, Economy.cpp and MAP) were fixed at merge.
- **Surfaced at edit time from:** MAP.md "CICERO GEAR FIXES" note and the EquipAuthority entry.

### MFO-B225 (SEV-4/5) -- G1 review follow-ups
Raised against 957d748 (`fix/mfo-g1-exact-gates`, Opus tier-A review MERGE), 2026-10-05.
- SEV-4 (F2 scope): on an unsupported runtime the plugin still loads and its tick reads CommonLib bucket-selected layouts (GetActorRuntimeData in Scheduler/Evaluator/Hands, AMMO/TESObjectCELL GetRuntimeData). Before F2, 1.7.104 is filed as SE, so those reads would use the SE layout. F2 must make those accessors fatal or refuse off-table builds, or MFO must refuse to start its tick when !Known().
- SEV-5: Board.cpp:1724-1730 InstallInputHook still uses a 3-field pair. Switch it to Runtime::IsVerified1_6_1170()/1_5_97(); a 1.6.1170.1 repack would otherwise hit a fatal id miss before SeatVerified. This also fixes the misleading "using the input sink + ControlMap path" log at :1731-1734.
- SEV-5: Upkeep.cpp:~550 skip log says "(VR)" on any non-exact build. Use Runtime::GateReason().
(The CHANGELOG naming of the Field Orders board was fixed at merge.)
- **SEV-4 RESOLVED by F2b (`feat/mfo-1.7.104`, 2026-10-05).** The fork now files 1.7.x with AE and refuses every 1.7.x
  but 1.7.104.0 at load, so 1.7.104 is no longer read as SE. Every bucket read the item names was compared against the
  1.7.104 executable and is the same value there: Actor runtime data at +0xE8 (currentProcess +0xF8, combatController
  +0x160, currentCombatTarget +0x104, magicCasters +0x1A8, addedSpells +0x190, boolBits +0xE8, boolFlags +0x204), AMMO
  data +0x110/+0x118/+0x11C, TESObjectCELL runtime data +0x68 with worldSpace +0x128 (`Docs/ENGINE_NOTES.md` section
  0.49, with addresses). Other off-table 1.5.x / 1.6.x builds still read CommonLib's upstream layouts in the tick, the
  same as before G1; that is not an F2 question and stays as it was. The two SEV-5 items above stay open.

### MFO-B226 (SEV-4/5) -- pickup-sound seat review follow-ups
Raised against 7e17730 (`feat/mfo-pickup-sound-at-follower`, Opus tier-3 review MERGE), 2026-10-05.
- SEV-4: the generated doc and header text is misleading. The "Regenerating" command in `Docs/VERIFIED-ADDRESSES.md` omits `--idmap tools/verified_addresses/idmap-1.7.104-mfo.csv`, and the `native/VerifiedAddresses.h:211` comment says the seat is refused on 1.7.104 when the raw row covers it. Fix it in the generator template, not by hand.
- SEV-5: any other Actor::PlayPickUpSound that runs synchronously on main inside MFO's ActivateRef is also made positional and logged against the follower. Harmless.
- SEV-5: the raw row carries no byte check, and only Install's runtime checks verify it. The generator could emit signature bytes for raw rows later.
- Reasoning (author; the coordinator relayed severity and finding text only): none of the three changes behaviour today. The generator is shared byte-for-byte with APMF, so the SEV-4 fix is a two-repo generator round.
- **Surfaced at edit time from:** MAP.md logistics `logistics/PickupSound.cpp` "What breaks".

### MFO-B227 -- per-hand heal road: deferred review findings (F5 SEV-3 + SEV-4/SEV-5)
- **LEFT-WEAPON SHAPE DECIDED + FIXED 2026-10-11 (marth: give the weapon back when the heal ends, tracked as owed gear):** `Loadout::Prepare`'s Caster branch books a left weapon displaced by a live left heal claim as gear debt (`Debt::healWeapon`); `Loadout::Tick` returns it once no heal claim stands on the left. ClickUp 86e3m3q0b.
Raised against 9895a53 (`feat/mfo-perhand-heal`, Opus tier-A review, 2026-10-05). F1 (Harbinger proxy is one slot per OWNER, freed by the departing claim) is fixed in APMF by a separate agent; F2/F3/F4/F6 were fixed in the follow-up round. Deferred here, verbatim from the reviewer's log (`agentlogs/review-perhand-heal.md`; the review's full prose went to the coordinator, so these are its recorded lines):
- **F5 (SEV-3, deferred by the coordinator: `iCastControl` is 2 on LoreRim, exact mode is not in the test plan):** "castLvl>=4 Prepare deselects right heal -> SEV-3." `Loadout::Prepare`'s Caster branch at cast-control level 4 `DeselectSpell`s the RIGHT hand's spell when it is not the spell being prepared, so an offense or second-recipient Prepare for the LEFT can strip a right-hand heal claim's spell. Fix shape: skip the deselect while a right-hand heal claim names that spell (`APMFBridge::GetHealCastSpell(fid, kApmfHandRight)`).
- **SEV-4 (the non-F6 half):** "Prepare AlreadyReady via right-hand copy -> false repair log -> SEV-3/4." F6 fixed the heal-claim Prepare; a non-heal caller (offense) still answers AlreadyReady off either hand's copy (unchanged, pre-existing).
- **SEV-5:** MAP.md file:line citations in the native/cast/ entries were stale (corrected in the fix round for the per-hand heal files; the older cast entries still carry pre-split numbers).
- **Surfaced at edit time from:** MAP.md native/cast/ "PER-HAND HEAL ROAD" What breaks.
- **SEV-4 (round 2 re-check of 57bd5e7, appended):** a LEFT weapon with the heal spell S only in the RIGHT hand reads as `Grip::Caster` in `Loadout::Read`, so the left heal Prepare's Caster branch equips S into the left over the weapon with no gear debt booked (the weapon is not given back by the debt ledger). Edge of F6's fix (`Loadout.cpp:329-333`); left open.
- **Round 3 re-check of 0ce73c4 (Opus, 2026-10-05), deferred / accepted.** Reviewer's log (verbatim): "SpellFire events deck-observed on NPC graphs (castobs 2026-09-04, ENGINE_NOTES:431). No proxy re-claim loop (c.proxy re-read from new handle; APMF publishes after Acquire/AddSpell on main and on fix branch e2a77a8). New: worker-thread Actor::HasSpell (engine id 37828) every refresh lap vs main-thread Add/RemoveSpell of proxies = threading carve-out (a), SEV-3 nominal. Undecided->bound off = conditional unbounded hold if sink silent (loud) SEV-3." The HasSpell threading item (T1) was FIXED in the follow-up round (the read moved to the main thread, latched per (follower, hand)). Open:
  - **S1 (SEV-3, accepted for this field cycle):** "unknown never releases" can hold a dead hand for as long as the per-hand SpellFire evidence is missing. Loud (the `[cfc] ... marked observed on NEITHER hand` and `never-fired bound NOT armed` WARNs), not masked; the field `[hand-fire]` lines decide it.
  - **S2 (SEV-3, accepted):** while a fire is undecided, `HealTakesLeft` reads "not fired" and returns false between casts, so in the undecided case only the left can thrash weapon / heal.
  - **SEV-4:** consume-on-take mis-pairing: an unclaimed AI cast on the other hand inside the 1.5 s window can be paired with a claimed cast.
  - **SEV-4:** with the APMF fix branch, one spurious proxy release per save if a lap lands between PreSaveSweep and ReteachLive.
  - **SEV-4 (pre-existing, not changed):** `Loadout::Prepare` calls `Actor::HasSpell` on the job worker (the same class as T1).
  - **SEV-5:** `[hand-fire]` volume on offense casts; HealObs drops the attribution when both hands fired; `LastFireHand` can be stale for a later fire of the same form; `g_handFire` and `g_handFireAttach` never shrink within a session (cleared only on load).

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED (S1 only) -- 1 NEITHER-hand line (MFO.1 12:41:53.390 Jesper 02003F52) but the right hand FIRED at 12:42:06/18/28 per APMF ctcensus, so no dead hand; the F5 and other parts were not covered.

### MFO-B228 (SEV-5) -- press-latched click follow-ups
Raised against deddf80 (`fix/mfo-ui-port-meo-drag`, Opus review MERGE), 2026-10-05. (B227 was not in this branch's copy of the file; check for a clash at merge.)
- SEV-5: Selectable popup-close parity. A latched Selectable click closes its popup on the `Popup` window flag alone; ImGui's own condition also involves `!auto_selected` / `AutoClosePopups`. The two can differ in edge cases.
- SEV-5: the skill-tree node selection ring (hover-steered `s_selNode`) and the popup target can briefly disagree after a latched click, because the latch fires on release, not on hover.
- SEV-5: a stale latch can survive a tab or popup bounce within one held press (it is dropped only when no mouse button is down, or on a new press). A matching id plus rect slop is needed to fire, so the window is narrow.
- SEV-5: the MAP.md note for `Board_Click.cpp` was first placed in the file-size paragraph; moved to the Board section in the round that raised this.
- **Surfaced at edit time from:** MAP.md Board section "Press-latched clicks".

### MFO-B229 -- Harbinger own-LoS adoption: deferred review findings F2-F7
- **F2-F4 RULED 2026-10-11, NOT IMPLEMENTED (STOP, needs the APMF side):** marth: "Never. Line of sight is checked each time." MFO's adoption (`Sightline::CheckWithin/OccludedRun/MeasureNow` Basis::Own via `APMFBridge::LosOf`, apmf/Los.cpp) can only read what Harbinger's `GetLineOfSight` serves: a lock-free STORED verdict kept measured by the ask, with a 2 s interest window (APMF_API.h ~2192); it never casts a ray, and `SenseActor` (which does) is main-thread-only and answers awareness, not a heal pair verdict. A fresh per-decision measurement needs a new APMF ABI slot (controller-agent area). ClickUp 86e3m3q0d left open.
Raised against 259630e (`feat/mfo-harbinger-los-adopt`, Opus review MERGE, 2026-10-05). F1 (SEV-3, `LosSupported` did not probe whether the service is armed) was fixed in the follow-up round. Reviewer text is summarised from its log (`agentlogs/review-mfo-los-adopt.md`: "PinTarget Unchanged path keeps flag across weapon swap (SEV-4); interest window 2s vs round-robin N*133ms -> N>=16 cold (SEV-4); OccludedRun has no callers") plus the coordinator's relay of F2-F7.
- **F2 (SEV-4):** the ch.20 pin's `kTargetPin_OwnLineOfSight` is fixed at pin creation. The `PinTarget` Unchanged path keeps the flag across a weapon swap (bow to sword or back), so it changes only on the next target change or re-pin.
- **F3 (SEV-4):** Harbinger drops a pair nobody asked for in `kLosInterestMs` (2 s). The round-robin service asks a given follower's pairs every N x 133 ms, so a party of 16 or more reads cold (Unknown, fail-open) for part of each lap.
- **F4 (SEV-4):** `RefuseHealApplyOnMain` (`cast/CanAct.cpp`) now reads Harbinger's STORED verdict through `MeasureNow(Own)` (typically <= 250 ms old, up to 1 s, Unknown on the first ask) instead of measuring synchronously.
- **F5 (SEV-5):** the `[los]` change lines for Own pairs no longer appear at ABI 18 (the Own path bypasses `Sightline::Measure`), so field analysis loses them. `Sightline::OccludedRun(Own)`'s Harbinger branch has no callers today (dead).
- **F6 (SEV-5):** `apmf/APMFBridge.h` is at ~1528 lines. The next brief that touches it proposes a split (its own round).
- **F7 (SEV-5):** `SenseOf` failing (NoOrigin, bad args, a race to unarmed) falls back to `Sightline::MeasureNow` on the engine basis in `EngageOnSight.cpp` without a log line.
- **Surfaced at edit time from:** MAP.md Sightline.cpp and EngageOnSight.cpp "What breaks".

### MFO-B230 -- Self-buff claim road (cast/BuffRoad.cpp): deferred review findings F5, F7-F9
- **R3-2 DECIDED + FIXED 2026-10-11 (marth: keep the hand claimed between a buff's casts, "only in fanning though"):** only for AUTO's buff series: `g_autoBuffSeries` (Auto.cpp sets it around its CastOn) makes `BuffRefreshGate` end a landed / idle claim with takeBack=false, so the next recipient finds the spell already in hand. Single-target buff rules release as before. The spell stays in hand after the series runs out of recipients until the normal release (rule stops / combat end). ClickUp 86e3m3q0g.
- **F5 DECIDED + FIXED 2026-10-11 (marth: a config change mid-claim keeps the running claim's safety limits):** `BuffRefreshGate` (cast/BuffRoad.cpp) keeps bounding a standing claim (lock names the spell on LEFT) when a config flip makes `ChooseBuffRoad` answer DirectDegrade; the ward stream cap and the never-fired bound still run. ClickUp 86e3m3q0e.
Raised against d5c51e0 (`feat/mfo-remaining-cast-kinds`, Opus review FIX FIRST, 2026-10-05). F1 (never-fired bound), F2 (preempt before the already-up decline), F3 (Light effect / rowApprox), F4 (ward cap keeps its rank, lock channel clock) and F6 (main-thread latched up-read) were fixed in the follow-up round. Reviewer's log (verbatim, `agentlogs/review-buffroad.md`): "F5 SEV-4 (BuffRefreshGate keyed on road choice: config flip mid-claim leaves ward unbounded). F7-9 SEV-5 (logs/labels, revert clear, unguarded LEFT release)." The F9 wording below is the coordinator's relay.
- **F5 (SEV-4):** `BuffRefreshGate` runs only while `ChooseBuffRoad` answers Claim. Flipping `bApmfCast` / `bLegacyCastHybrid` / `bEquipToCast` mid-claim sends the lap to the old refresh without the ward stream cap or the never-fired bound until the claim's own TTL ends it. A mid-claim config flip skips the never-fired bound too.
- **F7 (SEV-5):** log and label wording: the `[buff]` lines share the heal road's labels in places, and `self-cast could not fire` (CastOn) is the label for a transparent BuffSelfClaim Declined.
- **F8 (SEV-5):** `EndBuffClaim` releases the LEFT offense slot (`ReleaseCastClaimOnHand`) without checking the claim there is this spell's; the lock rank gate makes a stranger on LEFT unlikely, not impossible. (The revert-clear half is fixed: `ResetBuffRoad` from `ClearCastLocks`.)
- **F9 (SEV-5):** the banner and the brief said a buff at an ally or the player and AUTO's beneficial fan are "logged" by the road; they never reach `ChooseBuffRoad`, so they are not. Fixed in the banner and MAP.md in this round.
- **R2-5 (SEV-5, raised against 626c4a2):** the ward stream cap writes the lock's `restreamAt` / `channelSince` / `channelCap` directly in `cast/BuffRoad.cpp`, duplicating about four lines of `ReleaseOwnHealClaim`'s restream branch (`cast/Hands.cpp`). A shared Hands.cpp helper later (Hands.cpp is at ~1484 lines; its own round).
- **R3-2 (SEV-4, raised against c0a6842):** release/re-claim churn after a fire. The claim is released as idle (or when the buff lands) and re-claimed when the rule wants it again. Fix later by keeping a fired claim standing between casts, like the heal road's `kBetweenCasts`, or by releasing it with `takeBack=false` plus `restreamAt`.
- **R3-3 (SEV-5):** `ClearCastLock(follower)` also runs mid-fight on the Scheduler's `!castSeen` tick, so the never-fired hold-off and the fired set end there too, not only at the fight's end. The WARN now reads "held off until the next cast-rule reset".
- **Surfaced at edit time from:** MAP.md `cast/BuffRoad.cpp` entry.

### MFO-B232 -- Buff / summon / rowless claim road (cast/BuffRoad.cpp): deferred notes
Raised against 6b29d4c (`feat/mfo-claim-road-summon-ally-rowless`), recorded at the follow-up commit. Surfaced at edit time from MAP.md `cast/BuffRoad.cpp`.
- **N1 (SEV-4):** a hostile rowless spell at a foe (Calm / Fear / Frenzy) takes CastOn's owned-offense claim (`ownedCast`). On a Harbinger below ABI 19 that claim never fires (no Script-row seat). Pre-existing and loud (the `[cfc]` silent-claim warning), with no direct fallback by design; not gated here because that would change the offense road.
- **N2 (SEV-5):** a buff at an ally or the player takes the LEFT hand, one recipient at a time (AUTO is a series, lowest HP first), instead of the heal road's RIGHT-hand second recipient (`cast/HealRoad.cpp`). Reusing that model needs real HealRoad surgery; revisit if serial buffing proves too slow in the field.
- **N3 (SEV-5):** the WARN rate of a Reanimate claim with no corpse (never-fired release, then held off per fight) is unmeasured. It is bounded to one WARN per fight per (recipient, spell), but the cast-rule reset mid-fight (see R3-3 of MFO-B230) can repeat it.
- **F4 (SEV-4, Opus review of 9c4f1dd, reviewer's wording):** the take-back is skipped entirely inside `g_aiCastGrace`; `ReleaseSpell` returns early and never retries. Candlelight then stays equipped and unclaimed for the whole transparent period, up to the buff's duration, until Scheduler H3 or the next claim. The bound in that window is the engine's NATIVE Light 0x06 already-applied walk (for a kSelf form it lands on the caster); Harbinger's composed bound acts only while a claim stands.
- **F5 (SEV-4, reviewer's wording):** the claim road's own window. A claim fire never sets `lastCast`. For a summon whose effect lands via a TargetLocation projectile there is no active effect until impact, so in that window `SummonGate` returns Clear and Harbinger's `AlreadyApplied` is false; a second charge started then can complete. The engine's own limit evicts the first summon, so creatures do not accumulate.
- **F6 (SEV-5, reviewer's wording):** (a) the B231 collision, resolved by renumbering to B232; (b) MAP.md not citing the backlog id, fixed; (c) a within-lap race where `ChooseBuffRoad` flips between the `Fire.cpp`/`Auto.cpp` check and `CastSelfDirect`'s re-check, which would send a summon to the direct self stream (inputs are the same within a lap, so negligible).
- **R2-1 (SEV-3, fixed in the follow-up round):** the explicit-self kTargetActor claim (a non-kSelf buff at the caster, target = his own FormID) has an UNPROVEN landing. Harbinger's STATUS predicts a "hand stuck" for a self-aimed delivery, and a fire that never lands re-casts until the magicka runs dry. Bounded now for every buff claim: a first observed fire with the up-read still No after `kHoldLastSeenCapMs` releases the claim with a `[buff] ... FIRED but NEVER LANDED` WARN and holds the rule off for the fight. Concentration is excluded from `ExplicitSelfBuff` and keeps the Task-1 target-0 claim. **Field observable:** a `[buff] <fid> <name> (<spell>): self-buff CLAIM at <the caster's own id>` line, then a `[cast]` SpellFire for that spell, then a visible effect on the caster (or the NEVER LANDED WARN).
- **Surfaced at edit time from:** MAP.md `cast/BuffRoad.cpp` entry ("Open findings: MFO-B230, MFO-B232").

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- R2-1 CLOSED (Lamb of Mara claim at own id 12:46:35, LANDED +1600ms, FIRED via Script) but N3 Reanimate had 0 log lines; needed: a Reanimate cast in the field.

### MFO-B231 -- Held-heal approach (apmf/CombatApproach.cpp): deferred review findings F5
Raised against e8ba5a0 (`feat/mfo-held-heal-approach`, Opus review FIX FIRST; F1-F4 fixed in the follow-up round), 2026-10-05. (B231 was the next free id in this branch's copy of the file; check for a clash at merge.)
- **F5a (SEV-5):** `apmf/APMFBridge.h` is at ~1538 lines (past the ~1500 plan-a-split line). Propose a split as its own brief (`apmf/` public header per claim family); this branch only added 10 declaration lines.
- **F5b (SEV-5):** the `[heal-approach]` heartbeat logs every 1.5 s per standing claim while Approaching / Holding; chatty on a long approach. Thin it (state changes plus every ~5 s) once the field shape is known.
- **F5c (SEV-5):** an MCM change of `fHealApproachRadius` reaches a standing claim only at its next Repoint (a recipient change); a new claim reads the live value.
- **F5d (SEV-5):** `g_approachBlocked` / `g_approachRefileAfter` for a follower who dies mid-fight are cleared only by `ReleaseHealApproach(.., fightOver)` (dismissal, combat ended) or `ClearHealApproaches` (load / revert); a few FormIDs can linger until then.
- **Surfaced at edit time from:** MAP.md `apmf/CombatApproach.cpp` entry.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): CLOSED (no thinning needed) -- heal-approach ran (31 lines over 7 claims) with no failure seen but the F5b-specific datum is not in the logs; needed: a LoS-held heal approach with the F5b condition logged.

### MFO-B233 -- Self-flip proxy has no capability signal (cast/BuffRoad.cpp)
Raised against 2f48f5c (`feat/mfo-self-proxy-aimed-touch`, Opus review MERGE), 2026-10-06.
- **DECIDED (coordinator): no ABI bump.** Released Harbinger 0.10.0 is ABI 17, below MFO's ABI >= 19 gate, and every future release contains the self-flip proxy (APMF main 46f4ce1). Only unreleased dev builds between d8bb76d and 46f4ce1 fall in the gap, and BuffRoad's never-fired / fired-but-never-landed releases (MFO-B232 R2-1) bound that loudly.
- **SEV-3 (F1, RESOLVED 2026-11-09 for Calm / Frenzy / Demoralize / Paralysis (and TurnUndead) by `feat/auto-control-hostile`: ClassifySpell calls them Offense; unflagged Stagger remains OPEN):** MFO's beneficial filter is looser than Harbinger's self-flip gate. `CasterConsent::ClassifySpell` (CasterConsent.cpp:35) rejects only `IsHostile() || IsDetrimental()` effects; Harbinger also refuses unflagged Calm / Frenzy / Demoralize / Paralysis / Stagger archetypes (Pacify, MS08KematuParalysis, MG08 Ancano / Tolfdir). Such a spell reaches Claim in MFO (falls to NoRow); Harbinger then logs "NO self-flip proxy; the seats aim the original form at the caster", which never fires per APMF-B39 and is caught by the never-fired release. Still safer than the old DirectNoSeat force-cast onto the caster. Fix in ClassifySpell (also feeds the slider and AUTO's beneficial-to-party choice), NOT in ExplicitSelfBuff (declining there would send it back to the direct self cast).
- SEV-4: the BuffRoad.cpp comment "a hostile spell is never a Buff kind" holds only for flagged effects.

- FIELD VERDICT 2026-10-06 (Opus, Tuxborn deck logs): STILL NEEDS DATA -- 0 'NO self-flip proxy' lines, so no refusal was seen, but the capability signal itself was not exercised; needed: a self-flip cast on a runtime or spell where the proxy is unavailable.

### MFO-B234 -- A re-started concentration channel on the same claim is not protected again (cast/HealRoad.cpp)
- **SEV-3 bullet DECIDED + FIXED 2026-10-11 (marth: after 5 s at the approach floor with the recipient still out of sight, count the approach as blocked, release the heal with exit token approach-blocked, let it re-claim later):** `ServiceHealApproach` (apmf/CombatApproach.cpp) times the floor, marks the recipient blocked + stuck; `NeverFiredRelease` (cast/HealRoad.cpp) takes the stuck mark (`HealApproachStuckTake`) and releases. ClickUp 86e3m450a.
Raised against `fix/mfo-heal-hold-until-fire`, 2026-10-06.
- **SEV-5 (accepted under marth's ruling "for now yes", open):** `HealHoldStands` ends the hold on the first observed fire since the claim stamp unless a concentration channel is still in flight on the hand. A channel that dips out of flight and is re-started on the SAME claim (no re-stamp) finds the hold already ended and lapsed, so offense on the other hand can interrupt the second channel. Fix would need a per-claim "channel running" latch.
- **SEV-4 caveat:** a LoS-held span closes only on a Visible verdict (an Unknown keeps it accruing), so the exclusion in `NeverFiredRelease` can over-count an Unknown stretch. Bounded by `HealApproachBlocked` (no exclusion once the ch.24 approach is blocked or unavailable).
- **DECIDED by marth 2026-10-06, DRAINED by `f1bbd83` (`fix/mfo-post-2.2.0`):** "without line of sight it stops the actual cast and healing, but without full health its still active in harbinger and will restart the second line of sight is restored, which wont be long because losing LOS restarts the healing moveto." The held heal's ch.24 approach now keeps closing while sight is lost: `apmf/CombatApproach.cpp` TIGHTENS R (min of fHealApproachRadius and max(128, 0.6 x the current distance)) while the state reads Holding and the recipient is still Occluded, so occlusion counts as not arrived. The claim stays throughout; the never-fired bound still excludes LoS time only while the approach is live (`HealApproachBlocked`).
- SEV-3 (round-3 review of 0c9b0a5, marth's call): when Harbinger's ch.24 approach reaches the radius and HOLDS but the recipient stays occluded inside it (field 2026-10-06: walked 474 -> 341 vs R 384), the recipient is never marked blocked, so LoS-held time stays excluded and offense stays held until sight returns or the heal rule stops matching. Literal to marth's "exclude LoS-held time". If "nothing locked up" should win here: stop excluding LoS time once the approach has held at the radius with the recipient still Occluded for one more bound.
- TRIAGE 2026-10-06 (NEEDS-DECISION): the open SEV-3 bullet (approach holds at the radius with the recipient still occluded; offense stays held) is filed as a ClickUp needs-decision task together with MFO-B238 R1: see the task titled "Decision: should a held heal give up when the follower is at the approach floor and the recipient is still out of sight? (MFO-B234 SEV-3 / MFO-B238 R1)" in list 901717064773 (ClickUp 86e3m450a).

### MFO-B237 -- heal-until-full review leftovers (A4, A5 remainder, A7)
Raised against `d006496` (`fix/mfo-post-2.2.0`, Opus review, nothing above SEV-3 after the A1/A2 fixes), 2026-10-06. Reviewer's text as relayed by the coordinator.
- **A4 (SEV-4):** with a heal held, `Loadout::ReleaseSpell` is skipped entirely (Scheduler `!castSeen` block). `g_mfoSpell` can name a left-hand offense spell from a claim `ReleaseUnheldCastHands` just ended, and that spell stays equipped and unclaimed for the whole hold. Suggested fix: call `ReleaseSpell` unless `g_mfoSpell` is one of the held heal spells.
- **A5 remainder (SEV-4):** the exit tokens other than `preempted` (rank preempt, weapon-owns-right: fixed) are classified from free text or set only at some release sites; any release site that does not go through `ReleaseWithExit` logs `released-elsewhere`.
- **A7 (SEV-5):** `g_healAsked` entries from a follower's last combat tick survive until his next combat tick. Harmless.
- **Surfaced at edit time from:** MAP.md `cast/HealRoad.cpp` entry.

### MFO-B239 -- 1006c offense fixes review leftovers (item 1 / 2 / 3)
Raised against `f51c579` (`fix/mfo-offense-1006c`, Opus review MERGE OK), 2026-10-06. Reviewer's text paraphrased by the coordinator.
- **SEV-5:** `s_selfBuffFoeLog` (cast/CastOn.cpp, the once-per-(follower, spell) self-buff-at-a-foe warning) is not cleared on reload.
- **SEV-5:** when `sinceClaim` is 0 ms the "ever fired" latch can answer the freshness check in `CanPreemptHand`.
- **SEV-3 (watch in the field):** a blind concentration heal whose channel starts while offense is mid-charge on the other hand could be interrupted.
- **SEV-3 (accepted):** a direct-road lock (no APMF claim to observe) stays fresh until the 6 s `kInFlightHoldCap`, and the waiter clock can stretch a higher rule's wait to about 12 s.
- **Surfaced at edit time from:** MAP.md "FIELD 1006c OFFENSE FIXES" bullet.

### MFO-B250 (SEV-3, FIXED in 5b100b1, marth ruled) -- current-AV feedback lock-in in the weapon class and school picks
- **Raised:** Opus tier-3 review round 2 of `a015450` (`feat/mfo-gem-relevance`), 2026-10-12. Reviewer's log (verbatim): "SEV-3 current-AV feedback lock-in (ComputeWeaponRoles + TopTwoSchoolMask use GetActorValue; ArmorPrefFor uses base on purpose)."
- **What it means:** `ComputeWeaponRoles` (logistics/Gear.cpp) picks 1H vs 2H from the CURRENT One-Handed / Two-Handed values and `TopTwoSchoolMask` (logistics/Cast.cpp) the top two schools from the current school values, both including fortify buffs. A socketed Fortify One-Handed (or worn school gear) raises the value it is judged by, so the pick can lock onto whatever is already buffed. The F3 gem direction reuses both answers on purpose (gem choice must agree with gear choice), so it inherits this.
- **marth's ruling (2026-10-12, Q1):** judge by BASE skill values; perks break ties.
- **Why it was NOT fixed in the gem round:** it changes GEAR choice too (both functions feed the wardrobe / loot / buy / combat pick), so it gets its own brief and field cycle. The gem round must not change `ComputeWeaponRoles` or `TopTwoSchoolMask`.
- **Fix shape:** `GetBaseActorValue` in both (the `ArmorPrefFor` read), and on an exact base tie the perk votes / `StyleVotes::skillRanks` decide.
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "F3 GEM RELEVANCE" and the `ComputeWeaponRoles` entry.

### MFO-B242 (SEV-4 x3, SEV-5 x2) -- MEO gem rules review (51617e0, Opus BLOCK; SEV-2/3 fixed in the follow-up)
Raised against `51617e0` (`feat/mfo-meo-gem-rules`), 2026-10-12. Reviewer's text paraphrased by the coordinator.
- **SEV-4:** `GemRules.cpp` `FindOwned` / `Decide` read `InventoryEntryData::GetValue()`, the value of the whole entry (stack), not one copy.
- **SEV-4:** `Decide` does not check whether the source container was deleted (`IsDeleted`), only `IsDisabled`.
- **SEV-4:** the worker-side `RemoveItem` out of a container (`TakeAndProbe`) can race a player looting the same container at the same moment.
- **SEV-5:** `g_probed` (worker-only set of probed source/base/uid keys) grows without bound until revert.
- **SEV-5:** `#include "GemRules.h"` landed on the trailing-comment line of the neighbouring include in Lotd.cpp / Service.cpp / Upkeep.cpp, so the comment now sits on the wrong line.
- **SEV-4:** the `DropObjectAt` path (non-null extra list, position and angle) has never been seen running in the field.
- **SEV-4:** gear the PLAYER dropped carrying MEO's title is recognised as loose gemmed gear and gets its gems stripped.
- **SEV-5:** (fixed in the hold rework, kept for the record) the first hold key had overlapping bits; the hold is now a (follower, base, uid) tuple.
- **SEV-5:** the worker-only sets (`g_looseRefs`, `g_looseTaken`, `g_probed`) grow without bound within a session.
- **SEV-5:** shed and swap-up holds (Reason::Shed / SwapUp) have no abandon point; they end when the item leaves the follower.
- **Surfaced at edit time from:** MAP.md `logistics/GemRules.h` entry.
- Final check of f2ad1c7 (Opus, MERGE OK), SEV-4: heal/shed/swap-up holds have no abandon point; an item that stays and is later worn is never re-socketed (gems stay loose in the pack, not lost).
- SEV-4: frame-scale stale window: a reconcile SocketGem queued into a WORN force-sell item just before its first arm can land after the refresh, so later scans read Clear and it may sell with that gem.
- SEV-4: a socketed museum relic now costs two crate cooldowns (arm pass, then strip pass).


## LoreRim round 2 (feat/mfo-lorerim-round2, Opus review of 8c68e4a)
- **SEV-4:** the ch.24 offense approach (`apmf/OffenseApproach.cpp`) reads `fHealApproachRadius` for its tightening ceiling, so the heal setting also governs offense (the radius is now always raised to the 1024 standoff, so this only matters above 1024). Give offense its own config key when next touching it.
- **SEV-4:** `logistics/Service.cpp` (2163 lines) and `apmf/APMFBridge.h` (1577 lines) are past the ~1500-line plan-a-split line; propose the split as its own brief.
- **SEV-4:** `NavConnected` only follows links to meshes of the follower's and the ref's own cells; exterior cross-cell targets read Unknown (no Island), by design.
- **SEV-5:** touch-spell standoff reuses the approach's tightening floor (128); no dedicated touch-reach constant exists.
- **SEV-4:** the 30 s navmesh reach cache (`logistics/LootReach.cpp`) is keyed on the follower but not his position; a follower who changes islands within 30 s reads the old verdict.
- **SEV-3:** worst case about 5-8 ms in one frame when 4 pending fills land in the same main-thread drain. Option: one compute per drain (re-post the rest).
- **Surfaced at edit time from:** MAP.md loot failure model (e) and `apmf/OffenseApproach.cpp`.

## Bucket 3 (fix/mfo-bucket3, Opus review of 082bcb1)
- **SEV-4 (C1):** a perk referenced only by player-only spells is still marked effective (the entry-less perk rule in `Progression.cpp` does not check that an NPC can satisfy the spell side).
- **SEV-4 (C2):** an equip gambit that never fires still pushes casts to the left hand (the B3 right-first gate counts an enabled equip gambit, not one that actually fires).
- **Surfaced at edit time from:** MAP.md `Progression.cpp` and `cast/Hands.cpp` entries.

### MFO-B245 (SEV-3 field check, SEV-4 x5, SEV-5 x4) -- silver ammo + silver loot economy gate review, deferred items
Raised against 6332bdb / a006451 (`fix/mfo-silver-loot-economy-gate`, Opus review two rounds,
`scratchpad/agentlogs/review-silver-ammo.md`), 2026-10-12. Final MERGE OK at a69cdc9. The SEV-2 bow->melee episode
merge, the SEV-3 false player pick, the re-nock churn and the SEV-3 silver-pick suppression were fixed on the branch.
- (SEV-3, FIELD CHECK) Confirm silver ammo actually changes damage. On LoreRim this is verified at the data level
  (coordinator 2026-10-12): Requiem's RFTI_Trait_* perks on undead/vampire/werewolf/etc. test the ATTACKER's
  `WornHasKeyword(REQ_WeaponType_SilverArrow)` (fn 682, tab 1) and the attacker weapon's `HasKeyword(WeapMaterialSilver)`
  (fn 560, tab 2), so a follower wearing silver ammo gets the bonus. Vanilla SilverPerk tests the bow, not the ammo, so
  on a vanilla-perk load order silver ammo may do nothing. Field: compare damage to an Unholy foe with and without.
- (SEV-4) IsSilverAmmo uses WeapMaterialSilver / SilverPerk weapon-tab keywords; LoreRim's bonus keys on
  REQ_WeaponType_SilverArrow. Every LoreRim REQ silver round also has WeapMaterialSilver, but Katana.esp
  `AK69CommonLeadShot` (C76623) has only WeapMaterialSilver -> treated as silver, gets no Requiem bonus. Consider also
  reading the keywords the load order's Unholy trait perks test on the attacker's worn ammo.
- (SEV-4) A mid-fight bow-hold release (Equip.cpp ~1186) runs SilverHoldEnded: restore + re-nock churn bounded by
  the hold-release dwell.
- (SEV-4) Under the authority the restore goes to the declaration's best ammo, not ammoPrev; a non-pick special
  round (explosive/enchanted) worn before the episode is lost to the ordinary best.
- (SEV-4) `roles.melee == Other` counts a follower with an act.equip_melee rule but no melee weapon as ranged-only,
  so he never restocks a silver melee weapon (keeping an owned one is unaffected).
- (SEV-4) While an episode is open a player ammo pick is not recorded (the episode wins anyway); if APMF swaps the
  player's ammo to silver during the episode, that choice is not remembered afterwards.
- (SEV-5) EquipObject with the whole stack count for ammo; APMF's proven ammo equip passes 1
  (APMF channels/EquipAuthority.cpp:352). A worker-read count can be stale by the time the queued equip runs.
- (SEV-5) The SUPPLY "arrows below N" pass can buy silver arrows as ordinary kArrows when wantSilverAmmo is false;
  the bought silver then counts as owned for the restock (pre-existing shape).
- (SEV-5) Redundant IsSilverAmmo checks in AmmoObsolete / Economy off-kind / PlanBuy (AmmoIsSpecialBase pins anyway).
- (SEV-5) The empty-trade memo signature ignores wantSilverAmmo (same shape as wantSilver).

### MFO-B246 (SEV-4 x3, SEV-5 x2) -- perk owner remap P1 review, deferred items
Raised against 28f4c4d / b312d91 (`feat/mfo-perk-owner-remap-p1`, Opus review two rounds,
`scratchpad/agentlogs/review-perk-p1.md`), 2026-10-12. Final MERGE OK at b312d91. The SEV-2 co-save aloneGate breadth,
the SEV-3 hostile-aura targets line, F4/F6/F8/F9/F10 were fixed on the branch.
- (SEV-4, F3) Mixed nodes do not get "dead + refund": if rank N>1 is alone-gated but rank 1 is not, the node is kept,
  a saved rank N is neither dropped nor refunded and the follower keeps the dead rank form; if rank 1 is gated but rank 2
  is not, rank 2 is unreachable. Ask marth if it ever shows up in a census.
- (SEV-4, F7) Apply*Spell EPs (51/52/67/69) and `perkConditions` are not recursed for owner facts (gap against the
  design; conservative, nothing is demoted or killed through them).
- (SEV-4) EP condition tab 0 = owner tab is the CK/xEdit convention (reviewer); not proven from the binary.
  `runOnRef` population at kDataLoaded is unverified: watch the census `runOnPlayerRef=` counter.
- (SEV-5, N1) On a follower switch `stateOk` is false for a frame, so `ownedAt` reads all-false and the board lays out
  twice (brief selection re-clamp). Cosmetic.
- (SEV-5, N2) `ReplaceYou` treats a "You" after a colon as a subject ("Bonus for: you" reads wrong). Rare.

### MFO-B247 (SEV-4 x2, SEV-5 x3) -- M1b spell picker sort review, deferred items
Raised against 4bf1441 (`feat/mfo-m1b-spell-sort`, Opus review, `scratchpad/agentlogs/review-m1b.md`), 2026-10-12. MERGE OK.
- (SEV-4) Scope: the 6 Str keys (`native/i18n/Strings_keys.h`) + regenerated `MFO_ENGLISH.txt` were outside the brief's
  file list but required by Docs/TRANSLATING.md:67 and release.sh's template check; the commit message did not flag them.
- (SEV-4) `native/Board.cpp` is 1987 lines (1945 on main), past the ~1500 plan-a-split line: the next brief touching it
  proposes a split as its own round.
- (SEV-5) School headers share the TextDisabled style of Gb_PickHint / Gb_TeachHeader; "Teach from spellbook" followed by
  a school header reads flat. Indent or accent.
- (SEV-5) Gamepad UP scrolls only to the focused Selectable, so the header above can stay out of view one step.
- (SEV-5) The 4-key comparator lambda is duplicated per list; the "(unsupported)" suffix is still hardcoded English
  (pre-existing).

### MFO-B248 (SEV-5 x3) -- M1a review deferred items
Raised against a930f00 (`fix/mfo-m1a-equip-gem-backlog`, Opus review round 2, `scratchpad/agentlogs/review-m1a.md`),
2026-10-12. MERGE OK.
- (SEV-5) `s_declined` and `s_nextWeapUp` (logistics/EquipAuthority.cpp, B28) are not cleared on revert/load; a stale
  declined {held, best} pair can suppress one legitimate upgrade for the session. Clear on revert or time-bound it.
- (SEV-5) The held-silver gate blocks a follower whose ORDINARY primary is a looted silver weapon from the
  out-of-combat upgrade. Narrow to an actual silver episode (needs a public `SilverWeaponEpisode` query).
- (SEV-5, pre-existing root) Under bWeaponStyleControl off (direct mode) a silver weapon left in the hand after combat
  stays: `SilverHoldEnded` returns 0 for direct episodes and never restores the main weapon. B28 used to mask it.

### MFO-B249 (SEV-4 x2) -- M2 roster removal review, deferred items
Raised against db4c03a (`feat/mfo-m2-roster-remove`, ClickUp 86e3eewaf + 86e3940zh, Opus tier-3 review,
`scratchpad/agentlogs/review-m2.md`), 2026-10-12. No SEV-1/2; the SEV-3s (F1 baseTouched premise, F2 UndoSkills
over non-MFO writes, F3 not-tracked != not-following), F5 (main-thread g_followers access, threading carve-out) and
F7 were fixed on the branch. Cross-referenced from MAP.md (progression/ ROSTER REMOVAL undo, Followers.cpp roster removal).
- **F4 (SEV-4)** -- auto-calc followers at their level cap keep the reshaped HMS/skills. `UndoHms` leaves a LEVELING
  follower's H/M/S (and `UndoSkills` puts REVERT-mode skills at the enrollment baseline) on the premise that the
  engine re-slams its own auto-calc values at the follower's next level-up; a follower already at his level cap never
  levels again, so MFO's reshape stays. Field check: remove one, save, reload, log base HMS + skills.
- **F6 (SEV-4)** -- the allocated rank form is removed even when the load order now grants it natively.
  `ForgetFollower`'s perk removal (like Respec's, which does the same) removes the held rank form whenever it is on
  the base; it cannot tell MFO's grant from a native grant of the same form (SPID/Requiem). Fix shape when drained:
  a per-session record of the forms MFO actually AddPerk'd.

### MFO-B251 (SEV-5 x2) -- engage-on-sight hostility review, deferred items
Raised against f1e4890 / b660df0 (`fix/mfo-eos-faction-hostility`, Opus review 3 rounds, `scratchpad/agentlogs/review-eos.md`),
2026-10-12. MERGE OK. The SEV-1 (faction-only test), the SEV-3 territorial ally `||` and the SEV-4 skip list over-reach
were fixed on the branch.
- (SEV-5) `ReactionOf` (EngageOnSight.cpp, the data-only stand-in for the engine's one-direction faction reaction)
  ignores reaction entries keyed by RACE, and has no relationship-rank step (`BGSRelationship::GetRelationship` has no
  VerifiedAddresses row). A friend-by-relationship territorial NPC could read as Neutral.
- (SEV-5) Field watch: the territorial term has no "radius entered" runtime flag (by marth's ruling), so a territorial
  animal anywhere within leash and sight is engaged even when it has not noticed anyone. Watch the per-actor skip log.

### MFO-B252 (SEV-4/5 x3) -- archer dual-cast heal review, deferred items
Raised against 447d2f8 (`fix/mfo-heal-archer-dualcast`, Opus review, `scratchpad/agentlogs/review-heal-archer.md`), 2026-10-12.
The SEV-2s (decline vs ruling, session-long launcher memory) and SEV-3s were fixed on the branch. Cross-referenced from
MAP.md (ARCHERS HEAL BY DUAL-CASTING).
- **(SEV-4)** Exact mode (`iCastControl` 4): `Loadout.cpp:~424-430` strips a RIGHT-hand proxy from the hands; a dual
  heal's proxy is armed on both hands, so exact-mode Prepare can deselect the right copy. Same family as MFO-B227.
- **(SEV-5)** A dual heal is `heal[0].hand == kApmfHandDualCast` in the LEFT slot only. Future readers that map a claim's
  `hand` to a watch slot (`WatchSlot`, silentPastGate's `c.hand == kApmfHandLeft ? Left : Right`) will read it as RIGHT.
  Map DualCast to the left slot there before adding such a reader.
- **(SEV-4/5, round 3 notes)** (a) The renewed `archerFloor` stamp keeps a launcher user weaponless for as long as a
  heal keeps being attempted without firing (each lap re-stamps before Try's result is known). (b) `silentPastGate` is
  skipped for the archer floor, so a silent driver does not drop it. (c) `NoteArcherRightFloor` creates the `g_owned`
  entry itself; an entry holding only the stamp is erased by the next `EraseIfEmpty` unless a floor stands. (d) ABI 19:
  the full floor is minted there where MFO previously minted none; untested on a real ABI 19 Harbinger.
- **(SEV-5)** `kObservedFiringRecencyMs` 5000 rests on two inter-fire gaps (1.35 s, 3.78 s; n=2). Re-measure from the
  next field log before trusting it as more than a floor.

### MFO-B253 (SEV-5, OPEN, tools) -- cosave_roundtrip leftovers from the M3 review (raised against d8f6a68)
Opus review of `fix/mfo-m3-cosave-tool`, MERGE OK; everything above SEV-5 was fixed in the closing round. Left open:
- The seeded v<8 retro never exercises the `HmsNpcRates` unreadable branch, the `fNPCHealthLevelBonus <= 0`
  branch, or the fixed-stat / grant-history SKIP branch (README lists them). Fix shape: more seed variants.
- `_fdsign` / `_dsign` / `_ldsign` CRT imports are unmodelled (logged by name only).
- The "engine call not modelled" fault names the immediate caller (`report_and_fail`), not the MFO function that
  started the chain. Fix shape: walk the emulated stack for return addresses.
- The GMST placeholder values are not LoreRim's real ones. Re-run on the Deck's real v6/v7 saves when it is awake.

### MFO-B254 (SEV-4, OPEN, Scheduler) -- EquipRangeUndecidable lacks the PickFoe trackability gates (raised against 5c01051)
Opus review of `fix/foe-targeted-unholy`; pre-existing, not changed by that branch. Reviewer text, verbatim: "Pre-existing for range rules, worth carrying over: `EquipRangeUndecidable`'s "usable" test omits the trackability gates (3D, kTargetLost) that `PickFoe` applies. A lost but non-null target therefore still reads known-false. A silver demotion should use the same gates as `PickFoe`."

### MFO-B255 (SEV-4) -- focus key can still send followers at the player's own summon
Raised against 297ca4c (`fix/focus-skip-followers`, Opus review), 2026-10-12. Reviewer (verbatim): "The remaining hole: the player's own summon or atronach, which is a teammate but not tracked with summons off. If the player hits it by mistake and it becomes their combat target, followers are still sent after it. The fix is cheap: add `&& !a->IsPlayerTeammate()` (and the same on `pt`) to both filters. It is a flag read already used off-worker (logistics/Sinks.cpp:253), and the skip log text would say \"one of your allies\"." Not fixed in the branch: outside the brief's spec (filter = the crosshair path's player + tracked).

### MFO-B258 (SEV-4, OPEN, EngageOnSight) -- a no-progress end on an ActivelyHostile target beyond R is misread as a range drop
Raised against 863106c (`feat/mfo-engage-range`, Opus review MERGE OK, 2026-10-12). `Service`'s Ended branch calls an end a range drop when the target is alive, `IsEnemy` and beyond `Note::engageRange`. Harbinger's no-progress / arrived-but-not-fighting ends on a hostile target that happens to be past R look identical, so the target is not given up and a bounded re-engage cycle follows. The review log (`agentlogs/review-mfo-engage.md`) records only the verdict, so this is the coordinator's relay of the finding, not the reviewer's verbatim words. Fix shape: Harbinger exposes the end reason.

### MFO-B259 (SEV-4, OPEN, EngageOnSight) -- the drop is classified at Service time, not drop time
Raised against 863106c, same review. The range test runs when `Service` sees the Ended state, not when the bridge sweep marked it. A target back inside R in that window is wrongly given up. Relayed, not verbatim (see MFO-B258). Fix shape: Harbinger exposes the end reason, or MFO samples distance when the bridge sweep marks the entry Ended.

### MFO-B260 (SEV-4, OPEN, EngageOnSight) -- the 0.9 band is only 50u at the 500u minimum
Raised against 863106c, same review. With `fEngageOnSightRange` at its 500u minimum the start margin `kStartRangeMargin` x R leaves a 50u band, so a receding target can be engaged and dropped at once. Relayed, not verbatim (see MFO-B258). Fix shape: start inside min(0.9R, R - 250u).

### MFO-B261 (SEV-5, OPEN, EngageOnSight) -- "another cell" ends compare raw positions
Raised against 863106c, same review. Harbinger ends a hold for a target in another cell, and the range-drop test compares raw `GetPosition()` distances across cells. Relayed, not verbatim (see MFO-B258).
### MFO-B256 (SEV-5, OPEN, cast/EquipWeigh.cpp) -- D3 preference re-sent on every per-cast claim change (raised against e06a54f)
Opus tier-3 review of `feat/mfo-weighting-w1w2` @ e06a54f. Reviewer's finding, verbatim: "SEV-5: the preference is re-sent each time a per-cast claim comes or goes, which means info-level log churn." Reasoning: `BuildPreference` drops a weapon entry while MFO's own cast claim holds its hand (D3), so every cast claim that takes and frees a hand changes the ch.26 declaration and costs one `SetEquipPreference` enqueue plus one `[equip-pref] ... DECLARE` info line (and a new escalation episode). Harbinger already withholds the lift for a hand a cast claim holds (`kEquipPrefState_HandHeld`), so the churn buys nothing but strict D3 wording. Fix shape when drained: keep the entry and let Harbinger's HandHeld do D3, or debounce the D3 drop. Surfaced at edit time from: MAP.md section 2 `cast/Equip.cpp` W1 entry.

### MFO-B257 (SEV-3, RESOLVED in 09d1056, pre-existing, cast/Equip.cpp CoLoadForcedWeapons) -- under the authority, the FWPN load still force-unequips Harbinger-placed non-forced weapons on every load (raised against e06a54f)
Opus tier-3 review of `feat/mfo-weighting-w1w2` @ e06a54f; PRE-EXISTING (not changed by that branch). Reviewer's finding, verbatim: "Pre-existing, not new: under authority the FWPN load still force-unequips Harbinger-placed (non-forced) weapons on every load." Grading: raised from SEV-4 to SEV-3 at the re-review of 67722ac under the CLAUDE.md rule 9 carve-outs (a) co-save area and (b) the next field cycle exercises it, so fixed now, not deferred. Reasoning: FWPN saves every ledger pair, and `CoLoadForcedWeapons` force-unequips each one on load to clear the T#76 prevent-removal lock. Under the ch.17 authority (and the W1 weighting road) those holds were placed by Harbinger's equip pass, non-forced, so there is no lock to clear and the load only strips the follower's weapon (the MFO-B64 empty-handed-after-load shape). The runtime `ForcedHold::forcedRight/forcedLeft` flags that would tell the two apart are deliberately NOT serialized (no co-save change in a W1 round). Fix shape when drained: a co-save decision for marth (persist which pairs were force-equipped, an FWPN version bump with a reader for v1), or skip the load unequip when the pair's weapon is not locked (read `ExtraCannotWear` on the worn entry, ENGINE_NOTES 0.48). Co-save area: tier A. Surfaced at edit time from: MAP.md section 2 `cast/Equip.cpp` W1 entry.
- **RESOLVED 09d1056 (`feat/mfo-weighting-w1w2`):** the second shape, with NO format change. `CoLoadForcedWeapons`' posted main-thread body (#62 kept) force-unequips a pair's weapon, and in the `weapon == 0` sweep each held weapon, only when its inventory entry's `extraLists` carry `ExtraDataType::kCannotWear` (0x3D, `ExtraDataList::HasType`, mit-3.7 fork). It logs `stale force-hold cleared on load` only when a lock was found, else `force-hold on load: no lock (placed non-forced) -- left in hand`. The FWPN bytes, versions and reader are unchanged.

### MFO-B262 (SEV-3 field gap, FIXED 2026-11-08: Harbinger ABI v33) -- ch.26 state is per ACTOR, so D4 cannot classify a miss of any entry but the last scored (raised against 67722ac)
Opus re-review of `feat/mfo-weighting-w1w2` @ 67722ac. Finding (coordinator's relay of the review): Harbinger's `Record` (core/EquipPreference.cpp:213-228 @17650fc) overwrites the per-actor `lastForm` and `state` on every scoring; a pass scores all declared entries back to back, so the reported item is almost always the LAST one scored, in a stable order. With two entries (R sword + L shield, or dual wield) MFO's `info.lastForm == missForm` gate (cast/EquipWeigh.cpp `ServicePreference`) can pin D4 off for the earlier-scored entry forever. Mitigation in 09d1056: one INFO line per episode, `[equip-pref] <id>: miss of '<item>' not classified -- Harbinger reports only '<lastForm>' (per-actor state); no D4 for it`. **Request (append-only Harbinger ABI, for marth to schedule):** a per-entry state read, e.g. `std::uint32_t (*GetEquipPreferenceItemState)(RE::FormID actor, RE::FormID form, std::uint8_t slot, APMF_EquipPrefInfo* out)`, so D4 reads the state of the missing item itself. Surfaced at edit time from: MAP.md section 2 `cast/EquipWeigh.cpp` entry.
- **FIXED 2026-11-08 (MFO 44debd4 on `feat/apmf-api-batch`, Harbinger 967143b on `feat/api-batch-b62-ch17`, ABI v33):** Harbinger keeps a scoring record per (actor, item, hand) and `APMF_API_v33::GetEquipPreferenceItemState(actor, form, slot, out)` reads it. `ServicePreference` (cast/EquipWeigh.cpp) classifies the missing entry by its OWN state (`mstate` / `minfo`) when Harbinger is ABI >= 33, so D4 can escalate for any entry. Below 33 the per-actor read and the once-per-episode `not classified` line stay.

### MFO-B263 (SEV-4, OPEN, logistics/Upkeep.cpp) -- `s_skipAt` is never cleared (raised against cc3716d)
Opus review of `fix/mfo-torch-place-logging` @ cc3716d. Reviewer's finding, verbatim: "`s_skipAt` is never cleared at revert, at dismissal or in `ClearTransientState`. `g_torchRefusedLogged` is cleared in `ClearTransientState`. The only effect is that a stale 10-second window can hide the first skip line after a load or re-recruit, and the map grows only by follower count."

### MFO-B264 (SEV-4, OPEN, logistics/Upkeep.cpp) -- the torch-skip suppression hides a real foreign Left hold while any placement is pending (raised against cc3716d)
Opus review of `fix/mfo-torch-place-logging` @ cc3716d. Reviewer's finding, verbatim: "the new comment overclaims. It reads 'A placement of MFO's OWN already waiting owns Left', but `PlacementPending` is true for any pending placement, including a right-hand one. While any placement is pending, a real foreign Left hold's skip line is hidden. The return value is unchanged (false either way), so this is logging only. A precise fix would check for a pending placement in the left slot." (The comment wording was corrected in the follow-up commit. The logic is left.)

### MFO-B265 (SEV-5, OPEN, cast/EquipWeigh.cpp) -- "attempt N in a row" can overcount (raised against cc3716d)
Opus review of `fix/mfo-torch-place-logging` @ cc3716d. Reviewer's finding, verbatim: "'attempt N in a row' can overcount. The streak resets only when a pending placement is seen landing. If the item lands by another road, such as the engine's own AI torch equip with no wish pending, the next failure still counts on from the old streak."

### MFO-B269 (SEV-4, OPEN, ComposedCast.cpp ChooseHealRoad) -- a kTargetLocation self heal still needs DirectNoSeat (F2, raised against db940a0)
Opus review of `feat/mfo-animated-combat-casts` (substrate S1) @ db940a0. The review log (`agentlogs/review-substrate-s1.md`) was never given a separate "verbatim backlog text" section; its text for this finding is, verbatim, the clause "heal road lacks R2-1 never-landed bound + kTargetLocation mirror" and "APMF ControlMap self-flip block (gate: beneficial, non-placement, non-kTargetLocation; no conc exclusion)". Meaning: Harbinger's flip does not serve a kTargetLocation spell, the buff road mirrors that (`BuffSelfRuneWhy`), and R2 removed ChooseHealRoad's whole DirectNoSeat test. Fix shape: keep DirectNoSeat for kTargetLocation self heals. Surfaced at edit time from MAP.md "COMBAT SUBSTRATE SLICE S1".
Reviewer (verbatim, supplied by the coordinator from the review report): "The R2 chooser does not mirror Harbinger's self-flip refusals. A kTargetLocation (non-placement) heal at its own caster, an unflagged Calm/Frenzy/Demoralize/Paralysis/Stagger archetype in a Heal-kind spell, or a self-flip pool overflow leaves the explicit-self claim with no served handle (ControlMap.cpp SELF-FLIP block: 'NO self-flip proxy; left as before' / 'no self-flip proxy could be had'), so the claim stands unserved until the 4 s never-observed release and repeats every lap, where db940a0's parent cast it direct (DirectNoSeat). Fix: keep DirectNoSeat in ComposedCast::ChooseHealRoad for kTargetLocation delivery (as BuffSelfRuneWhy does for buffs). Reviewer reasoning: ClassifySpell already excludes hostile/detrimental, so only the kTargetLocation and overflow shapes are realistic; mod heals are where they appear."

### MFO-B270 (SEV-3, FIXED 2026-11-08: Harbinger APMF-B62) -- the self-flip claims draw on the shared 8-slot proxy pool (F3, raised against db940a0)
Same review, verbatim from its log: "pool B62 pressure". Every non-Self heal / buff at the caster or an ally mints a delivery-flip proxy from one shared 8-slot pool (APMF-B62); R2, R9 and the AUTO caster candidate raise the demand. Drain APMF-B62 before a release cut.
Reviewer (verbatim, supplied by the coordinator from the review report): "R2 widens the consumers of Harbinger's 8-slot proxy pool (shared, keyed per owner/spell/hand/flip) to every non-Self self heal, beside the forward flips of ally kSelf heals and buffs. On overflow the claim's handle is left invalid and it stands as a plain deny, a dead heal until the never-observed release. Drain APMF-B62 (size the pool from the field's peak concurrent proxies, or per-owner slots) before release. Reviewer reasoning: before S1 a self heal of a non-Self delivery took no slot."
- **FIXED 2026-11-08 (Harbinger 967143b on `feat/api-batch-b62-ch17`, APMF-B62, no ABI change, no MFO code):** the pool holds up to 64 slots (three hand keys for 21 actors), minted lazily, so legitimate demand cannot overflow it. An overflow at the cap is a counted ERROR line naming the owners that hold the slots.

### MFO-B271 (SEV-4, OPEN, Scheduler.cpp:1365) -- the retreat-heal path changes behaviour and its comment still says "NoSeat" (F6, raised against db940a0)
Same review, verbatim from its log: "retreat-heal behaviour change". The Scheduler's `ChooseHealRoad(f, sp, f) == Claim` test now answers Claim for a non-Self heal at self (it answered DirectNoSeat). Check the intent and fix the comment.
Reviewer (verbatim, supplied by the coordinator from the review report): "Scheduler.cpp:1365 retreat self-heal: a non-Self-delivery self heal used to answer DirectNoSeat and fire while retreating; it now answers Claim and is skipped (`continue`) for as long as a CombatController lingers after StopCombat, so it waits until DirectNoCombat. The comment there still lists 'NoSeat' among the roads that run. Reviewer reasoning: narrow window; behavioural change not named in the brief."

### MFO-B272 (SEV-5, OPEN, cast/Unanimated.cpp) -- `g_unanimatedLast` is never cleared (raised against db940a0)
Same review. A stale 30 s window can hide the first line after a load; the map grows only by (follower, spell, road). Fix shape: clear it from `ClearCastLocks`.
Reviewer (verbatim, supplied by the coordinator from the review report): "cast/Unanimated.cpp's rate-limit map (follower, spell, road) -> time is never cleared (not on ResetAllState, not on dismissal). Growth is bounded by distinct triples and small, but it outlives the session state it describes. Clear it from the cast reset path."

### MFO-B273 (SEV-5, OPEN, cast/Auto.cpp) -- the R10 "next lap picks the next foe" comment overclaims (raised against db940a0)
Same review. For an instant spell `ShouldApplyTo` is always true, so the same nearest foe is picked again (focus fire); only a DoT moves the series on.
Reviewer (verbatim, supplied by the coordinator from the review report): "Auto.cpp's R10 block says 'the lock is dropped by CastOn when the cast has fired, and the next lap picks the next foe'. For an instant offense spell the next lap picks the NEAREST foe again, usually the same one; only a DoT moves on (HostileAutoFoes' ShouldApplyTo drops a covered foe). Reword to 'the next lap picks again (the nearest uncovered foe)'."

### MFO-B274 (SEV-5, OPEN, MAP.md) -- MAP's `cast/CastOn.cpp` line count was stale (raised against db940a0)
Same review. MAP said 1657, the file is 1742 (corrected in the closing round; it is past the ~1500 plan-a-split mark, marth 2026-10-05: do not split, `CastOn()` is one function).
Reviewer (verbatim, supplied by the coordinator from the review report): "MAP.md:496 still gives cast/CastOn.cpp as 1657 lines; it is 1742 on the branch (1725 on main). Update the figure. The 'do NOT split it' ruling (marth 2026-10-05) stands, and the file is below the 2500 backstop."

### MFO-B275 (SEV-4, OPEN, cast/HealObs.cpp) -- the self-flip bar is cleared mid-fight by the no-cast-rule path (R3, raised against 3859cd8)
Opus focused re-review of `feat/mfo-animated-combat-casts` @ 3859cd8, verbatim from its log: "Bar cleared mid-fight by Scheduler no-cast-rule ClearCastLock (2019) -> 'rest of the fight' wording wrong, bounded. SEV-4." The WARN text was reworded to "until the cast rule lapses or the fight ends"; the clearing itself is by design and bounded.
Reviewer (verbatim, supplied by the coordinator from the review report): "`ClearHealNeverFired` is reached from `ClearCastLock` at combat end (Scheduler ~1043), at dismissal (Followers:467), and on the no-cast-rule-held path (Scheduler:2019, `healHeld == 0`). That last path clears the bar mid-fight whenever no cast rule holds for one tick. The spell then retries the claim and can take up to three more misses before it is barred again. This is bounded, and it matches the existing never-fired latch."

### MFO-B276 (SEV-5, OPEN, cast/HealObs.cpp) -- a posted self-flip verdict can land in the next fight (R5, raised against 3859cd8)
Same re-review, verbatim from its log: "Stale posted verdict into next fight SEV-5." The `[heal-obs]` verdict is posted to the main thread about 1 s after the fire and counts into `g_selfFlipMiss` after the fight's record was cleared.
Reviewer (verbatim, supplied by the coordinator from the review report): "A verdict posted near fight end can count one stale miss into the next fight's map."
### MFO-B277 (SEV-4, OPEN, Taunt.cpp) -- a follower leaving the party is not a taunt release reason (raised against 101cf49)
Opus tier-3 review of `feat/mfo-taunt` @ 101cf49. Reviewer's finding, relayed from its log (design 4.5 item 4 lists "the follower leaves the party" as a release reason): "Release paths ok except 'follower leaves party' (design 4.5.4) missing." A dismissed taunter's pins last until the duration or another reason fires.

### MFO-B278 (SEV-4, OPEN, Taunt.cpp) -- the sense gate barely filters, and ABI 17 has none (raised against 101cf49)
Same review. Reviewer's finding, relayed: "SenseActor hearing-combat makes sense gate ~vacuous (SEV-4); ABI17 no sense (SEV-4)." Below ABI 18 `SenseOf` is unusable and the foe is rolled with `sense=unknown`.

### MFO-B279 (SEV-4, OPEN, Taunt.cpp / apmf/TauntClaims.cpp) -- IdleForceDefaultState on release, and Repoint on a dead handle (raised against 101cf49)
Same review (F6, relayed). Releasing a still-held ch.12 idle makes Harbinger send IdleForceDefaultState once, which can cut a clip short, and `PinFoeToTaunter`'s Repoint of a handle that has just died is a silent no-op until the next Ended() read. Text not preserved verbatim in the review log; re-check against the review before draining.

### MFO-B280 (SEV-4, OPEN, Taunt.cpp / apmf/TauntClaims.cpp) -- an Accepted-but-never-confirmed idle holds the follower during the window, and the next attempt follows (raised against 7bdacd3)
Opus re-review of `feat/mfo-taunt` @ 7bdacd3 (residual of F2). The ch.1 hold is filed once Harbinger accepts the play, so an idle that is Accepted and then never confirmed (NotConfirmed after the 3 s window) holds the follower still for that window, and since there is no cooldown the next attempt can follow it. Consistent with "no cooldown", recorded so the field run knows to look for it.

### MFO-B281 (SEV-4, OPEN, Board_FieldKit.cpp) -- the gambit picker still offers Taunt while bTaunt=0 (raised against f916d41)
Opus tier-B review of `fix/mfo-taunt-off-switch` @ f916d41. Outside that brief (it gated dispatch only); marth to decide whether to hide it for the release.
Reviewer (verbatim): "the gambit board still offers Taunt in the action picker (`Board_internal.h:234`, `Board_FieldKit.cpp:818`). A player can author a Taunt rule that silently does nothing. The only hint is the MCM help sentence on the duration slider plus the one log line."

### MFO-B282 (SEV-5, OPEN, Taunt.cpp) -- the "[taunt] disabled" line never re-arms (raised against f916d41)
Same review. Reviewer (verbatim): "`s_said` never re-arms. If the INI goes 0→1→0 within one session, the disabled line will not print again. Cosmetic."

### MFO-B283 (SEV-4, OPEN, progression/Allocator.cpp / logistics/EquipAuthority.cpp) -- declaration deferred forever when MainThread is not installed (raised against 538e8be)
Opus tier-3 review of `fix/mfo-equip-self-violations` @ 538e8be. Reviewer (verbatim): "if `MainThread` is not installed or `g_dead` (seat refused, VR), `PollTick` never runs. Every enrolled follower's declaration is then deferred forever and MFO equips nothing through the authority. This was already broken for progression, but now equipping fails too, without any warning (SEV-4)." Fix: a loud warning (or no deferral) when MainThread is absent.

### MFO-B284 (SEV-5, OPEN, Scheduler.cpp) -- g_partyCombat stays stale while g_active is empty (raised against 538e8be)
Same review. Reviewer (verbatim): "`Scheduler::Tick` returns early when `g_active` is empty (before the party-combat block), so the flag can stay stale while the party is empty. Debts for dismissed followers go through `Restore`, so the effect is benign (SEV-5)."

### MFO-B285 (SEV-5, OPEN, logistics/Gear.cpp) -- owned-hand upgrade skip ordering vs weighing; one-cycle stale scope (raised against 538e8be)
Same review. Reviewer (verbatim): "The skip comes before the weighing branch. Under weighing, the placement used to be 'kept pending' until the hold released ... Weighing is OFF in production ... so this is moot today." and "`EquipAuthorityOwns` reads the last scope sent. A just-released hold that hasn't been re-sent, or a pending right-hand placement or torch placement owning Left ... skips the upgrade for one cycle."

### MFO-B286 (SEV-5, OPEN, cast/Equip.cpp / cast/EquipSilver.cpp) -- style-control toggle edge and re-arm ordering after a skipped DeclareLedger (raised against 538e8be)
Same review. Reviewer (verbatim): "ON→OFF releases holds on the next reconcile (`Equip.cpp:1289`). A silver `!styleControl` plain equip in the same lap can hit a still-owned Right once, then succeeds." and "If the `DeclareLedger` send right before them was skipped (change detector, 3D-absent, deferral), APMF may still own Right with the silver in-set, and the main-weapon re-arm is refused."

### MFO-B287 (SEV-5, OPEN, plugin.cpp / Loadout.cpp) -- load-time Reconcile can equip a shield the first declaration then unequips (raised against 538e8be)
Same review. Reviewer (verbatim): "load-time `Loadout::Reconcile` (`plugin.cpp:519`, before any claim). It can equip a shield that the first declaration then makes APMF unequip, so there is one equip/unequip churn per load. This predates the branch."

### MFO-B288 (SEV-5, OPEN, logistics/Upkeep.cpp) -- the "[heal] ... NOT healed" skip logs every service lap while the hold lasts (raised against 0d38508)
Author's own note on the closing round: the skip in `HealExcludedWeapon` logs on every poll while a hold owns the hand. Rare case (an excluded weapon in a held hand); latch it per follower if it proves noisy.

### MFO-B289 (SEV-4, OPEN, logistics/EquipAuthority.cpp) -- in combat, a hold whose item returns is not re-declared until the next gambit/ledger trigger (raised against d797089)
Opus tier-B review of `fix/mfo-drop-uncarried-holds` @ d797089. Reviewer (verbatim): "In combat: the only rebuild triggers are gambit equip, off-hand top-up and bound weapon live/ended (`Equip.cpp:578, 598, 789, 796, 1223`). There is no inventory-change trigger. If a declaration is built while the weapon is gone (for example a Disarm with an AI pickup) and the weapon then returns mid-fight, the hand stays unowned until the next gambit fire or combat end. ... this degrades softly (the AI arms the hand, no lockout)."

### MFO-B290 (SEV-5, OPEN, logistics/EquipAuthority.cpp) -- the "no longer carried" log latch never re-arms; its text over-promises (raised against d797089)
Same review. Reviewer (verbatim): "the `g_uncarriedLogged` latch (:417) is only cleared on forget or revert. A second loss of the same item in one session is not logged again (log only)." and "The log text 'the next equip gambit re-picks' (:784-786) is optimistic, because a gambit that sees its pick already satisfied may not call `DeclareFromLedger` again."

### MFO-B291 (SEV-5, OPEN, logistics/EquipAuthority.cpp) -- jewelry: copies with no ExtraDataList are never scored when the entry has lists (raised against c3b55dd)
Opus tier-B review of `fix/mfo-jewelry-all` @ c3b55dd. Reviewer (verbatim): "when an entry has one or more ExtraDataLists, copies that have no list are never scored (`:676-679`). Example: one list is RemovalHeld and two plain copies have no list, so that base offers no gem-host candidate. This is harmless."

### MFO-B292 (SEV-5, OPEN, logistics/Economy.cpp) -- jewelry: a displaced worn ring may be sold as redundant (raised against c3b55dd)
Same review. Reviewer (verbatim): "`DeclaredSetKeeps` now reports a displaced worn ring as not kept. `Economy.cpp:725` may then sell it when it is `redundantInferior`. On main a non-mage's worn ring was always kept by rule 5. Player picks are still protected by `IsPlayerPick`."

### MFO-B293 (SEV-5, OPEN, logistics/SwapUp.cpp) -- jewelry: DeclaredJewelry runs with APMF absent (raised against c3b55dd)
Same review. Reviewer (verbatim): "`DeclaredJewelry` in `SwapUp.cpp` runs even when APMF is absent. It keeps a ring the non-mage will never wear and costs an extra slate build per sell pass." Also: the legacy APMF-absent EquipBestOwnedGear still gives non-mages no jewelry.

### MFO-B294 (SEV-5, OPEN, logistics/EquipAuthority.cpp) -- jewelry: mage reconcile scores an arbitrary instance; [jewelry] log scores the chosen, not the worn, copy (raised against c3b55dd / 39ca40f)
Same review: "The mage reconcile's `mxl` (`:698-700`) takes an arbitrary instance's list when the mage's pick is not the worn base. That only affects the score comparison, never the base, so there is no churn." and "the engine may put on a plain copy of the chosen base. The `[jewelry]` line logs the chosen instance's score, not the worn copy's." Author note on 39ca40f: the uid tag in decl.names replaces the first occurrence of the display name (log only).

### MFO-B295 (SEV-5, OPEN, logistics/Economy.cpp) -- comment says player-given weapons are protected; only LotD relics are (raised against faf0c0f)
Opus review of `fix/mfo-potions-roles` @ faf0c0f (F8). Verbatim: "the comment at `Economy.cpp:~796` says player-given weapons are protected. Only museum relics are (Lotd); an ordinary bow the player handed over is not protected by sell or by Shed."

### MFO-B296 (SEV-5, OPEN, logistics/Economy.cpp) -- off-family launcher sale rides on a carried-derived ammo family (raised against faf0c0f)
Same review (F9). Verbatim: "selling a launcher outside the chosen ammo family goes beyond the ruling. The family choice (`wantCrossbow`) is itself worked out from what is carried and the ammo held."

### MFO-B297 (SEV-4, OPEN, logistics/Upkeep.cpp / Gear.cpp) -- perk-derived ranged role needs the progression add-on; no doRanged source log before edf5df5
Same review (F6). Verbatim: "the perk part of the ranged role needs the progression add-on. Without it the vote array is empty (`Progression.h:403`)." (edf5df5 added the [role] source line; the add-on dependency stands.)

### MFO-B298 (SEV-4, OPEN, apmf/ErrandClaims.cpp) -- a Superseded ch.12 idle claim is dropped without Release (raised against e26db0d)
Opus re-review of `feat/mfo-town-r1a` @ e26db0d. Verbatim: "`kIdleEnd_Superseded` is not an ended claim ... The new `endCause != None` test drops `c.idle` without calling `Release`. That leaks a standing ch.12 claim at `kOwnBasis` until the next load. If that claim wins ch.12 back, it would replay a stale idle, and no `Finish` would ever release it." Fix: `api->Release(c.idle)` before invalidating (safe on an ended claim; same line on the ClaimDead path).

### MFO-B299 (SEV-4, OPEN, logistics/Errand.cpp / Service.cpp) -- town R1a: errand Tick gets healWants, not tripHealWants; GemRules Sale holds linger outside towns; TownAnchor static before kDataLoaded (raised against 2e1f5a2)
Opus review of `feat/mfo-town-r1a` @ 2e1f5a2: (1) "`Tick` gets `healWants`, not the `tripHealWants` shape (`Service.cpp:408`). The rule loop never runs during an errand, so no heal is ever stamped" (out of combat in town, up to 240 s); (2) "GemRules Sale holds now linger outside towns. `ReleaseHoldsExcept(..., Sale, ...)` runs only inside `EconomyProbe`, which is now never called outside towns"; (3) "`TownAnchor` is a function-local static built from `TESDataHandler` on first call ... if anything ever calls it before kDataLoaded it caches an empty keyword set forever."

### MFO-B300 (SEV-5, PARTLY ADDRESSED, logistics/Errand.cpp) -- town R1a: queue/plan churn, backstop slot under-count, vendor convergence, own-cell catalog, recall placement (raised against 2e1f5a2 / 0574a95)
Same reviews: a QUEUED follower re-plans (cell + inventory walk) and logs every 6 s; kLeg_Failed → Finish + retry ~7 s line churn while slots are full; a distance-backstop arrival leaves the ch.19 leg live (holds a slot) and CountWalking under-counts it; all followers rank vendors the same way and converge (others hold the give pose up to 33 s on the per-chest guard); the catalog walks only the follower's own cell (4096 u); recall MoveTo 250 u behind the player has no navmesh check; fast travel out of town triggers a redundant recall.
- PARTLY ADDRESSED 2026-10-19 (town phase 2 R5, `feat/mfo-town-p2-r5-trips`): vendor convergence -- the trip planner (`logistics/ErrandTrip.cpp` `Trip::Plan`) charges a crowd penalty (150 gold-equivalent per OTHER follower whose trip is heading to that chest, `Trip::ChestCrowd`) in the sell partition and when it adds a stop. The other parts (queue/plan churn, backstop slot under-count, own-cell catalog, recall placement) stay OPEN.

### MFO-B301 (SEV-4, OPEN, TradeBridge.cpp / Economy.cpp) -- town R1b: empty-trade memo key mismatch when a faction-planned shop trades with a resolved vendor (raised against 393632c)
Opus re-review of `feat/mfo-town-r1b` @ 393632c. Verbatim: "The shop is planned from outside (keyed by faction), but the trade then runs with a resolved vendor actor: picked up on screen through `EconomyProbe` kExecute, or replayed after a Repoint set `r.vendorActor`. The memo lands under the actor, and the next plan from outside looks up the faction key. Suggested fix: `SkipTrade` checks both keys, or `ReportTrade` files under both when `memoKey` differs from the actor."

### MFO-B302 (SEV-4, OPEN, logistics/ErrandSim.cpp) -- town R1b: a resolve-to-now that can't dispatch (chest busy) ends without the trade (raised against 393632c)
Same review (accepted deviation). "Nothing is lost or minted, because nothing moves until `RunTrade` runs. The usual cause is another follower's live order on the same chest ... Optional backlog item: one short retry window before dropping." Also from the 6d48c00 review: ResumePhysical ignores the kMaxWalking budget (Errand.cpp:543); RepointErrandTravel's result is ignored (Errand.cpp:826).

### MFO-B303 (SEV-5, OPEN, logistics/Errand.cpp / MAP.md) -- town R1b small edges (raised against 393632c)
Same review: `Tick` Finishes on "town errands / economy switched off" even for an unloaded follower with no hand-off/recall (~:797, manual toggle only); the at-shop ResumePhysical failure drops the trade instead of resolving to now; MAP.md R1b line refs a few lines stale (tier C).

### MFO-B304 (SEV-4, DUPLICATE of MFO-B304 (the identical entry below), Loadout.cpp) -- ranged: heal-quiet stamping and combat-flag flaps (raised against e63db74)
Opus review of `fix/mfo-ranged-approach` @ e63db74. F3: "For the two-hander (bow) debt, `healSeenAt` is stamped only while `HealTakesLeft` is true ... A claim that fired more than 2 s ago and still stands still gets the bow back." F5: "`healQuiet` uses the actor's own `IsInCombat()`, which flaps mid-fight ... A flap tick reads as quiet and gives the bow back."

### MFO-B305 (SEV-4, DUPLICATE of MFO-B305 (the fuller entry below), apmf/OffenseApproach.cpp / Evaluator.cpp) -- ranged: dwell tuning; disagreeing selector rules reset each other's clocks (raised against e63db74)
Same review. F8: kSightDwellMs 1500 vs 1-2 s field flicker still allows a ~3 s claim/release cycle — tune from field data. F7: "When two own-ray foe rules disagree on the same lap, the occluded rule erases the visible run every lap ... Bounded by `kStaleMs`, death and combat end." Author's round-2 spots: PinnedTarget/CombatEntryStateOf read every PickFoe pass (g_mx, unprofiled); LogChaseCapWaiver static map assumes single-threaded PickFoe.

### MFO-B304 (SEV-4, OPEN, Loadout.cpp) -- ranged: heal-quiet stamping and combat-flag flaps (raised against e63db74)
Opus review of `fix/mfo-ranged-approach` @ e63db74. F3: "For the two-hander (bow) debt, `healSeenAt` is stamped only while `HealTakesLeft` is true ... A claim that fired more than 2 s ago and still stands still gets the bow back." F5: "`healQuiet` uses the actor's own `IsInCombat()`, which flaps mid-fight ... A flap tick reads as quiet and gives the bow back."

### MFO-B305 (SEV-4, OPEN, apmf/OffenseApproach.cpp / Evaluator.cpp) -- ranged: dwell tuning; disagreeing selector rules; chase-cap waiver gate (raised against e63db74 / 4cd7100)
Same review. F8: kSightDwellMs 1500 vs 1-2 s flicker still allows a ~3 s claim/release cycle — tune from field data. F7: two own-ray rules that disagree reset each other's dwell clocks (bounded by kStaleMs). Author's round-2 spots: the waiver needs the foe's currentCombatTarget == the follower (a dragon targeting the player instead does not qualify — per marth's "engaged with the follower"); PinnedTarget/CombatEntryStateOf read every PickFoe pass (g_mx, unprofiled); LogChaseCapWaiver static map assumes single-threaded PickFoe.

### MFO-B306 (SEV-4, OPEN, logistics/SwapUp.cpp / LootEquipment.cpp) -- keep-set: Mage edge cases (raised against 392aba1)
Opus re-review of `fix/mfo-keepset-roles` @ 392aba1. (1) Pre-existing: "a class-3 follower with a melee gambit but no cast gambit (`IsCasterFollower` false) still gets `roles.melee` in keep and loot. The combat pick is dagger-only for him, so a sword can win bucket 1 and the unworn dagger sells." (2) "with bMageDaggersOnly OFF, combat lets a base Mage plus melee gambit wield a 2H, while keep holds only one 1H backup. An unworn 2H he fights with now sells."

### MFO-B307 (SEV-5, OPEN, logistics/SwapUp.cpp / Economy.cpp / Upkeep.cpp) -- keep-set small edges (raised against 392aba1 / cae6bb4)
Same reviews: an in-role catalog-excluded weapon (artifact, "keep but never wield") counts toward anyInRole and suppresses the floor; Economy's buy-threshold armor baseline (~:357/:435) doesn't skip ArmorWontRender (self-corrects after one vendor visit); the once-per-item heal set persists across loads in a session; shield keep vs EquipAuthority pureCaster deny edge; ArmorWontRender not gated by g_beastHeadFix (harmless).

### MFO-B308 (FIXED 2026-11-08: Harbinger ABI v33) -- artifacts "keep but never wield" needs a per-item equip deny
marth 2026-10-19: catalog-excluded non-quest off-role weapons: "Keep but never wield". Interim (392aba1): unequipped once per item per session, never sold; the engine may re-wield. Proper fix waits on Harbinger ClickUp 86e3t8w23 (ch.17 per-item DENY entry, ABI).
- **FIXED 2026-11-08 (MFO 44debd4 + the F1 follow-up on `feat/apmf-api-batch`, Harbinger 967143b on `feat/api-batch-b62-ch17`, ABI v33):** one test, `Logistics::NeverWieldWeapon` (catalog-excluded, not a creature weapon, entry not quest-bound, whatever bLootSpecialItems says), now gates the SOURCES: the combat weapon judge (cast/Equip.cpp, the pick, the off-hand pick and the "another weapon serves" tests) and `EquipBestOwnedWeapon`'s candidates never choose such a weapon, so MFO's declaration never names one. `RefreshEquipDeclaration` (logistics/EquipAuthority.cpp) sends every one he carries as the ch.17 claim's per-item deny list (`SetEquipDenyItems`). Harbinger refuses every engine equip of them on him from every route, the player's own hand-over included (marth: DENY WINS, no player-given exemption), answers NO at the combat selection seat and unequips a worn one. `HealExcludedWeapon` (logistics/Upkeep.cpp) makes no equip op for a listed item while the deny is in effect. The "kept OFF the deny" carve-out for a form MFO's own declaration names stays as a guard and logs once if it ever fires. The interim once-per-session unequip stays only for Harbinger below ABI 33, observe-only, or before the first declaration. The unarmed fallback (marth: "We would never allow an off class weapon, unless unarmed") is its own task, not part of this fix.

### MFO-B309 (SEV-5, CLOSED, Serialization.cpp) -- ERND: version 0 read as v1; ReadString accepts a 1-3 byte partial length read (raised against 3e9b659)
Opus tier-3 review of `feat/mfo-town-r2` @ 3e9b659. F4: "an ERND record with version 0 is parsed as v1. There is no lower-bound guard, the same as PGIV." F6 (pre-existing helper): "`ReadString` checks `!ReadRecordData(len)`, which accepts a 1-3 byte partial read. In ERND the following bound check or short read still catches it."
- CLOSED 2026-10-19 (town phase 2 R5, `feat/mfo-town-p2-r5-trips`): F4 -- the ERND dispatcher skips a record with version < 1 (logged, nothing staged); F6 -- `ReadString` requires the full 4-byte length read (`ReadRecordData(len) != sizeof(len)` is a short read).

### MFO-B310 (SEV-4, OPEN, CasterConsent.cpp) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "CasterConsent's two deny halves now disagree for a substrate holder. The hard deny at CasterConsent.cpp:1041 was converted to InCombat. Its comment justifies the gate as 'what CheckStartCast gets by construction', but that premise no longer holds. The soft CheckStartCast path at :657 (CtrlUnlatchedDeny), and the latched deny below it, still run under the substrate's live controller. So a holder with a leftover g_ctrl / g_want entry is soft-denied but no longer hard-backed. The window is short: the entries last at most until the 2-lap OOC teardown's CasterConsent::Clear. Either gate the soft half the same way or fix the comment." Reasoning: Deferred by the coordinator (closing round, SEV-4): the leftover entries live at most until the 2-lap OOC teardown clears them.

### MFO-B311 (SEV-5, OPEN, Scheduler.cpp) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "Scheduler.cpp:828: std::unordered_set substrateNow is built every pump (133 ms); MSVC allocates a sentinel node even for an empty set. Make it a reused static or a small vector." Reasoning: Cost only; recorded, not fixed in the closing round.

### MFO-B312 (SEV-5, OPEN, Scheduler.cpp) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "Scheduler.cpp:871-876: the 'left the Harbinger combat substrate' line also fires when a noted follower dies, is disabled or is dismissed (the continue at :853 comes before the check), so the wording is wrong in those cases." Reasoning: Log wording only.

### MFO-B313 (SEV-5, OPEN, logistics/Lockpick.cpp) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "Lockpick.cpp:819-834: if a substrate draws his hands, the wait line says 'sheathing it first', but the posted sheathe is gated on raw !IsInCombat(), so it waits out the substrate (at most 15 s) without sheathing. Behaviour fine; log text wrong." Reasoning: Log wording only; the raw sheathe gate is the deliberate mechanics line.

### MFO-B314 (SEV-5, OPEN, Docs/STATUS.md) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "Docs/STATUS.md says a holder 'runs no combat table'; only true while the party is otherwise out of combat. If the player or another follower fights, the combat table runs for him (own-OOC). Behaviour correct; wording overclaims." Reasoning: Doc wording only.

### MFO-B315 (SEV-5, OPEN, CombatSense.h) -- substrate-not-combat review (raised against 5876057)
Opus tier-3 review of `fix/mfo-substrate-not-combat` @ 5876057. Verbatim: "CombatSense.h:6-11 re-declares two APMFBridge functions to avoid an include cycle; a future signature drift would only fail at link time." Reasoning: Maintenance hazard; recorded.

### MFO-B316 (SEV-5, OPEN, logistics/Errand.cpp) -- errand backoff: shared-chest duplicate listing (raised against 397b24d1)
Opus review of `fix/mfo-errand-field-1019` @ 397b24d1. Verbatim: "A backed-off loaded vendor never marks seenChest, so the faction-only loop also lists the same chest under the faction name. Several living vendors that share one chest each add their own entry too. (Errand.cpp:345, :402-404)" Reasoning: the backoff skip `continue`s before `seenChest` is set, so the same chest can re-enter the catalog as a faction shop and the skip line may name it twice; cosmetic today because the faction path checks `backedOff` by chest too.

### MFO-B317 (SEV-5, OPEN, logistics/Errand.cpp) -- errand backoff: WarnCombatDisagree wording (raised against 397b24d1)
Opus review of `fix/mfo-errand-field-1019` @ 397b24d1. Verbatim: "WarnCombatDisagree (Errand.cpp:510-514): Tick has already called Finish on APMFBridge::InCombat(follower) or player combat at the top of the same tick (~:903), so any kLeg_CombatCancelled that reaches the walk-end sites meets InCombat == false. The WARN amounts to 'every CombatCancelled', including a short real combat that ended before the leg was read. Not a mask, just noisier than its wording suggests." Reasoning: log-wording only; the failure is still loud (principle 7).

### MFO-B318 (SEV-5, OPEN, logistics/Errand.cpp) -- errand backoff: chest 0 reports 0 s (raised against 397b24d1)
Opus review of `fix/mfo-errand-field-1019` @ 397b24d1. Verbatim: "Errand.cpp:497-498: if r.chest is 0, NoteWalkFailed returns 0 and the Finish line says 'backed off 0 s'. Cannot happen with the current catalog, which always sets the chest." Reasoning: unreachable today; a future catalog source without a chest would silently get no backoff.

### MFO-B319 (SEV-5, OPEN, logistics/ErrandSim.cpp) -- errand backoff: simulation DONE does not reset (raised against 397b24d1)
Opus review of `fix/mfo-errand-field-1019` @ 397b24d1. Verbatim: "The off-screen simulation's DONE (ErrandSim.cpp) does not erase the backoff entry, so the fails count carries over and the next failure doubles from there." Reasoning: the backoff state is private to Errand.cpp and the simulation completes in its own file; the effect is a longer backoff after a later failure, bounded by the 15 min cap.

### MFO-B320 (SEV-4, OPEN, logistics/Errand.cpp) -- town toasts: toast precedes a possible immediate failure (raised against ad8746aa)
Opus review of `feat/mfo-town-toasts` @ ad8746aa. Verbatim: "The physical toast precedes a possible immediate failure: beginBeat can Finish the errand right after the toast (Errand.cpp:1038-1042); the errand can retry after the cooldown and toast again for the same vendor. Minor; fits 'begin' semantics." Reasoning: the toast marks the begin of the activity, so a failed first beat then a retry is a second begin. Cosmetic only.

### MFO-B321 (SEV-5, OPEN, logistics/ErrandSim.cpp) -- town toasts: sim walk=0 path never toasts (raised against ad8746aa)
Opus review of `feat/mfo-town-toasts` @ ad8746aa. Verbatim: "The sim's walk=0 path never toasts: when now >= tArrive at Begin (ErrandSim.cpp:287) the record goes straight to Trade with no toast. Under-toast, harmless." Reasoning: the toast fires only on the Out to Trade transition in Sweep.

### MFO-B322 (SEV-5, OPEN, logistics/Errand.cpp) -- town toasts: VR fallback branch effectively dead (raised against ad8746aa)
Opus review of `feat/mfo-town-toasts` @ ad8746aa. Verbatim: "The VR fallback branch (Errand.cpp:646) is effectively dead: errands refuse on VR (ErrandSupported())." Reasoning: harmless, kept for the Economy.cpp pattern.
### MFO-B323 (SEV-4, CLOSED 2026-10-19 in `feat/mfo-town-p2-r2-prices`: As<TESObjectREFR>() for 8/60/59, IsBoundObject() for 76, refused + logged otherwise, logistics/TradePerks.cpp) -- town S: EntryPointValue does not type-check its condition argument (raised against 527877c6)
Opus tier-3 review of `feat/mfo-town-p2-s-rows` @ 527877c6. Verbatim: "EntryPointValue(ep, owner, TESForm* arg, seed) does not type-check the argument per entry point: a non-REFR form passed for 8/60/59, or a non-bound object for 76, would be read by the conditions as the wrong type. Add As<TESObjectREFR>() / IsBoundObject() checks before R2 adds the first caller." Reasoning: no caller exists yet; the check must land before round R2 adds the first one. MAP: "logistics/TradePerks.cpp" entry in the logistics file list.

### MFO-B324 (SEV-4, OPEN, Progression.cpp / cast/Summon.cpp / progression/PerkGate.cpp / ProgProbe.cpp / TauntProbe.cpp) -- town S: unguarded HandleEntryPoint / IsTrue callers (raised against 527877c6)
Opus tier-3 review of `feat/mfo-town-p2-s-rows` @ 527877c6. Verbatim: "Unguarded HandleEntryPoint / IsTrue callers remain: Progression.cpp:1429, cast/Summon.cpp:112, progression/PerkGate.cpp:403, ProgProbe.cpp:401, TauntProbe.cpp:89. They pre-date this change. Route them through the TradePerks wrappers in their own round." Reasoning: the rows now exist and verify, so these calls run at verified addresses, but they do not refuse when a row fails; out of round S scope. MAP: "SEAT SELF-CHECK" bullet (the NOT-yet-routed list).

### MFO-B325 (SEV-5, OPEN, Packages.cpp) -- town S: VR load noise from the refused alias reader (raised against 527877c6)
Opus tier-3 review of `feat/mfo-town-p2-s-rows` @ 527877c6. Verbatim: "VR noise: each VR load now prints 7 'NOT swept' error lines in addition to the seat refusal; the VR loot/retreat sweeps were already no-ops before this change." Reasoning: log volume only on VR (no verified table there); since the review fix the lines read "occupancy unknown ... NOT swept (no marker or no native ForceRefTo)". MAP: Packages.cpp "ALIAS READER behind its self-check row" bullet.

### MFO-B330 (SEV-5, OPEN, logistics/Pricing.cpp) -- town R2: a GMST read failure turns the price model off at full base value (raised against a11fa74c)
Opus review of `feat/mfo-town-p2-r2-prices` @ a11fa74c. Verbatim: "Pricing.cpp:126: when a GMST cannot be read the model turns off with multipliers 1.0; the sell side then pays full base value, more generous than the old 0.30-0.50 Speech fraction. Logged once as ERROR; the six GMSTs always exist as exe defaults." Reasoning: SEV-5, logged loudly, the settings always exist. MAP: the #21 "PRICES (town phase 2 R2)" bullet in the TradeBridge section (cross-referenced there as MFO-B330).

### MFO-B331 (SEV-5, CLOSED 2026-10-19 in the R2 closing round, MAP.md) -- town R2: stale RegisterFuncs line number in MAP (raised against a11fa74c)
Opus review of `feat/mfo-town-p2-r2-prices` @ a11fa74c. Verbatim: "MAP.md cites TradeBridge.cpp:365-376 for RegisterFuncs; the real location is ~:662-675 (stale before this change)." Reasoning: doc only; FIXED in the R2 closing round (now `:661-676`). MAP: TradeBridge section and the script-ABI table row.
### MFO-B332 (SEV-4, CLOSED on fix/mfo-cast-gates-1027, apmf/Substrate.cpp / cast/SubstrateRoad.cpp) -- S3: NOT OPENED error repeats for a standing refusal (raised against 733c20cc)
CLOSED 2026-10-27: `FileSubstrateCast` gate 2b now refuses at the source what Harbinger refuses by rule (player in combat, not loaded, not high process, in a scene, dialogue open) with a rate-limited `[info]` line, so those requests are never filed (80 refusals in the 2026-10-27d session).
Opus tier-3 review of `feat/mfo-substrate-s3` @ 733c20cc, SEV-4 (b). Verbatim: "The NOT OPENED ERROR repeats every pacing window when Harbinger refuses for a standing reason (dialogue, scene, not high process); a summon (no pacing) re-files every time its verdict ages out." Reasoning: log volume and Harbinger churn only; each refusal is still a no-cast, never the direct road. Surfaced at edit time from: MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".

### MFO-B333 (SEV-4, OPEN, cast/SubstrateRoad.cpp / cast/BuffRoad.cpp) -- S3: ChooseBuffRoad from the OOC table erases g_neverFired early (raised against 733c20cc)
Opus tier-3 review of `feat/mfo-substrate-s3` @ 733c20cc, SEV-4 (d). Verbatim: "ChooseBuffRoad called from the OOC table erases g_neverFired (BuffRoad.cpp:364) on the first one-service party-OOC flap tick, before the 2-tick debounce." Reasoning: a per-fight never-fired hold-off can be lifted one service early on a party-OOC flap with bSubstrateCasts ON. Surfaced at edit time from: MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".

### MFO-B334 (SEV-4, OPEN, cross-branch: fix/mfo-riften-1019b + feat/mfo-substrate-s3) -- S3: AnchorOnlyLeak may fire falsely on a close (raised against 733c20cc)
Opus tier-3 review of `feat/mfo-substrate-s3` @ 733c20cc, SEV-4 (f). Verbatim: "Cross-branch: fix/mfo-riften-1019b's AnchorOnlyLeak ERROR may fire falsely on the frame between an S3 close (holder dropped) and the controller teardown. Watch in F2." Reasoning: a false ERROR line only; watch for it in the F2 field run. Surfaced at edit time from: MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".

### MFO-B335 (SEV-5, OPEN, apmf/Substrate.cpp) -- S3: spdlog under g_subMx (raised against 733c20cc)
Opus tier-3 review of `feat/mfo-substrate-s3` @ 733c20cc, SEV-5. Verbatim: "spdlog is called under g_subMx." Reasoning: a leaf mutex held across a log write; cost only. Surfaced at edit time from: MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".

### MFO-B336 (SEV-5, CLOSED in the review round on 733c20cc, cast/Summon.cpp) -- S3: the Summon NotTaken case (raised against 733c20cc)
Opus tier-3 review of `feat/mfo-substrate-s3` @ 733c20cc, SEV-5. Verbatim: "The Summon NotTaken case comment is correct, but nothing enforces it." Closed: the switch is gone; `SummonOnMain` treats anything but Filed as a Failed no-cast, so no NotTaken path exists to fall through to the direct cast. Surfaced at edit time from: MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".
### MFO-B326 (SEV-5, OPEN, apmf/Bridge.cpp) -- riften fixes: const_cast in InCombat (raised against fb883fe3)
Opus review of `fix/mfo-riften-1019b` @ fb883fe3. Verbatim: "const_cast in InCombat is harmless (AnchorOnlyLeak only reads); it could take const RE::Actor*." Reasoning: read-only. NOTE: the round that followed (this branch's second head) removed the leak test from `InCombat` altogether (the SEV-3 narrowing), so the const_cast is gone; the finding is kept for `AnchorOnlyLeak` taking `RE::Actor*` where it could take a const one.

### MFO-B327 (SEV-5, OPEN, logistics/LootTravel_internal.h) -- riften fixes: the gate-ref label is per call, not per record (raised against fb883fe3)
Opus review of `fix/mfo-riften-1019b` @ fb883fe3. Verbatim: "ReadmitOnGateEvent's ', the record's own gate ref' label is set if ANY record names this ref, not per erased record. Log text only." Reasoning: the label is computed once before the erase; no behavioural effect.

### MFO-B328 (SEV-5, OPEN, Targeting.cpp / CombatSense.h) -- riften fixes: group read lock per combat tick (raised against fb883fe3)
Opus review of `fix/mfo-riften-1019b` @ fb883fe3. Verbatim: "The per-call group read lock now runs per combat tick in the Targeting hook while any style is owned, and calls APMF GetSubstrateState under the engine lock (the FoeCount pattern)." Reasoning: same exposure FoeCount already has; the SubstrateOnly call added to `AnchorOnlyLeak` in the follow-up round is lock-free on the APMF side.

### MFO-B329 (SEV-5, OPEN, CombatStyle.cpp) -- riften fixes: a CSTY written before a leak began stays (raised against fb883fe3)
Opus review of `fix/mfo-riften-1019b` @ fb883fe3. Verbatim: "A CSTY written before a leak began stays on the live controller until it dies." Reasoning: the Targeting hook stops re-asserting a style for a leaked teammate, it does not restore the engine-derived one; the controller's own teardown does.

### MFO-B337 (SEV-4, OPEN, Board.cpp / Board.h) -- town R3: r.cell is dead after the location column
Opus review of `feat/mfo-town-p2-r3-board` @ 92c761cd. Verbatim: "Board.cpp:1785-1790 and Board.h:64-66: r.cell is now dead. It is still computed with duplicate engine reads, and the cell field and its comment in Board.h remain, but nothing draws it any more. The replacement is not clean: delete the block and the field."

### MFO-B338 (SEV-4, CLOSED 2026-10-19 at merge) -- town R3: '?' vendor fallback shows "Walking to ?"
Opus review @ 92c761cd. Verbatim: "Errand.cpp:906 and ErrandSim.cpp:456: vendorName falls back to '?', not to an empty string... the 'the shop' fallback in Describe never fires. It should check for '?' as well as empty." Fixed inline at merge (both Describe sites treat '?' as empty).

### MFO-B339 (SEV-4, OPEN, Errand.cpp / ErrandSim.cpp / Board) -- town R3: errand status strings bypass i18n
Opus review @ 92c761cd. Verbatim: "the status strings are English literals, while the same tab keys its sibling states (Fk_StateLooting / Fk_StateTrading)... Fix: have Describe return a kind plus the shop name, and let the Board format it through {1} keys. The toast literals match the existing ToastTrading... so they can stay."

### MFO-B340 (SEV-5, OPEN, Board.cpp:34-35) -- town R3: mangled include comment
Opus review @ 92c761cd. Verbatim: "the include comment got mangled. The // IsLooting/IsTrading... comment for Logistics.h was appended to the Errand.h line."

### MFO-B341 (SEV-5, OPEN, Errand.cpp / ErrandSim.cpp) -- town R3: "Trading with" vs "trading at"
Opus review @ 92c761cd. Verbatim: "the physical errand says 'Trading with X' and the simulated one says 'Off-screen: trading at X'. Pick one preposition."

### MFO-B342 (SEV-5, OPEN, Board_FieldKit.cpp:510) -- town R3: '- %s' separator
Opus review @ 92c761cd. Verbatim: "the '- %s' separator uses a hyphen as a dash. It is carried over from the old draw. VOICE.md asks for a colon or parentheses, so '(with you)' would fit better. Optional."

### MFO-B343 (SEV-4, OPEN, ErrandSim.cpp:357-375) -- town R4: the other browsing case is not built (REWORDED 2026-10-19: browse-only; placement criteria removed)
Opus review @ c2253ac3. Verbatim: "The design's other browsing case is not built: if the simulated trade has already run and it is before tTradeEnd, the errand resolves straight to DONE (ErrandSim.cpp:357-375). With no placement facet either, field criteria R4-1 and R4-3 cannot pass as written." Update (`fix/mfo-town-counter-drop`, marth 2026-10-19): the placement facet and its criteria are GONE; the sim-trade-already-ran case still resolves straight to DONE with no browse beat. MAP: "R4 / R4b" section.

### MFO-B344 (SEV-5, CLOSED (obsolete: counter learning and placement removed (marth 2026-10-19)), ErrandCounter.cpp Learn) -- town R4: "confirmed" is not "at the counter"
Opus review @ c2253ac3. Verbatim: "'Confirmed' means stationary for ~6 s at any sandbox marker, not 'at the counter'. Fine while the map is log-only; the placement facet must not trust it blindly."

### MFO-B345 (SEV-5, OPEN, logistics/ErrandTrip.cpp) -- town R5: needs only at a carrier (raised against c32c8c1e)
Opus tier-3 review of `feat/mfo-town-p2-r5-trips` @ c32c8c1e. Verbatim: "Needs not carried by a chosen stop's VEND are no longer bought there (phase 1 tried them at whichever vendor); a need with no carrier and no free stop slot goes unserved. Design-intended (1.b), little practical effect."

### MFO-B346 (SEV-5, OPEN, logistics/ErrandSim.cpp) -- town R5: an aborted replay reads Done (raised against c32c8c1e)
Same review. Verbatim: "ErrandSim.cpp:735-738: a replayed trade that ABORTED still sets reported, so AdvanceStop marks the stop Done instead of Skipped (log/state wording)."

### MFO-B347 (SEV-5, OPEN, logistics/Errand.cpp) -- town R5: talk beat aimed at an unloaded later-stop vendor (raised against c32c8c1e)
Same review. Verbatim: "Errand.cpp:1045-1050: a later stop whose vendor is unloaded but whose leg reads Arrived aims the talk beat at an unloaded vendor; the next tick's vendor gate skips the stop."

### MFO-B348 (SEV-5, OPEN, logistics/ErrandTrip.cpp) -- town R5: bestRow recomputed in the sort comparator (raised against c32c8c1e)
Same review. Verbatim: "ErrandTrip.cpp:189-192: the stable_sort comparator recomputes bestRow (N log N x candidates x rows per plan on the worker); cache it if a plan ever stalls."

### MFO-B370 (SEV-4, CLOSED (obsolete: counter learning and placement removed (marth 2026-10-19)), apmf/Place.cpp) -- town R4b: a flags-0 RequestPlace is unguarded (raised against f2bfbc72)
Opus review of `feat/mfo-town-p2-r4b-place` @ f2bfbc72. Verbatim: "the flags-0 RequestPlace used to raise the unsupported warning is safe today but fragile: if either gate changes, a flags-0 request means 'move whatever the player can see' with no guards; send the real flags or add a WarnPlaceUnsupported()." FIXED in the closing round (the flags-0 call is removed; the caller always sends the real flags and the wrapper warns once on ABI < 28). Kept for the record. MAP: "R4" What-breaks.

### MFO-B371 (SEV-5, CLOSED (obsolete: counter learning and placement removed (marth 2026-10-19)), logistics/ErrandCounter.cpp) -- town R4b: a multi-stop arrival overwrites the pending placement (raised against f2bfbc72)
Same review. Verbatim: "a multi-stop arrival overwrites the pending entry; harmless in practice." MAP: "R4" What-breaks.

### MFO-B360 (SEV-4, OPEN, logistics/ErrandShops.cpp) -- town R6: gate subject/target pairing differs from the engine's (raised against f0635350)
Opus review of feat/mfo-town-p2-r6-fences. Verbatim: "Actor::CalculateCurrentVendorFaction (AE id 37383 @0x14066C250) calls IsTrue(vendorConditions, vendor, vendor) at 0x14066C3C5. With the vendor unloaded, the subject is the player, so a negated subject check can wrongly pass. Suggested fix: use the vendor's persistent ref, from the R4 learned map."

### MFO-B361 (SEV-4, FIXED (Errand.cpp near-self vendor build replaces the default continue; triage 2026-11-08), logistics/Errand.cpp) -- town R6: type-12 fence with an unloaded vendor not built (raised against f0635350)
Opus review of feat/mfo-town-p2-r6-fences. Verbatim: "The type-12 (near self) fence with an unloaded vendor (Niranye) is not built (Errand.cpp:286 default continue)."

### MFO-B362 (SEV-5, OPEN, logistics/Errand.cpp) -- town R6: restored or re-planned stops are not re-gated (raised against f0635350)
Opus review of feat/mfo-town-p2-r6-fences. Verbatim: "Restored or re-planned stops are not re-gated (fence perk / conditions); the trade-time stolen split still applies."

### MFO-B363 (SEV-5, OPEN, logistics/Economy.cpp) -- town R6: ClassifyStolen does a second full GetInventory per scan (raised against f0635350)
Opus review of feat/mfo-town-p2-r6-fences. Verbatim: "ClassifyStolen does a second full GetInventory on every scan (Economy.cpp:929)."

### MFO-B372 (SEV-4, OPEN, logistics/ErrandSmith.cpp) -- town R8: an enchanted worn piece is not tempered (raised against R8 authoring, own note)
The vanilla temper recipe's condition is `EPTemperingItemIsEnchanted != 1 OR HasPerk(ArcaneBlacksmith)`. The item-context function reads the engine's tempering context, which MFO does not own, so `EvalConditions` answers an enchanted piece as Unknown and skips it (logged once per recipe). Evaluating the HasPerk branch alone needs a per-node `TESConditionItem::IsTrue` seat (a new verified-address row) or the engine's context. Tempering enchanted gear is a follow-up.

### MFO-B373 (SEV-5, OPEN, logistics/ErrandSmith.cpp) -- town R8: the Smith stop carries the smithy shop's chest (raised against R8 authoring, own note)
The ERND reader requires a non-zero faction and chest for a Pending stop, so the Smith stop stores the smithy shop's; `Trip::ChestCrowd` therefore counts a smith stop as "heading to that chest" and slightly over-penalises other followers' plans there. Filter `StopKind::Smith` out of both CrowdInto walks when the hook-up touches them.
### MFO-B349 (SEV-4, OPEN, native/Serialization.h) -- town R7: downgrade path (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "Downgrade path. The b68f0a8 reader drops an entry whose current stop is 'inn'/'temple'. A follower unloaded in the inn is then not recalled, so the Serialization.h comment 'simply with the player' overstates it." The comment's wording is FIXED in the same round (it now says a downgraded DLL does not bring him back); the behaviour stays. MAP.md: "Town phase 2 R7".

### MFO-B350 (SEV-4, OPEN, logistics/ErrandRest.cpp) -- town R7: NoteTown has no grace period (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "NoteTown has no grace period (ErrandRest.cpp:712). The Sweep waits 2 s; this does not. A brief null or dungeon location (the Ratway, an interior with no location) clears every mark." MAP.md: "Town phase 2 R7".

### MFO-B351 (SEV-4, OPEN, apmf/ErrandWork.cpp) -- town R7: shared-slot window (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "Shared-slot window. After ReleaseErrandWork frees a slot, another follower can rewrite that record while the first follower's AI may still run it until its next package evaluation." MAP.md: "Town phase 2 R7".

### MFO-B352 (SEV-4, OPEN, logistics/Errand.cpp) -- town R7: staying follower keeps the chest counted busy (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "A staying follower keeps the innkeeper's chest counted as busy. Physical CrowdInto (Errand.cpp:789) counts it for the whole stay; the sim side excludes Rest." MAP.md: "Town phase 2 R7".

### MFO-B353 (SEV-4, FIXED (Errand.cpp early return now only when inn errands are off; triage 2026-11-08), logistics/Errand.cpp) -- town R7: no temple trip when no shop is open (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "No temple trip when no shop is open. TryStart returns early on an empty shop list (Errand.cpp:968), so a diseased follower at night gets no temple visit." MAP.md: "Town phase 2 R7".

### MFO-B354 (SEV-4, FIXED (food order verified on 1.6.1170, 1.5.97, 1.7.104 at ErrandRest.cpp:412,473; triage 2026-11-08), logistics/ErrandRest.cpp) -- town R7: innkeeper food order is 1.6.1170 only (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "The innkeeper food order runs on 1.6.1170 only. This matches the BuildTownShops precedent and does no harm. 1.7.104 is a supported runtime, so a per-runtime path belongs in the backlog." EXTENDED (marth/coordinator): the `ResolveViaData` fallback added in the fix round (innkeeper-faction place + location special refs) is also 1.6.1170-only. MAP.md: "Town phase 2 R7".

### MFO-B355 (SEV-4, OPEN, logistics/ErrandRest.cpp) -- town R7: FoodWorth bypasses the Synthesis catalog (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "FoodWorth judges food at run time rather than through the Synthesis item catalog. That departs from the catalog-first rule." MAP.md: "Town phase 2 R7".

### MFO-B356 (SEV-5, OPEN, logistics/ErrandRest.cpp) -- town R7: stale temple work entry (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "Stale temple work entry. When a temple stop hands over to the inn, NextStopOrFinish never calls Rest::End, so the temple entry lingers. Its claims were already released at Exit, so it is harmless." MAP.md: "Town phase 2 R7".

### MFO-B357 (SEV-5, OPEN, logistics/ErrandRest.cpp) -- town R7: bed choice (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "Bed choice. FindBed's ownership filter does not bind the Sleep package's own bed choice within its 500-unit radius. Field check." MAP.md: "Town phase 2 R7".

### MFO-B358 (SEV-5, OPEN, Serialization.cpp) -- town R7: misleading reader log (raised against bfc92b50)
Opus review of `feat/mfo-town-p2-r7-inn` @ bfc92b50. Verbatim: "Misleading reader log. The reader's 'shop gone' counter also counts a current temple or inn stop." MAP.md: "Town phase 2 R7".

### MFO-B374 (SEV-5, OPEN, logistics/ErrandSmith.cpp) -- town R8: 21 vanilla temper recipes with a mixed condition group (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "21 vanilla temper recipes mix a separate condition group with the item-context group and are skipped even when unenchanted. All 785 Requiem recipes evaluate correctly." (EvalConditions answers a group it cannot evaluate node by node as Unknown.) MAP.md: "R8 — smithing / tempering errands".

### MFO-B375 (SEV-4, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: a perkless follower gets no tempering (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "A perkless follower gets no tempering. EntryPointValue returns nullopt when perkData is null. R0-4 says use the seed and log 'perks not evaluated'." Fixed: BuildOnMain checks the perk data itself and uses the formula's delta, logging it. MAP.md: "R8 — smithing / tempering errands".

### MFO-B376 (SEV-4, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: VerifyOnMain native RemoveItem without Is3DLoaded (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "VerifyOnMain calls native f->RemoveItem without Is3DLoaded." Fixed: the ledger charges only while he is loaded. MAP.md: "R8 — smithing / tempering errands".

### MFO-B377 (SEV-4, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: material purse cap per form (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "The material purse cap is half the purse per form (TradeBridge.cpp:~303), up to about 87% across several materials." Fixed: one half-purse budget computed once. MAP.md: "R8 — smithing / tempering errands".

### MFO-B378 (SEV-5, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: MAP still calls OfferWork a STUB (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "MAP still calls OfferWork a 'STUB'." Fixed: MAP R8 text rewritten. MAP.md: "R8 — smithing / tempering errands".

### MFO-B379 (SEV-5, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: stale BuildOnMain after ClearTransientState (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "A BuildOnMain posted before ClearTransientState can restore a stale g_want after a load." Fixed: a generation counter. MAP.md: "R8 — smithing / tempering errands".

### MFO-B380 (SEV-5, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: ARMO with slot mask 0 hits SKSE's weapon path (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "An ARMO with slot mask 0 would hit SKSE's weapon path." Fixed: ReadWorn skips it. MAP.md: "R8 — smithing / tempering errands".

### MFO-B381 (SEV-5, CLOSED in the R8 fix round, logistics/ErrandSmith.cpp) -- town R8: hand-off comment wrong (raised against 06699bd8)
Opus review of `feat/mfo-town-p2-r8-smith` @ 06699bd8. Verbatim: "The hand-off comment 'SMITH_SIM finishes it' is wrong, because Begin already erased g_plan." Fixed: comment rewritten. MAP.md: "R8 — smithing / tempering errands".

### MFO-B382 (SEV-4, OPEN, logistics/ErrandSmith.cpp) -- town R8: a refused later removal leaves a part-paid line (raised against 623ed40c)
Opus re-check of `feat/mfo-town-p2-r8-smith` @ 623ed40c. Verbatim: "If a later removal is refused at dispatch after an earlier one was accepted, he pays part of the cost and gets no temper (logged)." MAP.md: "R8 — smithing / tempering errands".

### MFO-B383 (SEV-4, OPEN, logistics/ErrandSmith.cpp) -- town R8: the VM can quietly remove or buy fewer than asked (raised against 623ed40c)
Opus re-check of `feat/mfo-town-p2-r8-smith` @ 623ed40c. Verbatim: "The VM can quietly remove fewer items than asked; the same on the buy side: MFO_Trade.psc adds qty to boughtCount without checking the actual move." MAP.md: "R8 — smithing / tempering errands".

### MFO-B384 (SEV-4, OPEN, logistics/ErrandSmith.cpp) -- town R8: the ledger is cleared on load (raised against 623ed40c)
Opus re-check of `feat/mfo-town-p2-r8-smith` @ 623ed40c. Verbatim: "The ledger is cleared on load, so a temper dispatched but not yet read back before a save becomes free after reloading." Also noted: "the whole-plan credit is strict in the safe direction (an unbought gear line skips the materials credit)." MAP.md: "R8 — smithing / tempering errands".

### MFO-B385 (SEV-4, OPEN, logistics/ErrandRest.cpp FindPlaces / ResolveViaData + Errand.cpp TryStart) -- town: a follower already inside the inn plans a trip to his own cell (raised against d69e4869)
Opus review of `fix/mfo-town-counter-drop` @ d69e4869. Verbatim: "a follower already inside the inn: FindPlaces finds no inn door from inside, so ResolveViaData resolves the inn cell, and stops[0] becomes the cell he is standing in. How a ch.19 cell claim to the actor's own cell resolves is unverified. Watch for it in the field." MAP: R7 section.

### MFO-B386 (SEV-4, OPEN, logistics/Errand.cpp Tick combat exit) -- town: combat during the inn-only walk re-plans the inn (raised against d69e4869)
Same review. Verbatim: "combat-while-walking re-plans inn": an inn-only trip that ends on combat during the walk is not marked trip-done (deliberately left, the coordinator is asking marth), so after the combat cooldown he plans the inn trip again. MAP: R7 section.

### MFO-B387 (SEV-4, OPEN, logistics/Gear.cpp:871) -- torch: misleading "left to the next combat equip gambit" skip while the torch stands (raised against 68f3841)
Opus review of `feat/mfo-torch-ch17` @ 68f3841. Verbatim (log): "Gear.cpp:871 (Right|Left overlap -> 2H/bow upgrade skipped while torch stands; SEV-4 misleading log)". Reasoning: while the standing torch owns Light|Left, AcquireEquip's weapon upgrade for a two-hander or bow is skipped and the log says the combat equip gambit will place it, which is not what happens OOC. MAP: "logistics/Upkeep.cpp" EquipTorch entry.

### MFO-B388 (SEV-5, OPEN, logistics/EquipAuthority.cpp rule 1d) -- torch: STARTED is logged before resolveCarried (raised against 68f3841)
Same review. Verbatim (log): "STARTED logged before resolveCarried". An uncarried torch logs a start with nothing declared. MAP: EquipTorch entry.

### MFO-B389 (SEV-4, OPEN, logistics/Upkeep.cpp TorchDeclaredFor) -- torch: MFO does not check that Harbinger has the torch seat (raised against 68f3841)
Same review. Verbatim (log): "MFO doesn't check Harbinger has the torch seat -- mixed-version stow not denied". With an older Harbinger (no H1 seat) the claim still owns Light|Left but the engine's stow is not denied, so the torch can be stowed and re-placed. MAP: EquipTorch entry.

### MFO-B390 (SEV-4, OPEN, logistics/Service.cpp early returns) -- torch: a standing declaration outlives a lap with no build outside combat (raised against 68f3841)
Same review (minor strands of finding A): bLogistics toggled off at runtime, `CannotActReason` (bleedout), and the Scheduler's retreat / EngageOnSight returns all skip `ServiceFollower`'s declaration tail, so a torch declaration built earlier stands until the next build or the first combat lap (`TorchDeclarationDue` ends it there). Not pushed outside combat. MAP: EquipTorch entry.

### MFO-B391 (SEV-4, FIXED (Upkeep.cpp:212-213 FurnitureIdleVerified true on all three runtimes; triage 2026-11-08), logistics/Upkeep.cpp TorchSeatedReason) -- torch: furnitureIdle check gated to 1.6.1170 only (raised against 7fff38a)
Closing review of feat/mfo-torch-ch17: "widen the furnitureIdle gate to 1.5.97 (proven above) and to 1.7.104 (on H1's instruction-identical note)". Reviewer disassembled SE 38902 @0x1406801d0: `cmp qword [rax+0x228],0; jne <stow>` same as 1.6.1170. The 1.6-only gate is a safe floor (the other three seated signals still apply).

### MFO-B392 (SEV-4, OPEN, logistics/Upkeep.cpp TorchRuleHolds) -- torch: rules above the torch rule no longer suppress it (raised against 7fff38a)
"TorchRuleHolds ignores the rules above the torch rule. A higher Wait rule ('stops all gambits below it') no longer suppresses the torch. Before this change the torch's action had to be reached in the scan."

### MFO-B393 (SEV-4, OPEN, logistics/Upkeep.cpp TorchDeclaredFor) -- torch: an OOC substrate cast reads as combat (raised against 7fff38a)
"IsInCombat() is true while an S3 substrate cast runs (APMFBridge::InCombat deliberately excludes the substrate). With bSubstrateCasts=1, every out-of-combat substrate cast gives a STOPPED/STARTED pair and a re-send." Use the substrate-aware combat test.

### MFO-B394 (SEV-4, OPEN, cast/Equip.cpp ReconcileForcedWeapon) -- torch: Due re-send repeats while RefreshEquipDeclaration early-returns (raised against 7fff38a)
"Due stays true while RefreshEquipDeclaration returns early before 1d (reapply pending, 3D absent, refused, unsupported). The result is a DeclareFromLedger on every in-combat lap until it clears." Cheap; bounded only by the early-return state clearing.

### MFO-B395 (SEV-5, OPEN, logistics/Upkeep.cpp TorchRuleHolds) -- torch: full-table Evaluate per build (raised against 7fff38a)
"When the torch rule fails, Evaluate keeps scanning the rest of the table on every build. Checking just the one rule would be cheaper."

### MFO-B398 (SEV-5, OPEN, logistics/ErrandRest.cpp LogBlockedDoors) -- town: a BLOCKED cell leg does not log the door's open state (raised against fix/mfo-errand-hold)
The brief asked for lock level AND open state. Only the lock level is logged: `BGSOpenCloseForm::GetOpenState` is an id-based engine call (fork RELOCATION_ID 14180/14288) with no `VerifiedAddresses.h` row. Needs a spec row + regenerated header + `SeatVerified()` guard first.

### MFO-B399 (SEV-4, OPEN, logistics/ErrandRest.cpp ResolveViaData) -- town: the data-resolved inn / temple has no door, so its lock is never read (raised against fix/mfo-errand-hold)
`FindPlaces` checks the load door pair's lock; `ResolveViaData` (cell from the vendor faction's PLVD place or the location's special refs) knows only the interior cell, so a locked temple reached only through the data road is still planned. The BLOCKED log (`Rest::LogBlockedDoors`) names the locked door after the first failed walk, and the walk backoff applies.

### MFO-B396 (SEV-5, OPEN, logistics/ErrandSim.cpp:250 / ErrandRest.cpp:847) -- helmets: a stored errand order keeps its plan-time buy (raised against 4f320cf)
Opus tier-3 review of `feat/mfo-wear-helmets` @ 4f320cf (ClickUp 86e3phy41). Verbatim-ish: "errand stored orders (ErrandSim.cpp:250 / ErrandRest.cpp:847) use the plan-time buy, so a plan made while ON can still buy a helmet that trip." `BuildBuyThresholds` is rebuilt live per on-screen stop; only an order already stored (the off-screen sim replay, the inn/temple order) carries the `BuyThresholds::noHelmets` it was planned with. Effect: at most one trip's helmet buy after the toggle flips OFF. MAP.md: "WEAR HELMETS".

### MFO-B397 (SEV-5, OPEN, logistics/Economy.cpp IsHelmetPiece / MageClothingSlot) -- helmets: a keyworded helmet with hands/feet bits sits in a mage's hands/feet slot (raised against 4f320cf)
Same review. Verbatim-ish: "head+hands piece class mismatch." It survives F2 (the keyword test): an ARMO carrying ArmorHelmet/ClothingHead AND a hands or feet biped bit is a helmet to `IsHelmetPiece` (so it is never looted, bought or worn while OFF) but `MageClothingSlot` files it under hands/feet, so with helmets ON the mage judge ranks it against gloves/boots. Rare (no vanilla piece). MAP.md: "WEAR HELMETS".

### MFO-B400 (SEV-4, OPEN, apmf/ErrandClaims.cpp / logistics/Errand.cpp) -- town: a held cell leg holds one of Harbinger's 8 package slots for the whole stay (raised against dc0c334)
Reviewer: a held cell leg keeps its package record and slot for the whole inn stay, so up to 8 stayers (plus loot and deposit legs) fill the table. Pre-existing and related: `RepointErrandTravel` reports true even when the table is full (the refusal is only visible later as kLeg_Failed).

### MFO-B401 (SEV-4, OPEN, deploy) -- town: the hold needs Harbinger bf7f065 or later; an older Harbinger at the same ABI ends legs on unload (raised against dc0c334)
Reviewer: MFO no longer treats ActorGone as a hand-off and no longer releases at the hand-off, so against an older Harbinger (leg ends ActorGone on unload) the stop ends loudly instead of handing off. Pair-deploy requirement; no ABI gate exists.

### MFO-B402 (SEV-5, OPEN, logistics/Errand.cpp Tick) -- town: "went in and came back out" wording misattributes a third-party teleport (raised against dc0c334)
Reviewer: the WARN for a follower seen loaded outside the cell he arrived in is worded as the hold failing; a quest teleport or another mod moving him reads the same.

### MFO-B403 (SEV-5, OPEN, logistics/ErrandRest.cpp LogBlockedDoors) -- town: scans only near the follower (raised against dc0c334)
Reviewer: the BLOCKED door log lists only load doors within kScan of the follower in his own cell; a destination door in another cell reads "none found".

### MFO-B404 (SEV-5, OPEN, logistics/Errand.cpp) -- town: Errand.cpp is 1672 lines (raised against dc0c334)
Reviewer: past the ~1500 plan-a-split line. Split note only; a split is its own brief (CLAUDE.md).

### MFO-B405 (SEV-4, OPEN, logistics/ErrandSim.cpp MoveHold) -- town: a ref-destination sim stop has no hold after arrival and fails quietly (raised against 8444346)
Round-2 closing review of feat/mfo-errand-hold: Harbinger accepts an unloaded ref destination while its handle resolves, but "a ref leg ends at arrival and drops its offer (Key Concept 11a covers cell legs only). Once he arrives off-screen during TRADE_SIM, nothing holds him and the follow package walks him back toward the player." And if the vendor ref no longer resolves, ComposeLeg ends the held leg Failed, Repoint returns nothing, `RepointErrandTravel` still returns true and MoveHold logs "sim moved hold". Suggestion: for a ref destination, read the leg state on the next sim poll and WARN on Failed, DestGone or Arrived-at-ref. Risk low (vendors loaded at plan time, mostly persistent).

### MFO-B406 (SEV-4, OPEN, apmf/ErrandClaims.cpp ~:160) -- town: a refused ClaimErrandTravel erases live ch.12/ch.1 handles without releasing them (raised against 8444346)
"ClaimErrandTravel erases the whole s_claims entry when Harbinger refuses. That entry can still hold live ch.12 and ch.1 handles (the temple kneel claims now kept at hand-off, or a counter hand-off). They are dropped without being released, so the ch.1 MovementBlock leaks until a load." Only reachable when the travel claim is refused (bTravel=0, esl missing, VR -- an INI change mid-session). Fix: on refusal, reset only the travel fields.

### MFO-B407 (SEV-4, OPEN, logistics/Errand.cpp ResumePhysical work path) -- town: temple work-path resume keeps the old ch.1/ch.12/Activate claims (raised against 8444346)
"The temple work-path resume (ResumePhysical, a_info.work, then Rest::Begin Seek) does not release the kept ch.1, ch.12 or Activate work claims before walking to the shrine. He is usually still at the shrine, so the distance check passes as 'arrived' and the next Activate re-uses the existing claim. Mostly self-healing."

### MFO-B408 (SEV-4, OPEN, logistics/ErrandSim.cpp MoveHold) -- town: MoveHold logs success when Harbinger's Repoint silently fails (raised against 8444346)
"MoveHold logs success even when the Repoint silently fails in Harbinger. This is the same root as" MFO-B405 (Repoint is void in the ABI; the wrapper reports true).

### MFO-B418 (SEV-4, FIXED in 0502c1a + 8525dec, Board_Progression.cpp / progression/PerkGate.cpp:376) -- progression: board and refusal text show only the general perk pool (raised against 708c244)
"The take dialog (Pg_PointsAvail, Board_Progression.cpp ~:1151) and tree header (Pg_TreePoints, ~:534) show only unspentPerk, so a crafting node with general 0 and craft 1 shows '0 points' beside an enabled Take. Pg_NoneToSpend (~:230) shows whenever general < 1 even with craft points to spend. Its 'spent' count and PerkGate.cpp:376's whyNot 'no perk points (earned X, Y spent)' use AllocatedRanks, which includes craft-funded ranks, so it can read 'earned 5, 6 spent'." Reviewer: wording only, no incorrect behaviour. Drain before the release cut (user-visible).

### MFO-B419 (SEV-5, FIXED in 0502c1a, progression/Harness.cpp:71) -- progression: dev status log and the auto-pick "no eligible perk" log print only the general pool (raised against 708c244)
"Harness.cpp:71 and the auto-pick 'no eligible perk' log print only the general pool." Reviewer: dev-only diagnostics.

### MFO-B420 (SEV-5, OPEN, progression AllocateNextEligible) -- progression: auto-allocator spends craft points only if the class weights a crafting skill (raised against 708c244)
"AllocateNextEligible's only caller is the dev harness (Harness.cpp:162), so no production effect. It walks the class perk-priority list then class skill weights; craft points are spent only if a class weights Smithing/Alchemy/Enchanting or names a crafting perk. A future production auto-spend needs a crafting-tree pass." Reviewer: informational.

### MFO-B421 (SEV-5, OPEN, progression PerkPoolsOf) -- progression: disabling the craft stream on a save that used it leaves the follower "ahead" (raised against 708c244)
"Moving the slider 4 -> 0 on a save that used craft points drops general to 0 (clamped); nothing is stripped." Reviewer: by design, informational.
### MFO-B409 (SEV-4, OPEN, logistics/ErrandReopen.cpp Poll) -- town: the re-plan uses ORDERS recorded at the last plan, which can be stale (raised against feat/mfo-town-run2)
"The recorded order of an hours-closed shop was built when the trip was planned. Its sell rows list items the earlier stops may since have sold, and its purse is the plan-time purse. On screen the execute re-reads live counts for the stop's share, so only the partition is stale; the off-screen replay (TRADE_SIM) sends the stored rows as they were. The empty-trade memo (SkipTrade) is also read at plan time. Accepted by the ruling 'decide from the needs recorded at the last plan and replay that order'; a refresh on the next loaded tick would remove it."

### MFO-B410 (SEV-5, OPEN, logistics/ErrandReopen.cpp Poll) -- town: the hour gate reads at game-hour boundaries, so a shop opening mid-hour is seen up to an hour late (raised against feat/mfo-town-run2)
"Poll compares floor(days*24) with the last read, so the first read after a record is the next whole game hour and a shop whose start hour is not an integer (VENV hours are integers) is unaffected, but a record made at 8:59 reads again at 9:00 and a record made at 9:01 not until 10:00. The ruling says at most once per game hour, so this is within it."

### MFO-B411 (SEV-4, OPEN, logistics/ErrandRest.cpp PostTempleDoorRead) -- town: the temple door read needs the player OUTDOORS (raised against feat/mfo-town-run2)
"TES::ForEachReference walks only the player's interior cell when the player is indoors, so the read reports 'indoors' and the planner defers (capped at kMagicMaxDefers, then plans without the temple). A follower who wants the temple while the player stays inside a building plans no temple until the player steps out. The inn is unaffected (its door is read from the follower's own cell)."

### MFO-B412 (SEV-5, OPEN, logistics/ErrandSim.cpp Sweep) -- town: an off-roster loaded follower in a non-REST sim stage is simulated while loaded (raised against feat/mfo-town-run2)
"The awayOffRoster fall-through lets a loaded, eligible, off-roster record run the whole timeline (OUT_SIM, TRADE_SIM, RET_SIM, PlaceReturn MoveTo) while his 3D is up. The roster drop it targets is a few seconds long; a follower that stays off the roster for a whole stage is not distinguished from one that is away."

### MFO-B413 (SEV-5, OPEN, logistics/ErrandRest.cpp FindPlaces) -- town: the inn has no separate hours gate (raised against feat/mfo-town-run2)
"The inn and the temple are gated only by their load door's lock (a night-locked door reads locked at that hour). There is no VENV-style opening-hours test; a place that is unlocked but 'closed' by script is walked to. The spec for F4 asked to apply 'the inn's LOCKED + hours gate': the inn has the lock gate only, which is what the temple now gets."


### MFO-B414 (SEV-5, FIXED in the comment, logistics/ErrandRest.cpp:29) -- town: stale "leaf lock" comment (raised against 8aae97b)
"leaf comment" (Opus review of feat/mfo-town-run2): the file header called g_mx a LEAF never nested, but Rest::NoteTown holds it while calling Reopen::ClearAll. The header comment now states the real order (Rest g_mx -> Reopen g_mx, never reversed).

### MFO-B415 (SEV-5, OPEN, logistics/Errand.cpp BuildCatalog / ErrandReopen.cpp) -- town: a vendor whose quote is still pending (refreshPending) drops out of the watch with a misleading reason (raised against 8aae97b)
"quote-pending closed shop" (Opus review of feat/mfo-town-run2): a closed shop whose plan-time quote/order was not ready leaves the watch with the 'yield NO stop' reason instead of being re-recorded.

### MFO-B416 (SEV-5, OPEN, logistics/ErrandRest.cpp Augment) -- town: g_doorLogged is consumed by the first verdict so the success door line may never log (raised against 8aae97b)
"door log" (Opus review of feat/mfo-town-run2): the once-per-visit g_doorLogged flag is eaten by an earlier 'could not be read' verdict, so the later 'entry door found' line never logs.

### MFO-B417 (SEV-5, OPEN, logistics/ErrandReopen.cpp Poll) -- town: a reopened trip re-adds an inn stop that was Skipped in the first trip (raised against 8aae97b)
"skipped inn re-appended" (Opus review of feat/mfo-town-run2): the recorded inn stop is appended to every reopened trip even when the first trip's inn stop ended Skipped (locked door / failure).

### MFO-B422 (SEV-4, FIXED in 0502c1a, Docs/CAST-DELIVERY.md:111, Docs/INI-REFERENCE.md) -- MCM cleanup: docs still point users at the removed MCM bLegacyCastHybrid control (raised against 5a48803)
"Docs/CAST-DELIVERY.md:111 still says 'turn on the MCM bLegacyCastHybrid'. That control no longer exists, and a value set in the MCM store is now ignored. Docs/INI-REFERENCE.md does not document bLegacyCastHybrid as an INI-only key either, so there is no documented way to turn on the legacy hybrid." Fix: point CAST-DELIVERY.md:111 at SKSE/Plugins/MFO.ini and add the key to INI-REFERENCE.md. Drain before the release cut.

### MFO-B423 (SEV-5, FIXED in 0502c1a, Docs/INI-REFERENCE.md:74, Docs/TEST_GUIDE.md:421) -- docs list the deleted bProfileRapport key (raised against 5a48803)
"Docs/INI-REFERENCE.md:74 still lists bProfileRapport, and Docs/TEST_GUIDE.md:421 (step 27) tells testers to turn it on. Both were already dead on main."

### MFO-B424 (SEV-5, FIXED in 0502c1a, Docs/CAST-DELIVERY.md:1343) -- docs name the deleted Config::g_cfcBackoffMs (raised against 5a48803)
"Docs/CAST-DELIVERY.md:1343 still names Config::g_cfcBackoffMs, which has now been deleted."

### MFO-B425 (SEV-5, FIXED in 0502c1a, MAP.md:2851, :4046) -- MAP line citations to Config.h/Config.cpp drifted (raised against 5a48803)
"MAP.md:2851 -> Config.h:475 and MAP.md:4046 -> Config.h:586 have drifted by about 19 lines because of the removed block. The Config.cpp citations drift by about 5."

### MFO-B426 (SEV-5, FIXED in 0502c1a, out/MCM/Config/MFO/config.json Interface page) -- a "Debug" header still sits above the Fashionrim toggle on the Interface page (raised against 5a48803)
"The Interface page still has a header titled 'Debug' above the Fashionrim toggle (bDollsMode). Now that a separate Debug page exists, this may confuse players." Pre-existing, outside the brief.

### MFO-B427 (SEV-5, FIXED in 0502c1a, logistics/ErrandSmith.cpp:523) -- stale comment still names TES::ForEachReferenceInRange (raised against 75fadf7)
"Stale comment at ErrandSmith.cpp:523, which still says 'TES::ForEachReferenceInRange reads the worldspace…'. It is a comment only."

### MFO-B428 (SEV-5, FIXED in 0502c1a, logistics/LootScan.cpp ForEachLoadedRef) -- unknown build skips the sky cell silently (raised against 75fadf7)
"On an unknown build the sky cell is skipped with no log line. A one-time warning would be enough."

### MFO-B429 (SEV-5, FIXED in 0502c1a, LootScan.cpp comment, Docs/ENGINE_NOTES.md §0.30) -- imprecise GetSkyCell caller counts (raised against 75fadf7)
"The author's caller counts in the code comment and ENGINE_NOTES ('30/31 callers') are imprecise (reviewer counted 39/40/35). The conclusion holds."

Note (SEV-4, tracked as a fork task, not here): the mit-3.7 fork's TES.h guards AE members with the never-defined SKYRIM_SUPPORT_AE (worldSpace, landBorderMode, sizeof assert; same class in UserEvents.h, InterfaceStrings.h, BGSSaveLoadManager.h). MFO avoids the TES walks by rule until the fork is corrected.
### MFO-B430 (SEV-4, OPEN, native/FollowerCatchUp.cpp MoveOnMain) -- the 250 u MoveTo offset can land him inside geometry in interiors (raised against 8fe6952)
"The catch-up uses the same ~250 u behind-the-player offset as Errand.cpp RecallToPlayer. In a small interior that point can be inside a wall or furniture. Same exposure as RecallToPlayer; watch in the field."

### MFO-B431 (SEV-4, OPEN, native/FollowerCatchUp.cpp kSettleS) -- the 3 s settle is shorter than the engine's own carry (raised against 8fe6952)
"Sven was measured absent for about 40 s after a load (Followers.cpp kLoadWindowS). A follower who is walking through the door himself therefore appears behind the player instead of at the door when the catch-up fires at 3 s."

### MFO-B432 (SEV-5, OPEN, Board_Progression.cpp IsCraftTreeAv / progression/ProgAllocator_internal.h IsCraftSkill) -- progression: the craft-skill set is duplicated (raised against 0502c1a)
"IsCraftTreeAv duplicates the allocator's IsCraftSkill, so the two could drift. Expose one copy in the public ProgAllocator.h later."

### MFO-B433 (SEV-5, OPEN, logistics/LootScan.cpp WalkLoaded) -- the three-way runtime check repeats Runtime::IsVerifiedRuntime() (raised against 0502c1a)
"LootScan's three-way IsVerified1_5_97 / 1_6_1170 / 1_7_104 check in the unknown-build warning repeats Runtime::IsVerifiedRuntime() (Runtime.h:70)."
### MFO-B434 (SEV-5, OPEN, logistics/Errand.cpp TryStart `s_priceDefers`) -- never cleared on load / revert (raised against c83d197)
"The F1 per-follower price-defer counter is a function-local static: it is erased only when a plan sees no deferral, not on load or revert. A stale count can shorten the next wait."

### MFO-B435 (SEV-4, OPEN, LocationTypes.h InnOf, pre-existing data) -- vanilla jail locations carry LocTypeInn (raised against c83d197)
"FalkreathBarracksJailLocation and RiftenHoneysideLocation carry LocTypeInn in vanilla, so InnOf may name the jail before Dead Man's Drink. Names only (logs/Board); the walk goes through the load-door search."

### MFO-B436 (SEV-4, OPEN, informational, LocationTypes.h TownAnchor) -- outermost anchor widens "the town" (raised against c83d197)
"With Lux Via-style nesting, Left Hand Mine and similar places now count as Markarth, so walking there does not end an errand as 'left town'."

### MFO-B437 (SEV-4, OPEN, logistics/Errand.cpp Tick inn-stage retry) -- a failed retry declare loses the stop (raised against a658951)
"The inn-stage retry block clears ls.retryLater before the walk is declared, so when the declare fails the stop is no longer on the retry-later list."

### MFO-B438 (SEV-4, OPEN, logistics/ErrandRest.cpp Rest::End) -- log says 'the trip is over' on a noMark retry (raised against a658951)
"Rest::End logs 'the trip is over' for a_tripOver=true even when the inn-stage retry ends the stay with noMark; the trip goes on."

### MFO-B439 (SEV-5, OPEN, logistics/ErrandRest.cpp ~:861) -- stale comment (raised against a658951)
"The F4 comment still says the temple is SKIPPED when no door is found or readable; since run 4 only a LOCKED door skips."

### MFO-B440 (SEV-5, OPEN, logistics/ErrandSim.cpp RET_SIM mark) -- marks without anyDone (raised against a658951)
"The RET_SIM MarkTripDone fires without the anyDone condition NextStopOrFinish uses."

### MFO-B441 (SEV-4, OPEN, logistics/ErrandSim.cpp) -- off-screen trips never retry skipped stops from the inn (raised against a658951)
"The inn-stage retry-later exists only in the on-screen Tick; an unloaded follower's REST_SIM never retries stops skipped for a failed walk."

### MFO-B442 (SEV-4, OPEN, logistics/ErrandPlace.cpp:81-182, Errand.cpp LoadStop) -- town run 5: a late Place request can still teleport him after MFO gave up (raised against ed41d50)
"Request / ResolveCellOnMain re-create a cleared job through g_jobs[a_fid] and call RequestPlace without first checking that the job still exists and is unrequested. A door-scan result that lands after Poll has timed out (15 s), or after stopOver/LoadStop moved to the next stop (they do not call Unstick::Clear), still teleports him to the old stop. Timeouts use steady_clock rather than Scheduler::ServiceClock()." Fix: find-and-check (!requested, same gen) under the lock before RequestPlace; ServiceClock timeouts; Unstick::Clear in LoadStop. ~one-frame window.

### MFO-B443 (SEV-4, WATCH, ErrandPlace / Harbinger APMF-B149) -- first observation of a LOADED placement (raised against ed41d50)
marth R1 (always teleport, even in view) overrides Harbinger APMF-B149 ("MFO should always pass OnlyIfUnloaded; the first field run should show a loaded placement before anyone relies on one"). Next town test: read the Harbinger [place] PLACED line with info WasLoaded, and any FAILED NotMoved with a few units of distance (1 u read-back tolerance false failure on a loaded character).

### MFO-B444 (SEV-5, OPEN, Errand.cpp NextStopOrFinish noInnTown arm) -- a shop-only trip in a no-inn town counts on any Done stop even if the last shop was skipped (raised against ed41d50)

### MFO-B445 (SEV-5, OPEN, Errand.cpp giveUp after placements) -- both placements spent logs a generic WARN; under R4 it should be an ERROR naming the placements ("placed 2/2, still stuck: UNREACHABLE") (raised against ed41d50)

### MFO-B446 (SEV-5, OPEN, ErrandPlace.cpp:154) -- fallback places 90 u along the interior door's own heading; door mesh +Y into the room not verified (only when the landing point is >800 u from the linked door) (raised against ed41d50)

### MFO-B447 (SEV-5, OPEN, TradeBridge.cpp ReportTrade comment; MFO_Trade.psc) -- stale psc line cites after the reorder; a form on both the sell list and buy plan could be sold back (no current planner path produces it) (raised against ed41d50)

### MFO-B448 (SEV-5, OPEN, Errand.cpp NotPlanning, against 6613442) -- "not planning -- player not in a town" / combat reasons repeat every 30 s per follower; a change-only option for steady-state reasons would cut log volume.

### MFO-B449 (SEV-4, OPEN, TradeBridge.cpp ReportTrade sell replay, against 7f75f5b) -- missed sells: a coincidental totals match with equal unit prices can mis-file rows
"sell-replay coincidental totals match with equal unit prices can mis-file rows (needs per-row Papyrus results; frozen ReportTrade signature)" (Opus review of feat/mfo-missed-sells, F5). The replay is accepted when its sold count and value equal the report; two rows with the same unit price can swap which one is called unsold.

### MFO-B450 (SEV-4, OPEN, logistics/ErrandReopen.cpp Record, against 7f75f5b) -- missed sells: Record drops a gold entry whenever the new trip visits that faction
"Record drops a gold entry whenever the new trip visits that faction even if that stop carries no unsold rows" (Opus review of feat/mfo-missed-sells, F6).

### MFO-B451 (SEV-5, OPEN, Errand.cpp TryStart log / ErrandReopen.cpp Assemble, against 7f75f5b) -- missed sells: stale log line; the stop cap drops late forms
"stale 'no hours-closed shop holds work for him -- no watch' log line (the Watch is now always stored), and Assemble's stop cap cuts trailing shops without re-queuing their late forms" (Opus review of feat/mfo-missed-sells, F7).

### MFO-B452 (SEV-4, OPEN, logistics/ErrandReopen.cpp DropGoldForms) -- a late trip drops ALL its job.forms from the gold watch, incl. forms no stop of the late trip carries (raised against 6e97df7)
"DropGoldForms drops every form in job.forms from the gold watch, including forms that no stop of the late trip actually carries (Trip::Plan placed only some, or the stop cap cut them; related to B451). Such a row loses its gold-watch retry at the original vendor; the item stays in inventory so the next normal trip can still pick it up."

### MFO-B453 (SEV-4, OPEN, logistics/ErrandRest.cpp PostShopDoorRead, against d68446e) -- per-cell door reads each walk the grid + up to 64 interiors in one frame
"per-cell door reads each walk the grid + up to 64 interiors in one frame every 5 min (batch them)" (Opus review of feat/mfo-college-vendors, F2). Every interior shop cell posts its own main-thread read; a town with many shop cells walks the loaded grid (and the first-hop interiors) once per cell in the same frame.

### MFO-B454 (SEV-4, OPEN, logistics/ErrandRest.cpp ShopDoorFor, against d68446e) -- a None verdict is cached 5 min regardless of player movement; no defer cap / warn
"None verdict cached 5 min regardless of player movement + no defer cap/warn" (Opus review of feat/mfo-college-vendors, F3). A shop whose door was not in the loaded grid when read stays out of the catalog for 5 real minutes even after the player walks next to it; the Pending path has no defer cap or WARN like PlacesPending's.

### MFO-B455 (SEV-4, FIXED (part 1) on feat/mfo-college-vendors in the R-A/R-B commit after 72180d9, logistics/Errand.cpp BuildTownShops chest anchor, against d68446e) -- College physical arrival often unattended by day
FIX (marth 2026-10-21, "1 now, 2 next week"): a chest-placed shop whose vendor is not found on arrival trades with the merchant chest like TRADE_SIM (`Errand.cpp` Tick Walking, `ChestPlacedFaction`), WARN "vendor not present -- traded with the merchant chest (chest-placed shop)". Part 2 (walking on to find the vendor) is next week's task.
"College physical arrival often unattended by day (chest vs Hall of the Elements)" (Opus review of feat/mfo-college-vendors, F4). The College vendors' chests (the store cells) are in the Hall of Countenance / Hall of Attainment / Arcanaeum, while by day the vendors sandbox in the Hall of the Elements; the on-screen arrival then relies on the vendor search and the unattended-shop retries. The off-screen simulation needs no vendor.

### MFO-B456 (SEV-4, FIXED on feat/mfo-college-vendors in the R-A/R-B commit after 72180d9, logistics/ErrandShops.cpp SpouseGate / R6 rule, against d68446e) -- any conditioned non-spouse vendor is dropped; needs a marth ruling
FIX (marth 2026-10-21, "Evaluate the conditions"): `SpouseGate` evaluates a non-spouse conditioned vendor's own conditions (`ConditionGate`, subject = the loaded vendor else the player) and opens it when they pass; the spouse's own store is unchanged.
"R6 rule drops any conditioned non-spouse vendor (LoreRim CollegeEntry.esp drops Faralda), needs marth ruling" (Opus review of feat/mfo-college-vendors, F5). CollegeEntry.esp (Improved College Entry) adds two conditions to ServicesWinterholdCollegeFaralda, so R6 treats her as a spouse-store candidate and closes her.

### MFO-B457 (SEV-4, OPEN, logistics/Errand.cpp BuildTownShops chest anchor, against d68446e) -- chest anchor can route a fence via the Ratway; an exterior chest has no reachability check
"chest anchor can route a fence via the Ratway / exterior chest has no reachability check" (Opus review of feat/mfo-college-vendors, F6). A chest-placed fence whose chest sits in a Ratway / guild interior is walked to like any shop cell; a chest in an exterior cell is a Ref stop with only the straight-line travel-radius test.

### MFO-B458 (SEV-5, OPEN, logistics/Errand.cpp TryStart College mage gate, against d68446e) -- the tome-wish mage test counts any cast gambit
"tome-wish mage gate counts any cast gambit" (Opus review of feat/mfo-college-vendors, F7). `g_economyBuyTomes && HasCastGambit` lets a melee follower with one utility cast gambit (a heal, a ward) into the College.

### MFO-B459 (SEV-4, OPEN, logistics/ErrandShops.cpp SpouseGate, against 14789a2) -- ServicesSpouse* factions now reach ConditionGate
"The non-spouse branch now also receives the ServicesSpouse* factions that used to be hard-Closed: no spouse, the player married to someone else, or a loaded member who is not the spouse. Whether they stay closed rests on the new comment's claim that 'their conditions name their own spouse NPC, so they stay closed by their data'. That claim is unverified, and the file's own header (line 17-18) says these factions carry GetInFaction(<job faction>) on the subject. With a loaded member vendor as subject, that condition passes for any merchant in the job faction. The exposure is narrow: in vanilla the spouse factions likely have no members and no PLVD before marriage, and a chest-anchored spouse chest sits outside any town. Still, verify the spouse-faction condition data, or keep spouse factions closed unless the spouse is a member." (Opus review of feat/mfo-college-vendors at 14789a2, relayed by the release driver).

### MFO-B460 (SEV-4, OPEN, logistics/ErrandShops.cpp SpouseGate -> ConditionGate, widens B360, against 14789a2) -- faction-only conditioned shops are gated with the player as subject
"For a faction-only (unloaded-vendor) conditioned shop, ConditionGate evaluates the vendor's subject-side conditions with the PLAYER as subject (EvaluateOnMain :93-96). The engine evaluates them with the vendor as subject. Before F5 this mismatch could only reach fences (B360). Now it reaches every load order's conditioned non-spouse vendor. A condition like GetInFaction / GetActorValue / GetIsID on the subject is answered for the player: it closes a legitimate vendor when it fails (conservative), and it can open a vendor the engine would refuse when the player happens to satisfy it." (Opus review of feat/mfo-college-vendors at 14789a2, relayed by the release driver).

### MFO-B461 (SEV-4, OPEN, logistics/Errand.cpp BuildCatalog faction-only list + R-A arrival trade, against 14789a2) -- no alive/enabled check on a faction-only shop vendor
"Nothing in the faction-only shop path checks that the vendor is alive or enabled. It has only OpenAt, park and backoff, with no IsDead or IsDisabled check, unlike the loaded-vendor path at :382. So a chest-placed shop whose vendor was killed (for example a College vendor) is still planned, and on arrival R-A now trades with the dead merchant's chest ON SCREEN. Before this branch, on-screen play only reached the unattended retries and then gave up. The off-screen Sim already had this behaviour (TRADE_SIM with no vendor)." (Opus review of feat/mfo-college-vendors at 14789a2, relayed by the release driver).

### MFO-B462 (SEV-5, OPEN, logistics/ErrandShops.cpp file header lines 14-18, against 14789a2) -- header still says every other conditioned vendor stays dropped
"The header still says 'Every other conditioned vendor stays dropped (marth 2026-10-18, Q10), exactly as before', which F5 now contradicts. The SpouseGate body, Errand_internal.h and MAP.md were updated, but the file header was not." (Opus review of feat/mfo-college-vendors at 14789a2, relayed by the release driver).

### MFO-B463 (SEV-5, OPEN, logistics/Errand.cpp R-A arrival trade, against 14789a2) -- on-screen merchant-chest trade plays no give or take idle
"The on-screen merchant-chest trade plays no give or take idle; items just move. That matches marth's 'F4 part 1' ruling, but it conflicts with the standing rule that every action is animated. Address it in part 2." (Opus review of feat/mfo-college-vendors at 14789a2, relayed by the release driver).

### MFO-B464 -- v2.7.1 review SEV-4/5 (raised against a455cb0, Opus review)
- **SEV-4:** 2c fall-through cooldown reads `r.reported`/`r.dispatched` after `AdvanceStop` cleared them, so a restored record that traded gets a 0 s cooldown (ErrandSim.cpp final DONE branch).
- **SEV-4:** `InnFactionsOf` food filter drops a modded inn whose innkeeper list uses a custom food keyword (town then rests at the last shop). Log the filtered faction.
- **SEV-5:** the catalog cap moved after the loop, so fence/spouse gates and `ShopDoorFor` posts run for shops the cap would have cut.
- **SEV-5:** `clamp(0.5*destRad, 20, 90)` also moves vendor-actor placements to 85 u, and a radius-50 ref to 25 u (could land inside counter collision).
- **SEV-4 (MFO-B464, re-review of 53f902b):** `refNear` compares no worldspace (a city child worldspace shares Tamriel's coordinates): add `GetWorldspace()` equality.
- **SEV-4 (MFO-B464):** a 2c advance past a stop whose trade finished but whose outcome was not yet taken (dispatched, not reported, no live order) marks it Skipped and nothing takes the outcome before a rest-only inn or temple: tripSpent and NoteUnsold / NoteLate are lost.
- **SEV-4 (MFO-B464):** a rest-only inn resumed on screen whose innkeeper is loaded in his cell walks to the innkeeper and runs the vendor path with no order.

### MFO-B465 (SEV-5, OPEN, logistics/Economy.cpp BuildBuyThresholds equip-off buy gate, raised against cc1dc1f, Opus tier-3 review) -- EquipArmor 0 also stops shield buys
With EquipArmor at 0 and EquipWeapon on, `buy.buyArmor = false` also stops shield buys, although a shield ranks on EquipWeapon (the sell side follows EquipWeapon for a shield). Asymmetric with the sell scan; harmless (no shield is bought, none is sold while kept).
### MFO-B470 (SEV-4, OPEN, logistics/ErrandVendors.cpp BuildIndex, against a19762b, Opus review of fix/mfo-272) -- vendor index main-thread cost unmeasured
"BuildIndex main-thread cost unmeasured (read 'vendor index built ... N ms'; if > ~30 ms move to kPostLoadGame)". Reviewer's reasoning: the build walks the whole all-forms map on the main thread once per load (and again per town change / NoMembers rebuild after review F1); its cost on a large list is not known until the field line is read.

### MFO-B466 (SEV-4, OPEN, logistics/Errand.cpp BuildCatalog + Reopen / later stops, against a19762b, Opus review of fix/mfo-272) -- destinations frozen at plan time
"destinations frozen at plan time (relocate re-aims <= 3)". Reviewer's reasoning: a stop's vendor destination is located at PLAN time; a reopen-watch re-plan or a later stop walks to where the vendor was then, and only `relocate` (cap 3 per stop) corrects it on arrival.

### MFO-B467 (SEV-5, OPEN, logistics/Errand.cpp tryPlace / retryWalk, against a19762b, Opus review of fix/mfo-272) -- a stuck walk in dialogue is skipped without an ERROR
"tryPlace returns false in dialogue -> stop skipped without ERROR (re-arm one retry)". Reviewer's reasoning: `tryPlace` treats dialogue as "not stuck" and returns false with no line, so a spent-retries walk while he talks to the player falls to giveUp; re-arm one retry instead.

### MFO-B468 (SEV-5, OPEN, logistics/ErrandVendors.cpp Locate) -- NoMembers re-arm fires when all indexed members were skipped as unloaded temporaries (raised against 996c445)
"The NoMembers re-arm also fires when the index has members for the faction but F4 skipped them all as unloaded temporary actors. A rebuild cannot fix that case: a pointless rebuild plus a ~6 s plan wait every minute." Fix: re-arm only when ids is empty.

### MFO-B469 (SEV-5, OPEN, logistics/Errand.cpp ~553) -- vendor-located shops wait on the place-door read only the smith stop needs (raised against 996c445)
"A vendor-located shop now waits on its place-door read even though only the smith stop uses the place. Bounded (5 min cache). Could be skipped when the shop is not a smithy candidate."

### MFO-B471 (SEV-5, OPEN, apmf/Substrate.cpp SweepSubstrateCasts backstop, against 43906e5, Opus tier-3 review of feat/mfo-substrate-followup, finding F4) -- the backstop has no wall-clock cap
"F4 SEV-5 backstop no wall cap (ok)". Reviewer's reasoning: `kBackstopSec` (27.5 s) runs on `Scheduler::ServiceClock` (unpaused game time) so a menu or a pause never ages a pair; the cost is that a pair whose clock stops advancing (a long pause, a stalled pump) is never cut by wall time. Graded acceptable: Harbinger's own TTLs bound both claims on its side, and the pair is released by the normal close path once the clock moves.

### MFO-B472 (SEV-5, OPEN, CombatStyle.cpp ApplyTick restorePending, against 9bdd6fa, Opus tier-3 re-review of feat/mfo-substrate-followup) -- a lingering restore, or controller address reuse, writes the actor's own saved style
A restore armed by a reason=real-combat close (including the one frame between a normal close's StopCombat and the controller teardown) lingers until the follower's next combat tick, keeping `AnyActive` true meanwhile, and is then dropped on a different controller. If a new controller reuses the old address, the identity compare matches and the old fight's saved engine style is written onto the new controller. Graded benign: `saved` is the actor's own engine-derived style form (never freed), the write is one-shot, and nothing MFO-specific lands.

### MFO-B473 (SEV-5, OPEN, Followers.cpp ReleaseHeldStateImpl / CombatStyle::Clear, against 9bdd6fa, Opus tier-3 re-review of feat/mfo-substrate-followup) -- dismissal during a real fight leaves the cast style until that controller dies
`Clear` from dismissal or death drops only the combat-table stance; the pair's bit and a pending restore survive until the pair's own end. A follower dismissed during the real fight that took the substrate controller keeps MFO_CastStyle (or the restore stays armed) until that controller is destroyed. Graded benign: the style dies with the controller and the next fight re-baselines.

### MFO-B474 (SEV-5, OPEN, apmf/Substrate.cpp OpenSubstrateCast open-gap branch, against 9bdd6fa, Opus tier-3 re-review of feat/mfo-substrate-followup) -- the open-gap branch arms a restore without knowing the close reason
When a sweep ends the pair in the unlocked gap between the open and its `WantSubstrateCast`, the open's `gone` branch calls `ClearSubstrateCast(..., seq, true)` without knowing why the pair ended. On a normal close the armed restore lingers and is dropped on the next controller (the MFO-B472 shape). Graded benign; a fix would carry the close reason out of the sweep.

### MFO-B475 (SEV-4, OPEN, logistics/ErrandVendors.cpp, against ec5e235, Opus review of fix/mfo-273) -- a refused ConditionTrue seat drops a loaded member but an unloaded one counts by hours
The seat-refused case is inconsistent: `LoadedCounts` answers No (member dropped) while an unloaded member passes on hours. Graded: only on an unsupported build, where the seat refuses loudly anyway.

### MFO-B476 (SEV-4, OPEN, logistics/ErrandSim.cpp Sim::Begin alreadyThere, against ec5e235, Opus review of fix/mfo-273) -- a persistent-cell false positive
`alreadyThere` (parent cell identity) can be a false positive when an unloaded exterior vendor reports the worldspace's persistent cell and the follower is in it.

### MFO-B477 (SEV-5, OPEN, logistics/ErrandVendors.cpp EvaluateOnMain, against ec5e235, Opus review of fix/mfo-273) -- a stale-generation evaluation erases pending and can cause one extra post
After a load bumps the generation, the dropped evaluation still erases the actor from `s_verdictPending`, so the next call may post one extra evaluation.

### MFO-B478 (SEV-5, OPEN, logistics/ErrandVendors.cpp Locate, against ec5e235, Opus review of fix/mfo-273) -- the NotOffering log carries no count of failing members
The "NOT a candidate: no member would trade" line does not say how many members failed which gate.

### MFO-B479 (SEV-4, OPEN, logistics/ErrandVendors.cpp s_jmActors / s_jmShops, against ad85581, Opus re-review of fix/mfo-273) -- the JobMerchant sets are stale until the next town change
The sets are built at the index build. If a quest alias grants JobMerchantFaction rank 0 mid-session, they stay stale until the index is rebuilt (the player's town changes).

### MFO-B480 (SEV-4, OPEN, logistics/Errand.cpp relocate ~1916 and ErrandVendors.cpp Locate, against ad85581, Opus re-review of fix/mfo-273) -- Pending falls to stopOver with no retry, and one unknown verdict hides a Located member
`relocate` returning false on `Pending` falls to the stop-over path with no retry. `Locate` also returns `Pending` when any loaded member's verdict is unknown even if another member is already Located.

### MFO-B481 (SEV-4, PARTLY SUPERSEDED by fix/mfo-errand-requeue: the in-trip `Reopen::Requeue` folds a missed kind into an already-pending stop; the hourly Poll path is unchanged, logistics/ErrandReopen.cpp Poll MISSED BUY, against f1953f8, Opus review of fix/mfo-missed-buy) -- a missed-buy candidate already in `open` is skipped
Poll skips any missed-buy candidate already in `open` (reopened / gold-watch shop) instead of adding the missed kind to that shop's order; the kind goes to a farther shop or is dropped.

### MFO-B482 (SEV-5, OPEN (the no-watch line now also records the shop in the live tried set), logistics/ErrandReopen.cpp NoteUnfilled, against f1953f8, Opus review of fix/mfo-missed-buy) -- wrong reason logged with bErrandInn off
With bErrandInn off, NoteUnfilled says "a trip restored from the save"; the real reason is the inn setting is off so no watch was recorded.

### MFO-B483 (SEV-5, OPEN, logistics/ErrandReopen.cpp Poll MISSED BUY, against f1953f8, Opus review of fix/mfo-missed-buy) -- the missed-buy order keeps the template's sig
The missed-buy order keeps the template's sig, so its empty-trade memo is keyed by another shop's signature.

### MFO-B484 (SEV-5, OPEN, logistics/ErrandTrip.cpp Plan step 2, against f1953f8, Opus review of fix/mfo-missed-buy) -- the "NO stock evidence" line is unthrottled
ErrandTrip.cpp "need kind N has NO stock evidence" repeats on every re-plan, unthrottled.

### MFO-B485 (SEV-5 info, OPEN, logistics/Errand.cpp, against f1953f8, Opus review of fix/mfo-missed-buy) -- file size
logistics/Errand.cpp is 2583 lines, split due (cap relaxed until next major).

### MFO-B494 (SEV-4, OPEN, logistics/Errand.cpp PlanLiveNeeds, against fix/mfo-errand-requeue) -- interior shops with a vendor in another cell are skipped when Harbinger lacks cross-cell travel
The town-wide index builder has no entry-door read (that is BuildCatalog's cell walk), so with `APMFBridge::ErrandCrossCellsSupported()` false an interior-vendor shop is not a candidate of a LIVE NEED RE-CHECK (the log counts them).

### MFO-B495 (SEV-5, OPEN, logistics/TradeBridge.cpp, against fix/mfo-errand-requeue) -- the cheapest-unit floor is session-wide, not per vendor
`MinSeenUnitPrice` is the cheapest unit PlanBuy classified for a kind in any chest this session (-1 = never, then `iErrandMinPurse`); a kind no chest has stocked yet gates on the minimum purse.

### MFO-B496 (SEV-5, OPEN, logistics/ErrandReopen.cpp CarryAndClear/ApplyCarried, against fix/mfo-errand-requeue) -- a carried need builds its order from the previous town's template
A skipped order at the chosen shop is rebuilt from the carried `missedTmpl` (its quote and ids belong to another vendor, as in Poll's MISSED BUY plan; sig is not reset, cf. MFO-B483).

### MFO-B497 (SEV-5, RESOLVED in the review round: fTempleRealSec 12 / fSmithWorkRealSec 60, old keys read and ignored, logistics/ErrandSim.cpp, against fix/mfo-errand-requeue) -- `iTempleGameMin` / `iSmithWorkGameMin` were game minutes
Only the shop/inn work and the walk were moved to real seconds x timescale; the temple and smith stage costs have no measured on-screen counterpart yet.

### MFO-B498 (SEV-5 info, OPEN, logistics/Errand.cpp, against fix/mfo-errand-requeue) -- file size
logistics/Errand.cpp grew by the live-need planner (2812 lines, from 2585); split due (cap relaxed until next major).
### MFO-B490 (SEV-4, OPEN, cast/SubstrateRoad.cpp LongPaceSecs / FileSubstrateCast, against 1b8e551, Opus review of fix/mfo-substrate-recast) -- pacing-window hole for multi-effect buffs
`LongPaceSecs` and `FileSubstrateCast`'s already-up check use the costliest effect's duration. The old `g_beneficialRecast` arm used `AuthoredDuration`, the longest duration across all effects. So a spell whose costliest effect is instant or short, with a long secondary effect, gets only the 3 s floor after it fires. Gate 1 that remains is `ShouldApplyTo`'s active-effect scan, which matches on `ae->spell == a_spell`. When Harbinger's self-flip proxy delivers the spell (a non-Self buff cast on the caster), the proxy is the effect's spell, so the scan misses. Gate 2 checks the costliest MGEF, which is gone. The result would be a recast about every 3 s after a fire. The case is narrow, and the same hole already exists for multi-recipient sets and in Service.cpp. Suggested fix, as its own round: make `LongPaceSecs` use the longest duration across effects.

### MFO-B491 (SEV-4, OPEN, cast/Auto.cpp OOC order + SubstrateRoad.cpp SubstrateLastWindowUnfired, against 1b8e551, Opus review of fix/mfo-substrate-recast) -- first-in-line recipient can end the lap
`SubstrateLastWindowUnfired` is also true for a recipient whose open Harbinger REFUSED (`g_subPace` is written even when `!filed`). That recipient goes first, and if Harbinger refuses it again, the follower-wide Refused ends the lap. Other recipients still get served on laps where it sits in its short window, so this is alternation, not starvation. The shape existed before, it is just more consistent now.

### MFO-B492 (SEV-5, OPEN, MAP.md SubstrateRoad entry, against 1b8e551, Opus review of fix/mfo-substrate-recast) -- stale line refs
MAP.md line references are stale. `FileSubstrateCast (:129)` is now about :147, `TrySubstrateCast (:243)` about :292, and the "(248)" file size about 300.

### MFO-B493 (SEV-5, OPEN, cast/SubstrateRoad.cpp check 7, against 1b8e551, Opus review of fix/mfo-substrate-recast) -- double log on Harbinger refusal
A check-7 refusal now logs an info line on top of `OpenSubstrateCast`'s own ERROR. Harmless.

### MFO-B499 (SEV-4, OPEN, Errand_internal.h ShopPlaceCellName ~74-77 + TradeBridge VM label, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- no interior check on the chest-cell name step
A hidden holding cell could lend its name to the shop (no IsInteriorCell test on the merchant chest's cell).

### MFO-B500 (SEV-4, OPEN, ErrandReopen.cpp ~664-672, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- stale sig after a fold
BuyOrderFor and ApplyCarried the same; recompute with TradeBridge::TradeSignature.

### MFO-B501 (SEV-4, OPEN, ErrandSim Requeue call, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- sim Requeue sorts by r.lastPos
It should use r.stops[r.cur].pos.

### MFO-B502 (SEV-4, OPEN, ErrandReopen.cpp Requeue, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- a Requeue-placed stop later skipped loses its kind
The kind is erased from w.missed and never lands in unmet.

### MFO-B503 (SEV-4, OPEN, Errand.cpp TryStart cooldown site, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- the cooldown site skips TryStart's settle gate (fErrandSettleSec)

### MFO-B504 (SEV-4, OPEN, Errand.cpp PlanLiveNeeds, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- one live re-check trip covers one shop per kind
N back-to-back trips are possible.

### MFO-B505 (SEV-5 bundle, OPEN, against c9a0dda, Opus review of fix/mfo-errand-requeue) -- closing notes
ShopPlaceName is dead code; EconomyLiveSupplyNeeds duplicates the BUY NEEDS rule (Economy.cpp:1016-1036); the PlanLiveNeeds log prints kind0 not BuyKindName; s_gate is not cleared on revert; the StartReopenedTrip failure line wording is wrong for LIVE NEED trips; the backoff first step is 24 s, not 12 s; the NoteUnfilled watch path records only the key (a multi-member faction may retry once).

### MFO-B506 (SEV-4, OPEN, logistics/Errand.cpp g_coolDone, against e8b27e7, coordinator read of the fix/mfo-errand-requeue review round) -- completion cooldown recognised by value
`Finish` and `detail::Cooldown` mark a cooldown as the completion one when `a_cooldownSec == Config::g_errandCooldownSec`, so a failure cooldown that happens to equal `fErrandCooldownSec` (user-configured) is treated as completion and the live need re-check bypasses it. Pass an explicit completion flag instead of comparing seconds.

### MFO-B507 (SEV-4, OPEN, TradeBridge.cpp PlanBuy POTION CAP, against 1b29b7d, author note of fix/mfo-potion-cap-stock) -- the potion cap is per order, not per trip
The cap is half of the budget the order's supply pass starts with (the trip purse left, since the order carries it), computed once per PlanBuy call. A multi-stop trip can therefore still spend 1/2 at the first stop, 1/2 of the rest at the next (3/4 over two stops). Matches how R7 food / R8 materials cap (per pass), so it is not a regression, but a per-TRIP potion ceiling would need the trip's original purse on the order.

### MFO-B508 (SEV-5, OPEN, logistics/ErrandTrip.cpp StockNow, against 1b29b7d) -- ammo keeps the VEND claim until a stock reading exists
Ammo kinds (3, 4) have no apothecary-class analog, so `needStock` for them is still `needCarry` while the chest has no fresh reading; only a reading of 0 lines removes it. A general store whose VEND lists arrows but which stocks none can therefore still be one buy stop per visit for ammo (the zero reading then ends it). Decide whether ammo should be stock-only with a "smith/general class" rule.

**UPDATE (fix/mfo-ammo-stock-class, 2026-10-26, marth: "Assume a blacksmith sells arrows. There are archery specific shops too. But a regular blacksmith almost always does"):** ammo now has an ARMS CLASS (`ArmsClass`, `Errand.cpp`: vendor NPC in JobBlacksmithFaction 0005091D, or a plain VEND list carrying VendorItemArrow with >= half Weapon/Armor/Arrow keywords). `Trip::StockRank` orders ammo evidence last-seen stock (3) > arms class (2) > VEND claim alone (1); `Trip::Plan` picks the best-ranked usable candidate. The VEND-only claim remains as the fallback (so the general-store case in this finding is demoted, not removed); a last-seen 0 reading still vetoes. Still OPEN: whether to drop the VEND-only fallback entirely.

### MFO-B515 (SEV-5, OPEN, ErrandTrip.cpp:303-306, against f0f2ce0, Opus review of fix/mfo-ammo-stock-class) -- cap-full free-try log text inaccurate
Cap-full + only chosen stocking stop is rank 1 -> fallback covers via carries(i) but logs "NO stock evidence at any usable candidate" (false; log only).

### MFO-B516 (SEV-5, OPEN, ErrandTrip.cpp:394-395, against f0f2ce0, Opus review of fix/mfo-ammo-stock-class) -- ammoUpgrade ignores rank
ammoUpgrade rides the first stop with needCarry arrows ignoring rank (swap-up may land on a VEND-only general store while the need goes to the blacksmith).

### MFO-B509 (SEV-5, OPEN, logistics/ErrandReopen.cpp g_unserved, against 1b29b7d) -- unserved needs are keyed by follower only
`NoteUnserved` files a need per follower with the last template; `CarryAndClear` moves it to the carried list when the town changes. If a plan in the same town later serves the kind, the stale entry is dropped at the next town by `ApplyCarried`'s live-count check (not before), and a Poll replan can file the same kind again (idempotent MergeKind, no extra log).

### MFO-B484 / MFO-B504 status note (1b29b7d, fix/mfo-potion-cap-stock)
B484: the "NO stock evidence" line in Plan step 2 is reworded (no buy trip; carried) and now fires only for a need with neither a stock stop nor a planned sell stop to try it at; still logged on every re-plan, so still open as a throttling item. B504: narrowed, not closed; with stock-only, a live re-check trip is built only over shops with stock evidence, so the "one shop per kind" limit now costs a trip only where there is stock.

### MFO-B510 (SEV-4, OPEN, MFO_Trade.psc:42-50 + ErrandTrip.cpp StockNow:47, against 1b29b7d, Opus review of fix/mfo-potion-cap-stock) -- pre-existing 128-line truncation now a veto
MFO_Trade.psc:42-50 fills stockCounts = new int[128]; lines past 127 get count 0; a large modded apothecary chest can file a 0 potion reading, which under this branch vetoes the shop for a restock window (StockNow:47). Root fix: size the counts array to m, or file -1 when truncated (separate Papyrus change).

### MFO-B511 (SEV-5, OPEN, TradeBridge.cpp:156-158, :303, against 1b29b7d, Opus review of fix/mfo-potion-cap-stock) -- silver ammo is not counted as arrows/bolts
wantSilverAmmo classifies silver arrows/bolts as kAmmoSilver (kindLines[5]) not kArrows/kBolts (TradeBridge.cpp:156-158, :303); a silver-only shop files arrows=0 and is vetoed for other followers until restock.

### MFO-B512 (SEV-5, OPEN, Errand.cpp PlanLiveNeeds:1582-1585, against 1b29b7d, Opus review of fix/mfo-potion-cap-stock) -- a no-stock kind is carried only when no trip is made
PlanLiveNeeds (Errand.cpp:1582-1585) files NoteUnserved only when usable==0; a no-stock kind dropped while other kinds made a trip is not carried (forced status lost).

### MFO-B513 (SEV-5, OPEN, ErrandTrip.cpp:48, against 1b29b7d, Opus review of fix/mfo-potion-cap-stock) -- StockNow with lines>0 still requires needCarry
StockNow with lines>0 still requires needCarry (ErrandTrip.cpp:48); loses a shop with potions in stock but no VendorItemPotion keyword.

### MFO-B514 (SEV-5, OPEN, ErrandTrip.cpp:296, against 1b29b7d, Opus review of fix/mfo-potion-cap-stock) -- free-try log text inaccurate
Free-try log text at ErrandTrip.cpp:296 ("already planned for selling") is inaccurate when the stop was added for another kind's stock.

### MFO-B517 (SEV-4, OPEN, CombatStyle.cpp ApplyTick, against bdf6ae9, Opus review of fix/mfo-dualwield-gate) -- stale ESP keeps 0x833 on a flip to non-dual
If a follower already owns 0x833 on the live controller and flips to non-dual, the early return happens before the HANDOFF. 0x833 then stays on that controller for the rest of the fight. This only happens with a stale ESP (g_meleeSingleStyle null; the load-time MISSING error is loud).

### MFO-B518 (SEV-4, OPEN by design, logistics/Gear.cpp offHand vote, against bdf6ae9, Opus review of fix/mfo-dualwield-gate) -- 2H-class follower with left-hand perk lead gets the dual style
A 2H-class follower whose perks lead on left-hand weapons gets allowDual = true. The vote ignores the 1H/2H class, as the Equip fill does, so MFO_MeleeStyle could let the engine AI swap to two carried one-handers. Consistent with the ruling's literal "perks support it".

### MFO-B519 (SEV-5, OPEN, docs/comments + cost, against bdf6ae9, Opus review of fix/mfo-dualwield-gate) -- stale comments, per-tick inventory scan
(1) logistics/Logistics_internal.h:281-285 still says offHand is "DETECTED AND LOGGED ONLY" and that "MFO_MeleeStyle does not" allow dual wielding (pre-existing). (2) MAP.md:4911 says "NEXT_OBJECT_ID is now 0x916" (now 0x917). (3) MFO_GenerateESP.py:121 comment "(0x916 since R7 ..." next to value 0x917 reads confusingly. (4) HEDR numRecords hard-coded to 100 (MFO_GenerateESP.py:294, pre-existing). (5) One GetInventory() scan per follower per combat tick at the Want sites; caching roles per tick is optional.

### MFO-B520 (SEV-4, OPEN, SelfStatus.cpp, against af12ebc, Opus review of feat/mfo-gambit-conditions-1027) -- poisoned mirror lags a cure
After a Cure Poison lands, the mirror can still say poisoned for up to about 1 s plus a frame (kRefreshSec).

### MFO-B521 (SEV-5, FIXED (no Unreleased header in CHANGELOG.md, top section v2.8.1; triage 2026-11-08), CHANGELOG.md, against af12ebc, Opus review of feat/mfo-gambit-conditions-1027) -- Unreleased header
The '## Unreleased' header needs renaming to the version at the release cut.

### MFO-B522 (SEV-5, OPEN, Evaluator.cpp FoeIsSummoned, against af12ebc, Opus review of feat/mfo-gambit-conditions-1027) -- IsSummoned per-runtime offset, near redundant
FoeIsSummoned's IsSummoned() reads middleHigh->summonedCreature (+0x31C) with no per-runtime check (the offset looks stable on all three runtimes) and is nearly redundant with IsCommandedActor().

### MFO-B523 (SEV-4, OPEN, Evaluator.cpp Evaluate ally branch + cast/Fire.cpp kActCastTarget, against 65b31fe, Opus review of feat/mfo-gambit-conditions-1027) -- hostile spell can be cast at an ally
A hostile spell can be cast at an ally. The issue already existed for Ally: HP below and is now widened to four more selectors. Take a rule like "Ally: Paralysed / Downed / Poisoned -> Cast at target" with a destruction spell and TARGET = Auto or Ally. The selector sets a_choice.target, which turns off autoPick. ResolveCastTarget then returns the ally (or the player), and CastOn has no check that the spell's nature suits the recipient. The only hostility check is CastOn.cpp:406, which covers self-delivered buffs. Since Auto means "pick by the spell's nature", Auto with a detrimental or hostile spell on an ally selector should be a transparent NoOp. Reviewer: the player has to author the rule, so SEV-4.

### MFO-B524 (SEV-4, OPEN, Evaluator.cpp PickAllyState, against 65b31fe, Opus review of feat/mfo-gambit-conditions-1027) -- no line-of-sight filter
No heal reach / line-of-sight filter, none of the a_healSpell filtering PickAlly got in the review-C2 fix. Because it picks the nearest ally, distance reach is self-consistent. The remaining gap is line of sight: an occluded nearest downed ally shadows a visible farther one for "Ally: Downed -> heal". Minor.

### MFO-B525 (SEV-5, OPEN, SelfStatus.h header comment + MAP.md, against 65b31fe, Opus review of feat/mfo-gambit-conditions-1027) -- stale comment, missing backlog cross-reference
(1) SelfStatus.h still says "No co-save, no ResetAllState hook: the trust window ages a pre-load entry out by itself", which contradicts Clear() and the call at Serialization.cpp:1048. (2) MAP.md's gambit-conditions entry does not cross-reference MFO-B520..B525 (rule 9).

### MFO-B526 (SEV-5, OPEN note, SelfStatus.cpp, against 65b31fe, Opus review of feat/mfo-gambit-conditions-1027) -- refresh closures queue during a paused menu
While the player's Update is not running (a paused menu), a worker that keeps evaluating queues one refresh closure per actor every 3 s (kAskRetrySec). Drained on resume, growth negligible.

### MFO-B527 (SEV-4, OPEN, tools/splitcheck, raised by the F3 CommonLib proof round 2026-10-27) -- splitcheck tool gaps
(1) splitcheck.py:1833 `A.names_at.get(ra, ()) | B.names_at.get(rb, ())` ORs a set with a tuple and crashes in _data_diff. (2) A changed library source-path string is only treated as harmless when unnamed (_unnamed_equal / PATHLIT). When MSVC names the string after its contents (??_C@ symbols), a vcpkg build-folder change shows as FAIL and must be classified by hand.

### MFO-B528 (SEV-5, OPEN, apmf/CombatEntry.cpp, against fbe8d72, Opus review of fix/mfo-nopath-giveup) -- give-up falling edge seen only on a poll
"A falling edge is only recorded when something asks (the engage-on-sight out-of-combat lap, or PinTarget when the gambit picks that foe). A disengage and re-engage with no poll in between misses the lift."

### MFO-B529 (SEV-5, OPEN, apmf/CombatEntry.cpp Judge party-side lift, against 624ccd8, author's least-sure spot on fix/mfo-nopath-giveup) -- raw distance across worldspaces
The party-side lift compares raw coordinates with no same-cell or same-worldspace check, so a follower moved to another worldspace could read as having closed in and lift a give-up early.

### MFO-B530 (SEV-5, OPEN, EngageOnSight.cpp ABI-30 path comment, against 624ccd8, coordinator) -- comment misattributes a ruling
The ABI-30 comment "A RANGE DROP is not a give-up (marth 2026-10-12...)" misreads 10-12. marth 2026-10-27: the 10-12 ruling was that a follower stuck at the leash edge COULD chase (leash = distance from the player), not that distant foes should not be dropped (drop = distance from the follower). Reword the comment.

### MFO-B531 (SEV-4, OPEN, cast/Archetype.cpp, against bea32e9, Opus review of fix/mfo-cast-gates-1027) -- kRows lacks four ValueModifier-family archetypes
Verbatim: "kRows has no rows for Absorb, DualValueModifier, AccumulateMagnitude and PeakValueModifier, so the archetype prediction reads predicted=none for spells the engine keys to a ValueModifier row (Close Greater Wounds keyed Restore natively). Alias those four to the ValueModifier rows in Archetype.cpp, then a NoRow claim gate becomes possible." Reasoning: the D3 claim gate (OffenseClaimHasNoRow) therefore refuses only a pure Script row, not NoRow. Surfaced at edit time from: MAP.md cast/Archetype.cpp.

### MFO-B532 (SEV-4, OPEN, cast/BuffRoad.cpp BuffAimedLightWhy, against dbbf60a, Opus review of fix/mfo-candlelight-adopt-lift) -- kTouch Light at another recipient now takes a claim that never fires
Verbatim: "a kTouch Light cast at someone else now takes the claim road, Harbinger's Light 0x06 refuses non-Self non-TargetActor, so that claim never fires (bounded by BuffRoad's never-fired release; Touch Light is essentially absent from real data). Fix: add kTouch to the direct set." Reasoning: the narrowing of BuffAimedLightWhy to kAimed / kTargetLocation (2026-10-28) leaves kTouch on the claim road, where Harbinger's Light seat refuses it. Bounded by the never-fired release, and no real spell has this shape. Surfaced at edit time from: MAP.md cast/BuffRoad.cpp.

### MFO-B533 (SEV-4, OPEN, progression/VanillaSmithing.cpp parent edges, against d7a790b, Opus review of feat/vanilla-smithing) -- a filtered vanilla parent becomes no requirement
Reviewer: "A filtered vanilla parent becomes \"no requirement\". The catalog's second pass instead bridges to that parent's kept ancestors. For example, if Orcish were filtered, Ebony would need only 80 skill rather than Dwarven." Reasoning: unlikely in practice, since vanilla Smithing perks are kept. The false comment was corrected in f802943.

### MFO-B534 (SEV-4, OPEN, progression/PerkGate.cpp sweep, against d7a790b) -- a perk both stripped and bought keeps costing a point
Reviewer: "If a perk was recorded as stripped and the player then also bought it through MFO, RestoreNativePerksImpl skips it because it is already on the base. That part is safe: no double add. But the follower keeps paying a point for a perk that is now meant to be native and free, and a respec removes it for good. Edge case."

### MFO-B535 (SEV-4, OPEN, logistics/Errand_internal.h ShopPlaceCellName, against 5c41f7f, Opus review of fix/trade-names) -- a stall vendor's chest in a named holding cell would name the shop after that cell
Reviewer: "for a stall vendor like Adrianne, the step after the near-ref is the merchant chest's cell FULL. If that chest sits in a holding cell that has a FULL, the shop gets that cell's name instead of \"Adrianne's stall\". Holding cells usually carry only an EDID, so they would pass through to the stall name. This risk existed before the change. The first field run should show whether Adrianne reads as \"Adrianne's stall\"."

### MFO-B536 (SEV-5, OPEN, logistics/Errand_internal.h ShopPlaceCellName, against 5c41f7f) -- a named non-actor near-ref no longer names the shop
Reviewer: "a non-actor near-ref that has a sensible name (a named activator or marker) no longer contributes its name. That is acceptable under the brief."

### MFO-B537 (FLAGGED WORKAROUND, RESOLVED in 2.8.1 (fix/town-cast-unblock: default OFF with Harbinger ABI 32, automatic below, opt-in key), cast/SubstrateRoad.cpp SubstrateRoadFor, marth ruling 2026-11-01, ClickUp 1dfcuabc4dn) -- the substrate is not requested while the player is in a town
Ruling, verbatim: "Yes, gate in towns". Reasoning: the town wide panic has no located cause in MFO or Harbinger logs, so until the Harbinger fix lands MFO keeps the out-of-combat substrate (ch.27) shut in towns and the cast takes the direct road. This is a workaround, not a solution (CLAUDE.md principle 10). REMOVE `bSubstrateBlockedInTowns` (Config.h, Config.cpp, MFO.ini, INI-REFERENCE) and the gate in `SubstrateRoadFor` when the Harbinger fix is merged. Surfaced at edit time from MAP.md section 2 "cast/SubstrateRoad.cpp + apmf/Substrate.cpp".

### MFO-B538 (SEV-4, OPEN, Progression.cpp BuildPerkTwins, against debea50, Opus review of feat/perk-gate-fix) -- unnamed perks can twin
Reviewer: "NameOf returns \"<unnamed>\" and never \"\", so both !nm.empty() guards are dead code. Unnamed perks all land in one name bucket, so they can still twin if their condition lists are identical. LoreRim has no such case today. Compare against the raw GetName() instead."

### MFO-B539 (SEV-5, OPEN, Progression.cpp / PerkGate.cpp twins + world-progress, against debea50) -- first twin wins silently, no Pass 2b edge to the twin, the (f) subject swap changes no result
Reviewer: "When several in-tree candidates share a name and conditions, the first one wins with no log line." "Pass 2b draws no graph edge to the twin." "The (f) subject swap does nothing. All seven functions read quest, global or player-stat state whatever the subject."

### MFO-B540 (SEV-5, OPEN, Board.cpp away rows, against 7e9c613, Opus review of feat/mfo-town-ui) -- a just-dismissed follower can show Away until Errand::Sweep runs
Reviewer: "after a dismissal Describe can stay non-empty until Errand::Sweep runs, so the row can show away for that short gap."

### MFO-B541 (SEV-4, OPEN, progression/Poll.cpp:144-146, against 3569203, Opus review of feat/mfo-leveling-errand-in-party) -- the two-probe party read can race a roster publish
Reviewer: "Poll.cpp:144-146 reads IsTrackedFast and IsTransientDropFast under two separate g_mx acquisitions ... the poll then sees tracked=false from the old mirror and transient=false from the new one, so a level-up in that one tick is banked as benched. It needs a level-up on the same tick, so it is rare. Fix: one combined IsInPartyFast that reads both sets under a single lock."

### MFO-B542 (SEV-4, OPEN, progression/Poll.cpp, against 3569203) -- the gap between an errand ending and the roster re-adding still reads as benched
Reviewer: "Between Sim/Errand dropping the record and the next Refresh re-adding him, he reads as benched. The cost is real when it hits: the active branch then gives min(d, lag) with d=0, so the bench-rate loss is never refunded. It needs a level-up inside about one Refresh period." Also SEV-5: "an Auto-class errand follower is not in g_resolvedClass ... so RecomputeHMS returns at \"noclass\". He gets the level but no HMS on that level-up, the same as a benched follower did before."

### MFO-B543 (SEV-4, OPEN, progression/Hms.cpp cap, against 8ce9ca8..711d406, Opus tier-3 reviews of feat/mfo-hms-fix) -- the cap trims against the pure class profile, not the skewed one
Reviewer: "The cap trims against the pure class profile, not the skewed one (eprof / hmsSkew). The sweep therefore cuts the off-class pool first and wipes the skew that ruling B builds. In normal play a follower at P gains at most what the player gained, so the trim rarely fires after the sweep."

### MFO-B544 (SEV-4, OPEN, progression/Hms.cpp / Poll.cpp cap timing, against 8ce9ca8..711d406) -- a benched follower is swept only when it returns, and the level-up sample can trail one attribute pick
Reviewer: "A benched follower above the cap keeps his total until he returns, and a record adopted this poll is capped on the next one." "the sample is taken on the GetLevel edge. If a list raises the player's level before the +10 attribute pick, the cap trails by one pick for a whole level, and for a leveling follower that cut stays in W."

### MFO-B545 (SEV-5, OPEN, progression/Hms.cpp, against 8ce9ca8..711d406) -- fractional base AVs, the unreadable path hides the capped line, an older build floors stats back up
Reviewer: "The fixed-stat cut writes fractional base AVs, while the grant path writes whole points." "The \"unreadable\" error path sets hmsCapLogged, which hides that session's \"adopted total capped\" line." "older builds put the baseline floor back, so stats rise ... This is a stat change only, with no format or co-save corruption."

### MFO-B546 (SEV-4, OPEN, cast/Fire.cpp s_notInCombatSince, against 8ed3d9f, Opus review of feat/mfo-engage-fights) -- the not-in-combat timer is never cleared on Forget or ResetAllState
Reviewer: "s_notInCombatSince is never cleared on Forget or ResetAllState. An entry left over from an earlier flap can skip the 1 s debounce, though it is mostly masked because a new fight's first command is Changed."

### MFO-B547 (SEV-4, OPEN, EngageOnSight.cpp, against 8ed3d9f) -- an entry filed before ToldToWait or the Wait rule turns on is not released by that gate (pre-existing)
Reviewer: "an entry filed before ToldToWait or the Wait rule turns on is not released by that gate. That predates this branch."

### MFO-B548 (SEV-4, OPEN, cast/Fire.cpp transparent NoOp, against 6a6bd24) -- a stale pinned X can still trade places with a live Y once per debounce period
Author: "a stale X can still trade places with a live Y once per debounce period, until the stale-entry release or Harbinger drops X. I did not close it." Note for marth (reviewer SEV-5): a retreating follower (RetreatPhase not None) is not pulled into the party fight, since engaging mid-retreat would undo the retreat.

### MFO-B549 (SEV-4, FIXED 9cf3986, logistics/ErrandRest.cpp ShopDoorLockedNow / PostShopDoorRead, against ea67e43, Opus round-2 review of fix/errand-closed-shop) -- a read that starts before the request can stall the door gate for up to 30 s
Verbatim: "In PostShopDoorRead the stamp is taken at the start of the lambda on the main thread. If a planning read is already running at the moment the worker records the request, its write lands after the erase, with a stamp older than the request. The next ask finds an entry that is done but too old. That fails the verdict test and also the it == end test, so nothing is posted. ... Every ask returns Pending until the request is 30 s old." Reasoning: self-recovering, narrow window, no crime results. Fix: when the entry is done but older than the request, post again (else post = true), or stamp the read when it finishes.

### MFO-B550 (SEV-4, FIXED 9cf3986, logistics/ErrandRest.cpp FollowerSideDoorLocked, against ea67e43) -- the player-indoors fallback sees only the follower's own cell
Verbatim: "It reads only load doors within kScan (4096) in his own cell that lead directly to the place cell. With the player indoors, a door in the next exterior cell, or a shop reached through an inner interior, gives false. That falls through to the old retries and placement. The OpenAt hours gate still applies." Reasoning: narrowed from the round-1 SEV-4. A script-locked door during open hours can still be placed past while the player is indoors.

### MFO-B551 (SEV-5, FIXED 9cf3986, logistics/Errand.cpp + ErrandRest.cpp, against 0df3d7b..ea67e43) -- small leftovers from the closed-shop review
Verbatim: "A door that locks inside the 30 s window goes unseen" (mostly covered by the OpenAt hours gate in tryPlace). "The depth-2 check stops at the first matching inner door." "In closedDoorGate the break only leaves the inner loop, so the last matching town wins." "closedDoorGate and the hours gate in tryPlace ignore stopOver's return" (one service tick). "Inn and temple stops park g_parked[r.faction], where r.faction may be 0 or the inn faction." "Future Home stop: the generic branch would close the player's own home (kRequiresKey), so a player-owned exemption will be needed when that lands."

### MFO-B552 (SEV-4, FIXED 9cf3986, logistics/ErrandRest.cpp TownWalkBarredByLock / FollowerSideDoorLocked, against 8b659e0, Opus review of fix/town-walk-locked-doors) -- no exemption for the player's own doors
Verbatim: "TownWalkBarredByLock treats any locked door the same way. A player-home door stays kRequiresKey after purchase, and a teammate walking into the player's own house is not a crime. So when the player is inside his home and the follower is outside, a loot target in the house is refused." Reasoning: a false refusal with no crime risk. Fix: skip a locked door whose ref owner or cell owner is the player (0x7 / 0x14) or PlayerFaction (0xDB1), inside the shared predicate, so the future errand Home stop (MFO-B551) gets it too.

### MFO-B553 (SEV-4, FIXED 9cf3986, same predicate, against 8b659e0) -- loot and LOTD walks see only own-cell doors (the MFO-B550 limit)
Verbatim: "If the follower stands in an exterior cell and the door into the target's interior is in the next exterior cell, the predicate returns false and the walk is still declared. That can reproduce the original BLOCKED-at-a-locked-door case." Reasoning: only when a loaded target sits in another interior (player inside, follower outside). Fix with MFO-B550.

### MFO-B554 (SEV-5, FIXED 9cf3986, LootScan.cpp gates + TownWalkBarredByLock, against 8b659e0) -- one door scan per candidate, and the log reset depends on a later call
Verbatim: "Every candidate that reaches the gate and stands in another interior cell triggers a full own-cell door scan (up to 96 handles)." Caching per (cell, tick) removes it. "s_logged is cleared only by a call made while the player is outside a town." Reasoning: bounded by MarkTravelFailed cooldown, cleanup only.

### MFO-B555 (SEV-5, OPEN, logistics/ErrandRest.cpp door predicates, against 9cf3986, Opus review of fix/door-backlog-b549) -- leftovers after the B549..B554 drain
Verbatim: "either side being player-owned exempts the whole pair" (tighten to the side the walk enters). "the 1 s cache shares an answer across followers in one cell" (two followers more than 4096 u apart can differ for up to 1 s). "The per-visit log set only resets through NoteTown ... With town errands turned off, g_twLogged never resets on a town change." "Loot and LOTD cells read through ShopDoorFor now log as [errand] shop place cell X" (wording only). Reasoning: no crime path, logs and false refusals only.

### MFO-B556 (SEV-4, OPEN, logistics/Errand.cpp StartReopenedTrip + ErrandSim.cpp ShopLockGate, against 4a81706, Opus review of fix/errand-solitude) -- with the player indoors the reopen and sim lock gates let the stop through
Verbatim: "In C, ShopDoorFor returns None when the player is indoors, and in D, ShopDoorLockedNow returns Unknown ... The real cost is one BLOCKED walk at the door, not a crime-free guarantee from the start." "Adding the FollowerSideDoorLocked fallback in C would close the gap for live trips." Reasoning: the physical walk is still covered by closedDoorGate (follower-side fallback) and the tryPlace gate, and off screen no door is activated.

### MFO-B557 (SEV-5, OPEN, same files, against 4a81706..340fb7a) -- leftovers from the Solitude review
Verbatim: "when every shop in a C or REST_SIM plan is dropped, the same plan is re-built on every poll" (cheap, NoteVendorOnce stops the log spam). "the off-screen gates (D) do not park the faction" (harmless). Author note on 340fb7a: the dropped-stop count is appended to the STARTED line, but the stop names in the original line may still list the dropped shop. Identity match by FormID across vendor key, faction and vendor actor: a dropped shop sharing an id with a kept stop keeps its entries.

### MFO-B558 (SEV-4, OPEN, logistics/Gear.cpp ComputeWeaponRoles + keep/sale sets, against 5b100b1, Opus review of fix/b250-base-skill) -- a once-per-load 1H/2H flip on an exact base tie
Verbatim: "Before StyleVotesReady, the !ready branch already uses bestMeleeSkill from the perk-less tie, which comes out OneHand. If his 2H ranks lead, the class flips to TwoHand once the tally lands. In that window EquipAuthority and the wardrobe can equip one class and then the other, and the keep and sale sets (ComputeKeepSet, the Economy off-role sale) see a 2H weapon as off-role." Fix direction: "make the keep set and the sale treat both melee classes as in-role while a tied pick is not ready." Reasoning: needs a vendor sale inside the first seconds after load to lose anything.

### MFO-B559 (SEV-4, OPEN, cast/EquipWeigh.cpp ServicePreference ~459, raised against 943efcf) -- D4 classifies only the FIRST missing entry
Opus tier-A review of the ABI v33 batch, finding F4 (as relayed by the coordinator): "D4 classifies only the first missing entry, cast/EquipWeigh.cpp ~459." With the per-entry read (MFO-B262 fixed) the missing entry is classified by its own state, but `missForm` / `missSlot` is still the first declared entry not in its hand. When that first one reads Declared / ZeroScore / HandHeld (not a scored miss) and a later missing entry reads Lifted / Leading, D4 does not escalate for the later one. Fix shape: walk every missing entry and run the clock on the first one whose own state is a scored miss. Deferred (SEV-4).

### MFO-B560 (SEV-4, OPEN, Board.cpp WndProcHook WM_CHAR, against 01c1cc3, Opus review of fix/wndproc-callwindowproc) -- ANSI WM_CHAR fed to AddInputCharacterUTF16 (pre-existing)
Verbatim: "SetWindowLongPtrA also makes the window ANSI. From then on our thunk, and every proc below it, gets ANSI WM_CHAR. Characters outside the system ANSI code page arrive as '?'. On a CJK code page they arrive as split lead and trail bytes. So AddInputCharacterUTF16(wParam) is wrong for non-ASCII." Fix: "keep the A subclass, and in the WM_CHAR branch convert ANSI to UTF-16 with MultiByteToWideChar(CP_ACP). Buffer a lead byte when IsDBCSLeadByte says so, then convert the pair." Switching to the W API is a marth decision: it would expose later direct-call subclassers (the same crash pattern) to MFO's W proc. Also: MEO and MAO carry the same direct-call `return func(...)` (out of MFO scope, reported to marth).

### MFO-B561 (SEV-4, RESOLVED by marth 2026-11-09: Thief 0.20 and Master Thief 0.20 encoded, Templar no cap with a C8 TODO for a vs-undead power floor, classes/ClassTable.cpp ch.18 column, against bc39bee, Opus review of feat/classes-c1-c2) -- draft ch.18 caps not encoded where the walkthrough is silent
Verbatim (reviewer, paraphrased by the coordinator): the draft package gave Thief 0.2, Master Thief 0.2 and Templar 0.25 power-share caps; the table leaves them unset because marth's walkthrough rulings do not state them. Reasoning: a design question, not a defect. Ask marth before C8 encodes them.

### MFO-B562 (SEV-4, OPEN, classes/ClassTable.cpp, against bc39bee) -- C2's CSTY-ids-per-stance column and the ch.18 governed-class bits are not in the table
Verbatim (reviewer): C2 as written lists CSTY ids per stance, and ch.18 needs governed-class bits (Bash / DualWield / Left); neither is in `ClassDef`. Reasoning: both belong with the generator and the packages, so C8 owns them (card amendment: C8 adds the CSTY-ids column and the ch.18 governed-class bits to the table). Nothing reads them before C8.

### MFO-B563 (SEV-5, OPEN, classes/ClassTable.cpp flags, against bc39bee) -- three package-policy leftovers
Verbatim (reviewer): Ebony Warrior has kDisengage without kSpellRange. Saint has kNearPlayer while the ruling says it "leads vs undead". T1 default gambit sets exist while the ruling says "T1 behaves vanilla". Reasoning: all three are policy for C8 (packages) and C12 (defaults), no behaviour today.

### MFO-B567 (SEV-4, OPEN, classes/State.cpp kept MCLS record, against 2a035b1) -- stale FormIDs in a kept record
Verbatim (reviewer, F-3): a record kept verbatim by an older DLL carries the FormIDs of the save it came from. Downgrade -> load-order change -> upgrade, and the newer DLL resolves them against the wrong plugin table. Reasoning: only a parse could re-resolve them and the older DLL cannot parse; the exposure is the stored gambit rows' actionParamForm / subjectActorForm. C12's restore must type-check actionParamForm (a spell for a cast row, and so on) the way the live evaluator does, never trust it.

### MFO-B568 (SEV-4, CLOSED by card C12 / MCLS v2 `feat/mcls-v2` 2026-11-11, classes/State.cpp, against 2a035b1) -- only the COMBAT table is stored per class
Resolution: marth Q29 ruled that the per-class memory stores BOTH tables. MCLS v2 appends one stored LOGISTICS table per class under `a_version >= 2` (plus the classes-OFF tables and Dragonborn's picked T4); no v1 field changed. Proof: `tools/mcls_byte_test/run.sh`.
Verbatim (reviewer, F-4): Q8 says "remember each classes setup"; MCLS v1 stores the combat table only. Reasoning: whether a class's setup includes the logistics table is marth's call; if yes it is a v2 APPEND (a per-table kind byte or a second table list under `if (a_version >= 2)`), no v1 change.

### MFO-B569 (SEV-5, CLOSED by card G9 `feat/g9-rule-cooldown` 2026-11-09, classes/State.cpp rowSchema, against 2a035b1) -- rowSchema is stamped from kSchemaVersion
Resolution: `GambitWire::kRowSchema` (Serialization.h) = the FLWR version at which the gambit ROW last changed (6, the G9 cooldown), in FLWR's own version numbers. MCLS stamps it and reads 1..kRowSchema; a FLWR bump that does not change the row leaves it alone. Every MCLS written before (rowSchema 5) reads unchanged.
Verbatim (reviewer, F-5): MCLS's rowSchema is the FLWR record version, so ANY FLWR bump (even one that does not touch the row) makes an older DLL keep MCLS verbatim and freeze class progress. Reasoning: harmless today; note for card G9 (FLWR v6): give the gambit ROW its own encoding version (GambitWire) and stamp that instead.

### MFO-B570 (SEV-5, OPEN, GambitWire ReadRow in MCLS, against 2a035b1) -- unresolved actionParamForm rows disabled while the ESL is absent
Verbatim (reviewer, F-6): a stored class row whose actionParamForm does not resolve is disabled and zeroed, FLWR's rule, also while the class ESL is absent. Reasoning: today no class row targets an ESL form, so nothing is lost; revisit if MFO_Classes.esl ever ships gambit-targetable forms (they would be zeroed by an ESL-absent session).

### MFO-B571 (SEV-5, OPEN, card C4, against 2a035b1) -- XP addition must saturate
Verbatim (reviewer, F-8): ClassProgress::xp is u32 with no overflow guard in the store. Reasoning: C4's accrual must use saturating addition (clamp at UINT32_MAX), never a wrapping add.

### MFO-B564 (SEV-4, OPEN, native/selftest/Fixtures.cpp PostFixtureCleanup, against a1c0e6f, Opus tier-3 review of feat/mfo-dev-selftest, F5) -- a taught fixture spell is removed even if it was learned for real during the run
Coordinator's paraphrase of the finding (not the reviewer's verbatim text): "a taught spell is removed even if it was learned for real during the run." The ledger records "taught" only when the follower lacked the spell at the add, and the cleanup cannot tell a later real learning (player teach, spell book) from the fixture. Dev builds only; deferred under rule 9 (SEV-4, not co-save / threading / ABI, not exercised by the next field cycle's release DLL).
ID COLLISION (F10, SEV-4, Opus round-2 review against fb4a866): MFO-B564 is also used by the parked `fix/mfo-bound-bows` branch. Whichever of the two merges SECOND renumbers its entry (and its references) to the next free id.

### MFO-B565 (SEV-4, OPEN, native/Evaluator.cpp + a release build's Fire fall-through, against a1c0e6f, same review, F6) -- the marker's WHEN cell is evaluated on release builds
Coordinator's paraphrase of the finding (not the reviewer's verbatim text): "the WHEN cell is evaluated on release builds (harmless transparent fall-through, uses a combat slot)." Only a dev build skips the marker; a release build evaluates its condition and, when it holds, takes Fire's transparent "unknown action" fall-through each scan. Harmless by the save-compat proof (Docs/SELFTEST.md section 6, which now states it). Deferred under rule 9.

### MFO-B566 (SEV-5, OPEN, native/selftest/, against a1c0e6f, same review) -- leftovers
Coordinator's paraphrase of the finding (not the reviewer's verbatim text): "spdlog under g_mx" (Runner.cpp ApplyResetIni and Record.cpp CoLoad's commit log while holding the leaf lock; harmless today, spdlog takes no lock the worker or a fixture closure holds, but the leaf-lock rule says no call under it) and "an unresolvable-follower orphan blocks starts with a 30 s error" (a load-orphan whose follower does not resolve keeps blocking every test start and logs an error every 30 s). F7 (the doc's "the marker's param is 0") and F8 (the misplaced CMake comment) were fixed in the review round, not deferred.

### MFO-B572 (SEV-5, OPEN, native/progression/Hms.cpp ReadHmsCapView + the cap block in RecomputeHMS, against fb4a866, Opus round-2 review of feat/mfo-dev-selftest) -- the per-pool floor 50 is mirrored by hand
Coordinator's paraphrase of the finding (not the reviewer's verbatim text): the dev-only `ReadHmsCapView` repeats the cap's per-pool floor `50.0f` instead of sharing it; a shared constexpr (e.g. lifting `kHmsCapPoolFloor` to namespace scope in Hms.cpp) would keep the self-test's floor test from drifting if the cap's floor changes. Deferred under rule 9 (SEV-5; the release cap is unchanged).
### MFO-B580 (SEV-3 per the reviewer; RESOLVED for the Auto fan-out on feat/auto-control-hostile: `HostileAutoFoes` calls `WillWork`, a No drops the foe, Unknown keeps it; act.cast_player remains unchecked) (was SEV-4, OPEN, cast/Fire.cpp G10 wiring, against 48be8b3) -- WillWork is wired only on act.cast_target
Verbatim (reviewer, relayed): wiring covers only kActCastTarget. Reasoning: the Auto fan-out (Rally is level-capped and not in the nature-undeclared set; mod-flagged hostile control spells) and act.cast_player for condition-gated spells are not checked. Wiring Auto needs cast/Auto.cpp and cast/Archetype.cpp, which feat/auto-control-hostile is changing (shared nature classifier), so the narrowing is recorded, not done. (ID chosen above origin/main's B572 to avoid a collision; renumber on merge.)

### MFO-B581 (SEV-4, OPEN, cast/WillWork.cpp, against 48be8b3) -- time-varying CTDAs cache a false No
Verbatim (reviewer): time-varying CTDAs cache a false No for the refresh window. Reasoning: the mirror trusts a verdict 6 s and refreshes at 2 s.

### MFO-B582 (SEV-4, OPEN, cast/WillWork.cpp, against 48be8b3) -- a magnitude-0 / kNoMagnitude capped effect gives a false No
Verbatim (reviewer): a magnitude-0 or kNoMagnitude capped effect gives a false No. Reasoning: the cap is read from effectItem.magnitude unconditionally.

### MFO-B583 (SEV-4, OPEN, cast/WillWork.cpp, against 48be8b3) -- the cap misses dual-cast scale, kPowerAffectsMagnitude and the target's EP41
Verbatim (reviewer): the magnitude misses the dual-cast scale, kPowerAffectsMagnitude, and the target's EP41 incoming-magnitude entry point.

### MFO-B584 (SEV-5, OPEN, cast/WillWork.cpp, against 48be8b3) -- kReanimate is unreachable
Verbatim (reviewer): kReanimate is unreachable, the IsSummonSpell branch in Fire.cpp returns first.
- NOTE 2026-11-11 (feat/g7-reanimate, card G7): `PickCorpse` (cast/Corpse.cpp) and Fire's cast-at-target check now call WillWork with a corpse as the target, so the kReanimate cap is reached on that road. Left OPEN for the reviewer's original point until a field run shows a `[willwork]` line for a Reanimate spell.

### MFO-B585 (SEV-5, OPEN, cast/WillWork.cpp, against 48be8b3) -- the engine's MGEF condition site was not located
Verbatim (reviewer): the engine's MGEF condition site was not located; the IsTrue(target, caster) order was confirmed from ActiveEffect::EvaluateConditions and vanilla data.

### MFO-B600 -- auto-control-hostile review leftovers (raised against bbe0fd6, Opus review)
- **SEV-4:** AUTO re-picks an already-controlled foe. `ShouldApplyTo` (cast/Auto.cpp:271-281) returns true for any hostile spell with no damage component (`dotRate <= 0` -> always recast), so a second foe never gets controlled while the first is still held.
- **SEV-5:** the `[classify]` log (CasterConsent.cpp ClassifySpell) fires per effect in record order, so a mixed spell can log "unflagged" for an effect that sits before a flagged hostile one. Use a hostile-first pass.
- **SEV-5:** the log's dedup is keyed on FormID (dynamic FF spells recycle ids) and is never cleared on load.
- **SEV-5:** MFO's control set (Calm, Frenzy, Demoralize, Paralysis, TurnUndead; no Stagger) differs from Harbinger's self-flip refusal set (which includes Stagger). Document it.
- **SEV-5:** the ClassifySpell comment names IAUS as a reader before any IAUS code exists.
- **SEV-5:** a theoretical `std::bad_alloc` inside the set insert in the combat-thread hook.

Round-2 additions (Opus review of cf567a3, coordinator's paraphrase, not verbatim):
- SEV-4: MFO's NoRow is wider than truly rowless. kRows has no Absorb / DualValueModifier / PeakValueModifier / AccumulateMagnitude rows, though the engine files those under ValueModifier rows (APMF STATUS.md:690). A fire-and-forget hostile Absorb/DVM spell is predicted NoRow, so a transient 4 s no-start now trips the never-fired release and holds it off for the fight (loud WARN, transparent). Fix: mirror Harbinger's alias rows in Archetype.cpp or exclude those archetypes from the NoRow arm.
- SEV-4: the WillWork mirror (256 entries) can thrash in big fights now that AUTO adds followers x gated spells x foes. At the cap the sweep falls back to g_cache.clear(), so every triple reads Unknown (fires unchecked) and re-posts. Never a wrong No. Fix: kMaxEntries 1024 or evict-oldest; PostMeasure's insert also ignores the cap.
- SEV-5: CastOn.cpp:593 comment, the :621 WARN and the :398/:636 reasons plus MAP.md:773-775 still say "pure-Script" for a release that now also covers NoRow.
- SEV-5: Auto.cpp:666 "no enemies in range" also covers "every foe will-not-work".

### MFO-B610 -- follower spell cost parity leftovers (ClickUp 1dfcuabcr0c, redesigned 2026-11-09 to charge the player-curve cost; carried from the perk design ae6c6bb)
- **SEV-5:** a negative Base GMST gave NaN (FIXED: explicit error + refusal). The perk-design items (marker ability, epoch guard, ex-follower sweep) are gone with the perk.
- (The earlier "SEV-5: HandleEntryPoint and CalculateMagickaCost run on the worker" line was FALSE and is withdrawn: review of c0f6b1a, F1, made the animated roads post to the main thread, see MAP.md. The direct roads already run on main.)
- The gate, dual-cast and multi-school items moved to MFO-B612 / MFO-B614.

### MFO-B612 (SEV-4, OPEN, CostParity gates, against c0f6b1a, Opus review F4) -- the affordability gates read the engine cost
Verbatim (reviewer, relayed by the coordinator): F4 SEV-4: affordability gates read engine cost E (Auto.cpp:860, Summon.cpp:426, CastOn.cpp:284, Service.cpp:1915/2006, DirectSelf.cpp:370, DirectTarget.cpp:380). With a pool between E and P the cast passes and the clamped deduct makes it cheaper than P. Bounded to the last cast of each pool, and inherent on the animated road.

### MFO-B613 (SEV-4, OPEN, CostParity dedupe stamp, against c0f6b1a, Opus review F5) -- the stamp is written on every Cost() call
Verbatim (reviewer, relayed by the coordinator): F5 SEV-4: the dedupe stamp is written on every Cost() call even with no cast (Summon.cpp:363 stamps inside the affordability check, then refuses at :364). Field logs show direct CastSpellImmediate raises NO TESSpellCastEvent (16 direct casts, 0 sink lines), so the 3 s window only swallows genuine animated recasts (undercharge).
Reviewer's remedy (from the original F5 text): since direct casts raise no sink event, the window guards nothing real and could be cut to well under a second, or the stamp could move to after the actual deduct.

### MFO-B614 (SEV-4, OPEN, CostParity dual cast / multi-school, against c0f6b1a, Opus review F6)
Verbatim (reviewer, relayed by the coordinator): F6 SEV-4: dual cast is topped up on the single-cast delta, and a multi-school autocalc spell uses the costliest effect's school while the engine curves each effect. Reviewer's reasoning: both only shift the approximation (CalculateCost's per-effect loop calls 0x1401434D0).

### MFO-B615 (SEV-5, OPEN, CostParity comments / publication, against c0f6b1a, Opus review F8)
Verbatim (reviewer, relayed by the coordinator): F8 SEV-5: the comments at CostParity.cpp:40 / CostParity.h:28-29 say the sink event follows a direct cast "ms apart", but the field shows it never fires. g_pc/g_np are published before a relaxed Active() load. LogDue(Key^1u) collisions. Active() ignores an INI re-read. Also kAddRangeToValue perks (random) break the affine probe.

### MFO-B616 (SEV-4, OPEN, CostParity channel close, against 1dc1c9b, Opus re-review N1) -- the Tick job can close a channel that was just reopened
Verbatim (reviewer): The erase at :301-304 removes by (caster, spell) on a single sample where `Channelling()` is false. If the follower recasts the same concentration spell between the worker posting and the main job running, the record `OnObservedCast` just reset (:262) is erased. The same happens if the new channel is still in state 4 (Casting), before it reaches state 6. Field data shows up to ~300 ms in state 4. Effect: that whole channel goes unbilled. It is an undercharge only, so it is not a correctness or safety issue. The window existed before the fix; the post widens it slightly. Fix later: a generation counter or `paidThrough` check before erasing, or close only after two consecutive misses.

### MFO-B617 (SEV-4, OPEN, include rule, against 1dc1c9b, Opus re-review N2) -- logistics includes a cast/ internal header
Verbatim (reviewer): logistics/Service.cpp:14 includes `cast/CostParity.h`. CLAUDE.md says another subsystem may include only that subsystem's one public header, and no other non-cast file includes any cast/ header except `cast/Actuation.h`. The root files (Followers, Diagnostics, plugin) are not subsystems yet, so only Service.cpp breaks the rule. Fix: re-export through `cast/Actuation.h`, or record an explicit exception in MAP.md.

### MFO-B618 (SEV-5, OPEN, CostParity leftovers, against 1dc1c9b, Opus re-review N3)
Verbatim (reviewer): The `Deduct` comment at :108 still says "WORKER"; it now runs only on main. `BillOnMain` re-checks tracking but not `mfoEnabled` (the worker checked that at observe time, :248). A post-sized window, negligible.

### MFO-B601 (SEV-4/5, OPEN, G9 per-rule cooldown, against 9b338f1, Opus tier-3 review) -- review leftovers
Coordinator's paraphrase of the findings (not the reviewer's verbatim text):
- C1 (SEV-4, co-save, field): the v5->v6 load path has no executed proof (the MCLS "rows v5" self-test runs only with bClassCoSaveSelfTest=1; CI never runs it). Deck: load a real v5 save with the switch on, expect "MCLS self-test (rows v5, pre-G9): PASS" and an FLWR summary matching the previous session; then save + reload (v6->v6). On the in-game test list.
- C2 (SEV-4, co-save, release): every v6 save is unreadable to a v5 DLL even with no cooldown set, and that DLL wipes every board on its next save. The release carrying G9 must say so plainly (CHANGELOG + Nexus). Optional design alternative saved for marth (question 39): write FLWR v5 while every row's cooldown is 0.
- B1 (SEV-5): a cooling EQUIP row would not set equipHeld (force-hold releases while cooling); only matters once CooldownExposed widens or C12/IAUS write cooldowns to other rows.
- B2 (SEV-5): a non-zero cooldown on a row the board does not expose is enforced but invisible; C12 must limit itself to power-attack rows or widen CooldownExposed in the same change.
- B3 (SEV-5): the "last: cooling down (Ns)" tooltip goes stale after expiry when the rule stops matching (display only, like G10's).
- B4 (SEV-4, rule-mandated): Board_FieldKit.cpp is 1528 lines -- plan its split as its own round. MAP.md's Serialization.h:289 anchor is off by three (kSchemaVersion is at :292).

### MFO-B602 (SEV-5, OPEN, apmf/Bridge.cpp + cast/CastOn.cpp CanServe adoption, against 6bebdb4, Opus tier-3 review) -- leftovers
Coordinator's paraphrase of the findings (not the reviewer's verbatim text):
- Buff route with no recipient: the gate queries target 0 where BuffClaim files the caster in the explicitSelf case; the verdict cannot differ today. Derive the target with ExplicitSelfBuff(spell) for exactness.
- The composed-heal gate re-derives Try's target / dual / concentration (matches ComposedCast.cpp:358-709 today); expose one shared claim-argument helper from ComposedCast. The gate also runs before Try's Enabled() check (both outcomes are a transparent no-cast).
- g_lastServeRefusal is never cleared (its sibling is erased at CastOn.cpp:1844 and cleared at :1863); add it to both sites.
- Comment at CastOn.cpp:64 names a nonexistent LogServeRefusal (the function is HarbingerWouldRefuse).
- MFO-B2's open list should also name: a claim Harbinger accepts while MFO's cast-facet priority is 0 (offFacets) still displaces the incumbent; and the kServePending_Arbitration bit.
- Note: CastOn.cpp is 1876 lines (plan its split as its own round); Bridge.cpp 1373.

### MFO-B611 (SEV-4/5, OPEN, progression/PerkGate.cpp SetLivePerkRank, against 8bbb53d, Opus tier-3 review) -- review leftovers
Coordinator's paraphrase (not verbatim):
- SEV-4: removal takes EVERY live copy of an entry pointer and apply is skipped while any copy is live, so another source holding the same perk (a magic effect's perk-to-apply, a temporary perk) can lose or lend its copy; recovers at the next Load3D. Rare (only catalog-tree perks are mutated).
- SEV-4 (pre-existing, not a regression): an engine task 0x5F queued by FinishLoadGame/Load3D before MFO strips or respecs perk P can re-apply P after the removal until the next Load3D.
- SEV-5: the self-test asserts "no duplicates" only; add a removal-leaves-0-copies and missing==0 check (regression test for the never-removed-live bug).
- SEV-5: ProgProbe.cpp:409/:471 (dev bProgProbe, default OFF) still call ApplyPerksFromBase and duplicate when armed; never read a probe run as evidence.
- SEV-5: the post-change event 0x391760 has no in-binary subscriber; a raw-address plugin listener would no longer see MFO's changes.
- Unverified by the reviewer: the 1.7.104 addresses (author's only).

### MFO-B619 (SEV-4, CLOSED by card C6 `feat/classes-c6-carriers`: the three marker carrier SPELs 0xA80-0xA82 ship with the same casting type and delivery as their MGEFs; no passive tests a marker yet, H-MARK will, MFO_GenerateESP.py make_classes_esl / MFO_Classes.esl, against ee36630d, Opus review of C7; owner C6 / H-MARK)
- The marker MGEFs (MFOC_Marked, MFOC_Flanked, MFOC_Guarded) have no carrier SPEL yet, so `HasMagicEffectKeyword` on the reference perk (and any C6 passive that tests a marker) cannot be satisfied until C6 or H-MARK adds a SPEL that carries them. The SPEL must use the same casting type and delivery as its MGEF (Marked FF + Contact, Guarded Concentration + Aimed, Flanked FF + Target Actor).

### MFO-B620 (SEV-5, OPEN, MFO_GenerateESP.py class add-on block, against ee36630d, Opus review of C7)
- The consistency checks in `load_class_table` / `make_class_styles` are `assert` statements, which `python -O` strips; use explicit raises.
- The `kRoot*` / `kDisSpell` regexes raise AttributeError (not a named error) when ClassTable.cpp changes shape.
- The manifest FLST leaves out MFOC_Version (0x820); the marker MGEFs, the flag factions (MFOC_OffClassWeapon, MFOC_MarkPending) and the reference perk are reachable only by local id.
- The TES4 HEDR record count is the house constant 100, not the real count.

### MFO-B621 (SEV-5, process notes, against ee36630d, Opus review of C7)
- `tools/audit_esp.py` was edited without being named in the brief (accepted, additive).
- The manual CI dispatch (`gh workflow run native.yml`) compiled an unchanged DLL and proves nothing for a generator-only diff. Do not dispatch CI for a generator-only change again; GitHub's API is rate-limited.

### MFO-B623 (SEV-4, OPEN, selftest b1, against ed548d14, Opus review) -- the heal-left + weapon-right sample only counts
Verbatim (reviewer): The self-test only counts (selftest/Tests.cpp:79-90). The new counter `s.n[4]` (the array is `n[8]`, SelfTest_internal.h:40, so it is in bounds) records heal-left + weapon-right samples but never fails. A real check would fail the step if a right-hand cast claim or lock is live during such a sample (`ClaimLiveOnHand(fid, kHandRight)`, or a right `g_castLock` spell). Field evidence still comes from the log, so this does not need to land in this round.

### MFO-B624 (SEV-5, OPEN, docs + Harbinger nicety, against ed548d14, Opus review)
Verbatim (reviewer): The change reaches three callers the brief and docs do not name. `RightHandIsWeaponHand` -> `WeaponHandExposure` -> `WeaponHandActive` also feeds cast/HealRoad.cpp:475, :891 and :902. The new behaviour there is correct: the heal road's second recipient no longer takes a right hand holding a sword or staff while the left holds a spell, and a standing right-hand heal claim is now released when a weapon appears in the right while the left holds a spell. Worth one line in MAP. Optional SEV-5 nicety for Harbinger: warn when a client's cast claim names a hand that the same client's ch.17 declaration holds with a weapon. That self-conflict is the shape B1a had, and the warning would have surfaced it sooner.
### MFO-B625 (SEV-5, OPEN, Scheduler.cpp ServiceRetreat flee hold, against fix/retreat-hold-until-flee-ends, Opus review) -- review leftovers
Coordinator's paraphrase (not verbatim):
- SEV-5: kFleeNotCombat (10) is the CK-authorable Flee procedure and is correctly excluded from the flee hold; add a one-time info line if one is ever seen as the current package during a retreat.
- SEV-5: the flee hold suppresses the party-OOC table and delays the retreat cooldown for up to 20 s (kRetreatFleeHoldSecs) while the engine Flee wins anyway. By design (marth Q20), noted here.

### MFO-B626 (SEV-5, OPEN, progression/Hms.cpp above-parity gate, against 4e5abfb3, Opus review)
Verbatim (reviewer): (Hms.cpp:506 and :713-740, a behaviour shift the gate makes common) a frozen follower has `budget` = 0, so the skew block is skipped (no decay, no growth) and the battle-window counters (`battlesSinceLevelUp` / `battlesOffClass`, Hms.cpp:737) are never consumed. They then build up over every frozen level. The first award after the player catches up computes off-class usage over a multi-level window. Before, with the cap ON, `applied` > 0 each level, so the window closed every level. The same shape already existed when credit was 0. The fix would be to close the window on a gate-frozen award, which is a design call.
(selftest/Tests.cpp ~:310, ProgAllocator.h:574 HmsCapView) no self-test covers the gate. HmsCapView has no headroom field, and the cap-OFF road (where the gate is the only bound) has no test.
### MFO-B627 (SEV-4, ANSWERED by card C6: `CarrierRanks` never calls Scale01; an equipped class at level 0 counts as level 1, Dragonborn puts every mastered class at its cap; the ESL ladder indexes the level directly. classes/Mastery.cpp Scale01, against 5d22b760, Opus review of C5)
Reviewer's finding (F3), as relayed by the coordinator: Scale01 returns 0 for level 0. Dragonborn sits at level 0 / cap 0, so C6 calling Scale01(level, LevelCap(id)) gives Dragonborn zero effect, and a class equipped via SetEquipped directly stays at level 0. C6's contract must say how Dragonborn and level 0 are handled. Reasoning: Dragonborn has no level by ruling (all mastered passives at full), and SwitchClass/ActivateClass now starts a class at level 1, but a direct SetEquipped does not. See MAP.md section 9 (classes/Mastery.cpp).

### MFO-B628 (SEV-5, OPEN, classes/Classes.h, against 5d22b760, Opus review of C5)
Reviewer's finding, as relayed by the coordinator: any future "reset class progress" must clear the stored unlocked bits explicitly, because IsUnlocked takes stored | computed (unlocks are never revoked). Noted in the Classes.h mastery block (RESET NOTE); no code path resets today.

### MFO-B629 (SEV-4/5, OPEN except R3 which is MOOT, classes C5 fix round, against 8f298c48, Opus re-review)
Verbatim (reviewer):
- R3 MOOT / WITHDRAWN (card C5b, marth 2026-11-10 "Auto is disabled per follower when classes are enabled on that follower"): the C5 class picker (kSkillMap, PickAutoClass, AnySchool tie-break) was removed, so there is nothing left to tie-break. Original text kept below.
- R3 SEV-4 (Mastery.cpp kSkillMap and PickAutoClass): `AnySchool` rows (Mage, Arch-Mage, and the school half of Grand Battlemage) score the follower's best school, so they tie exactly with every single-school row of the same tier and skill count. Example: a Restoration-led follower with both Arch-Mage and Saint unlocked gets Arch-Mage, because both rows score Restoration, are T4 with one skill, and the lower `Id` wins. Same in kind for Grand Battlemage against any same-tier melee hybrid with that school. Suggested fix: make `AnySchool` lose ties, or ask marth for the Arch-Mage row as part of the OPEN-row ruling (Q42).
- R1 SEV-5 (State.cpp:791/812 through `Mutate`'s `g_store[a_fid]`): a write refused because the class is locked still creates an all-default `FollowerClasses` entry, which is then saved. Harmless (default == no entry); a find-first lambda or a no-create Mutate would avoid it.
- R2 SEV-5 (State.cpp:620-637): the load repair refreshes `unlocked` only when it adds a mastered bit, so a record saved with only the unlocked set stale is not refreshed at load. Limited impact: every reader combines stored and computed bits, and no shipped build had the old writer.

### MFO-B630 (SEV-4/5, OPEN, cast/Archetype.cpp C0 census + C1 alias, against 0ca32d72, Opus review)
Verbatim (reviewer):
- SEV-4: the census triggers ClassifySpell's `[classify]` latch (CasterConsent.cpp:60-72). At load it logs one uncapped `[classify]` line per unflagged enemy-control spell, outside the 80-line cap, and uses up each spell's s_ctlLogged entry, so the line never appears when a follower really casts it (a diagnostic loss).
- SEV-4 (BuffRoad.cpp:359, reverse consequence of C1, engine-correct): a Buff-kind 4/5/32/34 effect on Magicka at self (Peak-style Fortify Magicka) now predicts Restore and routes DirectNoRow (UNANIMATED; it was NoRow, then Claim on a capable Harbinger). The prediction is right (the engine builds the Restore caster, which gates on the AV %, so the old claim would rarely fire). But the set of DirectNoRow spells grows, which touches marth's "animations required for all actions" (the same policy already covers archetype-0 Fortify Magicka). And a mixed Armor + Peak Magicka spell now goes rowApprox and is refused by BuffAmbiguousRowWhy (Restore is not in its allowed set).
- SEV-5 labels: (a) Wall = any effect whose impact data spawns a hazard, ranked above AimedAoE/Touch, so a fireball leaving a scorch hazard reads Wall; (b) SelfHazard doesn't require the hazard to be hostile; (c) Script vs ScriptRider by costliest effect falls to the first effect when cost ties or is 0; (d) Archetype.cpp and MAP cite Harbinger CastClassify.cpp:91-98 as the source; they should cite the engine builder 0x81DCA0 / SE 0x7816C0.
### MFO-B631 (SEV-4 and below, OPEN, classes/Carriers.cpp + generator, against 2d4ec4a2, Opus tier-3 review of C6)
Fixed in the review round (not open): SEV-2 `Is3DLoaded` gate removed from the reconcile (Carriers.cpp `Reconcile`; a drop after 600 tries is now a WARN naming the reason); SEV-3 Witchhunter's "casters" facet added to `kMissing`.
Reviewer's findings, verbatim from the coordinator's message:
- SEV-4: the CoLoad newer-save keep path (State.cpp :607/:619) never calls RefreshAllCarriers, so the frozen state is applied half-way.
- SEV-4: Spellsword cost cut not gated on a weapon, though a road exists (GetEquippedItemType on owner tab 0).
- SEV-4: Living Legend x0 ModIncomingDamage is negation, not a block (stagger, enchant and poison still apply), so record it as partial.
- SEV-4: Mutate (State.cpp:498) queues a reconcile on EVERY write incl. XP: once C4 accrues XP each grant costs a main-thread reconcile, so refresh only on equipped/level/mastered/unlocked changes (C4 must fix this before wiring XP).
- SEV-5: GetRandomPercent <= 40*s gives 41% at full (use <).
- SEV-3-field: Scout speed: SpeedMult changes may not apply until movement speed is recomputed (inventory/carry-weight change). Field check first; if it shows, nudge CarryWeight after Add/RemoveSpell.
OPEN, deliberately NOT done (coordinator said stop rather than guess): Warlock / Nightblade "+N levels" have no target tab, so the bonus also lands on ally Illusion buffs (Courage, Rally, Call to Arms). Vanilla Animage restricts by race (ActorTypeAnimal), not by hostility; no vanilla-census-proven "target is hostile to the owner" function exists on the target tab (GetInFaction 71 is proven there only with quest factions, 715 IsUndead and 560 HasKeyword only with keywords), so a PlayerFaction test would be a guess. Needs a ruling or a proven road.
Corrected earlier author note: Sorcerer's summon-duration keyword list DOES cover the Dremora Lord (SummonDremoraLord MGEF 0x0010DDED carries MagicSummonShock 0x2482A).
Author's list, still true: the AddToFaction in-place update and tab i == argument i are now PROVEN by the review (HandleEntryPoint 0x385F30, BGSEntryPointPerkEntry vfunc 0 0x38F010; AddToFaction 0x68B310 on 1.6.1170, 0x5F8430 on 1.5.97); Warlock/Nightblade/Witchhunter add to all non-shout Illusion magnitudes; a T4 with an N-of pool picks the first N mastered T3 parents in Id order (ruling silent).
### MFO-B632 (SEV-4/5, OPEN, progression/Inherit.cpp hidden-perk census C1, against 2e0764a, Opus review)
Numbered B631 because B630 is the latest on main when this was written; renumber at merge if main has moved. Verbatim (reviewer, as relayed by the coordinator):
- SEV-4 script-detection gaps: VM-attached scripts on non-Script MGEFs are not detected; EP 51/52/53/67/69 SelectSpell spells are not walked; the VirtualMachine::attachedScripts road is unverified.
- SEV-4 the helper declarations (ConditionPlayerGated, ReadOwnerFacts, SameConditions, OwnerCtx) live in ProgAllocator_internal.h, whose natural home is Progression.h (layering only).
- SEV-5 RunCensus re-calls HasPerk (unconfirmed is always 0). [Removed in the fix round that followed: the re-call is gone because an effect-applied perk is never a HasPerk hit.]
- SEV-5 MAP still says "FNV signature of the three lists". [Reworded in the fix round.]
- SEV-5 an AddActivateChoice perk shows no-follower-use with no scriptEp field.
- SEV-5 SameCondList duplicates SameConditions (drift risk).

### MFO-B633 (SEV-4/5, OPEN, progression/Inherit.cpp census fix round, against 3e6d1c2, Opus re-check)
Verbatim (reviewer):
- SEV-4: `:inactive` effect perks are counted in `held` and described as "applies it", but the engine has removed them at that moment (34092 at 0x5AE813-0x5AE889: RemoveTemporaryPerk when bit 16 applied and bit 18 kDispelled or bit 15 kInactive is set; apply only when bit 15/16/17 clear). Fix with a summary count of inactive-only perks and a reworded comment.
- SEV-5: The MFO.ini comment still says "whenever the player's perk lists change". It is close enough, but could say "a continuous sweep; full list once per load, then added/removed".

### MFO-B634 (SEV-4/5, OPEN, selftest/Runner.cpp DevClassEquip (dev build only), against 29bc2488, Opus review of C5c)
Reviewer's findings (condensed from the review; full text in scratchpad agentlogs/review-c5c.md):
- F1 SEV-4 (Runner.cpp:153-156): the local id is passed to LookupFormID unmasked and with no range check; an ESL ref written as X.esl|FE012ABC, a local over 0xFFF on a light file, or an ESP ref with its load-order byte resolves to a garbage FormID and only warns "still not a live managed follower after 60 s". Reject local > 0xFFFFFF, and > 0xFFF when the file is light, with the parse WARN.
- F2 SEV-4 (:154/:161): std::stoul does not check the whole token was consumed; do as the level field does (used != size).
- F3 SEV-4 (:248): a full-hex FormID in the 0xFF range or any id IsPersistableID refuses waits forever under the misleading WARN; refuse at parse.
- F4 SEV-4 (:256-266): Dragonborn with :mastered logs " mastered" and "L0" though no mastered bit is set (cap 0); reject at parse or drop from the log.
- F5 SEV-5 (:254): SetUnlocked(GetUnlocked|Bit) takes two g_mx holds; a concurrent unlock (C10 later) could be lost. Not worth a compound writer before C10.
- F6 SEV-5 (:248): mfoEnabled-off followers still get the class (deliberate); note it in SELFTEST.md section 10.
- F7 SEV-5 (:221-230): parsed once per load; an MCM re-read does not re-apply; say so in INI-REFERENCE and MFO.ini.
- F8 SEV-5 (MFO.ini:188-200): the release seed INI carries 13 lines for a key the release DLL ignores (labelled DEV BUILD ONLY; optional move).
- F9 SEV-5 (:240-245): the 60 s WARN timer counts only while Active().

### MFO-B635 (SEV-4/5, OPEN, class pick accessor C2b (Gear.cpp / ClassResolve.cpp / Followers.cpp), against 736c0c3f, Opus review)
Reviewer's findings (verbatim from the coordinator's relay of the review):
- F3 SEV-4: the gambit actuation path (Fire.cpp:441-442 -> cast/Equip.cpp:427 EquipWeapon) still equips a carried off-class weapon on a classes-ON follower (a player-given weapon survives F1); belongs to the gambit-reset/picker card or a guard in EquipWeapon.
- F4 SEV-4: a Dragonborn with a tie or no perks and raw pick Auto behaves fully Auto on every reader (defensible per Q60 "like auto"; recorded as a DECISION).
- F6 SEV-5: PublishActiveMirror loops dismissed records too, and for a Dragonborn-equipped one DragonbornRoot -> StyleVotesFor inserts a mirror entry and posts a main-thread tally every 10 s (bounded, wasteful).
- F2 SEV-3 (round 2, NOT fixed in code, deferred by the author): DragonbornRoot is blind without the Progression catalog: Progression.cpp:1442 (TallyStyleVotes no-catalog path, gated on bMeoAwareGems + MEO GemThemeSupported) leaves skillRanks zero. Widening that gate to Classes::Active() would also change the tie-breaks that read skillRanks (Cast.cpp:175 school pick, Gear.cpp:474 1H vs 2H) on classes-Active saves, so it was NOT done; DragonbornRoot logs a once-per-session WARN instead. Needs marth's call (widen the gate and accept those tie-break changes, or give the Dragonborn root its own tally).

### MFO-B637 (SEV-5, OPEN, selftest/Setup.cpp RestoreClass (dev build only), against 1a3002f, Opus review of self-test v2)
Raised against 1a3002f: when the follower had NO MCLS record before a class run, `RestoreClass` writes the all-zero snapshot through the State.cpp writers, which creates an all-zero MCLS record (equipped none, no progress). Behaviourally equivalent to "no record" for every reader, but the record now exists and is saved. No writer exists to delete a record; a dev-save-only effect.

### MFO-B638 (SEV-5, OPEN, selftest/Fixtures.cpp + Runner.cpp (dev build only), against 1a3002f, Opus review of self-test v2)
Raised against 1a3002f: (a) a `hurt` fixture whose `added` (damage x10) exceeds 100000 is DROPPED on load by the shared `added > 100000` bound, so a follower with a huge Health pool loses its give-back entry (the damage then heals by itself); (b) a fixture ack that arrives after its run was finished (late ack) leaves a stale `g_acks` / `g_spawnRef` entry keyed by the dead token until the next ClearAll (bounded, no behavioural effect).

### MFO-B639 (SEV-4, OPEN, iaus/Engine.h Forget / EndDecision threading, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `Forget` is documented for the "toggle edge or dismissal" but mutates `g_minds` with no lock (Engine.h:59-62). The board toggle runs on the render thread and dismissal on main / Papyrus, so I5/I6 must marshal both to the worker. Reasoning: every Mind access is the g_followers worker domain (#4); a direct call from render or main is a data race on the map. Author round 1 added the worker-only comment at Engine.h; the marshalling itself belongs to I5/I6. (The companion SEV-4 "Forget/Clear reset Mind.epoch to 0" was FIXED in round 1, not backlogged: `g_epochFloor` keeps the epoch monotonic.)

### MFO-B640 (SEV-5, OPEN, iaus/Catalogue.cpp BumpRow, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `BumpRow` indexes `p.family` / `p.role` unchecked at `kCount` (Catalogue.cpp:109-110). Reasoning: I8 is the only caller and passes table data, so only a data bug reaches it, but an out-of-range index writes past the array.

### MFO-B641 (SEV-5, OPEN, iaus/Score.cpp negative weights, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: a negative profile weight gives comp > 1 and can give a negative score, which breaks prune soundness (Score.cpp:86). Reasoning: `s^w` with w < 0 exceeds 1; the P5 bound assumes every compensated term is <= 1. I8 must validate w >= 0 (or Score must clamp).

### MFO-B642 (SEV-5, OPEN, iaus/Score.cpp kMaxSet, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `kMaxSet = 24` truncates a longer consideration set silently (Score.cpp:33). Reasoning: the largest set today is 20, so nothing is lost now; a `static_assert` over the set sizes would make a future overflow a compile error.

### MFO-B643 (SEV-5, OPEN, iaus/Decide.cpp Scored::idx width, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `Scored::idx` is `uint16` while `maxCandidates` is a tunable int (Decide.cpp:95). Reasoning: the default cap is 384; a tuned cap above 65535 would wrap the index.

### MFO-B644 (SEV-5, OPEN, iaus/Decide.cpp d.rolled, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `d.rolled` is true when the eligible list has more than one entry but SharpPick's own k / floor pool is 1 (Decide.cpp:176). Reasoning: cosmetic, the `[iaus]` line prints a roll that did not choose anything.

### MFO-B645 (SEV-5, OPEN, iaus/Decide.cpp posture stats, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: Posture candidates passed to `Decide` are counted as vetoed in the action stats (Decide.cpp:102). Reasoning: the `veto=` count in the `[iaus]` line is inflated if I6 passes both layers in one span; better a separate counter or a documented "never pass posture here".

### MFO-B646 (SEV-5, OPEN, iaus/Catalogue.cpp KeyOf includes posture, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: `KeyOf` includes `f.posture` for action candidates, so a melee / silver swap resets PowerAttack's cooldown and momentum key. Reasoning: if I4 fills `f.posture` on action candidates (to say which weapon they use), the Cooldowns id and the running key change with the weapon; I4 should leave it None on action candidates or KeyOf should drop it outside the Posture family.

### MFO-B647 (SEV-4, CLOSED by marth Q62, iaus Q34 strict reading, against a0745ee6, Opus tier-3 review of I2 round 1)
Raised against a0745ee6: ruling Q34's last sentence ("only abilities from mastered classes are valid in emergencies") read strictly would also bar ON-class abilities of an unmastered class in an emergency. The author implemented it behind `Tuning::emergencyOnlyMastered`, default OFF. Decision: keep it OFF; the coordinator saves the strict reading as a question for marth.
**Resolved 2026-11-10:** marth answered Q62 "own class stays valid", so OFF is the ruling. Closed.

### MFO-B648 (SEV-5, OPEN, iaus/Catalogue.cpp cosmetics, against a167d994, Opus tier-3 review of I2 round 2)
Catalogue.cpp:16: an odd indent on `float B(bool)`, and `<limits>` is probably unused now. Cosmetic.

### MFO-B649 (SEV-5, OPEN, iaus DecidePosture bad facts, against a167d994, Opus tier-3 review of I2 round 2)
DecidePosture: a posture candidate with a bad fact is vetoed silently. It is not counted in badFacts and no WARN fires, because only Decide's scoreOne counts them.

### MFO-B650 (SEV-5, OPEN, iaus/Score.cpp inf from an input calc, against a167d994, Opus tier-3 review of I2 round 2)
The input-calculation check catches NaN but not inf. No current input can produce inf from finite facts, so nothing is reachable today.

### MFO-B651 (SEV-4, OPEN, iaus window anchor, for I9 tuning, against a167d994, Opus tier-3 review of I2 round 2)
The window re-anchors on every Fired (design §4.5, same as #26), so a decision that keeps firing needs about 1.56x its base score to be replaced, at every rank; the rapport roll only acts when a decision lapses or changes. Within the design, not a defect. I9 option: anchor at decidedAt instead, or lower fIausPreemptMargin. I4 brief note from the same review: never leave a stale or garbage value in a Facts field the family does not read, because FactsFinite vetoes the whole candidate.

### MFO-B652 (SEV-5, OPEN, FlwrEnabledByte.h stray bytes, against 906c603, Opus tier-3 review of I5)
Bits 5-7 are ignored rather than clamped, and a stray 0x08 or 0x20 reads enabled=true, helmet Default, toggle off. That is exactly what both older DLLs read, and no writer produces it. Intentional and documented; the reviewer agrees. Recorded only.

### MFO-B653 (SEV-5, OPEN, MAP.md FollowerState slot caps citation, against 906c603, Opus tier-3 review of I5)
Pre-existing drift: "Slot caps ... :112-113" should be State.h:142 (kCombatSlotsByRank). Someone following the map lands on the helmetOverride comment.

### MFO-B654 (SEV-5, OPEN, FlwrEnabledByte.h:34/:59 sign conversion, against 906c603, Opus tier-3 review of I5)
clang/g++ -Wsign-conversion warns on the int-to-unsigned shift; tools/flwr_byte_test/run.sh suppresses it. Same expression as the old code, harmless. Also noted (process, no action): the new header was not named in the brief; disclosed, verbatim move, proven.

### MFO-B655 (SEV-5, OPEN, scheduler/CombatScan.cpp + Scheduler_internal.h file-header comments, against 4d6a8f2, I0 author least-sure spot + Opus tier-3 review)
The file headers say "proven function by function with tools/splitcheck" without naming the one allow entry (tools/splitcheck/allow/iaus-i0.txt, ServiceRetreat). Left unedited so the proof builds stayed paired; MAP.md and Docs/research/2026-11/iaus-i0-split.md state it accurately. Fix with the next edit to either file. Also: CombatScan.cpp is 1622 lines (past ~1500); the split plan needs Tick broken into functions first (I1-shaped work).

### MFO-B656 (SEV-4, OPEN, iaus/SpellBook.cpp StaffCharge, against bbd9bdf, Opus tier-3 review of I4, F3)
Staff charge has never been observed. The symbols are verified against fork 0b904231 (kRightItemCharge 64, kLeftItemCharge 82, ExtraCharge::charge float@0x10, amountofEnchantment uint16). The semantics are not: whether an NPC's equipped staff really drains k*ItemCharge (vanilla NPC enchanted items are commonly reported not to drain), whether EnchantmentItem::CalculateMagickaCost(actor) is the charge cost per use, whether a missing ExtraCharge always means full, and whether an unequipped stack of mixed charges is read correctly (only the first ExtraCharge is). A wrong "EMPTY" silently removes the staff posture. Before I6 relies on it: a passive, rate-limited [spellbook] probe line around a staff fire (AV, ExtraCharge, EAMT, cost per use).

### MFO-B657 (SEV-4, OPEN, iaus/SpellBook.cpp GateFor:326 + Generate.cpp Casts, against bbd9bdf, Opus tier-3 review of I4, F4 = Q63)
Postures honour `resolved && strict`, but the cast gate ignores both strict and treeOn. With the tree OFF and strict OFF, a T1 Melee follower's own learned heals produce zero candidates. That conflicts with "the tree is optional, new gambits work without it". Not checked: that no T4/T5 class with line Melee or Ranged carries cast rows. Needs marth's ruling (Q63) before I6.

### MFO-B658 (SEV-4, OPEN, I6 obligation, against bbd9bdf, Opus tier-3 review of I4, F5)
Buff/ward activeOn and summon liveSummon in the book can be up to 5 s stale (kRefreshSec), and no container event covers them. I6 must call SpellBook::Invalidate after every Fired buff, ward, summon and drink. The author's log names only summon and drink.

### MFO-B659 (SEV-5, OPEN, iaus/SpellBook.cpp PostRefresh:289-290, against bbd9bdf, Opus tier-3 review of I4, F6)
When the actor is gone, `asked` is reset to epoch, so the next Read re-posts at once instead of after 3 s, despite the comment saying the stamp ages out. Rare because Gather pre-checks 3D and dead.

### MFO-B660 (SEV-5, OPEN, iaus/SpellBook.cpp:227, against bbd9bdf, Opus tier-3 review of I4, F7)
Entry.onClass = !gate.strict || gate.weaponStaff ignores gate.resolved, so an unresolved follower under strict shows his staff as off-class. Postures use resolved && strict. Only the log and board mirror are affected.

### MFO-B661 (SEV-3, OPEN, MUST FIX BEFORE I6 ENABLES IAUS, iaus/Prune.h BlastUnits + SpellBook MakeEntry, against 325e8ea, Opus check of the I4 fix round, N1)
BlastUnits takes max(EFIT x 64/3, explosion radius), so a spell with EFIT area 0 but any projectile/MGEF explosion gets areaUnits > 0. Generate then sets hasArea=true and computes splashSafe, and Catalogue.cpp:179 makes SplashSafe a veto. A bolt-type spell with a cosmetic impact explosion (damage 0, no enchantment) would be vetoed whenever an ally is within that radius of the target, which is the common case of a melee tank beside his foe. Fix: let the explosion enlarge only a spell that already has arch.area > 0, or one whose explosion carries damage or an enchantment (BGSExplosion damage at 0x34), until the area probe proves the semantics.

### MFO-B662 (SEV-4, OPEN, iaus/SpellBook.cpp staff probe, against 325e8ea, N2)
The B656 staff probe logs once per staff form per session, so it records one sample from the first follower measured, probably while the staff is full. It cannot show whether an NPC staff drains. Log again when the charge read changes, rate-limited.

### MFO-B663 (SEV-5, OPEN, iaus/SpellBook.cpp Measure, against 325e8ea, N3)
Probe strings are formatted on every Measure (every 5 s per area spell and staff), even after the form is already logged. Pre-check the ledger first.

### MFO-B664 (SEV-5, OPEN, cast/DirectSelf.cpp HealClaimLive, against 325e8ea, N4)
Spell and target are read under two separate bridge-lock acquisitions, so they are not an atomic pair. Harmless while claims only change on the worker.

### MFO-B665 (SEV-5, OPEN, iaus/SpellBook.cpp MakeEntry, against 325e8ea, N5)
Chained explosions (BGSExplosion::spawnProjectile) are not followed for the blast radius.

### MFO-B666 (SEV-5, OPEN, scheduler/CombatScan.cpp size, against 7ba469c, Opus tier-3 review of I1)
CombatScan.cpp is 1724 lines, past the ~1500 plan-a-split line. The next brief that touches it proposes the split as its own round. The reconciliation (~:1290-1577) and the bookkeeping are still spans inside Tick and must be extracted first (see B655).

### MFO-B667 (SEV-4, OPEN, I6 obligation, cast/Fire.cpp:216 g_firingRule, against 7ba469c, Opus tier-3 review of I1)
g_firingRule = a_choice.rank feeds the rank-preemption cast lock (sentinel kNoRule = INT_MAX). An IAUS rank (>= 1000, up to 1e9) still reads as ranked, but a table rank (0-254) always outranks an IAUS rank there. I6 must decide whether that cross-source comparison is what it wants and write it down.

### MFO-B668 (SEV-4, OPEN, iaus posture under VR / no fresh book, against 71359dd, Opus tier-3 review of I6, F3)
In VR, or whenever the spell book is never fresh, the posture is always unknown. An inherited weapon hold then lasts until combat ends and no posture order is ever issued. Bounded; document it in MAP §11.

### MFO-B669 (SEV-5, OPEN, iaus/SpellBook.cpp:418-427 scrollUser, against 71359dd, F4)
With the class tree OFF, scrollUser is still keyed to class. Inert today (Scroll has no road), but it contradicts Q63 once one exists.

### MFO-B670 (SEV-5, OPEN, iaus/Prune.h BlastUnits, against 71359dd, F5)
BlastUnits still takes the max with a cosmetic explosion radius when the EFIT area is > 0. B661 is fixed only for area 0. Pre-existing behaviour.

### MFO-B671 (SEV-5, OPEN, IAUS beneficial cast subject, against 71359dd, F6)
If a beneficial cast's handle dies between MakeChoice and Fire, it falls to Player, which can spend a heal on a full-health player. The window is within one lap.

### MFO-B672 (SEV-5, info, PrecisionBridge in-image check, against 71359dd, F7)
The in-image check relies on Precision's interfaces being append-only. A future Precision that grants V2 with a different slot 8 would still pass; the call is probe-only today. Optionally log the slot offset against +0x117fe0 (it already prints it).

Closed in code by I6 (merge of feat/iaus-i6-full): MFO-B651 (Q67 no re-anchor), B657 (Q63 tree-OFF gate), B658 (Invalidate after Fired), B661 (explosion-only area), B667 (decided, MAP §2 "THE IAUS SOURCE"); B639 marshalling done (IausSweep / IausToggleEdge on the worker).

### MFO-B673 (SEV-4, OPEN, Board_Iaus.cpp kStaleSec, against cad75de, Opus review of I7, F1)
The 3 s stale threshold is a flat expiry, not sized from the cadence (principle 9). A follower's readout refreshes once per own service = N x 133 ms (N = active roster incl. summons, Scheduler_internal.h:303). At about 23+ actors a live fight shows "Last decision, N s ago (not in a fight now)". Display-only. Fix: ~3 laps x N x 133 ms with a 3 s floor, or an in-combat flag in the Readout.

### MFO-B674 (SEV-5, OPEN, iaus Explain/MakeReadout, against cad75de, F2)
The panel row shows s.score from Decide, but the breakdown terms come from FormatWhy -> ScoreOne with fixed extras (1.0, 0.0) on the post-Decide Mind, so momentum/roll folded into s.score is not in the terms. The [iaus-why] line has the same property. Note or fix with I8's profile pass.

### MFO-B675 (SEV-4, OPEN, iaus/Candidates.cpp ask cap 24 vs cast/WillWork.cpp kMaxEntries 256, against 0c70d81, Opus review of D1, F1)
The per-follower ask cap of 24 raises the WillWork shared mirror's worst case for a ten-follower party from ~60 to ~240 keys. When the cache is full and every entry is within trust, WillWork.cpp:196-200 runs g_cache.clear(): every follower's verdict goes Unknown for a lap plus a re-measure post burst. Degrades to the old behaviour (Unknown passes, Fire refuses at fire time). Fix: cap ~12, raise kMaxEntries, or document the coupling in both files.

### MFO-B676 (SEV-5, OPEN, iaus/Generate.h NextAskList doc, against 0c70d81, F2)
"never a top pair" (dropped for the cap) holds only while |top| <= cap (6 <= 24 today); nothing asserts it. Add a static_assert or note.

### MFO-B677 (SEV-5, OPEN, iaus/Candidates.cpp kRefusedKeepSec comment, against 0c70d81, F3)
The 6 s keep is timed from Gather's stamp, not the mirror's measurement time, so the list can hold a pair up to ~6 s after the mirror's No expired (extra Unknown asks only). Comment wording.

### MFO-B678 (SEV-4, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
class tauntCooldownSec replaces the IAUS taunt floor (Generate.cpp:117-118) instead of max(rowCd, kTauntCooldownSec); latent (0 today).

### MFO-B679 (SEV-5, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
LogSkip static map not cleared in Clear() (bounded, harmless).

### MFO-B680 (SEV-5, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
Cooldowns use steady_clock (run out in menu pauses) vs Taunt g_clockMs game clock; existing convention.

### MFO-B681 (SEV-5, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
MAP.md Taunt TauntedBy/Busy line refs stale (~407/413), IsPinned has none; TAUNT-DESIGN.md "a different taunter repoints it" outdated.

### MFO-B682 (SEV-5, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
(pre-existing) Resolve calls APMFBridge::PinFoeToTaunter under Taunt g_mx, so g_mx is not strictly a leaf; no cycle.

### MFO-B683 (SEV-5, OPEN, taunt rules, against d159a89, Opus review of feat/taunt-rules)
Estimate.h add/add conflict with feat/iaus-sim-v2: resolve by taking sim-v2's file.


### MFO-B684 (SEV-3, FIXED on feat/iaus-engine-fills: SpellEntry::measured + the bestOn filters, harness EngineFillRules; was OPEN, IAUS phase 2, against 303c14d/34ae66d, Opus tier-3 review of feat/iaus-sim-v2)
(before the engine fill card) iaus Generate bestOn (DamageValue denominator) takes every kept affordable offense spell without the per-candidate filters (off-class Q34, hand held P1, undeadOnly vs living, reach), so a strong undead-only spell shrinks offense scores vs every living foe. Also Snapshot uses 0 for "not measured" and "measured 0": the fill needs a per-entry measured flag (DamageValue input 0 gives 0.30, not a veto).

### MFO-B685 (SEV-4, OPEN, IAUS phase 2, against 303c14d/34ae66d, Opus tier-3 review of feat/iaus-sim-v2)
MAP.md lacks Estimate.h, Reach.h, the new Snapshot/Facts fields, the four new families and road flags; "Run it after ANY change to" split across the sim and dash lines.

### MFO-B686 (SEV-5, OPEN, IAUS phase 2, against 303c14d/34ae66d, Opus tier-3 review of feat/iaus-sim-v2)
four Cons named "need" (ambiguous [iaus-why]); -Wconversion at harness.cpp:917-923; Guards() loops foes with every road off.

### MFO-B687 (SEV-4, FIXED on feat/iaus-engine-fills: Fills.cpp LearnReach leaves meleeReach at fMeleeReach while the learner is unknown; was OPEN, IAUS phase 2, against 303c14d/34ae66d, Opus tier-3 review of feat/iaus-sim-v2)
(Reach wiring card) an unknown Reach learner returns Gate()==0, which makes RangeMelee not applicable (power attack from any distance): wiring must fall back to fMeleeReach while !Known().

### MFO-B688 (SEV-4, OPEN, IAUS phase 2, against 303c14d/34ae66d, Opus tier-3 review of feat/iaus-sim-v2)
MAP.md §11 lists 'DamageValue 0 for a no-damage spell' as LIVE; it is not live until the engine fills spell damage (term reads Off). Move it to the NOT-live list.


### MFO-B689 (SEV-5, OPEN, apmf/CombatMove.cpp g_last + apmf/GroundApproach.cpp g_steps, against 694df41, Opus tier-3 review of feat/iaus-combat-move)
Not cleared on load. Harmless (Harbinger handles are process-monotonic, a stale step drains in 700 ms with one log line). Clear in ResetAllState with the next touch.

### MFO-B690 (SEV-5, OPEN, iaus/Fills.cpp F2 indirect-spell test, against 7684dc3, Opus re-check of feat/iaus-engine-fills)
"an Offense spell is "indirect" if any one of its effects is Script, Cloak, SpawnHazard or EnhanceWeapon. Modded destruction spells often carry a Script effect next to a direct health ValueModifier, so those also go unmeasured and lose their readable direct damage." Errs safe (unmeasured, as before the fills, never a wrong 0). Refine: indirect only when no direct Damage effect is present.

### MFO-B691 (SEV-5, OPEN, iaus/Fills.cpp F1 srcLeft, against 7684dc3, same re-check)
"when he dual-wields the same weapon form in both hands, lw == w, so srcLeft stays 0 and left-hand hits count as right-hand hits." Informational: same weapon, same reach, so the reach evidence is still valid.

### Harbinger-side backlog from the same review (recorded here for the MFO adoption; APMF entries in APMF Docs): F1-3 HitSeen matches the engine's own bash within 2.5 s; F1-4 the combat move does not yield to an active ch.8 cast claim; F1-5 kAtkLeft is the charge bit; F2-2 pointSpace bound at the first sweep; probe Post per melee hit while [Probe] bCombatMove is on.

### MFO-B692 (SEV-4, OPEN, IAUS classes lab tools/iaus_sim, against 62f67b1, Opus review of feat/iaus-classes)
"Free stealth entry ends only on a weapon hit. Where: Setup.h `cloakUntil = 1e9` is cleared only in `Physical`. A spell cast (ReleaseCast) or a missed arrow never ends it, so a follower who opens with a spell (for example the parked Listener's Fear) stays invisible to foe targeting for the whole fight." Reasoning: Lab-only (tools/iaus_sim), no native code. Fix with the next stealth lab round.

### MFO-B693 (SEV-4, OPEN, IAUS classes lab tools/iaus_sim, against 62f67b1, Opus review of feat/iaus-classes)
"Staffs read the hand sneak rows. In Data.h `SneakKind`, anim 8 (staff) maps to hand. compile_world never emits hand rows today, so the effect is latent." Reasoning: Lab-only and latent today.

### MFO-B694 (SEV-5, OPEN, IAUS classes lab tools/iaus_sim, against 62f67b1, Opus review of feat/iaus-classes)
"Possible invalid JSON in the gap ledger. Results.h writes each class gap row with an unconditional comma prefix. That is invalid JSON if GapRows() is ever empty. It is not empty today." Reasoning: Lab-only results formatting.

### MFO-B695 (SEV-4, OPEN, selftest/Setup.cpp SpawnUnsafeReason mount skip, against 7e095eb, Opus review of fix/selftest-spawn-guard)
"an unridden follower horse still blocks the spawn. The player's horse is skipped even when nobody is riding it (QLastRiddenMount). A follower's horse is only skipped while the follower is on it (GetMountedBy rider is party). A dismounted follower horse within 3500 units that is protected or non-hostile still refuses." Engine limit: Actor slot 0x114 returns a null handle, only PlayerCharacter overrides it, so no follower has a last-ridden mount. Dev-build only. The refusal line now names the horse, so it diagnoses in one field cycle.

### MFO-B696 (SEV-4, OPEN, apmf/SneakClaims.cpp re-file, against a31f154, Opus review of feat/gcards-batch1)
"The re-file assumes MFO holds the only ch.3 claim on the actor. If another client also holds one, our Release does not bring the claim count to 0. The new Request then becomes an additional claim (ControlMap.cpp ~:1290, no Engage), so the re-assert sends nothing. It would log a RE-FILE every ~3 s and IsSneaking would stay FALSE." Visible in the log, not masked.

### MFO-B697 (SEV-5, OPEN, G1 combat Sneak row, against a31f154, same review)
"A combat-table Sneak row does nothing in the follower's own combat by default (switch off). The board gives no sign of this: the FailedOther reason reaches only the chain."

### MFO-B698 (SEV-5, OPEN, classes/ClassTable.cpp todo.* rows, against a31f154, same review)
"The card's always-true default row for the four sneak classes is not in the diff (ClassTable untouched)." The class rows still use todo.act.sneak, todo.cond.self_no_summon, todo.cond.foe_attacking_my_summon: the class defaults card wires them.
### MFO-B710 (SEV-4, FIXED @46213a0 on fix/iaus-sim-backlog, IAUS sim tool tools/iaus_sim/sim.cpp, against a6844761, Opus review of fix/iaus-sim-tool)
"`--seeds` headline always runs 24 fights and does not say so. `--seeds 1,2 --fights 60` gives exactly the `--fights`-less result, and the HEADLINE line has no fight count." Reasoning: consistent with the single-seed headline (also 24). The real 60-fight measurement today is `--opt <p> --opt-passes 0 --opt-fights 60 --seeds ...`. Drain before the Fable tuning round.

### MFO-B711 (SEV-4, FIXED @46213a0 on fix/iaus-sim-backlog, IAUS sim tool, against a6844761, same review)
"`covers=` prints what was asked for, not which classes were matched. `--classes mage,mgae --seeds 1,2,3` silently covers only mage but prints covers=mage,mgae. `--classes mgae --opt ...` runs the whole search at J=0.000 with no error." Reasoning: operator typo guard; drain before the tuning round.

### MFO-B712 (SEV-4, FIXED @46213a0 on fix/iaus-sim-backlog, IAUS sim tool, against a6844761, same review)
"The JSON `headline.j` is now scoped, but no field records the scope. A `--classes X` results file cannot be told apart from an all-class one; a later --compare or archive diff would read a scope change as a tuning delta. The oracle headline is scoped too but prints no covers=." Reasoning: results provenance; drain before the tuning round.

### MFO-B713 (SEV-4, FIXED @46213a0 on fix/iaus-sim-backlog, IAUS sim tool, against a6844761, same review)
"`--opt-rows` and `--opt-prefix` silently ignore `--seeds` and `--opt-fights`. They call Objective(seed, fights) directly." Reasoning: latent, the tune scripts do not pass these flags today.

### MFO-B714 (SEV-5, FIXED @46213a0 on fix/iaus-sim-backlog, IAUS sim tool, against a6844761, same review)
Notes: scoped scenario seed uses the filtered-list index (unbiased and paired, but not equal to the all-class slice; keeping the original index i would fix it); `--seeds` n=1 prints se=0.00 instead of n/a; `--seeds ,` silently ignored; the printed SE is between-seed SE of absolute J, looser than the paired-difference noise a keep decision needs; `--classes X` and `--opt-class X` are different objectives (0.5/0.5 natural only vs 0.35/0.65 over 3 brackets), the doc could say so.

### MFO-B699 (SEV-4, OPEN, iaus/Candidates.cpp NoteOcclusion, against 24a1dbb, Opus review of fix/iaus-field-1)
NoteOcclusion keeps stale occlusion state: the per-foe age is not reset across a pause or a target switch and has no cap, so a long-occluded foe that becomes visible and is occluded again can start with an old age. Reasoning: the hold is bounded by occlusionHoldSec and an untracked age (0) takes no hold since b838392; worst case is a shorter hold. Drain with the next IAUS round.

### MFO-B700 (SEV-4, OPEN, apmf/Excursion.cpp TargetCalmNow, against 24a1dbb, same review)
TargetCalmNow has no thread note: it reads actor state (combat target, hostility) and is called from the pin sweep and ReleaseTargetPin under g_mx. Document which threads reach it and that it takes no lock. Reasoning: doc-only; the reads are the same plain member reads the sweep already makes.

### MFO-B701 (SEV-5, OPEN, class gate GateFor, against 24a1dbb, same review)
GateFor strict-inherit: a follower with no own class row inherits the party gate strictly; a looser inherit may be wanted when the class tree switch is OFF. Reasoning: matches the current ruling; revisit with C2/C6.

### MFO-B730 (SEV-4, OPEN, StealthProbe.cpp hit ring, against 8c06925, Opus review of feat/g2-hide-layer1)
g_ring entries never age out, and the overflow count is credited to the first drainer. A hit recorded for a follower who is never serviced again (dismissed, unloaded) stays in the ring until Reset, and a full ring's overflow is added to whichever follower drains next, not the one whose hits were lost. Reasoning: the probe is passive and OFF by default (bStealthProbe), the ring is capped at 64 so nothing grows; the attribution of dropped hits is only a diagnostic line.

### MFO-B731 (SEV-4, OPEN, StealthProbe.cpp q87 drop, against 8c06925, same review)
The q87 drop merges hits between two health samples: two hits landing between one pair of samples both read the same before/after, so each is credited the combined drop, and the sneak/plain ratio is biased. Reasoning: a probe estimate with a stated tolerance (25%), not a decision input; I9 reads it as a rough check of the engine multiplier.

### MFO-B732 (SEV-5, OPEN, StealthProbe.cpp ResolveHitMain, against 8c06925, same review)
fCombatSneakAttackBonusMult is left out of "expected": the expected multiplier is the weapon's fCombatSneak*Mult through entry 18 only, and the engine's flat sneak attack bonus setting is not added. Reasoning: probe line only; the 25% tolerance absorbs it until the field run shows otherwise.

### MFO-B733 (SEV-5, OPEN, StealthProbe.cpp MovingBits, against 8c06925, same review)
The actorState1 moving bits (movingForward/Back/Left/Right) are not the engine's IsMoving / IsRunning (Address Library ids 37953 / 37234); the P1 moving= column can disagree with what the detection formula sees. Reasoning: the P1 line is a probe, the engine functions are not called by design (no new address in this layer); the field run reads the difference.

### MFO-B720 (SEV-4, OPEN, classes/Defaults.cpp LayDown/LandPicks, against e6aaa1c, Opus tier-3 review of feat/mcls-v2)
"A save in the 1 or 2 frames between LayDown and LandPicks stores the default cast rows disabled with no spell. After that save loads, nothing picks them again." Suggested fix: on load, re-post the pick for disabled, spell-less cast rows that still match the equipped class's default prefix (SameDefault). Reviewer: rare window, rows visible on the board and the WARN is logged; not a corruption.

### MFO-B721 (SEV-5, OPEN, classes/State.cpp SwapClass + dev hook, against e6aaa1c, same review)
"If test rows are live when the dev hook equips, SwapClass stores them in offCombat and Scrub zeroes their uid", which gets past FLWR's SelfTest::IsRunRow strip (Serialization.cpp:596), so they can persist in dev saves through MCLS. Dev build only.

### MFO-B722 (SEV-5, OPEN, classes/State.cpp SwapClass, against e6aaa1c, same review)
"A refused SwapClass (locked, or already equipped) still creates an empty g_store entry through Mutate and calls RefreshCarriers." Same as ActivateClass/SetPickedT4 today; harmless.

### MFO-B723 (SEV-5, OPEN, classes/ClassTable.cpp Todo rows, against e6aaa1c, same review)
Thief and Conjurer Todo placeholder rows (G1, G5, G6) remain though the opcodes exist on main (see MFO-B698). Also: the sim's kept Warrior/Conjurer/Warlock sets were reverted before merge (marth 11-11, "Revert to old rows"); they return after the in-game probe. Note for that round: the sim's Conjurer row 2 `cond.foe_lowest_hp -> act.attack` is true whenever any foe exists and shadows the foe_any cast row under first-true-wins.

### MFO-B724 (SEV-5, OPEN, Board_FieldKit.cpp Gambits-tab Class: button, against da493fe, Opus tier-2 review of feat/class-gui)
The disabled Gambits-tab "Class:" button shows the raw combatClassOverride name (e.g. "Class: Auto"), not the class root in effect while a class is equipped; the tooltip explains it.

### MFO-B725 (SEV-5, OPEN, Board_Classes.cpp, against da493fe, same review)
The Classes tab has no reserved footer, so with long trees the footer hint scrolls out of view.

### MFO-B752 (SEV-5, FIXED-IN-PLACE comment, classes/Xp.cpp "not wiped" test, against a5e3a4e, Opus review of feat/class-xp-mcm)
"The 'not wiped' test checks only the player (Xp.cpp:218-220) but the comment says 'and at least one follower alive'." Reasoning: the paying follower is serviced on his own tick so he is alive by construction; the comment was fixed to say the test is the player only. No code change.

### MFO-B753 (SEV-5, FIXED-IN-PLACE comment, Config.h bClassTree / bClassStrict, against a5e3a4e, same review)
"Config.h still says bClassTree/bClassStrict are 'INI-only (no MCM key)'." Reasoning: card C11 gave both a key in the class add-on's own MCM; the comment was corrected. No code change.
### MFO-B734 (SEV-4, OPEN, iaus hold claim + cannot-act, against 6253a53, Opus re-review of the G2 fix round)
Knockdown/paralysis churn: the hold row is picked by Next(), files ch.1 + ch.7 and logs FILED; the cannot-act check rejects it and the next Next() releases both and logs RELEASED: 2 Requests, 2 Releases and 2 unthrottled info lines per lap while he is down and hidden. Fix: service the hold only once the scan stops on it, or skip filing when he cannot act.

### MFO-B735 (SEV-4, OPEN, apmf/HideClaims.cpp kStaleSweeps, against 6253a53, same review)
The stale expiry counts pumps (30), not laps; a follower's lap is N x 133 ms, so it is only safe under about 30 followers, beyond which a live hold flaps release/refile every lap (principle 9). Use counted laps like SneakClaims kHoldLaps, or max(30, 3 x active followers). Legitimate logged release, not a mask.

### MFO-B736 (SEV-4, OPEN, StealthProbe.cpp:524-531 q87 median, against 6253a53, same review)
Hits with before=max go into the sneak median; "not in his group" does not mean full health, which pushes the median up and can mislead the Q87 verdict. Keep them out of the median or report them separately (with MFO-B731).

### MFO-B737 (SEV-5, OPEN, apmf/HideClaims.cpp refused claims, against 6253a53, same review)
If Harbinger refuses one of the two claims, the warning logs once but RequestEx is retried every hold lap (APMF.log may repeat the refusal); a partial claim (ch.1 without ch.7 or the reverse) is allowed to stand and is logged.

(Folded into the branch by the coordinator from the same review: F1 retreat lap now releases the hide hold, CombatScan.cpp; F4 a never-live handle is released before reset, HideClaims.cpp.)

### MFO-B740 (SEV-4, OPEN, Concealment.cpp / Sightline::PointSeenNow worst-case cost, against 1f59152, Opus tier-3 review of the movement batch)
The pick's worst case is about 432 picks plus 56 FindEmptySpace calls per frame per follower on the main thread. If the Deck hitches, add a per-frame ray budget (fewer candidates per frame, or a shared budget across followers).

### MFO-B741 (SEV-4, OPEN, Concealment.cpp hold test, against 1f59152, same review)
The hold's "he was hit" test (health drop since arrival) counts damage-over-time as "found". A poisoned or burning thief ends the hold early. Count only a hit event, or ignore a steady drain.

### MFO-B742 (SEV-5, OPEN, Concealment.cpp duplicate pick after Release, against 1f59152, same review)
A Release erases the follower's pick entry, so a pick job still in flight on the main thread can land after it (the epoch only guards a load) and a second pick can run for the same fight. Harmless today (one run per answer); a per-follower generation counter would close it.

### MFO-B743 (SEV-5, OPEN, a sustained heal claim during the run, against 1f59152, same review)
A sustained heal claim (the heal-hold records) is not released when the hide run starts; the run's ch.7 Offense deny and a standing heal approach (ch.24, which outranks a disengage) can contend. Observe in the field before changing. The non-sneak flee makes this more likely, since it wants heals during the run.

### MFO-B750 (SEV-5, OPEN, CombatStyle.cpp:341-345 comment, against 2628beb, Opus review of feat/class-packages)
The old "no owner on this path, or form missing -> stance disabled ..." comment now trails the class-style line instead of the `if (!target) return;` line.

### MFO-B751 (SEV-5, OPEN, C8 author's spots, against 2628beb, same review)
Accepted as documented: fClassLeashMult 0.5 is a tunable guess (floored at fLeashMin; field tunes it); dead followers are released on the not-managed path with the 30 s backoff for an ended claim; Harbinger absent = silent mix sweep (matches AimClaims); Dragonborn with no picked T4 uses its own tier-5 package; Templar vs-undead power floor not built (needs a conditional facet) and Ebony Warrior inherits 0.20 (not ruled).

### MFO-B755 (SEV-3, FIXED, tools/iaus_sim headline fights, against a2e7336, Opus review of fix/iaus-sim-backlog)
`hf = fightsGiven ? fights : 24` also changes the normal (non-`--seeds`) full-run headline J: before, it was always 24 fights; now `--fights N` computes it at N. The count is printed and in JSON (headline.fights), but `--compare` against an archived baseline made at another fight count would read the change as a J delta (unchecked whether --compare looks at headline.fights). Fix: back to 24 for non-`--seeds` runs, or make --compare refuse when headline fights/covers differ. The Fable tuning round must RE-BASELINE before tuning.
FIXED on feat/iaus-sim-stealth: a full run's headline J is always 24 fights per scenario (a --seeds run still honours --fights). A --baseline file starts with a #meta line and --compare refuses a baseline whose line differs. CLI checks in run.sh.

### MFO-B756 (SEV-4, FIXED, tools/iaus_sim JSON oracle_over, against a2e7336, same review)
B712 incomplete: the JSON records covers and fights for the main J, but oracle_over has no covers field for the oracle headline (only the printed oracle line gained covers=).
FIXED on feat/iaus-sim-stealth: headline.oracle_covers in the JSON. CLI check in run.sh.

### MFO-B757 (SEV-5, FIXED, tools/iaus_sim jCovers, against a2e7336, same review)
jCovers defaults to "all classes" even when hasJ is false (JSON can claim coverage for a J that was not computed), and fights is then 0. Cosmetic.
FIXED on feat/iaus-sim-stealth: jCovers has no default, and headline.covers / headline.fights are null when no J was computed.

### MFO-B758 (SEV-5, OPEN, B714 notes, against a2e7336, same review)
The B714 notes on paired-difference SE and on --classes vs --opt-class being different objectives are handled only in iaus-sim-v2.md; the backlog marks the whole of B714 FIXED.

### MFO-B759 (SEV-4, OPEN, apmf/SneakClaims.cpp:236-243 HoldSneakForHide, against ba1328c, Opus re-review of the G3 fix round)
F3. A hideHeld sneak claim that Harbinger refused at the hidden end is never refiled: HoldSneakForHide answers "Harbinger refused the sneak claim (retrying)" while retryIn > 0, and nothing calls it again once the run has ended, so G2's hold row then has no stance claim. Refile from the stance watch (Concealment::Service, the no-run branch) or from the sweep.

### MFO-B760 (SEV-4, OPEN, Concealment.cpp flee end, against ba1328c, same review)
F4. The flee ends with no hysteresis the moment health is above 0.30 (kFleeHealthLine), so a follower hovering at the line can end a flee and qualify for the next one at once. Bounded by the family's 25 s cooldown, counted from the end of the run. A second, higher line for the end would remove the flap.

### MFO-B761 (SEV-4, RULED, Retreat vs flee below 0.30 health, against ba1328c, same review)
F5. Below 0.30 health the Retreat family and the non-sneak flee are both generated and compete on score. marth 2026-11-11: "Score decides". Recorded as ruled, not a defect: the sim tunes the two weights.

### MFO-B762 (SEV-5, NOTED, scheduler/CombatScan.cpp death branch, against ba1328c, same review)
Out of brief in the G3 fix round: the CombatScan follower-down branch also calls APMFBridge::ReleaseHideHold (a G2 claim). Harmless (idempotent, and the right thing for a dead follower). Noted so the diff is explained.

<!-- T2 batch M entries (MFO-B800 onward) sit here, above the G7 block, so this file merges cleanly with the entries other branches append -->
### MFO-B800 (SEV-3, OPEN, iaus/ + scheduler/IausSource.cpp, raised by the author against feat/melee-moves, 2026-11-11)
Sentient AI (an `iausCombat` follower) has no Block family: `act.block` is produced only by the table scan. A Knight on Sentient AI never raises a held guard. The IAUS files are outside batch M's file boundary (batch I owns them). Fix direction: a `Family::Block` candidate behind `Snapshot::blockRoad` (ABI >= 43 and `BlockHandsReason == nullptr`), scored from incoming melee threat, mapped to `Vocab::kActBlock` in `IausSource.cpp`, and read from the class row in `iaus/SpellBook.cpp` (the `powerAttack` / `taunt` pattern) with its cooldown. The sim (batch S) needs the same family. Deferred, not dropped: the T2 field session tests Block with Sentient AI off (test step 24m).

### MFO-B801 (SEV-4, OPEN, apmf/BlockClaims.cpp, same branch)
Not field run. What only a game can show: (1) whether 2.5 s on / 1.5 s off reads as "blocks a lot" (Q-melee-1); (2) whether `Foe: Targeting me (melee)` is true early enough to have the guard up before the swing lands (it reads the foe's combat target, not its swing); (3) how the follower moves while his Offense leaves fail (he should keep facing and closing); (4) whether `BlockHandsReason` and Harbinger's `HandsWouldNotBlock` ever disagree on a real loadout (a `[block] ... REFUSED: his hands would not block` line after MFO's own gate passed).

### MFO-B802 (SEV-5, CLOSED 2026-11-12 by the batch M fix round, classes/ClassTable.cpp, same branch)
CLOSED: marth 2026-11-12 (Q-melee-2): the Ebony Warrior is Knight line and carries the row; every other class blocks only by the engine's own style. `kEbonyWarriorG` has the row; Templar deliberately does not.
Only Knight and Great Knight carry the default Block row. Templar and Ebony Warrior may equip shields and inherit the Great Knight's line but their default sets were not changed (Q-melee-2).

### MFO-B803 (SEV-4, CLOSED 2026-11-12 by the batch M fix round: `Cooldowns::Forget` from `PollCombatMove` on a Refused / Failed power attack, marth Q-melee-4; cast/Fire.cpp power attack road=harbinger + Cooldowns, raised by the author against feat/melee-moves, 2026-11-11)
On road=harbinger the handler returns `Fired` when the request is accepted at the call. The row's cooldown (the follower-wide 10 s power slot, Q73) and the suppression window start then. Harbinger can still refuse on the game thread (graph not attack-ready, no stamina for any power entry, a ch.7 Offense deny, MFO ranked below Engine): the swing does not happen and the cooldown is spent. `PollCombatMove` logs the refusal. Fix direction: a `Cooldowns::Forget(actor, kind, id)` called from `PollCombatMove` when a power attack ends Refused or Failed (Cooldowns.h is outside batch M's boundary), or a synchronous dry run through Harbinger's `CanServe`. Reasoning for SEV-4: no wrong action is taken; the cost is a late next power attack.

### MFO-B804 (SEV-4, OPEN, apmf/StyleClaims.cpp + CombatStyle.cpp, raised by the author against feat/melee-moves, 2026-11-11)
Not field run. What only a game can show: (1) that a class style claimed actor-wide visibly changes how a follower fights and does not break the class's other facets (the leash, ch.18 mix, the equip authority); (2) that an equip-choice-only claim holds a weapon stance as well as the old per-tick write did (the old write re-asserted every combat tick; the claim is applied by the engine's own refresh at engage, a package change and release, and Harbinger's seat answers every later refresh); (3) the handoff latency of one 133 ms sweep plus one Harbinger frame on a melee <-> ranged flip; (4) the substrate cast pair's style arriving on the substrate controller in time for the first cast score (the pair opens, the sweep files, Harbinger refreshes). `bCstyReassert`, the rederive counter and the F2 restore are dead code on the Harbinger road and can be removed once the road is field-proven and the absent-degrade is all they serve.

### MFO-B805 (SEV-4, OPEN, apmf/StyleClaims.cpp SweepStyleClaims, same branch)
A Harbinger v45+ that refuses the claim at the call is re-asked every ~30 s per follower with one WARN each. There is no session latch (the sibling tables latch and were flagged for blaming the seats on any refusal, MFO-B765) and no `CanServe` dry run. Fix direction: ask `CanServe` once per load and name the refusal reason in MFO's own line.

### MFO-B806 (SEV-3, VALUE SET 2026-11-12 by the batch M fix round: `CLS_AVOID_THREAT = {'mercenary': 0.3}` (marth Q-melee-7), `MFO_Classes.esl` regenerated, audit PASS; the FIELD step below stays open; MFO_GenerateESP.py class_csty_params / CLS_CSGD, raised by the author against feat/melee-moves, 2026-11-11)
T2 batch M item 4 (the Mercenary's dodge) is NOT delivered. The road exists since item 5: the engine's own side step (CombatBehaviorDodgeThreat) rolls against the combat style's `avoidThreatChance`, read through `Actor::GetCombatStyle`, and a class style is now claimed actor-wide through Harbinger ch.33. But every MELEE class CSTY is generated with `avoidThreatChance` 0.0 (`CLS_CSGD[1]`, tenth value) and only the `kDisengage` non-melee classes get +0.20, so the Mercenary's style asks for no dodge. Fix direction: a per-class Avoid Threat value for the Mercenary's melee CSTY (suggested 0.5, the vanilla missile style's) in the generator, regenerate `MFO_Classes.esl`, audit with `tools/audit_esp.py`, then the field step: a Mercenary side steps incoming arrows and spells on a list with no dodge mod, and on LoreRim Ultimate NPC Dodging rolls him (its score reads the same style fields; whether it reads them through `Actor::GetCombatStyle` or the base record was not verified). If the field shows the vanilla side step is too weak to read as a dodge, a dedicated Harbinger dodge move is the next design (framework event names resolved at runtime). SEV-3 because a brief item is open, not because anything is wrong.

### MFO-B807 (SEV-5, OPEN, classes/ClassTable.cpp Knight / Great Knight / Ebony Warrior Block row vs classes/Defaults.cpp SameDefault, Opus tier-3 review of 88777f7, finding F7)
As relayed by the coordinator: "ClassTable Block row at index 1 vs SameDefault prefix". The Block row is INSERTED in the middle of the class default sets (after the taunt rows), not appended. `Defaults.cpp` compares a follower's live table with the class defaults row by row (`SameDefault(live[i], defs[i])`, :429) to tell "still the untouched default" from "the player edited it". A follower whose stored Knight table was written from the OLD defaults no longer matches from the inserted index on, so he reads as player-edited and is left alone: he does not pick up the Block row (and would not pick up any later default change either) until the class is re-equipped or reset. No wrong action is taken. Fix direction: append new default rows at the end, or version the default sets so an untouched older default can be recognised and upgraded. Reasoning for SEV-5: classes are not released yet (Classes Beta), so no shipped save holds the old Knight defaults.

### MFO-B808 (SEV-4, OPEN, apmf/BlockClaims.cpp kMaxRenewals / kRest, raised by the author in the batch M fix round, 2026-11-12)
The guard's bounds (one renewal, 1.5 s of rest after any guard) are constants chosen without a field run (Q-melee-8). They are not INI keys. The block row's own cooldown is not known to `ServiceBlock`, so the rest also applies after a guard that the row's cooldown already spaces (harmless: 1.5 s is below the 4 s default). A Block request Harbinger refuses on the game thread after MFO's pre-gates (not attack-ready, MFO ranked below Engine) still spends the ROW's cooldown: only the power attack's follower-wide slot is given back (Q-melee-4), because `PollBlock` does not know the row's uid.

<!-- card G7 entries (MFO-B766..B770) sit here, above B763, so this file merges cleanly with the base branch's own appended entries -->
### MFO-B766 (SEV-4, OPEN, cast/Corpse.cpp + cast/BuffRoad.cpp, raised by the author against feat/g7-reanimate, 2026-11-11)
Card G7 is not field run. Unproven: (1) that a Reanimate claim fires at all under a claim (MFO-B232's N3 never got its field cast either); (2) that `WillWork`'s Yes and the engine's own keep test agree (Harbinger APMF-B285: whether the engine's validity test enforces the level cap is not read; MFO-B583: WillWork's cap misses dual-cast scale and two entry points). Where they disagree the claim never fires, Harbinger logs `REFUSED the claimed corpse`, the never-fired bound releases it after `kHoldLastSeenCapMs` and that corpse is held off for the fight; (3) the release after a successful raise: with the commanded-actor limit at 1 the up-read turns Blocked and ends the claim, with a higher limit the rule simply stops naming that corpse and the claim ages out unrefreshed. Reasoning: disassembly proves a path exists, not that it runs. Next field cycle exercises this (carve-out b), so it is listed, not deferred silently.

### MFO-B767 (SEV-4, OPEN, cast/Corpse.cpp PickCorpse, same branch)
The pick has no line-of-sight test. A vanilla Reanimate spell is an aimed projectile, so a corpse behind cover is cast at and missed. Bounded: the fire is observed, the corpse stays dead, and BuffRefreshGate's fired-but-never-landed bound releases the claim and holds that corpse off for the fight. Costs one cast of magicka per hidden corpse. Fix shape: `Sightline::Want` / `CheckWithin(Basis::Own)` on the candidates, as the offense road does for a foe.

### MFO-B768 (SEV-5, OPEN, apmf/Bridge.cpp BuildCastRequestLocked, same branch)
The builder sets `kCastFlag_CorpseTarget` for ANY claim of a spell with a Reanimate effect at another actor. A spell that has a Reanimate effect AND a hostile-flagged effect classifies Offense and is claimed at a LIVE foe on the offense road. If the engine builds the Reanimate caster for it, Harbinger's seat refuses the living target every poll and the claim never fires. Before G7 the engine raised its own corpse under that claim. No vanilla spell has that shape. Fix shape: also require the target to be dead in the builder (then the dry run, the replay and the claim identity must still agree across a raise).

### MFO-B769 (SEV-5, OPEN, cast/Fire.cpp ResolveCastTarget rung 0, same branch)
A "Corpse: Within -> Cast at target" row whose target was set to Self or Enemy on the board resolves to the caster or the commanded foe before the selector's corpse (rung 0 outranks `Choice::target`). Self then files the pre-G7 target-0 claim (the engine's corpse), Enemy casts a Reanimate at a living foe. The player chose it, so nothing is forced. The board could grey those two targets on a corpse row.

### MFO-B770 (SEV-4, OPEN, cast/CastOn.cpp below the buff fork, same branch)
With Harbinger absent or the claim road switched off, a corpse row falls past the buff claim fork onto the direct roads with a DEAD actor as the cast target (the AI-first hybrid, `ForceCast`). That road was never run at a corpse: before G7 a Reanimate rule could only reach it with a living target or the player (MFO-B84's history). It may raise the corpse unanimated (the documented "Harbinger absent" degrade) or do nothing. Not observed either way. The same road is reached WITH Harbinger when `ChooseBuffRoad` answers a Direct road for the spell (one `[buff] ... DIRECT road` line names why): an ambiguous row with an Unknown or NoRow effect beside the Reanimate one is still such a case. G7 only added `EngineRow::Reanimate` to `BuffAmbiguousRowWhy`'s bounded set.

### MFO-B771 (SEV-5, OPEN, cast/Corpse.cpp, against 533d6ef, Opus tier-3 review of feat/g7-reanimate, finding F5)
As handed to the author: "cast/Corpse.cpp uses IsDead() which accepts Dying; GetLifeState()==kDead would match StillCorpse." `IsDead()` is true for Dead, Dying and Recycle. Harbinger's seat (`core/ReanimateSeat.cpp` `StillCorpse`) and the engine's own keep test want life state Dead exactly, so a rule can pick an actor that is still dying and the seat answers NO for that poll (logged there). The claim stands and the next poll passes once the actor is dead.

### MFO-B763 (SEV-5, OPEN, Concealment.cpp WatchList, against 05260d0, Opus tier-3 review of the ch.7 v40 round, finding F5c)
As handed to the author (the reviewer's own log was a two-line summary): "WatchList duplicates Gather's gates." The hidden-end / reveal watch re-implements the gather's foe set tests instead of sharing them, so a change to one can drift from the other. Reviewer's log: "WatchList mirrors Gather (lock order OK)".

### MFO-B764 (SEV-5, OPEN, apmf/HideClaims.cpp ServiceHideHold, against 05260d0, same review, finding F5d)
As handed to the author: "FILED line repeats per stand-down." After every stand-down (and, since the F2 fix, every change of the deny's mask) `hadDeny` is false on the next lap, so the "ch.1 movement block + ch.7 ... deny FILED" line prints again next to the transition line that already says it. Note 2026-11-11 (fix round on 938f45f, 5955a22): a deny refiled under another mask INSIDE one lap used to skip the FILED line (`hadDeny` was read before the release), so the new handle was never logged; that lap now prints it. The repeat this entry describes is unchanged and still open.

### MFO-B765 (SEV-5, OPEN, apmf/AversionClaims.cpp FileLocked, against 5c77e8a, Opus tier-3 review of feat/class-aversion + Harbinger 0a052e1, finding H2-5)
As handed to the author (the reviewer's full report was not on disk; this is the finding as the coordinator relayed it): "MFO AversionClaims.cpp FileLocked latches g_avSeatRefused for the session on any refusal and blames the seats (AimClaims has the same shape)". `FileLocked` sets `g_avSeatRefused` on the first refused `RequestEx`, whatever Harbinger's reason was (a weight out of range, an unloaded actor, the player), logs it as the seats not being installed, and files no aversion claim for anybody until the next process start. `apmf/AimClaims.cpp` has the same latch. Reasoning for SEV-5: today the only refusals MFO can draw are the seat-level ones (MFO clamps the weight inside (0, 1) and files for loaded followers only), so the blame is right in practice; the latch becomes wrong the day Harbinger adds a per-request refusal. The fix is to ask Harbinger why (a seat-state query, or the refusal reason) before latching.

### MFO-B780 (SEV-4, OPEN, Concealment.cpp ServiceBlow :312-328 + NoteBlow :621-627, against c0cfe9a, Opus tier-3 re-review of the P1-G3 fix, finding 1)
"Once a blow is armed, nothing bounds the hold until his swing, shot or cast is actually seen." If the approach never closes, the action is not an attack/cast, or a short miss swing falls between two laps with no re-detect, he stays crouched with the sneak claim for the rest of the fight (until a reveal or fight end). Reviewer: this is the pre-pick hide hold exactly; principle 9 forbids a timer from the pick; test 22's lines show it, so carve-out (b) does not apply.

### MFO-B781 (SEV-5, OPEN, Concealment.cpp NoteBlow :623, against c0cfe9a, same review, finding 2)
A later blow pick while one is armed returns early without resetting `armedAt`, so the "N ms after the pick" in the under-way / RESOLVED lines counts from the first pick and misleads a test-22 reader.

### MFO-B782 (SEV-5, OPEN, Concealment.cpp ServiceBlow :301-317, against c0cfe9a, same review, finding 3)
`OwnCastInFlight` is used without a family check, so any live own cast (a heal during the hold) counts as the blow: the reveal check is skipped for up to 6 s + 1.5 s and the stance is released as "a miss". Small and logged.

### MFO-B790 (SEV-4, OPEN, logistics/TheftGate.cpp GuardInterim, against the theft-gate round, author's note)
The interim guard gate treats ANY live, non-teammate guard within 3500 u whose sight to the follower or item is Visible or UNMEASURABLE as
seeing him (it does not test the guard's hostility, facing or detection formula, and a fresh MeasureNow runs per ask on the main thread).
Over-barring is the point (marth 2026-11-11, interim), but a town with many guards costs one main-thread sightline pair per guard per
(follower, source) every 2 s. Remove or replace when the culprit is found.

### MFO-B791 (SEV-4, OPEN, logistics/TheftGate.cpp TakeBarred, against the theft-gate round, author's note)
`IsAnOwner(ref, actor, useFaction, requiresOwner)` (AE id 20210, the engine's own pickup/steal test) is NOT bound: it is a new address id
(tier A, needs a VerifiedAddresses row). The gate reads ExtraOwnership / `GetOwner()` data instead and bars ANY non-follower-side owner,
which is stricter than the engine's test (the faction-rank rule is approximated by membership). Bind it only with a seat row.

### MFO-B792 (SEV-5, OPEN, logistics/TheftGate.cpp Take, against the theft-gate round, author's note)
A plain (extra-list-free) copy that shares a stack with a barred copy is never taken (RemoveItem cannot choose the copy), so one stolen
arrow in a 20-arrow stack makes the whole plain remainder untakeable from that source. Same as the sale's "partly stolen" rule
(ErrandShops NoteMixed). A per-copy pick would need the engine's own removal order proven.

### MFO-B793 (SEV-5, OPEN, logistics/TheftGate.cpp Analyse / TakeBarred, against the theft-gate round, author's note)
A corpse or chest in an OWNED cell (a house, an inn) reads the cell owner through `GetOwner()` and is barred whole for its plain copies,
including what the player dropped there. Intended by "bar all stolen", but it also removes legitimate loot from owned interiors (bandit
corpses in a rented room). No cell/location exemption was added.

### MFO-B783 (SEV-3 handled in the 30.10 handoff, CLOSED by T2 batch S 2026-11-12: every deliberate movement keeps his feet going, `--feet-stop` is the old lab, tools/iaus_sim/World.h StepWorld / RunMove, against a645ca2, Opus tier-2 review of feat/iaus-sim-stealth, finding 1)
Only during a G3 run do his feet keep moving through a weapon swap, a potion or a cast; everywhere else (Retreat included) those still stop his body, so the lab "makes the flee look a little better than Retreat" (Q-SIM-5). The 30.10 handoff no longer searches `aw.4` against `aw.21` or `c.63.*` until the asymmetry is removed.

### MFO-B784 (SEV-4, CLOSED by T2 batch S: the World.h comment and iaus-sim-v2.md 31.10 carry the correction, iaus-sim-v2.md 30.7 finding 4 + World.h `g_blowStands` comment, against a645ca2, same review, finding 3)
Both still say NoteBlow releases the stance when the blow is PICKED; main's 3db7882 keeps it until the blow resolves. Q-SIM-2 is marked resolved in the questions file; the two texts still need the same correction.

### MFO-B785 (SEV-4, CLOSED by T2 batch S: the Detect.h comment and iaus-sim-v2.md 31.10 state the provenance, tools/iaus_sim/Detect.h ApplyProfile("steep"), against a645ca2, same review, finding 4)
Every GMST in the "steep" profile equals LoreRim.sim.json exactly (alias `lorerim-like`), while the doc and comment call it hand-written and not read from a list. Lab-only, nothing in native/ reads it, but the provenance claim is wrong next to the no-list-specific-data rule. Say where the values came from or make them generic.

### MFO-B786 (SEV-5, OPEN, tools/iaus_sim/Policy.h Fire, against a645ca2, same review, finding 5)
The lab's "blow" families for releasing the stance claim leave out Family::Exploit, which native IausSource.cpp:341-342 includes; only `stanceHeld` bookkeeping differs.

### MFO-B787 (SEV-5, OPEN, tools/iaus_sim/Policy.h PreparePick, against a645ca2, same review, finding 6)
A re-pick overwrites the current pick at once; native keeps the old pick while the new job is pending, so BeginRun can use a point not yet "landed" at the edge of the 1.5 s window (~0.1 s optimism).

### MFO-B788 (SEV-5, CLOSED by T2 batch S: stated in World.h Level and iaus-sim-v2.md 31.10, tools/iaus_sim/World.h Level, against a645ca2, same review, finding 7)
`in.flying = outOfMeleeReach` stands in for "flying", giving a 3x max distance to any actor out of melee reach (a ledge). Errs pessimistic for the follower; unstated.

### MFO-B794 (SEV-4, OPEN, logistics/TheftGate.cpp ScanGuards (VR, no pump), against 41ab1ec, Opus review of fix/theft-gate, finding F5)
When the main-thread pump is not installed (VR) `GuardCheck` runs `ScanGuards` on the worker, which walks `ProcessLists::highActorHandles` and runs the sightline off the main thread (INVARIANTS #72, the crash4 class). Today the VR looting road is mostly inert (ActivateRef has no pump) but the container strips still pass the gate. Fix shape: on VR, answer Seen/Pending-forever (fail closed) instead of scanning.

### MFO-B795 (SEV-5, OPEN, logistics/TheftGate.cpp Takeable / Analyse, against 41ab1ec, same review, finding F6)
`Takeable` runs a filtered `GetInventory` per candidate item, so a scan of one body costs O(items^2) inventory builds on the worker. Fix shape: one inventory pass per source per call, caching the per-object verdicts.

### MFO-B796 (SEV-5, OPEN, logistics/TheftGate.cpp g_guard / g_rings / g_logAt, against 41ab1ec, same review, finding F8)
None of the gate's three maps is cleared on a revert/load (FormID keys of the old session; the guard cache ages out in 2 s, the ring and log-throttle maps are bounded but stale). Fix shape: a `TheftGate::ClearTransientState()` called from `Logistics::ClearTransientState`.
### MFO-B797 (SEV-4, OPEN, Board_Classes.cpp PassiveLabel :45-50, against 2b7736d, Opus review of fix/aversion-tune, doubt 2)
PassiveLabel(rank) shows Cl_PassiveQuarter "passive: 1/4" for any class at kRankMasteredUnequipped. Aversion is effectively the only Conjurer/Healer passive and now gives none when mastered and unequipped (Q94), so the label misleads for those two. Fix is logic, not a string: pass the class id and return no/none label for Conjurer/Healer at kRankMasteredUnequipped.
### MFO-B809 (SEV-5, OPEN, Harbinger core/CombatStyle.cpp:494 Heartbeat WriteSlot, against 6b297a2, Opus re-review of the batch M fix round, F5 note)
Heartbeat now runs WriteSlot on every redecide even when nothing changed, opening a few-store seqlock window every 5 s per claim (the old code did one atomic store); a reader preempted inside it misses one read (gets the engine's own style). Optional: skip the write when the flags are unchanged. Also: APMF-B298 item (3) (controller lifetime across EngineRefresh's UpdateCombatStyle) must be CLOSED before the release cut (re-review verdict: not a merge blocker).

### MFO-B810 (SEV-4, OPEN, logistics/Steal.cpp Evaluate, against batch-l, author note for the Opus review)
The witness scan walks `ProcessLists::highActorHandles` on the main thread and calls `SenseOf` twice per NPC in range (follower, player). Harbinger caps synchronous sight rays at 8 pairs per frame; the rest come back deferred and the verdict is Pending until a later frame reuses the stored measurement. In a crowded town a steal can stay Pending for several 400 ms rounds. Fail-closed by design; the cost is latency, not safety. Fix shape: a per-follower witness-set cache refreshed by distance.

### MFO-B811 (SEV-5, RESOLVED in the fix round, logistics/Steal.cpp, author note)
Superseded: animals are now counted as witnesses (no skip), so a wildlife-heavy exterior refuses more. Revisit when RE shows whether the engine's witness loop skips animals.

### MFO-B812 (SEV-5, OPEN, logistics/EquipAuthority.cpp CarriesOnClass, author note)
`CarriesOnClass` is cached 1 s per follower; a weapon picked up or dropped inside that window is judged one second late. The deny list re-sends on change, so it converges. Also the cache and `OffClassTick`'s held map are not cleared on revert/load (FormID keys).

### MFO-B814 (SEV-4, OPEN, logistics/Lockpick.cpp / Steal.cpp, Opus review of c05b277)
The lockpick idle clip may break the follower's sneak while the pick runs, which then fails the sneaking half of the witness pre-check at the write and skips an otherwise clear pick. Fix shape: re-assert sneak after the idle or accept the skip as designed.

### MFO-B815 (SEV-4, OPEN, logistics/EquipAuthority.cpp OffClassTick, Opus review of c05b277)
The player notice says he "fights with" the fallback weapon even when the ch.17 deny list is not enforced (Harbinger below ABI 33): the off-class rule then does not stop an in-set swap. Wording only; the damage penalty follows the held weapon, not the deny.

### MFO-B816 (SEV-4, OPEN, logistics/EquipAuthority.cpp CarriesOnClass, Opus review of c05b277)
A LOTD museum relic counts as an in-set weapon ("armed") although he may not wield it, so the fallback may not open for him.

### MFO-B817 (SEV-4, OPEN, logistics/Steal.cpp Evaluate, Opus review of c05b277)
The witness scan walks ProcessLists::highActorHandles only; an NPC in the low or middle-low process list inside the radius is not asked.

### MFO-B813 (SEV-5, OPEN, logistics/Steal.cpp, author note)
The steal arrival pre-check is per loose ref; the scan only applies the source bars. A follower may walk toward an item in a crowd and be refused at arrival (the leg is then transient-blocklisted 25 s). A scan-time witness read per candidate would stop the walk but multiplies SenseOf calls.
### MFO-B820 (SEV-4, OPEN, iaus/Reposition.h SpellRange / kMinimumFrac, raised by the author against feat/batch-i, 2026-11-12)
The 0.4 minimum, the 0.8 / 0.9 factors and the 15 percent close slack are the brief's words plus a chosen fraction; none is measured. "Cone" is read as "concentration" (the record has no cone flag), so a fire-and-forget cone (Fire Breath style) is treated as aimed. The sim tunes them (batch S models H4 ranges).
### MFO-B821 (SEV-4, OPEN, apmf/RangeApproach.cpp, same branch)
The ch.24 close is `kApproach_Hold` X = foe, R up to ~1800 units. Hold on a FOE with a large R was only field-proven for the held-heal and occlusion approaches (R about 1024 / 128). Unverified: that a large R does not pin him to the foe's side, and how the Return To Combat Area behaves when the foe flies. The `[range]` lines plus Harbinger's ch.24 log answer it in the field. A claim nobody refreshes is released after 3.5 s; an `Arrived` Hold stands until then.
### MFO-B822 (SEV-4, OPEN, classes/StealthEntry.cpp + iaus/Candidates.cpp, same branch)
The free entry reads `Hide::Verdict` Unknown as unaware on the guaranteed fight. CORRECTED (review F3 of dcca7d7): Hide::Verdict already answers No when a foe's group has no record of him on EVERY fight; Unknown only means the foe has no combat group this lap, so the guaranteed fight is near-inert. A real guarantee needs the H10 invisibility lever (Q-I-4, re-asked). The counter counts only fights the IAUS fight clock saw (Sentient AI followers); a table-mode Thief never counts.
### MFO-B823 (SEV-5, OPEN, Board.cpp (2100+ lines), Board_Classes.cpp, same branch)
Board.cpp is past the ~1500 line "plan a split" mark; the Teach Spells batch added a 25 line helper (`TeachSpellNow`) there instead of splitting (scope rule 1). A split brief should move the follower-row snapshot builder. The page shows for casting hybrids too (Q-I-7).
### MFO-B824 (SEV-5, OPEN, iaus/Prune.h EffectWeighable, same branch)
The unweighted test is by archetype id only (Script, scripted spawn, kNone, past the last known). It does not look at magnitudes or at whether the engine fills ran, so an unusual-but-known archetype (a Rally, a Banish) stays a candidate scored on its nature alone.
### MFO-B825 (SEV-4, OPEN, Concealment.cpp Kind::Step / BackOff, same branch)
Both runs use the engine's own Return To Combat Area, which runs at the combat gait (the lab's x0.55 sneak gait is not applied by the engine to it, G3's open P1 question). A Step may therefore be noisier than a sneak walk; the engine's detection decides. The Step's pick has 10 candidates and no cover test (staying hidden is the engine's verdict, checked every lap by the run). A back-off to a point the engine cannot path to ends as a failure at 5 s.
### MFO-B826 (SEV-5, OPEN, iaus/Catalogue.cpp BackOffNeed / aw(Step) / aw(BackOff), same branch)
BackOff weight 1.0, Step weight 0.9, the Mercenary need table (0.6 / 0.8 / 1.0) and the 6 s / 3 s cooldowns are chosen, not searched. Block's score (BlockNeed with blockBase 0) is 0 whenever the time-to-live estimate is unknown, which is the default (the engine fills are OFF), so Block NEVER fires with the fills off (no candidate: `Combatant::swinging` is filled only with `bIausFillSwing`): Q-I-8.
### MFO-B827 (SEV-4, CLOSED by T2 batch S: RealClasses.cpp sets `block` and `blockCooldownSec` from the class row, CLI check added, tools/iaus_sim, against dcca7d7, Opus review F5)
The sim's ClassGate never sets `block`, so `--roads block` is dead after batch I. Batch S must set `ClassGate::block` (and `blockCooldownSec`) from the class rows' `act.block`.
### MFO-B828 (SEV-4, OPEN, iaus/Reposition.h MageBackOffNeed, against dcca7d7, review F6)
`MustClose` reads `hit` / `hitIsMagic`, which are filled only with `bIausFillHit`; without it a mage's back-off can fire on archers and casters. Fix shape: require a known melee foe for the mage need.
### MFO-B829 (SEV-4, OPEN, iaus/Candidates.cpp fight clock, against dcca7d7, review F8)
A combat flicker (gap over kFightGapSec) starts a new fight, so it counts one for the C13 counter.
### MFO-B830 (SEV-4, OPEN, Concealment.cpp g_picks, against dcca7d7, review F9)
Hide, Flee, BackOff and Step share ONE pick slot per follower; Build asks for one kind per lap (hide/flee > back-off > step) but a changing want re-picks every 1.5 s.
### MFO-B831 (SEV-4, OPEN, Concealment.cpp Kind::Step bow pick, against dcca7d7, review F10)
A bow Step candidate must still be at least 100 units nearer the foe, although only a line of fire matters; a line from the same distance is refused.
### MFO-B832 (SEV-4, OPEN, apmf/RangeApproach.cpp ServiceRangeClose, against b0b7bb0, re-check of batch I)
`ServiceRangeClose` has no `GroundStepStands` check: while a give-ground step stands (the earlier or later claim), the close can re-file and `RequestGroundStep` releases it again, so the close churns for the step's 2.5 s. Fix shape: ask whether a ground step stands before filing, as it asks `HealApproachStands`.
### MFO-B833 (SEV-5, OPEN, tools/iaus_sim/Data.h Classify + tools/list_balance, T2 batch S 2026-11-12, author's note; lowered from SEV-4 after the review of 0880667)
The lab's `SpellEntry::reach` is the record's range from 300 up, else 4096 as a stand-in for the projectile's range (never extracted). H4 is modelled as a bound (`min` with the lab's own standoff, review F2), so the stand-in no longer moves a mage's distance. It still feeds the mage back-off's minimum (0.4 of 0.9 x reach). Fix shape: extract the PROJ range in `tools/list_balance` and read it in `Classify`. Not a native/ defect.
### MFO-B834 (SEV-4, OPEN, native/classes/ClassTable.cpp:212 kNecromancerG, T2 batch S, found while modelling G7)
The Necromancer's corpse row is still `Todo(kTodoCorpse, ...)` although card G7 (`cond.corpse_within`) is merged, so his default set does not get it. T3, not a T2 gate. Also from the lab: a corpse row rarely fires from a table (magicka is spent by the first kill, or the spell's level cap is under the corpse's), and IAUS has no Reanimate family (`iaus/Prune.h`: nature Unknown).
### MFO-B835 (SEV-5, OPEN, tools/iaus_sim World.h TryDodge / Land, T2 batch S, author's note)
The dodge roll (one per projectile, 0.2 s flight gate, clears a non-area projectile) and the uncorrected miss chance (12% against a moving target) have no field anchor. The `[aim]` probe and field step 24x give the numbers to replace them. Foes do not dodge by their own style in the lab (they keep the flat miss).
### MFO-B836 (SEV-5, OPEN, selftest/TestsT2.cpp t2.theft_gate part d, against bb0a728, Opus review S4)
`Steal::CheckNow(f, pc, ...)` in the dev test writes a `[steal] F=<fol> item=00000007(...) ... verdict=REFUSED:the follower is not sneaking` line (the player ref is the stand-in item) and a Steal cache entry. Not distinguishable from a field line except by item=00000007. Grep field logs for `[steal]` lines with item=00000007 as selftest noise. Fix shape if wanted: a dev-only tag parameter on `Steal::Say`, outside selftest, so left.
### MFO-B845 (SEV-5, OPEN, iaus/Reposition.h touchSpent, pre-T2 batch, author's note)
`Snapshot::touchSpent` reads `Mind::firedAt` (the FIRST fire of the running decision) against a 3 s window, so a touch cast that keeps running as the same decision stops counting after 3 s and the range flips back to the touch spell. Fix shape: stamp the last cast time per form. The 3 s window (`kTouchSpentSec`) is CHOSEN, not searched. The touch range now follows the cast IAUS chose on its LAST decision (`Snapshot::chosenCast`, `Candidates::NoteDecision`), not a damage proxy (marth: touch range only when the touch spell is the chosen cast).
### MFO-B846 (SEV-5, OPEN, tools/iaus_sim World.h handSpell2, pre-T2 batch, author's note)
The lab's second hand is "the spell cast before the last one"; a real follower's two hands are an equip choice (Loadout), so the Q-I-2 model is an approximation and the built-in fixtures hardly exercise a touch + distance pair.
### MFO-B847 (SEV-5, OPEN, iaus/Generate.cpp Guards, Q-S-5, author's note; reworded after the review of 24b4421)
The guard line counts foes under a live taunt of HIS OWN (`Combatant::tauntedByMe` = `Taunt::TauntedBy(foe, self)`, matching the sim's `tauntBy == him`) that have him as their target. Another follower's taunt cannot pin a foe to a different follower (marth: impossible), so no other-taunter case exists. Unverified in the field; the ceiling of the share is Harbinger's guard constants (2.5 s hold, one renewal, 1.5 s rest).
### MFO-B848 (SEV-5, OPEN, iaus/Catalogue.cpp BoostGuardLine + Generate.cpp Guards, Q-S-5, author's note; reworded after the review of 24b4421)
`BoostGuardLine` x2.0 is flat above 1.5 in the lab. By weight alone a held guard (2.0 x 1.25 momentum = 2.5) would beat the health potion (2.2 at most), so the guard line is switched OFF in `Generate` (`tauntedOnMe` 0: no boost, no full need) when `selfHp < kDrinkHealthLine` (0.34, DrinkNeed's midpoint) AND a health potion is in stock; the Block then needs a seen swing and a normal need, and the potion wins. NOT covered: a Knight with no potion (the guard line stands), and a heal CAST that the NoHealDrink gate prefers. The 0.34 line is the existing DrinkNeed midpoint; the exact line will come from the sim drink-curve study.
### MFO-B849 (SEV-5, OPEN, iaus/Reposition.h MageBackOffNeed touchSpent, against 24b4421, review of the pre-T2 batch)
The back-out after a touch cast ends early when a new decision becomes the running one (touchSpent reads `Mind::running.form == touch form` within 3 s of `firedAt`; a different running decision after the cast clears it, so the back-off need drops before he is out). Fix shape: stamp the last touch cast time apart from the running decision.
### MFO-B840 (SEV-4, OPEN, native/logistics/Service.cpp owned-chest arrival + Steal.cpp TakeOwnedContents, feat/owned-pick, author's note)
An owned chest whose steal take is CUT (a witness walks in) stays unlocked with its contents: act.pick_locks only seeks LOCKED chests, and the loot rules bar every owned copy, so it is not revisited. Items already moved stay moved (no refund, by ruling). A later pass could re-admit an unlocked owned chest that still holds steal kinds through Steal::Check.
### MFO-B841 (SEV-5, OPEN, native/logistics/LootScan.cpp pickOwned, feat/owned-pick, author's note)
act.pick_locks admits a locked owned chest on "not empty", exactly as for an unowned one: a chest holding only gear or ammo is picked (lockpicks spent) and yields nothing on the steal road, which takes the Q-L-1 kinds only. A peek for a steal kind before walking would save the pick.
### MFO-B842 (SEV-5, OPEN, native/logistics/Steal.cpp OwnedTakeOnMain, feat/owned-pick, author's note)
(RESOLVED in the review round after 746b755, F5: one Steal::CheckNow per take, before the first RemoveItem; all RemoveItems run in that one main-thread call.)
### MFO-B843 (SEV-5, OPEN, native/logistics/Steal.cpp + Service.cpp owned chest, against 746b755, Opus review F6)
Raised by the Opus review of feat/owned-pick 746b755 (finding F6). Verbatim: "an abandoned in-flight entry is replayed on a revisit: if the excursion ends between Finish and the next read the entry stays; a later visit to the same chest returns that old Took/Cut answer once without taking anything; a different chest replaces it; harmless, one wasted visit"
### MFO-B844 (SEV-5, OPEN, native/logistics/Steal.cpp + Service.cpp owned chest, against 746b755, Opus review F7)
Raised by the Opus review of feat/owned-pick 746b755 (finding F7). Verbatim: "owned copies skipped for carry weight can still be moved by the TheftGate::Take(..., Road::Container) fallthrough because TakeBarred sees no owner on them; that fallthrough skips the carry-weight check"
### MFO-B850 (SEV-4 x3 + SEV-5 x2, RESOLVED, native/logistics/Steal.cpp OwnedTakeOnMain / OwnedChestBarred, against ad1e08b, Opus spot review)
(RESOLVED in the round after ad1e08b, "fix now, light tier A", marth 2026-11-12.) The reviewer's findings, from its log: "SEV-4 leak on merge path (ABI carve-out note), SEV-4 xl path silent launder when owner null, SEV-4 F2 cell-owner gap, SEV-5 comment wrong, SEV-5 16-cap spin/skip." and "Existing entry: compare 11594 (skips ExtraCount 0x24, compares Ownership 0x21); on equivalent list it adds count to the existing list and does NOT link or free the caller's list (cmp rsi,[rsp+0xf0] at 0x227189) => LEAK of xl2 on repeat steals." Fix: a merged list is destroyed with the engine's destructor + free behind the new self-check row `Steal.ExtraDataList.Destroy`; the extra-list road warns and labels `unstamped`; `OwnedChestBarred` reads the effective (chest, else cell) owner; the chest's count is compared around every RemoveItem.
### MFO-B851 (SEV-5, RESOLVED, native/logistics/Steal.cpp stamp seat, against the B850 fix round, author's note)
(RESOLVED in the round after 05df82f, marth 2026-11-12 "Fix before release".) Was: on 1.7.104 the stamp road was refused and a plain stack stolen from an owned chest moved UNSTAMPED. Now 1.7.104 is mapped (constructor id 11583 @0x157420, destructor body raw 0x156960, size 0x20, the add's merge rule identical to 1.6.1170) and the stamp runs there; a refused seat on any build leaves plain stacks IN THE CHEST, never moved unstamped. The fork still lacks the 1.7.104 size and the destructor id: `_commonlib/fork-plan-2026-09-23/fork-additions-queue.md` rows 1-3, READY TO LAND; after it lands the raw row becomes an id row.
### MFO-B852 (SEV-5, OPEN, native/logistics/Steal.cpp WhereListLanded, against the B850 fix round, author's note; widened after the Opus review of 05df82f)
Two ways one ExtraDataList (0x20 / 0x18 + its ExtraOwnership) is not reclaimed, both logged, both chosen over a possible double free. (1) 1.6.1170 / 1.7.104: `Landed::Unknown` (the new list is neither linked into the follower's inventory nor merged into a list he held) leaves the list alone with an ERROR, because a container-changed sink of another plugin runs inside the add and may move the item on with the list. (2) 1.5.97: the engine destroys a merged caller list itself, except while BarterMenu is open (15815 @0x1DA46F), when it drops it unfreed; Steal.cpp never touches the list after the add there, so that one leaks as the engine's own callers' lists do. Neither observed. If the ERROR of (1) ever appears in a field log, read which sink moved the item before changing this.
### MFO-B853 (SEV-1, RESOLVED, native/logistics/Steal.cpp DestroyUnadoptedList on 1.5.97, against 05df82f, Opus spot review)
(RESOLVED in the round after 05df82f.) The reviewer's finding, from its log: "SEV-1: SE 15815 @0x1DA46F->0x1DA491 after merge frees r14 (= caller list, no guard) unless menu (79937). Our Merged destroy = double free on 1.5.97." Fix: a merged list is destroyed on 1.6.1170 and 1.7.104 only; on 1.5.97 the list is never touched after the add. Also from that review, SEV-5: the moved count on the plain road is clamped to the copy's count.
### MFO-B854 (SEV-4, RESOLVED, native/logistics/Steal.cpp OwnedTakeOnMain, against 1ec94fd, Opus review ownedpick-r2)
(RESOLVED in the round after 1ec94fd, marth 2026-11-12 "a stolen item is NEVER laundered".) The reviewer's finding, from its log: "SEV-4 spot4 launder": a PLAIN stack whose owner is null (cellOwnedOnly with no cell owner, or TakeBarred bars it but the chest owner is null and the chest is not off-limits) or dynamic (SetOwner refuses) still moved UNSTAMPED, and so did an extra-listed copy whose owner could not be stamped. Fix: the unstamped-move branches are removed; such a copy STAYS IN THE CHEST (`leftInChest`, one warn naming item and reason).
### MFO-B855 (SEV-5, OPEN, native/logistics/Steal.cpp OwnedTakeOnMain, against 1ec94fd, Opus review ownedpick-r2)
Raised by the Opus review of 1ec94fd. Verbatim (the brief's wording of the finding): "refused seat -> chest still a pick candidate -> follower revisits it (Service.cpp:1031-1043 path)". A chest whose plain stacks stay because the ListSeat is refused (or an owner cannot be stamped) is still a pick / take candidate, so the follower can walk back to it again. Fix shape: remember such a chest as spent for the session.
### MFO-B856 (SEV-5, OPEN, native/VerifiedAddresses.h comment, against 1ec94fd, Opus review ownedpick-r2)
Raised by the Opus review of 1ec94fd. Verbatim (the brief's wording): "stale VerifiedAddresses.h comment (generator output lists Steal.ExtraDataList.Destroy as refused on 1.7.104 though the raw row covers it)". The comment is generator output (tools/verified_addresses/gen_verified_addresses.py); the 1.7.104 destructor is covered by the raw row Steal.ExtraDataList.Destroy.1_7_104. Fix in the generator and regenerate, not by hand.

### MFO-B857 (SEV-4, OPEN, native/Packages.cpp EnsureEvictMarker, against fd948554, read-only dangling-state audit 2026-11-15)
Raised by the read-only dangling-state audit (agentlogs/dangling-audit.md), verbatim: "Packages.cpp:1570-1588 `PlaceObjectAtMe(XMarker, true)`, called from plugin.cpp:524. The handle `g_evictMarker` (:1277) is not co-saved, so a load of an older save mints again. Each MFO.log mints one per session. _evidence/2026-11-01-lorerim/MFO.log.1 mints FF00283C, then FF003F8A, across loads in ONE session. Effect: orphan persistent refs pile up in the save chain. That is save bloat, not a crash. All runtimes." Deferred by fix/save-integrity-mfo (its brief: "If this needs a co-save record change, STOP and report instead"). Why it was not fixed there: the marker can only be found again by its FormID, which only a co-save record can carry across a game restart, or by shipping one persistent XMarker ref in MFO.esp (a generator change). A find-existing without either (remember the minted FormID for the rest of the process) covers only a reload inside one session and can adopt another mod's runtime 0x3B marker that happens to hold that id, so it was not shipped. Proper fix (its own brief): co-save the marker's FormID and re-find it after load with the base-0x3B / not-deleted check, as Harbinger poscast does (PositionCast.cpp:425-464), or the ESP ref. Old orphans from earlier sessions cannot be identified either way.

### MFO-B859 (SEV-3, OPEN, threading: what INVARIANTS #83's 2026-11-16 extension leaves on the worker; raised by the author of `fix/worker-writes-main`, 2026-11-16)
Cross-referenced from MAP.md "ENGINE WRITES RUN ON THE MAIN THREAD" ("What breaks" 6) and INVARIANTS #83 ("What still runs on the worker"). `fix/worker-writes-main` moved every worker-side engine write it could move without changing a synchronous read-back. These stay on the AddTask worker, each named so the next agent starts from them. CLAUDE.md rule 9 carve-out (a) applies (threading): this is its own tier-A brief, not a drain item.
- **RESOLVED 2026-11-16 by `fix/alias-ops-main` (INVARIANTS #83 rule 9): every fill and eviction is one alias op run on the main thread (`Packages.cpp` `AliasOp` / `RunAliasOp` / `PostAliasOp`), the read-back and the roll-back are inside it, the occupancy tests of `LootTravelEvictIf` / `RetreatEvictIf` moved into it, `ReleaseAll` keeps its synchronous order, and the inner calls were read on all three binaries (ENGINE_NOTES 0.30 amendment 2026-11-16c: the fill writes the actor's change flags, extra list, AI process package state and the persistence manager).** Was: **(SEV-3) the quest alias fills.** `Packages.cpp` `ForceRefToNative` calls `TESQuest::ForceRefTo` (1.6.1170 0x3CDEE0, id 25052 / SE 24523) from the tick: the cast holder fill and release (`ClearAlias`), `LootTravelFill` / `LootTravelRetarget` / `LootTravelClear`, `RetreatFill` / `RetreatClear`. Read so far: the function holds the quest's alias write lock around the edit (0x3CDF41 to 0x3CE18A) and a read lock around the alias lookup; its inner calls (0x3CDB00, 0x398D20, 0x3D1A00, 0x3D4870, 0x3A1770) were NOT read, so what they write on the reference (its `ExtraAliasInstanceArray`, the persist flag, a package re-evaluation) is not proven worker-safe. Not moved because the callers read the fill back synchronously ("alias filled NATIVELY (synchronous)", `VerifyDetachedFrom`, the roll-back when the second fill fails) and the release order is CLAUDE.md "five things" 3. Fix shape: post the fill with FormIDs, move the read-back and the roll-back into the posted body, keep `ReleaseAll`'s order. The `EvaluatePackage` that follows each fill already runs on the main thread (posted behind the fill in time, since the fill is synchronous on the worker).
- **RESOLVED 2026-11-16 by `fix/alias-ops-main` (INVARIANTS #83 rule 9): the cast package's inputs are written inside the alias op on the main thread, each field only when it differs. The location / target references (`SetAPMFLootTravelTarget`, `SetPackageLocation`, `SetPackageTargetRef`) stay on the caller's thread as ONE aligned 32-bit handle store (`StoreLocationRef` / `StoreTargetRef`): the union's type is authored as 0 in every such package and is refused off the main thread if it ever differs, and the radius is its own store written only when it differs. They are not posted because the Harbinger claim that hands the package out is filed right behind the write.** Was: **(SEV-4, regraded from SEV-3 by the Opus review of ca0465c9, F5: the writes are aligned single-word stores) writes into MFO's own package forms.** `Packages.cpp` `SetPackageLocation` (:2504) and `SetAPMFLootTravelTarget` (:710) write `loc->data.refHandle`, and the cast package's input mutation writes the spell and target into the package's custom data, all on the worker, while the actor's AI may read that package on the main thread or on an AI job. They are MFO's ESP forms and the writes are single words, but nothing proves a reader cannot see the write half-way through a multi-field change. Same fix shape, same brief as the alias fills.
- **(SEV-4, confirmed by the Opus review of ca0465c9 as F4, "power attack graph road answers Fired early") `cast/Fire.cpp` power attack, `road=graph`:** the animation event is posted, so the worker answers Fired before the graph has accepted it. It pre-tests the attack state (the first refusal); any other graph rejection is a main-thread WARN and the rule has spent its suppression window on a swing that did not play. Only where Harbinger does not serve ch.31 (1.7.104 today) or with bPowerAttackEngineRoad=0. Fix: feed the rejection back to the worker (an atomic per follower the next lap reads as FailedOther).
- **(SEV-4) `EngineOps::Equip`'s posted re-check** refuses an equip when the live count is 0 on the main thread. The worker's ledger (`g_forcedWeapon`, Loadout's debt) has already recorded the hold. The existing reconcile paths notice a hold that is not in the hand, but no code reads the `[engine-op]` refusal back. Rare (the item must leave his pack inside one frame).
- **(SEV-5) `Probe.cpp` `Fire`** has no caller. Its actions (StartCombat, StopCombat, CastSpellImmediate, EvaluatePackage, DrawWeaponMagicHands) would be worker-side engine writes if anything called it from the tick. Delete it or route it through `MainThread::RunOrPost` when the harness is next touched.
- **(SEV-5) the sink tasks in `Diagnostics.cpp`** (the hit / spell-cast / combat sinks' own AddTask closures under `PumpTickGate`) are not inside `WorkerTickScope` and are not exception-guarded. They make no engine write (ledger notes only); a throw there still unwinds through the SKSE task.
- **(SEV-5) `selftest/Fixtures.cpp`** (dev builds only) spawns, moves and starts combat on its fixtures from the worker.
- **(DONE 2026-11-16, `fix/alias-ops-main`)** **The reviewer's plan for the alias fills and the package-form writes (Opus review of ca0465c9, relayed in the coordinator's fix brief):** "alias ops posted with generation, Papyrus-road style pending/read-back, ReleaseAll synchronous". That is: each fill / clear is posted to the main thread carrying a generation; the worker keeps a pending entry and reads the result back on a later lap (as the Papyrus dispatch road does), never synchronously; `Packages::ReleaseAll` (kPreLoadGame / post-load / revert, "five things" 3) stays synchronous on the main thread with its order unchanged.
- **Opus 5.5 review of ca0465c9 (2026-11-16), SEV-5 findings deferred by the coordinator (rule 9). Wording from the coordinator's fix brief (the reviewer's report was relayed, not filed). The bold title is the brief's wording, the sentence after it is the fix author's reading of the code:**
  - **(SEV-5, OPEN) the posted teach's requeue overwrite.** `Board.cpp` `TeachSpellNow` posts the teach and queues the SAME edit again for the next drain; a newer edit to that row queued in between can be overwritten by the requeued one.
  - **(SEV-5, OPEN) bDriveCaster `ArmGrace` / `StartCooldown` on a main refusal.** `cast/CastOn.cpp`'s drive probe arms its grace and starts its cooldown on the worker when it posts; a CheckCast refused on the main thread still spends them.
  - **(SEV-5, OPEN) `DrinkBest`'s per-AV gate is armed after a main refusal.** `logistics/Service.cpp`: the worker arms the per-actor-value drink gate when it posts `EngineOps::Drink`; a drink the main thread refuses (he cannot act, the potion is gone) still holds the gate.
  - **(SEV-5, OPEN) the board hotkey and the Field Orders power collapse into one toggle.** `Diagnostics.cpp` `RequestBoardToggle`: both requests pressed inside one tick are served as ONE `Board::Toggle` (the power's wins), where two closures used to toggle twice.
  - **(SEV-5, OPEN) MAP.md's `CastOn.cpp:1815` reference points 20 lines high** (the drive probe moved in this branch).

- **Opus 5.5 review of 9ea0ec36 (2026-11-16), deferred findings (wording from the coordinator's fix brief, raised against 9ea0ec36):**
  - **(F3, SEV-4, OPEN) the re-arm claim is wider than the code.** `StartPump`'s `RearmStoppedStages` clears the stop flags and ResetAllState clears the save-scoped state, but process-lifetime statics in `Probe::Tick`, `AtkObserveTick` and the Board publish caches are not reset, so a re-armed stage may read state it half-wrote. The wording is fixed (`Diagnostics.cpp` stage comment, MAP.md); resetting those statics is backlogged.
  - **(F5, SEV-5, OPEN) gated sink bodies are dropped for the whole engine save window.** `PlayerGiven.cpp:101-108`, `Rapport.cpp:125`, `logistics/Sinks.cpp:220`, `Followers.cpp:458` bail at `PumpTickGate` while the save hold / pause stands; only the Classes `LandPicks` retries.
  - **(F6, SEV-5, OPEN) `MainThread::DrainForSave` accepts a call from inside the seat.** It should refuse in seat phase 2.
  - **(note, DONE 2026-11-16 on Harbinger `fix/save-drain`) Harbinger's `apmf::mainthread` queue is not drained at kSaveGame.** Harbinger-side follow-up.
- **Raised by the author of `fix/alias-ops-main` (2026-11-16), OPEN:**
  - **(SEV-4) a posted loot / retreat fill that fails on the main thread is not fed back to the worker.** `Packages.cpp` `LootTravelFill` / `RetreatFill` (legacy alias road, Harbinger absent) return "posted". If the op fails on main (a reference stopped resolving in the frame between) it is an ERROR line, the aliases are rolled back, and the caller finds out only through its own watch (loot: the leg never engages and its engage window ends it; retreat: the travel timeout). The cast holder does get the failure (`g_castFillFailed`). Fix shape: a per-slot / per-follower atomic the next lap reads.
  - **(SEV-5) `Packages.cpp` is about 2900 lines** (it was 2556 before this change, past the 2500 backstop that is relaxed until the next major release). The alias op and the package-input writers are a natural `packages/` split; its own brief.
  - **(SEV-5) the names of three AI-process callees of the alias fill were not identified** (1.6.1170 0x6D5690, 0x713CE0, 0x6E32F0, called on `actor->currentProcess` right after `Actor::EndInterruptPackage`). They take no lock and run on the actor's process; that was enough to decide the thread, not enough to name them.


### MFO-B861 (SEV-3, items 1 and 2 DONE 2026-11-16 by `fix/thread-seed-followups`; the friendly-fire walk DECISION stays OPEN as its own item; SEV-4 NoteSeatThread count DONE (`[overlap]` prints "main-thread record != player Update thread (adoptions)", must be 0); original header follows: threading carve-out: two private "main thread" ids are the kDataLoaded thread; raised by the author of `fix/main-thread-id`, 2026-11-16, against that branch; MAP "CasterConsent.cpp", "iaus/Fills", "MainThread.cpp"; INVARIANTS #84 rule 8; ENGINE_NOTES 0.55)
- **Finding.** SKSE sends kDataLoaded from the engine's InitTESThread, not from the main thread (ENGINE_NOTES 0.55, disassembly of both SKSE builds). `fix/main-thread-id` fixed `MainThread`'s own id. Two older copies still record the kDataLoaded thread as "main" and were LEFT AS THEY ARE (outside that brief: fixing the first one turns a dead gate on):
  1. **`CasterConsent.cpp` `g_mainThread`** (set in `InstallCheckCastHook`, "InstallHook runs at kDataLoaded (main)"; read by `CheckCastThunk`'s friendly-fire hold, `std::this_thread::get_id() == g_mainThread`). The recorded thread is gone once the data load ends, so the test is never true: **the Harbinger-absent (legacy) friendly-fire hold of the pre-charge CheckCast gate has never evaluated `WouldHitTeammate`**, and the one-time `[consent] CheckCast fires on ... thread` line always says "a NON-main". It fails open (the cast is allowed), so nothing crashes and nothing is corrupted. With Harbinger the hook is not installed (ch.25 CastGuard does the hold), so the field lists are not affected.
  2. **`iaus/Fills.cpp` `g_mainThread`** (set in `StartupCheck` at kDataLoaded; read by the TESHitEvent sink for the probe's at-event health, `bIausFillProbe`, default OFF). Never equal, so `hpEvt` stays NaN in every probe line; the `[iaus-fill]` startup line prints the kDataLoaded thread as "main thread N" (the Deck log's `main thread 1908`).
- **Fix shape.** Both become `MainThread::IsMainThread()` (the one shared reader, principle 12) and the private ids go; the Fills startup line stops calling its thread "main". (1) makes the legacy friendly-fire walk live for the first time where CheckCast fires on the main thread: it needs its own decision and a Harbinger-absent field look (which thread CheckCast fires on is not known: no field log has the legacy hook installed). (2) is probe-only.
- **DONE 2026-11-16 (`fix/thread-seed-followups`).** (1) `CasterConsent.cpp`: the private id is removed; the walk is behind `constexpr bool kFriendlyFireWalkOpen = false` (the code of the walk is kept, field behaviour unchanged), and the one-time `[consent] CheckCast fires on ...` line now says the walk is DISABLED. **OPEN as its own item:** whether to open it via `MainThread::IsMainThread()` (turns on a path no field log has shown; CheckCast's firing thread is unknown, needs a Harbinger-absent field look). (2) `iaus/Fills.cpp`: `MainThread::IsMainThread()` gates the probe's at-event health; the startup line says "kDataLoaded thread N (the engine's InitTESThread, not the main thread)". (3) `MainThread.cpp` `NoteSeatThread` counts every mismatch (`SeatThreadMismatches()`), printed in `[overlap]`, so the log's 4-line cap can no longer hide one.
- **Why SEV-3 and not higher.** Both fail in the safe direction (no walk, no read). Why not lower: a dead safety gate that reads as live.
- **Also seen, not MFO's (Harbinger 0.20.5, report only):** `core/PapyrusCaller.cpp` `g_mainTid` is stored from the calling thread at `Install("kDataLoaded")` (the InitTESThread) and again at kPostLoadGame / kNewGame (the load thread), so it is wrong only until the first load; `hook::OnMainThread()` is learned from the player-Update seat only, so it is false until the first frame (SpaceQuery / Awareness / Sightline / the kSaveGame drain answer "not main" during the first kPostLoadGame).

### MFO-B862 (OPEN, author's own list for `feat/adopt-seats`, 2026-11-23, after Opus review round 1; MAP "ABI v46 SEATED HOSTILE ROWS")
1. **(SEV-4, accuracy)** Stagger / Paralyze immunity the engine keeps in code is not readable (race `kNoKnockdowns` is knockdown, not stagger; there is no paralysis resist flag): only MGEF conditions and the two-zero hold catch it.
2. **(SEV-4, accuracy)** The zero-effect reading: an instant effect (Stagger, Banish) is never judged; Disarm reads "the right-hand weapon seen at the fire is still there 2 to 6 s later" (a quick re-equip or a spare weapon can fool it); a lasting effect is read with `MagicTarget::HasMagicEffect` only (no active-effect source scan). The reading is driven by the claim's refresh laps and WillWork asks; if neither runs in the window nothing is counted.
3. **(SEV-5)** Banish's level cap (the magnitude as a level) is not in the WillWork level-capped set: not verified in the binary. The "essential" refusal for Disarm is the TARGET actor's essential flag (engine AE 0x678BD0 per the review), not read by this author. The three `fCombatMagic*Distance` GMSTs exist in all three executables (review) but are unused: they are margins, not ranges.
4. **(SEV-5)** The MGEF "keywords" of the brief are covered only as HasKeyword conditions inside the MGEF / effect condition lists.

### MFO-B860 (OPEN, author's own list for `feat/perk-effects`, ClickUp 1dfcuabd2pk, 2026-11-16; raised by the author before the Opus review; MAP section 11 "PERK EFFECTS")
What the perk-effect reader leaves open by design. None is co-save, ABI or a hook seat. Item 1 is threading-adjacent and is stated, not deferred.
1. **(stated, threading)** `PerkEffects::BuildLive` reads the base's perk array and each perk's `perkEntries` on the main thread while MFO's own allocator changes them on the main thread too (`SetLivePerkRank`, PollWork): same thread, no race. Any code that would mutate a TESNPC perk array OFF the main thread would break this reader and the engine alike.
2. **(SEV-4, accuracy)** The target of the two-argument shapes (EP35, EP29) is the follower himself (the engine's display-read convention). A bonus conditioned on the victim is under-counted for most followers and over-counted for a follower who is himself that kind of target (an undead follower with a perk against undead). A per-foe-kind table (ask against the current combat target's kind) is the fix shape; the damage estimate (`iaus/Fills.cpp`) already asks with the real target.
3. **(SEV-4, unused data)** `PerkRead::dualCast` (EP75 per school) is recorded and read by nobody. `Loadout.cpp OwnsDualCastPerk` still reads a vanilla perk form: moving it onto the table makes dual casting follow any perk overhaul. Its own change (it alters a cast decision).
4. **(SEV-5, unused data)** `PerkRead::blockPct`, `Table::abilityAv` (bow draw speed, weapon speed and the rest of the actor values perk abilities change) and `entryCount` are recorded and logged, not scored. Candidates: Block family by `blockPct`, posture by bow draw speed.
5. **(SEV-4, tuning)** The gear picks now carry BOTH the perk votes (`kStyleBias` 1.5, `kArmorPerkBias` 1.25: how many perk ranks name a kind) and the engine factor (how much those perks are worth). They can stack on the same perks. `t.perkArmor` / `t.perkWeapon` are PLACEHOLDER 1.0; a tuning pass decides whether the votes shrink or go.
6. **(SEV-4, sim)** `Cons::BoostSpellPerks` ships a hand PLACEHOLDER (x1.15 at edge 0.5) and the game now feeds `Cons::BoostPosturePerks`, whose curve stays flat. The simulator fills neither spell input; its mirror and the search are a Fable sim batch (design note section 9).
7. **(SEV-5, coverage)** One build asks at most 64 spells (`kMaxSpells`); a follower who knows more has the rest counted, not asked (the table line says "N of M known asked"). The generic item of a kind (asked when he carries none) may lack a material keyword a perk is conditioned on: that kind reads 1.00 until he owns a matching piece.
8. **(SEV-5, log volume)** The table line is re-printed on every change of its text, and the armor mean changes with every rated piece that enters or leaves his pack. Long looting runs print several lines per follower. Pace it only if a field log shows it matters (principle 8).
9. **(SEV-5, pre-existing, seen while here)** `tools/iaus_harness` fails two H4 checks on `origin/main` 43c075a2 ("a concentration (cone) spell = its range x0.8, no minimum", "a cone / touch spell wants the foe near: no minimum, no back-off"). Not from this branch.

### MFO-B858 (SEV-3 as a set, OPEN, what INVARIANTS #84 leaves open; raised by the author of `fix/worker-snapshots`, 2026-11-16, against the stability audit agentlogs/stability-audit.md)
Cross-referenced from MAP.md "THE WORKER SNAPSHOTS" ("What breaks"). `fix/worker-snapshots` moved every worker READ of an engine inventory / active-effect list to a main-built snapshot. These stay, each named so the next agent starts from them:
- **RESOLVED 2026-11-16 by `fix/worker-writes-main` (INVARIANTS #83 extended): container sources are queued like actor sources, the drinks / the tome / the board teach run on the main thread, and `Gear.cpp` `KeepHeadClear` and `ErrandSmith.cpp` the charge were found to run on the main thread already (their callers are posted).** Was: **(audit M2, SEV-3; counted at runtime as `[overlap]` "worker-side writes reading their own live list"):** two worker-side transfers keep a LIVE read because the write needs the live ExtraDataList: `TheftGate.cpp` `MoveNow` -> `AnalyseLive` for a CONTAINER source, and `GemRules.cpp` `TakeAndProbe`'s container road (the uid re-find, then `FindOwned`). Fix: queue container sources through the #83 queue as actor sources are. The other worker-side writes carry no live read of ours but are still engine writes off the main thread: `Service.cpp` `DrinkBest`, `PotionFx.cpp` `DrinkPotionForm`, `logistics/Cast.cpp` `LearnCarriedTomes`, `Board.cpp` `TeachSpellNow` (the PLAYER's pack), `Gear.cpp` `KeepHeadClear`, `ErrandSmith.cpp` the charge. Each calls `InvSnap::Touch` after it. When one moves to the main thread, replace its `Touch` by `InvSnap::RebuildOnMain` there (or nothing: a post made in the tick runs before the tick's build pass).
- **RESOLVED 2026-11-16 by `fix/worker-writes-main`: every equip, unequip, spell-hand and draw call goes through `EngineOps` (the main thread).** Was: **(audit M3, SEV-3):** worker-side equips (`cast/Equip.cpp`, `cast/EquipSilver.cpp`, `Loadout.cpp`) are not touched by this branch. Their decision reads go through the snapshot, so a WORN flag is one tick behind the equip call, as it always could be (the engine may queue an equip).
- **(SEV-4):** `MEOBridge::CarriedGems` (MEO's API) walks the follower's pack INSIDE MEO.dll. `GemRules::Check` with `a_live` calls it on the worker (`Gear.cpp` `KeepHeadClear`, the sale path's `CheckRemovalLive`). MEO is not ours: the fix is to route every `a_live` check through `MEOBridge::RequestCarriedGemRefresh` (posted).
- **(audit M6, SEV-4, left as is on purpose):** `ProcessLists::highActorHandles` is walked on the worker without a lock (`Followers.cpp`, `EngageOnSight.cpp`, `TheftGate.cpp` `ScanGuards` is main, `Steal.cpp` `Evaluate` is main, `cast/Corpse.cpp`; `CasterConsent.cpp` on the combat thread). Every handle is re-validated by lookup, so a reallocation yields a missed or wrong actor, not a fault. If the `[overlap]` line shows overlap, publish the array from the snapshot build pass (it already walks it on main for the effect mirror).
- **(SEV-4):** outside the tick (a stray `AddTask` closure, a Papyrus native) a party snapshot can read NOT YET in the frame after a worker-side write or just after a pause. The stateful entry points guard it (`InvSnap::Ready`). `Board.cpp` `TeachSpellNow` does not: a teach click in that frame reads "no book" and is refused (no state is lost, the click must be repeated). RESOLVED in the fix round on 35a8a778 (review F2/F4): the player's pack is an on-demand source slot now, and `TeachSpellNow` returns NotYet on an unread pack, queues the edit again and logs `[board] teach ... waits`.
- **(SEV-4):** "tick t reads everything tick t-1 posted" holds for a closure posted ONCE. A closure that posts itself again (`MainThread::Post([x] { MainThread::Post(x); })`: `Followers.cpp` restore, `cast/Equip.cpp` re-arm, the transfer drain under an open menu) lands a frame later, possibly after that tick's build pass, so the next tick can read a party pack one tick behind it (the same lag an engine-side change has). Only the transfer drain replaces the snapshots itself.
- **RESOLVED 2026-11-16 by `fix/worker-writes-main` (`TheftGate.cpp` asks `MainThread::EngineWritesAllowed()`; its own thread id is removed).** Was: **(SEV-5):** `TheftGate.cpp` keeps its own `OnMainThread()` (the drain's thread id, backlog F8 above). `MainThread::IsMainThread()` now records the pump's thread every frame and is the better source.
- **(SEV-5, OPEN, Opus review of ca0465c9, filed here by its fix brief; brief wording "AddSpell on main vs worker HasSpell reads", the rest is the fix author's reading):** `AddSpell` now runs on the main thread (the tome, the board teach) while the worker still reads `HasSpell` on the same actor's spell list live. A read racing the add can see the list mid-append. Fix shape: mirror the spell list in the snapshot build pass, or read `HasSpell` only in the posted body (the re-checks already do).
- **(SEV-5):** on the main thread `InvSnap::Items` builds the WHOLE pack (value, quest alias and all) where a filtered `GetInventory` copied one entry. `Lotd` `TransferOnMain` and `Equip.cpp`'s load-time `locked` do that per item. Low frequency, measured nowhere yet.
- **(SEV-5):** a snapshot holds `TESBoundObject*` / `EnchantmentItem*` / `TESForm*` (owner) pointers for up to a tick plus a reader's hold. They are forms, not list nodes. A created (0xFF) form the engine deletes inside that window would dangle exactly as it would have in the old code's shorter window.
- **(SEV-4):** `Diagnostics.cpp`'s tick gate skips the WHOLE body, bookkeeping included (`Packages::Pump`, `Taunt::Tick`), whenever the main thread has not run the previous tick's build pass: the tick after a pause ends, the tick after a follower joins or a load, and every second tick when a frame takes longer than a tick (133 ms, under 7.5 fps). Same class of event as the pacing return at the top of `Scheduler::Tick`, but new, and it makes the worker's rate follow the frame rate at very low fps. A paused tick is not gated: its bookkeeping (`Packages::Pump`, the reconciles, `Board::PublishSnapshot`) reads the party's published snapshots as they stand, which can predate a write the last unpaused tick posted if the pause began before that tick's pass ran (no follower decision is made in a paused tick). A run of 5 s of skips is logged. The `[overlap]` line counts the skips.
- **Opus 5.5 review of 35a8a778 (2026-11-16), deferred by the coordinator (rule 9). Wording from the coordinator's fix brief (the reviewer's report was relayed, not filed):**
  - **F3 (SEV-4, OPEN, `logistics/Service.cpp:565-579`, comment :548-553):** the coin-purse readback latch. The comment's premise ("one tick after the Activate dispatch ... the previous tick's call has had its frame(s) by now") no longer holds for the coin count: the readback reads the follower's pack from the previous tick's build pass, which can run before the asynchronous Papyrus harvest credits him, so a real credit can read as none and LATCH the coin-purse road off for the session. Fix: read it back one tick later, or treat "HARVESTED but NOT credited" as suspect rather than final.
  - **F8 (SEV-5, OPEN, `iaus/EffectMirror.cpp`):** dead foes are now mirrored (the build pass walks every high actor, dead ones included). Cost only; nothing reads a dead foe's effects for a decision.
  - **F9 (SEV-5, OPEN, `logistics/InvSnapshot.cpp` Readable / `Diagnostics.cpp` gate):** the paused-tick exemption from the party's rule is assumed safe because no follower decision is made in a paused tick (`Scheduler::Tick` returns on pause). Not proven for every paused-tick consumer.
  - **RESOLVED 2026-11-16 by `fix/worker-writes-main` (`Diagnostics::RequestBoardToggle`: both are served inside the tick body, behind the tick gate; the teach is posted).** Was: **(SEV-4, OPEN, M2 residue):** the off-tick `AddTask` closures (`Board.cpp` ~:938 the board hotkey, `Diagnostics.cpp` ~:520 the Field Orders power) run `Board::PublishSnapshot` / `ApplyEdits` outside the tick bracket, ungated by the tick gate. Their party reads can be not-yet (stateful entries guard with `InvSnap::Ready`), and their worker-side writes (the teach) are the M2 class above.
  - **F1, Lotd part (CLOSED, no change):** `Lotd.cpp` `TransferOnMain` already rebuilds the follower's pack ONCE per transfer job, after its item loop (`if (moved > 0) InvSnap::RebuildOnMain(f)`), so the once-per-distinct-ref pattern holds there. Its per-item live `InvSnap::Items(f)` on main is the SEV-5 item above. The TheftGate drain (F1 proper) was fixed in the same round.

### feat/oracle-gap review (raised against 7e883a9a, Opus 5.5, 2026-11-12)
L1 (SEV-3), L2, L3, G1 and G2 (SEV-4) were fixed in the next round (16cad7b7, 1bcb736a). The SEV-5 items, verbatim:
- **SEV-5 (tools/iaus_sim/Logistics.h header):** verbatim: "The `Logistics.h` header lists the errand triggers as sell >= 100 g or carry > 80%, but the code also triggers on `needs`. The code is right (native's live-need re-check) and the doc says so; only the header is incomplete." Reviewer: header only. RESOLVED in 16cad7b7 (the header rewritten with the L1-L3 rules names the need trigger).
- **SEV-5 (tools/iaus_sim/tune_driver.py stage_gap -> stage_joint):** verbatim: "Open decisions raised by the `stage_joint` call inside the gap stage are tagged stage \"gap\", so a later run of `joint` alone does not clear them." OPEN.
- **SEV-5 (tools/iaus_sim/tune_driver.py gap_ledger):** verbatim: "`gap_ledger` reads `st[\"cells\"][c/w]` for every world in the current `A.worlds`. A kept class measured earlier on a different `--worlds` set would raise a KeyError." OPEN.
- **SEV-5 (tools/iaus_sim/edge_search.py:215/325):** verbatim: "`edge_search.py:215/325` still says Piece points cannot be reached through `SetCurveParam`. That is pre-existing and now stale." OPEN.
### feat/class-tuning review (raised against 33a0b963, Opus 5.5, 2026-11-12)
- **SEV-5 (F7, tools/iaus_sim/tune_driver.py:274; native ClassCurves SetCurveParam):** verbatim: "A Logi `b` candidate for a negative midpoint clamps to 0.02, which flips its sign (:274). `SetCurveParam` accepts a px equal to its neighbour (a duplicate x)." Reviewer: two edge cases the driver never produces today. OPEN.
- **SEV-5 (F8, tune_driver.py:413):** verbatim: "the combatmods check runs only in `stage_prep` (:413). Running `--stages classes` alone on a fresh `--out` skips it. It would still fail loudly, because that list's potworth file would be missing too." OPEN.

### SEV-4 · retreat outlasting a buff gives a false NEVER LANDED (raised against 6f5011b4, fix/sight-wins-buff-release, 2026-11-12)
Verbatim: "Retreat case: if a retreat outlasts the buff's duration, the late judgment reads Up::No with ageMs past the cap. That produces a false 'FIRED but NEVER LANDED' WARN and holds the buff off for the rest of the fight. Failure scenario: Oakflesh fires, she retreats for more than 60 s, she returns, and the buff is held off. One possible fix: store the first Yes seen in TickBuffLanded, or call TickBuffLanded on the retreat path too."
Reasoning: the in-combat retreat branch (CombatScan.cpp ~:1189) returns before TickBuffLanded (:1336). The bound lasts only for the retreat; the window is wrong only when the retreat is longer than the buff. Area: cast/BuffRoad.cpp TickBuffLanded, scheduler/CombatScan.cpp.

### SEV-5 · ArmorPrefFor recomputed per tick in the mage-clothing path (raised against 81e75408, fix/mage-line-clothing, Opus 5.5, 2026-11-12)
Verbatim (Opus review of 81e75408, F6): "The mage branch of `ComputeOwnedGearPick` (EquipAuthority.cpp:128) now calls `ArmorPrefFor` (the perk-vote tally) on every call just to write a log line. That includes the calls from `DeclaredSetKeeps`. It is a small cost per tick." Reasoning: `ComputeOwnedGearPick`'s mage branch now calls `LogArmorClassIfChanged(a_follower, ArmorPrefFor(a_follower), true)` each tick only to print the change-only `[armor]` CLOTHING line (two base AV reads + the style-mirror lock per tick). Deferred, not fixed; cache or skip the pref when the line's key is unchanged.

### fix/mage-buff-loop review, F5 / F6 / F7 (SEV-5), raised against 3f4ee3ca
Opus 5.5 full review of 3f4ee3ca (`fix/mage-buff-loop`) vs d16b27cc. DEFERRED, not fixed (rule 9). Verbatim:
- **F5 SEV-5: per-actor pacing.** A self heal right after a buff is delayed by at most max(1, fCastCooldown), or `kConcApplyPeriod` for concentration. It returns `Refreshed`, which is transparent, and the next lap applies it. Concentration billing is unaffected: `BeatChargeSec` detects the first beat by `paidThrough` == epoch, not by `lastApply`. `SettleSec` returns 0 before the first beat. Acceptable. Exempting Heal kind would be a cheap option.
- **F6 SEV-5: the 0xFF dynamic-form skip.** It also drops any spell crafted at runtime (spellmaking mods) that a follower was taught. I found no MFO path that teaches a dynamic spell, and MFO's own ConcProxy forms are never in a spell list. So no legitimate spell is dropped in practice. Asking Harbinger whether a form is its proxy would be more precise.
- **F7 SEV-5: a buff we did not cast can be dispelled.** If the `ApplySelfEffect` already-active guard skipped because a buff from elsewhere was already up, no lock is set. On the next lap the item-3 latch says Yes and the rule goes transparent. The stream goes stale and is dispelled (keep=false), costing one recast.
- Coordinator note (fix round 8ce5f200): F7's keep=false path is now judged on main and only for non-Buff-kind or concentration spells, so a fire-and-forget Buff is kept; F7 may already be closed, re-check at drain. Wards are Buff kind, so a fire-and-forget ward is also kept on a stale/switch release.

### feat/buff-potions review, F5 / F6 / F7 / F8 / F9 (DEFERRED), raised against a444e159
Opus 5.5 full review of a444e159 (`feat/buff-potions`) vs b4533665. Verbatim:
- **F5 (SEV-4):** `ElementShare` is a ratio, so a single fire mage aiming at the player gives 0.5/0.5 = 1.0, and the follower drinks Resist Fire even though he isn't the target. "A foe on him counts double" only matters when foes are mixed. (The comment part, shouts/breath are read, was fixed in dc53cc42.)
- **F6 (SEV-4):** `LiveBuffKinds` marks every kind of a multi-effect potion as live while any one of its effects lasts, which over-gates. It only causes under-use.
- **F7 (SEV-4):** there is no `magnitude > 0` check. A modded negative resist effect without the Detrimental flag would be classed as a resist. Nothing in vanilla does this.
- **F8 (SEV-5):** DummyPotion and DefaultPotionRackPotionMarker classify as fortify_health with magnitude 1 and duration 0. This is harmless.
- **F9 (SEV-5):** each lap copies the Kit's 17-entry BuffStock, and names over 15 characters allocate on the heap, but only for kinds he carries. The cost is negligible.

### feat/oracle-gap batch 3 + part C review (raised against ca46a3e2, Opus 5.5, 2026-11-13; log scratchpad agentlogs/review-oracle-gap-b3.md). DEFERRED, not fixed (rule 9). F2 (STATUS scope) and F7 (duplicate kNames "threat") were fixed in the merge round.
- **F1 (SEV-4, OPEN, tools/iaus_sim World.h StepWorld / Policy.h:441 / Oracle.h:181 / Setup.h:244 / World.h:723):** verbatim: "--space compression in first StepWorld, Serve runs first at now=0 (Policy.h:441, Oracle.h:181) -> first decision on open-space distances. Late spawns (Setup.h:244) snap to rim on first Move; dodge (World.h:723) bypasses wall." Reviewer: the room is applied one step late, a late spawn starts outside it, and a dodge step is not clamped; lab-only, behind `--space`.
- **F3 (SEV-5, OPEN, tools/iaus_sim/Setup.h:176-177):** verbatim: "named all-(-1) -> best=-1 -> OOB." Reviewer: a named entry whose specs all failed to build indexes specs[-1]; no current extract produces it.
- **F4 (SEV-5, OPEN, tools/iaus_sim/Setup.h named override):** verbatim: "named override keeps class kit k.silver etc." Reviewer: the named follower keeps kit flags (silver, staff) from the class kit it does not carry.
- **F5 (SEV-5, OPEN, tools/iaus_sim/Data.h:393):** verbatim: "named specs enter TagRoles pools (Data.h:393). Verified current extracts: 0 spells, non-Bandit -> no effect today." Reviewer: the named specs share the foe spec pool; inert today.
- **F6 (SEV-5, OPEN, native/iaus/Generate.cpp:392-398):** verbatim: "Generate.cpp:392-398 perkShare max(1h,2h) / melee-only share vs bow perks; inert while flat." Reviewer: the melee side takes the larger of one- and two-hand rather than his wielded kind; inert while the curve ships flat and the game leaves PerkRead unread.

### feat/offclass-storage r2 review, retrieval cost (SEV-4) / keep-gate combine (SEV-5), raised against 665c3f46
Opus 5.5 review r2 of 665c3f46 (`feat/offclass-storage`) vs 17d358c8. Status: OPEN. Verbatim. (The same review's SEV-3 ESL new-CELL finding was FIXED, not deferred: the chest now lives in an override of vanilla CELL 001052FD, design doc section 0a.)
- **SEV-4:** "Retrieval: ComputeKeepSet+BuildSaleKeepGates per fitting not-kept stored item every 5 s forever. SEV-4 perf."
- **SEV-5:** "(2) SaleKeepsUnworn vs sell loop 868-876: weapon = keep && !offRole && !weapCannotEquip; armor = !blacklisted(forceSell for unworn) && keep && !armoCannotEquip. redundantInferior/deniedWorn are worn-only. No drift. Two combine sites remain (SEV-5 nit)."

### feat/iaus-loot r2 review (raised against 0066bbbf, Opus 5.5, 2026-11-13; log scratchpad agentlogs/review-iaus-loot-r2.md)
- **R2-1 (SEV-4, FIXED on fix/soulgem-cap; ruling marth 2026-11-13: "cap em, but better is a weapon charge check a weapon with a charge below 50% triggers the looting of gems". Cap = LootTuning::soulGemTarget 3 via NeedScore, trigger = LootTuning::gemChargeTrigger 0.5; native/iaus/LootNeed.h:226 / native/logistics/LootNeed.cpp:143):** verbatim: "the soul-gem need can never be met. MFO has no road that recharges a weapon. So carried gems never lower weaponNeedsCharge. A follower holding a drained enchanted weapon takes every soul gem until the room factor stops him." Fix: count carried soul gems against a small target (soulGemTarget = 3, NeedScore), or the recharge road (ClickUp 1dfcuabd356). Not exercised on MEO lists.
- **R2-2 (SEV-5, FIXED on fix/soulgem-cap: LowestChargeFrac checks every extra list; LootNeed.cpp:144):** verbatim: "GetEnchantmentCharge stops at the first extra list that has a charge, so in a stack of several copies a drained second copy is not seen. The result is a missed need, never a hoard."

### feat/tuning-file (raised against the author's own note, 2026-11-13)
- **DUPLICATED SPEC PARSER (SEV-5, deferred until a Fable sim batch):** the sim's spec parser (`tools/iaus_sim/RealClasses.cpp ApplyCurveSpecs`, `Tune.h SetParam` / `ParseSets` / `kTuneScalars`) and the native one (`native/iaus/TuningFile.cpp`, `Catalogue.cpp SetTuningScalar`) are two copies of the same grammar. Until the sim moves onto the native parser, a new knob or form must be added to BOTH or a pasted tuning line is rejected in game. tools/iaus_sim was off limits to the author of this change.
- **(SEV-5, raised against 9eafa8c7, Opus 5.5 review of feat/tuning-file):** verbatim: "shared.txt change runs RebuildAllClasses, which also reads class files still in pending (mid-write); transient, next check re-applies final content"
- **(SEV-4, raised against 789ed637, Opus 5.5 review 2):** verbatim: "shared-line all-or-nothing, parity gap with the sim's ParseSets (Tune.h:75-80): (a) a trailing comma is accepted by the sim but the native loop runs while p <= line.size(), so the empty entry rejects the whole line; (b) the sim accepts st.*/pk.*/row.* inside a --set list, native rejects them, so a pasted mixed line loses every real entry. Fix: skip an empty entry like the sim." Reasoning: loud, and tune_driver emits single t.* sets today (TuningFile.cpp:438).
- **(SEV-4, raised against 789ed637, Opus 5.5 review 2):** verbatim: "RebuildAllClasses ignores the return value: on a shared.txt change, an unchanged but temporarily unreadable class file keeps its old live row built from the OLD shared curve, and is never retried until its own file changes. Fix: on a false return reset that entry's last to Stamp{} so the next check retries." Reasoning: needs an AV/Syncthing lock at that instant; not threading/co-save (TuningFile.cpp:359).
- **(SEV-5, raised against 789ed637, Opus 5.5 review 2):** verbatim: "MAP.md TuningFile entry calls Board::PublishSnapshot main-thread; it runs on the worker (an AddTask body). Doc nit."

### fix/chase-waiver-player (Opus 5.5 review, 2026-11-13, against 3a75e198)
- **(SEV-5):** verbatim: "Evaluator.cpp:186 still opens with 'engaged both ways', which is no longer accurate when the foe targets the player."
- **(SEV-5):** verbatim: "Evaluator.cpp:550 calls RE::PlayerCharacter::GetSingleton() when the function already has a_player, used at :567. Using that would be consistent; behaviour is the same either way."
- **(note, not a defect):** the widened legacy waiver lets a hurt melee follower chase a HELD foe of any distance that targets the player (marth's "HIM or PLAYER" ruling covers it). The sim (tools/iaus_sim/Policy.h:78) has no held-foe waiver at all: next Fable sim batch.
### fix/loot-guard-gate (Opus 5.5 review 2, 2026-11-13, against d2c1661d)
- **(SEV-5):** verbatim: "5 s window vs the real cadence: a held body's stamp is only refreshed on that follower's own turn, N x 133 ms. That is about 2.1 s at 16 active followers, so 5 s holds up to about 37. Derive the window from the period x active count instead of a literal." (Service.cpp GuardHeldAt, cap sweep ~:276)
- **(SEV-5, round 1):** verbatim: "a future inner gate on a different key would wrongly mark the body as held" (StripCorpse refusal counter; comment added at LootScan.cpp ~:1050).

### fix/heal-claim-gates (Opus 5.5 review, 2026-11-13, against dc930d69)
- **(SEV-3):** verbatim: "The queue never compares g_firingRule with the rank of the rule that owns the left claim. A dying ally from a higher-ranked rule waits behind a lower-ranked concentration heal on a healthy ally... when the queued rule outranks the left claim, release the left claim so the higher-ranked recipient gets it."
- **(SEV-3 follow-up):** verbatim: "DirectSelf.cpp:186, DirectTarget.cpp:203 and Auto.cpp:667 are combat-table-only... the same inactive-controller stall probably applies to offense and buff concentration claims."
- **(SEV-4):** verbatim: "each change to inactive releases the standing claim through the existing no-controller branch... watch for inactive=true lines in the next field run."
- **(SEV-4, Opus 5.5 review 2 against adf5880e):** verbatim: "inside the else-if at :989-1007, the guard `if (onHand[kHandLeft].found)` is always true, because the else-if condition already requires it. So `a_hand = kHandRight` and the displacing bookkeeping below it (:998-1005) are unreachable. The surrounding comment (:984-987) is stale. Cleanup only."
### feat/iaus-loot-paths (Opus 5.5 review, 2026-11-13, against 68159eed)
- **(SEV-5):** verbatim: "CorpseGuardWaived applies only to Road::Corpse; Gear/Swap/Silver takes from an unowned wilderness corpse (SwapUp.cpp:610, Silver.cpp:468, Gear.cpp:856) stay guard-gated (conservative, inconsistent with the ruling)."
- **(SEV-5):** verbatim: "the waiver depends on TESObjectREFR::GetOwner returning null for a dead actor; if it returns the cell owner the waiver never fires (fails safe; the WAIVED log shows it)."
- **(SEV-5):** verbatim: "g_stealSkipLogged not cleared on load (logs only)."
### fix/iaus-claim-thrash (Opus 5.5 review, 2026-11-13, against 0c899c78)
- **(SEV-3):** verbatim: "a lower Buff claim the engine never builds (NOT BUILT) now holds off a higher Offense for up to 6 s; nothing checks the incumbent's target (dead foe / now-buffed ally keeps the hand until it fires, ends or the cap); consider releasing early when ClaimLiveOnHand stands but the incumbent's target is dead or the claim reports not-built." (`cast/Hands.cpp` CanPreemptHand fresh-incumbent guard; reviewer reasoning: the widening is kept by ruling, this is the cost.)
- **(SEV-4, Opus 5.5 review 2 against 92731a4c):** verbatim: "The foe prune leaves no log line, only the droppedTarget count (Generate.cpp:678). Suggest one line per (follower, spell, foe MGEF) in Candidates when FxAllReplaced fires."
- **(SEV-4):** verbatim: "The buff measure calls LiveFx(p) inside the per-buff-spell loop, O(buffs x party x effects), allocating keyword vectors each time (SpellBook.cpp:376-387). Hoist one LiveFx per party member above the spell loop."
- **(SEV-4, note):** "For a re-apply after the beat (liveInstantStream, Applied), commitPreempt now runs after CastSelfDirect instead of before it. That stream holds no hand, no harm found."
- **(SEV-5):** "The WantFoeFx Post lambda checks !IsDead() but not Is3DLoaded(), unlike PostRefresh. Safe (LiveFx returns empty), but match PostRefresh."
- **(design note for marth):** all Influence MGEFs (Calm, Fear, Fury) share keyword 00078098 with the dispel flag, so a Fear spell is pruned on a calmed or frenzied foe and vice versa (the engine dispels one with the other). Switching a foe from fear to frenzy on purpose is a design question.
### feat/tuner-auto-decisions (Opus 5.5 review, 2026-11-13, against b59b0a1a)
F1-F6 of this review were FIXED in the fix round (not deferred). Deferred below, status OPEN, tools/iaus_sim/tune_driver.py:
- **F7 (SEV-4, OPEN):** BUFF_SHARE_CAP is never checked on its own; the potion gate only refuses a rise of a.drink_buff beyond noise, so a start table already over the 1% cap is held, never brought under it.
- **F8 (SEV-4, OPEN):** a second `--stages edge` run re-widens from the already widened value past EDGE_ROUNDS (the round count is not carried across runs).
- **F9 (SEV-4, OPEN):** `SUMMARY["withheld"]` only accumulates across resumes; it is not cleared at recalib start, so a value withheld in an earlier run stays withheld after it would now hold.
- **F10 (SEV-4, OPEN):** the Knight guard floor is checked per accepted value, not for the whole final table.
- **F11 (SEV-5, OPEN):** an edge value rejected in a later round overwrites e["best"] / steps / verdict of the earlier accepted round.
### fix/sneak-release-hold (Opus 5.5 review, 2026-11-13, against 6f555217)
- **(SEV-5):** a bow held drawn for more than 2 s still gets SneakStop mid-shot after the deferral bound (Harbinger's release confirm covers it).
- **(SEV-5):** `s_last` in `StanceProbeTick` (native/apmf/SneakClaims.cpp) is never cleared across loads.

### fix/loot-scan-perf (Opus 5.5 review, 2026-11-13, against 55b80011)
- **(SEV-4, design; RESOLVED 2026-11-16 by `fix/worker-snapshots`, INVARIANTS #84: every one of these reads the main-built `InvSnap` snapshot now. Re-graded SEV-2 / 3 by the stability audit of 2026-11-16, row M1, under carve-out (a).)** the worker reads mutable container inventories (HasLoot LootScan.cpp:597, [arrowprobe] :611, PickLocks GetInventoryCounts :572, Take Analyse/RemoveItem, CorpseGuardWaived). Documented #72 worker-transfer pattern, no field crash attributed. Redesign as ONE per-tick per-ref main-side inventory snapshot shared across categories (also the next perf lever: HasLoot builds a GetInventory map per corpse/container 6-7x per follower per tick). The branch's main-side waiver move was reverted: it mirrored the race (main read vs MFO's own worker RemoveItem) instead of removing it.
- **(SEV-5; RESOLVED 2026-11-16 by `fix/worker-snapshots`: removed.)** [arrowprobe] is diagnostic only; drop it.
### fix/caster-posture (Opus 5.5 review, 2026-11-13, against 0ed9c705)
SEV-2 findings 1, 2 and SEV-3 findings 3-5 were FIXED in the fix round. Deferred, status OPEN:
- **(SEV-4, a):** verbatim: "Reposition.h RangeToHold / MageBackOffNeed read DefaultTuning().worth while Generate uses a_t.worth (sim mismatch; pass worth in)."
- **(SEV-4, b):** verbatim: "HandsRange closeOk=false falls back to the book's longest spell without an affordability / free-hand check."
- **(SEV-4, c):** verbatim: "the cap makes a tie and the tie-break prefers the lower cost (a cheaper buff can win); bestOffense ignores offense scrolls."
- **(SEV-4, d):** verbatim: "the close-spell refusal is counted in droppedTarget."
- **(SEV-4, e):** verbatim: "prevRight / prevLeft are captured only on the right-hand gambit path, a pre-drawn AI dagger may be 'restored', and the restore placement can sit 120 s."
- **(SEV-4, f):** verbatim: "the static per-follower log maps are never cleared."
- **(SEV-5, g, against aaacee6e):** verbatim: "s_spellsHold (Candidates.cpp) is not erased by Candidates::Forget; IausRetreatSelfHeal's Build resets the latch out of combat (harmless)."

### fix/save-integrity-mfo (Opus 5.5 review, 2026-11-15, against ab93a51c)
F1 (SEV-3, stream targets outside the process lists), F2 (SEV-4, two-pass sweep), F3 (SEV-4, MagicItem type check), F6 (SEV-5, thread id in the summary lines), F7 (SEV-5, CLAUDE.md rule count) and the F4 load-summary rewording were FIXED in the fix round. Deferred, status OPEN (cross-referenced from MAP.md's `cast/ProxySave.cpp` "What breaks"):
- **(SEV-4, F4):** verbatim-ish: "no post-load watch. Harbinger's post-load watch covers MFO casters only on Harbinger fix/proxy-save-integrity, not with Harbinger absent; an old save whose caster / effect points at a form destroyed AFTER the post-load repair passes is not caught." Reviewer's reasoning: the repair runs at kPostLoadGame sync + first pump only.
- **(SEV-5, F5):** verbatim-ish: "the load repair resets fewer fields than 34435: SetDualCasting(false), desiredTarget, the caster timers and SetCurrentSpellImpl(nullptr) are not reset." Reviewer's reasoning: the repair skips 34435's vcalls on purpose (they read the dead spell), so a field 34435 clears through them stays as it was; low impact, the caster's next cast rewrites them.

### fix/corpse-take-main (Opus 5.5 review, 2026-11-15, against 4cb790da)
F1 (SEV-3, landing-hold retry cadence), F2 (SEV-4, "logic honesty only" doc), F3 (SEV-4, no transfer under an open inventory menu), F4 (SEV-4, the one-worn-take cap: dropped), F5 (SEV-5, landed Say rate-limit key) and F6 (SEV-5, AfterLanding only for an actor source) were FIXED in the fix round. Deferred, status OPEN (cross-referenced from MAP.md's "CORPSE TRANSFERS RUN ON THE MAIN THREAD" entry):
- **(SEV-4, F2 follow-up, optional; RESOLVED 2026-11-16 by `fix/worker-snapshots`, INVARIANTS #84: not the Dekker gate. The worker reads the main-built snapshot, and `RunTransfer` replaces both snapshots before the drain pops the entry. Re-graded SEV-2 / 3 by the stability audit, row M1.)** the worker still READS corpse / follower inventories (Analyse / GetInventory in TheftGate::Takeable / Take, DeclareAtExit, IAUS ReadLootInv) while the main-thread drain writes them: the old race in the other direction. Reviewer's reasoning: the fix removes the worker WRITE to an actor, not every off-main read; an optional Dekker-style gate (the drain sets a flag the worker checks before an inventory walk, and vice versa) would close it without making the worker wait.
- **(SEV-5, F7):** verbatim-ish: "planned-count ledgers (Lotd.cpp:1936 needs.uncoveredAll, Errand::Smith::NoteTaken LootTake.cpp:442) count Take's PLANNED return; a planned take that moves fewer on main leaves them over-counted until they self-heal." Reviewer's reasoning: both re-derive from the pack on their next pass.
- **(SEV-5, F8):** verbatim-ish: "OnMainThread() is false before the first drain has run (g_mainTid unset), so a main-thread caller before then queues instead of running at once." Fix: use a pump-recorded IsMainThread (the thread id MainThread's pump records), not the drain's.
- **(SEV-5, F9):** verbatim-ish: "IAUS ReadLootInv inv.carried (LootNeed.cpp:226) ignores InFlightWeight", so the IAUS carry read is one frame behind for queued corpse takes.
- **(SEV-5):** verbatim-ish: "Steal.cpp:577 reads Takeable == false as empty", so a landing hold there leaves one item for the next visit instead of retrying (also in MAP's known limits).
- **(SEV-5, F1 residue):** the landing hold fires on any Takeable of the form type, including objects the caller's later filters reject (LootEquipment.cpp calls Takeable first in its inventory loop). Moving it after the armor / weapon filters is NOT a pure reorder (the loop's many branches each pick a best candidate), so it was left; the 150 ms re-arm bounds the cost.

### fix/alias-ops-main (Opus 5.5 review, 2026-11-16, against af339fe7)
- **(SEV-4, FIXED in the merge round):** Pump Requested tested `filled` before "has the op landed"; now the not-landed wait comes first.
- **(SEV-4):** a loot or retreat fill that fails on main is not fed back to the worker (confirms the author's entry).
- **(SEV-5):** the critical-stage release can drop a cast ClearAlias eviction queued behind it; if the holder was already reset, ReleaseAll's ClearAlias is skipped and that op's concentration InterruptCast is lost (the alias itself is still swept).
- **(SEV-5):** on a runtime with no native, LootTravelEvictIf / RetreatEvictIf now log "RELEASE FAILED" as ERROR if the alias holds a_id (was a silent no-op; only from an earlier session's save).
- **(SEV-5):** the single-store claim for StoreLocationRef / StoreTargetRef relies on MSVC emitting one mov; std::atomic_ref<std::uint32_t> would make it structural.

### feat/perk-effects (Opus 5.5 review, 2026-11-16, against a314a21b)
- **(SEV-3, RESOLVED by marth 11-16 "Ship 0 plan tune soon"):** armor / weapon perk factors shipped at weight 0 (t.perkArmor, t.perkWeapon). Before turning on: the sim needs a gear pick to tune them; address the feedback loop (armor mean over carried pieces; weapon kind factor from one probe weapon), in-combat condition swings (freeze the table in combat), and double counting with kStyleBias / kArmorPerkBias (skill must still decide armor class).
- **(SEV-3, FIXED):** tune app lists: spell_perks cons, t.perkArmor / t.perkWeapon scalars. Pre-existing SEV-4: the app accepts only a|b|floor curve fields, not py0..py2.
- **(SEV-4):** main-thread cost: ~80 HandleEntryPoint calls per typical build, up to ~970 worst, each follower every ~2 s; raise kRefreshTicks (~112) or rebuild on a change signature; cap the armor asks.
- **(SEV-4):** seeds: EP35/EP85 seeded with base not skill-scaled values, EP38 with 100 not the spell cost, EP39 with 1.0: Add entries distorted.
- **(SEV-5):** harness.cpp "// School affinity" comment now above SpellPerkRows; generic armor probe counted in armorPieces; [perk-fx] line length / reprint per flip (B860 item 8).

## feat/prog-class-weight (Opus 5.5, 2026-11-16, against 0892051b)

- **F3 SEV-4:** sneak-line rows (Thief, Master Thief, Nightingale, Listener: kSneakAlways, Steal / PickLocks rows) give no Sneak / Lockpicking / Pickpocket weight in `ClassProfile`. Reasoning: the profile reads weapons and spell roles only; a thief's signature skills get only the Progression list's own share. Deferred: a table row in `ClassProfile` when tuned.
- **F4 SEV-4:** the perk pick (Verbs.cpp:253, the `WeightsFor` fallback in `AllocateNextEligible`) ignores the class profile and the crafting share, so perks follow the Progression list while skill points follow the class. Reasoning: scoped out ("auto skill allocation"); a perk-order change is its own decision.
- **F6 SEV-5:** the crafting ceiling is checked once per grant batch (`GrantWeightsFor`), so a large batch of pending levels can push a crafting skill slightly past 50. Reasoning: tiny share (4 %), floors keep it bounded.

## fix/chase-waiver-veto (Sonnet 5.5 author, 2026-11-16, tier B)

- **SIM MIRROR (open, Fable-only, `tools/iaus_sim`):** (1) the sim's act.attack range input must score a waived foe at x = min(dist/cap, 1.0) (melee) or min(dist/reach, Tuning::waivedRangeX = 0.85, a PLACEHOLDER for the sim to search) (ranged) instead of vetoing it (Policy.h where the cap veto is applied; Generate mirror: `Facts::foeChaseWaived` set from the waiver in Policy.h ~:84-86); (2) the sim's held test omits `entryFoeID` (native already includes a Standing-entry foe via `ChaseWaived`), model a `--waiver` arm for it or note it. Re-run the N6 laps (dragon dropped 20.5 percent with `--waiver player`) after (1).
- **SEV-3 (sim re-tune, against daca96ee review F1):** a NON-waived ranged act.attack scores RangeSpell Logi(-10, 0.85), ~0.18 at the cap and under the act floor from about 0.85 of the cap, so an archer or staff user goes idle on foes past ~0.85 of the chase cap while a melee follower names them to the cap. Pre-existing; not retuned here. Sim to search the ranged act.attack range curve (or its own shape) against real foes.
### feat/conc-hand-cost (author notes, 2026-11-16; ClickUp 1dfcuabd22x; Opus review round 1 fixed in the next commit)
- **(SEV-4, sim mirror):** the sim has no `hand_lost` consideration, `t.concChannelSec` / `t.handRedrawSec` scalars or `Combatant::weaponLeftShare` (mirror list in the author report: cons + Piece curve, weight 1 in CastBuff / CastWard, the share rule, the two scalars in `Tune.h`). Fable sim batch. Until mirrored every value is a placeholder: curve `Catalogue.cpp` (0 -> 1.0, 0.05 -> 0.6, 0.15 -> 0.35, 1 -> 0.2), channel 11.5 s, redraw 0.65 s.
- **(SEV-4):** a staff in the LEFT hand is not priced: `EstimateHit`'s staff-in-left path returns before `leftShare`, so a staff right + melee left reads 0.
- **(SEV-5):** heals and ally buffs: a channelled HEAL is not priced (heal-hold rules own the hand); a buff at an ally through the ConcProxy is priced the same as a self buff (it holds the same hand).
- **(SEV-5):** mid-channel the bow is unequipped (`cast/Hands.cpp:977`), so `w` is likely null and the cost reads not applicable then. Harmless: the stream runs to its cap, and the price matters at the pick, before the bow leaves his hand.

### fix/field-1117-loot (author notes, 2026-11-17)
- **(SEV-4):** the hostile-to-the-player read is taken once at death and kept as a FormID set (session state, not saved): a hostile body that died before the sink saw it (loaded corpse, previous session) is NOT waived in a town (fail closed). Could be widened with a live faction / aggression read of the corpse once one is verified to survive death.
- **(SEV-5):** the party-wide corpse bar's re-ask uses `GuardCheck(follower, corpse)`, which answers "a guard sees THIS follower or the body", so a bar can lift or hold on the asking follower's position, not the first follower's.
- **(SEV-5):** the near-face reach uses the base's `boundData` min horizontal half-extent unscaled by the ref (the LOTD crate reader's rule); a scaled container is under-measured (conservative).

### fix/field-1117-loot (Opus 5.5 review, 2026-11-17, against 156aa06e): MERGE after the SEV-3 (fixed: ClearHostileDeaths on revert)
- **(SEV-4):** LootScan.cpp:95 opens the full in-town scan whenever any hostile death is known; keep the early exit for categories that cannot source a corpse (Steal, PickLocks, loose-only) or gate on a hostile body in the scan's cells (Steal::Check main-thread scans and dibs accrual now run under guard sight only to be discarded at :619).
- **(SEV-4):** a hostile body that died before the death sink saw it (loaded corpse, earlier session) stays barred (fail closed) but the log does not say why: add "not known hostile (died before this session)" to the BarCorpse / waiver line.
- **(SEV-4):** L2 near-face reach (fActivatePickLength + smaller horizontal half-extent, unscaled) likely does not cover the 225-306 u field stops; the 2-strike blocklist does the work. Next field run's far-arrival line prints the reach. Root cause (Harbinger navmesh goal far from a shelf/counter ref) out of scope.
- **(SEV-4):** g_farArrivals erased only on a real arrival or a strike; stale entries can count as "2nd in a row" later (64-entry cap).
- **(SEV-5):** near-face reach unscaled by ref scale (under-measures scale > 1); kFarArrivalProgress / kFarArrivalMax / kBarRecheck are constexpr, not LootTuning rows; a ~3 s old Clear verdict can lift a bar; a near-face arrival can bypass the player bubble for reach > 200 u. (Teammate-target hostility is FINE per marth 11-17: a townsperson hit by friendly fire fails the crime check and the hit was a crime.)
## fix/field-1117-cast (Sonnet 5.5 author, 2026-11-17, tier B; against aeb8d543)
- OPEN (design, not built): a self-delivered AoE OFFENSE spell (Frost Nova, shape ScriptRider, Self / FF, kind Offense) still takes the unanimated `self-stream` road (`cast/CastOn.cpp` self fork, `CastSelfDirect`). `ChooseBuffRoad` answers NotBuff for Offense kind, so there is no claim road to hand it to. It needs Offense-kind spells routed through the buff claim shape (ClaimOffenseCast, target = the caster's own FormID, Script row) and a Harbinger field proof that an Offensive-row self claim fires; that is a new road/claim shape (animations required for ALL actions). Field 1117 Arch-Mage 10:36:54.
- PLACEHOLDERS (labelled, derive from the sim / field): `SubstrateTuning::kUnfiredBackoffMs` 30 s (cast/SubstrateRoad.cpp), `kUnfiredRun` 2 (cast/SubstrateRoad.cpp), `kFullFireWindowMs` 60 s (cast/HealRoad.cpp, only a recency guard).
- A7 backs off ANY unfired close of the pair, not only "the behaviour tree never ran" (MFO cannot read Harbinger's close reason; its own `closeReason` is closed-unfired / cast-claim-ended / not-opened). Harbinger ask: expose the close reason, and diagnose why the substrate tree no longer starts (FINDINGS A7).

- OPEN (design, round 2; marth 11-17 final: "the spell with no animated road is double proxied into a spell with one, but the same effect") Frost Nova / self-centred attack spells: NOT built (STOPPED). Findings: the heal delivery-flip proxy is MINTED BY HARBINGER (castproxy: a kTargetActor copy with SHARED effects, transiently taught; log `minted delivery-flip proxy 0xFF0016A5 for spell 0x00012FCC (delivery 0 -> 3)`), MFO's own ConcProxy pool (`cast/Direct.cpp`, #82 sweep in `cast/ProxySave.cpp`) is the concentration-stream apply form, not a claim form. A Self-delivery spell needs NO flip to be claimed: delivery Self with target 0 is the native self cast (the self buff / Script-row claim road, `BuffClaim`), the area centres on the caster exactly as the original would, and no MFO form is minted (so no new #82 coverage). A flip to an aimed copy would instead centre the area on the aimed target and change the spell. What is missing is only the ROAD: `ChooseBuffRoad` answers NotBuff for Offense kind (`cast/BuffRoad.cpp:427`) and the `CastOn` self fork (`cast/CastOn.cpp` "SELF-CAST forks off FIRST") sends every self spell to `CastSelfDirect` (self-stream). Design: let a Self-delivery, FF, area, hostile-effect spell (ScriptRider, Frost Nova / Warstomp) take `BuffClaim` with target 0 on the Script row when a controller exists (kind gate widened in `ChooseBuffRoad`, consent for hostile effects in `BuffClaim`, a cast pace instead of the "already up" guard because a hostile AoE has no on-self effect to test). Harbinger asks / proofs before building: (1) a Script-row claim of a spell whose effects are hostile is served and FIRES for the claimant (the seat-0 classify of a hostile Self AoE; its `CheckStartCast` / `GetMagicTarget` seats), (2) the claim's "already on target" bound does not suppress or re-cast a hostile AoE, (3) which animation the hand plays for a Self FF spell with charge 0.5 s (passive field capture first, principle 5). Tier A on the Harbinger side; MFO side is a road widening (tier B).
- OPEN (design, round 2): retreat self-heal (10:33:52, 10:35:09 and 10:36:38 are ALL retreats: `pick=flee`, ch.22 deny live, StopCombat landed; none was a disengage). Keeping a substrate under a retreat conflicts with (a) the ch.22 re-entry deny that is live for the 35 s window (a substrate is a combat entry the deny refuses), (b) Harbinger's open refusal while the PLAYER is in combat (`cast/SubstrateRoad.cpp` step 2b: no substrate in real combat), and (c) the retreat's single StopCombat. Harbinger ask: let a substrate heal open exempt from the ch.22 deny for ONE self claim during a retreat, and relax the player-in-combat refusal for a self heal of a retreating holder; MFO side then files `TrySubstrateCast` from `IausRetreatSelfHeal` (`scheduler/IausSource.cpp:572`) and the retreat table walk (`scheduler/CombatScan.cpp:1195`) instead of `Fire`. Done now: a DISENGAGE defers a would-be unanimated heal (`cast/CastOn.cpp`).
**fix/field-1117-engage (2026-11-17, Sonnet 5.5 author, tier B; field Deck run 11-17 FINDINGS A1/A3/A5 + marth's three field notes; NOT merged, NOT field run; NO co-save, ABI or ESP change).** (A1a) `scheduler/IausSource.cpp` `OccludedFoe` / `ServiceOccludedApproach`: a follower whose hands hold a blade (`Snapshot::drawn` Melee / Silver) files NO offense approach and a standing one is released (the 1024 ranged standoff parked a sword fighter that merely carries spells); spell phase keeps touch 128 / ranged 1024. (A1b) `apmf/OffenseApproach.cpp` `Tight(.., a_tightening)`: a standing claim that is Holding and still occluded tightens down to `kMinRadius` (not the standoff) every `kTightenGap`; the 15 s regular-movement release now fires only at the true floor, and the sight dwell ends the claim at the first point with sight. (A1c) `g_offBlocked` is `follower -> foe -> Blk{pos, leash}`: a block lapses when `Confidence::LeashRadius` grew by `kBlockLeashGrow` 150 or he moved `kBlockMoveUnits` 300 (placeholders; stamped after `SweepOffenseApproaches` drops `g_offMx`, because LeashRadius takes the combat group lock). (A1d) `apmf/SneakClaims.cpp` sweep: a hide hold whose FIRST RE-FILE did not take (IsSneaking still false) calls `Concealment::AbortHide` (outside `g_sneakMx`): run + ch.24 + ch.7 deny released, `g_stanceFailed` forbids new Hide / Step runs until `Concealment::Release` (combat end). (A3) `iaus/Candidates.cpp`: a foe past the chase cap and not waived is kept, marked `Combatant::noApproach`, when it is inside the bow's range (`Reposition::InWeaponRange`, `Tuning::bowWeaponRange` 3000 placeholder) or targets him; `Generate.cpp` Swings names only bow-range noApproach foes (scored at the cap edge like a waived foe), `RangeFoe` skips them, the occluded approach walks to one only when `ApproachEligible` (bow range). (A5) `Scheduler.cpp` ServiceRetreat: root cause of the 73.6 s "timeout" = the game sat in a menu (heartbeat `mainAge` climbed to 73 s, Journal/MCM re-read at 10:38:33) and the clock was WALL time; travel timeout and a new stall give-up (`kRetreatStallMove` 50 u in `kRetreatStallSecs` 10 s unpaused) now count `g_serviceClock` from `RetreatNote::travelClockAt`; the APMF retreat package radius 150 -> 200 = `kRetreatArriveDist`. (field) `Reposition::MeleeUnreachableNow` (airborne OR another level and Occluded): `Generate.cpp` Swings prefers a reachable foe for a blade. **What breaks:** `noApproach` foes now sit in `Snapshot::foes` (Retreat / Disengage / Estimate threat reads see them; keep the `noApproach` skip where a foe is a DESTINATION); the weapon range is read only through `InWeaponRange`. **Open (SEV-3/4, tuning):** `bowWeaponRange`, block constants and stall constants are placeholders the sim must search. **SIM MIRROR (Fable-only, `tools/iaus_sim`):** `Combatant::noApproach`, `Tuning::bowWeaponRange` (Tune.h `t.bowRange`), Swings skip + `foeChaseWaived` clamp for noApproach, `MeleeUnreachableNow` preference, and `StrikeReady` still reads the chase cap for a bow (unchanged, candidate to move to `InWeaponRange`). **HARBINGER ASKS:** none required (the ch.24 goal is the foe reference with R down to 128, the engine paths it, which is what climbs a tower); optional: ch.3 in-combat sneak never taking (separate item), ch.23 leash denial of `MaintainOptimalRange` outside the leash (proposal only, not changed). **Unproven:** what moved Adelinda down the ridge in the first Frostmoth fight (her foe list was EMPTY by the chase cap, A3, so no IAUS road ran; engine movement presumed); where Cicero's through-the-wall target was chosen (engine pin / keep-current hold not traced).

**fix/field-1117-engage review round 1 (Opus, d7919dbe; fixed in the next commit):** blade whose foes are ALL `MeleeUnreachableNow` files the approach at the touch standoff 128 (a reachable foe = none; the engine swings at a foe it counts as in reach instead of pathing); `Generate.cpp` Swings `anyReachable` skips what the loop skips; noApproach foes rank after every eligible foe in `x.top` and are never Offense / Control cast targets (the `CanCastOffense` latch skips them too); A1(c) leashed blocks are stamped with player + foe positions and lapse on either moving > 300 u (other blocks: the follower's move); `kAPMFRetreatRadius` back to 150 (below `kRetreatArriveDist` 200); `MeleeUnreachableNow` uses `Reposition::OutOfVerticalReach`, the same h + reach line `CombatSense::OutOfMeleeReach` now calls; a pin on an unreachable foe is released when a reachable one stands (`IausSource.cpp` `ReleaseUnreachablePin`); `g_stanceFailed` is forgotten only at the fight's end. **Deferred (SEV-4/5):** `HoldSneakForHide` refiles inherited from an adopted claim; noApproach foes as ch.24 destinations are tightened to 128 (should stop at the standoff for a noApproach foe); `StepTarget` / Bash do not skip noApproach foes.

- OPEN (feat/anim-reach, 11-17) the chain's forward carry is not measured: laps (133 ms x party) are too coarse to place a mid-chain
  position, so a sequence's value is "reach from the chain's START". A graph-event timestamp pair (`weaponSwing` + `HitFrame`) with
  a main-thread position read would measure it.
- OPEN (feat/anim-reach, 11-17) there is no light / power / bash key: no graph tag is proven to tell a power chain from a light one (`MCO_PowerWinOpen` is a combo window). The swing line logs the power-window / bash tag counts per chain; when a run proves one, give `ReachBook::SeqKey` a kind back.
- OPEN (feat/anim-reach, 11-17) `TESObjectWEAP::Data::reach` (fork TESObjectWEAP.h:169) is not a seed: nothing proves the engine / Precision hit geometry follows it. Log it with the swing line in a field run, then decide.
- OPEN (feat/anim-reach, 11-17, review F5b) lock order: the ReachBook published mutex is a leaf taken inside the combat group's read lock (`CombatSense::OutOfMeleeReach`); nothing may be taken under it.
- OPEN (feat/anim-reach, 11-17, review F6) the sink's swing / power / bash counters are uint32 and monotonic; the chain's swing count is an unsigned difference, so a wrap is harmless, but nothing exercises it.
- OPEN (feat/anim-reach, 11-17, review N2) the chain grace (0.45 s of attack state kNone) is sized from the engine's ~0.6 s attack-decision gap, not measured for the combo frameworks, and a lap is party x 133 ms, so it is shorter than a lap from four followers up (the end is seen late; the N1 split keeps the hit window and swing count exact). If a combo's steps drop to kNone for longer than the grace, a chain splits into several. The swing line prints the lap length. REAL FIX once a field run proves it: end chains on the sink's `attackStop` event instead of a timer.
- OPEN SEV-4 (feat/anim-reach, review of d36bb29b) `CombatSense::BodyRadius` takes the wider of the OBND x / y extents / 2: a long body (a dragon, a horse) overshoots its real footprint radius, a slight downward bias on the learned edge reach.
- OPEN SEV-4 (feat/anim-reach, review of d36bb29b) the hit sink latches every follower-caused hit (spells and cloak ticks too); it should latch weapon sources only. Not trivial: telling a weapon from a spell needs a form lookup, which the sink thread must not do.
- OPEN SEV-4 (feat/anim-reach, review of d36bb29b) `ReachBook::ComboEdge` is a max over noisy per-swing-count keys, so it biases upward by a few units under noise (the sim shows 201 / 204 / 212 against a 178 / 208 truth).
- OPEN (feat/anim-reach, review N1) a split chain's hit latch keeps only the latest time per (target, source): a new attack's hit on the same target updates that row past the split, so the old chain can read a miss for a blow it landed. Rare (needs a split and a hit on both sides).
- OPEN (N3, Fable sim mirror batch) `tools/iaus_sim/Policy.h:138` now feeds `Snapshot::comboEdge` (renamed from `swingReach`): the Attack family reads the learned value where it used to read the flat 200 fallback, which changes the sim's behaviour; the line also carries a duplicated trailing comment. Left untouched for the sim mirror batch (Fable owns tools/iaus_sim).
- OPEN (feat/anim-reach, 11-17) the weapon learner starts at fMeleeReach, not at Precision's per-weapon value (iaus-i6 / Q64: a capsule
  length from the hand is not a centre-to-centre reach). A seed formula (length + arm + foe radius) needs a ruling.
- OPEN (feat/anim-reach, 11-17) a left-hand-only chain (a left power attack) is learned for the right-hand weapon (the existing review F1 ambiguity).
**fix/field-1117b-choice (2026-11-17, Sonnet 5.5 author, tier B; field Deck run 11-17b, Jesper's weak spells and death, Adelinda's approach; NOT merged, NOT field run; NO co-save, ABI or ESP change).** Open items the author names (no review yet):
- OPEN (uncertain): `effectRate` divides a fire-and-forget total by the placeholder `Tuning::spellCastSec` (1.5 s) for EVERY spell; real cast times differ per spell and are not read. The damage / heal CONSIDERATIONS still compare per CAST (a stream's 1 s total against an instant spell's whole damage), so a stream still scores low in `damage`; only the tie-break and `heal_rate` are per second.
- OPEN (uncertain): a concentration cost is counted over `Tuning::concChannelSec` (11.5 s, the heal / utility draw) in the tie-break for offense streams too.
- OPEN (design): Harbinger's ch.25 cast guard has no per-cast readback, so `fireLineKeepSec` memory is fed by MFO's own mirror of the guard's test, not by an observed refusal.
- OPEN (threading): `Actuation::NoteHealerThreat` / `g_healerThreat` is written by `Candidates::Build` and read by `CanPreemptHand`, both on the pump worker; clear on revert via `ClearCastLocks`.
- OPEN (sim): `tools/iaus_sim` mirror not done: survival band per caster, `heal_rate`, the tie-break order, `t.spellCastSec` / `t.dyingSelfHp` / `t.fireLineKeepSec`.
- OPEN (Harbinger ask): Adelinda's ch.3 in-combat sneak never takes (IsSneaking false x4); until it is field-proven the hide road stays OFF (`bIausHideRoad`, already default OFF). A per-cast ch.25 refusal readback would let the refusal memory use the real answer.
**fix/field-1117b-choice review round 1 (Opus, c8ac55be; SEV-3s fixed in the next commit: Hit / Swing fills default ON, recipient-aware + healer-exposure heal refusal, heal_rate against his best castable heal, fireLineKeepSec 1 s + a per-minute fireBlocked log). SEV-4 / SEV-5 deferred:**
- SEV-4: `Estimate::InLineOfFire` is a THIRD copy of the segment math (make Estimate.h `SegmentDist` the shared pure reader later); it does not exempt Harbinger-occluded friendlies and omits summons / bystanders; the Control family has WillWork but no SplashSafe.
- SEV-4: `g_healerThreat` stays true after Sentient AI is toggled off mid-fight (stale until `CombatEnded` / `ClearCastLocks`), and there is a bounded heal <-> offense thrash; watch `THREAT ON THE HEALER` / `URGENT HEAL` in the field.
- SEV-4: `Concealment.cpp` abort warn text (":772"), and `AbortHide` sets `g_stanceFailed`, which also bans the Step for the rest of the fight.
- SEV-5: per-cast vs per-second scope: a concentration heal is never braked by `heal_size`.
- Not checked in the heal fallback: the recast lockout / claim state of the fallback heal (only hands, cost, reach and sight).


## feat/sim-mirror-1117 (Fable author, 2026-11-17, tools/iaus_sim only; record: Docs/research/2026-11/iaus-sim-v3.md section 13)

- **CLOSED by this batch (the SIM MIRROR entries above):** fix/chase-waiver-veto items (1) and (2) (the waived mark, the entry foe as a MODEL), feat/conc-hand-cost's sim mirror (`weaponLeftShare`, `t.concChannelSec`, `t.handRedrawSec`), fix/field-1117-engage's sim mirror (`noApproach`, `t.bowRange`), feat/anim-reach N3 (`Policy.h` reads the game's `ReachBook`, the duplicated comment is gone), fix/field-1117b-choice (refusal memory, healer threat, placeholders).
- **OPEN (not re-run):** the N6 dragon laps after the waived mark. `dragon-circle` runs in the CLI checks, the 20.5 percent figure was not re-measured.
- **OPEN (SEV-3, data):** every cast delivery figure comes from ONE follower (Jesper) in two sessions of one day. Small rows: buff first fire n=7, offense concentration n=9, heal n=8.
- **OPEN (SEV-3, model):** the unfired share counts claims the game ended for reasons the lab also models (target death, a re-target), so delivery failure is over-stated by an unknown amount. `--deliver-unfired` scales it.
- **OPEN (SEV-4, model):** the repeat window (2 s) and the climb (a path three times the ledge height) are assumptions. The entry foe is a model of Harbinger's Standing entry.
- **OPEN (SEV-4):** `StrikeReady` for a bow still reads the chase cap (the game's own code, candidate to move to `InWeaponRange`).
- **OPEN (SEV-4):** the survival band numbers (0.35 downs a fight, 25 to 75 percent) are judgement, not a card value.
- **FIXED here (pre-existing):** the run.sh logistics CLI check expected `--logi-policy iaus` to be refused. It failed at f5249bce before any edit of this batch.

### feat/sim-mirror-1117 review round 1 (Opus 5.5 @ 5749026a; fixed in the next commit)

- **F1 (SEV-2, FIXED):** the unfired shares counted windows whose target died and NOT BUILT windows of spell shapes Harbinger never builds a caster for. Both are out of the shares now: offense 0.344, offense concentration 0.182, heal 0.111, heal concentration 0.091, a cloak 0.125. The buff row is a cloak's only: summons, wards and armor spells always fire after the pooled first-fire time (assumption, never sampled).
- **HARBINGER (to file):** no caster is built for Frost Cloak 0003AEA2, Flame Cloak 0003AE9F, Lightning Cloak 0003AEA3 ("no-row"), Frostbite 0002B96B and Creeping Cold 800279BD ("av26 row-without-caster"), Turn Lesser Undead 0004D3F8 ("TargetEffect no-row"). Counts: iaus-sim-v3.md 13.2.
- **F2 (SEV-3, FIXED):** `load_cards` parses `inRange()` and refuses a raw `Band{ }`.
- **F3 (SEV-3, FIXED + NATIVE FOLLOW-UP OPEN):** `t.waivedRangeX` and `t.bowRange` are out of the driver's `GLOBALS`. Native: add both to `SetTuningScalar` (iaus/Catalogue.cpp), then search them.
- **F4 (SEV-3, FIXED):** the entry foe is behind `--engage-on-sight` / `FightSpec::engageOnSight`, OFF by default.
- **F5 (marth ruling "Oracle downs", DONE):** the caster survival bands are oracle-relative (`downs/f`, `selfHealLow%`), measured with the gap pass's `ORCSURV` line. OPEN SEV-4: the bounds are static numbers from one 160-fight oracle run and must be re-measured when the world or the oracle changes. OPEN SEV-4: a tiny `downs/f` bound costs almost no persona credit (the credit's scale has a floor of 5).
- The earlier entry "the survival band numbers (0.35 downs a fight, 25 to 75 percent) are judgement" is superseded by F5.

### fix/loot-1117c (author notes, not a review round)

- **OPEN (SEV-4, not done):** `kTeamDispatchSpreadU` (spread teammates' dispatches) was NOT built: a distance-to-teammate-target skip would starve clustered loot (a museum shelf is dozens of items inside 100 u). It needs a sort tier (like the actor-block tiers), not a filter.
- **OPEN (SEV-4):** the museum deposit's root cause for the four unmoved items (Honed Falmer Sword, Silver Greatsword, Deathbrand, Bonemold Helmet, 11-17e) is NOT proven from the log: every silent skip now logs, and a RemoveItem that moves nothing names the item (worn plan, unequip asked). Read the next run's `[lotd] ... transfer SKIPPED / MOVED NOTHING` lines before changing the unequip order.
- **OPEN (SEV-4):** not equipping museum-bound weapons as the off-class fallback is not changed here (the fallback pick is `cast/Equip.cpp IsMuseumRelic` / the hold, not `OffClassTick`).
- **OPEN (SEV-5):** `batchHoldMinSec` 3 (marth 11-17: "Keep 3 s") and `offClassDwellSec` 3 are placeholders for the sim / field; `gateReadmitMoveU` / `gateReadmitSec` default 0 (off).
- **OPEN (SEV-4, review):** books such as 'Boethiah's Glory' also fail every trip, so the cause is not gems or worn copies: suspect the `delta <= 0` RemoveItem path in `TransferOnMain`; the new `MOVED NOTHING` / `SKIPPED` lines will say.
- **OPEN (SEV-5, review):** `SetLootKnob` range message text for non-unit caps (`gateConeDeg` <= 90) says "> 0 and <= 1".
- **OPEN (SEV-5, review):** `LootPartyInCombat` comment: the [sense] foe term is implied (foes are tallied only for followers already in combat).
- **OPEN (SEV-5, review):** `GuardKeepWarm` doc says "stamps nothing"; it does set `e.posted`.
- **OPEN (SEV-5, review):** the transfer SKIPPED line's "the world changed since the plan" also covers a cached-vs-live gem verdict disagreement.

### fix/style-before-entry (author notes, not a review round)

- **OPEN (SEV-4):** the pre-entry style is the RESTING stance only (class override / his class's root T1). A follower whose stance in the fight comes from an equip gambit or a cast latch (not the class root) gets the resting class style at entry and a Repoint when the fight's own Want lands (~seconds later), i.e. the old gap for him. Evaluating the combat table pre-entry was not done.
- **OPEN (SEV-5):** a perk-voted dual wielder's pre-entry melee style is the single-weapon flavour (EngageOnSight has no FollowerState for `CombatStyleFor`); the fight's Want re-points it.
- **OPEN (SEV-4):** a through-teammate join sets the entry's engage range to the LEASH; Harbinger's range drop is then the leash, so a foe the teammate fights far from the joiner can pull him a long way (marth ruling 11-17: within the leash). Tune the reach if the field shows over-chasing.

**fix/cast-stutter (2026-11-17, author notes, no review yet):**
- OPEN (SEV-4): the occluded-claim bound covers OFFENSE claims (CastOn) and HEAL claims (HealRoad); a buff claim aimed at an occluded ally is bounded only by BuffRoad's own never-fired bound.
- OPEN (design): the heal-claim expiry sweep (apmf/Bridge.cpp) still ends a heal nobody refreshes while the rule is "asking" (matched/asked skips in `HealSustainLap`); the root cause of the unrefreshed claim was not traced, only its sight half is bounded.
- OPEN (sim): `tools/iaus_sim` mirror not done for `t.occClaimRelease`, `t.offApproachBackoff`, `t.sharedRadiusMargin`, `t.preemptSettle`.
- OPEN (Harbinger): self Frost Cloak never charges; OOC substrate Candlelight closed-unfired 15/16 (ClickUp 1dfcuabdb2u?).

**fix/cast-stutter review round 1 (Opus, 3c325324): SEV-2 F1/F2 and SEV-3 F3-F6 fixed in the next commit. Deferred (SEV-4/5):**
- OPEN (SEV-4): `g_healOccludedBlock` and `g_unsightedCharge` hold one entry per follower (a second released target overwrites the first; the first may re-claim once and be released again).
- OPEN (SEV-4): `WithinSharedRadius` is the reader of the heal-claim picker and MaintainCompanion only; the buff series (Auto.cpp), Hands.cpp `IncumbentTargetLost` and Evaluator `PickAlly` still read plain `g_sharedRadius`.
- OPEN (SEV-5): `CanPreemptHand`'s `s_settleLog` throttle map is not cleared on revert (a throttle only).
- NOTE (principle 9): Sightline's 1 s freshness vs a service lap over 1 s with 8+ followers makes the occluded-claim bound fail safe (it does not fire); noted at `Tuning::occludedClaimReleaseSec`.


### fix/cast-stutter (Opus 5.5 re-review, 2026-11-17, against 1569f497): MERGE
- **(SEV-4):** OffenseApproach.cpp:413 `static_cast<int>(offenseApproachDropCap)` is UB above INT_MAX (SetTuningScalar only rejects negative / non-finite): clamp before the cast or bound the scalar.
- **(SEV-4):** CastOn.cpp:719-735 the Sightline-timed fallback also runs for IAUS followers whenever OwnOccludedAgeSec returns 0 (incl. a fresh not-occluded record): two readers of the bound (fails safe via the stamp min).
- **Note:** with 8+ followers the occluded bound may never fire (Sightline 1 s freshness vs the lap): watch in the field.

### fix/heal-timing (author notes, 2026-11-17, awaiting Opus review)
- **(SEV-4, author):** the gross floor uses the attacker's estimate from ITS LAST lap (one lap stale; first hit of a new attacker has no floor, only the net drop).
- **(SEV-5, author):** `kGrossWindowSec` 4 s / `kMaxHitWindowSec` 8 s and `t.dyingHitsToDown` / `t.urgencyHitsDiv` / `t.dyingAllyHp` are placeholders for the sim to search.
- **(SEV-5, author):** a hit with no TESHitEvent (poison, cloak tick, script damage) is gross-counted by its net drop only and never enters `maxHit`.
- **Review round 1 (Opus, 2de581de), FIXED in the next commit:** F1 SEV-2 party heals / buffs raise TESHitEvent on allies and were floored with the healer's attack estimate (phantom gross, feedback into the dying boost): the sink now drops events caused by a tracked follower or the player. F2 SEV-2 + F3: maxHit was gross / event count (a 5-event volley read 33 not 166; a drop-only lap recorded 0): maxHit is now the largest per-LAP gross (`Estimate::LapGross` / `BurstReads`). SEV-4 harness rows asserted nothing / only !=: now directional, with pure-function rows. SEV-5: log text names bIausFillLoss; the two windows moved to `Tuning`.
- **Re-check round (Opus, a30ba2ee):** SEV-3 FIXED: a lap for an ally is one sample interval, so maxHit is now the largest SUM of samples inside `Tuning::burstWindowSec` (0.5 s, PLACEHOLDER); `t.dyingHitsToDown` default 1.0 -> 1.2 (placeholder). **SEV-5 (deferred):** untracked party-side actors (summons) still count as `taken` when they cause the hit (the cause test is the tracked-follower mirror and the player only).
- **OPEN (SEV-5):** (fix/loot-gated, 11-17g) `gateRepeatU / gateShortU / gateNearU` (300 / 300 / 200) and `gateReadmitMoveU` (default 500 when added, now 0 = off since 11-24) are placeholders for the field; `kStuckTargets` is now 2 consecutive blocked ends within 300 u (the same target counts again).
- **OPEN (SEV-4, pre-existing):** `GuardHeldAt` is not stamped on the loose-item arrival-Pending path (only the corpse path stamps it). Raised against bdf1923a.
- **OPEN (SEV-5):** `FindGateObstacle` counts ANY enabled activator on the way (outdoor trigger boxes / critter spawners can still gate); only the lane/reach test narrows it. Raised against bdf1923a.

## feat/oracle-unbounded (Fable author, 2026-11-17 and 11-18, tools/iaus_sim only; record: Docs/research/2026-11/iaus-sim-v3.md sections 14 and 15)

marth's standing tuning rule of 11-17: the oracle scores every candidate at every decision point, races its streams to a statistical separation (safety ceiling 128), and is held to the class's abilities and rulings. Fix round (Opus review of d8b686a7, marth's rulings): random streams per actor inside a rollout, the bash and the guard under the game's cooldowns, hide and flee only as the game's own runs, the sprint away for a class with the disengage flag. Same batch: the field damage read and the hit-size spread.

- **FIXED (review round 1 @ d8b686a7):** F1 the ledger logs only picks the race dropped. F2 `ext:block` needs the class's Block row and runs under its cooldown, `ext:bash` under `bashCooldownSec`. F3 Utility's magicka term is for casters only (baselines: U lines only, at most +0.13). F4 and the hide and flee rulings: the oracle has no run of its own. F5 every target of an extended action is offered, what remains is declared in 14.10.
- **OPEN (SEV-3, method, review F6):** an option can be dropped after 8 streams by a test that assumes a roughly normal paired difference, with no multiplicity correction over the five looks. Utility differences are heavy-tailed. No measurement of the wrong-drop rate exists.
- **OPEN (SEV-3, method):** the ceiling still ends 27 to 82 percent of decisions after the streams per actor (21 to 67 on the five classes of the A/B, from 75 to 84). A caster's options are often the same spell on another foe.
- **OPEN (SEV-3, evidence):** `World.h kFieldHit` (the v3 switch `hittail`) is sixteen hits of one fight of one session (deck-run-1117f). It moves every J of the v3 world (U -4.56 over the 90 baseline rows) and applies to the player model too. Re-derive when more sessions carry `bIausFillProbe`. `--v3 hittail=off` is the lab before it.
- **OPEN (SEV-3, model):** the natural bracket is 11 to 28 times gentler than the field day in downs a follower-minute (section 15.4). The bands gate on natural rows. Whether they should also gate under pressure (`--gap-bracket`, section 15.6) is marth's ruling.
- **OPEN (SEV-3, mirror):** the native heal-timing fix (`fix/heal-timing`) was not on origin when this batch closed. Mirror it, then re-measure 15.6.
- **OPEN (SEV-4, review F7):** the survival, heal-timing and ally-survival bounds are measured with every follower oracle-planned, static numbers from one 160-fight run. Re-measure when the world or the oracle changes.
- **OPEN (SEV-4):** the potion-worth tables were measured before F3 and before the hit sizes. The driver's prep stage should re-measure them before the retune reads them.
- **OPEN (SEV-4):** five rows still hold `healAt% <= 75` (a cap on the mean health at the heal). The oracle heals at 67 to 75 percent, so they sit at the edge of the best play. Only the Healer's and the Priest's `<= 60` were replaced by `healLate%`.
- **OPEN (SEV-4, model):** a rollout holds an option for 2.5 s and the search is one step deep over IAUS. The game's top-five foe list bounds the catalogue. Declared in 14.10.
- **OPEN (SEV-4, cost):** a gap stage pass is about 3.4 hours on 16 cores per world and the 160-fight band measurement 139 minutes. `--oracle-ceiling 32` is the lever.
- **OPEN (SEV-5, review F8):** the diagnostic throttle's sneak hold, the duplicate rule (two options with the same outcome on every stream count as one), `edge_search.py:99` runs the oracle too.
- **ASKS (field):** run every session with `bIausFillProbe = 1` (one of three had it), and one passive line per foe with its current target.
- **NATIVE FOLLOW-UPS (a tune cannot close them, section 14.10):** no step back road (melee 30 percent of his divergence cost), no Regroup client (`Snapshot::regroupRoad`), the Bash family's road, the vetoed summon of the healer and the evoker, no "clear line of fire" family, potions scored under the floor.

### feat/oracle-unbounded (Opus review 2026-11-18, against 610e2952), open before the retune
STATUS 2026-11-23 (`feat/sim-v4-s1`, iaus-sim-v4.md section 9): F-A FIXED @7141eeca, revised in the review round (per-event table: the 27 plain single blows of five sessions, each over its attacker's own printed after-armor estimate, reproducible by `calib_damage.py`, kOneHitLine 0.35). Heal-timing mirror FIXED @ec6cf706 (v3 switch `burst`); the ORCSURV / ORCHEAL re-measure is NOT run (about 7 CPU-hours a class): a listed pre-tune step, the band rows of `Playstyles.h` are marked stale. F-B FIXED @ee73e70f. F-C FIXED @ee73e70f for the statistics (ORCSTOP / ORCREFUSED count the subject only); "bands measured beside oracle companions" stays OPEN (marth's decision 3 in iaus-sim-v4.md 11). F-D: the uncounted heals FIXED @ee73e70f (scroll and engine heals are tallied in `World.h StartCast`); the band choice stays OPEN (decision 2).
- SEV-3 F-A, tools/iaus_sim/World.h ~763 kFieldHit: "Each value is a window of health loss (took = before minus after over 0.13-0.55 s), not one hit." The 3.13 value is a 5-event window, 92.6 is likely a power attack (other source form), and the lab already applies powerMult and per-arrow hits, so the tail is stacked on modelled spread; kOneHitLine 0.30 inherits it; calib_damage.py cannot reproduce the 16 values. Re-derive per event (de-duplicated, excluding power/sneak/enchant) or accept as a labelled assumption BEFORE the retune.
- Heal-timing mirror missing: the sim never fills Snapshot.grossPerSec / maxHit, so recipientHitsToDown stays +inf; t.dyingHitsToDown / urgencyHitsDiv / burstWindowSec / grossWindowSec / dyingAllyHp are flat knobs; a retune would over-correct heal thresholds (double correction in game). Mirror, then re-measure ORCSURV/ORCHEAL.
- SEV-4 F-B, Data.h:68: V3::Any() omits hittail (a v2 run with hittail=on is tagged v2).
- SEV-4 F-C, sim.cpp:998-999: ORCSTOP / ORCREFUSED merge companions under the subject's class label; the tree-ON warrior CLI check can be satisfied by a companion; bands measured beside oracle companions.
- SEV-4 F-D: the healer healLate band does not bind on the natural bracket (IAUS 1.9 vs bound 8); needs marth's ruling on a pressure-bracket band row. Ext::Scroll heals and engine-AI heals not counted in healAt/healLate.

## 2026-11-19: tune_driver ship.needs_joint_rerun stays set after a passing joint rerun (SEV-4, stage-order bookkeeping)
FIXED 2026-11-23 @037b17f8 (`feat/sim-v4-s1`): `final_curves()` leaves out the withheld values, the joint stage grades that table, `ship_state()` clears the flag after a passing joint on it; `test_tune_driver.py ShipState`.
Raised against 77c607a6 (run 11-18). When the recalib stage withholds a value (11-18: `mercenary:0.7.a=0.931` withheld) the driver sets
`ship.needs_joint_rerun`. The joint rerun then PASSED twice, but the flag stayed set: the summary kept saying the joint check was owed.
It is bookkeeping across stage order (recalib runs after joint; nothing clears the flag when a later joint pass covers the withheld
value's final table). No wrong value ships; the report only misleads. Fix: clear the flag when a joint stage passes on the table that
recalib left (or compare the table hash the joint passed on with the final one).

### feat/tune-24-classes (Opus review 2026-11-19, against f26dac4a)
- SEV-4, tools/iaus_sim/tune_driver.py:94 / :326: "TIGHT = {"thief","mercenary"} is not extended. master-thief, nightingale, nightblade and listener get the 0.5 J hard band instead of 0.25." Needs marth's ruling. RESOLVED 11-19: marth "Strict of course." -> TIGHT extended.
- SEV-5: --parked under --cells all also turns on parked scenarios, so the 15 old cells' numbers in an all-run are on a different fight set; do not compare directly.
- SEV-5: "15 cells" text left in docstrings / summary.md (:27, :54-55, :1054-1057, :1086/1092, :1598, :1694-1709, :386, :2001-2002); SUMMARY["config"] does not record cells. FIXED 2026-11-23 @037b17f8 (the text says "every cell" or the real count; config records `cells` and `cell_list`).

## 2026-11-19: fix/unservable-claims (author notes, SEV-4)
- SEV-4: Blur (FE13282A), a concentration Self buff predicted NoRow, was never classified by Harbinger in field 1119 while Longstride / Detect Life (same shape) were served as Script. Cause unexplained (Harbinger side); only the never-fired release bounds it (BuffRoad's own).
- SEV-4: unseated rows (TargetEffect / Stagger / Disarm / Paralyze) have no road; needs a Harbinger tier-A brief (seat them, or force the key for a hostile claim). Reader: `HarbingerSeatsSpell`, no caller.
- SEV-4: Blur stalls ~6.5 s per reset (BuffRoad never-fired release); cause Harbinger-side.
- SEV-5: IAUS target switches re-stamp lastSeen, so the offense never-started window may not be reached (watch in field).

## feat/sim-v4-s1 (Fable author, 2026-11-23, tools/iaus_sim + docs only; record: Docs/research/2026-11/iaus-sim-v4.md section 9)
Author's notes, open after the slice (none above SEV-3 known to the author):
- **OPEN (SEV-3, evidence):** `World.h kFieldHit` is 27 plain single blows out of 139 windows of five sessions, each over its attacker's own estimate, so part of the spread is the estimate's error between attackers. `calib_damage.py MIN_SHARE` 0.25 is a labelled assumption (it removed one window). Re-derive as sessions with `bIausFillProbe = 1` accumulate.
- **OPEN (SEV-3, model):** the plain blow is 19 percent of the field's loss windows. Riders on foe blows are 25 percent (not modelled in the lab) and foe spells 10 percent with the worst tail (p90 71 percent of the target's health; the lab's foe spell sizes are unchecked against it). No window holds blows of several attackers or instants (0 of 139), so no multi-blow burst is modelled or needed. `calib_damage.py` prints each kind on its own line (iaus-sim-v4.md 9.2, 4.3 item 5).
- **OPEN (SEV-3, stale bounds):** the oracle-relative band rows of `Playstyles.h` (downs/f, selfHealLow%, healLate%, dwh/f) were measured before this slice's three world changes. Re-measure before the next tune (command in iaus-sim-v4.md 9.6).
- **OPEN (SEV-4, mirror):** the lab's attacker estimates are closer to the real hit than the game's (which read 2 to 6 times under), so `Estimate::LapGross`'s floor binds more often in the lab than in the game. The three heal windows are not searched because the lab samples every 133 ms and the game every 133 ms x party.
- **OPEN (SEV-4, mirror):** the five 11-17f cast-layer scalars (`t.occClaimRelease`, `t.offApproachBackoff`, `t.offApproachDropCap`, `t.sharedRadiusMargin`, `t.preemptSettle`) and the twelve new `LootTuning` rows are not mirrored (slices S6 and S8 to S10).
- **OPEN (SEV-4, tools/tune_app, not this branch's files):** `tuningfiles.py T_SCALARS` lacks `t.dyingSelfHp`, `t.dyingAllyHp`, `t.dyingHitsToDown`, `t.urgencyHitsDiv` and the three windows, which the game's loader accepts and the driver now searches; the app should start rounds through `live/run_round.py` and read `live/priority.py` for its drop-down.
- **OPEN (SEV-5):** `gen_class_files.py` writes class files only. Accepted shared values (worth, loot, globals) are reported by the driver and not written.
- **OPEN (SEV-5):** the generator's log check reads the `classes <cls> pass N <knob> a -> b train ... ACCEPTED` line shape of `progress.log`; a change of that shape silences the check (it then finds no accepted line and passes). It compares knob and value against `stages.classes.<cls>.rows` since the review round.
- **OPEN (SEV-3, NATIVE finding, Opus review of 1874f81e; not this branch's files):** `iaus/Fills.cpp` floors a lap's gross damage with the attacker's estimate once per `Taken` (`:530-539`), and the sink pushes one `Taken` per `TESHitEvent` (`:216-218`). The engine raises the event twice for most blows and once more per rider (field lines: identical time, pair and window), so one plain blow floors at 2x the estimate and an enchanted one at 3x or more. The estimate reads about 2.2x under a real plain blow (27 field blows), so the doubled floor is near the truth for a plain blow by accident and over it for a blocked or glancing one. Fix shape: de-duplicate the sink by (attacker, target, instant), or floor with the largest estimate, not the sum. The lab's `NoteHit` counts a blow once (so its floor is the single estimate, and the lab's estimates are closer to the real hit than the game's).
- **Review round of 2026-11-23 (Opus review of 1874f81e), FIXED on the branch:** kFieldHit over 27 blows; the generator refuses smoke rounds, rounds whose joint never ran and partial rounds, and compares accepted values knob by knob; `final_curves()` keeps one value per knob (rejudge against classes); the joint stage runs again in the same run after recalib withholds a value; `config.bin_sha` and `carry_over` across binaries; `run_round.py` lock, original arguments, binary hash, an outside stop recorded as stopped; the manifest names shared values no file carries. The design doc: ch.24 `kApproach_Point` is the held-point road (H1 shrinks), no veto for unseated rows (marth: run every spell), one decider for position, the batch list.

### feat/adopt-seats re-check (Opus 2026-11-23, against a2104555)
- SEV-5 B: the engine's Disarm test also needs the weapon DRAWN (AE 0x678C2D, weapon state >= 3) and a 0x2235A0 test not identified; MFO accepts a sheathed weapon, an early Disarm may be spent.
- SEV-5 D: SeatRows::NeedMet has no game caller (WillWork inlines it), its harness rows prove nothing; HoldLedger and Actuation.h comments still describe the one-reading hold.

### feat/tune-app-priority (author note 2026-11-24, b179c001)
- SEV-4, tools/iaus_sim/live/run_round.py: driver_args is written once and compared on every resume even after --new-args was accepted (driver_args_now is kept apart), so every later resume needs --new-args again. **FIXED 2026-11-24 on feat/sim-v4-choice** (the resume is compared with the arguments in force, `driver_args_now`; `test_live.py` covers it). The same shape is still open for the BINARY: `bin_sha` is compared with the original after `--new-bin` was accepted (`bin_sha_now` is kept apart), not fixed there (outside that ruling).
- SEV-5: a real Stop of a wrapper-led round from the app was not re-run end to end after the fix round.
## feat/sim-v4-world (Fable author, 2026-11-23 and 11-24, tools/iaus_sim + the pure native/iaus EnvQuery / PathSearch files + docs; record: Docs/research/2026-11/iaus-sim-v4.md section 13)

### Opus review 2026-11-23, against 37a705e6
The review file on disk (`_agentlogs/mfo/review-sim-world.md`), verbatim:
> Review 37a705e6: FIX LIST. v2/v3/v4 compares 0 changed; env-test 52 pass; threads 1=8.
> F1 SEV-2 EnvWorld.h:174 straight-line shortcut has no z check: 217/11266 solo walks end under/over the goal, 216 with zero stuck tally; adding a 16u z check -> 1.
> F2 SEV-2 blasts/areas 2D through walls/floors World.h:1322,1222. F3 SEV-2 backpedal = 65% of evoker stuck% (2.53 -> 0.88). Rest in hand-back. Test: scratchpad rsw/t.cpp

F4 to F13 and the native API items reached the authors as the coordinator's fix list, by number and a one-line title. Their verbatim review text is in the reviewer's hand-back, which is not on disk. The coordinator should paste it here. All of F1 to F13 are FIXED on the branch (what was built and its check, per finding: iaus-sim-v4.md 13.4 and 13.5). The fix list's wording of the two that were finished last:
- F11: "sight-driven weights are not tunable until the engine-ray parity probe: enforced by a skip list in tune_driver, not only text". FIXED (`tune_driver.py sight_gated`).
- F8 leftovers: "want gaps reset the stuck window, approach backoff counted as stuck while a capped foe counts zero, UNREACHABLE / STUCK repeats at one spot free, detour over FindPath length". FIXED (`EnvWorld.h EnvMove` / `EnvLegEnd`).

### Opus re-check 2026-11-24, against 91946263
The review file's re-check block, verbatim:
> Verified: env-test 74 PASS; v2/v3/v4 compares 0 changed; solo walk fixtures 657/0 wrong, library 10620/2 wrong (doc says 3)/0 over-under; tune driver tests 39 OK. F1-F13 + native API fixed in code except as below. SeatRows.h standard-only, unwired native.
> R1 SEV-2 World.h:1539-1540 / 1623-1624 (Brain + Feet): caster osc is LAB DITHER. No dwell on sight: LoS lost -> walk in; next step LoS back and d<320 -> back off behind the same corner. 9219 reversals (6 caster classes, 2016 library fights): 71% within 0.5 s of the last leg, trace alternates every 0.03-0.10 s. Native: OffenseApproach.cpp:71 kSightDwellMs 1500 on claim AND release. Scratch experiment (1.5 s dwell on los at both sites, env only): osc/min mean 11.85 -> 4.47, failing cells 32 -> 5, stuck% 0.34 -> 0.24, U 76.61 -> 76.29 (84 cells). Residual 3447: 2022 side-step related.
> R2 SEV-3 EnvWorld.h:98-99 door BLOCKED = permanent per-foe no-walk for every class; native floor-give-up is not "a door"; native blocks lapse (BlockStands), an undone path = backoff, and only bound the ch.24 claim, not melee pursuit. EnvWorld.h:248.

R1 and R2 are FIXED on the branch, with the coordinator's small items (net goal displacement in the detour, `no legs` detour cells said so, `FindPath` and a shut leaf inside one triangle, `Invalid()` on wall and leaf floats, the solo-walk count, 13.6). What was built and its checks: iaus-sim-v4.md 13.9.

### Author's notes, open after both rounds (none above SEV-3 known to the author)
- **OPEN (SEV-3, gate):** 5 of 210 library cells fail the placeholder bounds: ranged and evoker on `exterior_many_doors` (a foe behind a door the follower cannot open: reblock/f, hold%, holdUnreach), and detour p90 for ranged on `exterior_actors_containers` (5.00), warrior (4.00) and ranged-off (3.25) on `interior_small_multilevel`. Not chased: whether they are odd pathing is for the oracle pass (`--path-oracle`, iaus-sim-v4.md 10.3 item 3).
- **OPEN (SEV-4, assumption):** `EnvTuning::backOutU` 100 and `awayCommitSec` 2 s (the back-off's hysteresis and how long its point stands) have no field number behind them. `sightDwellSec` and `blockLapseU` are native's own values, copied by hand: a change to `apmf/OffenseApproach.cpp kSightDwellMs` or `kBlockMoveUnits` needs the same change in `Env.h EnvTuning`.
- **OPEN (SEV-4, mirror):** the sight dwell is applied to every body's walk-in in a place, foes included, and to a walk-in that is the engine's own combat movement and not the ch.24 claim. Native's `kOffenseRegularMoveMs`, the tightening radius and the ch.23 leash side of a block (`kBlockLeashGrow`) are not mirrored. A door block in the lab also lapses when any door changes, which native has no word for.
- **OPEN (SEV-4, measure):** the detour bound is not tested in the `no legs` cells (28 of 210 on the library, mostly bows). The report says so.
- **OPEN (SEV-3, assumption):** `--seat-immunity unseen` is the default: the game's will-work cannot read paralysis immunity and the two-zero-fires hold catches it (native `cast/WillWork.cpp` (c) says so). The lab's foes carry the `ImmuneParalysis` keyword and the extract holds no effect conditions, so whether a list's paralysis effects test it is not known in the lab. NATIVE NOTE (not this branch's files): the keyword is a plain record keyword, readable with `HasKeyword`, and `WillWork.cpp` does not read it.
- **OPEN (SEV-4, model):** a follower behind a friend who stands in front of a closed door sidesteps round him and back instead of waiting. The author's reading of one trace, not proven further: `EnvTryStep`'s tangent step succeeds, so the yield timer never starts. Measured: the doorway fixture, mage-off, 25 s before he opens the door, counted as stuck% 2.79 and repeat/f 0.38 in that cell. The engine's soft avoidance is not modelled (`yieldSec` is an ASSUMPTION). Counted, not hidden.
- **OPEN (SEV-4, assumption):** `EnvTuning::wantGapSec` 3 s (not asked to move for longer = standing by choice) and the zero-effect reading's timing (every fire read at the start of the settle window) are labelled assumptions with no field number behind them.
- **OPEN (SEV-4, bounds):** the two new gate rows (`hold%` 5, `repeat/f` 1) are placeholders like the other six.
- **OPEN (SEV-5):** the solo walk's 3 wrong trips of 10620 on the library copy (2 still walking at the time limit, 1 stuck, places x_Skyrim-050015_-1_2, x_Skyrim-02EE41_5_-9, x_Dawnguard-001408_-8_-1) are under the tenth of a percent the check allows and were not chased.
- **OPEN (SEV-5):** the adopt-seats mirror leaves out Disarm's weapon need, Command's summoned need, effect conditions and the approximate-row exemption (the lab has no such effect or record).


### feat/sim-v4-world closing pass (Opus 2026-11-24, against f2506977)
- SEV-4: SightWalk: a sighted walk-in runs on toward the foe for up to 1.5 s; native stops at its standoff.
- SEV-4: iaus-sim-v4.md 13.9 should state the legMinU sensitivity (mean osc over 8 classes 2.93 at 150, 5.01 at 80, 1.07 at 200).
- SEV-4: EnvAwayPick is blind to second foes during the commit.
- SEV-5: sightDwellSec / blockLapseU are hand copies of native constants; add a run.sh grep check.

### fix/spell-worth (author notes 2026-11-24, Sonnet; before the Opus review)
- SEV-4, sim mirror: `tools/iaus_sim/Policy.h:143` sets `measured = true` for every spell, so the lab never has an unmeasured offense spell and cannot search `t.unmeasuredDamage` (PLACEHOLDER 0.5); `Estimate::SpellOn` is shared with the sim, so skipping the element term on the primary AV and the poison swap move its ResistMagic / poison-type scores; `Facts::resist` (exploit) is now 0 for a measured offense spell in the game and not in the lab; the lab's control spells carry no `levelCap`; `t.unmeasuredDamage` is in the game's `SetTuningScalar` and the tune app's list only (sim `Tune.h` kTuneScalars lacks it).
- SEV-4: `SpellEntry::levelCap` is asked of the perk entry point against his combat target at book time (up to 5 s old, the record magnitude with no target), and applied to every foe; a perk that raises the cap only against some foes could rule a foe out that WillWork would pass (WillWork still asks per pair, so the failure is a lost candidate, never a cast that cannot work).
- SEV-5: a conditional direct effect (a CTDA on the effect or its MGEF) is counted in the damage (the reader has no target); the per-pair WillWork gate drops the pair it cannot hit, but the damage number of a spell whose only direct effect is conditional is an overestimate against a foe the condition fails for.
- SEV-5: the poison swap reads the foe's poison resist only when `Candidates.cpp` lists it (a poison-type spell is in the book); the engine's player cap on resist (`fPlayerMaxResistance`) is not modelled (foes only).
- SEV-5: `[iaus-fill] spell` is now logged whenever the spell fill is on (it was probe-only); the ledger prints each (follower, spell) once per session, about 74 lines per follower on a big book.
- (review round of 9f9734e2, author) SEV-4: DoT damage is still counted over its whole duration (no foe time-to-live reader exists to cap it at the foe's expected life). SEV-4: the mutually-exclusive-conditional-effect rule (sum unconditional, take the largest conditional) was NOT verified against the Elemental Bolt / Flare records (the logs carry no per-effect conditions): check them in xEdit-free form via a record dump before trusting the lower bound. SEV-5: the damage-spell cap exemption keys on a hostile Health value-modifier effect; Turn Lesser Undead's uncapped companion effect (0004D3F9 family) must not be one, unverified here. SEV-5: `Facts::bestDamage` changed unit (per cast second): the sim mirror reads it through Generate, so its DamageValue moves.

### fix/spell-worth re-check (Opus 2026-11-24, against 2b504275)
- SEV-3: a spell whose only damage is conditional, plus an unconditional rider, still counts in full against a foe the condition fails for (WillWork answers Yes there).
- SEV-4: SpellOn uses one element for the whole spell, a mixed-element spell with its keyed element at 100 is dropped.
- SEV-4: capQueued pairs go ahead of kept refusals; with about 18+ unanswered capped pairs live refusals are evicted (append them after).
- SEV-4: WillWork HasWorkingHealthDamage accepts PeakValueModifier and excludes Absorb, Fills::Classify the reverse: align.
- Settled: Turn Lesser Undead 0004D3F9 is Stagger, not hostile, not Health (Skyrim.esm, MysticismMagic.esp); Adamant's third effect is gated on HasMagicEffect 0004B145, so the cap exemption cannot fire above the cap.
- Open: DoT is counted over its whole duration (no foe time-to-live reader).

### feat/sim-v4-loot (sim v4 batch L; Opus review 2026-11-24 against 9689734b, fix round by Fable)

The review's record as saved (`_agentlogs/mfo/review-sim-loot.md`, verbatim; the full fix list S1 to S6 went to the author by message and is not on disk):

> review done: fix list (crimes row tautological; driver new_fail only; hasDefer not cleared; gateable by takeThreshold; Vanilla x_Skyrim-104217 = duplicate NAVMs). Native: N2/N3 proven 1118+1123; N1 code-proven; N6 gateReadmitMoveU 500 loop 1118 000EA786. worktree removed.

- FIXED in the round (doc `iaus-sim-v4.md` 14.9): S1 crimes row tautological, S2 driver new_fail only, S3 gateable by takeThreshold, S4 hasDefer not cleared, S5 gate rows for the excuses, S6 gate knobs not searched, the duplicate-NAVM place skipped by the loader, the batch W fixtures.
- NATIVE, NOT FIXED HERE (this batch edits no native file; each needs its own brief): N1 coin dibs need near AND facing (code-proven; marth 11-24: "Near is enough"). N2 / N3 the static-block and locked-door retry cycles (proven in the 11-18 and 11-23 field logs). N6 `gateReadmitMoveU` 500 lifts a gate by its own follower's walk (loop in the 11-18 log, item 000EA786; marth 11-24: real events only, a route-based lift later). N4 the carry peek weighs equipment only. N5 a guard answer asked on the way is used unread at arrival.
- OPEN, SEV-4 (author): `origin/main` 2d617b7d fails its own three sim baselines (`--world v2` 824, v3 1099, v4 899 metrics changed) since `fix/spell-worth` merged without rewriting them. This branch plays the same fights as main (0 changed against baselines written by main's binary) and did NOT rewrite them. Whoever owns the spell-worth change rewrites the three files after reading the diff.
- OPEN, SEV-4 (author): the gate COUNT of rows over their bound is not a judge. A table that walks less reads lower on `legs/src`, `restart%`, `reversals/min`, `stuckOff` and `guardSeenTakes`, and where the shipped table is already over a row a worse number adds nothing to the count (takeThreshold 1 ties the shipped count on three of seven sets). The driver compares each row with the shipped number instead. Bounds stay PLACEHOLDERS for marth.
- OPEN, SEV-4 (author): the wait rows now hold a censored wait for every wanted stack left released and reachable, so on the library (`leftRR/ep` 1.2 to 3.1) both read the episode's 90 s and no longer tell sets apart. A separate "p90 of what was taken" row would bring that back.
- OPEN, SEV-5 (author): `LOOT_NOT_SEARCHED`'s reason for the gate rows says the field blocks 2 to 5 times as often as the lab. Measured after the round, under today's rules on Tuxborn: `leg ended BLOCKED` 0.19 per follower-minute against the field's 0.20 to 0.68, so 1 to 3.5 times. The reason stands, the figure in the string is the older one.
- OPEN, SEV-5 (author): the oracle is greedy in decision order (a lower bound, doc 14.5). A search over orders or followers is not built.

## feat/sim-v4-loot review round 2 (Opus 5.5, 2026-11-24, raised against 59d0a63d; verbatim)

R1 and R2 are to be fixed BEFORE any loot tune runs (extension point: tools/iaus_sim/LootLab.h Score, tune_driver.py loot_gate_check). R6 is rewritten once with the choice batch.

SEV-3 R1 (before any loot tune, not a merge blocker). The search objective charges shipped's OWN N5 guard-seen takes at 1000 each (LootLab.h Score, kCrimeJ), so a table that takes LESS earns the fine back. 1920 eps, all in-grid single-knob steps:
  set            shipped guardSeen  loot.buffScale=0.475 value / charged   loot.foodScale=0.45 value / charged
  LoreRim        13 -> 0            +1.36 / +8.13                          +1.14 / +7.91
  LoreRim held   98 -> 1 (food 2)   +1.22 / +51.74                         +0.97 / +50.97
  Tuxborn        0 -> 0             +1.15                                  +0.43
  Tuxborn held   94 -> 14 (18)      +2.48 / +44.15                         +1.49 / +41.07
  while leftRR, roadLeft%, walkedPast (and leftOutOfRange on those sets) get worse. tune_driver.py:loot_gate_check would only make them an open decision; the hard row improves, held-out value improves, so the table is KEPT. Not proven accepted end to end (I did not run the driver).
  Part of the gain is real (+1.15 on Tuxborn with no guard takes: potion/food trips cost gold trips), part is N5's artifact.
  FIX: charge the excess over the shipped table's count on the same seed/episode, or train on rules with N5 fixed, or search uncharged and keep the hard reject.
SEV-3 R2. loot_gate_check FLAGS, never rejects, except the two hard rows. Under --decisions auto, open_decision (tune_driver.py:587) becomes a note and the kept table stays in the summary. No noise band: one take in 480 eps flips a row, e.g. takeThreshold .667 (value +0.01) worsens 8 rows on Tuxborn, gate0 7, so every searched table will be flagged and the flag stops meaning anything. FIX: paired per-seed test per row, reject (verdict="rejected") on leftRR / goldLeftRR / roadLeft% / dibsNear beyond noise.
SEV-3 R3. "So no row gets better because a table wants less" (doc 14.3) is not true. All four free categories at 0 (loot.gold/equipment/valuables/jewelry=0): leftRR not worse on any of the 7 sets; wait dibs 0 on all 7 sets (60..90 shipped); over-bound rows LOWER on Vanilla (7 vs 8) and Vanilla held-out (5 vs 7), equal on Tuxborn, LoreRim and LoreRim held-out, higher on fixtures and Tuxborn held-out. Cause: the claim clock (Loot.h TierReleased) only runs for categories in the LIVE plan; an ignored dibs stack is "player dibs (never scanned)", which is not rr, so neither leftRR nor the censored wait (LootLab.h:257) sees it. Still caught: value falls 4.7 to 30.8 on every set, dibsNear and roadLeft% worse on every set. Zero is outside the search grid (grid minima gold .5, equipment .4, valuables/jewelry .375), so it is a claim/robustness defect. FIX: run the claim clock for the shipped plan in the ledger, or count never-scanned in-reach stacks as rr; correct the sentence.
SEV-4 R4. Censored waits: sound as an anti-cheat idea but the rows saturate. Shipped, 480 eps, free/dibs: 90 free on Tuxborn, LoreRim and Vanilla and their held-outs except Vanilla held-out (67); dibs 87 on Tuxborn, 90 on the other library sets; fixtures 19/60. On library sets the free row reads its cap for nearly every table, dibs near it. Cannot mis-tune: no wait row is in the objective or a hard row. But at 1920 eps the dibs row is 84 on Tuxborn and LoreRim and 60..90 under other tables, so it is not fully dead. FIX: add "p90 of taken" beside it (already backlog) or a left-share row.
SEV-4 R5. gateEventsOnly: matches marth's ruling on gateReadmitMoveU 0 (memory big-sim-round-1123). Kept lifts: gateNearU (player/other follower with an ACTIVE slot within 200 u, straight line, so through a wall or the locked door itself, not "reached the item"; native does the same) and the idle own-record lift, which is a four-idle-tick clock and not an event. On Tuxborn 480 eps the default rules lift 9 idle / 0 near-or-walk; idle off changes leftRR 1.915 -> 1.938, restart 28.0 -> 27.5, small. The walk lift was 64. Under the new default locked-door-cycle is not exercised at all: the scenario is pinned to gateEvents=0. FIX: ask marth whether the idle lift counts; add a default-rules never-opened-door scenario.
SEV-4 R6 (pre-existing, confirmed). Five red baseline checks come from main. Both binaries: v2 824, v3 1099, v4 899 changed; main's baselines = branch's byte for byte (v2, v3 0 diff; v4 only the pool-hash line, old fixtures vs new). Rewrite together with the choice batch:
  tools/iaus_sim/baseline_builtin.txt (--world v2 --classes beta1), baseline_v3_builtin.txt (--world v3 = default), baseline_v4_builtin.txt (--world v4; NB its line 1 now fixtures#64b360f8)
  tools/iaus_sim/run.sh:79 J=79.31 (82.34, 76.28) -> measured here J=79.47 (scenarios 82.14, natural 76.80); run.sh:81 J=80.10 (75.52, 84.69) -> J=80.07 (75.45, 84.69). These are main-at-2d617b7d values; re-measure after the choice batch. run.sh:78 (J=69.13) still matches.
SEV-5 R7. LOOT_NOT_SEARCHED _GATE_WHY "2 to 5 times". Tuxborn 480 eps beta1: BLOCKED 0.192 per follower-min under today's native (the field ran that: 0.20-0.68 = 1.0 to 3.5x), 0.119 under the lab's default rules (1.7 to 5.7x, not like for like). Say "1 to 3.5 times under today's rules". Also in doc 14.5 (same sentence). The "no signal" reason holds: gate0 (cone 180, beyondU 100000, readmitSec 600) is inert or within noise on all sets.
Note: the batch hold at 600 s (out of grid) lowers value on every set and worsens 6 to 11 rows: flagged. "Abandoning legs" has no table knob; not tested as a rule.

### MFO-B863 (OPEN, deferred from the sim reviews, 2026-11-24; tools/iaus_sim only; MAP "SIM BUNDLE A")
Verbatim, raised at 59d0a63d (loot review r2):
1. SEV-3 R3. "So no row gets better because a table wants less" (doc 14.3) is not true. All four free categories at 0 (loot.gold/equipment/valuables/jewelry=0): leftRR not worse on any of the 7 sets; wait dibs 0 on all 7 sets (60..90 shipped); over-bound rows LOWER on Vanilla (7 vs 8) and Vanilla held-out (5 vs 7), equal on Tuxborn, LoreRim and LoreRim held-out, higher on fixtures and Tuxborn held-out. Cause: the claim clock (Loot.h TierReleased) only runs for categories in the LIVE plan; an ignored dibs stack is "player dibs (never scanned)", which is not rr, so neither leftRR nor the censored wait (LootLab.h:257) sees it. Still caught: value falls 4.7 to 30.8 on every set, dibsNear and roadLeft% worse on every set. Zero is outside the search grid (grid minima gold .5, equipment .4, valuables/jewelry .375), so it is a claim/robustness defect. FIX: run the claim clock for the shipped plan in the ledger, or count never-scanned in-reach stacks as rr; correct the sentence. (The sentence is corrected and `loot_gate_check` rejects on `dibsNear` and `roadLeft%`; the ledger part is what stays open.)
2. SEV-4 R4. Censored waits: sound as an anti-cheat idea but the rows saturate. Shipped, 480 eps, free/dibs: 90 free on Tuxborn, LoreRim and Vanilla and their held-outs except Vanilla held-out (67); dibs 87 on Tuxborn, 90 on the other library sets; fixtures 19/60. On library sets the free row reads its cap for nearly every table, dibs near it. Cannot mis-tune: no wait row is in the objective or a hard row. But at 1920 eps the dibs row is 84 on Tuxborn and LoreRim and 60..90 under other tables, so it is not fully dead. FIX: add "p90 of taken" beside it (already backlog) or a left-share row.

Verbatim, choice review, raised on the batch C branch:
3. SEV-4 proof coverage: the replay is 74 decisions, Tuxborn mage-line only; no LoreRim, conjurer, Knight, scenario fights.

### MFO-B864 (OPEN, author's own list for `feat/sim-v4-pretune`, 2026-11-24; tools/iaus_sim only; MAP "SIM BUNDLE A")
1. (SEV-4) A rune's damage lives in its own effect behind Target Location: no hazard road, so runes stay worth nothing in the lab and are dropped in the game.
2. **CLOSED on `feat/placed-l1m1` (L1).** (SEV-4) The lab keeps Vanilla's Wall-shaped bolts that native drops (MFO-B865): the lab's Vanilla books are not the game's.
3. (SEV-5) Hazards hurt only the owner's enemies and nobody avoids them (the engine's can hit anyone). A summon appears at once (no Landing state).
4. (SEV-5) The spell names the oracle picks are not printed, so "a wall among the picks" cannot be counted.

### MFO-B865 (OPEN, found by `feat/sim-v4-pretune`, 2026-11-24; NATIVE, not touched; needs marth's ruling on 1; iaus-sim-v4.md 16.6)
1. **CLOSED 2026-11-24 on `feat/placed-l1m1` (M1).** Was: `iaus/Prune.h:93` with `iaus/SpellBook.cpp:253`: a Target Location conjuration is dropped as Placement. Vanilla 14 of 14 taught conjurations, Tuxborn 307 of 311. A field log names one (Summon the Udefrykte Matron).
2. **CLOSED 2026-11-24 on `fix/impact-hazard-shape` (item 2 only; item 1 and the real-wall note stay OPEN).** Was: `cast/Archetype.cpp:214 HasImpactHazard`: any IPCT hazard makes shape Wall, so Vanilla's Firebolt, Flames, Fireball, Frostbite, Ice Spike, Ice Storm, Incinerate, Expel / Banish Daedra are dropped. Masked where the spell also has a Script effect (Tuxborn, most of LoreRim). Now: Wall = `Prune::IsWallShape` (concentration, non-Self, impact hazard with kDropToGround). Checked on Skyrim.esm: Firebolt / Flames / Frostbite impact hazards have flag 16 clear; only the Barrier* MGEFs (Fire, Frost, Shock walls) reach a flag-16 hazard.

## feat/sim-v4-pretune review (Opus 5.5, 2026-11-24, raised against 28c805e6; verbatim file)

P1 to P4 are to be handled BEFORE the tune (tools/iaus_sim/tune_driver.py, LootLab.h, the book extract).

# review-sim-pretune (Opus 5.5, 2026-11-24) feat/sim-v4-pretune @ 28c805e6. VERDICT: MERGE. Nothing above SEV-3. P1 to P4 before the tune starts.
Checks: own build at 28c805e6 = sim_final byte for byte (5a9a82bb). env-test 105, loot-test 24, driver 58, live 25 (1 skip). v2/v3/v4 compares 0 changed. Off against main's v4 baseline (meta retagged) 0. summons=off alone 0.
Stronger off proof (main's sim BUILT from origin/main, same list cache, fixtures places, 3 casters, fights 2): Vanilla off 0 of 4092 lines, Tuxborn off 1 metric (evoker strong2 sp.capTop%, consistent with the levelCap mirror fix, not traced), Vanilla summons-only 0, Vanilla on 583, Tuxborn on 446.
Not run: the long CLI suite, the oracle arms. pretune-smoke had only reached `spells` (13:44): the final driver path (spells set check, joint, gap) is UNPROVEN end to end, and follow took 7778 s for 2 cells x 2 worlds x 1-2 seeds (re-estimate 15.7).

P1 SEV-3 (tune blocker for Vanilla / conjurer / warlock) lab book != game book. tools/iaus_sim/Data.h:521 drops delivery TargetLocation only; native SpellBook.cpp:253 also drops Rune, Wall, TLBurst.
  Vanilla taught: lab keeps 13 Offense. 8 aimed bolts / streams with an impact hazard, no script (Incinerate, IcySpear, IceStorm, IceSpike, Fireball, Firebolt, Flames, Frostbite) + 3 Walls are Wall shape in game: 11 spells IAUS never gets. FireStorm (Self, impact) has an impact hazard too.
  NEW (not in B864/B865): REAL WALLS too. Fixing HasImpactHazard for bolts does not close it: native still drops a Wall as Placement, while the lab offers it and section 16.3 just gave it damage. So item 6's wall value trains on an action the game cannot take. Noscript aimed impact rows: Tuxborn 6, LoreRim 5 (4 FF + 1 conc).
  Conjurations: Vanilla 14/14, Tuxborn 307/311, LoreRim 204/224 TargetLocation. Vanilla natures: summon=0, summonUp% 0 for conjurer / warlock / evoker. Their cast_summon rows can only be judged on the fixtures.
  MUST BE TRUE before the tune: (i) marth rules on B865.1/.2; (ii) native fixed (summons exempt from the TargetLocation drop, their road aims; HasImpactHazard = concentration stream + drop-to-ground hazard, or explicit keep), verified in game or at least reviewed; (iii) the lab's StaticDrop uses the SAME predicate, from extracted booleans (impactHazard incl. spell-less hazards: hazard_row returns None for those, so `hazard` is NOT that boolean; scriptRider; rune / TLBurst), real Walls dropped unless native keeps them; (iv) lists re-extracted, a count test lab drops == native census on one list.
  If not fixed in native first: mirror the drop in the lab now, take Vanilla out of --worlds for caster cells, and do not accept cast_summon / summon rows for conjurer and warlock.
P2 SEV-3 loot search cannot produce a table. tune_driver.py stage_loot_episodes / loot_judge: the reject rows are tested only on the final kept table, whole. The objective still prefers known bad steps: Tuxborn train, buffScale 0.475 value 127.93 -> 129.58 but leftRR 1.96 -> 3.46, roadLeft 40 -> 55; foodScale 0.45 -> 129.15, leftRR 2.92. Each is accepted by loot_judge, carried, then everything is rejected at the end. Safe, but the stage's outcome is predetermined.
  Fix: run loot_gate_check per candidate inside loot_judge (train places), keep the final one.
P3 SEV-3 tune_driver.py:2507 held-out VALUE loss of the loot table is open_decision kind "note": with --decisions auto (the tune) the table is KEPT. And without --loot-places no held-out check exists at all and the fixtures table is kept with a note. So "a tune can no longer accept a worse held-out table" is NOT true for loot. Fix: paired per-seed value (d < -1.5 se) rejects; no --loot-places rejects (or refuses the stage).
P4 SEV-3 joint verdict does not bind the SHARED values. ship_state() (1181) compares joint.table with final_curves only. Proven by call: worth accepted after a joint PASS -> needs_joint_rerun ""; rerun_after("worth") and ("spells") -> []; a FAILED joint leaves spells.kept in final_sets(), and gen_class_files.shared_not_written lists it as accepted. worth runs after joint in DEFAULT_STAGES and has no set check on held-out. The reverse too: class files ship alone, but the joint graded them WITH the spells set.
  Fix: compare joint.sets with final_sets(), rerun joint after worth / spells, put sets in ship (empty unless PASS), and either ship both or joint-grade class rows without sets.

SEV-4 LootLab.h ScoreOver pairs per EPISODE: a table that only MOVES a guard-seen take to another episode pays 1000 with no refund. Conservative (never accepts worse), but on guarded sets (Tuxborn held-out 78 takes in 1440 eps) it is noise against every candidate. Pair per seed total instead. Same for the no-band hard row (takeThreshold 0.8: 78 -> 81, se 0.33 per seed 0.19).
SEV-4 cache key: key_of / ident for epi: and lootrep: hash no <set>.loot.json (pre-existing). A re-extracted loot table replays old results. Combat keys are right (bin sha, sim.json, mods, worth, every .mfoenv).
SEV-4 loot_gate_check: a row with no LOOTSEED line reads d=0 -> ok silently. 22 of 22 rows have them today. Make it an error.
SEV-4 held_out_check: an empty pairing (per[c] == []) is ok. Require fights in every cell.
SEV-4 stale_spawns passes when ONE effect resolves.
SEV-4 World.h LandSummon / SummonGate: SummonCapSkipped (noCap) not mirrored; BrainCast never casts Summon nature, so a foe's eviction is reached by the test only; only "IAUS/oracle" matters.
SEV-5 doc 16.2 table omits noCap. SEV-5 a hazard spell with INSTANT effect (dur 0) is scaled by interval; the engine applies it in full each interval. (Non-hostile hazard effects on enemies are harmless: SpellHit reads damage / control only.) HazardHit holds a raw pointer against the comment (compare only; safe). Hazards flag 2 uses the effect's duration: ok.

Answers
(1) choice R2, R3 (class + spells + joint), R4, R5 closed, except P4 for worth/shared. Loot R1 closed, R2 closed: all of buffScale .475, foodScale .45, grid minima, takeThreshold .8, zeros are REJECTED on train AND held-out (leftRR / roadLeft / dibsNear beyond 6 se). Remaining holes P2, P3.
(2) Mirror agrees line for line with LootTravel_internal.h GateReadmitSweep / LiveGateAtPoint (Same wins) / MarkGated R4 / Service.cpp:1345-1400. Lab re-tests only targetOnly by CALL flag, as native. Old arms 27/25/15 still shown, pinned by native1124=0. Bound 4 justified: native erases g_lastBlock at each strike, so First + Same per strike, two strikes = sticky.
(3) Off path draw-for-draw (above). Dead-listed 3 s and no Landing: acceptable MODEL, effect is one re-cast delay; native's Landing floor (appear + margin after the last cast) only matters for a creature that dies inside it.
(4) One reader (spawned()), Classify for both, game data not in git (no json in the diff), offsets self-checked. Impact rule MODEL is fine for the WORLD; not for what IAUS is offered (P1).
(5) Valid: --compare keys on metric names, the meta line is only the world guard. It is vacuous for hazards on fixtures (none exist); the list runs above cover that.
(7) No list names or tables in logic. vanilla formIDs appear only as extractor self-checks.

**CLOSED by `feat/s5-e1` (S5-E1, 2026-11-25):** P2, P3, P4, and the SEV-4s "cache key" (`epi:` / `lootrep:`), "loot_gate_check: no LOOTSEED line" and "held_out_check: empty pairing". Still open above: P1 (marth), the ScoreOver pairing, `stale_spawns`, the World.h summon rows, the SEV-5s. Left by S5-E1, raised by the Opus review against 2d6948a4 (SEV-4, deferred): E4 "next-best of a refused sweep. Cached delta later; not a blocker" (a refused loot candidate is the best value of its sweep and the next best is not tried in its place); E9 "one bad worth value fails joint for every class: no leave-one-out of shared values". Closed in the fix round after that review: E1 (`rerun_after("gap")` names `joint` first), E2 (`check_start`: worth / loot refuse at START without `--consumables on`, the loot episodes without `--loot-places` and its `.loot.json`), E3 (`runreader.kept_specs` reads `ship`; a failed joint offers nothing), E5 (loot is not `complete` and keeps nothing until its final checks ran), E6 (tests pin the candidate gate: against the shipped table, on the train places, no note), E7 (the app stage list is the driver's, its log reader knows follow / spells / worth / loot rows), E8 (selftest vocabulary literals from `len()`).

## fix/impact-hazard-shape (MFO-B865), Opus review @753b93e5
SEV-4 FF Aimed/Touch spells with a kDropToGround impact hazard are kept (LoreRim 6 Aimed + 6 Touch, Tuxborn 29 + 9; mostly enemy spells; REQ_Destruction2_Arcane_AimedHazard, _BL_OppressiveBarrier). Many EFIT area 0, so splash_safe has no radius for the ground hazard they leave at the foe: a melee ally beside the foe may stand in it. BlastUnits could include the dropped hazard's radius. Ordinary bolt hazards (flag clear: FireLg/FireSm 4, IceHazard 0) are the engine's normal impact effects, same as a gambit cast or an enemy's. Not shown to hurt allies (not verified in engine).
SEV-5 harness rows for MFO-B865 test the pure rule and the drop only; nothing tests the record reader (impactHazardDrops in cast/Archetype.cpp).

## feat/placed-l1m1 (Opus 5.5 author, 2026-11-24), author's own open list
1. (SEV-4, pre-existing) A SELF buff claim, and so now every IAUS summon, is filed without the `HarbingerWouldRefuse` dry run its ally sibling makes (`cast/CastOn.cpp` self fork; the ally fork has it). A refused summon claim is transparent (the next IAUS pick runs) and logged once per throttle, but the incumbent was already displaced and `Loadout::Prepare` already put the spell in LEFT, every lap it is picked. A NEVER-FIRED claim does have its hold-off (`g_neverFired`, the rest of the fight) and is transparent before the preempt.
2. **CLOSED by the review fix round (F1: `Prune::SummonRoadFor`, `SummonClaimUnservedWhy`).** (SEV-4, pre-existing) An IAUS summon picked while the follower has NO combat controller, or whose row is ambiguous, takes the unanimated one-shot (`cast/Fire.cpp` summon fork runs before A6's defer of self buffs). Loud (`UNANIMATED road`). A6's rule ("defer, never direct") does not cover summons; M1 makes it reachable for every conjurer.
3. (SEV-4) Both-hands summons (thralls) are candidates; the claim prepares LEFT only and no both-hands cast under a claim was ever seen (S9 / C3).
4. (SEV-4, lab) `Data.h` areaUnits still reads the EFIT area alone; native takes the larger of it and the explosion radius (`BlastOf`). The radius is now extracted (`placed.expl`); using it moves splash results, so it is its own change.
5. (SEV-5) A Self hazard whose hazard spell has a hostile effect is blocked whatever the rest of the record says: list circles of the "harm foes inside, help friends" kind are blocked too (Tuxborn / LoreRim names in plan section 11).
6. (SEV-5) `feat/placed-l1m1`: foes in the lab now cast Target Location summons they were denied before (same predicate, `foeDrop`); `BrainCast` never picks a Summon nature, so no fight changes today.
7. (SEV-5, fix round) A self Control / Offense spell the narrowed block releases (Harmony, Hallowed Weapon, Vanish, Mass Inhibition) is cast by the direct self road (`CastSelfDirect`, unanimated), as on main before the block; M4 is the animated road.
8. (SEV-4, lab, fix round) The "batch C" CLI check was a one-seed pin: with the new lists seed 1 gives evoker sp.tie% 1.9 and sp.unmTop% 5.8 (limits 0 and 5), and main's own build fails the same limits at seeds 3 and 6. It is now a six-seed mean (old tie > 0.5, today < a quarter of it). Why today's rules still tie now and then (evoker mean 0.3-0.4%) is not looked into.

### Opus review of 562f6f4b (2026-11-24), deferred, verbatim (F1-F4, F8, F9 fixed in the fix round)
- F5 SEV-4. book_census.py is a second DERIVATION by the same author with the same assumptions (cost proxy for effect cost, script shape, HEALTH any effect). Verified line by line against Archetype.cpp Compute, CasterConsent::ClassifySpell, SpellSupport.cpp: matches. Real outside evidence is only the 33 deck-log dropped spells, Tuxborn; natures of KEPT spells have no outside check.
- F6 SEV-4. Both-hands summons (thralls; chargeTime>=2: Vanilla 3, Tuxborn 18, LoreRim 12) are candidates; LEFT-only claim. If never fired: ~kHoldLastSeenCapMs of a held left hand once per fight per spell (then g_neverFired, transparent before preempt). Bounded. Author listed it. Tuxborn conjurers may burn several in a row.
- F7 SEV-4. Baseline explanation: builtin v2/v3/v4 compare 0 changed (proven). o-/b- old vs new per list differ widely because books and draws change (4 fights/cell = noise); outputs do not isolate "books alone" from the foe reclassification (ClassifyNature now also sets foes' nature, World reads it). Sim code otherwise untouched between the two binaries. Ok as a smoke, not a proof.

## feat/move-s1 (Opus review @1dc002fe, verbatim; F1 to F7, B1, B4, B5, B6, B8 and the comment half of B7 are fixed on the branch)
- B2 SEV-4 no distance-to-foe consideration at all (unless intended for S5): a firing spot may lie at a foe's feet.
- B3 SEV-4 sort / dead triples, paths: 80 paths with 154-225 skipped a window for ten: each member gets about 8. Budget number, S5.
- B7 SEV-5 Catalogue.cpp: the F7 review comment moved from heal_rate's line to the spot_cover line. Plan contract's TrustTable / Light::Field are not in the picture (S3 / S4 append; light keyed by SpotKey, known only after UnitPool, so a sampler must fill it after Phase1: document).
(B7: the light half is documented at `PartyPicture.h LightAt`: keyed by the pool, filled after Phase1. TrustTable / Light::Field stay S3 / S4.)
- SEV-5 (author of the fix round) `tools/tune_app/selftest.py` was not run to the end: it hangs headless (pre-existing, the reviewer met it too). The validator was called directly.

## feat/move-s1 review round 2 (Opus 5.5, 2026-11-24, raised against fd1def9b; verbatim)

# Opus re-check: feat/move-s1 1dc002fe..fd1def9b. VERDICT: MERGE. Nothing above SEV-4.
Runs: move_rows standalone g++ -O2, 0 failed, golden 21/21 diff 0; rank_equiv 304732/0; test_tune_driver OK. No MSVC, no sim re-run.
F1 closed: offers per (spot, member, action) by the row's own gen + check; Pairing/PairsAny gone from paths, asks, table. Kept spot: KeptHolds (retreat/kite direction). 72 picks, 0 toward.
F2 closed: sight checked before the erase, kept ask at 1 s, Answer marks directly, FillStatic keeps the No.
F3 closed: unanswered threat = 0.75, gap 8.6% < 15%; stays asked for every threatened ranked member.
F4 closed: marks inbox / seen; Adopt clears only seen; an unadopted window keeps them. Re-marking costs one extra rescoring at worst.
F5 closed: 2048, cut counted, kite uses the flood's bound.
F6 closed: stamp + vert count, work dropped, Phase3 with no pairs = all stay; answers of the old mesh wiped at the next Freeze (stamp differs).
F7 closed: late list (64, 5 s), t / viewerAt on the answer, Stale() is the one freshness rule, a late No only marks inside spotKeptAgeSec.
COST measured (-O2, host), worst unit / total per window: 1508 tris n10 0.12 / 4.6 ms; 6032 tris n10 0.35 / 10.8; 15660 tris n10 0.44 / 13.7; pins on, same 0.73 / 21.8 ms.
 Generator re-run (Offer) 0.02 ms a spot, 0.3 ms a window. Place (ExitsAt) 0.04 a spot. The 2048 gather makes Flood + Generate the bulk (5.5 + 6.3 ms). Every unit under the 3 ms slice;
 21.8 ms = 8 slices of 133 ms = over the 0.5 s window at worst case (counted unfinished: nobody moves). See R1.
5 s OLD ANSWERS: never used as fresh without Stale() (viewer <= 128 u AND same tri, unless t given).
INERT: no native file outside MovePass/Spots/MoveDump names MovePass/PartyPicture. Decide.cpp gains uncalled TopRanked; TuningFile adds spot.<kind> and the floor. Unchanged.
R1 SEV-4 (S2/S3) a 10 member party, pins on, ~16k tris needs about 8 pump slices; a window holds about 3 to 4: unfinished every window = nobody re-decides. 4 members no-pins (7 ms) fits 3. Fix in S3: window ends when phase 1 ends (stretch), or gatherTris scaled by party size.
R2 SEV-4 "first filing counts" also for a re-ask answered promptly with t < 0: the fresh ray is stamped with the OLD viewer place and dropped as Stale when he moved, so that question costs one more window. Fix: answer inside the window with t<0 and `now` found: use *now if the holder is synchronous, or make S2 always send t. Documented in the header, hence SEV-4.
R3 SEV-5 ExitsAt and every foe flood allocate best(v.tris) (63 KB at 16k tris) per call, 60 to 80 a window; hold it in Scratch.
R4 merge note: conflicts with feat/spellworth in tools/iaus_harness/run.sh (both edit the source list: keep both); then the MOVE_STANDALONE line in run.sh needs iaus/SpellWorth.cpp (Catalogue.cpp calls SetSpellWorthKnob).

## feat/spellworth (Opus review @b3fa8ae1, 2026-11-24, verbatim; S1 to S6, the INERT row, the 0<live<1 pins, bestRaw, the sim's chargeSec and engineRow are fixed on the branch)
Everything below matters only at `sw.live` > 0 (shipped 0).
- SEV-4 AreaValue consideration already rewards foesInArea; AreaFactor at 1.0 counts it again in the rate; SplashSafe vetoes what areaFriendPenalty charges (penalty can never matter for a live candidate). AreaFactor(0 foes) = 1 (self blast with nobody in it).
- SEV-4 MagickaShare is a step (x0.5 at one magicka) and assumes back to back casting for the whole fight (FightSec = sum of lives); regen counted 0 -> nearly every spell x0.5 in a long fight; on top of MpReserve.
- SEV-5 conditional damage with a duration is put in directDamage. FoeLifeSec counts weapon Dps of all allies whomever they target. FightSec recomputed per (spell, foe): cache.
- Author's note, SEV-5: buff / ward / summon worth has no Generate call site (S2: the readers run now, the families are not ranked by it); the sim does not fill ward power or the summoned body.

## feat/anim-a1a2 (author notes, Sonnet 5.5, 2026-11-24; no review yet; recorded so the next edit of these files reads them)
### MFO-B866 (SEV-5, OPEN, cast/PartyGap.cpp) -- `g_partyGapLast` is never cleared (raised against feat/anim-a1a2)
Author: the rate-limit table (follower, spell, site) -> time is not cleared on ResetAllState or dismissal, the same shape as MFO-B272 for `g_unanimatedLast`. Bounded by distinct triples and small. Clear it from the cast reset path with that one.
### MFO-B867 (SEV-4, OPEN, cast/BuffRoad.cpp) -- A2 adopts only the AMBIGUOUS-row refusal, not the single-row Restore / Offensive buff kinds (raised against feat/anim-a1a2)
Author: the plan (section 2.2) also says the `kBuffRowElsewhere` default (L1: a Buff kind whose single row is Restore / Offensive) should claim fire-once. The brief names ambiguous rows only, and L1 has zero field lines, so `BuffRowOk` serves only a `BuffFireOnce` claim. To adopt L1: let `BuffFireOnce` answer true for a non-approx spell whose row is Restore or Offensive (same gate, same seat bit), nothing else changes.
### MFO-B868 (SEV-5, OPEN, cast/BuffRoad.cpp) -- a Reanimate candidate in an ambiguous mask is never fire-once (raised against feat/anim-a1a2)
Author: Harbinger serves a fire-once Reanimate spell only with kCastFlag_CorpseTarget, which the road chooser cannot promise (the corpse is chosen per rule). `SeatRows::FireOnceServed(..., a_corpseClaim=false)` therefore refuses it and the spell keeps the old refusal; `EnsureCastClaimLocked` also refuses a fire-once Reanimate claim without a corpse target (fail closed, warn once). A spell with a Reanimate effect AND a Restore / Offensive effect stays direct until the chooser knows the corpse.
### Review of feat/anim-a1a2 @ 11fc22ad (Opus 5.5, 2026-11-24): findings deferred, verbatim
- F3 SEV-4 (backlog) instant AUTO fan (authoredDur <= 0) is held in the gap although with a controller it still fans UNANIMATED (A4 pending), so "his controller is the only missing gate" and the `[cast-hold]` promise ("the animated claim needs one") are untrue there. Plan table wants hold, so behaviour stays; note it or pass authoredDur > 0 until A4.
- F5 SEV-5 OffenseCastFired matches (hand, spell) not target. Not reachable wrongly today: the gate runs only inside handPlan.inFlight (same rule, spell, target). Add c.target to the test if a second caller appears.
- F6 SEV-5 a spent claim that lapses (6 s TTL, unrenewable) is re-filed by EnsureCastClaimLocked's aged-out branch and its Fired reason is lost. Reachable only when the gate does not run for > 6 s with the replay running (ChooseBuffRoad != Claim, i.e. controller gone, under an in-flight lap). The duration up-read still bounds it.
- F7 SEV-5 Substrate.cpp: fireOnce is decided again at filing (main for summons). A bApmfCast flip between the worker's choice and the filing files the pair without the bit. Bounded by Harbinger's close on the fire.

## feat/move-s2 (author notes, Opus 5.5, 2026-11-24; no review yet)
- Closed from feat/move-s1 round 2: R3 (the flood's allocation). R1 stays (does not bite in the lab: phase 1 runs in one go, pins off). R2 stays (the sim always sends t).
- CLOSED (`feat/heal-move`): SEV-3 (design gap, native) `Generate.cpp` drops a heal past its reach / occluded, so `heal_reach` never moves a healer to a patient he cannot reach (iaus-sim-v4.md 17.6 item 1).
- CLOSED as an input, inert (`feat/heal-move`: `TickIn::spotAction`, `t.spotActionMomentum` 1, the number is S5's): SEV-4 (design gap, native) `spotAction` momentum read-back of 8.1 is not in `Decide` (17.6 item 2).
- SEV-5 `ExitsAt` (the pooled spot's exits) still counts open directions; only the retreat kind has `CheckExit`.
- SEV-5 `CheckExit` floods up to 64 times in one Generate unit when nothing passes. Harness "dearest unit" 1.3 ms of the 3 ms slice, host.

### MOVE-S3: what `feat/move-s3` closed of the MOVE-S2 findings below (evidence: iaus-sim-v4.md 18.1)
- F1 CLOSED: 48 fights a row, the vacuity guard counts fights that filed a place, 16 seeds pass (1, 4, 21 to 26, 31 to 38). Lost-sight's true rate at 200 fights is 97.5 and 94 percent. No rule was loosened.
- F2 CLOSED: fixtures 4.7x to 2.3x, extracted places 2.3 to 2.5x to 1.7x, oracle env-ridge 5.1x to 2.25x, byte identical baselines.
- F4 CLOSED: six peel rows and spot-test `peel-bow-in-hand`. Each of the three mutations (Intercepts, 2 x spotPeelU, KeptHolds) fails a row.
- F5 CLOSED: the doorway is measured by geometry. PortalsAt ring off fails the pass arm.
- F6 CLOSED: `Trust::Refused` and `MoveClaim::walks` (spot-test `blocked-spot`, `waiting-claim`). F8's cost half CLOSED (cached floods); its behaviour half (a big room on a dense mesh as its own exit, the drift) stays open.
- OPEN: F3 (S5), F7, the heal move-only candidate.
- SEV-4 (author, S3): a route's trust is read at points 128 units apart. A distrusted corner clipped between two points is missed.
- SEV-4 (author, S3): the six static rows of `kTrustSigns`, and `moved-unwalked`, `unplanned-move`, `falling`, have rows and harness checks and no filler (G1 / G3 / M2).

### MOVE-S2: review findings on 409231ba (Opus, nothing above SEV-3). F1 and the heal finding are taken by the next slot B rounds

PROVEN
- Road off: main merge-base binary vs branch binary, all classes, seed 5, fights 2, --world v2/v3/v4, 1 and 8 threads: baseline files BYTE IDENTICAL (44244/44260/51520 lines). msvc.log EXIT 0 (194/194). cli.log CLI-CHECKS PASS, EXIT 0.
- Native: the changed files are Core.h (one t.spot default), MoveDump.h, MovePass.cpp, PartyPicture.h, Spots.cpp/h. Includers of those: only each other (+ TuningFile.cpp / Core.h by name). No MovePass caller in the DLL: inert. Generate.cpp is NOT touched (finding only).
- Golden: regenerated in one commit only (409231ba). gcc -O2 replay 0 failed. REAL MSVC cl /O2 replay (msvc-wine, wine run of move_rows.exe): 0 failed, every case read. The branch docs record no MSVC replay and neither localbuild nor CI runs it; this run is the record.
- Determinism with the road on: point 1 vs 8 threads, knight+evoker+warrior, byte identical.
- Flood memory: touched list cleaned at next call, -1 closed marker consistent, resize safe on a smaller mesh. OK.
- Executor: EnvMove/EnvTryStep on the true mesh (walls, doors, bodies); drift goes through WalkLine; a claim only clears nav.n. No teleport or wall path found. Locked door/closed door/island/one-way/ignored-wall/narrow traces (2 classes, 3 seeds): 0 blocked loops, spotBlocked/f max 0.05; stuck% 0.046 -> 0.010.
- Ray budget: pass's own cap (32 facts, 24 spots, 64 pool, 80 paths) is compiled in, so the sim can not exceed it. mv.rays 1.1 a window, no short.
MUTATIONS (4, native source copy, seeds 1 and 4)
- CheckExit always true: both dead-end rows fail, 10 golden rows fail. Own-ray No removed: lost-sight fails, 2 golden. PortalsAt ring off: caught (portals fault "STILL HOLDS", 3 golden). Peel Intercepts guard off: NOTHING fails (spot-test 18/18 twice, golden 0).
FINDINGS
F1 SEV-3 SpotTest.h: the rules sit on their edge. Fresh seeds 21..26: 3 of 6 runs FAIL. 21 and 25: ridge "places reached 5" (needs 6) though 12/12 hit and 0.00 s blind; 23: lost-sight 9 of 12 hit (<80%), blind 0.54 s. Doc claims seeds 1-6, CI runs 1 and 4. So the pass rules were checked on a few seeds. Fix: 48+ fights a row, ridge vacuity guard should count fights that saw a filed place, lost-sight's real rate is about 80% so either the rule or the pass is wrong: measure it at 200 fights before tune.
F2 SEV-3 cost. Fixtures, 720 fights, 1 thread: user 2.26 s off -> 10.05 s point = 4.4x (3.1 -> 14 ms a fight; MOVECOST 10.4). With --oracle (env-ridge, evoker, 2 fights): user 14.4 s -> 50.4 s = 3.5x, 620 windows a counted fight (rollouts copy the whole PartyPicture and re-run the pass). The doc's "5 / 13 ms a fight" hides that a fight itself is about 3 ms. Any budget of the tune/oracle must assume about 4x (fixtures) until the flood / PathTo cost is cut; extracted places not measured here.
CLOSED on `feat/s5-c` (S5-C): F3 SEV-3 oracle (17.6 item 5 as written): the oracle's subject holds no claim, so with the road on the gap is not a gap (env-ridge evoker went +3.81 -> -0.74). S5 must not read oracle gaps with the road on before this is fixed.
F4 SEV-4 peel rules (Intercepts, 2 x spotPeelU, KeptHolds(Peel)) have no test that fails: mutation M3. Needs a move_rows row + an archer spot-test.
F5 SEV-4 doorway-block meters "two in it" with PortalsAt itself, the function under test. With the ring off the pass row still says 0 two. Measure by geometry (both claim points within the gap).
F6 SEV-4 MoveLegEnd clears the commit on BLOCKED/UNREACHABLE; kMarkPathBlocked then excludes nothing, so the next window can re-file the same spot (only EnvWorld retrySec paces it). Not seen in fixtures. S3 trust owns it, say so. Also any goal-less leg (nav.goalBody 0: give ground, run) that ends while a claim waits releases the claim.
F7 SEV-4 parity: lab phase 1 is one go (native slices at 3 ms; worst window here 8.4 ms, so 1 slice at most 3), 3 rays a fact against up to 6, no lost answers. Lab kinder. Rays could be given an Unknown share like MoveTuning.dropShare. Plan items not built: sharedRadiusMargin / preemptSettle mirrors, calib_point.py (documented 17.6).
F8 SEV-5 CheckExit floods 256 triangles; on dense meshes f.cut makes `far` small (0.8 x bound) so a big room can be its own exit; UnitOffer also calls it per member/action/spot (backlogged as "64 in one unit"). Drift adds up to 0.4 run speed on top of the brain's own step.
S1 BACKLOG: R3 flood alloc FIXED. R1 (21.8 ms slices) OPEN, measured 4.5 to 8.4 ms worst window here with small parties, still over one 3 ms slice. R2 re-ask stamp OPEN in native (sim always sends t); must be closed before G2.
HEAL FINDING (8) **CLOSED on `feat/heal-move` (iaus-sim-v4.md 20)**: real in game. Generate.cpp:969 drops any heal whose recipient is past e.reach or Sight::Occluded (Actuation::HealInReach, same in the cast roads), and the ch.24 held-heal approach (CombatApproach.cpp) is built for a heal already held, so a patient who STARTS out of reach or round a corner gets no approach. heal_reach gets no ranked heal, so its weight can not be tuned: the tune would learn nothing. Do: a small native brief (own review) before S5: emit such a heal as a MOVE-ONLY candidate (scored as if in reach, never a Decide winner, never cast, exported only through TopRanked / MoveNoteRanked), gated so that shipped decisions stay byte identical when no pass asks. Safety: HealRefused must still apply. Then the healer scenario runs unscripted.

### `feat/sim-strict` (author notes, Opus 5.5, 2026-11-24; no review yet)
- CLOSED here: HEAL-MOVE F1 (the export keeps Decide's order), F2 (five rows, each mutation fails one), F3 (doc), F5 (a patient whose in-reach heal is on its hard cooldown). F4 and F6 stay open (SEV-5).
- OPEN (S5): the acceptance of the mode set is a tournament of constant rules under IAUS's own actions. The oracle's subject still holds no claim (S2 F3).
- CLOSED on `feat/s5-ab` (Hold is a scored kind, iaus-sim-v4.md 22.1): `StayMode`'s three lines (`t.orderHoldFoes`, `t.orderNearU`, `t.orderStandMinU`) are rules, not scores: Free against a hold on his own place is not weighed by the considerations. If the oracle over spots shows gaps there it wants a spot kind of its own.
- CONTRACT (`feat/s5-ab`: the lab ends the claim on Free and the next order pays `refileStall`; M-1's client still owes it): Free is the only order without a leash here. H-M1's Free keeps the claim's R as a bound (hm1 7.2): the MFO client has to end the claim for a Free, or the lab must bound it.
- OPEN (SEV-5, named since `feat/s5-ab`: `MoveTuning::blockAfterSec` / `blockAgainSec`): `blockedShare` blocks an order at 0.5 s and again each 2 s until it ends. The field may show single blocks that pass.
- OPEN (SEV-5): the lab's summoner (what he calls joins as a late foe) and flyer (the dragon) are models. 1 foe kind x count cell is not applicable (a boss with adds is no single foe).
- OPEN (G2): hide run, back-off, retreat, regroup still take the body from an order, in the lab as in the game today.

### `feat/heal-move` (author notes, Opus 5.5, 2026-11-24; no review yet)
- OPEN (design, marth or S5): the in-reach rule is literal. A patient in reach who is scratched but ranked over the act floor holds the healer from one who is dying round a corner, as long as his heal loses to other actions. `t.moveHealYield` (0 = literal) is the knob, not searched.
- OPEN: a patient whose nearest healing place is past `t.spotRadius` (1500 walked) is still not approached: no place is found.
- OPEN (SEV-5): a move-only heal of a book's far reach round a corner costs a full sort of his points once a window (the 64 ray tests are the same).
- OPEN (G2): native must fill `moveAsk`, cooldowns over `moveOnly`, `spotAction`.

### MOVE-S4: what `feat/move-s4` closed of the MOVE-S3 findings below (evidence: iaus-sim-v4.md 19.1)
- R1 CLOSED: a refused place is dropped before the sort. Harness row (60 rounds of refusals, the next best in order) fails without it.
- R2 CLOSED: the line is 64 past where he stood, 40 wide, and the walk goes along it (26 degrees). Rows for slides at 30, 45, 60 degrees and the thin end fail with the old numbers.
- R3 CLOSED (per mark), R4 CLOSED (one leg or 1.5 s is one event), R6 CLOSED (`trust-walk-clears`, fault `no-walked`), R7 CLOSED (starts and targets vary) with its trace: in the reviewer's variants the target is SEEN once the wall is gone, so he needs no place, "used again 0 of 12" was the rule's fault. The other three mutations of R6 (allroute, noexempt, notrail) stay harness rows only.
- OPEN: R5 (before G2, field calibration), the clipped corner.
- SEV-4 (author, S4): the straight-line half of `SpotPoint::lit` reads three points of a straight way. Round a corner it reads a way he does not take. Only the rank of candidates, the score reads the real walk.
- SEV-4 (author, S4): `Light::At` is a linear scan of up to 256 samples. It runs per candidate point for members who want the dark only (19.4 has the cost). An index is due before a party of five stealth members is in the game (G6).

### MOVE-S4: review findings on 8ae6f6c6 (Opus). F1 (SEV-2), F2, F3, F4 (SEV-3), F5, F7, F8, F10 and the scroll of ward of F11 FIXED in the round (iaus-sim-v4.md 19.6). F12 no action
- F6 SEV-4 SpellEntry::lightRadius filled, never read: a light on an unsneaking player/member next to a hidden one lights him (lit-ally-stealth passes only because they stand 3-4 cells apart). Plan is per recipient; needs marth's word before G6. **RULED (marth 2026-11-24): ignore the radius, only the hidden one himself is never lit. No code.**
- F9 SEV-4 decast-road-off off arm is 3..9 of 48 over 19 seeds (never 0, but weak): make the off arm strong (one other party member, player lit). Adequate evidence, not a strong row. OPEN.
- F11 SEV-5 scroll of ward not under the cap (FIXED); Candidates.cpp:457/:547 still read e.allyRoad only (right today, must become AllyRoadTo in G6 or a live proxy stream at an allyRoadOn recipient is not seen). MovePass.cpp:131 comment "a last bit of libm does not reach the rank" is not strictly true (a score at an integer edge). OPEN.
- F12 SEV-5 capped light can fall under actFloor and still be ranked (Decide :168) - harmless. NO ACTION.
- CLOSED (`feat/heal-move`, marth 11-24: `Flag::kDarkClass`, one reader `Classes::WantsDark`): "a class that sneaks" is `kSneakAlways` (Thief, Master Thief, Nightingale, Listener). Nightblade has no such flag, so a Nightblade is dark only while he sneaks, hides or is invisible.

### MOVE-S3: review findings on 6996d274 (Opus, nothing above SEV-3). R1 and R2 are fixed at the start of S4; R5 before G2

R1 SEV-3 (determinism) MovePass.cpp UnitGenerate: partial_sort to max(geomPerGen 64, perGen 3), but the new `Trust::Refused -> continue` does not count in `tested`, so a checked kind with k refused spots in its prefix reads k entries of the UNSORTED tail (order implementation defined: gcc vs MSVC, golden/live parity). Fix: drop refused before the sort, or add refused.size() to the bound.
R2 SEV-3 Trust::Crosses gate plane is only 16 u (aheadU/2) past where he stood and clearU 64 wide: PROVEN with unit program, a body that slides 30 u along a 45 degree wall clears its own true NoGround mark (marks 1 -> 0). Walking round a thin end does the same within 64. Never reaching "repeat" on an oblique wall is possible; my lab variants (starts 9,1 / 9,0 / 8,1) still recovered in 2.6 to 5.5 s, so not seen live. Before G2.
R3 SEV-4 one crossing clears the whole 512 region: PROVEN, a LengthFar mark 540 u away walked over erased two true NoGround marks. Clear per mark (or marks within a radius).
R4 SEV-4 independence: PROVEN one leg that goes LengthFar then STUCK = two signs = distrusted; two bodies stuck at one moment = distrusted at once. Rule says different signs, but one leg / one event should be one.
R5 SEV-3 pre-G2 (Q4): LengthFar doubts every unpassed region of up to 16 points; in game engine routes differ routinely and two followers on one detour distrust all of them for 30 s (spots x0.02 = party stays). Mostly self-heals in 30 s. Rows PLACEHOLDER; needs field calibration before any caller.
R6 SEV-4 vacuous scenario half: mutation "Walked clears nothing" PASSES spot-test 34/34: "used again within a decay of the walk" is satisfied by decay alone. "allroute" (passed bits ignored), "noexempt", "notrail" also pass spot-test. Harness rows catch all four (1,1,2,1 fails).
R7 SEV-4 trust scenarios are one fight repeated: worst == mean on all seeds (2.6 s), so 48 fights / 16 seeds add no evidence there. Variant starts 8,1->18,5 and 9,1->13,6: distrusted + recovered, but "used again after repair" 0 of 12 (may be the route not crossing the gap; not traced).
Q2 clipped corner: backlogged as SEV-4, agree. avoid ×6 softens it. Deviations 1 to 5: acceptable; 1 (0.02) makes R5 costlier.
NOT DONE: cost sample (running bf849bgi1 output), cli/off4 read, R7 trace.

### MFO-B869 (OPEN, author's own list for `feat/placed-m2`, 2026-11-24; `iaus/AreaCensus.h`, `iaus/AreaActors.*`)
1. (SEV-4) `kAreaGuard.safetyMargin` 128 is a PLACEHOLDER sized from the cadence, no field number.
2. (SEV-4) The build cost (actors x asking followers `IsHostileToActor` calls each 133 ms in a fight) is unmeasured in a crowded town: the `[area-guard] since start` line carries it.
3. (SEV-4) An area cast picked while clear has no MFO re-check between the claim and the release; Harbinger ch.25 is the only guard there.
4. (SEV-5) The lab leaves `SpellEntry::hostileArea` false and has no bystanders: a follower's hostile control area beside an ally scores in the lab and not in the game. Fill it from the extracted `placed` bits when the sim is next opened.
5. (SEV-5) A summon that is not hostile to the caster closes every area beside it, his own included (the plan's definition). The field decides whether that is too strict for a conjurer.

### PLACED-M2: review findings on 14968ab8 (Opus, nothing above SEV-3). F1 (crime hole), F3 and the scroll row go into the M3 round; F2 waits for marth

VERDICT: MERGE (nothing above SEV-3).
F1 SEV-3 AreaActors.cpp:125 hostility bit = IsHostileToActor ("would attack"); bounty / vampire / werewolf names guards + townsfolk, so they drop out of the table while attacking them is a crime (EngageOnSight.cpp:239ff, :250 crime-faction rule). Harbinger ch.25 uses the same predicate, misses it too. Not a regression. Fix: skip the bit when a->GetCrimeFaction() && !FightingParty(a) (EngageOnSight.cpp:234, same file-local test, currentCombatTarget resolves to player/teammate; s.foes still removes live foes by id, so add: foes list must not re-remove a crime-faction non-fighter) .
F2 SEV-3 (ruling) AreaActors.cpp:122-127 party summons / player's horse are bystanders. Plan text says so; Harbinger ch.25 Bystander (CastGuard.cpp:319) excludes party-commanded + player-owned per marth 2026-11-02 "Bystanders only". But ch.25 LineOfFire Friendly (:311, area+64) already HOLDS aimed blasts near any non-hostile, summons included, so M2 mostly turns an unfired claim into a re-pick; extra = 128 margin. A conjurer whose atronach tanks: every aimed AoE at that foe closed. Ask marth.
F3 SEV-4 cost, AreaActors.cpp:108-133: no distance cull, rows x casters IsHostileToActor (up to 64 casters). Estimate 1-3 us/call (2 combat-group lookups, faction reaction, a recursion for teammates): 40x3 ~0.1-0.4 ms; 100x10 ~1-3 ms per 133 ms. Fix: skip actors farther than ~6000 u from every caster BEFORE the hostility calls.
F4 SEV-4 MFO has no VerifiedAddresses row for IsHostileToActor (36537/37537; ~25 existing callers); Harbinger has one. Pre-existing.
F5 SEV-4 margin 128: ok as placeholder. Aimed recheck: ch.25 LineOfFire does re-measure per frame at release, non-hostile within area+64, so MFO-B869.3 is overstated when Harbinger is present and armed; absent = none.
F6 SEV-5 mutations survive: (a) Generate.cpp:606 drop hits.bystanders from friends; (b) AreaCensus.h nearest -> farthest (log only); (c) kScrollOffense SplashSafe removed. Caught: NaN-as-clear, control-ally. (c) is a real untested consumer: add a scroll row.
RESOLVED in `feat/placed-m3` (2026-11-24): F1 (crime-faction non-fighters stay bystanders, `FightingParty` shared through EngageOnSight.h, Gather keeps them), F2 (marth's ruling: MCM `bAreaSummonsBlock`, default OFF, enforced by `CountsAsBystander`), F3 (`kAreaGuard.cullRange` 6000), F6 (harness rows for (a), (b), (c), each proved by a failing mutation). F4, F5, F7 stay open.
F7 SEV-5 ghosts count as bystanders (MFO-B111, no IsGhost row). Dead/disabled/deleted excluded; commanded foes resolve through commander in engine; teammates resolve to the player (disasm 0x6796b0).
Freshness: no starvation. Tick gate requires pass t-1, so age<=1 in any run tick. Only Read answers NotReady when follower's lap falls on the first combat tick (or right after a pause = tick skipped anyway); re-read next lap <=1.3 s/10 followers. Wants TTL 60 covers it.
Threads: leaf mutex, shared_ptr const, held NiPointers, Clear after StopPump: ok. Layouts via GetActorRuntimeData, ProcessLists 0x30/0x60 untouched by version: ok. 1.7.104 exe not unpacked here, not disassembled.
Sim: BASELINE-IDENTICAL; simcmp.log's 292-398 diff is the dec.us timing column. Cons/enum unchanged; SplashSafe appended last on kControl, Off when open.
merge-tree vs 8ae6f6c6: only Docs/STATUS.md conflicts (Generate/Catalogue/Core/Snapshot/Candidates/harness automerge). Not built merged.

### MFO-B871 (OPEN, author's own list for `feat/placed-m3`, 2026-11-24; `iaus/SpellBook.cpp`, `iaus/Generate.cpp`, `iaus/AreaActors.cpp`)
1. (SEV-4) A WALL's value is the unmeasured-offense placeholder (`Tuning::unmeasuredDamage` x his best measured option), not the plan's "stream damage plus hazard damage for the dwell time". The lab has the hazard damage (iaus-sim-v4 16.3); the dwell is a sim-derived number. Wire both when the sim is next opened (Slot B owns `tools/iaus_sim` now). The plan's "prefer a melee foe that is closing" target rule is not built either.
2. (SEV-4) The wall's footprint is centred on the FOE, the stream's line is guarded with the same bystander margin (80 + 128 each side). Where the stream actually lands (first thing it hits) and the hazard's drift are unmeasured: F1's wall look decides.
3. (SEV-5) F1's rule (`GetCrimeFaction() && !FightingParty`) also lists a crime-faction ENEMY who is busy with someone else (a Forsworn fighting a deer) as a bystander, so an area beside him scores 0: conservative on purpose (the same rule `EngageOnSight::IsEnemy` uses to refuse a fight).
4. (SEV-5) The distance cull (6000) and `AreaKind::PartySummon` / `PlayerHorse` classification run in the main-thread build only and are not reachable from the harness; the harness proves the census and the options, the build is proved by the DEV self-test and the field line.
5. (SEV-5) `bAreaSummonsBlock` OFF means a conjurer's atronach beside the foe is no bystander for the MFO guard. Harbinger's ch.25 line-of-fire still holds an aimed blast within area + 64 of ANY non-hostile (including that summon) when it is present and armed.
6. (SEV-3, OPEN HALF of review F2, `feat/placed-m3` @5797d757) A wall's hazard lives 30 s (limit 10): a PARTY member (or, option OFF, his own summon / the player's horse) who walks into a live wall after the cast is hurt. Bystanders are covered by `kAreaGuard.wallMargin` (PLACEHOLDER 1000, footprint and line); the party half is not built (not a crime, plan 3.2 asks only the footprint for allies). In-game test list "Walls M3" 2 carries the watch item. Needs the field run to size it.
7. (SEV-5, review F7, `feat/placed-m3` @5797d757) A hostile guard's or townsperson's SUMMON has a null crime faction (`IsEnemy` handles its commander), so it is not a crime to hit. A player mount without ExtraOwnership (a Shadowmere-class horse) stays `AreaKind::Mount` and blocks even with the option OFF while the MCM text says "your horse". Noted, not changed.

### MFO-B870 (OPEN, `fix/summon-claim`, 2026-11-24; `cast/BuffRoad.cpp`, `scheduler/CombatScan.cpp`, `scheduler/IausSource.cpp`)
Author's own list. (1) The Summon item's own 0x0F (engine 0x8318F0 -> 0x8201F0: !fleeing, cooldown, afford, not applied, state gate) and BoundItem's (0x82C760 -> 0x821710) are unmeasured until this fix is field run: a claim that stands but never reaches seat 0x06 is then the never-fired bound's case, not this one. (2) Ward's engine 0x0F (0x8334D0 -> 0x81ECC0) says NO against a foe that is no threat (a melee mudcrab); Harbinger forces a verdict only for restore rows, so a ward claim there never fires (diag-anim-1124c section C; a Harbinger / test-foe question, not MFO). (3) `SummonHoldsCastChannel` runs `ChooseBuffRoad` once per summon rule per lap (log-free); it also performs ChooseBuffRoad's `g_neverFired.erase` on a no-controller answer, as Fire.cpp's own call already does.

### fix/summon-claim (Opus 5.5 review, against fbb527b7)
- **(SEV-4, deferred F3):** Fix 5 flap: no hold-off; a flicker costs one release + one claim/equip per controller edge, at most 1 per service lap; a steady controller never flaps. It does not check CastInFlightOnHand: a charge in flight is cut (takeBack). Acceptable; Fire-reached only when InCombat false, else claim stands as before (the "waiting for his combat controller" pass never calls Fire), so Fix 5 is partial there: FacetExpiry ends it.
- **(SEV-3, FIXED in the fix round):** F1 (isCast true for the body's whole lifetime: BuffUpTransparent added to SummonHoldsCastChannel) and F2 (g_neverFired erased on a no-controller ask: controller test first).

### MOVE-S4 round 2 review on 8914e1c3 (Opus, nothing above SEV-4; fix R2-1 and R2-3 before G6). marth 11-24: Nightblade counts as a sneaking class, never lit

ROUND 2 (8914e1c3) started Tue Nov 24 09:01:01 PM PST 2026
VERDICT @8914e1c3: MERGE. Nothing above SEV-4. Fuzz: _localbuild/review-s4/r2/p3.cpp (b.sh).
F1 FIXED: 4000 random boards x ranks 0-5 x 60 seeds (1.44M ticks; 2886 boards with heal/ward/scroll, 1220 ward, 894 scroll; half a running light; with and without window; shuffled): light first 0, Hold on a light 0, light before any ranked heal/ward in the order 0 (2.07M light fall-back steps, so the fall-back is kept), empty order 0.
F2 FIXED: player known needing: 0 of 242179 light picks elsewhere. F3: known board, unknown picked 0 of 339310.
R2-1 **CLOSED on `feat/heal-move`** SEV-4 Decide.cpp:196-206 (uncertain 2: YES). `known` = min over ALL known light candidates, also those under the floor. Known Lydia at 60..68.5 (raw .17...03, under the floor, never lit) drags player+self unknown (.88/.51) under the floor: no light to anybody as long as she stands there; at 69.5 her candidate is vetoed and both jump back to .88/.51. Also min over spells: an expensive second light's .31 caps all. Conservative direction, mixed known/unknown only, DLL has no fill. Before G6: cap at the best known score per recipient, and only from known ones above the floor.
R2-2 SEV-5 Decide.cpp:209-212 (uncertain 3: no new loss). Lift: player at 68.5 raw .048 becomes .521 and beats attack .494. Only ever to a score a real member light already held on that board, so a light beat the attack anyway; a ranked heal caps it. One spare cast per light duration on a nearly lit player. It also copies the member's running momentum into base (Scored.base != score/mom). Accept.
R2-3 **CLOSED on `feat/heal-move` (both mutations fail a row)** SEV-4 coverage: two mutations survive: scroll of ward out of HealOrWard (F11); yielding lights take pool places (the `continue` in the pool loop). Behaviour right, rows missing (p3.cpp covers the first).
CAUGHT: OrderLights off; InParty without self (4 rows).
F5 InParty, F7 heads, F10 length weights (via ends at the goal, PartyPicture.h:123), F8 rows: read, correct. Decast: WantsLightEnded = WantsNow or class; player sneaking loses only MayEnd + InParty + own-caster lights. Nightblade without kSneakAlways: author's open item, marth's call.
UNCHANGED: pin digest main 9d94875f = branch fbf8ec48124dd622; extended digest (verdict + order, 6 ranks x 50 seeds x running/window) 19a729538b65e5df both. Author off/det logs SAME. No DLL fill of decastRoad / light / lightField.
MERGE STATE: origin/main 9d94875f is an ancestor; merge-tree clean.


### HEAL-MOVE: review findings on 55f6986a (Opus, nothing above SEV-4). F1, F2, F5 go into S5; F3 fixed at merge. S5 searches t.moveHealYield

VERDICT: MERGE. Nothing above SEV-4. Tier-B shape, pure files + one class flag; no co-save/ABI/seat/threading. 28 files, all in brief; no A/D/R.
PROVEN
- Off inert: fuzz, 800k boards (running, offense, ward, buff, scroll, summons, damage class, all rapport ranks): hash of Decide (verdict, ranked, scores, order, roll, bestHeal, counts), candidates, 2-arg TopRanked identical base 0cac0f81 vs 55f6986a. Sim road off base vs head binaries (own builds): see appended lines.
- Asking (1M+800k boards): decisions, candidate keys, counts, healRefused identical to not asking; moveOnly empty when off; no move-only for a patient with any heal action; none for self.
- Export: move-only score always strictly under every ranked in-reach ally heal, never first over a held action, never under floor, never under a light: 0 violations in ~365k exported (lights) + ~279k. Result independent of moveOnly input order. 1 vs 8 threads identical.
- R2-1: oracle over 1M boards (203,866 comparable, no heal/ward): 0 mismatches.
- Golden v4: shared 27 cases: only 228 'a' + 43 'c' lines changed, each exactly by an inserted 0 token; all x/t/v/m lines identical; 3 new move_only cases. Old version -> err "version" (refused loudly, no legacy reader: golden is regenerated, fine).
- Spot tests fresh seeds 13, 5150, 60606, 918273: pass; healer rows 48/48 vs 0/48. Scenarios truly unscripted (no c.script).
- spot.heal_reach 0.3/1/2 gives different tables (600 / 621 of ~1.5k lines, healer). Cost road on healer+priest 4.56 -> 4.82 ms/fight, rays 72 -> 81.
- final.sha FINAL DONE, det2/off/msvc/harness/cl/cli all EXIT 0 on 55f6986a.  CLI skipped calib_damage (no _evidence in the worktree): not run by anybody.
- kSneakAlways readers (Defaults.cpp:369 sneak row, SpellBook sneakClass, Candidates.cpp:608/609, TableIsConsistent, sim stance) unchanged; Nightblade has no kSneakAlways. Combatant::sneakClass is only filled in the sim.
FINDINGS
F1 SEV-4 Decide.cpp:380 the 3rd TopRanked re-sorts by raw score, dropping Decide's tie order (current target, effectRate, cost; OrderLights) and the demoted held light. When any move-only clears the floor the row order/cut of the ORDINARY actions changes: tieflip ~0.2% of no-light boards (767/400k), ~2.3% with lights; a move-only that itself does not make the three rows can still reorder them (588/400k, 1830/250k with lights). Road-on only. top[0] of a not-holding follower can flip inside a tie -> kMarkActionChanged noise. Fix: merge the far rows into d.ranked's order (upper-bound insert) instead of sorting; before G2.
F2 SEV-4 rows: 5 of my 8 mutations survive the whole harness+golden: (1) held-first dropped from the export sort; (2) act-floor test -> s > 0; (3) a held light stays first (`&& !lightBuff` removed); (4) best heal of a target reversed (worst kept); (5) moveOnlySight not erased with moveOnly (sights shift to another patient). Caught: Pairs guard, any-reach healCastable, tie margin of the cap. Behaviour right by fuzz; add rows.
F3 SEV-4 (G6 trap) iaus-sim-v4.md:1709 still tells G6 to fill Combatant::sneakClass from ClassGate::sneakClass. It must be darkClass, else the Nightblade is lit in game. Doc one-liner.
F4 SEV-5 spotAction at momentum 1 is not strictly flat: it switches off pruning for that candidate, so evaluated/ranked/buff-capped boards may differ when hasSpotAction (lab only, DLL never sets it).
F5 SEV-5 "no heal candidate above" counts a heal in `actions` even when Decide vetoes it (cooldown, floor): a patient whose only in-reach heal is on a hard cooldown is not walked to for another. Intended reading of 'reaches', note only.
F6 SEV-5 parallel vectors moveOnly/moveOnlySight invite F2(5): one struct.
Off sim seed 991 fights 2 threads 8 beta1 + nightblade,healer,thief,wizard, v3 v4: byte identical (17408/20301/4682/5445 lines). fuzz.cpp kept: _localbuild/hmrev/fuzz.cpp

### `feat/s5-ab` (author notes, Opus 5.5, 2026-11-25; review on 3ebcca6a: MERGE, nothing above SEV-3)
- CLOSED of SIM-STRICT below: T1, T3's rows, F1, F2, F3, F4, F5, F6, F7, F9. F10 stays (a comment at the row) until MFO's APMF_API.h reaches v48. F8 and T2 are S5-C and S5-D.
- CLOSED in the fix round: F1 to F4, F8, F10, `notCarriedShare`, F7(a) (one foe left = 0.75). F6 and F7(d) are documented (iaus-sim-v4.md 22.1).
- OPEN (field, strict.flee): a fear effect takes the order away once (`fearUncarried`); whether the game repeats it while the effect lasts is not known.
- OPEN (G-filler): the game must fill `Foe::reach / speed / ranged` and `Member::anchorVel`. Unread is not applicable, never favourable.
- OPEN (SEV-5): `hold_narrow` is known only when his nearest foe is a target of his three actions (no path is searched for it).
- Review on 3ebcca6a (Opus 5.5), open, VERBATIM:
  - F5 SEV-4 a self or targetless top action (drink, self buff, summon) is no Hold, so a held member goes Free and pays a re-file. Allowing it while held: evoker Free orders 2.12 -> 1.85 a fight, stands 3.72 -> 3.44, U flat. Plan 3.1 asks for this gate, so not a defect of the branch; put it to the planner for S5-C (the oracle must be able to hold while drinking).
  - F7 SEV-4 shapes. (a) blade: foes_on 1 = 0.85 x 1.25 kept = 1.06: once held by two, one foe left keeps him held, only zero frees him: the "fights freely with one foe" ruling is true only for the first decision. (b) contact is near flat: exponent 0 / 1 / 3 / 8 moves U under 0.2 and claims under 4 points; foes_on exponent 3 moves knight U +0.9. (c) an exponent cannot change a factor's direction and has no upper bound (pow overflow -> 0, safe, never NaN); direction needs the per-class curve (px/py), which the sim's c.<i>.a|b does not reach for Piece rows. (d) hold_kept's exponent and t.orderHoldMomentum are one degree of freedom: search one. Gradient exists: spot.hold 0.5/0.85/1.2/3 moves U monotonically past 1 (archer -2.3, knight -4.3, warrior -3.7 at 3). Hold neither dead nor absolute at shipped values, but at them it costs (knight, warrior, about 1 U; F1 alone does not restore it). All placeholders. [(a) and (d) closed above, (b) and (c) open]
  - F9 SEV-5 C4457 MovePass.cpp(117): `Light::Read a` hides parameter a. Pre-existing (ed7ec4a5, S4), harmless (the action is used again only after the scope closes). Rename on the next touch.
  - CLOSED on `feat/s5-c` (S5-C, from F3): an order option must be bounded by time or a refusal, `blockTold` 0 never ends one. F5 for the oracle (it holds without a target; the pass's gate is unchanged, plan 3.1). MOVE-S2 F3.
- Round 2 on d153091f + a8f9c24b (Opus 5.5): MERGE. R2-1 and R2-2 CLOSED on `feat/s5-c` with rows. Open, VERBATIM:
  - F10: three not mutation-covered (shotStop row default 0; walk back under fear; same-point re-travel). Read against Harbinger: right. Acceptable (SEV-5 backlog), but backlog text "CLOSED F10" must say "no row". (The "CLOSED F10" above means: no row.)
  - R2-3 SEV-5 notCarried (and a point re-filed faster than 1.1 s) is invisible to the pass: no mark, no trust event. Matches the game, but an oracle / search must not read "order filed" as "order obeyed".

### `feat/s5-c` (author notes, Opus 5.5, 2026-11-25)
- OPEN (S5-D): under strict,kite Stand, GoTo and Kite at one place are one option (the executor carries them out alike), so the mode of an option is its place's row.
- CLOSED on the fix round (review finding 5): `GAPORD` is read and classified by the driver (`ORCMODE` has no reader). OPEN (S5-E2): search `t.orderHoldMomentum`, not `hold_kept`'s exponent.
- OPEN (S5-F): cut 6 (freezing the rollout's pass, at most 1.45x), the ext moves' retirement table. Cut 4 is built (iaus-sim-v4.md 23.6). Review findings 1 to 9 of S5-C are closed there.
- OPEN (SEV-5): a subject another fixed body keeps out of a place is refused that place, where the pass would drop it from the table only if the other was planned first.

### `feat/s5-d` (author notes, Opus 5.5, 2026-11-25)
- CLOSED: SIM-STRICT F8 (variety is what was fought: `GENFOUGHT`, iaus-sim-v4.md 24.4), T2's acceptance mechanics. OPEN until the S5-C verdict: the oracle pilot (`_tuning/s5-d/pilot.sh`), the held-out families, the acceptance itself.
- OPEN (later delta stage with kite): the `MELEE_KITE` numbers are placeholders, the rows `slow-heavy-open` / `slow-blades-open` / `boss-contact` are drawn only when their mode is served, and Stand against Kite at one place is a place's row, not an executor difference (S5-C note).
- OPEN (S5-E2): the driver's manifest of families (design, train, heldout) and the cache key of a calibration table.
- OPEN (SEV-5): one seed's light, level and terrain reach the fight, but the light only reaches the stealth model and places with no light layer.

### SIM-STRICT: review findings on d252f39d (Opus, nothing above SEV-3). F1 to F3 and T1 to T3 are in S5 (plan: Fable), the rest is this entry (what `feat/s5-ab` closed is in its own block above)


## Proof run by the reviewer
- Built -O2 on own tree. Spot tests at fresh seeds 5151, 88321, 60606: 82/82 each. Strict baseline threads 1 vs 8, seed 913: same (6534 lines). Road off and point vs main's sim0, seed 913: byte same (6221 / 6224).
- Golden: 30 old cases, every non-"o" line is the old line with fields appended (only case/pic/x lines differ), plus 6 new cases.
- Native: movement pass has no caller outside native/iaus; StayMode/Orders/OrderOf inert in the DLL. MoveMode 0..5 = kMove_* at d01c2e3 (row checks the five numbers as literals: no compile-time tie to APMF_API.h, because MFO's copy has no v48 yet).
- HEAL-MOVE F1 (insertion, no re-sort), F2 (rows), F5 (moved to TopRanked; no other reader of Generated::moveOnly lost a filter): fixed, read in code + rows. Generate now returns moveOnly un-deduped; only TopRanked reads it.
- Mutations (mine): CAUGHT 1 (pin leash -> orderLeash, golden). SURVIVED 6: hold keeps point only of a Stand; hold takes t.orderLeash instead of the standing radii; t.orderNearU ignored in StayMode; StrictLeashed (ch.23 wins) removed; GoTo/Disengage run speed without the disengage cap; blockBackoff ignored. -> F4.
- Mode test fresh seed 4242, 60 scenarios, 32 fights, caps strict,kite: resolved 7.3%, highest mean "PASS", resolved NOT MET. Forced arms still issue Disengage with the cap off. mode-cover seed 31337: 99.5%.

## Findings
F1 SEV-3 (lab kinder than Harbinger). Main d01c2e3 and feat/hm1-pa2 serve ONLY the implicit Stand. SetMoveOrder answers ModeNotServed to every mode; Caps() = kMoveCaps_Strict alone; no code sets Blocked or Unreachable. Lab default caps = strict+kite+disengage. Kite has no MoveCaps bit, so RoadCaps.kite has no API source yet. Smallest fix: --move-caps default strict,kite; docs "what is served today".
F2 SEV-3 fear. Lab: a fear effect under an order does nothing at all (he keeps attacking and travelling). Game: while the area is off during a flee the pick is kDenyOnly (Flee/Hide no, rest engine's, NOT stand or travel) with a 1.5 s latch, area re-evaluated each 1.0 s. No field row measures whether he attacks. The oracle will learn "Stand as an answer to fear". Fix: residue rows fearUncarried (seconds free + no order) and fearActs; placeholder 1.5 / unknown; row strict.fear.
F3 SEV-3 speed. kiteSpeed applies only when dot(dir, foe) < 0. Strafing across a foe is at full run, travel with no target or toward a foe too. Harbinger: travel while a foe is his target keeps the aim; 60..193 was measured for travel, no strafe figure. Add strafeSpeed row, measured by strict.goto.
F4 SEV-4 the six surviving mutants above: each needs a row (hold from a Kite order, hold keeps pin radii, NearU, strict-leashed, caps without disengage, backoff timing).
F5 SEV-4 residue table is not the only place: 0.5 s and 1.0 s block timing, min stop 6, sneak 0.55, drift speed 0.3 and 3 s, dot<0, lunge standoff 60, deny on MFO's own runs. Single for the numbers of the plan, not for the executor.
F6 SEV-4 OrderOf maps a.kinds unmasked. A kept commit survives on `kinds & allowed != 0` (MovePass 637) and keeps a RetreatExit/Kite bit whose cap went off. Mask with KindsOf in OrderOf. Reachable only if caps change mid fight.
F7 SEV-4 StayMode has no hysteresis (NearU, StandMinU, reach flip Free<->Stand each window; game: each flip is a claim file/release) and no comparison with Free. Also melee `onHim` counts foes within 400 whether he reaches them or not; doc 21.1 says "that he reaches".
F8 SEV-4 coverage number is a BUILD CHECK, not independence. speed, count, kind are read back from the spec fields the builder set; "leaving" = the flag set by ModePlayer; terrain lines were written for those fixtures; "door" = any leaf in the fight (comment: between them). One random build per cell at t=0, five-way "filled" if any of 3 player placements is right. 99.6% is honest for that, must not be cited as variety. Only 360 of 12,600 cells (2.9%) are ever FOUGHT; 6+ is always exactly 6; level 25, rank 3 fixed; one fixture a terrain.
F9 SEV-4 forced arms ignore g_moveCaps; Kite/Disengage re-Same after Ended can leave the order dead for the fight (Same -> no gen).
F10 SEV-5 doc 21.1 text (F7), mode-enum row should become static_assert when MFO's APMF_API.h reaches v48.

## Lab assumptions kinder than the game (full list)
1 caps (F1). 2 Fear (F2). 3 strafe speed (F3). 4 startDelay 0: a claim filed while the area is off or standard is NotCarried up to 1.0 s [FIELD strict.goto]. 5 Every new point is an in-place order with 0.25 s stall; P-A needs Release + Request and a sweep (about 0.1 s plus area carry), P-C not built [FIELD after P-C]. 6 Blocked -> Unreachable ends and MFO is told; today he loops 1 s holds for ever and nobody is told [P-B]. 7 2D distance inside a storey; Harbinger is 3D: a point whose z is 24 off never arrives (slope, stairs) [FIELD stopError]. 8 Travelling stops the instant he is inside stopAt, no pick delay, no overshoot; short stops (accepted up to leashR, 2 ends) not modelled: arriveError should reach leashR [U5]. 9 stagger, knockdown, ragdoll displacement 0 [strict.stand_*]. 10 lunge 60/110/0.5 guesses, never past the target [strict.stand_melee]. 11 casts and draws cost no travel speed [strict.kite]. 12 Yielded/NotCarried by package area, Exit Water, flight, mount: none. 13 Free unbounded (game: R) . 14 MFO order seen at a pass window with no bridge latency. Harsher than the game: blockedShare (a 5% order blocks again every 2 s until ended).
FIELD numbers needed first: kiteSpeed and strafe, lungeOut/share, startDelay/re-file, stopError incl. z, displaceU, fear, blockedShare, shots while travelling.

## Author's uncertain spots
1 StayMode by rules: a real defect FOR THE TUNE (Free vs Stand is marth's ruling and the biggest choice: it depends on foe kind and terrain, the rules see neither). Not a merge blocker. Give S5 a Hold kind (scored, own considerations: foes on him, gap, foe kind/speed, sight, cover) with StayMode as its gate. At minimum the three lines are searched per class.
2 Free without leash: bound it by the claim R or end the claim; the client contract (end the claim on Free) is simplest and equals the lab; then file latency applies (4).
3 Caps: tune with strict,kite only until H-M2/H-M3 ship and are measured. Disengage and run-speed GoTo go into a later delta stage.
4 Fear: F2.
5 blockedShare/lunge/travel: run a residue sensitivity arm (each row at low/high) over the same scenarios. Rows that flip a tuned choice wait for the field.

## Acceptance
Race statistics sound (paired, p 0.001, null resolves 0 of 360). It is not the plan's acceptance: forced constant arms, IAUS actions, no oracle. Highest mean is dominated by arm count (null 9/41/20/29) and must never be quoted as PASS: drop it from the verdict. Null 0 of 360 proves the false rate is low, not that power is adequate. 15% resolved selects large effects, not a random share. Kite rests on 7 of 68 archer scenarios (95% interval about 5 to 20%): not distinguishable from failing. Archer Stand 85% of the resolved: allowed by the plan's "over all" wording, but for that class the set teaches one habit. Extracted: 4.2%, NOT MET. H-M1 caps reseed: NOT MET.

## What S5 and the tune must change
T1 (SEV-2 tune) default caps strict,kite; forced arms and oracle obey caps.
T2 (SEV-2 tune) oracle subject gets a claim and races orders (mode x place) with its actions; Hold as a scored kind; acceptance = oracle best mode, with equivalence margin (a mode within 0.01 U counts as tied), 10% with lower 95% bound, 60% also per class, at least 40 resolved a class, on held-out seeds and extracted places apart.
T3 (SEV-2 tune) no spots stage before the field residues; fear, strafe, 3D and re-file rows added.
Variety: foe speed continuous 0.2 to 1.5 of run with a third under kiteSpeed (slowed, frosted, giants, draugr walkers); kiting needs open ground of 1500+ u plus one foe; melee-only foes; foe count 4 to 10 and mixed groups (shield + archers + mage); level 10/25/45 and ratios tuned so base win rate is 30 to 70%; several fixtures a terrain, mirrored and random start distance 300 to 2500; chain two fights; more than one follower decided; draw new cells every seed (not the same 360); extracted: stratify by narrowness/height/doors, 14 -> 60 places, 128 fights, choose ratio by a pilot so that the win rate is 30 to 70%.

## Cost
The measured factor off->strict is 2.7x on fixtures (movement-plan 6 assumed 1.2 to 1.5 a rollout). 1,250 x 1.2-1.5 cannot hold where the road runs in every rollout. If the road runs in all stages expect 3,000 to 3,800 CPU h; 1,800 to 2,500 is credible only if follow/spells/loot/potions run road OFF (plan 11 allows exactly that). No oracle fight was measured on the strict road: measure 20 before starting. Cuts, biggest first: road off outside spots; in rollouts freeze the pass (no Phase1/2, keep order, refresh 2 s); race orders only at window boundaries and only arms from the class's AllowedKinds; 15 cells first; 3 worlds; residue sensitivity on IAUS fights only; reuse cached search baselines.

## Verdict: MERGE. F4, F6-F10 to the backlog; F1-F3 before any tune on strict.
### PLACED-M3 round 2 review on fda14930 (Opus). R2-1 fixed (lab drops walls until the field run), R2-3 and R2-4 open

# ROUND 2 @fda14930 (Opus 5.5). VERDICT: DO NOT MERGE as is, R2-1 only (one lab line); native half OK to merge.
F1-F8: all fixed or recorded (F2 party half + F7 in B871). 22 vs 64/3: 3 ft 66 vs 64, inside the 64 body pad; AreaUnits right. Inherit branch: EFIT feet, falls to the record. Floor 64: ok. FightingPartyOrItsOwn: main thread (BuildPass), handles held, only clears crime flag. wallMargin: hostile actors are never listed as bystanders, so dungeon fights are not starved. cullRange 6000 covers reach 4096 + margin.
Merge-tree vs origin/main 1d052a3e: textually clean, tree 4d4addd881; harness of that tree 0 failed. MSVC of merged tree NOT built.
R2-1 SEV-3: head's sim --env-test FAILS (126 checks, 1 failed: "placed: a wall is no IAUS candidate, a foe keeps it"; base passes). tools/list_balance/book_census.py:60 also still drops walls (lab != census). The author's M3 plan needs both or neither changed.
R2-2 SEV-3 (before tune): lab wall entry has areaUnits 0 (Data.h:521; native SpellBook.cpp:363 sets 64+), so no splash_safe / foesInArea / LoF widening: lab streams walls at foes a party member melees, game scores 0. Hazards skip the party in the lab (World.h:1325). Value itself: IAUS book = native's (stream only); world damage exists (sim-pretune-plan item 6, iaus-sim-v4 16.3), ON only with --world v4, so the author's 80.80->80.10 (world v3, hazards off) was measured without it.
Experiment (seed 1): Vanilla minus the 3 walls, v3 fights 1: base vs head baseline byte-identical (dec.us excluded) => every difference, knights included, is the three pool rows moving the RNG draws of kits/mates. v4 train places field books fights 2: no walls 75.98, walls hazards off 76.33, hazards on 76.40 (one seed, sign differs from v3 = noise scale).
HANDLING: plan section 7 already excludes walls until the M3 field run. In Data.h Classify after IsPlacement: if (in.wall) e.drop = Prune::Drop::Placement (lab-only, labelled; foeDrop untouched). Fixes R2-1 and R2-2 for the tune, keeps EnvTest + census + baselines. Proof: env-test 126/0, Vanilla baseline == base. Footprint (areaUnits + party hazard damage) after field run.
R2-3 SEV-4: Generate.cpp:629 WorthOf counts friends with safetyMargin, comment says "same test" (spellWorth.live 0 shipped).
R2-4 SEV-5: outdoors, deer/wildlife not hostile within 1000 and unowned mounts close walls; log only.

### S5-AB: review findings on 3ebcca6a (round 1) and d153091f (round 2), Opus, nothing above SEV-3. R2-1 and R2-2 go first in S5-C; the rest is this entry


## Proof run by the reviewer (own -O2 build)
- move_rows gcc: 0 failed. Golden v6: 36 old cases, every line = old line + appended fields except `case` (format number) and the `x` of strict_blades_0..2; 6 new cases. Dump version 5 -> 6 read and written alike.
- Spot rows, fresh seeds 6161, 90210, 5550123: 110/110 each. Strict 1 vs 8 threads, seed 913: same (6533 lines). Off / point / pinned vs main's sim0, seed 913: byte same.
- Nothing outside native/iaus includes MovePass / MoveOrder / PartyPicture / Spots. Cons, SpotKind appended. Author's MSVC build + harness + CLI on 3ebcca6a/eb1b82b5 green (logs read). Tag check: no tag contains 1666d9e8, so deleting the four t.order* keys is safe (a hand file gets an unknown-key line).
- tune-app lists match Core/Catalogue by reading. selftest.py did not end in 120 s: NOT proven by me.
- Mutations (mine). Native: CAUGHT pace min, Stay's kind weight off Hold, leash 2.0->1.2. SURVIVED contact = farthest foe (max), anchorVel.y dropped; `caps.strict` guard on `narrow` (equivalent). Sim: CAUGHT fear repeated, HighOff at Pending. SURVIVED startDelay row read nowhere / repointStall off (startDelay, repointStall have no off-arm row: plan 4.1 asks one only of new rows).

## Findings
F1 SEV-3 hold_kept steals committed places. HoldPoint is true for ANY standing order, so a member at (or inside 2 leashes of) his committed Firing / Cover place gives Hold the 1.25 of hold_kept on that SAME point, on top of sight 1.1, contact 1.15 etc. the place itself never gets. Hold wins, Adopt clears the commit (spot < 0): pin reservation, trust, the place's kinds are lost, and it then holds the bar (bar = Hold score x (1 + spotPreempt)) against every new place. Measured (5 classes, --fights 30, strict,kite, one seed 913, no standard error taken), claim% main -> branch -> with the one-line fix: archer 76.3 / 31.7 / 67.2, evoker 33.3 / 10.3 / 24.6, knight 32.9 / 26.6 / 30.8, thief 38.2 / 27.2 / 34.9. Knight U 77.83 -> 76.38 -> 76.16 (the fix restores the claims, NOT that U; unexplained, may be noise or foes_on 0.85 x kept, F7a). Fix: `f.holdKept = HoldPoint(...) && !m.commit.on` (MovePass FillHold) + a row: standing on a marked committed place, Hold over it must need a reason beyond kept. The hysteresis on the bar: momentum and spotPreempt multiply, say so.
F2 SEV-3 refileStall / startDelay ROOT him. In the lab Pending is StrictLive: no movement of his own, he stands. Harbinger d01c2e3: area not yet carrying the new target = CarryOf kNot = Pick::kOrig, the ENGINE moves him (hm1 278, 514), Flee only denied if isFleeing. Lab is another mechanism, kinder for holds. Measured at refileStall 1.1, rooted -> uncarried: archer U 70.47 -> 69.60, evoker 62.15 -> 62.97, knight 76.15 -> 76.91, knight out% 10.0 -> 11.1, warrior 12.0 -> 13.1 (single seed family; 0.25 is the plan's margin). A sensitivity arm on this row answers the wrong question, and it is executor code (frozen). Fix: StrictLive false while Pending and now < notBefore when notBefore came from refileStall / startDelay (repointStall stays rooted), keep StrictFiled (flee denied only when fear/flee runs), Pending kept in the !live branch of StrictStep.
F3 SEV-3 (uncertain 1) rule: 4.1 and trap 8 contradict, and trap 8's reason wins: defaults must not sit at the kind end. Freeze defaults refileStall 1.1, blockTold 0; arms unchanged; the one doc line and the strict rows state their own residue already. They then go off at P-C / P-B as values, no binary change. Do F2 first. Trap: blockTold 0 + forced/oracle order that blocks never ends (dueBlock every 2 s, he walks half the time), so S5-C must not wait for an end event: a refusal or a time bound on an order.
F4 SEV-4 fear kinder than Harbinger in one case. fearSeen is not cleared by Free or by an end: fear under an order (counted), then Free (he really flees), then a new order in the same fear: carried at once. That is exactly Harbinger's deny-only case (a flee that BEGAN before the order is carried). Fix: in StrictPublish, `if (!was) q.fearSeen = 0.0`. Same for a low-health flight begun under Free (c.fleeing, not fearUntil): not modelled.
   Uncertain 3 otherwise: ONCE per effect is the right read. While carried, Flee answers NO, the Flee node's context push is not entered, so nothing sets isFleeing again; a repeat has no mechanism in d01c2e3, despite 4.1's "at a time". For a fear that lands on an order already standing the code suggests ZERO uncarried (lo arm 0 covers it). 1.5 = latch 1.5, area back within 1.0: fine as a placeholder. FIELD decides.
F5 SEV-4 a self or targetless top action (drink, self buff, summon) is no Hold, so a held member goes Free and pays a re-file. Allowing it while held: evoker Free orders 2.12 -> 1.85 a fight, stands 3.72 -> 3.44, U flat. Plan 3.1 asks for this gate, so not a defect of the branch; put it to the planner for S5-C (the oracle must be able to hold while drinking).
F6 SEV-4 the Hold's shared facts (sight, exposure, cover, KeyOfStay, plan position) are at m.p while the point is up to 128 off (uncertain 5). Pin row needs exactly that; only HoldPlayer uses `at`. Accept, say it in 22.1. S5-C must evaluate options at the order's point.
F7 SEV-4 shapes. (a) blade: foes_on 1 = 0.85 x 1.25 kept = 1.06: once held by two, one foe left keeps him held, only zero frees him: the "fights freely with one foe" ruling is true only for the first decision. (b) contact is near flat: exponent 0 / 1 / 3 / 8 moves U under 0.2 and claims under 4 points; foes_on exponent 3 moves knight U +0.9. (c) an exponent cannot change a factor's direction and has no upper bound (pow overflow -> 0, safe, never NaN); direction needs the per-class curve (px/py), which the sim's c.<i>.a|b does not reach for Piece rows. (d) hold_kept's exponent and t.orderHoldMomentum are one degree of freedom: search one. Gradient exists: spot.hold 0.5/0.85/1.2/3 moves U monotonically past 1 (archer -2.3, knight -4.3, warrior -3.7 at 3). Hold neither dead nor absolute at shipped values, but at them it costs (knight, warrior, about 1 U; F1 alone does not restore it). All placeholders.
F8 SEV-4 rows for the survivors: two foes on a bow at different times to contact (nearest counts); hold_player with a y walk; startDelay zeroed.
F9 SEV-5 C4457 MovePass.cpp(117): `Light::Read a` hides parameter a. Pre-existing (ed7ec4a5, S4), harmless (the action is used again only after the scope closes). Rename on the next touch.
F10 SEV-5 shotStop also charges a shot begun before travel started (actSeen is updated only while travelling). A short walk back interrupted by fear resumes as Travelling (no walkBackSpeed). samePoint new gen while Standing outside arriveR: Harbinger Resets and travels, lab leaves him.

## Author's uncertain spots
1 F3: refileStall 1.1, blockTold 0. 2 notCarriedShare: CODE IT NOW, before the freeze, default 0, arm 0 / 0.1: a frozen binary cannot get it later (trap 1) and the stand session reads exactly that line. Reuse F2's "uncarried" state: that share of orders is uncarried for its life. ~6 lines + a row. 3 F4. 4 displaceDownU: the lab's only fall is paralysis, so the arm is blind for classes / foe sets without it: accept, mark the row "not armed without a paralysis foe" (a SENSITIVE / INSENSITIVE verdict must not be read from such cells). 5 F6: accept.

## Judged sound
foes_on / contact split by melee (a blade is offered Hold only in reach, contact would be 0). Reach play = leashR, works. Tie goes to Stay. NaN / unread = Off. Kind mask in OrderOf. Residue complete against 4.1 with static_assert. Caps default strict,kite; forced arms and modetest ACCEPT obey; the oracle today issues no orders and gets the pass's through g_moveCaps, so S5-C must reuse AllowedKinds (its ext:sprint_back / flee are not caps-bound by design). Forced re-file after Ended (F9 old) ok. bridgeLag, arriveZ, walk back, strafe cone, short stop <= leashR read and match d01c2e3's acceptance.

## S5-C / D need
F1, F2, F3 + notCarriedShare coded BEFORE S5-C (they change picks, the executor and the baseline it is measured on); F4 with them (one line). S5-C: hold options at their own point and target-free (F5, F6); no waiting on Unreachable under blockTold 0; order rows read kOrderRows + caps. S5-D: displaceDownU cells and nearest-foe narrow known only for his three targets (the gap cells must check hold_narrow fires).

## Verdict: MERGE.

Own build of a8f9c24b (_tuning/s5-ab-r2). Scope: native = 2 lines (Catalogue foes_on points, MovePass holdKept); merges 3675d7f6 and a8f9c24b have no conflict hunk; the rest of the diff is main's S5-E1 / walls.
- F1 closed as asked (hold_kept off on a committed place + row, mutant caught). NOT closed as "hold never takes over a committed place": R2-1. Author 6 seeds, mean +- s.e.: claim% main / 3ebcca6a / fix: archer 76.88 / 32.43 / 68.27 (fix-main -8.61+-0.43), evoker 32.58 / 10.89 / 25.82 (-6.76+-0.41), knight 32.71 / 28.55 / 31.89 (-0.82+-0.34). U fix-main archer -0.30+-0.21, evoker -0.05+-0.20, knight +0.24+-0.22: the old U cost is gone.
- F2 closed: StrictLive false while now < carriedAt or notCarried; StrictFiled (flee / hide denial, fear) untouched; repointStall carried and rooted; Pending kept in !live. Same kNot as d01c2e3. Mutants on both halves caught.
- F3 closed (refileStall 1.1, blockTold 0). notCarriedShare: default 0, arms 0 / 0.1, rolled per point by gen, F2's state, row + mutant. F4 closed, mutant caught. F8 three rows, mutants caught. F7a: foes_on 1 = 0.75, row + mutant.
- F10: three not mutation-covered (shotStop row default 0; walk back under fear; same-point re-travel). Read against Harbinger: right. Acceptable (SEV-5 backlog), but backlog text "CLOSED F10" must say "no row".
- Proof seen on a8f9c24b: point / pinned / off v2 v3 v4 byte same to main fa1a8d0d's binary (1, 8 threads), strict 1 = 8, harness gcc 0 failed; candidates digest is a pinned harness Check (fbf8ec48124dd622), green on 3675d7f6. My spot tests on a8f9c24b, fresh seeds 6161 90210 424242: 120/120 each.
Findings:
R2-1 SEV-4 a Hold still takes his own committed place by the hold factors (sight 1.1 x contact 1.15 ...) that the incumbent never gets; the incumbent has no bar against it (MovePass 850 gates only new spots), and once taken the hold has hold_kept and the preempt bar. 4 seeds, 30 fights: archer ~265 takeovers against ~25800 kept and ~1900 preempted by another pick; barring Hold while an incumbent triple lives: archer claim% 68.63 -> 74.35 (+5.73+-0.23), U +0.16+-0.04; evoker 26.83 -> 29.11, U +0.17+-0.08; knight +0.47+-0.05, U -0.01+-0.09. Fix one line (skip spot<0 && kinds==Hold when inc[member] >= 0) or make Hold meet bar x (1 + spotPreempt). Planner's call before the spots search / freeze: it decides who owns a place, weights alone cannot express it.
R2-2 SEV-5 dueBlock counts from publish (blockAfterSec 0.5), not from carriedAt: at refileStall 1.1 a residue block fires on the first carried step, he never walks. Use max(now, carriedAt) + blockAfterSec.
R2-3 SEV-5 notCarried (and a point re-filed faster than 1.1 s) is invisible to the pass: no mark, no trust event. Matches the game, but an oracle / search must not read "order filed" as "order obeyed".
Verdict: MERGE once f3.sha ends F3 DONE with harness.log, msvc.log, cli.log all EXIT 0 (dated after 03:09 on 11-25: the older logs are 3675d7f6's).
S5-C: options bounded by time or refusal; hold options evaluated at the order's point, target-free (F5, F6); R2-1 decided first; reuse AllowedKinds; carriedAt/notCarried time is the engine's.
