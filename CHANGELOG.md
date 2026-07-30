# MFO — Changelog

Versions are immutable once released. Bump `VERSION` for every build that
reaches the game.

## v0.8.14 — arrow probe: dump what the scan actually sees on a body

v0.8.13 proved the arrow scan runs and finds every nearby corpse "empty" — even
one that visibly holds "Iron Arrow (9)" and that he already looted the gold from.
The `IsBolt()` classification is verified correct against the CommonLibSSE-NG
source, so the miss is in the runtime inventory read, not the logic. This build
adds a temporary `[arrowprobe]` line: during an arrow scan, for every corpse
within 300u it logs the body's id, distance, dead/container flags, item count,
and EVERY ammo item with its `isBolt` flag + count. That distinguishes the three
possibilities in one look — the body isn't being scanned (cell/range), the
inventory read comes back without the arrows, or the arrows carry an unexpected
flag. Probe only; no behaviour change. Removed once the cause is pinned.

## v0.8.13 — see which loot category he's actually hunting

Chasing "he has zero arrows and ignores the arrows on bodies, but loots potions
and gold just fine." Everything in the arrow path checks out on inspection, so
this build makes the loot diagnostic tell the truth: the `[loot]` line now names
the category it scanned (`cat=arrows`, `cat=gold`, …) and logs once **per
category** instead of once per follower — before, only the first category
scanned in each 10-second window ever reached the log, which is exactly why the
arrow scan's own counts were invisible. One deck cycle now says whether the
arrow scan finds nothing (detection / bodies genuinely carry no arrows) or finds
them but the walk/pickup fails.

Also: the attack log prints the chosen foe's HP% so we can confirm the "fight
the weakest foe" gambit is picking the lowest-HP target (it fires correctly —
the "already on that target" lines just mean no visible switch was needed).
Router polish: the no-progress "unreachable" giveup relaxes 4s→7s to stop
transient stalls being mistaken for dead ends, and the per-body WALK diagnostic
is rate-limited so it no longer floods the tick.

## v0.8.12 — the freeze fixed at the source: skip bodies that are off the navmesh

The research settled why he'd freeze staring at a body: it was physics-settled
OFF the navmesh (clipped into geometry, on furniture, a disconnected ledge), so
the game's pathfinder had no ground triangle to aim at and never even started
walking. No amount of package tuning fixes that — the destination itself is
unwalkable.

Now MFO checks the game's own navmesh data *before* he sets out: if there's no
navmesh near a body, it's skipped on the spot (a quick data lookup) instead of
committing him to a walk he can't complete. So he only heads for bodies he can
actually reach. Tunable on the Behaviour Layer page ("Off-navmesh skip"); the
diagnostic now also logs the engine's own path-speed, so if any freeze remains
we can see instantly whether it's a path that never built.

## v0.8.11 — audit hardening (crash-safety, the freeze softened, cleanups)

A multi-agent audit of the whole loot system produced these fixes:
- **Crash-safety:** on save-revert/load the background worker could write to state
  the main thread was clearing at the same moment (undefined behavior). The worker
  is now stopped and drained before any of that happens.
- **The "stares at a body across a wall" freeze is softened:** if he can't reach a
  target, he now gives it up quickly instead of committing to it, and only ever
  heads for a body that actually holds what he wants — so far fewer bad trips. (A
  proper "don't even pick unreachable bodies" pass is next — see below.)
- Fixed a regression where he'd abandon a corpse you were browsing via QuickLoot.
- He no longer drops a *near* body in favor of a far one when many are around.
- Config/MCM cleanups (slider range, a stray default, tooling coverage).

