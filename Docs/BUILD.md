# MFO — Build & Working Practice

The family's working agreement, applied to MFO. **This file should have
existed from day one**; its absence is why MFO spent a session violating rules
that were written down in MAO's and MRO's `BUILD.md` all along.

MFO ships in small, individually **CI-green** builds. Each milestone is one
downloadable `MFO.dll` artifact **marth tests in-game before the next is
written**.

---

## The per-build checklist (MAO's, verbatim in intent)

1. Write **one** milestone's change only.
2. **Fable review reports** before the push (#45a — see below).
3. `git push` → watch CI green; fix compile errors and repush.
4. `./release.sh` → install → **run that milestone's slice of the test matrix
   in-game**.
5. **Only then start the next build.**

**Step 5 is the one MFO broke.** v0.1.0 → v0.4.1 were cut on a single in-game
session, so defects accumulated across five builds instead of being caught one
at a time. A build that has not been tested is not finished, however green CI
is.

**Granularity (marth, 2026-07-21): batch small changes; do not atomize.** "One
change per build" is about *bisectable risk*, not literal minimalism — a
one-function bug fix does not deserve its own review-and-release cycle. **Very
small changes are a waste; roll them into the next substantive build.** The
balance: one *hook class* or one *risky mechanism* per build (so a CTD
bisects), but a bug fix found in testing rides along with the next change and
is reviewed as part of that change's diff. The boss-detection fix
(v0.4.1→next) is the worked example — held, not cut alone.

## Ordering: lowest crash surface first

Inherited from MRO, restated in MAO: **one hook class per build, so a CTD
bisects to exactly one change.** Event sinks (low risk) land before code hooks
(present/input thunks — higher risk).

MFO's ordering held here: sinks (M3) before the Field Kit's three trampoline
hooks. Tier B, when it comes, is **one mechanism per release** in the §4.5
reversibility order.

## Review (#45a)

**A commit touching `native/**` is not pushed until a Fable review of that
change has reported.** Not "usually", not "when substantial".

*Why it is stricter here than in the siblings:* there is no local compiler.
MSVC and CommonLibSSE-NG exist only on the CI runner, so CI is the first
compiler that ever sees the code and an in-game session is the first runtime.
The review is the only gate before both.

**Recording a review** — the family uses three mechanisms, all of them:
- the **State/Gate cell** in the milestone table below (`✅ Fable-reviewed`),
- the **CHANGELOG entry**, naming what the review yielded,
- the **tag**, when the review gated a release.

**Findings become permanent docs.** A review finding is not "fixed and
forgotten" — it lands in `ANTI_PATTERNS.md` (portable), `INVARIANTS.md` (with
its failure mode), or `ENGINE_NOTES.md` (if it is an engine fact).

## Release

- **`./release.sh` is the only way a build reaches the game.** Ad-hoc copying
  cost MRO a session where the running game and the archive disagreed.
- **Two-phase**: `./release.sh X.Y.Z` stamps and pushes; after CI is green,
  `./release.sh` verifies, packages and tags.
- **Releases and tags are immutable. Bump VERSION instead.**
- **Version granularity (marth):** a **patch** (`0.4.1`) is for review fixes on
  a build just cut. **Minor corrections found in testing do NOT earn their own
  patch** — they fold into the next **minor** (`0.5.0`) alongside its
  substantive milestone. The boss-detection and probe fixes are staged for
  `0.5.0`, not cut as `0.4.2`.
- **Minor bumps (`0.x.0`) are for MILESTONES only** — a real capability jump
  (gambits execute, board authorable, 1.0). Everything else is a patch.
  *Retrospective:* the first arc over-inflated — 0.1.0→0.4.0 should have been a
  handful of patches on a `0.0.x` line, since it was all pre-alpha foundation.
  0.3.0 (a bug fix) is the clearest offender. **This is not fixable** — tags
  are immutable, and numbering *down* would read as a downgrade and trip the
  newer-save guard. Forward-only from 0.4.x, with the discipline applied from
  here: `0.5.0` = M5 (the evaluator), then `0.5.x` patches until M6/M7.
- A release **must** carry its CHANGELOG entry (the script enforces it), and
  the entry must state **save compatibility** and any **upgrade action**.
- **FormIDs are forever once a build reaches a save. Never renumber.**
- A withdrawn release is **recorded as withdrawn** in the changelog, not
  deleted.

## Testing

- **marth tests, in-game.** MFO cuts releases locally and hands them over;
  MO2 setup is his side.
- **The test matrix is written before the code it gates**, and each check says
  what a *failure* would mean for the design — MEO's `P0_TESTING.md` format,
  which MFO's matrices should adopt.
- **A test pass gates the next milestone.**
- **Verify the build loaded first.** The version header in `MFO.log` is the
  mandatory first check; a stale binary voids every result.
- Test guides **supersede**, they do not accumulate.
- **Read the log after every test**, not only when something looks wrong. The
  v0.3.0 session reported "works, except boss mult"; the log showed kills
  crediting nobody at all.

## Division of labour

- **marth decides** design, balance, scope and sequencing. Decisions are
  recorded dated and attributed.
- **marth runs the game.** All in-game verification is his.
- **The assistant must not**: invent hook sites; write engine patterns from
  memory; hand-edit generated artifacts; re-derive what a sibling solved;
  push `native/**` before a review reports.
- **Escalate rather than improvise.** Where no maintained reference exists,
  the contract gets renegotiated with marth rather than reverse-engineered.

---

## Milestone state

| # | Milestone | Change class | State | Gate |
|---|---|---|---|---|
| M0 | CI green | build only | ✅ | artifact downloads |
| M1 | co-save round-trip | serialization | ⏳ **untested** | save/reload/load-order change |
| M2 | ESP generator + form resolution | records | ✅ in-game | forms resolve to ESL band |
| M3 | detection + Rapport | event sinks | ✅ detection tested; Rapport **untested** | award correctness matrix |
| — | Field Kit overlay | **code hooks ×3** | ✅ Fable-reviewed, v0.2.0 | HUD + panel + controller |
| — | v0.3.0 field fixes | logic | ⚠️ shipped unreviewed, reviewed after | kills credit correctly |
| — | scope cut + co-save v2 | schema | ✅ Fable-reviewed, v0.4.1 | schema verified clean |
| M4 | probe harness | engine calls | ✅ Fable-reviewed, **unrun** | retention watch answers §4.7 |
| M5 | evaluator + Tier A | hot path | — | perf budget, do-nothing guarantee |
| M6 | logistics table | — | — | loot rules, first dibs |
| M7 | board rule editing | UI | — | authorable on pad, no double-fire |

**Honest accounting of process violations so far**, because a rule stated
without its cost is not in the house style:

| Violation | Cost |
|---|---|
| Four builds pushed without a Fable review | Two save-corruption paths, a cursor over gameplay, and a schema bump shipped in the same session as "safe to start saving" |
| Five releases on one in-game test | Defects accumulated across builds instead of bisecting to one |
| No `BUILD.md` until now | Every rule above was already written down in MAO/MRO and went unread |
