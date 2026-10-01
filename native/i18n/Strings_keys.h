// i18n/Strings_keys.h -- THE LIST of MFO's translatable display text. One X-macro
// line per string: MFO_STR(Id, "English default", argc). Included (with MFO_STR
// defined by the includer) by Strings.h (the K enum) and Strings.cpp (the
// defaults). NO include guard on purpose. Rules, because tools/i18n/gen_template.py
// parses this file line by line:
//   * ONE entry per line, ONE plain string literal (no adjacent-literal concatenation).
//   * `argc` is how many {1}..{argc} placeholders the template takes.
//   * A trailing `// ...` comment is the translator's hint and is copied into the
//     template. `// == Group ==` lines become group headers.
//   * The id is the stable key: it is spelled `$MFO_<Id>` in MFO_<LANGUAGE>.txt.
//     NEVER rename or renumber an id once shipped (a translator's file keys on it).
//   * Display text only. Gambit opcode strings (cond.*, act.*) are FROZEN and are
//     NOT keys: the editor shows Cond_<Op> / Act_<Op> labels, the opcode is what is saved.
//   * Do not put `{` or `}` in a default except as {N}.

// == Board window and footer ==
MFO_STR(Win_Title,          "Follower Overhaul", 0)   // the window title bar
MFO_STR(Win_Header,         "Field Orders", 0)        // the big header inside the window
MFO_STR(Ft_Picker,          "[A]/E pick   [B]/Esc back   d-pad to move", 0)   // footer while a list is open
MFO_STR(Ft_Main,            "[A]/E open list   [B]/Esc back-or-close   [LB]/[RB] follower   [View] tab   [Y] toggle line   d-pad move   -   Skin in MCM", 0)   // footer hint line

// == Words ==
MFO_STR(Word_Unset,         "(unset)", 0)   // a rule slot with nothing picked
MFO_STR(Word_Health,        "H", 0)         // short form in the vitals tooltip. Other languages: the game's own word is used if you leave this out
MFO_STR(Word_Magicka,       "M", 0)         // short form in the vitals tooltip
MFO_STR(Word_Stamina,       "S", 0)         // short form in the vitals tooltip

// == HUD strip (always on screen, no input) ==
MFO_STR(Hud_EvalNever,      "[eval: NEVER RAN]", 0)
MFO_STR(Hud_Eval,           "[eval {1} tk {2}ms]", 2)   // {1}=evaluator ticks, {2}=milliseconds per tick
MFO_STR(Hud_Session,        "| session {1} kill  {2} rap  {3}/hr", 3)   // {1}=kills, {2}=rapport earned this session, {3}=rapport per hour
MFO_STR(Hud_RankTotal,      "R{1} {2} total", 2)   // {1}=rank, {2}=lifetime rapport
MFO_STR(Hud_NoFollowers,    "no followers", 0)
MFO_STR(Hud_GlyphCombat,    "[C]", 0)   // combat marker, keep it short
MFO_STR(Hud_GlyphLooting,   "[L]", 0)   // looting marker, keep it short
MFO_STR(Hud_GlyphTrading,   "[T]", 0)   // trading marker, keep it short

// == Followers tab ==
MFO_STR(Fk_Tab,             "Followers", 0)
MFO_STR(Fk_Tracked,         "{1} tracked  (active + retained)", 1)   // {1}=how many followers
MFO_STR(Fk_ColFollower,     "Follower", 0)
MFO_STR(Fk_ColState,        "State", 0)
MFO_STR(Fk_ColRapport,      "Rapport", 0)
MFO_STR(Fk_ColRank,         "Rank", 0)
MFO_STR(Fk_ColSlots,        "Slots", 0)
MFO_STR(Fk_ColVitals,       "H/M/S", 0)   // health / magicka / stamina bars
MFO_STR(Fk_ColDist,         "Dist", 0)    // distance to you
MFO_STR(Fk_TipMfoOn,        "MFO ON -- managing this follower", 0)
MFO_STR(Fk_TipMfoOff,       "MFO OFF -- follower left vanilla (no gambits, logistics, or equip)", 0)
MFO_STR(Fk_StateRetained,   "retained", 0)
MFO_STR(Fk_StateSummon,     "summon", 0)
MFO_STR(Fk_StateCombat,     "In Combat", 0)
MFO_STR(Fk_StateLooting,    "Looting", 0)
MFO_STR(Fk_StateTrading,    "Trading", 0)
MFO_STR(Fk_StateFollowing,  "following", 0)
MFO_STR(Fk_TipVitalLine,    "{1} {2} / {3}", 3)   // one line of the vitals tooltip. {1}=H/M/S word, {2}=current, {3}=max

