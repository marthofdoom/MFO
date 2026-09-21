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

### MFO-B37 — `Actuation.cpp` is over the 2500-line cap (2678): the split is its own brief
- **Raised:** Fable tier-3 review of `87cabc1` (`feat/mfo-1.5.97-pass`), SEV-5 hygiene, CONFIRMED (`wc -l`: 2605 on `main` `5e1c41b` before the branch, 2678 after round 2's gate comments and the EquipLeftHeld refusal logging).
- **Severity:** SEV-5 (process; no behaviour)
- **Finding (verbatim):** Actuation.cpp 2615 lines reported.
- **Reviewer's reasoning:** CLAUDE.md's 2500-line HARD RULE is overridden by scope rule 1 inside an unrelated task — crossing the cap is a STOP-and-report, never a licence to split; a TU split is tier 3 (never "CI-identical") and needs its own field cycle.
- **Why it was NOT fixed:** the 1.5.97 pass brief said "cap relaxed — report, do not split". The file was already over the cap on `main` (the dual-wield / combat-pick work of 2026-09-13 took it from ~2480 to 2605); this branch added 17 comment lines.
- **Fix shape when drained (verbatim):** a dedicated split brief for `Actuation.cpp` — candidates are the weapon equip block (`WeaponRolesFor` `:1677`, `IsOneHandMelee` `:1685`, `PickOffHandWeapon` `:1695`, `EquipLeftHeld` `:1748`, `EquipWeapon` `:1782`, the `g_forcedWeapon` ledger + `ReconcileForcedWeapon`/`CoLoadForcedWeapons`) into an `Actuation_Equip.cpp` next to `Actuation_Direct.cpp`/`Actuation_Hands.cpp`, shared state through `Actuation_internal.h`, as the two earlier splits did. Full adversarial Fable review, one field cycle on 1.6.1170 before the next feature lands on it.

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

### MFO-B47 — the idle-hand floor's unobserved gate is a FOURTH consumer of the contested 4000 ms constant
- **Raised:** Fable tier-B review of `ed3d9d0` (`fix/mfo-combat-restoration-direct`), SEV-3 (note). Ids `MFO-B47`-`MFO-B49` continue from `main`'s highest, `MFO-B46`.
- **Severity:** SEV-3 (note; the cycle ended with nothing above SEV-3)
- **Finding (verbatim):** the floor gate is consumer (d) of the 4000 ms constant.
- **Reviewer's reasoning:** `kIdleFloorUnobservedMs` aliases `kHealHoldNeverObservedMs`, which `MFO-B8` already records as answering three differently-sized questions (a)-(c). The gate adds (d): an OFFENSE claim's latency WITH an equip cycle in front of it (heals no longer claim on this branch). The header comment as first written asserted "4000 ms clears every measured latency"; `MFO-B7` records the 0908 heal at 4.5-6.1 s (`Docs/DIAG-2026-09-08-field.md:267`), so it does not, and (d) itself is UNMEASURED.
- **Why it was NOT fixed:** B7's ruling — measure, do not resize from n=2. The comment was corrected in the closing round to cite B7/B8 and the 4.5-6.1 s datum and to say (d) is unmeasured; the constant is unchanged. Cost of the gate firing early is the pre-F10 state (the other hand opens to the AI while a real cast still charges), never a freeze.
- **Fix shape when drained (verbatim):** read the next Deck log's `[cfc]`/castobs fire timestamps against the `IDLE-HAND FLOOR released -- driving claim has no observed cast` lines; size (d) from that, as its own named constant under B8's scheme.

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
- **Finding (verbatim):** `AtkClearSlot` from ResetAllState can race one in-flight ProcessEvent (fid cleared first, counters after) leaving <=1 stale count inherited by the next occupant's fight#1 (cosmetic, no fix).
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

## DRAINED

### MFO-B4 — `IncumbentTargetLost` and `PickAlly` disagree on "resolves"
- **Raised:** Fable review of `1044816`, taken as a comment by `625f3b7`; the review of `625f3b7` asked for a reword once the heartbeat cap exists.
- **Severity:** SEV-5
- **Finding:** `PickAlly`'s "resolves" means "in `Followers::g_active`, or the player"; `IncumbentTargetLost` resolves the raw FormID via `LookupByID`. A mid-fight DISMISSED follower therefore reads valid until healed above threshold.
- **Author's reasoning for leaving the DIVERGENCE:** cost is one claim's life against an ally still standing and still hurt; reaching `g_active` would bind the cast lock to the roster list for a case the field has never reported. Unchanged — the divergence itself is still open by choice, and if it ever bites it comes back as a new entry.
- **Follow-up that was owed:** the comment's stated bound ("a re-aim held for at most one claim's life") had to be reworded once the heartbeat cap landed, because that cap is what makes the real bound something other than the claim's life.
- **DRAINED by `6bfbd79`** (the same commit that landed the cap): the comment now states the actual bound — the re-aim is held while the claim is fed, and the hold's own heartbeat stops at `kHealHoldNeverObservedMs` for a claim that never fires, so the cost is bounded by that window rather than by the fight. Verified in the round-6 review of that SHA.

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
