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


---

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

| `<SHA>` | `fix/mfo-heal-recognition` — the F12 in-flight heal hold in `ComposedCast::Try` + the combat-AI gate on `CastTargetDirect`'s claim + `CastInFlightOnHand`/`kHandLeft` promoted to `Actuation.h` | v2.0.5 | **HIGH — tier 3.** Changes WHO wins the heal claim slot and adds a hold with a new bound, and moves a predicate out of an anonymous namespace into a public header. marth ordered it built with NO review under the token limit. The three uncertainties the author flagged are in the branch report: (1) an OBSERVED, continuously channelling incumbent is now displaceable only after `kInFlightHoldCap` (6 s) rather than immediately — bounded but a real behaviour change; (2) `CastSelfDirect` (`Actuation_Direct.cpp:886`) and the OOC offense-concentration `ClaimOffenseCast` (`:1216`) have the IDENTICAL undeliverable-claim exposure and were deliberately left alone as outside the brief; (3) a heal claim left standing when combat ends is not released by the gate, so `Logistics.cpp`'s `IsHealCastActive` label can read "APMF claimed" for up to one 6 s TTL while the direct force is what actually delivered. |

Merges `6d8074d` and the docs-only `b8087a9` (CLAUDE.md rule 11) carry no
unreviewed logic.

**Note for whoever reviews `0b8c816` / `6d3e9c5`:** both were coordinator-reasoned
fixes for a symptom neither one fixed (the real cause was that a `BSTEventSink`
cannot consume, which `49c9d1b` addresses). They are not known-wrong, but they are
unproven and were kept only because they are independently defensible. If a review
finds either is a no-op or harmful, reverting is fine.
