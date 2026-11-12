# T2 build manifest (Classes Beta 1, tiers 1-2): the single gate list

Written 2026-11-11 19:30 PT by the coordinator after marth: "i keep finding more unaccounted for t2 needs. c8, harbinger block, etc".
Built from every T1/T2 class block in `class-perks-packages.md` (section 2), the M0-M2 cards in `class-build-plan.md`,
the gap ledger in `iaus-classes.md`, and the answered questions (Q82, Q83, the 11-11 answers). Checked against both
repos' `origin/main` logs and branches, and against `native/Vocabulary.h` for every default-gambit opcode.

Rule (marth 11-11): T2 does not ship and is not field-tested until every row below is DONE, and every row is modelled
in `tools/iaus_sim` (marth 11-11 18:36: "they need to be in sim"). A new T2 need goes into THIS file first.

Status: DONE = merged to main. FLIGHT = branch in review or being written. OPEN = nothing built yet.

## 1. Machinery and base gambits

| Card | What | Status | Sim |
|---|---|---|---|
| C1-C7 | Skeleton, data model, MCLS, XP, mastery, carriers, ESL generator | DONE | n/a |
| C8 | Class combat packages: per-stance CSTY, ch.18 attack mixes, leash scale (5e69f06) | DONE | OPEN (C-C8: lab does not read the CSTY / ch.18 / ch.23 package) |
| C10, C11, C12 | Classes tab, MCM, defaults (Priest/Saint auras are T3, card C11b) | DONE | n/a |
| G1 | act.sneak | DONE | DONE |
| G5, G6 | Summon gates, foe attacking my summon | DONE | DONE |
| G9 | Per-rule cooldown (FLWR v6) | DONE | DONE |
| G10 | Spell will work | DONE | DONE |
| H1 ch.18 | Attack selection | DONE | via C8 row |
| ch.7 v40 | Offense = hostile only, allow-list nature (Harbinger 337e688) | DONE | DONE (flee/hide deny) |
| H3 | Disengage drive + anchor-leak fix (Harbinger 47962b2) | DONE | DONE (G3 run) |
| G2 | Hide-when-unseen Layer 1 | DONE | DONE (sim-stealth cfee1bf) |
| G3 | Disengage to out-of-sight, flee, hide run | DONE (3d2ca76) | DONE (sim-stealth cfee1bf) |
| H2 | Aversion, ABI v41 (Conjurer, Healer) | DONE (Harbinger 1b1ef40, MFO 057d93c) | OPEN (C-H2) |
| H5 | Archer miss correction, ch.29 + MFO miss budgets | DONE | OPEN (C-H5: lab miss rate is one fixed number) |
| G7 | Corpse target / reanimate | DONE (Harbinger 7407303, MFO a4a7e21) | OPEN (sim drops reanimate spells) |
| T | Town theft fix (1dfcuabczbg): stolen-copy gate, guard gate, locked/owned door + furniture gate | DONE (merged to main; ships with T2 per marth 11-12) | n/a |

## 2. Per-class needs found missing in this pass

