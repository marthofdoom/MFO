# Perk-effect reader: design note (ClickUp 1dfcuabd2pk, `feat/perk-effects`, 2026-11-16)

marth 2026-11-12: "Perks are just one more thing that needs a weight" ... "And what spells to use. Armor to pick, it
needs to read perks like potions and see how they affect the world."

Status: built, local build green, NOT merged, NOT field run. No co-save, ABI, address-id, hook-seat, INI or ESP change.
Every number below marked PLACEHOLDER is a hand value that a tuning run replaces. `tools/iaus_sim` is not edited
(section 9 says what its mirror needs).

## 1. The idea in one paragraph

A perk is never read by its name, editor id or tree position. MFO asks the ENGINE what the follower's perks do to a
concrete thing: this weapon's damage, this armor piece's rating, this spell's cost and magnitude. The engine function
every perk entry point runs through (`BGSEntryPoint::HandleEntryPoint`) is called with the follower as the perk owner
and a real item. The engine walks his perk entries of that entry point, runs each entry's own conditions on the owner,
the item and the target, applies each entry's function, and hands back the value. Value out over value in is the
multiplier. That is the potion reader's rule ("read the effect, not the name") applied to perks, and it holds for any
perk overhaul and any hand-authored perk in any load order.

## 2. What is read

### 2.1 Entry points the engine is asked (the numbers decisions read)

Each shape was decoded from an engine call site on all three executables (1.6.1170, 1.5.97, 1.7.104) with
`tools/entrypoint_census` (`ep_census.py`, `arity.py`, `dz.py`). The shape is the same on all three, so there is one
code path and no per-runtime branch. Tab i of a perk entry's conditions is argument i (ENGINE_NOTES 0.54).

| EP | Name | Shape (after the owner) | Seed MFO passes | Engine call site (1.6.1170 / 1.5.97 / 1.7.104) | Feeds |
|---|---|---|---|---|---|
| 35 | ModAttackDamage | weapon (TESObjectWEAP), target (Actor), &value | the weapon's base damage (min 1) | display read `0x72816E` / `0x6943A6` / `0x73AC9E` (target = the player singleton); real hit `0x7DCB86` / `0x744391` / `0x7F1856` | weapon pick, posture |
| 85 | ModArmorRating | armor (TESObjectARMO), &value | the piece's rating | `0x726BDD`, `0x728346` / `0x692DCD`, `0x694576` / `0x73970D`, `0x73AE76` (form type 0x1A tested and armor type 2 = clothing skipped before the call, so MFO asks about light and heavy pieces only; the result is added into the actor's total) | armor pick |
| 38 | ModSpellCost | spell, &value | 100 | `0x14C2C8` / `0x101B0B` / `0x151858` (`MagicItem::CalculateCost`) | spell choice |
| 29 | ModSpellMagnitude | spell, target, &value | the costliest effect's magnitude (100 when it has none) | `0x5ABE68` / `0x53DADF` / `0x5BA998` (already used by `iaus/Fills.cpp`) | spell choice |
| 75 | CanDualCastSpell | spell, &value | 0 (the engine's own seed; any other value out = yes) | `0x6C3F4A` / `0x63214D` / `0x6D6A0A` | recorded only |
| 39 | ModPercentBlocked | &value | 1.0 | `0x7DC153` / `0x74395D` / `0x7F0E03` | recorded only |

Notes that matter:

- **EP75 is player-only by engine code** (every call site passes the player singleton, ENGINE_NOTES 0.54). The engine
  never dual casts a follower because of it. MFO's own dual-cast decision (`Loadout.cpp OwnsDualCastPerk`) reads a
  vanilla perk form today. The table now holds the engine's per-school answer, so that decision can move onto it. That
  is its own change (REVIEW-BACKLOG MFO-B860 item 3). Nothing reads `dualCast` yet.
- **Bow zoom (EP20) is player-only** (a PlayerCharacter camera vfunc), so it is not asked. **Bow draw speed** is not
  an entry point at all: perks do it with an ability that raises an actor value, which is the census in 2.2.
- **Sneak multipliers (EP18)** are already read where they are used, against the real weapon and the real target
  (`StealthProbe.cpp`, `Facts::strikeMult`). A stand-in target would be wrong for them, so they are not in the table.
  The census counts the entries.
- **Clothing has no rating** for EP85 to multiply. What a robe-wearing mage's perks give him (mage armor perks) shows up
  in EP29 on his armor spells, evaluated against what he wears now.

### 2.2 The census (record data, for the log and for later kinds; no decision reads it)

`Census` walks the base's perk array (`TESNPC::perks`: a follower's perks live on the base) and each perk's
`perkEntries`:

