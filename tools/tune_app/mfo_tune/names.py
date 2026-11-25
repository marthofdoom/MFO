"""Name tables the in-game tuning loader accepts, copied from native/iaus/Catalogue.cpp (Name(Family) / Name(Cons)) and native/classes/ClassTable.cpp
(class slugs) of the tuning-file worktree. A family / consideration token in a tuning file is an index into these or the (first) name match.
Keep in step with the game source: a stale list here makes the validator disagree with the loader."""
FAMILIES = (
    "attack", "power_attack", "taunt", "sneak", "flee", "drink_health", "drink_stamina", "drink_magicka", "equip",
    "cast_offense", "cast_control", "cast_heal", "cast_buff", "cast_summon", "cast_ward", "use_scroll", "exploit", "regroup",
    "give_ground", "block", "bash", "disengage", "back_off", "step", "drink_buff", "decast",
)
CONS = (
    "range", "range", "los", "threat", "on_me", "on_player", "on_ally", "player_target", "current_target", "finish",
    "affordable", "mp_reserve", "cooldown", "recast", "variety", "will_work", "target_cond", "already_active", "hand_free",
    "splash_safe", "exploit", "class_fit", "foe_blocking", "stamina", "foe_stronger", "taunted", "self_hp", "unseen",
    "fight_start", "self_hp", "no_heal", "disengage", "need", "stock", "no_heal", "ranged_dist", "melee_dist", "ammo",
    "staff_charge", "melee_reach", "unholy", "mundane_vs_unholy", "class_weapon", "area", "controlled", "foe_count",
    "recipient_hp", "heal_reach", "early", "in_melee", "live_summon", "foes", "caster_on_me", "self_hp", "scroll_stock",
    "mp_low", "engaged", "caster", "downed", "outnumbered", "damage", "overkill", "survive", "danger", "heal_size",
    "player_dying", "recipient_dying", "chase", "helpless", "taunt_chance", "heal_claimed", "heal_urgency", "control_worth",
    "magicka_buys", "range", "mortal", "mortal_danger", "trade", "need", "far", "need", "need", "need", "strike", "hold", "need",
    "flee_heal", "need", "guard_line", "buff_threat", "buff_stealth", "buff_foes", "perks", "consumable_worth", "spell_perks", "hand_lost", "heal_rate",
    "spot_path", "spot_sight", "spot_exposure", "spot_fire_line", "spot_range", "spot_path_exposed", "spot_exits", "spot_ally_dist", "spot_hostile_area",
    "spot_leash", "spot_climb", "spot_drop", "spot_detected", "spot_light", "spot_foe_reach", "spot_lane", "spot_portal_busy", "spot_serves", "spot_cover",
    "spot_region_trust", "spot_path_trust",
    "light_veto", "light_need", "light_rank", "self_light", "light_on_stealth", "spot_path_light",
    "hold_foes_on", "hold_contact_sec", "hold_foe_speed", "hold_foe_ranged", "hold_sight", "hold_cover", "hold_narrow", "hold_ally_front", "hold_player", "hold_kept",
)
# the hold considerations (class files: spot.hold.<cons>=<v>, v >= 0): a run of CONS
HOLD_CONS = tuple(c for c in CONS if c.startswith("hold_"))
# native/iaus/Spots.h kSpotKinds, in order (class files: spot.<kind>=<v>)
SPOT_KINDS = ("stay", "firing", "cover", "high_ground", "chokepoint", "flank", "heal_reach", "peel", "kite", "regroup", "loot_route", "retreat", "reach", "hold")
CLASS_SLUGS = (
    "melee", "ranged", "mage", "knight", "warrior", "mercenary", "archer", "thief", "evoker", "conjurer", "healer", "warlock",
    "great-knight", "champion", "blademaster", "ranger", "master-thief", "wizard", "necromancer", "priest", "spellsword",
    "sorcerer", "templar", "nightblade", "witchhunter", "scout", "arch-mage", "living-legend", "nightingale", "listener",
    "ebony-warrior", "grand-battlemage", "jaqspur", "saint", "dragonborn",
)
BASE_OFF_SLUGS = ("melee-off", "ranged-off", "mage-off")
