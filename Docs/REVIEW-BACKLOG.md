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
- **Raised:** Fable review of `bd53a81`+`ed9cbbc`, restated on `625f3b7`. Outside the author's file boundary both times; the author correctly refused to reach for it.
- **Severity:** SEV-4 (design residual, currently masked by hand-selection order)
- **Finding:** `Loadout::Prepare` equips into `LeftHandSlot()` unconditionally (`Loadout.cpp:279, :359`) and at cast level 4 `DeselectSpell`s the RIGHT hand's other spell (`:296-298`). With heal-LEFT and offense-RIGHT as the designed steady state, the two fight at the equipment layer: an offense claim's first-lap `Prepare` puts its spell into the LEFT slot on top of an in-flight heal, and a heal's `Prepare` deselects the offense spell from the RIGHT.
- **Cost recorded at the site as:** "one wasted left-hand equip per offense claim on a weaponless follower" — which Fable judged an understatement once right-first landing became the norm.
- **Fix shape:** give `Prepare` the target hand. Its own brief; it changes an interface used from several call sites.

### MFO-B2 — the F2 preemption residual is structural
- **Raised:** Fable review of `376d347`+`db56802` (finding 3, SEV-4). Recorded in MAP.md by `1044816`; text verified true by the review of `625f3b7`.
- **Severity:** SEV-4
- **Finding:** `commitPreempt` fires immediately before the claim, but the refusal sites all sit AFTER it — `CastSelfDirect` -> `Held`/`Declined` (`Actuation_Direct.cpp:888/892/938`), `ClaimOffenseCast` refused, `ComposedCast::Try` -> `Held`/`ApmfRefused`, `Loadout::Prepare` failed. A deterministic refusal still displaces the incumbent and still yields lap-rate churn for that shape. Only `Try`'s `Held` is pre-checkable (pure MFO state).
- **Reviewer's reasoning for deferral:** not fixable in general without an APMF dry-run / "can you serve this?" query. Recorded so the next reader does not assume F2 is closed.

### MFO-B3 — right-first hand selection fails toward RIGHT when `bWeaponStyleControl` is OFF
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

### MFO-B7 — `kHealHoldNeverObservedMs` (4000 ms) is sized from a datum the newest session already exceeded
- **Raised:** Fable review of `e1e55fb` (F-2, SEV-4). Pre-existing; NOT introduced by this cycle. `e1e55fb` did, however, bind two further questions to the same number (see MFO-B8).
- **Severity:** SEV-4
- **Finding:** `APMFBridge.h:652-694` sizes the constant from the 0906 heal's 2.95 s claim-to-observed. The 0908 heal is **4.5-6.1 s** by `Docs/DIAG-2026-09-08-field.md`, which itself asks for >=5000 for the mere warning (`:200, :359`). For `RefreshHealCastClaim`'s competing-rule cap that means the lift (cap + a 1.1-1.3 s lap) straddles the measured fire.
- **DELIBERATELY NOT FIXED, and this is the point:** the reviewer's instruction was "report only; do not resize from n=2." Resizing now means guessing from two samples. **The action here is a MEASUREMENT, not an edit.**
- **Tension with rule 9's carve-out, stated openly:** the carve-out says anything the next field cycle exercises is never deferrable. That carve-out exists to stop a deferred FIX from corrupting the next test. Here the next field cycle IS the remedy — the heal path exercises this constant directly, so the deck log answers it. Read it from the next heal cycle rather than editing the number first.

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

### MFO-B9 — two comments in one file now state two different bounds for the same mechanism
- **Raised:** Fable review of `e1e55fb` (F-4, SEV-5)
- **Severity:** SEV-5
- **Finding:** `Actuation_Hands.cpp:439-442` (landed by `6bfbd79`, and cited as the drain for MFO-B4) says the hold's heartbeat "stops at `kHealHoldNeverObservedMs` for a claim that never fires ... bounded by that window". `Actuation_Hands.cpp:710-716` (landed by `e1e55fb`) says the real bound is the SUM, ~4.8-6.5 s, and that it also covers a claim that fired and went quiet.
- **Fix:** one-line reword at `:439-442`. A file that states two bounds for one mechanism will get the wrong one believed — the same shape as the `g_forcedWeapon` "no lock" / "guard every access" pair this cycle already had to reconcile.

### MFO-B10 — the hand-index enum in `Actuation_internal.h` is unnamed
- **Raised:** Fable review of `89085bd` (the `refactor/actuation-split` mechanical split), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** `Actuation_internal.h:168` is the only *unnamed* enum in any `*_internal.h` (Board/Logistics headers all use named `enum class`). Enumerators of an unnamed namespace-scope enum have no linkage, so strictly the header's use of `kHandCount` inside the external-linkage `FollowerCastLocks` is the same pedantic ODR corner as pre-C++17 `static const` in a header. No practical effect: the enumerators are only ever used as `std::size_t` constants, never as a type (`decltype(kHand…)` occurs nowhere). Naming it is a rename and outside this brief; record and leave.
- **Reviewer's reasoning:** the enum was file-local in `Actuation.cpp` before the split, where linkage of its enumerators could not matter; promoting it to a shared header is what surfaces the corner. It is a CORNER, not a bug — no practical effect, and the reviewer said so in the same breath. The whole cycle graded nothing above SEV-5, so rule 9 ends it here.
- **Why it was NOT fixed in the split:** naming the enum is a RENAME, which the split's brief forbids by name ("no refactors, no renames"), and a rename touching a shared header consumed by three TUs is its own change with its own review. Deferred, not dropped.
- **Fix shape when drained:** give it a name in the house style the other internal headers use (`enum class` where the call sites can take the scoping, or a plain named `enum` if the bare `kHandLeft`/`kHandRight`/`kHandCount` spellings must survive at ~60 call sites across `Actuation.cpp` and `Actuation_Hands.cpp`). Purely mechanical, but it touches every one of those call sites, so it wants a diff review of its own.

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