// == Gambits tab ==
MFO_STR(Gb_Tab,             "Gambits", 0)
MFO_STR(Gb_NoFollower,      "No active follower. Recruit one to edit gambits.", 0)
MFO_STR(Gb_Rank,            "rank {1}", 1)   // {1}=the follower's rank
MFO_STR(Gb_ClassBtn,        "Class: {1}", 1)   // {1}=Auto / Melee / Ranged / Mage
MFO_STR(Gb_SwitchHint,      "[LB]/[RB] change follower", 0)
MFO_STR(Gb_PageCombat,      "Combat", 0)
MFO_STR(Gb_PageLogistics,   "Logistics", 0)
MFO_STR(Gb_SlotsUsed,       "{1} / {2} slots used", 2)   // {1}=rules in use, {2}=slots available
MFO_STR(Gb_PickHint,        "d-pad move   [A]/E pick   [B]/Esc back", 0)   // top of every list
MFO_STR(Gb_TitleClass,      "Combat class", 0)
MFO_STR(Gb_ClassAuto,       "Auto", 0)
MFO_STR(Gb_ClassMelee,      "Melee", 0)
MFO_STR(Gb_ClassRanged,     "Ranged", 0)
MFO_STR(Gb_ClassMage,       "Mage", 0)
MFO_STR(Gb_When,            "When (target / condition)", 0)   // column header and list title
MFO_STR(Gb_Value,           "Value", 0)    // column header and list title
MFO_STR(Gb_Do,              "Do (action)", 0)   // column header and list title
MFO_STR(Gb_Spell,           "Spell", 0)    // column header and list title
MFO_STR(Gb_Target,          "Target", 0)   // column header and list title
MFO_STR(Gb_ColOn,           "On", 0)
MFO_STR(Gb_TitleFoe,        "Foe:", 0)           // the submenu row and its title
MFO_STR(Gb_TitleLootPotions,"Loot potions:", 0)  // the submenu row and its title
MFO_STR(Gb_TitleLootMisc,   "Loot misc:", 0)     // the submenu row and its title
MFO_STR(Gb_WhenRow,         "When {1}", 1)   // {1}=the condition label
MFO_STR(Gb_DoRow,           "-> {1}", 1)     // {1}=the action label
MFO_STR(Gb_FmtPct,          "{1}%", 1)       // a percent value, {1}=the number
MFO_STR(Gb_FmtDist,         "{1}u", 1)       // a distance value in game units, {1}=the number
MFO_STR(Gb_PickSpell,       "(pick spell)", 0)
MFO_STR(Gb_LastFail,        "last: {1}", 1)   // tooltip, {1}=why the last cast failed
MFO_STR(Gb_NoSpells,        "No spells known, no spellbooks carried", 0)
MFO_STR(Gb_TeachHeader,     "Teach from spellbook (consumes it):", 0)
MFO_STR(Gb_TeachArmed,      "{1}  Teach? This DESTROYS the book.", 1)   // {1}=spell name. Shown after the first click
MFO_STR(Gb_Spellbook,       "{1}  (spellbook)", 1)   // {1}=spell name
MFO_STR(Gb_MagickaCost,     "Magicka cost: {1}", 1)
MFO_STR(Gb_WhatItDoes,      "What it does:", 0)
MFO_STR(Gb_SubjAuto,        "Auto", 0)   // target column when MFO picks who
MFO_STR(Gb_SubjAutoInfer,   "Auto (infer from spell)", 0)   // in the target list
MFO_STR(Gb_SubjAlly,        "Ally: Nearest", 0)
MFO_STR(Gb_SubjPlayer,      "Player", 0)   // only if the game gives no player name
MFO_STR(Gb_SubjGone,        "(follower gone)", 0)
MFO_STR(Gb_Up,              "up", 0)       // small button, keep it short
MFO_STR(Gb_Down,            "dn", 0)       // small button, keep it short
MFO_STR(Gb_Del,             "del", 0)      // small button, keep it short
MFO_STR(Gb_DelSure,         "sure?", 0)    // the del button after one click
MFO_STR(Gb_FullRules,       "Full rules (read-only). Top wins.", 0)
MFO_STR(Gb_SumWhen,         "{1}.  When {2}", 2)       // {1}=line number, {2}=condition
MFO_STR(Gb_SumWhenVal,      "{1}.  When {2} {3}", 3)   // {1}=line number, {2}=condition, {3}=value (50% / 3 / 500u)
MFO_STR(Gb_SumCast,         "{1} ({2})", 2)   // {1}=action, {2}=spell name
MFO_STR(Gb_SumOff,          "{1}   ->   {2}   [off]", 2)   // {1}=when part, {2}=do part, for a switched-off line
MFO_STR(Gb_SumArrow,        "->", 0)
MFO_STR(Gb_AddRule,         "+ Add rule", 0)
MFO_STR(Gb_AllUsed,         "All {1} slots used. More unlock with rapport.", 1)   // {1}=slots
MFO_STR(Gb_FooterHelp,      "Highlight a slot and press [A]/E to open its list. [Y] toggles the highlighted line. Top rule wins.", 0)

