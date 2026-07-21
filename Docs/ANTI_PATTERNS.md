# MFO — Anti-Patterns

The portable "never again" catalog. One rule, one line on how it bit.

Ported from MEO's release-proven digest (trimmed to what has an MFO analog),
plus **MFO's own**, which are marked `[MFO]` and dated. MFO's entries come
almost entirely from **Fable reviews** — this project cannot compile locally,
so review is the only gate between writing code and CI, and it has caught
things that would have shipped.

---

## Diagnostics and read-only code

**Never let a diagnostic mutate the state it reports.** `[MFO 2026-07-21]`
`g_followers[id]` in the state dump — `operator[]` **inserts**. One press of
the debug power would have created the record `Refresh` deliberately withheld
from a summon, keyed on a runtime FormID, which the next save would persist.
Use `find()` in anything that claims to be read-only.

**A counter nobody reads is not instrumentation.** `[MFO 2026-07-21]` The
combat-event counter was incremented and never printed, so the measurement it
existed for was unanswerable. Every counter needs a reader before it counts as
built.

**A diagnostic that always prints the same number is worse than none.**
`[MFO 2026-07-21]` The Field Kit's quirk-table line was hardcoded to 0/0
because nothing populated it — it would have "confirmed" a working quirk table
on a build where the table was empty.

**Instrument what you will actually be able to see.** `[MFO 2026-07-21]`
Detection only refreshed on load or on a kill, so recruiting a follower logged
nothing — the test matrix would have "passed" by producing no output at all.
Ask *how will I observe this* before writing the thing to be observed.

## Persistence

**Never store raw runtime FormIDs across sessions.** `0xFF`-prefixed ids
dangle or get reused by a different created form.

**`ResolveFormID` does NOT protect you from `0xFF` ids — it passes them
through as resolved.** `[MFO 2026-07-21]` So the guard has to be on the way
*in*, not on load. And `IsCommandedActor()` is not the same test: a spawned or
cloned teammate is not "commanded" but still carries a runtime id.

**Bound counts at BOTH ends.** `[MFO 2026-07-21]` The writer cast counts to
the field width but looped the whole list — desync at 256 entries. And writing
more than the reader's cap makes the reader abort the *entire* co-save, i.e.
total data loss for the mod. Clamp the count and the loop together.

**Over-cap entries must still be CONSUMED from the stream before being
dropped.** `[MFO 2026-07-21]` Clamping the count and skipping the reads
desyncs every byte after it.

**Never log a comforting falsehood about data safety.** SKSE destroys unread
co-save records on the next save; a reassuring log line walks users into
bricking their data.

**A dropped record gets a counter and a line.** Silent drops are how "the mod
ate my save" begins.

## Player-relative logic

**Never apply a player-relative filter to non-player state.** MEO's stacking
cap asked "is this among *the player's* worn items?", was reached with an NPC,
and stripped every NPC's enchant — v1.0.6's worst blocker. Thread the subject
actor explicitly; a `nullptr` subject is an error, not a wildcard.

**Gate before you scan.** `[MFO 2026-07-21]` The death sink ran a full
`ProcessLists` sweep for every death in the world and counted them all as
"kills" — so the measurement built to compare party kills against a balance
assumption was actually measuring ambient wildlife. Cheap filter first, scan
second.

## Threading and rendering

**Set per-frame render state per frame, not once at init.** `[MFO 2026-07-21]`
`io.MouseDrawCursor = true` at D3D init draws a software cursor over ordinary
gameplay for the entire session — invisible in a reference implementation that
only renders while its menu is open, immediate in one that renders every frame.

**"It works in the reference" is not evidence when you changed the frequency.**
`[MFO 2026-07-21]` MEO draws only when open; MFO's HUD draws always. Three
separate defects (the cursor, drawing over the title screen, drawing over
loading) came from that single divergence, and none of them exists in MEO.

**A lock-ordering rule you wrote must be a lock-ordering rule you keep.**
`[MFO 2026-07-21]` The mutex-nesting invariant was asserted in a comment and
violated eighty lines later in the same file. Not a deadlock — but the next
person to add an IO touch to the publish path would hang the render thread.

**Filter input at the source, not at the consumer.** `[MFO 2026-07-21]`
Skyrim re-fires button events every frame while a key is held; using
`IsPressed()` instead of an `IsDown()/IsUp()` edge filter made the close logic
depend on ImGui's internal dedupe rather than on correct input handling.

**One persistent sleeper thread, never a thread per tick.** `[MFO 2026-07-21]`
A detached thread spawned per 2 s interval is a leak and a shutdown hazard.

**A shutdown flag nobody sets is a comment, not a mechanism.**
`[MFO 2026-07-21]` `g_pumpRunning` was never cleared, so both guards were dead
code and the "safe across shutdown" claim was fiction.

## Build and CI

**`actions/cache` does NOT save when the job fails.** `[MFO 2026-07-21]`
Every failed compile threw away the vcpkg build, so a one-line error cost a
full cold rebuild. Early development is mostly failed builds — the cache
matters *most* when the build is broken. Split `cache/restore` + `cache/save`
with `if: always()`, saving right after the dependency step.

**Anything hashed into the cache key is a build-time cost.**
`[MFO 2026-07-21]` Stamping a version into `vcpkg.json` — metadata that
affects nothing — invalidated the cache on every release. (`restore-keys`
prefix fallback softened it, but don't rely on that.)

**Gate releases on the tree that produces the artifact, not the commit sha.**
`[MFO 2026-07-21]` A sha comparison blocks a release for a docs-only commit
and forces a pointless rebuild. Compare `git rev-parse HEAD:native`.

**The PCH must supply what generated code needs.** `[MFO 2026-07-21]`
`add_commonlibsse_plugin` generates a file using `"..."sv` literals and
force-includes your PCH into it — without `using namespace std::literals` the
build fails with C3688 in a file you never wrote.

## Environment

**An empty search of the docs is evidence about the DOCS.** `[MFO 2026-07-21]`
Twice in one day: controller support (shipped in MEO, undocumented) and the
entire actor-AI vocabulary (one `Actor.psc` read away) were both written off
as "no precedent" on a documentation search.

**Verify which instance is live before writing paths into a test procedure.**
`[MFO 2026-07-21]` A whole runbook pointed at the wrong MO2 instance because
"LoreRim is the test list" was inherited from a sibling's playbook rather than
checked. Testing happens in `custom-modlist`/Requiem.

**Where a plugin logs is a CHOICE, not an environment fact.**
`[MFO 2026-07-21]` Game-root-relative writes land in MO2's Overwrite beside
every other plugin's log; `SKSE::log::log_directory()` lands in the wine
prefix on a different filesystem. Picking the second means nobody finds your
log.

## Process

**Write the test matrix before the code it gates.** A gate defined afterwards
is a gate defined to fit whatever got built.

**A stale binary voids every test.** Check the log's version header before
believing any in-game result.

**Substantive code gets a Fable review before CI.** `[MFO 2026-07-21]` MFO
skipped it once for M3 and the follow-up review found two save-corruption
paths in that same code.
