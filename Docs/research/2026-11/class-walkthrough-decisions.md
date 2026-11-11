# Class walkthrough decisions (marth, interactive, 2026-11-09)
Source draft: class-perks-packages.md. Each ruling here OVERRIDES the draft.

## Global
- Passive scale: equipped values stay as drafted. Permanent (mastered, not equipped) = 1/4 of equipped, rounded (marth: "equipped strength is good halve passive with rounding. so 1/4").
- Tier-1 pool bonuses halved: Melee/Ranged/Mage foundation = +6 / +12 (permanent ~1/4 of equipped). Root lift kept (not overruled).
- Tier 1 behaviour: vanilla for its role (marth: "t1 behaves vanilla for those roles"). No custom combat package at tier 1; specialised packages start at tier 2.
- Targeting: combat packages never choose targets. Gambits decide targeting (marth: "Dont change targeting, gambits decide targeting"). Strip every "Target:" line from the packages.

## Knight
- Passive: ONLY "blocking stops 15% more damage" equipped (marth: "Just the shield blocking 15% more."). Permanent = 1/4 (~4%). Bash +25%, Heavy Armor and One-Handed effects dropped from Knight. (Interpretation, confirm at the end: Knight owns only the Shield line, with only the block effect.)
- Package approved: the wall, stays close, blocks a lot, bashes, little circling/fallback, no flank, short ch.23 leash, ch.18 power share max 0.25. No targeting.
- Default gambits approved as drafted.

## Dodge research (2026-11-09, dodge-research.md)
- LoreRim: Ultimate NPC Dodging (UND.dll + esp) dodges NPCs by sending TKDodge* events; its tactical dodge score reads CSTY avoidThreat/fallback/circle/strafe. Detect UND.dll / esp; UND_ExcludeDodge keyword opts out.
- Tuxborn: only the TuxBFCO profiles have Simple Npc Dodge (SPID perk + OAR idle swap). Standard Tuxborn and custom-modlist: none.
- ONE PASSIVE PER CLASS (marth: "Lets only do one passive per class"). Supersedes the 13 shared trait lines: each class owns exactly one passive effect. (Lines shared between classes are gone.)

## Warrior
- Passive: melee weapon damage +15% equipped (any melee weapon), permanent 1/4 (~4%).
- Package: pushes hard, head on, BUT circles and flanks too (marth: "a warrior will circle and flank, but push hard"). Presses with allies, never backs off. ch.18 power FLOOR 40% "if it works"; a regular attack must fire when stamina can't pay for a power attack (sent to the ch.18 author).
- Gambits: drafted set approved WITH a 10 s cooldown on each power-attack gambit (power attack blocking foe, power attack stronger foe). Check base gambits support a per-rule cooldown; if not, it is a base gambit gap.

## Mercenary
- Passive: +40 Stamina, +25% stamina regen equipped (permanent 1/4).
- Package approved: in-and-out skirmisher. "Retreat" means BACKING AWAY while still facing and fighting (CSCR fallback), never a flee/disengage (marth: "retreat cant be the package its just backing away? Then yes"). Circles wide, flanks, avoidThreat high, ch.18 power cap 15%, rare bash.
- Gambits approved: casters, then archers, then lowest HP; stamina potion < 25%; else equip melee.
- Weapon swaps: decided by GAMBITS, never by a package (marth, Archer round). Packages hold range/movement/attack style only.

## Archer
- Passive: bow and crossbow damage +15% equipped (1/4 permanent).
- Package: line archer approved, PLUS miss detection and correction (marth: "Needs miss detection and correction"). This is the deferred ranged-accuracy work (memory ranged-accuracy-needs-projectile-hook: WeaponFireProjectile hook). GAP: Harbinger projectile facet.
- Archer melee weapon: ONE-HANDED ONLY (marth: "archer can only use 1h").
- Gambits approved as drafted.

