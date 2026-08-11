# MFO Gambit Guide — writing follower orders that actually work

A **gambit** is one line: **[Condition] → [Action]**. Your follower reads their list top to bottom every heartbeat, and the **first line whose condition is true fires — then it stops.** That's the whole engine. Everything below is how to make it sing.

Four things to hold in your head:

1. **First match wins, top to bottom.** Order *is* the logic. A line that's always true, high up, will smother everything under it.
2. **One action per beat.** A follower does exactly one thing per tick — so *the cost of a heal is the attack not made.* Don't stuff a table; every line you add competes for that one slot.
3. **Two separate tables.** The **Combat** table runs while fighting; the **Logistics** table runs the rest of the time (looting, drinking, restocking, waiting). They never interleave — combat rules can't loot, logistics rules can't attack.
4. **Competence is not permission.** You can't order what a follower can't do — no spell in their book, no magicka, no skill → the line simply fails and the next one is tried. The board tells you *why*, so a list that misbehaves is legible, not mysterious.

---

## Tip spotlight: **Switch to Melee** (not just "Attack")

This is the one most people miss. **Attack** says *"fight the chosen foe with whatever's in your hands."* **Switch to Melee** / **Switch to Ranged** do something stronger — they set the follower's whole **stance**: which weapon they draw *and* how they move (since v1.0.53 a melee follower actually closes in and holds ground instead of kiting).

Use the switch actions to *control the engagement*, not just start it:

- **Stop an archer from kiting a dangerous target:**
  `Foe within 400 → Switch to Melee`, then a line below: `Foe: any → Attack`.
  When something closes the distance, they draw a blade and commit instead of backpedalling into a wall.
- **Peel a bruiser off a squishy follower:**
  `Foe attacking me (melee) → Switch to Ranged` — they disengage the melee and put arrows in it from range.
- **Force a caster to stay a caster (or a warrior to stay a warrior)** regardless of what they picked up:
  `Always → Switch to Ranged` at the top of a bow-user's table pins them to the bow even after they loot a shiny sword.

**Pattern:** put the *stance* line above the *Attack* line. They arm the way you want, then engage. Attack alone leaves the weapon choice to the engine.

> Related: the **Combat Class** dropdown on the board (Auto / Melee / Ranged / Mage) does this *globally and permanently* for a follower. Use the dropdown for "she is always a mage"; use switch-gambits for "melee *when* something gets close."

---

## Picking *which* foe

The **Foe** conditions are also **selectors** — they choose the target the action then uses. This is where the FFXII flavor lives:

