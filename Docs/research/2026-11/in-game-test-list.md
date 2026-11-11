# MORNING TEST 2026-11-11: Sentient AI (IAUS) first run, LoreRim Ultra
Build: main d1fc315 (DEV DLL) with Harbinger 599346f, deployed. "Sentient AI" is the player-facing name (IAUS in the logs). Sources: iaus-i6.md (steps 1-30), iaus-i7.md, iaus-i1-seam.md, taunt-rules.md.

## Setup
- In MFO.ini `[Debug]` set `bIausExplain=1` for this run. That turns on the `[iaus-why]` lines after each `[iaus]` line.
- Three followers: one Melee, one Ranged (bow and a sword), one Mage T1. Turn Sentient AI ON for each with the Gambits tab button "Sentient AI: On/Off" (default is Off).
- Do the whole run twice: class tree OFF first, then tree ON (only checks 29 and the notes say where the tree matters).
- Have a save from before the run for the parity check (check 4).

## A. Off means off
1. All three followers with Sentient AI OFF, fight one group. `MFO.log` has no `[iaus]`, `[iaus-gen]`, `[spellbook]` or `[precision]` line.
2. Parity (IAUS-off `[eval]`): load the same save on the last main build deployed before this one, then on this DLL. Same follower, same table, about 60 s of the same fight each. `grep '\[eval\]'` both logs, strip timestamps, compare. Expect the same rule numbers in `fired rule N`, `rules A,B skipped -> rule N`, `did NOT fire`, `held off` lines in the same order and the same `combat table: N rules, none matched` line. Board pulses and a G9 rule's "cooling down" look as on main. Timing differences are fine, rule NUMBER differences are a defect.

## B. The source runs (Sentient AI ON)
3. Enter combat. Within about a second: one `[spellbook] <name>: spells offense=..` line per follower, then `[iaus-gen]` every 10 s and an `[iaus]` line on every change of pick, each followed by `[iaus-why]`. No `[eval] combat table: N rules, none matched` line for them.
4. Melee T1: `[iaus] pick=attack -> <foe>` and he fights that foe. The pick does not flip between foes every second. `[eval] ... held off: already on that target` appears once, not per tick.
5. Ranged T1: `[iaus-gen] ... posture bow` at range. Walk a foe into him and the posture turns to melee after the dwell with ONE swap, and back when the foe leaves. No weapon vanishing.
6. Mage T1: casts are animated at the foe the `[iaus]` line names. No hostile cast at the player or an ally. Expected until card I8, not a defect: with the default profile he may draw a melee weapon when a foe is near and cast one-handed. Note how often.
7. Idle: when nothing clears the floor the line reads `idle: nothing above the floor` and he fights on with his own AI. He must not stand still.

## C. The I6 rules
8. Q67: hurt a follower who has a heal while he keeps attacking. The heal comes within about a second. `[iaus] hold=` lines stay rare.
9. Heal hand: while a heal runs on one hand, no other cast takes that hand until the recipient is full.
10. Power attack (tree OFF): at most one swing per 10 s. The closing leg does not start the cooldown.
11. Ward: a mage who knows a ward, against a caster. `[spellbook]` counts `ward=1` and a `cast_ward` pick appears.
12. B661: a mage with a bolt spell while a melee follower stands on the target still casts it, and `[spellbook] area probe:` for it says `cosmetic explosion: no area`. With Fireball he must NOT cast into the melee.
13. B658: after he summons, the summon is not a candidate again while it lives. After a drink the potion count in the next `[spellbook]` line is one lower.
14. `[iaus-probe] player currentCombatTarget: set on N of M`. In a real fight N must be above 0. If it stays 0, report it.
15. D1: a spell answered "won't work" (G10 level cap, Turn Undead on a living foe) no longer flickers in and out of the `[iaus]` ranked list every second lap. It stays out.

## D. Taunt and power rules (taunt-rules.md)
16. Taunt (melee follower, `bTaunt=1`): he taunts a foe that is on the player or an ally. A second taunt inside 30 s logs `[taunt] skip: cooling down`, hit or miss.
17. Success follows the level gap: chance is 50% plus 5% per level the taunter is above the foe, clamped 10% to 90%. A much higher level taunter lands nearly always, a much lower one rarely.
18. When every foe is already pinned: `[taunt] skip: every foe already taunted` and no idle, no cooldown started.
19. Two knights (two melee followers with taunt) never double-taunt one foe, in Sentient AI mode too.
20. Power rows cannot stack: with two power-attack rows on one follower there is still only one swing per 10 s, shared.
21. Toggle Sentient AI off or on mid-fight right after a taunt: no extra taunt inside the 30 s.

## E. Toggle, board and gate (I7 and bIausAvailable)
22. Switch ON for one follower, save, reload: the button still says On. The other followers stay Off.
23. Gamepad: d-pad to the Sentient AI button, [A] flips it. No mouse needed.
24. Fight with it on: the Thinking panel (Combat page) shows the decision, top 3 candidates and their scores moving. After the fight it reads "Last decision, N s ago".
25. Switch OFF mid-fight (then ON again): one `[iaus] ... combat source: IAUS -> gambit table` line. The spell leaves his hand, no cast keeps running, the weapon stays, the table takes over next lap, the panel disappears. With an equip rule holding a two-hander or shield, the heal still takes the left hand on the next laps (no `[heal-hand]` refusal naming a rule 0-254).
26. MCM "Sentient AI available" OFF mid-fight: the follower stands down to his gambits on his next scan.
27. Available OFF, save, load, then Available ON: Sentient AI resumes for the followers who had it on.
28. With Available OFF: the board button is greyed with a tooltip and the Thinking panel is hidden.
29. `bIausExplain=0` (flip after the run): no `[iaus-why]` lines.