## Thief
- Passive: sneaking movement 30% quieter equipped (1/4 permanent).
- Package: "a free stealth entry every 5 fights, and decide based on stats for the other 4" (1 fight in 5 opens guaranteed undetected; the other 4 roll from the Thief's own sneak vs the foes' detection). "prefer dagger if they are close and flanking, otherwise undetected bow on the lowest hp enemies." So the Thief uses dagger + bow; swaps stay gambits.
- Gambits approved as drafted (act.sneak and Steal are base gaps).
- DISENGAGE (marth, Evoker round): "thief and mage classes both can disengage, and use regular movement in combat to reposition faster." Applies to Thief and every mage class (and their lines). GAP: a Harbinger reposition/disengage drive (regular, non-combat movement mid-fight; ch.19 travel or ch.23/24 relatives). Note: this extends the "retreat = backing away" rule only for these classes.
- SPELL RANGE (marth): mage classes "use spell appropriate range automatically. Near the furthest range an equipped spell will work, but will move to range for touch or cone spells automatically." Range is derived from the equipped spell's own range/delivery, never fixed per class.

## Evoker
- Passive: Destruction magnitude +15% equipped (1/4 permanent).
- Package: auto spell range (above) + disengage. Steps out of incoming spells. Cast guard as today.
- Default gambits: ONLY fire on a fire-weak foe, nearest enemy, magicka potion (marth: "just fire weakness, nearest enemy, and magicka potion usage by default").