- entry-point entries, counted per entry point id (all 92);
- ability entries: the ability spell is collected, and `AbilityValues` sums, per actor value, the magnitude of the
  LIVE effects on him whose source is one of those abilities and whose archetype modifies a value. Live means the
  engine has already run the effect's own conditions, so no condition is interpreted here either. This is where a
  perk that works through an actor value shows up (bow draw speed, weapon speed, flat armor, movement, the skill
  modifier values);
- quest entries, counted (they never do anything for a follower).

The skill modifier actor values (`*Modifier`, `*PowerModifier`) have no effect of their own in the engine. They act
through entry-point entries whose function reads the actor value (the hidden skill-boost perks, iaus-i8-damage-research
section "school power modifier"). So whatever of them reaches a follower is already inside the 2.1 answers, and the
census rows are not added on top. Adding them would count the same perk twice.

## 3. How conditions are handled

MFO interprets none. The engine evaluates every entry's conditions with the real owner, the real item and a real
target. A condition MFO could not classify is therefore never guessed and never ignored.

Two consequences, both deliberate:

- **Owner-state conditions read his state NOW.** A "wearing all heavy armor" bonus counts only while he wears all
  heavy. For the armor pick this leans toward what he already wears, which is the stable direction.
- **The target is the follower himself** for the two shapes that take one (EP35, EP29). The engine's own display read
  of weapon damage (`PlayerCharacter::GetDamage`, id 40291 / 39215) passes the perk owner as the target, and MFO
  follows that. A bonus conditioned on the victim (a bane perk) counts only if it would hold against a body like his
  own. That under-counts bane perks for most followers and over-counts one for a follower who is himself that kind of
  target (an undead follower with a "more against undead" perk). The per-target truth is `iaus/Fills.cpp`'s job (it
  asks EP29 with the real target for the damage estimate). The table answers "what are his perks worth to this kind
  of gear or school in general".

**What is asked about (the item).** The best weapon of each kind he carries, every rated armor piece he carries (the
mean per type), every castable spell he knows (the mean per school, at most 64 spells a build). For a weapon kind or
armor type he carries none of, the load order's GENERIC item of that kind: the lowest-FormID playable, unenchanted,
untemplated weapon of the kind (damage above 0), and the same for a body armor piece of each type and for a shield.
It is picked once at data load from whatever the load order holds (no list-specific data), and the table says per kind
whether the answer came from his own item or the generic one. A perk conditioned on a material keyword the generic
item lacks reads 1.00 there until he owns a matching piece.

**Refusals.** An entry point is asked only through `Logistics::TradePerks::EntryPointValue`, which refuses a shape that
is not in its proven table, a null or wrong-typed argument, an unverified seat, and an owner with no middle-high
process. A refused or non-finite answer, or one outside 0..100, leaves that multiplier at 1.00 and is counted
(`Table::refused`, printed in the table line). Nothing is substituted.

## 4. The table

`MFO::PerkEffects::Table` (`native/iaus/PerkEffects.h`), immutable once published:

- `read` (`Iaus::PerkRead`, plain data, what the IAUS snapshot carries): `weaponDamage[OneHand|TwoHand|Bow|Crossbow]`,
  `spellCost[5]`, `spellMagnitude[5]`, `spellCount[5]`, `dualCast[5]`, `armorRating[Light|Heavy|Shield]`, `blockPct`,
  each group with its own read flag (`read`, `spellsRead`, `armorRead`, `blockRead`). The flags are separate because
  the simulator sets `read` for the weapon group alone.