### MFO-B13 — F3's hold-time floor can shave a fraction off a v6-migrated, cap-saturated skill on the first v7 hold
- **Raised:** Fable round-2 review of `028eb5a` (`feat/mfo-progression-strict-points-perk-style`), SEV-5. R2-2. Adjacent to MFO-B11 (same root).
- **Severity:** SEV-5
- **Finding (verbatim):** F3's hold-time floor can shave a fraction off a v6-migrated, cap-saturated skill on the first v7 hold — PLAUSIBLE, likely invisible. Where: native/ProgAllocator.cpp:2293 (`autoPts = max(0, points − manual)`) feeding :418 (`floor(e.autoPoints + 1e-3) + manualPoints`). Scenario: v6 wrote whole-point shares (main RecomputeSkills: floor(total*w + 0.5)), so migrated autoPts is integral unless the skill was cap-clamped: then points = skillCap − natural, and if natural is fractional (engine autocalc base, ADOPT mode), autoPts inherits the fraction (e.g. natural 23.7 → auto 76.3). The v7 hold floors it → desired 99.7 instead of 100.0. Skyrim's skill UI floors, and perk conditions compare >= integers, so 99.7 reads as "99" where 100 read as "100" — a one-integer visible drop only in that exact corner (cap-saturated AND fractional natural). Adjacent to the already-recorded MFO-B11 (same root: the v6 datum is the clamped applied delta).
- **Reviewer's reasoning:** the migration carries the v6 applied delta verbatim (the only datum), and the F3 hold-time floor (a whole-point actor write) is applied to it like any v7 ledger value; the two are individually correct and only meet in the cap-saturated + fractional-natural corner, where the effect is at most one displayed integer. Nothing above SEV-3 remained in the round, so rule 9 ends the cycle here.
- **Why it was NOT fixed:** raised in the round that ended the cycle; a non-layout tweak of the migration branch with its own review cost. Deferred, not dropped.
- **Fix shape when drained (verbatim):** round the migrated autoPts up to the next whole point when points is cap-clamped (autoPts = ceil(autoPts − 1e-3)), or fold into the MFO-B11 entry as a note. Not a co-save layout change.

### MFO-B14 — the empty-hand condition signal can mark a "free off-hand" one-hand perk as unarmed
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

### MFO-B16 — after the MFO armor swap and before the vendor sale, the engine's rating-only auto-equip and `EquipBestOwnedGear` disagree; a worn off-class piece can flip back and forth until sold
- **Raised:** Fable tier-2 review of `bb77b69` (`fix/mfo-armor-class-score`), SEV-4, PLAUSIBLE.
- **Finding (verbatim):** `native/Logistics_Economy.cpp:352-376` — `EquipBestOwnedGear` rated branch, every `ServiceFollower` tick (no combat gate on the rated branch). Before this commit MFO's owned-upgrade judge and the engine's auto-equip both ranked by raw rating, so they never fought. Now: a light follower wearing steel (31) who owns leather (26 → 52) gets leather put on by MFO. The engine's own follower auto-equip re-evaluates "best armor" by raw rating on inventory adds (every loot pickup) and on default-outfit re-apply (cell load), and will put the steel back; MFO re-swaps within ≤ N×133 ms. The un-wear the author relies on (redundant-inferior force-sell) only fires inside `EconomyProbe` — a vendor visit. Until then the body slot is contested, event-paced (not per-frame), each flip emitting `[equip] OWNED` + `[armor]` + two `[armor-obs]` lines. Also `FitsCarryWeight` (pre-existing) gates the swap on an owned pick — an over-encumbered follower never gets the light piece worn by MFO and the sale strips him to the engine's choice.
- **Reviewer's reasoning (verbatim):** Not a crash, not co-save, converges at the first vendor. The brief said "No engine deny", so this window is a design consequence the brief accepted, not a defect the author could close. It IS what the next field cycle exercises — the `[armor-obs]` probe is precisely the instrument that will measure the flip rate. Read the first deck log for `[armor-obs] ... EQUIP '<heavy>'` lines that are NOT preceded by an MFO `[equip]` line (engine re-wears).
- **Why it was NOT fixed:** the fix is an engine equip-best deny/steer (new mechanism, principles 1/2) to be sized FROM the probe, not before it (Fable diagnosis 2026-09-14). marth informed.
- **Fix shape when drained:** measure the flip rate from `[armor-obs]`; if the engine re-wears, either (a) drop/sell the off-class piece without a vendor once a better-scored owned piece is worn, or (b) portal+deny on the engine's equip-best for followers MFO dresses.
- **Surfaced at edit time from:** MAP.md `EquipBestOwnedGear` / ARMOR CLASS BY SKILL "What breaks".

### MFO-B17 — armor SEV-5 notes from the same review (`bb77b69`)
- (a) `ArmorClassSuits` (`Logistics_Loot.cpp:303-310`) is now dead code with changed semantics (BASE AV); zero callers. Delete or keep as the named predicate; never re-call on a hot path.
- (b) The "allocator and wardrobe agree" comment (`Logistics_Loot.cpp:201-210`, `Logistics_internal.h:373-376`) overstates: on an exact skill tie the wardrobe takes the perk vote, `DominantArmorSkill` goes straight to light. Doc fix.
- (c) `TradeBridge::BuyThresholds` (`TradeBridge.h:91-107`) is NOT byte-shared with the .pex (in-DLL only; the .pex sees the 10 natives). Append-only was honoured anyway; stop calling it byte-shared.
- (d) Shields are half in the wear decision: `EquipBestOwnedGear:372-373` skips shields while keep buckets and force-sell score them; a worn heavy shield with an owned light one is force-sold at a vendor but MFO never puts the light one on. Pre-existing shape.

### MFO-B18 — `WeaponHandExposure` steers every cast LEFT while a force-hold exists, so a dual-wielder's left weapon yields and refills at cast cadence (DECIDED BY MARTH: by design)
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-4. F6.
- **Severity:** SEV-4 (design)
- **Finding (verbatim):** F6 — SEV-4 design, marth decides (record only): WeaponHandExposure (Actuation_Hands.cpp:163-172, consumer :872 preferRight = !exposure) steers every cast to the LEFT while any force-hold exists, and heals are left-only, so each cast on a dual-wield follower yields the left weapon and the top-up re-equips it once the lock lapses — weapon↔spell churn at cast cadence on the hand the CSTY change just filled.
- **Reviewer's reasoning:** a design consequence of two standing rules (right-first-unless-a-weapon-owns-the-right, heals left-only) meeting the new left hold, not a defect in either; the churn is bounded by the top-up floor (5 s after every yield, F1 fix `1ac3c6b`).
- **Decision (marth 2026-09-14, verbatim):** "for F6 the weapon should always yield to a spell on left hand." DECIDED, not open: a force-held left weapon ALWAYS yields to any spell that needs the left hand (heals included); the weapon<->spell churn at cast cadence is accepted by design. No right-hand cast preference and no "dual-wielder does not cast" rule may be added for this. Recorded here so the reasoning survives; nothing to drain.
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks + `Docs/STATUS.md` DECIDED item.