## Conjurer
- Passive: Conjuration 15% cheaper + a low chance of being targeted by foes (marth's pick, two effects). GAP: "less likely to be targeted" is an enemy-side target-selection weight; needs a Harbinger seat on foes' target choice (ch.6/ch.20 relatives) or a vanilla lever (check a perk entry/AV that lowers aggro priority before building).
- Package approved: summon first, behind it, re-summon, auto spell range + disengage, summons on the ground near the foe.
- Gambits: summon when none active (MISSING cond.self_has_summon), nearest enemy, magicka potion, + melee (dagger) equipped, + target the enemy targeting its summon (MISSING cond/target: foe attacking my summon).
- Page: https://claude.ai/artifact/EWMz5VGpjuTXKa35AuUVwf now reads db collection "classes" (doc id = class slug, fields s/p/a/g) and "meta/rules" {items}; update rows via ArtifactData, no republish.

## Healer
- Passive: Restoration 15% cheaper + less likely to be targeted (same shape as Conjurer).
- Package approved as drafted (medic, behind the party, leash near player) + auto spell range + disengage.
- Gambits: trimmed set: heal player <50, ally <50, self <50, magicka potion, nearest enemy.

## Warlock
- Passive: Illusion affects foes ~8 levels higher (equipped; 1/4 permanent ~2 levels).
- Package approved (controller) + auto spell range + disengage.
- Gambits: trimmed set (paralyze/fear stronger foe, armor spell up, magicka potion, Destruction nearest). BASE RULE (marth): "Needs to recognize when a spell will never work, that gambit is skipped if it wont work due to the level difference." A level-capped spell (Illusion max level, paralyze/etc. by magnitude) whose cap, after the caster's perks/passive, is below the target's level makes its gambit SKIP (fall through), never fire. Base MFO gambit gap, applies to every class.
- TIER 3+ SHAPE (marth): signature = the class's one passive, BUT "T3s get two strong passives" = one named passive carrying two strong effects. 1/4-when-mastered rule assumed to still apply (confirm at the end).

## Great Knight
- Passive "Shieldwall": staggered 50% less + blocking stops 10% more damage.
- Package approved (Knight more extreme, power cap 20%, short leash). Gambits: Knight's + taunt at 3+ foes.
- T3+ PASSIVE SHAPE CORRECTED (marth): "its 1 strong and 1 smaller effect ... labeled in the same manner as shield wall. we'll need to come up with a name for each passive at t3 and up". (Shieldwall = 50% stagger resist strong + 10% block smaller.)

## Champion
- Passive (name TBD): power attacks +20% damage (strong) + 10% less power attack stamina (smaller).
- Package: Warrior more extreme, power floor SAME 40%.
- Gambits: same as Warrior (10 s cooldowns). No sweep/staggered extras.

## Blademaster
- Passive (name TBD): +15% damage from any weapon source (strong) + all stamina costs -10% (smaller).
- Package approved (Mercenary more extreme, power cap 15%, left-hand mix = known risk).
- Gambits: same as Mercenary.
- CLASS-ONLY GAMBITS exist (marth, Ranger): a class can unlock a gambit action usable only while it is equipped. Reconcile with "gambits are base": the action lives in base MFO, the class add-on gates its availability (to confirm when building).

## Ranger
- Passive (name TBD): +15% armor penetration with all ranged weapons. Smaller effect = the Mark gambit unlock.
- MARK (Ranger-only gambit): sets the WHOLE PARTY's target to that foe for 10 s, and the foe takes +10% damage. Needs a party target pin (Harbinger ch.20 per follower) + a damage-taken effect on the foe.
- Package: longest range; "should not miss more than 1 in 10 shots. I dont know how we'll do that yet" (accuracy research open: projectile hook / aim correction).
- Gambits: Archer's set WITHOUT melee swap; instead Mark a closing melee foe ("like a reverse taunt").

## Master Thief
- Passive (name TBD): sneak attacks x1.5 damage (dagger and bow). NO paralyze. Smaller effect = the flanking rule below.
- Free stealth entry 1 in 3 fights.
- SNEAK RULES (marth, Thief + Master Thief, all sneak classes): sneak "regardless of player action". "Sneak itself needs to use disengage to go to an area enemies cannot see and sneak. Unlike the player follower sneak doesn't need to wait for them to forget, if sneaking not seen they should be in sneak. like d&d rules." => an unseen sneaking follower is HIDDEN immediately (Harbinger ch.16 detection / ch.3 stance; a detection-state set, check deny-completeness).
- D&D FLANKING (marth): "t3 and higher sneak classes also get half sneak attack damage according to d&d flanking, and engaged rules" => T3+ sneak classes deal half the sneak-attack bonus on a foe that is engaged with an ally (flanked), even when detected.
- Gambits: Thief's set, fleeing foes first; sneak always on.

## ch.18 built (2026-11-09)
- APMF feat/apmf-ch18-attack-select 496243e, CI green 42664016813, ABI v34, header sha256 5bb0342e... Not merged; tier-3 Opus review dispatched. Open marth questions: floor threshold (>=2 vs >=1 undenied), static Normal deny stamina-gated?
- ch.18 rulings: floor threshold = 1 "in circumstances it is required" (never empty, counted over eligible candidates); stamina rule applies to EVERY Normal deny. Review (tier 3) found SEV-2 (floor counted raw pool), SEV-3 (threshold), SEV-4 ABI zero-share semantics; sent to the author with the rulings.

## Wizard
- Passive (name TBD): Destruction +20% on a controlled foe (strong) + all spells cost 8% less (smaller).
- Package: stays at range like the other mages (spell range + disengage); gambits decide most of it.
- WARD DESIGN NEEDED (marth): "a design that when gambited only blocks when hostile spells are being charged at the follower." Base gambit gap: cond "hostile spell being charged at me" + act ward (hold a ward only during the foe's charge). Not built anywhere yet.
- Gambits: ward (new design), paralyze stronger foe (level-skip), Evoker's set.

## Necromancer
- Passive (name TBD): +1 summon/thrall at a time (strong; risk on engine AI casts) + thralls +25% health.
- Package: far back, BETTER at staying away than other mages (more fallback/disengage), and less likely to be targeted, slightly more than the T2 Conjurer/Healer (marth).
- Gambits: Conjurer's set + reanimate corpse (corpse target gap).

## Priest
- Passive (name TBD): healing +20% magnitude + undead/Daedra within 6 m take 2 sun dmg/s (cloak).
- Package: Healer's leash and priority, keeps the lower-target chance, "full anti undead aggression" (closes on and attacks undead).
- Gambits: Healer's set with sun/turn undead on unholy foe above the self-heal.

## Spellsword
- Passive "Spellstrike": melee hits +10 element damage (other hand's element, fire if none) + Destruction 10% cheaper with a weapon in hand.
- Package: HYBRID MOVEMENT BIAS (marth): "A health/magicka/range bias is used to determine movement, so a low health spellsword may disengage to magic range, while low magicka or high health may go back in." Likely the rule for all melee+magic hybrids (confirm). Needs a live per-tick stance-distance choice (MFO decides, Harbinger moves: ch.23/24 + disengage drive).
- Gambits: as drafted (approved implicitly).
- HYBRID "GO IN" RULES (marth, nuance per class, not one shared bias): Spellsword = health/magicka/range bias. Sorcerer = "only go in when high health and full summons". Templar = "only go in when all the party is in good health, or when they have no magicka to help". Nightblade = "only go in on controlled enemies, or when none can be controlled". (Witchhunter/Scout: ask.)

## Sorcerer
- Passive (name TBD): summons last twice as long (strong) + 20% less damage taken while all summons are out (smaller).
- Package approved + go-in rule above. Gambits approved (bound weapon must count as melee weapon for equip authority: VERIFY).

## Templar
- Passive (name TBD): weapon always counts as silver (strong; vanilla silver bonus is a player-only perk, so our own perk entry, check on NPCs) + Restoration 10% cheaper (right-hand spell, left shield; weapon+shield+spell at once is impossible, swaps via gambits).
- Package: Knight-like line holder, go in only when ALL party >= 70% health or no magicka.
- Gambits: heal thresholds = the same 70% "party healthy" threshold (marth).
- LISTENER pre-ruling (marth): Listener's strong effect = "all illusion effects affect any lower level non boss".
- CORRECTION, Listener pre-ruling: Listener strong = Illusion works on any NON-BOSS foe up to the Listener's own level (option 3), not "any lower level".
- STACKING ACROSS CLASSES (marth): mastered passives of OTHER classes stay active (at 1/4) beside the equipped class's full passive ("the mastered warlock passive will contribute too"). Only the same passive never doubles.

## Nightblade
- Passive (name TBD): Illusion works on foes ~16 levels higher (strong) + +15% melee damage on controlled foes (smaller).
- Package approved: go in only on controlled foes or when none can be controlled; flanks; disengage. Gambits approved.

## Witchhunter
- Passive (name TBD): Illusion works ~18 levels higher on undead, Daedra and casters (strong) + every hit drains stamina and movement speed (smaller).
- RISK: many vanilla/modded Illusion spells carry target conditions that exclude undead/Daedra/automatons outright (not level-based). A level bonus does not lift those. Check the spell conditions on the lists; may need the Witchhunter to bypass the immunity condition (perk entry) or this effect does nothing on undead.
- Package approved (never goes in, range, disengage). Gambits approved.
- Witchhunter: LIFT the Illusion immunity of undead/Daedra for its spells (marth approved).

## Scout
- Passive (name TBD): +20% movement speed (strong) + Restoration 15% cheaper (smaller). (Healing field rejected: "A scout would stay ranged and its healing field wouldn't be effective".)
- Package approved, 70% party-healthy threshold; gambits at 70%.
- TIER 4 SHAPE (marth): "1 effect that matches the t3 classes required to unlock full passives, and one unique effect." => a T4 equipped gives its required T3 parents' passives at FULL strength, plus one unique effect of its own.

## Arch-Mage
- Effect 1: the full passives of the two T3 parents it unlocked through (two of Wizard / Necromancer / Priest).
- Package + gambits approved (parents' sets merged by priority; ward vs charging caster, summon when outnumbered, heal < 70%, element vs weakness, nearest, potion).
- T4 UNIQUES must be ACTUAL new mechanics, not stat bumps (marth: "Id prefer actual uniques").

## Arch-Mage
- Unique: two SIMULTANEOUS casts of DIFFERENT spells (one per hand at once). FEASIBILITY: heavy Harbinger work; the engine's cast latch is actor-wide (memory cast-latch-is-actor-wide-hand-blind); a dual-hand concurrent cast needs a design pass.

## Living Legend
- Effect 1: Blademaster + Champion passives at full.
- Unique: "hostile attacks while attacking are automatically blocked 40% of the time" (a hit-time chance negation while the LL is mid-attack; needs a hit hook or perk entry with a random condition: check feasibility).
- Package + gambits approved (Champion mode floor 40% vs one big foe, flurry mode cap 15% vs many).
- PER-SECOND NUMBERS (marth): "All our /s numbers will probably need to be factored per list, or available in an mcm for tweaking." => every /s value is MCM-tunable, with a default scaled at runtime (never per-list tables: no-list-specific-data rule).

## Ebony Warrior
- Unique: GUARDIAN: allies near it take 25% less damage; it takes that share instead.
## Grand Battlemage
- Unique: every power attack also releases the off-hand spell for free at the struck foe. "the AI will need to know this" => its package/gambits must weight power attacks with a spell in the off hand (ch.18 floor for GB when a spell is in hand).
## Jaqspur
- Unique: bow/crossbow draw speed 60% faster.
## Saint
- Unique: healing aura, allies +30 HP/s, undead take 30/s (MCM-tunable per the /s rule).
- EW / Jaqspur / Saint packages approved as listed (parents' sets merged; EW Templar go-in 70%; Jaqspur sniper + Mark; Saint leads vs undead, 70%).
- Grand Battlemage AI (marth): "Awareness of exactly what spell is in hand and the enemies weakness (and splash damage)" => the power-attack-release choice reads the off-hand spell, the foe's weakness, and the splash (never splash the party).
## Listener
- Approved: parents at full + Illusion on any non-boss up to own level; executioner package, power cap 25%, gambits as listed.
## Nightingale
- Unique: SHADOWCLOAK, once per 30 s in a fight, undetected for 3 s, re-open with a sneak attack.
## Nightingale: package + gambits approved.
## Dragonborn: no extra unique; all mastered passives at full, no class limits, all common shouts gambitable; package copies strongest mastered T4 + shouts.

## WALKTHROUGH COMPLETE 2026-11-09 (all 35). Open follow-ups:
- Names for every T3/T4 passive (only Shieldwall, Spellstrike named).
- Confirm: 1/4-when-mastered applies to T3 two-effect passives; Knight = only the block effect.
- New gaps raised: less-likely-targeted (foe target weighting), ward-on-charge gambit, miss correction (Archer <=? / Ranger 1 in 10), disengage drive, always-hidden sneak + D&D flanking, hybrid go-in rules, class-only Mark gambit (party pin + damage taken), spell-will-work level skip, gambit per-rule cooldown, Illusion immunity lift, LL 40% auto-block, Arch-Mage dual concurrent casts, GB spell release on power attack, Guardian damage share, Shadowcloak, MCM-tunable /s values.

## Question batch answers (marth 2026-11-09, interactive)
- Q1: 1/4-when-mastered applies to T3 passives too.
- Q2: Knight = block 15% only. Intent: "the follower with knight, and indeed any in your face tank taunts and only stops blocking to attack essentially" => tank packages block near-constantly, dropping block only to attack.
- Q3: I propose lore names for T3/T4 passives, marth picks.
- Q6: M1 sequenced; the four effects (H5, H2, G2, H3) land in M2, logged as missing.
- Q7: per-rule cooldown on POWER-ATTACK rows only for now ("i'll likely find other things that need pacing").
- Q8: class defaults apply on FIRST equip + reset button, AND "remember each classes setup so when they switch back its there" => per follower, per class gambit table is stored (C3 MCLS / C12 must hold per-class gambit sets).
- Q9: keep class progress when the ESL is absent.
- Q10: Witchhunter never goes in (no melee by default), but a melee gambit may fire if ALL enemies are controlled. Scout: default stands (party >= 70% rule).
- Q11: Living Legend auto-block = sound + spark only.
- Q12: Shadowcloak = vanilla Invisibility (visible cloak 3 s).
- Q22/23: mage classes may use wands + staves AND should pick up and use SCROLLS; Thief no crossbow. NEXT BIG UPDATE: an IAUS mode that disables gambits; start soon with some gambits being IAUS-powered: scrolls, and enemy weakness -> Exploit.
- Q24: hybrid root = weapon side.
- Q25: T2 needs ONLY its root T1 mastered (T1 is the exception to the 2-masteries rule). Pure T3 = own T2 + a SAME-LINE T2 (marth picked option 2; FE/FFT precedent). Wizard follows the same rule (Evoker or Warlock + another mage-line T2), no fixed pair.
- Q26: let AUTO treat control spells (paralyze/fear/calm/turn undead) as HOSTILE (enemy-targeted) => base change to the nature handling of c62d4cf for enemy-control archetypes; class rows then use AUTO normally (drop pendingRuling).
- Q27: keep draft caps Thief 0.2, Master Thief 0.2; Templar: NO 0.25 cap: "a Templar would still swing heavy pretty close to vanilla and more against undead" => Templar engine split + higher power share vs undead (a conditional floor vs undead).
- Q28: T1 keeps today's default kit (no class default set).
- Q13 magicka: "Cheap option, double it." Followers' spell cost x2, applied ONLY if a startup check proves the NPC cost curve is cheaper than the player's (GMST compare). Implement as a cost multiplier the engine itself applies on every road (e.g. a ModSpellCost x2 ability/perk on managed followers), not a rescale of MFO's own charge. Note: the 29-50% figures are curve-only (before perks), so 2x lands around 58-100% of the player's curve cost.
- Q14: dual-wield with tree OFF: leave as today.
- Q15: Harbinger substrate (case a): let the combat substrate open when the Engine slider outranks the client (marth picked 2), AND "make sure the allowed count properly tracks who it is" (menu Allowed/Served counters attribute to the real source).
- Q16-19: accept all defaults.
- Q20: retreat keeps HOLDING until the engine Flee ends.
- Q21: accept the narrower Skooma fix.
- B1a: queue the hidden-weapon hand planner as its own task; merge B1.
- Q4: MFO-B560 switch board text input to the W (UTF-16) API (tier A, window hook).
- Q5: leave the Harbinger v0.17.0 lightweight tag.
- Turn undead (and any target-conditioned control spell): AUTO picks only foes passing the spell's own target conditions (undead for turn undead); else the row falls through (marth 11-09).

## Question batch 2 answers (marth 2026-11-09, interactive)
- Q29: per-class memory stores BOTH tables (combat AND logistics) => MCLS v2 append (logistics tables per class).
- Q30: bound bows WAIT for the animated Harbinger fix (1dfcuabcr7c); MFO direct-road branch stays parked.
- Q31: IAUS OFF by default per follower; feature available.
- Q33: no low-rapport randomness exemption in general emergencies...
- Q34 (refines Q33): spells for melee/ranged classes are usable "Only if a mage class is mastered, and then it really does score low. But this is a place where emergency could boost its score. But again only abilities from mastered classes are valid in emergencies." => off-class spells are candidates ONLY when the follower has mastered a class that grants them (a mage class); they score low normally; an EMERGENCY consideration may boost them; in emergencies only abilities from MASTERED classes are valid candidates.
- Q35: player focus x1.5 at rank V, boost only (1.0/1.1/1.2/1.35/1.5).
- Q36: everyone may loot scrolls; only mage line, mage hybrids, Dragonborn (tree OFF: Mage T1) use them.
- Q37: with IAUS on, the combat table is hidden and kept. Q38: bash stays in the ch.18 mix this release.
- Q39: FLWR v6 stays a simple one-way upgrade, stated in the release notes.
- Q40: spell cost parity must NEVER exceed the player's cost: multiplier = min(2, PCfactor(skill)/NPCfactor(skill)) per spell school skill.
- Q41: summons and runtime followers count as managed (pay the same).
- Q40 redesign (marth): charge the real player-curve cost + follower perks; animated road tops up the delta

## Level cap + strong-effect scaling (marth 2026-11-10, interactive)
- Mastery cap grows by tier: T1 10, T2 15, T3 20, T4 25 levels. (Dragonborn T5 has no passive of its own, so no cap.) The XP curve per tier is still sized from the ~3 h / 6 h / 10 h targets, so a level means more play time on higher tiers.
- The STRONG effect (T3) and the UNIQUE mechanic (T4) scale linearly with that class's level: 25% at level 1, rising each level to 100% at mastery. Mastered and unequipped, the passive keeps 1/4 of the FULL (mastered) value.
- Flat (not scaled): the T1/T2 passives and the T3 smaller effect. T4 parents' passives at full (they are mastered by definition).

## Miss budgets (marth 2026-11-10, supersedes the shared "Ranger 1 in 10" at line 85 / H5)
- Ranger: at most 3 misses in 10 shots.
- Jaqspur: at most 1 miss in 10. marth: "Id even be ok with Jaqspurs being unable to miss", so never-miss is an accepted alternative if it is the cleaner road.
- Archer: detection + correction only, no fixed budget (unchanged).

## Auto with the class add-on (marth 2026-11-10, interactive)
- "Auto is disabled with class add on." With the class add-on active, the Gambit-tab combat class Auto setting is not used; the player chooses each follower's class. The C5 34-class Auto picker (AutoClassFor / PickAutoClass / kSkillMap in classes/Mastery.cpp) is NOT wanted: remove it (cleanup card), and C10 hides/disables Auto while the add-on is active. Q42 (skill map, Arch-Mage ties) is MOOT.
- OPEN for the follow-up card: what class an un-set follower gets when the add-on first sees him (default proposal: its root T1 from today's Auto resolution, applied once, then player-owned).
- REFINED (marth 2026-11-10): "Auto is disabled per follower when classes are enabled on that follower." Classes are on/off PER FOLLOWER. A follower with classes ON has no Auto (the player picks his class). A follower with classes OFF keeps today's Gambit-tab Auto (Melee/Ranged/Mage resolution) unchanged. Proposed encoding (no layout change): MCLS equipped == kNoClass means classes OFF for that follower; equipping a class turns them ON; ClearEquipped turns them OFF. C5b + C10 follow this.

## Question batch answers 3 (marth 2026-11-10, interactive, Q56-Q60)
- Q56 miss budgets: KEEP Archer 5 / Scout 4 / Witchhunter 4 misses in 10 (Ranger 3, Jaqspur 1 as ruled). INI-tunable.
- Q57 dismissed followers: STRIP class passives on dismiss, restore on rejoin; class levels and mastery are kept. -> card C6b.
- Q58 Warlock/Nightblade Illusion bonus also strengthens ally buffs: "Its ok for release, but we'll need to figure it out for a quick patch after, especially if it makes the buffs very strong." -> ship as is; POST-RELEASE QUICK PATCH task to find a foe-only road; the Deck run must check how strong Courage/Rally/Call to Arms get.
- Q59 Gambit-tab pick vs equipped class: "Remember the gambits reset on a new class pick. Choosing a non matching gambit is not possible. But Class wins if someone manages to do it." -> class wins (C2b). The board must make a non-matching pick impossible while classes are ON (C10 + G16 strict adherence), and a class pick resets the follower's gambits to the class defaults (C12, per Q8: per-class memory restores his own edits for that class).
- Q60 Dragonborn root: not answered; provisional default stays (root of his highest-level mastered T4). Ask again with C10.
- Q60 Dragonborn (marth 2026-11-10, verbatim): "It is a pickable t4 for combat behaviour, and It uses the most perked weapon as main, but also like auto should the player want a bow for flying dragons or something."
  => Dragonborn's COMBAT BEHAVIOUR (package/CSTY/attack mix) is a player-picked T4 (any mastered T4). His MAIN weapon kind = his most-perked weapon line; but he switches situationally like Auto (e.g. a bow vs flying dragons). Storage of the picked T4 needs a new MCLS field (MCLS v2 card). Stance root for C2b = the root of his most-perked weapon line (1H/2H -> melee, archery -> ranged, a school -> mage) when no T4 is picked.

## IAUS mage melee (marth 2026-11-10, after the sim's cost-fixed run: T1 mage attack 21-42% of fired actions)
"A T1 mage should almost never melee, and even less often pure mages up the tree."
- Target for I8 (T1 Mage profile): attack is a last resort, near 0% of fired actions while any affordable cast, drink-magicka, staff or posture option exists. Sim acceptance: mage `attack=` share in the low single digits across brackets.
- Pure-mage classes higher in the tree (T2+ line Mage, not hybrids) melee even less than T1. Hybrids (one weapon class + one school) are not covered by this rule.
- Do not get there by removing the attack candidate (attacks are additive, never removed): weight it down in the class profile so a mage reaches for magicka, staff or retreat first.

## IAUS skill fit + damage estimate (marth 2026-11-10: "absolutely, skill fit and class considerations even better if we can estimate the damage and make rulings on that")
Finding: IAUS Facts (native/iaus/Core.h) carry no weapon-skill, school-skill or damage fact, so an attack and a cast score on range/target only. That is why the sim's T1 mage melees 21-42% of the time.
Ruling, for I8:
1. **Skill fit:** each candidate is scaled by the follower's skill for it (weapon skill for attacks/power attacks, the spell's school skill for casts), read from his live AVs at runtime.
2. **Class weight** on top (the mage melee ruling above).
3. **Damage estimate** where it can be proven: expected damage per second (and per magicka for casts) for each offensive candidate, against the target's resist/armor, so the rulings compare real outcomes ("the firebolt does 3x the dagger"). It must use the engine's own numbers (weapon damage with skill and perks, spell magnitude with perk entry points), resolved at runtime and never from list tables. Each engine read is verified against the fork + disassembly and observed with a passive probe before it drives scoring (principle 5). If a read cannot be proven, that candidate falls back to skill fit alone, logged, never a guess.
Sim: model the same estimate so the sim can tune it, and use the target "T1 mage attack share in low single digits".
