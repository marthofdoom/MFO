# MFO — Gambit vocabulary to build

## Principle (marth, 2026-07-28)

**A gambit is nothing but `[what determines the target/trigger] → [what the
action is]`.** There are NO named gambits, no clever titles, no bespoke
special-cased rules. Everything the player can express is a composition of the
primitive **conditions** and **actions** below. If a wanted behaviour can't be
written as condition → action, the answer is a new *primitive*, never a named
one-off. The board shows exactly the condition and the action — that is the
whole identity of a rule.

This doc is therefore the **vocabulary to build**, derived from the rows marth
approved. The "gambits" are just illustrative compositions of it.

---

## Shipped vocabulary (the BASIC tier, already unlocked)

**Conditions:** `always`; self HP/MP/SP % below; player HP % below; foe
selectors `nearest` / `lowest-HP` / `HP-%-below`; potion-count & out-of-arrows
(logistics). **Actions:** `wait`; cast self; cast target; attack (UpdateCombat
hook); drink health/stamina/magicka potion (now fires in the combat table too);
loot arrows / potions / equipment.

Two live hooks only: `UpdateCombat` (attack targeting) and `CheckStartCast`
(caster consent). The M9 alias-package route gives animated casts/positioning
and is proven (ENGINE_NOTES §0.21). Per §0.30 the tick is on a job worker
thread, so every new condition must be a pure, non-allocating, lock-respecting
read (INVARIANTS #23).

---

## Approved compositions (condition → action)

Only marth-approved rows. Each is described *only* by its condition and action;
the tag points at the primitive(s) it needs from the build list below.

### Expressible with shipped vocab (no code)
- Self HP % below → cast self (a heal the follower knows)
- Foe HP % below → attack
- Foe lowest HP → attack
- Foe nearest → cast at foe
- Self HP/SP/MP % below → drink health/stamina/magicka potion
- Player HP % below → cast at player
- Always → wait

### Approved, needs new vocabulary
- Ally HP % below → cast at that ally  *(needs `ally_hp_pct_below` selector)*
- Self MP % above → cast at nearest foe  *(needs `self_mp_pct_above`)*
- Foe beyond range → equip ranged weapon  *(needs `foe_beyond_range`, `act.equip_ranged`)*
- Foe within range → equip melee weapon  *(needs `foe_within_range`, `act.equip_melee`)*
- Interior & night & not already lit → equip torch  *(needs `is_interior`, `is_night`, `self_torch_equipped`, `act.equip_torch`)*
- Foe highest HP → attack  *(needs `foe_highest_hp`)*
- Foe attacking the player → attack  *(needs `foe_attacking_player`)*
- Foe attacking me → attack  *(needs `foe_attacking_me`)*
- Foe count ≥ N in radius → cast self (ward/AoE)  *(needs `foe_count_ge`)*
- Foe is undead → cast at foe  *(needs `foe_is_undead`)*
- Foe is dragon → cast at foe  *(needs `foe_is_dragon`)*
- **Foe weak to fire/frost/shock → cast matching element at foe**  *(needs `foe_weak_to_element`; marth: this is the INTERMEDIATE form of the kind-selectors — prefer it over "is undead → fire")*
- **Foe is ranged → attack**  *(needs `foe_ranged`; marth's reframe of "interrupt the caster" — the discriminator is ranged, not caster)*
- Foe already has my damage-over-time effect → attack next foe  *(needs `target_has_effect` as a selector exclusion)*
- Self HP % below & foe within range → flee  *(needs `act.flee`, Tier B package)*
- Foe within range & I hold a bow → keep distance from foe  *(needs `act.keep_distance`, Tier B package)*
- Self has a disease → cast cure/restore (self)  *(needs `self_has_debuff`; marth: only if followers can contract diseases — verify first)*

### Denied / deferred (recorded so they aren't re-proposed)
- Equip shield, foe highest-level, foe is animal, kite-swap, steady-aim,
  stand-down/sneak, regroup, second-wind, stagger/paralysis suppress, on-fire,
  triage (redundant with ally-heal), ally-MP battery, focus-standing-order,
  bodyguard-hold (*too static — player and foes move; defer*), MP-nuke gate,
  power-attack duck, detection gates, war-cry shout, look-at, block, dodge.
- **On-fire / seek-water and similar** — deferred to a later **"invisible
  gambits"** pass: ambient rules that make followers read as lifelike, run
  without the player authoring them (marth).

### Action item pulled from the review
- **Cure poison / cure disease potions** must be recognised by the potion
  classification so a follower will drink/loot them like any restorative
  (marth, from denied #35). Touches `PotionRestores` / the drink-potion set —
  independent of the vocab additions.

---

## Vocabulary to build (the real work, deduplicated)

### New conditions

**Cheap** — one actor-value / flag read, job-thread-safe, no allocation (#23):
| Opcode | Read |
|---|---|
| `cond.self_mp_pct_above` (+ hp/sp mirror for symmetry) | `Pct(self, av)` |
| `cond.is_interior` | `GetParentCell()->IsInteriorCell()` |
| `cond.is_night` | Calendar game-hour |
| `cond.self_torch_equipped` | `GetEquippedObject` (or make `act.equip_torch` idempotent) |

**Foe selectors** — extend the existing `PickFoe` walk of `combatGroup->targets`
(these choose the target AND gate):
| Opcode | Read in the walk |
|---|---|
| `cond.foe_highest_hp` | max `HealthPct` |
| `cond.foe_within_range` / `cond.foe_beyond_range` (param N) | distance gate |
| `cond.foe_attacking_player` / `cond.foe_attacking_me` | foe `currentCombatTarget` == player / self |
| `cond.foe_count_ge` (gate, param N) | count survivors, no target |
| `cond.foe_is_undead` / `cond.foe_is_dragon` | race/keyword per foe |
| `cond.foe_ranged` | foe has bow/crossbow or spell equipped |
| `cond.foe_weak_to_element` (param fire/frost/shock) | foe `kResist<Element>` low, or weakness keyword |

**Moderate** — one active-effect / ally walk; keep low in the list (order is the cost):
| Opcode | Read |
|---|---|
| `cond.ally_hp_pct_below` (lowest ally under X%, yields target) | combat-ally scan + `HealthPct` |
| `cond.target_has_effect` (param archetype) | active-effect walk on the chosen foe |
| `cond.self_has_debuff` (param disease/poison) | active-effect archetype walk on self |

### New actions

**Tier A** — `ActorEquipManager`, no hook; mirror `LootEquipment` category logic, idempotent:
| Opcode | Mechanism |
|---|---|
| `act.equip_ranged` / `act.equip_melee` / `act.equip_torch` | equip best-in-category from own inventory |

**Tier B** — M9 alias-package route (proven, not a new hook; each is its own #45 release; releases on combat exit / cell detach):
| Opcode | Template |
|---|---|
| `act.keep_distance` (move away from a ref) | `Travel` |
| `act.flee` (Travel to a safe node — harder) | `Travel` |

---

## Build order

1. **Ally-HP selector** (`cond.ally_hp_pct_below`) → unlocks follower-heals-ally, the biggest gap.
2. **Tier-A equip actions** (`equip_ranged` / `equip_melee` / `equip_torch`) → the range/dark family, no hook.
3. **Cheap foe selectors in `PickFoe`** (`foe_highest_hp`, `foe_within/beyond_range`, `foe_count_ge`) → range/threat tactics at near-zero cost.
4. **`foe_attacking_player` / `foe_attacking_me`** → "peel for the player", one field read per foe.
5. **`self_mp_pct_above`, `is_interior`/`is_night`, kind selectors (`foe_is_undead`/`_dragon`/`_ranged`/`_weak_to_element`)** → conditional, elemental and utility lists.

Then the moderate reads (`target_has_effect`, `self_has_debuff`) and the Tier-B
movement actions (`keep_distance`, `flee`), each as its own release. Separately,
add cure-poison/disease to the potion classification.