// == Spell effect line (shown in the spell tooltip) ==
MFO_STR(Spell_For,          "for {1}s", 1)   // {1}=duration in seconds
MFO_STR(Spell_In,           "in {1}ft", 1)   // {1}=area in feet

// == Progression tab (the Field Orders board, hosted tab) ==
MFO_STR(Pg_NoFollower, "No active follower. Recruit one to manage progression.", 0)
MFO_STR(Pg_Level, "level {1}", 1)   // {1}=the follower's level
MFO_STR(Pg_NotEnrolled, "not enrolled", 0)
MFO_STR(Pg_ChooseTitle, "Choose a class", 0)   // list title
MFO_STR(Pg_ClassHint, "Skills auto-scale to level by class; perks stay yours to pick.", 0)
MFO_STR(Pg_NoClasses, "(no classes declared by the addon)", 0)
MFO_STR(Pg_CannotProgress, "{1} cannot progress: {2}.", 2)   // {1}=follower name, {2}=the reason (comes from the game data, may stay English)
MFO_STR(Pg_NotEnrolledMsg, "{1} is not enrolled. Pick a class to begin -- no skills are touched until you do.", 1)   // {1}=follower name
MFO_STR(Pg_ChooseBtn, "Choose class...", 0)
MFO_STR(Pg_PerkPoints, "Perk points: {1}", 1)   // {1}=points to spend
MFO_STR(Pg_NoneToSpend, "None to spend: 1 point per {1} levels -- level {2} has earned {3}, and {4} are already spent.", 4)   // {1}=levels per point, {2}=level, {3}=points earned, {4}=points spent
MFO_STR(Pg_PerPoint, "1 perk point per {1} levels. Pick a skill to open its tree.", 1)   // {1}=levels per point
MFO_STR(Pg_Syncing, "syncing follower state...", 0)
MFO_STR(Pg_Manual, "Manual skill points", 0)   // checkbox
MFO_STR(Pg_ManualTip, "Manual OVERRIDE: while ON, this follower earns {1} skill\npoints per level for YOU to place -- INSTEAD OF automatic\nclass-based skill growth, never on top of it. Toggle OFF\nto resume auto growth. For mage or multiclass builds the\nclass weights won't serve.", 1)   // tooltip, {1}=skill points per level. \n is a line break
MFO_STR(Pg_SkillPoints, "Skill points: {1}", 1)   // {1}=pooled points
MFO_STR(Pg_ManualNote, "replaces auto growth -- select a skill to apply (+1 base, cap {1})", 1)   // {1}=the skill cap
MFO_STR(Pg_ColSkill, "Skill", 0)   // column header
MFO_STR(Pg_ColLevel, "Level", 0)   // column header
MFO_STR(Pg_ColPerks, "Perks", 0)   // column header
MFO_STR(Pg_SkillsSyncing, "skill levels syncing...", 0)
MFO_STR(Pg_SkillFallback, "(skill)", 0)
MFO_STR(Pg_ToSpend, "{1} to spend", 1)   // {1}=perk points. Keep it short, it sits in a narrow column
MFO_STR(Pg_NoTree, "no tree", 0)   // keep it short, it sits in a narrow column
MFO_STR(Pg_SkillBase, "base {1} (manual +{2})  |  {3} point(s) pooled", 3)   // {1}=base level, {2}=manual points added, {3}=pooled points
MFO_STR(Pg_Apply, "Apply 1 skill point  ({1} -> {2})", 2)   // {1}=level now, {2}=level after
MFO_STR(Pg_NoPooled, "no pooled points", 0)
MFO_STR(Pg_AtCap, "at the skill cap", 0)
MFO_STR(Pg_OpenTree, "Open perk tree", 0)
MFO_STR(Pg_TreePoints, "--  {1} point(s)", 1)   // {1}=perk points to spend
MFO_STR(Pg_ShowMarginal, "Show marginal", 0)   // checkbox
MFO_STR(Pg_TreeHint, "d-pad move  [A] node  [LB]/[RB] zoom  [Y] next tree  [View] marginal  [B]/Esc back", 0)
MFO_STR(Pg_NothingUseful, "Nothing in this tree is useful to a follower -- [View] shows marginal perks.", 0)
MFO_STR(Pg_TipOwned, "{1} -- rank {2}/{3} (allocated by MFO)", 3)   // {1}=perk, {2}=rank owned, {3}=ranks in the perk
MFO_STR(Pg_TipNative, "{1} -- granted by your load order", 1)   // {1}=perk
MFO_STR(Pg_TipTake, "{1} -- [A] take rank {2} (1 point)", 2)   // {1}=perk, {2}=the rank you would take
MFO_STR(Pg_TipLocked, "{1} -- locked: {2}", 2)   // {1}=perk, {2}=why (comes from the game data, may stay English)
MFO_STR(Pg_PointsAvail, "{1} perk point(s) available", 1)   // {1}=points
MFO_STR(Pg_Marginal, "marginal for followers", 0)
MFO_STR(Pg_RankLine, "rank {1}: {2}{3}", 3)   // {1}=rank number, {2}=the skill requirement, {3}=empty or the owned note
MFO_STR(Pg_NoSkillReq, "no skill requirement", 0)
MFO_STR(Pg_Owned, "[owned]", 0)
MFO_STR(Pg_Native, "Granted by your load order -- MFO leaves it untouched.", 0)
MFO_STR(Pg_FullyAlloc, "Fully allocated ({1}/{2}).", 2)   // {1}=ranks owned, {2}=ranks in the perk
MFO_STR(Pg_Take, "Take rank {1}  (1 perk point)", 1)   // {1}=the rank you take
MFO_STR(Pg_LockedMsg, "Locked: {1}", 1)   // {1}=why (comes from the game data, may stay English)
MFO_STR(Pg_LockedPlain, "Locked.", 0)
MFO_STR(Pg_SyncingShort, "syncing...", 0)
MFO_STR(Pg_CloseHint, "[B]/Esc close", 0)
MFO_STR(Pg_Respec, "Respec", 0)   // button
MFO_STR(Pg_RespecFree, "refund all perks, free (one time)  |  d-pad move   [A] open skill tree   [B] back", 0)
MFO_STR(Pg_RespecCost, "refund all perks, -{1} rapport  |  d-pad move   [A] open skill tree   [B] back", 1)   // {1}=rapport lost
MFO_STR(Pg_RespecTitle, "Respec?", 0)   // popup title
MFO_STR(Pg_RespecBodyFree, "Every perk MFO allocated to {1} is removed and its points refunded. This one is free (one time): no rapport is lost.", 1)   // {1}=follower name
MFO_STR(Pg_RespecBodyCost, "Every perk MFO allocated to {1} is removed and its points refunded. They will resent the reset: -{2} rapport.", 2)   // {1}=follower name, {2}=rapport lost
MFO_STR(Pg_Confirm, "Confirm respec", 0)
MFO_STR(Pg_Cancel, "Cancel", 0)   // If you leave this out in a translated game, the game's own Cancel word is used

