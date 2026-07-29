# marth's Follower Overhaul (MFO) — Design Document

Programmable follower behavior for Skyrim SE. Every follower carries an
ordered list of **Gambits** — `[Condition] -> [Action]` rules, first match
wins — authored live, in game, per follower. This is Final Fantasy XII's
gambit system rebuilt on the engine's own actor primitives.

**A gambit has NO name.** Its entire identity is its condition (what determines
the target/trigger) and its action (what it does) — the board shows exactly
that and nothing else. There are no named, clever, or special-cased gambits;
every behaviour the player can express is a *composition* of the primitive
conditions and actions in the vocabulary. If a wanted behaviour can't be
written as condition → action, the fix is a new primitive, never a bespoke
named rule (marth, 2026-07-28). The build list of approved primitives lives in
`GAMBIT_LIBRARY.md`.

Target: vanilla SE + SKSE core. Built with the toolchain in
`MANUAL_MOD_CREATION_GUIDE.md` (Python ESP generator, Papyrus via Proton
wine) plus the native layer (`native/plugin.cpp`, CommonLibSSE-NG, CI-built),
copied from the sibling projects rather than re-derived.

**STATUS (v0.0.0, pre-implementation): this is a SPEC, not a reconciliation.**
Unlike MEO's and MAO's design docs, nothing here has shipped. Every section is
a design commitment subject to the phase gates in §10. Where a mechanism has
been *researched but not yet run*, it is marked **RESEARCHED** and cites the
reference that maps it; where a sibling has shipped it, **PROVEN**.

### Why native (the two reasons that drive it)

1. **Conformance.** A CommonLibSSE-NG SKSE plugin is the strictest, most
   stable modern standard — Address-Library-based, engine-version-resilient,
   the ecosystem norm. A Papyrus gambit evaluator is not merely
   non-conforming, it is **impossible at the required cadence**: §4.1 sets a
   133 ms response deadline with a per-frame budget under 1% of a 30 fps
   frame, across N followers with a dozen predicates each. Papyrus VM latency
   under combat load is exactly the failure mode. MRO
   measured the adjacent version of this cost directly — a global
   `RegisterForActorAction` on a high-frequency action taxes the whole VM on
   a large list, and **the cost is the dispatch, not the handler body, so
   filtering inside the handler does not help.**
2. **No save bloat.** Per-follower rule lists, Rapport ledgers, and the
   taught-spell ledger live in the DLL and serialize to the **SKSE co-save**
   (`.skse`), NOT the Papyrus VM in the `.ess`. Per-actor script bookkeeping
   in Papyrus is what bloats and orphans saves; owning the index natively
   sidesteps the class entirely.

### Inherited doctrine (from MRO/MEO/MAO — applied, not re-derived)

The sibling projects bought these with debugging time. MFO adopts them whole;
see `ANTI_PATTERNS.md` (ported digest) and `INVARIANTS.md`.

- **Replicate engine features by CALLING the engine's own flow, the way
  SKSE's Papyrus natives do — never hand-write the state a flow produces.**
  Before mutating actor state, read the SKSE64 source for the equivalent
  Papyrus native and replicate *those* engine calls. Distrust
  CommonLibSSE-NG's C++ reimplementations for engine-visible state. This rule
  is also MFO's **research method** — see §2.
- **No Linux cross-build.** CommonLibSSE-NG needs the MSVC linker → build on
  a GitHub Actions `windows-latest` runner; vcpkg binary cache keeps warm runs
  ~2–3 min. Pin the colorglass registry baseline; never float it. Verify any
  API against the **CharmedBaryon/CommonLibSSE-NG** raw headers before
  spending a CI round-trip — the forks diverge and no headers exist locally.
- **Every DLL/ESP build that reaches the game goes through the release
  script** (tag + immutable `releases/vX.Y.Z/`, version stamped into the MCM
  About page). **A stale binary voids every in-game test** — the log's version
  header is the mandatory first check before believing any result.
- **Hooks: Address Library IDs only, one hook per release, INI-gated,
  self-verify the code site at install (bail on opcode mismatch).**
  Instruction-cave/fixed-offset patches are banned. The disk EXE is Steam-DRM
  encrypted — **static byte reads return garbage**; verify against
  `/proc/<pid>/mem` of the running game (`verify_hook_site_live.py`), and
  validate the Address Library `.bin` against crash-log ground truth first.
- **All engine mutation goes through `SKSE::GetTaskInterface()->AddTask`.**
  Sinks, timers, and menu actions only queue; the render thread reads
  mutex-guarded snapshots.
- **Never mutate a container or `BSSimpleList` while iterating it** —
  collect `(object, xList, key)` tuples first, act second, re-find records by
  key, hold actors by `ActorHandle` re-resolved at act time.
- **GlobalVariable values are save-persisted** — re-assert on
  `kPostLoadGame`/`kNewGame`.
- **Instrument, don't eyeball.** When a computed number is wrong, log EVERY
  term of the formula; before shipping a fix, check the hypothesis reproduces
  the observed number. Split skip/failure counters by reason — a single
  aggregate count hides a 100%-systematic failure inside what reads like
  ordinary attrition. Log the zero case too.
- **DYNAMIC_OR_DROP:** anything whose behavior depends on values baked in from
  THIS machine's load order at generation time must become dynamic or be
  dropped before 1.0. MFO is structurally compliant by construction — the
  entire action vocabulary is derived from the live actor at runtime (§3).

### Core principles

- **The follower is the subject, never the player.** Every predicate is
  evaluated against a named actor. See §8.4 — MFO's single most dangerous
  inherited landmine.
- **Gambits advise; they never seize.** Vanilla AI keeps running. A gambit
  fires only when it has something to say (§4.4).
- **The list is the player's.** MFO executes it as written — no reordering,
  no coordination, no deduplication, no penalty for using every slot. A badly
  written list is made *legible*, never corrected (§4.3a).
- **Competence is not permission.** Teaching a follower a spell does not mean
  they can cast it. Skill and magicka are checked by the engine, not by us
  (§5.3).
- **Nothing MFO gives a follower is unremovable.** Every taught spell, every
  package override, every applied effect has a ledgered reversal (§8.5).

---

## 1. Player loop

1. Recruit any follower — vanilla, modded, framework-managed. MFO detects
   them as a player teammate at runtime (§3) and creates their **Gambit
   Board**: 2 open slots and a starter vocabulary drawn from what that
   specific follower can already do.
2. Fight alongside them. Shared combat earns **Rapport** — MFO's per-follower
   progression currency (§5). Rapport is earned *with a specific follower*
   and is never pooled, traded, or transferred.
3. Open the Board (crosshair a follower, use the **Field Orders** lesser
   power) and author rules: pick a Condition, pick an Action, order them.
   **The topmost matching rule wins**, exactly as FFXII.
4. Rapport ranks unlock more slots and a wider vocabulary. **What unlocks is
   gated by that follower's own skills** — a battlemage unlocks spell
   actions a pure warrior never will.
5. A follower's vocabulary tracks their own skills, so anything that levels
   them — *A Fun Way To Level Followers*, a perk overhaul, a quest reward —
   widens what you can tell them to do, with no patch (§5.4).
6. Watch it run. A follower whose gambits fire well is doing what you told
   them to; one whose gambits fall through is telling you their skills or
   magicka aren't up to the rule you wrote.

The design goal is that **a badly written gambit list is legible**. The
player should be able to look at a follower standing there not casting Heal
Other and understand *why* — insufficient magicka, wrong rule order, a
condition that never becomes true.

---

## 2. How MFO researches what it doesn't know (METHOD)

MFO is the first project in this family to drive actor behavior rather than
items and effects, so it will meet unknown engine mechanisms more often than
its siblings did. It does not improvise when that happens. The family's
documented method, applied in this order:

1. **Ask whether a hook is needed at all.** Most per-actor behavior needs no
   code hook — event sinks and direct engine calls cover it. MFO's Tier A
   (§4.5) contains zero code hooks.
2. **Read the SKSE64 source for the equivalent Papyrus native and replicate
   its engine calls.** If Papyrus can already do a thing, the engine flow
   that does it is discoverable and is the sanctioned implementation. This is
   the standing doctrine and it is how §4.5's Tier B was mapped.
3. **Model technique on verified references, never memory.** Take from a
   maintained reference mod only its *published* Address Library ID +
   offset — a fact about the binary — and write your own thunk. Don't invent
   sites.
4. **Validate the Address Library `.bin` against crash-log ground truth**,
   then verify the site live against `/proc/<pid>/mem`. The on-disk exe is
   encrypted; static verification is worse than useless because it produces
   confident false results.
5. **Instrument and reproduce.** Temporary `spdlog` lines dumping every term,
   then read the log. Strip before release.
6. **When a framework call has no visible effect, read its source.**
7. **Dump a vanilla record and diff, never trust format docs** — for anything
   touching the ESP.

**Named reference sources for MFO's problem domain** (the specific repos this
project will read, following rule 3):

| Source | What MFO takes from it |
|---|---|
| **`ianpatt/skse64`** | The engine call sequence behind every vanilla Papyrus actor native (§4.5 Tier B is enumerated from `Actor.psc`) |
| **`powerof3/PapyrusExtenderSSE`** | Implementations of `GetAllActorPlayableSpells`, `GetCombatAllies`/`GetCombatTargets`, `GetRunningPackage`, `RemoveAddedSpells`, `AddBaseSpell`, the detection getters, `EvaluateConditionList`. Already a family-cited hook source |
| **PapyrusUtil (`ActorUtil`)** | The package-override implementation: add/remove/count, priority semantics |
| **`D7ry/wheeler`** | The in-process ImGui overlay MEO already copied; also the family's only controller-first ordered-list UI |
| **CharmedBaryon/CommonLibSSE-NG headers** | API signatures, verified before a CI round-trip |

**Reverse obligation.** When MFO proves a mechanism, it is written into
`ENGINE_NOTES.md` and then into Linux-Native-Tools **in the same release that
ships it, not afterward.** This project exists at the edge of the family's
knowledge and is the one that owes the knowledge base an actor-AI document.

**A note on how this section came to exist.** An early draft of this design
declared actor AI "unmapped ground no sibling has walked" on the strength of
an empty documentation search. That was wrong twice over: controller support
was already shipped in MEO's `plugin.cpp` but absent from its docs, and the
entire Tier-B vocabulary was one `Actor.psc` read away. **An empty search of
the knowledge base is evidence about the docs, not about the engine.** Grep
the siblings' `native/plugin.cpp`, read the Papyrus surface, and only then
claim something is unproven.

---

## 3. Follower detection and the gambit vocabulary

### 3.1 Detection — framework-agnostic (DECIDED)

MFO takes no dependency on any follower framework and integrates with none.
Eligibility is a runtime property of the actor:

- **Primary test:** the actor is a player teammate AND is in the high
  process. Enumerated from `RE::ProcessLists::highActorHandles`, the MEO
  `ReapplyFollowerSockets` pattern (`plugin.cpp:5485`). The Papyrus
  cross-check is `PO3_SKSEFunctions.GetPlayerFollowers()`, which needs no
  quest alias — the documented fix for "works for player, not followers".
- **Held by `ActorHandle`, re-resolved at act time — never a raw pointer.**
- **`CurrentFollowerFaction` membership is sufficient but not necessary** —
  NFF, AFT, and custom-follower mods manage teammates that never enter it.

**CORRECTED (2026-07-21, from the prior-art survey): `IsPlayerTeammate()`
alone is NOT sufficient either.** An earlier draft called teammate status
"the one signal every framework agrees on." It is not. Reading Swiftly Order
Squad's shipped `IsFollower` / `IsDismissedCustomFollower`
(`LoreRim/mods/Swiftly Order Squad - Follower Commands UI/Source/Scripts/`
`qfcAliasScript.psc`) — a mod that has solved this across a large user
base — the real world is messier:

- **Inigo sets `PlayerTeammate` while following but does NOT clear it on
  dismissal** — he signals that with `GetActorValue("WaitingForPlayer") == -1`
  instead. So the teammate flag alone reports him as an active follower
  forever after you dismiss him. He is one of the most-installed followers in
  existence, so this is not an edge case.
  *(Corrected 2026-07-21 in Fable review: an earlier draft of this section
  claimed he "does not use PlayerTeammate at all". Re-reading Swiftly Order
  Squad's shipped `IsFollower` shows it gates on `IsPlayerTeammate()` and puts
  the Inigo check in the DISMISSAL path — so the flag is necessary, just not
  sufficient. The distinction decides whether quirks grant or revoke.)*
- **Vilja** carries her own `DismissedFollowerFaction`; **Tindra** signals
  dismissal as rank 0 in her own follow faction. Same shape: still teammates,
  dismissal signalled elsewhere.
- **Pet frameworks** use their own faction (`PetFramework_PetFollowingFaction`
  rank ≥ 1) rather than teammate status.

So detection is: **teammate OR a known follow-faction rank, MINUS a
dismissed-state check, MINUS dead / disabled / deleted.** MFO ships the
same shape of quirk table, data-driven from `data/follower_quirks.json` so a
new custom follower is a data edit rather than a code change — and, per
DYNAMIC_OR_DROP, every entry resolves against the live load order and is
skipped silently when its plugin is absent.

