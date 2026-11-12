# Open questions for marth

**ALL 28 ANSWERED 2026-11-09 (interactive). Answers: class-walkthrough-decisions.md "Question batch answers" / ClickUp 86e3dhbd3.** New questions go below this line.

---
## ANSWERED (archive)
Each: question, context, my recommended default (work proceeds on the default where possible).

1. Class passives: does the 1/4-when-mastered rule also apply to the T3 two-effect passives? Default: yes.
2. Knight passive: is it really ONLY "blocking stops 15% more damage" (Heavy Armor + One-Handed effects dropped)? Default: yes.
3. Names for every T3/T4 passive (only Shieldwall, Spellstrike named). Default: the class-prep agent proposes lore names; marth picks later.
4. MFO-B560: switch board text input from ANSI to the W (UTF-16) API? Default: leave open.
5. Harbinger v0.17.0 tag is lightweight; annotating needs a force-push of the tag (needs marth's approval). Default: leave as is.

## From the class build plan (2026-11-09; work proceeds on the default)
6. M1 scope: ship tiers 1-2 with four T2 effects missing (Archer miss correction H5, Conjurer/Healer less-targeted H2, Thief instant hiding G2, Thief/mage disengage H3), each logged as missing, landing in M2? Default: yes (sequenced, not cut).
7. Per-rule gambit cooldown: a cooldown cell on EVERY gambit row (0 = none), or power-attack rules only? Default: every row (one FLWR v6 save bump either way).
8. Class default gambits on a switch: apply only the first time a class is equipped, plus a "reset to class defaults" button? Default: yes, never overwrite player edits silently.
9. Class progress when the add-on ESL is removed for a session: keep it? Default: keep.
10. Go-in rules: Witchhunter never goes in; Scout uses the Templar rule (party >= 70%), else ranged, melee only when out of ammo? Default: yes.
11. Living Legend auto-block: block sound/spark only, no forced block animation? Default: yes.
12. Shadowcloak = the vanilla Invisibility power (follower visibly cloaked 3 s)? Default: yes. (Items 1-3 above cover the other walkthrough follow-ups.)

## From the HMS / magicka research (2026-11-09)
13. Magicka cost: the engine gives NPCs a cheaper cost curve (29-50% of the player's at skill 50-100). Should managed followers pay the PLAYER's curve? Default: no change until you decide; if yes it is a tier-A Harbinger hook on the cost curve (86e3d6dp0). Also check LoreRim's own Requiem cost perks first.
14. CSTY kAllowDualWielding is global on MFO_MeleeStyle (86e3940yv): with the class tree OFF, should an unperked follower be kept from AI dual-wielding (needs a per-follower CSTY)? With the tree ON, class CSTYs (card C8) settle it. Default: leave as today.
15. Harbinger inactive winner (86e3t1gfn): the code already lets the next source through except (a) the combat substrate refuses to open when the Engine's slider outranks the client (deliberate deny-completeness), and (b) a foreign hook module beneath a seat is credited even when it only passed the engine answer through. Which scenario did you see? Default: wait for a field log naming the credited source.

## From the class M0 engine research (2026-11-09, class-m0-re.md attached to 86e3dhbd3; none blocking)
16. H3 disengage: if the engine's run turns out to be a strafe/backpedal, is a facing seat in scope for M2? Default: yes, decided after the heading probe.
17. H2 less-targeted: ship 1.6.1170 / 1.7.104 first with 1.5.97 refused by name (its check is inlined, needs its own research)? Default: yes.
18. G2 hide-when-unseen: also seat the combat group's low-process detection pass, or declare it a known gap? Default: known gap first, seat later if the field shows it.
19. H5 archer misses: if the engine's aim does not lead moving targets, is a lead input in scope? Default: yes, after the hit/miss probe.
20. Retreat (86e3kcvfk): when a follower reaches you at full HP while the engine's own Flee is still running, should MFO release the hold (today, per spec) or keep holding until the engine Flee ends? Default: unchanged until the Harbinger flee deny exists.
21. Skooma (86e3t1gfq): MFO now keeps drugs off its combat allow-list, which only covers followers with a cast gambit; a complete fix is a Harbinger potion allowance (filed). Accept the narrower MFO fix now? Default: yes, merged with the limit stated in code; Harbinger item filed.

## From classes C1+C2 (2026-11-09; encoded as defaults in feat/classes-c1-c2, G16 will enforce them)
22. Mage-line classes may hold a dagger + a STAFF (staff was not in your rulings). Default: allowed.
23. Thief line: dagger + bow, NO crossbow. Default: as stated.
24. Hybrids' root T1 (drives Auto stance) is the WEAPON side (Sorcerer/Templar = Melee; Scout/Witchhunter = Ranged). Default: as stated.
25. Unlocks: T2 = own T1 + one other T1 mastered; pure T3 = own T2 + one other T2; Wizard needs both Evoker AND Warlock. Default: as stated.
26. Enemy-control gambit target: "paralyze/fear THE stronger foe" — today AUTO refuses spells whose nature the record doesn't declare (c62d4cf) and Enemy means "the commanded target" only. Should control spells (paralyze, fear, calm, turn undead) on a condition-picked foe target that foe? Default: class defaults use AUTO, marked pending, not applied until ruled; likely needs a "selected foe" target subject in base gambits.
27. Do draft ch.18 caps stand where the walkthrough was silent (Thief 0.2, Master Thief 0.2, Templar 0.25)? Default: not encoded until ruled.
28. T1 classes: should a T1's default gambit set be applied at all with the tree ON, given "T1 behaves vanilla"? Default: no (keep today's default kit).

## ANSWERED 2026-11-09 batch 2 (29-38): see class-walkthrough-decisions.md "Question batch 2 answers"

## New (saved 2026-11-09, not asked)
29. Per-class gambit memory (your Q8): the class save stores each class's COMBAT table only. Should the Logistics table also be remembered per class? Default: combat only now; logistics can be added later without breaking saves.
30. Bound bows (86e3t1gfx): the engine never builds an animated combat caster for bound BOWS, so the only working road today is the direct, UNANIMATED cast. Your standing rule is "animations required for ALL actions". Accept unanimated bound bows as a named exception, or wait for Harbinger to make the engine build a bow caster (animated)? Default: follow the standing rule. The MFO direct-road branch is parked (not merged) and the Harbinger task is filed.

## From the IAUS design (2026-11-09, iaus-design.md attached to 1dfcuabcr1d; numbered 31-38)

31. **Per-follower default.** Recommend OFF for every follower (their tables keep running). IAUS is turned on per
   follower on the board. The feature itself ships available (`bIausAvailable = 1`).
32. **Tree OFF / add-on absent.** Recommend IAUS works in base MFO with the three T1 profiles, and the class add-on
   only refines the weights.
33. **Low-rapport randomness in an emergency.** Recommend no special exemption: the floor-gated pool (5.1) already
   makes a clearly best emergency heal win at every rank, so randomness only swaps near-equals.
34. **Spells for melee and ranged classes under strict adherence.** Recommend that spells are not class-gated, only
   weighted (a Warrior can still cast his self heal, it just scores low), and strict adherence keeps gating weapons
   only (G16). The alternative is to limit each line to the schools its default rows name.
35. **Player focus strength.** Recommend m = 1.5 at rank V, and it boosts only (it never suppresses
   non-player candidates).
36. **Who uses scrolls.** Recommend: every follower may LOOT scrolls (the category), and only the mage line, mage
   hybrids and Dragonborn USE them (ruling Q22/23). Tree OFF = the Mage T1.
37. **"The table is gone".** Recommend hidden and kept, not deleted, so switching back restores it and a DLL
   downgrade stays safe.
38. **Bash.** MFO has no bash action. Recommend leaving bash inside the ch.18 attack mix (C8) for this release, rather
   than adding an `act.bash` and a Harbinger drive.
**ANSWERED 11-09: 39 simple one-way; 40 never above player cost; 41 all managed.**
39. Save upgrade (G9, FLWR v6): every save made with the new MFO is unreadable to older MFO versions, which wipe follower boards on their next save. Optional softening: keep writing the old format while no rule has a cooldown, so the one-way step only happens once you set a cooldown. Default: keep it simple (one-way upgrade, stated in the release notes).
40. Spell cost x2 (1dfcuabcr0c): the check compares the curves at skill 50 only. With LoreRim's settings the player curve gives no skill discount, so a doubled follower with LOW Restoration/Destruction skill pays MORE than you (up to 2x at skill 0); at skill 50+ it is at or below your cost. Keep the simple x2, or scale the multiplier with the follower's skill so it never exceeds your cost? Default: simple x2 as you ruled.
41. Spell cost x2: summons (when allowed) and recordless runtime followers count as managed and pay x2 too. Default: yes, they are your followers.

## New (saved 2026-11-10, not asked; nothing calls Auto yet, so not blocking)
42. [MOOT 11-10: Auto disabled with the class add-on] Class Auto skill map (C5, strict adherence): which skills decide the fit for these classes? Defaults in code now:
   Warrior, Mercenary, Champion, Blademaster = Melee (better of 1H/2H); Thief, Master Thief = Sneak + Archery;
   Wizard = Destruction + Illusion; Listener = Sneak + Illusion. (Knight/Great Knight/Ebony Warrior = Melee + Block;
   the rest follow their school.) ALSO: Arch-Mage (AnySchool) ties and beats Saint/Priest for a Restoration follower (B629 R3); should a specific school class win ties? Default: yes. Alternative ideas: Warrior = Two-Handed, Mercenary = One-Handed + Heavy Armor, Blademaster = One-Handed dual.
43-50. [marth 11-10: KEEP the task. "If the list or style (like requiem) uses perks to enforce its rules. But thats a very thin line. You d have to recognize what should and shouldnt be inherited." Ask the details AFTER the C1 census shows the real list] Hidden player-only perks (86e3eevgz, research doc scratchpad/hidden-perks-research.md; defaults in brackets; C1 census proceeds on defaults):
 43. Inheritance core for every managed follower (like HMS), or only Progression-enrolled? [core]
 44. Exclude standing-stone perks? [exclude]
 45. Perk with a script part, until per-entry copy exists: hold back whole, or copy whole with scripts? [hold back, show "not copied"]
 46. Conditions that read the PLAYER's values (run on PlayerRef) make the follower's bonus follow your skill. Accept for now? [accept, flag on board, decide after census]
 47. "Identical" duplicate = same EP + function + value + identical conditions? [yes]
 48. EP9 extra loot on follower kills in scope? [yes, later card]
 49. Lockpick extras (broken lock, crime chance, key reward) apply to followers? [no for now]
 50. Drop EP22 skill-use XP (MFO progression levels with the player)? [drop]
51-55. [11-10: Q53 KEEP ground-position cast (drop zone etc), Q55 skip silently, Q54 by other shape; Q51-52 defaults stand] Complex spells (86e3dmtr1 round 2, doc scratchpad/complex-spells-research.md):
 51. Keep MFO's copied engine-row table (fixed now by C1) or move to a Harbinger query slot (C7)? [C1 now, C7 in the next Harbinger ABI batch]
 52. May Harbinger seat 0 flip the self bit for CLAIMED caster-centred hostile spells (Fire Storm) so they key the Offensive row (C6)? [yes, claimed forms only, after the C2 Deck census]
 53. Descope 86e3dwn9d step 1 (target-location position seat)? Vanilla TL spells are only summons + runes, which the engine places. [descope unless the census finds another TL spell]
 54. Charged / dual-cast-only mod spells aren't marked in the record: treat by their other shape, or per-spell rules? [other shape, logged, no rule]
 55. Reanimate with no corpse in range? [decline, one warning per fight, no fallback]
56. [ANSWERED 11-10: keep 5/4/4] H5 miss budgets for the classes you gave no number (Fable picked defaults; INI-tunable): Archer 5 misses in 10, Scout 4, Witchhunter 4 (Ranger 3, Jaqspur 1 as you ruled). Keep these, or should Archer only detect + log with no correction? [keep]
57. [ANSWERED 11-10: strip on dismiss, restore on rejoin] Class passives on DISMISSED followers: a dismissed follower keeps his class record and his passive effects (faction ranks, Mercenary stamina / Scout speed abilities) indefinitely. Keep them while dismissed, or strip on dismiss and restore on rejoin? [strip on dismiss, restore on rejoin: matches how other MFO effects end on dismiss]
58. [ANSWERED 11-10: ok for release, quick patch after] Warlock / Nightblade passive ("Illusion affects foes ~8 / ~16 levels higher"): the only proven perk road (vanilla Animage) raises the follower's Illusion spell power for every target, so ally buffs like Courage / Rally / Call to Arms also get stronger. The engine has no proven "target is hostile" check for that perk slot. Accept the ally-buff side effect, or leave the passive off until a proven foe-only road exists? [accept and note it on the passive text]
59. [ANSWERED 11-10: class wins; non-matching picks impossible; gambits reset on class pick] With classes ON for a follower, does his equipped class beat an older explicit Gambit-tab pick (e.g. you set him to Melee before classes, then equip Healer = Mage root)? [yes: while classes are ON the class decides; the Gambit-tab pick comes back when classes are turned OFF]
60. [ANSWERED 11-10: pickable T4 for combat behaviour; main = most-perked weapon; situational like Auto (bow vs flying dragons)] Dragonborn has no root (it can use anything). With classes ON and Dragonborn equipped, what decides melee/ranged/mage? [the root of his highest-level mastered T4 class]
61. [ANSWERED 11-10: allow; "no drugs" limits only MFO/Harbinger's own choices] Harbinger potion allowance (ch.30): when a follower is set to "no harmful potions/drugs", should a SCRIPT or console that makes him drink one (e.g. a quest or another mod's EquipItem(Skooma)) still be allowed? Today's design lets scripts and console through (they rank above a client, like the equip rules), blocks only the game AI and sandbox eating, and never blocks your own menu use. [allow scripts/console: they are deliberate]
62. [ANSWERED 11-10: own class stays valid] IAUS, ruling Q34: in an emergency, may a follower still use abilities of its current class before mastering it (e.g. a T1 Healer with the party at 10%)? [yes; off-class abilities of mastered classes get the emergency lift; `Tuning::emergencyOnlyMastered` stays OFF (MFO-B647)]
63. [ANSWERED 11-10: nothing] IAUS I4: with the class tree OFF, what does a T1 Melee/Ranged follower's class gate? Read literally (Core.h "tree OFF: its T1 class") he gets no spell candidates and no power attack / taunt / retreat. [tree OFF = no class restrictions: any known spell, power attack, taunt, retreat, like today's table. GateFor must change in I6]
64. [ANSWERED 11-10: Precision reach] IAUS I4: attack reach for a melee candidate: fMeleeReach x1.15 vs the chase cap? [marth: "Almost every list runs precision, the correct answer is within precision reach". Use Precision's weapon reach at runtime when Precision is loaded (detect it, never assume it; load-order agnostic), else the vanilla melee reach. I6 verifies Precision's API/reach source before use]
65. [ANSWERED 11-10: uncast; research next week] IAUS I4: spells with no readable nature (costliest effect is a Script, ~250 in LoreRim). [leave uncast (logged once). IAUS must know the effect to decide in every case. Research task next week: can the Synthesis item catalog (mfo_items.json pattern) classify them? marth expects most are Illusion / Alteration]
66. [ANSWERED 11-10: yes, ~100k] IAUS sim: one fix round on the Sonnet author for review findings S1-S7, S9-S11 (positions 0 gate all area offense, Control over-classified, Hold not continued, non-T1 gates, ...). S8 (movement) to backlog.
67. [ANSWERED 11-10: no re-anchor] IAUS: the commitment window re-anchors on every Fired (MFO-B651), so a repeating attack keeps the 1.5 s window open and a #1-ranked heal must beat attack x1.25 (sim: 470/1800 unhealed low-HP ticks in LoreRim strong fights). [repeat fires of the same decision do not re-anchor the window; no emergency margin skip. Fix in I6 (closes B651)]
68. [REFRAMED 11-10] Precision reach. marth: "precision reach will be different for every single weapon. so find one formula wont work". [Not one reach number: the swing gate uses each weapon's OWN reach, read live per weapon. Plan: GetAttackCollisionCapsuleLength per weapon (proven API, main thread) plus the conversion Precision itself uses for AI reach, (length + fAIWeaponReachOffset) x k + r, with fAIWeaponReachOffset read from Precision's INI at runtime and k, r taken from its published docs or measured, never copied code (GPL). The field probe confirms the conversion; until then vanilla fMeleeReach]
69. [ANSWERED 11-10] Taunt cooldown/rules. marth: "a 30s cool down it lasts for 15s if it works. There has to be a stat based reason it fails or succeeds that iaus can read, and the gambit side needs a cool down too or else it just constantly fires. Both should skip if everyone already taunted or no one can be." [30 s cooldown, 15 s effect on success, stat-based success chance readable by IAUS (formula designed in the sim lab, comes back as a question), table-mode taunt gambit gets the 30 s cooldown too, both modes skip when every relevant foe is already taunted or none can be]
70. [ANSWERED 11-10, clarifies Q64] Reach veto. marth: "I meant when it should swing the weapon, not when it should choose a target." [target choice is not reach-gated (any foe in chase range may be named); the reach gate applies to swing decisions IAUS times (power attack, bash, any committed attack animation), Precision reach when proven, else vanilla fMeleeReach]
71. [ANSWERED 11-10: add, default ON] A global MCM switch bIausAvailable (I7 card) so a player can turn the whole IAUS feature off for every follower; per-follower toggles stay default OFF.
72. [ANSWERED 11-10: keep the level rule] Taunt success formula: clamp(0.5 + 0.05 x (taunter level - foe level), 0.1, 0.9) (Taunt.cpp:219), readable by IAUS before it chooses.
73. [ANSWERED 11-10: per follower] Power-attack cooldown: one cooldown per follower shared by every row and by IAUS (like the Q69 taunt cooldown); rows cannot stack around Q7.
74. [ANSWERED 11-10: top rows, rest unlock] Rank I has 3 combat slots, class default sets have 4-5 rows: the follower gets the top rows in priority order; the rest are added in the same order as slots unlock with rapport. C12 defaults must be good at every prefix.
75. [ANSWERED 11-10: research, and check MFO] Dynamic Follower Scaling on LoreRim (ReductionPerFollower = 20): marth: "Agent can research it, but I'd bet MFO already overrides it. I guess make sure." Sim v2 researches what it scales and whether MFO overrides it (file:line).
76. [ANSWERED 11-10: factor it in, in the SIM] Dynamic Follower Scaling (LoreRim FollowerScaling.dll 1.1.0) scales a follower's weapon hit damage by 0.8^(followers loaded - 1) for CurrentFollowerFaction actors; MFO does not override it. marth: "Then its relevant in lorerim and we factor it in." [MFO leaves it to the list; the sim models it in the LoreRim world. marth: "I just mean its factored in the sim." No native work for it]
77. [ANSWERED 11-11: engine flags] (also Q85) What tells MFO a follower is mortal: the engine's essential / protected flags at runtime (not essential and not protected = mortal). No list knowledge.
78. [ANSWERED 11-11: relearn each load] (also Q84) The learned per-weapon reach is runtime only, relearned after every load (~12 swings). No co-save change.
79. [ANSWERED 11-11: accept] The taunt's "no foe can sense him" case is found after the idle plays (no safe off-main sense read); logged per foe, the 30 s starts.
80. [ANSWERED 11-11: all obey 10 s] Every power-attack row, including rows without a cooldown of their own, and IAUS obey the one per-follower 10 s power-attack cooldown.
81. [ANSWERED 11-11: hold until probed, then adopt with a cast yield] Harbinger INVARIANTS #0 (l) START ONE COMBAT MOVE stays PROPOSED until marth's field probe proves the bash; then it is adopted with the added condition that the move yields to an active ch.8 cast claim (a bash never cuts his own charging spell, review F1-4).
82. [ANSWERED 11-11: move to Harbinger] If the probe confirms MFO's power attack is only an animation event, move it to the ch.31 action as kCombatMove_PowerAttack (the engine's real road: power damage and stamina).
83. [ANSWERED 11-11: build next] A Harbinger block (held state: window + deny of his own offense and of the engine's release) is the next Harbinger round, after the bash road is field-proven.
86. [ANSWERED 11-11: no measured rate] A foe's spell threat is not a measured per-actor casts-per-second. marth: "no, its iaus weighted based on magicka, damage, etc." [IAUS estimates a foe caster's threat from what it can read: its magicka pool and regen, the damage/effect of the spells it can cast, charge time; no per-actor learned rate]
87. [ANSWERED 11-11: yes, once confirmed for followers] The list's sneak attack multipliers (up to x15 dagger on Vanilla perks, x12 on LoreRim) are left to the list only once a field probe confirms they apply to followers. marth: "If we confirm the lists number affects followers yes, the related perks the follower can get through progression must also work" [progression-granted sneak perks must also work for followers]
88. [ANSWERED 11-11: read it, Harbinger deny as the fallback] Read CalculateDetection (AE 37774) as a read-only RE card beside G2. marth: "Yes read it, worst case we may need to add forced non detection blocks into harbinger on npcs who would otherwise detect if the game doesnt properly calculate npc to npc detection"
89. [ANSWERED 11-11: cap it] (T3, parked) Cap the G4 flanking bonus (half the sneak bonus on a flanked foe is about x9 per hit on LoreRim multipliers).
90. [ANSWERED 11-11: blade primary, bow fallback] (T4, parked) The Listener. marth: "Personality says they enjoy the blood of melee, but they will bow when needed" [blade primary, bow when needed]

