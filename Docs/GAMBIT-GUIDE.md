# MFO Gambit Guide — writing follower orders that actually work

A **gambit** is one line: **[When …] → [Action]**. Your follower reads their list top to bottom every heartbeat, and the **first line that's true *and can actually act* fires — then it stops.** That's the whole engine. Everything below is how to make it sing.

Four things to hold in your head:

1. **First match wins, top to bottom.** Order *is* the logic. A line that's always true — and always *able* to act — high up will smother everything under it.
2. **One action per beat.** A follower does exactly one real thing per beat — so *the cost of a heal is the attack not made.* After acting they also hold a short human pause (~1.5 s, tunable) before acting again; only a line *above* the one that fired can break in early — which is exactly why the urgent stuff goes on top.
3. **Two separate tables.** The **Combat** table runs while fighting; the **Logistics** table runs the rest of the time (looting, drinking, restocking, waiting). They never interleave — combat rules can't loot, logistics rules can't attack.
4. **A line that can't act is skipped, not a wall.** Competence is not permission — no spell in their book, not enough magicka, no bow in the pack, potion stack empty, target out of the spell's range → the line *falls through* and the next one gets its turn that same beat. This transparent skip is load-bearing: it's what makes every fallback pattern below work. The board remembers *why* a line held (hover it), so a list that misbehaves is legible, not mysterious.

---

## Tip spotlight: **Equip Melee Weapon** (not just "Attack")

This is the one most people miss. **Attack** says *"fight the chosen foe with whatever's in your hands."* **Equip melee weapon** / **Equip ranged weapon** do something stronger — they set the follower's whole **stance**: they draw the *best* weapon of that category from their own pack *and* fight like that kind of fighter (since v1.0.53 a melee-stanced follower actually closes in and holds ground instead of back-pedalling like a caster; a ranged one keeps bowman spacing). The stance then *holds* until the fight ends or another equip line flips it.

Two properties make these lines special:

- **They're idempotent.** Already holding the right category → the line quietly *passes through* and the rules below it run. So an equip line high in the list does **not** smother anything — it arms the follower, then gets out of the way. (`Always → Attack` walls the list; `Always → Equip ranged weapon` doesn't.)
- **They claim the hand for the beat.** A contradictory equip lower in the list is skipped, so a mixed list can't thrash melee↔ranged.

Use them to *control the engagement*, not just start it:

- **Stop an archer from kiting a dangerous target:**
  `Foe targeted within range (400) → Equip melee weapon`, then below: `Foe: nearest → Attack`.
  When the foe they're fighting closes the distance, they draw a blade and commit instead of backpedalling into a wall.
