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
  - **FABLE reviews EVERY commit's diff** (see dispatcher rule 8), plus deep research
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
8. **EVERY COMMIT GETS A FABLE DIFF REVIEW (marth 2026-09-06). No exceptions.** Not just pre-cut, not just
   risky ones — each commit, as it lands. CI-green is not a review and the coordinator's own read is not a
   substitute: dispatch a Fable diff review of that SHA, give it the brief the commit was written against so
   it can catch unrequested scope, and tell it to be adversarial and report via ReportFindings. Review the
   BRANCH's files (`git show <branch>:<path>`), never the main working copy. Nothing merges, tags or deploys
   on a commit whose Fable review has not come back and been acted on.
   (Why: every regression this project shipped recently reached the deck through a review nobody performed.
   The 2026-09-06 deny/heal failure was a reviewed, CI-green, deliberate change that was simply the wrong
   trade — exactly what a summary-level review cannot catch. Supersedes "pre-cut = one focused review".)

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

## LOCAL CommonLibSSE SOURCE — verify symbols here, do NOT fetch upstream

Two checkouts exist on this machine. **They are not interchangeable. Using the wrong one
re-creates the exact invented-symbol CI failures the "NEVER GUESS AN API OR A SYMBOL" rule exists
to prevent** — a symbol can be real in one tree and absent in the other.

- **`/mnt/gaming/modlists/Projects/_commonlib/pinned-3.7.0-c4ab853d/`**  ← **AUTHORITATIVE. VERIFY HERE.**
  `CharmedBaryon/CommonLibSSE-NG` @ `c4ab853d095e81e3390b282d7ba01ab2f24ebf25` = commonlibsse-ng **3.7.0**,
  exactly what the colorglass vcpkg registry pins (`native/vcpkg-configuration.json` baseline
  `6309841a…`) and therefore exactly what CI compiles and links. **Every symbol/vfunc/signature claim
  must be checked against THIS tree.** If it is not here, it does not exist for our build.
- **`/mnt/gaming/modlists/Projects/_commonlib/live-alandtse-ng/`**  ← reference only, DO NOT verify against.
  `alandtse/CommonLibSSE-NG` branch `ng`, currently **v7.2.0** — the actively maintained fork (the pinned
  CharmedBaryon repo has not been pushed to since 2024-09-04). Useful for seeing how upstream solved
  something, or for planning a migration. It has bindings 3.7.0 does NOT (e.g. more `StartCombat` /
  `HasQuestObject` overloads), so verifying against it will produce code that fails CI.

Refresh with `git -C <dir> fetch --depth 1` if ever needed; the pinned tree must stay at that exact SHA.
**Still verify against the DISASSEMBLED binary, not the header, for anything ABI-shaped** — CommonLib
declarations are not ABI-trustworthy (a wrong `GetMagicTarget` signature with a hidden sret out-slot once
made a "passive" probe crash the game).

**Open strategic question, not yet decided:** we are pinned four major versions behind (3.7.0 vs 7.2.0) on
a dormant repo. Migrating to the alandtse fork is its own scoped brief with its own Fable review — changing
the ABI source under a plugin doing vtable and offset work breaks in the FIELD, not in CI. Do not start it
as a side effect of another task.

## FIELD DIAGNOSIS + AGENT REUSE (marth 2026-09-06)

**4. WHEN AN IN-GAME TEST DOES NOT DO WHAT WE EXPECT, IT GOES STRAIGHT TO FABLE.** marth: *"when a test in
game doesnt do what we expect, immediately goes to fable."* The split is strict:
- **The coordinator GATHERS.** Pull the deck logs; verify the deployed DLL sha against the branch that
  produced them; check the INI switch states; build a consolidated EVIDENCE file (tag histograms, message
  shapes, per-ACTOR and per-HAND attribution, timestamps). This works and makes Fable fast.
- **FABLE CONCLUDES.** Do NOT arrive with a root-cause theory. Hand over the evidence and the brief.
Measured 2026-09-06: from one 8-minute deck log the coordinator produced FOUR confident root causes and
Fable overturned ALL FOUR using that same evidence file (a "deny hole" that was by-design chaining; a spell
attributed to the follower that four Chaurus Reapers were casting; a "stale" proxy that was actually read
before it was minted; a config problem that was a code problem). Also pass Fable any METHOD constraint marth
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
