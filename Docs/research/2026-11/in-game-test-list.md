# In-game test list (needs a play session; goal set 2026-11-09)
No sessions for a while. Every item here is tagged `needs-session` in ClickUp with its due date cleared; due dates get set when sessions resume.

| ClickUp | What to test | Pass condition |
|---|---|---|
| 1dfcuabbt0p | Script-archetype spells cast, animated (Cicero Arcane Pull etc.) | spell fires with animation |
| 1dfcuabbnb6 | Substrate: OOC casts on Sven time out (2 of 10) | no ttl timeouts without a built cast |
| 86e3ednub | Batch-8 play session (classes + ports), normal play on the Deck | no regressions |
| 1dfcuabcqd9 | ch.18 power floor: does it slow the attack rate? (Warrior class 40% floor) | power share reaches ~40%, attack rate not visibly lower; check [atk-sel] zeroed / floorStood / powerUnable |
| 86e3pyjk2 | Taunt failed its Deck test (2026-10-13) | taunt pulls the foe |
| 86e3h87hb | Batch A freeze 2026-09-30: if it recurs, keep the game frozen and capture stacks (gdb winstk.py) | capture or no recurrence |
| (ch.18 field rung) | Claim a melee follower with Power denied: `zeroed` rises and power attacks stop | as stated |
| (class M0 probe) | One passive probe run: G1 sneak held IN combat (does the combat tree stand an NPC up?), the H10 invisibility drop, G4/H8 perk-condition behaviour on NPCs | each observed or refuted in the log |
| 86e3d6dnv | HMS: one v2.8.x session watching the "adopted total capped" and parity lines | caps fire, follower totals track the player's |
| 86e3d6dp0 | Magicka cost capture (needs the tier-C log line first): once-per-caster skill / curve / perk / player-curve factors + cost GMSTs | numbers match the research's curve model |
| 86e3t1gfn | Inactive winner blocking a lower source: reproduce, keep the Harbinger "stands / YIELDS" line naming the credited source + priority | identify the decision point (Engine, foreign hook, substrate) |
| class H3 | Passive heading-vs-velocity probe while a recentred combat area runs a follower back | runs facing forward vs strafes/backpedals |
| class H5 | Hit/miss detection probe for follower arrows (no seat) | real miss rate per follower; does the solve lead movers |
| class G11 | Read-only probe: does a foe's hand caster fill its target field (+0x20) while charging aimed spells (state 2/3) | target present while charging |
| class G7 | Which corpse the engine picks for a follower's reanimate | sensible corpse, else seat the corpse search |
| 86e3t1gfx | Bound weapons (after the fix): bound-bow gambit in combat logs road=direct-self, equip-auth declare bound, equip-obs allow, no [cfc] NO observed cast, bow fires; also a bound SWORD gambit to see if the claim road works | bow in hand and firing |
| 86e3kcvfk + Harbinger flee task | Retreat: passive flee-origin probe; after the fix, APMF [obs] current package = FECF983A (MFO retreat) not FF00xxxx Flee, dPlayer falls to <=200 u, release then re-engage/follow | no run to another room |
| 86e3t1gfq | Skooma: follower (with a cast gambit) carrying Skooma + real potions in combat: no "CAST Skooma ... AI-fired"; allow-list count excludes it; real potions still drunk | as stated (a non-caster may still drink until the Harbinger potion allowance) |
| MFO-B1 | Heal left + offense right together: right-only offense claim logs Equipped right, no left equip/debt; left heal not displaced; level-4 heal Prepare does not deselect the right offense spell; shield left stays; take-backs work | as stated |
| IAUS S0 (1dfcuabcr8m) | Passive scroll probe: scroll in a follower's inventory; log scroll combat-inventory builds and uses; read-only Harbinger claim on a scroll form | does the AI use scrolls; can a claim drive one |
| IAUS I3 (1dfcuabcr8k) | Exploit rows on a fire-weak and a frost-resistant foe | picks the element the foe is weak to, avoids the resisted one |
| G10 WillWork (1dfcuabcqe7) | Calm/Fear/Frenzy at a foe ABOVE the spell's level cap skips with "will not work: ... affects up to level N, target is level M" on the board; at or below the cap it fires; Turn Undead on a living foe skips, on an undead fires; normal Flames/Healing rows unaffected | as stated |
| AUTO control (1dfcuabcr0d) | A fear or paralyze row on a stronger foe with AUTO target casts on that foe, never an ally/self/player; `[classify] ... classified Offense` once per unflagged spell; no `[auto] nature undeclared` for control spells (Script spells still warn); a rowless control claim that never starts releases after ~4 s | as stated |
| G9 cooldown (1dfcuabcqe6) | Load a real v5 save with [Debug] bClassCoSaveSelfTest=1: "MCLS self-test (rows v5, pre-G9): PASS", FLWR summary matches the last session; give a Warrior "foe blocking -> power attack" a 10 s cooldown: one swing about every 10 s, tooltip "last: cooling down (Ns)", the closing step does not block its own swing; save (schema v6), reload: cooldown still 10 s | as stated; DO NOT load a v6 save on an older MFO and save |