### MFO-B19 — `Actuation.cpp` includes `Logistics_internal.h` for two symbols
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-5. F7.
- **Severity:** SEV-5
- **Finding (verbatim):** F7 — SEV-5 backlog: #include "Logistics_internal.h" in Actuation.cpp:13 pulls a ~900-line internal header; no ODR hazard (inline globals, WeaponScore pure inline, ComputeWeaponRoles defined once); proper seam is a public declaration in Logistics.h.
- **Reviewer's reasoning:** correctness is unaffected (every definition in that header is `inline` or defined once); the cost is a layering smell and compile-time coupling only.
- **Why it was NOT fixed:** raised in a round with nothing above SEV-3 once F1-F5 were fixed; a public seam in `Logistics.h` was outside the brief's file boundary. Deferred, not dropped.
- **Fix shape when drained (verbatim):** proper seam is a public declaration in Logistics.h (declare `WeaponRoles`/`ComputeWeaponRoles`/`WeaponScore` there, or a thin `Logistics::CombatWeaponScore(actor, weapon)` wrapper, and drop the internal include from `Actuation.cpp`).
- **Surfaced at edit time from:** MAP.md §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.

### MFO-B20 — `kMaxForcedWeapons` (64) is now a PAIR cap and the FWPN reader aborts the whole load above it
- **Raised:** Fable tier-3 review of `f771399` (`feat/mfo-dualwield-combat-pick`), SEV-5. F8.
- **Severity:** SEV-5
- **Finding (verbatim):** F8 — SEV-5 backlog: kMaxForcedWeapons = 64 (Actuation.cpp:2407) is now a PAIR cap and the reader aborts the whole FWPN load above it (:2447-2450). Unreachable at party scale.
- **Reviewer's reasoning:** a dual hold writes two pairs per follower, so the cap is effectively 32 dual-wielding followers; the party is bounded far below that, and the abort path only drops the release sweep (a session starts hold-free anyway once the gambit re-forces).
- **Why it was NOT fixed:** unreachable at party scale; raised in a round with nothing above SEV-3 once F1-F5 were fixed. Deferred, not dropped.
- **Fix shape when drained (verbatim):** raise the cap to `2 * kMaxFollowers`-shaped headroom or make the reader skip (not abort) past it; not a layout change (the count field is unchanged).
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

### MFO-B23 — CoLoad both-hands-same-form: the second unequip is slot-less by object after the left-slot one
- **Raised:** Fable round-2 review of `1ac3c6b` (`feat/mfo-dualwield-combat-pick`), SEV-5. F-F.
- **Severity:** SEV-5
- **Finding (verbatim):** F-F — SEV-5 backlog: CoLoad both-hands-same-form (:2552-2553): the second unequip is slot-less by object after the left-slot unequip is queued ahead of it; passing the right-hand slot would make it unambiguous. (Note: there is no RightHandSlot() helper; if you add one it is the LookupByID<BGSEquipSlot>(0x00013F42) twin of LeftHandSlot — only do it if trivial and in-boundary, otherwise backlog as-is.)
- **Reviewer's reasoning:** only the same-form count>=2 dual hold reaches this branch on load; the two queued unequips target the same object and which instance the slot-less second one clears is the same unverified engine question STATUS.md already carries for the F4 probe.
- **Why it was NOT fixed:** backlogged as-is per the coordinator (no `RightHandSlot()` helper exists; adding one is its own small brief). Deferred, not dropped.
- **Fix shape when drained (verbatim):** passing the right-hand slot would make it unambiguous — add `Loadout::RightHandSlot()` (the `kRightHandEquip` twin of `LeftHandSlot`, verify the default-object id in the pinned tree) and pass it on the `inRight` unequip.
- **Surfaced at edit time from:** MAP.md §1 FWPN entry + §2 Actuation "COMBAT PICK + DUAL WIELD BY PERKS" What-breaks.

### MFO-B24 — keep gates the second one-hander on `keepRoles.melee`, loot/buy on `meleeTargetClass`; a base caster keeps two daggers but never fetches a second
- **Raised:** Fable review of `b3ac577` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5 (a).
- **Severity:** SEV-5
- **Finding (verbatim):** SEV-5 keep gates the second 1H on `keepRoles.melee == OneHand` (Logistics_Economy.cpp:687) while loot/buy gate on `meleeTargetClass == OneHand` (Loot_Equipment.cpp:147, Economy.cpp:459), which is `Other` for a base caster → a caster with a dagger and dual-wield votes keeps two daggers but never fetches a second; harmless; MAP's "ONE rule" overstated for casters.
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

### MFO-B26 — the reconcile back-off lifts on ANY inventory change, so under churn "one warn per 60 s" becomes "one warn per change + 3 passes"
- **Raised:** Fable review of `b3ac577` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5 (c).
- **Severity:** SEV-5
- **Finding (verbatim):** SEV-5 back-off lift: the fingerprint includes loose stack count and every worn (base,uid); any legitimate socket elsewhere or worn swap lifts the back-off, restarts the 3-pass count and re-warns — under heavy churn "one warn per 60 s" becomes "one warn per inventory change + 3 passes"; bounded and loud by design.
- **Reviewer's reasoning:** bounded (every re-warn costs three passes of a changed inventory) and loud by design; the failure it reports is real each time.
- **Why it was NOT fixed:** the lift-on-change is what keeps the back-off from masking a request that WOULD now succeed; narrowing the fingerprint trades that for quieter logs and wants a deck measurement first. Deferred, not dropped.
- **Fix shape when drained (verbatim):** narrow the fingerprint to the stalled key's own inputs (that item's worn (base,uid) + that gem base's loose count) so an unrelated socket/swap does not lift it; or keep the lift but suppress the re-warn within the original 60 s window.
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "RETURN LOGGING + STALL DETECTOR" What-breaks.

