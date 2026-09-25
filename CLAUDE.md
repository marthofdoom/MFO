# CLAUDE.md — MFO (marth's Follower Overhaul), SKSE C++ plugin

**Consult `MAP.md` first.** It is the architecture + change-impact map: per-
subsystem responsibility, key symbols at `file:line`, and — the point —
"Depended-on-by / What breaks if you change this." A bare symbol read will cause
regressions here; the ripple notes are why the map exists.

## Working rules

- **Navigate by `file:line` from MAP.md.** Grep to a symbol, read a narrow
  window — do NOT read whole files. Large files must never linger in context.
- **SOURCE FILE SIZE AND SPLITS (marth 2026-09-24: "Go with the subsystem folders,
  tool first." Supersedes the bare 2500-line rule of 2026-08-31, history below).**
  - **Subsystem folders, split by concern.** Code lives in `native/<subsystem>/`
    (`cast/`, `progression/`, `apmf/` so far; `loot/`, `board/` ... as later waves
    move them), each with ONE small public header (the only thing another
    subsystem may include, e.g. `cast/Actuation.h`), ONE internal header for the
    state its files share (`<Name>_internal.h`), and cohesive one-concern `.cpp`
    files.
  - **A new mechanism is a new file in its subsystem, by default** (write it into
    the brief), unless it is a small change to an existing concern.
  - **~1500 lines = plan a split.** The next brief that touches a file past ~1500
    proposes the split; it is done as ITS OWN round (a split brief), never inside
    an unrelated task (scope rule 1 below still holds).
  - **2500 lines = hard backstop** only. Crossing it inside another task is a
    STOP-and-report (scope rule 1).
  - **Every split is proven, not asserted.** `tools/splitcheck` (README there)
    compares main's CI build with the split branch's FUNCTION BY FUNCTION (PDB
    symbols, bytes compared with relocations resolved to symbols) and
    `tools/splitcheck/linecheck.py` proves the source lines are the same multiset
    and in the same order. Paste both results into the report. A split is still
    tier A for review. **marth 2026-09-24: "pass, explained can only pass if the
    self test and logic tests on that changed area still result in the same
    results as pre change."** So a function the tool can only EXPLAIN (it differs
    in the shipped /O2 build) passes only with BEHAVIOURAL equivalence: (1) the
    no-optimizer proof build (`native` workflow dispatch `noopt=true`, main and
    the branch) compares `splitcheck --strict` PASS, fed to `--proof`; (2) any
    co-save function in the explained set also passes an emulated save
    round-trip over real co-saves (identical written bytes and call sequence in
    both DLLs); (3) `tools/splitcheck/selftest.py` passes on the pair (the tool
    still catches every planted defect). "CI-identical" remains an invalid claim
    for a split: CI proves it compiles.
  - History: the 2026-08-31 split pass modularized the top four giants into
    ≤2500-line modules (Logistics [core + Loot/Economy/Cast], Board [shell +
    Progression tab], Actuation [dispatch + Direct + Hands], ProgAllocator
    [engine+co-save + Hms + Manifest]); Packages/CasterConsent deferred. Wave 1 of
    the folder layout (2026-09-24) moved Actuation to `cast/`, ProgAllocator to
    `progression/` (the PRGN co-save block stays whole in
    `progression/Allocator.cpp`) and APMFBridge to `apmf/`. Keep MAP.md's
    file:line nav current.
- **Delegate bulk file-reads to a subagent** and keep only its conclusion, so
  large files never sit in the main context.
- **AGENTS KEEP DISK LOGS (marth 2026-09-15).** Token-heavy work is protected against
  token loss: every agent brief includes "create `<scratchpad>/agentlogs/<task>.md` at the
  start and append after every meaningful step (values verified, decisions + why, files
  written file:line, what remains, branch/commit state); code agents commit + push WIP at
  each coherent checkpoint; if resumed after a limit, read the log first." A replacement
  for a died agent is pointed at that log, never re-briefed from zero.