## Spell cost parity (feat/follower-spell-cost-parity c0f6b1a, pending review + next prerelease)
- Startup: player + NPC curve lines, 5 ratio lines (skill 0/25/50/75/100), "ACTIVE".
- Direct road: `[costparity] ... direct` line; pool drops by player cost P.
- Animated road: `animated ... topped up`; pool drops by P total (engine E + top-up).
- Concentration (both roads): drain = P per second.
- Worked example: Jesper, Fast Healing (was 3.5 engine cost).
- Controls: MFO-OFF follower pays E; Powers unchanged; bFollowerSpellCostParity=0 -> E on both roads.
- Pool never below 0; no cast double-billed.

## Tailor compat (1dfcuabcrr7) -- only if Tailor is installed on a test list
- T1 preview with the MFO board never opened; T2 right after closing the board and again after 2 s; T3 open Tailor with the board open; T4 preview on a managed follower + grep equip denies; T5 both input hooks installed in logs; T6 non-ASCII typing in a Tailor field (MFO-B560).

## MFO-B1a (92b6e84)
- Follower with heal LEFT + one-hander RIGHT + an offense cast rule: sword stays, no right claim minted each lap, no right-plan lock spam.
- Pure caster heal LEFT / offense RIGHT: b1.heal_left_offense_right still PASS on the DEV DLL.

## Retreat hold through engine Flee (912e545)
- Follower reaches player at full HP while engine Flee current: "[retreat] ... Flee package is still current ... holding" then "engine Flee ended after Ns" + release. No Flee: old "stay over ... healed". Bound WARN after 20 s if a Flee never ends.
- Also: HP dip mid-hold + stay time reached with Flee current -> 'stay time reached ... holding'; then 'engine Flee ended ... stay time over' release.

## HMS above-parity gate (e3d15c4)
- Cap ON, follower at cap: on player level-up "[hms-parity] <id> above player parity: held total T vs player P -> headroom H ..." then "applied ~H withheld ~A-H"; total ends ~P; no "award trimmed to the player's total" line.
- Cap OFF (bHmsCapToPlayerTotal=0), recruit far above player: headroom 0, applied 0.0, base H/M/S unchanged until the player passes him.
- Follower below player: no above-parity line, applied unchanged. Jesper (fixed-stat): grant lines unchanged. Reload same level-up twice: same numbers.

## BLOCKER for every class test (C6 carriers, H5 miss budgets, C5b): nothing can EQUIP a class yet (SetEquipped/ActivateClass/SwitchClass have no caller until C10 GUI). UNBLOCKED by C5c (af93cea): use the DEV DLL from main (deck-stage-2100/dev-main/), enable MFO_Classes.esl, bClassTree=1, and set [Debug] sDevClassEquip = <Plugin.esp|LOCALHEX>:archer:5, <Plugin.esp|LOCALHEX>:warrior:15:mastered, <Plugin.esp|LOCALHEX>:knight:3 (comma-separated; LOCAL id only, no load-order byte, max 0xFFF for ESL). Expect "[classes] DEV equip <fid> (<name>) -> archer L5 (dev build only)".

## Class C6 passive carriers (84678a5) -- needs MFO_Classes.esl + bClassTree=1
- Load: "[classes] carriers: 22 carrier class(es) resolved from MFO_Classes.esl (21 perk(s), 23 ability spell(s), 3 marker spell(s))" + 22 "carrier MISSING -- <class>: <facet>" lines.
- First class equip: "carriers <fid> (<name>): 21 perk(s) granted (N entr(ies) live) ... ranks: <slug>=<n>". Warrior L7 -> warrior=7; mastered Warrior + Knight equipped -> warrior=100; T4 equipped -> its two T3 parents at 20; Mercenary gains MFOC_Mercenary_Full.
- Effects: Warrior melee +15%, Conjurer/Healer cost -15%, Living Legend ~40% hits ignored at L25 while attacking.
- ClearEquipped (classes OFF): all ranks 0, abilities removed, no error. bClassTree=0: TEARDOWN line per follower. Never a "faction ... read back as" error.

## Hidden player-only perk census (92604e0) -- log only; also: perks from active effects show src=mgef:..., condPlayerish field; full list once per load then ADDED/REMOVED only; no frame hitches on the Deck
- One "tree set:", one "candidate forms:", per-perk [inherit-census] lines, one summary per load; treePerksHeld > 0 on a mid-game save (0 = invalid census); no skill-tree perks listed; no repeat summary until a level-up/perk change. BRING THE LIST TO MARTH (rule vs bonus) before card C2.