### MFO-B27 — the stall detector's progress signal is the per-ITEM empty count shared by both ops; another slot landing on the same item resets a genuinely stuck key
- **Raised:** Fable round-2 review of `5f814d5` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** B27 SEV-5 — MEOBridge.cpp:371 progress signal is the per-ITEM empty count shared by both ops; no false STUCK reachable, but another slot's landing on the same item resets a genuinely stuck key's passes (false progress), delaying detection by up to (empty slots × 3) passes; bounded.
- **Reviewer's reasoning:** no false STUCK is reachable (a key only advances when the count is unchanged); the only cost is delayed detection while sibling slots on the same item still land, bounded by the item's empty-slot count.
- **Why it was NOT fixed:** bounded and in the safe direction (later, never spurious); a per-slot progress signal needs `GetGemDetails` re-read per slot per pass and its own review. Deferred, not dropped.
- **Fix shape when drained (verbatim):** key progress on the SLOT, not the item: record `filled.contains(slot)` (from `GetGemDetails`) at issue and treat "this slot is still empty next pass" as no-progress, independent of sibling slots; the unsocket op mirrors it with "this slot is still filled".
- **Surfaced at edit time from:** MAP.md §7 MEOBridge "RETURN LOGGING + STALL DETECTOR" What-breaks.

### MFO-B28 — bought weapons never reach `AcquireEquip`, so a BOUGHT primary upgrade carries no gems (pre-existing)
- **Raised:** Fable round-2 review of `5f814d5` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-5.
- **Severity:** SEV-5
- **Finding (verbatim):** B28 SEV-5 — bought weapons never reach AcquireEquip (Logistics_Economy.cpp:330-378 picks ARMO only), so a BOUGHT primary upgrade wielded by Actuation's EquipObject (Actuation.cpp:1924/1935) carries no gems (QueueGemMove has exactly one caller); pre-existing, outside this SHA.
- **Reviewer's reasoning:** `EquipBestOwnedGear`'s rated branch only ever picks armor, so the one `QueueGemMove` caller (`AcquireEquip`) never sees a bought weapon; the combat equip in Actuation is a plain `EquipObject` with no gem carry. Pre-existing; the SHA did not change it.
- **Why it was NOT fixed:** outside the SHA and the brief (Actuation + the owned-weapon wear decision are separate mechanisms). Deferred, not dropped.
- **Fix shape when drained (verbatim):** give `EquipBestOwnedGear` (or the trade completion path) a WEAPON branch that routes a bought/owned in-role primary upgrade through `AcquireEquip(a_src=nullptr, myWeap, forceStock=false)` so the same-role gem capture + `QueueGemMove` run for it; the combat equip then finds the gems already moved.
- **Surfaced at edit time from:** MAP.md §4 Logistics "CLOSED 2026-09-14 — THE SECOND ONE-HANDER" What-breaks + §7 MEOBridge gem-transfer entry.

---

### MFO-B29 — on a FULLY-socketed item the back-off's expiry re-entry point is the swap-up, so the eviction bounce recurs every ~63 s while MEO keeps refusing, and the wiped key means the warn never escalates
- **Raised:** Fable round-3 review of `64512aa` (`fix/mfo-deck-0914-helmet-offhand-verdict-meo`), SEV-4, CONFIRMED by trace.
- **Finding (verbatim):** `native/MEOBridge.cpp:518-521` (swap-up skip reads only `backoffUntil`), `:546-550` (erase_if drops an expired, untouched key). On the reported case (M fully socketed, W in slot s, G better and refused) the slot loop never touches `keyG`, so at expiry: `socketBackedOff` → false → swap-up evicts W → end of pass `keyG` is ERASED (untouched, not backed off). Next passes: (s,G) is a NEW key → issue, issue, STUCK + warn + back-off → excluded → W re-socketed. Per cycle (~60 s + 3-4 passes): 1 unsocket, 2 socket-G, 1 socket-W, one full STUCK warn that always reads "accepted 2 time(s)". Not the round-2 SEV-2 (that was self-lifted every ~5 s; this honours the full 60 s and is loud), and it IS the retry contract MAP.md documents — but the retry on a full item is an eviction of a worn gem, ~5 % duty loss of W and 4 MEO ops/min, forever.
- **Reviewer's reasoning:** carve-out (b) question: if marth's pass criterion is "M stops bouncing" fix before deploy; if "bounce ≤ once per back-off window, warned" is acceptable, defer.
- **Why it was NOT fixed:** coordinator call 2026-09-14: loud, bounded to once per 60 s window, does not corrupt the next test (the STUCK line still names the request); deploy proceeds; marth informed.
- **Fix shape when drained (verbatim):** (i) at expiry retry ONLY via the slot path — the swap-up keeps skipping a gem whose key EXISTS for this item (expired or not); the key is then erased only by the fingerprint-change lift; or (ii) keep `reported`/pass history across expiry and grow the back-off (exponential) so the warn escalates instead of repeating.

### MFO-B30/B31/B32 — SEV-5 notes from the same review (`64512aa`)
- **B30:** stuck key is per-SLOT, exclusion per-ITEM, and a uid-0 key does not cover the minted uid (`:473`, `:436`, `:398-404`): each newly-opened slot (and the first post-mint pass) re-runs the 2-issue + warn detour for G; bounded by capacity (≤ 8 per item), never spurious. Note against B27.
- **B31 (B26 addendum):** our OWN legitimate swap-up on the item is a fingerprint change that lifts G's back-off in the SLOT path (`:364-369`, `:512-537`): a three-pass detour (2 wasted G issues + a repeat STUCK warn) per legitimate swap-up on that item; no loop.
- **B32 (B28 broadened):** route-2b LOOSE-looted weapons never reach `AcquireEquip` either (`AcquireEquip` callers: `Logistics_Loot_Equipment.cpp:473`, `Logistics_Economy.cpp:417` armor-only; `Logistics_Loot.cpp:1835` only sets `lootable`), so a loose primary one-hander is worn by Actuation's plain `EquipObject` with no gem carry. B28's fix shape becomes "bought OR loose-looted OR owned weapon".

