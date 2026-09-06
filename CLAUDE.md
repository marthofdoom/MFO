# CLAUDE.md — MFO (marth's Follower Overhaul), SKSE C++ plugin

**Consult `MAP.md` first.** It is the architecture + change-impact map: per-
subsystem responsibility, key symbols at `file:line`, and — the point —
"Depended-on-by / What breaks if you change this." A bare symbol read will cause
regressions here; the ripple notes are why the map exists.

## Working rules

- **Navigate by `file:line` from MAP.md.** Grep to a symbol, read a narrow
  window — do NOT read whole files. Large files must never linger in context.
- **HARD RULE (marth 2026-08-31): NEVER let a source file exceed 2500 lines.**
  Split it into focused modules before it crosses (an internal header for shared
  file-local state + cohesive modules; a pure mechanical move, CI-identical).
  The split pass is COMPLETE: the top four giants are modularized (Logistics
  [core + Loot/Economy/Cast], Board [shell + Progression tab], Actuation
  [dispatch + Direct], ProgAllocator [DONE: engine+co-save + Hms + Manifest —
  the PRGN co-save block stays whole in ProgAllocator.cpp]) into ≤2500-line
  modules; Packages/CasterConsent deferred. Keep MAP.md's file:line nav current.
- **Delegate bulk file-reads to a subagent** and keep only its conclusion, so
  large files never sit in the main context.
- **Control the token/agent burn (marth 2026-09-02).** ONE complete brief per
  agent, run to completion — never drip-feed sequential refinements (each resume
  re-orients it into a fresh 100+-tool pass; resume only to fix a real error, not
  to add scope). Do small greps/reads/edits/ssh-checks INLINE; spawn an agent only
  for genuinely bulk work. Never resume an agent that self-spins on follow-ups it
  invented (background notifications are never user input); TaskStop a looping one.
  No redundant/overlapping agents; reuse via SendMessage over respawn; one agent
  per build tree. CHEAP model for WORKERS (Sonnet; Haiku for trivial) — the bulk
  build/audit grind must NOT be Opus; reserve Opus/Fable for diff-REVIEWS, deep
  research, and risky co-save/threading. Cheap = bulk-writing AND applying corrections
  (it holds the file context); expensive = READING diffs + DIRECTING fixes back to the
  cheap worker (which applies them in-context). Authoring a fix that needs surrounding
  code means loading whole files into the expensive window — avoid: review the diff,
  hand corrections back; Opus hand-edits directly only for context-free one-liners. Lean on memory summaries + file:line nav, not raw
  crashlogs/reports/log-dumps in context. Report at real milestones, not every
  background ping.
- **Never read vendored code:** `native/imgui_impl_win32.*` (the only vendored
  file in `native/`), plus any `build/`, `.git/`. ImGui comes via vcpkg.
- **BEFORE editing a subsystem:** re-read its MAP.md "What breaks" entry and
  re-verify it against current code (line numbers drift). **If the structure
  moved, update MAP.md as part of the change.** A stale map misleads the next
  session.
- Also consult `Docs/INVARIANTS.md` (49 numbered rules) and `Docs/ARCHITECTURE.md`
  before non-trivial changes; MAP.md cites both as `#N` / §N.

## SCOPE DISCIPLINE — the git system only catches regressions if nobody skips it

**Every regression this project has shipped recently came from UNREQUESTED SCOPE reaching the deck through
a review nobody actually performed.** These are hard rules, for the agent doing the work AND for whoever
dispatches and merges it.

### For the worker
1. **DO EXACTLY WHAT THE BRIEF ASKS. NOTHING ELSE.** No refactors, no file splits, no moving code between
   files, no renames, no "while I was in there" cleanups, no new files — unless the brief asks for them by
   name. If the work seems to *need* one, **STOP and report it**; do not do it and mention it afterwards.
   **This OVERRIDES the 2500-line rule above: crossing 2500 lines is a STOP-and-report, NOT a licence to
   split inside an unrelated task.** A split is its own brief and its own field cycle, because a "pure
   mechanical, CI-identical" move is exactly the change whose breakage only shows up in the field — and
   "CI-identical" is not a claim any TU split may assert, since CI proves it compiles, not that it behaves.
   (What actually happened, 2026-09-06: a loot task hit the 2500 cap, split 560 lines into a new file, and
   SAID SO in its commit message — it was obeying the rule. The split then rode into an integration build
   and onto the deck. **The failure was that nobody read the diffstat, which listed the new file in plain
   sight.** The worker followed the rules as written; the rules and the review were at fault.)
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
   onto `CombatMagicCasterRestore` — a caster the engine never runs (0 occurrences in
   a whole session vs 69 Offensive). Everything was correct except the assumption.
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
10. **PROPER SOLUTIONS, NOT WORKAROUNDS.** Never work around unless absolutely
    needed; solve the root cause. If a compromise is genuinely unavoidable, FLAG it
    explicitly and record why — never bury it.

## The five things that corrupt saves or crash — verify before touching

1. **Co-save layout** (`Serialization.cpp`, `ProgAllocator::CoSaveSave/Load`,
   `State.h`, `Vocabulary.h`): 4 records FLWR v5 / MSTK v1 / PRGN v6 / FWPN v1
   (`Serialization.h`). Changing a
   field order/type/count, bumping a version without a matching `if(version>=N)`
   reader, renaming a serialized opcode string, or renumbering the `Subject` /
   `Stance` / `combatClassOverride` enums corrupts live saves. Keep readers for
   every shipped version forever (#12).
2. **`ResetAllState` order** (`Serialization.cpp:562`): `StopPump()` first, then
   clears. Reordering, or mutating save-scoped maps off the drained worker, is UB.
3. **Alias fills** (`Packages.cpp`): persist into the `.ess`. Never skip/reorder
   `ReleaseAll` (kPreLoadGame / post-load / revert). Evict marker must stay a
   non-actor XMarker (base `0x3B`) or furniture-eject re-breaks. `EvaluatePackage`
   `resetAI` must stay `false`.
4. **Combat vfunc hooks** (`Targeting`/`CasterConsent`/`CombatStyle`): install-once
   at `plugin.cpp:293-295`, VR-refused, run on the combat thread. Any
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