| Class | Need (source) | Card / owner | Status | Sim |
|---|---|---|---|---|
| Knight | "Blocks a lot": a held guard. No `act.block`, ch.31 `kCombatMove_Block` is RESERVED (class block, C-BLOCK, Q83 "build next") | Harbinger ch.31 Block (tier A) + MFO `act.block` | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | OPEN |
| Warrior | Power attacks that land as real power attacks: `act.power_attack` is still MFO's own graph drive (`cast/Fire.cpp:471`); Q82 "move to Harbinger" as ch.31 `kCombatMove_PowerAttack` | Harbinger ch.31 PowerAttack (tier A) + MFO port | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | OPEN (C-POWER) |
| Warrior | A power-attack FLOOR: ch.18 only has a ceiling; needs a "Normal share max" (gap G-H2) | Harbinger ch.18 extension (tier A, ABI) | DONE (already built: ch.18 powerShareMin v34, Q-melee-5) | OPEN |
| Mercenary | Dodge: "steps out of incoming spells and arrows", a list-agnostic dodge (gap G-H3, "the biggest risk in tier 2") | Harbinger dodge facet (tier A, RE first: list dodge framework when present, else the vanilla side step) | DONE via the style facet (side step rolls the CSTY Avoid Threat; Mercenary 0.3, Q-melee-7), field step 24x | OPEN |
| Mercenary | "Backs off when two foes press, then darts in again"; the lab finds no way out for Knight/Warrior/Mercenary (C-NOFLEE, G21) | MFO IAUS: a Mercenary back-off road on H3 | OPEN (default below) | OPEN |
| Evoker (all mages) | Range follows the equipped spell (H4, C-H4) | MFO tier B on ch.24 + H3 back-off | OPEN | OPEN |
| Thief | Stealth entry, 1 fight in 5 opens undetected (C13) | MFO tier B | OPEN | OPEN |
| Thief | `act.steal` default gambit is MISSING from Vocabulary (G13, crime pre-check) | MFO tier B | FLIGHT (`feat/batch-l`) | OPEN (pre-check vs Detect.h) |
| Thief | Pick nearby locks (G14) | MFO tier B, with G13 | FLIGHT (`feat/batch-l`) | OPEN (pre-check) |
| Archer | Off-class weapon rule: one-handed only, two-hander only if otherwise unarmed, half damage (G16) | MFO tier B on equip authority | FLIGHT (`feat/batch-l`) | n/a (equip) |
| All, tree ON | Strict class style: "would need a combat style slider in harbinger" (packages 0.5). MFO writes `CombatController::combatStyle` itself today, against the Harbinger-first rule | Harbinger combat-style facet (tier A) + MFO C8 adoption | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | n/a |
| All mage-line | Teach spells with IAUS on (marth 11-12: "I'm only concerned about the iaus issue. The class page will need a teach spell page for mage line and it will filter out spells that aren't weighted"). Today the only teach entry is a cast row's spell cell in the gambit table, which IAUS mode does not use (and Q37 hides). | MFO tier B (Sonnet): a Teach Spells page on the Classes tab, shown for mage-line followers; lists the player's castable tomes the follower does not know, filtered to spells IAUS would weight for his class (the iaus/SpellBook.cpp ClassGate / class_fit path; an unweighted spell is not listed); teaching consumes the tome and adds the spell (the existing TeachSpell edit). Buy and auto-learn roads unchanged | OPEN (joins batch I) | OPEN (sim: a taught spell enters the book) |
| Conjurer | Summon placed at a ground point (memory "summons target ground XYZ", packages Conjurer block) | Harbinger ch.8b pos road | OPEN, needed only if the field shows summons spawning badly | n/a |

## 3. Defaults taken (saved as questions; marth may overrule)

- Q97 REVERSED: G13 steal IS T2 (the Thief's own default gambit `act.steal` is missing). G13 and G14 ship together.
- Q-T2-1 (Q83 order): Q83 said Block comes after the bash road is field-proven. Default: build Block now, and field-test
  bash and Block together in the T2 session, since T2 cannot test the Knight without it.
- Q-T2-2 (Q82 condition): Q82 moved the power attack to Harbinger "if the probe confirms". Default: build ch.31
  PowerAttack now and run the probe in the same session; the field read decides which road the gambit keeps.
- Q-T2-3 (C-NOFLEE): Knight and Warrior hold the line by design; the Mercenary gets a back-off road (his class text asks
  for it).

## 4. Batching (agent cap 3, 2 while a Fable runs)

1. Harbinger melee batch, one Fable: ch.31 Block + PowerAttack, ch.18 Normal-share-max, the dodge facet RE + seat,
   the combat-style facet. MFO adoption (`act.block`, power-attack port, C8 style adoption, Mercenary back-off) in the
   same run (pair rule).
2. MFO tier-B batch, one Sonnet: H4, G16, C13.
3. MFO tier-B batch, one Sonnet: G13 + G14 (logistics, crime pre-check).
4. Sim, one Fable after the code lands: C8 packages, H2, H5 miss budget, G7, the G13/G14 pre-check, block, power,
   dodge, H4, C13.
