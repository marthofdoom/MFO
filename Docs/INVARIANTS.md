# MFO — Invariants

The load-bearing rules. Each is an imperative plus the failure mode that
violating it produced. **Read before ANY code change.**

**Every rule here is currently `INHERITED`** — MFO has shipped nothing, so no
rule yet carries a local incident. Inherited rules are not weaker: each was
bought with debugging time on MRO, MEO, or MAO, and the citation names who
paid. Rules marked `DESIGN` follow from MFO's own design decisions rather
than from an incident; they are the ones most likely to be argued with, so
their reasoning is stated rather than assumed.

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

**#20 — Never use `AddBaseSpell` / `RemoveBaseSpell` on a follower.** They
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
