# MFO — Gambit Library (proposal for approval)

A tiered catalogue of ~50 follower gambits, ascending in usefulness and
complexity. **Purpose: go down each row and mark it Approve / Deny.** Nothing
here is built; this proposes *what to build and in what order*.

Feasibility is the real cost — most gambits are combinations of a small number
of shared new opcodes. The new conditions/actions are summarised once at the
end (§ Vocabulary additions); a gambit's feasibility tag points at them.

### How to read the feasibility tag

| Tag | Meaning | Cost |
|---|---|---|
| **[EXISTING]** | Pure combo of the shipped vocabulary (Vocabulary.h). No code. | none |
| **[NEW-COND]** | Needs one new *condition* (a state read). Cheap unless noted. | small, shared |
| **[NEW-ACT]** | Needs one new *action*. Tier A (engine call, no hook) unless flagged. | medium, shared |
| **[HARD]** | Needs an unproven mechanism or a **new vtable hook** (EXPENSIVE, one-per-release per INVARIANTS #45). | large / risky |

### Shipped baseline (the BASIC tier, already unlocked — for reference)

Conditions: `always`, self HP/MP/SP % below, player HP % below, foe selectors
(nearest / lowest-HP / lowest-HP-under-X%). Actions: `wait`, cast self, cast
target, attack (UpdateCombat hook), drink H/S/M potion (fires in **both**
tables now), loot arrows / potions / equipment. The two live hooks are
`UpdateCombat` (attack targeting) and `CheckStartCast` (caster consent); the
M9 alias-package route delivers **animated** casts at a chosen target and is
field-proven (ENGINE_NOTES §0.21–0.22). Note per §0.30 the tick runs on a
**job worker thread**, so every new condition must be a pure, non-allocating,
lock-respecting read (INVARIANTS #23).

---

## Tier 0 — BASIC gaps (obvious holes in the shipped set)

These are cheap and belong in the default/low-rank set. Most are [EXISTING].

| # | Name | Condition → Action | Feasibility | Why it earns the slot | ✅/❌ |
|---|---|---|---|---|---|
| 1 | Heal Thyself | Self HP < 40% → Cast Heal (self) | [EXISTING] | The flagship; already the seed. Survival floor. | ☐ |
| 2 | Finish Him | Foe HP < 25% → Attack (that foe) | [EXISTING] | Execute the low-HP target instead of spreading damage. | ☐ |
| 3 | Focus Fire | Foe lowest HP → Attack | [EXISTING] | Party converges one target; kills come faster. | ☐ |
| 4 | Open Fire | Foe nearest → Cast \<offense\> at foe | [EXISTING] | Basic caster loop; animated via M9 when the AI agrees. | ☐ |
| 5 | Emergency Draught | Self HP < 25% → Drink health potion | [EXISTING] | In-combat drink now actuates in the combat table. | ☐ |
| 6 | Catch My Breath | Self SP < 20% → Drink stamina potion | [EXISTING] | Keeps power-attackers/sprinters functional. | ☐ |
| 7 | Mana Sip | Self MP < 25% → Drink magicka potion | [EXISTING] | Lets a caster sustain a cast rule instead of going dry. | ☐ |
| 8 | Hold the Line | Always → Wait | [EXISTING] | The FFXII no-op; suppresses everything below it. A slot-1 "stand down" toggle without editing the list. | ☐ |

---

## Tier 1 — INTERMEDIATE (Rank II–III; wider vocabulary, still Tier A)

Cheap self/foe reads and no-hook equip actions. This is where the mod stops
being "heal + attack" and starts being tactical.

| # | Name | Condition → Action | Feasibility | Why it earns the slot | ✅/❌ |
|---|---|---|---|---|---|
| 9 | Mercy for the Master | Player HP < 40% → Cast Heal Other at player | [EXISTING] | Already possible; belongs here as the canonical "pocket healer" line. | ☐ |
| 10 | Tend the Wounded | **Ally** HP < 40% → Cast Heal Other at that ally | **[NEW-COND]** ally selector | The single biggest gap: followers can only heal player/self today. Heal *each other*. | ☐ |
| 11 | Conserve the Pool | Self MP **above** 30% → Cast \<offense\> at nearest foe | **[NEW-COND]** self MP-above gate | Stops a caster emptying its pool; leaves a reserve for the heal rule below. | ☐ |
| 12 | Draw Steel | Player weapon drawn → Draw weapon / ready | **[NEW-COND]** player-weapon-drawn | Follower readies when you do, not only once shot at. | ☐ |
| 13 | Nock an Arrow | Foe **beyond** 900u → Equip bow | **[NEW-COND]** foe-range + **[NEW-ACT]** equip_ranged | The classic "bow at range." Pure Tier A `ActorEquipManager`. | ☐ |
| 14 | Draw the Blade | Foe **within** 400u → Equip melee weapon | [NEW-COND] foe-range + **[NEW-ACT]** equip_melee | The other half of #13; melee when the gap closes. | ☐ |
| 15 | Raise the Shield | Foe within 400u & shield in pack → Equip shield | [NEW-COND] foe-range + **[NEW-ACT]** equip_shield | Sword-and-board on contact; mirrors §4.5b off-hand policy. | ☐ |
| 16 | Light the Way | Interior & night & no light → Equip torch | **[NEW-COND]** is_interior/is_night + self-torch-equipped + **[NEW-ACT]** equip_torch | Cosmetic-but-loved; a follower who lights up a dungeon reads as alive. | ☐ |
| 17 | Cull the Weak | Foe **highest HP** → Attack | **[NEW-COND]** foe_highest_hp | Opposite of Focus Fire: tie up the tank while the party clears adds. | ☐ |
| 18 | Mark the Champion | Foe **highest level** → Attack | **[NEW-COND]** foe_highest_level | Prioritise the dangerous one over trash. | ☐ |
| 19 | Guard the Master | Foe **attacking the player** → Attack that foe | **[NEW-COND]** foe_attacking_player | Peel duty — the follower behaviour players most wish existed. | ☐ |
| 20 | Watch My Back | Foe **attacking me** → Attack that foe | **[NEW-COND]** foe_attacking_me | Self-preservation; stop ignoring the thing hitting you. | ☐ |
| 21 | Brace for the Swarm | Foe count ≥ 3 in radius → Cast \<AoE/ward\> (self) | **[NEW-COND]** foe_count_ge | Density gate: react to being mobbed, not to one foe. | ☐ |
| 22 | Kite the Melee | Foe within 300u & I hold a bow → Equip melee | [NEW-COND] foe-range + self_ranged_equipped + [NEW-ACT] equip_melee | Don't get cornered plinking; swap when they're on you. | ☐ |
| 23 | Silver for the Dead | Foe **undead** → Cast \<fire/turn\> at foe | **[NEW-COND]** foe_is_undead | Kind-matched offense; huge in undead-heavy dungeons. | ☐ |
| 24 | Beast Season | Foe **animal** → Attack | **[NEW-COND]** foe_is_animal | Cheap discriminator; also lets "ignore animals" lists exist. | ☐ |
| 25 | Dragonsbane | Foe **dragon** → Cast \<best offense\> at foe | **[NEW-COND]** foe_is_dragon | Commit the party's biggest guns to the biggest threat. | ☐ |
| 26 | Steady the Aim | I hold a bow & foe beyond 1200u → Wait | [NEW-COND] self_ranged_equipped + foe-range | Deliberate no-op: don't waste arrows at extreme range; let a higher melee rule win when they close. | ☐ |
| 27 | Stand Down | Player sneaking → Sheathe / Wait | **[NEW-COND]** player_sneaking + **[NEW-ACT]** sheathe (or Wait) | Follower stops charging in and blowing your stealth approach. | ☐ |
| 28 | Shadow the Sneak | Player sneaking → Enter sneak | [NEW-COND] player_sneaking + **[NEW-ACT]** enter_sneak (Tier B) | Follower crouches with you instead of standing lit in the doorway. | ☐ |
| 29 | Regroup | Distance to player > 2500u → Move to player | **[NEW-ACT]** regroup (Travel pkg, Tier B) + **[NEW-COND]** dist_to_player | Straggler recall; uses the proven M9 package machinery, new PACK only. | ☐ |
| 30 | Second Wind | Self HP above 70% & foe nearest → Attack | [NEW-COND] self HP-above gate | Only press the attack when healthy; below it, lower rules (heal/retreat) win. | ☐ |

---

## Tier 2 — ADVANCED (Rank IV–V; moderate reads, Tier B packages, a few hard)

Active-effect and detection walks, ally coordination, package-driven
positioning. Higher slot cost; several share the M9 package route (proven, but
each new PACK/behaviour is its own release per #45).

| # | Name | Condition → Action | Feasibility | Why it earns the slot | ✅/❌ |
|---|---|---|---|---|---|
| 31 | Shake It Off | Self **staggered/recoiling** → Wait | **[NEW-COND]** anim-var read | An action issued mid-stagger is silently eaten (§4.5a #5); this makes the follower *not* waste the tick and keeps the board legible. | ☐ |
| 32 | Burn It Away | Self **on fire** → Drink health potion | **[NEW-COND]** self-debuff (archetype walk) | React to the *reason* you're losing HP, not just the HP. | ☐ |
| 33 | Break the Hold | Self **paralyzed/frozen** → Wait (suppress) | [NEW-COND] self-debuff | Legibility: shows "can't act" rather than a rule mysteriously not firing. | ☐ |
| 34 | Cleanse | Self has curable debuff → Cast \<cure/restore\> (self) | [NEW-COND] self-debuff | Self-cleanse where the follower knows the spell. Kind-matched to the debuff. | ☐ |
| 35 | Purge the Rot | Self **poisoned/diseased** → Drink health potion | [NEW-COND] self-debuff | Cheap survival vs. DoTs without needing a cure spell. | ☐ |
| 36 | No Double Dip | Foe already has \<my DoT\> → **switch** (foe next-lowest) | **[NEW-COND]** target_has_effect | Stop re-applying the same DoT; spread it. Real caster tactics. | ☐ |
| 37 | Triage | **Ally lowest HP** < 30% → Cast Heal Other at ally | [NEW-COND] ally selector | Advanced form of #10: always top the *most* wounded ally. | ☐ |
| 38 | Battery | **Ally** MP < 25% & I know it → Cast \<restore-MP\> at ally | **[NEW-COND]** ally-MP selector | Niche support-mage line; low priority but flavourful at Rank V. | ☐ |
| 39 | Sic 'Em | Foe attacking the player → **Focus** that foe (standing order) | [EXISTING] attack + [NEW-COND] foe_attacking_player | Peel as a *committed* target (§4.7 latch), not a one-tick swing. | ☐ |
| 40 | Bodyguard Hold | Player HP < 30% → **Hold position** by player | **[NEW-ACT]** hold_position (HoldPosition pkg, Tier B) | Stop chasing kills and body-block for a dying player. | ☐ |
| 41 | Fighting Retreat | Self HP < 20% & foe within 400u → **Flee** | **[NEW-ACT]** flee (Travel-away pkg, Tier B — harder) | Break off instead of dying in melee; hardest of the movement verbs. | ☐ |
| 42 | Unleash | Self MP above 60% & foe highest level → Cast \<biggest offense\> at foe | [NEW-COND] self MP-above + foe_highest_level | Dump the expensive nuke only when affordable and worth it. | ☐ |
| 43 | Weak to Fire | Foe **undead/frost-kin** → Cast \<fire\> at foe | [NEW-COND] foe_is_undead (+ kind) | Element matching; the payoff of the Kind selectors as a family. | ☐ |
| 44 | Interrupt the Caster | Foe **is casting/is a caster** & nearest → Attack | **[NEW-COND]** foe_is_caster / foe_casting (anim/equip) | Rush enemy mages first — high-value target discrimination. | ☐ |
| 45 | Duck the Power Attack | Foe **power-attacking** me → Equip shield / Wait | **[NEW-COND]** foe power-attack anim | Reactive defense; anim-var read, cheap-ish but per-foe. | ☐ |
| 46 | Stay Unseen | I am **detected** → Wait / retreat | **[NEW-COND]** self_detected (PO3 detection) | Stealth-build support; act only while still hidden. | ☐ |
| 47 | Loose from the Dark | I am **not** detected & foe nearest → Attack (sneak strike) | [NEW-COND] self_detected (negated) | Follower opens from stealth for the sneak-attack multiplier. | ☐ |
| 48 | Kite | Foe within 300u & I hold a bow → **Keep distance** from foe | **[NEW-ACT]** move/keep-distance (Travel pkg, Tier B) | Real kiting behaviour; the standing complaint about archer followers. | ☐ |
| 49 | War Cry | Foe count ≥ 4 & I have a shout → **Shout** | **[NEW-ACT]** shout (EquipShout+AI or pkg, Tier B) | Uses shouts a levelling mod granted (§5.4); dramatic AoE control. | ☐ |
| 50 | Raise Guard | Foe within 250u & I hold a shield → **Block** | **[HARD]** no block verb; needs a new combat/anim hook (#45, expensive) | Genuinely wanted, genuinely expensive — flagged so it isn't assumed cheap. | ☐ |
| 51 | Dodge | Foe power-attacking me → **Dodge** | **[HARD]** depends on TDM/Precision or anim events; unproven | Only viable if piggybacking an installed dodge mod; note the risk. | ☐ |
| 52 | Look Alive | Player in combat → **Look at** player's target | **[NEW-ACT]** look_at (Tier B, cosmetic) | Cheap perceived-intelligence win; low mechanical value. | ☐ |

---

## Vocabulary additions needed (the real implementation cost, deduplicated)

Build these once; the gambits above are combinations of them. Ordered by
value-per-cost.

### New CONDITIONS

**Cheap** — single actor-value or flag read on a known actor; job-thread-safe,
no allocation (INVARIANTS #23). Each reuses `Vocab::Pct` or a direct getter.

| Opcode (proposed) | Read | Used by |
|---|---|---|
| `cond.self_hp/mp/sp_pct_above` | `Pct(self, av)` — mirror of the `_below` trio | 11, 30, 42 |
| `cond.player_mp/sp_pct_below` | `Pct(player, av)` | support lines |
| `cond.player_weapon_drawn` / `player_sneaking` / `player_in_combat` / `player_mounted` | `Actor` flags on player | 12, 27, 28, 52 |
| `cond.self_weapon_drawn` / `self_sneaking` | `AsActorState()` | stance rules |
| `cond.self_ranged/melee/shield/spell/torch_equipped` | `GetEquippedObject` (Logistics::EquippedRanged already does this) | 16, 22, 26, 48 |
| `cond.dist_to_player_within/beyond` (param N) | `pos.GetDistance(playerPos)` | 29 |
| `cond.is_interior` / `cond.is_night` | `GetParentCell()->IsInteriorCell()`; Calendar GameHour | 16 |

**Foe-selector / combat-group scan** — extend `PickFoe` (Evaluator.cpp). One
locked walk of `combatGroup->targets`, no allocation, same shape as today.
These *also choose the target*, like the existing foe selectors.

| Opcode (proposed) | Read in the walk | Used by |
|---|---|---|
| `cond.foe_hp_pct_above` | `HealthPct(foe) >= p` | fresh-target lines |
| `cond.foe_highest_hp` / `foe_highest_level` / `foe_furthest` | score by HP / `GetLevel()` / distance | 17, 18 |
| `cond.foe_within_range` / `foe_beyond_range` (param N) | distance gate, already computed | 13, 14, 15, 22, 26, 48 |
| `cond.foe_attacking_player` / `foe_attacking_me` | foe's `currentCombatTarget` == player / self | 19, 20, 39 |
| `cond.foe_count_ge` (gate, param N) | count survivors in the walk (no target) | 21, 49 |
| `cond.foe_is_undead/daedra/dwarven/dragon/animal` | race/actor keyword check per foe | 23, 24, 25, 43 |
| `cond.foe_is_caster` | foe has a spell equipped (`GetEquippedObject`) | 44 |

**Moderate** — one active-effect / anim-var / detection walk; keep low in the
list (§4.2 rule 5 = order is the cost). Per §4.5a #5 the stagger read is also
an actuation precondition.

| Opcode (proposed) | Read | Used by |
|---|---|---|
| `cond.self_staggered_or_recoiling` | anim-var bool `IsStaggering`/`IsRecoiling` | 31 |
| `cond.self_has_debuff` (param = archetype/kind: fire/frost/shock/poison/disease/paralysis) | active-effect archetype walk on self | 32–35 |
| `cond.target_has_effect` (param archetype) | active-effect walk on chosen foe | 36 |
| `cond.self_detected` | `PO3.IsDetectedByAnyone` / `CanActorBeDetected` | 46, 47 |
| `cond.foe_casting` / `foe_power_attacking` | anim-var on chosen foe | 44, 45 |

**Ally selector** (moderate) — the highest-value single addition. Needs a
combat-ally scan (`PO3.GetCombatAllies`, or walk player teammates in range).
Yields both the truth value *and* the target, like a foe selector.

| Opcode (proposed) | Read | Used by |
|---|---|---|
| `cond.ally_hp_pct_below` (lowest-HP ally under X%) | ally scan + `HealthPct` | 10, 37 |
| `cond.ally_mp_pct_below` | ally scan + `MagickaPct` | 38 |

### New ACTIONS

**Tier A — no hook, `ActorEquipManager` / direct engine call. Build first.**
Follows the shipped Loadout/Logistics equip discipline (snapshot-then-act,
debounce, idempotent). Off-hand policy per §4.5b for the spell/shield cases.

| Opcode (proposed) | Mechanism | Used by |
|---|---|---|
| `act.equip_ranged` / `equip_melee` / `equip_shield` / `equip_torch` | `ActorEquipManager::EquipObject` best-in-category from own inventory (mirror `LootEquipment`'s category logic) | 13–16, 22 |
| `act.sheathe_weapon` | actor sheath call (low risk) | 27 |
| `act.dispel_debuff` | `ActiveEffect::Dispel(true)`, collect-then-dispel — **flag: scope carefully to MFO-neutral negative effects, never strip arbitrary effects** | 34 (alt) |

**Tier B — the M9 alias-package route (proven, ENGINE_NOTES §0.21). NOT a new
hook**, but each new behaviour is a new PACK instance riding a vanilla template
and is its own release per #45. Positioning packages must release on combat
exit / cell detach (§4.7.5).

| Opcode (proposed) | Vanilla template | Used by |
|---|---|---|
| `act.hold_position` | `HoldPosition` (000503D0) | 40 |
| `act.regroup` / `act.keep_distance` (move to / away from a ref) | `Travel` (00016FAA) | 29, 48 |
| `act.flee` | `Travel` away (harder — pick a safe node) | 41 |
| `act.enter_sneak` | `StartSneaking` / stance package | 28 |
| `act.shout` | `EquipShout` + AI, or a shout package | 49 |
| `act.look_at` | `SetLookAt` (cosmetic) | 52 |

**[HARD] — needs an UNPROVEN mechanism or a NEW vtable hook (EXPENSIVE,
one-per-release, #45). Do not assume cheap.**

| Opcode (proposed) | Risk | Used by |
|---|---|---|
| `act.block` | No engine "raise block" verb; needs a new combat/anim hook | 50 |
| `act.dodge` | Depends on an installed dodge framework (TDM/Precision) or anim-event injection; unproven in this architecture | 51 |
| `act.power_attack` | No verb; would drive the animation graph — the same class MFO already refuted for casting | (future) |

---

## Recommended build order — the top 5

Highest usefulness-per-cost, all avoid a new hook:

1. **Ally-HP selector + Heal Other (gambits 10, 37).** `[NEW-COND]` combat-ally
   scan. Closes the biggest gap in the whole mod — today a follower cannot heal
   another follower, only the player or itself. One condition unlocks true party
   support.
2. **Equip-swap actions (13–16, 22).** `[NEW-ACT]` Tier A, no hook — reuses the
   proven `ActorEquipManager` path already in Loadout/Logistics. Unlocks the
   entire "bow at range, blade up close, shield on contact, torch in the dark"
   family from one action group.
3. **Cheap foe-range + foe-count + foe-highest-{hp,level} selectors
   (13,14,17,18,21).** `[NEW-COND]`, all inside the existing `PickFoe` walk —
   near-zero marginal cost, and they make range- and threat-aware tactics
   possible at all.
4. **`foe_attacking_player` / `foe_attacking_me` (19, 20, 39).** `[NEW-COND]`,
   one field read per foe in the same walk. Delivers "peel for the player" — the
   single most-requested follower behaviour — as a committed standing order.
5. **Self/player state gates: MP-above, player-sneaking, self-equipped-kind
   (11, 27, 30).** `[NEW-COND]`, cheapest reads there are. They turn flat rules
   into conditional ones (conserve mana, don't blow stealth, only press when
   healthy) and make good lists authorable.

Everything above is Tier A or a cheap read — **no new hook, no unproven
mechanism.** The Tier-B package actions (hold/regroup/flee/shout, 29,40,41,48,49)
are the natural *next* wave once these land, each as its own #45 release. The
`[HARD]` block (50–52) is flagged precisely so it is not mistaken for cheap.