**Blocklist by BOTH actor and `GetActorBase()`** (also from Swiftly Order
Squad): blocking by base is what excludes generic followers — hirelings,
summons of a given type — in one entry rather than per-instance.

Consequences, accepted:
- **Summons and conjured creatures are teammates.** Board state keyed to a
  runtime (0xFF) form cannot persist — **never persist a runtime-created
  FormID**. Summons therefore get an ephemeral, session-only board and are
  excluded from Rapport. Gated behind `bAllowSummons` (default OFF for 1.0).
  `PO3.GetCommandedActors(actor)` enumerates them when wanted.
- **Dismissed followers keep their board and their Rapport.** The co-save
  record is keyed on the actor's persistent FormID, not on current teammate
  status. Re-recruiting resumes exactly where you left off — the emotional
  core of the Rapport model, non-negotiable.
- **A framework that hard-overrides AI packages will out-rank MFO's Tier-B
  actuation.** Detected via `GetRunningPackage`/`GetCurrentPackage`
  contention (§4.6); MFO degrades to Tier A rather than fighting.

### 3.2 Conditions — curated core (shipped in the DLL)

Conditions are hand-authored primitives compiled into the DLL from a design
JSON via codegen (`tools/gen_vocab_header.py`, the MEO `gen_catalog_header.py`
pattern — **no runtime JSON parsing in the DLL**). Each names a **subject**,
yields a **target** for the action, and carries a **cost tier** (cheap /
moderate / expensive, §4.2 rule 4) that is part of the shipped vocabulary
data and is surfaced on the board.

| Group | Conditions | Param | Cost |
|---|---|---|---|
| Always | `Always` — the unconditional fallback rule | — | cheap |
| Self | Magicka above X% (a *gate*, not a target) | threshold | cheap |
| Self | Weapon drawn (`IsWeaponDrawn`), sheathed, out of ammo, torch lit | — | cheap |
| Self | I am sneaking (`IsSneaking`) | — | cheap |
| Vitals | Self HP / Magicka / Stamina below X% | threshold | cheap |
| Party | Player is sneaking / in combat / mounted | — | cheap |
| Vitals | Ally HP / Magicka / Stamina below X% | threshold, subject selector | moderate |
| Vitals | Foe HP below X% (execute), Foe HP above X% | threshold | moderate |
| Status | Self/Ally is staggered, paralyzed, poisoned, diseased, bleeding, burning, frozen, shocked | — | moderate |
| Status | Target has / lacks magic effect of archetype | archetype | moderate |
| Range | Target within / beyond N units | N | moderate |
| Kind | Foe is undead / daedra / dwemer construct / dragon / animal / humanoid | — | moderate |
| Stealth | I am detected (`PO3.CanActorBeDetected` / `IsDetectedByAnyone`) | — | moderate |
| Threat | Foe is attacking the player / me / an ally | — | expensive |
| Threat | Foe count within radius >= N | N, radius | expensive |
| Threat | Nearest / furthest / highest-level / lowest-HP foe | — | expensive |
| Kind | Foe is weak to fire / frost / shock / poison | element | expensive |

Cost tiers are a **design commitment measured at P2**, not a guess kept
forever: any condition whose measured cost contradicts its declared tier is
re-tiered in the vocabulary data, and the board reflects it.

**Subject selectors** — who the condition reads: `Self`, `Player`,
`Any ally`, `Lowest-HP ally`, `Nearest foe`, `Player's current target`,
`My current target`. The selector is what makes a condition also a targeting
instruction — the FFXII `Ally: HP < 50%` idiom.

**Engine-provided target sets (RESEARCHED).** Two of the hardest selectors
have purpose-built engine answers rather than hand-rolled scans:
`PO3.GetCombatAllies(actor)` and `PO3.GetCombatTargets(actor)` return the
actor's own combat allies and targets, and `Actor::GetCombatTarget()` /
`GetCombatState()` (0 = not in combat, 1 = in combat, 2 = searching) give the
current-target and engagement predicates directly. Prefer these over walking
`highActorHandles` and re-deriving hostility — they are the engine's own
answer to the same question.

**Open option: reuse the engine's condition evaluator.**
`PO3.EvaluateConditionList(form, actionRef, targetRef)` runs a real CTDA list
against a subject/target pair. If the native equivalent is reachable, a
subset of MFO's conditions could be authored as CTDA data in the ESP and
evaluated by the engine instead of by hand-written predicates — fewer
hand-rolled scans, and mod-added condition functions work for free. **Not
committed**: CTDA evaluation cost per tick is unmeasured, and §4.2's budget
is the binding constraint. Evaluate during P2 with real numbers; the
hand-written path is the default until it loses on measurement.

**RULE (design):** conditions are **pure reads with no engine mutation and no
allocation**. A condition that cannot answer within its tick budget returns
false, never blocks.

### 3.3 Actions — derived from the live actor (DECIDED)

Actions are **not a fixed list**. At board-open and on a debounced refresh,
MFO enumerates what *this actor* can actually do:

- **Cast <spell>** — from **`PO3.GetAllActorPlayableSpells(actor)`**, which
  is precisely this query, rather than a hand-assembled union of added spells
  and race/class lists. Modded spells appear with zero patching. This is the
  load-order-derived half of the vocabulary and the reason MFO needs no
  compatibility patches for spell mods.
- **Drink <potion> — health, stamina, or magicka only** (RULED — marth;
  §4.8.2). From the actor's own inventory, classified by MGEF **archetype**
  (`kValueModifier`/`kDualValueModifier` for real resource effects), never by
  the effect's target actor value. *Requiem note inherited from MRO: elements
  damage different resources (fire→health, frost→stamina, shock→magicka), so
  archetype is the only portable classifier.* Cure/fortify/resist potions are
  deliberately out of the vocabulary — nobody writes those rules, and every
  entry in a picker is a tax on reading it.
- **Shout <shout>** — where the actor has one. MFO never grants shouts; if a
  levelling mod gives them one, it appears here like any other (§5.4).
- **Equip <item>** — weapon/shield/torch swaps from the actor's own
  inventory (§4.5 Tier A). The classic "bow at range, sword in melee" gambit.
- **Positioning / Targeting / Stance** — §4.5 Tier B.
- **Wait** — an explicit no-op that *consumes the tick*, so a rule can
  deliberately suppress lower-priority rules. FFXII players will recognize
  the "do nothing on purpose" idiom; it is load-bearing for authoring good
  lists.

**Serialization RULE (inherited, load-bearing):** a gambit persists its
condition and action as **stable string opcode ids** (`cond.ally_hp_below`,
`act.cast_spell`), never enumeration ordinals — reordering the vocabulary
must not scramble every save's rule lists. Spell/item params persist as
FormIDs passed through `ResolveFormID` on load; **unresolvable → disable that
rule with a log line and a board marker, never guess and never silently drop
the whole list.**

### 3.4 What is deliberately NOT in the vocabulary

- **No mini-language, no user-authored expressions.** An expression evaluator
  over live engine state is the hardest possible thing to make crash-safe and
  cannot be presented in a controller-navigable menu. Composition comes from
  rule *ordering*, as in FFXII.
- **No boolean combinators inside one rule.** Two conditions = two rules. A
  real expressiveness loss, accepted: every rule stays one readable line and
  evaluation stays linear.
- **No cross-follower conditions.** Post-1.0; it multiplies the state each
  tick must snapshot.

---

## 4. The evaluator

### 4.1 Cadence — derived from a response deadline, not chosen (DECIDED — marth)

The tick rate is not a tuning knob picked by feel. It falls out of human
perception, and it resolves into **a band, not a number.**

- Design floor is **30 fps** — the worst framerate MFO must stay correct at.
  One frame = 33.3 ms.
- **Lower bound — simple reaction, ~133 ms.** The fastest human reaction to a
  visual event is ~150 ms. 4 frames at 30 fps = 133 ms, at or under that.
- **Upper bound — complex reaction, ~500–600 ms.** A gambit decision is not a
  simple reaction. It is a *choice* reaction: assess a situation, select among
  alternatives, act. In humans that runs 300–600 ms, several times slower than
  a reflex. **Gambit responses are choice reactions, so the honest target is
  the choice-reaction band, not the reflex floor.**

**133 ms is a FLOOR on response time, never a target (RULE — marth).** Nothing
in MFO may react faster than 133 ms, because a reaction a human cannot
perceive as faster buys nothing. This inverts the usual instinct to poll as
fast as affordable: extra speed here is pure waste.

**And the ceiling is generous, which is where the real saving lives.** A
follower responding in 400 ms is not "late" — it is *more* lifelike than one
responding in 133 ms every single time, because a companion who reacts at
reflex speed to a tactical situation reads as a machine. The band 133–600 ms
is budget to spend, and §4.1's scheduler spends it on scaling (below) rather
than on unnecessary speed.

**The interval is therefore `max(4 frames, 133 ms)` — framerate-adaptive.**
Both terms matter and they govern in opposite regimes:

| Framerate | 4 frames | Interval used | Polls/sec/follower | vs. fixed 4-frame polling |
|---|---|---|---|---|
| 144 fps | 28 ms | **133 ms** (~19 frames) | 7.5 | **4.8× less work** |
| 60 fps | 67 ms | **133 ms** (8 frames) | 7.5 | **2× less work** |
| 30 fps | 133 ms | **133 ms** (4 frames) | 7.5 | same |
| 20 fps | 200 ms | **200 ms** (4 frames) | 5.0 | same — floor governs |
| 15 fps | 267 ms | **267 ms** (4 frames) | 3.75 | same — floor governs |

Two properties fall out of that table, and they are the whole justification:

- **Above 30 fps the absolute cost per second is constant** — 7.5 evaluations
  per follower per second, whatever the framerate. A 144 fps machine does not
  do 4.8× the work of a 30 fps machine for an identical, imperceptible
  result. This is the saving, and it is large.
- **Below 30 fps the evaluator sheds load automatically.** The 4-frame term
  takes over and polling *slows* in wall-clock terms exactly when the frame
  budget is tightest. A plain 133 ms wall-clock timer would do the opposite —
  holding its rate while frames get longer means consuming an ever-larger
  share of each one. The frame floor makes degradation implicit, so §4.2's
  explicit ladder is a backstop rather than the first line of defence.

**Implementation.** MFO already installs a DXGI present hook for the board
(§6.2), which is a natural frame clock: increment an atomic frame counter and
record the frame delta there (render thread, lock-free), and have the
main-thread scheduler read it. No second timing mechanism, no wall-clock
sampling of its own.

**The catch-up trap — a hard rule.** A scheduler that advances its deadline
by `last += interval` in a loop will fire a burst of queued ticks after any
stall: a 5 s load screen becomes ~37 back-to-back evaluations the instant
gameplay resumes, on the exact frame that is already the most loaded. **Fire
at most once per wake and reset `last = now`.** Never accumulate, never catch
up — a missed tick is missed forever, which is correct, because its condition
is re-read from live state on the next one anyway. This is the same shape as
the inherited rule that a timer fired during a long load screen is swallowed,
so load reactivation anchors on `LoadingMenu`-CLOSE rather than a blind timer.

### 4.1a Scaling — followers spread ACROSS ticks, not within one (DECIDED — marth)

The naive reading of "stagger" is that every follower is evaluated every
tick, merely on different frames. That makes per-tick work **O(N)** and means
a ten-follower party costs ten times a solo one. It is also unnecessary,
because of the ceiling above.

**The rule: a tick services a bounded number of followers, round-robin.
Followers are spread across successive ticks, not packed into one.**

- Tick cadence `T = max(4 frames, 133 ms)` — the heartbeat, unchanged.
- Each tick services **K followers**, K small and bounded (default 1).
- A follower's effective response interval is therefore `(N / K) · T`.
- **K is raised only to keep that interval inside the choice-reaction
  ceiling** — never to make anyone faster.

| Followers | K | Effective per-follower interval | Verdict |
|---|---|---|---|
| 1 | 1 | 133 ms | reflex-fast, the floor |
| 2 | 1 | 266 ms | inside the band |
| 3 | 1 | 400 ms | inside the band, feels deliberate |
| 4 | 1 | 533 ms | at the ceiling |
| 5–8 | 2 | 333–533 ms | K raised once to stay under it |
| 9–12 | 3 | 400–533 ms | K raised again |

**Work per tick is O(K), not O(N).** A large party costs a constant per tick
and pays in response time — the correct currency, and one the ceiling gives
us room to spend. This, not the frame-rate adaptation, is what makes MFO safe
in a party-framework load order with a dozen teammates.

**Missed ticks are free, and this is a structural property, not a
tolerance.** The evaluator holds **no state between ticks**: every tick
re-reads live actor and world state and re-evaluates from rule 1. There is no
queue, no backlog, no accumulated intent, nothing that can be "behind." A
follower skipped this tick loses exactly nothing — the next tick asks the
same questions of fresher state and reaches a better answer than the stale
one would have been. That is what licenses round-robin servicing, load
shedding (§4.2), and the no-catch-up rule above; all three are safe for the
same reason.

Corollary for prioritisation: because skipping is free, the round-robin may
be **weighted** rather than strictly cyclic — followers in combat serviced
ahead of idle ones, and a follower whose condition fired recently deferred in
favour of one that has waited longest. Simple aging, no scheduler complexity.
Out of combat the whole question is moot: the interval relaxes to ~1 s and N
barely matters.

