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