### MFO-B33 — LEFTOVER `off-domain` / `duplicate-copy` WARN every 60 s in by-design steady states
- **Raised:** Fable tier-3 review of `1efa3e3` (`fix/mfo-meo-no-loose-gems`), SEV-5 #3, record only.
- **Finding (verbatim):** kOffDomain and kDuplicateCopy WARN every 60 s in by-design steady states (one loose armor gem, all armor full, a weapon socket open); suggest INFO; marth's call.
- **Reviewer's reasoning:** the invariant is "no loose gem while a socket it FITS is open"; an off-domain gem with only an off-domain socket open is not a broken invariant, and a junk copy that never sells (economy off) is a steady state, so both warn on a condition the user cannot act on from MFO's side.
- **Why it was NOT fixed:** coordinator call 2026-09-15: log LEVEL is marth's call; only `minting` is INFO today (`LeftoverWhy` → the `spdlog::info`/`warn` split at the LEFTOVER issue site, `native/MEOBridge.cpp`).
- **Fix shape when drained (verbatim):** suggest INFO for kOffDomain and kDuplicateCopy; keep stuck / capacity / support-limit / refused / unclassified at WARN.
- **Addendum (Fable round 2 on `9dacc0e`, coordinator 2026-09-15):** `support-limit` belongs on the by-design-steady-state list too (a loose second support gem with every dual-socket item already holding one warns every 60 s on a condition MFO cannot change); same call, same fix shape.

### MFO-B35 — the duplicate-copy deferral is redundant per MEO's in-place mint and starves the worn item while a spare copy is carried
- **Raised:** Fable round 2 on `9dacc0e` (`fix/mfo-meo-no-loose-gems`), SEV-4, PLAUSIBLE, pre-existing guard (the deferral predates the branch; the branch narrowed it to UNWORN copies).
- **Finding (verbatim):** the duplicate-copy deferral is redundant per MEO plugin.cpp:9139-9153 and costs the worn item its gems for as long as a spare copy is carried. :586 defers every uid-0 request while invBaseCount > wornCopies, on the theory (:579-581) that a uid-0 SocketGem could mint on the wrong instance. MEO's uid-0 path (ApiEnsureInstanceXList) mints IN PLACE on the first xList that is worn AND has no ExtraUniqueID AND no kEnchantment; it falls through to the drop-stamp-pickup of an unworn copy ONLY when no such worn xList exists. MFO reaches :586 only for an item whose worn xList it already located (addItem :437), uid 0 and !xlEnchanted (:562) — exactly the instance MEO will pick; the junk copy is never a candidate. Cost: a follower carrying a second Iron Dagger never gets the worn one gemmed until the spare sells — with economy off, never.
- **Reviewer's reasoning:** as above — the guard defends against a path MEO's own code cannot take for the instance MFO targets.
- **Why it was NOT fixed:** coordinator call 2026-09-15: round returned nothing above SEV-3, cycle ended (rule 9); the deferral is loud (LEFTOVER `duplicate-copy` every 60 s) and cannot corrupt the next test.
- **Fix shape when drained (verbatim):** drop the dupDeferred branch and kDuplicateCopy (keep mintedBases one-per-base-per-pass).

### MFO-B36 — the swap-up never decrements `avail[loot]`, so two items can evict their weakest for one loose copy
- **Raised:** Fable round 2 on `9dacc0e` (`fix/mfo-meo-no-loose-gems`), SEV-5, CONFIRMED.
- **Finding (verbatim):** the swap-up never decrements avail[loot], so item A and a later item B can both evict their weakest for the same single loose copy; next pass one re-fills with its own evicted gem — one wasted unsocket/re-socket, no loop.
- **Reviewer's reasoning:** bounded to one wasted pair of MEO ops per such pass; no loop, no loss.
- **Why it was NOT fixed:** coordinator call 2026-09-15: nothing above SEV-3 this round, cycle ended (rule 9); tier 2 is OFF by default and gem CHOICE is marth's planned redesign.
- **Fix shape when drained (verbatim):** reserve the candidate at the swap-out (`--avail[loot]` when `UnsocketGem` returns true) so a later item cannot evict for the same copy.
- **FIXED-IN `fix/mfo-loot-m1`** (loot round M1, 2026-09-24): the fix shape as written, `--avail[loot]` when `UnsocketGem` returns true (`native/MEOBridge.cpp`, the swap-up block). The LEFTOVER line's `swapPending` subtraction is removed with it (and the now-dead `swapPending` map), because the reserved copy is already out of `avail[]` and subtracting it again would count it twice. Pending its Opus 5.5 review on the branch.

### MFO-B37 — `Actuation.cpp` is over the 2500-line cap (2678): the split is its own brief
- **Raised:** Fable tier-3 review of `87cabc1` (`feat/mfo-1.5.97-pass`), SEV-5 hygiene, CONFIRMED (`wc -l`: 2605 on `main` `5e1c41b` before the branch, 2678 after round 2's gate comments and the EquipLeftHeld refusal logging).
- **Severity:** SEV-5 (process; no behaviour)
- **Finding (verbatim):** Actuation.cpp 2615 lines reported.
- **Reviewer's reasoning:** CLAUDE.md's 2500-line HARD RULE is overridden by scope rule 1 inside an unrelated task — crossing the cap is a STOP-and-report, never a licence to split; a TU split is tier 3 (never "CI-identical") and needs its own field cycle.
- **Why it was NOT fixed:** the 1.5.97 pass brief said "cap relaxed — report, do not split". The file was already over the cap on `main` (the dual-wield / combat-pick work of 2026-09-13 took it from ~2480 to 2605); this branch added 17 comment lines.
- **Fix shape when drained (verbatim):** a dedicated split brief for `Actuation.cpp` — candidates are the weapon equip block (`WeaponRolesFor` `:1677`, `IsOneHandMelee` `:1685`, `PickOffHandWeapon` `:1695`, `EquipLeftHeld` `:1748`, `EquipWeapon` `:1782`, the `g_forcedWeapon` ledger + `ReconcileForcedWeapon`/`CoLoadForcedWeapons`) into an `Actuation_Equip.cpp` next to `Actuation_Direct.cpp`/`Actuation_Hands.cpp`, shared state through `Actuation_internal.h`, as the two earlier splits did. Full adversarial Fable review, one field cycle on 1.6.1170 before the next feature lands on it.
- **DRAINED by `refactor/subsystem-folders-wave1` (2026-09-24), pending that branch's tier-A review + field cycle.** The dedicated split brief (marth 2026-09-24, "Go with the subsystem folders, tool first") moved the whole Actuation family to `native/cast/` by concern; the equip block this entry names is `cast/Equip.cpp` (`EquipWeapon`, the weapon-style helpers, the `g_forcedWeapon` ledger, `ReconcileForcedWeapon`, the FWPN co-save), largest file `cast/CastOn.cpp` at 1387 lines. The same wave split `APMFBridge.cpp` (the file MFO-B40's addendum asked to include) into `native/apmf/` and `ProgAllocator.cpp` into `native/progression/`. Proof: `tools/splitcheck` function-by-function over the CI builds + `tools/splitcheck/linecheck.py`. Close this entry when that review comes back clean.