### 4.1b Deviation — companions must not fire like engine cylinders (RULE — marth)

A correct scheduler is not yet a believable one. **Regularity is the tell**,
more than latency is: a party whose members respond at fixed intervals in a
fixed order reads as machinery no matter how good the numbers are. Three
distinct sources of regularity exist here, and they need different fixes.

**1. Fixed phase — a follower always answering at the same offset.** Partly
solved for free: a condition becomes true at an arbitrary moment between that
follower's ticks, so observed latency is already spread across `[0, interval]`.
That is genuine variance, not simulated. But a *sustained* condition still
gets noticed at the same phase every cycle. **Fix: jitter each follower's
next-tick time by a per-cycle random offset** (±25% of the interval, clamped
so the effective interval never drops below the 133 ms floor or past the
choice-reaction ceiling).

**2. Strict rotation — A, B, C, A, B, C.** This is the literal cylinder
firing order and it is the most damning of the three, because the player sees
the *pattern between* companions, not just each one's timing. **Fix: the
round-robin shuffles its service order every cycle** rather than cycling
deterministically, with the §4.1a aging weights biasing selection rather than
dictating a sequence.

**3. Metronomic repetition on a sustained condition.** A rule that stays true
must not re-fire on a perfect beat. **Fix: the §4.4 suppression window is
itself jittered** (±30%), so repeated actions drift rather than tick.

**Jitter the OBSERVATION, never queue the ACTION.** The tempting
implementation — match a rule, then schedule the action after a random
reaction delay — introduces a pending action, which is exactly the state
between ticks that §4.1a's statelessness forbids, and it re-opens the
catch-up and stale-intent problems that property closes. Instead, randomize
*when the follower looks*; they act immediately on what they see. This is
also the more accurate model: human reaction variance comes mostly from
attention and noticing, not from motor execution.

**Shape the distribution, don't just add noise.** Human reaction times are
not uniform — they are right-skewed, with a hard floor, a mode near it, and a
long tail (the ex-Gaussian shape in the RT literature). Uniform jitter is
itself a detectable signature: too few fast responses, too few slow ones.
Draw from a floored right-skewed distribution so most responses cluster
early-to-middle and occasional ones lag — the follower who *didn't notice for
a moment* is the single most human thing this system can produce.

**Urgency modulates variance, and this is the part that sells it.** Not every
decision deserves the same spread. A follower about to die reacts fast and
consistently; one deciding whether to swap weapons can dawdle. Each condition
group carries an urgency weight that narrows or widens its draw:

| Urgency | Conditions | Spread |
|---|---|---|
| Critical | self/ally HP low, paralyzed, about to die | tight, near the floor |
| High | foe closing, execute range, status to cure | moderate |
| Routine | weapon swap, buffing, repositioning | wide, tail-heavy |

**Competence shifts the distribution — and this is where Rapport becomes
felt.** A follower's reaction spread narrows and its floor drops as Rapport
rank rises (and with relevant skill). A Rank I companion hesitates and
occasionally misses a beat; a Rank V one who has fought beside you for a
hundred hours anticipates. Slot counts are an abstract reward; **reaction
quality is a reward the player perceives without being told about it**, and
it costs nothing to implement — it is one parameter on a distribution that
already exists.

**Decision complexity does NOT add latency — Hick's law is REJECTED (RULE —
marth).** The realistic version of this system would scale reaction time with
the number of alternatives, so a 12-gambit follower deliberates longer than a
2-gambit one. It is rejected on progression grounds, and the reasoning
generalizes: **slots are a Rapport reward, so spending them must never carry a
penalty.** A reward that degrades the thing it rewards is not a reward. A
Rank V follower with a full board is the payoff for a hundred hours of shared
combat and must feel *better* in every dimension than the Rank I version of
themselves, never slower.

It is also unnecessary architecturally. First-match-wins (§4.3) means
evaluation cost is proportional to **the position of the matching rule, not
the length of the list** — a 12-rule list whose second rule fires costs
exactly what a 2-rule list costs. Long rule lists are not inherently
expensive, so there is no performance argument for a length penalty either.
Reaction spread is set by competence and urgency alone.

**What must never be jittered:** the 133 ms floor is absolute (§4.1); jitter
only ever delays, never accelerates. And deviation is not an excuse for
missed reactions — every jittered draw stays inside the choice-reaction
ceiling, so "lifelike" never becomes "unresponsive."

Out of combat there is no reaction-time requirement (nothing is trying to
kill anyone), so the interval relaxes to **~1 s / 30 frames**. Followers in
the low process, in an unattached cell, dead, in dialogue, or inside the
suppression window (§4.4) are **not scheduled at all**.

A native timer queues an `AddTask`; evaluation runs on the main thread.

### 4.2 Performance — the budget, and why this is MFO's novel risk

**MFO is the first project in this family with a polling architecture.** MRO,
MEO, and MAO are entirely event-driven: the engine announces a kill, a hit, a
container change, and work happens in response. Their idle cost is
structurally zero. MFO's evaluator does **unconditional work on a timer**, per
follower, forever. That is a different risk class and it gets designed for
explicitly rather than measured after the fact.

The family already paid for the adjacent lesson once. MRO registered a global
`RegisterForActorAction` on a high-frequency action and it taxed the whole VM
on a large list — and the finding that matters is **the cost was the
dispatch, not the handler body, so filtering inside the handler did not
help.** The MFO translation: *a rule that is cheap to evaluate is not free to
schedule.* Cost is controlled by not running, not by returning early.

**The budget.** Anchored to the 30 fps floor from §4.1: one frame is 33.3 ms,
and MFO's claim on it is **under 1%**. Targets, to be replaced by
measurements at P2:

| Quantity | Budget | As a share of a 30 fps frame |
|---|---|---|
| Evaluator wall time on a **tick** frame | **< 0.30 ms** | < 0.9% |
| One follower's evaluation | < 0.10 ms | < 0.3% |
| Shared world snapshot, on the ticks it is built | < 0.20 ms | < 0.6% |
| Followers serviced per tick | **K**, default 1, raised only per §4.1a |  |
| Evaluator wall time on a **non-tick** frame | **0** — nothing runs | 0% |

The last row is the one that matters. At 30 fps a tick lands on 1 frame in 4;
at 60 fps, 1 in 8; at 144 fps, 1 in ~19. **The overwhelming majority of frames
execute no evaluator code whatsoever** — not a cheap early-out, not a
predicate that returns false, literally nothing scheduled. Combined with
§4.1a's O(K) tick, total cost is independent of both framerate and party
size, which is the property the whole cadence design exists to produce.

**Five structural rules that keep it there:**

1. **Never evaluate what cannot act.** Low process, unattached cell, dead,
   disabled AI, in dialogue, or inside the suppression window (§4.4) → the
   tick is not scheduled at all. The suppression window is a *performance*
   feature as much as a behavior one: a follower who just acted costs nothing
   for 1.5s.
2. **One shared world snapshot per frame, never per follower and never per
   rule.** Foe enumeration is a single bounded walk, distance-filtered, built
   once and read by whichever follower evaluates that frame. Prefer
   `GetCombatTargets`/`GetCombatAllies` (§3.2) over hand-rolled scans — the
   engine already maintains those sets.
3. **Spread across ticks, never batch within one** (§4.1a). Per-tick work is
   O(K), not O(N). A batch of ten followers evaluating on one frame is a
   visible hitch even when the total work is identical, and it is also
   unnecessary — the choice-reaction ceiling gives room to serve them in
   turn. **Party size is paid for in response time, not framerate.**
4. **Conditions are cost-classed, and the class is part of the vocabulary.**
   Each condition carries a static cost tier — **cheap** (a single actor-value
   or flag read on a known subject), **moderate** (one active-effect or
   inventory walk), **expensive** (anything that needs the world snapshot or
   a set intersection). The evaluator computes nothing until a rule asks for
   it: the world snapshot is built **lazily on first expensive predicate this
   frame**, so a list of only cheap rules never pays for it.
5. **First-match-wins makes rule order a performance contract, and the
   player writes it.** A player who puts an expensive condition in slot 1
   pays it every single tick; the same condition in slot 6 is only reached
   when five cheap rules have already failed. This is a real consequence of
   §4.3 and MFO does not hide it — **the board displays each condition's cost
   tier**, so an expensive rule sitting at the top is visible, the same way
   the failure reason in §5.3 is visible. Teaching the player to put cheap
   discriminating rules first is both better authoring and cheaper execution.
   MFO never silently reorders rules to optimize: order is the program (§4.3)
   and reordering it behind the player's back would change behavior.

**Degradation, not collapse.** §4.1's frame floor already sheds load
implicitly below 30 fps, which handles the common case without any explicit
logic. The ladder below is the backstop for when that is not enough — a
follower count high enough that `N / P` breaches the cap of 2, or a measured
overrun of the per-frame budget:

1. **Hold K at 1 and let the round-robin lengthen** — the effective interval
   drifts past the choice-reaction ceiling for large parties. Cheapest step,
   and for most overruns the only one needed.
2. **Extend the tick itself** — 4 → 8 → 16 frames. Costs response time for
   everyone, which is the correct thing to spend when the alternative is
   framerate.
3. **Suspend Tier-B actuation** (§4.5) — the positioning and targeting calls
   are the most expensive and the least essential.

Each step logs once, with the measured number that triggered it and the step
it moved to, and logs again when it recovers. A silent slow mod is worse than
a loud degraded one — and per the inherited doctrine, **a guarded path must
log its zero case too**, or "never ran" and "ran and found nothing" look
identical.

**Measurement is a phase gate, not a follow-up.** P2 does not pass on "feels
fine." Per the inherited instrumentation doctrine — *when a computed number
is wrong, log every term* — the evaluator ships with a debug timing mode
(`bProfileEvaluator`) that logs per-tick wall time split by phase: snapshot
build, predicate evaluation, actuation. The numbers are taken **in a
Lorerim-class load order during a real fight with multiple followers**, not
in an empty test cell, because that is the only configuration where the
answer is meaningful. Those measurements also settle §3.2's open CTDA
question on evidence rather than preference.

**RULE:** if the snapshot cannot be built (actor unresolvable, process lists
unavailable), the tick is **skipped**, not partially evaluated. A half-built
snapshot answering predicates is the fabricate-from-garbage failure mode the
co-save invariants exist to prevent, moved into gameplay.

### 4.3 First match wins

Rule 1 is checked, then 2; the first true condition fires its action and the
tick ends. Rules below never see that tick. **Ordering is the program** — and
per §4.2 rule 5, ordering is also the cost.

**Exactly one action per follower per tick — the FFXII contract.** No
scoring, no best-match, no tie-breaking: rule 1 beating rule 2 *is* the
mechanism. A follower cannot heal and attack in the same cycle, and the cost
of a heal is the attack not made. Multi-action ticks would turn slot count
into a raw power multiplier and break §4.2's budget.

**And the scan restarts from rule 1 every tick** — no program counter, no
resumption, no memory of where the previous scan stopped. That is why
priority is *continuously re-asserted* rather than decided once at selection
time, and it is what §4.4's positional preemption preserves: a rule the
player ranked first outranks a running rule ranked sixth, every tick, without
MFO needing an opinion about which is more important.

### 4.3a The rule list is the player's, and MFO does not second-guess it (RULE — marth)

The evaluator executes the list as written. It does not optimize, coordinate,
dedupe, or economize on the player's behalf. Three consequences, all
deliberate:

- **No silent reordering** for performance (§4.2 rule 5). Order is the
  program; changing it changes behavior.
