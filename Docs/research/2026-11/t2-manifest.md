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
| C8 | Class combat packages: per-stance CSTY, ch.18 attack mixes, leash scale (5e69f06) | DONE | DONE (batch S, iaus-sim-v2 31: ch.18 window, ch.23 leash, the engine's own power attacks) |
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
| H2 | Aversion, ABI v41 (Conjurer, Healer) | DONE (Harbinger 1b1ef40, MFO 057d93c) | DONE (batch S: weight 0.25, equipped class, 6 s windows) |
| H5 | Archer miss correction, ch.29 + MFO miss budgets | DONE | DONE (batch S: the budget window. The uncorrected miss rate stays a lab number, `--aim-miss` / `--aim-scatter`, until the `[aim]` probe) |
| G7 | Corpse target / reanimate | DONE (Harbinger 7407303, MFO a4a7e21) | DONE (batch S: kept, raised from a table row. IAUS has no Reanimate family: gap C-G7) |
| T | Town theft fix (1dfcuabczbg): stolen-copy gate, guard gate, locked/owned door + furniture gate | DONE (merged to main; ships with T2 per marth 11-12) | n/a |

## 2. Per-class needs found missing in this pass

| Class | Need (source) | Card / owner | Status | Sim |
|---|---|---|---|---|
| Knight | "Blocks a lot": a held guard. No `act.block`, ch.31 `kCombatMove_Block` is RESERVED (class block, C-BLOCK, Q83 "build next") | Harbinger ch.31 Block (tier A) + MFO `act.block` | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | DONE (batch S: 2.5 s held, 4 s row, the held-guard gate. IAUS barely blocks: 31.4 finding 2) |
| Warrior | Power attacks that land as real power attacks: `act.power_attack` is still MFO's own graph drive (`cast/Fire.cpp:471`); Q82 "move to Harbinger" as ch.31 `kCombatMove_PowerAttack` | Harbinger ch.31 PowerAttack (tier A) + MFO port | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | DONE (batch S: the lab's power attack was the engine's. Refusals counted, no cooldown spent) |
| Warrior | A power-attack FLOOR: ch.18 only has a ceiling; needs a "Normal share max" (gap G-H2) | Harbinger ch.18 extension (tier A, ABI) | DONE (already built: ch.18 powerShareMin v34, Q-melee-5) | DONE (batch S: floor 0.40 with the stamina lift) |
| Mercenary | Dodge: "steps out of incoming spells and arrows", a list-agnostic dodge (gap G-H3, "the biggest risk in tier 2") | Harbinger dodge facet (tier A, RE first: list dodge framework when present, else the vanilla side step) | DONE via the style facet (side step rolls the CSTY Avoid Threat; Mercenary 0.3, Q-melee-7), field step 24x | DONE (batch S: a side step roll per projectile, MODEL) |
| Mercenary | "Backs off when two foes press, then darts in again"; the lab finds no way out for Knight/Warrior/Mercenary (C-NOFLEE, G21) | MFO IAUS: a Mercenary back-off road on H3 | DONE (batch I merge) (Family::BackOff, Q-I-3) | DONE (batch S) |
| Evoker (all mages) | Range follows the equipped spell (H4, C-H4) | MFO tier B on ch.24 + H3 back-off | DONE (batch I merge) (Reposition.h, RangeApproach.cpp, Q-I-1/2) | DONE (batch S: a bound that can only make him close. Neutral in the lab) |
| Thief | Stealth entry, 1 fight in 5 opens undetected (C13) | MFO tier B | DONE (batch I merge) (classes/StealthEntry.cpp, Q-I-4) | DONE (batch S: as built it does nothing in the lab, `--c13 lever` is the H10 guarantee) |
| Thief | `act.steal` default gambit is MISSING from Vocabulary (G13, crime pre-check) | MFO tier B | DONE (01a0e04) | DONE (batch S: `--steal-lab`, iaus-sim-v2 31.8) |
| Thief | Pick nearby locks (G14) | MFO tier B, with G13 | DONE (01a0e04; unowned chests only, owned parked per Q-L-6) | DONE (batch S: the same check, nothing passes its owned path today) |
| Archer | Off-class weapon rule: one-handed only, two-hander only if otherwise unarmed, half damage (G16) | MFO tier B on equip authority | DONE (01a0e04) | n/a (equip) |
| All, tree ON | Strict class style: "would need a combat style slider in harbinger" (packages 0.5). MFO writes `CombatController::combatStyle` itself today, against the Harbinger-first rule | Harbinger combat-style facet (tier A) + MFO C8 adoption | DONE (melee batch, Harbinger c522f3c / MFO merged 11-12) | n/a |
| All mage-line | Teach spells with IAUS on (marth 11-12: "I'm only concerned about the iaus issue. The class page will need a teach spell page for mage line and it will filter out spells that aren't weighted"). Today the only teach entry is a cast row's spell cell in the gambit table, which IAUS mode does not use (and Q37 hides). | MFO tier B (Sonnet): a Teach Spells page on the Classes tab, shown for mage-line followers; lists the player's castable tomes the follower does not know, filtered to spells IAUS would weight for his class (the iaus/SpellBook.cpp ClassGate / class_fit path; an unweighted spell is not listed); teaching consumes the tome and adds the spell (the existing TeachSpell edit). Buy and auto-learn roads unchanged | DONE on `feat/batch-i` (Board_Classes.cpp Teach Spells page, SpellBook::WouldWeigh) | DONE (batch S: `--teach <token>`, from the weighable pool only) |
| All, tree OFF | The three base classes with no tree (marth 11-12: "I dont see the 3 base classes with no tree in the sim. I imagine theyd say Line: Base"). The real-class sim builds only the 35 tree-ON rows; the tree-OFF cells (`melee-off` / `ranged-off` / `mage-off`) exist only in `--classes legacy` (Setup.h LegacyClasses). Add them to RealClasses as line "Base", tier 1. marth 11-12: "They do technically have weapon gates (mage knife etc), and should still loosely follow tree on versions. They just have all available options at thier disposal". So each base cell is built FROM its tree-ON T1 row (melee / ranged / mage: same weapon set and kit, so the mage keeps his dagger + staff, and the same default kit rows), and then opens every option the tree-OFF branch opens (SpellBook::GateFor, iaus/SpellBook.cpp:470: any known spell, power attack, taunt, retreat; scrollUser by the T1 root). Note for the author: in the DLL the tree-OFF weapon choice comes from the stance (combatClassOverride root) equip side, not the IAUS gate, which leaves weapons open; the sim mirrors it through the T1 kit. Vanilla dodge 0.20. Include them in `--classes beta1` | MFO sim (FABLE), in the sim batch | OPEN | the row itself |
| Sim dash | marth 11-12: "On the sim dash, the gap and trend portion on the class page never update and are always empty. id also like the third comlumn to be a rating(Tx) column. so we can easily see if a class in a tier is less effective than another. which means scenarios will have to be even more diverse". CAUSE (coordinator read, 11-12): tools/iaus_dash/dash.py class_gap() takes the mean oracle gap of the class's scenarios, else the ledger delta_u for that class slug. A normal class run writes 0 scenarios (39 of the last 40 runs in ~/.cache/mfo-iaus-sim/runs), every ledger delta_u is null, and the ledger's class field is a group name ("knight line", "healers", "casters", "*"), never a slug, so Gap is always empty and Trend (gap vs the previous run) with it. NEED: (1) every class report run fills a per-class gap (the oracle over that class's scenarios, priced so a normal run stays affordable) and the ledger maps its group names to slugs; (2) Classes page column 3 = Rating (Tx): the class's score on ONE shared rating battery, ranked inside its tier (e.g. "T2 3/9, -2.1 vs tier mean"), with a below-tier flag; Line moves right; (3) the rating battery: a diverse scenario set every class of a tier plays identically (foe archetypes melee swarm / archers / mages / healer-backed / boss / undead / big creature / ambush / kite-at-range / mixed, group sizes, level bands, equal gear budget per tier), so a tier comparison is fair | MFO sim + dash (FABLE: sim structure is Fable-only, marth 11-11), ONE batch with the tree-OFF base cells row above, after feat/dodge-rates and feat/iaus-pre-t2 merge | OPEN | the row itself |
| All followers | Heavy armor never dodges for EVERYONE (marth 11-12, Q-dodge-heavy-scope "Everyone"; cuirass decides). feat/dodge-rates covers T2+ class styles only (zero-avoid twins claimed by apmf/StyleClaims.cpp). Still open: tier-1 classes, classes OFF, the MFO stance styles (EquipChoiceOnly), and the Harbinger-absent degrade (CombatStyle::ApplyTick) | MFO (Sonnet), after feat/dodge-rates merges | OPEN | n/a (sim has no heavy armor) |
| All, sim | Health-decline curves and the best drink time (marth 11-12 Q-knight-drink: "the sim should help come up with health decline curves and perfect drinking times as well. If it uses real enemies and such"). The sim measures each class's health-decline curve against real enemies (the list's actors, never hard-coded list data in logic) and searches the drink line (DrinkNeed curve, the Knight guard-line cutoff) for the best outcome; the native defaults follow the result | MFO sim (FABLE), in the same sim batch as the dash and base-cell rows | OPEN | the row itself |
| All classes | Per-class curves (marth 11-12: "Each class shoudl already have its own curves as well"). FOUND (coordinator, 11-12): they do not. iaus/Core.h Profile holds per-class family / role / weight multipliers only (no curve shapes), and NOTHING in native calls BumpRow or SetWeight (Catalogue.cpp:132/139 only), so every class scores with the default Profile today; card I8 never landed (the sim uses it only with --profiles mech). NEED: Profile gains per-class curve overrides (knee / slope / line per consideration) on top of the weights; filled per class from the class table at equip; every value found by the sim per class against real enemies (the edge-number rule above). Native part tier B (Core.h is not byte-shared or serialized) | MFO IAUS (Sonnet) + the sim batch | DONE (merged 11-12; values from the Fable sim batch): MECHANISM on `feat/class-curves` (iaus/ClassCurves.*, Profile::curves, sim `--curve`; shipped rows = shared defaults, values come from the sim batch) | the row itself |
| Conjurer | Summon placed at a ground point (memory "summons target ground XYZ", packages Conjurer block) | Harbinger ch.8b pos road | OPEN, needed only if the field shows summons spawning badly | n/a |

RULE (marth 11-12): "The sim should be used to find all the ideal edge case numbers, thats the point of a sim for iaus". Every IAUS constant that decides an edge case (curve knees, boosts, health/magicka lines, hold/rest and touch-spent windows, cooldowns, ranges) is found by a sim search against real enemies before it ships; a hand value is a labelled placeholder with a row here. Current placeholders: the Knight guard-line boost 2.0 and its drink cutoff (~0.34, DrinkNeed), the 3 s touch-spent window (kTouchSpentSec), the 2.5 s / 1.5 s guard hold and rest. The sim batch adds a constant-search pass over them (and an inventory of every other hand-picked IAUS constant, each searched or listed).

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