## F. Edges
30. Dismiss a Sentient AI follower mid-fight and recruit him again. No crash, his first `[iaus]` line starts clean.
31. Save and load mid-fight. No `[iaus]` or `[spellbook]` line from before the load, a fresh `[spellbook]` line after.
32. Retreat: a follower who falls back heals himself (`[retreat-heal] ... IAUS self-heal`) and nobody else.
33. Knock him down or paralyse him: `[iaus] ... walk=0 (1: ... 'cannot act: ...')` once, no cast, weapon and spell stay.
34. Budget: `[iaus]` lines end with the decision time. With 8 foes and about 30 spells it stays under 0.5 ms and there is no `over fIausBudgetMs` warning.
35. Rune is counted under not-candidates with one `no IAUS candidate: target location` line.
36. Give him a potion mid-fight then take it back: the count follows within one lap.
37. Teach him a spell by console mid-fight: it appears within 5 s.
38. A staff, charged and empty: the line says `(charged)` or `(EMPTY)`. This read is unobserved (MFO-B656).
39. A buff already on him (Oakflesh): no buff candidate for him while it runs.
40. A melee-only follower against a flying dragon: no attack names the dragon while it is airborne out of reach.
41. Tavern brawl: no candidate names the brawl partner.
42. Tree ON, a Warrior with a known heal and no mastered mage class: `dropped class` above 0 and no cast. Master a mage class and the heal appears, scoring low until someone is badly hurt. Tree OFF: the same Warrior casts it.

## G. Probe lines to capture for the sim calibration
43. `[precision] bridge live` once, and `[spellbook] reach probe:` lines with a capsule length above 0 for each follower holding a weapon. Write down per weapon the Precision length and the distance at the time. Without Precision the line says the DLL is not loaded.
44. `[spellbook] area probe:` lines for every area spell used (Fireball, bolt spells).
45. Staff probe: the `[spellbook]` staff line for one charged and one empty staff.
46. `[iaus-probe]` lines (player target probe from check 14, and any other `[iaus-probe]` line).

## What to send back
- `MFO.log` from this run. It sits where the earlier sections' logs do: `/mnt/gaming/modlists/LoreRim/overwrite/SKSE/Plugins/MFO.log` (verified). Also the main-build `MFO.log` from check 2.
- Any crash log (Crash Logger / .NET Script Framework file) and the time it happened.
- Which checks you skipped, and notes on 6 (how often the mage drew a weapon) and 43 (length and distance per weapon).

---

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

### NEW: Harbinger ch.31 combat move, the BASH road (APMF `feat/combat-move` + MFO `feat/iaus-combat-move`, both pending review; iaus-sim-v2.md section 29)
Needs BOTH DLLs from those branches. A follower with a shield (or any melee weapon) and a melee fight.
- Setup: `APMF.ini [Probe] bCombatMove=1`, `MFO.ini [Debug] bCombatMoveProbe=1`. Leave `bIausBashRoad=0`.
- Install line: `[combat-move] installed (kIntent_CombatMove, ABI v38) ... hit sink registered; [Probe] bCombatMove=1` in APMF.log.
- PASSIVE part (any melee fight, APMF.log): `[combat-move-probe] HIT ...` lines. Wanted: one where a follower or a foe BASHED by itself (`bash=true`, attack data `bashStart`), one where a follower BLOCKED a hit (`blocked=true`, `target wantBlocking=true`), and several follower POWER ATTACKS (do they read `power=true`, and what is the attacker's attack data?).
- SENT part: every 6 s in melee reach MFO.log shows `[combat-move] ... PROBE bash requested`. Then in APMF.log either `[combat-move] <h> STARTED: ... the engine set his attack data and the Bash attack state` followed by `[combat-move] <h> HIT ...`, or `REFUSED` / `FAILED` with a reason. MFO.log repeats the outcome (`STARTED`, then `HIT` or `NO bash hit arrived`).
- By eye: does he visibly bash (the shield or weapon shove), does the foe stagger, is a charging mage interrupted, does his stamina drop?
- PASS = STARTED and HIT lines with `bash=true` in the probe line, and it looks like a bash. Anything else: send both logs, change nothing.
- Then turn both probe switches OFF again.

### NEW: give ground, Harbinger ch.24 on a point (APMF `feat/approach-point` + MFO `feat/iaus-combat-move`, pending review; iaus-sim-v2.md 29.6)
- NOT TESTABLE YET: the step is chosen from the follower's seconds to live, which the engine-side estimate fill (the next card) provides. Run this entry only on a build that has that fill.
- Setup: `MFO.ini [Debug] bIausGroundRoad=1`, Sentient AI ON for an archer or a mage follower, a melee brute on him.
- MFO.log: `[ground] <id> '<name>': step back from <foe> ... to x,y,z (R 150, h=..)`, then within 2.5 s `[ground] <id>: step back h=.. over: arrived (the area carried the bound N time(s) ...)`. N must be above 0.
- APMF.log: `[ch.24] 0x.. combat-approach claim ENGAGED: close to within 150 of the POINT x,y,z`.
- By eye: does he back off while still fighting, or turn his back and walk? Does he get stuck against a wall?
- FAIL lines to send: `the engine never carried the bound`, `another ch.24 claim owns him`, `ENGINE DROPPED`.

### Saved question
- Q61 (still OPEN): should scripts/console still make a "no drugs" follower drink? Default kept: allow.
