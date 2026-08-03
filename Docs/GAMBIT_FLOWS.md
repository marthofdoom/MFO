# GAMBIT_FLOWS — natural combat flows, hand arbitration, and processing heuristics

*Design document (task #43). Analysis + recommendations only — no engine changes are
made by this doc. Reviewed against the code as of v1.0.11 (`7d4266b`, `5e3a4ce`).*

---

## 0. Ground truth (verified against the code)

| Fact | Where |
|---|---|
| Two tables (combat / logistics), never interleaved | `Scheduler.cpp:165` (branch on `IsInCombat`), `Evaluator.h:27` |
| First-match-wins, scan restarts at rule 0 every tick | `Evaluator.cpp:360-399`, DESIGN §4.3 |
| One follower serviced per 133 ms tick, round-robin | `Scheduler.cpp:24,119-146` |
| **Combat** table: the winning rule consumes the tick, whatever `Fire()` returns | `Scheduler.cpp:255,291` — `Evaluate(f, it->second)` is only ever called once, with `startIndex = 0` |
| **Logistics** table: fall-through scan — a matched rule whose action does nothing is skipped via `startIndex = ruleIndex + 1` | `Logistics.cpp:2460-2507`, `Evaluator.h:36-40` |
| Suppression is positional: after a `Fired` at rule *k*, rules ≥ *k* are quiet for `fSuppressWindow` (default 1.5 s); rules < *k* may preempt | `Scheduler.cpp:283-289`, `Config.cpp:190` |
| Only `Fired` buys suppression; `NoOp` / `Failed*` do not | `Scheduler.cpp:306-333` |
| `EquipWeapon` is idempotent: `NoOp "already holding that category"` when the right hand already holds that class | `Actuation.cpp:331-334` |
| Cast = equip spell into a hand (Loadout ledger) + let the follower's own AI cast (consent + grace), silent fallback after `fAiCastGrace` (3 s) | `Actuation.cpp:104-153`, `Loadout.cpp:175-257` |
| The spell is released the moment the *final winning choice* is not a cast | `Scheduler.cpp:265-268` |
| A cast the follower cannot afford fails with a reason (competence gate + optional reserve) and internally releases the spell | `Actuation.cpp:37-75` |
| Attack = target latch, re-asserted by the UpdateCombat hook; re-commanding the same foe is `NoOp "already on that target"` | `Actuation.cpp:377-411` |
| `act.flee` / auto-retreat: while the retreat alias is held, the gambit table is **not evaluated at all** | `Scheduler.cpp:215-239`, `Actuation.cpp:432-440` |
| Weapon loot only STOCKS the pack, gambit-driven roles decide what is maintained; the equip gambits move weapons into the hand | commit `7d4266b`, `Logistics.cpp:399-422` |
| Torch is logistics-only upkeep (out of combat) | `Actuation.cpp:429-430`, `Logistics.cpp:2492` |
| Rules have exactly ONE condition; there is no AND. Ordering + the competence gate are the only composition operators | `State.h` gambit shape, GAMBIT_LIBRARY.md |

**The single most important discrepancy found:** `Actuation.h:15` documents
`FailedSkill` as *"the follower could not afford/execute it → fall through"* — but in
the combat table **no fall-through exists**. The tick simply ends
(`Scheduler.cpp:291` fires once and returns). The logistics table got fall-through
because of the shadowing bug ("loot potions" starving "loot equipment",
`Logistics.cpp:2460-2467`); the combat table has the identical disease and never got
the cure. Most defects below are instances of this one gap.

---

## 1. The hand-arbitration model

### 1.1 The resources and the claimants

Two hand slots. Five mechanisms compete for them:

| Claimant | Slot(s) | Persistence | Tracked? | Where |
|---|---|---|---|---|
| **Equip gambit** (`act.equip_melee/ranged`) | Right (2H/bow claims both) | Persistent until something else equips | NOT ledgered (deliberate: it is the follower's authored loadout, not a loan) | `Actuation.cpp:330-357` |
| **Cast loadout** (spell for a cast rule) | Left hand (off-hand pivot); 2H must stow | Transient — held only while a cast rule keeps winning | Ledgered + restored (`g_debt`) | `Loadout.cpp:175-257`, DESIGN §4.5b |
| **Shield** | Left | MFO never equips one (denied vocab); it only *restores* one it displaced — on hit, combat end, dismissal | Ledgered | `Loadout.cpp:326-334,392-399` |
| **Torch** | Left | Logistics upkeep, out of combat only | idempotent | `Logistics.cpp:2492` |
| **The follower's own AI** | Both | Any time — vanilla AI runs throughout (§4.4) | n/a | — |

### 1.2 Precedence rules (the model as it naturally is — mostly already correct)

- **P1 — Rule order is the only in-table arbiter.** A tick has one winner; the hand
  goes to whichever equip/cast rule the player put higher. No score, no negotiation.
- **P2 — Cast borrows, equip owns.** The cast loadout is a *loan* against the
  off-hand with a ledger and a restore obligation; the equip gambit is a *transfer of
  ownership* with no ledger. This asymmetry is right: the player authored the weapon
  role; the spell is MFO's temporary need.
- **P3 — Release-before-fire keeps the loan honest.** The scheduler releases the
  gambit spell *before* firing the new winner (`Scheduler.cpp:268` runs before
  `:291`), so an equip rule that takes over never clobbers a still-ledgered spell.
  Natural and already correct — but fragile; see D6 for how a fall-through change
  breaks it if done naively.
- **P4 — One unpaid debt at a time.** Loadout refuses to displace more gear while it
  owes some (`Loadout.cpp:219-223`). Correct; keep.
- **P5 — A follower already holding their own spell is left alone** (`Grip::Caster`,
  `Loadout.cpp:184-188`). Their hands are their own.
- **P6 — Two-handed pays a debounce (6 s), off-hand swaps are free** (`Loadout.cpp:202-215`).

### 1.3 The compositions, natural vs surprising

| Composition | What happens today | Verdict |
|---|---|---|
| **Cast rule above, equip rule below** (mage-with-fallback) | While the cast rule's condition holds and magicka lasts: cast wins every window; equip never runs. When magicka runs out: `FailedSkill "insufficient magicka"` — which **consumes the tick and does not fall through** (`Scheduler.cpp:291,319-331`). The melee fallback below NEVER fires. Vanilla AI covers visibly, but the authored fallback is dead. | **SURPRISING — D1.** FFXII skips an unaffordable gambit and runs the next line; MFO stalls on it. |
| **Equip rule above, anything below** (e.g. "Foe beyond 600 → equip ranged" over "Foe any → attack") | First tick: equips, `Fired`, 1.5 s window. Every tick after: condition still true, `NoOp "already holding that category"` — consumes the tick. Everything below is shadowed for as long as any foe is beyond 600. The attack latch below never engages. | **SURPRISING — D2.** A satisfied maintenance rule should be transparent. |
| **Equip-melee AND equip-ranged in one list** (the range-switcher) | Only the higher one can ever win while both conditions are true (a mixed pack: one foe near, one far — `foe_within_range` and `foe_beyond_range` are both ANY-foe reads, `Evaluator.cpp:199-202`). Whichever is higher decides the weapon; no thrash *today* because the lower can never preempt. But the higher one's satisfied-NoOp shadows everything below (D2). | **Half natural.** Order resolves the tie legibly (good); the shadow is the bug. NOTE: any fall-through fix MUST NOT let the lower contradictory equip fire — that would manufacture thrash. See H2. |
| **A cast rule that keeps winning vs an equip rule below** | The spell sits in the off hand while the rule wants it (frequency limiter, `Scheduler.cpp:257-268`); for a 1H fighter this costs nothing (off-hand pivot). For a 2H fighter the weapon is stowed and only restored when the cast rule stops winning or on the debounce cycle. | **Natural** — this is §4.5b working as ruled. |
| **Attack rule that keeps winning** (`NoOp "already on that target"`, `Actuation.cpp:404`) | Shadows everything below. | **NATURAL — keep.** This is FFXII exactly: Attack is an *activity*; lines below an always-true Attack never run, and the player fixes it by putting reactive rules above it. Do NOT make attack transparent. |
| **Torch vs shield vs spell in the left hand** | No combat contention: torch is logistics-only; MFO never equips shields; the spell loan displaces a shield and restores it on hit / combat end. | **Natural.** |
| **Equip-melee on a pure caster follower** | `EquipWeapon`'s idempotence only recognizes *weapons* (`Actuation.cpp:331-334`): if the follower's own AI re-equips its spells (casters do), the equip gambit re-fires every window — a visible tug-of-war between MFO and the follower's own AI. | **SURPRISING — D7.** Rare (requires authoring a melee rule on a caster) but ugly. |
| **Drink rule above with no potions** | `FailedSkill "no matching potion, or still on cooldown"` (`Actuation.cpp:480`) consumes the tick while HP is low → a heal rule *below* it never fires. A follower dies holding a heal they know. | **SURPRISING — the worst concrete instance of D1.** |
| **Flee vs drink** | While the retreat alias is held the table is skipped entirely (`Scheduler.cpp:215-239`) — a fleeing follower cannot drink. Authoring fix exists: put drink ABOVE flee; drink fires (window ~1.5 s), flee fires next tick. | **Acceptable** with guidance; optional carve-out in P3. |

---

## 2. The outcome taxonomy (the analytical tool everything else uses)

Every `Fire()` outcome falls into one of four semantic classes. The current engine
only distinguishes them for *logging*; the heuristics below make the scheduler
distinguish them for *flow*:

| Class | Meaning | Examples (file:line) | Natural tick semantics |
|---|---|---|---|
| **FIRED** | State changed | equip swap `Actuation.cpp:352`, new attack latch `:403`, cast dispatched `:319`, drink `:478` | Consume tick + suppression window (unchanged) |
| **OPAQUE HOLD** | Deliberately doing nothing *as the action* | `act.wait` `:371`; "waiting for their AI to cast it" `:141`; attack "already on that target" `:404`; "caster has not selected the spell yet" `:233` | Consume tick, no window (unchanged) — Wait is the authored suppress idiom (§3.3); the cast-grace hold IS the action; a latched attack is an ongoing activity |
| **TRANSPARENT (satisfied)** | The rule's goal already holds; nothing to do | equip "already holding that category" `:334` | **Fall through** (+ claim the hand, H2) |
| **TRANSPARENT (blocked)** | Cannot act this tick, and holding the tick helps nobody | insufficient magicka `:50`, reserve floor `:72`, "no melee/ranged weapon carried" `:350`, no-potion/cooldown `:480`, power-attack without melee `:457`, two-handed debounce / cast cooldown `:149`, "already owe gear" (`Loadout.cpp:221`) | **Fall through** |

The dividing line is **activity vs state**: actions whose meaning is an ongoing
activity (attack, wait, a cast being granted its grace window) legitimately occupy
the tick; actions whose meaning is a *target state* (equip) or that *provably cannot
run* (failures) should be windows, not walls. FFXII agrees on both halves: an
unaffordable gambit is skipped, and lines below an active Attack are not.

---

## 3. The flow matrix

Rule sets use shipped vocabulary only, written as the natural role templates (these
should become the seeded/library sets — P2). **Bold** = mismatch with the current
engine; the D-number is the defect.

### 3.1 Pure melee — "Vanguard"
`1: self_hp<25% → drink health` · `2: foe_attacking_player → attack` · `3: foe_any → attack`

| Situation | Intended | Current engine | Verdict |
|---|---|---|---|
| Foe in melee range | Attack it (peel to player's attacker first) | Correct — latch + hook re-assert | OK |
| Foe at distance | Attack (confidence chase cap governs the approach, `Evaluator.cpp:184-187`) | Correct | OK |
| Outnumbered | Keep attacking; confidence shrinks leash; auto-retreat if enabled | Correct (#22/#23) | OK |
| Ally hurt | Nothing (melee has no heal) — vanilla AI unchanged | Correct | OK |
| Self hurt | Drink; if out of potions, keep fighting | **Out of potions: rule 1 `FailedSkill` consumes every tick → rules 2–3 starve; follower stops receiving attack latches while below 25% HP** | **D1** |

### 3.2 Pure ranged — "Archer"
`1: self_hp<25% → drink health` · `2: foe_within_range 150 → equip melee` · `3: foe_any → equip ranged` · `4: foe_any → attack`

| Situation | Intended | Current engine | Verdict |
|---|---|---|---|
| Foe at distance | Bow out, attack far foe | **Rule 3 equips once, then its satisfied-NoOp consumes every tick → rule 4 never latches a target** (vanilla AI shoots, but authored targeting — peel/lowest-HP variants — is dead) | **D2** |
| Foe closes to melee | Draw steel, fight | Rule 2 fires (higher, preempts inside window too) — then **its satisfied-NoOp shadows 3–4** | **D2**; also: rule 2 true while ANY foe is within 150 even if the archer's actual target is far — ANY-foe semantics, **D5** |
| Foe dies, next foe far | Swap back to bow | Rule 2 goes false, rule 3 equips ranged — correct transition (order melee-above-ranged is the safe authoring; the reverse order sticks the bow) | OK if authored this way; **needs to be the seeded order** (H5) |
| Self hurt, no potions | Fall through to fighting | **D1 shadow as in 3.1** | **D1** |
| Outnumbered | Melee rule naturally wins as foes close | OK | OK |

### 3.3 Hybrid warrior (bow + melee, the Erik case)
`1: self_hp<30% → drink` · `2: foe_within_range 200 → equip melee` · `3: foe_beyond_range 600 → equip ranged` · `4: foe_attacking_me_melee → attack` · `5: foe_any → attack`

| Situation | Intended | Current engine | Verdict |
|---|---|---|---|
| Mixed pack (one near, one far) | Melee wins (rule 2 higher) and holds; do NOT thrash | Correct choice today — but **after any naive fall-through fix, rule 2 satisfied → falls to rule 3, true (far foe), unsatisfied → equips bow → next tick rule 2 re-equips melee → thrash at window cadence.** | **D4 — must ship H2 with H1** |
| All far | Bow out, attack | **Satisfied rule 3 shadows 4–5** | **D2** |
| All near | Melee out, peel own attacker | **Satisfied rule 2 shadows 4–5** | **D2** |
| Weapon looted mid-fight | Pack restocked only; hand changed only by rules 2/3 | Correct (7d4266b) | OK |

### 3.4 Mage — "Elementalist"
`1: self_hp<40% → cast heal self` · `2: ally_hp<40% → cast heal other` · `3: foe_weak_fire → cast firebolt` · `4: foe_any → cast lightning`

| Situation | Intended | Current engine | Verdict |
|---|---|---|---|
| Foe at distance / in range | Nuke; own AI casts what MFO equips | Correct — consent + grace machinery | OK |
| Ally hurt | Heal ally (rule 2 preempts rule 3-4 positionally even inside windows) | Correct — positional suppression is exactly right here | OK |
| Self hurt | Heal self first | Correct | OK |
| **Magicka exhausted** | Skip unaffordable casts; do nothing authored (vanilla AI covers) — *legible* | Rule 1 (or first true cast) `FailedSkill` consumes every tick. Functionally similar (nothing happens) but rules below that COULD run (a cheaper spell lower in the list! `CalculateMagickaCost` is per-spell) never get tried. "Expensive nuke → cheap Sparks fallback" is inexpressible. | **D1** |
| Outnumbered | `foe_count_at_least → cast AoE/ward` above the nukes | Works when authored | OK |

### 3.5 Spellsword
`1: self_hp<40% → cast heal` · `2: foe_any → cast firebolt` (with `fMagickaReserve` ≈ 0.25) · `3: foe_any → equip melee` · `4: foe_any → attack`

| Situation | Intended | Current engine | Verdict |
|---|---|---|---|
| Full magicka | Open with spells (off-hand pivot keeps the sword) | Correct — the 1H off-hand equip is free | OK |
| **Reserve floor reached** | **Stop casting, fall to steel: this is THE spellsword flow** — the reserve is the only conjunction operator the vocabulary has ("cast only while MP above X" without an AND) | **Rule 2 `FailedSkill "magicka reserve"` consumes every tick; rules 3–4 never run.** The single most damning composition: the archetype is unbuildable today. (Workaround: author `self_mp_pct_above → cast` instead — works, but the reserve knob + failure path is the documented mechanism and it's dead.) | **D1** |
| Two-handed spellsword | Stow to cast, debounced 6 s | During `Debounced` the cast rule NoOps and shadows 3–4 — **the greatsword sits stowed-adjacent while nothing below runs** | **D10 (D1-family)** |
| Foe closes while casting | Rules are ordered; melee only reachable when cast stops winning | With D1 fixed, natural | — |

### 3.6 Cross-cutting situations

| Situation | Intended | Current | Verdict |
|---|---|---|---|
| Brawl / non-hostile "foe" | Hold fire entirely | Correct (#34 gate, `Evaluator.cpp:152-173`) | OK |
| Player HP low → cast at player | Heals the player | Correct (RC#6, `Evaluator.cpp:389-391`) | OK |
| Fleeing (act.flee / auto-retreat) | Disengage; optionally drink on the way | Table skipped during retreat hold; **drink-above-flee authoring works** (drink fires, flee next tick) | OK w/ guidance; P3 option |
| Foe blocking → power attack, holding a bow | Skip to lower rules | **`FailedSkill "no melee weapon drawn"` consumes the tick forever while the foe blocks** | **D1** |

---

## 4. The defect list (ranked)

| # | Defect | Rule/opcode involved | Anchor | Severity |
|---|---|---|---|---|
| **D1** | **Combat table has no fall-through: `FailedSkill`/`FailedOther` consume the tick**, starving every rule below for as long as the condition holds. Kills: out-of-potions → heal fallback; out-of-magicka → melee fallback (the spellsword archetype); reserve-floor → steel; power-attack-without-melee → attack. `Actuation.h:15` already documents the intended semantics ("→ fall through"); it was never implemented for combat. | any failing action; worst on `act.drink_*`, `act.cast_*`, `act.power_attack` | `Scheduler.cpp:255,291`; contrast `Logistics.cpp:2460-2507` | **Critical** |
| **D2** | **Satisfied equip NoOp shadows everything below** while its condition stays true — authored attack/targeting rules below an equip rule go dead for whole fights. | `act.equip_melee` / `act.equip_ranged` | `Actuation.cpp:334`, `Scheduler.cpp:334-354` | **Critical** |
| **D4** | Naive fall-through would create **equip thrash between contradictory equip rules** (melee↔ranged at window cadence in mixed packs). Not live today — a landmine inside the D1/D2 fix. | equip pair | §3.3 row 1 | **Critical (design constraint)** |
| **D6** | `wantsCast` / spell-release is computed from the **final** winning choice only (`Scheduler.cpp:265-268`). Under fall-through, a cast rule that is skipped (debounce, grace…) while a lower rule wins would get its spell yanked mid-grace. Landmine, not live. | `act.cast_*` | `Scheduler.cpp:265-268` | **High (design constraint)** |
| **D5** | `foe_within_range` / `foe_beyond_range` are ANY-foe reads: the equip pair keys off the *wrong foe* in mixed packs (bow out with an orc in your face if authored ranged-first; melee-first is safe but only by convention). | `cond.foe_within/beyond_range` | `Evaluator.cpp:199-202` | Medium |
| **D10** | Two-handed debounce / cast cooldown (`Ready::Debounced` → NoOp) shadows lower rules for up to 6 s / 4 s per cycle. D1-family; fixed by the same taxonomy. | `act.cast_*` on 2H wielders | `Actuation.cpp:145-149`, `Loadout.cpp:197-215` | Medium |
| **D7** | Equip gambit vs the follower's own caster AI: idempotence only recognizes weapons, so an authored melee rule on a spell-wielding follower re-fires every window against the AI re-equipping its spells — visible tug-of-war, no back-off, no log escalation. | `act.equip_*` | `Actuation.cpp:331-334` | Medium |
| **D8** | An always-winning cast rule starves equips below — *natural for a mage*, but the only tools to gate it (reserve, `self_mp_pct_above`) are respectively broken-by-D1 and undiscoverable. Resolves with D1 + seeded templates. | `act.cast_target` | §3.5 | Low (falls out of D1) |
| **D9** | No drinking while retreating (table skipped under the retreat holder). Authoring order works around it. | `act.flee` + `act.drink_*` | `Scheduler.cpp:235-239` | Low |
| **D3** | (Recorded as NOT-a-defect:) attack's "already on that target" NoOp shadows lower rules. This is FFXII's own semantics — an active Attack line blocks lower lines; reactive rules belong above it. Keep opaque. | `act.attack` | `Actuation.cpp:404` | — by design |

---

## 5. Proposed heuristics (each accepted / refined / rejected, with reasoning)

### H1 — ACCEPT: combat fall-through over transparent outcomes (fixes D1, D2, D10)

Mirror the logistics scan (`Logistics.cpp:2471-2507`) in the combat branch: loop
`Evaluate(f, state, Combat, start)`; if the outcome is **transparent** (per the §2
taxonomy), `start = ruleIndex + 1` and continue; stop on FIRED (suppression window as
today) or OPAQUE HOLD (wait, cast-grace, attack-latched). Still **at most one real
action per tick** — the loop only skips past rules that provably did nothing, exactly
the precedent logistics set.

*Legibility:* this is not MFO overriding the list — a skipped rule is a rule that
*could not act*, and each skip is loggable ("rule 1 skipped: no matching potion").
FFXII itself skips unaffordable lines. The one semantic change the player can
observe is strictly "my lower rules now run when the upper one is impossible" —
which is what every field report of "follower looks passive" was asking for.

*Boundary decisions (the part that needs marth's sign-off):*
- `act.wait` — **opaque** (the authored suppress idiom, §3.3). Unchanged.
- attack "already on that target" — **opaque** (FFXII activity semantics, D3). Unchanged.
- "waiting for their AI to cast it" (grace) — **opaque**: the hold IS the cast
  happening. Letting lower rules fire mid-grace risks the AI's cast being disturbed
  and re-opens the §0.6-style confound.
- "cast cooling down" / "two-handed debounce" / "already owe gear" — **transparent**:
  seconds-long waits during which the follower should fight.
- all `FailedSkill`/`FailedOther` — **transparent** (this is literally what
  `Actuation.h:15` promises).

### H2 — ACCEPT (this is the load-bearing refinement): a satisfied equip rule claims the hand and suppresses lower contradictory equips (fixes D4)

When the scan passes a *satisfied* equip rule (transparent), record "hand claimed:
melee" (or ranged) for the remainder of this tick's scan; any lower `act.equip_*` of
the **other** category is skipped *without firing*, regardless of its condition.

*Why this is legible, not an override:* it is first-match-wins applied to the hand as
a resource. The player ranked "within 200 → melee" above "beyond 600 → ranged"; when
both are true, the higher rule already states which weapon wins. Today the engine
enforces that by accident (the shadow); H2 enforces it on purpose while letting
non-equip rules below run. Without H2, H1 manufactures thrash — the two must ship in
the same commit.

*Scope:* per-tick, per-scan state only. Nothing persists; the evaluator stays
stateless between ticks (INVARIANTS #22).

### H3 — ACCEPT (implementation constraint, not a feature): scan-aware spell release (fixes D6)

Track `castSeen` during the scan: true if any cast rule's condition held this tick
(fired, held, debounced, or failed — the failure path already self-releases,
`Actuation.cpp:47,71`). Call `Loadout::ReleaseSpell` / `CasterConsent::Clear` only
when `!castSeen`. Preserves the frequency-limiter contract ("a gambit spell is held
only while its rule wants it", `Loadout.h:73-81`) under fall-through.

### H4 — REJECT: "attack auto-picks the right weapon for the target's range"

This would rebuild the Erik weapon-thrash bug that v1.0.11 just excised
(`7d4266b`: *"gambit-driven roles, never skill-forced"*). Weapon role is authored
intent — the equip gambits ARE the range-aware selection mechanism, with the
player's own thresholds. An attack that silently swaps weapons is MFO disagreeing
with the list (§4.3a), and its interaction with H2's hand claims would be
unresolvable (which wins: the claim or the auto-pick?). Attack stays a pure
targeting verb.

### H5 — ACCEPT: seeded role templates + safe-ordering doctrine (mitigates D5, D8)

Ship the §3 role sets as the Board's seed/library compositions, with the two ordering
rules baked in and documented on the board:
1. **Reactive above active**: drink/heal/flee above attack/cast-nuke (FFXII doctrine;
   also what makes D3's opacity harmless).
2. **Melee-equip above ranged-equip**: when both range conditions are true, closing
   steel wins (the safe tie-break under ANY-foe semantics, §3.3).

No engine change; pure authoring surface. This is also where `self_mp_pct_above →
cast` gets surfaced as the discoverable MP-gate idiom.

### H6 — REFINE (defer to vocabulary v2, #35): target-relative range conditions

D5's real fix is `cond.target_within_range` / `cond.target_beyond_range` — distance
to the follower's *current latched/engaged* target rather than ANY foe. Cheap read
(latched handle already exists in Targeting; fallback = nearest foe). Defer: H5's
ordering doctrine makes ANY-foe semantics safe enough, and new opcodes are a frozen
serialization commitment (#10) that shouldn't ride along with a scheduler change.

### H7 — REFINE: equip-vs-own-AI back-off with a legible reason (fixes D7)

If an equip gambit's chosen weapon is displaced by the follower's own AI within the
suppression window N times in one combat (N = 2), stop re-firing and report
`FailedSkill "their own AI keeps re-equipping a spell"` (transparent → lower rules
run). This is not overriding the list — it is *reporting* that the engine refuses
the instruction, the same shape as "insufficient magicka". Cheap: one FormID→(count,
lastDisplaced) map, cleared on combat end. Low priority; the trigger requires
unusual authoring.

### H8 — REJECT (for now): running drink actions during retreat hold

The retreat comment (`Scheduler.cpp:233-235`) is right: a retreating follower is
disengaging, not gambitting, and any evaluation during the hold re-opens the
alias-contention question (§0.24/§0.25: alias claim drives arbitration). The
authoring pattern (drink above flee — drink fires first, flee the next tick) covers
the need. Revisit only if soak logs show followers dying *during* retreats with
potions in the pack.

---

## 6. Prioritized implementation plan

### Phase 1 — the scheduler scan (high value, low risk, one reviewable commit)

All in existing functions; no new opcodes, no serialization changes, CI-verifiable.

| Step | Change | File / function |
|---|---|---|
| 1.1 | Add outcome transparency: either a `bool transparent` on `Outcome` or new `Result::NoOpSatisfied` (equip already-holding) — tag per the §2 / H1 boundary table. `Fire()` call sites: equip `Actuation.cpp:334,350`, cast fail `:50,:72`, Debounced `:149`, drink fail `:480`, power-attack `:457,:464` | `native/Actuation.h` (Outcome), `native/Actuation.cpp` (`Fire`, `CastOn`, `EquipWeapon`) |
| 1.2 | Combat fall-through loop with H2 hand-claim set and positional-suppression preserved: during a window, stop the scan (do nothing) when the next candidate index ≥ `firedRule`; never scan past it | `native/Scheduler.cpp` `Tick()` (the block at `:253-305`), modeled on `Logistics.cpp:2471-2507`; `Eval::Evaluate` already takes `a_startIndex` (`Evaluator.h:41`) — **zero evaluator changes** |
| 1.3 | H3 scan-aware `castSeen` release | same block, replacing `Scheduler.cpp:265-268` |
| 1.4 | Transition-only skip logging: one `[eval]` line naming the skip chain (`"rules 0(no potion),2(satisfied) skipped -> rule 3 fired"`) so the board/log stays legible (#22j dedup keyed on the chain) | `native/Scheduler.cpp` |
| 1.5 | Fix the stale comment claiming combat keeps `startIndex = 0` by design | `native/Evaluator.h:36-40`, DESIGN §4.3 cross-ref |

Risk notes: per-tick loop is bounded by list size (≤ slot count) with each pass a
pure read + at most one `Fire`; the only mutation-per-tick invariant (§4.3) is
preserved because transparent outcomes are by definition non-mutating.

### Phase 2 — authoring surface (medium)

| Step | Change | File |
|---|---|---|
| 2.1 | H5 seeded role templates (Vanguard/Archer/Hybrid/Elementalist/Spellsword) with the two ordering rules; surface `self_mp_pct_above` idiom | `native/Board.cpp` seeds / GAMBIT_LIBRARY.md |
| 2.2 | H7 equip back-off (map + reason string) | `native/Actuation.cpp` `EquipWeapon` |
| 2.3 | Board affordance: show a skipped rule's reason inline (the `lastFailReason` plumbing already exists, `Scheduler.cpp:297-304`) | `native/Board.cpp` |

### Phase 3 — optional polish / vocabulary v2 (defer, each its own release)

- H6 `cond.target_within_range` / `_beyond_range` (frozen opcodes — bundle with #35).
- H8 revisit drink-during-retreat only on soak evidence.
- Suppression window sized per action class (a cast's window ≈ cast time, an equip's
  ≈ 0 — DESIGN §4.4 already gestures at this; today one 1.5 s knob serves all).

### Explicitly not doing

- Attack auto-weapon / range-aware auto-selection (H4 — rejected, would resurrect
  the Erik thrash and violate §4.3a).
- Making `act.attack`'s latched-NoOp or `act.wait` transparent (D3 — FFXII semantics
  are the contract).
- Any reordering, dedup, or cross-follower coordination of the player's list (§4.3a).

---

## 7. Open questions for marth — RESOLVED (2026-08-03)

1. **H1 boundary sign-off** — DECIDED: cast-grace stays **opaque**;
   cooldown/debounce become **transparent**. The follower FIGHTS during a cast
   cooldown or 2H debounce instead of standing quiet (accepted visible change).
2. **H2 claim scope** — DECIDED: a satisfied equip claims **only against a lower
   contradictory EQUIP rule**, NOT against a lower cast rule's off-hand borrow.
   Casting still fires below an equip rule (spellsword lists work).
3. **Skip legibility** — DECIDED: **one skip-chain `[eval]` log line** in Phase 1;
   NO per-rule board "skipped: reason" state for now (that stays Phase 2 / optional).