// == Gambit conditions ("When ...") ==
MFO_STR(Cond_Always, "Always", 0)
MFO_STR(Cond_SelfHpBelow, "Self: HP below", 0)
MFO_STR(Cond_SelfMpBelow, "Self: Magicka below", 0)
MFO_STR(Cond_SelfSpBelow, "Self: Stamina below", 0)
MFO_STR(Cond_PlayerHpBelow, "Player: HP below", 0)
MFO_STR(Cond_FoeLowestHp, "Foe: Lowest HP", 0)
MFO_STR(Cond_FoeHpBelow, "Foe: HP below", 0)
MFO_STR(Cond_FoeHighestHp, "Foe: Highest HP", 0)
MFO_STR(Cond_FoeHighestLevel, "Foe: Highest level", 0)
MFO_STR(Cond_FoeAny, "Foe: Nearest", 0)
MFO_STR(Cond_FoeWithinRange, "Foe: Target within", 0)
MFO_STR(Cond_FoeBeyondRange, "Foe: Target beyond", 0)
MFO_STR(Cond_FoeAttackingPlayer, "Foe: Targeting player", 0)
MFO_STR(Cond_FoeAttackingMe, "Foe: Targeting me", 0)
MFO_STR(Cond_FoeAttackingMeMelee, "Foe: Targeting me (melee)", 0)
MFO_STR(Cond_FoeAttackingMeRanged, "Foe: Targeting me (ranged)", 0)
MFO_STR(Cond_FoeIsUndead, "Foe: Undead", 0)
MFO_STR(Cond_FoeIsDragon, "Foe: Dragon", 0)
MFO_STR(Cond_FoeIsMechanical, "Foe: Mechanical", 0)
MFO_STR(Cond_FoeIsCaster, "Foe: Spellcaster", 0)
MFO_STR(Cond_FoeIsRanged, "Foe: Ranged", 0)
MFO_STR(Cond_FoeWeakerThanMe, "Foe: Weaker than me", 0)
MFO_STR(Cond_FoeStrongerThanMe, "Foe: Stronger than me", 0)
MFO_STR(Cond_FoeBlocking, "Foe: Blocking", 0)
MFO_STR(Cond_FoeFleeing, "Foe: Fleeing", 0)
MFO_STR(Cond_FoeWeakFire, "Foe: Weak to fire", 0)
MFO_STR(Cond_FoeWeakFrost, "Foe: Weak to frost", 0)
MFO_STR(Cond_FoeWeakShock, "Foe: Weak to shock", 0)
MFO_STR(Cond_FoeMultipleWithin, "Foe: Multiple within", 0)
MFO_STR(Cond_FoeCountAtLeast, "Foe: Count at least", 0)
MFO_STR(Cond_SelfHpAbove, "Self: HP above", 0)
MFO_STR(Cond_SelfMpAbove, "Self: Magicka above", 0)
MFO_STR(Cond_SelfSpAbove, "Self: Stamina above", 0)
MFO_STR(Cond_AllyHpBelow, "Ally: HP below", 0)
MFO_STR(Cond_SelfLowHealthPotion, "Health potions below", 0)
MFO_STR(Cond_SelfLowStaminaPotion, "Stamina potions below", 0)
MFO_STR(Cond_SelfLowMagickaPotion, "Magicka potions below", 0)
MFO_STR(Cond_SelfOutOfArrows, "Arrows below", 0)
MFO_STR(Cond_SelfOutOfBolts, "Bolts below", 0)
MFO_STR(Cond_IsInterior, "In an interior", 0)
MFO_STR(Cond_IsNight, "At night", 0)
MFO_STR(Cond_Dark, "In the dark", 0)
MFO_STR(Cond_SelfCarryWeightAbove, "Self: Carry weight above", 0)

