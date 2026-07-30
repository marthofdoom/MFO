# marth's Follower Overhaul (MFO)

Programmable follower behaviour for Skyrim SE. Every follower carries ordered
lists of **Gambits** — `[Condition] -> [Action]` rules, first match wins —
authored live, in game, per follower. Final Fantasy XII's gambit system
rebuilt on the engine's own actor primitives.

**Status: v0.8.0, pre-release.** Gambits execute — both tables. Follower
detection, Rapport, the in-game board, and combat + logistics actuation are
built and field-tested; followers cast (their own AI, animated, at chosen
targets), attack, drink, restock, loot, and now *walk* to loot. Active
development, so 0.x: newest features want hardware tuning. Build order and
what's still deferred are in [`ROADMAP.md`](Docs/ROADMAP.md); the per-version
history is in [`CHANGELOG.md`](CHANGELOG.md).

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
2.  Out of arrows           ->  Loot arrows
3.  Always                  ->  Loot better equipment
4.  Always                  ->  Loot gold
```

The vocabulary: drink and loot health/stamina/magicka potions (or **any**
potion), loot arrows and bolts (separate gambits), loot gold, and **loot
better equipment**. Equipment generalises by **category and metric, never by
item** — a weapon upgrade is judged within the follower's dominant weapon-skill
class (a two-hander takes the greatsword over a dagger, an archer won't swap to
a mace), armour by rating on the slot. Modded gear works with no patch, and a
caster won't hoover up a random sword.

**Followers loot like people, not vacuums:**

- **They walk to it.** A follower paths to the loot and picks it up — they
  don't teleport-grab from across the room. How far they'll range is the
  *confidence leash* below.
- **They can pick locks they're skilled enough for** — a Novice lock at any
  Lockpicking skill, an Expert lock only near mastery; Master and key-locked
  are out of reach.
- **First dibs are yours.** Arrows and potions a follower restocks at once, but
  it leaves gear and gold on a fresh corpse for a few seconds first — your pick.
  It never takes owned goods, and holds off entirely while **you're sneaking**
  so they don't blow your stealth. Once *you've* taken from a source the wait
  collapses; you've had your look.

---

## How it behaves

**Gambits layer on top of vanilla AI; they never replace it.** A follower with
no matching rule behaves byte-identically to one without MFO installed — no
no-op package, no neutral command, no engine call at all.

**Confidence, on an invisible string.** How far a follower operates *from you*
isn't a fixed follow distance — it's a live readout of how confident they are
to survive on their own right now. Healthy in an easy zone, they push ahead and
range out to loot; hurt, or in a hard fight, they pull back and fight at your
side. You never see a number. You just feel a companion get bold clearing a
bandit camp and wary in a dragon's lair. It tunes on the new **Behaviour Layer**
config page, and it's the model for how MFO adds realism: hidden variables you
read through behaviour, not sliders.

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

Rule **slots** are earned through **Rapport**, built by fighting alongside that
specific follower. It's never pooled or transferable, and it survives dismissal
— leave someone in Breezehome for two hundred hours and they're exactly as you
left them. (Gating the *vocabulary* by rank — not just slot count — is designed
and next on the roadmap.)

| Rank | Combat slots | Logistics slots | Reactions |
|---|---|---|---|
| I | 2 | 1 | visibly hesitant |
| III | 6 | 3 | ↓ |
| V | 12 | 5 | anticipates |

What a follower *can* be told to do depends on their own skills and spellbook —
a battlemage opens cast actions a pure warrior never will, and the board shows
each follower only what they could actually perform.

---

## The board — Field Orders

Rules are authored **in game**, per follower, on the **Field Orders** power —
because reading a log after the fact is a hopeless way to develop a behaviour
mod.

- A **passive HUD** that takes no input, so it stays readable while fighting:
  per-follower rank, rapport, live health/magicka/stamina, distance.
- The **Field Orders board** (titled *Follower Overhaul*): a Followers roster
  and the Gambits editor — cycle each rule's condition, action, and value, with
  a full-width plain-language summary of every rule so a whole gambit is legible
  even on a Steam Deck. Styled after MEO, in four skins.

Full controller parity throughout — gamepad navigation is a standing
requirement across these mods, not an afterthought. Settings live in an
**MCM** (MCM Helper), including the Behaviour Layer page for the confidence
leash and walk-to-loot.

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
- [`GAMBIT_LIBRARY.md`](Docs/GAMBIT_LIBRARY.md) — the condition→action vocabulary
- [`ROADMAP.md`](Docs/ROADMAP.md) — build order to a shippable mod
- [`CHANGELOG.md`](CHANGELOG.md) — per-version history

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