- **Control the token/agent burn (marth 2026-09-02).** ONE complete brief per
  agent, run to completion — never drip-feed sequential refinements (each resume
  re-orients it into a fresh 100+-tool pass; resume only to fix a real error, not
  to add scope). Do small greps/reads/edits/ssh-checks INLINE; spawn an agent only
  for genuinely bulk work. Never resume an agent that self-spins on follow-ups it
  invented (background notifications are never user input); TaskStop a looping one.
  No redundant/overlapping agents; reuse via SendMessage over respawn; one agent
  per build tree. **MODEL POLICY (marth 2026-09-06 — SUPERSEDES the older "cheap
  workers" rule, which said Sonnet should do the build grind):**
  - **AN OPUS AGENT WRITES THE CODE — INCLUDING SMALL CHANGES.** Not Sonnet, and NOT
    the coordinator itself. marth set the threshold LOW on purpose: "by reasonably sized
    I mean smaller. but we cant afford the sloppy work weve been getting from teh cheap
    agents." The driver is QUALITY, not token size. If in doubt, it is an Opus agent's job.
  - **Cheap models (Sonnet/Haiku) are NOT for authoring code at all now.** Reserve them
    for non-authoring mechanical grinds — log/artifact sweeps, bulk greps, collating
    output — and even there, check their work. Their sloppiness is what cost us the
    invented-symbol CI failures and misapplied fixes.
  - **OPUS 5.5 reviews EVERY commit's diff** (see dispatcher rule 8), plus deep research
    and risky co-save/threading work.
  - **The coordinator does NOT author reasonably sized additions itself.** It dispatches,
    does small greps/reads/ssh-checks inline, READS diffs, and DIRECTS corrections back
    to the worker that holds the file context. Hand-editing is for context-free
    one-liners only. Lean on memory summaries + file:line nav, not raw
  crashlogs/reports/log-dumps in context. Report at real milestones, not every
  background ping.
- **Never read vendored code:** `native/imgui_impl_win32.*` (the only vendored
  file in `native/`), plus any `build/`, `.git/`. ImGui comes via vcpkg.
- **BEFORE editing a subsystem:** re-read its MAP.md "What breaks" entry and
  re-verify it against current code (line numbers drift). **If the structure
  moved, update MAP.md as part of the change.** A stale map misleads the next
  session.
- Also consult `Docs/INVARIANTS.md` (**95 rules: 81 numbered `#1`–`#81` plus 14
  lettered**) before non-trivial changes; MAP.md cites it as `#N`.
  **`Docs/ARCHITECTURE.md` is HISTORICAL** (banner-marked 2026-09-07; it describes
  a pre-implementation design and contradicts shipped code on hooks, records,
  threads and Papyrus) — read `MAP.md` for the real architecture.
- **CITATION NAMESPACE (2026-09-07): `#N` is an INVARIANT; `T#N` is a TASK/issue
  number.** They used to share one namespace and five task numbers collided with
  real rules (`T#67` pointed at a REVOKED invariant, `T#75` at the xEdit
  subrecord-order rule). See `Docs/INVARIANTS.md` "CITATION NAMESPACE" for the
  convention and for the pre-2026-09-07 residue that is still bare.

> **REVIEWER MODEL (marth 2026-09-23): every review, spot check and field diagnosis in this file is done by an
> OPUS 5.5 agent (the Agent tool's `opus` model), replacing Fable.** Historical statements below about what
> Fable found are left as they were, because they are records of what happened. marth's own quoted words
> are left verbatim too. The standing review model is still the cheap one: the author names its 1-5
> uncertain spots and the reviewer answers only those, unless the change is co-save / ABI / a new engine
> seat / a TU split.

## SCOPE DISCIPLINE — the git system only catches regressions if nobody skips it

**Every regression this project has shipped recently came from UNREQUESTED SCOPE reaching the deck through
a review nobody actually performed.** These are hard rules, for the agent doing the work AND for whoever
dispatches and merges it.

### For the worker
1. **DO EXACTLY WHAT THE BRIEF ASKS. NOTHING ELSE.** No refactors, no file splits, no moving code between
   files, no renames, no "while I was in there" cleanups, no new files — unless the brief asks for them by
   name. If the work seems to *need* one, **STOP and report it**; do not do it and mention it afterwards.
   **This OVERRIDES the file-size rule above (~1500 plan / 2500 backstop): crossing either is a
   STOP-and-report, NOT a licence to split inside an unrelated task.** A split is its own brief and its own field cycle, because a "pure
   mechanical, CI-identical" move is exactly the change whose breakage only shows up in the field — and
   "CI-identical" is not a claim any TU split may assert, since CI proves it compiles, not that it behaves.
   (What actually happened, 2026-09-06 — CORRECTED 2026-09-06 after a transcript audit, because the first
   version of this note was WRONG in three ways and a rules file teaching a false lesson is worse than no
   rule. A loot task hit the 2500 cap, split 560 lines into a new file, and SAID SO in its commit message:
   it was obeying the rule as written. The coordinator then **DID run `git diff --stat` and DID see
   `Logistics_Loot_Equipment.cpp | 461 +++++`** — and wrote that very filename into the tag message sixteen
   seconds later. It still shipped. **The gate was not skipped; it RAN IN A MODE WHERE IT COULD NOT FIRE**,
   because the diffstat was scanned for the things the reviewer already expected (`interrupt|loose|quest`)
   rather than for ANOMALIES. And the split did NOT break anything: a later line-by-line audit found the
   contained-item eligibility path **byte-for-byte unchanged**. **THE LESSON IS NOT "read the diffstat" —
   it is "read it for what you did NOT expect."** A review that only confirms the story you arrived with is
   not a review. Grep it for `^[ADRC]` and for files outside the brief's stated scope, and paste that output
   verbatim before merging, tagging or deploying.)
2. **NEVER GUESS AN API OR A SYMBOL.** Verify it against the real CommonLibSSE-NG header/source or the
   disassembly before using it. "It compiles in my head" is not verification. (Real cost: two CI failures
   in one night on invented symbols — `ExtraDataList::HasQuestObjectAlias`, `EffectSetting::Data::Flag::kHostile`.)
3. **NEVER REPORT SUCCESS WITHOUT A VERIFIED-GREEN CI RUN.** Re-read the NEWEST run id, confirm its
   `headSha` is YOUR commit, and accept only `success`/`failure` (`cancelled`/`pending`/`queued` mean keep
   waiting — the workflow sets `cancel-in-progress`, so a newer run cancels older ones). A green run on
   the wrong SHA proves nothing about your code.
4. **DO NOT CREATE BRANCHES, TAGS, RELEASES OR INTEGRATIONS YOU WERE NOT ASKED FOR.** Push your own branch;
   that is all.
5. **STAY INSIDE YOUR FILE BOUNDARY.** If the brief lists files you own, touching anything else — even
   something obviously related — must be reported, not assumed.

### For whoever dispatches and merges
6. **READ THE ACTUAL DIFF BEFORE MERGING OR TAGGING. A SUMMARY IS NOT A REVIEW.** Run `git diff --stat`
   first and treat **any new file, deleted file, or large move as a STOP** until it is explained and
   justified. A branch whose diffstat does not match its brief has not been reviewed just because its CI
   is green — CI proves it compiles, not that it does what was asked.
7. **Never deploy a branch you have not personally diffed.** CI-green plus a plausible agent summary is
   exactly how an unreviewed refactor reaches the field.
8. **EVERY COMMIT GETS AN OPUS 5.5 DIFF REVIEW (marth 2026-09-06). No exceptions.** Not just pre-cut, not just
   risky ones — each commit, as it lands. CI-green is not a review and the coordinator's own read is not a
   substitute: dispatch an Opus 5.5 diff review of that SHA, give it the brief the commit was written against so
   it can catch unrequested scope, and tell it to be adversarial and report via ReportFindings. Review the
   BRANCH's files (`git show <branch>:<path>`), never the main working copy. Nothing merges, tags or deploys
   on a commit whose Opus 5.5 review has not come back and been acted on.
   (Why: every regression this project shipped recently reached the deck through a review nobody performed.
   The 2026-09-06 deny/heal failure was a reviewed, CI-green, deliberate change that was simply the wrong
   trade — exactly what a summary-level review cannot catch. Supersedes "pre-cut = one focused review".)

9. **REVIEW CYCLES MUST CONVERGE — SEVERITY FLOOR + A DEFERRED BACKLOG (marth 2026-09-08).** A review round
   that returns **nothing above SEV-3 ENDS the cycle.** Remaining SEV-4/SEV-5 findings are DEFERRED, never
   dropped: recorded in `Docs/REVIEW-BACKLOG.md` with the finding's VERBATIM text, its severity, the SHA it
   was raised against, and the reviewer's reasoning. Anything above SEV-3 keeps the cycle running as before.
   **Drain the backlog in ONE batch** at a natural boundary — before a release cut, or before a field cycle
   touching that subsystem — as a single agent round with a single Opus 5.5 review.
   **CARVE-OUTS that are ALWAYS fixed immediately, whatever their nominal severity:** (a) co-save layout,
   threading, or ABI / byte-shared-header findings — a SEV-4 threading finding is a SEV-4 right up until the
   day it is not, and `g_forcedWeapon`'s unguarded read was graded with "no live race today" while being the
   exact shape of the 2026-08-17 SEV-1; (b) anything the NEXT field cycle will exercise, because a deferred
   finding that corrupts the next test costs a whole deploy cycle to rediscover.
   **The backlog must be cross-referenced from MAP.md's "What breaks" entry** for each affected subsystem, so
   the next agent to touch that code reads the open findings BEFORE it writes. A deferred finding that is not
   surfaced at edit time comes back as a higher-severity one.
   (Why: the cost is per-ROUND, not per-finding. An author round plus its review costs ~250-400k tokens
   whether it fixes one finding or eight, so chasing a lone SEV-5 costs the same as draining the whole
   backlog. Measured 2026-09-08: one branch consumed six author/review rounds — ~1.8M subagent tokens across
   the session — with severities converging SEV-2 -> SEV-3 -> SEV-4/5 while every round still paid full price.
   This rule does NOT weaken rule 8 or the fix-everything rule: nothing is dropped, and a SEV-1/SEV-2 still
   blocks the merge.)
10. **SCALE REVIEW DEPTH TO THE DIFF'S SHAPE — the review still happens, its DEPTH varies (marth 2026-09-08).**
   Rule 8 stands: every commit gets a review. But match the instrument to the change.
   - **Tier 1 — doc/comment-only.** Prove it mechanically instead of dispatching: strip `//` comments from
     every touched source file on both sides of the commit and diff; they MUST be identical. Any code delta
     at all escalates to tier 2 or 3. (A comment-only commit consumed an 85k adversarial review on
     2026-09-08 to conclude what that strip-and-diff proves in seconds — which is exactly the check the
     reviewer itself ran first.)
   - **Tier 2 — localized change inside an existing mechanism.** Focused Opus 5.5 review, carrying the
     cleared-ledger of what earlier rounds already proved so it does not re-derive settled ground.
   - **Tier 3 — NO DOWNGRADE EVER.** New mechanism, threading, co-save, ABI or byte-shared headers, engine
     seats/hooks, or a TU split. Full adversarial Opus 5.5 review. A TU split is tier 3 no matter how mechanical
     it looks, because "CI-identical" is not a claim a split may assert.
   Never downgrade the REVIEWER to a cheap model at any tier. Fable found every defect that mattered this
   week; cheap review is how the 2026-09-06 regression shipped.
   **COST TABLE (marth 2026-09-15: "we cant have every small change being this expensive … higher
   risk changes get higher effort checks with better agents"). Tier is decided by the DIFF'S SHAPE:**
   | Tier | Shape | Author | Review | Rounds |
   |---|---|---|---|---|
   | A | engine seat / ABI / byte-shared header / co-save / threading / TU split | Opus | Opus 5.5 tier 3 | until nothing >SEV-3 |
   | B | logic inside an existing mechanism (most feature work) | Opus | Opus 5.5 ONE pass; author fixes; coordinator merges on the author's report + its own diff read | ≤2 |
   | C | docs / strings / log text / comments / backlog entries / small INI | Opus small brief, or the coordinator inline for one-liners | mechanical (comment-strip diff) | 1 |
   Two rules that cut the most waste: (1) a CLOSING round (strings, wording, backlog notes) never gets an
   Opus 5.5 pass — prove it by comment-strip diff or the coordinator's read; (2) SEV-4/SEV-5 findings go to
   the backlog, never into the current round "because the author is there" — one drain before the cut
   (rule 9). Measured 2026-09-15: the equip authority took ~12 agent rounds / ~3M tokens across two repos,
   and most rounds after the first two were tier-C cleanup reviewed at tier A. Tier A stays expensive on
   purpose: every real defect this week (the Conduit loop, the null left slot, the AI shield re-equip, the
   uninstalled-seat claim) was tier A and Fable found all of them.

## Engineering + design principles — HOW TO THINK HERE (read before designing anything)

These are hard-won, each one paid for with a crash, a wasted deploy cycle, or a
month-long bug. They apply to EVERY worker on this codebase, not just the author
of a given change.

1. **THINK WITH PORTALS (marth).** An engine STATE is a CONTAINER OF FACETS, never a
   monolith and never a cost to accept. When machinery you need only exists inside a
   state you don't want (e.g. the `CombatMagicCaster` set only exists with a live
   `CombatController`), do NOT avoid the state and do NOT work around it: force the
   state as a pure SUBSTRATE, open a portal for the ONE facet you came for, and DENY
   everything else it switches on. Combat becomes a substrate, not a mode. This
   generalizes: any "the engine only does X while in state Y" problem has this shape.
2. **DENY-COMPLETENESS LICENSES STATE-FORCING.** For EVERY facet a client can claim,
   there must be a COMPLETE deny — the competing source reduced to ZERO influence.
   That invariant is what makes principle 1 legitimate instead of a blunt hack: if
   nothing unrequested can get through, entering any state is safe. So when reviewing
   a design, NEVER ask "which side effects are acceptable?" — ask **"for each facet
   this switches on, do we have a complete deny, and WHERE ARE THE HOLES?"**
3. **COMPOSITION, NOT SUBSTITUTION.** Never swap in a whole package; a substituted
   package makes the preempted source lose its slot, fire OnPackageEnd and tear down
   (that is why substitution FREEZES the body). Moderate PER FACET. The NPC ends up
   running its own non-denied facets plus what we let through, while its real package
   keeps ticking.
4. **DECLARE → ENFORCE.** The client declares WHAT and WHERE; the framework enforces
   the ordered composition. Enforce ONLY what was declared — never fabricate un-given
   input. "Manufacturing the ordered composition" is allowed; inventing intent is not.
5. **DISASSEMBLY PROVES A PATH EXISTS, NOT THAT IT RUNS.** Before building on a code
   path, OBSERVE IT EXECUTING (a passive probe, a log line, a field capture). Cost of
   ignoring this: five engine seats were implemented, reviewed, CI-green and deployed
   onto `CombatMagicCasterRestore` — a caster the engine builds so rarely it is useless
   as a seat site: **3 constructions against 120 Offensive in one 8-minute session —
   2.5% of the Offensive count** — and 0 in the two sessions before that, because the
   engine built none at all until APMF's seat-0 classify keyed the spell into the
   Restore row. Everything was correct except the assumption. (Figures + seat configurations: `Docs/ENGINE_NOTES.md` §0.41.
   Do NOT quote the 0-Restore runs as current, and do not say "never" — that older
   phrasing was this file's, and §0.41 now forbids it.)
6. **COMMONLIB DECLARATIONS ARE NOT ABI-TRUSTWORTHY.** Verify every vfunc signature
   against the DISASSEMBLED target binary, never against CommonLib's header. Cost of
   ignoring this: a wrong `GetMagicTarget` signature (a hidden sret out-slot CommonLib
   omits) made a "passive" observation probe crash the game.
7. **NEVER MASK A FAILURE.** No watchdog, retry-fallback or safety net that makes a
   broken mechanism LOOK like it worked. An unmasked failure diagnoses in ONE field
   cycle; a masked one hides indefinitely and costs many. Log the failure loudly and
   let it fail. (Distinct from a legitimate DEGRADE path chosen by design — e.g.
   "framework absent → legacy path" — which is a documented contract, not a mask.)
8. **LOG VOLUME IS NOT IMPORTANCE.** A failing path that retries is loud by
   construction. Rank by what the user actually needs, not by line count.
9. **A FLOOR IS SAFE; AN EXPIRY IS NOT.** Size every budget/TTL from the REAL refresh
   cadence, not from a guess. A round-robin tick means per-item refresh is
   `N x period` — a flat expiry shorter than that silently kills LIVE state.
11. **PER-RUNTIME PATHS, NOT A LOWEST-COMMON-DENOMINATOR COMPROMISE (marth 2026-09-09).**
    Both binaries are unpacked now (1.6.1170 and 1.5.97), so version fragility is no
    longer a reason to weaken a mechanism. **Whatever is proven on 1.6 is what ships on
    1.6.** 1.5 gets its OWN separate path, behaviourally identical, with every offset and
    id verified against the 1.5 LAYOUT. Never degrade the proven path to make one
    construct span both runtimes, and never assume a construct valid on one layout is
    valid on the other — the layouts genuinely differ (17 vs 18 input contexts shifts
    every `ControlMap` member past `controlMap[]`).
    (Cost of the opposite, paid across three releases: `5c0957c` replaced three WORKING
    call-site trampolines with a `BSTEventSink` + `ControlMap` toggle so a single path
    could span every runtime. That compromise produced a session-ending crash (the 3.7.0
    header wrote `contextPriorityStack.size` instead of `enabledControls`), then a
    control-mask hole that let Favorites open over the board, then a dead d-pad and B
    button. The mechanism it replaced had worked for months.)

10. **PROPER SOLUTIONS, NOT WORKAROUNDS.** Never work around unless absolutely
    needed; solve the root cause. If a compromise is genuinely unavoidable, FLAG it
    explicitly and record why — never bury it.

## The five things that corrupt saves or crash — verify before touching

1. **Co-save layout** (`Serialization.cpp`, `ProgAllocator::CoSaveSave/Load`,
   `State.h`, `Vocabulary.h`): 4 records FLWR v5 / MSTK v1 / PRGN v8 / FWPN v1
   (`Serialization.h`). Changing a
   field order/type/count, bumping a version without a matching `if(version>=N)`
   reader, renaming a serialized opcode string, or renumbering the `Subject` /
   `Stance` / `combatClassOverride` enums corrupts live saves. Keep readers for
   every shipped version forever (#12).
2. **`ResetAllState` order** (`Serialization.cpp:680`, `StopPump()` at `:686`): `StopPump()` first, then
   clears. Reordering, or mutating save-scoped maps off the drained worker, is UB.
3. **Alias fills** (`Packages.cpp`): persist into the `.ess`. Never skip/reorder
   `ReleaseAll` (kPreLoadGame / post-load / revert). Evict marker must stay a
   non-actor XMarker (base `0x3B`) or furniture-eject re-breaks. `EvaluatePackage`
   `resetAI` must stay `false`.
4. **Combat vfunc hooks** (`Targeting`/`CasterConsent`/`CombatStyle`): install-once
   at `plugin.cpp:299-301`, VR-refused, run on the combat thread. Any
   `CombatController` member touched there must be `< 0x68` (AE +8 layout bug).
5. **Frozen external contracts:** `Forms.h` FormIDs (↔ `MFO_GenerateESP.py`);
   `MEO_API.h` (byte-shared with a separate MEO.dll, append-only); `APMF_API.h`
   (byte-shared with a separate APMF.dll, append-only — mirror APMF's copy exactly;
   consumed via `APMFBridge`); TradeBridge's
   10 Papyrus natives + Papyrus.cpp's 3 method-name strings (called by shipped
   `.pex`); Config INI key names (MCM-Helper persistence identity).

## Threading (one wrong access = a data race)

- `g_followers` / `Followers::g_active` / `g_activeIds` are **main-thread /
  serial-SKSE-task only, no lock** (#4). Off-thread → snapshot, never lock.
- The per-follower tick runs on the **AddTask job worker** (driven by the sleeper
  thread in `Diagnostics.cpp`, `kPumpMs=133`). `MainThread::Post` is the *only*
  road to the true main thread (for 3D/cell mutation, physics queries).
- Combat-thread hooks read FormIDs / atomic mirrors, never the follower lists.

## LOCAL CommonLibSSE SOURCE — verify symbols here, do NOT fetch upstream

Two checkouts exist on this machine. **They are not interchangeable. Using the wrong one
re-creates the exact invented-symbol CI failures the "NEVER GUESS AN API OR A SYMBOL" rule exists
to prevent** — a symbol can be real in one tree and absent in the other.

- **`/mnt/gaming/modlists/Projects/_commonlib/mit-3.7-fork/` at the `mit-3.7` tip**  ← **AUTHORITATIVE (F1, 2026-09-24). VERIFY HERE.**
  CI builds CommonLib from `marthofdoom/CommonLibSSE-NG` branch **`mit-3.7`**, served by `marthofdoom/vcpkg-registry`
  (port `commonlibsse-ng` 3.7.0#N; `native/vcpkg-configuration.json` pins the registry baseline, which pins the
  fork SHA in the portfile). Local clones: `_commonlib/mit-3.7-fork/` and `_commonlib/vcpkg-registry/`. **The fork
  has diverged from the pinned tree (F1)**, so verify against the fork at the SHA your registry baseline names,
  never against the pinned tree for anything F1 touched: an Address Library id miss is now FATAL (not the next
  id), `BGSDefaultObjectManager` / `ControlMap` / `CombatController` members past the version-dependent point
  are behind `GetRuntimeData()` / exact-build accessors, `CombatMagicCaster::GetMagicTarget` returns
  `MagicTarget` by value (the engine's hidden out-slot), `Actor::StartCombat` / `SendInventoryUpdateMessage` /
  `ExtraDataList()` exist, and `REL::Module::IsExactly`, `SKSE::RUNTIME_SSE_1_6_1170` and `REL::SelfCheck` are new
  (fork README "What changed from 3.7.0"). If it is not in the fork at that SHA, it does not exist for our build.
- **`/mnt/gaming/modlists/Projects/_commonlib/pinned-3.7.0-c4ab853d/`**  ← upstream 3.7.0, for DIFFING only.
  `CharmedBaryon/CommonLibSSE-NG` @ `c4ab853d095e81e3390b282d7ba01ab2f24ebf25`, the fork's root. Everything F1 did
  not touch is still identical to it; everything F1 touched differs, so it is no longer a verify source.
- **SEAT SELF-CHECK (F1):** every engine address this DLL hooks, calls or classifies with through its own id is a
  row in `native/VerifiedAddresses.h`, generated by `tools/verified_addresses/gen_verified_addresses.py` from OUR
  unpacked executables (never from Address Library files alone) and documented in `Docs/VERIFIED-ADDRESSES.md`.
  A new hook seat or id-based call needs a new spec row, a regenerated header, and a `SeatVerified()` guard at
  its install / call site. Refusing a seat is the only allowed reaction to a failed row.
- **`/mnt/gaming/modlists/Projects/_commonlib/live-alandtse-ng/`**  ← reference only, DO NOT verify against.
  `alandtse/CommonLibSSE-NG` branch `ng`, currently **v7.2.0** — the actively maintained fork (the pinned
  CharmedBaryon repo has not been pushed to since 2024-09-04). Useful for seeing how upstream solved
  something, or for planning a migration. It has bindings 3.7.0 does NOT (e.g. more `StartCombat` /
  `HasQuestObject` overloads), so verifying against it will produce code that fails CI.

Refresh the pinned tree with `git -C <dir> fetch --depth 1` if ever needed; it must stay at that exact SHA.
**Still verify against the DISASSEMBLED binary, not the header, for anything ABI-shaped** — CommonLib
declarations are not ABI-trustworthy (a wrong `GetMagicTarget` signature with a hidden sret out-slot once
made a "passive" probe crash the game).

**DECIDED (marth 2026-09-23): no migration to the alandtse fork** (7.x is GPL-3.0-or-later, our mods are MIT).
We own an MIT 3.7.0 line instead: plan in `_commonlib/fork-plan-2026-09-23/own-addrlib-design.md`
(F0 fork + registry, byte-identical; F1 corrections + self-check; F2 1.7.104 clean-room). Never copy code
from `live-alandtse-ng/` into the fork. Changing the ABI source under a plugin doing vtable and offset work
breaks in the FIELD, not in CI, so each fork stage is its own scoped brief with its own Opus 5.5 review.

## FIELD DIAGNOSIS + AGENT REUSE (marth 2026-09-06)

**4. WHEN AN IN-GAME TEST DOES NOT DO WHAT WE EXPECT, IT GOES STRAIGHT TO OPUS 5.5.** marth: *"when a test in
game doesnt do what we expect, immediately goes to fable."* The split is strict:
- **The coordinator GATHERS.** Pull the deck logs; verify the deployed DLL sha against the branch that
  produced them; check the INI switch states; build a consolidated EVIDENCE file (tag histograms, message
  shapes, per-ACTOR and per-HAND attribution, timestamps). This works and makes Opus 5.5 fast.
- **OPUS 5.5 CONCLUDES.** Do NOT arrive with a root-cause theory. Hand over the evidence and the brief.
Measured 2026-09-06: from one 8-minute deck log the coordinator produced FOUR confident root causes and
Fable overturned ALL FOUR using that same evidence file (a "deny hole" that was by-design chaining; a spell
attributed to the follower that four Chaurus Reapers were casting; a "stale" proxy that was actually read
before it was minted; a config problem that was a code problem). Also pass Opus 5.5 any METHOD constraint marth
has already given — e.g. do NOT argue from loot arrival counts or travel/arrival ratios; a follower walking
past loot during an ordinary follow produces arrival-shaped lines that prove nothing.

**5. REVIEW FINDINGS GO BACK TO THE AGENT THAT WROTE THE PATCH, via SendMessage — never a fresh agent.**
marth: *"If the same patch has more issues it would make sense to reuse the same agent for the iterations."*
The author already holds the file context, the symbol verifications and the reasoning; a fresh agent
re-orients from zero over the same files (measured cost of getting this wrong: 173k / 224k / 108k subagent
tokens re-deriving what the author already knew). This IS the sanctioned resume case under the anti-drip-feed
rule above — applying review findings is "fixing a real error", not adding scope. A resumed agent does NOT
re-read its original brief, so the message must still carry the hard rules (verify symbols, CI-green on your
own SHA, no merge/tag/force-push, the file boundary). Spawn a NEW agent only for a genuinely different patch,
a different repo/tree, or work the author was never briefed on — and never two agents in one build tree.