### MFO-B38 — "one judge" holds for the RATED armor branch only; mage-mode may stock a piece legacy wore
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-4 (F11).
- **Severity:** SEV-4
- **Finding (verbatim):** F11 (one-judge holds for the rated branch only; mage-mode may stock a piece legacy wore).
- **Reviewer's reasoning:** `ComputeOwnedGearPick` is EquipBestOwnedGear's pick verbatim, but the mage-apparel branch's equip carried a thrash guard (OOC-only + 5 s per follower) that the declaration does not: the declaration names the pick every tick it changes, while the legacy equip would have waited. A piece the mage branch would have worn after its guard can be declared (and worn by APMF) earlier, or — with the pick replacing the overlapping worn clothing in the set — the worn piece drops to stock a tick before legacy would have swapped it.
- **Why it was NOT fixed:** below the severity floor; the declaration's own change gate bounds it to one send per real change, and the field cycle is observe-only.
- **Fix shape when drained (verbatim):** carry the mage branch's thrash guard into the declaration (only declare the mage pick when EquipBestOwnedGear's guard would have let it through), or drop the guard from legacy so both roads agree.

### MFO-B39 — `bApmfEquipAuthority` is the first per-feature APMF toggle in the MCM; conflicts with the recorded consolidation plan
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-5 (F12). marth's call.
- **Severity:** SEV-5 (policy)
- **Finding (verbatim):** F12 (bApmfEquipAuthority is the first per-feature APMF toggle in the MCM, conflicts with the recorded consolidation plan — marth call).
- **Reviewer's reasoning:** memory `mcm-apmf-toggle-consolidation` records the plan to drop per-feature APMF toggles and keep ONE global force-fallback in Debug (default OFF); `bApmfCast`/`bApmfLootTravel`/`bApmfRetreat` are INI-only, this one is exposed in the MCM General tab.
- **Why it was NOT fixed:** the brief asked for the MCM toggle by name; the consolidation is marth's decision, not a review fix.
- **Fix shape when drained (verbatim):** either move `bApmfEquipAuthority` to INI-only beside the other three, or fold all four into the planned single Debug-tab fallback switch. The INI key name is frozen either way (MCM-Helper persistence identity).

### MFO-B40 — `Actuation.cpp` 2803: the split brief (MFO-B37) should precede the v8 re-mirror
- **Raised:** Fable tier-3 review of `c66dc80` (`feat/mfo-equip-authority`), SEV-5 hygiene (F13).
- **Severity:** SEV-5 (process; no behaviour)
- **Finding (verbatim):** F13 (Actuation.cpp 2803: split brief B37 should precede the v8 re-mirror).
- **Reviewer's reasoning:** the branch adds `PostHandEquipsDeferred` / `DeclareFromLedger` and the authority branches inside `EquipWeapon` on top of a file already over the cap (MFO-B37); the v8 hand-aware `SetEquipSetEx` re-mirror will REMOVE the deferred hop and touch the same block again — doing that on a split file costs one review, doing it before the split costs two.
- **Why it was NOT fixed:** scope rule 1 — a split is its own brief and its own field cycle.
- **Fix shape when drained (verbatim):** run MFO-B37's split (`Actuation_Equip.cpp`) BEFORE the v8 re-mirror brief; the v8 brief then retires `PostHandEquipsDeferred` inside the new TU.
- **Still growing, 2026-09-22 (`fix/mfo-spell-authority-0922`, REPORTED not acted on):** `Actuation.cpp` is now **2978** lines — the stand-down re-arm added ~75 to a file already over the cap. Per CLAUDE.md's scope rule the cap is a STOP-and-report, never a licence to split inside an unrelated task, so nothing was split. **Also now on the watch list: `APMFBridge.cpp` is 2471** and the ch.8 gate put ~200 of that there, so the next addition to that file crosses the cap as well and `MFO-B37`'s split brief should be scoped to cover both.
- **Split landed, 2026-09-24 (`refactor/subsystem-folders-wave1`, see MFO-B37):** both files are split (`native/cast/`, `native/apmf/`), which is the ordering this entry asked for: the v8 re-mirror now lands on the split file (`cast/Equip.cpp`). Note for whoever closes it: `PostHandEquipsDeferred` no longer exists in the tree (`grep` 2026-09-24: only two comments still name it, `Followers.cpp:348` and `Logistics_Economy.cpp:972`). Close with MFO-B37 on that branch's review.