## ANSWERED 2026-11-11 (marth, AskUserQuestion; recorded here after a /tmp wipe lost the session copy)
- T2 scope: "No point in t2 without hide and disengage, anchor leak too." G2, G3/H3 and the anchor-leak fix are in Classes Beta 1. Batch similar tasks per agent.
- Engine effects: "harbinger is already capable of stopping movement, almost all the calls we are adding should be through harbinger" and "new harbinger facets being created to suit what was needed". Reads may stay in MFO.
- MCLS v2 review: revert the sim's Warrior/Conjurer/Warlock default rows (done); keep and restore the classes-OFF tables (N1); logistics tables remembered per class, no defaults (B#29/N2).
- G2: engine hide timing, no forced non-detection seat (Q-G2-1); point visibility through MFO's own rays for now, a Harbinger sight query joins the full-port list (Q-G2-2); the group-record read is tier B (Q-G2-3); P4 light probe only if P2 shows outdoor disagreement (Q-G2-4). "There is a perk for hidden casting in some lists": silent casters (EP 55 result 0) keep casts while hidden.
- ch.24 combat area centred on a point: yes (already shipped as kApproach_Point, ABI v39).
- B#37 old table: hidden and kept. B#34: "Emergency only heals otherwise gated" (off-class spells gated; heals get the emergency lift). B#35 player focus m: whatever the oracle shows works best. B#33: no emergency exemption for low-rapport randomness. Q68 reach: vanilla fMeleeReach until the probe.
- B#36 scrolls: everyone may read scrolls; a non-mage may hand one to a mage; scrolls of known spells are traded to a follower who does not know the spell; all MCM options; a card after the T2 session.
- G3 pacing: "the main purpose is to become hidden, and it simply doubles as a version of flee": sneak classes end the run when hidden, then sneak attack; anyone else flees and heals ASAP. Hide run for sneak classes only (MB3). IAUS holds G1's sneak claim while hiding (MB5). Anchor fix refuses every held-holder join (MB2). Retreat vs flee below 30%: score decides.
- Flee deny: "wouldn't it be non offense cast": Harbinger ch.7 Offense now means hostile actions only (ABI v40, feat/ch7-hostile-offense).
- Tuning: Fable fixes the sim and makes real sim changes; pure curve training goes to a cheap agent (Haiku medium, coordinator's pick).
- Open, with defaults taken (from the movement and ch.7 runs): MB1, MB4, MB6, MB7, MB-F1..F4, CH7-Q1..Q5 (see the agents' reports and the backlog entries B740..B762).

## ANSWERED 2026-11-11 18:36 PT (marth, chat)
- T2 scope grows: asked "g2 g7 g9 g14, are those done?" (G2 Layer 1 and G9 merged; G7 and G14 not started), marth: "Looks like we need some of these for t2, all probably. and they need to be in sim". So G7 corpse target / reanimate and G14 pick nearby locks join the T2 build, and G2, G7, G9 and G14 must all be modelled in tools/iaus_sim. G9 cooldowns are already in the sim; G2 is going in now (feat/iaus-sim-stealth).
- Saved, default taken: Q97. G14 depends on G13's witness pre-check. Default: build the shared pre-check with G14; the G13 steal action itself stays in T3 unless marth says otherwise.
- Saved, default taken: Q98. The combat sim has no world logistics. Default: G14 is simulated as its witness pre-check against the sim's engine detection model (Detect.h: every nearby NPC's senses must all be 0), not as a lockpicking walk.
- Saved, default taken: Q99. G7 is listed for Necromancer and Arch-Mage (T3). Default: the cond.corpse_within selector ships as a base gambit usable at T2 (Conjurer has reanimate spells), class defaults stay with the Necromancer line.

## Saved 2026-11-11 19:30 PT (T2 manifest pass, defaults taken; full list in t2-manifest.md)
- Q97 default REVERSED: G13 steal is T2 (the Thief's default `act.steal` is missing); G13 and G14 ship together.
- Q-T2-1: Block is built now (not after the bash road's field proof); bash and Block are field-tested together in T2.
- Q-T2-2: ch.31 PowerAttack is built now; the Q82 probe runs in the same session and decides which road the gambit keeps.
- Q-T2-3: Knight and Warrior hold the line by design; the Mercenary gets a back-off road (his class text asks for it).
- marth 11-11 ~21:00: "We'll need harbinger to block crime alarms later, it may as well store that data for that task, it may even be needed for steal gambit". The town-theft RE (ClickUp 1dfcuabczbg) also maps the engine's crime-alarm road (ids/RVAs per runtime, cleanest seat) for a future Harbinger "deny crime alarm" facet; G13's no-witness pre-check reuses the engine's own witness decision from that note.
## From the ch.7 v40 fix round (2026-11-11, Harbinger feat/ch7-hostile-offense + MFO feat/g3-disengage; work proceeds on the default)
91. (CH7-Q6) Harbinger's "beneficial" is now an allow-list: any spell with a Script effect counts as hostile under an Offense-only deny. A heal from a spell mod that carries a script effect (a description or perk-hook effect beside the heal) is therefore DENIED while a follower flees (G3) or holds hidden with a heal claim (G2), and APMF.log names it. DEFAULT taken: leave it denied and logged (the safe side of the review's SEV-2). Alternative: a spell the SAME client holds a live cast claim for counts as declared by that client and passes its own Offense-only deny (a Harbinger change, its own card).
92. (CH7-Q7) The benign list is exactly the one in the fix brief. NightEye, Dispel, Open, Lock, Darkness, ValueAndParts, Werewolf and VampireLord are NOT on it, so a Night Eye or a Dispel cast is denied under an Offense-only deny. DEFAULT taken: not on the list. Alternative: add NightEye (and Dispel when delivered to Self).

## From class card H2, aversion (2026-11-11, Harbinger feat/aversion + MFO feat/class-aversion; work proceeds on the default)
93. (H2-Q1) The aversion weight for Conjurer and Healer. The rulings say "a low chance of being targeted" and "same shape", with no number. DEFAULT taken: 0.50 for both (`fAversionConjurer`, `fAversionHealer` in MFO.ini; a foe whose pick lands on him goes to its next best target half the time). Alternative: name the number (0.25 mild, 0.75 strong). Necromancer "slightly more than T2" then follows from it on its own card.
94. (H2-Q2) A mastered, unequipped Conjurer or Healer passive gives a QUARTER of the weight (the standing mastered-passive rule), and the class at any level gives the full weight (no growth by class level). DEFAULT taken as stated. Alternative: aversion only while the class is equipped, or a weight that grows with class level.
95. (H2-Q3) Harbinger draws "averted or not" per foe and follower for a 6 second window, so the share of time is the weight whatever the game's re-pick cadence is. DEFAULT taken: 6 s windows. Alternative: a fresh draw at every re-pick (the share of time then follows the game's 1 s / 3 s cadence: weight 0.5 leaves the follower targeted about a quarter of the time).
96. (H2-Q4) Harbinger INVARIANTS #0 (m), "weight a claimant's candidacy in another actor's target selection", is PROPOSED with the branch. It shapes the choice of an actor that holds no claim (the foe), which is new. DEFAULT: adopted at the merge of feat/aversion. Alternative: reject, and the card becomes a proven limit.

## From the G3 / H2 fix round (2026-11-11, MFO feat/g3-disengage + feat/class-aversion, Harbinger feat/aversion; work proceeds on the default; numbers 97-99 are taken on main)
100. (G3-FIX-Q1) The blow from stealth keeps the sneak stance until it resolves (review P1-G3: the game reads the stance when the hit lands). Three defaults taken, marth may overrule each. (a) While his swing, shot or cast is under way, a foe detecting him does NOT stand him up; the stance is released when the blow resolves. Before the swing starts a reveal still releases it at once. Alternative: a reveal always releases at once, also mid-swing. (b) A swing, drawn bow or charged cast that stays live 6 seconds with no hit releases the stance and logs a warning (the number is chosen, not measured; the log line gives the real duration). (c) After a shot or a cast leaves the hand the stance is kept 1.5 seconds more for the projectile unless the hit comes first. Not verified in the engine: whether an arrow's sneak multiplier is fixed when it is loosed or read when it lands. The tail covers both.
101. (H2-FIX-Q1) Aversion, review H2-1. The game's target scorer lowers the score of a target the foe cannot reach, cannot path to, that is dead or essential-down, or that is FLEEING, but does not skip it. So when a foe is turned away from the Healer or Conjurer, its "next best" target could be one of those. DEFAULT taken by the coordinator, marth may overrule: Harbinger uses the next best target only when it passes the game's own four tests (it calls the game's own functions; it still computes no score). If it fails one, the foe stays on the claimant and APMF.log says which test. Two notes from the fix round. (a) The fourth test was called "downed" in the review; reading the function shows it is "is fleeing" (essential-down is already covered by the dead test). Default: a fleeing next-best target is not used either, because the game itself marks it down and a foe sent after a runner undoes that runner's flee (card G3). Alternative: use a fleeing target anyway. (b) Only the first alternative is tested; when it fails, Harbinger does not look for a usable third-best. Alternative: leave the unusable one out too and ask the scorer again (up to four tries).

## ANSWERED 2026-11-11 ~22:30 PT (marth, AskUserQuestion)
- Town theft (1dfcuabczbg): "It may be a mod that alters the crime system then. Bar all stolen, and disable looting anything in front of guards for every follower for now until we find the exact culprit". Already-stolen copies are never taken, including what the player drops (overrides the 09-27 "what you drop is fair game" ruling for stolen items). Interim: no loot take while a guard can see the follower or the item. RE: Docs/research/2026-11/theft-and-crime-alarm-re.md.
- Theft fix scope: both halves (per-item stolen gate + locked/owned door and furniture gate) in one round.
- Q100 (a): keep the stance mid-swing (default confirmed).
- Q101: keep strict, a foe never switches onto a fleeing ally (default confirmed).