## Spell-row alias + shape census (bbf90fd)
- Oakflesh + Lesser Ward cast by a follower: "[buff] ... CLAIM ... (animated" with row Armor/Ward; [archetype] predicted=Armor/Ward (not none); visibly animated.
- grep "shape-census": Wall/Rune/Summon/SelfHazard/TwoHanded/ScriptRider buckets; bSpellShapeCensus=0 silences it. No crash at load.

## H5 projectile aim (APMF 62dc4e7 + MFO 7e8dc21) -- APMF [ProjectileAim] bProbe=1, MFO bAimProbe=1, class ESL present
- Install line "[ch.29] projectile-aim seat installed on 1.6.1170 ... gravity scale ... (now <NON-ZERO>)".
- "[aim] ... ch.29 aim claim FILED (... class Archer, budget 5 misses in 10)" + "[ch.29] ... projectile aim ENGAGED".
- Per shot: "SHOT #n ENGINE|CORRECTED ... delta (dz, dx) deg" (a few degrees), then "HIT (corrected)" or "MISS (corrected) ... projectile-to-target D". A run of corrected misses with a large same-direction distance = ballistic model error (report).
- Ranger window never > 3/10, Jaqspur <= 1/10 (iAimBudgetJaqspur=0 to test never-miss). Bash/melee hits must NOT count as arrow hits; opening a menu mid-flight must not create misses.
- C6 extra: a follower left waiting in ANOTHER cell at load must still get his passives when he rejoins (no "dropped" line). Scout +20% speed: check it applies at once, not only after he loots something (reviewer SEV-3-field).
- C6: Warlock/Nightblade bonus must not apply to ally buffs (Courage/Rally) after the target gate fix.
- Deck check: Warlock/Nightblade with Courage/Rally/Call to Arms: how much stronger do the buffs get? (marth Q58: quick patch after release if very strong)

## ADDENDUM 2026-11-10: next session = LoreRim on this PC (ClickUp 1dfcuabcqdm due 2026-11-11)

### ONE RUN COVERS MOST OF THE LIST: the self-driving dev self-test (main 2b0b91a, DEV DLL)
Setup (Docs/SELFTEST.md): DEV MFO.dll + .pdb in LoreRim mods/MFO, Harbinger main (ch.30), MFO_Classes.esl on, bClassTree=1, bAimBudget=1. One follower, Combat page rule DO "DEV: run self-tests". Stand somewhere open and quiet (no town/inn/house, nobody friendly near). Prefer an ESSENTIAL/protected follower: the hurt-% tests refuse (RETRY) otherwise.
Read: `grep '\[selftest\]' MFO.log`. End line `ALL DONE: PASS n, FAIL n, MANUAL n (ids), INFO n` (or `ALL RUNNABLE DONE ... BLOCKED n` with one follower: the ally-buff test needs a 2nd ally).

| self-test id | covers list item |
|---|---|
| b1.heal_left_offense_right, b1a.weapon_stays_right | MFO-B1, MFO-B1a |
| costparity.fireforget, costparity.concentration | Spell cost parity |
| buff.animated_claim_road, census.shape_ran | Spell-row alias + shape census |
| class.c6_carriers, class.scout_speed | C6 passive carriers, Scout speed |
| aim.h5_miss_budget | H5 projectile aim (budget) |
| class.ally_buff_strength | Q58 Warlock/Nightblade ally-buff strength (INFO) |
| skooma.not_drunk_by_ai | Skooma (86e3t1gfq) |
| bound_bow.fires | Bound weapons (86e3t1gfx), bow half |
| hms.cap_holds, hms.above_parity_gate | HMS (86e3d6dnv), above-parity gate (needs a level-up in the window) |
| prog.perk_entries_unique | perk-dup (c3018f1) |
| census.hidden_perk_ran | Hidden-perk census (then BRING THE LIST TO MARTH) |

### Still MANUAL after the self-test (look by hand)
retreat.flee_hold, visual.animated_casts, visual.frame_hitches, ch18.power_floor, taunt.pull, substrate.ooc_timeouts, probe M0/H3/G11/G7 + H5 hit/miss probe, log.inactive_winner, iaus.scroll_probe, iaus.exploit_rows, g10.will_work, auto.control_target, g9.cooldown, tailor.compat, magicka.cost_capture, freeze.batch_a, play.batch8.

### NEW: Harbinger ch.30 potion allowance (APMF main 599346f; the MFO adoption card is next)
- Install line for ch.30 with every row verified; 1.7.104 refused by name.
- A claimed follower with Skooma refused: the AI never selects it (0x0F seat) and never drinks it (DrinkPotion 0x10F seat); real potions still drunk.
- Player drinking from the menu is unaffected; a console/script drink on that follower STANDS (Q61 default allow); a QUEUED script drink is refused (known hole APMF-B258, logged loudly).

### Saved question
- Q61 (still OPEN): should scripts/console still make a "no drugs" follower drink? Default kept: allow.