// == Gambit actions ("Do ...") ==
MFO_STR(Act_Wait, "Wait", 0)
MFO_STR(Act_CastSelf, "Cast on self", 0)
MFO_STR(Act_CastTarget, "Cast on target", 0)
MFO_STR(Act_Attack, "Attack", 0)
MFO_STR(Act_DrinkHealthPotion, "Drink health potion", 0)
MFO_STR(Act_DrinkStaminaPotion, "Drink stamina potion", 0)
MFO_STR(Act_DrinkMagickaPotion, "Drink magicka potion", 0)
MFO_STR(Act_EquipRanged, "Equip ranged weapon", 0)
MFO_STR(Act_EquipMelee, "Equip melee weapon", 0)
MFO_STR(Act_PowerAttack, "Power attack", 0)
MFO_STR(Act_Flee, "Flee to player", 0)
MFO_STR(Act_LootArrows, "Loot arrows", 0)
MFO_STR(Act_LootBolts, "Loot bolts", 0)
MFO_STR(Act_LootPotions, "Loot potions (any)", 0)
MFO_STR(Act_LootHealthPotion, "Loot health potions", 0)
MFO_STR(Act_LootStaminaPotion, "Loot stamina potions", 0)
MFO_STR(Act_LootMagickaPotion, "Loot magicka potions", 0)
MFO_STR(Act_LootEquipment, "Loot better equipment", 0)
MFO_STR(Act_LootGold, "Loot gold", 0)
MFO_STR(Act_LootJewelry, "Loot jewellery", 0)
MFO_STR(Act_LootSoulGems, "Loot soul gems", 0)
MFO_STR(Act_LootLockpicks, "Loot lockpicks", 0)
MFO_STR(Act_LootIngredients, "Loot ingredients", 0)
MFO_STR(Act_LootValuables, "Loot valuables and gold (to sell)", 0)
MFO_STR(Act_LootMuseum, "Loot museum items", 0)
MFO_STR(Act_EquipTorch, "Equip torch", 0)
MFO_STR(Act_CastPlayer, "Cast on player", 0)
