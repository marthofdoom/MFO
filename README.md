# marth's Follower Overhaul (MFO)

Programmable follower behaviour for Skyrim SE. Every follower carries ordered
lists of **Gambits** — `[Condition] -> [Action]` rules, first match wins —
authored live, in game, per follower. Final Fantasy XII's gambit system
rebuilt on the engine's own actor primitives.

**Status: v0.3.0, pre-alpha.** Follower detection, Rapport progression, and an
in-game Field Kit overlay work and are validated in play. **Gambit execution
is not built yet** — the evaluator is M5 in [`ROADMAP.md`](Docs/ROADMAP.md).

---

## Two tables, not one

A follower carries **two independent rule lists** with separate slots. They
never interleave: combat runs in combat, logistics runs out of it.

### The combat table

```
1.  Ally: HP < 40%          ->  Cast Fast Healing
2.  Foe: weak to fire       ->  Cast Flames
3.  Self: magicka < 20%     ->  Drink magicka potion
4.  Always                  ->  Attack lowest-HP foe
```

Top-down, first match wins, **one action per tick**. A follower cannot heal
and attack in the same cycle — the cost of a heal is the attack not made.

### The logistics table

Upkeep, so a follower who runs dry of arrows or is still wearing their
recruitment armour isn't something you have to micromanage.

```
1.  Health potions < 3      ->  Loot health potions
2.  Arrows < 20             ->  Loot ammo for my bow
3.  Better heavy armour near ->  Loot and equip it
```

Equipment rules generalise by **category and metric, never by item** — "loot
better heavy armour" means higher armour rating *weighted by that follower's
own armour skill*, computed at runtime. Modded gear works with no patch, and a
light-armour follower isn't upgraded into heavy just because the number is
bigger. Potions are health/stamina/magicka only; that's what self-sufficiency
means, and every extra picker entry is a tax on reading the list.

**First dibs are yours.** A follower won't touch a corpse or container for 25
seconds, and never takes owned goods. Once *you've* taken from it the wait
drops to ~4 seconds — not zero, because QuickLoot takes items one at a time
and a follower shouldn't grab from a body you're still working through.

---

## How it behaves

**Gambits layer on top of vanilla AI; they never replace it.** A follower with
no matching rule behaves byte-identically to one without MFO installed — no
no-op package, no neutral command, no engine call at all.

**Reactions are human, not robotic.** A gambit decision is a *choice*
reaction, which in people runs 300–600 ms, so responses are drawn from a
right-skewed distribution rather than firing on a metronome. Followers
occasionally miss a beat. A companion that reacts at reflex speed to a
tactical situation reads as a machine.

**Competence is not permission.** Teaching a follower a spell doesn't mean
they can cast it — if they lack the magicka or the skill, the rule simply
fails and the next one is tried. The board tells you *why*, so a list that
doesn't work is legible rather than mysterious.

**The list is yours.** MFO executes it as written: no reordering, no
deduplication, no coordination between followers, no penalty for filling every
slot. Two followers both healing the same ally is your authoring, and the fix
is yours to make.

---

## Progression

Slots and vocabulary are earned through **Rapport**, built by fighting
alongside that specific follower. It's never pooled or transferable, and it
survives dismissal — leave someone in Breezehome for two hundred hours and
they're exactly as you left them.

| Rank | Combat slots | Logistics slots | Reactions |
|---|---|---|---|
| I | 2 | 1 | visibly hesitant |
| III | 6 | 3 | ↓ |
| V | 12 | 5 | anticipates |

What unlocks depends on that follower's own skills — a battlemage opens spell
actions a pure warrior never will. At high Rapport MFO can *teach* spells, and
un-assigning the rule takes the spell back.

---

## The Field Kit

There's an in-game overlay, because reading a log after the fact is a hopeless
way to develop a behaviour mod.

- A **passive HUD** that takes no input, so it stays readable while fighting:
  per-follower rank, rapport, live health/magicka/stamina, distance.
- A **panel** on the Field Orders power with the follower table, live
  measurements, config in force, and (currently) an engine-probe harness.

Full controller parity throughout — gamepad navigation is a standing
requirement across these mods, not an afterthought.

---

## Docs

Start at [`Docs/INDEX.md`](Docs/INDEX.md), which sets the read order and marks
which documents are specs versus which record proven behaviour.

- [`DESIGN.md`](Docs/DESIGN.md) — the spec. §4.5 splits the mod into proven
  and unproven engine ground; read it before estimating anything.
- [`BALANCE.md`](Docs/BALANCE.md) — the Rapport ladder and its content budget
- [`ARCHITECTURE.md`](Docs/ARCHITECTURE.md) — subsystems, threads, co-save schema
- [`INVARIANTS.md`](Docs/INVARIANTS.md) — read before any code change
- [`ANTI_PATTERNS.md`](Docs/ANTI_PATTERNS.md) — the portable "never again" list
- [`ENGINE_NOTES.md`](Docs/ENGINE_NOTES.md) — what is proven vs merely researched
- [`ROADMAP.md`](Docs/ROADMAP.md) — build order to a shippable mod

## Building

C++ lives in `native/`. The DLL is built by GitHub Actions on
`windows-latest` — CommonLibSSE-NG needs the MSVC linker and there is no Linux
cross-build, so **CI is the only compiler that ever sees this code.** `git
push` is the build button.

Releases are cut with `./release.sh`, which refuses a dirty tree, refuses if
`native/` has drifted from the last green CI run, and records commit, run id
and artifact hashes in a manifest. vcpkg registry baselines are **pinned
deliberately** — bump them on purpose, never float them.

## Licence

MFO's own code is MIT — see [`LICENSE`](LICENSE). All of it lives in `native/`;
no third-party source is vendored here. The shipped DLL statically links
CommonLibSSE-NG (MIT, © 2018 Ryan-rsm-McKenzie) and its dependencies; their
notices are in [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md), which ships
with every release.
