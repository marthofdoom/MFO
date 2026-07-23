# MFO — Invariants

The load-bearing rules. Each is an imperative plus the failure mode that
violating it produced. **Read before ANY code change.**

Tags: `INHERITED` (bought with debugging time on MRO/MEO/MAO — the citation
names who paid), `DESIGN` (follows from MFO's own decisions rather than an
incident; the ones most likely to be argued with, so the reasoning is stated),
and **`MFO`** (earned here, in the field, with the version and symptom).

**MFO now has its own scars: #22i and #22j**, both found by reading a real
session log rather than by review or by the tester's verbal report. That is
the point of the tag — a rule with a local incident outranks the same rule
with a borrowed one, because the next person will believe it.

**When MFO earns its own incident, replace the tag with the version and
symptom.** A rule with a local scar outranks the same rule with a borrowed
one, because the next person will believe it.

---

## A. Threading and concurrency

**#1 — All engine mutation goes through `SKSE::GetTaskInterface()->AddTask`.**
Sinks, the frame clock, and board actions only *queue*; the render thread
only *reads* mutex-guarded snapshots.
`INHERITED` (MEO §7, MAO §6).

**#2 — Never mutate a container or `BSSimpleList` while iterating it.**
Collect `(object, xList, key)` tuples first, act second, and **re-find live
records by key at act time** — a prior action may have rewritten the map.
Hold actors by `ActorHandle`, re-resolved at act time, never a raw pointer.
*Failure:* double XP ticks and chainable level-ups in MEO; node
use-after-free on the post-load path.
`INHERITED` (MEO §6, INVARIANTS #6).

**#3 — Equip/unequip dispatch is SYNCHRONOUS into every registered sink.**
Cycling gear on a follower re-enters follower AI and third-party outfit
managers (guaranteed present in a Lorerim-class order) which mutate the same
inventory. *Failure:* node use-after-free. MFO does this on the Tier-A equip
path, so #2's discipline is mandatory there, not advisory.
`INHERITED` (Linux-Native-Tools §18; MEO INVARIANTS #6).

**#4 — The authoritative follower store is main-thread-only and takes no
lock.** Sinks queue to main; serialization callbacks run on main; board edits
queue to main. If something needs it off-thread, the answer is a **snapshot**,
never a lock — a lock here would end up held across engine calls and
reintroduce the re-entrant self-deadlock.
*Failure (sibling):* MAO deadlocked on load calling `AddObjectToContainer`
under a non-recursive lock, because the call fires
`TESContainerChangedEvent` synchronously.
`DESIGN` (`ARCHITECTURE.md` §3.1) + `INHERITED` (MAO §7).

**#5 — Every ImGui-IO touch takes `g_imguiIoMx`; release it before the engine
passthrough.** IO is pushed (input thread), cleared (window thread), and
drained (render thread) concurrently.
*Failure:* a real data race hiding behind "it's just events."
`INHERITED` (MEO §19, ANTI_PATTERNS).

**#6 — `g_board.lock` and `g_imguiIoMx` are never nested**, in either order.
`INHERITED` (MEO §19).

**#7 — Every config value a live re-read can change mid-session is an
atomic.**
`INHERITED` (MAO §9).

---

## B. Persistence and the co-save

**#8 — Every persisted FormID passes `ResolveFormID` on load. Unresolvable →
drop the record, never guess.** The co-save stores raw runtime FormIDs
including the mod-index byte; any load-order change remaps plugin indices.
*Failure:* "the mod ate my save" on any Nexus install.
`INHERITED` (MEO §8, MAO §11).

**#9 — Never persist a runtime-created (0xFF) FormID.** Next session it
dangles or is reused by a different created form. This is why summons get
session-only boards (`DESIGN.md` §3.1).
`INHERITED` (MAO §14).

**#10 — Persist stable string opcodes, never enumeration ordinals.**
Gambit conditions and actions serialize as `cond.ally_hp_below`, not as index
7. *Failure:* reordering the vocabulary silently scrambles every save's rule
lists — and MFO's vocabulary is expected to grow every phase.
`INHERITED` (MEO §12) + `DESIGN` (`ARCHITECTURE.md` §8.2).

**#11 — Bound every count, bail on short reads, clamp at ingestion.**
Rule count clamped to the rank's slot maximum; rank clamped to `[1,5]`. A
truncated record stops the parse; it never fabricates rules from garbage.
*Failure:* a corrupt level 0 indexed `thresholds[-1]` two subsystems away.
`INHERITED` (MEO §9/§10, MAO §12).

**#12 — Versioned schema; readers for every shipped version kept forever;
fields append-only. SKSE does NOT round-trip unread co-save records — a
downgraded DLL that saves DESTROYS newer ones.** Warn loudly on-screen at
`kPostLoadGame`. **Never log a comforting "preserved as unread."**
*Failure:* the comforting log line would itself have walked users into
bricking their data.
`INHERITED` (MEO §11, MAO §13/§15).

**#13 — A migration that cannot honestly map a record DROPS it, with a log
line saying what was lost.** It never fabricates a replacement.
`INHERITED` (MAO §15).

---

## C. Actors, and the player-relative landmine

**#14 — Every predicate takes its subject actor as an explicit parameter.
There is no ambient "the actor" and no default-to-player. A `nullptr` subject
is an error, never a wildcard.**
*Failure (the one that should scare you):* MEO's stacking cap asked "is this
among **the player's** top-2 worn copies?" Reached from the NPC path the
answer was always no, so the enchant was stripped from every NPC item and its
record orphaned in the co-save permanently — **v1.0.6's worst blocker.**
**MFO is built entirely out of that shape**: every condition in `DESIGN.md`
§3.2 is a state scan and every one runs against a follower.
`INHERITED` (Linux-Native-Tools §20; MEO INVARIANTS #14).

**#15 — Every state-scan helper names WHOSE state it reads, in its function
comment; every new call site is audited against that comment.**
*Failure:* the player-side path in MEO had the comment explaining all this,
and the follower path — written later — violated it anyway. **The compiler
will not catch this.**
`INHERITED` (Linux-Native-Tools §18 corollary).

**#16 — Call the engine's own flow; never hand-write the state a flow
produces.** Before mutating actor state, read the SKSE64 source for the
equivalent Papyrus native and replicate *those* calls. Distrust
CommonLibSSE-NG's C++ reimplementations for engine-visible state.
*Failure:* MEO burned ~6 release cycles rediscovering, field by field, what
one engine call does. Any exception is enumerated in `DESIGN.md` §8.3 or it
does not exist. **MFO's current exception list is empty.**
`INHERITED` (MEO §1, standing doctrine).

**#17 — `PathToReference` is BANNED.** It is latent — "does not return until
the goal is reached or pathing failed or was interrupted" — so a call from
the evaluator stalls the main thread for the duration of a walk. Positioning
uses `KeepOffsetFromActor`, which is a state set, not a journey.
`DESIGN` (`DESIGN.md` §4.5, from the `Actor.psc` research pass).

**#18 — `ClearPackageOverride` is BANNED.** It removes overrides *added by
other mods*. MFO removes only its own, by handle, one at a time.
*Failure class:* the same shape as NG's `RemoveByType` — a library call whose
contract is wider than the caller's intent. This one is being obeyed before
the crash rather than after.
`DESIGN` (`DESIGN.md` §4.5).

**#19 — Every package override and every tutored spell is ledgered in the
co-save, and reconciled against live state every load.** Package overrides
**persist through saves**, so an unledgered one outlives the mod and breaks
`DESIGN.md` §8.5's clean-uninstall promise. An override with no ledger entry
is a bug and is logged as one.
`DESIGN` (`DESIGN.md` §4.5 / §5.4).

**#20 — Never use `AddBaseSpell` / `RemoveBaseSpell` on a follower.**
*(Moot in practice since 2026-07-21: MFO grants no spells at all — acquisition
is out of scope, `DESIGN.md` §5.4. Kept because the reasoning is sound and
costs nothing to retain.)* They
edit the **ActorBase** spell list, which is shared by every actor using that
base — for a generic follower, an entire class of NPCs. Tutoring uses
`AddSpell`/`RemoveSpell` (the added-spell list) only.
`DESIGN` (`DESIGN.md` §5.4).

---

## D. The evaluator

**#21 — Exactly one action per follower per tick, chosen by scanning from
rule 1 top-down and stopping at the first true condition.** No scoring, no
best-match, no tie-breaking, no program counter, no resumption.
*Why it is load-bearing:* multi-action ticks would make slot count a raw
power multiplier and break §4.2's budget; a persistent scan position would
break the statelessness that #22 depends on.
`DESIGN` (`DESIGN.md` §4.3 — the FFXII contract).

**#22 — The evaluator holds NO state between ticks.** Every tick re-reads
live state and re-evaluates from rule 1. No queue, no backlog, no pending
action, nothing that can be "behind."
*Why:* this single property is what makes round-robin servicing, load
shedding, and the no-catch-up rule all safe. A skipped follower loses
nothing, because the next tick asks the same questions of fresher state.
**Corollary:** never implement "match now, act after a reaction delay" —
jitter the *observation*, never queue the *action* (`DESIGN.md` §4.1b).
`DESIGN` (`DESIGN.md` §4.1a).

**#22a — A standing order is issued ONCE and re-issued only on invalidation.**
Never re-issue an identical target command per tick: `StopCombat`/
`StartCombat` churn resets the combat state the follower is acting on and
produces stuttering, never-actually-attacking behavior. Same target + still
valid + still the winning rule ⇒ **no engine call at all**.
`DESIGN` (`DESIGN.md` §4.7.2).

**#22b — The target latch is ACTUATION state, never EVALUATOR state.** The
test that keeps #22 intact: *if the latch were lost, would behavior change?*
Only by a redundant re-issue. The scan still runs top-down from rule 1 and
produces the same winner; the latch only decides whether to repeat itself. If
a change ever makes the latch affect *which rule wins*, that change is wrong.
`DESIGN` (`DESIGN.md` §4.7.2).

**#22c — Hysteresis damps WITHIN-rule target re-resolution ONLY. A different
rule winning supersedes instantly and is never damped.** Two switch kinds:
between-rule (the player's ordering decided it — instant) and within-rule
(MFO re-resolved the same rule's target — damped by `fTargetSwitchMargin`,
default 15%). Without damping on the second, two foes at 41% and 39% health
oscillate a follower forever. **Damping the first would be MFO overriding the
player's rule order** (#28). Invalidity always wins over commitment —
hysteresis governs preference, never validity.
`DESIGN` (`DESIGN.md` §4.7.3) + `INHERITED` (Aggro Management ships the same
15% for the same reason).

**#22d — The target latch is never serialized, and is cleared on load.** A
target handle that survived a save is the dangling reference #9 forbids.
Rebuild from live state; clear any owned command alias before the first tick
so a save made mid-order does not resume pointing at something stale.
`DESIGN` (`DESIGN.md` §4.7.5).

**#22e — A follower NEVER takes an owned item.** Ownership is absolute —
houses, shops, player-owned containers.
*Why:* an auto-looting follower that takes owned goods makes the player a
thief by proxy — the same class as the ownerless-`PlaceObjectAtMe` bounty
bug (#E), arriving before the incident rather than after.
`DESIGN` (`DESIGN.md` §4.8.3).

**#22g — NEVER mutate a container while the player has its menu OPEN. This is
a safety rule, not a courtesy one.** Mutating an engine container while a
vanilla menu is building its list from it breaks that menu — it broke
Belethor's barter menu in MEO (m19e). An open `ContainerMenu` is an absolute
bar, ahead of every loot delay and waiver. Where a mutation genuinely must
follow a menu interaction, defer with `AddTask` inside `AddTask` (two frames).
`INHERITED` (MEO m19e).

**#22h — First dibs: delay by default, collapsed (never zeroed) by a player
TAKE.** A ref is ineligible until `fLootDelaySeconds` (25) in radius; once the
player takes from it the delay drops to `fLootWaiverSeconds` (4), **with the
timer reset on every take.**
*Why not zero:* QuickLoot IE — present in 4 of 5 lists here, so the normal
case — takes items one at a time over seconds. An instant waiver lets the
follower grab from a corpse the player is still working through.
**Detect via `TESContainerChangedEvent` filtered to items entering the
player.** Two traps: (a) QuickLoot **never opens `ContainerMenu`**, so
menu-close detection alone misses nearly every user; (b) QuickLoot's menu
appears **passively on crosshair**, so *opening is not intent* — key the
waiver on TAKING, never on looking. The direction filter is mandatory or the
sink re-triggers on its own removal (MAO's infinite credit loop). Marked set
is a bounded LRU, deliberately not serialized.
`DESIGN` (`DESIGN.md` §4.8.3) + `INHERITED` (MAO §23 direction filter).

**#22f — Over-cap gambits must be fully CONSUMED from the co-save stream
before being dropped.** Clamping the count and skipping the remaining reads
desyncs every byte after it. Read all, store `min(count, slotMax)`.
`DESIGN` (`ARCHITECTURE.md` §7) — the general form of #11.

**#22i — One missed sweep is not evidence of absence.** `highActorHandles`
can transiently omit a live actor. Dropping a follower on a single miss is
not merely cosmetic: the death sink refreshes before awarding, so a kill in
that window credits nobody.
*Failure (MFO v0.2.0, field log):* `- 000E1BA9` then `+ 000E1BA9` 117 ms
apart — far tighter than the 500 ms pump, i.e. a death-triggered refresh
re-finding a follower the previous sweep had just dropped.
`MFO` — first invariant earned in the field rather than inherited.

**#22j — An observation surface that replaces another must not reduce
coverage.** *Failure (MFO v0.2.0):* the Field Kit made Rapport visible in game
and, in the same release, awards stopped being logged at all — they only
spoke on a rank change. The log had nothing to say about the one thing being
tested.
`MFO`.

**#23 — Conditions are pure reads. No mutation, no allocation.** A predicate
that mutates is a bug even when it works, because #21's early-out means it
runs an unpredictable number of times.
`DESIGN` (`DESIGN.md` §3.2).

**#24 — Never catch up after a stall. Fire at most once per wake; set
`last = now`.** A `last += interval` loop turns a 5 s load screen into ~37
back-to-back evaluations on the first frame of resumed gameplay — the most
loaded frame there is.
`DESIGN` (`DESIGN.md` §4.1) + `INHERITED` (MEO: a timer fired during a long
load screen is swallowed; anchor on `LoadingMenu`-CLOSE).

**#25 — The evaluator must not depend on the render hook.** The frame clock
is a refinement with a `std::chrono` fallback. If D3D init fails the board is
disabled; the followers must still work.
`DESIGN` (`ARCHITECTURE.md` §4).

**#26 — Suppression is positional: a rule ABOVE the one that fired may
preempt; a rule at or below its position may not.** Never absolute.
*Why:* an absolute window makes a first-ranked rule wait out a sixth-ranked
one, which inverts the contract in #21. The earlier "critical-urgency
conditions break suppression" ruling was **RETRACTED** — it created a second
priority system, owned by MFO, competing with the player's ordering.
`DESIGN` (`DESIGN.md` §4.4, `BALANCE.md` §6).

**#27 — Urgency tiers set NOTICING SPEED ONLY, never priority.** Which rule
wins is decided by the player's ordering alone. The moment MFO decides its
notion of "critical" outranks the player's rule 1, the list stops being a
program.
`DESIGN` (`BALANCE.md` §3/§6).

**#28 — MFO never reorders, dedupes, coordinates, or economizes on the
player's rule list.** Redundant heals across two followers are the player's
authoring, surfaced (§5.3) and not corrected. A full board carries no
penalty.
*Why:* anything else is the mod quietly disagreeing with an instruction it
was given — the one thing a programmable system must never do.
`DESIGN` (`DESIGN.md` §4.3a).

**#29 — A tick that cannot build its snapshot is SKIPPED, never partially
evaluated.** A half-built snapshot answering predicates is the
fabricate-from-garbage failure mode of #11, moved into gameplay.
`DESIGN` (`DESIGN.md` §4.2).

---

## E. The board

**#30 — Menu action rows fire SINGLE-SHOT via `ImGui::IsItemActivated()` —
never `Selectable-return || IsItemClicked`.** `IsItemClicked` reports the
PRESS frame; the `Selectable` return reports the RELEASE frame; OR-ing them
is **two fires per physical click**. The `busy` disable does not reliably
mask it, because the task pump usually beats the next present so no disabled
frame is drawn. **A snapshot generation check CANNOT catch it — the echo is
stale in intent, not in data.**
*Failure:* one click socketing both units of a stack; one click burning two
soul gems. Masqueraded as "misclicks" for a year. **In MFO it would delete
two rules per click.**
`INHERITED` (MEO INVARIANTS #20b — the single most expensive UI bug in the
family).

**#31 — Selection is keyed on stable IDENTITY, never on row index.** MFO keys
on rule id. *Failure:* MEO's label-sorted rows moved under a raw index and
the selection silently landed on a different item. **This matters more in MFO
than it did in MEO**, because reordering is a primary action — every move
would otherwise shift the selection to a different rule.
`INHERITED` (MEO m19e).

**#32 — Fetch the ImGui draw list INSIDE each `BeginChild`.** Rows drawn
through the outer window's list ignore the child's clip rect.
*Failure:* "long lists leave the pane" — and a rule list is exactly a long
list.
`INHERITED` (MEO m24c).

**#33 — No drag-and-drop for reordering.** The cursor is integrated from raw
mouse deltas and has a documented press/release divergence; a drag is
precisely the interaction that must survive press→move→release. It is also
unusable on a stick. Reorder is a discrete move-by-one on `▲▼` / L1 / R1.
`DESIGN` (`DESIGN.md` §6.5) + `INHERITED` (MEO's missed-click report).

**#34 — Overwrite `io.DisplaySize` from the cached backbuffer size every
frame, between `ImGui_ImplWin32_NewFrame()` and `ImGui::NewFrame()`.** The
Win32 backend reads `GetClientRect`, which disagrees with the backbuffer
under Proton and upscalers.
`INHERITED` (MEO §9).

**#35 — Never disable the list the player adds from.** Show it always and
reinterpret the action, saying in the header what it will now do ("REPLACES
rule 3"). Greying it out reads as broken.
`INHERITED` (MEO m35e).

**#36 — Full controller parity is a shipping requirement, not a phase-2
nicety.** Every board action, including reorder, must be reachable on a
gamepad with no keyboard. Standing family rule.
`DESIGN` + `INHERITED` (MEO m32f/m36e).

---

## F. Config, ESP, release

**#37 — An INI/MCM key that changes SEMANTICS must be RENAMED.** MCM Helper
persists values per key name into MO2's overwrite and they survive mod
updates. *Failure:* MEO's absolute→multiplier change on an unchanged key was
found live in a deployed profile and would have cut an XP stream ~100× for
every upgrading user, with the slider innocently showing the stale number.
`INHERITED` (MEO §21, MAO §29).

**#38 — Reset-then-parse every config pass; skip unparseable values; clamp at
parse; strip the UTF-8 BOM.** An absent key must revert to default, not stick
at its last in-memory value. A failed parse must never become `0.0`.
`INHERITED` (MEO §22, MAO §27/§30).

**#39 — Backfill every new MCM key into existing Settings files on deploy.**
MCM Helper reads a key absent from an existing Settings ini as OFF/zero; it
does **not** fall back to `config.json`'s default.
`INHERITED` (MAO §28).

**#40 — Generated text must equal DLL behavior.** The vocabulary JSON
generates the board strings, the MCM config, and the compiled tables from one
source. *Failure:* MEO shipped tooltips claiming 8–40% where the DLL did
5–25%, forcing a re-cut.
`INHERITED` (MEO §24, MAO §32).

**#41 — FormIDs are forever. The `0x800`–`0xFFF` band is a frozen
generator↔DLL contract**, anchored by `data/mfo_forms.frozen.json`;
`next_fid = max+1`; never recycled; the freeze guard trips on **both** drift
and shrink. `tools/audit_esp.py` PASS is a merge gate.
`INHERITED` (MEO §23, MAO §31, MRO).

**#42 — Before creating any new record type, dump a vanilla record that does
what you want and mirror its subrecord list, order, and byte layout exactly.**
The engine drops malformed records **silently** — there is no error.
`INHERITED` (the family's one principle).

**#42a — Every statically-linked dependency's licence notice ships with the
release, and `THIRD-PARTY-NOTICES.md` is updated in the SAME COMMIT that
changes `native/vcpkg.json`.** The `x64-windows-static-md` triplet links
dependencies into the binary, and MIT requires its notice to accompany
"copies or substantial portions" — a statically-linked DLL is a copy. No
third-party source is ever vendored into the repo; that is a separate matter
and is not what discharges this obligation.
*Do not assume a transitive dependency's licence* — enumerate the real linked
set and reproduce each licence from its own repository. Assuming a licence is
the same mistake as assuming an engine mechanism.
`DESIGN` — and a gap in MRO/MEO/MAO, none of which currently carries one.

**#43 — Do not ship VMADs for scripts you don't ship.**
*Failure:* orphaned script references spammed Papyrus errors every load in
every MEO 1.0.x zip.
`INHERITED` (MEO ANTI_PATTERNS).

**#44 — Every build that reaches the game goes through the release script; a
stale binary voids every test.** The log's version header is the mandatory
first check before believing any in-game result — MEO was bitten twice.
`INHERITED` (MRO/MEO).

**#45 — One new hook or engine mechanism per release**, so a CTD bisects to
one change. Especially binding for Tier B (`DESIGN.md` §4.5).
`INHERITED` (MRO NATIVE_REWRITE_PLAN).

**#45a — Substantive code gets a FABLE CODE REVIEW before it goes to CI or
release.** A standing family rule, not a per-project choice. Precedent: MAO's
`BUILD.md` records P1d as "✅ Fable-reviewed, tag `v0.8.2-p1d`"; MEO's 1.0.7
roadmap requires the tally-cap change to get "its own Fable review + deck
test, not a graft into a finished release."
*Why it matters here specifically:* this project cannot compile locally —
there is no MSVC or CommonLibSSE-NG on the dev machine by design — so CI is
the first compiler that ever sees the code and an in-game test is the first
runtime. A review is the only check that happens before either.
*Scope:* new subsystems, engine-facing code, anything touching the co-save or
threading. Not doc-only commits.
`INHERITED` (MAO BUILD.md, MEO ROADMAP-1.0.7).

**MFO has violated this FOUR times** — M3, then three builds in a row on
2026-07-21: the v0.3.0 field fixes, the tutoring removal, and **the co-save
schema v2 bump**. The last of those shipped in the same session as telling
marth it was safe to start saving real games.

**The pattern, because it is a pattern and not four slips:** the violations
all happened when something *else* was the headline. Fixing a reported bug,
cutting scope, reacting to "I'll be saving from now on" — the review got
skipped precisely when momentum felt highest, which is exactly when review is
worth most. A rule obeyed only when nothing is happening is not a rule.

**Mechanical form, so it does not depend on remembering:** a commit touching
`native/**` does not get pushed until a review of that change has reported.
Not "usually", not "when substantial" — the schema change looked small and was
the most dangerous thing in the project.

---

## G. Diagnostics

**#46 — Log the zero case.** A guarded or periodic path that logs only on
success makes "ran and found nothing" indistinguishable from "never ran."
*Failure:* a real MEO vendor bug hunt was blind for two visits.
`INHERITED` (MEO ANTI_PATTERNS).

**#47 — Split skip/failure counters by reason.** A single aggregate count
hides a 100%-systematic failure inside what reads like ordinary attrition.
*Failure:* MEO's 10,146-row conversion table resolved "0 live" because
`LookupForm<T>` gates on `Is(FORMTYPE)` and intermediates return nullptr
every time — compiled clean, failed silently.
`INHERITED` (Linux-Native-Tools §14).

**#48 — When a computed number is wrong, log EVERY term of the formula; and
before shipping a fix, check the hypothesis reproduces the observed number.**
*Failure:* MRO's diagnostic logged every term except the guilty one, costing
two release cycles.
`INHERITED` (MRO DEBUGGING).

**#49 — An empty search of the documentation is evidence about the DOCS, not
about the engine or the codebase.** Before writing "no precedent exists,"
grep the siblings' `native/plugin.cpp` and CHANGELOGs and read the Papyrus
surface of the mechanism.
*Failure:* two sections of MFO's own design document were written wrong on
this basis — controller support (shipped in MEO, undocumented) and the entire
Tier-B action vocabulary (one `Actor.psc` read away).
`INHERITED` — **and the only rule here MFO has already broken.**

### 46. The evaluator's cadence is the evaluator's constant

M5 rode the diagnostics pump's refresh interval. That interval existed to
decide how often a HUD redraws; when it moved 2000 ms -> 500 ms for the Field
Kit, it silently quadrupled how fast gambits fire, and the `133 ms` constant in
`Scheduler.cpp` was dead code the whole time because the caller never arrived
faster than it. A display constant must never set behaviour. The pump now wakes
at the response deadline and DIAGNOSTICS subsample it, not the reverse.

*Caught in the M5 pre-CI review, before the first field session.*

### 47. A percentage is over the TRUE maximum, temporary modifiers included

`GetPermanentActorValue` is base + permanent and OMITS the temporary modifier,
which is exactly where fortify-health from gear and potions lives. Using it as
"max" makes a buffed follower read full while wounded, so `HP < 40%` fires near
27% of real maximum. In a heavily-modded order every follower wears that gear.
One shared helper (`Vocab::Pct`) computes it, and the Field Kit's bars call the
same helper -- a HUD that disagreed with the evaluator would lie at exactly the
moment the player consults it to ask why a rule did not fire.

### 48. A repeating failure logs on transition, never per tick

A permanently failing rule is the WINNING rule every tick, because failures
correctly do not buy suppression. Logging it unconditionally is ~7.5 lines per
second per follower, each with a synchronous flush on the main thread: a frame
cost and a flood that drowns the signal #22j protects. Log on change of
(rule, reason); the board carries the per-tick truth.

### 49. One pump thread, enforced by generation token

`StopPump` clears a flag and `StartPump` sets it. A thread mid-`sleep_for`
across a fast revert -> load wakes, re-reads a flag that is true again, and
keeps looping alongside its own replacement -- doubling the tick rate, once per
fast load, permanently. A boolean cannot express "you specifically should
stop". An epoch counter can.

### 50. Every path out of the round-robin advances the cursor

An identity-keyed cursor fixes the reordering unfairness of a bare index, but
it introduces a failure the index form did not have: any `return` that skips
the cursor update makes the NEXT tick select the SAME slot. A held null handle
is the live case -- `Followers::Refresh` deliberately re-pushes handles that
fail to resolve for up to `kMissesBeforeDrop` sweeps, so a transient null sits
at a fixed position and pins the rotation. Nobody in the party is evaluated for
up to ~1.6 s, in combat, which is the only time it matters.

`g_activeIds` exists for exactly this: it still knows who a slot is when the
handle cannot say. **Advance on the null path, then return.**

*A fix for one review finding (index-based unfairness) creating a worse one is
the argument for the verification pass, not against the first review.*

### 51. Combat participation is remembered, never sampled after the fact

Rapport's shared-kill test ran from a queued death task and asked
`IsInCombat()`. But killing the last enemy **ends the fight**, so a follower who
fought the entire battle reads `false` at the only instant the test gets to
look. The radius fallback then measures to the PLAYER — and the follower who
did the work is the one most likely to be far from them.

Observed: Cosnach fought and killed a fox ~2792u out and was credited nothing.
The log said `0 follower(s) credited` and could not say why.

Sample combat state on the sweep and keep a timestamp; test "was fighting
within N seconds", plus proximity to the VICTIM as well as the player. **Any
state that the triggering event itself destroys must be observed before the
event, not after it.**

**Corollary, review-found while fixing the above:** a null handle must not mean
"no credit". `Refresh` deliberately HOLDS unresolvable handles for up to
`kMissesBeforeDrop` sweeps, and `if (!f) continue;` silently skipped exactly
those followers — in both the award loop and the new diagnostic. Nothing in
either needed the actor: the kill test is a FormID compare, the grace test
takes a FormID, `Award` takes a FormID. Iterate index-aligned with
`g_activeIds` and fall back to id-only tests. A diagnostic that skips the
unresolvable case prints an EMPTY block under "0 credited", which reads as
"the loop never ran" — the least useful possible output from the code whose
only job is explaining the zero.

### 52. A HUD counter says whose and over what window

The overlay showed `0 rap` directly above a follower reading `5`. Both were
correct — the header is session-earned, the row is lifetime — but read together
they say "the mod lost your save". A number with no scope label is a bug report
waiting to happen, and this one arrived within a day.

### 53. Silence must be distinguishable from death

The 0.5.1 session seeded rules correctly and produced not one `[eval]` line.
That is CORRECT behaviour if no condition matched -- and it is also exactly what
a dead evaluator looks like. The log could not tell them apart, and the tick
counters that would have (`TicksThisSession`, `LastTickMs`) had existed since
0.5.0 and were surfaced nowhere. A review flagged them as unused; the finding
was recorded and not acted on.

Any subsystem whose correct behaviour is *doing nothing* must publish a
heartbeat. Otherwise the first field report is unanswerable without a rebuild.

### 54. Read the class declaration before naming something an open problem

The missing cast animation was called "the biggest open problem in the mod" and
answered with a list of guesses. One fetch of `ActorMagicCaster.h` showed it
inherits `SimpleAnimationGraphManagerHolder` and sinks `BSAnimationGraphEvent`
-- the caster is driven by the animation graph, so `CastSpellImmediate` could
never animate regardless of casting source. Two shipped mods already solve the
general problem and point at a different architecture entirely.

The research method exists and is documented. It was skipped twice in one
project: once on actor AI, once here.

### 55. Restore before you stop tracking

The shield-restore sink is gated on `Followers::IsTracked`. A dismissed
follower therefore falls out of tracking *while still holding MFO's spell*, and
nothing afterwards can ever give their gear back. Hand back what is owed in the
dismissal path itself, BEFORE the record leaves the active set.

The general rule: any obligation keyed on "is this actor ours?" must be settled
at the moment they stop being ours, not after.

### 56. A projectile call cannot cast a self-targeted spell

`Projectile::LaunchSpell` looked like the animated cast MFO needed. It launches
a **projectile** — and a Self-delivery spell has none. The flagship gambit
(`Self HP < 40% -> Cast Healing`) would have equipped the spell, played nothing,
and applied NO EFFECT: strictly worse than the silent heal it replaced. It also
bypasses `MagicCaster`, so it spends no magicka, which would have made §5.3's
competence gate a tautology.

Before swapping the verb that performs an action, check it against the
*delivery types* the action actually uses — not just the one being demoed.

### 57. Do not ship the mechanism you told yourself to probe

ENGINE_NOTES §0.13 was written this session and ended: *"M4-style probe first,
both mechanisms, before any design commitment."* The very next change shipped an
unprobed third hybrid. The instruction was correct and one hour old.

Pressure to deliver a complete build is exactly when the probe gets skipped, and
exactly when it is worth most.

### 58. Read the function's own doc comment before building on its name

`DoCombatSpellApply` sounds like "make this actor cast this spell at that target
as a combat action". It is not. Skyrim's own `Actor.psc` says, one line above
the declaration: `; Apply a spell to a target in combat`. The Papyrus index
positions it as the alternative to `AddSpell`. Bethesda's Dawnguard uses it to
*instantly eject the player from a shield sphere*, and one shipped mod calls it
out of combat entirely, where no animation is possible.

It is the Papyrus twin of `CastSpellImmediate`. An entire mechanism was designed
and shipped default-ON around a verb whose own documentation, present on disk in
the installed SKSE scripts, refuted it in one line.

### 59. Only redirect when the engine already has a target

The combat-target hook writes MFO's choice ONLY when vanilla has already picked
someone. If the engine cleared the target — the foe fled, died, went undetected,
combat ended — it did so for reasons MFO cannot see, and forcing a target back
in means fighting the engine's own validity logic. That is how followers end up
swinging at things they cannot perceive.

**Commanding WHICH foe is ours. Commanding THAT there is a foe is not.**

### 60. Name the conflicting mod in the log, at install time

`Aggro Management in Skyrim` steers follower targets through its own hate table
and hooks detection. It is installed in LoreRim, and MFO writes the same two
fields. Two writers is a fact of the terrain, not a reason to avoid the
mechanism — but it must be VISIBLE at startup, or the resulting weirdness gets
blamed on whichever mod the user installed most recently.

Any hook that shares state with a known third-party plugin announces that plugin
by name when it detects it.

### 61. If a shipped mod already solves it, read its source before building a probe

The §4.7 retention question — "does a commanded combat target stick?" — cost
roughly 90 minutes of the maintainer's play time across two sessions, plus a
probe harness, a crosshair sink, a hotkey, and three builds. The answer was one
sentence in an open-source plugin already installed on the machine:
SmartTargetingNPC rewrites the target after every `UpdateCombat` **because the
engine re-picks**.

The research method already says installed modlists are primary sources. It does
not say "after you have built the instrument". **Reading costs minutes and the
maintainer's time is the scarcest resource in this project** — a probe is for
questions no existing code answers, and that has to be checked FIRST, not after
the harness is written.

Applies with double force to anything requiring a human to play the game to
observe it.

### 62. A commit message is a claim about the code, not about the intent

Commit `7e10ca1` described an upgrade to the `[cast]` evidence line -- resolving
the form properly, flagging MFO gambit spells -- **that was never written.** The
edit was clobbered while removing an unrelated feature from the same file, and
the message was composed from what was meant to land rather than from the diff.
The history now permanently describes code that did not exist.

Two sibling failures the same hour: a feature reported as "reverted" when its
commit had already pushed (the working tree was clean, which proved nothing),
and a push to CI with no Fable review (#45a, the recorded pattern: it happens
when something else is the headline).

**Read the diff before writing the message.** `git show --stat` at minimum, and
grep for the specific thing being claimed. A status report that was not verified
is worse than no report, because it stops anyone else from checking.

### 63. Never make the rule smarter than the player wrote it

A field session showed a follower's AI declining to cast a very weak heal, so
only MFO's silent fallback ran. The instinct was to make the seed prefer spells
the AI would actually use. **That is the wrong instinct**, and marth stopped it:
*"a bad gambit setting still needs to fire."*

MFO executes the list as written. It does not substitute a better spell, pick a
better target than the selector named, or decline a rule because the outcome
looks poor. A rule that wastes magicka on a useless heal is information the
player needs to see, and hiding it behind a heuristic makes the board a liar.

The mod's job is to make the list run and to say plainly what happened
(§5.3, §4.3a). Judging the list is the player's.

### 64. Match the source to the QUESTION TYPE, not to habit

Two different questions, two different primary sources, and confusing them cost
this project most of a session:

| Question shape | Primary source |
|---|---|
| *"Can I call X?"* | CommonLibSSE-NG headers |
| **"How does the game already DO X?"** | **`Skyrim.esm` records, and shipped mods' ESPs** |

MFO's questions were almost all the second kind — it is the first mod in this
family that drives ACTOR BEHAVIOUR rather than menus or crafting. Headers were
read constantly; game data was barely touched until ten hours in.

**One dump of `Skyrim.esm` would have shown `UseMagic`, `UseWeapon`,
`HoldPosition`, `Travel` and `Activate` — the entire actuation architecture,
already in vanilla — in the first hour.** Instead three cast verbs were refuted
by guessing, ~90 minutes of the maintainer's play time went on a question
answered in an installed plugin's source, and the architecture was found last.

The engine is a DATA-DRIVEN game. When the question is what an actor should do,
the answer is far more often a record than a function.

**Practical rule:** before reaching for an API to make an actor behave a certain
way, dump the relevant vanilla records and ask whether the game already models
it. `Skyrim.esm` is on disk. Reading it costs minutes.

### 65. Never point a package's target at its OWN delivering alias  *(revised)*

MFO's first cast package pointed its Target input at `PTDA targType 4 -> alias 0`
-- the same alias that delivers the package -- reasoning that since the alias
holds the follower, alias 0 is himself. The package took ownership of the actor
and never cast: no animation, no effect, no magicka spent.

**REVISED by the probe ladder (§0.21).** The original rule said "an alias
targets someone else; self has its own target type". Half right. Probe 3 --
`targType 0` naming the caster's OWN reference -- casts fine. So self-targeting
is not the problem.

The problem is the **indirection back to the delivering alias**: `t4 -> alias 0`
where alias 0 is what delivered the package. That resolves to the runner through
the very alias being evaluated, and the procedure goes inert.

**Proven shapes:** `cast_self` = `t0` -> the follower's own reference.
`cast_target` = `t4` -> a DIFFERENT alias, with `QNAM` (probe 5).

**Diagnostic worth keeping:** a package that OWNS an actor while the actor does
nothing and spends nothing means the procedure's inputs are unresolvable. It
does not mean delivery failed -- delivery visibly succeeded, or the actor would
not be rooted.

### 66. Author only record SHAPES that occur in vanilla — and a FormID is not a spell

MFO authored a package that was alias-delivered, self-targeting, and carried
`QNAM`. The engine CTD'd evaluating it — a null deref inside `TESPackage`, no
MFO frames in the stack. That combination occurs **zero times** in Skyrim.esm:
all 9 alias-delivered `UseMagic` packages use a specific-reference target and no
`QNAM`, and none of the 7 self-casting ones is alias-delivered.

**Before authoring a record shape, ask whether vanilla ships it.** With
`tools/esp_inspect.py` that is one command. A shape with no precedent is not
necessarily illegal, but it is unexplored, and the engine's error handling for
unexplored combinations is a crash.

*(The first version of this rule blamed concentration spells. It was wrong —
12 of 46 vanilla `UseMagic` packages cast them, two with Flames. The wrong
conclusion came from parsing `SPIT` at the wrong offset and not sanity-checking
a result that called vanilla `Healing` a constant effect.)*

**Second rule, still standing:** a vanilla FormID does not identify a vanilla
spell. `00012FCC` is `Healing` in Skyrim.esm and `REQ_Restoration2_HealSelf` in
a Requiem load order. Validate what the LOAD ORDER resolved, never what the wiki
says the FormID is.

### 66a. A record shape is a VECTOR of axes; precedent is a contingency table

#66 said "ask whether vanilla ships this shape". **That rule, as written, would
not have prevented the crash — because the postmortem ran the query and drew the
wrong conclusion from it.** Counting "alias-delivered UseMagic packages" gave 9
records and the false rule *"QNAM must be absent"*. Counting the same question
over all 5,857 PACK instances gives 356 records that ship exactly MFO's intended
shape.

**The sharper rule:**

1. **Decompose the record into AXES** (delivered-by-alias? input-names-alias?
   QNAM present? target targType? subrecord order?), not into a single "shape".
2. **Count over the widest population sharing the MECHANISM** — all PACK
   instances, not one template's — and **enumerate the cells, including the
   zeros.** A zero cell is the finding; a small non-zero cell is permission.
3. **Change ONE axis at a time from a NAMED exemplar.** The crash record changed
   three at once, so its failure identified none of them.
4. **Subrecord ORDER is part of the shape.** `QNAM` after `PKCU` is 0 of 2,109.

`tools/esp_inspect.py --pack-shapes` produces this table directly, and its
selftest pins the population facts so a future refactor cannot silently move
them.

**Corollary:** a query answered over a subpopulation you chose for convenience
is not evidence about the population you care about. Both wrong versions of
§0.20 were confidently derived from real data.

### 67. ~~Refuse concentration self-casts~~ — REVOKED 2026-07-22

**This rule was wrong and is revoked.** It forbade concentration self-casts on
the theory that `targType 6` in an alias-delivered package was the rev-4 crash
axis. Probe 6 is precisely that shape — CollegePracticeWard, Concentration,
Self, `t6`, alias-delivered — **and it casts.**

The crash was the `QNAM`, not the `t6`: rev 4 carried one (misordered, after
`PKCU`) on a record whose inputs named no alias. The surviving rule is the one
already in the generator — **emit `QNAM` only when an input names an alias, and
always immediately before `PKCU`.**

Kept as a numbered entry rather than deleted, because the reasoning that
produced it was sound given the evidence at the time, and the correction is the
useful part: a rule derived from a crash with TWO novel axes cannot attribute
the crash to either one. Change one axis at a time (#66a) or the postmortem is a
guess.

### 67a. (superseded content below, retained for the reasoning trail)

Vanilla ships concentration + Self-delivery spells in `UseMagic` **only** with
`targType 6` (2 of 2), and `targType 6` in the target slot of an
**alias-delivered** package is a zero cell that CTD'd the game (§0.20). The two
vanilla concentration-self records are not alias-delivered, so the shapes do not
compose.

Until a probe proves otherwise, a gambit naming a concentration spell with Self
delivery **fails with a reason** -- the §5.3 pattern, legible on the board --
rather than being synthesised into a record with no precedent.

Fire-and-forget Self spells are fine: `targType 0` at a reference works
(probes 2 and 3, and `dunReachwaterRockGauldurReforgeAmulet`).

### 68. A doc edit anchored on a non-unique string duplicates the document

Twice in one session an ENGINE_NOTES section was pasted twice because the
anchor text ("### NOT yet proven, despite the session") occurs more than once in
the file, and a blind replace hit every occurrence. The second copy fused into
the following header and corrupted the tail.

This is the code lesson in prose form: **assert the anchor is unique before
replacing, and re-read the result.** `grep -c` on the header afterwards is one
command and catches it immediately.

### 69. Never leave a follower in MFO's alias without a valid package

An actor sitting in MFO's alias while every alias package's condition is false
**stands still** — verified in the field with `MFO_ProbeSelect = 0`, before any
probe was chosen. MFO's quest outranks the follower's own at priority 60, wins
the arbitration, and then supplies nothing.

Vanilla's answer is an ungated fallback at the bottom of the package list (299
of 740 multi-package aliases do this). **MFO must not copy it:** an always-valid
fallback means MFO permanently supplies the winning package, which is "MFO owns
the FOLLOWER" — the §4.5c violation, and a permanent override of whatever
framework manages that follower.

**So: fill the alias for one action, clear it the moment the action completes,
and never park a follower there.** Clearing is correctness, not tidiness — a
missed clear is a frozen follower. The watchdog timeout, the `kPreLoadGame`
release and the post-load reconcile are all load-bearing, not defensive.

### 70. Alias membership is the gate; production packages carry no conditions

Measured (§0.25): the engine picks the highest-priority quest whose alias CLAIMS
the actor, then asks it for a package. It does not skip a claiming quest that has
nothing valid. So MFO at priority 60 with an empty alias freezes the follower,
and MFO at 25 never gets asked at all.

**Therefore production packages are UNGATED.** MFO fills the alias exactly when
it wants that action, so the package is valid the instant it arrives and MFO
never claims a follower while offering nothing. Fill = act, clear = release.

The probe ladder's `GetGlobalValue` conditions exist only because it force-fills
the alias permanently for testing convenience. **Do not carry that pattern into
production** — a gated package on a permanently-filled alias is precisely the
frozen-follower bug of #69.

### 71. Never deref a CombatController member at offset >= 0x68 (NG AE-layout bug)

CommonLibSSE-NG's `CombatController.h` guards its AE-only member on
`SKYRIM_SUPPORT_AE`, which NG never defines -- so the struct compiles SE-layout
on every build and every member past 0x68 is +8 at runtime on AE. Reading
`cachedAttacker` (compile 0xC8) actually reads `handleCount` (= 1 in a
one-enemy fight); a pointer valued 1 crashes on the formID read. It CTD'd the
game twice, identically, and two wrong fixes recurred because both touched the
actor-resolution PATH, not the corrupt OFFSET (§0.29).

Use only the layout-stable members before 0x68: `combatGroup` (0x00),
`attackerHandle` (0x28), `targetHandle` (0x2C). For anything past it, gate on
`REL::Module::IsAE()` and use manual offsets. This is the second time a
plausible-looking pinned header was wrong (StartCombat relocation §0.12 was the
first) -- a header compiling is not proof its layout matches the runtime.

### 72. The tick is on a JOB thread: walks read-only, mutate after, iterate under a lock

crash4 (§0.30) proved the SKSE-task tick runs on a `BSJobs` worker thread, not
the main thread -- so `Scheduler::Tick` and everything it calls overlaps the
engine's cell-streaming threads. Two rules follow, both mandatory for any code
reached from the tick:

- **Never call `RE::TES::ForEachReferenceInRange` (or otherwise chase
  `TES::worldSpace`/`gridCells`/the skycell).** Those globals are rewritten
  mid-transition and tore into a garbage pointer that CTD'd the game. Walk the
  actor's OWN parent cell instead -- `GetParentCell()`, gated on `IsAttached()`,
  then `cell->ForEachReferenceInRange(...)`, which iterates only that cell's
  reference list under the cell's `BSSpinLock`.
- **A world walk READS ONLY inside the walk and MUTATES AFTER** on re-resolved
  handles (collect handles, then act). Mutating a container or a ref from inside
  the walk, off the main thread, races the engine that owns it.

This also retro-justifies the collect-then-act shape (#2) as a threading
requirement, not just a re-entrancy nicety.