- **Peel off a bruiser:**
  `Foe attacking me: melee → Equip ranged weapon`, and *below it* `Foe attacking me: melee → Attack`.
  The equip line swaps the weapon (it doesn't pick targets by itself); the Attack line under it aims them at that same attacker. First beat they arm, next beat the equip passes through and they engage.
- **Pin a follower to a role** regardless of what they picked up:
  `Always → Equip ranged weapon` at the top of a bow-user's table keeps them on the bow even after they loot a shiny sword.

**Pattern:** put the *stance* line above the *Attack* line. They arm the way you want, then engage. Attack alone leaves the weapon choice to the engine.

> Note the exact wording: **"Foe targeted within range" / "Foe targeted beyond range"** measure the distance to the foe your follower is *currently fighting* — not to some other enemy across the room — and read false when they have no target yet. That's deliberate: it makes a within/beyond pair key off the *same* foe.

> Related: the **Class** picker on the board (Auto / Melee / Ranged / Mage) does this *globally* for a follower, and it **wins over switch-gambits** when both are set. Use the picker for "she is always a mage"; use equip-gambits for "melee *when* the fight closes." **Auto** changes nothing (safe for custom followers).

---

## Picking *which* foe

Most **Foe** conditions are also **selectors** — they choose the target the action then uses. This is where the FFXII flavor lives:

- `Foe: weak to fire → Cast at foe/ally (Flames)` — targets the nearest enemy with an *actual* fire weakness (a real negative resistance, not a guess by name). Same for frost / shock.
- `Foe: lowest HP → Attack` — focus-fire the one about to drop. `Foe: highest HP → …` to chip the tank.
- `Foe is a spellcaster → …` or `Foe is ranged → …` — go after the backliners. These read what the foe is *actually holding right now*, not their character sheet.
- `Foe attacking player → Attack` — a bodyguard rule: they peel whatever's on you.
- `Foe attacking me: melee` / `Foe attacking me: ranged` — react to *who's hitting you and how.*
- `Foe is blocking → …` (turtling — power-attack it), `Foe is fleeing → …` (finish or let go), `Foe is weaker than me`, `Foe is undead`, `Foe is dragon`.
- `Foe targeted within range` / `Foe targeted beyond range` — range-gate a line, measured to the foe they're currently fighting.
- `Foe count at least (N) → …` — react to being swarmed. **This one is a gate, not a selector** — it counts but picks nobody, so pair it with `Flee to player`, `Wait`, an equip or a self-cast, not with `Attack`.

Two quiet rules keep selectors sane: a selector that finds nobody is simply *false* (the line falls through — that's why `Foe: lowest HP → Attack` can sit above `Always → Wait`), and distance-blind selectors never pick a foe beyond the follower's **confidence-scaled chase radius** — a healthy follower ranges across the field, a hurt or mobbed one only fights what's on top of them. No more suicide charges across a Falmer pack to reach "the weakest".

Order the specific selectors **above** a plain `Foe: nearest → Attack` catch-all, or the catch-all eats them.

---

## Casting well

- Combat cast actions: **Cast on self** and **Cast at foe/ally**. Each takes a specific spell. (**Cast on player** lives in the *Logistics* table — out-of-combat buffs and top-ups. In combat, `Player HP % below → Cast at foe/ally` makes *you* the target.)
- A cast at a chosen target **beyond the spell's own range is skipped** — transparently, so the line *below* it still gets a turn that beat. Put a fallback under a long-range nuke.
- **Utility casts fall through to you.** `When dark → Cast at foe/ally (Magelight)` with no target around resolves to the player — it lights up around *you* instead of quietly doing nothing. ("When dark" = interior *or* night, in one condition; "In an interior" and "At night" also exist separately.)
- **Concentration spells are streamed in bounded bursts** (v1.0.58): a hostile stream like Flames runs 1–4 s (each follower's burst length is consistently their own), a heal stream holds until the target tops off, and a hostile beam is *held or cut the instant a teammate crosses the line of fire*. Streams also need line of sight — no flames into a wall.
  **Caveat:** this covers casts *at a target*. A concentration spell **on self** (vanilla *Healing*) can't be streamed yet — that line fails legibly and falls through. Give self-heals as fire-and-forget spells instead: *Fast Healing*, *Close Wounds*.
- Remember competence: the spell isn't in their book, or the magicka isn't there → the line fails and the next is tried. Give casters a fallback: `Self Magicka % above (20%) → Cast at foe/ally (Firebolt)`, then below, `Always → Equip melee weapon` for when they run dry.
- (Cast gambits are for current Skyrim (AE). On old SE 1.5.97 / VR they stand down and the follower's own AI does the casting.)

---

## Staying alive

- **Self-preservation goes at the very top of the combat table** so it out-prioritizes attacking:
  `Self HP % below (40%) → Drink health potion` · `Self Magicka % below (25%) → Drink magicka potion`.
  Drinking works mid-fight and is cooldown-gated per resource, so a winning rule can't chug the whole stack.
- **Heal your allies / yourself:** `Ally HP % below (30%) → Cast at foe/ally` with a healing spell — the ally condition targets the *most wounded* teammate in range. `Player HP % below (25%) → Cast at foe/ally` heals you.
- **Flee to player** disengages and retreats to you, releasing on arrival or when the fight ends. Good as a swarm valve: `Foe count at least (4) → Flee to player`. It's the explicit version of the invisible **confidence leash** — a hurt or badly mobbed follower already tightens up and (with Auto-Retreat on, the default) falls back to you on their own.
- Because it's one-action-per-beat, a follower that's constantly healing isn't attacking. Tune the threshold (40% vs 60%) to trade survivability for damage.

---

## The Logistics table (out of combat)

Separate list, runs when the fight's over (about once a second — calm work at a calm pace):

- **Loot:** `Loot better equipment`, `Loot gold`, `Loot jewellery`, `Loot potions (any)` (or the per-type versions), `Loot arrows` / `Loot bolts`, `Loot soul gems`, `Loot lockpicks`. They *walk* to it, judge upgrades by the role they actually fight in (an archer won't grab a mace), give you a few seconds of **first dibs** on fresh corpses — waived the moment you've looted that body yourself — and never touch owned goods or a container you have open.
- **Restock/supply:** `Arrows below (10) → Loot arrows` — the number is a count, "fewer than N carried". With the follower economy enabled (off by default), they'll also buy those shortfalls at a merchant.
- **Wait / hold:** `Carry weight % above (90%) → Wait` stops a follower from looting themselves immobile — **Wait** deliberately ends the sweep, gating every line below it.
- Drinks live here too for calm top-ups, and so does out-of-combat casting: self-buffs, `Cast on player` for heals on you, `When dark → Cast on self (Candlelight)`, `Equip torch`.

---

## Little tips & gotchas

- **A too-general *acting* line high up is the #1 mistake.** `Always → Attack` at the top means nothing below it ever runs (an active Attack is a commitment, not a pass-through). Keep `Always → …` and `Foe: nearest → …` at the **bottom** as catch-alls. (The exception, as above: a satisfied equip line is transparent and safe anywhere.)
- **Give every caster a melee/ranged fallback** below their cast lines, for when magicka's gone.
- **Vitals are percentages**, so they scale with the follower's max — 40% works whether they have 200 or 600 health, fortify gear included.
- **Range-gate to split behavior:** `Foe targeted beyond range (800) → Equip ranged weapon` above `Foe targeted within range (300) → Equip melee weapon` turns one follower into a proper skirmisher — both lines read the *same* engaged foe, so they can't fight each other.
- **Focus fire wins fights:** a `Foe: lowest HP → Attack` line makes a group actually kill things instead of everyone poking a different enemy.
- **They react like people, not machines.** A beat of readiness at the start of a fight (~a quarter second, each follower's own), a visible "sizing up" hesitation before abandoning a live target for a new one (self-defense is exempt — `Foe attacking me` stays snappy), and per-follower timing quirks that never change: Lydia is consistently the deliberate one. In a large party the shared heartbeat also comes around a little less often per follower.
- **Rapport buys slots, not speed.** Fighting alongside a follower ranks them up and unlocks more rule **slots** (combat 3 → 12, logistics 4 → 8), and it survives dismissal. It does *not* change their reaction time.
- **The board explains itself.** A rule that fired flashes a green **\***; hover a rule to see the last reason it held ("insufficient magicka", "no melee weapon carried", "target beyond range…"). Read it before assuming a rule is broken.
- **Nothing is scripted to a named follower.** Every rule works on anyone your framework treats as a teammate — vanilla or modded.

---

## How gambits sit alongside the rest of MFO

- **Class picker** (on the board) = a global stance override (Auto/Melee/Ranged/Mage) that outranks equip-gambit stances. **Auto** changes nothing (safe for custom followers). Equip-gambits are the *conditional* version of the same idea.
- **Confidence leash** = how far they roam vs hug you, read live from how safe they feel — it shrinks their loot range *and* their combat chase range when they're hurt or outnumbered. Auto-Retreat and `Flee to player` are its explicit hooks.
- **Rapport** = earned rule slots, per follower, surviving dismissal.

Build small, watch one fight, reorder, repeat. A three-line table that's ordered right beats a twelve-line one that isn't.