### MFO-B41 — a displaced hand with an unchanged ledger is never re-equipped under the authority; the equip rule re-Fires every lap
- **Raised:** Fable tier-3 review of `e0b52b5` (`feat/mfo-equip-authority`), SEV-4 (F-B).
- **Severity:** SEV-4
- **Finding (verbatim):** F-B (a displaced hand with an unchanged ledger is never re-equipped under the authority; equip rule re-Fires every lap — fix shape: erase g_lastDeclared[id] before DeclareFromLedger when the ledger's weapon is in neither hand and the ledger did not change).
- **Reviewer's reasoning:** under the authority MFO makes no equip call; the declaration is sent only on change (F3). If something un-seated (a script, an observe-mode engine take-back) removes the ledger's weapon from the hand, the ledger is unchanged, the next lap's `EquipWeapon` Fires again (the hand no longer holds the category), writes the same ledger, and `DeclareFromLedger` compares equal and sends nothing — APMF never re-equips, and the rule Fires every lap.
- **Why it was NOT fixed:** below the severity floor; the cycle ended at nothing above SEV-3; observe mode is where it shows and enforcement (the seat refusing the take-back) is what stops the common cause.
- **Fix shape when drained (verbatim):** erase `g_lastDeclared[id]` before `DeclareFromLedger` when the ledger's weapon is in neither hand and the ledger did not change.

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

### MFO-B45 — scope and set are two enqueues; a Drain can land between them
- **Raised:** Fable tier-B review of `7857446` (`feat/mfo-equip-authority-v9`), SEV-5 (F3).
- **Severity:** SEV-5
- **Finding (verbatim):** F3 SEV-5 scope and set are two enqueues, a Drain can land between them (one-frame self-healing mismatch; the comments at Logistics_Economy.cpp:933-937 / APMFBridge.cpp:1685-1688 / APMFBridge.h:588-589 overstate "same Drain" — soften them).
- **Reviewer's reasoning:** `DeclareEquipScope` and `DeclareEquipSet` are two thread-safe enqueues from the worker; APMF's per-frame Drain on the game thread can run between them, so for one frame the seat pairs the NEW scope with the OLD set. The order is fixed (scope first), so the reverse (a stale scope over a new set) cannot occur, and the next Drain heals it.
- **Why it was NOT fixed:** below the floor; a one-frame window with a fixed, safe order. The three comments were softened in the same round (they no longer claim "same Drain" as a guarantee).
- **Fix shape when drained (verbatim):** none needed beyond the comment fix; an atomic scope+set call would be an APMF ABI change.

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

## DRAINED

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
- **Raised:** author of `fix/mfo-spell-authority-0922`, 2026-09-22, under the brief's own instruction ("include only if it does not risk the round ... If this makes the round too large, STOP and report it as deferred rather than half-doing it").
- **Severity:** SEV-2 (a follower stood visibly bare for 2 min 36 s in the field, so this is NOT a cosmetic backlog item)
- **Finding (verbatim, from the brief):** "The declaration is 'one judged pick + everything else worn' (`Logistics_Economy.cpp:835,930-937`), so a piece the engine's outfit-apply displaced falls out of the set; the re-declare went into a 3D-absent actor (APMF logged 'equip pass skipped, re-declare once loaded' and MFO never re-sent); then SEND-ONLY-ON-CHANGE (`:950`) swallowed 7 correct picks and the follower stood bare for 2 min 36 s. Fix: declare best-per-slot from the judge rather than pick+worn; treat 'a declared piece is not worn at the next service and nothing is in flight' as a change (erase the last-declared cache and re-send, MFO-B41's shape generalised to armor); skip declaring while `!Is3DLoaded()` and mark dirty so the first loaded tick re-sends."
- **Why it was NOT fixed:** it is three independent changes to `RefreshEquipDeclaration` (a new best-per-slot judge output, a worn-set drift detector with its own in-flight notion, and a 3D gate with a dirty flag), in the same function this round already touched for the shield sale, on a round with NO full Fable review. Half of it would ship a declaration that is neither pick+worn nor best-per-slot. The engine-side trigger is also still open: the `08:25:10` `OutfitApply` made the shield and the hood helmet VANISH from `GetInventory` while APMF still listed the shield, and `field-diag-2026-09-22` records that as UNRESOLVED pending a `TESContainerChangedEvent` probe.
- **Fix shape when drained (verbatim):** as the brief states it above. Take the `TESContainerChangedEvent` probe first, because "a declared piece is not worn" and "a declared piece is not in the inventory at all" want different answers.
- **PARTLY DRAINED 2026-09-22 on `fix/mfo-declaration-hygiene-b63` (off `main` `e522f61` = v2.0.10), after marth confirmed it still happening on v2.0.10. Two of the three parts SHIPPED; best-per-slot is STILL OPEN (below).** No Fable review on the round (LOW TOKEN WEEK, marth's order): coordinator diff read plus green CI.
  - **SHIPPED — "not worn is a change".** At the SEND-ONLY-ON-CHANGE compare in `RefreshEquipDeclaration`, a set that matches the last one is still re-sent when any declared ARMO the follower still owns is not on his body. Enforced mode only (`IsEquipAuthorityEnforced`; in observe mode declared-and-not-worn is the designed state). Throttled to `kDeclDriftHold` = 3 s via `g_lastDeclSentAt`, which is ALSO the "nothing is in flight" test, because APMF exposes no "the pass ran" query. `MFO-B41`'s weapon-ledger detector generalised to armor, on the same hold. Non-ARMO keys and forms he no longer owns are skipped.
  - **SHIPPED — no declaration while `!Is3DLoaded()`.** The send is skipped and `g_lastDeclared` + `g_lastDeclSentAt` are dropped, which IS the dirty mark, so the first loaded tick sends the rebuild as a change. The standing claim is kept.
  - **NOT taken: the `TESContainerChangedEvent` probe.** The shield and hood helmet VANISHING from `GetInventory()` after the `08:25:10` `OutfitApply` is untouched and is its own brief. The re-send is written so it cannot make that worse: a form he no longer owns is skipped by the drift test, so a vanished piece never drives a re-send loop.
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
- **Fix shape when drained (verbatim, REVISED 2026-09-22):** from the first field log, (1) confirm NO `[t2c] ... ch.8 select DENY` names an ALCH form while a follower is low on health with potions -- if one appears, the potion enumeration is reaching the wrong set and must be widened before anything else; (2) count how often `the gate is NOT claimed` fires and on whom -- if ordinary followers are overflowing 32, the answer is an APMF-side `kMaxSpellAllowList` raise (its own brief, byte-shared header, tier A), NOT truncation on MFO's side; (3) only then read the remaining `DENY` lines against the `[cast-select]` allow-list line for a missing exempt class.

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

- **MFO-B73 (SEV-4, raised against f5ea00e, Opus 5.5 tier-A round 2).** "An outside positive write larger than W is measured as an award and permanently inflates W by X - W_old." With W >= X the detection line prints and the hold reverts the write; with W < X (any new or low-level follower) the write is counted as an engine award, withheld with no credit, W becomes X and no line prints, so later real awards are under-measured by X - W_old for good. Belongs with MFO-B70 (what to do about outside changes).
- **MFO-B74 (SEV-5, raised against f5ea00e).** "A false 'outside change' line prints once for a granted fixed-stat follower on a load-time re-slam." The difference is -G (the granted points). Latched, one line, noise not correctness.
- **MFO-B75 (SEV-4, raised against dd4c56f, Opus 5.5 review of fix/mfo-summon-no-fanout).** "Script-archetype mod summons (OnEffectStart PlaceAtMe) do not match kSummonCreature and still fan out under AUTO; CasterHasLiveSummon cannot see them either." Reasoning: the cast-once rule in `CastAuto` keys on the kSummonCreature archetype only, so a mod summon built as a Script effect is still treated as an ordinary beneficial spell (one cast per party member) and its creature is invisible to the liveness guard.
- **MFO-B76 (SEV-5, raised against dd4c56f).** "A summon whose effect is flagged hostile/detrimental now fires out of combat via AUTO and skips the hostile-branch Sightline LoS check; no vanilla summon affected." Reasoning: the summon block runs before the hostile/beneficial split, so a hostile-flagged summon no longer needs a foe in the combat group and never reaches the LoS gate.
- **MFO-B77 (evidence gap, raised against dd4c56f).** "Serana's raise spells are Reanimate archetype and still fan out under AUTO (CastSpellImmediate(reanimate, living ally)); confirm which spell froze before calling the freeze fixed." Reasoning: the fix covers kSummonCreature only; if Serana's freezing spell is a Reanimate spell, AUTO still casts it once per living party member and the freeze lead is untouched.

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

### MFO-B84 (SEV-4) -- Reanimate still takes the old cast roads; its liveness read looks at the wrong actor
Raised against 78f9631 (fix/mfo-summon-oneshot), Opus 5.5 tier-B review 2026-09-24. Verbatim: "Reanimate still takes the old roads and CasterHasLiveSummon's ReanimateEffect match reads the caster's list though the effect sits on the corpse." Reasoning: `IsSummonSpell` matches kSummonCreature only, so a raise-dead gambit still goes through CastOn / CastSelfDirect / CastAuto (a `cast_self` raise-dead row can hit the same self-stream stale-dispel loop the summon fix removed), and `CasterHasLiveSummon`'s `ReanimateEffect` branch scans the CASTER's active effects while the engine puts that effect on the reanimated corpse, so it likely never sees a live thrall. Relates to MFO-B77. (Numbered B83 on the branch before the merge with main, whose B83 is the F1b items.) The CasterHasLiveSummon calls left on the worker for this path (Actuation.cpp Fire guard, Logistics.cpp OOC guard) are the pre-existing off-thread active-effect read the review flagged; summons no longer use them.

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
- **Reviewer's reasoning:** cohesion, not correctness; the header carries logic and a cross-thread mutex it would not normally hold.
- **Why it was NOT fixed:** a move is its own brief (scope rule 1; a TU split is tier 3).
- **Fix shape when drained:** in the wave-2 Logistics split, move it to `native/loot/Gate.cpp` (or `Logistics_Gate.{h,cpp}`) with its own small header; `TravelFailedRecently` keeps calling `GatedNow` through that header.
- **Surfaced at edit time from:** MAP.md §4 Logistics "LOOT ROUND M1" What-breaks.

### MFO-B90 (SEV-4) -- actor-block check only walks two cells
Raised against e39cee2 (loot M1, author's least-sure spot 5), coordinator 2026-09-24. `FindActorBlocker` (Logistics_internal.h) walks only the follower's and the player's attached cells with ForEachReferenceInRange, so near an exterior cell border an actor standing in a third cell is missed and an actor jam is read as a GATE (with no timer, that gate lasts until a door/lever/cell event). Prefer the engine's loaded-actor list (ProcessLists high/middle actors) filtered by the 200 u cone, which is cell-independent. Also note spot 4: a block that is neither a gate nor an actor (clutter physics) is GATED and only clears on a door/lever/cell event (marth's no-timer rule; accepted, documented).

### MFO-B91 (SEV-4, latent) -- `PerkPointsAvailable` is declared `inline` in the public header but defined only in the internal header
Raised against 26dd9bd (`refactor/subsystem-folders-wave1`, tier-A review of the wave-1 split), 2026-09-24. `progression/ProgAllocator.h:424` declares `inline int PerkPointsAvailable(const ProgState&)`; the only definition is `progression/ProgAllocator_internal.h:238`. Every caller today (PerkGate, Harness, Verbs, Poll) includes the internal header, so it builds and behaves. A TU outside `progression/` that includes only the public header and calls it would odr-use an inline function with no definition in that TU (a compile/link error, not a silent miscompile). Reviewer's reasoning: the public header promises a function it cannot deliver on its own; the split made the declaration `inline` (to match the internal-header definition) instead of keeping the definition next to it. Why it was NOT fixed: the split round is a pure move (no code changes to the split files). Fix shape when drained: either move the definition into `ProgAllocator.h` (it reads only `ProgState` and `g_econ`, so check `g_econ`'s visibility first) or drop it from the public header if nothing outside `progression/` needs it. Surfaced at edit time from: MAP.md progression "What breaks".

### MFO-B92 (SEV-4, latent) -- splitcheck: the text-prefix rule for unnamed read-only data is broader than documented
Raised against 31d97ab (`fix/splitcheck-hardening`, review of the splitcheck hardening), 2026-09-24. Reviewer's verbatim finding: "text-first rule in _unnamed_equal: any unnamed rodata whose first bytes are printable then NUL (small-int tables e.g. {50,..}->"2", UTF-16 L"..") compared only as the short string -> tail hidden." Reviewer's reasoning: "empirical scan of b0 (37972d1 /Od): 2406 rodata targets from MFO:: fns; all text-looking ones are genuine literals (nonzero tails = separate UTF-16 lib literals). finding (a) = latent only -> SEV-4." Note (author, R2): the R2 fix also reads an object whose first byte is 0 in BOTH builds as the empty literal "" (it may stop at a pooled literal only one build has), which is the same residual class. Fix shape when drained: decide string-vs-data from the referencing instruction's access width / the consumer (a `lea` fed to a string parameter vs a sized load), not from the bytes.

### MFO-B93 (SEV-4) -- splitcheck provenance does not bind the PDB
Raised against 31d97ab (`fix/splitcheck-hardening`), 2026-09-24. Reviewer's verbatim finding: "Q4 provenance: same SHA / missing / other-run all refused (hash-bound). PDB not bound (SEV-4)." Reviewer's reasoning: the build record binds each MFO.dll by SHA-256, but the MFO.pdb beside it is trusted; a PDB from another build would steer symbol matching. Fix shape when drained: record the PDB's RSDS GUID+age (or its SHA-256) in build-info and have `check_provenance` compare it with the DLL's debug directory and the PDB file.

### MFO-B94 (SEV-5) -- splitcheck F5: a twin vs a unique symbol with no module record is accepted by name
Raised against 31d97ab (`fix/splitcheck-hardening`), 2026-09-24. Reviewer's verbatim finding (review summary, SEV-5): "F5 no-module accept". Reviewer's reasoning: `names_match` TU-checks a twin on one side against a unique symbol on the other through the unique symbol's owning module, but when that symbol has no module record (globals/publics only) it still accepts by name (counted in the notes); on wave 1 every one of the 77 hits had a module record. Fix shape when drained: FAIL (or report) the no-module case instead of accepting it.