- `weaponKind[9]` by `Progression::WeaponKind` bit index (sword, dagger, war axe, mace, greatsword, battleaxe,
  warhammer, bow, crossbow), with the form asked about and `Probe::Own | Generic | None` per kind. The four posture
  classes are the best kind of the class among the kinds he carries, else among the generic answers.
- `armorPieces[3]`, `armorProbe[3]`, `spellsKnown`, `spellsAsked`.
- the census: `perks`, `entryPointEntries`, `abilityEntries`, `questEntries`, `entryCount[92]`, `abilityAv`.
- `refused`, `buildMicros`, `text` (the log line, also the change signature).

## 5. Threading

Perks live on the actor and its base and change on the main thread. Entry-point conditions read live actor and world
state with no lock. So (INVARIANTS #85, the #84 pattern):

- **Built on the main thread.** `PerkEffects::BuildPass` runs inside the snapshot pass `Diagnostics.cpp` already posts
  once per worker tick, after `InvSnap::BuildPass` and `EffectMirror::BuildPass`. It builds AT MOST ONE follower per
  pass: the one whose table is oldest and at least 15 ticks (~2 s) old, never-built first. Its cost is inside the pass
  time `[invsnap]` already reports, and each table carries its own `buildMicros`.
- **Read anywhere.** `PerkEffects::Get(formID)` hands out `shared_ptr<const Table>` under a leaf mutex and never waits.
- **Not yet is not empty.** No table = not known. The IAUS snapshot keeps every group unread (the considerations are
  not applicable), and the gear picks use the factor 1.0 (the pick MFO made before this reader).
- **No expiry.** A table stands until a newer one replaces it (principle 9: a floor is safe). Perks change on a
  level-up or a respec. A follower the engine cannot evaluate now (unloaded: no middle-high process) keeps his last
  table, and that is said once per wait.
- **No tick gate.** The party's tick gate (#84 rule 2) is not extended: a perk table two seconds old is as good as a
  fresh one, and gating the tick on it would buy nothing.
- The party is named each tick (`PerkEffects::Want`, beside `EffectMirror::WantParty`). A slot nobody named for ~15 s
  is forgotten. `Clear()` runs in `ResetAllState` beside the other snapshots.
- The two gear-pick weights are mirrored in atomics (`SetWeights`, called by `TuningFile.cpp` after each shared.txt
  load), because the gear picks also run on the main thread while the worker swaps the live tuning table.

## 6. How each answer becomes a consideration input

| Decision | Input | Where | Weight (by name) | Shipped |
|---|---|---|---|---|
| Posture (ranged vs melee) | `Facts::perkShare` = this side's weapon-damage multiplier over the sum of both sides | `Generate.cpp` posture (unchanged code, batch 3 part C), now fed by the game | `Cons::BoostPosturePerks`, curve `c.perks.py0..py2` in shared.txt or a class file | FLAT 1.0 (unchanged: this curve is a searched value, the sim batch owns it) |
| Spell choice | `Facts::spellPerkEdge` = `PerkSpellEdge(perks, school)` = 1 - cost multiplier / magnitude multiplier, 0..1; not known when the group is unread, the cast is a scroll or a staff, or no spell of the school was asked | `Generate.cpp` cast candidates | NEW `Cons::BoostSpellPerks` (`spell_perks`), a Boost on CastOffense, Exploit, CastControl, CastHeal, CastBuff, CastSummon, CastWard; curve `c.spell_perks.py0..py2` | PLACEHOLDER: x1.00 at 0, x1.15 at 0.5, flat after |
| Armor pick | `PerkEffects::ArmorFactor` = 1 + weight x (EP85 multiplier - 1), multiplied into `ArmorPref::heavyBias / lightBias` | `logistics/Gear.cpp ArmorPrefFor` (so loot, keep, owned-equip, sell and the buy thresholds all get it) | `Tuning::perkArmorWeight`, `t.perkArmor` in shared.txt | PLACEHOLDER 1.0 (the perk counts at face value) |
| Weapon pick | `WeaponRoles::perkKind[9]` = 1 + weight x (EP35 multiplier - 1), multiplied into `WeaponScore` for the weapon's kind | `Logistics_internal.h WeaponScore`, filled by `Gear.cpp ComputeWeaponRoles`; carried to the combat pick (`CombatWeaponStyle::perkKind`) and the buy planner (`BuyThresholds::perkKind`) | `Tuning::perkWeaponWeight`, `t.perkWeapon` in shared.txt | PLACEHOLDER 1.0 |

The armor and weapon factors multiply a score, like the existing class and style biases. They are never a filter, and
the factor is floored at 0.1 so a weight can never zero or flip a score. The existing perk VOTES (`StyleVotes`,
`kStyleBias`, `kArmorPerkBias`) are untouched: they count how many perk ranks are conditioned on a kind, and the new
factor is how much the engine says those perks are worth. Folding the votes into the factor is a tuning question
(REVIEW-BACKLOG MFO-B860 item 5).

Recorded and not scored yet: `dualCast`, `blockPct`, the ability actor values, the per-entry-point counts.

## 7. Logs

- `[perk-fx] perk-effect reader ON ...` once at data load: the entry points asked, the cadence, the two weights and the
  generic item per kind. `... OFF` (error) when the HandleEntryPoint row did not verify: no table is ever built.
- `[perk-fx] <name> (<id>): N perk(s) ... | weapon damage x ... | armor rating x ... | spells ... | block ... | perk
  ability actor values ... | entries by entry point ... | built in N us`: the per-follower table, ONE line per CHANGE
  of the table (first build, a new perk, a different best weapon, a new armor piece, a learned spell).
- `[perk-fx] <id>: perks cannot be evaluated now ...` once per wait.
- `[armor]` (existing line) now also prints the two perk factors inside the biases.
- `[tuning]` reports `t.perkArmor` / `t.perkWeapon` and `c.spell_perks.*` lines like every other tuned value.

## 8. Self-test and harness

- DEV self-test `perk.effects_table` (`native/selftest/TestsPerkEffects.cpp`, read-only): the table arrives, every
  group is read and every multiplier is sane. A multiplier that is not 1.00 must have at least one entry of its entry
  point in his base perks (the census checked against the engine). The real Generate + ScoreOne on a synthetic
  one-spell fight score by the live `spell_perks` curve, and an unread group changes nothing. The evidence line is his
  real table.
- Harness batch "spell perks" (`tools/iaus_harness/harness.cpp SpellPerkRows`): the pure input rule, the shipped
  placeholder, bit-identical scoring when the input is not known, Generate's copy onto spell casts only.

## 9. What the simulator mirror needs (a separate Fable sim batch; nothing in `tools/iaus_sim` was touched)

1. **Fill the spell group.** `Policy.h` sets `sn.perks.read` and `weaponDamage` from the lab's perk chain today. It
   needs `spellsRead = true` and, per school, `spellCost`, `spellMagnitude`, `spellCount` from the same chain (the
   lab's cost and magnitude multipliers at the spell's school). Until then `Cons::BoostSpellPerks` is not applicable
   in the sim and the sim scores exactly as before (checked: `run.sh --compare baseline_builtin.txt`, section 10).
2. **Knob names.** `c.spell_perks.py0 / py1 / py2` work in the sim's `--set` already (the curve is in `Tuning::curve`).
   `t.perkArmor` and `t.perkWeapon` are in the game's `SetTuningScalar` only. The sim's `kTuneScalars` (`Tune.h`) does
   not list them and does not need to: the sim has no gear pick. If the tune driver ever round-trips shared.txt it
   must pass them through.
3. **Search.** The three points of `spell_perks`, and the three points of `perks` (posture) now that the game feeds
   it. The posture curve stays flat in the game until that search writes it.
4. **Optional.** `armorRating` and `blockPct` exist in `PerkRead` if the lab wants a mitigation term. No game
   consideration reads them in a fight yet.

## 10. Checks run

- Local MSVC build of the worktree (release and DEV self-test configurations).
- `tools/iaus_harness/run.sh`: the new batch passes. Two checks fail on `origin/main` 43c075a2 as well (H4 "a
  concentration (cone) spell = its range x0.8" and "a cone / touch spell wants the foe near"); they are not from this
  branch.
- `tools/iaus_sim/run.sh --classes beta1 --fights 8 --seed 1 --compare baseline_builtin.txt` with the unedited sim
  sources: result recorded in the branch report.