- **No cross-follower coordination, and redundant actions are not a bug.**
  Two followers with the same healing rule will both notice the same wounded
  ally and both heal. **That is the player's authoring, and the wasted heal
  is the player's to fix** — by giving one of them a narrower condition, a
  different subject selector, or a lower priority. MFO surfaces what happened
  (§5.3's per-rule outcome) and stops there. Anything else means the mod
  quietly disagreeing with an instruction it was given, which is the one
  thing a programmable system must never do.
- **No penalty for a full board** (§4.1b). Slots are earned; using them is
  free.

This is the same contract as FFXII and the same one §5.3 makes about
competence: **a badly written gambit list is legible, not corrected.** The
system's job is to make the failure visible and the fix obvious, never to
paper over it.

### 4.4 Layering over vanilla AI (DECIDED — marth)

**Gambits layer on top; they never replace.**

- **No match → MFO does nothing at all this tick.** Not a no-op package, not
  a neutral command — literally no engine call. A follower with an empty or
  entirely-false list behaves *byte-identically to one without MFO
  installed*. This is the compatibility guarantee and it is testable by diff
  against a baseline save.
- **A match issues the action and opens a suppression window** sized to that
  action — a cast suppresses for its cast time, a potion for its animation,
  an equip swap for almost nothing (`fSuppressWindow` is the fallback,
  jittered ±30% per §4.1b so repeats drift rather than tick). It stops a
  follower being jerked between two rules that alternate truth each tick.
- **Suppression is POSITIONAL, not absolute — a higher rule always preempts
  (RULE).** During the window, rules **above** the one that fired may still
  fire; rules at or below its position may not. This is FFXII's semantics
  exactly: the list is re-scanned from the top every tick and priority is
  re-asserted continuously, so a rule the player ranked first is never made
  to wait out a rule they ranked sixth. It also kills thrashing for free —
  two rules alternating at positions 3 and 4 cannot fight, because 4 can
  never preempt 3 — and it does so **without the mod substituting its own
  judgement for the player's ordering** (§4.3a).
- **The window is an MFO-side cooldown, not an AI lockout.** Vanilla AI runs
  throughout.
- **Tier-B actions hold the AI only for the duration of the action, then
  release it explicitly.** No indefinite package override ever ships.

### 4.5 Actuation tiers

**Tier A — PROVEN (ship first; 1.0 core).** Each has a working call pattern
in a sibling's shipped code:

| Action | Mechanism | Precedent |
|---|---|---|
| Cast a spell | `ActorMagicCaster::CastSpellImmediate` | MEO Echo follower-share; MAO flask payload |
| Teach / unteach | `Actor::AddSpell` / `RemoveSpell` | MEO startup grants |
| Remove an effect | `ActiveEffect::Dispel(true)` — **collect the full list before dispelling, never mid-walk** | MEO `DispelStaleGemEffects` |
| Equip / swap gear | `ActorEquipManager::UnequipObject` → `EquipObject`; hand slots from `BGSDefaultObjectManager` | MEO worn-ability cycle |
| Drink a potion | the equip path on an `AlchemyItem` | MAO consume intercept |
| Read actor state | `AsActorValueOwner()`, `HasSpell`, `HasPerk`, active-effect walk | all three siblings |

**Tier A's inherited landmine, which MFO steps on constantly:**
equip/unequip dispatch is **synchronous into every registered sink** —
cycling gear on a follower re-enters follower AI and third-party outfit
managers (guaranteed in a Lorerim-class order) which mutate the same
inventory. MEO ate a node use-after-free here. Snapshot tuples first, act
second, re-find by key, hold actors by handle. The unequip→equip cycle is
**idempotent** by design — repeated passes cannot accumulate.

**Tier B — RESEARCHED, not yet run.** Every action below has a documented
Papyrus native, which per §2 rule 2 means the engine flow is discoverable and
sanctioned. These are not blind spots; they are unwritten milestones.

| MFO action | Papyrus native (the map) | Notes |
|---|---|---|
| Formation / follow spacing (**out of combat only**) | `Actor.KeepOffsetFromActor(target, x,y,z, angX,angY,angZ, catchUpRadius, followRadius)` + `ClearKeepOffsetFromActor()` | **NOT a combat tool — see §4.5a.** Reversible, no package needed, but must be torn down the moment combat starts |
| Hold position | `KeepOffsetFromActor(self, 0,0,0)` — offset an actor from *itself* to pin it (Simply Order Summons' idiom), or `SetDontMove(bool)` | Both reversible |
| Combat positioning / engagement | **A conditioned PACKAGE, driven by a GlobalVariable or ActorValue — see §4.5a** | The sanctioned route. Not `KeepOffsetFromActor` |
| Focus target / break off | `Actor.StartCombat(target)`, `StopCombat()`, `GetCombatTarget()` | |
| Combat-aware cast | `Actor.DoCombatSpellApply(spell, target)` | A combat-context cast; likely a better fit than `CastSpellImmediate` for gambit casts. `PO3.LaunchSpell(actor, spell, source)` is the projectile-level alternative |
| Assign hands | `Actor.EquipSpell(spell, source)` (0 = left, 1 = right), `EquipShout(shout)` | |
| Look at | `Actor.SetLookAt(target, pathingLookAt)` / `ClearLookAt()` | Cheap, cosmetic, high perceived value |
| Stance | `Actor.StartSneaking()`, `DrawWeapon()` | |
| Package control | `Actor.EvaluatePackage()`, `GetCurrentPackage()`, `PO3.GetRunningPackage()`, PapyrusUtil `ActorUtil.AddPackageOverride(actor, pkg, priority 0–100, flags)` / `RemovePackageOverride` / `CountPackageOverride` | **Last resort only** — see the three rules below |
| Combat observation | `TESCombatEvent` (== `OnCombatStateChanged`: 0 none / 1 in combat / 2 searching); `OnPackageStart` / `OnPackageChange` / `OnPackageEnd` | |

**Three hard rules discovered while mapping Tier B. All are load-bearing.**

1. **`PathToReference` is LATENT and must never be called from the
   evaluator.** Its own documentation: *"this method doesn't return until the
   goal is reached or pathing failed or was interrupted."* A latent call on
   the tick path would stall the main thread for the duration of a walk.
   Positioning uses `KeepOffsetFromActor`, which is a state set, not a
   journey. **`PathToReference` is banned from MFO outright** — there is no
   safe caller for it in this architecture.
2. **Package overrides PERSIST THROUGH SAVES.** PapyrusUtil states it
   plainly. That directly threatens §8.5's clean-uninstall promise: an
   override left on a follower outlives the mod. Therefore — **every override
   MFO adds is ledgered in the co-save**, is
   removed by `RemovePackageOverride` when its action ends, and is reconciled
   against `CountPackageOverride` every load. An override with no ledger
   entry is a bug, and MFO logs it.
3. **`ClearPackageOverride` is BANNED.** It *"remove[s] all package overrides
   on this actor, including ones that were added by other mods."* Calling it
   would silently break every follower framework sharing the actor. MFO
   removes only its own, by handle, one at a time. This is the
   `RemoveByType`-class lesson arriving before the crash rather than after.

### 4.5aa THE BINDING GAP — Tier B's natives are not in the library (2026-07-21)

**Found by Fable review of the M4 harness, before it ever ran.** Five of the
primitives §4.5 lists as Tier B have **no C++ binding in CommonLibSSE-NG**:

| Primitive | Reality |
|---|---|
| `KeepOffsetFromActor` / `ClearKeepOffsetFromActor` | **Papyrus-only.** Not in NG, not in po3's fork. Only `RTTI_IMovementSetKeepOffsetFromActor` exists |
| `SetDontMove` | **Papyrus-only.** Not bound anywhere |
| `DoCombatSpellApply` | **Papyrus-only.** Not bound anywhere |
| `StartCombat` | **po3's fork only**, as a relocation thunk (`RelocationID(37608, 38561)`, SE/AE — no sourced VR id) |

**§4.5a is not wrong — it is incomplete.** The Papyrus surface named the right
engine flows, and the research method that found them (read the native, call
the same flow) still holds. What it could not tell us is whether the *library*
exposes them, and it does not. "There is a Papyrus native" and "I can call it
from C++" are different claims, and only the first was verified.

**Two routes, and they are different mechanisms with different risks:**

1. **Papyrus VM dispatch** — `DispatchMethodCall2` on the actor's handle.
   Lands on the next VM frame rather than immediately, so it is not a
   drop-in for a synchronous call. **Measurement-wise this is the honest
   route**: the precedent MFO is imitating (Kaidan, Swiftly Order Squad) *is*
   Papyrus, so this probes the same mechanism M9 would ship.
2. **Sourced relocation** — take a maintained reference's *published* Address
   Library ID, as with po3's `StartCombat`. Fast and synchronous, but it
   probes a **different** mechanism than the Papyrus precedent, and an
   unsourced ID is exactly what the family's hook doctrine forbids.

**This is its own milestone, not a footnote in M4.** M4 ships only what the
pinned library verifiably binds, and shows the unavailable primitives in the
UI rather than omitting them — a probe that hides what it cannot reach hides
its most important finding.

**Consequence for §4.7 and the roadmap:** the standing-order model still rests
on `StartCombat`, which *is* reachable via po3's published ID, so the retention
question can be answered now. Positioning (`KeepOffsetFromActor`) cannot be
tested until the VM-dispatch route exists — so **§4.5a's positioning
conclusions remain RESEARCHED, and cannot be promoted by M4.**

### 4.5a Tier B corrections from the prior-art survey (2026-07-21)

Reading shipped follower code — chiefly **Immersive Kaidan AIO**
(`Authoria/mods/Immersive Kaidan AIO - rerun/scripts/source/`), the only
complete follower-command framework in ~41k Papyrus scripts across the
installed lists — corrected several Tier-B assumptions. These are not
refinements; two of them invalidate what the design said.

**1. `KeepOffsetFromActor` FIGHTS THE COMBAT CONTROLLER. It is a follow tool,
not a combat tool.** Kaidan's `kaidanfollowaliasscript.psc` tears down every
movement override the instant combat begins:

```papyrus
Event OnCombatStateChanged(Actor tgt, int cst)
    if cst == 1 || cst == 2          ; in combat / searching
        KaidanREF.ClearLookAt()
        KaidanREF.ClearKeepOffsetFromActor()
        KaidanREF.EvaluatePackage()
```

A live offset during combat produces the classic *"follower moonwalks around
the enemy"* bug. The design named `KeepOffsetFromActor` "the preferred
positioning primitive" — **wrong for the case MFO most cares about.** It
governs formation and spacing out of combat; combat engagement must go
through a package.

**2. The sanctioned Tier-B architecture is DECLARATIVE: conditioned packages
driven by globals/ActorValues, not `AddPackageOverride`.** Kaidan keeps ~20
GlobalVariables (`KaiFollowDis`, `Kaidan_ShouldApproachTarget`, …); packages
are conditioned on them in the CK, and script only sets a global and calls
`EvaluatePackage()`. Package *overrides* appear exactly once in that mod, and
only as a **lock** (a do-nothing package at priority 90 to freeze an actor
during an animation) — never as behavior.

MFO adopts this: **ship our own conditioned packages in `MFO.esp`, drive them
by setting a global/AV and calling `EvaluatePackage()`.** It is reversible by
construction, it cannot strip another mod's overrides, and the AI stack stays
in charge of execution. Package overrides drop to last resort, unchanged from
§4.5's ordering.

**3. `EvaluatePackage()` NO-OPS if the package it would select is already
current.** To force a re-selection you must flicker the condition:

```papyrus
Kaidan_ShouldApproachTarget.SetValue(0)
KaidanRef.EvaluatePackage()
Utility.WaitMenuMode(0.1)      ; forces the transition to register
Kaidan_ShouldApproachTarget.SetValue(1)
KaidanRef.EvaluatePackage()
```

Any MFO action that re-targets the same package needs this dance. Unknown
whether the native path needs the delay — **verification queue item.**

**4. `KeepOffsetFromActor` does not survive the actor leaving the high
process.** Simply Order Summons re-applies on `OnCellAttach`/`OnCellDetach`.
MFO's equivalent: re-assert any movement state on cell attach, and treat it
as lost otherwise.

**5. An actor mid-recoil or mid-stagger cannot act.** Every VIGILANT boss
script guards each cast with
`getAnimationVariableBool("IsRecoiling")` / `"IsStaggering"`. **This becomes
an actuation precondition** — an action issued into a stagger is silently
eaten, which would present to the player as "my gambit didn't fire" with no
reason attached, defeating §5.3.

**6. Always `StopCombat()` before `StartCombat()`, and preserve aggression
around a forced target switch.** The taunt idiom, from
`AK69TauntTargetsScript.psc`:

```papyrus
float anger = akTarget.GetActorValue("Aggression")
akTarget.SetActorValue("Aggression", 0)
akTarget.StopCombat()
akTarget.StartCombat(akCaster)
akTarget.SetActorValue("Aggression", anger)
```

**7. `aeCombatState == 0` is not trustworthy on its own.** `There Is No
Umbra` waits and re-reads both actors' combat state before believing combat
ended. MFO's combat-exit Rapport award (§5.1) must do the same or it will
fire mid-fight.

**8. Nobody in the corpus calls `RemovePackageOverride`** — every mod uses
the destructive `ClearPackageOverride` instead. `INVARIANTS.md` #18 still
stands, but note MFO would be **without precedent** there; the header is the
only documentation.

**Also adopted:** the ordered panic-reset sequence (stop combat → clear
globals → clear offsets/look-at → stop scenes → clear overrides → force idle
→ `EvaluatePackage`) as the model for MFO's uninstall and per-follower reset;
and `WaitingForPlayer` AV + `EvaluatePackage()` as the vanilla-sanctioned
wait/hold mechanism (Swiftly Order Squad), which works *with* the follower
package rather than against it.

**Sequencing doctrine for Tier B.** Land Tier A completely and ship a
playable 1.0 candidate on it alone — a gambit system that casts, heals,
drinks, and swaps gear is already the mod. Then take Tier B **one mechanism
per release**, each its own milestone, instrumented rather than eyeballed,
so a CTD bisects to one change. Prefer, in order: (a) reversible state sets
(`KeepOffsetFromActor`, `SetDontMove`, `SetLookAt`), (b) combat-state calls
(`StartCombat`/`StopCombat`), (c) package overrides — last, because they are
the only ones that persist. Any mechanism that cannot be made to release the
AI cleanly is **DROPPED, not shipped half-working** (the MRO Boss Readiness
precedent: "removed rather than shipped half-accurate").

**Design consequence:** the board renders a Tier-B action greyed with a
reason string when the DLL reports its mechanism unavailable. A 1.0 shipping
Tier A only is a *complete* mod with a smaller verb list, not a broken one.

### 4.5b The equip policy — how a gambit spell actually gets cast (DECIDED — marth, 2026-07-21)

**The problem.** `CastSpellImmediate` applies an effect with NO animation, from
ANY casting source (ENGINE_NOTES §0.8, §0.10 — tested, refuted). The reason is
structural: `ActorMagicCaster` inherits `SimpleAnimationGraphManagerHolder` and
sinks `BSAnimationGraphEvent`, so a real cast is *driven by the animation
graph*. `CastSpellImmediate` is the trap/script path and bypasses it (§0.13).

**Therefore MFO must make the follower cast, not cast on the follower's
behalf** — which is what §4.4's layering doctrine said all along. The vanilla
flow is already fully animated; every enemy mage in the game uses it.

**The cost, and the whole design problem:** equipping a spell consumes a hand.

#### The ruling

**The off-hand is the pivot. A spell swapped into the off hand is essentially
free** — the follower keeps their weapon, keeps fighting, and the animation is
the real one.

| Follower is holding | Policy |
|---|---|
| Spell already equipped, or is a caster | **Nothing to do.** Free — vanilla AI casts it animated |
| **One-handed weapon** (right hand) | **Equip the spell to the OFF HAND.** Free; the weapon is untouched |
| One-handed + **shield** | Equip to off hand, displacing the shield. **The shield is restored when the follower TAKES A HIT** — not immediately |
| **Two-handed** weapon (incl. bow/crossbow) | Equip/restore, **with a debounce.** The weapon must leave both hands, so swapping is expensive and must not thrash |
| Empty hands | Equip to off hand |

#### The backstop (AMENDED after review, 2026-07-21)

Hit-only restore has a hole: a follower who is never hit again — the fight
ends, the player tanks, the follower is ranged — keeps the shield benched
forever. **§4.5b's first invariant outranks its own optimization**, so the
shield also comes back when COMBAT ENDS, and on dismissal. A shield is deferred,
never abandoned.

#### Why the shield restores on a hit, not on a timer

A shield's only job is to matter when something hits you. Restoring it
immediately after every cast produces a swap-storm — equip, cast, unequip,
re-equip — for a benefit nobody experiences until they are attacked. Deferring
the restore to the moment the follower actually takes damage means the shield
is back exactly when it starts mattering, and costs nothing the rest of the
time. This needs a `TESHitEvent` sink.

#### Why two-handed gets a debounce and one-handed does not

The off-hand swap leaves the follower armed and fighting, so it can happen as
often as the rules want. A two-handed wielder must *stow their weapon entirely*
to cast: it is visible, it interrupts their attack, and a rule that fires often
would leave them permanently sheathing and drawing. The debounce is a floor on
how often MFO is willing to pay that, independent of the rule's own suppression.

#### Invariants this creates

- **MFO restores what it displaced.** A follower who loses a shield to a gambit
  gets it back. Anything else is "the mod ate my follower's gear".
- **The ledger is transient, never persisted.** It records a live loadout, and a
  loadout is engine state; see INVARIANTS #16.
- **One unpaid debt at a time.** If MFO already owes a follower gear, it takes
  nothing else from them. Overwriting a ledger entry orphans the first item
  permanently — and the second entry can be a *spell*, which would then be
  "restored" with the wrong engine call entirely.
- **A follower already holding a spell is left alone.** Their hands are their
  own (or MFO's from last tick); rearranging them is how the orphan above
  happens.
- **Never `AddBaseSpell`** (#20). If a follower does not know the spell, the
  rule FAILS with a reason (§5.3 — competence is not permission). MFO does not
  grant spells; that is out of scope (§5.4).

### 4.5c THE ACTUATION ARCHITECTURE — a package IS the action (RULED — marth, 2026-07-22)

**Every single-action gambit is executed as a package.** Not casting only —
casting, attacking, moving, holding position, using an item. One mechanism.

#### The doctrine question, and its answer

§4.4 says *layer on top of vanilla AI, never replace it*, and a package **takes
over the actor for its duration**. That looks like a violation. It is not, and
the distinction is exact:

> **Taking over for the duration of an action enforces the VALIDITY of that
> action. It does not replace the follower's AI.** — marth

Vanilla AI does precisely this. When the engine's combat controller decides to
cast, it owns the actor until that cast completes; nothing else interrupts it.
MFO issuing an action the player's list chose, and owning the actor exactly as
long, is **symmetric with the engine's own behaviour** — not an escalation of it.

**What still layers, and this is what keeps the doctrine intact:**

* The **choice** of what to do is MFO's rules sitting on top of vanilla — it
  never replaces the follower's own decision-making, it preempts it for one
  action.
* **Between actions the follower is entirely their own.** The package pops the
  moment the action completes or the rule stops winning.
* MFO owns **the action**, never **the follower**.

#### Why this and not the alternatives

An action vanilla AI can interrupt mid-flight is not an action, it is a
suggestion — and a gambit is a commitment. Every non-package mechanism MFO tried
is exactly that suggestion:

| Mechanism | Refuted because |
|---|---|
| `CastSpellImmediate` (all 4 sources) | applies an effect, no animation, AI never involved |
| `Projectile::LaunchSpell` | no projectile on a self-delivery spell; bypasses the caster |
| `DoCombatSpellApply` | the Papyrus twin of the first — instant, silent |
| Animation events | the graph EMITS them; nothing can send them |
| Equip + let the AI cast | works, but **AI-discretionary** (§0.16) — it declined a weak heal every time |
| Driving `MagicCaster` directly | wedges at state 1 without a package to run it |

The package is what remains, and it is the only one that makes the action
*happen* rather than *hoping* it happens.

#### Consequences

* **§4.5a's "package overrides are a last resort" is superseded for actuation.**
  The caution stands for anything persistent; a per-action package that pops
  immediately is a different thing from a standing override.
* **Framework contention (§4.6) becomes the primary compatibility risk**, not a
  footnote. MFO pushes packages onto followers managed by NFF, Inigo and others.
  It must restore exactly what it displaced, and announce contention in the log.
* **The action must be bounded.** A package that does not pop is MFO owning the
  follower, which IS the violation. Every action gets a completion condition and
  a hard timeout.

#### What this does to the roadmap

M9 stops being "the casting fix" and becomes **the actuation layer** — the
foundation the rest of the vocabulary is built on, not a patch beside it.

### 4.6 Framework contention

`GetRunningPackage` / `GetCurrentPackage` tell MFO what is actually driving
the actor. When another system holds a higher-priority override, MFO
**degrades to Tier A and says so on the board** rather than escalating
priority. Escalation wars with follower frameworks are unwinnable and the
loser is the user's save.

### 4.7 Standing orders and target commitment (DECIDED — marth)

Until now the design treated every action as a one-shot. That is wrong for a
whole class of them. **Actions divide into two kinds, and they need different
lifecycles:**

| Kind | Examples | Lifecycle |
|---|---|---|
| **Transient** | cast a spell, drink a potion, swap weapon | Fires once. Governed by the §4.4 suppression window |
| **Standing order** | attack *this* target, hold position, keep distance | **Issued once, then persists.** Governed by a commitment latch — re-issued only when invalidated |

A rule like `Always -> Attack lowest-HP foe` must **not** re-issue every tick.
Re-issuing means `StopCombat`/`StartCombat` churn several times a second,
which resets the combat state the follower is trying to act on and produces
exactly the stuttering, never-actually-attacking behavior the whole system
exists to avoid. The order is given once; vanilla AI executes it
uninterrupted; MFO stays quiet until something makes the order wrong.

#### 4.7.1 The mechanism

Per §4.5a, targeting goes through the **declarative** route, not a package
override:

1. `ForceRefTo` the chosen actor into MFO's per-follower **combat-target
   alias** (`MFO.esp` ships a fixed pool; see §8.2).
2. Set that follower's `MFO_HasCommandedTarget` global/AV.
3. `EvaluatePackage()` — and if the same package is already current, use the
   §4.5a flicker (set 0 → evaluate → set 1 → evaluate), because
   `EvaluatePackage()` no-ops otherwise.
4. Kick combat with `StopCombat()` → `StartCombat(target)`, preserving
   aggression across the switch (§4.5a rule 6).

Release is the same in reverse: clear the alias, unset the global,
`EvaluatePackage()`. **Release is mandatory and ledgered** — a follower left
latched onto a corpse is the failure mode this section exists to prevent.

#### 4.7.2 The latch, and why it does not violate statelessness

Actuation holds, per follower:

```
CommandedTarget { ActorHandle target; uint8 issuedByRule; uint32 issuedAtTick; }
```

**The order persists exactly as long as that rule keeps winning with the same
resolved target. Nothing more is going on than that.** Each tick the scan
runs top-down from rule 1 exactly as before, produces a winner, and the
winner resolves to an `(action, target)` pair. Then:

- **Identical to what is already commanded, and still valid → NO ENGINE CALL
  AT ALL.** The tick ends having done nothing.
- **Anything else → issue the new order, which supersedes the old.**

There is no separate preemption machinery, because none is needed. A
different rule winning *is* the mechanism:

```
Tick 40:  rule 4 wins  ->  attack lowest-HP foe (bandit B)   [issued]
Tick 41:  rule 4 wins  ->  attack lowest-HP foe (bandit B)   [no-op, already commanded]
Tick 42:  rule 1 wins  ->  Ally HP<40%: heal the player       [supersedes -- follower turns to heal]
Tick 43:  rule 3 wins  ->  Foe weak to fire: cast Flames      [supersedes -- switches to that foe]
Tick 44:  rule 4 wins  ->  attack lowest-HP foe (bandit B)    [re-issued; the order had lapsed]
```

**If no rule matches, the standing order simply stands.** §4.4 says MFO makes
no engine call on a non-matching tick, and that includes not tearing down an
order — the follower keeps attacking who they were told to attack, and
vanilla AI keeps executing it.

**This is actuation state, not evaluator state, and the distinction is
load-bearing** (`INVARIANTS.md` #22). The test: *if the latch were lost
entirely, would behavior change?* Only by a redundant re-issue of an
identical order. The scan still produces the same winner; the latch only
decides whether to repeat itself. It is a smoothing layer over the world, not
a memory the evaluator reads to decide. A skipped tick still loses nothing.

#### 4.7.3 Invalidation — two questions, never conflated

**Is the target still VALID?** (mechanical, cheap, checked every tick)
dead · handle unresolvable · left the high process · no longer hostile ·
disabled or deleted. Any of these → release immediately.

**Is the target still CORRECT for the rule?** (semantic, expensive) — "lowest
HP" moves as damage lands; "nearest" moves as everyone runs around.

**The crux — two kinds of switch, and only one of them is damped:**

| Switch | Cause | Behavior |
|---|---|---|
| **Between rules** | A different rule wins this tick | **Instant. Never damped.** The player's ordering decided it, and §4.3a forbids MFO second-guessing that. Heal-the-player outranking attack means it outranks it *now* |
| **Within one rule** | Same rule still wins, but its target re-resolved to someone else | **Damped by `fTargetSwitchMargin`** (default 15%) |

Only the second is MFO making a choice, so only the second gets hysteresis.
Without it, two foes at 41% and 39% health oscillate a follower between them
forever; the margin is taken from Aggro Management's shipped threat table,
which uses the same figure for the same reason. Applying damping to the first
kind would be the mod overriding the player's rule order, which is exactly
what it must never do.

**Invalidity always wins over commitment.** A dead or unloaded target is
released immediately regardless of margin — hysteresis governs *preference*,
never *validity*.

#### 4.7.4 Re-resolution is throttled independently of the tick

Evaluating `Always` is cheap; resolving "lowest-HP foe" needs the world
snapshot and is expensive-tier (§4.2). So **target re-resolution runs on its
own slower cadence** — default every 4th service tick (~530 ms) — while the
cheap validity check runs every tick.

The effect: a target that dies is dropped within one tick, but the expensive
"who is lowest now?" question is asked four times less often. Cheap
selectors (`my current target`, `player's target`) are direct reads and skip
the throttle entirely.

#### 4.7.5 What this costs, honestly

A standing order means MFO is holding engine state between ticks, which is
the thing §8.5's clean-uninstall promise is most exposed to. Consequently:

- Every latch is **released on**: combat exit, follower death, dismissal,
  cell detach (`KeepOffsetFromActor` does not survive high-process exit —
  §4.5a rule 4), board edit of the issuing rule, MFO shutdown, and the panic
  reset.
- The latch is **not serialized.** It is rebuilt from live state after a
  load, because a target handle that survived a save is exactly the dangling
  reference `INVARIANTS.md` #9 forbids.
- **MFO may always clean up after itself; it may only ACT when a rule
  matches.** These are different permissions and the distinction resolves the
  apparent conflict with §4.4. Releasing a latch whose target just died is
  cleanup of MFO's own prior call — always allowed, even on a tick where
  nothing matched. Issuing a *new* order always requires a winning rule.
  Leaving an alias pointing at a corpse because "no rule matched this tick"
  would be a bug wearing a principle's clothes.
- On load, MFO **clears any commanded-target alias it owns** before the first
  tick, so a save made mid-order does not resume pointing at something stale.

### 4.7a THE MECHANISM (SUPERSEDES §4.7's mechanism list — 2026-07-21)

§4.7 designed target commitment around an alias + global + package override +
`StartCombat`. **That whole stack is wrong**, and the field proved each piece:

* `StartCombat` via a sourced relocation returned OK and **did not take**
  (ENGINE_NOTES §0.12).
* `EvaluatePackage` **no-ops** when the chosen package is unchanged (§0.7), so
  package-driven retargeting needs a condition flicker to do anything at all.
* And the premise was unreachable regardless: **Papyrus has no combat-target
  setter.** None. Not in `Actor.psc`, SKSE, po3, or PapyrusUtil — only
  `StartCombat`/`StopCombat` and a *getter* (§0.14).

**The actual mechanism is a vfunc hook on `Character::UpdateCombat`** (vtable
index `0xE4`), writing the latched target into `currentCombatTarget` and
`combatController->targetHandle` after the original runs. Reference
implementation: `SmartNPCTargetSelector.dll`, open source, shipped, and
installed in LoreRim on this machine.

#### What this changes about commitment

**Retention is a property of the MECHANISM, not of the target.** §4.7 asked
"does our commanded target stick?" and two field sessions failed to answer it.
The question was malformed. The engine re-picks continuously — that is the
premise the reference implementation is built on — so:

* A write from MFO's 7.5 Hz tick **would** drift, in the gaps between ticks.
* Re-asserting inside the hook runs at the ENGINE's cadence, so it cannot drift.
* Re-asserting is a handle compare-and-write, **not** a `StopCombat`/`StartCombat`
  reset — so #22a's "no combat-state churn" survives intact.

The three action lifecycles, now that the mechanism is known:

| Action | Lifecycle |
|---|---|
| Attack target | **Continuous** — latch holds it, hook re-asserts at engine cadence |
| Cast at target | **Transient** — suppression window governs it |
| Formation / hold | **State-set** — issued once, re-assert on cell attach |

#### Two rules the mechanism imposes

1. **ONLY REDIRECT WHEN THE ENGINE ALREADY HAS A TARGET.** If vanilla cleared
   it — foe fled, died, went undetected, combat ended — it did so for reasons
   MFO cannot see. Commanding WHICH foe is ours; commanding THAT there is a foe
   is fighting the engine's own validity logic, and that is how a mod ends up
   with followers swinging at things they cannot perceive.
2. **Write only for followers under an active gambit latch.** Every other actor
   passes through untouched. This is what keeps the blast radius to "followers
   under player orders" — the only defensible half of the split when a
   target-selection mod is also installed.

### 4.7b MFO SHIPS NO PAPYRUS SCRIPT (RULED — 2026-07-21)

Previously an unexamined preference. Now a finding: **a companion `.psc` could
not deliver the attack verb at all**, because the capability does not exist in
the script language. And everything a script *could* deliver
(`KeepOffsetFromActor`, `SetDontMove`, `EquipSpell`) is reachable from the DLL
by VM dispatch with identical semantics and no shipped script, no VMAD, no
quest, and no compile step. The no-script architecture is confirmed, not
indulged.

### 4.8 The logistics table — non-combat rules (DECIDED — marth)

Gambits govern **upkeep** as well as fighting. A follower who runs dry of
arrows, has no health potion, or is still wearing the fur armor they were
recruited in is a follower the player has to micromanage — which is the
chore this mod exists to delete.

**These are a SEPARATE TABLE with their own slots.** Not a section of the
combat list. Three reasons, all structural:

1. **Different cadence.** Logistics runs on the out-of-combat idle tick
   (~1 s, §4.1), never at 133 ms. Nothing here is reflex-timed.
2. **Different conditions.** "Fewer than 20 arrows" is not the same kind of
   question as "ally below 40% health", and mixing them makes both lists
   harder to read.
3. **No slot competition.** Combat slots are the Rapport reward (§5.2).
   Making a player choose between "heal me when I'm dying" and "pick up
   arrows" is a false and annoying choice.

**The two tables never interleave.** Combat table runs in combat; logistics
runs out of it. A follower in combat does not stop to loot, and a follower
looting is not mid-fight. `bScavengeInCombat` exists and defaults **OFF**.

#### 4.8.1 Conditions (supply-oriented)

| Condition | Param |
|---|---|
| I have fewer than N health / stamina / magicka potions | count, which |
| I have fewer than N arrows / bolts for my equipped bow or crossbow | count |
| There is lootable equipment nearby better than mine | category (below) |
| A lootable corpse or container is within N units | radius |
| My carry weight is above X% | threshold |
| The player is / is not looting right now | — |

#### 4.8.2 Actions

- **Loot potions — health, stamina, magicka ONLY (RULED — marth).** Not
  cure-disease, not fortify, not resist. Those three are what
  self-sufficiency means; everything else is a rule nobody writes and a
  vocabulary nobody reads. Other potions are left where they lie for the
  player. *(The combat table's `Drink` action narrows to the same three, §3.3
  — a follower drinking a cure-disease potion mid-fight is not a gambit
  anyone wants.)*
- **Loot ammo** matching the equipped bow or crossbow, preferring higher
  damage. **Portability note:** vanilla grants a follower infinite arrows of
  any type they own one of, which makes this rule near-pointless; Requiem-class
  lists remove that, which makes it essential. The rule is written for the
  list that needs it and is harmlessly idle on the one that doesn't.
- **Loot better equipment**, generalized by category — **never by item**:

  | Category | "Better" means |
  |---|---|
  | Heavy armor / light armor | higher armor rating, weighted by *their* armor skill |
  | One-handed / two-handed / bow / crossbow | higher damage, weighted by *their* weapon skill |
  | Shield | higher block rating |

  A rule reads `Loot better heavy armor`, not `Loot Ebony Cuirass`. This is
  the §3.3 derived-vocabulary principle applied to loot: **modded gear works
  with no patch**, because "better" is computed from the item's own stats
  against the follower's own skills at runtime. A follower skilled in light
  armor is not upgraded into heavy just because the number is bigger.
- **Equip what was looted** — otherwise the upgrade sits in a bag.

#### 4.8.3 The rules that keep this from being a menace

Auto-looting followers have a long history of being hated. Four hard limits:

- **Ownership is absolute. A follower never takes an owned item.** Not from
  houses, not from shops, not from player-owned containers. Anything else
  makes the follower a pickpocket who gets the player arrested — the
  ownerless-`PlaceObjectAtMe` lesson (`INVARIANTS.md` §E) in a new costume.
- **First dibs belong to the player, and they are claimed by DELAY, waived by
  USE (RULED — marth).** Two mechanisms, one visible and one not:

  1. **The delay** — `fLootDelaySeconds`, MCM-tunable, **default 25 s**. A
     corpse or container is not eligible for follower looting until it has
     been in the follower's consideration radius that long. Long enough that
     the player who wants the good sword will have walked over and taken it;
     short enough that a follower topping up arrows still feels responsive.
  2. **The waiver — invisible, always on, not configurable.** Once the player
     has taken from a ref, its delay collapses from 25 s to
     `fLootWaiverSeconds` (**default 4 s**) — *not to zero*.

  **Why the waiver is 4 s and not instant: QuickLoot.** QuickLoot IE is
  installed in **4 of the 5 Skyrim lists on this machine**, so it is the
  normal looting UX, not an edge case. It takes items **one at a time over
  several seconds**, so an instant waiver would let the follower start
  grabbing things out of the same corpse while the player is still working
  down the list. The waiver timer therefore **resets on every take**: the
  follower moves in 4 s after the player's *last* take, not their first.

  **QuickLoot also breaks the obvious detection, in two ways:**

  - **It never opens `ContainerMenu`.** A design that marks refs on
    container-menu close would miss QuickLoot users entirely — i.e. almost
    everyone. **The primary signal is therefore
    `TESContainerChangedEvent` filtered to items whose new container is the
    player.** The direction filter is mandatory: without it the sink
    re-triggers on its own removal, which is MAO's infinite credit loop.
  - **Its menu appears passively on crosshair.** So *looking* is not intent —
    a QuickLoot user glances at every corpse they walk past. **The waiver
    keys on TAKING, never on opening.** The one exception is a deliberate
    full `ContainerMenu` activation, which is an explicit act and counts on
    its own.

  The marked set is a **bounded LRU (256 refs), deliberately NOT serialized**:
  worst case after a load is one more 25 s wait on an already-picked corpse,
  which is not worth a growing save record.

- **Never touch a container while the player has it OPEN — and this is a
  SAFETY rule, not only a courtesy one.** MEO learned that mutating an engine
  container while a vanilla menu is building its list from it breaks that menu
  (it broke Belethor's barter menu, m19e). An open `ContainerMenu` is an
  absolute bar regardless of delay or waiver.
- **Respect carry weight.** Never loot a follower into being overencumbered;
  that turns a convenience into a bug report.
- **Rate-limited and bounded.** At most one loot action per idle tick, per
  the one-action-per-tick contract (§4.3). A follower does not vacuum a room
  in a frame.

#### 4.8.4 Phasing

- **Tier A — loot what is already in reach.** When the follower is near a
  valid container or corpse, MFO performs the transfer directly. No pathing,
  no packages, engine calls only. This is the whole feature for most play,
  because followers stand next to the corpses anyway.
- **Tier B — go and fetch.** Sending a follower across a room to a corpse
  needs positioning, so it inherits every §4.5a constraint and ships later,
  or not at all.

Ship Tier A. A follower who tops up arrows and potions from the bodies at
their feet, and upgrades their own armor when something better is lying
there, is the entire value; walking to fetch is a refinement.

---

## 5. Rapport — per-follower progression (DECIDED — marth)

### 5.1 The currency

**Rapport** is earned by fighting *together*. Per-follower, never pooled,
never transferable, never purchasable. (Naming is original to MFO; code and
serialization use `rapport`.)

Sources:
- **Shared kills** — a kill by the follower (always counts), or a kill by the
  player while that follower is **in combat OR within `fSharedRadius`**
  (default 3,000 units). The combat-state test is what makes archery builds
  work; the radius is the fallback for stealth kills that end a fight before
  the follower engages (`BALANCE.md` §2). Native `TESDeathEvent` sink;
  **fires twice, act only on `dead == true`**; killer tested as player or
  teammate.
- **Shared survival** — combat time where both are engaged, awarded on
  combat exit via `TESCombatEvent` rather than per-tick.
- **Gambit success is deliberately NOT a source.** Rewarding rules for firing
  incentivizes spammy rules; Rapport measures time fought together, not
  automation quality.

### 5.2 Ranks, slots, vocabulary

| Rank | Combat slots | Logistics slots (§4.8) | Vocabulary opened | Reaction (§4.1b) |
|---|---|---|---|---|
| I (start) | 2 | 1 | Vitals conditions; Cast/Drink from what they already know; loot potions | widest spread, tail-heavy — visibly hesitant |
| II | 4 | 2 | Status + Kind conditions; Equip actions; loot ammo | ↓ |
| III | 6 | 3 | Threat + Range + Stealth conditions; Tier-B stance when available; loot better equipment | ↓ |
| IV | 8 | 4 | Party/Self conditions | ↓ |
| V | 12 | 5 | Full condition set; full derived action set | tightest spread, lowest floor — anticipates |

The logistics ladder is deliberately shallow — **5 slots covers the entire
vocabulary in §4.8** (three potions, ammo, equipment), so a Rank V follower
is fully self-sufficient rather than merely closer to it. Combat is where the
depth is, and where slots stay scarce enough to force real choices.

The reaction column is deliberate: it is the one Rapport reward the player
**feels without being told about it**. Slots and vocabulary are read off a
menu; a companion who stops hesitating is noticed in play.

Thresholds are INI/MCM-tunable (`iRapportRank2`…`iRapportRank5`). **Shipped
defaults: 250 / 1,000 / 2,500 / 5,000 cumulative** — roughly 6 / 22 / 55 /
110 hours adventuring with that follower at ~45 Rapport/hour. Rank III lands
near the end of a mid-length questline with one steady companion; Rank V is a
long-haul commitment to *one* follower. Serial follower-swapping is
deliberately slower than loyalty — that is the design statement. **Rapport
never decays and survives dismissal.** Full derivation, income model, and the
reaction curve: `BALANCE.md`.

### 5.3 Aptitude — the competence gate (the headline mechanic)

**Unlocking a gambit is not the same as being able to execute it.**

- **Skill gates what unlocks.** The vocabulary a follower is *offered* at each
  rank is filtered by their own actor values, read live via
  `GetBaseActorValue` — never from a baked per-follower table. A hardcoded
  "Lydia is a warrior" list is exactly the DYNAMIC_OR_DROP liability.
- **Magicka gates what executes.** A `Cast` whose spell the follower cannot
  afford **fails, and the rule falls through to the next one.** MFO does not
  top up magicka, discount cost, or substitute a cheaper spell. The engine's
  cast attempt is the arbiter.
- **Skill gates how well it lands.** A taught spell cast by an unskilled
  caster is weak, slow, and often fails, because that is what the engine does
  with it. MFO adds no compensation.

A player can absolutely write a list their follower cannot run, and watching
it fail teaches them their follower's actual build. **The board surfaces
this**: a rule that failed to execute renders with a reason (`insufficient
magicka`, `spell not known`, `no valid target`) rather than silently doing
nothing — the inherited "log the zero case" doctrine applied to UI. A failure
the player cannot see reads as "never examined."

### 5.4 Spell acquisition is OUT OF SCOPE (RULED — marth, 2026-07-21)

**MFO does gambits. It does not teach followers spells.**

Earlier drafts had a "Tutoring" system: at Rank IV, assigning a gambit whose
action needed an unknown spell would grant it, ledgered, and un-assigning
would revoke it. **Cut.**

**Why.** *A Fun Way To Level Followers* (TrumanAE, Nexus 181813, SKSE,
[open source](https://github.com/TrumanGIT/Follower-Leveling-System-Redone))
already does this properly: followers gain skill points per player level, and
learn perks and spells at 20/40/50/60/80/100, configurable through
`PerksAndSpells.json` so it works with any perk or spell overhaul. It is
installed and enabled in the test profile.

Two mods granting spells to the same actor is not a feature. The clean split:

| | Owns |
|---|---|
| **A Fun Way To Level Followers** | **Acquisition** — how a follower comes to know Fireball |
| **MFO** | **Deployment** — when they should cast it |

**This is synergy, not merely deconfliction.** §5.3's aptitude gate reads
`GetBaseActorValue` to decide what vocabulary a follower is *offered* — and
without a levelling system those values barely move, so the gate is nearly
static and the progression it implies never really happens. AFWTLF makes it
live: you spend skill points, their skills rise, and MFO's vocabulary opens in
response. **Their mod is what makes MFO's aptitude gate mean something.**

**What this deletes**, and the deletion is the point: the tutored-spell
ledger, the revoke path, the `MFO_GrantedSpell` keyword, the
`PO3.RemoveAddedSpells` backstop, and the reconcile-on-load. That was the
largest remaining surface in MFO that could damage someone else's state — a
revoke that mis-scoped would have eaten spells another mod granted.

**What remains**: §3.3's derived action vocabulary reads whatever the follower
knows, via `PO3.GetAllActorPlayableSpells`. It does not care who taught them.
A spell learned through AFWTLF, a perk overhaul, or a quest all appear the
same way, with no patch.

**Consequences elsewhere**
- Rank IV no longer unlocks tutoring; it unlocks Party/Self conditions only.
- `MFO.esp` FormID `0x802` shipped as the granted-spell keyword and is now
  **RESERVED and unused**. FormIDs are forever — it is never recycled
  (`INVARIANTS.md` #41).
- `INVARIANTS.md` #20 (never `AddBaseSpell`) becomes moot in practice: MFO
  adds no spells at all. It stays on the list because the reasoning is sound
  and cheap to keep.
- The co-save loses its tutored block — see `ARCHITECTURE.md` §7 for why that
  was safe to do without a version bump, and why this was the last moment it
  would have been.

---

## 6. The Gambit Board (UI) — copy MEO exactly

**MEO's gem menu is the reference implementation and MFO copies it
wholesale**: hooks, threading model, snapshot discipline, controller story,
skin system, and row idiom. Only the *content* differs. Everything in this
section is quoted from a shipped, field-validated build
(`MEO/native/plugin.cpp`, `namespace menuhook` :3709–4570) and should be
lifted rather than re-derived. MEO in turn verified it against
`D7ry/wheeler`.

### 6.1 Opener

The **Field Orders** lesser power, granted at startup. Crosshair a follower,
use the power, their board opens. Crosshair via `CrosshairRefEvent`; the cast
observed via `TESSpellCastEvent` (which fires for lesser powers) — MEO's
`SpellCastSink` shape, gated on `IsPlayerRef()`. With no follower under the
crosshair, the power opens a party roster.

Deliberately **not** dialogue-driven: a dialogue opener means quest records,
aliases, and Papyrus, and it fights every framework that rewrites follower
dialogue.

### 6.2 Hooks and init

Three trampoline call-hooks, **`SKSE::AllocTrampoline(256)`** (MEO's
ENGINE_NOTES says 64; the shipped code uses 256 — **trust the code**),
installed as the *first* statement in `SKSEPluginLoad`, before the renderer
initializes:

| Hook | RelocationID | Offset | Thread |
|---|---|---|---|
| D3DInit | `(75595, 77226)` | `VariantOffset(0x9, 0x275, 0x0)` | init, once |
| DXGIPresent | `(75461, 77246)` | `Offset(0x9)` | **render** |
| InputDispatch | `(67315, 68617)` | `Offset(0x7B)` | input |

Init order in D3DInit: call original first → `BSGraphics::Renderer`
singleton (bail + log "menu disabled" if null) → swapchain desc → device and
context from `data.forwarder`/`data.context` → `CreateContext`,
`io.IniFilename = nullptr`, nav flags → fonts → Win32/DX11 backends →
WndProc swap (`WM_KILLFOCUS` → `ClearInputKeys()`) → apply skin → cache
`sd.BufferDesc.{Width,Height}` → `g_d3dReady = true`. **Every failure path
logs a distinct reason and leaves the menu disabled — it degrades to a
notification, never a crash.**

Build traps: `WIN32_LEAN_AND_MEAN` + `NOMINMAX`, and **`#undef GetObject`
after the D3D includes** or `wingdi.h` hijacks
`BGSDefaultObjectManager::GetObject<T>()`.

### 6.3 Threading and snapshots

**Two mutexes, never nested:** `g_menu.lock` (guards snapshot vectors; taken
only to publish, after engine reads are done into locals) and `g_imguiIoMx`
(**every** ImGui-IO touch on all three threads — held around the IO block,
**never** across the engine passthrough).

The snapshot holds **plain value types only** — strings, FormIDs, ints. **No
engine pointer ever crosses to the render thread.** For MFO the row struct is
the flattened rule: `{ruleId, enabled, condLabel, actionLabel, condCategory,
lastResult, lastReason}`.

Mutation is immediate-per-action, **not** batched on close: MEO's
reconcile-on-close design was retired when the engine's uid rewrites orphaned
records. `QueueMenuTask` claims `busy` with an atomic `exchange(true)`,
queues an `AddTask` that mutates, rebuilds the snapshot, and clears `busy`.
Widgets capture **copies** of their row, never references.

### 6.4 Two invariants that outrank everything else in this section

**(a) Menu action rows fire SINGLE-SHOT via `ImGui::IsItemActivated()` —
never `Selectable-return || IsItemClicked`.** MEO's INVARIANTS #20b, earned
over a year of "misclicks": `IsItemClicked` reports the PRESS frame and the
`Selectable` return reports the RELEASE frame, so OR-ing them is **two fires
per physical click**. The `busy` disable does not reliably mask the echo,
because the task pump usually beats the next present so no disabled frame is
drawn — and **a snapshot generation check cannot catch it, because the echo
is stale in intent, not in data.** Symptoms in MEO: one click socketing both
units of a stack, one click burning two soul gems. For MFO the equivalent
would be one click deleting two rules.

Non-mutating selection is the deliberate exception: it acts on **press**
(`IsItemClicked`), because the raw-delta cursor can drift off the row between
press and release and cancel a release-click.

**(b) Selection is IDENTITY, never index (MEO m19e).** Rows are sorted, and
any edit can move a row; a raw index silently lands on a *different* subject.
MEO keyed selection on `(base, uid)` after a socket changed a label and the
selection landed on the wrong item. **MFO keys selection on the stable rule
id** — and this matters more here than it did in MEO, because reordering is a
primary action: every move would otherwise shift the selection to a different
rule.

### 6.5 Controller — the shipped pattern (standing family rule)

Full controller support ships in every marth mod; MEO validated this pattern
in the field.

- `ImGuiConfigFlags_NavEnableGamepad` + `ImGuiBackendFlags_HasGamepad`; both
  panes `ImGuiChildFlags_NavFlattened` so nav crosses child boundaries;
  `ImGuiCol_NavHighlight = skin.accent` so the focus ring survives a custom
  palette.
- `GamepadToImGuiKey()` maps `RE::BSWin32GamepadDevice::Key` → `ImGuiKey_*`:
  d-pad → `GamepadDpad*`, `kA` → `GamepadFaceDown` (activate), `kX`/`kY` →
  `FaceLeft`/`FaceUp`, shoulders → `GamepadL1`/`R1`.
- **`kB` is deliberately absent from that table** — it is intercepted before
  translation as close, so it never reaches ImGui nav as Cancel and fight the
  close.
- **Left stick is edge-triggered into d-pad keys**, ±0.5 threshold, state in
  `bool g_stickNav[4]`, reset to false on every open. ImGui's own nav repeat
  handles held directions, so only edges are synthesized. Right stick ignored.
- **Pane switching is deterministic, not geometric** (m36e): track which pane
  held focus last frame; on the opposite d-pad press call
  `SetKeyboardFocusHere()` into the other pane. Spatial nav across panes
  depended on row alignment and felt broken.
- **Close on the shout key fires on RELEASE with both edges swallowed**, and
  requires a press seen while open (m23c) — closing on press leaked the
  release to the game, which re-cast the power and instantly reopened the
  menu.
- **No control locking.** MFO never calls `DisablePlayerControls`. The input
  hook sets `*a_events = nullptr` while open, so the game sees nothing: no
  bleed-through, no stuck-controls failure mode, no save-state pollution.
- Footer spells the bindings out for pad users.

**MFO's one new interaction: reordering.** MEO has no reorder, no drag-drop,
and no move affordance anywhere — its lists are sorted deterministically by
the snapshot builder. So this is the one place MFO extends rather than
copies, and the architecture dictates how:

- **No drag-and-drop.** The cursor is integrated from raw mouse deltas and
  has a documented press/release divergence that already produced field bug
  reports; a drag is exactly the interaction that must survive
  press→move→release. Drag is also unusable on a stick.
- **Discrete move-by-one**, via `▲`/`▼` small buttons and — conveniently —
  **`GamepadL1`/`R1`, which MEO already translates and does not use.** Free,
  proven bindings.
- Reorder is a **mutating action**: single-shot → `QueueMenuTask` → mutate
  the authoritative list on the main thread → rebuild snapshot → render
  thread sees new order next present.
- Selection keyed on rule id (§6.4b), or every move loses the selection.

### 6.6 Layout

Window: centered on `ImGuiCond_Appearing` (not `Always`, so it stays
draggable/resizable), 62% × 68% of display, min 640×420, `NoTitleBar` with a
hand-drawn centered title and flanking rules, `NoSavedSettings`.

Two panes as `BeginChild` with `-footer` height: **left = follower roster**
(name, Rapport rank, slots used, live "last rule fired"); **right = the
ordered rule list**, one row per gambit.

**Fetch the draw list INSIDE each `BeginChild`** — MEO's m24c bug was rows
drawn through the outer window's list ignoring the child's clip rect, so
"long lists leave the pane." A rule list is exactly a long list.

**The row idiom, copied verbatim:** capture `GetCursorScreenPos()` *before*
an invisible full-width `Selectable("##row", selected, 0, ImVec2(0, rowH))`
used purely as hit/nav target, then hand-draw all visuals into the child's
draw list — a category-colored pip at the left, the condition text, the
action text, right-aligned status. Interactive sub-widgets (enable checkbox,
`▲▼`) come after with explicit `SetCursorScreenPos` + `PushID(ruleId)`.
Non-interactive rows use `Dummy(ImVec2(0, rowH))` for identical geometry.

**Never disable the list the player adds from.** MEO's m35e lesson: show it
always and *reinterpret* the action, changing the header to say what will now
happen ("SWAP — replaces socket 1"). MFO's analogue: with all slots full, the
picker stays live and says "REPLACES rule 3". Greying it out reads as broken.

Only the mutating pane is `BeginDisabled` while busy; the roster pane is
never disabled, because eating clicks during the busy window reads in the
field as "the menu misses clicks."

Tooltips explain what the row will do, sourced from engine data (the spell's
own description, the actor's current magicka vs cost) rather than hardcoded
strings.

Destructive actions (delete a rule, clear a table) use MEO's two-click
arm: label flips to "Confirm", color to `skin.danger`, and the arm is cleared
by *any* other interaction.

### 6.7 Skins, fonts, cursor

Four runtime skins selected by `iMenuStyle` (0–3), the MEO structure
verbatim: 10 colors + a sans flag, square corners, flat fills, hover/active
states computed by `Mix()` rather than authored, `ImGuiCol_PopupBg = panel`
so tooltips follow the skin. **Restyle only on change** (`g_appliedSkin`).
MEO's nine gem theme colors are frozen across skins so gems always read by
color; **MFO's analogue is a fixed per-condition-category palette** so rule
kinds stay identifiable in every skin.

**`io.DisplaySize` lies under Proton/upscalers** — the Win32 backend reads
`GetClientRect`, which can disagree with the backbuffer. Cache
`sd.BufferDesc.{Width,Height}` at init and overwrite `io.DisplaySize` every
frame **between `ImGui_ImplWin32_NewFrame()` and `ImGui::NewFrame()`**.

**Real TTFs baked at backbuffer scale** (`max(1.0, height/1080)`), all
optional with an `ifstream(...).good()` gate, head falling back to body (not
to the default bitmap font). `FontGlobalScale` on the bitmap font was blurry
above 1080p.

**Seed the cursor position on open.** ImGui only learns cursor position from
move events, so the first click of a session landed at an invalid position
and silently missed. The cursor itself is integrated from raw
`mouseInputX/Y` deltas, clamped to display size, stored as atomics.

---

### 6.7a THE FOUR SKINS — branding, not decoration (RULED — marth, 2026-07-22)

**The board ships MEO's four skins, by name, selectable live.** This is a
STANDING FAMILY RULE in the same class as controller support: every marth
Skyrim mod carries it, and it is never scoped out of a milestone.

| # | Skin | Character |
|---|---|---|
| 0 | **Ebony & Brass** | near-black, warm brass border and accent |
| 1 | **Dwemer Parchment** | light parchment, dark umber text — the only bright skin |
| 2 | **Soul Cairn** | deep violet, pale cyan accent |
| 3 | **Quicksilver** | cool slate, sans face, spaced HUD title |

Selected by `iMenuStyle` 0..3, an MCM dropdown, applied at runtime — the
player picks live, no reload. MEO's `MenuSkin` struct is the shape to copy:

```cpp
struct MenuSkin {
    const char* name;
    ImVec4 winBg, panel, border, text, dim, sel, accent, btn, track, danger;
    bool   sans;    // Quicksilver only: sans face + spaced title
    const char* title;
};
```

**Copy the palette values verbatim from MEO** (`native/plugin.cpp`, `kSkins[4]`).
They are the mockup artifact's numbers and they are the brand; re-deriving them
by eye produces a mod that looks *nearly* like its siblings, which is worse than
not matching at all.

**Design language, inherited:** square corners, flat fills throughout — ImGui's
honest range, and closer to Skyrim's own UI than its default debug grey.

**MFO's title string is its own** (MEO's is `GEM SOCKETING`), but everything
else about the chrome matches.

#### The Field Kit's density is the target, not a problem to solve

marth, on the current overlay: *"I like the complicated design of the current
one, so it's more theming."* The Field Kit's information density — live vitals,
per-rule state, counters, probe controls — is the intended character of these
menus. **M7 skins that design; it does not simplify it.** A sparse board would
be off-brand for the family even if it were easier to read.

## 7. MCM / INI surface

The MEO/MAO surface inherited whole: a seed `SKSE/Plugins/MFO.ini` plus MCM
Helper's `MCM/Settings/MFO.ini` read last so it wins, re-read live on
JournalMenu close **and at every board open** (MCM Helper only flushes when
*its* menu closes, so a skin change must be picked up on open).

Inherited rules that bite here:
- **An INI/MCM key that changes SEMANTICS must be RENAMED.** MCM Helper
  persists values per key name into MO2's overwrite and they survive mod
  updates; MEO's absolute-to-multiplier change silently cut an XP stream
  ~100× for upgrading users.
- **Reset-then-parse every pass** so an absent key reverts instead of
  sticking at its last in-memory value.
- **Strip the UTF-8 BOM** MCM Helper writes.
- **Skip unparseable values, never apply 0.0.**
- **Backfill every new key into existing Settings files on deploy** — MCM
  Helper reads an absent key as OFF/zero and does *not* fall back to
  `config.json`'s default.
- **Generator text must equal DLL math** — tooltips that disagree with
  behavior are a shipped lie.

---

## 8. State, persistence, and the borders

### 8.1 What lives in the co-save

Per-actor `{actorFormID, rapport, rank, gambits[], packageOverrides[]}` plus globals (grant latches, schema version). Rules
serialize as string opcode ids + resolved FormID params (§3.3).

Inherited invariants, none optional:
- **Every persisted FormID passes `ResolveFormID` on load. Unresolvable →
  drop the record, never guess.**
- **Never persist a runtime-created (0xFF) FormID** — why summons get
  session-only boards (§3.1).
- **Bound every count; bail on short read; clamp at ingestion.** Rule count
  clamped to the rank's slot maximum; rank clamped to [1,5]. A truncated
  record stops the parse, never fabricates.
- **Versioned schema, readers for every shipped version kept forever, fields
  append-only. SKSE does NOT round-trip unread co-save records — a downgraded
  DLL that saves DESTROYS newer records.** Warn loudly in a `kPostLoadGame`
  message box; never log a comforting "preserved as unread."
- A migration that cannot honestly map a record **drops it with a log line
  saying what was lost** — never fabricates.

### 8.2 FormID band (frozen generator↔DLL contract)

`MFO.esp` is ESL-flagged (TES4 flag `0x200`); own records use the own-file
master-index prefix and stay inside `0x800`–`0xFFF`. Allocation is anchored by
`data/mfo_forms.frozen.json` — regen unions new pairs, `next_fid = max+1`,
**never recycles**, and the freeze guard trips in **both** directions (drift
AND shrink).

| Range | Contents |
|---|---|
| `0x800` | Field Orders MGEF |
| `0x801` | Field Orders SPEL (lesser power) |
| `0x802` | shipped as a granted-spell keyword; now **RESERVED, unused** (§5.4). FormIDs are forever — never recycled |
| `0x804` | startup QUST |
| `0x808` | MCM QUST |
| `0x80A` | **command QUST** — owns the combat-target alias pool (§4.7.1) |
| `0x80B` | `MFO_HasCommandedTarget` GLOB |
| `0x80C`–`0x80F` | reserved for further command globals (hold/spacing flags) |
| `0x810`+ | reserved — future player-side perks (§11) |
| `0x820`+ | MFO's own conditioned PACKAGEs (attack-commanded-target, hold, spacing) — §4.5a's declarative route |

**Alias pool sizing (§4.7.1).** Aliases are a fixed count on a quest, so the
pool caps how many followers can hold a standing order simultaneously.
**Default 8**, which comfortably exceeds vanilla's follower limit and matches
the party sizes framework users actually run. Beyond the cap, a follower gets
Tier-A actions only and the board says so — the same graceful degradation as
§4.6, never a silent failure.

`MFO.esp` declares a single master (`Skyrim.esm`) and references no external
records. **There is no patch plugin and no installer** — unlike MEO's
calibration or MAO's perk-tree surgery, MFO bakes nothing from a load order;
its entire data surface is the actor in front of you.

### 8.3 Enumerated hand-write exceptions

Per the call-the-engine doctrine, every deviation is named here or it does not
exist. **Current list: none.** Tier A and every Tier-B mechanism in §4.5 are
engine calls. If a future mechanism requires hand-writing engine bookkeeping,
it is added here with its scope and reversal, or it is dropped.

### 8.4 The player-relative landmine (MFO's highest-risk inherited rule)

MEO's worst 1.0.6 blocker: a helper that answered a question by scanning
**the player's** state was reached from a path carrying a **non-player
actor**; it stripped every NPC's enchant and orphaned records permanently.

**MFO is built entirely out of that shape.** Every condition in §3.2 is a
state scan, and every one runs against a follower.

- **Every predicate takes its subject actor as an explicit parameter.** No
  ambient "the actor", no default-to-player.
- **Every scan helper names whose state it reads in its function comment**,
  and every new call site is audited against that comment.
- **A `nullptr` subject is an error, never a wildcard.**
- **Prefer deriving from live per-actor state over threading an owner through
  layers** — MEO's own stated endgame, learned late, adopted here from the
  start.
- The corollary MEO paid for twice: *the player-side path had the comment
  explaining all this and the follower path, written later, violated it
  anyway.* **Audit every new call site against existing doctrine; the
  compiler will not.**

### 8.5 Save safety

- **Install mid-save: safe.** New records only; no vanilla record edited.
- **Update in place: safe by design.** FormIDs frozen post-release; the
  co-save migrates forward; package overrides are reconciled against
  `CountPackageOverride` every load, so a missed release self-heals.
- **Uninstall mid-save: genuinely clean, uniquely for this family.** MFO
  attaches nothing durable to items or the world. Walk the ledgers, revoke
  spells (with the `RemoveAddedSpells` backstop) and remove every MFO package
  override, and the followers are exactly as found. This is a design *goal*
  that drove §4.5 rule 2 and §5.4 — **the reason no indefinite override ever
  ships is that overrides outlive the mod.**
- **Dev/test workflow:** keep one read-only baseline save that has never seen
  the plugin; reload it for every test of a build whose records changed. When
  in doubt, clean reload — persisted state lies.

---

## 9. Runtime architecture

Everything lives in **`MFO.dll`** (CommonLibSSE-NG; `native/plugin.cpp`):

- **Startup** (`kDataLoaded` / `kPostLoadGame` / `kNewGame`): resolve forms,
  grant Field Orders, load and migrate the co-save, reconcile the override
  ledger, re-assert GlobalVariable handshakes.
- **Event sinks:** `TESDeathEvent` (Rapport; `dead == true` only),
  `TESCombatEvent` (combat entry/exit, shared-survival award),
  `TESSpellCastEvent` (opener), `CrosshairRefEvent` (board target),
  `MenuOpenCloseEvent` (MCM live re-read; **LoadingMenu-CLOSE as the
  gameplay-resumed anchor**, never a blind timer — a timer fired during a long
  load screen is swallowed), `TESEquipEvent` (debounced vocabulary refresh).
- **The evaluator:** staggered per-follower timer → `AddTask` → snapshot →
  top-down scan → actuation (§4).
- **The board:** in-process ImGui on the present hook, mutex-guarded snapshot
  (§6).
- **Co-save serialization** (§8.1).

**`MFO.esp`** (generated by `MFO_GenerateESP.py`, FormIDs frozen): the Field
Orders MGEF/SPEL, the granted-spell keyword, the startup QUST, the MCM QUST,
and the SEQ file. Papyrus surface is **one compile-time MCM stub** — no
runtime Papyrus, no tracker quest, no heartbeat script. **Do not ship VMADs
for scripts you don't ship.**

Generator discipline: **before creating any new record type, dump a vanilla
record that already does what you want and mirror its subrecord list, order,
and byte layout exactly** (`tools/dump_record.py`). The engine silently drops
records whose layout it dislikes — there is no error. `tools/audit_esp.py`
must PASS as a merge gate.

---

## 10. Phase gates (what "done" means)

Ordered so each phase is independently playable and a CTD bisects to one
change. **One new hook or engine mechanism per release.**

| Phase | Gate |
|---|---|
| **P0** | DLL loads, logs its version; co-save round-trips a hand-authored rule list across save / load / load-order change. No gameplay. Proves §8.1. |
| **P1** | Follower detection + Rapport accrual, no actions. Proves §3.1 and §5.1 across dismissal, death, cell change, and with a follower framework installed. Needs `BALANCE.md` first. |
| **P2** | Evaluator + Tier-A cast/drink, rules seeded from console. Proves §4.4's do-nothing guarantee — **tested by diffing behavior against an MFO-absent baseline save.** **Hard perf gate: `bProfileEvaluator` numbers from a Lorerim-class order in a real multi-follower fight must meet §4.2's budget, worst case being a full rule list of expensive conditions.** An empty-cell measurement does not count. Also verify the §4.1 cadence adapts (**tick rate holds at ~7.5/s across 30 / 60 / 144 fps and drops below 30** — if not, the frame clock is wrong) and that §4.1a holds (**per-tick cost is flat from 1 to 12 followers**; if it scales with party size, the round-robin is broken). Confirm no tick burst follows a load screen. Settles §3.2's CTDA option on these numbers. |
| **P3** | The board — ImGui, **mouse/keyboard AND controller together**, copied from MEO. Gate: every action including reorder reachable on a gamepad with no keyboard; no double-fire (§6.4a) under a task-pump race. |
| **P4** | Tier-A equip actions. Isolated because of the synchronous-equip-dispatch landmine. |
| **P6+** | Tier B, one mechanism per release, in the §4.5 preference order, each instrumented — or dropped. Each proven mechanism is written into `ENGINE_NOTES.md` and Linux-Native-Tools **in the release that ships it.** |

1.0 requires P0–P4 green, a DYNAMIC_OR_DROP ledger with no open
DROP-CANDIDATEs, and a clean-install test on a load order that is not this
machine's.

---

## 11. Open questions and post-1.0 candidates

**Open, need a ruling before the phase that touches them:**
- **CTDA conditions vs hand-written predicates** (§3.2) — settle on P2
  measurement.
- **Kills per hour on a Requiem-class list** — the entire Rapport ladder
  rests on this one estimate (`BALANCE.md` §1.1). Measure it in P1 before
  trusting any hour figure.
- **Whether the miss-a-beat draw reads as charm or as jank** (§4.1b,
  `BALANCE.md` §3) — the highest-variance feel decision in the design.

*Resolved since the first draft: Rapport thresholds, shared-kill credit, and
the reaction curve are now derived in `BALANCE.md` §1–3.*
- **Does the player get a perk tree?** Every sibling has one. MFO's
  progression is per-follower by design, and a player-side "Tactician" tree
  (more slots, a second board preset) cuts across that.
  Band `0x810`+ is reserved; the decision is deferred, not made.

**Post-1.0 candidates:**
- Gambit **presets** — save a rule list as a template, apply to a new
  follower. Cheap once the schema exists; deferred so the schema settles.
- Cross-follower conditions (§3.4).
- Tier-B formation actions.
- Explicit NFF/AFT integration — surfacing the board inside their menus. A
  soft, optional layer only; §3.1's independence is not traded for it.