Next: the routing research found there's no native "walk here" call — every mod
uses the package approach MFO already does; the freeze is bodies physics-settled
OFF the navmesh, which no package can path to. The fix is to detect off-navmesh
bodies and skip them before he sets out (using the game's navmesh data), landing
in a follow-up.

## v0.8.10 — only walk to a body that actually has what he wants

A follower no longer walks over to a corpse or barrel that doesn't hold the thing
his gambit is looting for. Before, he'd trek to every nearby body, arrive, find no
arrows (or whatever he was after), and move on — wasting the trip and, when the
next candidate was unreachable, stalling. Now the check happens before he sets
out: bodies without the wanted category are skipped outright, so he only makes
trips that pay off. This also sharply cuts the unreachable-body problem, since a
far body with nothing he wants is never a target in the first place.

## v0.8.9 — walk-to-loot works; stop over-committing to unreachable bodies

v0.8.8 landed the real fix — the follower now genuinely walks to and loots
reachable bodies. This tunes what was left: he'd keep trying to reach bodies with
no path to them (across a gap, another level) for ~15s each and cycle to the
next, so a loot run dragged to its 60s cap and he wouldn't come back to you. Now
if he isn't actually moving toward a body (no path), he gives up in a few seconds
and moves on — so the run ends and he returns to you promptly. (Distances he'll
walk are still the full confidence leash; this only drops ones he physically
can't reach.)

## v0.8.8 — walk-to-loot: the actual root cause fixed

The diagnostic from v0.8.6/0.8.7 finally caught it: the follower was never
receiving MFO's travel package at all — he stayed on his normal follow package
even though MFO's priority read 60. The reason: the engine decides which quest
"owns" a follower at the moment his alias is filled, and MFO was raising its
priority to 60 *after* that, which the engine ignores for ownership. So he never
walked to loot; he only ever grabbed what he ended a fight next to.

Fix: MFO now claims the follower at the correct priority from the start (the same
way the combat-casting system — which works — always has), and releases him by
handing the slot back rather than lowering a number that doesn't un-claim him.
This is the fix the whole walk-to-loot saga was circling; the built-in diagnostic
will confirm it on the next run (the follower should finally close the distance to
bodies).

## v0.8.7 — combat no longer interrupted by loot runs (+ carries the v0.8.6 diagnostic)

Fixed a real regression from batching: a loot run could last up to 60s and ran
**right through a fight** — the follower stayed committed to looting at high
priority instead of fighting, which read as "less aggressive in combat." Now the
instant a follower is in combat, any loot run he's on ends immediately and he
fights. (Includes the v0.8.6 walk diagnostic and the leash-based walk range, so a
single relaunch gets everything.)

## v0.8.6 — walk-to-loot diagnostic + wider walk range

Walk-to-loot still isn't working — the follower doesn't head for bodies a short
walk away, only grabs what he ends a fight next to. Everything checkable is
correct (the travel package matches a vanilla one that works, the wiring, the
priority), so this build adds a targeted log line to catch the one thing left:
whether he's actually running MFO's travel package or still just following. That
tells us exactly what to fix next instead of guessing.

Also: walk range is now the **confidence leash** (was a fixed 768u / one room),
so bodies anywhere in his leash are fair game — the old cap was hiding most of
them.

## v0.8.5 — batched loot runs (no more return-trip per corpse)

A follower now loots in a **batch**: he stays out and visits each valid corpse in
one trip — closest first — instead of turning back toward you after every single
pickup. He returns when the loot is exhausted, when you walk away (past his
confidence leash), after a safety time cap, or when combat starts.

- **Waits for your dibs mid-run.** If nearby loot is still under your first-dibs
  timer, he holds in place briefly (re-checking) rather than abandoning it — so a
  corpse you were about to pass on still gets picked up once you clearly move on.
  Tunable: *Batch: wait for your dibs* (Behaviour Layer, default 4s; 0 = never
  wait).
- **Can't run away.** *Batch: max loot-run length* (default 60s) caps any single
  run, and the confidence leash still bounds how far he'll roam from you.
- **Respects your gambits.** A "Wait" rule that starts matching mid-run stops the
  batch immediately, and opening a container or crouching pauses it (he doesn't
  loot out from under your menu or blow your stealth) without ending the run.
- No carry-weight cap — MFO only ever takes the item types your gambits name, so
  there's nothing to hoover; carry weight stays your load order's business.

## v0.8.4 — walk-to-loot release fixed for real: hand the follower back by priority

v0.8.3's `ResetQuest` clear was disproven on the deck exactly like v0.8.2's
`ForceRefTo(None)` before it — the readback fired again (`CLEAR STILL FAILED`),
the follower stayed latched and wouldn't follow or come on cell change. Two
failed clears settle it: **a force-filled alias ref is sticky and can't be
cleared** from native code with the natives available. A first attempt to work
around that with a package condition was caught in review before shipping — it
would have *rooted* the follower (the engine keeps him claimed with nothing to
do), so it was scrapped.

### The real fix — release by quest priority
The follower is handed back by making MFO's loot quest **outrank** the follower
framework while he walks to loot (so MFO drives him) and **drop below** it to
release (so the framework takes him back and he follows). The alias staying
filled no longer matters — whoever outranks wins. This has no "stuck" state: he's
always driven by *someone*, and if anything about the hand-off ever misbehaves it
fails toward "doesn't bother looting," never toward "frozen." A save stranded by
an older build heals itself on load.

### Edge cases closed
- Turning the loot/logistics toggle **off mid-walk** now releases him immediately
  instead of stranding him.
- A follower **dismissed mid-walk** is properly let go (otherwise he'd keep
  wandering off to loot, even across reloads).

### Also fixed — the corpse-shuffle churn
A follower who arrived at a corpse and found nothing he wanted used to be re-sent
to the same nearby corpses every few seconds, forever. Arriving at a source
(looted or not) now puts it on a short cooldown, so he moves on.

## v0.8.3 — walk-to-loot actually works: reliable clear + real Run gait

The v0.8.2 walk-to-loot fixes were disproven by the deck in one run (the
readback discipline earned its keep). Two blind theories were wrong; both are
now settled against evidence.

### The follower stopped going unresponsive — the alias clear is fixed for real
v0.8.2 cleared the loot alias with `ForceRefTo(None)`. The guard readback fired:
`NATIVE CLEAR DID NOT TAKE` — that native (id 25052) KEEPS the current ref when
passed null, so the alias stayed latched and the follower stayed stuck. v0.8.3
clears via **`ResetQuest`** (id 25014), which empties every alias fill natively;
the bare start-game-enabled loot quest re-inits empty and running for the next
fill. A second readback confirms the quest is still running after the reset.

### The follower runs to loot now instead of a slow walk
The travel package's `preferredSpeed = Run` byte was **inert** because the
"Preferred Speed" general flag (`0x2000`) wasn't set — it had been mislabelled
"AlwaysSneak" and cleared. Proven by scanning all 5,961 Skyrim.esm packages
(speed only varies when `0x2000` is set). v0.8.3 sets the flag, so followers
actually **Run** to loot and rejoin. `0x2000` is not the ignore-combat bit, so
combat still interrupts looting.

### Recovery
A follower latched by a v0.8.1/v0.8.2 build frees itself on the next load
(`ReleaseAll` now runs the working `ResetQuest` clear).

## v0.8.2 — loot priority redesign (Claim-and-Release) + walk-to-loot fixed

Two big loot changes: the "first dibs" system is redesigned into **Claim-and-
Release**, and the v0.8.1 walk-to-loot strand is fixed.

### Claim-and-Release — how a follower decides it may take your loot
The old flat first-dibs timer measured the wrong thing (it counted from when the
FOLLOWER saw a corpse, so a distant player lost the race). Now every source is
under an implicit **player claim**, released only by evidence about YOU:
- **Tiers**: consumables (arrows/potions) are free; ordinary gear waits a short
  grace; **valuables (gold) release only by rejection, fair-chance, or
  abandonment** — no timer a distant player can lose to.
- **Rejection**: you looted the source and moved to another / walked away / went
  quiet after your last take.
- **Fair chance**: you were near AND facing a source (3× while you're viewing it)
  and left it; or you never came near it at all (abandonment cleanup).
- **Convergence yield**: a follower never walks to loot right next to you — you
  win the race for the corpse you're heading to.
- **QuickLoot-aware**: detects QuickLoot in the load order; your crosshair on a
  corpse (its HUD up) counts as considering it — the follower won't take from a
  source you're viewing, and yields to it. Falls back to the vanilla menu.
- Tuned on a new, grouped **Loot Priority** MCM page.

### Walk-to-loot fixed (the v0.8.1 strand)
A follower went **unresponsive** — a priority-60 loop toward loot it could never
reach and never release (root cause in ENGINE_NOTES §0.34):
- **Native alias release** (`ForceRefTo(None)`) — the old Papyrus-VM `Clear`
  failed every time (MFO's aliases have no script) and left the follower latched.
  Also **self-heals a latched v0.8.1 save on load**.
- **Walkable radius** (`fTravelRadius`, ~one room) with a distance-scaled
  deadline; farther loot is left. **Closest loot first**; a **travel-failed skip**
  stops the churn.
- Followers now **run** (not walk) to loot. Off-switch: **Walk to loot** in the
  Behaviour Layer MCM.

## v0.8.1 — board font parity, elemental weakness gambits

- **The board bakes real typefaces** (MEO's, at backbuffer scale) instead of
  scaling ImGui's default bitmap font — the header uses the display face, the
  body the text face. This was the last "less polished than MEO" tell. Falls
  back to the scaled default if the fonts are missing.
- **`Foe: weak to fire / frost / shock`** — foe-selector conditions that pick
  the nearest enemy whose resistance to that element is negative (a race trait
  or active weakness), read from the actor's own resist value. Pair with a
  matching Cast action for the FFXII "exploit the weakness" play.
- Docs: README brought current to the shipped feature set (it was stuck at
  v0.3.0), and its stale figures corrected (real slot counts per rank, the
  Field Orders board name, first-dibs timing, removed the cut spell-teaching).

## v0.8.0 — logistics comes alive: followers loot, walk to it, and know their limits

The arc from v0.6.0 (the cast pipeline) through the v0.7.x field builds and into
Option A. The headline: the logistics table is no longer inert — followers
restock, loot, and equip on their own, they *walk* to loot instead of teleport-
grabbing it, and how far they'll range is governed by a new core tenet.

### The confidence leash — a core tenet (invisible strings)
How far a follower operates *from the player* is never a fixed number; it is a
live readout of how confident they are to survive alone (`native/Confidence.h`:
`Of()` → `LeashRadius()`, from vitality, dampened in a fight). Bold in an easy
zone (leash grows, they push ahead and range out to loot), cautious in a hard
one (leash shrinks, they fall back to the player). The player never sees a
number — they feel it. One primitive; the loot leash today, combat target-
distance next. See DESIGN "Core principles".

### Option A — engine-pathed loot acquisition
Followers now WALK to loot via the vanilla Travel package (`MFO_LootQuest` +
`MFO_TravelPackage`, authored from Skyrim.esm's own shape — ENGINE_NOTES §0.33),
transfer on arrival, and release cleanly (arrival / combat / timeout / vanished
target / open container menu / sneak / a global stale-expiry / unconditional
release on every load — the #55 alias latch self-heals). On by default; one MCM
toggle off. Off-AE falls back to arm's-reach transfer.

### Looting, made real
- **Dumb consumables**: loot arrows, bolts (own gambit), and potions (any, plus
  per-resource loot health/stamina/magicka). Arrows/potions skip first-dibs.
- **Loot gold**, and **skill-based "loot better equipment"** — weapon upgrades
  judged in the follower's dominant weapon-skill class (1H/2H/Archery); armor by
  rating. Casters don't hoover up random weapons.
- **Locked containers are Lockpicking-skill-gated** (Novice→Expert; Master and
  key-required never open). Owned loot stays a crime.
- **Confidence leash + "don't loot while you're sneaking"** courtesy.
- Loot reach is a tunable `fLootRadius` (~4–5 rooms); leash bounds and walk-to-
  loot live on a new **Behaviour Layer** MCM page.

### The board — "Follower Overhaul / Field Orders"
Retitled, trimmed to the two player-facing tabs (Followers, Gambits), restyled
on MEO, and given a full-width read-only gambit summary so a whole rule is
legible on the Steam Deck. Per-rule value steppers for pad control.

### Fixes
- **MCM finally works**: the missing compiled `MFO_MCM.pex` (the config's
  binding script) now ships, the settings store seeds every control (no more
  −1), and controls bind by `id` (ENGINE_NOTES §0.31).
- Removed the `MFO_ProbeSelect` diagnostic switchboard (no longer needed).

## v0.6.0 — followers cast, with animations, at targets you choose

**The headline: MFO does not cast. It arranges the conditions and the
follower's own AI casts** — animated, magicka-arbitrated, correctly aimed,
because it is the vanilla path. Confirmed in the field: with a spell MFO put in
his hand, Cosnach cast it himself, repeatedly, with the normal casting
animation and real magicka cost.

Three cast *verbs* were refuted getting here — `CastSpellImmediate` (all four
casting sources), `Projectile::LaunchSpell` (no projectile on a self-heal), and
`DoCombatSpellApply` (the Papyrus twin of the first). The verb was never the
missing piece. `ActorMagicCaster` is driven by the animation graph, and the
graph is driven by the follower's combat AI, so the real question was what
*state* an NPC needs before its AI casts: a spell in hand, and a target.

### New — the attack verb

`act.attack`, with foe selectors that also choose the target, the way FFXII
gambits do:

```
Foe: lowest HP   ->  Attack
Self HP < 60%    ->  Cast <spell>
Always           ->  Wait
```

Candidates come from the follower's own combat group, not a world sweep — the
engine already knows who is in the fight, and a swept list could name someone
they are not engaged with.

**Papyrus cannot express commanded targeting at all.** There is no
combat-target setter in `Actor.psc`, SKSE, po3 or PapyrusUtil — only
`StartCombat`/`StopCombat` and a *getter*. A survey of ten installed modlists
found zero Papyrus attack commands. So MFO hooks `Character::UpdateCombat` and
writes the latched target after the engine's own re-pick, the technique the
open-source SmartTargetingNPC uses. This also settles a question two field
sessions failed to answer: **targets are not sticky** — the engine re-picks
continuously, which is why the hook re-asserts rather than writing once.

Off by default (`bCommandTarget`), because it installs a vfunc hook. VR is
refused outright: the vtable index is verified for SE/AE only.

### New — the equip policy

A gambit spell goes in the follower's **off hand**, which costs a one-handed
fighter nothing. A displaced shield returns when they take a hit, when combat
ends, or on dismissal — deferred, never abandoned. A two-hander must be stowed,
so that swap is debounced.

### New — cast rate limiting

`fCastCooldown` (4s). MFO cannot tell a combat AI to cast less often, but it
decides what is in the follower's hand and they can only cast what they hold.
A cast takes the spell back; the cooldown decides when it returns. Applies to
the follower's own casts as well as MFO's.

`fMagickaReserve` exists but defaults to **0**, deliberately: a follower
healing to stay alive is exactly who a floor gets killed.

### Also

- The evaluator's cadence is its own constant, not the diagnostics pump's — a
  HUD refresh rate was silently setting how fast gambits fire.
- Suppression is positional: a higher-priority rule always preempts.
- The combat table runs only in combat.
- HP% is computed over true maximum, including fortify effects, so heals no
  longer fire late on buffed followers.
- Rapport no longer loses a follower's kill when the fight ends before the
  death event is processed.
- The co-save round-trip is proven in the field: a save carrying rapport
  reloaded intact.

## v0.5.1 — first field session. M1 closed, one hypothesis dead.

### M1 is closed

A save carrying Cosnach at **rapport 5 reloaded with the record intact** —
nothing dropped, no collisions, rapport preserved. The co-save is the
"mod ate my save" subsystem; it had been reviewed four times and never once
executed end to end. It is now proven (ENGINE_NOTES §0.11).

### The animation hypothesis is refuted

**All four casting sources — kLeftHand, kRightHand, kOther, kInstant — cast
with no animation.** `CastSpellImmediate` is a silent effect application
regardless of which `MagicCaster` issues it; the casting source is not the
variable. `iCastSource` stays configurable but defaults back to `3`/kInstant,
which at least names what happens.

A visible cast still needs `LaunchSpell`, Papyrus VM dispatch, or a separate
animation event. **This is now the biggest open problem in the mod** — the core
loop's most visible action has no visible action (§0.10).

### The engine does not gate casts on magicka

Casting works **unlimited times at zero magicka**. So `CastSpellImmediate`
deducts while a pool exists, then casts free forever — which makes MFO's own
`CalculateMagickaCost` pre-check **the only gate that exists**, not
belt-and-braces. Delete it and every follower is an infinite spell battery
(§0.9).

### Rapport was losing followers' kills

Cosnach fought and killed a fox ~2792u away and earned nothing.

The shared-kill test ran from a **queued** death task and asked `IsInCombat()`
— but killing the last enemy *ends combat*, so a follower who fought the whole
battle reads `false` at the only instant the test gets to look. The radius
fallback then measured to the **player**, and the follower who did the work is
the one most likely to be far from them. Both checks failed.

Combat participation is now **sampled on the sweep** and tested as "was
fighting within `fSharedCombatGrace` (15s)", with proximity to the **victim**
as a third clause. The zero-credit case now logs why, per follower (#51).

Review caught that the first fix left the same signature reachable: a
held-but-unresolvable handle was skipped by both the award loop and the new
diagnostic, though neither needs the actor — the kill test is a FormID compare
and `Award` takes a FormID. Both are index-aligned now, and the unresolvable
case reports itself instead of printing an empty block under "0 credited".

### The HUD contradicted itself

It read `0 rap` directly above a follower showing `5`. Both correct — session
versus lifetime — but together they say the mod lost your save. Now labelled
`session` and `total` (#52).

### Also

`StartCombat` **did not take**: returned OK, then `NEVER ENTERED COMBAT`. The
earlier result that looked like success was the confounded single-target case,
so §4.7 standing orders rest on nothing measured (§0.12). Seeds are enabled in
the shipped INI.

## v0.5.0 — the evaluator. Gambits execute.

**First playable.** A follower with a rule list now acts on it: round-robin one
follower per tick, top-down first-match, one action per tick — the FFXII
contract. Conditions are self HP/MP/SP % and player HP % plus `always`; actions
are cast (self/target) and wait.

Set `bSeedEvaluatorRules = 1` to seed `Self HP < 40% -> Cast Healing` and
`Always -> Wait` onto every follower; the rule board arrives at M7.

**The evaluator only runs in combat.** A wounded follower standing in town is
supposed to do nothing — the combat and logistics tables never interleave
(§4.8). Testing out of combat reads as "broken" when it is behaving.

### Deliberately not in this slice

Jitter, urgency tiers, distance LOD, standing orders, drink/equip, and the
logistics table. Each is designed and each is its own build. Standing orders in
particular wait on §4.7's retention question, which is still confounded — no
target latch gets built on an unproven assumption.

### What the reviews caught

Two Fable passes. The first found two blockers that would have made the whole
milestone test nothing:

- **The seed was never called.** Defined, wired to a config key, documented in
  the test guide — and no call site. With no board until M7 that seam is the
  only way a follower ever gets rules, so every follower would have returned on
  an empty table forever. M5 would have shipped, run, logged nothing, and
  looked like a design failure.
- **The "133 ms" tick ran at 500 ms.** It rode the diagnostics pump, whose
  interval existed to pace a HUD redraw, so the self-gate was dead code — the
  caller never arrived faster than it. When that constant moved 2000 -> 500 ms
  for the Field Kit, it silently quadrupled how fast gambits fire. A display
  constant must never set behaviour (#46).

Also fixed: suppression was **absolute**, so a just-fired low-priority rule
deafened a follower to a higher one for the full window — the priority
inversion #26 exists to forbid. It is positional now; a higher rule always
preempts. HP% used `GetPermanentActorValue`, which omits the temporary modifier
where fortify-health gear lives, so heals came late on any buffed follower
(#47) — the Field Kit's bars now share the one formula, because a HUD that
disagreed with the evaluator would lie exactly when consulted. Repeating
failures logged every tick with a synchronous flush (#48). A fast revert->load
could leave two pump threads running (#49).

The verification pass then caught a bug the **first round's own fix**
introduced: the new identity-keyed cursor did not advance past a held null
handle, so one unresolvable follower pinned the entire rotation for up to
~1.6 s — in combat, the only time it matters (#50).

### Engine findings

- **`CastSpellImmediate` DOES deduct magicka** (§0.9). This refutes the
  standing assumption that it is a free scripted cast. §5.3's competence gate
  is load-bearing rather than decorative, and MFO must **not** hand-write a
  deduction — the engine produces that state (#16).
- **`kInstant` is by name the no-animation caster** (§0.10), which likely
  explains the missing cast animation. The probe now fires one variant per
  casting source, and `iCastSource` selects the winner **without a rebuild**.
  This may make `LaunchSpell`/VM dispatch unnecessary.

Folded in from the held batch: level-relative boss detection, `PickFoe`
candidate counting.

## v0.4.1 — review fixes. **Save on this one, not v0.4.0.**

v0.4.0 shipped without a Fable review — a process failure, since the same
build changed the save schema. The review that should have run first came back
with the schema **verified clean** (read/write symmetry, the v1 compatibility
reader, version handling and hostile-input bounds, checked against the actual
shipped v0.1.0–v0.3.0 writers) but found four real defects around it. Two of
them cost you data during ordinary play.

**A follower's miss-streak survived across saves.** The hysteresis that stops
a transiently-unresolvable follower being dropped is keyed by FormID — and the
same NPC has the same FormID in every save, so a streak from one save applied
to the next. From the second save load onward, that re-opened the exact
"a kill in the drop window credits nobody" bug the hysteresis exists to fix.

**A cloned or spawned teammate's first kill created a doomed record.** The
award path used the unguarded record accessor, so an actor with a runtime
FormID — routine in a big load order, and *not* covered by the summon check —
got a record that the save layer then had to throw away every single save.

**The downgrade warning is now on screen**, not only in the log. If a save was
written by a newer MFO than the DLL reading it, saving over it destroys that
data — and nobody reads a log until after they have lost something.

Also: a write failure now says plainly that the save's MFO data is truncated
and to re-save, rather than looking normal; a follower whose handle briefly
fails to resolve now gets the hysteresis hold instead of vanishing silently;
truncation at the rule and override caps logs instead of dropping quietly; and
`iBossLevelDelta` is floored at 1, since 0 would have made every equal-level
kill a boss.

Everything in v0.4.0 below still applies.

## v0.4.0 — scope cut, co-save v2, and the probe harness

**Read this one before installing** — it changes the save schema and removes a
feature.

### Spell acquisition is out of scope

MFO does gambits. It no longer teaches followers spells. *A Fun Way To Level
Followers* (TrumanAE, SKSE, open source) already does that properly — skill
points per player level, perks and spells at 20/40/50/60/80/100, configurable
so it works with any perk or spell overhaul.

The split: **they own acquisition** (how a follower comes to know Fireball),
**MFO owns deployment** (when they should cast it).

This is synergy rather than deconfliction. MFO's aptitude gate reads a
follower's skills to decide what vocabulary they're offered, and without a
levelling system those values barely move — the gate is nearly static. Their
mod makes it live: spend skill points, and MFO's vocabulary opens in response.

Deleted with it: the tutored-spell ledger, the revoke path, the
`RemoveAddedSpells` backstop, and the reconcile-on-load. **That was the
largest remaining surface in MFO that could damage another mod's state** — a
mis-scoped revoke would have eaten spells another mod granted. MFO now adds no
spells or perks to any actor, ever.

MFO's derived action vocabulary still reads whatever a follower knows, so a
spell from a levelling mod, a perk overhaul, or a quest all appear identically
with no patch.

### Co-save schema v2

**v1 saves are still readable**; the v1 reader consumes and discards the old
tutored block. Nothing is lost that MFO owned, and spells already on an actor
are untouched — MFO never held them, it only remembered granting them.

*Why the bump:* the tutored block was first removed at v1 without one, on the
reasoning that no save had ever held an MFO record. That was true when written
and expired as soon as saving with the mod active was on the table — and
v0.3.0 already writes v1 *with* that block, so a v0.3.0 save read by this
build would have misparsed with no guard able to catch it. "Nobody has data
yet" is a fact with an expiry date; a version number is not.

What a save actually carries right now is 14 bytes per follower, all
fixed-width: FormID, rapport, rank, table count, two zero rule counts, zero
overrides. No strings, no form references — those arrive with the evaluator.

### The Probe tab (M4)

A new tab in the Field Kit: pick a follower, fire one engine primitive, watch
what happens. Nothing persists. Its centrepiece is the **target-retention
watch** — press *StartCombat*, and it samples the follower's actual combat
target every tick, comparing by handle rather than name, distinguishing "the
commanded target died" (invalidation) from "the engine re-picked" (a problem),
and reporting how long the commanded target actually survived.

That single measurement decides whether the standing-order model in the design
holds or needs a refresh cadence, and it cannot be answered by reading any
source.

**It already produced its most valuable finding before running.** Review
established that **five of the twelve primitives the design lists as Tier B
have no C++ binding in CommonLibSSE-NG**: `KeepOffsetFromActor`,
`ClearKeepOffsetFromActor`, `SetDontMove` and `DoCombatSpellApply` are
Papyrus-only, and `StartCombat` exists only in po3's fork. The research was
sound — the Papyrus surface named the right engine flows — but *"a Papyrus
native exists"* and *"I can call it from C++"* are different claims and only
the first had been checked. Positioning probes are therefore **blocked** and
shown as such in the UI rather than quietly omitted.

### Also
- `bSeedTestData` writes synthetic rules onto a player-keyed record. Leave it
  **off** for real play; turn it on to exercise the co-save.
- FormID `0x802` shipped as the granted-spell keyword and is now reserved and
  unused. FormIDs are never recycled.

## v0.3.0 — kills actually get credited

Field fixes from reading a real session log. The reported symptom was "boss
multiplier didn't apply"; the log showed something worse underneath it.

**Kills were being silently dropped.** A follower transiently absent from the
engine's high-actor list was removed from the tracked set instantly — and the
death sink refreshes that set *before* awarding, so a kill landing in that
window credited nobody at all. The log signature was a remove and re-add
**117 ms apart**, far tighter than the 500 ms refresh. Now held for three
consecutive missed sweeps: one miss is not evidence of absence.

**The log had gone blind to Rapport.** Awards only logged on a rank change,
so the overlay made Rapport visible in game and invisible in the log in the
same release. Every award and every credited kill now logs at info —
including the zero case — with victim name, both levels, and classification.

**Boss detection was wrong, as reported.** `IsUnique()` means *named one-off
actor*, not *hard fight*; generic dungeon bosses are leveled and not unique,
so a bandit chief with a boss bar read as standard. Now unique **or** at
least `iBossLevelDelta` levels above you (default 5) — relative, so a chief
is a boss at level 8 and an inconvenience at 50. Tunable in `MFO.ini`.

**The panel explains itself.** Measurements now shows the last credited kill:
name, its level, your level, the classification, and what was awarded to how
many followers. "Why wasn't that a boss?" is answerable without a log.

*Two of these became this project's first invariants earned in the field
rather than inherited from a sibling (#22i, #22j).*

*Note: `[cosave] saved 0 follower record(s)` appeared in a session with no
manual save — autosave and quicksave fire regardless of intent. Harmless with
seeding off, but worth knowing that "not saving" is not under your control.*

## v0.2.0 — the Field Kit (in-game overlay)

Everything v0.1.0 had, now **visible in game**. Reading a log after the fact
is a hopeless loop for a behaviour mod, so the overlay was pulled forward
from M7.

**⚠ First code hooks in the project.** Three trampoline hooks (D3DInit,
DXGIPresent, InputDispatch) install at plugin load, **before** the renderer
exists, and they install regardless of any INI setting. Offsets and thunk
shapes are transcribed from MEO's shipped, field-validated implementation and
were confirmed against it in review — but **this build has never run**. If the
game hard-crashes on launch, this is the suspect, and `bShowHud = 0` will not
help because the hooks are already in by then.

**The HUD** — top-right, passive, draws every frame and takes **no input**, so
it stays readable while fighting. Per follower: name, combat flag, rank,
rapport, live health/magicka/stamina bars, distance. Plus session kills,
rapport, and rapport/hour. This is the primary observation surface; the panel
is for detail. `bShowHud` in `MFO.ini`.

**The panel** — the **Field Orders** power opens it; Esc, gamepad B, or the
shout key closes it. Three tabs:
- *Followers* — the full table, **including retained-but-inactive records**,
  so dismissal being non-destructive is visible rather than taken on faith.
- *Measurements* — the two numbers this build exists to take: the
  teammate-filtered combat-event rate, and kills/rapport per hour. The
  rapport/hr figure turns amber below 30, which is the case where
  `BALANCE.md`'s rank ladder needs redoing.
- *Config* — what is actually in force, plus quirk-table resolution.

Full controller parity throughout (family standing rule): gamepad nav, stick
edge-triggered into d-pad, B to close.

**Also**
- Test seeding moved to `bSeedTestData` in `MFO.ini`, **default off**. With it
  off and no saving, **MFO writes nothing anywhere** — it reads state and
  draws it.
- Detection refresh 2 s → 500 ms so the HUD reads as live.
- Log now lands in the MO2 profile's `overwrite/SKSE/Plugins/MFO.log`,
  alongside every other plugin's, rather than in the wine prefix.

**Validated in-game (from v0.1.0 testing)** — follower detect/undetect across
three cycles with records retained, form resolution to the ESL band, SPIT
type 3 correct for a castable lesser power, `TESSpellCastEvent` firing for
lesser powers. See `ENGINE_NOTES.md` §0.

**Still unproven, deliberately:** the co-save (no saving with MFO active yet),
all of Rapport (no kills have occurred), and combat-event volume.

**Review:** Fable pre-CI review found one blocker — a software mouse cursor
that would have been composited over ordinary gameplay from the moment the
renderer came up — plus four majors including a mutex-nesting violation of the
project's own invariant and an input-swallow divergence from the reference.
All fixed before this build.

## v0.1.0 — first testable build

Detection, Rapport, and the co-save. **No gambit execution yet** — the
evaluator arrives at M5. This build exists to prove the foundations before
anything is built on them.

**What it does**
- Loads, resolves its forms, and grants the **Field Orders** power.
- Detects followers framework-agnostically: teammate status as the gate, plus
  a quirk table that revokes eligibility for custom followers who signal
  dismissal their own way (Inigo's `WaitingForPlayer == -1`, Vilja's and
  Tindra's factions). Summons excluded by default and never persisted.
- Accrues **Rapport** per follower from shared kills — combat state carries
  the archery case, a radius fallback carries the stealth case — with boss
  ×5 and dragon ×10, and ranks I–V driving both slot ladders.
- Round-trips all of it through the SKSE co-save, with `ResolveFormID` on
  every FormID and per-rule disable (never whole-list drop) for missing
  targets.

**How to observe it.** There is no UI until M7. Press **Field Orders** at any
time to dump a full state report to the log: config in force, the quirk
table, active followers with their eligibility flags, every stored record
including dismissed ones, and the two measurements this build exists to take
— teammate-filtered combat-event rate, and kills/hour.

**Log location.** `<MO2 instance>/overwrite/SKSE/Plugins/MFO.log`. MFO writes
game-root-relative so USVFS lands its log beside every other plugin's, rather
than in the wine prefix where `SKSE::log::log_directory()` would put it.

**Known and deliberate**
- Ships with seeded test gambits on a player-keyed record (`kP0SeedTestData`),
  so the co-save round-trip is provable without a UI. **Use a throwaway save.**
- MCM settings need a restart; there is no live re-read sink until M7.
- The combat-exit survival award is not implemented; `fRapportSurvival` is
  parsed but unused.
- `BALANCE.md`'s rank thresholds rest on an **unmeasured** ~45 rapport/hour
  estimate. Taking that measurement is the main purpose of this build.

**Engineering notes**
- Zero code hooks. Event sinks only (death, combat, spell-cast), so there is
  no Address Library exposure in the core loop.
- Passed a pre-CI Fable review that caught two save-corruption paths — a
  diagnostic that inserted records via `operator[]`, and an unguarded path
  that could persist runtime `0xFF` FormIDs — plus six behaviour defects,
  including a kill counter that counted every death in the world rather than
  party kills.
