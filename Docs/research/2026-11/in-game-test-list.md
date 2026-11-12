## T2 field session (Classes Beta 1 + IAUS field fixes)
Collected 2026-11-11 from class-build-plan.md cards C4, C8, C10, C11, C12, G2 and the Movement batch (branch `feat/g3-disengage`, its
`in-game-test-list.md`), plus the IAUS field-fix log lines. One play session. `MFO.log` / `APMF.log` are the files named below.
Nothing here was invented: each step quotes its source card. Steps marked (needs the G3 + ch.7 merge) cannot run until those branches are on main.

### SETUP
- Deploy `MFO.dll` + `APMF.dll` from CI (main once the class branches are merged), `MFO_Classes.esl`, `out/SEQ/MFO_Classes.seq`, and the class MCM files
  (`out/MCM/Config/MFO_Classes/config.json` + `settings.ini`, script `out/Scripts/MFOC_MCM.pex`). MCM Helper installed.
- `MFO.ini [Debug]`: `bIausExplain=1`, `bTaunt=1`, `bStealthProbe=1`, `bSneakInCombat=1`, `bIausHideRoad=1`, `bClassCoSaveSelfTest=1`.
  `bClassTree=0` for the IAUS-only checks (Part 1), then `bClassTree=1` for the class part (Part 2). `bAttackMixProbe=1` for C8.
- `APMF.ini [CombatApproach] bApproachGaitProbe=1`.
- Followers: one Melee, one Mage, one Thief-line, one Healer, all on Sentient AI (Gambits tab button). Keep a save made BEFORE this build (step 1, 22).
- Order below: load/save checks, IAUS checks with the tree OFF, class equip, fights, stealth, flee, save/load round trip, XP rate last.