- `Foe: weak to Fire → Cast Flames` — targets the nearest enemy with an actual fire weakness. (Same for Frost / Shock.)
- `Foe: lowest HP → Attack` — focus-fire the one about to drop. `Foe: highest HP → …` to chip the tank.
- `Foe: is a caster → …` or `Foe: is ranged → …` — go after the backliners first. Pair with **Switch to Ranged** or a silence/interrupt spell.
- `Foe attacking the player → Attack` — a bodyguard rule: they peel whatever's on you.
- `Foe attacking me (melee)` / `Foe attacking me (ranged)` — react to *who's hitting you and how.*
- `Foe: blocking → …` (they're turtling — power-attack or reposition), `Foe: fleeing → …` (finish or let go), `Foe: weaker than me`, `Foe: is undead`, `Foe: is a dragon`.
- `Foe within <units>` / `Foe beyond <units>` — range-gate a line (measured to the current target).
- `Foe count at least N → …` — react to being swarmed (see Flee, below).

Order these specific selectors **above** a plain `Foe: any → Attack` catch-all, or the catch-all eats them.

---

## Casting well

- Actions: **Cast on Self**, **Cast at Target**, **Cast on the Player**. Each takes a specific spell.
- A cast whose target is **out of the spell's range is skipped** — so the line *below* it still gets a turn that beat. Put a fallback under a long-range nuke.
- **World/utility casts fall through to you.** `When dark → Cast Magelight` with no enemy around lights up around the player instead of quietly doing nothing. ("When dark" = interior/dungeon *or* night, in one condition.)
- **Concentration spells (Flames, Frostbite, Sparks, Healing) are held for a bounded time now** — a short burst for offense, until-healed for heals — and won't fire into a teammate standing in the beam.
- Remember competence: no magicka or the spell isn't in their book → the line fails and the next is tried. Give casters a fallback: `Self MP above 20% → Cast Firebolt`, then below, `Always → Switch to Melee` for when they run dry.

---

## Staying alive

- **Self-preservation goes at the very top of the combat table** so it out-prioritizes attacking:
  `Self HP below 40% → Drink Health Potion` · `Self MP below 25% → Drink Magicka Potion`.
- **Heal your allies / yourself:** `Ally HP below 30% → Cast Healing`, `Player HP below 25% → Cast Healing Hands`.
- **Flee** disengages and retreats to you. Good as a swarm/outmatched valve: `Foe count at least 4 → Flee`. It works with the invisible **confidence leash** — a badly outmatched follower already pulls back on their own; Flee is the explicit version.
- Because it's one-action-per-beat, a follower that's constantly healing isn't attacking. Tune the threshold (40% vs 60%) to trade survivability for damage.

---

## The Logistics table (out of combat)

Separate list, runs when the fight's over:

- **Loot:** Equipment, Gold, Jewelry, Potions (or specific potion types), Arrows/Bolts, Soul Gems, Lockpicks. They *walk* to it, judge upgrades by class-and-metric (an archer won't grab a mace), and leave your owned goods alone.
- **Restock/supply:** `Out of Arrows → Loot Arrows`. With the economy enabled, they'll also buy shortfalls at a merchant.
- **Wait / hold:** `Carry weight above 90% → Wait` stops a follower from over-looting into encumbrance. **Wait** is also a plain "do nothing" you can park at the bottom of a table.
- Drinks live here too for the calm top-ups: `Self: low on health potions → …` etc.

---

## Little tips & gotchas

- **A too-general line high up is the #1 mistake.** `Always → Attack` at the top means nothing below it ever runs. Keep `Always → …` and `Foe: any → …` at the **bottom** as catch-alls.
- **Give every caster a melee/ranged fallback** below their cast lines, for when magicka's gone.
- **Vitals are percentages**, so they scale with the follower's max — 40% works whether they have 200 or 600 health.
- **Range-gate to split behavior:** `Foe beyond 800 → Switch to Ranged` / `Foe within 300 → Switch to Melee` turns one follower into a proper skirmisher.
- **Focus fire wins fights:** a `Foe: lowest HP → Attack` line makes a group actually kill things instead of everyone poking a different enemy.
- **They react like people, not machines** — 300–600 ms, occasionally a missed beat. Higher **Rapport** visibly tightens that up, and Rapport also unlocks more rule **slots** and richer **vocabulary** for that specific follower (earned by fighting alongside *them*).
- **The board explains itself.** When a line doesn't fire, the summary tells you whether the condition was false or the follower simply couldn't (no magicka, no skill, target out of range). Read it before assuming a rule is broken.
- **Nothing is scripted to a named follower.** Every rule works on anyone your framework treats as a teammate — vanilla or modded.

---

## How gambits sit alongside the rest of MFO

- **Combat Class dropdown** = a global stance override (Auto/Melee/Ranged/Mage). **Auto** changes nothing (safe for custom followers). Switch-gambits are the *conditional* version of the same idea.
- **Confidence leash** (MCM) = how far they roam vs hug you, read from how safe they are. Flee/retreat are its explicit hooks.
- **Rapport** = earned slots + vocabulary, per follower, surviving dismissal.

Build small, watch one fight, reorder, repeat. A three-line table that's ordered right beats a twelve-line one that isn't.