### PART 1: load/save and the IAUS field fixes (bClassTree=0)
1. **Old save loads under the new build (C12 step 1).** Load the pre-build save with class state. Log: `loaded class state ... (MCLS v1 ...) ... an older record: the next save writes it as v2`, four `MCLS self-test ... PASS` lines, no `FAIL`. Save, load that save: `MCLS v2` and the same follower count. PASS = all of that.
2. **Party-combat "OPEN refused this lap".** Fight with a caster follower whose out-of-combat cast opens a substrate claim while the party is in combat. `MFO.log`: `[substrate] <id> OPEN refused this lap: <detail> -- <spell> (<form>) road=substrate-claim`, at most once per 10 s per (follower, spell, check). PASS = refusal is named and not spammed, no cast fired through the refusal.
3. **Occlusion hold EXPIRED (engine ray).** Let a foe duck behind a wall or pillar while a follower is attacking it. `[iaus] <id>: keep-current-attack hold on <foe> EXPIRED, engine|own ray occluded N.N s (hold N.N s)` once per occlusion; the pick then moves to a visible foe. PASS = the line appears once, `engine` ray named when the engine ray is the one read, he does not stand staring at the wall.
4. **Calm give-up and lift.** A foe that stops fighting (calm) while pinned. `[engage-give-up] <name> (<id>): gives up <foe> (<id>) at Nu now. Harbinger ended the ... as ...`; when the foe fights the party or becomes hostile again: `[engage-give-up] ... is no longer given up, <why>. It may be engaged again.` (MAP.md fix/iaus-field-1 round 2: a CALM give-up is lifted by the first fight with the party or hostility). PASS = give-up on calm, lift on first hostile act, no flip-flop each lap.
5. **Target pin end, then give-up.** Kill a pinned foe. APMF `pin ended`, MFO `[target-pin] <id>: ... ENDED`, then a fresh `[target-pin] <id>: PIN <next foe>` (STATUS feat/mfo-target-pin field checks). If the pin ends while the foe is calm, step 4's give-up line follows. PASS = every pin end is followed by a new PIN or a named give-up, never silence.
6. **ch.21 controller state.** In `APMF.log` after the first fight look for the ch.21 line `controller INACTIVE` (before any engage) and `controller ACTIVE after N ms` once it is up. (Exact wording is Harbinger's; log string not in MFO source. Send the lines if they read differently.) PASS = INACTIVE before, ACTIVE after N ms with N plausible (well under a few seconds), no stuck INACTIVE during a fight.
7. **Orc Hunter pathing.** Fight Orc Hunters / Marauders (the 11-10 evidence file `evidence-iaus-field-2.md` pattern: `pinned, but not in combat (not counted as engaged)` on a ranged Orc). Followers should path to him and close, not stand off. PASS = the `[iaus] pick=attack -> Orc Hunter(...)` pick is followed by the follower reaching melee, no long run of `none fired, first=attack` with `walk=0`.
8. **Selftest blocker naming.** Run the DEV self-test (Combat page rule DO "DEV: run self-tests"). End line: `[selftest] ALL DONE: PASS n, FAIL n, MANUAL n (ids), INFO n` or `ALL RUNNABLE DONE ... | BLOCKED n: <ids>`. PASS = every blocked test is NAMED with its reason (one follower: the ally-buff test needs a 2nd ally), no silent skip.
9. **Q87 multiplier probe (also Part 3 step 20).** With `bStealthProbe=1`: `[stealth-probe] hit ... kSneakAttack=true ... expected multiplier N ... health before B after A drop D` lines exist during the Thief fight in step 19; checked there.
10. **IAUS off means off (older morning list A, optional).** Sentient AI OFF on all, fight: no `[iaus]`, `[iaus-gen]`, `[spellbook]` line.

### PART 2: class equip (bClassTree=1, restart or MCM toggle)
11. **ESL + MCM load (C11).** Load with the ESL: `[classes] carriers: 22 carrier class(es) resolved ...`, `[classes] package styles resolved: 35 of 35 classes, 95 CSTY record(s)`, `[classes] aura values: ... 0 effect(s) patched, 3 aura ability(ies) not in the ESL yet` (EXPECTED, the C11 GAP, not a defect). MCM lists "MFO Classes": 3 toggles + 4 sliders. PASS = those lines, no `package styles:` ERROR line.
12. **Board Classes tab (C10 1-3).** Tab `Classes` after Gambits; absent with `bClassTree=0` or ESL missing. Pad: View/Back reaches the tab, L1/R1 switches follower, d-pad walks, A presses, B closes the confirm popup first and the board second. No class: "No class equipped"; Melee/Ranged/Mage unlocked, T2+ grey with the reason. Equip a T1: row highlights, `[classes] board equip ... defaults`, Gambits-tab `Class:` button greyed with tooltip. PASS = all of that.
13. **Equip a T2 and per-class memory (C10 4, C12 2).** Equip a Knight (cheat unlock or master the T1). Combat table = Knight defaults (taunt, attack, potion: 3 rows at Rank I). Edit a rule, Switch to Warrior and back: the edit is still there. DEV alternative: `sDevClassEquip = <follower>:knight:1, <follower>:warrior:1, <follower>:knight:1` gives `classes OFF -> knight ... DEFAULTS (first equip)`, `knight -> warrior ... DEFAULTS`, `warrior -> knight ... RESTORED`. PASS = those lines / behaviour.
14. **Reset, Classes off (C10 5-6).** "Reset to class defaults": confirm popup opens focused on Cancel; Reset restores the default rows; B or Cancel changes nothing; greyed for tier 3+. "Classes off": class clears, Gambits-tab `Class:` works again, pre-class tables back. PASS = as stated.
15. **Healer defaults (C12 3).** Healer follower (mage): `sDevClassEquip = <follower>:healer:1`. `default combat table laid down ... cast row(s) waiting for a spell pick`, then `default row ... casts <spell>` per known role or `knows no Heal spell ... stays DISABLED`. Board: each cast row shows its spell and is enabled, or is disabled with none. PASS = as stated.
16. **Packages (C8 2-4).** Knight into a fight: `[wstyle] <fid> <name>: combat style OWNED ... (melee stance, ... class package Knight)`; a T1 shows NO "class package"; switch mid-fight: `stance HANDOFF ... class package <new>`. `[mix] <fid>: ch.18 attack mix FILED (h=.., class Knight, power share ceiling 0.25 floor -1.00 ...)`; Warrior `ceiling -1.00 floor 0.40`; Mercenary/Blademaster 0.15; Thief/Master Thief 0.20 ceiling; Listener 0.25; Templar and Archer NO `[mix]` line; probe `share` settles under the ceiling (Knight) / above the floor (Warrior) once `fill` reaches 8. `[leash] <fid>: ch.23 pursuit leash CLAIMED (... radius N)` for a Knight about half an Archer's, never under `fLeashMin`. PASS = all of that.
17. **Unequip, dismiss, no-APMF (C8 5-6).** Unequip: `[mix] ... released (no package class ...)` within a second, next `[wstyle]` has no "class package". Dismiss: `[mix] ... released (dismissed)`. Optional: APMF.dll absent -> no `[mix]`/class `[leash]` lines, class CSTY still applies. PASS = as stated.

### PART 3: fights, stealth, flee
18. **Taunt and power under a class (older list D, abbreviated).** Knight with `bTaunt=1`: `[taunt]` fires on a foe that is on the player or an ally, second taunt inside 30 s logs `[taunt] skip: cooling down`. PASS = as stated.
19. **G2 stealth, Thief-line follower (G2 steps 1-5).** Thief with a melee weapon or bow, `Always -> Sneak` combat rule, two bandits. (a) **P1**: `[stealth-probe] self 0x<id>: sneaking=true ... inCombat=true` once a second; `sneaking=false inCombat=true` every 5 s means the stance does not hold (leave `bSneakInCombat` OFF afterwards). Tail `entries (seed->result ...)` 57/48/47/18 differ from seeds for a perk owner. (b) **P2**: `[stealth-probe] seen foe ... group HELD|LOST|ABSENT detect=N ... MFO verdict: detects me = YES|NO (hidden)`; an unaware foe goes LOST after about 10 s. (c) **Hold row + strike**: `[iaus]` shows `hold 1`, pick `sneak`; unaware foe in reach makes the pick `attack`/`power_attack` with `strike` x1 in `[iaus-why]`. (d) **4b hold claim**: `[hide] <id>: ch.1 movement block (h=..) + ch.7 offense/cast deny (h=..) FILED` once, `RELEASED (...)` with a reason BEFORE the attack on the strike lap; hold lines match the `[iaus]` pick lap for lap; first blow lands on the strike lap; `REFUSED` or `the ch.1 movement block claim ended` = report. Read P1 `moving=` while the hold stands: `moving=false` = ch.1 parks him in combat, `moving=true` with a live claim = it does not (then next step is a ch.23 leash claim, not built). (e) **4c silent casting**: P1 line ends `castSilent=n/m`; with a hidden-casting perk the hold keeps his silent spells as candidates, without one no cast is generated while holding. (f) **P3/Q87**: after 5 sneak + 5 plain hits with one weapon type `[stealth-probe] q87 ...` says `WITHIN 25%` or `OUTSIDE 25%`; no sneak hit at all = the engine's sneak attack does not run for an NPC on this list. PASS = each sub-bullet observed or refuted in the log.
20. **Anchor leak Q92 (Movement batch 1) (needs the G3 + ch.7 merge).** Two followers, one a caster with an out-of-combat buff (Candlelight). Kill the last near foe so the party drops out of combat while more foes are close. `APMF.log`: `HELD HOLDER GROUP JOIN REFUSED`, NO `ANCHOR LEAK` line, the other follower keeps fighting (no `combat ENDED` on him). Then once with `bSubstrateHolderJoinGuard=0`: `ANCHOR LEAK ... his fight is GENUINE`, then `REAL COMBAT (merged-into-a-genuine-fight ...)` for the caster, and still no `combat ENDED` on the other follower. PASS = as stated.
21. **H3 gait probe (Movement batch 2) (needs the G3 + ch.7 merge).** Any ch.24 claim that moves a follower (hide run or give-ground). `[ch.24] 0x... GAIT PROBE (H3, passive): speed N u/s, heading-vs-velocity N deg = FORWARD|STRAFE|BACKPEDAL` once a second plus a `SUMMARY` line. PASS = summary lines captured; mostly FORWARD at 300+ u/s = a real run, mostly STRAFE/BACKPEDAL = he keeps facing his target (facing seat question B#16). Send the summaries.
22. **G3 hide run, then the sneak attack (Movement batch 3) (needs the G3 + ch.7 merge).** Let a foe find the Thief near cover (pillar, rock, wall corner, doorway). `MFO.log`: `[conceal] ... concealment point x,y,z ... hidden from all N foe(s)`, `[iaus]` choosing `disengage`, `[disengage] ... running to ...`, `[disengage] ... ARRIVED` (or the run ends on the way), `[conceal] ... hide run over: hidden ... no foe detects him (G2's verdict)` as soon as the foes lose him, `[sneak] ... the hide hold (card G3) keeps the stance claim`, then `[conceal] ... the blow from stealth is picked: the hide hold's stance is KEPT until the blow resolves`, `[conceal] ... the blow from stealth is under way N ms after the pick ... sneaking now = yes`, and `[conceal] ... the blow from stealth RESOLVED: his hit landed on <foe> N ms after the pick with the stance held (sneak attack flag SET)`. Only after that line may `[sneak]` / Harbinger show the stance released. A miss reads `RESOLVED without a hit: his swing ended ...`. FAIL lines to send back: `sneak attack flag NOT set`, `sneaking now = NO`, `did NOT resolve: the blow's CAP was hit` (6 s with the swing still live), or he stands up before the hit lands. Watch: runs (not walks), stops attacking on the way, run ends the moment foes lose him (not after 12 s), first blow lands as a sneak attack. Cap: `hide run FAILED: the hold CAP (12 s) was hit ...`. Open ground with no cover: `NO concealment point` and no run. Re-detected before the swing starts: `a foe detects him again (revealed)` (while a swing is live the reveal waits for the blow to resolve). PASS = as stated, with he stays crouched through the swing and the flag SET.
23. **G3 flee, non-sneak class, tree ON (Movement batch 3b) (needs the G3 + ch.7 merge, Harbinger ABI 40).** A fighter class that may disengage (kDisengage), `bIausHideRoad=1`. Get him under 30% health with a foe on him and a health potion or heal spell. `[conceal] ... FLEE point ...`, `disengage` chosen, `[disengage] ... running to ... ch.7 deny: Offense (attacks and hostile casts; his heals pass) h=N`, during the run no swings or offense casts, a heal cast or potion as soon as possible (with a heal SPELL: `APMF.log` `[ch.7] hostile-offense: 0x... casts '<heal>' ... NOT Offense, passes the Offense deny`, and the heal lands), then `flee run over: healed past the retreat's health line` (or `arrived at the point away from the foes`). Tree OFF: he must never take this flee. PASS = as stated.
23b. **FIELD WATCH: heals denied as UNDECLARED (review P1-1) (needs the G3 + ch.7 merge, Harbinger ABI 40).** During steps 19 (the G2 hold with a heal claim: `[hide] ... the ch.7 deny is refiled as Offense ALONE`) and 23 (the flee), search `APMF.log` for `[ch.7] hostile-offense: 0x... casts '<spell>' ... nature UNDECLARED by its record ... counted as HOSTILE, the Offense deny stands`. On LoreRim many heals from spell mods carry a script effect, and Harbinger denies those by rule (only heals on its beneficial allow-list pass; open question 91). Each such line during a hold or a flee is a heal that did NOT land. Send every UNDECLARED line with the spell name and whether the follower stayed unhealed. No line and the heal landed = the allow-list covered that spell. This is a watch item, not a pass / fail step.
24. **G3 failures loud, no hitch (Movement batch 4-5) (needs the G3 + ch.7 merge).** If he never arrives: `[conceal] ... hide run FAILED: <why>` / `flee run FAILED` (no path, outside the leash, another ch.24 claim, 10 s). 25 s cooldown counts from the END of the run. While the Thief is attacked the pick runs about every 1.5 s: watch the Deck frame time. PASS = every failure carries a line, no hitch.

### PART 3b: class aversion, "less likely to be targeted" (card H2; needs the `feat/aversion` + `feat/class-aversion` merge, Harbinger ABI 41)
Class field tests run on the DEV DLL with `[Debug] sDevClassEquip` (marth: "it will need to be tested with the dev gambit since i wont have the classes unlocked"). Setup for these steps: DEV `MFO.dll`, `MFO_Classes.esl` on, `bClassTree=1`, `bClassAversion=1` (default), and in `MFO.ini [Debug]` ONE line naming a Conjurer test follower and a Healer test follower (replace each `Plugin.esp|0XXXXX` with that follower's actor ref):
`sDevClassEquip = Plugin.esp|0AAAAA:conjurer:1, Plugin.esp|0BBBBB:healer:1`
Bring a third follower with NO class or a fighter class (the "other target"). A release DLL ignores the key.
24a. **Seats and claims.** Load. `APMF.log`: `[ch.32] aversion seats installed on <runtime> (kIntent_Aversion, ABI v41) ...`. `MFO.log` within a second of `[classes] DEV equip ... -> conjurer L1` / `-> healer L1`: `[aversion] <fid>: ch.32 aversion claim FILED (h=N, weight 0.50 ...)` once per test follower, and `APMF.log` `[ch.32] 0x<fid> aversion ENGAGED: weight 0.50`. PASS = both followers filed, none for the third follower. `seats NOT installed -- <why>` or `Harbinger REFUSED` = send the line.
24b. **Foes look elsewhere, half the time.** Fight three or more melee bandits with the player and the three followers close together for at least a minute. `APMF.log`: lines `[ch.32] aversion: foe 0x.. selected 0x<conjurer or healer> (weight 0.50, h=N) -- AVERTED for this 6 s window: the engine's next best valid target 0x.. is answered instead` and `-- KEPT by the draw for this 6 s window`, each at most once per 10 s per pair, and about every 30 s `[ch.32] aversion H ... landed=N averted=N kept-by-draw=N kept-no-other-target=N kept-unfit=N ...`. PASS = `landed` > 0 and `averted` and `kept-by-draw` are both > 0. The weight is NOT a pass bar in either direction: the share of time a foe spends off the claimant can be under 0.50 (the game keeps its current target 1 to 3 s into an averted window, and an averted window keeps the claimant when no other usable target exists) or over it (the game's own bonus for its current target). Do not fail the step on `averted` being lower than `kept-by-draw` + the other kept totals; send the last `aversion H` line and the numbers are read from it. By eye: the Conjurer and the Healer are attacked less than the third follower, and are still attacked sometimes. A line `... the engine's next best target 0x.. is not answered: <which test> -- the pick STANDS` (review H2-1: the next best was out of the foe's combat area, unpathable, dead or fleeing) is correct behaviour; send those lines too, they are the first field sight of that test. `landed=0` after a minute with foes on them = send both logs (the seat is not being reached).
24c. **Never untargetable.** The Healer must be the only target the bandits DETECT. Walking the others away is not enough: a bandit that has seen them keeps them among its group's targets, so the game still scores them. Set it up so the others were never seen or are lost: leave the player and the other followers in ANOTHER CELL (outside the door of an interior, or told to wait before the bandits are in sight) or have the player sneak undetected and far back, and let only the Healer walk into the bandits. He is attacked as normal. `APMF.log`: `... averted by the draw, but the engine scored NO OTHER valid target -- the pick stands`, and `kept-no-other-target` rises. PASS = he is attacked and that line appears. FAIL = the bandit stands idle or runs off to search for somebody it cannot see.
    If the others WERE detected and then moved away (the easy setup), expect the other outcome instead, and it is also a pass: either `AVERTED ... 0x<other> is answered instead` while that other is still reachable (the bandit goes for him), or, once he is out of the bandit's combat area or in another cell it cannot path to, `... the engine's next best target 0x<other> is not answered: it is outside the foe's combat area ...` (or `... cannot path to ...`) `-- the pick STANDS` with `kept-unfit` rising, and the Healer is attacked. FAIL in that setup = the bandit leaves the Healer for a target it cannot reach and stands idle.
24d. **A taunt still wins.** With a Knight follower and `bTaunt=1`: taunt a foe that an averted follower's window would send elsewhere. The foe goes to the taunter (`[taunt]` + ch.20 pin lines as in step 18). PASS = the pin holds for its 15 s.
24e. **Release roads.** (1) Board "Classes off" on the Conjurer (or `bClassAversion=0` and reload): `[aversion] <fid>: ch.32 aversion claim released (no Conjurer / Healer passive ...)` within a second and APMF `[ch.32] 0x<fid> aversion released`. (2) Dismiss the Healer: `released (dismissed)`. (3) Let one die (a mortal follower): `released (he is dead)` or, if MFO dismisses him first, `released (dismissed)`. (4) Save and load: the claims are filed again (`FILED`), none survive from before. (5) Optional, once, with an older `APMF.dll` (ABI 40 or lower): ONE `[aversion] Harbinger ABI N is older than 41 ...` line and no claim. PASS = each release has its line and no `[ch.32] aversion:` line names a released follower afterwards.
24f. **Mastered quarter (optional).** `sDevClassEquip = Plugin.esp|0AAAAA:conjurer:15:mastered, Plugin.esp|0AAAAA:evoker:1`: the claim is filed with `weight 0.12` or `0.13` (a quarter of 0.50). PASS = that weight.

### PART 3c: raise the corpse the rule chose (card G7; needs the `feat/ch8b-dead-target` + `feat/g7-reanimate` merge, Harbinger ABI 42)
Setup: DEV `MFO.dll`, the new `APMF.dll`, `MFO_Classes.esl` on, `bClassTree=1`, and in `MFO.ini [Debug]` one line naming a mage test follower (replace `Plugin.esp|0AAAAA` with his actor base, as in Part 3b):
`sDevClassEquip = Plugin.esp|0AAAAA:conjurer:1`
The gambit itself needs no class. The Conjurer equip is there so a strict class tree lets him cast Conjuration. He must KNOW a Reanimate spell: click him in the console, `help "raise zombie" 4` (or `help reanimate 4`) to get the spell's id on this list, then `addspell <id>`. On his board, Combat table, add a row at the TOP: WHEN `Corpse: Within` 1500, DO `Cast at target` with that spell, target left on Auto.
How to get a corpse he can raise: fight a group of three or more weak humans (bandits at a low-level camp). The first one to die is the corpse. The fight must still be going (one foe alive), because he casts through his combat AI. A wolf or a skeleton does not count (humanoid corpses only, question 103).
24g. **Seat installed.** Load. `APMF.log`: `[ch.8b reanimate] Reanimate corpse seat installed: 0x06 CheckStartCast and 0x0D SetupAimController ...`. PASS = the line, and no `[selfcheck] ... REFUSED` line naming `ReanimateSeat`.
24h. **He raises the corpse.** In the fight, after the first bandit dies with another still alive: he raises his left hand, casts, and the dead bandit stands up and fights for you. `MFO.log`: `[buff] <fid> <spell> (<id>): ... CLAIM at <corpse id> (the rule's corpse, kCastFlag_CorpseTarget) ...`. `APMF.log`: `[ch.8b reanimate] FIRST call of the Reanimate caster's seat 0x06` (and `0x0D`), `[ch.8b reanimate 0x06] ... CheckStartCast -> YES ... the engine's own keep test accepted the claim's corpse`, `[ch.8b reanimate 0x0D] ... aim -> claimed corpse 0x<corpse> ...; the engine's own search had picked handle 0x...`. PASS = the corpse named in the MFO line is the one that stands up. Send back the 0x0D line: it shows whether the game's own pick was a different corpse. If `MFO.log` shows `[buff] <fid> <spell> ...: ... -- DIRECT road, unanimated` for the spell instead of a CLAIM line, send that line back and try another Reanimate spell (MFO-B770).
24i. **A corpse the spell is too weak for is skipped.** Same rule, but the only corpse in range is well above the spell's level (a bandit chief for Raise Zombie). `MFO.log`: `[willwork] caster <fid> spell <id> -> target <corpse>: WILL NOT WORK -- '...' affects up to level N, target is level M`. He does not cast and his other rules run. With a weak corpse AND a strong one in range he raises the weak one. PASS = no cast at the strong corpse, no `[buff] ... NEVER FIRED` line for it.
24j. **One thrall at a time (no Twin Souls).** With his thrall up and a second weak corpse in range he does not cast again (the game would destroy the first thrall). `MFO.log` shows no second `CLAIM at` line while the thrall lives. Kill the thrall (or let its time run out): he raises the next corpse. PASS = as stated.
24k. **The game refuses, loudly.** Only if it happens: `APMF.log` `[ch.8b reanimate 0x06] ... -> NO ...: the engine's own keep test REFUSED the claimed corpse 0x...`, then about 6 s later `MFO.log` `[buff] ... at <corpse> NEVER FIRED this fight ... RELEASED`. He then leaves that corpse alone for the fight and takes another if there is one. Send both lines back: it means MFO's test and the game's test disagree about that corpse (MFO-B766).
24l. **Off switch and old Harbinger.** (1) `APMF.ini [CastSeats] EnableReanimateSeat=0`, restart: `APMF.log` `[ch.8b reanimate] disabled by [CastSeats] EnableReanimateSeat=0 ...`, and the first raise attempt logs `[apmf] <fid> Harbinger WOULD REFUSE the buff cast claim (ABI v35 dry run) ... [CastSeats] EnableReanimateSeat=0` in `MFO.log` (MFO asks first, so nothing is filed). Nothing is cast and his other rules run. Set it back to 1. (2) Optional, with the PREVIOUS `APMF.dll` (ABI 41): the `[buff] ... CLAIM at <follower id>` line has no "the rule's corpse" text and he raises whichever corpse the game picks. PASS = as stated.

### PART 3d: melee moves (T2 batch M; needs the `feat/melee-moves` merge in both repos, Harbinger ABI 43+)
Setup: DEV `MFO.dll`, the new `APMF.dll`, `MFO_Classes.esl` on, `bClassTree=1`, and in `MFO.ini [Debug]` one line per test follower (replace `Plugin.esp|0AAAAA` with his actor base, as in Part 3b). Each step names its own line.
24m. **Block: the Knight holds his guard.** `sDevClassEquip = Plugin.esp|0AAAAA:knight:1`. Give him a one-handed weapon and a shield. Sentient AI OFF for him on the board (MFO-B800). Load: `APMF.log` `[ch.31] block hold deny ARMED: guard leaves Block paired / BlockAttack paired ...` and `[combat-move] BLOCK (ABI v43, kCombatMove_Block) available ...`. His Combat table has `Foe: Targeting me (melee) -> Block` with a 4.0 s cooldown cell. Fight one melee bandit and let it swing at HIM. He raises his shield and keeps it up for about 2.5 seconds without swinging, then fights for about 1.5 seconds, then raises it again while the bandit still goes for him. `MFO.log`: `[block] <fid>: guard UP for 2.5 s (h=N)` then `[block] <fid>: guard DOWN (h=N): the window ran out; blocked a hit: yes`. `APMF.log`: `[combat-move] N STARTED: actor ... BLOCK held for 2.50 s (Left Attack action, event 'blockStart') ...` and `... block ENDED: ... window over ...`. PASS = the guard visibly stays up for the window, at least one `blocked a hit: yes`, and no swing of his inside a window. Send back the `[combat-move] H block holds:` line (it counts the leaves failed and the re-raises).
24n. **Block without a shield, and with nothing to block with.** Same follower. (1) Take the shield away (one-handed sword, empty left hand): he still blocks, with the sword. (2) Give him a bow only: no `[block]` line at all, the board row reads `block: a bow, a crossbow, a staff or bare fists ...` and his other rows run. (3) A spell in his left hand: the row reads `block: his left hand holds a weapon, a spell or a staff ...`. (4) The SAME one-handed weapon in both hands (two iron swords): the row reads `block: the same one-handed weapon in both hands (dual wield: no block from those hands)` and he never swings his left weapon as a "block". PASS = as stated, and no `[combat-move] ... REFUSED` flood in `APMF.log` (MFO does not ask).
24o. **Block: the guard survives a stagger, and ends when stamina runs out.** Let a two-handed bandit power attack into his guard. If the hit staggers him, `APMF.log` shows `[combat-move] N block RE-RAISED: ... the engine dropped his guard inside the window ...` and the guard comes back up. Drain his stamina (console `damageav stamina 500` on him) while he holds: `APMF.log` `... block ENDED early: ... stamina at 0: the engine's own block rule dropped the guard ...`, `MFO.log` `guard DOWN ... it could not be kept ...`. PASS = both lines when the situation happens. Send back any `block re-raise REFUSED` or `FAILED ... want-block state did not set` line.
24p. **Block: off switches and old Harbinger.** (1) `APMF.ini [CombatMove] bBlock=0`, restart: `APMF.log` `[combat-move] BLOCK (ABI v43, kCombatMove_Block) NOT available -- [CombatMove] bBlock=0 ...`, `MFO.log` ONE line per load `[combat-move] Harbinger does NOT serve the block (kCombatMove_Block) on this game: [CombatMove] bBlock=0. The Block gambit is unavailable ...`, the board row reads `no block road: Harbinger does not serve the block on this game ...`, no guard, his other rows run. Set it back to 1. (2) Dismiss him mid-guard: `MFO.log` `[block] <fid>: guard lowered (dismissed): block end requested ...` and the shield comes down at once. (3) Optional, with the PREVIOUS `APMF.dll` (ABI 42): the row reads `no block road: Harbinger absent or older than ABI 43`. PASS = as stated.

24q. **Power attack: the real one (this is the Q82 probe).** `sDevClassEquip = Plugin.esp|0AAAAA:warrior:1`, a one-handed or two-handed weapon, Sentient AI either way. `APMF.ini [Probe] bCombatMove=1` so every melee hit is logged with its flags. Fight a bandit that blocks (or one stronger than him): his row `Foe: Blocking -> Power attack` fires at most every 10 s. `MFO.log`: `[power-attack] <fid>: road=harbinger (ch.31 kCombatMove_PowerAttack, h=N) -> ...`, then `[combat-move] <fid>: power attack h=N STARTED on ... (stamina spent S)` and `... HIT ...`. `APMF.log`: `[combat-move] N STARTED: actor ... power attack (event 'attackPowerStart...', ...; his graph refused first: ...)` and `[combat-move] N HIT: actor ...'s power attack landed on ...`. PASS = he visibly power attacks, `stamina spent` is above 0, and the `[Probe]` hit line for that swing carries the PowerAttack flag. Send back the STARTED line (it names the entry that ran and the ones his graph refused).
24r. **Power attack: the old road, for the comparison.** `MFO.ini [Debug] bPowerAttackEngineRoad = 0`, restart, same fight. `MFO.log`: `[power-attack] <fid>: road=graph (animation event attackPowerStartInPlace; bPowerAttackEngineRoad=0) ...`. Send back the `APMF.log` `[Probe]` hit lines of these swings: whether they carry the PowerAttack flag is the answer to Q82. Set it back to 1. PASS = the road line, and the hit lines sent back.

24s. **Warrior power floor (ch.18, already built; batch M item 3 found no code was needed).** `sDevClassEquip = Plugin.esp|0AAAAA:warrior:1`. Fight one tough foe for two minutes with his `Power attack` rows turned OFF on the board, so only the game's own attack choice shows. `MFO.log`: `[mix] <fid>: ch.18 attack mix FILED (h=N, class Warrior, power share ceiling -1.00 floor 0.40; -1 = none)`, and the `[mix] <fid> (Warrior) state ...` line with the measured share. `APMF.log`: `[ch.18] 0x<fid> '...' claim h=N: active deny now ...` naming Normal while his share is under the floor. PASS = his measured power share sits near or above 0.40 after the first 20 attacks, and he never stands without attacking (the floor lifts itself when it would leave no attack). Send back the last mix state line.

24t. **Combat style: the class style really applies.** `sDevClassEquip = Plugin.esp|0AAAAA:knight:1, Plugin.esp|0BBBBB:mercenary:1` (two followers, both with a one-handed weapon; the Knight with a shield). Load: `APMF.log` `[ch.33] combat style seats installed on <runtime> (kIntent_CombatStyle, ABI v45) ...`. Start a fight. `MFO.log`: `[wstyle] <fid>: ch.33 combat style claim FILED (h=N, style <id>, actor-wide (class style))` for each. `APMF.log`: `[ch.33] 0x<fid> '...' combat style ENGAGED: style 0x<id> '...', scope ACTOR-WIDE ...`, `... is LIVE ...`, `the engine refreshed his controller's cached combat style (claim live): 0x<old> -> 0x<id>`, and once per session `[ch.33] FIRST answer of seat A ...` and `FIRST answer of seat B ...`. PASS = those lines, and the two visibly fight differently from each other over a two-minute fight (the Knight stands and blocks, the Mercenary moves more). Send back the `[ch.33] H combat style` line and one sentence on what you saw each of them do.
24u. **Combat style: no class, off switch, release.** (1) A follower with NO class and a `Foe: Any -> Equip melee weapon` row: `MFO.log` `[wstyle] <fid>: ch.33 combat style claim FILED (h=N, style <MFO_MeleeStyle id>, equip choice only)`; he draws and keeps his melee weapon as before. (2) End the fight: `[wstyle] <fid>: ch.33 combat style claim released (no stance owns his style, ...)`, `APMF.log` `[ch.33] ... combat style released ...`. (3) `APMF.ini [CombatStyle] bCombatStyle=0`, restart, fight: `APMF.log` `[ch.33] combat style seats NOT installed -- [CombatStyle] bCombatStyle=0 ...`, `MFO.log` `[wstyle] <fid>: Harbinger REFUSED the ch.33 combat style claim at the call ...` about every 30 s, and NO `[wstyle] ... combat style OWNED` line (MFO does not write the field itself). Set it back to 1. (4) Optional, with the PREVIOUS `APMF.dll` (ABI 44 or older): `[wstyle] ... combat style OWNED ...` appears as before this change. PASS = as stated.

24v. **Block: he does nothing else while the guard is up, and the guard always ends (fix round).** The Knight of 24m, with a `Foe: Any -> Attack` row and a healing spell row below the Block row. While `[block] <fid>: guard UP` stands, the board shows those rows as `holding his guard: no attack, cast or weapon change until it ends` and `MFO.log` has no cast claim and no `[power-attack]` line for him. Then set the Block row's cooldown to 0 on the board: he guards for two windows (about 5 s), `MFO.log` `[block] <fid>: guard DOWN ...; renewed 1 time(s) (at most 1); no new guard for 1500 ms`, and for about 1.5 s the row reads `block: resting after a guard` and he attacks or heals. A NEW Block row added on the board shows a 4.0 s cooldown by itself. PASS = as stated. Set the cooldown back to 4.
24w. **Power attack: a refused one gives its cooldown back (fix round).** The Warrior of 24q with his stamina drained (console `damageav stamina 500`). `MFO.log`: `[combat-move] <fid>: power attack h=N FAILED: the engine refused the action ...` followed by `[combat-move] <fid>: power attack h=N did not happen: its cooldown is given back`, and the row tries again within a second or two instead of after 10 s. PASS = both lines. (He will keep trying while he has no stamina: that is the game refusing, logged each time.)
24x. **Mercenary side step (Q-melee-7).** `sDevClassEquip = Plugin.esp|0AAAAA:mercenary:1`, the regenerated `MFO_Classes.esl`. On a list with NO dodge mod (Tuxborn Deck): fight two archers or a mage for two minutes and watch whether he steps sideways out of incoming arrows and spells more than a Knight does. `APMF.log` must show `[ch.33] ... combat style ENGAGED ... scope ACTOR-WIDE` and `is LIVE` for him (his style's Avoid Threat is 0.3). On LoreRim (Ultimate NPC Dodging): the same fight; note whether he dodge-rolls. PASS = a visible difference on at least one list. Send back one sentence for each list: this is the open half of MFO-B806.
24y. **1.7.104 only (if that game version is ever on a test list).** Load: `MFO.log` one line each `[combat-move] Harbinger does NOT serve the block ...` and `... the power attack (kCombatMove_PowerAttack) on this game: ...`. The Block row is skipped; `Power attack` logs `[power-attack] ... road=graph (animation event attackPowerStartInPlace; Harbinger does not serve combat moves on this game)`. PASS = as stated, and no repeating refusal lines.

### PART 4: save/load round trip, then XP rate last
25. **MCLS round trip (C12 4-5, C10 7, C8 7).** With classes and edited tables in place save and load: the class, levels, tables and stored tables are the same (`saved class state ... N combat + N logistics + 2 classes-OFF stored table(s)` on save, the same counts on load); C8 claims re-file after load (`FILED` again, none survive from before). Then `bClassTree=0`, load, save, `bClassTree=1`, load: class and every stored table unchanged. Self-tests: no `MCLS self-test ... FAIL`. PASS = as stated. DO NOT load a v2 save on an older MFO and save.
26. **MCM toggles (C11 1-5).** `Class XP rate` to 5, close the MCM: the next fight's `[classes] XP` line shows `rate 5.00`. `Class tree` OFF: the Classes tab disappears; ON: it returns with progress intact. `Priest sun` to 6, close: `[classes] aura values: Priest sun ... 6/s ...` (still 0 effects patched, the GAP). `Data/MCM/Settings/MFO_Classes.ini` holds the changed keys. PASS = as stated.
27. **XP rate (C4 1-6), LAST.** With `fClassXpRate = 5`: (1) kill a bandit with the follower beside you; about 1 s after the party leaves combat one `[classes] XP <fid> <slug>: +N (fight won: ... kill/assist) -> xp / total level L [(LEVEL UP ...)]` line per follower, the Classes tab xp rises, a second fighter gets his own line. (2) A fight with no kill (flee, non-hostile wildlife): no XP line. (3) Healer who heals you in a won fight: `heal` count in the line; Conjurer with a summon up at the end: `summon N`. (4) Take a class to its cap: `[classes] ... mastered <slug> (level N)` once, unlocks appear, no more XP lines for that class. (5) Unequip: no XP after the next fight; save/load keeps xp and level. (6) `fClassXpRate = 0`: no XP at all. Also judge the PACE: how many fights to a level, to compare with the ~10 fights/hour assumption (rate tuning is the point of the field run). PASS = as stated, plus the pace note.

### SEND BACK
`MFO.log`, `APMF.log`, any crash log, the step numbers skipped, the last `[ch.32] aversion H` line (24b), the gait SUMMARY lines (21), the q87 line (19f), the `[ch.8b reanimate 0x0D]` line (24h), the pace note (27).
### Gaps found while collecting (not invented, for the coordinator)
- No file on main lists the "IAUS field-fix" checks as steps. Steps 2-8 were rebuilt from the log strings in code (`cast/SubstrateRoad.cpp:87`, `iaus/Candidates.cpp:73`, `apmf/CombatEntry.cpp:333,419`), STATUS (target-pin) and the 11-10 evidence file. The ch.21 "controller INACTIVE / ACTIVE after N ms" wording is Harbinger's and is not in MFO source (step 6).
- The "Movement batch" section lives only on `feat/g3-disengage` (not main); its steps are copied into 20-24 with the merge caveat.

---

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

## Movement batch (Harbinger `feat/h3-disengage` + MFO `feat/g3-disengage`, pending review; g3-disengage.md)

Setup: both DLLs from the two branches. Harbinger `APMF.ini`: `[CombatApproach] bApproachGaitProbe=1` (default),
`[CombatSubstrate] bSubstrateHolderJoinGuard=1` (default). MFO.ini `[Debug]`: `bIausHideRoad=1`, `bSneakInCombat=1`,
and for step 2 `bIausGroundRoad=1`. Class tree ON, one follower with a Thief-line class equipped and Sentient AI ON.

1. **Anchor leak Q92.** Fight a group with two followers, one of them a caster with an out-of-combat buff (Candlelight).
   Kill the last near foe so the party drops out of combat for a moment while more foes are close.
   Expect in `APMF.log`: `HELD HOLDER GROUP JOIN REFUSED` when the engine tries to pull the caster into the other
   follower's fight, NO `ANCHOR LEAK` line, and the other follower keeps fighting (no `combat ENDED` on him).
   Then once with `bSubstrateHolderJoinGuard=0`: `ANCHOR LEAK ... his fight is GENUINE`, then
   `REAL COMBAT (merged-into-a-genuine-fight ...)` for the caster, and still no `combat ENDED` on the other follower.
2. **H3 gait (also "class H3" above).** Any ch.24 claim that moves a follower: the give-ground step, or the hide run.
   Expect `[ch.24] 0x... GAIT PROBE (H3, passive): speed N u/s, heading-vs-velocity N deg = FORWARD|STRAFE|BACKPEDAL`
   once per second and a `SUMMARY` line at the end. Send the summary lines. Mostly FORWARD at 300+ u/s = a real run.
   Mostly STRAFE or BACKPEDAL = he keeps facing his target (the facing seat question, B#16).
3. **G3 hide run (sneak class), then the sneak attack.** Let a foe find the Thief near cover (a pillar, a rock, a wall corner,
   a doorway). Expect in `MFO.log`: `[conceal] ... concealment point x,y,z ... hidden from all N foe(s)`, then `[iaus]` choosing
   `disengage`, `[disengage] ... running to ...`, `[disengage] ... ARRIVED` (or the run ends on the way), and
   `[conceal] ... hide run over: hidden ... no foe detects him (G2's verdict)` as soon as the foes lose him, with
   `[sneak] ... the hide hold (card G3) keeps the stance claim`. Then G2's hold row keeps him still and the blow from stealth
   follows. The stance is KEPT through the blow (fix of review P1-G3: the engine reads the stance when the hit lands):
   `[conceal] ... the blow from stealth is picked: the hide hold's stance is KEPT until the blow resolves`, then
   `[conceal] ... the blow from stealth is under way N ms after the pick (attack state N, own cast ...) ... sneaking now = yes`,
   then `[conceal] ... the blow from stealth RESOLVED: his hit landed on <foe> N ms after the pick with the stance held (sneak
   attack flag SET)`. He must stay crouched until that line. A miss reads `RESOLVED without a hit: his swing ended with no hit
   event` (a shot or a cast waits 1.5 s more for its projectile). Send back any `sneak attack flag NOT set`, any
   `sneaking now = NO`, and any `did NOT resolve: the blow's CAP was hit` (6 s with the swing still live). Watch: does he run (not walk),
   does he stop attacking on the way, does the run end the moment the foes lose him (not after 12 s), does the first blow
   land as a sneak attack. If the cap is hit expect `hide run FAILED: the hold CAP (12 s) was hit ...`. In the open with no
   cover expect `NO concealment point` and no run. A foe that detects him again before his swing starts releases the stance
   (`a foe detects him again (revealed)`); once the swing is live the reveal waits until the blow resolves.
3b. **G3 flee (non-sneak class, tree ON).** A fighter class that may disengage (kDisengage), `bIausHideRoad=1`. Get him under 30%
   health with a foe on him and a health potion or a heal spell. Expect `[conceal] ... FLEE point ...`, `disengage` chosen,
   `[disengage] ... running to ...`, and during the run no swings or offense casts, a heal cast or a potion drunk as soon as
   possible, then `flee run over: healed past the retreat's health line` (or `arrived at the point away from the foes`).
   With the class tree OFF he must never take this flee.
3c. **ch.7 Offense means hostile (Harbinger ABI 40, the flee's heals).** Same setup as 3b, the follower has a heal SPELL and
   no potion. Startup, `APMF.log`: `[ch.7] hostile-offense (ABI v40) on 1.6.1170: CastImmediateSpell hostile-only,
   CastConcentrationSpell hostile-only, PrepareDualCast hostile-only, CastShout hostile-only` (a `REFUSED` there = send the
   line). During the flee, `MFO.log`: `[disengage] ... ch.7 deny: Offense (attacks and hostile casts; his heals pass) h=N`.
   `APMF.log`: `[ch.7] hostile-offense: 0x... casts '<heal spell>' (0x...), declared non-hostile -- NOT Offense, passes the
   Offense deny (h=N)`, and about every 30 s `[ch.7] hostile-offense H ... passed-beneficial=N denied-hostile=N ...`.
   Watch: the heal animates and his health goes up while he runs, and he casts no offense spell and swings at nobody.
   PASS = at least one heal lands during the flee and `denied-hostile` counts any offense cast the engine tried.
   A spell line with `nature UNDECLARED ... counted as HOSTILE` names a script-only spell: send it.
   Once with an old Harbinger DLL (ABI 39): `[disengage] Harbinger ABI 39 is older than 40 ... A flee files NO ch.7 deny`
   once, and the start line says `ch.7 deny: NOT FILED`.
3d. **Hide run and G2 hold keep every cast denied.** In step 3 the start line says `ch.7 deny: Offense|Cast (attacks and
   every cast) h=N` and he casts nothing on the way. The run now ends after TWO laps in a row with no foe detecting him.
   With a silent caster (Quiet Casting) who holds hidden and picks an OFFENSE spell: `[hide] ... ch.7 deny STANDS DOWN whole
   while his own offense cast claim is delivered`, the cast lands, then `... the ch.7 offense/cast deny is filed again`.
   When the pick is a HEAL: `[hide] ... his heal claim is being delivered: the ch.7 deny is refiled as Offense ALONE`, the
   heal lands, he does NOT swing or shoot meanwhile, then `... filed again`. FAIL = an attack during the heal, or APMF.log
   `nature UNDECLARED ... counted as HOSTILE` naming the heal (send the spell name: its record has a script effect).
4. **G3 failures are loud.** If he never arrives: `[conceal] ... hide run FAILED: <why>` / `flee run FAILED` (no path, outside the
   leash, another ch.24 claim, 10 s). Send the line. The 25 s cooldown counts from the END of the run.
5. **No hitch.** While a Thief is being attacked the pick runs about every 1.5 s. Watch the frame time on the Deck.

